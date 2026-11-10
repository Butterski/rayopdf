# RayoPDF Agent Guide

Read this file before changing the project. RayoPDF is a PDF to Markdown converter, the reverse
of [RayoMD](https://github.com/Butterski/rayomd): the C++17 engine under `src/` (modules common,
crypt, pdf, text, image, raster, layout, ocr and cli; the OCR add-on `rayopdf-ocr` is built from
`src/ocr/addon`), its tests under `tests/`, the evaluation harness under `eval/` and
`tools/eval.py`, and the research and roadmap under `docs/`.

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
   process. Third-party numbers are marked "reproduced" or "claimed".

7. Holdout: never inspect per-document outputs of the `weasyprint` producer or the `readoc`
   suite while designing converter heuristics; use aggregate scores only. RayoMD producers never
   use `--embed-source`. Tagged and untagged producers are always reported separately.

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
    (`README_SECTIONS`) copies them into the release archives and stops if one is missing.

13. Each version measured on the harness gets one record `docs/benchmarks/v<version>-<date>.md`;
    baselines are `baselines-<suite>-<date>.md` or `baselines-<date>.md`, release checks
    `release-check-<date>.md`. Add every new record to `docs/benchmarks/README.md` (current
    records and history) in the same commit.

14. To bump the version, change `VERSION` (CMake and `tools/package.py` read it) and the version
    in the README's Status section in the same commit.

15. `python3 tools/check_repo.py` checks rules 1, 2, 12 and 13, the fuzz targets' names and the
    relative links of the documents; it finds nothing before a push (the Repository Hygiene
    workflow runs it).
