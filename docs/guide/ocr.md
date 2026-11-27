# OCR add-on

RayoPDF reads the text layer of a PDF. Some pages have none: scans, text drawn inside images, or
text drawn as outlines. The optional OCR add-on reads those pages. It is a separate program,
`rayopdf-ocr`, shipped in its own archive with its models, and is never built into `rayopdf`.

## Install

The add-on is built for Linux x64 and Windows x64 only. On Linux arm64 and macOS, `rayopdf`
runs without it: pages that need OCR are converted without their text, as when the add-on is
missing.

1. Download `rayopdf-ocr-VERSION-linux-x64-static.tar.gz` or `rayopdf-ocr-VERSION-windows-x64.zip`
   from [Releases](https://github.com/Butterski/rayopdf/releases/latest). It holds the
   `rayopdf-ocr` program and its models (`rayopdf-ocr-models/`: PP-OCRv6 small and tiny on
   [ncnn](https://github.com/Tencent/ncnn), about 7 MB plus 36 MB).
2. Unpack it into the same folder as the `rayopdf` archive. Its folder has the same name, so
   `rayopdf-ocr` lands beside `rayopdf`.

That is all: `rayopdf` finds the add-on beside itself and starts it only when a page needs it.
Pages with a usable text layer never wait for it.

To keep the add-on elsewhere, name it with `--ocr-addon=PATH` or the `RAYOPDF_OCR` environment
variable. Without either, `rayopdf` looks beside itself, then on `PATH`.

## What it reads

- Pages without a text layer, in Latin and CJK scripts.
- Pages scanned sideways or upside down: they are turned upright first.
- Glyphs whose font maps them to no text or to wrong text: their shapes are read.
- Text inside images. When images are written, the text read in an image becomes its
  alternate text, and an image that holds a table filling it becomes that table.

The optional script pack `rayopdf-ocr-scripts-VERSION-<platform>` (about 21 MB), unpacked over
the add-on, adds Cyrillic, Korean, Arabic, Devanagari, Tamil and (roughly) Kannada. A script's
model loads only when a page needs it. Hebrew is not read yet.

Without the add-on, a page without text keeps the comment
`<!-- rayopdf: page N has no extractable text -->` in the output.

## Options

```sh
rayopdf --convert scan.pdf scan.md --report      # ocr_pages_routed, ocr_pages, ocr_failures
rayopdf --convert scan.pdf scan.md --ocr=force   # also re-read pages with an invisible OCR layer
rayopdf --convert a.pdf a.md --ocr=off           # never start the add-on
```

| Option | Effect |
|---|---|
| `--ocr=auto` | The default: use the add-on, when it is installed, for pages and images whose text the PDF lacks. |
| `--ocr=off` | Never start the add-on. |
| `--ocr=force` | Also OCR pages that already carry an invisible OCR text layer, replacing that layer. |
| `--ocr-addon=PATH` | The add-on to run. |

The [report record](formats.md#the-report-record) counts the pages sent to the add-on
(`ocr_pages_routed`), those it read (`ocr_pages`), those it failed on (`ocr_failures`), images
read as tables (`ocr_tables`), and whether the add-on was needed but not found
(`ocr_addon_missing`).

## Speed and memory

An OCR page takes seconds, not milliseconds: about 2 s on one core for a 300 dpi page of Latin
text, more for other scripts. When a page needs it, the add-on uses about 0.6 GB of memory.

## Where it stands

On the synthetic scanned pages of RayoPDF's benchmarks, RayoPDF with the add-on reads text with
a similarity (NID) of 0.918, against 0.856 for Tesseract and 0.754 for docling with OCR. On
olmOCR-bench's old scans it passes 0.213 of the tests, below docling with OCR (0.251). Details:
[benchmarks](../benchmarks.md#scanned-pages).

The container image, the Python wheels and the npm package hold no add-on. In Python, OCR is off
by default; with `ocr="auto"` the package finds an installed add-on as the command line does, or
takes `ocr_addon=PATH`. The WebAssembly build cannot start one.

## Building it

The add-on builds in its own Docker image; see the
[OCR engine document](../development/ocr-engine.md#build-run-package). How the add-on and
`rayopdf` talk to each other is in the [OCR protocol](../development/ocr-protocol.md).
