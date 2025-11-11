# Module Configuration Guide

**Phase**: 3 (Runtime Module Configuration)
**Audience**: Users configuring Mimic for scientific runs
**Prerequisites**: Basic understanding of parameter files

## Overview

Mimic's modular architecture allows you to enable/disable galaxy physics modules and configure their parameters at runtime without recompilation. This guide explains how to configure modules via the parameter file.

## Enabling Modules

### EnabledModules Parameter

Modules are enabled via the `EnabledModules` parameter in your `.par` file:

```
EnabledModules  simple_cooling,simple_sfr
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
EnabledModules  simple_cooling,simple_sfr

# WRONG: simple_sfr runs first, sees no cold gas!
EnabledModules  simple_sfr,simple_cooling
```

### Physics-Free Mode

To run without any physics modules (halo tracking only):

```
# Option 1: Empty list
EnabledModules

# Option 2: Omit the parameter entirely (not recommended for clarity)
```

## Module-Specific Parameters

Each module can have configurable parameters using the format:

```
ModuleName_ParameterName  value
```

### Parameter Format

- **Naming**: `ModuleName_ParameterName` (underscore separator)
- **Module name**: Must match exactly (case-sensitive)
- **Parameter name**: Defined by each module
- **Value**: String (modules parse to appropriate type)

### Example: Simple Cooling Module

```
# Enable simple cooling
EnabledModules  simple_cooling

# Configure cooling parameters
SimpleCooling_BaryonFraction  0.15
```

### Example: Multiple Modules

```
# Enable multiple modules
EnabledModules  simple_cooling,simple_sfr

# Simple cooling parameters
SimpleCooling_BaryonFraction  0.15

# Star formation parameters
SimpleSFR_Efficiency  0.02
```

## Available Modules (Phase 3)

### simple_cooling

**Purpose**: Placeholder cooling module for infrastructure testing.

**Physics**: ΔColdGas = f_baryon * ΔMvir

**Parameters**:
- `SimpleCooling_BaryonFraction` (optional, default=0.15)
  - Fraction of accreted mass that cools
  - Valid range: 0.0 - 1.0
  - Cosmic value: ~0.15 (Ω_b/Ω_m)

**Dependencies**: None

**Provides**: ColdGas property

---

### simple_sfr

**Purpose**: Placeholder star formation module for infrastructure testing.

**Physics**: ΔStellarMass = ε * ColdGas * (Vvir/Rvir) * Δt

**Parameters**:
- `SimpleSFR_Efficiency` (optional, default=0.02)
  - Star formation efficiency
  - Valid range: 0.0 - 1.0
  - Typical values: 0.01 - 0.05

**Dependencies**:
- Requires ColdGas (from simple_cooling or similar)
- Must run after cooling module

**Provides**: StellarMass property

---

## Complete Example Parameter File

```
%------------------------------------------
%----- Simulation Information ------------
%------------------------------------------
FirstFile               0
LastFile                7
OutputDir               ./output/results/millennium/
TreeName                trees_063
SimulationDir           ./input/trees/millennium/
FileWithSnapList        ./input/a_list.txt

%------------------------------------------
%----- Cosmological Parameters ------------
%------------------------------------------
Omega                   0.25
OmegaLambda             0.75
PartMass                0.0860657
Hubble_h                0.73
BoxSize                 500.0

%------------------------------------------
%----- Output Control ---------------------
%------------------------------------------
LastSnapshotNr          63
NumOutputs              -1
TreeType                lhalo_binary
OutputFormat            binary
OutputFileBaseName      millennium

%------------------------------------------
%----- Units (auto-calculated) -----------
%------------------------------------------
UnitLength_in_cm               3.08568e+24
UnitVelocity_in_cm_per_s       100000
UnitMass_in_g                  1.989e+43

%------------------------------------------
%----- Galaxy Physics Modules -------------
%------------------------------------------
EnabledModules          simple_cooling,simple_sfr

# Simple cooling module
SimpleCooling_BaryonFraction    0.15

# Star formation module
SimpleSFR_Efficiency          0.02
```

## Error Handling

### Unknown Module

If you list a module that isn't registered:

```
EnabledModules  fake_module
```

**Error**:
```
ERROR: Module 'fake_module' listed in EnabledModules but not registered
Available modules:
  - simple_cooling
  - simple_sfr
```

**Solution**: Check module name spelling or verify module is compiled

### Missing Required Parameter

If a module requires a parameter and it's missing, the module will use its default value and log a warning or error depending on the module's design.

### Invalid Parameter Value

Modules validate their parameter values during initialization. Invalid values will produce clear error messages:

```
ERROR: SimpleCooling_BaryonFraction = 2.0 is outside valid range [0.0, 1.0]
```

## Tips

1. **Start with defaults**: Omit optional parameters to use defaults, add configuration as needed
2. **One module at a time**: When testing, enable modules one at a time to isolate issues
3. **Check logs**: Module initialization logs show which parameters were loaded
4. **Physics order**: Always list modules in dependency order (cooling before star formation)
5. **Comment your config**: Use `#` or `%` to document why you chose specific values

## Troubleshooting

**Problem**: Modules not executing
**Check**: `EnabledModules` parameter present and module names spelled correctly

**Problem**: Wrong physics results
**Check**: Module execution order - dependencies must run first

**Problem**: Parameter not taking effect
**Check**: Parameter name matches exactly (ModuleName_ParameterName format)

**Problem**: Module errors at initialization
**Check**: Module logs during initialization for parameter validation errors

## See Also

- `docs/architecture/roadmap_v4.md` - Module system implementation roadmap
- `docs/architecture/vision.md` - Architectural principles
- Module developer guide (planned for Phase 4) - Writing new modules
