# Evaluation contract

This document defines how RayoPDF and its competitors are measured. Every number RayoPDF ever
claims comes from this harness (`tools/eval.py` and `eval/`), run as described here. The first
full run is recorded in [baselines-2026-11-05.md](../benchmarks/baselines-2026-11-05.md); the
milestone gates of [roadmap.md](roadmap.md) are stated against it.

## Purpose

RayoMD's decision record
([arbitrary_pdf_research_decision.md](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/arbitrary_pdf_research_decision.md),
"Consequences") allows a PDF converter only as a separate package with explicit uncertainty, a
representative quality corpus, and evidence on package size, latency, RSS and security. The
harness answers each condition:

|Condition|Answered by|
|---|---|
|Representative quality corpus|A synthetic corpus with exact ground truth over 4 document classes and 11 producers, plus 3 public benchmarks (odl, olmOCR, READoc)|
|Explicit uncertainty|The text-layer census measures how often a born-digital PDF has no usable text (`image_only`, `broken`); the roadmap turns this into uncertainty comments and the M8 OCR gate|
|Latency and RSS|Single-core speed runs: warm pages/s, ms/page, fresh-process cold start, peak RSS|
|Package size|`images.json` records each competitor image; RayoPDF's binary and package size are a release-bar item|
|Security|The census and synthetic corpora are the crash/hang corpora of the roadmap's ASan gates|

## Suites

|Suite|Root (under `benchmark-output/eval/`)|Documents|Ground truth|Role|
|---|---|---|---|---|
|synthetic|`corpus/`|the valid items of `corpus/corpus.json`|`eval/fixtures/md/<fixture>.md`|development; the `weasyprint` producer is the holdout|
|odl|`data/odl-bench/pdfs`|200 single-page PDFs|`data/odl-bench/ground-truth/markdown`|development|
|olmocr|`data/olmOCR-bench/bench_data/pdfs`|1,269 single-page PDFs: `arxiv_math` 522, `headers_footers` 266, `long_tiny_text` 62, `multi_column` 231, `tables` 188|the benchmark's JSONL unit tests|development|
|readoc|`data/READoc-data/sel`|300 multi-page PDFs: `arxiv/<stem>` 150, `github/<stem>` 150|`data/READoc-data/sel-gt/<subset>/<stem>.md`|**holdout**|
|scanned|`scanned/`|the valid items of `scanned/scanned.json`: 398 image-only PDFs (1,020 pages) made from the 199 valid synthetic items, `s300/<producer>/<fixture>` and `s200/<producer>/<fixture>`|`eval/fixtures/md/<fixture>.md`|OCR development; items made from the `weasyprint` producer are the holdout|
|olmocr-scans|`data/olmOCR-bench/bench_data/pdfs`|134 single-page scans: `old_scans` 98, `old_scans_math` 36|the benchmark's JSONL unit tests (`old_scans.jsonl`, `old_scans_math.jsonl`)|OCR development|
|multiscript|`multiscript/pdf/`|63 image-only PDFs (64 pages): text in 9 scripts, upright and turned (§ Multiscript suite)|`multiscript/gt/<id>.md` (the fixture text)|OCR development (scripts and orientation); no holdout|

Suite ids are paths relative to the suite root without `.pdf`. READoc's selection: the ground-truth
stems of each subset (`<subset>_ground_truth/*.md`) sorted by the SHA-256 of the stem, first 150
whose PDF is in `<subset>.zip`; `data/READoc-data/selection.json` records it.

Pinned sources:

|Source|Pin|
|---|---|
|opendataloader-bench|`https://github.com/opendataloader-project/opendataloader-bench` commit `7af1d8f4d0c09f51ea1a5c6ba5f66e993286d109` (Git LFS)|
|olmOCR-bench|HF dataset `allenai/olmOCR-bench` revision `54a96a6fb6a2bd3b297e59869491db4d3625b711`|
|READoc code|`https://github.com/icip-cas/READoc` commit `afa047715184ac319046ce0230a6109cd270e6fc` (no licence: run from the clone, never copied)|
|READoc data|HF dataset `lazyc/READoc` revision `782bf954e4d9a31ae2bcfe14ff59f1f4b2467592`|
|SafeDocs|`CC-MAIN-2021-31-PDF-UNTRUNCATED/zipfiles/0000-0999/0000.zip`, 1,266,879,273 bytes, 1,000 PDFs|

## Synthetic corpus

### Fixtures

