# OCR add-on engine (`rayopdf-ocr`)

The OCR add-on is a separate executable, `rayopdf-ocr` (`rayopdf-ocr.exe` on Windows), plus its
model folder `rayopdf-ocr-models/<tier>/`. The core `rayopdf` starts it as `rayopdf-ocr --serve` for
pages its routing sends to OCR and talks [protocol 1](ocr-protocol.md) with it over pipes. The core
never links the add-on, the inference runtime or the models. The decision to use linked PP-OCRv6 is
[ocr_ml_decision.md](ocr_ml_decision.md).

**Recommendation (measured below):**
- runtime: **ncnn**;
- tier: **PP-OCRv6 small**, with **tiny's recogniser as a first pass**: small detects and re-reads
  the lines tiny reads with less than 0.995 confidence or with CJK text (OCR speed, below);
- detection input: longer side 1280;
- no second engine for accuracy: measured in Phase D, it stays far below its gate.

The `tiny` tier alone is the option for size. ONNX Runtime (identical text, a few percent faster on
Linux, but no MinGW-w64 build), the `medium` tier (not worth its cost) and an external Tesseract
backend were measured (History) and removed in 1.1.

## Code

|Part|Files|
|---|---|
|Pixels: resampling (box filter shrinking, bilinear growing), rotated crops, normalisation|`src/ocr/raster.*`|
|Detection post-processing (DB): binarise at `det_thresh`, 8-connected components from row runs, convex hull, minimum-area rectangle, box score, unclip, line order|`src/ocr/detect.*`|
|Recognition post-processing: dictionary, greedy CTC, space recovery, character spans, confidence|`src/ocr/recognize.*`|
|The engine (`Engine`): PP-OCRv6 through a `Runtime`|`src/ocr/engine.*`|
|Model folder: `models.txt` (SHA-256 and sizes), `config.txt`, `dict.txt`|`src/ocr/models.*`|
|Protocol server (frames through `src/common/ocr_protocol.*`)|`src/ocr/server.*`|
|Executable and the ncnn runtime (only with `-DRAYOPDF_OCR_ADDON=ON`)|`src/ocr/addon/main.cpp`, `runtime_ncnn.cpp`|
|Build image: ncnn from pinned source, models downloaded and converted|`docker/ocr.Dockerfile`, `docker/ocr_models.py`|
|Tests|`tests/test_ocr.cpp` (1250–1299), `tests/fuzz/fuzz_ocr_server.cpp`, `tests/verify_ocr.py`|

`src/ocr/*.cpp` is the static library `rayopdf_ocr`. It depends on `common` and `crypt` (SHA-256
for `--verify`), and holds no third-party code. It is built in every configuration, so the core
tests, ASan and the fuzzer cover it without a runtime: `tests/ocr_fake_runtime.h` stands in for
the networks.

## One page

1. **Detection input.** The page is resampled so that its longer side is at most 1280 pixels; it is
   never enlarged. Each side is rounded to a multiple of 32, and the pixels are normalised with
   ImageNet's mean and deviation. The network returns a text-probability map of the same size.
2. **Boxes.** The map is binarised at `det_thresh` (0.2). Each 8-connected component gives the
   minimum-area rectangle of its convex hull. A rectangle is dropped if:
   - it is thinner than 3 map pixels; or
   - its mean probability is below `det_box_thresh` (0.45; tiny 0.4).

   The rest grow by `area × det_unclip_ratio (1.4) / perimeter` on every side (PaddleOCR's
   DBPostProcess). A rectangle still thinner than 5 is dropped. Survivors are scaled to page pixels.
   The thresholds come from each model's `inference.yml`, through `config.txt`. Work is bounded:
   - at most 3,000 components;
   - box scores read the map at most 8 times in total, then fall back to the component mean.
3. **Order.** Rows top to bottom, left to right within a row. The core orders lines itself.
4. **Crops.** Each rectangle is cut out of the full-resolution page at its own angle, so skew is
   handled. A crop at least 1.5 times taller than wide is turned a quarter turn (vertical text).
   A crop is at most 192 × 12,800 samples.
5. **Recognition.** The crop is resampled to height 48 (at most 3,200 wide). It is padded to at
   least 320 columns, as PaddleOCR does, and normalised to [−1, 1]. The output is decoded with
   greedy CTC; the lowest class index wins a tie.

   **Tiny first pass** (the default; `--first-pass none` turns it off, `--tier tiny` has none).
   Every line is read by tiny's recogniser first, in reading order. Small's recogniser reads the
   line again, and its reading replaces tiny's, when:
   - tiny's confidence is below 0.995; or
   - tiny's text holds a CJK character (U+2E80–U+9FFF, U+F900–U+FAFF, U+FF00–U+FFEF), because
     tiny's dictionary has no kana and only 6,174 of small's 15,565 Han.

   Once at least 8 lines of a page are read and more than half of them were read again (a
   degraded scan), the page's remaining lines go to small alone. Detection stays small's, so the
   line boxes, with the bullets and indents the core reads from them, are the tier's own. `lines`
   requests (glyph shapes) always use small's recogniser.
6. **Space recovery.** Greedy decoding drops spaces whose probability is spread over several blank
   steps (PP-OCRv6 writes "andit", "revenueup"). Between two characters, a space is inserted when
   `1 − Π(1 − p_space)` over the gap's steps is at least 0.25. Inserted spaces do not count in the
   confidence.
