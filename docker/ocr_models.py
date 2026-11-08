#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Download the pinned PP-OCRv6 models and convert them for rayopdf-ocr (runs in docker/ocr.Dockerfile).

For each tier (tiny, small, medium) it fetches PaddlePaddle's official ONNX exports of the
detection and recognition models at a fixed Hugging Face revision, checks their SHA-256, and
writes into OUT/<tier>/:

  det.ncnn.param, det.ncnn.bin, rec.ncnn.param, rec.ncnn.bin   ncnn, converted by pnnx (pinned)
  det.ort, rec.ort                                            ONNX Runtime format (optimisation
                                                              level "extended": no NCHWc/NHWC
                                                              layouts, so one file fits every x86-64 CPU)
  dict.txt                                                    the recogniser's character list, one
                                                              entry a line, from inference.yml
  config.txt                                                  detection thresholds from inference.yml
  models.txt                                                  "<sha256> <bytes> <file>" of every file above

and OUT/required_operators.config (the operators of all six ORT models) for ONNX Runtime's
minimal build. Every step is deterministic: same pinned inputs, same output bytes.

Usage: python3 ocr_models.py --out /opt/ocr/models --pnnx /opt/pnnx/pnnx --work /tmp/models
"""

from __future__ import annotations

import argparse
import hashlib
import os
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
    ("medium", "det"): ("61323801669c338b7891481ec7bac61ce31b576a",
                        "eb13b44b25bb36f89528b68720af8a61d9cf381176107f465db1757b65d086e1",
                        "7298d5ead546584af2504d03355f881ac7a7bc0eb1e282d3e159277c1d0af871"),
    ("medium", "rec"): ("50c7eacafc52fa7bcf4194e8cd08e46f8558504b",
                        "9c09abf0957f7968c7586464b7397b84ad2387a0497a351af40e9acc71b673ba",
                        "991b700facf5b50a7de193468207d5f4255b538dde0d312ae3b7c7a9b6873129"),
}
# pnnx needs two example shapes to keep the width and height dynamic in the ncnn graph.
PNNX_SHAPES = {"det": ("[1,3,1536,1088]", "[1,3,960,736]"), "rec": ("[1,3,48,1600]", "[1,3,48,320]")}


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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--out", required=True)
    parser.add_argument("--pnnx", required=True)
    parser.add_argument("--work", required=True)
    args = parser.parse_args()
    out, work = Path(args.out), Path(args.work)
    onnx_dir = work / "onnx"
    onnx_dir.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)

    for (tier, kind), (rev, onnx_sha, yml_sha) in MODELS.items():
        fetch(HF.format(tier=tier, kind=kind, rev=rev, name="inference.onnx"), onnx_dir / f"{tier}_{kind}.onnx", onnx_sha)
        fetch(HF.format(tier=tier, kind=kind, rev=rev, name="inference.yml"), work / f"{tier}_{kind}.yml", yml_sha)

    # ONNX Runtime format, all six models at once, so one operator list covers every tier.
    env = dict(os.environ, ORT_CONVERT_ONNX_MODELS_TO_ORT_OPTIMIZATION_LEVEL="extended")
    ort_dir = work / "ort"
    subprocess.run(["python3", "-m", "onnxruntime.tools.convert_onnx_models_to_ort", str(onnx_dir),
                    "--output_dir", str(ort_dir), "--optimization_style", "Fixed", "--target_platform", "amd64"],
                   check=True, env=env)
    shutil.copyfile(ort_dir / "required_operators.extended.config", out / "required_operators.config")

    for tier in ("tiny", "small", "medium"):
        target = out / tier
        target.mkdir(exist_ok=True)
        for kind in ("det", "rec"):
            first, second = PNNX_SHAPES[kind]
            scratch = work / f"pnnx-{tier}-{kind}"
            scratch.mkdir(exist_ok=True)
            subprocess.run([args.pnnx, str(onnx_dir / f"{tier}_{kind}.onnx"), f"inputshape={first}",
                            f"inputshape2={second}", "fp16=0",
                            f"ncnnparam={target / (kind + '.ncnn.param')}", f"ncnnbin={target / (kind + '.ncnn.bin')}",
                            f"ncnnpy={scratch / 'ncnn.py'}", f"pnnxparam={scratch / 'pnnx.param'}",
                            f"pnnxbin={scratch / 'pnnx.bin'}", f"pnnxpy={scratch / 'pnnx.py'}",
                            f"pnnxonnx={scratch / 'pnnx.onnx'}"], check=True, cwd=scratch, stdout=subprocess.DEVNULL)
            shutil.copyfile(ort_dir / f"{tier}_{kind}.extended.ort", target / f"{kind}.ort")

        rec = yaml.safe_load((work / f"{tier}_rec.yml").read_text(encoding="utf-8"))
        characters = rec["PostProcess"]["character_dict"]
        for entry in characters:
            if not isinstance(entry, str) or not entry or "\n" in entry or "\r" in entry:
                raise SystemExit(f"ocr_models: {tier}: dictionary entry {entry!r} cannot be written one a line")
        (target / "dict.txt").write_bytes(("\n".join(characters) + "\n").encode("utf-8"))

        post = yaml.safe_load((work / f"{tier}_det.yml").read_text(encoding="utf-8"))["PostProcess"]
        if post.get("name") != "DBPostProcess":
            raise SystemExit(f"ocr_models: {tier}: detection post-processing {post.get('name')!r}")
        config = (f"model PP-OCRv6_{tier}\n"
                  f"det_thresh {post['thresh']}\n"
                  f"det_box_thresh {post['box_thresh']}\n"
                  f"det_unclip_ratio {post['unclip_ratio']}\n"
                  f"rec_height 48\n")
        (target / "config.txt").write_bytes(config.encode("ascii"))

        names = ["config.txt", "dict.txt", "det.ncnn.param", "det.ncnn.bin", "rec.ncnn.param", "rec.ncnn.bin",
                 "det.ort", "rec.ort"]
        lines = [f"{sha256(target / n)} {(target / n).stat().st_size} {n}\n" for n in names]
        (target / "models.txt").write_bytes("".join(lines).encode("ascii"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