`eval/fixtures/md/<id>.md`, listed in `eval/fixtures/manifest.json`. Line 1 of every fixture is
`Sentinel start <ID>.` and its last body paragraph is `Sentinel end <ID>.` (before footnote and
link-reference definitions); `<ID>` is the id prefix uppercased. In `d06-slides` the start
sentinel is the first paragraph of the first slide. Images are `img/chart.png`, `img/icon.png`
and `img/photo.png`, generated deterministically at render time by
`eval/producers/make_images.py`.

|id|Contents|
|---|---|
|c01-headings|H1–H6 (ATX) with 2-sentence paragraphs; a setext H1 and H2; a second H2 branch with H3 → H4|
|c02-paragraphs|8 long English paragraphs: long words, curly quotes, dashes, ellipsis, units, a backslash hard break|
|c03-emphasis|bold, italic, bold-italic, strikethrough, inline code, `_underscore_` forms, nesting, emphasis next to punctuation|
|c04-lists|3-level bullets, ordered 1–12, ordered from 5, ordered inside bullet, task list, loose list, paragraph after a list|
|c05-code|fenced python/json/bash (one 120-character line), a plain fence with indentation and an inner blank line, a tab, inline code|
|c06-tables|3×4, aligned numeric columns, cells with bold/code/link, empty cells, a 40-row table, a 7-column wrapping table|
|c07-links|inline links with titles, autolinks, bare URLs, reference links (full/collapsed/shortcut), email, a wrapping link, an internal link|
|c08-quotes|quote paragraphs, nested quote, quote with a list and a code block|
|c09-footnotes|5 footnotes, one with 2 paragraphs, one referenced from a list item|
|c10-images|3 images with alt text, one inline, one with a title|
|c11-unicode|Polish, German, French, Greek, Russian, Chinese, Japanese, Korean; combining marks; NBSP; fi/ff/ffl words|
|c12-rtl|2 Arabic and 2 Hebrew paragraphs, a mixed LTR/RTL sentence|
|c13-math|inline and display math, a sum, an integral, a 2×2 `pmatrix`|
|c14-misc|thematic breaks, HTML entities, backslash escapes, `<sub>`/`<sup>`|
|c15-alerts|the 5 GitHub alerts|
|d01-tester|RayoMD's `tester.md` without images and page breaks, with `$`-delimited math|
|d02-business-report|~4 pages: summary, KPI tables, findings, recommendations, footnotes, a note, links|
|d03-user-manual|~6 pages: numbered sections, procedures, shell code, option tables, notes, 2 images|
|d04-paper|~5 pages: title, authors, abstract, numbered sections, math, results table, captioned figure, footnotes, 10 references|
|d05-book-chapter|chapters I–II of *Alice's Adventures in Wonderland* (public domain) with a verse blockquote|
|d06-slides|8 slides: section and slide titles, bullets, a table, code, an image|

### Producers

`eval/producers/render.py` renders every fixture × producer pair in `rayopdf-eval-producers:1`
(Arch Linux pinned to the 2026-11-01 archive) with a 300 s timeout. `P` stands for
`pandoc {in} -f gfm -s --resource-path=.`.

|Producer|Tagged output (measured)|Command|
|---|---|---|
|rayomd|no|`rayomd --export {in} {out} native modern normal --no-image-cache`|
|rayomd-pdfua|yes|the same plus `--pdfua`|
|rayomd-tech|no|`rayomd --export {in} {out} native tech compact --page-size=letter --page-numbers --footnotes=page --compress --no-image-cache`|
|xelatex|no|`P -o {out} --pdf-engine=xelatex -V mainfont="DejaVu Sans" -V monofont="DejaVu Sans Mono" -V CJKmainfont="Noto Sans CJK SC" -V geometry:margin=2.5cm`|
|xelatex-2col|no|xelatex plus `-V classoption=twocolumn -V fontsize=10pt -V geometry:margin=1.8cm`|
|xelatex-eisvogel|no|`P -o {out} --pdf-engine=xelatex --template=eisvogel.latex --syntax-highlighting=idiomatic` with xelatex's fonts; the Eisvogel template 3.5.1 (BSD-3-Clause, release SHA-256 pinned in the producers image). READoc's GitHub subset was made with pandoc + Eisvogel: this development producer shows that layout without opening the holdout (added 2026-11-06, after the baselines)|
|typst|yes|`P -o {out} --pdf-engine=typst -V mainfont="DejaVu Sans"`|
|chromium|yes|`P --mathml --embed-resources` to HTML, then headless Chromium `--print-to-pdf`|
|weasyprint (holdout)|no|`P` to HTML, then `weasyprint`|
|libreoffice|yes|pandoc to DOCX, then `soffice --convert-to pdf`|
|libreoffice-tagged|yes|the same with `UseTaggedPDF=true`|
|beamer|no|`pandoc -t beamer --pdf-engine=xelatex -V mainfont="DejaVu Sans"`|

