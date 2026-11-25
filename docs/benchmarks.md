# Benchmarks

Where RayoPDF 1.0.0 stands against other PDF converters, on public benchmarks and on test sets
of this project. RayoPDF 1.0.0 converts every document exactly as 0.5.0 did (the same converter,
byte for byte the same output), so 0.5.0's measurements below are 1.0.0's. Every number on this
page was measured by this project on one machine, unless it is marked *claimed*. The
[dated records](benchmarks/README.md) hold every number of every version; this page summarises
them.

- [Summary](#summary)
- [Machine and dates](#machine-and-dates)
- [The benchmarks](#the-benchmarks)
- [The tools](#the-tools)
- [The benchmarks' conventions](#the-benchmarks-conventions)
- [Results](#results)
- [Speed](#speed)
- [Where other tools lead](#where-other-tools-lead)
- [What was not measured](#what-was-not-measured)
- [Reproducing the numbers](#reproducing-the-numbers)

## Summary

- **Quality.** With each benchmark's conventions, RayoPDF scores highest overall on
  opendataloader-bench (0.944), on READoc's arXiv papers (text, headings, tables and formulas)
  and GitHub READMEs, on olmOCR-bench's multi-column, table and arXiv math tests, and on a
  synthetic corpus with exact ground truth. Other tools lead in some places: see
  [Where other tools lead](#where-other-tools-lead).
- **Speed.** RayoPDF converts a one-page PDF in 1.01 ms as a fresh process, 17 times faster than
  pdftotext and more than 1,000 times faster than opendataloader, markitdown, PyMuPDF and
  docling, and 2,207 pages a second on one core in a running process.
- **Scope.** RayoPDF reads the PDF's text layer. Scanned pages need its OCR add-on. Converters
  built on large vision models were not run; on scanned pages they are expected to be stronger.

## Machine and dates

- AMD Ryzen 5 PRO 4650GE (12 logical CPUs), Docker 29.8.2. Every tool ran in its own container
  image, pinned to an exact version, without network.
- RayoPDF 0.5.0, the converter of 1.0.0: quality and speed measured on 2026-11-24 (code
  `a95ad7f`; [record of 1.0.0](benchmarks/v1.0.0-2026-11-25.md)).
- The text-layer tools and docling: 2026-11-05 ([baselines](benchmarks/baselines-2026-11-05.md));
  on the synthetic corpus again on 2026-11-06, when it grew to 199 documents
  ([record of 0.0.3](benchmarks/v0.0.3-2026-11-06.md)).
- docling with OCR and Tesseract on scanned pages: 2026-11-07
  ([OCR baselines](benchmarks/baselines-scanned-2026-11-07.md)).
- kordoc: 2026-11-12 ([kordoc baselines](benchmarks/baselines-2026-11-12.md)).
- Speed of the other tools: 2026-11-09 ([record of 0.0.12](benchmarks/v0.0.12-2026-11-10.md));
  kordoc's on 2026-11-12.

Quality runs used 8 cores; speed runs one pinned core with nothing else of this project running.
Times are from a native timer, not from Python.

## The benchmarks

| Benchmark | What it measures | Documents | Source and licence |
|---|---|---|---|
| [opendataloader-bench](https://github.com/opendataloader-project/opendataloader-bench) | Markdown against expected Markdown: text and reading order (NID), tables (TEDS), headings (MHS); overall is their mean. | 200 single-page PDFs | Commit `7af1d8f`; repository Apache-2.0, pages from DP-Bench (MIT). |
| [olmOCR-bench](https://huggingface.co/datasets/allenai/olmOCR-bench) | Pass rate of unit tests: text present or absent, reading order, table cells, formulas rendered with KaTeX. | 1,269 born-digital single pages in five categories (arXiv math 522, headers and footers 266, long tiny text 62, multi-column 231, tables 188), and 134 old scans | Revision `54a96a6`; ODC-BY-1.0. |
| [READoc](https://github.com/icip-cas/READoc) | Whole documents against Markdown made from their sources: text, headings (as a tree), tables, formulas, reading order. | 300 multi-page documents: 150 arXiv papers, 150 GitHub READMEs | Data `lazyc/READoc` revision `782bf95` (MIT); evaluator commit `afa0477`, which has no licence and is run from its own repository. |
| [DocLayNet](https://github.com/DS4SD/DocLayNet) | Whether text is kept, and the roles given to it (page headers and footers, headings, lists, footnotes, tables), against human annotations. | 6,489 pages (its val split) and 4,999 (its test split) | Version 1.0.0; CDLA-Permissive-1.0. |
| [OmniParseBench](https://huggingface.co/datasets/datalab-to/omni_parse_bench) | Unit tests on each page's HTML (text, order, tables, math) and its layout blocks; graded by `omni-parse-bench` 1.5.0. | Its 2,149 PDF pages, in two halves of 1,074 and 1,075 | Tests CC BY 4.0, pages under their sources' licences; two non-commercial sources are only scored, never used otherwise. |
| Synthetic corpus | Markdown against the exact source: NID, TEDS, MHS, and F1 of Markdown constructs (headings, lists, code, quotes, alerts, links, emphasis, footnotes, …). | 21 Markdown documents rendered to PDF by up to 14 producers (pandoc with XeLaTeX, ConTeXt, Typst, Chromium, WeasyPrint, LibreOffice, Beamer, RayoMD) | This project's, Apache-2.0 ([fixtures](../eval/fixtures/)). |
| Scanned pages | The same measures on image-only PDFs. | 398 PDFs (1,020 pages) made from the synthetic PDFs at 300 and 200 dpi, with five image codecs and four kinds of damage (noise, skew, blur, none) | This project's. |

Two sets are kept as **test sets**: READoc's 300 documents above and the synthetic PDFs rendered
by WeasyPrint (with their scanned copies). They are measured once per release and their
documents are never looked at while RayoPDF is developed, so their scores show how well the
rules carry over to documents they were not made for. Other READoc documents, DocLayNet's pages
and OmniParseBench's pages are used for development, each with a part that is only ever scored
as a whole. Tagged and untagged PDFs are reported apart.

## The tools

| Tool | Version | Licence | How it ran |
|---|---|---|---|
| pdftotext | Poppler 25.03.0 | GPL | `pdftotext -enc UTF-8` (plain text, no Markdown) |
| markitdown | 0.1.9 (pdfminer.six 20260107) | MIT | `MarkItDown().convert(p).text_content` |
| PyMuPDF | PyMuPDF, pymupdf4llm and pymupdf_layout 2.1 | AGPL-3.0 | `to_markdown()` with its layout model on, no OCR |
| opendataloader | opendataloader-pdf 2.5.19, OpenJDK 21 | Apache-2.0 | local mode, Markdown, no images |
| liteparse | 2.18.0 | Apache-2.0 | `lit parse --format markdown --no-ocr --image-mode off` |
| edgeparse | 0.3.2 | Apache-2.0 | `edgeparse.convert(p, format="markdown")` |
| docling | 2.147.0, CPU | MIT | OCR off; its layout and table models on |
| docling with OCR | the same, with RapidOCR 3.10.0 and PP-OCRv6 | MIT, models Apache-2.0 | on scanned pages and in comparisons with the OCR add-on |
| Tesseract | 5.5.0, `eng` | Apache-2.0 | pages rendered at 300 dpi, plain text |
| kordoc | 4.24.66, Node 22, pdf.js 4.10.38 | MIT | as its own benchmark script runs it, OCR off; also with its OCR on (its default) on opendataloader-bench |

The first six are the **text-layer tools**: like RayoPDF they read the PDF's text, without
large models. docling, which runs layout and table models, is the machine-learning reference.
kordoc reports the top score on opendataloader-bench. Each tool ran with its own default output.

## The benchmarks' conventions

Benchmarks expect different things from a converter, and some of their expectations contradict
each other. opendataloader-bench's expected Markdown keeps running headers and page numbers,
which olmOCR-bench's tests require to be gone. READoc's arXiv papers write formulas as LaTeX;
RayoPDF's default writes Unicode. RayoPDF's default output stays faithful to the page, and
options adapt it to a reader's conventions ([command-line guide](guide/cli.md#options-for-a-readers-conventions)).

Each benchmark is measured with one fixed, published set of these options (in
[`eval/profiles.json`](../eval/profiles.json)), chosen from what its ground truth contains and
the same for every document; [evaluation.md](development/evaluation.md#benchmark-profiles)
records when each option was added. Tables on this page show the default output beside it.

| Benchmark | Options | Why |
|---|---|---|
| opendataloader-bench | `--images=none --furniture=keep --plain --tables=html --quotes=ascii --small-caps=keep` | Its expected Markdown keeps page headers and footers, has no emphasis or links, writes tables as indented HTML and quotation marks as `'` and `"`, and writes headings set in small capitals in capitals. |
| olmOCR-bench | `--plain --small-caps=keep --figure-text=drop` | Its tests strip only bold and italics before comparing, so links, `<sup>` tags and footnote marks would count as text; they compare small-capital titles as printed. |
| olmOCR-bench, arXiv math | the same plus `--math=latex` | Its tests compare formulas as LaTeX. |
| READoc | `--math=latex --tables=pipe-latex --figure-text=drop` | Its ground truth comes from LaTeX sources: formulas in LaTeX, no text from inside figures. Its evaluator reads pipe tables only and passes each cell through LaTeX, where a bare `%` ends the row. |
| DocLayNet, OmniParseBench | `--format=json --images=none` (OmniParseBench also `--math=latex`) | Their scorers read the blocks' positions; OmniParseBench's math tests compare LaTeX. |
| Synthetic corpus | none (the default output) | Its ground truth is ordinary Markdown. |

## Results

Higher is better everywhere. **Bold** marks the best value in a column.

### opendataloader-bench, 200 pages

| Tool | Overall | Text (NID) | Tables (TEDS) | Headings (MHS) |
|---|---:|---:|---:|---:|
| RayoPDF 0.5.0, the benchmark's conventions | **0.944** | **0.954** | 0.934 | 0.927 |
| RayoPDF 0.5.0, default output | 0.917 | 0.926 | 0.915 | 0.898 |
| kordoc 4.24.66, OCR off | 0.939 | 0.940 | **0.937** | **0.934** |
| liteparse | 0.885 | 0.914 | 0.818 | 0.822 |
| docling | 0.884 | 0.902 | 0.879 | 0.827 |
| PyMuPDF | 0.866 | 0.906 | 0.773 | 0.782 |
| opendataloader | 0.838 | 0.914 | 0.427 | 0.760 |
| edgeparse | 0.838 | 0.897 | 0.639 | 0.729 |
| pdftotext | 0.597 | 0.897 | 0.000 | 0.000 |
| markitdown | 0.589 | 0.844 | 0.273 | 0.000 |

With OCR, on the default output: RayoPDF with its OCR add-on 0.924 (NID 0.934, TEDS 0.915, MHS
0.902), docling with OCR 0.889 (0.906, 0.925, 0.821), kordoc with its OCR 0.964 (0.964, 0.980,
0.953). Two of the benchmark's pages set their tables as images, which kordoc's OCR reads as
tables and RayoPDF's add-on does not.

The benchmark ships the outputs of engines that were not run here. Rescored with the same
evaluator against the same ground truth: opendataloader hybrid 0.907, nutrient (commercial)
0.885, marker 0.867, MinerU 0.835. *Claimed*, on other machines and not reproduced: kordoc
4.15.7 in a plain-output mode 0.949 and LM-Kit One 0.948 (opendataloader-bench pull requests
[#35](https://github.com/opendataloader-project/opendataloader-bench/pull/35) and
[#34](https://github.com/opendataloader-project/opendataloader-bench/pull/34)).

### olmOCR-bench, born-digital pages (pass rates)

| Tool | arXiv math | Headers and footers | Long tiny text | Multi-column | Tables |
|---|---:|---:|---:|---:|---:|
| RayoPDF 0.5.0, the benchmark's conventions | **0.860** | 0.891 | **0.525** | **0.762** | **0.653** |
| RayoPDF 0.5.0, default output | – | 0.890 | 0.509 | 0.732 | 0.646 |
| docling | 0.000 | **0.898** | 0.523 | 0.695 | 0.610 |
| liteparse | 0.538 | 0.554 | 0.240 | 0.662 | 0.559 |
| PyMuPDF | 0.000 | 0.385 | 0.337 | 0.667 | 0.615 |
| pdftotext | 0.000 | 0.313 | 0.471 | 0.653 | 0.000 |
| edgeparse | 0.000 | 0.490 | 0.251 | 0.462 | 0.401 |
| opendataloader | 0.000 | 0.361 | 0.348 | 0.638 | 0.247 |
| markitdown | 0.000 | 0.386 | 0.312 | 0.393 | 0.249 |
| kordoc 4.24.66, OCR off | 0.000 | 0.421 | 0.308 | 0.490 | 0.534 |

The arXiv math tests need LaTeX, so the default output (Unicode formulas) is not scored on them.
60 of these 1,269 pages have no text layer, 23 of them in "long tiny text"; with the OCR add-on
RayoPDF passes 0.879 / 0.869 / 0.749 / 0.701 of the last four categories in its default output,
against docling with OCR's 0.873 / 0.769 / 0.733 / 0.684.

### READoc, 300 documents (test set)

arXiv papers (150):

| Tool | Text | Headings | Tables | Inline formulas | Reading order |
|---|---:|---:|---:|---:|---:|
| RayoPDF 0.5.0, the benchmark's conventions | **0.893** | **0.719** | **0.730** | **0.687** | 0.983 |
| RayoPDF 0.5.0, `--math=latex` alone | 0.887 | 0.718 | 0.609 | 0.685 | 0.983 |
| RayoPDF 0.5.0, default output | 0.803 | 0.716 | 0.604 | – | 0.982 |
| docling | 0.841 | 0.403 | 0.656 | 0.001 | **0.984** |
| liteparse | 0.826 | 0.254 | 0.407 | 0.369 | 0.969 |
| PyMuPDF | 0.754 | 0.515 | 0.685 | 0.001 | 0.976 |
| kordoc 4.24.66 | 0.787 | 0.297 | 0.322 | – | 0.958 |

GitHub READMEs (150):

| Tool | Text | Headings | Reading order |
|---|---:|---:|---:|
| RayoPDF 0.5.0, default output | **0.936** | **0.882** | **0.997** |
| docling | 0.908 | 0.471 | 0.993 |
| pdftotext | 0.874 | 0.021 | 0.956 |
| liteparse | 0.761 | 0.778 | 0.989 |
| opendataloader | 0.803 | 0.763 | 0.990 |
| kordoc 4.24.66 | 0.835 | 0.405 | 0.981 |

The READoc keys are `text_eds`, `head_teds`, `table_teds`, `inline_eds` and `seg_kt`; the other
tools' values for every key are in the [baselines](benchmarks/baselines-2026-11-05.md#6-readoc-300-holdout-aggregate-only).

### Synthetic corpus

Text (NID), tables (TEDS), headings (MHS) and Markdown constructs (F1), on the 11 producers
every tool ran on:

| Tool | Untagged PDFs | Tagged PDFs | Test set (WeasyPrint) |
|---|---|---|---|
| RayoPDF 0.5.0 | **0.960** / **0.986** / **0.942** / **0.971** | **0.972** / **0.978** / **0.933** / **0.953** | **0.935** / 0.792 / **0.853** / **0.848** |
| docling | 0.925 / 0.870 / 0.802 / 0.560 | 0.939 / 0.881 / 0.832 / 0.599 | 0.910 / **0.877** / 0.802 / 0.513 |
| Best text-layer tool | 0.920 / 0.845 / 0.845 / 0.628 | 0.931 / 0.775 / 0.857 / 0.594 | 0.909 / 0.774 / 0.751 / 0.562 |
| kordoc 4.24.66 | 0.923 / 0.852 / 0.793 / 0.468 | 0.925 / 0.577 / 0.749 / 0.498 | – |

"Best text-layer tool" takes the best of the six for each number (PyMuPDF or liteparse). On the
two producers added later (Chromium without tags and ConTeXt), which no other tool ran on,
RayoPDF scores 0.964 / 0.889 / 0.929 / 0.866.

### DocLayNet and OmniParseBench

Only RayoPDF writes the block positions these scorers read, so no other tool is listed.

| Benchmark | Measures | RayoPDF 0.5.0 | With the OCR add-on |
|---|---|---|---|
| DocLayNet, val split (6,489 pages) | text kept / page headers and footers F1 / headings F1 / lists F1 / footnotes F1 / tables F1 | 0.988 / 0.636 / 0.775 / 0.732 / 0.766 / 0.777 | – |
| DocLayNet, test split (4,999 pages) | the same | 0.983 / 0.508 / 0.732 / 0.687 / 0.711 / 0.779 | – |
| OmniParseBench, first half (1,074 pages) | headline / text / tables / layout | 0.709 / 0.703 / 0.595 / 0.830 | 0.751 / 0.768 / 0.602 / 0.883 |
| OmniParseBench, second half (1,075 pages) | the same | 0.685 / 0.699 / 0.553 / 0.803 | 0.737 / 0.776 / 0.562 / 0.873 |

### Scanned pages

| Tool | Synthetic scans (362 PDFs): NID / TEDS / MHS / constructs F1 | Synthetic scans, test set (36 PDFs) | olmOCR-bench old scans |
|---|---|---|---|
| RayoPDF 0.5.0 with the OCR add-on | **0.918** / **0.771** / **0.847** / **0.623** | **0.922** / **0.808** / **0.846** / **0.575** | 0.213 |
| docling with OCR | 0.754 / 0.717 / 0.670 / 0.530 | 0.700 / 0.666 / 0.671 / 0.536 | **0.251** |
| Tesseract | 0.856 / 0.000 / 0.013 / 0.219 | 0.847 / 0.000 / 0.015 / 0.236 | 0.211 |

docling with OCR writes nothing for JPEG 2000 pages in JP2 files (49 of the 362); without them
it scores NID 0.873. Text-layer tools write nothing on these pages, and empty output already
passes 0.133 of olmOCR-bench's old-scan tests. On 63 image-only PDFs in nine scripts, RayoPDF with
the add-on and its script pack has a pooled character error rate of 0.203 and a word F1 of
0.718.

## Speed

Each tool converts the same PDF through its own command line, as a fresh process, start to exit,
on one pinned core, output to a RAM disk; the median of 3 to 40 runs after warm-up runs. The one
page is document 183 of opendataloader-bench (its median page, text and tables); the report is
SafeDocs document 0000348 (33 pages).

| Tool | Version | One page | × | 33-page report | × | Peak memory |
|---|---|---:|---:|---:|---:|---:|
| **RayoPDF** | 0.5.0 | **1.01 ms** | 1 | **10.8 ms** | 1 | **7 MiB** |
| pdftotext (plain text only) | Poppler 25.03 | 17 ms | 17 | 46 ms | 4.3 | 18 MiB |
| liteparse | 2.18.0 | 27 ms | 27 | 138 ms | 13 | 33 MiB |
| edgeparse | 0.3.2 | 45 ms | 45 | 306 ms | 28 | 27 MiB |
| kordoc | 4.24.66 | 773 ms | – | 1,667 ms | – | 195 MiB |
| opendataloader | 2.5.19 | 1,074 ms | 1,063 | 3,705 ms | 343 | 250 MiB |
| markitdown | 0.1.9 | 1,236 ms | 1,224 | 3,143 ms | 291 | 104 MiB |
| PyMuPDF | 2.1, layout on | 4,749 ms | 4,702 | 70,092 ms | 6,490 | 346 MiB |
| docling | 2.147.0, no OCR | 9,490 ms | 9,396 | 93,052 ms | 8,616 | 1,905 MiB |

kordoc was timed on another day, while other jobs ran on the machine's other cores; its
factors are not given. This compares the time to a finished file, not identical output: the
tools differ in what they recover (above). A fresh process pays each tool's start: the Python
tools import their packages and models, opendataloader starts a Java VM. Inside one running
process the gap is smaller: edgeparse takes about 5 ms a page there, PyMuPDF and docling about
2 s.

- **Four cores:** RayoPDF reads the report's pages on threads and takes 7.3 ms; the other tools
  take 38 ms to 42 s.
- **In a running process** (`--batch`, `--serve --json`): 2,207 pages a second on one core over
  opendataloader-bench's 200 pages, a median of 0.37 ms a page.
- **Memory:** with images off, the median peak per document is 1.5 to 3.2 MiB on each corpus;
  see the [release check](benchmarks/release-check-2026-11-24.md#e-footprint) for every corpus.

### Speed by version

The 200 one-page PDFs of opendataloader-bench without images, on one pinned core: pages a second
in one running process ("warm"), and the median time of a fresh process per document. An empty
static program takes 0.22 ms in the same timer.

| Version | Warm, pages/s | Fresh process, one page | What changed |
|---|---:|---:|---|
| 0.0.7 | about 1,050 | 1.72 ms | the first complete converter |
| 0.0.8 | about 1,500 | 1.50 ms | numbers and glyph names read in one pass, fast paths for ASCII text |
| 0.0.10 | 1,560 | 1.22 ms | faster content parsing and line building, font programs read only when needed |
| 0.0.12 | 2,215 | **0.89 ms** | faster parsing, glyph placement, decompression, fonts and tables; hot code grouped; pages on threads |
| 0.0.13 | 2,138 | 0.91 ms | better headings, tables and text on papers (they cost 3 %), images on page threads |
| 0.1.0 | 2,209 | 0.93 ms | tables, page headers and footers, escaping and script forms; earlier costs won back |
| 0.1.1 | 2,217 | 0.91 ms | lists of contents, slide titles, heading styles and paper title blocks (they cost 1 %) |
| 0.2.0 | **2,231** | 0.89 ms | JSON output, the C++ API, tables in images, table and page-furniture rules; speed work won back their cost |
| 0.3.0 | 2,214 | 0.90 ms | output options, chunks, the layout view, the C ABI, title and page-number rules (they cost 0.5 %: 0.2.0 gave 2,225 and 0.89 ms in the same runs) |
| 0.4.0 | 2,188 | 0.91 ms | pipe tables and ASCII quotes, a small trained model for page headers and footers, table and formula rules (they cost 1.1 %: 0.3.0 gave 2,212 and 0.90 ms in the same runs) |
| 0.5.0 | 2,207 | 0.91 ms | figure-text, small-capitals and LaTeX-table options, LaTeX from font codes, table, heading, list and furniture rules; speed work won back their cost (0.4.0 gave 2,194 and 0.91 ms in the same runs) |

Every speed change is profiled and kept only if a pinned A/B measurement shows it faster, and
it leaves the output byte for byte the same on every benchmark.

## Where other tools lead

- **kordoc on opendataloader-bench.** In RayoPDF's default output kordoc leads overall (0.939
  against 0.917). With the benchmark's conventions RayoPDF leads overall (0.944) and on text,
  but kordoc still leads on tables (0.937 against 0.934) and headings (0.934 against 0.927).
  With OCR, kordoc leads clearly (0.964 against 0.924), because its OCR reads tables set as
  images. Two *claimed* results, kordoc 4.15.7 in a plain-output mode (0.949) and LM-Kit One
  (0.948), are above RayoPDF's 0.944. On every other benchmark, kordoc trails RayoPDF.
- **docling on olmOCR-bench's headers and footers** (0.898 against 0.891 with the conventions,
  0.890 in the default output), and on its long tiny text in the default output (0.523 against
  0.509; with the conventions RayoPDF leads, 0.525).
- **docling on HTML-rendered tables:** on the synthetic test set rendered from HTML by WeasyPrint,
  docling's tables score 0.877 against RayoPDF's 0.792.
- **docling on READoc's arXiv reading order** (0.984 against 0.983).
- **PyMuPDF and docling on READoc's arXiv tables outside READoc's conventions:** 0.685 and 0.656
  against RayoPDF's 0.604 in the default output and 0.609 with `--math=latex` alone. In the
  default output docling and liteparse also lead its arXiv text (0.841 and 0.826 against 0.803),
  because READoc's ground truth writes formulas as LaTeX.
- **docling with OCR** on olmOCR-bench's old scans (0.251 against 0.213) and on
  opendataloader-bench's tables (0.925 against 0.915).

## What was not measured

- **Converters built on vision-language models**, such as olmOCR, MinerU and marker, were not run:
  they are built to run on a GPU (marker and MinerU take 6 to 54 seconds a page on a CPU, by
  opendataloader-bench's figures). marker and MinerU appear only through the outputs that
  opendataloader-bench ships (above). On scanned pages they are expected to be stronger than
  RayoPDF's OCR add-on: the olmOCR-2 model *claims* 82.4 on all of olmOCR-bench, scans included.
- **Commercial services** were not run.
- RayoPDF is the only tool measured on DocLayNet and OmniParseBench here, because their scorers
  need block positions that only RayoPDF's JSON gives.

## Reproducing the numbers

Every quality number comes from the evaluation harness, `tools/eval.py` with the container
images under `eval/docker/`; [evaluation.md](development/evaluation.md) specifies the
benchmarks, pins, metrics and commands. In short:

```sh
python3 tools/eval.py images                      # build the pinned tool and scorer images
python3 tools/eval.py fetch                       # download the benchmarks
python3 tools/dev.py build --config static        # build/static/rayopdf
python3 tools/eval.py convert --tool cli --suite odl --bin-dir build/static --profile declared
python3 tools/eval.py score --suite odl --tools cli
python3 tools/eval.py report
```

`--profile declared` runs RayoPDF with the benchmark's conventions, `--profile default` with its
default output. Speed is measured with `tools/bench.py`
([performance.md](development/performance.md#rerunning)).

The [benchmark records](benchmarks/README.md) list every measured version, the baselines of the
other tools and the release checks. The numbers of 0.5.0 are in
[its record](benchmarks/v0.5.0-2026-11-24.md) and its
[release check](benchmarks/release-check-2026-11-24.md).