7. **Output.** Each character's span runs midway between neighbouring characters' step centres,
   mapped back onto the page's x axis. Confidence is the mean of the decoded characters' best
   probabilities. Lines without text are left out.

Each request runs on one thread. The same request gives the same response bytes:
- the server keeps no state that changes results;
- ncnn runs with one thread;
- tests check determinism (codes 1276, 1288), as do the fuzz target and `verify_ocr.py`.

Limits:
- protocol limits (frame, pixels, lines);
- detection side at most 4,096, 4,096 lines per page;
- crop and recognition widths as above;
- a response whose header would exceed 1 MiB is answered `too-large`.

**Scripts and orientation** (round 7, 2026-11-08): § Scripts and rotated pages below.

## Build, run, package

```
python3 tools/dev.py image --name ocr             # rayopdf-ocr:2 (once; ~6 min, ncnn's two builds dominate)
python3 tools/dev.py test --config ocr            # static-pie build/ocr/addon/rayopdf-ocr (ncnn) + all tests + tests/verify_ocr.py
python3 tools/dev.py test --config ocr-windows    # MinGW-w64 build/ocr-windows/addon/rayopdf-ocr.exe (ncnn), tests under Wine
python3 tools/dev.py --image rayopdf-ocr:2 run -- sh -c 'rm -rf build/ocr-models && cp -r /opt/ocr/models build/ocr-models'
python3 tools/package.py --only ocr-linux         # or --ocr: rayopdf-ocr-VERSION-*.tar.gz/.zip with rayopdf-ocr-models/tiny/ and small/
```

`rayopdf-ocr --serve [--models DIR] [--tier tiny|small] [--first-pass tiny|none] [--scripts on|off] [--orient on|off]`:
- models default to `rayopdf-ocr-models/` beside the executable; the default reads `small/` and,
  for the first pass, `tiny/`;
- `--image PAGE.pgm --repeat N` times one page;
- `--verify` checks the model files' SHA-256 against `models.txt` (both folders with a first pass).

Exit codes:

|Code|Meaning|
|---|---|
|0|quit or end of input|
|2|usage|
|3|models|
|4|runtime|
|5|unreadable frame|
|6|output gone|
|7|image|
|8|engine|

The build puts the add-on in `build/<dir>/addon/`. Discovery looks beside the running executable,
and the core tests in the same build directory must not start the real add-on. An add-on archive
unpacks into the same folder as the core archive, so `rayopdf-ocr` lands beside `rayopdf`, where
the core looks for it.

Pinned inputs (`/opt/ocr/versions.json` in the image):
- ncnn 20261020 (source zip, SHA-256): built static, without OpenMP or Vulkan, only the layers the
  networks use (68 of 110 layers off);
- pnnx 20261020;
- PaddlePaddle's official PP-OCRv6 ONNX exports, by Hugging Face revision and SHA-256
  (`third_party/licenses/pp-ocrv6.txt`), converted to ncnn by pnnx.

## Open items

- **Reading order of multi-column pages** is left to the core.
- **Hebrew** (§ Scripts and rotated pages): a permissive model exists, its ncnn conversion does not
  work yet.
- **Right-to-left text on turned pages through the core**: the core reorders right-to-left lines
  only in direction 0 (`src/layout/lines.cpp`), so Arabic and Hebrew pages turned a quarter or
  upside down come out in visual order.

## History

Dated measurement rounds, oldest first. The sections above describe the engine as it is.

### Measurements (B2, B4), 2026-11-07/08

**Setup.**
- Machine: AMD Ryzen 5 PRO 4650GE; Docker image `rayopdf-ocr:1`.
- Suite: ScanSet's `scanned` suite, development items only (362 items, 908 pages). These are the
  synthetic fixtures rendered at 200 and 300 dpi and encoded as JPEG, CCITT G4, JBIG2, JPEG 2000 and
  Flate, clean or with noise, blur or skew.
- Pages were rendered with `pdftoppm -r <dpi> -gray`, which gives the encoded pixels 1:1, and sent to
  the add-on directly. Each item ran as one `--serve` process over pipes, as the core does.
- Speed: CPU 3 alone (`--cpuset-cpus=3 --cpus=1`), one thread, `--image --repeat 3` with the median
  per page. The timed sample is the first page of every 9th item: 41 pages, 300 and 200 dpi.
- Startup: process start to handshake (models loaded), median of 5.
- Peak RSS: one `--serve` process over 5 pages at 300 dpi.
- Accuracy runs: the work CPUs.

**Accuracy metrics.**
- *Word F1:* multiset overlap of words. It ignores order, so it measures recognition alone.
- *CER:* Levenshtein distance over GT characters. GT is the fixture Markdown through
  `pandoc -t plain`, NFKC, with whitespace collapsed. CER is given as the per-item mean and pooled
  over all characters. It depends on reading order.
- *NID:* the harness's odl NID on the add-on's raw text (lines joined) against the fixture Markdown.
  It is low in absolute terms because the text carries no Markdown.

The core's own numbers through `--ocr=auto` are the parent's B3.

