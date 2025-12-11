# Multi-Phase Pipeline Migration Guide

**Date**: 2025-12-09
**Status**: Migration complete as of commit 58d51e4
**Purpose**: Guide for updating code, tests, and configurations to use the new multi-phase pipeline architecture

---

## Overview

Mimic has migrated from a single-phase module execution pipeline to a configurable multi-phase pipeline with time sub-stepping support. This change enables:
- Better numerical stability through time sub-stepping
- Clear separation of physics phases (setup, main physics, mergers, finalization)
- Runtime-configurable execution order
- Galaxy-major loop structure for better cache locality

**This is a breaking change with no backwards compatibility.**

---

## Configuration Format Changes

### Old Format (DEPRECATED - Removed)

```yaml
modules:
  enabled: [module1, module2, module3]
  parameters:
    SomeParameter: 1.0
```

### New Format (Current)

```yaml
# Time sub-stepping
SubSteps: 20  # Number of substeps per snapshot (1 = no substeps)

modules:
  # Phase 1: Setup (runs once before substeps)
  pre_timestep:
    - sage_reionization: once
    - sage_calculate_infall: once

  # Phase 2: Main physics (runs each substep for each galaxy)
  phase_1:
    - sage_cooling: all
    - sage_starformation_feedback: all

  # Phase 3: Mergers/disruption (runs each substep for each galaxy)
  phase_2:
    - sage_mergers: all

  # Phase 4: Finalization (runs once after substeps)
  post_timestep:
    - sage_finalization: once

  # Model parameters (unchanged)
  parameters:
    GlobalBaryonFraction: 0.17
    # ...
```

**Key Changes**:
1. ✅ `enabled` list → 4 phase-based lists (`pre_timestep`, `phase_1`, `phase_2`, `post_timestep`)
2. ✅ Each module entry specifies loop mode (`once` or `all`)
3. ✅ Added `SubSteps` parameter for time sub-stepping

---

## Code Changes

### Old Code (DEPRECATED - Removed)

```c
// OLD: Module configuration
MimicConfig.EnabledModules[0] = "my_module";
MimicConfig.NumEnabledModules = 1;

// OLD: Module struct
struct Module {
    const char *name;
    int (*init)(void);
    int (*process)(struct Halo *halos, int ngal);  // No context
    int (*cleanup)(void);
};
```

### New Code (Current)

```c
// NEW: Multi-phase configuration
MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
MimicConfig.phase_1[0].module_name = strdup("my_module");
MimicConfig.phase_1[0].loop_mode = LOOP_MODE_ALL;
MimicConfig.num_phase_1 = 1;
MimicConfig.SubSteps = 1;

// NEW: Module struct with context
struct Module {
    const char *name;
    int (*init)(void);
    int (*process)(struct ModuleContext *ctx, struct Halo *halos, int ngal);  // With context!
    int (*cleanup)(void);
};
```

**Key Changes**:
1. ✅ Configuration uses phase arrays instead of `EnabledModules`
2. ✅ Module `process()` function receives `ModuleContext *ctx` parameter
3. ✅ Context provides substep information (substep_number, substep_dt, etc.)

---

## Testing Changes

### Unit Tests

**Old**:
```c
strcpy(MimicConfig.EnabledModules[0], "test_fixture");
MimicConfig.NumEnabledModules = 1;
```

**New**:
```c
MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
MimicConfig.phase_1[0].module_name = strdup("test_fixture");
MimicConfig.phase_1[0].loop_mode = LOOP_MODE_ALL;
MimicConfig.num_phase_1 = 1;
MimicConfig.SubSteps = 1;
```

### Integration Tests (Python)

**Old (DEPRECATED)**:
```python
param_file, output_dir, temp_dir = create_test_param_file(
    output_name="test",
    enabled_modules=["module1", "module2"]  # OLD
)
```

**New (Preferred)**:
```python
param_file, output_dir, temp_dir = create_test_param_file(
    output_name="test",
    phase_config={
        'pre_timestep': [('sage_reionization', 'once')],
        'phase_1': [('sage_cooling', 'all'), ('sage_starformation', 'all')],
        'phase_2': [],
        'post_timestep': []
    },
    model_params={
        "GlobalBaryonFraction": 0.17
    }
)
```

**Backward Compatibility**: The old `enabled_modules` parameter still works (puts all modules in `phase_1` with `loop_mode=all`) but issues a deprecation warning.

---

## Module Development

### Module Process Function Signature

**Old**:
```c
static int my_module_process(struct Halo *halos, int ngal) {
    // No access to substep information
    double dt = ???;  // How to get timestep?
}
```

**New**:
```c
static int my_module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    // Access to all execution context
    double dt = ctx->substep_dt;              // Time for this substep
    int substep = ctx->substep_number;        // Current substep (0-indexed)
    int total_substeps = ctx->num_substeps;   // Total substeps
    double z = ctx->redshift;                 // Current redshift
    double time_interval = ctx->time_interval; // Total time for snapshot

    // Module receives either:
    // - ngal=1 (if loop_mode=all) - process single galaxy
    // - ngal>1 (if loop_mode=once) - process full array
}
```

