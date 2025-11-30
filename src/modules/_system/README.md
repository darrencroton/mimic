# Mimic Framework Infrastructure (_system/)

**Purpose**: Permanent framework infrastructure for module development.

**DO NOT MODIFY** unless adding universal framework features.

---

## Directory Contents

### Generated Code
- `generated/` - Auto-generated module registration code (created by build system)

### Module Templates & Testing
- `template/` - Template for creating new physics modules
- `test_fixture/` - Infrastructure testing module (validates framework functionality)

### Universal Constants
- **`physical_constants.h`** - Universal physical constants (G, c, Z_sun, etc.)
  - Single source of truth for physical constants
  - Used by all modules for physics calculations
  - Terse format (one constant per line)

### Output Infrastructure
- **`output_helpers.h`** - Output formatting macros for HDF5 and binary writers

---

## Usage in Modules

### Include Physical Constants
```c
#include "../_system/physical_constants.h"

// Now you can use:
// - GRAVITY, SOLAR_MASS, CM_PER_MPC (CGS for unit conversions)
// - C_CGS, C_KM_S, C_SQUARED_CGS (speed of light)
// - SOLAR_METALLICITY (Z_sun)
// - SN_ENERGY_ERG (supernova energy)
// - RADIATIVE_EFFICIENCY (η for black holes)
// - etc.
```

### Include Output Helpers
```c
#include "../_system/output_helpers.h"

// Provides macros for output file writing
```

---

## Adding New Universal Constants

**Before adding**, ask:
1. Is this truly universal (not module-specific)?
2. Is it already defined elsewhere?
3. Does it belong in model parameters instead?

**If yes to all**:
1. Add to `physical_constants.h` with inline comment
2. Use terse format: `static const double NAME = value;  /* units, reference */`
3. Update this README if needed

**Examples of what belongs here**:
- ✅ Fundamental constants (G, c, h, k_B)
- ✅ Solar values (M_sun, L_sun, Z_sun)
- ✅ Universal empirical constants (η, recycling fraction)

**Examples of what does NOT belong here**:
- ❌ Model parameters (efficiency factors, thresholds) → use `model_parameters.yaml`
- ❌ Module-specific calibrations → keep in module file
- ❌ User-swappable models → use `_shared/` directory

---

## Philosophy

This directory contains the **permanent framework infrastructure** that modules depend on:

- **Generated code**: Auto-generated, never hand-edited
- **Templates**: For creating new modules
- **Test infrastructure**: For validating framework
- **Physical constants**: Universal, immutable
- **Output helpers**: Standard formatting

If you're developing a **physics module**, you probably want `_shared/` instead (for reusable utilities) or your own module directory (for module-specific code).

---

## See Also

- `src/modules/_shared/` - User-created utilities and physics models
- `docs/developer/module-developer-guide.md` - How to create modules
- `docs/architecture/vision.md` - Framework architecture principles
