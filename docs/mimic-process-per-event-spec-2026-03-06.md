# Mimic `process_per_event` Extension Spec (SAGE Parity + YAML Clarity)

Date: 2026-03-06  
Status: Draft for implementation planning  
Scope: Mimic core execution model + SAGE modules (`sage_*`)  
Audience: Fresh engineer/agent picking this up in a new chat

---

## 1. Why This Spec Exists

The recent P2/P3 parity work fixed correctness issues, but created a readability/architecture problem:

- Merger-triggered quasar/starburst physics is now hidden inside `sage_merge_galaxies` helper calls.
- The user-facing YAML no longer fully communicates the conceptual physics flow.
- This conflicts with the intended role of YAML as the primary model flowchart and with Mimic's modular philosophy.

Desired end state:

1. Keep strict SAGE parity for the SAGE module set.
2. Keep Mimic physics-agnostic at core level.
3. Make merger-triggered quasar/starburst explicit in YAML.
4. Preserve module independence and graceful no-op behavior where possible.
5. Avoid persistent per-galaxy queue state and cleanup complexity.

---

## 2. Current Technical Context

### 2.1 Existing processing modes

Core currently supports only:

- `process_full_halo`
- `process_by_galaxy`

Execution in each phase is fixed to:

1. all full-halo modules
2. then by-galaxy modules

This is implemented in `src/core/module_registry.c` (`execute_phase`), and enforced in parser + validation + generator tooling.

### 2.2 Why SAGE-side queue alone is insufficient for strict parity

A pure SAGE-side queue with phase_2 `quasar_mode` and `collisional_starburst` as `process_by_galaxy` consumers is readable, but not exact for multi-merger ordering.

Because full-halo modules run first, that pattern becomes:

- transfer all mergers -> BH over all events -> starburst over all events

SAGE parity for multi-mergers requires per-event sequencing:

- transfer(event1) -> BH(event1) -> starburst(event1) -> transfer(event2) -> BH(event2) -> starburst(event2)

---

## 3. Proposed Solution

Add a **third processing mode** in Mimic core:

- `process_per_event`

Design principle:

- Full-halo producer modules can emit transient events into a core-managed event list.
- Core dispatches `process_per_event` consumers over those events.
- Events are ephemeral (phase-local); no persistent queue fields required in galaxy metadata.

User YAML target:

```yaml
phase_2:
  - sage_update_merger_time:     process_full_halo
  - sage_merge_galaxies:         process_full_halo   # emits merger events; dispatch happens immediately after emission
  - sage_disrupt_satellites:     process_full_halo
  - sage_quasar_mode:            process_per_event   # consumer registration order
  - sage_collisional_starburst:  process_per_event   # consumer registration order
```

---

## 4. Core Behavioral Contract

### 4.1 Event lifecycle

- Events exist only during `execute_phase()` call.
- Events are produced by module code via a core API (`module_emit_event(...)`).
- Events are consumed by `process_per_event` modules.
- Events are dropped automatically at phase end.
- No clear module, no property reset needed.

### 4.2 Ordering contract (critical)

To preserve SAGE multi-merger semantics, event dispatch must be tied to producer progress, not deferred to end-of-phase.

Required contract:

- Partition phase modules into three ordered lists using YAML order:
  - full-halo producers
  - per-event consumers
  - by-galaxy consumers
- Run each `process_full_halo` module in normal order.
- Immediately after each full-halo module call, dispatch only the events emitted by that module to all `process_per_event` modules, in YAML order among per-event modules.
- Continue with the next full-halo module and repeat.
- Existing by-galaxy pass remains unchanged and runs only after all full-halo modules complete.

Implementation detail to make explicit:

- `execute_phase` maintains an explicit dispatch cursor (e.g., `last_dispatched_event`).
- After each full-halo module returns, dispatch event range `[last_dispatched_event, event_count)`, then set `last_dispatched_event = event_count`.
- This guarantees "dispatch only events emitted by that module call" behavior.

This allows merger events to be consumed before later full-halo steps if needed.

YAML implication to document clearly:

- Placement of `process_per_event` lines in YAML defines consumer registration order, not strict execution placement relative to other full-halo modules.
- Effective execution point is defined by producer emission sites inside full-halo execution.

Consumer event-code contract:

- Per-event consumers must default to graceful no-op for unknown `event_code` values.
- Unknown event codes are not treated as consumer errors by default.

### 4.3 Module invocation shape for `process_per_event`

- Module `process(ctx, halos, ngal)` signature remains unchanged.
- For per-event invocation:
  - `ngal = 1`
  - `halos` points to event target halo (for merger events: central halo)
  - `ctx` carries active event payload metadata.

---

## 5. Event API and Context Design (physics-agnostic)

Do not add physics-specific core fields (e.g., `MergerMassRatio` in core structs).

### 5.1 Proposed generic event struct

