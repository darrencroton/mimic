# Phase 0 Driver Dispatch Implementation Plan

**Status:** Frozen implementation plan
**Date:** 2026-06-19
**Scope:** Implement Phase 0 from `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`: establish the processing-driver/input-ordering axis for `tree_ordered` and future `snapshot_ordered` runs, with only the existing tree driver wired in this slice.

## Context

Mimic already has a reader-format axis: `input.tree_type` resolves through `src/io/tree/registry.c` to a `struct TreeReader`, and each reader declares a partition model (`PARTITION_PER_FILE` or `PARTITION_PER_TASK`) in `src/io/tree/reader.h`. That is not the same as the processing-driver axis. Phase 0 must add the explicit driver/input-ordering seam so Mimic has independent concepts for reader format and processing order:

- Reader format: `lhalo_binary`, `lhalo_hdf5`, `consistent_trees_ascii`, `consistent_trees_hdf5`.
- Processing driver/input ordering: `tree_ordered` now; `snapshot_ordered` recognized but not implemented until later phases.

Current worktree note: the repository is already dirty in `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` and `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md` from the planning refresh. Do not revert those changes. Phase 0 implementation may update those same docs if it completes the status change.

## Slice 1: Phase 0 Driver Dispatch Seam

### Intended Change

- Add a first-class input ordering / processing driver setting with accepted values `tree_ordered` and `snapshot_ordered`.
- Default the setting to `tree_ordered` so existing run files and simulation package configs keep working unchanged.
- Keep `input.tree_type` as the reader-format selector; do not rename it and do not use it to imply processing order.
- Annotate current registered readers as `tree_ordered` compatible, and validate the selected reader against the selected processing order.
- Extract the current tree-processing loop from `main()` into a named `run_tree_driver()` function without changing execution order, output naming, MPI partitioning, skip semantics, HDF5 finalization, master-file aggregation, metadata writing, memory cleanup, or logging phases.
- Add a top-level dispatch step that calls `run_tree_driver()` for `tree_ordered`.
- Recognize `snapshot_ordered` in configuration but fail fast with a clear "snapshot-ordered driver is not implemented yet" error unless and until later phases add the driver.
- Update user/developer documentation so the difference between `input.tree_type` and the new input-ordering setting is explicit.

### Acceptance Criteria

- Inputs:
  - Existing run files and simulation configs that omit the new ordering field still run as `tree_ordered`.
  - A run file or simulation config may set the new ordering field to `tree_ordered` and runs identically to omission.
  - A run file or simulation config may set the new ordering field to `snapshot_ordered`, but startup validation fails before tree processing with a clear not-implemented message.
  - Unknown ordering values fail during configuration validation with a clear accepted-values message.
- Outputs:
  - For default `tree_ordered` runs, binary and HDF5 outputs remain identical to the current release-candidate baseline.
  - HDF5 `tree_type` metadata remains the reader-format string, not the processing-order string.
  - If a new output/provenance field records processing order, it records `tree_ordered` for existing runs without changing halo records.
- User-visible behaviour:
  - `input.tree_type` remains the documented format selector.
  - The new ordering setting is documented as the processing-driver selector and defaults to `tree_ordered`.
  - `snapshot_ordered` is visible as a recognized future option but clearly unavailable in v1.0.
- Behaviour that must not change:
  - No current run YAML is required to add the new key.
  - Current L-Halo and Consistent-Trees readers keep their `PARTITION_PER_FILE` / `PARTITION_PER_TASK` semantics unchanged.
  - UniqueGalaxyID generation remains unchanged for all existing readers.
  - `--skip`, partial-output detection, output file naming, HDF5 master-file scan ranges, CPU-limit handling, debug rate limiting, module init/cleanup, memory leak checking, metadata copying, and final run status remain unchanged.
  - No snapshot reader, snapshot driver, snapshot-global operation hook, or converter contract is implemented in this slice.

### Authorized Surface

