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
        [--no-exact] [--no-annotations] [--report]
rayopdf --inspect IN.pdf
rayopdf --batch …
rayopdf --serve --json
```

`--serve --json` uses RayoMD's protocol-1 request and record shape.

|Exit code|Meaning|
|---|---|
|0|converted|
|2|usage error|
|3|unreadable input|
|10|corrupt beyond repair|
|11|password required|
|12|unwritable output|
|13|unsupported encryption (a handler or revision other than the Standard handler R2–R6)|

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

### Status: 0.0.2 (2026-11-06)

The first converter (0.0.1) covered M1 and the core of M2, M3 and M4 at once, so that every later
step is measured on a working pipeline; 0.0.2 adds tables, images, footnotes, right-to-left and
CJK text. Harness results: [0.0.1](../benchmarks/v0-2026-11-05.md),
[0.0.2](../benchmarks/v0.0.2-2026-11-06.md).

- **M1 Reader:** implemented in full (classic and stream cross-reference data, object streams,
  hybrid files, repair, every non-image filter, the Standard security handler R2–R6, the page
  tree, `--inspect`); exact recovery of RayoMD PDFs is not. Gates met: page counts (996 of 996),
  0 crashes and hangs under ASan. Open: 24 h of fuzzing, the `--inspect` timing.
- **M2 Text:** the font → Unicode chain including Adobe's CJK collections, words, lines,
  duplicates as bold, ligatures, dehyphenation, uncertainty comments. NID is above the best
  text-layer tool on the synthetic corpus (0.954 against 0.921); open: odl (0.909 against 0.914),
  garbage ToUnicode detection.
- **M3 Layout:** XY cuts with column checks, running headers and footers by repetition and by
  position, right-to-left lines (UAX #9 without explicit embeddings). Open: olmOCR
  `multi_column`, `long_tiny_text` and `headers_footers` are below the best measured tool.
- **M4 Semantics:** headings with absolute levels, lists, code, quotes, emphasis, links,
  footnotes, image files, Unicode math; construct F1, MHS and heading-level accuracy above every
  measured tool on the synthetic corpus. Open: odl MHS, image alt text (M6).
- **M5 Tables:** ruled, horizontal-rule-only and high-confidence borderless tables, GFM or HTML
  with spans. Synthetic untagged TEDS 0.949 (gate 0.95); open: odl TEDS 0.645 (gate 0.818).
- **M6–M8:** not started.

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
