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

## Second round: images and scans (0.0.9 code, 2026-11-08)

Base `99fcb3f` (0.0.9 code). Every gate run with `RAYOPDF_OCR` set to a missing path, so the OCR
add-on is never found; the scanned suite (`benchmark-output/eval/scanned`, 398 PDFs: CCITT,
JBIG2, Flate, JPEG and JPX pages at 200 and 300 dpi) joined G-bytes with images on and off
(5,734 comparisons). A change that rewrites PNG files on purpose is checked by a pixel gate: both
builds convert every corpus with images written, the image trees are compared PNG by PNG after
decoding (any depth, palette, alpha), every other file and every report (without `ms`) byte by
byte.

|Finding (base2)|Number|
|---|---|
|SafeDocs 0000315, 416 pages, images written / off|11.2 s / 0.07 s; its images are JPEG 2000 gray (`JPXDecode`), and the time is the JPX decoder (T1 decoding 57 % of the cycles), not 1-bit images|
|159 bitonal scanned items (CCITT, JBIG2), `--batch`, images written|20.8 s, peak 32.7 MiB: each 1-bit page widened to 8-bit gray (unpack 33 %, repack 26 % of the cycles) and compressed a byte a pixel|
|odl 136 (4653 x 2334 RGB + soft mask), peak RSS|96.9 MiB: pixels, mask, the filtered rows and the compressed file whole|
|odl warm, release, images off|0.846 ms/page median (0.0.8: 0.835 on the same day); fresh static 1.40 ms|

|Step|Commit|Change|Result|
|---|---|---|---|
|10|`fc6981f`|gray of 1, 2 or 4 bits that is a PNG's, and stencils, kept packed and written at their depth; other gray at the fewest exact bits; `Deflate::ZlibStream` (the same bytes, a 32 KB window, a block and its lookahead held); the PNG writer filters a row at a time into it and streams the file through `Image::Sink` / `File::StagedFile` (IDAT length set in place, renamed at the end)|bitonal items 20.8 -> 4.4 s, peak 32.7 -> 9.0 MiB, images 29 -> 20 MB; scanned suite 87.5 -> 75.2 s, peak 309 -> 261 MiB; odl batch with images 1.06 -> 0.98 s, peak 122 -> 79 MiB; odl 136 peak 96.9 -> 55.6 MiB; Markdown unchanged, 1,356 gray PNGs rewritten with the same pixels, every other PNG byte-identical|

G-warm for step 10: text fixtures (images off) 0.00 % instructions. Images written:
syn-chromium-images +1.8 % user instructions (copying rows into the compressor's window; callgrind
also counts `rep movsb` a byte at a time) with -12 % task-clock, odl 107 +1.6 % instructions with
-35 % task-clock (no zero-filled image-sized buffers): kept, against the +0.3 % instruction gate,
for the bounded buffers and the lower CPU time.

