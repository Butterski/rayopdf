#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Black-box checks of a built rayopdf binary: arguments, exit codes, output files, --inspect,
--report, encrypted input, --convert with several page workers (the same Markdown and record as
one), --batch (records, exit codes, subfolders, skipping, the same files and records with 1 and 4
workers) and --serve --json (several requests, malformed ones among them, answers in request order
with 1 and 4 workers), and the output options (--format=json, --format=chunks, --chunk-chars,
--format=inspect, --format=text, --format=html, --page-markers, --front-matter, --furniture,
--plain, --tables). It builds the PDFs it needs in a temporary directory (none are committed) and
never times anything.

Usage:
  python3 tests/verify_cli.py --binary build/release/rayopdf [--emulator wine]
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


def build_pdf(content: bytes, trailer_extra: bytes = b"", encrypt=None, image: bool = False) -> bytes:
    """A one-page PDF showing `content` in Helvetica. `encrypt(num, data)` encrypts the content
    stream's data when given. `image` adds a 64x64 RGB image XObject /Im1 to the resources."""
    data = zlib.compress(content)
    if encrypt:
        data = encrypt(4, data)
    xobjects = b" /XObject << /Im1 6 0 R >>" if image else b""
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >>" + xobjects +
        b" >> /Contents 4 0 R >>",
        b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(data) + data + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
    ]
    if image:
        pixels = zlib.compress(bytes((x * 4, y * 4, 128)[c] for y in range(64) for x in range(64) for c in range(3)))
        objects.append(b"<< /Type /XObject /Subtype /Image /Width 64 /Height 64 /ColorSpace /DeviceRGB"
                       b" /BitsPerComponent 8 /Filter /FlateDecode /Length %d >>\nstream\n" % len(pixels) +
                       pixels + b"\nendstream")
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
    def __init__(self, binary: str, work: Path, emulator: str | None = None) -> None:
        self.command = [emulator, binary] if emulator else [binary]
        self.work = work
        self.failures: list[str] = []
        self.count = 0

    def run(self, *args: str, stdin: bytes | None = None, cwd: Path | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([*self.command, *args], capture_output=True, timeout=300, input=stdin, cwd=cwd)

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


def records(text: bytes) -> list[dict]:
    out = []
    for line in text.decode("utf-8", "replace").splitlines():
        try:
            out.append(json.loads(line))
        except ValueError:
            out.append({"unparsed": line})
    return out


def without_timing(items: list[dict], replace: tuple[str, str] = ("", "")) -> list[dict]:
    """Records with their wall time dropped and one path prefix renamed, for comparing runs."""
    out = []
    for item in items:
        item = {k: v for k, v in item.items() if k != "ms"}
        for key in ("input", "output", "error", "markdown"):
            if isinstance(item.get(key), str) and replace[0]:
                item[key] = item[key].replace(replace[0], replace[1])
        out.append(item)
    return out


def tree(root: Path) -> dict[str, bytes]:
    return {str(p.relative_to(root)): p.read_bytes() for p in sorted(root.rglob("*")) if p.is_file()}


def check_batch(c: Checker, work: Path, picture: str) -> None:
    source = work / "batch-in"
    (source / "sub" / "deeper").mkdir(parents=True)
    (source / ".hidden").mkdir()
    # Documents of different lengths, so that 4 workers finish them out of order.
    for i in range(12):
        lines = b" ".join(b"0 -14 Td (Line %d of document %d) Tj" % (n, i) for n in range(1 + 7 * (i % 4)))
        (source / f"doc{i:02d}.pdf").write_bytes(build_pdf(b"BT /F1 12 Tf 72 720 Td " + lines + b" ET"))
    (source / "sub" / "nested.pdf").write_bytes(build_pdf(b"BT /F1 12 Tf 72 720 Td (Nested text) Tj ET"))
    (source / "sub" / "deeper" / "deep.PDF").write_bytes(build_pdf(b"BT /F1 12 Tf 72 720 Td (Deep text) Tj ET"))
    (source / ".hidden" / "skip.pdf").write_bytes(build_pdf(b"BT /F1 12 Tf 72 720 Td (Hidden) Tj ET"))
    (source / "picture.pdf").write_bytes(Path(picture).read_bytes())
    (source / "notes.txt").write_bytes(b"not a PDF name\n")
    (source / "zz-broken.pdf").write_bytes(b"hello, this is not a PDF\n" * 20)
    (source / "locked.pdf").write_bytes(encrypted_pdf(b"BT /F1 12 Tf 72 720 Td (Batch Secret) Tj ET", b"user", b"o"))

    runs = {}
    for workers in (1, 4):
        target = work / f"batch-out-{workers}"
        r = c.run("--batch", str(source), str(target), "--recursive", f"--workers={workers}",
                  "--report=" + str(work / f"batch-{workers}.jsonl"))
        report = records((work / f"batch-{workers}.jsonl").read_bytes()) if (work / f"batch-{workers}.jsonl").is_file() \
            else []
        runs[workers] = (r, target, report)
    r, target, report = runs[1]
    # Sorted by path: doc00..doc11, locked, picture, sub/deeper/deep.PDF, sub/nested, zz-broken.
    names = [Path(item.get("input", "")).relative_to(source).as_posix() for item in report]
    expected = [f"doc{i:02d}.pdf" for i in range(12)] + ["locked.pdf", "picture.pdf", "sub/deeper/deep.PDF",
                                                         "sub/nested.pdf", "zz-broken.pdf"]
    c.check("batch records in input order", names == expected, repr(names))
    statuses = {n: (item.get("status"), item.get("code")) for n, item in zip(names, report)}
    c.check("batch statuses", statuses.get("doc00.pdf") == ("ok", 0) and statuses.get("locked.pdf") == ("error", 11)
            and statuses.get("zz-broken.pdf") == ("error", 10) and statuses.get("sub/nested.pdf") == ("ok", 0),
            repr(statuses))
    c.check("batch exit code is the last failure's", r.returncode == 10, f"{r.returncode} {r.stderr!r}")
    c.check("batch errors on stderr", b"locked.pdf needs a password" in r.stderr and b"failed for 2 of 17" in r.stderr,
            repr(r.stderr))
    text = (target / "sub" / "deeper" / "deep.md").read_text(encoding="utf-8") \
        if (target / "sub" / "deeper" / "deep.md").is_file() else ""
    c.check("batch mirrors subfolders", "Deep text" in text and (target / "doc11.md").is_file() and
            not (target / ".hidden").exists() and not (target / "notes.md").exists(), repr(text))
    text = (target / "picture.md").read_text(encoding="utf-8") if (target / "picture.md").is_file() else ""
    c.check("batch images beside each document", "![](picture-images/p1-1.png)" in text and
            (target / "picture-images" / "p1-1.png").is_file(), repr(text))
    ok = [item for item in report if item.get("status") == "ok"]
    c.check("batch record counters", all(item.get("pages") == 1 and "glyphs" in item and "ms" in item and
                                         item.get("bytes", 0) > 0 for item in ok) and len(ok) == 15, repr(report[:2]))
    r4, target4, report4 = runs[4]
    c.check("batch: 4 workers write the same files as 1", tree(target) == tree(target4) and len(tree(target)) == 16,
            f"{sorted(tree(target))} {sorted(tree(target4))}")
    c.check("batch: 4 workers write the same records as 1", r4.returncode == r.returncode and
            without_timing(report, (str(target), "OUT")) == without_timing(report4, (str(target4), "OUT")))

    r = c.run("--batch", str(source), str(target), "--skip-unchanged", "--password=user", "--report")
    report = records(r.stdout)
    statuses = {Path(item.get("input", "x")).name: item.get("status") for item in report}
    c.check("batch skip-unchanged and password", r.returncode == 10 and statuses.get("doc03.pdf") == "skipped" and
            statuses.get("locked.pdf") == "ok" and len(report) == 15 and "Batch Secret" in
            (target / "locked.md").read_text(encoding="utf-8"), repr(statuses))

    c.check("batch of a missing folder", c.run("--batch", str(work / "nowhere"), str(work / "o")).returncode == 3)
    c.check("batch into an impossible folder",
            c.run("--batch", str(source), str(work / "no" / "such" / "out")).returncode == 12)
    c.check("batch with an image directory", c.run("--batch", str(source), str(target), "--images=x").returncode == 2)
    c.check("batch with 0 workers", c.run("--batch", str(source), str(target), "--workers=0").returncode == 2)
    c.check("batch with 65 workers", c.run("--batch", str(source), str(target), "--workers=65").returncode == 2)
    c.check("batch without its folders", c.run("--batch", str(source)).returncode == 2)
    clash = work / "batch-clash"
    clash.mkdir()
    (clash / "a.pdf").write_bytes(build_pdf(b"BT /F1 12 Tf 72 720 Td (Lower) Tj ET"))
    (clash / "a.PDF").write_bytes(build_pdf(b"BT /F1 12 Tf 72 720 Td (Upper) Tj ET"))
    if len(list(clash.iterdir())) == 2:  # a case-insensitive file system holds one
        r = c.run("--batch", str(clash), str(work / "clash-out"), "--report")
        report = records(r.stdout)
        c.check("batch refuses two inputs with one output", r.returncode == 12 and len(report) == 2 and
                report[0].get("status") == "ok" and report[1].get("status") == "error" and
                "Upper" in (work / "clash-out" / "a.md").read_text(encoding="utf-8"), repr(report))


def check_serve(c: Checker, work: Path, hello: str, locked: str, picture: str) -> None:
    out = work / "serve-out"
    out.mkdir()
    requests = [
        json.dumps({"id": "1", "input": hello, "output": str(out / "hello.md")}),
        json.dumps({"id": 2, "input": hello}),
        "{not json",
        json.dumps({"id": "4", "input": hello, "markdown": "x"}),
        "",
        json.dumps({"id": "5", "input": str(work / "missing.pdf"), "output": str(out / "m.md")}),
        json.dumps({"id": "6", "input": locked, "output": str(out / "locked.md")}),
        json.dumps({"id": "7", "input": locked, "output": str(out / "locked.md"), "args": ["--password=user"]}),
        json.dumps({"id": "8", "input": hello, "output": str(out / "x.md"), "args": ["--workers=2"]}),
        json.dumps({"id": "9", "input": hello, "args": ["--pages=0"]}),
        json.dumps({"id": "10", "input": picture, "output": str(out / "pic.md")}),
        json.dumps({"id": "11", "input": picture}),
        json.dumps({"input": hello, "output": str(out / "no-id.md"), "args": ["--pages=2"]}),
        "x" * ((1 << 20) + 10),
        json.dumps({"id": "14", "input": hello, "id2": 1}),
    ]
    stdin = ("\n".join(requests) + "\nquit\n" + json.dumps({"id": "after", "input": hello}) + "\n").encode()
    runs = {}
    for workers in (1, 4):
        args = ["--serve", "--json"] + ([f"--workers={workers}"] if workers > 1 else [])
        runs[workers] = c.run(*args, stdin=stdin)
    r = runs[1]
    lines = records(r.stdout)
    c.check("serve hello line", r.returncode == 0 and bool(lines) and lines[0].get("protocol") == 1 and
            "rayopdf" in lines[0], repr(r.stdout[:200]))
    answers = lines[1:]
    c.check("serve answers every request, in order, and stops at quit", len(answers) == 14 and
            [a.get("id") for a in answers] == ["1", 2, None, "4", "5", "6", "7", "8", "9", "10", "11", None, None, "14"],
            repr([a.get("id") for a in answers]))
    if len(answers) == 14:
        codes = [(a.get("status"), a.get("code")) for a in answers]
        c.check("serve codes", codes == [("ok", 0), ("ok", 0), ("error", 2), ("error", 2), ("error", 3), ("error", 11),
                                         ("ok", 0), ("error", 2), ("error", 2), ("ok", 0), ("ok", 0), ("ok", 0),
                                         ("error", 2), ("error", 2)], repr(codes))
        c.check("serve writes the output file", "Hello World" in (out / "hello.md").read_text(encoding="utf-8") and
                answers[0].get("bytes") == (out / "hello.md").stat().st_size)
        c.check("serve answers with the Markdown", "Hello World" in answers[1].get("markdown", "") and
                answers[1].get("output") == "-")
        c.check("serve request args", "Batch Secret" not in (out / "locked.md").read_text(encoding="utf-8") and
                "Secret Text" in (out / "locked.md").read_text(encoding="utf-8") and
                "Hello" not in (out / "no-id.md").read_text(encoding="utf-8"))
        c.check("serve images beside the output, none inline",
                (out / "pic-images" / "p1-1.png").is_file() and "![" not in answers[10].get("markdown", "x"))
        c.check("serve too long a line", "exceeds" in answers[12].get("error", ""))
    c.check("serve: 4 workers answer as 1", runs[4].returncode == 0 and
            without_timing(records(runs[4].stdout)) == without_timing(lines), repr(runs[4].stdout[:300]))
    c.check("serve without --json", c.run("--serve").returncode == 2)
    c.check("serve with --report", c.run("--serve", "--json", "--report").returncode == 2)
    c.check("serve with an image directory", c.run("--serve", "--json", "--images=dir").returncode == 2)
    r = c.run("--serve", "--json", stdin=b"")
    c.check("serve of an empty input", r.returncode == 0 and len(records(r.stdout)) == 1)


def build_pages_pdf(contents: list[bytes]) -> bytes:
    """A PDF of one page per item of `contents`, every page showing its content in one shared
    Helvetica."""
    count = len(contents)
    font = 3 + 2 * count
    kids = b" ".join(b"%d 0 R" % (3 + 2 * i) for i in range(count))
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>", b"<< /Type /Pages /Kids [" + kids + b"] /Count %d >>" % count]
    for i, content in enumerate(contents):
        data = zlib.compress(content)
        objects.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 %d 0 R >> >>"
                       b" /Contents %d 0 R >>" % (font, 4 + 2 * i))
        objects.append(b"<< /Length %d /Filter /FlateDecode >>\nstream\n" % len(data) + data + b"\nendstream")
    objects.append(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offsets = []
    for i, body in enumerate(objects, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objects) + 1, xref)
    return bytes(out)


