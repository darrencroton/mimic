# Module Configuration Guide

**Audience**: Users configuring Mimic for scientific runs
**Prerequisites**: Basic understanding of YAML parameter files

## Overview

Mimic's modular architecture allows you to enable/disable galaxy physics modules and configure model parameters at runtime without recompilation. This guide explains how to configure modules and physics parameters via the YAML parameter file.

**Key Concepts:**
- **Modules**: Physics components (e.g., sage_cooling, sage_starformation_feedback)
- **Model Parameters**: Physics parameters used by modules (e.g., BaryonFrac, SfrEfficiency)
- **Decentralized Definitions**: Each module defines its needed parameters in its own `module_info.yaml`
- **No Defaults**: All parameters MUST be specified - ensures reproducible science

## Enabling Modules

### EnabledModules Parameter

Modules are enabled via the `modules.enabled` list in your YAML configuration file:

```
EnabledModules  cooling_model,starformation_model
```

**Format:**
- Comma-separated list of module names
- No spaces around module names (spaces after commas are trimmed)
- Execution order = list order (important for dependent modules!)
- Empty list or omitted parameter = physics-free mode (halo tracking only)

### Execution Order Matters

Modules execute in the order listed. If Module B depends on properties created by Module A, list A before B:

```
# CORRECT: cooling provides ColdGas for star formation
EnabledModules  cooling_model,starformation_model

# WRONG: starformation runs first, sees no cold gas!
EnabledModules  starformation_model,cooling_model
```

### Physics-Free Mode

To run without any physics modules (halo tracking only):

```
# Option 1: Empty list
EnabledModules

# Option 2: Omit the parameter entirely (not recommended for clarity)
```

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

1. **Cosmological Parameters** (1): BaryonFrac
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
  BaryonFrac: 0.17

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

#### sage_infall

**Purpose**: Cosmological gas infall and satellite stripping

**Physics**:
- Gas infall: infallingMass = f_reion * BaryonFrac * Mvir - (total baryons)
- Reionization suppression following Gnedin (2000)
- Environmental stripping of satellite hot gas

**Dependencies**: None (provides initial hot gas reservoir)

**Provides**: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, TotalSatelliteBaryons, InfallingGas

**Execution Order**: Should run **early** in pipeline (before cooling, star formation)

**Model Parameters Used**: BaryonFrac

---

#### sage_satellite_stripping

**Purpose**: Environmental gas stripping for satellite galaxies

**Physics**: Removes hot gas from satellites in massive halos

**Dependencies**: Requires sage_infall (provides hot gas reservoir)

**Provides**: Updates HotGas, MetalsHotGas for satellites

**Execution Order**: After sage_infall, before cooling

**Model Parameters Used**: BaryonFrac

---

#### sage_cooling

**Purpose**: Gas cooling from hot halo to cold disk

**Physics**: Radiative cooling with AGN feedback suppression

**Dependencies**: Requires hot gas (from sage_infall)

**Provides**: ColdGas, MetalsColdGas

**Execution Order**: After infall/stripping, before star formation

**Model Parameters Used**: RadioModeEfficiency, AGNrecipeOn, CoolFunctionsDir

---

#### sage_starformation_feedback

**Purpose**: Star formation and supernova feedback

**Physics**: Converts cold gas to stars, reheats/ejects gas via supernovae

**Dependencies**: Requires ColdGas (from sage_cooling)

**Provides**: StellarMass, MetalsStellarMass (updates HotGas, EjectedMass)

**Execution Order**: After cooling

**Model Parameters Used**: SFprescription, SfrEfficiency, EnergySNcode, EtaSNcode, SupernovaRecipeOn, FeedbackReheatingEpsilon, FeedbackEjectionEfficiency, RecycleFraction, Yield, FracZleaveDisk

---

#### sage_reincorporation

**Purpose**: Reincorporation of ejected gas back to hot halo

