# Speed: how fast PDF engines and parsers get fast (research for rayopdf, 2026-11-09)

Research note, 2026-11-09. Third-party numbers are as published unless marked reproduced; numbers
measured for this note are marked **measured** and their method is in §10.

Conventions: **claim** = number published by a tool's vendor or author, not reproduced here.
[INFERENCE] = a judgement or arithmetic of this note, not a source's statement. "Fresh" = a new
process per document, start to exit; "warm" = conversions repeated inside one process. Shares of
RayoPDF's time are from `docs/development/performance.md` (round 3, callgrind over odl) unless a
measurement here says otherwise.

## 0. Summary

- **No engine is fast by one trick.** The fast ones parse lazily, cache what is pure, keep the
  per-token work small and allocate little. RayoPDF already does what the open engines do on the
  hot path: lazy objects, byte-class tables, operators packed into integers, numbers as an integer
  plus one division, lazily read font programs, single-page documents on the calling thread
  (§1, §2). The engine sources hold no unused hot-path design with a published large gain.
- **SIMD structural indexing (simdjson) does not fit content streams.** Its cost model charges
  about 11 cycles per structural element against 0.6–0.9 per byte. Its lead narrows on inputs
  with fewer than 5 bytes per token, and PDF operands are 1–4 bytes (§2.2). Number parsing at
  "a gigabyte per second" (fast_float) helps long floats, not 1–4-digit operands (§2.2).
- **Inflate is near its ceiling.** RayoPDF takes 1.15× libdeflate's time, and libdeflate is the
  fastest published DEFLATE decoder on a Zen CPU (TurboBench, §3). Closing the whole gap is worth
  about 1.3 % of the total [INFERENCE]. Multi-symbol tables only pay on long outputs per table.
- **The fresh process is mostly kernel work, and that work is page faults.** This is the largest
  new finding here (measured, §5.3, §6). A median odl conversion takes 150 minor faults.
  - 31 of them map text: a single conversion touches 31 of the 33 64 KiB windows of a 2.1 MB
    `.text`, while the functions it runs total about 0.5 MB.
  - 79 are heap pages.
  - The heaviest page (odl 141) takes 2,536 faults. 2,036 of them are `memmove` copies: its
    content-stream pieces are copied out of Inflate and appended to a page buffer that regrows
    by copying.
  - Indicative costs on this machine are 1.8–2.1 µs per anonymous fault and about 3.2 µs per
    file-backed fault-around fault. For the median document that is roughly 0.3 ms of a 1.22 ms
    fresh conversion [INFERENCE].
- **Most promising for the release bar:**
  - fewer copied and regrown buffers: inflate output, glyph vectors;
  - hot-section ordering, which packs the 0.5 MB of executed code into about 8 windows instead
    of 31;
  - then PGO, the largest published compiler-side gain (2 % to 20 % on parsers and compilers),
    but at a cost in reproducibility.
  - DT_RELR is free but small: −53 KB, −1 to −2 faults (measured).
- **Do not adopt:**
  - huge pages for short processes: one 2 MiB fault costs about 0.2 ms (measured);
  - pre-faulting: it is no cheaper per page (measured);
  - BOLT on this Zen 2 machine: no branch records;
  - `-march=x86-64-v3` for the whole binary: about 1 % typical, and FMA contraction can change
    output bytes;
  - static non-PIE: it loses ASLR on untrusted input.
- **Fair claims** must state the regime (fresh or warm), what the timed region includes, the
  threads, the corpus and the statistic. odl-bench's "s/page" column is per document and includes
  each engine's setup (§8).

## 1. Where RayoPDF starts (2026-11-09)

From the parent's profile (`docs/development/performance.md`, round 3, release build, GCC 13.3
`-O2`, AMD Ryzen 5 PRO 4650GE, CPU 3):
- **Warm:** odl median 0.42 ms/page, p95 0.91 ms, about 1,560 pages/s over all pages.
- **Fresh:** static build 1.22 ms per document; an empty static program takes 0.22 ms.
- **Release bar:** ≥ 2,000 pages/s warm single-core, ≤ 1 ms/page fresh.
- **Instruction shares:**

  |Area|Share|
  |---|---|
  |content lexer|~14 %|
  |inflate|~10 %|
  |glyph placement (ShowString)|~10 %|
  |font loading|~10 %|
  |BuildLines + FoldDuplicates|~15 %|
  |object parsing|~4 %|
  |writer|~6 %|
  |tables|~4 %|

Measured for this note (callgrind, whole process, `--convert --images=none`, the static binary of
`163408f`): odl 107 0.77 M instructions, odl 183 (median page) 4.65 M, odl 192 (p95) 10.17 M.
[INFERENCE] At these counts the user-mode work of a median fresh conversion is well under half of
its 1.22 ms. The rest is exec, page faults and cold caches, which §5–§6 quantify.

## 2. PDF engines and lexing

### 2.1 How the open engines read a page

