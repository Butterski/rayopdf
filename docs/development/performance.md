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

## Open candidates (measured, not done)

- ShowString: the record built in place (draft above); about 300 instructions a glyph.
- Line and word texts copied once when contiguous in the page text (`LineText`, `WordText`, about
  5 M instructions over odl each).
- The OCR route's `MeasurePage` on pages that cannot go to OCR (1.3 % of odl).
- JPX: a row-streaming vertical wavelet pass for tall tiles; ICT and RGB interleave per row; the
  tier-1 per-block zero-fill replaced by the significance bits. JPEG, JPEG 2000, CCITT and JBIG2
  decoded a row or band at a time for bounded RSS.

## Rerunning

The lab scripts of the campaign (`gate.sh`, `warmsuite.py`, `fresh_timer.cpp`, `profile.sh`,
`ledger.jsonl`) live outside the repository in the campaign's lab directory; they call the agent
skills' `outcmp.py`, `abbench.py`, `freshstat.py` and `ledger.py`:

```sh
docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker
python3 tools/dev.py build --config release --dir perf-release   # copy the binary out first
gate.sh bytes STEP PREV NEW; gate.sh warm STEP PREV NEW; gate.sh suite STEP LABEL...
```

`TCPU=N gate.sh warm|suite ...` runs on another CPU than 3 (instruction counts do not depend on
it; times then are indicative only).