def check_page_workers(c: Checker, work: Path) -> None:
    """--convert reads the pages of a document on several threads: the same Markdown and record for
    every --workers, and by default."""
    contents = []
    for page in range(7):
        lines = b" ".join(b"0 -14 Td (Line %d of page %d) Tj" % (n, page + 1) for n in range(3 + 5 * (page % 3)))
        contents.append(b"BT /F1 16 Tf 72 740 Td (Page %d heading) Tj ET BT /F1 11 Tf 72 700 Td " % (page + 1) +
                        lines + b" ET")
    pages = c.write("pages.pdf", build_pages_pdf(contents))
    results = {}
    for workers in ("1", "2", "4", "8", None):
        out = work / f"pages-{workers or 'default'}.md"
        r = c.run("--convert", pages, str(out), "--report", *([f"--workers={workers}"] if workers else []))
        results[workers] = (r.returncode, out.read_bytes() if out.is_file() else b"",
                            without_timing(records(r.stdout), (str(out), "OUT")))
    c.check("convert: 1 worker", results["1"][0] == 0 and b"Line 12 of page 6" in results["1"][1] and
            results["1"][2][0].get("pages") == 7, repr(results["1"][2]))
    for workers in ("2", "4", "8", None):
        c.check(f"convert: {workers or 'default'} workers as 1", results[workers] == results["1"],
                repr(results[workers][2]))
    r = c.run("--bench", pages, str(work / "pages-bench"), "2", "--workers=4")
    c.check("bench with workers", r.returncode == 0 and b"pages=7" in r.stdout, repr(r.stdout))


