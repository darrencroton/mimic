# Mimic Snapshot-Global Modules Acceptance Record

**Purpose:** Record the measured regression and execution evidence for the snapshot-global modules increment (Slice 4 of [the implementation plan](MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md)). Every number below was read from the named logs, not from an exit-code summary.

**Status:** every gate in this record passed except one finding that predates this feature. The version 2 `micro-uchuu-ascii-horizontal` real-data gate passes all four bitwise parity legs but fails its Stage 8 (vertical-path preservation against `aedded2f`) on six `RunProperties/Version` `version` attribute additions that commit `99ee3055` introduced before any feature code; the same failure reproduces at the pre-feature reference commit. The human ruled it pre-existing and out of scope for this increment (2026-10-03). It is recorded as a residual with a recommended follow-up, not as a feature regression. See [the real-data gates](#real-data-cross-format-gates) and [the residuals](#residuals-and-plan-defects).

**Scope of the evidence:** the committed synthetic micro-Uchuu and mini-Millennium fixtures isolate the new contract with a known oracle; the real-data runs below have global modules disabled. An enabled real-data science or scale run is outside this increment. **No production-scale global-run memory and no observational validation were measured.**

## Commits and selectors

| Item | Value |
|---|---|
| Feature trees | The six real-data gates ran on `d5eaec2d500ae80d087167e596188c0653ee1126` (the slice's starting commit). The final code commit is `3ff253924f22b5f29195558e5ece9e53982c960f`; the slice's commits are `bdd267c9`, `49765243`, `08eb0546`, `2c2b8470`, `fbeede86`, `4cd50d1d`, `3ff25392` and the record commit that follows, and the record commits change only this file. Which rows ran on which commit: the default suite, `tests-horizontal-v3` and the legacy SHAM validation and tests (rows 01, 02, 05, 06 below) ran on `08eb0546`; `tests-snapshot-global` and `tests-snapshot-global-identity` were re-run on `fbeede86` and again on `3ff25392`, the final code commit (rows 03, 04 and the repeat paragraph); `check-docs`, `check-format` and `beautify.sh` (rows 07–09) ran on `08eb0546`. The default suite, `tests-horizontal-v3` and the legacy SHAM tests were not re-run after `fbeede86` and `3ff25392`, because those commits changed only the two snapshot-global Make targets, the identity test, `tests/README.md` and the validation skill, none of which those suites execute; PM re-runs them independently. Verified by `git diff --name-only d5eaec2d 3ff25392`: the slice range touches only `Makefile`, `tests/README.md`, `tests/integration/test_snapshot_module_schema.py`, `tests/manual/test_snapshot_disabled_identity.py`, `.agents/skills/mimic-validation-and-qa/SKILL.md` and this record, none under `src/`, `scripts/` or `models/`. The identity test itself only checks that no runtime path has an uncommitted edit. |
| Reference commit (disabled-mode identity) | `501bac12f654d9622b797bc9b26c536e5385aca2`, derived by the test as the parent of the first commit after the plan's last change that touches a non-planning path (`a74fea44`); no `REFERENCE_COMMIT` was supplied. PM ruled this derived value correct. |
| Slice 1 `before_head` recorded by PM | `7a3f661c762a75134cb5ec9bb037d03da08fbf92`; see [the residuals](#residuals-and-plan-defects) for why this is not the derived value |
| Default selectors | `MODEL=sage16 SIMULATION=mini-millennium`, restored and rebuilt after every run |
| Fixture selectors | `halos-only` on `micro-uchuu-ascii-horizontal` (v2, adjacent links) and `mini-millennium-horizontal` (v3, gapped links); `sham` on `micro-uchuu-ascii-horizontal` |
| Real-data gate selectors | `MODEL=halos-only SIMULATION=<package> tests-scientific` for the six packages below, at HEAD `d5eaec2d` |

`501bac12` and `7a3f661c` differ in no path under `src/`, `scripts/`, `models/` or `Makefile` (`git diff --name-only 501bac12 7a3f661c` lists documentation, two gate test files and planning records only), so the runtime tree the identity comparison used is the one PM recorded.

## Command and log identifiers

Logs live in machine-local, git-ignored locations; they are operational records, not technical authority. Test-tier logs: `archive/snapshot-global-gates/20261002T143729Z/default-suite-logs/` (numbered `01`–`13`). Gate logs: `archive/snapshot-global-gates/20261002T143729Z/NN_<package>.log`. Identity evidence: `archive/snapshot-global-identity/20261002T143610Z-ref501bac12-headd5eaec2d/evidence.json` plus its `logs/`, `runs/` and `worktrees/`.

Evidence for the plan's criterion 5. All rows ran sequentially on HEAD `08eb0546fa49ceb16c2741a75c2092402361a687` with a clean working tree (rows 03 and 04 were repeated on the final code commit, see below); logs in `archive/snapshot-global-gates/final-head-20261002T164214Z/`.

| # | Command | Exit | Elapsed | Result |
|---|---|---:|---:|---|
| 01 | `make MODEL=sage16 SIMULATION=mini-millennium tests summary` | 0 | 373 s | unit `total=51 passed=51 failed=0`; integration and scientific tiers passed; 21 SKIP, 1 WARN, no FAIL or ERROR (listed below) |
| 02 | `make tests-horizontal-v3` | 0 | 18 s | `PASS: tests-horizontal-v3 (4 C tests, 2 Python tests, no unexpected skips)` |
| 03 | `make tests-snapshot-global` | 0 | 31 s | 74 cases: 11 + 16 + 9 + 11 counted directly and the 27-case sham battery, every step gated on its declared count, no skips |
| 04 | `make tests-snapshot-global-identity` | 0 | 68 s | 17 stage markers PASS, 0 per-ID mismatches; evidence `archive/snapshot-global-identity/20261002T164946Z-ref501bac12-head08eb0546/evidence.json` |
| 05 | `make MODEL=sham SIMULATION=mini-millennium generate validate-modules lint-parameters check-generated` | 0 | 1 s | validation passed; generated code up to date |
| 06 | `make MODEL=sham SIMULATION=mini-millennium tests summary` (legacy SHAM module tests) | 0 | 370 s | unit `total=32 passed=32 failed=0`; tiers passed; 32 SKIP, 1 WARN, no FAIL or ERROR |
| 07 | `make check-docs` | 0 | 2 s | documentation checks passed |
| 08 | `make check-format` | 0 | 206 s | C, Black and isort passed |
| 09 | `./scripts/beautify.sh` | 0 | 36 s | `git status --short` empty afterwards: no file changed |
| 10 | default build restore (`generate`, then `make`) | 0 | 2 s | `Build complete` |

Repeated after the last review fixes (steer attempt 3), on HEAD `fbeede868857d6eae65376e1631f3ea8a79a1984` with a clean working tree (the commit changed only the two Makefile targets, the identity test's docstring and self-check skip, and `tests/README.md`): `make tests-snapshot-global` exit 0 in 31 s (74 cases, every step gated, no skips) and `make tests-snapshot-global-identity` exit 0 in 65.5 s (17 stage markers PASS, 0 per-ID mismatches, evidence `archive/snapshot-global-identity/20261002T174801Z-ref501bac12-headfbeede86/evidence.json`, same permitted-difference set). The default suite, `tests-horizontal-v3` and the legacy SHAM tests were not repeated: those edits cannot affect them, and their rows above stand on `08eb0546`. The review fix itself was demonstrated: with a test that prints all its markers and then exits 1, the previous Makefile passed the target, the fixed one fails it (`FAIL: ... exited 1`, exit 2). Repeated again after the comparator fixes (steer attempt 4), on the final code commit `3ff253924f22b5f29195558e5ece9e53982c960f` with a clean working tree: `make tests-snapshot-global` exit 0 in 32 s (74 cases, no skips) and `make tests-snapshot-global-identity` exit 0 in 66 s (17 stage markers PASS, 0 per-ID mismatches, same permitted-difference set, comparator self-check `36 mutation/control cases behaved as required`, evidence `archive/snapshot-global-identity/20261002T182951Z-ref501bac12-head3ff25392/evidence.json`). A `WARN` marker in a snapshot-global step now fails it with its own message (demonstrated with an injected warning). Further evidence rows:

| # | Evidence | Outcome |
|---|---|---|
| 11 | Style audit against `docs/STYLE-GUIDE.md` (style-guide skill, audit mode) of the changed Python, Makefile, README and skill files | no open finding; fixed on the way: a README duration claim, a natural-language log parse in the identity test, and a source-text assertion in the schema test |
| 12 | Differential lint, `python3 /Users/dcroton/Documents/AI/repos/ai-agent-coder/skills/lint/scripts/lint.py check --base d5eaec2d500ae80d087167e596188c0653ee1126`, run before every commit including `fbeede86` | `verdict: pass` (ruff-check, ruff-format, markdownlint, codespell: 0 findings) |

Earlier runs, kept for the record; the tree is named per row.

| # | Command | Tree | Exit | Elapsed | Result |
|---|---|---|---:|---:|---|
| E1 | `make MODEL=sage16 SIMULATION=mini-millennium tests summary` | `d5eaec2d` + slice files then uncommitted | 0 | 383 s | unit 51/51; 21 SKIP, 1 WARN |
| E2 | `make tests-horizontal-v3` | same | 0 | 18 s | pass |
| E3 | `make MODEL=sham SIMULATION=mini-millennium ... tests summary` | same | 0 | 340 s | unit 32/32; 32 SKIP, 1 WARN |
| E4 | `make tests-snapshot-global` | same | 0 | 31 s | 47 counted cases plus the 27-case sham battery, no skips |
| E5 | `make tests-snapshot-global-identity` | same | 0 | 62 s | 17 stage markers PASS, 0 per-ID mismatches |
| E6 | `make tests-snapshot-global` and `-identity` | `bdd267c9` | 0, 0 | 32 s, 67 s | same counts and results |
| E7 | `make tests-snapshot-global-identity` | `49765243` | 0 | 66 s | same results (run after the README footprint edit, evidence `archive/snapshot-global-identity/20261002T163654Z-ref501bac12-head49765243/`) |
| E8 | `make tests-scientific`, `make tests-integration` (default pair, no summary, for PASS counts) | `d5eaec2d` + slice files | 0, 0 | 11 s, 50 s | scientific 19 PASS, 1 WARN; integration 230 PASS, 11 SKIP |
| G1–G6 | the six gate commands | `d5eaec2d` | see below | see below | five passed, one failed in its pre-existing Stage 8 |

Logs of the earlier runs: `archive/snapshot-global-gates/20261002T143729Z/default-suite-logs/`. The first attempt at E1 was lost when the suite's own `make clean` removed `build/`; it was re-run unchanged. E8 was run only to obtain per-tier PASS counts that summary mode hides.

## Fixture battery: `make tests-snapshot-global`

The target builds each pair as a test build, runs the declared tests by path, and fails on any build or test exit status, any `MIMIC_RESULT: FAIL`, `ERROR` or `SKIP`, and any PASS count different from the number of cases the test files declare (`def test_` or `TEST_RUN(`); the sham step is gated the same way on its declared 18 C + 9 Python cases. Its recipe is split so that `make -n tests-snapshot-global` prints the test commands without running any test (verified: the dry run exits 0, starts no test process and the printed commands are only text). The caller's generated code is restored by the last step and, on an interrupt, by a trap each step installs; a failed restore fails the target. Failure paths exercised on the final tree: a case dropped from the phase runner (10 of 11 PASS, exit 2), an injected `TestSkipped`, an injected assertion failure, a case dropped from the sham Python runner (26 of 27, exit 2), a nonexistent simulation (build failure reported, run continues, exit 2), a restore forced to fail with `SG_RESTORE_TARGET=no-such-target` (`FAIL: could not restore generated code`, exit 2) and a SIGTERM during the run (generated code restored, exit 143). The identity target never changes this checkout's generated code (it builds only in worktrees), so it has no restore to fail.

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

`tests/manual/test_snapshot_disabled_identity.py` lives outside every auto-discovered tier and is invoked only by its target. It resolved the reference to `501bac12f654d9622b797bc9b26c536e5385aca2` (derived; ancestor of HEAD; no `process_snapshot`, `PROCESSING_MODE_SNAPSHOT`, `SnapshotContext` or `post_snapshot` under `src/`, `scripts/` or `models/`; every simulation package, config and input path the legs select present), and refused, as designed, an explicit value naming HEAD, a non-commit and an older commit that differs outside the planning surface. It built twelve detached worktrees (six per commit) in 37 s of build time, and 62.2 s end to end (the final-tree run at `08eb0546`: 67.6 s, with every run's recorded `git_commit` equal to its worktree). Its ordered stages run with `abort_on_failure`, so a failed prerequisite stops the chain (the remaining stages report SKIP, which fails the target), and it asserts that each run file's top-level keys are the shipped file's plus the two scheme keys. Every run recorded `RunProperties/Version@git_commit` equal to its worktree's commit, the leg's `TimestepScheme`, `ModelName` and `SimulationName`, and no run reported a memory leak.

Each leg compared two feature runs (`modules.post_snapshot` absent, and `post_snapshot: []` under `modules:`) with the reference run of the same run file. Every leg derives from the shipped `sage16_mini-millennium.yaml`. The harness substitutes, and asserts it substituted, only the model name, the simulation name and `simulation.config`, `output.output_directory`, `output.snapshot_list` (`[]`, meaning all snapshots) and the added `MaxDynamicSubsteps` and `TimestepScheme` keys, and asserts the parsed top-level keys are the shipped file's plus those two. For SAGE legs the `modules` mapping and every parameter stay identical to the shipped file, so metal enrichment is included; halos-only replaces only its `modules` mapping with an empty pipeline. Row counts equal the planning preflight: 307518 halos-only and 173205 (fixed) / 173197 (dynamic) sage16 rows on the vertical fixture, 13 / 11 on v2, 6 / 4 on gapped v3.

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

Each comparison required the same file set, the same HDF5 objects and attributes, identical dataset presence, byte-equal `RunProperties/Parameters`, `FieldMetadata`, `EnabledModules`, `EventContracts` and `Redshifts` where present (SAGE legs compared all five; halos-only legs compared `FieldMetadata` and `Redshifts`), a byte-identical `metadata/output_schema.json`, and identical IDs and per-ID raw bytes at every output snapshot. The permitted differences that actually occurred, named in `evidence.json` for every leg, were across all recorded runs exactly: `RunProperties/Version@git_commit`, `RunProperties/Version@version`, `RunProperties/Version@git_date` (from the `bdd267c9` run on, because the compared commits then fall on different dates), `RunProperties@RunEndTime` (only when the two runs ended in different seconds), `metadata/version_info.json`, and, in the explicit-empty-list legs only, the added `post_snapshot: []` line of `metadata/identity.yaml`. `Version@git_branch` and `Version@build_date` never differed. The set above is the union over the final-tree identity run, `archive/snapshot-global-identity/20261002T164946Z-ref501bac12-head08eb0546/evidence.json` (head `08eb0546fa49ceb16c2741a75c2092402361a687`, 67.6 s, 0 per-ID mismatches), and the earlier runs. No path-valued attribute or metadata file differed, because every run used the same relative run-file paths. No numeric tolerance or comparator exception exists.

The comparator proved itself against 36 mutation and control cases on real output before the legs were trusted: untouched and rewritten copies accepted; a dropped ID, a duplicated ID, a one-bit field change, a galaxy in the wrong snapshot, payloads swapped between two IDs, each of the five named datasets missing or altered, an extra dataset, an altered or added attribute, an altered output schema, an altered or missing metadata file, and the empty-list line outside its leg, absent from its leg, or accompanied by another change all rejected; a compound dataset with identical values but a different record layout, and a permitted provenance attribute (`Version@git_commit`, `RunEndTime`) with a changed dtype or shape, rejected (a permitted attribute may differ in value only; compound layout means record size and each field's name, order, dtype, shape and offset, while values are still compared field by field because padding differs between identical runs); the permitted provenance differences and a path-prefix-only difference accepted and named, with the path-prefix string-attribute allowance accepted on `RunProperties` and rejected on any other object.

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
2. **Reference derivation versus `before_head` (plan defect, resolved on the record).** The plan calls the reference both "the `before_head` PM records for Slice 1" (`7a3f661c`) and the value derived by walking history from the plan's last change. In this history, commit `a74fea44` (documentation and two gate test files) follows the last plan change, so the derivation yields its parent `501bac12`, and an explicit `7a3f661c` is refused because it differs from that in non-planning paths. PM ruled that the derived `501bac12` is correct and the test keeps following the derivation. The two commits differ in no runtime path, so the evidence holds for both.
3. **Reference derivation is not stable once the plan file changes (plan defect, open).** The derivation walks from the commit that last changed the plan file. Once the plan is edited or archived after the feature commits, that starting point moves past the feature commits, the derived value moves with it, and `make tests-snapshot-global-identity` refuses to run (a derived reference that already contains the feature fails the no-feature-symbol check). The test follows the frozen rule. Recommendation to the plan owner: pin the reference commit (record it in the plan or pass `REFERENCE_COMMIT=501bac12f654d9622b797bc9b26c536e5385aca2`) or change the rule before the plan is archived.
4. **Earlier-slice rulings carried unchanged:** the run-file parser keeps its own mode-name mirror because `tests/unit/tools/build_topology_dump.sh` links it without the module system; `parse_phase_config` takes only the first pair of a multi-key entry for the FoF phases (fixed for `post_snapshot` only); `scripts/fuzz_pipeline.py` does not know `post_snapshot`; `make validate-modules` does not load test properties under a test build, so production-only validators are applied before the test-build step; Type 3 exclusion from snapshot populations is evidenced at unit level because no integration fixture creates a Type 3 galaxy.
5. The default suite's own `make clean` removes `build/`, including logs written there; the logs of this record were copied out of it.
6. No production-scale global-run memory or observational validation was measured, and no scientific baseline was regenerated.
