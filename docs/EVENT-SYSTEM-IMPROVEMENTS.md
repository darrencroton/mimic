# Event System Improvements: Routing and Flexible Payloads

**Purpose**: Design improvements to Mimic's event system so that multiple physics
models — not just SAGE — can define, emit, and consume diverse event types without
modifying core infrastructure.

**Scope**: Core event dispatch (`module_registry.c`), event payload structure
(`module_interface.h`), module metadata (`module_info.yaml`), and YAML pipeline
configuration.

**Guiding Principles**: All changes must satisfy the 8 core architectural
principles in `docs/VISION.md`. The two most relevant are:

- **Principle 1 (Physics-Agnostic Core)**: The core must never know specific event
  codes or payload semantics. Event codes remain module-defined integers.
- **Principle 3 (Metadata-Driven Architecture)**: New capabilities should be
  expressible through metadata and YAML configuration, not hardcoded logic.

---

## 1. Current Event System (Baseline)

### 1.1 Architecture Overview

The event system enables cross-galaxy communication during phase execution.
It has three roles:

| Role | Processing Mode | Description |
|------|----------------|-------------|
| **Producer** | `process_full_halo` | Iterates the full FOF halo array, emits events via `module_emit_event()` |
| **Consumer** | `process_per_event` | Receives one event at a time; core sets `ctx->active_event` and passes the target halo |
| **Bystander** | `process_by_galaxy` | Runs after all events are dispatched; sees `ctx->active_event == NULL` |

### 1.2 Key Files

| File | Role |
|------|------|
| `src/core/module_interface.h` | Defines `struct ModuleEvent`, `enum ModuleEventType`, `module_emit_event()` signature |
| `src/core/module_registry.c` | Implements event buffering, dispatch loop, emission guards |
| `src/core/module_registry.h` | Defines `struct PhaseModuleConfig` (module name + processing mode) |
| `src/modules/_shared/sage_events.h` | SAGE-specific event codes (`SAGE_EVENT_MERGER = 1`) |

### 1.3 Data Structures

```c
/* module_interface.h */
enum ModuleEventType {
  MODULE_EVENT_TYPE_NONE = 0,
  MODULE_EVENT_TYPE_SCALAR = 1
};

struct ModuleEvent {
  enum ModuleEventType type;   /* Payload encoding */
  int event_code;              /* Producer-defined semantic code */
  int source_index;            /* Source halo index in FoFWorkspace */
  int target_index;            /* Target halo index in FoFWorkspace */
  double value0;               /* Primary scalar payload */
  double value1;               /* Secondary scalar payload */
};
```

Phase-local buffering (`module_registry.c`):

```c
#define MAX_PHASE_EVENTS 4096

struct PhaseEventDispatchState {
  struct ModuleEvent events[MAX_PHASE_EVENTS];
  int event_count;
  int last_dispatched_event;
  struct PhaseModuleConfig *phase_config;
  int num_modules;
  struct ModuleContext *ctx;
  struct Halo *halos;
  int ngal;
  bool active;
  bool emission_allowed;
};
```

### 1.4 Dispatch Flow

Within `execute_phase()`, processing happens in three passes:

1. **Pass 1 — Full-halo producers**: Each `process_full_halo` module runs with
   `emission_allowed = true`. When a producer calls `module_emit_event()`, the
   event is appended to the phase buffer and **immediately dispatched** to all
   `process_per_event` consumers in YAML order.

2. **Pass 2 — Safety dispatch**: After each full-halo module completes, any
   un-dispatched events are flushed (defensive — normally a no-op since events
   dispatch immediately on emit).

3. **Pass 3 — By-galaxy modules**: Galaxy-major loop over all
   `process_by_galaxy` modules. `ctx->active_event` is NULL.

### 1.5 Emission Guards

- Events can **only** be emitted during `PROCESSING_MODE_FULL_HALO` execution.
- Emission is **disabled** during consumer dispatch to prevent recursive re-emit.
- Outside active phase dispatch (e.g., unit tests), events are silently dropped.