def check_output_features(c: Checker, work: Path, hello: str) -> None:
    """--format=json (one object, schema 1, every block's Markdown in the Markdown output in order;
    --json-detail=lines|words adds each block's lines and words and changes nothing else; --batch
    writes NAME.json, a serve answer carries "json"), --page-markers (one marker a page),
    --front-matter (YAML first, with the page count) and --furniture, --plain, --tables, --quotes,
    --small-caps and --figure-text (taken by --convert and in serve requests; a bad value is a usage error)."""
    pages = str(work / "pages.pdf")
    markdown = c.run("--convert", pages, "-").stdout.decode("utf-8")
    r = c.run("--convert", pages, str(work / "pages.json"), "--format=json")
    try:
        document = json.loads((work / "pages.json").read_bytes())
    except (OSError, ValueError):
        document = {}
    blocks = document.get("blocks") or []
    c.check("json output", r.returncode == 0 and document.get("schema") == 1 and len(document.get("pages", [])) == 7
            and bool(blocks) and all("type" in b and "page" in b and "boxes" in b for b in blocks), repr(document)[:300])
    at, ordered = 0, True
    for block in blocks:
        found = markdown.find(block.get("markdown", "\0"), at)
        ordered = ordered and found >= 0
        at = found + len(block.get("markdown", "")) if found >= 0 else at
    c.check("json blocks' Markdown in the Markdown's order", bool(blocks) and ordered)
    r = c.run("--convert", pages, "-", "--format=json", "--json-detail=words")
    try:
        detailed = json.loads(r.stdout).get("blocks") or []
    except ValueError:
        detailed = []
    texts = [b for b in detailed if b.get("type") not in ("comment", "image")]
    c.check("json detail: lines and words", r.returncode == 0 and bool(texts) and all(
        b.get("lines") and all(line.get("page") and len(line.get("box", [])) == 4 and line.get("text") and
                               line.get("words") for line in b["lines"]) for b in texts) and
        [{k: v for k, v in b.items() if k != "lines"} for b in detailed] == blocks, repr(detailed)[:300])
    r = c.run("--convert", pages, "-", "--format=json", "--json-detail=lines")
    c.check("json detail: lines without words", r.returncode == 0 and b'"lines":[{' in r.stdout and
            b'"words"' not in r.stdout)
    c.check("bad json detail", c.run("--convert", pages, "-", "--format=json", "--json-detail=glyphs").returncode == 2)
    c.check("bad format", c.run("--convert", pages, "-", "--format=pdf").returncode == 2)
    r = c.run("--convert", pages, "-", "--page-markers")
    marked = r.stdout.decode("utf-8")
    markers = [n for n in range(1, 8) if f"<!-- page {n} -->" in marked]
    positions = [marked.find(f"<!-- page {n} -->") for n in markers]
    c.check("page markers", r.returncode == 0 and markers == list(range(1, 8)) and positions == sorted(positions) and
            marked.count("<!-- page ") == 7, repr(marked[:300]))
    r = c.run("--convert", pages, "-", "--front-matter")
    front = r.stdout.decode("utf-8")
    c.check("front matter", r.returncode == 0 and front.startswith("---\n") and "\npages: 7\n---\n\n" in front and
            front.endswith(markdown), repr(front[:200]))
    r = c.run("--convert", pages, "-", "--format=json", "--page-markers", "--front-matter")
    c.check("json ignores markers and front matter", r.returncode == 0 and r.stdout == (work / "pages.json").read_bytes())
    for flag in ("--furniture=keep", "--furniture=drop", "--plain", "--tables=html", "--tables=auto", "--tables=pipe",
                 "--tables=pipe-latex",
                 "--quotes=ascii", "--quotes=keep", "--small-caps=keep", "--small-caps=lower", "--figure-text=drop",
                 "--figure-text=keep"):
        r = c.run("--convert", pages, "-", flag)
        c.check(f"output option {flag}", r.returncode == 0 and b"page" in r.stdout.lower(), repr(r.stderr[:200]))
    for flag in ("--furniture=hide", "--tables=gfm", "--plain=yes", "--quotes=straight", "--small-caps=upper",
                 "--figure-text=hide"):
        c.check(f"bad output option {flag}", c.run("--convert", pages, "-", flag).returncode == 2)
    requests = (json.dumps({"id": 1, "input": hello, "args": ["--plain", "--tables=html", "--furniture=keep",
                                                               "--quotes=ascii", "--small-caps=keep",
                                                               "--figure-text=drop"]}) +
                "\n" + json.dumps({"id": 2, "input": hello, "args": ["--tables=gfm"]}) + "\n").encode()
    lines = records(c.run("--serve", "--json", stdin=requests).stdout)
    c.check("serve takes the output options", len(lines) == 3 and "Hello World" in lines[1].get("markdown", "") and
            lines[2].get("code") == 2 and "--tables" in lines[2].get("error", ""), repr(lines)[:300])

    source = work / "json-in"
    source.mkdir()
    (source / "hello.pdf").write_bytes(Path(hello).read_bytes())
    r = c.run("--batch", str(source), str(work / "json-out"), "--format=json")
    try:
        batch = json.loads((work / "json-out" / "hello.json").read_bytes())
    except (OSError, ValueError):
        batch = {}
    c.check("batch writes NAME.json", r.returncode == 0 and batch.get("schema") == 1 and
            not (work / "json-out" / "hello.md").exists(), repr(batch)[:200])
    request = json.dumps({"id": 1, "input": hello, "args": ["--format=json"]}).encode() + b"\n"
    lines = records(c.run("--serve", "--json", stdin=request).stdout)
    answer = lines[1] if len(lines) > 1 else {}
    c.check("serve answers with the JSON", isinstance(answer.get("json"), dict) and answer["json"].get("schema") == 1
            and "markdown" not in answer and any("Hello World" in b.get("markdown", "")
                                                 for b in answer["json"].get("blocks", [])), repr(answer)[:300])