**Physics**: Time-dependent return of ejected baryons

**Dependencies**: Requires EjectedMass (from sage_starformation_feedback)

**Provides**: Updates HotGas, EjectedMass

**Execution Order**: After star formation

**Model Parameters Used**: ReIncorporationFactor

---

#### sage_mergers

**Purpose**: Galaxy mergers and black hole growth

**Physics**: Handles major/minor mergers, BH accretion, quasar feedback

**Dependencies**: Tree structure (galaxy hierarchies)

**Provides**: BlackHoleMass (triggers bulge formation, quasar feedback)

**Execution Order**: After star formation

**Model Parameters Used**: BlackHoleGrowthRate, QuasarModeEfficiency, ThreshMajorMerger

---

#### sage_disk_instability

**Purpose**: Disk instability and bulge formation

**Physics**: Converts unstable disk stars to bulge

**Dependencies**: Requires StellarMass

**Provides**: BulgeMass, DiskMass

**Execution Order**: After star formation

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
  BaryonFrac: 0.17
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

# Physics Modules
modules:
  enabled:
  - sage_infall
  - sage_satellite_stripping
  - sage_cooling
  - sage_starformation_feedback
  - sage_reincorporation
  - sage_mergers
  - sage_disk_instability
```

**Note**: Mimic uses YAML format for configuration. YAML provides better structure, validation, and is industry-standard.

## Error Handling

### Unknown Module

If you list a module that isn't registered:

```yaml
modules:
  enabled:
  - fake_module
```

**Error**:
```
ERROR: Module 'fake_module' listed in modules.enabled but not registered
Available modules:
  - sage_infall
  - sage_cooling
  - sage_starformation_feedback
  - ...
```

**Solution**: Check module name spelling or verify module is compiled

### Missing Model Parameter

**Parameters needed by your enabled modules are REQUIRED.** If any required parameter is missing:

**Error**:
```
ERROR: Required model parameter 'BaryonFrac' not found in input file
ERROR:   (needed by modules: sage_infall, sage_satellite_stripping)
```

**Solution**: Add the missing parameter to the `modules.parameters:` section. The error message lists which modules need it. To see parameter details (type, units, valid range), check the module's `module_info.yaml` file in `src/modules/<module_name>/`.

### Invalid Parameter Value

Parameter values are validated against ranges defined in each module's `module_info.yaml`:

**Error**:
```
ERROR: BaryonFrac = 2.0 is outside valid range [0.0, 1.0]
ERROR:   (defined by module: sage_infall)
```

**Solution**: Correct the parameter value to be within the valid range. Check the module's `module_info.yaml` for parameter specifications.

## Tips

1. **Copy from example**: Start with `input/millennium.yaml` and modify as needed
2. **Smart validation**: Only parameters needed by your enabled modules are required
3. **One module at a time**: When testing, enable modules one at a time to isolate issues
4. **Check logs**: Module initialization logs show which parameters were loaded
5. **Physics order**: Always list modules in dependency order (see module descriptions above)
6. **Comment your config**: Use `#` in YAML to document why you chose specific values

## Troubleshooting

**Problem**: Modules not executing
**Check**: `modules.enabled:` list present and module names spelled correctly

**Problem**: Missing parameter errors
**Check**: Parameters needed by enabled modules specified in `modules.parameters:` section (check error message for which modules need the parameter)

**Problem**: Wrong physics results
**Check**: Module execution order - dependencies must run first (see module descriptions above)

**Problem**: Parameter value errors
**Check**: Parameter values within valid ranges (see module's `module_info.yaml` for specifications)

**Problem**: Module errors at initialization
**Check**: Module logs during initialization for parameter validation errors

## See Also

- `docs/architecture/roadmap.md` - Module system implementation roadmap
- `docs/architecture/vision.md` - Architectural principles
- Module developer guide - Writing new modules (see `docs/developer/module-developer-guide.md`)