RayoMD producers never use `--embed-source`. LibreOffice 26.8 tags its PDFs by default, so both
LibreOffice producers are tagged and there is no untagged LibreOffice row. The item-level
`tagged` flag comes from `pdfinfo`, and tagged and untagged items are always reported apart.

**Exclusions** (`manifest.json`): `c12-rtl` is not rendered by rayomd, rayomd-pdfua, rayomd-tech,
xelatex, xelatex-2col, xelatex-eisvogel and beamer; `c13-math` not by rayomd-pdfua, weasyprint,
libreoffice and libreoffice-tagged; `c15-alerts` only by the three rayomd producers; `d06-slides`
only by beamer and rayomd; beamer renders only `d06-slides`.

**Validity.** An item is valid when the producer exits 0, `qpdf --check` exits 0 or 3, and the
whitespace-collapsed `pdftotext -enc UTF-8` output contains both sentinel sentences. Invalid items
keep their reason and are not scored. pandoc's `longtable` cannot be set in two-column mode, so
`xelatex-2col` is invalid on every fixture with a table.

**Classes**, first match wins: `d02` → office; `d04` → academic; `d03`, `d05`, `d06` → book;
producer `libreoffice*` → office; producer `xelatex-2col` → academic; everything else → renderer.

`corpus/corpus.json`: `{"schema":1,"versions":{…},"items":[{"id":"<producer>/<fixture>",
"fixture","producer","class","tagged","pages","sha256","valid","reason","holdout"}]}`.

## Scanned suite

`python3 tools/eval.py corpus --suite scanned` runs `eval/producers/scan.py` in
`rayopdf-eval-producers:1` over the valid items of `corpus/corpus.json` (run `corpus` first).
Every source item becomes two image-only PDFs, one at 300 dpi (`s300/…`) and one at 200 dpi
(`s200/…`):

1. `pdftoppm -r <dpi>` renders every page, gray or (colour variants) RGB.
2. One degradation: `clean`; `noise` (paper darkened to 0.9·v+14, Gaussian noise σ = 10, 0.05 %
   salt and pepper); `skew` (rotation by 0.3–1.2° either way, bicubic, white fill); `blur`
   (Gaussian, radius dpi/250). Random values come from NumPy's PCG64 seeded with the SHA-256 of
   the item id.
3. One codec, a single image XObject per page; bilevel codecs threshold the gray page at 150:

|Codec|Encoder|Sub-variants (rotating)|
|---|---|---|
|jpeg|libjpeg-turbo via Pillow|baseline q85 RGB 4:2:0; baseline q70 gray; progressive q75 RGB|
|ccitt|libtiff Group 4 via Pillow, one strip|`/K -1` DeviceGray; `/BlackIs1 true` with `/Decode [1 0]`; `/ImageMask true`|
|jbig2|jbig2enc 0.30 (Apache-2.0, built from the release tarball, SHA-256 pinned)|generic region; generic region with TPGD (`-d`); symbol mode (`-s`) with one `/JBIG2Globals` stream shared by the document's pages|
|jpx|OpenJPEG 2.5.4 `opj_compress`|JP2 lossy RGB (`-r 20 -I`); raw J2K codestream, gray, `-r 15 -I -p RPCL -n 5 -b 32,32` with precincts, `/ColorSpace` in the dictionary; JP2 lossless gray, 1024² tiles, PCRL; JP2 RGB with 3 quality layers, LRCP, SOP and EPH markers|
|flate|zlib level 6|8-bit gray; 8-bit gray with PNG Up predictor (`/Predictor 15`); 8-bit RGB|

4. An own minimal PDF writer (in `scan.py`) wraps the images: page size = image size at the
   item's dpi, so `pdftoppm -r <dpi>` reproduces the encoded pixels 1:1.

