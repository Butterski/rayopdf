# Release check of 0.1.0, 2026-11-12

RayoPDF 0.1.0 (code `c1116f0`, frozen; the commits after it change only `tools/fuzz.py`, VERSION
and documents) against the release bar of
[roadmap.md](../development/roadmap.md#goal-and-release-bar). Quality numbers are those of
[v0.1.0-2026-11-12.md](v0.1.0-2026-11-12.md); speed, security and footprint were measured for
this check. Same machine and harness as [the baselines](baselines-2026-11-05.md).

**Result: every item of the bar holds except one key, READoc arXiv `table_teds`, and 0.1.0 is
released with that key named as open, by the user's decision of 2026-11-11.**

| item | status | detail |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | met | every odl, synthetic, olmOCR and READoc GitHub metric; READoc arXiv `text_eds`, `head_teds`, `seg_kt`, `inline_eds` (LaTeX mode, D6) |
| (b) TEDS ≥ docling | met except one key | odl, synthetic, olmOCR met; READoc arXiv `table_teds` 0.617 against docling's 0.656: **open** |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | met | 0.931 ms fresh, 2,209 pages/s warm (CPU 3) |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | 24 targets, 25.1 h aggregate, clean; ASan sweeps of 2,802 PDFs in three modes clean |
| (e) binary size, package size, peak RSS per document reported | met | below |

## (a) Quality

Core alone, default mode unless named (READoc arXiv in LaTeX mode, as decided 2026-11-11):

| metric | RayoPDF 0.1.0 | best text-layer tool | docling |
|---|---|---|---|
| odl overall / NID / TEDS / MHS | 0.896 / 0.917 / 0.889 / 0.840 | 0.885 / 0.914 / 0.818 / 0.822 | 0.884 / 0.902 / 0.879 / 0.827 |
| synthetic untagged NID / TEDS / MHS / construct F1 | 0.960 / 0.987 / 0.944 / 0.969 | 0.920 / 0.845 / 0.845 / 0.628 | 0.925 / 0.870 / 0.802 / 0.560 |
| synthetic tagged NID / TEDS / MHS / construct F1 | 0.972 / 0.978 / 0.933 / 0.951 | – / 0.775 / – / 0.594 | – / 0.881 / – / 0.599 |
| olmOCR headers_footers / long_tiny_text / multi_column / table_tests | 0.841 / 0.509 / 0.709 / 0.632 | 0.554 / 0.471 / 0.667 / 0.615 | 0.898 / 0.523 / 0.695 / 0.610 |
| olmOCR arxiv_math (LaTeX mode) | 0.720 | 0.538 | 0.000 |
| READoc GitHub (holdout) text_eds / head_teds / seg_kt | 0.936 / 0.881 / 0.997 | 0.874 / 0.778 / 0.990 | 0.908 / 0.471 / 0.993 |
| READoc arXiv (holdout, LaTeX mode) text_eds / head_teds / seg_kt / inline_eds | 0.882 / 0.693 / 0.981 / 0.654 | 0.826 / 0.515 / 0.976 / 0.369 | 0.841 / 0.403 / 0.984 / 0.001 |
| READoc arXiv (holdout, LaTeX mode) table_teds | 0.617 | 0.685 | 0.656 |

## (b) Tables

TEDS against docling's: odl 0.889 against 0.879; synthetic untagged 0.987 against 0.870, tagged
0.978 against 0.881; olmOCR `table_tests` 0.632 against 0.610. READoc arXiv `table_teds` (held to
docling's number by the user's decision of 2026-11-11) is 0.617 against 0.656. On the READoc
validation split (300 other arXiv papers, aggregate only) RayoPDF scores 0.577 and docling 0.640.
What is left: tables with a body cell spanning rows are written as HTML (decision D6), which
READoc scores 0 (writing them as pipe tables: validation +0.009, odl TEDS 0.889 -> 0.882); a `%`
or `&` in a cell breaks READoc's own LaTeX conversion for every tool; the rest is spread over
single papers (missed header rows, merged columns). The table-ML branch of the M8 gate stays
untaken.

## (c) Speed

CPU 3, 0.0.12 and 0.1.0 interleaved, 2026-11-11 22:35 UTC (load about 2):

| measure | 0.1.0 | bar |
|---|---|---|
| fresh-process median per odl document (native `posix_spawn` timer, 12 rotated rounds, `--images=none`) | 0.931 ms (0.0.12: 0.907) | ≤ 1 ms/page |
| warm throughput over the 200 odl pages (`--bench`, best of 3) | 2,209 pages/s (0.0.12: 2,189) | ≥ 2,000 pages/s |
| warm median / p95 per page | 0.371 / 0.692 ms | – |

An empty static program takes 0.22 ms in the same timer.

## (d) Security

**Fuzzing:** 24 libFuzzer targets, one per parser entry point (the 22 of the last check, plus
`fuzz_layout_math` and `fuzz_layout_pages`, which converts with several page threads and compares
with one), ASan and UBSan, 3,763-3,764 s each on the frozen code, twelve at a time
(90,314 s, 25.1 h aggregate): 24 of 24 clean, no new unit.

|target|runs|target|runs|
|---|---|---|---|
|common_ocr_protocol|85.8 M|text_cjk|764 k|
|pdf_exact|36.0 M|text_bidi|706 k|
|pdf_filters|32.9 M|layout_structure|646 k|
|image_ccitt|24.4 M|image_jpeg|569 k|
|text_repair|20.6 M|layout_tables|398 k|
|common_serve|12.7 M|image_jbig2|268 k|
|text_outline|10.5 M|pdf_document|179 k|
|layout_pages|2.4 M|text_cmap|146 k|
|text_program|2.0 M|ocr_server|127 k|
|layout_math|1.7 M|crypt_handler|106 k|
|image_jpx|1.1 M|image_decode|87 k|
|layout_content|851 k|layout_document|37 k|

Every committed regression input replays clean (`python3 tools/fuzz.py replay`). The campaign's
own summary said 16 of 24: the eight others listed units left in `build/fuzz-artifacts/` by the
0.0.13 campaign (written 2026-11-11 04:19-04:50 UTC; six were starvation on an overloaded machine,
two real slow inputs fixed in `52244c2`); `tools/fuzz.py` now counts only a run's own units
(`e3bf300`).

**ASan sweeps:** an ASan + UBSan build converted 2,802 PDFs (synthetic corpus, odl, olmOCR
born-digital, SafeDocs census sample) with images written and a 120 s cap per file, in three modes
(default, `--workers=1`, `--math=latex`): 0 crashes, 0 hangs, 0 sanitizer reports in each.
Fail-closed rejections: 1 password required (exit 11) and 3 corrupt beyond repair or not a PDF
(exit 10), all in SafeDocs.

## (e) Footprint

Archives as `tools/package.py` builds them from the tagged commit (byte for byte the same on a
second run; the core archives are rebuilt by the Release workflow with the same bytes):

| artifact | Linux x64 (static) | Windows x64 |
|---|---|---|
| `rayopdf` | 3,465,072 B | 4,448,726 B |
| `rayopdf-ocr` (add-on) | 7,688,848 B | 7,883,513 B |
| core archive | 1,746,036 B | 1,701,139 B |
| add-on archive (with small, tiny and orientation models) | 36,073,338 B | 35,913,497 B |
| script pack archive (optional) | 21,102,214 B | 21,100,233 B |

Peak RSS per document of `rayopdf --convert` (static), measured with `wait4` from a small parent,
add-on not installed:

| documents | images off: median / p95 / max | images written: median / p95 / max |
|---|---|---|
| odl (200) | 1.8 / 2.2 / 9.7 MiB | 2.2 / 4.7 / 17.3 MiB |
| olmOCR born-digital (1,403) | 2.1 / 3.0 / 37.2 MiB | 2.4 / 5.1 / 38.4 MiB |
| synthetic development (181) | 1.5 / 3.4 / 4.0 MiB | 1.7 / 5.6 / 6.8 MiB |
| scanned development (362) | 1.5 / 1.5 / 1.8 MiB | 5.1 / 130.5 / 525.1 MiB |
| SafeDocs sample (1,000) | 3.0 / 15.8 / 299.9 MiB | 4.3 / 31.9 / 305.1 MiB |

The largest values are the input file itself (mapped into memory) and JPEG 2000 and JPEG images,
which are decoded whole; since 0.0.13 a document's images are decoded on up to four threads, so a
scanned document with several large JPEG 2000 pages holds up to four of them at once (the scanned
maximum went from 142.5 MiB at 0.0.10 to 525.1 MiB). The OCR add-on, when a page needs it, peaks
at about 0.6 GB.

## Builds

Release, ASan + UBSan, ThreadSanitizer, Windows (Wine), OCR and OCR Windows: core tests pass (24,
25 with the add-on), 0 warnings; debug and static builds 0 warnings; every generated table matches
its generator (`gen_{text,cjk,bidi,math}_tables.py --check`); `tools/check_repo.py` 0 findings.
