# Features of PDF-to-Markdown converters: what RayoPDF lacks (research for 1.2, 2026-11-26)

Research note of 2026-11-26, written after the release of 1.1.0 (619071e). It compares features,
where [landscape.md](landscape.md) (2026-11-05) compared benchmark scores. Part 1 lists the tools
and versions read; part 2 is the feature matrix; part 3 lists RayoPDF's gaps with the evidence that
users want them and a rough cost judged from the 1.1.0 code; part 4 ranks ten of them; part 5 lists
what RayoPDF has that the others lack; part 6 says what this note did not verify.

Conventions:

- Every value of a third-party tool comes from its own documentation, repository, release notes or
  API reference, read on 2026-11-26 at the version named in §1. A link sits in each cell.
- "claimed" marks a vendor's number or promise that was not measured here. The four commercial
  services (LlamaParse, Mistral OCR, Azure Document Intelligence, AWS Textract) are claims
  throughout.
- "not documented" means the sources read do not mention the feature; it does not mean the tool
  lacks it.
- **[INFERENCE]** marks my reasoning, not a statement of a source.
- Numbers measured here say how: the command, the corpus and the count.

## 0. Summary

- **RayoPDF loses text that the others keep: form-field values and text drawn by annotations.**
  RayoPDF reads link annotations only (`src/layout/interpreter.cpp:1682-1714`) and never draws an
  annotation's appearance. On SafeDocs document 0000854, whose whole text sits in four form fields,
  RayoPDF 1.1.0 writes 81 bytes (a signature line) and no comment that text is missing (§3, G1).
  Apache Tika, PyMuPDF, liteparse and docling (opt-in) read field values. ParseBench 2 scores forms
  as one of its six dimensions
  ([README](https://raw.githubusercontent.com/run-llama/ParseBench/main/README.md)).
- **Users cannot install it the way the README says.** `pip install rayopdf` is the README's
  install line, but `https://pypi.org/pypi/rayopdf/json` answered 404 on 2026-11-26, and the npm
  package is attached to the release only. The competitors closest in kind ship for five platforms
  (edgeparse: Linux, macOS and Windows on x64, Linux and macOS on arm64; liteparse: Linux and macOS
  on x64 and arm64); RayoPDF ships Linux x64 and Windows x64 (G2, G3).
- **Grounding below the block.** RayoPDF's JSON gives one box per block and page. PyMuPDF,
  pdfplumber, liteparse, docling, LlamaParse, Azure and Textract give line or word boxes,
  ParseBench scores "visual grounding" as one of its dimensions, and bounding boxes draw more issue
  titles (128 relevant in the eleven trackers searched) than any other topic RayoPDF lacks (G4).
- **Chunking by tokens, overlap and pages** is offered by docling, PyMuPDF and unstructured;
  RayoPDF cuts by headings and bytes only (G5).
- **Structure is dropped or never read:** the outline (read for heading levels, never written),
  page labels, internal links, and in JSON the block types caption, formula and running header
  (G6, G7).
- **RayoPDF's own strengths stay unmatched among local tools** (§5): about 1 ms a page as a fresh
  process, a 2.1 MB static executable, footnotes as linked `[^n]`, LaTeX rebuilt from glyphs with no
  model, tables merged across pages, the tagged structure tree checked before use, and comments
  where text could not be read.

## 1. Tools and versions read

All read on 2026-11-26. "Kind": text-layer (no model on the default path), ML (layout or table
models, CPU possible), VLM (vision-language model), service (hosted API).

| Tool | Version, release date | Kind | Licence | Main sources |
|---|---|---|---|---|
| RayoPDF | 1.1.0, 2026-11-26 | text-layer, small trees; OCR add-on | Apache-2.0 | [README](../../README.md), [guides](../guide/cli.md), code at 619071e |
| docling | 2.153.0, 2026-11-25 (docling-core 2.108.0, docling-serve 1.39.0) | ML, VLM presets | MIT; models Apache-2.0 / CDLA-P-2.0 | [docs](https://docling-project.github.io/docling/), [release](https://github.com/docling-project/docling/releases/tag/v2.153.0) |
| marker | marker-pdf 2.2.0, 2026-11-12 | ML + VLM | Apache-2.0; weights modified OpenRAIL-M | [README](https://github.com/datalab-to/marker), [release](https://github.com/datalab-to/marker/releases/tag/v2.2.0) |
| MinerU | 4.2.3, 2026-11-16 | ML + VLM (tiers flash, basic, standard, advanced) | MinerU Open Source License (Apache-2.0 plus terms) | [docs](https://opendatalab.github.io/MinerU/), [release](https://github.com/opendatalab/MinerU/releases/tag/mineru-4.2.3-released) |
| PyMuPDF (former pymupdf4llm and pymupdf-layout) | 2.2, 2026-11-26 | text-layer + small GNN (layout mode, default) | AGPL-3.0 or commercial | [README](https://github.com/pymupdf/PyMuPDF), [docs/document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst) |
| opendataloader-pdf | 2.5.24, 2026-11-23 | text-layer; hybrid mode calls docling | Apache-2.0 | [README](https://github.com/opendataloader-project/opendataloader-pdf), [docs](https://opendataloader.org/docs/reference/cli-options) |
| kordoc | 4.25.20, 2026-11-25 | text-layer; optional OCR and formula models | MIT | [README-EN](https://github.com/chrisryugj/kordoc/blob/main/README-EN.md), [usage-en](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md) |
| edgeparse | 0.3.2, 2026-10-02 | text-layer | Apache-2.0 | [README](https://github.com/raphaelmansuy/edgeparse), [release](https://github.com/raphaelmansuy/edgeparse/releases/tag/v0.3.2) |
| liteparse | 2.21.2, 2026-11-24 | text-layer; optional ocria-nano | Apache-2.0; ocria-nano modified OpenRAIL | [README](https://github.com/run-llama/liteparse), [docs](https://developers.llamaindex.ai/liteparse/guides/extraction/) |
| unstructured (library) | 0.27.38, 2026-11-24 | text-layer (`fast`) or ML (`hi_res`) | Apache-2.0 (telemetry on by default) | [docs](https://docs.unstructured.io/open-source/core-functionality/partitioning), [PyPI](https://pypi.org/project/unstructured/0.27.38/) |
| pdfplumber | 0.11.11, 2026-11-20 | positioned text, no Markdown | MIT | [README](https://github.com/jsvine/pdfplumber) |
| MarkItDown | 0.1.9, 2026-10-21 | text-layer (pdfminer.six) | MIT | [README](https://github.com/microsoft/markitdown), [PDF converter](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/converters/_pdf_converter.py) |
| Apache Tika | 4.2.0, 2026-10-28 (3.4.0 maintenance, 2026-11-23) | text-layer (PDFBox) | Apache-2.0 | [PDF parser](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html), [4.0.0 notes](https://tika.apache.org/4.0.0/index.html) |
| LlamaParse | Parse v2 tiers of 2026-11-09 to 2026-11-19; SDK llama-cloud 2.20.0 | service (VLM) | proprietary | [configuring parse](https://developers.llamaindex.ai/llamaparse/parse/guides/configuring-parse/), [response format](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/) |
| Mistral OCR | mistral-ocr-4-1, GA 2026-08-31 | service (VLM) | proprietary | [basic OCR](https://docs.mistral.ai/studio/document-processing/basic-ocr), [API](https://docs.mistral.ai/api/endpoint/ocr) |
| Azure Document Intelligence | prebuilt-layout, API v4.0 2024-11-30 | service (ML) | proprietary | [layout](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0), [Markdown elements](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/markdown-elements?view=doc-intel-4.0.0) |
| AWS Textract | API 2018-06-27; Textract Insights GA 2026-11-20 | service (ML) | proprietary | [AnalyzeDocument](https://docs.aws.amazon.com/textract/latest/APIReference/API_AnalyzeDocument.html), [Insights formats](https://docs.aws.amazon.com/textract/latest/dg/insights-output-formats.html) |

Changes since landscape.md that matter here: pymupdf4llm and pymupdf-layout are now empty packages
and their code lives in `pymupdf` 2.x, with layout mode on by default
([PyPI](https://pypi.org/project/pymupdf/2.2/)); Tika 4.x writes Markdown by default
([4.0.0](https://tika.apache.org/4.0.0/index.html)); MinerU 4.x replaced the backends `pipeline`,
`hybrid` and `vlm` with four tiers ([migration](https://opendatalab.github.io/MinerU/reference/migration_4/));
Textract gained native Markdown with Insights on 2026-11-20
([announcement](https://aws.amazon.com/about-aws/whats-new/2026/11/textract-insights-ga/)).

## 2. Feature matrix

Five tables over the same rows. "yes" means the feature is on the default path or one option away;
"opt-in model" means it needs a model download or a service call.

### 2.1 Inputs and outputs

| Tool | Page selection | Password | Bytes or standard input | Other inputs | JSON tree | HTML document | Plain text | Other outputs |
|---|---|---|---|---|---|---|---|---|
| **RayoPDF** | `--pages=A-B` ([cli](../guide/cli.md)) | R2–R6; no certificate encryption ([cli](../guide/cli.md)) | bytes in Python, JS, C ABI; the CLI reads a path only ([python](../guide/python.md), [main.cpp](../../src/cli/main.cpp)) | PDF only ([README](../../README.md)) | schema 1, typed blocks ([formats](../guide/formats.md)) | no; tables only, and `inspect` is a layout view ([formats](../guide/formats.md)) | no; `--plain` is Markdown without inline markup ([cli](../guide/cli.md)) | chunks JSONL, report record ([formats](../guide/formats.md)) |
| docling | `--page-range` ([cli](https://docling-project.github.io/docling/reference/cli/#docling-convert)) | `--pdf-password` ([cli](https://docling-project.github.io/docling/reference/cli/#docling-convert)) | `DocumentStream` ([options](https://docling-project.github.io/docling/usage/advanced_options/#convert-from-binary-pdf-streams)) | about 30 formats ([formats](https://docling-project.github.io/docling/usage/supported_formats/#supported-input-formats)) | DoclingDocument, DocTags ([formats](https://docling-project.github.io/docling/usage/supported_formats/#supported-output-formats)) | yes, spans kept ([serialization](https://docling-project.github.io/docling/concepts/serialization/#table-cell-spans)) | yes ([formats](https://docling-project.github.io/docling/usage/supported_formats/#supported-output-formats)) | YAML, LaTeX, WebVTT ([formats](https://docling-project.github.io/docling/usage/supported_formats/#supported-output-formats)) |
| marker | `--page_range`, 0-based ([README](https://github.com/datalab-to/marker#convert-a-single-file)) | not documented ([provider](https://github.com/datalab-to/marker/blob/master/marker/providers/pdf.py)) | `BytesIO`, through a temporary file ([pdf.py](https://github.com/datalab-to/marker/blob/master/marker/converters/pdf.py#L180-L225)) | images, Office, HTML, EPUB ([install](https://github.com/datalab-to/marker#installation)) | block tree ([JSON](https://github.com/datalab-to/marker#json)) | yes ([HTML](https://github.com/datalab-to/marker#html)) | no ([README](https://github.com/datalab-to/marker#convert-a-single-file)) | chunks, OCR JSON ([chunks](https://github.com/datalab-to/marker#chunks)) |
| MinerU | `--pages` ([page ranges](https://opendatalab.github.io/MinerU/next/page-ranges/)) | no: `file_encrypted` ([README](https://github.com/opendatalab/MinerU#error-recovery)) | API takes bytes; SDK a path ([tasks](https://opendatalab.github.io/MinerU/usage/tasks/#uploads-urls-and-caching)) | images, Office, EPUB, OFD, HTML ([inputs](https://github.com/opendatalab/MinerU#supported-inputs)) | MiddleJson, content list ([outputs](https://opendatalab.github.io/MinerU/reference/output_files/#intermediate-json)) | yes ([rendering](https://opendatalab.github.io/MinerU/next/middle-json/rendering/)) | not documented ([targets](https://opendatalab.github.io/MinerU/reference/output_files/#nine-rendering-targets)) | DOCX, EPUB, LaTeX, PDF ([targets](https://opendatalab.github.io/MinerU/reference/output_files/#nine-rendering-targets)) |
| PyMuPDF | `pages=` ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L458)) | yes ([README](https://github.com/pymupdf/PyMuPDF#core-capabilities)) | `stream=` ([open](https://github.com/pymupdf/PyMuPDF/blob/main/docs/how-to-open-a-file.rst)) | XPS, EPUB, images; Office commercial ([formats](https://github.com/pymupdf/PyMuPDF#supported-file-formats)) | boxes, lines, spans ([schema](https://github.com/pymupdf/PyMuPDF/blob/main/docs/json-schema.rst#L1038-L1059)) | tables only ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L462)) | `to_text` ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L556-L600)) | chunks ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L480-L514)) |
| opendataloader-pdf | `--pages` ([CLI](https://opendataloader.org/docs/reference/cli-options#options)) | `--password` ([CLI](https://opendataloader.org/docs/reference/cli-options#encrypted-pdf)) | `convert_bytes`, a JVM a call ([README](https://github.com/opendataloader-project/opendataloader-pdf#pdf-bytes-in-string-out)) | PDF only ([matrix](https://github.com/opendataloader-project/opendataloader-pdf#capability-matrix)) | `kids` tree with schema ([schema](https://opendataloader.org/docs/reference/json-schema)) | yes ([formats](https://github.com/opendataloader-project/opendataloader-pdf#output-formats)) | yes ([formats](https://github.com/opendataloader-project/opendataloader-pdf#output-formats)) | tagged PDF, annotated PDF ([formats](https://github.com/opendataloader-project/opendataloader-pdf#output-formats)) |
| kordoc | `--pages` ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#parse-a-document)) | HWP and HWPX only; PDF not documented ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#parse-a-document)) | buffer ([Python SDK](https://github.com/chrisryugj/kordoc/blob/main/sdk/python/README.md)) | HWP, HWPX, Office, images ([formats](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#-supported-formats)) | IR blocks, outline ([types.ts](https://github.com/chrisryugj/kordoc/blob/main/src/types.ts)) | API only ([render](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#generate--render)) | text-centric Markdown only ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#parse-a-document)) | HWPX, HWP ([features](https://github.com/chrisryugj/kordoc/blob/main/README-EN.md#features)) |
| edgeparse | `--pages` ([CLI](https://github.com/raphaelmansuy/edgeparse/blob/main/tutorials/01-cli.md#4-page-ranges)) | `--password` ([CLI](https://github.com/raphaelmansuy/edgeparse/blob/main/tutorials/01-cli.md#9-encrypted-pdfs)) | WASM bytes; Python and Node a path ([WASM](https://github.com/raphaelmansuy/edgeparse#webassembly-sdk)) | PDF only ([FAQ](https://github.com/raphaelmansuy/edgeparse#does-it-support-scanned-pdfs)) | typed elements ([formats](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/05-output-formats.md)) | yes ([formats](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/05-output-formats.md)) | yes ([formats](https://github.com/raphaelmansuy/edgeparse#output-formats)) | CSV of tables ([formats](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/05-output-formats.md)) |
| liteparse | `--target-pages` ([CLI](https://github.com/run-llama/liteparse#parse-command)) | `--password` ([CLI](https://github.com/run-llama/liteparse#parse-command)) | bytes; CLI `lit parse -` ([Python](https://github.com/run-llama/liteparse/blob/main/packages/python/README.md#parsing-from-bytes)) | Office through LibreOffice, images ([inputs](https://github.com/run-llama/liteparse#multi-format-input-support)) | text items, blocks, forms, annotations ([extraction](https://developers.llamaindex.ai/liteparse/guides/extraction/)) | no ([CLI](https://github.com/run-llama/liteparse#parse-command)) | yes, the default ([CLI](https://github.com/run-llama/liteparse#parse-command)) | page screenshots ([screenshots](https://github.com/run-llama/liteparse#generate-screenshots)) |
| unstructured | no ([pdf.py](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/partition/pdf.py#L160-L182)) | `password=` ([pdf.py](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/partition/pdf.py#L179)) | `file=` ([partitioning](https://docs.unstructured.io/open-source/core-functionality/partitioning)) | 20+ types ([partitioning](https://docs.unstructured.io/open-source/core-functionality/partitioning)) | flat element list ([elements](https://docs.unstructured.io/open-source/concepts/document-elements)) | yes ([staging](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/staging/base.py)) | yes ([staging](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/staging/base.py)) | CSV, DataFrame ([integrations](https://docs.unstructured.io/open-source/integrations)) |
| pdfplumber | `--pages`, `pages=` ([CLI](https://github.com/jsvine/pdfplumber#command-line-interface)) | `password=` ([loading](https://github.com/jsvine/pdfplumber#loading-a-pdf)) | file object ([loading](https://github.com/jsvine/pdfplumber#loading-a-pdf)) | PDF only ([README](https://github.com/jsvine/pdfplumber#pdfplumber)) | objects ([CLI](https://github.com/jsvine/pdfplumber#command-line-interface)) | no ([text](https://github.com/jsvine/pdfplumber#extracting-text)) | `extract_text` ([text](https://github.com/jsvine/pdfplumber#extracting-text)); no Markdown | CSV ([CLI](https://github.com/jsvine/pdfplumber#command-line-interface)) |
| MarkItDown | no ([PDF converter](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/converters/_pdf_converter.py)) | no ([PDF converter](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/converters/_pdf_converter.py)) | stream, stdin ([_markitdown.py](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/_markitdown.py#L329-L560)) | Office, images, audio, HTML and more ([README](https://github.com/microsoft/markitdown#markitdown)) | no ([result](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/_base_converter.py)) | no ([result](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/_base_converter.py)) | no; Markdown only ([result](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/_base_converter.py)) | none |
| Apache Tika | `maxPages` only ([PDF parser](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html)) | `--password` ([CLI](https://tika.apache.org/docs/4.2.x/using-tika/cli/index.html)) | stream, stdin ([Java API](https://tika.apache.org/docs/4.2.x/using-tika/java-api/index.html)) | over 1,000 types ([site](https://tika.apache.org/)) | `/rmeta` metadata and content ([server](https://tika.apache.org/docs/4.2.x/using-tika/server/index.html)) | XHTML ([CLI](https://tika.apache.org/docs/4.2.x/using-tika/cli/index.html)) | yes ([CLI](https://tika.apache.org/docs/4.2.x/using-tika/cli/index.html)) | XMP, attachments ([CLI](https://tika.apache.org/docs/4.2.x/using-tika/cli/index.html)) |
| LlamaParse | `target_pages` ([config](https://developers.llamaindex.ai/llamaparse/parse/guides/configuring-parse/#page-ranges)) | no: `PDF_IS_PROTECTED` ([troubleshooting](https://developers.llamaindex.ai/llamaparse/general/troubleshooting/#parse-errors)) | bytes, URL ([SDK](https://pypi.org/project/llama-cloud/2.20.0/)) | 130+ types ([types](https://developers.llamaindex.ai/llamaparse/general/supported_document_types/)) | `items` tree ([format](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#items-pages)) | tables only ([tables](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#tables)) | yes ([spatial text](https://developers.llamaindex.ai/llamaparse/parse/guides/configuring-parse/#spatial-text)) | XLSX, word-box JSONL ([downloads](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#download-urls)) |
| Mistral OCR | `pages`, 0-based ([API](https://docs.mistral.ai/api/endpoint/ocr#operation-ocr_v1_ocr_post)) | not documented ([FAQ](https://docs.mistral.ai/studio/document-processing/basic-ocr#faq)) | base64, file, URL ([PDFs](https://docs.mistral.ai/studio/document-processing/basic-ocr#pdfs)) | Office, images, LaTeX and more ([FAQ](https://docs.mistral.ai/studio/document-processing/basic-ocr#faq)) | pages and blocks ([API](https://docs.mistral.ai/api/endpoint/ocr)) | tables only ([features](https://docs.mistral.ai/studio/document-processing/basic-ocr#key-features)) | not documented | JSON by schema ([annotations](https://docs.mistral.ai/studio/document-processing/annotations)) |
| Azure DI layout | `pages=` ([API](https://learn.microsoft.com/en-us/rest/api/aiservices/document-models/analyze-document?view=rest-aiservices-v4.0%20%282024-11-30%29&tabs=HTTP)) | no ([input](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#input-requirements)) | base64, URL ([API](https://learn.microsoft.com/en-us/rest/api/aiservices/document-models/analyze-document?view=rest-aiservices-v4.0%20%282024-11-30%29&tabs=HTTP)) | images, DOCX, XLSX, PPTX, HTML ([types](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#supported-file-types)) | `analyzeResult` ([data](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#data-extraction)) | HTML tables and figures inside Markdown ([Markdown](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/markdown-elements?view=doc-intel-4.0.0)) | `content` ([layout](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#output-response-to-markdown-format)) | figure PNG crops ([figure API](https://learn.microsoft.com/en-us/rest/api/aiservices/document-models/get-analyze-result-figure?view=rest-aiservices-v4.0%20%282024-11-30%29&tabs=HTTP)) |
| AWS Textract | per query only ([StartDocumentAnalysis](https://docs.aws.amazon.com/textract/latest/APIReference/API_StartDocumentAnalysis.html)) | no ([limits](https://docs.aws.amazon.com/textract/latest/dg/limits-document.html)) | bytes up to 10 MB, synchronous ([API](https://docs.aws.amazon.com/textract/latest/APIReference/API_AnalyzeDocument.html)) | JPEG, PNG, TIFF ([limits](https://docs.aws.amazon.com/textract/latest/dg/limits-document.html)) | block graph ([Block](https://docs.aws.amazon.com/textract/latest/APIReference/API_Block.html)) | Insights only ([formats](https://docs.aws.amazon.com/textract/latest/dg/insights-output-formats.html)) | yes ([formats](https://docs.aws.amazon.com/textract/latest/dg/insights-output-formats.html)) | CSV of tables ([formats](https://docs.aws.amazon.com/textract/latest/dg/insights-output-formats.html#insights-output-formats-csv)) |

### 2.2 Elements: tables, lists, code, formulas, notes

| Tool | Table spans | Tables across pages | Nested lists | Code | Formulas as LaTeX | Footnotes | Captions |
|---|---|---|---|---|---|---|---|
| **RayoPDF** | row and column spans in JSON; HTML where needed ([formats](../guide/formats.md)) | merged into one table ([formats](../guide/formats.md)) | `depth` in JSON ([formats](../guide/formats.md)) | fenced blocks ([formats](../guide/formats.md)) | `--math=latex`, rebuilt from glyphs ([cli](../guide/cli.md)) | `[^n]`, linked to the reference ([formats](../guide/formats.md)) | no caption type: a paragraph ([formats](../guide/formats.md)) |
| docling | in JSON, HTML; Markdown flattens ([serialization](https://docling-project.github.io/docling/concepts/serialization/#table-cell-spans)) | not documented ([catalog](https://docling-project.github.io/docling/usage/model_catalog/#vlm-convert-stage)) | PDF not documented ([grouping](https://docling-project.github.io/docling/concepts/docling_document/#grouping)) | yes; language opt-in ([code](https://docling-project.github.io/docling/usage/enrichments/#code-understanding)) | opt-in model ([formula](https://docling-project.github.io/docling/usage/enrichments/#formula-understanding)) | table and picture notes ([serializer](https://github.com/docling-project/docling-core/blob/main/docling_core/transforms/serializer/markdown.py#L1249-L1255)) | yes ([options](https://docling-project.github.io/docling/usage/advanced_options/#extract-native-pdf-form-fields)) |
| marker | HTML spans ([tables](https://github.com/datalab-to/marker#extract-tables)) | with `--use_llm` ([hybrid](https://github.com/datalab-to/marker#hybrid-mode)) | yes ([list.py](https://github.com/datalab-to/marker/blob/master/marker/processors/list.py#L17-L99)) | yes ([Markdown](https://github.com/datalab-to/marker#markdown)) | yes, OCR or VLM ([usage](https://github.com/datalab-to/marker#usage)) | block type; markers as superscripts ([Markdown](https://github.com/datalab-to/marker#markdown)) | yes ([JSON](https://github.com/datalab-to/marker#json)) |
| MinerU | HTML spans, nested ([medium](https://opendatalab.github.io/MinerU/next/middle-json/current-medium/)) | merged ([4.2.0](https://opendatalab.github.io/MinerU/reference/changelog/#420)) | yes ([medium](https://opendatalab.github.io/MinerU/next/middle-json/current-medium/)) | yes, with language ([medium](https://opendatalab.github.io/MinerU/next/middle-json/current-medium/)) | yes ([medium](https://opendatalab.github.io/MinerU/next/middle-json/current-medium/)) | yes ([rendering](https://opendatalab.github.io/MinerU/next/middle-json/rendering/)) | yes ([rendering](https://opendatalab.github.io/MinerU/next/middle-json/rendering/)) |
| PyMuPDF | opt-in `refine=True` ([_table_spans.py](https://github.com/pymupdf/PyMuPDF/blob/main/src/_table_spans.py#L26-L60)) | not documented ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L464)) | levels, written flat ([document_layout.py](https://github.com/pymupdf/pymupdf4llm/blob/main/src/helpers/document_layout.py#L182-L201)) | yes ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L397)) | no; formulas as images ([document_layout.py](https://github.com/pymupdf/pymupdf4llm/blob/main/src/helpers/document_layout.py#L1025-L1030)) | as block quotes ([document_layout.py](https://github.com/pymupdf/pymupdf4llm/blob/main/src/helpers/document_layout.py#L612-L620)) | box class ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L2541-L2563)) |
| opendataloader-pdf | in JSON; HTML opt-in ([cells](https://opendataloader.org/docs/reference/json-schema#table-cells)) | linked by ids, not merged ([tables](https://opendataloader.org/docs/reference/json-schema#tables)) | yes ([lists](https://opendataloader.org/docs/reference/json-schema#lists)) | not documented ([matrix](https://github.com/opendataloader-project/opendataloader-pdf#capability-matrix)) | hybrid mode only ([formulas](https://github.com/opendataloader-project/opendataloader-pdf#formula-extraction-latex)) | not documented ([schema](https://opendataloader.org/docs/reference/json-schema)) | yes ([captions](https://opendataloader.org/docs/reference/json-schema#captions)) |
| kordoc | spans; nested tables (claimed 101/101) ([validation](https://github.com/chrisryugj/kordoc/blob/main/README-EN.md#validation)) | one block on its first page ([types.ts](https://github.com/chrisryugj/kordoc/blob/main/src/types.ts)) | yes ([types.ts](https://github.com/chrisryugj/kordoc/blob/main/src/types.ts)) | not documented ([types.ts](https://github.com/chrisryugj/kordoc/blob/main/src/types.ts)) | opt-in models, about 155 MB ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#parse-a-document)) | yes ([formats](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#-supported-formats)) | not documented |
| edgeparse | in JSON ([tables](https://github.com/raphaelmansuy/edgeparse/blob/main/tutorials/01-cli.md#6-table-detection-methods)) | linked ([pipeline](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/02-pipeline.md)) | yes ([matrix](https://github.com/raphaelmansuy/edgeparse#capability-matrix)) | not documented ([data model](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/03-data-model.md)) | not documented ([data model](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/03-data-model.md)) | detected ([pipeline](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/02-pipeline.md)) | yes ([pipeline](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/02-pipeline.md)) |
| liteparse | not documented ([blocks](https://developers.llamaindex.ai/liteparse/guides/extraction/#layout-blocks)) | not documented ([tables](https://developers.llamaindex.ai/liteparse/guides/extraction/#tables)) | levels ([blocks](https://developers.llamaindex.ai/liteparse/guides/extraction/#layout-blocks)) | yes ([blocks](https://developers.llamaindex.ai/liteparse/guides/extraction/#layout-blocks)) | no ([README](https://github.com/run-llama/liteparse#without-ml-models)) | not documented | not documented |
| unstructured | `hi_res` HTML; spans not documented ([pdf.py](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/partition/pdf.py#L201-L207)) | not documented ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#table-specific-metadata)) | Word only ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#common-metadata-fields)) | `CodeSnippet` type ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#element-type)) | `Formula` type ([staging](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/staging/base.py#L442-L470)) | type in the enum only ([elements.py](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/documents/elements.py#L673-L717)) | `FigureCaption` ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#element-type)) |
| pdfplumber | no; lists of lists ([tables](https://github.com/jsvine/pdfplumber#extracting-tables)) | no | no | no | no | no | tagged PDFs only ([structure](https://github.com/jsvine/pdfplumber/blob/stable/docs/structure.md)) |
| MarkItDown | no ([PDF converter](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/converters/_pdf_converter.py)) | no | no | no | no | not documented | not documented |
| Apache Tika | tagged PDFs only ([tagged](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html#_tagged_pdfs_markedcontent)) | not documented | tagged PDFs only ([tagged](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html#_tagged_pdfs_markedcontent)) | not documented | no; `Alt` text of tagged formulas ([tagged](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html#_tagged_pdfs_markedcontent)) | not documented | tagged table captions ([tagged](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html#_tagged_pdfs_markedcontent)) |
| LlamaParse | HTML spans ([tables](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#tables)) | `merge_continued_tables` ([tables](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#tables)) | yes ([items](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#item-types)) | yes ([items](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#item-types)) | claimed; form not documented ([tiers](https://developers.llamaindex.ai/llamaparse/parse/guides/tiers/#agentic-plus)) | no item type ([items](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#item-types)) | image `caption` ([items](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#item-types)) |
| Mistral OCR | HTML spans ([features](https://docs.mistral.ai/studio/document-processing/basic-ocr#key-features)) | not documented | `list` blocks ([blocks](https://docs.mistral.ai/studio/document-processing/basic-ocr#block-extraction)) | `code` blocks ([blocks](https://docs.mistral.ai/studio/document-processing/basic-ocr#block-extraction)) | `equation` blocks (claimed) ([blocks](https://docs.mistral.ai/studio/document-processing/basic-ocr#block-extraction)) | not documented | `caption` blocks ([blocks](https://docs.mistral.ai/studio/document-processing/basic-ocr#block-extraction)) |
| Azure DI layout | HTML spans ([tables](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#tables)) | no; post-process ([tables](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#tables)) | not documented ([Markdown](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/markdown-elements?view=doc-intel-4.0.0)) | not documented | paid add-on ([formulas](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/add-on-capabilities?view=doc-intel-4.0.0#formula-extraction)) | `footnote` role ([roles](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#paragraph-roles)) | yes ([figure](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/markdown-elements?view=doc-intel-4.0.0#figure)) |
| AWS Textract | `MERGED_CELL` ([tables](https://docs.aws.amazon.com/textract/latest/dg/how-it-works-tables.html)) | Insights `page_indices` ([tables](https://docs.aws.amazon.com/textract/latest/dg/how-it-works-tables.html)) | `LAYOUT_LIST`, nesting not documented ([layout](https://docs.aws.amazon.com/textract/latest/dg/layoutresponse.html)) | not documented | not documented | `TABLE_FOOTER` only ([tables](https://docs.aws.amazon.com/textract/latest/dg/how-it-works-tables.html)) | `TABLE_TITLE`; Insights figure captions ([captions](https://docs.aws.amazon.com/textract/latest/dg/insights-summarization-captioning.html)) |

### 2.3 Elements: forms, annotations, links, navigation

| Tool | Form-field values (AcroForm) | Other annotations (comments, highlights) | Links in the Markdown | Outline (bookmarks) in the output | Running headers and footers | Page numbers and markers | Heading levels |
|---|---|---|---|---|---|---|---|
| **RayoPDF** | no: widgets are not read or drawn (§3, G1) | no: `/Link` with a URI only ([interpreter.cpp](../../src/layout/interpreter.cpp)) | yes; `--no-annotations` ([cli](../guide/cli.md)) | no: read for heading levels only ([roadmap M4](../development/roadmap.md)) | dropped by default; `--furniture=keep` ([cli](../guide/cli.md)) | `--page-markers`; page of each block in JSON ([formats](../guide/formats.md)) | 1–6 from styles, outline or structure tree ([formats](../guide/formats.md)) |
| docling | opt-in `--extract-form-fields` ([options](https://docling-project.github.io/docling/usage/advanced_options/#extract-native-pdf-form-fields)) | PDF not documented ([CHANGELOG](https://github.com/docling-project/docling/blob/main/CHANGELOG.md)) | yes since 2.83.0 ([#3131](https://github.com/docling-project/docling/issues/3131)) | read for levels only ([heading levels](https://docling-project.github.io/docling/usage/heading_levels/#bookmarks)) | furniture layer, left out ([document](https://docling-project.github.io/docling/concepts/docling_document/)) | `page_no`, page-break placeholder ([reference](https://docling-project.github.io/docling/reference/docling_document/)) | all level 1 unless `--heading-hierarchy` ([heading levels](https://docling-project.github.io/docling/usage/heading_levels/)) |
| marker | forms flattened first [INFERENCE] ([pdf.py](https://github.com/datalab-to/marker/blob/master/marker/providers/pdf.py#L42-L45)) | no: `draw_annots=False` ([pdf.py](https://github.com/datalab-to/marker/blob/master/marker/providers/pdf.py#L425-L434)) | yes ([pdf.py](https://github.com/datalab-to/marker/blob/master/marker/providers/pdf.py#L78-L81)) | TOC from detected headings ([document_toc.py](https://github.com/datalab-to/marker/blob/master/marker/processors/document_toc.py)) | removed ([README](https://github.com/datalab-to/marker#convert-a-single-file)) | `--paginate_output` ([markdown.py](https://github.com/datalab-to/marker/blob/master/marker/renderers/markdown.py#L77-L85)) | k-means over line heights ([sectionheader.py](https://github.com/datalab-to/marker/blob/master/marker/processors/sectionheader.py#L24-L89)) |
| MinerU | not documented ([medium](https://opendatalab.github.io/MinerU/next/middle-json/current-medium/)) | not documented | partial ([medium](https://opendatalab.github.io/MinerU/next/middle-json/current-medium/)) | TOC pages become index blocks ([rendering](https://opendatalab.github.io/MinerU/next/middle-json/rendering/)) | hidden by default ([rendering](https://opendatalab.github.io/MinerU/next/middle-json/rendering/)) | `page_number` blocks ([rendering](https://opendatalab.github.io/MinerU/next/middle-json/rendering/)) | 1–6 ([medium](https://opendatalab.github.io/MinerU/next/middle-json/current-medium/)) |
| PyMuPDF | `form_fields` in JSON ([schema](https://github.com/pymupdf/PyMuPDF/blob/main/docs/json-schema.rst#L1055-L1057)) | core API, not in the Markdown ([README](https://github.com/pymupdf/PyMuPDF#core-capabilities)) | legacy mode only ([document_layout.py](https://github.com/pymupdf/pymupdf4llm/blob/main/src/helpers/document_layout.py#L524-L526)) | `toc` in JSON ([schema](https://github.com/pymupdf/PyMuPDF/blob/main/docs/json-schema.rst#L1044-L1047)) | `header=False`, `footer=False` ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L377)) | `page_number` ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L423)) | by size rank ([document_layout.py](https://github.com/pymupdf/pymupdf4llm/blob/main/src/helpers/document_layout.py#L639-L663)) |
| opendataloader-pdf | tagged check boxes only ([tagged](https://github.com/opendataloader-project/opendataloader-pdf#tagged-pdf-support)) | not documented ([CLI](https://opendataloader.org/docs/reference/cli-options#options)) | opt-in `--include-links` ([CLI](https://opendataloader.org/docs/reference/cli-options#options)) | read for levels (hybrid) ([headings](https://github.com/opendataloader-project/opendataloader-pdf#heading-hierarchy)) | filtered; JSON elements ([schema](https://opendataloader.org/docs/reference/json-schema#headers-and-footers)) | separators ([CLI](https://opendataloader.org/docs/reference/cli-options#options)) | yes ([headings](https://github.com/opendataloader-project/opendataloader-pdf#heading-hierarchy)) |
| kordoc | label/value pairs from tables; AcroForm not documented ([forms](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#extract-and-fill-form-fields)) | HWP memos only ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#parse-a-document)) | yes ([formats](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#-supported-formats)) | heading tree, not bookmarks ([types.ts](https://github.com/chrisryugj/kordoc/blob/main/src/types.ts)) | removed from 3 pages on ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#parse-a-document)) | Markdown per page ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#parse-a-document)) | 1–6 ([types.ts](https://github.com/chrisryugj/kordoc/blob/main/src/types.ts)) |
| edgeparse | no ([CHANGELOG](https://github.com/raphaelmansuy/edgeparse/blob/main/CHANGELOG.md)) | not documented ([data model](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/03-data-model.md)) | not documented ([formats](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/05-output-formats.md)) | read for levels only ([README](https://github.com/raphaelmansuy/edgeparse#what-problems-does-this-solve)) | filtered ([CLI](https://github.com/raphaelmansuy/edgeparse/blob/main/tutorials/01-cli.md#11-layout-options)) | separators ([CLI](https://github.com/raphaelmansuy/edgeparse/blob/main/tutorials/01-cli.md#12-page-separators)) | yes ([formats](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/05-output-formats.md)) |
| liteparse | values of text, check box, radio and choice fields; XFA ([forms](https://developers.llamaindex.ai/liteparse/guides/extraction/#form-fields)) | yes, in JSON ([annotations](https://developers.llamaindex.ai/liteparse/guides/extraction/#annotations)) | yes ([links](https://developers.llamaindex.ai/liteparse/guides/markdown/#links)) | not documented | stripped ([headers](https://developers.llamaindex.ai/liteparse/guides/markdown/#headers-and-footers)) | stripped ([headers](https://developers.llamaindex.ai/liteparse/guides/markdown/#headers-and-footers)) | 1–6 ([blocks](https://developers.llamaindex.ai/liteparse/guides/extraction/#layout-blocks)) |
| unstructured | `FormKeysValues` from `hi_res`; AcroForm not documented ([pdf.py](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/partition/pdf.py#L237-L241)) | not documented | metadata only ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#additional-metadata-fields-by-document-type)) | not documented | element types; the user filters ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#element-type)) | `PageNumber`, `PageBreak` ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#element-type)) | one level in Markdown ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#metadata-for-document-hierarchy)) |
| pdfplumber | a pdfminer snippet ([form values](https://github.com/jsvine/pdfplumber#extracting-form-values)) | `.annots` ([objects](https://github.com/jsvine/pdfplumber#objects)) | as objects ([objects](https://github.com/jsvine/pdfplumber#objects)) | not documented | manual cropping | `page_number` ([page](https://github.com/jsvine/pdfplumber#the-pdfplumberpage-class)) | no |
| MarkItDown | not documented | not documented | no ([PDF converter](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/converters/_pdf_converter.py)) | not documented | not documented | no | no |
| Apache Tika | `extractAcroFormContent`, on by default ([PDF parser](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html)) | `extractAnnotationText`, on by default ([PDF parser](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html)) | in an annotation block; inline when tagged ([AbstractPDF2XHTML.java](https://github.com/apache/tika/blob/main/tika-parsers/tika-parsers-standard/tika-parsers-standard-modules/tika-parser-pdf-module/src/main/java/org/apache/tika/parser/pdf/AbstractPDF2XHTML.java#L1056-L1064)) | `extractBookmarksText`, on by default ([PDF parser](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html)) | tagged artifacts only ([tagged](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html#_tagged_pdfs_markedcontent)) | a `div` a page ([AbstractPDF2XHTML.java](https://github.com/apache/tika/blob/main/tika-parsers/tika-parsers-standard/tika-parsers-standard-modules/tika-parser-pdf-module/src/main/java/org/apache/tika/parser/pdf/AbstractPDF2XHTML.java#L335)) | tagged PDFs only ([tagged](https://tika.apache.org/docs/4.2.x/configuration/parsers/pdf-parser.html#_tagged_pdfs_markedcontent)) |
| LlamaParse | beta, read from the image ([forms](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#forms-beta)) | Word revisions only ([revisions](https://developers.llamaindex.ai/llamaparse/parse/guides/configuring-parse/#revision-tracking)) | `annotate_links` ([API](https://developers.llamaindex.ai/reference/resources/parsing/methods/create)) | not documented ([items](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#item-types)) | header and footer fields ([crop box](https://developers.llamaindex.ai/llamaparse/parse/guides/configuring-parse/#crop-box)) | printed page number ([option](https://developers.llamaindex.ai/llamaparse/parse/guides/configuring-parse/#printed-page-numbers)) | 1–6 ([items](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#item-types)) |
| Mistral OCR | not documented ([annotations](https://docs.mistral.ai/studio/document-processing/annotations)) | not documented | URL list a page ([API](https://docs.mistral.ai/api/endpoint/ocr)) | not documented | `extract_header`, `extract_footer` ([API](https://docs.mistral.ai/api/endpoint/ocr#operation-ocr_v1_ocr_post)) | not documented | `title` blocks ([basic OCR](https://docs.mistral.ai/studio/document-processing/basic-ocr)) |
| Azure DI layout | key-value add-on, from the image ([add-ons](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/add-on-capabilities?view=doc-intel-4.0.0#key-value-pairs)) | not documented | not documented ([Markdown](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/markdown-elements?view=doc-intel-4.0.0)) | not documented | roles, in HTML comments ([Markdown](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/markdown-elements?view=doc-intel-4.0.0#pagenumberpageheaderpagefooter)) | `<!-- PageNumber -->` ([Markdown](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/markdown-elements?view=doc-intel-4.0.0#pagenumberpageheaderpagefooter)) | title, section headings, sections ([sections](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#sections)) |
| AWS Textract | `FORMS`, from the image ([API](https://docs.aws.amazon.com/textract/latest/APIReference/API_AnalyzeDocument.html)) | not documented ([Block](https://docs.aws.amazon.com/textract/latest/APIReference/API_Block.html)) | not documented ([Insights](https://docs.aws.amazon.com/textract/latest/dg/insights-extraction-results.html)) | not documented | `LAYOUT_HEADER`, `LAYOUT_FOOTER` ([layout](https://docs.aws.amazon.com/textract/latest/dg/layoutresponse.html)) | `LAYOUT_PAGE_NUMBER` ([layout](https://docs.aws.amazon.com/textract/latest/dg/layoutresponse.html)) | two levels ([layout](https://docs.aws.amazon.com/textract/latest/dg/layoutresponse.html)) |

### 2.4 Coordinates, images, OCR and scripts

| Tool | Coordinates | Images | Image descriptions (VLM) | Charts to data | OCR | Right-to-left | CJK; vertical |
|---|---|---|---|---|---|---|---|
| **RayoPDF** | a box a block and page ([formats](../guide/formats.md)) | embedded images as PNG, or the PDF's JPEG and JPEG 2000; vector figures are not drawn ([README](../../README.md)) | no; OCR text of an image as its alt text ([ocr](../guide/ocr.md)) | no | add-on: auto, off, force; Latin, CJK and six more scripts ([ocr](../guide/ocr.md)) | bidi on the text layer; Arabic OCR rough, Hebrew OCR none ([ocr](../guide/ocr.md)) | yes ([formats](../guide/formats.md)); vertical writing mode placed ([interpreter.cpp](../../src/layout/interpreter.cpp)), order of vertical columns not measured |
| docling | block boxes; char, word or line cells ([pipeline options](https://docling-project.github.io/docling/reference/pipeline_options/#docling.datamodel.pipeline_options.NativePdfPipelineOptions)) | page crops or embedded ([CLI](https://docling-project.github.io/docling/reference/cli/#docling-convert)) | opt-in models ([picture description](https://docling-project.github.io/docling/usage/enrichments/#picture-description)) | opt-in model ([pipeline options](https://docling-project.github.io/docling/reference/pipeline_options/)) | on by default; several engines ([OCR](https://docling-project.github.io/docling/concepts/OCR/)) | partial ([CHANGELOG](https://github.com/docling-project/docling/blob/main/CHANGELOG.md)) | yes; not documented ([OCR](https://docling-project.github.io/docling/concepts/OCR/)) |
| marker | block polygons; characters through `OCRConverter` ([OCR only](https://github.com/datalab-to/marker#ocr-only)) | page crops, JPEG ([renderers](https://github.com/datalab-to/marker/blob/master/marker/renderers/__init__.py#L24-L67)) | `--use_llm` ([README](https://github.com/datalab-to/marker#convert-a-single-file)) | not documented | modes no_ocr, fast, balanced, vllm ([usage](https://github.com/datalab-to/marker#usage)) | not documented ([Surya](https://github.com/datalab-to/surya#multilingual)) | yes; not documented ([usage](https://github.com/datalab-to/marker#usage)) |
| MinerU | block boxes only ([medium](https://opendatalab.github.io/MinerU/next/middle-json/current-medium/)) | yes ([outputs](https://opendatalab.github.io/MinerU/reference/output_files/#zip-image-names)) | advanced tier ([tiers guide](https://github.com/opendatalab/MinerU/discussions/5709)) | advanced tier ([tiers guide](https://github.com/opendatalab/MinerU/discussions/5709)) | auto, txt, ocr; four tiers ([parser](https://opendatalab.github.io/MinerU/next/sdk/parser/)) | Arabic recognizer ([2.6.2](https://opendatalab.github.io/MinerU/reference/changelog/#262-20251024)) | yes; partial ([2.1.0](https://opendatalab.github.io/MinerU/reference/changelog/#210-20250705)) |
| PyMuPDF | box, line, span; words in legacy mode ([schema](https://github.com/pymupdf/PyMuPDF/blob/main/docs/json-schema.rst#L1274-L1293)) | region renders at a chosen dpi ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L367-L405)) | not documented | not documented | hybrid auto; Tesseract, RapidOCR ([OCR](https://github.com/pymupdf/PyMuPDF/blob/main/docs/ocr/index.rst)) | bidi level in JSON ([schema](https://github.com/pymupdf/PyMuPDF/blob/main/docs/json-schema.rst#L1397-L1398)) | claimed; `wmode` in JSON ([schema](https://github.com/pymupdf/PyMuPDF/blob/main/docs/json-schema.rst#L1455-L1462)) |
| opendataloader-pdf | element boxes ([schema](https://opendataloader.org/docs/reference/json-schema#common-content-fields)) | PNG or JPEG ([CLI](https://opendataloader.org/docs/reference/cli-options#options)) | hybrid mode ([hybrid](https://opendataloader.org/docs/hybrid-mode#chart-and-image-description)) | no ([hybrid](https://opendataloader.org/docs/hybrid-mode#chart-and-image-description)) | hybrid mode only ([server](https://opendataloader.org/docs/hybrid-mode#server-options)) | no reordering ([scanned](https://opendataloader.org/docs/hybrid-mode#scanned-pdfs-ocr)) | yes; not documented ([FAQ](https://github.com/opendataloader-project/opendataloader-pdf#does-it-work-with-korean-japanese-or-chinese-documents)) |
| kordoc | block boxes; OCR lines and characters ([OCR lines](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#ocr-lines-and-models)) | PNG or WebP; page images ([CLI](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#-cli)) | custom provider only ([OCR](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#ocr-scanned--image-based-pdfs)) | not documented | auto, force; Korean PP-OCRv5 ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#parse-a-document)) | not documented | Korean first; partial |
| edgeparse | element boxes ([matrix](https://github.com/raphaelmansuy/edgeparse#capability-matrix)) | PNG or JPEG ([CLI](https://github.com/raphaelmansuy/edgeparse/blob/main/tutorials/01-cli.md#7-image-extraction)) | no ([matrix](https://github.com/raphaelmansuy/edgeparse#capability-matrix)) | not documented | tables in images; scans through hybrid ([OCR models](https://edgeparse.com/guides/ocr-models/#tiers-and-sizes)) | not documented | partial; not documented |
| liteparse | lines; words in the library; cells ([word boxes](https://developers.llamaindex.ai/liteparse/guides/extraction/#word-boxes)) | embedded bytes; screenshots ([images](https://developers.llamaindex.ai/liteparse/guides/extraction/#embedded-images)) | no ([README](https://github.com/run-llama/liteparse#liteparse)) | no ([README](https://github.com/run-llama/liteparse#without-ml-models)) | auto; Tesseract, ocria-nano, HTTP ([OCR](https://developers.llamaindex.ai/liteparse/guides/ocr/)) | RTL recognizer ([ocria-nano](https://developers.llamaindex.ai/liteparse/guides/ocr/#ocria-nano-liteparses-own-models)) | yes; not documented ([OCR](https://developers.llamaindex.ai/liteparse/guides/ocr/)) |
| unstructured | element boxes ([elements](https://docs.unstructured.io/open-source/concepts/document-elements)) | `hi_res` crops ([partitioning](https://docs.unstructured.io/open-source/core-functionality/partitioning)) | no ([constants.py](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/partition/utils/constants.py#L14-L18)) | not documented | auto, fast, hi_res, ocr_only ([strategies](https://docs.unstructured.io/open-source/concepts/partitioning-strategies)) | not documented | Tesseract packs; not documented |
| pdfplumber | characters, words, lines ([chars](https://github.com/jsvine/pdfplumber#char-properties)) | no ([images](https://github.com/jsvine/pdfplumber#image-properties)) | no | no | none ([comparison](https://github.com/jsvine/pdfplumber#comparison-to-other-libraries)) | `char_dir` ([text](https://github.com/jsvine/pdfplumber#extracting-text)) | not documented; `upright` ([chars](https://github.com/jsvine/pdfplumber#char-properties)) |
| MarkItDown | no | no for PDF ([PDF converter](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/converters/_pdf_converter.py)) | images and PPTX only ([API](https://github.com/microsoft/markitdown#python-api)) | no | plugin calling an LLM ([markitdown-ocr](https://github.com/microsoft/markitdown/blob/main/packages/markitdown-ocr/README.md)) | not documented | not documented |
| Apache Tika | no; hOCR for OCR text ([Tesseract](https://tika.apache.org/docs/4.2.x/configuration/parsers/tesseract-ocr-parser.html)) | inline images, page renders ([pages](https://tika.apache.org/docs/4.2.x/configuration/pages.html)) | VLM transcription, experimental ([VLM](https://tika.apache.org/docs/4.2.x/configuration/parsers/vlm-parsers.html)) | not documented | AUTO by default; Tesseract, VLMs ([pages](https://tika.apache.org/docs/4.2.x/configuration/pages.html)) | not documented | Tesseract models incl. vertical ([Tesseract](https://tika.apache.org/docs/4.2.x/configuration/parsers/tesseract-ocr-parser.html)) |
| LlamaParse | word, line, cell; characters on the top tier ([granular boxes](https://developers.llamaindex.ai/llamaparse/parse/guides/configuring-parse/#granular-bounding-boxes)) | screenshots, embedded, layout crops ([images](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#images)) | `caption` field; generation not documented | yes ([charts](https://developers.llamaindex.ai/llamaparse/parse/guides/configuring-parse/#specialized-chart-parsing)) | automatic; 80+ languages ([OCR](https://developers.llamaindex.ai/llamaparse/parse/features/ocr-and-languages/#options)) | `ar` listed ([OCR](https://developers.llamaindex.ai/llamaparse/parse/features/ocr-and-languages/#options)) | listed; not documented |
| Mistral OCR | block boxes; word confidence ([confidence](https://docs.mistral.ai/studio/document-processing/basic-ocr#confidence-scores)) | figure crops ([API](https://docs.mistral.ai/api/endpoint/ocr)) | box annotations ([annotations](https://docs.mistral.ai/studio/document-processing/annotations)) | claimed ([annotations](https://docs.mistral.ai/studio/document-processing/annotations)) | always (VLM) | Arabic, Hebrew, Persian, claimed ([languages](https://docs.mistral.ai/resources/languages#ocr)) | listed; not documented |
| Azure DI layout | words, lines, paragraphs ([lines and words](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#text-lines-and-words)) | figure crops ([figures](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#figures)) | no ([pricing](https://azure.microsoft.com/en-us/pricing/details/document-intelligence/)) | not documented | always ([languages](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/language-support/ocr?view=doc-intel-4.0.0#layout)) | Arabic, Hebrew, Urdu ([languages](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/language-support/ocr?view=doc-intel-4.0.0#layout)) | yes; not documented |
| AWS Textract | words, lines ([BoundingBox](https://docs.aws.amazon.com/textract/latest/APIReference/API_BoundingBox.html)) | Insights page images ([results](https://docs.aws.amazon.com/textract/latest/dg/insights-extraction-results.html#insights-extraction-results-pages)) | Insights figure captions ([captions](https://docs.aws.amazon.com/textract/latest/dg/insights-summarization-captioning.html)) | not documented | always; 40+ languages ([languages](https://docs.aws.amazon.com/textract/latest/dg/supported-languages.html)) | Arabic, Hebrew ([languages](https://docs.aws.amazon.com/textract/latest/dg/supported-languages.html)) | yes; horizontal only ([limits](https://docs.aws.amazon.com/textract/latest/dg/limits-document.html)) |

### 2.5 Chunking, metadata, delivery and deployment

| Tool | Chunking | Document metadata | Streaming or incremental output | Server | MCP | Bindings | Framework integrations | Deployment |
|---|---|---|---|---|---|---|---|---|
| **RayoPDF** | by headings and bytes; tables split by rows ([formats](../guide/formats.md)) | Info dictionary and page count; YAML front matter ([formats](../guide/formats.md)) | no | `--serve --json` over standard streams; no HTTP ([cli](../guide/cli.md)) | rayopdf-mcp, a separate repository ([agents](../guide/agents.md)) | Python, JS (WebAssembly), C++, C ABI ([README](../../README.md)) | LangChain, LlamaIndex ([python](../guide/python.md)) | 2.1 MB static, CPU, no models; Linux x64 and Windows x64 ([README](../../README.md)) |
| docling | hybrid (tokenizer), hierarchical ([chunking](https://docling-project.github.io/docling/concepts/chunking/)) | "coming soon" ([README](https://github.com/docling-project/docling#coming-soon)) | progress callbacks only ([options](https://docling-project.github.io/docling/usage/advanced_options/#track-conversion-progress)) | docling-serve ([REST](https://docling-project.github.io/docling/usage/api_server/rest_api/)) | docling-mcp ([MCP](https://docling-project.github.io/docling/usage/mcp/)) | Python; TS and Java clients ([docling-java](https://raw.githubusercontent.com/docling-project/docling-java/HEAD/README.md)) | LangChain, LlamaIndex, Haystack and more ([integrations](https://docling-project.github.io/docling/integrations/)) | CPU image 4.4 GB; models ([docling-serve](https://raw.githubusercontent.com/docling-project/docling-serve/HEAD/README.md)) |
| marker | block chunks ([chunks](https://github.com/datalab-to/marker#chunks)) | TOC, page statistics ([metadata](https://github.com/datalab-to/marker#metadata)) | no ([README](https://github.com/datalab-to/marker#convert-a-single-file)) | `marker_server`, small-scale ([server](https://github.com/datalab-to/marker#api-server--viewer)) | not documented | Python | not documented | PyTorch; VLM of about 690 M parameters for some modes ([README](https://github.com/datalab-to/marker#performance)) |
| MinerU | none, by design ([README](https://github.com/opendatalab/MinerU)) | title, author, language ([metadata](https://opendatalab.github.io/MinerU/next/metadata/)) | progressive reading by page or block ([continue](https://github.com/opendatalab/MinerU#continue-reading)) | Tasks API, router ([CLI tools](https://opendatalab.github.io/MinerU/usage/cli_tools/)) | for the hosted API ([MCP](https://raw.githubusercontent.com/opendatalab/MinerU-Ecosystem/main/mcp/README.md)) | Python | LangChain, LlamaIndex, hosted API ([ecosystem](https://raw.githubusercontent.com/opendatalab/MinerU-Ecosystem/main/README.md)) | models 0.8–3 GB ([README](https://github.com/opendatalab/MinerU#model-engines-with-extras)) |
| PyMuPDF | by page; by tokens ([document.rst](https://github.com/pymupdf/PyMuPDF/blob/main/docs/document.rst#L480-L514)) | Info fields ([schema](https://github.com/pymupdf/PyMuPDF/blob/main/docs/json-schema.rst#L970-L978)) | a result a document ([functions](https://github.com/pymupdf/PyMuPDF/blob/main/docs/functions.rst#L83-L121)) | no | no | Python, Pyodide ([Pyodide](https://github.com/pymupdf/PyMuPDF/blob/main/docs/pyodide.rst)) | LangChain, LlamaIndex ([RAG](https://pymupdf.readthedocs.io/en/latest/rag.html#integration-with-langchain)) | wheels 63–69 MB, five platforms ([PyPI](https://pypi.org/project/pymupdf/2.2/#files)) |
| opendataloader-pdf | recipes only ([RAG](https://opendataloader.org/docs/rag-integration#chunking-strategies)) | title, author, dates, pages ([schema](https://opendataloader.org/docs/reference/json-schema#root-node)) | no ([CLI](https://opendataloader.org/docs/reference/cli-options#options)) | hybrid back end only ([server](https://opendataloader.org/docs/hybrid-mode#server-options)) | yes ([MCP](https://github.com/opendataloader-project/opendataloader-pdf/blob/main/python/opendataloader-pdf-mcp/README.md#tools)) | Java; Python, Node wrappers ([quick start](https://github.com/opendataloader-project/opendataloader-pdf#quick-start)) | LangChain ([README](https://github.com/opendataloader-project/opendataloader-pdf#langchain-integration)), Haystack ([converter](https://docs.haystack.deepset.ai/docs/opendataloaderconverter)) | Java 11+, 22.8 MB ([release](https://github.com/opendataloader-project/opendataloader-pdf/releases/tag/v2.5.24)) |
| kordoc | by structure, no tokens ([chunks.ts](https://github.com/chrisryugj/kordoc/blob/main/src/chunks.ts)) | title, author, dates, pages ([types.ts](https://github.com/chrisryugj/kordoc/blob/main/src/types.ts)) | progress callback ([worker](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#bulk-conversion--persistent-parse-worker)) | no | 22 tools, in the MCP Registry ([MCP](https://github.com/chrisryugj/kordoc/blob/main/docs/usage-en.md#-mcp-server)) | JS; Python and Java through a worker ([README](https://github.com/chrisryugj/kordoc/blob/main/README-EN.md#documentation-and-security)) | MCP clients | Node 20+ ([usage](https://github.com/chrisryugj/kordoc/blob/main/docs/usage.md)) |
| edgeparse | no ([FAQ](https://github.com/raphaelmansuy/edgeparse#how-do-i-chunk-pdfs-for-semantic-search)) | title, author, dates, pages ([formats](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/05-output-formats.md)) | progress events in the browser ([WASM](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/09-wasm-sdk.md)) | no | a code pattern only ([skill](https://github.com/raphaelmansuy/edgeparse/blob/main/docs/08-agent-skill.md)) | Rust, Python, Node, WASM ([channels](https://github.com/raphaelmansuy/edgeparse#release-channels)) | snippets only ([README](https://github.com/raphaelmansuy/edgeparse#langchain-integration)) | archives 5.6–6.2 MB; Linux, macOS, Windows; x64 and arm64 ([release](https://github.com/raphaelmansuy/edgeparse/releases/tag/v0.3.2)) |
| liteparse | no ([README](https://github.com/run-llama/liteparse#overview)) | pages, producer; XMP opt-in ([metadata](https://github.com/run-llama/liteparse#document-metadata-content-bounds-and-xfa-packets)) | `parseBatches()` iterator ([Node](https://github.com/run-llama/liteparse/blob/main/packages/node/README.md#bounded-memory-parsing)) | liteparse-server ([server](https://developers.llamaindex.ai/liteparse/guides/server-usage/)) | through LlamaIndex's MCP ([MCP](https://developers.llamaindex.ai/llamaparse/for-agents/mcp/#available-tools)) | Rust, Node, Python, WASM ([install](https://github.com/run-llama/liteparse#installation)) | agent skill ([skill](https://github.com/run-llama/liteparse#agent-skill)) | binaries 8.4–9.7 MB; Linux and macOS, x64 and arm64 ([release](https://github.com/run-llama/liteparse/releases/tag/python-v2.21.2)) |
| unstructured | basic, by title; tokens; overlap ([dispatch.py](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/chunking/dispatch.py#L135-L138)) | per element ([elements](https://docs.unstructured.io/open-source/concepts/document-elements#common-metadata-fields)) | chunk generators ([base.py](https://github.com/Unstructured-IO/unstructured/blob/main/unstructured/chunking/base.py)) | no | hosted API only | Python | LangChain, LlamaIndex ([integrations](https://docs.unstructured.io/open-source/integrations)), Haystack ([converter](https://docs.haystack.deepset.ai/docs/unstructuredfileconverter)) | system packages ([install](https://github.com/Unstructured-IO/unstructured#installing-the-library)) |
| pdfplumber | no | Info dictionary ([PDF](https://github.com/jsvine/pdfplumber#the-pdfplumberpdf-class)) | page by page ([page](https://github.com/jsvine/pdfplumber#the-pdfplumberpage-class)) | no | no | Python | LangChain ([loader](https://reference.langchain.com/python/langchain-community/document_loaders/pdf/PDFPlumberLoader)) | 60 KB wheel ([PyPI](https://pypi.org/project/pdfplumber/0.11.11/#files)) |
| MarkItDown | no | none for PDF ([PDF converter](https://github.com/microsoft/markitdown/blob/main/packages/markitdown/src/markitdown/converters/_pdf_converter.py)) | no | out of scope ([README](https://github.com/microsoft/markitdown#what-to-contribute)) | markitdown-mcp ([MCP](https://github.com/microsoft/markitdown/blob/main/packages/markitdown-mcp/README.md)) | Python | Haystack ([converter](https://docs.haystack.deepset.ai/docs/markitdownconverter)), MCP | 90 KB core wheel ([PyPI](https://pypi.org/project/markitdown/0.1.9/)) |
| Apache Tika | experimental ([inference](https://tika.apache.org/docs/4.2.x/configuration/inference.html)) | title, author, dates, `/Lang`, XMP ([PDFParser.java](https://github.com/apache/tika/blob/main/tika-parsers/tika-parsers-standard/tika-parsers-standard-modules/tika-parser-pdf-module/src/main/java/org/apache/tika/parser/pdf/PDFParser.java#L607-L650)) | SAX events, gRPC ([gRPC](https://tika.apache.org/docs/4.2.x/using-tika/grpc/index.html)) | tika-server ([server](https://tika.apache.org/docs/4.2.x/using-tika/server/index.html)) | no | Java; REST, gRPC | LangChain4j ([parser](https://docs.langchain4j.dev/integrations/document-parsers/apache-tika)), Haystack ([converter](https://docs.haystack.deepset.ai/docs/tikadocumentconverter)) | 53–57 MB, Java 17 ([downloads](https://downloads.apache.org/tika/4.2.0/)) |
| LlamaParse | not in Parse ([Split](https://developers.llamaindex.ai/llamaparse/split/getting_started/)) | confidence, printed page number ([metadata](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#metadata)) | jobs, webhooks ([downloads](https://developers.llamaindex.ai/llamaparse/parse/guides/response-format/#download-urls)) | REST ([API](https://developers.llamaindex.ai/reference/resources/parsing/methods/create)) | hosted ([MCP](https://developers.llamaindex.ai/llamaparse/for-agents/mcp/#available-tools)) | Python, TS, Go, Java | LlamaIndex, LangChain, n8n ([agents](https://developers.llamaindex.ai/for-agents/)) | USD 1.50–37.50 a 1,000 pages ([pricing](https://developers.llamaindex.ai/llamaparse/general/pricing/#parsing)) |
| Mistral OCR | no ([announcement](https://mistral.ai/news/ocr-4/)) | page counts ([API](https://docs.mistral.ai/api/endpoint/ocr)) | no; batch jobs ([batch](https://docs.mistral.ai/inference/batch-processing)) | REST ([API](https://docs.mistral.ai/api/endpoint/ocr)) | no OCR tool ([MCP](https://docs.mistral.ai/resources/mcp#what-the-server-exposes)) | Python, TS ([SDKs](https://docs.mistral.ai/resources/sdks)) | Haystack ([converter](https://docs.haystack.deepset.ai/docs/mistralocrdocumentconverter)) | USD 4 a 1,000 pages ([pricing](https://docs.mistral.ai/inference/pricing)) |
| Azure DI layout | no; sections suggested ([RAG](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/concept/retrieval-augmented-generation?view=doc-intel-4.0.0)) | pages; language add-on ([pages](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#pages)) | polling ([what's new](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/whats-new?view=doc-intel-4.0.0)) | REST | no ([tools](https://learn.microsoft.com/en-us/azure/developer/azure-mcp-server/tools)) | .NET, Java, JS, Python ([layout](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/prebuilt/layout?view=doc-intel-4.0.0#development-options)) | LangChain ([loader](https://docs.langchain.com/oss/python/integrations/document_loaders/azure_document_intelligence)), Haystack | USD 10 a 1,000 pages; container 8 cores, 16 GB ([containers](https://learn.microsoft.com/en-us/azure/ai-services/document-intelligence/containers/install-run?view=doc-intel-4.0.0#recommended-cpu-cores-and-memory)) |
| AWS Textract | Insights output levels ([formats](https://docs.aws.amazon.com/textract/latest/dg/insights-output-formats.html#insights-output-formats-choosing-rag)) | page count ([results](https://docs.aws.amazon.com/textract/latest/dg/insights-extraction-results.html)) | jobs and notifications ([StartDocumentAnalysis](https://docs.aws.amazon.com/textract/latest/APIReference/API_StartDocumentAnalysis.html)) | REST | none dedicated ([awslabs/mcp#4541](https://github.com/awslabs/mcp/issues/4541)) | AWS SDKs ([API](https://docs.aws.amazon.com/textract/latest/APIReference/API_AnalyzeDocument.html)) | LangChain ([loader](https://docs.langchain.com/oss/python/integrations/document_loaders/amazon_textract)), Haystack | USD 1.50–70 a 1,000 pages ([pricing](https://aws.amazon.com/textract/pricing/)) |

## 3. RayoPDF's gaps

Each gap names the tools that have the feature (from §2), the evidence that users want it, and a
rough cost judged from the 1.1.0 code (619071e): **S** a few days in one or two files, **M** one to
three weeks over a module, **L** a new subsystem. Costs are [INFERENCE]; none was prototyped.

### 3.1 How often PDFs carry the features (measured)

Census of the input PDFs, not of any tool's output: a throwaway script over PyMuPDF 2.1 in the
`rayopdf-eval-pymupdf:1` image (`--network none`, read-only inputs), run on 2026-11-26. Counts are
documents with the feature. SafeDocs is the 1,000-document sample (16,252 pages) of the 1.1.0
release check's memory and identity runs
([release check](../benchmarks/release-check-2026-11-26.md)); the others are the development
suites of [evaluation.md](../development/evaluation.md). No holdout suite was read.

| Feature (as PyMuPDF reports it) | SafeDocs, 1,000 docs | odl, 200 | olmOCR-bench, 1,403 | READoc dev, 300 | READoc dev2, 600 |
|---|---:|---:|---:|---:|---:|
| Outline (bookmarks) | 109 | 0 | 0 | 267 | 542 |
| Page labels (`/PageLabels`) | 101 | 0 | 0 | 16 | 15 |
| Catalog `/Lang` | 384 | 0 | 0 | 0 | 1 |
| Tagged (`/StructTreeRoot`) | 361 | 0 | 0 | 0 | 0 |
| XMP metadata | 582 | 1 | 0 | 65 | 118 |
| Internal links (GoTo, named) | 49 | 39 | 531 | 166 | 367 |
| URI links | 303 | 33 | 143 | 250 | 501 |
| Form widgets | 14 | 0 | 6 | 0 | 0 |
| Form fields with a text value | 4 | 0 | 0 | 0 | 0 |
| Markup annotations (not Link, Widget or Popup) | 14 | 0 | 2 | 0 | 0 |
| Embedded files | 0 | 0 | 0 | 0 | 0 |
| Encrypted (all of them needing a password) | 1 | 0 | 0 | 0 | 0 |

The 14 SafeDocs documents with markup annotations hold FreeText (4), Highlight (4), Text (3),
Square (2), Stamp (2), FileAttachment, Redact, StrikeOut and Ink (1 each). The benchmark suites
RayoPDF is tuned on carry almost no forms or annotations, which is why their loss (G1) never showed
in a score.

### 3.2 Demand signals in the tools' issue trackers

GitHub issue titles of eleven repositories, counted on 2026-11-26 with `gh api search/issues`
(issues only, open and closed): docling, marker, MinerU, pymupdf4llm, opendataloader-pdf,
MarkItDown, unstructured, liteparse, edgeparse, LlamaParse (`run-llama/llama_parse` now redirects to
the archived `run-llama/llama_cloud_services`, which was searched) and pdfplumber. One query a
feature and repository, `repo:R is:issue in:title T1 OR T2 …`, plus exact-substring Chinese terms
(GitHub splits CJK text into characters, so those were filtered locally). "Relevant" removes titles
that match a word but not the topic ("picture annotations", Python "type annotations", MinerU's 目录
meaning "directory"), judged from the title by two model passes and a hand check, so read it as
±10–20 %. Title-plus-body counts are an upper bound, inflated by issue templates. edgeparse has 0
title matches on every topic; pymupdf4llm's users probably also file in `pymupdf/PyMuPDF`, which was
not searched.

| Topic | Relevant title matches (raw) | Title or body | Examples | RayoPDF 1.1.0 |
|---|---:|---:|---|---|
| Math, LaTeX | 219 (228) | 565 | [MinerU #2280](https://github.com/opendatalab/MinerU/issues/2280), [markitdown #17](https://github.com/microsoft/markitdown/issues/17) (14 reactions) | has (`--math=latex`) |
| Memory, large PDFs | 144 (176) | 559 | [docling #1283](https://github.com/docling-project/docling/issues/1283) (19 reactions), [marker #583](https://github.com/datalab-to/marker/issues/583) | has (2–15 MiB for most documents) |
| Server, REST API, Docker | 139 (158) | 778 | [marker #70](https://github.com/datalab-to/marker/issues/70), [pymupdf4llm #365](https://github.com/pymupdf/pymupdf4llm/issues/365) | container image; no HTTP (G12) |
| Bounding boxes, coordinates | 128 (129) | 607 | [pymupdf4llm #136](https://github.com/pymupdf/pymupdf4llm/issues/136), [markitdown #1246](https://github.com/microsoft/markitdown/issues/1246), [liteparse #403](https://github.com/run-llama/liteparse/issues/403) | blocks only (G4) |
| Chunking | 117 (132) | 484 | [docling #287](https://github.com/docling-project/docling/issues/287) (29 reactions, 45 comments), [unstructured #1706](https://github.com/Unstructured-IO/unstructured/issues/1706) (overlap) | by headings and bytes (G5) |
| Speed | 117 (121) | 401 | [marker #919](https://github.com/datalab-to/marker/issues/919) (21 reactions), [marker #960](https://github.com/datalab-to/marker/issues/960) (20) | has |
| Metadata | 92 (101) | 547 | [markitdown #1664](https://github.com/microsoft/markitdown/issues/1664) (PDF title, author, date) | has; language not (G6) |
| Hyperlinks | 89 (116) | 405 | [docling #828](https://github.com/docling-project/docling/issues/828), [marker #860](https://github.com/datalab-to/marker/issues/860), [pdfplumber #318](https://github.com/jsvine/pdfplumber/issues/318) (internal links) | URI links; internal links not (G6) |
| Running headers and footers | 68 (69) | 220 | [docling #1272](https://github.com/docling-project/docling/issues/1272), [llama_cloud_services #42](https://github.com/run-llama/llama_cloud_services/issues/42) | has; no type in JSON (G7) |
| macOS, Apple Silicon, arm64 | 66 (87) | 865 | [marker #960](https://github.com/datalab-to/marker/issues/960), [liteparse #311](https://github.com/run-llama/liteparse/issues/311) | no (G3) |
| Vector figures, page images | 62 (73) | 1,601 | [docling #1503](https://github.com/docling-project/docling/issues/1503), [markitdown #2688](https://github.com/microsoft/markitdown/issues/2688) | no (G10) |
| Image descriptions | 53 (55) | 416 | [markitdown #256](https://github.com/microsoft/markitdown/issues/256) (14 reactions), [docling #2493](https://github.com/docling-project/docling/issues/2493) | no (G14) |
| Merged cells, nested tables | 47 (47) | 143 | [pdfplumber #79](https://github.com/jsvine/pdfplumber/issues/79), [MinerU #2014](https://github.com/opendatalab/MinerU/issues/2014) | spans yes |
| Outline, bookmarks, TOC | 44 (58) | 147 | [docling #287](https://github.com/docling-project/docling/issues/287), [liteparse #543](https://github.com/run-llama/liteparse/issues/543), [pdfplumber #1319](https://github.com/jsvine/pdfplumber/issues/1319) | read, not written (G6) |
| HTML, DOCX, text output | 44 (88) | 736 | [docling #300](https://github.com/docling-project/docling/issues/300), [liteparse #255](https://github.com/run-llama/liteparse/issues/255); no title asks for DOCX output | no (G9) |
| Tables across pages | 43 (48) | 91 | [docling #2976](https://github.com/docling-project/docling/issues/2976), [unstructured #2355](https://github.com/Unstructured-IO/unstructured/issues/2355) | has |
| Charts | 33 (34) | 115 | [docling #3771](https://github.com/docling-project/docling/issues/3771) | no (G14) |
| Right-to-left | 30 (30) | 51 | [markitdown #2336](https://github.com/microsoft/markitdown/issues/2336), [llama_cloud_services #522](https://github.com/run-llama/llama_cloud_services/issues/522) (Hebrew) | text layer yes; Hebrew OCR no (G15) |
| MCP | 28 (30) | 60 | [opendataloader-pdf #343](https://github.com/opendataloader-project/opendataloader-pdf/issues/343), [pymupdf4llm #256](https://github.com/pymupdf/pymupdf4llm/issues/256) | has (rayopdf-mcp) |
| Form fields | 18 (22) | 86 | [docling #2899](https://github.com/docling-project/docling/issues/2899) (18 comments), [pdfplumber #120](https://github.com/jsvine/pdfplumber/issues/120), [llama_cloud_services #724](https://github.com/run-llama/llama_cloud_services/issues/724) | no (G1) |
| Language detection | 18 (19) | 93 | [docling #1370](https://github.com/docling-project/docling/issues/1370) | no (G6) |
| Vertical CJK text | 17 (28) | 151 | [docling #4672](https://github.com/docling-project/docling/issues/4672), [MinerU #439](https://github.com/opendatalab/MinerU/issues/439) | not measured |
| Page selection | 17 (19) | 184 | [marker #16](https://github.com/datalab-to/marker/issues/16) | has |
| Footnotes | 17 (17) | 79 | [docling #433](https://github.com/docling-project/docling/issues/433), [docling #3128](https://github.com/docling-project/docling/issues/3128) | has |
| Schema extraction | 16 (28) | 252 | [docling #2216](https://github.com/docling-project/docling/issues/2216) | no (G14) |
| Annotations | 11 (27) | 169 | [liteparse #378](https://github.com/run-llama/liteparse/issues/378) (text only in appearance streams), [llama_cloud_services #459](https://github.com/run-llama/llama_cloud_services/issues/459) | no (G1, G8) |
| Passwords | 11 (14) | 61 | [markitdown #1585](https://github.com/microsoft/markitdown/issues/1585), [liteparse #44](https://github.com/run-llama/liteparse/issues/44) | has |
| Check boxes | 9 (11) | 38 | [MinerU #3127](https://github.com/opendatalab/MinerU/issues/3127) | drawn task boxes yes; form check boxes no (G1) |
| Other-language bindings | 8 (33) | 319 | [docling #2944](https://github.com/docling-project/docling/issues/2944) (Rust), [markitdown #1288](https://github.com/microsoft/markitdown/issues/1288) (Node) | C ABI, Python, JS, C++ |
| Streaming output | 6 (50) | 732 | [marker #885](https://github.com/datalab-to/marker/issues/885), [MinerU #4736](https://github.com/opendatalab/MinerU/issues/4736) | no (G13) |

Three readings. First, the most-asked topics (math, memory, speed, running headers, tables across
pages, footnotes) are ones RayoPDF already serves, so they argue for keeping its lead rather than
for new features. Second, the gaps with the most issues are coordinates (128), page images (62),
platforms (66), outline (44) and other output formats (44). Third, forms and annotations draw few
issues (18 and 11) because the tools that drop them do it silently, as RayoPDF does; the measured
loss in G1 is the stronger evidence. Issues with 20 or more reactions on these topics are three:
[docling #287](https://github.com/docling-project/docling/issues/287) (TOC for chunking, 29),
[marker #919](https://github.com/datalab-to/marker/issues/919) (speed, 21) and
[marker #960](https://github.com/datalab-to/marker/issues/960) (speed on Mac, 20).

### 3.3 The gaps

**G1. Form-field values and text drawn by annotations are lost without a comment.**
- *Who has it:* Apache Tika (`extractAcroFormContent` and `extractAnnotationText`, both on by
  default), PyMuPDF (`form_fields` in JSON), liteparse (values of text, check box, radio and choice
  fields, and XFA packets), docling (opt-in `--extract-form-fields`), pdfplumber (a documented
  snippet). The services read forms from the page image (LlamaParse forms beta, Azure key-value
  pairs, Textract `FORMS`). §2.3.
- *Measured here:* RayoPDF reads `/Annots` for links only (`src/layout/interpreter.cpp:1682-1714`
  keeps `Subtype /Link` with a URI action) and never draws an annotation's appearance stream. With
  the 1.1.0 static binary (`master-619071e47b8f-static`, `--images=none --ocr=off`):
  - SafeDocs 0000854, a one-page notice whose text is held in four form fields ("REQUEST FOR
    PROPOSALS NO. 21-005", "The Guam Economic Development Authority (GEDA), is issuing…"): the
    output is 81 bytes, a signature line; none of the field text and no comment.
  - SafeDocs 0000425: page 1 gets `<!-- rayopdf: page 1 has no extractable text -->` while its two
    fields hold the title text ("НД № 2-020101-090" found 0 times in the output).
  - SafeDocs 0000329 and 0000849: the FreeText notes ("Lot Number 7721621"; a Japanese note of
    several lines) are absent (0 matches each).
  This breaks the README's promise that "text that could not be read is never dropped silently".
- *Evidence of demand:* ParseBench 2 added a forms dimension of 600 pages, `fillable_forms` among
  them, as one of its six equally weighted dimensions
  ([README](https://raw.githubusercontent.com/run-llama/ParseBench/main/README.md)); 18 relevant
  issue titles on form fields and 11 on annotations (§3.2), among them
  [liteparse #378](https://github.com/run-llama/liteparse/issues/378) ("pages whose only text
  lives in annotation appearance streams") and
  [llama_cloud_services #724](https://github.com/run-llama/llama_cloud_services/issues/724) ("Not
  extracting data filled in a PDF form").
- *Cost: M.* (a) Draw the normal appearance (`/AP /N`) of each annotation that is not hidden
  (flags Hidden and NoView; Popup skipped) as a Form XObject placed by the annotation's `/Rect`
  and the form's `/Matrix` (ISO 32000-2 §12.5.5), through the existing Form XObject path of
  `Interpreter::DoXObject` (`src/layout/interpreter.cpp`, around 1053–1110). The text then flows
  through layout like page text. (b) Optionally, a JSON `fields` list (name, value, page, box) from
  `/AcroForm /Fields` with inherited values. Files: `src/layout/interpreter.cpp` and `.h`,
  `src/layout/page.h`, `src/layout/markdown_writer.cpp` (b only), a `tests/test_layout_*.cpp` check,
  the fuzz targets `fuzz_layout_content` and `fuzz_layout_document`, `docs/guide/formats.md`. The
  development suites have almost no annotations (§3.1), so the strict gate should see few changes.

**G2. The packages are not on PyPI or the npm registry.**
- *Who has it:* every Python tool in §1 is on PyPI; edgeparse and liteparse publish npm packages
  ([edgeparse release](https://github.com/raphaelmansuy/edgeparse/releases/tag/v0.3.2),
  [liteparse](https://github.com/run-llama/liteparse#installation)).
- *Measured here:* `https://pypi.org/pypi/rayopdf/json` and `https://registry.npmjs.org/rayopdf`
  both answered 404 on 2026-11-26, while the [README](../../README.md) and the
  [Python guide](../guide/python.md) give `pip install rayopdf` as the install command. The
  [JavaScript guide](../guide/javascript.md) says the npm package is release-only.
- *Evidence of demand:* the LangChain and LlamaIndex loaders (`rayopdf[langchain]`,
  `rayopdf[llama-index]`) are useful only where `pip install` works; every framework integration in
  §2.5 installs from a registry.
- *Cost: S.* No code. CONTRIBUTING.md § Versions and releases puts the two wheels on PyPI by hand
  after a release (`twine upload`) and keeps the npm package on the release only; the 404 says the
  PyPI step has not been done for any version yet. Publishing to npm is a decision to change that
  rule.

**G3. Builds for Linux arm64 and macOS.** Done in 1.2: Linux arm64 and macOS arm64 and x64
archives, libraries and wheels, their output compared with Linux x64's in CI (roadmap D1).
- *Who has it:* edgeparse ships Linux, macOS and Windows on x64 plus Linux and macOS on arm64;
  liteparse Linux and macOS on x64 and arm64; PyMuPDF wheels for macOS arm64, Linux aarch64, Linux
  x86_64 and Windows ([PyPI](https://pypi.org/project/pymupdf/2.2/#files)); the Python and Java
  tools run wherever their runtime does.
- *RayoPDF:* Linux x64 and Windows x64 only ([README](../../README.md)); `RAYOPDF_STATIC` stops
  with "RAYOPDF_STATIC is for Linux." on Apple (`CMakeLists.txt`). The WebAssembly package runs
  anywhere but is single-threaded.
- *Evidence of demand:* 66 relevant issue titles on macOS, Apple Silicon or arm64 in seven
  repositories (§3.2), [marker #960](https://github.com/datalab-to/marker/issues/960) with 20
  reactions among them; most are about running there at all, which a native tool without a build
  for the platform cannot offer.
- *Cost: M for Linux arm64, M to L for macOS.* The SSE2 kernels have scalar fallbacks under
  `__SSE2__` guards, and the WebAssembly build already compiles them: its output matches the
  native program's on all 200 odl pages and on 5,867 of 5,886 test documents
  ([JavaScript guide](../guide/javascript.md)), and the 19 others were not traced
  ([release check](../benchmarks/release-check-2026-11-26.md)). An arm64 build needs the same
  identity check and probably the same differences examined. Files: `CMakeLists.txt`,
  `tools/dev.py` (a cross compiler in `docker/build.Dockerfile`), `tools/package.py`,
  `.github/workflows/release.yml`, `src/common/cpu.cpp` (x86 feature detection). macOS needs the
  Apple SDK, so a macOS runner in CI, which costs Actions minutes (AGENTS.md rule 16).

**G4. Line and word positions in JSON (grounding).**
- *Who has it:* PyMuPDF (box, line and span boxes), pdfplumber (character, word, line), liteparse
  (line boxes; word boxes in the library), docling (character, word or line cells), LlamaParse
  (word, line, cell), Azure (word, line, paragraph), Textract (word, line). Block-level only:
  RayoPDF, marker, MinerU, opendataloader-pdf, edgeparse, unstructured. §2.4.
- *Evidence of demand:* ParseBench scores "visual grounding" ("tracing every extracted element back
  to its source location on the page. Required for auditability in regulated workflows") as one of
  its dimensions, 500 pages in v1 and 535 cases in v2
  ([README](https://raw.githubusercontent.com/run-llama/ParseBench/main/README.md)); 128 relevant
  issue titles on bounding boxes in ten repositories (§3.2), the most of any topic RayoPDF lacks.
- *Cost: S to M.* The JSON writer already walks each block's lines with their boxes
  (`BlockBoxes`, `src/layout/markdown_writer.cpp:2426-2449`; `WriteJson`, 2470–2578), and word boxes
  exist for tables (`src/layout/word_box.h`). An opt-in `--json-detail=lines|words` adding
  `lines: [{page, bbox, text}]` per block keeps schema 1 (a new optional field changes no field's
  meaning). Mapping each line to byte offsets of the block's Markdown is the harder half. Files:
  `src/layout/markdown_writer.cpp` and `.h`, `src/layout/convert.h`, `src/api/flags.cpp`,
  `src/cli/main.cpp`, the Python and npm option lists, `docs/guide/formats.md`.

**G5. Chunks by token budget, with overlap, or by page.**
- *Who has it:* docling (`HybridChunker`, tokenizer-aware, repeats table headers), PyMuPDF
  (`to_chunks`: 400 tokens by default, tiktoken or an estimate; `page_chunks`), unstructured
  (`max_tokens` with tiktoken, `overlap`), Tika (experimental, `overlapChars`). kordoc and RayoPDF
  chunk by structure only; marker gives one chunk a block. §2.5.
- *RayoPDF:* a heading starts a chunk, `--chunk-chars` caps bytes
  ([formats](../guide/formats.md)); no overlap, no page mode.
- *Evidence of demand:* 117 relevant issue titles on chunking (§3.2), among them
  [unstructured #1706](https://github.com/Unstructured-IO/unstructured/issues/1706) asking for
  overlap and [docling #287](https://github.com/docling-project/docling/issues/287), the most-reacted
  issue of the survey, asking for the TOC to guide chunking. Embedding models limit input in tokens,
  not bytes [INFERENCE].
- *Cost: S.* `WriteChunks` (`src/layout/markdown_writer.cpp`) plus options: `--chunk-overlap=N`
  (repeat the last sentences up to N bytes), `--chunks-by=page`. A tokenizer is data-heavy (BPE
  merge tables) and belongs in the Python loaders, which can re-split by a caller's
  `length_function` (`python/rayopdf/integrations/`).

**G6. Structure the PDF holds and RayoPDF reads or could read: outline, page labels, internal
links, language.**
- *Who has it:* outline: PyMuPDF (`toc`), Tika (`extractBookmarksText`); docling, opendataloader
  and edgeparse read it for heading levels only, as RayoPDF does. Printed page numbers: LlamaParse
  (`printed_page_number`), Azure (`PageNumber`). Language: Tika (`/Lang`), MinerU. §2.3, §2.5.
- *Measured here (§3.1):* outlines in 109 of 1,000 SafeDocs documents and 809 of 900 READoc
  development documents; page labels in 101 of 1,000 SafeDocs documents; `/Lang` in 384; internal
  links in 531 of 1,403 olmOCR-bench pages.
- *Evidence of demand:* 44 relevant issue titles on outlines and TOCs, 89 on links (§3.2), among
  them [liteparse #543](https://github.com/run-llama/liteparse/issues/543) ("Expose document outline
  in Python/CLI output"), [pdfplumber #1319](https://github.com/jsvine/pdfplumber/issues/1319)
  ("Bookmarks extraction from PDF") and
  [pdfplumber #318](https://github.com/jsvine/pdfplumber/issues/318) ("Can internal links be
  extracted?").
- *RayoPDF:* `ReadOutline` (`src/layout/interpreter.cpp:1775-1803`) keeps each entry's title and
  level for the heading rules and drops the target; page labels, `/Lang` and GoTo links are not
  read.
- *Cost: S* for a JSON `outline: [{level, title, page}]`, `pages[].label` and `metadata.language`
  (targets need `/Dest`, `/A /D` and the `/Dests` name tree resolved to page numbers); *M* for
  internal links as Markdown anchors, which need a stable id a heading. Files:
  `src/layout/interpreter.cpp` and `.h`, `src/layout/page.h` (`OutlineEntry`),
  `src/layout/metadata.cpp`, `src/layout/markdown_writer.cpp`, `docs/guide/formats.md`.

**G7. More block types in JSON: caption, formula, running header and footer.**
- *Who has it:* PyMuPDF (caption and formula box classes), marker and MinerU (caption, header,
  footer), opendataloader-pdf (caption, header, footer), unstructured (`FigureCaption`, `Header`,
  `Footer`, `PageNumber`, `Formula`), Azure (roles `pageHeader`, `pageFooter`, `pageNumber`,
  `footnote`), Textract (`LAYOUT_*`), Mistral (`caption`, `equation`), LlamaParse (`header`,
  `footer`). §2.2, §2.3.
- *RayoPDF:* `heading`, `paragraph`, `list_item`, `code`, `table`, `image`, `footnote` and
  `comment` (`src/layout/markdown_writer.h:97-98`); furniture kept with `--furniture=keep` and
  captions come out as `paragraph` ([formats](../guide/formats.md)).
- *Evidence of demand:* 68 relevant issue titles on running headers and footers, spread over nine
  repositories (§3.2); most ask to remove them, which RayoPDF does, and a type lets a reader decide
  per use. A retrieval pipeline that indexes captions with their figure needs the type
  [INFERENCE]; RayoPDF's own DocLayNet and OmniParseBench scorers read block types
  ([benchmarks](../benchmarks.md)).
- *Cost: S to M.* The layout already marks furniture lines and the caption rule of
  `--figure-text=drop` (`src/layout/semantics.cpp:1229-1233`) and display formulas. An optional
  `role` field (`caption`, `formula`, `page_header`, `page_footer`, `page_number`) keeps schema 1,
  where a new `type` value would break readers that switch on `type`. Files:
  `src/layout/semantics.cpp` and `.h`, `src/layout/markdown_writer.cpp`.

**G8. Comments, highlights and other markup annotations as content.**
- *Who has it:* Tika (on by default), liteparse (JSON: subtype, contents, author, dates), pdfplumber
  (`.annots`), PyMuPDF (core API, not in its Markdown). ParseBench counts highlight among its
  formatting rules, outside the headline score.
- *Measured here:* 14 of 1,000 SafeDocs documents (§3.1); 0000849 alone holds 766.
- *Cost: S to M.* Generalise `ReadLinks` (`src/layout/interpreter.cpp:1682`) to keep Text,
  FreeText, Highlight, Underline, StrikeOut and Squiggly annotations with `/Contents`, `/T` and the
  covered text; JSON gets `annotations` a page; Markdown could mark highlighted text as `<mark>`.
  Shares its parsing with G1.

**G9. HTML document and plain-text output.**
- *Who has it:* HTML: docling, marker, MinerU, opendataloader-pdf, edgeparse, unstructured, Tika,
  Textract Insights. Plain text: docling, PyMuPDF, opendataloader-pdf, edgeparse, liteparse,
  unstructured, pdfplumber, Tika, LlamaParse, Azure, Textract. §2.1.
- *RayoPDF:* neither; `--format=inspect` draws the layout and `--plain` is Markdown without inline
  markup.
- *Evidence of demand:* 44 relevant issue titles (§3.2), such as
  [docling #300](https://github.com/docling-project/docling/issues/300) and
  [liteparse #255](https://github.com/run-llama/liteparse/issues/255) (HTML export); none asks for
  DOCX output.
- *Cost: S.* A writer over the same `BlockList` (`src/layout/markdown_writer.cpp` already writes
  HTML tables and escapes), `OutputFormat` in `src/layout/convert.h`, the option lists of the CLI,
  C ABI, Python and npm, `docs/guide/formats.md`.

**G10. Figures drawn as vectors become images.**
- *Who has it:* PyMuPDF (page-region renders at a chosen dpi), docling and marker (crops of the
  rendered page), liteparse and Tika (page renders), kordoc (`--page-images`), LlamaParse, Azure and
  Mistral (figure crops). §2.4.
- *RayoPDF:* "a figure drawn as vector graphics does not become an image" ([README](../../README.md)
  § Limitations).
- *Evidence of demand:* 62 relevant issue titles on vector figures, page images and screenshots
  (§3.2), such as [docling #1503](https://github.com/docling-project/docling/issues/1503) and
  [markitdown #2688](https://github.com/microsoft/markitdown/issues/2688).
- *Cost: L.* `src/raster/raster.h` is an 8-bit gray anti-aliased filler, and
  `src/layout/ocr_paint.h` paints paths, image masks and images for OCR but not text, shadings,
  clipping or transparency. A grey crop of a figure region is that painter plus glyph outlines
  (`src/text/outline.h`) and clipping; colour figures need a colour pipeline. The PNG encoder exists
  (`src/image/png.h`). It also costs speed only when images are written.

**G11. Small integration gaps: Haystack, standard input.**
- Haystack lists converters for docling, opendataloader-pdf, MarkItDown, Kreuzberg, Tika,
  unstructured, Mistral, Azure and Textract, and none for RayoPDF
  ([converters](https://docs.haystack.deepset.ai/docs/converters)). *Cost: S:*
  `python/rayopdf/integrations/haystack.py` beside the LangChain and LlamaIndex loaders.
- `rayopdf --convert - out.md` does not read standard input; MarkItDown, Tika and liteparse do.
  *Cost: S:* `src/cli/main.cpp`, with `File::ReadStandardInput` already in
  `src/common/file.h:155`.

**G12. An HTTP server.** docling-serve, liteparse-server, tika-server and MinerU's Tasks API serve
HTTP (§2.5); RayoPDF serves JSON lines over standard streams. *Cost: M*, and a listening socket is a
new attack surface for a tool that parses hostile input; a thin wrapper in the container image or
in rayopdf-mcp is cheaper [INFERENCE].

**G13. Page-by-page output.** liteparse (`parseBatches()`), MinerU (progressive reading) and Tika
(SAX events) hand out results before the document ends. *Cost: L* for RayoPDF: furniture removal,
paragraphs across pages, footnotes at the end and tables merged across pages all look at later
pages (`src/layout/pipeline.cpp`).

**G14. Model-backed features: image descriptions, chart data, schema extraction.** docling,
marker, MinerU, opendataloader hybrid and the services offer them, each through a VLM or an LLM
(§2.4). They conflict with "no models to download" ([README](../../README.md)) and roadmap M9's
size limit; a hook in the Python loaders that hands image files to a caller's model is the only
cheap form [INFERENCE].

**G15. OCR scripts.** Hebrew is not read and Arabic is rough ([OCR guide](../guide/ocr.md)); Mistral
(claimed), Azure, Textract and liteparse's ocria-nano list right-to-left scripts. Open under roadmap
M8 ([roadmap](../development/roadmap.md)); its cost is a model question for the add-on and is not
judged here.

**G16. Other input formats** (DOCX, PPTX, HTML, images): docling, MinerU, MarkItDown, Tika and
the services take many. Outside a PDF converter's scope, except images, which the OCR add-on could
read directly [INFERENCE].

## 4. Top ten for 1.2

Ranked by what a user loses today, then by the evidence of demand, then by cost. The ranking is my
judgement [INFERENCE]; the evidence is in §3.

| # | Gap | Cost | Why this rank |
|---|---|---|---|
| 1 | G1: draw annotation appearances, so form-field values and FreeText notes reach the output; optionally a JSON `fields` list | M | Text is lost without the comment RayoPDF promises; measured on four SafeDocs documents (one keeps only a signature line); ParseBench 2 now scores forms; Tika, PyMuPDF, liteparse and docling read fields. |
| 2 | G2: publish the wheels to PyPI and the package to npm | S | The README's install command fails (PyPI 404 on 2026-11-26); the LangChain and LlamaIndex loaders are reachable only through `pip`. No code. |
| 3 | G4: line (then word) boxes in JSON, opt-in | S–M | The most-asked topic RayoPDF lacks (128 issue titles); a ParseBench dimension; the lines and their boxes already exist in the writer. |
| 4 | G6: outline with target pages, page labels, `/Lang` and internal-link targets in JSON | S | The data is read or one lookup away (`ReadOutline`); outlines sit in 10.9 % of SafeDocs documents and 90 % of READoc's; the survey's most-reacted issue asks for the TOC. |
| 5 | G5: chunk overlap, page chunks, and token-aware re-splitting in the Python loaders | S | 117 issue titles; docling, PyMuPDF and unstructured offer it; one writer function and the loaders. |
| 6 | G3: Linux arm64, then macOS | M (arm64), M–L (macOS) | 66 issue titles; edgeparse and liteparse ship four or five platforms; needs the scalar paths proven byte-identical and CI time for macOS. |
| 7 | G7: a `role` field for captions, formulas and kept running headers, footers and page numbers | S–M | Running headers are the most evenly spread request (nine repositories); most tools type them; the layout already knows them. |
| 8 | G9: `--format=html` and `--format=text` | S | 44 issue titles; eleven of the sixteen tools offer plain text and eight HTML; a writer over existing blocks. |
| 9 | G10: figures drawn as vectors written as images, grey first | L | 62 issue titles and the README's first limitation; docling, marker, PyMuPDF, liteparse, Tika and the services crop or render figures or pages; the OCR painter is a start but lacks text, clipping and colour. |
| 10 | G11: a Haystack converter and `--convert -` from standard input | S | Haystack lists converters for eight of the tools surveyed and none for RayoPDF; standard input saves a temporary file in pipelines. |

Left out of the ten: G8 (annotation contents as data; 11 issue titles, best done right after G1,
whose parsing it shares), G12 (HTTP server; most of its 139 issue titles are about Docker images
and deployment, and RayoPDF has an image), G13 (page-by-page output; 6 relevant titles, high
cost), G14 (model-backed descriptions, charts and schemas; against the no-model design), G15 (OCR
scripts; a model question for the add-on) and G16 (other input formats).

## 5. What RayoPDF has that the others lack

Shared with some tools is said so. "Not documented" for the others means their sources read in §1
do not mention it.

- **Speed and size without a runtime.** About 1 ms a one-page PDF as a fresh process and 2,228
  pages a second warm, from a 2.1 MB static executable with no runtime or model files
  ([README](../../README.md), [roadmap Status](../development/roadmap.md)). The nearest measured
  tools, liteparse and edgeparse, took 27 and 45 ms a page fresh (README, measured 2026-11-09).
- **Formulas rebuilt as LaTeX from the glyphs, with no model.** Every other tool that writes
  LaTeX uses a model or a service (docling, marker, MinerU, kordoc opt-in, opendataloader hybrid,
  Azure add-on, Mistral); the text-layer tools write none. RayoPDF passes 0.860 of olmOCR-bench's
  arXiv math tests ([benchmarks](../benchmarks.md)).
- **Footnotes as linked GFM footnotes** (`[^n]` defined after the referencing block). marker writes
  superscripts, PyMuPDF block quotes; the others keep footnotes as blocks or roles.
- **Tables merged across pages** into one table without the repeated header rows, in a text-layer
  tool. opendataloader-pdf and edgeparse only link the parts; MinerU and LlamaParse merge with
  models.
- **Uncertainty marked where it occurs.** `<!-- rayopdf: page N has no extractable text -->` and
  related comments sit at the place in the Markdown, and `--report` counts unmapped glyphs, repairs
  and limits ([formats](../guide/formats.md)). Others give per-page or per-document signals
  (docling confidence grades, kordoc `needsOcr`, liteparse `is-complex`, LlamaParse and Mistral
  confidence); none documents a marker in the output itself. G1 is the exception to this promise.
- **The tagged structure tree checked before it is trusted** (`structure` in the report: `used`,
  or the reason it was not: `broken`, `role-cycle`, `coverage`, `generic`, `order`). Tika,
  opendataloader-pdf (`--use-struct-tree`), edgeparse and MinerU's flash tier read tags without a
  documented check.
- **Deterministic, platform-independent bytes.** The same file and options give the same bytes on
  Linux and Windows and for any `--workers` ([cli](../guide/cli.md)). edgeparse claims the same
  output every run and LlamaParse pins its tiers by date; the others do not document it.
- **Options for a reader's conventions** (`--plain`, `--tables=html|pipe|pipe-latex`,
  `--furniture`, `--quotes`, `--small-caps`, `--figure-text`), which match each benchmark's ground
  truth ([cli](../guide/cli.md)).
- **Exact recovery of PDFs made by RayoMD** (`rayomd-source/1`), unique to the pair.
- **One core behind every binding**: the CLI, C++ library, C ABI, Python wheel and WebAssembly
  package give the same output (the WebAssembly build on all but 19 of 5,886 test documents,
  [JavaScript guide](../guide/javascript.md)). edgeparse and liteparse also ship Rust, Python, Node
  and WASM from one core; of the Python and Java tools only PyMuPDF has a browser build (Pyodide,
  experimental).
- **Fuzzing as a release bar**: 25 libFuzzer targets, 25 h in the 1.1.0 release check
  ([roadmap Status](../development/roadmap.md)); not documented by the others.

## 6. What this note did not verify

- No third-party tool was run for this note; §2 is what their sources say. The four services are
  claims throughout.
- The census (§3.1) counts what PyMuPDF 2.1 reports, used as a black box in its evaluation image;
  "form fields with a text value" counts non-button fields whose value is neither empty nor `Off`.
- The losses of G1 were checked on four SafeDocs documents, by searching the output for field and
  note text; how many more documents lose text this way was not measured.
- RayoPDF's reading order on vertical CJK pages was not measured.
- Issue counts (§3.2) match titles only, include closed issues and noise, grow with a repository's
  age and popularity, and were not normalised; they show which topics recur, not how many users
  want them.
- From the surveys: how marker treats form fields after flattening, how marker reads PPTX, XLSX,
  HTML and EPUB, whether MinerU's images are page crops or embedded images, and the release date of
  docling-mcp 3.5.1 were not verified.