### 1.6 Current Usage

Only one event type exists: `SAGE_EVENT_MERGER`.

- **Producer**: `sage_handle_mergers_immediate` (phase_2, `process_full_halo`)
  emits merger events after `mimic_sage_merge_transfer()` completes.
- **Consumers**: `sage_quasar_mode` and `sage_collisional_starburst` (phase_2,
  `process_per_event`) both receive every event and check
  `event->event_code == SAGE_EVENT_MERGER` before acting.

YAML configuration (`input/millennium.yaml`):

```yaml
phase_2:
  - sage_handle_mergers_immediate: process_full_halo
  - sage_quasar_mode: process_per_event
  - sage_collisional_starburst: process_per_event
```

---

## 2. Identified Limitations

### 2.1 No Event Routing (All Consumers Receive All Events)

**Problem**: `dispatch_events_range()` iterates all `process_per_event` modules
in the phase and calls every one of them for every event, regardless of
`event_code`. Consumers must internally check and silently ignore irrelevant
events.

**Current dispatch logic** (`module_registry.c:406-446`):

```c
for (int event_index = start_index; event_index < end_index; event_index++) {
  const struct ModuleEvent *event = &phase_event_state.events[event_index];
  struct Halo *target_halo = &phase_event_state.halos[event->target_index];

  for (int i = 0; i < phase_event_state.num_modules; i++) {
    if (phase_config[i].processing_mode != PROCESSING_MODE_PER_EVENT)
      continue;
    // ^^^ No event_code filter — ALL per-event modules called for ALL events
    struct Module *mod = find_module_by_name(phase_config[i].module_name);
    mod->process(ctx, target_halo, 1);
  }
}
```

**Consequences as Mimic scales**:

- **Wasted work**: If a phase has 5 event types and 10 consumers, each event
  triggers 10 function calls even if only 2 consumers care about that type.
- **Error-prone**: A consumer that forgets the `event_code` check silently
  processes the wrong event type. This is a latent physics bug with no compile-time
  or runtime guard.
- **Unclear dependencies**: Reading the YAML, you cannot tell which consumer
  responds to which event type. The relationship is buried in C code.

### 2.2 Fixed Payload Shape (Two Doubles Only)

**Problem**: `struct ModuleEvent` provides exactly `value0` and `value1`. This
is sufficient for SAGE mergers (mass ratio + dt) but constrains future models.

**Examples of payloads that don't fit two doubles**:

| Hypothetical Event | Required Payload |
|-------------------|-----------------|
| Ram-pressure stripping | 3D velocity vector (3 doubles), gas density |
| Multi-species feedback | yields per element (N doubles) |
| Tidal interaction | orbital parameters (eccentricity, pericenter, inclination) |
| Environmental quenching | local density, distance to cluster centre, temperature |

A model author's only workaround today is to stash extra data in galaxy
properties as temporary transport fields — the same pattern that
`sage_disk_instability` uses for `UnstableDiskGasFraction`. This works but
defeats the purpose of the event system by scattering payload data across two
mechanisms.

---

## 3. Proposed Improvements

### 3.1 Event Subscription via YAML (Event Routing)

#### 3.1.1 Design

Allow `process_per_event` consumers to declare which event codes they subscribe
to, directly in the pipeline YAML. The core routes events only to matching
consumers.

**New YAML syntax**:

```yaml
phase_2:
  - sage_handle_mergers_immediate: process_full_halo
  - sage_quasar_mode:
      mode: process_per_event
      events: [1]                    # SAGE_EVENT_MERGER = 1
  - sage_collisional_starburst:
      mode: process_per_event
      events: [1]                    # SAGE_EVENT_MERGER = 1
```

**Rules**:

- `events` is an optional list of integer event codes.
- If `events` is **omitted**, the consumer receives **all** events (preserves
  current broadcast behaviour for backward compatibility).
