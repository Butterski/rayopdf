# RayoPDF

PDF to Markdown, the reverse of [RayoMD](https://github.com/Butterski/rayomd).

## Status

Version 0.0.8: born-digital PDFs to GitHub Flavored Markdown with headings, paragraphs, lists,
code blocks, block quotes, alerts, emphasis, links, tables, footnotes and images, right-to-left and
CJK text included; tagged PDFs are read through their structure tree. Scanned pages are marked,
not read (no OCR yet); math is written as Unicode text.
The [roadmap](docs/development/roadmap.md) lists what comes next.

## Usage

```sh
rayopdf --convert IN.pdf OUT.md [--images=DIR|none] [--password=PW] [--pages=A-B]
                                [--no-annotations] [--no-structure] [--report]
rayopdf --batch IN-FOLDER OUT-FOLDER [--recursive] [--skip-unchanged] [--report[=FILE]]
                                     [--workers=N] [--images=none] [--password=PW] ...
rayopdf --serve --json [--workers=N] [--images=none] [--password=PW] ...
rayopdf --inspect IN.pdf          # the document's structure as one JSON object
```

`OUT.md` `-` writes to standard output. Images go to `OUT-images/` beside `OUT.md` (PNG, or
the PDF's own JPEG and JPEG 2000 data) unless `--images` says otherwise. Text that could not be
read is never dropped silently: a `<!-- rayopdf: ... -->` comment marks the spot. Exit codes: 0
converted, 2 usage, 3 unreadable input, 10 corrupt beyond repair, 11 password required, 12
unwritable output, 13 unsupported encryption.

`--report` prints one JSON record of the document: `input`, `output`, `status` (`ok`, `skipped`
or `error`), `code` (the exit code), `error` when it failed, `pages`, `bytes` (of Markdown), `ms`,
then the counters behind the uncertainty comments (glyphs without Unicode, pages without text,
repairs, limits reached, ...).

### Batch conversion

`--batch` converts every `*.pdf` of a folder (any case of `.pdf`) to `OUT-FOLDER/NAME.md`, its
images to `OUT-FOLDER/NAME-images/`. `--recursive` adds the subfolders, except hidden ones, mirrored
under `OUT-FOLDER`; `--skip-unchanged` keeps Markdown files at least as new as their PDF; `--report`
writes one record a document, in the sorted order of the inputs, to standard output or to FILE.
`--workers=N` converts N documents at once (1 to 64; by default one a CPU, at most 6). The files and
records do not depend on N, only the `ms` of each record. The exit code is 0 when every document
converted, else the code of the last one that failed (in input order); stderr names each failure.

```sh
rayopdf --batch papers md-out --recursive --skip-unchanged --report=report.jsonl --workers=8
```

### Front ends: `--serve --json`

`--serve --json` keeps one process running for an editor plugin, an agent tool or a script, with
[RayoMD](https://github.com/Butterski/rayomd)'s protocol 1: each line in is a JSON request, each
line out is the request's record with its `id`. The first line out is
`{"rayopdf":"VERSION","protocol":1}`. A request has `input` (a PDF), optionally `id` (a string or
an integer, sent back as it came), `output` (a Markdown file; without it the record carries the
Markdown as `markdown` and no images are written) and `args` (flags of `--convert`:
`--password`, `--pages`, `--images`, `--no-annotations`, `--no-structure`, over those the session
started with). A malformed request gets an error record (code 2) and the session goes on; `quit`,
`exit` or the end of the input ends it. With `--workers=N` requests already sent are converted N at
a time, and the answers still come in request order.

```sh
printf '%s\n' '{"id":"1","input":"a.pdf","output":"out/a.md","args":["--pages=1-3"]}' \
  '{"id":2,"input":"b.pdf"}' | rayopdf --serve --json
```

## Building

The compilers live in a Docker image (`docker/build.Dockerfile`); nothing is installed on the
host. Requirements: Linux, Docker and Python 3.10 or newer.

```sh
python3 tools/dev.py image                    # build rayopdf-build:2 once (with MinGW-w64 and Wine)
python3 tools/dev.py build                    # build/release/rayopdf
python3 tools/dev.py test --config asan       # core tests and the CLI verifier under ASan + UBSan
python3 tools/dev.py test --config tsan       # the same under ThreadSanitizer
python3 tools/dev.py build --config static    # a fully static build/static/rayopdf
python3 tools/dev.py test --config windows    # build/windows/rayopdf.exe, tested under Wine
python3 tools/package.py                      # release archives and SHA256SUMS in build/dist/
```

The source is C++17 with no third-party library: `src/crypt` (Standard security handler),
`src/pdf` (objects, filters, page tree), `src/text` (fonts to Unicode), `src/layout`
(interpretation, reading order, Markdown), `src/image`, `src/common` (files, threads, the batch
record and the serve protocol) and `src/cli`. The release archives `rayopdf-VERSION-linux-x64-static.tar.gz`
and `rayopdf-VERSION-windows-x64.zip` hold the binary, this README's usage part, LICENSE, NOTICE,
VERSION and `third_party/licenses/`; `tools/package.py` makes them byte for byte reproducible.

## Documents

- [Landscape](docs/research/landscape.md): existing converters, benchmarks, metrics and corpora.
- [Algorithms](docs/research/algorithms.md): PDF structure, content streams, fonts and Unicode,
  layout, Markdown semantics and speed engineering for a native converter.
- [OCR and ML](docs/research/ocr-ml.md): when OCR is unavoidable, usable engines and runtimes, and
  the decision gate.
- [Evaluation contract](docs/development/evaluation.md): suites, metrics, competitors, speed and
  census method.
- [Baselines](docs/benchmarks/baselines-2026-11-05.md): the first full harness run.
- [RayoPDF 0.0.1 on the harness](docs/benchmarks/v0-2026-11-05.md): the first converter against
  the baselines.
- [RayoPDF 0.0.2 on the harness](docs/benchmarks/v0.0.2-2026-11-06.md): tables, images,
  footnotes, right-to-left and CJK text.
- [RayoPDF 0.0.3 on the harness](docs/benchmarks/v0.0.3-2026-11-06.md): odl headings and tables,
  code listings, the 24-hour fuzz campaign.
- [RayoPDF 0.0.4 on the harness](docs/benchmarks/v0.0.4-2026-11-06.md): page furniture,
  reading order, run-in headings, tables; every odl metric at or above the best text-layer tool.
- [RayoPDF 0.0.5 on the harness](docs/benchmarks/v0.0.5-2026-11-06.md): borderless tables,
  every Adobe CJK CMap, the full Unicode bidi algorithm.
- [RayoPDF 0.0.6 on the harness](docs/benchmarks/v0.0.6-2026-11-06.md): table detection; every
  olmOCR born-digital category except `arxiv_math` at or above the best text-layer tool.
- [RayoPDF 0.0.7 on the harness](docs/benchmarks/v0.0.7-2026-11-07.md): tagged PDF, batch and
  serve front ends, Windows build, ToUnicode repair, JPEG decoding, the OCR/ML gate.
- [OCR/ML decision](docs/development/ocr_ml_decision.md): the M8 measurements and the decision.
- [RayoPDF 0.0.8 on the harness](docs/benchmarks/v0.0.8-2026-11-07.md): the first performance
  campaign, output byte-identical to 0.0.7.
- [Performance](docs/development/performance.md): campaign method, findings, steps, results and
  rejected attempts.
- [Roadmap](docs/development/roadmap.md): decisions, milestones and exit gates.

## Running the evaluation

Requirements: Linux, Docker, Python 3.10 or newer, and at least 25 GB of free disk. Every
third-party tool runs inside the images built from `eval/docker/`; nothing is installed on the
host. Outputs go to `benchmark-output/eval/`.

```sh
python3 tools/eval.py all
```

`all` runs these steps in order; each can also be run alone:

```sh
python3 tools/eval.py images                      # build the Docker images
python3 tools/eval.py fetch                       # download the public benchmarks
python3 tools/eval.py corpus                      # render the synthetic corpus
python3 tools/eval.py score --selftest            # check the construct scorer
python3 tools/eval.py convert --tool T --suite S  # T: pdftotext, markitdown, pymupdf, opendataloader,
                                                  #    liteparse, edgeparse, docling, cli
                                                  # S: synthetic, odl, olmocr, readoc
python3 tools/eval.py score --suite S
python3 tools/eval.py speed --tool T
python3 tools/eval.py census
python3 tools/eval.py report                      # benchmark-output/eval/report.md
```

`python3 tools/eval.py all --resume` continues an interrupted run.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
