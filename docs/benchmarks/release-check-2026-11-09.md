# Release check against the 0.1.0 bar, 2026-11-09

RayoPDF 0.0.10 (commit 273129a) against the release bar of
[roadmap.md](../development/roadmap.md#goal-and-release-bar), on frozen code. Quality and speed
numbers are those of [v0.0.10-2026-11-09.md](v0.0.10-2026-11-09.md); security and footprint were
measured for this check. Same machine and harness as [the baselines](baselines-2026-11-05.md).

**Result: the bar is not met, so 0.1.0 is not tagged.** (d) security and (e) footprint are met;
(a) quality, (b) tables and (c) speed are not, each for the reasons below.

| item | status | what is short |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | not met | READoc arxiv (5 keys), olmOCR `arxiv_math`; every odl, synthetic, other olmOCR and READoc GitHub metric is met |
| (b) TEDS ≥ docling | not met | READoc arxiv `table_teds` 0.483 against 0.656; odl, synthetic and olmOCR met |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | not met | fresh 1.22 ms per odl document; warm about 1,560 pages/s |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | – |
| (e) binary size, package size, peak RSS per document reported | met | – |

## (a) Quality

Met (core alone; the bundle with the OCR add-on is at least as high on each): odl overall 0.896,
NID 0.918, TEDS 0.884, MHS 0.840 (best text-layer 0.885 / 0.914 / 0.818 / 0.822; docling NID 0.902,
MHS 0.827); synthetic untagged and tagged NID, TEDS, MHS, construct F1 and heading-level accuracy
all above every tool; olmOCR `headers_footers` 0.732, `long_tiny_text` 0.509 (0.869 with the
add-on), `multi_column` 0.698, `table_tests` 0.620 (best text-layer 0.554 / 0.471 / 0.667 /
0.615); READoc GitHub `text_eds` 0.937, `head_teds` 0.862, `seg_kt` 0.997 (bundle; best text-layer
0.874 / 0.778 / 0.990).

Not met:

| metric | RayoPDF | best text-layer tool | docling |
|---|---|---|---|
| READoc arxiv text_eds (holdout) | 0.747 | 0.826 (liteparse) | 0.841 |
| READoc arxiv head_teds (holdout) | 0.473 | 0.515 (pymupdf) | 0.403 |
| READoc arxiv inline_eds (holdout) | 0.001 | 0.369 (liteparse) | 0.001 |
| READoc arxiv table_teds (holdout) | 0.483 | 0.685 (pymupdf) | 0.656 |
| READoc arxiv seg_kt (holdout) | 0.953 | 0.976 (pymupdf) | 0.984 |
| olmOCR arxiv_math | 0.000 | 0.538 (liteparse) | 0.000 |

`arxiv_math` and `inline_eds` score formulas written as LaTeX; liteparse rebuilds LaTeX from the
text layer, and RayoPDF writes math as Unicode by decision (D6), as docling does here. These two
cannot be met without LaTeX math output. The other four READoc arxiv keys are a holdout: they are
worked on through development analogs only (olmOCR arxiv pages, `xelatex-2col`, odl academic
documents), which has moved them little since 0.0.3.

## (b) Tables

TEDS against docling's: odl 0.884 against 0.879; synthetic untagged 0.987 against 0.870, tagged
0.978 against 0.881; olmOCR `table_tests` 0.620 (core) and 0.672 (bundle) against 0.610. READoc
arxiv `table_teds` 0.483 against 0.656: figures, block diagrams and algorithm boxes still read as
tables on arXiv-like pages, and the separators tried so far cost real tables. The M8 table-ML
branch is not taken (Q2 ≤ +0.002 TEDS on odl), so (b) stands as written.

## (c) Speed

| measure | value | bar |
|---|---|---|
| fresh-process median per odl document (native `posix_spawn` timer, CPU 3, binary in the container, `--images=none`) | 1.22 ms (empty static program 0.22 ms) | ≤ 1 ms/page |
| fresh-process median, [performance.md](../development/performance.md) fixtures | 1.06 ms | ≤ 1 ms/page |
| warm, in process, median per page (odl, CPU 3, best of 3) | 0.41 ms | – |
| warm throughput over all odl pages ([performance.md](../development/performance.md)) | about 1,560 pages/s | ≥ 2,000 pages/s |

The median page is fast enough; heavy pages (p95 0.90 ms) set the throughput. The remaining cost
is spread: the content lexer about 14 % of instructions, inflate 10 %, glyph placement 10 %, font
loading 10 %, line building and duplicate folding 15 % together. The fresh figure follows the warm
one plus about 0.23 ms of process start.

## (d) Security

**Fuzzing:** 22 libFuzzer targets, one per parser entry point (PDF document, filters, Standard
security handler, content streams, structure trees, tables, CMaps, CJK, font programs, glyph
outlines, ToUnicode repair, bidi, image decoding, JPEG, JPEG 2000, JBIG2, CCITT, the serve protocol,
the OCR pipe protocol, the OCR add-on's server, RayoMD source payloads, whole-document layout), with
ASan and UBSan, 3,929–3,934 s each on frozen code (86,472 s, 24.0 h aggregate): 22 of 22 clean,
no findings.

|target|runs|target|runs|
|---|---|---|---|
|common_ocr_protocol|106.1 M|image_jpeg|608 k|
|pdf_exact|54.4 M|text_program|445 k|
|image_ccitt|35.0 M|image_jbig2|291 k|
|pdf_filters|30.9 M|layout_tables|279 k|
|text_repair|19.6 M|pdf_document|230 k|
|common_serve|9.4 M|ocr_server|173 k|
|text_outline|8.5 M|image_decode|132 k|
|layout_structure|1.3 M|text_cjk|117 k|
|layout_content|1.2 M|crypt_handler|112 k|
|text_bidi|707 k|text_cmap|68 k|
|image_jpx|691 k|layout_document|34 k|

Every committed regression input replays clean (`python3 tools/fuzz.py replay`). The 11 targets
of 0.0.3 had a 24 h campaign then too; this is the release campaign over all 22.

**ASan sweep:** an ASan + UBSan build converted 2,802 PDFs (synthetic corpus, odl, olmOCR
born-digital, SafeDocs census sample) with images written and a 120 s cap per file: 0 crashes, 0
hangs, 0 sanitizer reports. Fail-closed rejections: 1 password required (exit 11) and 3 corrupt
beyond repair or not a PDF (exit 10), all in SafeDocs.

## (e) Footprint

| artifact | Linux x64 (static) | Windows x64 |
|---|---|---|
| `rayopdf` | 3,363,080 B | 3,861,490 B |
| `rayopdf-ocr` (add-on) | 7,705,224 B | 7,881,297 B |
| core archive | 1,630,086 B | 1,490,273 B |
| add-on archive (with small, tiny and orientation models) | 36,076,859 B | 35,915,359 B |
| script pack archive (optional) | 21,104,878 B | 21,105,839 B |

Peak RSS per document of `rayopdf --convert`, measured with `wait4` from a small parent, add-on not
installed:

| documents | images off: median / p95 / max | images written: median / p95 / max |
|---|---|---|
| odl (200) | 2.6 / 2.9 / 9.7 MiB | 2.8 / 4.2 / 15.3 MiB |
| olmOCR born-digital (1,403) | 2.8 / 3.6 / 42.3 MiB | 2.9 / 5.0 / 42.2 MiB |
| synthetic development (181) | 2.3 / 3.8 / 4.0 MiB | 2.3 / 5.0 / 6.2 MiB |
| scanned development (362) | 1.9 / 2.3 / 2.5 MiB | 4.1 / 124.0 / 142.5 MiB |
| SafeDocs sample (1,000) | 3.3 / 15.2 / 313.4 MiB | 4.1 / 23.0 / 313.4 MiB |

The largest values are the input file itself (it is mapped into memory) and JPEG 2000 and JPEG
images, which are decoded whole. The OCR add-on, when a page needs it, peaks at about 0.6 GB.

## What closing the bar takes

1. **Speed (c):** about 22 % more throughput on heavy pages and about 0.2 ms of fresh-process time,
   from the measured candidates above.
2. **Math (a):** a decision on LaTeX math output. Keeping Unicode math (D6) means exempting the two
   LaTeX-math metrics (`arxiv_math`, READoc `inline_eds`) from the bar; meeting them means an
   optional LaTeX output mode for formulas.
3. **arXiv-like documents (a, b):** text order, heading structure and tables on scientific papers,
   worked on through development analogs only.

## Update, 0.0.11 (same day)

The user chose an optional LaTeX output (`--math=latex`, [v0.0.11](v0.0.11-2026-11-09.md)); the
two LaTeX-math metrics are now measured in that mode, everything else in the default mode (D6). Both
pass: olmOCR `arxiv_math` 0.718 against liteparse's 0.538, READoc arxiv `inline_eds` 0.650 against
0.369 (holdout, measured once). Still open for the bar, unchanged in the default mode: READoc arxiv
`text_eds` 0.747, `head_teds` 0.473, `table_teds` 0.483 and `seg_kt` 0.953 against the best tools'
0.826 / 0.515 / 0.685 / 0.976 (a, b), and speed (c). Security (d) holds: ASan sweeps of 2,802 PDFs
in both math modes are clean, and the new `fuzz_layout_math` target needs its share of a release
campaign before the next tag.
