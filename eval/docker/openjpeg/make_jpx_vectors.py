#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Makes the JPEG 2000 test vectors of tests/test_image_jpx.cpp with OpenJPEG.

Runs inside rayopdf-eval-openjpeg:1 (opj_compress and jpx_ref). Each vector is a small synthetic
image (the formula Sample() below, which the test repeats) encoded with one feature; the script
also builds what opj_compress cannot write by rewriting its output: COC and QCC (components
encoded separately and joined in CPRL order), PPM and PPT (packet headers moved out of the tile
data), scalar derived quantization. For every vector it records the hash of OpenJPEG's decoded
components (palette and channel definitions off) at full and reduced resolution, and whether
the decoding equals the source (lossless).

Usage (from the repository root):
  docker run --rm -u $(id -u):$(id -g) --network none -v $PWD:/src -w /tmp \
      rayopdf-eval-openjpeg:1 python3 /src/eval/docker/openjpeg/make_jpx_vectors.py > vectors.inc
The output is the generated block of tests/test_image_jpx.cpp.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile


def sample(x, y, c, bits, signed):
    v = (x * 13 + y * 7 + c * 61 + ((x * y) >> 2) + ((x ^ (y * 3)) & 15) * 9) % (1 << bits)
    return v - (1 << (bits - 1)) if signed else v


def write_pnm(path, w, h, comps, bits):
    maxval = (1 << bits) - 1
    wide = bits > 8
    out = bytearray()
    for y in range(h):
        for x in range(w):
            for c in range(comps):
                v = sample(x, y, c, bits, False)
                out += struct.pack(">H", v) if wide else bytes([v])
    magic = b"P5" if comps == 1 else b"P6"
    with open(path, "wb") as f:
        f.write(b"%s\n%d %d\n%d\n" % (magic, w, h, maxval) + bytes(out))


def write_raw(path, w, h, comps, bits, signed, subsampling):
    out = bytearray()
    for c in range(comps):
        dx, dy = subsampling[c]
        cw, ch = (w + dx - 1) // dx, (h + dy - 1) // dy
        for y in range(ch):
            for x in range(cw):
                v = sample(x, y, c, bits, signed)
                out += struct.pack(">b" if signed else ">B", v) if bits <= 8 else struct.pack(">h" if signed else ">H", v)
    with open(path, "wb") as f:
        f.write(bytes(out))


