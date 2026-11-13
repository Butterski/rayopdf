# RayoPDF Agent Guide

Read this file before changing the project. RayoPDF is a PDF to Markdown converter, the reverse
of [RayoMD](https://github.com/Butterski/rayomd): the C++17 engine under `src/` (modules common,
crypt, pdf, text, image, raster, layout, api, ocr and cli; the C++ API's public header is
`include/rayopdf/rayopdf.h`, the C ABI's `include/rayopdf/rayopdf_c.h`; the OCR add-on
`rayopdf-ocr` is built from `src/ocr/addon`), its tests under `tests/`, the Python package over
the C ABI under `python/` (ctypes, Python 3.9 or newer, the standard library only outside
`rayopdf/integrations/`), the evaluation harness under `eval/` and `tools/eval.py`, and the
research and roadmap under `docs/`.

## Rules

1. Never commit generated material: PDFs, downloaded data, corpora, images, or anything under
   `benchmark-output/`.

2. Every own source file starts with an SPDX line: `# SPDX-License-Identifier: Apache-2.0` in
   Python, Dockerfile and shell files (after the shebang line where there is one), or
   `// SPDX-License-Identifier: Apache-2.0` in C and C++ files. Markdown and JSON files are
   exempt.

3. Host-side Python under `tools/` uses only the standard library and runs on Python 3.10 or
   newer. Each script has a shebang, the SPDX line, a module docstring with usage, `argparse`,
   and ends with `raise SystemExit(main())`. Container-side Python under `eval/` may import only
   what its Dockerfile installs.

4. Third-party tools run only inside images built from `eval/docker/*.Dockerfile`. Conversion,
   scoring and census runs use `--network none`, read-only input mounts and
   `-u $(id -u):$(id -g)`. Only `tools/eval.py images` and `tools/eval.py fetch` use the
   network.

5. Pin the version of every tool under evaluation and every benchmark: pip `==`, git commit,
   Hugging Face dataset revision, or release SHA-256. Each image records its full resolved
   versions in `/opt/eval/versions.json`.

6. Benchmark claims are dated and scoped: hardware, versions, threads, and warm or fresh
   process. Third-party numbers are marked "reproduced" or "claimed". Speed is measured with
   `tools/bench.py` (docs/development/performance.md § Rerunning), which prints that scope with
   its numbers; a time is quoted only from a CPU nothing else ran on.

7. Holdout: never inspect per-document outputs or scores of the `weasyprint` producer or the
   `readoc` suite (the 300 READoc documents of `data/READoc-data/selection.json`) while
   designing converter heuristics; use aggregate scores only. The validation split
   `readoc-val` (600 further READoc documents, `selection-val.json`) is aggregate-only in the
   same way, and every quality landing is checked against it; so are `doclaynet-val`
   (DocLayNet's test split) and `omni-val` (half of OmniParseBench's PDF pages). `readoc-dev`
   (300 other READoc documents, `selection-dev.json`), `doclaynet-dev` and `omni-dev` are
   development suites: inspect them freely. RayoMD producers
   never use `--embed-source`. Tagged and untagged producers are always reported separately.
   Every quality landing passes the strict gate (`qgate.sh`): no gated metric may fall more than
   0.0001 below its best since the last rebase, and each suite is measured with its declared
   options (`eval/profiles.json`).

8. Never copy AGPL or GPL code or data into the repository (PyMuPDF/MuPDF, Poppler, marker).
   They run as black boxes inside images only. READoc's code has no licence: run it from a
   clone and never copy it.

9. Engine code follows `docs/development/roadmap.md` § Engineering conventions.

10. Build and test only through `tools/dev.py`, which runs the compilers in the
    `rayopdf-build:2` image. Every change keeps `python3 tools/dev.py test --config asan` passing
    and a fresh build free of compiler warnings. Core tests live in
    `tests/test_<module>[_<part>].cpp`, every check with its own code in the range `tests/check.h`
    lists; `tests/verify_cli.py` checks the built binary. Every parser of untrusted input has a
    libFuzzer target `tests/fuzz/fuzz_<module>_<name>.cpp` (`tools/fuzz.py` runs them); a fixed
    finding's minimized input goes to `tests/fuzz/regressions/<target>/`. Tests build the PDFs they
    read in memory.

11. Generated tables (`src/text/*_data.cpp`, `src/layout/math_data.cpp`) are written only by
    `tools/gen_*_tables.py`. After changing a generator, regenerate, and check that every
    generated file matches with `python3 tools/gen_<name>_tables.py --check`.

12. `README.md` keeps the H2 headings `Status`, `Usage` and `License`: `tools/package.py`
    (`README_SECTIONS`) copies them into the release archives and stops if one is missing. Each
    archive carries the licences of what it holds: a new file under `third_party/licenses/` is
    added to `LICENSES` in `tools/package.py`.

13. Each version measured on the harness gets one record `docs/benchmarks/v<version>-<date>.md`;
    baselines are `baselines-<suite>-<date>.md` or `baselines-<date>.md`, release checks
    `release-check-<date>.md`. Add every new record to `docs/benchmarks/README.md` (current
    records and history) in the same commit.

14. To bump the version, change `VERSION` (CMake and `tools/package.py` read it) and the version
    in the README's Status section in the same commit. A release is the tag `v<VERSION>`
    (CONTRIBUTING.md § Versions and releases); agents create no tag and no release unless asked.

15. `python3 tools/check_repo.py` checks rules 1, 2, 12 and 13, the fuzz targets' names and the
    relative links of the documents; it finds nothing before a push (the Repository Hygiene
    workflow runs it).

16. GitHub Actions minutes are a monthly budget. The CI workflow runs rule 10's commands in the
    build image on pull requests, tags `v*`, once a night and by hand, never on pushes to master;
    keep it so, with `concurrency` that cancels superseded runs and `timeout-minutes` on every
    job. A workflow runs the same `tools/` commands a developer runs, in the image built from
    `docker/build.Dockerfile` (`.github/actions/build-image`), or for the `wheel` config the one
    `tools/dev.py image --name wheel` builds, not its own build steps.
