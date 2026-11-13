#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Write the training corpus of the profile-guided static build (tools/dev.py --config static).

The PDFs exercise the code an ordinary conversion runs: text pages in the standard-14 fonts
(WinAnsi encoding, with and without /Widths, literal and hex strings, TJ arrays with kerning and
with word gaps but no space glyphs, word spacing), headings, lists, footnotes, two-column pages,
ruled and borderless tables, running heads and page numbers, inline images (Flate and ASCIIHex)
and image XObjects; a third of the files keep their objects in object streams with a
cross-reference stream. The output is the same bytes on every run (fixed seeds, no dates).

Usage:
  python3 tools/pgo_corpus.py [--out build/pgo] [--count 300]

The files are named pgo-000.pdf, pgo-001.pdf, ...; nothing else in the folder is touched. They are
generated material: never commit them (AGENTS.md rule 1).
"""

from __future__ import annotations

import argparse
import random
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

SYLLABLES = ["ra", "yo", "pen", "dor", "mi", "sta", "lu", "ver", "con", "tra", "ble", "qui", "so",
             "net", "al", "gor", "ith", "mic", "pro", "ces", "lay", "out", "ta", "ble", "col",
             "umn", "re", "port", "da", "ta", "ex", "am", "ple", "struc", "ture", "graph", "in"]
ACCENTED = [b"caf\xe9", b"na\xefve", b"\xfcber", b"Stra\xdfe", b"r\xe9sum\xe9", b"se\xf1or",
            b"co\xf6perate", b"fa\xe7ade"]
PUNCT = [b",", b",", b";", b":", b"", b"", b"", b""]
# Advance widths in 1/1000 em by character class: an estimate for line breaking, and the /Widths
# of the files that carry them.
NARROW, WIDE = set(b"iljtfr.,;:'!|()[] "), set(b"mwMW@")


def char_width(c: int, mono: bool) -> int:
    if mono:
        return 600
    if c in NARROW:
        return 278
    if c in WIDE:
        return 833
    if 65 <= c <= 90:
        return 667
    if 48 <= c <= 57:
        return 556
    return 500


def text_width(text: bytes, size: float, mono: bool = False) -> float:
    return sum(char_width(c, mono) for c in text) * size / 1000


def word(rng: random.Random) -> bytes:
    if rng.random() < 0.03:
        return rng.choice(ACCENTED)
    if rng.random() < 0.05:
        return str(rng.randint(1, 2030)).encode()
    return "".join(rng.choice(SYLLABLES) for _ in range(rng.randint(1, 3))).encode()


def sentence(rng: random.Random, words: int) -> bytes:
    parts = [word(rng) for _ in range(words)]
    parts[0] = parts[0][:1].upper() + parts[0][1:]
    for i in range(len(parts) - 1):
        parts[i] += rng.choice(PUNCT)
    return b" ".join(parts) + rng.choice([b".", b".", b".", b"?", b"!"])


def paragraph(rng: random.Random) -> bytes:
    return b" ".join(sentence(rng, rng.randint(4, 16)) for _ in range(rng.randint(1, 5)))


def wrap(text: bytes, width: float, size: float, mono: bool = False) -> list[bytes]:
    lines, line = [], b""
    for w in text.split(b" "):
        candidate = line + b" " + w if line else w
        if line and text_width(candidate, size, mono) > width:
            lines.append(line)
            line = w
        else:
            line = candidate
    if line:
        lines.append(line)
    return lines


def literal(text: bytes) -> bytes:
    out = bytearray(b"(")
    for c in text:
        if c in b"()\\":
            out += b"\\" + bytes([c])
        elif c >= 128:
            out += b"\\%03o" % c
        else:
            out.append(c)
    return bytes(out + b")")


def hexstring(text: bytes) -> bytes:
    return b"<" + text.hex().upper().encode() + b">"


def num(value: float) -> bytes:
    text = f"{value:.2f}".rstrip("0").rstrip(".")
    return (text if text != "-0" else "0").encode()


FONTS = {b"F1": b"Times-Roman", b"F2": b"Times-Bold", b"F3": b"Helvetica", b"F4": b"Helvetica-Bold",
         b"F5": b"Times-Italic", b"F6": b"Courier", b"F7": b"Helvetica-Oblique"}


class Page:
    def __init__(self, rng: random.Random, width: float, height: float, style: dict):
        self.rng, self.width, self.height, self.style = rng, width, height, style
        self.ops: list[bytes] = []
        self.xobjects: set[bytes] = set()

    def show(self, text: bytes) -> bytes:
        """One text-showing operator for a line, in the document's string style."""
        mode = self.style["strings"]
        if mode == "hex" or (mode == "mixed" and self.rng.random() < 0.3):
            return hexstring(text) + b" Tj"
        if mode == "kern" or (mode == "mixed" and self.rng.random() < 0.3):
            items = []
            for i, w in enumerate(text.split(b" ")):
                if i:
                    items.append(b"-250" if self.style["nospace"] else literal(b" "))
                cut = self.rng.randint(1, max(1, len(w) - 1)) if len(w) > 3 else len(w)
                items.append(literal(w[:cut]))
                if cut < len(w):
                    items.append(num(-self.rng.choice([10, 15, 20, 30])))
                    items.append(literal(w[cut:]))
            return b"[" + b" ".join(items) + b"] TJ"
        return literal(text) + b" Tj"

    def line(self, font: bytes, size: float, x: float, y: float, text: bytes) -> None:
        self.ops.append(b"BT /%s %s Tf %s %s Td %s ET" % (font, num(size), num(x), num(y), self.show(text)))

    def text_block(self, font: bytes, size: float, x: float, y: float, lines: list[bytes],
                   leading: float, justify: float = 0.0) -> None:
        ops = [b"BT /%s %s Tf %s TL %s %s Td" % (font, num(size), num(leading), num(x), num(y))]
        for i, text in enumerate(lines):
            last = i == len(lines) - 1
            spaces = text.count(b" ")
            tw = (justify - text_width(text, size)) / spaces if justify and spaces and not last else 0
            if tw > 0 and not self.style["nospace"]:
                ops.append(b"%s Tw" % num(min(tw, size * 0.6)))
            elif justify:
                ops.append(b"0 Tw")
            ops.append(self.show(text) + (b" T*" if not last else b""))
        ops.append(b"ET")
        self.ops.append(b"\n".join(ops))

    def rule(self, x0: float, y0: float, x1: float, y1: float, width: float = 0.5) -> None:
        self.ops.append(b"%s w %s %s m %s %s l S" % (num(width), num(x0), num(y0), num(x1), num(y1)))

    def inline_image(self, x: float, y: float, w: float, h: float) -> None:
        cols, rows = self.rng.choice([(8, 8), (16, 12), (24, 24)])
        gray = self.rng.random() < 0.5
        channels = 1 if gray else 3
        data = bytes((r * 13 + c * 7 + k * 50) & 255 for r in range(rows) for c in range(cols)
                     for k in range(channels))
        if self.rng.random() < 0.5:
            filt, data = b"/F /Fl", zlib.compress(data, 9)
        else:
            filt, data = b"/F /AHx", data.hex().encode() + b">"
        cs = b"/G" if gray else b"/RGB"
        self.ops.append(b"q %s 0 0 %s %s %s cm BI /W %d /H %d /CS %s /BPC 8 %s ID\n" %
                        (num(w), num(h), num(x), num(y), cols, rows, cs, filt) + data + b"\nEI Q")

    def xobject_image(self, name: bytes, x: float, y: float, w: float, h: float) -> None:
        self.xobjects.add(name)
        self.ops.append(b"q %s 0 0 %s %s %s cm /%s Do Q" % (num(w), num(h), num(x), num(y), name))

    def fill_rect(self, x: float, y: float, w: float, h: float) -> None:
        shade = num(0.85 + 0.1 * self.rng.random())
        self.ops.append(b"q %s %s %s rg %s %s %s %s re f Q" % (shade, shade, num(0.9), num(x), num(y), num(w), num(h)))

    def content(self) -> bytes:
        return b"\n".join(self.ops) + b"\n"


