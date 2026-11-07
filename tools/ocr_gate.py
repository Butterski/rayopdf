#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""M8 OCR/ML gate: computes M1, M2, M3, R, S, Q1-Q3 and T of docs/research/ocr-ml.md §6.2.

Every number in docs/development/ocr_ml_decision.md comes from this script. It reads only harness
outputs; `collect` and `collect-rayopdf` are the steps that run containers.

Usage:
  python3 tools/ocr_gate.py collect [--jobs 6]
      Runs eval/census/pages.py in rayopdf-eval-producers:1 (--network none) over the SafeDocs
      zip and olmOCR-bench's old_scans and old_scans_math PDFs. Writes
      <root>/ocr-gate/safedocs-pages.jsonl and <root>/ocr-gate/old-scans-pages.jsonl.
  python3 tools/ocr_gate.py collect-rayopdf [--bin build/static/rayopdf] [--jobs 6]
      Runs eval/census/rayopdf_pages.py in rayopdf-build:2 (--network none): the binary converts
      every census page alone (--pages=N-N --images=none) and each page is judged by the census's
      own broken-text-layer checks. Writes <root>/ocr-gate/rayopdf-pages.jsonl and
      <root>/ocr-gate/rayopdf-pages.meta.json (binary version and SHA-256).
  python3 tools/ocr_gate.py measure [--tool cli] [--ml docling]
      Reads the census, the collect outputs, the odl, synthetic and olmOCR scores and the speed
      files. Writes <root>/ocr-gate/gate.json and prints the tables and the branch the thresholds
      select.

All take --root (default benchmark-output/eval).

Inputs read by `measure` (under --root):
  census/census.jsonl                    pdftotext census classes and scripts per page
  ocr-gate/rayopdf-pages.jsonl           RayoPDF's own class per census page (collect-rayopdf)
  ocr-gate/safedocs-pages.jsonl          per-image coverage of each census page (collect)
  ocr-gate/old-scans-pages.jsonl         census classes of olmOCR old_scans* (collect)
  scores/odl/work/<tool>/evaluation.json odl-bench NID/TEDS/MHS per document
  scores/synthetic/<tool>.json           synthetic NID/TEDS/MHS per item
  scores/olmocr/<tool>.log               olmOCR pass rates per JSONL
  speed/cli.json, speed-cli-noimages.json, speed/<ml>.json
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import importlib.util
import json
import random
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("rayopdf_eval_harness", REPO / "tools" / "eval.py")
H = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(H)

BUILD_IMAGE = "rayopdf-build:2"
DEFAULT_BIN = REPO / "build" / "static" / "rayopdf"

# Text-bearing image regions on digital pages (§6.2 R, third term). Without OCR the content of an
# image is unknown, so the rule is an upper bound: on a page whose text layer is good (census class
# `text`, RayoPDF not broken), every image covering at least REGION_MIN_IMAGE of the page counts as
# a possibly text-bearing region; the page contributes min(1, sum of those images' area fractions)
# page-equivalents. Smaller images (logos, icons, bullets, rules drawn as images) are not counted.
REGION_MIN_IMAGE = 0.05

# Thresholds of roadmap M8 / ocr-ml.md §6.3.
R_NO_OCR = 0.001
R_LINKED = 0.05
Q1_MIN = 0.03
Q2_MIN = 0.10
Q3_MIN = 0.05
LATIN_CYRILLIC_MOSTLY = 0.5

# Claimed OCR costs from docs/research/ocr-ml.md §2.1 (not reproduced here).
OCR_COST_CLAIMED = [
    ("Tesseract 4/5 LSTM, 1 page", "1.5 s wall / 2.5 s CPU", "tessdoc 4.0 Accuracy and Performance (2017)"),
    ("Tesseract, 1 Hindi page", "1.8 s (OpenMP+AVX) to 4.6 s (no SIMD)", "tessdoc 4.0 Accuracy and Performance (2017)"),
    ("PP-OCRv6 tiny, ONNX Runtime", "0.22 s/image", "PP-OCRv6 doc, Xeon 8350C"),
    ("PP-OCRv6 small, ONNX Runtime", "0.61 s/image", "PP-OCRv6 doc, Xeon 8350C"),
    ("PP-OCRv6 medium, ONNX Runtime / OpenVINO", "3.31 s / 1.40 s per image", "PP-OCRv6 doc, Xeon 8350C"),
]

