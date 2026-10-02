# Mimic Snapshot-Global Modules Acceptance Record

**Purpose:** Record the measured regression and execution evidence for the snapshot-global modules increment (Slice 4 of [the implementation plan](MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md)). Every number below was read from the named logs, not from an exit-code summary.

**Status:** every gate in this record passed except one finding that predates this feature. The version 2 `micro-uchuu-ascii-horizontal` real-data gate passes all four bitwise parity legs but fails its Stage 8 (vertical-path preservation against `aedded2f`) on six `RunProperties/Version` `version` attribute additions that commit `99ee3055` introduced before any feature code; the same failure reproduces at the pre-feature reference commit. The human ruled it pre-existing and out of scope for this increment (2026-10-03). It is recorded as a residual with a recommended follow-up, not as a feature regression. See [the real-data gates](#real-data-cross-format-gates) and [the residuals](#residuals-and-plan-defects).

**Scope of the evidence:** the committed synthetic micro-Uchuu and mini-Millennium fixtures isolate the new contract with a known oracle; the real-data runs below have global modules disabled. An enabled real-data science or scale run is outside this increment. **No production-scale global-run memory and no observational validation were measured.**

## Commits and selectors

| Item | Value |
|---|---|
| Feature tree (HEAD at every run) | `d5eaec2d500ae80d087167e596188c0653ee1126`; this slice's changes touch only `Makefile`, `tests/`, `.agents/skills/mimic-validation-and-qa/SKILL.md` and this record, none under `src/`, `scripts/` or the non-test parts of `models/` (checked by the identity test itself) |
| Reference commit (disabled-mode identity) | `501bac12f654d9622b797bc9b26c536e5385aca2`, derived by the test as the parent of the first commit after the plan's last change that touches a non-planning path (`a74fea44`); no `REFERENCE_COMMIT` was supplied |
| Slice 1 `before_head` recorded by PM | `7a3f661c762a75134cb5ec9bb037d03da08fbf92`; see [the residuals](#residuals-and-plan-defects) for why this is not the derived value |
| Default selectors | `MODEL=sage16 SIMULATION=mini-millennium`, restored and rebuilt after every run |
| Fixture selectors | `halos-only` on `micro-uchuu-ascii-horizontal` (v2, adjacent links) and `mini-millennium-horizontal` (v3, gapped links); `sham` on `micro-uchuu-ascii-horizontal` |
| Real-data gate selectors | `MODEL=halos-only SIMULATION=<package> tests-scientific` for the six packages below, at HEAD `d5eaec2d` |

`501bac12` and `7a3f661c` differ in no path under `src/`, `scripts/`, `models/` or `Makefile` (`git diff --name-only 501bac12 7a3f661c` lists documentation, two gate test files and planning records only), so the runtime tree the identity comparison used is the one PM recorded.

## Command and log identifiers

Logs live in machine-local, git-ignored locations; they are operational records, not technical authority. Test-tier logs: `archive/snapshot-global-gates/20261002T143729Z/default-suite-logs/` (numbered `01`–`13`). Gate logs: `archive/snapshot-global-gates/20261002T143729Z/NN_<package>.log`. Identity evidence: `archive/snapshot-global-identity/20261002T143610Z-ref501bac12-headd5eaec2d/evidence.json` plus its `logs/`, `runs/` and `worktrees/`.

| # | Command | Exit | Elapsed | Result |
|---|---|---:|---:|---|
| 01 | `make MODEL=sage16 SIMULATION=mini-millennium tests summary` | 0 | 383 s | unit `total=51 passed=51 failed=0`; integration and scientific tiers passed; 21 SKIP, 1 WARN (listed below) |
| 02 | `make tests-horizontal-v3` | 0 | 18 s | `PASS: tests-horizontal-v3 (4 C tests, 2 Python tests, no unexpected skips)` |
| 03 | `make MODEL=sham SIMULATION=mini-millennium generate validate-modules lint-parameters check-generated` | 0 | 1 s | validation passed; generated code up to date |
| 04 | `make MODEL=sham SIMULATION=mini-millennium tests summary` | 0 | 340 s | legacy `sham_assign_stellar_mass` tests and the default tiers passed; unit `total=32 passed=32 failed=0`; 32 SKIP, 1 WARN |
| 06 | `make tests-snapshot-global` | 0 | 31 s | 47 cases counted by the target plus the 27-case sham battery, no skips |
| 07 | `make tests-snapshot-global-identity` | 0 | 62 s | 15 stages passed, 12 legs, 0 per-ID mismatches |
| 08 | `make check-docs` | 0 | — | documentation checks passed |
| 09 | `make check-format` | 0 | — | C, Black and isort passed (229 Python files unchanged) |
| 10 | `make tests-scientific` (default pair, no summary) | 0 | 11 s | 19 PASS, 1 WARN |
| 11 | `make tests-integration` (default pair, no summary) | 0 | 50 s | 230 PASS, 11 SKIP |
| 13 | `./scripts/beautify.sh` | 0 | — | no file changed |
| G1–G6 | the six gate commands | see below | see below | five passed, one failed |

Step 01's first attempt was lost when the suite's own `make clean` removed `build/`; it was re-run unchanged and the logs above are from that re-run. Tests 10 and 11 were run separately only to obtain per-tier PASS counts that summary mode hides.

## Fixture battery: `make tests-snapshot-global`

The target builds each pair as a test build, runs the declared tests by path, and fails on any build or test exit status, any `MIMIC_RESULT: FAIL`, `ERROR` or `SKIP`, and any PASS count different from the number of cases the test file declares. It restores the caller's generated code on every exit. It was exercised for its failure paths on this tree: a case dropped from the runner list (10 of 11 PASS, target exits non-zero), an injected `TestSkipped` (rejected), an injected assertion failure (rejected), a nonexistent simulation (build failure reported, run continues, exits non-zero) and a SIGINT during the run (generated code restored, non-zero exit).

| Step | Selector | Expected cases | Executed | Skips |
|---|---|---:|---:|---:|
| `test_snapshot_phase.py` | `halos-only` / `micro-uchuu-ascii-horizontal` | 11 | 11 | 0 |
| `test_snapshot_module_contract` (C) | same build | 16 | 16 | 0 |
| `test_snapshot_module_schema.py` | same build | 9 | 9 | 0 |
| `test_snapshot_phase.py` | `halos-only` / `mini-millennium-horizontal` | 11 | 11 | 0 |
| `tests-snapshot-global-sham` | `sham` / `micro-uchuu-ascii-horizontal` | 27 (18 C + 9 Python) | 27 | 0 |

The phase tests cover, through the executable: one call per snapshot in YAML order for empty, non-output and final snapshots and for different substep schemes; the current generation only (Types 0/1/2, never Type 3); writes visible to the next callback and inherited across adjacent links and across the v3 empty-snapshot gap; rejection of `module_emit_event` from inside a callback; callback failure with module, snapshot and return code and a cleaned-up output directory; and `RunProperties/EnabledModules` provenance. `test_snapshot_fixture` logs one expected `ERROR` line per snapshot from its deliberately rejected emission probe; that line is the evidence, not a failure.

The sham battery covers the hand-solvable oracle (`16`, `16/3`, `16/5` for the tie case), independent-oracle ranks, the global effect of a higher proxy in another FoF, permutation and repeat bitwise identity, the zero-proxy, empty and all-ineligible populations, float peak storage bounds, malformed input, the numerical edge battery and its materialised-density detector, per-ID `StellarMass` in the written HDF5 for every fixture snapshot, the cross-FoF tie on a temporary derivative, rejection alongside legacy `sham_assign_stellar_mass`, invalid parameters and an above-range rank mass. Numerical oracle results: every case passed within the plan's stated bounds (exact bytes for same-build comparisons, at most two float ULPs against the independent 60-digit Decimal reference). No tolerance was added or negotiated.

This slice changed one test: `test_standalone_fallback_keeps_three_modes` in `test_snapshot_module_schema.py` asserted a line of the validator's source text; it now runs the validator's own discovery over a temporary standalone `.c` file and checks the modes it synthesises.

## Disabled-mode identity: `make tests-snapshot-global-identity`

`tests/manual/test_snapshot_disabled_identity.py` lives outside every auto-discovered tier and is invoked only by its target. It resolved the reference to `501bac12f654d9622b797bc9b26c536e5385aca2` (derived; ancestor of HEAD; no `process_snapshot`, `PROCESSING_MODE_SNAPSHOT`, `SnapshotContext` or `post_snapshot` under `src/`, `scripts/` or `models/`; every simulation package, config and input path the legs select present), and refused, as designed, an explicit value naming HEAD, a non-commit and an older commit that differs outside the planning surface. It built twelve detached worktrees (six per commit) in 37 s of build time, and 62.2 s end to end. Every run recorded `RunProperties/Version@git_commit` equal to its worktree's commit, the leg's `TimestepScheme`, `ModelName` and `SimulationName`, and no run reported a memory leak.

Each leg compared two feature runs (`modules.post_snapshot` absent, and `post_snapshot: []` under `modules:`) with the reference run of the same run file. SAGE legs use the shipped `sage16_mini-millennium.yaml` verbatim apart from its selectors, so metal enrichment and its parameters are included; halos-only replaces only its `modules` mapping. Row counts equal the planning preflight: 307518 halos-only and 173205 (fixed) / 173197 (dynamic) sage16 rows on the vertical fixture, 13 / 11 on v2, 6 / 4 on gapped v3.

| Model | Fixture | Scheme | Galaxies | Snapshots | Fields | Per-ID mismatches (absent / empty) |
|---|---|---|---:|---:|---:|---|
| halos-only | vertical mini-Millennium | fixed | 307518 | 64 | 20 | 0 / 0 |
| halos-only | vertical mini-Millennium | dynamic | 307518 | 64 | 20 | 0 / 0 |
| halos-only | v2 micro-Uchuu | fixed | 13 | 6 | 20 | 0 / 0 |
| halos-only | v2 micro-Uchuu | dynamic | 13 | 6 | 20 | 0 / 0 |
| halos-only | v3 gapped mini-Millennium | fixed | 6 | 5 | 20 | 0 / 0 |
| halos-only | v3 gapped mini-Millennium | dynamic | 6 | 5 | 20 | 0 / 0 |
| sage16 | vertical mini-Millennium | fixed | 173205 | 64 | 42 | 0 / 0 |
| sage16 | vertical mini-Millennium | dynamic | 173197 | 64 | 42 | 0 / 0 |
| sage16 | v2 micro-Uchuu | fixed | 11 | 6 | 42 | 0 / 0 |
| sage16 | v2 micro-Uchuu | dynamic | 11 | 6 | 42 | 0 / 0 |
| sage16 | v3 gapped mini-Millennium | fixed | 4 | 5 | 42 | 0 / 0 |
| sage16 | v3 gapped mini-Millennium | dynamic | 4 | 5 | 42 | 0 / 0 |

Each comparison required the same file set, the same HDF5 objects and attributes, identical dataset presence, byte-equal `RunProperties/Parameters`, `FieldMetadata`, `EnabledModules`, `EventContracts` and `Redshifts` where present (SAGE legs compared all five; halos-only legs compared `FieldMetadata` and `Redshifts`), a byte-identical `metadata/output_schema.json`, and identical IDs and per-ID raw bytes at every output snapshot. The permitted differences that actually occurred, named in `evidence.json` for every leg, were exactly: `RunProperties/Version@git_commit`, `RunProperties/Version@version`, `RunProperties@RunEndTime` (only when the two runs ended in different seconds), `metadata/version_info.json`, and, in the explicit-empty-list legs only, the added `post_snapshot: []` line of `metadata/identity.yaml`. No path-valued attribute or metadata file differed, because every run used the same relative run-file paths. No numeric tolerance or comparator exception exists.

The comparator proved itself against 30 mutation and control cases on real output before the legs were trusted: untouched and rewritten copies accepted; a dropped ID, a duplicated ID, a one-bit field change, a galaxy in the wrong snapshot, payloads swapped between two IDs, each of the five named datasets missing or altered, an extra dataset, an altered or added attribute, an altered output schema, an altered or missing metadata file, and the empty-list line outside its leg, absent from its leg, or accompanied by another change all rejected; the permitted provenance differences and a path-prefix-only difference accepted and named.

## Real-data cross-format gates

Each ran `make MODEL=halos-only SIMULATION=<package> tests-scientific` one at a time at HEAD `d5eaec2d`, building its own worktrees, with every gate's existing model, epoch and file coverage, the existing bitwise comparator and the existing baseline anchor. None skipped for a missing dataset.

| Gate | Source | Exit | Elapsed | Population | Legs (fixed / dynamic) |
|---|---|---:|---:|---|---|
| `micro-uchuu-ascii-horizontal` | v2 | 2 | 7:00 | 4409643 halos-only, 3112186 / 3112152 sage16 galaxies, 8 snapshots | all four parity legs PASS; Stage 8 FAIL (pre-existing, see below) |
| `mini-millennium-horizontal` | v3 | 0 | 0:41 | 292163 halos-only, 187832 / 187817 sage16 | PASS |
| `micro-uchuu-horizontal` | v3, L-Halo | 0 | 0:44 | 4409643 | PASS |
| `micro-uchuu-hdf5-horizontal` | v3, Consistent-Trees HDF5 | 0 | 0:46 | 4409643 | PASS |
| `millennium-horizontal` | v3, files 0–511 (whole simulation) | 0 | 15:09 | 148798431 | PASS |
| `mini-uchuu-horizontal` | v3, files 0–127 (whole simulation) | 0 | 27:43 | 299185269 | PASS |

Every leg's comparator line read `PASSED: <N> galaxies over 8 output snapshot(s) are bitwise identical in all <20 or 42> field(s), with identical UniqueGalaxyID sets and no duplicates`; no log contains a memory-leak line. The generic core scientific tests inside each gate run passed; `test_zero_values` warned (`2 field(s) with zero values`) in each, and `micro-uchuu-hdf5-horizontal` skipped its four generic core tests by design because the package ships no committed fixture (its gate stages all passed).

**The `micro-uchuu-ascii-horizontal` Stage 8 failure (pre-existing, ruled out of scope).** Every parity stage of this gate passes (preconditions, dataset provenance, run files, builds, and the halos-only and sage16 fixed and dynamic legs, all bitwise identical). `stage_tree_path_preservation` reports `6 HDF5 metadata difference(s) beyond the 0 permitted deltas`. Re-run with the runner's first-line truncation removed, the six are `halos.hdf5` and `halos_000`–`halos_004`: `/RunProperties/Version attr version: added`. The gate's anchor `BASELINE_COMMIT = "aedded2f"` predates commit `99ee3055` (2026-09-30, "Record the release name in every output and the startup banner"), which added that attribute in `src/io/output/metadata_hdf5.c`, and the gate's `PERMITTED_DELTAS` is still empty. The galaxy records themselves match (`4409643 galaxy records byte-identical to the aedded2f baseline`). The same gate run from a worktree of the reference commit `501bac12`, before any feature code, fails identically with the same six differences. Logs: `archive/snapshot-global-gates/20261002T143729Z/01_micro-uchuu-ascii-horizontal.log` (HEAD `d5eaec2d`) and `archive/snapshot-global-gates/preexisting-stage8-at-501bac12/gate.log` (reference `501bac12`). The human ruled this pre-existing and out of scope; the gate file, `PERMITTED_DELTAS` and `BASELINE_COMMIT` were not touched. Recommended follow-up, separate from this increment: add the `version` delta to `PERMITTED_DELTAS` (with the matching `assert_output_schema_delta` expectation, pinned to its exact before and after) or re-anchor `BASELINE_COMMIT` per the in-file procedure in `simulations/micro-uchuu-ascii-horizontal/_tests/scientific/test_cross_format_identity.py`, then re-run that gate.

## Memory and leak results

Every Mimic run launched by the framework tests, the identity test (36 runs) and the gates is checked for an allocator leak report; none was reported in any log of this record. No production-scale memory measurement was made: the largest populations exercised were the whole-simulation gates with global modules disabled, and the global-rank module ran only on the committed fixtures (`test_no_allocation_growth_across_snapshots` checks that module scratch does not accumulate across snapshots).

## Skips and warnings

Default pair (`sage16`/`mini-millennium`), 21 SKIP, each with a stated configuration reason: the 7 horizontal-execution phase cases (`configuration SKIP: selected package mini-millennium is not a horizontal package with a committed fixture`), 4 `test_horizontal_*` cases (the selected package is not horizontal), 9 v3 reader cases (`the v3 fixture's /schema matches only SIMULATION=mini-millennium-horizontal`) and `test_unknown_module_error` (needs process isolation). Under `MODEL=sham SIMULATION=mini-millennium` there are 11 more: the 9 `sham_global_rank` Python cases (the package is vertical; they run under `tests-snapshot-global-sham`) and the two baseline comparisons (the committed baseline is for `sage16`). One WARN, `test_zero_values`, is pre-existing. The target batteries (`tests-snapshot-global`, `tests-horizontal-v3`) have zero unexpected skips.

## Residuals and plan defects

1. **Stage 8 of the `micro-uchuu-ascii-horizontal` gate (pre-existing, not a feature regression).** Described above; the follow-up is the human's. The plan's all-six-gates requirement is met for every parity stage of every gate and for the five gates that have no Stage 8; this one stage stays red until the follow-up lands.
2. **Reference derivation versus `before_head` (plan defect, resolved on the record).** The plan calls the reference both "the `before_head` PM records for Slice 1" (`7a3f661c`) and the value derived by walking history from the plan's last change. In this history, commit `a74fea44` (documentation and two gate test files) follows the last plan change, so the derivation yields its parent `501bac12`, and an explicit `7a3f661c` is refused because it differs from that in non-planning paths. The human and PM ruled that the derived `501bac12` is correct and the test keeps following the derivation. The two commits differ in no runtime path, so the evidence holds for both.
3. **Earlier-slice rulings carried unchanged:** the run-file parser keeps its own mode-name mirror because `tests/unit/tools/build_topology_dump.sh` links it without the module system; `parse_phase_config` takes only the first pair of a multi-key entry for the FoF phases (fixed for `post_snapshot` only); `scripts/fuzz_pipeline.py` does not know `post_snapshot`; `make validate-modules` does not load test properties under a test build, so production-only validators are applied before the test-build step; Type 3 exclusion from snapshot populations is evidenced at unit level because no integration fixture creates a Type 3 galaxy.
4. The default suite's own `make clean` removes `build/`, including logs written there; the logs of this record were copied out of it.
5. No production-scale global-run memory or observational validation was measured, and no scientific baseline was regenerated.