def check_chunks(c: Checker, work: Path, hello: str) -> None:
    """--format=chunks: JSON Lines whose unsplit blocks' Markdown, joined, is the Markdown output and
    whose block indices run in order; --chunk-chars takes 256 to 1000000 (else a usage error);
    --batch writes NAME.jsonl; a serve answer carries the array "chunks"."""
    pages = str(work / "pages.pdf")
    markdown = c.run("--convert", pages, "-").stdout.decode("utf-8")
    r = c.run("--convert", pages, str(work / "pages.jsonl"), "--format=chunks")
    try:
        chunks = [json.loads(line) for line in (work / "pages.jsonl").read_text("utf-8").splitlines()]
    except (OSError, ValueError):
        chunks = []
    indices = [b for chunk in chunks for b in chunk.get("blocks", [])]
    c.check("chunks output", r.returncode == 0 and bool(chunks) and
            [chunk.get("chunk") for chunk in chunks] == list(range(len(chunks))) and
            indices == list(range(len(indices))) and
            "\n\n".join(chunk.get("markdown", "") for chunk in chunks) + "\n" == markdown and
            all(isinstance(chunk.get("heading_path"), list) and len(chunk.get("pages", [])) == 2 and "boxes" in chunk
                for chunk in chunks), repr(chunks)[:300])
    r = c.run("--convert", pages, "-", "--format=chunks", "--chunk-chars=256")
    c.check("chunk size taken", r.returncode == 0 and all(
        len(json.loads(line)["markdown"].encode()) <= 256 for line in r.stdout.decode("utf-8").splitlines()),
        repr(r.stdout[:300]))
    r = c.run("--convert", pages, "-", "--format=chunks", "--chunk-chars=256", "--chunk-overlap=40")
    try:
        overlapped = [json.loads(line) for line in r.stdout.decode("utf-8").splitlines()]
    except ValueError:
        overlapped = []
    r2 = c.run("--convert", pages, "-", "--format=chunks", "--chunk-chars=256")
    plain = [json.loads(line) for line in r2.stdout.decode("utf-8").splitlines()] if r2.returncode == 0 else []
    c.check("chunk overlap: without it the chunks are the same", r.returncode == 0 and len(overlapped) == len(plain) > 1 and
            overlapped[0].get("overlap") == 0 and any(chunk.get("overlap", 0) > 0 for chunk in overlapped) and
            all(chunk["markdown"].encode()[chunk["overlap"]:] == other["markdown"].encode()
                for chunk, other in zip(overlapped, plain)), repr(overlapped[:2])[:300])
    r = c.run("--convert", pages, "-", "--format=chunks", "--chunk-by=page")
    try:
        paged = [json.loads(line) for line in r.stdout.decode("utf-8").splitlines()]
    except ValueError:
        paged = []
    c.check("chunks by page", r.returncode == 0 and len(paged) >= 7 and "overlap" not in paged[0] and
            all(chunk["pages"][0] == chunk["pages"][1] for chunk in paged), repr(paged[:2])[:300])
    for flag in ("--chunk-chars=10", "--chunk-chars=1000001", "--chunk-chars=", "--chunk-chars=4k",
                 "--chunk-overlap=-1", "--chunk-overlap=500001", "--chunk-overlap=", "--chunk-by=line"):
        c.check(f"bad {flag}", c.run("--convert", pages, "-", "--format=chunks", flag).returncode == 2)
    source = work / "chunks-in"
    source.mkdir()
    (source / "hello.pdf").write_bytes(Path(hello).read_bytes())
    r = c.run("--batch", str(source), str(work / "chunks-out"), "--format=chunks")
    try:
        lines = [json.loads(line) for line in (work / "chunks-out" / "hello.jsonl").read_text("utf-8").splitlines()]
    except (OSError, ValueError):
        lines = []
    c.check("batch writes NAME.jsonl", r.returncode == 0 and len(lines) == 1 and
            "Hello World" in lines[0].get("markdown", "") and not (work / "chunks-out" / "hello.md").exists(),
            repr(lines)[:200])
    request = json.dumps({"id": 1, "input": hello, "args": ["--format=chunks", "--chunk-chars=300"]}).encode() + b"\n"
    answers = records(c.run("--serve", "--json", stdin=request).stdout)
    answer = answers[1] if len(answers) > 1 else {}
    c.check("serve answers with the chunks", isinstance(answer.get("chunks"), list) and len(answer["chunks"]) == 1 and
            "markdown" not in answer and "Hello World" in answer["chunks"][0].get("markdown", ""), repr(answer)[:300])