```c
enum ModuleEventType {
  MODULE_EVENT_TYPE_NONE = 0,
  MODULE_EVENT_TYPE_SCALAR = 1
};

struct ModuleEvent {
  enum ModuleEventType type;
  int event_code;          // producer-defined semantic code
  int source_index;        // FoFWorkspace index
  int target_index;        // FoFWorkspace index
  double value0;           // generic scalar payload
  double value1;           // optional second scalar
};
```

For SAGE merger events:

- `event_code = SAGE_EVENT_MERGER`
- `target_index = central_idx`
- `source_index = satellite_idx`
- `value0 = mass_ratio`

Ownership rule:

- `SAGE_EVENT_MERGER` is defined in SAGE module code, not core, to preserve physics-agnostic core boundaries.
- Proposed home: `src/modules/_shared/sage_events.h`.

### 5.2 Proposed `ModuleContext` additions

```c
const struct ModuleEvent *active_event; // NULL unless process_per_event invocation
```

Note:

- `active_event_index` is intentionally omitted in v1 because it is not required for correctness.
- If desired later, it can be added as debug-only instrumentation.

### 5.3 Emission API

Core-exposed function callable by modules:

```c
int module_emit_event(struct ModuleContext *ctx,
                      int event_code,
                      int source_index,
                      int target_index,
                      double value0,
                      double value1);
```

Contract:

- Returns 0 on success.
- Returns non-zero if out-of-capacity or invalid indices.
- Producer module must treat non-zero as fatal and return module failure (no warning-only downgrade).

Header placement (locked for v1):

- Declare `module_emit_event(...)` in `src/core/module_interface.h`, since module implementations include this header.
- Keep `module_registry.h` as internal registry/execution plumbing (no module-facing dependency on it).

### 5.4 Event buffer capacity

Use bounded internal buffer in first implementation for simplicity/predictability, e.g.:

- `MAX_PHASE_EVENTS = 4096` (configurable constant in core)

Overflow policy (locked for v1):

- Fail hard with clear error (deterministic correctness over silent truncation).

---

## 6. YAML and Validation Contract

### 6.1 New processing mode string

Parser accepts:

- `process_full_halo`
- `process_by_galaxy`
- `process_per_event` (new)

### 6.2 Mode support validation

Module metadata validation must understand new mode.

If module configured as `process_per_event` but does not declare support, `module_system_init` fails with clear message.

### 6.3 Same module in multiple phases/modes

Supported and expected pattern:

- The same module can be registered in multiple phases with different modes.
- Example: `sage_quasar_mode` in `phase_1` as `process_by_galaxy` (DI channel) and in `phase_2` as `process_per_event` (merger channel).
- Registration must remain phase-local and deterministic.

---

## 7. Important Note on Standalone Modules

All discussed SAGE modules are standalone `.c` modules (no per-module `module_info.yaml`).

Today, standalone support modes are auto-synthesized in tooling as:

- `process_full_halo`
- `process_by_galaxy`

This is why standalone handling is relevant, not an exception:

- If we add `process_per_event` in core but keep standalone defaults unchanged, configuring `sage_quasar_mode`/`sage_collisional_starburst` as `process_per_event` will fail validation.

Strategies considered:

1. **Loose default strategy**: standalone modules default to all 3 modes.
   - Fast, low friction.
   - Weaker mode validation fidelity.
2. **Explicit strategy**: add lightweight metadata source for standalone mode declarations.
   - More work.
   - Better validation quality.

Decision for first iteration: use loose default (all 3) to keep migration simple, then tighten later.

Additional standalone requirement:

- Any standalone module configured as `process_per_event` must handle `ctx->active_event == NULL` and unknown `event_code` defensively as graceful no-op.
- Validation permissiveness must never imply unsafe assumptions in module code paths.

---

## 8. SAGE Module Changes

### 8.1 `sage_merge_galaxies`

Current: transfer + inline BH/starburst + morphology/timing.

Target:

- Keep mass transfer, morphology, timestamp updates.
- Remove inline BH/starburst calls.
- Emit one merger event per merged satellite with mass ratio payload.
- Continue to mark merged satellite Type=3 and clear satellite merge flags.

### 8.2 `sage_quasar_mode`

Support two channels:

1. `process_by_galaxy` path:
   - Existing disk-instability behavior (`UnstableDiskGasFraction`).
2. `process_per_event` path:
   - Read `ctx->active_event`.
   - If `event_code == SAGE_EVENT_MERGER`, use `value0` (mass ratio) as efficiency factor.
   - For NULL/unknown event code, perform no-op and return success.

### 8.3 `sage_collisional_starburst`

Same dual-mode approach as quasar:

- by-galaxy: disk instability trigger.
- per-event: merger event payload.
- NULL/unknown event code: no-op and return success.

### 8.4 Trigger clear modules

- Keep `sage_clear_disk_instability_triggers` for phase_1 DI channel.
- No merger clear module required with transient core events.

### 8.5 Dual-phase registration example (explicit)

