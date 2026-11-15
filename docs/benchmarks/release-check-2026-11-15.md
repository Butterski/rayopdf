# Release check of 0.4.0, 2026-11-15

RayoPDF 0.4.0 (code `a1db445`, frozen; the commit after it changes only documents) against the
release bar of [roadmap.md](../development/roadmap.md#goal-and-release-bar). Quality numbers are
those of [v0.4.0-2026-11-15.md](v0.4.0-2026-11-15.md), measured on this code; speed, footprint,
the ASan sweeps and the fuzz campaign too. Same machine and harness as
[the baselines](baselines-2026-11-05.md).

**Result: every item of the bar holds except READoc arXiv `table_teds`, open as in
[0.3.0](release-check-2026-11-14.md) by the user's decision of 2026-11-11, its gap to docling
halved in READoc's declared options.**

| item | status | detail |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | met | every odl, synthetic, olmOCR and READoc GitHub metric; READoc arXiv `text_eds`, `head_teds`, `seg_kt`, `inline_eds` (LaTeX mode, D6) |
| (b) TEDS ≥ docling | met except one key | odl, synthetic, olmOCR met; READoc arXiv `table_teds` 0.618 in LaTeX mode, 0.636 in READoc's declared options (`--math=latex --tables=pipe`), against docling's 0.656: **open** |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | met | 0.906 ms fresh, 2,188 pages/s warm |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | 25 targets, 26.1 h aggregate, no finding; ASan sweeps of 2,837 PDFs in three modes clean |
| (e) binary size, package size, peak RSS per document reported | met | below |

## (a) Quality

Core alone, default mode unless named (READoc arXiv in LaTeX mode, as decided 2026-11-11):

| metric | RayoPDF 0.4.0 | best text-layer tool | docling |
|---|---|---|---|
| odl overall / NID / TEDS / MHS | 0.916 / 0.925 / 0.915 / 0.891 | 0.885 / 0.914 / 0.818 / 0.822 | 0.884 / 0.902 / 0.879 / 0.827 |
| odl, its declared options (`--images=none --furniture=keep --plain --tables=html --quotes=ascii`) | 0.941 / 0.954 / 0.929 / 0.917 | 0.885 / 0.914 / 0.818 / 0.822 | 0.884 / 0.902 / 0.879 / 0.827 |
| synthetic untagged NID / TEDS / MHS / construct F1 (the 11 producers of the baselines) | 0.960 / 0.986 / 0.942 / 0.969 | 0.920 / 0.845 / 0.845 / 0.628 | 0.925 / 0.870 / 0.802 / 0.560 |
| synthetic tagged NID / TEDS / MHS / construct F1 | 0.972 / 0.978 / 0.933 / 0.952 | – / 0.775 / – / 0.594 | – / 0.881 / – / 0.599 |
| olmOCR headers_footers / long_tiny_text / multi_column / table_tests | 0.850 / 0.509 / 0.710 / 0.631 | 0.554 / 0.471 / 0.667 / 0.615 | 0.898 / 0.523 / 0.695 / 0.610 |
| olmOCR arxiv_math (LaTeX mode) | 0.721 | 0.538 | 0.000 |
| READoc GitHub (holdout) text_eds / head_teds / seg_kt | 0.936 / 0.882 / 0.997 | 0.874 / 0.778 / 0.990 | 0.908 / 0.471 / 0.993 |
| READoc arXiv (holdout, LaTeX mode) text_eds / head_teds / seg_kt / inline_eds | 0.882 / 0.697 / 0.981 / 0.654 | 0.826 / 0.515 / 0.976 / 0.369 | 0.841 / 0.403 / 0.984 / 0.001 |
| READoc arXiv (holdout) table_teds, LaTeX mode / declared options | 0.618 / 0.636 | 0.685 | 0.656 |

The two untagged producers added in 0.3.0 (chromium-untagged and context, 35 development items),
on which no other tool has run: NID 0.964, TEDS 0.889, MHS 0.929, construct F1 0.849 (0.3.0:
0.956, 0.715, 0.919, 0.847).

## (b) Tables

TEDS against docling's: odl 0.915 (0.929 in its declared options) against 0.879; synthetic
untagged 0.986 against 0.870, tagged 0.978 against 0.881; olmOCR `table_tests` 0.631 against
0.610. READoc arXiv `table_teds` is 0.618 in LaTeX mode against docling's 0.656 on the holdout, as
in 0.3.0; written as pipe tables (`--tables=pipe`, READoc's declared options since 0.4.0: its
evaluator reads only pipe tables) it is 0.636. Of the development documents that trail docling's
by 0.1 or more, 26 of 66 hold a table READoc's evaluator cannot read (a `%` or `&` in a cell,
passed unescaped into LaTeX), as docling's tables do in about as many
([evaluation.md](../development/evaluation.md#readoc)).

## (c) Speed

CPU 3, 0.3.0 and 0.4.0 interleaved, 2026-11-15 13:39-13:44 UTC (nothing of this project running):

| measure | 0.4.0 | bar |
|---|---|---|
| fresh-process median per odl document (native `posix_spawn` timer, 12 rotated rounds, `--images=none`) | 0.906 ms (0.3.0: 0.896) | ≤ 1 ms/page |
| warm throughput over the 200 odl pages (`--bench`, best of 3; median of three runs) | 2,188 pages/s (0.3.0: 2,212) | ≥ 2,000 pages/s |
| warm median / p95 per page | 0.374 / 0.715 ms | – |

The release binary is the one the Release workflow builds on GitHub's runners: a dry run of the
workflow on the release's code with the version raised (`4050fc0`, run 43756933759) built the
same bytes as a local build of that commit (SHA-256 `1263a738…`). Those bytes differ from the
binary timed here (`a1db445`) in 24 bytes: the version string, `0.3.0` → `0.4.0` in its five
places, and the build ID that hashes the file; so these times hold for it.

## (d) Security

**Fuzzing:** 25 libFuzzer targets, one per parser entry point (the 25 of the last check), ASan and
UBSan, 3,761-3,775 s each on `a1db445` (94,074 s, 26.1 h aggregate):

|target|runs|target|runs|
|---|---|---|---|
|common_ocr_protocol|137.8 M|text_bidi|1.3 M|
|text_repair|57.0 M|image_jpx|1.0 M|
|pdf_exact|46.2 M|text_cjk|874 k|
|pdf_filters|36.3 M|image_jpeg|637 k|
|image_ccitt|30.7 M|layout_content|590 k|
|text_outline|18.8 M|layout_structure|494 k|
|common_serve|16.8 M|layout_tables|349 k|
|image_jbig2|5.8 M|image_decode|259 k|
|layout_imagerules|2.6 M|pdf_document|224 k|
|layout_pages|2.1 M|ocr_server|130 k|
|text_program|2.0 M|crypt_handler|100 k|
|layout_math|1.4 M|text_cmap|90 k|
|||layout_document|12 k|

25 of 25 clean, no finding. The campaign ran twice: the first (2026-11-15 18:22-20:12 UTC) shared
its CPUs with builds and measurements of the next version's work, and 16 targets stopped early on
libFuzzer's 10 s timeout; every one of those inputs converts in 28 to 102 ms alone on an idle CPU,
so the machine had stalled, not the code. Those 16 ran again for the full time with nothing else
on their CPUs (20:13-22:18 UTC); the 9 that had finished clean in the first run are counted from
it. Every committed regression input replays clean on the frozen code (`python3 tools/fuzz.py
replay`).

**ASan sweeps:** an ASan + UBSan build of `a1db445` converted 2,837 PDFs (synthetic corpus, odl,
olmOCR born-digital, SafeDocs census sample) with images written and a 120 s cap per file, in
three modes (default, `--workers=1`, `--math=latex`): 0 crashes, 0 hangs, 0 sanitizer reports in
each. Fail-closed rejections: 1 password required (exit 11) and 3 corrupt beyond repair or not a
PDF (exit 10), all in SafeDocs.

## (e) Footprint

The seven core packages as the Release workflow's dry run built them on `4050fc0` (the tagged
archives differ by a few bytes: their entries' time is the tagged commit's), and the OCR add-on's
archives as `tools/package.py --ocr` builds them here:

| artifact | Linux x64 (static) | Windows x64 |
|---|---|---|
| `rayopdf` | 3,817,464 B | 4,967,487 B |
| `rayopdf-ocr` (add-on) | 7,688,848 B | 7,883,513 B |
| core archive | 1,918,644 B | 1,870,855 B |
| C++ API archive (`librayopdf.a`, header, example) | 1,756,295 B | 1,852,065 B |
| Python wheel (`librayopdf.so` or `rayopdf.dll` with the package) | 1,558,124 B | 1,838,315 B |
| add-on archive (with small, tiny and orientation models) | 36,076,816 B | 35,917,938 B |
| script pack archive (optional) | 21,107,723 B | 21,104,675 B |

The npm package, one for every platform, is 1,104,191 B (its `rayopdf-core.wasm` 2,462,680 B). The
static `rayopdf` grew from 0.3.0's 3,700,016 B to 3,817,464 B: the edge-row model's trees (about
47 KB of data), the formula tree and the new rules.

Peak RSS per document of `rayopdf --convert` (static), measured with `wait4` from a small parent,
add-on not installed:

| documents | images off: median / p95 / max | images written: median / p95 / max |
|---|---|---|
| odl (200) | 2.0 / 2.4 / 10.4 MiB | 2.3 / 5.1 / 17.9 MiB |
| olmOCR born-digital (1,403) | 2.3 / 3.1 / 37.4 MiB | 2.6 / 5.3 / 39.0 MiB |
| synthetic development (216) | 1.8 / 3.4 / 4.1 MiB | 1.8 / 6.1 / 7.0 MiB |
| scanned development (362) | 1.4 / 1.6 / 1.9 MiB | 5.3 / 130.8 / 525.4 MiB |
| SafeDocs sample (1,000) | 3.1 / 13.8 / 234.3 MiB | 4.3 / 30.9 / 238.1 MiB |

The largest values are the input file itself (mapped into memory) and JPEG 2000 and JPEG images,
which are decoded whole, up to four of a document's at once. The OCR add-on, when a page needs
it, peaks at about 0.6 GB.

## Builds

On `a1db445`: release, ASan + UBSan and ThreadSanitizer 31 of 31 tests, Windows (Wine), OCR, OCR
Windows and the wheel configuration (with `tests/verify_python.py`) 32 of 32, the WebAssembly
configuration 1 of 1 (`tests/verify_wasm.mjs` under Node.js), 0 warnings in each; the static and
debug builds, 0 warnings; every generated table matches its generator
(`gen_{text,cjk,bidi,math,model}_tables.py --check`); every archive of `tools/package.py`;
`tools/check_repo.py` 0 findings.
