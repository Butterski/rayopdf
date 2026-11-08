#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Build the `scanned` suite: image-only PDFs made from the valid synthetic corpus items.

Usage: scan.py --corpus /corpus --out /out [--only ids] [--jobs 6]

/corpus holds corpus.json and <producer>/<fixture>.pdf (the synthetic corpus). Every valid item
becomes two scanned items, s300/<producer>/<fixture>.pdf and s200/<producer>/<fixture>.pdf:

1. pdftoppm renders each page at the item's dpi (gray, or RGB for colour variants).
2. A degradation is applied: clean, noise (Gaussian noise, grey paper, salt and pepper), skew
   (0.3-1.2 degree rotation) or blur (Gaussian, radius dpi/250). Random values come from a NumPy
   PCG64 generator seeded with the SHA-256 of the item id, so every build is identical.
3. The page is encoded as one image XObject: JPEG (libjpeg-turbo via Pillow), CCITT G4 (libtiff
   via Pillow), JBIG2 (jbig2enc: generic region, generic with TPGD, or symbol mode with a shared
   /JBIG2Globals stream), JPEG 2000 (opj_compress: JP2 or raw codestream, lossy 9/7 or lossless
   5/3, several progression orders, tiles, precincts, layers) or Flate (8-bit, with or without
   the PNG Up predictor). Bilevel codecs use a fixed threshold of 150 on the gray page.
4. A small own PDF writer wraps the images, one per page, with the page size equal to the image
   size at the item's dpi (so `pdftoppm -r <dpi>` reproduces the encoded pixels 1:1).

The codec, its sub-variant and the degradation are assigned by rotation over the items sorted by
id, so each (codec, degradation) pair occurs equally often per dpi (see matrix() below).