SCRIPTS = ["latin", "cyrillic", "greek", "cjk", "arabic", "hebrew", "indic", "other", "unknown"]


def frac(n, d):
    return n / d if d else None


def pct(x, digits=2):
    return "–" if x is None else f"{100 * x:.{digits}f} %"


def num(x, digits=3, sign=False):
    return "–" if x is None else (f"{x:+.{digits}f}" if sign else f"{x:.{digits}f}")


# ------------------------------------------------------------------------------- collect


def cmd_collect(args) -> int:
    out = args.root / "ocr-gate"
    out.mkdir(parents=True, exist_ok=True)
    safedocs = args.root / "data" / "safedocs"
    scans = args.root / "data" / "olmOCR-bench" / "bench_data" / "pdfs"
    if not list(safedocs.glob("*.zip")):
        raise SystemExit(f"no zips in {safedocs}; run `tools/eval.py fetch --only safedocs` first")
    gate = REPO / "eval" / "census"
    script = ["python3", "/gate/pages.py", "--jobs", str(args.jobs)]
    H.run(H.docker_cmd(H.tag("producers"), script + ["--zips", "/data", "--out", "/out/safedocs-pages.jsonl"],
                       mounts=[(gate, "/gate", True), (safedocs, "/data", True), (out, "/out", False)]))
    H.run(H.docker_cmd(H.tag("producers"),
                       script + ["--pdfs", "/scans/old_scans", "/scans/old_scans_math",
                                 "--out", "/out/old-scans-pages.jsonl"],
                       mounts=[(gate, "/gate", True), (scans, "/scans", True), (out, "/out", False)]))
    return 0


