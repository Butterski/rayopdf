#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Black-box checks of a built rayopdf binary: arguments, exit codes, output files, --inspect,
--report and encrypted input. It builds the PDFs it needs in a temporary directory (none are
committed) and never times anything.

Usage:
  python3 tests/verify_cli.py --binary build/release/rayopdf
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

PAD = bytes.fromhex("28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A")


def build_pdf(content: bytes, trailer_extra: bytes = b"", encrypt=None) -> bytes:
    """A one-page PDF showing `content` in Helvetica. `encrypt(num, data)` encrypts the content
    stream's data when given."""
    data = zlib.compress(content)
    if encrypt:
        data = encrypt(4, data)
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >>"
        b" /Contents 4 0 R >>",
        b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(data) + data + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    ]
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    out += b"trailer\n<< /Size %d /Root 1 0 R " % (len(objects) + 1) + trailer_extra
    out += b" >>\nstartxref\n%d\n%%%%EOF\n" % xref
    return bytes(out)


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) % 256
        s[i], s[j] = s[j], s[i]
    i = j = 0
    out = bytearray()
    for byte in data:
        i = (i + 1) % 256
        j = (j + s[i]) % 256
        s[i], s[j] = s[j], s[i]
        out.append(byte ^ s[(s[i] + s[j]) % 256])
    return bytes(out)


def encrypted_pdf(content: bytes, user: bytes, owner: bytes) -> bytes:
    """Standard security handler, V 1 R 2, 40-bit RC4 (ISO 32000-1 Algorithms 1, 2, 3, 4)."""
    doc_id = bytes(range(16))
    permissions = -4
    # Algorithm 3: /O.
    owner_key = hashlib.md5((owner + PAD)[:32]).digest()[:5]
    o_value = rc4(owner_key, (user + PAD)[:32])
    # Algorithm 2: the file key.
    md5 = hashlib.md5((user + PAD)[:32] + o_value + (permissions & 0xFFFFFFFF).to_bytes(4, "little") + doc_id)
    key = md5.digest()[:5]
    # Algorithm 4: /U.
    u_value = rc4(key, PAD)

    def encrypt(num: int, data: bytes) -> bytes:
        object_key = hashlib.md5(key + num.to_bytes(3, "little") + (0).to_bytes(2, "little")).digest()[:10]
        return rc4(object_key, data)

    trailer = (b"/Encrypt << /Filter /Standard /V 1 /R 2 /O <" + o_value.hex().encode() + b"> /U <" +
               u_value.hex().encode() + b"> /P %d >> /ID [<" % permissions + doc_id.hex().encode() +
               b"> <" + doc_id.hex().encode() + b">]")
    return build_pdf(content, trailer, encrypt)


class Checker:
    def __init__(self, binary: str, work: Path) -> None:
        self.binary = binary
        self.work = work
        self.failures: list[str] = []
        self.count = 0

    def run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([self.binary, *args], capture_output=True, timeout=60)

    def check(self, name: str, condition: bool, detail: str = "") -> None:
        self.count += 1
        if condition:
            print(f"PASS {name}")
        else:
            print(f"FAIL {name} {detail}")
            self.failures.append(name)

    def write(self, name: str, data: bytes) -> str:
        path = self.work / name
        path.write_bytes(data)
        return str(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--binary", required=True)
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        c = Checker(binary, work)
        hello = c.write("hello.pdf", build_pdf(b"BT /F1 12 Tf 72 720 Td (Hello World) Tj ET"))
        out = str(work / "hello.md")

        r = c.run("--version")
        c.check("version", r.returncode == 0 and r.stdout.startswith(b"rayopdf "), repr(r.stdout))
        r = c.run("--help")
        c.check("help", r.returncode == 0 and b"--convert" in r.stdout)
        c.check("no arguments", c.run().returncode == 2)
        c.check("unknown argument", c.run("--frobnicate").returncode == 2)
        c.check("convert without output", c.run("--convert", hello).returncode == 2)
        c.check("bad page range", c.run("--convert", hello, out, "--pages=3-1").returncode == 2)
        c.check("two modes", c.run("--convert", hello, out, "--inspect", hello).returncode == 2)

        c.check("missing input", c.run("--convert", str(work / "missing.pdf"), out).returncode == 3)
        c.check("directory input", c.run("--convert", tmp, out).returncode == 3)
        not_pdf = c.write("text.pdf", b"hello, this is not a PDF\n" * 20)
        c.check("not a PDF", c.run("--convert", not_pdf, out).returncode == 10)

        r = c.run("--convert", hello, out)
        text = Path(out).read_text(encoding="utf-8") if Path(out).is_file() else ""
        c.check("convert", r.returncode == 0 and "Hello World" in text, f"{r.returncode} {text!r} {r.stderr!r}")
        r = c.run("--convert", hello, "-")
        c.check("convert to stdout", r.returncode == 0 and b"Hello World" in r.stdout)
        r = c.run("--convert", hello, str(work / "no-such-dir" / "out.md"))
        c.check("unwritable output", r.returncode == 12, str(r.returncode))
        r = c.run("--convert", hello, out, "--pages=2")
        c.check("page range past the end", r.returncode == 0 and "Hello" not in Path(out).read_text(encoding="utf-8"))

        r = c.run("--inspect", hello)
        try:
            info = json.loads(r.stdout)
        except ValueError:
            info = {}
        c.check("inspect", r.returncode == 0 and info.get("pages") == 1 and info.get("encrypted") is False,
                repr(r.stdout))

        r = c.run("--convert", hello, out, "--report")
        try:
            report = json.loads(r.stdout)
        except ValueError:
            report = {}
        c.check("report", r.returncode == 0 and report.get("pages") == 1 and report.get("glyphs", 0) >= 10,
                repr(r.stdout))

        locked = c.write("locked.pdf", encrypted_pdf(b"BT /F1 12 Tf 72 720 Td (Secret Text) Tj ET", b"user", b"owner"))
        c.check("password required", c.run("--convert", locked, out).returncode == 11)
        c.check("wrong password", c.run("--convert", locked, out, "--password=nope").returncode == 11)
        r = c.run("--convert", locked, out, "--password=user")
        c.check("user password", r.returncode == 0 and "Secret Text" in Path(out).read_text(encoding="utf-8"),
                f"{r.returncode} {r.stderr!r}")
        r = c.run("--convert", locked, out, "--password=owner")
        c.check("owner password", r.returncode == 0 and "Secret Text" in Path(out).read_text(encoding="utf-8"),
                f"{r.returncode} {r.stderr!r}")
        odd = c.write("odd.pdf", build_pdf(b"BT ET", b"/Encrypt << /Filter /SomeVendorHandler /V 2 /R 3 >>"))
        c.check("unsupported encryption", c.run("--convert", odd, out).returncode == 13)

        print(f"{c.count - len(c.failures)}/{c.count} checks passed")
        return 1 if c.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
