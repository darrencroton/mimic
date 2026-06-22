# UniqueGalaxyID Encoding Implementation Plan

**Status:** Proposed implementation plan.
**Date:** 2026-06-21
**Source design:** `docs/dev/GALAXY-ID-ENCODING-REDESIGN.md`

## Plan State

This plan is anchored to `docs/VISION.md`, especially the requirements for format-agnostic I/O, reproducible output, metadata as structural truth, bounded memory, and fast failure. The redesign document is currently untracked in this checkout (`?? docs/dev/GALAXY-ID-ENCODING-REDESIGN.md`), so an implementation chat must confirm that document is intentionally in scope before editing or committing it.

The current implementation encodes `UniqueGalaxyID` in `src/core/build_model.c` from `(partition_output_id, unit, halonr)`. `PARTITION_PER_TASK` readers guard the old per-task ID capacity in `src/io/tree/read_ctrees_common.h`, `src/io/tree/read_ctrees_ascii.c`, and `src/io/tree/read_ctrees_hdf5.c`; `PARTITION_PER_FILE` readers have no equivalent per-file tree-count guard. `src/core/core_properties.yaml` and generated output schema metadata still describe the old `file*10^15 + tree*10^9 + creation_halonr` formula.

## Independent Assessment

The proposed two-term encoding is the right architectural direction. It removes MPI rank and file number from the identity, which directly improves reproducibility across MPI configurations and aligns with the vision that output semantics should be format-agnostic and not depend on engineering partition boundaries.

This is pre-v1.0 public-release work for a single current user, so backwards compatibility is not a requirement. The implementation should leave the codebase clean and fit for purpose for v1.0 rather than carrying compatibility shims, retired constants, stale comments, or old-output interpretation paths. If an old symbol, helper, documentation statement, or baseline artifact no longer has an active, tested purpose after the new scheme lands, remove or update it in the same slice that makes it obsolete.

The implementation should treat the new ID as run-scoped over the selected input set, not absolute over a full simulation unless the run includes the full simulation. The prefix sum should start at `first_file` for the run and should preserve the existing `PARTITION_PER_FILE` behavior of skipping missing input files. If a previously missing file appears or `first_file` changes, downstream IDs can change because the run input set changed. If cross-run identity across arbitrary subranges is required, this plan needs a design change before implementation.

Use a shared overflow helper instead of open-coded arithmetic in each reader. The safe total-forest guard is `total_forests <= LLONG_MAX / TREE_MUL_FAC`; with forest indices `0..total_forests-1`, the maximum encoded ID is `total_forests * TREE_MUL_FAC - 1`. The proposal's strict `<` guard is conservative by one forest, but a named helper makes the contract explicit and prevents inconsistent checks.

Do not reduce `TREE_MUL_FAC` in this implementation. The current value is the purpose-fit choice until Shin-Uchuu maximum per-forest halo counts are measured. Add central component validation so all readers, not only ctrees, reject `halonr >= TREE_MUL_FAC` before corrupting IDs.

The formula must not be activated until both reader families already provide `GlobalForestOffset`. Otherwise ctrees MPI output would collide across ranks because every rank would encode from offset zero.

## Slice 1: Encoding Primitives And Global State

### Intended Change
- Add a small, testable UniqueGalaxyID helper header that owns the two-term formula, capacity limit, and component validation.
- Add `GlobalForestOffset` as declared runtime state, initialized to zero, without changing current ID behavior yet.

### Acceptance Criteria
- Inputs: `halonr`, `forestnr_global`, `TREE_MUL_FAC`, and total forest counts passed to the helper functions.
- Outputs: helper functions report the maximum valid forest count, validate total forests and components, and encode `halonr + TREE_MUL_FAC * forestnr_global` for valid inputs.
- User-visible behaviour: none in this slice; existing output values remain unchanged because `make_unique_galaxy_id()` still uses the old formula.
- Behaviour that must not change: current readers, output file naming, output baseline values, MPI partitioning, and generated output schema.

### Authorized Surface
- Files allowed to change:
  - `src/include/galaxy_id.h` (new)
  - `src/include/globals.h`
  - `src/core/allvars.c`
  - `tests/unit/test_galaxy_id_encoding.c` (new)
- Functions/classes/components allowed to change:
  - New inline helpers such as `mimic_unique_galaxy_id_max_forests()`, `mimic_unique_galaxy_id_total_forests_valid()`, `mimic_unique_galaxy_id_components_valid()`, and `mimic_encode_unique_galaxy_id()`
  - Global declaration and definition for `int64_t GlobalForestOffset`
