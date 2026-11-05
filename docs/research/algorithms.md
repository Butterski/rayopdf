# rayopdf — algorithms for a native C++ PDF→Markdown converter

Research note, 2026-11-05. Third-party numbers are as published unless marked reproduced; reproduced numbers are in `docs/benchmarks/`.

Research note, 2026-11-05. Every claim links to a spec section, a source file or a paper. Tags:
- `[INFERENCE]`: my own design reasoning, with no measurement behind it.
- `[unverified]`: I did not read the source for it in this session.

Spec section numbers are ISO 32000-1:2008 ([iso1]) unless marked "-2" ([iso2]; section numbers confirmed through the PDF Association errata page [iso2-errata]).
Context: the earlier RayoMD decision required a separate package, explicit uncertainty, a quality corpus, and evidence on size, latency, RSS and security ([rayomd/docs/development/arbitrary_pdf_research_decision.md](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/docs/development/arbitrary_pdf_research_decision.md), "Consequences").

---

## 1. File structure & robustness

### 1.1 Structure
- **Header** `%PDF-1.N` / `%PDF-2.0` (§7.5.2). The catalog `/Version` can override it. Real files have junk before the header, so search a prefix instead of requiring offset 0 (pdf.js and Acrobat both tolerate this `[unverified]`).
- **Classic xref** (§7.5.4): subsections `first count`, then 20-byte entries `oooooooooo ggggg n|f` + 2-byte EOL. Writers emit 19/21-byte variants, so parse each entry by tokens, not by fixed stride.
- **Trailer/startxref** (§7.5.5): read the tail and find the *last* `startxref`. Trailer keys: `Root`, `Info`, `ID` (never encrypted, §7.5.5 Table 15), `Encrypt`, `Prev`, `Size`, and `XRefStm` for hybrids.
- **Incremental updates** (§7.5.6): follow the `/Prev` chain; the newest section wins per object number.
  - pdf.js keeps a set of already-parsed offsets to stop `/Prev` cycles ([pdfjs-xref] `readXRef`, lines 735–821).
  - pdf.js also accepts the illegal `/Prev N 0 R` (lines 803–806).
- **Xref streams** (§7.5.8): `/W [t f2 f3]`, `/Index`, entry types 0 (free), 1 (offset), 2 (in object stream `objstm#, index`). Unknown types are null (§7.5.8.3).
  - The stream is never encrypted and must not use a Crypt filter (§7.5.8.2).
  - Usually Flate + PNG predictor 12.
- **Object streams** (§7.5.7): `/N`, `/First`, `/Extends`; the header holds `N` pairs `objnum offset`.
  - They never contain streams, objects with gen ≠ 0, the Encrypt dictionary, or their own `/Length` object.
- **Hybrid files** (§7.5.8.4): a classic table plus `/XRefStm` in the trailer holding the "hidden" compressed objects. pdf.js queues `XRefStm` offsets into the same walk ([pdfjs-xref] lines 774–775).
- **Linearization** (Annex F): needs no special handling. The first-page xref and the main xref are linked by `/Prev`, so the normal chain resolves them. Ignore hint streams.

### 1.2 Repair
- pdf.js falls back to `indexObjects()` ([pdfjs-xref] line 432 onward) when the xref parse throws. It does a linear scan for `N G obj` and `trailer`, finds xref streams and replays them, and handles a missing `endobj` (an `obj` found inside an `obj`) and a missing `startxref` (warnings at lines 569 and 607).
- pdf_oxide's main speedup was a one-pass scan for object offsets kept in a hash map. It had been rescanning the whole file per missing object, which is O(n²) on tagged files whose structure elements are absent from the xref ([oxide-perf] "Scan-for-Object Offset Cache").
- How often this matters: the Kumar & Wang 2024 corpus pipeline opened only 30K of 35K scholarly PDFs with pypdf ([kumar] §3). Repair is a core path, not an edge case.
- Wrong `/Length`: if `endstream` is not at `start+Length`, search forward for `endstream` and use that end (pdf.js and MuPDF do this `[unverified]`).

### 1.3 Filters (§7.4)
| Filter | Must decode | Notes |
|---|---|---|
| FlateDecode | yes | `/DecodeParms`: Predictor 2 = TIFF, 10–15 = PNG with a per-row filter byte, Colors/BPC/Columns (§7.4.4.4). `/DL` (decoded length) is optional, so the output size is usually **unknown**. |
| LZWDecode | yes | `EarlyChange` defaults to 1 (§7.4.4.2 `[unverified exact §]`). Same predictors. |
| ASCII85Decode | yes | `z` = 4 zero bytes, `~>` ends the data, partial final group. |
| ASCIIHexDecode | yes | `>` ends the data; an odd final digit gets a trailing 0. |
| RunLengthDecode | yes | Length byte 128 ends the data. |
| Crypt | yes | Selects a crypt filter by name (§7.4.10, §7.6.5). |
| DCT, JPX, JBIG2, CCITTFax | **pass through** | Only to write image files or placeholders. |
| BrotliDecode | report | PDF Association extension EXTN-BROTLI-1 v1.3 (2025) for PDF 2.0 ([brotli]). Expect it in new files. |

Flate robustness matters more than raw throughput:
- **libdeflate**: on failure "the contents of the output buffer are undefined" ([libdeflate-h] lines 215–217). With an unknown size you must guess a capacity and retry on `LIBDEFLATE_INSUFFICIENT_SPACE` (lines 231–240). It **cannot salvage truncated streams**, and those are common in damaged PDFs.
- **RayoMD's own inflater** cannot be reused as-is:
  - It requires an exactly known `outputSize`, a valid zlib header and Adler-32, and hands output out in pieces ([rayomd/src/core/inflate.h](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/src/core/inflate.h) lines 5–12 and the `Start`/`Finish` comments).
  - libdeflate is only RayoMD's test oracle; no product links it ([rayomd/CMakeLists.txt](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/CMakeLists.txt) lines 264–271).
- **PNG predictor undo** already exists in SIMD form: `UnfilterPngRow`/`UnfilterPngRows` ([rayomd/src/core/png_filter.h](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/src/core/png_filter.h)). Its comment says bpp 1–4 "here"; PDF predictors can have a larger bpp (Colors×BPC/8).
- **Bomb caps**: PDFium caps Flate output at 1 GiB in total (`kMaxTotalOutSize`, [pdfium-flate] line 59).

### 1.4 Encryption (Standard security handler, §7.6.3; -2 §7.6.4)
- **What is encrypted**: all strings and streams except the trailer `ID`, strings in the Encrypt dictionary, and strings inside already-encrypted streams such as content streams and object streams (§7.6.1). Xref streams are never encrypted (§7.5.8.2).
- **Per-object key, R2–R4** (Algorithm 1, §7.6.2): take MD5 of the file key + objnum (3 bytes, low byte first) + gen (2 bytes), plus `"sAlT"` for AES. Use the first n+5 bytes, at most 16. AES-CBC takes its IV from the first 16 bytes of the data.
- **File key, R2–R4** (Algorithm 2, §7.6.3.3): MD5 of the 32-byte padded password, O, P (4 bytes LE), ID[0], and `FF FF FF FF` when `EncryptMetadata` is false (R≥4). R≥3 adds 50 MD5 rounds. Key length is 5 bytes for R2, otherwise `/Length`/8.
- **Check the user password** with Algorithm 6 (§7.6.3.4): R2 compares RC4(pad), R3+ compares the first 16 bytes of the 20-round RC4 of MD5(pad‖ID0).
- **V4 crypt filters** (§7.6.5): `/CF /StdCF /CFM /V2` (RC4) or `/AESV2` (AES-128); `/StmF`, `/StrF`; `/Identity` means not encrypted.
- **R6 / AES-256** (-2 §7.6.4.3.3 Algorithm 2.A, §7.6.4.3.4 Algorithm 2.B):
  - Hash validation salt / key salt from the 48-byte U and O.
  - 2.B loop: build K1 = 64× (password ‖ K [‖ U for owner]); take AES-128-CBC(key = K[0:16], IV = K[16:32]) of it. The sum of the first 16 bytes mod 3 picks SHA-256/384/512. Run at least 64 rounds until the last byte ≤ round − 32 (the errata page corrects the K0 construction: [iso2-errata]).
  - Then decrypt UE with AES-256-CBC (IV = 0) to get the file key, and check `Perms` with AES-256-ECB.
  - **There is no per-object key derivation for AESV3** `[unverified: I did not re-read the clause]`.
