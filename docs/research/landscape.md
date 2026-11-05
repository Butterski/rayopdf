# PDF→Markdown landscape (research for rayopdf, 2026-11-05)

Research note, 2026-11-05. Third-party numbers are as published unless marked reproduced; reproduced numbers are in `docs/benchmarks/`.

Conventions: **claim** = number published by the tool's vendor/author, not reproduced here. "odl-bench" = opendataloader-bench. Scores are as published; harnesses differ (see §2.9).

## 0. Executive summary

- Fast rule-based (text-layer) tools now come close to ML pipelines on **reading order** of born-digital PDFs. On odl-bench, opendataloader local scores NID 0.902 vs docling 0.898 ([odl-bench](https://github.com/opendataloader-project/opendataloader-bench)). They lose badly on **tables** (TEDS 0.40–0.49 vs 0.87–0.93), **headings** (MHS 0.41 for pymupdf4llm vs 0.82 for docling) and **math** (0% of olmOCR-bench math tests for every text-layer tool) ([LiteParse README](https://github.com/run-llama/liteparse), [marker README](https://raw.githubusercontent.com/datalab-to/marker/master/README.md)).
- Newer native rule-based engines claim to have closed most of the table/heading gap on odl-bench. LiteParse v2 (Rust/PDFium, no model) claims 0.886 overall (TEDS 0.818, MHS 0.821) ([LiteParse](https://github.com/run-llama/liteparse)). EdgeParse claims 0.837–0.857 at ~0.02–0.036 s/page ([EdgeParse](https://github.com/raphaelmansuy/edgeparse), [odl-bench](https://github.com/opendataloader-project/opendataloader-bench)). These are vendor-run and harness-dependent.
- Speed: model-free tools run at roughly 1–40 ms/page. ML pipelines on CPU run at roughly 0.5–54 s/page (docling 3.1 s/page on an 8-thread x86 CPU, [Docling paper](https://arxiv.org/html/2501.17887); marker 53.9 s/page in odl-bench).
- No single benchmark is sufficient. Each one normalizes Markdown differently, covers different producers, and some are image-only. An evaluation of rayopdf needs several benchmarks plus a multi-producer synthetic corpus with exact ground truth.

## 1. Tools

### 1.1 Markdown emitters that are rule-based or text-layer first (rayopdf's direct competitors)

| Tool | License / lang | Approach | Markdown structure emitted | Published speed | Published quality |
|---|---|---|---|---|---|
| **pymupdf4llm** (now inside PyMuPDF 2.x; the `pymupdf4llm` package is an empty shim, [PyPI](https://pypi.org/project/pymupdf4llm)) | AGPL-3.0 ([repo](https://github.com/pymupdf/pymupdf4llm)); Python over MuPDF (C) | Heuristic: headers from font size (`IdentifyHeaders`) or the PDF outline (`TocHeaders`) ([API](https://pymupdf.readthedocs.io/en/latest/pymupdf4llm/api.html)). Optional **Layout** mode runs a small CPU **GNN** on MuPDF features ([Artifex blog](https://artifex.com/blog/pymupdf-layout-10-faster-pdf-parsing-without-gpus)), so it is hybrid. The repo ships OCR adapters (rapidocr/tesseract/paddle) and an ONNX "OCR decision" model ([repo tree](https://github.com/pymupdf/pymupdf4llm)) | Headings (#…), tables, images. With page_chunks the box classes include title, section-header, page-header/footer, list-item, footnote, formula ([API](https://pymupdf.readthedocs.io/en/latest/pymupdf4llm/api.html)) | odl-bench 0.091 s/page (single-threaded CPU) ([odl speed](https://opendataloader.org/docs/benchmark/speed)); 55.5 ms mean/doc (claim by [pdf_oxide](https://github.com/yfedoseev/pdf_oxide)); 0.640 s/doc in the EdgeParse harness ([EdgeParse](https://github.com/raphaelmansuy/edgeparse)) | odl-bench overall 0.732 (NID .885, TEDS .401, MHS .412). READoc-arXiv avg 40.55 (heading tree 20.77, table tree 15.83, formulas ≈0). READoc-GitHub 63.78 ([READoc](https://arxiv.org/html/2409.05137v3)) |
| **opendataloader-pdf** (local/"fast" mode) | Apache-2.0; Java core, Python/Node wrappers ([PyPI](https://pypi.org/project/opendataloader-pdf/2.5.17)) | Deterministic; XY-Cut++ reading order; uses the tagged-PDF struct tree (`--use-struct-tree`); header/footer/watermark filter; "hybrid" mode routes complex pages to a docling-fast server | Headings, lists (numbered/bulleted/nested), simple bordered tables, images; JSON with bboxes. LaTeX formulas and OCR only in hybrid ([PyPI capability matrix](https://pypi.org/project/opendataloader-pdf/2.5.17)) | 0.015 s/page, Apple M4, v2.2.1, 200 docs in 3.0 s ([evaluation.json](https://raw.githubusercontent.com/opendataloader-project/opendataloader-bench/main/prediction/opendataloader/evaluation.json)). Docs say single-threaded CPU ([speed](https://opendataloader.org/docs/benchmark/speed)) | odl-bench 0.831 (NID .902, TEDS .489 on only 42 docs, MHS .739 on 107 docs). Hybrid 0.907 at 0.463 s/page. olmOCR-bench 32.5 (as run by LiteParse authors, [LiteParse](https://github.com/run-llama/liteparse)) |
| **EdgeParse** | Apache-2.0 badge (GitHub API reports "Other"); Rust, single ~15 MB binary ([repo](https://github.com/raphaelmansuy/edgeparse)) | XY-Cut++; ruling-line plus "cluster" borderless tables; heading levels from font metrics, outline and section numbers; filters hidden or off-page text | Headings, tables, paragraphs, images; JSON bboxes | ~0.02 s/doc on Apple M4 Max (claim); 0.036 s/page in odl-bench | Claim: official odl-bench 0.857 deterministic / 0.900 hybrid; own harness 0.782. odl-bench table lists 0.837 (TEDS .717, MHS .706) |
| **LiteParse v2** (LlamaIndex) | Apache-2.0; Rust over PDFium (C++) ([repo](https://github.com/run-llama/liteparse)) | No ML; "grid projection" layout; optional Tesseract or HTTP OCR | Headings, tables, lists, images, links | "~2–5 ms per page" text parsing (claim). Marker's harness measured an older liteparse at 1721 pg/s with no OCR ([marker](https://raw.githubusercontent.com/datalab-to/marker/master/README.md)) | Claim (Apple M2 Max, v2.14.4): odl-bench 0.886 (NID .917, TEDS .818, MHS .821); olmOCR-bench 39.6; ParseBench 0.364. Marker's harness got 22.4 / 20.4 olmOCR for an older version, so results diverge across versions and harnesses |
| **pdf-inspector** (Firecrawl) | MIT; Rust ([repo](https://github.com/firecrawl/pdf-inspector)) | Classifies text-based / scanned / mixed, then text extraction to Markdown | Markdown with tables | n/a | olmOCR-bench 33.7; refuses 192/1,403 scanned pages. ParseBench 0.283 (as measured by [LiteParse](https://github.com/run-llama/liteparse)) |
| **pdf_oxide** | MIT/Apache-2.0; Rust core with many bindings ([repo](https://github.com/yfedoseev/pdf_oxide)) | Text extraction plus `page.markdown(detect_headings=True)` | Headings (opt-in); tables API | Claim: 0.8 ms mean / 9 ms p99 per doc, 100% pass on 3,830 PDFs (veraPDF, pdf.js and SafeDocs suites), single thread, text only | No published Markdown-quality numbers; claims "99.5% text parity vs PyMuPDF/pypdfium2" |
| **MarkItDown** | MIT; Python; PDF via pdfminer.six + pdfplumber ([pyproject](https://raw.githubusercontent.com/microsoft/markitdown/main/packages/markitdown/pyproject.toml)) | Rule-based; optional Azure Document Intelligence or LLM OCR plugin ([README](https://raw.githubusercontent.com/microsoft/markitdown/main/README.md)) | Mostly plain text; no headings (MHS 0.000) | odl-bench 0.114 s/page; 108.8 ms/doc (pdf_oxide claim) | odl-bench 0.589; READoc-arXiv 35.21 (heading concat 2.90); olmOCR 28.7 (LiteParse-run) |
| **unstructured** (`fast`) | Apache-2.0 ([odl-bench](https://github.com/opendataloader-project/opendataloader-bench)); Python | `fast` = "rule-based", `hi_res` = model-based layout, `ocr_only` ([docs](https://docs.unstructured.io/open-source/concepts/partitioning-strategies)) | Elements, not native Markdown | odl-bench: fast 0.077 s/page, hi_res 3.008 s/page | odl-bench: fast 0.686 (TEDS 0.000); hi_res 0.841. DP-Bench (Upstage-run): NID 91.78, TEDS 80.26 ([DP-Bench](https://huggingface.co/datasets/upstage/dp-bench)) |
| **Nutrient** pdf-to-markdown | Commercial | Not disclosed | n/a | 0.008 s/page (fastest in odl-bench) | odl-bench 0.885 (TEDS .708) ([odl-bench](https://github.com/opendataloader-project/opendataloader-bench)) |
| **Xberg** (formerly **Kreuzberg**; repo redirects) | MIT; Rust core with 15 bindings ([repo](https://github.com/kreuzberg-dev/kreuzberg)) | Text layer by default; optional ML layout (PP-DocLayout-V3, RT-DETR) and table models (TATR, SLANet); Tesseract/Paddle OCR | Markdown, Djot, HTML, DocTags | Claim: native PDF p50 32 ms/doc, 64 ms cold start, 239 MB peak; "Xberg Layout" 2.3 s ([benchmarks](https://xberg.io/benchmarks)) | Claim: TF1 0.982 on its own 60-doc / 24-type corpus |

### 1.2 Low-level extractors (positioned text, no Markdown semantics)

| Tool | License / lang | What it gives you | Speed (published) |
|---|---|---|---|
| **pdfminer.six** | MIT ([pdfplumber README](https://raw.githubusercontent.com/jsvine/pdfplumber/stable/README.md)); pure Python | Text/HTML/hOCR/XML, automatic layout analysis (LAParams), CJK/vertical text, tagged contents ([README](https://raw.githubusercontent.com/pdfminer/pdfminer.six/master/README.md)) | 16.8 ms/doc mean (pdf_oxide claim) |
| **pdfplumber** | MIT; Python on pdfminer.six | Chars/lines/rects/curves; table finder (lines/text/explicit strategies, after Nurminen's thesis / Tabula); visual debugging; no OCR ([README](https://raw.githubusercontent.com/jsvine/pdfplumber/stable/README.md)) | 23.2 ms/doc (pdf_oxide claim) |
| **pypdfium2 / PDFium** | pypdfium2 Apache-2.0 / BSD-3; PDFium "BSD-style" ([README](https://raw.githubusercontent.com/pypdfium2-team/pypdfium2/main/README.md)) | Text with positions, rendering | 4.1 ms/doc (pdf_oxide claim). Docling authors report "merged text cells across far-apart tokens or table columns" with pypdfium ([Docling paper](https://arxiv.org/html/2501.17887)) |
| **docling-parse** | MIT (Docling); C++ on qpdf | Docling's default PDF backend | 81 ms/page x86 CPU, 44 ms M3 Max ([Docling paper](https://arxiv.org/html/2501.17887)) |
| **pdftext** (marker's backend) | GPL-3.0 (per [pdf_oxide table](https://github.com/yfedoseev/pdf_oxide)) | Text in reading order | 7.3 ms/doc (claim) |
| **Poppler pdftotext** | GPL family (rayomd decision doc) | `-layout`, `-raw`, `-bbox`, `-bbox-layout` (block/line/word XHTML), `-tsv` ([man](https://manpages.debian.org/testing/poppler-utils/pdftotext.1.en.html)) | not published |
| **mutool draw** (MuPDF) | AGPL or commercial (rayomd decision doc) | Text outputs: plain, html, structured text as xml/json ("stext") ([docs](https://mupdf.readthedocs.io/en/latest/tools/mutool-draw.html)) | not published |
| **Apache PDFBox PDFTextStripper** | Apache-2.0 (rayomd decision doc); Java | `setSortByPosition`, bead/article separation (default true), paragraph heuristics (indent threshold 2.0, drop threshold 2.5), list-item regex patterns ([javadoc](https://pdfbox.apache.org/docs/2.0.13/javadocs/org/apache/pdfbox/text/PDFTextStripper.html)) | not published |
| **pdf.js text layer** | liberal license (per [pypdfium2 README](https://raw.githubusercontent.com/pypdfium2-team/pypdfium2/main/README.md)); JS | `getTextContent()` → TextItems + styles + `/Lang`; `includeMarkedContent` exposes marked-content (tag) boundaries ([api.js](https://raw.githubusercontent.com/mozilla/pdf.js/master/src/display/api.js)) | not published |
| **Extractous** | Apache-2.0; Rust + Apache Tika compiled with GraalVM native-image ([repo](https://github.com/yobix-ai/extractous)) | Text/XML + metadata, Tesseract OCR; **no Markdown** | Claim "25× faster than unstructured" |
| **GROBID** | Apache-2.0; Java ([README](https://raw.githubusercontent.com/kermitt2/grobid/master/Readme.md)) | Scholarly PDFs → **TEI XML** (68 labels: sections, paragraphs, refs, footnote callouts, figures/tables); CRF by default, optional DL | Claim: 10.6 PDF/s (~20M pages/day) on one 16-CPU machine; reference parsing F1 ≈ .87–.90 |

### 1.3 ML pipelines and VLMs (quality ceiling, not speed competitors)

| Tool | License | Approach | Markdown | Speed | Quality |
|---|---|---|---|---|---|
| **Docling** | MIT ([paper](https://arxiv.org/html/2501.17887)) | docling-parse text layer + layout detector + TableFormer + optional OCR | Headings, tables, lists | x86 CPU (8 threads): 3.1 s/page mean, median 0.79; L4 GPU 0.49 s/page; disabling OCR saves 60% ([paper](https://arxiv.org/html/2501.17887)); odl-bench 0.762 s/page | odl-bench 0.882 (TEDS .887, MHS .824); olmOCR 50.3 (marker-run); READoc-arXiv 58.83, -GitHub 49.87 |
| **marker** | Code Apache-2.0, weights modified OpenRAIL-M ([README](https://raw.githubusercontent.com/datalab-to/marker/master/README.md)). odl-bench still lists GPL-3.0, apparently outdated | pdftext + layout model (rf-detr in fast mode) + Surya VLM for OCR, math and low-confidence tables; `--disable_ocr` is pure CPU | Tables (HTML), `$$` LaTeX, inline math, code fences, links, images, header/footer removal | One B200 host: balanced 2.9 pg/s, fast 7.4, no-OCR 23.7 (42 ms/page) (claim); odl-bench 53.9 s/page | olmOCR-bench: balanced 76.0 / fast 66.6 / no-OCR 43.6 (born-digital subset: 83.5 / 71.6 / 55.8); OmniDocBench v1.6 78.44 |
| **MinerU** | "MinerU Open Source License", Apache-2.0 plus extra conditions ([README](https://github.com/opendatalab/MinerU)); odl-bench lists AGPL-3.0 (older) | Layout (DocLayout-YOLO) + recognisers; 4.x tiers flash/basic/standard/advanced | Headings, tables, formulas (LaTeX) | Docling paper: 3.3 s/page x86 CPU, 0.21 s/page L4; odl-bench 5.96 s/page | OmniDocBench v1.6 pipeline 86.47 (text edit .055, CDM 83.07, TEDS 81.88) ([README](https://raw.githubusercontent.com/opendatalab/OmniDocBench/main/README.md)); READoc-arXiv 73.07 |
| **Nougat** | Code MIT, weights **CC-BY-NC** ([README](https://raw.githubusercontent.com/facebookresearch/nougat/main/README.md)) | arXiv-trained image→Markdown transformer | Markdown + LaTeX | READoc: 51–101 s/doc on Titan RTX | READoc-arXiv 81.42 but Zenodo 49.94 (poor transfer) ([READoc](https://arxiv.org/html/2409.05137v3)) |
| **olmOCR** | Apache-2.0 ([README](https://raw.githubusercontent.com/allenai/olmocr/main/README.md)) | 7B VLM, RLVR on unit-test rewards | Plain text / Markdown, LaTeX math, tables | Claim < $200 per million pages; needs GPU | olmOCR-bench v0.4.0 82.4; OmniDocBench v1.6 85.74 |
| PyMuPDF-Layout | AGPL ([repo](https://github.com/ArtifexSoftware/pymupdf_layout)) | GNN over MuPDF heuristic features, CPU only | via pymupdf4llm | Claim "10× faster than competing solutions" | none published |

## 2. Benchmarks and metrics

### 2.1 Overview

| Benchmark | Measures | Size | Source docs | Born-digital? | GT license |
|---|---|---|---|---|---|
| **opendataloader-bench** ([repo](https://github.com/opendataloader-project/opendataloader-bench)) | NID / NID-S (reading order), TEDS / TEDS-S (tables, APTED), MHS / MHS-S (heading tree, APTED); overall = mean | 200 PDFs + GT Markdown | DP-Bench corpus (per [PSPDFKit](https://github.com/PSPDFKit/pdf-to-markdown/blob/main/docs/benchmarks.md)) | Native text, with "a handful of scanned pages" ([LiteParse](https://github.com/run-llama/liteparse)) | Repo Apache-2.0; corpus from DP-Bench (MIT) |
| **DP-Bench** (Upstage) ([HF](https://huggingface.co/datasets/upstage/dp-bench)) | NID (tables/figures excluded), TEDS, TEDS-S, latency | 200 pages: 90 Library of Congress, 90 OER, 20 Upstage; 55 tables; only Heading1 | Mixed | Mixed | MIT |
| **olmOCR-bench** ([README](https://raw.githubusercontent.com/allenai/olmocr/main/olmocr/bench/README.md), [HF](https://huggingface.co/datasets/allenai/olmOCR-bench)) | 7,010 pass/fail unit tests: text present/absent, natural reading order, table neighbour relations, math rendered with KaTeX and checked by relative symbol position | 1,403 single-page PDFs (arXiv math 522, old scans math 36, tables 188, old scans 98, headers/footers 266, multi-column 231, long tiny text 62) | arXiv, Internet Archive, LoC, AI2 crawl | Mostly; old_scans + old_scans_math (134 PDFs) are scans | ODC-BY-1.0 |
| **OmniDocBench** v1.6 ([README](https://raw.githubusercontent.com/opendatalab/OmniDocBench/main/README.md), [HF](https://huggingface.co/datasets/opendatalab/OmniDocBench)) | Text normalized edit distance, formula CDM, table TEDS / TEDS-S, reading-order edit; Overall = ((1 − TextEdit)·100 + TEDS + CDM)/3 | 1,651 pages, 10 doc types, 5 layouts, 5 languages | Papers, reports, newspapers, textbooks, notes, … | **Images** (`image_to_pdf.py` produces image-only PDFs); "original PDF slices" were added on 2024-12-25 | "Research purposes only, not for commercial use" |
| **READoc** ([paper](https://arxiv.org/html/2409.05137v3), [code](https://github.com/icip-cas/READoc)) | Whole-document PDF→Markdown: text EDS + vocab F1, heading concat EDS + ToC **tree TEDS**, formula EDS (inline/display), table EDS + TEDS, reading order Kendall-tau (block/token) | 3,576 docs (arXiv 1,009 / GitHub 1,224 / Zenodo 1,343); multi-page (avg 6.5–14.9 pages) | **Synthetic GT**: LaTeXML→HTML→MD; README→pandoc+Eisvogel→PDF; DOCX/HTML→PDF via python-docx/Chromium | Yes (born-digital) | HF `lazyc/READoc` lists MIT ([HF](https://huggingface.co/datasets/lazyc/READoc)) |
| **ParseBench** ([README](https://raw.githubusercontent.com/run-llama/ParseBench/main/README.md)) | Tables (GriTS + TableRecordMatch), charts, content faithfulness, semantic formatting (strike/sup/sub/bold/title hierarchy), visual grounding; deterministic rules, 169k rules | 2,078 pages / 1,211 docs, enterprise (insurance/finance/gov) | Real | Mixed | Apache-2.0 |
| **marker's benchmark** ([README](https://raw.githubusercontent.com/datalab-to/marker/master/README.md)) | Now olmOCR-bench (macro-average of 8 categories) + sustained pages/s; "digital-only" = 6 non-scanned categories | uses olmOCR-bench | — | — | — |
| **docling-eval** ([repo](https://github.com/docling-project/docling-eval)) | Harness: DP-Bench, OmniDocBench, DocLayNet, FinTabNet, PubTabNet, Pub1M; layout mAP, TEDS, reading-order ARD, markdown BLEU / edit / F1 / METEOR ([DP-Bench doc](https://raw.githubusercontent.com/docling-project/docling-eval/main/docs/DP-Bench_benchmarks.md)) | — | — | — | MIT |
| **DocLayNet** ([repo](https://github.com/DS4SD/DocLayNet)) | Layout boxes, 11 classes, COCO format; human-annotated, partly double/triple annotated | 80,863 pages (28 GiB core); "extra" 7.5 GiB = **single-page PDFs + JSON digital text cells** | Finance, science, patents, tenders, law, manuals | PDFs available | CDLA-Permissive-1.0 ([LICENSE](https://raw.githubusercontent.com/DS4SD/DocLayNet/main/LICENSE)) |
| **PubLayNet** ([repo](https://github.com/ibm-aur-nlp/PubLayNet)) | Layout (auto-labelled by PDF↔XML matching) | >360k images ([arXiv](https://arxiv.org/abs/1908.07836)); page PDFs released 2020 | PMC OA | Yes | Annotations CDLA-P-1.0; images under PMC OA terms ([LICENSE](https://raw.githubusercontent.com/ibm-aur-nlp/PubLayNet/master/LICENSE.md)) |
| **PubTabNet** ([repo](https://github.com/ibm-aur-nlp/PubTabNet)) | Table image → HTML; **TEDS reference code** | 568k+ table images; test GT withheld (ICDAR 2021 set public) | PMC OA | Images | Annotations CDLA-P-1.0 ([LICENSE](https://raw.githubusercontent.com/ibm-aur-nlp/PubTabNet/master/LICENSE.md)) |
| **FinTabNet** ([GTE paper](https://arxiv.org/abs/2005.00589)) | Table structure in financial reports | size not verified in this pass; FinTabNet.c is a cleaned fork | Annual reports | PDFs | Original CDLA-Permissive; FinTabNet.c CDLA-Permissive-2.0 ([HF](https://huggingface.co/datasets/bsmock/FinTabNet.c)) |

### 2.2 Metric reference implementations

- **TEDS**: [PubTabNet `src/metric.py`](https://raw.githubusercontent.com/ibm-aur-nlp/PubTabNet/master/src/metric.py) (lxml HTML → APTED tree edit; `structure_only` flag = TEDS-S). ParseBench ships a numba Zhang-Shasha version it states is score-identical ([README](https://raw.githubusercontent.com/run-llama/ParseBench/main/README.md)). docling-eval has `evaluators/table/teds.py`.
- **NID**: 1 − indel distance / (len(gt) + len(pred)), with substitutions excluded ([DP-Bench](https://huggingface.co/datasets/upstage/dp-bench) `evaluate.py`; odl-bench `src/evaluator.py`).
- **MHS**: headings flattened and compared with APTED ([odl-bench §3.3](https://github.com/opendataloader-project/opendataloader-bench)).
- **CDM** (formulas): renders both LaTeX strings and matches characters spatially ([arXiv 2409.03643](https://arxiv.org/abs/2409.03643), code in UniMERNet/cdm). OmniDocBench v1.6 re-implemented the Node/KaTeX dependencies in Python (~3× faster) and added MGAM matching.
- **olmOCR checks**: `python -m olmocr.bench.benchmark`. Math tests need Playwright Chromium. Markdown syntax is ignored, Unicode is NFC-normalized, dashes and quotes are folded ([README](https://raw.githubusercontent.com/allenai/olmocr/main/olmocr/bench/README.md)).
- **READoc S³uite**: standardization (heading styles, formula delimiters, MD tables → LaTeX, strips images and links) → regex segmentation → EDS / TEDS / KTDS ([paper App. C](https://arxiv.org/html/2409.05137v3)).

### 2.3 How to run locally (no host pollution; all inside docker)

- odl-bench: `git clone` + `git lfs pull`, then `uv sync --extra <engine>` and `uv run src/run.py --engine X`. AGPL/GPL/commercial engine runners were **removed** from the repo for licensing reasons; only their stored predictions remain ([README](https://github.com/opendataloader-project/opendataloader-bench)).
- olmOCR-bench: `pip install -e .[bench]`, `playwright install chromium`, `huggingface-cli download allenai/olmOCR-bench`, write per-page `.md` files into `bench_data/<tool>/`, then `python -m olmocr.bench.benchmark --dir …`.
- OmniDocBench: official docker for evaluation and CDM (added 2025-11-04); configs `end2end.yaml` / md2md.
- DP-Bench: `python evaluate.py --ref-path dataset/reference.json --pred-path … --mode all|layout|table|speed`.
- ParseBench: `parse-bench run <pipeline> [--group table]`; custom tools are added as providers ([docs/extending.md](https://raw.githubusercontent.com/run-llama/ParseBench/main/README.md)).

### 2.4 Leaderboards: rule-based vs ML (as published)

**odl-bench** ([repo](https://github.com/opendataloader-project/opendataloader-bench); speed single-threaded CPU; opendataloader row measured on Apple M4):

| Engine | Kind | Overall | NID | TEDS | MHS | s/page |
|---|---|---|---|---|---|---|
| opendataloader hybrid | rules + docling | 0.907 | 0.934 | 0.928 | 0.821 | 0.463 |
| nutrient | commercial | 0.885 | 0.925 | 0.708 | 0.819 | 0.008 |
| docling | ML | 0.882 | 0.898 | 0.887 | 0.824 | 0.762 |
| marker | ML | 0.861 | 0.890 | 0.808 | 0.796 | 53.932 |
| unstructured hi_res | ML | 0.841 | 0.904 | 0.588 | 0.749 | 3.008 |
| edgeparse | rules | 0.837 | 0.894 | 0.717 | 0.706 | 0.036 |
| opendataloader | rules | 0.831 | 0.902 | 0.489 | 0.739 | 0.015 |
| mineru | ML | 0.831 | 0.857 | 0.873 | 0.743 | 5.962 |
| pymupdf4llm | rules | 0.732 | 0.885 | 0.401 | 0.412 | 0.091 |
| unstructured fast | rules | 0.686 | 0.882 | 0.000 | 0.388 | 0.077 |
| markitdown | rules | 0.589 | 0.844 | 0.273 | 0.000 | 0.114 |
| *LiteParse v2 no OCR (claim, own run)* | rules | *0.886* | *0.917* | *0.818* | *0.821* | — |

**olmOCR-bench** (overall = macro-average):
- ML / VLM: Chandra 0.1.0 83.1*, olmOCR v0.4.0 82.4, Infinity-Parser 82.5*, PaddleOCR-VL 80.0*, Marker 1.10.1 76.1, MinerU 2.5.4 75.2*, Mistral OCR 72.0 (* = author-reported) ([README](https://raw.githubusercontent.com/allenai/olmocr/main/olmocr/bench/README.md)). Marker-run: marker balanced 76.0, MinerU pipeline 72.7, docling 50.3.
- Text-layer: marker no-OCR 43.6 (has a 20M layout model), LiteParse no OCR 39.6 (claim), pdf-inspector 33.7, opendataloader 32.5, markitdown 28.7.
- For all text-layer tools the two math categories score **0** by construction (LaTeX required), and baseline is 82.9–99.9 ([LiteParse](https://github.com/run-llama/liteparse)). Marker's no-OCR mode scores arXiv-math 0.0, tables 46.1, multi-column 67.0, headers/footers 92.8.

**READoc-arXiv** avg ([paper Table 3](https://arxiv.org/html/2409.05137v3)): Nougat-base 81.42, MinerU 73.07, Marker 64.98, Docling 58.83, PyMuPDF4LLM 40.55, MarkItDown 35.21, Tesseract 35.13. Reading-order block KTDS for MarkItDown is 97.13, i.e. **order is easy and structure is hard**.

**OmniDocBench v1.6** ([README](https://raw.githubusercontent.com/opendatalab/OmniDocBench/main/README.md)): MinerU-Pipeline 86.47, olmOCR 85.74, Mistral OCR 85.66, Marker 78.44; top specialised VLMs ≈97. No pure text-layer tool is listed, and the image-based inputs make it unsuitable for a text-layer engine except on the original-PDF slices.

**DP-Bench** (Upstage leaderboard, commercial APIs only): NID 82.2–96.6, TEDS 77.9–96.1 ([HF](https://huggingface.co/datasets/upstage/dp-bench)).

### 2.5 Harness sensitivity (important for rayopdf claims)

- Same corpus, different harness: docling TEDS is 0.887 in odl-bench but 0.568 in the EdgeParse harness. pymupdf4llm speed is 0.091 s/page vs 0.640 s/doc. EdgeParse is 0.837 (odl table) vs 0.857 (its own "official" run) vs 0.782 (its own harness) ([EdgeParse](https://github.com/raphaelmansuy/edgeparse), [odl-bench](https://github.com/opendataloader-project/opendataloader-bench)).
- liteparse is 22.4 on olmOCR in marker's run vs 39.6 in its own (different versions) ([marker](https://raw.githubusercontent.com/datalab-to/marker/master/README.md), [LiteParse](https://github.com/run-llama/liteparse)).
- The odl-bench TEDS mean covers only **42** documents and MHS **107** of 200 ([evaluation.json](https://raw.githubusercontent.com/opendataloader-project/opendataloader-bench/main/prediction/opendataloader/evaluation.json)). Differences of a few hundredths are within noise; report CIs.

## 3. Synthetic ground truth (render known sources to PDF)

**Practice in the field**
- READoc built exact PDF/Markdown pairs from three pipelines: arXiv LaTeX → LaTeXML → modified Nougat HTML→MD; GitHub READMEs → pandoc + Eisvogel LaTeX template → PDF; Zenodo DOCX → MarkItDown → python-docx, and HTML → pandoc → Chromium ([paper §4.1](https://arxiv.org/html/2409.05137v3)).
- Nougat trained on arXiv LaTeX-derived pages ([README](https://raw.githubusercontent.com/facebookresearch/nougat/main/README.md)). DocBank weakly labelled 500K pages from arXiv LaTeX ([arXiv](https://arxiv.org/abs/2006.01038)). PubLayNet/PubTabNet matched PMC PDF↔XML ([arXiv](https://arxiv.org/abs/1908.07836)).
- olmOCR 2 generates synthetic documents "with known ground-truth HTML source code" and extracts unit tests from them ([arXiv 2510.19817](https://arxiv.org/abs/2510.19817)). olmOCR-bench arXiv-math aligned rendered PDFs back to single-file TeX sources ([bench README](https://raw.githubusercontent.com/allenai/olmocr/main/olmocr/bench/README.md)).

**Pitfalls (evidence)**
- **Producer/renderer bias.** Nougat drops from 81.42 (arXiv) to 49.94 (Zenodo); Docling from 58.83 (arXiv) to 49.87 (pandoc-built GitHub subset) ([READoc](https://arxiv.org/html/2409.05137v3)). One producer means a single typographic dialect (fonts, heading sizes, list glyphs, table rules), which heuristics overfit easily. READoc-GitHub is all pandoc+Eisvogel and excludes tables and formulas.
- **GT conversion noise.** READoc keeps only documents converted "without any errors". LaTeXML/Nougat conversion and its custom macros need filtering; olmOCR excluded macro-dependent cases manually.
- **Normalization debt.** Each scorer strips different syntax (READoc removes links and images; olmOCR ignores Markdown; odl NID is plain text). GT must be canonicalized the same way per metric.
- **Tags and embedded-source leakage.** Tagged PDFs make structure recovery trivial (opendataloader's `--use-struct-tree` overrides hybrid, [PyPI](https://pypi.org/project/opendataloader-pdf/2.5.17)). rayomd's reversible profile embeds exact Markdown ([rayomd/docs/development/reversible_pdf_profile.md](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/reversible_pdf_profile.md), "Decision"). Synthetic test PDFs must exist in **untagged, no-embedded-source** variants, otherwise the benchmark measures tag reading rather than inference.
- **Synthetic ≠ wild.** Synthetic PDFs have clean ToUnicode maps, embedded fonts and no scans. Robustness and quality on real data still need §4 and §2 corpora.

## 4. Real-world PDF corpora (robustness, fuzz seeds, regression)

| Corpus | Size | License / access | Notes |
|---|---|---|---|
| **SafeDocs CC-MAIN-2021-31-PDF-UNTRUNCATED** ([README](https://digitalcorpora.s3.amazonaws.com/corpora/files/CC-MAIN-2021-31-PDF-UNTRUNCATED/README.md)) | 7.93M unique PDFs (8.4M URLs), 7,933 zips of 1–2.8 GB, ≈8 TB uncompressed | Free on AWS Open Data (Digital Corpora); subject to Common Crawl terms of use | Truncated (>1 MB) files re-fetched; pdfinfo metadata CSV (producer, tagged, pages, encryption); 37,692 pdfinfo exit-1. Best for stratified sampling by producer |
| **UNSAFE-DOCS** ([index](https://raw.githubusercontent.com/pdf-association/pdf-corpora/master/README.md)) | >5.0M files incl. synthetic malicious ones | Digital Corpora | Known malformed/malicious; fuzz seeds only, inside a sandbox |
| **SafeDocs issue-tracker corpus** (same index) | >32K files, >31 GB | Tika regression server tarballs | Bug-tracker attachments; high malformed rate |
| **GovDocs1** (same index; [Digital Corpora](https://digitalcorpora.org/corpora/file-corpora/files)) | ~1M files, >231,000 PDFs, 1000 zips ≈308 GB | "Freely redistributable"; AWS S3 | Old (2010) .gov producers |
| **pdf.js test corpus** ([tree](https://github.com/mozilla/pdf.js/tree/master/test/pdfs)) | 897 PDFs used by pdf_oxide's benchmark ([pdf_oxide](https://github.com/yfedoseev/pdf_oxide)) | Repo license; many entries are `.pdf.link` URLs fetched on demand ([API listing](https://api.github.com/repos/mozilla/pdf.js/contents/test/pdfs?ref=master)) | Font/encoding edge cases |
| **PDFium regression suite** ([pdfium_tests](https://pdfium.googlesource.com/pdfium_tests/)) | not stated | Git; license not verified | Real + synthetic, rendering expectations |
| **PDF Association pdf-differences** ([repo](https://github.com/pdf-association/pdf-differences)) | small, targeted | PDFs CC BY 4.0, code Apache-2.0 | Interop differentials (mostly rendering) |
| **veraPDF corpus** ([repo](https://github.com/veraPDF/veraPDF-corpus)) | 2,907 files used by pdf_oxide | see repo | PDF/A atomic tests |
| **PDF/UA Reference Suite**, Matterhorn ([index](https://raw.githubusercontent.com/pdf-association/pdf-corpora/master/README.md)) | 10 PDFs | PDF Association | Well-tagged reference; useful for struct-tree path |
| **iText regression suite** (same index) | ~4,000 small PDFs | AGPL repo | Feature-specific, encrypted PDFs |
| **Artifex MuPDF test corpus**, **Qiqqa Evil-PDF** (same index) | — | git | Malformed/malicious |
| **arXiv bulk** ([S3 doc](https://raw.githubusercontent.com/arXiv/arxiv-docs/develop/source/help/bulk_data_s3.md)) | PDFs ≈2.7 TB, sources ≈2.9 TB (Mar 2023), total ≈9.2 TB (Apr 2025) | Requester-pays S3 (`s3://arxiv/pdf`, `s3://arxiv/src`, ~500 MB tars + manifest). Default arXiv license does **not** grant redistribution | Paired LaTeX source + PDF, the main source of exact GT for papers/math |
| **DocLayNet extra** ([repo](https://github.com/DS4SD/DocLayNet)) | 80,863 single-page PDFs + text cells | CDLA-P-1.0 | Born-digital, 6 domains, human layout labels: good for heading/table/caption *detection* scoring |

## 5. Conclusions and recommended evaluation design for rayopdf

### 5.1 What fast rule-based tools achieve vs ML

- **Reading order / plain text (born-digital):** near parity. NID 0.885–0.917 rule-based vs 0.857–0.934 ML on odl-bench. READoc block KTDS ≥97 even for MarkItDown and Tesseract.
- **Headings:** classic rule-based tools are weak (MHS 0.00–0.41; READoc heading-tree 0.9–20.8). Newer engines claim 0.71–0.82, i.e. reachable with font-statistics, outline and numbering heuristics, but unproven outside vendor runs.
- **Tables:** the biggest gap. Ruling-line plus text-alignment heuristics give TEDS 0.27–0.49 (classic) and claimed 0.72–0.82 (EdgeParse, LiteParse), vs 0.81–0.93 for TableFormer-class models. Borderless tables, spans and merged headers are the failure mode. Markdown tables cannot express rowspan/colspan, which caps olmOCR table scores ([bench README](https://raw.githubusercontent.com/allenai/olmocr/main/olmocr/bench/README.md)).
- **Math:** the text layer has no LaTeX. olmOCR math categories score 0 for every text-layer tool, and marker's no-OCR mode notes that "garbled math glyphs" also cost baseline points ([marker](https://raw.githubusercontent.com/datalab-to/marker/master/README.md)). Best honest output is Unicode math or `$…$` with low confidence.
- **Scans:** impossible without OCR (old_scans 13–19 for LiteParse even with OCR). rayopdf must detect image-only pages and report them as unconverted rather than emit garbage. This matches the rayomd decision doc requirement for explicit uncertainty ([rayomd/docs/development/arbitrary_pdf_research_decision.md](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/arbitrary_pdf_research_decision.md), "Consequences").
- **Multi-column / complex layouts:** all systems degrade (READoc Table 7; OmniDocBench paper). Rule-based tools reach 49.7–69.1 on olmOCR multi_column vs 76–84 for ML.
- **Speed:** native rule-based tools run at 1–40 ms/page. Even "fast" ML pipelines are ≥0.5 s/page on CPU. A C++ engine targeting ≤5 ms/page with structure quality ≥ opendataloader-local would dominate the speed/quality Pareto front, which is the realistic goal. Matching docling-level TEDS without a model is unproven.

### 5.2 Recommended evaluation design

**A. Quality tiers (all scored by each benchmark's own unmodified evaluator, versions pinned)**
1. **odl-bench** (200 PDFs, NID / TEDS / MHS). The primary public comparability target. Report bootstrap 95% CIs, because TEDS covers n=42 and MHS n=107.
2. **olmOCR-bench**, reporting both "overall" and a **born-digital subset** (exclude old_scans and old_scans_math, as marker does), plus per-category results. Math categories are expected to score 0 and should be stated as such.
3. **READoc** (arXiv/GitHub/Zenodo): the only whole-document benchmark with a heading **tree** metric, multi-page input and published pymupdf4llm/MarkItDown/MinerU/Marker baselines.
4. **ParseBench** text_formatting + text_content + table groups (semantic formatting tests bold/strike/sup/sub and title hierarchy, which matter for Markdown).
5. **DP-Bench** NID/TEDS (MIT license, fast). **OmniDocBench** only on the original-PDF slices, and only if non-commercial use is acceptable.
6. **In-house synthetic corpus with exact GT** (see B), scored with the same NID / TEDS / MHS code plus per-construct unit tests in the olmOCR style (heading level, list nesting, code-fence presence, link target, footnote linkage, emphasis span).

**B. Synthetic corpus construction (anti-overfitting)**
- Sources: CommonMark/GFM feature fixtures; READoc-style READMEs; arXiv LaTeX (single-file sources from `s3://arxiv/src`, internal use only, never redistributed); HTML pages.
- **≥5 producers** per source: pandoc→xelatex and pandoc→typst (`qtbench` image), chromium print-to-PDF (`qtbench`), rayomd itself, and LibreOffice DOCX→PDF. LibreOffice is not in any listed image, so a new docker image is needed.
- Variants per producer: tagged/untagged, rayomd with and without the reversible source profile (the evaluated path must never read embedded source), fonts subset vs non-embedded, one vs two columns.
- **Held-out producer split**: tune heuristics on N−1 producers and report on the unseen one. Report per-producer scores so one producer cannot carry the mean.

**C. Comparators in docker (host stays clean)**
- One pinned image per tool:
  - pymupdf4llm / PyMuPDF 2.x (AGPL; run as a black box only, never vendor)
  - opendataloader-pdf (JRE 11+), local and hybrid modes
  - docling (CPU)
  - marker `--mode fast --disable_ocr` and default CPU mode (needs llama.cpp `llama-server`). Label CPU numbers as not comparable to marker's B200 figures.
  - MinerU `flash` / `basic` tiers
  - LiteParse, EdgeParse, MarkItDown, pdftotext / mutool (`pdftools` / `rgui-arch` images) as raw baselines
- Bake model weights into images at build time, then run conversions with `--network none`, a read-only corpus mount (`:ro`) and a write-only output volume. Use `--cpus`, `--cpuset-cpus` and `--memory` limits so every tool gets the same budget.
- Timing:
  - Single-thread and all-12-thread throughput, both reported.
  - Cold (process start + first doc) vs warm (batch); JVM and Python import costs dominate cold runs ([Xberg cold-start method](https://xberg.io/benchmarks)).
  - Per-page wall time via hyperfine (`perfbox`), peak RSS via `/usr/bin/time -v`, plus failure/timeout/empty-output buckets kept in the denominator.
- Robustness: run rayopdf over stratified SafeDocs samples, by producer from the pdfinfo CSV, plus pdf.js, pdf-differences and veraPDF suites. Gate on zero crashes, a hard time and memory cap, and a deterministic output hash. Seed fuzzers from UNSAFE-DOCS and issue-tracker PDFs inside the sandbox.

**D. Reporting rules**
- Mark every third-party number as reproduced or claimed, with version, hardware, harness commit and thread count.
- Publish the Pareto plot (quality vs ms/page) rather than a single "#1" claim.
- Publish per-category failure tables (scans, math, borderless tables, multi-column) with rayopdf's explicit "unconverted / low-confidence" counts. This satisfies the corpus, latency, RSS and security evidence required by the rayomd decision doc.
