# Mimic Snapshot-Global Modules Implementation Plan

**Purpose:** Add an explicit snapshot-wide module contract and demonstrate deterministic global abundance matching without changing existing FoF physics.

**Status:** Reviewed planning contract, revision 8 (2026-10-02; revision 8 records that the `millennium-horizontal` and `mini-uchuu-horizontal` real-data gates now cover the whole simulations and states their measured cost and scratch need, without changing any slice or contract; revision 5 resolved the final independent review's Slice 2 and Slice 4 wording findings, revision 6 moved the planning baseline past the horizontal micro-Uchuu package rename, and revision 7 removes every commit hash that must track the repository from this file, none changing scope). The July brief supplies intent; current code at the planning baseline determines the design. Independent reviews and executable planning checks are recorded in [the review record](MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-REVIEW.md) and [the checks record](MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-CHECKS.md). Existing-code fixture/build preflights have run; no snapshot-global feature is implemented or accepted. The user authorized committing these planning artifacts on 2026-10-01; implementation remains a separate approval.
**Planning baseline:** the commit named by `planning_baseline` in `docs/dev/snapshot-global-checks/fixtures/anchors.json`, which is always the last commit that changes anything outside the planning surface (this plan, the pathway, the two records and the checker directory). The hash lives in that checker fixture and not in this file on purpose: PM binds a run to the SHA-256 of this file's bytes, and a hash written here would change them every time an unrelated commit landed before the run, forcing a re-freeze. After any commit outside the planning surface, review the drift, record the review in the checks record and move that one fixture value; nothing in this file changes. Revisions 1–5 were baselined at `717cf3ed5647eb85d2426f44f7dcda7ecd695103` (HEAD at revision 3, 2026-09-30). Revision 6 moved the baseline because the horizontal micro-Uchuu packages were renamed after their source format (`micro-uchuu-horizontal` became `micro-uchuu-ascii-horizontal` and `micro-uchuu-lhalo-horizontal` became `micro-uchuu-horizontal`), four sage16 horizontal run files were added and one README claim was corrected. A drift review found no behavioural change: runtime sources differed only by comments in four files and one `Makefile` comment and path, every committed v2 fixture data file is byte-identical under the new name (the two manifests differ only in their generator path), no test globs run files, and the cited line anchors held. Revision 7 followed a further non-planning commit, adding the `plot_profile.yaml` files that the horizontal packages lacked, which no criterion depends on. The drift reviews and the preflights repeated at the later baselines are in the checks record. The working tree differs from the baseline only by the planning artifacts, which the planning checker verifies. The committed version of this file at the first baseline is the July requirements brief; it is superseded in full. Every claim below was checked against the code at `717cf3ed` during revisions 2 and 3 and carried to each later baseline by those reviews, and no assumption from the brief was carried forward unverified. Recheck relevant drift before execution; do not silently rebase the contract onto changed interfaces.
**Owner:** [Development pathway, step 2](MIMIC-DEVELOPMENT-PATHWAY.md#the-ordered-road). Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md).

## Outcome and Limits

A configured `modules.post_snapshot` list runs once after every horizontal snapshot's FoF sweep, including empty snapshots and snapshots not selected for output. Its callbacks borrow the current processed population, may update its galaxy properties, and finish before that generation is published for later inheritance or written. Existing FoF callbacks, events, ordering, timestep schemes, readers and output formats retain their contracts.

The first module is `sham_global_rank`, in `models/sham/`. It ranks peak circular velocities over the entire supplied snapshot and matches ranks to an explicit analytic cumulative stellar mass function. This is an uncalibrated framework demonstration, not an observationally validated galaxy model. It neither replaces nor changes `sham_assign_stellar_mass`. A small fixture proves the mathematical contract; it does not establish completeness of a real cosmological population.

This plan supports a single process and fully resident snapshots. It does not implement distributed reduction, chunked slab streaming, galaxy creation/deletion, topology changes, lightcones, HOD, rate callbacks, batch callbacks, scatter, observational fitting, external mass-function tables, or a self-consistent reionization field. A one-pass post-snapshot callback cannot claim to solve simultaneous source–field feedback; that needs a later explicit causal or iterative contract. Later mode families must remain possible without changing the existing FoF function signature.

The old brief's statement that Shin-Uchuu could not run locally was superseded by the completed corrected production run recorded in `simulations/shin-uchuu/README.md`. That result does not promise headroom for global sorting scratch. This feature's gate uses committed micro-Uchuu and mini-Millennium fixtures; no production-scale memory claim follows. Chunking remains separate.

## Repository Evidence and Design Decisions

- `src/core/horizontal_driver.c:1576–1650` sweeps current FoFs, publishes their processed generation, releases expired generations and writes selected snapshots. Insert the global call after the FoF coverage check and before publication. The current population is `cur->processed.halos[0:count]`, not the raw slab or every retained generation. An empty snapshot keeps a seeded, allocated, non-null buffer with count zero.
- `src/core/output_buffer.c:31–66` filters Type 3 and copies surviving halo records while retaining their galaxy pointers. This is the state later inheritance consumes; modifying its galaxy data requires no second copy or output-only fixup.
- `src/core/inheritance.c:140–144` writes workspace-local `CentralHalo` offsets. These are not indices into the concatenated snapshot; global callbacks must not interpret them as such.
- `src/core/module_registry.c:123–130` centralizes phase visitation; lifecycle collection, event validation/contract enumeration, logging and HDF5 `EnabledModules` (`src/io/output/metadata_hdf5.c:260–302`) use that visitor. A phase addition must cover all these paths and cleanup, not merely parsing and dispatch. `module_registry_add` (`module_registry.c:154`) currently treats a null `process` as fatal and must become family-aware. `execute_phase` (`module_registry.c:884`) returns early when `ngal <= 0`; the snapshot dispatcher must not copy that shortcut, because empty snapshots are real calls.
- `src/core/module_registry.c:770–790` is `module_emit_event`: a null context returns `-1`, and an inactive phase dispatch state returns success so direct module unit tests can call producers. Snapshot dispatch must check its active flag before that shortcut and add its own active state that rejects emission from a non-null context without disturbing that shortcut.
- `scripts/generate_module_registry.py:714–790` currently assumes every module has the FoF process symbol. Snapshot-only and dual-callback registration must be generated from declared modes, without fake FoF stubs or incompatible casts. Its freshness hash (`compute_metadata_hash`, `:662–689`) and the duplicate in `scripts/check_generated.py` (`compute_module_metadata_hash`, `:113–132`) hash only the generator and module metadata, so a new descriptor file is invisible to both until added.
- `scripts/discovery.py:295–310` auto-registers every `src/module_system/test_*/module_info.yaml` for test builds, and `tests/generated/module_sources.txt` gives the unit runner the matching sources, but the TEST_BUILD executable lists the fixture sources explicitly (`Makefile:122–129`). A new fixture directory therefore needs a `Makefile` SOURCES line or every Python tier fails to link.
- `scripts/generate_test_registry.py:44–47` globs `tests/integration/test_*.py` into the default integration tier, and `:151–156` excludes model-owned tests for horizontal simulations. Anything that must not run in `make tests` cannot live under `tests/integration/`, and explicit new test targets must invoke SHAM tests rather than widening `FULL_MODEL_TEST_SIMULATIONS`.
- `tests/framework/harness.py:614–633` emits any `phase_config` key other than `pre_timestep`/`post_timestep` under `phases:` as a user-named substep phase, so `post_snapshot` must be added there as a fixed lifecycle key before any harness-built run file can carry it.
- `src/core/read_parameter_file.c:1501–1528` already rejects binary output, resume and multi-rank horizontal runs; `:1228` reserves phase names; `:1310–1314` rejects unknown keys; `:422–424` accepts a repo-relative `simulation.config` when run from the repository root. Preserve those restrictions and add the snapshot-phase/vertical mismatch check before module initialization or input processing.
- `src/module_system/parameter_helpers.h:100–135` range macros compare only against bounds, so NaN passes them, and `parse_double_strict` (`module_registry.c:1150–1160`) accepts `nan` and `inf`. There is no combined exclusive internal-unit macro; `(0, 100000]` is `LOAD_PARAM_DOUBLE_INTERNAL` followed by `VALIDATE_RANGE_EXCLUSIVE`, with an explicit finiteness check before either range macro.
- `src/core/virial.c:45–52` computes `Mvir` in double as the catalog mass or `Len × PartMass`, while `ShamMpeak` is a float property, so the legacy `(float)max_double(prev, Mvir)` cast can yield `inf` from a finite `Mvir` above `FLT_MAX`: unreachable from a real catalog, reachable from a test-constructed halo. Slice 3 bounds the double before the cast.
- The planning oracle (`docs/dev/snapshot-global-checks/sham_rank_oracle.py`) measured that `exp(log(100000.0))` is `100000.00000000001` on the planning platform and that `(r + 0.5) / BoxSize^3` overflows for admitted volumes; Slice 3 freezes the expanded logarithmic form and a log-space upper bound in response. Both are recorded as measured values, not derivations.
- Committed fixtures already cover the needed topologies: v2 `simulations/micro-uchuu-ascii-horizontal/_tests/data/generic/` (`BoxSize` 100 Mpc/h, snapshot 0 empty, snapshot 5 with a three-progenitor halo so Type 2 orphans exist, no `Vmax` ties) and v3 `simulations/mini-millennium-horizontal/_tests/data/{worked_graph,three_snapshot_chain,adjacent}` (`BoxSize` 62.5 Mpc/h, `worked_graph` snapshot 3 empty with a gapped link). No new authoritative fixture data is needed.

These are planning anchors, not substitutes for reading the code. The binding contracts are repeated inside the slice receipts so PM's generated Developer and Reviewer prompts contain them. The Reviewer prompt carries Acceptance Criteria but not Validation Plan, so every binding obligation is stated in Acceptance Criteria.

## Implementation Profiles and Run Preparation

| Order | Recommended Developer | Effort | Reason |
|---|---|---|---|
| 1 | Claude Opus | high | Additive ABI and metadata generation |
| 2 | Claude Opus | high | Configuration, lifetime and execution seam |
| 3 | Claude Opus | high | Numerical and scientific contract |
| 4 | Claude Sonnet | high | Explicit acceptance battery and regression evidence |
| 5 | Claude Sonnet | medium | Documentation and skill reconciliation |

PM executes each atomic slice separately; no batching is recommended. Use the installed `opus` and `sonnet` aliases, record the resolved model versions, and pass the table's model and effort explicitly to `start-slice`. Reviewer recommendation: Claude Fable 5.1 (`claude-fable-5-1`), high effort, in separate fresh sessions for drift audit and code review. An unavailable model is a setup blocker, never permission to silently substitute.

Slice granularity was reviewed against the rehearsal. Slice 2 stays whole: its natural seam (configuration and lifecycle first, driver call second) would leave an intermediate commit that accepts a configured `post_snapshot` phase and silently never executes it, which the contract forbids. Slice 4 stays whole: separating the hours-long real-data gates would produce a slice with no code change, so their environmental risk is handled by the dataset and toolchain preflight below and an extended Slice 4 timeout instead. Slice 4 is also the operationally heaviest slice (a new byte-level comparator with self-mutation checks, baseline worktree builds for twelve fixture legs, six real-data gates and the acceptance record), so PM should expect more steer rounds there than for Slices 1–3; that is an accepted nonblocking risk, and the Sonnet high recommendation stands. Two of the six gates, `millennium-horizontal` and `mini-uchuu-horizontal`, were run on 2026-10-02 when they were retargeted at the whole simulations and took 15 and 27 minutes (checks record); the other four have not been measured, and Slice 4's timeout still has to be sized for them.

Slices 1–4 require recorded human approval because they change shared interfaces, execution/global state, model behavior, or build/test entry points. They also require independent review. Approval may be provided for the named slices together before execution, then recorded for each using PM's `approve` command; this document does not grant that approval. Slice 5 requires independent review but no new implementation approval. This is a planned checkpoint, not a PM parser defect.

Before PM initialization, complete these pre-run actions in order. Read-only checks and reversible preflights are already authorized; commits and approval-flagged implementation slices require explicit user authorization:

1. Commit the reviewed plan and the planning records. PM's `init` calls `require_clean_worktree` and refuses a tree that is dirty outside `.pm/`, and it freezes the run to `plan_digest`, the SHA-256 of the plan file's actual bytes, whether or not those bytes are committed. The file must therefore be committed before `init` only because the tree must be clean, and any later edit to it changes the digest and stops the run. The user authorized the planning-artifact commit on 2026-10-01. Confirm that it has landed and the worktree is clean before initialization; that authorization does not cover implementation commits.
2. Re-run `python3 docs/dev/snapshot-global-checks/run_checks.py` on the committed plan. Its drift check allows only the planning surface (this plan, the pathway, the two records and the checker directory) to differ from the baseline and fails on any other committed, staged, unstaged or untracked path; its phrase, witness, anchor and mutant fixtures live in that directory and are maintained with the plan. It is a pre-implementation checker and is expected to fail once any slice lands.
3. Check the recorded baseline preflights in the checks record against the current environment. The 12-leg `{halos-only, sage16} × {mini-millennium vertical, micro-uchuu horizontal v2, mini-millennium horizontal v3} × {fixed, dynamic}` matrix passed. The `sham`/`micro-uchuu-ascii-horizontal` production-validation and TEST_BUILD link/run preflight also passed. Repeat an affected preflight only if source, toolchain, fixtures or selectors have drifted. Use the shipped `models/sage16/input/sage16_mini-millennium.yaml` pipeline and parameters, including metal enrichment; the older full-physics test YAML omits it. Do not pre-run the six real-data identity gates to estimate durations; they run inside Slice 4. The exception on record is the `millennium-horizontal` and `mini-uchuu-horizontal` pair, which was run on 2026-10-02 against the whole simulations (15 and 27 minutes of gate time; the checks record has the stage timings), so their durations are known and need no repeat.
4. Confirm the selected PM Developer harness can execute the required Make/compiler/venv commands in its actual isolated environment; a local-agent preflight does not prove a different CLI permission profile. Resolve setup restrictions before starting a slice, without bypassing sandbox or approval controls. Confirm the `/Volumes/Internal` datasets each Slice 4 gate reads, `mimic_venv`, HDF5, `mpicc` and `mpirun` are present. Confirm the volume holding the repository's `output/` directory, where the gates build their scratch worktrees and runs, has the free space the gates pin (150 GiB for `millennium-horizontal`, 300 GiB for `mini-uchuu-horizontal`, whose gate lowered free space by about 198 GB at its largest five-minute sample). Give PM's `observe --wait` for Slice 4 an extended timeout sized for hours-long gates rather than the default; a gate that exceeds it is a stop for the user, not a skip.

Work on the current branch unless the user explicitly authorizes a feature branch. PM refuses implicit `main`, so use an explicit `--branch main` only if the user chooses to execute there. No implementation session may edit this frozen plan, change dependency manifests/licenses, regenerate baselines, or push.

Format-check note for every slice: `make check-format` runs `black` and `isort` over the whole tree, which follows the `obsidian-inbox` symlink out of the repository. A failure attributable only to files reached through that symlink is recorded as pre-existing and is not fixed; no session edits files outside the repository. Run the full unmodified `./scripts/beautify.sh` on a complete candidate source snapshot without ignored external directory links, compare the result with the candidate, and bring back only authorized formatting changes. Files in the diff must also pass the pinned Black/isort or clang-format checks. This avoids modifying unrelated files through local symlinks.

## Slice 1: Add the typed callback and metadata contract

### Intended Change

- Recommended Developer: Claude Opus; effort: high. No prerequisites beyond the planning baseline.
- Add `PROCESSING_MODE_SNAPSHOT` / `process_snapshot`, a dedicated `SnapshotContext`, a typed optional `Module.process_snapshot` callback, and mode-aware generation/registration. Extend the existing `test_fixture` to dual mode and add a snapshot-only `test_snapshot_fixture`. This slice exposes no new run-file phase and makes no driver call.

### Acceptance Criteria

- [ ] Inputs: existing module metadata remains valid; a directory module may declare `process_snapshot` alone or alongside existing FoF modes. Unknown, duplicate and empty mode lists fail both generator and validator. Standalone fallback modules retain exactly the original three modes.
- [ ] Outputs: `int (*process_snapshot)(const struct SnapshotContext *ctx, const struct Halo *halos, int64_t count)` is added without changing `int (*process)(struct ModuleContext *, struct Halo *, int)`, existing enum values, event payloads, or property schemas.
- [ ] `SnapshotContext` contains `int snapshot_number`, `double redshift`, `double time` in the existing lookback-time units, and `const struct MimicConfig *params`; it exposes no fictitious FoF central, event, substep or common galaxy timestep.
- [ ] The API documents borrowed current-generation storage, call-limited pointer lifetime, count zero with a nullable population pointer, positive counts with non-null storage and galaxy pointers, int64 indexing, and writes only through `halos[i].galaxy`. No callback may retain pointers, reorder/resize the population, change halo fields or pointers, or interpret `CentralHalo` as a snapshot offset.
- [ ] `const` on the borrowed view is shallow and the project warning set does not include `-Wcast-qual`. The API documentation states that an explicit cast away from `const`, retention of the borrowed pointer in static or heap storage, and indexing by `CentralHalo` compile silently and are forbidden by contract. Every code review of a snapshot callback in this plan checks for those three patterns explicitly; no compiler flag is added to enforce them.
- [ ] Generation emits only the required declarations: `extern int <name>_process_snapshot(...)` only for modules declaring the mode and `extern int <name>_process(...)` only for modules declaring an FoF mode; snapshot-only modules have `.process = NULL`; FoF-only modules have `.process_snapshot = NULL`; dual-mode modules have both typed callbacks. Init and cleanup remain mandatory, once per configured module.
- [ ] Registration rejects missing callbacks for every advertised family, but accepts a null unused callback. Existing modules compile without added stubs or edits.
- [ ] The Python generator and validator share explicit mode descriptors in `scripts/module_modes.py`; the C side centralizes string parsing, naming and dispatch-family lookup in `module_registry.c`. Unknown enum/string values fail closed. Neither side treats every non-FoF mode as snapshot mode; adding a later family requires an explicit descriptor and implementation.
- [ ] Generator freshness includes the new descriptor source: `compute_metadata_hash` in the generator and `compute_module_metadata_hash` in `scripts/check_generated.py` both hash the path and bytes of `scripts/module_modes.py`, and the `Makefile` module-generation stamp depends on it, so changing the descriptor invalidates generated registration under normal `make generate` and `make check-generated` detects stale output. Generated files are never hand-edited.
- [ ] Metadata declaring snapshot-only event emission/consumption fails the existing required-FoF-mode checks; dual-mode modules retain their valid FoF event declarations. Snapshot callbacks do not acquire an event contract.
- [ ] Test-build fixtures cover every family with real modules: `src/module_system/test_fixture/` gains `process_snapshot` in its supported modes and a `test_fixture_process_snapshot` callback (dual mode); the new `src/module_system/test_snapshot_fixture/` declares only `process_snapshot` (snapshot-only); the untouched event fixtures remain FoF-only. Both fixtures compile into the TEST_BUILD executable through the `Makefile` SOURCES list and into the unit runner through the generated `tests/generated/module_sources.txt`; production registration and the production executable exclude them. The existing `test_fixture` tests continue to pass with the added mode.
- [ ] Unit/schema tests prove snapshot-only, FoF-only, dual-mode, missing-callback and invalid-mode cases, using framework markers and real callback invocations, not source-text assertions alone. Both HDF5-enabled and `USE-HDF5=no` default builds compile.
- [ ] Required evidence: focused tests, clean default build, `check-generated`, all default test tiers and existing baseline comparisons pass; no baseline refresh or weakened tests.
- [ ] `style-guide` write mode and a recorded differential audit against `docs/STYLE-GUIDE.md` cover naming, comments, documentation, logging, metadata, tests and generated-code handling; findings are fixed in scope or explicitly adjudicated. Format, docs and differential lint results are recorded, with any `check-format` failure outside the repository tree named as pre-existing.

### Authorized Surface

- Files allowed to change:
  - `src/core/module_interface.h`
  - `src/core/module_registry.h`
  - `src/core/module_registry.c`
  - `scripts/module_modes.py` (new shared mode descriptors)
  - `scripts/generate_module_registry.py`
  - `scripts/validate_modules.py`
  - `scripts/check_generated.py`
  - `Makefile` (the module-generation stamp dependencies and the TEST_BUILD framework fixture SOURCES list only)
  - `src/module_system/test_fixture/module_info.yaml`
  - `src/module_system/test_fixture/test_fixture.c`
  - `src/module_system/test_fixture/README.md`
  - `src/module_system/test_fixture/_tests/test_unit_test_fixture.c`
  - `src/module_system/test_fixture/_tests/test_integration_test_fixture.py`
  - `src/module_system/test_snapshot_fixture/` (new neutral snapshot-only test module)
  - `tests/unit/test_snapshot_module_contract.c`
  - `tests/integration/test_snapshot_module_schema.py`
  - `docs/DEVELOPER-GUIDE.md` (callback/metadata contract)
  - `.agents/skills/mimic-modules/SKILL.md`
- Functions/classes/components allowed to change: mode conversion/lookup, registration checks, generator callback declarations/initializers, metadata validation/freshness, the dual-mode extension of `test_fixture` and the new snapshot-only fixture. No unrelated dispatcher refactor. `src/module_system/test_fixture/test_properties.yaml` and `scripts/discovery.py` are read, not changed.
- Tests allowed or expected to change: new contract/schema tests, the fixtures' own tests. Generated artifacts may be regenerated by commands; none are tracked at the planning baseline.

### Explicit Non-Goals

- No run YAML key, driver dispatch, model physics, property additions, new C translation unit in core, new dependency, schema version bump, new compiler flag or generated-file hand edits. No change to existing FoF behavior. `TestDummyProperty` stays `output: false`.

### Risk Flags

- Risky surfaces touched: shared types, callback API, generated registration and build dependency tracking.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: callback registration matrix; generator/validator agreement; descriptor freshness (edit `scripts/module_modes.py` in a scratch copy and show `check-generated` fails); compile a positive galaxy write and negative halo/pointer writes through the const view under the project warning set; TEST_BUILD fixture exclusion from the production registry.
- Commands to run: use `mimic_venv/bin/python` for Python and identical selectors throughout each build block. Run `make MODEL=sage16 SIMULATION=mini-millennium generate validate-modules lint-parameters check-generated`; run the new Python test directly; run `MODEL=sage16 SIMULATION=mini-millennium tests/unit/run_tests.sh test_snapshot_module_contract test_unit_test_fixture`; clean/build once with `USE-HDF5=no`, then clean/build the default HDF5 configuration with `TEST_BUILD=yes` to prove the fixtures link, then clean/build the default configuration and run its unit, integration and scientific tiers sequentially. Delegate long tiers to a test subagent capturing exit codes and logs.
- Lint (differential, via the `lint` skill): required against the slice's `before_head`, including new files.
- Manual checks: inspect generated declarations and fixture exclusion; reread the diff using style-guide, checking the three forbidden const-view patterns; run `make check-docs`, `./scripts/beautify.sh` and `make check-format`; sweep the authorized module skill for stale "exactly three functions" and "three processing modes" claims. Inspect all skips. Do not commit generated ignored files.

### Rollback Path

- Revert this slice in a new commit before dependent slices land; if dependents already landed, revert them in reverse order first. Never amend history or delete new files; archive abandoned files per repository policy.

## Slice 2: Execute an ordered post-snapshot phase

### Intended Change

- Recommended Developer: Claude Opus; effort: high. Requires Slice 1.
- Wire `modules.post_snapshot` through parsing, startup validation, lifecycle enumeration, dispatch, cleanup, horizontal execution and provenance as one operational feature, with the test harness able to write the phase.

### Acceptance Criteria

- [ ] Inputs: `modules.post_snapshot` uses the existing phase sequence shape, for example `- test_snapshot_fixture: process_snapshot`; absent, YAML-null and empty lists all mean no global modules. Duplicate module entries in this phase fail startup. `post_snapshot` is reserved against a user-named substep phase.
- [ ] Only `process_snapshot` entries are legal in `post_snapshot`, and that mode is illegal in pre-timestep, post-timestep and named substep phases. Malformed entries, unknown modules, unsupported modes and missing callback families fail before module init or dataset processing with the phase/module identified.
- [ ] A non-empty post-snapshot phase requires the horizontal driver; the vertical driver accepts an omitted/empty phase and rejects a non-empty one. Existing horizontal HDF5-only, no-resume and single-rank restrictions remain enforced, including actual two-rank rejection evidence from the explicit isolated MPI validation command specified below when MPI is available.
- [ ] Outputs: the horizontal driver invokes each configured callback in YAML order exactly once per input snapshot after the FoF coverage check and before publishing the current generation, releasing generations or writing output. It does so for snapshot zero, empty snapshots, non-output snapshots and the final snapshot, independent of fixed/dynamic substeps. The new dispatcher has no `count <= 0` early return.
- [ ] The callback receives only `cur->processed.halos` and its int64 count: all surviving Types 0/1/2 across all FoFs, no Type 3, no raw slab-only objects and no older retained generations. Zero count is valid even when the driver retains an allocated non-null buffer.
- [ ] Context is reconstructed for every snapshot from its index, `MimicConfig.ZZ`, `Age` and config pointer. Writes to galaxy properties are visible to the next callback in the same phase and to later adjacent or gapped inheritance, and the call precedes output so they are visible to the current output; the driver introduces no snapshot copy or rank-dependent order. This slice proves the inter-callback and inheritance visibility directly with `TestDummyProperty` (which stays `output: false`); output visibility is proven in Slice 3 through `StellarMass`.
- [ ] Configured snapshot-only and dual-mode modules join lifecycle collection and `module_configured_anywhere`, initialize once, clean up once, and release phase-owned configuration on normal shutdown. `for_each_phase` visits non-empty `post_snapshot` after the existing phases; old empty configurations yield the same observable pipeline/provenance as before.
- [ ] `execute_post_snapshot` uses only the dedicated typed callback. Nonzero callback return fails the run with module, snapshot and return code; existing horizontal failure cleanup removes incomplete run products and frees owned state. There is no silent callback skip or success output after failure.
- [ ] Snapshot dispatch cannot emit or consume FoF events, including an attempted `module_emit_event` during a snapshot callback; reject it rather than using the no-active-phase direct-unit-test success shortcut. Concretely: the registry keeps a snapshot-dispatch-active state; `test_snapshot_fixture` calls `module_emit_event` with a non-null local `struct ModuleContext` from inside its callback, requires the `-1` rejection and fails the callback if the call is accepted; the existing shortcut for an inactive phase state is unchanged. FoF event behavior is unchanged outside snapshot dispatch.
- [ ] HDF5 `RunProperties/EnabledModules` records each configured global entry with phase `post_snapshot` and mode `process_snapshot` in execution order; existing copied run YAML and parameter provenance remain complete. No HDF5 layout/version change is needed, and inactive runs retain their old EnabledModules rows and order.
- [ ] `tests/framework/harness.py` routes a `post_snapshot` key in `phase_config` as a fixed lifecycle key emitted after `post_timestep`, never as a user-named substep phase; existing harness callers are unaffected.
- [ ] `test_snapshot_phase.py` runs parser and vertical-rejection checks on vertical packages and reports an explicit configuration SKIP only for horizontal-execution cases. Slice 2 adds no Make target: the required horizontal evidence is two explicit-selector invocations of `tests/integration/test_snapshot_phase.py`, one under `MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=yes` and one under `MODEL=halos-only SIMULATION=mini-millennium-horizontal TEST_BUILD=yes`, each of which must run every required horizontal case with no `MIMIC_RESULT: SKIP`; the wrapping `tests-snapshot-global` target arrives in Slice 4. The two-rank rejection evidence comes from a separate explicit validation command, never from a test inside the auto-discovered tiers: in an isolated complete candidate scratch copy or detached worktree of the final Slice 2 tree, build with `USE-MPI=yes`, launch `mpirun -n 2` on a horizontal run file, and record the expected startup diagnostic and nonzero exit as slice evidence; the shared `mimic` executable of the main checkout is never rebuilt with MPI inside a test tier. `test_snapshot_phase.py` itself never invokes `make` and contains no two-rank case; the separate explicit MPI command is the sole two-rank check. The recorded result of that command is required Slice 2 evidence, and MPI absence leaves the requirement unmet rather than accepted.
- [ ] Slice 2 updates VISION Principle 4 to distinguish determinism given a FoF input from determinism given a complete snapshot population. Snapshot-module scratch is outside `input.retention_memory_ceiling_mb` accounting; documentation states this without claiming a bound on total process memory. Slice 5 reconciles the final documentation.
- [ ] Required evidence: neutral integration tests on the committed v2 `micro-uchuu-ascii-horizontal` and v3 `mini-millennium-horizontal` fixtures demonstrate empty/non-output/final snapshots, two ordered callbacks (`test_snapshot_fixture` and the dual-mode `test_fixture`, order proven from their log markers), different substep counts, Type 2 inclusion, Type 3 exclusion, callback failure cleanup, and adjacent/gapped inheritance of a `TestDummyProperty` value within its declared `[0, 1]` range. The gapped test distinguishes current-generation data from an older retained one. All default tiers and `tests-horizontal-v3` pass without baseline changes. No new committed fixture data is added.
- [ ] Required evidence includes a recorded style-guide audit against `docs/STYLE-GUIDE.md`, differential lint, format/docs checks and relevant skill updates, including the accepted-keys list in `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`. No unreviewed style finding or unexplained skip is called a pass.

### Authorized Surface

- Files allowed to change:
  - `src/include/types.h`
  - `src/core/read_parameter_file.c`
  - `src/core/module_interface.h`
  - `src/core/module_registry.h`
  - `src/core/module_registry.c`
  - `src/core/horizontal_driver.c`
  - `src/io/output/metadata_hdf5.c`
  - `src/module_system/test_fixture/test_fixture.c`
  - `src/module_system/test_fixture/README.md`
  - `src/module_system/test_snapshot_fixture/`
  - `tests/framework/harness.py` (the `post_snapshot` lifecycle key only)
  - `tests/framework/test_phase_config.h`
  - `tests/unit/test_snapshot_module_contract.c`
  - `tests/integration/test_snapshot_phase.py`
  - `tests/integration/test_phase_execution.py`
  - `tests/integration/test_processing_modes.py`
  - `docs/VISION.md`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `.agents/skills/mimic-modules/SKILL.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
- Functions/classes/components allowed to change: config phase storage/parsing/validation, mode lookup, phase visitors and dependency search, typed dispatch and event guard, cleanup, the post-sweep insertion in `run_horizontal_driver`, pipeline provenance, the harness phase writer and relevant docs. Committed fixtures are read, never modified.
- Tests allowed or expected to change: neutral core tests; use the TEST_BUILD-only `TestDummyProperty` and fixture log markers for visibility assertions. No production model is an infrastructure fixture.

### Explicit Non-Goals

- No change to ordinary `ModuleContext`/FoF process signature, FoF dispatch ordering, core inheritance algorithms, retention horizons/ceiling semantics, raw halo schema, output layout, scientific baselines, production run files, simulation package data, property metadata or `TestDummyProperty` output visibility. Snapshot topology is immutable; no pre-snapshot hook or iteration loop.

### Risk Flags

- Risky surfaces touched: YAML/API contracts, execution order, global dispatch state, memory lifetime, output provenance.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the acceptance matrix above, with marker-level assertions that the positive cases actually run; malformed config cases assert nonzero status and named diagnostics. Unit-test lifecycle/dispatch with synthetic populations, including a zero-count call with a non-null buffer, and test real driver calls on the committed v2 adjacent fixture and the v3 `worked_graph` gapped fixture.
- Commands to run: default `make MODEL=sage16 SIMULATION=mini-millennium generate validate-modules lint-parameters check-generated`, clean build and all three tiers; run `make MODEL=sage16 SIMULATION=mini-millennium tests-horizontal-v3`; build `MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=yes`, run `tests/integration/test_snapshot_phase.py` with those selectors and require zero `MIMIC_RESULT: SKIP`; repeat for `mini-millennium-horizontal`. For the two-rank rejection evidence, create an isolated complete candidate scratch copy or detached worktree of the final Slice 2 tree outside the main checkout, build it with `MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=yes USE-MPI=yes`, launch `mpirun -n 2 ./mimic <horizontal run file>` from that copy, and capture the startup diagnostic and exit status into the slice evidence; never run `make USE-MPI=yes` in the main checkout or inside a test tier, and retain or archive the copy under the project archive policy. MPI absence is an explicit unvalidated requirement, not acceptance. Restore/regenerate the default selectors and rebuild the main checkout before finishing. Long suites run sequentially under a test subagent.
- Lint (differential, via the `lint` skill): required.
- Manual checks: inspect actual EnabledModules datasets, copied run YAML, error cleanup and test markers; style-guide audit; `./scripts/beautify.sh`, `make check-format`, `make check-docs`; update only relevant skill statements. The VISION amendment describes the implemented additive scope, keeping future rate/batch modes prospective.

### Rollback Path

- Revert this slice and its dependent slices in reverse order. Slice 1 remains a buildable unused API. Archive abandoned test files; do not rewrite existing fixture baselines.

## Slice 3: Implement deterministic global rank SHAM

### Intended Change

- Recommended Developer: Claude Opus; effort: high. Requires Slices 1–2.
- Add the opt-in `sham_global_rank` directory module, its local tests and a fixture-oriented horizontal run example. Freeze the analytic target, selection and example parameters below; do not choose scientific policy during implementation.

### Acceptance Criteria

- [ ] Inputs: the borrowed snapshot obeys the framework contract (Types 0/1/2 with non-null galaxy pointers, unique positive `UniqueGalaxyID`s). The module verifies these conditions before assignment and fails on violations; it does not silently drop corrupt entries or invent fallback IDs.
- [ ] `init()` requires finite parameters `ShamGlobalMassScale` in `(0, 100000]` internal `1e10 Msun/h`, `ShamGlobalNumberDensity` in `[1e-12, 1e3]` `(Mpc/h)^-3`, and `ShamGlobalSlope` in `[0.1, 10]`; `BoxSize` must be finite and positive and its cubed volume finite and positive, evaluated once at init for this domain check only. Each value is checked with `isfinite` explicitly before its range macro, because the range macros pass NaN and the strict parser accepts `nan`/`inf`. The mass parameter uses `LOAD_PARAM_DOUBLE_INTERNAL` then `VALIDATE_RANGE_EXCLUSIVE`, so its `(0, 100000]` check applies to the internal-unit value after conversion; density and slope use the plain double loader with `VALIDATE_RANGE_INCLUSIVE` and explicitly documented fixed units. No new helper macro is introduced. Missing/malformed/non-finite/out-of-range values fail: for the mass scale the strings `nan`, `inf`, `infinity`, `1e400`, `1,0` and `abc` fail init, while `1e5` and `100000` are accepted and give identical bits. No implementation may rely on `strtod` reporting `ERANGE` for subnormal results; a subnormal value that parses is in-domain and its logarithm is finite.
- [ ] The module is configured exactly once in post_snapshot as `process_snapshot`. Startup rejects simultaneous configuration of `sham_assign_stellar_mass` anywhere: the two independent assignment prescriptions must not overwrite each other silently.
- [ ] At each callback, Type 0/1 entries update `ShamVpeak = max(previous ShamVpeak, Vmax)` and `ShamMpeak = max(previous ShamMpeak, Mvir)` using existing field storage types. Type 2 keeps inherited peaks. All consumed peak/current proxy values must be finite and nonnegative; malformed values fail even if that object would otherwise be ineligible. `Mvir` is a double and `ShamMpeak` a float: the updated double `max(previous ShamMpeak, Mvir)` must be `<= FLT_MAX` before it is cast to float storage, and after the update both stored peaks must be finite floats; otherwise fail the snapshot with the `UniqueGalaxyID` and the value. `Mvir = FLT_MAX` is accepted and stored exactly; `Mvir = 1e39` fails. The module does not age/remove orphans or change any halo field.
- [ ] Eligible objects are every Type 0/1/2 with updated `ShamVpeak > 0`; zero-peak objects are ineligible. There is no implicit central-only cut, mass cut, scatter, orphan lifetime cut or baryon cap.
- [ ] Sort scratch records by descending `ShamVpeak`, then ascending positive `UniqueGalaxyID` for exact ties. Never sort the borrowed population. ID uniqueness is checked by a second pass over the ID-sorted scratch. Use int64/size_t-safe counts and checked scratch byte sizing, no int narrowing, tracked module allocations, and release scratch before callback return on success or returned failure. Scratch scales with the current population, never the number of snapshots.
- [ ] Outputs: for zero-based rank `r`, `ln M_r = ln M0 - [ln(r + 0.5) - 3 ln BoxSize - ln n0] / alpha`, using the three module parameters in their stated units. Evaluate with double libm operations: form the bracket, divide by `alpha`, then subtract from `ln M0`. This inverts `n(>M) = n0 * (M/M0)^(-alpha)` at `n = (r + 0.5) / BoxSize^3`. Never materialise `n_r`, `BoxSize^3` or `n_r / n0` as intermediate doubles in rank evaluation; the cube is evaluated only for the init-time domain check. This avoids overflow/underflow of intermediates when the final mass is representable. No extrapolation table or normalization to eligible count.
- [ ] The acceptance order is frozen: first fail the snapshot if `ln M_r > log(100000.0)`, comparing computed doubles before exponentiation; then compute `M_r = exp(ln M_r)` and require its stored float to be finite, nonzero and `<= 100000`. No tolerance is added and no mass is clipped. A mass that rounds to a nonzero positive float32 subnormal is accepted and stored as that subnormal. Failure diagnostics identify rank, `UniqueGalaxyID`, `ln M_r` and the four parameters; no output for the failed snapshot is accepted.
- [ ] The upper-bound decision follows the specified double computation, including its platform roundoff; it does not promise exact-real classification arbitrarily close to the boundary. In particular, `exp(log(100000.0))` evaluates to `100000.00000000001` on the planning platform, so checking that exponentiated double would reject the exact endpoint. The endpoint (`M0=100000`, `n0=0.5`, `BoxSize=1`, `alpha=1`, rank 0) must store exactly `100000.0f`; `n0=0.5000000000005` with the other endpoint parameters must fail. The measured local roundoff examples in the checks record are not a universal error bound.
- [ ] For every entry reset `StellarMass`, `ShamStellarMassNoScatter`, `ShamScatterDex`, `BulgeMass`, `MetalsStellarMass`, `MetalsBulgeMass` and `StarFormationRate` to zero; eligible entries then receive the float-rounded `M_r` in both stellar-mass fields. Leave `ShamOrphanAge` unchanged; peak tracking and the listed assignments are the only galaxy writes.
- [ ] Empty populations succeed without allocation or rank evaluation. For nonempty all-ineligible populations, validate every ID and consumed value, update peaks and reset the specified fields, then return without rank evaluation. ID-validation scratch is permitted: uniqueness applies to all entries, including ineligible ones, and is checked using ID-sorted scratch before assignment. Ineligible entries remain zero.
- [ ] Repeating a callback with identical input peaks/identities/parameters gives identical assigned bits; permuting FoFs or population order preserves the per-ID result. Adding a higher-proxy galaxy in a different FoF changes lower ranks, demonstrating a global calculation.
- [ ] Tests include an independent hand-solvable oracle: `alpha=1`, `n0*V=1`, `M0=8`, proxies `(ID 80,200), (ID 7,100), (ID 42,100)` yield, before float rounding, exactly ID 80 receives 16 (rank 0), ID 7 receives 16/3 (rank 1, winning the tie against ID 42 by lower ID) and ID 42 receives 16/5 (rank 2). Same-build permutations/repeats compare exact float bytes; independent formula checks allow at most two float ULPs for double/libm rounding and explain that bound. Tests catch wrong tie ordering and use of `r+1` instead of `r+0.5`.
- [ ] Numerical edge tests compare success/failure exactly and successful masses against an independent 60-digit Decimal reference within at most two float ULPs. Tiny-volume case: `BoxSize=1e-105, M0=1e5, n0=1e-12, alpha=10`, rank 0, succeeds (approximately `2.13846919998e-28`). Huge-volume case: `BoxSize=5.6e102, M0=1, n0=1e3, alpha=0.1` fails; changing `M0=1e-30, alpha=10` succeeds (approximately `14.2744507875`). These cases must detect a materialized-density implementation.
- [ ] Subnormal case: `M0=1e-40, n0=1e-12, alpha=10, BoxSize=1`, rank 0, has reference float bits `0x000012DA`; apply the same two-ULP independent-oracle bound. With `M0=1e-46, n0=1e3` and the other parameters unchanged, the float rounds to zero and the callback fails. Same-build repeats/permutations remain bitwise exact. Test the endpoint, malformed strings and peak-storage failures stated above. Differences beyond the stated bounds are failures, never newly negotiated tolerances.
- [ ] Permutation invariance and the tie key are proven at unit level with synthetic populations passed directly to the module's rank helper. End-to-end determinism is proven by repeated runs on the committed `micro-uchuu-ascii-horizontal` fixture compared bitwise. Where a run-level case needs a tie or a moved higher proxy, the test writes a temporary derivative into its own temp dir by copying the committed fixture and editing only `Vmax` values through h5py, leaving `/schema`, links, ordering and `fixture_manifest.json` untouched; the derivative's schema is the committed fixture's own, the generator is the test, and nothing is committed under `tests/data/`.
- [ ] The example run file freezes `ShamGlobalMassScale = 8` internal units, `ShamGlobalNumberDensity = 1e-6` `(Mpc/h)^-3` and `ShamGlobalSlope = 1` against the committed `micro-uchuu-ascii-horizontal` fixture (`BoxSize` 100 Mpc/h), which gives a rank-0 mass of exactly 16 internal units and every lower rank `8 / (r + 0.5)`, all inside `(0, 100000]`. These values are a framework demonstration chosen to keep the example inside the specified range; they carry no observational claim. The run file names `simulation.config` as the fixture's `_tests/input/test_simulation.yaml` by repository-relative path and is run from the repository root.
- [ ] New unit and integration/scientific tests run explicitly under `MODEL=sham` with horizontal fixtures even though normal model test discovery excludes horizontal simulations. Do not change `FULL_MODEL_TEST_SIMULATIONS`. The module's Python tests are also registered in the default tiers under `MODEL=sham SIMULATION=mini-millennium`; there they skip with a stated reason because the selected package is vertical, while under the explicit horizontal target every required case runs and any `MIMIC_RESULT: SKIP` fails the target. The module's C unit tests run without skips under either `sham` pair. Test invalid parameters/IDs/proxies, overflow/range failure, peak persistence, Type 2 inclusion, zero candidates and no cross-snapshot allocation growth. Output visibility of snapshot-callback writes is proven here by reading `StellarMass` per `UniqueGalaxyID` from the written HDF5 output.
- [ ] Existing `sham_assign_stellar_mass`, its parameters/run files/tests and all `sage16` physics remain unchanged. The example and README identify the target as uncalibrated and the fixture as incomplete, state units/selection/rank conventions, and make no observational-parity claim.
- [ ] Validation uses the existing production/test-build separation with identical MODEL/SIMULATION selectors: unset inherited `MIMIC_TEST_BUILD`; first run `TEST_BUILD=no generate validate-modules lint-parameters check-generated`; then run `TEST_BUILD=yes generate validate-build mimic` and the explicit module tests. Production-only validators must not be applied to test-property output. Restore production generation before a production freshness check; this is the current supported workflow, not a waived validation failure.
- [ ] Required evidence: module validation, parameter lint, generation/freshness, module tests, default framework tiers and pre-existing SHAM tests pass. Record a style-guide audit against `docs/STYLE-GUIDE.md`, differential lint, format/docs checks and skip accounting; no property schema or baseline change.

### Authorized Surface

- Files allowed to change:
  - `models/sham/modules/sham_global_rank/`
  - `models/sham/parameter_units.yaml`
  - `models/sham/input/sham_global_micro-uchuu-ascii-horizontal.yaml`
  - `models/sham/README.md`
  - `Makefile` (only the explicit global-SHAM test target and its help/phony declarations)
- Functions/classes/components allowed to change: new module init/process_snapshot/cleanup and private mathematical/sorting helpers; module-local oracle tests and their temporary-derivative generator; mass parameter units; one new example and test runner target. The target directly invokes declared model tests rather than relying on filtered registries.
- Tests allowed or expected to change: new module-local C and Python tests. Existing core/simulation/model metadata and committed fixtures may be read, never patched to make the new test pass.

### Explicit Non-Goals

- No changes to legacy SHAM, property definitions, simulation metadata, committed fixtures, global module infrastructure, observations/dependencies, plotting, or arbitrary user tables. No claims of realistic stellar masses, calibrated abundance, conserved stellar mass along branches, or feasible Shin-Uchuu/full-Uchuu memory.

### Risk Flags

- Risky surfaces touched: a new model prescription, parameter units, sorting memory and an explicit build/test entry point.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: module-owned C unit oracle/invalid-input/permutation cases, the numerical edge battery listed in Acceptance Criteria (the planning oracle is not imported by any test), and Python end-to-end rank, inheritance, repeat and derivative-tie tests on the micro-Uchuu committed fixture. A focused target `make tests-snapshot-global-sham` builds `MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal TEST_BUILD=yes`, directly invokes the declared tests (full C test path where registry filtering hides it), rejects unexpected SKIPs/nonzero statuses, and restores the caller's generated selectors on every exit, following the `tests-horizontal-v3` recipe. It must not recursively run the real-data parity gate.
- Commands to run: module target above; `make MODEL=sham SIMULATION=mini-millennium generate validate-modules lint-parameters check-generated` and existing SHAM unit tests; run the example run file from the repository root and inspect its output; restore `MODEL=sage16 SIMULATION=mini-millennium`, clean/build, run all default tiers sequentially. Capture long suites with a test subagent. No new third-party packages.
- Lint (differential, via the `lint` skill): required.
- Manual checks: inspect actual output per ID and recorded parameters/EnabledModules; account for every candidate and excluded galaxy; verify no baseline changes and no real-data assumption in the fixture example; check the three forbidden const-view patterns; style audit, formatter, `make check-format` and `make check-docs`.

### Rollback Path

- Revert this module slice after reverting later dependent slices. The infrastructure remains usable with no global modules. Archive newly abandoned module/example files; never change legacy outputs to match this prescription.

## Slice 4: Close the regression and execution gates

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. Requires Slices 1–3.
- Add one explicit complete fixture acceptance target, one explicit disabled-mode identity target backed by a manual test outside the auto-discovered tiers, and record measured default/disabled-mode preservation and real cross-format gates. Fix only acceptance-harness defects within this surface; runtime defects return to the user for a corrected plan/run, not an unauthorized repair here.

### Acceptance Criteria

- [ ] `make tests-snapshot-global` runs the neutral phase tests under v2 micro-Uchuu and v3 mini-Millennium fixtures, the typed callback/schema tests and `tests-snapshot-global-sham`, in sequence. It fails on subprocess/build errors, missing expected cases or unexpected `MIMIC_RESULT: SKIP`, captures logs, and restores the caller's generated selectors on every exit. It does not silently broaden model discovery, launch real-data gates or run the reference-commit comparison.
- [ ] The fixture battery demonstrates the complete cross-slice contract: once per snapshot regardless of output/substeps, current generation only, writes inherited over an empty gap, event rejection, callback error cleanup, provenance, global rank/tie/zero cases and per-ID permutation invariance as Slice 3 defines them. Tests exercise the executable, not only source structure or mock callbacks.
- [ ] The disabled-mode identity comparison lives in `tests/manual/test_snapshot_disabled_identity.py`, a new directory that `scripts/generate_test_registry.py` does not glob, so it never enters `make tests`. It is invoked only by `make tests-snapshot-global-identity`. The baseline leg runs with the `modules.post_snapshot` key absent, because the pre-feature reference commit rejects that key as unknown. The reference commit is the last commit before any feature code, which is the `before_head` PM records for Slice 1. The test takes it as `REFERENCE_COMMIT=<hash>` or derives it as the parent of the first commit, walking `git log --first-parent --reverse` from the commit that last changed this plan file, that changes any path outside the planning surface (this plan, the development pathway, the two planning records and the checker directory under `docs/dev/`); it never embeds a commit hash. Either way the commit must be an ancestor of HEAD, must contain no feature symbol (`process_snapshot`, `PROCESSING_MODE_SNAPSHOT`, `SnapshotContext` or `post_snapshot` under `src/`, `scripts/` and `models/`), and must contain every simulation package and run-file selector the legs use under the same names as the feature tree. An explicit value must equal the derived one or differ from it only in planning-surface paths, and the evidence records the resolved hash. Because the reference carries every non-planning change made before the run, including the horizontal package names, the reference and feature builds select the same simulation names. Two feature legs run on the final feature tree, one with the key absent and one with an explicit empty list (`post_snapshot: []`), and both compare to that same key-absent baseline output on committed default, v2 and gapped-v3 fixtures. Cover `halos-only` and `sage16`, fixed and dynamic, comparing each scheme to its own baseline. Derive the SAGE pipeline and parameters from the shipped `models/sage16/input/sage16_mini-millennium.yaml`, including metal enrichment; do not use the older full-physics test YAML. Require identical ID sets and per-ID raw property bytes at every output snapshot, identical dataset presence and, where present, byte-equal `RunProperties/Parameters`, `FieldMetadata`, `EnabledModules`, `EventContracts` and `Redshifts` datasets, an identical `metadata/output_schema.json`, and identical values for every other `RunProperties` attribute. The only permitted differences are the existing provenance records that necessarily differ between two builds and two runs: the `RunProperties/Version` attributes (`version`, `git_commit`, `git_branch`, `git_date`, `build_date`), `RunProperties/RunEndTime`, `metadata/version_info.json`, path-valued `RunProperties` string attributes and copied `metadata/` files whose content differs only by the reference-worktree or scratch-output path prefix, and, in the explicit-empty-list feature leg only, the copied run YAML's added `post_snapshot: []` line. Each permitted difference is named explicitly in the evidence; no numeric tolerance or new comparator exception is added. The test builds the reference in a detached `git worktree` as `tests/framework/parity_gate.py` does. Preserve failed evidence; archive scratch/reference material under the project archive policy rather than deleting it.
- [ ] Existing real-data cross-format identity gates pass with global modules disabled: v2 `micro-uchuu-ascii-horizontal` and v3 `mini-millennium-horizontal`, `micro-uchuu-horizontal`, `micro-uchuu-hdf5-horizontal`, `millennium-horizontal`, `mini-uchuu-horizontal`, using each gate's existing model/epoch/file coverage (for `millennium-horizontal` and `mini-uchuu-horizontal` that coverage is the whole simulation, files 0–511 and 0–127, with the shipped vertical run file's smaller range overridden in a scratch copy by the gate itself). Missing datasets fail this acceptance requirement; they do not become SKIPs or narrower populations. Preserve the existing bitwise comparator and baseline anchor.
- [ ] Default full suite, `tests-horizontal-v3`, `tests-snapshot-global`, `tests-snapshot-global-identity` and explicit legacy SHAM module tests pass at the final code tree. Run real-data gates sequentially in separate worktrees as their harness expects, never competing for shared generated code. Record every exit status and unexpected skip; do not regenerate any scientific baseline.
- [ ] Write `docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-ACCEPTANCE.md` with exact commits/selectors, command/log identifiers, fixture versus real-data populations, expected/executed test counts, per-ID mismatch counts, numerical oracle results, memory/leak results, elapsed times and all residuals. The original brief's enabled-module gate is satisfied by the committed synthetic micro-Uchuu fixture, which isolates the new contract with a known oracle; an enabled real-data science or scale run is outside this increment. State that no production-scale global-run memory or observational validation was measured. PM must read actual outputs, not just an exit-code summary.
- [ ] Required evidence includes style-guide write mode/audit against `docs/STYLE-GUIDE.md`, differential lint and format/docs checks. Every emitted test case uses the framework marker convention, `tests/README.md` documents `tests/manual/` as explicitly invoked and never auto-discovered, and relevant validation documentation reflects the explicit horizontal model test targets.

### Authorized Surface

- Files allowed to change:
  - `Makefile` (snapshot-global acceptance targets/help only)
  - `tests/integration/test_snapshot_phase.py`
  - `tests/integration/test_snapshot_module_schema.py`
  - `tests/manual/test_snapshot_disabled_identity.py` (new file in a new directory)
  - `models/sham/modules/sham_global_rank/_tests/`
  - `docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-ACCEPTANCE.md`
  - `tests/README.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
- Functions/classes/components allowed to change: acceptance orchestration, independent comparison assertions and test-local temporary derivative construction. Read/reuse `tests/framework/parity_gate.py` and `scripts/compare_cross_format_identity.py` unchanged.
- Tests allowed or expected to change: only the named new tests and module-test envelopes. Reference builds/worktrees and captured logs are ignored operational outputs, not production fixture/baseline rewrites.

### Explicit Non-Goals

- No runtime repair outside this surface, changes to old baselines or comparator exceptions, committed fixture data, production datasets, external jobs/transfers, science calibration, CI configuration, or changes to the frozen plan. A discovered runtime defect must be reported with a reproducer rather than masked in the tests.

### Risk Flags

- Risky surfaces touched: build/test orchestration and scientific acceptance evidence. Real-data runs are local and read input only; outputs go to isolated scratch.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the two explicit targets plus the manual disabled-mode reference comparison; mutate comparator inputs in the test (drop/duplicate ID, perturb field, wrong snapshot/rank) to prove the new assertions fail. Never weaken the existing comparator.
- Commands to run: `make tests-snapshot-global`; `make tests-snapshot-global-identity`; `make MODEL=sage16 SIMULATION=mini-millennium tests summary`; `make tests-horizontal-v3`; legacy SHAM test commands from Slice 3; each real-data gate via `make MODEL=halos-only SIMULATION=<package> tests-scientific` for the six named packages, one at a time. Use a test subagent for long suites/gates, capture output and return pass/fail summaries. Restore/regenerate/rebuild default selectors. Re-run affected final-tree tests after any harness fix.
- Lint (differential, via the `lint` skill): required.
- Manual checks: verify references and feature binaries actually name their intended commits and timestepping scheme; inspect mismatch counts, HDF5 provenance, all skip reasons and leak reports. Run style audit, formatter, `make check-format` and `make check-docs`; review the validation skill for drift.

### Rollback Path

- Revert this evidence/harness slice without altering the preceding runtime commits; retain failed evidence as a failure record. Archive scratch/reference worktrees with repository policy rather than deleting user data.

## Slice 5: Publish the supported contract and reconcile guidance

### Intended Change

- Recommended Developer: Claude Sonnet; effort: medium. Requires accepted Slices 1–4, including the measured acceptance record.
- Finish permanent user/developer/module guidance, update the relevant project skills and pathway status, and audit wording against actual supported behavior.

### Acceptance Criteria

- [ ] Inputs: the implemented code and accepted Slice 4 evidence, not the aspirational brief, determine claims. All unresolved acceptance requirements stop closeout.
- [ ] Outputs: VISION Principle 4 and data flow distinguish existing FoF work from an additive post-snapshot population scope. User/developer guidance documents exact YAML placement, callback signature/context, ownership and mutation limits, current-generation/empty/non-output behavior, timing before inheritance/output, serial/HDF5/no-resume restrictions, supported mode-family extension points and failure behavior.
- [ ] SHAM documentation states the exact analytic cumulative function, units, candidates, peak evolution, deterministic tie key, rank convention, range failures and output fields; labels the example uncalibrated; distinguishes fixture completeness from whole-box science. No existing SHAM or SAGE guarantee is replaced by this feature.
- [ ] Relevant skills, template guidance and test instructions agree with the delivered modes/callbacks and explicit horizontal test targets, including the config-key reference and the debugging playbook's symptom table for the new startup rejections. The original three-mode standalone fallback remains documented. Do not introduce new permanent links to `docs/dev/` or cite ignored/machine-local evidence as technical authority.
- [ ] The pathway records this work as complete only when the acceptance record satisfies every gate; distributed operation and chunking remain separate future work. Do not archive or edit the frozen plan during the PM run; archiving is a later action by the user after PM completion.
- [ ] Required evidence: a differential style-guide audit explicitly covers every changed Markdown file under `docs/STYLE-GUIDE.md`, with no hard-wrapped prose and concrete resolution of findings; `make check-docs`, `./scripts/beautify.sh` and `make check-format` pass, with any failure outside the repository tree named as pre-existing. Skill sweep outcome is recorded. No code/test rerun is claimed from documentation checks alone.

### Authorized Surface

- Files allowed to change:
  - `docs/VISION.md`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/STYLE-GUIDE.md` (mode/lifecycle naming examples only)
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `models/sham/README.md`
  - `models/sham/modules/sham_global_rank/README.md`
  - `src/module_system/README.md`
  - `src/module_system/template/README.md`
  - `tests/README.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `.agents/skills/mimic-debugging-playbook/SKILL.md`
  - `.agents/skills/mimic-modules/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
- Functions/classes/components allowed to change: documentation only, limited to the feature and its supported-evidence statements.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No runtime/config/metadata changes, altered science claims beyond measured coverage, whole-guide rewrite, plan amendment, plan archiving or new dependency.

### Risk Flags

- Risky surfaces touched: none newly implemented; documentation of already accepted APIs and measured outcomes.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Tests to add/update: none; verify examples against the already tested run files and actual compiled API.
- Commands to run: `make check-docs`, `./scripts/beautify.sh`, `make check-format`; inspect their exit codes and ensure formatter caused no unauthorized code changes.
- Lint (differential, via the `lint` skill): required only if the installed linter covers a changed format; otherwise record the coverage gap and documentation checks.
- Manual checks: complete style-guide audit and relevant skill sweep; search for stale assertions of exactly three modes/exactly three lifecycle symbols and for permanent references to development plans. Preserve accurate FoF-only statements by qualifying their scope rather than globally replacing text.

### Rollback Path

- Revert documentation in a new commit without reverting accepted runtime behavior; restore accurate supported behavior wording if rolling runtime back later. Do not delete or archive the active frozen plan.

## Next Chat Prompts

### Mode A — Checkpointed alternative

```text
Plan file: docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md
Slices this session: Slice 1

Read the full plan. Stop before coding if any receipt is incomplete or baseline drift
changes its meaning. Work on the current branch; do not create a branch without my instruction.
Use orchestrator as the controlling skill. Keep implementation, tests and Git local.
Use the slice's recommended Developer model/effort and a fresh read-only Claude Fable 5.1
high-effort Reviewer for each independent review. Do not substitute or self-audit.
Restate the frozen surface and non-goals. Obtain approval for the flagged slice first.
Apply scoped-implementation and style-guide write mode. Run its validation and differential lint.
Apply drift-audit with the Reviewer, and report the authorization gate and provenance.
Only after it passes, run differential code-health for structural changes and independent
code-review. Fix findings in scope and repeat the affected gates at the final tree.
Report style audit findings and their resolution. Ask me before committing; then use commit.
After the selected slice is committed, use handoff and stop with the next slice recorded.
Confirm the plan, selected slice, branch and model/effort before beginning.
```

### Mode B — Supervised execution

```text
Plan file: docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: harness claude model opus effort high initially
Reviewer: harness claude model claude-fable-5-1 effort high

Use project-manager. You are the accountable PM and never write slice code.
Read the complete frozen plan; run check-plan with repo context. Require a clean committed
planning baseline, the recorded preflight results named in the plan, and resolve any drift
first. Use the current branch explicitly; do not create a new branch unless I authorize it.
Record human approval for each flagged slice before starting it; neither this launcher nor
the plan grants those approvals.
Ask for commit authorization before launch unless I have already explicitly granted it
for this run; once granted, PM Developer slice commits follow the PM contract.
Use a real isolated execution environment as project-manager requires. Keep PM_RUN_TOKEN
private to the PM seat. Preflight local datasets/toolchains without changing source data.

For each slice in order:
1. Launch a fresh Developer, explicitly passing that receipt's model and effort to
   start-slice (Opus high for 1–3, Sonnet high for 4, Sonnet medium for 5).
2. Wait with one long observe --wait sized to the preflight durations, asynchronously when
   possible. Do not poll a healthy session. Stop for actual dialogs and human gates; nudge
   only a genuine stall.
3. Check the mechanical floor, then the frozen authorization contract and actual evidence.
   Run differential lint and style-guide audit; investigate differential code-health for
   structural changes. Required test evidence in Acceptance Criteria binds both seats.
4. Commission fresh independent drift-audit; read and judge it and report authorization
   before commissioning fresh code-review. Both must cover the exact final commit.
   Rerun validation as required for elevated slices. Inspect actual output and skip counts.
5. Record reviewer/developer judgments and accept, steer or stop from repository evidence.
   A contract defect stops the run; do not amend this plan or invent missing science policy.
   Carry resolved decisions and outstanding evidence in PM notes.

Confirm the plan, branch, Developer/Reviewer models and effort, approvals and first slice.
After all slices are decided, read the PM report and report its verified total elapsed time,
accepted commits/evidence, audit provenance, plan defects, stops and residual limitations.
```
