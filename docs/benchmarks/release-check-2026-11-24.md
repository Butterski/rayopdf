# Release check of 0.5.0, 2026-11-24

RayoPDF 0.5.0 (code `a95ad7f`, frozen; the commit after it changes only documents) against the
release bar of [roadmap.md](../development/roadmap.md#goal-and-release-bar). Quality numbers are
those of [v0.5.0-2026-11-24.md](v0.5.0-2026-11-24.md), measured on this code; speed, footprint,
the ASan sweeps and the fuzz campaign too. Same machine and harness as
[the baselines](baselines-2026-11-05.md).

**Result: every item of the bar holds. READoc arXiv `table_teds`, open since 0.3.0, is met in
READoc's declared options (0.730 against docling's 0.656 on the holdout); in LaTeX mode alone it
is 0.609.**

| item | status | detail |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | met | every odl, synthetic, olmOCR and READoc GitHub metric; READoc arXiv `text_eds`, `head_teds`, `seg_kt`, `inline_eds` (LaTeX mode, D6) |
| (b) TEDS ≥ docling | met | odl, synthetic, olmOCR; READoc arXiv `table_teds` 0.730 in READoc's declared options (`--math=latex --tables=pipe-latex --figure-text=drop`) against docling's 0.656 (0.609 in LaTeX mode) |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | met | 0.912 ms fresh, 2,207 pages/s warm |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | 25 targets, 26.1 h aggregate, no finding; ASan sweeps of 2,837 PDFs in three modes clean |
| (e) binary size, package size, peak RSS per document reported | met | below |

## (a) Quality

Core alone, default mode unless named (READoc arXiv in LaTeX mode, as decided 2026-11-11):

| metric | RayoPDF 0.5.0 | best text-layer tool | docling |
|---|---|---|---|
| odl overall / NID / TEDS / MHS | 0.917 / 0.926 / 0.915 / 0.898 | 0.885 / 0.914 / 0.818 / 0.822 | 0.884 / 0.902 / 0.879 / 0.827 |
| odl, its declared options (`--images=none --furniture=keep --plain --tables=html --quotes=ascii --small-caps=keep`) | 0.944 / 0.954 / 0.934 / 0.927 | 0.885 / 0.914 / 0.818 / 0.822 | 0.884 / 0.902 / 0.879 / 0.827 |
| synthetic untagged NID / TEDS / MHS / construct F1 (the 11 producers of the baselines) | 0.960 / 0.986 / 0.942 / 0.971 | 0.920 / 0.845 / 0.845 / 0.628 | 0.925 / 0.870 / 0.802 / 0.560 |
| synthetic tagged NID / TEDS / MHS / construct F1 | 0.972 / 0.978 / 0.933 / 0.953 | – / 0.775 / – / 0.594 | – / 0.881 / – / 0.599 |
| olmOCR headers_footers / long_tiny_text / multi_column / table_tests | 0.890 / 0.509 / 0.732 / 0.646 | 0.554 / 0.471 / 0.667 / 0.615 | 0.898 / 0.523 / 0.695 / 0.610 |
| olmOCR, its declared options (`--plain --small-caps=keep --figure-text=drop`) | 0.891 / 0.525 / 0.762 / 0.653 | 0.554 / 0.471 / 0.667 / 0.615 | 0.898 / 0.523 / 0.695 / 0.610 |
| olmOCR arxiv_math (LaTeX mode) | 0.860 | 0.538 | 0.000 |
| READoc GitHub (holdout) text_eds / head_teds / seg_kt | 0.936 / 0.882 / 0.997 | 0.874 / 0.778 / 0.990 | 0.908 / 0.471 / 0.993 |
| READoc arXiv (holdout, LaTeX mode) text_eds / head_teds / seg_kt / inline_eds | 0.887 / 0.718 / 0.983 / 0.685 | 0.826 / 0.515 / 0.976 / 0.369 | 0.841 / 0.403 / 0.984 / 0.001 |
| READoc arXiv (holdout) table_teds, LaTeX mode / declared options | 0.609 / 0.730 | 0.685 | 0.656 |

