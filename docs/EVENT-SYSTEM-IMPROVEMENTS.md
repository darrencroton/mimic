# Event System Implementation Plan

## Objective

Provide an implementation-ready plan for generalising Mimic's event system so it:

1. serves as clean core infrastructure rather than merger-shaped infrastructure
2. remains simple and transparent for model builders
3. supports the current SAGE merger pathway as the first user of the generalised system
4. leaves room for future non-merger event uses without redesigning the core again

This plan supersedes the earlier proposal-oriented contents of this file.

## Scope

Included:

- event identity, contracts, routing, and validation
- module metadata and generator changes
- core runtime changes
- migration of the current SAGE merger event chain
- output metadata
- documentation and testing requirements

Excluded:

- arbitrary pointer payloads
- asynchronous or deferred event dispatch
- non-event pipeline redesign
- cross-phase event persistence

## Context

Mimic already has a useful event-system skeleton:

- producers emit during `process_full_halo`
- consumers receive targeted halos during `process_per_event`
- dispatch is phase-local and synchronous

The current implementation is still too narrow for long-term reuse because:

- every `process_per_event` consumer receives every event
- event identity is effectively just a raw integer
- event wiring is not visible in module metadata or run metadata
- consumer modules still rely on defensive manual filtering in C

The goal is not to create a fully generic message bus. The goal is to define a
small, stable, scientific-computing event mechanism that fits Mimic's
architecture and can be reused by different galaxy models.

## Problem Statement

The current event system is under-generalised in the wrong places and
over-flexible in the wrong places.

Under-generalised:

- event routing is broadcast-based rather than contract-based
- event identity is not producer-scoped
- event behavior is not represented in module metadata
- run metadata does not preserve event wiring explicitly

Over-flexible in the previous proposal:

- arbitrary opaque payload pointers add lifetime and ownership complexity before
  Mimic has a demonstrated need for them

Mimic needs a simpler and more architectural solution:

- human-authored event contracts belong in module metadata
- runtime dispatch should use resolved machine-friendly identifiers
- the current merger pathway should become the first clean instance of the
  general system

## Goals

- Generalise the event system without changing Mimic's high-level execution model.
- Keep input YAML focused on phase structure and processing mode.
- Move event contracts into `module_info.yaml` so they become metadata-driven.
- Make event identity producer-scoped.
- Route only to subscribed consumers.
- Keep the first generalised payload model small and numeric.
- Preserve or improve reproducibility via run metadata.
- Give a fresh implementation team a concrete file-by-file path.

## Non-Goals

- No wildcard or broadcast consumer mode in the first generalised version.
- No arbitrary `void *` or borrowed-pointer payloads.
- No event emission from `process_per_event` or `process_by_galaxy` in v1.
- No attempt to solve every hypothetical future event shape now.
- No changes to the existing phase model (`pre_timestep`, `phase_1`, `phase_2`,
  `post_timestep`).

## Constraints

- The design must align with `docs/VISION.md`, especially Principles 1, 3, 4, 5, and 8.
- The design should follow KISS and DRY.
- The input YAML parser should remain unchanged if possible.
- Event dispatch should remain phase-local and synchronous.
- Existing SAGE merger behavior must be preserved after migration.
- Output metadata should remain self-describing and reproducible.

## Method

This plan is based on review of:

- current event dispatch and phase execution code
- current module metadata / generator patterns
- current SAGE merger event users
- current HDF5 run metadata design
- the approved architectural direction in `docs/EVENT-SYSTEM-RECOMMENDATION.md`

## Evidence

- Current broadcast dispatch: `src/core/module_registry.c:486-515`
- Current event payload shape: `src/core/module_interface.h:158-165`
- Current phase parser shape: `src/core/read_parameter_file.c:440-493`
- Current metadata-driven module architecture:
  - `docs/VISION.md:51-62`
  - `docs/DEVELOPER-GUIDE.md:1233-1258`
  - `scripts/generate_module_registry.py:274-316`
- Current run metadata dataset:
  - `src/io/output/hdf5.c:654-763`
  - `docs/USER-GUIDE.md:348-357`

## Options Considered

### Option A: YAML routing plus pointer payloads

Rejected as the target design.

Why rejected:

- moves too much module behavior into runtime YAML
- duplicates event contracts across metadata and YAML
- introduces fragile payload lifetime rules too early

### Option B: YAML routing only

Rejected as the target design.

Why rejected:

- fixes routing, but leaves too much implicit event behavior in C
- does not make event contracts a metadata-level concern
- is weaker on DRY and long-term transparency

### Option C: Metadata-owned contracts, producer-scoped routing, conservative payloads

Selected.

Why selected:

- best fit to Mimic's metadata-driven architecture
- cleanest separation between pipeline structure and module behavior
- easiest model-builder story
- strongest DRY outcome
- avoids over-designing payloads before needed

## Recommended Approach

### 1. Keep the current execution model

Retain:

- `process_full_halo` as the only event producer mode in v1
- `process_per_event` as the event consumer mode
- phase-local event buffering
- immediate synchronous dispatch
- target-halo delivery

This preserves Mimic's unified processing model and avoids unnecessary
architectural churn.

