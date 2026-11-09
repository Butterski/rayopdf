# RayoPDF roadmap

RayoPDF converts PDF to Markdown and is the reverse of [RayoMD](https://github.com/Butterski/rayomd).
This document records the architecture decisions and the milestones. Every exit gate is stated
against the measured baselines in
[docs/benchmarks/baselines-2026-11-05.md](../benchmarks/baselines-2026-11-05.md), produced by the
harness that [evaluation.md](evaluation.md) specifies. The decisions cite the research notes in
`docs/research/`: [landscape](../research/landscape.md), [algorithms](../research/algorithms.md)
and [OCR/ML](../research/ocr-ml.md).

RayoMD's decision record
([arbitrary_pdf_research_decision.md](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/arbitrary_pdf_research_decision.md),
"Consequences") requires a converter to be a separate package with explicit uncertainty, a
representative quality corpus, and evidence on package size, latency, RSS and security. The release
bar below turns each of those into a measured gate.

## Goal and release bar

RayoPDF 0.1.0 ships when all of the following hold on the harness of `docs/development/evaluation.md`:

- **(a) Quality.** Every quality metric is at least the best text-layer tool's. NID and MHS are also
  at least docling's local numbers. "Text-layer tools" are pdftotext, markitdown, pymupdf,
  opendataloader, liteparse and edgeparse; docling is the ML reference.
- **(b) Tables.** TEDS is at least docling's, or the table-ML branch of the M8 gate is taken.
- **(c) Speed.** The fresh-process median is at most 1 ms per page on odl with one pinned core, and
  warm throughput is at least 2,000 pages/s on one core. The fresh-process figure is taken with a
  native timer (as RayoMD's `fresh.sh` does), because the harness's Python `subprocess` timing adds
  about 1 ms. Baseline for scale: the fastest warm tool is edgeparse at 139.7 pages/s, the fastest
  fresh-process tool pdftotext at 17.1 ms/page.
- **(d) Security.** Every parser entry point has a libFuzzer target with at least 24 h of clean
  aggregate fuzzing in the release campaign. An AddressSanitizer build also converts the census and
  synthetic corpora with 0 crashes and 0 hangs. Fail-closed rejections are allowed, and the
  release benchmark reports their count.
- **(e) Footprint.** The release benchmark reports binary size, package size and peak RSS per
  document.

Uncertainty is always explicit:

- A `<!-- rayopdf: … -->` comment sits at each affected spot: a page with no extractable text, a
  low-confidence table, unmapped glyphs, a repaired cross-reference table.
- `--report` emits one JSON line per document with the counters behind those comments.

## Decisions

### D1 Language and build

C++17 with CMake, mirroring RayoMD:

- No exceptions, RTTI, iostreams, `<filesystem>` or libm in `src/`.
- Optimisation level per file (`-O2` or `-Os`), each with a measured reason recorded next to it.
- Static Linux and MinGW Windows builds.
- SIMD through intrinsics with runtime dispatch and a scalar reference implementation that tests
  compare against. No hand-written assembly.

Source: [algorithms §7](../research/algorithms.md#7-speed-engineering), RayoMD
[AGENTS.md](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/AGENTS.md)
§ Performance Guardrails.

### D2 Dependencies

- The core is our own code.
- Generated tables come from permissive sources: the Adobe Glyph List and Adobe cmap-resources
  (BSD-3-Clause), and the standard-14 font widths from pdf.js (Apache-2.0).
- libdeflate (MIT) is only a test oracle, as in RayoMD; no product links it.
- Any new permissive library (MIT, BSD, Apache-2.0, zlib) needs a measured justification.
- No GPL or AGPL code in anything shipped.

Source: [algorithms §1.3, §3](../research/algorithms.md), [landscape §1](../research/landscape.md).

### D3 Reuse with provenance headers

Forked files keep a header naming their origin repository and commit:

- From `rayomd-gui` `src/pdf/`: `value.h`, `syntax.*`, `reader.*` and `content.*` give the arena,
  the lexer, the classic cross-reference reader and the content-stream lexer.
- From RayoMD:
  - `src/common/*`;
  - `src/core/inflate.*`, extended with an unknown output size, a salvage mode and size caps;
  - `png_filter.*`, generalised to any bytes per pixel for PDF predictors;
  - `cff.*`, plus charset and built-in encoding readers;
  - the PNG encoder;
  - `rayomd_pdf_source.*` for exact recovery of RayoMD-made PDFs.

Source: [algorithms §1.3, §1.4, §3.3](../research/algorithms.md).

### D4 Pipeline

1. **Reader:** memory-mapped file, lazy cross-reference sections, repair scan.
2. **Per-page interpreter:** the content stream becomes an event list (glyph runs, rulings, fills,
   images, marked content) in a page arena.
3. **Font and Unicode resolution**, in the order of [algorithms §3.1](../research/algorithms.md#31-priority-order-spec--engines).
4. **Words and lines:** MuPDF's size-relative thresholds plus PDFBox's duplicate-glyph handling.
5. **Page layout:** XY-Cut++ geometry with Breuel's rules as the validator; headers and footers
   are removed first.
6. **Cross-page document semantics:** headings, lists, paragraphs continued across pages and
   columns, footnotes.
7. **Writers:** the Markdown writer and the image writer.
8. **Report:** the `--report` counters.

Pages are processed in parallel. The output must not depend on the thread count.

Source: [algorithms §2, §4, §5, §6](../research/algorithms.md).

### D5 Source precedence

1. `rayomd-source/1` exact recovery of RayoMD-made PDFs (`--no-exact` turns it off).
2. A structure tree that passes validation.
3. The document outline.
4. Geometry heuristics.

Source: [algorithms §6.10](../research/algorithms.md#610-tagged-pdf-as-the-primary-signal),
[landscape §3](../research/landscape.md).

### D6 Output dialect

- GitHub Flavored Markdown.
- Tables are GFM pipe tables, or HTML when a table has spanning cells or multi-paragraph cells.
- Footnotes are `[^n]`.
- Images are written as `![alt](<stem>-images/p<page>-<n>.<ext>)`: DCT streams as `.jpg`, JPX
  streams as `.jp2`, everything else decoded to `.png`.
- Math is Unicode with `<sup>`/`<sub>`, wrapped in `$…$` only for linear inline math.

### D7 Command line

```
rayopdf --convert IN.pdf OUT.md [--images=DIR|none] [--password=PW] [--pages=A-B]
        [--no-exact] [--no-annotations] [--no-structure] [--report]
rayopdf --inspect IN.pdf
rayopdf --batch IN-FOLDER OUT-FOLDER [--recursive] [--skip-unchanged] [--report[=FILE]]
        [--workers=N] [--images=none] [--password=PW] [--pages=A-B] [--no-exact] [--no-annotations]
        [--no-structure]
rayopdf --serve --json [--workers=N] [--images=none] [--password=PW] [--pages=A-B] [--no-exact]
        [--no-annotations] [--no-structure]
```

`--no-exact` converts the pages of a RayoMD PDF that carries its source (D5 step 1,
`src/layout/exact.h`; the record's `exact` counter says whether the source was used, and why not).
One record shape serves `--report`, the batch report and the serve answers, after RayoMD's
protocol-1 record: `{"id"?, "input", "output", "status": "ok"|"skipped"|"error", "code",
"error"?, "pages", "bytes", "ms", <counters>, "markdown"?}`, one JSON object a line; `code` is the
exit code the document alone would give.

- **`--batch`** converts the `*.pdf` files of a folder (with `--recursive` its subfolders too,
  hidden ones and links to folders excepted, mirrored) to `NAME.md` and `NAME-images/` under the
  output folder, sorted by their UTF-8 paths. Two inputs with one output (`a.pdf`, `a.PDF`) leave
  the second refused (code 12). The exit code is the last failed document's, in input order.
- **`--serve --json`** answers requests `{"id"?, "input", "output"?, "args"?}` (RayoMD's protocol
  1 without its `markdown`/`source_path` input, as the input of a PDF converter is a file). The
  first line out is `{"rayopdf":"VERSION","protocol":1}`; without `output` the answer carries the
  Markdown. `args` holds flags of `--convert` only; batch flags and unknown fields are code-2
  errors and the session goes on. Lines are at most 1 MiB, `args` at most 64. The request reader
  (`src/common/serve.*`) has the fuzz target `fuzz_common_serve`.
- **`--workers=N`** (1–64) converts documents in parallel, one thread each: a batch defaults to one
  per usable CPU, at most 6; a serve session to 1. Records and output files never depend on N:
  documents share no mutable state (the arena chunk pool is locked, scratch buffers are
  thread-local), records are written in input (request) order, and only `ms` varies. The
  converter runs clean under ThreadSanitizer (`tools/dev.py test --config tsan`).
- **Windows:** a static MinGW-w64 build (`tools/dev.py build --config
  windows`); paths and arguments are UTF-8 inside and UTF-16 at the Win32 API; standard streams
  are binary, so the Markdown is byte for byte the Linux build's.

|Exit code|Meaning|
|---|---|
|0|converted|
|2|usage error|
|3|unreadable input|
|10|corrupt beyond repair|
|11|password required|
|12|unwritable output|
|13|unsupported encryption (a handler or revision other than the Standard handler R2–R6)|

Releases (`tools/package.py`): `rayopdf-VERSION-linux-x64-static.tar.gz` and
`rayopdf-VERSION-windows-x64.zip`, each one folder with the binary, the README's usage part,
LICENSE, NOTICE, VERSION and `third_party/licenses/`, with fixed times and owners, and
`SHA256SUMS`.

### D8 Limits

The limits of [algorithms §1.5](../research/algorithms.md#15-hostile-input-limits-precedents):
object nesting 64, form XObject depth 40 with a cycle set, a page-tree cycle set, q/Q stack 256,
operand stack 64, Flate output per stream min(256 MiB, 1000 × compressed size), a per-document
decoded-bytes budget, a per-page operator budget and a per-document wall-clock budget. Every limit
fails closed and is counted in `--report`.

## Milestones

Each milestone ends with:

- a harness report saved as `docs/benchmarks/m<N>-<date>.md`;
- core tests for the new code;
- a fuzz target for each new parser entry point;
- an A/B speed record (see Engineering conventions).

"Best text-layer" and "docling" mean the numbers in
[baselines-2026-11-05.md](../benchmarks/baselines-2026-11-05.md).

|M|Scope|Exit gate|
|---|---|---|
|M1 Reader|Object layer: classic and stream cross-reference tables, object streams, hybrid files, `/Prev` chains with a cycle check, repair scan. Filters: Flate with salvage, LZW, ASCII85, ASCIIHex, RunLength, predictors; image filters passed through. Standard security handler R2–R6 with our own MD5, RC4, AES and SHA-2. Page tree. Exact recovery. `--inspect`.|Page count equals `pdfinfo`'s on ≥ 99.5 % of the census files that pdfinfo opens. 0 crashes and 0 hangs (ASan, 10 s cap per file) over the census and synthetic corpora. 24 h of clean fuzzing. `--inspect` ≤ 0.3 ms fresh-process median on odl.|
|M2 Text|Interpreter, the full font → Unicode chain, widths, words and lines, duplicate glyphs → bold, hidden-text filters, ligatures, diacritics, dehyphenation. Paragraph-only Markdown. Uncertainty comments.|NID ≥ best text-layer on odl and on every untagged synthetic producer. Census `broken` page rate ≤ pdftotext's on the same pages. ≤ 2 ms/page fresh-process and ≥ 5× faster than the fastest local competitor.|
|M3 Layout|XY-Cut++ with Breuel's rules; headers, footers and page numbers; rotation; right-to-left text through the bidi algorithm; paragraphs across columns and pages.|olmOCR `multi_column`, `headers_footers` and `long_tiny_text` ≥ the best of all measured tools. Synthetic `xelatex-2col` NID ≥ 0.95. odl NID ≥ docling.|
|M4 Semantics|Headings (outline, then style clusters, then numbering), lists (glyph and vector bullets, enumerators, task lists), code, quotes and alerts, rules, emphasis, links, footnotes, image extraction, Unicode math.|odl MHS ≥ docling. Synthetic construct F1 ≥ 0.95 on the untagged development producers and ≥ 0.90 on the holdout. Heading-level accuracy ≥ 0.95. READoc `head_teds` ≥ the best measured tool.|
|M5 Tables|Ruled lattice tables from vector edges; spans written as HTML; borderless tables detected only above a confidence threshold, otherwise text plus an uncertainty comment.|odl TEDS ≥ best text-layer, with the gap to docling reported. Synthetic TEDS ≥ 0.95. olmOCR `table_tests` ≥ best text-layer.|
|M6 Tagged|Structure tree, RoleMap, ParentTree, the four validation criteria of [algorithms §6.10](../research/algorithms.md#610-tagged-pdf-as-the-primary-signal), Alt and ActualText, artifacts.|Construct F1 on tagged producers ≥ 0.98, and ≥ the untagged path's on the same fixtures.|
|M7 Product|Final command line, `--report`, `--batch`, `--serve --json`, `--workers`, Windows build, performance campaign (hot-section linker script `hot.ld`, A/B runs with byte-identical Markdown), packaging.|The release bar above.|
|M8 OCR/ML gate|Measure M1, M2, M3, R, S and Q1–Q3 of [OCR/ML §6.2](../research/ocr-ml.md#62-measurements) on the census, odl and olmOCR, including olmOCR's `old_scans*` categories.|The decision is written to `docs/development/ocr_ml_decision.md`. R < 0.1 % → no OCR. 0.1–5 % and mostly Latin or Cyrillic → optional external `tesseract`. ≥ 5 %, or CJK, or a need for OCR on Windows with nothing installed → linked PP-OCRv6 through ONNX Runtime minimal or ncnn. Table ML only if Q2 ≥ 0.10 TEDS after M5. Reading-order or heading ML only if Q1 ≥ 0.03 or Q3 ≥ 0.05.|

**M8 prior (baseline census, SafeDocs `0000.zip`, 996 PDFs, 8,905 pages examined).**
`R_upper` = 12.4 % of examined pages: 8.9 % `image_only` and 3.4 % `broken`. This is an upper
bound: it is measured before any of RayoPDF's own Unicode repair, which can only lower the
`broken` part (M3 of the gate). `image_only` pages alone are above the 5 % threshold of the
linked-OCR branch. Of the `image_only` + `broken` pages, 43.2 % are Latin, 0.6 % CJK, 0.1 %
Cyrillic, 3.7 % other scripts, and 52.3 % have no text to tell (`unknown`). 14.0 % of the
documents have at least one `image_only` page and 4.1 % at least one `broken` page. The sample
is web-crawled PDFs, not the four target document classes, so the gate re-measures R on its own
corpus.

### Status: 0.0.10 (2026-11-09)

The first converter (0.0.1) covered M1 and the core of M2, M3 and M4 at once, so that every later
step is measured on a working pipeline; 0.0.2 added tables, images, footnotes, right-to-left and
CJK text; 0.0.3 closed READoc's GitHub regression; 0.0.4 passed the best text-layer tool on every
odl metric; 0.0.5 completed the CJK CMaps and UAX #9; 0.0.6 passed olmOCR `table_tests`; 0.0.7
added tagged PDF (M6), the product front ends (M7 without the performance campaign) and the OCR/ML
decision (M8); 0.0.8 was the first performance campaign; 0.0.9 built M8: the OCR add-on, scan
codecs, glyph-shape recovery, and exact recovery of RayoMD PDFs; 0.0.10 adds OCR scripts and
rotation, two more performance rounds and a quality round. Harness results:
[0.0.1](../benchmarks/v0-2026-11-05.md), [0.0.2](../benchmarks/v0.0.2-2026-11-06.md),
[0.0.3](../benchmarks/v0.0.3-2026-11-06.md), [0.0.4](../benchmarks/v0.0.4-2026-11-06.md),
[0.0.5](../benchmarks/v0.0.5-2026-11-06.md), [0.0.6](../benchmarks/v0.0.6-2026-11-06.md),
[0.0.7](../benchmarks/v0.0.7-2026-11-07.md), [0.0.8](../benchmarks/v0.0.8-2026-11-07.md),
[0.0.9](../benchmarks/v0.0.9-2026-11-08.md), [0.0.10](../benchmarks/v0.0.10-2026-11-09.md);
performance method and steps: [performance.md](performance.md).

- **M1 Reader:** implemented in full (classic and stream cross-reference data, object streams,
  hybrid files, repair, every filter including CCITT, JBIG2 and JPEG 2000, the Standard security
  handler R2–R6, the page tree, `--inspect`, exact recovery of `rayomd-source/1` PDFs). Gates met:
  page counts (996 of 996), 0 crashes and hangs under ASan, 24 h of clean aggregate fuzzing over
  all 22 targets in the release campaign of 2026-11-09, `--inspect` 0.286 ms fresh-process median
  on odl. Release bar: (d) and (e) met, (a), (b) and (c) not
  ([release check](../benchmarks/release-check-2026-11-09.md)).
- **M2 Text:** the font → Unicode chain including every predefined CMap of Adobe's five CJK
  collections, detection and repair of untrustworthy ToUnicode maps, glyph-shape recovery through
  the OCR add-on, words, lines, duplicates as bold, ligatures, dehyphenation, uncertainty comments.
  Gates met: NID above the best text-layer tool on odl (0.918 core, 0.926 with the add-on) and on
  every untagged synthetic producer; census `broken` pages 0.99 % with the add-on and its script
  pack, 1.71 % without, against pdftotext's 3.45 % on the same pages; speed: 1.22 ms per odl
  document fresh-process median with a native timer, and 0.41 ms/page in process against the
  fastest competitor's 4.88 (edgeparse, in process).
- **M3 Layout:** XY cuts with column checks, obstacles, bridging lines and Breuel's partial order
  as fallback; running headers and footers by repetition and, on single pages, by position, style
  and publication metadata; right-to-left lines (full UAX #9). Gates met: odl NID ≥ docling,
  olmOCR `multi_column` 0.698 ≥ docling's 0.695 (0.718 with the add-on). Open: `headers_footers`
  0.732 (0.721 with the add-on) against docling's 0.898; `long_tiny_text` passes docling only
  with the add-on (0.869 against 0.523); `xelatex-2col` NID 0.936 (gate 0.95).
- **M4 Semantics:** headings with absolute levels (run-in, numbered), lists, code, quotes, GitHub
  alerts, emphasis with nesting, strikethrough, links, footnotes, image files with alt text from
  tags, Unicode math. Gates met: odl MHS 0.840 ≥ docling's 0.827, heading-level accuracy 0.975,
  construct F1 0.951 (gate 0.95, no margin). Open: holdout construct F1 0.836 (gate 0.90), READoc
  arxiv `head_teds` 0.473 against pymupdf's 0.515.
- **M5 Tables:** ruled, horizontal-rule-only, striped, sideways, form and high-confidence
  borderless tables, GFM or HTML with spans. Gates met: synthetic untagged TEDS 0.987, odl TEDS
  0.884 ≥ liteparse's 0.818 and docling's 0.879, olmOCR `table_tests` 0.620 ≥ pymupdf's 0.615
  (0.672 with the add-on). Open: READoc arxiv `table_teds` 0.483 against pymupdf's 0.685.
- **M6 Tagged:** structure tree read, checked and used for blocks, tables, lists, figures and
  order. Tagged synthetic producers: TEDS 0.978 (docling 0.881), construct F1 0.928, above the
  untagged path on every tagged producer. Open: construct F1 gate 0.98 (inline code untagged by
  rayomd-pdfua, chromium's title heading, `<sup>`/`<sub>` math).
- **M7 Product:** `--batch`, `--serve --json`, `--workers`, the record, Windows build, packaging
  done. First performance campaign done (0.0.8: −30 % in-process, −13 % fresh, peak RSS with
  images halved, output byte-identical); third round (2026-11-08, output byte-identical): odl warm
  0.505 ms/page median (static 0.493), 1,563 pages/s, fresh static 1.061 ms, peak RSS no longer
  growing with Flate images, JPEG 2000 decoding faster than OpenJPEG's release build. Open: ≥ 2,000
  pages/s warm, ≤ 1 ms/page fresh, and JPEG/JPEG 2000/CCITT/JBIG2 decoded whole
  ([performance.md](performance.md)).
- **M8 OCR/ML gate:** decided in [ocr_ml_decision.md](ocr_ml_decision.md) (no table ML, no
  reading-order or heading ML) and built as the add-on `rayopdf-ocr` (PP-OCRv6 small detection,
  tiny-first recognition, ncnn, Linux and Windows) with an optional script pack (Cyrillic, Korean,
  Arabic, Devanagari, Tamil, Kannada) and rotation: scanned suite NID 0.919, TEDS 0.737, MHS 0.847,
  construct F1 0.623 against docling with OCR's 0.754 / 0.717 / 0.670 / 0.530. Open: Hebrew,
  Kannada and Arabic quality, inline code and footnotes on OCR'd pages.

## Engineering conventions

Condensed from RayoMD's
[AGENTS.md](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/AGENTS.md)
and
[docs/development/performance.md](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/performance.md);
those files hold the reasons.

- **Naming.** Functions and types are PascalCase (`ReadXrefSection`), constants are `kName`
  (`kMaxFormDepth`), and code lives in the `RayoPdf` namespace.
- **Errors.** Functions that can fail return a status enum (or a result struct that carries one),
  never an exception. Release builds have no exceptions, RTTI or unwind tables; never write `try`,
  `catch`, `throw`, `dynamic_cast` or `typeid`.
- **Memory.** Page work lives in per-page arenas that are reset, not freed, between pages. Nothing
  is allocated per glyph, word or line on hot paths; buffers are reused across pages and
  documents in batch and server modes. Output is written into the caller's buffer.
- **Algorithms.** Single linear scans over input; never rescan per object or per opener. Every
  expansion a hostile file can trigger draws on a document-wide budget.
- **Core tests.** One core-test runner in which every check returns its own unique exit code, so a
  failure names the check.
- **CLI verifier.** A stdlib-only Python verifier (`tests/verify_cli.py`) checks an already-built
  binary as a black box; building and timing stay outside it.
- **Fuzzing.** libFuzzer targets for every untrusted-input entry point, with a replay mode for CI.
  A minimized reproducer of every fuzz finding is committed with its fix.
- **Performance changes.** An A/A run on the same build bounds the noise; the A/B run is pinned
  (`--cpuset-cpus=3`), and the Markdown and image bytes are identical before and after unless the
  change says otherwise.
- **Records.** Measured-and-rejected ideas are written down with their date and numbers, so they
  are not tried again blindly.
