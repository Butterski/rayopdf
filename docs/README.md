# RayoPDF documentation

Start with the [README](../README.md) to install RayoPDF and convert a first PDF. The documents
below are in three groups: guides for users, benchmarks, and the development and research
documents for contributors.

## Guide

For people who use RayoPDF.

| Guide | What it covers |
|---|---|
| [Command line](guide/cli.md) | Every command and option: converting, pages, passwords, images, formulas, the options for a reader's conventions, PDFs made by RayoMD, `--batch`, `--serve --json`, `--inspect`, exit codes. |
| [Output formats](guide/formats.md) | What is written: Markdown, page markers and front matter, JSON, chunks for retrieval, the layout view, the `--report` record. |
| [OCR add-on](guide/ocr.md) | Reading scanned pages: install, scripts, options, speed. |
| [Python](guide/python.md) | The `rayopdf` package, its errors, and the LangChain and LlamaIndex loaders. |
| [JavaScript](guide/javascript.md) | The npm package (WebAssembly) for browsers and Node.js. |
| [C and C++](guide/cpp.md) | The C++ API and the C ABI. |
| [Container image](guide/container.md) | Running RayoPDF with Docker. |
| [AI agents](guide/agents.md) | The Agent Skill and the MCP server. |

To report a vulnerability, see [SECURITY.md](../SECURITY.md).

## Benchmarks

| Document | What it covers |
|---|---|
| [Benchmarks](benchmarks.md) | Where RayoPDF stands: the benchmarks, the tools compared, the results, speed, where other tools lead, and how to reproduce the numbers. |
| [Benchmark records](benchmarks/README.md) | The dated records: every version measured, the baselines of the other tools, the release checks. They are kept as they were written, in the project's working vocabulary. |

## Development and research

For contributors. These documents record how RayoPDF is built and measured, and the decisions
behind it. They use the project's working terms: for example, a *holdout* is a test set measured
only once per release and never examined during development, a *declared profile* is the set of
options a benchmark is measured with, the *strict gate* is the rule that no gated benchmark score
may fall more than 0.0001 below its best when a change lands, and D1-D8 and M1-M9 name
decisions and milestones of the roadmap. Start with
[CONTRIBUTING.md](../CONTRIBUTING.md) and [AGENTS.md](../AGENTS.md).

| Document | What it covers |
|---|---|
| [Roadmap](development/roadmap.md) | The release criteria, the architecture decisions (D1-D8, among them the output dialect, the command line and the limits), the milestones and the engineering conventions. |
| [Evaluation](development/evaluation.md) | The evaluation harness: benchmarks and their pins, the synthetic and scanned corpora, metrics, competitors, benchmark profiles, commands. |
| [Performance](development/performance.md) | Every speed round: method, steps, results, what was tried and rejected, and how to rerun the measurements. |
| [OCR engine](development/ocr-engine.md) | The OCR add-on's engine: models, runtimes, build, packaging, measurements. |
| [OCR protocol](development/ocr-protocol.md) | How `rayopdf` and the OCR add-on talk: discovery, frames, requests, routing. |
| [OCR and ML decision](development/ocr_ml_decision.md) | Why the add-on uses PP-OCRv6, and the decision on small models in the core. |

Research notes, written before or during the work they informed:

| Note | Topic |
|---|---|
| [Landscape](research/landscape.md) | Other converters, benchmarks and corpora. |
| [Features](research/features.md) | Features of the other converters and RayoPDF's gaps (for 1.2). |
| [Algorithms](research/algorithms.md) | PDF parsing, fonts, text, layout and structure recovery. |
| [Speed](research/speed.md) | Techniques of fast parsers and how to measure them. |
| [OCR and ML](research/ocr-ml.md) | When OCR is needed, engines, models and runtimes. |
| [Small ML](research/small-ml.md) | Small learned models from the text layer, their data and licences. |
| [Layout math](research/layout-math.md) | Statistics in place of fixed thresholds, and formula structure. |