- If `events` is **present**, the consumer is only called for events whose
  `event_code` matches one of the listed integers.
- Event codes are opaque integers to the core — their meaning is defined by
  physics modules (Principle 1).

The shorthand form remains valid for broadcast consumers:

```yaml
  - my_catch_all_consumer: process_per_event   # receives everything
```

#### 3.1.2 Data Structure Changes

Extend `PhaseModuleConfig` in `module_registry.h`:

```c
struct PhaseModuleConfig {
  char *module_name;
  enum ProcessingMode processing_mode;

  /* Event subscription filter (only for PROCESSING_MODE_PER_EVENT) */
  int *subscribed_event_codes;    /* NULL = subscribe to all (broadcast) */
  int num_subscribed_event_codes; /* 0 when subscribed_event_codes is NULL */
};
```

#### 3.1.3 Dispatch Logic Change

In `dispatch_events_range()`, add a filter check before calling each consumer:

```c
for (int i = 0; i < phase_event_state.num_modules; i++) {
  if (phase_config[i].processing_mode != PROCESSING_MODE_PER_EVENT)
    continue;

  /* NEW: event code filter */
  if (!event_matches_subscription(&phase_config[i], event->event_code))
    continue;

  /* ... existing dispatch ... */
}
```

Where `event_matches_subscription()` is:

```c
static bool event_matches_subscription(const struct PhaseModuleConfig *config,
                                       int event_code) {
  if (config->subscribed_event_codes == NULL)
    return true;  /* broadcast: no filter */

  for (int j = 0; j < config->num_subscribed_event_codes; j++) {
    if (config->subscribed_event_codes[j] == event_code)
      return true;
  }
  return false;
}
```

#### 3.1.4 YAML Parsing Changes

The YAML parser for phase configuration (`config.c` or equivalent) must handle
the extended map form alongside the existing string shorthand. Pseudocode:

```
for each entry in phase list:
  if entry is "module_name: mode_string":
    parse as before (subscribed_event_codes = NULL)
  else if entry is "module_name: {mode: ..., events: [...]}":
    parse mode from "mode" key
    parse subscribed_event_codes from "events" list
    validate: events list only valid when mode == process_per_event
```

#### 3.1.5 Validation

At pipeline init time (`module_system_init()`), add:

- **Warning**: If a `process_per_event` module has no `events` filter and the
  phase has multiple event-emitting producers, log a warning that the consumer
  will receive all event types. This catches accidental broadcast subscriptions.
- **Error**: If `events` is specified on a non-`process_per_event` module, fail
  with a clear message.

#### 3.1.6 Impact on Existing Modules

**None.** The shorthand YAML syntax is unchanged. Existing consumers with no
`events` field get `subscribed_event_codes = NULL`, which means broadcast —
identical to current behaviour. No module C code changes are required.

---

### 3.2 Extended Event Payloads

#### 3.2.1 Design

Add an optional opaque payload pointer to `ModuleEvent` so producers can attach
arbitrary data. The core transports the pointer without inspecting it. Producers
and consumers share the payload type definition via module-level headers.

#### 3.2.2 Data Structure Changes

In `module_interface.h`, extend `ModuleEvent`:

```c
struct ModuleEvent {
  enum ModuleEventType type;
  int event_code;
  int source_index;
  int target_index;
  double value0;              /* Keep for simple payloads */
  double value1;              /* Keep for simple payloads */
  const void *payload;        /* NEW: optional opaque extended payload */
  size_t payload_size;        /* NEW: payload size in bytes (0 = unused) */
};
```

#### 3.2.3 API Changes

Extend `module_emit_event()` with a new variant for extended payloads:

```c
/* Existing API — unchanged, sets payload=NULL, payload_size=0 */
int module_emit_event(struct ModuleContext *ctx, int event_code,
                      int source_index, int target_index,
                      double value0, double value1);

/* NEW: emit with extended payload */
int module_emit_event_ex(struct ModuleContext *ctx, int event_code,
                         int source_index, int target_index,
                         double value0, double value1,
                         const void *payload, size_t payload_size);
```

