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

## Open candidates (measured, not done)

- **Text path:** line and word texts read in place when contiguous in the page text, instead of
  copied by the callers that only read them (twelfth round: only one-word lines are contiguous,
  the copies being 0.4-0.5 % of odl, `multi_column` and `tables`).
- **The OCR route's `MeasurePage`** (1.4 % of odl after round 11): counts per font slot are
  untried.
- **Order and the writer:** Breuel's topological order (about 20 M on `multi_column`); the writer's
  `StyleWords` (33 M on `multi_column`). Round 10's rewrites of both gained nothing (tenth round,
  Tried and rejected), nor did skipping the link test of words outside the page's links (twelfth
  round).
- **The LaTeX rebuild:** after the twelfth round `malloc`, `free`, `new` and `delete` are still
  1.90 % of `arxiv_math` (`Matrix`, the limits' and braces' parts, `std::string` growth of the
  items' LaTeX); `MarkDisplayFormulas` reads each line's glyphs twice. After the fourteenth round
  (self costs in percent of `arxiv_math`): `MarkDisplayFormulas` 2.76 %, `FindInlineFormulas`
  2.52 %, `Builder::Row` 2.42 %, `PrepareMathGlyphs` 2.15 %,
  `ResolveAtom` 0.97 %; inclusive, `BuildLatex` 8.50 %, `Builder::Expr` 6.35 %, `Builder::Layout`
  5.32 %. `math.cpp` is at GCC's inlining budget for its unit (fifteenth round: 130 calls out of
  line for want of it), as `markdown_writer.cpp` was.
- **Blocks:** `semantics.cpp`'s `LineInfo` (1.18 % of odl, 1.67 % of `multi_column` self) and
  `BuildBlocks` (0.84 %, 1.32 %), measured in the fifteenth round.
- **Pages:** the structure tree and outline read while the pages are interpreted (about 3 % on
  tagged documents); the image round overlapped with the layout: images are encoded on the page
  threads (`9aecd77`), but in a round of their own between the image decisions and the layout, so
  one large image holds the layout up. Overlapping them needs the decisions and the files' naming
  apart and a round that runs beside the layout's, for untagged pages without inline images (a
  gain in wall time only).
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
|`bench.py fresh`|`tools/bench/fresh_timer.c`, 11 runs a document after 2, rounds in rotated order: median, paired change, p95, mean, CPU time, page faults|release bar, fresh|
|`bench.py pages`|fresh wall time of multi-page documents per `--workers` count, summed (`tools/bench/cmdbench.c`)|Pages in parallel|
|`bench.py tools`|every tool through its own command line as a fresh process: time, factor against RayoPDF, peak RSS|the comparison of a version record|

```sh
python3 tools/dev.py image && docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker   # once
python3 tools/dev.py build --config static --dir perf-static   # then copy build/perf-static/rayopdf away: NEW
E=benchmark-output/eval; O=$E/data/odl-bench/pdfs
python3 tools/bench.py identical --a BASE --b NEW --input $E/corpus --input $O --set none=--images=none \
    --set images= --set nostruct=--no-structure
python3 tools/bench.py instr --bin base=BASE --bin new=NEW --opt=--images=none --doc FIXTURE.pdf --doc ...
python3 tools/bench.py warm  --bin base=BASE --bin new=NEW --dir $O --opt=--images=none
python3 tools/bench.py fresh --bin base=BASE --bin new=NEW --dir $O --opt=--images=none
python3 tools/bench.py pages --bin NEW --list MULTIPAGE.txt --cpus 2,3,4,5
python3 tools/bench.py tools --bin NEW --doc odl183=$O/01030000000183.pdf --doc safe348=0000348.pdf \
    --cpus 3 --cpus 2,3,4,5
python3 tools/dev.py --image rayopdf-perf:1 run --mount benchmark-output:/src/benchmark-output:ro -- \
    python3 tools/hot_sections.py --build-dir build/release --build-dir build/static   # src/hot.ld
```

- **Binaries:** compare files kept outside `build/` (the base is the binary of the commit
  before), static builds or builds made in `rayopdf-build:2`.
- **CPUs:** `warm` and `fresh` time on one CPU (`--cpu 3` here), `pages` and `tools` on the CPUs of
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