|Runtime|Tier|Exe Linux, stripped (B)|Exe Windows, stripped (B)|Models (B)|ms/page median (p95)|Startup ms|Peak RSS (MiB)|Word F1|CER mean / pooled|NID|
|---|---|---|---|---|---|---|---|---|---|---|
|ONNX Runtime|tiny|7,228,184|not buildable (MinGW)|6,442,485|815 (1,692)|20|455|0.9342|0.1263 / 0.0962|0.8632|
|ncnn|tiny|7,310,064|6,504,960|6,185,082|826 (1,857)|7|813|identical text|||
|ONNX Runtime|small|7,228,184|–|31,351,278|2,498 (6,578)|64|710|0.9401|0.1241 / 0.0945|0.8700|
|ncnn|small|7,310,064|6,504,960|30,987,999|2,739 (7,512)|21|1,344|identical text|||
|ONNX Runtime|medium|7,228,184|–|138,934,407|11,940 (30,005)|237|1,697|0.9393|0.1242 / 0.0950|0.8704|
|ncnn|medium|7,310,064|6,504,960|138,516,943|12,873 (34,399)|74|3,885|identical text|||
|Tesseract 5.3.4 `eng` (external, `--engine tesseract`)|–|(same add-on)|(same add-on)|not shipped|881 (1,953)|10|73 (add-on and tesseract)|0.9417|0.1359 / 0.0718|0.8571|

The rows above use detection side 1536 and the ncnn defaults (winograd and sgemm on), which were the
first settings.

The ncnn executables are pruned to the models' layers. Before pruning they were 10,222,320 B
(Linux) and 9,351,168 B (Windows). The core `rayopdf` is not affected.

**Runtimes.**
- ncnn and ONNX Runtime return byte-identical text:
  - small: all 362 items and 29,499 lines;
  - tiny and medium (pruned ncnn): every 15th item.
- ONNX Runtime is 1–9 % faster and needs about half the peak memory with ncnn's default convolution
  algorithms. ncnn starts 3× faster and builds for Windows.
- ncnn's memory comes from winograd and im2col-sgemm buffers. With both off (the default build now),
  the peak RSS on three 300 dpi pages (small, side 1280) fell from 960 MB to 660 MB, at +6 % time.
  The text was identical.

**Tiers.**
- small over tiny:
  - +0.006 word F1;
  - −0.0018 mean CER;
  - +0.007 NID;
  - 3.3× the time and 5× the model bytes.
- tiny's dictionary has no Japanese kana (PP-OCRv6 model card).
- medium is not better than small on this suite (word F1 0.9393 against 0.9401) at 4.8× the time and
  4.5× the models.

**Tesseract (B4).**
- It works when `tesseract` is installed: `--engine tesseract`, with the page as PGM on stdin and
  TSV parsed. Its word F1 (0.9417) is level with PP-OCRv6 small (0.9401).
- Its pooled CER is lower (0.072 against 0.095). Almost all of that is reading order: on
  `xelatex-2col` pages its page segmentation reads columns one after the other, while the add-on
  reads rows across both columns. The core re-orders the add-on's lines, so this does not carry over.
- It is 3× faster per page than small, but it is Latin-only with `eng`, needs an installed program,
  and does not run on Windows with nothing installed.
- ScanSet's own baselines on the same items:
  - pdftoppm + tesseract `--psm 1`: word F1 0.9381, NID 0.8557;
  - docling with its default OCR: word F1 0.8040, NID 0.7545 (fails on most JPEG 2000 pages:
    CER 0.738 there).

**Default settings, chosen on development pages** (small, ONNX Runtime; every 3rd item, about 300
pages):

|Setting|Values tried|Word F1|CER pooled|Chosen|
|---|---|---|---|---|
|Space recovery threshold|off / 0.5 / 0.35 / **0.25** / 0.15|0.9404 / 0.9417 / 0.9433 / **0.9437** / 0.9439 (precision falls)|0.0973 / 0.0971 / 0.0969 / **0.0968** / 0.0968|0.25|
|Detection longer side|1024 / **1280** / 1536 / 2048|0.9426 / **0.9436** / 0.9437 / 0.9435|0.0971 / **0.0968** / 0.0968 / 0.0970|1280: same accuracy as 1536, with −12 % time and −18 to −23 % peak RSS|
|Line order|rows / XY cut|0.9437 / 0.9437|0.0968 / 0.1134|rows. Rejected 2026-11-07: an XY cut read tables column by column (c06-tables +2,510 edits) for one 2-column gain.|

**The default build** (ncnn, small, side 1280, winograd and sgemm off), 908 development pages:
- word F1 0.9397;
- CER 0.1245 mean / 0.0947 pooled;
- NID 0.8699.

Speed at side 1280 (CPU 3):

|Build|ms/page median (p95)|Peak RSS (MiB)|
|---|---|---|
|small, ncnn, default convolution options|2,402 (7,553)|1,029|
|small, ONNX Runtime|2,184 (6,521)|584|
|tiny, ncnn|684 (1,734)|610|

**The default build, final** (ncnn, small, side 1280, winograd and sgemm off; CPU 3; the other
columns as above):

|Build|Exe Linux / Windows, stripped (B)|Models (B)|ms/page median (p95)|Startup ms|Peak RSS (MiB)|Word F1|CER mean / pooled|NID|
|---|---|---|---|---|---|---|---|---|
|**ncnn small (default)**|7,310,064 / 6,504,960|30,987,999 (25,940,280 gzip -9)|2,663 (8,289)|20|745|0.9397|0.1245 / 0.0947|0.8699|
|ncnn tiny, same options|7,310,064 / 6,504,960|6,185,082|714 (1,795)|6|374|0.9342|0.1263 / 0.0962|0.8632|

The release archives hold the add-on and `rayopdf-ocr-models/small/`. They are 29,441,566 B
(`rayopdf-ocr-VERSION-linux-x64-static.tar.gz`) and 29,275,719 B (`…-windows-x64.zip`). The
packaged add-on finds its models (`--verify`).

