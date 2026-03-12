# Review: Immediate Merger Cleanup Sign-off

Date: 2026-03-12
Reviewed commit: `53fb19044feee61ac3a9d38144dc55536c9efb1f`
Status: Historical review snapshot

Update: The two sign-off blockers in this review were completed in the working
tree later on 2026-03-12. Use
`docs/workplan-immediate-merger-followup-2026-03-12.md` as the authoritative
record of what remains after sign-off.

## Sign-off blockers

### 1. `sage_handle_mergers_immediate` still advertises unsupported processing modes

Severity: High

This is the most important issue in the current tree.

Evidence:
- `src/modules/_system/generated/module_init.c:130` still declares `sage_handle_mergers_immediate_supported_modes[] = {PROCESSING_MODE_FULL_HALO, PROCESSING_MODE_PER_EVENT, PROCESSING_MODE_BY_GALAXY}`.
- `scripts/generate_module_registry.py:159-171` shows why: standalone `.c` modules default to all three modes when no explicit metadata exists.
- `src/modules/sage_handle_mergers_immediate.c:74-85` and `src/modules/sage_handle_mergers_immediate.c:79-185` make it clear this module expects the full halo array, finds the FOF central, and iterates over all satellites. It is not a valid `process_by_galaxy` or `process_per_event` module.
- `src/core/module_registry.c:189-196` shows that startup validation trusts the advertised supported-mode list.

Impact:
- A misconfigured YAML using `sage_handle_mergers_immediate: process_by_galaxy` or `process_per_event` would incorrectly pass startup validation.
- Because this is now the only live SAGE merger path, the safety contract around processing mode is part of the correctness boundary.

Required fix:
- Promote follow-up Task 2 to a pre-sign-off fix.
- Give `sage_handle_mergers_immediate` explicit metadata so it advertises only `process_full_halo`.
- Add a negative validation test that intentionally configures the wrong mode and asserts startup rejection.

### 2. No automated end-to-end test covers the only live merger event chain

Severity: High

The current tests cover the immediate merger handler in isolation and the per-event consumers in isolation, but not the production chain that now matters most:

`sage_handle_mergers_immediate -> module_emit_event() -> process_per_event consumers`

Evidence:
- `src/modules/sage_handle_mergers_immediate/_tests/test_unit_sage_merger_ordering_parity.c:245-367` calls `sage_handle_mergers_immediate_process(...)` directly in a unit-test context.
- `src/core/module_registry.c:462-466` explicitly drops emitted events when no phase-dispatch context is active, so those unit tests do not exercise `sage_quasar_mode` or `sage_collisional_starburst`.
- `src/modules/_tests/module_info.yaml:54-70` has no integration test registered for the immediate merger event chain.
- Existing consumer tests exercise `process_per_event` behavior by constructing synthetic events directly: `src/modules/_tests/test_unit_sage_quasar_mode.c:355-405` and `src/modules/_tests/test_unit_sage_collisional_starburst.c:467-520`.

Impact:
- The sole live merger path can regress in event payload, dispatch timing, or consumer reachability without an automated integration test catching it.
- Manual debug traces in `docs/bugreport-sage-merger-parity-pathway-investigation-2026-03-12.md` are useful evidence, but they are not a regression guard.

Required fix:
- Promote follow-up Task 3 into the pre-sign-off set.
- Add a dedicated integration test for the immediate handler's merger-event dispatch chain.
- Prefer a deterministic small integration fixture or temporary config over a broad output-only heuristic on the standard dataset.

## Non-blocking observations

- `make validate-modules` passed on 2026-03-12. The only output was an unrelated existing warning: `sage_calculate_cooling: No documentation specified`.
- `make test-unit` passed on 2026-03-12: 28/28 unit tests green.
- `make test-integration` passed on 2026-03-12.
- The removed split files are archived under `ignore/archived-split-pass-modules/`, so the cleanup did preserve the historical code outside the live tree.
- Live user docs are updated correctly: `docs/USER-GUIDE.md:195-258` now describes the immediate merger path.

## Follow-up workplan assessment

Workplan reviewed: `docs/workplan-immediate-merger-followup-2026-03-12.md`

### Task 1: Post-Minor-Merger Disk Instability Recheck

Assessment: Valid concern, but the implementation guidance needs revision.

