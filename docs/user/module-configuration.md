# Module Configuration Guide

**Audience**: Users configuring Mimic for scientific runs
**Prerequisites**: Basic understanding of YAML parameter files

## Overview

Mimic's modular architecture allows you to enable/disable galaxy physics modules and configure model parameters at runtime without recompilation. This guide explains how to configure modules and physics parameters via the YAML parameter file.

**Key Concepts:**
- **Modules**: Physics components (e.g., sage_cooling, sage_starformation_feedback)
- **Model Parameters**: Physics parameters used by modules (e.g., GlobalBaryonFraction, SfrEfficiency)
- **Decentralized Definitions**: Each module defines its needed parameters in its own `module_info.yaml`
- **No Defaults**: All parameters MUST be specified - ensures reproducible science

## Multi-Phase Pipeline Configuration

### Overview

Mimic uses a **multi-phase pipeline** architecture that organizes module execution into four distinct phases with optional time sub-stepping. This provides fine-grained control over physics execution order and numerical stability.

### The Four Execution Phases

```yaml
modules:
  # Pre-timestep: Setup calculations (runs once before substeps)
  pre_timestep:
    - sage_reionization: once
    - sage_infall: once

  # Phase 1: Main physics (runs each substep for each galaxy)
  phase_1:
    - sage_cooling: all
    - sage_starformation_feedback: all

  # Phase 2: Secondary physics (runs each substep for each galaxy)
  phase_2:
    - sage_mergers: all

  # Post-timestep: Finalization (runs once after substeps)
  post_timestep:
    - sage_finalization: once

  parameters:
    # Model parameters go here (see Model Parameters section)
```

**Phase Execution Order:**
1. **pre_timestep**: Runs once before time sub-stepping begins
   - Use for: Setup calculations, budget calculations, snapshot-level operations
   - Example: Calculate reionization suppression, total infall budget

2. **phase_1**: Runs each substep for each galaxy (or galaxy group)
   - Use for: Main baryonic physics that needs time integration
   - Example: Cooling, star formation, feedback, reincorporation

3. **phase_2**: Runs each substep for each galaxy (or galaxy group)
   - Use for: Secondary physics, typically following phase_1
   - Example: Galaxy mergers, disruption, satellite tracking

4. **post_timestep**: Runs once after all substeps complete
   - Use for: Finalization, converting accumulators to rates
   - Example: Calculate average star formation rates, finalize statistics

### Time Sub-Stepping

Control numerical stability with the `SubSteps` parameter (top-level, outside `modules:`):

```yaml
# Time sub-stepping (top-level parameter)
SubSteps: 1  # Number of substeps per snapshot (1 = no substeps, 20 = SAGE-like)

modules:
  pre_timestep:
    # ...
```

**SubSteps Behavior:**
- `SubSteps: 1` (default): No sub-stepping, phase_1 and phase_2 run once
- `SubSteps: 20`: SAGE-like behavior, phase_1 and phase_2 run 20 times with dt = time_interval/20
- Modules in pre_timestep and post_timestep always run once regardless of SubSteps

### Loop Modes

Each module can run in one of two loop modes:

- **`once`**: Module processes the entire galaxy array at once (ngal = full array size)
  - Use for: Operations that need access to all galaxies simultaneously
  - Example: Snapshot-level calculations, setup operations
  - Performance: Better for vectorized operations

- **`all`**: Core loops over galaxies, module processes one at a time (ngal = 1)
  - Use for: Galaxy-specific physics, time integration
  - Example: Cooling, star formation, feedback
  - Performance: Better cache locality, matches SAGE behavior

**Galaxy-Major Loop:**
When multiple modules use `loop_mode: all` in the same phase, they execute in galaxy-major order:
```
for each galaxy g:
  module1(galaxy g)
  module2(galaxy g)
  module3(galaxy g)
```

This provides better cache locality and matches SAGE execution behavior.

### Physics-Free Mode

To run without any physics modules (halo tracking only):

```yaml
modules:
  pre_timestep: []
  phase_1: []
  phase_2: []
  post_timestep: []

  parameters: {}  # No parameters needed
```

Or simply omit modules that you don't need:

```yaml
modules:
  pre_timestep:
    - sage_reionization: once
  phase_1: []     # Empty phase
  phase_2: []     # Empty phase
  post_timestep: []

  parameters:
    GlobalBaryonFraction: 0.17
```

### Adding New Phases (Advanced)

