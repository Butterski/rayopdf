# Release check of 1.1.0, 2026-11-26

RayoPDF 1.1.0 against the release bar of
[roadmap.md](../development/roadmap.md#goal-and-release-bar). The converter is that of `e1d9f98`
(frozen on 2026-11-25; the commits after it regenerate `src/hot.ld`, output byte-identical, take
the WebAssembly module back to its scalar build and change documents). Quality numbers are those
of [v1.1.0-2026-11-26.md](v1.1.0-2026-11-26.md), measured on this code; speed, footprint, the ASan
sweeps and the output identity runs too; the fuzz campaign ran on `fee2c14`, whose code is this
code. Same machine and harness as [the baselines](baselines-2026-11-05.md).

**Result: every item of the bar holds, as for 1.0.0.**

| item | status | detail |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | met | every output of the harness equals 1.0.0's but for the version JSON names, so [0.5.0's measurements](release-check-2026-11-24.md#a-quality) hold |
| (b) TEDS ≥ docling | met | the same: READoc arXiv `table_teds` 0.730 in READoc's declared options against docling's 0.656 (0.609 in LaTeX mode); odl, synthetic, olmOCR above docling |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | met | 0.883 ms fresh, 2,228 pages/s warm |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | 25 targets, 25.0 h aggregate clean (four rerun after timeouts that were stalls of the loaded machine); ASan sweeps of 2,837 PDFs in three modes clean |
| (e) binary size, package size, peak RSS per document reported | met | below |

## (a) Quality and (b) tables

The harness converted every suite with this code in the core's default options, LaTeX mode, with
the OCR add-on and in each suite's declared options, as for 0.5.0's record: of 47,022 output
files, 33,385 equal those of 0.5.0's run (1.0.0's converter) byte for byte and 13,637 differ only
in the JSON output's version field; every conversion's status and every aggregate score are the
same. The tables of the [release check of 0.5.0](release-check-2026-11-24.md) are therefore
1.1.0's.

## (c) Speed

CPU 3, 1.0.0's release binary and 1.1.0's code interleaved, 2026-11-25 21:43-21:48 UTC, nothing
of this project running:

| measure | 1.1.0 | bar |
|---|---|---|
| fresh-process median per odl document (native `posix_spawn` timer, 12 rotated rounds, `--images=none`) | 0.883 ms (1.0.0: 0.924 in the same run, −5.5 % paired) | ≤ 1 ms/page |
| warm throughput over the 200 odl pages (`--bench`, best of 3; median of three runs) | 2,228 pages/s (1.0.0: 2,203) | ≥ 2,000 pages/s |
| warm median / p95 per page | 0.367 / 0.698 ms | – |

The release binary is the one the Release workflow builds on GitHub's runners. A dry run of the
workflow on the release's code with the version raised (`573434c`, run 45709733714) built a Linux
binary (4,343,736 B) whose converter is the code timed here: every RayoPDF function has the size
it has in a local build of that commit, which differs from the binary timed here (`5fa1f0d`) in
25 bytes, the version string `1.0.0` → `1.1.0` in its five places and the build ID. Its static C
library is newer, as for 0.5.0: the workflow's cached build image holds a later Ubuntu update of
glibc (`is_trusted_path_normalize` no longer linked, a few other functions of glibc changed in
size; 48 bytes less in all).

## (d) Security

**Fuzzing:** 25 libFuzzer targets, one per parser entry point (the 25 of the last check), ASan and
UBSan, 3,602-3,615 s each on `fee2c14` (90,076 s, 25.0 h aggregate) on ten CPUs on
2026-11-25. The first run, 16:42-19:47 UTC while other jobs loaded the machine (load average 40 to
100), ended 21 targets clean; libFuzzer stopped the other four on its 10 s timeout after 52 to
2,386 s (`crypt_handler`, `layout_imagerules`, `layout_math`, `layout_pages`). Each of their
inputs runs in 6 to 180 ms on the quiet machine: stalls of the loaded one, not slow inputs. The
four ran again for 3,602 s each (19:52-20:52 UTC) and were clean:

|target|runs|target|runs|
|---|---|---|---|
|pdf_exact|373.0 M|layout_pages|6.8 M|
|common_ocr_protocol|106.6 M|common_serve|6.7 M|
|image_jpx|105.7 M|image_jbig2|6.6 M|
|pdf_filters|77.7 M|pdf_document|5.5 M|
|text_repair|24.8 M|image_decode|5.5 M|
|image_ccitt|17.1 M|ocr_server|5.1 M|
|image_jpeg|16.9 M|layout_document|4.7 M|
|text_outline|14.3 M|layout_imagerules|4.6 M|
|layout_math|3.0 M|text_program|2.2 M|
|layout_content|1.1 M|text_cjk|991 k|
|layout_tables|982 k|layout_structure|810 k|
|text_bidi|799 k|text_cmap|403 k|
|crypt_handler|164 k|||

