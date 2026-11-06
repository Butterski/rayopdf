#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate src/text/bidi_data.cpp: UAX #9 bidi classes and mirrored pairs from the UCD.

Downloads (urllib) extracted/DerivedBidiClass.txt and BidiMirroring.txt of the pinned Unicode
Character Database version and writes the compact tables src/text/bidi.cpp searches: one sorted
array of class ranges (each entry is start << 5 | class and runs to the next entry's start) and the
sorted Bidi_Mirroring_Glyph pairs (algorithms §4.4). The Unicode License v3 text that the generated
file reproduces is read from third_party/licenses/unicode.txt.

Usage:
  python3 tools/gen_bidi_tables.py                 # download and write src/text/bidi_data.cpp
  python3 tools/gen_bidi_tables.py --cache DIR     # keep / reuse the downloads in DIR
  python3 tools/gen_bidi_tables.py --check         # exit 1 when the committed file differs
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
}

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
    out.append(f"// {UCD_BASE}BidiMirroring.txt, under the Unicode License v3:")
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
    out.append("} // namespace RayoPdf::Text::Bidi")
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
                print("gen_bidi_tables: src/text/bidi_data.cpp is out of date", file=sys.stderr)
                return 1
        return 0
    with open(OUTPUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(f"wrote {os.path.relpath(OUTPUT)} ({len(content)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
