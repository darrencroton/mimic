# Event System Recommendation for Mimic

## Objective

Reassess Mimic's event-system direction against two baseline goals:

1. The event system should be genuinely general core infrastructure, not merger-shaped infrastructure that happens to work for mergers.
2. The system should stay as simple, clean, and transparent as possible for model builders.

This report compares the main design options, evaluates them against `docs/VISION.md`, KISS, DRY, and Mimic's actual workflow, and recommends a new direction for team review.

## Scope

Included:

- Current event dispatch and event data structures
- Current YAML pipeline parsing and validation
- Current SAGE producer/consumer usage
- Current module metadata and generation model
- Current run-metadata / reproducibility behavior
- Design options for generalising the event system

Excluded:

- Implementation work
- Performance benchmarking
- Non-event module pipeline redesign

## Context

Mimic already has the skeleton of a generic event system:

- The core does not know merger physics directly; it carries `event_code`, source/target indices, and scalar payload values in `struct ModuleEvent` in `src/core/module_interface.h:158-165`.
- Event dispatch is phase-local and synchronous in `src/core/module_registry.c:462-579`.
- Producers emit during `process_full_halo`; consumers run in `process_per_event`; by-galaxy modules remain separate.

That is a good starting point. The problem is not that the core is full of merger-specific code. The problem is that the first real use case was mergers, so the current event behavior assumes a very narrow operating regime:

- every `process_per_event` consumer receives every event in `src/core/module_registry.c:486-515`
- there is no producer-level namespacing of event identity
- the payload contract is very small and informal
- the runtime configuration and output metadata do not expose event wiring clearly

So the real design question is not "how do we bolt arbitrary payloads onto a merger system?" It is:

How do we turn the existing event skeleton into a stable, model-builder-friendly, phase-local trigger mechanism for Mimic?

## Comparison Criteria

The options below are judged against these criteria:

1. Generality
   Can the system serve mergers and future non-merger triggers without hidden assumptions?
2. Simplicity
   Does the model stay easy to explain, debug, and use?
3. Vision alignment
   Does it fit `docs/VISION.md`, especially Principles 1, 3, 4, 5, and 8?
4. DRY / single source of truth
   Does it avoid duplicating event contracts across C, YAML, docs, and output metadata?
5. Runtime transparency
   Can a user inspect a run configuration and understand which modules emit and consume which events?
6. Scientific safety
   Does it reduce the chance of silent wrong-physics behavior?
7. Implementation proportionality
   Is the complexity of the mechanism proportional to Mimic's current and near-term needs?

## Method

This assessment is based on static inspection of the current tree:

- Proposal document: `docs/EVENT-SYSTEM-IMPROVEMENTS.md`
- Vision and module architecture docs: `docs/VISION.md`, `docs/DEVELOPER-GUIDE.md`, `docs/USER-GUIDE.md`
- Core event implementation: `src/core/module_interface.h`, `src/core/module_registry.c`, `src/core/read_parameter_file.c`
- Current SAGE event producer/consumer modules
- Module metadata and generator pipeline: `module_info.yaml` usage and `scripts/generate_module_registry.py`
- Current HDF5 run-configuration metadata in `src/io/output/hdf5.c`

No code was modified and no tests were run for this report.

## Evidence

- The current dispatcher broadcasts every event to every per-event consumer in `src/core/module_registry.c:486-515`.
- The current parser only accepts `module_name: processing_mode` entries in `src/core/read_parameter_file.c:440-493`.
- `struct ModuleEvent` currently carries only `value0` and `value1` in `src/core/module_interface.h:158-165`.
- Current SAGE consumers still protect themselves with in-module `event_code` checks in:
  - `src/modules/sage_quasar_mode/sage_quasar_mode.c:81-100`
  - `src/modules/sage_starburst_feedback/sage_starburst_feedback.c:232-264`
- The current module system is strongly metadata-driven for module capabilities:
  - `docs/VISION.md:51-62`
  - `docs/DEVELOPER-GUIDE.md:1233-1258`
  - `scripts/generate_module_registry.py:274-316`
