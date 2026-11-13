# Small machine learning for PDF to Markdown: evidence, landscape and fit for RayoPDF

Research notes of 2026-11-13 for the 0.3.0 program (roadmap M9, small models in the core). Part 1 is the literature's answer on where small learned models beat rules when they read the text layer; part 2 the landscape of small models, runtimes and training data with their licences, and their fit for RayoPDF in two tiers (A: compiled into the core; B: in the OCR add-on); part 3 the literature's evidence behind the answer, its disagreements, coverage and references. Third-party numbers are as published; **[INFERENCE]** (or [inference]) marks the author's reasoning, **unverified** what no primary source confirmed.

## Part 1. Answer: learned structure from the text layer against rules

Date of search: 2026-11-13. Scope: methods that work from glyphs, words, lines, fonts and positions rather than page images. Image-only methods appear only where a paper compares them with a text-layer method.

### 1. Answer

The conclusions here are my **inference** from the evidence in §2; the numbers are from the cited papers.

| Topic | Evidence that a small CPU model beats good rules | Expected gain [inference] | Cheapest feature set (from the papers) | Evidence against learned models |
|---|---|---|---|---|
| **c. Table structure from word positions** | **Strongest of the six.** Small MLPs on token-pair features beat the ICDAR 2013 winner Nurminen (rules) in adjacency F1, 0.972 vs 0.946, and the rule tool of Shigarov et al. (0.936) [Aoyagi 2022]. On the same features, a CRF for table-line labelling beats the authors' rules, 96.4 vs 91.9 F [Liu 2008]. A small graph attention model (d=64, 4 blocks) beats rule-based Tabby on spanning cells, 0.955 vs 0.882 micro F1 [Chi 2019]. | +2–5 adjacency-F points on ICDAR-2013-style government tables. Larger on tables with spanning cells. A TEDS gain is likely but untested: TEDS was not reported on the same split. | Gap between token pairs, same font and size, numeric or not, merge position, number of aligned tokens in the row and column, ruling lines between tokens, implicit-ruled-line point clusters [Aoyagi 2022]. Line sparsity: largest gap, number of text pieces, character density [Liu 2008]. | A text-only GNN is weak at finding where tables are: GLAM's table mAP is 50.2 without image features [Wang 2023]. Detection needs ruling lines or graphics, which RayoPDF has. A 2025 Japanese follow-up gets TEDS 0.780 on FinTabNet and reports dropped or wrong tokens from pdfalto [Tagami 2025]. |
| **e. Text-cell / line classification** | **Strong** for some classes. A 480K-parameter, 5.8 MB BiLSTM with geometric and character features (no text content) gets weighted F1 0.97 on 17 labels and replaced random forests in IBM CCS [Livathinos 2021]. A 1M-parameter text-only GNN beats a 140M image detector on page headers and footers [Wang 2023]. Layout features lift CRF line labelling by 9 macro-F points [Luong 2010]. | Largest per-class gains in the literature: footnote +28 F1, sub-sub-section heading +40, caption +15, equation +13, list item +9 [Luong 2010]. DocBank shows the same classes gain most from layout: footer +11, list +18, caption +10 [Li 2020]. For RayoPDF: probably +1–4 overall points where these classes count (headings, captions, list items). | Relative font-size rank, bold/italic, relative y-band, first-token patterns (numbering, "Figure/Table", bullet glyph), gap to previous and next line, same format as previous line, alignment [Luong 2010; Livathinos 2021; Josi 2022]. | Text-only cells are weak on list, caption, footnote and formula unless visual cues (bullets, rules, backgrounds) are added: GLAM without its CNN drops 15 mAP on list items and 23 on formulas [Wang 2023]. Random forests trained per layout collapse on diverse layouts, e.g. title F1 0.93 to 0.71 [Livathinos 2021]. |
| **b. Headings and levels** | **Moderate.** Learned classifiers (MaxEnt, extra-trees, LSTM; weighted F1 0.968–0.982) beat the only rule system in FinTOC-2019 title detection (0.949) [Juge 2019]. Gradient-boosted trees on 28 hand-crafted features detect titles at 98.4% F1 on prospectuses [Bentabet 2019]. | Detection: +1–3 F1 over good rules [inference from FinTOC]. **Level assignment**: pointwise trees are weak (GBT 64–74% F1). A sequence model over the document's headings gets 85–93% [Bentabet 2019]. So use a tiny sequence model (linear-chain CRF) or keep rules for levels. | is_bold, is_all_caps, font size relative to body, numbering prefix, indent and size difference to previous and next block, text length, contains a verb [Bentabet 2019]. Relative font rank plus multi-level numbering [Luong 2010]. | No paper compares a learned level model with font-size clustering plus numbering rules. With little data, trees beat the neural model [Bentabet 2019]. Adding keyword features ("Annex", "Appendix") hurt [Juge 2019]. |
| **f. Display and inline math** | **Moderate for display math.** On 200 textbook pages (English and Chinese), an SVM on 9 glyph and layout features beats the rule scorer, isolated-formula F1 95.6 vs 90.6; the hybrid reaches 96.1 [Lin 2011]. Inline: a CRF on font, font-ID and symbol features gets macro F1 80–83 vs 24 for InftyReader OCR [Madisetty 2021, citing Iwatsuki 2017]. | Display formula lines: about +5 F1 over well-tuned rules, in-domain. Inline math: span starts (B-Math F1 0.73–0.76) remain the weak point. Gains are plausible, but no strong rule baseline on born-digital PDFs exists. | Italic, math-symbol classes (8), named functions, font differs from body font, token font equals a display-formula font, single-letter token, line height and spacing, centring, equation number at line end [Lin 2011; Madisetty 2021]. | No study beats a strong born-digital inline-math rule system: the only inline baseline is OCR-based. Lin 2011 kept embedded-formula detection rule-based (F1 83.6) and did not test a learned variant. |
| **a. Page furniture** | **Weak for "beats good rules".** It is strong that text-layer models do this well: GLAM's GNN-only model gets page-footer mAP 87.3 and page-header 78.1, vs 61.1 and 67.9 for YOLOv5x6 [Wang 2023]. But no paper compares a learned model with good cross-page rules. | Small [inference]. Cross-page rules already reach P 98.0 / R 92.7 [Lin 2002]. Where they fail (headers that occur on only 1–2 pages, as in short articles) a per-page classifier could help. On odl the effect could even be negative, because odl ground truth keeps headers and footers. | Position band (top 5 / bottom 3 lines), weighted similarity to the same-rank line on ±8 neighbouring pages after replacing digits with '@', geometry similarity [Lin 2002]. Per-element: y-position, size, digit presence, font [Josi 2022]. | Furniture appears only on pages, so cross-page repetition is the stable signal [Lin 2002]. Every Chinese source found (journal paper and patents) uses rules; no learned text-layer method was found. |
| **d. Reading order** | **Evidence mostly against large learned models.** On OmniDocBench, LayoutReader (LayoutLMv3, fine-tuned on 500K pages) gets BLEU-4 0.595 on three-column pages, below plain XY-Cut (0.702) and far below rule-based XY-Cut++ (0.967) [Liu 2025]. A *small* hybrid works: a 267K-parameter GCN predicts only "column-wise vs row-wise" plus paragraph links, rules do the sort, and normalized Levenshtein falls from 0.146 to 0.098 vs a topological-sort baseline [Wang R. 2023]. | Only in hybrid form: a tiny classifier for local pattern (table or row-wise vs column-wise) or pairwise order for ambiguous blocks (sidebars, captions). Gain unknown on born-digital multi-column PDFs. | Box corner coordinates over a β-skeleton graph; image features add only about 1.7 F1 [Wang R. 2023]. Pairwise geometric features fed to an MLP [Quirós 2022]. | XY-Cut++ (rules plus coarse labels) beats LayoutReader and MinerU on DocBench-100, 0.988 vs 0.788 and 0.873 BLEU-4, at 514 vs 22 FPS on CPU [Liu 2025]. Caveat: the authors built that benchmark (preprint). |

**Overall [inference]:**

1. The clearest case for a small learned model in RayoPDF is **table structure** (merging tokens into cells and placing implicit separators), then **line and cell role classification** for footnotes, captions, list items and sub-headings.
2. Both work with GBDT- or MLP-sized models (hundreds of KB to a few MB) on features RayoPDF already computes.
3. For **reading order** and **page furniture**, rules are competitive or better; learning helps only as a local tie-breaker.
4. For **heading levels**, a document-level sequence step matters more than the model class.

Confidence: moderate. Most comparisons are in-domain (train and test from the same collections) and use different metrics from RayoPDF's suites (odl NID/TEDS/MHS, olmOCR unit tests).

## Part 2. Landscape: small models, runtimes and training data, and their fit

**Conventions.** Third-party numbers are as published. **[INFERENCE]** means my own reasoning, not stated by any source. **unverified** means I could not confirm it from a primary source. "Excluded" means the item cannot ship or train a shipped model under the permissive-only rule (MIT, BSD, Apache-2.0, zlib, CDLA-Permissive).

**RayoPDF facts used below:**
- Static `rayopdf` is 3.4 MB (`build/static/`).
- The OCR add-on executable is 7,314,160 B stripped (Linux).
- PP-OCRv6 tiny models are 6,185,082 B; small models are 30,987,999 B.
- The add-on takes 1,905 ms/page median on one thread (small tier with a tiny first pass).
- Source: `docs/development/ocr-engine.md` §"Sizes" and §"Time per page".

### 1. Text-layer (non-vision) learned models on PDF tokens and lines

