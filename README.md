<a id="readme-top"></a>

<div align="center">

<img src="docs/assets/branding/rayopdf.svg" alt="RayoPDF mascot" width="170">

# RayoPDF

**Fast, local PDF to Markdown and JSON, from one small executable.**<br>
No GPU, no Python or Java runtime, no models to download. The reverse of [RayoMD](https://github.com/Butterski/rayomd).

<a href="https://github.com/Butterski/rayopdf/releases"><img alt="Version: 1.1.0" src="https://img.shields.io/badge/version-1.1.0-informational?style=for-the-badge"></a>
<a href="LICENSE"><img alt="License: Apache-2.0" src="https://img.shields.io/badge/license-Apache--2.0-blue.svg?style=for-the-badge"></a>

<a href="https://github.com/Butterski/rayopdf/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/Butterski/rayopdf/actions/workflows/ci.yml/badge.svg"></a>
<a href="https://github.com/Butterski/rayopdf/actions/workflows/hygiene.yml"><img alt="Repository Hygiene" src="https://github.com/Butterski/rayopdf/actions/workflows/hygiene.yml/badge.svg"></a>
<img alt="C++17" src="https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white">
<img alt="Platforms: Windows and Linux" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-2ea44f">

</div>

RayoPDF reads a PDF and writes GitHub Flavored Markdown: headings, paragraphs, lists, tables,
code, links, footnotes, formulas and images, rebuilt from the page's layout. It can also write
JSON with every block's position on the page, chunks for retrieval-augmented generation, or an
HTML view of the page layout. It runs on your machine, converts a typical page in about a
millisecond, and is one static executable for Linux or Windows. Python, JavaScript and C/C++
programs can call it in-process.

## Status

Version 1.1.0, released on 2026-11-26. RayoPDF converts PDFs that have a text layer (most PDFs
made by software); scanned pages need the optional OCR add-on. Each release is checked for
quality, speed, memory use and robustness: see the
[release check of 1.1.0](docs/benchmarks/release-check-2026-11-26.md).

## Highlights

- **Fast:** a one-page PDF in under 1 ms, start to exit (median over opendataloader-bench's 200
  pages); over 2,000 pages a second on one core in a running process.
- **Small:** a 2.1 MB download; no runtime, no model files; 2 to 15 MiB of memory for most
  documents.
- **Structured:** headings, lists, tables, code, quotes, links, footnotes and images; tagged PDFs
  are read through their structure tree.
- **Math:** formulas as Unicode text, or rebuilt as LaTeX with `--math=latex`.
- **Scans:** an optional OCR add-on reads pages without text, in Latin, CJK and six more scripts.
- **Careful:** every parser is fuzz-tested, and text that could not be read is marked in the
  output, never dropped silently.

## Install

Download from [Releases](https://github.com/Butterski/rayopdf/releases/latest). Each archive
holds one folder with the program, its licences and this README's usage section.

```sh
# Linux x64: one static executable, no dependencies
curl -LO https://github.com/Butterski/rayopdf/releases/download/v1.1.0/rayopdf-1.1.0-linux-x64-static.tar.gz
tar -xzf rayopdf-1.1.0-linux-x64-static.tar.gz
./rayopdf-1.1.0-linux-x64-static/rayopdf --version
```

- **Windows x64:** unpack `rayopdf-1.1.0-windows-x64.zip` and run `rayopdf.exe`.
- **Python 3.9 or newer** (Linux x64 with glibc 2.28 or newer, Windows x64): `pip install rayopdf`.
  See the [Python guide](docs/guide/python.md).
- **JavaScript** (browsers and Node.js 18 or newer, WebAssembly): the npm package is attached to
  the release and is not on the npm registry: `npm install ./rayopdf-1.1.0.tgz`. See the
  [JavaScript guide](docs/guide/javascript.md).
- **Container image:** `docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/w" ghcr.io/butterski/rayopdf:1.1.0 --convert in.pdf out.md`.
  See the [container guide](docs/guide/container.md).
- **C and C++:** the `rayopdf-lib` archives hold a static library and its header. See the
  [C and C++ guide](docs/guide/cpp.md).
- **OCR add-on (optional):** unpack `rayopdf-ocr-1.1.0-<platform>` into the same folder as
  `rayopdf`. RayoPDF then finds `rayopdf-ocr` beside itself and starts it only for pages that
  have no usable text. It reads Latin and CJK text; the optional script pack adds Cyrillic,
  Korean, Arabic, Devanagari, Tamil and Kannada. OCR takes about 2 seconds a page, not
  milliseconds. See the [OCR guide](docs/guide/ocr.md).

| Release file | Size, 1.1.0 | For |
|---|---:|---|
| `rayopdf-<version>-linux-x64-static.tar.gz` | 2,139 KB | Linux: one static executable |
| `rayopdf-<version>-windows-x64.zip` | 2,138 KB | Windows: `rayopdf.exe` |
| `rayopdf-lib-<version>-<platform>` | 1,949 KB / 2,042 KB | C++ API for Linux / Windows: `librayopdf.a`, header, example |
| `rayopdf-<version>-py3-none-<platform>.whl` | 1,792 KB / 2,107 KB | Python on Linux / Windows |
| `rayopdf-<version>.tgz` | 1,303 KB | JavaScript (WebAssembly) |
| `rayopdf-ocr-<version>-<platform>` | 34 MB | The OCR add-on with its models |
| `rayopdf-ocr-scripts-<version>-<platform>` | 20 MB | Optional: more scripts for the OCR add-on |

To build from source (Linux with Docker and Python 3.10 or newer), see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Quick start

```sh
rayopdf --convert paper.pdf paper.md                   # Markdown; images in paper-images/
rayopdf --convert paper.pdf - --images=none            # Markdown to the terminal
rayopdf --convert paper.pdf paper.json --format=json   # every block with its page and position
rayopdf --convert paper.pdf paper.md --math=latex      # formulas as LaTeX
rayopdf --batch papers/ markdown/ --recursive          # every PDF of a folder
```

```python
import rayopdf
print(rayopdf.convert("paper.pdf").text)
```

## Usage

```sh
rayopdf --convert IN.pdf OUT.md [options]          # one PDF; OUT.md "-" writes to standard output
rayopdf --batch IN-FOLDER OUT-FOLDER [options]     # every *.pdf of a folder
rayopdf --serve --json [options]                   # a long-running process: JSON requests, one a line
rayopdf --inspect IN.pdf                           # the document's properties as JSON, no conversion
rayopdf --version | --help
```

Options go after the paths, in any order. The most used:

| Option | What it does |
|---|---|
| `--images=DIR` / `--images=none` | Write image files to DIR, or write none. Default: `OUT-images/` beside `OUT.md` (none with `-`). |
| `--pages=A-B` | Convert pages A to B only (1-based; `3`, `3-` and `-7` work too). |
| `--password=PW` | Open an encrypted PDF. |
| `--format=json` | One JSON object: the document's metadata and its blocks, each with its type, page, position and Markdown. `--json-detail=lines` or `words` adds each block's lines, or lines and words, with their positions. |
| `--format=chunks` | JSON Lines, one chunk for retrieval a line, each with its heading path and pages. `--chunk-chars=N` sets the size (default 4,000 bytes). |
| `--format=inspect` | One HTML file that draws each page's lines and blocks in reading order, to see what the layout analysis decided. |
| `--format=text` | Plain text without markup; pages separated by form feeds. |
| `--math=latex` | Formulas as LaTeX (`$…$`, `$$…$$`) instead of Unicode text. |
| `--page-markers` | Write `<!-- page N -->` before each page's Markdown. |
| `--front-matter` | Start the Markdown with YAML front matter from the PDF's metadata. |
| `--plain` | No inline markup: no emphasis, code spans, links or `<sup>`/`<sub>` (their text stays). |
| `--tables=html` / `--tables=pipe` | Every table as HTML, or every table as a pipe table. Default: pipe tables, and HTML only for a table whose cells span rows. |
| `--furniture=keep` | Keep running headers, footers and page numbers (removed by default). |
| `--ocr=off` / `--ocr=force` | Never start the OCR add-on, or also OCR pages that already carry an invisible OCR text layer. |
| `--report` | Print one JSON record with the document's counters (pages, repairs, OCR, …). |
| `--workers=N` | Threads (1 to 64). The output never depends on it. |

`rayopdf --help` lists every option. More options adapt the output to a reader's conventions
(`--quotes`, `--small-caps`, `--figure-text`, `--tables=pipe-latex`), and `--batch` takes
`--recursive`, `--skip-unchanged` and `--report=FILE`. The
[command-line guide](docs/guide/cli.md) explains each of them, and the
[output formats guide](docs/guide/formats.md) describes what is written.

Text that could not be read is never dropped silently: a `<!-- rayopdf: ... -->` comment marks
the place, for example a page without a text layer. Exit codes: 0 converted, 2 usage error,
3 unreadable input, 10 corrupt beyond repair, 11 password required, 12 output not writable,
13 unsupported encryption.

## Benchmarks

RayoPDF 1.1.0 against other converters on public benchmarks and on a synthetic corpus with
exact ground truth. Higher is better. Measured by this project on an AMD Ryzen 5 PRO 4650GE,
each tool in its own container: RayoPDF on 2026-11-25 (1.1.0's code, whose output is 1.0.0's and
0.5.0's byte for byte), docling and the text-layer tools on 2026-11-05 (the synthetic corpus on
2026-11-06), kordoc on 2026-11-12.

| Benchmark | RayoPDF, default output | RayoPDF, benchmark's conventions | kordoc 4.24.66 (OCR off) | docling 2.147.0 (no OCR) | Best text-layer tool |
|---|---:|---:|---:|---:|---:|
| opendataloader-bench, 200 pages: overall | 0.917 | **0.944** | 0.939 | 0.884 | 0.885 |
| olmOCR-bench: multi-column / tables / headers and footers | 0.732 / 0.646 / 0.890 | **0.762** / **0.653** / 0.891 | 0.490 / 0.534 / 0.421 | 0.695 / 0.610 / **0.898** | 0.667 / 0.615 / 0.554 |
| olmOCR-bench: arXiv math | – | **0.860** | 0.000 | 0.000 | 0.538 |
| READoc, 150 arXiv papers: text / headings / tables | 0.803 / 0.716 / 0.604 | **0.893** / **0.719** / **0.730** | 0.787 / 0.297 / 0.322 | 0.841 / 0.403 / 0.656 | 0.826 / 0.515 / 0.685 |
| READoc, 150 GitHub READMEs: text / headings | **0.936** / **0.882** | – | 0.835 / 0.405 | 0.908 / 0.471 | 0.874 / 0.778 |
| Synthetic corpus, 11 PDF producers: Markdown constructs (F1) | **0.971** | (same) | 0.468 | 0.560 | 0.628 |

"Benchmark's conventions" means RayoPDF ran with the options that match that benchmark's ground
truth, for example `--furniture=keep --plain --tables=html` for opendataloader-bench, whose
expected output keeps page headers and has no emphasis. The other tools ran with their own
defaults. The text-layer tools are pdftotext, markitdown, PyMuPDF, opendataloader, liteparse
and edgeparse; the last column shows the best of them for each number. A dash means not
measured (arXiv math needs `--math=latex`). On scanned pages, with the OCR add-on, RayoPDF reads
text with a similarity (NID) of 0.918, against 0.754 for docling with OCR and 0.856 for
Tesseract (those two measured on 2026-11-07).

Time to convert, start to exit, as a fresh process on one CPU core (other tools measured on
2026-11-09, RayoPDF on 2026-11-25):

| Tool | Version | One page | A 33-page report | Peak memory |
|---|---|---:|---:|---:|
| **RayoPDF** | 1.1.0 | **1.07 ms** | **10.7 ms** | **7 MiB** |
| pdftotext (plain text only) | Poppler 25.03 | 17 ms | 46 ms | 18 MiB |
| liteparse | 2.18.0 | 27 ms | 138 ms | 33 MiB |
| edgeparse | 0.3.2 | 45 ms | 306 ms | 27 MiB |
| opendataloader | 2.5.19 | 1,074 ms | 3,705 ms | 250 MiB |
| markitdown | 0.1.9 | 1,236 ms | 3,143 ms | 104 MiB |
| PyMuPDF | 2.1, layout on | 4,749 ms | 70,092 ms | 346 MiB |
| docling | 2.147.0, no OCR | 9,490 ms | 93,052 ms | 1,905 MiB |

Where other tools lead:

- **kordoc** leads opendataloader-bench in RayoPDF's default output (0.939 against 0.917), and
  on its tables (0.937 against 0.934) and headings (0.934 against 0.927) even with the
  benchmark's conventions. With its OCR on, kordoc scores 0.964.
- **docling** leads olmOCR-bench's headers and footers (0.898 against 0.891), and the tables of
  the synthetic PDFs rendered from HTML by WeasyPrint (0.877 against 0.792).
- **PyMuPDF** and **docling** lead READoc's arXiv tables in the default output (0.685 and 0.656
  against 0.604), and docling and liteparse lead its arXiv text there (0.841 and 0.826 against
  0.803). READoc writes formulas and tables the way LaTeX sources do; with its conventions
  RayoPDF leads.
- **GPU converters** built on vision-language models (olmOCR, MinerU, marker) were not run; on
  scanned pages they are expected to be stronger.

Every benchmark, tool version and number, and how to reproduce them:
[benchmarks](docs/benchmarks.md).

## Limitations

- **Not a renderer.** RayoPDF recovers text and structure; it does not render pages, and a
  figure drawn as vector graphics does not become an image.
- **Scans need the OCR add-on**, which takes seconds a page. Hebrew is not read yet; Arabic and
  Kannada are rough. Formulas on scanned pages stay text.
- **Tables of scientific papers** are weaker in the default output than with READoc's
  conventions (above).
- **Encrypted PDFs** need their password; certificate encryption is not supported.
- **Memory** grows with the input file (it is mapped into memory) and, when images are written,
  with the largest JPEG or JPEG 2000 image.

## Documentation

- [Documentation index](docs/README.md): every guide and document.
- Guides: [command line](docs/guide/cli.md), [output formats](docs/guide/formats.md),
  [OCR add-on](docs/guide/ocr.md), [Python](docs/guide/python.md),
  [JavaScript](docs/guide/javascript.md), [C and C++](docs/guide/cpp.md),
  [container image](docs/guide/container.md), [AI agents](docs/guide/agents.md).
- [Benchmarks](docs/benchmarks.md): where RayoPDF stands, and the dated records.
- [SECURITY.md](SECURITY.md): how to report a vulnerability.

## Contributing

[CONTRIBUTING.md](CONTRIBUTING.md) explains how to build, test, fuzz and measure, and
[AGENTS.md](AGENTS.md) holds the project's rules in short form. The core stays its own code with
no runtime dependency. A change to the output comes with its benchmark scores, a change to the
speed with measurements before and after.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

<p align="right"><a href="#readme-top">back to top ↑</a></p>
