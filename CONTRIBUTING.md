# Contributing

RayoPDF converts PDFs to Markdown in its own C++17 code: one small static binary, fast enough to
run per file, and explicit about what it could not read. Contributions are welcome when they keep
that shape. [AGENTS.md](AGENTS.md) holds the rules in short form; this file says how to work with
them.

## Priorities

- **Own code, no runtime.** The core links no third-party library. A new dependency needs a
  permissive licence and a measured reason ([roadmap D2](docs/development/roadmap.md#d2-dependencies)).
  The OCR add-on is a separate program and is never linked into `rayopdf`.
- **Untrusted input.** Every loop over input is bounded and every limit fails closed
  ([roadmap D8](docs/development/roadmap.md#d8-limits)). A new parser comes with a fuzz target.
- **Measured claims.** Quality is what the evaluation harness scores, speed is what a pinned A/B
  shows; both are recorded with date, machine and versions.
- **Nothing generated in the repository:** no PDFs, corpora, models, images, build trees or
  benchmark output.

## Build and test

The compilers live in a Docker image; nothing is installed on the host. Requirements: Linux,
Docker and Python 3.10 or newer.

```sh
python3 tools/dev.py image                    # rayopdf-build:2, once
python3 tools/dev.py build                    # build/release/rayopdf
python3 tools/dev.py test --config asan       # core tests and the CLI verifier, ASan + UBSan
python3 tools/dev.py test --config tsan       # the same under ThreadSanitizer
python3 tools/dev.py test --config windows    # rayopdf.exe (MinGW-w64), tested under Wine
python3 tools/fuzz.py replay                  # every fuzz target over the committed regressions
python3 tools/check_repo.py                   # the repository's rules that need no build
```

Before a change is proposed: `test --config asan` passes, a fresh build has no compiler
warning and `tools/check_repo.py` finds nothing (the Repository Hygiene workflow runs it on
every push). Changes to threads also pass `--config tsan`; changes to files, paths or processes also
pass `--config windows`; changes to the add-on pass `--config ocr` (image `rayopdf-ocr:1`).

Tests live in `tests/test_<module>[_<part>].cpp`, each check with its own code from the range
`tests/check.h` lists, and build the PDFs they read in memory. `tests/verify_cli.py` checks the
built binary from outside.

## Changing what a parser accepts

Every parser of untrusted input has a libFuzzer target `tests/fuzz/fuzz_<module>_<name>.cpp`.
Run the targets of the code you changed (`python3 tools/fuzz.py run --seconds 600 --targets
fuzz_<module>_<name>`), and commit the minimized input of anything it finds under
`tests/fuzz/regressions/<target>/` together with the fix.

## Changing the output

Output quality is measured by the harness of
[evaluation.md](docs/development/evaluation.md) (`tools/eval.py`; README § Running the evaluation).

- Work against the development suites. The holdouts (the `weasyprint` producer, the `readoc`
  suite) are scored as aggregates only and never inspected per document.
- A rule is general: it speaks of geometry, typography and structure, not of one document. It
  comes with a core test.
- Report the change of every development aggregate, not only the one the change was made for.
- A version measured on the harness gets a record `docs/benchmarks/v<version>-<date>.md`, added to
  [the index](docs/benchmarks/README.md) in the same commit.

## Changing the speed

[performance.md](docs/development/performance.md) has the method and what was tried.

- The output stays byte-identical (Markdown, image files, exit codes, `--report` records) on the
  whole corpus and for every `--workers` count, unless changing it is the purpose.
- Instruction counts decide (callgrind per conversion, no fixture more than 0.3 % up); times are
  reported from one pinned, otherwise idle CPU, base and candidate interleaved.
- One step is one commit with its numbers. An attempt that lost goes into performance.md's
  "Tried and rejected" with its numbers, so that it is not tried again blindly.
- `src/hot.ld` is generated (`tools/hot_sections.py`); regenerate it after hot code moved.

## Generated tables

`src/text/*_data.cpp` and `src/layout/math_data.cpp` are written by `tools/gen_*_tables.py` only.
After changing a generator, regenerate and check: `python3 tools/gen_<name>_tables.py --check`.

## Versions and releases

`VERSION` holds the version; CMake and `tools/package.py` read it, and the README's Status section
names it (change both in one commit). `tools/package.py` builds the release archives byte for
byte reproducibly from already-built binaries; it copies the README's `Status`, `Usage` and
`License` sections, so those three headings stay.

## Licence

RayoPDF is Apache-2.0, and contributions are accepted under it. Every own source file starts with
its SPDX line. Code forked from another project names its origin and commit in its header comment
and is listed in [NOTICE](NOTICE). GPL or AGPL code and data (PyMuPDF/MuPDF, Poppler, marker, Grok)
are never copied into the repository: such tools run as black boxes inside the evaluation images
only.
