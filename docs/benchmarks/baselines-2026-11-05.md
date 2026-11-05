# RayoPDF baselines, 2026-11-05

Generated 2026-11-05 20:09 by `tools/eval.py report`.

Baseline run of 2026-11-05 on one machine (CPU and versions in § 1). Every number below was
measured here ("reproduced"), except the stored odl-bench engine predictions in § 4, which are
third-party conversions rescored by our scorer. Quality runs used 8 cores with
`OMP_NUM_THREADS=4`; speed runs used one pinned core with thread variables set to 1 (§ 7).
`tools/eval.py all --resume` ended with 0 failed steps. `benchmark-output/eval/report.md` of the
same run is reproduced unchanged from § 1 on.

## Findings

"Text-layer tools" are pdftotext, markitdown, pymupdf (its default layout mode runs a small GNN),
opendataloader, liteparse and edgeparse; docling is the ML reference. "Best" is the highest score
among the text-layer tools; the last column is docling's score minus that score.

### Quality: best text-layer tool per metric and docling's margin

| metric | best text-layer tool | its score | docling | docling − best |
|---|---|---|---|---|
| odl overall | liteparse | 0.885 | 0.884 | 0.000 |
| odl NID | liteparse | 0.914 | 0.902 | -0.012 |
| odl TEDS | liteparse | 0.818 | 0.879 | +0.061 |
| odl MHS | liteparse | 0.822 | 0.827 | +0.005 |
| synthetic untagged NID (class macro) | liteparse | 0.921 | 0.923 | +0.002 |
| synthetic untagged TEDS (class macro) | pymupdf | 0.844 | 0.862 | +0.018 |
| synthetic untagged MHS (class macro) | liteparse | 0.850 | 0.776 | -0.075 |
| synthetic untagged construct F1 (all) | pymupdf | 0.633 | 0.534 | -0.099 |
| synthetic untagged heading-level acc | liteparse | 0.805 | 0.606 | -0.199 |
| synthetic tagged NID (class macro) | pymupdf | 0.931 | 0.939 | +0.008 |
| synthetic tagged TEDS (class macro) | pymupdf | 0.775 | 0.881 | +0.106 |
| synthetic tagged MHS (class macro) | liteparse | 0.857 | 0.832 | -0.025 |
| synthetic tagged construct F1 (all) | pymupdf | 0.594 | 0.599 | +0.005 |
| synthetic tagged heading-level acc | liteparse | 0.972 | 0.575 | -0.397 |
| olmOCR arxiv_math | liteparse | 0.538 | 0.000 | -0.538 |
| olmOCR headers_footers | liteparse | 0.554 | 0.898 | +0.344 |
| olmOCR long_tiny_text | pdftotext | 0.876 | 0.523 | -0.353 |
| olmOCR multi_column | pymupdf | 0.667 | 0.695 | +0.027 |
| olmOCR table_tests | pymupdf | 0.615 | 0.610 | -0.005 |
| olmOCR born-digital mean (excl. arxiv_math) | liteparse | 0.504 | 0.681 | +0.178 |
| READoc arxiv text_eds | liteparse | 0.826 | 0.841 | +0.016 |
| READoc arxiv head_teds | pymupdf | 0.515 | 0.403 | -0.112 |
| READoc arxiv inline_eds | liteparse | 0.369 | 0.001 | -0.368 |
| READoc arxiv table_teds | pymupdf | 0.685 | 0.656 | -0.029 |
| READoc arxiv seg_kt | pymupdf | 0.976 | 0.984 | +0.008 |
| READoc github text_eds | pdftotext | 0.874 | 0.908 | +0.034 |
| READoc github head_teds | liteparse | 0.778 | 0.471 | -0.307 |
| READoc github seg_kt | opendataloader | 0.990 | 0.993 | +0.002 |

- **odl-bench:** liteparse (0.885) and docling (0.884) tie on the overall score. docling leads on
  tables (TEDS +0.061) and by a small margin on headings (MHS +0.005); liteparse leads on reading
  order (NID +0.012). docling's local overall 0.884 is 0.003 above the benchmark's stored docling
  result (0.882).
- **Synthetic corpus:** docling leads on tables (TEDS +0.018 untagged, +0.106 tagged) and is level
  with the best text-layer tool on NID (+0.002 untagged, +0.008 tagged). It trails on headings:
  MHS −0.075 untagged, and heading-level accuracy 0.606 against liteparse's 0.805 (untagged) and
  0.575 against 0.972 (tagged).
- **Markdown constructs:** the best construct F1 over all categories is 0.633 (pymupdf, untagged).
  No tool recovers blockquote paragraphs, GitHub alerts, footnotes or subscripts (F1 0.000 for
  every tool); only pymupdf recovers emphasis to a useful degree (strong 0.291, emph 0.686,
  strikethrough 0.424, inline code 0.752, untagged). No tool, as configured here, writes Markdown
  image references (image recall 0.000 for all).
