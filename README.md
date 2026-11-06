# RayoPDF

PDF to Markdown, the reverse of [RayoMD](https://github.com/Butterski/rayomd).

## Status

Version 0.0.1, the first converter: born-digital PDFs to GitHub Flavored Markdown with headings,
paragraphs, lists, code blocks, emphasis and links. Tables, images, footnotes and OCR are not
written yet; the [roadmap](docs/development/roadmap.md) lists what comes next.

## Usage

```sh
rayopdf --convert IN.pdf OUT.md [--password=PW] [--pages=A-B] [--no-annotations] [--report]
rayopdf --inspect IN.pdf          # the document's structure as one JSON object
```

`OUT.md` `-` writes to standard output. Text that could not be read is never dropped silently: a
`<!-- rayopdf: ... -->` comment marks the spot. Exit codes: 0 converted, 2 usage, 3 unreadable
input, 10 corrupt beyond repair, 11 password required, 12 unwritable output, 13 unsupported
encryption.

## Building

The compilers live in a Docker image (`docker/build.Dockerfile`); nothing is installed on the
host. Requirements: Linux, Docker and Python 3.10 or newer.

```sh
python3 tools/dev.py image                    # build rayopdf-build:1 once
python3 tools/dev.py build                    # build/release/rayopdf
python3 tools/dev.py test --config asan       # core tests and the CLI verifier under ASan + UBSan
python3 tools/dev.py build --config static    # a fully static build/static/rayopdf
```

The source is C++17 with no third-party library: `src/crypt` (Standard security handler),
`src/pdf` (objects, filters, page tree), `src/text` (fonts to Unicode), `src/layout`
(interpretation, reading order, Markdown) and `src/cli`.

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