Why it is valid:
- SAGE really does recheck disk instability after a minor-merger starburst: `sage-code/sage/model_mergers.c:483-490`.
- Mimic does not currently do that in the merger-event path. `src/modules/sage_collisional_starburst.c:98-138` handles merger-triggered starbursts and returns; there is no post-starburst disk-instability recheck.
- The current phase structure confirms why this gap exists: `input/millennium.yaml:57-67` runs disk instability in `phase_1`, but merger starbursts happen later in `phase_2`.

What should be revised in the workplan:
- The current Option A wording is incomplete. In SAGE, `check_disk_instability(...)` does more than compute instability; it also triggers black-hole growth and a collisional starburst for unstable gas via `grow_black_hole(...)` and `collisional_starburst_recipe(...)` (`sage-code/sage/model_disk_instability.c:119-143`).
- Therefore, simply reusing the current Mimic `sage_disk_instability` logic is not enough. Mimic's `sage_disk_instability` only computes instability and sets `UnstableDiskGasFraction` for phase_1 consumers (`src/modules/sage_disk_instability.c:110-118`).
- `src/modules/sage_clear_disk_instability_triggers.c:1-30` is not the core problem here; it only clears the phase_1 trigger after phase_1 consumers have run.

Recommendation:
- Keep this as a module-layer fix.
- Revise the task so the intended implementation explicitly reproduces the full SAGE post-minor-merger behavior, not just the instability check.
- The clean design is probably a shared helper that performs the instability calculation and then immediately invokes the same downstream physics required in the merger context, rather than trying to reuse `UnstableDiskGasFraction` as a cross-phase flag.
- Also state clearly that Mimic's equivalent of SAGE `DiskInstabilityOn` is module presence/configuration, not a standalone boolean parameter.

### Task 2: Tighten module registration for `sage_handle_mergers_immediate`

Assessment: Fully valid and should be promoted from follow-up to sign-off blocker.

Why it is valid:
- The bug exists now in the current tree: `src/modules/_system/generated/module_init.c:130`.
- The root cause is exactly as the workplan says: `scripts/generate_module_registry.py:159-171` gives every standalone `.c` module all three processing modes unless metadata says otherwise.
- The module implementation is full-halo only: `src/modules/sage_handle_mergers_immediate.c:74-85`.

Recommendation:
- Fix this before sign-off.
- The proposed directory-module solution is reasonable and consistent with the architecture.
- If the team prefers not to move files, an explicit metadata override for standalone modules would also solve the immediate problem, but it should not be comment-based or ad hoc. Keep it metadata-driven.
- The workplan should explicitly add a short audit of other standalone modules after fixing this one. This generator default is broader than just the merger handler.

### Task 3: Integration-level event consumer test

Assessment: Valid concern, but the proposed test design should be revised.

Why it is valid:
- The current test coverage does not exercise the production dispatch chain for immediate mergers. See the evidence in blocker 2 above.

Why the current proposal should be revised:
- Running `./mimic --quiet input/millennium.yaml` on the standard dataset and then inferring event-consumer behavior from final output fields is too indirect and too noisy.
- Final outputs do not preserve a clean per-event baseline, so statements like "BlackHoleMass is larger than the pre-merger baseline" are not robust unless a control run is constructed very carefully.
- The workplan's optional control-run idea is better than a pure one-run heuristic, but it still uses a large, high-variance setup for what should be a deterministic regression test.

Recommendation:
- Keep the concern, but redesign the acceptance path around a small deterministic integration fixture or temporary config.
- The test should run through real phase dispatch, not direct unit calls, and should compare:
  1. immediate handler + event consumers enabled
  2. the same setup with the relevant consumer disabled
- Assert direct observable differences on the merger remnant that are tightly tied to the consumer being tested.

## Additional follow-up items to add

1. Add a negative configuration test for Task 2.
   - Once `sage_handle_mergers_immediate` is restricted to `process_full_halo`, include a test that intentionally configures it as `process_by_galaxy` and asserts the startup validator rejects the config with a clear message.

2. Add a deterministic fixture for Task 1.
   - Do not rely only on broad Millennium statistics to validate the post-minor-merger disk-instability recheck.
   - Add a focused fixture where a merger starburst leaves the remnant just over the instability threshold so the expected behavior is unambiguous.

## Bottom line

The split-path cleanup itself is directionally correct and the current tree is clean enough to validate and run. The remaining blockers are now concentrated and well-defined:

1. fix the immediate handler's advertised processing modes
2. add an end-to-end automated test for its merger-event dispatch chain

After those are addressed, the remaining work naturally moves into parity-fidelity follow-up, especially the post-minor-merger disk-instability recheck.