- Tests allowed or expected to change:
  - New model-neutral C unit coverage for boundary values, including `0`, `LLONG_MAX / TREE_MUL_FAC`, one over the limit, `halonr == TREE_MUL_FAC - 1`, and `halonr == TREE_MUL_FAC`

### Explicit Non-Goals
- Do not change `make_unique_galaxy_id()` yet.
- Do not add reader callbacks yet.
- Do not edit generated files or baselines in this slice.
- Do not change `TREE_MUL_FAC` or remove `FILENR_MUL_FAC`.

### Risk Flags
- Risky surfaces touched: shared global state, shared header/API contract.
- Approval needed before implementation: yes, because this introduces new global state and a shared encoding API.

### Validation Plan
- Tests to add/update: `tests/unit/test_galaxy_id_encoding.c`.
- Commands to run:
  - `tests/unit/run_tests.sh test_galaxy_id_encoding`
  - `make check-format`
- Manual checks:
  - Confirm no generated files changed.
  - Confirm `git diff` shows no behavioral changes outside the helper and global declaration/definition.

### Rollback Path
- Remove `src/include/galaxy_id.h`, remove the new unit test, and remove the `GlobalForestOffset` declaration/definition.

## Slice 2: PARTITION_PER_FILE Offset Scan

### Intended Change
- Extend `struct TreeReader` with a `count_partition_trees` callback for `PARTITION_PER_FILE` readers.
- Implement allocation-free tree counting for L-Halo binary and L-Halo HDF5 readers.
- Add a one-time prefix-sum table in the `PARTITION_PER_FILE` branch of `run_tree_driver()` and set `GlobalForestOffset` before each partition is processed.

### Acceptance Criteria
- Inputs: configured `first_file..last_file`, existing or missing partition files, and per-file tree counts from L-Halo binary/HDF5 headers.
- Outputs: a per-partition `int64_t` offset table where each present file's offset equals the sum of tree counts in earlier present files in the selected run; missing files contribute zero and are still skipped as today.
- User-visible behaviour: none in this slice while the old formula is still active; missing `PARTITION_PER_FILE` input files continue to log and skip rather than becoming fatal.
- Behaviour that must not change: output file naming, `--skip` semantics, MPI striding over partition indices, `open_partition()` ownership of per-partition metadata, and HDF5/non-HDF5 build registration.

### Authorized Surface
- Files allowed to change:
  - `src/io/tree/reader.h`
  - `src/io/tree/binary.c`
  - `src/io/tree/hdf5.c`
  - `src/io/tree/read_ctrees_ascii.c`
  - `src/io/tree/read_ctrees_hdf5.c`
  - `src/core/main.c`
  - `tests/unit/test_tree_reader_counts.c` (new, binary count coverage only)
- Functions/classes/components allowed to change:
  - `struct TreeReader`
  - `LHaloBinaryReader` and `LHaloHDF5Reader` initializers
  - `CTreesAsciiReader` and `CTreesHDF5Reader` initializers only to set `.count_partition_trees = NULL`
  - New static count helpers in the L-Halo readers
  - `run_tree_driver()` and new static helpers local to `src/core/main.c` for partition path/existence and prefix-sum construction
- Tests allowed or expected to change:
  - New C unit coverage for `LHaloBinaryReader.count_partition_trees()` against a tiny synthetic binary header

### Explicit Non-Goals
- Do not activate the new UniqueGalaxyID formula yet.
- Do not change `PARTITION_PER_TASK` readers in this slice.
- Do not make missing L-Halo input files fatal.
- Do not allocate per-tree halo arrays during count callbacks.

### Risk Flags
- Risky surfaces touched: shared reader interface, global runtime state, MPI partition semantics.
- Approval needed before implementation: yes, because this changes the tree-reader contract used by every registered reader.

### Validation Plan
- Tests to add/update: `tests/unit/test_tree_reader_counts.c`.
- Commands to run:
  - `tests/unit/run_tests.sh test_tree_reader_counts`
  - `make USE-HDF5=no`
  - `make`
- Manual checks:
  - Confirm every `struct TreeReader` initializer explicitly sets `.count_partition_trees`, with `NULL` for `PARTITION_PER_TASK` readers.
  - Confirm the prefix-sum table is freed after the tree driver completes.
  - Confirm the prefix scan uses the same path rules as the processing loop, including `format_partition_path`.

