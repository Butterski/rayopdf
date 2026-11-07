# Performance

How RayoPDF's speed is measured and changed, what was measured, and what was tried. Every change
keeps the output bytes (Markdown, image files, `--report` records except `ms`) identical.

## Method

- **Machine:** AMD Ryzen 5 PRO 4650GE, CPU 3 pinned (`--cpuset-cpus=3`), nothing else timed at
  once. Compiler GCC 13.3 `-O2` (`rayopdf-build:2`), release (dynamic glibc, static libstdc++) and
  static (`-static-pie`) builds.
- **Measuring image:** `docker/perf.Dockerfile` (`rayopdf-perf:1`): the build image plus valgrind
  and perf, so profiles run against the binaries' own glibc. perf needs a privileged container
  (host `perf_event_paranoid` is 4).
- **Warm loop:** `rayopdf --bench IN.pdf TMPDIR N [options]` converts IN once, then N times in one
  process (read, open, convert, write Markdown and images) and prints the fastest and median run.
- **Release-bar metrics:**
  - warm: every odl PDF through `--bench DOC TMP 3 --images=none`, best of the 3 runs, two
    alternating rounds; ms/page median and p95 over documents, pages/s = pages / sum of times;
  - fresh: a native `posix_spawn` timer (no Python in the timed path) runs
    `rayopdf --convert DOC OUT --images=none` 2 times untimed and 11 times timed per odl PDF;
    median of the per-document medians.
- **Gates per step** (one commit each, compared with the previous step's binary, never with a
  binary in `build/`):
  - G-bytes: the agent skills' `outcmp.py` over the synthetic corpus (all producers; `--images=none`,
    images written, `--no-structure`), odl, olmOCR born-digital and SafeDocs `0000.zip` (images off
    and on): 5,535 comparisons, 0 differ; `--batch --workers=1` against `--workers=4` at the end;
  - G-warm: `abbench.py` with callgrind instruction counts per conversion (`(I(2N) - I(N)) / N`)
    over 12 fixtures; no fixture may grow by more than 0.3 %; time is reported, not gated;
  - G-fresh (start-up steps): `freshstat.py --perf --control`, CPU time +1 % and faults +0 at most;
  - G-tests: 0 warnings, `dev.py test` (release; ASan + UBSan, TSan and Windows at the end).
- Fixtures: odl 107 (smallest), 183 (median), 192 (p95), 141 (slowest, a vector page), an olmOCR
  `arxiv_math` and `tables` page, synthetic xelatex book chapter, rayomd-pdfua report (tagged),
  libreoffice manual, xelatex-2col paragraphs, typst Unicode, chromium images.

## Findings at 0.0.7 (2026-11-07)

|Finding|Number|
|---|---|
|odl warm, release, images off|1.108 ms/page median, 2.22 p95, 740 pages/s|
|odl warm, static|1.046 ms/page median, 779 pages/s|
|odl fresh, static / release, images off|1.541 / 2.005 ms median (146 / 187 minor faults)|
|empty static-pie program, fresh|0.225 ms, 26 faults|
|`--inspect`, static, fresh|0.296 ms median (after step 7; reader code unchanged)|
|odl in one batch, callgrind|2.32 G instructions: page interpretation 67 % (content lexer 18 %, ShowString 17 %, font loading 20 %, content inflate 6 %), lines 15 %, writer 7 %, tables 7 %, blocks 2 %, order 1 %, reader 0.5 %|
|std::fmin/fmax through libm|6.3 % of instructions plus PLT stubs; 16 calls per glyph|
|Syntax::ParseNumber|7.8 %; 27 % of the slowest odl page|
|glyph-name binary search|3.8 %, 610 instructions a lookup|
|RepairToUnicode sort of a large CID ToUnicode|43 % of olm-tables|
|FoldDuplicates probe chains|27 % of BuildLines' cycles (branch mispredictions)|
|images written|odl 107: 7.8 ms against 0.23 ms without images|
|inflate|13.9 MB inflated over odl, about 14 instructions and 6 cycles a byte|