- **olmOCR-bench:** docling leads the born-digital mean by +0.178 (0.681 against liteparse's
  0.504), mostly through `headers_footers` (0.898 against 0.554). pdftotext is best on
  `long_tiny_text` (0.876 against docling's 0.523). On `arxiv_math` only liteparse scores (0.538):
  it writes LaTeX rebuilt from the text layer; every other tool, docling included, scores 0.000.
- **READoc-300 (holdout, aggregate only):** docling has the best text similarity on both subsets
  (`text_eds` 0.841 arxiv, 0.908 github). Text-layer tools lead on heading structure (`head_teds`
  0.515 pymupdf against docling's 0.403 on arxiv; 0.778 liteparse against 0.471 on github) and
  pymupdf on arxiv tables (`table_teds` 0.685 against 0.656).

### Speed and memory (one pinned core)

- Warm throughput ranges from 0.30 pages/s (docling, 2,003 ms/page median) to 139.7 pages/s
  (edgeparse, in-process, 4.9 ms/page median). pymupdf runs 0.50 pages/s and markitdown 2.18.
- Fresh-process tools: pdftotext 64.3 pages/s (17.1 ms/page median) and liteparse 49.0 pages/s
  (22.5 ms/page); both times include about 1 ms of Python `subprocess` overhead. opendataloader
  converts the whole list in one JVM at 22.1 pages/s, JVM start included.
- Cold start (fresh adapter process, one 1-page PDF) ranges from 0.08 s (liteparse) to 11.15 s
  (docling); peak RSS from 19 MiB (pdftotext) to 2,109 MiB (docling).
- Against RayoPDF's release bar: 2,000 pages/s warm is 14× edgeparse's 139.7 pages/s, and
  ≤ 1 ms/page fresh-process is 17× below pdftotext's measured 17.1 ms/page.

### OCR evidence (census)

- 996 of 1,000 SafeDocs PDFs opened; 8,905 pages examined (at most 50 per file).
- `R_upper` = 12.36 % of examined pages: 8.92 % `image_only` and 3.45 % `broken`. 14.0 % of the
  documents have at least one `image_only` page and 4.1 % at least one `broken` page.
- Script mix of those pages: 52.3 % unknown (no usable text), 43.2 % Latin, 3.7 % other, 0.6 % CJK,
  0.1 % Cyrillic.
- 65.2 % of the processed files have at least one font that `pdffonts` reports without a Unicode
  mapping; 34.2 % are tagged.
- olmOCR's born-digital categories also contain pages without a text layer: pdftotext returns
  nothing for 60 of the 1,269 pages (long_tiny_text 23, headers_footers 13, tables 13,
  multi_column 11).

### Failures

- **Producers:** `xelatex-2col` fails on the 5 fixtures with tables (c06, d01–d04): pandoc writes
  every table as `longtable`, which LaTeX refuses in two-column mode. 181 of 186 corpus items are
  valid; every `rayomd*` item is valid.
- **LibreOffice tags by default:** LibreOffice 26.8 tags its PDFs even without `UseTaggedPDF`, so
  `libreoffice` and `libreoffice-tagged` give identical scores and the corpus has no untagged
  LibreOffice PDF.
- **Tools:** no timeouts. markitdown fails on 7 olmOCR `multi_column` pages (pdfminer
  `PDFKeyError`); edgeparse fails on 2 READoc arXiv documents (content-stream decode errors);
  opendataloader writes no output for 1 olmOCR `tables` page (CLI exit 1). Empty outputs on
  olmOCR are the pages without a text layer above (39 for docling, 3 for liteparse, 59–64 for the
  others).
- **odl stored predictions:** the stored `evaluation.json` of marker, mineru and pymupdf4llm differ
  from our rescoring in MHS only (by 0.006, 0.004 and 0.003 overall); they predate the benchmark's
  2026-01 ground-truth changes and match exactly when rescored against the ground truth of their
  own commits. The other 11 stored engines match to 0.000.

## 1. Setup

- Date: 2026-11-05
- CPU: AMD Ryzen 5 PRO 4650GE with Radeon Graphics (12 logical CPUs); work cpuset `0-2,4,6-8,10`, timing cpuset `3` with `--cpus=1`
- Docker: 29.8.2
- opendataloader-bench `7af1d8f4d0c09f51ea1a5c6ba5f66e993286d109`; olmOCR-bench `54a96a6fb6a2bd3b297e59869491db4d3625b711`; READoc code `afa047715184ac319046ce0230a6109cd270e6fc`, data `782bf954e4d9a31ae2bcfe14ff59f1f4b2467592`; SafeDocs CC-MAIN-2021-31-PDF-UNTRUNCATED `0000.zip`

| image | id | size | versions |
|---|---|---|---|
| producers | 726178aa48db | 6.15 GB | chromium Chromium 156.0.8078.79 Arch Linux; pandoc pandoc 3.11; pdfinfo pdfinfo version 26.10.0; qpdf qpdf version 12.4.2; rayomd rayomd 5.3.0; soffice LibreOffice 26.8.1.1 680(Build:1); typst typst 0.15.1 (9dfd3a08); weasyprint WeasyPrint version 70.0; xelatex XeTeX 3.141592653-2.6-0.999998 (TeX Live 2026/Arch Linux) |
| scorer | 1ede96543a78 | 0.64 GB | git git version 2.47.3; git-lfs git-lfs/3.6.1 (GitHub; linux amd64; go 1.24.4); pandoc pandoc 3.13; python Python 3.13.18 |
| olmocr | 388ea6fbbe59 | 2.66 GB | olmocr 0.4.27; playwright 1.64.1; python Python 3.11.18 |
| readoc | 7bd1b41f30e6 | 1.41 GB | pandoc pandoc 3.1.11.1; python Python 3.11.18 |
| pdftotext | ec3f02f73d6a | 0.25 GB | pdftotext pdftotext version 25.03.0 |
| markitdown | 303b1a2e8858 | 0.53 GB | markitdown Version: 0.1.9 |
| pymupdf | b1a479a28855 | 0.61 GB | pymupdf 2.1 |
| opendataloader | 6ab5b264e148 | 0.52 GB | java openjdk version "21.0.13" 2026-10-20; opendataloader-pdf Version: 2.5.19 |
| liteparse | dca208de497a | 0.24 GB | liteparse lit 2.18.0 |
| edgeparse | 3f05001e5f32 | 0.21 GB | edgeparse Version: 0.3.2 |
| docling | 1f70380d4615 | 5.27 GB | docling Docling version: 2.147.0 |
| cli | 567c1ed7979e | 0.18 GB | python3 Python 3.11.2 |

## 2. Corpus validity

181 of 186 items valid; blank = excluded by the manifest.

| fixture | beamer | chromium | libreoffice | libreoffice-tagged | rayomd | rayomd-pdfua | rayomd-tech | typst | weasyprint | xelatex | xelatex-2col |
|---|---|---|---|---|---|---|---|---|---|---|---|
| c01-headings |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c02-paragraphs |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c03-emphasis |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c04-lists |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c05-code |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c06-tables |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | **x** |
| c07-links |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c08-quotes |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c09-footnotes |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c10-images |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c11-unicode |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c12-rtl |  | ok | ok | ok |  |  |  | ok | ok |  |  |
| c13-math |  | ok |  |  | ok |  | ok | ok |  | ok | ok |
| c14-misc |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| c15-alerts |  |  |  |  | ok | ok | ok |  |  |  |  |
| d01-tester |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | **x** |
| d02-business-report |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | **x** |
| d03-user-manual |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | **x** |
| d04-paper |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | **x** |
| d05-book-chapter |  | ok | ok | ok | ok | ok | ok | ok | ok | ok | ok |
| d06-slides | ok |  |  |  | ok |  |  |  |  |  |  |

| invalid item | reason |
|---|---|
| xelatex-2col/c06-tables | pandoc exit 43: ! Package longtable Error: longtable not in 1-column mode. |
| xelatex-2col/d01-tester | pandoc exit 43: ! Package longtable Error: longtable not in 1-column mode. |
| xelatex-2col/d02-business-report | pandoc exit 43: ! Package longtable Error: longtable not in 1-column mode. |
| xelatex-2col/d03-user-manual | pandoc exit 43: ! Package longtable Error: longtable not in 1-column mode. |
| xelatex-2col/d04-paper | pandoc exit 43: ! Package longtable Error: longtable not in 1-column mode. |

Producers with tagged output: chromium, libreoffice, libreoffice-tagged, rayomd-pdfua, typst.

## 3. Synthetic

NID/TEDS/MHS: opendataloader-bench evaluator means over documents where the metric applies. Construct F1: micro-averaged over documents. Development items exclude the weasyprint holdout. "overall" = macro-average over the 4 classes.

### 3.1 Development, untagged items

| tool | items | NID overall | TEDS overall | MHS overall | NID renderer | NID office | NID academic | NID book | construct F1 (all) |
|---|---|---|---|---|---|---|---|---|---|
| pdftotext | 71 | 0.883 | 0.000 | 0.000 | 0.876 | 0.886 | 0.873 | 0.899 | 0.192 |
| markitdown | 71 | 0.772 | 0.157 | 0.004 | 0.816 | 0.838 | 0.598 | 0.838 | 0.170 |
| pymupdf | 71 | 0.918 | 0.844 | 0.822 | 0.888 | 0.953 | 0.883 | 0.947 | 0.633 |
| opendataloader | 71 | 0.915 | 0.596 | 0.841 | 0.897 | 0.935 | 0.911 | 0.917 | 0.459 |
| liteparse | 71 | 0.921 | 0.720 | 0.850 | 0.882 | 0.953 | 0.898 | 0.952 | 0.495 |
| edgeparse | 71 | 0.877 | 0.697 | 0.660 | 0.843 | 0.923 | 0.816 | 0.927 | 0.424 |
| docling | 71 | 0.923 | 0.862 | 0.776 | 0.869 | 0.952 | 0.910 | 0.962 | 0.534 |

### 3.2 Development, tagged items

| tool | items | NID overall | TEDS overall | MHS overall | NID renderer | NID office | NID academic | NID book | construct F1 (all) |
|---|---|---|---|---|---|---|---|---|---|
| pdftotext | 92 | 0.916 | 0.000 | 0.006 | 0.876 | 0.885 | 0.940 | 0.965 | 0.188 |
| markitdown | 92 | 0.918 | 0.387 | 0.011 | 0.871 | 0.881 | 0.953 | 0.966 | 0.201 |
| pymupdf | 92 | 0.931 | 0.775 | 0.802 | 0.887 | 0.915 | 0.941 | 0.981 | 0.594 |
| opendataloader | 92 | 0.913 | 0.202 | 0.693 | 0.874 | 0.864 | 0.945 | 0.969 | 0.437 |
| liteparse | 92 | 0.923 | 0.507 | 0.857 | 0.882 | 0.906 | 0.949 | 0.957 | 0.497 |
| edgeparse | 92 | 0.895 | 0.523 | 0.682 | 0.849 | 0.833 | 0.940 | 0.956 | 0.437 |
| docling | 92 | 0.939 | 0.881 | 0.832 | 0.896 | 0.919 | 0.962 | 0.979 | 0.599 |

### Construct F1 per category, development untagged

| tool | heading | list_item | code_block | quote_para | alert | link | strong | emph | strike | code_inline | sup | sub | footnote |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pdftotext | 0.000 | 0.422 | 0.000 | 0.000 | 0.000 | 0.378 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| markitdown | 0.000 | 0.382 | 0.000 | 0.000 | 0.000 | 0.360 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| pymupdf | 0.929 | 0.819 | 0.371 | 0.000 | 0.000 | 0.385 | 0.291 | 0.686 | 0.424 | 0.752 | 0.048 | 0.000 | 0.000 |
| opendataloader | 0.923 | 0.531 | 0.000 | 0.000 | 0.000 | 0.378 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| liteparse | 0.923 | 0.662 | 0.557 | 0.000 | 0.000 | 0.340 | 0.030 | 0.014 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| edgeparse | 0.796 | 0.553 | 0.000 | 0.000 | 0.000 | 0.378 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| docling | 0.858 | 0.767 | 0.308 | 0.000 | 0.000 | 0.421 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |

### Attribute accuracy, math and image recall, development untagged

| tool | heading.level | list_item.depth | list_item.ordered | list_item.task | alert.type | link.url | code_block.exact | math recall | image recall |
|---|---|---|---|---|---|---|---|---|---|
| pdftotext | – | 0.926 | 1.000 | 1.000 | – | 0.905 | – | 0.568 | 0.000 |
| markitdown | – | 0.942 | 1.000 | 1.000 | – | 0.900 | – | 0.477 | 0.000 |
| pymupdf | 0.800 | 0.953 | 1.000 | 0.975 | – | 0.905 | 0.769 | 0.462 | 0.000 |
| opendataloader | 0.784 | 0.871 | 0.282 | 0.994 | – | 0.905 | – | 0.500 | 0.000 |
| liteparse | 0.805 | 0.944 | 1.000 | 0.985 | – | 0.923 | 0.444 | 0.417 | 0.000 |
| edgeparse | 0.121 | 0.851 | 1.000 | 1.000 | – | 0.905 | – | 0.212 | 0.000 |
| docling | 0.606 | 0.707 | 0.909 | 0.956 | – | 0.917 | 0.000 | 0.530 | 0.000 |

### Construct F1 per category, development tagged

| tool | heading | list_item | code_block | quote_para | alert | link | strong | emph | strike | code_inline | sup | sub | footnote |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| pdftotext | 0.000 | 0.422 | 0.000 | 0.000 | 0.000 | 0.365 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| markitdown | 0.000 | 0.448 | 0.100 | 0.000 | 0.000 | 0.365 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| pymupdf | 0.886 | 0.820 | 0.088 | 0.000 | 0.000 | 0.335 | 0.327 | 0.642 | 0.524 | 0.275 | 0.034 | 0.000 | 0.000 |
| opendataloader | 0.818 | 0.531 | 0.000 | 0.000 | 0.000 | 0.365 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| liteparse | 0.929 | 0.673 | 0.436 | 0.000 | 0.000 | 0.364 | 0.030 | 0.022 | 0.000 | 0.012 | 0.000 | 0.000 | 0.000 |
| edgeparse | 0.794 | 0.599 | 0.000 | 0.000 | 0.000 | 0.365 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| docling | 0.916 | 0.890 | 0.265 | 0.000 | 0.000 | 0.417 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |

### Attribute accuracy, math and image recall, development tagged

| tool | heading.level | list_item.depth | list_item.ordered | list_item.task | alert.type | link.url | code_block.exact | math recall | image recall |
|---|---|---|---|---|---|---|---|---|---|
| pdftotext | – | 0.933 | 1.000 | 1.000 | – | 1.000 | – | 0.568 | 0.000 |
| markitdown | – | 0.925 | 1.000 | 1.000 | – | 1.000 | 0.000 | 0.479 | 0.000 |
| pymupdf | 0.867 | 0.895 | 0.996 | 0.998 | – | 1.000 | 1.000 | 0.542 | 0.000 |
| opendataloader | 0.791 | 0.762 | 0.371 | 1.000 | – | 1.000 | – | 0.510 | 0.000 |
| liteparse | 0.972 | 0.909 | 1.000 | 1.000 | – | 1.000 | 0.410 | 0.380 | 0.000 |
| edgeparse | 0.097 | 0.887 | 0.994 | 1.000 | – | 1.000 | – | 0.391 | 0.000 |
| docling | 0.575 | 0.728 | 0.874 | 0.986 | – | 1.000 | 0.077 | 0.448 | 0.000 |

### Holdout (weasyprint), aggregate only

| tool | items | NID | TEDS | MHS | construct F1 (all) |
|---|---|---|---|---|---|
| pdftotext | 18 | 0.886 | 0.000 | 0.000 | 0.186 |
| markitdown | 18 | 0.863 | 0.304 | 0.016 | 0.124 |
| pymupdf | 18 | 0.909 | 0.774 | 0.747 | 0.556 |
| opendataloader | 18 | 0.873 | 0.000 | 0.730 | 0.212 |
| liteparse | 18 | 0.886 | 0.526 | 0.751 | 0.562 |
| edgeparse | 18 | 0.859 | 0.683 | 0.748 | 0.508 |
| docling | 18 | 0.910 | 0.877 | 0.802 | 0.513 |

### Per producer: NID / construct F1 (all)

| producer | tagged | pdftotext | markitdown | pymupdf | opendataloader | liteparse | edgeparse | docling |
|---|---|---|---|---|---|---|---|---|
| beamer | no | 0.82 / 0.12 | 0.75 / 0.12 | 0.87 / 0.79 | 0.77 / 0.61 | 0.90 / 0.63 | 0.74 / 0.40 | 0.91 / 0.97 |
| chromium | yes | 0.88 / 0.19 | 0.87 / 0.18 | 0.88 / 0.46 | 0.86 / 0.39 | 0.88 / 0.43 | 0.85 / 0.42 | 0.90 / 0.50 |
| libreoffice | yes | 0.89 / 0.19 | 0.89 / 0.19 | 0.92 / 0.63 | 0.87 / 0.48 | 0.91 / 0.49 | 0.84 / 0.39 | 0.92 / 0.63 |
| libreoffice-tagged | yes | 0.89 / 0.19 | 0.89 / 0.19 | 0.92 / 0.63 | 0.87 / 0.48 | 0.91 / 0.49 | 0.84 / 0.39 | 0.92 / 0.63 |
| rayomd | no | 0.90 / 0.18 | 0.90 / 0.20 | 0.91 / 0.63 | 0.93 / 0.45 | 0.90 / 0.45 | 0.87 / 0.41 | 0.90 / 0.42 |
| rayomd-pdfua | yes | 0.91 / 0.19 | 0.92 / 0.22 | 0.93 / 0.51 | 0.94 / 0.43 | 0.91 / 0.46 | 0.91 / 0.42 | 0.93 / 0.60 |
| rayomd-tech | no | 0.89 / 0.20 | 0.90 / 0.21 | 0.90 / 0.64 | 0.92 / 0.44 | 0.89 / 0.44 | 0.87 / 0.42 | 0.87 / 0.52 |
| typst | yes | 0.89 / 0.17 | 0.88 / 0.22 | 0.91 / 0.73 | 0.87 / 0.40 | 0.90 / 0.61 | 0.86 / 0.56 | 0.91 / 0.63 |
| weasyprint | no | 0.89 / 0.19 | 0.86 / 0.12 | 0.91 / 0.56 | 0.87 / 0.21 | 0.89 / 0.56 | 0.86 / 0.51 | 0.91 / 0.51 |
| xelatex | no | 0.87 / 0.19 | 0.69 / 0.10 | 0.91 / 0.63 | 0.89 / 0.48 | 0.90 / 0.59 | 0.85 / 0.46 | 0.90 / 0.63 |
| xelatex-2col | no | 0.84 / 0.20 | 0.53 / 0.15 | 0.86 / 0.60 | 0.88 / 0.44 | 0.89 / 0.47 | 0.80 / 0.37 | 0.91 / 0.52 |

## 4. opendataloader-bench (200 documents)

Local runs (reproduced):

| tool | overall | NID | NID-S | TEDS | TEDS-S | MHS | MHS-S | missing |
|---|---|---|---|---|---|---|---|---|
| pdftotext | 0.597 | 0.897 | 0.877 | 0.000 | 0.000 | 0.000 | 0.000 | 0 |
| markitdown | 0.589 | 0.844 | 0.838 | 0.273 | 0.328 | 0.000 | 0.000 | 0 |
| pymupdf | 0.866 | 0.906 | 0.889 | 0.773 | 0.811 | 0.782 | 0.889 | 0 |
| opendataloader | 0.838 | 0.914 | 0.923 | 0.427 | 0.447 | 0.760 | 0.837 | 0 |
| liteparse | 0.885 | 0.914 | 0.917 | 0.818 | 0.844 | 0.822 | 0.892 | 0 |
| edgeparse | 0.838 | 0.897 | 0.891 | 0.639 | 0.692 | 0.729 | 0.819 | 0 |
| docling | 0.884 | 0.902 | 0.858 | 0.879 | 0.896 | 0.827 | 0.903 | 0 |

Stored engine predictions shipped with the benchmark, rescored in our scorer image against the pinned ground truth (their conversions were not run here). A stored evaluation.json older than the benchmark's last ground-truth change differs from the rescore in the metrics that change touched.

| engine | stored overall | rescored overall | abs. diff | NID | TEDS | MHS | metrics differing by > 0.001 |
|---|---|---|---|---|---|---|---|
| docling | 0.882 | 0.882 | 0.000 | 0.898 | 0.887 | 0.824 | none |
| edgeparse | 0.837 | 0.837 | 0.000 | 0.894 | 0.717 | 0.706 | none |
| liteparse | 0.576 | 0.576 | 0.000 | 0.866 | 0.000 | 0.000 | none |
| marker | 0.861 | 0.867 | 0.006 | 0.890 | 0.808 | 0.819 | mhs |
| markitdown | 0.589 | 0.589 | 0.000 | 0.844 | 0.273 | 0.000 | none |
| mineru | 0.831 | 0.835 | 0.004 | 0.857 | 0.873 | 0.759 | mhs |
| nutrient | 0.885 | 0.885 | 0.000 | 0.925 | 0.708 | 0.819 | none |
| opendataloader | 0.831 | 0.831 | 0.000 | 0.902 | 0.489 | 0.739 | none |
| opendataloader-hybrid | 0.907 | 0.907 | 0.000 | 0.934 | 0.928 | 0.821 | none |
| opendataloader-hybrid-helium | 0.845 | 0.845 | 0.000 | 0.879 | 0.807 | 0.755 | none |
| opendataloader-hybrid-hydrogen | 0.877 | 0.877 | 0.000 | 0.926 | 0.796 | 0.769 | none |
| pymupdf4llm | 0.732 | 0.735 | 0.003 | 0.885 | 0.401 | 0.424 | mhs |
| unstructured | 0.686 | 0.686 | 0.000 | 0.882 | 0.000 | 0.388 | none |
| unstructured-hires | 0.841 | 0.841 | 0.000 | 0.904 | 0.588 | 0.749 | none |

## 5. olmOCR-bench (born-digital categories)

Pass rate per JSONL (tests passed / tests); last column = mean of the 4 born-digital categories excluding arxiv_math. Scanned categories (`old_scans*`) are left to the M8 OCR gate.

| tool | arxiv_math | headers_footers | long_tiny_text | multi_column | table_tests | born-digital mean (excl. arxiv_math) |
|---|---|---|---|---|---|---|
| pdftotext | 0.000 | 0.282 | 0.876 | 0.653 | 0.000 | 0.452 |
| markitdown | 0.000 | 0.384 | 0.312 | 0.393 | 0.249 | 0.334 |
| pymupdf | 0.000 | 0.381 | 0.337 | 0.667 | 0.615 | 0.500 |
| opendataloader | 0.000 | 0.361 | 0.348 | 0.638 | 0.247 | 0.399 |
| liteparse | 0.538 | 0.554 | 0.240 | 0.662 | 0.559 | 0.504 |
| edgeparse | 0.000 | 0.490 | 0.251 | 0.462 | 0.401 | 0.401 |
| docling | 0.000 | 0.898 | 0.523 | 0.695 | 0.610 | 0.681 |

## 6. READoc-300 (holdout, aggregate only)

### arxiv

| metric | pdftotext | markitdown | pymupdf | opendataloader | liteparse | edgeparse | docling |
|---|---|---|---|---|---|---|---|
| text_eds | 0.768 | 0.584 | 0.754 | 0.806 | 0.826 | 0.725 | 0.841 |
| text_f1 | 0.836 | 0.633 | 0.834 | 0.820 | 0.814 | 0.816 | 0.847 |
| head_eds | 0.045 | 0.021 | 0.610 | 0.354 | 0.391 | 0.426 | 0.685 |
| head_teds | 0.006 | -0.001 | 0.515 | 0.308 | 0.254 | 0.229 | 0.403 |
| inline_eds | 0.001 | 0.001 | 0.001 | 0.001 | 0.369 | 0.001 | 0.001 |
| outline_eds | 0.000 | 0.000 | 0.000 | 0.000 | 0.002 | 0.000 | 0.000 |
| table_eds | 0.000 | 0.130 | 0.690 | 0.196 | 0.504 | 0.371 | 0.668 |
| table_teds | 0.000 | 0.036 | 0.685 | 0.138 | 0.407 | 0.286 | 0.656 |
| seg_kt | 0.888 | 0.508 | 0.976 | 0.969 | 0.969 | 0.948 | 0.984 |
| word_kt | 0.978 | 0.914 | 0.978 | 0.979 | 0.976 | 0.960 | 0.979 |
| seg_sp | 0.879 | 0.491 | 0.972 | 0.960 | 0.959 | 0.941 | 0.982 |
| word_sp | 0.969 | 0.899 | 0.970 | 0.970 | 0.968 | 0.953 | 0.970 |

### github

| metric | pdftotext | markitdown | pymupdf | opendataloader | liteparse | edgeparse | docling |
|---|---|---|---|---|---|---|---|
| text_eds | 0.874 | 0.757 | 0.813 | 0.803 | 0.761 | 0.781 | 0.908 |
| text_f1 | 0.861 | 0.578 | 0.795 | 0.806 | 0.845 | 0.801 | 0.891 |
| head_eds | 0.037 | 0.018 | 0.622 | 0.725 | 0.763 | 0.454 | 0.744 |
| head_teds | 0.021 | 0.009 | 0.586 | 0.763 | 0.778 | 0.384 | 0.471 |
| seg_kt | 0.956 | 0.756 | 0.980 | 0.990 | 0.989 | 0.986 | 0.993 |
| word_kt | 0.995 | 0.927 | 0.981 | 0.985 | 0.992 | 0.982 | 0.992 |
| seg_sp | 0.948 | 0.745 | 0.964 | 0.989 | 0.985 | 0.984 | 0.992 |
| word_sp | 0.992 | 0.888 | 0.977 | 0.980 | 0.990 | 0.978 | 0.990 |

## 7. Speed and memory

Single pinned core (`--cpuset-cpus=3 --cpus=1`, thread env vars = 1). 200 odl PDFs + 6 multi-page synthetic PDFs; warm = median of 3 repeats after 1 warm-up; pages/s = pages of successful documents / sum of their medians; cold start = median wall time of 5 fresh adapter processes on one 1-page PDF; peak RSS = max RSS of the whole warm run.

| tool | timing | pages/s | ms/page median | ms/page p95 | cold start s | peak RSS MiB | ok docs |
|---|---|---|---|---|---|---|---|
| pdftotext | fresh-process | 64.325 | 17.147 | 19.829 | 0.090 | 19.051 | 206/206 |
| markitdown | in-process | 2.179 | 452.761 | 691.913 | 1.510 | 247.078 | 206/206 |
| pymupdf | in-process | 0.504 | 1898.597 | 2901.896 | 4.250 | 768.961 | 206/206 |
| opendataloader | batch | 22.121 | – | – | 0.930 | 217.727 | 206/206 |
| liteparse | fresh-process | 49.004 | 22.525 | 28.855 | 0.080 | 40.363 | 206/206 |
| edgeparse | in-process | 139.695 | 4.884 | 15.347 | 0.210 | 172.176 | 206/206 |
| docling | in-process | 0.301 | 2003.177 | 10009.736 | 11.150 | 2109.145 | 206/206 |

## 8. Failures

| tool | suite | docs | error | timeout | empty |
|---|---|---|---|---|---|
| pdftotext | synthetic | 181 | 0 | 0 | 0 |
| pdftotext | odl | 200 | 0 | 0 | 0 |
| pdftotext | olmocr | 1269 | 0 | 0 | 60 |
| pdftotext | readoc | 300 | 0 | 0 | 0 |
| markitdown | synthetic | 181 | 0 | 0 | 0 |
| markitdown | odl | 200 | 0 | 0 | 0 |
| markitdown | olmocr | 1269 | 7 | 0 | 59 |
| markitdown | readoc | 300 | 0 | 0 | 0 |
| pymupdf | synthetic | 181 | 0 | 0 | 0 |
| pymupdf | odl | 200 | 0 | 0 | 0 |
| pymupdf | olmocr | 1269 | 0 | 0 | 61 |
| pymupdf | readoc | 300 | 0 | 0 | 0 |
| opendataloader | synthetic | 181 | 0 | 0 | 0 |
| opendataloader | odl | 200 | 0 | 0 | 0 |
| opendataloader | olmocr | 1269 | 1 | 0 | 60 |
| opendataloader | readoc | 300 | 0 | 0 | 0 |
| liteparse | synthetic | 181 | 0 | 0 | 0 |
| liteparse | odl | 200 | 0 | 0 | 0 |
| liteparse | olmocr | 1269 | 0 | 0 | 3 |
| liteparse | readoc | 300 | 0 | 0 | 0 |
| edgeparse | synthetic | 181 | 0 | 0 | 0 |
| edgeparse | odl | 200 | 0 | 0 | 0 |
| edgeparse | olmocr | 1269 | 0 | 0 | 64 |
| edgeparse | readoc | 300 | 2 | 0 | 0 |
| docling | synthetic | 181 | 0 | 0 | 0 |
| docling | odl | 200 | 0 | 0 | 0 |
| docling | olmocr | 1269 | 0 | 0 | 39 |
| docling | readoc | 300 | 0 | 0 | 0 |


## 9. Text-layer census (SafeDocs CC-MAIN-2021-31 0000.zip)

- files: 1000 total, 996 opened by pdfinfo; pages examined: 8905 (≤ 50 per file)
- tool failures: {"zip": 0, "pdfinfo": 4, "pdffonts": 0, "pdftotext": 0, "pdfimages": 0}
- tagged 34.2 %, encrypted 1.9 %, Type 3 font 1.1 %, non-Unicode font 65.2 % of processed files
- **R_upper = 12.36 %** of examined pages are image_only or broken (upper bound of the OCR need before RayoPDF's own Unicode repair)

| scope | docs | pages | image_only % pages | empty_or_vector % pages | broken % pages | text_over_image % pages | text % pages |
|---|---|---|---|---|---|---|---|
| all | 996 | 8905 | 8.916 | 2.650 | 3.447 | 6.109 | 78.877 |
| adobe | 295 | 3449 | 8.089 | 2.349 | 6.379 | 5.074 | 78.110 |
| ms-office | 278 | 2017 | 2.826 | 3.173 | 1.041 | 3.917 | 89.043 |
| other | 210 | 1444 | 10.734 | 2.839 | 0.277 | 9.141 | 77.008 |
| apple | 50 | 483 | 6.418 | 4.348 | 11.594 | 2.692 | 74.948 |
| ghostscript | 34 | 221 | 4.977 | 4.977 | 0.453 | 1.810 | 87.783 |
| unknown | 32 | 239 | 36.402 | 2.092 | 0.000 | 0.837 | 60.669 |
| itext | 28 | 272 | 2.206 | 1.838 | 0.000 | 4.044 | 91.912 |
| scanner-ocr | 28 | 259 | 55.985 | 0.000 | 0.386 | 43.629 | 0.000 |
| libreoffice | 17 | 213 | 10.798 | 0.000 | 0.469 | 7.042 | 81.690 |
| chromium | 12 | 69 | 0.000 | 2.899 | 0.000 | 0.000 | 97.101 |
| tex | 12 | 239 | 0.000 | 2.510 | 1.255 | 0.000 | 96.234 |

| class | % of documents with ≥ 1 such page |
|---|---|
| image_only | 13.956 |
| empty_or_vector | 11.044 |
| broken | 4.117 |
| text_over_image | 9.639 |
| text | 81.426 |

| script | % of image_only + broken pages |
|---|---|
| unknown | 52.316 |
| latin | 43.233 |
| other | 3.724 |
| cjk | 0.636 |
| cyrillic | 0.091 |

