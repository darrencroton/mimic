# Workplan: SAGE-Mimic Parity Investigation for Merger/Disruption Immediate Ordering

Date: 2026-03-08
Status: Ready for execution
Audience: Fresh dev team (core + SAGE module maintainers)
Primary sources:
- `sage-code/sage/core_build_model.c` (`handle_mergers(...)`)
- `sage-code/sage/model_mergers.c` (`deal_with_galaxy_merger(...)`, `disrupt_satellite_to_ICS(...)`)
- `docs/sage-merger-ordering-parity-contract.md`

## 1) Problem Statement

Mimic currently does not execute satellite merger/disruption decisions in the same immediate in-loop order as SAGE.

SAGE behavior (single-pass immediate handling):
- File: `sage-code/sage/core_build_model.c` (see `handle_mergers`, around lines 489-549).
- For each satellite in index order, SAGE immediately:
1. decrements `MergTime`,
2. checks eligibility,
3. resolves target,
4. if the chosen target already has `mergeType > 0`, redirects exactly one hop via that target's `CentralGal`,
5. executes either disruption or merger on the spot,
6. mutates shared state before the next satellite is processed.

Current Mimic behavior (split-pass handling):
- Files:
- `src/modules/sage_update_merger_time.c`
- `src/modules/sage_merge_galaxies.c`
- `src/modules/sage_disrupt_satellites.c`
- `input/millennium.yaml` (`phase_2`, lines 63-69)
- Mimic first flags all candidates (`IsMerging`/`IsDisrupting`), then runs all merges, then runs all disruptions.

This can change outcomes in interaction-heavy substeps where earlier satellite outcomes should affect later target state/route decisions.

## 2) Why This Matters

Scientific and parity impact:
- This is the highest-priority parity risk identified in the audit.
- Differences can propagate into merger classification (major/minor), event-triggered quasar/starburst physics, and downstream mass channels.
- Even if average statistics look similar, this creates deterministic branch-level divergence from SAGE in edge cases.

Project impact:
- Goal is SAGE parity inside Mimic's SAGE module stack.
- Mimic is broader than SAGE, but parity mode must still be correct and reproducible.

## 3) Architectural Guardrails (from `docs/VISION.md`)

Any fix must respect:
- Physics-agnostic core: no SAGE-specific logic hardcoded in `src/core/*` unless absolutely necessary.
- Runtime modularity: preserve configurable pipelines.
- Unified processing model and KISS: avoid complex alternate execution engines unless required.
- DRY: avoid duplicating merger/disruption transfer physics across multiple modules.

Interpretation for this work:
- Prefer a SAGE module-level solution first.
- Only change core scheduler behavior if module-level design cannot reproduce SAGE semantics.

## 4) Current Behavior Map (Relevant Code)

SAGE reference implementation:
- `sage-code/sage/core_build_model.c`
- `handle_mergers(...)` immediate per-satellite execution loop.
- `sage-code/sage/model_mergers.c`
- `deal_with_galaxy_merger(...)` around lines 107-154.
- `disrupt_satellite_to_ICS(...)` around lines 536-560.

Mimic execution path:
- `src/core/build_model.c`
- `process_halo_evolution(...)` substep loop around lines 552-604.
- `src/core/module_registry.c`
- `execute_phase(...)` full-halo modules run in module order.
- `src/modules/sage_update_merger_time.c`
- sets flags and precomputes ratio hints.
- `src/modules/sage_merge_galaxies.c`
- executes all `IsMerging` objects.
- `src/modules/sage_disrupt_satellites.c`
- executes all `IsDisrupting` objects.
- `src/modules/_shared/central_link.h`
- current target fallback logic for Type 2 satellites.