### 2. Make authored event contracts metadata-driven

Add optional event sections to `module_info.yaml`.

Producer metadata:

```yaml
module:
  name: sage_resolve_mergers_and_disruption
  supported_processing_modes: [process_full_halo]
  events:
    emits:
      - name: merger
        description: "Emitted after live-target merger transfer completes"
```

Consumer metadata:

```yaml
module:
  name: sage_quasar_mode
  supported_processing_modes: [process_by_galaxy, process_per_event]
  events:
    consumes:
      - producer: sage_resolve_mergers_and_disruption
        event: merger
```

Author-facing rule:

- humans write event names in metadata
- humans do not hand-author numeric event IDs in v1

### 3. Generate numeric IDs for runtime and C code

Numeric IDs are still useful internally, but they should be generated rather
than manually duplicated.

Generated artifacts should provide:

- per-producer event ID enums for module C code
- lookup tables for emitted events
- lookup tables for consumer subscriptions

Recommended generated interface:

```c
enum SageResolveMergersAndDisruptionEventId {
  SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER = 1
};
```

Producer code then emits by generated constant rather than by handwritten raw
integer:

```c
module_emit_event(ctx, SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                  source_index, target_index, value0, value1);
```

This gives the runtime compact numeric identifiers without making model builders
manage those IDs manually.

### 4. Make event identity producer-scoped

The core should treat event identity as:

- producer module
- event ID within that producer

Recommended runtime shape:

```c
struct ModuleEvent {
  int producer_module_id;
  int event_id;
  int source_index;
  int target_index;
  double value0;
  double value1;
};
```

Equivalent representations are acceptable if they preserve these semantics and
keep hot-path dispatch numeric rather than string-based.

### 5. Route from resolved subscriptions, not consumer-side filtering

At startup:

- resolve each configured `process_per_event` module's subscriptions from its
  metadata declarations
- validate that the referenced producer is enabled in the same phase
- validate that the referenced event exists on that producer
- validate that the consumer is configured in `process_per_event`

At dispatch:

- only call consumer modules whose resolved `(producer_module_id, event_id)`
  subscription matches the emitted event

Design rule:

- no implicit broadcast delivery in v1
- if a module is configured as `process_per_event`, it must declare
  `events.consumes`

This is stricter than the current system and is intentional. It improves
scientific safety and transparency.

### 6. Keep the first payload model conservative

For v1, retain the existing two-scalar payload shape:

- `value0`
- `value1`

Do not add:

- `void *payload`
- borrowed payload ownership rules
- deep-copy payload infrastructure

If a future use case needs more numeric context, the first expansion path should
be a small fixed numeric payload, not a pointer payload.

### 7. Preserve reproducibility explicitly

Add an `EventContracts` dataset to HDF5 run metadata under `RunProperties`.

Recommended fields:

- `phase`
- `consumer_module`
- `producer_module`
- `event_name`
- `event_id`

This keeps the run self-describing even when event contracts live in metadata
and are resolved at startup.

## Implementation Phases

### Phase 0: Finalise representation choices

Deliverables:

- approve metadata schema
- approve generated-ID approach
- approve `EventContracts` output metadata

Decisions for this plan:

- event names are the authored source of truth
- numeric event IDs are generated
- no parser-level event filters in input YAML
- no pointer payloads in v1

### Phase 1: Extend module metadata schema and generator

Files:

- `scripts/generate_module_registry.py`
- `src/modules/_system/template/template_module_info.yaml`
- `docs/DEVELOPER-GUIDE.md`

Work:

1. Extend module metadata parsing to support optional:
   - `events.emits`
   - `events.consumes`
2. Add generator validation rules:
   - emitted event names must be unique within a producer
   - consumed `(producer, event)` pairs must be unique within a consumer
   - `events.emits` only valid for modules that support `process_full_halo`
   - `events.consumes` only valid for modules that support `process_per_event`
3. Generate:
   - public event ID enums for module C code
   - emitted-event lookup tables
   - consumer subscription tables
4. Update the module metadata template and schema docs.

Recommended output artifacts:

- public generated header for event IDs
- generated C tables for runtime resolution

### Phase 2: Extend core runtime contracts

Files:

- `src/core/module_interface.h`
- `src/core/module_registry.h`
- `src/core/module_registry.c`

Work:

1. Update `struct ModuleEvent` to carry producer-scoped identity.
2. Add runtime-resolved subscription storage to `PhaseModuleConfig` or an
   equivalent phase-local runtime structure.
3. During `module_system_init()`:
   - resolve subscriptions for configured per-event consumers
   - validate producer presence and phase matching
   - validate event existence
4. Update dispatch to match only resolved subscriptions.
5. Keep `module_emit_event()` as the producer API, but document that the event
   argument is now a generated per-producer event ID.
6. Update cleanup to free any resolved subscription arrays.

Important design note:

- `src/core/read_parameter_file.c` should remain unchanged in v1 because phase
  YAML structure is intentionally unchanged.

### Phase 3: Migrate the SAGE merger chain onto the generic contracts

Files:

