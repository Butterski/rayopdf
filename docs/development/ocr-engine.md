# OCR add-on engine (`rayopdf-ocr`)

The OCR add-on is a separate executable, `rayopdf-ocr` (`rayopdf-ocr.exe` on Windows), plus its
model folder `rayopdf-ocr-models/<tier>/`. The core `rayopdf` starts it as `rayopdf-ocr --serve` for
pages its routing sends to OCR and talks [protocol 1](ocr-protocol.md) with it over pipes. The core
never links the add-on, the inference runtime or the models. The decision to use linked PP-OCRv6 is
[ocr_ml_decision.md](ocr_ml_decision.md).

**Recommendation (measured below):**
- runtime: **ncnn**;
- tier: **PP-OCRv6 small**;
- detection input: longer side 1280;
- no second engine on doubtful lines: measured in Phase D, it stays far below its gate.

ONNX Runtime gives identical text and runs a few percent faster on Linux. It is not the default
because it has no MinGW-w64 build, so it cannot give the Windows add-on. The `tiny` tier is the
option for size and speed. The `medium` tier is not worth its cost.

## Code

|Part|Files|
|---|---|
|Pixels: resampling (box filter shrinking, bilinear growing), rotated crops, normalisation|`src/ocr/raster.*`|
|Detection post-processing (DB): binarise at `det_thresh`, 8-connected components from row runs, convex hull, minimum-area rectangle, box score, unclip, line order|`src/ocr/detect.*`|
|Recognition post-processing: dictionary, greedy CTC, space recovery, character spans, confidence|`src/ocr/recognize.*`|
|Engines behind one interface (`Engine`): PP-OCRv6 through a `Runtime`|`src/ocr/engine.*`|
|Optional external Tesseract backend (B4)|`src/ocr/tesseract.*`|
|Model folder: `models.txt` (SHA-256 and sizes), `config.txt`, `dict.txt`|`src/ocr/models.*`|
|Protocol server (frames through `src/common/ocr_protocol.*`)|`src/ocr/server.*`|
|Executable and runtimes (only with `-DRAYOPDF_OCR_ADDON=ON`)|`src/ocr/addon/main.cpp`, `runtime_ncnn.cpp`, `runtime_onnxruntime.cpp`|
|Build image: runtimes from pinned source, models downloaded and converted|`docker/ocr.Dockerfile`, `docker/ocr_models.py`|
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
6. **Space recovery.** Greedy decoding drops spaces whose probability is spread over several blank
   steps (PP-OCRv6 writes "andit", "revenueup"). Between two characters, a space is inserted when
   `1 − Π(1 − p_space)` over the gap's steps is at least 0.25. Inserted spaces do not count in the
   confidence.
7. **Output.** Each character's span runs midway between neighbouring characters' step centres,
   mapped back onto the page's x axis. Confidence is the mean of the decoded characters' best
   probabilities. Lines without text are left out.

Each request runs on one thread. The same request gives the same response bytes:
- the server keeps no state that changes results;
- ncnn and ONNX Runtime each run with one thread;
- tests check determinism (codes 1276, 1288), as do the fuzz target and `verify_ocr.py`.

Limits:
- protocol limits (frame, pixels, lines);
- detection side at most 4,096, 4,096 lines per page;
- crop and recognition widths as above;
- a response whose header would exceed 1 MiB is answered `too-large`.

**Angle handling** (a 180° turn) is not implemented. The scanned suite's pages are upright or
skewed by a few degrees, and skew is handled by the rotated crops. It is added only if a measured
suite needs it.

## Build, run, package

```
python3 tools/dev.py image --name ocr                                   # rayopdf-ocr:1 (once; ~50 min, ONNX Runtime dominates)
python3 tools/dev.py test --config ocr            # static-pie build/ocr/addon/rayopdf-ocr (ncnn) + all tests + tests/verify_ocr.py
python3 tools/dev.py test --config ocr-windows    # MinGW-w64 build/ocr-windows/addon/rayopdf-ocr.exe (ncnn), tests under Wine
python3 tools/dev.py build --config ocr-ort       # the ONNX Runtime variant (Linux only)
python3 tools/dev.py --image rayopdf-ocr:1 run -- sh -c 'rm -rf build/ocr-models && cp -r /opt/ocr/models build/ocr-models'
python3 tools/package.py --only ocr-linux         # or --ocr: rayopdf-ocr-VERSION-*.tar.gz/.zip with rayopdf-ocr-models/small/
```

`rayopdf-ocr --serve [--models DIR] [--tier tiny|small|medium] [--engine pp-ocrv6|tesseract]`:
- models default to `rayopdf-ocr-models/` beside the executable;
- `--image PAGE.pgm --repeat N` times one page;
- `--verify` checks the model files' SHA-256 against `models.txt`.

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
  six networks use (68 of 110 layers off);
- ONNX Runtime 1.31.1 (tag archive, SHA-256): minimal build, the six models' operators only, no
  exceptions or RTTI;
- pnnx 20261020;
- PaddlePaddle's official PP-OCRv6 ONNX exports, by Hugging Face revision and SHA-256
  (`third_party/licenses/pp-ocrv6.txt`). They are converted to ncnn by pnnx, and to ORT format at
  optimisation level "extended", so no CPU-specific NCHWc layout is baked into the files;
- Tesseract 5.3.4 with `eng` 4.1.0 (Ubuntu 24.04 packages) for the B4 measurements.

## Measurements (B2, B4), 2026-11-07/08

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
- ONNX Runtime stays a Linux build option (`ocr-ort`). It is not packaged.
- Tesseract (`--engine tesseract`) is the optional external backend for comparison. Its recognition
  is level with small on Latin text and 3× faster, but it needs an installed program and covers only
  its installed languages. As a second engine on doubtful lines it was measured and rejected
  (Phase D, below).

## Second-engine cascade (Phase D), 2026-11-08

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

**For OCR speed (plan B7, not this gate).** tiny→small with the trigger confidence < 0.99:
- On the development items, word F1 is 0.001 lower than small alone, NID is level and pooled CER is
  0.0006 higher. On the holdout, the aggregates are level with the default.
- It takes 0.31× the default's time per page (mean, CPU 3) and 0.88× the model bytes.
- On `old_scans` it costs 1.36× the default, because most lines trigger.
- tiny's dictionary has no Japanese kana. Before tiny can be a first pass, Japanese lines need a
  trigger of their own, such as script detection on the page.

## Open items

- **Windows ONNX Runtime.** ONNX Runtime has no MinGW-w64 build; MSYS2 patches about 80 files for
  1.29. ncnn covers Windows, so the ONNX Runtime variant stays Linux-only.
- **Reading order of multi-column pages** is left to the core.
- **Right-to-left scripts** (c12-rtl) are not in PP-OCRv6's dictionaries. Cyrillic is covered.
