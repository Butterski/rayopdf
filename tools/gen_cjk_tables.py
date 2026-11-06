#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate src/text/cjk_data.cpp: CID -> Unicode for the Adobe CJK collections and code -> CID for
the common predefined CMaps, from Adobe's cmap-resources (BSD-3-Clause, pinned commit).

Downloads (urllib) each collection's cid2code.txt and the CMap files cmap_names() lists, and
writes the compact tables that src/text/cjk.cpp searches (algorithms §3.4, roadmap D2):
  - per collection (Adobe-Japan1, -GB1, -CNS1, -Korea1): CID -> Unicode, one uint16 per CID, from
    the Unicode columns of cid2code.txt: the collection's UTF32 column first, then its other
    UTF32 and UCS2 columns (UniJIS-UCS2-HW gives the half-width CIDs), then vertical ("v") entries,
    then one-byte codes 0x20-0x7E of the legacy columns read as ASCII (Roman CIDs only EUC-style
    CMaps reach).
    When several code points share a CID, compatibility, radical, presentation, combining,
    private-use and fullwidth forms lose to the ordinary character, then the lowest code point
    wins. CIDs with a supplementary-plane code point hold 0xD800 + an index into the supplementary
    list. So that cjk.cpp can rebuild the Uni*-UTF32-H CMap exactly as Unicode -> CID, a bit per
    CID says whether UTF32-H maps the CID's code point to it, and "extras" list the UTF32-H
    mappings that this inverse does not give back.
  - per predefined CMap: its codespace, a run list (code, count, CID) and an optional base CMap
    searched after it (a -V CMap's -H, as its usecmap says). Unicode-based CMaps (Uni*-UCS2,
    -UTF16, -UTF32, -UTF8) store their codes as code points and only where they differ from the
    collection's UTF32-H, which is their final base.

Usage:
  python3 tools/gen_cjk_tables.py                 # download and write src/text/cjk_data.cpp
  python3 tools/gen_cjk_tables.py --cache DIR     # keep / reuse the downloads in DIR
  python3 tools/gen_cjk_tables.py --check         # exit 1 when the committed file differs
"""

import argparse
import hashlib
import os
import re
import sys
import urllib.request

COMMIT = "f5cf3bca7fdfeaceb77aa82847e974f2306c20b4"
BASE = f"https://raw.githubusercontent.com/adobe-type-tools/cmap-resources/{COMMIT}/"

# (ordering, directory, Unicode CMap prefix, legacy CMaps) in cjk.h Collection order (from 1).
COLLECTIONS = [
    ("Japan1", "Adobe-Japan1-7", "UniJIS",
     ["90ms-RKSJ-H", "90ms-RKSJ-V", "90msp-RKSJ-H", "90msp-RKSJ-V", "EUC-H", "EUC-V", "H", "V"]),
    ("GB1", "Adobe-GB1-6", "UniGB",
     ["GBK-EUC-H", "GBK-EUC-V", "GBpc-EUC-H", "GBpc-EUC-V", "GB-EUC-H", "GB-EUC-V", "GB-H", "GB-V"]),
    ("CNS1", "Adobe-CNS1-7", "UniCNS",
     ["B5pc-H", "B5pc-V", "ETen-B5-H", "ETen-B5-V", "HKscs-B5-H", "HKscs-B5-V"]),
    ("Korea1", "Adobe-Korea1-2", "UniKS",
     ["KSCms-UHC-H", "KSCms-UHC-V", "KSCms-UHC-HW-H", "KSCms-UHC-HW-V", "KSC-EUC-H", "KSC-EUC-V", "KSC-H",
      "KSC-V"]),
]
UNICODE_FORMS = ["UCS2", "UTF16", "UTF32", "UTF8"]
# cjk.h CMapEncoding values.
ENCODING = {"legacy": 0, "UCS2": 1, "UTF16": 2, "UTF32": 3, "UTF8": 4}


def cid2code_name(directory):
    return f"{directory}-cid2code.txt"


def cmap_names():
    """(collection index, directory, CMap name) of every CMap the tables hold."""
    out = []
    for index, (_, directory, prefix, legacy) in enumerate(COLLECTIONS):
        names = list(legacy)
        for form in UNICODE_FORMS:
            names += [f"{prefix}-{form}-H", f"{prefix}-{form}-V"]
        if prefix == "UniJIS":
            names += ["UniJIS-UCS2-HW-H", "UniJIS-UCS2-HW-V"]
        out += [(index, directory, name) for name in names]
    return out


SOURCES = {name: f"{BASE}{directory}/CMap/{name}" for _, directory, name in cmap_names()}
SOURCES.update({cid2code_name(directory): f"{BASE}{directory}/cid2code.txt" for _, directory, _, _ in COLLECTIONS})

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUTPUT = os.path.join(ROOT, "src", "text", "cjk_data.cpp")


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


def parse_cmap(text):
    """usecmap, codespace [(bytes, low, high)], {(bytes, code): cid}, notdef [(bytes, low, high, cid)],
    WMode."""
    use = re.search(r"/(\S+)\s+usecmap", text)
    wmode = re.search(r"/WMode\s+(\d)", text)
    codespace = []
    for block in re.findall(r"begincodespacerange(.*?)endcodespacerange", text, re.S):
        for low, high in re.findall(r"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>", block):
            codespace.append((len(low) // 2, int(low, 16), int(high, 16)))
    mapping = {}
    for block in re.findall(r"begincidrange(.*?)endcidrange", text, re.S):
        for low, high, cid in re.findall(r"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>\s*(\d+)", block):
            lo, hi, cid = int(low, 16), int(high, 16), int(cid)
            for k in range(hi - lo + 1):
                mapping[(len(low) // 2, lo + k)] = cid + k
    for block in re.findall(r"begincidchar(.*?)endcidchar", text, re.S):
        for code, cid in re.findall(r"<([0-9a-fA-F]+)>\s*(\d+)", block):
            mapping[(len(code) // 2, int(code, 16))] = int(cid)
    notdef = []
    for block in re.findall(r"beginnotdefrange(.*?)endnotdefrange", text, re.S):
        for low, high, cid in re.findall(r"<([0-9a-fA-F]+)>\s*<([0-9a-fA-F]+)>\s*(\d+)", block):
            notdef.append((len(low) // 2, int(low, 16), int(high, 16), int(cid)))
    return (use.group(1) if use else None), codespace, mapping, notdef, int(wmode.group(1)) if wmode else 0


def code_point(form, length, code):
    """The code point a Unicode-based CMap's code stands for, None for an invalid one."""
    if form in ("UCS2", "UTF32"):
        return code
    if form == "UTF16":
        if length == 4:
            high, low = code >> 16, code & 0xFFFF
            if 0xD800 <= high <= 0xDBFF and 0xDC00 <= low <= 0xDFFF:
                return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00)
            return None
        return code
    data = code.to_bytes(length, "big")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    return ord(text) if len(text) == 1 else None


