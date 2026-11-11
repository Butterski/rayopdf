# Benchmark records

Every record is a dated run of the harness that [evaluation.md](../development/evaluation.md)
specifies, on one machine (AMD Ryzen 5 PRO 4650GE, Docker). The roadmap's gates are stated
against the baselines; each version record compares RayoPDF with them.

## Current

| Record | Contents |
|---|---|
| [baselines-2026-11-05.md](baselines-2026-11-05.md) | The competitors on every suite, the census; the reference for every gate |
| [baselines-scanned-2026-11-07.md](baselines-scanned-2026-11-07.md) | The OCR references on the scanned suites |
| [baselines-readoc-dev-2026-11-10.md](baselines-readoc-dev-2026-11-10.md) | The READoc development split (`readoc-dev`): RayoPDF and the competitors on 300 READoc documents outside the holdout, beside their holdout scores |
| [v0.0.14-2026-11-11.md](v0.0.14-2026-11-11.md) | The latest version on every suite, core and bundle, both math modes, the holdouts once; the release bar's READoc arXiv keys in LaTeX mode |
| [v0.0.13-2026-11-11.md](v0.0.13-2026-11-11.md) | The latest wall-clock speed against 0.0.12 |
| [v0.0.12-2026-11-10.md](v0.0.12-2026-11-10.md) | The comparison of speed with other tools |
| [release-check-2026-11-09.md](release-check-2026-11-09.md) | 0.0.10 (and the 0.0.11 update) against the 0.1.0 release bar |

## History

Each version record links its predecessor.

| Version | Date | Record | Headline change |
|---|---|---|---|
| 0.0.1 | 2026-11-05 | [v0-2026-11-05.md](v0-2026-11-05.md) | The first converter against the baselines |
| 0.0.2 | 2026-11-06 | [v0.0.2-2026-11-06.md](v0.0.2-2026-11-06.md) | Tables, images, footnotes, right-to-left and CJK text |
| 0.0.3 | 2026-11-06 | [v0.0.3-2026-11-06.md](v0.0.3-2026-11-06.md) | odl headings and tables, code listings, the 24-hour fuzz campaign |
| 0.0.4 | 2026-11-06 | [v0.0.4-2026-11-06.md](v0.0.4-2026-11-06.md) | Page furniture, reading order, run-in headings; every odl metric at or above the best text-layer tool |
| 0.0.5 | 2026-11-06 | [v0.0.5-2026-11-06.md](v0.0.5-2026-11-06.md) | Borderless tables, every Adobe CJK CMap, the full Unicode bidi algorithm |
| 0.0.6 | 2026-11-06 | [v0.0.6-2026-11-06.md](v0.0.6-2026-11-06.md) | Table detection; olmOCR born-digital categories except `arxiv_math` at or above the best text-layer tool |
| 0.0.7 | 2026-11-07 | [v0.0.7-2026-11-07.md](v0.0.7-2026-11-07.md) | Tagged PDF, batch and serve, Windows build, ToUnicode repair, JPEG decoding, the OCR/ML gate |
| 0.0.8 | 2026-11-07 | [v0.0.8-2026-11-07.md](v0.0.8-2026-11-07.md) | The first performance campaign, output byte-identical to 0.0.7 |
| 0.0.9 | 2026-11-08 | [v0.0.9-2026-11-08.md](v0.0.9-2026-11-08.md) | The OCR add-on, scan codecs, glyph-shape recovery, exact recovery |
| 0.0.10 | 2026-11-09 | [v0.0.10-2026-11-09.md](v0.0.10-2026-11-09.md) | OCR scripts and rotation, performance rounds 2 and 3, a quality round |
| 0.0.11 | 2026-11-09 | [v0.0.11-2026-11-09.md](v0.0.11-2026-11-09.md) | Optional LaTeX math (`--math=latex`) |
| 0.0.12 | 2026-11-10 | [v0.0.12-2026-11-10.md](v0.0.12-2026-11-10.md) | Performance round 4: 2,215 pages/s warm, 0.885 ms fresh, pages on threads; output byte-identical to 0.0.11 |
| 0.0.13 | 2026-11-11 | [v0.0.13-2026-11-11.md](v0.0.13-2026-11-11.md) | Quality round 5 on scientific papers (READoc arXiv headings 0.473 → 0.688), the READoc development split, images on page threads |
| 0.0.14 | 2026-11-11 | [v0.0.14-2026-11-11.md](v0.0.14-2026-11-11.md) | Segment order and tables on papers (READoc arXiv seg_kt 0.981, table_teds 0.604, LaTeX mode); round 5's speed cost won back; two fuzz findings fixed |

## Naming

- `v<version>-<date>.md`: one record per version measured on the harness (`v0-2026-11-05.md`
  is 0.0.1, named before this rule).
- `baselines-<date>.md` and `baselines-<suite>-<date>.md`: the competitors without RayoPDF.
- `release-check-<date>.md`: a version against the release bar of the
  [roadmap](../development/roadmap.md#goal-and-release-bar).

Add each new record to this index in the same commit, and move the record it replaces from
Current to History.
