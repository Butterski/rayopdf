# Baselines on the READoc development split, 2026-11-10

`readoc-dev` is a development suite of the harness ([evaluation.md](../development/evaluation.md)):
150 arXiv and 150 GitHub documents of READoc (dataset `lazyc/READoc` at revision `782bf954`), taken
in the holdout's own order (ground-truth stems sorted by the SHA-256 of the stem) right after the
holdout's 150 of each subset, so the two sets share no document. Its documents, outputs and
per-document scores may be inspected; the holdout (`readoc`) stays aggregate-only. Scored with
READoc's evaluator at commit `afa0477`, as the holdout is.

Tools and versions as in [the baselines](baselines-2026-11-05.md); RayoPDF is 0.0.12, core alone
(`rayopdf --convert IN OUT`), in its default mode and with `--math=latex`. Same machine (AMD
Ryzen 5 PRO 4650GE). Reproduced numbers.

## arXiv (150 documents)

| metric | RayoPDF | RayoPDF `--math=latex` | pdftotext | markitdown | pymupdf | opendataloader | liteparse | edgeparse | docling |
|---|---|---|---|---|---|---|---|---|---|
| text_eds | 0.761 | 0.859 | 0.759 | 0.585 | 0.763 | 0.809 | **0.836** | 0.699 | 0.847 |
| text_f1 | 0.847 | 0.905 | **0.846** | 0.619 | 0.840 | 0.832 | 0.834 | 0.813 | 0.858 |
| head_eds | 0.605 | 0.606 | 0.042 | 0.018 | **0.611** | 0.407 | 0.412 | 0.399 | 0.702 |
| head_teds | 0.488 | 0.489 | 0.007 | 0.002 | **0.523** | 0.371 | 0.280 | 0.226 | 0.393 |
| inline_eds | 0.000 | 0.620 | 0.001 | 0.001 | 0.000 | 0.001 | **0.375** | 0.001 | 0.000 |
| outline_eds | 0.000 | 0.571 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 |
| table_eds | 0.457 | 0.466 | 0.000 | 0.113 | **0.656** | 0.254 | 0.466 | 0.349 | 0.666 |
| table_teds | 0.439 | 0.436 | 0.000 | 0.036 | **0.657** | 0.187 | 0.348 | 0.272 | 0.655 |
| seg_kt | 0.944 | 0.952 | 0.928 | 0.532 | 0.964 | 0.972 | **0.973** | 0.939 | 0.979 |
| word_kt | 0.965 | 0.973 | 0.981 | 0.895 | 0.977 | **0.983** | 0.976 | 0.942 | 0.971 |
| seg_sp | 0.916 | 0.929 | 0.916 | 0.508 | 0.958 | 0.969 | **0.971** | 0.942 | 0.980 |
| word_sp | 0.950 | 0.959 | 0.978 | 0.882 | 0.973 | **0.979** | 0.973 | 0.941 | 0.967 |

Bold: the best text-layer tool (docling is the ML reference).

The holdout's values of the same keys, for comparison (aggregates of the earlier records):

| metric | RayoPDF | RayoPDF `--math=latex` | pdftotext | markitdown | pymupdf | opendataloader | liteparse | edgeparse | docling |
|---|---|---|---|---|---|---|---|---|---|
| text_eds | 0.747 | 0.858 | 0.768 | 0.584 | 0.754 | 0.806 | 0.826 | 0.725 | 0.841 |
| head_teds | 0.473 | 0.474 | 0.006 | -0.001 | 0.515 | 0.308 | 0.254 | 0.229 | 0.403 |
| table_teds | 0.483 | 0.491 | 0.000 | 0.036 | 0.685 | 0.138 | 0.407 | 0.286 | 0.656 |
| seg_kt | 0.953 | 0.956 | 0.888 | 0.508 | 0.976 | 0.969 | 0.969 | 0.948 | 0.984 |

## GitHub (150 documents)

| metric | RayoPDF | RayoPDF `--math=latex` | pdftotext | markitdown | pymupdf | opendataloader | liteparse | edgeparse | docling |
|---|---|---|---|---|---|---|---|---|---|
| text_eds | 0.943 | 0.943 | **0.870** | 0.769 | 0.815 | 0.807 | 0.757 | 0.789 | 0.904 |
| text_f1 | 0.922 | 0.922 | **0.858** | 0.604 | 0.794 | 0.812 | 0.844 | 0.811 | 0.890 |
| head_eds | 0.889 | 0.889 | 0.051 | 0.020 | 0.643 | 0.762 | **0.768** | 0.469 | 0.748 |
| head_teds | 0.885 | 0.885 | 0.031 | 0.008 | 0.603 | **0.780** | 0.769 | 0.385 | 0.472 |
| seg_kt | 0.997 | 0.997 | 0.971 | 0.786 | 0.988 | **0.993** | 0.991 | 0.985 | 0.995 |
| word_kt | 0.997 | 0.997 | **0.995** | 0.929 | 0.988 | 0.987 | 0.994 | 0.985 | 0.991 |
| seg_sp | 0.996 | 0.996 | 0.962 | 0.767 | 0.979 | **0.990** | 0.988 | 0.981 | 0.994 |
| word_sp | 0.996 | 0.996 | **0.994** | 0.896 | 0.984 | 0.984 | 0.992 | 0.982 | 0.989 |

Bold: the best text-layer tool (docling is the ML reference).

The holdout's values of the same keys, for comparison (aggregates of the earlier records):

| metric | RayoPDF | RayoPDF `--math=latex` | pdftotext | markitdown | pymupdf | opendataloader | liteparse | edgeparse | docling |
|---|---|---|---|---|---|---|---|---|---|
| text_eds | 0.937 | 0.938 | 0.874 | 0.757 | 0.813 | 0.803 | 0.761 | 0.781 | 0.908 |
| head_teds | 0.862 | 0.866 | 0.021 | 0.009 | 0.586 | 0.763 | 0.778 | 0.384 | 0.471 |
| seg_kt | 0.997 | – | 0.956 | 0.756 | 0.980 | 0.990 | 0.989 | 0.986 | 0.993 |

## Notes

- The split behaves like the holdout: on arXiv RayoPDF is below the best tools on text, tables
  and segment order and at pymupdf's level on headings; on GitHub it is above every tool on every
  key. Each tool's values lie within a few hundredths of its holdout values.
- READoc's text metric removes `$…$` from the ground truth and from the output before comparing,
  so a formula written as LaTeX costs nothing and one written as Unicode text counts as inserted
  text: that is the gap between RayoPDF's two modes on arXiv `text_eds` (0.098), and why
  liteparse, which writes LaTeX, leads the text-layer tools there. Its table metric reads pipe
  tables only: an HTML table (RayoPDF writes one when cells span) counts as missing.
- Failures: edgeparse fails on 2 documents and times out on 1 (120 s); pymupdf times out on 1 arXiv document (eight others that timed out beside other jobs converted when run again on the idle machine); every other tool converts all 300. A failed document scores 0 on every key.
