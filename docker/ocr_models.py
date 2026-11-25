#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Download the pinned PP-OCRv6 models and convert them for rayopdf-ocr (runs in docker/ocr.Dockerfile).

For each tier (tiny, small) it fetches PaddlePaddle's official ONNX exports of the detection and
recognition models at a fixed Hugging Face revision, checks their SHA-256, and writes into
OUT/<tier>/:

  det.ncnn.param, det.ncnn.bin, rec.ncnn.param, rec.ncnn.bin   ncnn, converted by pnnx (pinned)
  dict.txt                                                    the recogniser's character list, one
                                                              entry a line, from inference.yml
  config.txt                                                  detection thresholds from inference.yml
  models.txt                                                  "<sha256> <bytes> <file>" of every file above

The script pack and the orientation classifier go to:

  OUT/scripts/<script>/   rec.ncnn.param, rec.ncnn.bin, dict.txt, config.txt ("model", "script",
                          "order visual|logical"), models.txt: PaddleOCR's PP-OCRv5 mobile
                          recognisers for cyrillic, hangul (korean), arabic, devanagari and tamil
                          (official ONNX exports), and PP-OCRv3 kannada (the newest Kannada model;
                          PaddlePaddle publishes it in Paddle format only, so the `kannada` stage of
                          docker/ocr.Dockerfile converts it with paddle2onnx and this script checks
                          the result's SHA-256)
  OUT/orient/             cls.ncnn.param, cls.ncnn.bin, dict.txt (the labels "0_degree",
                          "180_degree"), config.txt, models.txt: PP-LCNet_x0_25_textline_ori, the
                          text-line 0/180 degree classifier

The script pack's weights are stored in half precision (pnnx fp16=1; ncnn widens them to float32
when it loads them, arithmetic stays float32): half the bytes, and the same readings on the
multilingual suite to 0.0002 CER per script (docs/development/ocr-engine.md). Every step is
deterministic: same pinned inputs, same output bytes.

Usage: python3 ocr_models.py --out /opt/ocr/models --pnnx /opt/pnnx/pnnx --work /tmp/models --kannada /opt/ka
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import urllib.request
from pathlib import Path

import yaml

HF = "https://huggingface.co/PaddlePaddle/PP-OCRv6_{tier}_{kind}_onnx/resolve/{rev}/{name}"
# (tier, kind): (revision, SHA-256 of inference.onnx, SHA-256 of inference.yml)
MODELS = {
    ("tiny", "det"): ("2ba1506c0380b8f0b03dd142459aac66d4421f6c",
                      "193bab7a04fca699a6c82e6abb5b81bdb28177f0abd4062552b04908dafb19f8",
                      "3ac018be6f97499a08faa3bbdeb33640968d9307f6736d152902747a9f259593"),
    ("tiny", "rec"): ("2612ab37152ae0a677521bae4e1e3d4fb4cf7c30",
                      "9ef676d6ed3c88256a2d92c640c44f25b0c40947e111b14b8be8f594091563e6",
                      "66170210bad538e83fff3c4a3867e547d6bf20b50d64b20347c4b913f3034ea1"),
    ("small", "det"): ("28fe5895c24fd108c19eb3e8479f4ab385fbfc62",
                       "d73e0058b7a8086bbd57f3d10b8bcd4ff95363f67e06e2762b5e814fe9c9410e",
                       "193f435274bf9f0b5f71a929bbfbcf148282df7e633b34e7c373e8f44741b516"),
    ("small", "rec"): ("b8f84f0b80c529de40b4fbb3544b84fa7233a513",
                       "5435fd747c9e0efe15a96d0b378d5bd157e9492ed8fd80edf08f30d02fa24634",
                       "ab078671bb49f06228eadccd34f1bb501e157f7a047095ffb943ba81512c77d1"),
}
# pnnx needs two example shapes to keep the width and height dynamic in the ncnn graph.
PNNX_SHAPES = {"det": ("[1,3,1536,1088]", "[1,3,960,736]"), "rec": ("[1,3,48,1600]", "[1,3,48,320]")}

