# Release check of 1.2.0, 2026-11-27

RayoPDF 1.2.0 against the release bar of
[roadmap.md](../development/roadmap.md#goal-and-release-bar). The release's code is `a745e50`
(frozen on 2026-11-27; the commits after it change documents and the version). Quality numbers are
those of [v1.2.0-2026-11-27.md](v1.2.0-2026-11-27.md), measured on `0d4b42d`, whose converter is
this one (`a745e50` regenerates `src/hot.ld`, output byte-identical); speed, footprint, the ASan
sweeps, the output identity runs and the fuzz campaign ran on `a745e50`. Same machine and harness
as [the baselines](baselines-2026-11-05.md).

**Result: every item of the bar holds, as for 1.1.0.**

| item | status | detail |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | met | of the 118 metrics of the strict gate, 42 rose and none fell by more than its epsilon (0.0001) against 1.1.0, which met this item; the [record](v1.2.0-2026-11-27.md#quality) has the headline numbers |
| (b) TEDS ≥ docling | met | READoc arXiv `table_teds` 0.730 in READoc's declared options against docling's 0.656; opendataloader-bench TEDS 0.937 against docling's 0.879 |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | met | 0.896 ms fresh, 2,227 pages/s warm |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | 27 targets, 24.0 h aggregate clean; ASan sweeps of 2,837 PDFs in three modes clean |
| (e) binary size, package size, peak RSS per document reported | met | below |

## (a) Quality and (b) tables

The harness converted every suite with this code in the core's default options, LaTeX mode, with
the OCR add-on and in each suite's declared options: every conversion succeeded, and every
aggregate the strict gate reads is at or above 1.1.0's within 0.0001 (the record lists the four
that fell, by 0.00004 at most). The comparisons with the other tools in the
[release check of 0.5.0](release-check-2026-11-24.md) hold with 1.2.0's numbers.

## (c) Speed

CPU 3, 1.1.0's release binary and 1.2.0's code interleaved, 2026-11-27 19:07-19:12 UTC, nothing
else running:

| measure | 1.2.0 | bar |
|---|---|---|
| fresh-process median per odl document (native `posix_spawn` timer, 12 rotated rounds, `--images=none`) | 0.896 ms (1.1.0: 0.899 in the same run, −0.3 % paired) | ≤ 1 ms/page |
| warm throughput over the 200 odl pages (`--bench`, best of 3; median of three runs) | 2,227 pages/s (1.1.0: 2,222) | ≥ 2,000 pages/s |
| warm median / p95 per page | 0.370 / 0.708 ms | – |

The release binary is the one the Release workflow builds on GitHub's runners. A dry run of the
workflow on the release's code with the version raised (run 46055483358) built a Linux binary of
4,536,280 B; the binary timed here, a local build of the same code with the version 1.1.0, is
4,536,336 B.

## (d) Security

**Fuzzing:** 27 libFuzzer targets, one per parser entry point (the 25 of the last check,
`pdf_trees` for the name and number trees and `layout_forms` for form fields), ASan and UBSan,
3,201-3,207 s each on `a745e50` (86,455 s, 24.0 h aggregate) on six CPUs on 2026-11-27, 13:46-18:15
UTC, from the merged corpora and 679 seed PDFs for the four targets that read whole documents:

|target|runs|target|runs|
|---|---|---|---|
|pdf_exact|750.2 M|layout_forms|2.5 M|
|image_jpx|190.4 M|layout_math|1.8 M|
|common_ocr_protocol|190.0 M|text_cjk|1.3 M|
|pdf_filters|87.7 M|text_bidi|797 k|
|text_repair|34.3 M|layout_structure|791 k|
|image_jpeg|28.0 M|layout_tables|689 k|
|image_ccitt|26.0 M|layout_content|375 k|
|text_outline|22.9 M|pdf_trees|374 k|
|common_serve|12.1 M|pdf_document|236 k|
|ocr_server|8.6 M|text_cmap|198 k|
|image_jbig2|8.5 M|crypt_handler|190 k|
|image_decode|8.5 M|layout_pages|116 k|
|layout_imagerules|4.4 M|layout_document|93 k|
|text_program|2.6 M|||

27 of 27 clean, no finding. The targets that read whole documents ran fewer inputs than in the
last check (`layout_document` 93 k against 4.7 M): their corpora now start from the 679 seed PDFs,
each a real document. Every committed regression input replays clean on the frozen code
(`python3 tools/fuzz.py replay`).

**ASan sweeps:** an ASan + UBSan build of `a745e50` converted 2,837 PDFs (synthetic corpus, odl,
olmOCR born-digital, SafeDocs census sample) with images written and a 120 s cap per file, in
three modes (default, `--workers=1`, `--math=latex`): 0 crashes, 0 hangs, 0 sanitizer reports in
each. Fail-closed rejections: 1 password required (exit 11) and 3 corrupt beyond repair or not a
PDF (exit 10), all in SafeDocs.

## Output identity

Every comparison below is of exit code, `--report` record and every output file, over the 5,886
documents of the speed steps' inputs (synthetic corpus, opendataloader-bench, olmOCR-Bench, READoc
dev and dev2, OmniParseBench's pages, 1,000 SafeDocs documents), against the static x64 program:

- **Linux arm64:** the static aarch64 program under qemu, images off, on, in LaTeX mode and JSON:
  23,544 comparisons, 0 differ (before the release's last steps, 58,860 comparisons in all ten
  option sets of the speed steps, 0 differ).
- **libc++:** the program built with Clang and libc++ (the standard library of macOS and of the
  WebAssembly module), the same four sets: 23,544 comparisons, 0 differ. Before 1.2 it differed in
  90; the sorts now give one order whatever the library.
- **macOS:** CI's identity job builds on Linux x64, Linux arm64 and macOS on Apple silicon and
  Intel and compares the outputs' digests over opendataloader-bench's 200 documents in four
  option sets: equal on every platform.
- **WebAssembly:** the npm package's module against `--convert`, images off: 5,886 of 5,886
  identical (1.1.0: 5,867).
- **Threads:** one `--convert` a document at `--workers=1` against `--workers=4`, images off, in
  LaTeX mode and JSON: 17,658 comparisons, 0 differ.

## (e) Footprint

The sixteen core packages as the Release workflow's dry run built them on the release's code with
the version raised (run 46055483358; the tagged archives differ by a few bytes: their entries'
time is the tagged commit's, and the README sections they carry), and the OCR add-on's archives
as `tools/package.py` builds them here:

| artifact | Linux x64 (static) | Linux arm64 (static) | macOS arm64 | macOS x64 | Windows x64 |
|---|---|---|---|---|---|
| `rayopdf` | 4,536,280 B | 4,181,232 B | 3,153,576 B | 3,569,920 B | 5,918,368 B |
| core archive | 2,279,247 B | 2,121,575 B | 1,698,815 B | 1,854,346 B | 2,306,180 B |
| C++ API archive (`librayopdf.a`, header, example) | 2,096,626 B | 2,027,355 B | 1,848,557 B | 1,992,257 B | 2,195,281 B |
| Python wheel (`librayopdf.so`, `.dylib` or `rayopdf.dll` with the package) | 1,916,929 B | 1,849,166 B | 1,664,090 B | 1,817,697 B | 2,274,546 B |
| `rayopdf-ocr` (add-on) | 7,651,360 B | – | – | – | 7,845,361 B |
| add-on archive (with small, tiny and orientation models) | 36,055,896 B | – | – | – | 35,899,805 B |
| script pack archive (optional) | 21,100,663 B | – | – | – | 21,098,946 B |

The npm package, one for every platform, is 1,345,507 B (1.1.0: 1,334,149 B). The static Linux
`rayopdf` grew from 1.1.0's 4,343,736 B to 4,536,280 B (+4.4 %) and the Windows program from
5,587,414 B to 5,918,368 B (+5.9 %): the readers of forms, annotations, outlines and page labels,
the HTML and text writers and the JSON additions. The add-on changed only in code for macOS,
where it is not built (7,651,360 B on Linux in 1.1.0 too).

Peak RSS per document of `rayopdf --convert` (static), measured with `wait4` from a small parent,
add-on not installed:

| documents | images off: median / p95 / max | images written: median / p95 / max |
|---|---|---|
| odl (200) | 2.1 / 2.5 / 7.9 MiB | 2.4 / 5.1 / 18.0 MiB |
| olmOCR born-digital (1,403) | 2.4 / 3.2 / 37.3 MiB | 2.7 / 5.6 / 39.2 MiB |
| synthetic development (216) | 1.9 / 3.6 / 4.0 MiB | 2.0 / 6.3 / 7.4 MiB |
| scanned development (362) | 1.6 / 1.9 / 2.1 MiB | 5.5 / 131.0 / 523.0 MiB |
| SafeDocs sample (1,000) | 3.2 / 15.0 / 232.3 MiB | 4.6 / 30.8 / 237.2 MiB |

Against 1.1.0's, the medians rose by 0.1 to 0.3 MiB (not traced); the largest values are, as
before, the input file itself (mapped into memory) and JPEG 2000 and JPEG images, which are
decoded whole.

## Builds

On the release code: release, ASan + UBSan and ThreadSanitizer 32 of 32 tests; Windows (Wine), OCR
and the wheel configuration (with `tests/verify_python.py`) 33 of 33; the arm64 configuration under
qemu 30 of 30 (the two tests that start the stand-in OCR add-on run on the native arm64 runner in
CI); the WebAssembly configuration 1 of 1 (`tests/verify_wasm.mjs` under Node.js); 0 warnings in
each. CI's runners build and test the macOS configurations on Apple silicon and Intel and the
Linux arm64 one natively. Every generated table matches its generator
(`gen_{text,cjk,bidi,math,model}_tables.py --check`); `tools/check_repo.py` 0 findings.