**ModuleContext Fields**:
- `redshift`, `time`, `snapshot_number` - Snapshot information
- `substep_number`, `num_substeps` - Substep tracking
- `substep_dt`, `time_interval`, `substep_time` - Time information
- `central_index` - Index of central galaxy in array
- `params` - Pointer to full configuration

---

## Loop Modes

### LOOP_MODE_ONCE
- Core calls module **once** with full halo array
- Module receives `ngal` = full array size (e.g., 10, 100)
- Module processes entire array at once
- **Use for**: Array-level operations, setup calculations

### LOOP_MODE_ALL
- Core loops over galaxies, calls module per-galaxy
- Module receives `ngal` = 1 always
- Executed in **galaxy-major order** for better cache locality
- **Use for**: Per-galaxy physics (cooling, star formation, etc.)

**Galaxy-Major Example**:
```
Config:
  phase_1:
    - cooling: all
    - starformation: all

Execution:
  for each galaxy g:
    cooling(galaxy g)        # Process galaxy g
    starformation(galaxy g)  # Same galaxy g
  (then move to next galaxy)
```

---

## Execution Phases

### Phase Semantics

| Phase | Frequency | Typical Use |
|-------|-----------|-------------|
| **pre_timestep** | Once before substeps | Setup calculations (reionization, infall budget) |
| **phase_1** | Each substep | Main baryonic physics (cooling, SF, feedback) |
| **phase_2** | Each substep | Mergers/disruption physics |
| **post_timestep** | Once after substeps | Finalization (convert accumulators to rates) |

**Execution Flow**:
```
process_halo_evolution():
  execute_phase(pre_timestep)           # Once

  for step in range(SubSteps):
    execute_phase(phase_1)              # Each substep
    execute_phase(phase_2)              # Each substep

  execute_phase(post_timestep)          # Once
```

---

## Common Migration Patterns

### Pattern 1: Simple Module → Phase_1

**Before**:
```yaml
modules:
  enabled: [my_module]
```

**After**:
```yaml
modules:
  pre_timestep: []
  phase_1:
    - my_module: all
  phase_2: []
  post_timestep: []
```

### Pattern 2: Setup + Physics → Multi-Phase

**Before**: All in one phase, no clear separation
**After**:
```yaml
modules:
  pre_timestep:
    - setup_module: once
  phase_1:
    - physics_module: all
  phase_2: []
  post_timestep:
    - finalize_module: once
```

### Pattern 3: Time Sub-Stepping

**Before**: Single timestep per snapshot
**After**:
```yaml
SubSteps: 20  # 20 substeps for numerical stability

modules:
  phase_1:
    - cooling: all  # Runs 20 times per galaxy per snapshot
```

---

## Migration Checklist

### For Users
- [ ] Update all YAML parameter files to new multi-phase format
- [ ] Add `SubSteps` parameter (start with 1, tune as needed)
- [ ] Assign modules to appropriate phases
- [ ] Specify loop mode (`once` or `all`) for each module
- [ ] Test configuration produces expected results

### For Developers
- [ ] Update module `process()` signature to accept `ModuleContext *ctx`
- [ ] Update all test code to use new phase configuration
- [ ] Update integration tests to use `phase_config` parameter
- [ ] Update documentation examples
- [ ] Verify module works in different phases/loop modes

### For Test Writers
- [ ] Update C unit tests: use phase arrays instead of `EnabledModules`
- [ ] Update Python integration tests: use `phase_config` instead of `enabled_modules`
- [ ] Update test YAML files to new format
- [ ] Verify tests pass with new configuration

---

## Troubleshooting

### Error: "Module 'X' configured but not registered"
**Cause**: Module name in YAML doesn't match registered name
**Fix**: Check module_info.yaml `name` field matches YAML exactly

### Error: "Module 'X' failed on galaxy Y"
**Cause**: Module doesn't handle `ngal=1` correctly (if `loop_mode=all`)
**Fix**: Verify module processes single galaxy when `ngal=1`

### Warning: "enabled_modules parameter is deprecated"
**Cause**: Using old `enabled_modules` in `create_test_param_file()`
**Fix**: Switch to `phase_config` parameter

### Unexpected execution order
**Cause**: Modules in wrong phase
**Fix**: Review phase semantics and assign modules appropriately

---

## References

- **Design Document**: `multi-phase-pipeline-design.md`
- **Implementation**: Commit 58d51e4
- **Module Developer Guide**: `docs/developer/module-developer-guide.md`
- **User Configuration Guide**: `docs/user/module-configuration.md`
- **Test Framework**: `tests/framework/harness.py`

---

## Summary

The multi-phase pipeline is a **clean break** from the old architecture:
- ✅ Better numerical stability (time sub-stepping)
- ✅ Clearer phase separation
- ✅ Runtime-configurable execution
- ✅ Galaxy-major loop optimization
- ✅ No backwards compatibility baggage

**Migration is straightforward**: Update YAML files to new format, update module signatures to accept context, update tests to use new configuration structure.
