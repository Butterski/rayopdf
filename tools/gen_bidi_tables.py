#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate src/text/bidi_data.cpp: UAX #9 bidi classes, mirrored and paired brackets from the UCD.

Downloads (urllib) extracted/DerivedBidiClass.txt, BidiMirroring.txt, BidiBrackets.txt and
BidiCharacterTest.txt of the pinned Unicode Character Database version and writes the compact
tables src/text/bidi.cpp searches: one sorted array of class ranges (each entry is start << 5 |
class and runs to the next entry's start), the sorted Bidi_Mirroring_Glyph pairs and the
Bidi_Paired_Bracket pairs with their type (algorithms §4.4). The Unicode License v3 text that the
generated files reproduce is read from third_party/licenses/unicode.txt.

It also writes tests/bidi_character_test.h, a deterministic sample of BidiCharacterTest.txt that
tests/test_text_bidi.cpp checks ResolveLevels and LogicalToVisual against (the test binary also
reads a whole BidiCharacterTest.txt or BidiTest.txt given on its command line).

Usage:
  python3 tools/gen_bidi_tables.py                 # download and write the two files
  python3 tools/gen_bidi_tables.py --cache DIR     # keep / reuse the downloads in DIR
  python3 tools/gen_bidi_tables.py --check         # exit 1 when a committed file differs
"""

import argparse
import hashlib
import os
import re
import sys
import urllib.request

UCD_VERSION = "16.0.0"
UCD_BASE = f"https://www.unicode.org/Public/{UCD_VERSION}/ucd/"
SOURCES = {
    "DerivedBidiClass.txt": UCD_BASE + "extracted/DerivedBidiClass.txt",
    "BidiMirroring.txt": UCD_BASE + "BidiMirroring.txt",
    "BidiBrackets.txt": UCD_BASE + "BidiBrackets.txt",
    "BidiCharacterTest.txt": UCD_BASE + "BidiCharacterTest.txt",
}
# tests/bidi_character_test.h: the test cases of the file's first TEST_HEAD lines (the UAX #9
# examples and the cases of the 8.0 and 14.0 changes) and every TEST_STRIDE-th case after them.
TEST_HEAD = 400
TEST_STRIDE = 64

# The order of bidi.h's Class enum.
CLASSES = ["L", "R", "AL", "EN", "ES", "ET", "AN", "CS", "NSM", "BN", "B", "S", "WS", "ON",
           "LRE", "LRO", "RLE", "RLO", "PDF", "LRI", "RLI", "FSI", "PDI"]
LONG_NAMES = {
    "Left_To_Right": "L", "Right_To_Left": "R", "Arabic_Letter": "AL", "European_Number": "EN",
    "European_Separator": "ES", "European_Terminator": "ET", "Arabic_Number": "AN",
    "Common_Separator": "CS", "Nonspacing_Mark": "NSM", "Boundary_Neutral": "BN",
    "Paragraph_Separator": "B", "Segment_Separator": "S", "White_Space": "WS", "Other_Neutral": "ON",
}

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
OUTPUT = os.path.join(ROOT, "src", "text", "bidi_data.cpp")
TEST_OUTPUT = os.path.join(ROOT, "tests", "bidi_character_test.h")
LICENSE = os.path.join(ROOT, "third_party", "licenses", "unicode.txt")


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


def parse_range(field):
    field = field.strip()
    if ".." in field:
        low, high = field.split("..")
        return int(low, 16), int(high, 16)
    return int(field, 16), int(field, 16)


def parse_classes(text):
    """The class of every code point: the @missing defaults first, then the listed values."""
    classes = bytearray([CLASSES.index("L")]) * 0x110000
    for line in text.splitlines():
        match = re.match(r"#\s*@missing:\s*([0-9A-F.]+)\s*;\s*(\w+)", line)
        if match:
            low, high = parse_range(match.group(1))
            value = CLASSES.index(LONG_NAMES[match.group(2)])
            classes[low:high + 1] = bytes([value]) * (high - low + 1)
    for line in text.splitlines():
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        cps, value = (part.strip() for part in body.split(";"))
        if value not in CLASSES:
            raise SystemExit(f"gen_bidi_tables: unknown class {value!r}")
        low, high = parse_range(cps)
        classes[low:high + 1] = bytes([CLASSES.index(value)]) * (high - low + 1)
    return classes


def parse_mirroring(text):
    pairs = []
    for line in text.splitlines():
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        a, b = (int(part.strip(), 16) for part in body.split(";"))
        pairs.append((a, b))
    pairs.sort()
    return pairs


def parse_brackets(text):
    """(code point, Bidi_Paired_Bracket, is opening) sorted by code point."""
    out = []
    for line in text.splitlines():
        body = line.split("#", 1)[0].strip()
        if not body:
            continue
        cp, pair, kind = (part.strip() for part in body.split(";"))
        if kind not in ("o", "c"):
            raise SystemExit(f"gen_bidi_tables: bracket type {kind!r}")
        out.append((int(cp, 16), int(pair, 16), kind == "o"))
    out.sort()
    return out


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


def generate(cache):
    raw = {name: fetch(name, cache) for name in SOURCES}
    digests = {name: hashlib.sha256(data).hexdigest() for name, data in raw.items()}
    classes = parse_classes(raw["DerivedBidiClass.txt"].decode("utf-8"))
    pairs = parse_mirroring(raw["BidiMirroring.txt"].decode("utf-8"))
    brackets = parse_brackets(raw["BidiBrackets.txt"].decode("utf-8"))
    with open(LICENSE, encoding="utf-8") as f:
        license_text = f.read().rstrip("\n").splitlines()

    ranges = []
    for cp in range(0x110000):
        if cp == 0 or classes[cp] != classes[cp - 1]:
            ranges.append(cp << 5 | classes[cp])
    if len(pairs) > 0xFFFF or len(ranges) > 0xFFFF:
        raise SystemExit("gen_bidi_tables: table too large")

    out = []
    out.append("// SPDX-License-Identifier: Apache-2.0")
    out.append("// Generated by tools/gen_bidi_tables.py; do not edit. Layout: src/text/bidi_tables.h.")
    out.append("//")
    out.append(f"// Third-party data (see NOTICE): the Unicode Character Database {UCD_VERSION},")
    out.append(f"// {UCD_BASE}extracted/DerivedBidiClass.txt and")
    out.append(f"// {UCD_BASE}BidiMirroring.txt and BidiBrackets.txt, under the Unicode License v3:")
    out.append("//")
    for line in license_text:
        out.append(("// " + line).rstrip())
    out.append("//")
    out.append("// SHA-256 of the inputs:")
    for name in SOURCES:
        out.append(f"//   {name} {digests[name]}")
    out.append("")
    out.append('#include "text/bidi_tables.h"')
    out.append("")
    out.append("namespace RayoPdf::Text::Bidi {")
    out.append("")
    out.append(f"const uint16_t kClassRangeCount = {len(ranges)};")
    out.append(f"const uint32_t kClassRanges[{len(ranges)}] = {{")
    out.append(rows(ranges, lambda v: f"0x{v:X}"))
    out.append("};")
    out.append("")
    out.append("const uint8_t kAsciiClasses[128] = {")
    out.append(rows([classes[cp] for cp in range(128)], str))
    out.append("};")
    out.append("")
    out.append(f"const uint16_t kMirrorCount = {len(pairs)};")
    out.append(f"const uint32_t kMirrorPairs[{len(pairs)}][2] = {{")
    out.append(rows(pairs, lambda p: f"{{0x{p[0]:X}, 0x{p[1]:X}}}"))
    out.append("};")
    out.append("")
    out.append(f"const uint16_t kBracketCount = {len(brackets)};")
    out.append(f"const uint32_t kBrackets[{len(brackets)}][2] = {{")
    out.append(rows(brackets, lambda b: f"{{0x{b[0]:X}, 0x{b[1] | (b[2] << 24):X}}}"))
    out.append("};")
    out.append("")
    out.append("} // namespace RayoPdf::Text::Bidi")
    return "\n".join(out) + "\n"


def generate_tests(cache):
    data = fetch("BidiCharacterTest.txt", cache)
    digest = hashlib.sha256(data).hexdigest()
    cases = []
    for number, line in enumerate(data.decode("utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        if number <= TEST_HEAD or number % TEST_STRIDE == 0:
            if '"' in line or "\\" in line:
                raise SystemExit("gen_bidi_tables: unexpected character in a test line")
            cases.append((number, line))
    with open(LICENSE, encoding="utf-8") as f:
        license_text = f.read().rstrip("\n").splitlines()
    out = []
    out.append("// SPDX-License-Identifier: Apache-2.0")
    out.append("// Generated by `python3 tools/gen_bidi_tables.py`; do not edit. Read by tests/test_text_bidi.cpp.")
    out.append("//")
    out.append(f"// Third-party data (see NOTICE): {len(cases)} test cases (the lines up to {TEST_HEAD} and every")
    out.append(f"// {TEST_STRIDE}th line after) of the Unicode Character Database {UCD_VERSION}")
    out.append(f"// {UCD_BASE}BidiCharacterTest.txt, under the Unicode License v3:")
    out.append("//")
    for line in license_text:
        out.append(("// " + line).rstrip())
    out.append("//")
    out.append(f"// SHA-256 of the input: {digest}")
    out.append("// Each case: code points; paragraph direction (0 LTR, 1 RTL, 2 auto); resolved paragraph level;")
    out.append("// resolved levels (x: removed by X9); visual order. The number is the source line.")
    out.append("#pragma once")
    out.append("")
    out.append("namespace RayoPdf::Test {")
    out.append("")
    out.append("struct BidiCharacterCase {")
    out.append("    int line;")
    out.append("    const char* text;")
    out.append("};")
    out.append("")
    out.append(f"constexpr int kBidiCharacterCaseCount = {len(cases)};")
    out.append("constexpr BidiCharacterCase kBidiCharacterCases[kBidiCharacterCaseCount] = {")
    for number, line in cases:
        item = f'    {{{number}, "{line}"}},'
        if len(item) <= 120:
            out.append(item)
            continue
        # Long lines split into adjacent string literals.
        head = f"    {{{number}, "
        first = 120 - len(head) - 2
        # The first piece leaves room for the closing "}," should no second piece be needed.
        first = min(first, len(line) - 1)
        pieces = [line[:first]] + [line[k:k + 110] for k in range(first, len(line), 110)]
        out.append(head + '"' + pieces[0] + '"')
        for k, piece in enumerate(pieces[1:], 1):
            out.append('        "' + piece + '"' + ("}," if k == len(pieces) - 1 else ""))
    out.append("};")
    out.append("")
    out.append("} // namespace RayoPdf::Test")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", help="directory to keep and reuse the downloaded sources")
    parser.add_argument("--check", action="store_true", help="compare with the committed files instead of writing")
    args = parser.parse_args()
    outputs = [(OUTPUT, generate(args.cache)), (TEST_OUTPUT, generate_tests(args.cache))]
    if args.check:
        stale = False
        for path, content in outputs:
            with open(path, encoding="utf-8") as f:
                if f.read() != content:
                    print(f"gen_bidi_tables: {os.path.relpath(path)} is out of date", file=sys.stderr)
                    stale = True
        return 1 if stale else 0
    for path, content in outputs:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"wrote {os.path.relpath(path)} ({len(content)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
