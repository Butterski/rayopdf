# Release check of 1.0.0, 2026-11-25

RayoPDF 1.0.0 against the release bar of
[roadmap.md](../development/roadmap.md#goal-and-release-bar). 1.0.0's converter is 0.5.0's: a
static build of each has the same size and they differ in 30 bytes, the version string in its
five places and the build ID. So the measurements of the
[release check of 0.5.0](release-check-2026-11-24.md) hold for 1.0.0: quality, speed, the
26.1 h fuzz campaign over all 25 targets and the ASan sweeps were run on that code.

**Result: every item of the bar holds, as for 0.5.0.**

| item | status | detail |
|---|---|---|
| (a) quality ≥ best text-layer tool; NID and MHS ≥ docling | met | as 0.5.0 ([v0.5.0](v0.5.0-2026-11-24.md)) |
| (b) TEDS ≥ docling | met | READoc arXiv `table_teds` 0.730 in READoc's declared options against docling's 0.656 (0.609 in LaTeX mode); odl, synthetic and olmOCR as 0.5.0 |
| (c) fresh ≤ 1 ms/page, warm ≥ 2,000 pages/s | met | 0.912 ms fresh, 2,207 pages/s warm (0.5.0's code, 2026-11-24) |
| (d) 24 h clean fuzzing per parser entry point; ASan sweep 0 crashes, 0 hangs | met | 25 targets, 26.1 h aggregate, no finding; ASan sweeps of 2,837 PDFs in three modes clean (0.5.0's code) |
| (e) binary size, package size, peak RSS per document reported | met | below |

## The release binary

The Release workflow builds the release binary on GitHub's runners. A dry run of the workflow on
the release's code (`c36ff52`, run 45591963080) built a Linux binary (SHA-256 `a144019c…`,
3,991,968 B) that differs from 0.5.0's release binary (`2619cfa4…`) in 30 bytes: the version
string in its five places and the build ID. 0.5.0's release binary was timed against the binary
measured for 0.5.0 and was equal within the noise
([release check of 0.5.0](release-check-2026-11-24.md#c-speed)), so those times hold for 1.0.0.

## Footprint

The seven core packages as the Release workflow's dry run built them on `c36ff52` (the tagged
archives differ by a few bytes: their entries' time is the tagged commit's). The archives are
about 7 KB smaller than 0.5.0's: the README sections they carry are shorter.

| artifact | Linux x64 (static) | Windows x64 |
|---|---|---|
| `rayopdf` | 3,991,968 B | 5,190,054 B |
| core archive | 2,011,721 B | 1,966,275 B |
| C++ API archive (`librayopdf.a`, header, example) | 1,860,971 B | 1,959,314 B |
| Python wheel | 1,650,750 B | 1,934,733 B |

The npm package is 1,156,850 B. The OCR add-on and its archives are 0.5.0's code; their sizes and
the peak RSS per document are in the [release check of 0.5.0](release-check-2026-11-24.md#e-footprint).

## Builds

On the 1.0.0 tree: release and ASan + UBSan 31 of 31 tests, Windows (Wine), OCR and the wheel
configuration (with `tests/verify_python.py`) 32 of 32, the WebAssembly configuration 1 of 1,
0 warnings in each; the static and OCR Windows builds, 0 warnings; `tools/check_repo.py`
0 findings.