| name | predicts | input | params / file size | CPU latency (conditions) | published accuracy (dataset, metric) | code licence | weights licence | training-data licence | URL | fit |
|---|---|---|---|---|---|---|---|---|---|---|
| HURIDOCS "fast" LightGBM, model 1: token type | 11 DocLayNet classes per Poppler token (Caption, Footnote, Formula, List item, Page footer, Page header, Picture, Section header, Table, Text, Title) | PDF tokens from Poppler `pdftohtml` XML. Features per adjacent token pair: same font, page font-size mode, lengths, space and punctuation counts, 22 geometry features, Unicode categories of the first and last 2 characters. A ±4-token context window gives 8 pairs (`TokenFeatures.py`, `ModelConfiguration.py` context_size=4) | `pdf_tokens_type.model` 102,325,660 B. GBDT multiclass, 700 rounds × 11 classes, 127 leaves | ~0.42 s/page for the whole fast pipeline (i7-8700, 15-page academic docs, README "Speed") | **None published for the LightGBM models.** The README only says "slightly worse" than VGT | Apache-2.0 | **openrail** (HF `HURIDOCS/pdf-segmentation` card) | DocLayNet (CDLA-Permissive-1.0) | [repo](https://github.com/huridocs/pdf-document-layout-analysis), [features](https://github.com/huridocs/pdf-document-layout-analysis/blob/main/src/adapters/ml/pdf_tokens_type_trainer/TokenFeatures.py), [weights](https://huggingface.co/HURIDOCS/pdf-segmentation) | **Weights excluded** (OpenRAIL use restrictions); 100× over the 1 MB budget anyway. The feature design can be reused as an idea |
| HURIDOCS "fast" LightGBM, model 2: segmentation | Joins token *i* to token *i+1*, giving paragraph boundaries | The same pair features plus one-hot token types of both tokens (`ParagraphExtractorTrainer.py`) | `paragraph_extraction_model.model` 17,122,514 B | Included in the 0.42 s/page above | None published | Apache-2.0 | openrail | DocLayNet | [trainer](https://github.com/huridocs/pdf-document-layout-analysis/blob/main/src/adapters/ml/fast_trainer/ParagraphExtractorTrainer.py) | Excluded (weights). Shows the useful design: a pairwise "same segment" edge classifier |
| GLAM (Kensho, ICDAR 2023) | Node class (11 DocLayNet classes) plus edge positive/negative; segments are the connected components | PDF text boxes with 79 features (bbox, text length, numeric share, font type and size). Edges to the nearest neighbour up/down/left/right plus heuristic reading-order edges. Full GLAM adds ResNet-18 ROI features | GLAM 4M params; **GLAM-CV (no vision) 1M** | GLAM-CV 9 ms CPU, GLAM 16 ms CPU (G4 AWS; CPU type not given) | DocLayNet test mAP@[.5:.95]: GLAM 68.6; GLAM+YOLOv5x6 80.8. GLAM-CV 59.9 overall but **page footer 87.3, page header 78.1** (YOLOv5x6: 61.1 / 67.9). Picture 4.3, table 50.2 | No official code found (unverified) | n/a | DocLayNet (CDLA-P-1.0) | [paper](https://arxiv.org/abs/2308.02051) | **Best evidence that text-box-only models beat vision models on page furniture and headings.** Architecture to copy at a much smaller size |
| PyMuPDF Layout (`pymupdf_layout`, now in pymupdf 2.x) | Layout classes plus table grid | Box features from MuPDF; GNN (`BoxRFDGNN.py`) plus ONNX table-grid models (`table_grid_model_v1…v4.onnx`); optional image features | ONNX in repo (sizes not read) | "10× faster" (vendor claim, unverified) | None published | **AGPL-3.0** | AGPL-3.0 | unverified | [repo](https://github.com/ArtifexSoftware/pymupdf_layout) | **Excluded (AGPL).** Confirms that a text-layer GNN plus table-grid models is a shippable product shape |
| Datalab Indra `order.onnx` ("OrderNet", used by Marker) | Pairwise "line *i* before line *j*" logits, decoded by insertion search into reading order (`decode_order`) | **Line boxes plus text, not pixels**: 16 features per line (10 geometry, 6 text: log length, digit share, bullet/number start, trailing hyphen, sentence end, initial capital) (`order_post.py` `line_features`) | 2,973,138 B | Part of Indra's 212 ms/page median (4 vCPU); not timed separately | No standalone number. Marker now defaults to it instead of the PDF stream order ("accurately across multi-column and out-of-order PDF streams", `line.py`) | Apache-2.0 | **Modified OpenRAIL-M** (free below $20M funding or revenue; may not compete with Datalab's API) | unverified | [order_post.py](https://github.com/datalab-to/indra/blob/main/indra_ocr/order_post.py), [weights](https://huggingface.co/datalab-to/indra-ocr) | **Excluded (weights).** A 16-feature pairwise model is cheap enough for RayoPDF to retrain on its own data |
| Indra/Marker layout plus `det.onnx` | Block labels (Text, Section-Header, Page-Header, Page-Footer, List-Group, Table, Caption, Footnote, Table-Of-Contents, Form) and line boxes | Rendered page (vision, 200 dpi default) | det.onnx 389,948 B; `model.safetensors` 5,013,304 B (role unverified); Marker cites "~18 MB Indra ONNX" | Marker `no_ocr` 57.0 pg/s on one CPU host (cores not stated) | OmniParseBench layout family 92.2 (Indra), 77.8 (Marker no_ocr) | Apache-2.0 | Modified OpenRAIL-M | unverified | [Indra README](https://github.com/datalab-to/indra), [Marker README](https://github.com/datalab-to/marker) | Excluded (weights) |
| opendataloader hybrid triage | Routes a page to Java rules or to docling models (heron plus TableFormer) | Rules on PDF features (table borders, vector grids, text-table patterns, large images, line-chunk ratio) | n/a (rules, not ML) | ~0.05 s rule pages; 0.463 s/page hybrid | odl-bench: rules 0.902 / 0.489 / 0.739 (NID / TEDS / MHS) → hybrid 0.934 / 0.928 / 0.821 | Apache-2.0 (unverified here) | docling Apache-2.0 / CDLA-P-2.0 | — | [discussion #180](https://github.com/opendataloader-project/opendataloader-pdf/discussions/180), [odl-bench](https://github.com/opendataloader-project/opendataloader-bench) (numbers via `docs/research/ocr-ml.md` §3) | Pattern only: triage plus ML on a few pages; ML helped tables (+0.44 TEDS) far more than order (+0.03) |
| LayoutLM v1 on DocBank (token classification) | 12 DocBank classes per token | PDF tokens, 2-D positions, text | base ~113M params (unverified) | not published | DocBank test macro F1: layoutlm-base 0.9316, BERT-base 0.8770; X101+layoutlm 0.9478 | MIT (unilm) | **MIT** (HF `microsoft/layoutlm-base-uncased`) | DocBank Apache-2.0 | [DocBank](https://github.com/doc-analysis/DocBank), [weights](https://huggingface.co/microsoft/layoutlm-base-uncased) | Permissive but ~100× too large for the core. Its token-level F1 shows what text-layer labels can reach |
| VILA (AllenAI) | Token classes using visual layout groups (lines/blocks) | PDF tokens plus groups | transformer (size unverified) | unverified | unverified | Apache-2.0 | unverified | unverified | [repo](https://github.com/allenai/vila) | Too large; idea only |
| VGT (Alibaba; served by HURIDOCS) | 11 DocLayNet classes | Image plus a 2-D **text grid** of PDF tokens | large (unverified) | ~13.5 s/page CPU (i7-8700), 1.75 s GTX 1070 (HURIDOCS README) | PubLayNet 96.2 mAP (ICCV paper); HURIDOCS PubLayNet F1 0.962 | Apache-2.0 (HURIDOCS repo) | HURIDOCS card apache-2.0 | DocLayNet | [paper](https://openaccess.thecvf.com/content/ICCV2023/html/Da_Vision_Grid_Transformer_for_Document_Layout_Analysis_ICCV_2023_paper.html) | Out (speed) |
| DocLayNet text-cell baselines | — | — | — | — | DocLayNet itself publishes only vision baselines (MRCNN, FRCNN, YOLOv5x6; the table in the GLAM paper) | — | — | — | [GLAM Table 1](https://arxiv.org/abs/2308.02051) | GLAM is the reference text-cell baseline |

### 2. Small vision layout detectors usable on CPU

| name | predicts | input | params / size | CPU latency (conditions) | published accuracy | code | weights | training data | URL | fit |
|---|---|---|---|---|---|---|---|---|---|---|
| PP-DocLayout-S (PicoDet-S) | 23 classes (Paddle set, including formula, header, footer, table) | image | 4.834 MB | 18.53 ms normal / 6.29 ms high-performance (Xeon Gold 6271C; 8 threads per ocr-ml.md) | mAP@0.5 70.9 on Paddle's own set | Apache-2.0 | Apache-2.0 | self-built, not released | [doc](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/layout_detection.en.md) | **Tier B candidate** |
| PP-DocLayout-M (PicoDet-L) | same | image | 22.578 MB | 43.39 / 24.44 ms | 75.2 | Apache-2.0 | Apache-2.0 | self-built | same | Just over 20 MB in fp32; int8 might fit **[INFERENCE]** |
| PP-DocLayout-L / plus-L | 23 / plus classes | image | 123.8 / 126 MB | 503 / 634.62 ms | 90.4 / 83.2 | Apache-2.0 | Apache-2.0 | self-built | same | Out (size, speed) |
| PicoDet_layout_1x_table | table regions | image | 7.4 MB | 27.66 / 16.75 ms | mAP 97.5 (own set) | Apache-2.0 | Apache-2.0 | self-built | same | **Tier B** table finder for routed pages |
| PicoDet-S_layout_3cls / 17cls | 3 classes (table, image, stamp) / 17 classes | image | not read | not read | 88.2 / 87.4 | Apache-2.0 | Apache-2.0 | self-built | same | Alternative |
| PP-DocLayoutV3 (RT-DocLayout, ECCV 2026) | multi-point boxes plus reading order | image | 130,806,572 B `inference.pdiparams` | not given | not read | Apache-2.0 | Apache-2.0 | unverified | [HF](https://huggingface.co/PaddlePaddle/PP-DocLayoutV3) | Out (size) |
| docling egret-m (D-FINE-m) | 17 canonical classes | image, then snapped to PDF cells | 19.5M params (~78 MB fp32 **[INFERENCE]**) | 0.334 s/image mean (AMD EPYC 7763, 4 threads, batch 32) | DocLayNet mAP 0.686 raw; canonical DocLayNet 0.765 | MIT (docling) | Apache-2.0 | 150k documents including proprietary | [paper](https://arxiv.org/abs/2509.11720) | Out (3× over 100 ms) |
| docling egret-l / egret-x | same | image | 31.2M / 62.7M | 0.472 / 0.808 s | canonical 0.747 / 0.753 | MIT | Apache-2.0 | same | same | Out |
| docling heron / heron-101 (RT-DETRv2 r50 / r101) | same | image | 42.9M / 76.7M | 0.643 / 0.988 s | canonical 0.776 / 0.780 | MIT | Apache-2.0 | same | [heron](https://huggingface.co/docling-project/docling-layout-heron) | Out; reference ceiling |
| unstructured `yolo_x_layout` | layout classes (`label_map.json`, not read) | image | yolox_tiny.onnx 20,194,877 B; yolox_l0.05_quantized 54,599,886 B; yolox_l 216,625,723 B | not published | no model card (README 404) | Apache-2.0 (unstructured, unverified) | Apache-2.0 (repo `LICENSE.txt`) | unverified | [HF](https://huggingface.co/unstructuredio/yolo_x_layout) | tiny is at the 20 MB limit with no accuracy data; low priority |
| DocLayout-YOLO | layout | image | — | — | DocLayNet mAP 79.7 (via ocr-ml.md) | **AGPL-3.0** | AGPL | — | [repo](https://github.com/opendatalab/DocLayout-YOLO) | **Excluded (AGPL)** |
| Ultralytics YOLOv8/v10/v11 | detection | image | — | — | — | **AGPL-3.0** or paid | same | — | [licence](https://www.ultralytics.com/license) | **Excluded** |
| PDF-Extract-Kit layout and MFD | layout, formula detection | image | — | — | — | **AGPL-3.0** ("uses YOLO code and PyMuPDF") | AGPL | — | [repo](https://github.com/opendatalab/PDF-Extract-Kit) | **Excluded** |

### 3. Table structure recognition

| name | predicts | input | params / size | CPU latency (conditions) | published accuracy | code | weights | training data | URL | fit |
|---|---|---|---|---|---|---|---|---|---|---|
| SLANet | HTML structure tokens plus cell boxes | table image | 6.9 MB | – / 43.12 ms high-performance (Xeon Gold 6271C, FP32, 8 threads) | 59.52% (Paddle "high-difficulty Chinese table" set) | Apache-2.0 | Apache-2.0 | unverified (PubTabNet plus internal) | [doc](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/table_structure_recognition.en.md) | Tier B |
| **SLANet_plus** | same, stronger on wireless (unruled) tables | table image | 6.9 MB | – / 41.80 ms (same conditions) | 63.69% (same set) | Apache-2.0 | Apache-2.0 | unverified | same | **Tier B, first choice** |
| SLANeXt_wired / _wireless | same | table image | 351 MB (ocr-ml.md) | GPU 85.92 ms | 69.65% | Apache-2.0 | Apache-2.0 | unverified | same | Out (size) |
| PP-LCNet_x1_0_table_cls | wired vs wireless table | table image | 6.6 MB | 3.17 / 1.14 ms | top-1 94.2 | Apache-2.0 | Apache-2.0 | unverified | [doc](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/table_classification.en.md) | Tier B router (rules vs SLANet) |
| TableFormer (docling) | structure tokens plus cell boxes; content matched to PDF cells | image plus PDF cells | not verified | not verified | TEDS 93.6 (claim, via ocr-ml.md) | MIT | CDLA-Permissive-2.0 / Apache-2.0 (HF card) | unverified | [HF](https://huggingface.co/docling-project/docling-models) | Probably too large for tier B (unverified); the odl hybrid used it to reach TEDS 0.928 |
| TATR v1.1 (Pub / Fin / All) | rows, columns, spanning cells, headers (DETR); text from the PDF | table image (plus PDF words for content) | 110 MB weights each | not published | v1.0 on PubTables-1M: GriTS_Con 0.9850, Acc_Con 0.8243 | MIT | MIT (HF v1.1-all card) | PubTables-1M and FinTabNet.c, both CDLA-P-2.0 | [repo](https://github.com/microsoft/table-transformer), [HF](https://huggingface.co/microsoft/table-transformer-structure-recognition-v1.1-all) | Out (110 MB; int8 ≈ 28 MB **[INFERENCE]**). Its training data is usable |
| UniTable | structure, bbox, content | table image | large ViT (unverified) | unverified | unverified | MIT | unverified | unverified | [repo](https://github.com/poloclub/unitable) | Out |
| **From PDF word boxes:** SciTSR GraphTSR | horizontal/vertical adjacency between text chunks | PDF chunks (Tabby) with coordinates | unverified | unverified | not read (repo ships a P/R/F1 scorer) | MIT | unverified | SciTSR (see §7) | [repo](https://github.com/Academic-Hammer/SciTSR) | **The pattern for tier A**: an edge classifier on text-layer chunks |
| From PDF word boxes: pymupdf_layout TableGridExtractor | table grid | MuPDF boxes (plus image in some variants) | ONNX in repo | — | — | AGPL | AGPL | — | [repo](https://github.com/ArtifexSoftware/pymupdf_layout) | Excluded; pattern only |
| From PDF word boxes: Marker digital tables | HTML table | text layer, "CPU heuristics + OrderNet table heads" | in Indra weights | in no_ocr 57 pg/s | OmniParseBench tables 22.0 (no_ocr), 59.6 (fast) | Apache-2.0 | OpenRAIL-M | — | [Marker README](https://github.com/datalab-to/marker) | Excluded; pattern only |
| Rule-based reference points (no ML) | — | text layer | — | kordoc-plain 0.032 s/doc; folio 0.039 s/doc | odl TEDS: kordoc-plain 0.9395, LM-Kit 0.9342, folio 0.894 (RayoPDF 0.915) | MIT (kordoc) / — | — | — | [PR #35](https://github.com/opendataloader-project/opendataloader-bench/pull/35), [PR #33](https://github.com/opendataloader-project/opendataloader-bench/pull/33) | On odl the TEDS gap is closable without ML. The READoc and olmOCR gaps are a different matter |

### 4. Formula recognition and detection

| name | predicts | input | params / size | CPU latency | published accuracy | code | weights | training data | URL | fit |
|---|---|---|---|---|---|---|---|---|---|---|
| pix2tex (LaTeX-OCR) | LaTeX | formula image | not stated; PaddleX port `LaTeX_OCR_rec` 99 MB | not given | BLEU 0.88, normed edit 0.10, token accuracy 0.60 (own set); Paddle En-BLEU 74.55 | MIT | weights in GitHub releases; licence = repo MIT (unverified for weights) | rendered arXiv/Wikipedia formulas (unverified) | [repo](https://github.com/lukas-blecher/LaTeX-OCR) | Out (size) |
| UniMERNet tiny / small / base | LaTeX | image | git clone 441 MB / 773 MB / 1.3 GB; PaddleX port 1,530 MB | PaddleX: – / 8,288 ms high-performance | Paddle En-BLEU 85.91 | Apache-2.0 | Apache-2.0 (HF tiny) | UniMER-1M (licence unverified) | [repo](https://github.com/opendatalab/UniMERNet) | Out |
| PP-FormulaNet-S / plus-S | LaTeX | image | 224 MB / 248 MB | – / 254.39 ms; – / 260.99 ms (Xeon Gold 6271C) | En-BLEU 87.00 / 88.71 (Paddle internal set) | Apache-2.0 | Apache-2.0 | internal | [doc](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/formula_recognition.en.md) | Out (11× over 20 MB) |
| TexTeller | LaTeX | image | unverified (~300M from memory, unverified) | unverified | BLEU (card metric, value not read) | Apache-2.0 | Apache-2.0 | `OleehyO/latex-formulas` (licence unverified) | [repo](https://github.com/OleehyO/TexTeller) | Out |
| texify | LaTeX | image | — | — | — | repo now 404 (licence unverified) | **CC-BY-NC-SA-4.0** | — | [HF](https://huggingface.co/datalab-to/texify) | **Excluded (non-commercial)** |
| PDF-Extract-Kit MFD (YOLOv8_ft) | formula boxes | image | — | — | — | **AGPL-3.0** | AGPL | — | [repo](https://github.com/opendatalab/PDF-Extract-Kit) | **Excluded** |
| Indra math (`mathcls.onnx` + `math_enc` / `math_dec_*`) | math-line flag plus inline LaTeX | line image | 191,005 B classifier; 9.9 + 8.4 + 1.4 MB recogniser | +145 ms/page median with `--math` (357 vs 212 ms, 4 vCPU) | OmniParseBench math tests 14.0 → 55.8 | Apache-2.0 | OpenRAIL-M | — | [Indra](https://github.com/datalab-to/indra) | Excluded. Proves a ~20 MB permissive math recogniser *could* exist; none does today |

RayoPDF already scores best of all measured tools on olmOCR arxiv_math (0.720, from the text layer in LaTeX mode). Formula ML is low priority.

### 5. Reading-order models and the learned-vs-rules evidence

| name | predicts | input | size | CPU latency | accuracy | code | weights | data | URL | fit |
|---|---|---|---|---|---|---|---|---|---|---|
| LayoutReader (`hantian/layoutreader`) | token/line order | boxes (LayoutLMv3 backbone) | — | — | — | MIT (unverified) | **CC-BY-NC-SA-4.0** | ReadingBank (licence unverified) | [HF](https://huggingface.co/hantian/layoutreader) | **Excluded** |
| HURIDOCS pdf-reading-order | token order | PDF features | — | ~0.65 s/page | 99.86% test, 99.77% average on its own labels | Apache-2.0 | **openrail** | own | [HF](https://huggingface.co/HURIDOCS/pdf-reading-order) | Excluded |
| Indra OrderNet | pairwise order | 16 line features (§1) | 2.97 MB | in 212 ms/page | none standalone | Apache-2.0 | OpenRAIL-M | unverified | [order_post.py](https://github.com/datalab-to/indra/blob/main/indra_ocr/order_post.py) | Excluded; architecture to retrain |
| PP-DocLayoutV3 | order from image | image | 130.8 MB | — | — | Apache-2.0 | Apache-2.0 | — | [HF](https://huggingface.co/PaddlePaddle/PP-DocLayoutV3) | Out |
| XY-Cut++ / opendataloader `XYCutPlusPlusSorter` | rule-based order | boxes | — | — | DocBench-100 BLEU-4 98.6 / 98.9 (labels from PP-DocLayout) | — | — | — | `docs/research/algorithms.md` ~L338 | Rules baseline |

**Evidence: learned order vs stream or rules.**
- **The marker evidence has flipped.**
  - The `order_blocks_by_reading_order` docstring still says the PDF character stream "beat surya's learned order head on multi-column pages".
  - But `use_pdftext_reading_order` now defaults to `False`, "since OrderNet (order.onnx) already orders blocks accurately across multi-column and out-of-order PDF streams" ([line.py](https://github.com/datalab-to/marker/blob/master/marker/builders/line.py) ~L163-170).
  - The "75% vs 56%" figure cited in `docs/research/ocr-ml.md` §0 is no longer in current master (unverified where it went).
  - Takeaway: a small *geometric* learned model now beats stream order there. The large vision head did not.
- On odl-bench, ML improves reading order by only +0.03 NID (0.902 → 0.934, opendataloader rules vs hybrid; `ocr-ml.md` §3).

### 6. Inference options

| name | role | binary-size cost | determinism | static linking | licence | URL | fit |
|---|---|---|---|---|---|---|---|
| ncnn 20261020 (already in the add-on) | CNN / transformer runtime | add-on executable 7,310,064 B stripped (Linux), 6,504,960 B (Windows), all tier layers included | Byte-identical text to ORT on 29,499 lines; single thread; fp32; winograd and sgemm off | yes (MinGW too) | BSD-3-Clause (header of `src/ocr/addon/runtime_ncnn.cpp`; GitHub API says NOASSERTION) | [repo](https://github.com/Tencent/ncnn), `docs/development/ocr-engine.md` | **Tier B runtime** (already shipped, Windows-capable) |
| ONNX Runtime 1.31.1 minimal static | same | add-on 7,228,184 B; ops limited to PP-OCRv6 | identical text to ncnn | Linux only (no MinGW) | MIT | [custom build](https://onnxruntime.ai/docs/build/custom.html) | Tier B Linux alternative; each new model widens the op set |
| ggml | tensor library (LLMs) | not measured | — | yes | MIT | [repo](https://github.com/ggml-org/ggml) | Overkill |
| tl2cgen (from Treelite) | GBDT → C code | code size grows with trees **[INFERENCE]** | yes (comparisons plus fixed-order sums) | yes (plain C) | Apache-2.0 | [repo](https://github.com/dmlc/tl2cgen) | Tier A option |
| m2cgen | sklearn / LightGBM / XGBoost → C and other languages | same | same | yes | MIT | [repo](https://github.com/BayesWitnesses/m2cgen) | Tier A option |
| Hand-written generator from a LightGBM `dump_model()` JSON | GBDT → `constexpr` node arrays in C++ | ~12 B/node: 50 trees × 31 nodes ≈ 19 KB **[INFERENCE]** | bit-exact given identical features | yes | LightGBM MIT (not re-checked) | [LightGBM](https://github.com/microsoft/LightGBM) | **Tier A, preferred**: no dependency, reviewable, same build |
| Hand-written int8 / int16 MLP | small dense nets | 16→32→16→k ≈ 1.2k weights ≈ 1–2 KB **[INFERENCE]** | exact with integer accumulation; with float, needs `-ffp-contract=off` and a fixed summation order **[INFERENCE]** | yes | own | — | **Tier A** for line roles and pairwise order |

**Tier-A cost estimate [INFERENCE].** RayoPDF runs ~2,230 pages/s, about 450 µs per page, so 50 µs is roughly an 11% budget.
- A GBDT of 100 trees × depth 6 visits ~600 nodes per sample, ~1–2 µs per line, so 80 lines ≈ 80–160 µs. That is over budget.
- Keep trees to ≤ 50 at depth ≤ 5, or score only candidate lines.
- Pairwise order models cost N². Run them at block level, and only on pages flagged multi-column.

### 7. Training data usable for shipped models

| dataset | content | text layer? | licence (verified source) | URL | fit |
|---|---|---|---|---|---|
| DocLayNet core + extra | 80,863 pages; 11 classes; core = PNG + COCO (28 GiB); **extra = single-page PDFs + JSON of every PDF text cell with bbox and text** (7.5 GiB) | **yes** | **CDLA-Permissive-1.0** (GitHub `LICENSE` file; HF card "Licensing Information") | [repo](https://github.com/DS4SD/DocLayNet) | **Primary data for tier A role classifiers.** Map cells to boxes with GLAM's IoU ≥ 0.95 procedure. No heading levels and no reading order |
| PubLayNet | PMC pages; 5 classes (text, title, list, table, figure) | no (PDFs fetchable from PMC) | annotations CDLA-Permissive-1.0; images under the PMC OA Subset terms (commercial-use collection) (`LICENSE.md`) | [repo](https://github.com/ibm-aur-nlp/PubLayNet) | Secondary. Its boxes are misaligned with text boxes (GLAM §4.3) |
| DocBank | 500K arXiv pages; token-level labels in 12 classes | **yes (tokens)** | **Apache-2.0** ("We update the license to Apache-2.0", README) | [repo](https://github.com/doc-analysis/DocBank) | Token-level labels match tier A directly; science papers only |
| PubTables-1M | ~1M PMC tables; structure plus **words JSON** (TATR eval reads `--table_words_dir`) | words yes | **CDLA-Permissive-2.0** (HF card) | [HF](https://huggingface.co/datasets/bsmock/pubtables-1m) | **Primary data for the tier-A table edge classifier** |
| FinTabNet / FinTabNet.c | financial-report tables | original said to include PDFs (unverified) | FinTabNet.c **CDLA-Permissive-2.0** (HF); original IBM licence unverified | [HF](https://huggingface.co/datasets/bsmock/FinTabNet.c) | Training data for unruled financial tables |
| SciTSR | 15,000 arXiv tables: PDFs, chunks, structure, relation labels | **yes (chunks)** | repo **MIT**; the underlying arXiv PDFs keep their authors' licences (risk) | [repo](https://github.com/Academic-Hammer/SciTSR) | Use with care |
| ICDAR 2013 table competition / ICDAR-2013.c | EU/US government PDFs (unverified) | yes | original: no licence stated (unverified); ICDAR-2013.c **CDLA-Permissive-2.0** (HF) | [orig](https://www.tamirhassan.com/html/competition.html), [.c](https://huggingface.co/datasets/bsmock/ICDAR-2013.c) | Small; better kept for evaluation |
| OmniParseBench | 2,937 pages, 16,288 tests | mixed | **per-source**; tests CC-BY-4.0; olmOCR-derived tests ODC-BY; some CC-BY-SA; two **non-commercial** sources kept "as fair use" (Ohio DOT, htr-united) | [licences](https://huggingface.co/datasets/datalab-to/omni_parse_bench/blob/main/licenses/README.md) | **Benchmark only.** Non-commercial pages and test contamination rule out training |
| odl-bench, olmOCR-bench | benchmarks | — | — | [odl](https://github.com/opendataloader-project/opendataloader-bench) | Evaluation only (contamination) |
| RayoMD-generated PDFs (own) | unlimited Markdown → PDF pairs with known heading levels, tables, lists, column layouts | yes | own (Apache-2.0) | — | **[INFERENCE]** The only source with heading-level and multi-column order labels. Mix with real data: the weasyprint holdout shows the synthetic-to-real gap |

### 8. Recent small or CPU claims on public benchmarks

| system | benchmark result | what it uses | licence | URL |
|---|---|---|---|---|
| LM-Kit One 2026.9.8 `lm-kit` | odl overall 0.9478 (NID .9545, TEDS .9342, MHS .9389), 0.0041 s/doc on an M4 Max; byte-identical across 5 rounds | "text-extraction" strategy, no OCR; method not disclosed | commercial (free tier) | [PR #34](https://github.com/opendataloader-project/opendataloader-bench/pull/34) |
| LM-Kit One `lm-kit-ocr` | 0.9630, 0.0243 s/doc | plus built-in OCR; dictionaries ship in the package | commercial | same |
| kordoc 4.15.7 `plain` | 0.9491 (NID .9540, TEDS .9395, MHS .9425), 0.032 s/doc on an M5 | pdf.js text layer, rules. The author says most of the 0.937 → 0.949 gain comes from output formatting (no image placeholders, links or bold; HTML tables) | MIT | [PR #35](https://github.com/opendataloader-project/opendataloader-bench/pull/35) |
| kordoc `plain-ocr` | 0.9711 | plus bundled PP-OCRv5 via onnxruntime-node (reads text inside images) | MIT | same |
| folio | 0.924 (NID .947, TEDS .894, MHS .879), 0.039 s/doc on a 4-core Xeon | pure Python: pypdfium2 text layer plus geometry; no OCR or ML | unverified | [PR #33](https://github.com/opendataloader-project/opendataloader-bench/pull/33) |
| PDFluent 1.3.0 auto / fast | 0.9154 / 0.8573 (TEDS .8695 / .6316) | auto downloads "layout" and "tables" models | AGPL-3.0 or commercial (excluded) | [PR #37](https://github.com/opendataloader-project/opendataloader-bench/pull/37) |
| Indra OCR 1.1 | OmniParseBench 71.4 with `--math`, 66.7 default, 61.2 OCR-only; leaderboard row 69.5; 212 ms/page median on 4 vCPU | 12.3M params of ONNX in total (1.25M main recogniser); text layer where it passes checks, OCR elsewhere | code Apache-2.0, weights modified OpenRAIL-M | [Indra](https://github.com/datalab-to/indra), [blog](https://www.datalab.to/blog/open-ocr-models-omniparsebench) |
| Marker `fast` | OmniParseBench 71.0 (text 62.3, tables 59.6, layout 91.2), 23.6 pg/s on CPU | pdftext plus ~18 MB Indra ONNX (layout, OrderNet, line and math OCR) | same | [Marker](https://github.com/datalab-to/marker) |
| Marker `no_ocr` | 40.8 headline (82.7 on digital documents), 57.0 pg/s | pdftext plus Indra layout and OrderNet plus CPU table reconstruction; no OCR | same | same |

Reading: the three strongest text-layer odl entries (LM-Kit, kordoc-plain, folio) use no ML, or none they disclose. On odl the gap is largely output conventions, not models. ML matters on harder table and multi-column suites.

---

### Fit for RayoPDF

The gaps come from the shared context:
- tables: READoc table_teds 0.618, olmOCR table_tests 0.631, holdout TEDS 0.722;
- headings: MHS 0.8905;
- headers and footers: olmOCR 0.849 vs docling 0.898;
- multi-column: 0.710;
- odl NID 0.9254.

#### Tier A: in-core (deterministic, no download, ≤ 1 MB, ≤ 50 µs/page, permissive)

1. **Line-role classifier (heading yes/no and level, page header/footer, caption, footnote, list item).**
   - Model: GBDT with ≤ 50 trees at depth ≤ 5, or an int8 MLP, over features RayoPDF already computes:
     - font size relative to the page mode;
     - bold or italic and a font-name class;
     - caps share; numbering or bullet prefix;
     - Unicode categories of the first and last 2 characters (HURIDOCS `TokenFeatures`);
     - gaps above and below; width relative to the column;
     - y position; repetition of the line across pages.
   - Training data:
     - DocLayNet extra cells joined to the COCO boxes (CDLA-P-1.0; GLAM's IoU ≥ 0.95 assignment);
     - DocBank tokens (Apache-2.0);
     - RayoMD-generated documents for heading *levels*.
   - Runtime: a hand-written generator from the LightGBM JSON dump into `constexpr` C++ arrays.
   - Evidence: text-box-only GLAM-CV (1M params) beats YOLOv5x6 on page footer (87.3 vs 61.1), page header (78.1 vs 67.9) and section header (79.8 vs 74.6 for full GLAM) ([GLAM Tables 1 and 3](https://arxiv.org/abs/2308.02051)).
   - Expected cost: ~20 KB and ~10–30 µs/page **[INFERENCE]**.
   - Targets: MHS and headers_footers.

2. **Word-pair edge classifier for table structure (same cell / same row / same column) inside table candidates.**
   - Model: GBDT or MLP on pairwise geometry, alignment, gap and ruling-line features.
   - Training data: PubTables-1M words plus structure, FinTabNet.c (both CDLA-P-2.0), and RayoMD tables. Keep ICDAR-2013.c for evaluation.
   - Cost: runs only on table regions, so the average per page stays small **[INFERENCE]**.
   - Evidence:
     - SciTSR GraphTSR uses exactly this formulation on PDF chunks ([SciTSR](https://github.com/Academic-Hammer/SciTSR)).
     - Marker reconstructs digital tables from the text layer with learned table heads ([README](https://github.com/datalab-to/marker)).
     - Pure text-layer engines reach odl TEDS 0.89–0.94 ([PR #33](https://github.com/opendataloader-project/opendataloader-bench/pull/33), [#34](https://github.com/opendataloader-project/opendataloader-bench/pull/34), [#35](https://github.com/opendataloader-project/opendataloader-bench/pull/35)).
   - Targets: READoc, olmOCR table and holdout TEDS.

3. **OrderNet-style pairwise block-order model for multi-column pages.**
   - Features: 16 per block (10 geometry, 6 text), as in Indra's `line_features`; insertion decoding.
   - Retrained on our own data, since the Indra weights are OpenRAIL-M.
   - Cost control: run only on pages the rules flag as multi-column, and at block level, because cost is N².
   - Evidence: Marker switched its default from PDF stream order to OrderNet for multi-column and out-of-order streams ([line.py](https://github.com/datalab-to/marker/blob/master/marker/builders/line.py)).
   - Caveats:
     - Order labels exist in no permissive real-world dataset I verified. DocLayNet has none; ReadingBank's licence is unverified. Training depends on RayoMD synthetic data.
     - The odl ML gain on order was only +0.03 NID.
   - Target: multi_column 0.710. Rank 3 because of the data risk.

**Not for tier A:**
- HURIDOCS LightGBM: 102 MB and openrail.
- Full GLAM: ResNet-18 features.
- VGT and LayoutLM: 100M+ parameters.

#### Tier B: OCR add-on (ncnn, opt-in or routed pages, ≤ 20 MB, ≤ 100 ms/page on 4 threads)

1. **SLANet_plus plus PP-LCNet_x1_0_table_cls on routed table crops.** Cell text comes from the text layer, as TATR and TableFormer pipelines do.
   - Size: 6.9 + 6.6 = 13.5 MB.
   - Latency: 41.80 + 3.17 ms (high-performance and normal mode respectively; Xeon Gold 6271C, 8 threads).
   - Licence: Apache-2.0 for code and weights.
   - Evidence:
     - SLANet_plus 63.69% vs SLANet 59.52% on Paddle's hard set ([doc](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/table_structure_recognition.en.md));
     - the table classifier has top-1 94.2 ([doc](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/table_classification.en.md));
     - the odl hybrid's table-ML routing raised TEDS 0.489 → 0.928 (`ocr-ml.md` §3).
   - Risk: SLANet's autoregressive structure decoder may need splitting for pnnx/ncnn **[INFERENCE]**. Latency at 4 threads is unmeasured.

2. **PicoDet_layout_1x_table (7.4 MB, 27.66 ms; table mAP 97.5) or PP-DocLayout-S (4.834 MB, 18.53 ms; 23 classes, mAP@0.5 70.9) as the region detector for routed pages.**
   - Uses: find unruled tables, figures whose text should not enter prose, and formula regions.
   - Snap boxes to text-layer lines, as docling does with PDF cells ([heron paper](https://arxiv.org/abs/2509.11720)).
   - Licence: Apache-2.0 weights. Training data is Paddle's own and unreleased, which is acceptable because the weights are permissive.
   - Combined with B1: ≈ 21 MB with PicoDet-table or ≈ 18.3 MB with PP-DocLayout-S. Prefer PP-DocLayout-S to stay under 20 MB.

3. **A small detector retrained on DocLayNet (CDLA-P-1.0) plus RayoMD pages with RayoPDF's own classes, used as a second opinion on low-confidence header/footer and heading pages.**
   - Architecture: PicoDet-S scale (~5 MB, as PP-DocLayout-S).
   - Evidence:
     - docling's D-FINE-m (egret-m) reaches 0.765 mAP on canonical DocLayNet but needs 334 ms on 4 threads ([paper](https://arxiv.org/abs/2509.11720));
     - PicoDet-S class models run at ~18 ms ([doc](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/layout_detection.en.md)).
   - Accuracy at that size on DocLayNet is unmeasured **[INFERENCE]**. D-FINE and PicoDet code licences were not re-checked.

**Not for tier B:**
- formula recognisers: PP-FormulaNet-S 224 MB, UniMERNet 441 MB+;
- TATR: 110 MB;
- docling heron and egret: ≥ 334 ms;
- PP-DocLayoutV3: 130.8 MB.

**Excluded, with reasons:**
- HURIDOCS weights (fast models and reading order): openrail.
- Indra, Marker and Surya weights: modified OpenRAIL-M.
- PyMuPDF Layout, PDFluent, DocLayout-YOLO, Ultralytics, PDF-Extract-Kit: AGPL-3.0.
- LayoutLMv3 and `hantian/layoutreader`: CC-BY-NC-SA-4.0.
- texify weights: CC-BY-NC-SA-4.0.
- LM-Kit One: closed and commercial.
- OmniParseBench, odl-bench, olmOCR-bench as training data: benchmarks, with non-commercial sources in OmniParseBench.

#### Where this touches RayoPDF
- `src/layout/` — where the tier-A line-role, table-edge and order classifiers would plug into the existing rule pipeline.
- `src/ocr/addon/runtime_ncnn.cpp` — the ncnn runtime that tier-B layout and table networks would extend.
- `docker/ocr_models.py` — pinned model download and pnnx conversion; new tier-B models (SLANet_plus, PP-DocLayout-S) would go here.
- `docs/development/ocr-engine.md` — measured add-on sizes and timings, the baseline for the tier-B budget.
- `docs/research/ocr-ml.md` — the earlier decision gate (table ML only if TEDS improves by ≥ 0.10). This report updates its reading-order evidence (marker now defaults to OrderNet).

## Part 3. Literature: evidence, disagreements, coverage and references

### 2. Evidence

Strength ratings: **S** = strong (large or independent test set, like-for-like baseline); **M** = moderate (peer-reviewed, small or in-domain data, or baseline not like-for-like); **W** = weak (small, unclear evaluation, weak venue).

| # | Work | Lang | Venue type | Yr | Topic | Finding with conditions | Model size, data, code and licence | Strength |
|---|---|---|---|---|---|---|---|---|
| 1 | Wang J. et al., GLAM | en | ICDAR 2023 (LNCS), arXiv | 2023 | e, a, b, c | DocLayNet test, mAP@[.5:.95]. GNN-only (≈1M params, 79 features per text box, no image): page-footer 87.3, page-header 78.1, section-header 70.1, title 72.9, caption 60.6, footnote 61.1, formula 43.8, list 61.5, table 50.2, overall 59.9. YOLOv5x6 (140M): 61.1 / 67.9 / 74.6 / 82.7 / 77.7 / 77.2 / 66.2 / 86.2 / 86.3, overall 76.8. Adding ResNet-18 ROI features (4M total) gives overall 68.6. CPU 16 ms/page (GNN-only 9 ms). The authors note mAP favours text-box snapping, which helps text-cell models. | 1–4M params; DocLayNet cells. No code link in the paper. DocLayNet licence CDLA-Permissive-1.0 (LICENSE file). | M |
| 2 | Livathinos et al., Robust PDF conversion with RNNs | en | AAAI 2021 (IAAI) | 2021 | e | 17 labels per PDF text cell, 2,940 pages, 7 publisher layouts, 60/40 page split (layouts seen in training). Bi-LSTM + attention, stacked binned features (geometry + character features, no text content): weighted F1 0.97. Recursive random forest: 0.93–0.99 on one layout, 0.71–0.97 on diverse layouts. Mask R-CNN much lower on subtitle and list. Served CPU-only, 180 MB per replica. | 479,769 params, 5.8 MB. Data on Kaggle; licence not checked (unverified). | M |
| 3 | Luong, Nguyen, Kan, SectLabel | en | IJDLS 2010 | 2010 | e, b | CRF line labelling, 23 classes, 40 papers, 10-fold CV. Macro F1: 68.4 (ParsCit features), 75.6 (+raw text), 84.7 (+OCR layout features). Per class (+text → +layout): footnote 41.5→69.6, sub-sub-section header 41.3→81.7, sub-section header 76.0→91.4, equation 58.8→72.0, figure caption 62.2→76.9, list item 62.3→71.2, page number 95.4→97.8. Layout features came from OmniPage OCR, not the PDF text layer. | CRF++. Data on the ParsCit site; licence unverified. | M |
| 4 | Li M. et al., DocBank | en | COLING 2020 | 2020 | e | 500K arXiv pages, token labels, area-weighted F1. BERT-base → LayoutLM-base (text+2D position): footer 0.78→0.90, list 0.71→0.89, caption 0.86→0.96, equation 0.82→0.89, section 0.91→0.96. Shows which classes depend on position. The models are not small. | README says Apache-2.0 but also "Please DO NOT re-distribute our data". Conflicting terms; legal check needed before training a shipped model. | S (for the class ranking) |
| 5 | Meuschke et al., benchmark of PDF IE tools | en | iConference 2023 (LNCS) | 2023 | e, a, f | DocBank-based, 10 tools. Footers: only rule-based PdfAct extracts them (F1 0.20). Equations: only GROBID (CRF) (0.25). Lists: 0 for all tools. Sections: GROBID 0.74 vs PdfAct 0.16. Captions: 0.49 vs 0.45. Off-the-shelf tools, not like-for-like training. | Code and data public (pdf-benchmark.gipplab.org). | M |
| 6 | Huang P.-W. et al., Lightweight contextual LSR | en | SDP workshop 2022 | 2022 | e | Text-only RoBERTa with sliding-window line attention: macro F1 0.832 on SectLabel data vs 0.847 for SectLabel with rich features. Context lines partly replace layout features. Not small. | — | M |
| 7 | HURIDOCS pdf-document-layout-analysis | en | software (no paper) | 2024–26 | e | "Fast" mode = two LightGBM models (token type plus segmentation) over Poppler XML features, trained on DocLayNet; 0.42 s/page on CPU (i7-8700). 13 token types incl. HEADER, FOOTER, PAGE_NUMBER, FOOTNOTE, FORMULA. **No published accuracy for the LightGBM mode found** (unverified). | Apache-2.0 (GitHub API). | W |
| 8 | Josi, Wartena, Heid, legal PDFs | en | CS & IT (AIRCC) 2022 | 2022 | a, e | 70 German contracts, 20,938 text regions, 10-fold CV. Random forest on geometry and font features: header F1 0.87, footer 0.95, heading 0.54. Per-page-layout classifiers raise accuracy to 0.96–0.98. No cross-page features. | Corpus released CC BY 4.0 (paper). Venue weak. | W |
| 9 | Lin X., header/footer by page association | en | HP Labs TR / SPIE DRR 2003 | 2002 | a | Rules. Candidate top-5 and bottom-3 lines, digits replaced by '@', edit-distance similarity × geometry similarity to the same-rank line on ±8 pages, position weights. 9 OCR'd documents, 1,156 pages: P 98.0%, R 92.7%. Misses headers unique to 1–2 pages (short articles). | No code. | M |
| 10 | 刘高军等, 页眉页脚判别 (Liu G. et al., header/footer discrimination) | zh | 北方工业大学学报 2013 | 2013 | a | Rules on OCR: 「提出了分割线探测结合区域特征的页眉页脚判别算法」 ("proposes a header/footer discrimination algorithm combining separator-line detection with region features"). Source: CQVIP abstract. | — | W |
| 11 | Chinese patents on PDF/OFD header/footer extraction (CQVIP list) | zh | patents | 2019–25 | a | All rule-based; one CNN-on-image patent. E.g. CN110543810A uses the order of objects in the content stream plus position; an OFD patent 「检测重复特征和页码特性」 ("detects repetition features and page-number properties"). Not peer-reviewed. | — | W |
| 12 | Juge, Bentabet, Ferradans, FinTOC-2019 overview | en | FNP workshop (ACL Anthology) | 2019 | b | 58 English prospectuses; title detection on text blocks, weighted F1 on a blind test set: Aiai BiLSTM 0.982, UWB MaxEnt 0.972, YseopLab char-CNN 0.972, FinDSE extra-trees 0.970, **Daniel (rule-based, parses TOC pages) 0.949**. Level extraction: rule system 0.427 (ICDAR'13 measure). | Shared-task data; licence unverified. | M |
| 13 | Bentabet, Juge, Ferradans, TOC generation | en | ICDAR 2019 | 2019 | b | Title detection F1 — arXiv (252K training docs): GBT on 28 hand-crafted features 88.1, GBT combined 91.2, char-CNN 93.8; investment docs (47 train / 24 test): GBT combined **98.4** vs char-CNN 97.6. Level assignment: GBT combined 64.2 (arXiv) / 74.2 (investment) vs word-CNN + BiLSTM-CRF 92.9 / 84.8. Templates help only with fewer than 10 training documents. | XGBoost / Keras; no code link. | M |
| 14 | Budhiraja & Mago, heading detection | en | arXiv | 2018 | b | 500 PDFs, 83K text runs, SMOTE-balanced. Decision tree on 7 features (bold, font above body mode, word count, case, verbs, nouns, numbers): accuracy 96.95%. No rule baseline; balanced evaluation is unclear. | Data repository linked in the paper. | W |
| 15 | Chi et al., GraphTSR / SciTSR | en | arXiv | 2019 | c | Input: table cells from PDF. Graph attention on a KNN graph; 4 blocks, d=64. Micro F1 on adjacency relations: ICDAR 2013 0.872 vs Tabby (rules) 0.854; SciTSR 0.953 vs 0.921; SciTSR-COMP (spanning cells) 0.955 vs 0.882. Trained on 12K arXiv tables; ICDAR 2013 is cross-domain. | MIT (GitHub API); content derived from arXiv LaTeX, licences vary (unverified). | M |
| 16 | Aoyagi et al., plural NN modules | en (Japanese group) | ICPRAM 2022 | 2022 | c | pdfalto tokens. 4 small MLPs: horizontal merger (107 + 144 input dims), implicit-ruled-line identifier (13 features + 100-d word2vec), vertical merger, cell generator. ICDAR 2013, 156 tables, regions given by hand: R/P/F 0.967 / 0.977 / **0.972** vs Nurminen (competition winner, rules) 0.946, Shigarov 0.936, GraphTSR 0.872. Trained on 209 tables, half from the ICDAR 2013 practice set (in-domain). | No code link. | M |
| 17 | 青柳ら, 表検出を含むエンドツーエンド表構造解析 (Aoyagi et al., end-to-end table structure analysis with detection) | ja | WI2 workshop 2022 (J-STAGE, not reviewed) | 2022 | c | 「提案手法はABBYY FineReader PDFと比較して，セルの隣接関係に基づく評価指標のF値が0.962となり0.2ポイント上回った一方で，TEDSが0.948となり1ポイント下回る」 ("compared with ABBYY FineReader PDF, adjacency F was 0.962, 0.2 points higher, while TEDS was 0.948, 1 point lower"). Table detection by CascadeTabNet (image). Only the abstract and tables were extractable from the PDF. | — | W–M |
| 18 | 田上ら, トークン抽出処理の改良とFinTabNet評価 (Tagami et al., improved token extraction, FinTabNet evaluation) | ja | WI2 2025 (J-STAGE) | 2025 | c | Same method family on FinTabNet, 10,635 test tables. pdfalto tokens 「欠落や誤抽出が見られた」 ("omissions and mis-extractions were seen"). Added OCR, fine-tuned on 150 tables: TEDS **0.780**, +8.9 points over their DEIM 2025 method. Abstract only. | — | W |
| 19 | Liu Y., Mitra, Giles, sparse lines | en | CIKM 2008 | 2008 | c | 300 pages (chemistry, CS, archaeology), 33,069 lines, 5-fold. Table-line labelling F: **CRF 96.36**, linear SVM 94.38, the authors' rules 91.93. Orthographic-only CRF ≈42–45: layout features decide the result. | No code. | M |
| 20 | Klampfl, Jack, Kern, unsupervised tables | en | D-Lib 2014 | 2014 | c | Unsupervised clustering and projection on PDFBox words; ICDAR 2013 adjacency R/P ≈0.77–0.86. Notes the ICDAR 2013 winner was Nurminen's rule system. | — | M |
| 21 | Holeček et al., table understanding | en | ICDARW 2019 | 2019 | c | GCN over PDF word boxes for invoice tables; abstract only opened, numbers not extracted. | — | W (unread body) |
| 22 | Wang Z. et al., LayoutReader / ReadingBank | en | EMNLP 2021 | 2021 | d | ReadingBank (DOCX→PDF words, 500K pages). Page BLEU: left-to-right top-to-bottom heuristic 0.697, layout-only LayoutReader 0.973, full 0.982. Layout carries most of the signal, but the model is LayoutLM-base sized. | ReadingBank: "research purpose only… DO NOT re-distribute". **Excluded** for training shipped models. | S (in-domain) |
| 23 | Liu S., Li, Wei, XY-Cut++ | en | arXiv (v3 2026) | 2025 | d | Block-level BLEU-4. DocBench-100: XY-Cut 0.797, LayoutReader 0.788, MinerU 0.873, **XY-Cut++ 0.988**. OmniDocBench (single / double / three-column / complex): LayoutReader 0.988 / 0.831 / 0.595 / 0.716, XY-Cut 0.895 / 0.695 / 0.702 / 0.717, XY-Cut++ 0.993 / 0.951 / 0.967 / 0.901. CPU sort-only: XY-Cut++ 514 FPS vs LayoutReader 22. Needs detector labels; weights grid-searched on 2.8K docs; authors' own benchmark. | Rust port exists (third party, not checked). | M |
| 24 | Wang R., Fujii, Bissacco (Google), sparse graph segmentation | en | ICDAR 2023 (LNCS) | 2023 | d | GCN (10 steps, node dim 32) on β-skeleton of line boxes. Predicts column-wise vs row-wise pattern per line and paragraph edges; rules do cluster-and-sort. Normalized Levenshtein, English: 0.098 vs 0.146 (topological-sort baseline) vs 0.126 (fully connected GCN). Image features add +1.7 F1 for the pattern task. Trained on 25K OCR'd images plus PubLayNet. | 267K params (144K MobileNetV3); no code. | M |
| 25 | Quirós & Vidal, pairwise order on handwriting | en | Neural Comput. Appl. 2022 | 2022 | d | MLP learns P(a before b) from geometric features, then greedy or "first decide then decode" sorting. OHG dataset: under 4 swaps per page vs about 13 for top-bottom-left-right (−74%). Handwritten domain. | Open access. | M (domain gap) |
| 26 | Lin X., Gao, Tang et al., formula identification in PDF | en (PKU) | ICDAR 2011 | 2011 | f | 200 textbook pages (English + Chinese), 1,541 isolated and 3,237 embedded formulas, 5-fold. Isolated F1: rules 90.60, SVM (9 features) 95.64, hybrid 96.14. Embedded: rules only, 83.61. Used in a commercial Chinese e-book product. | No code. | M |
| 27 | Madisetty et al., inline math (reproduces Iwatsuki 2017) | en | Expert Systems 2021 | 2021 | f | 70 ACL PDFs, 378K tokens, 5-fold, macro F over B/I/O tags: InftyReader 23.8, Iwatsuki CRF 80.4, CRF with 17 features 83.4, Bi-LSTM + features 86.1. B-Math F1 0.73–0.76. | Iwatsuki data licence unverified. | M |
| 28 | 林晓燕 (Lin Xiaoyan), PhD thesis, Peking University (supervisor 汤帜) | zh | thesis | 2014 | f | Abstract: 「针对机器学习技术应用于公式定位时的数据不平衡问题，提出了重采样改进方案，显著改善了公式定位的效果」 ("for class imbalance when applying ML to formula localization, a resampling scheme markedly improved localization"). Abstract only (CQVIP list view; record page failed). | — | W |
| 29 | GROBID design notes | en | software documentation | — | a, e | Line-level CRF "segmentation" model labels headnotes, footnotes, header and body, relying on layout features; trained on small hand-labelled sets. | Licence not checked this session (believed Apache-2.0, unverified). | W (no numbers) |

Integrity: OpenAlex `is_retracted` = false for GLAM, LayoutReader, SectLabel, Lin 2011, Livathinos (AAAI), Liu 2008, Quirós 2022, Madisetty 2021, Aoyagi 2022, DocBank, Bentabet 2019, GraphTSR (arXiv), and the Google reading-order paper. Crossref `updated-by` was not checked.

### 3. Disagreements and gaps

1. **Rules vs learned on furniture.** No study pits a learned model against cross-page repetition rules. GLAM's win is over an image detector, and its mAP metric rewards text-box snapping [Wang 2023]. RayoPDF would have to run that comparison itself [inference].
2. **Table numbers are not comparable across papers.** Aoyagi 2022 trains partly on the ICDAR 2013 practice set and is given table regions by hand. GraphTSR is trained on arXiv and evaluated cross-domain. Liu 2008's rules are the authors' own. None reports TEDS on READoc or olmOCR-style tables.
3. **Reading order.** LayoutReader is near-perfect on ReadingBank (DOCX-derived, mostly single-column [inference from its BLEU distribution]) but weak on three-column OmniDocBench [Liu 2025]. This points to training-distribution effects, not model capacity.
4. **Heading levels.** Learned level assignment (85–93%) vs rule clustering of font sizes is untested. FinTOC's only level system was rule-based and scored 0.427 on a different measure [Juge 2019].
5. **Inline math.** The only inline baseline is OCR (InftyReader). No paper compares a learned model with a strong rule system on born-digital fonts, e.g. TeX math fonts.
6. **Size and latency are rarely reported.** Exceptions: Livathinos 2021, GLAM, Google 2023, HURIDOCS speed. No paper reports µs-per-page CPU cost at RayoPDF's ~1 ms budget.
7. **Language coverage.** No Chinese, Japanese or Korean work learns furniture, headings or reading order from the PDF text layer. The Japanese table work (Okayama University / NII) is the only CJK line of text-layer learning found.

### 4. Coverage

- **English:** arXiv full texts (GLAM, Livathinos, LayoutReader, GraphTSR, DocBank, Bentabet, Budhiraja, Meuschke, XY-Cut++, Google 2023); ACL Anthology (FinTOC overview, SDP 2022); author and institution PDFs (SectLabel, Lin 2011, Liu 2008, Madisetty, Lin 2002, Josi 2022); D-Lib (Klampfl); Springer open access (Quirós); GitHub and Hugging Face for licences; OpenAlex for venues and retraction status.
- **Chinese (zh):**
  - OpenAlex `language:zh` title search: 版面分析 → 56 (mostly newspaper-design noise); 表格结构识别 → 0; 页眉页脚 → 5 (Word tips); "PDF 表格" → 2 (noise).
  - CQVIP: "PDF 表格结构识别" → 3,224 (mostly patents; incl. BIT's GraphTSR patent CN110751038A); "PDF 页眉页脚 检测" → 33; "PDF 数学公式 识别" → 826.
- **Japanese (ja):**
  - J-STAGE API 表構造認識 → 1 (medical forms).
  - CiNii: "PDF レイアウト解析" → 2 (one JSAI 2025 DocLayout-YOLO paper, image-based); 表構造解析 → 23 (term verified in titles; found the Aoyagi/Ohta line); 読み順推定 → 1 (evolutionary OCR ordering, 2012); "数式検出 PDF" → 0.
- **Korean (ko):**
  - OpenAlex `language:ko` 레이아웃 분석 → 69 (design and engineering noise).
  - KoreaScience: 표 구조 인식 → 51 (image or rule-based); 문서 레이아웃 분석 → 6 (incl. a 2026 DocLayout-YOLO + OCR TOC generator).
  - RISS: "PDF 문서 구조 분석" → 190 (10 articles, 57 theses; a 2016 Hanyang thesis on iterative Y-cut, title only).
  - No learned text-layer work found.
- **Blocked or partial sources:**
  - ACM DL (Iwatsuki 2017, DocEng): HTTP 403 Cloudflare; numbers taken from Madisetty 2021 (secondary).
  - DEIM 2019 and 2020 PDFs (Japanese table-structure papers): text extraction failed; unused.
  - NLPCC 2013 「中文电子文档的数学公式定位研究」 (Lin, Gao, Tang): PDF unparseable and not in OpenAlex; unused (snippet only).
  - CQVIP thesis record page: redirect failure; list-view abstract used.
  - ICDAR 2013 Table Competition report: no open full text found; winner identity from Klampfl 2014; Nurminen's figures from Aoyagi 2022.
  - Google Scholar and Semantic Scholar: not used (blocked, per the skill's source notes).

### 5. References

1. Wang J., Krumdick M., Tong B. et al. *A Graphical Approach to Document Layout Analysis.* ICDAR 2023, LNCS. DOI 10.1007/978-3-031-41734-4_4; https://arxiv.org/abs/2308.02051
2. Livathinos N., Berrospi C., Lysak M. et al. *Robust PDF Document Conversion Using Recurrent Neural Networks.* AAAI 2021. DOI 10.1609/aaai.v35i17.17777; https://arxiv.org/abs/2102.09395
3. Luong M.-T., Nguyen T.D., Kan M.-Y. *Logical Structure Recovery in Scholarly Articles with Rich Document Features.* IJDLS 2010. DOI 10.4018/jdls.2010100101; https://www.comp.nus.edu.sg/~kanmy/papers/ijdls-SectLabel.pdf
4. Li M., Xu Y., Cui L. et al. *DocBank: A Benchmark Dataset for Document Layout Analysis.* COLING 2020. DOI 10.18653/v1/2020.coling-main.82; licence at https://github.com/doc-analysis/DocBank
5. Meuschke N., Jagdale A., Spinde T. et al. *A Benchmark of PDF Information Extraction Tools…* iConference 2023, LNCS 13972. https://arxiv.org/abs/2303.09957
6. Huang P.-W., Ramesh Kashyap A. et al. *Lightweight Contextual Logical Structure Recovery.* SDP 2022. https://aclanthology.org/2022.sdp-1.5.pdf
7. HURIDOCS. *pdf-document-layout-analysis* (software, Apache-2.0). https://github.com/huridocs/pdf-document-layout-analysis ; token labeler https://github.com/huridocs/pdf-tokens-type-labeler
8. Josi F., Wartena C., Heid U. *Preparing Legal Documents for NLP Analysis…* CS & IT 2022. DOI 10.5121/csit.2022.120102; https://serwiss.bib.hs-hannover.de/files/2161/csit120102.pdf
9. Lin X. *Header and Footer Extraction by Page-Association.* HP Labs HPL-2002-129; SPIE DRR X 2003. DOI 10.1117/12.472833; https://shiftleft.com/mirrors/www.hpl.hp.com/techreports/2002/HPL-2002-129.pdf
10. 刘高军, 刘妍妍, 付晓玲 (Liu G. et al.). [header/footer discrimination algorithm; full title not shown in the CQVIP list] 《北方工业大学学报》 2013(1):17–24. Via https://www.cqvip.com/search?k=PDF%20页眉页脚%20检测
11. CQVIP patent records CN110543810A, CN111144300A, CN110751038A, CN119206749A, CN122531041A. https://www.cqvip.com/search?k=PDF%20表格结构识别
12. Juge R., Bentabet N.-I., Ferradans S. *The FinTOC-2019 Shared Task: Financial Document Structure Extraction.* FNP 2019. https://aclanthology.org/W19-6407.pdf
13. Bentabet N.-I., Juge R., Ferradans S. *Table-Of-Contents Generation on Contemporary Documents.* ICDAR 2019. DOI 10.1109/ICDAR.2019.00025; https://arxiv.org/abs/1911.08836
14. Budhiraja S.S., Mago V. *A Supervised Learning Approach for Heading Detection.* arXiv 1809.01477. https://arxiv.org/abs/1809.01477
15. Chi Z., Huang H., Xu H.-D. et al. *Complicated Table Structure Recognition.* arXiv 1908.04729. https://arxiv.org/abs/1908.04729 ; code https://github.com/Academic-Hammer/SciTSR (MIT)
16. Aoyagi H., Kanazawa T., Takasu A., Uwano F., Ohta M. *Table-structure Recognition Method Consisting of Plural Neural Network Modules.* ICPRAM 2022. DOI 10.5220/0010817700003122; https://www.scitepress.org/Papers/2022/108177/108177.pdf
17. 青柳拓志, 金澤輝一, 高須淳宏, 上野史, 太田学 (Aoyagi et al.). 「表検出を含むエンドツーエンド表構造解析手法の評価」 ("Evaluation of an end-to-end table-structure analysis method including table detection"). WI2 18, 2022. DOI 10.57413/wii.18.0_1; https://www.jstage.jst.go.jp/article/wii/18/0/18_1/_article/-char/ja/
18. 田上歩夢, 金澤輝一, 上野史, 太田学 (Tagami et al.). 「表構造解析手法のトークン抽出処理の改良とFinTabNetを用いた評価」 ("Improving token extraction of a table-structure analysis method and evaluation on FinTabNet"). WI2 21, 2025. DOI 10.57413/wii.21.0_108; https://cir.nii.ac.jp/crid/1390025562512128640
19. Liu Y., Mitra P., Giles C.L. *Identifying Table Boundaries in Digital Documents via Sparse Line Detection.* CIKM 2008. DOI 10.1145/1458082.1458255; https://clgiles.ist.psu.edu/pubs/CIKM2008-table-boundaries.pdf
20. Klampfl S., Jack K., Kern R. *A Comparison of Two Unsupervised Table Recognition Methods from Digital Scientific Articles.* D-Lib 20(11/12), 2014. DOI 10.1045/november14-klampfl
21. Holeček M., Hoskovec A., Baudiš P., Klinger P. *Table Understanding in Structured Documents.* ICDARW 2019. https://arxiv.org/abs/1904.12577
22. Wang Z., Xu Y., Cui L., Shang J., Wei F. *LayoutReader: Pre-training of Text and Layout for Reading Order Detection.* EMNLP 2021. DOI 10.18653/v1/2021.emnlp-main.389; data terms at https://github.com/doc-analysis/ReadingBank
23. Liu S., Li Y., Wei J. *XY-Cut++: Advanced Layout Ordering via Hierarchical Mask Mechanism on a Novel Benchmark.* arXiv 2504.10258v3. https://arxiv.org/abs/2504.10258
24. Wang R., Fujii Y., Bissacco A. *Text Reading Order in Uncontrolled Conditions by Sparse Graph Segmentation.* ICDAR 2023. DOI 10.1007/978-3-031-41731-3_1; https://arxiv.org/abs/2305.02577
25. Quirós L., Vidal E. *Reading Order Detection on Handwritten Documents.* Neural Computing and Applications 34, 2022. DOI 10.1007/s00521-022-06948-5
26. Lin X., Gao L., Tang Z., Lin X., Hu X. *Mathematical Formula Identification in PDF Documents.* ICDAR 2011. DOI 10.1109/ICDAR.2011.285; http://www.iapr-tc11.org/archive/icdar2011/fileup/PDF/4520b419.pdf
27. Madisetty S., Maurya K.K., Aizawa A., Desarkar M.S. *A Neural Approach for Detecting Inline Mathematical Expressions from Scientific Documents.* Expert Systems 2021. DOI 10.1111/exsy.12576; https://kaushal0494.github.io/assets/pdf/papers/Expert_Systems_Camera.pdf . Cites Iwatsuki K., Sagara T., Hara T., Aizawa A., *Detecting In-line Mathematical Expressions in Scientific Documents*, DocEng 2017, DOI 10.1145/3103010.3121041 (not opened: blocked).
28. 林晓燕 (Lin Xiaoyan). 北京大学博士学位论文 (Peking University PhD thesis), 导师 汤帜, 2014. [Title not shown; PDF formula recognition and retrieval per abstract.] CQVIP record 00005EL0MDC83J9XMFR.
29. GROBID. *How GROBID works.* https://github.com/grobidOrg/grobid/blob/master/doc/Principles.md
30. DocLayNet dataset licence (CDLA-Permissive-1.0). https://github.com/DS4SD/DocLayNet/blob/main/LICENSE
31. Tamir Hassan. *ICDAR 2013 Table Competition* page and dataset. http://www.tamirhassan.com/html/competition.html (dataset licence not stated, unverified)

**Licence summary for training shipped models:**
- **Allowed:** DocLayNet (CDLA-Permissive-1.0), SciTSR repository (MIT), HURIDOCS code (Apache-2.0).
- **Ambiguous:** DocBank (Apache-2.0 stated, plus "do not redistribute").
- **Excluded:** ReadingBank (research-only, no redistribution); InftyReader and ABBYY (commercial baselines only).
- **Unverified:** ICDAR 2013, FinTOC, SectLabel/ParsCit data, the Iwatsuki dataset, and the Livathinos Kaggle set.

#### Where this touches RayoPDF
- `docs/research/algorithms.md` — the existing algorithm research notes; extend with the table-structure and line-classification findings above.
- `docs/development/roadmap.md` — where a "small learned re-scorer" item (tables first, then line roles) would be planned.
- `docs/development/evaluation.md` — the head-to-head furniture, heading-level and reading-order comparisons in §3 need eval protocol entries.
- `docs/research/landscape.md` — already cites DocBank and READoc; reconcile with the licence findings here.
- `docs/benchmarks/v0.2.0-2026-11-13.md` — baseline numbers (TEDS 0.618, MHS 0.8905) that any learned component must beat.
