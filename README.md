# RayoPDF

PDF to Markdown, the reverse of [RayoMD](https://github.com/Butterski/rayomd).

## Status

Research and evaluation; there is no converter yet.

## Documents

- [Landscape](docs/research/landscape.md): existing converters, benchmarks, metrics and corpora.
- [Algorithms](docs/research/algorithms.md): PDF structure, content streams, fonts and Unicode,
  layout, Markdown semantics and speed engineering for a native converter.
- [OCR and ML](docs/research/ocr-ml.md): when OCR is unavoidable, usable engines and runtimes, and
  the decision gate.
- [Evaluation contract](docs/development/evaluation.md): suites, metrics, competitors, speed and
  census method.
- [Baselines](docs/benchmarks/baselines-2026-11-05.md): the first full harness run.
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
