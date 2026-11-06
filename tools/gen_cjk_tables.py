#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate src/text/cjk_data.cpp: CID -> Unicode for the Adobe CJK collections and code -> CID for
every predefined CMap, from Adobe's cmap-resources (BSD-3-Clause, pinned commit) and the Unicode
Ideographic Variation Database (Unicode License v3, pinned version).

Downloads (urllib) each collection's cid2code.txt, every CMap file CMAPS lists and
IVD_Sequences.txt, and writes the compact tables that src/text/cjk.cpp searches (algorithms §3.4,
roadmap D2):
  - per collection (Adobe-Japan1, -GB1, -CNS1, -Korea1, -KR): CID -> Unicode, one uint16 per CID.
    The candidates of a CID, best tier first:
      0. the collection's UTF32 column of cid2code.txt;
      1. its other UTF32 and UCS2 columns (UniJIS-UCS2-HW gives the half-width CIDs);
      2. vertical ("v") entries of those columns;
      3. the base character of an Ideographic Variation Sequence registered for the CID
         (Adobe-Japan1 in the IVD: kanji glyph variants);
      4. one-byte legacy codes 0x20-0x7E read as ASCII (Roman CIDs only EUC-style CMaps reach);
      5. the character another CID has at the same code of a CMap of the same encoding family,
         restricted to the families' standard double-byte areas (FAMILIES: Big Five across the
         B5 CMaps of Adobe-CNS1, KS X 1001 across the EUC and UHC CMaps of Adobe-Korea1): the
         vendor glyph variants of a character.
    Among candidates, private-use, compatibility, radical and vertical forms lose even across
    tiers; then the best tier; then combining, fullwidth and similar forms lose; then the lowest
    code point. CIDs with a supplementary-plane code point hold 0xD800 + an index into the
    supplementary list. So that cjk.cpp can rebuild the collection's Uni*-UTF32-H CMap exactly as
    Unicode -> CID, a bit per CID says whether UTF32-H maps the CID's code point to it, and
    "extras" list the UTF32-H mappings that this inverse does not give back.
  - per predefined CMap: its codespace and notdefrange (resolved over usecmap), its WMode, a run
    list (code, count, CID) and an optional base CMap searched after it. A legacy CMap is stored as
    its difference from the CMap of its collection that makes that difference smallest (chosen
    greedily, chains at most MAX_CHAIN long), with "absent" runs (CID 0xFFFF) for codes the base
    maps and it does not. Unicode-based CMaps (Uni*-UCS2, -UTF16, -UTF32, -UTF8) store code
    points, as their difference from their usecmap or else from the collection's UTF32-H, which is
    their final base.

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
IVD_VERSION = "2022-09-13"
IVD_URL = f"https://www.unicode.org/ivd/data/{IVD_VERSION}/IVD_Sequences.txt"
IVD_NAME = f"IVD_Sequences-{IVD_VERSION}.txt"