**Matrix.** The valid synthetic items are sorted by id; item *k* gets codec `k mod 5` at 300 dpi
and `(k+2) mod 5` at 200 dpi, degradation `⌊k/5⌋ mod 4` (shifted by 2 at 200 dpi) and the
codec's sub-variant `⌊k/20⌋` (shifted by 1 at 200 dpi). Every block of 20 items holds each
(codec, degradation) pair once per dpi, and no source gets the same codec twice. The 2026-11-07
build: JPEG 211 pages, CCITT 197, JBIG2 211, JPX 202, Flate 199.

**Validity.** An item is valid when every page was encoded, `pdfimages -list` reports the codec's
encoding for every image, `pdftotext` finds no text, and `pdftoppm -r <dpi> -gray` of every page
differs from the gray page that was encoded by a mean absolute difference < 40 (`render_diff`,
the worst page). Splash resamples the 1:1 image by up to a pixel, so lossless pages too differ
by a few levels (at most 14.4 in the 2026-11-07 build, on a noisy page); an inverted or garbled
image differs by about 200. The build is deterministic: a rebuild with the same images
reproduced every item's `sha256`.

`scanned/scanned.json`: `{"schema":1,"versions":{…},"items":[{"id":"s<dpi>/<producer>/<fixture>",
"source","fixture","producer","class","source_tagged","tagged":false,"dpi","codec","codec_variant",
"colour","degradation","holdout","pages","bytes","sha256","render_diff","valid","reason"}]}`.
Scored like the synthetic suite (odl evaluator on `s<dpi>__<producer>__<fixture>.md`,
`constructs.py` on the nested layout). Items made from `weasyprint` items are the holdout:
aggregate scores only.

## Metrics

A conversion that errors, times out or is missing scores as empty output in every scorer, and so
does output that is only white space: olmOCR's text-presence test passes on a lone newline (11 of
the 12 tests of the image-only page `long_tiny_text/10a_pg1`), which would reward tools that write
nothing.

### opendataloader-bench evaluator (unmodified, commit `7af1d8f`)

Run from the clone's `src/evaluator.py` in `rayopdf-eval-scorer:1` on `odl` and on the synthetic
suite (flat layout `<producer>__<fixture>.md`). Per document:

- **NID** = `rapidfuzz.fuzz.ratio` of the whitespace-collapsed Markdown of GT and prediction
  (Markdown tables converted to HTML first), / 100. **NID-S** is the same with HTML tables removed.
- **TEDS** = PubTabNet tree-edit-distance similarity over the HTML tables (`th` treated as
  `td`); **TEDS-S** compares structure only. Undefined when the GT has no table.
- **MHS** = APTED similarity of a flat tree of ATX headings and the content between them; heading
  levels are ignored and setext headings are not recognised. **MHS-S** ignores text. Undefined
  when the GT has no heading; 0 when the prediction has none.
- **overall** = mean of the defined NID, TEDS and MHS.

Suite numbers are means over the documents where the metric is defined. MHS's blindness to
heading levels and NID's raw-Markdown comparison are why the construct scorer exists.

### Construct scorer (`eval/scorer/constructs.py`)

Both sides are parsed with pandoc 3.13 (`-f gfm` with footnotes, tex_math_dollars, alerts,
task_lists, strikeout, pipe_tables and autolink_bare_uris) and walked into typed items:

|Category|Item|Attributes checked on matched pairs|
|---|---|---|
|heading|level, text|level exact|
|list_item|ordered, depth, task state (☐/☒), text of first block|depth exact, ordered flag exact, task state exact|
|code_block|text (tabs → 4 spaces, lines rstripped, trailing blank lines removed)|text identical|
|quote_para|paragraphs directly inside a blockquote at any depth|–|
|alert|type (note/tip/important/warning/caution), text|type exact|
|link|text, URL (inline, reference and autolinks)|URL exact after lowercasing scheme and host and stripping one trailing `/`|
|strong, emph, strike, code_inline, sup, sub, footnote|text (`<sup>`/`<sub>` raw HTML pairs count)|–|

Text is NFKC-normalised, whitespace-collapsed and stripped. Per category, all GT × prediction
pairs are scored with `rapidfuzz.fuzz.ratio`; pairs ≥ 90 (≥ 95 for code blocks) are matched
greedily one-to-one by descending score. Precision = matched / predicted, recall = matched / GT,
F1 = 2·matched / (GT + predicted) (null only when both are empty).

- **math**: a GT formula counts as found when `rapidfuzz.fuzz.partial_ratio(gt, P) ≥ 80`, where P is
  `pandoc -t plain` of the prediction. GT formula text is pandoc's Unicode plain-text rendering;
  for TeX that the plain writer leaves as TeX (`\frac`, `\int`/`\sum` with limits, matrices) it is
  the text of pandoc's MathML rendering, and math in the prediction is rendered the same way.
  Recall only.