What still grows with an image: its decoded pixels and its decoded mask (the samples are inflated
whole, then converted; CCITT, JBIG2 and JPX decoders produce whole bitmaps), and the JPX decoder's
own planes (the scanned suite's 261 MiB peak is a 300 dpi colour JPX page).

Also tried in this round: a 10-bit literal/length root table in Inflate (cheaper table builds):
text streams 1.60 -> 1.57 ns/byte, all streams 0.486 -> 0.496 ns/byte; not kept.

## Third round: text path, JPEG 2000, decode-side memory (2026-11-08)

Base `83a0d2d` (snapshot `s11`, the code of `fc6981f`). The work ran as parallel branches, one
area each (interpretation, layout, inflate and image memory, JPEG 2000, then content lexer,
objects and fonts, lines and tables), each step gated against its branch's previous step: G-bytes
over 6,331 comparisons (597 synthetic, 5,734 odl, olmOCR born-digital, SafeDocs 0000 and scanned,
images off and on; PNG files compared by bytes), G-warm at most +0.3 % instructions on every
fixture, release tests. The branches were then put together in order and the whole gated against
the base. CPU 3 was in use by the OCR work for most of the day, so the steps were gated on
instruction counts (callgrind, on other CPUs) and the times below were taken at the end on CPU 3
with the machine otherwise quiet, base and final interleaved.

|Finding (base `s11`)|Number|
|---|---|
|odl in one batch, callgrind, images off|1,770.8 M instructions: Inflate 11.1 % + tables 1.4 %, ContentLexer::Next 9.0 %, ShowString 8.7 %, FoldDuplicates 5.4 %, BuildLines 4.5 % + 1.7 % min/max, memset 3.0 % (1.6 % of it Inflate's output growth), writer about 6 %, tables about 4 %; font loading 18.8 % after the first interpretation steps, with 127 of the 129 embedded programs it inflated never needed|
|SafeDocs 0000315 (416 pages of 8-bit gray JPEG 2000), images written|11.5 s; JPX tier-1 57 % of the cycles; 18.3 ms/MP against OpenJPEG 2.5.4 release's 17.0|
|peak RSS with images written|grows with the largest Flate image: 203 MiB for an 8636 x 5491 RGB image, 55.6 MiB for odl 136|

### Steps

Instructions per conversion (callgrind) against the branch's previous step; "odl" names the
odl fixtures (107 smallest, 141 vector page, 183 median, 192 p95).

|Commit|Change|Largest effects|
|---|---|---|
|`737944d`|Flate image data decoded a row at a time into the colour conversion, masks alongside (`Inflate::Stream`, `Filters::Decoder`/`Chain`, `Document::StreamReader`); decisions that need every row (mask opacity, colour-key opacity, gray depth) read the data a second time; a PNG started at the declared height restarts when the data is shorter|peak RSS table below; text fixtures -0.37 to 0.00 %, odl 107 with images -6.7 %|
|`8556df2`|Inflate decodes into a per-thread buffer that grows without zero-fill, copied out once|olm arxiv -3.9 %, odl -0.6 to -1.0 %|
|`7cf1154`, `f47e969`, `a244487`|Inflate tables filled in canonical order and doubled 16 bytes at a time; code lengths read without a refill per code; entries from constexpr tables, subtables in canonical order|Inflate 241.7 -> 221.5 M over odl, geomean -1.3 %|
|`0d1e6a2`|JPX tier-1: one 32-bit flag word per stripe column (OpenJPEG's layout), MQ contexts as one word, renormalisation in one shift|0000315 -19 % decode time|
|`b6120a0`|JPX samples converted a row at a time, straight to bytes for 8-bit images (SSE2), no tile-sized int32 buffer for 9/7|other development images -21 %|
|`0b766ac`|JPX wavelet: 8-row transposes in registers, vertical pass prefetched, tile cleared only where no code-block writes|scanned JPX -28 %|
|`c37d48a`|List markers compared without strlen|odl -1.2 to -1.6 %|
|`775c226`, `e8725a7`|Writer: style passes only for what a block holds; run text appended in place, escaped only when it holds an escapable byte|style passes: 2col -7.4 %, xelatex book -6.5 %, odl 107 -4.5 %; in-place runs up to -1.7 %|
|`c0b437e`|Table column gaps from the words' bin events, not every bin|odl 183 -4.9 %, odl 192 -5.2 %, 2col -7.3 %|
|`f61ede7`, `f516bb6`, `d874946`, `0413103`|Duplicate filter: cell by one conversion; window cells in float below 2048 units (rounding inside the 1/1024 margin); 2x2 window without loops; table and filter kept per thread, emptied by a moving base|cell conversion -0.2 to -1.5 %; the last three 2col -3.8 %, xelatex book -3.4 %|
|`113e816`, `7ea956a`, `e61b94a`, `99c9c08`|No running-line keys on one-page documents; right-to-left pass only where a right-to-left lead byte occurs; line code points read only while a test needs them; borderless page rows built once|-0.1 to -1.9 % each|
|`e5b9c4b`, `308dae7`|Plain min and max where no NaN can reach them (pages of finite glyphs; word boxes)|xelatex book -2.8 %, 2col -2.7 %, odl 192 -2.3 %|
|`3314f67`, `929190f`|Body faces counted in LineInfo, not again by the writer; escapable bytes found while a run is copied|-0.1 to -0.5 %|
|`06f95cb`|Strings and names as views of the content data, hex strings decoded in one pass, numbers in ParseValue by the one-pass reader|odl 183 -2.9 %, odl 107 -2.2 %|
|`32ae449`|Arena reset free when nothing was allocated; keywords by their first byte; operand count checked inline|odl 141 -1.7 %|
|`bff33fc`|Simple fonts walk the sorted ToUnicode entries with codes 0-255 instead of 256 searches|odl 107 -2.4 %, odl 183 -2.3 %|
|`79fd9de`|Glyph run: what a string's glyphs share computed once and kept while its inputs are bit-identical|pdfua -3.9 %, xelatex book -3.5 %, odl 192 -3.0 %|
|`f1b73aa`, `1c9ae3f`|Embedded font programs read only when a shown code needs them (a simple font with a trusted ToUnicode and /Widths; a Type0 font whose ToUnicode lacks a shown CID)|olm arxiv -31.8 %, chromium images -32.2 %, odl 192 -9.3 %|
|`6ef27aa`, `5bb7663`|ToUnicode: plain hex entries read by a scanner, the sort skipped when in order; RepairToUnicode reuses its first count|olm tables -18.2 %, odl 107 -5.4 %|
|`cfe94bd`, `138fc71`|Simple-font Font::Next inline; the `n g R` look-ahead after a number scans bytes|odl 107 -2.0 %, odl 192 -1.4 %|
|`65affb4`, `9391a7f`, `2112921`, `e28921c`|Content lexer: operands in its own slots with numbers as doubles beside them; byte classes by one table, numbers straight into their slot; arrays, names and strings read by the lexer; operators by tables|odl 141 -18.4 %, odl 183 -4.6 %, lo manual -3.1 % (arrays and strings)|
|`5d98639`|Path boxes grown by plain comparisons (finite values)|odl 141 -1.1 %|
|`36a7cea`, `f7e189d`, `825d6bd`|BuildLines builds its lines in place from per-thread vectors; the fragment being extended kept in locals; a glyph reference carries its text|odl 107 -2.6 %, odl 192 -3.5 %|
|`a731cff`, `4d6901b`, `d9087c0`|Table words sorted by insertion when the order is unique (else std::sort); detector vectors and Cut's per-depth vectors kept per thread|odl 107 -1.6 %, odl 183 -1.4 %|
|`587751f`, `3fd7477`|Objects: an array's numbers read once in File mode; the reference test inline|odl 107 -2.5 %|
|`55c16cd`, `658e128`, `0f979c1`, `f8a8539`, `7cd84ed`, `fd1cc1e`|Fonts and objects: /Widths only for the covered codes; RepairText only for texts it can change; file objects' names and strings as views; one-unit ToUnicode texts appended directly; one-byte texts without the UTF-8 decoder; a font's slots view one pooled copy of its texts|olm tables -8.8 %, odl 107 -4.0 %, chromium images -2.3 %|
|`140cff7`|Per-thread scratch through `PerThread<T>()` (below)|Windows crash fixed; Linux within +-0.013 %|

Font programs read when needed change one counter's meaning: `--report`'s `salvaged_streams`,
`failed_streams` and `limit_hits` no longer count a broken program that is never needed, as they
never counted the fonts of text that is not shown. No corpus item's report changed.

**Windows: `thread_local` objects with destructors.** MinGW-w64's GCC keeps `thread_local` objects
in emulated TLS; at a thread's end winpthreads runs its key destructors in creation order and the
emulated-TLS key comes first, so every block is freed before libstdc++ runs the object's
destructor (a probe under wine sees the heap's free-list pointer in the object's first word). The
per-thread vectors of this round made it a crash at thread exit (the Windows `layout_ocr` test, three
conversion threads: 2 of 3 runs, 0 of 32 before them); the bidi work vectors had carried the same
flaw since 0.0.2. `common/per_thread.h` keeps each such object behind a pthread key of its own on
Windows (a plain `thread_local` elsewhere); 0 of 48 runs failed after it.

### Results (base `s11` against the final tree)

CPU 3, base and final interleaved, 2026-11-08 18:15 (load average 3.7); warm = best of 3 per
document, two alternating rounds; fresh = native timer, median of 11 per document.

|Metric|Base|Final|Release bar|
|---|---|---|---|
|odl warm, release, ms/page median|0.835|0.505 (-39.5 %)|≤ 0.5: 1 % over|
|odl warm, static, ms/page median|0.833|0.493 (-40.8 %)|≤ 0.5: met|
|odl warm, release, ms/page p95 / mean|1.664 / 0.996|1.050 / 0.640||
|odl warm, release / static, pages/s|1,004 / 1,016|1,563 / 1,579|≥ 2,000: not met|
|odl fresh, static, images off, median|1.379 ms (154 faults)|1.061 ms (158 faults)|≤ 1 ms: 6 % over|
|odl fresh, release, images off, median|1.796 ms (190 faults)|1.565 ms (200 faults)||
|odl fresh, static, images written, median|1.693 ms|1.379 ms||
|odl in one batch, callgrind, images off|1,770.8 M instructions|1,101.7 M (-37.8 %)||

G-warm, final against base, instructions per conversion (images off): odl 107 -39.2 %, odl 141
-24.2 %, odl 183 -36.9 %, odl 192 -40.2 %, olm arxiv_math -53.7 %, olm tables -29.0 %, 2col
paragraphs -35.5 %, chromium images -49.7 %, lo manual -28.1 %, pdfua report -25.6 %, typst
Unicode -30.2 %, xelatex book -29.4 %; images written: odl 107 -8.8 %, chromium images -3.8 %.
G-bytes: 6,331 comparisons, 0 differ (PNG files byte-identical, so pixel-identical); `--batch
--workers=1` against `--workers=4`: 0 differ. Tests: release, ASan + UBSan, TSan, debug and Windows
pass with 0 warnings; the fuzz targets build.

Peak RSS with images written (`--convert`, CPU 3, best of 3 interleaved; wall time base -> final):

|Document|Largest image|Base|Final|Time|
|---|---|---|---|---|
|olmOCR arxiv 2503.04486 pg 21|8636 x 5491 RGB, colour key|202.9 MiB|6.7 MiB|0.68 -> 0.48 s|
|olmOCR arxiv 2503.06549 pg 8|7825 x 2424 RGB|66.5 MiB|5.5 MiB|0.12 -> 0.05 s|
|scanned xelatex-eisvogel d04 paper (300 dpi)|Flate page|79.4 MiB|7.6 MiB|0.36 -> 0.20 s|
|scanned rayomd-pdfua c04 lists (300 dpi)|Flate page|72.9 MiB|5.4 MiB|0.13 -> 0.06 s|
|scanned libreoffice d04 paper (300 dpi)|Flate page|68.7 MiB|9.5 MiB|0.35 -> 0.23 s|
|odl 136|4653 x 2334 RGB + soft mask|55.6 MiB|5.4 MiB|0.15 -> 0.10 s|
|SafeDocs 0000277|CMYK + soft mask|48.7 MiB|14.8 MiB|0.21 -> 0.17 s|
|SafeDocs 0000768|RGB + soft mask|45.6 MiB|6.0 MiB|0.13 -> 0.10 s|
|olmOCR arxiv 2503.08206 pg 4|RGB + soft mask|44.7 MiB|5.5 MiB|0.12 -> 0.09 s|
|olmOCR headers_footers 73d9e54b p 15|RGB + soft mask|42.5 MiB|6.0 MiB|0.12 -> 0.09 s|
|SafeDocs 0000157|JPEG decoded whole + Flate mask|242.7 MiB|200.0 MiB|1.27 -> 1.23 s|
|scanned rayomd d03 user manual (300 dpi, a 130 MB file)|seven 2480 x 3509 RGB Flate pages|153.1 MiB|128.7 MiB, the mapped input file (3.8 MiB with images off)|1.17 -> 1.08 s|
|SafeDocs 0000315|416 gray JPEG 2000 pages|45.8 MiB|45.9 MiB|11.46 -> 8.95 s|
|odl, one batch, one worker|—|79.0 MiB|18.9 MiB|0.94 -> 0.79 s|
|scanned suite, one batch, one worker (398 PDFs)|—|261.0 MiB|160.6 MiB|69.4 -> 54.3 s|

JPEG 2000 decode speed, ms per decoded megapixel, single-threaded, each decode in a fresh process
timed around the decode, best of 3, base, final and OpenJPEG 2.5.4's own Release build
(`jpx_ref_fast`, -O3 -ffast-math) interleaved. SafeDocs 0000315 (416 images, 475 MP) on CPU 3;
the development (892) and scanned (182) sets on CPU 11 of the loaded machine (relative only):

|Set|Reduce|Base|Final|OpenJPEG|Final / OpenJPEG|
|---|---|---|---|---|---|
|SafeDocs 0000315 (CPU 3)|0|18.25|13.14|16.97|0.77|
||1|38.50|28.17|33.22|0.85|
||2|63.54|47.78|57.44|0.83|
||3|98.64|78.86|104.20|0.76|
|development (CPU 11)|0|40.01|28.38|33.89|0.84|
||1|59.20|42.72|50.28|0.85|
||2|87.57|64.89|77.88|0.83|
||3|139.54|109.15|144.64|0.75|
|scanned (CPU 11)|0|74.49|53.65|57.49|0.93|
||1|95.71|70.00|75.38|0.93|
||2|126.10|93.08|102.12|0.91|
||3|168.91|127.60|151.48|0.84|

Every development and scanned JPX image decodes to the same components as before at reduce 0-3
(1,074 images x 4, compared by hash; the new tier-1 also block by block against the old one,
34,648 code-blocks).

### The remaining gap

- **Warm median (release 0.505, static 0.493 ms/page):** at the bar.
- **Warm throughput (1,563 pages/s, mean 0.640 ms):** the mean needs 22 % less. No single function
  holds a large share (final callgrind over odl): content lexer 13.1 % + 1.3 % (numbers about
  7-8 instructions a digit; reading 8 or 2 digits a step measured slower), Inflate 10.4 % (1.15x
  libdeflate's time on text streams), ShowString 10.0 % (about 300 instructions a glyph),
  BuildLines 7.5 % and FoldDuplicates 7.3 %, writer 3.6 %, tables 2.5 %, the OCR route's
  `MeasurePage` 1.3 % on every page (OCR code, not in this round), object parsing about 4 %, font
  loading about 9 %. The heavy pages weigh most in the mean (p95 1.05 ms; odl 141, a vector page,
  is 195.7 M instructions).
- **Fresh (static 1.061 ms):** about 0.23 ms is process start-up (the empty static program; 158
  faults against its 26) and the rest the conversion with cold caches, so fresh follows the warm
  time: about 0.47 ms warm would give 1 ms fresh. A fresh conversion now takes 4 (static) and 10
  (release) more faults than at the base; after the first group of steps it took 9 and 3 fewer
  (145 and 187), so the last group of steps added about 13.
- **Peak RSS:** no longer grows with a Flate image (5-15 MiB for every Flate-dominated document
  above, apart from the input file itself, which is mapped: a 130 MB PDF read for its images is
  130 MB resident). It still grows with JPEG samples (decoded whole: SafeDocs 0000157, 200 MiB),
  JPEG 2000 planes (scanned weasyprint d04 paper, 150 MiB: the scanned suite's 161 MiB peak) and
  CCITT/JBIG2 bitmaps (1 bit a pixel).

### Tried and rejected (2026-11-08)

- **Glyph run set up per string** (first design of `79fd9de`): odl 183 +0.62 %, lo manual +0.49 %;
  their TJ arrays hold one or two glyphs a string. Kept as a run reused while its inputs are
  bit-identical.
- **Number digits eight at a time (SWAR)**: odl 141 +7.0 %, every fixture up (1-4 digit runs).
  **Two digits a step**: odl 141 +2.9 %, odl 183 +0.6 %.
- **White-space skipping inlined into the content lexer** (with the byte-scan `n g R` look-ahead):
  odl 141 +0.58 %; kept as `138fc71` without the inlining.
- **ToUnicode entries reserved at data size / 16**: olm tables +0.37 %; **ParseToUnicode templated
  on its sink with reserves**: olm tables +1.09 %. **One-byte fast paths with the junk test out of
  line**: olm tables +1.02 % (kept with it inline).
- **Lexer operand slots zeroed per stream** (10 KB): chromium images +1.09 %; kept growing from 8.
  **PathPoint inlined into Execute**: pushed the operator lookup out of line, odl 141 +2 %. **Array
  and string dispatch inside `Next`**: spills in the white-space loop, odl 141 +0.3 %.
- **ShowString record built in place, rise and spacing read once a string** (not gated, out of
  time): odl 192 -0.55 %, odl 183 -0.21 %; the next step on ShowString.
- **Duplicate filter as one 64-bit word per key and 8 x 8 cell block**: +0.1 to +1.1 % on 11
  fixtures. **Per-thread scratch recording every slot set** (three designs before `0413103`): odl
  107 +0.10 to +0.24 %. **Column histogram walked a run of equal coverage at a time**: under 0.3 %
  on two fixtures only. **Min/max with a five-field NaN sum and an 8-byte right-to-left scan**: lost
  the inlining of `PlaceGlyphs`, +0.1 % everywhere. **Word sort keys zero-filled first**: odl 192
  -0.05 % only. **Detector members as references into the scratch**: odl 192 +0.09 %.
- **Inflate: a literal tested before the subtable at the top of the decode loop**: +1.5 % (goto)
  to +25 % (lambda; register spills) on text streams.
- **A counting pass over an image's rows before its PNG** (to know its height first): inflate is
  32 % of odl 136 with images written, so about +24 %; the PNG restarts at the shorter height
  instead.
- **JPX**: branchless MQ decoding (everywhere +1.0 %, for sign/refinement/uniform contexts only
  +2.5 % on 0000315), a lazy MQ fast path (+7.2 %), SSE2 skipping of empty columns (+0.75 %),
  prefetching the vertical pass's stores (+3.0 %), the 9/7 lifting fused into one sweep (+1.8 to
  +3.3 %).
- **Measured and dropped**: fonts loaded lazily as a whole (4 of 833 loaded fonts are never shown);
  ToUnicode maps shared between fonts (581 of 583 streams distinct); sharing words between the
  table detector and the writer (they build different words).

## Fourth round: research first, then ten areas at once (2026-11-09)

Base `163408f` (0.0.11; snapshots `p4-base`, `p4-base-static`). The round started from three
research notes: how RayoMD got its speed and which of its techniques transfer, how the fast PDF
engines and parsers work ([speed.md](../research/speed.md)), and a profile of 0.0.11 with a cost
model per stage, the page faults of a fresh process by source, and build experiments. Ten areas
were then worked on at once, each in its own worktree, each step gated on its own: G-warm (no
fixture more than +0.3 % instructions), G-bytes quick (synthetic and odl: 997 comparisons), and
before an area's last landing G-bytes full (6,331 comparisons) and ten minutes of fuzzing for every
changed parser of untrusted input. A landing script took one lock for all areas: rebase onto
master, static and release builds without warnings, the core tests, G-bytes quick against the
round's base, push. CPU 3 was kept for timing; the gates ran on instruction counts on the other
CPUs.

|Finding (base `p4-base`)|Number|
|---|---|
|odl in one batch, user instructions|1,103 M: InterpretPage 58.8 % (content lexer 17.5 %, ShowString 13.8 %, font loading 9.5 %, Inflate 12.8 % over all callers), BuildLines with FoldDuplicates 17.7 %, DetectTables 7.7 %, WriteMarkdown 7.2 %, BuildBlocks 3.0 %|
|one page of odl (141: 4.17 MB of path operators, 7 glyphs)|17.7 ms warm, 14.1 % of odl's time; 43 % lexer, 38 % Inflate|
|font loading on SafeDocs, olmOCR `multi_column` and `tables`|18 % of instructions|
|a fresh conversion of the median odl page (183)|136 faults against the empty program's 26: heap 75, program text and data 40, stack 9; a new heap page costs 1.62 µs, a file-backed fault (16 pages) 3.4 µs: about 0.25 ms of 1.16 ms CPU|
|odl 192, fresh|111 faults in the growth of the page's glyph vector|
|`--batch` over odl|15 % of cycles in the kernel: about 10 system calls a document, re-faulted heap|
|SafeDocs sample|703 of 1,000 documents have two pages or more (16 pages a document on average); page-local stages are 75 to 90 % of a conversion's instructions|
|scanned pages with images written|JPEG 2000 tier-1 51 % of cycles, 148 ms a 200-dpi page|
|`--math=latex` on olmOCR `arxiv_math`|the writer 21 % of instructions|

### Steps

Instructions per conversion (callgrind) against the step before in the same area.

|Commit|Change|Largest effects|
|---|---|---|
|`22d56e0`, `0b53bbc`, `9811be0`, `a0a6d70`|Content lexer: its loop inline in `RunStream`, numbers of up to 18 digits read without end checks up to the data's last delimiter, path operators and their points handled in the loop, a point batch per operator; numbers of TJ arrays read straight into their item; operators told from `true`/`false`/`null` by one class load; `q Q cm re`, painting and clipping operators in the loop, `Execute` out of line with a small frame|odl 141 -16.0 %, -1.3 %; odl 183 -1.8 %; lexer 23.5 -> 16.8 instructions a content byte on odl|
|`8e0a17e`, `16840d1`, `2a9ee3a`|Glyphs: the page's glyph vector and text reserved from the content length; one placement per text operator instead of per string, simple-font codes decoded inline, records written in place into a vector grown once an operator, all strings of a TJ array in one loop, a fast path for horizontal runs with finite terms, the glyph box in SSE2 (scalar reference compared bit for bit, test 744-747); the adjustment test and box terms once a run|odl 192 -7.6 %, lo manual -7.1 %, 2col -5.5 %, xelatex book -5.3 %; ShowString 420 -> 266 instructions a glyph on odl; odl 192 fresh: 111 growth faults -> 0|
|`c923a7e`|Page content decoded piece by piece into one per-thread buffer that grows by `realloc`; a Flate piece inflates straight into it|odl 141 -4.9 %, fresh faults 2,540 -> 1,398, peak RSS 9.9 -> 8.6 MiB|
|`8af8551`|Inflate decode loop: a literal flag in the table entry, extra bits masked through a table, short distances without a division; the loop compiled a second time for BMI2 and chosen at run time (test 265 compares both)|DecodeCodes -16.4 % instructions a byte on text streams; odl 141 -5.7 %; BMI2 against the same loop without it: -2.2 % time over all odl streams, equal on text streams|
|`685089d`|Objects: an array's `n g R` found at the `R`, digit tokens, names, arrays and dictionaries read by inline readers, escaped strings decoded through a pointer, hex strings a table load a byte|object parsing -30.7 % over odl; chromium images -6.3 %, lo manual -4.8 %, typst -3.9 %|
|`fbe0346`, `fce601f`, `87af237`, `dd7f0eb`|Lines: duplicates found through exact filter words and chains while the glyphs are collected (one pass), the probe table built only when a page needs it; line metrics taken in the pass that builds the line when no gutter splits it; the open fragment kept in registers; the filter at half size in zero-mapped scratch|BuildLines 198.1 -> 132.9 M over odl (537 -> 360 instructions a glyph); xelatex book -6.0 %; fresh faults -24 a conversion against the first three steps|
|`eb057e1`, `48d5102`, `5d5a155`, `7225007`, `dbc5ea7`, `f0e67e9`, `ea5ed01`|Fonts: a ToUnicode map judged in the pass that reads it; one-unit ranges of 8 codes or more written in place; CID widths from a dense table; long runs judged by code point, the most-shared text counted only when it can be a majority; entry texts read without a bounds check where the scanner has checked them; glyph names through a generated hash (`tools/gen_text_tables.py`); ranges written by UTF-8 length|olm tables -47.3 %, -16.7 %, -14.4 %, -6.9 %; FontCache::Get -68.7 % on SafeDocs, -50.1 % on `multi_column`, -50.4 % on `tables`|
|`80dcc52`, `0d1e901`, `99b784b`, `2241adf`, `c644d07`|Tables and order: a run's column extents grown a row at a time (was rebuilt per row: quadratic); Breuel's order tests a pair's lines-between through overlap bit sets above 32 lines (was cubic); a word's text facts gathered once a page; lattice cells looked up from the last hit, edges listed per component; ordered items not sorted again|odl 192 -6.1 %, odl 183 -3.0 %, 2col -2.1 %; DetectTables + OrderLines 504 -> 261 M on `multi_column`, 229 -> 170 M on `tables`|
|`4c0cadf`, `6a83ea7`|Writer and blocks: blocks without emphasis, scripts, inline images or links written in one pass; style passes linear; line facts in two phases, the footnote finder reads raised lines only|synthetic -2.5 to -4.8 %, odl 107 -3.6 %; WriteMarkdown + BuildBlocks 114.5 -> 81.8 M over odl|
|`534f431`, `99e779a`, `c02e883`|`--math=latex`: glyph classes by table, accent candidates listed once a page; the builder's storage per thread, its baseline weights linear (were quadratic), blocks without a formula candidate on the plain path; the finder's word tests|`--math=latex`: olm arxiv -8.2 %, -6.9 %; synthetic up to -16.3 %; writer and math stages 2,473 -> 1,386 M on `arxiv_math`|
|`2f38e4a`|Linux release builds without unwind tables and without libgcc's unwinder, emergency pool and demangler (RayoMD's stubs; a throw still ends in `terminate`); DT_RELR for the static build|static binary 3,515,432 -> 3,192,752 B; fresh faults median 176 -> 172|
|`e2a476c`, `8c8d842`|`src/hot.ld`: the sections 52 fresh conversions run, in one block before `.text` (`tools/hot_sections.py` regenerates it with callgrind)|fresh faults median 172 -> 158 at the time; warm instructions +0.001 %|
|`70f42b5`, `934b1c6`, `f7284e1`|One-shot `--convert` keeps blocks below 1 MiB on the heap; `--batch`: inputs up to 256 KiB read into a per-worker buffer, the temporary output created with its final mode, counters built only with `--report`, the calling thread is worker 0; batch keeps blocks up to 32 MiB on the heap and 256 MiB untrimmed|batch over odl: faults -40 %, kernel instructions -28 %|
|`ab6d68e`, `135b017`, `b32ee64`|Pages of one document on several threads, started by the content read (below)|27 documents of 4 to 39 pages on four cores: -44 % wall against 0.0.11|
|`60a129a`, `64686d3`, `21d8aeb`, `072578d`|JPEG 2000 tier-1: rows to code found from bit masks, refinement two columns a word, the MQ decoder's registers in 64 bits with one branch on the common path; tile buffers not zero-filled on growth. PNG: filter chosen and applied in one pass, Paeth on bytes (all 2^24 inputs against the scalar form); deflate's main loop without end checks, candidates whose first 4 bytes differ skipped|JPX page with images written 145.4 -> 128.7 ms (same session); branch misses -46 %; PNG encoding -20.0 % instructions|
|`7f95997`, `bf1b10e`|Text path again: a page's glyph records are left unset when the vector grows (`GlyphVector`; the interpreter writes every field: builds that fill new records with 0x00 and with 0xA5 give the same output on 1,835 conversions, memcheck reports nothing), the shown, unmapped and guessed counts in one word, `MeasurePage`'s letters and vowels counted in one word, the line builder's nearness test on the coordinates' bits; path points and boxes in SSE2 (scalar reference, tests 748-749); the ToUnicode parser without virtual calls, hex digits two a step|odl -1.7 %, -0.8 %; syn6 -2.8 %; heavy pages -2.3 %; ShowText 262 -> 248, BuildLines 360 -> 340 instructions a glyph|
|`94988dd`|Tables: grid bounds read once and the cell lookup inline, rows and words sorted only when out of order, a word located once for every column split, word boxes in SSE2 (scalar reference, tests 859-861); bullet marks indexed by height once 16 lines have scanned them|`tables` -2.3 %, odl -1.2 %, `multi_column` -1.2 %|
|`7e9145b`, `1614601`, `d7df386`|Fresh process: the static binary's start no longer reads the `/proc/self/exe` link (a stand-in for glibc's `_dl_get_origin`, `src/cli/static_start.cpp`), and `--convert` ends with `_Exit` after its output is written; Inflate's dynamic tables per thread instead of a 26 KB stack frame; a one-shot conversion frees the page content buffer once the pages are read, so later stages reuse it|odl 183: kernel cycles -6 %, stack faults 9 -> 3; fresh faults median 167 -> 160; fresh -4.3 % (8 paired rounds)|

### Pages in parallel

`--convert` reads a document of two pages or more on up to four threads (`--workers=N`; one page
or `--workers=1` starts none), and the output is the same for every count (roadmap D4):

- The document is shared: objects already parsed are read without a lock, a first parse takes
  one lock, the report counters and the decode budget another. One font cache serves every thread
  and loads a font once.
- What depends on page order is not left to the threads. When a shared read meets such a case (a
  reference cycle, the nesting limit, an object stream reached while it loads, a decode budget
  that is nearly spent), the threaded pass is undone and the pages are read again in order.
- Interpretation, lines, tables, reading order, run-in headings and display formulas run per page
  on the threads. A table that runs on from the page before is the one cross-page input of table
  detection: each page is first detected as if nothing ran on, and in page order a page whose
  previous page ends in a table is put back and detected again with the real state.
- Structure, running lines, images, blocks and the writer stay on the calling thread.
- A thread costs its start and the first touch of its buffers, so no thread starts by itself: the
  calling thread starts one for every 16 KiB of page content it has read, while pages are left
  for it. A document of nearly empty pages (6 pages, 58 glyphs) took 0.71 ms with three threads
  started at once and takes 0.48 ms with none; over the 27 documents the sum is the same as with
  all threads at once (146.7 against 146.0 ms; 151.4 at 32 KiB a thread, 157.6 at 64 KiB). A
  count given with `--workers` starts its threads at once.

### Results (base `p4-base` against the final tree)

CPU 3, base and final interleaved, 2026-11-10 07:15 UTC with the machine otherwise idle; static
builds. Warm: `--bench`, best of 3 per document, two alternating rounds. Fresh: the native timer,
11 runs a document after 2, in 12 rounds with the order of the two binaries rotated.

|odl, 200 one-page PDFs, `--images=none`|Base (0.0.11)|Final (0.0.12)|
|---|---|---|
|warm, ms/page median|0.492|0.370|
|warm, ms/page p95|1.056|0.686|
|warm, ms/page mean|0.630|0.452|
|warm, pages/s|1,587|2,215 (+40 %)|
|fresh, ms a document, median|1.065 (1.055-1.078 over the rounds)|0.885 (0.878-0.894); -16.9 % paired|
|fresh, p95 and mean|1.80, 1.23|1.45, 1.01|
|fresh, CPU ms median|1.020|0.843|
|fresh, page faults median, p95, mean|161, 315, 188|150, 264, 167|
|fresh with images written, median|1.359|1.110|
|`--batch --workers=1`: task-clock|125.5 ms|93.9 ms (-25 %)|
|`--batch --workers=1`: kernel share of cycles, page faults|14.7 %, 4,336|13.0 %, 1,951|

User instructions of `--batch --workers=1` (perf stat):

|Documents|Base, M|Final, M|Change|
|---|---|---|---|
|odl (200 pages)|1,103.0|777.1|-29.5 %|
|the 10 heaviest odl pages|277.2|201.1|-27.4 %|
|6 synthetic documents of several pages|174.6|121.8|-30.2 %|
|40 SafeDocs documents (315 pages)|2,309.0|1,538.8|-33.4 %|
|olmOCR `multi_column` (231 pages)|2,774.4|1,671.2|-39.8 %|
|olmOCR `tables` (188 pages)|1,515.2|1,004.7|-33.7 %|
|olmOCR `arxiv_math` (522 pages), `--math=latex`|7,974.5|5,495.2|-31.1 %|

Per fixture, instructions a conversion (callgrind) and time (the same gate run, on CPU 4; A/A
within 1.5 %):

|Fixture|Instructions|Time|
|---|---|---|
|odl 107 (smallest)|-22.4 %|-11.9 %|
|odl 141 (vector page)|-29.6 %|-30.9 % (19.6 -> 13.6 ms)|
|odl 183 (median)|-25.2 %|-17.8 %|
|odl 192 (p95)|-34.3 %|-43.2 %|
|olm arxiv_math page|-21.4 %|-15.7 %|
|olm tables page|-69.7 %|-61.6 %|
|xelatex 2col paragraphs|-30.4 %|-39.6 %|
|chromium images|-21.7 %|-12.8 %|
|libreoffice manual (7 pages)|-26.0 %|-5.9 %|
|rayomd-pdfua report (4 pages)|-21.5 %|-7.0 %|
|typst unicode|-24.5 %|-17.2 %|
|xelatex book chapter (6 pages)|-28.2 %|-20.6 %|
|odl 107, images written|-14.6 %| |
|chromium images, images written|-18.2 %| |

The documents of several pages gain less time than instructions in the `--bench` loop: with
glibc's default thresholds the loop gives its large blocks back after every conversion and maps
them again (with `--batch`'s settings the same loop takes 1.66 ms instead of 1.97 for the manual,
2.84 instead of 3.33 for the book chapter). `--batch` has those settings; `--bench` and `--serve`
do not.

Documents of several pages (27 documents of 4 to 39 pages: 6 synthetic, 21 from SafeDocs), a
fresh `--convert --images=none` on four cores, median of 15 runs a document, summed:

|Run|Sum, ms|Against 0.0.11|
|---|---|---|
|0.0.11 (one thread)|262.8| |
|final, `--workers=1`|205.1|-22 %|
|final, `--workers=2`|154.9|-41 %|
|final, `--workers=4`|147.9|-44 %|
|final, default (threads started by the content read)|147.4|-44 %|

A JPEG 2000 page with its image written: 140.9 -> 125.9 ms (median over the 100 pages of 40
scanned documents, `--bench`). Static binary 3,515,432 -> 3,315,776 bytes, release 2,594,128 ->
2,493,336.

Output of the final tree against the base, every comparison identical (Markdown, image files,
exit codes and `--report` records): 6,331 conversions of the synthetic corpus, odl, olmOCR
born-digital, SafeDocs and the scanned suite (images off and on, structure off); 1,842 with
`--math=latex`; 1,491 of READoc's 300 PDFs, olmOCR's old scans and the multiscript suite (compared
by bytes only); `--workers=1` against 2, 4 and 8, 9,197 each; the default page threads against the
base's one thread over all of the above; `--batch --workers=1` against `--workers=4`. Tests pass
in the release, ASan + UBSan, ThreadSanitizer and Windows builds; ThreadSanitizer and ASan
conversions of 399 documents with the default threads and with four report nothing; every changed
parser of untrusted input was fuzzed for ten minutes per step, clean.

### The remaining gap

- **Release bar (c):** met on this machine: 2,215 pages/s warm against 2,000, and 0.885 ms fresh
  against 1 ms.
- **Warm:** what is left is spread again. Over odl the content lexer is about 17 instructions a
  content byte (exact number parsing is 55 to 70 instructions a number, and odl has a million of
  them), BuildLines about 340 and ShowText about 248 instructions a glyph (each was above 400),
  Inflate about 10 instructions a byte, font loading 11 % with no hot spot, the writer and blocks
  about 10 %, tables about 8 %.
- **Fresh:** 0.24 ms is the empty static program and 150 page faults are about 0.25 ms. The heap
  faults a document, averaged over 18 odl pages: the glyph array 25 (56 bytes a glyph), the
  duplicate filter about 20, table vectors 10. The start of the static PIE binary costs 9 faults
  for relocation writes and about 12 in glibc.
- **Pages:** on the synthetic documents 44 % of one thread's time stays serial (process start,
  structure, running lines, blocks, the writer), and a thread costs about what 16 KB of content
  cost. Images are still written by one thread.
- **Scans:** JPEG 2000 tier-1 is still 63 % of a page with its image written; most of its branch
  misses come from the decoded data itself.

### Tried and rejected (2026-11-09)

- **Build level** (one build each, output compared over 4,020 conversions):
  - profile-guided optimisation, with and without LTO: +3 to +4 % pages/s, fresh -2 %, output
    identical, held-out pages gain as much as the training pages. Not taken: the training corpus is
    not in the repository, so a release could not be rebuilt from it; untrained JPEG 2000 pages ran
    1.5 % slower; MinGW would need the same two-stage build.
  - `-O3`: +0.9 % warm for 336 KB, fresh slower. LTO alone: within noise. `-march=x86-64-v3`:
    slower, and 10 PNG files differ (FMA contraction in the image code). Static without PIE:
    fresh +1.5 to +2.7 %. BOLT (this CPU records no branches): warm -2 to -6 %, 9.2 MB.
  - a bump allocator: -16 to -26 % (every allocation touches new memory; malloc is 2 % of cycles).
    Huge pages: 0.2 ms for one 2 MiB fault. Pre-faulting (`MAP_POPULATE`, `MADV_POPULATE_READ`):
    no cheaper a page than faulting.
- **Page content read through a window while the page is interpreted** (for odl 141's faults):
  not exact. A page that reaches the 256 MB stream limit before its last piece no longer loads its
  objects in the old order, which the report counters and reference cycles can show. The buffer is
  filled eagerly instead.
- **Lexer:** one digit loop across the decimal point, odl 141 +4.8 %; names and strings of TJ
  arrays written in place, +0.95 M over odl; `Execute` inlined into the loop, text fixtures -0.1
  to -0.8 % but odl 141 +0.8 %; a cache of decoded forms: forms drawn again are 0.55 % of SafeDocs
  at most. Eight digits at a time was estimated at 72 to 82 instructions a token against 65.
- **Glyphs:** the vector grown once a string, +3 to +9 instructions a glyph; the page text grown
  once an operator with ASCII stored in place, 266 -> 285 a glyph; text operators dispatched in
  the lexer loop, +0.01 %.
- **The OCR route's counts taken in the glyph loop** (to drop `MeasurePage`'s pass, 1.4 % of odl):
  exact on 3,066 files, but the counting costs what the pass costs (odl +0.3 to +0.6 %).
- **Objects:** 16-byte SSE2 scans for names and literal strings, more instructions (they are
  short); a literal-string scan four bytes a step, 9.6 -> 11.1 M.
- **Lines:** the first filter design sent every later glyph of a page with a probe table down
  the slow path, olm tables +0.47 %; fragment state in locals, +2.4 M; row words of 4 bits, cell
  chains and branchless bucket heads all above 72 M against the 51.7 M kept.
- **Fonts:** ranges of every length written in place, odl 183 +1.0 % (2- and 3-code ranges);
  runs as a step of their own, olm arxiv +0.30 %; a glyph-name hash of 2^13 slots (55 % full).
- **Tables:** a cell cache per word, `tables` -11.5 M but odl 183 +0.50 % and 2col +0.48 %; the
  row-word vector sized exactly, +0.68 M; text facts through a 256-entry table, slower.
- **Writer:** link boxes tested per word on the plain path, odl 192 +1.3 %; the LaTeX test as a
  run-time flag on the plain path, +0.01 to +0.03 % on every fixture (a template instead).
- **Process:** a 32 MiB heap threshold for one-shot runs, the same faults and 10 MB more for
  scanned documents; every input below 4 MiB through `read`, no gain; batch with only one of its
  two malloc thresholds, more faults than none; `_Exit` after a conversion, about 4 µs.
- **Pages:** per-thread stream buffers on the single-thread path, chromium images +0.70 %; the
  lock inline in the object lookup, +0.3 to +0.4 % (the shared branches are split out instead);
  glibc arena settings against the threads' extra faults, no effect.
- **JPEG 2000:** column pairs skipped in the significance pass, +6 % cycles; branch-free marking
  of the neighbour stripes, +2.5 %; the vertical wavelet pass on 32-column strips, +0.6 %; a lazy
  MQ register, +2 to +3 % (as in round 3).
- **Fresh process:** the static binary without PIE, 5 faults and about 57 K instructions less
  a start, not taken: the image would lose address randomisation in a parser of untrusted input.
  The duplicate filter at half its size, 10 faults less on average but odl 192 +0.37 % and xelatex
  book +0.73 %. A 4 MiB heap top pad, within noise. Page-aligned inflate tables, 5 faults more.
  The binary's pointer tables as offsets, about 3 faults for 500 entries in 15 tables.
- **Second pass on the text path:** printable ASCII gathered a string at a time, 248 -> 274
  instructions a glyph (a TJ string holds 2 or 3 glyphs); the OCR route's counts in the glyph loop
  a third time, odl +0.25 %. The plain writer path chosen by a block's box against the page's
  links, odl 192 +0.37 % (+1.7 % with `--math=latex`); the bullet-mark index built eagerly, odl 141
  +0.5 %.

## Fifth round: a document's work on its threads (2026-11-10)

Base 0.0.12 (`2290bb7`). Round 5 was mostly about quality on scientific papers (the record
[v0.0.13](../benchmarks/v0.0.13-2026-11-11.md)); its performance work moved more of a document onto the
page threads, kept the output byte-identical, and was gated as in round 4 (instructions, G-bytes,
the workers matrix at 2, 4 and 8, ThreadSanitizer and ASan sweeps, fuzzing).

|Commit|Change|Effect|
|---|---|---|
|`df1665e`|`--convert` never takes the conversion apart: no pool join, no destructors after the output is written|the segment after the writer 4.1 / 10.6 ms -> 2.5 ms over 27 multi-page documents (one / default threads)|
|`5aa52a6`|each page's OCR route measured on the thread that interpreted it|instructions +0.11 % at most (odl 107)|
|`99f0928`|the outline and the structure tree read as a job beside the pages' lines (the document still read by one thread at a time, in the serial order)|tagged documents: structure off the critical path|
|`1687644`|`fuzz_layout_pages`: converts with several page threads and compares Markdown and report with one thread|1,203 s, 10,473 runs, clean|
|`8c2c2a6`|a shared section whose parses another order would have stopped at the nesting limit is undone and read in order (found while writing the fuzz target)|test 1605-1609, 1624-1629|
|`1afe0d2`|`--bench` with `--batch`'s allocator settings and input buffer|the bench loop measures what `--batch` does|
|`9ccd4b5`|`--convert` grows its heap 8 MiB of address space at a time (`M_TOP_PAD`)|the largest test document with threads: `mprotect` 151 -> 8, `brk` 25 -> 6, `futex` 296 -> 105|
|`9aecd77`|a document's images decoded and encoded on its page threads (staged files named in page order); JPEG 2000 code-blocks, wavelet, component transform and tile rows on the threads|80 JPEG 2000 documents with images written: 45.3 s on one thread, 19.6 s on four (2.3x); one-thread instructions -0.05 to -0.24 %|
|`4774c53`|`src/hot.ld` regenerated over the round's code (712 sections; the round had renamed the page pool's functions)|output identical|

