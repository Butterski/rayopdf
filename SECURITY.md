# Security

RayoPDF reads PDFs from anywhere, so every byte of its input is treated as hostile.

## Reporting a problem

Please report a vulnerability privately: on GitHub, open the repository's **Security** tab and
choose **Report a vulnerability** (a private security advisory). Do not open a public issue for
it. Include the RayoPDF version (`rayopdf --version`), the platform, the command line and, if you
can share it, the file; a minimized file is ideal.

The latest release is the supported version. A fix comes with the minimized input as a
regression test.

## What counts

- Memory errors, crashes, hangs and unbounded memory or time in `rayopdf` or in the OCR add-on
  `rayopdf-ocr` on any input file.
- Output written outside the paths given on the command line (the Markdown file, its image
  folder, `--batch`'s output folder).
- Anything in the `--serve --json` protocol or the add-on's pipe protocol that lets one request
  affect another.

A PDF that converts to wrong or incomplete Markdown is a quality bug, not a vulnerability: open
an ordinary issue.

## How the code is kept safe

- Every loop over input is bounded, and every limit (nesting, reference chains, decoded bytes of
  a stream and of a document, image sizes, table and formula work) fails closed and is counted in
  `--report` ([roadmap D8](docs/development/roadmap.md#d8-limits)).
- The code throws no exceptions (a failed allocation ends the process), and no third-party
  library parses input in the core. The OCR add-on is a separate process: the core renders the
  page images itself and passes it pixels, never the PDF.
- Every parser of untrusted input has a libFuzzer target (`tests/fuzz/`, built with ASan and
  UBSan); a release needs 24 hours of clean fuzzing over all targets, and an ASan build converts
  the evaluation corpora without a report
  ([release bar](docs/development/roadmap.md#goal-and-release-bar), item d).
- The static Linux binary is position-independent (`-static-pie`), so its image is loaded at a
  random address.
