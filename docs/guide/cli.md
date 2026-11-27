# Command-line guide

`rayopdf` converts PDF files to Markdown, JSON, chunks for retrieval or an HTML layout view. This
page explains every command and option. [Output formats](formats.md) describes what each format
contains. `rayopdf --help` prints a short version of this page.

- [Commands](#commands)
- [Converting one PDF](#converting-one-pdf)
- [Options](#options)
- [Options for a reader's conventions](#options-for-a-readers-conventions)
- [PDFs made by RayoMD](#pdfs-made-by-rayomd)
- [A folder of PDFs: `--batch`](#a-folder-of-pdfs---batch)
- [A long-running process: `--serve --json`](#a-long-running-process---serve---json)
- [Document properties: `--inspect`](#document-properties---inspect)
- [Exit codes](#exit-codes)

## Commands

```sh
rayopdf --convert IN.pdf OUT.md [options]
rayopdf --batch IN-FOLDER OUT-FOLDER [options] [--recursive] [--skip-unchanged] [--report[=FILE]]
rayopdf --serve --json [options] [--workers=N]
rayopdf --inspect IN.pdf [--password=PW]
rayopdf --version | --help
```

Options go after the paths, in any order. On Windows the program is `rayopdf.exe`; paths and
arguments are UTF-8. The output is byte for byte the same on every platform: Linux x64 and arm64,
macOS arm64 and x64, and Windows x64. On macOS a program downloaded with a browser is refused by
Gatekeeper (it is not signed by a registered developer) until its quarantine flag is removed:
`xattr -d com.apple.quarantine rayopdf`; one downloaded with `curl` runs as it is.

## Converting one PDF

```sh
rayopdf --convert report.pdf report.md
```

The command prints nothing when it succeeds and exits with code 0. It writes `report.md` and the
document's images to `report-images/` beside it, linked from the Markdown as
`report-images/p<page>-<n>.<ext>`. An image is written as PNG, or as the PDF's own JPEG or
JPEG 2000 data when that data shows what the PDF shows.

The output `-` writes to standard output. Then no images are written unless `--images=DIR`
names a folder:

```sh
rayopdf --convert report.pdf - | less
```

The same file and the same options always give the same bytes.

The input `-` reads the PDF from standard input, to its end, and converts it as the file with
the same bytes; messages call it `standard input`, and the `--report` record's `input` is `-`:

```sh
curl -s https://example.org/report.pdf | rayopdf --convert - report.md
```

`--batch`, `--serve` and the developer option `--bench` read files only.

## Options

### Pages, passwords and images

| Option | Effect |
|---|---|
| `--pages=A-B` | Convert pages A to B only. Pages count from 1; `--pages=3`, `--pages=3-` and `--pages=-7` work too. |
| `--password=PW` | The user or owner password of an encrypted PDF. Without it, an encrypted PDF that needs one stops with exit code 11. Certificate encryption is not supported (exit code 13). |
| `--images=DIR` | Write image files to DIR, linked from the Markdown relative to the output file. |
| `--images=none` | Write no image files. The Markdown then has no image links. |

### Output format

| Option | Effect |
|---|---|
| `--format=markdown` | GitHub Flavored Markdown (the default). |
| `--format=json` | One JSON object: the metadata, the pages' sizes and every block with its type, page, position and Markdown. |
| `--json-detail=lines` | With JSON: each block also lists its lines, each with its page, box and text. `--json-detail=words` adds each line's words with their boxes; `blocks` (the default) gives neither. |
| `--format=chunks` | JSON Lines: one chunk for retrieval-augmented generation a line, with its heading path, pages, positions and Markdown. |
| `--chunk-chars=N` | With chunks: at most N bytes of Markdown a chunk, 256 to 1,000,000 (default 4,000). A longer block is split. |
| `--chunk-overlap=N` | With chunks: each chunk begins with up to N bytes of the chunk before it, from a word start, and its `overlap` says how many (0 to 500,000, at most half of `--chunk-chars`; default 0). |
| `--chunk-by=page` | With chunks: a chunk also ends at every page (default `size`: by size and headings only). |
| `--format=inspect` | One HTML file that draws each page: its lines, its blocks numbered in reading order, removed headers and footers struck through, table cells and images. Open it in a browser. |
| `--format=text` | Plain text without markup: headings and paragraphs as their text, list markers kept, code without fences, a table one row a line with its cells separated by tabs, footnotes `[n] text` at the end. Pages are separated by form feeds. |
| `--format=html` | The document as one HTML5 page: headings, paragraphs with their emphasis, code and links, nested lists, code blocks, block quotes, tables, images and footnotes; `lang` and `<title>` from the PDF's metadata. |
| `--page-markers` | Markdown only: write `<!-- page N -->` before each page's Markdown. |
| `--front-matter` | Markdown only: start with YAML front matter from the PDF's metadata (title, author, dates, language, page count). |

[Output formats](formats.md) shows each format with an example.

### Formulas: `--math`

`--math=unicode` (the default) writes formulas as Unicode text. A raised or lowered part of a
formula uses Unicode superscript and subscript characters (`mc²`, `x₀`), or `<sup>`/`<sub>`
when one of its characters has no such form. Raised or lowered ordinary text
(`H<sub>2</sub>O`) uses `<sup>`/`<sub>` too.

`--math=latex` writes formulas as LaTeX, rebuilt from the positions of their glyphs: inline
formulas as `$…$`, displayed formulas as `$$…$$`, followed by `(n)` when the formula has an
equation number. The rebuild handles fractions, roots, scripts (also a script's own script,
drawn apart from its line), limits, matrices, cases, binomials, `\overset`/`\underset` and
`\overbrace`/`\underbrace`. Some details:

- A formula whose accents the first reading cannot attach (an accent with no letter under it,
  accents stacked on one letter) is read again as the cheapest tree joining its glyphs by
  baselines, scripts, fraction bars and roots, unless it is a paragraph's whole text without
  being set as a display.
- A formula that neither reading can rebuild stays Unicode text.
- A number in a table cell (`−0.1`, `308±6`) stays text, as in the default mode.
- A `$` in the text is written `\$` where it could start a formula; `$8,000` stays as it is.
- In a tagged PDF, a formula whose alternate text is LaTeX (a command, `^` or `_`, balanced
  braces) is written as that text, as `$$…$$` when the formula is a block of its own.

Formulas are read from the glyphs of the text layer. On scanned pages they stay text.

### Reading the PDF

| Option | Effect |
|---|---|
| `--no-structure` | Ignore the structure tree of a tagged PDF and use layout analysis only. By default a structure tree that passes RayoPDF's checks decides the blocks. |
| `--annotations=MODE` | `all` (default): link annotations become Markdown links, and the text of form fields and FreeText annotations is read where it stands on the page, as a viewer shows it (hidden ones left out); a check box or radio button becomes a task box (`- [x]`, `- [ ]`), a password field is never read. With `--format=json` the form's fields and the markup annotations (highlights, comments, …) are also listed as data ([output formats](formats.md#json---formatjson)). `links`: links only, as 1.1 read them. `none` (also `--no-annotations`): no annotation is read. |
| `--no-exact` | Convert the pages even when the PDF carries its RayoMD source ([below](#pdfs-made-by-rayomd)). |

### OCR

| Option | Effect |
|---|---|
| `--ocr=auto` | The default: when the OCR add-on is installed, use it for pages and images whose text the PDF lacks. |
| `--ocr=off` | Never start the add-on. |
| `--ocr=force` | Also OCR pages that already carry an invisible OCR text layer, replacing that layer. |
| `--ocr-addon=PATH` | The add-on to run. By default: the `RAYOPDF_OCR` environment variable, then `rayopdf-ocr` beside `rayopdf`, then `PATH`. |

The [OCR guide](ocr.md) explains the add-on.

### Diagnostics and threads

| Option | Effect |
|---|---|
| `--report` | Print one JSON record of the document to standard output: status, pages, size, time and the counters behind the `<!-- rayopdf: ... -->` comments. [Output formats](formats.md#the-report-record) lists the fields. |
| `--workers=N` | 1 to 64. With `--convert`: how many pages of the document are read at once (default: one a CPU, at most 4; a thread starts only when the pages read so far are worth it). With `--batch` and `--serve`: how many documents are converted at once. The output never depends on N. |

## Options for a reader's conventions

Readers and benchmarks disagree on some details of Markdown: whether page headers belong in the
text, whether tables should be HTML, how quotation marks should look. RayoPDF's default output
stays faithful to the page. These options adapt it to a reader that expects something else.
Each is off by default.

| Option | Effect | Default |
|---|---|---|
| `--plain` | No inline markup: no emphasis, code spans, strikethrough or links (the link text stays), no `<sup>`/`<sub>` tags (their text stays, and so do Unicode script forms such as `x₀`). A footnote reference is its bare number (`text.1`) and a footnote is written `1 text`. Headings, lists, tables and escapes are unchanged. | inline markup written |
| `--tables=html` | Every table as an HTML `<table>`, one row a line, with a space between tags and around each cell's text (`<tr> <td> 12 </td> </tr>`), as HTML pretty-printers write them. | `--tables=auto` |
| `--tables=pipe` | Every table as a pipe table, for readers that take no HTML. A cell that spans rows or columns is written in its first row and column, the cells it covers stay empty, and header rows after the first become body rows. | `--tables=auto` |
| `--tables=pipe-latex` | The pipe tables of `--tables=pipe`, for a reader that passes each cell through LaTeX (READoc's evaluator does): `%`, `#` and `&` outside `$…$` math and code spans are written as the text `\%`, `\#`, `\&` (in the Markdown `\\%`). A Markdown reader shows the backslash. | `--tables=auto` |
| `--furniture=keep` | Keep running headers, footers and page numbers as text, in paragraphs of their own and never as headings: those in the upper half of a page before its text, the others after it (and after its footnotes). A tagged PDF whose structure tree is used still leaves out what the tree marks as artifacts. | `--furniture=drop`: left out |
| `--quotes=ascii` | Write typographic quotation marks (‘ ’ ‚ ‛ “ ” „ ‟) as `'` and `"`. Dashes and other punctuation stay as printed. | `--quotes=keep`: as printed |
| `--small-caps=keep` | Write a heading set in small capitals as printed, every letter a capital (`## V. CONCLUSION`). | `--small-caps=lower`: the source's case (`## V. Conclusion`) |
| `--figure-text=drop` | Leave out a figure's own text: on a page with a figure caption, the blocks that stand in or beside a drawn figure or image (tick labels, axis titles, legends, a diagram's labels) and do not read as prose. Captions, subfigure captions (`(b) …`) and sentences near a figure stay. | `--figure-text=keep`: written as paragraphs |

By default (`--tables=auto`) a table is a pipe table, unless a cell spanning rows, or spanning
columns below the header rows, needs HTML; that HTML has no extra spaces.

Example, for a reader that wants page headers kept, plain text and HTML tables:

```sh
rayopdf --convert paper.pdf paper.md --plain --tables=html --furniture=keep
```

The [benchmarks page](../benchmarks.md#the-benchmarks-conventions) lists the options each
benchmark is measured with, and why.

## PDFs made by RayoMD

A PDF that [RayoMD](https://github.com/Butterski/rayomd) exported with `--embed-source` carries
its Markdown source (the `rayomd-source/1` profile). RayoPDF then writes that Markdown byte for
byte instead of converting the pages, after two checks:

1. The payload's own checks: its object layout, length, SHA-256 digest and UTF-8.
2. A comparison with the pages: the letters and digits of the source, compared in runs of 8 with
   those of a normal conversion, may miss or add at most 32 plus 20 % of either side's (to allow
   for link targets, the TeX commands of formulas and comments).

The digest detects damage, not forgery, and the comparison does not cover punctuation, Markdown
syntax or link targets beyond that allowance. The payload carries no images: the source's image
links stay as written and no image files are written.

The `exact` field of the [report record](formats.md#the-report-record) says what happened:

| `exact` | Meaning |
|---|---|
| `used` | The source was written. |
| `none` | The PDF carries no source. |
| `off` | `--no-exact` was given. |
| `pages` | Not used because `--pages` selects part of the document. |
| `format` | Not used because an option needs the pages' layout: `--format=json`, `chunks`, `inspect`, `text` or `html`, `--page-markers`, `--plain`, any `--tables` other than `auto`, `--furniture=keep`, `--quotes=ascii`, `--small-caps=keep` or `--figure-text=drop`. |
| `unsupported`, `corrupt`, `integrity`, `limit`, `utf8`, `mismatch` | The payload was rejected for this reason, and the pages were converted. |

`--no-exact` always converts the pages.

## A folder of PDFs: `--batch`

```sh
rayopdf --batch papers md-out --recursive --skip-unchanged --report=report.jsonl --workers=8
```

`--batch` converts every `*.pdf` of a folder (any case of `.pdf`) to `OUT-FOLDER/NAME.md`, and
its images to `OUT-FOLDER/NAME-images/`. With `--format=json` the file is `NAME.json`, with
`--format=chunks` `NAME.jsonl`, with `--format=inspect` or `html` `NAME.html`, with
`--format=text` `NAME.txt`.

| Option | Effect |
|---|---|
| `--recursive` | Include subfolders (not hidden ones), mirrored under OUT-FOLDER. |
| `--skip-unchanged` | Keep output files that are at least as new as their PDF. |
| `--report` / `--report=FILE` | One JSON record a document, in the sorted order of the inputs, to standard output or to FILE. |
| `--workers=N` | Documents converted at once, 1 to 64 (default: one a CPU, at most 6). |

Every option of `--convert` applies to each document, except that `--images` takes only `none`.
The files and records do not depend on the number of workers; only each record's `ms` does.
Inputs are taken in the order of their UTF-8 paths. When two inputs would write the same output
(`a.pdf` and `a.PDF`), the second is refused with code 12. The exit code is 0 when every
document converted, else the code of the last one that failed, in input order; standard error
names each failure.

## A long-running process: `--serve --json`

`--serve --json` keeps one process running for an editor plugin, an agent tool or a script, and
saves the start-up time of a process for each document. It speaks protocol 1 of
[RayoMD](https://github.com/Butterski/rayomd): each line in is a JSON request, each line out is
the request's record.

```sh
printf '%s\n' '{"id":"1","input":"a.pdf","output":"out/a.md","args":["--pages=1-3"]}' \
  '{"id":2,"input":"b.pdf"}' | rayopdf --serve --json
```

- The first line out is `{"rayopdf":"VERSION","protocol":1}`.
- A request has `input` (a PDF path) and optionally `id` (a string or an integer, sent back as it
  came), `output` and `args`.
- With `output`, the converted file is written there, with its images beside it as with
  `--convert`. Without `output`, the answer carries the result and no images are written: the
  Markdown as `markdown`, with `--format=json` the JSON object as `json`, with `--format=chunks`
  the chunks as the array `chunks`, with `--format=inspect` or `html` the HTML as `html`, with
  `--format=text` the text as `text`.
- `args` holds options of `--convert` (such as `--pages`, `--password`, `--format`, `--math` or
  `--plain`), applied over the options the session started with. Batch options are refused.
- A malformed request gets an error record with code 2, and the session goes on. A line is at
  most 1 MiB, and `args` at most 64 options.
- `quit`, `exit` or the end of the input ends the session.
- With `--workers=N`, requests already sent are converted N at a time; the answers still come in
  request order. The default is 1.

The record has the fields of the [report record](formats.md#the-report-record) plus `id`.

## Document properties: `--inspect`

```sh
rayopdf --inspect file.pdf [--password=PW]
```

`--inspect` prints the document's properties as one JSON object, without converting it:
`file`, `bytes`, `version`, `pages`, `objects`, `encrypted`, `repaired`, `linearized`,
`xref_sections`, `title`, `producer` and `creator` (when the PDF has them), `tagged`,
`first_page` (its `width`, `height` and `rotate`) and `broken_objects`. The input `-` reads the
PDF from standard input, as with `--convert`; `file` is then `-`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Converted. |
| 2 | Usage error: an unknown option or an invalid value. |
| 3 | The input cannot be read: missing, not a file, larger than 4 GiB, or a failed read of standard input. |
| 10 | Not a PDF, corrupt beyond repair, or past a document limit. |
| 11 | Password required: the PDF is encrypted and no password, or a wrong one, was given. |
| 12 | An output could not be written. |
| 13 | Unsupported encryption: a security handler other than the standard one (revisions 2 to 6). |

A converted document can still have parts without text. Check the `<!-- rayopdf: ... -->`
comments in the output, or the counters of `--report` ([output formats](formats.md#the-report-record)).

`rayopdf` also has two developer options, `--bench` and `--features`, for performance
measurements and model training; `rayopdf --help` lists them.