### Rollback Path
- Remove the callback field and count helpers, restore the original `run_tree_driver()` per-file loop, and remove the new unit test.

## Slice 3: PARTITION_PER_TASK Offsets And Product Guards

### Intended Change
- Store each ctrees task's global start forest in reader state and assign `GlobalForestOffset` during ctrees partition setup.
- Add the new total-forest product guard to ctrees ASCII and HDF5 setup while leaving old task-rank/per-task guards in place until activation.
- Confirm weighted and uniform ctrees distribution starts are global contiguous offsets.

### Acceptance Criteria
- Inputs: ctrees ASCII forest lists, ctrees HDF5 metadata, selected forest distribution scheme, `ThisTask`, and `NTask`.
- Outputs: `GlobalForestOffset` equals the global start forest assigned to the current task; total forest counts above the new encoding capacity fail before any forest is processed.
- User-visible behaviour: existing ctrees runs still produce the old IDs in this slice; full-Uchuu still fails on the old per-task guard until Slice 4 removes it.
- Behaviour that must not change: forest distribution, file/forest mapping, ctrees value conventions, per-forest halo-count guard, and existing ctrees unit tests.

### Authorized Surface
- Files allowed to change:
  - `src/io/tree/read_ctrees_ascii.c`
  - `src/io/tree/read_ctrees_hdf5.c`
  - `src/io/tree/read_ctrees_common.h`
  - `tests/unit/test_ctrees_support.c`
- Functions/classes/components allowed to change:
  - `struct ctrees_ascii_partition`
  - `struct ctrees_hdf5_partition`
  - `open_partition_ctrees_ascii()`
  - `setup_forests_io_ctrees_hdf5()`
  - ctrees distribution unit-test helpers
- Tests allowed or expected to change:
  - Add or strengthen unit assertions that uniform and weighted distributions form contiguous global ranges and that `start + count` never exceeds `totnforests`

### Explicit Non-Goals
- Do not remove `CTREES_MAX_FORESTS_PER_TASK` or `CTREES_MAX_TASK_ID` yet.
- Do not change ctrees forest distribution algorithms.
- Do not change `make_unique_galaxy_id()` yet.
- Do not add MPI communication.

### Risk Flags
- Risky surfaces touched: tree-reader setup, global runtime state, large-run startup validation.
- Approval needed before implementation: yes, because this touches ctrees startup and full-scale simulation behavior.

### Validation Plan
- Tests to add/update: `tests/unit/test_ctrees_support.c`.
- Commands to run:
  - `tests/unit/run_tests.sh test_ctrees_support`
  - `make validate-modules`
  - `make`
- Manual checks:
  - Confirm `GlobalForestOffset` is assigned for tasks with zero forests as well as tasks with work.
  - Confirm HDF5 and ASCII readers use the same capacity helper from Slice 1.

### Rollback Path
- Remove the new ctrees state fields, remove `GlobalForestOffset` assignments and product guards, and restore the previous ctrees unit tests.

## Slice 4: Activate Two-Term Encoding

### Intended Change
- Switch `make_unique_galaxy_id()` to encode from `(GlobalForestOffset + unit, halonr)`.
- Remove `partition_output_id` from the build-model recursion and inheritance call chain.
- Remove old partition-term ID guards, constants, branches, and comments now made obsolete by the activated formula unless a remaining active use is documented and tested.
- Update the source property description and generated schema metadata through `make generate`.

### Acceptance Criteria
- Inputs: `GlobalForestOffset`, `unit`, and `halonr` for every processed tree/forest.
- Outputs: `UniqueGalaxyID = halonr + TREE_MUL_FAC * (GlobalForestOffset + unit)` for every newly created galaxy; invalid components fail fast before returning an ID.
- User-visible behaviour: IDs become independent of MPI rank and partition output id for the same selected input set and forest ordering.
- Behaviour that must not change: `UniqueCentralGalaxyID` propagation, galaxy inheritance semantics, output file names, `FileNum`/`TreeID` globals used for diagnostics, property field type/layout, and binary/HDF5 output schema structure apart from the `UniqueGalaxyID` description text.