- The YAML file is currently intended to define phase structure and processing mode, not full module behavior:
  - `src/include/types.h:111-134`
- HDF5 output currently records enabled modules with phase and processing mode only in `src/io/output/hdf5.c:654-763`, and the user guide describes this as preserving the complete pipeline in `docs/USER-GUIDE.md:348-357`.

## Options

### Option A: Adopt the existing proposal largely as written

Summary:

- add YAML event-code filters for `process_per_event`
- keep event codes as raw integers
- add an opaque `void *` payload plus `payload_size`
- keep the current immediate synchronous dispatch model

Representative shape:

```yaml
phase_2:
  - producer: process_full_halo
  - consumer:
      mode: process_per_event
      events: [1]
```

### Option B: Minimal routed events in YAML, producer-scoped, no arbitrary payload

Summary:

- keep event contracts runtime-configurable in YAML
- make event identity producer-scoped, not just `event_code`
- keep payload bounded and numeric
- do not add arbitrary pointer payloads

Representative shape:

```yaml
phase_2:
  - sage_resolve_mergers_and_disruption: process_full_halo
  - sage_quasar_mode:
      mode: process_per_event
      subscriptions:
        - producer: sage_resolve_mergers_and_disruption
          event_codes: [1]
```

### Option C: Module-owned event contracts with producer-scoped routing and minimal payload

Summary:

- keep phase placement and processing modes in input YAML
- move event contracts into module metadata
- make event identity producer-scoped
- route automatically from generated module metadata
- keep payload small and numeric for now
- explicitly defer arbitrary payload pointers unless a real use case forces them

Representative shape:

`module_info.yaml` for a producer:

```yaml
module:
  name: sage_resolve_mergers_and_disruption
  supported_processing_modes: [process_full_halo]
  events:
    emits:
      - name: merger
```

`module_info.yaml` for a consumer:

```yaml
module:
  name: sage_quasar_mode
  supported_processing_modes: [process_by_galaxy, process_per_event]
  events:
    consumes:
      - producer: sage_resolve_mergers_and_disruption
        event: merger
```

Pipeline YAML remains focused on pipeline structure:

```yaml
phase_2:
  - sage_resolve_mergers_and_disruption: process_full_halo
  - sage_quasar_mode: process_per_event
  - sage_starburst_feedback: process_per_event
```

### Option D: Routing only, defer all other changes

Summary:

- add filtered routing only
- keep current two-scalar payload unchanged
- keep consumer-side event checks
- defer both metadata and payload work

## Findings

### 1. The existing proposal solves a real problem, but not the right problem completely

Its strongest idea is filtered routing. The current broadcast dispatcher is not sustainable as Mimic grows. It creates wasted work and, more importantly, a silent wrong-physics failure mode if a consumer forgets an `event_code` check.

That part should change.

Its weakest idea is the opaque pointer payload. That solves a theoretical flexibility problem by introducing a fragile lifetime rule into core infrastructure. The producer must know that dispatch is immediate forever, stack-backed payloads must stay live only for the consumer call, and future changes to dispatch timing become much harder. That is not a good trade for a first generalisation.

### 2. Raw integer event lists in YAML are not a good long-term user interface

They are explicit, but not clean:

- they duplicate module-owned event knowledge already present in C headers
- they are not self-explanatory to users reviewing a run file
- they weaken DRY and Principle 4
- they make the YAML file responsible for detailed physics contracts rather than pipeline structure

This is especially important in Mimic because the codebase already uses module metadata as a first-class mechanism for module behavior and validation.

### 3. The current system already suggests a better division of responsibility

Today, Mimic splits responsibilities like this:

- input YAML chooses phase structure and processing mode
- `module_info.yaml` declares module capabilities
- generated code turns metadata into runtime validation

That pattern is already established and documented in:

- `docs/VISION.md:51-62`
- `docs/DEVELOPER-GUIDE.md:1233-1258`
- `scripts/generate_module_registry.py:274-316`

Event contracts fit more naturally into this module-owned metadata layer than into the run YAML.

### 4. The event system should stay narrow in purpose

The safest generalisation is not "events can carry anything."

