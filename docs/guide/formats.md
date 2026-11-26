# Output formats

What RayoPDF writes: Markdown (the default), JSON, chunks for retrieval, plain text, HTML and an
HTML layout view, plus the report record of `--report`. The [command-line guide](cli.md) lists
the options that choose and adjust them.

- [Markdown](#markdown)
- [Page markers and front matter](#page-markers-and-front-matter)
- [JSON: `--format=json`](#json---formatjson)
- [Chunks: `--format=chunks`](#chunks---formatchunks)
- [Plain text: `--format=text`](#plain-text---formattext)
- [HTML: `--format=html`](#html---formathtml)
- [Layout view: `--format=inspect`](#layout-view---formatinspect)
- [The report record](#the-report-record)

## Markdown

RayoPDF writes [GitHub Flavored Markdown](https://github.github.com/gfm/):

- **Headings** `#` to `######`, levels taken from the document's styles or its structure tree.
- **Paragraphs**, with emphasis, strikethrough, code spans and links where the PDF has them.
- **Lists.** Items keep their printed markers. Numbers followed by `.` or `)` become ordered
  items with the printed numbers (`4)` stays `4)`). Any other marker, such as `a.`, `(e)`, `iv)`
  or `[12]`, stays as text at the start of an unordered item: `- (e) No charge`. A printed check
  box (☐, ☑, ☒) before an item makes a task item, `- [ ]` or `- [x]`.
- **Code blocks**, block quotes and GitHub alerts (`> [!NOTE]`).
- **Tables** as pipe tables. A table where a cell with text spans rows, or spans columns below a
  header of several rows, or holds several paragraphs, is written as an HTML `<table>`.
  `--tables` changes this ([command-line guide](cli.md#options-for-a-readers-conventions)).
- **Footnotes** as `[^n]`. A note's definition follows the block that references it; notes
  printed on the last page (the notes of a one-page document, endnotes) stay at the end.
- **Images** as `![alt](report-images/p<page>-<n>.<ext>)`, with the images written beside the
  Markdown ([command-line guide](cli.md#converting-one-pdf)).
- **Formulas** as Unicode text, or as LaTeX with `--math=latex`
  ([command-line guide](cli.md#formulas---math)).
- **Right-to-left and CJK text** in reading order.
- **Form fields and FreeText annotations** as text where they stand, as a viewer shows them: a
  filled field's value, a check box or radio button as a task box (`- [x] I agree`); hidden
  annotations and password fields are left out. `--annotations=links` reads link annotations
  only, `--annotations=none` none ([command-line guide](cli.md#reading-the-pdf)).

Text is escaped only where it would otherwise change the structure. For example, `*` and `_`
are escaped only where a CommonMark reader could take them as emphasis, so `0.05**` stays as it
is.

### Comments for what could not be read

RayoPDF never drops text silently. Where something could not be read, a comment marks the place:

| Comment | Meaning |
|---|---|
| `<!-- rayopdf: page N has no extractable text -->` | The page has no text layer (a scan, or text drawn as shapes). The [OCR add-on](ocr.md) reads such pages. |
| `<!-- rayopdf: page N has no extractable text; OCR failed -->` | The add-on was started and failed on the page. |
| `<!-- rayopdf: page N text is mostly unmapped glyphs -->` | The page's fonts map most glyphs to no text; the add-on can read their shapes. |
| `<!-- rayopdf: the file was damaged and has been repaired; text may be missing -->` | At the start of the output, for a damaged file. |
| `<!-- rayopdf: this PDF is an XFA form; its fields are not read -->` | At the start of the output (after the comment of a repaired file), for an XFA form whose fields live only in its XFA data: its catalog asks a viewer to render it from XFA (`/NeedsRendering`), or no form-field widget stands on the converted pages. Not with `--annotations=links` or `none`. |
| `<!-- rayopdf: image on page N not extracted (NAME) -->` | An image that could not be written. |

The [report record](#the-report-record) counts the same cases, all but the XFA form.

## Page markers and front matter

`--page-markers` writes `<!-- page N -->` before the Markdown of each page, N being the page's
number in the file. A list or block quote that runs across a page break is split there.

```sh
rayopdf --convert paper.pdf paper.md --page-markers
```

`--front-matter` starts the Markdown with YAML front matter from the PDF's document
information: `title`, `author`, `subject`, `keywords`, `creator`, `producer`, `created` and
`modified` when the file has them (dates in ISO 8601), `language` (the PDF's `/Lang`, such as
`en-GB`) when it has one, and `pages`.

```sh
rayopdf --convert paper.pdf paper.md --front-matter
```

```yaml
---
title: "Annual report 2025"
author: "Example Ltd"
created: "2026-03-04T10:20:30+01:00"
pages: 12
---
```

Both apply to Markdown only. The JSON output always carries the metadata and each block's page.

## JSON: `--format=json`

```sh
rayopdf --convert paper.pdf paper.json --format=json
```

The document as one JSON object (schema 1), on one line:

```json
{"rayopdf":"1.1.0","schema":1,"repaired":false,
 "metadata":{"title":"...","author":"...","created":"2021-03-04T10:20:30+01:00"},
 "pages":[{"page":1,"width":612.00,"height":792.00},{"page":2,"width":612.00,"height":792.00}],
 "blocks":[
  {"type":"heading","page":1,"boxes":[{"page":1,"bbox":[72.00,700.10,310.45,718.30]}],"level":1,"markdown":"# Results"},
  {"type":"paragraph","page":1,"boxes":[...],"markdown":"The measured ..."},
  {"type":"table","page":2,"boxes":[...],"rows":3,"cols":2,"header_rows":1,
   "cells":[{"row":0,"col":0,"row_span":1,"col_span":1,"text":"Year"}, ...],"markdown":"| Year | Value |\n|---|---|\n..."}
 ]}
```

Top level:

| Field | Value |
|---|---|
| `rayopdf` | The version that wrote it. |
| `schema` | `1`. A later schema that changes a field's meaning gets a new number. |
| `repaired` | `true` when the file was damaged and repaired (text may be missing). |
| `metadata` | The document information fields that are not empty: `title`, `author`, `subject`, `keywords`, `creator`, `producer`, `created`, `modified`, and `language` (the PDF's natural language, such as `en-GB`). Dates are `YYYY-MM-DDTHH:MM:SS` with `Z` or `±HH:MM` when the PDF gives an offset; a date that does not parse stays as the PDF wrote it. |
| `pages` | One object a converted page: `page` (its number in the file, from 1), `width` and `height` in points, and `label` when the PDF has page labels: the number printed on the page as the PDF defines it (`iv`, `A-7`, `Index`). |
| `outline` | Only when the PDF has an outline (bookmarks): one `{"level","title","page"}` an entry, depth first, `level` 1 for the top level, `page` the page its destination leads to (`null` when it leads to none). Entries outside `--pages` are listed too. |
| `fields` | Only when the PDF has form fields with a widget on a converted page, and only with `--annotations=all` (the default): one object a field, in the order of the form's field tree: `name` (its full name, the partial names joined with `.`), `type` (`text`, `checkbox`, `radio`, `choice`, `signature`, `button`), `value` (`null` when it has none, and always for passwords, signatures and pushbuttons; a check box's `true` or `false`; a radio group's chosen option; a choice field's selection, a list when several are selected), `page` and `box` (`[x0,y0,x1,y1]`, its first widget on a converted page), `read_only` and `required`. |
| `annotations` | Only when a converted page has markup annotations shown on screen, and only with `--annotations=all` (the default): one object an annotation, page by page in the page's order: `page`, `type` (`highlight`, `underline`, `strikeout`, `squiggly`, `note` for a comment or `freetext`), `boxes` (`[[x0,y0,x1,y1],...]`, each quad's bounding box, or its rectangle when it has no quads), `contents` (its comment), `author`, `date` (as `metadata`'s dates) and, for the four text markups, `text`: the page's text whose glyph centres fall in its boxes, in reading order. A value the annotation lacks is `null`. These annotations do not change the Markdown. |
| `blocks` | The content, in reading order. |

Every block has, in this order:

| Field | Value |
|---|---|
| `type` | `heading`, `paragraph`, `list_item`, `code`, `table`, `image`, `footnote` or `comment`. |
| `page` | The page it starts on. |
| `boxes` | One `{"page":N,"bbox":[x0,y0,x1,y1]}` for each page it covers: the union of its lines, in points, origin at the bottom left of the page, two decimals. Turned text has its box on the upright page; a table has the table's box, an image its placement; a comment has `[]`. |
| `role` | Only on some blocks, what the block is besides its type: `caption` (a figure's or table's caption; in a tagged PDF its `/Caption` elements), `formula` (a displayed formula, with `--math=latex`), `page_header`, `page_footer` or `page_number` (page furniture kept with `--furniture=keep`). |

Then, by type:

| Type | Fields |
|---|---|
| `heading` | `level` (1 to 6). |
| `list_item` | `depth` (the nesting depth), `ordered` (`true` or `false`), `number` (ordered items only), `task` (`"open"` or `"done"`, task items only). |
| `table` | `rows`, `cols`, `header_rows`, and `cells`: `{"row","col","row_span","col_span","text"}`, where `text` is the cell's inline Markdown (also when the table is written as HTML). A table broken across pages is one table, without the header rows its later parts repeat. |
| `image` | `file` (the link target, or `null` when the image was not written) and `alt`. |
| `footnote` | `number`. |

Any block may also have `quote` (its block-quote depth, when above 0) and, on the first block of
an alert, `alert` (`note`, `tip`, `important`, `warning` or `caution`).

With `--json-detail=lines` or `--json-detail=words`, every block also has `lines`, just before
`markdown`:

```json
{"type":"heading","page":1,"boxes":[...],"level":1,
 "lines":[{"page":1,"box":[72.00,700.10,310.45,718.30],"text":"Results",
           "words":[{"box":[72.00,700.10,310.45,718.30],"text":"Results"}]}],"markdown":"# Results"}
```

| Field | Value |
|---|---|
| `lines` | The block's lines in reading order: `page`, `box` (`[x0,y0,x1,y1]`, as in `boxes`) and `text`. A table lists the lines of its cells, cell by cell; an image and a comment have `[]`. |
| `text` | The line's text as the page shows it: words joined by one space, without markup, without a list item's marker or inline images. A word hyphenated at the end of a line keeps its hyphen here. |
| `words` | `--json-detail=words` only: the line's words, each with its `box` and `text`. |

Last, every block has `markdown`: exactly the bytes the Markdown output writes for it. The
blocks' `markdown` values occur in the Markdown output in the same order, so a program can find
each block in the Markdown. A `comment` block is one of the `<!-- rayopdf: ... -->` comments.

`--page-markers` and `--front-matter` do not change the JSON. The options for a reader's
conventions (`--plain`, `--tables`, `--furniture`, `--quotes`, `--small-caps`, `--figure-text`)
do: each block's `markdown` and table cells are written as the Markdown output writes them, and
kept page furniture appears as blocks.

## Chunks: `--format=chunks`

```sh
rayopdf --convert paper.pdf paper.jsonl --format=chunks --chunk-chars=2000
```

Chunks for retrieval-augmented generation, as JSON Lines: one JSON object a line.

```json
{"chunk":3,"heading_path":["2 Methods","2.1 Data"],"pages":[2,3],"boxes":[...],"blocks":[14,15,16],"markdown":"### 2.1 Data\n\nWe collected ..."}
```

| Field | Value |
|---|---|
| `chunk` | Its number, from 0. |
| `heading_path` | The headings that enclose it, outermost first, as plain text. |
| `pages` | The first and last page it covers. |
| `boxes`, `blocks` | The boxes of its blocks and their indices in the `blocks` of `--format=json`. |
| `overlap` | With `--chunk-overlap`: how many bytes at the start of `markdown` come from the chunk before. |
| `markdown` | The blocks' Markdown, joined by a blank line. |

How chunks are cut:

- A heading starts a new chunk, unless the open chunk holds only headings so far.
- Blocks follow under it until the next block would take the chunk past `--chunk-chars=N`
  bytes (256 to 1,000,000; default 4,000).
- A block longer than N becomes several chunks of its own: text is split at sentence ends (else
  at the last space), a table by rows with its header rows repeated in each piece, code by lines
  with its fence reopened. A single table row or code line longer than N stays whole.
- With `--chunk-by=page` a chunk also ends where a block starts on another page than the block
  before it (long pages still split by size).
- With `--chunk-overlap=N` (0 to 500,000 bytes, at most half of `--chunk-chars`) each chunk but
  the first begins with the end of the chunk before it: at most its last N bytes, starting at a
  word, then what stood between the two (a blank line, or the space where a long text was split).
  `overlap` gives that length: the chunks' `markdown` without their first `overlap` bytes is what
  `--format=chunks` writes without overlap. A later piece of a split table or code block begins
  with its reopened header or fence instead, with `overlap` 0.
- Page markers and front matter are not written into chunks.

## Plain text: `--format=text`

```sh
rayopdf --convert paper.pdf paper.txt --format=text
```

The blocks of the Markdown as text without any markup:

- headings and paragraphs as their text: no `#`, emphasis, code spans, `<sup>`/`<sub>` tags or
  escapes; a link as its text;
- list items with their markers (`-`, `1.` or `1)`, as in the Markdown) and their indentation, and task
  items with `[x]` or `[ ]`;
- code as its lines, without fences;
- a table one row a line, its cells separated by a tab (a spanning cell's text in its first row
  and column, the cells it covers empty);
- footnotes as `[n] text` after all other text, their references in the text as `[n]`;
- no images and no comments.

Blocks are separated by a blank line, list items by a line break. Pages are separated by a form
feed (U+000C): the text of page N follows the (N−1)th form feed, and a page without text adds
one. With `--math=latex` formulas are written as `$…$` and `$$…$$`.

## HTML: `--format=html`

```sh
rayopdf --convert paper.pdf paper.html --format=html
```

One HTML5 page (no style, no script) with the blocks of the Markdown as elements:

- `<html lang>` from the PDF's language (`/Lang`) and `<title>` from its title;
- headings `<h1>`–`<h6>`, paragraphs `<p>`, code blocks `<pre><code>`;
- lists `<ul>` and `<ol start>`, nested by depth, a list item's further paragraphs inside it, task
  items with a disabled check box;
- block quotes `<blockquote>`; an alert's has the class `alert-note`, `alert-tip`,
  `alert-important`, `alert-warning` or `alert-caution`;
- tables as the HTML tables of the Markdown output (`--tables=html` spaces their cells);
- written images as `<img src alt>`, and the comments of the Markdown as HTML comments;
- footnotes at the end, in `<section class="footnotes">` as a numbered list whose items have the ids
  `fn-N` the references `<sup><a href="#fn-N">N</a></sup>` link to.

Inside a block the words are those of the Markdown, with tags in place of its markup:
`<strong>`, `<em>`, `<code>`, `<del>`, `<a href>`, `<sup>` and `<sub>`; text is HTML-escaped
and nothing else is. A link to a `javascript:`, `vbscript:` or `data:` target keeps its text
without the `href`. With `--math=latex` formulas are written as `\(…\)` and `\[…\]` for MathJax
or KaTeX; `--plain` leaves out the inline tags.

## Layout view: `--format=inspect`

```sh
rayopdf --convert paper.pdf paper.html --format=inspect --images=none
```

One HTML file (a style block, no script) that shows what the layout analysis decided. Each page
is drawn as SVG in PDF points:

- every line's text where it stands on the page; running headers, footers and page numbers that
  were left out are drawn grey and struck through;
- every block's boxes, coloured by type and numbered in reading order (hover over one to see its
  type and the start of its Markdown);
- a line through the blocks in output order;
- table cells and images.

At most 20,000 lines are drawn a page; a note says how many more there were.

## The report record

`--report` prints one JSON record of the document. `--batch --report` writes one a document,
and `--serve --json` answers each request with one.

```sh
rayopdf --convert paper.pdf paper.md --report
```

```json
{"input":"paper.pdf","output":"paper.md","status":"ok","code":0,"pages":12,"bytes":48211,"ms":9.8,"pages_without_text":0,"glyphs":61234,"unmapped_glyphs":0,...,"exact":"none","ocr_pages_routed":0,"ocr_pages":0,"ocr_failures":0,"ocr_addon_missing":false,...,"repaired":false,"limit_hits":0}
```

It starts with `input`, `output`, `status` (`ok`, `skipped` or `error`), `code` (the exit code
of this document), `error` (when it failed), `pages`, `bytes` (of output) and `ms`. The counters
that follow explain the output:

| Counter | What to look at |
|---|---|
| `pages_without_text` | Pages without a text layer. Each has a comment in the output; install the [OCR add-on](ocr.md). `ocr_addon_missing` is `true` when the add-on was needed and not found. |
| `ocr_pages_routed`, `ocr_pages`, `ocr_failures`, `ocr_tables` | Pages sent to the OCR add-on, pages it read, pages it failed on, and images it read as tables. |
| `glyphs`, `unmapped_glyphs` | Glyphs on the pages, and glyphs whose font maps them to no text. Many unmapped glyphs mean missing or wrong text; the add-on reads their shapes. |
| `repaired`, `broken_objects`, `failed_streams`, `content_errors`, `limit_hits` | The file was damaged, or a safety limit was reached; the output starts with a comment. |
| `images`, `images_skipped` | Images written, and images not written (undecodable, too small, or decoration). |
| `structure` | `used` when a tagged PDF's structure tree decided the blocks; other values (`none`, `broken`, `role-cycle`, `coverage`, `generic`, `order`) mean layout analysis did. |
| `exact` | Whether the RayoMD source of the PDF was used ([command-line guide](cli.md#pdfs-made-by-rayomd)). |
| `header_footer_lines` | Running headers, page numbers and similar lines left out (0 with `--furniture=keep`). |
| `headings`, `paragraphs`, `list_items`, `code_blocks`, `tables`, `links`, `footnotes` | How many of each were written. |
