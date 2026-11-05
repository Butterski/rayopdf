# RayoPDF Agent Guide

Read this file before changing the project. RayoPDF is a PDF to Markdown converter, the reverse
of [RayoMD](https://github.com/Butterski/rayomd). The repository currently holds the research,
the evaluation harness and the roadmap; there is no converter yet.

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
