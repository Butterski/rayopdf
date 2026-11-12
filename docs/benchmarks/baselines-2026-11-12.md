# Baselines: kordoc, 2026-11-12

[kordoc](https://github.com/chrisryugj/kordoc) (MIT, TypeScript on pdfjs-dist) converts HWP,
HWPX, PDF and Office files to Markdown and claims the top of opendataloader-bench: overall 0.939
with its OCR off and 0.965 by default, its OCR reading the text in images. It joins the harness as
two tools ([evaluation.md](../development/evaluation.md#competitors)): `kordoc`, its OCR off, on
every suite, and `kordoc-ocr`, its default with its OCR models present, on odl.

- kordoc 4.24.66 (npm, exact), Node v22.23.5, pdfjs-dist 4.10.38, onnxruntime-node 1.31.1 and its
  PP-OCRv5 models (`det.onnx`, `rec_korean.onnx`); image `rayopdf-eval-kordoc:1`.
- One Node process a document: `parse()` and its `markdown`, as kordoc's `bench/odl-bench.mjs`
  calls it; offline (`KORDOC_OFFLINE=1`, `--network none`).
- RayoPDF is 0.1.0 as released, core alone (`rayopdf --convert IN OUT`) unless named, with the
  numbers of [its record](v0.1.0-2026-11-12.md). Same machine (AMD Ryzen 5 PRO 4650GE).
  kordoc's numbers are reproduced here.

## opendataloader-bench (200 documents)

| tool | overall | NID | TEDS | MHS |
|---|---|---|---|---|
| kordoc, OCR off | 0.939 | 0.940 | 0.937 | 0.934 |
| kordoc-ocr (its default) | **0.964** | **0.964** | **0.980** | **0.953** |
| RayoPDF 0.1.0 core | 0.896 | 0.917 | 0.889 | 0.840 |
| RayoPDF 0.1.0 with the OCR add-on | 0.902 | 0.926 | 0.889 | 0.843 |
| RayoPDF master after this round (core) | 0.902 | 0.922 | 0.889 | 0.853 |
| docling (ML reference) | 0.884 | 0.902 | 0.879 | 0.827 |

kordoc's claims are reproduced: 0.939 with its OCR off, 0.965 by default (0.964 here). Its OCR
adds most to TEDS: two documents (110 and 122) set their tables as images, which it reads as
tables (TEDS 0 → 0.999 and 0.817); RayoPDF's OCR add-on leaves those images unread (its output
there is the core's). Compared document by document with RayoPDF
(odl is a development suite), the gap is mostly ground-truth conventions RayoPDF departs from on
purpose:

- The ground truth keeps running heads and page numbers on its single pages ("6 CHAPTER 1",
  "84 BAIRD"); RayoPDF removes them, as olmOCR's `headers_footers` tests require (RayoPDF 0.841,
  kordoc 0.421).
- The ground truth writes no emphasis: RayoPDF's `*`, `**` and `<sup>` taken out of its output
  raise NID by 0.0034, MHS by 0.0028 and overall by 0.0031.
- Small capitals: the ground truth writes them as capitals ("FIGURE 1.5"); the PDFs' text is
  "Figure 1.5" (MuPDF reads the same), pdf.js reads capitals.
- Tables whose cells span: the ground truth writes HTML; RayoPDF writes pipe tables where
  READoc's table metric reads them (roadmap D6).

What transfers was taken over (below).

## Synthetic corpus (development producers)

| tool | untagged NID / TEDS / MHS / construct F1 | tagged NID / TEDS / MHS / construct F1 |
|---|---|---|
| kordoc | 0.923 / 0.852 / 0.793 / 0.468 | 0.925 / 0.577 / 0.749 / 0.498 |
| RayoPDF 0.1.0 | **0.960** / **0.987** / **0.944** / **0.969** | **0.971** / **0.978** / **0.933** / **0.951** |
| docling | 0.925 / 0.870 / 0.802 / 0.560 | 0.939 / 0.881 / 0.832 / 0.599 |

## olmOCR-bench, born-digital categories (pass rates)

| tool | headers_footers | long_tiny_text | multi_column | table_tests | arxiv_math |
|---|---|---|---|---|---|
| kordoc | 0.421 | 0.308 | 0.490 | 0.534 | 0.000 (writes no LaTeX) |
| RayoPDF 0.1.0 | 0.841 | 0.509 | **0.709** | **0.632** | **0.720** (`--math=latex`) |
| docling | **0.898** | **0.523** | 0.695 | 0.610 | 0.000 |

## READoc

kordoc writes no LaTeX. arXiv: `text_eds` / `head_teds` / `table_teds` / `seg_kt`; GitHub:
`text_eds` / `head_teds` / `seg_kt`.

| split | tool | arXiv | GitHub |
|---|---|---|---|
| holdout (300, aggregate) | kordoc | 0.787 / 0.297 / 0.322 / 0.958 | 0.835 / 0.405 / 0.981 |
| holdout | RayoPDF 0.1.0, default mode | **0.800** / **0.690** / **0.602** / **0.981** | **0.936** / **0.881** / **0.997** |
| development (300) | kordoc | 0.788 / 0.342 / 0.263 / 0.965 | 0.835 / 0.414 / 0.989 |
| validation (600, aggregate) | kordoc | 0.769 / 0.307 / 0.251 / 0.962 | 0.845 / 0.441 / 0.984 |
| validation | RayoPDF 0.1.0, `--math=latex` | 0.865 / 0.701 / 0.577 / 0.979 | 0.939 / 0.874 / 0.997 |

## Speed

`tools/bench.py tools`, each tool's own command line (kordoc's reads without OCR) as a fresh
process on CPU 3, 2026-11-12 10:12 UTC (load about 16: kordoc-ocr and a fuzz campaign on other
cores); RayoPDF is master at `8e716fa`:

| tool | one page (odl 183) | 33 pages (SafeDocs 0000348) | peak memory |
|---|---|---|---|
| RayoPDF | **1.09 ms** | **12.6 ms** | **8 MiB** |
| kordoc 4.24.66 | 773 ms (712×) | 1,667 ms (132×) | 195 MiB |

## Taken from kordoc

kordoc's code was read, not copied: each idea was written anew in RayoPDF and landed through the
quality gate, which holds synthetic, olmOCR and READoc's development and validation splits next to
odl, so that what lands transfers beyond odl.

| step | idea (kordoc's file) | development effect when it landed |
|---|---|---|
| a list of contents is text, its leader dots one run | `table-roles.ts`, `tab-leaders.ts`, `text-clean.ts` | odl NID +0.0036, MHS +0.0038 |
| a space an Office run leaves out is a word break | pdf.js's spacing | "furtherdata", "3AI" read apart; odl NID +0.0001 |
| a kicker over a slide's title heads nothing | `heading-demote.ts` | odl MHS +0.0061 |
| a size's paragraphs count in the heading-style share | – | odl MHS +0.0030, READoc-dev arXiv `head_teds` +0.0030 |
| turned chart labels read where they stand | – | odl NID +0.0008 |
| a paper's title block past wide affiliations, flush left, under a split title | – | READoc-dev arXiv `head_teds` +0.0056 |
| a supplement's page number ("S84") goes; a line holding an e-mail or web address heads nothing | – (found reading the development outputs) | olmOCR `headers_footers` +0.0013, READoc-dev arXiv `head_teds` +0.0014 |

Together, against 0.1.0: odl overall 0.8965 → 0.9018 (NID 0.9177 → 0.9222, MHS 0.8404 →
0.8534); READoc arXiv `head_teds` 0.700 → 0.710 on the development split and 0.701 → 0.706 on
the validation split; olmOCR `headers_footers` 0.841 → 0.839 (two absent tests: page numbers
that the turned labels used to follow now fall in the tests' last characters) and `multi_column`
0.709 → 0.710; synthetic unchanged. Speed: 0.77 % more instructions over odl; warm on CPU 3
(2026-11-12 10:36 UTC, load about 3), 2,199 → 2,188 pages/s. The holdouts were not run.

## Notes

- kordoc's lead on odl rests partly on fitting odl's 200 documents: its repository keeps odl
  studies document by document, and on every other suite it trails RayoPDF on every key above.
- kordoc-ocr ran on odl only: its OCR took more than 10 s on 72 of odl's 200 documents (up to
  146 s, one core during the quality run; not a speed result), and on the scanned suites about
  two and a half minutes a page, where its runs were stopped. `tools/eval.py all` runs it on odl
  and the scanned suites.
- One per-document comparison of synthetic outputs during the word-spacing step included the
  holdout producer (weasyprint) by mistake; it showed a right-to-left word split, and the step
  then kept its rule out of right-to-left text, on the general ground that shaped text leaves
  room inside words. The holdout's aggregate stays the measure of that step.
