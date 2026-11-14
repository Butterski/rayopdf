# Release check of 0.3.0, 2026-11-14

RayoPDF 0.3.0 (code `635ec7b`, frozen; the commit after it changes only VERSION and documents)
against the release bar of [roadmap.md](../development/roadmap.md#goal-and-release-bar). Quality
numbers are those of [v0.3.0-2026-11-14.md](v0.3.0-2026-11-14.md), measured on `6c9b541`, whose
output the two code commits after it leave byte for byte (the profile-guided static build and its
`hot.ld`: 1,302 comparisons, none differs). Speed, security and footprint were measured for this
check, on `635ec7b`. Same machine and harness as [the baselines](baselines-2026-11-05.md).

**Result: every item of the bar holds except READoc arXiv `table_teds`, open as in
[0.2.0](release-check-2026-11-13.md) by the user's decision of 2026-11-11.**

| item | status | detail |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | met | every odl, synthetic, olmOCR and READoc GitHub metric; READoc arXiv `text_eds`, `head_teds`, `seg_kt`, `inline_eds` (LaTeX mode, D6) |
| (b) TEDS ≥ docling | met except one key | odl, synthetic, olmOCR met; READoc arXiv `table_teds` 0.618 against docling's 0.656: **open** |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | met | 0.891 ms fresh, 2,226 pages/s warm |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | 25 targets, 26.1 h aggregate, no finding; ASan sweeps of 2,837 PDFs in three modes clean |
| (e) binary size, package size, peak RSS per document reported | met | below |

## (a) Quality

Core alone, default mode unless named (READoc arXiv in LaTeX mode, as decided 2026-11-11):

| metric | RayoPDF 0.3.0 | best text-layer tool | docling |
|---|---|---|---|
| odl overall / NID / TEDS / MHS | 0.915 / 0.925 / 0.915 / 0.890 | 0.885 / 0.914 / 0.818 / 0.822 | 0.884 / 0.902 / 0.879 / 0.827 |
| odl, its declared options (`--images=none --furniture=keep --plain --tables=html`) | 0.936 / 0.945 / 0.928 / 0.912 | 0.885 / 0.914 / 0.818 / 0.822 | 0.884 / 0.902 / 0.879 / 0.827 |
| synthetic untagged NID / TEDS / MHS / construct F1 (the 11 producers of the baselines) | 0.960 / 0.987 / 0.942 / 0.969 | 0.920 / 0.845 / 0.845 / 0.628 | 0.925 / 0.870 / 0.802 / 0.560 |
| synthetic tagged NID / TEDS / MHS / construct F1 | 0.972 / 0.978 / 0.933 / 0.951 | – / 0.775 / – / 0.594 | – / 0.881 / – / 0.599 |
| olmOCR headers_footers / long_tiny_text / multi_column / table_tests | 0.849 / 0.509 / 0.710 / 0.631 | 0.554 / 0.471 / 0.667 / 0.615 | 0.898 / 0.523 / 0.695 / 0.610 |
| olmOCR arxiv_math (LaTeX mode) | 0.720 | 0.538 | 0.000 |
| READoc GitHub (holdout) text_eds / head_teds / seg_kt | 0.936 / 0.882 / 0.997 | 0.874 / 0.778 / 0.990 | 0.908 / 0.471 / 0.993 |
| READoc arXiv (holdout, LaTeX mode) text_eds / head_teds / seg_kt / inline_eds | 0.882 / 0.697 / 0.981 / 0.654 | 0.826 / 0.515 / 0.976 / 0.369 | 0.841 / 0.403 / 0.984 / 0.001 |
| READoc arXiv (holdout, LaTeX mode) table_teds | 0.618 | 0.685 | 0.656 |

The synthetic corpus gained two untagged producers in 0.3.0 (chromium-untagged and context, 35
development items), on which no other tool has run; RayoPDF scores NID 0.956, TEDS 0.715, MHS
0.919 and construct F1 0.847 there, 0.2.0 the same.

## (b) Tables

TEDS against docling's: odl 0.915 (0.928 in its declared options) against 0.879; synthetic
untagged 0.987 against 0.870, tagged 0.978 against 0.881; olmOCR `table_tests` 0.631 against
0.610. READoc arXiv `table_teds` is 0.618 against docling's 0.656 on the holdout, as in 0.2.0:
this version's table steps (borderless columns chosen by dynamic programming, a fraction bar read
as a table) failed the strict gate on the development and validation splits and were not landed
([v0.3.0](v0.3.0-2026-11-14.md#tried-and-not-landed)).

## (c) Speed

CPU 3, 0.2.0 and 0.3.0 interleaved, 2026-11-14 11:10-11:17 UTC (load under 1.5):

| measure | 0.3.0 | bar |
|---|---|---|
| fresh-process median per odl document (native `posix_spawn` timer, 12 rotated rounds, `--images=none`) | 0.891 ms (0.2.0: 0.888) | ≤ 1 ms/page |
| warm throughput over the 200 odl pages (`--bench`, best of 3; median of three runs) | 2,226 pages/s (0.2.0: 2,217) | ≥ 2,000 pages/s |
| warm median / p95 per page | 0.366 / 0.700 ms | – |

## (d) Security

**Fuzzing:** 25 libFuzzer targets, one per parser entry point (the 25 of the last check), ASan and
UBSan, 3,762-3,767 s each on `635ec7b`, ten at a time (94,068 s, 26.1 h aggregate):

|target|runs|target|runs|
|---|---|---|---|
|common_ocr_protocol|79.6 M|text_bidi|679 k|
|pdf_filters|34.2 M|layout_content|640 k|
|text_repair|29.3 M|text_cjk|626 k|
|pdf_exact|26.3 M|image_jpeg|549 k|
|image_ccitt|23.5 M|layout_structure|411 k|
|text_outline|17.1 M|layout_tables|333 k|
|common_serve|13.3 M|pdf_document|212 k|
|image_jbig2|5.2 M|image_decode|185 k|
|text_program|2.4 M|text_cmap|158 k|
|layout_imagerules|2.3 M|ocr_server|134 k|
|layout_pages|1.5 M|crypt_handler|79 k|
|layout_math|1.4 M|layout_document|15 k|
|image_jpx|846 k|||

25 of 25 clean, no finding. `fuzz_layout_document` converts each input twice since 0.3.0 (to
Markdown and to one of JSON, chunks and the inspector's HTML, by its size), hence its lower count.
Every committed regression input replays clean on the frozen code (`python3 tools/fuzz.py replay`).

**ASan sweeps:** an ASan + UBSan build of `635ec7b` converted 2,837 PDFs (synthetic corpus, odl,
olmOCR born-digital, SafeDocs census sample) with images written and a 120 s cap per file, in
three modes (default, `--workers=1`, `--math=latex`): 0 crashes, 0 hangs, 0 sanitizer reports in
each. Fail-closed rejections: 1 password required (exit 11) and 3 corrupt beyond repair or not a
PDF (exit 10), all in SafeDocs.

## (e) Footprint

`tools/package.py` on the release commit's tree before it was committed (the tagged archives
differ by a few bytes: the entries' time is the commit's; the Release workflow builds the seven
core packages on the tag):

| artifact | Linux x64 (static) | Windows x64 |
|---|---|---|
| `rayopdf` | 3,996,560 B | 4,809,984 B |
| `rayopdf-ocr` (add-on) | 7,688,848 B | 7,883,513 B |
| core archive | 2,017,724 B | 1,815,202 B |
| C++ API archive (`librayopdf.a`, header, example) | 1,897,270 B | 1,784,068 B |
| Python wheel (`librayopdf.so` or `rayopdf.dll` with the package) | 1,502,853 B | 1,782,870 B |
| add-on archive (with small, tiny and orientation models) | 36,076,047 B | 35,917,363 B |
| script pack archive (optional) | 21,107,040 B | 21,104,100 B |

The npm package `rayopdf-0.3.0.tgz`, one for every platform, is 1,057,132 B (its
`rayopdf-core.wasm` 2,338,278 B). The static `rayopdf` grew from 0.2.0's 3,576,608 B to
3,700,016 B with this version's code and to 3,996,560 B with the profile-guided build.

Peak RSS per document of `rayopdf --convert` (static), measured with `wait4` from a small parent,
add-on not installed:

| documents | images off: median / p95 / max | images written: median / p95 / max |
|---|---|---|
| odl (200) | 2.1 / 2.5 / 10.6 MiB | 2.4 / 5.1 / 18.1 MiB |
| olmOCR born-digital (1,403) | 2.4 / 3.3 / 37.5 MiB | 2.7 / 5.3 / 39.0 MiB |
| synthetic development (216) | 1.8 / 3.4 / 4.0 MiB | 1.9 / 6.1 / 7.0 MiB |
| scanned development (362) | 1.6 / 1.8 / 2.1 MiB | 5.4 / 130.7 / 525.4 MiB |
| SafeDocs sample (1,000) | 3.1 / 13.9 / 234.7 MiB | 4.4 / 30.8 / 237.5 MiB |

The largest values are the input file itself (mapped into memory) and JPEG 2000 and JPEG images,
which are decoded whole, up to four of a document's at once. The medians are about 0.2 MiB above
0.2.0's; the static binary is 0.4 MB larger (0.3 MB of it from the profile-guided build). The OCR
add-on, when a page needs it, peaks at about 0.6 GB.

## Builds

On the frozen code: release, ASan + UBSan and ThreadSanitizer 30 of 30 tests, Windows (Wine), OCR
and OCR Windows 31 of 31, the wheel configuration 31 of 31 (with `tests/verify_python.py`), the
WebAssembly configuration 1 of 1 (`tests/verify_wasm.mjs` under Node.js), 0 warnings in each; the
debug build, 0 warnings; every generated table matches its generator
(`gen_{text,cjk,bidi,math}_tables.py --check`). With VERSION 0.3.0, the static, Windows, wheel,
WebAssembly, OCR and OCR Windows builds, 0 warnings, and every archive of `tools/package.py`;
`tools/check_repo.py` 0 findings.