- **Rejected:** `--serve` with `--batch`'s allocator settings: wall within noise over 1,000
  SafeDocs documents, but 180 MiB more kept at the end of the session.
- **Open:** thread start chained off the calling thread (about 1 ms of 147 over the 27 documents);
  PNG deflate as a pipeline across threads; JPEG 2000 tiles in parallel; the image thresholds
  (`kImageThreadPixels`, `kThreadSamples`, the split size) are not tuned by measurement; the fuzz
  targets decode JPEG 2000 on one thread only (the core test covers the threaded path).

### Results (0.0.12 against round 5's final tree)

CPU 3, the two static builds interleaved, 2026-11-11 with the machine otherwise idle; methods as in
round 4. Round 5's quality rules (headings, tables, text on scientific papers) cost a little time;
its page-thread and image work gain on documents of several pages and on images.

|Measure|0.0.12|Round 5|
|---|---|---|
|odl warm, pages/s (median, p95 ms/page)|2,201 (0.372, 0.699)|2,138 (0.368, 0.826)|
|odl fresh process, ms a document (12 rotated rounds, paired)|0.890|0.911 (+2.5 %)|
|odl fresh, page faults median / p95|150 / 264|153 / 254|
|`--batch --workers=1` over odl, task-clock|94.2 ms|99.7 ms|
|27 documents of 4 to 39 pages, four cores: one thread / default threads|205.1 (2026-11-10) / 147.3 ms|213.4 / 147.1 ms|
|a JPEG 2000 page with its image written (`--bench`, one CPU)|126.0 ms|108.8 ms|
|80 JPEG 2000 documents with images written, fresh, four cores (loaded machine)|45.3 s one thread|19.6 s default threads|

The release bar (c) still holds: 2,138 pages/s against 2,000 and 0.911 ms against 1 ms. The warm
p95 grew 0.699 -> 0.826 ms; it is not traced yet and goes into the next round's plan.

## Sixth round: round 5's cost won back (2026-11-11)

Base 0.0.13 (code `4774c53`). Round 5's quality work cost odl 7.1 % more user instructions than
0.0.12 (`--batch --workers=1`, images off; +2.3 % with the figure regions of 6239f1a, +6.5 % with
the table landing 81dd833). A callgrind comparison of `9aecd77` and `4774c53` put almost all of
it in `FigureRegions` (odl 15.6 -> 48.9 M instructions, SafeDocs 12.0 -> 68.4 M): a flood fill that
visited eight neighbours of every covered cell of a 4-point grid, and a grid of text-line centres
rebuilt on every call. Round 6's quality steps (footnote placement, title block, tables in labelled
sections, rows from verticals drawn row by row) then added their own costs, profiled and cut the
same way. Every step byte-identical (full G-bytes, 6,331 comparisons), gated by `gate.sh warm`.

|Commit|Change|Effect|
|---|---|---|
|`f6d2ca3`|Figure regions: covered cells a bit each, painted per stroke (filled from corner counts when strokes overlap so much that painting costs more), each row's runs joined to the touching runs above by union-find, clusters numbered as the flood fill numbered them; no clustering where the strokes cannot weigh enough; the text grid only when a curve must be weighed|odl -5.7 %, SafeDocs -4.0 %; `FigureRegions` 48.9 -> 2.2 M on odl; warm odl-183 -24.0 %|
|`ffa7114`|A page's stroke clusters found once; a curve weighed against each text line's centre when that is cheaper than the grid|odl -0.3 %, tables -0.3 %, mc -0.2 %|
|`ba48624`|Inflate: a dynamic block's code lengths counted as they are read, so the table builder does not count them again|with `09d8278`: warm -0.28 % on average, syn-pdfua -1.1 %|
|`09d8278`|Outline matching keys written in place, not a byte at a time (`MatchKey` was 8.2 M on SafeDocs)|see `ba48624`|
|`96fa732`, `d01c1f6`, `c09d8ef`|The e-mail test only for words with an at sign (a flag of the word's character facts); a candidate title's first line read once; a lattice's rows split by its verticals only when a vertical piece ends inside the grid; the title block's abstract found from blocks' first lines before any whole block text is read|together: tables -0.27, syn6 -0.45, odl -0.12 points; warm -0.15 % on average|

User instructions against 0.0.12 (`instr.sh`, `--batch --workers=1`): odl +7.11 % at 0.0.13 ->
+1.16 %, SafeDocs +5.61 -> +1.30 %, olmOCR `tables` +5.79 -> +2.39 %, `multi_column` +6.64 ->
+2.41 %, `arxiv_math` +2.82 -> +1.25 %, syn6 +3.84 -> +2.51 %, heavy10 +2.80 -> +0.33 %.

- **Left:** `multi_column` keeps the Type 3 pixel-font em of bcc4bee (779 small glyph procedures
  decoded, 14 M instructions; a shortcut would change the decode counts that `--report` and the
  decode budget see); syn6 keeps the outline-title check; odl keeps `PlaceNotes` (1.1 M) and the
  figure-caption scan (0.4 M).
- **Fuzz findings fixed (`52244c2`):** a ToUnicode `bfrange` with a 70-unit destination wrote
  65,536 entries of 211 bytes (13.8 MB from 340 input bytes) and walked them on the slow repair
  path: a budget of 2^14 UTF-16 units per map for multi-code-point range destinations. A cmap
  inversion read 2^20 codes from each of eight subtables listed twice: one shared budget, a
  subtable read once, the walk stopped when every glyph has its code, format 4 read by segment.
  The largest real map in every corpus held 622 bytes of such text; output unchanged.

Wall-clock timings were not taken: the machine ran agents and harness runs throughout the
round; the release check times the candidate on a quiet machine.

## Seventh round: furniture and table landings won back (2026-11-11)

Base `43211fc`. Round 7's quality work (tables 54a9282, 598ba44, 49972c1; page furniture ac9f628, ae1fe94,
43211fc) left user instructions against 0.0.12 (`instr.sh`, `--batch --workers=1`, images off) at odl +1.30 %,
SafeDocs +1.68 %, olmOCR `tables` +2.87 %, `multi_column` +2.43 %, syn6 +2.68 %, `arxiv_math` +1.27 %. Profiles
put about 2 % of every group in the furniture test of each page's edge rows (a row's text, its repetition key and a
search for metadata words, for every candidate row), and up to 3.4 % in line texts built by one string append per
glyph.

|Commit|Change|Effect|
|---|---|---|
|`fa16417`|A line's text appended a run at a time where its glyphs' texts follow each other in the page text; running keys written in place; metadata words found in one pass|syn6 -1.5 %, odl -0.4 %, `multi_column` -0.35 %, `tables` -0.4 %, SafeDocs -0.6 % (callgrind)|
|`1746d8c`|Body size key counted, not sorted; edge-row key by byte classes, metadata by first letter; the separator test of reading order over wide lines only; filled-area nesting only between areas whose x ranges meet; table word facts by a byte class table|`tables` -0.6 %, SafeDocs -0.4 %, odl -0.3 %, `multi_column` -0.3 %|
|`e3f7df3`|Cross-layout lines scanned over a packed copy; row, word and first-word texts appended by runs|`multi_column` -0.35 %, `tables` -0.35 %|

User instructions against 0.0.12 after the round (master `82818ed`, on `4128ae3`): odl +0.39 %, SafeDocs +0.30 %,
`tables` +1.52 %, `multi_column` +1.34 %, syn6 +0.74 %, `arxiv_math` +0.86 %, heavy10 -0.56 %.

- **Rejected:** a ToUnicode lookup bounded by code distances (maps of subset fonts have gaps: no gain); runs under
  an empty clip taken off the SSE2 box path (odl places many glyphs under empty clips: +0.3 %).
- Wall-clock timings were not taken: the machine ran agents throughout the round.

## Ninth round (2026-11-12)

### Steps

- **CPU dispatch** (6.1): `src/common/cpu.h` chooses a kernel at run time in one place
  (`CpuFeatures()`, `ForceCpuBaseline` for the tests); the inflater's BMI2 choice moved there.
  Output identical, instructions unchanged (odl -0.001 %).
- **Dense ToUnicode index** (6.4): a composite font's ToUnicode map, looked up for every glyph,
  becomes an array from code to entry when its codes span at most 65,536 codes and 16 for each
  code it maps (`ToUnicodeMap::Index`). Output identical; instructions odl -0.20 %,
  `multi_column` -0.35 %, SafeDocs -0.11 %; fresh page faults over odl median 164 → 163.
- **Glyph record 56 → 40 bytes** (2026-11-12): `Glyph` keeps only its box in the direction frame;
  the box on the upright page is that box turned back (`GlyphPageBox`, `layout/page.h`), equal to
  it for direction 0, and an inline image's glyph takes its image's box. Output identical in 4,100
  conversions: synthetic without weasyprint, odl, olmOCR born-digital and READoc-dev, each with
  images written and again with `--math=latex`, and odl with `--format=json`. User instructions
  (`instr.sh`): odl -0.19 %, `multi_column` -0.23 %, `tables` -0.18 %, `arxiv_math` -0.21 %,
  heavy10 -0.04 %, syn6 +0.06 %, SafeDocs +0.09 %. Fresh page faults over odl (`fresh_timer`):
  median 163 → 156, p95 264 → 247. Warm over odl, images off (CPU 3, 2026-11-12 22:24 UTC, load
  about 2): 2,213 → 2,229 pages/s.
- **This program's cost** (2026-11-12): callgrind self instructions of 0.1.0 (`c1116f0`) against
  the program's code (origin/master with `q4/boxgrid`, `q5/repeatheader`, `q7/edgegap`,
  `q8/cards`, `q9/leadline`, `q10/subtitle`, `ocr/imagetables` and `p9/glyph40`), over odl, the
  SafeDocs sample and olmOCR `tables` (Release with `-g` and a `src/hot.ld` placing nothing: its
  placement leaves the placed functions without line information). The program cost odl +0.67 %,
  SafeDocs +2.31 %, `tables` +1.62 %, syn6 +1.04 %, `multi_column` +0.46 % (`instr.sh`). The ten
  functions that grew most (odl + SafeDocs + `tables`; functions inlined or split between the
  builds counted together, the dense ToUnicode index counted with the search it replaced):

  |Function|odl|SafeDocs|`tables`|
  |---|---|---|---|
  |`Detector::BoxGrids` (q4)|0 → 1.13 M|0 → 26.5 M|0 → 8.5 M|
  |`BuildLines` (with `PlaceGlyphs`, `MarkStruck`, `DropLineNumbers`)|113.1 → 115.5 M|161.5 → 164.3 M|146.0 → 148.7 M|
  |`BuildBlocks`|5.32 → 6.09 M|8.91 → 9.98 M|6.16 → 6.91 M|
  |`GlyphPageBox` with the tagged layout's `Piece` (glyph40)|0 → 0.02 M|10.94 → 14.77 M|0 → 0.11 M|
  |`AppendLineText` (called by `BuildBlocks`)|2.24 → 3.21 M|6.83 → 7.10 M|2.69 → 3.57 M|
  |`Detector::BuildRows` (q5's `RepeatedHeaders`)|8.83 → 8.89 M|11.93 → 12.03 M|12.25 → 13.60 M|
  |`PlanPage` (glyph40)|2.54 → 3.16 M|2.10 → 2.64 M|1.12 → 1.41 M|
  |`Writer::StyleWords` (glyph40)|13.67 → 14.05 M|17.92 → 18.42 M|10.24 → 10.64 M|
  |`Folder::Place` (glyph40)|7.87 → 8.20 M|10.32 → 10.78 M|8.97 → 9.40 M|
  |`Writer::JoinLeaderDots` with `string::find` and `memchr` (0.1.1)|0.10 → 0.66 M|0.29 → 1.10 M|0.49 → 1.29 M|

  One rewrite each, kept at 0.1 % fewer instructions or more on odl or the group it serves, each
  byte-identical (G-bytes 0 of 6,331 differ):

  |Commit|Change|Effect (`instr.sh`)|
  |---|---|---|
  |`93f7b2d`|Box grids: a box's rivals looked up by centre and left edge in sorted order, rows and columns by sorted sweeps, groups joined in index order from row and column member lists; groups of fewer than four boxes skipped|SafeDocs -0.99 %, `tables` -0.30 %, heavy10 -0.09 %|
  |`9189f9c`|The title block's abstract test builds a line's text only when its first byte is an a|odl -0.15 %, `multi_column` -0.14 %, `tables` -0.09 %|
  |`a2c69db`|A glyph's combining-mark test only where a word break is found|odl -0.15 %, syn6 -0.24 %, `multi_column` -0.19 %, `tables` -0.13 %, SafeDocs -0.10 %|
  |`f719f31` (after glyph40)|`GlyphPageBox(page, glyph)` inlined, an inline image's box looked up out of line (`Piece`, `StyleWords`)|syn6 -0.42 %, SafeDocs -0.18 %|
- **PNG writer checksums** (6.5): the chunk CRC-32 folds 64 bytes a step with carry-less
  multiplies and the zlib stream's Adler-32 runs on 256-bit lanes, both ported from zlib-ng at
  6727d88 (`src/image/checksum_x86.cpp`) and chosen by `CpuFeatures()` (PCLMULQDQ with SSE4.2;
  AVX2); slicing-by-8 and the SSE2 Adler-32 stay as the baseline paths the tests compare them
  with (every length 0-300 from every start within 32 bytes, runs to 70,000 bytes, chained
  calls). Output identical (the synthetic corpus without weasyprint and the 40 JPEG 2000
  documents, images written). Instruction counts compare different instruction sets, so the gain
  is taken in time: warm `--bench` with images written over the synthetic corpus without
  weasyprint (181 documents), CPU 3, 2026-11-12 22:23 UTC, load about 2: 1,528 → 1,574 pages/s
  (+3.0 %, mean 0.655 → 0.635 ms a page).
- **`src/hot.ld` regenerated** (6.7): `tools/hot_sections.py` over the release and static builds
  of `ec4dfad` (every tenth odl document and four synthetic fixtures of eight producers): 776
  sections, the program's new and renamed functions (the API's conversion, the JSON writer, box
  grids, chart axes, the glyph page box) placed with the code every conversion runs. Output
  identical; fresh page faults over odl (median of 200 documents) 156 → 150 (`56fba35`).

### Result (0.1.1 against 0.2.0, 2026-11-13)