The multi-phase pipeline is designed to be extensible. While Mimic currently supports 4 phases (pre_timestep, phase_1, phase_2, post_timestep), adding new phases is straightforward for developers who need finer-grained control over execution order.

**To add phase_3, phase_4, etc., modify these files:**

1. **src/core/module_interface.h**
   - Add enum values to `enum ModulePhase`:
     ```c
     enum ModulePhase {
       MODULE_PHASE_PRE_TIMESTEP,
       MODULE_PHASE_1,
       MODULE_PHASE_2,
       MODULE_PHASE_3,        // Add this
       MODULE_PHASE_POST_TIMESTEP,
       MODULE_PHASE_COUNT
     };
     ```

2. **src/include/types.h**
   - Add configuration fields to `struct MimicConfig`:
     ```c
     struct PhaseModuleConfig *phase_3;
     int num_phase_3;
     ```

3. **src/core/build_model.c**
   - Add execution call in `process_halo_evolution()`:
     ```c
     /* PHASE 3 */
     execute_phase(MimicConfig.phase_3, MimicConfig.num_phase_3,
                   &ctx, FoFWorkspace, ngal);
     ```
   - Add phase_3 to the substep loop (between phase_2 and post_timestep)

4. **src/core/read_parameter_file.c**
   - Add parsing in `parse_modules_section()`:
     ```c
     parse_phase_config(doc, phase_node, &MimicConfig.phase_3,
                       &MimicConfig.num_phase_3, "phase_3");
     ```

5. **src/core/module_registry.c** (**CRITICAL: Memory cleanup**)
   - Add cleanup in `module_system_cleanup()` BEFORE the final "Module system cleanup complete" message:
     ```c
     // Free phase_3 configuration array and module name strings
     if (MimicConfig.phase_3) {
       for (int i = 0; i < MimicConfig.num_phase_3; i++) {
         if (MimicConfig.phase_3[i].module_name) {
           free((void *)MimicConfig.phase_3[i].module_name);
         }
       }
       myfree(MimicConfig.phase_3);
       MimicConfig.phase_3 = NULL;
     }
     ```
   - **Important**: Module names are allocated with `strdup()` during parsing, so they must be freed with `free()`, not `myfree()`
   - **Important**: The phase array itself is allocated with `mymalloc_cat()`, so it must be freed with `myfree()`
   - **Important**: Forgetting this step causes memory leaks (2 blocks per new phase)

6. **src/io/output/hdf5.c** (if using HDF5 output)
   - Add phase_3 collection in `write_enabled_modules()`:
     ```c
     for (int i = 0; i < MimicConfig.num_phase_3; i++) {
       add_unique_module(module_list, &num_modules,
                         MimicConfig.phase_3[i].module_name);
     }
     ```

7. **input/millennium.yaml** (example configuration)
   - Add phase_3 section:
     ```yaml
     modules:
       pre_timestep:
         - ...
       phase_1:
         - ...
       phase_2:
         - ...
       phase_3: []   # New phase
       post_timestep:
         - ...
     ```

**Design Philosophy:**
- Generic phase names (phase_1, phase_2, etc.) avoid assuming specific physics
- Configuration-driven approach allows maximum flexibility
- Each phase can have its own mix of loop_mode: once and loop_mode: all modules
- Phases inside the substep loop (phase_1, phase_2, etc.) run each substep
- Phases outside the loop (pre_timestep, post_timestep) run once per snapshot

**When to add a new phase:**
- Physics requires distinct execution ordering beyond current phases
- Circular dependencies between modules require separation
- Performance optimization benefits from specific execution ordering
- Different time integration requirements for different physics

**Note**: Most physics models work well with the current 4 phases. Only add new phases if you have a compelling scientific or technical reason.

## Model Parameters

**IMPORTANT**: All physics parameters are centralized in the `modules.parameters:` section of your YAML configuration file. Parameters are REQUIRED based on which modules you enable - no defaults are used.

### Smart Parameter Validation

Mimic uses **smart validation** for reproducible science:
- Only parameters needed by your enabled modules are required
- Physics-free mode (no modules) requires NO parameters
- Your input file defines the complete physics model for your configuration
- No hidden defaults - all assumptions are explicit
- Different runs can be compared by comparing parameter files
- Parameter definitions are in each module's `module_info.yaml` file

### Parameter Categories

The 20 model parameters are organized into scientific categories:

1. **Cosmological Parameters** (1): GlobalBaryonFraction
2. **Cooling & AGN Feedback** (3): RadioModeEfficiency, AGNrecipeOn, CoolFunctionsDir
3. **Star Formation** (4): SFprescription, SfrEfficiency, EnergySNcode, EtaSNcode
4. **Stellar Feedback** (3): SupernovaRecipeOn, FeedbackReheatingEpsilon, FeedbackEjectionEfficiency
5. **Stellar Evolution** (3): RecycleFraction, Yield, FracZleaveDisk
6. **Gas Reincorporation** (1): ReIncorporationFactor
7. **Galaxy Mergers** (3): BlackHoleGrowthRate, QuasarModeEfficiency, ThreshMajorMerger
8. **Disk Instability** (2): DiskInstabilityOn, DiskRadiusFactor

### YAML Configuration Format

```yaml
# Parameters needed by enabled modules (values shown are SAGE defaults)
modules.parameters:
  # Cosmological Parameters
  GlobalBaryonFraction: 0.17

  # Cooling & AGN Feedback
  RadioModeEfficiency: 0.01
  AGNrecipeOn: 1
  CoolFunctionsDir: "input/CoolFunctions"

  # Star Formation
  SFprescription: 0
  SfrEfficiency: 0.02
  EnergySNcode: 1.0
  EtaSNcode: 0.5

  # Stellar Feedback
  SupernovaRecipeOn: 1
  FeedbackReheatingEpsilon: 3.0
  FeedbackEjectionEfficiency: 0.3

  # Stellar Evolution
  RecycleFraction: 0.43
  Yield: 0.03
  FracZleaveDisk: 0.3

  # Gas Reincorporation
  ReIncorporationFactor: 1.0

  # Galaxy Mergers
  BlackHoleGrowthRate: 0.01
  QuasarModeEfficiency: 0.001
  ThreshMajorMerger: 0.3

  # Disk Instability
  DiskInstabilityOn: 1
  DiskRadiusFactor: 3.0
```

**Complete Example**: See `input/millennium.yaml` for a working configuration file.

**Parameter Descriptions**: Each module documents its required parameters in its `module_info.yaml` file (located in `src/modules/<module_name>/`). These files specify parameter types, units, valid ranges, and scientific meaning.

## Available Modules

**Note**: All physics parameters are centralized in the `modules.parameters:` section (see above). Modules are enabled via `modules.enabled:` list.

### SAGE Physics Modules

The following modules implement the SAGE (Semi-Analytic Galaxy Evolution) model:

#### sage_reionization

**Purpose**: Calculate local baryon fraction with reionization suppression

**Physics**:
- Sets HaloBaryonFraction = GlobalBaryonFraction × f_reion(Mvir, z)
- Reionization suppression following Gnedin (2000) with Kravtsov et al. (2004) fitting formulas
- Suppresses gas accretion onto low-mass halos after cosmic reionization

**Dependencies**: None

**Provides**: HaloBaryonFraction (local baryon fraction for each halo)

**Recommended Phase**: pre_timestep (runs once, before other modules need HaloBaryonFraction)

**Loop Mode**: once (calculates property for all halos in one call)

**Model Parameters Used**: GlobalBaryonFraction

**Implementation Notes**:
- Reionization parameters (z0=8.0, zr=7.0, alpha=6.0) are currently hardcoded
- These match SAGE default values for Millennium simulation

---

#### sage_infall

**Purpose**: Cosmological gas infall onto central galaxies

**Physics**:
- Gas infall: infallingMass = HaloBaryonFraction × Mvir - (total baryons)
- Uses HaloBaryonFraction property (set by sage_reionization module)

**Dependencies**: Requires sage_reionization (provides HaloBaryonFraction)

**Provides**: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS

**Recommended Phase**: pre_timestep (calculates total infall budget for timestep)

**Loop Mode**: once (budget calculation for all halos)

**Model Parameters Used**: None (uses HaloBaryonFraction property)

---

#### sage_satellite_stripping

**Purpose**: Environmental gas stripping for satellite galaxies

**Physics**: Removes hot gas from satellites in massive halos using local baryon fraction

**Dependencies**: Requires sage_reionization (provides HaloBaryonFraction) and sage_infall (provides hot gas reservoir)

**Provides**: Updates HotGas, MetalsHotGas for satellites

**Recommended Phase**: phase_1 (runs each substep for time integration)

**Loop Mode**: all (processes each satellite galaxy individually)