25 of 25 clean, no finding. Every committed regression input replays clean on the frozen code
(`python3 tools/fuzz.py replay`).

**ASan sweeps:** an ASan + UBSan build of `e1d9f98` converted 2,837 PDFs (synthetic corpus, odl,
olmOCR born-digital, SafeDocs census sample) with images written and a 120 s cap per file, in
three modes (default, `--workers=1`, `--math=latex`): 0 crashes, 0 hangs, 0 sanitizer reports in
each. Fail-closed rejections: 1 password required (exit 11) and 3 corrupt beyond repair or not a
PDF (exit 10), all in SafeDocs.

## Output identity

- **WebAssembly:** the npm package's module (built without SIMD: a SIMD build was tried and
  refused for speed) against the static build's `--convert`, images off: opendataloader-bench 200
  of 200 identical; the other 5,686 documents of the speed steps' inputs (synthetic corpus,
  olmOCR-Bench, READoc dev and dev2, OmniParseBench's pages, 1,000 SafeDocs documents) 5,667
  identical. The 19 others (9 OmniParseBench pages, 5 SafeDocs files, 2 READoc arXiv papers, 3
  olmOCR pages) differ in the scalar and the SIMD build alike, and differed before 1.1 too:
  differences between the platforms' runtimes, not traced.
- **Threads:** `--batch --workers=1` against `--workers=4` over the 27 multi-page documents, images
  off and with `--math=latex --format=json`: every file identical.

## (e) Footprint

The seven core packages as the Release workflow's dry run built them on `573434c` (the tagged
archives differ by a few bytes: their entries' time is the tagged commit's, and the README
sections they carry), and the OCR add-on's archives as `tools/package.py` builds them here:

| artifact | Linux x64 (static) | Windows x64 |
|---|---|---|
| `rayopdf` | 4,343,736 B | 5,587,414 B |
| `rayopdf-ocr` (add-on) | 7,651,360 B | 7,845,353 B |
| core archive | 2,190,176 B | 2,188,967 B |
| C++ API archive (`librayopdf.a`, header, example) | 1,996,050 B | 2,090,637 B |
| Python wheel (`librayopdf.so` or `rayopdf.dll` with the package) | 1,835,326 B | 2,157,986 B |
| add-on archive (with small, tiny and orientation models) | 36,055,605 B | 35,899,508 B |
| script pack archive (optional) | 21,100,300 B | 21,098,692 B |

The npm package, one for every platform, is 1,334,149 B. The static `rayopdf` grew from 1.0.0's
3,991,968 B to 4,343,736 B (+8.8 %): the BMP's UTF-8 table (192 KiB) and the base encodings'
texts (36 KiB), now built in instead of made at run time, and `std::to_chars`' tables for JSON
numbers (about 115 KiB). The add-on lost 37 KB with the backends that never shipped.

Peak RSS per document of `rayopdf --convert` (static), measured with `wait4` from a small parent,
add-on not installed:

| documents | images off: median / p95 / max | images written: median / p95 / max |
|---|---|---|
| odl (200) | 2.0 / 2.3 / 7.9 MiB | 2.3 / 4.9 / 17.4 MiB |
| olmOCR born-digital (1,403) | 2.2 / 2.9 / 38.9 MiB | 2.5 / 5.3 / 39.1 MiB |
| synthetic development (216) | 1.8 / 3.3 / 3.9 MiB | 2.0 / 5.9 / 6.8 MiB |
| scanned development (362) | 1.6 / 1.9 / 2.1 MiB | 5.2 / 130.6 / 522.7 MiB |
| SafeDocs sample (1,000) | 3.1 / 14.4 / 229.8 MiB | 4.3 / 30.1 / 234.7 MiB |

Against 0.5.0's, the medians fell by up to 0.3 MiB and odl's largest page from 10.5 to 7.9 MiB
(its inflate buffer in huge pages); the scanned pages with images off rose by 0.1 MiB in the
median and 0.3 MiB in the p95 (not traced). The largest values are the input file itself
(mapped into memory) and JPEG 2000 and JPEG images, which are decoded whole, up to four of a
document's at once. The OCR add-on, when a page needs it, peaked at about 0.6 GB in 0.5.0's
check; its ncnn engine and models are unchanged.

## Builds

On the release code: release, ASan + UBSan and ThreadSanitizer 31 of 31 tests, Windows (Wine), OCR
and the wheel configuration (with `tests/verify_python.py`) 32 of 32, the WebAssembly
configuration 1 of 1 (`tests/verify_wasm.mjs` under Node.js), 0 warnings in each; the static,
debug and OCR Windows builds, 0 warnings; every generated table matches its generator
(`gen_{text,cjk,bidi,math,model}_tables.py --check`); `tools/check_repo.py` 0 findings.
