# RayoPDF JSON output, schema 1

`rayopdf --convert in.pdf out.json --format=json` (and a `--serve --json` request with
`"args":["--format=json"]` and no `output`, answered in the record's `"json"` field) writes one
JSON object on one line, ending in a line break:

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

## Top level

| Field | Value |
|---|---|
| `rayopdf` | the version that wrote it |
| `schema` | `1`; a later schema that changes a field's meaning gets a new number |
| `repaired` | `true` when the file was damaged and repaired (text may be missing) |
| `metadata` | the document information that is not empty: `title`, `author`, `subject`, `keywords`, `creator`, `producer`, `created`, `modified` (dates as `YYYY-MM-DDTHH:MM:SS` with `Z` or `±HH:MM` when the PDF gives an offset; a date that does not parse stays as the PDF wrote it) |
| `pages` | one object a converted page: `page` (1-based number in the file), `width`, `height` (points) |
| `outline` | only when the PDF has bookmarks: `{"level","title","page"}` an entry, depth first (`level` 1 at the top; `page` the page it leads to, `null` when none; entries outside `--pages` too) |
| `fields` | only when the PDF has form fields with a widget on a converted page, and only with `--annotations=all` (the default): one object a field, in the form's field order: `name` (full name, partial names joined with `.`), `type` (`text`, `checkbox`, `radio`, `choice`, `signature`, `button`), `value` (`null` when it has none and always for passwords, signatures and pushbuttons; `true`/`false` for a check box; a list for a choice of several), `page`, `box` (`[x0,y0,x1,y1]`, its first widget on a converted page), `read_only`, `required` |
| `annotations` | only when a converted page has markup annotations shown on screen, and only with `--annotations=all`: one object an annotation, page by page in the page's order: `page`, `type` (`highlight`, `underline`, `strikeout`, `squiggly`, `note`, `freetext`), `boxes` (`[[x0,y0,x1,y1],...]`: each quad's bounding box, else its rectangle), `contents`, `author`, `date`, and for the four text markups `text` (the page text its boxes cover); `null` for what it lacks. The Markdown does not change. |
| `blocks` | the content in reading order |

## A block

Every block has, in this order:

| Field | Value |
|---|---|
| `type` | `heading`, `paragraph`, `list_item`, `code`, `table`, `image`, `footnote` or `comment` |
| `page` | the page it starts on |
| `boxes` | one `{"page":N,"bbox":[x0,y0,x1,y1]}` a page it covers: the union of its lines, in page points, origin at the bottom left, two decimals (turned text: its box on the page; a table: the table's box; an image: the image's box; a comment: `[]`) |
| `role` | only on some blocks: `caption` (a figure's or table's caption; tagged PDFs: their `/Caption`), `formula` (a displayed formula, `--math=latex`), `page_header`, `page_footer`, `page_number` (furniture kept with `--furniture=keep`) |

Then by type:

| Type | Fields |
|---|---|
| `heading` | `level` (1-6) |
| `list_item` | `depth` (the nesting depth), `ordered` (`true`/`false`), `number` (ordered items only), `task` (`"open"` or `"done"`, task items only) |
| `table` | `rows`, `cols`, `header_rows`, `cells`: `{"row","col","row_span","col_span","text"}`, `text` the cell's inline Markdown (also when the table is written as HTML) |
| `image` | `file` (the link target, or `null` when the image was not written), `alt` |
| `footnote` | `number` |

Any block may also have `quote` (the block-quote depth, when above 0) and, on the first block
of an alert, `alert` (`note`, `tip`, `important`, `warning` or `caution`).

With `--json-detail=lines` (or `words`) every block also has `lines`, just before `markdown`:
one `{"page","box":[x0,y0,x1,y1],"text"}` a line in reading order (a table: its cells' lines,
cell by cell; an image or a comment: `[]`). `text` is the line as printed: words joined by one
space, no markup, no list marker, no inline image; a hyphen at a line's end stays.
`--json-detail=words` adds `"words":[{"box","text"}...]` to each line. The default
(`--json-detail=blocks`) writes no `lines`.

Last, every block has `markdown`: exactly the bytes `--format=markdown` writes for it. The
blocks' `markdown` values occur in the Markdown output in the same order, so a consumer can
locate each block in the Markdown. A `comment` block is one of the converter's
`<!-- rayopdf: ... -->` comments (a page without text, an image not extracted).

`--front-matter` and `--page-markers` do not change JSON output: `metadata` and each block's
`page` carry the same information. `--plain`, `--tables=html`, `--tables=pipe` and
`--furniture=keep` do: each block's `markdown` and table cells are written as the Markdown output
writes them with those options, and kept page furniture appears as blocks.
