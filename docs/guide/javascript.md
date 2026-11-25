# JavaScript (WebAssembly)

The npm package `rayopdf` is RayoPDF's converter built to WebAssembly with Emscripten. It runs in
browsers and in Node.js 18 or newer, as an ES module, and gives the same output as the command
line.

## Install

The package is attached to each [release](https://github.com/Butterski/rayopdf/releases/latest)
as `rayopdf-VERSION.tgz`; it is not on the npm registry. Install it from the file:

```sh
npm install ./rayopdf-0.5.0.tgz
```

## Convert

```js
import { readFileSync } from "node:fs";
import { convert, version, RayoPdfError } from "rayopdf";

const pdf = new Uint8Array(readFileSync("paper.pdf"));
const result = await convert(pdf);
console.log(result.text);                                   // the Markdown

const { data } = await convert(pdf, { format: "chunks", chunkChars: 2000 });
console.log(data.length, "chunks");
```

The same from the command line:

```sh
node --input-type=module -e 'import { readFileSync } from "node:fs"; import { convert } from "rayopdf";
console.log((await convert(new Uint8Array(readFileSync("paper.pdf")))).text)'
```

`convert(pdf, options)` takes the PDF as a `Uint8Array`. The options are those of the
[Python package](python.md#convert) in camelCase:

| Option | Values |
|---|---|
| `format` | `"markdown"` (default), `"json"`, `"chunks"`, `"inspect"` |
| `math` | `"unicode"` (default), `"latex"` |
| `pages` | `3`, `"2-5"`, `"-4"`, or `[first, last]` with `null` for an open end |
| `password` | the user or owner password |
| `images` | `true` (images linked as `images/NAME`) or the folder name the Markdown links them under |
| `pageMarkers`, `frontMatter`, `plain` | `true` turns them on |
| `furniture` | `"drop"` (default), `"keep"` |
| `tables` | `"auto"` (default), `"html"`, `"pipe"`, `"pipe-latex"` |
| `quotes` | `"keep"` (default), `"ascii"` |
| `smallCaps` | `"lower"` (default), `"keep"` |
| `figureText` | `"keep"` (default), `"drop"` |
| `chunkChars` | 256 to 1,000,000 bytes a chunk (default 4,000) |
| `structure`, `annotations`, `exact` | `true` (default); `false` is `--no-structure`, `--no-annotations`, `--no-exact` |

The [command-line guide](cli.md#options) explains each option, and
[output formats](formats.md) what each format holds.

`convert` resolves to `{ text, data, report, pages, images }`:

| Field | Value |
|---|---|
| `text` | The Markdown, or the JSON, JSON Lines or HTML of `format`. |
| `data` | The JSON object for `"json"`, the list of chunks for `"chunks"`, else `null`. |
| `report` | The counters of `--report` ([report record](formats.md#the-report-record)). |
| `pages` | The pages converted. |
| `images` | A `Map` from file name to bytes (`Uint8Array`); empty unless `images` is set. |

A failure rejects with a `RayoPdfError` whose `status` is the command line's exit code (2 usage,
3 unreadable, 10 corrupt, 11 password required, 12 unwritable, 13 unsupported encryption).
`version()` resolves to the converter's version. Types are in `rayopdf.d.ts`.

## Differences from the native program

The WebAssembly build runs on one thread and without SIMD, so it is slower than the native
program; its output is the same. It cannot start the OCR add-on: `ocr: "auto"` and `"force"`
find none, and pages without a text layer keep their comment.
