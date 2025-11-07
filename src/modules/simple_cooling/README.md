# Simple Cooling Module

## Overview

This module implements a minimal cooling prescription for PoC Round 2, designed to test module interaction and time evolution patterns.

## Physics

Cold gas accumulates from newly accreted dark matter:

```
ΔColdGas = f_baryon * ΔMvir
```

where:
- `f_baryon = 0.15` - Cosmic baryon fraction (Ω_b / Ω_m)
- `ΔMvir` - Halo mass accreted since last timestep (from `struct Halo.deltaMvir`)
- `ColdGas` - Total cold gas mass (accumulates via progenitor inheritance)

## Implementation Details

### What This Module Does

1. Processes only central galaxies (`Type == 0`)
2. For halos that have grown (`deltaMvir > 0`):
   - Converts 15% of accreted mass to cold gas
   - Adds to existing cold gas (inherited from progenitor)
3. For halos losing mass (`deltaMvir < 0`):
   - No new gas added
   - Keeps inherited cold gas

### Module Properties

- **Created properties**: `ColdGas` (float, 10^10 Msun/h)
- **Dependencies**: None (first module in pipeline)
- **Execution order**: Must run before `stellar_mass` module

## PoC Round 2 Test Cases

This module validates:

1. **Time evolution**: Uses `deltaMvir` calculated per timestep
2. **Property inheritance**: `ColdGas` persists through progenitor copying
3. **Module communication**: Provides `ColdGas` for `stellar_mass` module
4. **Execution ordering**: Must run before modules that consume cold gas

## Parameters

Currently hardcoded:
- `BARYON_FRACTION = 0.15`

Future (Phase 3): Read from parameter file as `Cooling_BaryonFraction`

## Validation

Expected behavior:
- ColdGas should grow over time (higher at low redshift)
- ColdGas ≈ 0.15 * Mvir for newly formed halos
- Cold gas mass function should have reasonable shape

## Files

- `simple_cooling.h` - Module interface
- `simple_cooling.c` - Implementation
- `README.md` - This file

## Registration

In `src/core/main.c`:
```c
#include "../modules/simple_cooling/simple_cooling.h"

simple_cooling_register();  // Before stellar_mass
stellar_mass_register();
module_system_init();
```