def run(args):
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def reference(path, reduce):
    """OpenJPEG's components: [(w, h, prec, sgnd, samples)] or None."""
    out = path + ".ref%d" % reduce
    if subprocess.run(["jpx_ref", path, out, str(reduce), "1"], stdout=subprocess.DEVNULL,
                      stderr=subprocess.DEVNULL).returncode != 0:
        return None
    data = open(out, "rb").read()
    n = struct.unpack_from("<I", data, 12)[0]
    pos = 20
    comps = []
    for _ in range(n):
        w, h, x0, y0, dx, dy, prec, sgnd, alpha = struct.unpack_from("<9I", data, pos)
        comps.append([w, h, prec, sgnd])
        pos += 36
    for comp in comps:
        count = comp[0] * comp[1]
        comp.append(list(struct.unpack_from("<%di" % count, data, pos)))
        pos += 4 * count
    if reduce:
        # OpenJPEG sizes a reduced image with an offset one sample too large on a side and
        # leaves the extra samples 0; T.800's size is ceil(x1 / 2^r) - ceil(x0 / 2^r).
        cs = open(path, "rb").read()
        siz = cs.find(b"\xff\x4f\xff\x51") + 6
        x1, y1, x0, y0 = struct.unpack_from(">IIII", cs, siz + 2)
        for c, comp in enumerate(comps):
            dx, dy = cs[siz + 36 + 3 * c + 1], cs[siz + 36 + 3 * c + 2]
            def size(a, b, d):
                return -(-(-(-b // d)) // (1 << reduce)) - -(-(-(-a // d)) // (1 << reduce))
            w, h = size(x0, x1, dx), size(y0, y1, dy)
            samples = comp[4]
            assert w <= comp[0] and h <= comp[1]
            assert all(samples[y * comp[0] + x] == 0 for y in range(comp[1]) for x in range(comp[0]) if x >= w or y >= h)
            comp[4] = [samples[y * comp[0] + x] for y in range(h) for x in range(w)]
            comp[0], comp[1] = w, h
    return comps


def fnv(comps):
    h = 0xCBF29CE484222325
    def mix(v):
        nonlocal h
        for b in struct.pack("<I", v & 0xFFFFFFFF):
            h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    for w, hh, prec, sgnd, samples in comps:
        mix(w)
        mix(hh)
        for v in samples:
            mix(v)
    return h


def lossless(comps):
    for c, (w, h, prec, sgnd, samples) in enumerate(comps):
        for y in range(h):
            for x in range(w):
                if samples[y * w + x] != sample(x, y, c, prec, bool(sgnd)):
                    return False
    return True


# ---------------------------------------------------------------------------- codestream edits

def segments(cs):
    """The main header's marker segments [(marker, body)] and the offset of the first SOT."""
    pos = 2
    out = []
    while True:
        marker = struct.unpack_from(">H", cs, pos)[0]
        if marker == 0xFF90:
            return out, pos
        length = struct.unpack_from(">H", cs, pos + 2)[0]
        out.append((marker, cs[pos + 4:pos + 2 + length]))
        pos += 2 + length


def single_tile_body(cs, sot):
    """The one tile-part's header segments and data (Psot honoured, EOC dropped)."""
    psot = struct.unpack_from(">I", cs, sot + 6)[0]
    end = sot + psot if psot else len(cs) - 2
    pos = sot + 12
    header = []
    while struct.unpack_from(">H", cs, pos)[0] != 0xFF93:
        marker, length = struct.unpack_from(">HH", cs, pos)
        header.append((marker, cs[pos + 4:pos + 2 + length]))
        pos += 2 + length
    return header, cs[pos + 2:end]


def marker(m, body):
    return struct.pack(">HH", m, len(body) + 2) + body


def assemble(main, tile_header, body):
    out = b"\xff\x4f" + b"".join(marker(m, b) for m, b in main)
    th = b"".join(marker(m, b) for m, b in tile_header)
    psot = 12 + len(th) + 2 + len(body)
    out += struct.pack(">HHHIBB", 0xFF90, 10, 0, psot, 0, 1) + th + b"\xff\x93" + body + b"\xff\xd9"
    return out


def join_components(parts):
    """Joins single-component CPRL codestreams into one (COC and QCC for components 1..)."""
    first_main, _ = segments(parts[0][0])
    siz = None
    cod = qcd = None
    for m, b in first_main:
        if m == 0xFF51:
            siz = bytearray(b)
        elif m == 0xFF52:
            cod = bytearray(b)
        elif m == 0xFF5C:
            qcd = b
    siz[34:36] = struct.pack(">H", len(parts))
    siz = siz[:36]
    main_cocs = []
    bodies = b""
    for i, (cs, dx, dy) in enumerate(parts):
        segs, sot = segments(cs)
        csiz = [b for m, b in segs if m == 0xFF51][0]
        siz += csiz[36:37] + bytes([dx, dy])
        if i > 0:
            ccod = [b for m, b in segs if m == 0xFF52][0]
            cqcd = [b for m, b in segs if m == 0xFF5C][0]
            main_cocs.append((0xFF53, bytes([i, ccod[0] & 1]) + ccod[5:]))
            main_cocs.append((0xFF5D, bytes([i]) + cqcd))
        _, body = single_tile_body(cs, sot)
        bodies += body
    cod[1] = 4  # CPRL
    cod[4] = 0  # no MCT
    return assemble([(0xFF51, bytes(siz)), (0xFF52, bytes(cod)), (0xFF5C, qcd)] + main_cocs, [], bodies)


class Bio:
    """OpenJPEG's packet header bit reader (opj_bio)."""

    def __init__(self, data, pos):
        self.data, self.start, self.bp, self.buf, self.ct = data, pos, pos, 0, 0

    def bytein(self):
        self.buf = (self.buf << 8) & 0xFFFF
        self.ct = 7 if self.buf == 0xFF00 else 8
        if self.bp < len(self.data):
            self.buf |= self.data[self.bp]
            self.bp += 1

    def read(self, n):
        v = 0
        for _ in range(n):
            if self.ct == 0:
                self.bytein()
            self.ct -= 1
            v = (v << 1) | ((self.buf >> self.ct) & 1)
        return v

    def inalign(self):
        if (self.buf & 0xFF) == 0xFF:
            self.bytein()
        self.ct = 0


class TagTree:
    def __init__(self, w, h):
        self.nodes, self.parent = [], []
        levels = []
        lw, lh = w, h
        while True:
            levels.append((lw, lh))
            if lw * lh <= 1:
                break
            lw, lh = (lw + 1) // 2, (lh + 1) // 2
        start = 0
        for k, (lw, lh) in enumerate(levels):
            nstart = start + lw * lh
            for j in range(lh):
                for i in range(lw):
                    if k + 1 < len(levels):
                        nw = levels[k + 1][0]
                        self.parent.append(nstart + (j // 2) * nw + i // 2)
                    else:
                        self.parent.append(-1)
                    self.nodes.append([999, 0])
            start = nstart

    def decode(self, bio, leaf, threshold):
        stack = []
        node = leaf
        while self.parent[node] >= 0:
            stack.append(node)
            node = self.parent[node]
        low = 0
        while True:
            n = self.nodes[node]
            if low > n[1]:
                n[1] = low
            else:
                low = n[1]
            while low < threshold and low < n[0]:
                if bio.read(1):
                    n[0] = low
                else:
                    low += 1
            n[1] = low
            if not stack:
                break
            node = stack.pop()
        return self.nodes[node][0] < threshold


def split_headers(cs):
    """Packet headers and bodies of a 1-component, 1-tile LRCP codestream at the origin with one
    precinct per resolution and code-block style 0."""
    segs, sot = segments(cs)
    siz = [b for m, b in segs if m == 0xFF51][0]
    cod = [b for m, b in segs if m == 0xFF52][0]
    qcd = [b for m, b in segs if m == 0xFF5C][0]
    w, h = struct.unpack_from(">II", siz, 2)
    layers = struct.unpack_from(">H", cod, 2)[0]
    levels, cbw, cbh = cod[5], cod[6] + 2, cod[7] + 2
    guard = qcd[0] >> 5
    exps = [b >> 3 for b in qcd[1:]]
    _, data = single_tile_body(cs, sot)

    def ceil_shift(v, n):
        return -((-v) >> n)

    resolutions = []
    for r in range(levels + 1):
        bands = []
        if r == 0:
            bw, bh = ceil_shift(w, levels), ceil_shift(h, levels)
            bands.append((bw, bh, exps[0]))
        else:
            nb = levels + 1 - r
            for b, (xo, yo) in enumerate(((1, 0), (0, 1), (1, 1))):
                bw = ceil_shift(w - (xo << (nb - 1)), nb) - ceil_shift(-(xo << (nb - 1)), nb)
                bh = ceil_shift(h - (yo << (nb - 1)), nb) - ceil_shift(-(yo << (nb - 1)), nb)
                bands.append((bw, bh, exps[3 * (r - 1) + 1 + b]))
        state = []
        for bw, bh, e in bands:
            cw, ch = -(-bw // (1 << cbw)), -(-bh // (1 << cbh))
            empty = bw == 0 or bh == 0
            state.append({"cw": 0 if empty else cw, "ch": 0 if empty else ch, "incl": TagTree(cw, ch),
                          "zbp": TagTree(cw, ch), "blocks": [{"in": False, "lblock": 3} for _ in range(cw * ch)],
                          "empty": empty})
        resolutions.append(state)
    pos = 0
    headers, bodies = [], []
    for layer in range(layers):
        for r in range(levels + 1):
            bio = Bio(data, pos)
            lengths = []
            if bio.read(1):
                for band in resolutions[r]:
                    if band["empty"]:
                        continue
                    for k, blk in enumerate(band["blocks"]):
                        if not blk["in"]:
                            inc = band["incl"].decode(bio, k, layer + 1)
                        else:
                            inc = bio.read(1)
                        if not inc:
                            continue
                        if not blk["in"]:
                            i = 0
                            while not band["zbp"].decode(bio, k, i):
                                i += 1
                            blk["in"] = True
                        n = 1
                        if bio.read(1):
                            n = 2
                            if bio.read(1):
                                t = bio.read(2)
                                if t != 3:
                                    n = 3 + t
                                else:
                                    t = bio.read(5)
                                    n = 6 + t if t != 31 else 37 + bio.read(7)
                        while bio.read(1):
                            blk["lblock"] += 1
                        lengths.append(bio.read(blk["lblock"] + n.bit_length() - 1))
            bio.inalign()
            hdr_end = bio.bp
            body_end = hdr_end + sum(lengths)
            headers.append(data[pos:hdr_end])
            bodies.append(data[hdr_end:body_end])
            pos = body_end
    assert pos == len(data), (pos, len(data))
    return segs, b"".join(headers), b"".join(bodies)


def to_ppm(cs):
    segs, headers, bodies = split_headers(cs)
    ppm = b"\x00" + struct.pack(">I", len(headers)) + headers
    return assemble(segs + [(0xFF60, ppm)], [], bodies)


def to_ppt(cs):
    segs, headers, bodies = split_headers(cs)
    return assemble(segs, [(0xFF61, b"\x00" + headers)], bodies)


def to_derived(cs):
    segs, sot = segments(cs)
    out = []
    for m, b in segs:
        if m == 0xFF5C:
            b = bytes([(b[0] & 0xE0) | 1]) + b[1:3]
        out.append((m, b))
    return b"\xff\x4f" + b"".join(marker(m, b) for m, b in out) + cs[sot:]


# ---------------------------------------------------------------------------- vectors

W, H = 19, 15
BASE = ["-n", "3", "-b", "4,4"]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.parse_args()
    tmp = tempfile.mkdtemp()
    os.chdir(tmp)
    write_pnm("g8.pgm", W, H, 1, 8)
    write_pnm("rgb8.ppm", W, H, 3, 8)
    write_pnm("g16.pgm", W, H, 1, 16)
    write_pnm("g12.pgm", W, H, 1, 12)
    write_raw("g4.raw", W, H, 1, 4, False, [(1, 1)])  # a 4-bit PGM would come out 8-bit
    write_pnm("c2.pgm", (W + 1) // 2, (H + 1) // 2, 1, 8)
    write_raw("s8.raw", W, H, 1, 8, True, [(1, 1)])
    # Even sizes: OpenJPEG's raw reader sizes subsampled planes by flooring.
    write_raw("sub.raw", W + 1, H + 1, 3, 8, False, [(1, 1), (2, 2), (2, 2)])
    lossless_layers = ["-r", "8,3,1"]
    specs = [
        ("rev_gray", "g8.pgm", BASE),
        ("rev_rgb", "rgb8.ppm", BASE),
        ("irr_rgb", "rgb8.ppm", BASE + ["-I", "-r", "4"]),
        ("irr_gray_layers", "g8.pgm", BASE + ["-I", "-r", "20,8,2"]),
        ("lrcp", "rgb8.ppm", BASE + ["-c", "[8,8],[4,4]", "-p", "LRCP"] + lossless_layers),
        ("rlcp", "rgb8.ppm", BASE + ["-c", "[8,8],[4,4]", "-p", "RLCP"] + lossless_layers),
        ("rpcl", "rgb8.ppm", BASE + ["-c", "[8,8],[4,4]", "-p", "RPCL"] + lossless_layers),
        ("pcrl", "rgb8.ppm", BASE + ["-c", "[8,8],[4,4]", "-p", "PCRL"] + lossless_layers),
        ("cprl", "rgb8.ppm", BASE + ["-c", "[8,8],[4,4]", "-p", "CPRL"] + lossless_layers),
        ("bypass", "g8.pgm", BASE + ["-M", "1"] + lossless_layers),
        ("reset", "g8.pgm", BASE + ["-M", "2"] + lossless_layers),
        # 8x8 blocks: OpenJPEG's encoder underestimates its buffer for 4x4 blocks with TERMALL.
        ("termall", "g8.pgm", ["-n", "3", "-b", "8,8", "-M", "4"] + lossless_layers),
        ("causal", "g8.pgm", BASE + ["-M", "8"] + lossless_layers),
        ("predictable", "g8.pgm", BASE + ["-M", "16"] + lossless_layers),
        ("segmentation", "g8.pgm", BASE + ["-M", "32"] + lossless_layers),
        ("all_styles", "g8.pgm", BASE + ["-M", "63"] + lossless_layers),
        ("all_styles_irr", "rgb8.ppm", BASE + ["-M", "63", "-I", "-r", "12,3"]),
        ("tiles", "rgb8.ppm", ["-n", "2", "-b", "4,4", "-t", "8,8", "-d", "3,5", "-T", "1,2", "-TP", "R"]),
        ("sop_eph", "rgb8.ppm", BASE + ["-SOP", "-EPH", "-r", "8,1"]),
        ("poc", "rgb8.ppm", BASE + ["-POC", "T1=0,0,1,2,3,CPRL/T1=2,0,1,4,3,LRCP"]),
        ("roi", "g8.pgm", BASE + ["-ROI", "c=0,U=5"]),
        ("plt_tlm", "g8.pgm", BASE + ["-PLT", "-TLM"]),
        ("gray16", "g16.pgm", BASE),
        ("gray12_irr", "g12.pgm", BASE + ["-I", "-r", "5"]),
        ("gray4", "g4.raw", BASE + ["-F", "%d,%d,1,4,u" % (W, H)]),
        ("signed", "s8.raw", BASE + ["-F", "%d,%d,1,8,s" % (W, H)]),
        ("levels0", "g8.pgm", ["-n", "1", "-b", "4,4"]),
        ("block_wide", "g8.pgm", ["-n", "2", "-b", "16,4"]),
        ("guard1", "g8.pgm", BASE + ["-I", "-GuardBits", "1", "-r", "3"]),
        ("subsampled", "sub.raw", BASE + ["-F", "%d,%d,3,8,u@1x1:2x2:2x2" % (W + 1, H + 1), "-mct", "0"]),
        ("jp2_rgb", "rgb8.ppm", BASE),
        ("layers_gray", "g8.pgm", BASE + ["-r", "4,1"]),
    ]
    vectors = []
    for name, src, args in specs:
        out = name + (".jp2" if name.startswith("jp2") else ".j2k")
        run(["opj_compress", "-i", src, "-o", out] + args)
        vectors.append((name, open(out, "rb").read()))
    streams = dict(vectors)
    # Separately encoded components joined: COC, QCC, precisions, subsampling, both wavelets.
    run(["opj_compress", "-i", "g8.pgm", "-o", "p0.j2k", "-n", "3", "-b", "4,4", "-p", "CPRL"])
    run(["opj_compress", "-i", "g4.raw", "-F", "%d,%d,1,4,u" % (W, H), "-o", "p1.j2k", "-n", "2", "-b", "8,4", "-p",
         "CPRL", "-I", "-r", "2"])
    run(["opj_compress", "-i", "c2.pgm", "-o", "p2.j2k", "-n", "1", "-b", "4,4", "-p", "CPRL"])
    vectors.append(("coc_qcc", join_components([(open("p0.j2k", "rb").read(), 1, 1),
                                                (open("p1.j2k", "rb").read(), 1, 1),
                                                (open("p2.j2k", "rb").read(), 2, 2)])))
    vectors.append(("ppm", to_ppm(streams["layers_gray"])))
    vectors.append(("ppt", to_ppt(streams["layers_gray"])))
    vectors.append(("derived", to_derived(streams["guard1"])))
    for name, data in vectors[-3:-1]:
        open(name + ".j2k", "wb").write(data)
        assert fnv(reference(name + ".j2k", 0)) == fnv(reference("layers_gray.j2k", 0)), name

    print("// BEGIN GENERATED by eval/docker/openjpeg/make_jpx_vectors.py (OpenJPEG 2.5.4)")
    rows = []
    for name, data in vectors:
        path = name + ".bin"
        open(path, "wb").write(data)
        hashes, levels = [], 0
        full = reference(path, 0)
        if full is None:
            sys.exit("OpenJPEG cannot decode " + name)
        for reduce in range(3):
            comps = reference(path, reduce) if reduce else full
            hashes.append(fnv(comps) if comps is not None else 0)
        print("constexpr uint8_t kVector_%s[] = {" % name)
        for i in range(0, len(data), 20):
            print("    " + ", ".join("0x%02x" % b for b in data[i:i + 20]) + ",")
        print("};")
        rows.append('    {"%s", kVector_%s, sizeof kVector_%s, %s, {0x%016xull, 0x%016xull, 0x%016xull}},'
                    % (name, name, name, "true" if lossless(full) else "false", hashes[0], hashes[1], hashes[2]))
    print("constexpr Vector kVectors[] = {")
    print("\n".join(rows))
    print("};")
    print("// END GENERATED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