|Engine (licence)|Objects|Content tokeniser|Caches|Text pipeline|
|---|---|---|---|---|
|**PDFium** (BSD-3)|Lazy: `CPDF_Parser::ParseIndirectObject` parses an entry on first request, cycle-guarded ([cpdf_parser.cpp](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/parser/cpdf_parser.cpp#1071)); every parsed object is kept for the document's life ([cpdf_indirect_object_holder.cpp](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/parser/cpdf_indirect_object_holder.cpp#58)). An object stream is inflated once, its header read, and each member parsed on demand ([cpdf_object_stream.cpp](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/parser/cpdf_object_stream.cpp#97)). The file is read through 512-byte windows ([cpdf_stream.h](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/parser/cpdf_stream.h#25))|Byte at a time with class predicates; each word is copied into a 256-byte buffer with an "is numeric" flag ([cpdf_streamparser.cpp](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/page/cpdf_streamparser.cpp#252)). Operators are looked up in a `std::map` ([cpdf_streamcontentparser.cpp](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/page/cpdf_streamcontentparser.cpp#600)); operands sit in a 16-slot ring of `std::variant`|Per document: fonts, colour spaces, images ([cpdf_docpagedata.h](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/page/cpdf_docpagedata.h#123)). Per process: standard-14 fonts, predefined CMaps ([cpdf_fontglobals.h](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/font/cpdf_fontglobals.h#26))|Needs the full page-object model (paths and images too), then a char list closed into lines ([cpdf_textpage.cpp](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdftext/cpdf_textpage.cpp#750))|
|**MuPDF** 1.28.6 (AGPL: design only)|Cached in the xref entry on first fetch ([pdf-xref.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/source/pdf/pdf-xref.c#L2548-L2603)); the first access to any member of an object stream parses all of its members ([pdf-xref.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/source/pdf/pdf-xref.c#L2097-L2248))|Inline `rp != wp ? *rp++ : refill` reads ([stream.h](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/include/mupdf/fitz/stream.h#L443-L448)). Integers by a "fast but inaccurate" `fast_atoi` ([pdf-lex.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/source/pdf/pdf-lex.c#L190-L267)). Operators of ≤ 3 bytes packed into a `uint32` and dispatched by one `switch`; operands in a `float` stack ([pdf-interpret.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/source/pdf/pdf-interpret.c#L1304-L1340))|Fonts and colour spaces in a reference-counted store, 4,096-slot hash under a lock ([store.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/source/fitz/store.c#L65-L71)). Glyph advances and Unicode→glyph caches are two-level: a block of 256 entries is filled at once on first touch ([font.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/source/fitz/font.c#L1971-L2039))|Lines are built as glyphs arrive, in content order, from pen deltas against size-relative thresholds; no global sort ([stext-device.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/source/fitz/stext-device.c#L869-L1013))|
|**pdf.js** (Apache-2.0)|`XRef.fetch` cache keyed by number ([xref.js](https://github.com/mozilla/pdf.js/blob/6619bf0c6fa6f7797e6b109cb6d431b0ab1ed00d/src/core/xref.js#L861-L910)); streams are not cached; object streams are parsed whole ([xref.js](https://github.com/mozilla/pdf.js/blob/6619bf0c6fa6f7797e6b109cb6d431b0ab1ed00d/src/core/xref.js#L969-L1052))|`specialChars[256]` byte classes. `getNumber` accumulates every digit into one integer and divides once by a power of ten ([parser.js](https://github.com/mozilla/pdf.js/blob/6619bf0c6fa6f7797e6b109cb6d431b0ab1ed00d/src/core/parser.js#L884-L1022))|A font *promise* per font reference, plus aliases ([evaluator.js](https://github.com/mozilla/pdf.js/blob/6619bf0c6fa6f7797e6b109cb6d431b0ab1ed00d/src/core/evaluator.js#L1208-L1340))|`getTextContent` on a worker, batches of 10 items, size-relative space factors ([evaluator.js](https://github.com/mozilla/pdf.js/blob/6619bf0c6fa6f7797e6b109cb6d431b0ab1ed00d/src/core/evaluator.js#L2485-L2528))|
|**Poppler** 26.11.0 (GPL: black box only)|`XRef::fetch` **re-parses** an uncompressed object on every fetch, under a mutex ([XRef.cc](https://gitlab.freedesktop.org/poppler/poppler/-/blob/poppler-26.11.0/poppler/XRef.cc#L1310-L1375)); object streams are cached and parsed eagerly|`specialChars[256]`, a 128-byte token buffer, and a one-byte look-ahead cache, because `getChar` follows `lookChar` "~30 % on PDF Reference 1.6" ([Lexer.h](https://gitlab.freedesktop.org/poppler/poppler/-/blob/poppler-26.11.0/poppler/Lexer.h#L37-L97))|—|`TextOutputDev`: words bucketed by baseline; `coalesce` runs an all-pairs block loop plus a topological sort ([TextOutputDev.cc](https://gitlab.freedesktop.org/poppler/poppler/-/blob/poppler-26.11.0/poppler/TextOutputDev.cc#L2195-L2293))|
|**lopdf** 0.47 (MIT)|**Eager**: every xref entry is parsed and every object stream expanded at load, in parallel with rayon by default ([reader.rs](https://github.com/J-F-Liu/lopdf/blob/4eaec16bd176da538d960fad5e06fa0b77f267da/src/reader.rs#L1017-L1137)); nom parser|—|An object-stream cache, because without it `/Length` lookups were "quadratic" ([reader.rs](https://github.com/J-F-Liu/lopdf/blob/4eaec16bd176da538d960fad5e06fa0b77f267da/src/reader.rs#L367-L372))|(via pdf-extract)|
|**hayro-syntax** (MIT/Apache-2.0)|"Zero-parse": dictionaries keep raw bytes plus key offsets and parse values on use; linear key scan up to 16 keys, binary search above ([dict.rs](https://github.com/LaurenzV/hayro/blob/360f4a23b76ff54c603f79b39b2f2f70c6967041/hayro-syntax/src/object/dict.rs#L126-L148))|—|Decoded object streams in an append-only list, decoded outside the lock|Renderer; the README lists missing glyph caching as a known gap and gives no numbers ([README](https://github.com/LaurenzV/hayro/blob/360f4a23b76ff54c603f79b39b2f2f70c6967041/README.md#L14-L46))|
|**pdf_oxide** 0.3.78 (MIT/Apache-2.0)|Page-tree cache built once; one file scan for missing offsets|A byte-level parser for path operators that is "2–4× faster" because it allocates no object per operand ([parser.rs](https://github.com/yfedoseev/pdf_oxide/blob/08accb2e0be8252309083a33fafc74015269594c/src/content/parser.rs#L161-L176))|Six font-cache layers, including a **process-wide cross-document** cache keyed by structural hash; subset and Type 3 fonts are excluded ([global_cache.rs](https://github.com/yfedoseev/pdf_oxide/blob/08accb2e0be8252309083a33fafc74015269594c/src/fonts/global_cache.rs#L1-L10))|Claim: the mean fell from 23.3 to 0.8 ms per document after four algorithmic fixes: the O(n²) page walk (a 10,000-page PDF 55 s → 332 ms), the single offset scan, one extraction pass instead of two, and a pre-sized operator vector ([performance](https://pdf.oxide.fyi/docs/performance))|

**Published engine speeds**:
- PyMuPDF claims plain-text extraction at 2.01 s for 1,310 pages and 2.41 s for 3,071 pages,
  file writes included. That is ≈ 0.8–1.5 ms/page [INFERENCE: division]. XPDF takes 3.42× as
  long, PyPDF2 12.7× and pdfminer 28.4×; no CPU is stated
  ([app4.rst](https://github.com/pymupdf/PyMuPDF/blob/2.1/docs/app4.rst)).
- pdf_oxide claims 0.8 ms mean per document over 3,830 PDFs, "single-thread, no warm-up", against
  PyMuPDF 4.6 ms and pypdfium2 4.1 ms
  ([README](https://github.com/yfedoseev/pdf_oxide/blob/08accb2e0be8252309083a33fafc74015269594c/README.md#L88-L114)).
  Its method is disputed (§8).
- No text-extraction timings are published for PDFium, Poppler or hayro.

**What RayoPDF already has** (`performance.md` steps; source `src/pdf/content_lexer.cpp`,
`src/pdf/syntax.h`, `src/common/batch.cpp`):
- lazy objects;
- byte classes by one table;
- operators by tables;
- numbers read in one pass as an integer plus one division by `kPowers[fraction]` (pdf.js's
  method, correctly rounded below 2^53);
- strings, names and arrays as views;
- embedded font programs read only when a shown code needs them;
- ToUnicode read by a scanner;
- one worker run on the calling thread when there is one job (lopdf makes the same choice for
  small documents, below).

The open design points left are:
- **Paged advance and Unicode caches filled 256 entries at a time** (MuPDF). [INFERENCE] These
  touch glyph placement (~10 %), mostly on CID/TrueType fonts.
- **Lines built as glyphs arrive** (MuPDF stext). [INFERENCE] This could cut part of BuildLines'
  15 %, but it changes the output, so it is a quality project, not a byte-identical step.
- **A process-wide cache of non-subset fonts across documents** (pdf_oxide). It pays only in
  `--batch` and `--serve`. It stays deterministic only if a hit and a miss give identical results.

### 2.2 Short tokens: SIMD indexing, number parsing, branches

- **simdjson** (Langdale & Lemire, [arXiv:1902.08318](https://arxiv.org/abs/1902.08318) v7).
  - Stage 1 classifies 64-byte blocks with two `vpshufb` nibble lookups, finds string interiors
    with one carry-less multiply, and extracts structural positions with `tzcnt`/`blsr`
    (paper §3.1). Stage 2 walks those positions.
  - Its cost model (Skylake, R² ≥ 0.99) is ≈ 19·floats + 11·structurals + 0.92·bytes cycles
    (paper §4.3). Speed is 2.2–3.2 GB/s on string-heavy files but only 0.94–1.1 GB/s on number-heavy
    ones (Table 10).
  - On canada.json (6.7 B per structural) a scalar in-place parser, gason, reaches 0.95 GB/s
    against simdjson's 1.1 (Tables 5, 10).
  - simdzone applies the same stage 1 to DNS zone files (~60-byte records) and gets 0.8 GB/s,
    3–4× Knot DNS ([arXiv:2411.12035](https://arxiv.org/abs/2411.12035), Table 3). Its records
    are still an order of magnitude longer than a content-stream operand.
  - SIMD transcoding: "over tiny strings, there is little benefit"; it needs about 100 bytes
    ([arXiv:2109.10433](https://arxiv.org/abs/2109.10433) §6.6).
  - [INFERENCE] A content stream has a structural every 2–5 bytes. Stage 1 would add work per
    token and remove little from stage 2. Only long skips could gain: string literals, inline
    image data, comments.
- **Numbers.**
  - fast_float (Eisel–Lemire, [arXiv:2101.11408](https://arxiv.org/abs/2101.11408)) costs 280
    instructions and 66 cycles per random double, against 1,100 and 370 for glibc `strtod` (Zen 2,
    Table 10). On integers, where every parser takes Clinger's fast path, its lead is "modest
    (about 20 %)" (Tables 9, 12).
  - GCC 12+ `std::from_chars` calls a bundled fast_float
    ([floating_from_chars.cc](https://github.com/gcc-mirror/gcc/blob/releases/gcc-12.1.0/libstdc++-v3/src/c++17/floating_from_chars.cc#L66-L80)),
    out of line.
  - For 1–3 digits, Lemire's branchless parse uses one 4-byte load, an XOR, a variable shift and
    one multiply by `0x640a01`. In millions of numbers per second (Ice Lake, GCC 12):

    |Input|SWAR|naive loop|`from_chars`|
    |---|---|---|---|
    |random lengths|425|210|145|
    |predictable lengths|425|365|255|

    So it is about 2× the naive loop on random lengths and +15 % on predictable ones. It needs
    4 readable bytes ([blog 2023-11-28](https://lemire.me/blog/2023/11/28/parsing-8-bit-integers-quickly/)).
  - The 8-digit SWAR form was "only barely faster" than a plain loop
    ([blog 2018-10-03](https://lemire.me/blog/2018/10/03/quickly-parsing-eight-digits/)). That
    matches RayoPDF's rejected 8-digit and 2-digit attempts (+7.0 % and +2.9 % on odl 141).
- **Branches.**
  - A mispredict costs about 18 cycles on Zen 1–3. Zen 2 predicts loops with up to 12 iterations
    well (Agner Fog, [microarchitecture.pdf](https://www.agner.org/optimize/microarchitecture.pdf)
    §3.13, 2026-05-23; [7-cpu Zen 2](https://www.7-cpu.com/cpu/Zen2.html)).
  - Turning a data-dependent branch into arithmetic cut 15 to 3.8 cycles per item in Lemire's
    example ([blog 2019-10-15](https://lemire.me/blog/2019/10/15/mispredicted-branches-can-multiply-your-running-times/)).
- **Dispatch.**
  - CPython's computed goto was "up to 15–20 % faster" than `switch`
    ([ceval_macros.h](https://github.com/python/cpython/blob/v3.12.0/Python/ceval_macros.h#L28-L30)).
  - Modern indirect predictors make the difference small: Haswell/ITTAGE in Rohou et al., CGO 2015
    ([hal-01100647](https://inria.hal.science/hal-01100647)).
  - CPython 3.14's tail-call interpreter gives 3–5 %
    ([What's new](https://docs.python.org/3.14/whatsnew/3.14.html)).
- **End-of-input checks.**
  - re2c's sentinel method "eliminates the need for the end of input checks altogether" when a
    byte can never occur in a token; otherwise padding allows one check per DFA component
    ([re2c manual](https://re2c.org/manual/manual_c.html)).
  - simdjson requires `SIMDJSON_PADDING` readable bytes past the input
    ([performance.md](https://github.com/simdjson/simdjson/blob/v5.1.1/doc/performance.md)).
- **Benchmark trap.** Branch predictors learn small inputs that are repeated: below about 30 kB,
  far fewer mispredicts were seen than expected
  ([arXiv:2010.03090](https://arxiv.org/abs/2010.03090) §7.1). [INFERENCE] RayoPDF's warm
  best-of-3 on one-page documents favours branchy code. Gains of branch-free rewrites should be
  checked in instruction counts and across rotating documents (`abbench.py` already does both).

## 3. Decompression

- **libdeflate** v1.26 (MIT; [`92e6a0d`](https://github.com/ebiggers/libdeflate/tree/92e6a0db9fa848d742f9eb286c92afc60f2c3dda)).
  Design, from `lib/deflate_decompress.c` and `lib/decompress_template.h`:
  - an 11-bit litlen root table, one subtable level, and `table_bits` capped at the longest
    codeword to cut table builds;
  - an 8-bit offset table;
  - a fast loop that checks bounds once, refills branch-free and preloads the next entry;
  - up to 2 extra literals per refill: "we could actually do 3 … but that actually decreases
    performance slightly";
  - match copies of 5 words, with a broadcast for offset 1;
  - a BMI2 variant chosen at run time.
  - v1.14 was 1.26× faster than v1.13 on Zen 2 with GCC (NEWS.md L181–205).
  - RayoPDF's Inflate already follows this design (packed entries, 56-bit refills, up to 3
    literals a refill). It measured 1.58 vs 1.29 ns/byte on odl text streams
    (`performance.md` "Tried and rejected").
- **Published speeds.**
  - TurboBench on silesia.tar, Ryzen 6600HS (2023), decompression in MB/s
    ([zlib-ng#1486](https://github.com/zlib-ng/zlib-ng/issues/1486)): libdeflate 1,076–1,134,
    ISA-L igzip 969–1,081, zlib-ng 652–735, zlib 449–484.
  - zlib-ng 2.3.1 inflates Silesia-small in 0.0173 s against stock zlib's 0.0736 s on a Ryzen
    8700GE ([discussion #2022](https://github.com/zlib-ng/zlib-ng/discussions/2022)).
  - Chromium's inflate chunk copy plus SIMD Adler/CRC: +50 % zlib-wrapped inflate on the snappy
    corpus ([commit 65e2abc](https://chromium.googlesource.com/chromium/src/+/65e2abcb74b1c07fa14f46abaa1fb1717892eec3)).
- **Multi-symbol tables.**
  - ISA-L igzip's 12-bit "large" table packs up to 3 literals per entry
    ([igzip_inflate.c](https://github.com/intel/isa-l/blob/7c3479e0a9dac17f448603ec1ad64c7c625f530c/igzip/igzip_inflate.c)).
    It is still not faster than libdeflate in the table above.
  - zstd's double-symbol Huffman table costs 735 to build against 221 for single-symbol, but
    decodes 256 bytes in 107 against 192
    ([huf_decompress.c](https://github.com/facebook/zstd/blob/f8745da6ff1ad1e7bab384bd1f9d742439278e99/lib/decompress/huf_decompress.c#L1793-L1840)).
    [INFERENCE] It breaks even only after about 1.7–2.9 KB of output per table. DEFLATE rebuilds
    tables per block, and many content streams are short.
  - Giesen's multi-stream trick needs several bitstreams
    ([Oodle 3-stream decoders](https://fgiesen.wordpress.com/2022/09/05/entropy-decoding-in-oodle-data-x86-64-3-stream-huffman-decoders/));
    DEFLATE has one.
- **[INFERENCE] Value for RayoPDF.**
  - Inflate is ~10 % of instructions, and the gap to libdeflate is 13 % of that: ≤ 1.3 % total.
    The cheap parts are a BMI2 build of the decode loop behind a function pointer (libdeflate's
    pattern, §4.3) and capping `table_bits`; RayoPDF already tried a 10-bit root table, which
    was not kept.
  - The larger lever is not decoding but where the output goes: §5.3 shows the copy-out and the
    regrowth of a page's concatenated content costing more faults than the decode.

## 4. Compiler and link time

### 4.1 Profile-guided optimisation

- **Options.**
  - GCC 13 `-fprofile-use` enables value profiling, unrolling, peeling, tracer, IPA-CP cloning,
    vectorisation and function reordering by first execution
    ([Optimize-Options](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/Optimize-Options.html)).
  - Code that the training run does not reach is optimised "aggressively for size" unless
    `-fprofile-partial-training` is given, which "leads to significantly bigger code".
- **Published gains.**

  |Program|Gain|Source|
  |---|---|---|
  |SPEC2017 int (GCC 9, Zen)|PGO ≈ +2 %, LTO ≈ +4 %, both ≈ +6.5 %|[Hubička 2019](http://hubicka.blogspot.com/2019/05/gcc-9-link-time-and-inter-procedural.html)|
  |rustc|PGO −5 % instructions, −10 to −16 % wall time|[Rust blog 2020-11-11](https://blog.rust-lang.org/inside-rust/2020/11/11/exploring-pgo-for-the-rust-compiler/)|
  |Clang/LLVM|"can decrease overall compile time by 20 %"|[HowToBuildWithPGO.rst](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.0/llvm/docs/HowToBuildWithPGO.rst#L10)|
  |Chrome M85|pages "up to 10 % faster at the median"|[Chromium blog 2020-08-25](https://blog.chromium.org/2020/08/chrome-just-got-faster-with-profile.html)|
  |serde_json (community, M1 Pro)|+18 to +34 %; simd-json +1 to +6 %|[json-benchmark#23](https://github.com/serde-rs/json-benchmark/issues/23)|
  |RayoMD (size only)|+16 KB, +127 KB with partial training; no speed record|[performance.md L358](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/performance.md#L358)|

  [INFERENCE] The serde_json and simd-json pair shows the pattern: branchy scalar parsers gain
  most, branch-free ones least. RayoPDF's lexer, font and line code are branchy, so 5–15 % warm
  is plausible, but RayoPDF has not measured it.
- **Reproducibility.**
  - PGO builds are unreproducible "unless all inputs in step 2 are constant"
    ([theunreproduciblepackage/pgo](https://github.com/bmwiedemann/theunreproduciblepackage/blob/b4414248976561b5b4bed719e5168287df318910/pgo/README.md)).
    The remedies listed are a constant training input, or stored profiles that depend on
    architecture and source.
  - GCC's default `-fprofile-reproducible=serial` is reproducible only for single-threaded,
    order-stable training. The docs warn that counts can depend on temporary file names or
    address randomisation through hash collisions
    ([Instrumentation-Options](https://gcc.gnu.org/onlinedocs/gcc-13.3.0/gcc/Instrumentation-Options.html)).
  - Chromium pins one downloaded profile per build by name
    ([linux.pgo.txt](https://chromium.googlesource.com/chromium/src/+/refs/tags/130.0.6723.0/chrome/build/linux.pgo.txt)).
  - Arch and Fedora build CPython with PGO anyway
    ([PKGBUILD](https://gitlab.archlinux.org/archlinux/packaging/packages/python/-/blob/main/PKGBUILD),
    [python3.13.spec](https://src.fedoraproject.org/rpms/python3.13/blob/rawhide/f/python3.13.spec)).
  - **[INFERENCE] For RayoPDF:**
    - The training input must be constant, but the repository commits no PDFs (AGENTS.md rule
      1). Either the release build generates its training corpus deterministically, as the
      synthetic producers do with pinned versions, or the profiles are pinned by hash outside
      the repository.
    - Train with `--workers=1` and ASLR off (`setarch -R`).
    - MinGW ships `libgcov.a`, so a Windows profile is possible, but it is a second training run.
    - Output bytes should not change: no `-ffast-math`, and the baseline ISA has no FMA. Still,
      every PGO build must pass G-bytes.
- **AutoFDO, BOLT, Propeller** all want hardware branch records. On AMD, Linux's branch sampling
  starts with Zen 3 (family 19h) ([brs.c](https://github.com/torvalds/linux/blob/v6.1/arch/x86/events/amd/brs.c#L3)),
  and `perf record -b` needs Zen 4's LbrExtV2 ([LWN 904482](https://lwn.net/Articles/904482/)).
  The development CPU is Zen 2.

### 4.2 LTO, BOLT, hot/cold placement

- **LTO.**
  - SPEC2017 int about +4 % (Hubička 2019); rustc thin LTO −4.1 % instructions
    ([Kobzol 2022](https://kobzol.github.io/rust/rustc/2022/10/27/speeding-rustc-without-changing-its-code.html)).
  - The build image's `libc.a` carries no LTO sections (checked locally), so LTO stops at glibc.
  - RayoMD keeps LTO off in its package
    ([performance.md L2007](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/performance.md#L2007)).
  - [INFERENCE] LTO renames local sections (`.lto_priv.N`), so a name-based hot list (below) has
    to be generated after LTO.
- **BOLT** ([arXiv:1807.06735](https://arxiv.org/abs/1807.06735)).
  - Up to 20.4 % on top of FDO+LTO for GCC and Clang binaries.
  - Clang built with PGO+LTO built Clang in 202.7 s, with BOLT 180.1 s. Taken branches fell 41 %
    while instructions fell only 0.6 % ([OptimizingClang.md](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.0/bolt/docs/OptimizingClang.md#L100-L125)).
  - Requirements: `--emit-relocs` and `-fno-reorder-blocks-and-partition`. Without branch
    records the expected gains "are expected to be lower"
    ([README](https://github.com/llvm/llvm-project/blob/llvmorg-19.1.0/bolt/README.md#L10-L36)).
  - Static non-PIE input support is from Nov 2023; static-pie support is not confirmed.
  - It is ELF only, so there is no Windows counterpart.
- **Propeller** (ASPLOS'23): +1.1 to +8 % over PGO+ThinLTO, Clang +7 %
  ([Google Research](https://research.google/pubs/propeller-a-profile-guided-relinking-optimizer-for-warehouse-scale-applications/)).
- **Hot/cold splitting.**
  - GCC's `-freorder-blocks-and-partition` (on at `-O2`) moves cold blocks out "to improve paging
    and cache locality". Clang's `-fsplit-machine-functions` needs a profile.
  - `-freorder-functions` "isn't effective unless you either provide profile feedback … or
    manually annotate functions with hot or cold attributes" (GCC 13 Optimize-Options).
  - The RayoPDF binary has 69 `.cold` fragments totalling 1,540 bytes, so almost nothing is split
    today (checked locally).
- **Section ordering without a profile compiler.**
  - RayoMD's `src/hot.ld` is generated by `scripts/hot_sections.py`. It is a GNU ld
    `INSERT BEFORE .text` list of the input sections callgrind sees in fresh runs, glibc members
    included.
  - Result: 3 to 4 fewer faults, 7 to 19 µs per fresh export
    ([performance.md L1016](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/performance.md#L1016)).
    A rename that dropped one hot function from the list cost +0.7 to +1.2 % cycles (L580).
  - It needs GNU ld: gold has no `INSERT` (L1940).
  - RayoPDF has not tried it ("measured too small" was RayoMD's number). §6.2 measures why it
    should matter more here.

### 4.3 ISA levels and multiversioning

- **Levels and published gains.**
  - x86-64-v2 adds SSE4.2 and POPCNT; v3 adds AVX2, BMI1/2, FMA, LZCNT and MOVBE
    ([psABI](https://gitlab.com/x86-psABIs/x86-64-ABI/-/blob/e1ce098331da5dbd66e1ffc74162380bcc213236/x86-64-ABI/low-level-sys-info.tex#L15-L67)).
  - Ubuntu's archive rebuilt for v3: "most packages show a slight (around 1 %) performance
    improvement" ([Ubuntu discourse 2025-10-30](https://discourse.ubuntu.com/t/introducing-architecture-variants-amd64v3-now-available-in-ubuntu-25-10/71312)).
  - Red Hat: v3 "primarily benefits numerical applications"
    ([Red Hat 2024-01-02](https://developers.redhat.com/articles/2024/01/02/exploring-x86-64-v3-red-hat-enterprise-linux-10)).
- **Determinism.**
  - GCC defaults to `-ffp-contract=fast` for C++ (the ISO switch to `off` applies to C only,
    [c-opts.cc](https://github.com/gcc-mirror/gcc/blob/releases/gcc-13.3.0/gcc/c-family/c-opts.cc#L819-L827)).
  - [INFERENCE] Any code compiled with FMA available (v3, or `target("fma")`) may fuse `a*b+c`
    and change RayoPDF's float geometry, and so its Markdown, against the baseline and Windows
    builds. Such code needs `-ffp-contract=off`.
- **Dispatch.**
  - `target_clones` works on static-pie (IRELATIVE, applied by `ARCH_SETUP_IREL()` in
    [libc-start.c](https://sourceware.org/git/?p=glibc.git;a=blob;f=csu/libc-start.c;hb=refs/tags/glibc-2.39#l267)).
    MinGW GCC 13 rejects it ("requires 'ifunc'", checked locally;
    [GCC bug 107727](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=107727)).
  - The portable pattern is libdeflate's: a function pointer that starts at a dispatcher,
    probes CPUID once and rewrites itself; the variant is compiled with
    `__attribute__((target("bmi2")))`
    ([deflate_decompress.c L1095–1123](https://github.com/ebiggers/libdeflate/blob/bf11fcce4d2e12c413ac92ba519917a92e9baac7/lib/deflate_decompress.c#L1095-L1123)).
    zeux describes the same move away from `target_clones`
    ([blog 2024-04-20](https://zeux.io/2024/04/20/target-clones-trap/)).

## 5. Memory

### 5.1 Allocators and arenas

- **Custom allocators.** In 6 of 8 programs with custom allocators, the general-purpose Lea
  allocator (glibc malloc's ancestor) was as fast or faster. Only region (arena) allocators won,
  by up to 44 %, at up to 230 % more memory (Berger, Zorn & McKinley, OOPSLA 2002,
  [PDF](https://sfkaplan.people.amherst.edu/courses/2004/spring/cs40/papers/BZM:RCMA.pdf)).
- **mimalloc** ([MSR-TR-2019-18](https://www.microsoft.com/en-us/research/uploads/prod/2019/06/mimalloc-tr-v1.pdf)):
  free-list sharding gave more than 25 % in Lean. A bump-pointer variant was "consistently about
  2 % worse", because it adds two hard-to-predict branches.
- **Small buffers and arenas.** LLVM `SmallVector` stores N elements inline and calls no malloc
  while the size stays within N
  ([ProgrammersManual](https://llvm.org/docs/ProgrammersManual.html#llvm-adt-smallvector-h)).
  Protobuf arenas make allocation "a simple pointer increment" and keep messages contiguous
  ([arenas](https://protobuf.dev/reference/cpp/arenas/)).
- **[INFERENCE] For RayoPDF.** It already uses page arenas and per-thread scratch reused across
  documents (round 3). Allocator speed is not where its time goes; *new pages* are (§5.3).

### 5.2 Data layout

- **Zig 0.8.0** moved its tokens and AST to struct-of-arrays: a token is a 1-byte tag plus a
  4-byte offset. The parser got 28 % fewer instructions, 15 % fewer cache misses and 22 % less
  wall time; IR generation 39 % less time and 40 % less peak memory
  ([release notes](https://ziglang.org/download/0.8.0/release-notes.html#Reworked-Memory-Layout)).
- **Zen 2 latencies** ([7-cpu](https://www.7-cpu.com/cpu/Zen2.html); Agner Fog, Table 22.3):
  L1D 4 cycles, L2 12, L3 38–40; an L2 TLB miss with 4 KiB pages costs about 55 cycles.
- **RayoPDF's `Layout::Glyph`** (`src/layout/page.h`) is 56 bytes and stores two boxes: the
  upright one and the direction frame, which are equal for left-to-right text. [INFERENCE]
  - A narrower record, or a split into hot geometry and cold attributes, would cut the bytes
    BuildLines and FoldDuplicates stream, and the pages a heavy page faults in (§5.3: 111 faults
    of odl 192 are `std::vector<Glyph>` growth).
  - Expected gain: a share of those phases' 15 %, not published for this workload.

### 5.3 Page faults (measured)

Minor faults of one fresh `rayopdf --convert DOC OUT --images=none`, the static binary of
`163408f`, attributed with `perf record -e page-faults -d` to their mapping and faulting
function (method in §10):

|Mapping|odl 107|odl 183 (median)|odl 192 (p95)|odl 141 (slowest)|
|---|---|---|---|---|
|all|95|150|328|2,536|
|text (`r-xp`)|31|31|31|31|
|RELRO (relocation writes) + `.data`|11 + 2|11 + 2|11 + 2|11 + 2|
|`.rodata`|5|5|5|5|
|`.bss` (anonymous, after the file mapping)|4|4|8|11|
|heap (brk)|27|79|139|395|
|separate anonymous mappings (malloc ≥ 128 KiB)|0|0|116|2,043|
|stack|9|10|10|10|
|input PDF|2|4|2|24|
|first page of the file, vDSO, vvar|4|4|4|4|

Who faults the heap:
- **odl 183:**
  - `_int_malloc` 40 (first touch of new chunks);
  - `std::vector<Glyph>` growth 13;
  - `ShowString` 8;
  - `memmove` 6;
  - Inflate 4.
- **odl 192:**
  - `std::vector<Glyph>` growth 111;
  - `ShowString` 52;
  - `_int_malloc` 35;
  - `std::vector<TWord>` 18.
- **odl 141:**
  - `memmove` 2,038. Call stacks (`perf record -g`) put 1,655 of them in
    `Document::PageContent` and 381 in `Inflate`'s copy-out (`out.assign`).
  - The page's `/Contents` is an array of streams. Each piece is inflated into Inflate's
    per-thread buffer (grown by a new block plus `memcpy`), then copied into a piece string,
    then appended to the page string, which regrows by doubling and copies everything so far
    (`src/pdf/document.cpp` L1553–1575, `src/pdf/inflate.cpp` L361–376, L822–836).
  - The final page string is ~5.6 MB: its mapping takes 1,403 faults, and its earlier sizes
    take most of the rest. Inflate's buffer is freed after a call when above 1 MiB
    (`kKeptBuffer`);
  - `std::vector<Shape>` 96;
  - `Paint` 95;
  - `std::vector<Ruling>` 53.

Over all 200 odl PDFs (one run each):

|Setting|Faults in total|Median|p95|odl 141|
|---|---|---|---|---|
|default|40,521|175|324|2,537|
|`GLIBC_TUNABLES=glibc.malloc.mmap_threshold=33554432`|38,757 (−4.4 %)|175|279|2,148|
|`glibc.malloc.hugetlb=1`|38,271 (−5.6 %)|175|280|1,723|

The faulting counter here includes exec. That is why it reads 175 at the median, where
`performance.md` reports about 155.

**What a fault costs here** (measured, indicative only: CPU 7 while other work ran, load average
3–12, kernel 7.0.0-38 with `init_on_alloc` on, THP `madvise`):

|Operation|Cost|
|---|---|
|first touch of an anonymous 4 KiB page|1.8–2.1 µs|
|the same after `MAP_POPULATE`|1.8–2.1 µs per page (no cheaper)|
|`MADV_HUGEPAGE` region|0.42–0.46 µs per 4 KiB, i.e. about 0.2 ms per 2 MiB fault|
|touching an already-mapped page|12–16 ns|
|file-backed text fault with 64 KiB fault-around|≈ 3.2 µs per fault|
|`MADV_POPULATE_READ` over all 2 MiB of text|190 µs (vs 104 µs to fault in all 33 windows)|

The cost sits in allocating and zeroing pages, not in the trap. So pre-faulting does not help, and
huge pages trade many small faults for a few 0.2 ms ones, which only pays when the whole 2 MiB is
used.

Kernel facts:
- Fault-around maps up to 64 KiB around a *read* fault on a file mapping; a copy-on-write fault
  maps one page ([mm/memory.c](https://github.com/torvalds/linux/blob/v6.8/mm/memory.c#L4633-L4666)).
- Multi-size THP (64 KiB anonymous folios) exists but is off by default and set per system, not
  per process
  ([transhuge.rst L48–51, L142](https://github.com/torvalds/linux/blob/v6.8/Documentation/admin-guide/mm/transhuge.rst)).

glibc facts:
- Allocations of ≥ 128 KiB go to `mmap` and are unmapped on `free`. The threshold rises
  dynamically after such a free, up to 32 MiB ([mallopt(3)](https://man7.org/linux/man-pages/man3/mallopt.3.html)).
- `realloc` of an mmapped chunk uses `mremap` on Linux, so the old pages are moved, not copied or
  faulted again ([malloc.c L3475](https://github.com/bminor/glibc/blob/glibc-2.39/malloc/malloc.c#L3475),
  [malloc-sysdep.h L59](https://github.com/bminor/glibc/blob/glibc-2.39/sysdeps/unix/sysv/linux/malloc-sysdep.h#L59)).

[INFERENCE] Consequences for RayoPDF:
- **Fault time.** At these costs the median fresh conversion spends roughly 0.1 ms on text faults
  and 0.15–0.2 ms on heap faults.
- **Large or many-piece content.**
  - PDFium decodes a page's pieces first, then copies them into one buffer with a space between
    each ([cpdf_contentparser.cpp](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/core/fpdfapi/page/cpdf_contentparser.cpp#177)).
  - Decoding each piece straight onto the end of one buffer reserved once would remove the
    regrowth and copy-out faults (≈ 630 of odl 141's 2,036).
  - Lexing the pieces in sequence without concatenating them would also avoid touching the
    5.6 MB page string at all (≈ 1,900). ISO 32000-1 lets the division between streams "occur
    only at the boundaries between lexical tokens"
    ([PDF 32000-1:2008](https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf),
    §7.7.3.3, Table 30, `Contents`). So a lexer can carry its operands from one piece to the
    next, treating a piece's end as the white space RayoPDF inserts today.
  - Each fault avoided is ~2 µs plus a 4 KiB copy. In the warm loop the effect depends on
    glibc's dynamic mmap threshold, which rises after the first large free.
- **Glyph and word vectors.** Reserving them from a size estimate (pdf_oxide pre-sizes its
  operator vector at `len/20`), or growing them in chunks without copying, removes the regrowth
  faults on heavy pages.
- **Caveats.**
  - `mallopt(M_MMAP_THRESHOLD)` gives the measured −4.4 % process-wide in one line, but it keeps
    freed memory in long `--batch` and `--serve` runs. RayoMD rejected the same tunable in its
    GUI for that reason, measuring 860 → 490 faults a keystroke with no clear time gain
    ([performance.md L96](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/performance.md#L96)).
  - Do not use huge pages (`glibc.malloc.hugetlb`) for a 1 ms process. They cut the fault count
    by 5.6 %, but every 2 MiB fault costs about 0.2 ms. RayoMD found them slower for batches too
    (L1988).

## 6. Process start

### 6.1 Relocations and the C runtime

- **glibc's static start** (2.39): auxv, tunables, CPU features, `_dl_relocate_static_pie`, then
  the IRELATIVE ifunc resolvers, then TLS, guards and constructors
  ([libc-start.c](https://sourceware.org/git/?p=glibc.git;a=blob;f=csu/libc-start.c;hb=refs/tags/glibc-2.39#l264)).
  musl's static start only parses auxv and sets up TLS and the canary
  ([__libc_start_main.c](https://git.musl-libc.org/cgit/musl/tree/src/env/__libc_start_main.c?h=v1.2.5)).
- **RayoPDF's static-pie binary** (checked locally) carries:
  - 2,257 `R_X86_64_RELATIVE` relocations (54,168 bytes of `.rela.dyn`), which write 12
    distinct pages;
  - 29 `R_X86_64_IRELATIVE` (glibc's string, memory and rounding ifuncs).
  - About 560 of the relative relocations are RayoPDF's own pointer tables (`Cjk::kPredefined`
    144, `kStandardAliases` 72, `kStandardRoles` 57, …). The rest are vtables, glibc's
    `vfprintf` jump tables (448), the libstdc++ demangler tables (206) and the C locale.
  - The binary also links libstdc++'s verbose terminate handler, the demangler and the unwinder,
    although it builds with `-fno-exceptions`.
- **DT_RELR** (`-z pack-relative-relocs`; glibc 2.36, binutils 2.38;
  [MaskRay](https://maskray.me/blog/2021-10-31-relative-relocations-and-relr)).
  - Measured here: the same objects relinked give `.rela.dyn` 54,168 → 0 bytes plus 608 bytes
    of `.relr.dyn`, and the binary 3,515,432 → 3,462,256 bytes (−53 KB).
  - Faults: odl 107 96–98 → 95–96, odl 183 149–151 → 148–150, odl 192 328–329 → 327–328 (11 runs
    each). Markdown is identical on all three.
  - The static binary carries no glibc version need, so there is no run-time requirement; it was
    checked on a toy program and on this relink. RayoMD excluded the flag because its *dynamic*
    build would need glibc 2.36 at run time
    ([performance.md L1932](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/performance.md#L1932)).
- **Static-pie vs static non-PIE.** RayoMD measured static-pie at 4 to 6 more faults and +0.02 to
  +0.04 ms, and kept PIE for ASLR on untrusted input (L951–957). Measured here: an empty program
  shows no fault difference (27–30 either way).
- **RayoMD's own start-up trims.**
  - It stubs the unwinder out of its static build: −107 KB (L1928).
  - It drops `libm.so` from the dynamic build: −10 faults, −0.07 ms (L1014).
  - It reads TrueType tables in place: −28 faults, −60 µs (L1010).
- **[INFERENCE] Pointer tables as offsets.** A char blob plus offsets needs no relocation
  (Drepper, [How To Write Shared Libraries](https://www.akkadia.org/drepper/dsohowto.pdf)
  §2.4.3). Moving RayoPDF's own tables there, and dropping the demangler, would shrink the 11
  copy-on-write RELRO faults (~2 µs each).
- **Prelink** is gone: glibc 2.35 removed its support
  ([NEWS](https://sourceware.org/git/?p=glibc.git;a=blob;f=NEWS;hb=refs/tags/glibc-2.40)).

### 6.2 Text placement (measured)

- **The binary.** `.text` is 2,124,032 bytes, 33 windows of 64 KiB.
- **What one conversion runs.** Every conversion measured faults text 31 times, so it touches 31
  of the 33 windows. Callgrind lists the functions it runs:

  |Document|Functions run|Their size|4 KiB pages|
  |---|---|---|---|
  |odl 107|427|474 KB|237|
  |odl 183|471|512 KB|235|
  |odl 192|438|481 KB|234|

  These are whole function bodies, so the bytes actually executed are fewer.
- **[INFERENCE] Packed contiguously,** that code fits in about 8 windows. The union over a corpus
  is larger, say 10–12 windows. That would save about 20 text faults, ≈ 60 µs at the indicative
  3.2 µs, or ≈ 5 % of the 1.22 ms fresh time, before any i-TLB or L2 effect.
- **Why RayoMD saw less** [INFERENCE]: it measured 3–4 faults, probably because its `.text` is
  smaller and an export touches less of it.
- **Tooling and risk.** The generator (callgrind, then a sorted section list) and the GNU ld
  `INSERT` script are RayoMD's. Placement changes no output byte.
- **Related.** Cold code compiled `-Os` (RayoMD's per-file `-O2`/`-Os` policy) shrinks the
  windows the cold paths share.

## 7. Parallelism with deterministic output

- **MuPDF.** Document access is serial: one thread loads pages and records display lists; other
  threads render them with cloned contexts. Output order is fixed by joining in page order
  ([multi-threaded.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/docs/examples/multi-threaded.c#L205-L299);
  rules in [overview.md](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/docs/reference/c/overview.md#L162-L290)).
  `mutool draw -T` splits *bands within a page*
  ([mudraw.c](https://github.com/ArtifexSoftware/mupdf/blob/1.28.6/source/tools/mudraw.c#L1125-L1149)).
  PyMuPDF: "does not support running on multiple threads"; use processes
  ([recipes-multiprocessing.rst](https://github.com/pymupdf/PyMuPDF/blob/2.1/docs/recipes-multiprocessing.rst)).
- **PDFium.** "None of the PDFium APIs are thread-safe"
  ([fpdfview.h](https://pdfium.googlesource.com/pdfium/+/96b71fa413f9fd9231bdc3e0a09e16ffcc5766f4/public/fpdfview.h#11)).
  LiteParse runs a pool of processes because "PDFium otherwise serializes concurrent parses"
  ([README](https://github.com/run-llama/liteparse/blob/wasm-v2.18.0/README.md)).
- **pdf.js.** One worker per document, with pages handled serially inside it
  ([api.js](https://github.com/mozilla/pdf.js/blob/6619bf0c6fa6f7797e6b109cb6d431b0ab1ed00d/src/display/api.js#L2084-L2206)).
- **pdf_oxide.** Every rayon task opens its own document; results are sorted by page index "to
  guarantee order" ([parallel.rs](https://github.com/yfedoseev/pdf_oxide/blob/08accb2e0be8252309083a33fafc74015269594c/src/parallel.rs#L103-L106)).
- **edgeparse.** Per-page `into_par_iter().map().collect()`
  ([parallel.rs](https://github.com/raphaelmansuy/edgeparse/blob/8d706e0e467984dd890e9a896d46b793d01adc36/crates/edgeparse-core/src/pipeline/parallel.rs#L27-L95)).
- **Docling.** Reassembles pages by page number after a staged, queued pipeline
  ([standard_pdf_pipeline.py](https://github.com/docling-project/docling/blob/1005c8d7d39447ff433ff650fbc49e046ad0aeee/docling/pipeline/standard_pdf_pipeline.py#L1114-L1118)).
- **lopdf.** Keeps small documents (< 16 entries, < 1 MiB) on the calling thread, because the
  first use "spawns the pool" and costs more than it saves
  ([reader.rs](https://github.com/J-F-Liu/lopdf/blob/4eaec16bd176da538d960fad5e06fa0b77f267da/src/reader.rs#L387-L399)).
- **The common pattern:** pure per-page work, shared caches that are read-only or hold pure
  results, and a merge by page index.
- **RayoPDF** already follows this pattern.
  - `--workers=1` against `--workers=4` is in G-bytes.
  - `RunJobs` runs one job on the calling thread (`src/common/batch.cpp` L136–142).
  - [INFERENCE] One step remains for multi-page fresh runs: with more than one worker, the
    calling thread only waits. Letting it take jobs saves one thread start and its stack faults.
  - None of this helps the one-page odl metric or the single-core throughput bar.

## 8. How benchmarks time tools, and what a fair claim states

- **opendataloader-bench** ([`7af1d8f`](https://github.com/opendataloader-project/opendataloader-bench/blob/7af1d8f4d0c09f51ea1a5c6ba5f66e993286d109/src/pdf_parser.py#L60-L81)).
  - One `time.time()` around the whole engine run over 200 PDFs, divided by the document count.
    It is stored as `elapsed_per_doc` and shown as "Speed (s/page)".
  - Setup lands inside the timed region: docling's `DocumentConverter()` with its models,
    `MarkItDown()`, `LiteParse()`.
  - opendataloader is called once per directory, so its JVM start is spread over 200 documents;
    edgeparse is called per file inside one Python process
    ([pdf_parser_opendataloader.py](https://github.com/opendataloader-project/opendataloader-bench/blob/7af1d8f4d0c09f51ea1a5c6ba5f66e993286d109/src/pdf_parser_opendataloader.py)).
  - Published (Apple M4): nutrient 0.0083 s, opendataloader 0.015 s, edgeparse 0.036 s, docling
    0.76 s.
- **py-pdf/benchmarks** take the minimum of 3 runs, warm Tika's JVM first, and normalise to a
  reference machine by a pure-Python calibration ("works best for pure-Python libraries")
  ([benchmark.py](https://github.com/py-pdf/benchmarks/blob/18ee8c4e38a5627e9f2e56995e4bd57d0f2e4779/benchmark.py#L56-L88)).
- **pdf_oxide's documents and code disagree.**
  - The docs describe one process per PDF with no warm-up; the in-repo script loops in one
    process ([benchmark_all_libraries.py](https://github.com/yfedoseev/pdf_oxide/blob/08accb2e0be8252309083a33fafc74015269594c/scripts/benchmark_all_libraries.py#L70-L80)).
  - The Rust bench is warm and repeats page 0
    ([rust_bench.rs](https://github.com/yfedoseev/pdf_oxide/blob/08accb2e0be8252309083a33fafc74015269594c/bench/rust_bench.rs#L36-L65)).
  - Kreuzberg's author disputed the comparison over single samples, different tasks per library
    and missing raw data ([issue #153](https://github.com/yfedoseev/pdf_oxide/issues/153)).
- **Kreuzberg/Xberg harness.**
  - It defaults to `cold_process` (a fresh process per call, OS cache warm).
  - Warm in-process numbers are opt-in and kept out of cross-tool rankings.
  - Pages/s uses the harness's own page count
    ([benchmark-harness README](https://github.com/kreuzberg-dev/kreuzberg/blob/bb57c5e84167e9bcf35cdd1615a3aa109940a443/tools/benchmark-harness/README.md#L281-L332)).
- **marker v2.1.0** reports "sustained concurrent pages/sec on one B200 host … not single-stream
  latency" ([README](https://github.com/datalab-to/marker/blob/v2.1.0/README.md)).
- **hyperfine** runs commands without a shell by default, to avoid shell noise "especially for
  fast commands (< 5 ms)", and has `--warmup` and `--prepare` for cache control
  ([README](https://github.com/sharkdp/hyperfine/blob/v2.0.0-alpha.2/README.md#L50-L138)).
  RayoPDF's native `posix_spawn` timer avoids Python in the timed path for the same reason.

[INFERENCE] **A fair RayoPDF speed claim states:**
- the CPU and its pinning, the threads per document, and whether the competitors had the same;
- the binary (static or dynamic) and its flags;
- the regime: fresh process per document, or warm in-process with discarded warm-ups;
- what is timed: process start, model or JVM initialisation, file read, output write;
- the corpus at a commit, with document and page counts;
- the unit (per document or per page) and the statistic (median, p95, mean, or pages/s over the
  sum);
- the competitors' versions and modes, with the quality score at the same settings.

Against odl-bench's column the like-for-like RayoPDF number is the fresh per-document time,
1.22 ms. The 0.42 ms warm median is labelled warm. Its harness's competitor numbers in this
repository (`docs/benchmarks/`) were measured with the regimes listed in
`docs/development/evaluation.md`.

## 9. Candidates for RayoPDF, ranked

Ranked by expected gain against the release bar (fresh ≤ 1 ms, warm ≥ 2,000 pages/s) per unit of
effort and risk. "Fresh" gains are per median one-page document unless said otherwise.

|#|Candidate|Expected gain (sourced or [INFERENCE])|Effort|Risk: determinism / portability / licence|
|---|---|---|---|---|
|1|**No copy-out and no copying regrowth of page content**: decode each `/Contents` piece onto the end of one buffer reserved once (PDFium copies the decoded pieces once), or lex the pieces in sequence without concatenating; keep large decode buffers per thread|Measured: odl 141 has 2,036 `memmove` faults out of 2,536 (1,655 in `PageContent`'s appends, 381 in Inflate's copy-out). [INFERENCE] −630 (one reserved buffer) to −1,900 faults (no concatenation) and several MB of copying on such pages; ≈ 1.3–4 ms on odl 141 at the indicative 2 µs per fault; moves the mean and p95, not the median|Low (reserve) to medium (lexer across pieces)|Same bytes if a piece's end acts as the `\n` inserted today (ISO 32000-1 Table 30: pieces split only between tokens). Portable. Own code|
|2|**Hot-section ordering** (RayoMD `hot.ld` method, regenerated from fresh-run callgrind over the corpora)|Measured: 31 of 33 text windows faulted; executed functions ≈ 0.5 MB. [INFERENCE] ≈ −20 faults, ≈ −60 µs fresh (≈ 5 %); RayoMD measured −3 to −4 faults, −7 to −19 µs on a smaller binary|Medium (port the script, regenerate after large changes)|No output change. GNU ld only (not gold or lld); Linux only. A stale list costs warm cycles (RayoMD +0.7 to +1.2 %), so G-warm must catch it|
|3|**Fewer heap pages on heavy pages**: reserve glyph, word, shape and ruling vectors from content-size estimates, or grow them in chunks; consider a narrower `Glyph` (56 bytes, two boxes)|Measured: odl 192 has 111 `vector<Glyph>` faults and 52 in ShowString of 328 in total. [INFERENCE] −50 to −100 faults on p95 pages (≈ 0.1–0.2 ms fresh); warm gains from fewer bytes streamed, cf. Zig's −22 % parser time from compact SoA tokens|Low (reserve) to high (SoA)|Same bytes if only storage changes. Over-reserving raises RSS (reported per document)|
|4|**GCC PGO** (`-fprofile-use`, training on the synthetic, odl and olmOCR corpora)|Published: SPEC int +2 %, rustc −10 to −16 % wall, serde_json +18 to +34 %, Clang up to 20 %. [INFERENCE] 5–15 % warm; unmeasured here|Medium–high: two-stage build on Linux and MinGW, profile upkeep|Reproducible only with a constant training input and serial training; the corpora are not committed (AGENTS.md rule 1), so pin the profile by hash or generate the corpus deterministically. Untrained paths are optimised for size unless `-fprofile-partial-training`. G-bytes on every build. GCC licence exception covers `libgcov`|
|5|**DT_RELR** (`-Wl,-z,pack-relative-relocs`, Linux static build)|Measured: −53 KB, −1 to −2 faults, Markdown identical. [INFERENCE] ≤ 10 µs|Trivial|No run-time glibc requirement for the static binary (checked). Not for the dynamic release build on older glibc, nor applicable to PE|
|6|**Fewer relocated pointer tables and no unwinder or demangler** (offset tables, RayoMD's `no_unwind` stubs)|Measured: 11 RELRO copy-on-write faults, about 560 of RayoPDF's own relocations, 206 of the demangler's. [INFERENCE] a few faults and µs; −100 KB (RayoMD −107 KB)|Low–medium|Stubs depend on glibc internals (RayoMD hit a duplicate `_Unwind_Resume`); output unchanged|
|7|**`mallopt(M_MMAP_THRESHOLD)` raised** in one-shot `--convert` only|Measured: −4.4 % faults over odl, p95 324 → 279. [INFERENCE] ≈ 0.1 ms on p95 pages fresh|Trivial|Keeps freed memory in long runs, so not in `--batch`/`--serve` (RayoMD's reason). glibc only|
|8|**LTO** (`-flto`, with `-frandom-seed` fixed)|Published: SPEC int about +4 %, rustc −4 % instructions. [INFERENCE] 0–5 %; stops at glibc|Low|Deterministic for fixed inputs [INFERENCE]; breaks name-based `hot.ld`, so generate the list after LTO; MinGW LTO untested here|
|9|**Branch-free short-number parse** (one 4-byte load, a multiply, a SWAR digit check) with padded content buffers|Published: 2× the naive loop on random lengths, +15 % on predictable ones. RayoPDF's 2-digit and 8-digit variants lost (+2.9 %, +7.0 %). [INFERENCE] ≤ 1–2 %, uncertain|Low|Needs 4 readable bytes past every buffer (padding). Deterministic; portable|
|10|**Inflate: BMI2 decode loop behind a function pointer; `table_bits` capped at the longest code**|[INFERENCE] ≤ 1.3 % total (the whole 1.15× gap to libdeflate on a ~10 % share)|Low–medium|No FMA in integer code; dispatch via function pointer works on MinGW. libdeflate is MIT if consulted|
|11|**Paged advance and Unicode caches** (MuPDF design); **cross-document font cache** for `--batch`/`--serve` (pdf_oxide)|[INFERENCE] part of glyph placement (~10 %) and font loading (~10 %); the cache helps warm and batch only|Medium|Cache hits must equal misses byte for byte; bound the memory. MuPDF is AGPL: take the design, not code|
|12|**Calling thread as a worker** in multi-page runs|[INFERENCE] one thread start and its stack faults per multi-page fresh run; nothing on odl|Low|Ordered output is already enforced|
|13|`-march=x86-64-v3` binary or `target_clones`|Published: about 1 % typical (Ubuntu)|Low (second binary) / not possible on MinGW (`target_clones`)|FMA contraction can change Markdown: needs `-ffp-contract=off`; drops pre-Haswell CPUs. Not recommended|
|14|Lines built as glyphs arrive (MuPDF stext)|[INFERENCE] part of BuildLines' 15 %|High|Changes output, so a quality project, not a byte-identical step|
|15|SIMD structural indexing of content streams|[INFERENCE] ≈ 0 or negative (tokens of 1–5 bytes; §2.2)|High|Rejected|
|16|BOLT / AutoFDO / Propeller|Published: up to 20 % over FDO+LTO on large binaries|High|Zen 2 has no branch records; static-pie support unconfirmed; ELF only; a binary rewrite in the release pipeline. Rejected for now|
|17|Huge pages (`MADV_HUGEPAGE`, `glibc.malloc.hugetlb`), pre-faulting (`MAP_POPULATE`, `MADV_POPULATE_READ`), static non-PIE|Measured: THP 0.2 ms per 2 MiB fault; pre-faulting no cheaper per page, and populating the whole text costs more than faulting it; non-PIE −4 to −6 faults (RayoMD)|Trivial|Rejected: slower for a 1 ms process, or loses ASLR on untrusted PDFs|

## 10. Measurements made for this note

- **Machine.** AMD Ryzen 5 PRO 4650GE (Zen 2), Linux 7.0.0-38-generic, THP `madvise`,
  `init_on_alloc` on.
  - All runs used CPU 7, never CPU 3. Other work was running (load average 3–12), so the times
    in §5.3 are indicative; the fault counts are the main result.
  - Build image `rayopdf-build:2`: GCC 13.3.0, GNU ld 2.42, glibc 2.39. Profiling image
    `rayopdf-perf:1`: perf 6.8, valgrind.
- **Binaries.** `build/static/rayopdf` of `163408f`; no source file was newer than it.
  - Relinked from the same objects with the link command in `build/static/build.ninja`
    (`-O3 -DNDEBUG -static-pie -Wl,--gc-sections`): byte-identical to `build/static/rayopdf`.
  - The same link plus `-Wl,-z,pack-relative-relocs`, for DT_RELR.
- **Fault counts.** A 20-line C spawner: `posix_spawn`, `waitpid`, and the difference in
  `getrusage(RUSAGE_CHILDREN).ru_minflt`.
  - Runs: `rayopdf --convert DOC /tmp/x.md --images=none`, 11 runs per document for §6.1, 5 for
    the tunables, 1 per document over the 200 odl PDFs.
  - Tunables through `GLIBC_TUNABLES`.
- **Fault attribution.**
  - `perf record -e page-faults -c 1 -d`, then `perf script --show-mmap-events -F ip,sym,addr`.
  - Each fault address was matched to its mapping and each faulting `ip` to a symbol with
    `nm -C`.
  - odl 141 again with `perf record -g`: the first RayoPDF frame of each faulting stack.
- **Executed code.** `valgrind --tool=callgrind` per document. The function names in the output
  were matched to `nm -C -S` sizes and addresses, giving distinct 4 KiB pages and 64 KiB windows.
- **Fault costs.** Small static C programs:
  - anonymous: touch 4,096 pages of a fresh mapping, best of 7, with plain, `MAP_POPULATE` and
    `MADV_HUGEPAGE` (2 MiB-aligned) mappings, then a second touch;
  - file-backed: map the binary's 2 MiB text range from the page cache and touch one byte per
    64 KiB, one byte per 4 KiB, or call `MADV_POPULATE_READ`; best of 15.
- **Local inspection.** `readelf -SrdlW` for sections, relocations and segments.
  - Relocation owners came from the `nm -S` ranges.
  - `target_clones` was tried on MinGW GCC 13 and static-pie GCC 13 with toy programs.

## Open verification items

- Fault costs on the quiet timing CPU (CPU 3). The §5.3 values come from a loaded CPU 7.
- Hot-section ordering's real fault and time change on RayoPDF: the 8–12-window estimate assumes
  whole functions and a corpus union not yet computed.
- PGO's warm gain on RayoPDF, and whether `-fprofile-use` output is byte-identical given
  identical `.gcda` files. The GCC docs do not state the latter; [INFERENCE] yes.
- BOLT support for x86-64 static-pie inputs; BOLT gains without branch records on Zen 2.
- Whether `target_clones` works on MinGW in GCC 14 or 15.
- CPU and harness behind pdf_oxide's 0.8 ms claim, and behind PyMuPDF's Appendix 4.
- Published text-extraction timings for PDFium, Poppler and hayro; none were found.
- An isolated published gain for libdeflate's BMI2 path.