# (ordering, directory, Unicode CMap prefix) in cjk.h Collection order (from 1).
COLLECTIONS = [
    ("Japan1", "Adobe-Japan1-7", "UniJIS"),
    ("GB1", "Adobe-GB1-6", "UniGB"),
    ("CNS1", "Adobe-CNS1-7", "UniCNS"),
    ("Korea1", "Adobe-Korea1-2", "UniKS"),
    ("KR", "Adobe-KR-9", "UniAKR"),
]
# Every CMap of each collection's directory except the CIDFont CMaps (Adobe-*) and Identity.
CMAPS = {
    "Japan1": [
        "78-EUC-H", "78-EUC-V", "78-H", "78-RKSJ-H", "78-RKSJ-V", "78-V", "78ms-RKSJ-H", "78ms-RKSJ-V",
        "83pv-RKSJ-H", "90ms-RKSJ-H", "90ms-RKSJ-V", "90msp-RKSJ-H", "90msp-RKSJ-V", "90pv-RKSJ-H",
        "90pv-RKSJ-V", "Add-H", "Add-RKSJ-H", "Add-RKSJ-V", "Add-V", "EUC-H", "EUC-V", "Ext-H", "Ext-RKSJ-H",
        "Ext-RKSJ-V", "Ext-V", "H", "Hankaku", "Hiragana", "Katakana", "NWP-H", "NWP-V", "RKSJ-H", "RKSJ-V",
        "Roman", "UniJIS-UCS2-H", "UniJIS-UCS2-HW-H", "UniJIS-UCS2-HW-V", "UniJIS-UCS2-V", "UniJIS-UTF16-H",
        "UniJIS-UTF16-V", "UniJIS-UTF32-H", "UniJIS-UTF32-V", "UniJIS-UTF8-H", "UniJIS-UTF8-V",
        "UniJIS2004-UTF16-H", "UniJIS2004-UTF16-V", "UniJIS2004-UTF32-H", "UniJIS2004-UTF32-V",
        "UniJIS2004-UTF8-H", "UniJIS2004-UTF8-V", "UniJISPro-UCS2-HW-V", "UniJISPro-UCS2-V", "UniJISPro-UTF8-V",
        "UniJISX0213-UTF32-H", "UniJISX0213-UTF32-V", "UniJISX02132004-UTF32-H", "UniJISX02132004-UTF32-V",
        "V", "WP-Symbol",
    ],
    "GB1": [
        "GB-EUC-H", "GB-EUC-V", "GB-H", "GB-V", "GBK-EUC-H", "GBK-EUC-V", "GBK2K-H", "GBK2K-V", "GBKp-EUC-H",
        "GBKp-EUC-V", "GBT-EUC-H", "GBT-EUC-V", "GBT-H", "GBT-V", "GBTpc-EUC-H", "GBTpc-EUC-V", "GBpc-EUC-H",
        "GBpc-EUC-V", "UniGB-UCS2-H", "UniGB-UCS2-V", "UniGB-UTF16-H", "UniGB-UTF16-V", "UniGB-UTF32-H",
        "UniGB-UTF32-V", "UniGB-UTF8-H", "UniGB-UTF8-V",
    ],
    "CNS1": [
        "B5-H", "B5-V", "B5pc-H", "B5pc-V", "CNS-EUC-H", "CNS-EUC-V", "CNS1-H", "CNS1-V", "CNS2-H", "CNS2-V",
        "ETHK-B5-H", "ETHK-B5-V", "ETen-B5-H", "ETen-B5-V", "ETenms-B5-H", "ETenms-B5-V", "HKdla-B5-H",
        "HKdla-B5-V", "HKdlb-B5-H", "HKdlb-B5-V", "HKgccs-B5-H", "HKgccs-B5-V", "HKm314-B5-H", "HKm314-B5-V",
        "HKm471-B5-H", "HKm471-B5-V", "HKscs-B5-H", "HKscs-B5-V", "UniCNS-UCS2-H", "UniCNS-UCS2-V",
        "UniCNS-UTF16-H", "UniCNS-UTF16-V", "UniCNS-UTF32-H", "UniCNS-UTF32-V", "UniCNS-UTF8-H", "UniCNS-UTF8-V",
    ],
    "Korea1": [
        "KSC-EUC-H", "KSC-EUC-V", "KSC-H", "KSC-Johab-H", "KSC-Johab-V", "KSC-V", "KSCms-UHC-H", "KSCms-UHC-HW-H",
        "KSCms-UHC-HW-V", "KSCms-UHC-V", "KSCpc-EUC-H", "KSCpc-EUC-V", "UniKS-UCS2-H", "UniKS-UCS2-V",
        "UniKS-UTF16-H", "UniKS-UTF16-V", "UniKS-UTF32-H", "UniKS-UTF32-V", "UniKS-UTF8-H", "UniKS-UTF8-V",
    ],
    "KR": ["UniAKR-UTF16-H", "UniAKR-UTF32-H", "UniAKR-UTF8-H"],
}
# Tier 5: legacy cid2code columns whose codes `standard` accepts name the same character: the
# standard Big Five areas (symbols A140-A3FE, level 1 A440-C67E, level 2 C940-F9D5; not the
# C6A1-C8FE and F9D6-F9FE areas ETen and the Hong Kong sets assign differently) and KS X 1001
# (both bytes A1-FE; not the UHC extension).
FAMILIES = {
    "CNS1": [(("B5", "B5pc", "ETen-B5", "ETenms-B5", "HKscs-B5", "ETHK-B5", "HKdla-B5", "HKdlb-B5", "HKgccs-B5",
               "HKm314-B5", "HKm471-B5"),
              lambda c: 0xA140 <= c <= 0xA3FE or 0xA440 <= c <= 0xC67E or 0xC940 <= c <= 0xF9D5)],
    "Korea1": [(("KSC-EUC", "KSCpc-EUC", "KSCms-UHC", "KSCms-UHC-HW"),
                lambda c: 0xA1 <= c >> 8 <= 0xFE and 0xA1 <= c & 0xFF <= 0xFE)],
}
UNICODE_FORMS = ["UCS2", "UTF16", "UTF32", "UTF8"]
# cjk.h CMapEncoding values.
ENCODING = {"legacy": 0, "UCS2": 1, "UTF16": 2, "UTF32": 3, "UTF8": 4}
# The longest base chain a legacy CMap may get (cjk.cpp kMaxChain is larger).
MAX_CHAIN = 4
ABSENT = 0xFFFF


