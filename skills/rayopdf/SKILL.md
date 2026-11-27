---
name: rayopdf
description: Converts PDF to Markdown (or to structured JSON) with the rayopdf command-line tool (one small static binary, no Python, no GPU, about a millisecond a page), keeping headings, lists, tables, links, footnotes, formulas and images; scans through an optional OCR add-on. Use when the user wants the text of a PDF to read, quote, search, summarise or edit (a paper, report, manual, invoice, slide deck or a folder of them).
license: Apache-2.0
compatibility: Needs a shell and the rayopdf binary (Linux x64 or arm64, macOS arm64 or x64, or Windows x64 release, or the ghcr.io/butterski/rayopdf container); network access only to download it.
metadata:
  source: https://github.com/Butterski/rayopdf
---

# PDF to Markdown with RayoPDF

RayoPDF reads a PDF's text layer and writes Markdown itself: headings, paragraphs, lists, tables
(GitHub pipe tables, or HTML when cells span), links, footnotes, code blocks, formulas and
images. The same file and flags give the same bytes, and a damaged file is repaired as far as it
can be rather than refused. A page without a text layer (a scan) needs the OCR add-on (section 4).

## 1. Get the binary

```sh
rayopdf --version          # prints "rayopdf <version>"; on Windows: rayopdf.exe --version
```

If it is not on `PATH`, download the release asset for the platform from
https://github.com/Butterski/rayopdf/releases and check it against the SHA-256 that GitHub
lists for that asset (`digest` in the releases API, shown on the release page).

| Asset | Platform |
|---|---|
| `rayopdf-<version>-linux-x64-static.tar.gz` | Linux x64, no dependency |
| `rayopdf-<version>-linux-arm64-static.tar.gz` | Linux arm64 (aarch64), no dependency |
| `rayopdf-<version>-macos-arm64.tar.gz` | macOS 11 or newer on Apple silicon |
| `rayopdf-<version>-macos-x64.tar.gz` | macOS 10.15 or newer on Intel |
| `rayopdf-<version>-windows-x64.zip` | Windows x64, `rayopdf.exe` |

Linux and macOS (needs `curl`, `python3`, and `sha256sum` or `shasum`):

```sh
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) p=linux-x64-static ;; Linux-aarch64) p=linux-arm64-static ;;
  Darwin-arm64) p=macos-arm64 ;; Darwin-x86_64) p=macos-x64 ;;
  *) echo "no release for $(uname -s) $(uname -m)" >&2; exit 1 ;;
esac
api=$(curl -fsSL https://api.github.com/repos/Butterski/rayopdf/releases/latest)
set -- $(printf '%s' "$api" | python3 -c 'import json,sys
d=json.load(sys.stdin); v=d["tag_name"][1:]; n="rayopdf-%s-%s.tar.gz" % (v, sys.argv[1])
print(v, n, next(a["digest"] for a in d["assets"] if a["name"] == n).split(":")[1])' "$p")
v=$1 asset=$2 sum=$3
curl -fLO "https://github.com/Butterski/rayopdf/releases/download/v$v/$asset"
if command -v sha256sum > /dev/null; then check="sha256sum -c -"; else check="shasum -a 256 -c -"; fi
echo "$sum  $asset" | $check        # stop if this does not print "OK"
tar -xzf "$asset"
export PATH="$PWD/rayopdf-$v-$p:$PATH"
rayopdf --version
```

Windows (PowerShell):

```powershell
$r = Invoke-RestMethod https://api.github.com/repos/Butterski/rayopdf/releases/latest
$v = $r.tag_name.TrimStart('v'); $asset = "rayopdf-$v-windows-x64.zip"
$sum = ($r.assets | Where-Object name -eq $asset).digest -replace '^sha256:', ''
Invoke-WebRequest "https://github.com/Butterski/rayopdf/releases/download/v$v/$asset" -OutFile $asset
if ((Get-FileHash $asset -Algorithm SHA256).Hash -ne $sum) { throw "SHA-256 mismatch: $asset" }
Expand-Archive $asset -DestinationPath .
$env:PATH = "$PWD\rayopdf-$v-windows-x64;$env:PATH"
rayopdf.exe --version
```

