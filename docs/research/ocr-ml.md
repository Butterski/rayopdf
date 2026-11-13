# rayopdf — OCR / ML decision research

Research note, 2026-11-05. Third-party numbers are as published unless marked reproduced; reproduced numbers are in `docs/benchmarks/`.

Status: research input for the rayopdf plan, compiled 2026-11-05. Scope: when a born-digital-looking PDF still needs OCR, which permissively licensed engines, models and runtimes a native C++17 binary could use, and a measurable decision gate.
Conventions: **[CLAIM]** marks a vendor or author claim that has not been independently reproduced. **[INFERENCE]** marks my own reasoning that no source states directly. All other statements are linked to a primary source.

---

## 0. Summary

- **Most "OCR-needed" pages in born-digital PDFs are broken font mappings, not images.** Missing or wrong ToUnicode, Type3 or subset fonts with meaningless glyph names, and Identity-H fonts without ToUnicode all fall into this group. Glyph-level repair can fix many of them without page OCR. The cache-per-glyph idea is proven by LANL's [poppler-science](https://github.com/lanl/poppler-science) (GPL, so the idea is reusable but the code is not).
- **Detection is cheap and well understood.** Docling's broken-text-layer check runs in under 1 ms per page ([PR #4654](https://github.com/docling-project/docling/pull/4654)). MinerU, OCRmyPDF, marker and pymupdf4llm publish their concrete signals and thresholds (§1.2).
- **ML layout gives the most benefit for tables, not reading order.** On [opendataloader-bench](https://github.com/opendataloader-project/opendataloader-bench) (200 PDFs), the rule-based opendataloader engine scores reading order 0.902, headings 0.739, tables 0.489. The hybrid with docling models scores 0.934, 0.821 and 0.928. Marker's own code says the PDF character stream beats learned reading order on multi-column pages (olmOCR-bench order tests: 75% vs 56%) ([marker line.py](https://github.com/datalab-to/marker/blob/master/marker/builders/line.py)).
- **Best permissive OCR candidates:** Tesseract 5.5.x (Apache-2.0, external CLI) and PaddleOCR PP-OCRv6 small/tiny (Apache-2.0 weights) via ONNX Runtime (MIT) or ncnn (BSD-3). VLMs set the quality ceiling, but none belongs inside a small static binary.
- **Exclude:** Surya weights (restricted OpenRAIL-M), MinerU2.5 weights (AGPL-3.0), DocLayout-YOLO and Ultralytics (AGPL-3.0), LayoutLMv3 (CC-BY-NC-SA-4.0), Nanonets-OCR2-3B (no licence declared; its base model uses the Qwen research licence).
- **Decision gate (§6):** measure the residual unrecoverable-text rate R and the rule-vs-ML deltas on our corpus.
  - R < 0.1% → no OCR.
  - 0.1–5% → optional external `tesseract` process.
  - ≥ 5%, or CJK, or a no-install requirement → linked PP-OCRv6 through a minimal static runtime.
  - Table ML only if the TEDS delta is ≥ 0.10 after our rule-based tables.

---

## 1. When OCR is unavoidable in born-digital-looking PDFs

### 1.1 Failure taxonomy

| Case | Observable signal in the PDF | Fixable without page OCR? |
|---|---|---|
| Image-only page (scan inside a "digital" PDF) | ~0 text operators; image XObjects cover most of the page | No. Needs OCR. |
| Image inserts (scanned pages or figures with text) on a digital page | Text layer good elsewhere; large image regions with ink | No for the region. Region OCR only. |
| Simple font, no ToUnicode, custom `/Differences` with meaningful names | Glyph names in the AGL, or `uniXXXX` | Yes. AGL lookup (pdf.js does this, see below). |
| Simple or Type3 font with meaningless names (`/g3`, `/a12`, `/C0042`) | Names not in the AGL | Partly: pdf.js `Gxx`/`gxx`/`Cdd` heuristics. Otherwise glyph-shape recovery (§1.4) or OCR. |
| CID font, Identity-H, no ToUnicode | Codes are glyph IDs; no Adobe registry/ordering | Possibly, via the embedded TrueType `cmap` reverse map **[INFERENCE: verify per font]**. Otherwise glyph-shape recovery or OCR. |
| Wrong ToUnicode (valid Unicode, wrong letters) | Text looks plausible but differs from the rendered glyphs | Shape checks only: [poppler-science](https://github.com/lanl/poppler-science) (µ→m, Word "ff"→"#"). Docling's checks cannot see it ([PR #4654](https://github.com/docling-project/docling/pull/4654)). |
| Existing invisible OCR layer of poor quality | Text render mode 3 (or 7) over a full-page image | Re-OCR, or keep it if a quality check passes. |
| Text drawn as vector outlines | Many small filled paths shaped like characters, no text operators | No. Region OCR. |

### 1.2 How existing tools detect these cases (signals and thresholds)

**pdf.js — Unicode recovery, no OCR** ([evaluator.js](https://github.com/mozilla/pdf.js/blob/master/src/core/evaluator.js), `buildToUnicode` / `_simpleFontToUnicode`)
- If ToUnicode is present, pdf.js uses it. For simple fonts it also builds `fallbackToUnicode` from the encoding, because "some fonts contain incomplete ToUnicode data".
- Simple fonts: base encoding plus `/Differences`, then the Adobe Glyph List.
- Then undocumented heuristics: `Gxx` (hex, length 3), `g00xx` (hex, length 5), `Cdd{d}`/`cdd{d}` (decimal; the whole encoding is re-parsed as hex if any non-decimal value appears), `uniXXXX`/`uXXXX{XX}`, and the ligatures `f_h`, `f_t`, `T_h`.
- The code comment says "Adobe Reader does not do this step, but OSX Preview does".
- Composite fonts: when a predefined CMap other than Identity is used, or the font is Adobe-GB1/CNS1/Japan1/Korea1, pdf.js maps through `Registry-Ordering-UCS2`. Otherwise it falls back to "the viewer's choice, just use an identity map". This is the source of garbage text from Identity-H fonts without ToUnicode.

**OCRmyPDF — page classification** ([pdfinfo/info.py](https://github.com/ocrmypdf/OCRmyPDF/blob/main/src/ocrmypdf/pdfinfo/info.py), [advanced.md](https://github.com/ocrmypdf/OCRmyPDF/blob/main/docs/advanced.md))
- Per-page flags:
  - `has_text`.
  - `has_visible_text`: text in an invisible render mode, or text in a glyphless font, does not count as visible.
  - `has_vector`: "Vector graphics are sometimes used to draw fonts".
  - `has_corrupt_text`: a text box is corrupt if its first character extracts as U+FFFD.
- Modes:
  - `default` exits if text is found. A `/StructTreeRoot` (tagged PDF) is also treated as "probably no OCR needed".
  - `skip` skips pages that have text.
  - `redo` strips invisible text, masks the visible text and OCRs the rest. It "cannot distinguish" OCR text that was painted over.
  - `force` rasterizes every page. The docs recommend it "for fixing OCR text with a damaged character map".
- Limits: Tesseract timeout 180 s per page by default. "A 300 DPI, 8.5×11" page image is 8.4 megapixels."

**PyMuPDF / pymupdf4llm** ([FAQ](https://pymupdf.readthedocs.io/en/latest/faq/index.html), [OCR guide](https://docs.pdf4llm.com/python/guides/OCR/index.md), [maintainer reply in #5068](https://github.com/pymupdf/PyMuPDF/discussions/5068))
- MuPDF emits U+FFFD for characters it cannot map ([PyMuPDF-Utilities tesseract1.py](https://github.com/pymupdf/PyMuPDF-Utilities/blob/master/OCR/tesseract1.py)).
- pymupdf4llm OCRs a page in two cases:
  - (a) It has roughly no text but is "covered with images or many character-sized vectors", and text is "probably" detectable.
  - (b) Too many characters are unreadable. Then only the affected areas are OCR'd.
- `analyze_page` returns `chars_total`, `chars_bad` (U+FFFD count), `txt_area`/`img_area`/`vec_area`, `vec_norects` ("vectors that MIGHT mimic characters"), `needs_ocr` and `reason`. It uses a probability threshold of 93%.
- In the maintainer's example, 1 bad character out of 676 → no OCR; 573 out of 742 → OCR (reason `chars_bad`).
- Since 1.27.2, partial OCR also covers vector regions. Tesseract then over-triggers on lines and rectangles (same thread).

**marker 2.x** ([line.py](https://github.com/datalab-to/marker/blob/master/marker/builders/line.py), [README](https://github.com/datalab-to/marker/blob/master/README.md)). A page keeps its embedded text only if all four checks pass:
1. It has provider lines.
2. A page-level OCR-error classifier (surya `OCRErrorPredictor`) does not label the text "bad".
3. Layout coverage: at least 25% of non-figure/table layout blocks contain at least 1 provider line.
4. Every line lies within the page bounding box plus 5 pt, and fewer than 50% of lines overlap more than two others (overlap fraction > 0.1). This catches duplicate OCR layers.

Block-level repair:
- Empty layout blocks covering ≥ 1% of the page area that contain ink are re-OCR'd.
- The garbled check runs only on blocks with ≥ 50 characters, and only on pages whose page-level P(bad) is ≥ 0.05.
- `fast` (CPU) mode promotes a page to full OCR when more than 50% of its blocks are bad. `balanced` promotes on any bad block.
- Licence: GPL-3.0 ([opendataloader-bench table](https://github.com/opendataloader-project/opendataloader-bench)).

**Docling** ([pipeline_options.py](https://github.com/docling-project/docling/blob/main/docling/datamodel/pipeline_options.py), [PR #4654](https://github.com/docling-project/docling/pull/4654), [issue #4834](https://github.com/docling-project/docling/issues/4834))
- The default `OcrMode` is `PDF_AWARE_LAYOUT_REGIONS`. It OCRs every layout cluster that overlaps a bitmap and skips clusters made only of PDF text cells. Pages are rendered at scale 3 (216 DPI).
- `bitmap_area_threshold` (OCR only when bitmaps cover ≥ X of the page) was removed in 2.116.0. Issue #4834 asks for it back.
- Opt-in `replace_broken_text_layer` (`is_broken_text_layer()`) checks visible text only. A page is flagged if any of these holds:
  1. Control, U+FFFD or private-use characters make up ≥ 3% of the page. A lone junk bullet at the start of a line is not counted.
  2. U+FFFD or control characters appear in ≥ 10% of lines and in ≥ 3 lines.
  3. ≥ 50% of Latin letters fall in U+00C0–U+024F (with ≥ 50 Latin letters). This is the signature of an 8-bit code page mis-decoded as Latin.
  4. The vowel share of Latin letters is < 0.25 (≥ 200 letters, words of 3–20 letters). This catches shifted maps such as `Wkh txduwhuo|`.
- Reported results:
  - Under 1 ms per page median.
  - On the DocLayNet v1.1 test split (4,948 pages), 18 pages were flagged, all in one document whose whole text layer was broken.
  - Stated misses: correct-alphabet-but-wrong-letters cases (Arabic with a wrong ToUnicode; Indic PDFs exported from Word).
- A commenter on the PR reports that more than half of 2,280 Arabic pages from 57 Shamela PDFs had broken text layers that this check missed. The patterns were decimal glyph codes, glyph names and wrong letters [CLAIM].
- Licence: MIT (SPDX header in the source).

**MinerU** ([pdf_classify.py @ 2.5.4](https://github.com/opendatalab/MinerU/blob/mineru-2.5.4-released/mineru/utils/pdf_classify.py)). Classification is per document, not per page:
- It samples up to 10 random pages.
- It returns `ocr` when any of these holds:
  - The average non-whitespace characters per page (pdfium) is < 50.
  - The pdfminer `(cid:N)` share is > 5%.
  - ≥ 80% of sampled pages have image coverage ≥ 0.8.
  - The PDF forbids extraction.

**olmOCR** ([filter.py](https://github.com/allenai/olmocr/blob/main/olmocr/filter/filter.py), [model card](https://huggingface.co/allenai/olmOCR-2-7B-1025))
- The filter selects documents for a corpus; it does not route OCR. It drops forms. On the first 5 pages it uses `pdftotext`, keeps documents with < 200 characters or alpha ratio < 0.50 ("might be OCRed badly"), and filters by language and SEO spam.
- The pipeline itself always renders the page (longest side 1288 px) and runs the VLM. The current card example uses a "no-anchoring" prompt.

### 1.3 Signal set for rayopdf (proposal: union of the above, all computable from the content stream)

1. Per page: visible text characters (render modes other than 3 and 7), image coverage ratio, and count/area of character-sized vector paths.
2. Per font: ToUnicode present? Encoding type, glyph names in the AGL or pdf.js-heuristic names, embedded font program type, Identity-H flag, share of mapped codes.
3. Per text run: U+FFFD, control and private-use share, `GLYPH<n>`/`(cid:n)` equivalents, docling checks 3 and 4, decimal-code runs and glyph-name runs (from the Shamela data in [PR #4654](https://github.com/docling-project/docling/pull/4654)).
4. Invisible-layer pages: text in render mode 3 over an image covering ≥ 0.8 of the page. Its quality is unknown until checked (§5).

### 1.4 Glyph-shape recovery instead of page OCR — prior art

| Project | Method | Licence | Evidence |
|---|---|---|---|
| [lanl/poppler-science](https://github.com/lanl/poppler-science) | Renders every font glyph to a small bitmap; a 2-layer MLP (3000/2000 nodes) predicts Unicode; results cached per glyph per file; C++ SIMD CPU inference | GPL-3.0 (do not reuse code) | Trained on 650,544 glyph bitmaps from 685,100 PMC Open Access PDFs; 862 classes; weights ~89 MB [CLAIM] |
| [PyMuPDF-Utilities tesseract1.py](https://github.com/pymupdf/PyMuPDF-Utilities/blob/master/OCR/tesseract1.py) | OCRs only spans containing U+FFFD: renders the bbox at 4×, runs `tesseract --psm 7` | AGPL ecosystem (pattern only) | Demo script |
| [shreevatsa/pdf-glyph-mapping](https://github.com/shreevatsa/pdf-glyph-mapping) | Dumps the glyphs of each font; a human maps them; writes `/ActualText` | MIT | Shows Devanagari glyph→Unicode is many-to-many and in visual order, so per-glyph classification alone fails for complex scripts |
| [jedarden/pdftract design doc](https://github.com/jedarden/pdftract/blob/main/docs/research/glyph-recognition-and-unicode-recovery.md) | Cascade ToUnicode → AGL → font-hash database → glyph pHash nearest-neighbour | n/a | Design spec only; no accuracy data |

Recovery path for rayopdf **[INFERENCE]**. Key the cache on (font object, glyph ID).
1. Run the pdf.js-style name heuristics.
2. Use the embedded TrueType/CFF `cmap` or `post` names.
3. Classify the glyph shape once per unique glyph. Either use a small classifier, or render all unique glyphs of a font as one synthetic text line and pass it through the OCR recognizer once.
4. Score the result with a script/character n-gram check (docling's vowel-share idea).

The cost scales with the number of unique glyphs (typically hundreds per font), not with pixels per page. Complex scripts (Indic, Arabic shaping) need the cluster-level page fallback.

---

## 2. OCR engines usable from C/C++

### 2.1 Classic engines

| Engine | Licence | C/C++ integration | Size | Speed (source hardware) | Accuracy / languages |
|---|---|---|---|---|---|
| [Tesseract 5.5.4](https://github.com/tesseract-ocr/tesseract/releases/tag/5.5.4) | Apache-2.0; [tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast) data Apache-2.0; [Leptonica](http://www.leptonica.org/about-the-license.html) BSD-2-style | Native C++ API or CLI. Optional libarchive/libcurl/libtiff can be disabled; OpenMP off by default ([CMakeLists](https://github.com/tesseract-ocr/tesseract/blob/main/CMakeLists.txt)) | eng LSTM component in a best-style model: 11.7 MB + 3.7 MB word dawg ([Data-Files](https://github.com/tesseract-ocr/tessdoc/blob/main/Data-Files.md)); fast models are "integerized … smaller network" | 2017 measurements ([tessdoc](https://github.com/tesseract-ocr/tessdoc/blob/main/tess4/4.0-Accuracy-and-Performance.md)): LSTM 1.5 s wall / 2.5 s CPU; one Hindi page 1.8 s (OpenMP+AVX), 2.7 s (AVX, no OpenMP), 4.6 s (no SIMD) | ~100+ languages and script models; Latin trained on ~400k lines × ~4,500 fonts ([tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast)); no modern published benchmark found in primary docs |
| [PP-OCRv5](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/algorithm/PP-OCRv5/PP-OCRv5.en.md) mobile/server | Apache-2.0 | ONNX via [RapidOCR](https://github.com/RapidAI/RapidOCR), ORT, OpenVINO, ncnn | det 4.7 / 84.3 MB; rec 16 / 81 MB ([det](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/text_detection.en.md), [rec](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/text_recognition.en.md)) | Xeon 8350C, ORT end-to-end: mobile 0.61 s/image, server 6.36 s/image ([PP-OCRv6 doc](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/algorithm/PP-OCRv6/PP-OCRv6.en.md)) [CLAIM] | Printed-English rec 0.875 (mobile) [CLAIM]; separate rec models for Latin, Cyrillic, East Slavic, Arabic, Devanagari, Thai, Greek, Tamil, Telugu, Korean |
| [PP-OCRv6](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/algorithm/PP-OCRv6/PP-OCRv6.en.md) tiny/small/medium (1.5M–34.5M params) | Apache-2.0 ([HF card](https://huggingface.co/PaddlePaddle/PP-OCRv6_small_rec)) | Same as v5 | det 1.9 / 9.6 / 59.4 MB; rec 4.4 / 20.4 / 73.3 MB | Xeon 8350C, ORT: tiny 0.22 s, small 0.61 s, medium 3.31 s (OpenVINO 1.40 s) per image [CLAIM] | One model covers 50 languages: CJK + 46 Latin-script (tiny: 49, no Japanese). Printed-EN rec 93.3 (small), 88.4 (tiny) on an internal benchmark [CLAIM]; v6 and v5 metrics are "not directly comparable" |
| [RapidOCR](https://github.com/RapidAI/RapidOCR) | Apache-2.0 | Paddle models converted to ONNX; ORT, OpenVINO, MNN back ends; C++ bindings live in separate repos | — | — | Default OCR engine in docling ([discussion #180](https://github.com/opendataloader-project/opendataloader-pdf/discussions/180)) |
| [docTR](https://github.com/mindee/doctr/blob/main/LICENSE) | Apache-2.0 | PyTorch; C++ only via ONNX export **[INFERENCE]** | — | — | — |
| [EasyOCR](https://github.com/JaidedAI/EasyOCR/blob/master/LICENSE) | Apache-2.0 | PyTorch | — | — | 80+ languages |
| [Surya](https://github.com/datalab-to/surya) | Code Apache-2.0; **weights: modified AI Pubs OpenRAIL-M** (free only below $5M funding/revenue) | Served by vLLM or llama.cpp | 689M params | 4.5 pages/s on H100 [CLAIM] | 83.7% olmOCR-bench [CLAIM]; **weights not acceptable for us** |

Security note: Tesseract 5.5.4 fixed 17 security advisories ([release](https://github.com/tesseract-ocr/tesseract/releases/tag/5.5.4)). Running it as a separate process isolates parser crashes on untrusted input **[INFERENCE]**.

### 2.2 VLM OCR (quality ceiling; not for the binary)

olmOCR-bench overall scores come from the [olmocr README](https://github.com/allenai/olmocr/blob/main/README.md); rows marked * there were self-reported by the authors.

| Model | Params | Licence (weights) | olmOCR-bench | Hardware / notes |
|---|---|---|---|---|
| [olmOCR-2-7B-1025](https://huggingface.co/allenai/olmOCR-2-7B-1025) | 7B (Qwen2.5-VL-7B) | Apache-2.0 | 82.4 (v0.4.0 pipeline) | README: "requires a GPU" |
| [Chandra](https://huggingface.co/datalab-to/chandra) | — | `openrail` | 83.1 (0.1.0*) | Check terms before use |
| [PaddleOCR-VL](https://huggingface.co/PaddlePaddle/PaddleOCR-VL) | 0.9B (NaViT + ERNIE-4.5-0.3B) | Apache-2.0 | 80.0* | 109 languages; layout by PP-DocLayoutV2/V3; version 1.6 now exists |
| [DeepSeek-OCR](https://huggingface.co/deepseek-ai/DeepSeek-OCR) | — | MIT | 75.7 | — |
| [dots.ocr](https://huggingface.co/rednote-hilab/dots.ocr) | 1.7B LLM | MIT | — | Claims SOTA on OmniDocBench [CLAIM] |
| [GOT-OCR2.0](https://huggingface.co/stepfun-ai/GOT-OCR2_0) | 580M ([paper](https://arxiv.org/abs/2409.01704)) | Apache-2.0 | — | — |
| [MinerU2.5](https://huggingface.co/opendatalab/MinerU2.5-2509-1.2B) | 1.2B | **AGPL-3.0** | 75.2 (MinerU 2.5.4*) | Excluded |
| [Nanonets-OCR2-3B](https://huggingface.co/nanonets/Nanonets-OCR2-3B) | 3B | **None declared**; base [Qwen2.5-VL-3B](https://huggingface.co/Qwen/Qwen2.5-VL-3B-Instruct) is `qwen-research` | 69.5 | Excluded |
| [Mistral OCR](https://docs.mistral.ai/models/ocr-4-1) | — | Proprietary API | 72.0 (older API version) | OCR 4.1: $4 per 1,000 pages; OCR 3: $2 per 1,000 ([pricing](https://docs.mistral.ai/inference/pricing)) |
| Marker 1.10.1 (pipeline) | — | GPL-3.0 | 76.1 | — |

---

## 3. ML layout and table-structure models

| Model | Licence | Size / params | CPU latency (source) | Published accuracy |
|---|---|---|---|---|
| [PP-DocLayout-L/M/S](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/layout_detection.en.md) | Apache-2.0 | 123.8 / 22.6 / 4.8 MB | 503 / 43 / 18.5 ms (normal mode; Xeon Gold 6271C, 8 threads) | mAP@0.5 90.4 / 75.2 / 70.9 on Paddle's own 23-class set (not DocLayNet) [CLAIM] |
| PP-DocLayout_plus-L (PP-StructureV3 layout) | Apache-2.0 | 126 MB | 635 ms | mAP@0.5 83.2 (own set) [CLAIM] |
| [PP-DocLayoutV3](https://huggingface.co/PaddlePaddle/PP-DocLayoutV3) | Apache-2.0 | — | — | Multi-point boxes plus reading order; built for PaddleOCR-VL 1.5/1.6 |
| [Docling heron](https://huggingface.co/docling-project/docling-layout-heron) (RT-DETRv2-r50) | Apache-2.0 | 42.9M params | 0.643 s/image (AMD EPYC 7763, 4 threads, batch 32) ([paper](https://arxiv.org/html/2509.11720)) | DocLayNet mAP 0.699 raw, 0.776 on canonical DocLayNet |
| Docling egret-m / heron-101 | Apache-2.0 | 19.5M / 76.7M params | 0.334 / 0.988 s/image | Canonical DocLayNet: heron-101 0.780 |
| [TableFormer](https://huggingface.co/docling-project/docling-models) | CDLA-Permissive-2.0 / Apache-2.0 | — | — | TEDS 93.6 (all tables) [CLAIM] |
| [SLANet_plus](https://github.com/PaddlePaddle/PaddleOCR/blob/main/docs/version3.x/module_usage/table_structure_recognition.en.md) / SLANeXt | Apache-2.0 | 6.9 MB / 351 MB | 41.8 ms / 502 ms (high-performance mode) | 63.69% / 69.65% on a hard Chinese table set [CLAIM] |
| [DocLayout-YOLO](https://github.com/opendatalab/DocLayout-YOLO) | **AGPL-3.0** | — | — | DocLayNet mAP 79.7 (pretrained). **Excluded** |
| Ultralytics YOLO | **AGPL-3.0** or paid enterprise licence ([licence](https://www.ultralytics.com/license)) | — | — | **Excluded** |
| [LayoutLMv3](https://huggingface.co/microsoft/layoutlmv3-base) | **CC-BY-NC-SA-4.0** | — | — | **Excluded** |

**Do they help born-digital PDFs beyond rule-based layout?**
- [opendataloader-bench](https://github.com/opendataloader-project/opendataloader-bench) uses 200 PDFs and scores reading order (NID), tables (TEDS) and headings (MHS). Results:
  - opendataloader, rule-based: 0.902 / 0.489 / 0.739 (overall 0.831) at 0.015 s/page.
  - opendataloader hybrid: 0.934 / 0.928 / 0.821 (overall 0.907) at 0.463 s/page. The hybrid uses docling heron plus TableFormer on triaged pages.
  - docling: overall 0.882.
  - nutrient (commercial): overall 0.885, tables 0.708, at 0.008 s/page.
  - pymupdf4llm: overall 0.732.
- Takeaway: ML adds about +0.44 TEDS on tables but only +0.03 NID on reading order. Heading levels gain +0.08.
- Marker's own code orders blocks by the PDF character stream because it "beats learned reading-order models on multi-column pages (olmocr-bench order tests: 75% vs 56%)" ([line.py](https://github.com/datalab-to/marker/blob/master/marker/builders/line.py)).
- Docling post-processes layout boxes by snapping them to native PDF cells ([heron paper](https://arxiv.org/html/2509.11720)). ML layout there builds on the text layer rather than replacing it.
- [OmniDocBench](https://github.com/opendatalab/OmniDocBench) scores Markdown generated from page **images**. Its pipeline-vs-VLM rankings (e.g. docling EN overall edit 0.589 vs PP-StructureV3 0.145 in the [dots.ocr table](https://huggingface.co/rednote-hilab/dots.ocr)) therefore do not measure text-layer extraction **[INFERENCE]**.
- The olmOCR-bench published table has no pure text-layer baseline ([bench README](https://github.com/allenai/olmocr/blob/main/olmocr/bench/README.md)). We must run our own.

Updated 2026-11-13: [small-ml.md](small-ml.md) surveys small learned models that read the text
layer instead of page images (line roles, table structure from word positions, furniture,
reading order), their training data and licences, and which fit the core (tiny decision trees) or
the add-on (PP-DocLayout-S, SLANet_plus). [layout-math.md](layout-math.md) has the statistical
methods that replace fixed thresholds and the formula-structure algorithms.

---

## 4. Inference runtimes for a small static C++ binary

| Runtime | Licence | Static / size facts | Windows + Linux | Notes |
|---|---|---|---|---|
| [ONNX Runtime](https://onnxruntime.ai/docs/build/custom.html) | MIT | `--minimal_build` (ORT format only), `--include_ops_by_config`, `--enable_reduced_operator_type_support`, `--disable_exceptions/--disable_rtti`, `MinSizeRel`. Custom build for ResNet50 ops: x86_64 `.so` 4,240,864 B vs 18,222,208 B prebuilt (v1.18, [mobile doc](https://onnxruntime.ai/docs/tutorials/mobile)). Static build: omit `--build_shared_lib`; this produces many `.a` files to merge ([discussion #16799](https://github.com/microsoft/onnxruntime/discussions/16799)) | Yes; watch MSVC runtime mismatch ([#15928](https://github.com/microsoft/onnxruntime/issues/15928)) | Int8 quantization cuts model size ~4× ([mobile doc](https://onnxruntime.ai/docs/tutorials/mobile)) |
| [ncnn](https://github.com/Tencent/ncnn) | BSD-3 | "No third-party runtime dependencies and no BLAS"; prebuilt static Windows packages plus `-shared` variants; int8; `pnnx` converts ONNX/PyTorch | Yes (MSVC, MinGW) | Smallest dependency surface; library size not verified |
| [OpenVINO](https://github.com/openvinotoolkit/openvino/blob/master/docs/dev/static_libraries.md) | Apache-2.0 | `-DBUILD_SHARED_LIBS=OFF` plus conditional compilation. **TBB stays a shared library**; static TBB "not a recommended configuration" | Windows x64, Linux x64 | Fastest for PP-OCRv6-medium on Xeon (1.40 s vs ORT 3.31 s) [CLAIM]; breaks the single-binary goal |
| [ggml](https://github.com/ggml-org/ggml) | MIT | "Plain C/C++ implementation without any dependencies"; x86/ARM SIMD | Yes | Path for VLMs (Surya uses llama.cpp on CPU); overkill for CNN OCR |
| Hand-written int8 | Ours | Smallest possible | Yes | Feasible for PP-OCRv6 tiny/small (conv + light attention + CTC) **[INFERENCE]**; high maintenance cost |

---

## 5. Hybrid designs

| Tool | Granularity | Routing rule |
|---|---|---|
| OCRmyPDF | Page | Modes skip/redo/force; redo strips invisible text and OCRs masked images ([advanced.md](https://github.com/ocrmypdf/OCRmyPDF/blob/main/docs/advanced.md)) |
| docling | Layout region (page with opt-in `replace_broken_text_layer`) | OCR clusters that overlap bitmaps; page OCR on a broken text layer |
| marker | Page, or block in `fast` mode | OCR-error classifier plus coverage/overlap checks (§1.2) |
| MinerU | Whole document | `txt` vs `ocr` from a 10-page sample |
| pymupdf4llm | Page → affected regions | `analyze_page` (§1.2) |
| opendataloader hybrid | Page | Triage: table borders, vector grids, text-table patterns, large images or a high line-chunk ratio go to the docling back end; "simple text-only pages (the majority)" stay in Java at ~0.05 s ([discussion #180](https://github.com/opendataloader-project/opendataloader-pdf/discussions/180)) |

**External binary vs linked library**
- Prior art for the external binary: OCRmyPDF finds `tesseract` on `PATH` and honours `TESSDATA_PREFIX` ([advanced.md](https://github.com/ocrmypdf/OCRmyPDF/blob/main/docs/advanced.md)). Docling ships `TesseractCliOcrOptions` with `tesseract_cmd` ([pipeline options](https://docling-project.github.io/docling/reference/pipeline_options)). PyMuPDF pipes PNGs to `tesseract stdin stdout` ([tesseract1.py](https://github.com/pymupdf/PyMuPDF-Utilities/blob/master/OCR/tesseract1.py)).
- External binary, pros **[INFERENCE]**: 0 bytes added to rayopdf; licence and crash isolation; the user manages language packs.
- External binary, cons **[INFERENCE]**: one process spawn per call (batch whole pages or regions into one TSV/hOCR call); absent by default on Windows; output varies by version.
- Linked library, pros: deterministic, offline, works with no install.
- Linked library, cost: runtime plus model bytes in the binary, and a native attack surface.

**Existing invisible OCR layers.** Keep the layer when it passes the §1.3 checks and an OCR-error check. Re-OCR when it fails or when a sampled comparison with fresh OCR disagrees **[INFERENCE]** (marker uses an OCR-error classifier; OCRmyPDF `redo` always re-OCRs).

---

## 6. Decision gate for rayopdf

### 6.1 Corpus

- A stratified sample of target documents: reports, papers, slides, forms, CJK, RTL and Indic if relevant.
- Plus [opendataloader-bench](https://github.com/opendataloader-project/opendataloader-bench) (200 PDFs).
- Plus the born-digital olmOCR-bench categories (ArXiv, Headers & footers, Multi-column, Long tiny text, Tables) ([bench](https://github.com/allenai/olmocr/tree/main/olmocr/bench)).
- Ground truth: the benchmark annotations. Use VLM output (PaddleOCR-VL or olmOCR-2, §2.2) only as the reference ceiling on unlabeled pages.

### 6.2 Measurements

| ID | Metric | How |
|---|---|---|
| M1 | Image-only page rate | Visible characters < 50 and image coverage ≥ 0.8 (MinerU thresholds), or character-sized vector paths with no text |
| M2 | Broken text-layer page/region rate | Docling's four checks, plus `(cid:)`/`GLYPH<>`/private-use/decimal-code/glyph-name runs, plus per-font unmapped-code share |
| M3 | Share of M2 fixed without OCR | After AGL plus pdf.js heuristics, embedded-font `cmap`, and glyph-shape recovery (§1.4); verify against ground truth |
| R | Residual OCR need | M1 + (M2 − M3) + text-bearing image or vector regions on digital pages, as pages-equivalent / total pages |
| S | Script mix of R | Latin / Cyrillic / CJK / Arabic / Indic share |
| Q1 | Reading-order delta | NID: rayopdf rule-based vs docling-hybrid (heron + TableFormer) |
| Q2 | Table delta | TEDS on table-bearing pages, same pair |
| Q3 | Heading delta | MHS, same pair |
| T | Cost | ms/page for text-layer pages; s/page for OCR pages on the reference CPU |

### 6.3 Branches (thresholds are our proposal; calibrate on the first corpus run)

**(a) No OCR.** Condition: R < 0.1% of pages.
- Ship text layer plus font repair plus glyph recovery.
- Mark every unrecoverable page or region explicitly in the Markdown, e.g. `<!-- rayopdf: page 7 has no extractable text -->`. Never drop content silently.
- Binary cost: the glyph classifier only, **[INFERENCE]** < 1 MB if int8.

**(b) External OCR binary.** Condition: 0.1% ≤ R < 5% and S is mostly Latin or Cyrillic.
- `--ocr=auto|off|force`. Auto-detect `tesseract` (PATH or env override), send crops or pages over stdin, read TSV.
- If tesseract is missing, emit the (a) marker plus a hint.
- Engine: Tesseract 5.5.x with `tessdata_fast` (Apache-2.0).
- Expected cost: about 1.5–4.6 s per OCR page (2017 measurements, §2.1); 0 MB added.

**(c) Linked OCR.** Condition: R ≥ 5%, or S includes CJK, or the user needs OCR on Windows with nothing installed, or OCR pages need < 1 s.
- Engine: PP-OCRv6 small (det 9.6 + rec 20.4 MB fp32), or tiny (1.9 + 4.4 MB) for size. Int8 brings either down roughly 4× **[INFERENCE]**.
- Runtime: ONNX Runtime minimal static build with a reduced operator config (~4 MB by analogy with ResNet50 **[INFERENCE]**), or ncnn if the static ORT merge proves fragile.
- Expected speed: 0.22 s (tiny) to 0.61 s (small) per image on a Xeon 8350C [CLAIM]. OCR runs only on routed pages or regions, so text-layer pages keep sub-millisecond speed.
- Keep models as optional side files, or embed only tiny int8, so the default binary stays small **[INFERENCE]**.

**ML layout gate (independent of OCR).**
- Tables: adopt table ML (SLANet_plus 6.9 MB, or TableFormer via ORT) only if Q2 ≥ 0.10 TEDS after our rule-based table engine is tuned. Route only triaged pages, following the opendataloader pattern.
- Reading order and headings: adopt only if Q1 ≥ 0.03 or Q3 ≥ 0.05. Current evidence predicts no (§3).
- Heron (0.64 s/page CPU) is acceptable only behind triage.

**VLMs.** Use them as evaluation references only (PaddleOCR-VL Apache-2.0, olmOCR-2 Apache-2.0, DeepSeek-OCR MIT). Never ship them in the binary.

### 6.4 Re-evaluation triggers

Re-run the gate on new corpora, after glyph-recovery improvements (they lower R), and when PP-OCR or Tesseract release major versions.