def cmd_collect_rayopdf(args) -> int:
    binary = args.bin.resolve()
    if not binary.is_file():
        raise SystemExit(f"no binary at {binary}; build it with `python3 tools/dev.py build`")
    out = args.root / "ocr-gate"
    out.mkdir(parents=True, exist_ok=True)
    census_dir = args.root / "census"
    safedocs = args.root / "data" / "safedocs"
    if not (census_dir / "census.jsonl").is_file():
        raise SystemExit(f"{census_dir / 'census.jsonl'} is missing; run `tools/eval.py census` first")
    mounts = [(REPO / "eval" / "census", "/gate", True), (binary.parent, "/opt/tool", True),
              (census_dir, "/census", True), (safedocs, "/data", True), (out, "/out", False)]
    version = H.run(H.docker_cmd(BUILD_IMAGE, [f"/opt/tool/{binary.name}", "--version"], mounts=mounts),
                    capture=True).stdout.strip()
    H.run(H.docker_cmd(BUILD_IMAGE, ["python3", "/gate/rayopdf_pages.py", "--bin", f"/opt/tool/{binary.name}",
                                     "--census", "/census/census.jsonl", "--zips", "/data",
                                     "--out", "/out/rayopdf-pages.jsonl", "--jobs", str(args.jobs)],
                       mounts=mounts))
    H.write_json(out / "rayopdf-pages.meta.json", {
        "binary": str(binary.relative_to(REPO)) if binary.is_relative_to(REPO) else str(binary),
        "version": version, "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "image": BUILD_IMAGE, "args": ["--serve", "--json", "--pages=N-N", "--images=none"]})
    return 0


# ------------------------------------------------------------------------------- census side


def load_pages(root: Path) -> tuple[list[dict], dict]:
    """One record per census page, joined with RayoPDF's own class and the image areas."""
    ours_path = root / "ocr-gate" / "rayopdf-pages.jsonl"
    ours = {(r["file"], r["page"]): r for r in H.read_jsonl(ours_path)}
    if not ours:
        raise SystemExit(f"{ours_path} is missing; run `tools/ocr_gate.py collect-rayopdf` first")
    coverage = {}
    cov_path = root / "ocr-gate" / "safedocs-pages.jsonl"
    for rec in H.read_jsonl(cov_path):
        coverage[rec["file"]] = rec["pages"]
    pages, mismatch, missing = [], 0, 0
    for rec in H.read_jsonl(root / "census" / "census.jsonl"):
        cov = coverage.get(rec["file"])
        for i, (cls, script) in enumerate(zip(rec["page_classes"], rec["page_scripts"]), start=1):
            images = None
            if cov is not None and i <= len(cov):
                images = cov[i - 1]["images"]
                mismatch += cov[i - 1]["class"] != cls
            mine = ours.get((rec["file"], i))
            missing += mine is None
            pages.append({"file": rec["file"], "family": rec.get("family") or "unknown", "page": i,
                          "pdftotext": cls, "script": script,
                          "ours": mine["class"] if mine else "no_text",
                          "ours_checks": mine["checks"] if mine else [],
                          "ours_broken": bool(mine) and mine["class"] == "broken",
                          "ours_status": mine["status"] if mine else "missing",
                          "glyphs": mine["glyphs"] if mine else 0, "unmapped": mine["unmapped"] if mine else 0,
                          "guessed": mine["guessed"] if mine else 0,
                          "unmapped_comment": bool(mine) and mine["unmapped_comment"],
                          "images": images})
    meta = {"rayopdf_pages_file": str(ours_path.relative_to(REPO)),
            "rayopdf": H.read_json(root / "ocr-gate" / "rayopdf-pages.meta.json"),
            "rayopdf_pages_missing": missing,
            "coverage_file": str(cov_path.relative_to(REPO)),
            "coverage_pages": sum(1 for p in pages if p["images"] is not None),
            "coverage_class_mismatches": mismatch}
    return pages, meta


def region_equiv(p: dict) -> float:
    """Page-equivalents of possibly text-bearing image regions on a good digital page."""
    if p["pdftotext"] != "text" or p["ours_broken"] or not p["images"]:
        return 0.0
    return min(1.0, sum(f for f in p["images"] if f >= REGION_MIN_IMAGE))


def residual(p: dict) -> bool:
    """Text-layer page RayoPDF cannot read: its text fires a check, or the pdftotext text was
    broken and RayoPDF writes fewer than 50 characters."""
    return p["ours_broken"] or (p["pdftotext"] == "broken" and p["ours"] == "no_text")


def in_r_core(p: dict) -> bool:
    return p["pdftotext"] == "image_only" or residual(p)


def census_group(group: list[dict]) -> dict:
    """M1, M2, M3, R for a set of pages."""
    n = len(group)
    m1 = sum(p["pdftotext"] == "image_only" for p in group)
    m2_pt = sum(p["pdftotext"] == "broken" for p in group)
    m2_ours = sum(p["ours_broken"] for p in group)
    both = sum(p["pdftotext"] == "broken" and p["ours_broken"] for p in group)
    ours_only = m2_ours - both
    fixed = sum(p["pdftotext"] == "broken" and p["ours"] == "text" for p in group)
    no_text_on_broken = sum(p["pdftotext"] == "broken" and p["ours"] == "no_text" for p in group)
    residual_text = sum(residual(p) for p in group)
    regions = sum(region_equiv(p) for p in group)
    region_pages = sum(region_equiv(p) > 0 for p in group)
    have_cov = all(p["images"] is not None for p in group)
    r_core = sum(in_r_core(p) for p in group)
    docs = collections.defaultdict(lambda: collections.Counter())
    for p in group:
        d = docs[p["file"]]
        d["m1"] += p["pdftotext"] == "image_only"
        d["m2_pt"] += p["pdftotext"] == "broken"
        d["m2_ours"] += p["ours_broken"]
        d["r"] += in_r_core(p)
        d["region"] += region_equiv(p) > 0
        d["tover"] += p["pdftotext"] == "text_over_image"
    nd = len(docs)
    checks = collections.Counter(c for p in group if p["ours_broken"] for c in p["ours_checks"])
    return {
        "pages": n, "docs": nd,
        "M1_pages": m1, "M1": frac(m1, n),
        "M2_pdftotext_pages": m2_pt, "M2_pdftotext": frac(m2_pt, n),
        "M2_rayopdf_pages": m2_ours, "M2_rayopdf": frac(m2_ours, n),
        "both_broken": both, "rayopdf_only_broken": ours_only,
        "rayopdf_only_broken_by_pdftotext_class": dict(collections.Counter(
            p["pdftotext"] for p in group if p["ours_broken"] and p["pdftotext"] != "broken")),
        "rayopdf_checks": dict(checks),
        "rayopdf_junk_lines_only": sum(p["ours_broken"] and p["ours_checks"] == ["junk_lines"] for p in group),
        "rayopdf_classes": dict(collections.Counter(p["ours"] for p in group)),
        "rayopdf_status_not_ok": sum(p["ours_status"] != "ok" for p in group),
        "M3_fixed_pages": fixed, "M3": frac(fixed, m2_pt), "no_text_on_pdftotext_broken": no_text_on_broken,
        "residual_text_layer_pages": residual_text,
        "glyphs": sum(p["glyphs"] for p in group), "unmapped_glyphs": sum(p["unmapped"] for p in group),
        "guessed_glyphs": sum(p["guessed"] for p in group),
        "unmapped_comment_pages": sum(p["unmapped_comment"] for p in group),
        "R_core_pages": r_core, "R_core": frac(r_core, n),
        "region_pages": region_pages if have_cov else None,
        "region_page_equiv": round(regions, 2) if have_cov else None,
        "regions": frac(regions, n) if have_cov else None,
        "R_with_regions": frac(r_core + regions, n) if have_cov else None,
        "text_over_image_pages": sum(p["pdftotext"] == "text_over_image" for p in group),
        "empty_or_vector_pages": sum(p["pdftotext"] == "empty_or_vector" for p in group),
        "docs_M1": frac(sum(d["m1"] > 0 for d in docs.values()), nd),
        "docs_M2_pdftotext": frac(sum(d["m2_pt"] > 0 for d in docs.values()), nd),
        "docs_M2_rayopdf": frac(sum(d["m2_ours"] > 0 for d in docs.values()), nd),
        "docs_R_core": frac(sum(d["r"] > 0 for d in docs.values()), nd),
        "docs_regions": frac(sum(d["region"] > 0 for d in docs.values()), nd) if have_cov else None,
        "docs_text_over_image": frac(sum(d["tover"] > 0 for d in docs.values()), nd),
    }


def script_mix(group: list[dict], key) -> dict:
    counts = collections.Counter()
    for p in group:
        w = key(p)
        if w:
            counts[p["script"]] += w
    total = sum(counts.values())
    return {s: frac(counts[s], total) for s in SCRIPTS if counts[s]} | {"_page_equiv": round(total, 2)}


def old_scans(root: Path) -> dict | None:
    recs = H.read_jsonl(root / "ocr-gate" / "old-scans-pages.jsonl")
    if not recs:
        return None
    out = {}
    for cat in ("old_scans", "old_scans_math"):
        rows = [r for r in recs if r["file"].startswith(cat + "/")]
        classes = collections.Counter(p["class"] for r in rows for p in r["pages"])
        n = sum(classes.values())
        tests = H.read_jsonl(root / "data" / "olmOCR-bench" / "bench_data" / f"{cat}.jsonl")
        out[cat] = {"pdfs": len(rows), "pages": n, "tests": len(tests),
                    "tests_by_type": dict(collections.Counter(t.get("type") for t in tests)),
                    "classes": {c: frac(classes[c], n) for c in sorted(classes)},
                    "no_usable_text_layer": frac(classes["image_only"] + classes["empty_or_vector"]
                                                 + classes["broken"], n)}
    return out


# ------------------------------------------------------------------------------- Q side


def bootstrap_ci(deltas: list[float], seed: int = 0, rounds: int = 10000) -> tuple[float, float] | None:
    if len(deltas) < 2:
        return None
    rng = random.Random(seed)
    n = len(deltas)
    means = sorted(sum(rng.choice(deltas) for _ in range(n)) / n for _ in range(rounds))
    return means[int(0.025 * rounds)], means[int(0.975 * rounds) - 1]


def odl_q(root: Path, tool: str, ml: str) -> dict:
    """Paired odl deltas (ml - tool) per metric, over documents where both have the metric."""
    docs = {}
    for t in (tool, ml):
        data = H.read_json(root / "scores" / "odl" / "work" / t / "evaluation.json")
        if not data:
            raise SystemExit(f"missing odl scores for {t}")
        docs[t] = {d["document_id"]: d.get("scores") or {} for d in data["documents"]}
        docs[t + ".means"] = data["metrics"]["score"]
    out = {}
    for m in ("nid", "teds", "mhs"):
        ids = sorted(i for i in docs[tool] if docs[tool][i].get(m) is not None
                     and docs[ml].get(i, {}).get(m) is not None)
        a = [docs[tool][i][m] for i in ids]
        b = [docs[ml][i][m] for i in ids]
        deltas = [y - x for x, y in zip(a, b)]
        out[m] = {"docs": len(ids), tool: H.mean(a), ml: H.mean(b), "delta": H.mean(deltas),
                  "ci95": bootstrap_ci(deltas),
                  "evaluator_mean_" + tool: docs[tool + ".means"].get(f"{m}_mean"),
                  "evaluator_mean_" + ml: docs[ml + ".means"].get(f"{m}_mean")}
    return out


def synthetic_q(root: Path, tool: str, ml: str) -> dict:
    """Class-macro means as in the harness report (development items, holdout excluded)."""
    out = {}
    data = {t: H.read_json(root / "scores" / "synthetic" / f"{t}.json") for t in (tool, ml)}
    for t, d in data.items():
        if not d:
            raise SystemExit(f"missing synthetic scores for {t}")
    for tagged in (False, True):
        label = "tagged" if tagged else "untagged"
        row = {}
        for t, d in data.items():
            dev = [i for i in d["items"] if not i["holdout"] and bool(i["tagged"]) == tagged]
            per_class = {c: H.odl_means([i for i in dev if i["class"] == c]) for c in H.CLASSES
                         if any(i["class"] == c for i in dev)}
            row[t] = {m: H.mean([v[m] for v in per_class.values() if v[m] is not None])
                      for m in ("nid", "teds", "mhs")}
            row[t]["items"] = len(dev)
            row[t]["teds_items"] = sum(1 for i in dev if (i.get("odl") or {}).get("teds") is not None)
        row["delta"] = {m: (row[ml][m] - row[tool][m]) if row[ml][m] is not None and row[tool][m] is not None
                        else None for m in ("nid", "teds", "mhs")}
        out[label] = row
    return out


def olmocr_q(root: Path, tool: str, ml: str) -> dict:
    rates = {}
    for t in (tool, ml):
        path = root / "scores" / "olmocr" / f"{t}.log"
        rates[t] = H.parse_olmocr_log(path.read_text(encoding="utf-8", errors="replace")) if path.is_file() else {}
    out = {}
    for key in sorted(set(rates[tool]) | set(rates[ml])):
        a, b = rates[tool].get(key), rates[ml].get(key)
        out[key] = {tool: a, ml: b, "delta": (b - a) if a is not None and b is not None else None}
    return out


def speed(root: Path, ml: str) -> dict:
    out = {}
    for label, path in (("cli images written (default)", root / "speed" / "cli.json"),
                        ("cli --images=none", root / "speed-cli-noimages.json"),
                        (f"{ml} (layout + table ML, no OCR)", root / "speed" / f"{ml}.json")):
        d = H.read_json(path)
        if d:
            out[label] = {k: d.get(k) for k in ("date", "timing", "cpu", "pages", "pages_per_s",
                                                "ms_per_page_median", "ms_per_page_p95", "cold_start_s",
                                                "peak_rss_kib")} | {"file": str(path.relative_to(REPO))}
    return out


# ------------------------------------------------------------------------------- decision


def decide(r: float, mix: dict, q: dict) -> dict:
    latin_cyr = (mix.get("latin") or 0) + (mix.get("cyrillic") or 0)
    known = 1 - (mix.get("unknown") or 0)
    latin_cyr_known = latin_cyr / known if known > 0 else None
    cjk = (mix.get("cjk") or 0) > 0
    if r < R_NO_OCR:
        ocr = "(a) no OCR"
    elif r < R_LINKED and not cjk and latin_cyr_known is not None and latin_cyr_known >= LATIN_CYRILLIC_MOSTLY:
        ocr = "(b) optional external tesseract"
    else:
        ocr = "(c) linked PP-OCRv6 via ONNX Runtime minimal or ncnn"
    reasons = []
    if r >= R_LINKED:
        reasons.append(f"R {r:.4f} >= {R_LINKED}")
    if cjk:
        reasons.append("S includes CJK")
    return {"ocr": ocr, "ocr_reasons": reasons, "latin_cyrillic_share_of_known": latin_cyr_known,
            "table_ml": max(q["Q2"].values()) >= Q2_MIN,
            "layout_ml": max(q["Q1"].values()) >= Q1_MIN or max(q["Q3"].values()) >= Q3_MIN}


# ------------------------------------------------------------------------------- measure


def cmd_measure(args) -> int:
    root = args.root
    pages, meta = load_pages(root)
    overall = census_group(pages)
    families = {f: census_group([p for p in pages if p["family"] == f])
                for f in sorted({p["family"] for p in pages})}
    by_class = {c: census_group([p for p in pages if p["pdftotext"] == c])
                for c in sorted({p["pdftotext"] for p in pages})}
    s_core = script_mix(pages, lambda p: 1 if in_r_core(p) else 0)
    s_regions = script_mix(pages, region_equiv) if meta["coverage_pages"] else None
    s_all = script_mix(pages, lambda p: (1 if in_r_core(p) else 0) + region_equiv(p)) \
        if meta["coverage_pages"] else None
    # Sensitivity: without the families whose producer string marks a scanner or is empty; and
    # with pages flagged by the junk-lines check alone counted as readable (that check is stricter
    # on Markdown, which writes a paragraph as one line).
    no_scan_families = census_group([p for p in pages if p["family"] not in ("scanner-ocr", "unknown")])
    junk_lines_only = sum(in_r_core(p) and p["pdftotext"] != "image_only" and p["ours_checks"] == ["junk_lines"]
                          for p in pages)
    sensitivity = {
        "R_core_without_scanner_ocr_and_unknown": no_scan_families["R_core"],
        "R_with_regions_without_scanner_ocr_and_unknown": no_scan_families["R_with_regions"],
        "pages_without_scanner_ocr_and_unknown": no_scan_families["pages"],
        "M1_without_scanner_ocr_and_unknown": no_scan_families["M1"],
        "R_core_pages_flagged_by_junk_lines_alone": junk_lines_only,
        "R_core_without_junk_lines_alone": frac(overall["R_core_pages"] - junk_lines_only, overall["pages"]),
    }

    odl = odl_q(root, args.tool, args.ml)
    syn = synthetic_q(root, args.tool, args.ml)
    olm = olmocr_q(root, args.tool, args.ml)
    q = {"Q1": {"odl": odl["nid"]["delta"], "synthetic untagged": syn["untagged"]["delta"]["nid"],
                "synthetic tagged": syn["tagged"]["delta"]["nid"]},
         "Q2": {"odl (table docs)": odl["teds"]["delta"], "synthetic untagged": syn["untagged"]["delta"]["teds"],
                "synthetic tagged": syn["tagged"]["delta"]["teds"],
                "olmOCR table_tests": (olm.get("table_tests") or {}).get("delta")},
         "Q3": {"odl": odl["mhs"]["delta"], "synthetic untagged": syn["untagged"]["delta"]["mhs"],
                "synthetic tagged": syn["tagged"]["delta"]["mhs"]}}
    q = {k: {v: d for v, d in views.items() if d is not None} for k, views in q.items()}
    r_for_gate = overall["R_with_regions"] if overall["R_with_regions"] is not None else overall["R_core"]
    decision = decide(overall["R_core"], s_core, q)
    decision_upper = decide(r_for_gate, s_all or s_core, q)

    result = {
        "schema": 1,
        "inputs": {"census": str((root / "census" / "census.jsonl").relative_to(REPO)),
                   **meta,
                   "odl": [str((root / "scores" / "odl" / "work" / t / "evaluation.json").relative_to(REPO))
                           for t in (args.tool, args.ml)],
                   "synthetic": [str((root / "scores" / "synthetic" / f"{t}.json").relative_to(REPO))
                                 for t in (args.tool, args.ml)],
                   "olmocr": [str((root / "scores" / "olmocr" / f"{t}.log").relative_to(REPO))
                              for t in (args.tool, args.ml)]},
        "rules": {"region_min_image": REGION_MIN_IMAGE, "R_no_ocr": R_NO_OCR, "R_linked": R_LINKED,
                  "Q1_min": Q1_MIN, "Q2_min": Q2_MIN, "Q3_min": Q3_MIN,
                  "Q_sign": f"{args.ml} minus {args.tool}; positive means the ML pipeline is ahead"},
        "census": {"overall": overall, "by_family": families, "by_pdftotext_class": by_class,
                   "S_core": s_core, "S_regions": s_regions, "S_with_regions": s_all,
                   "sensitivity": sensitivity},
        "old_scans": old_scans(root),
        "Q": q, "odl": odl, "synthetic": syn, "olmocr": olm,
        "T": {"measured": speed(root, args.ml), "ocr_claimed": OCR_COST_CLAIMED},
        "decision_R_core": decision, "decision_R_with_regions": decision_upper,
    }
    H.write_json(root / "ocr-gate" / "gate.json", result)
    print_report(result, args)
    return 0


def print_report(res: dict, args) -> None:
    c = res["census"]["overall"]
    print(f"# OCR/ML gate ({args.tool} vs {args.ml})\n")
    print(f"census: {c['pages']} pages, {c['docs']} documents; coverage file pages "
          f"{res['inputs']['coverage_pages']}, class mismatches {res['inputs']['coverage_class_mismatches']}; "
          f"RayoPDF {res['inputs']['rayopdf']}, pages missing {res['inputs']['rayopdf_pages_missing']}\n")
    keys = [("M1 image_only", "M1_pages", "M1"), ("M2 pdftotext broken", "M2_pdftotext_pages", "M2_pdftotext"),
            ("M2 RayoPDF broken", "M2_rayopdf_pages", "M2_rayopdf"), ("M3 fixed (of M2 pdftotext)", "M3_fixed_pages", "M3"),
            ("R core = M1 + RayoPDF residual", "R_core_pages", "R_core"),
            ("image regions (upper bound)", "region_page_equiv", "regions"),
            ("R with regions", None, "R_with_regions")]
    print("| measure | pages | rate |\n|---|---|---|")
    for label, n, r in keys:
        print(f"| {label} | {c[n] if n else '–'} | {pct(c[r])} |")
    print(f"\nresidual text-layer pages {c['residual_text_layer_pages']} (RayoPDF broken {c['M2_rayopdf_pages']} "
          f"+ no text on pdftotext-broken {c['no_text_on_pdftotext_broken']}); "
          f"both {c['both_broken']}, RayoPDF only {c['rayopdf_only_broken']} "
          f"{c['rayopdf_only_broken_by_pdftotext_class']}; checks {c['rayopdf_checks']}, junk_lines alone "
          f"{c['rayopdf_junk_lines_only']}; RayoPDF classes {c['rayopdf_classes']}, status not ok "
          f"{c['rayopdf_status_not_ok']}; glyphs {c['glyphs']}, unmapped {c['unmapped_glyphs']}, guessed "
          f"{c['guessed_glyphs']}, unmapped-comment pages {c['unmapped_comment_pages']}; pages with a counted "
          f"image region {c['region_pages']}; text_over_image {c['text_over_image_pages']}; empty_or_vector "
          f"{c['empty_or_vector_pages']}")
    print(f"documents: >=1 image_only {pct(c['docs_M1'])}, >=1 pdftotext broken {pct(c['docs_M2_pdftotext'])}, "
          f">=1 RayoPDF broken {pct(c['docs_M2_rayopdf'])}, >=1 R-core page {pct(c['docs_R_core'])}, "
          f">=1 region {pct(c['docs_regions'])}, >=1 text_over_image {pct(c['docs_text_over_image'])}")
    print("sensitivity: " + ", ".join(f"{k} {v if isinstance(v, int) else pct(v)}"
                                      for k, v in res["census"]["sensitivity"].items()) + "\n")
    print("| family | docs | pages | M1 | M2 pdftotext | M2 RayoPDF | M3 | R core | regions | R with regions | docs with R-core page |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    for f, g in res["census"]["by_family"].items():
        print(f"| {f} | {g['docs']} | {g['pages']} | {pct(g['M1'], 1)} | {pct(g['M2_pdftotext'], 1)} | "
              f"{pct(g['M2_rayopdf'], 1)} | {pct(g['M3'], 1)} | {pct(g['R_core'], 1)} | {pct(g['regions'], 1)} | "
              f"{pct(g['R_with_regions'], 1)} | {pct(g['docs_R_core'], 1)} |")
    for label in ("S_core", "S_regions", "S_with_regions"):
        mix = res["census"][label]
        if mix:
            print(f"\n{label}: " + ", ".join(f"{k} {pct(v, 1)} ({v * mix['_page_equiv']:.1f})"
                                             for k, v in mix.items() if not k.startswith("_"))
                  + f" (of {mix['_page_equiv']} page-equivalents)")
    if res["old_scans"]:
        print("\nolmOCR scanned categories (census classifier):")
        for cat, d in res["old_scans"].items():
            print(f"  {cat}: {d['pdfs']} PDFs, {d['pages']} pages, {d['tests']} tests {d['tests_by_type']}; "
                  + ", ".join(f"{k} {pct(v, 1)}" for k, v in d["classes"].items())
                  + f"; no usable text layer {pct(d['no_usable_text_layer'], 1)}")
    print("\nodl (paired documents; delta = ML - rule-based, bootstrap 95 % CI):")
    for m, d in res["odl"].items():
        ci = d["ci95"]
        print(f"  {m}: {d['docs']} docs, {args.tool} {d[args.tool]:.4f}, {args.ml} {d[args.ml]:.4f}, "
              f"delta {d['delta']:+.4f}" + (f" [{ci[0]:+.4f}, {ci[1]:+.4f}]" if ci else ""))
    print("synthetic (class macro, development):")
    for label, row in res["synthetic"].items():
        print(f"  {label}: " + "; ".join(
            f"{m} {row[args.tool][m]:.4f} vs {row[args.ml][m]:.4f} ({row['delta'][m]:+.4f})"
            for m in ("nid", "teds", "mhs") if row["delta"][m] is not None)
              + f"; items {row[args.tool]['items']}, TEDS items {row[args.tool]['teds_items']}")
    print("olmOCR pass rates:")
    for k, d in res["olmocr"].items():
        print(f"  {k}: {num(d[args.tool])} vs {num(d[args.ml])} ({num(d['delta'], sign=True)})")
    print("\nQ (max view decides):")
    for k, views in res["Q"].items():
        print(f"  {k}: " + ", ".join(f"{v} {d:+.4f}" for v, d in views.items()))
    print("\nT measured:")
    for label, d in res["T"]["measured"].items():
        print(f"  {label}: {d['ms_per_page_median']:.2f} ms/page median, p95 {d['ms_per_page_p95']:.2f}, "
              f"{d['pages_per_s']:.2f} pages/s, {d['timing']}, {d['cpu']}, {d['date']} ({d['file']})")
    print("T OCR (claimed): " + "; ".join(f"{a}: {b} ({s})" for a, b, s in res["T"]["ocr_claimed"]))
    for key in ("decision_R_core", "decision_R_with_regions"):
        d = res[key]
        print(f"\n{key}: OCR {d['ocr']} ({', '.join(d['ocr_reasons']) or 'thresholds'}); "
              f"Latin+Cyrillic share of known-script R {pct(d['latin_cyrillic_share_of_known'], 1)}; "
              f"table ML {'yes' if d['table_ml'] else 'no'}; reading-order/heading ML {'yes' if d['layout_ml'] else 'no'}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", type=Path, default=H.DEFAULT_ROOT, help="harness output root")
    sub = ap.add_subparsers(dest="command", required=True)
    p = sub.add_parser("collect", help="per-page image areas (SafeDocs) and old_scans classes, in Docker")
    p.add_argument("--jobs", type=int, default=6)
    p.set_defaults(func=cmd_collect)
    p = sub.add_parser("collect-rayopdf", help="RayoPDF's own broken-page check on every census page, in Docker")
    p.add_argument("--bin", type=Path, default=DEFAULT_BIN, help="rayopdf binary (default build/static/rayopdf)")
    p.add_argument("--jobs", type=int, default=6)
    p.set_defaults(func=cmd_collect_rayopdf)
    p = sub.add_parser("measure", help="compute the gate numbers from harness outputs")
    p.add_argument("--tool", default="cli", help="rule-based tool (default cli = RayoPDF)")
    p.add_argument("--ml", default="docling", help="ML reference tool (default docling)")
    p.set_defaults(func=cmd_measure)
    args = ap.parse_args()
    args.root = args.root.resolve()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