**Model Parameters Used**: None (uses HaloBaryonFraction property)

---

#### sage_cooling

**Purpose**: Gas cooling from hot halo to cold disk

**Physics**: Radiative cooling with AGN feedback suppression

**Dependencies**: Requires hot gas (from sage_infall)

**Provides**: ColdGas, MetalsColdGas

**Recommended Phase**: phase_1 (runs each substep for time integration)

**Loop Mode**: all (processes each galaxy individually)

**Model Parameters Used**: RadioModeEfficiency, AGNrecipeOn, CoolFunctionsDir

---

#### sage_starformation_feedback

**Purpose**: Star formation and supernova feedback

**Physics**: Converts cold gas to stars, reheats/ejects gas via supernovae

**Dependencies**: Requires ColdGas (from sage_cooling)

**Provides**: StellarMass, MetalsStellarMass (updates HotGas, EjectedMass)

**Recommended Phase**: phase_1 (runs each substep for time integration, after cooling)

**Loop Mode**: all (processes each galaxy individually)

**Model Parameters Used**: SFprescription, SfrEfficiency, EnergySNcode, EtaSNcode, SupernovaRecipeOn, FeedbackReheatingEpsilon, FeedbackEjectionEfficiency, RecycleFraction, Yield, FracZleaveDisk

---

#### sage_reincorporation

**Purpose**: Reincorporation of ejected gas back to hot halo

**Physics**: Time-dependent return of ejected baryons

**Dependencies**: Requires EjectedMass (from sage_starformation_feedback)

**Provides**: Updates HotGas, EjectedMass

**Recommended Phase**: phase_1 (runs each substep for time integration)

**Loop Mode**: all (processes each galaxy individually)

**Model Parameters Used**: ReIncorporationFactor

---

#### sage_mergers

**Purpose**: Galaxy mergers and black hole growth

**Physics**: Handles major/minor mergers, BH accretion, quasar feedback

**Dependencies**: Tree structure (galaxy hierarchies)

**Provides**: BlackHoleMass (triggers bulge formation, quasar feedback)

**Recommended Phase**: phase_2 (typically runs after main baryonic physics)

**Loop Mode**: all (processes each merger event individually)

**Model Parameters Used**: BlackHoleGrowthRate, QuasarModeEfficiency, ThreshMajorMerger

---

#### sage_disk_instability

**Purpose**: Disk instability and bulge formation

**Physics**: Converts unstable disk stars to bulge

**Dependencies**: Requires StellarMass

**Provides**: BulgeMass, DiskMass

**Recommended Phase**: phase_1 or phase_2 (flexible, depends on model design)

**Loop Mode**: all (processes each galaxy individually)

**Model Parameters Used**: DiskInstabilityOn, DiskRadiusFactor

---

### Legacy Test Modules (Archived)

The following modules were used for infrastructure testing and are now archived:

- **simple_cooling**: Placeholder cooling (archived to `src/modules/_archive/`)
- **simple_sfr**: Placeholder star formation (archived to `src/modules/_archive/`)

**Note**: For production runs, use the SAGE modules above.

---

## Complete Example Configuration

See `input/millennium.yaml` for a complete working configuration file. Key sections:

```yaml
# Model Parameters - Only those needed by enabled modules required
modules.parameters:
  GlobalBaryonFraction: 0.17
  RadioModeEfficiency: 0.01
  # ... (see input/millennium.yaml for complete list)

# Output Configuration
output:
  output_filename: model
  output_directory: ./output/results/millennium/
  output_format: hdf5
  snapshot_list: [63, 37, 32, 27, 23, 20, 18, 16]

# Input Files
input:
  first_file: 0
  last_file: 7
  tree_name: trees_063
  tree_type: lhalo_binary
  simulation_dir: ./input/data/millennium/

# Simulation Properties
simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73
  box_size: 62.5
  particle_mass: 0.0860657

# Time Sub-Stepping
SubSteps: 1  # Number of substeps per snapshot (1 = no substeps, 20 = SAGE-like)

# Multi-Phase Pipeline
modules:
  # Pre-timestep: Setup (runs once before substeps)
  pre_timestep:
    - sage_reionization: once
    - sage_infall: once

  # Phase 1: Main physics (runs each substep)
  phase_1:
    - sage_satellite_stripping: all
    - sage_cooling: all
    - sage_starformation_feedback: all
    - sage_reincorporation: all
    - sage_disk_instability: all

  # Phase 2: Mergers (runs each substep)
  phase_2:
    - sage_mergers: all

  # Post-timestep: Finalization (runs once after substeps)
  post_timestep: []

  # Model parameters needed by enabled modules
  parameters:
    GlobalBaryonFraction: 0.17
    RadioModeEfficiency: 0.01
    AGNrecipeOn: 1
    CoolFunctionsDir: "input/CoolFunctions"
    SFprescription: 0
    SfrEfficiency: 0.02
    EnergySNcode: 1.0
    EtaSNcode: 0.5
    SupernovaRecipeOn: 1
    FeedbackReheatingEpsilon: 3.0
    FeedbackEjectionEfficiency: 0.3
    RecycleFraction: 0.43
    Yield: 0.03
    FracZleaveDisk: 0.3
    ReIncorporationFactor: 1.0
    BlackHoleGrowthRate: 0.01
    QuasarModeEfficiency: 0.001
    ThreshMajorMerger: 0.3
    DiskInstabilityOn: 1
    DiskRadiusFactor: 3.0
```

