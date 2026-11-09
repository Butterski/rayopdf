# OCR add-on protocol and routing

RayoPDF runs OCR only where a PDF's text layer fails, and only through an add-on: a separate
executable `rayopdf-ocr` (`rayopdf-ocr.exe` on Windows) with its model files. The core
`rayopdf` holds the client (routing, rendering the page images, the pipe protocol), never an OCR
engine. Without the add-on the core writes exactly what it wrote before: a page that needs OCR
keeps its `<!-- rayopdf: page N has no extractable text -->` comment. The decision behind this
is [ocr_ml_decision.md](ocr_ml_decision.md).

|Part|Code|
|---|---|
|Frame codec, shared by both sides (standard library only)|`src/common/ocr_protocol.{h,cpp}`|
|Routing|`src/layout/ocr_route.{h,cpp}`|
|Page rasters and the OCR text layer|`src/layout/ocr_layer.{h,cpp}`|
|Add-on discovery, process, client|`src/layout/ocr_client.{h,cpp}`, `src/layout/ocr_process.{h,cpp}`|
|Orchestration inside `Convert`|`src/layout/ocr.{h,cpp}`|
|Glyph-shape recovery and vector text|`src/layout/ocr_glyphs.{h,cpp}`, `src/layout/ocr_paint.{h,cpp}`, `src/text/outline.{h,cpp}`, `src/raster/`|
|The add-on (server)|`src/ocr/` (built with `-DRAYOPDF_OCR_ADDON=ON`)|
|Stand-in add-on for the core tests|`tests/ocr_mock_addon.cpp`|

## Command line

|Flag|Meaning|
|---|---|
|`--ocr=auto` (default)|OCR image-only pages and text-bearing images, when an add-on is found|
|`--ocr=off`|never look for or start the add-on|
|`--ocr=force`|OCR every page image and every large image without the text check; an existing invisible text layer is replaced when the add-on returns text|
|`--ocr-addon=PATH`|the add-on to run|

The flags apply to `--convert`, `--batch`, `--bench` and to `--serve` sessions and requests.
`--report` gains the counters `ocr_pages_routed`, `ocr_pages`, `ocr_failures`,
`ocr_addon_missing`, `ocr_broken_pages`, `ocr_vector_text_pages`, `ocr_glyph_fonts` and
`ocr_glyphs` (below).

## Discovery and process lifetime

The add-on is looked for in this order, the first hit wins:

1. `--ocr-addon=PATH`;
2. the environment variable `RAYOPDF_OCR`;
3. `rayopdf-ocr` in the directory of the running `rayopdf` executable;
4. `rayopdf-ocr` in the directories of `PATH`.

A path given by the flag or the variable that names no executable file is not replaced by a
later step: the add-on counts as missing. Discovery happens only when a page of the document is
routed to OCR, so documents with a working text layer pay nothing.

The core starts the add-on as `rayopdf-ocr --serve` with its standard input and output connected
to the core (a socket pair on Linux, two overlapped named pipes on Windows) and its standard
error sent to the null device (set `RAYOPDF_OCR_STDERR=1` to keep it). One add-on process
serves one document with `--convert`; in `--batch` and `--serve` each worker keeps one process
from document to document. At the end the core sends `quit`, waits up to 5 s for the exit, then
kills the process.