V5 = "https://huggingface.co/PaddlePaddle/{repo}/resolve/{rev}/{name}"
# script: (repository, revision, SHA-256 of inference.onnx, SHA-256 of inference.yml, text order of
# the network's output). The order is "visual" for right-to-left scripts read left to right.
SCRIPTS = {
    "cyrillic": ("cyrillic_PP-OCRv5_mobile_rec_onnx", "2cef88145434beb8afa9dd82d77d799eb1ad7b29",
                 "5371ee1ddaa7983cc62d0818d99e982b6804638c85e4f960d59a574094e172e5",
                 "5c76cc91fa98410178a09f498db10050d0ec1634a660053d3005ab7be581f501", "logical"),
    "hangul": ("korean_PP-OCRv5_mobile_rec_onnx", "5c6f574b8e2230adf4287b33e736d71b9fabd28e",
               "92f0b7785e64fc9090106a241cf4c1eb97472824558272751b88a2a4476d3a08",
               "f757fa1c40e99edcf27e9cce879b93eb2a51fa46f5ef39095689b8c37dd75998", "logical"),
    "arabic": ("arabic_PP-OCRv5_mobile_rec_onnx", "14aaedcd75825982689ecf5cd64ab33ee083215a",
               "799113ebf267fbe742deb99eb36e8d42c9ddc5291ceacf92add41b4d52a59110",
               "21368419e6c016c31db55d316d59e11c128e1913e6e6fe10287084710043d3a6", "visual"),
    "devanagari": ("devanagari_PP-OCRv5_mobile_rec_onnx", "251aec19e36739540d35e2cc943f6aa7503b98e5",
                   "cb789212ce96c69d3e74728ae4309d179281d68cb3945d0616b67cafab41c986",
                   "9bd172dd26440c8ce94d1cde5d5baea6aefdc7cf3c5c8492e0beedef656d4e54", "logical"),
    "tamil": ("ta_PP-OCRv5_mobile_rec_onnx", "0db89798af0d5218fad9ea7ace0557526e9077f1",
              "c6d2b682d2a0ea4cb1fccdba295976f93fd439964d16cdc666cadef531accbee",
              "88a28f5a1bb30cabe38a0985cb5e6619fa4f0c7c78e57a08274674228c5219a6", "logical"),
}
# PP-OCRv3 Kannada, converted by the kannada stage: SHA-256 of its inference.yml and of the
# converted ka.onnx (paddle2onnx 2.1.0 with paddlepaddle 3.3.1, opset 14).
KANNADA = ("ka_PP-OCRv3_mobile_rec", "948eb2cdc413b7754a3c41e1b4902fbab5478f0bb0f1b9d814f5b08c7bb51765",
           "40d12b908d08d883233d3953c7fdb65ffa875efe7fb723e5f3c87ca2b379abe5")
ORIENT = ("PP-LCNet_x0_25_textline_ori_onnx", "aea1f18d97338aaf84b463f90192f2820524a00a",
          "94a6a0a0425f2b5f08b5df72086f2d72abe40f1d22f6d12d2cd83674f11f2ff3",
          "d9a0a3fc7c5c0ae416dbb565d63a32cb4692f05aee1bf7b677b9dd571adaa759")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def fetch(url: str, path: Path, expected: str) -> None:
    if not path.exists():
        with urllib.request.urlopen(url, timeout=300) as response, open(path, "wb") as out:
            shutil.copyfileobj(response, out)
    actual = sha256(path)
    if actual != expected:
        raise SystemExit(f"ocr_models: {url}: SHA-256 {actual}, expected {expected}")


def pnnx(binary: str, onnx: Path, shapes: tuple, target: Path, stem: str, scratch: Path, fp16: bool = False) -> None:
    scratch.mkdir(parents=True, exist_ok=True)
    first, second = shapes
    argv = [binary, str(onnx), f"inputshape={first}"] + ([f"inputshape2={second}"] if second else []) + [
        f"fp16={int(fp16)}", f"ncnnparam={target / (stem + '.ncnn.param')}", f"ncnnbin={target / (stem + '.ncnn.bin')}",
        f"ncnnpy={scratch / 'ncnn.py'}", f"pnnxparam={scratch / 'pnnx.param'}", f"pnnxbin={scratch / 'pnnx.bin'}",
        f"pnnxpy={scratch / 'pnnx.py'}", f"pnnxonnx={scratch / 'pnnx.onnx'}"]
    subprocess.run(argv, check=True, cwd=scratch, stdout=subprocess.DEVNULL)


def write_dict(target: Path, characters: list, what: str) -> None:
    for entry in characters:
        if not isinstance(entry, str) or not entry or "\n" in entry or "\r" in entry:
            raise SystemExit(f"ocr_models: {what}: dictionary entry {entry!r} cannot be written one a line")
    (target / "dict.txt").write_bytes(("\n".join(characters) + "\n").encode("utf-8"))


def write_manifest(target: Path, names: list) -> None:
    lines = [f"{sha256(target / n)} {(target / n).stat().st_size} {n}\n" for n in names]
    (target / "models.txt").write_bytes("".join(lines).encode("ascii"))