**Note**: Mimic uses YAML format for configuration. YAML provides better structure, validation, and is industry-standard.

## Error Handling

### Unknown Module

If you list a module that isn't registered:

```yaml
modules:
  phase_1:
    - fake_module: all
```

**Error**:
```
ERROR: Module 'fake_module' listed in phase_1 but not registered
Available modules:
  - sage_reionization
  - sage_infall
  - sage_satellite_stripping
  - sage_cooling
  - sage_starformation_feedback
  - ...
```

**Solution**: Check module name spelling or verify module is compiled

### Missing Model Parameter

**Parameters needed by your enabled modules are REQUIRED.** If any required parameter is missing:

**Error**:
```
ERROR: Required model parameter 'GlobalBaryonFraction' not found in input file
ERROR:   (needed by modules: sage_reionization)
```

**Solution**: Add the missing parameter to the `modules.parameters:` section. The error message lists which modules need it. To see parameter details (type, units, valid range), check the module's `module_info.yaml` file in `src/modules/<module_name>/`.

### Invalid Parameter Value

Parameter values are validated against ranges defined in each module's `module_info.yaml`:

**Error**:
```
ERROR: GlobalBaryonFraction = 2.0 is outside valid range [0.0, 1.0]
ERROR:   (defined by module: sage_reionization)
```

**Solution**: Correct the parameter value to be within the valid range. Check the module's `module_info.yaml` for parameter specifications.

## Tips

1. **Copy from example**: Start with `input/millennium.yaml` and modify as needed
2. **Smart validation**: Only parameters needed by your enabled modules are required
3. **Start simple**: Begin with empty phases (`[]`) and add modules incrementally
4. **Check logs**: Module initialization logs show which modules loaded and which phases they're in
5. **Phase placement**: Use recommended phases from module descriptions above
6. **Loop mode choice**: Use `once` for snapshot-level ops, `all` for per-galaxy physics
7. **Comment your config**: Use `#` in YAML to document physics choices
8. **Sub-stepping**: Start with `SubSteps: 1` for testing, increase for better numerical stability

## Troubleshooting

**Problem**: Modules not executing
**Check**: Module names spelled correctly in phase configurations (pre_timestep, phase_1, etc.)

**Problem**: "Phase 'phase_2' must be a sequence" error
**Solution**: Fixed in current version - phases with only comments are now automatically treated as empty. Using `phase_2: []` syntax still works but is no longer required.

**Problem**: Missing parameter errors
**Check**: Parameters needed by enabled modules specified in `modules.parameters:` section (check error message for which modules need the parameter)

**Problem**: Wrong physics results
**Check**:
- Modules in correct phases (setup in pre_timestep, main physics in phase_1, etc.)
- Dependencies satisfied (e.g., sage_reionization before sage_infall)
- Correct loop mode (once vs all) for each module

**Problem**: SubSteps not working
**Check**: SubSteps is top-level parameter (outside `modules:`), not inside modules section

**Problem**: Parameter value errors
**Check**: Parameter values within valid ranges (see module's `module_info.yaml` for specifications)

**Problem**: Module errors at initialization
**Check**: Module logs during initialization for parameter validation errors

## See Also

- `docs/architecture/roadmap.md` - Module system implementation roadmap
- `docs/architecture/vision.md` - Architectural principles
- Module developer guide - Writing new modules (see `docs/developer/module-developer-guide.md`)