0.2.0 (`536de1f`) against 0.1.1, CPU 3, interleaved, 10:11-10:19 UTC, load about 1 to 2: warm
over odl 2,214 → 2,231 pages/s (median of three runs; p95 0.696 → 0.694 ms), fresh 0.910 →
0.893 ms (12 rotated rounds), fresh page faults 154 → 147, `--batch --workers=1` task-clock 96.6
→ 96.2 ms, SafeDocs 0000348 on one core 11.96 → 11.02 ms. The program's own cost (+0.67 %
instructions over odl, +2.3 % over SafeDocs) is won back, and its warm target of 2,250 pages/s
is not reached ([v0.2.0](../benchmarks/v0.2.0-2026-11-13.md#speed)).

### Tried and rejected (2026-11-12)

- `RepeatedHeaders`: no `BuildRows` below a table unless a free word there starts with the first
  byte of its header labels: `tables` -0.006 %, odl +0.004 % (most pages have such a word).
- `PlanPage`: the image's centre test before `Counted`: odl -0.056 %, `multi_column` -0.059 %.
- `Folder::Place`: the corner `CollectAndFold` computed passed in: odl +0.026 %, syn6 +0.040 %.
- `JoinLeaderDots`: a `memchr` scan testing the bytes after each dot instead of `string::find`:
  odl -0.064 %, `tables` -0.076 %, syn6 -0.072 %.
- `BuildBlocks` itself: its growth (0.1.1's rules and q8-q10) is spread over a dozen passes of
  20-50 k instructions each over odl; no single pass reaches 0.1 %, none rewritten.
- JPEG 2000 inverse transforms on AVX2 lanes (6.6, branch `p9/jpx`): the 9/7 and 5/3 lifting and
  the colour transforms written once (`jpx_dwt_lanes.h`) and compiled for SSE2 and for AVX2
  without FMA, every float bit-equal to the SSE2 path, chosen by `CpuFeatures()`. On the 40 JPEG
  2000 documents (`jpxtime.sh`, CPU 3, 2026-11-12 22:24 UTC): -1.67 % (sum of best times 11,773 →
  11,576 ms; median page 108.9 → 106.5 ms), below the 5 % the step had to reach: the transforms
  are about a tenth of the decoder's instructions (round 3's profile), the rest is entropy
  decoding.

## Tenth round (2026-11-13)

Base `613f071` (`p10-base`) against 0.2.0 (`536de1f`). The 0.3.0 program so far cost user instructions
(`instr.sh`, `--batch --workers=1`, images off; `arxiv_math` with `--math=latex`): odl +0.32 %, heavy10
+0.11 %, syn6 +0.37 %, SafeDocs +0.24 %, `multi_column` +0.61 %, `tables` +0.27 %, `arxiv_math` +0.61 %.
Other jobs ran on the machine all round: no times were taken, and every step was kept or rejected by
instruction counts alone (at least 0.1 % fewer on odl or on the group the code serves; 0.5 % for the
open candidates' own groups).

### Steps

- **This program's cost** (7.1): callgrind self instructions of 0.2.0 against `613f071` over odl and
  olmOCR `multi_column` (Release with `-g`; `multi_column` linked with gold to keep line information).
  The functions that grew most, with functions inlined or split between the builds counted together
  (`RemoveEdgeRows`, inlined into `RemoveRunningLines` in 0.2.0, did not grow: 3.98 → 3.83 M over odl):

  |Function|odl|`multi_column`|What grew|
  |---|---|---|---|
  |`BuildBlocks`|6.06 → 6.45 M|17.94 → 24.13 M|card titles (`e4d1d0a`): a column looked for below each title-like block|
  |`Writer::CollectWords`|11.13 → 11.80 M|24.94 → 26.48 M|a soft hyphen's word break carried to the next glyph|
  |`Writer::Inline` (with `InlinePlain`)|23.56 → 24.10 M|61.10 → 62.45 M|the same, and plain output's script tags|
  |`OrderLines` (`DeferNotes`)|2.19 → 2.39 M|6.84 → 7.40 M|notes after columns (`e4d1d0a`)|
  |`AppendLineText`|2.35 → 2.62 M|5.09 → 5.29 M|`DeferNotes`' line texts|
  |`Writer::StyleWords`|14.10 → 14.23 M|33.11 → 33.36 M|its source unchanged since 0.2.0|
  |`IsCaption` (moved to `lines.cpp`)|0.06 → 0.10 M|0.21 → 0.24 M|called by `DeferNotes`|
  |`memcpy`, `malloc`|9.12 → 9.28 M|26.59 → 26.07 M|their callers above|
  |`string::append`|1.60 → 1.66 M|5.66 → 5.71 M|their callers above|
  |`strlen`|0.56 → 0.61 M|1.60 → 1.71 M|their callers above|

  One rewrite each for the program's own functions (the library functions follow their callers),
  kept at 0.1 % fewer instructions or more, each byte-identical (`land.sh`, G-bytes 0 of 997 differ):

  |Commit|Change|Effect (`instr.sh`)|
  |---|---|---|
  |card titles (`perf(layout): card titles tested cheaply first…`)|Card titles: a title-like block's cheap tests before its characters are summed, the peer loop's geometry before `titleLike`; the column below a title looked for only when the block has a peer (the budget taken as before, in the same order), over a packed array of the raw blocks' first lines|`multi_column` -0.165 %, `tables` -0.020 %, the other groups within +0.012 %|

- **Open candidates** (7.2):

  |Commit|Change|Effect (`instr.sh`)|
  |---|---|---|
  |hex strings (`perf(pdf): hex strings read in one pass`)|Hex strings read in one pass: the closing `>` found with `memchr`, the bytes checked and decoded in one pass into room for a byte per two bytes of the string|odl -0.103 %, syn6 -0.248 %, `multi_column` -0.126 %, SafeDocs -0.061 %, `tables` -0.029 %|

- **`src/hot.ld` regenerated** (7.4): `tools/hot_sections.py` over the release and static builds
  of the release's code: 799 sections (776 before). The code it places in the static binary grew
  from 948,493 to 1,013,037 bytes, in the release binary to 850,678; output byte-identical (G-bytes
  0 of 1,302 differ). CPU 3, 2026-11-14 12:10-12:12 UTC, against the same code with the ninth
  round's list: warm odl images off (three runs) 2,185 → 2,200-2,205 pages/s (+0.8 %), fresh 0.912
  → 0.897 ms (12 rotated rounds, -1.6 %), page faults median 151 → 148.

### Result (`p10-base` against the round's two steps)

User instructions, `--batch --workers=1`, images off (`arxiv_math` with `--math=latex`), CPU 9; the
round's tree is `613f071` with the two steps:

|Group|0.2.0|`613f071`|Round 10|Program|Round|0.2.0 → round|
|---|---|---|---|---|---|---|
|odl|783,989,572|786,517,003|785,874,263|+0.32 %|-0.08 %|+0.24 %|
|heavy10|200,407,195|200,632,270|200,615,428|+0.11 %|-0.01 %|+0.10 %|
|syn6|123,336,618|123,798,363|123,507,063|+0.37 %|-0.24 %|+0.14 %|
|SafeDocs|1,564,331,356|1,568,096,879|1,567,519,958|+0.24 %|-0.04 %|+0.20 %|
|`multi_column`|1,699,366,109|1,709,649,542|1,704,958,097|+0.61 %|-0.27 %|+0.33 %|
|`tables`|1,040,050,173|1,042,839,209|1,042,421,870|+0.27 %|-0.04 %|+0.23 %|
|`arxiv_math`|5,548,888,634|5,582,868,790|5,583,860,757|+0.61 %|+0.02 %|+0.63 %|

### Tried and rejected (2026-11-13)

Each against `613f071` (`instr.sh`); none reached its threshold.

- `CollectWords` and `InlinePlain`: a line's superscript and subscript bounds read once, a soft
  hyphen's word break carried as flag bits: odl -0.045 %, syn6 -0.064 %, `multi_column` -0.065 %,
  `arxiv_math` -0.067 %. With `InlinePlain`'s style test moved from its pass before writing into the
  writing loop (what was written goes again): odl -0.010 %, syn6 +0.231 %, `multi_column` -0.071 %.
- `DeferNotes`: the first 10 bytes of a line's text in place of the whole (all the tests read), its
  vectors kept by the thread: odl -0.040 %, `arxiv_math` +0.025 %, the rest within 0.006 %.
- `StyleWords` (open candidate): its passes over every glyph through local pointers and style masks (a
  style byte's store may alias every member): odl -0.025 %, `multi_column` -0.027 %, SafeDocs
  -0.019 %, syn6 -0.030 %.
- Breuel's `Topological` (open candidate, 20.8 M of `multi_column`'s 1,735 M under callgrind): the
  side-by-side test over the wide lines only, the edges as bit rows and the free lines' in-degrees
  lowered along set bits: `multi_column` +0.052 %; with its vectors kept by the thread +0.083 %; out of
  line besides +0.075 % (odl +0.013 %). Its own lines fell by about a fifth; `Cut`, which it was inlined
  into, grew by more.
- Card titles without the packed array (the first design of card titles (`perf(layout): card titles tested cheaply first…`)): `multi_column` -0.082 %.
- **Images encoded on the page threads** and **`--serve` with `--batch`'s malloc settings**: their
  gains are wall time only (the instructions stay or grow), which this round could not measure; they
  stay open.
- **Profile-guided static build** (7.3; landed as `551d3a6` with `tools/pgo_corpus.py` and the
  three-stage `--config static`, `src/hot.ld` regenerated for it in `635ec7b`, both reverted
  before the release): GCC's profile feedback from 300 generated PDFs, output byte-identical.
  Built and timed on this machine (CPU 3, 2026-11-14 11:15-11:27 UTC, against the plain build of
  the same code) it gained warm odl +2.0 % (five runs, +1.8 % to +2.1 %) and fresh -2.0 % (0.907 →
  0.892 ms), and lost on JPEG 2000 pages, a decoder the generated PDFs never reach (median 108.3 →
  112.8 ms a page); instructions odl -1.28 %, syn6 -2.78 %, SafeDocs +0.23 %. The Release workflow
  builds on GitHub's runners, where the same stages made a different binary (the plain build is
  the same bytes on both machines): timed here it was 1.4 % slower warm and 14 % slower fresh
  (1.020 against 0.895 ms) than the profile-guided binary built here, so the release would have
  missed the bar's 1 ms. A first measurement at 00:29-00:31 UTC, while unrelated services loaded
  the other CPUs, had shown +3.8 % warm.

## 0.4.0 (2026-11-15)

No round of its own: the 0.4.0 program's quality steps were each checked for their cost when they
landed (`instr.sh`, `--batch --workers=1`, images off; `arxiv_math` with `--math=latex`), and two of
them were rewritten before landing, each byte-identical to the measured rule:

|Commit|Change|Effect (`instr.sh`, the rule as written → as landed)|
|---|---|---|
|`8811572`|A table cell's emphasis measured once (`CellStyleless` returns at once for a cell that does not start with a star, and the writer cuts the delimiter length its first pass measured instead of calling it again); the rule pieces' vector sized once|the synthetic table rules' cost: odl +0.115 → +0.042 %, `tables` +0.47 → +0.14 %, SafeDocs +0.21 → +0.07 %|
|`50e3bda`|Rows of cells set to the middle: each run judged as it grows from its first line, and the search from a line stopped at the first failure no longer run escapes (a line too far below its cell's, uneven lines, a line short of its column, every column of two lines or more); 3,986 PDFs compared, none differs|`tables` +3.13 → +0.18 %, `multi_column` +0.36 → +0.02 %, odl +0.07 → +0.01 %|

The edge-row model (M9) costs odl +0.24 % (`multi_column` +0.29 %, syn6 +0.32 %): the 16 features
of each row the rules keep in a page's outer tenth and 60 trees of 15 leaves. `src/hot.ld` was
regenerated over the release's code (`a1db445`; 815 sections, 799 before; output byte-identical,
G-bytes 0 of 997 differ): CPU 3, 2026-11-15 13:37-13:39 UTC, against the same code with the old
list, warm odl 2,187 → 2,190 pages/s (medians of three runs), fresh 0.911 → 0.909 ms (12 rotated
rounds), page faults median 152 → 149.

### Result (0.3.0 against 0.4.0)

User instructions, as above, CPU 11: odl +0.32 %, heavy10 +0.17 %, syn6 +0.37 %, SafeDocs +0.22 %,
`multi_column` +0.40 %, `tables` +0.59 %, `arxiv_math` +0.46 %. Times on CPU 3, the 0.3.0 release
binary and 0.4.0's interleaved, 2026-11-15 13:39-13:44 UTC, nothing of this project running (other
services used about 8 % of the CPUs): warm odl (three runs) 2,206-2,214 → 2,188-2,189 pages/s
(-1.1 %), p95 0.692-0.730 → 0.703-0.717 ms a page; fresh 0.896 → 0.906 ms (12 rotated rounds, +1.1 %
paired), page faults median 148 → 149; `--batch --workers=1` over odl, task-clock 96.6 → 98.0 ms;
a JPEG 2000 page with its image written (40 documents, 100 pages) 107.4 → 108.8 ms median.

## Eleventh round: 0.4.0's cost won back (2026-11-15)

0.4.0 (`a1db445`) cost user instructions against 0.3.0 (`instr.sh`, `--batch --workers=1`, images off;
`arxiv_math` with `--math=latex`): odl +0.32 %, heavy10 +0.17 %, syn6 +0.37 %, SafeDocs +0.22 %,
`multi_column` +0.40 %, `tables` +0.59 %, `arxiv_math` +0.46 %. Callgrind of 0.3.0's code (`18bf2a0`)
against `a1db445` (Release with `-g`, linked with gold) over odl, `multi_column`, `tables` and
`arxiv_math`: the edge-row furniture model (`RemoveRunningLines` inclusive: odl +2.1 M, `multi_column`
+5.1 M, `tables` +2.5 M, `arxiv_math` +4.9 M; `RawScore` about a third, the rows' features the rest),
`TextGrid` +4.6 M and `RuleStacks` +1.1 M on `tables`, and on `arxiv_math` the arborescence fallback
(14.6 M, 8.5 M of it reading every glyph a second time), `JoinMathSymbols` called for every pair (3.9 M)
and the Greek-script count of every word (2.3 M). Each step byte-identical (`bench.py identical`
against `a1db445`, images off, `--math=latex`, `--tables=pipe`: 0 differ):

|Commit|Change|Effect (`instr.sh`)|
|---|---|---|
|LaTeX rebuild (`perf(layout): the LaTeX rebuild's lookups cheaper…`)|The arborescence reads the greedy rebuild's atoms; pairs tested before `JoinMathSymbols`; the Greek count only when asked; `AppendToken`'s scan only before a letter; `CommandOf`'s overrides only from U+00A0; function names by a collision-free hash; four finiteness tests as one sum|`arxiv_math` -0.585 %, the rest within 0.003 %|
|Tables (`perf(layout): table rows set to the middle…`)|`TextGrid`'s vectors from the thread's scratch, a middle row's facts in one struct, its continuation tests only while needed; `ToFrame` inlined; GFM cells appended whole without pipe or line break, dot leaders trimmed once|`tables` -0.311 %, SafeDocs -0.152 %, odl -0.084 %, syn6 -0.076 %, heavy10 -0.075 %, `multi_column` -0.050 %|
|`MeasurePage` (`perf(layout): MeasurePage tests a skipped glyph's flags once…`)|A space, dropped, clipped or image glyph found by one flag test; a one-byte glyph checked against the page text by its start alone and counted on its own path (it was 1.6 % of odl, 2.3 % of syn6, about 33 instructions a glyph)|syn6 -0.289 %, `multi_column` -0.230 %, odl -0.188 %, `tables` -0.156 %, SafeDocs -0.118 %, `arxiv_math` -0.109 %, heavy10 -0.059 %|

After the two 0.4.0 steps against 0.3.0: odl +0.233 %, heavy10 +0.092 %, syn6 +0.293 %, SafeDocs
+0.071 %, `multi_column` +0.348 %, `tables` +0.281 %, `arxiv_math` -0.141 %. With `MeasurePage`, from
the head's own profile (the largest self costs left are the content lexer, Inflate, `PlaceGlyphs`,
`ShowText` and `BuildLines`, each tuned in earlier rounds): against `a1db445` odl -0.272 %, heavy10
-0.134 %, syn6 -0.368 %, SafeDocs -0.270 %, `multi_column` -0.279 %, `tables` -0.465 %,
`arxiv_math` -0.708 %; against 0.3.0 odl +0.045 %, heavy10 +0.033 %, syn6 +0.004 %, SafeDocs
-0.047 %, `multi_column` +0.117 %, `tables` +0.123 %, `arxiv_math` -0.251 %. What is left is the
edge-row model,
whose code was being changed on another branch: an exact early exit over the forest (after each
tree, stop when the score plus the largest leaves of the trees left stays under the threshold),
the nodes packed with float thresholds (the largest float at most each double threshold gives the
same splits), and the row text and repetition key the rules already built reused for the features
would cut it. No times were taken (a fuzz campaign used the other CPUs).

Tried and rejected (2026-11-15):

- The empty-clip flag folded into the SSE2 clip test (open candidate): `BoxTerms` holding a lane
  mask ORed into the comparison in place of the flag cost more than the branch it removed: odl
  +0.192 %, syn6 +0.253 %, `multi_column` +0.220 %.
- `MeasurePage`'s single flag test without the one-byte path: odl -0.068 % (kept only with it).
- Not attempted this round: line and word texts copied once when contiguous (`AppendLineText` and
  `ForGlyphTextRuns` already append a run of contiguous glyph texts at a time; what is left is in
  the callers that copy a line's text to read it), `StyleWords` and Breuel's `Topological` (round
  10's designs gained nothing; no new design was ready).

## Twelfth round (2026-11-16)

Master `65459c1` (0.5.0 program), profiled with callgrind (Release with `-g`, linked with gold;
`--batch --workers=1`, images off; `arxiv_math` with `--math=latex`) over odl, `multi_column`,
`tables` and `arxiv_math`. The ten largest self costs of each group, per function with the code
inlined into it, in percent of the group's instructions:

|Function|odl|`multi_column`|`tables`|`arxiv_math`|
|---|---|---|---|---|
|`Interpreter::RunStream` (with the content lexer)|15.67|6.85|13.45|13.94|
|`Interpreter::ShowText`|10.28|12.08|9.74|6.11|
|`Inflater::DecodeCodesBmi2`|12.53|6.00|7.57|11.64|
|`PlaceGlyphs`|7.67|9.77|7.25|4.10|
|`BuildLines`|6.41|7.81|6.06|3.70|
|`Writer::Inline`|3.04|3.59|2.51|1.97|
|`ContentLexer::ReadArray`|1.63|1.86|1.92|2.21|
|`Detector::Run`|1.70|2.18|1.90|1.00|
|`Writer::StyleWords`|1.79|1.92|1.01|1.45|
|`Orderer::Cut`|0.69|2.49|0.79|0.89|
|`ParseDict`|1.54|1.06|1.10|1.00|
|`LoadToUnicode`|0.34|1.97|1.65|0.69|
|`Detector::Commit`|0.56|0.20|2.07|0.01|
|`MarkDisplayFormulas`|-|-|-|2.71|
|`Builder::Row`|-|-|-|2.66|
|`FindInlineFormulas`|-|-|-|2.53|
|`PrepareMathGlyphs`|-|-|-|2.08|

The first five were tuned in earlier rounds; the 0.5.0 landings' table, math and furniture code
(`SplitRowsByLines` 0.77 % of `tables` inclusive, `SplitColumnsByText` 0.73 %) holds no single
large cost. Below the top ten, three costs had cheaper equivalents: dictionary lookups
(`PdfValue::Find` alone 0.42 % of odl, 0.86 % of `arxiv_math` and 7.05 % of the SafeDocs group,
whose documents hold dictionaries of thousands of entries, besides its `memcmp` calls), the lookups
of every `Tf` (about 180,000 on `arxiv_math`: `/Font`, the name, the font cache, the page's font
ids) and the LaTeX rebuild's allocations (`malloc`, `free`, `new` and `delete` 2.76 % of
`arxiv_math`, about two fifths from `Expr`, `Layout` and `Row`). Every step byte-identical
(`bench.py identical` against `65459c1`, 0 differ): the branch head over the synthetic corpus, odl,
olmOCR-Bench, READoc development (dev, dev2) and 1,000 SafeDocs documents with option sets images
off, `--math=latex`, `--format=json`, `--plain`, `--tables=html`, `--tables=pipe`,
`--no-structure` and images on; each earlier step over the synthetic corpus, odl, olmOCR-Bench and
READoc dev with images off, `--math=latex`, `--format=json`, `--plain` and `--tables=html`.

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(pdf): a dictionary lookup tells keys apart by their last byte…`|`PdfValue::Find` tests a key's length and last byte, then compares the rest inline (keys of one dictionary share their length and first bytes: /F1 … /F12)|SafeDocs -2.815 %, `arxiv_math` -0.545 %, `multi_column` -0.341 %, syn6 -0.244 %, `tables` -0.190 %, odl -0.108 %, heavy10 -0.046 %|
|`perf(layout): Tf finds a page's fonts again…`|The page's last font per resource dictionary and name (16 slots, keyed by the dictionary's entries and the name's bytes) set without the lookups; kept only where no lookup follows a reference chain, so no limit hit is skipped|`arxiv_math` -0.775 %, syn6 -0.505 %, SafeDocs -0.430 %, `tables` -0.420 %, `multi_column` -0.247 %, odl -0.139 %, heavy10 -0.106 %|
|`perf(layout): the LaTeX rebuild's rows, expressions and layouts keep their storage per depth`|`Expr`'s items, `Layout`'s rows and `Row`'s roles, limit texts and scripts in the thread's builder scratch, one set per formula depth (a nested call is always one level deeper); a row already in order no longer goes through the sort buffer, which swapped a fresh buffer into every row|`arxiv_math` -1.652 %, the rest within 0.003 %|
|`perf(layout): a large XObject dictionary looked up again and again is searched by sorted keys`|A dictionary of 64 entries or more looked up more than 16 times on a page gets its keys sorted with their entry index (the first of repeated keys is found, as before); four such indexes a page|SafeDocs -1.185 %, `multi_column` -0.014 %, the rest within 0.007 %|

Against `65459c1`: odl -0.244 %, heavy10 -0.152 %, syn6 -0.747 %, SafeDocs -4.381 %, `multi_column`
-0.603 %, `tables` -0.609 %, `arxiv_math` -2.939 %. No times were taken.

Tried and rejected (2026-11-16, callgrind totals):

- `PdfValue::Find` with the last-byte test but the rest still compared by `memcmp`: `arxiv_math`
  -0.35 %, `multi_column` -0.25 % where the inline comparison gives -0.54 % and -0.34 % (kept the
  inline one).
- `StyleWords`' link test skipped for a word whose centre lies outside the box round its page's
  links: odl -0.020 %, `multi_column` -0.006 % (the words' boxes, built anyway, are most of the
  cost; under the 0.1 % bar).
- Not attempted: line texts read in place (`AppendLineText` with `LineText` is 0.48 % of odl,
  0.43 % of `multi_column`, 0.39 % of `tables` inclusive; the copies put in the word-break spaces
  the page text lacks, so a view serves only lines of one word), and Breuel's `Topological`
  (`Orderer::Cut` 2.49 % of `multi_column` self, spread over its pair tests; no design beyond round
  10's).

## Thirteenth round (2026-11-16)

The quality landings after the twelfth round's head (`a1163bc`) up to `59c95c1` cost user
instructions (`instr.sh`, `--batch --workers=1`, images off; `arxiv_math` with `--math=latex`):
odl +0.233 %, heavy10 +0.105 %, syn6 +0.159 %, SafeDocs +0.183 %, `multi_column` +0.257 %, `tables`
+0.731 %, `arxiv_math` +1.396 %. Callgrind of both (Release with `-g`, linked with gold) over odl,
`multi_column`, `tables` and `arxiv_math`; the growth per function, inclusive, with the function's
share of its group at `59c95c1`:

|Group|Growth|Where|
|---|---|---|
|`tables`|+7.75 M|`TextGrid` +4.59 M (1.12 %): the formula-grid tests (relation-sign column, tall-bar pieces) copied every word's text, `AppendGlyphTexts` +2.04 M, `strlen` +0.92 M; `SplitHeaderLevels` +1.85 M (0.17 %); the edge rows +0.58 M (`IsMetadata`, pages with text only at their edges); `PlaceGlyphs` +0.55 M (the `\cong` branch of the lone-script join)|
|odl|+1.89 M|`TextGrid` +0.62 M; `SplitHeaderLevels` +0.53 M (0.07 %); the edge rows +0.37 M (`IsMetadata` 0.07 % self); `PlaceGlyphs` +0.14 M|
|`multi_column`|+4.41 M|`BuildBlocks` +1.66 M (the OCR layer's centred headings and type sizes about 0.8 M); `TextGrid` +1.03 M; the edge rows +0.87 M; `SplitHeaderLevels` +0.74 M|
|`arxiv_math`|+76.3 M|`PrepareMathGlyphs` +59.0 M (3.47 % of the group): its `hanging` lambda runs out of line for each of the 1.17 M glyphs, 55.6 M, 24.7 M of it the call's entry and exit; the LaTeX rebuild (`BuildLatex`) +21.5 M, `Builder::Row` +14.5 M of it (extensible arrows +6.5 M, variation selectors +3.5 M, `\cong` +2.7 M); `IsRelation` +2.1 M, now out of line|

`src/layout/math.cpp` changed on another branch during this round, so its costs (+71.4 M self,
1.28 % of `arxiv_math`) stay open candidates. The step is byte-identical (`bench.py identical`
against `59c95c1` over the synthetic corpus, odl, olmOCR-Bench and READoc dev and dev2, 2,737
documents, with images off, `--math=latex`, `--format=json`, `--plain`, `--tables=html` and
`--tables=pipe`: 16,422 comparisons, 0 differ):

|Step|Change|Effect (`instr.sh`, against `59c95c1`)|
|---|---|---|
|`perf(tables): the formula-grid tests read a word's first bytes, no text copy`|The relation-sign and tall-bar tests read a word's length and first three bytes from `Chars` (read for every word by the cells' tests anyway) instead of copying its text; `RelationSign`'s signs are `string_view`s, tried only for texts of one to three bytes (it took `strlen` of each of its 18 signs per call)|`tables` -0.336 %, odl -0.060 %, heavy10 -0.050 %, syn6 -0.044 %, `multi_column` -0.043 %, SafeDocs -0.014 %, `arxiv_math` -0.001 %|

Against `a1163bc`: odl +0.173 %, heavy10 +0.055 %, syn6 +0.115 %, SafeDocs +0.169 %, `multi_column`
+0.214 %, `tables` +0.393 %, `arxiv_math` +1.394 %. No times were taken.

Tried and rejected (2026-11-16, `instr.sh` against the step):

- `SplitHeaderLevels` building no labels for a header row of one line (no label over another
  line's run there): `tables` -0.023 %, odl -0.006 %. The rest is the header rows' words gathered
  and located per row; the whole function is 0.17 % of `tables`.
- `IsMetadata` skipping bytes no keyword starts with through a byte table, and read only where it
  decides (a row shaped like a strong one, or after a wrapped stamp's last line): odl -0.025 %,
  `multi_column` -0.026 %, `tables` -0.012 %.
- Not attempted, measured under the bar: `SplitColumnsByText` scanning every located word once per
  column (0.8 M of `tables`, under 0.08 % with the words bucketed by column); the `\cong` branch's
  cost in `PlaceGlyphs` (0.05 % of `tables`, code layout); `BuildBlocks`' OCR heading tests (about
  0.05 % of `multi_column`, work of their own).

## Fourteenth round: the LaTeX rebuild's cost won back (2026-11-17)

The `arxiv_math` group (`instr.sh`, `--batch --workers=1`, images off, `--math=latex`) grew from
the twelfth round's head `a1163bc` (5,444.5 M user instructions) to master `76ed0ef` (5,528.8 M,
+1.55 %), and the branch `b20/mathfails4` (`ec1902e`: operator names, text-italic variables,
call operands) took it to 5,576.4 M (+0.86 %); the other groups did not move with it. Callgrind
of the three (Release with `-g`, linked with gold) over `arxiv_math`; self costs, in millions,
with the share of the group at `ec1902e` (5,632.8 M under callgrind):

|Function|`a1163bc`|`76ed0ef`|`ec1902e`|Share|Why it grew|
|---|---|---|---|---|---|
|`PrepareMathGlyphs`' `hanging` lambda|-|54.9|54.9|0.97 %|called out of line for each of the 1.18 M glyphs; its first tests (direction, flags, font class) reject most|
|`PrepareMathGlyphs`|117.8|121.7|121.7|2.16 %|the variation-selector test reads every glyph's math font|
|`IsOperatorName` with its `memcmp`|-|-|25.6|0.45 %|16 `string_view` comparisons for each of 104,000 words (`IsFunctionWord` in `FindInlineFormulas` and `MarkDisplayFormulas`)|
|`FindInlineFormulas`|132.0|132.6|148.6|2.64 %|the text-italic variable rule walks every word's glyphs, with or without scripts|
|`Builder::Row` (both depths)|131.2|145.2|146.6|2.60 %|the `\cong` and extensible-arrow searches walk every row|
|`IsRelation`|-|2.2|2.2|0.04 %|out of line since `a1163bc`|

Every step byte-identical (`bench.py identical` against the step before over the synthetic corpus,
odl, olmOCR-Bench (`arxiv_math` included) and READoc dev and dev2, 2,737 documents, with
`--images=none --math=latex`, `--images=none` and `--format=json --images=none --math=latex`:
8,211 comparisons a step, 0 differ):

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(math): PrepareMathGlyphs calls its hanging-ink test only for extension and symbol font glyphs`|The glyph's math font class, read once per glyph, decides before the call; the variation-selector test reads it too|`arxiv_math` -0.959 %, the rest within 0.002 %|
|`perf(math): operator names found by the function names' hash, one key per function word`|`IsOperatorName` looks its word up in a 64-slot table under `kFunctionHash` (no collision, checked at compile time) as `IsFunctionName` does; `IsFunctionWord` packs the word once and compares keys|`arxiv_math` -0.533 %, the rest within 0.002 %|
|`perf(math): FindInlineFormulas' italic-variable rule and Row's searches run only where their trigger glyphs are`|The italic rule only in words holding a raised or lowered glyph (-0.175 %); `Row` counts, while it lists its sort keys, what each search needs: a ∼ and an = (`\cong`), a bar and an arrowhead (extensible arrows), three dots, a slash or one of →, ←, ⇒, ⇐ (every pair `JoinMathSymbols` and `\mapsto` join holds one) (-0.244 %)|`arxiv_math` -0.418 %, the rest within 0.002 %|

Against `ec1902e`: `arxiv_math` -1.899 % (5,470.5 M), the other groups within 0.004 %; against
`76ed0ef` -1.054 %, against `a1163bc` +0.477 %. Callgrind at the round's head (5,526.6 M): the
`hanging` lambda 0.08 %, `IsOperatorName` gone into `IsFunctionWord`, `FindInlineFormulas` 2.52 %,
`Builder::Row` 2.42 % (both depths), `PrepareMathGlyphs` 2.15 %. No times were taken.

Tried and rejected, or not attempted (2026-11-17):

- `Row`'s searches gated on a looser test (a ∼; a bar or ←; any dot; any symbol `MayJoin` takes):
  `arxiv_math` -0.056 %; most rows hold a minus or an equals sign. Kept in the tighter form above.
- Not attempted, under the bar by their callgrind cost: `CommandOf`'s linear scan of its 35
  overrides (7.4 M inclusive, 34,000 calls; a binary search would save about 2.7 M, 0.05 %);
  `IsRelation` inline again (2.2 M); `Row`'s `writeScripts` lambda out of line (1.6 M self); the
  rest of the growth since `a1163bc` lies outside `math.cpp` (`PlaceGlyphs`' `\cong` branch of the
  lone-script join, 3.2 M).

## Fifteenth round: the footnote and furniture landings won back (2026-11-17)

From the twelfth round's head `a1163bc` to master `0dbbb73` (the thirteenth and fourteenth
rounds, the footnote rules of `76ed0ef`, the furniture rules of `ee6a711` and the edge-row model
v3) user instructions grew (`instr.sh`, `--batch --workers=1`, images off; `arxiv_math` with
`--math=latex`; two runs of one binary differ by 0.002 % at most): odl +0.373 %, heavy10
\+0.142 %, syn6 +0.333 %, SafeDocs +0.322 %, `multi_column` +0.464 %, `tables` +0.624 %,
`arxiv_math` +0.473 %. Callgrind of both (Release with `-g`, linked with gold) over odl,
`multi_column`, syn6 and SafeDocs; the growth per function, inclusive, with the function's share of
its group at `0dbbb73`:

|Function|odl|`multi_column`|syn6|SafeDocs|Why it grew|
|---|---|---|---|---|---|
|`PlaceGlyphs`|+0.73 M (8.39 %)|+2.25 M (10.77 %)|+0.12 M (12.24 %)|+1.28 M (5.97 %)|`RaisedNoteMark` called from the glyph loop: two more instructions a glyph for the loop's registers|
|`WriteMarkdown`|+0.76 M (7.34 %)|+1.28 M (8.02 %)|+0.14 M (9.90 %)|+0.87 M (5.02 %)|`markdown_writer.cpp` at GCC's inlining budget for the unit: `Writer::Inline`'s space before each word became a call of `push_back` (+0.72 M on odl); the "(a)" reference tests per glyph (+0.27 M)|
|`DetectTables`|+0.72 M (7.41 %)|+1.06 M (8.77 %)|+0.10 M (4.92 %)|+2.20 M (7.96 %)|`SplitHeaderLevels` +0.53 M (0.07 %), +0.74 M, +0.10 M, +2.19 M (0.14 %); `TextGrid` +0.14 M on odl|
|`RemoveEdgeRows`|+0.50 M (1.07 %)|+1.35 M (1.14 %)|+0.05 M (0.68 %)|+0.80 M (0.56 %)|`IsMetadata` out of line, `IsCaption` on each row read and on the model's candidates, the page's lowest row read, `IsRuleLine`|
|`BuildBlocks` (`semantics.cpp`)|+0.15 M (3.86 %)|+2.10 M (4.87 %)|+0.02 M|-0.14 M|the footnote rules (`FindNotes`, `PlaceNotes`, table-cell references) and the OCR layer's headings|
|`LineInfo` (`semantics.cpp`)|+0.14 M (1.45 %)|+0.44 M (2.11 %)|+0.04 M|+0.14 M|the notes' marks (`Marks::letterNotes`)|

`semantics.cpp` and `math.cpp` changed on other branches during this round and stay as they are.
The steps were measured on master `fabe200` (`0dbbb73` with `--small-caps=keep`). Every step is
byte-identical (`bench.py identical` against the step before over the synthetic corpus, odl,
olmOCR-Bench and READoc dev and dev2, 2,737 documents, with `--images=none` alone and with
`--math=latex`, `--format=json`, `--plain`, `--tables=html` and `--tables=pipe` beside it: 16,422
comparisons a step, 0 differ):

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(lines): a note's mark before a tab is tested once a line is read, not in the glyph loop`|The glyph loop keeps the line's first wide gap (its glyph and width, when no segment ends before it); `RaisedNoteMark` runs after the line's last glyph with the same operands|syn6 -0.132 %, `multi_column` -0.095 %, odl -0.081 %, `tables` -0.062 %, SafeDocs -0.050 %, `arxiv_math` -0.042 %, heavy10 -0.023 %|
|`perf(writer): markdown_writer.cpp with a larger inlining budget; Inline reads the letter-reference flag once`|GCC's `--param inline-unit-growth=60` for that file (CMake, GCC only): at the default 40 % `-fopt-info-inline-missed` listed 120 calls left out of line for want of budget, none at 60; `letterRefs_` copied to a local (`out_`'s writes made the loop load it again for every glyph)|`arxiv_math` -0.182 %, syn6 -0.128 %, `multi_column` -0.101 %, odl -0.092 %, SafeDocs -0.065 %, `tables` -0.057 %, heavy10 -0.044 %|
|`perf(layout): the edge rows' caption and metadata tests skip texts by their first bytes`|`IsCaption` returns at once for a text whose first byte starts no label; `IsMetadata` tries its words only where a byte and the next are a word's first two (a 256-entry table of second bytes), not at every byte whose letter starts one|`multi_column` -0.056 %, odl -0.052 %, syn6 -0.040 %, `tables` -0.038 %, SafeDocs -0.020 %, `arxiv_math` -0.018 %, heavy10 -0.012 %|

Against `fabe200`: odl -0.225 % (787.79 M), heavy10 -0.079 %, syn6 -0.299 %, SafeDocs -0.136 %,
`multi_column` -0.251 %, `tables` -0.157 %, `arxiv_math` -0.242 %; against `a1163bc` odl +0.148 %,
heavy10 +0.064 %, syn6 +0.033 %, SafeDocs +0.188 %, `multi_column` +0.212 %, `tables` +0.468 %,
`arxiv_math` +0.230 %. No times were taken.

Tried and rejected, or not attempted (2026-11-17, `instr.sh`-like counts of the Release build with
`-g`):

- The word's space written through an always-inlined helper marked `flatten`: odl +0.000 %,
  `multi_column` -0.001 %. GCC inlines the helper into `Inline` before flattening it, so
  `push_back` stays a call; the unit's budget is what decides.
- `letterRefs_` read once without the larger budget: odl +0.025 %, `multi_column` +0.031 %
  (other calls of the unit went out of line instead). With the budget it is part of the second step
  (odl -0.032 % of its -0.092 %).
- Not attempted, under the bar by their callgrind cost: `SplitHeaderLevels` (0.53 M on odl over
  214 lattices, 2,500 instructions a call; its vectors' allocations 0.12 M, `Chars` of each header
  word 0.02 M; 0.14 % of SafeDocs, spread over its loops); `TextGrid` (+0.14 M on odl); the rest of
  `RemoveEdgeRows`' row texts (`AppendLineText` 1.24 M, `RunningKey` 1.02 M and `IsPageNumberKey`
  0.57 M on odl: read for every row in `kNumberBand`, and kept for the edge-row model's rows).
- Left for a later round (callgrind at `0dbbb73`): `semantics.cpp`, self costs, `LineInfo` 1.18 %
  of odl, 1.67 % of `multi_column`, 0.77 % of `arxiv_math`; `BuildBlocks` 0.84 %, 1.32 %, 0.47 %
  (inclusive 3.86 %, 4.87 %, 2.23 %); `PlaceNotes` 0.14 % of odl; `Continues` 0.15 % and `NoTitle`
  0.11 % of `multi_column`. `math.cpp` on `arxiv_math`: self, `MarkDisplayFormulas` 2.76 %,
  `FindInlineFormulas` 2.68 %, `Builder::Row` 2.66 %, `PrepareMathGlyphs` 2.23 %, `ResolveAtom`
  0.97 %, `Builder::Expr` 0.55 %, `Item`'s `emplace_back` 0.53 %, `AddMathRules` 0.45 %;
  inclusive, `BuildLatex` 8.37 %, `Builder::Expr` 5.98 %, `Builder::Layout` 4.83 %. `math.cpp` is
  the one other unit at GCC's inlining budget: 130 calls left out of line for want of it.

## Sixteenth round: blocks and the LaTeX rebuild's unit (2026-11-17)

Master `4dffacb`, profiled with callgrind (Release with `-g`, linked with gold; `--batch
--workers=1`, images off; `arxiv_math` with `--math=latex`) over odl, `multi_column`, safe40 and
`arxiv_math`, confirmed the fifteenth round's open costs. Self costs, the code inlined into each
function counted with it, in millions and in percent of the group:

|Function|odl|`multi_column`|safe40|`arxiv_math`|
|---|---|---|---|---|
|`LineInfo`|9.51 (1.20 %)|29.32 (1.69 %)|14.65 (0.96 %)|43.24 (0.78 %)|
|`BuildBlocks` (with its lambdas)|6.69 (0.84 %)|22.78 (1.31 %)|11.31 (0.74 %)|26.23 (0.48 %)|
|`SplitHeaderLevels` (inclusive)|0.53 (0.07 %)|0.74 (0.04 %)|2.19 (0.14 %)|-|
|`TextGrid` (inclusive)|1.29 (0.16 %)|3.03 (0.17 %)|-|-|

`LineInfo` spent most of its instructions in the glyph loop past a line's first letter (about 19
a glyph on `multi_column`: the bold and italic counts, the raised-glyph test, the monospace test)
and in its first word's text (`AppendGlyphTexts`, 188 instructions a line, and two `ParseMarker`
calls). `math.cpp` left 130 calls out of line for want of GCC's unit budget
(`-fopt-info-inline-missed`, "inline-unit-growth limit"), none at 60 %. Every step is
byte-identical (`bench.py identical` against the step before over the synthetic corpus, odl,
olmOCR-Bench and READoc dev and dev2, 2,737 documents, with `--images=none` alone and with
`--math=latex`, `--format=json`, `--plain`, `--tables=html`, `--tables=pipe` and
`--small-caps=keep` beside it: 19,159 comparisons a step, 0 differ):

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(semantics): LineInfo counts faces at once and reads the first word in place`|The bold and italic glyphs counted by one table add a glyph (the two face bits index counters packed in one 64-bit word); the first word read in place in the page text when its glyph texts follow each other there (`ForGlyphTextRuns`' test), copied only otherwise; `ParseMarker` of the first glyph alone called only for a text of three bytes or fewer starting with a byte a bullet or task box starts with|syn6 -0.321 %, `multi_column` -0.314 %, odl -0.233 %, `tables` -0.139 %, SafeDocs -0.135 %, `arxiv_math` -0.118 %, heavy10 -0.065 %|
|`perf(semantics): BuildBlocks reads a line's text for formula pieces and float captions only where it can decide`|A block that may be a piece of a display formula has its text read (`LineText`, `NoTitle`, `MathSymbol`) only beside another candidate (alone it joins nothing); a paragraph's float-caption test reads its text only when the line's first byte (`FirstTextByte`) starts one of `IsCaption`'s labels|syn6 -0.110 %, odl -0.081 %, `multi_column` -0.071 %, `arxiv_math` -0.070 %, SafeDocs -0.068 %, `tables` -0.049 %, heavy10 -0.021 %|
|`perf(math): math.cpp with a larger inlining budget and -O3's limit for functions not declared inline`|GCC's `--param inline-unit-growth=60` and `--param max-inline-insns-auto=30` (-O3's value; -O2's is 15) for that file (CMake, GCC only)|`arxiv_math` -0.261 %, the rest within 0.003 %|