def scripts(args, work: Path, out: Path) -> None:
    """The script pack (OUT/scripts/<script>/) and the orientation classifier (OUT/orient/)."""
    sources = {}
    for script, (repo, rev, onnx_sha, yml_sha, order) in SCRIPTS.items():
        onnx, yml = work / f"script_{script}.onnx", work / f"script_{script}.yml"
        fetch(V5.format(repo=repo, rev=rev, name="inference.onnx"), onnx, onnx_sha)
        fetch(V5.format(repo=repo, rev=rev, name="inference.yml"), yml, yml_sha)
        sources[script] = (onnx, yml, repo.replace("_onnx", ""), order)
    kannada = Path(args.kannada)
    for path, expected in ((kannada / "inference.yml", KANNADA[1]), (kannada / "ka.onnx", KANNADA[2])):
        if sha256(path) != expected:
            raise SystemExit(f"ocr_models: {path}: SHA-256 {sha256(path)}, expected {expected}")
    sources["kannada"] = (kannada / "ka.onnx", kannada / "inference.yml", KANNADA[0], "logical")
    # Config lines beyond model, script and order: PP-OCRv3 Kannada drops the spaces between words,
    # which come back from gaps in the ink (InsertInkSpaces; 0.16 line heights, measured on the
    # multilingual set: docs/development/ocr-engine.md).
    extra = {"kannada": "space_gap 0.16\n"}
    # The code points of each script's letters: a recogniser's reading wins a line only when most of
    # its letters are of its own script (engine.h, step 6).
    letters = {"cyrillic": "0400-052F", "hangul": "AC00-D7AF", "arabic": "0600-08FF", "devanagari": "0900-097F",
               "kannada": "0C80-0CFF", "tamil": "0B80-0BFF"}

    for script, (onnx, yml, model, order) in sources.items():
        target = out / "scripts" / script
        target.mkdir(parents=True, exist_ok=True)
        pnnx(args.pnnx, onnx, PNNX_SHAPES["rec"], target, "rec", work / f"pnnx-script-{script}", True)
        write_dict(target, yaml.safe_load(yml.read_text(encoding="utf-8"))["PostProcess"]["character_dict"], script)
        config = (f"model {model}\nscript {script}\norder {order}\nrec_height 48\nletters {letters[script]}\n" +
                  extra.get(script, ""))
        (target / "config.txt").write_bytes(config.encode("ascii"))
        write_manifest(target, ["config.txt", "dict.txt", "rec.ncnn.param", "rec.ncnn.bin"])

    repo, rev, onnx_sha, yml_sha = ORIENT
    onnx, yml = work / "orient.onnx", work / "orient.yml"
    fetch(V5.format(repo=repo, rev=rev, name="inference.onnx"), onnx, onnx_sha)
    fetch(V5.format(repo=repo, rev=rev, name="inference.yml"), yml, yml_sha)
    spec = yaml.safe_load(yml.read_text(encoding="utf-8"))
    labels = spec["PostProcess"]["Topk"]["label_list"]
    resize = [op["ResizeImage"]["size"] for op in spec["PreProcess"]["transform_ops"] if "ResizeImage" in op]
    if labels != ["0_degree", "180_degree"] or resize != [[160, 80]]:
        raise SystemExit(f"ocr_models: {repo}: labels {labels}, input {resize}")
    target = out / "orient"
    target.mkdir(parents=True, exist_ok=True)
    pnnx(args.pnnx, onnx, ("[1,3,80,160]", None), target, "cls", work / "pnnx-orient")
    write_dict(target, labels, "orient")
    config = f"model {repo.replace('_onnx', '')}\ncls_width 160\ncls_height 80\n"
    (target / "config.txt").write_bytes(config.encode("ascii"))
    write_manifest(target, ["config.txt", "dict.txt", "cls.ncnn.param", "cls.ncnn.bin"])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", required=True)
    parser.add_argument("--pnnx", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--kannada", required=True, help="folder with ka.onnx and inference.yml (kannada stage)")
    args = parser.parse_args()
    out, work = Path(args.out), Path(args.work)
    onnx_dir = work / "onnx"
    onnx_dir.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)

    for (tier, kind), (rev, onnx_sha, yml_sha) in MODELS.items():
        fetch(HF.format(tier=tier, kind=kind, rev=rev, name="inference.onnx"), onnx_dir / f"{tier}_{kind}.onnx", onnx_sha)
        fetch(HF.format(tier=tier, kind=kind, rev=rev, name="inference.yml"), work / f"{tier}_{kind}.yml", yml_sha)

    for tier in ("tiny", "small"):
        target = out / tier
        target.mkdir(exist_ok=True)
        for kind in ("det", "rec"):
            pnnx(args.pnnx, onnx_dir / f"{tier}_{kind}.onnx", PNNX_SHAPES[kind], target, kind, work / f"pnnx-{tier}-{kind}")

        rec = yaml.safe_load((work / f"{tier}_rec.yml").read_text(encoding="utf-8"))
        write_dict(target, rec["PostProcess"]["character_dict"], tier)

        post = yaml.safe_load((work / f"{tier}_det.yml").read_text(encoding="utf-8"))["PostProcess"]
        if post.get("name") != "DBPostProcess":
            raise SystemExit(f"ocr_models: {tier}: detection post-processing {post.get('name')!r}")
        config = (f"model PP-OCRv6_{tier}\n"
                  f"det_thresh {post['thresh']}\n"
                  f"det_box_thresh {post['box_thresh']}\n"
                  f"det_unclip_ratio {post['unclip_ratio']}\n"
                  f"rec_height 48\n")
        (target / "config.txt").write_bytes(config.encode("ascii"))

        write_manifest(target, ["config.txt", "dict.txt", "det.ncnn.param", "det.ncnn.bin", "rec.ncnn.param",
                                "rec.ncnn.bin"])
    scripts(args, work, out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