- **R5**: the deprecated Adobe extension level 3; a single SHA-256 instead of 2.B. Support it; files exist `[unverified prevalence]`.
- **Text extraction only needs the user password**, and most "protected" PDFs use an empty one. Try `""` first, then a password passed on the command line. Ignore the `/P` permissions: extraction is reading, and the decision belongs to the user, as with pdf.js and poppler `[INFERENCE: policy]`.
- **Primitives**: MD5, RC4, AES-128/256 (CBC/ECB decrypt), SHA-256/384/512. RayoMD has SHA-256 with SHA-NI, but its public API returns hex ([rayomd/src/core/rayomd_pdf_source.h](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/src/core/rayomd_pdf_source.h), `Sha256Hex`). R6 needs raw digests and SHA-384/512.
- The public-key handler (§7.6.4) is out of scope: report it.

### 1.5 Hostile-input limits (precedents)
- PDFium:
  - syntax recursion depth 64 (`kParserMaxRecursionDepth`, [pdfium-syn] line 97);
  - form XObject nesting at most 40, plus a stream-revisit check (`kMaxFormLevel`, [pdfium-csp] lines 54 and 1643–1645);
  - Flate output 1 GiB ([pdfium-flate] line 59).
- pdf.js: detects circular references during resolution and returns `CIRCULAR_REF` ([pdfjs-xref] lines 887–888).
- PDFium is not thread-safe across documents at all ([pypdfium2-thread]). MuPDF allows one thread per document plus cloned contexts for display-list work ([mupdf-mt]).

**Recommendation for rayopdf**
1. **Reader structure.**
   - mmap'd, read-only file view; xref = flat vector indexed by object number, filled lazily per section.
   - Resolve the `/Prev` chain newest-first, with a visited-offset set.
   - Merge hybrid `XRefStm` sections into the same update section.
   - Treat linearization as a normal file.
2. **Repair** whenever the xref parse fails or an entry does not point at `N G obj`: run a single-pass scan, with SIMD `memchr` for `obj`/`endobj`/`trailer`/`stream`, into one offset table. Never rescan per object. Report "repaired" in the output's uncertainty header.
3. **Flate**: an rayopdf-owned streaming inflater, forked from `RayoMd::Inflate` (Apache-2.0, same author) with three changes:
   - unknown output size with geometric growth;
   - salvage mode that returns the bytes decoded before an error or truncation, and accepts a missing or bad Adler and zlib header;
   - hard caps: per stream min(256 MiB, 1000× compressed size) and a per-document decoded budget `[INFERENCE: thresholds; PDFium uses a flat 1 GiB]`.
   - Reuse `UnfilterPngRow` for predictors, generalised to any bpp.
   - libdeflate stays a test oracle only, as in RayoMD; it cannot salvage.
4. **Implement** LZW, A85, AHx, RL and the crypt filters. Pass image filters through. Recognise `BrotliDecode` and report it as unsupported until a decoder is justified.
5. **Encryption**: Standard handler R2–R6, `""` user password first, own MD5/RC4/AES/SHA-2 with test vectors from the spec; permissions ignored.
6. **Limits**: object nesting 64, form depth 40 with a cycle set, page-tree cycle set, q/Q stack 256 `[INFERENCE]`, operand stack 64 `[INFERENCE]`, per-page operator budget plus a per-document wall-clock budget. Fuzz every parser entry point, matching RayoMD's practice.

---

## 2. Content streams

### 2.1 Tokenizer
- Lexical rules (§7.2):
  - whitespace is NUL, HT, LF, FF, CR, SP;
  - delimiters are `( ) < > [ ] { } / %`;
  - literal strings have balanced parens, `\ddd` octal and `\`-EOL continuation;
  - hex strings ignore whitespace;
  - names use `#xx`.
- **Numbers** (§7.3.3): writers "shall not use … non-decimal radices … or exponential format". A small fixed-format decimal parser is therefore enough, with no `strtod`. Tolerate junk like `4.-.002` or `--5` the way pdf.js does `[unverified]`.
- `/Contents` can be an array of streams parsed as one concatenation; insert a whitespace between parts (§7.8.2 `[unverified exact wording]`).
- Unknown operators are skipped with their operands. Inside `BX … EX`, unknown operators are explicitly legal (§7.8.2 `[unverified §]`).

### 2.2 Operators that matter (Annex A)
| Group | Operators | Use |
|---|---|---|
| Text objects | BT ET | Reset Tm and Tlm. |
| Text state | Tc Tw Tz TL Tf Tr Ts | Tr 3 = invisible (OCR layers), Tr 7 = clip only, Tr 1/2 = stroke, i.e. synthetic bold (§9.3.6). |
| Positioning | Td TD Tm T* | `TD` also sets TL = −ty. |
| Showing | Tj TJ ' " | `'` = T* Tj; `"` = set Tw, Tc, then `'`. |
| Graphics state | q Q cm gs | `gs` can set `/Font`, `/ca`, `/CA` (alpha 0 = invisible) and `/SMask`. |
| Colour | g G rg RG k K cs CS sc scn SC SCN | Needed for white-on-white and background-fill tests. |
| XObjects | Do | Form: push Matrix, clip to BBox, use the form's Resources falling back to the parent's; Image: record bbox. |
| Marked content | BMC BDC EMC MP DP | `/ActualText`, `/MCID`, `/Artifact`, `/OC` (optional content, §8.11). |
| Paths | m l c v y h re; S s f F f* B B* b b* n; W W* | Rulings, cell borders, rule lines, background boxes, drawn bullets. |
| Shading | sh | Background fills (code-block or table bands). |
| Inline images | BI ID EI | See 2.4. |
| Type 3 | d0 d1 | Only inside CharProcs; never page text. |

### 2.3 Text space → device space and advances (§9.4.4)
- `Trm = [Tfs·Th 0 0; 0 Tfs 0; 0 Trise 1] × Tm × CTM`, recomputed per glyph.
- Advance (horizontal): `tx = ((w0 − Tj/1000)·Tfs + Tc + Tw)·Th`, then `Tm = [1 0 0; 0 1 0; tx 0 1] × Tm`.
  - Vertical writing uses `ty = (w1 − Tj/1000)·Tfs + Tc + Tw`.
  - `Tw` applies only to the single-byte code 32 (§9.3.3).
- `w0` is in glyph space/1000:
  - simple fonts: `Widths[code − FirstChar]`, else `FontDescriptor/MissingWidth`, else standard-14 AFM metrics;
  - CID fonts: `W` / `DW` (default 1000) (§9.7.4.3);
  - Type 3: `Widths × FontMatrix`, not /1000 (§9.6.5).