- `src/modules/sage_resolve_mergers_and_disruption/module_info.yaml`
- `src/modules/sage_quasar_mode/module_info.yaml`
- `src/modules/sage_starburst_feedback/module_info.yaml`
- producer/consumer C files as needed

Work:

1. Declare the merger event in producer metadata.
2. Declare consumer subscriptions in quasar and starburst metadata.
3. Replace hand-maintained shared event-code headers with generated event IDs if
   possible.
4. Keep consumer-side defensive checks temporarily if useful during migration,
   but treat routing as the primary filter.
5. Confirm the resulting behavior matches the current merger pathway.

### Phase 4: Add explicit run metadata for event contracts

Files:

- `src/io/output/hdf5.c`
- `docs/USER-GUIDE.md`

Work:

1. Add `RunProperties/EventContracts`.
2. Populate it from the resolved startup contracts.
3. Update the user guide's HDF5 structure documentation.
4. Keep `EnabledModules` unchanged; `EventContracts` is additive.

### Phase 5: Strengthen tests with dedicated synthetic event modules

Recommendation:

- do not overload `src/modules/_system/test_fixture/`
- keep `test_fixture` focused on generic pipeline execution behavior
- add dedicated sibling test-only modules under `src/modules/_system/`

Recommended modules:

- `src/modules/_system/test_event_producer/`
- `src/modules/_system/test_event_consumer_alpha/`
- `src/modules/_system/test_event_consumer_beta/`

Why sibling modules rather than folding into `test_fixture`:

- clearer single responsibility
- cleaner metadata contracts
- easier targeted integration tests
- avoids making `test_fixture` harder to reason about

Required tests:

1. Generator/schema validation tests
   - duplicate emitted names fail
   - invalid consumed producer/event fail
   - invalid mode/event combinations fail
2. Core unit tests
   - resolved subscription matching
   - no delivery to unsubscribed consumers
   - phase-local validation failures
   - cleanup frees new allocations
3. Synthetic integration tests
   - one producer, two consumers, different subscriptions
   - multiple events from one producer
   - multiple producers in one phase
   - invalid configuration fails fast with clear errors
4. SAGE regression tests
   - current merger event integration tests still pass
5. Metadata tests
   - `EventContracts` appears in HDF5 output and matches configuration

### Phase 6: Documentation deliverables

Files:

- `docs/DEVELOPER-GUIDE.md`
- `docs/USER-GUIDE.md`
- `src/modules/_system/template/template_module_info.yaml`

Work:

1. Update developer docs to explain:
   - how producers declare emitted events
   - how consumers declare subscriptions
   - how generated event IDs are used in C
   - that input YAML does not carry event filters
2. Update user docs with:
   - event-capable module examples
   - HDF5 `EventContracts` metadata
3. Update the module template so new modules see the pattern immediately.

Required developer-doc note:

Add a short section describing the approved future payload-expansion path so a
future developer does not have to guess. That section should say:

- the default event payload in v1 is two scalar values
- if more payload is needed, first widen to a small fixed numeric payload
- do not introduce pointer payloads without a separate design review
- any payload expansion must update:
  - `struct ModuleEvent`
  - emit helpers
  - zero-initialization / cleanup assumptions
  - tests
  - output metadata if affected
  - documentation examples

That note should be brief in the guide, but explicit enough to implement without
guessing.

## Validation Plan

Success criteria:

1. No phase YAML syntax changes are required for normal use.
2. The SAGE merger chain uses the new generalised contracts successfully.
3. A synthetic non-SAGE event test passes.
4. Unsubscribed consumers are never called.
5. Invalid event contracts fail at startup with clear errors.
6. HDF5 output records resolved event contracts.
7. Developer docs explain both normal usage and the approved payload-expansion
   path.

Suggested verification order:

1. generator validation tests
2. core unit tests
3. synthetic integration tests
4. SAGE regression tests
5. output metadata checks
6. docs consistency checks

## Risks / Unknowns

- Generator changes touch a core architectural seam and should be reviewed
  carefully.
- The team must decide exactly where the generated event-ID header and runtime
  tables should live.
- Temporary coexistence of generated IDs and old hand-written event-code headers
  during migration may create short-lived duplication.
- If a real future use case exceeds a small numeric payload, a second design pass
  will still be needed.

## Open Questions

Non-blocking:

- Exact filenames and directories for generated event-ID headers and runtime
  contract tables.
- Whether consumer C modules should keep optional defensive event assertions
  after routing is in place, or remove them entirely once coverage is strong.

Blocking:

- None, if the approved direction in this plan is accepted.

## Recommended Next Actions

1. Treat this file as the implementation plan of record.
2. Start with Phase 1 and Phase 2 before touching SAGE modules.
3. Add dedicated synthetic event test modules under `src/modules/_system/`.
4. Add `EventContracts` metadata in the same implementation cycle as routing so
   reproducibility does not lag behind behavior.
5. Update `docs/DEVELOPER-GUIDE.md` only as part of the implementation cycle so
   the guide reflects shipped behavior, not speculative behavior.

## Completion Status

Complete for the requested scope.

This file now provides the approved design direction and a concrete staged plan
that a fresh team can use to implement the new event system.
