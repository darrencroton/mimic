# Mimic Performance Acceptance Record (Step 4, Tier A)

**Purpose:** The evidence of record for [`MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md`](MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md): the baseline every slice is measured against, and one dated section per slice with its gate result, wall-time pair, cost line and findings. Nothing outside `docs/dev/` may cite this file.

**Status:** Open. Created by Slice 1 (2026-10-21); each later slice appends its section, and the closing slice adds the re-profile, the super-forest measurement and the spectrum annotation.

---

## Table of Contents

1. [Baseline](#baseline)
2. [Slice 1: The reference identity gate](#slice-1-the-reference-identity-gate)

---

## Baseline

The numbers the plan was designed against, copied from its Frozen Facts and Evidence. Every figure is from one machine (Mac Studio, Apple M3 Ultra, 32 cores, 512 GiB, macOS 27.0.1) and one build (`cc -g -O2`, HDF5 1.14.6, libyaml 0.2.5, MPI off); run-to-run noise on the default pair is 1.6% (one standard deviation over 14 repeats).

**The measurement of 2026-10-20** (commit `5daa24ee`, Mac Studio, Apple M3 Ultra, macOS 27.0.1, `cc -g -O2`, HDF5 1.14.6, libyaml 0.2.5, MPI off; `scripts/profiling/` harness per its README: `run_wall.py` 15 repeats with the first discarded, `sample_runs.sh` 20 repeats with the first discarded, `attribute.py --skip-first --wall-mean-s`; `source_attribution.mapped_fraction` 1.0 on both). Not comparable with the 2026-08-20 figures (3.3229 s), which were taken on a different machine.

| | Vertical (`sage16_mini-millennium.yaml`) | Horizontal (`sage16_mini-millennium-horizontal.yaml`) |
|---|---|---|
| Wall, mean ± σ (n = 14) | 2.6335 ± 0.0427 s (min 2.5866, max 2.7466) | 2.7251 ± 0.0403 s (min 2.6904, max 2.8469) |
| Peak RSS (run profile) | 0.066 to 0.074 GB | 0.176 to 0.185 GB |
| Largest live-buffer population `P` / galaxy pool high-water | 14,648 (one tree) / 15,525 | 37,425 (one snapshot) / 38,743 |
| Physics modules | 46.11% | 48.02% |
| Core execution, of which `module_registry.c` | 34.55%, 30.00% | 32.80%, 28.40% |
| Utilities, of which `error.c` | 5.86%, 3.30% | 5.67%, 3.13% |
| Math library | 4.65% | 4.64% |
| Output I/O, of which kernel `__open` via libhdf5 | 4.31%, 2.36% | 2.75%, 2.21% |
| Runtime/system, of which `__bzero` | 3.34%, 2.14% | 2.44%, 0.28% |
| Input I/O | 0.90% | 3.16% |
| `SubSteps: 1` differential | 0.7136 ± 0.0208 s (n = 6), so 0.2133 s per substep and about 0.50 s fixed | not measured |

Hottest lines (vertical, %): `module_registry.c:1603` 5.06 (the by-galaxy `process()` call), `sage_calculate_cooling_budget.c:132` 4.83, `sage_radio_mode_heating.c:248` 4.34, `error.c:211` 3.30, `module_registry.c:1598` 2.98, `sage_apply_star_formation_supernova.c:194` 2.77, `module_registry.c:0` 2.71, `module_registry.c:1604` 2.56, kernel `__open` 2.36, `__bzero` 2.14. The horizontal list is the same set in a slightly different order. Per-line attribution inside a translation unit is soft under `-O2` (the harness's own caveat), which is why Slice 2 exists. The raw profiles are machine-local scratch and are not cited; the facts above are what they showed.

**The research-scale measurement of 2026-10-21** (commit `8613bded`, same machine and build, `sage16` on all 128 mini-Uchuu files, 1,451,359,554 halos, the vertical run file's `last_file` raised to 127 in a scratch copy; 3 wall repeats after a warm-up, 2 sampled runs at a 5 ms interval, about 700,000 samples per driver; full tables, method and the derived cost model in [`MIMIC-PERFORMANCE-REFERENCE.md`](MIMIC-PERFORMANCE-REFERENCE.md)).

| | Vertical (`mini-uchuu`) | Horizontal (`mini-uchuu-horizontal`) |
|---|---|---|
| Wall, mean of 3 | 2167.0 s, σ 7.1 s | 2267.1 s, σ 73.1 s (one slow repeat; the other two average 2225.0 s) |
| Peak RSS | 1.82 GB | 55 to 63 GB (retention pool at most 43.2 GB, 2 generations) |
| `C` / `P` | 3,853,400 / 743,295 (the 5× seed on a 770,680-halo forest) | 41,788,163 / 39,452,969 |
| `SubSteps: 1` | 548.3 s, so 179.9 s per substep and 368.4 s (17%) fixed | 573.2 s, so 188.2 s per substep and 385.0 s (17%) fixed |
| Physics modules | 49.53% | 49.62% |
| `module_registry.c` | 30.82% | 29.60% |
| `error.c` | 3.71% | 3.56% |
| Math library | 4.53% | 4.45% |
| Output I/O, of which kernel `__open` | 1.07%, 0.05% | 0.41%, 0.02% |
| `__bzero` / `_platform_memmove` | 2.16% / 0.09% | 0.52% / 2.28% |
| Input I/O | 0.87% | 2.85% |

---

## Slice 1: The reference identity gate

**What changed.** `make tests-reference-identity REFERENCE_COMMIT=<hash>` (`tests/manual/test_reference_identity.py`) requires per-`UniqueGalaxyID` byte identity, and identical files, HDF5 objects, attributes (apart from the named provenance) and `metadata/`, between a required ancestor commit and HEAD on the real mini-Millennium data: `{halos-only, sage16} x {vertical, horizontal} x {fixed, dynamic}`, eight legs. The worktree, reference and output-comparison helpers moved from `tests/manual/test_snapshot_disabled_identity.py` into `tests/framework/parity_gate.py` (parameterised where they read battery-local state), plus one helper for the reusable HEAD worktree (`head__<model>__<simulation>`), and both tests import them.

**Gate result, first green run** (2026-10-21, `REFERENCE_COMMIT=5daa24ee`). HEAD was `faa2228b`, the slice's starting commit, whose `src/`, `models/`, `simulations/`, `scripts/` and `Makefile` equal the reference's, so the two builds are the same code and this run proves the instrument, not a change. The slice's `Makefile` target was not yet committed, and the gate refuses a runtime tree with uncommitted edits, so the Makefile edit was set aside and the script was run directly as `python3 tests/manual/test_reference_identity.py`. Exit 0; eleven markers, all `PASS` (resolution, builds, eight legs, verdicts); 69.5 s with both sides' eight builds from cold (the HEAD worktrees were created by the run). Evidence: `archive/reference-identity/20261021T024358Z-ref5daa24ee-headfaa2228b/evidence.json` (machine-local).

| Leg | Verdict | Galaxies compared | Output snapshots | Fields | Master links | Galaxy differences | `P` (both runs) |
|---|---|---|---|---|---|---|---|
| `halos_only__vertical__fixed` | PASS | 292,163 | 8 | 20 | 128 | 0 | 40,537 |
| `halos_only__vertical__dynamic` | PASS | 292,163 | 8 | 20 | 128 | 0 | 40,537 |
| `halos_only__horizontal__fixed` | PASS | 292,163 | 8 | 20 | 8 | 0 | 82,137 |
| `halos_only__horizontal__dynamic` | PASS | 292,163 | 8 | 20 | 8 | 0 | 82,137 |
| `sage16__vertical__fixed` | PASS | 187,832 | 8 | 42 | 128 | 0 | 14,648 |
| `sage16__vertical__dynamic` | PASS | 187,817 | 8 | 42 | 128 | 0 | 14,643 |
| `sage16__horizontal__fixed` | PASS | 187,832 | 8 | 42 | 8 | 0 | 37,425 |
| `sage16__horizontal__dynamic` | PASS | 187,817 | 8 | 42 | 8 | 0 | 37,424 |

The `sage16` fixed legs report `P` of 14,648 (vertical, one tree) and 37,425 (horizontal, one snapshot), the baseline table's figures, so the legs ran on the real data. The only differences any leg permitted were `RunProperties/Version@git_commit`, `@git_date` and `@version`, `metadata/version_info.json`, and `RunProperties@RunEndTime`, which a leg names only when the two runs' end times differ: seven legs named it, and `halos_only__horizontal__dynamic` did not, because both runs ended within the same second.

**Gate result at the final code commit** (2026-10-21, `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` at `1820af9d`, the commit after the drift audit and review corrections; the acceptance record was committed afterwards, and `docs/dev/` is not a runtime path). Exit 0; eleven markers, all `PASS`, of which the eight `test_leg_` markers are the legs; no FAIL, ERROR, SKIP or NA; `HEAD build flags: none`; 38.4 s with both sides' worktrees already built. Every leg compared the same galaxy counts as the first run with zero galaxy differences, and the `sage16` fixed legs again report `P` of 14,648 (vertical) and 37,425 (horizontal). The permitted differences were `RunProperties/Version@git_commit`, `@git_date` and `@version` and `metadata/version_info.json` in every leg, and `RunProperties@RunEndTime` in seven of the eight (not `halos_only__vertical__fixed`, whose two runs ended in the same second), which is why the set a leg names varies from run to run. Evidence: `archive/reference-identity/20261021T034537Z-ref5daa24ee-head1820af9d/evidence.json` (machine-local).

**Cost.** One new test file, one moved-and-parameterised helper block in `tests/framework/parity_gate.py` (about 500 lines moved out of the battery, with three optional hooks in the comparison for the battery-only pinned-description and empty-list allowances), one Makefile target, documentation. No C change, no comparator change.

**Findings.** (1) The comparator's default mode excludes created rows (`UniqueGalaxyID` below zero) from the byte comparison and only counts them; the two models on mini-Millennium create none, so nothing is unchecked here, but a later plan whose slice creates records must use the comparator's `--compare-created`. (2) The gate refuses a runtime tree with uncommitted edits by design, so a slice's gate run is taken at its commit; a fix after a failed gate is a new commit.
