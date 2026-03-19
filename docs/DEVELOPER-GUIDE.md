# Mimic Developer Guide

**Practical guide to developing physics modules and extending Mimic**

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Architecture Overview](#architecture-overview)
3. [Creating Physics Modules](#creating-physics-modules)
4. [Property System](#property-system)
5. [Testing](#testing)
6. [Development Workflow](#development-workflow)
7. [Complete Example: Multi-File Module](#complete-example-multi-file-module)
8. [Appendix: Reference Tables](#appendix-reference-tables)

**Common tasks**:
- Adding a module? [Creating Physics Modules](#creating-physics-modules) | [Quick Start](#quick-start)
- Adding a property? [Property System](#property-system) | [A2. Property Metadata Schema](#a2-property-metadata-schema)
- Using events? [process_per_event example](#processing-modes) (under Processing Modes)
- Debugging a module? [Debugging a broken module](#debugging-a-broken-module)
- How does code generation work? [Code generation internals](#how-code-generation-works)
- API lookup? [A4. ModuleContext](#a4-modulecontext-structure) | [A5. Parameters](#a5-parameter-loading-macros) | [A6. Processing Modes](#a6-processing-modes)
- Configuration reference? [USER-GUIDE.md Configuration](USER-GUIDE.md#configuration)

---

## Quick Start

**Create a minimal runtime module in 3 steps**:

```bash
# 1. Create module directory
mkdir -p src/modules/my_module/_tests

# 2. Create module implementation
cat > src/modules/my_module/my_module.c << 'EOF'
#include "_system/parameter_helpers.h"

static double my_efficiency;

static int my_module_init(void) {
  LOAD_PARAM_DOUBLE("MyEfficiency", my_efficiency);
  VERBOSE_LOG("MyEfficiency = %.3f", my_efficiency);
  return 0;
}

static int my_module_process(struct ModuleContext *ctx,
                              struct Halo *halos, int ngal) {
  for (int i = 0; i < ngal; i++) {
    struct GalaxyData *gal = halos[i].galaxy;
    if (gal == NULL) continue;
    gal->ColdGas += my_efficiency * ctx->substep_dt;
  }
  return 0;
}

static int my_module_cleanup(void) {
  return 0;
}
EOF

# 3. Add metadata, configure, and run
# Create src/modules/my_module/module_info.yaml:
# module:
#   name: my_module
#   supported_processing_modes: [process_by_galaxy]
#
# Add to input/millennium.yaml:
#   modules:
#     phase_1:
#       - my_module: process_by_galaxy
#     parameters:
#       MyEfficiency: 0.5

make generate && make
./mimic input/millennium.yaml
```

Every runtime module now lives in its own directory and must declare
`supported_processing_modes` in `module_info.yaml`.

---

## Architecture Overview

For the design principles behind these decisions, see [VISION.md](VISION.md).

### System Architecture

```
┌───────────────────────────────────────────────────────┐
│                    Mimic Application                  │
├───────────────────────────────────────────────────────┤
│  Configuration        │  Module System                │
│  - YAML parsing       │  - Runtime registration       │
│  - Validation         │  - Dependency resolution      │
├───────────────────────────────────────────────────────┤
│               Physics-Agnostic Core                   │
│  ┌─────────────┬──────────────┬────────────────────┐  │
│  │ Memory Mgmt │ Properties   │ I/O System         │  │
│  │ Tree Process│ Pipeline     │ Testing            │  │
│  └─────────────┴──────────────┴────────────────────┘  │
├───────────────────────────────────────────────────────┤
│                   Physics Modules                     │
│  ┌──────────────┬───────────────┬─────────────────┐   │
│  │  Standalone  │   Directory   │ Test Fixtures   │   │
│  └──────────────┴───────────────┴─────────────────┘   │
└───────────────────────────────────────────────────────┘
```

**Key directories**:

- `src/core/`: Physics-agnostic execution (main, init, build_model, parameters)
- `src/io/`: Tree readers and output writers (binary, HDF5)
- `src/modules/`: Physics modules
  - `_system/`: Framework infrastructure (don't modify)
  - `_shared/`: Reusable physics utilities (can modify/extend)
  - `_tests/`: Shared cross-module regression tests only
  - `_archive/`: Retired modules/tests kept out of the live runtime tree
  - `sage_*/`: SAGE physics implementation
- `src/util/`: Memory, logging, numerical utilities
- `src/include/generated/`: Auto-generated code from metadata

### Key Concepts

**Galaxy types**:
- **Type 0** — FOF central galaxy (has a resolved halo, is the main subhalo of the FOF group)
- **Type 1** — Satellite galaxy with a resolved subhalo (still tracked by the N-body simulation)
- **Type 2** — Orphan satellite (subhalo no longer resolved; galaxy properties are preserved from last resolved snapshot)

Infall properties (`infallMvir`, `infallVvir`, `infallVmax`) are recorded when a galaxy transitions from Type 0 to Type 1 or Type 2.

**Halo data structures**:
- **InputTreeHalos** (`struct RawHalo`): Raw merger tree (immutable, read from file)
- **FoFWorkspace** (`struct Halo`): Temporary processing workspace (modules modify galaxies here)
- **ProcessedHalos** (`struct Halo`): Permanent storage (written to output after processing)

**Galaxy inheritance**: When processing a new snapshot, each galaxy is deep-copied from `ProcessedHalos` (previous snapshot) into `FoFWorkspace` (current snapshot). This preserves all galaxy properties across time. Snapshot-scoped accumulator properties (those with `init_repeat: true`) are reset to their initial value after the copy. Halo properties (Mvir, Rvir, etc.) are updated from the current tree data.

**Module error handling**:
- `init()` returns non-zero: startup aborts immediately with an error message
- `process()` returns non-zero: program exits with `EXIT_FAILURE` (fatal — the pipeline does not continue)
- `cleanup()` returns non-zero: error is logged but cleanup continues for remaining modules (best-effort)

Modules should return `-1` on error after logging a descriptive message with `ERROR_LOG()`.

### Module Communication

- Modules **never** call each other directly
- Communication **only** through galaxy property system
- Core calls modules in dependency-resolved order

---

## Creating Physics Modules

### Runtime Module Pattern

Runtime physics modules use a single directory-based pattern:
```
src/modules/my_module/
  my_module.c
  module_info.yaml
  helper.c
  README.md
  _tests/
```
- `module_info.yaml` is required for runtime modules
- `supported_processing_modes` must be declared explicitly
- Module-specific tests live in the module-local `_tests/` directory
- Additional helper files are optional

Two special collections also exist under `src/modules/`:

- `_tests/`: shared cross-module tests that do not belong to one module
- `_archive/`: retired modules/tests preserved for reference, not compiled

### Minimal Module Example

**Runtime module** (`src/modules/my_cooling/my_cooling.c`):

```c
#include "_system/parameter_helpers.h"

/* Module parameters */
static double cooling_efficiency;

/* Required: Initialize module */
static int my_cooling_init(void) {
  /* Load parameters */
  LOAD_PARAM_DOUBLE("CoolingEfficiency", cooling_efficiency);

  /* Validate */
  if (cooling_efficiency < 0.0 || cooling_efficiency > 1.0) {
    ERROR_LOG("CoolingEfficiency must be in [0,1], got %.3f",
              cooling_efficiency);
    return -1;
  }

  VERBOSE_LOG("My Cooling initialized");
  VERBOSE_LOG("  CoolingEfficiency = %.3f", cooling_efficiency);
  return 0;
}

/* Required: Process halos */
static int my_cooling_process(struct ModuleContext *ctx,
                               struct Halo *halos, int ngal) {
  /* Access simulation context */
  double dt = ctx->substep_dt;       /* Time step */
  double z = ctx->redshift;           /* Current redshift */
  double h = ctx->params->Hubble_h;   /* Hubble parameter */

  /* Process each galaxy */
  for (int i = 0; i < ngal; i++) {
    struct GalaxyData *gal = halos[i].galaxy;
    if (gal == NULL) continue;  /* Skip if no galaxy */

    /* Read properties (inputs) */
    float hot_gas = gal->HotGas;
    float mvir = halos[i].Mvir;

    /* Compute physics */
    float cooling_mass = cooling_efficiency * hot_gas * dt;

    /* Write properties (outputs) */
    gal->HotGas -= cooling_mass;
    gal->ColdGas += cooling_mass;
  }

  return 0;
}

/* Required: Cleanup module */
static int my_cooling_cleanup(void) {
  VERBOSE_LOG("My Cooling cleaned up");
  return 0;
}
```

**Minimal `module_info.yaml`** (`src/modules/my_cooling/module_info.yaml`):

```yaml
module:
  name: my_cooling
  supported_processing_modes: [process_by_galaxy]
```

**Configure in `input/millennium.yaml`**:

```yaml
modules:
  phase_1:
    - my_cooling: process_by_galaxy

  parameters:
    CoolingEfficiency: 0.5
```

**Build and run**:

```bash
make generate && make
./mimic input/millennium.yaml
```

### Directory Module with Metadata

This is the standard runtime module layout for both simple and complex modules.
Use extra helper files only when needed.

**Structure**:

```
src/modules/my_cooling/
  my_cooling.c           # Main implementation
  cooling_tables.c       # Helper functions
  cooling_tables.h       # Helper headers
  module_info.yaml       # Metadata
  README.md              # Physics documentation
  _tests/
    test_unit.c
```

**Minimal `module_info.yaml`**:

```yaml
module:
  name: my_cooling
  supported_processing_modes: [process_by_galaxy]
```

**With validation** (recommended):

```yaml
module:
  name: my_cooling
  description: "Metallicity-dependent radiative cooling"
  supported_processing_modes: [process_by_galaxy]

  # my_cooling.c is implicit (always auto-included)
  additional_files:
    - cooling_tables.c
    - cooling_tables.h

  dependencies:
    properties:
      - HotGas
      - ColdGas
      - MetalsHotGas
    parameters:
      - CoolingEfficiency
      - CoolFunctionsDir

  tests:
    unit: _tests/test_unit_my_cooling.c
```

**Key points**:
- Runtime modules must be directory modules with `module_info.yaml`
- `{module_name}.c` is **always** implicit - never declare it
- `additional_files` only for **helper** files
- `dependencies` provides validation, not enforcement
- All fields except `name` and `supported_processing_modes` are optional
- Put module-specific tests in `src/modules/<module>/_tests/`
- Use `src/modules/_tests/` only for shared cross-module coverage

### Processing Modes

Modules can process galaxies in three modes (see [Appendix A6](#a6-processing-modes) for complete reference):

**Example: process_by_galaxy** (better cache locality):

```c
static int my_module_process(struct ModuleContext *ctx,
                              struct Halo *halos, int ngal) {
  /* Core guarantees ngal = 1 for process_by_galaxy */
  struct GalaxyData *gal = halos[0].galaxy;
  if (gal == NULL) return 0;

  /* Process single galaxy */
  gal->StellarMass += compute_star_formation(gal, ctx->substep_dt);
  return 0;
}
```

**Example: process_full_halo** (better for vectorization):

```c
static int my_module_process(struct ModuleContext *ctx,
                              struct Halo *halos, int ngal) {
  /* Receives full FOF group array (ngal can be 1 to 1000s) */

  /* Example: Calculate total hot gas in FOF group */
  double total_hot = 0.0;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy != NULL) {
      total_hot += halos[i].galaxy->HotGas;
    }
  }

  /* Example: Distribute among satellites */
  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type == 1 && halos[i].galaxy != NULL) {
      halos[i].galaxy->HotGas += total_hot * some_fraction;
    }
  }

  return 0;
}
```

**Example: process_per_event** (complete wiring):

Event contracts are declared in `module_info.yaml` and numeric event IDs are
generated by `make generate`. No hand-written event-code enums are needed.

**Step 1 — Producer metadata** (`my_merge_producer/module_info.yaml`):

```yaml
events:
  emits:
    - name: merger
      description: "Triggered when satellite merges into central; value0=mass_ratio, value1=source_dt"
```

**Step 2 — Consumer metadata** (`my_quasar_consumer/module_info.yaml`):

```yaml
events:
  consumes:
    - producer: my_merge_producer
      event: merger
```

**Step 3 — Producer C code** (`my_merge_producer.c`):

```c
#include "_system/generated/event_contracts.h"

/* ... inside process() ... */
if (module_emit_event(ctx, MY_MERGE_PRODUCER_EVENT_MERGER,
                      satellite_idx, central_idx,
                      mass_ratio, source_dt) != 0) {
  ERROR_LOG("Failed to emit merger event (source=%d target=%d ratio=%.6f)",
            satellite_idx, central_idx, mass_ratio);
  return -1;
}
```

`module_emit_event(...)` returns `0` on success and non-zero on failure. Treat
non-zero as fatal in producer modules to avoid silent physics divergence.
In normal pipeline execution, non-zero means invalid context/mode/indices or
event-buffer overflow. For direct producer unit tests (outside active phase
dispatch), events are intentionally dropped and `0` is returned.

**Step 4 — Consumer C code** (`my_quasar_consumer.c`):

```c
/* No event_contracts.h needed — subscription routing already filters events.
 * Only subscribed events arrive at this process() function. */

static int my_quasar_consumer_process(struct ModuleContext *ctx,
                                      struct Halo *halos, int ngal) {
  if (ctx->active_event == NULL) return 0;
  if (ngal != 1) return -1;

  /* Subscription routing guarantees only merger events reach here. */
  apply_event_physics(&halos[0], ctx->active_event->value0);
  return 0;
}
```

**Step 5 — Run `make generate`** to produce the generated constants:

```
src/modules/_system/generated/event_contracts.h:
  #define MODULE_ID_MY_MERGE_PRODUCER 3
  enum MyMergeProducerEventId {
    MY_MERGE_PRODUCER_EVENT_MERGER = 1,
  };
```

**Using process_per_event in YAML** (no change from before):

```yaml
phase_2:
  - my_merge_producer: process_full_halo         # Declares events.emits: [merger]
  - my_other_full_halo_step: process_full_halo
  - my_quasar_consumer: process_per_event         # Declares events.consumes: [my_merge_producer/merger]
  - my_starburst_consumer: process_per_event      # Declares events.consumes: [my_merge_producer/merger]
```

Notes:
- `process_per_event` consumers are called only when a subscribed producer emits.
  Unsubscribed events are silently skipped. The consumer's `process()` function
  does not need to check event identity — subscription routing handles filtering.
- Every `process_per_event` module must declare at least one `events.consumes`
  entry. A module with no subscriptions is rejected at startup with a clear error.
- The producer must be configured as `process_full_halo` in the same phase as its
  consumers. Missing producer = startup error.
- YAML order of `process_per_event` entries defines consumer call order.
- Resolved event contracts are recorded in HDF5 output under
  `RunProperties/EventContracts` for reproducibility.

**Event payload** (v1):

The event payload is two `double` scalars (`value0`, `value1`). By convention:
- `value0`: primary physics quantity (e.g., mass ratio, efficiency fraction)
- `value1`: time-like quantity (e.g., source object's substep `dt`)

**Standalone modules and mode inheritance**

A bare `.c` file placed directly in `src/modules/` (no `module_info.yaml`) is a
valid standalone module. The generator will discover it and automatically assign
all three processing modes. This is intentional: standalone modules are a
lightweight option for quick prototyping or simple utilities that genuinely
support any calling convention.

When a standalone module is discovered, `make generate` prints:

```
WARNING: Standalone module 'my_module' inherits all three processing modes (no module_info.yaml).
```

This is informational. If the module has a real mode constraint, convert it to a
directory module and declare `supported_processing_modes` explicitly — the
generator will then enforce the declaration and startup validation will reject
misconfigured pipelines.

### Pipeline Phases

The multi-phase pipeline executes modules in four distinct phases:

```
┌───────────────────────────────────────────────────────────────┐
│ Snapshot N → N+1                                              │
├───────────────────────────────────────────────────────────────┤
│                                                               │
│  PRE_TIMESTEP (runs once):                                    │
│    ├─ Pass 1: Execute all process_full_halo modules           │
│    ├─ Pass 2: Dispatch emitted events to process_per_event    │
│    └─ Pass 3: For g = 0..ngal:                                │
│                 Execute all process_by_galaxy modules         │
│                                                               │
│  FOR each substep (0..SubSteps-1):                            │
│    │                                                          │
│    ├─ PHASE_1 (runs each substep):                            │
│    │    ├─ Pass 1: Execute all process_full_halo modules      │
│    │    ├─ Pass 2: Dispatch emitted events to process_per_event│
│    │    └─ Pass 3: For g = 0..ngal:                           │
│    │                 Execute all process_by_galaxy modules    │
│    │                                                          │
│    └─ PHASE_2 (runs each substep):                            │
│         ├─ Pass 1: Execute all process_full_halo modules      │
│         ├─ Pass 2: Dispatch emitted events to process_per_event│
│         └─ Pass 3: For g = 0..ngal:                           │
│                      Execute all process_by_galaxy modules    │
│                                                               │
│  POST_TIMESTEP (runs once):                                   │
│    ├─ Pass 1: Execute all process_full_halo modules           │
│    ├─ Pass 2: Dispatch emitted events to process_per_event    │
│    └─ Pass 3: For g = 0..ngal:                                │
│                 Execute all process_by_galaxy modules         │
│                                                               │
└───────────────────────────────────────────────────────────────┘

Key execution details:
  • Within each phase, process_full_halo modules ALWAYS execute first
  • Events emitted by full-halo modules are dispatched to process_per_event modules
  • process_per_event YAML placement controls consumer order, not exact timing vs full_halo lines
  • Then process_by_galaxy modules execute in galaxy-major loop
  • Each by_galaxy module receives single galaxy (ngal=1)
  • Each per_event module receives single event target halo (ngal=1, `ctx->active_event != NULL`)
  • Full_halo modules receive entire array (ngal can be 1-1000s)
  • Module order within same mode is preserved from YAML config
```

**Example: Multi-phase cooling**:

```yaml
SubSteps: 10  # Time sub-stepping

modules:
  pre_timestep:
    - my_calculate_cooling_budget: process_full_halo  # Calculate once

  phase_1:
    - my_add_cooling: process_by_galaxy  # Distribute over substeps

  post_timestep:
    - my_convert_to_rates: process_full_halo  # Finalize
```

**Choosing the right phase**:

- **pre_timestep**: Needs snapshot-level context (e.g., reionization, total infall)
- **phase_1**: Time-dependent physics requiring integration (cooling, SF, feedback)
- **phase_2**: Physics depending on phase_1 results (mergers, disruption)
- **post_timestep**: Converting accumulators to rates, cleanup

### Accessing Central Galaxy

**Both centrals and satellites can access the FOF central**:

```c
static int my_module_process(struct ModuleContext *ctx,
                              struct Halo *halos, int ngal) {
  for (int i = 0; i < ngal; i++) {
    struct GalaxyData *gal = halos[i].galaxy;
    if (gal == NULL) continue;

    /* Access FOF central galaxy (always available) */
    struct Halo *central = ctx->central_galaxy;

    /* Read central's halo properties */
    double central_vvir = central->Vvir;
    double central_mvir = central->Mvir;

    /* Read central's galaxy properties */
    double central_hot_gas = central->galaxy->HotGas;

    /* Example: Eject gas to central's hot halo */
    double ejected_mass = compute_ejection(gal, central_vvir);
    gal->ColdGas -= ejected_mass;
    central->galaxy->HotGas += ejected_mass;
  }
  return 0;
}
```

**Use cases**:
- Calculate ejection relative to central's potential well
- Add satellite's stripped gas to central's hot halo
- Eject gas from central's hot halo to central's ejected reservoir

**Safe to use**:
- `ctx->central_galaxy` is **always** non-NULL during module execution
- When processing a central, `ctx->central_galaxy` points to itself
- Works in both `process_by_galaxy` and `process_full_halo` modes

### Module Best Practices

**Parameter handling** (see [Appendix A5](#a5-parameter-loading-macros) for full reference):
```c
/* Load in init() */
static int my_module_init(void) {
  LOAD_PARAM_DOUBLE("MyEfficiency", my_efficiency);
  LOAD_PARAM_INT("MyMode", my_mode);
  LOAD_PARAM_STRING("MyPath", my_path, MAX_STRING_LEN);

  /* Validate (physics-based) */
  if (my_efficiency < 0.0 || my_efficiency > 1.0) {
    ERROR_LOG("MyEfficiency out of physical range");
    return -1;
  }

  return 0;
}
```

**Property access**:
```c
/* Read inputs */
float cold_gas = gal->ColdGas;
float mvir = halos[i].Mvir;

/* Write outputs */
gal->StellarMass += new_stars;
gal->ColdGas -= consumed_gas;
```

**Memory management** (see [Appendix A8](#a8-memory-management) for categories and functions):
```c
#include "util/memory.h"

/* Allocate with category tracking */
float *data = mymalloc_cat(size * sizeof(float), MEM_UTILITY);

/* Free in cleanup() */
static int my_module_cleanup(void) {
  myfree(data);
  return 0;
}
```

**Error handling and logging** (see [Appendix A9](#a9-logging-macros) for all levels):
```c
/* Return 0 on success, non-zero on failure */
if (error_condition) {
  ERROR_LOG("Descriptive error message");
  return -1;
}

DEBUG_LOG("Detailed debugging info");        /* --debug only */
VERBOSE_LOG("Configuration info");           /* --verbose or --debug */
INFO_LOG("General progress");                /* Default level */
WARNING_LOG("Non-fatal issues");             /* Always shown */
FATAL_ERROR("Fatal errors");                 /* Always shown, exits program */
```

**Shared utilities** (`src/modules/_shared/`):
```c
#include "../_shared/metallicity.h"  /* Use in your module */
```

See `src/modules/_shared/README.md` for guidelines on creating new shared utilities.

---

## Property System

### Overview

Properties are galaxy/halo attributes stored in C structs and defined in YAML metadata.

**Two categories**:
- **Halo properties** (`src/core/halo_properties.yaml`): From merger tree (Mvir, Rvir, Vmax)
- **Galaxy properties** (`src/modules/model_properties.yaml`): From physics modules (ColdGas, StellarMass)

**Workflow**:
1. Define property in YAML
2. Run `make generate` → auto-generates C structs, accessors, output code, Python dtypes
3. Rebuild → property available in modules
4. Access via `gal->PropertyName` or `halos[i].PropertyName`

### Adding a Galaxy Property

**Step-by-step example**:

**1. Edit `src/modules/model_properties.yaml`**:

```yaml
galaxy_properties:
  - name: MyNewProperty
    type: float
    units: "1e10 Msun/h"
    description: "My new baryonic property"
    output: true
    init_source: default
    init_value: 0.0
    output_source: galaxy_property
```

**2. Regenerate code**:

```bash
make generate
```

This auto-generates:
- C struct field in `GalaxyData`
- Initialization code
- Output writers (binary and HDF5)
- Python dtypes for reading

**3. Use in module**:

```c
/* Read */
float value = gal->MyNewProperty;

/* Write */
gal->MyNewProperty = calculated_value;
```

**4. Rebuild**:

```bash
make clean && make
```

Property is now available in all modules and output files.

### Adding a Halo Property

Same workflow, but edit `src/core/halo_properties.yaml`:

```yaml
halo_properties:
  - name: MyHaloProperty
    type: float
    units: "Mpc/h"
    description: "Custom halo property"
    output: true
    init_source: copy_from_tree  # or default, calculate
    output_source: copy_direct
```

### Property Metadata Fields

For complete schema specification with all fields and options, see [Appendix A2](#a2-property-metadata-schema).

**Quick reference**:

```yaml
- name: dT
  type: float
  units: "Myr"
  description: "Time since previous snapshot"
  output: true
  init_source: default
  init_value: -1.0
  output_source: copy_direct
  output_convert: "UnitTime_in_s / SEC_PER_MEGAYEAR"  # sec → Myr
  range: [0.0, 2000.0]
  sentinels: [-1.0]  # -1 for unset (not converted)
```

### Output Modifier Functions

**When to use**: Properties that need conditional or calculated values at output time (e.g., type-dependent output, recalculated virial quantities).

**How it works**:
1. Set `output_source: recalculate` in property metadata
2. Specify `output_function: function_name` (function defined in `output_helpers.h`)
3. Specify `output_function_arg: "args"` (arguments passed to function)
4. Code generation creates: `o->PropertyName = function_name(args);`

**Example** — output infall property for satellites, 0.0 for centrals:

```c
/* In src/modules/_system/output_helpers.h */
static inline float output_infall_property_or_zero(const struct Halo *g,
                                                     float value)
{
    return (g->Type != 0) ? value : 0.0f;
}
```

```yaml
# In halo_properties.yaml
- name: infallMvir
  type: float
  units: "1e10 Msun/h"
  description: "Virial mass at infall (satellites only, 0 for centrals)"
  output: true
  init_source: default
  init_value: -1.0
  output_source: recalculate
  output_function: output_infall_property_or_zero
  output_function_arg: "g, g->infallMvir"
  range: [0.000001, 1000000.0]
  sentinels: [0.0, -1.0]
```

**Guidelines**:
- Place functions in `src/modules/_system/output_helpers.h`, make them `static inline`
- Return type must match property type (`float`, `double`, `int`, `long`)
- Common arguments: `g` (halo pointer), `g->PropertyName` (property value)
- After adding, run `make generate && make` and verify with a small test run

**Existing helper functions** (see `src/modules/_system/output_helpers.h`):
- `output_infall_property_or_zero(g, value)`: Satellites only (0.0 for centrals)
- `output_rvir_conditional(g)`: Recalculate Type 0/1, preserve Type 2
- `output_vvir_conditional(g)`: Recalculate Type 0/1, preserve Type 2

---

## Testing

### Test Framework

Mimic uses **three-tier testing**:

**Tier 1: Unit Tests** (C, fast <10s)
- Test individual functions
- Located in `tests/unit/`
- Auto-discovered from module metadata

**Tier 2: Integration Tests** (Python, <1min)
- Test full pipeline execution
- Located in `tests/integration/`
- Validate output formats

**Tier 3: Scientific Tests** (Python, <5min)
- Validate physics accuracy
- Located in `tests/scientific/`
- Compare against published results

### Running Tests

```bash
# All tests (recommended before commits)
make tests

# Individual tiers
make test-unit
make test-integration
make test-scientific

# Individual unit test (runner compiles the named test)
tests/unit/run_tests.sh test_memory_system
tests/unit/run_tests.sh test_unit_sage_apply_cooling

# Individual integration test
python3 tests/integration/test_full_pipeline.py
python3 src/modules/sage_apply_cooling/_tests/test_integration_sage_apply_cooling.py

# Individual scientific test
python3 tests/scientific/test_scientific.py
```

Run these commands from the repository root. Unit tests go through
`tests/unit/run_tests.sh` because they are compiled on demand and rely on the
generated module/test registries. Integration and scientific tests are regular
Python scripts and can be executed directly by path.

### Writing Unit Tests

**Create test file** (`src/modules/my_module/_tests/test_unit_my_module.c`):

```c
#include <stdio.h>
#include <assert.h>
#include <math.h>

void test_compute_cooling(void) {
  /* Setup */
  double hot_gas = 100.0;
  double efficiency = 0.5;
  double dt = 0.01;

  /* Execute */
  double cooling = compute_cooling_rate(hot_gas, efficiency, dt);

  /* Verify */
  assert(fabs(cooling - 0.5) < 1e-6);
  printf("✓ test_compute_cooling passed\n");
}

int main(void) {
  printf("Running my_module unit tests...\n");
  test_compute_cooling();
  printf("All tests passed!\n");
  return 0;
}
```

**Register in `module_info.yaml`**:

```yaml
tests:
  unit: _tests/test_unit_my_module.c
```

**Build and run**:

```bash
make generate  # Regenerate test configuration
make test-unit
```

### Writing Integration Tests

**Create test file** (`src/modules/my_module/_tests/test_integration_my_module.py`):

```python
#!/usr/bin/env python3
"""Integration test for my_module"""

import subprocess
import h5py
import numpy as np

def test_my_module_integration():
    """Test my_module in full pipeline"""

    # Run Mimic
    result = subprocess.run(
        ['./mimic', 'tests/data/test_config.yaml'],
        capture_output=True,
        text=True
    )

    # Verify success
    assert result.returncode == 0, f"Mimic failed: {result.stderr}"

    # Verify output properties
    with h5py.File('output/test/model_000.hdf5', 'r') as f:
        halos = f['Snap063/Galaxies'][:]

        # Check property exists
        assert 'MyNewProperty' in halos.dtype.names

        # Validate physics
        my_prop = halos['MyNewProperty']
        assert np.all(my_prop >= 0.0), "Property should be non-negative"

    print("✓ Integration test passed")

if __name__ == '__main__':
    test_my_module_integration()
```

**Run**:

```bash
python3 src/modules/my_module/_tests/test_integration_my_module.py
```

---

## Development Workflow

### Daily Development

**1. Edit code**:
```bash
vim src/modules/my_module/my_module.c
```

**2. Format code** (before commits):
```bash
./scripts/beautify.sh
```

**3. Build**:
```bash
make -j$(nproc)  # Parallel build
```

**4. Test**:
```bash
./mimic --debug input/millennium.yaml
make check-docs  # Docs links + USER-GUIDE module phase consistency
make tests  # Full test suite
```

### Code Generation

**When to regenerate**:
- After editing property YAML files
- After editing module metadata
- After adding/removing modules

**Regenerate**:
```bash
make generate
```

**Verify current** (CI check):
```bash
make check-generated
```

#### How code generation works

Two generator scripts produce all auto-generated code from YAML metadata:

```
halo_properties.yaml ──┐
                       ├──► generate_properties.py ──► property_defs.h, *.inc files, dtype.py
model_properties.yaml ─┘

module_info.yaml (each module) ──► generate_module_registry.py ──► module_init.c, event_contracts.h
```

**Property generation** (`scripts/generate_properties.py`) reads the two property YAML files and produces:

| Generated file | Location | Purpose |
|----------------|----------|---------|
| `property_defs.h` | `src/include/generated/` | C struct definitions (`Halo`, `GalaxyData`, `HaloOutput`) |
| `init_halo_properties.inc` | `src/include/generated/` | Copy tree fields into `FoFWorkspace` |
| `init_galaxy_properties.inc` | `src/include/generated/` | Zero/default galaxy fields |
| `reset_galaxy_properties.inc` | `src/include/generated/` | Reset snapshot-scoped accumulators (`init_repeat: true`) |
| `copy_to_output.inc` | `src/include/generated/` | Copy `Halo` → `HaloOutput` with unit conversions |
| `hdf5_field_*.inc` | `src/include/generated/` | HDF5 output metadata (field count, definitions, units) |
| `dtype.py` | `output/mimic-plot/generated/` | NumPy dtype and `get_units()` for reading output |
| `property_ranges.json` | `tests/generated/` | Validation ranges for scientific tests |

**Module generation** (`scripts/generate_module_registry.py`) scans all `module_info.yaml` files and produces:

| Generated file | Location | Purpose |
|----------------|----------|---------|
| `module_init.c` | `src/modules/_system/generated/` | Forward declarations, `struct Module` definitions, `register_all_modules()` |
| `event_contracts.h` | `src/modules/_system/generated/` | `#define MODULE_ID_*` macros and per-producer event enums |
| `module_sources.mk` | `tests/generated/` | Makefile fragment listing module sources for test compilation |

**Runtime flow**: At startup, `register_all_modules()` (generated) calls `module_registry_add()` for each module. The core then uses the registered `struct Module` entries — containing function pointers to `init()`, `process()`, `cleanup()` plus supported processing modes and event subscriptions — to dispatch the pipeline.

**Staleness detection**: Each generated file embeds an MD5 hash of its source YAML. `make check-generated` validates property-generated files only (comparing the embedded hash in `src/include/generated/` and `output/mimic-plot/generated/` against the current property YAML). Module-generated files (`module_init.c`, `event_contracts.h`) also embed hashes but are not yet covered by `check-generated`. The `make` build rules track YAML dependencies for both systems, so a normal `make` will regenerate automatically when any YAML file changes.

### Debugging

**Debug build with verbose output**:
```bash
make clean && make
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```

**Check memory leaks**:
```bash
# Built-in tracking
./mimic --debug input/millennium.yaml
# Check final memory report

# Valgrind (if needed)
valgrind --leak-check=full ./mimic input/millennium.yaml
```

**GDB debugging**:
```bash
gdb ./mimic
(gdb) run input/millennium.yaml
(gdb) bt  # Backtrace on crash
```

#### Debugging a broken module

When a module misbehaves, work through these stages in order:

**1. Validate metadata first** — catches most wiring errors before compilation:
```bash
make validate-modules
```
Common errors caught here:
- `Missing required fields: supported_processing_modes` — add to `module_info.yaml`
- `Module name 'X' doesn't match directory name 'Y'` — rename to match
- `Property 'X' not found in property metadata` — check spelling in `model_properties.yaml`
- `Invalid processing mode(s): process_invalid` — use one of `process_by_galaxy`, `process_full_halo`, `process_per_event`

**2. Regenerate and rebuild** — if the module was recently added or metadata changed:
```bash
make generate && make clean && make
```
If you skip this step, you'll see at runtime:
```
ERROR: Module 'my_module' configured but not registered
ERROR: Available modules:
ERROR:   - sage_apply_cooling
ERROR:   - ...
```

**3. Run with `--debug`** — shows module lifecycle with full context:
```bash
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```
Debug output traces each stage:
```
DEBUG - Initializing module: my_module
DEBUG - Executing module: my_module (full array, ngal=256, substep 3/10, z=1.234)
```

**4. Interpret failure messages** by stage:

| Stage | Error message | Behaviour |
|-------|---------------|-----------|
| `init()` returns non-zero | `Module 'X' initialization failed with code -1` | Program exits immediately — no trees processed |
| `process()` returns non-zero | `Module 'X' failed (substep N)` or `failed on galaxy N` | Program exits immediately — `exit(EXIT_FAILURE)` |
| `cleanup()` returns non-zero | Error logged | Cleanup continues for remaining modules (best-effort) |
| Missing parameter | `Required model parameter 'X' not found in input file` | Triggered during `init()` — add to YAML `parameters:` |
| Wrong processing mode | `Module 'X' does not support processing mode 'Y'` | Program exits at startup — fix mode in YAML |
| Event wiring error | `Module 'X' is configured as process_per_event but declares no event subscriptions` | Program exits at startup — add `events.consumes` to metadata |

**5. If process() fails silently** (exits with no descriptive error), add `ERROR_LOG()` before the `return -1` in your module. The core reports which module and substep failed, but only the module itself knows the physics reason.

---

## Complete Example: Multi-File Module

This example demonstrates a production-quality module with helper files, metadata, and tests.

**Directory structure**:

```
src/modules/advanced_cooling/
  advanced_cooling.c
  cooling_tables.c
  cooling_tables.h
  module_info.yaml
  README.md
  _tests/
    test_unit.c
```

**File**: `src/modules/advanced_cooling/cooling_tables.h`

```c
#ifndef COOLING_TABLES_H
#define COOLING_TABLES_H

/* Initialize cooling function lookup tables */
int init_cooling_tables(const char *table_dir);

/* Get cooling rate for given temperature and metallicity */
double get_cooling_rate(double temperature, double metallicity);

/* Cleanup cooling tables */
void free_cooling_tables(void);

#endif
```

**File**: `src/modules/advanced_cooling/cooling_tables.c`

```c
#include <stdio.h>
#include <stdlib.h>
#include "cooling_tables.h"
#include "util/error.h"
#include "util/memory.h"

/* Internal data structures */
static double *table_temp = NULL;
static double *table_rate = NULL;
static int table_size = 0;

int init_cooling_tables(const char *table_dir) {
  /* Load tables from file */
  char filename[512];
  snprintf(filename, 512, "%s/cooling_rates.dat", table_dir);

  FILE *f = fopen(filename, "r");
  if (f == NULL) {
    ERROR_LOG("Cannot open cooling table: %s", filename);
    return -1;
  }

  /* Read table size */
  fscanf(f, "%d", &table_size);

  /* Allocate memory */
  table_temp = mymalloc_cat(table_size * sizeof(double), MEM_UTILITY);
  table_rate = mymalloc_cat(table_size * sizeof(double), MEM_UTILITY);

  /* Read data */
  for (int i = 0; i < table_size; i++) {
    fscanf(f, "%lf %lf", &table_temp[i], &table_rate[i]);
  }

  fclose(f);
  VERBOSE_LOG("Loaded %d cooling table entries", table_size);
  return 0;
}

double get_cooling_rate(double temperature, double metallicity) {
  /* Simplified: linear interpolation */
  for (int i = 0; i < table_size - 1; i++) {
    if (temperature >= table_temp[i] && temperature < table_temp[i+1]) {
      double frac = (temperature - table_temp[i]) /
                    (table_temp[i+1] - table_temp[i]);
      return table_rate[i] + frac * (table_rate[i+1] - table_rate[i]);
    }
  }
  return 0.0;
}

void free_cooling_tables(void) {
  if (table_temp != NULL) myfree(table_temp);
  if (table_rate != NULL) myfree(table_rate);
}
```

**File**: `src/modules/advanced_cooling/advanced_cooling.c`

```c
#include "_system/parameter_helpers.h"
#include "cooling_tables.h"

/* Module parameters */
static char cool_dir[MAX_STRING_LEN];

static int advanced_cooling_init(void) {
  /* Load parameter */
  LOAD_PARAM_STRING("CoolFunctionsDir", cool_dir, MAX_STRING_LEN);

  /* Initialize helper functions */
  if (init_cooling_tables(cool_dir) != 0) {
    return -1;
  }

  VERBOSE_LOG("Advanced Cooling initialized");
  VERBOSE_LOG("  CoolFunctionsDir = %s", cool_dir);
  return 0;
}

static int advanced_cooling_process(struct ModuleContext *ctx,
                                     struct Halo *halos, int ngal) {
  double dt = ctx->substep_dt;

  for (int i = 0; i < ngal; i++) {
    struct GalaxyData *gal = halos[i].galaxy;
    if (gal == NULL) continue;

    /* Calculate virial temperature */
    double vvir = halos[i].Vvir;  /* km/s */
    double temp = 35.9 * vvir * vvir;  /* K */

    /* Get metallicity */
    double metallicity = 0.0;
    if (gal->HotGas > 0.0) {
      metallicity = gal->MetalsHotGas / gal->HotGas;
    }

    /* Use helper function */
    double cooling_rate = get_cooling_rate(temp, metallicity);

    /* Calculate cooling mass */
    double cooling_mass = cooling_rate * gal->HotGas * dt;
    if (cooling_mass > gal->HotGas) {
      cooling_mass = gal->HotGas;
    }

    /* Update properties */
    gal->CoolingGas = cooling_mass;
  }

  return 0;
}

static int advanced_cooling_cleanup(void) {
  free_cooling_tables();
  VERBOSE_LOG("Advanced Cooling cleaned up");
  return 0;
}
```

**File**: `src/modules/advanced_cooling/module_info.yaml`

```yaml
module:
  name: advanced_cooling
  description: "Metallicity-dependent radiative cooling with lookup tables"
  supported_processing_modes: [process_by_galaxy]

  # advanced_cooling.c is implicit (auto-included)
  additional_files:
    - cooling_tables.c
    - cooling_tables.h

  dependencies:
    properties:
      - HotGas
      - MetalsHotGas
      - CoolingGas
    parameters:
      - CoolFunctionsDir

  tests:
    unit: _tests/test_unit.c
```

**Build and run**:

```bash
make generate  # Regenerate with new module
make clean && make
./mimic input/millennium.yaml
```

---

## Appendix: Reference Tables

### A1. Module Metadata Schema (`module_info.yaml`)

**Required fields**:

| Field | Type | Description | Example |
|-------|------|-------------|---------|
| `name` | string | Module name (lowercase_with_underscores) | `"my_cooling"` |
| `supported_processing_modes` | array | Processing modes this module supports | `[process_by_galaxy]` |

**Optional fields**:

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `description` | string | "" | 1-2 sentence physics summary |
| `display_name` | string | Auto from name | Human-readable name |
| `version` | string | `"1.0.0"` | Semantic version |
| `author` | string | "" | Attribution |
| `additional_files` | array | `[]` | Helper `.c` and `.h` files (excludes `{name}.c`) |
| `dependencies.properties` | array | `[]` | Properties used (validation only) |
| `dependencies.parameters` | array | `[]` | Parameters from input YAML (validation) |
| `events.emits` | array | `[]` | Events this module emits (producers only; generates C enum constants) |
| `events.consumes` | array | `[]` | Events this module subscribes to (consumers only; required for `process_per_event`) |
| `tests.unit` | string | - | Path to C unit test |
| `tests.integration` | string | - | Path to Python integration test |
| `tests.scientific` | string | - | Path to scientific validation test |
| `docs.physics` | string | - | Path to physics documentation |
| `compilation_requires` | array | `[]` | Required features: `["hdf5", "mpi", "gsl"]` |

### A2. Property Metadata Schema

**Halo properties**: `src/core/halo_properties.yaml`
**Galaxy properties**: `src/modules/model_properties.yaml`

**Required fields**:

| Field | Type | Description | Example |
|-------|------|-------------|---------|
| `name` | string | Property name (PascalCase) | `"ColdGas"` |
| `type` | enum | C type: `float`, `double`, `int`, `long` | `float` |
| `units` | string | Physical units | `"1e10 Msun/h"` |
| `description` | string | Human-readable description | `"Cold gas mass in disk"` |
| `output` | boolean | Include in output files? | `true` |
| `init_source` | enum | Initialization method (see table below) | `default` |
| `output_source` | enum | Output method (required only if `output: true`) | `galaxy_property` |

**Initialization sources** (`init_source`):

| Value | Additional Field | Description |
|-------|-----------------|-------------|
| `default` | `init_value` (required) | Initialize to constant value |
| `copy_from_tree` | `init_value` (tree field name) | Copy from merger tree input |
| `calculate` | `init_function` (function name) | Call function to calculate |
| `skip` | - | Custom initialization (manual in code) |

**Output sources** (`output_source`, only required if `output: true`):

| Value | Description |
|-------|-------------|
| `copy_direct` | Copy halo property directly to output |
| `galaxy_property` | Copy galaxy property directly to output |
| `recalculate` | Call function at output time (`output_function` required) |

**Note**: Properties with `output: false` do not require `output_source` and will not be written to output files.

**Optional fields**:

| Field | Type | Description |
|-------|------|-------------|
| `init_value` | varies | Initialization value or tree field name |
| `init_function` | string | Function to call for calculated initialization |
| `init_repeat` | boolean | Re-initialize each snapshot? (default: `false`) |
| `output_convert` | string | Unit conversion expression (e.g., `"sec to Myr"`) |
| `output_transform` | string | Transform function (e.g., `"log10"`) |
| `output_function` | string | Function to call for recalculated output |
| `output_function_arg` | string | Argument to output function |
| `range` | array `[min, max]` | Valid range for validation |
| `sentinels` | array | Special values exempt from range checks |
| `role` | string | Semantic role: `transport` marks inter-module scratch fields (not persistent output). Use alongside a comment naming the producer and consumer modules. |

**Complete examples**:

```yaml
# Output property (requires output_source)
- name: ColdGas
  type: float
  units: "1e10 Msun/h"
  description: "Cold gas mass available for star formation"
  output: true
  init_source: default
  init_value: 0.0
  output_source: galaxy_property
  range: [0.0, 100000.0]
  sentinels: [0.0]

# Transport property (inter-module scratch field, not persistent output)
- name: CoolingGas
  type: float
  units: "1e10 Msun/h"
  description: "Gas mass cooling this substep"
  role: transport  # Written by sage_calculate_cooling_budget; read/modified by sage_radio_mode_heating; consumed by sage_apply_cooling
  output: false
  init_source: default
  init_value: 0.0
  init_repeat: true
  range: [0.0, 100000.0]
  sentinels: [0.0]

# Property with conversion
- name: dT
  type: float
  units: "Myr"
  description: "Time since previous snapshot"
  output: true
  init_source: default
  init_value: -1.0
  output_source: copy_direct
  output_convert: "UnitTime_in_s / SEC_PER_MEGAYEAR"
  range: [0.0, 2000.0]
  sentinels: [-1.0]

# Property with output modifier function
- name: infallMvir
  type: float
  units: "1e10 Msun/h"
  description: "Virial mass at infall (satellites only, 0 for centrals)"
  output: true
  init_source: default
  init_value: -1.0
  output_source: recalculate
  output_function: output_infall_property_or_zero
  output_function_arg: "g, g->infallMvir"
  range: [0.000001, 1000000.0]
  sentinels: [0.0, -1.0]
```

**Note**: For detailed guide on implementing output modifier functions, see [Output Modifier Functions](#output-modifier-functions).

### A3. Input Configuration YAML

For the complete configuration reference (all sections, fields, and a worked example with SAGE modules), see the [Configuration section in USER-GUIDE.md](USER-GUIDE.md#configuration).

**Quick reference** — top-level YAML sections:

| Section | Description |
|---------|-------------|
| `output` | Output filename, directory, format (`binary`/`hdf5`), snapshot list |
| `input` | Tree files: type, directory, file range, snapshot list file |
| `simulation` | Cosmology (Omega, h), box size, particle mass, unit conversions |
| `SubSteps` | Time sub-stepping |
| `modules` | Multi-phase pipeline: `pre_timestep`, `phase_1`, `phase_2`, `post_timestep`, `parameters` |

**Module phase format**:

```yaml
modules:
  phase_name:
    - module_name: processing_mode
  parameters:
    ParamName: value
```

### A4. ModuleContext Structure

**Definition**: `src/core/module_interface.h`

**All fields are read-only** - modules should not modify context.

**Snapshot information**:

| Field | Type | Description |
|-------|------|-------------|
| `redshift` | double | Current snapshot redshift |
| `time` | double | Cosmic time (lookback from z=0) |
| `snapshot_number` | int | Snapshot index (0 = z=127) |

**Sub-stepping information**:

| Field | Type | Description |
|-------|------|-------------|
| `substep_number` | int | Current substep (0-indexed, 0 to num_substeps-1) |
| `num_substeps` | int | Total substeps (from SubSteps config) |
| `time_interval` | double | Total time for this timestep |
| `substep_time` | double | Cosmic time at substep midpoint |
| `substep_dt` | double | **Time step for this substep** - **use for integration** |

**Halo information**:

| Field | Type | Description |
|-------|------|-------------|
| `central_index` | int | Index of Type 0 central in FoFWorkspace array |
| `central_galaxy` | struct Halo* | **Pointer to FOF central galaxy** (always non-NULL) |

**Event information** (only relevant for `process_per_event` modules):

| Field | Type | Description |
|-------|------|-------------|
| `active_event` | const struct ModuleEvent* | Active event (non-NULL in `process_per_event`; NULL otherwise). Fields: `value0`, `value1` (double payload) |

**Configuration access**:

| Field | Type | Description |
|-------|------|-------------|
| `params` | const struct MimicConfig* | Read-only access to simulation parameters |

**Usage example**:

```c
static int my_module_process(struct ModuleContext *ctx,
                              struct Halo *halos, int ngal) {
  /* Snapshot info */
  double z = ctx->redshift;
  double cosmic_time = ctx->time;
  int snap = ctx->snapshot_number;

  /* Sub-stepping info */
  double dt = ctx->substep_dt;  /* USE THIS for time integration */
  int substep = ctx->substep_number;
  int total_substeps = ctx->num_substeps;

  /* Access central galaxy */
  struct Halo *central = ctx->central_galaxy;
  double central_vvir = central->Vvir;
  double central_hot_gas = central->galaxy->HotGas;

  /* Access configuration */
  double hubble_h = ctx->params->Hubble_h;
  double omega_m = ctx->params->Omega;

  /* Physics calculations... */
  return 0;
}
```

### A5. Parameter Loading Macros

**Definition**: `src/modules/_system/parameter_helpers.h`

**Loading macros**:

| Macro | Description | Example |
|-------|-------------|---------|
| `LOAD_PARAM_DOUBLE(name, var)` | Load double parameter | `LOAD_PARAM_DOUBLE("MyParam", my_param);` |
| `LOAD_PARAM_INT(name, var)` | Load int parameter | `LOAD_PARAM_INT("MyFlag", my_flag);` |
| `LOAD_PARAM_STRING(name, var, len)` | Load string parameter | `LOAD_PARAM_STRING("MyPath", my_path, MAX_STRING_LEN);` |

**Validation macros**:

| Macro | Range | Description |
|-------|-------|-------------|
| `VALIDATE_RANGE_EXCLUSIVE(param, val, min, max, msg)` | `(min, max]` | Exclusive lower, inclusive upper |
| `VALIDATE_RANGE_INCLUSIVE(param, val, min, max, msg)` | `[min, max]` | Inclusive both bounds |
| `VALIDATE_OPTION(param, val, max, msg)` | `[0, max]` | Integer option selector |

**Combined load-and-validate macros**:

| Macro | Description |
|-------|-------------|
| `LOAD_AND_VALIDATE_RANGE_EXCLUSIVE(param, var, min, max, msg)` | Load + validate exclusive range |
| `LOAD_AND_VALIDATE_RANGE_INCLUSIVE(param, var, min, max, msg)` | Load + validate inclusive range |
| `LOAD_AND_VALIDATE_OPTION(param, var, max, msg)` | Load + validate option |

### A6. Processing Modes

**Definition**: `src/core/module_interface.h`

**Enum values**:

| Mode | Value | Description |
|------|-------|-------------|
| `PROCESSING_MODE_FULL_HALO` | 0 | Module receives full galaxy array (ngal can be 1 to 1000s) |
| `PROCESSING_MODE_PER_EVENT` | 1 | Core dispatches one emitted event target at a time (ngal = 1, `ctx->active_event` set) |
| `PROCESSING_MODE_BY_GALAXY` | 2 | Core loops over galaxies, module processes one at a time (ngal = 1) |

**When to use**:

| Mode | Best For | Cache | Vectorization |
|------|----------|-------|---------------|
| `process_full_halo` | Snapshot-level operations, array calculations | Lower | Better |
| `process_per_event` | Event-triggered physics linked to full-halo producers | Event-local | Low |
| `process_by_galaxy` | Per-galaxy physics, time integration | Better | Lower |

### A7. Pipeline Phases

For the detailed execution diagram and phase-selection guidance, see [Pipeline Phases](#pipeline-phases).

**Quick reference**:

| Phase | Runs | Has dt | Typical Use |
|-------|------|--------|-------------|
| `pre_timestep` | Once before substeps | No | Setup, budget calculation, snapshot-level physics |
| `phase_1` | Each substep | Yes (`ctx->substep_dt`) | Main baryonic physics (cooling, SF, feedback) |
| `phase_2` | Each substep | Yes (`ctx->substep_dt`) | Secondary physics (mergers, disruption) |
| `post_timestep` | Once after substeps | No | Finalization, converting accumulators to rates |

### A8. Memory Management

**Categories**:

| Constant | Description |
|----------|-------------|
| `MEM_GALAXIES` | Galaxy data structures |
| `MEM_HALOS` | Halo data structures |
| `MEM_TREES` | Merger tree structures |
| `MEM_IO` | I/O buffers |
| `MEM_UTILITY` | Utility and physics module allocations |

**Functions**:

| Function | Description |
|----------|-------------|
| `mymalloc_cat(size, category)` | Allocate with category tracking |
| `myfree(ptr)` | Free allocated memory |
| `print_allocated()` | Print total allocated memory |
| `print_allocated_by_category()` | Print memory by category |

### A9. Logging Macros

**Definition**: `src/util/error.h`

**Log levels** (from most to least verbose):

| Macro | Visibility | Use For |
|-------|-----------|---------|
| `DEBUG_LOG(fmt, ...)` | `--debug` only | Detailed debugging info (very verbose) |
| `VERBOSE_LOG(fmt, ...)` | `--verbose` or `--debug` | Configuration, initialization messages |
| `INFO_LOG(fmt, ...)` | Default level | General progress information |
| `WARNING_LOG(fmt, ...)` | Always shown | Non-fatal issues, warnings |
| `ERROR_LOG(fmt, ...)` | Always shown | Errors (non-fatal) |
| `FATAL_ERROR(fmt, ...)` | Always shown | Fatal errors (terminates program) |

### A10. Physical Constants

**Definition**: `src/modules/_system/physical_constants.h`

**CGS fundamental constants** (`#define` macros):

| Constant | Value | Units | Description |
|----------|-------|-------|-------------|
| `GRAVITY` | 6.672e-8 | cm^3/(g s^2) | Gravitational constant (CGS) |
| `SOLAR_MASS` | 1.989e33 | g | Solar mass |
| `CM_PER_MPC` | 3.085678e24 | cm | Megaparsec in cm |
| `HUBBLE` | 3.2407789e-18 | h/s | Hubble constant H_0 |
| `SEC_PER_MEGAYEAR` | 3.155e13 | s | Seconds per Myr |
| `SEC_PER_YEAR` | 3.155e7 | s | Seconds per year |
| `PROTONMASS` | 1.6726e-24 | g | Proton mass |
| `BOLTZMANN` | 1.3806e-16 | erg/K | Boltzmann constant |
| `ENERGY_SN` | 1.0e51 | erg | Canonical supernova energy |
| `ETA_SN` | 5.0e-3 | - | Supernova mass loading efficiency |

**Speed of light** (`static const double`):

| Constant | Value | Units |
|----------|-------|-------|
| `C_CGS` | 2.9979e10 | cm/s |
| `C_SQUARED_CGS` | 8.9875e20 | cm^2/s^2 |
| `C_KM_S` | 2.99792458e5 | km/s |
| `C_SQUARED_KM_S` | 8.9875e10 | (km/s)^2 |

**Derived constants** (computed at runtime, stored in `MimicConfig`):

| Field | Typical Value | Units | Description |
|-------|---------------|-------|-------------|
| `MimicConfig.G` | 43007.1 | (km/s)^2 Mpc / (1e10 Msun/h) | G in code units (computed from CGS + simulation unit system) |

---

**For additional details**:
- Architecture principles: [VISION.md](VISION.md)
- Installation and configuration: [USER-GUIDE.md](USER-GUIDE.md)
