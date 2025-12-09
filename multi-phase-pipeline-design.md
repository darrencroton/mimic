# Multi-Phase Pipeline Architecture for Mimic

**Date**: 2025-12-03 (Design Approved - Config-Driven)
**Status**: Implementation-ready specification
**Purpose**: Enable SAGE physics reproduction through multi-phase, sub-stepped execution

**Dev team leader comment**: 
- CHANGE: rename physics, merger phases to phase_1, phase_2 to avoid assuming how users will build their models
- CONSIDER: make the implementation easily extendable to add additional phases later. Doesn’t need to be fully generic now (unless that is a simple extension of the plan?) - for now keep just these 2 plus pre/post phases (follow KISS!) - just make it easy to hardcode more phases later (follow DRY!)

---

## Executive Summary

This document defines the multi-phase pipeline architecture that enables Mimic to reproduce SAGE galaxy evolution physics while maintaining all core architectural principles.

**Design Approved - Config-Driven Approach:**
- ✅ 4 execution phases (pre_timestep, physics, mergers, post_timestep)
- ✅ 2 loop modes (once, all)
- ✅ 1 function signature (simple, clean interface)
- ✅ **Configuration-driven** (pipeline defined in input YAML, not module metadata)
- ✅ Galaxy-major loop structure (matches SAGE exactly)
- ✅ Maintains physics-agnostic core
- ✅ Follows KISS and DRY principles

**Key Innovation**: Pipeline structure defined in configuration file where it belongs. Users see and control the entire execution flow. Modules are simple - they don't dictate where they run.

---

## Table of Contents