Existing tests to leverage (and gaps):
- Unit tests exist for each module, including Type 2 target behavior and mixed-dT parity:
- `src/modules/_tests/test_unit_sage_update_merger_time.c`
- `src/modules/_tests/test_unit_sage_merge_galaxies.c`
- `src/modules/_tests/test_unit_sage_disrupt_satellites.c`
- `src/modules/_tests/test_unit_mixed_dt_parity.c`
- Gap: no deterministic cross-module parity test proving SAGE-equivalent immediate ordering for multi-satellite shared-target scenarios.

## 5) Scope and Non-Goals

In scope:
- Find and quantify ordering divergences between SAGE and Mimic SAGE pipeline.
- Implement parity-preserving execution ordering for SAGE mode in Mimic.
- Add deterministic parity tests for event ordering and outcomes.

Out of scope (for this workplan):
- Rewriting non-SAGE physics models.
- Broad output-schema changes unrelated to ordering.
- Performance optimization beyond parity-safe baselines.

## 6) Investigation Questions

1. Which minimal fixtures produce deterministic SAGE vs Mimic divergence today?
2. Is divergence caused only by split-pass module sequencing, or also by target redirect behavior when targets are already consumed?
3. Can strict parity be achieved fully within SAGE modules?
4. If not, what is the smallest core scheduling change that remains physics-agnostic and low-complexity?

## 7) Workstreams

### WS1: Build a deterministic parity harness first

Deliverable:
- New parity-focused tests that compare event order and final state between SAGE reference and Mimic SAGE-mode pipeline.

Tasks:
1. Create a micro-tree fixture with at least:
- one FOF Type 0 central,
- one Type 1 subhalo central,
- two or more satellites that can both target overlapping centrals,
- one case where one satellite is disruption-eligible while another is merger-eligible in the same substep,
- one case where a Type 2 target must be redirected after an earlier same-substep target consumption.
2. Add trace capture in both codepaths for each satellite processed:
- satellite id/index,
- substep,
- resolved target index/id,
- action (`merge`/`disrupt`/`none`),
- pre/post key masses used by classification.
3. Produce a machine-diffable event log format.

Suggested locations:
- Mimic test: `src/modules/_tests/` (unit/integration parity test pair)
- Optional harness helper: `tests/framework/`

Acceptance for WS1:
- At least one fixture demonstrates current divergence reproducibly.
- Logs are stable across repeated runs.

### WS2: Validate exact parity semantics to reproduce

Deliverable:
- A one-page parity contract checked into repo docs.

Tasks:
1. Write explicit contract for SAGE immediate ordering semantics from:
- `sage-code/sage/core_build_model.c` `handle_mergers(...)`
- `sage-code/sage/model_mergers.c` merge/disrupt functions.
2. Record edge-case decisions:
- one-hop target redirection behavior if target already has `mergeType > 0`,
- ordering of state mutation relative to merger-triggered black-hole growth, starburst physics, and major-merger morphology,
- treatment of Type 2 target links after earlier in-loop mutations, including how Mimic's `CentralHalo` mapping must emulate SAGE's `CentralGal` redirect.

Suggested location:
- `docs/sage-merger-ordering-parity-contract.md`

Acceptance for WS2:
- Team agrees on explicit parity contract before code refactor lands.

### WS3: Implement module-level parity path (recommended first)

Recommended design:
- Introduce one SAGE module that performs immediate in-loop handling in one pass per substep.
- Keep core unchanged unless proven insufficient.

Tasks:
1. Add new module, e.g. `src/modules/sage_handle_mergers_immediate.c`, that in one loop:
- computes per-object dt/time (reuse `time_parity.h` helpers),
- applies eligibility checks,
- resolves target,
- executes disruption or merger immediately,
- emits per-event consumers immediately after each merger action.
2. Keep DRY by extracting shared transfer logic from existing modules into a shared helper (example):
- `src/modules/_shared/sage_merger_ops.h` (+ optional `.c` if needed),
- include reusable functions for transfer/morphology/event emission.
3. Keep existing three modules (`update_merger_time`, `merge_galaxies`, `disrupt_satellites`) for non-parity experiments; do not break them.
4. Update parity config (likely `input/millennium.yaml`) to use immediate module for SAGE parity profile.