def check_inspect(c: Checker, work: Path, hello: str) -> None:
    """--format=inspect: one HTML file without a script, a section and SVG a page with the text and
    the blocks numbered; --batch writes NAME.html; a serve answer carries the string "html"."""
    pages = str(work / "pages.pdf")
    r = c.run("--convert", pages, str(work / "pages.html"), "--format=inspect")
    html = (work / "pages.html").read_text("utf-8") if (work / "pages.html").exists() else ""
    c.check("inspect output", r.returncode == 0 and html.startswith("<!DOCTYPE html>") and "<script" not in html and
            all(f'<section id="page-{n}">' in html for n in range(1, 8)) and html.count("<svg ") == 7 and
            '<rect class="t-' in html and "<title>#1 " in html, repr(html[:300]))
    again = c.run("--convert", pages, "-", "--format=inspect")
    c.check("inspect output is deterministic", again.returncode == 0 and again.stdout.decode("utf-8") == html)
    source = work / "inspect-in"
    source.mkdir()
    (source / "hello.pdf").write_bytes(Path(hello).read_bytes())
    r = c.run("--batch", str(source), str(work / "inspect-out"), "--format=inspect")
    written = work / "inspect-out" / "hello.html"
    c.check("batch writes NAME.html", r.returncode == 0 and written.exists() and
            ">Hello World</text>" in written.read_text("utf-8") and not (work / "inspect-out" / "hello.md").exists())
    request = json.dumps({"id": 1, "input": hello, "args": ["--format=inspect"]}).encode() + b"\n"
    answers = records(c.run("--serve", "--json", stdin=request).stdout)
    answer = answers[1] if len(answers) > 1 else {}
    c.check("serve answers with the HTML", isinstance(answer.get("html"), str) and "markdown" not in answer and
            ">Hello World</text>" in answer["html"], repr(answer)[:300])