- **TJ numbers** are the strongest word-gap signal: large negative adjustments are inter-word gaps (see §4).
- **Glyph box**: x-extent from origin to origin + advance; y-extent from `FontDescriptor` `Descent`/`Ascent` (or FontBBox) scaled by Tfs, shifted by Trise. Use a fixed ±0.8/−0.2 em fallback when the descriptor is missing `[INFERENCE]`.

### 2.4 Inline-image pitfalls (§8.9.7)
- The data after `ID` is binary, and `EI` may appear inside it. pdf.js ([pdfjs-parser]):
  - Uses the filter's own end marker when it has one: DCT EOI, A85 `~>`, AHx `>` (lines 343–466).
  - Otherwise scans for `EI` + whitespace and accepts it only if the following bytes look like ASCII operators that the lexer knows (lines 199–335).
  - Falls back to the last `EI` seen (lines 316–323).
- Also: a 1-byte off-by-one when the data is followed directly by `EI` (lines 326–335).
- Abbreviated keys/filters (`/W /H /BPC /CS /F /DP`; `AHx A85 LZW Fl RL CCF DCT`) (§8.9.7 Tables 92–93).
- PDF 2.0 adds a length key for inline-image data `[unverified: the -2 text fetch timed out]`. Use it when present.

### 2.5 Marked content and optional content
- Store a stack of BDC property lists:
  - `/Span <</ActualText …>>` replaces the enclosed glyphs' text (§14.9.4);
  - `/P <</MCID n>>` links content to the structure tree (§14.7.4.2);
  - `/Artifact <</Type /Pagination /Subtype /Header|/Footer…>>` (§14.8.2.2.2 Table 330);
  - `/OC /oc1` points at an OCG that is OFF in `/OCProperties /D`, so hidden.
- Clipping objects inside marked content count as part of the sequence (§14.6.3).

**Recommendation for rayopdf**
- A zero-copy tokenizer over the decoded buffer:
  - a 256-entry byte-class table;
  - an operand stack of tagged 16-byte values with string views into the buffer (no allocation);
  - operators recognised by a perfect hash on ≤3 bytes;
  - a custom fixed-format number parser.
- One "interpreter" pass per page emits an **event list** into a per-page arena: glyph runs (with font, size, Trm, colour, alpha, Tr, MCID stack top, ActualText id, OC visibility, clip box), axis-aligned line and rect segments (with stroke/fill and colour), image boxes, and shading boxes.
- Track the clip as an axis-aligned box only (an intersection of rects); exact path clipping is not needed for text `[INFERENCE]`.
- Follow pdf.js's inline-image EI logic exactly.
- Never extract text from Type 3 CharProcs or annotation appearances, except as an option for form field values.

---

## 3. Fonts & Unicode

### 3.1 Priority order (spec + engines)
- **Spec** (§9.10.2):
  1. ToUnicode;
  2. a simple font with MacRoman/MacExpert/WinAnsi or Differences drawn from the standard Latin/Symbol names → code→name→AGL;
  3. a composite font with a predefined CMap other than Identity-H/V, or the Adobe-GB1/CNS1/Japan1/Korea1 collections → CID → `Registry-Ordering-UCS2`.
  Otherwise the result is "no way to determine".
- **pdf.js** ([pdfjs-eval] `buildToUnicode`, lines 3923–4006):
  - If a ToUnicode is present, it *also* builds a fallback map from the encoding for simple fonts, for incomplete ToUnicode maps (issue 8229).
  - For simple fonts it **always** builds code→name→AGL, not only for the spec's three encodings.
  - For composite fonts: the CJK `-UCS2` path; else an identity map.
  - Name heuristics (lines 3841–3901), which pdf.js notes "Adobe Reader does not do … OSX Preview does":
    - `Gxx` (hex) and `g00xx`;
    - `Cdd`/`cdd`, decimal, re-parsed as hex if any name fails decimal;
    - `uniXXXX`/`uXXXX[XX]`;
    - `f_h`, `f_t`, `T_h` ligatures;
    - `.notdef` skipped in Differences.
- **PDFBox** ([pdfbox-simple] `toUnicode`, lines 279–330): ToUnicode → encoding name → glyph list.
  - With no mapping, simple fonts get "Acrobat simply coerces the character code into Unicode". Composite fonts are **skipped**, as Acrobat does ([pdfbox-legacy] lines 278–297).
  - With no `/Encoding` it reads the font program's built-in encoding ([pdfbox-simple] line 107).
- **MuPDF** ([mupdf-unicode]):
  - Composes ucs_from_cpt through the font's code→gid CMap (`pdf_remap_cmap`), with an identity shortcut.
  - Uses the CJK `*-UCS2` system CMaps for the four Adobe collections.
  - Simple fonts: glyph-name table → `fz_unicode_from_glyph_name`, else U+FFFD.
  - "synthesize a ToUnicode … from freetype cmap/post" is a TODO.
  - stext options choose the CID or GID when Unicode fails (`use-cid/gid-for-unknown-unicode`, [mupdf-opts]).
- **pdfminer** ([pdfminer-font] lines 1123–1144): for `Adobe-Identity` with no ToUnicode it **inverts the embedded TrueType cmap** (gid→char; `create_unicode_map`, lines 740–779). Otherwise it uses its CMapDB unicode map for the collection.

### 3.2 ToUnicode CMap parsing (§9.10.3, §9.7.5; Adobe TN #5014, #5411)
- Support: `begincodespacerange` (1–4-byte ranges; simple fonts are 1 byte), `beginbfchar`, `beginbfrange`, `usecmap`.
- Destinations are UTF-16BE; decode surrogate pairs (the spec example `<3A51> <D840DC3E>` → U+2003E).
- bfrange with a string destination: increment the **last** byte for each code.
- bfrange with an array destination: one string per code (the spec example ff/fi/ffl ligatures).
- Also accept `begincidrange`/`cidchar` in mislabelled ToUnicode streams, as pdf.js does `[unverified]`.
- Tolerate missing `endcmap`, odd hex lengths and dst strings over 2 bytes on 1-byte codes.
- Bound everything: at most 65 536 bfrange iterations per range `[INFERENCE]`, total entries ≤ 2^20.

### 3.3 Simple-font encodings
- Base encodings: Standard, WinAnsi, MacRoman, MacExpert (Annex D.2), Symbol and ZapfDingbats built-in (D.5/D.6).
- `/Differences` overrides the base (§9.6.6.1).
- **PDFDocEncoding** (D.3) is for *text strings* (outlines, Alt, ActualText, Info), not glyphs. Text strings can also be UTF-16BE with a `FE FF` BOM, or UTF-8 with an `EF BB BF` BOM in PDF 2.0 (-2 §7.9.2.2 `[unverified §]`).
- **Base encoding when `/Encoding` is missing or only has Differences**:
  - nonsymbolic → StandardEncoding;
  - symbolic or embedded → the font program's **built-in encoding** (§9.6.6.2);
  - Type1: the `dup <code> /<name> put` lines in the clear-text part of FontFile;
  - CFF/Type1C: the Encoding and charset tables.
  - RayoMD's CFF reader has INDEX, DICT, CharStrings and FDSelect, but `CffFont` holds **no charset/encoding fields** ([rayomd/src/core/cff.h](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/src/core/cff.h), `struct CffFont`). Readers for both must be added.