The two untagged producers added in 0.3.0 (chromium-untagged and context, 35 development items),
on which no other tool has run: NID 0.964, TEDS 0.889, MHS 0.929, construct F1 0.866 (0.4.0:
0.964, 0.889, 0.929, 0.849). olmOCR `headers_footers` (0.890, 0.891 in olmOCR's declared
options) and `long_tiny_text` in the default output (0.509) stay below docling's 0.898 and 0.523;
`long_tiny_text` passes it in olmOCR's declared options (0.525).

## (b) Tables

TEDS against docling's: odl 0.915 (0.934 in its declared options) against 0.879; synthetic
untagged 0.986 against 0.870, tagged 0.978 against 0.881; olmOCR `table_tests` 0.646 (0.653
declared) against 0.610. READoc arXiv `table_teds` on the holdout is 0.730 in READoc's declared
options, above docling's 0.656 and the best text-layer tool's 0.685: `--tables=pipe-latex` writes a
cell's `%`, `#` and `&` as LaTeX reads them (READoc's evaluator passes each cell into LaTeX, where a
bare `%` ended the row and with it the table), and `--figure-text=drop` leaves out the chart text
READoc's ground truth never holds ([evaluation.md](../development/evaluation.md#readoc)). In LaTeX
mode alone, without those conventions, it is 0.609 (0.4.0: 0.618).

## (c) Speed

CPU 3, 0.4.0 and 0.5.0 interleaved, 2026-11-24 17:58-18:07 UTC (nothing of this project running):

| measure | 0.5.0 | bar |
|---|---|---|
| fresh-process median per odl document (native `posix_spawn` timer, 12 rotated rounds, `--images=none`) | 0.912 ms (0.4.0: 0.911) | ≤ 1 ms/page |
| warm throughput over the 200 odl pages (`--bench`, best of 2; median of three runs) | 2,207 pages/s (0.4.0: 2,194) | ≥ 2,000 pages/s |
| warm median / p95 per page | 0.368 / 0.706 ms | – |

The release binary is the one the Release workflow builds on GitHub's runners. A dry run of the
workflow on the release's code with the version raised (`dff6e89`, run 45504203147) built a Linux
binary (SHA-256 `2619cfa4…`) whose converter is the code timed here: every RayoPDF function has
the size it has in a local build of that commit, which differs from the binary timed here
(`a95ad7f`) in 25 bytes, the version string `0.4.0` → `0.5.0` and the build ID. Its static C
library is newer: the workflow's cached build image holds a later Ubuntu update of glibc than the
local image (`is_trusted_path_normalize` no longer linked, five other functions of glibc changed
in size; 56 bytes less in all). Timed against the binary measured here, interleaved on CPU 3 after
the fuzz campaign (2026-11-25 01:52-01:54 UTC): warm 2,197 against 2,201 pages/s (medians of three
runs; the measured binary's own runs spread from 2,191 to 2,206), fresh 0.929 against 0.937 ms
(12 rotated rounds, paired −0.9 %), page faults 152 in both; so the times above hold for it.

## (d) Security

**Fuzzing:** 25 libFuzzer targets, one per parser entry point (the 25 of the last check), ASan and
UBSan, 3,762-3,763 s each on `a95ad7f` (94,059 s, 26.1 h aggregate), in three waves on ten CPUs
during the night of 2026-11-24, with nothing else of this project running:

|target|runs|target|runs|
|---|---|---|---|
|pdf_exact|581.2 M|common_serve|9.6 M|
|image_jpx|187.3 M|layout_pages|9.2 M|
|common_ocr_protocol|165.6 M|layout_content|8.3 M|
|image_jpeg|113.9 M|text_cjk|3.8 M|
|pdf_filters|59.7 M|layout_math|3.8 M|
|image_ccitt|39.8 M|text_program|3.5 M|
|ocr_server|36.8 M|layout_imagerules|3.3 M|
|text_outline|25.8 M|layout_tables|2.6 M|
|text_repair|19.3 M|text_bidi|1.9 M|
|pdf_document|11.7 M|text_cmap|1.7 M|
|image_decode|10.6 M|layout_structure|1.6 M|
|image_jbig2|9.9 M|crypt_handler|226 k|
|layout_document|9.6 M|||

25 of 25 clean, no finding. Every committed regression input replays clean on the frozen code
(`python3 tools/fuzz.py replay`).

**ASan sweeps:** an ASan + UBSan build of `a95ad7f` converted 2,837 PDFs (synthetic corpus, odl,
olmOCR born-digital, SafeDocs census sample) with images written and a 120 s cap per file, in
three modes (default, `--workers=1`, `--math=latex`): 0 crashes, 0 hangs, 0 sanitizer reports in
each. Fail-closed rejections: 1 password required (exit 11) and 3 corrupt beyond repair or not a
PDF (exit 10), all in SafeDocs.

## (e) Footprint

The seven core packages as the Release workflow's dry run built them on `dff6e89` (the tagged
archives differ by a few bytes: their entries' time is the tagged commit's), and the OCR add-on's
archives as `tools/package.py --ocr` builds them here:

