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
python3 tools/dev.py test --config windows    # rayopdf.exe and rayopdf.dll (MinGW-w64), tested under Wine
python3 tools/dev.py image --name wheel       # rayopdf-wheel:1 (manylinux_2_28), once
python3 tools/dev.py test --config wheel      # librayopdf.so and the Python package (tests/verify_python.py)
python3 tools/dev.py image --name wasm        # rayopdf-wasm:1 (Emscripten), once
python3 tools/dev.py test --config wasm       # the WebAssembly module and the npm package (tests/verify_wasm.mjs)
python3 tools/fuzz.py replay                  # every fuzz target over the committed regressions
python3 tools/check_repo.py                   # the repository's rules that need no build
```

Before a change is proposed: `test --config asan` passes, a fresh build has no compiler
warning and `tools/check_repo.py` finds nothing (the Repository Hygiene workflow runs it on
every push). Changes to threads also pass `--config tsan`; changes to files, paths or processes also
pass `--config windows`; changes to the C ABI or the Python package (`python/`) also pass
`--config wheel` and `--config windows`; changes to the C ABI or the npm package
(`packaging/npm/`) also pass `--config wasm`; changes to the add-on pass `--config ocr` (image
`rayopdf-ocr:1`).

The CI workflow (`.github/workflows/ci.yml`) runs the same commands in the same image on pull
requests and once a night when master changed: the release build without warnings, the tests in
release and ASan + UBSan, and the fuzz regressions; on a tag `v*` and by hand
(`gh workflow run ci.yml`) also ThreadSanitizer, Windows, the wheel config and the wasm config.
It does not run on pushes to master.

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
[evaluation.md](docs/development/evaluation.md) (`tools/eval.py`; its § Commands); the public
summary of the results is [docs/benchmarks.md](docs/benchmarks.md).

- Work against the development suites. The holdouts (the `weasyprint` producer, the `readoc`
  suite) are scored as aggregates only and never inspected per document.
- A rule is general: it speaks of geometry, typography and structure, not of one document. It
  comes with a core test.
- Report the change of every development aggregate, not only the one the change was made for.
- A version measured on the harness gets a record `docs/benchmarks/v<version>-<date>.md`, added to
  [the index](docs/benchmarks/README.md) in the same commit.

## Changing the speed

[performance.md](docs/development/performance.md) has the method and what was tried;
`tools/bench.py` measures (performance.md § Rerunning; `--help` has every option). `BASE` is the
binary of the commit before, `NEW` the changed one, both kept as files outside `build/`:

```sh
docker build -f docker/perf.Dockerfile -t rayopdf-perf:1 docker       # the measuring image, once
E=benchmark-output/eval; O=$E/data/odl-bench/pdfs
python3 tools/bench.py identical --a BASE --b NEW --input $E/corpus --input $O   # same output
python3 tools/bench.py instr --bin base=BASE --bin new=NEW --doc DOC.pdf --opt=--images=none
python3 tools/bench.py warm  --bin base=BASE --bin new=NEW --dir $O --opt=--images=none
python3 tools/bench.py fresh --bin base=BASE --bin new=NEW --dir $O --opt=--images=none
python3 tools/bench.py pages --bin NEW --dir MULTIPAGE-PDFS             # per --workers count
python3 tools/bench.py tools --bin NEW --doc NAME=DOC.pdf               # against the other tools
```

- The output stays byte-identical (Markdown, image files, exit codes, `--report` records) on the
  whole corpus and for every `--workers` count, unless changing it is the purpose
  (`bench.py identical`: 0 differ).
- Instruction counts decide (`bench.py instr`: callgrind per conversion, no fixture more than
  0.3 % up); times (`warm`, `fresh`) are reported from one pinned, otherwise idle CPU, base and
  candidate interleaved, with the line `bench.py` prints first: machine, CPU, date.
- One step is one commit with its numbers. An attempt that lost goes into performance.md's
  "Tried and rejected" with its numbers, so that it is not tried again blindly.
- `src/hot.ld` is generated (`tools/hot_sections.py`); regenerate it after hot code moved.

## Generated tables

`src/text/*_data.cpp`, `src/layout/math_data.cpp` and `src/layout/model_data.cpp` are written by
`tools/gen_*_tables.py` only. `model_data.cpp` holds the small models' decision trees from the
trained dumps `eval/models/<name>.json` and `<name>.meta.json` (written by `eval/train/<name>.py`,
docs/development/evaluation.md § Training data of the small models). After changing a generator
or a model, regenerate and check: `python3 tools/gen_<name>_tables.py --check`.

## Versions and releases

`VERSION` holds the version; CMake and `tools/package.py` read it, and the README's Status section
names it (change both in one commit). `tools/package.py` builds the release archives byte for
byte reproducibly from already-built binaries; it copies the README's title block and its
`Status`, `Usage` and `License` sections, so those three headings stay; the same excerpt is the
Python wheel's description (`tools/build_wheel.py`), so it must read well on its own, offline.
It puts into each archive the licences of what that archive holds (`LICENSES` in the script: a
new file under `third_party/licenses/` is added there).

A release is a tag `v<VERSION>` on a commit of master:

```sh
V=$(cat VERSION)
gh workflow run release.yml              # optional dry run: the core archives as workflow artifacts
git tag v$V && git push origin v$V
```

The tag starts two workflows. CI runs every configuration, ThreadSanitizer and Windows included.
Release stops unless the tag is `v<VERSION>`, builds `--config static` and `--config windows` in
the build image, `--config wheel` in `rayopdf-wheel:1` and `--config wasm` in `rayopdf-wasm:1`,
packages them twice to check the bytes repeat, and makes a **draft** release with the seven core
packages and `SHA256SUMS`: `rayopdf-$V-linux-x64-static.tar.gz`, `rayopdf-$V-windows-x64.zip`,
the C++ API's `rayopdf-lib-$V-linux-x64.tar.gz` and `rayopdf-lib-$V-windows-x64.zip`
(`librayopdf.a`, which those builds' `rayopdf_bundle` target merges from the modules, with its
header and example), the Python wheels `rayopdf-$V-py3-none-manylinux_2_28_x86_64.whl` and
`rayopdf-$V-py3-none-win_amd64.whl` (`tools/build_wheel.py`: the package with `librayopdf.so` or
`rayopdf.dll`) and the npm package `rayopdf-$V.tgz` (the WebAssembly module), attached to the
release only, not published to PyPI or npm. Its `container` job builds the image of
`packaging/container/Dockerfile` from the Linux archive, checks that it prints its version and
converts a one-page PDF, and pushes it as `ghcr.io/butterski/rayopdf:$V` and `:latest`.

The OCR add-on's archives need the `rayopdf-ocr:1` image (inference runtimes and models; its
build takes about 50 minutes), so they are built locally from the tagged commit and attached to
the draft:

```sh
git checkout v$V
python3 tools/dev.py image --name ocr                        # rayopdf-ocr:1, once
python3 tools/dev.py test --config ocr                       # the add-on and its tests
python3 tools/dev.py build --config ocr-windows --target rayopdf-ocr
python3 tools/dev.py --image rayopdf-ocr:1 run -- \
    sh -c 'rm -rf build/ocr-models && cp -r /opt/ocr/models build/ocr-models'
rm -rf build/dist && mkdir -p build/dist
gh release download v$V --dir build/dist --pattern "rayopdf-$V-*" --pattern "rayopdf-$V.tgz" \
    --pattern "rayopdf-lib-$V-*"                             # the seven core packages
for part in ocr-linux ocr-windows ocr-scripts-linux ocr-scripts-windows; do
    python3 tools/package.py --only $part                    # SHA256SUMS: every archive in build/dist
done
(cd build/dist && sha256sum -c SHA256SUMS)                   # eleven packages
gh release upload v$V build/dist/rayopdf-ocr-* build/dist/SHA256SUMS --clobber
```

`SHA256SUMS` then lists all eleven packages, the seven core ones with the bytes the workflow built.
Publish the draft (`gh release edit v$V --draft=false`) once CI has passed on the tag and the
version has its record under `docs/benchmarks/`.

## Licence

RayoPDF is Apache-2.0, and contributions are accepted under it. Every own source file starts with
its SPDX line. Code forked from another project names its origin and commit in its header comment
and is listed in [NOTICE](NOTICE). GPL or AGPL code and data (PyMuPDF/MuPDF, Poppler, marker, Grok)
are never copied into the repository: such tools run as black boxes inside the evaluation images
only.
