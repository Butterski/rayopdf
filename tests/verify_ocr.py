#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check a built OCR add-on (rayopdf-ocr) as a black box: protocol 1 over pipes, with real models.

It starts `rayopdf-ocr --serve --models DIR` and checks the handshake (protocol, engine, the model
hashes against DIR/<tier>/models.txt), a page with a line of text drawn in a 5x7 dot font (the
text is read back), a blank page (no lines), the same request twice (the same bytes), a "lines"
request, a bad request (an error carrying its id, the server goes on) and "quit" (exit 0); then
--version, --verify, and a missing model folder (exit 3).

Usage:
  python3 tests/verify_ocr.py --binary build/ocr/rayopdf-ocr --models /opt/ocr/models [--emulator wine]
"""

from __future__ import annotations

import argparse
import difflib
import json
import os
import struct
import subprocess
import sys

# 5x7 dot font for the test line.
FONT = {
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "D": ["11100", "10010", "10001", "10001", "10001", "10010", "11100"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
}
TEXT = "RAYOPDF 2026"
DOT = 6


def draw(text: str, width: int, height: int, x: int, y: int) -> bytes:
    pixels = bytearray([255]) * (width * height)
    for ch in text:
        if ch == " ":
            x += 4 * DOT
            continue
        for row, bits in enumerate(FONT[ch]):
            for col, bit in enumerate(bits):
                if bit == "1":
                    for dy in range(DOT):
                        start = (y + row * DOT + dy) * width + x + col * DOT
                        pixels[start:start + DOT] = bytes(DOT)
        x += 6 * DOT
    return bytes(pixels)


def frame(header: dict, payload: bytes = b"") -> bytes:
    data = (json.dumps(header, separators=(",", ":")) + "\n").encode()
    return struct.pack("<I", len(data)) + data + payload


def read_frame(stream) -> tuple[bytes, dict]:
    head = stream.read(4)
    if len(head) != 4:
        raise SystemExit("verify_ocr: the add-on closed its output")
    (n,) = struct.unpack("<I", head)
    header = stream.read(n)
    parsed = json.loads(header)
    payload = stream.read(parsed.get("payload", 0)) if isinstance(parsed, dict) else b""
    return head + header + payload, parsed


def check(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"verify_ocr: FAILED: {message}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--binary", required=True)
    parser.add_argument("--models", required=True)
    parser.add_argument("--emulator")
    args = parser.parse_args()
    base = ([args.emulator] if args.emulator else []) + [args.binary]

    out = subprocess.run(base + ["--version"], capture_output=True, text=True, timeout=120)
    check(out.returncode == 0 and out.stdout.startswith("rayopdf-ocr "), f"--version: {out.stdout!r}")
    tier = out.stdout.split("(", 1)[1].split(",")[0].split()[1]

    proc = subprocess.Popen(base + ["--serve", "--models", args.models], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    _, hello = read_frame(proc.stdout)
    # Model hashes: the tier's files by name, the first pass's as "tiny/<file>", the orientation
    # classifier's as "orient/<file>" and the script pack's as "scripts/<script>/<file>"
    # (ocr-protocol.md).
    manifests = {}
    folders = [tier, "tiny", "orient"]
    if os.path.isdir(f"{args.models}/scripts"):
        folders += [f"scripts/{name}" for name in sorted(os.listdir(f"{args.models}/scripts"))]
    for folder in folders:
        try:
            with open(f"{args.models}/{folder}/models.txt", encoding="ascii") as f:
                manifests[folder] = dict((name, digest) for digest, _, name in (line.split() for line in f))
        except FileNotFoundError:
            pass
    check(hello.get("protocol") == 1 and hello.get("engine") == "pp-ocrv6" and hello.get("tier") == tier,
          f"handshake {hello}")

    def listed(name: str, digest: str) -> bool:
        folder, _, file = name.rpartition("/")
        return manifests.get(folder or tier, {}).get(file) == digest

    first_pass = "with a tiny first pass" in out.stdout
    check(all(listed(name, digest) for name, digest in hello["models"].items()) and
          "dict.txt" in hello["models"] and ("tiny/rec.ncnn.bin" in hello["models"] or
                                             "tiny/rec.ort" in hello["models"]) == first_pass,
          "handshake model hashes differ from models.txt")
    check("latin" in hello.get("scripts", []), f"scripts {hello.get('scripts')}")

    width, height = 640, 160
    page = draw(TEXT, width, height, 40, 50)
    request = {"id": 1, "op": "page", "width": width, "height": height, "channels": 1, "dpi": 300,
               "payload": len(page)}
    blank = bytes([255]) * (width * height)
    word = draw("RAYOPDF", 300, 70, 10, 14)
    proc.stdin.write(frame(request, page) + frame(dict(request, id=2), page) +
                     frame(dict(request, id=3), blank) +
                     frame({"id": 4, "op": "lines", "width": 0, "height": 0, "channels": 1, "dpi": 300,
                            "lines": [{"width": 300, "height": 70}], "payload": len(word)}, word) +
                     frame({"id": 5, "op": "dance"}) +
                     frame({"id": 6, "op": "quit", "width": 0, "height": 0, "channels": 1, "dpi": 0, "payload": 0}))
    proc.stdin.flush()
    raw1, first = read_frame(proc.stdout)
    raw2, second = read_frame(proc.stdout)
    check(first["status"] == "ok" and len(first["lines"]) == 1, f"page: {first}")
    line = first["lines"][0]
    similar = difflib.SequenceMatcher(None, line["text"], TEXT).ratio()
    check(similar >= 0.8, f"page text {line['text']!r} (expected about {TEXT!r})")
    check(len(line["chars"]) == len(line["text"]) and 0 < line["conf"] <= 1, f"page line {line}")
    x0, y0, x1, y1 = line["box"]
    right = 40 + sum(4 * DOT if ch == " " else 6 * DOT for ch in TEXT) - DOT  # the last ink column + 1
    check(x0 <= 40 and y0 <= 50 and x1 >= right and y1 >= 50 + 7 * DOT, f"box {line['box']} (ink {40}..{right})")
    check(raw2.replace(b'"id":2', b'"id":1', 1) == raw1, "the same page twice gave different bytes")
    _, third = read_frame(proc.stdout)
    check(third["id"] == 3 and third["status"] == "ok" and third["lines"] == [], f"blank page: {third}")
    _, fourth = read_frame(proc.stdout)
    check(fourth["id"] == 4 and len(fourth["lines"]) == 1 and fourth["lines"][0]["box"] == [0, 0, 300, 70] and
          difflib.SequenceMatcher(None, fourth["lines"][0]["text"], "RAYOPDF").ratio() >= 0.8, f"lines: {fourth}")
    _, fifth = read_frame(proc.stdout)
    check(fifth["id"] == 5 and fifth["status"] == "error" and fifth["error"] == "bad-request", f"bad request: {fifth}")
    _, sixth = read_frame(proc.stdout)
    check(sixth["id"] == 6 and sixth["status"] == "ok", f"quit: {sixth}")
    proc.stdin.close()
    check(proc.wait(timeout=120) == 0, "exit status after quit")

    out = subprocess.run(base + ["--verify", "--models", args.models], capture_output=True, text=True, timeout=300)
    check(out.returncode == 0 and "verified" in out.stdout, f"--verify: {out.stdout!r} {out.stderr!r}")
    out = subprocess.run(base + ["--serve", "--models", args.models + "/nowhere"], capture_output=True, timeout=120)
    check(out.returncode == 3 and out.stdout == b"", f"missing models: exit {out.returncode}")
    print(f"verify_ocr: all checks passed ({tier}: {line['text']!r}, conf {line['conf']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