- **image**: count recall = min(predicted, GT) / GT, alt-text match rate, and the rate of image
  targets that exist relative to the prediction file.

Report aggregates are micro-averages over documents ("construct F1 (all)" pools every category).
`eval/scorer/selftest.py` (`tools/eval.py score --selftest`) is the permanent check that the
metrics react correctly: identity scores 1.0; an empty prediction has recall 0; raising every
heading level one step keeps heading F1 at 1.0 with level accuracy 0 (H6 becomes H5); changing one
link URL gives URL accuracy (n−1)/n; flattening lists keeps item F1 at 1.0 and lowers depth
accuracy.

### olmOCR-bench

`python -m olmocr.bench.benchmark --dir /work/bench_data --candidate <tool> --skip_baseline` in
`rayopdf-eval-olmocr:1` (olmocr 0.4.27). The work directory holds `pdfs/<category>` links for the
scored suite's categories only (the benchmark requires a candidate file for every PDF under
`pdfs/`): the 5 born-digital categories for `olmocr`, `old_scans` and `old_scans_math` for
`olmocr-scans`; their JSONLs; and `<tool>/<category>/<name>_pg1_repeat1.md`. Reported: pass rate
per JSONL (tests passed / tests); for `olmocr` also the mean of the 4 born-digital JSONLs without
`arxiv_math`, for `olmocr-scans` the mean of its 2 JSONLs. Markdown syntax is ignored by these
tests; text presence/absence, reading order, table cell neighbourhood and KaTeX-rendered math are
checked.

### READoc

`python /data/READoc/src/evaluation/suite_e2e.py --gold_dir … --pred_dir … --result_json …` per
subset in `rayopdf-eval-readoc:1` (pandoc 3.1.11.1, the `pandoc` Python package 2.4, NLTK
wordnet baked in). Keys: `text_eds`, `text_f1` (text edit similarity and vocabulary F1),
`head_eds`, `head_teds` (heading concatenation and ToC tree), `inline_eds`, `outline_eds`
(formulas), `table_eds`, `table_teds`, `seg_kt`, `word_kt`, `seg_sp`, `word_sp` (reading order,
Kendall tau and Spearman, block and token level); the GitHub subset has no formula or table keys.
READoc normalises headings, formulas and tables and strips links and images before scoring.

## Multiscript suite

`python3 tools/eval.py corpus --suite multiscript` runs `eval/producers/multiscript.py` (the
repository's `eval/producers/` mounted over the image's) in `rayopdf-eval-producers:1`. The texts
are `eval/fixtures/multiscript/<script>.md`, original prose written for this suite (Apache-2.0,
like the repository): latin (English), cyrillic (Russian, Ukrainian, Bulgarian), hangul, arabic
(with a Persian paragraph), hebrew, devanagari (Hindi, Nepali, Marathi), kannada, tamil, and mixed
(English, Russian, Korean, Hindi, Arabic paragraphs). Each is set by Pillow with raqm (complex
shaping; right-to-left paragraphs right-aligned) in Noto Sans or Noto Serif of its script, 11 pt,
A4, 2.2 cm margins, gray, and written as an image-only PDF (Flate, one image per page; no
`/Rotate`). Items `<script>-<variant>`: `s300-sans-clean`, `s300-serif-clean`, `s200-sans-clean`,
`s300-sans-noise` (scan.py's noise), and `rot90`, `rot180`, `rot270` (the 300 dpi sans page turned
clockwise, the page box following). Ground truth is exact: `gt/<id>.md` is the fixture text,
`gt/<id>/p<N>.txt` each page's paragraphs; `pgm/<id>/p<N>.pgm` the pages' pixels for the add-on.
The same inputs give the same bytes.

Scoring (`score --suite multiscript`, `eval/scorer/ocr_text.py` in `rayopdf-eval-scorer:1`): the
tool's Markdown and the text are both stripped of Markdown markup and HTML comments, zero-width
(non-)joiners dropped, NFKC and whitespace collapsed; CER = Levenshtein / GT characters, word F1 on
whitespace tokens with end punctuation stripped; per script, per variant group and in total (mean
and pooled CER). Reading order counts in the CER. All items are development: the suite measures
recognition of scripts and orientation, not structure, and its texts are not tuned on; report it
per script.

## Competitors