The original `module_emit_event()` is implemented as a thin wrapper:

```c
int module_emit_event(struct ModuleContext *ctx, int event_code,
                      int source_index, int target_index,
                      double value0, double value1) {
  return module_emit_event_ex(ctx, event_code, source_index, target_index,
                              value0, value1, NULL, 0);
}
```

#### 3.2.4 Payload Lifetime and Ownership

**Critical constraint**: Events are dispatched **immediately** when emitted
(inside `module_emit_event()`). This means the producer's stack frame is still
active when consumers run. Therefore:

- Producers can pass a pointer to a **stack-allocated** struct. The pointer is
  valid for the duration of consumer dispatch.
- No heap allocation or deep copy is needed.
- After `module_emit_event_ex()` returns, the payload pointer is no longer
  stored — it is safe to let the stack variable go out of scope.

This is the same lifetime model as `ctx->active_event` itself (valid only during
the consumer's `process()` call).

#### 3.2.5 Consumer Access Pattern

```c
/* In a consumer module */
#include "my_model_events.h"   /* defines struct MyStrippingPayload */

static int my_consumer_process(struct ModuleContext *ctx,
                               struct Halo *halos, int ngal) {
  if (ctx->active_event == NULL) return 0;
  if (ctx->active_event->event_code != MY_EVENT_STRIPPING) return 0;

  /* Type-safe payload access */
  const struct MyStrippingPayload *p =
      (const struct MyStrippingPayload *)ctx->active_event->payload;

  apply_stripping(halos[0].galaxy, p->velocity, p->gas_density);
  return 0;
}
```

#### 3.2.6 Safety Considerations

- The core never dereferences `payload`. It stores and forwards the pointer
  only (Principle 1).
- `payload_size` is informational — it enables defensive checks in consumers
  (`assert(event->payload_size == sizeof(MyPayload))`) but the core does not
  use it.
- Producers that use the original `module_emit_event()` automatically get
  `payload = NULL, payload_size = 0`. Consumers checking `payload != NULL`
  before casting are safe.

#### 3.2.7 Impact on Existing Modules

**Minimal.** The original `module_emit_event()` signature is unchanged. The
`struct ModuleEvent` gains two new fields at the end, initialized to zero/NULL
by the original API. Existing consumers never read `payload` and are unaffected.

---

## 4. Implementation Plan

### Phase A: Event Routing (Higher Priority)

This is the higher-priority change because it affects correctness and
maintainability. Without routing, adding a second event type creates a class of
silent bugs.

| Step | File(s) | Change |
|------|---------|--------|
| A1 | `src/core/module_registry.h` | Add `subscribed_event_codes` and `num_subscribed_event_codes` to `PhaseModuleConfig` |
| A2 | `src/core/module_registry.c` | Add `event_matches_subscription()` helper; insert filter in `dispatch_events_range()` |
| A3 | YAML parser (likely `src/core/config.c`) | Support extended map form `{mode: ..., events: [...]}` alongside string shorthand |
| A4 | `src/core/module_registry.c` | Add validation in `module_system_init()`: warn on broadcast consumers when multiple producers exist; error on `events` with non-per-event mode |
| A5 | Memory cleanup | Free `subscribed_event_codes` arrays during `module_system_cleanup()` or config teardown |
| A6 | Tests | Unit tests for `event_matches_subscription()`: broadcast (NULL), single match, multi-match, no match. Integration test with two event types in one phase verifying correct routing. |
| A7 | `docs/DEVELOPER-GUIDE.md` | Update `process_per_event` examples to show new YAML syntax |
| A8 | `input/millennium.yaml` | Optionally update SAGE consumers to use explicit `events: [1]` (not required — backward compatible) |

### Phase B: Extended Payloads (Lower Priority)

This can wait until a concrete use case arrives. It is a smaller change with no
backward-compatibility risk.

| Step | File(s) | Change |
|------|---------|--------|
| B1 | `src/core/module_interface.h` | Add `payload` and `payload_size` fields to `struct ModuleEvent` |
| B2 | `src/core/module_interface.h` | Declare `module_emit_event_ex()` |
| B3 | `src/core/module_registry.c` | Implement `module_emit_event_ex()`; refactor `module_emit_event()` as wrapper |
| B4 | `src/core/module_registry.c` | Initialize `payload = NULL, payload_size = 0` in the `_ex` function |
| B5 | Tests | Unit test: emit with payload, verify consumer receives correct pointer and size. Unit test: emit without payload (original API), verify NULL. |
| B6 | `docs/DEVELOPER-GUIDE.md` | Add extended payload example |

### Phase C: Optional Enhancements (Future)

These are not required now but are natural extensions once A and B land.

- **Named event codes in YAML**: Allow `events: [merger]` instead of
  `events: [1]` by having modules declare event code names in
  `module_info.yaml`. The generator would produce a mapping table. This improves
  readability but adds generator complexity.
- **Event code registry**: A runtime registry where producers register their
  event codes during `init()`. This enables validation that consumers subscribe
  to event codes that actually exist in the pipeline. Lightweight but adds a new
  core concept.
- **Event statistics**: Count events emitted/dispatched/filtered per phase for
  diagnostic logging. Trivial to add in `dispatch_events_range()`.

---

## 5. Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| YAML parser complexity increases | Keep the string shorthand as primary form; map form is opt-in. Test both forms thoroughly. |
| Payload pointer misuse (wrong type cast) | Document that `payload_size` should be checked. Core cannot enforce type safety across the physics boundary — this is inherent to C's type system and consistent with how `event_code` works today. |
| Broadcast consumers silently miss new event types | The validation warning in A4 catches this. Recommend explicit `events` subscriptions in documentation. |
| Performance regression from filter check | `event_matches_subscription()` is O(N) where N is the subscription list length (typically 1-3). Negligible compared to the physics computation in each consumer. |

---

## 6. Vision Principle Compliance

| Principle | How This Design Complies |
|-----------|------------------------|
| 1. Physics-Agnostic Core | Core routes by integer event codes without knowing their meaning. Payload is an opaque `void *`. No physics-specific types in core. |
| 2. Runtime Modularity | Event subscriptions are configured in YAML at runtime. No recompilation needed to change routing. |
| 3. Metadata-Driven Architecture | Subscriptions live in the pipeline YAML (the single specification for a run). Future named event codes would be declared in `module_info.yaml`. |
| 4. Single Source of Truth | The YAML file is the complete specification of which modules receive which events. No implicit broadcast surprises. |
| 5. Unified Processing Model | No changes to the three-pass execution model. Routing is a filter within the existing dispatch loop. |
| 6. Memory Efficiency | Payload uses stack pointers with zero-copy semantics. No heap allocation. Event buffer size is unchanged. |
| 7. Format-Agnostic I/O | No impact. |
| 8. Type Safety | `payload_size` enables runtime assertions. Subscription validation catches misconfiguration at init time. |

---

## 7. Files to Modify (Complete List)

| File | Phases | Nature of Change |
|------|--------|-----------------|
| `src/core/module_registry.h` | A | Add fields to `PhaseModuleConfig` |
| `src/core/module_registry.c` | A, B | Filter in dispatch; new emit function; validation |
| `src/core/module_interface.h` | B | Extend `ModuleEvent`; declare `module_emit_event_ex()` |
| YAML parser (`src/core/config.c` or equivalent) | A | Parse extended map form |
| `docs/DEVELOPER-GUIDE.md` | A, B | Update examples |
| `input/millennium.yaml` | A (optional) | Add explicit event subscriptions |
| Test files | A, B | New unit and integration tests |