**olmOCR-bench `old_scans`** (98 pages, 526 tests; reproduced, 2026-11-08):
- Pages were rendered at 300 dpi; the 73 pages over 2^26 pixels were rendered at the dpi that fits,
  as the core's canvas does.
- Each tool's text was scored by the harness's olmOCR scorer.

|Tool|old_scans|present|order|absent|
|---|---|---|---|---|
|add-on, small (default)|17.3 %|14.3 %|9.0 %|50.0 %|
|add-on, tiny|14.1 %|8.6 %|5.6 %|57.1 %|
|add-on, Tesseract backend|20.9 %|14.0 %|10.2 %|75.7 %|
|ScanSet: pdftoppm + tesseract `--psm 1`|21.1 %|14.0 %|10.7 %|–|
|ScanSet: docling with OCR|25.1 %|14.7 %|14.7 %|92.9 %|

`old_scans_math` scores 0 % for every tool: its tests need LaTeX, and RayoPDF writes Unicode math.

On `present`, the default is level with Tesseract and docling. The gap is in `absent`: the add-on
alone returns every line, including running headers and page numbers, which the core removes after
OCR.

**Holdout** (the 36 weasyprint items, 112 pages; measured once with the default build, aggregate
only, not used for any choice):
- word F1 0.9408;
- CER 0.1341 mean / 0.0650 pooled;
- NID 0.8637.

**Recommendation.**
- ncnn + PP-OCRv6 small + detection side 1280 is the default. Releases ship this build and this
  tier.
- tiny is the measured alternative where model bytes (6 MB against 31 MB) or time (0.7 s against
  2.7 s per page) matter more than 0.006 word F1. tiny also drops Japanese.
- ONNX Runtime stays a Linux build option (`ocr-ort`). It is not packaged. *Removed in 1.1.*
- Tesseract (`--engine tesseract`) is the optional external backend for comparison. Its recognition
  is level with small on Latin text and 3× faster, but it needs an installed program and covers only
  its installed languages. As a second engine on doubtful lines it was measured and rejected
  (Phase D, below). *Removed in 1.1.*

### Second-engine cascade (Phase D), 2026-11-08

**Decision: not kept.** The add-on has no cascade, and the protocol is unchanged. The gate was:
on the `scanned` development items, NID +0.01 or CER −10 % relative, at no more than +30 % OCR
time per page. No candidate comes near it. Even a perfect choice among the candidates stays below
it, because recognition errors are a small share of what these metrics count.

**Method.**
- A measurement probe, kept outside the tree, linked against the add-on's libraries and ncnn in
  `rayopdf-ocr:1`. It runs the default detection once per page. Then it reads every line crop with
  each candidate and times every reading. Its default readings reproduce the add-on: the same CER,
  word F1 and NID as the default build above, on all 362 development items.
- Candidates for the second reading:
  - *medium*: medium recognition on the same crop. Its ncnn conversion already ships in the image.
  - *tiny-rec*: tiny recognition on the same crop.
  - *upscale*: small recognition on the crop sampled at twice its size.
  - *padded*: small recognition on the rectangle grown by a quarter of its height on every side.
    This is the "other view" candidate: crops are already deskewed, because each is cut at its
    own angle.
  - *tesseract*: Tesseract 5.3.4 `eng`, `--psm 7`, on the same crop. External and optional.
  - *tiny→small*: the tiny tier as the first pass (detection and recognition), with small
    recognition as the second reading.
- Triggers on the first reading:
  - mean confidence below 0.9, 0.95, 0.98 or 0.99;
  - lowest character probability (*minp*) below 0.5, 0.7, 0.85 or 0.95;
  - *implausible*: docling's four text-layer checks at line scale, or fewer than half of the
    tokens word-like;
  - minp or implausible combined, and every line.
- Choosers: always the second reading; the higher mean confidence; the higher minp; or a
  plausibility score (confidence × word-likeness, halved when implausible).
- Every candidate ran with every trigger and every chooser (11 × 4). The table gives the best row of
  each candidate within the time limit.
- medium and tiny→small ran on every third item (121 items, 298 pages) to save time. Their rows are
  compared with the default on the same items.
- Ground truth per line: the best partial match (rapidfuzz) of any candidate's reading in the
  fixture's plain text. Each reading is scored by its edits against that span.
- Time:
  - *+time* is the second reading's time on the triggered lines, over detection plus the first
    reading. It is summed over all items, on the work CPUs.
  - *ms/page* is detection plus recognition per page, measured by the probe on CPU 3 with its SMT
    sibling (CPU 9) idle, over the 41-page timing sample. The probe's default comes out at 3,458 ms
    median against the add-on's 2,663 ms above. The machine was busier (load average 16 → 5 during
    the run), so compare rows only within this table.

**Where the default fails** (908 pages, 29,020 lines, 1,250,102 GT characters; pooled CER 0.0947,
about 118,000 edits):
- Recognition edits on the 28,750 lines that match ground truth: 18,815 (1.5 % of the GT
  characters, about 16 % of all edits). Of these:
  - substitutions 9,834;
  - extra characters 3,243;
  - missing characters 2,502;
  - dropped spaces 2,348;
  - extra spaces 888.
- 270 lines (1,873 characters) match no GT text.
- GT text that no line covers:
  - 21,443 characters of text: missed lines, and image alt text, which is not in the pixels;
  - 33,736 characters of plain-text markup, such as pandoc's table rules.
