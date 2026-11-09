#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate the LaTeX math tables of src/layout/math_data.cpp from KaTeX (MIT), pinned.

Downloads (urllib) three files of KaTeX (https://github.com/KaTeX/KaTeX) at the commit of its
release v0.16.21, the KaTeX olmOCR-bench renders formulas with, checks their SHA-256 and writes:

- the math-mode command of each Unicode character KaTeX accepts (src/symbols.js `defineSymbol`
  with a backslash name, and src/macros.js single-character macros such as U+2260 -> \\neq);
- the Unicode character of each code of TeX's math fonts cmmi10 (OML), cmsy10 (OMS), cmex10 (OMX),
  msam10 and msbm10 (src/metrics/mapping.pl, which KaTeX adapted from MathJax-dev, Apache-2.0),
  with the KaTeX font family each code is drawn from (calligraphic, blackboard, delimiter size).

src/layout/math_tables.h declares the layout. The text layer gives a math font's glyph its code
as a WinAnsi guess when the font names the glyph with a TeX-only name (integraldisplay); the code
tables turn such glyphs back into their symbols (layout/math.h).

Usage:
  python3 tools/gen_math_tables.py                 # download and write src/layout/math_data.cpp
  python3 tools/gen_math_tables.py --cache DIR     # keep / reuse the downloads in DIR
  python3 tools/gen_math_tables.py --check         # exit 1 when the committed file differs
"""

import argparse
import hashlib
import os
import re
import sys
import urllib.request

KATEX_COMMIT = "923f2aa15692d06ef77051bfb018c17339ef1ab4"  # tag v0.16.21
KATEX_BASE = f"https://raw.githubusercontent.com/KaTeX/KaTeX/{KATEX_COMMIT}/"
SOURCES = {
    "symbols.js": ("src/symbols.js", "47cdc639fac0658c3a4de79a2638383911977c17f02234d6d764855cae5aaa64"),
    "macros.js": ("src/macros.js", "edec1b1ca725bf7d74f3dc910a582a112f15def63437160b15fb3c1cf21fcbd6"),
    "mapping.pl": ("src/metrics/mapping.pl", "064d5725db05baf0c663cde540deb4fb4aea1b5bfb4ebc17ed849b6260d27317"),
}

# The order of math_tables.h's TexFont enum.
TEX_FONTS = ["cmmi10", "cmsy10", "cmex10", "msam10", "msbm10"]

# math_tables.h's TexFamily values.
FAMILY_PLAIN, FAMILY_CALIGRAPHIC, FAMILY_BLACKBOARD = 0, 1, 2
FAMILY_SIZE1 = 3  # Size1..Size4 are 3..6

OUTPUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "layout", "math_data.cpp")


def fetch(name, cache):
    path, digest = SOURCES[name]
    data = None
    if cache and os.path.exists(os.path.join(cache, name)):
        with open(os.path.join(cache, name), "rb") as f:
            data = f.read()
    if data is None:
        with urllib.request.urlopen(KATEX_BASE + path, timeout=60) as response:
            data = response.read()
        if cache:
            os.makedirs(cache, exist_ok=True)
            with open(os.path.join(cache, name), "wb") as f:
                f.write(data)
    if hashlib.sha256(data).hexdigest() != digest:
        raise SystemExit(f"{name}: SHA-256 differs from the pinned {digest}")
    return data.decode("utf-8")


def js_string(literal):
    """The value of a double-quoted JavaScript string literal's body."""
    out, i = [], 0
    while i < len(literal):
        c = literal[i]
        if c == "\\" and i + 1 < len(literal):
            n = literal[i + 1]
            if n == "u":
                out.append(chr(int(literal[i + 2:i + 6], 16)))
                i += 6
                continue
            out.append(n)
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


STRING = r'"((?:[^"\\]|\\.)*)"'


def commands(symbols, macros):
    """Code point -> math command (with its backslash)."""
    chosen = {}  # cp -> (canonical, command)
    pattern = re.compile(r"defineSymbol\(\s*math\s*,\s*\w+\s*,\s*\w+\s*,\s*" + STRING + r"\s*,\s*" + STRING +
                         r"\s*(,\s*true\s*)?\)")
    for m in pattern.finditer(symbols):
        replace, name, canonical = js_string(m.group(1)), js_string(m.group(2)), m.group(3) is not None
        if len(replace) != 1 or not name.startswith("\\") or not re.fullmatch(r"\\[A-Za-z]+", name):
            continue
        cp = ord(replace)
        if cp < 0x80:
            continue  # ASCII is written by the code itself
        if cp not in chosen or (canonical and not chosen[cp][0]):
            chosen[cp] = (canonical, name)
    for m in re.finditer(r"defineMacro\(\s*" + STRING + r"\s*,\s*" + STRING + r"\s*\)", macros):
        key, value = js_string(m.group(1)), js_string(m.group(2))
        if len(key) != 1 or ord(key) < 0x80 or ord(key) in chosen:
            continue
        if re.fullmatch(r"\\[A-Za-z]+(\{[A-Za-z]\})?", value):
            chosen[ord(key)] = (True, value)
    return {cp: name for cp, (_, name) in chosen.items()}


def tex_codes(mapping):
    """Font -> [128 entries of (cp, family)] from mapping.pl; (0, 0) where a code maps nothing."""
    tables = {}
    for font in TEX_FONTS:
        m = re.search(r"\$map\{" + font + r"\}\s*=\s*\{(.*?)\n\};", mapping, re.S)
        if not m:
            raise SystemExit(f"mapping.pl: no map for {font}")
        table = [(0, 0)] * 128
        for block in re.finditer(r'"([A-Za-z0-9-]+)"\s*=>\s*\[(.*?)\n\s*\],', m.group(1), re.S):
            family_name, body = block.group(1), block.group(2)
            size = re.fullmatch(r"Size([1-4])(-Regular)?", family_name)
            if size:
                family = FAMILY_SIZE1 + int(size.group(1)) - 1
            elif family_name.startswith("Caligraphic"):
                family = FAMILY_CALIGRAPHIC
            else:
                family = FAMILY_PLAIN
            for line in body.split("\n"):
                line = line.split("#", 1)[0].strip()
                if not line:
                    continue
                e = re.fullmatch(r"(\[(0x[0-9A-Fa-f]+|\d+),\s*(0x[0-9A-Fa-f]+|\d+)\]|0x[0-9A-Fa-f]+|\d+)\s*=>\s*"
                                 r"(\[\s*(0x[0-9A-Fa-f]+)[^\]]*\]|0x[0-9A-Fa-f]+),?", line)
                if not e:
                    raise SystemExit(f"mapping.pl {font}: unread entry {line!r}")
                if e.group(2):
                    first, last = int(e.group(2), 0), int(e.group(3), 0)
                else:
                    first = last = int(e.group(1), 0)
                target = int(e.group(5) or e.group(4), 0)
                for code in range(first, last + 1):
                    if code >= 128 or table[code][0]:
                        continue  # the first mapping of a code is the symbol
                    cp = target + code - first
                    fam = family
                    # msbm10's capitals are KaTeX's AMS letters: \mathbb.
                    if font == "msbm10" and family_name == "AMS" and 0x41 <= cp <= 0x5A:
                        fam = FAMILY_BLACKBOARD
                    table[code] = (cp, fam)
        tables[font] = table
    return tables


def generate(cache):
    symbols = fetch("symbols.js", cache)
    macros = fetch("macros.js", cache)
    mapping = fetch("mapping.pl", cache)
    cmds = commands(symbols, macros)
    codes = tex_codes(mapping)
    out = []
    out.append("// SPDX-License-Identifier: Apache-2.0")
    out.append("// Generated by tools/gen_math_tables.py from KaTeX v0.16.21 (commit " + KATEX_COMMIT[:7] + ", MIT):")
    out.append("// src/symbols.js, src/macros.js and src/metrics/mapping.pl (adapted by KaTeX from MathJax-dev,")
    out.append("// Apache-2.0). Do not edit.")
    out.append('#include "layout/math_tables.h"')
    out.append("")
    out.append("namespace RayoPdf::Layout::MathTables {")
    out.append("")
    pool, entries = [], []
    offset = 0
    for cp in sorted(cmds):
        name = cmds[cp]
        entries.append(f"    {{0x{cp:04X}, {offset}, {len(name)}}},")
        pool.append(name)
        offset += len(name)
    out.append("const char kCommandNames[] =")
    line = ""
    for name in pool:
        escaped = name.replace("\\", "\\\\")
        if len(line) + len(escaped) > 96:
            out.append(f'    "{line}"')
            line = ""
        line += escaped
    out.append(f'    "{line}";')
    out.append("")
    out.append("const Command kCommands[] = {")
    out.extend(entries)
    out.append("};")
    out.append(f"const size_t kCommandCount = {len(entries)};")
    out.append("")
    out.append("const uint32_t kTexCodes[kTexFontCount][128] = {")
    for font in TEX_FONTS:
        out.append(f"    // {font}")
        out.append("    {")
        row = []
        for code, (cp, fam) in enumerate(codes[font]):
            row.append(f"0x{(fam << 24) | cp:07X}")
            if len(row) == 8:
                out.append("        " + ", ".join(row) + ",")
                row = []
        out.append("    },")
    out.append("};")
    out.append("")
    out.append("} // namespace RayoPdf::Layout::MathTables")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--cache", help="directory to keep and reuse the downloads in")
    parser.add_argument("--check", action="store_true", help="exit 1 when the committed file differs")
    args = parser.parse_args()
    text = generate(args.cache)
    if args.check:
        with open(OUTPUT, encoding="utf-8") as f:
            if f.read() != text:
                print(f"{OUTPUT} is out of date", file=sys.stderr)
                return 1
        return 0
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"wrote {OUTPUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