## Steps

Instructions per conversion (callgrind) and time against the previous step; steps 1–7 ran the
G-warm fixtures with images written, which dilutes the text path on the odl fixtures.

|Step|Commit|Change|Largest effects|
|---|---|---|---|
|0|7f3840a|`--bench`, perf image|baseline|
|1|5495757|fmin/fmax inlined with glibc's exact results|xelatex book -12.6 %, pdfua report -12.0 %, odl 192 -10.6 %; geomean -6.3 % (time -7.8 %)|
|2|e499470|content-lexer numbers read in one pass|odl 141 -17.9 % (time -28 %)|
|3|05d6f9b|table glyph names found by offset|arxiv_math -3.2 %|
|4|2848ddf|ToUnicode largest share by a hash count|olm-tables -49.7 % (time -48 %)|
|5|8885f11|glyph box without NaN checks, font metrics once a string, ASCII text fast path|pdfua report -10.6 %, xelatex book -10.0 %, odl 192 -8.6 %|
|6|6841dd8|exact no-duplicate filter before the duplicate probe|time -4 to -6.6 % on text pages, instructions within +0.27 %|
|7|0a0f11a|base-encoding glyph texts once per process|arxiv_math -3.5 %, manual -1.7 %|

## Results (step 7 against 0.0.7)

|Metric|0.0.7|now|Release bar|
|---|---|---|---|
|odl warm, release, ms/page median|1.108|0.813 (-27 %)|≤ 0.5: not met|
|odl warm, release, pages/s|740|1,028|≥ 2,000: not met|
|odl warm, static, ms/page median|1.046|0.798 (-24 %)||
|odl fresh, static, images off, median|1.541 ms|1.295 ms (-16 %)|≤ 1 ms: not met|
|odl fresh, static, images written, median|1.883 ms|1.602 ms||
|odl fresh, release, images off, median|2.005 ms|1.712 ms||
|peak RSS with images|188 MiB|unchanged|bounded: not addressed|

The remaining gap is broad: at step 7 no function holds more than 13 % of the cycles over odl
(inflate 12.8 % + table building 2 %, content lexer 8.6 %, ShowString 8.1 %, BuildLines 6.5 %,
FoldDuplicates 5.5 %, writer 6 %, tables 4 %). A fresh process costs about 0.3 ms before the
conversion (the start-up of `--inspect`) plus the conversion with cold caches, about 1.25 times
its warm time, so the fresh target follows the warm one (warm ≈ 0.55 ms gives fresh ≈ 1 ms).

## Tried and rejected

- **2026-11-07, duplicate filter with a window widened by one unit and three multiplies a test**
  (first design of step 6): time -3 to -6 %, but instructions +0.4 to +1.7 % on seven fixtures
  (gate +0.3 %): a wide window hits the same letter close by, so most glyphs paid both the filter
  and the probe. The kept design tests the exact tolerance window.

## Open candidates (measured, not done)

- Inflate: 14 instructions a byte where libdeflate-style decoders need 4-6; the table build is
  2 % of the cycles over 1,018 streams.
- Content lexer: split the number loop into integer and fraction runs; inline `PdfValue::Number`.
- Writer (`StyleWords`, `Inline`) string building; table detector passes (`Columns`, `Borderless`).
- Start-up: hot-section ordering (RayoMD's `hot.ld` method), heap growth faults, `_Exit` after the
  last write.
- Images: decode and encode in rows for bounded peak RSS.

## Rerunning

The lab scripts of the campaign (`gate.sh`, `warmsuite.py`, `fresh_timer.cpp`, `profile.sh`,
`ledger.jsonl`) live outside the repository in the campaign's lab directory; they call the agent
skills' `outcmp.py`, `abbench.py`, `freshstat.py` and `ledger.py`:

```sh
docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker
python3 tools/dev.py build --config release --dir perf-release   # copy the binary out first
gate.sh bytes STEP PREV NEW; gate.sh warm STEP PREV NEW; gate.sh suite STEP LABEL...
```