def check_text(c: Checker, work: Path, hello: str) -> None:
    """--format=text: no markup, one form feed a page break, every heading's text a line of its
    own; --batch writes NAME.txt; a serve answer carries the string "text"."""
    pages = str(work / "pages.pdf")
    markdown = c.run("--convert", pages, "-").stdout.decode("utf-8")
    r = c.run("--convert", pages, "-", "--format=text")
    text = r.stdout.decode("utf-8")
    headings = [line.lstrip("#").strip() for line in markdown.splitlines() if line.startswith("#")]
    lines = text.replace("\f", "\n").splitlines()
    c.check("text output", r.returncode == 0 and text.count("\f") == 6 and "<!--" not in text and
            not any(line.startswith("#") for line in lines) and all(h in lines for h in headings), repr(text[:300]))
    source = work / "text-in"
    source.mkdir()
    (source / "hello.pdf").write_bytes(Path(hello).read_bytes())
    r = c.run("--batch", str(source), str(work / "text-out"), "--format=text")
    written = work / "text-out" / "hello.txt"
    c.check("batch writes NAME.txt", r.returncode == 0 and written.exists() and
            written.read_text("utf-8") == "Hello World\n" and not (work / "text-out" / "hello.md").exists())
    request = json.dumps({"id": 1, "input": hello, "args": ["--format=text"]}).encode() + b"\n"
    answers = records(c.run("--serve", "--json", stdin=request).stdout)
    answer = answers[1] if len(answers) > 1 else {}
    c.check("serve answers with the text", answer.get("text") == "Hello World\n" and "markdown" not in answer,
            repr(answer)[:300])