- Files allowed to change:
  - `src/include/types.h`
  - `src/core/read_parameter_file.c`
  - `src/core/main.c`
  - `src/io/tree/reader.h`
  - `src/io/tree/binary.c`
  - `src/io/tree/hdf5.c`
  - `src/io/tree/read_ctrees_ascii.c`
  - `src/io/tree/read_ctrees_hdf5.c`
  - `src/io/output/metadata_hdf5.c` only if processing-order provenance is added to HDF5 run metadata
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`
  - `tests/unit/test_parameter_parsing.c`
  - `tests/integration/test_output_formats.py`
  - `tests/integration/test_module_pipeline.py` or a new narrowly named integration test under `tests/integration/` if a subprocess startup-failure test fits better there
- Functions/classes/components allowed to change:
  - `struct MimicConfig`
  - `struct TreeReader`
  - `parse_cli()` only if needed for initialization defaults, not for a new CLI flag
  - `parse_input_section()`
  - `validate_and_postprocess()`
  - The current tree-processing loop in `main()`, by extraction into `run_tree_driver()`
  - `process_partition()` and `claim_and_process_partition()` only for signature cleanup required by `run_tree_driver()` extraction; their behaviour must not change
  - Current `TreeReader` constant initializers
  - Targeted docs sections describing input formats/readers and the dual-driver status
- Tests allowed or expected to change:
  - Add parser assertions that the omitted ordering field defaults to `tree_ordered`.
  - Add parser assertions that explicit `tree_ordered` is accepted.
  - Add subprocess integration coverage for invalid ordering and `snapshot_ordered` not implemented, because parser fatal paths exit the process.
  - Existing binary/HDF5 output baseline tests should remain the behavioural guard; do not weaken or remove them.

### Explicit Non-Goals

- Do not implement the snapshot-ordered reader model from Phase 4.
- Do not implement the snapshot driver from Phase 5.
- Do not change any existing reader's partition semantics.
- Do not rename `input.tree_type`.
- Do not require existing simulation packages, run files, test inputs, or user configs to add the new ordering key.
- Do not change module ABI, module execution order, inheritance semantics, output-buffer semantics, galaxy-pool lifetime, or HDF5 table layout for halo data.
- Do not refresh baselines unless a separate reviewed decision accepts an intentional metadata/provenance-only baseline update; halo records should not drift.
- Do not update `docs/VISION.md`; the vision review remains gated on implemented snapshot-driver behaviour.

### Risk Flags

- Risky surfaces touched:
  - Public YAML configuration schema (`input` section).
  - Global runtime configuration (`struct MimicConfig`).
  - Main program control flow and MPI partition loop.
  - Reader interface ABI (`struct TreeReader` initializers).
  - Output provenance if HDF5 metadata records the new processing-order field.
- Approval needed before implementation:
  - Yes. This slice touches public config and main runtime flow. The next implementation chat must explicitly request this slice and confirm the authorized surface before coding.

### Validation Plan

- Tests to add/update:
  - Extend `tests/unit/test_parameter_parsing.c` to assert the default ordering is `tree_ordered` after parsing an existing fixture.
  - Add a generated or temporary fixture with explicit `tree_ordered` and assert it parses successfully.
  - Add subprocess integration tests for unknown ordering and explicit `snapshot_ordered`, asserting non-zero exit and the expected error text.
  - Rely on existing output-format baseline tests to prove tree-driver output did not drift.
- Commands to run:
  - `make check-generated`
  - `make validate-modules`
  - `make check-docs`
  - `make check-format`
  - `make tests-unit summary`
  - `make tests-integration summary`
  - `make tests-scientific summary`
  - If HDF5 is available, ensure the HDF5 output-format tests run and pass as part of integration summary.
- Manual checks:
  - Run `rg -n "tree_format|TreeFormat|snapshot_ordered|tree_ordered|tree_type" src docs tests simulations models` and confirm docs consistently distinguish reader format from processing order.
  - Inspect `src/core/main.c` after extraction and verify all former processing steps still occur in the same order: config/init, module init, HDF5 setup, tree processing, HDF5 master/binary finalization, memory report, cleanup, leak check, metadata write, success status.
  - Inspect all `struct TreeReader` initializers and confirm every current reader declares tree-ordered compatibility explicitly.
  - Confirm no generated files under `*/generated/` were hand-edited.

### Rollback Path

- Revert this slice as one commit if runtime dispatch causes instability.
- Rollback should remove the new ordering field from `MimicConfig`, remove parsing/validation for the new input key, restore the direct main tree loop, remove reader ordering annotations, and revert docs/tests added for Phase 0.
- Since the slice must not alter halo records or existing run-file requirements, rollback should not require baseline regeneration.

## Next Chat Prompt

```md
Plan file: PHASE0-DRIVER-DISPATCH-IMPLEMENTATION-PLAN.md
Working scope: Phase 0 Driver Dispatch Seam

Read the full plan file before starting. 

Use the `ai-orchestrator` skill as the controlling workflow for this session. The orchestrator owns the work and may do much of it directly, but should delegate when independence, context economy, or long-running execution matters, especially for reviews/audits, plan critique, codebase mapping, and test runs that would fill the main context.

Follow the repo workflow:

1. Create a new branch for this work and switch to it.
2. Identify the slice(s) in the selected working scope from the plan file.
3. Work through those slices in plan order.

For each slice:

4. Apply the `scoped-implementation` skill using the slice contract from the plan.
5. After each implementation, apply the `drift-audit` skill before any quality review. Use independent delegation when that makes the audit stronger.
6. If drift audit passes, apply the `code-review` skill. Prefer independent delegation when review independence matters.
7. Fix any drift or review findings before moving to the next slice.
8. Once the slice passes validation, drift audit, and code review, commit that slice to the branch using the `commit` skill. This prompt is explicit approval to commit each completed slice.
9. After committing each slice, use the `handoff` skill to record progress, current validation state, and the next restart point if the work continues in another chat.

Continue until all planned slices in the selected working scope are complete, or stop earlier if there is a blocker, unapproved scope change, or failed validation that cannot be resolved within the slice contract.

Start by confirming:
- the plan file you read
- the selected working scope
- the branch name
- the ordered slice list you will execute
- the first slice you are starting
```