### Authorized Surface
- Files allowed to change:
  - `src/core/build_model.c`
  - `src/include/proto.h`
  - `src/io/tree/read_ctrees_ascii.c`
  - `src/io/tree/read_ctrees_hdf5.c`
  - `src/io/tree/read_ctrees_common.h`
  - `src/include/constants.h`
  - `src/core/core_properties.yaml`
  - Generated files changed by `make generate`, expected to include `src/include/generated/hdf5_field_metadata.inc`, `src/include/generated/output_schema_writer.inc`, and `tests/generated/property_ranges.json`
  - `tests/integration/test_unique_galaxy_id_encoding.py` (new)
- Functions/classes/components allowed to change:
  - `make_unique_galaxy_id()`
  - `build_halo_tree()`
  - `join_progenitor_halos()`
  - ctrees old ID-limit guard sites and obsolete constants/comments
- Tests allowed or expected to change:
  - Add an integration test that builds a temporary two-file L-Halo binary input from the existing mini-Millennium fixture and verifies file 1 IDs follow the file 0 tree-count offset, not `FILENR_MUL_FAC`
  - Existing `tests/integration/test_output_formats.py::test_unique_id_contract`

### Explicit Non-Goals
- Do not change `TREE_MUL_FAC`.
- Do not keep `FILENR_MUL_FAC` solely for backwards compatibility, documentation continuity, or old-output interpretation. Remove it if the new implementation leaves no active code path using it.
- Do not change output file numbering or HDF5 master-file partition discovery.
- Do not attempt full-Uchuu production processing in this slice.

### Risk Flags
- Risky surfaces touched: output identity contract, generated metadata, shared core call signatures.
- Approval needed before implementation: yes, because this is the behavioral cutover and changes published IDs for multi-file or multi-task runs.

### Validation Plan
- Tests to add/update: `tests/integration/test_unique_galaxy_id_encoding.py`.
- Commands to run:
  - `make generate`
  - `make check-generated`
  - `make`
  - `tests/unit/run_tests.sh test_galaxy_id_encoding test_ctrees_support test_tree_reader_counts`
  - `MODEL=sage16 SIMULATION=mini-millennium python3 tests/integration/test_unique_galaxy_id_encoding.py`
  - `MODEL=sage16 SIMULATION=mini-millennium python3 tests/integration/test_output_formats.py`
- Manual checks:
  - Confirm `rg "partition_output_id" src/core src/include/proto.h` has no stale build-model parameter references.
  - Confirm `rg "CTREES_MAX_FORESTS_PER_TASK|CTREES_MAX_TASK_ID" src/io/tree` has no active old-limit checks.
  - Confirm `rg "FILENR_MUL_FAC" src tests models simulations docs` finds no stale active-code or current-documentation references; any remaining historical reference must be intentionally scoped and not part of v1.0 runtime guidance.
  - Confirm generated files were produced by the generator, not hand-edited.

### Rollback Path
- Restore the old `make_unique_galaxy_id()` signature and formula, restore `partition_output_id` propagation, restore ctrees old guards, revert generated metadata, and remove the new integration test.

## Slice 5: Documentation, Baselines, And Full Gate

### Intended Change
- Update developer documentation and planning notes to describe the new run-scoped global forest index formula, removing stale old-formula guidance rather than preserving it for backwards compatibility.
- Refresh committed baseline metadata and any output files that actually change after the official baseline regeneration commands.
- Run the final formatting, style, skill, generated-code, and test gates.

### Acceptance Criteria
- Inputs: completed Slice 4 implementation and regenerated default-package outputs.
- Outputs: documentation and baseline artifacts describe the active formula; baseline tests pass against committed artifacts.
- User-visible behaviour: users reading output schema metadata and developer docs see `forestnr_global` rather than the old file/task partition formula.
- Behaviour that must not change: baseline refresh must not hide unrelated physics changes; any changed baseline data file must be explained by the intentional ID/schema change; no current v1.0 documentation should instruct users or developers to reason about the old partition-term ID formula.