- Everything else is reading order: the add-on's rows against the GT order.
- The triggers do find the wrong lines:
  - lines with minp ≥ 0.97 (67 % of lines) hold 15.6 % of the edits;
  - lines with minp from 0.5 to 0.7 (9.5 %) hold 35.5 %, with a line CER of 0.036.

  The second readings do not fix those lines. Tesseract improves 2,634 lines and makes 3,366 worse;
  medium improves 277 and makes 519 worse (on its 121 items).

**Ceilings.**
- *oracle*: per line, whichever candidate's reading has the fewest edits.
- *perfect*: every matching line reads exactly its GT span; detection and order are kept.

Both stay below the gate:
- oracle: CER −2.5 % mean / −3.8 % pooled, NID +0.0005;
- perfect: CER −3.6 % / −6.0 %, NID +0.004.

Through the core the share is larger, but still short. The 0.0.9 bundle's Markdown, as plain text,
has a pooled CER of 0.0697 (87,161 edits). A 10 % cut needs 8,716 fewer edits; the oracle removes
5,062 recognition edits. If every one of them carried through the core, the bundle's CER would fall
by at most 5.8 % (an estimate; no cascade was run through the core).

**Decision table, `scanned` development items:**

|Second reading (trigger, chooser)|Lines read twice|CER mean / pooled|Word F1|NID|Line CER|+time|ms/page CPU 3 median / mean (p95)|Models (B)|
|---|---|---|---|---|---|---|---|---|
|none: default, small|–|0.1245 / 0.0947|0.9397|0.8699|0.0160|–|3,458 / 4,528 (12,736)|30,987,999|
|tiny-rec (minp < 0.85, higher confidence)|18.0 %|0.1224 / 0.0943|0.9398|0.8688|0.0163|+4.1 %|3,500 / 4,633 (12,736)|35,440,232|
|upscale (confidence < 0.98, higher confidence)|5.0 %|0.1246 / 0.0947|0.9396|0.8698|0.0160|+3.1 %|mean +1.9 %|30,987,999|
|padded (minp < 0.9 or implausible, higher confidence)|21.7 %|0.1243 / 0.0944|0.9422|0.8701|0.0157|+17.7 %|3,905 / 4,985 (12,800)|30,987,999|
|tesseract (minp < 0.9 or implausible, higher minp)|21.7 %|0.1270 / 0.0941|0.9464|0.8671|0.0155|+28.1 %|4,260 / 5,201 (12,856)|30,987,999 + Tesseract installed|
|oracle over the five above|–|0.1214 / 0.0911|0.9587|0.8704|0.0117|–|–|–|
|perfect recognition|–|0.1200 / 0.0890|0.9735|0.8739|0.0016|–|–|–|
|*every third item (121):* default|–|0.1100 / 0.0968|0.9436|0.8866|0.0164|–|3,458 / 4,528 (12,736)|30,987,999|
|medium (confidence < 0.95, higher confidence)|2.3 %|0.1086 / 0.0964|0.9437|0.8875|0.0161|+4.4 %|3,679 / 4,593 (12,736)|107,456,907|
|medium (confidence < 0.98, higher confidence)|5.2 %|0.1086 / 0.0964|0.9435|0.8874|0.0162|+13.3 %|4,183 / 4,835 (12,736)|107,456,907|
|oracle default or medium|–|0.1079 / 0.0959|0.9471|0.8880|0.0152|–|–|–|
|oracle over all six|–|0.1069 / 0.0941|0.9568|0.8884|0.0126|–|–|–|
|tiny alone (detection and recognition)|–|0.1135 / 0.0994|0.9344|0.8784|0.0180|–|739 / 971 (1,962)|6,185,082|
|tiny→small (confidence < 0.99, small's reading)|22.7 %|0.1107 / 0.0974|0.9423|0.8864|0.0135|+81.6 % of tiny|1,108 / 1,405 (2,143)|27,340,374|
|tiny→small (minp < 0.85, small's reading)|28.1 %|0.1106 / 0.0973|0.9430|0.8864|0.0133|+135.7 % of tiny|1,340 / 1,779 (4,408)|27,340,374|

- The executable is the same for every row: the pruned ncnn build already holds every tier's layers.
- Upscale's time comes from an earlier, loaded run (relative only). Resampling a crop up and then
  down to the recogniser's fixed 48 rows changes almost nothing.
- Gate check, best against the default:
  - CER: −1.7 % mean (tiny-rec, which costs 0.001 NID) and −0.6 % pooled (tesseract, which costs
    0.003 NID);
  - NID: +0.0009 (medium).
- No trigger or chooser can do better than its oracle row. This also covers ones not tried, such as
  a character n-gram model.

**olmOCR-bench `old_scans`** (98 pages, 526 tests). The add-on's text was scored as in B2. Times are
relative, on the work CPUs. On these pages most lines are doubtful, so the triggers fire far more
often:

|Second reading|old_scans|+time|
|---|---|---|
|none: default|17.3 % (91)|–|
|medium (confidence < 0.95)|17.1 % (90)|+130 %|
|medium (confidence < 0.98)|18.1 % (95)|+160 %|
|tiny-rec|17.7 % (93)|+9 %|
|upscale|17.3 % (91)|+34 %|
|padded|18.3 % (96)|+35 %|
|tesseract|17.5 % (92)|+119 %|
|tiny alone|14.1 % (74)|0.36× the default|
|tiny→small (confidence < 0.99)|17.7 % (93)|1.36× the default|
|tiny→small (minp < 0.85)|17.7 % (93)|1.39× the default|

The differences are at most 5 of 526 tests. Every row that gains more than 2 tests is over the time
limit.

**Holdout** (36 weasyprint items, 112 pages). Measured once, aggregate only, and not used for any
choice:
- default: CER 0.1341 / 0.0650, word F1 0.9408, NID 0.8637;
- medium (confidence < 0.98): 0.1336 / 0.0648, 0.9411, 0.8644;
- padded: 0.1348 / 0.0648, 0.9436, 0.8636;
- tesseract: 0.1327 / 0.0627, 0.9536, 0.8624;
- tiny-rec: 0.1312 / 0.0646, 0.9399, 0.8612;
- tiny→small (confidence < 0.99): 0.1341 / 0.0646, 0.9409, 0.8636.

**For OCR speed (plan B7, not this gate).** A tiny first pass gives small's accuracy in a
fraction of the time on these pages. It became the default (next section). The design measured
here detected with tiny; the default keeps small's detection, because tiny's boxes lose bullets.

### OCR speed (plan B7), 2026-11-08

**Change.** Tiny's recogniser reads first, small re-reads the doubtful lines, and degraded pages
switch to small alone (§ One page, step 5). The tier, the protocol and the core are unchanged. The
handshake lists tiny's files as `tiny/<file>` ([ocr-protocol.md](ocr-protocol.md)). Releases ship
`rayopdf-ocr-models/tiny/` beside `small/`.

**Choices, from Phase D's per-line probe (development items; 29,020 lines).**
- **Detection stays small's.** With tiny detecting as well (0.41× the time), the core lost 112
  list items on the development items and construct F1 fell 0.011. Tiny's boxes start after the
  bullet: on every third item, 322 lines began with a bullet glyph in small's boxes and 271 in
  tiny's, both read by small.
- **Trigger: confidence below 0.995.**
  - Below 0.99, 9 of 659 "•" lines kept tiny's misreading of the bullet ("o", "。") and 137 lines
    changed their first character.
  - At 0.995, 2 bullet lines and 28 lines.
  - Accuracy was equal in both cases.
- **CJK trigger.** Tiny reads a Japanese line as look-alike Han ("力夕力" for "カタカナ"). Its
  confidence then is about 0.76, but a short kana word could pass, so any CJK character sends the
  line to small. Chinese lines are therefore read twice; the extra cost is tiny's read.
- **Switch to small** after 8 lines when more than half were re-read. Without it, a tiny first
  pass cost 1.36× the default on `old_scans` (Phase D), where most lines are doubtful. On the
  development pages 27 % of the lines are read after a switch (short pages with tables and
  numbers), and accuracy is unchanged.

**Time per page** (CPU 3 alone, its SMT sibling idle, load average about 2; `--image --repeat 3`,
median of the three per page; 41 development pages as in B2, and 25 `old_scans` pages, every
fourth):

|Build|Development median / mean (p95), ms|`old_scans` median / mean (p95), ms|Start-up ms|Peak RSS MiB|
|---|---|---|---|---|
|before: small|2,996 / 4,340 (12,072)|3,662 / 5,080 (13,024)|21|823|
|after: small with a tiny first pass|**1,905 / 2,247 (3,581)**|**3,525 / 4,622 (10,421)**|23|843|
|change|−36 % / −48 % (−70 %)|−4 % / −9 % (−20 %)|+2|+20|

Through the harness (`rayopdf --convert` per document, fresh process, work CPUs under load), the
wall time per scanned page fell from 8,245 to 4,587 ms (development) and from 7,387 to 2,916 ms
(holdout).

Where the time goes now (probe, CPU 3, the same 41 pages, mean per page):
- small's detection: 894 ms (34 %);
- tiny's reads: 634 ms (24 %);
- small's re-reads: 1,119 ms (42 %).

The networks take over 90 % of the add-on's profile. The add-on's own code is small: CTC decoding
1.3 %, crops 0.9 %, resampling 0.6 %, normalising 0.2 % (perf, one page).

**Accuracy.** Gates: through the core, NID and construct F1 at most 0.002 lower; the add-on's
word F1 at most 0.002 lower and CER at most 1 % higher; `old_scans` not lower.

|Measure|Before|After|
|---|---|---|
|Add-on text, development (362 items): CER mean / pooled|0.1245 / 0.0947|0.1245 / 0.0946|
|Add-on text: word F1|0.9397|0.9404|
|Add-on text: NID|0.8699|0.8699|
|Core + add-on, development: NID|0.9064|0.9068|
|Core + add-on: construct F1|0.6197|0.6193|
|Core + add-on: TEDS / MHS|0.7356 / 0.8393|0.7372 / 0.8368|
|Core + add-on: heading-level accuracy|0.604|0.611|
|Core + add-on, `old_scans` (526 tests)|20.7 % (109)|20.5 % (108)|
|Core + add-on, holdout (36 items, once, aggregate only): NID / construct F1|0.8991 / 0.5642|0.8989 / 0.5635|

- The before and after rows use the same core build (the tree at 2026-11-08).
- The one `old_scans` test lost is an absence test for the running foot "JANUARY 1920 69" on
  page 64. Both outputs contain that foot. Before, the page number happened to be set as a code
  block, which broke the match; after, it is a plain paragraph.
- The other changes on that page are two misreadings ("frst", "afmliation").

**Sizes.**
- Executable (stripped, Linux): 7,314,160 B, up from 7,310,064.
- Models shipped: tiny 6,185,082 B + small 30,987,999 B. Tiny's detector (1,732,760 B) is used
  only by `--tier tiny`.
- Archives: 35,137,525 B (`linux-x64-static.tar.gz`, was 29,441,566) and 34,971,810 B
  (`windows-x64.zip`, was 29,275,719).

**Measured and not adopted.**
- Detection side 1024 instead of 1280, with tiny detecting: development time −4.6 % (work CPUs).
  CER mean 0.1250 against 0.1246, word F1 −0.0002, `old_scans` +2 tests. A small gain against an
  untested risk to structure (footnote marks, small type), so it was not adopted.
- Winograd and im2col-sgemm convolution for the recognition networks only: identical text on three
  pages, and no time difference beyond noise (±10 %).
- **Core side (plan item 3, the client).** The core's own work per OCR page, timed with the instant
  stand-in add-on (`tests/ocr_mock_addon.cpp`, CPU 3): 105 ms median / 131 mean on the development
  documents, and 132 / 193 on `old_scans`. That covers decoding, the canvas, the protocol and the
  OCR text layer's typography, and it is about 6 % and 4 % of the add-on's time per page after the
  change.
  - Pipelining page N+1's canvas with page N's recognition could hide at most that much wall time,
    and only with a second core. It changes nothing on one core.
  - Several pages per request would need a protocol change, and ncnn runs one image at a time, so
    it saves no compute.
  - The canvas resolution does not set the cost: recognition reads each crop at 48 rows whatever
    the dpi, and detection resamples to 1280. Rendering the canvas at the detector's ~116 dpi
    would starve recognition.

  None of the three was built.

### Scripts and rotated pages (round 7), 2026-11-08/09

**Models** (all Apache-2.0, pinned by revision and SHA-256 in docker/ocr_models.py,
`third_party/licenses/paddleocr-scripts.txt`):

|Folder|Model|Dictionary|`rec.ncnn.bin` (half-precision weights)|
|---|---|---|---|
|`scripts/cyrillic`|PP-OCRv5 mobile cyrillic|850 (Cyrillic 365, Latin 115)|3,999,288|
|`scripts/hangul`|PP-OCRv5 mobile korean|11,945 (Hangul 11,504)|6,684,280|
|`scripts/arabic`|PP-OCRv5 mobile arabic (reads right to left text left to right: `order visual`)|747|3,974,364|
|`scripts/devanagari`|PP-OCRv5 mobile devanagari|568|3,931,044|
|`scripts/kannada`|PP-OCRv3 mobile ka (Paddle format only: paddle2onnx in Docker, output checked by SHA-256; `space_gap 0.16`)|153|4,444,688|
|`scripts/tamil`|PP-OCRv5 mobile ta|513|3,917,736|
|`orient`|PP-LCNet_x0_25_textline_ori (0/180 degrees per line; float32)|2 labels|965,612|

Inventoried, not taken: the eslav, el, te, th, latin and en PP-OCRv5 recognisers (scripts the
census does not show, or covered already); PP-LCNet_x1_0_textline_ori and PP-LCNet_x1_0_doc_ori
(6.8 MB each; the 1 MB line classifier and the readings suffice). Hebrew: Rivok/paddleocr-hebrew
(Apache-2.0, published 2026-11-06) reads c12's Hebrew lines nearly exactly under ONNX Runtime with
its 77 MB server CTC model, but pnnx 20261020 converts its SVTRv2 attention wrongly for widths
other than its example shapes, also after rewriting the 5-D qkv split, and its 7.4 MB mobile model
is word-level and misreads whole lines; not shipped. The pack and the classifier are ncnn-only:
the ONNX Runtime minimal build keeps the six tier models' operators, so `ocr-ort` reads neither.

**Half-precision weights** (pnnx fp16=1; ncnn widens them to float32 on load, arithmetic stays
float32): per-script CER on the multilingual suite moved by at most 0.0002 (Arabic 0.0374 against
0.0375; every other script identical to four decimals); adopted for the pack (gate: < 0.002).

**Archives** (`tools/package.py`): the add-on archive gains `rayopdf-ocr-models/orient/`
(36,076,631 B linux tar.gz, was 35,137,525); the script pack is
`rayopdf-ocr-scripts-VERSION-linux-x64-static.tar.gz` (21,104,735 B; 45,154,040 B with float32
weights) and `…-windows-x64.zip`, unpacking into the same folder.

**Engine** (`src/ocr/engine.h`, steps 3 and 6):
- Script routing. A line is unread when its reading holds too few characters for its length or is
  below 0.80. Only lines the tier reads nearly empty (below 0.25 characters per line height) are
  probed, up to three, each by the winners so far first; a winner's reading needs ≥ 0.80, must not
  be unread, and ≥ 50 % of its letters in its script (config `letters`). Winners re-read the
  page's unread lines. Once six lines are read and two thirds are nearly empty, routing runs on
  them, and a winner reads the further lines first (the tier only where it fails): a Cyrillic page
  is not read three times.
- Orientation. Quarter turns from the boxes' shape; the direction by the samples' readings, the
  classifier breaking ties. Upside down: the classifier, confirmed by the readings; and, since the
  classifier misjudges Cyrillic and Hangul (an upright Cyrillic page scores 0.98 for 180°, an
  upside-down Hangul one 0.05), a page left with ≥ 80 % of its lines unread, and either a
  classifier mean ≥ 0.1 or as many lines nearly empty, is read upside down too and kept so only
  when that leaves under half as many unread. Lines carry `dir` (protocol). A box only a little
  taller than wide (a digit in a narrow table cell) is read turned but stays a line of direction 0.
- No memory from page to page: an add-on serves documents of any worker, and the same request
  bytes must give the same response (determinism), so the script found on one page is not carried
  to the next; the per-page caps above bound the cost instead.

**Multilingual suite** (harness suite `multiscript`: `eval/producers/multiscript.py`,
`eval/fixtures/multiscript/`, `eval/scorer/ocr_text.py`; 63 items, 64 pages: each script at 300 dpi
sans and serif, 200 dpi, noisy, and the 300 dpi page turned 90/180/270°). Add-on text per page,
CER pooled / word F1:

|Script|Before (f96cb56 add-on)|After|
|---|---|---|
|Latin|0.344 / 0.787 (rotated pages fail)|0.0004 / 1.000|
|Cyrillic|0.957 / 0.015|0.0004 / 0.998|
|Hangul|0.989 / 0.000|0.019 / 0.890|
|Arabic|0.994 / 0.001|0.037 / 0.926|
|Devanagari|0.985 / 0.018|0.018 / 0.928|
|Tamil|0.964 / 0.024|0.013 / 0.883|
|Kannada|0.958 / 0.023|0.244 / 0.314 (v3; spaces from ink gaps: F1 0.003 → 0.32)|
|Hebrew|0.871 / 0.022|0.884 / 0.016 (no model)|
|Mixed (en, ru, ko, hi, ar)|0.804 / 0.320|0.142 / 0.893|
|Rotated 90 / 180 / 270, all scripts, F1|0.257 / 0.002 / 0.008|0.770 each (as upright)|

Through the core (`convert --tool cli --suite multiscript`, f96cb56 bundle against this one, CER
pooled / word F1): Latin 0.359 / 0.672 → 0.0004 / 1.000, Cyrillic 0.961 / 0.015 → 0.0004 / 0.998,
Hangul 0.993 / 0 → 0.029 / 0.867, Devanagari 0.987 / 0.014 → 0.018 / 0.928, Tamil 0.968 / 0.022 →
0.013 / 0.883, Kannada 0.962 / 0.020 → 0.244 / 0.314, mixed 0.817 / 0.266 → 0.218 / 0.811, Arabic
0.996 / 0.001 → 0.460 / 0.477 (upright pages 0.21–0.23: lines split where the recogniser drops a
number; turned pages 0.78: open item below), Hebrew unchanged; all 0.881 / 0.159 → 0.202 / 0.719.

**Latin/CJK unchanged.**
- Core alone: Markdown byte-identical to f96cb56's core on synthetic (199), odl (200) and scanned
  (398) (core built from f96cb56 plus this round's files only).
- Scanned suite through the core, f96cb56 bundle against this one (development items, 362): NID
  0.9068 → 0.9192, all of it from the c11/c12 items (Russian, Korean, Arabic, Hebrew lines); without
  them NID 0.9196 → 0.9197, TEDS 0.7372 → 0.7372, MHS 0.8368 → 0.8368, construct F1 0.6199 →
  0.6199. Holdout (36 weasyprint items, once, aggregate only): NID 0.8989 → 0.9198.
- Add-on text on the 908 development pages: 878 identical; 28 of the 30 others are c11/c12 pages
  now read in their scripts, two are standing boxes now given their direction (a column of code
  line numbers, one misdetected word).
- Speed, CPU 3 (`--cpuset-cpus=3 --cpus=1`, no other container pinned to it, base and new
  interleaved per page, `--image --repeat 3`, median per page; the 41-page sample of B2): median
  2,119 → 2,143 ms (+1.1 %), mean 2,523 → 2,553 ms (+1.2 %), per-page ratio median 1.008, max
  1.106 (no extra work on those pages besides five classifier runs: noise).
- Cost on other scripts, same setup, one 300 dpi page each (base reads them as look-alikes):
  Cyrillic 8,756 → 12,278 ms, Hangul 7,058 → 9,364, Arabic 5,471 → 5,709, Devanagari 4,668 →
  5,006, Kannada 6,639 → 10,749, Tamil 8,233 → 7,611, the five-script page 4,591 → 12,804, Latin
  3,423 → 3,340.

**Glyph-shape recovery and the census** (`tools/ocr_gate.py collect-rayopdf` + `measure`, each
census page alone, f96cb56 bundle against this bundle; measured before the last rule, standing
boxes reading fewer than four characters keeping direction 0, and not run again after it): RayoPDF's broken pages 130 → 88 (1.46 % →
0.99 %), M3 177 → 219 of 307 (57.7 % → 71.3 %), R core 10.42 % → 9.95 %; no page worse (no text
page lost its text, no new broken page); every page answered within the 600 s per PDF. Fixed: the
Nepali Preeti/Himali pages of 0000828 (25), the Tamil NC* fonts of 0000707 (7), Hindi Chanakya of
0000031 (5), Kannada Nudi of 0000028 (3), 0000319 (2). Complex-script fonts are read by words
(ocr-protocol.md § Glyph shapes): per glyph the Devanagari recogniser reads 20 of 45 Chanakya shapes
and the font is rejected; by words the Tamil font of 0000707 recovers 7,308 glyphs on a page.
Remaining: 0000319 (45 pages, Nepali, several fonts per page), Kannada and Hindi pages where the
words read below the plausibility floor.