def parse_cid2code(text, prefix):
    """{cid: [(tier, cp)]} from the UTF32 and UCS2 columns (tier 0: the collection's UTF32 column,
    1: its other Unicode columns, 2: vertical entries of any) and, for CIDs no Unicode column
    names, from the legacy columns' one-byte codes 0x20-0x7E read as ASCII (tier 3: the
    proportional and half-width Roman CIDs that only EUC and Shift-JIS style CMaps reach)."""
    lines = [line for line in text.splitlines() if line and not line.startswith("#")]
    header = lines[0].split("\t")
    columns = [(i, name) for i, name in enumerate(header) if "UTF32" in name or "UCS2" in name]
    legacy = [i for i, name in enumerate(header) if i > 0 and not name.startswith("Uni")]
    out = {}
    for line in lines[1:]:
        fields = line.split("\t")
        cid = int(fields[0])
        for i, name in columns:
            if i >= len(fields) or fields[i] == "*":
                continue
            for value in fields[i].split(","):
                vertical = value.endswith("v")
                cp = int(value.rstrip("v"), 16)
                tier = 2 if vertical else 0 if name == f"{prefix}-UTF32" else 1
                out.setdefault(cid, []).append((tier, cp))
        if cid in out:
            continue
        for i in legacy:
            if i >= len(fields) or fields[i] == "*":
                continue
            for value in fields[i].split(","):
                if len(value) == 2 and 0x20 <= int(value, 16) <= 0x7E:
                    out.setdefault(cid, []).append((3, int(value, 16)))
    return out


def penalty(cp):
    """How poor a choice `cp` is as the text of a CID several code points share."""
    if 0xE000 <= cp <= 0xF8FF or cp >= 0xF0000:
        return 6  # private use
    if 0xF900 <= cp <= 0xFAFF or 0x2F800 <= cp <= 0x2FA1F:
        return 5  # compatibility ideographs
    if 0x2E80 <= cp <= 0x2FDF:
        return 5  # radicals
    if 0xFE10 <= cp <= 0xFE1F or 0xFE30 <= cp <= 0xFE4F:
        return 4  # vertical / compatibility presentation forms
    if 0x0300 <= cp <= 0x036F or cp == 0x3164:
        return 3  # combining marks, Hangul filler
    if 0xFF00 <= cp <= 0xFFEF or cp in (0x00A0, 0x00AD, 0x2011, 0x2329, 0x232A):
        return 2  # fullwidth forms, no-break and soft variants, deprecated angle brackets
    return 0


