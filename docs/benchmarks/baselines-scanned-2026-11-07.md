# OCR baselines: scanned suites, 2026-11-07

Reproduced 2026-11-07 with `tools/eval.py` (root `benchmark-output/eval-scanset`) on an AMD Ryzen 5
PRO 4650GE (12 logical CPUs), Docker 29.8.2, work cpuset `0-2,4,6-8,10`, `OMP_NUM_THREADS=4`,
shared with other jobs (load average 40-55 during the runs): the s/page column is a rough cost
indication, not a speed result.

**Suites.** `scanned`: 398 image-only PDFs (1,020 pages) made by `corpus --suite scanned` from the
199 valid synthetic items, each at 300 and 200 dpi; codec pages JPEG 211, CCITT G4 197, JBIG2 211,
JPEG 2000 202, Flate 199; degradations clean/noise/skew/blur balanced per dpi; 362 development
items, 36 holdout items (from the weasyprint producer, measured once here, aggregate only); all
398 valid; three builds reproduced every item's SHA-256. Ground truth = the fixture Markdown.
`olmocr-scans`: olmOCR-bench `old_scans` (98 PDFs, 526 tests) and `old_scans_math` (36 PDFs, 458
tests), revision `54a96a6`.

**Tool versions.** docling-ocr: docling 2.147.0 with default `OcrAutoOptions`, which selected
RapidOCR 3.10.0 on torch (onnxruntime not installed) with PP-OCRv6 small det + rec and the mobile
v2 angle classifier (`/models/RapidOcr`, image `rayopdf-eval-docling:1` id `1f70380d4615`).
tesseract: Tesseract 5.5.0 (Debian 5.5.0-1+b1, eng/osd 1:4.1.0-2, eng.traineddata SHA-256
`7d4322bd…`), pdftoppm 25.03.0 at 300 dpi, `--psm 1`, plain text. Suite tools: poppler 26.10.0,
Pillow 13.0.0, NumPy 2.5.4, libjpeg-turbo 3.2.1, libtiff 4.7.2, OpenJPEG 2.5.4, jbig2enc 0.30
(Leptonica 1.87.0), zlib 1.3.2 (Arch archive 2026-11-01). Text-layer tools as in the baselines.

### scanned, development items (362)

|tool|ok|NID|TEDS|MHS|construct F1 (all)|heading F1|heading level acc.|NID 300 dpi|NID 200 dpi|s/page|
|---|---|---|---|---|---|---|---|---|---|---|
|pdftotext|0|0.000|0.000|0.000|0.000|0.000|–|0.000|0.000|0.012|
|markitdown|0|0.000|0.000|0.000|0.000|0.000|–|0.000|0.000|0.019|
|pymupdf|0|0.000|0.000|0.000|0.000|0.000|–|0.000|0.000|1.791|
|opendataloader|0|0.000|0.000|0.000|0.000|0.000|–|0.000|0.000|–|
|liteparse|362|0.022|0.000|0.000|0.000|0.000|–|0.022|0.022|0.017|
|edgeparse|0|0.000|0.000|0.000|0.000|0.000|–|0.000|0.000|0.004|
|docling|55|0.005|0.000|0.000|0.000|0.000|–|0.005|0.006|2.917|
|docling-ocr|313|0.754|0.717|0.670|0.530|0.825|0.598|0.759|0.750|9.608|
|tesseract|362|0.856|0.000|0.013|0.219|0.000|–|0.857|0.854|4.709|

NID per codec: docling-ocr JPEG 0.867, CCITT 0.865, JBIG2 0.879, JPX 0.296, Flate 0.870;
tesseract 0.854, 0.844, 0.881, 0.838, 0.861. NID per degradation (clean/noise/skew/blur):
docling-ocr 0.745/0.770/0.744/0.760; tesseract 0.865/0.843/0.866/0.847. Construct F1 per
category, docling-ocr: heading 0.825, list_item 0.808, code_block 0.240, link 0.330, all others 0;
tesseract: list_item 0.490, link 0.346, all others 0.

docling-ocr writes nothing for every page whose image is a JP2-boxed JPEG 2000 (49 of 49
development JP2 items empty; the raw-J2K-codestream variant scores NID 0.899), while poppler and
pdfium render those pages. Without the 49 JP2 items (313 items): docling-ocr NID 0.873, TEDS 0.843,
MHS 0.782, construct F1 0.592; tesseract NID 0.860, TEDS 0.000, MHS 0.010, construct F1 0.221.
The text-layer tools output nothing (liteparse writes empty fenced blocks and page rules, docling
writes `<!-- image -->` placeholders).

### scanned, holdout items (36, aggregate only, measured once)

|tool|NID|TEDS|MHS|construct F1 (all)|
|---|---|---|---|---|
|text-layer tools and docling|≤ 0.025|0.000|0.000|0.000|
|docling-ocr|0.700|0.666|0.671|0.536|
|tesseract|0.847|0.000|0.015|0.236|

### olmocr-scans (olmOCR scorer, pass rate per JSONL)

|tool|old_scans|old_scans_math|mean|
|---|---|---|---|
|pdftotext, markitdown, pymupdf, opendataloader, liteparse, edgeparse, docling|0.133|0.000|0.067|
|docling-ocr|0.251|0.000|0.125|
|tesseract|0.211|0.000|0.106|

Empty output passes the 70 `absent` tests of old_scans (floor 0.133); old_scans_math has only
KaTeX `math` tests, which plain OCR text cannot pass. 15 of the 134 scans carry a text layer
(pdftotext is non-empty on them) and still pass nothing beyond the floor.