Against `4dffacb`: odl -0.313 % (785.44 M), heavy10 -0.086 %, syn6 -0.427 %, SafeDocs -0.202 %,
`multi_column` -0.384 %, `tables` -0.185 %, `arxiv_math` -0.449 %. No times were taken.

Tried and rejected, or not attempted (2026-11-17, `instr.sh` against the step before):

- `math.cpp` with the unit budget alone (`inline-unit-growth=60`): `arxiv_math` -0.116 %, under the
  group's 0.2 % bar. With `max-inline-insns-auto` beside it: 20 -0.194 %, 30 -0.264 %, 40
  -0.359 %, 60 -0.489 %, 100 -0.365 % (the binary 10 to 21 KB larger). Kept at -O3's 30, a value
  GCC itself uses, rather than a value fitted to this one group (100 gains less than 60).
- Not attempted, under the bar by their callgrind cost: `SplitHeaderLevels` (460 lattices on
  safe40 at 4,800 instructions a lattice; its own lines 1.0 M, `Chars` of the header words
  0.33 M, the rest its vectors' allocations and `HeadedRun`; what a rewrite could leave out, the
  vectors kept in the thread's scratch, is about 0.3 M, 0.02 %); `TextGrid` (28 grids on odl at
  38,000 instructions a grid, spread over some forty loops of which none holds 0.13 M; the
  formula-grid tests' `Chars` calls are cache hits, 0.16 M); the rest of `BuildBlocks` (on
  `multi_column` the card titles' column look 2.4 M, the OCR layer's centred-heading test 1.3 M and
  its caption test 0.9 M, `Continues` 2.3 M and `NoTitle` 1.8 M, each spread over its tests);
  `MoveAddresses` (0.70 M on odl: its title blocks' texts are read whole before the capitals test).

## Seventeenth round: the LaTeX path after the fraction landing (2026-11-17)

Master `b7f7045` (stacked fractions, radicals over fractions, readings by font and spacing) had
raised `arxiv_math` by 0.38 %. Profiled with callgrind (Release with `-g`, linked with gold;
`--batch --workers=1`, images off, `--math=latex`) over `arxiv_math` (5,509.6 M instructions
under callgrind), and over `multi_column` for the default path's share of `math.cpp`. The code
`b7f7045` added is under half of its cost: the fraction parts' walls and connected stacks
(`Connected` 0.96 M self, the fraction lines of `Expr` 4.93 M), the radical's rule search (2.0 M)
and the spacing readings (under 0.1 M) are about 0.16 % of the group together. Self costs, the
code inlined into each function counted with it, in millions and in percent of the group:

|Function|`arxiv_math`|
|---|---|
|`Builder::Row`|153.08 (2.78 %)|
|`MarkDisplayFormulas` (with `FactsOf`)|152.87 (2.77 %)|
|`FindInlineFormulas`|147.62 (2.68 %)|
|`PrepareMathGlyphs`|123.39 (2.24 %)|
|`Builder::ResolveAtom`|62.32 (1.13 %)|
|`Builder::Expr`|37.24 (0.68 %)|
|`AddMathRules`|27.15 (0.49 %)|
|`std::vector<Item>::emplace_back`|26.92 (0.49 %)|
|`AddMathGlyph`|17.83 (0.32 %)|
|`FindRows`|17.33 (0.31 %)|
|`EmOf`|14.87 (0.27 %)|

Inclusive: `BuildLatex` 466.88 M (8.47 %), `Builder::Expr` 341.76 M (6.20 %), `Builder::Layout`
277.90 M (5.04 %), `Builder::Row` 222.12 M (4.03 %). On `multi_column` (default output)
`math.cpp` holds 1.12 M (`MathFontOf`, 0.06 %): the default path pays for next to none of it.
`FactsOf` built a string of each line's first ten glyph texts and counted the words of every line
with math to the end; `Row` sorted rows already in order, and searched every item for each
operator's limits; `Resolve` destroyed and built its atoms for each formula. Every step is
byte-identical (`bench.py identical` against the step before over the synthetic corpus, odl,
olmOCR-Bench with `arxiv_math`, READoc dev and dev2 and OmniParseBench's pages, 4,886 documents,
with `--images=none` alone and with `--math=latex`, `--math=latex --plain`, `--math=latex
--tables=pipe`, `--format=json` and `--math=latex --format=json` beside it: 29,316 comparisons a
step, 0 differ):

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(math): MarkDisplayFormulas counts a long line's words only while they decide, and reads the equation number as it goes`|A line of more than 3 glyphs stops counting its words once they make it no candidate (one word with fewer than 5 math glyphs, two with fewer than 8, three); the equation-number test reads the bytes of the first ten glyphs as they come instead of appending them to a string|`arxiv_math` -0.759 %, the rest within 0.004 %|
|`perf(math): Row leaves items already in order unsorted and looks for limits among the row's scripts only`|Items each before the next (u0, then v downwards, no two equal) are the one order `std::sort` can give: kept without sorting; the limits' search visits the row's scripts (roles 1 and 2, the only ones it takes) in order, and none without scripts|`arxiv_math` -0.467 %, the rest within 0.003 %|
|`perf(math): Row passes over accents, braces and upright letters only in rows that hold them, and takes em from Layout`|The first pass over a row notes whether any item is an accent, a horizontal brace or an upright letter (no later step makes one); Layout hands Row the `EmOf` it computed of the same items|`arxiv_math` -0.260 %, the rest within 0.003 %|
|`perf(math): a formula's atoms made anew in place, with HangingInk's and AccentOf's tests first, and its glyphs and bars added more cheaply`|`Resolve` keeps the thread's atoms and `ResolveAtom` resets their fields; `HangingInk` called only for an extension font's glyph or a symbol font's radical, `AccentOf` only from U+00A8; `CommandOf`'s 35 overrides a switch instead of a scan; a one-byte glyph text appended as a byte; the bar limit tested as each bar is added|`arxiv_math` -0.452 %, the rest within 0.003 %|
|`perf(math): FindInlineFormulas counts a word's letters only when a glyph asks, and works out each class before storing it`|A word's letters decide only whether an italic glyph `ClassOf` takes for text stands alone, and the Greek test: counted at the first such glyph instead of for every word; each class worked out in a local and stored once (a byte store may alias what the loop reads)|`arxiv_math` -0.637 %, the rest within 0.007 %|

Against `b7f7045`: `arxiv_math` -2.548 % (5,315.47 M), odl +0.001 % (785.50 M), heavy10 0.000 %,
syn6 +0.012 %, SafeDocs -0.001 %, `multi_column` 0.000 %, `tables` -0.003 %. No times were
taken.

Tried and rejected (2026-11-17, `instr.sh`'s `arxiv_math` against the step before; the bar is
0.2 % of the group):

- `FindInlineFormulas` with its word starts, classes and per-word flags in the thread's scratch,
  the second pass reading a word's strong and text flags from the first: -0.063 %.
- `PrepareMathGlyphs`' accent search. Of 1,256 unsorted walks of the glyph list on the group, 210
  serve a page's first accent (497,000 glyphs visited) and 1,046 its 2nd to 16th (2.43 M). The
  accents found in the main pass (no list of every non-ASCII glyph) with the list walked by blocks
  of 16 bounded in x only: -0.037 % (in x only, without the main pass's part: +0.013 %: every line
  of the page has glyphs at the accent's x). Blocks bounded in x, baseline and size as well
  (float subtraction keeps the order, so a block passed over holds no glyph the test takes): 8
  glyphs -0.098 %, 16 -0.129 %, 32 -0.142 %. The first accent of a page looked up by walking the
  glyphs, the list made for the second: +0.008 %.
- `FactsOf`'s counts and band in locals, stored once into the line's facts: +0.088 % (both loops),
  +0.096 % (the first only).
- One-byte tokens appended as a byte in `AppendToken`, and a one-byte atom's LaTeX copied as a
  byte in `ItemOf`: -0.075 %.

## Eighteenth round: the compound lexicon's cost won back (2026-11-17)

Master `c77383a` (compound hyphens kept at line ends with a per-document lexicon of in-line
compounds, hyphen-broken lines joined in `semantics.cpp`, a caption rule in `Order`) raised user
instructions over `16565f7` (`instr.sh`, `--batch --workers=1`, images off; `arxiv_math` with
`--math=latex`): odl +0.173 %, heavy10 +0.090 %, syn6 +0.204 %, SafeDocs +0.161 %,
`multi_column` +0.407 %, `tables` +0.151 %, `arxiv_math` +0.089 %. Callgrind of both (Release with
`-g`, linked with gold) over odl, `multi_column`, syn6 and safe40; what `c77383a` added, in
millions and in percent of the group at `c77383a`:

|Code|odl|`multi_column`|syn6|safe40|Why it costs|
|---|---|---|---|---|---|
|`KnownCompound` (inclusive: the lexicon and the line ends' queries)|1.51 (0.19 %)|7.41 (0.43 %)|0.32 (0.25 %)|2.77 (0.18 %)|Built at the first query: for every "-" of the pages' texts the glyph found by `lower_bound` (about 120 instructions), both letter runs read through `GlyphText` and the out-of-line `WordLetter`, then hashed in a second pass|
|`AddFragment` (inclusive)|0.12|0.57|0.00|0.16|Two `find` calls over each glyph text of the word before a line-end hyphen|
|`LineInfo` (self, growth)|+0.32|+0.92|+0.08|+0.36|Not the hyphen-break test (0.04 M on `multi_column`): GCC's register allocation left the glyph loop past the line's first letter one instruction a glyph longer|
|`Orderer::Order` and `Cut` (self, growth)|+0.04|+0.76|0.00|+0.12|`Beside`, and Breuel's order over the bands the caption rule now forms (output that changed on purpose: kept)|

The costliest functions otherwise, self, in percent of the group at `c77383a`: odl `RunStream`
(the content lexer) 12.2 %, `DecodeCodesBmi2` 12.0 %, `PlaceGlyphs` 7.2 %, `ShowText` 6.6 % (and
1.6 % in its SSE code), `BuildLines` 6.4 %, `Writer::Inline` 1.9 %, `MeasurePage` 1.4 %,
`StyleWords` 1.3 %, `CollectWords` 1.0 %, `LineInfo` 1.0 %; `multi_column` `PlaceGlyphs` 9.2 %,
`ShowText` 7.8 %, `BuildLines` 7.8 %, `DecodeCodesBmi2` 5.8 %, `RunStream` 5.0 %, `Inline`
2.2 %, `MeasurePage` 1.6 %, `LineInfo` 1.4 %, `StyleWords` 1.3 %. Every step is byte-identical
(`bench.py identical` against the step before over the synthetic corpus, odl, olmOCR-Bench and
READoc dev and dev2, 2,737 documents, with `--images=none` alone and with `--math=latex`,
`--format=json`, `--plain`, `--tables=html`, `--tables=pipe`, `--tables=pipe-latex`,
`--quotes=ascii` and `--small-caps=keep` beside it: 24,633 comparisons a step, 0 differ):

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(writer): the compound lexicon keeps the hyphens between letters and reads a pair only when a line end asks for its bytes`|The lexicon keeps each in-line "-" between two letters on its baseline as a candidate, sorted by the folded bytes on either side of it; a candidate's runs are read and hashed (the old per-hyphen test) only when a line-end pair with the same bytes asks, once a candidate. Equal folded texts with no "-" but the hyphen have equal bytes; a line-end pair whose glyph texts hold a "-" of their own is compared with every candidate; over `kMaxCompounds` candidates the old sorted hashes are kept. Letter tests and hashes read a one-byte ASCII glyph text in place; `AddFragment` reads its text once|`multi_column` -0.137 %, syn6 -0.089 %, odl -0.083 %, SafeDocs -0.072 %, `tables` -0.062 %, `arxiv_math` -0.052 %, heavy10 -0.042 %|
|`perf(semantics): LineInfo follows a line's leading raised glyphs in a loop of their own`|Past the first letter, one loop while the line's leading raised glyphs last, then one that tests each glyph's faces, monospace flag and raise with no other state|syn6 -0.090 %, `multi_column` -0.068 %, odl -0.058 %, `arxiv_math` -0.041 %, `tables` -0.034 %, SafeDocs -0.031 %, heavy10 -0.016 %|

Against `c77383a`: odl -0.141 % (785.76 M), heavy10 -0.058 %, syn6 -0.180 %, SafeDocs -0.103 %,
`multi_column` -0.206 %, `tables` -0.096 %, `arxiv_math` -0.087 %; against `16565f7` odl +0.032 %,
heavy10 +0.033 %, syn6 +0.024 %, SafeDocs +0.058 %, `multi_column` +0.201 %, `tables` +0.055 %,
`arxiv_math` +0.002 %. After the round `KnownCompound` holds 0.75 M of odl (0.09 %), 3.74 M of
`multi_column` (0.22 %), 0.16 M of syn6 and 1.61 M of safe40, of which `InnerHyphen` (the glyph
of each "-" by `lower_bound`) 0.14 M, 0.78 M, 0.04 M and 0.33 M; the rest is the scan of the
pages' texts (`memchr`) and the line ends' own runs. Callgrind of the dynamic `-g` build also
counted 1.1 M more in `memcpy` on odl after the first step: four more large `realloc` copies (the
heap's layout), absent from `instr.sh`'s static binaries. No times were taken.

Tried and rejected, or not attempted (2026-11-17, `instr.sh` against the step before):

- The first step without the candidates (the letter tests and hashes reading one-byte texts in
  place, the right run hashed as it is read, `AddFragment` in one pass): odl -0.035 %,
  `multi_column` -0.072 %, under the bar alone; it is part of the step.
- `LineInfo` with the leading and raised states in the loops' conditions (`k < count && lead`,
  `k < count && !raised`): odl +0.010 %, `multi_column` +0.019 % (GCC combined the conditions
  with `setcc` and `and` on every glyph).
- Not attempted: `InnerHyphen` searching from the previous hyphen's glyph (0.78 M on
  `multi_column`, 0.05 %, under the bar). It gives `lower_bound`'s glyph only when the glyphs'
  text offsets never go down, which ActualText breaks (a span whose first glyph is dropped hands
  its text to a later glyph). `PlaceGlyphs`, `BuildLines` and the writer's `Inline` and
  `StyleWords` stay as earlier rounds left them; the extra Breuel order is output that changed.

## Nineteenth round: the outline lookup and the line builder's glyph loops (2026-11-17)

The landings after `54884b5` (DocLayNet heading rules, division labels, two table rounds) raised
user instructions to `a4f94c8` (`instr.sh`, `--batch --workers=1`, images off; `arxiv_math` with
`--math=latex`): odl +0.060 %, heavy10 +0.024 %, syn6 +0.166 %, SafeDocs +0.069 %,
`multi_column` +0.024 %, `tables` +0.094 %, `arxiv_math` +0.003 %. Callgrind of both (Release with
`-g`, linked with gold) over odl, syn6, `multi_column` and safe40: in `semantics.cpp`,
`reading_order.cpp` and `lines.cpp` the growth is `BuildBlocks` (odl +0.14 M self, `multi_column`
+0.46 M: the italic title and division label tests) and `MatchKey` (syn6 +0.15 M, SafeDocs +0.32
M: more blocks reach the outline lookup); the rest of it is in `tables.cpp` (`Detector::Commit`).
The costliest code of the three files, self, in percent of the group at `a4f94c8`: odl
`PlaceGlyphs` 7.2 %, `BuildLines` (with `CollectAndFold` inlined) 6.4 %, `Folder::Place` 0.9 %,
`LineInfo` 0.9 %, `BuildBlocks` 0.6 %; syn6 `PlaceGlyphs` 10.5 %, `BuildLines` 9.6 %, `MatchKey`
1.9 % (the outline lookup, 3.7 % inclusive with its `LineText` calls); `multi_column` `PlaceGlyphs`
9.2 %, `BuildLines` 7.8 %, `Orderer::Cut` 1.9 %, `LineInfo` 1.3 %, the gutter test of
`PlaceGlyphs` 1.1 %. In `PlaceGlyphs` two lines stood out: the size bucket count (3.1 M on odl,
a load, an add, a store and the leader's load at every glyph) and the fragment join test (4.3 M
and 1.4 M for the script test, worked out for every glyph). Every step is byte-identical
(`bench.py identical` against the step before over the synthetic corpus, odl, olmOCR-Bench and
READoc dev and dev2, 2,737 documents, with `--images=none` alone and with `--math=latex`,
`--format=json`, `--plain`, `--tables=html`, `--tables=pipe`, `--tables=pipe-latex`,
`--quotes=ascii` and `--small-caps=keep` beside it: 24,633 comparisons a step, 0 differ):

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(semantics): outline titles matched with one key pass a text, cut short once no title can match`|The key is built line by line with the text (a space adds nothing to it); the reading stops once it outgrows the longest title key by more than a numbering holds (72 bytes); keys longer than the longest title skip the hash lookup; the stripped key is the rest of the whole one after the numbering's letters and digits|syn6 -1.487 %, SafeDocs -0.198 %, the rest within 0.004 %|
|`perf(lines): a line's size buckets counted a run of equal sizes at a time`|A run of glyphs in one bucket counts in a register and is added at its end, where the lead is decided once (within a run only its own count grows)|odl -0.576 %, heavy10 -0.171 %, syn6 -0.908 %, SafeDocs -0.355 %, `multi_column` -0.685 %, `tables` -0.453 %, `arxiv_math` -0.295 %|
|`perf(lines): a fragment's join test leaves the script test to glyphs off its baseline`|Within 0.6 em, "on the baseline, a script or a space" is "on the baseline, a space, or smaller than the glyph before or the fragment": the size question only for a glyph off the baseline, the distance read once|odl -0.325 %, heavy10 -0.090 %, syn6 -0.506 %, SafeDocs -0.204 %, `multi_column` -0.383 %, `tables` -0.272 %, `arxiv_math` -0.163 %|
|`perf(lines): a fragment's end set as it closes, not at each glyph that joins it`|Every glyph after a fragment's first joins it until one opens the next: its end is that glyph's index, which frees a register in the loop|odl -0.138 %, heavy10 -0.039 %, syn6 -0.214 %, SafeDocs -0.085 %, `multi_column` -0.163 %, `tables` -0.115 %, `arxiv_math` -0.077 %|
|`perf(lines): a line's first glyph measured before the loop over the others`|Leading spaces passed over and the first glyph's segment, extent and size bucket opened before the loop, which then asks no "first glyph?" question|odl -0.185 %, heavy10 -0.053 %, syn6 -0.287 %, SafeDocs -0.120 %, `multi_column` -0.223 %, `tables` -0.160 %, `arxiv_math` -0.094 %|

Against `a4f94c8`: odl -1.223 % (776.62 M), heavy10 -0.352 %, syn6 -3.358 %, SafeDocs -0.963 %,
`multi_column` -1.446 %, `tables` -0.997 %, `arxiv_math` -0.620 %; against `54884b5` odl
-1.164 %, heavy10 -0.328 %, syn6 -3.203 %, SafeDocs -0.896 %, `multi_column` -1.420 %, `tables`
-0.901 %, `arxiv_math` -0.619 %. No times were taken.

Tried and rejected (2026-11-17, `instr.sh` against the step before; the bar is 0.05 % of odl or
0.1 % of a group):

- `MoveAddresses` reading only a first line's first 8 bytes for "abstract" and finding the author
  line in capitals from the glyphs' texts before making any block's text: odl -0.022 %,
  `multi_column` -0.047 %, `arxiv_math` -0.028 %.
- The edge-row model's guards (a row that ends a sentence or reads as a caption) tested before
  its other features when no feature sink listens: odl -0.016 %, `multi_column` -0.010 %, syn6
  -0.019 %.
- The gutter test (`PlaceGlyphs`, 1.1 % of `multi_column`): a neighbour's segments read only until
  one holds the gap's middle: `multi_column` -0.053 %, odl -0.023 %; with the test skipped on
  pages without a line of two segments: `multi_column` -0.067 %, odl -0.035 %, syn6 -0.058 %;
  with the neighbours copied into a compact array: `multi_column` -0.080 %, odl -0.024 %; the same
  with each neighbour's test in a lambda: `multi_column` +0.279 %, odl +0.089 % (the counters left
  the registers). Stopping at the first segment past the middle needs segments whose starts never
  go back, which glyph boxes with u1 < u0 or NaN would break: not attempted.
- `TallGutter` counting mates only while undecided: `multi_column` -0.041 %; with the right
  group's baselines sorted and each line's nearest looked up: `multi_column` -0.088 %, odl
  +0.014 %.
- The fragment loop as a loop over fragments around a loop over the glyphs joining each (no
  "open" flag): odl -0.030 %; with the fragment's opening in a lambda: odl +0.668 % (the fragment
  went to memory).