One image per tool, `rayopdf-eval-<tool>:1`, built from `eval/docker/tool-<tool>.Dockerfile`; the
shared adapter `eval/competitors/convert.py` runs each. "Text-layer tools" are the first six;
docling is the ML reference. docling-ocr and tesseract are the OCR references: `all` runs them on
the scanned suites only (`scanned`, `olmocr-scans`, `multiscript`); every other tool runs on every
suite.

|Tool|Version|Licence|Mode|Timing|
|---|---|---|---|---|
|pdftotext|Poppler 25.03.0 (Debian)|GPL|`pdftotext -enc UTF-8` (plain text, no Markdown)|fresh-process|
|markitdown|0.1.9 (pdfminer.six 20260107)|MIT|`MarkItDown().convert(p).text_content`|in-process|
|pymupdf|PyMuPDF 2.1, pymupdf4llm 2.1, pymupdf_layout 2.1|AGPL-3.0|`pymupdf.open(p).to_markdown()`, which PyMuPDF 2.1 provides only once pymupdf4llm is installed (the image installs it because bare PyMuPDF 2.1 lacks the method); layout GNN on (pymupdf4llm's default when `pymupdf_layout` is installed); no OCR engine installed|in-process|
|opendataloader|opendataloader-pdf 2.5.19, OpenJDK 21|Apache-2.0|local mode: `opendataloader-pdf <dir> -f markdown --image-output off -q`, one JVM per list|batch|
|liteparse|2.18.0|Apache-2.0|`lit parse --format markdown --no-ocr --image-mode off`|fresh-process|
|edgeparse|0.3.2|Apache-2.0|`edgeparse.convert(p, format="markdown")` (returns the Markdown string)|in-process|
|docling|2.147.0, CPU torch, models in `/models`|MIT|`DocumentConverter` with `PdfPipelineOptions(artifacts_path="/models", do_ocr=False)`, `export_to_markdown()`|in-process|
|docling-ocr|the docling image (no image of its own): docling 2.147.0, RapidOCR 3.10.0 on torch (docling's auto OCR picks it because onnxruntime is not installed), PP-OCRv6 small det + rec and the mobile v2 angle classifier from `/models/RapidOcr` (downloaded by `docling-tools models download` at image build; SHA-256 of the `.pth` files: det `fbdc74c9…`, rec `0107b2ad…`, cls `bfe13860…`)|MIT; models Apache-2.0|the same with `do_ocr=True` and docling's default `OcrAutoOptions` (OCR of bitmap regions, not forced full-page)|in-process|
|tesseract|Tesseract 5.5.0 (Debian `tesseract-ocr=5.5.0-1+b1`, `tesseract-ocr-eng`/`-osd=1:4.1.0-2`), Poppler 25.03.0 (`poppler-utils=25.03.0-5+deb13u4`), base `python:3.13-slim` pinned by digest|Apache-2.0 (Poppler GPL)|`pdftoppm -r 300 -gray -png`, then `tesseract <page> out -l eng --psm 1 --dpi 300` per page, single-threaded (`OMP_THREAD_LIMIT=1`), `OMP_NUM_THREADS` pages at a time; plain text, pages joined by blank lines; `versions.json` records the model SHA-256|fresh-process|
|cli|any binary|–|`--cmd` template, binary dir mounted at `/opt/tool`|fresh-process|

marker and MinerU are not run locally (6–54 s/page on CPU per odl-bench). Their odl-bench
predictions shipped with the benchmark are rescored; their olmOCR numbers are cited as claimed.

The adapter writes `<id>.md` and one `results.jsonl` line per document
(`{"id","status":"ok|error|timeout|empty","ms","error"}`) plus `meta.json` with the timing kind and
the image's `versions.json`. `tools/eval.py` mounts the repository's adapter over the image's copy,
so the adapter in use is always the committed one. In-process tools run in one persistent worker
process and are timed there with `perf_counter` around the conversion call only; a document past
the timeout (600 s for docling, tesseract and cli — which OCRs scanned pages when the OCR add-on
sits beside it —, 1,800 s for docling-ocr, 120 s otherwise) kills
the worker and the next document starts a fresh one, so stuck model threads cannot spoil later
documents. Subprocess tools are timed around `subprocess.run`, so their time includes process
start. Quality runs use the work cpuset with `OMP_NUM_THREADS=4`; their times are not speed
results.

## Speed and memory

`tools/eval.py speed --tool T`:

- Inputs: the 200 odl PDFs plus `d03-user-manual` and `d05-book-chapter` from producers rayomd,
  xelatex and libreoffice.
- Container: `--cpuset-cpus=3 --cpus=1 --memory=8g --network none`, with
  `OMP_NUM_THREADS=MKL_NUM_THREADS=OPENBLAS_NUM_THREADS=RAYON_NUM_THREADS=1`. Nothing else runs
  during a speed pass.
- Warm: the adapter with `--warmup 1 --repeat 3` under `/usr/bin/time -f %M`. Per document, the
  median of 3. pages/s = pages of successful documents / sum of their medians; ms/page median and
  p95 over documents. Peak RSS = the maximum RSS of the whole warm run.
- Cold start: 5 fresh adapter processes on one 1-page odl PDF under `/usr/bin/time -f '%e %M'`;
  median wall time and maximum RSS. This includes the Python adapter's start and the tool's
  import.
- Batch tools (opendataloader) report pages/s from the median batch time; per-document times do
  not exist.
- Page counts: `pdfinfo` for odl (`data/odl-pages.json`), `corpus.json` for the synthetic PDFs.

Fresh-process times measured through Python's `subprocess.run` include about a millisecond of
Python overhead on one core; sub-millisecond claims for RayoPDF need a native timer (as RayoMD's
`fresh.sh` does).

## Text-layer census

`eval/census/census.py` in `rayopdf-eval-producers:1` reads each PDF member of
`data/safedocs/0000.zip`, examines at most the first 50 pages with `pdfinfo`, `pdffonts`,
`pdftotext` and `pdfimages -list` (60 s per call), and classifies each examined page, first match
wins:

|Class|Rule|
|---|---|
|image_only|< 50 non-space characters and image coverage ≥ 0.8 (MinerU's thresholds)|
|empty_or_vector|< 50 non-space characters|
|broken|any of docling's four checks: junk (U+FFFD, private use, C0/C1 controls except tab/newline/form feed; a junk bullet at a line start is exempt) ≥ 3 % of characters; junk in ≥ 10 % of lines and ≥ 3 lines; ≥ 50 % of ≥ 50 Latin letters in U+00C0–U+024F; vowel share < 0.25 over ≥ 200 letters in words of 3–20 letters|
|text_over_image|image coverage ≥ 0.9|
|text|everything else|

Image coverage = Σ (w / x-ppi · 72) · (h / y-ppi · 72) over the page's `image` and `stencil`
entries / page area, capped at 1. Per page the dominant script (Latin, Cyrillic, Greek, CJK,
Arabic, Hebrew, Indic, other) is taken over pages with ≥ 20 letters, else the document's.
Producer families come from a case-insensitive regex table on Producer, then Creator (`tex` is
matched as `(?<!i)tex(?!t)` so iText and "Text" do not count). Outputs: `census/census.jsonl` and
`census/summary.json` with class percentages (pages and documents, overall and per family),
tagged/encrypted/Type 3/non-Unicode-font rates, the script mix of `image_only` + `broken` pages, and
`R_upper` = (`image_only` + `broken`) / examined pages, the upper bound of the OCR need before
RayoPDF's own Unicode repair.

## Commands

```sh
python3 tools/eval.py all [--resume]          # everything below, in order, then report.md
python3 tools/eval.py images [--only NAMES]   # build images, write images.json
python3 tools/eval.py fetch [--only odl,olmocr,readoc,safedocs]
python3 tools/eval.py corpus [--producers A,B] [--only FIXTURES] [--jobs 6]
python3 tools/eval.py corpus --suite scanned [--only PRODUCER/FIXTURE,...] [--jobs 6]
python3 tools/eval.py corpus --suite multiscript [--jobs 6]
python3 tools/eval.py score --selftest
python3 tools/eval.py convert --tool T --suite S
python3 tools/eval.py score --suite S [--tools A,B]
python3 tools/eval.py speed --tool T
python3 tools/eval.py census
python3 tools/eval.py report
```

`all --resume` skips images that are built and recorded, an existing corpus and scanned suite,
complete conversions, existing speed results and an existing census. `all` checks for 25 GB of
free disk less what the harness already uses; `fetch` checks for the space its downloads need.
Neither ever prunes Docker images. `all` does not run speed passes for the OCR tools.

Output layout under `benchmark-output/eval/` (ignored by git):

```
images.json                 image ids, sizes and versions.json of every image
corpus/                     <producer>/<fixture>.pdf, corpus.json
scanned/                    s300|s200/<producer>/<fixture>.pdf, scanned.json (about 0.8 GB)
multiscript/                pdf/<id>.pdf, pgm/<id>/p<N>.pgm, gt/, multiscript.json
data/                       odl-bench/, olmOCR-bench/, READoc/, READoc-data/, safedocs/, odl-pages.json
suites/<suite>/list.txt     the ids of the last conversion of that suite
runs/<tool>/<suite>/        <id>.md, results.jsonl, meta.json, batch.json
scores/{synthetic,scanned}/<tool>.json, scores/odl/{work,stored}/,
scores/{olmocr,olmocr-scans}/<tool>.log, scores/readoc/<tool>-<subset>.json,
scores/multiscript/<tool>.json (and .md, the tables)
speed/<tool>.json           and speed/<tool>/ with the raw runs
census/                     census.jsonl, summary.json
report.md
```

## Scoring a new converter

Any converter, RayoPDF included, is scored through the `cli` image (Debian 12 with Python 3).
RayoPDF's fully static build runs there as is:

```sh
python3 tools/dev.py build --config static        # build/static/rayopdf
for s in synthetic odl olmocr readoc scanned olmocr-scans multiscript; do
    python3 tools/eval.py convert --tool cli --suite $s --bin-dir build/static --cmd 'rayopdf --convert {in} {out}'
    python3 tools/eval.py score --suite $s --tools cli
done
python3 tools/eval.py speed --tool cli --bin-dir build/static --cmd 'rayopdf --convert {in} {out}'
python3 tools/eval.py report
```

The `cli` timing is fresh-process through Python's `subprocess.run` (see Speed and memory). Save
a milestone's report as `docs/benchmarks/m<N>-<date>.md`.

## Hygiene

These are AGENTS.md rules 4–8:

- Third-party tools run only inside images built from `eval/docker/*.Dockerfile`. Conversion,
  scoring and census runs use `--network none`, read-only input mounts and
  `-u $(id -u):$(id -g)`. Only `images` and `fetch` use the network.
- Every tool and benchmark is pinned (pip `==`, git commit, HF revision, release SHA-256); each
  image records its resolved versions in `/opt/eval/versions.json`.
- Benchmark claims are dated and scoped (hardware, versions, threads, warm or fresh process).
  Third-party numbers are marked "reproduced" or "claimed".
- Never inspect per-document outputs of the `weasyprint` producer, the scanned items made from
  it, or the `readoc` suite while designing converter heuristics; use aggregate scores only.
  Tagged and untagged producers are reported separately.
- No AGPL or GPL code or data in the repository (PyMuPDF/MuPDF, Poppler, marker): they run as
  black boxes in images. READoc's code is run from a clone and never copied.

## Deviations from the original plan

Recorded so that the next run reproduces the same harness:

- `texlive-langchinese` was added to the producers image: pandoc loads xeCJK whenever
  `CJKmainfont` is set, and xeCJK needs `ctexhook.sty`.
- `numpy==2.4.6` was added to the olmOCR image: `olmocr.bench` imports numpy, which olmocr
  0.4.27 lists only under an unrelated extra.
- READoc's ground truth is in the dataset's `<subset>_ground_truth/` folders, not in the zips; the
  fetch downloads one subset's zip at a time and deletes it after extraction. `hf download` takes
  one pattern per `--include`.
- edgeparse 0.3.2's `convert()` returns the Markdown string; bare PyMuPDF 2.1 has no
  `Document.to_markdown`, so the image installs pymupdf4llm (pinned 2.1), which supplies it.
- The plan's `ITIMER_REAL` timeout for in-process tools is replaced by a killable worker
  process: a SIGALRM inside docling left its table-stage threads stopped and failed every later
  document. The text-layer tools' quality runs of this baseline ran with the earlier adapter
  (`ITIMER_REAL`) and hit no timeout, so their results are the same.
- The odl-bench stored `evaluation.json` of marker, mineru and pymupdf4llm predate the
  benchmark's 2026-01 ground-truth changes ("multiline headings", commits `1691385` and
  `ab65ec2`): their rescored MHS differs; rescored against the ground truth of their own commits
  they match exactly. Every other stored engine matches to 0.000.
- The baseline run of 2026-11-05 first scored olmOCR with whitespace-only outputs as written;
  the same day every tool was rescored with whitespace-only output scored as empty (see Metrics),
  and `docs/benchmarks/baselines-2026-11-05.md` carries the rescored olmOCR numbers.