class Flow:
    """Places blocks top-down in one or more columns of a page, opening pages as needed."""

    def __init__(self, doc: "Document", columns: int):
        self.doc, self.columns = doc, columns
        self.page: Page | None = None
        self.new_page()

    def new_page(self) -> None:
        self.page = self.doc.new_page()
        self.column = 0
        self.y = self.doc.top

    def col_geometry(self) -> tuple[float, float]:
        d = self.doc
        gutter = 24.0
        width = (d.width - 2 * d.margin - gutter * (self.columns - 1)) / self.columns
        return d.margin + self.column * (width + gutter), width

    def need(self, height: float) -> Page:
        if self.y - height < self.doc.bottom:
            self.column += 1
            self.y = self.doc.top
            if self.column >= self.columns:
                self.new_page()
        return self.page

    def heading(self, text: bytes, size: float) -> None:
        x, width = self.col_geometry()
        lines = wrap(text, width, size)
        page = self.need(size * 1.3 * len(lines) + size)
        self.y -= size * 0.8
        page.text_block(b"F4" if size > 12 else b"F2", size, x, self.y - size, lines, size * 1.25)
        self.y -= size * 1.25 * len(lines) + size * 0.4

    def paragraph(self, text: bytes, size: float, font: bytes = b"F1", indent: float = 0.0) -> None:
        x, width = self.col_geometry()
        lines = wrap(text, width - indent, size, font == b"F6")
        leading = size * 1.2
        while lines:
            page = self.need(leading * 2)
            room = max(1, int((self.y - self.doc.bottom) // leading))
            part, lines = lines[:room], lines[room:]
            justify = width - indent if self.doc.style["justify"] and font != b"F6" else 0.0
            page.text_block(font, size, x + indent, self.y - size, part, leading, justify)
            self.y -= leading * len(part)
        self.y -= size * 0.6

    def list_items(self, items: list[bytes], size: float, ordered: bool) -> None:
        x, width = self.col_geometry()
        for n, item in enumerate(items, 1):
            mark = b"%d." % n if ordered else b"\x95"
            lines = wrap(item, width - 18, size)
            page = self.need(size * 1.2 * min(len(lines), 2))
            page.line(b"F1", size, x, self.y - size, mark)
            self.paragraph(item, size, indent=18)
            self.y += size * 0.4

    def space(self, height: float) -> Page:
        page = self.need(height)
        self.y -= height
        return page


class Document:
    def __init__(self, index: int):
        rng = self.rng = random.Random(index * 7919 + 17)
        self.width, self.height = rng.choice([(612.0, 792.0), (595.0, 842.0)])
        self.margin = rng.choice([54.0, 63.0, 72.0])
        self.top, self.bottom = self.height - self.margin, self.margin
        self.style = {"strings": rng.choice(["literal", "literal", "hex", "kern", "mixed"]),
                      "nospace": rng.random() < 0.2, "justify": rng.random() < 0.5,
                      "widths": rng.random() < 0.35, "objstm": index % 3 == 2,
                      "compress": rng.random() < 0.85}
        if self.style["nospace"]:
            self.style["strings"] = "kern"
        self.pages: list[Page] = []
        self.title = sentence(rng, rng.randint(3, 7)).rstrip(b".?!")

    def new_page(self) -> Page:
        page = Page(self.rng, self.width, self.height, self.style)
        self.pages.append(page)
        return page

    def body(self, flow: Flow, blocks: int) -> None:
        rng = self.rng
        for _ in range(blocks):
            kind = rng.random()
            if kind < 0.12:
                flow.heading(b"%d %s" % (rng.randint(1, 9), sentence(rng, rng.randint(2, 5)).rstrip(b".?!")),
                             rng.choice([12.0, 13.0, 14.0]))
            elif kind < 0.2:
                flow.list_items([sentence(rng, rng.randint(3, 14)) for _ in range(rng.randint(2, 6))],
                                10.0, rng.random() < 0.5)
            elif kind < 0.24:
                code = b" ".join(b"x%d = f(%d);" % (i, i * 3) for i in range(rng.randint(4, 12)))
                flow.paragraph(code, 9.0, b"F6")
            elif kind < 0.28:
                x, width = flow.col_geometry()
                h = rng.uniform(40, 120)
                page = flow.space(h + 8)
                if rng.random() < 0.5:
                    page.inline_image(x, flow.y + 4, min(width, h * 1.5), h)
                else:
                    page.xobject_image(b"Im1", x, flow.y + 4, min(width, h * 1.5), h)
                flow.paragraph(b"Figure %d. " % rng.randint(1, 20) + sentence(rng, rng.randint(4, 10)), 9.0, b"F5")
            elif kind < 0.31:
                x, width = flow.col_geometry()
                page = flow.space(30)
                page.fill_rect(x, flow.y + 4, width, 22)
                page.line(b"F4", 10, x + 6, flow.y + 11, sentence(rng, rng.randint(3, 6)))
            else:
                flow.paragraph(paragraph(rng), rng.choice([9.5, 10.0, 10.0, 11.0]),
                               b"F5" if rng.random() < 0.05 else b"F1")

    def table(self, flow: Flow, ruled: bool) -> None:
        rng = self.rng
        cols, rows = rng.randint(3, 6), rng.randint(4, 14)
        size = 9.0
        x, width = flow.col_geometry()
        header = [word(rng).capitalize() for _ in range(cols)]
        kinds = [rng.choice(["word", "int", "dec", "pct"]) for _ in range(cols)]
        kinds[0] = "word"
        cells = []
        for _ in range(rows):
            row = []
            for kind in kinds:
                if kind == "int":
                    row.append(f"{rng.randint(0, 99999):,}".encode())
                elif kind == "dec":
                    row.append(f"{rng.uniform(0, 1000):.2f}".encode())
                elif kind == "pct":
                    row.append(f"{rng.uniform(0, 100):.1f}%".encode())
                else:
                    row.append(b" ".join(word(rng) for _ in range(rng.randint(1, 2))))
            cells.append(row)
        col_w = [max(text_width(t, size) for t in [header[c]] + [r[c] for r in cells]) + 12 for c in range(cols)]
        scale = min(1.0, width / sum(col_w))
        col_w = [w * scale for w in col_w]
        row_h = size * 1.6
        flow.paragraph(b"Table %d. " % rng.randint(1, 30) + sentence(rng, rng.randint(3, 9)), 9.0, b"F2")
        page = flow.need(row_h * (rows + 1) + 6)
        top = flow.y
        xs = [x]
        for w in col_w:
            xs.append(xs[-1] + w)
        for r, row in enumerate([header] + cells):
            base = top - row_h * (r + 1) + size * 0.5
            for c, text in enumerate(row):
                font = b"F4" if r == 0 else b"F3"
                if kinds[c] != "word" and r:
                    tx = xs[c + 1] - 6 - text_width(text, size)
                else:
                    tx = xs[c] + 6
                page.line(font, size, tx, base, text)
        bottom = top - row_h * (rows + 1)
        if ruled:
            for r in range(rows + 2):
                page.rule(xs[0], top - row_h * r, xs[-1], top - row_h * r)
            for xv in xs:
                page.rule(xv, top, xv, bottom)
        elif rng.random() < 0.6:
            page.rule(xs[0], top, xs[-1], top, 0.8)
            page.rule(xs[0], top - row_h, xs[-1], top - row_h, 0.4)
            page.rule(xs[0], bottom, xs[-1], bottom, 0.8)
        flow.y = bottom - size

    def build(self, index: int) -> None:
        rng = self.rng
        layout = index % 5
        flow = Flow(self, 1)
        page = flow.page
        page.text_block(b"F4", 18.0, self.margin, self.top - 18, wrap(self.title, self.width - 2 * self.margin, 18.0), 22.0)
        flow.y -= 22.0 * len(wrap(self.title, self.width - 2 * self.margin, 18.0)) + 8
        flow.paragraph(b" ".join(word(rng).capitalize() for _ in range(rng.randint(2, 6))), 11.0, b"F5")
        if layout == 1 or (layout == 4 and rng.random() < 0.5):
            flow.columns = 2
        sections = rng.randint(1, 4)
        for _ in range(sections):
            self.body(flow, rng.randint(3, 10))
            if layout in (2, 4) or rng.random() < 0.25:
                self.table(flow, ruled=rng.random() < 0.5)
            if layout == 3 and rng.random() < 0.7:
                self.table(flow, ruled=False)
        if rng.random() < 0.4:
            page = flow.space(30)
            x, _ = flow.col_geometry()
            page.rule(x, flow.y + 20, x + 120, flow.y + 20)
            page.line(b"F1", 8, x, flow.y + 10, b"1 " + sentence(rng, rng.randint(5, 12)))
        if len(self.pages) > 1 or rng.random() < 0.5:
            head = b"%s \x97 %s" % (word(rng).capitalize(), self.title[:40])
            for n, page in enumerate(self.pages, 1):
                page.line(b"F7", 8, self.margin, self.height - self.margin / 2, head)
                folio = b"%d" % n if rng.random() < 0.9 else b"Page %d" % n
                if n % 2 or index % 2:
                    px = self.width / 2 - 5
                else:
                    px = self.width - self.margin - 10
                page.line(b"F3", 9, px, self.margin / 2, folio)

    def serialize(self) -> bytes:
        objects: list[bytes | None] = [None]  # 1-based object numbers
        streams: set[int] = set()

        def add(body: bytes, stream: bool = False) -> int:
            objects.append(body)
            if stream:
                streams.add(len(objects) - 1)
            return len(objects) - 1

        def stream(dict_items: bytes, data: bytes, compress: bool) -> int:
            if compress:
                data = zlib.compress(data, 6)
                dict_items += b" /Filter /FlateDecode"
            return add(b"<<%s /Length %d>>\nstream\n" % (dict_items, len(data)) + data + b"\nendstream", True)

        font_refs = {}
        for name, base in FONTS.items():
            entry = b"<</Type /Font /Subtype /Type1 /BaseFont /%s /Encoding /WinAnsiEncoding" % base
            if self.style["widths"]:
                mono = base == b"Courier"
                widths = b" ".join(b"%d" % char_width(c, mono) for c in range(32, 256))
                entry += b" /FirstChar 32 /LastChar 255 /Widths [%s]" % widths
            font_refs[name] = add(entry + b">>")
        pixels = bytes((r * 9 + c * 5 + k * 80) & 255 for r in range(24) for c in range(32) for k in range(3))
        image = stream(b"/Type /XObject /Subtype /Image /Width 32 /Height 24 /ColorSpace /DeviceRGB /BitsPerComponent 8",
                       pixels, True)
        fonts = b" ".join(b"/%s %d 0 R" % (n, r) for n, r in font_refs.items())
        pages_id = len(objects) + 2 * len(self.pages)
        kids = []
        for page in self.pages:
            content = stream(b"", page.content(), self.style["compress"])
            xobj = b" /XObject <</Im1 %d 0 R>>" % image if page.xobjects else b""
            kids.append(add(b"<</Type /Page /Parent %d 0 R /MediaBox [0 0 %s %s] /Contents %d 0 R "
                            b"/Resources <</Font <<%s>>%s>>>>" %
                            (pages_id, num(self.width), num(self.height), content, fonts, xobj)))
        assert add(b"<</Type /Pages /Kids [%s] /Count %d>>" %
                   (b" ".join(b"%d 0 R" % k for k in kids), len(kids))) == pages_id
        info = add(b"<</Title %s /Producer (RayoPDF pgo_corpus)>>" % literal(self.title))
        catalog = add(b"<</Type /Catalog /Pages %d 0 R>>" % pages_id)
        return self.write_objstm(objects, streams, catalog, info) if self.style["objstm"] else \
            self.write_table(objects, catalog, info)

    @staticmethod
    def write_table(objects: list, catalog: int, info: int) -> bytes:
        out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
        offsets = [0]
        for n in range(1, len(objects)):
            offsets.append(len(out))
            out += b"%d 0 obj\n" % n + objects[n] + b"\nendobj\n"
        xref = len(out)
        out += b"xref\n0 %d\n0000000000 65535 f \n" % len(objects)
        for offset in offsets[1:]:
            out += b"%010d 00000 n \n" % offset
        out += b"trailer\n<</Size %d /Root %d 0 R /Info %d 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (
            len(objects), catalog, info, xref)
        return bytes(out)

    @staticmethod
    def write_objstm(objects: list, streams: set, catalog: int, info: int) -> bytes:
        out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
        packed = [n for n in range(1, len(objects)) if n not in streams]
        stm_id, xref_id = len(objects), len(objects) + 1
        entries: dict[int, tuple[int, int, int]] = {0: (0, 0, 65535)}
        for n in range(1, len(objects)):
            if n in streams:
                entries[n] = (1, len(out), 0)
                out += b"%d 0 obj\n" % n + objects[n] + b"\nendobj\n"
        header, body = [], bytearray()
        for i, n in enumerate(packed):
            header.append(b"%d %d" % (n, len(body)))
            body += objects[n] + b"\n"
            entries[n] = (2, stm_id, i)
        head = b" ".join(header) + b"\n"
        data = zlib.compress(head + bytes(body), 6)
        entries[stm_id] = (1, len(out), 0)
        out += b"%d 0 obj\n<</Type /ObjStm /N %d /First %d /Filter /FlateDecode /Length %d>>\nstream\n" % (
            stm_id, len(packed), len(head), len(data)) + data + b"\nendstream\nendobj\n"
        entries[xref_id] = (1, len(out), 0)
        rows = b"".join(bytes([t]) + a.to_bytes(4, "big") + b.to_bytes(2, "big")
                        for t, a, b in (entries[n] for n in range(xref_id + 1)))
        rows = zlib.compress(rows, 6)
        xref = len(out)
        out += b"%d 0 obj\n<</Type /XRef /Size %d /W [1 4 2] /Root %d 0 R /Info %d 0 R /Filter /FlateDecode " \
               b"/Length %d>>\nstream\n" % (xref_id, xref_id + 1, catalog, info, len(rows)) + rows + \
               b"\nendstream\nendobj\nstartxref\n%d\n%%%%EOF\n" % xref
        return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter,
                                     epilog=__doc__.split("\n\n", 1)[1])
    parser.add_argument("--out", type=Path, default=REPO / "build" / "pgo", help="output folder (default build/pgo)")
    parser.add_argument("--count", type=int, default=300, help="number of PDFs (default 300)")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    pages = 0
    for index in range(args.count):
        doc = Document(index)
        doc.build(index)
        pages += len(doc.pages)
        (args.out / f"pgo-{index:03d}.pdf").write_bytes(doc.serialize())
    print(f"pgo_corpus: {args.count} PDFs, {pages} pages in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