- The size-run count in a helper function instead of a lambda: odl +0.010 %.
- Not attempted: the compound lexicon's glyph lookup per hyphen (`InnerHyphen`, in
  `markdown_writer.cpp`, outside this round's files).

## Twentieth round: the flanking landing won back, the writer's byte stores (2026-11-18)

`4eb7534` (emphasis delimiters flank as CommonMark reads them) raised user instructions against
`b687e48` (`instr.sh`, `--batch --workers=1`, images off; `arxiv_math` with `--math=latex`): odl
by 0.104 %, heavy10 by 0.056 %, syn6 by 0.129 %, SafeDocs by 0.092 %, `multi_column` by
0.157 %, `tables` by 0.101 %, `arxiv_math` by 0.288 %. Callgrind of both (Release with `-g`,
linked with gold) over odl, `multi_column` and `arxiv_math`: odl +0.86 M (the words loop moved
out of `Inline` into `WriteWords` +0.38 M net, `SetStyle`'s delimiter records +0.30 M,
`RepairFlanking` +0.20 M, `FlankBits` +0.04 M); `multi_column` +2.81 M (`WriteWords` +1.62 M,
`SetStyle` +0.78 M, `RepairFlanking` +0.36 M); `arxiv_math` +16.1 M, of which the formulas of
the 136 blocks written again rebuilt (`BuildLatex` +7.3 M, with `AddMathGlyph` and
`AddMathRules` about 8 M), the words loop +4.3 M, `SetStyle` +2.4 M and `RepairFlanking`
0.94 M more. The costliest code of `markdown_writer.cpp` and `math.cpp` at `452028b`, self, in
percent of the group: odl `StyleWords` 1.79 %, `Inline` (with `InlinePlain`) 1.73 %,
`CollectWords` 1.48 %, `WriteWords` 1.41 %, `SetStyle` 0.28 %; `multi_column` `Inline` 2.27 %,
`StyleWords` 1.92 %, `CollectWords` 1.51 %, `WriteWords` 1.49 %; `arxiv_math` `Builder::Row`
2.50 %, `PrepareMathGlyphs` 2.31 %, `MarkDisplayFormulas` 2.14 %, `FindInlineFormulas` 2.12 %,
`WriteWords` 1.69 %, `StyleWords` 1.52 %, `Inline` 1.48 %, `CollectWords` 1.38 %, `ResolveAtom`
1.15 %. Most `SetStyle` calls
change nothing (no style open and none wanted), yet each paid a call and the stack walk. The
writer's per-glyph loops store bytes (`out_`'s characters, `styles_`, `inline_`'s flags, a
`GlyphRef`'s script and line-start bytes), which may alias any object in C++: after each store the
compiler read the vectors' data pointers, the page's arrays and the body-face flags from memory
again. Every step is byte-identical (`bench.py identical` against the step before over the
synthetic corpus, odl, olmOCR-Bench, READoc dev and dev2 and OmniParseBench's PDF pages, 4,886
documents, with `--images=none` alone and with `--math=latex`, `--format=json`, `--plain`,
`--tables=html`, `--tables=pipe`, `--tables=pipe-latex`, `--quotes=ascii` and `--small-caps=keep`
beside it: 43,974 comparisons a step, 0 differ):

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(writer): a style change that changes nothing returns before the call`|`SetStyle` is an inline test (no style open and none wanted, or the one open style wanted) in front of the out-of-line `ChangeStyle`|odl -0.320 %, heavy10 -0.155 %, syn6 -0.412 %, SafeDocs -0.207 %, `multi_column` -0.338 %, `tables` -0.198 %, `arxiv_math` -0.318 %|
|`perf(writer): CollectWords reads a line's glyphs and script bounds from locals`|The line's glyph index and glyph arrays, the page text, the glyph count and the superscript and subscript bounds (the same float expressions as `IsSuperscript` and `IsSubscript`) read once a line; the monospace body flag once a block|odl -0.094 %, heavy10 -0.047 %, syn6 -0.110 %, SafeDocs -0.057 %, `multi_column` -0.099 %, `tables` -0.061 %, `arxiv_math` -0.091 %|
|`perf(writer): FindFormulas reads a page's arrays once, and passes over tagged formulas only on pages that have them`|The glyph and math-font arrays read again only when the page changes; the tagged-formula pass, a lookup a glyph, skipped when no page of the block has tagged formulas|`arxiv_math` -0.429 %, the rest within 0.002 %|
|`perf(writer): WriteWords reads the block's arrays from locals`|`refs_`, `styles_`, `mathRun_` and `words_`, which nothing changes while a block is written|odl -0.058 %, heavy10 -0.029 %, syn6 -0.062 %, SafeDocs -0.036 %, `multi_column` -0.061 %, `tables` -0.039 %, `arxiv_math` -0.053 %|
|`perf(writer): StyleWords reads the block's arrays and body faces from locals`|The same for its passes over the glyphs and words, with `before_` once it is sized|odl -0.088 %, heavy10 -0.044 %, syn6 -0.179 %, SafeDocs -0.063 %, `multi_column` -0.083 %, `tables` -0.037 %, `arxiv_math` -0.108 %|
|`perf(writer): InlinePlain reads a line's glyphs and script bounds from locals`|As in `CollectWords`, for the loop that writes a block without styles|odl -0.078 %, heavy10 -0.010 %, syn6 -0.165 %, SafeDocs -0.036 %, `multi_column` -0.107 %, `tables` -0.046 %, `arxiv_math` -0.013 %|

Against `452028b`: odl -0.638 % (772.48 M), heavy10 -0.284 %, syn6 -0.922 %, SafeDocs -0.397 %,
`multi_column` -0.684 %, `tables` -0.379 %, `arxiv_math` -1.016 %; against `b687e48` (before the
flanking landing) odl -0.535 %, heavy10 -0.228 %, syn6 -0.794 %, SafeDocs -0.305 %,
`multi_column` -0.527 %, `tables` -0.279 %, `arxiv_math` -0.731 %. No times were taken.

Tried and rejected (2026-11-18, `instr.sh` against the step before; the bar is 0.05 % of odl or
0.1 % of a group, 0.2 % of `arxiv_math`):

- A block written again by `RepairFlanking` taking its formulas from its first writing (each
  formula's place in `out_` recorded, the text copied once a block is written again) instead of
  rebuilding them: `arxiv_math` -0.123 %, the rest within 0.012 %. With the bookkeeping in
  `Inline`'s loop, `WriteFormula` inlined: odl +0.113 %, syn6 +0.202 %, `arxiv_math` -0.120 %;
  `WriteFormula` out of line: odl +0.076 %, `arxiv_math` -0.170 %. After the first step the whole
  check and its rewrites cost odl 0.037 %, `multi_column` 0.069 % and `arxiv_math` 0.083 %
  (measured with the check off), under every bar.
- `CollectWords`' script bounds alone, without the arrays in locals: odl -0.044 %,
  `multi_column` -0.047 %, `arxiv_math` -0.040 %.
- `InlinePlain` outside LaTeX mode testing a glyph's faces as it writes it, without the pass over
  the block before (the text written cut off again when one bails): odl +0.039 %, syn6 +0.306 %,
  `tables` -0.049 %.
- `WriteWords`' run ending at a formula's first glyph in its own loop, without the scan for the
  next formula in LaTeX mode: `arxiv_math` -0.102 %, odl -0.031 %; the run's end found while its
  text is written: odl +0.037 %, `multi_column` +0.049 %, `arxiv_math` -0.042 %.
- Not attempted: the compound lexicon's glyph lookup per hyphen (`InnerHyphen`, 0.78 M on
  `multi_column`, under its bar). A search that walks on from the last hyphen's glyph gives
  `lower_bound`'s glyph only when glyph text offsets never go down, which ActualText spans,
  replaced OCR texts and rebuilt accented letters do not ensure.

## Twenty-first round: the parsing side, ToUnicode maps and composite fonts (2026-11-23)