With Docker and no binary, the container image `ghcr.io/butterski/rayopdf:<version>` (the
static binary and nothing else; `latest` names the newest release) takes the same arguments:

```sh
docker run --rm -v "$PWD:/w" ghcr.io/butterski/rayopdf:<version> --convert paper.pdf paper.md
```

Its working folder is `/w`, so keep the PDF and the output under the mounted folder. If the
Markdown cannot be written (exit code 12), add `-u "$(id -u):$(id -g)"`.

## 2. Convert

```sh
rayopdf --convert paper.pdf paper.md
```

The command prints nothing on success and exits 0. Images are written to `paper-images/` beside
`paper.md` and linked from it. Output `-` writes the Markdown to standard output (then no images
are written):

```sh
rayopdf --convert paper.pdf -
```

Input `-` reads the PDF from standard input (1.2.0 and later), for a PDF that is not a file:

```sh
curl -s https://example.org/paper.pdf | rayopdf --convert - paper.md
```

Options go after the output path, in any order:

| Ask | Arguments |
|---|---|
| Some pages only | `--pages=3-7` (1-based; `5`, `5-`, `-7` too) |
| Images elsewhere / none | `--images=DIR` / `--images=none` |
| Formulas as LaTeX (`$...$`, `$$...$$`) | `--math=latex` (default `unicode`: Unicode text, `<sup>`/`<sub>` where needed) |
| Structured JSON instead of Markdown | `--format=json` (schema: [references/json.md](references/json.md)) |
| Each block's lines (page, box, text), or lines and words, in the JSON | `--json-detail=lines` / `--json-detail=words` (default `blocks`) |
| Chunks for retrieval (RAG) | `--format=chunks` (JSON Lines: `chunk`, `heading_path`, `pages`, `boxes`, `blocks`, `markdown`); `--chunk-chars=N` bytes a chunk (256-1000000, default 4000); `--chunk-overlap=N` each chunk begins with up to N bytes of the one before (its `overlap` says how many); `--chunk-by=page` a chunk also ends at every page |
| See what the layout analysis decided | `--format=inspect` (one HTML file, no script: each page's lines, blocks numbered in reading order, removed furniture struck through, table cells, images) |
| Plain text without any markup | `--format=text` (headings and paragraphs as text, list markers kept, tables a row a line with tab-separated cells, footnotes `[n] text` at the end, pages separated by form feeds) |
| The document as an HTML page | `--format=html` (one HTML5 file: `<h1>`-`<h6>`, `<p>` with `<strong>`/`<em>`/`<code>`/`<a href>`, nested lists, `<pre><code>`, tables, `<img>`, footnotes last; `lang` and `<title>` from the PDF; `--math=latex` writes `\(…\)`/`\[…\]`) |
| A comment before each page's text | `--page-markers` (`<!-- page N -->`; a list or quote across a page break is split there) |
| The PDF's metadata as YAML front matter | `--front-matter` (`title`, `author`, `subject`, `keywords`, `creator`, `producer`, `created`, `modified`, `language`, `pages`) |
| Text without inline markup | `--plain` (no `**`, `*`, `` ` ``, links, `<sup>`/`<sub>` or `[^n]`; their text stays) |
| Every table as HTML | `--tables=html` (default `auto`: pipe tables unless a span needs HTML) |
| Every table as a pipe table, spans flattened (readers without HTML) | `--tables=pipe` |
| Keep running headers, footers and page numbers | `--furniture=keep` (default `drop`) |
| Encrypted PDF | `--password=PW` |
| Ignore the tags of a tagged PDF | `--no-structure` (layout analysis only) |
| Links as plain text, no form-field values | `--annotations=none` (`links`: links only, without form-field values and FreeText notes) |
| Re-read a PDF RayoMD made from Markdown | `--no-exact` (by default its embedded, verified Markdown source is written as it is) |
| OCR | `--ocr=auto` (default), `off`, `force`; `--ocr-addon=PATH` (section 4) |

`--format=json`, `--page-markers` and `--front-matter` are not in releases before 0.2.0,
`--plain`, `--tables`, `--furniture`, `--format=chunks` and `--format=inspect` not before
0.3.0, and `--json-detail`, `--format=text`, `--format=html`, `--chunk-overlap` and `--chunk-by` not before 1.2.0: check `rayopdf --help` lists them.
`--front-matter` and `--page-markers` have no effect with `--format=json`, whose `metadata` and
per-block `page` carry the same information.

Examples:

```sh
rayopdf --convert paper.pdf paper.md --math=latex
rayopdf --convert report.pdf report.json --format=json --images=none
rayopdf --convert scan.pdf scan.md --pages=1-3 --page-markers --front-matter
rayopdf --convert locked.pdf locked.md --password=secret
```

### Many documents

For a folder: `rayopdf --batch <in-folder> <out-folder> [flags] --report` (`--recursive`,
`--skip-unchanged`, `--report=FILE`, `--workers=N`). Each `a.pdf` becomes `a.md` (`a.json` with
`--format=json`, `a.jsonl` with `--format=chunks`, `a.html` with `--format=inspect` or `html`, `a.txt` with `--format=text`), and its
images go to `a-images/` beside it (`--images=none` for none; the batch modes refuse
`--images=DIR`).

For many conversions from one process, `rayopdf --serve --json [flags]` reads one JSON request a
line on stdin and answers one JSON line a request, in order, with the fields of the report below
and the request's `id`. Its first output line is `{"rayopdf":"<version>","protocol":1}`.

```sh
rayopdf --serve --json --math=latex < requests.jsonl
```

Request lines (`input` a PDF; `output` the Markdown file to write, or left out to get the
Markdown in the answer's `"markdown"` field, or with `--format=json` the JSON document in its
`"json"` field, with `--format=chunks` the chunks as an array in its `"chunks"` field, with
`--format=inspect` or `html` the HTML in its `"html"` field, with `--format=text` the text in its `"text"` field; `args`
flags over the command line's: `--pages`, `--password`, `--math`,
`--format`, `--images=none`, ...):

```json
{"id":1,"input":"a.pdf","output":"a.md"}
{"id":2,"input":"b.pdf","args":["--pages=1-2","--math=latex"]}
{"id":3,"input":"c.pdf","args":["--format=json"]}
```

A line `quit` or the end of input ends the session.

## 3. Check the result

The exit code says whether the output was written: 0 converted; 2 bad arguments; 3 input not
readable (missing, not a file, over 4 GiB); 10 corrupt beyond repair (or not a PDF); 11 password
required (`--password=PW`); 12 output not writable; 13 unsupported encryption. A converted
document can still have parts without text. `--report` prints one JSON line with the counts:

```sh
rayopdf --convert paper.pdf paper.md --report
```

```json
{"input":"paper.pdf","output":"paper.md","status":"ok","code":0,"pages":12,"bytes":48211,"ms":9.8,"pages_without_text":0,"glyphs":61234,"unmapped_glyphs":0,"guessed_glyphs":0,"hidden_glyphs":0,"headings":14,"paragraphs":131,"list_items":22,"code_blocks":0,"tables":3,"links":9,"footnotes":4,"images":5,"images_skipped":0,"images_mask_dropped":0,"header_footer_lines":24,"content_errors":0,"structure":"none","structure_elements":0,"exact":"none","ocr_pages_routed":0,"ocr_pages":0,"ocr_failures":0,"ocr_addon_missing":false,"ocr_broken_pages":0,"ocr_vector_text_pages":0,"ocr_glyph_fonts":0,"ocr_glyphs":0,"ocr_tables":0,"repaired":false,"encrypted":false,"broken_objects":0,"salvaged_streams":0,"failed_streams":0,"limit_hits":0}
```

Check `status` is `"ok"` (`"error"`: `error` says why) and `pages` is plausible, then:

- `pages_without_text` > 0: pages with no text layer (scans, or text drawn as shapes). Each
  leaves `<!-- rayopdf: page N has no extractable text -->` in the Markdown. Install the OCR add-on
  (section 4); `ocr_addon_missing` `true` says it was needed and not found.
- `ocr_failures` > 0: the add-on failed on pages (`<!-- rayopdf: page N has no extractable text;
  OCR failed -->`).
- `unmapped_glyphs` high against `glyphs`: the fonts map glyphs to no text;
  `<!-- rayopdf: page N text is mostly unmapped glyphs -->` marks pages where most are. The add-on
  reads such glyphs' shapes; without it that text is missing or wrong.
- `repaired` `true`, `broken_objects`, `failed_streams`, `content_errors` or `limit_hits` > 0: the
  file is damaged; the Markdown starts with
  `<!-- rayopdf: the file was damaged and has been repaired; text may be missing -->`.
- The Markdown starts with `<!-- rayopdf: this PDF is an XFA form; its fields are not read -->`:
  the form's fields live only in its XFA data, which RayoPDF does not read; a viewer with XFA
  support (Adobe Reader) shows them.
- `images_skipped` > 0: images not written (undecodable, too small, or decoration); where one
  stood in the text, `<!-- rayopdf: image on page N not extracted (NAME) -->` marks it.
- `structure`: `used` when a tagged PDF's structure tree decided the blocks; other values (`none`,
  `broken`, `role-cycle`, `coverage`, `generic`, `order`) mean layout analysis did.
- `exact`: `used` when the PDF carried its RayoMD Markdown source and that was written as it is;
  `pages` when a page range kept it from being used; `format` when `--format=json`,
  `--page-markers`, `--plain`, `--tables=html`, `--tables=pipe` or `--furniture=keep` asked for
  the pages' layout; `off` with `--no-exact`; other values name why a payload was rejected.
- `header_footer_lines`: running heads, page numbers and similar lines left out of the text (0
  with `--furniture=keep`).

`rayopdf --inspect file.pdf [--password=PW]` prints the document's structure without converting
it (`version`, `pages`, `encrypted`, `repaired`, `tagged`, `title`, `producer`, `first_page` size,
`broken_objects`).

## 4. The OCR add-on

Scans, text inside images and text whose font maps to no text are read by a separate program,
`rayopdf-ocr`, in the release assets `rayopdf-ocr-<version>-linux-x64-static.tar.gz` and
`rayopdf-ocr-<version>-windows-x64.zip` (check their `digest` as in section 1; x64 only: Linux
arm64 and macOS have no add-on, so scanned pages give no text there). Unpack it into the
same folder as `rayopdf` (its folder has the same name, so it lands beside the binary); `rayopdf`
then finds it and starts it only for pages that need it. It reads Latin and CJK text; the
optional `rayopdf-ocr-scripts-<version>-<platform>` asset, unpacked over it, adds Cyrillic,
Korean, Arabic, Devanagari, Tamil and Kannada. Hebrew is not read.

```sh
rayopdf --convert scan.pdf scan.md --report      # ocr_pages_routed, ocr_pages, ocr_failures
rayopdf --convert scan.pdf scan.md --ocr=force   # also re-read pages with an invisible OCR layer
rayopdf --convert a.pdf a.md --ocr=off           # never start the add-on
```

`--ocr-addon=PATH` or the `RAYOPDF_OCR` environment variable name an add-on elsewhere (otherwise:
beside `rayopdf`, then `PATH`). An OCR page takes seconds (about 2 s on one core for a 300 dpi
Latin page), where a page with a text layer takes about a millisecond. The container image holds
no add-on.