- **Glyph name → Unicode** ([agl]):
  1. Drop everything from the first `.` (`a.sc`, `T.swash`, `one.oldstyle`).
  2. Split on `_` (ligatures `f_f_i`).
  3. Per component: ZapfDingbats list (only for the ZapfDingbats font), else AGL, else `uniXXXX…` (uppercase hex, groups of four, no surrogates), else `uXXXX[XX]`, else empty.
  - AGL names with PUA values (`Asmall`, superiors) map into the PUA. Normalise those to base letters after mapping `[INFERENCE]`.
- **TrueType** (§9.6.6.4):
  - *Nonsymbolic* + Encoding: code→name→AGL→(3,1) cmap.
  - *Symbolic* with no Encoding: the (3,0) subtable with codes in 0x0000/0xF000/0xF100/0xF200 + byte, else (1,0) direct.
  - For **Unicode**: a (3,0) symbol cmap gives PUA U+F0xx. Map Symbol/Wingdings-family fonts through name-specific tables (Symbol → AGL via the Symbol encoding names); keep other PUA output and flag it `[INFERENCE]`.
  - The `post` table names give an extra source of glyph names.
- **Type 3**: `/Encoding` is required (§9.6.6.3) and its Differences names go through AGL. Type 3 bitmap fonts with names like `g12` or `c5` get pdf.js's G/g/C heuristics.

### 3.4 CID fonts and predefined CMaps (§9.7)
- Type0 `/Encoding`: either a predefined CMap name (§9.7.5.2 Table 118: Identity-H/V, UniGB-UCS2-H, UniJIS-UTF16-H, 90ms-RKSJ-H, …) or an embedded CMap stream.
- Split codes by codespace ranges, choosing the matching byte length (§9.7.6.2). Undefined codes get notdef handling (§9.7.6.3).
- **Unicode for a non-Identity predefined CMap or the Adobe-{GB1,CNS1,Japan1,Korea1} collections**: CID → Unicode.
  - pdf.js and MuPDF ship `Adobe-*-UCS2` CMaps.
  - Adobe's **cmap-resources** repo ships, per collection, `cid2code.txt` (CID → Unicode/legacy codes) and the `Uni*-UTF16/UTF32-H` CMaps ([cmapres] `Adobe-Japan1-7/`).
  - Licence: **BSD-3-Clause** ([cmapres] LICENSE.md), compatible with Apache-2.0 plus NOTICE.
  - Note: cid2code.txt for Japan1 alone is 2.69 MB of text. Generate compact tables at build time.
- **Identity-H/V without ToUnicode**:
  - CIDFontType2 + embedded TrueType: invert the font's (3,1)/(3,10) cmap through CIDToGIDMap (pdfminer approach).
  - CIDFontType0 (CFF): use CFF charset glyph names if the font is name-keyed `[INFERENCE]`.
  - Otherwise: no Unicode; emit U+FFFD and flag the font. Do not emit raw codes; PDFBox and Acrobat skip composite codes.
- Vertical mode (`WMode 1`, `-V` CMaps): `W2`/`DW2` metrics; columns read top-to-bottom, right-to-left (§9.7.4.3).

### 3.5 Widths when absent
- The standard 14 (§9.6.2.2) may omit Widths, so AFM metrics are needed:
  - PDFBox ships the Adobe AFMs ([pdfbox-afm]);
  - pdf.js ships the same widths as an Apache-2.0 JS table ([pdfjs-metrics]), the cleanest licence source for rayopdf.
- Alias names (Arial → Helvetica, TimesNewRoman → Times-Roman, `,Bold` suffixes) as pdf.js `standard_fonts.js` does `[unverified file]`.
- PDFBox quirk: `.notdef` gets width 250 like Acrobat ([pdfbox-simple] lines 346–350).
- **Space width** for gap inference:
  - PDFBox: the font's space width, else 0.8 × average glyph width, else 1.0 ([pdfbox-legacy] lines 250–270);
  - pdf.js: thresholds relative to font size (§4).

### 3.6 Detecting garbage mappings
- Signals `[INFERENCE: thresholds to calibrate on the corpus]`:
  - a large share of mapped code points are controls (< U+0020), PUA, or U+FFFD;
  - an identity ToUnicode on a subset font whose codes are small sequential integers (0x01, 0x02…): typical of subset TrueType with gid-as-code;
  - many distinct codes map to the same Unicode;
  - words fail a character-bigram / script-consistency check (e.g. Latin text with a sudden Cyrillic or Armenian run).
- Cross-check: compare the ToUnicode result with the encoding fallback (pdf.js keeps both: `fallbackToUnicode`). Where they disagree on letters, trust the fallback if it yields dictionary-like text.
- Output policy: a font whose text is judged garbage is reported per page in the uncertainty header ("N glyphs unmapped / suspected wrong ToUnicode"). OCR is out of scope.

**Recommendation for rayopdf**
- Per document, a **font cache** keyed by font dictionary object id. Each entry holds:
  - a code→(Unicode string id, advance, flags) table: a flat array of 256 entries for simple fonts, a two-level sparse table for CID;
  - descriptor flags.
- **Parse font programs lazily, only when needed**: built-in encoding (Type1/CFF without an Encoding), a TrueType cmap/post for Identity-H without ToUnicode, or garbage-mapping repair.
- Resolution order: ToUnicode, with the per-code encoding fallback filling gaps (pdf.js) → encoding + Differences → AGL with suffix/ligature rules + pdf.js name heuristics → CJK cid2code tables → inverted TrueType cmap → U+FFFD plus a flag.
- Ship AGL + ZapfDingbats lists, the four base encodings plus Symbol/Dingbats, standard-14 widths (from pdf.js), and generated CJK CID→Unicode tables for the five Adobe collections. Measure binary size first; consider an optional "cjk" table blob if it is large `[INFERENCE]`.

---

## 4. Glyphs → words → lines

### 4.1 Space inference — reference thresholds
| Engine | Rule (source) |
|---|---|
| pdfminer | Same line if vertical overlap > `line_overlap`(0.5) × min height **and** horizontal distance < `char_margin`(2.0) × max width. Insert a space if the gap > `word_margin`(0.1) × max(w, h) of the char. Lines join a box at `line_margin` 0.5 ([pdfminer-layout] lines 76–80, 488–492, 695–700). |
| PDFBox | Expected next-word x = end + min(spaceWidth × `spacingTolerance`(0.5), avgCharWidth × `averageCharTolerance`(0.3)). Same-line test is a y-overlap with 0.1 tolerance. Paragraph rules: `indentThreshold` 2.0 and `dropThreshold` 2.5 ([pdfbox-strip] lines 72–73, 156–157, 595–633, 760–763). |
| pdf.js | gap ≤ 0.102·fs = tracking (no space); text spaces ≤ 0.03·fs are dropped; 0.102–0.6·fs = space in flow; > 0.6 or < −0.2·fs = new chunk; vertical shift > 0.25·h = new chunk ([pdfjs-eval] lines 2491–2516). |
| MuPDF | Distances in units of font size along the baseline direction: \|spacing\| < 0.15 = same word; 0.15–0.8 = add a space (two if > 0.3); baseline offset < 0.8 = same line; ≤ 1.5 = new line in the same block, else a new paragraph ([mupdf-stext] lines 85–88, 900–995). Direction change (dot < 0.999) or WMode change = new line. |

Also use the actual **space glyphs in the stream**:
- PDFBox has `ignoreContentStreamSpaceGlyphs` (default false) ([pdfbox-strip] line 150).
- pdf.js drops spaces narrower than 0.03·fs.
- A real U+0020 glyph is evidence; a large TJ gap is evidence; tracking (uniform small Tc) is not.