def cid2code_name(directory):
    return f"{directory}-cid2code.txt"


def cmap_names():
    """(collection index, directory, CMap name) of every CMap the tables hold."""
    out = []
    for index, (ordering, directory, _) in enumerate(COLLECTIONS):
        out += [(index, directory, name) for name in CMAPS[ordering]]
    return out


SOURCES = {name: f"{BASE}{directory}/CMap/{name}" for _, directory, name in cmap_names()}
SOURCES.update({cid2code_name(directory): f"{BASE}{directory}/cid2code.txt" for _, directory, _ in COLLECTIONS})
SOURCES[IVD_NAME] = IVD_URL

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
    WMode (None when the file does not say)."""
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
    return (use.group(1) if use else None), codespace, mapping, notdef, int(wmode.group(1)) if wmode else None


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
    """({cid: [(tier, cp)]} for tiers 0-2 and 4, {(column, code): [cid]} of the legacy two-byte codes,
    {cid: [(column, code)]}, CID count)."""
    lines = [line for line in text.splitlines() if line and not line.startswith("#")]
    header = lines[0].split("\t")
    columns = [(i, name) for i, name in enumerate(header) if "UTF32" in name or "UCS2" in name]
    legacy = [i for i, name in enumerate(header) if i > 0 and not name.startswith("Uni")]
    out, by_code, codes = {}, {}, {}
    count = 0
    for line in lines[1:]:
        fields = line.split("\t")
        cid = int(fields[0])
        count = max(count, cid + 1)
        for i, name in columns:
            if i >= len(fields) or fields[i] == "*":
                continue
            for value in fields[i].split(","):
                vertical = value.endswith("v")
                cp = int(value.rstrip("v"), 16)
                tier = 2 if vertical else 0 if name == f"{prefix}-UTF32" else 1
                out.setdefault(cid, []).append((tier, cp))
        ascii_codes = []
        for i in legacy:
            if i >= len(fields) or fields[i] == "*":
                continue
            for value in fields[i].split(","):
                if value.endswith("v"):
                    continue
                code = int(value, 16)
                if len(value) == 2 and 0x20 <= code <= 0x7E:
                    ascii_codes.append((4, code))
                if len(value) == 4:
                    by_code.setdefault((header[i], code), []).append(cid)
                    codes.setdefault(cid, []).append((header[i], code))
        if cid not in out and ascii_codes:
            out[cid] = ascii_codes
    return out, by_code, codes, count


def parse_ivd(text, collection):
    """{cid: [base code point]} of the IVD sequences registered in `collection`."""
    out = {}
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        sequence, name, cid = (part.strip() for part in line.split(";"))
        if name != collection or not cid.startswith("CID+"):
            continue
        out.setdefault(int(cid[4:]), []).append(int(sequence.split()[0], 16))
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


def choose(options):
    usable = [o for o in options if not (0xD800 <= o[1] <= 0xDFFF) and o[1] <= 0x10FFFF and o[1] != 0]
    if not usable:
        return None
    return min(usable, key=lambda o: (penalty(o[1]) >= 4, o[0], penalty(o[1]), o[1]))[1]


def runs(pairs):
    """Sorted (code, cid) pairs as (code, count, cid) runs where both count up; ABSENT pairs form
    runs of their own whose CID stays ABSENT."""
    out = []
    for code, cid in pairs:
        if out and out[-1][0] + out[-1][1] == code and out[-1][1] < 0xFFFF and (
                (cid == ABSENT and out[-1][2] == ABSENT) or
                (cid != ABSENT and out[-1][2] != ABSENT and out[-1][2] + out[-1][1] == cid)):
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
    cmap_files = {name for _, _, name in cmap_names()}
    parsed = {name: parse_cmap(data.decode("latin-1")) for name, data in raw.items() if name in cmap_files}
    ivd_text = raw[IVD_NAME].decode("utf-8")

    def resolved(name):
        """A CMap's own mapping over its usecmap chain."""
        use, _, mapping, _, _ = parsed[name]
        out = dict(resolved(use)) if use else {}
        out.update(mapping)
        return out

    def inherited(name, field):
        """A CMap's codespace (1), notdef ranges (3) or WMode (4), from its usecmap when it has none."""
        value = parsed[name][field]
        if (value is None or value == []) and parsed[name][0]:
            return inherited(parsed[name][0], field)
        return value

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
    for ordering, directory, prefix in COLLECTIONS:
        horizontal = unicode_map(f"{prefix}-UTF32-H", "UTF32")
        utf32.append(horizontal)
        candidates, by_code, codes, count = parse_cid2code(raw[cid2code_name(directory)].decode("utf-8"), prefix)
        for cid, bases in parse_ivd(ivd_text, f"Adobe-{ordering}").items():
            if all(tier > 3 for tier, _ in candidates.get(cid, [])):
                candidates.setdefault(cid, []).extend((3, cp) for cp in bases)
        chosen = {}
        for cid, options in candidates.items():
            cp = choose(options)
            if cp is not None:
                chosen[cid] = cp
        # Tier 5, after the others: a code shared with a CID that has Unicode.
        for columns, standard in FAMILIES.get(ordering, []):
            for cid in range(count):
                if cid in chosen or cid == 0:
                    continue
                options = []
                for column, code in codes.get(cid, []):
                    if column not in columns or not standard(code):
                        continue
                    for other_column in columns:
                        for other in by_code.get((other_column, code), []):
                            if other in chosen:
                                options.append((5, chosen[other]))
                cp = choose(options)
                if cp is not None:
                    chosen[cid] = cp
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
    full = {}
    for collection, _, name in names:
        if not any(f"-{f}-" in name for f in UNICODE_FORMS):
            full[name] = resolved(name)
            lengths = {}
            for length, code in full[name]:
                lengths.setdefault(length, set()).add(code)
            seen = set()
            for length in sorted(lengths):
                if lengths[length] & seen:
                    raise SystemExit(f"gen_cjk_tables: {name} has overlapping code lengths")
                seen |= lengths[length]

    def difference(name, base):
        """Runs of `name` over `base` (None: on its own), with ABSENT runs for codes only the base has."""
        own = full[name]
        if base is None:
            return runs(sorted((code, cid) for (_, code), cid in own.items()))
        other = full[base]
        pairs = [(code, cid) for key, cid in own.items() if other.get(key) != cid for code in (key[1],)]
        pairs += [(key[1], ABSENT) for key in other if key not in own]
        return runs(sorted(pairs))

    # Legacy CMaps: greedy (Prim) choice of bases within each collection.
    base_of, depth, legacy_runs = {}, {}, {}
    for collection in range(len(COLLECTIONS)):
        pending = [n for c, _, n in names if c == collection and n in full]
        placed = []
        alone = {n: difference(n, None) for n in pending}
        best = {n: (len(alone[n]), None) for n in pending}
        while pending:
            name = min(pending, key=lambda n: (best[n][0], index_of[n]))
            pending.remove(name)
            base = best[name][1]
            base_of[name] = base
            depth[name] = 0 if base is None else depth[base] + 1
            legacy_runs[name] = alone[name] if base is None else difference(name, base)
            placed.append(name)
            if depth[name] < MAX_CHAIN:
                for other in pending:
                    cost = len(difference(other, name))
                    if cost < best[other][0]:
                        best[other] = (cost, name)

    all_runs, codespaces, entries = [], [], []
    codespace_lists, run_lists = {}, {}
    for collection, _, name in names:
        use = parsed[name][0]
        wmode = inherited(name, 4) or 0
        codespace = inherited(name, 1)
        notdef = inherited(name, 3)
        form = next((f for f in UNICODE_FORMS if f"-{f}-" in name), None)
        if form is None:
            own_runs = legacy_runs[name]
            base = index_of[base_of[name]] if base_of[name] else -1
        else:
            # Unicode-based: only where the full mapping differs from the usecmap's, else from the
            # collection's UTF32-H.
            mapping = unicode_map(name, form)
            reference = unicode_map(use, form) if use else utf32[collection]
            own_runs = runs(sorted((cp, cid) for cp, cid in mapping.items() if reference.get(cp) != cid))
            base = index_of[use] if use else -1
            if use and use not in index_of:
                raise SystemExit(f"gen_cjk_tables: {name} uses {use}, which is not generated")
        # Identical run lists (the -V CMaps of one collection in its four Unicode forms) are stored once.
        run_key = tuple(tuple(r) for r in own_runs)
        if run_key not in run_lists:
            run_lists[run_key] = len(all_runs)
            all_runs += own_runs
        run_start = run_lists[run_key]
        key = tuple(codespace)
        if key not in codespace_lists:
            codespace_lists[key] = len(codespaces)
            codespaces += codespace
        if not codespace:
            raise SystemExit(f"gen_cjk_tables: {name} has no codespace")
        notdef_entry = (0, 0, 0, 0)
        if len(notdef) > 1:
            raise SystemExit(f"gen_cjk_tables: {name} has {len(notdef)} notdef ranges")
        if notdef:
            notdef_entry = (1,) + notdef[0][1:]
        entries.append((name, collection + 1, ENCODING[form or "legacy"], wmode, base, run_start,
                        len(own_runs), codespace_lists[key], len(codespace)) + notdef_entry)
    for run in all_runs:
        if run[2] > 0xFFFF or (run[2] != ABSENT and run[2] + run[1] - 1 >= ABSENT):
            raise SystemExit("gen_cjk_tables: CID out of range")
    if len(all_runs) > 0xFFFF:
        raise SystemExit("gen_cjk_tables: too many runs")

    out = []
    out.append("// SPDX-License-Identifier: Apache-2.0")
    out.append("// Generated by tools/gen_cjk_tables.py; do not edit. Layout: src/text/cjk_tables.h.")
    out.append("//")
    out.append("// Third-party data (see NOTICE): Adobe CMap resources, adobe-type-tools/cmap-resources")
    out.append(f"// commit {COMMIT}, BSD-3-Clause,")
    out.append("// Copyright 1990-2023 Adobe (third_party/licenses/cmap-resources.txt); and the Unicode")
    out.append(f"// Ideographic Variation Database {IVD_VERSION}, {IVD_URL},")
    out.append("// Unicode License v3 (third_party/licenses/unicode.txt). Files read:")
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