| artifact | Linux x64 (static) | Windows x64 |
|---|---|---|
| `rayopdf` | 3,991,968 B | 5,190,054 B |
| `rayopdf-ocr` (add-on) | 7,688,848 B | 7,883,513 B |
| core archive | 2,018,972 B | 1,972,756 B |
| C++ API archive (`librayopdf.a`, header, example) | 1,866,046 B | 1,965,794 B |
| Python wheel (`librayopdf.so` or `rayopdf.dll` with the package) | 1,657,236 B | 1,941,220 B |
| add-on archive (with small, tiny and orientation models) | 36,077,206 B | 35,918,462 B |
| script pack archive (optional) | 21,108,162 B | 21,105,198 B |

The npm package, one for every platform, is 1,163,547 B. The static `rayopdf` grew from 0.4.0's
3,817,464 B to 3,991,968 B (+4.6 %): the rules landed since, the font-code tables of LaTeX mode
and the retrained edge-row model.

Peak RSS per document of `rayopdf --convert` (static), measured with `wait4` from a small parent,
add-on not installed:

| documents | images off: median / p95 / max | images written: median / p95 / max |
|---|---|---|
| odl (200) | 2.1 / 2.5 / 10.5 MiB | 2.5 / 5.1 / 17.8 MiB |
| olmOCR born-digital (1,403) | 2.4 / 3.3 / 37.4 MiB | 2.8 / 5.5 / 39.1 MiB |
| synthetic development (216) | 1.9 / 3.6 / 3.9 MiB | 2.0 / 6.2 / 7.1 MiB |
| scanned development (362) | 1.5 / 1.6 / 1.9 MiB | 5.4 / 130.8 / 522.6 MiB |
| SafeDocs sample (1,000) | 3.2 / 13.8 / 234.3 MiB | 4.5 / 31.3 / 238.0 MiB |

The largest values are the input file itself (mapped into memory) and JPEG 2000 and JPEG images,
which are decoded whole, up to four of a document's at once. The OCR add-on, when a page needs
it, peaks at about 0.6 GB.

## Builds

On `a95ad7f`: release, ASan + UBSan and ThreadSanitizer 31 of 31 tests, Windows (Wine), OCR, OCR
Windows and the wheel configuration (with `tests/verify_python.py`) 32 of 32, the WebAssembly
configuration 1 of 1 (`tests/verify_wasm.mjs` under Node.js), 0 warnings in each; the static and
debug builds, 0 warnings; every generated table matches its generator
(`gen_{text,cjk,bidi,math,model}_tables.py --check`); every archive of `tools/package.py`;
`tools/check_repo.py` 0 findings.