def check_html(c: Checker, work: Path, hello: str) -> None:
    """--format=html: one HTML5 document without a script, a heading element for each Markdown
    heading; --batch writes NAME.html; a serve answer carries the string "html"."""
    pages = str(work / "pages.pdf")
    markdown = c.run("--convert", pages, "-").stdout.decode("utf-8")
    r = c.run("--convert", pages, "-", "--format=html")
    html = r.stdout.decode("utf-8")
    headings = sum(1 for line in markdown.splitlines() if line.startswith("#"))
    c.check("html output", r.returncode == 0 and html.startswith("<!DOCTYPE html>\n<html") and
            html.endswith("</body>\n</html>\n") and "<script" not in html and
            sum(html.count(f"<h{n}>") for n in range(1, 7)) == headings, repr(html[:300]))
    source = work / "html-in"
    source.mkdir()
    (source / "hello.pdf").write_bytes(Path(hello).read_bytes())
    r = c.run("--batch", str(source), str(work / "html-out"), "--format=html")
    written = work / "html-out" / "hello.html"
    c.check("batch writes NAME.html", r.returncode == 0 and written.exists() and
            "<p>Hello World</p>" in written.read_text("utf-8") and not (work / "html-out" / "hello.md").exists())
    request = json.dumps({"id": 1, "input": hello, "args": ["--format=html"]}).encode() + b"\n"
    answers = records(c.run("--serve", "--json", stdin=request).stdout)
    answer = answers[1] if len(answers) > 1 else {}
    c.check("serve answers with the HTML document", "<p>Hello World</p>" in answer.get("html", "") and
            "markdown" not in answer, repr(answer)[:300])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--binary", required=True)
    parser.add_argument("--emulator", help="run the binary through this program (wine for a Windows build)")
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    os.environ.setdefault("WINEDEBUG", "-all")

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        c = Checker(binary, work, args.emulator)
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

        bench = work / "bench"
        r = c.run("--bench", hello, str(bench), "3", "--images=none")
        c.check("bench", r.returncode == 0 and r.stdout.startswith(b"runs=3 best_ms=") and b" pages=1\n" in r.stdout
                and "Hello World" in (bench / "out.md").read_text(encoding="utf-8"), repr(r.stdout))
        c.check("bench without a count", c.run("--bench", hello, str(bench)).returncode == 2)
        c.check("bench with 0 runs", c.run("--bench", hello, str(bench), "0").returncode == 2)

        r = c.run("--convert", hello, out, "--report")
        try:
            report = json.loads(r.stdout)
        except ValueError:
            report = {}
        c.check("report", r.returncode == 0 and report.get("pages") == 1 and report.get("glyphs", 0) >= 10 and
                report.get("status") == "ok" and report.get("code") == 0 and report.get("input") == hello and
                report.get("bytes") == Path(out).stat().st_size, repr(r.stdout))
        c.check("report exact none", report.get("exact") == "none", repr(r.stdout))
        r = c.run("--convert", hello, out, "--report", "--no-exact")
        c.check("no-exact", r.returncode == 0 and b'"exact":"off"' in r.stdout, repr(r.stdout))
        r = c.run("--convert", str(work / "missing.pdf"), out, "--report")
        try:
            report = json.loads(r.stdout)
        except ValueError:
            report = {}
        c.check("report of a failure", r.returncode == 3 and report.get("status") == "error" and
                report.get("code") == 3 and "missing.pdf" in report.get("error", ""), repr(r.stdout))
        c.check("workers with inspect", c.run("--inspect", hello, "--workers=2").returncode == 2)
        c.check("convert with 0 workers", c.run("--convert", hello, out, "--workers=0").returncode == 2)
        check_page_workers(c, work)
        c.check("recursive without a batch", c.run("--convert", hello, out, "--recursive").returncode == 2)
        c.check("report file without a batch", c.run("--convert", hello, out, "--report=x.jsonl").returncode == 2)
        c.check("json without serve", c.run("--convert", hello, out, "--json").returncode == 2)

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

        picture = c.write("picture.pdf", build_pdf(
            b"BT /F1 12 Tf 72 720 Td (A picture) Tj ET q 300 0 0 300 72 300 cm /Im1 Do Q", image=True))
        r = c.run("--convert", picture, str(work / "picture.md"))
        text = (work / "picture.md").read_text(encoding="utf-8") if (work / "picture.md").is_file() else ""
        png = work / "picture-images" / "p1-1.png"
        c.check("image beside the output", r.returncode == 0 and "![](picture-images/p1-1.png)" in text and
                png.is_file() and png.read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", repr(text))
        (work / "sub").mkdir()
        r = c.run("--convert", picture, str(work / "sub" / "p.md"), "--images=" + str(work / "imgs"))
        text = (work / "sub" / "p.md").read_text(encoding="utf-8") if (work / "sub" / "p.md").is_file() else ""
        c.check("image directory option", r.returncode == 0 and "![](../imgs/p1-1.png)" in text and
                (work / "imgs" / "p1-1.png").is_file(), repr(text))
        r = c.run("--convert", picture, str(work / "none.md"), "--images=none")
        text = (work / "none.md").read_text(encoding="utf-8") if (work / "none.md").is_file() else ""
        c.check("no images", r.returncode == 0 and "![" not in text and not (work / "none-images").exists(),
                repr(text))
        r = c.run("--convert", picture, "-")
        c.check("no images on standard output", r.returncode == 0 and b"![" not in r.stdout)

        check_output_features(c, work, hello)
        check_chunks(c, work, hello)
        check_inspect(c, work, hello)
        check_text(c, work, hello)
        check_html(c, work, hello)
        check_batch(c, work, picture)
        check_serve(c, work, hello, locked, picture)

        print(f"{c.count - len(c.failures)}/{c.count} checks passed")
        return 1 if c.failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