### 4.2 Overprint, fake bold, duplicates
- PDFBox `suppressDuplicateOverlappingText` (default **true**): drops a char equal to an earlier one within width/len/3 in x and y, unless ActualText applies ([pdfbox-strip] lines 146, 911–934).
- MuPDF: same char at distance < 0.1 em is dropped (`FAKE_BOLD_MAX_DIST`), and a `check_for_fake_bold` style pass marks the survivor bold ([mupdf-stext] lines 89, 836–849, 878–884).
→ Detect duplicates and *mark them bold*, do not just drop them.

### 4.3 Invisible / hidden / clipped text
- Tr 3: invisible. **Keep it** when it is the page's only text and the page has a full-page image (an OCR layer). Otherwise treat it as hidden. Tr 7 is clip only.
- opendataloader-pdf filters, on by default ([odl-safety]):
  - `hidden-text`: transparent, low-contrast or invisible;
  - `off-page`: outside the CropBox;
  - `tiny`: ≤ 1 pt;
  - `hidden-ocg`.
  Motivation: indirect prompt injection via white-on-white text or hidden OCGs.
- MuPDF option `clip`: drop fully clipped text ([mupdf-opts]).
- White text: compare the fill colour against the topmost filled rect, shading or image under the glyph box (the event list from §2 holds paint order). With no fill under it, compare against white paper.

### 4.4 Ligatures, diacritics, hyphenation, scripts
- **Ligatures**:
  - ToUnicode arrays and AGL `f_f_i` already expand them;
  - presentation forms U+FB00–FB06 → NFKC-expand (MuPDF expands unless `preserve-ligatures`, [mupdf-opts]).
- **Combining marks**: MuPDF does not advance the pen for Mn-category characters ([mupdf-stext] lines 862–870). Attach marks to their base by x-overlap, then NFC-compose.
  - Spacing diacritics (`dieresis` drawn as a separate glyph over the letter, typical of TeX) → combine when the mark box is ≥ 50% inside the base x-extent and above or below it `[INFERENCE]`.
