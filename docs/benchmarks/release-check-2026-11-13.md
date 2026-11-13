# Release check of 0.2.0, 2026-11-13

RayoPDF 0.2.0 (code `536de1f`, frozen; the commit after it changes only VERSION and documents)
against the release bar of [roadmap.md](../development/roadmap.md#goal-and-release-bar). Quality
numbers are those of [v0.2.0-2026-11-13.md](v0.2.0-2026-11-13.md): the core's measured on
`56fba35`, whose output the two code commits after it leave byte for byte on every corpus
(`d4310cc` checked on the full byte gate, 6,331 comparisons; `536de1f` changes only what goes to
the OCR add-on), the bundle's on `536de1f`. Speed, security and footprint were measured for this
check. Same machine and harness as [the baselines](baselines-2026-11-05.md).

**Result: every item of the bar holds except READoc arXiv `table_teds`, open as in
[0.1.0](release-check-2026-11-12.md) by the user's decision of 2026-11-11.**

| item | status | detail |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | met | every odl, synthetic, olmOCR and READoc GitHub metric; READoc arXiv `text_eds`, `head_teds`, `seg_kt`, `inline_eds` (LaTeX mode, D6) |
| (b) TEDS ≥ docling | met except one key | odl, synthetic, olmOCR met; READoc arXiv `table_teds` 0.618 against docling's 0.656: **open** |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | met | 0.893 ms fresh, 2,231 pages/s warm |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | 25 targets, 26.1 h aggregate; one finding (a JBIG2 slow unit) fixed and its targets run again clean; ASan sweeps of 2,802 PDFs in three modes clean |
| (e) binary size, package size, peak RSS per document reported | met | below |

## (a) Quality

Core alone, default mode unless named (READoc arXiv in LaTeX mode, as decided 2026-11-11):

| metric | RayoPDF 0.2.0 | best text-layer tool | docling |
|---|---|---|---|
| odl overall / NID / TEDS / MHS | 0.909 / 0.923 / 0.915 / 0.874 | 0.885 / 0.914 / 0.818 / 0.822 | 0.884 / 0.902 / 0.879 / 0.827 |
| synthetic untagged NID / TEDS / MHS / construct F1 | 0.960 / 0.987 / 0.942 / 0.969 | 0.920 / 0.845 / 0.845 / 0.628 | 0.925 / 0.870 / 0.802 / 0.560 |
| synthetic tagged NID / TEDS / MHS / construct F1 | 0.972 / 0.978 / 0.933 / 0.951 | – / 0.775 / – / 0.594 | – / 0.881 / – / 0.599 |
| olmOCR headers_footers / long_tiny_text / multi_column / table_tests | 0.849 / 0.509 / 0.710 / 0.631 | 0.554 / 0.471 / 0.667 / 0.615 | 0.898 / 0.523 / 0.695 / 0.610 |
| olmOCR arxiv_math (LaTeX mode) | 0.720 | 0.538 | 0.000 |
| READoc GitHub (holdout) text_eds / head_teds / seg_kt | 0.936 / 0.882 / 0.997 | 0.874 / 0.778 / 0.990 | 0.908 / 0.471 / 0.993 |
| READoc arXiv (holdout, LaTeX mode) text_eds / head_teds / seg_kt / inline_eds | 0.882 / 0.697 / 0.981 / 0.654 | 0.826 / 0.515 / 0.976 / 0.369 | 0.841 / 0.403 / 0.984 / 0.001 |
| READoc arXiv (holdout, LaTeX mode) table_teds | 0.618 | 0.685 | 0.656 |

## (b) Tables

TEDS against docling's: odl 0.915 against 0.879; synthetic untagged 0.987 against 0.870, tagged
0.978 against 0.881; olmOCR `table_tests` 0.631 against 0.610. READoc arXiv `table_teds` is
0.618 against docling's 0.656 on the holdout; the steps of this version that raised it on the
development splits (`readoc-dev` 0.712 → 0.723, `readoc-dev2` 0.594 → 0.620) and on the
validation split (0.577 → 0.582) carried over to the holdout by 0.001 only.

## (c) Speed

CPU 3, 0.1.1 and 0.2.0 interleaved, 2026-11-13 10:11-10:19 UTC (load about 1 to 2):

| measure | 0.2.0 | bar |
|---|---|---|
| fresh-process median per odl document (native `posix_spawn` timer, 12 rotated rounds, `--images=none`) | 0.893 ms (0.1.1: 0.910) | ≤ 1 ms/page |
| warm throughput over the 200 odl pages (`--bench`, best of 3; median of three runs) | 2,231 pages/s (0.1.1: 2,214) | ≥ 2,000 pages/s |
| warm median / p95 per page | 0.368 / 0.694 ms | – |

## (d) Security

**Fuzzing:** 25 libFuzzer targets, one per parser entry point (the 24 of the last check, plus
`fuzz_layout_imagerules`, the rules of an OCR canvas's pixels), ASan and UBSan, 3,739-3,765 s
each on `56fba35`, twelve at a time (94,050 s, 26.1 h aggregate):

|target|runs|target|runs|
|---|---|---|---|
|common_ocr_protocol|74.3 M|text_bidi|586 k|
|text_repair|53.0 M|image_jpeg|565 k|
|pdf_exact|36.5 M|layout_structure|550 k|
|image_ccitt|25.2 M|text_cjk|542 k|
|pdf_filters|24.4 M|layout_tables|309 k|
|common_serve|13.4 M|image_jbig2|272 k|
|text_outline|12.8 M|pdf_document|178 k|
|text_program|2.9 M|ocr_server|110 k|
|layout_imagerules|2.2 M|crypt_handler|99 k|
|layout_pages|1.6 M|text_cmap|86 k|
|layout_math|1.3 M|image_decode|77 k|
|image_jpx|966 k|layout_document|19 k|
|layout_content|747 k|||

24 of 25 clean. `fuzz_image_decode` left a slow unit and a timeout: a JBIG2 generic region whose
arithmetic decoder had read past its data decoded every pixel of a large region on the 0xFF bytes
it is fed there (10.5 s in the fuzz build). The decoder now stops a region once it has read past
the end of its data `MqDecoder::kEndReads` (8) times, as pdfium does; on the 144 corpus PDFs with
JBIG2 images legitimate data reads past its end at most twice, and the full byte gate (6,331
comparisons) found no output changed (`d4310cc`, regression input
`tests/fuzz/regressions/fuzz_image_decode/slow-unit-jbig2-dictionary-past-its-data`, 31 ms).
On the frozen code `fuzz_image_decode` and `fuzz_image_jbig2` ran again from the campaign's
corpora, 3,762 s each: 403 k and 12.3 M runs, clean.

Every committed regression input replays clean on the frozen code (`python3 tools/fuzz.py replay`).

**ASan sweeps:** an ASan + UBSan build of `56fba35` converted 2,802 PDFs (synthetic corpus, odl,
olmOCR born-digital, SafeDocs census sample) with images written and a 120 s cap per file, in three
modes (default, `--workers=1`, `--math=latex`): 0 crashes, 0 hangs, 0 sanitizer reports in each.
Fail-closed rejections: 1 password required (exit 11) and 3 corrupt beyond repair or not a PDF
(exit 10), all in SafeDocs.

## (e) Footprint

`tools/package.py` on the release commit's tree before it was committed (the tagged archives
differ by a few bytes: the entries' time is the commit's; the Release workflow builds the four
core archives on the tag):

| artifact | Linux x64 (static) | Windows x64 |
|---|---|---|
| `rayopdf` | 3,576,608 B | 4,626,693 B |
| `rayopdf-ocr` (add-on) | 7,688,848 B | 7,883,513 B |
| core archive | 1,803,203 B | 1,761,381 B |
| C++ API archive (`librayopdf.a`, header, example) | 1,627,020 B | 1,717,111 B |
| add-on archive (with small, tiny and orientation models) | 36,074,692 B | 35,915,192 B |
| script pack archive (optional) | 21,104,622 B | 21,101,928 B |

Peak RSS per document of `rayopdf --convert` (static), measured with `wait4` from a small parent,
add-on not installed:

| documents | images off: median / p95 / max | images written: median / p95 / max |
|---|---|---|
| odl (200) | 1.9 / 2.3 / 10.4 MiB | 2.3 / 5.1 / 17.6 MiB |
| olmOCR born-digital (1,403) | 2.2 / 3.1 / 37.2 MiB | 2.6 / 5.3 / 38.8 MiB |
| synthetic development (181) | 1.7 / 3.2 / 3.5 MiB | 1.8 / 5.9 / 7.1 MiB |
| scanned development (362) | 1.6 / 1.6 / 1.9 MiB | 5.3 / 130.7 / 525.2 MiB |
| SafeDocs sample (1,000) | 2.9 / 13.8 / 234.4 MiB | 4.3 / 30.6 / 237.8 MiB |

The largest values are the input file itself (mapped into memory) and JPEG 2000 and JPEG images,
which are decoded whole, up to four of a document's at once. The OCR add-on, when a page needs
it, peaks at about 0.6 GB.

## Builds

On the frozen code: release, ASan + UBSan, ThreadSanitizer and Windows (Wine) 25 of 25 tests, OCR
and OCR Windows 26 of 26, 0 warnings in each; every generated table matches its generator
(`gen_{text,cjk,bidi,math}_tables.py --check`); `tools/check_repo.py` 0 findings. With VERSION
0.2.0 the static, Windows, OCR and OCR Windows builds, 0 warnings, and every archive of
`tools/package.py`; on `56fba35` also the debug build, 0 warnings.