An item is valid when every page was encoded, `pdfimages -list` reports the expected encoding for
every page, `pdftotext` finds no text, and `pdftoppm -r <dpi> -gray` of every page differs from
the gray page that was encoded by a mean absolute difference below MAX_RENDER_DIFF (codec loss,
resampling and rounding only; an inverted or garbled image is far above). /out/scanned.json
lists every item; ground truth is the source fixture's Markdown.
Items made from the weasyprint producer are holdout. Runs inside rayopdf-eval-producers:1 with
no network.
"""

import argparse
import concurrent.futures
import glob
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import zlib

import numpy as np
from PIL import Image, ImageFilter, TiffImagePlugin

DPIS = (300, 200)
CODECS = ("jpeg", "ccitt", "jbig2", "jpx", "flate")
DEGRADATIONS = ("clean", "noise", "skew", "blur")
# Sub-variants per codec: (name, colour). Colour variants render RGB, the others gray.
SUBVARIANTS = {
    "jpeg": (("baseline-q85", True), ("baseline-q70-gray", False), ("progressive-q75", True)),
    "ccitt": (("g4", False), ("g4-blackis1-decode", False), ("g4-imagemask", False)),
    "jbig2": (("generic", False), ("generic-tpgd", False), ("symbol-globals", False)),
    "jpx": (("jp2-lossy-rgb", True), ("j2k-lossy-gray-rpcl-precincts", False),
            ("jp2-lossless-gray-tiled-pcrl", False), ("jp2-layers-rgb-lrcp-sop-eph", True)),
    "flate": (("gray", False), ("gray-png-up", False), ("rgb", True)),
}
JPX_ARGS = {
    "jp2-lossy-rgb": ["-r", "20", "-I"],
    "j2k-lossy-gray-rpcl-precincts": ["-r", "15", "-I", "-p", "RPCL", "-n", "5", "-b", "32,32",
                                      "-c", "[256,256],[256,256],[128,128]"],
    "jp2-lossless-gray-tiled-pcrl": ["-t", "1024,1024", "-p", "PCRL"],
    "jp2-layers-rgb-lrcp-sop-eph": ["-r", "40,20,10", "-p", "LRCP", "-SOP", "-EPH"],
}
POPPLER_ENC = {"jpeg": "jpeg", "ccitt": "ccitt", "jbig2": "jbig2", "jpx": "jpx", "flate": "image"}
BILEVEL_THRESHOLD = 150
# Splash resamples a 1:1 image by up to a pixel, so even lossless pages differ by a few levels
# (up to about 15 on noisy pages); an inverted or garbled page differs by about 200.
MAX_RENDER_DIFF = 40.0
TIMEOUT_S = 600


def matrix(index, dpi):
    """(codec, sub-variant index, degradation) of the index-th source item at dpi.

    Within every block of 20 consecutive items each (codec, degradation) pair occurs once per
    dpi; the 200 dpi variant is shifted so one source never gets the same codec twice.
    """
    shift = 0 if dpi == 300 else 2
    codec = CODECS[(index + shift) % len(CODECS)]
    degradation = DEGRADATIONS[(index // len(CODECS) + shift) % len(DEGRADATIONS)]
    sub = (index // (len(CODECS) * len(DEGRADATIONS)) + (dpi == 200)) % len(SUBVARIANTS[codec])
    return codec, sub, degradation


def seed_of(item_id):
    return int.from_bytes(hashlib.sha256(item_id.encode()).digest()[:8], "little")


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, timeout=TIMEOUT_S, **kw)


# ---- pixels -------------------------------------------------------------------------------


def render_pages(pdf, dpi, colour, prefix):
    proc = run(["pdftoppm", "-r", str(dpi)] + ([] if colour else ["-gray"]) + [pdf, prefix])
    if proc.returncode != 0:
        raise RuntimeError("pdftoppm: " + proc.stderr.decode("utf-8", "replace")[-300:])
    files = sorted(glob.glob(prefix + "-*.p?m"),
                   key=lambda p: int(p.rsplit("-", 1)[1].split(".")[0]))
    if not files:
        raise RuntimeError("pdftoppm wrote no pages")
    return files


def degrade(img, degradation, dpi, rng):
    if degradation == "clean":
        return img
    if degradation == "blur":
        return img.filter(ImageFilter.GaussianBlur(radius=dpi / 250.0))
    if degradation == "skew":
        angle = float(rng.uniform(0.3, 1.2)) * (1 if rng.integers(0, 2) else -1)
        white = 255 if img.mode == "L" else (255, 255, 255)
        return img.rotate(angle, resample=Image.BICUBIC, expand=False, fillcolor=white)
    # noise: grey paper, Gaussian noise (sigma 10), 0.05 % salt and pepper
    a = np.asarray(img, dtype=np.float32) * 0.9 + 14.0
    a += rng.normal(0.0, 10.0, size=a.shape).astype(np.float32)
    flip = rng.random(a.shape[:2])
    a[flip < 0.00025] = 0.0
    a[flip > 0.99975] = 255.0
    return Image.fromarray(np.clip(a + 0.5, 0, 255).astype(np.uint8), img.mode)


def bilevel(img):
    """Mode "1" image: 0 = black ink, 255 = paper."""
    gray = img.convert("L")
    return gray.point(lambda v: 255 if v >= BILEVEL_THRESHOLD else 0).convert("1", dither=Image.NONE)


# ---- encoders: each returns (image dict entries, data, extra objects) -----------------------


def encode_jpeg(img, sub, tmp):
    quality = {"baseline-q85": 85, "baseline-q70-gray": 70, "progressive-q75": 75}[sub]
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=quality, progressive=sub.startswith("progressive"),
             optimize=False, subsampling=2 if img.mode == "RGB" else 0)
    cs = "/DeviceRGB" if img.mode == "RGB" else "/DeviceGray"
    return {"ColorSpace": cs, "BitsPerComponent": "8", "Filter": "/DCTDecode"}, buf.getvalue()


def g4_strip(img1):
    """CCITT G4 data of a mode "1" image from libtiff, as one strip with paper as white runs."""
    TiffImagePlugin.STRIP_SIZE = 1 << 30
    # libtiff codes runs of 0 bits as white. Pillow stores mode "1" with photometric
    # MinIsBlack (0 bits = black), so the ink/paper polarity is swapped before encoding.
    inverted = img1.point(lambda v: 255 - v)
    buf = io.BytesIO()
    inverted.save(buf, "TIFF", compression="group4")
    tif = Image.open(io.BytesIO(buf.getvalue()))
    offsets, counts = tif.tag_v2[273], tif.tag_v2[279]
    if len(offsets) != 1:
        raise RuntimeError(f"libtiff wrote {len(offsets)} strips")
    if tif.tag_v2.get(266, 1) != 1:
        raise RuntimeError("unexpected FillOrder")
    raw = buf.getvalue()
    return raw[offsets[0]:offsets[0] + counts[0]], tif.tag_v2.get(262)


def encode_ccitt(img, sub, tmp):
    img1 = bilevel(img)
    data, photometric = g4_strip(img1)
    w, h = img1.size
    params = f"<< /K -1 /Columns {w} /Rows {h}"
    if sub == "g4-blackis1-decode":
        entries = {"ColorSpace": "/DeviceGray", "BitsPerComponent": "1", "Decode": "[1 0]"}
        params += " /BlackIs1 true"
    elif sub == "g4-imagemask":
        entries = {"ImageMask": "true", "BitsPerComponent": "1"}
    else:
        entries = {"ColorSpace": "/DeviceGray", "BitsPerComponent": "1"}
    entries.update({"Filter": "/CCITTFaxDecode", "DecodeParms": params + " >>"})
    return entries, data, photometric


def write_pbm(img1, path):
    w, h = img1.size
    # PBM: 1 = black; mode "1" bytes from tobytes() are 1 = white, so invert.
    packed = bytes(b ^ 0xFF for b in img1.tobytes())
    with open(path, "wb") as f:
        f.write(b"P4\n%d %d\n" % (w, h) + packed)


def encode_jpx(img, sub, tmp, index):
    ext = ".j2k" if sub.startswith("j2k") else ".jp2"
    src = os.path.join(tmp, f"jpx{index}.{'ppm' if img.mode == 'RGB' else 'pgm'}")
    dst = os.path.join(tmp, f"jpx{index}{ext}")
    img.save(src)
    proc = run(["opj_compress", "-i", src, "-o", dst] + JPX_ARGS[sub])
    if proc.returncode != 0 or not os.path.isfile(dst):
        raise RuntimeError("opj_compress: " + proc.stderr.decode("utf-8", "replace")[-300:])
    with open(dst, "rb") as f:
        data = f.read()
    entries = {"Filter": "/JPXDecode"}
    if ext == ".j2k":
        # A raw codestream carries no colour box; the dictionary names the space.
        entries.update({"ColorSpace": "/DeviceRGB" if img.mode == "RGB" else "/DeviceGray",
                        "BitsPerComponent": "8"})
    return entries, data


def encode_flate(img, sub):
    a = np.asarray(img, dtype=np.uint8)
    h, w = a.shape[:2]
    colors = 3 if img.mode == "RGB" else 1
    entries = {"ColorSpace": "/DeviceRGB" if colors == 3 else "/DeviceGray",
               "BitsPerComponent": "8", "Filter": "/FlateDecode"}
    if sub == "gray-png-up":
        rows = a.reshape(h, w * colors).astype(np.int16)
        up = np.empty_like(rows)
        up[0] = rows[0]
        up[1:] = rows[1:] - rows[:-1]
        filtered = np.concatenate([np.full((h, 1), 2, np.int16), up & 0xFF], axis=1)
        raw = filtered.astype(np.uint8).tobytes()
        entries["DecodeParms"] = f"<< /Predictor 15 /Colors {colors} /BitsPerComponent 8 /Columns {w} >>"
    else:
        raw = a.tobytes()
    return entries, zlib.compress(raw, 6)


# ---- PDF writer ---------------------------------------------------------------------------


class PdfWriter:
    def __init__(self):
        self.objects = []

    def add(self, body):
        self.objects.append(body)
        return len(self.objects)

    def reserve(self):
        return self.add(None)

    def set(self, num, body):
        self.objects[num - 1] = body

    @staticmethod
    def stream(entries, data):
        head = "".join(f" /{k} {v}" for k, v in entries.items())
        return b"<<" + head.encode() + b" /Length %d >>\nstream\n" % len(data) + data + b"\nendstream"

    def bytes(self, root, info):
        out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
        offsets = []
        for i, body in enumerate(self.objects, 1):
            offsets.append(len(out))
            out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
        xref = len(out)
        out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(self.objects) + 1)
        for off in offsets:
            out += b"%010d 00000 n \n" % off
        out += (b"trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
                % (len(self.objects) + 1, root, info, xref))
        return bytes(out)


def build_pdf(pages, dpi, globals_data):
    """pages: list of (width, height, image dict entries, data)."""
    pdf = PdfWriter()
    catalog = pdf.reserve()
    pages_obj = pdf.reserve()
    info = pdf.add(b"<< /Producer (rayopdf-eval scan.py) >>")
    globals_ref = None
    if globals_data is not None:
        globals_ref = pdf.add(PdfWriter.stream({}, globals_data))
    kids = []
    for w, h, entries, data in pages:
        entries = dict(entries)
        if globals_ref is not None:
            entries["DecodeParms"] = f"<< /JBIG2Globals {globals_ref} 0 R >>"
        image = pdf.add(PdfWriter.stream({"Type": "/XObject", "Subtype": "/Image", "Width": str(w),
                                          "Height": str(h), **entries}, data))
        pw, ph = w * 72.0 / dpi, h * 72.0 / dpi
        content = pdf.add(PdfWriter.stream({}, b"q %.4f 0 0 %.4f 0 0 cm /Im0 Do Q" % (pw, ph)))
        kids.append(pdf.add(b"<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.4f %.4f] "
                            b"/Resources << /XObject << /Im0 %d 0 R >> >> /Contents %d 0 R >>"
                            % (pages_obj, pw, ph, image, content)))
    pdf.set(pages_obj, b"<< /Type /Pages /Kids [%s] /Count %d >>"
            % (b" ".join(b"%d 0 R" % k for k in kids), len(kids)))
    pdf.set(catalog, b"<< /Type /Catalog /Pages %d 0 R >>" % pages_obj)
    return pdf.bytes(catalog, info)


# ---- items --------------------------------------------------------------------------------


def encode_pages(images, codec, sub, tmp):
    """Returns (pages, globals data or None)."""
    if codec == "jbig2":
        bil = [bilevel(i) for i in images]
        pbms = []
        for n, img1 in enumerate(bil):
            path = os.path.join(tmp, f"page{n:04d}.pbm")
            write_pbm(img1, path)
            pbms.append(path)
        if sub == "symbol-globals":
            proc = run(["jbig2", "-s", "-p", "-b", "sym"] + pbms, cwd=tmp)
            if proc.returncode != 0:
                raise RuntimeError("jbig2: " + proc.stderr.decode("utf-8", "replace")[-300:])
            with open(os.path.join(tmp, "sym.sym"), "rb") as f:
                glob_data = f.read()
            pages = []
            for n, img1 in enumerate(bil):
                with open(os.path.join(tmp, f"sym.{n:04d}"), "rb") as f:
                    pages.append((*img1.size, {"ColorSpace": "/DeviceGray", "BitsPerComponent": "1",
                                               "Filter": "/JBIG2Decode"}, f.read()))
            return pages, glob_data
        pages = []
        for img1, path in zip(bil, pbms):
            argv = ["jbig2", "-p"] + (["-d"] if sub == "generic-tpgd" else []) + [path]
            proc = run(argv, cwd=tmp)
            if proc.returncode != 0 or not proc.stdout:
                raise RuntimeError("jbig2: " + proc.stderr.decode("utf-8", "replace")[-300:])
            pages.append((*img1.size, {"ColorSpace": "/DeviceGray", "BitsPerComponent": "1",
                                       "Filter": "/JBIG2Decode"}, proc.stdout))
        return pages, None
    pages = []
    for n, img in enumerate(images):
        if codec == "jpeg":
            entries, data = encode_jpeg(img, sub, tmp)
        elif codec == "ccitt":
            entries, data, _ = encode_ccitt(img, sub, tmp)
        elif codec == "jpx":
            entries, data = encode_jpx(img, sub, tmp, n)
        else:
            entries, data = encode_flate(img, sub)
        w, h = img.size
        pages.append((w, h, entries, data))
    return pages, None


def expected_gray(img, codec):
    """The page as the PDF should show it, 8-bit gray: what the codec was given."""
    if codec in ("ccitt", "jbig2"):
        return np.asarray(bilevel(img).convert("L"), dtype=np.int16)
    return np.asarray(img.convert("L"), dtype=np.int16)


def validate(out_pdf, codec, dpi, expected, tmp):
    """(reason or None, worst per-page mean abs diff between the encoded page and its render).

    The page size makes `pdftoppm -r <dpi>` map image pixels about 1:1, so the difference is the
    codec's loss plus Splash's resampling and gray-conversion rounding (a few levels), far
    below MAX_RENDER_DIFF unless the image is inverted or garbled.
    """
    proc = run(["pdfimages", "-list", out_pdf])
    if proc.returncode != 0:
        return "pdfimages failed", None
    encs = [line.split()[8] for line in proc.stdout.decode("utf-8", "replace").splitlines()[2:]
            if len(line.split()) > 8]
    if not encs or any(e != POPPLER_ENC[codec] for e in encs):
        return f"pdfimages encodings {sorted(set(encs))}", None
    proc = run(["pdftotext", "-enc", "UTF-8", out_pdf, "-"])
    if proc.returncode != 0 or proc.stdout.strip():
        return "pdftotext found text or failed", None
    files = render_pages(out_pdf, dpi, False, os.path.join(tmp, "v"))
    if len(files) != len(expected):
        return f"page count {len(files)} != {len(expected)}", None
    worst = 0.0
    for path, want in zip(files, expected):
        got = np.asarray(Image.open(path), dtype=np.int16)
        hh, ww = min(got.shape[0], want.shape[0]), min(got.shape[1], want.shape[1])
        worst = max(worst, float(np.abs(got[:hh, :ww] - want[:hh, :ww]).mean()))
    if worst >= MAX_RENDER_DIFF:
        return f"render differs from the encoded page (mean abs diff {worst:.1f})", worst
    return None, worst


def make_item(job):
    source, dpi, codec, sub_index, degradation, corpus_dir, out_dir = job
    sub, colour = SUBVARIANTS[codec][sub_index]
    item_id = f"s{dpi}/{source['id']}"
    record = {"id": item_id, "source": source["id"], "fixture": source["fixture"],
              "producer": source["producer"], "class": source["class"],
              "source_tagged": source["tagged"], "tagged": False, "dpi": dpi, "codec": codec,
              "codec_variant": sub, "colour": colour, "degradation": degradation,
              "holdout": source["holdout"], "pages": None, "bytes": None, "sha256": None,
              "render_diff": None, "valid": False, "reason": None}
    src_pdf = os.path.join(corpus_dir, source["id"] + ".pdf")
    out_pdf = os.path.join(out_dir, item_id + ".pdf")
    rng = np.random.default_rng(seed_of(item_id))
    try:
        with tempfile.TemporaryDirectory(dir="/tmp") as tmp:
            files = render_pages(src_pdf, dpi, colour, os.path.join(tmp, "p"))
            images = []
            for path in files:
                img = Image.open(path)
                img.load()
                images.append(degrade(img, degradation, dpi, rng))
            pages, glob_data = encode_pages(images, codec, sub, tmp)
            expected = [expected_gray(img, codec) for img in images]
            del images
            data = build_pdf(pages, dpi, glob_data)
            os.makedirs(os.path.dirname(out_pdf), exist_ok=True)
            with open(out_pdf, "wb") as f:
                f.write(data)
            record.update(pages=len(pages), bytes=len(data),
                          sha256=hashlib.sha256(data).hexdigest())
            record["reason"], diff = validate(out_pdf, codec, dpi, expected, tmp)
            record["render_diff"] = None if diff is None else round(diff, 2)
            record["valid"] = record["reason"] is None
    except (RuntimeError, OSError, subprocess.TimeoutExpired, ValueError) as e:
        record["reason"] = f"{type(e).__name__}: {e}"[:300]
    return record


def read_versions():
    try:
        with open("/opt/eval/versions.json", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", help="comma-separated source ids (<producer>/<fixture>)")
    ap.add_argument("--jobs", type=int, default=6)
    args = ap.parse_args()

    with open(os.path.join(args.corpus, "corpus.json"), encoding="utf-8") as f:
        sources = sorted((i for i in json.load(f)["items"] if i["valid"]), key=lambda i: i["id"])
    only = set(args.only.split(",")) if args.only else None
    jobs = []
    for index, source in enumerate(sources):
        if only and source["id"] not in only:
            continue
        for dpi in DPIS:
            codec, sub, degradation = matrix(index, dpi)
            jobs.append((source, dpi, codec, sub, degradation, args.corpus, args.out))
    path = os.path.join(args.out, "scanned.json")
    previous = []
    if only and os.path.isfile(path):
        with open(path, encoding="utf-8") as f:
            previous = json.load(f).get("items", [])
    done = {}
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for record in pool.map(make_item, jobs):
            done[record["id"]] = record
            print(f"{record['id']}: {record['codec']}/{record['codec_variant']}/"
                  f"{record['degradation']} {'ok' if record['valid'] else record['reason']}",
                  flush=True)
    items = {i["id"]: i for i in previous}
    items.update(done)
    items = [items[k] for k in sorted(items)]
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"schema": 1, "versions": read_versions(), "items": items}, f, indent=1,
                  ensure_ascii=False)
        f.write("\n")
    valid = sum(1 for i in items if i["valid"])
    print(f"scanned.json: {valid}/{len(items)} valid", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