1. [Why Multi-Phase?](#why-multi-phase)
2. [Current vs Target Architecture](#current-vs-target-architecture)
3. [Final Design Specification](#final-design-specification)
4. [Configuration Format](#configuration-format)
5. [Implementation Details](#implementation-details)
6. [Module Examples](#module-examples)
7. [Testing Strategy](#testing-strategy)
8. [Implementation Roadmap](#implementation-roadmap)

---

## Why Multi-Phase?

### The Problem

Mimic's current single-phase pipeline cannot reproduce SAGE physics:

**1. No Time Sub-Stepping → Numerical Instability**
- Cooling timescales (~10 Myr) << snapshot interval (~200 Myr)
- Without substeps: attempt to cool 200 Myr of gas in one step (unphysical overshooting)
- With 20 substeps: cool 10 Myr at a time (numerically stable)

**2. No Phase Separation → Wrong Execution Order**
- Infall must be calculated once (baryon conservation)
- Physics must complete before mergers (galaxy state must be final)
- Finalization must happen after all substeps (convert accumulators to rates)

**3. No Dependency Ordering → Physics Breaks**
- Gas must be infalling before cooling
- Gas must cool before star formation
- Stars must form before feedback ejects gas

### The Solution: Multi-Phase with Sub-Stepping

```
PRE_TIMESTEP (once):
    Calculate total infalling gas (baryon budget)

SUBSTEP LOOP (20 iterations):
    PHYSICS PHASE (all galaxies, galaxy-major loop):
        for each galaxy:
            Add infalling gas → Cool → Form stars → Feedback

    MERGERS PHASE (all galaxies, galaxy-major loop):
        for each galaxy:
            Check merger criteria → Execute mergers/disruptions

POST_TIMESTEP (once):
    Convert accumulators to rates
    Calculate summary statistics
```

---

## Current vs Target Architecture

### Current: Single-Phase Pipeline

```c
void process_halo_evolution(int halonr, int ngal) {
    /* Execute all modules once */
    module_execute_pipeline(halonr, FoFWorkspace, ngal);

    update_halo_properties(ngal);
}
```

**Limitations:**
- ❌ Single pass (no sub-stepping)
- ❌ Cannot separate phases
- ❌ Cannot enforce execution order within timestep
- ❌ No pre/post processing support

### Target: Multi-Phase with Sub-Stepping

```c
void process_halo_evolution(int halonr, int ngal) {
    struct ModuleContext ctx;
    setup_module_context(&ctx, halonr, ngal);

    /* PRE_TIMESTEP: Setup (once) */
    execute_phase(config.modules.pre_timestep, &ctx, FoFWorkspace, ngal);

    /* SUBSTEP LOOP */
    for (int step = 0; step < ctx.num_substeps; step++) {
        update_context_for_substep(&ctx, step);

        /* PHYSICS: Baryonic processes */
        execute_phase(config.modules.physics, &ctx, FoFWorkspace, ngal);

        /* MERGERS: Merger/disruption */
        execute_phase(config.modules.mergers, &ctx, FoFWorkspace, ngal);
    }

    /* POST_TIMESTEP: Finalization (once) */
    execute_phase(config.modules.post_timestep, &ctx, FoFWorkspace, ngal);

    update_halo_properties(ngal);
}
```

**Capabilities:**
- ✅ Multiple phases with clear semantics
- ✅ Time sub-stepping for numerical stability
- ✅ Galaxy-major loop (matches SAGE)
- ✅ Pre/post processing support
- ✅ Physics-agnostic core
- ✅ **User-visible pipeline structure in config file**

---

## Final Design Specification

### 1. Execution Phases

```c
enum ModulePhase {
    MODULE_PHASE_PRE_TIMESTEP,   // Before substeps: setup calculations (once)
    MODULE_PHASE_PHYSICS,        // During substeps: baryonic physics
    MODULE_PHASE_MERGERS,        // During substeps: mergers/disruption
    MODULE_PHASE_POST_TIMESTEP,  // After substeps: finalization (once)
    MODULE_PHASE_COUNT
};
```

**Phase Execution Rules:**

| Phase | Frequency | Loop Modes | Purpose |
|-------|-----------|------------|---------|
| **PRE_TIMESTEP** | Once before substeps | `once` only | Setup (infall budget, reionization) |
| **PHYSICS** | Each substep | `once` or `all` | Baryonic physics (cooling, SF, feedback) |
| **MERGERS** | Each substep | `once` or `all` | Merger/disruption processing |
| **POST_TIMESTEP** | Once after substeps | `once` only | Finalization (rates, summaries) |

### 2. Loop Modes

```c
enum LoopMode {
    LOOP_MODE_ONCE,   // Module processes entire halo array at once
    LOOP_MODE_ALL,    // Core loops over all galaxies, calls module for each
    LOOP_MODE_COUNT
};
```

**Loop Mode Behavior:**

**LOOP_MODE_ONCE**: Core calls module once with full array
```c
mod->process(ctx, halos, ngal);  // ngal = 10, 100, etc.
```

**LOOP_MODE_ALL**: Core loops, calls module per-galaxy
```c
for (int g = 0; g < ngal; g++) {
    mod->process(ctx, &halos[g], 1);  // ngal = 1 always
}
```

**Galaxy-Major Execution**: When multiple modules use LOOP_MODE_ALL in a phase:
```
for each galaxy g:
    module1(galaxy g)
    module2(galaxy g)
    module3(galaxy g)
(then move to next galaxy)
```

This matches SAGE exactly and provides better cache locality.

### 3. Module Interface

**Single function signature for all modules:**

```c
struct Module {
    const char *name;

    /* Lifecycle */
    int (*init)(void);
    int (*cleanup)(void);

    /* Single processing function */
    int (*process)(struct ModuleContext *ctx, struct Halo *halos, int ngal);

    /* NOTE: No execution_phase or loop_mode here!
     * These are specified in the input configuration file.
     * Modules are simple - they just implement physics.
     */
};
```

**Key Design Decision**: Phase and loop mode are **configuration**, not module metadata. This makes modules simpler and more reusable.

### 4. Module Context

```c
struct ModuleContext {
    /* Snapshot information */
    double redshift;
    double time;
    int snapshot_number;

    /* Sub-stepping information */
    int substep_number;        // Current substep (0 to num_substeps-1)
    int num_substeps;          // Total substeps
    double time_interval;      // Age[prev] - Age[current]
    double substep_time;       // Cosmic time at this substep
    double substep_dt;         // Time interval for this substep

    /* Halo information */
    int central_index;         // Index of central halo

    /* Configuration */
    const struct MimicConfig *params;
};
```

### 5. Module Metadata Schema (Simplified!)

**No execution_phase or loop_mode needed:**

```yaml
# module_info.yaml
name: module_name
requires: [property1, property2]
provides: [property3, property4]
parameters:
  - name: param1
    type: double
    # ...
```

**Optional validation (if module has restrictions):**

```yaml
# Only if module cannot work in certain phases/modes
valid_phases: [physics, mergers]  # Optional constraint
valid_loop_modes: [all]           # Optional constraint
```

**Benefits:**
- ✅ Much simpler module metadata
- ✅ Module doesn't dictate where it runs
- ✅ Same module can be used in different phases in different configurations
- ✅ Less to maintain, less to document

---

## Configuration Format

### Input YAML Structure

**Pipeline structure defined where it belongs - in the configuration file:**

```yaml
# input/millennium.yaml

# Sub-stepping configuration
SubSteps: 20

# Pipeline definition (user-visible execution structure)
modules:
  # Phase 1: Setup (runs once before substeps)
  pre_timestep:
    - sage_reionization: once    # Set HaloBaryonFraction
    - sage_infall: once           # Calculate infall budget

  # Phase 2A: Baryonic physics (runs each substep, all galaxies)
  physics:
    - sage_reincorporation: all           # Return ejected gas
    - sage_cooling: all                   # Hot → cold gas
    - sage_starformation_feedback: all    # Form stars, eject gas
    - sage_disk_instability: all          # Check disk stability

  # Phase 2B: Mergers (runs each substep, all galaxies)
  mergers:
    - sage_mergers: all                   # Execute mergers/disruptions

  # Phase 3: Finalization (runs once after substeps)
  post_timestep:
    - sage_finalization: once             # Convert rates, summaries

# Model parameters
parameters:
  GlobalBaryonFraction: 0.17
  RadioModeEfficiency: 0.01
  # ... (20+ more parameters)
```

**Benefits of This Format:**

1. ✅ **Visibility**: Entire pipeline visible at a glance
2. ✅ **Flexibility**: Easy to reorder, disable, or experiment
3. ✅ **Control**: User has full control over execution structure
4. ✅ **Simplicity**: Configuration is in the configuration file (KISS!)
5. ✅ **DRY**: No duplication between module metadata and config

### YAML Syntax Details

**Colon notation** (recommended):
```yaml
- sage_cooling: all
- sage_infall: once
```

Each list item is a single-key dictionary, which is valid YAML and very readable.

**Alternative (more verbose but explicit):**
```yaml
- module: sage_cooling
  loop: all
- module: sage_infall
  loop: once
```

**Recommendation**: Use colon notation for brevity and clarity.

### Configuration Structure in Code

```c
struct PhaseModuleConfig {
    char *module_name;
    enum LoopMode loop_mode;  // LOOP_MODE_ONCE or LOOP_MODE_ALL
};

struct MimicConfig {
    int SubSteps;

    /* Pipeline configuration (from input YAML) */
    struct PhaseModuleConfig *pre_timestep;
    int num_pre_timestep;

    struct PhaseModuleConfig *physics;
    int num_physics;

    struct PhaseModuleConfig *mergers;
    int num_mergers;

    struct PhaseModuleConfig *post_timestep;
    int num_post_timestep;

    /* Model parameters */
    struct ModelParam *params;
    int num_params;
};
```

---

## Implementation Details

### Core Processing Function

**File**: `src/core/build_model.c`

```c
void process_halo_evolution(int halonr, int ngal) {
    int centralgal;
    struct ModuleContext ctx;

    /* Identify central halo */
    centralgal = FoFWorkspace[0].CentralHalo;
    assert(FoFWorkspace[centralgal].Type == 0 &&
           FoFWorkspace[centralgal].HaloNr == halonr);

    /* Setup context */
    setup_module_context(&ctx, halonr, centralgal, ngal);

    /* PHASE 1: Pre-timestep (once) */
    execute_phase(MimicConfig.pre_timestep, MimicConfig.num_pre_timestep,
                  &ctx, FoFWorkspace, ngal);

    /* PHASE 2: Sub-stepping loop */
    for (int step = 0; step < ctx.num_substeps; step++) {
        update_context_for_substep(&ctx, step);

        /* PHASE 2A: Physics */
        execute_phase(MimicConfig.physics, MimicConfig.num_physics,
                     &ctx, FoFWorkspace, ngal);

        /* PHASE 2B: Mergers */
        execute_phase(MimicConfig.mergers, MimicConfig.num_mergers,
                     &ctx, FoFWorkspace, ngal);
    }

    /* PHASE 3: Post-timestep (once) */
    execute_phase(MimicConfig.post_timestep, MimicConfig.num_post_timestep,
                  &ctx, FoFWorkspace, ngal);

    /* Update output structures */
    update_halo_properties(ngal);
}
```

### Phase Execution Engine

**File**: `src/core/module_registry.c`

```c
/**
 * @brief Execute all modules in a phase
 *
 * Reads module list and loop modes from configuration.
 * Implements GALAXY-MAJOR loop structure for LOOP_MODE_ALL.
 *
 * @param phase_config  Array of modules in this phase (from config)
 * @param num_modules   Number of modules in phase
 * @param ctx           Module execution context
 * @param halos         Array of halos to process
 * @param ngal          Number of halos
 */
void execute_phase(struct PhaseModuleConfig *phase_config, int num_modules,
                   struct ModuleContext *ctx,
                   struct Halo *halos, int ngal) {
    if (num_modules == 0) return;

    /* PASS 1: LOOP_MODE_ALL modules (galaxy-major loop) */
    for (int g = 0; g < ngal; g++) {
        if (halos[g].galaxy == NULL || halos[g].MergeStatus != 0) continue;

        /* Execute all LOOP_MODE_ALL modules for this galaxy */
        for (int i = 0; i < num_modules; i++) {
            if (phase_config[i].loop_mode != LOOP_MODE_ALL) continue;

            /* Find module by name */
            struct Module *mod = find_module_by_name(phase_config[i].module_name);
            if (mod == NULL) {
                ERROR_LOG("Module '%s' not found", phase_config[i].module_name);
                exit(EXIT_FAILURE);
            }

            /* Call module with single galaxy */
            int result = mod->process(ctx, &halos[g], 1);
            if (result != 0) {
                ERROR_LOG("Module '%s' failed on galaxy %d",
                         mod->name, g);
                exit(EXIT_FAILURE);
            }
        }
    }

    /* PASS 2: LOOP_MODE_ONCE modules (full array) */
    for (int i = 0; i < num_modules; i++) {
        if (phase_config[i].loop_mode != LOOP_MODE_ONCE) continue;

        /* Find module by name */
        struct Module *mod = find_module_by_name(phase_config[i].module_name);
        if (mod == NULL) {
            ERROR_LOG("Module '%s' not found", phase_config[i].module_name);
            exit(EXIT_FAILURE);
        }

        /* Call module with full array */
        int result = mod->process(ctx, halos, ngal);
        if (result != 0) {
            ERROR_LOG("Module '%s' failed", mod->name);
            exit(EXIT_FAILURE);
        }
    }
}
```

### Configuration Parsing

**File**: `src/core/init.c`

```c
/**
 * @brief Parse module phase configuration from YAML
 *
 * Parses structure like:
 *   physics:
 *     - sage_cooling: all
 *     - sage_starformation: all
 */
int parse_phase_config(yaml_node_t *phase_node,
                       struct PhaseModuleConfig **config,
                       int *num_modules) {
    if (phase_node->type != YAML_SEQUENCE_NODE) {
        ERROR_LOG("Module phase must be a sequence");
        return -1;
    }

    *num_modules = 0;
    for (yaml_node_item_t *item = phase_node->data.sequence.items.start;
         item < phase_node->data.sequence.items.top; item++) {
        (*num_modules)++;
    }

    *config = malloc(*num_modules * sizeof(struct PhaseModuleConfig));

    int idx = 0;
    for (yaml_node_item_t *item = phase_node->data.sequence.items.start;
         item < phase_node->data.sequence.items.top; item++) {

        yaml_node_t *module_node = yaml_document_get_node(doc, *item);

        /* Parse "module_name: loop_mode" */
        if (module_node->type != YAML_MAPPING_NODE) {
            ERROR_LOG("Module entry must be 'name: mode'");
            return -1;
        }

        /* Should have exactly one key-value pair */
        yaml_node_pair_t *pair = module_node->data.mapping.pairs.start;

        yaml_node_t *key = yaml_document_get_node(doc, pair->key);
        yaml_node_t *value = yaml_document_get_node(doc, pair->value);

        /* Module name is the key */
        (*config)[idx].module_name = strdup((char *)key->data.scalar.value);

        /* Loop mode is the value */
        char *loop_str = (char *)value->data.scalar.value;
        if (strcmp(loop_str, "once") == 0) {
            (*config)[idx].loop_mode = LOOP_MODE_ONCE;
        } else if (strcmp(loop_str, "all") == 0) {
            (*config)[idx].loop_mode = LOOP_MODE_ALL;
        } else {
            ERROR_LOG("Invalid loop mode '%s' (must be 'once' or 'all')",
                     loop_str);
            return -1;
        }

        idx++;
    }

    return 0;
}
```

### Context Setup

```c
static void setup_module_context(struct ModuleContext *ctx,
                                int halonr, int centralgal, int ngal) {
    int snap = InputTreeHalos[halonr].SnapNum;

    ctx->redshift = ZZ[snap];
    ctx->time = Age[snap];
    ctx->snapshot_number = snap;
    ctx->central_index = centralgal;
    ctx->params = &MimicConfig;

    /* Determine number of substeps */
    ctx->num_substeps = MimicConfig.SubSteps;

    /* Calculate time interval */
    if (FoFWorkspace[centralgal].SnapNum >= 0) {
        int prev_snap = FoFWorkspace[centralgal].SnapNum;
        ctx->time_interval = Age[prev_snap] - Age[snap];
    } else {
        ctx->time_interval = 0.0;
    }

    ctx->substep_number = 0;
    ctx->substep_time = ctx->time;
    ctx->substep_dt = (ctx->num_substeps > 0) ?
                      (ctx->time_interval / ctx->num_substeps) : 0.0;
}

static void update_context_for_substep(struct ModuleContext *ctx, int step) {
    ctx->substep_number = step;
    ctx->substep_time = ctx->time - (step + 0.5) * ctx->substep_dt;
}
```

---

## Module Examples

### Example 1: Pre-Timestep + Once

**sage_infall.c:**

```c
static double total_infalling_gas = 0.0;  // Module state

static int sage_infall_process(struct ModuleContext *ctx,
                               struct Halo *halos, int ngal) {
    /* Called with LOOP_MODE_ONCE: receives full array */
    int central = ctx->central_index;
    double tot_baryons = 0.0;

    /* Sum all baryonic mass in FOF group */
    for (int i = 0; i < ngal; i++) {
        if (halos[i].galaxy == NULL) continue;
        tot_baryons += halos[i].galaxy->StellarMass;
        tot_baryons += halos[i].galaxy->ColdGas;
        tot_baryons += halos[i].galaxy->HotGas;
        tot_baryons += halos[i].galaxy->EjectedMass;
        tot_baryons += halos[i].galaxy->BlackHoleMass;
        tot_baryons += halos[i].galaxy->ICS;
    }

    /* Calculate infalling gas: BaryonFrac × Mvir - existing baryons */
    double baryon_frac = halos[central].galaxy->HaloBaryonFraction;
    total_infalling_gas = baryon_frac * halos[central].Mvir - tot_baryons;

    /* Consolidate satellite reservoirs to central */
    consolidate_satellite_reservoirs(halos, ngal, central);

    return 0;
}

/* Accessor for physics phase modules */
double sage_infall_get_total(void) {
    return total_infalling_gas;
}
```

**module_info.yaml** (simplified - no phase/loop):
```yaml
name: sage_infall
requires: [HaloBaryonFraction, Mvir]
provides: [HotGas, MetalsHotGas, EjectedMass]
```

**Configuration** (phase/loop specified here):
```yaml
modules:
  pre_timestep:
    - sage_infall: once
```

### Example 2: Physics + All

**sage_cooling.c:**

```c
static int sage_cooling_process(struct ModuleContext *ctx,
                                struct Halo *halos, int ngal) {
    /* Called with LOOP_MODE_ALL: ngal always = 1 */
    assert(ngal == 1);
    struct Halo *halo = &halos[0];

    if (halo->galaxy == NULL) return 0;

    /* Use substep_dt for this substep */
    double dt = ctx->substep_dt;

    /* Calculate and apply cooling */
    double coolingGas = cooling_recipe(halo, dt, ctx->redshift);
    if (coolingGas > halo->galaxy->HotGas) {
        coolingGas = halo->galaxy->HotGas;
    }

    cool_gas_onto_galaxy(halo->galaxy, coolingGas);

    /* Accumulate (converted to rate in post_timestep) */
    halo->galaxy->CoolingRate += coolingGas;

    return 0;
}
```

**module_info.yaml** (simplified):
```yaml
name: sage_cooling
requires: [HotGas, MetalsHotGas, Mvir, Vvir]
provides: [ColdGas, MetalsColdGas, CoolingRate]
```

**Configuration**:
```yaml
modules:
  physics:
    - sage_cooling: all
```

### Example 3: Mergers + All

**sage_mergers.c:**

```c
static int sage_mergers_process(struct ModuleContext *ctx,
                                struct Halo *halos, int ngal) {
    /* Called with LOOP_MODE_ALL: ngal always = 1 */
    assert(ngal == 1);
    struct Halo *halo = &halos[0];

    if (halo->galaxy == NULL) return 0;
    if (halo->Type != 1 && halo->Type != 2) return 0;  // Only satellites

    /* Update merger time */
    halo->MergTime -= ctx->substep_dt;

    /* Calculate current Mvir (accounting for stripping) */
    double frac = (double)(ctx->substep_number + 1) / ctx->num_substeps;
    double currentMvir = halo->Mvir - halo->deltaMvir * frac;

    double baryons = halo->galaxy->StellarMass + halo->galaxy->ColdGas;

    /* Check disruption/merger criteria */
    bool should_disrupt = (baryons == 0.0) ||
                         (currentMvir / baryons <= ThresholdSatDisruption);

    if (should_disrupt) {
        struct Halo *central = get_central_halo();  // From module state

        if (halo->MergTime > 0.0) {
            disrupt_satellite_to_ICS(central, halo);
        } else {
            deal_with_galaxy_merger(halo, central, ctx);
        }
    }

    return 0;
}
```

**module_info.yaml**:
```yaml
name: sage_mergers
requires: [StellarMass, ColdGas, Type, MergTime, Mvir, deltaMvir]
provides: [MergeStatus, mergeIntoID]
```

**Configuration**:
```yaml
modules:
  mergers:
    - sage_mergers: all
```

### Example 4: Post-Timestep + Once

**sage_finalization.c:**

```c
static int sage_finalization_process(struct ModuleContext *ctx,
                                     struct Halo *halos, int ngal) {
    /* Called with LOOP_MODE_ONCE: receives full array */
    int central = ctx->central_index;
    double total_sat_baryons = 0.0;

    /* Convert accumulators to rates */
    for (int i = 0; i < ngal; i++) {
        if (halos[i].galaxy == NULL) continue;

        /* Accumulated over all substeps, divide by total time */
        halos[i].galaxy->CoolingRate /= ctx->time_interval;
        halos[i].galaxy->HeatingRate /= ctx->time_interval;
        halos[i].galaxy->OutflowRate /= ctx->time_interval;

        /* Sum satellite baryons */
        if (i != central) {
            total_sat_baryons += halos[i].galaxy->StellarMass +
                                halos[i].galaxy->ColdGas +
                                halos[i].galaxy->HotGas +
                                halos[i].galaxy->BlackHoleMass;
        }
    }

    halos[central].galaxy->TotalSatelliteBaryons = total_sat_baryons;

    return 0;
}
```

**module_info.yaml**:
```yaml
name: sage_finalization
requires: [CoolingRate, HeatingRate, OutflowRate]
provides: [CoolingRate, HeatingRate, OutflowRate, TotalSatelliteBaryons]
```

**Configuration**:
```yaml
modules:
  post_timestep:
    - sage_finalization: once
```

---

## Testing Strategy

### Unit Tests

```c
// Test phase execution order
void test_phase_execution_order() {
    // Mock modules in config
    // Verify: pre → (physics, mergers) × N → post
}

// Test galaxy-major loop structure
void test_galaxy_major_loop() {
    // Config with multiple "all" modules in physics
    // Verify: mod1(gal0), mod2(gal0), mod1(gal1), mod2(gal1), ...
}

// Test substep context
void test_substep_context() {
    // Verify: substep_number, substep_time, substep_dt correct
}

// Test configuration parsing
void test_config_parsing() {
    // Parse YAML with phase structure
    // Verify: modules and loop modes parsed correctly
}
```

### Integration Tests

```python
def test_sage_infall_pre_timestep():
    """Verify infall runs once (not each substep)"""
    # Config: sage_infall: once in pre_timestep
    # Track execution count
    # Assert: called exactly 1 time

def test_sage_cooling_substeps():
    """Verify cooling runs each substep per galaxy"""
    # Config: sage_cooling: all in physics, SubSteps=10
    # Track execution count per galaxy
    # Assert: each galaxy processed exactly 10 times

def test_physics_before_mergers():
    """Verify physics completes before mergers"""
    # Config with both physics and mergers phases
    # Track execution order
    # Assert: all physics calls complete before any merger calls

def test_galaxy_major_ordering():
    """Verify modules execute in galaxy-major order"""
    # Config: multiple "all" modules in physics
    # Track (module, galaxy) execution order
    # Assert: (cooling,0), (SF,0), (cooling,1), (SF,1), ...

def test_reorder_modules():
    """Verify user can reorder modules in config"""
    # Config 1: cooling, then SF
    # Config 2: SF, then cooling
    # Assert: different results (order matters)
```

### Scientific Validation

```python
def test_sage_cooling_stability():
    """Verify SubSteps=20 more stable than SubSteps=1"""
    # Run with SubSteps=1 vs SubSteps=20
    # Compare cooled gas amounts
    # Assert: SubSteps=20 more stable

def test_sage_baryon_conservation():
    """Verify baryons conserved across sub-steps"""
    # Track total baryons before and after
    # Assert: final = initial + infall (within tolerance)

def test_sage_bit_identical():
    """Compare with SAGE output (bit-identical goal)"""
    # Run with SAGE configuration
    # Compare all output properties
    # Assert: bit-identical (or document differences)
```

---

## Implementation Roadmap

### Phase 1: Infrastructure (Weeks 1-2)

**Tasks:**
1. Extend `module_interface.h`: Add enums, extend ModuleContext
2. Simplify `Module` struct: Remove phase/loop fields (config-driven!)
3. Add config structures: `PhaseModuleConfig`, phase arrays in `MimicConfig`
4. Implement config parsing: Parse phase structure from input YAML
5. Update module metadata: Remove `execution_phase`, `loop_mode` fields

**Testing**: Unit tests for config parsing, struct initialization

**Deliverable**: Infrastructure in place, config-driven

---

### Phase 2: Core Processing (Weeks 3-4)

**Tasks:**
1. Refactor `process_halo_evolution()`: Add phase structure with substep loop
2. Implement `execute_phase()`: Takes config array, implements galaxy-major loop
3. Implement `setup_module_context()`: Substep calculation
4. Add validation: Check module exists, optional phase/loop constraints

**Testing**: Unit tests, mock modules in config

**Deliverable**: Multi-phase pipeline operational

---

### Phase 3: SAGE Module Migration (Weeks 5-8)

**Tasks:**
1. Simplify all SAGE module metadata: Remove phase/loop fields
2. Create SAGE configuration file: Define full pipeline structure
3. Update module code for substep awareness where needed
4. Test module interactions

**Testing**: Integration tests, SAGE comparison

**Deliverable**: All SAGE modules using config-driven system

---

### Phase 4: Validation & Optimization (Weeks 9-12)

**Tasks:**
1. Scientific validation (bit-identical with SAGE)
2. Baryon conservation verification
3. Numerical stability tests
4. Performance profiling and optimization
5. Complete documentation

**Testing**: Full scientific validation suite

**Deliverable**: Production-ready, validated system

---

## Complete Execution Flow

```
process_halo_evolution(halonr=42, ngal=10)
  │
  ├─ setup_module_context(&ctx, ...)
  │   ├─ ctx.num_substeps = 20 (from config.SubSteps)
  │   ├─ ctx.time_interval = Age[prev] - Age[curr]
  │   └─ ctx.substep_dt = time_interval / 20
  │
  ├─ PRE_TIMESTEP (once) - from config.pre_timestep
  │  ├─ sage_reionization: once
  │  │   └─ Called with (halos[0..9], 10)
  │  └─ sage_infall: once
  │      └─ Called with (halos[0..9], 10)
  │          └─ Calculate total_infalling_gas
  │
  ├─ for (step = 0; step < 20; step++)
  │  │
  │  ├─ update_context_for_substep(&ctx, step)
  │  │
  │  ├─ PHYSICS - from config.physics
  │  │  └─ Galaxy-major loop for "all" modules:
  │  │     ├─ for galaxy 0:
  │  │     │  ├─ sage_cooling: all → (&halos[0], 1)
  │  │     │  ├─ sage_starformation: all → (&halos[0], 1)
  │  │     │  ├─ sage_feedback: all → (&halos[0], 1)
  │  │     │  └─ sage_disk_instability: all → (&halos[0], 1)
  │  │     │
  │  │     ├─ for galaxy 1:
  │  │     │  ├─ sage_cooling: all → (&halos[1], 1)
  │  │     │  └─ ... (all physics modules)
  │  │     │
  │  │     └─ ... (for all 10 galaxies)
  │  │
  │  └─ MERGERS - from config.mergers
  │     └─ Galaxy-major loop for "all" modules:
  │        ├─ for galaxy 0:
  │        │  └─ sage_mergers: all → (&halos[0], 1)
  │        ├─ for galaxy 1:
  │        │  └─ sage_mergers: all → (&halos[1], 1)
  │        └─ ... (for all 10 galaxies)
  │
  └─ POST_TIMESTEP (once) - from config.post_timestep
     └─ sage_finalization: once
         └─ Called with (halos[0..9], 10)
             ├─ Convert accumulators → rates
             └─ Calculate TotalSatelliteBaryons
```

---

## Quick Reference

### Configuration Template

```yaml
SubSteps: 20

modules:
  pre_timestep:
    - module_name: once

  physics:
    - module_name: all
    - module_name: all

  mergers:
    - module_name: all

  post_timestep:
    - module_name: once

parameters:
  # Model parameters...
```

### Module Configuration Patterns

| Pattern | Syntax | Execution |
|---------|--------|-----------|
| **Pre-timestep setup** | `- sage_infall: once` | 1× before substeps |
| **Per-galaxy physics** | `- sage_cooling: all` | 20× per galaxy (SubSteps=20) |
| **Per-galaxy mergers** | `- sage_mergers: all` | 20× per galaxy |
| **Post-timestep finalize** | `- sage_finalization: once` | 1× after substeps |

### SAGE Complete Configuration

```yaml
SubSteps: 20

modules:
  pre_timestep:
    - sage_reionization: once
    - sage_infall: once

  physics:
    - sage_cooling: all
    - sage_starformation_feedback: all
    - sage_reincorporation: all
    - sage_disk_instability: all

  mergers:
    - sage_mergers: all

  post_timestep:
    - sage_finalization: once

parameters:
  GlobalBaryonFraction: 0.17
  # ... (20+ more)
```

### Key Design Decisions

1. **Configuration-Driven**: Pipeline structure in input YAML (not module metadata)
2. **4 Phases**: Clear semantics (pre, physics, mergers, post)
3. **2 Loop Modes**: Simple (once, all)
4. **1 Function Signature**: Clean interface
5. **Galaxy-Major Loop**: Matches SAGE, better cache locality
6. **User Control**: Full visibility and control over execution

### Why This Design is Superior

| Aspect | Metadata-Driven | Config-Driven (This Design) |
|--------|----------------|---------------------------|
| **Visibility** | Scattered | ✅ All in one file |
| **Flexibility** | Fixed by module | ✅ User controls |
| **Simplicity** | Complex metadata | ✅ Simple metadata |
| **User Control** | Limited | ✅ Full control |
| **Experimentation** | Change code | ✅ Edit config |
| **KISS** | Configuration split | ✅ Config in config |
| **DRY** | Duplication possible | ✅ Single source |

---

**End of Document**

This specification is complete and ready for implementation. The config-driven approach provides superior flexibility and user control while maintaining simplicity.
