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
|`--inspect`, static, fresh|0.287 ms median with the binary on the container's own file system (0.37 ms when run from a bind-mounted host directory, as in the 0.0.7 record); empty static program 0.215 ms|
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
G-warm fixtures with images written, which dilutes the text path on the odl fixtures; from step 8
every fixture runs with images off, plus two fixtures with images written.

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
|8|70a492c|number loop in integer and fraction runs, `PdfValue::Number` inline|odl 141 -7.1 %, odl 183 -0.9 %|
|9|db16d3a|alpha composed a row at a time (`Png::EncodeRows`), PNG built in an uninitialized per-thread buffer|peak RSS of odl 136 189 -> 97 MiB; odl batch with images 1.07 -> 0.93 s, 203 -> 120 MiB|

## Results (step 9 against 0.0.7)

Measured 2026-11-07, same machine and pinning; warm = best of 3 per document, two alternating
rounds; fresh = native timer, median of 11 per document.

|Metric|0.0.7|now|Release bar|
|---|---|---|---|
|odl warm, release, ms/page median|1.105|0.806 (-27 %)|≤ 0.5: not met|
|odl warm, release, ms/page p95|2.22|1.60||
|odl warm, release, pages/s|738|1,037 (+41 %)|≥ 2,000: not met|
|odl warm, static, ms/page median|1.049|0.796 (-24 %)||
|odl fresh, static, images off, median|1.571 ms|1.308 ms (-17 %)|≤ 1 ms: not met|
|odl fresh, static, images written, median / mean / p95|1.891 / 7.42 / 31.5 ms|1.578 / 6.10 / 26.6 ms||
|odl fresh, release, images off, median|2.016 ms|1.743 ms||
|`--inspect`, static, fresh median, binary on the container's file system|0.287 ms|0.286 ms|≤ 0.3 ms (M1): met by both; not a campaign gain|
|peak RSS, images written, 4653 x 2334 RGB + soft mask (odl 136)|189 MiB|97 MiB|bounded: reduced, not bounded|
|peak RSS, odl as one batch with images|203 MiB|120 MiB||

The remaining gap is broad: no function holds more than 13 % of the cycles over odl (inflate
12.8 % + table building 2 %, content lexer 8.6 %, ShowString 8.1 %, BuildLines 6.5 %,
FoldDuplicates 5.5 %, writer 6 %, tables 4 %). A fresh process costs about 0.3 ms before the
conversion (the start-up of `--inspect`) plus the conversion with cold caches, about 1.25 times
its warm time, so the fresh target follows the warm one (warm ≈ 0.55 ms gives fresh ≈ 1 ms).
Peak RSS with images still grows with the largest image: its decoded pixels, its decoded mask and
the filtered rows the compressor takes whole (about twice the pixels).

## Tried and rejected

- **2026-11-07, duplicate filter with a window widened by one unit and three multiplies a test**
  (first design of step 6): time -3 to -6 %, but instructions +0.4 to +1.7 % on seven fixtures
  (gate +0.3 %): a wide window hits the same letter close by, so most glyphs paid both the filter
  and the probe. The kept design tests the exact tolerance window.
- **2026-11-07, a new inflate decoder.** Measured first: on the 1,144 text-like Flate streams of
  odl (12.0 MB inflated) RayoPDF's Inflate takes 1.58 ns/byte, libdeflate 1.29 ns/byte (the
  perf image's reference build), with the same output for every stream. The decoder is already
  libdeflate's design (packed table entries, 56-bit refills, a fast loop with up to three
  literals a refill); a rewrite could save at most about 0.02 ms a page. Not done.
- **2026-11-07, the PNG file in a fresh uninitialized buffer per image** (first design of step 9):
  the same bytes and RSS, but kept per thread in the final design so a batch maps it once.
- **Not done, measured too small:** `_Exit` after the last write (the destructors and frees after
  the Markdown is written are about 12 k instructions, 4 µs, a conversion); hot-section ordering
  (RayoMD measured 3 to 6 fewer faults; a fresh odl conversion takes 150 faults, 26 of them the
  empty program's); page-level parallelism (odl's documents are single pages).

## Open candidates (measured, not done)

- Writer (`StyleWords`, `Inline`) string building; table detector passes (`Columns`, `Borderless`).
- The content lexer's string and name operands (`ParseValue`, 2.7 % of cycles).
- Images: a compressor that takes the filtered rows as they come (bounded RSS instead of about
  twice the largest image); the warm `--bench` loop over one image now maps glibc's inflate
  buffers anew (odl 107 with images: -5.6 % instructions, +4.8 % time in that loop only).

## Rerunning

The lab scripts of the campaign (`gate.sh`, `warmsuite.py`, `fresh_timer.cpp`, `profile.sh`,
`ledger.jsonl`) live outside the repository in the campaign's lab directory; they call the agent
skills' `outcmp.py`, `abbench.py`, `freshstat.py` and `ledger.py`:

```sh
docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker
python3 tools/dev.py build --config release --dir perf-release   # copy the binary out first
gate.sh bytes STEP PREV NEW; gate.sh warm STEP PREV NEW; gate.sh suite STEP LABEL...
```