An add-on that cannot be started, or whose first frame is not a valid handshake of protocol 1, is
not tried again for the rest of the run (the worker's session). An add-on that crashes, hangs or
answers anything but a well-formed response to the request it was sent is killed; no further
page of that document is sent, and the next document starts a new process.

## Frames

Every message in both directions is one frame:

1. a little-endian `u32`: the byte length of the header, its final `\n` included (2 to 1 MiB);
2. the header: one UTF-8 JSON object on one line, ending in `\n`, with no other `\n`;
3. the payload: as many bytes as the header's member `"payload"` says (0 when it has none).

Header and payload together are at most 256 MiB. Readers check the length before reading the
header and grow the payload buffer only as bytes arrive. JSON nesting is at most 16 levels.
Numbers are integers, except `conf`. Strings are UTF-8; `\uXXXX` escapes (with surrogate pairs)
are accepted; invalid UTF-8 and lone surrogates are rejected.

The writers in `ocr_protocol.h` emit members in the order shown below and nothing else, so the
same structure gives the same bytes on both sides. The parsers take members in any order, skip
unknown members, reject duplicate known members, and reject anything malformed or out of bounds.

## Handshake

The add-on's first frame, sent unprompted after start:

```json
{"rayopdf-ocr":"0.0.9","protocol":1,"engine":"pp-ocrv6","tier":"small","runtime":"ncnn",
 "models":{"config.txt":"<sha256>","dict.txt":"<sha256>","det.ncnn.param":"<sha256>",
           "det.ncnn.bin":"<sha256>","rec.ncnn.param":"<sha256>","rec.ncnn.bin":"<sha256>",
           "tiny/config.txt":"<sha256>","tiny/dict.txt":"<sha256>","tiny/rec.ncnn.param":"<sha256>",
           "tiny/rec.ncnn.bin":"<sha256>"},
 "scripts":["latin","greek","kana","han"]}
```

`rayopdf-ocr` (the add-on's version) and `protocol` are required; the core refuses any protocol
other than 1. The other members describe the engine for reports:
- `models`: the model files the engine reads, with their SHA-256 from `models.txt`. First the tier
  folder's: `config.txt`, `dict.txt`, then the runtime's network files (`det.ort` and `rec.ort` for
  ONNX Runtime). With a tiny first pass (the default, [ocr-engine.md](ocr-engine.md)) the tiny
  folder's `config.txt`, `dict.txt` and recognition network follow as `tiny/<file>`; then the
  orientation classifier's files as `orient/<file>` and those of each recogniser of the script pack
  as `scripts/<script>/<file>`, when they are installed ([ocr-engine.md](ocr-engine.md)).
- `scripts`: the scripts with at least 20 entries in the tier's dictionary, in the order latin,
  greek, cyrillic, hebrew, arabic, devanagari, thai, kana, han, hangul; then the script pack's
  (cyrillic, hangul, arabic, devanagari, kannada, tamil) not listed yet.

The Tesseract backend announces `"engine":"tesseract"`, its language as `tier`, its version as
`runtime`, no `models` and its scripts. The handshake must arrive within 30 s (models are loaded
before it).

## Requests

```json
{"id":1,"op":"page","width":2550,"height":3300,"channels":1,"dpi":300,"payload":8415000}
{"id":2,"op":"lines","width":0,"height":0,"channels":1,"dpi":300,
 "lines":[{"width":412,"height":48},{"width":96,"height":48}],"payload":24384}
{"id":3,"op":"quit","width":0,"height":0,"channels":1,"dpi":0,"payload":0}
```

- `id`: an unsigned integer, counted from 1 per add-on process; the response carries it back.
- `op`: `page` = detect and recognise the text of one image; `lines` = recognise given line
  images with the tier's recogniser, never a first pass (glyph-shape recovery, plan Phase C);
  `quit` = answer, then exit 0.
- Pixels: 8-bit gray (`channels` is always 1), rows top to bottom, no padding. `page`: `width` x
  `height` pixels. `lines`: the line images of `lines`, concatenated; `width` and `height` are 0.
- `dpi`: the resolution of the pixels (0 when unknown), so that the engine can scale them. The
  core sends 72 to 300 dpi; only a canvas over 2^26 pixels (a region larger than about 8 m² at
  72 dpi) goes below 72.
- Limits: each side at most 2^20 pixels, all pixels of a request at most 2^28, at most 65,536
  line images, `dpi` at most 10,000. `payload` must equal the pixel count.

## Responses

```json
{"id":1,"status":"ok","lines":[{"box":[120,80,1840,128],"text":"Chapter 1","conf":0.9731,
 "chars":[[120,170],[170,214],...]}]}
{"id":1,"status":"error","error":"bad-pixels"}
```

- `lines`: the add-on sends them in reading order (rows top to bottom, then left to right); the
  core orders them again by their boxes. `box` is `[x0,y0,x1,y1]` in pixels of the
  request's image, origin top-left, y down, `x1` and `y1` exclusive. `text` is UTF-8. `conf` is 0
  to 1 with at most four decimals (both sides hold it as an integer 0–10000). `text` is in logical
  order (right-to-left text as it is read). `chars` has exactly one `[x0,x1]` span per Unicode
  code point of `text`, spaces included (a space gets the gap it spans), from the recogniser's
  time steps.
- `dir` (optional, after `chars`, written only when not 0): the baseline direction of the line's
  text in the image, as `Glyph::dir`: 1 runs bottom to top, 2 upside down (right to left), 3 top
  to bottom. The spans of a line of direction 1 or 3 are rows (`[y0,y1]`), of 0 and 2 columns. A
  page scanned turned a quarter or upside down comes back with every line so; the core lays the
  glyphs out in the frame of that direction (`src/layout/ocr_layer.cpp`: the canvas is turned so
  that the lines stand upright, their typography measured there, and the glyphs and rules mapped
  back), as for rotated text of a digital page. Cores before this member skip it; protocol stays 1.
- For `lines` requests: one response line per line image, in request order, `box`
  `[0,0,width,height]` of that image; empty text and `conf` 0 when nothing is recognised.
- Errors: `too-large` (over a limit, or a response whose header would exceed 1 MiB),
  `bad-request` (malformed header), `bad-pixels` (payload size does not match), `engine-failed`.
  An error response leaves the add-on running. A frame the add-on cannot read whole is answered
  with an error for id 0, then the add-on exits 5; at the end of its input between frames it
  exits 0.
- Responses carry no payload.

## Determinism and timeouts

The same request bytes give the same response bytes: inference is single-threaded per request and
the answer does not depend on earlier requests or on the `id` beyond echoing it. With that, the
core's output does not depend on `--workers` or on which worker's add-on serves a document.

The core waits at most 120 s for a complete response (`OcrOptions::requestTimeoutMs`); then it
kills the add-on and falls back. Timing therefore affects output only when the add-on hangs.

## Routing

Routing runs per page after interpretation, from what the interpreter already found
(`ocr_route.h`). Characters counted are non-space glyphs that line building keeps (not clipped,
at least 1 pt, with text); coverage is the area of the placed images over the page area, summed
and capped at 1.

|Route|Rule (auto)|Census class it targets|
|---|---|---|
|Page|fewer than 50 characters (visible or an invisible OCR layer) and image coverage ≥ 0.8|`image_only`|
|Region|any other page: each image covering ≥ 5 % of the page, at least 64 pixels wide and high, with at most 10 characters drawn over it, whose pixels look like printed text|text-bearing image regions|
|Glyph shapes|the text layer fails the broken-text checks (below), or shows at least 3 junk characters: the fonts whose text is missing or wrong get it from their glyphs' shapes (§ Glyph shapes)|`broken`, junk lines|
|Vector text|≥ 300 glyph-sized filled curves on a page with fewer than 50 characters: the page's paths painted at 300 dpi and recognised as a page|part of `empty_or_vector`|
|None|a page whose text is an existing invisible layer (≥ 50 characters)|`text_over_image`|

`--ocr=force` routes every page with images and fewer than 50 characters, every page whose
visible text is under 50 characters over images covering ≥ 0.8 (its invisible layer is replaced),
and every large image region without the text check.

**Broken-text checks** are those of the census (`eval/census/census.py`, docling's checks) on the
page's glyph text: junk characters (U+FFFD, private use, C0/C1 controls) ≥ 3 %; ≥ 50 % of ≥ 50
Latin letters in U+00C0–U+024F; vowel share < 0.25 over ≥ 200 letters in words of 3–20 Latin
letters; or, as the "mostly unmapped" comment, unmapped or guessed codes for more than half of
≥ 20 glyphs. The census's junk-lines check needs lines and is not applied here. Pages with fewer
than 50 characters are not counted as broken. These pages are counted in `ocr_broken_pages`.

**Text check for regions** (`LooksLikeText`, on the region rendered as for OCR): Otsu's threshold
splits ink from paper; the paper is light (mean ≥ 160), ink and paper differ by ≥ 64 grey levels,
ink covers 0.5–35 % of the pixels and at most 15 % of the pixels are midtones (more than 30 % of
the contrast away from both means: photos fail here). Runs of rows holding ink are bands; a band
is a text line when it is 4 px to a third of the image high and each of its rows crosses at least
3 ink runs and at least 0.8 per band height of its ink span (strokes of characters, not bars or
rules). At least 4 text lines within a factor 2 of their median height, covering ≥ 15 % of the
rows, make a text region. A chart with a title and axis labels, a photo, a logo or a single
caption line does not.

## Pixels

For a page route all routed images are composed onto one gray canvas of the whole upright page;
for a region, the one image onto a canvas of its box (`ocr_layer.h`). The canvas resolution is
the finest of the images' own (pixels over placed area), clamped to 72–300 dpi and lowered so
that the canvas stays within 2^26 pixels and 32,768 pixels a side. Images are decoded through the
pixel API of `src/image/image.h` (`GrayXObject`, `GrayInline`: colour space, `/Decode`, masks
composited on white, area-averaged to about the canvas resolution) and drawn through their
placement matrix with nearest-neighbour sampling; where images overlap the darker pixel wins, which
keeps text masks over backgrounds (mixed raster content) and joins strip-wise scans. An image the
core cannot decode leaves the page as without OCR.

## Text layer

Each response line with `conf` ≥ 0.5 becomes glyphs, one per code point, with the box from its
`chars` span and the line's box, mapped back to page space. Lines are added top to bottom, then left
to right. The glyphs are flagged `kGlyphInvisible | kGlyphOcr` and are kept even when the page has
visible text (a region on a digital page). Unlike an invisible OCR layer that came with the PDF
(whose word boxes are fitted, so it gives paragraphs and lists only), the add-on's layer carries
the typography the canvas shows (`src/layout/ocr_style.h`), so headings, emphasis, code and quotes
come out of the usual rules:

- **Size and baseline.** Otsu's threshold over the line boxes splits ink from paper (pages whose
  classes are closer than 48 grey levels keep the box geometry below). In each letter's columns
  the vertical run of ink rows holding the most ink is the letter (a neighbouring line's ink is
  cut off by two blank rows); x-height letters (a c e m n o r s u v w x z, Greek and Cyrillic
  ones) give the x-height, capitals and digits the cap height, so the measure holds on skewed
  scans. The font size is the x-height over 0.5, or the cap height through the page's own
  cap/x-height ratio; the baseline is the median foot of the letters standing on it. Sizes within
  8 % of the page's body size (weighted median) become the body size, other sizes within 6 % of
  each other one size; a line with fewer than 3 measurable letters takes the size of the nearest
  sized line on its baseline (a table row), else the body size. The line keeps its box as its
  vertical extent, widened to 0.2 em below and 0.8 em above the baseline when smaller. Lines with
  nothing measurable keep the old geometry: baseline at a fifth of the box, size three quarters.
- **Bold.** A word's stroke width is the mean horizontal ink run (runs over 0.3 em left out) in
  the band of its x-height letters (or capitals), over the font size. Bold is at least 1.3 times
  the page's body words (weighted median). Text larger than the body is bold per size, all its
  lines pooled (one heading level, one style); body text per word of 3 letters or more, a line
  with 60 % of its letters in bold words bold throughout, a short word between bold words bold
  from 1.15 times.
- **Italic.** The shear (−0.15 to 0.45, 13 steps) that piles the ink of a word's stem letters
  (b d h l m n p q r u, B D E F H I L P R T U) into the fewest columns; italic is at least 0.12
  more than the page's body words, with the same line and between-word rules.
- **Fixed pitch.** A line of 16 code points or more whose character centres, spaces included,
  lie on a grid (least squares, refitted without up to 15 % slips; residual ≤ 0.12 of the cell,
  cell 0.47–0.75 em) is monospace; a shorter line whose centres all sit on the grid of a
  monospace neighbour (within 2.5 em, same size) too, a longer one with 85 % of them. Neighbouring
  monospace lines of one pitch share the grid of the longest, and their glyphs take its cells, so
  that code blocks keep their columns and indentation.
- **Rules.** Dark runs (at least max(24, contrast/5) below the paper) found in 16-pixel strips
  (covering 85 % of a strip, at most 0.3 em thick), chained across strips and extended pixel by
  pixel: horizontal ones at least 3 em long, vertical ones at least 1.3 em (a one-line quote bar).
  Each becomes a filled ruling and a filled rectangle shape, unless the page already has rulings
  over the canvas (vector-text pages). Lattice tables and quote bars use them as for digital
  pages.

Reading order, tables, header and footer removal and the writer then run as usual, on tagged
pages too. One rule differs for OCR text: a heading style holding a large share of the text
outside tables is demoted to body text, as for any text, except that a style of title size
(1.38 times the body) is weighed against the tables' text as well. A scan has no outline to name
the section titles of a document that is mostly tables. A page image covering ≥ 90 % of a page
with text is a background and is not written as an image file, as for a scan with its own OCR
layer.

**Text in images of digital pages (region routes)** goes where a reader meets the image. On a page
with a text layer (at least 50 characters), when images are written (the default), the text
becomes the image's alternate text,
`![text](file)`: the lines joined top to bottom with single spaces, white space and control
characters collapsed, at most 1,000 bytes cut after a whole word with "…", and Markdown escaped
by the writer. A tagged figure's `/Alt` is kept and the OCR text is then not used. Such an image is
never set inline in a text line; an image that is not written (a logo repeated on many pages, a
page background) takes its text with it. With `--images=none` the text is body text at the image's
place, as above. Whole-page routes and regions on pages without a text layer (scans, scans drawn
with margins) are always body text: there the image's text is the page's. The decision and its numbers are
recorded in [ocr_ml_decision.md](ocr_ml_decision.md#text-in-images-of-digital-pages).

## Glyph shapes

Fonts whose text is missing or wrong get it from the shapes of their glyphs
(`src/layout/ocr_glyphs.h`). This runs only when an add-on runs; without one nothing changes,
not even the report.

1. **Pages and fonts.** A page goes in when it fails a broken-text check or shows at least 3 junk
   characters, and no page route takes it. Such pages are interpreted again with each glyph's
   font, code and placement recorded. A font whose own text on them has mostly unmapped or
   guessed codes (≥ 4 glyphs), ≥ 50 % accented letters (≥ 20 Latin letters) or a vowel share
   below 0.25 (≥ 50 letters in words) is examined as a whole; in any other font only the codes
   whose text is junk, unmapped or guessed are. Then every page of the document with glyphs is
   interpreted again for those fonts' uses (at most 2^22 uses held).
2. **Outlines.** The font's embedded program is read again (`src/text/outline.h`): TrueType
   `glyf` (composites 8 deep), CFF with Type 2 charstrings (local and global subroutines 10 deep,
   flex, seac), Type 1 (eexec, lenIV, Subrs, flex and hint replacement OtherSubrs, seac), and
   Type 3 glyph procedures painted with their paths and image masks (`src/layout/ocr_paint.h`).
   Codes reach glyphs as §9.6.6 says: the encoding's glyph names (CFF charset, Type 1
   CharStrings), the TrueType (3,0), (1,0) and (3,1) cmaps and `post` names, the Type0 CID
   through /CIDToGIDMap or the CFF charset. The standard 14 fonts without a program and CFF2 are
   not drawn.
3. **Lines.** Each distinct glyph shape (pixel-identical glyphs once) is drawn by the core's
   rasterizer (`src/raster/`, exact-area anti-aliasing) on one baseline and scale per font: the
   glyphs' vertical extent (from 20 glyphs the 5th to 95th percentile, below that at least the
   font's em box) fills a 48-pixel line, the recogniser's own input height. The glyphs stand in
   order of first use with 0.35 em between their ink, at most 24 to a line and 2,400 pixels wide,
   and all of a font's lines go in one `lines` request. A glyph that draws nothing is a space.
4. **Splitting.** A character belongs to the glyph whose ink centre its span holds (spans run
   between the middles of neighbouring characters' time steps, so a character's span holds its own
   glyph's centre); when it holds none, to the glyph nearest the span's middle. A glyph can get
   several characters (a ligature, "fi"); one that gets none, or whose line is below 0.5
   confidence, keeps its text.
5. **Plausible reading.** A font is used only when its lines average ≥ 0.70 confidence per glyph
   (≥ 0.80 and at least 4 shapes read when every code of the font is replaced), ≥ 75 % of its
   shapes were read, and ≥ 90 % of the letters read are of one script (Latin, Greek, Cyrillic,
   CJK, other). Shapes of a script the recogniser lacks (the legacy Indic and Arabic fonts of the
   census and of olmOCR-bench) come out as a scatter of look-alikes at low confidence. Outside CJK
   text, full-width forms read (U+FF01–U+FF5E) become their ASCII characters: a glyph set apart in
   a line of single glyphs is often read as one.
6. **Isolated letters.** In a symbol font (the Symbolic flag, or Symbol, Dingbats or Wingdings in
   the name) or a font examined only for some codes, a glyph read as a letter or digit keeps that
   reading only where some use of it forms a word: its neighbours in content order (same
   direction, on its baseline, within 0.3 of its size) end or start with a letter or digit — on
   both sides in a font examined for some codes (a list marker set against its text has one), on
   one side in a whole symbol font. Check marks, arrows, bullets and circled numbers read as
   "V", "3", "4" or "1" keep their old text.
7. **Acceptance.** The new text replaces the font's text on the pages that sent it only when those
   pages then fail fewer broken-text checks, or as many with less junk or fewer unreliable codes
   and no check that did not fire before; otherwise everything is put back. An accepted font's
   glyphs get the text on every page. Glyphs covered by ActualText or dropped keep theirs.

**Complex scripts: words, not glyphs.** A legacy-encoded Devanagari, Kannada or Tamil font draws
a syllable from several glyphs (a vowel sign before its consonant, half forms), so a glyph alone
reads as nothing or as another letter. When a whole font's glyphs read mostly as letters of such a
script (the add-on's script pack recognises them, § ocr-engine.md), the font's words are drawn
instead (`RecogniseWords`): a word is a run of the font's glyphs in content order on one
baseline, each starting at most 0.25 em after the last one ends; each distinct word (glyphs and
offsets) is drawn once at the font's line scale with its glyphs at their places, the words go
0.6 em apart into lines of at most 12, and a line whose text splits at its spaces into as many
words gives each word its text (the other lines' words go again 4 to a line 1.5 em apart, then
the rest alone). A word's first glyph takes
the word's text and spans the word; its other glyphs give no text. Plausibility (≥ 0.70 mean
confidence, ≥ 75 % of the words read, ≥ 90 % of the letters of one script) and acceptance are as
above.

**Measured** (2026-11-08, AMD Ryzen 5 PRO 4650GE, Docker; add-on ncnn PP-OCRv6 small; harness
roots `benchmark-output/eval-glyph*`):

|What|Before|After|
|---|---|---|
|SafeDocs census pages RayoPDF's text leaves broken (`tools/ocr_gate.py`, each page alone)|152 (1.71 %)|130 (1.46 %)|
|M3: pdftotext-broken pages RayoPDF reads|156 of 307 (50.8 %)|177 of 307 (57.7 %)|
|census pages that got worse (text before, not after)|–|0|
|odl, olmOCR born-digital, synthetic Markdown without an add-on|–|byte-identical|
|olmOCR failed tests on the 10 documents where fonts were recovered|25|18|
|core `rayopdf`, static configuration|3,009,288 B|3,148,256 B (new code ≈ 132 KB of object text)|
|in-process odl median ms/page (CPU 3, best of 3, two A/B rounds)|1.261, 1.260|1.255, 1.244|

The 22 pages fixed: a Swedish Garamond with a shifted encoding (15 pages), a Type 3 Italian text,
circled numbers, rating stars and bullets of symbol fonts. Cost per document with recovered fonts,
the add-on's start included: +0.81 s for two fonts on 15 pages, +1.0 s for two Type 3 fonts,
+0.1 s for one symbol font (about 0.1–0.5 s per font).

**Vector text.** A page that looks like vector-outline text is painted onto a page canvas at
300 dpi: filled and stroked paths in their colours' luminance (a stroke as one quadrilateral per
flattened segment), image masks and images, through Form XObjects (8 deep, cycles skipped), at
most 2^22 operators. The canvas goes through the `page` op and its lines become an OCR text layer as
for a scan.

## Report and comments

|Counter|Meaning|
|---|---|
|`ocr_pages_routed`|pages routed: whole pages, and pages with a region that passed the text check (regions are only checked when an add-on runs)|
|`ocr_pages`|routed pages that received text|
|`ocr_failures`|routed pages the add-on failed on (start failure, crash, timeout, garbage, wrong id, error response), or that were not sent after it failed|
|`ocr_addon_missing`|pages were routed and no add-on was found|
|`ocr_broken_pages`|pages with a broken text layer|
|`ocr_vector_text_pages`|pages that look like vector-outline text (they are routed when an add-on runs)|
|`ocr_glyph_fonts`|fonts whose recognised glyphs replaced their text|
|`ocr_glyphs`|shown glyphs given text from their shapes|

A routed page that stays without text because the add-on failed is written as
`<!-- rayopdf: page N has no extractable text; OCR failed -->`. A missing add-on leaves the usual
`<!-- rayopdf: page N has no extractable text -->`.

## Tests

`tests/test_layout_ocr.cpp` (codes 1200–1249) covers the codec (round trips, every malformed
frame and header class, limits), routing, the text check, canvases and the text layer, the
client against `tests/ocr_mock_addon.cpp` in each failure mode (`RAYOPDF_OCR_MOCK_MODE`: crash,
crash at start, hang, hang at start, garbage, malformed JSON, wrong id, error response, bad
handshake, low confidence), whole conversions with and without the add-on, and conversions on
three threads with per-worker sessions against one session in order. `fuzz_common_ocr_protocol`
fuzzes the codec: every parsed header is written back, read again to the same structure, and
written again to the same bytes. `tests/test_layout_glyph.cpp` (codes 1350–1399) covers the
rasterizer, the TrueType, CFF and Type 1 outline readers, code-to-glyph mapping of the four font
kinds, whole recoveries through the stand-in's mode `glyphs` (bars read as letters, a four-bar
glyph as the ligature "fi"), the low-confidence rejection, ActualText left alone, isolated symbol
readings dropped, and a vector-text page; `fuzz_text_outline` fuzzes the outline readers and the
rasterizer.