Callgrind of `c59243c` (Release with `-g`, linked with gold) over odl, heavy10, safe40 and
`multi_column`, self cost of the parsing side in percent of the group: the content lexer (inlined
in `RunStream`) odl 12.4 %, heavy10 31.4 %, safe40 15.0 %, `multi_column` 5.3 %;
`DecodeCodesBmi2` (inflate) 12.2 %, 28.2 %, 12.6 %, 5.8 %; `ShowText` (with its SSE2 box) odl
8.3 %, heavy10 2.5 %, safe40 5.8 %, `multi_column` 10.0 %; `MeasurePage` 1.4 %, 0.5 %, 0.9 %,
1.6 %; `ParseArray` and `ParseDict` odl 2.1 %, safe40 1.6 %; the lexer's `ReadArray` odl 0.9 %,
`multi_column` 1.1 %; `ParseToUnicode` odl 0.9 %; on safe40 and `multi_column` the ToUnicode maps
of one-unit bfranges (identity maps of up to 65,536 codes): `LoadToUnicode` with its repair pass
2.4 % and 1.9 %, `UnicodeSink::Range` 1.1 % and 0.8 %, `ToUnicodeMap::Index` 0.3 % and 0.2 %;
inclusive, `FontCache::Get` 11.0 % of odl. The lexer's numbers are short (about 1.5 digits after
the point on heavy10, 3.2 on odl) and inflate is libdeflate's design: neither gave a step. Every
step is byte-identical (`bench.py identical`, see below); instruction counts only, no times:

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(cmap): a ToUnicode bfchar section's announced count reserves its entries and text`|The count before `beginbfchar` (at most a sixth of the bytes left) grows the entries and text once, at least twofold|odl -0.093 %, `tables` -0.052 %, `multi_column` -0.047 %, `arxiv_math` +0.057 %|
|`perf(font): a ToUnicode run's code points counted and voted a segment at a time`|Within the bounds of the classes `CountCodePointEntry` tests (`SegmentEnd`) a run's code points are counted once, their flags set with `memset`, the majority vote over distinct code points in closed form (`Vote::AddDistinct`); the surrogates one by one|SafeDocs -1.737 %, `multi_column` -1.206 %, `tables` -1.113 %, odl -0.015 %|
|`perf(font): RepairToUnicode's later passes find the entries to repair and the unusable ones eight flags a load`|`NextFlag`: a 64-bit load tests eight flags|SafeDocs -0.347 %, `multi_column` -0.235 %, `tables` -0.219 %|
|`perf(cmap): a one-unit bfrange's UTF-8 written a block of 64 units at a time`|Lead and middle bytes once a block, the last counting up; U+FFFD as its bytes; entries with a running offset|SafeDocs -0.453 %, `multi_column` -0.314 %, `tables` -0.283 %|
|`perf(cmap): a ToUnicode map's dense index filled 64 consecutive codes at a time`|Sorted entries, one per code: 64 whose codes span 63 fill a row of counting slots|SafeDocs -0.186 %, `multi_column` -0.127 %, `tables` -0.116 %|
|`perf(font): a composite font's code decoded inline in NextSlow, an Identity CMap's CID without a call`|`NextComposite` and `Composite` inlined into their one caller; an Identity CMap's CID is the code|syn6 -1.053 %, `multi_column` -0.439 %, SafeDocs -0.345 %, `tables` -0.248 %, odl -0.127 %, heavy10 -0.107 %|
|`perf(syntax): hexadecimal strings decoded sixteen digits at a time, then eight, with SSE2`|`HexPairs`: digits and A-F in either case lane by lane, pairs packed into bytes; any other byte left to the scalar loops|syn6 -0.120 %, `multi_column` -0.102 %, odl -0.068 %|
|`perf(lexer): an array's literal strings that end at their first look-at byte read in ReadArray`|A TJ string whose first `\`, `(`, `)` or CR is its closing parenthesis becomes its item in place|`arxiv_math` -0.561 %, `tables` -0.526 %, `multi_column` -0.505 %, odl -0.374 %, SafeDocs -0.373 %, heavy10 -0.096 %|

Against `c59243c`: odl -0.668 % (767.39 M), heavy10 -0.213 %, syn6 -1.188 %, SafeDocs -3.440 %,
`multi_column` -2.938 %, `tables` -2.524 %, `arxiv_math` -0.497 %. The round's last binary against
`c59243c`'s (`bench.py identical` over the synthetic corpus, odl, olmOCR-Bench, READoc and the
SafeDocs sample the folders hold, 1,857 documents, with `--images=none` alone and with
`--math=latex`, `--format=json`, `--plain`, `--tables=html` and `--tables=pipe-latex` beside it):
11,142 comparisons, 0 differ. Tried and rejected (2026-11-23, `instr.sh` against the step before;
the bar is 0.05 % of odl or 0.1 % of a group):

- The lexer's numbers read 8 bytes at a time (SWAR digit mask, the point removed, fast_float's
  eight-digit product): odl +3.99 %, heavy10 +11.6 % (the numbers are too short).
- `LiteralRunEnd` with SSE2, 16 bytes a step: odl +0.030 %, `tables` +0.139 % (short strings).
- `ToUnicodeMap::Find`'s dense case inline in `cmap.h`: `multi_column` -0.070 %, odl -0.015 %.
- Tj operands' literal strings read in `ReadValue` as in `ReadArray`: syn6 +0.666 %, odl
  +0.064 %; with the first run's end handed to the escape reader (`ReadLiteralRest`) and one
  shared reader for arrays and operands: odl +0.238 %, syn6 +0.681 % (`ReadText` no longer
  inlined); with `ReadText` forced inline: odl +0.124 %.
- The glyph loop's printable ASCII held in a cursor buffer and appended per text operator
  instead of `push_back` (6.3 instructions a glyph): odl +0.938 % with the buffer in the cursor
  (its address taken by `append`), +0.571 % with it outside.
- A lean `Font::NextSlow` case for two-byte Identity codes with a dense ToUnicode entry: odl
  -0.083 %, `multi_column` -0.450 %, but syn6 +0.545 % (its maps have no dense index); with the
  other cases inline after it: syn6 +0.331 %.

## Twenty-second round: the inset landing won back, Breuel's order, path pieces (2026-11-24)

`feb9e88` (insets set into a gutter read after their column; columns found without them, a capped
number of line comparisons) raised user instructions against `e4bf63d` (`instr.sh`, `--batch
--workers=1`, images off; `arxiv_math` with `--math=latex`): odl by 0.125 %, heavy10 by 0.021 %,
syn6 by 0.090 %, SafeDocs by 0.135 %, `multi_column` by 0.306 %, `tables` by 0.140 %,
`arxiv_math` by 0.086 %. Callgrind of both (Release with `-g`, linked with gold) over odl,
`multi_column` and safe40: `InsetColumns` (inlined in `Cut`) odl about 0.9 M, of which `Wrapped`'s
pass over a region's lines for each candidate of running text 0.55 M and `InsetCandidate`'s passes
0.22 M; `multi_column` 4.7 M (`Wrapped` about 3.5 M); safe40 1.8 M; the median size `Order` takes
for its bridging test about 0.1 M of odl. `Cut` grew enough that GCC left Breuel's `Topological`
out of line (20.5 M self on `multi_column`, 1.24 %) and took `SplitXSoft` in. The costliest code of
this round's files, self, in percent of odl, `multi_column` and safe40: the content lexer (inlined in
`RunStream`) 12.5, 5.5, 15.7; `ShowText` 6.8, 8.2, 5.0 (its SSE2 box 1.6, 1.9, 1.0 more); the
interpreter's operator loop 1.8, 1.0, 2.5; `MeasurePage` 1.4, 1.7, 1.0; the writer's `StyleWords`,
`CollectWords`, `Inline` and `WriteWords` 4.6, 5.2, 3.1 together; `AddPiece` 0.37, 0.25, 1.39
(304,213 calls on safe40, about 85 instructions each, a fifth of them the call); `PlanPage` 0.30 %
of odl, most of it its scan of the glyphs under each large image. Every step is byte-identical
(`bench.py identical` against the step before, see below); instruction counts only, no times:

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(order): Wrapped tests a line's vertical overlap with the inset first`|The overlap with the candidate's height and the side tests, on its box held in locals, before the line's identity, count, flags, size and width|`multi_column` -0.150 %, `tables` -0.055 %, SafeDocs -0.051 %, odl -0.050 %, `arxiv_math` -0.025 %, syn6 -0.015 %, heavy10 -0.004 %|
|`perf(order): Topological tests each pair of lines once for both directions, and finds the next free line from the first that can be`|One test of horizontal overlap and tolerance gives both directions of a pair; the scan of the lines between a line and one wholly right of it, the same for both, runs once, starting with the two comparisons a line of either column fails; the topmost free line searched from the first that can be free|`multi_column` -0.206 %, `arxiv_math` -0.063 %, `tables` -0.041 %, SafeDocs -0.015 %, odl -0.012 %, syn6 -0.006 %, heavy10 -0.004 %|
|`perf(ocr): PlanPage tests a glyph's centre against the image before the glyph's flags`|Most glyphs lie outside a large image: the centre test, on the image's box in locals, before `Counted`|`multi_column` -0.061 %, odl -0.056 %, SafeDocs -0.030 %, `tables` -0.026 %, syn6 -0.025 %, `arxiv_math` -0.012 %, heavy10 +0.000 %|
|`perf(interpreter): AddPiece inlined into the path operators`|`l`, `h` and `re` without the call|SafeDocs -0.260 %, heavy10 -0.205 %, `tables` -0.161 %, `arxiv_math` -0.066 %, odl -0.054 %, `multi_column` -0.051 %, syn6 +0.005 %|
|`perf(interpreter): a diagonal path piece is left before its ends' finiteness tests`|A piece more than a hundredth of a point long both ways adds nothing, finite or not: that test first|SafeDocs -0.200 %, heavy10 -0.129 %, `tables` -0.129 %, `multi_column` -0.040 %, odl -0.038 %, `arxiv_math` -0.038 %, syn6 -0.002 %|
|`perf(interpreter): Intersect inlined`|The clip and visibility boxes of paths, rulings, images and glyph runs without the call|heavy10 -0.182 %, SafeDocs -0.149 %, `tables` -0.125 %, odl -0.071 %, `arxiv_math` -0.044 %, `multi_column` -0.037 %, syn6 -0.010 %|
|`perf(ocr): MeasurePage counts a glyph with one addition and finds an ASCII letter before testing the byte's range`|Counted and invisible glyphs in the two halves of one 64-bit tally (the invisible flag shifted up to bit 32); the letter table indexed by any byte (0 from 0x80 on), so a one-byte glyph's letter is found before the ASCII range test|syn6 -0.171 %, `multi_column` -0.136 %, odl -0.113 %, `tables` -0.087 %, SafeDocs -0.067 %, `arxiv_math` -0.053 %, heavy10 -0.033 %|
|`perf(order): Topological finds the lines between two lines side by side through the overlap bitsets alone`|Ranges of up to 32 lines were scanned line by line, longer ones through the lines' overlap bitsets (a row per line, built when first needed); the bitsets now serve every range with lines in it|`multi_column` -0.291 %, `arxiv_math` -0.080 %, `tables` -0.077 %, syn6 -0.007 %, heavy10 +0.003 %, odl +0.008 %, SafeDocs +0.020 %|

Against `feb9e88`: odl -0.385 % (765.40 M), heavy10 -0.553 %, syn6 -0.239 %, SafeDocs -0.752 %,
`multi_column` -0.968 %, `tables` -0.699 %, `arxiv_math` -0.380 %; against `e4bf63d` (before the
inset landing) odl -0.261 %, heavy10 -0.532 %, syn6 -0.150 %, SafeDocs -0.618 %,
`multi_column` -0.665 %, `tables` -0.560 %, `arxiv_math` -0.294 %. Each step's binary against the
step before's (`bench.py identical` over the synthetic corpus, odl, olmOCR-Bench, READoc dev and
dev2 and 1,000 SafeDocs documents (`0000.zip`), 3,737 documents, with `--images=none` alone and
with `--math=latex`, `--format=json`, `--plain`, `--tables=html`, `--tables=pipe-latex`,
`--quotes=ascii` and `--small-caps=keep` beside it): 29,896 comparisons a step, 0 differ. Tried
and rejected (2026-11-24, `instr.sh` against the step before; the bar is 0.05 % of odl or 0.1 % of
a group):

- `Order` handing the median size it takes to `Cut` (which took it again for the same lines): odl
  -0.014 %, `multi_column` -0.012 %.
- `Topological`'s side-by-side test over the lines six ems wide only, copied to an array first:
  against the kept step odl +0.013 %, syn6 +0.038 %, `multi_column` +0.043 % (a pair is usually
  found early, and the copy costs more than the pairs it saves).
- `Topological`'s ranges scanned line by line up to 8 or 16 lines instead of none: `multi_column`
  -0.246 % and -0.194 % where none gives -0.291 % (odl +0.016 %, +0.002 %, +0.008 %); after it, the
  overlap rows built a word at a time (`multi_column` -0.011 %) and the edges kept as bit rows, the
  in-degrees lowered along their set bits (`multi_column` -0.021 %, odl -0.003 %).
- `TallGutter` counting mates only until the answer is settled (half the left group mated, or too
  few lines left to reach half): `multi_column` -0.042 %, odl -0.006 %; with `CrossLines` scanning
  the wide lines only, kept in an array of their own: odl +0.058 %, syn6 +0.036 %,
  `multi_column` -0.011 %.
- `MeasurePage`'s one-addition tally without the letter found first: odl -0.040 %, syn6 -0.061 %,
  `multi_column` -0.049 % (kept with it).
- The content lexer's fraction digits read two a step: odl +0.392 %, heavy10 +0.960 %.
- `Multiply` (matrices) inlined: heavy10 -0.063 %, odl -0.006 %, `multi_column` +0.010 %.

## Twenty-third round: the accents' glyph list, the table detector's rows and cells, font classes (2026-11-24)

From the twentieth round's base `6698987` to master `e8d8371` the LaTeX path and the table detector
grew: `c59243c` (row neighbours in `--math=latex`) raised `arxiv_math` by 0.07 %, the landings up to
`a64be6a` (stray accents seated over their glyph, a spacing accent no word break in `FactsOf`,
header labels centred over narrow columns) by 0.22 %, and `tables` by 0.13 %. Callgrind of both
(Release with `-g`, linked with gold; `--batch --workers=1`, images off; `arxiv_math` with
`--math=latex`) over `arxiv_math`, `tables` and `multi_column`: `math.cpp` self +6.2 M on
`arxiv_math` (`MarkDisplayFormulas` with `FactsOf` +2.6 M, the spacing-accent test's `AccentOf`
+0.3 M more; `Builder::Row`, both depths, +2.5 M: the raised first glyph and the other operators'
limits; `Builder::Expr` +0.35 M, `ResolveAtom` +0.2 M); `tables.cpp` +0.76 M on `tables` and
+0.32 M on `multi_column`, nearly all `TextGrid`'s centred-label tests (now out of line). The
costliest code of the two files at `e8d8371`, self, in percent of the group under callgrind:
`arxiv_math` (5,274 M) `FindInlineFormulas` 1.63 %, `PrepareMathGlyphs` 1.52 % (its main pass
about 33 M, the list of the glyphs an accent may sit on 17.5 M, that list's unsorted walks 21.3 M),
`MarkDisplayFormulas` 1.44 % (`FactsOf` 53.7 M of its 76.1 M), `Builder::Row` 1.79 % with both
depths (no line above 4 M), `ResolveAtom` 0.97 %; `tables` (1,020 M) `Detector::Commit` 1.42 %
(the glyph-to-cell loop with `Locator::Find` about 9 M), `Run` 0.99 % (`CollectWords` and
`SortWords` inlined), `BuildRows` 0.93 %, `Chars` 0.44 %, `TextGrid` 0.29 %, `SplitHeaderLevels`
0.03 %; `multi_column` (1,638 M) `BuildRows` 1.14 %, `Run` 1.05 % (`SortWords`' insertion sort
10.1 M, `CollectWords` 6.4 M), `AddInk` 0.50 %, `Borderless` 0.46 % (3.7 M its split test);
odl (773 M) `BuildRows` 0.81 %, `Run` 0.75 %, `Commit` 0.38 %, `Borderless` 0.32 %, and
`MathFontByte` 0.13 % inclusive (every font of every page classed by name in every mode). Every
step is byte-identical (`bench.py identical` against the step before over the synthetic corpus,
odl, olmOCR-Bench with `arxiv_math`, READoc dev and dev2, OmniParseBench's PDF pages and 1,000
SafeDocs documents (`0000.zip`), 5,886 documents, with `--images=none` alone and with
`--math=latex`, `--format=json`, `--plain`, `--tables=html`, `--tables=pipe-latex` and
`--math=latex --format=json` beside it: 41,202 comparisons a step, 0 differ); instruction counts
only, no times:

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(math): PrepareMathGlyphs walks the accents' glyph list four reaches a test, sorted after 64 accents`|The glyphs an accent may sit on listed as three arrays (the left and right ends of their reach, their glyph) written by index; without the sorted list an accent's walk tests four reaches at a time with SSE2 and calls the distance test only for those that hold its centre; the list sorted after 64 accents instead of 16|`arxiv_math` -0.365 %, the rest within 0.002 %|
|`perf(tables): Locator keeps the bounds of the cell it found, and tests a point against them alone`|The x range of the last column and the y range of the last row found (NaN where the boundaries are out of order): a point inside them by two comparisons, instead of the order flag, the index, two boundary loads and the last-boundary test|`tables` -0.174 %, SafeDocs -0.103 %, heavy10 -0.047 %, `multi_column` -0.046 %, syn6 -0.038 %, odl -0.035 %, `arxiv_math` -0.005 %|
|`perf(tables): Commit fills glyphs outside cells only when a line has them, and appends a cell line's glyphs through pointers`|A line whose glyphs all fall in a cell skips the two neighbour-fill passes; a piece's glyphs appended through pointers into arrays grown once, its box and faces in locals|`tables` -0.158 %, SafeDocs -0.055 %, odl -0.054 %, heavy10 -0.037 %, syn6 -0.029 %, `multi_column` -0.013 %, `arxiv_math` -0.001 %|
|`perf(tables): BuildRows builds the open row in a local and reads the words through pointers`|The row a word joins was `rows.back()`, loaded and stored for each word: a local, stored when the next row opens|`multi_column` -0.221 %, `tables` -0.183 %, odl -0.151 %, SafeDocs -0.117 %, syn6 -0.104 %, `arxiv_math` -0.095 %, heavy10 -0.064 %|
|`perf(tables): Borderless tests each row against its widest gap, found once for both passes`|A row splits at a column gap when its widest gap between a word and the ink before it is that wide (NaN gaps passed over); the widest gap and the code flag found when the page's rows are built, so the second pass compares one value a row|syn6 -0.122 %, odl -0.083 %, `multi_column` -0.067 %, `arxiv_math` -0.051 %, SafeDocs -0.036 %, `tables` -0.032 %, heavy10 -0.021 %|
|`perf(math): MathFontOf lowercases a font's name once and finds its parts in one pass`|The name lowercased into a buffer; a name whose first byte starts none of the 43 prefixes skips them, a prefix with another first byte is passed over before it is compared; "math", "mathitalic" and "doublestruck" found in one scan|syn6 -0.071 %, odl -0.060 %, SafeDocs -0.059 %, `multi_column` -0.054 %, `tables` -0.051 %, heavy10 -0.023 %, `arxiv_math` -0.017 %|
|`perf(tables): AddInk merges a row's words into the ink through pointers`|The merged spans written through a pointer into room for both lists, the old spans and the row's words read through pointers taken once, the proper flag in a local (each `push_back` stored the vector's end, which may alias the old list's bounds)|`multi_column` -0.134 %, odl -0.062 %, `tables` -0.048 %, `arxiv_math` -0.030 %, SafeDocs -0.024 %, heavy10 -0.016 %, syn6 -0.001 %|

Against `e8d8371`: odl -0.445 % (762.43 M), heavy10 -0.206 %, syn6 -0.366 %, SafeDocs -0.391 %,
`multi_column` -0.535 %, `tables` -0.643 %, `arxiv_math` -0.559 % (5,190.02 M); against
`6698987` `arxiv_math` -1.043 % and `tables` -3.542 % (with the twenty-first and twenty-second
rounds). Callgrind of the round's `PrepareMathGlyphs` on `arxiv_math`: 64.2 M, the list 21.9 M
(three arrays), its walks 5.6 M. Tried and rejected (2026-11-24, `instr.sh` against the step before;
the bar is 0.05 % of odl or 0.1 % of a group, 0.2 % of `arxiv_math`):

- The accents' reaches pushed back into three vectors instead of written by index: `arxiv_math`
  +0.619 % (`emplace_back` left out of line, 39 M). The sorted list's threshold with the SSE2 walk:
  after 8 accents +0.203 %, 32 -0.058 %, 64 -0.103 % (kept), 128 -0.112 % against 16.
- The reaches and the accents listed in `PrepareMathGlyphs`' main pass, without a pass of their
  own (most of the group's glyphs are on pages with an accent): `arxiv_math` +0.200 %.
- `SortWords` as a merge of the runs each strictly in order, two by two, instead of the bounded
  insertion sort: `multi_column` +0.071 %, `tables` +0.025 %, odl -0.019 % (many short runs, few
  moves).
- `BuildRows` joining a word on the open row's baseline without `Fmax` when a size is above 0:
  odl +0.011 %, `multi_column` +0.026 %; each word's centre computed once a page into an array:
  odl -0.012 %, `multi_column` -0.028 %, `tables` -0.021 %.
- `Chars` counting a word's bytes in locals, stored once: `tables` +0.157 %, odl +0.041 %.
- Not attempted: `FactsOf`'s counts in locals and `FindInlineFormulas`' per-word flags from its
  first pass (both rejected in the seventeenth round); `Builder::Row` (no line above 4 M);
  `MarkDisplayFormulas`' growth rounds over every line (a line's join depends on the box grown
  before it in the same round, so a line cannot be passed over without that box).

## Twenty-fourth round: the drop-cap landing won back, the fragment loop's registers (2026-11-24)

`f367f1b` (drop caps: `SeatDropCaps`' pass over a page's fragments, `InitialAlone` out of line in
`PlaceGlyphs`' script test, `FinishLine`'s drop-cap flag) raised user instructions against `0bbf680`
(`instr.sh`, `--batch --workers=1`, images off; `arxiv_math` with `--math=latex`): odl +0.099 %,
heavy10 +0.018 %, syn6 +0.096 %, SafeDocs +0.045 %, `multi_column` +0.089 %, `tables` +0.108 %,
`arxiv_math` +0.035 %. Callgrind of both (Release with `-g`, linked with gold; `--batch
--workers=1`, images off) over odl, syn6 and `multi_column`: on odl `PlaceGlyphs` self +269 K (its
glyph loop one instruction more a glyph, 355,693 glyphs: the content order and the glyph array
read from the stack at each glyph, the space flag kept there, since the call to `InitialAlone`
had to find registers for its arguments), `FinishLine` +98 K (the flag's test, about 12
instructions a line), `SeatDropCaps` +83 K (its pass, about 8 instructions a fragment),
`InitialAlone` itself 66; on `multi_column` +651 K, +270 K and +288 K. The costliest code of
`lines.cpp` and the writer at `f367f1b`, self, in percent of the group under callgrind: odl
(769.8 M) `BuildLines` 6.67 % (with `CollectAndFold`'s duplicate filter inlined, about 30 M of
it), `PlaceGlyphs` 6.30 % (its fragment loop about 25 M, the line builder's word loop about 8 M),
`StyleWords` 1.35 %, `CollectWords` 1.14 %, `Inline` 1.12 %, `WriteWords` 1.00 %, `Folder::Place`
0.96 %, `AppendLineText` 0.22 %; `multi_column` (1,632.2 M) `PlaceGlyphs` 8.40 %, `BuildLines`
8.29 %, `Inline` 1.49 %, `StyleWords` 1.47 %, `CollectWords` 1.22 %, `Folder::Place` 1.16 %,
`WriteWords` 1.08 %. The fragment loop and the duplicate filter were short of registers: the
fragment's fields (box, baseline, size, last edge and size, flags) and six finiteness sums each
held a float register, so values went to the stack and back at each glyph. Every step is
byte-identical (`bench.py identical` against the step before over the synthetic corpus, odl,
olmOCR-Bench (every category), READoc dev and dev2 and 1,000 SafeDocs documents (`0000.zip`),
3,737 documents, with `--images=none` alone and with `--math=latex`, `--format=json`, `--plain`,
`--tables=html`, `--tables=pipe-latex`, `--quotes=ascii` and `--small-caps=keep` beside it: 29,896
comparisons a step, 0 differ); instruction counts only, no times:

|Step|Change|Effect (`instr.sh`, against the step before)|
|---|---|---|
|`perf(lines): PlaceGlyphs starts a fragment and extends it in a loop of its own, without an open flag tested at each glyph`|A fragment starts at a glyph and an inner loop takes the glyphs after it while they join; the first that does not closes it and starts the next|syn6 -0.211 %, `multi_column` -0.151 %, odl -0.131 %, `tables` -0.092 %, SafeDocs -0.073 %, `arxiv_math` -0.055 %, heavy10 -0.031 %|
|`perf(lines): PlaceGlyphs keeps the fragment's box in one SSE2 register`|The box as (-u0 u1 -vb vt): one load, one sign flip and one `_mm_max_ps` a glyph; -(a < b ? a : b) is -a > -b ? -a : -b, so the bits are FiniteMinMax's, signed zeros included (the checked path and targets without SSE2 keep four floats)|syn6 -0.611 %, `multi_column` -0.470 %, odl -0.383 %, `tables` -0.326 %, SafeDocs -0.250 %, `arxiv_math` -0.203 %, heavy10 -0.112 %|
|`perf(lines): CollectAndFold adds the finiteness sums in two SSE2 registers`|u0 u1 vb vt added in one register, v and size in the low lanes of another, lane by lane in the same order, summed at the end as before|syn6 -0.298 %, `multi_column` -0.232 %, odl -0.192 %, `tables` -0.165 %, SafeDocs -0.127 %, `arxiv_math` -0.083 %, heavy10 -0.047 %|
|`perf(lines): PlaceGlyphs keeps a fragment's reach in em as a float, its marker flag set as it closes`|The join gap's factor (kMarkerGap after a list marker, else kFragmentGap) a float set as the fragment starts and when a glyph that is no space joins; the marker flag is whether the reach is still kMarkerGap|syn6 -0.147 %, `multi_column` -0.110 %, odl -0.090 %, `tables` -0.074 %, SafeDocs -0.057 %, `arxiv_math` -0.044 %, heavy10 -0.025 %|
|`perf(lines): a fragment starting with a one-byte glyph is a list marker only as - or *, without ParseMarker`|Of a one-byte text ParseMarker finds only the bullets "-" and "*" (an enumerator has two bytes at least): tested in place|`tables` -0.074 %, `multi_column` -0.071 %, syn6 -0.062 %, odl -0.054 %, SafeDocs -0.047 %, `arxiv_math` -0.028 %, heavy10 -0.015 %|

Against `f367f1b`: odl -0.847 % (756.71 M), heavy10 -0.230 %, syn6 -1.322 %, SafeDocs -0.553 %,
`multi_column` -1.029 %, `tables` -0.730 %, `arxiv_math` -0.413 % (5,170.30 M); against `0bbf680`
(before the drop caps) odl -0.750 %, syn6 -1.227 %, `multi_column` -0.941 %, `tables` -0.622 %.
Tried and rejected (2026-11-24, user instructions as above against the step before; the bar is
0.05 % of odl or 0.1 % of a group):

- `FinishLine`'s line flags in a local, the line's glyphs through pointers: odl +0.015 %, `tables`
  +0.021 %; with bold, italic, monospace and artifact taken over by shifts +0.009 % and +0.014 %,
  and out of line +0.007 % and +0.015 %.
- `__builtin_expect` on the far-smaller test before `InitialAlone` (before the first step): odl
  -0.003 %, `tables` +0.005 %.
- The proto's box in one SSE2 register in the line builder's word loop, as the fragment's:
  odl +0.012 %, `tables` +0.018 %.
- `SeatDropCaps` only on pages with a fragment that may be a drop cap (found in the pass that lists
  the fragments): odl -0.001 %, `multi_column` -0.003 %.
- `InlinePlain` testing a glyph's faces as it writes it, without the pass before (abandoned when
  one is set in a style): `tables` -0.070 %, `multi_column` -0.035 %, but syn6 +0.262 %,
  `arxiv_math` +0.066 %, odl +0.015 % (styled blocks written, then taken back).
- Fragments starting with spaces in a loop of their own, so that the main loop has no test of
  whether the fragment has a glyph that is no space; the join test a lambda called from both
  loops: odl +0.345 %, syn6 +0.555 % (the fragment left the registers).
- The size above which a glyph resets the fragment's baseline (its size × 1.15) kept per fragment
  instead of multiplied at each glyph: odl -0.038 %, syn6 -0.059 %, `multi_column` -0.045 %.
- `StyleWords`' bold, italic and struck styles from the glyph flags by masks and a shift: odl
  -0.012 %, `multi_column` -0.012 %.
- Not attempted: the duplicate filter's four window offsets and cells in one register (building the
  vector and taking the four integers out costs as much as the four scalar additions and
  truncations).

