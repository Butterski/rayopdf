# Baselines on the second READoc development split, 2026-11-13

`readoc-dev2` is a development suite of the harness ([evaluation.md](../development/evaluation.md)):
300 arXiv and 300 GitHub documents of READoc (dataset `lazyc/READoc` at revision `782bf954`), the
documents 601 to 900 of each subset in the order the holdout, `readoc-dev` and `readoc-val` take
(ground-truth stems sorted by the SHA-256 of the stem), so the four sets share no document. Its
documents, outputs and per-document scores may be inspected. It came after rounds 5 to 8 designed
their rules for papers on `readoc-dev`, where RayoPDF's arXiv `table_teds` (0.712, LaTeX mode)
stood far above the validation split's (0.577): new rules are found on documents no rule was
fitted to. Scored with READoc's evaluator at commit `afa0477`, as the other READoc sets are.

Tools: RayoPDF 0.1.1 (code `2a8a6a8`, core alone, `rayopdf --convert IN OUT --math=latex`, the
mode the release bar measures READoc arXiv in), and the tools of the earlier baselines that matter
on papers: docling (the ML reference), pymupdf (the best text-layer tool on READoc arXiv tables)
and kordoc ([its baselines](baselines-2026-11-12.md)); versions as recorded there. RayoPDF, kordoc
and docling converted on 2026-11-12, pymupdf on 2026-11-13 (a first run beside other jobs timed
out on 25 documents). Same machine (AMD Ryzen 5 PRO 4650GE). Reproduced numbers.

## arXiv (300 documents)

| metric | RayoPDF `--math=latex` | pymupdf | kordoc | docling |
|---|---|---|---|---|
| text_eds | **0.882** | 0.752 | 0.787 | 0.840 |
| text_f1 | **0.908** | 0.836 | 0.815 | 0.845 |
| head_eds | **0.730** | 0.609 | 0.489 | 0.689 |
| head_teds | **0.692** | 0.530 | 0.292 | 0.393 |
| inline_eds | **0.651** | 0.003 | 0.002 | 0.003 |
| outline_eds | **0.560** | 0.000 | 0.000 | 0.000 |
| table_eds | 0.672 | 0.680 | 0.344 | **0.694** |
| table_teds | 0.594 | 0.632 | 0.283 | **0.640** |
| seg_kt | **0.977** | 0.972 | 0.956 | 0.974 |
| word_kt | **0.985** | 0.975 | 0.973 | 0.969 |
| seg_sp | **0.977** | 0.970 | 0.946 | 0.972 |
| word_sp | **0.980** | 0.968 | 0.964 | 0.961 |

Bold: the best tool.

The same keys on the other READoc sets (aggregates of the earlier records; development 150,
validation 300 and holdout 150 arXiv documents):

| set | RayoPDF 0.1.1 `--math=latex`: text_eds / head_teds / table_teds / seg_kt | docling |
|---|---|---|
| readoc-dev | 0.889 / 0.711 / 0.712 / 0.984 | 0.847 / 0.393 / 0.655 / 0.979 |
| readoc-val (aggregate) | 0.865 / 0.706 / 0.577 / 0.979 | 0.824 / 0.402 / 0.640 / 0.976 |
| readoc (holdout, aggregate) | 0.882 / 0.695 / 0.617 / 0.981 | 0.841 / 0.403 / 0.656 / 0.984 |
| readoc-dev2 | 0.882 / 0.692 / 0.594 / 0.977 | 0.840 / 0.393 / 0.640 / 0.974 |

## GitHub (300 documents)

| metric | RayoPDF `--math=latex` | pymupdf | kordoc | docling |
|---|---|---|---|---|
| text_eds | **0.934** | 0.819 | 0.844 | 0.904 |
| text_f1 | **0.919** | 0.799 | 0.836 | 0.886 |
| head_eds | **0.905** | 0.657 | 0.625 | 0.766 |
| head_teds | **0.885** | 0.598 | 0.422 | 0.474 |
| seg_kt | **0.996** | 0.987 | 0.990 | **0.996** |
| word_kt | **0.996** | 0.987 | 0.991 | 0.991 |
| seg_sp | **0.995** | 0.977 | 0.987 | **0.995** |
| word_sp | **0.995** | 0.983 | 0.987 | 0.988 |

Bold: the best tool.

## Notes

- The new split behaves like the validation split and the holdout, not like `readoc-dev`: arXiv
  `table_teds` 0.594 against docling's 0.640 (0.577 against 0.640 on the validation split, 0.617
  against 0.656 on the holdout, but 0.712 against 0.655 on `readoc-dev`). Headings, text and
  inline math stay well above every tool on all four sets. 0.2.0's table steps, found on this
  split, raise its arXiv `table_teds` to 0.620 ([v0.2.0](v0.2.0-2026-11-13.md)).
- Failures: docling times out on 2 arXiv documents (600 s), pymupdf on 1 (120 s); RayoPDF and
  kordoc convert all 600. A failed document scores 0 on every key.