Acceptance for WS3:
- Parity harness from WS1 matches SAGE event order and outcomes for target fixtures.
- Existing module unit tests still pass (no regressions to retained modules).

### WS4: Core-level fallback only if module-level parity fails

Trigger condition:
- WS3 cannot reproduce SAGE semantics due scheduler constraints that cannot be solved cleanly in module code.

Tasks (only if needed):
1. Propose minimal physics-agnostic scheduler enhancement in `src/core/module_registry.c`.
2. Keep feature generic and opt-in via processing mode/config, not SAGE-special-cased core code.
3. Re-run parity harness and ensure no regressions in existing pipeline behavior.

Acceptance for WS4:
- Core change is generic, documented, and justified by failing WS3 evidence.

## 8) Concrete Places to Investigate (File-by-File)

1. `sage-code/sage/core_build_model.c`
- Ground truth for immediate loop ordering and target redirection.

2. `sage-code/sage/model_mergers.c`
- Ground truth for mutation sequence inside merge/disrupt execution.

3. `src/modules/sage_update_merger_time.c`
- Currently precomputes flags for all satellites; may front-load decisions.

4. `src/modules/sage_merge_galaxies.c`
- Recomputed mass ratio at execution-time is good; verify event timing and target resolution parity.

5. `src/modules/sage_disrupt_satellites.c`
- Runs after all merges today; key suspected source of ordering divergence.

6. `src/modules/_shared/central_link.h`
- Compare fallback behavior against SAGE's one-hop `mergeType > 0` then `CentralGal` redirection semantics.

7. `src/core/module_registry.c`
- Understand hard constraints of full-halo pass ordering and event dispatch.

8. `input/millennium.yaml`
- Current SAGE profile phase order; update only after parity module is ready.

9. `src/modules/_tests/test_unit_*` and integration tests
- Extend with cross-module ordering parity tests; avoid duplicating existing isolated checks.

## 9) Acceptance Criteria (Definition of Done)

Functional parity criteria:
1. Deterministic fixtures produce identical action sequence (`merge`/`disrupt`/target) vs SAGE.
2. For parity fixtures, final key state fields match SAGE exactly or within agreed float tolerance:
- `Type`, `MergTime`, `StellarMass`, `ColdGas`, `HotGas`, `ICS`, `BulgeMass`, `BlackHoleMass`, merger timing fields.
3. Event-driven downstream effects (quasar/starburst triggers) occur at the same point in substep sequence as SAGE semantics.

Engineering criteria:
1. Core remains physics-agnostic unless WS4 trigger is met.
2. DRY: no duplicated merge/disrupt transfer code paths.
3. KISS: minimal new concepts; no parallel parity engine unless required.
4. Docs updated with parity contract and configuration guidance.

## 10) Risks and Mitigations

Risk: Hidden SAGE quirks are relied on by existing analysis.
- Mitigation: Log exact behavior first (WS1/WS2), then implement.

Risk: Over-fitting to one fixture.
- Mitigation: Build at least three fixtures: simple, shared-target, and chained-target scenarios.

Risk: Regression in non-parity modular workflows.
- Mitigation: Keep existing modules intact; add parity path instead of replacing generic behavior globally.

Risk: Core creep.
- Mitigation: WS4 gated behind explicit evidence that WS3 cannot satisfy parity.

## 11) Recommended Execution Order

1. WS1 parity harness and reproducible failing case.
2. WS2 written parity contract.
3. WS3 module-level immediate ordering implementation.
4. Update SAGE parity profile config.
5. Only then consider WS4 if needed.

## 12) Immediate Next Actions for Team Kickoff

1. Assign one owner to parity harness/logging and one owner to module refactor.
2. Create a short RFC issue capturing the WS2 parity contract before implementation lands.
3. Implement module-level immediate handling behind a clearly named SAGE parity module/config entry.
4. Land deterministic parity tests in same PR series as behavior change.