```yaml
phase_1:
  - sage_quasar_mode:            process_by_galaxy   # disk-instability channel
  - sage_collisional_starburst:  process_by_galaxy

phase_2:
  - sage_update_merger_time:     process_full_halo
  - sage_merge_galaxies:         process_full_halo
  - sage_disrupt_satellites:     process_full_halo
  - sage_quasar_mode:            process_per_event   # merger channel
  - sage_collisional_starburst:  process_per_event
```

---

## 9. File-by-File Change Plan

### Core C headers/source

- `src/core/module_interface.h`
  - add new enum mode
  - add event fields in `ModuleContext`
  - declare `module_emit_event(...)` for module-side use
  - update docs/comments

- `src/core/module_registry.h`
  - document new mode and event dispatch semantics

- `src/core/module_registry.c`
  - support mode formatting/validation strings for new mode
  - implement event buffer + emission API
  - extend `execute_phase` to dispatch per-event modules
  - implement explicit dispatch cursor logic (`last_dispatched_event`)

- `src/core/read_parameter_file.c`
  - parse `process_per_event`
  - improve error text listing all valid mode strings

### Tooling

- `scripts/generate_module_registry.py`
  - map `process_per_event` to enum
  - standalone defaults strategy update

- `scripts/validate_modules.py`
  - allow `process_per_event` in `supported_processing_modes`

### SAGE modules

- `src/modules/_shared/sage_events.h`
  - define SAGE-owned event codes (e.g., `SAGE_EVENT_MERGER`)

- `src/modules/sage_merge_galaxies.c`
  - emit events; remove inline merger BH/starburst calls

- `src/modules/sage_quasar_mode.c`
  - add per-event consumer path

- `src/modules/sage_collisional_starburst.c`
  - add per-event consumer path

- `input/millennium.yaml`
  - set phase_2 quasar/starburst to `process_per_event`

### Docs

- `docs/DEVELOPER-GUIDE.md`
- `docs/USER-GUIDE.md`
- document YAML ordering semantics for `process_per_event` (consumer registration order vs producer-triggered dispatch timing)
- optionally append amendment note in parity plan doc

---

## 10. Suggested Implementation Sequence

1. Add enum + parser + tooling support for `process_per_event`.
2. Add core event API + internal event buffer.
3. Implement event dispatch in `execute_phase` with deterministic ordering.
4. Update SAGE modules to producer/consumer model.
5. Update YAML and docs.
6. Update/add unit tests for:
   - parser/validation acceptance
   - per-event dispatch ordering
   - dual-mode module behavior
   - multi-merger sequential parity behavior
   - no-events-emitted phase no-op behavior

---

## 11. Testing Plan (minimum)

### Core tests

- YAML parse accepts `process_per_event`.
- Invalid mode strings still fail with clear message.
- Module mode mismatch produces actionable error.
- Per-event dispatch interleaves immediately after producer execution, even when later full-halo modules appear before per-event lines in YAML.
- Event overflow policy is hard-fail.
- Phase with configured `process_per_event` consumers and zero emitted events completes as no-op.

### SAGE tests

- Multi-merger substep case validates per-event sequential outcomes (ColdGas depletion order-sensitive).
- `sage_quasar_mode` in by-galaxy mode remains DI-only and unchanged.
- `sage_quasar_mode` in per-event mode responds only to merger event code.
- Same for `sage_collisional_starburst`.
- `sage_quasar_mode`/`sage_collisional_starburst` configured across `phase_1` and `phase_2` with different modes behaves correctly.
- Unexpected event producers do not break consumers; unknown event codes are ignored as no-op.
- Existing P2/P3 acceptance tests remain green.

---

## 12. Risks and Mitigations

1. **Risk**: event dispatch ordering subtle regression.
- Mitigation: explicit ordering tests with known two-merger fixtures.

2. **Risk**: mode handling in standalone module metadata becomes too permissive.
- Mitigation: document temporary permissive strategy; tighten in follow-up.

3. **Risk**: event API leaks physics assumptions.
- Mitigation: keep payload generic and event-code-based.

4. **Risk**: confusion between per-event and by-galaxy channels in shared modules.
- Mitigation: explicit code-path guards and comments + tests for each mode.

---

## 13. Decisions Locked For v1

1. Event buffer policy: hard fail (no truncation, no warning-only downgrade).
2. Standalone mode strategy: permissive all-modes in generation for v1; tighten later.
3. Quasar/starburst design: dual-mode modules (do not split into DI/merger duplicates).
4. Event payload: keep minimal generic payload (`value0/value1`) in v1.

---

## 14. Quick Start Checklist For Next Chat

- Implement mode + parser + tooling first.
- Implement core event API and dispatch with tests before touching SAGE physics modules.
- Then migrate SAGE modules and YAML.
- Re-run parity-focused unit/integration tests and compare multi-merger behavior, including no-events and dual-phase registration cases.