The safest generalisation is:

Events are sparse, phase-local, targeted triggers that carry a small amount of numeric context from one module to another.

That is broad enough for mergers and many future uses, but narrow enough to remain understandable and testable. It also preserves Mimic's clean processing model in `docs/VISION.md:86-91`.

### 5. Producer-scoped identity is required for true multi-model use

Bare integer codes are not enough once Mimic hosts multiple physics families. A collision like "event code 1" in two different producers is too easy to create.

The core should treat event identity as at least:

- producer module
- event label or code

Without that, the system is still implicitly tailored to a one-producer / one-event-family world.

### 6. Output metadata must be part of the design, not an afterthought

If event subscriptions become part of runtime behavior, the run record must capture them. Current HDF5 metadata records only:

- module name
- phase
- processing mode

in `src/io/output/hdf5.c:654-763`.

Any final design must either:

- keep event contracts module-owned and reproducible from module metadata plus enabled modules, or
- extend output metadata with explicit event wiring

Otherwise Mimic's reproducibility story becomes weaker, not stronger.

## Tradeoffs

### Option A tradeoffs

Pros:

- Minimal generator impact
- Straightforward to implement from the current proposal
- Backward compatible for existing pipelines

Cons:

- YAML becomes a second source of truth for physics event contracts
- raw integer filters are hard to read and easy to drift
- pointer payloads are fragile and subtle
- still weak on producer namespacing unless extended further

Assessment:

Good incremental patch, not the best long-term design.

### Option B tradeoffs

Pros:

- Simpler and safer than Option A
- fixes the main correctness problem
- avoids pointer-lifetime fragility
- preserves runtime configurability

Cons:

- still duplicates event contracts in YAML
- still shifts too much module behavior into run configuration
- still needs expanded output metadata for reproducibility

Assessment:

A viable fallback if the team wants the smallest possible implementation step, but not the cleanest architectural destination.

### Option C tradeoffs

Pros:

- Best fit to Mimic's metadata-driven architecture
- Clearest single source of truth for module event behavior
- Keeps input YAML focused on phase structure and module placement
- Best DRY outcome
- Best user-facing transparency once documented and generated
- Allows strong validation at startup

Cons:

- Requires generator and metadata schema changes
- Slightly larger first implementation than pure YAML filtering
- Less runtime rewiring flexibility for consumers unless overrides are added later

Assessment:

Best architectural fit if the goal is an event system that remains clean after more than one model uses it.

### Option D tradeoffs

Pros:

- Lowest implementation cost
- Strong KISS discipline

Cons:

- Leaves too much implicit behavior in consumer C code
- does not solve namespacing or transparency well
- likely postpones the same design question rather than resolving it

Assessment:

Too small to be the right stopping point.

## Decision

Recommend Option C as the target design, with one important constraint:

Do not adopt arbitrary opaque payload pointers in the first generalised version.

Instead:

1. Generalise routing and event identity first.
2. Make event contracts module-owned and metadata-driven.
3. Keep payloads small and numeric in the first version.
4. Revisit payload expansion only when a concrete second use case demonstrates the need.

## Decision Rationale

Option C is the best match to the baseline goals.

It makes the event system genuinely general without making it conceptually broad. The event mechanism becomes a reusable core trigger system, not a merger patch with more knobs.

It also best matches Mimic's architecture:

- Principle 1: the core still knows nothing about physics semantics; it only routes generic event identities
- Principle 3: module-owned event contracts become metadata, which is exactly how Mimic already treats module capabilities
- Principle 4: event behavior is no longer split awkwardly between C and YAML integers
- Principle 5: the current processing model stays intact
- Principle 8: startup validation can become stronger because contracts are explicit and generated

Most importantly, it is the cleanest interface for model builders:

- write a producer module
- declare which events it emits
- write a consumer module
- declare which events it consumes
- place both modules in a phase using the existing YAML structure

That is easier to explain than telling users to wire raw event codes into run YAML and safer than telling them to pass opaque stack-backed payload pointers through core infrastructure.

## Recommended Design Outline

### A. Keep the current execution model

Retain all of the following:

- phase-local event buffering
- immediate synchronous dispatch
- `process_full_halo` producers
- `process_per_event` consumers
- target-halo dispatch semantics

This part already aligns well with Mimic's unified processing model.

### B. Make event identity producer-scoped

Add producer identity to `struct ModuleEvent`, with numeric event IDs generated
from metadata names, for example:

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

The important design rule is not the exact field names. The important rule is
that event identity is producer-scoped, and that human-authored metadata uses
names while generated code provides compact numeric IDs for dispatch.

### C. Add optional event metadata to `module_info.yaml`

Extend module metadata with optional event sections:

- `events.emits`
- `events.consumes`

The core remains generic. The generator turns these declarations into runtime tables for validation and routing.

This keeps phase placement in YAML and module behavior in module metadata, which
is consistent with the rest of Mimic.

### D. Route automatically from generated contracts

At startup:

- validate that consumed events correspond to emitted events from enabled modules in the same phase
- validate that consumer mode is `process_per_event`
- warn or fail on ambiguous or impossible contracts

At dispatch:

- route only to consumers whose generated subscriptions match the emitted `(producer, event)` pair

This removes the need for most consumer-side "ignore unrelated event" boilerplate.

### E. Keep payload expansion conservative

For the first generalised version:

- retain `value0` and `value1`, or at most widen to a small fixed numeric array later
- do not add `void *payload`
- do not build lifetime-sensitive borrowed-pointer semantics into the core

If a future model genuinely needs more than two scalar values, the next simplest step is a bounded numeric payload, for example:

```c
#define MODULE_EVENT_MAX_VALUES 4

struct ModuleEvent {
  const char *producer_name;
  int event_code;
  int source_index;
  int target_index;
  int num_values;
  double values[MODULE_EVENT_MAX_VALUES];
};
```

That remains predictable, serialisable, and easy to test. It is also much easier to explain than arbitrary pointer payloads.

### F. Preserve reproducibility

If event contracts become active runtime behavior, add corresponding output metadata.

Two acceptable paths:

1. Derive event contracts entirely from generated module metadata plus enabled modules.
2. Write an explicit `EventContracts` dataset into run metadata.

Either path is acceptable. Ignoring the metadata problem is not.

## Recommended Implementation Sequence

### Stage 1: Generalise routing and contracts

- Add producer identity to emitted events
- Extend `module_info.yaml` schema with optional emitted/consumed event declarations
- Extend `scripts/generate_module_registry.py` to validate and generate event-contract tables
- Route `process_per_event` consumers from generated contracts
- Update docs and run metadata
- Convert current SAGE merger usage to the generic contract

This stage delivers the actual architectural win.

### Stage 2: Reassess payload shape only if needed

Only after Stage 1 lands and at least one non-merger use case exists:

- decide whether two scalars are still enough
- if not, widen to a small fixed numeric payload

Do not implement pointer payloads by default.

## Risks / Unknowns

- Adding event fields to module metadata will require generator, schema, and documentation updates.
- Some teams may want YAML-level overrides for event subscriptions. That should
  be resisted initially unless a concrete use case appears.
- If a future use case truly needs non-numeric or large payloads, the bounded numeric payload model may need revision.
- Generated numeric event IDs should be treated as implementation detail unless a
  future external interface needs stable human-authored codes.

## Recommended Next Actions

1. Approve or reject the high-level direction in this report:
   module-owned event contracts, producer-scoped routing, no pointer payloads in v1.
2. Decide whether emitted events in metadata should be represented by:
   - names only, or
   - names plus numeric codes
3. Update `docs/EVENT-SYSTEM-IMPROVEMENTS.md` or replace it with an implementation plan aligned to this report.
4. Include run-metadata changes explicitly in the implementation plan.
5. Add a small pair of synthetic event-test modules so routing can be tested without coupling infrastructure tests to SAGE-specific physics.

## Completion Status

Complete for the requested scope.

This report re-evaluates the event-system direction from first principles, compares the main design choices, and recommends a new architecture that better matches Mimic's vision, KISS, DRY, and model-builder ergonomics.
