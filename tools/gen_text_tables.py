#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the font and glyph-name tables of src/text/ from pinned permissive sources.

Downloads (urllib, pinned commits) the Adobe Glyph List and its ZapfDingbats list
(adobe-type-tools/agl-aglfn, BSD-3-Clause) and, from mozilla/pdf.js (Apache-2.0), the simple-font
encodings, the standard 14 fonts' widths and basic metrics, the standard font name aliases, the CFF
standard strings and charsets, the PDFDocEncoding differences and the Macintosh standard glyph order. Writes src/text/tables_data.cpp,
whose layout src/text/tables.h declares (algorithms §3.3, §3.5, roadmap D2).

Usage:
  python3 tools/gen_text_tables.py                 # download and write src/text/tables_data.cpp
  python3 tools/gen_text_tables.py --cache DIR     # keep / reuse the downloads in DIR
  python3 tools/gen_text_tables.py --check         # exit 1 when the committed file differs
"""

import argparse
import hashlib
import os
import re
import sys
import urllib.request

AGL_COMMIT = "4036a9ca80a62f64f9de4f7321a9a045ad0ecfd6"
AGL_BASE = f"https://raw.githubusercontent.com/adobe-type-tools/agl-aglfn/{AGL_COMMIT}/"
PDFJS_COMMIT = "2b183c9754b421cd0bff36e25206db02b903bdf3"
PDFJS_BASE = f"https://raw.githubusercontent.com/mozilla/pdf.js/{PDFJS_COMMIT}/"

SOURCES = {
    "glyphlist.txt": AGL_BASE + "glyphlist.txt",
    "zapfdingbats.txt": AGL_BASE + "zapfdingbats.txt",
    "encodings.js": PDFJS_BASE + "src/core/encodings.js",
    "metrics.js": PDFJS_BASE + "src/core/metrics.js",
    "standard_fonts.js": PDFJS_BASE + "src/core/standard_fonts.js",
    "cff_parser.js": PDFJS_BASE + "src/core/cff_parser.js",
    "charsets.js": PDFJS_BASE + "src/core/charsets.js",
    "string_utils.js": PDFJS_BASE + "src/core/string_utils.js",
    "fonts_utils.js": PDFJS_BASE + "src/core/fonts_utils.js",
}

# The order of tables.h's Encoding enum.
ENCODINGS = [
    ("StandardEncoding", "Standard"),
    ("MacRomanEncoding", "MacRoman"),
    ("WinAnsiEncoding", "WinAnsi"),
    ("MacExpertEncoding", "MacExpert"),
    ("SymbolSetEncoding", "Symbol"),
    ("ZapfDingbatsEncoding", "ZapfDingbats"),
    ("ExpertEncoding", "CffExpert"),
]

# The order of tables.h's standard 14 indices.
STANDARD14 = [
    "Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique",
    "Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Helvetica-BoldOblique",
    "Times-Roman", "Times-Bold", "Times-Italic", "Times-BoldItalic",
    "Symbol", "ZapfDingbats",
]

OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "text", "tables_data.cpp")


def fetch(name, cache):
    if cache:
        path = os.path.join(cache, name)
        if os.path.exists(path):
            with open(path, "rb") as f:
                return f.read()
    with urllib.request.urlopen(SOURCES[name], timeout=60) as response:
        data = response.read()
    if cache:
        os.makedirs(cache, exist_ok=True)
        with open(os.path.join(cache, name), "wb") as f:
            f.write(data)
    return data


def js_array(text, name):
    match = re.search(r"const " + re.escape(name) + r" = \[(.*?)\];", text, re.S)
    if not match:
        raise SystemExit(f"gen_text_tables: no array {name}")
    body = re.sub(r"//[^\n]*", "", match.group(1))
    return re.findall(r'"([^"]*)"', body)


def js_key(token):
    token = token.strip()
    if token.startswith("["):
        return token[2:-2]
    return token[1:]


def parse_glyph_list(text):
    out = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        name, values = line.split(";")
        out[name] = [int(v, 16) for v in values.split()]
    return out


def parse_metrics(text):
    widths = {}
    basic = {}
    main, rest = text.split("const getFontBasicMetrics", 1)
    for match in re.finditer(r"\n  t(\.\w+|\[\"[^\"]+\"\]) = (\d+);", main):
        widths[js_key(match.group(1))] = int(match.group(2))
    for match in re.finditer(r"\n  t(\.\w+|\[\"[^\"]+\"\]) = getLookupTableFactory\(function \(t\) \{(.*?)\n  \}\);",
                             main, re.S):
        table = {}
        for entry in re.finditer(r"t(\.\w+|\[\"[^\"]+\"\]) = (\d+);", match.group(2)):
            table[js_key(entry.group(1))] = int(entry.group(2))
        widths[js_key(match.group(1))] = table
    for match in re.finditer(r"\n  t(\.\w+|\[\"[^\"]+\"\]) = \{(.*?)\};", rest, re.S):
        fields = dict(re.findall(r"(\w+): (-?\d+|Math\.NaN)", match.group(2)))
        basic[js_key(match.group(1))] = fields
    return widths, basic


def parse_std_font_map(text):
    match = re.search(r"const getStdFontMap = getLookupTableFactory\(function \(t\) \{(.*?)\n\}\);", text, re.S)
    out = {}
    for entry in re.finditer(r"t(\.\w+|\[\"[^\"]+\"\]) = \"([^\"]+)\";", match.group(1)):
        out[js_key(entry.group(1))] = entry.group(2)
    return out


def parse_pdfdoc(text):
    match = re.search(r"const PDFStringTranslateTable = \[(.*?)\];", text, re.S)
    values = [int(v, 0) for v in re.findall(r"0x[0-9a-fA-F]+|\d+", match.group(1))]
    table = list(range(256))
    for i, v in enumerate(values):
        if v:
            table[i] = v
    # ISO 32000-1 Annex D.3: these bytes are undefined in PDFDocEncoding.
    for undefined in (0x7F, 0x9F, 0xAD):
        table[undefined] = 0
    return table


def rows(values, fmt, indent="    "):
    """`values` formatted, comma-separated, packed into lines of at most 120 columns."""
    lines = []
    line = ""
    for v in values:
        item = fmt(v) + ","
        if line and len(indent) + len(line) + 1 + len(item) > 120:
            lines.append(indent + line)
            line = ""
        line = line + " " + item if line else item
    if line:
        lines.append(indent + line)
    return "\n".join(lines)


def cstring(s):
    out = []
    for ch in s:
        if ch in '"\\':
            out.append("\\" + ch)
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            raise SystemExit(f"gen_text_tables: non-ASCII name {s!r}")
    return "".join(out)


def generate(cache):
    raw = {name: fetch(name, cache) for name in SOURCES}
    digests = {name: hashlib.sha256(data).hexdigest() for name, data in raw.items()}
    text = {name: data.decode("utf-8") for name, data in raw.items()}

    agl = parse_glyph_list(text["glyphlist.txt"])
    zapf = parse_glyph_list(text["zapfdingbats.txt"])
    encodings = {js: js_array(text["encodings.js"], js) for js, _ in ENCODINGS}
    widths, basic = parse_metrics(text["metrics.js"])
    aliases = parse_std_font_map(text["standard_fonts.js"])
    cff_strings = js_array(text["cff_parser.js"], "CFFStandardStrings")
    expert_charset = js_array(text["charsets.js"], "ExpertCharset")
    expert_subset_charset = js_array(text["charsets.js"], "ExpertSubsetCharset")
    pdfdoc = parse_pdfdoc(text["string_utils.js"])
    mac_glyphs = js_array(text["fonts_utils.js"], "MacStandardGlyphOrdering")

    if len(mac_glyphs) != 258:
        raise SystemExit(f"gen_text_tables: {len(mac_glyphs)} Mac standard glyph names, expected 258")
    if len(cff_strings) != 391:
        raise SystemExit(f"gen_text_tables: {len(cff_strings)} CFF standard strings, expected 391")
    for js, enc in encodings.items():
        if len(enc) != 256:
            raise SystemExit(f"gen_text_tables: {js} has {len(enc)} entries")
    for font in STANDARD14:
        if font not in widths or font not in basic:
            raise SystemExit(f"gen_text_tables: no metrics for {font}")

    names = set(agl) | set(zapf) | set(cff_strings) | set(expert_charset) | set(expert_subset_charset) | set(mac_glyphs)
    for enc in encodings.values():
        names.update(n for n in enc if n)
    for table in widths.values():
        if isinstance(table, dict):
            names.update(table)
    names = sorted(names, key=lambda n: n.encode())
    index = {n: i for i, n in enumerate(names)}
    blob = "".join(names)
    if len(blob) >= 65536 or len(names) >= 0xFFFF:
        raise SystemExit("gen_text_tables: name table too large for uint16 offsets")

    # AGL values: one BMP code point, or kAglMultiBase + i for the i-th multi-code-point entry.
    multi = []
    agl_values = []
    for n in names:
        cps = agl.get(n)
        if not cps:
            agl_values.append(0)
        elif len(cps) == 1:
            if cps[0] > 0xFFFF or 0xD800 <= cps[0] <= 0xDFFF:
                raise SystemExit(f"gen_text_tables: AGL {n} outside the BMP")
            agl_values.append(cps[0])
        else:
            agl_values.append(0xD800 + len(multi))
            multi.append(cps)
    multi_flat = []
    multi_starts = []
    for cps in multi:
        multi_starts.append(len(multi_flat))
        multi_flat.extend(cps)
    multi_starts.append(len(multi_flat))

    zapf_pairs = sorted((index[n], cps[0]) for n, cps in zapf.items())

    # Standard 14 widths: one sorted (name, width) table per distinct width set.
    width_tables = []
    width_ref = {}
    fonts = []
    for font in STANDARD14:
        table = widths[font]
        fixed = 0
        start = count = 0
        if isinstance(table, int):
            fixed = table
        else:
            pairs = tuple(sorted((index[n], w) for n, w in table.items()))
            if pairs not in width_ref:
                width_ref[pairs] = sum(len(p) for p in width_tables)
                width_tables.append(pairs)
            start, count = width_ref[pairs], len(pairs)
        b = basic[font]
        ascent = int(b["ascent"]) if b["ascent"] != "Math.NaN" else 0
        descent = int(b["descent"]) if b["descent"] != "Math.NaN" else 0
        fonts.append((font, fixed, start, count, ascent, descent))
    width_flat = [p for table in width_tables for p in table]

    alias_items = sorted(((a, STANDARD14.index(s)) for a, s in aliases.items()), key=lambda x: x[0].encode())

    out = []
    out.append("// SPDX-License-Identifier: Apache-2.0")
    out.append("// Generated by tools/gen_text_tables.py; do not edit. Layout: src/text/tables.h.")
    out.append("//")
    out.append("// Third-party data (see NOTICE):")
    out.append("// - Adobe Glyph List glyphlist.txt and zapfdingbats.txt, adobe-type-tools/agl-aglfn")
    out.append(f"//   commit {AGL_COMMIT}, BSD-3-Clause, Copyright 2002-2019 Adobe.")
    out.append(f"// - mozilla/pdf.js commit {PDFJS_COMMIT}, Apache-2.0, Copyright Mozilla Foundation:")
    out.append("//   src/core/encodings.js (encodings), src/core/metrics.js (standard 14 widths and ascent/descent),")
    out.append("//   src/core/standard_fonts.js (getStdFontMap aliases), src/core/cff_parser.js (CFFStandardStrings),")
    out.append("//   src/core/charsets.js (Expert charsets), src/core/string_utils.js (PDFStringTranslateTable),")
    out.append("//   src/core/fonts_utils.js (MacStandardGlyphOrdering).")
    out.append("// SHA-256 of the inputs:")
    for name in SOURCES:
        out.append(f"//   {name} {digests[name]}")
    out.append("")
    out.append('#include "text/tables.h"')
    out.append("")
    out.append("namespace RayoPdf::Text::Tables {")
    out.append("")
    out.append(f"const uint16_t kGlyphNameCount = {len(names)};")
    out.append("")
    out.append("// Every glyph name the tables use, sorted by bytes, concatenated.")
    out.append("const char kGlyphNameBytes[] =")
    line = ""
    for n in names:
        piece = cstring(n)
        if len(line) + len(piece) > 100:
            out.append(f'    "{line}"')
            line = ""
        line += piece
    out.append(f'    "{line}";')
    out.append("")
    offsets = []
    at = 0
    for n in names:
        offsets.append(at)
        at += len(n)
    offsets.append(at)
    out.append("// Name i spans [kGlyphNameOffsets[i], kGlyphNameOffsets[i + 1]) of kGlyphNameBytes.")
    out.append(f"const uint16_t kGlyphNameOffsets[{len(offsets)}] = {{")
    out.append(rows(offsets, str))
    out.append("};")
    out.append("")
    out.append("// The AGL value of name i: 0 none, a BMP code point, or kAglMultiBase + j for kAglMulti entry j.")
    out.append(f"const uint16_t kAglValues[{len(agl_values)}] = {{")
    out.append(rows(agl_values, lambda v: f"0x{v:X}"))
    out.append("};")
    out.append("")
    out.append(f"const uint16_t kAglMultiCount = {len(multi)};")
    out.append(f"const uint16_t kAglMultiStarts[{len(multi_starts)}] = {{")
    out.append(rows(multi_starts, str))
    out.append("};")
    out.append(f"const uint16_t kAglMultiCodePoints[{max(1, len(multi_flat))}] = {{")
    out.append(rows(multi_flat, lambda v: f"0x{v:X}"))
    out.append("};")
    out.append("")
    out.append("// zapfdingbats.txt: (name, code point) by name index.")
    out.append(f"const uint16_t kZapfCount = {len(zapf_pairs)};")
    out.append(f"const uint16_t kZapfNames[{len(zapf_pairs)}] = {{")
    out.append(rows([p[0] for p in zapf_pairs], str))
    out.append("};")
    out.append(f"const uint16_t kZapfValues[{len(zapf_pairs)}] = {{")
    out.append(rows([p[1] for p in zapf_pairs], lambda v: f"0x{v:X}"))
    out.append("};")
    out.append("")
    out.append("// Simple-font encodings in the order of Encoding: the name index of each code, kNoName for none.")
    out.append(f"const uint16_t kEncodings[{len(ENCODINGS)}][256] = {{")
    for js, label in ENCODINGS:
        out.append(f"    {{ // {label}")
        out.append(rows([index[n] if n else 0xFFFF for n in encodings[js]], str, "        "))
        out.append("    },")
    out.append("};")
    out.append("")
    out.append("// PDFDocEncoding (ISO 32000-1 Annex D.3): the code point of each byte, 0 for undefined.")
    out.append("const uint16_t kPdfDocEncoding[256] = {")
    out.append(rows(pdfdoc, lambda v: f"0x{v:X}"))
    out.append("};")
    out.append("")
    out.append("// The CFF standard strings (CFF spec Appendix A) as name indices.")
    out.append("const uint16_t kCffStandardStrings[391] = {")
    out.append(rows([index[n] for n in cff_strings], str))
    out.append("};")
    out.append("")
    out.append("// The 258 standard Macintosh glyph names of TrueType 'post' formats 1 and 2, as name indices.")
    out.append("const uint16_t kMacGlyphNames[258] = {")
    out.append(rows([index[n] for n in mac_glyphs], str))
    out.append("};")
    out.append("")
    out.append("// The predefined CFF charsets Expert and ExpertSubset (glyph i's name index).")
    out.append(f"const uint16_t kCffExpertCharsetCount = {len(expert_charset)};")
    out.append(f"const uint16_t kCffExpertCharset[{len(expert_charset)}] = {{")
    out.append(rows([index[n] for n in expert_charset], str))
    out.append("};")
    out.append(f"const uint16_t kCffExpertSubsetCharsetCount = {len(expert_subset_charset)};")
    out.append(f"const uint16_t kCffExpertSubsetCharset[{len(expert_subset_charset)}] = {{")
    out.append(rows([index[n] for n in expert_subset_charset], str))
    out.append("};")
    out.append("")
    out.append("// Standard 14 widths: (name index, width) runs sorted by name index.")
    out.append(f"const uint16_t kStandardWidthNames[{len(width_flat)}] = {{")
    out.append(rows([p[0] for p in width_flat], str))
    out.append("};")
    out.append(f"const uint16_t kStandardWidthValues[{len(width_flat)}] = {{")
    out.append(rows([p[1] for p in width_flat], str))
    out.append("};")
    out.append("")
    out.append("const StandardFont kStandardFonts[14] = {")
    for font, fixed, start, count, ascent, descent in fonts:
        out.append(f'    {{"{font}", {fixed}, {start}, {count}, {ascent}, {descent}}},')
    out.append("};")
    out.append("")
    out.append("// pdf.js getStdFontMap: font names (spaces removed, ',' and '_' read as '-') that are standard 14 fonts.")
    out.append(f"const uint16_t kStandardAliasCount = {len(alias_items)};")
    out.append(f"const StandardAlias kStandardAliases[{len(alias_items)}] = {{")
    for alias, font in alias_items:
        out.append(f'    {{"{cstring(alias)}", {font}}},')
    out.append("};")
    out.append("")
    out.append("} // namespace RayoPdf::Text::Tables")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", help="directory to keep and reuse the downloaded sources")
    parser.add_argument("--check", action="store_true", help="compare with the committed file instead of writing")
    args = parser.parse_args()
    content = generate(args.cache)
    if args.check:
        with open(OUTPUT, encoding="utf-8") as f:
            if f.read() != content:
                print("gen_text_tables: src/text/tables_data.cpp is out of date", file=sys.stderr)
                return 1
        return 0
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(f"wrote {os.path.relpath(OUTPUT)} ({len(content)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