- **Dehyphenation**: join `-`/U+00AD at line end with the next line's first word when the next word starts lowercase and the line is inside a paragraph; keep hard hyphens in compounds (MuPDF `dehyphenate`, [mupdf-opts]).
  - ActualText may carry the correct spelling (the spec's German "Druk-ker" → "Drucker" example, §14.9.4).
- **Rotated text**: group by text direction (the Trm baseline vector, as MuPDF does); read 90° and 270° text in its own frame. `/Rotate` on the page rotates the whole frame (§7.7.3.3).
- **Sub/superscript**: a baseline offset with a smaller font (or Ts ≠ 0) stays in the line (MuPDF tolerance 0.8 em). Classify it as sup/sub by offset sign and size ratio < 0.85 `[INFERENCE]`, then emit `<sup>`/`<sub>` or footnote references (§6).
- **RTL/bidi**: PDF often stores RTL runs in visual order (§14.8.2.3.3 "Reverse-Order Show Strings").
  - MuPDF marks lines "visual" (`bidi = 3`) when advance and logical spacing disagree ([mupdf-stext] lines 900–935).
  - PDFBox runs `java.text.Bidi` per word or line ([pdfbox-strip] lines 1909–1913).
  → Sort each line by x, then apply the UAX #9 algorithm ([uax9]) to recover logical order when a strong-RTL majority is detected.

**Recommendation for rayopdf**
- Use MuPDF's **direction-normalised, font-size-relative** model (spacing/size and baseline offset/size along the Trm direction): it handles rotation and is resolution independent.
- Combine it with explicit signals: a stream space glyph, TJ gap ≥ 0.15 em, and gap > 0.5 × space width (PDFBox).
- Single pass in content order, merging into the current line when direction and baseline match. Then a sort-and-merge pass per line cluster, because content order can be arbitrary: re-sort lines by baseline (y) and by x within a line.
- Mark duplicates as bold; drop invisible, clipped, tiny and hidden-OCG text by default, and report what was dropped. Keep OCR layers.

---

## 5. Layout & reading order

### 5.1 Algorithms
- **XY-cut** (Ha et al. 1995; Meunier 2005 optimised version, both cited in [xycutpp] §2.1): recursively split at the widest projection gap.
  - Fails on "L-shaped" inputs and cross-column elements ([xycutpp] Figs. 2–3).
- **XY-Cut++** (Liu, Li, Wei 2025, [xycutpp] §4):
  1. Pre-mask "highly dynamic" elements (titles, figures, tables).
  2. Mark **cross-layout** blocks: length > β·median(lengths), β = 1.3, and horizontal-projection overlap with ≥ 2 other blocks (Eq. 1–2).
  3. Geometric pre-segmentation on isolated central title or figure blocks (Eq. 3).
  4. Density-driven recursion: τd = area(cross-layout)/area(single-layout). If > 0.9, split horizontally first (XY), else vertically first (YX); always cut at the minimum-projection gap (Eq. 4–5).
  5. Re-insert masked elements by label priority (cross-layout ≻ title ≻ vision ≻ others) with a weighted 4-term geometric distance (Eq. 7–14).
  - Results: BLEU-4 98.6 on complex and 98.9 on regular pages of DocBench-100, at 1.06× the FPS of geometric-only methods. The labels come from a detector model (PP-DocLayout).
  - **opendataloader-pdf** implements it without ML labels (`XYCutPlusPlusSorter.java`): β default 2.0, density threshold 0.9, minimum gap 5 pt, cut on the larger gap ([odl-ro]).
- **Breuel** ([breuel] §3, §7):
  - Maximal-whitespace-rectangle cover by branch and bound; keep only "tall" whitespace bounded by enough components on each side → column gutters.
  - Then text lines constrained by the gutters.
  - Then a partial order: (1) a before b if their x-ranges overlap and a is above b; (2) a before b if a is entirely left of b and no line c lies between them in y with an x-range overlapping both.
  - Topological sort gives the total order. No segmentation errors on 221 UW3 pages (73 multi-column).
- **Docstrum** (O'Gorman 1993, [docstrum]): k-nearest-neighbour angle and distance histograms over components → skew, intra- and inter-line spacing. Useful for estimating the spacing parameters per page, not for ordering.
- **pdfminer `boxes_flow`**: hierarchical agglomerative grouping of text boxes.
  - Distance = bbox-union area − both areas; skip pairs with another box between them.
  - Within groups, sort by `(1−bf)·x0 − (1+bf)·(y0+y1)`, bf = 0.5 ([pdfminer-layout] lines 650–657, 787–890).
  - O(n²) heap, which is where pdfminer's slowness comes from.
- **MuPDF**: blocks are formed during extraction (paragraph distance 1.5 em). Options `segment` and `table-hunt` add page segmentation ([mupdf-opts]).

### 5.2 Headers, footers, page numbers
- Tagged files: `/Artifact` with `/Type /Pagination` (`/Subtype /Header|/Footer|/Watermark`) (§14.8.2.2.2 Table 330).
- Untagged files: cross-page repetition `[INFERENCE: method, no source measured]`.
  - Candidates are lines in the top or bottom ~12% band.
  - Key = text with digit runs → `#` and roman numerals normalised, plus a rounded y (2 pt).
  - A key on ≥ 3 pages and ≥ 40% of pages (odd and even separately, for mirrored layouts) is a running header or footer.
  - A lone number (or "Page # of #") in the band whose value increases with the page index is a page number.
- pymupdf4llm and opendataloader both detect headers and footers ([pymupdf4llm]; [odl-ro] Phase 1 treats full-width headers and footers as cross-layout).

**Recommendation for rayopdf**
- Order of preference:
  1. a **valid structure tree** (§6.10);
  2. otherwise a geometry-only XY-Cut++ variant on *text blocks* (not glyphs), with:
     - cross-layout masking (β ≈ 1.3–2.0, to tune on the corpus);
     - pre-masking of tables and figures already found from vector rulings and images;
     - Breuel's two partial-order rules as the **tie-breaker and validator**: if the XY-cut order violates rule 1 (an x-overlapping block above comes later), fall back to the topological order of rule 1 + rule 2.
- Cost is O(n log n) per recursion level on block projections (sorted intervals), so cheap. Avoid pdfminer's O(n²) agglomeration.
- Remove headers, footers and page numbers by repetition **before** ordering.
- Emit an uncertainty flag per page when the column structure is ambiguous: more than one gutter candidate with a similar score, or orders from the two methods disagree.

---

## 6. Semantic structure for Markdown

### 6.1 Headings
- Signals:
  - font size relative to body (body = mode of glyph-weighted font sizes; pymupdf4llm `IdentifyHeaders` maps sizes above body to `#` levels with `body_limit` and `max_levels` = 6, [pymupdf4llm]);
  - weight (§6.6);
  - a block that is short and not ending in a period;
  - space above > space below;
  - numbering patterns (`1.`, `1.2.3`, `A.`, `Chapter`).
- **Outline** (§12.3.3): bookmark titles plus their destinations (page, /XYZ top). Match title text against blocks near the destination y. This gives level by tree depth and is the most reliable untagged signal when present. pymupdf4llm `TocHeaders` takes this route ([pymupdf4llm]).
- Assign levels by clustering distinct heading styles (size, weight, font) ranked by size. Cap at 6. Never skip levels in output (renumber), as RayoMD's own tagger requires ([rayomd/src/core/tagged.h](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/src/core/tagged.h) `BeginHeading`).

### 6.2 Paragraphs
- Breaks:
  - vertical gap > ~1.5× the median line pitch (MuPDF `PARAGRAPH_DIST` 1.5; PDFBox `dropThreshold` 2.5 × height);
  - first-line indent > ~2 char widths (PDFBox `indentThreshold` 2.0);
  - previous line ends well short of the right margin (< 85% of column width) with sentence-final punctuation `[INFERENCE]`.
- Join lines with a space; dehyphenate (§4.4). For justified text, ignore extra inter-word space.

### 6.3 Lists
- Bullets:
  - glyphs •◦▪‣–*·■□✓ and Symbol/Dingbats codes (ZapfDingbats a1..a191 via its list, [agl]);
  - **vector bullets**: small filled circles or squares (`re` or curves) with size ≈ 0.3–0.6 x-height, left of a text line with a hanging indent `[INFERENCE]`. RayoMD itself draws bullets and task checkboxes as paths ([rayomd/src/core/tagged.h](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/src/core/tagged.h), list-label comment).
- Enumerators: PDFBox's list regexes `\.`, `\d+\.`, `\[\d+\]`, `\d+\)`, `[A-Z]\.`, `[a-z]\.`, `[A-Z]\)`, `[a-z]\)`, `[IVXL]+\.`, `[ivxl]+\.` ([pdfbox-strip] lines 1777–1779). Require sequential values across items.
- Nesting from the marker x-position (cluster x into levels). Continuation lines align to the text start (hanging indent).

### 6.4 Code blocks
- Monospace detection: FontDescriptor FixedPitch flag (bit 1, §9.8.2 Table 123); a uniform width array; font name (Courier, Mono, Consolas, Menlo, Code, `*Mono*`).
- Supporting signals: a background fill rect or shading behind the block; a left bar.
- Preserve whitespace by computing leading spaces = (x − block_x0)/space_advance of the monospace font. Emit fenced code blocks. Inline monospace runs become backticks.

### 6.5 Blockquotes
- A left vertical bar (thin filled rect or stroked line spanning the block height) plus an indent. Text indented on both sides without a bar is ambiguous; keep it as a paragraph `[INFERENCE]`.

### 6.6 Emphasis
- Italic: Flags bit 7 or `ItalicAngle ≠ 0` (§9.8 `[ItalicAngle not re-read]`), name tokens (`Italic`, `Oblique`, `-It`), or a shear in Trm (synthetic oblique: Tm b/c ≠ 0).
- Bold:
  - `FontWeight ≥ 600` (FontDescriptor, PDF 1.5) `[unverified table #]`;
  - ForceBold (bit 19) `[unverified: bit from memory]`;
  - name tokens (`Bold`, `Black`, `Heavy`, `Semibold`, `,Bold`), after stripping the `ABCDEF+` subset prefix;
  - **synthetic bold**: Tr 2 with a stroke width > 0, or overprinted duplicates (§4.2).
- StemV relative to the document median is a weak signal.
- Emit `**`/`*` only over whole words, and only when the style differs from the block's dominant style.

### 6.7 Tables
- **Ruled lattice** (pdfplumber, after Nurminen [nurminen], [plumber-readme] lines 354–360):
  - edges from lines and rect sides;
  - snap parallel edges within 3 pt; join collinear segments within 3 pt; drop edges < 3 pt;
  - intersections within 3 pt;
  - minimal rectangles from intersections = cells; contiguous cells = a table ([plumber-table] lines 9–12, 475–487).
  - Camelot lattice does the same on a **rasterised** page with OpenCV morphology ([camelot] "Lattice"). Vector edges are exact and cheaper.
- **Stream (borderless)** (Camelot after Nurminen):
  - rows by y-overlap;
  - "text edges" (aligned left, right or centre word edges) suggest table areas;
  - column count = **mode** of words per row;
  - column x-ranges from the rows that match the mode ([camelot] "Stream").
  - Camelot `network`: an alignment graph, pruned to elements connected on both axes, grown from the most-connected seed.
- **Honest accuracy**: on borderless FinTabNet.c, Camelot's heuristic parsers plateau at about TEDS 0.20; its ML (TATR) backend reaches 0.371, about 2× ([camelot] "Borderless accuracy").
  - The opendataloader bench shows the same gap: table TEDS 0.489 heuristic vs 0.928 hybrid-ML ([odl-bench]).
- **Spanning cells**: in a lattice, a cell whose rectangle covers several minimal grid cells (a missing internal edge) is a span. GFM cannot express spans, so emit an HTML `<table>` with `rowspan`/`colspan`. Use GFM only for rectangular tables with a single header row and no multi-paragraph cells.
- Tagged input: `Table/TR/TH/TD` with `RowSpan`/`ColSpan` attributes (§14.8.5.7 `[unverified §]`).

### 6.8 Links
- Link annotations (§12.5.6.5): `Rect`, `QuadPoints`, `/A` (URI action §12.6.4.7, GoTo) or `/Dest`. Map them to text by glyph-box ∩ annotation-area (QuadPoints when present). §14.8.4.4.2 notes the link↔content association is purely visual unless tagged (`Link` + OBJR).
- GoTo → an internal anchor if the destination resolves to a heading; else drop the link and keep the text.
- Bare URLs in text → autolinks. Rejoin URLs broken across lines (no space at the line end, next line starts with URL characters) `[INFERENCE]`.

### 6.9 Footnotes, images, math
- **Footnotes**: superscript markers in body text (§4.4) matched to small-font blocks at the bottom of the page that start with the same marker → `[^n]` definitions. Tagged files: `Note` (+ `Reference`) (§14.8.4.4 Table 338 `[unverified table #]`).
- **Images**: placeholders `![alt](page-N-img-M.ext)`.
  - Extracting files means passing DCT as-is to `.jpg`, JPX to `.jp2`, and writing Flate raw pixels as PNG via RayoMD's PNG encoder.
  - Alt text from structure `Figure /Alt` (§14.8.4.5) when tagged; else empty alt plus a flag.
  - Ignore images < ~8×8 pt and full-page backgrounds; Type 3 inline images are glyphs, not figures.
- **Math**:
  - Font-family detection: CMMI/CMSY/CMEX/MSAM/MSBM (TeX), `*Math*`, STIX, Cambria Math, Latin Modern Math; plus Unicode Mathematical Alphanumerics.
  - Glyph-level LaTeX reconstruction is a layout-parsing research problem; pure heuristics produce wrong LaTeX `[INFERENCE]`. Emit Unicode math inside `$…$` only for inline sequences that are linear (no stacked fractions or radicals). Emit display math as an image placeholder or Unicode text, with an uncertainty flag.
  - Tagged `Formula` with `/Alt` or `/ActualText`: use it verbatim (LaTeX if the producer stored it).

### 6.10 Tagged PDF as the primary signal
- Model (§14.7–14.8):
  - `StructTreeRoot/K`, depth-first = logical order (§14.8.2.3.1 distinguishes it from content order);
  - kids are elements, MCIDs (page from `/Pg`), `MCR` (with `/Stm` for forms), and `OBJR` (annotations and links);
  - `ParentTree` (a number tree keyed by the page's `/StructParents`, or an XObject's `/StructParent`) maps MCID → element (§14.7.4.4);
  - `RoleMap` maps custom types to standard ones, transitively; detect cycles;
  - `Alt`, `ActualText`, `E`, `Lang` (§14.9).
- Standard types → Markdown: `H, H1–H6`, `P`, `L/LI/Lbl/LBody`, `Table/TR/TH/TD/THead/TBody/TFoot`, `Figure/Caption`, `Formula`, `Code`, `BlockQuote`, `Link`, `Note/Reference`, `TOC/TOCI`, `Span`, `Quote`; PDF 2.0 adds `Title`, `FENote`, `Aside`, `Em`, `Strong`, `Sub` (-2 §14.8.4 `[unverified list]`).
- **How reliable tags are in practice**:
  - Kumar & Wang 2024: 12.6% of 19,997 scholarly PDFs pass Adobe's "Tagged PDF" check; 15.9% pass "appropriate nesting"; 3.2% pass all six checks ([kumar] Table 2).
  - The automated checker misses untagged nested elements and table problems ([kumar] tool comparison).
  - opendataloader makes tag use opt-in (`--use-struct-tree`) and falls back to XY-Cut++ otherwise ([odl-tagged]).
  → Tags are rare, and present tags can be "tag soup" (everything `P`/`Span`, auto-tagged order).
- **Validate before trusting** `[INFERENCE: criteria]`:
  1. ≥ 95% of visible, non-artifact glyphs are reachable from the tree via MCID;
  2. heading and list types are actually used (not only P/Span/Div);
  3. the tree order of MCIDs within a page does not violate Breuel rule 1 for more than ~5% of pairs;
  4. no RoleMap cycles.
  - If a check fails, use heuristic layout but keep tag hints (Alt, ActualText, table cells, artifacts).
- **Free oracle**: RayoMD writes PDF/UA tags for exactly the types Markdown needs: `Document, P, H1–H6, L, LI, Lbl, LBody, BlockQuote, Div, Code, Table, THead, TBody, TR, TH, TD, Figure, Formula, Link, Reference, Note, TOC, TOCI, Caption` ([rayomd/src/core/tagged.h](https://github.com/Butterski/rayomd/blob/ef106cac5d8a7081cda6c02cec0f38d88048cf97/src/core/tagged.h) lines 21–24).
  - Round trips `md → rayomd (tagged and untagged) → rayopdf → md` give exact ground truth for both the tagged path and the heuristic path.

**Recommendation for rayopdf**
- Two-tier pipeline with uncertainty reporting:
  1. a validated structure tree → direct Markdown;
  2. heuristic classification over ordered blocks.
- Tables: **ruled lattice only** by default, rendered as GFM or HTML with spans. Borderless tables are detected by the stream method only when confidence is high (≥ 3 rows × ≥ 2 columns with consistent column count and alignment); otherwise keep them as text and flag them.
- Headings: outline match > tag > style clustering.
- Never invent LaTeX.
- Quality corpus:
  - opendataloader-bench (200 real PDFs; NID reading order, TEDS tables, MHS heading hierarchy, [odl-bench]);
  - RayoMD round trips;
  - the pdf.js test PDFs, veraPDF corpus and SafeDocs for robustness (as pdf_oxide uses, [oxide-perf]).

---

## 7. Speed engineering

### 7.1 What the fast extractors report
| Tool | Published number | Source / caveat |
|---|---|---|
| pdf_oxide (Rust) | 0.8 ms mean, 9 ms p99 **per document** over 3,830 PDFs (veraPDF 2,907 + pdf.js 897 + SafeDocs 26); PyMuPDF 4.6 ms, pypdfium2 4.1 ms, pdfminer 16.8 ms, pymupdf4llm 55.5 ms | [oxide-perf]: single thread, one process per PDF; mostly small test files; "quality" = 99.5% parity with PyMuPDF, not with ground truth |
| pdf_oxide gains | 10,000-page PDF from 55 s to 332 ms via a bulk page-tree cache (an O(n²) page lookup removed); a scan-once offset cache; a single pass; op vector pre-sized at len/20 | [oxide-perf] "Optimization History" |
| PyMuPDF / pypdfium2 / pdftotext | GeoTopo book (117 pages): 0.3 s / 0.3 s / 1.1 s ≈ 390 / 390 / 106 pages/s; pdfminer 16.1 s ≈ 7 pages/s | [pypdf-bench] (i7-6700HQ, Python wrappers) |
| PDF→Markdown engines | opendataloader 0.015 s/page, nutrient 0.008, edgeparse 0.036, pymupdf4llm 0.091, docling 0.762, marker 53.9 | [odl-bench] |

### 7.2 Inflate throughput (published)
- TurboBench, silesia.tar, Ryzen 6600HS ([turbobench]) — decompression:
  - **libdeflate ≈ 1.08–1.13 GB/s**;
  - **igzip ≈ 1.0 GB/s**;
  - **zlib-ng ≈ 650–735 MB/s**;
  - **zlib ≈ 450–484 MB/s**.
  libdeflate has whole-buffer API only (comment in the same thread).
- zlib-ng 2.3.1 on a Ryzen 8700GE: silesia-small (~15.7 MB) decompresses in ≈ 17.3 ms at level 6 ≈ 0.9 GB/s ([zlibng231]; throughput is my arithmetic from the published time and size). The maintainer reports zlib-ng 4–5× faster than stock zlib for level-6 compression.
- PDF content streams are small (KBs). Per-stream setup (table building, buffer growth) dominates, not steady-state throughput `[INFERENCE]`. Reuse decoder state per thread.

### 7.3 Engine architecture lessons
- **mmap + lazy xref**: parse objects only on demand.
  - The page tree is walked once and cached (pdf_oxide).
  - Do not parse font programs unless §3's fallbacks need them.
  - Never decode image streams unless writing images.
- **Threading**:
  - MuPDF: the document is single-threaded, page→display list on the main thread, workers use cloned contexts ([mupdf-mt]).
  - PDFium: not thread-safe even across documents ([pypdfium2-thread]).
  - rayopdf can do better by design: an immutable shared document (file view + xref + font cache with once-init per entry) and **per-page parallel extraction**: decode, interpret, line building and per-page layout. Cross-page passes (headers and footers, heading levels, footnotes) run after a barrier `[INFERENCE]`.
- **Arenas**:
  - a per-page bump arena for events, glyphs, lines and blocks, reset per page;
  - a per-document arena for the font cache;
  - no per-glyph heap allocation;
  - string views into decoded buffers.
- **SIMD**:
  - byte-class scanning for whitespace and delimiters; `memchr`-style search for `endstream`, `obj` and `EI` during repair;
  - simdjson-style structural classification is overkill for operand-dense content streams. Most time goes to number parsing and the operator dispatch per token `[INFERENCE: profile first, per RayoMD practice]`.
  - PDF's no-exponent number grammar (§7.3.3) allows a branch-light fixed-point parser.
- **Layout costs**: avoid O(n²) grouping (pdfminer's heap of all pairs, [pdfminer-layout] lines 846–851). Use sort-and-sweep on baselines and an interval tree or grid for neighbour queries.

**Recommendation for rayopdf**
- **Targets**:
  - single-threaded ≥ 1,000 text pages/s on text-dense pages (≈ 2.5× PyMuPDF's 390 pages/s on GeoTopo, accounting for Python overhead) `[INFERENCE: target, not measured]`;
  - < 5 ms cold start for a 1-page file;
  - RSS bounded by file size + per-thread page arenas.
- **Measure, as RayoMD does**: hyperfine or benchstat A/B against PyMuPDF, pdftotext and pdf_oxide on the same corpus (`perfbox`, `qtbench`, `pdftools` docker images). Report pages/s, p99 and RSS.
- **Inflater**: own salvage-capable streaming inflater (§1), benchmarked against libdeflate as the speed ceiling; it must fall within ~1.3× on the large-stream decode path `[INFERENCE: acceptance bar]`.
- **Parallelism**: page tasks on a fixed pool (12 threads on the dev box). Deterministic output ordering; the font cache is filled under a per-font `std::once_flag`.

---

[iso1]: https://opensource.adobe.com/dc-acrobat-sdk-docs/pdfstandards/PDF32000_2008.pdf
[iso2]: https://ia601607.us.archive.org/2/items/iso-32000-2-2020-sponsored/ISO_32000-2-2020_sponsored.pdf
[iso2-errata]: https://pdf-issues.pdfa.org/32000-2-2020/clause07.html
[agl]: https://github.com/adobe-type-tools/agl-specification
[cmapres]: https://github.com/adobe-type-tools/cmap-resources
[brotli]: https://pdfa.org/download-area/publications/pdf-extension-brotli.pdf
[pdfjs-eval]: https://github.com/mozilla/pdf.js/blob/master/src/core/evaluator.js
[pdfjs-xref]: https://github.com/mozilla/pdf.js/blob/master/src/core/xref.js
[pdfjs-parser]: https://github.com/mozilla/pdf.js/blob/master/src/core/parser.js
[pdfjs-metrics]: https://github.com/mozilla/pdf.js/blob/master/src/core/metrics.js
[pdfbox-strip]: https://github.com/apache/pdfbox/blob/trunk/pdfbox/src/main/java/org/apache/pdfbox/text/PDFTextStripper.java
[pdfbox-legacy]: https://github.com/apache/pdfbox/blob/trunk/pdfbox/src/main/java/org/apache/pdfbox/text/LegacyPDFStreamEngine.java
[pdfbox-simple]: https://github.com/apache/pdfbox/blob/trunk/pdfbox/src/main/java/org/apache/pdfbox/pdmodel/font/PDSimpleFont.java
[pdfbox-afm]: https://github.com/apache/pdfbox/tree/trunk/pdfbox/src/main/resources/org/apache/pdfbox/resources/afm
[mupdf-stext]: https://github.com/ArtifexSoftware/mupdf/blob/master/source/fitz/stext-device.c
[mupdf-unicode]: https://github.com/ArtifexSoftware/mupdf/blob/master/source/pdf/pdf-unicode.c
[mupdf-opts]: https://mupdf.readthedocs.io/en/1.28.6/reference/common/stext-options.html
[mupdf-mt]: https://mupdf.readthedocs.io/en/latest/cookbook/c/multi-threaded.html
[pdfium-csp]: https://github.com/chromium/pdfium/blob/main/core/fpdfapi/page/cpdf_streamcontentparser.cpp
[pdfium-syn]: https://github.com/chromium/pdfium/blob/main/core/fpdfapi/parser/cpdf_syntax_parser.h
[pdfium-flate]: https://github.com/chromium/pdfium/blob/main/core/fxcodec/flate/flatemodule.cpp
[pypdfium2-thread]: https://pypdfium2.readthedocs.io/en/stable/python_api.html
[pdfminer-layout]: https://github.com/pdfminer/pdfminer.six/blob/master/pdfminer/layout.py
[pdfminer-font]: https://github.com/pdfminer/pdfminer.six/blob/master/pdfminer/pdffont.py
[plumber-readme]: https://github.com/jsvine/pdfplumber/blob/stable/README.md
[plumber-table]: https://github.com/jsvine/pdfplumber/blob/stable/pdfplumber/table.py
[camelot]: https://github.com/camelot-dev/camelot/blob/master/docs/user/how-it-works.rst
[nurminen]: https://trepo.tuni.fi/bitstream/handle/123456789/21520/Nurminen.pdf?sequence=3
[xycutpp]: https://arxiv.org/abs/2504.10258
[odl-ro]: https://opendataloader.org/docs/reading-order
[odl-bench]: https://opendataloader.org/docs/benchmark
[odl-safety]: https://opendataloader.org/docs/ai-safety
[odl-tagged]: https://opendataloader.org/docs/tagged-pdf-rag
[breuel]: https://www.dfki.de/fileadmin/user_upload/import/2000_HighPerfDocLayoutAna.pdf
[docstrum]: https://doi.org/10.1109/34.244677
[kumar]: https://arxiv.org/abs/2410.03022
[oxide-perf]: https://pdf.oxide.fyi/rust/docs/performance
[pypdf-bench]: https://github.com/py-pdf/benchmarks
[turbobench]: https://github.com/zlib-ng/zlib-ng/discussions/2351
[zlibng231]: https://github.com/zlib-ng/zlib-ng/discussions/2022
[libdeflate-h]: https://github.com/ebiggers/libdeflate/blob/master/libdeflate.h
[pymupdf4llm]: https://pymupdf.readthedocs.io/en/latest/pymupdf4llm/api.html
[uax9]: https://www.unicode.org/reports/tr9/

## Open verification items

- **PDF 2.0 text**: the sponsored ISO 32000-2 PDF timed out on search. These points are marked `[unverified]` in the document:
  - the inline-image length key;
  - the UTF-8 text-string clause;
  - the full PDF 2.0 structure-type list;
  - whether AES-256 uses a per-object key.
- **Recalled, not re-read**: the ForceBold bit (19), the FontWeight entry, ItalicAngle, the RowSpan/ColSpan section number, and MuPDF's `pdf-repair.c`.
- **My own numbers**: every threshold and target tagged `[INFERENCE]` is a suggestion, not a measurement.