## 0.5.0 (2026-11-24)

Fourteen rounds since 0.4.0 (eleventh to twenty-fourth, above), each winning back the cost of the
quality steps landed before it. `src/hot.ld` was regenerated over the release's code (`a95ad7f`;
860 sections, 815 before; output byte-identical, G-bytes 0 of 997 differ): CPU 3, 2026-11-24
17:58-18:00 UTC, against the same code with the old list, warm odl 2,210 → 2,214 pages/s, fresh
0.920 → 0.915 ms, page faults median 157 → 152.

### Result (0.4.0 against 0.5.0)

User instructions, as above, CPU 11: odl −4.07 %, heavy10 −1.87 %, syn6 −7.85 %, SafeDocs
−10.01 %, `multi_column` −7.20 %, `tables` −5.52 %, `arxiv_math` −7.88 %. Times on CPU 3, the 0.4.0
release binary and 0.5.0's interleaved, 2026-11-24 18:00-18:07 UTC, nothing of this project
running: warm odl (three runs) 2,187-2,195 → 2,204-2,209 pages/s (+0.6 %), p95 0.701-0.716 →
0.706-0.707 ms a page; fresh 0.911 → 0.912 ms (12 rotated rounds, +0.1 % paired), page faults
median 149 → 152; `--batch --workers=1 --images=none` over odl, CPU time (the child's rusage)
100.0 → 99.3 ms; a JPEG 2000 page with its image written (40 documents, 100 pages) 108.7 → 108.7
ms median; 27 documents of 4 to 39 pages on four cores 140.5 ms (0.4.0, default threads) → 132.9
ms (`--workers=4`). The instructions fell four to ten times as far as the times: a short page's
fresh time is mostly the process and the file, and the warm time the memory the pages touch.

## Twenty-fifth round: the fresh process (2026-11-25)

Base 1.0.0 (`9b71ea2`). After twenty-four rounds of instruction work a fresh one-page conversion is
about half process start (0.24 ms) and page faults (0.25 ms), so this round measured what a
process does once and which memory it touches, besides the heavy pages. `tools/bench.py fresh
--per-doc` keeps each document's figures. A callgrind script outside the repository gives each
function's one-time cost o = I(2) − 2 × (I(3) − I(2)), I(n) being the instructions of `--bench DOC
TMP n`, and the program totals of fresh `--convert` processes; page faults were charged to their
RayoPDF caller with `perf record -e page-faults --call-graph dwarf` on a RelWithDebInfo build of
the same tree.

|Finding (1.0.0's code)|Number|
|---|---|
|odl fresh, `--images=none`, 12 rotated rounds, CPU 3|median 0.915 ms, p95 1.46, mean 1.04; page faults median 152|
|odl 141 (a vector page, 4.17 MB of content) fresh|18.1 ms and 2,030 faults: 0.090 ms of the 1.04 ms mean|
|fresh `--convert` instructions, 21 odl documents (fresh-time ranks 0, 10, ..., 190 and 141)|199.7 M summed|
|one-time cost in those documents|the base-encoding texts (`MakeEncodingTexts`) 147 K in 10 of 21; no other first-use table above 20 K in 4 or more|
|one-time cost in 8 CJK documents (olmOCR-Bench and OmniParseBench development pages)|`BmpText`'s table 6.48 M in 6 of 8|
|anonymous faults by caller, odl 183, 192 and fresh ranks 100 and 150|592 of 790: `BuildLines` 123 (its duplicate filter, calloc'd and so cleared on the heap's 8 MiB top pad), `ShowText` 56 (the page's glyph records and text), `PdfArena` 42, the table detector 42|
|heavy pages, self instructions|odl 141: `RunStream` 51.9 % (the content lexer's numbers), `Inflate` 39.6 %; heavy10: `RunStream` 41.1 %, `Inflate` 29.9 %, `ShowText` 3.2 %, `BuildLines` 2.0 %|

### Steps

Every step is output-identical: `bench.py identical` against the step before over 5,886 documents
(synthetic corpus, odl, olmOCR-Bench, READoc dev and dev2, OmniParseBench's pages, 1,000 SafeDocs
documents) in ten option sets (images off and written, `--math=latex`, `--format=json`,
`--format=chunks`, `--plain`, `--tables=html`, `--tables=pipe-latex`, `--quotes=ascii`,
`--small-caps=keep`): 58,860 comparisons a step, 0 differ. User instructions are `instr.sh`'s
(`--batch --workers=1`, images off, `arxiv_math` with `--math=latex`; A/A within 0.006 %) against
the step before; fresh instructions are callgrind's program totals of fresh `--convert`
processes; page faults are `bench.py fresh`'s over odl. Counts only: the machine was shared.

|Step|Change|Effect|
|---|---|---|
|`c9ad9f8` text: base-encoding texts generated with the tables|`tools/gen_text_tables.py` writes each base encoding's texts as a record holding them (no pointer, so no relocation at start)|fresh instructions, the 21 documents, −0.74 %; odl −0.041 %, heavy10 −0.102 %, syn6 −0.081 %|
|`c25eb3e` text: the BMP UTF-8 table made at compile time|`BmpText`'s 65,536 entries a `constexpr` table (192 KiB of read-only data) instead of a first-use build|fresh instructions, the 8 CJK documents, −39 %; SafeDocs −0.47 %, `tables` −0.65 %|
|`6b0a1b0` layout: a one-shot conversion frees each stage's storage for the stages after it|In `--convert`, the fonts go once the pages are read, and the line builder's, table detector's, figure finders' and order's per-thread storage once their stage is done, so the stages after them reuse those pages; the table detector's word sort and `BuildBlocks`' line facts reserved|odl page faults median 152 → 135, p95 245 → 212, mean 168 → 151; odl −0.116 %, `multi_column` −0.099 %|
|`c7adaaf` layout: body-size votes in arrays instead of hash maps|Sizes 0-255 counted in arrays, larger ones in overflow lists; the winner as the running lines' size vote|odl −0.151 %, `tables` −0.217 %, `multi_column` −0.159 %|
|`572abfc` layout: running keys interned in an open-addressing table|Keys copied once into a byte pool; a candidate holds its key's index|syn6 −0.113 %, SafeDocs −0.075 %|
|`6de3c2c` json and report numbers through `std::to_chars` instead of `snprintf`|Coordinates fixed with two decimals and the report's integers|`--format=json` over 12 documents: odl 183 −5.9 %, a Chromium page with images −5.2 %, Typst −3.5 %|
|`f6889e4` inflate: huge-page mappings for output blocks of 1 MiB or more in one-shot conversions|In `--convert` only, an inflate output of 1 MiB or more moves to a 2 MiB-aligned mapping with `MADV_HUGEPAGE`, grown by `mremap`|odl 141: page faults 2,030 → 481, peak RSS 11 → 8 MiB, CPU time −16 %; `--batch` unchanged|
|`9acff3a` perf(lexer): an operator takes the one white-space byte after it, as a number does|The byte after an operator was a whole pass of the scan loop; an inline image's data now starts from the end of the operator before it|odl 141 −1.15 %; odl −0.423 %, heavy10 −0.912 %, SafeDocs −0.537 %, `tables` −0.482 %|
|`ac7874b` perf(interpreter): the glyph box's run terms aligned, a non-finite advance an unlikely branch|`ShowText`'s per-run terms aligned to 16 bytes for aligned loads|odl −0.100 %, syn6 −0.151 %, `multi_column` −0.115 %|
|`7f90f63` perf(lines): Near tests each coordinate's bits on its own|The duplicate filter's nearness test compares each coordinate in 32 bits instead of packing two floats that are not adjacent into one 64-bit word|odl −0.127 %, syn6 −0.200 %, `multi_column` −0.160 %|

Against 1.0.0 (`9b71ea2`): user instructions odl −0.939 % (750.22 M), heavy10 −1.168 %, syn6
−0.921 %, SafeDocs −1.438 %, `multi_column` −0.863 %, `tables` −1.665 %, `arxiv_math` −0.571 %
(5,153.36 M); fresh `--convert` instructions of the 21 documents 199.68 M → 197.46 M (−1.11 %).
The heavy-page steps meet only odl 141 and heavy10 in full: heavy10 holds 29.7 K glyphs, and its
0.1 % would be 6.6 instructions a glyph of `ShowText`'s 213.

**WebAssembly.** `bench.py wasm` times the npm package's module under Node.js and compares its
output with a native binary's. The module is now built with WebAssembly SIMD (`-msimd128
-msse2`: the SSE2 kernels compile to SIMD instructions). `RAYOPDF_SSE2` (`common/compiler.h`) guards
those kernels, and the `scalar` configuration builds and tests the paths without them:
byte-identical to the static build over G-bytes' inputs in four sets, 23,544 comparisons. Over odl
both modules give the native binary's output, 200 of 200; over the other 5,686 documents the
scalar module differs from native in 36 and the SIMD module in the same 36, each byte for byte
the scalar module's output (differences between the platforms, older than SIMD, causes not
traced). CPU time on a shared core: scalar 741, SIMD 798 pages/s (+7.7 %; +5.8 % with the order
reversed).

### Tried and rejected (2026-11-25)

- **Large per-thread objects on the heap** (`DynamicTables` 26,048 B and `CutLevels` 8,232 B behind
  a pointer once `sizeof` passes 4 KiB; `.tbss` 40,304 → 6,072 B): SafeDocs +0.037 % and `tables`
  +0.023 % with a `unique_ptr`, +0.033 % and +0.021 % with an inline pointer; the main thread took
  3 to 10 more faults a document, as `new T()` clears all of `DynamicTables` where its static TLS
  was faulted only where touched. Four page threads: within the noise.
- **Huge pages for the inflate buffer as first planned:** `madvise(MADV_HUGEPAGE)` on the heap block
  once it reaches 4 MiB did nothing (odl 141's page buffer grows in steps of about 0.5 MB and
  passes 4 MiB only at its last growth: 2,030 → 2,031 faults); a 2 MiB-aligned mapping from 4 MiB
  on, −6 %. From 1 MiB on in every mode, heavy10 +0.036 %: under `--batch`'s 32 MiB heap threshold
  the block grew in place, and the move to the mapping copies 764 KB; hence one-shot conversions
  only. Releasing the buffer before its first growth: +4 % faults on 39 small odl documents (free
  and malloc touch new heap pages where realloc reused them).
- **Base-encoding texts as one shared pool with 16-bit offsets and flag bits:** two instructions
  more for each base slot read, `arxiv_math` +0.067 %; kept as records holding their texts in the
  old cache's layout.
- **The running keys' lookup inlined into the edge-row feature function:** odl +0.002 % (code
  generation: one-page documents never look a key up); out of line, as the hash map's was.
- **`BuildLines`' calloc'd filter from fresh pages:** the filter's words are hashed, so every page
  of it is written anyway; reusing freed stage storage (`6b0a1b0`) is what saved faults.
- **The content lexer's byte not held in a register across the scan's paths:** odl 141 +1.23 % (GCC
  kept the copies and spilled more). **`PlaceGlyphsGeneral` taking its placement by value:** odl
  +0.66 %, `multi_column` +0.74 %.

## Open candidates (measured, not done)

- **Text path:** line and word texts read in place when contiguous in the page text, instead of
  copied by the callers that only read them (twelfth round: only one-word lines are contiguous,
  the copies being 0.4-0.5 % of odl, `multi_column` and `tables`).
- **The OCR route's `MeasurePage`** (1.3 % of odl, 1.6 % of `multi_column` after the
  twenty-second round, about 27 instructions a glyph): counts per font slot are untried.
- **Order and the writer:** Breuel's topological order (about 14 M on `multi_column` after the
  twenty-second round, spread over the pairs' tests, the overlap rows, the side-by-side search and
  the picks); the writer's `StyleWords` (31 M on `multi_column` after the twentieth round). Round
  10's rewrites of both gained nothing (tenth round, Tried and rejected), nor did skipping the link
  test of words outside the page's links (twelfth round). `InsetColumns`' passes for its
  candidates (0.2 M of odl).
- **The LaTeX rebuild:** after the twelfth round `malloc`, `free`, `new` and `delete` are still
  1.90 % of `arxiv_math` (`Matrix`, the limits' and braces' parts, `std::string` growth of the
  items' LaTeX). After the seventeenth round (self costs in percent of `arxiv_math`):
  `Builder::Row` 2.45 % (spread over its passes: no line above 4 M), `PrepareMathGlyphs` 2.30 %
  (after the twenty-third round 64.2 M: the main pass about 33 M, the accents' list 21.9 M, its
  walks 5.6 M), `MarkDisplayFormulas` 2.13 %, `FindInlineFormulas` 2.11 %,
  `ResolveAtom` 1.12 %, `std::vector<Item>` growth 0.50 % with `ItemOf`'s copies of the atoms'
  LaTeX 9.8 M; inclusive, `BuildLatex` 7.71 %, `Builder::Expr` 5.66 %, `Builder::Layout` 4.47 %.
  `math.cpp` has had its unit budget raised (sixteenth round).
- **The table detector:** after the twenty-third round `BuildRows` (about 0.8 % of odl, 1.1 % of
  `multi_column`: each band's words walked from its top, those outside its x range passed over
  one by one), `SortWords`' insertion sort (10 M on `multi_column`), `CollectWords`' word records
  and spaces.
- **Blocks:** after the sixteenth round, `BuildBlocks`' tests spread over many loops (card titles,
  the OCR layer's headings, `Continues`, `NoTitle`); none holds 0.15 % of a group.
- **The compound lexicon:** after the eighteenth round 0.22 % of `multi_column` and 0.09 % of odl
  (`KnownCompound` inclusive); its largest part, each hyphen's glyph found by `lower_bound`
  (`InnerHyphen`, 0.78 M on `multi_column`), needs glyph text offsets that never go down before a
  cheaper search can give the same glyph.
- **Pages:** the structure tree and outline read while the pages are interpreted (about 3 % on
  tagged documents); the image round overlapped with the layout: images are encoded on the page
  threads (`9aecd77`), but in a round of their own between the image decisions and the layout, so
  one large image holds the layout up. Overlapping them needs the decisions and the files' naming
  apart and a round that runs beside the layout's, for untagged pages without inline images (a
  gain in wall time only).
- **Heavy pages:** after the twenty-fifth round the interpreter's `Shape` and `Ruling` vectors
  still grow by doubling (0.29 M instructions of copies on heavy10); reserved from the content's
  length they would copy nothing.
- **`--serve` with `--batch`'s malloc settings:** -2 % wall over 400 SafeDocs documents at the
  same peak RSS, but freed memory stays with the process (wall time only, not measured in round 10).
- **JPEG 2000:** registers for the significance and cleanup passes; a row-streaming vertical
  wavelet pass; ICT and RGB interleave per row. JPEG, JPEG 2000, CCITT and JBIG2 decoded a row or
  band at a time for bounded RSS.
- **Profile-guided builds:** +2.0 % warm and -2.0 % fresh when the profile is trained on the
  machine that builds and times the binary, but the same three stages on GitHub's runner made a
  slower binary (tenth round, Tried and rejected): the training must give the same profile on any
  machine first, and reach the image decoders (JPEG 2000 pages were 4 % slower).

## Rerunning

`tools/bench.py` runs the measurements of this document and of the version records from the
repository (`python3 tools/bench.py --help` has every option). Everything runs in Docker without
network: RayoPDF's binaries in the measuring image, the other tools in their evaluation images
(`tools/eval.py images`); binaries and documents are mounted read-only, outputs go to a RAM disk
in the container, and `RAYOPDF_OCR` names nothing. Each command prints a first line with what it
measured, on which CPU and machine, and when; `--json FILE` keeps the whole result.

|Command|Measures|In this document|
|---|---|---|
|`bench.py identical`|two binaries over every PDF under folders, per option set: exit code, `--report` record without `ms`, every output file|G-bytes|
|`bench.py instr`|instructions a conversion under callgrind, `(I(2N) - I(N)) / N`, and the change|G-warm, the steps' tables|
|`bench.py warm`|`--bench` best of 3 per document, the binaries alternating: ms/page median, p95, mean, pages/s|release bar, warm|
|`bench.py fresh`|`tools/bench/fresh_timer.c`, 11 runs a document after 2, rounds in rotated order: median, paired change, p95, mean, CPU time, page faults; with `--per-doc` also each document's figures and the ten slowest|release bar, fresh|
|`bench.py pages`|fresh wall time of multi-page documents per `--workers` count, summed (`tools/bench/cmdbench.c`)|Pages in parallel|
|`bench.py tools`|every tool through its own command line as a fresh process: time, factor against RayoPDF, peak RSS|the comparison of a version record|
|`bench.py wasm`|the WebAssembly module in Node.js (`tools/bench/wasm_bench.mjs`, `rayopdf-wasm:1`), best of 3 per document, the modules alternating: what `warm` prints; with `--native BIN` the exit code and output bytes against `BIN --convert`, "identical: N of M"|the wasm module's speed and identity|

```sh
python3 tools/dev.py image && docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker   # once
python3 tools/dev.py build --config static --dir perf-static   # then copy build/perf-static/rayopdf away: NEW
E=benchmark-output/eval; O=$E/data/odl-bench/pdfs
python3 tools/bench.py identical --a BASE --b NEW --input $E/corpus --input $O --set none=--images=none \
    --set images= --set nostruct=--no-structure
python3 tools/bench.py instr --bin base=BASE --bin new=NEW --opt=--images=none --doc FIXTURE.pdf --doc ...
python3 tools/bench.py warm  --bin base=BASE --bin new=NEW --dir $O --opt=--images=none
python3 tools/bench.py fresh --bin base=BASE --bin new=NEW --dir $O --opt=--images=none --per-doc
python3 tools/bench.py pages --bin NEW --list MULTIPAGE.txt --cpus 2,3,4,5
python3 tools/bench.py tools --bin NEW --doc odl183=$O/01030000000183.pdf --doc safe348=0000348.pdf \
    --cpus 3 --cpus 2,3,4,5
python3 tools/dev.py build --config wasm --dir wasm-new   # then copy build/wasm-new away: WASMDIR
python3 tools/bench.py wasm --module new=WASMDIR --dir $O --opt=--images=none --native NEW
python3 tools/dev.py --image rayopdf-perf:1 run --mount benchmark-output:/src/benchmark-output:ro -- \
    python3 tools/hot_sections.py --build-dir build/release --build-dir build/static   # src/hot.ld
```

- **Binaries:** compare files kept outside `build/` (the base is the binary of the commit
  before), static builds or builds made in `rayopdf-build:2`.
- **CPUs:** `warm`, `fresh` and `wasm` time on one CPU (`--cpu 3` here), `pages` and `tools` on the CPUs of
  `--cpus`; a time counts only when nothing else runs on them or on their sibling threads (here
  CPU 9 is CPU 3's second thread). `RAYOPDF_DOCKER_CPUS=0-2,4-8,10-11` keeps the containers of
  `tools/dev.py`, `bench.py identical` and `bench.py instr` off both. Instruction counts and
  `identical` do not depend on the load; under load every time grows (the same binaries gave 2,215
  pages/s on the idle machine and about 1,550 with sixty processes runnable beside the pinned
  CPU), while the change between two binaries interleaved in one run moved by a point or two.
- **Not in the tool:** the task-clock and user instructions of `--batch --workers=1` come from
  `perf stat -e instructions:u` in a privileged container of `rayopdf-perf:1` (host
  `perf_event_paranoid` is 4; the binary is `/usr/lib/linux-tools/*/perf`).
- **Before `bench.py`:** the numbers up to 0.0.12 were measured with the campaign's scripts outside
  the repository, which `bench.py` ports (the warm suite, the two native timers, the rotation, the
  tools' commands) or replaces (`identical` and `instr` for the agent skills' `outcmp.py` and
  `abbench.py`).
- A `-g` build for profiles needs `-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=gold`: with `src/hot.ld` GNU
  ld drops the line information of the functions it places.