def runs(pairs):
    """Sorted (code, cid) pairs as (code, count, cid) runs where both count up."""
    out = []
    for code, cid in pairs:
        if out and out[-1][0] + out[-1][1] == code and out[-1][2] + out[-1][1] == cid and out[-1][1] < 0xFFFF:
            out[-1][1] += 1
        else:
            out.append([code, 1, cid])
    return out


def rows(values, fmt, indent="    ", gap=" "):
    """`values` formatted, comma-separated (`gap` after each comma), packed into lines of at most 120
    columns."""
    lines = []
    line = ""
    for v in values:
        item = fmt(v) + ","
        if line and len(indent) + len(line) + len(gap) + len(item) > 120:
            lines.append(indent + line)
            line = ""
        line = line + gap + item if line else item
    if line:
        lines.append(indent + line)
    return "\n".join(lines)


def generate(cache):
    raw = {name: fetch(name, cache) for name in SOURCES}
    digests = {name: hashlib.sha256(data).hexdigest() for name, data in raw.items()}
    parsed = {name: parse_cmap(data.decode("latin-1")) for name, data in raw.items()}

    def resolved(name):
        """A CMap's own mapping over its usecmap chain."""
        use, _, mapping, _, _ = parsed[name]
        out = dict(resolved(use)) if use else {}
        out.update(mapping)
        return out

    def unicode_map(name, form):
        out = {}
        for (length, code), cid in resolved(name).items():
            cp = code_point(form, length, code)
            if cp is not None:
                out[cp] = cid
        return out

    # Collections.
    cid_unicode = []
    supplementary = []
    extra_codes, extra_cids, inverse_bits = [], [], []
    collections = []
    utf32 = []
    for ordering, directory, prefix, _ in COLLECTIONS:
        horizontal = unicode_map(f"{prefix}-UTF32-H", "UTF32")
        utf32.append(horizontal)
        candidates = parse_cid2code(raw[cid2code_name(directory)].decode("utf-8"), prefix)
        chosen = {}
        for cid, options in candidates.items():
            usable = [o for o in options if not (0xD800 <= o[1] <= 0xDFFF) and o[1] <= 0x10FFFF and o[1] != 0]
            if usable:
                # Private-use, compatibility, radical and vertical forms lose even across tiers.
                chosen[cid] = min(usable, key=lambda o: (penalty(o[1]) >= 4, o[0], penalty(o[1]), o[1]))[1]
        for cp, cid in horizontal.items():
            if cid not in chosen:
                raise SystemExit(f"gen_cjk_tables: {ordering} CID {cid} is in UTF32-H but not in cid2code.txt")
        cid_count = max(chosen) + 1
        table_start, supp_start = len(cid_unicode), len(supplementary)
        for cid in range(cid_count):
            cp = chosen.get(cid, 0)
            if cp > 0xFFFF:
                index = len(supplementary) - supp_start
                if index >= 0x800:
                    raise SystemExit(f"gen_cjk_tables: {ordering} has too many supplementary CIDs")
                supplementary.append(cp)
                cid_unicode.append(0xD800 + index)
            else:
                if 0xD800 <= cp <= 0xDFFF:
                    raise SystemExit(f"gen_cjk_tables: {ordering} CID {cid} maps to a surrogate")
                cid_unicode.append(cp)
        extras_start, bits_start = len(extra_codes), len(inverse_bits)
        for cp in sorted(horizontal):
            cid = horizontal[cp]
            if chosen[cid] != cp:
                extra_codes.append(cp)
                extra_cids.append(cid)
        bits = [0] * ((cid_count + 7) // 8)
        for cid, cp in chosen.items():
            if horizontal.get(cp) == cid:
                bits[cid >> 3] |= 1 << (cid & 7)
        inverse_bits += bits
        collections.append((ordering, cid_count, table_start, supp_start, len(supplementary) - supp_start,
                            extras_start, len(extra_codes) - extras_start, bits_start))

    # Predefined CMaps.
    names = cmap_names()
    index_of = {name: i for i, (_, _, name) in enumerate(names)}
    all_runs, codespaces, entries = [], [], []
    for collection, _, name in names:
        use, codespace, mapping, notdef, wmode = parsed[name]
        form = next((f for f in UNICODE_FORMS if f"-{f}-" in name), None)
        if form is None:
            # Legacy: the CMap's own entries; a usecmap base is searched after them.
            lengths = {}
            for length, code in mapping:
                lengths.setdefault(length, set()).add(code)
            if len(lengths) > 1:
                ones, twos = lengths.get(1, set()), lengths.get(2, set())
                if set(lengths) - {1, 2} or (ones & twos):
                    raise SystemExit(f"gen_cjk_tables: {name} has overlapping code lengths")
            own = sorted((code, cid) for (_, code), cid in mapping.items())
            base = index_of[use] if use else -1
            if use and use not in index_of:
                raise SystemExit(f"gen_cjk_tables: {name} uses {use}, which is not generated")
        else:
            # Unicode-based: only where the full mapping differs from the collection's UTF32-H.
            full = unicode_map(name, form)
            reference = utf32[collection]
            own = sorted((cp, cid) for cp, cid in full.items() if reference.get(cp) != cid)
            base = -1
            if use:
                # The usecmap base holds the rest; drop what it already says.
                base = index_of[use]
                base_full = unicode_map(use, form)
                own = sorted((cp, cid) for cp, cid in full.items() if base_full.get(cp) != cid)
        run_start = len(all_runs)
        all_runs += runs(own)
        cs_start = len(codespaces)
        for length, low, high in codespace:
            codespaces.append((length, low, high))
        notdef_entry = (0, 0, 0, 0)
        if len(notdef) > 1:
            raise SystemExit(f"gen_cjk_tables: {name} has {len(notdef)} notdef ranges")
        if notdef:
            notdef_entry = (1,) + notdef[0][1:]
        entries.append((name, collection + 1, ENCODING[form or "legacy"], wmode, base, run_start,
                        len(all_runs) - run_start, cs_start, len(codespaces) - cs_start) + notdef_entry)
    if any(not e[8] and e[4] < 0 for e in entries):
        raise SystemExit("gen_cjk_tables: a CMap without codespace or base")
    for run in all_runs:
        if run[2] > 0xFFFF:
            raise SystemExit("gen_cjk_tables: CID out of range")

    out = []
    out.append("// SPDX-License-Identifier: Apache-2.0")
    out.append("// Generated by tools/gen_cjk_tables.py; do not edit. Layout: src/text/cjk_tables.h.")
    out.append("//")
    out.append("// Third-party data (see NOTICE): Adobe CMap resources, adobe-type-tools/cmap-resources")
    out.append(f"// commit {COMMIT}, BSD-3-Clause,")
    out.append("// Copyright 1990-2023 Adobe (third_party/licenses/cmap-resources.txt). Files read:")
    out.append(rows(sorted(SOURCES), str, "//   ").replace(",", ""))
    out.append("// SHA-256 of the inputs (in the order above):")
    for name in sorted(SOURCES):
        out.append(f"//   {digests[name]}")
    out.append("")
    out.append('#include "text/cjk_tables.h"')
    out.append("")
    out.append("namespace RayoPdf::Text::Cjk {")
    out.append("")
    out.append(f"const uint16_t kCidUnicode[{len(cid_unicode)}] = {{")
    out.append(rows(cid_unicode, str, gap=""))
    out.append("};")
    out.append("")
    out.append(f"const uint32_t kSupplementary[{len(supplementary)}] = {{")
    out.append(rows(supplementary, str, gap=""))
    out.append("};")
    out.append("")
    out.append(f"const uint32_t kExtraCodes[{len(extra_codes)}] = {{")
    out.append(rows(extra_codes, str, gap=""))
    out.append("};")
    out.append(f"const uint16_t kExtraCids[{len(extra_cids)}] = {{")
    out.append(rows(extra_cids, str, gap=""))
    out.append("};")
    out.append("")
    out.append(f"const uint8_t kInverseBits[{len(inverse_bits)}] = {{")
    out.append(rows(inverse_bits, str, gap=""))
    out.append("};")
    out.append("")
    out.append("const CollectionTable kCollections[kCollectionCount] = {")
    for c in collections:
        out.append(f'    {{"{c[0]}", {", ".join(str(v) for v in c[1:])}}},')
    out.append("};")
    out.append("")
    out.append(f"const CidRun kRuns[{len(all_runs)}] = {{")
    out.append(rows(all_runs, lambda r: f"{{{r[0]},{r[1]},{r[2]}}}", gap=""))
    out.append("};")
    out.append("")
    out.append(f"const CodespaceSpan kCodespaces[{len(codespaces)}] = {{")
    out.append(rows(codespaces, lambda c: f"{{{c[0]}, 0x{c[1]:X}, 0x{c[2]:X}}}"))
    out.append("};")
    out.append("")
    out.append(f"const uint16_t kPredefinedCount = {len(entries)};")
    out.append(f"const PredefinedTable kPredefined[{len(entries)}] = {{")
    for e in entries:
        out.append(f'    {{"{e[0]}", {e[1]}, {e[2]}, {e[3]}, {e[4]}, {e[5]}, {e[6]}, {e[7]}, {e[8]}, {e[9]}, '
                   f'0x{e[10]:X}, 0x{e[11]:X}, {e[12]}}},')
    out.append("};")
    out.append("")
    out.append("} // namespace RayoPdf::Text::Cjk")
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
                print("gen_cjk_tables: src/text/cjk_data.cpp is out of date", file=sys.stderr)
                return 1
        return 0
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(f"wrote {os.path.relpath(OUTPUT)} ({len(content)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