### Authorized Surface
- Files allowed to change:
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/dev/CTREES-UCHUU-VALIDATION.md`
  - `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`
  - `docs/dev/GALAXY-ID-ENCODING-REDESIGN.md` only if the implementation chat confirms the currently untracked design doc is intentionally in scope
  - `tests/data/README.md` only if the baseline regeneration instructions need clarification
  - `tests/data/output/baseline/binary/metadata/output_schema.json`
  - `tests/data/output/baseline/hdf5/metadata/output_schema.json`
  - `tests/data/output/baseline/hdf5/model.hdf5` and `tests/data/output/baseline/hdf5/model_000.hdf5` if HDF5 FieldMetadata changes
  - `models/sage16/modules/_tests/baseline/physics-binary/metadata/output_schema.json`
  - Other files under `tests/data/output/baseline/` or `models/sage16/modules/_tests/baseline/` only if regenerated outputs differ and the diff is reviewed as intentional
- Functions/classes/components allowed to change:
  - Documentation prose and committed generated/baseline artifacts only
- Tests allowed or expected to change:
  - No new test logic unless Slice 4 validation exposes a missing assertion

### Explicit Non-Goals
- Do not hand-edit HDF5 or binary baseline data.
- Do not regenerate baselines to mask failures unrelated to the ID formula.
- Do not update Shin-Uchuu guidance beyond preserving the existing open question about measuring maximum halos per forest.
- Do not keep stale compatibility notes, dead-code references, or old ID formula descriptions in current user/developer guidance.
- Do not commit without explicit user approval.

### Risk Flags
- Risky surfaces touched: committed baselines, generated metadata, documentation of public output semantics.
- Approval needed before implementation: yes, because committed baseline artifacts are reference data and generated files are in scope.

### Validation Plan
- Tests to add/update: none expected.
- Commands to run:
  - `./scripts/beautify.sh`
  - `make check-format`
  - `make check-generated`
  - `make validate-modules`
  - `make tests-scientific summary`
  - Delegate long-running `make tests-unit summary` and `make tests-integration summary` to a subagent, with logs under `archive/test-logs/` and exit codes reported
  - If time and environment permit, run `make tests summary` as the final full-suite gate, also delegated because it can exceed one minute
- Manual checks:
  - Re-read the full diff against `docs/STYLE-GUIDE.md`.
  - Perform the skill sweep for `.agents/skills/mimic-tests/SKILL.md`, `.agents/skills/mimic-properties/SKILL.md`, and `.agents/skills/mimic-debug/SKILL.md`; update only if the implementation makes them stale.
  - Inspect baseline diffs with attention to `UniqueGalaxyID`, `UniqueCentralGalaxyID`, and schema descriptions.
  - Run `rg "file\\*10\\^15|FILENR_MUL_FAC|partition-term|old formula|retired" docs src tests models simulations` and either remove stale hits or document why each remaining hit is historical rather than current v1.0 guidance.
  - Confirm no unrelated untracked files were added.

### Rollback Path
- Restore documentation and baseline artifacts from the previous commit, then rerun baseline comparison tests to confirm the repository returns to the pre-refresh state.

## Cross-Slice Notes

- Slices 1 through 3 are preparation and should preserve output values.
- Slice 4 is the behavioral cutover and should not start until Slices 1 through 3 are complete.
- Slice 5 should follow Slice 4 before the work is considered shippable; Slices 4 and 5 are tightly coupled for release even if they are reviewed as separate authorization surfaces.
- This is a v1.0 cleanup, not a backwards-compatibility migration. The final implementation should not leave dead constants, compatibility branches, stale generated descriptions, or current docs for the previous encoding.
- All generated files must be produced by `make generate`; do not hand-edit files under `generated/`.
- Unit and integration suites can be long in this repository. Capture long runs to `archive/test-logs/`, check exit codes explicitly, and summarize failures rather than pasting raw logs.

## Next Chat Prompt

```md
Plan file: docs/dev/GALAXY-ID-ENCODING-IMPLEMENTATION-PLAN.md
Slices this session: Slice 1

Read the full plan file first. If the selected slice receipt is incomplete or the plan state is unclear, stop and tell me before coding.

Work on the current feature branch for this plan; if none exists, create one and tell me the name.

Use ai-orchestrator as the controlling skill. Keep the implementation local; delegate per that skill's guidance when independence or context economy helps, primarily hostile drift-audit, independent code-review, and long-running tests.

For each selected slice, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. If the slice's Risk Flags mark approval-needed, stop and get my approval before coding.
3. Apply scoped-implementation against the slice contract.
4. Apply drift-audit. Report the authorization gate result before any quality review.
5. If the gate passes, apply code-review. If it fails, fix the drift and re-audit.
6. Surface drift and review findings to me, fix them, then re-run the relevant gate.
7. Ask me before committing. On my approval, commit that slice with the commit skill.

After the selected slice is committed, use handoff to record state and the next slice to resume from. Do not continue past the selected slice.

Confirm before starting: plan file read, selected slice, branch, and the first slice.
```
