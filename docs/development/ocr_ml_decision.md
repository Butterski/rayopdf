# OCR/ML decision (M8 gate)

Decision record for roadmap milestone M8, 2026-11-07. It applies the measurement table of
[OCR/ML §6.2](../research/ocr-ml.md#62-measurements) and the branches of
[§6.3](../research/ocr-ml.md#63-branches-thresholds-are-our-proposal-calibrate-on-the-first-corpus-run)
with the thresholds of the [roadmap](roadmap.md#milestones) M8 row. Every number below is
reproduced on this machine (AMD Ryzen 5 PRO 4650GE, Docker, harness of
[evaluation.md](evaluation.md)) unless it is marked claimed.

**As built (2026-11-09).** The OCR branch was built as a separate add-on process, `rayopdf-ocr`,
on ncnn, which `rayopdf` starts only when a page needs it and talks to over a pipe; it is never
linked into `rayopdf`. ONNX Runtime remains a Linux-only measurement build and is not packaged.
See [ocr-engine.md](ocr-engine.md) and [ocr-protocol.md](ocr-protocol.md). The decision below is
unchanged as it was written.

## Decision

1. **OCR: branch (c), linked OCR.** R is 10.7 % of the census pages (15.1 % with the upper bound
   for image regions), above the 5 % threshold; most of it is pages with no text layer at all, for
   which OCR is the only way to produce any text. The engine is PP-OCRv6 (small, or tiny for size)
   through a minimal static ONNX Runtime build, or ncnn if the static ORT merge proves fragile, as
   §6.3 (c) describes. OCR is a routed path: it runs only on `image_only` pages, on pages whose
   text layer stays broken after RayoPDF's own repair, and (opt-in) on image regions of digital
   pages. Text-layer pages keep the text-layer path and its speed. The models are side files
   (`--ocr=auto|off|force`), so the default binary stays small; without them RayoPDF writes the
   existing `<!-- rayopdf: … -->` uncertainty comment, as in branch (a). PP-OCRv6's one model
   covers CJK and 46 Latin-script languages (tiny: no Japanese) [claimed]; Cyrillic, 0.9 % of the
   image regions here, needs a separate recognition model such as PP-OCRv5's.
2. **Table ML: no.** Q2 is below 0.10 TEDS in every view (largest: +0.0019, odl table documents).
3. **Reading-order and heading ML: no.** Q1 and Q3 are negative in every view (RayoPDF ahead),
   against the thresholds 0.03 and 0.05.

The OCR branch would be the same without the CJK condition, without the image-region term, and
without the scanner and producer-less families (sensitivity below). The Latin + Cyrillic share of
the R pages whose script is known is 98.1 %, so below 5 % this corpus would select branch (b).

## How to reproduce

```
python3 tools/eval.py census                # census/census.jsonl, summary.json (pdftotext + pdfimages classes)
python3 tools/ocr_gate.py collect           # ocr-gate/safedocs-pages.jsonl, ocr-gate/old-scans-pages.jsonl
python3 tools/ocr_gate.py collect-rayopdf --bin build/static/rayopdf
                                            # ocr-gate/rayopdf-pages.jsonl, rayopdf-pages.meta.json
python3 tools/ocr_gate.py measure           # ocr-gate/gate.json and the tables below
```

All paths are under `benchmark-output/eval/`.

- `collect` runs [`eval/census/pages.py`](../../eval/census/pages.py) in
  `rayopdf-eval-producers:1` with `--network none`. It re-runs the census's own poppler calls and
  classifier and keeps every image's area per page; its page classes match `census.jsonl` on all
  8,905 pages.
- `collect-rayopdf` runs [`eval/census/rayopdf_pages.py`](../../eval/census/rayopdf_pages.py) in
  `rayopdf-build:2` with `--network none`. One `rayopdf --serve --json` process per PDF converts
  each census page alone (`--pages=N-N --images=none`); the `<!-- rayopdf: … -->` comments are
  removed and the page is judged by `census.py`'s own functions (`no_text` below 50 non-space
  characters, else `broken` if `broken_checks()` fires, else `text`). The run recorded here is
  `rayopdf 0.0.7`, `build/static/rayopdf`, SHA-256
  `9870a50a3b87584cb4f37214ef8e4902d3a4ecb58b47eec5afff9511ae61b8f5`; all 8,905 pages answered.
- `measure` reads only files:

|Input|Content|
|---|---|
|`census/census.jsonl`|pdftotext census class and dominant script per page (SafeDocs CC-MAIN-2021-31 `0000.zip`, 996 PDFs opened, first 50 pages each, 8,905 pages)|
|`ocr-gate/rayopdf-pages.jsonl`|RayoPDF's own class per census page, from `collect-rayopdf`|
|`ocr-gate/safedocs-pages.jsonl`, `ocr-gate/old-scans-pages.jsonl`|from `collect`|
|`scores/odl/work/{cli,docling}/evaluation.json`|odl-bench evaluator scores per document|
|`scores/synthetic/{cli,docling}.json`|synthetic scores per item|
|`scores/olmocr/{cli,docling}.log`|olmOCR-bench pass rates per JSONL|
|`speed/cli.json`, `speed-cli-noimages.json`, `speed/docling.json`|single-core speed runs|

`cli` is RayoPDF (`rayopdf --convert {in} {out}`); docling is 2.147.0 with its layout model and
TableFormer and `do_ocr=False`, the ML reference of the [baselines](../benchmarks/baselines-2026-11-05.md).

## Census: M1, M2, M3, R

Page classes are those of [evaluation.md](evaluation.md) (census classes): `image_only` = fewer
than 50 non-space characters and image coverage ≥ 0.8; `broken` = any of docling's four checks.

|ID|Definition used here|Pages|Rate|
|---|---|---|---|
|M1|census `image_only` pages|794|8.92 %|
|M2 (pdftotext)|census `broken` pages|307|3.45 %|
|M2 (RayoPDF 0.0.7)|pages where RayoPDF's text has ≥ 50 characters and fires a check|152|1.71 %|
|M3|pdftotext-`broken` pages on which RayoPDF's text has ≥ 50 characters and fires no check|156 of 307|50.8 %|
|residual text-layer need|RayoPDF `broken` pages (152) + pdftotext-`broken` pages where RayoPDF writes < 50 characters (5)|157|1.76 %|
|R core|M1 + residual text-layer need (no page counted twice)|951|10.68 %|
|image regions|text-bearing image regions on digital pages, upper bound (rule below)|394.5 page-equivalents on 1,381 pages|4.43 %|
|R with regions|R core + image regions|1,345.5|15.11 %|

Of the 307 pdftotext-`broken` pages, RayoPDF also flags 146; it flags 6 pages that pdftotext's
text passes (all census class `text`). The checks that fire on RayoPDF's 152 pages: vowel share
90, accented Latin 47, junk lines 29, junk characters 16 (a page can fire several).

**Markdown and the junk-lines check.** The checks run on RayoPDF's Markdown as written, as the
round-6 measurement did. Markdown joins a paragraph into one line where pdftotext writes one line
per printed line, so the junk-lines check (junk in ≥ 10 % of lines) is stricter on RayoPDF's
text. 14 RayoPDF-`broken` pages fire that check alone; counting them as readable gives
R core 10.52 %, the same branch.

**Image-region rule.** The census cannot see what an image shows, so this term is an upper bound.
On a page whose text layer is good (census class `text` and RayoPDF not `broken`), every image
drawn over at least 5 % of the page area counts as a possibly text-bearing region; the page
contributes min(1, the sum of those images' area fractions) page-equivalents. Smaller images
(logos, icons, bullets) do not count. Photos and figures without text count too, so the true
share is lower. `text_over_image` pages (544, a text layer over an image covering ≥ 0.9 of the
page, mostly scans with an existing OCR layer) are not in R: RayoPDF reads their text layer, whose
quality is the §5 question of re-OCR, not of missing text.

**Documents** (996): 13.96 % have at least one `image_only` page, 4.12 % at least one
pdftotext-`broken` page, 1.81 % at least one RayoPDF-`broken` page, 15.96 % at least one R-core
page, 30.42 % at least one counted image region, 9.64 % at least one `text_over_image` page.

**Per producer family** (pages):

|family|docs|pages|M1|M2 pdftotext|M2 RayoPDF|M3|R core|regions|R with regions|docs with an R-core page|
|---|---|---|---|---|---|---|---|---|---|---|
|adobe|295|3,449|8.1 %|6.4 %|3.6 %|43.6 %|11.7 %|5.3 %|17.0 %|16.3 %|
|apple|50|483|6.4 %|11.6 %|0.0 %|94.6 %|7.0 %|2.4 %|9.5 %|22.0 %|
|chromium|12|69|0.0 %|0.0 %|0.0 %|–|0.0 %|5.4 %|5.4 %|0.0 %|
|ghostscript|34|221|5.0 %|0.5 %|0.0 %|100.0 %|5.0 %|2.1 %|7.0 %|11.8 %|
|itext|28|272|2.2 %|0.0 %|0.0 %|–|2.2 %|0.5 %|2.7 %|10.7 %|
|libreoffice|17|213|10.8 %|0.5 %|0.0 %|100.0 %|10.8 %|8.3 %|19.1 %|11.8 %|
|ms-office|278|2,017|2.8 %|1.0 %|1.1 %|9.5 %|4.0 %|3.9 %|7.9 %|6.1 %|
|other|210|1,444|10.7 %|0.3 %|0.3 %|0.0 %|11.1 %|6.0 %|17.1 %|18.6 %|
|scanner-ocr|28|259|56.0 %|0.4 %|0.0 %|0.0 %|56.4 %|0.0 %|56.4 %|64.3 %|
|tex|12|239|0.0 %|1.3 %|0.0 %|100.0 %|0.0 %|1.6 %|1.6 %|0.0 %|
|unknown|32|239|36.4 %|0.0 %|0.0 %|–|36.4 %|1.1 %|37.5 %|53.1 %|

**Sensitivity.** Without the `scanner-ocr` and `unknown` families (8,407 pages): M1 6.68 %,
R core 8.54 %, R with regions 13.20 %. Every variant stays above 5 %.

## S: script mix of R

Script per page is the census's dominant script (a page with fewer than 20 letters takes its
document's script; a document with fewer than 20 letters in all is `unknown`).

|Part of R|page-equivalents|Latin|Cyrillic|CJK|other|unknown|
|---|---|---|---|---|---|---|
|R core|951|38.6 % (367)|0.1 % (1)|0.7 % (7)|–|60.6 % (576)|
|image regions|394.5|96.5 %|0.9 %|2.6 %|0.1 %|–|
|R with regions|1,345.5|55.6 %|0.3 %|1.3 %|0.0 %|42.8 %|

`unknown` is the scanned documents with no text layer anywhere; their script cannot be known
without OCR. Of the R pages with a known script, Latin + Cyrillic is 98.1 % (97.7 % with regions).
CJK is present (7 R-core pages, 17.3 page-equivalents with regions), which by itself also selects
branch (c).

## Scanned documents: olmOCR-bench `old_scans*`

The harness does not convert or score these categories (they are excluded from the `olmocr`
suite), so there is no RayoPDF or text-layer score for them. The census classifier on their PDFs
(`ocr-gate/old-scans-pages.jsonl`) shows why a text-layer tool cannot pass them:

|category|PDFs/pages|tests|census classes|
|---|---|---|---|
|`old_scans`|98|526 (279 present, 177 order, 70 absent)|100 % `image_only`|
|`old_scans_math`|36|458 (math)|58.3 % `image_only`, 41.7 % `text_over_image`|

Every `present` and `order` test of `old_scans` needs text that only OCR can produce; only the 70
`absent` tests can pass on empty output. `old_scans_math` also needs LaTeX math, which RayoPDF
does not write (D6: Unicode math), as on `arxiv_math` (0/2,927 for RayoPDF and docling alike).

## Q1–Q3: RayoPDF (rule-based) against docling (ML)

RayoPDF here is 0.0.7, the integrated tree of 2026-11-07 (`build/static`, converted and scored
00:18–00:23). Q = docling − RayoPDF; positive means the ML pipeline is ahead. odl: paired
documents where both tools have the metric, with a bootstrap 95 % interval over documents (10,000
resamples, seed 0).
Synthetic: the harness report's class macro-average over development items (holdout excluded),
tagged and untagged separately. TEDS is computed only on documents whose ground truth has a table
(odl: 42; synthetic: 22 untagged and 25 tagged items).

|Q|view|RayoPDF|docling|Q|threshold|
|---|---|---|---|---|---|
|Q1 NID|odl (200)|0.9171|0.9018|−0.0153 [−0.0258, −0.0058]|0.03|
|Q1 NID|synthetic untagged (89)|0.9652|0.9253|−0.0399|0.03|
|Q1 NID|synthetic tagged (92)|0.9695|0.9393|−0.0302|0.03|
|Q2 TEDS|odl, table documents (42)|0.8774|0.8793|+0.0019 [−0.0349, +0.0483]|0.10|
|Q2 TEDS|synthetic untagged|0.9866|0.8702|−0.1164|0.10|
|Q2 TEDS|synthetic tagged|0.9779|0.8806|−0.0973|0.10|
|Q2 pass rate|olmOCR `table_tests` (1,020 tests)|0.618|0.610|−0.008|0.10|
|Q3 MHS|odl (107)|0.8402|0.8269|−0.0133 [−0.0471, +0.0196]|0.05|
|Q3 MHS|synthetic untagged|0.9501|0.8017|−0.1484|0.05|
|Q3 MHS|synthetic tagged|0.9320|0.8322|−0.0997|0.05|

The rule is that any view at or above its threshold triggers the ML branch; none does, and only
odl TEDS is above zero (+0.0019, interval up to +0.048). M5's gates are met
([roadmap status](roadmap.md#milestones)), so Q2 is the "after M5" value the gate asks for.

Outside Q1's definition (NID), olmOCR's page-furniture and order categories show where docling's
layout model is still ahead: `headers_footers` 0.707 against 0.898 (+0.191), `long_tiny_text`
0.509 against 0.523 (+0.014); `multi_column` is level (0.698 against 0.695). These are M3 layout
items for the rule-based path; they do not enter the gate.

## T: cost

|path|ms/page median|p95|pages/s|kind|source|
|---|---|---|---|---|---|
|RayoPDF 0.0.7, images written (default)|3.50|32.30|128.3|fresh process, CPU 3, 2026-11-07|`speed/cli.json`|
|RayoPDF 0.0.7, `--images=none`|3.05|4.33|330.4|fresh process, CPU 3, 2026-11-07|`speed-cli-noimages.json`|
|docling, layout + TableFormer, no OCR|2,003|10,010|0.30|in process, CPU 3, 2026-11-05|`speed/docling.json`|

In process (release libraries, CPU 3, odl, best of 3 per document, 0.0.7 A/B run, not read by
`ocr_gate.py`): 0.94 ms/page median.

OCR cost per routed page, claimed (from [OCR/ML §2.1](../research/ocr-ml.md#21-classic-engines),
not reproduced): PP-OCRv6 tiny 0.22 s, small 0.61 s, medium 3.31 s per image with ONNX Runtime
(1.40 s with OpenVINO) on a Xeon 8350C; Tesseract LSTM 1.5 s wall / 2.5 s CPU per page, 1.8–4.6 s
for a Hindi page depending on SIMD and OpenMP (2017 measurements). With routing, the corpus-wide
cost is the text-layer cost on 89.3 % of pages plus the OCR cost on R core's 10.7 %.

## Limits of the evidence

- **One web-crawled sample.** The census is one SafeDocs zip (996 PDFs opened of 1,000, first 50
  pages each), not the four target document classes; R varies from 0 % (tex, chromium) to 56 %
  (scanner-ocr) by producer family, and ms-office alone is 4.0 %.
- **Scanned-document view.** olmOCR's `old_scans*` are 134 single pages; they show that scans need
  OCR, not how often users convert scans.
- **M3 is not verified against ground truth.** A page counts as fixed when RayoPDF's text passes
  docling's four checks. The checks cannot see a wrong-but-plausible mapping (§1.2: wrong
  ToUnicode, Arabic, Indic), so M3 is an upper bound and R core a lower bound in that respect.
- **M1 misses vector-drawn text.** Character-sized vector paths are not measured; the 236
  `empty_or_vector` pages (2.65 %) may hold some.
- **The image-region term is an upper bound** (rule above); it does not change the branch.
- **Script of scans is unknown** for 60.6 % of R core.
- **Q compares one ML pipeline.** docling 2.147.0 is the only local ML reference; Q2 on odl rests
  on 42 table documents and its interval reaches +0.048.
- **Pages are judged one at a time.** `collect-rayopdf` converts each page alone, so repetition
  across pages (running headers and footers) is not removed as in a whole-document conversion; the
  page text is otherwise what `rayopdf --convert --pages=N-N` writes.

## What would change the decision

- **To branch (b), external tesseract:** R < 5 % on a corpus representative of RayoPDF's users
  (for example office documents alone, 4.0 % here), with Latin or Cyrillic dominant and no CJK
  need, and no requirement for OCR on Windows with nothing installed.
- **To branch (a), no OCR:** R < 0.1 %, which needs a corpus without scanned pages; here only the
  tex (239 pages) and chromium (69 pages) families reach it, and only before image regions.
- **Table ML:** any Q2 view ≥ 0.10 TEDS, for example READoc arxiv `table_teds` (holdout,
  aggregate only) confirming a gap on a development suite.
- **Reading-order or heading ML:** Q1 ≥ 0.03 or Q3 ≥ 0.05 in any view; olmOCR `headers_footers`
  staying 0.19 behind docling after M3's rule-based work is the closest candidate and would justify
  adding an order-test view to Q1.
- **Re-run triggers** (§6.4): a new census corpus, a change to RayoPDF's font → Unicode chain (it
  lowers M2 and raises M3), new PP-OCR or Tesseract major versions.

## After glyph-shape recovery (0.0.9)

Re-run of 2026-11-08 with the 0.0.9 bundle (core plus the OCR add-on, which reads the shapes of
glyphs whose font maps them to no or wrong text; `python3 tools/ocr_gate.py --root ...
collect-rayopdf --bin <bundle>/rayopdf`, then `measure`): RayoPDF's broken pages fall from 152
(1.71 %) to 130 (1.46 %), M3 rises from 156 to 177 of 307 (50.8 % → 57.7 %), and R core from
10.68 % to 10.42 % (14.85 % with image regions). No census page got worse. The decision stands:
R stays above 5 % and CJK is present. Of the 130 pages still broken, about 121 are Devanagari,
Kannada and Tamil in legacy font encodings, which the shipped recognition model does not cover.
With 0.0.10's script pack (Devanagari, Tamil, Kannada recognisers, fonts recovered word by word),
broken pages fall further to 88 (0.99 %), M3 to 219 of 307 (71.3 %) and R core to 9.95 %; no
page got worse (re-run of 2026-11-09).

## Text in images of digital pages

Decision of 2026-11-08 (plan row B6), taken by principle: text that region OCR reads in an image on
a page with a text layer belongs to the figure, not to the running text. With images written (the
default) it is the image's alternate text, `![text](file)` (one line, at most 1,000 bytes, a tagged
figure's `/Alt` kept instead). With `--images=none` it stays body text at the image's place. Text read
on pages without a text layer (whole-page scans, scans drawn with margins) is the page's text and
stays body text in both modes ([ocr-protocol.md](ocr-protocol.md#text-layer)).

Measured with the add-on (`rayopdf-ocr` 0.0.8 ncnn small, static builds, `cli` harness, images
written). "Body" is 0.0.9 at 8b692d9; "alt" is 8b692d9 with only this change:

|Suite (development)|Body|Alt|Documents whose Markdown differs|
|---|---|---|---|
|odl NID / TEDS / MHS / overall|0.9245 / 0.8774 / 0.8422 / 0.9004|0.9247 / 0.8774 / 0.8422 / 0.9007|6 of 200|
|olmOCR born-digital, pass rate per category (headers_footers, long_tiny_text, multi_column, table_tests)|68.8 / 87.1 / 71.7 / 64.7 %|68.8 / 87.1 / 71.7 / 64.7 %|5 of 1,269|
|synthetic (all producers)|unchanged|unchanged|0 of 199|

The text-based metrics hardly move, because the alternate text is still in the Markdown. The
change is where the words sit: next to the figure they come from instead of between the
paragraphs around it. One rule was found on the development data along the way: a scan drawn with
wide margins on an otherwise empty page is a region route. Before the `textLayer` condition, its
whole text went into the alt text and the page got the "no extractable text" comment (olmOCR
`tables`, 3 tests lost). With the condition it stays body text.

Holdout, measured once on the final build, aggregate only: READoc github text_eds 0.9377,
head_teds 0.876, seg_kt 0.9966; arxiv text_eds 0.7452, head_teds 0.4687, table_teds 0.4812,
seg_kt 0.956. These numbers were not used to decide. For reference, Main's earlier runs gave github
text_eds 0.939 without the add-on and 0.926 with body placement.

## Small models in the core (M9)

Decision of 2026-11-14 (roadmap milestone M9). A small model ships in the core only when all of
these hold: the strict quality gate passes (no gated metric more than its epsilon, 0.0001, below
its best); at least one gated metric rises by 0.002 or more; odl instructions (default options,
one worker) grow by at most 0.5 %; the generated model data is at most 256 KiB; two trainings
give byte-identical dumps; it learns only from DocLayNet's train subset and RayoPDF's generated
tables. The features are the converter's own, written by `rayopdf --features`; the training runs
in the `train` image without network ([evaluation.md](evaluation.md#training-data-of-the-small-models)).
**No model ships in 0.3.0.** In 0.4.0 the edge-row model ships with local removals: it met every
criterion (gate of 2026-11-15, below). Its retraining with seven more features (v2) passed the
gate on 2026-11-16 and replaces it after 0.4.0.

|Model|Training data|Validation|Strict gate|odl instructions|Generated data|Two trainings|Shipped|
|---|---|---|---|---|---|---|---|
|Edge rows (furniture the rules keep)|20,000 DocLayNet train pages: 46,947 rows, 21,425 furniture|doclaynet-dev: furniture F1 of rows 0.855 (rules) → 0.878 (rules and model)|0.3.0: failed, doclaynet-val heading_f1 −0.0003; 0.4.0 with local removals: passed, doclaynet-dev and doclaynet-val furniture_f1 +0.0020 and +0.0024|+0.24 %|1,740 nodes, about 46.6 KB|identical|0.4.0|
|Table gaps (borderless columns)|2,400 generated tables (400 fixtures, 6 producers): 3,884 candidate gaps|1,109 gaps, all boundaries|not run|–|–|identical|no|
|Edge rows v2 (after 0.4.0)|the same 20,000 pages: 46,817 rows, 21,425 furniture; 23 features|doclaynet-dev: 0.863 (rules) → 0.880 (rules and model at τ_p)|passed: doclaynet-dev and doclaynet-val furniture_f1 +0.0033 and +0.0097, olmOCR `headers_footers` +0.0027|−0.07 % against master (with 0.4.0's model)|1,740 nodes, 900 leaves, about 35.7 KB|identical|after 0.4.0|

**Edge rows.** LightGBM 4.7.0, 60 trees of 15 leaves over the 16 features of a row the furniture
rules examine in a page's outer tenth (`src/layout/features.h`); it would remove a row the rules
keep, at most 1.35 body sizes, when its probability reaches τ. At τ = 0.69, the threshold with the
best doclaynet-dev F1, it removed the first line of every `rayomd-tech` fixture, so the candidate
used τ_p = 0.933, the lowest threshold with a precision of 0.98 on doclaynet-dev's kept rows
(precision 0.983, recall 0.455), and never took a page below three other lines. It changed 0 of
216 synthetic PDFs, 3 of 200 odl documents, 9 of 1,403 olmOCR pages and 2 of 900 READoc
development documents. The gate measured doclaynet-dev furniture_f1 0.5946 → 0.5967,
doclaynet-val furniture_f1 0.4753 → 0.4781 and olmOCR `headers_footers` 0.8486 → 0.8513, above the
0.002 the criteria ask for, but doclaynet-val heading_f1 fell 0.69590 → 0.69562 (−0.00028, the
gate's epsilon is 0.0001): not shipped.

**Edge rows, local removals (shipped in 0.4.0).** The same trees and τ_p. A first
retry left out rows the semantics would make headings or captions; the gate still measured
doclaynet-val heading_f1 0.695899 → 0.695616 (−0.00028), so the drop came from another way. On
DocLayNet's train selection (the model's own training pages, read only to see what a removal does
to the rest of a page), removing a "Table of Contents" link row of 1.15 body sizes at the head of
three SEC filings turned 298 characters of body text elsewhere on the page into headings: the
removed line no longer counted in the page's statistics. Now the model's rows are not removed
from the page but marked (`kLineEdgeModel`): they take part as text in every later decision (reading
order, tables, body size, styles, heading levels), and only the paragraphs and headings they alone
make are left out at the end (`ConvertPages`); a model row inside a block with other lines stays.
A row that reads as a caption ("Fig. 4. …") is never taken (readoc-dev2 2212.05909: two figure
captions at a page's foot that READoc's ground truth holds); the rest of the first retry's guard
changed nothing once removals were local and is gone. Every block other than those left out is
byte-identical to master's: 0 pages with any other change of 100 changed doclaynet-dev pages, 54
omni-dev pages and 522 changed pages of the DocLayNet train selection (heading_f1 0.682837 →
0.683691, furniture_f1 0.600240 → 0.606688 there). Against master 601c079 in the declared
profiles: changed outputs in 100 of 6,489 doclaynet-dev pages, 0 of 200 odl documents
(`--furniture=keep`), 5 of 1,269 olmOCR pages, 1 of 522 `olmocr-math` pages, 1 of 300 readoc-dev
documents (1206.0456: a "Table of Contents" line READoc's ground truth does not hold), 0 of 600
readoc-dev2, 0 of 234 synthetic PDFs, 54 of 1,074 omni-dev pages (mostly "Page N of M" lines of
PubMed Central pages); each change only leaves blocks out. doclaynet-dev furniture_f1 0.594556 →
0.596594 (+0.0020), heading_f1 0.741168 → 0.741215, text_kept 0.987438 → 0.987436, the other
roles unchanged; olmOCR `headers_footers` 0.848606 → 0.849934 (+0.0013), the other categories
unchanged; omni-dev unchanged. Tests 2254 (a row the model takes is left out and the rest of the
output is that of `--furniture=keep` less the row's block) and 2255 (a caption under a figure,
scored furniture, stays). The strict gate passed (2026-11-15, master `8811572` → `0893b96`):
doclaynet-dev furniture_f1 +0.0020, doclaynet-val furniture_f1 0.4753 → 0.4777 (+0.0024) with
heading_f1 unchanged, olmOCR `headers_footers` +0.0013; odl instructions +0.24 %. It ships.

**Edge rows v2 (after 0.4.0).** A sweep of 0.4.0's model showed that lower
thresholds would gain (doclaynet-dev furniture_f1 and olmOCR `headers_footers`), but every τ
below 0.930 removed the first line "Sentinel start …" of eight `rayomd-tech` fixtures, costing
synthetic untagged MHS 0.9373 → 0.9277. Seven features were added (`src/layout/features.h` 16-22:
the row ends a sentence, its share of words starting in lower case, its words, the size of the
line inside it, its pitch to that line against the text's line pitch, its rank from the edge, its
left offset from the text) and the model retrained on the same pages (doclaynet-dev logloss of
16 against 23 features 0.1545 → 0.1442). The retrained trees still score the sentinel lines
0.95-0.96: DocLayNet's train selection holds only 7 short head rows set apart that end a
sentence (3 of them furniture), too
few for a leaf of 50 rows. So the engine never takes a row that ends a sentence; on doclaynet-dev
none of the rows the model would take at any threshold from 0.6 up that end a sentence is
furniture. With that guard τ_p is chosen at a precision of 0.90 over the rows the model may take
(0.98 before): τ_p = 0.821, precision 0.901, recall 0.492 (the 0.98 rule gives 0.96 with recall
0.08, which lost furniture_f1 0.5966 → 0.5948 and one `headers_footers` test). Against master
a1db445 in the declared profiles: doclaynet-dev furniture_f1 0.596594 → 0.599899 (+0.0033),
heading_f1 0.741236 → 0.741405, text_kept 0.9874361 → 0.9874321, the other roles equal (94 pages
changed, each only by blocks left out); olmOCR `headers_footers` 0.849934 → 0.852590 (+2 tests),
the other categories equal; `olmocr-math` 4 pages changed, equal; omni-dev 23 pages changed, all
four metrics equal; synthetic (the sentinel lines kept), readoc-dev, readoc-dev2 and odl
(`--furniture=keep`) byte-identical. The default odl row, not gated, moves 0.915543 → 0.915495
(a running title of 088 that odl's ground truth keeps). Thresholds 0.711 (precision 0.85) lost
heading_f1 −0.00005 and omni-dev text −0.0003; 0.885 (precision 0.95) gained furniture_f1
+0.0018 only. The forest now stops once the trees left cannot cross τ and its nodes take 12
bytes (float thresholds at or below LightGBM's), and the features reuse the row text the rules
built: odl instructions −0.06 % against 0.4.0 with its model. Tests 2256-2258. The strict gate
passed (2026-11-16, master `8d38727` → `c197bc1`): doclaynet-dev furniture_f1 +0.0033,
doclaynet-val furniture_f1 0.477742 → 0.487467 (+0.0097) and heading_f1 +0.00005, olmOCR
`headers_footers` +0.0027; the largest fall doclaynet-val table_f1 −0.00005, within the epsilon.
odl instructions −0.07 % against master.

**Table gaps.** 400 GFM tables (3-8 columns, 4-20 rows, unique cell texts) rendered by xelatex,
typst, rayomd, rayomd-tech, chromium-untagged and context, all 2,400 valid. Every labelled
candidate gap was a column boundary (3,884 of 3,884 in training, 1,109 of 1,109 in validation):
`--features table-gaps` reports the gaps the column finder already accepts, so there is nothing
to tell apart and LightGBM stops after one tree. A useful model needs candidates below the
minimum gap as negatives.

**Tier B, outside the core.** PP-DocLayout-S for page regions and SLANet_plus with PP-LCNet for
table structure (Apache-2.0) remain candidates for the OCR add-on. The trigger is a measured gap
on image tables or on pages routed to OCR, such as odl 110 and 122.
