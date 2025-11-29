# Mimic Unit System Guide

**Purpose**: Complete guide to Mimic's unit system for module developers

**Audience**: Developers implementing physics modules or modifying core code

**Last Updated**: 2025-11-29

---

## Quick Reference

**Code Units (Internal):**
- Mass: 10^10 Msun/h
- Length: Mpc/h
- Velocity: km/s
- Time: Derived from Length/Velocity
- G (gravitational constant): 43.0071 (pre-computed in code units)

**Golden Rule**: All internal calculations use code units. Conversions only at I/O boundaries.

---

## Table of Contents

1. [Unit System Overview](#unit-system-overview)
2. [Code Unit Definitions](#code-unit-definitions)
3. [Architecture & Separation of Concerns](#architecture--separation-of-concerns)
4. [Module Developer Checklist](#module-developer-checklist)
5. [Common Patterns (Good vs Bad)](#common-patterns-good-vs-bad)
6. [Adding New Properties](#adding-new-properties)
7. [Unit Conversions](#unit-conversions)
8. [Testing Your Module](#testing-your-module)
9. [Debugging Unit Issues](#debugging-unit-issues)

---

## Unit System Overview

### Why Code Units?

Mimic uses **code units** throughout all internal calculations for:
- **Efficiency**: No repeated unit conversions during calculations
- **Consistency**: Single unit system eliminates conversion errors
- **Simplicity**: Clear, predictable behavior across all modules
- **SAGE Heritage**: Proven approach from SAGE galaxy evolution code

### When Are Units Converted?

**Never during calculations** - only at these boundaries:
1. **Input**: Reading tree data (already in code units from simulation)
2. **Parameters**: Reading physics parameters (converted at initialization)
3. **Output**: Writing results to files (optional conversion for human readability)
4. **Physics Coefficients**: Specific calculations requiring CGS (e.g., cooling rates)

### Core Philosophy

```
Input (code units) → Physics Modules (code units) → Output (optionally converted)
                           ↓
                  No conversions here!
```

---

## Code Unit Definitions

### Base Units (Defined in Parameter File)

From `input/millennium.yaml`:
```yaml
UnitLength_in_cm: 3.08568e+24      # 1 Mpc/h
UnitMass_in_g: 1.989e+43           # 10^10 Msun
UnitVelocity_in_cm_per_s: 100000   # 1 km/s
```

### Derived Code Units

**Time**:
```
UnitTime = UnitLength / UnitVelocity
         = 3.08568e24 cm / 1e5 cm/s
         = 3.08568e19 s
         ≈ 978 Myr
```

**Energy**:
```
UnitEnergy = UnitMass × UnitVelocity²
           = 1.989e43 g × (1e5 cm/s)²
           = 1.989e53 g·cm²/s²
```

**Gravitational Constant G**:
```
G_code = 43.0071  (in code units)

Computed via:
G_cgs = 6.672e-8 cm³/(g·s²)
G_code = G_cgs / (UnitLength³ / (UnitMass × UnitTime²))
```

**Access via**: `ctx->params->G` (ALWAYS use this, never recompute!)

### Property Units

From `src/core/halo_properties.yaml` and `src/modules/model_properties.yaml`:

| Property | Code Units | Physical Meaning |
|----------|-----------|------------------|
| Mvir | 10^10 Msun/h | Virial mass |
| Rvir | Mpc/h | Virial radius |
| Vvir | km/s | Virial velocity |
| ColdGas | 10^10 Msun/h | Cold gas mass |
| StellarMass | 10^10 Msun/h | Stellar mass |
| dT | seconds (internal) | Time since last snapshot |
| Pos[3] | Mpc/h | 3D position (comoving) |
| Vel[3] | km/s | 3D velocity (peculiar) |

**Note**: dT is converted to Myr for output (only property with output conversion currently).

---

## Architecture & Separation of Concerns

### Core vs Modules

```
src/
├── core/          ← CORE (physics-agnostic, NO hardcoded physics)
├── io/            ← CORE (physics-agnostic, auto-generated I/O code OK)
├── util/          ← CORE (physics-agnostic utilities)
├── include/       ← CORE (headers + auto-generated code)
└── modules/       ← PHYSICS (all physics calculations here)
```

### Property Separation

**Core Properties** (`src/core/halo_properties.yaml`):
- Halo tracking (SnapNum, Type, MergeStatus, etc.)
- Virial properties (Mvir, Rvir, Vvir)
- Infall properties (infallMvir, infallVvir, infallVmax)
- **Can be accessed**: In core code and modules

**Galaxy Properties** (`src/modules/model_properties.yaml`):
- Baryonic physics (ColdGas, HotGas, StellarMass, etc.)
- Chemical composition (MetalsColdGas, MetalsHotGas, etc.)
- Feedback tracking (Cooling, Heating, OutflowRate, etc.)
- **Can ONLY be accessed**: In `src/modules/` (except auto-generated I/O code)

### Auto-Generated Code

**How core-physics separation works**:
1. Core code includes `generated/copy_to_output.inc` (auto-generated)
2. Generator (`scripts/generate_properties.py`) reads property metadata
3. Produces code that **changes dynamically** when modules change
4. If you remove all modules, generated code removes galaxy property references

**Example**:
```c
/* In src/io/output/binary.c (CORE) */
void prepare_halo_for_output(...) {
  /* AUTO-GENERATED code (changes with modules) */
  #include "../../include/generated/copy_to_output.inc"

  /* This line only exists if ColdGas property is defined: */
  o->ColdGas = g->galaxy->ColdGas;
}
```

**Result**: Core has NO hardcoded physics, only dynamic auto-generated glue code. ✅

---

## Module Developer Checklist

Use this checklist when developing or reviewing physics modules:

### 1. Gravitational Constant
- [ ] Use `ctx->params->G` (pre-computed, always ~43.0071)
- [ ] **Never** compute G from scratch using dimensional analysis
- [ ] If debugging, print G value to verify: `printf("G = %.6f\n", ctx->params->G);`

### 2. Unit Conversions
- [ ] All conversions use `ctx->params->Unit*_in_cgs` (UnitDensity_in_cgs, etc.)
- [ ] No hardcoded unit conversion factors
- [ ] Conversion points documented in code comments
- [ ] Only convert at specific boundaries (see examples below)

### 3. Input Assumptions
- [ ] Assume ALL input properties are in code units
- [ ] No implicit physical unit assumptions
- [ ] Access cosmological parameters via `ctx->params` (Omega, OmegaLambda, Hubble_h)

### 4. Output Properties
- [ ] All module outputs in code units (unless explicit `output_convert` in YAML)
- [ ] Output ranges physically reasonable
- [ ] Properties validated in scientific tests

### 5. Physical Constants
- [ ] Use constants from `src/include/constants.h` (GRAVITY, BOLTZMANN, PROTONMASS, etc.)
- [ ] All constants in CGS (standard convention)
- [ ] No magic numbers for physical constants

### 6. Documentation
- [ ] Unit assumptions documented in module header
- [ ] Complex conversions explained in comments
- [ ] References to papers include unit conventions used

### 7. Shared Utilities
- [ ] Check `src/modules/shared/` for existing utilities before implementing
- [ ] Add reusable calculations to `shared/` (metallicity, disk radius, etc.)
- [ ] Use header-only design for inline performance

---

## Common Patterns (Good vs Bad)

### Pattern 1: Using Gravitational Constant

**✅ GOOD** - Use pre-computed G:
```c
/* In your module physics calculation */
double G = ctx->params->G;  /* Already in code units (~43.0071) */
double t_dyn = Rvir / Vvir;
double t_merge = 2.0 * Rvir * Rvir * Vvir / (G * Msat);
```

**❌ BAD** - Compute G with dimensional analysis:
```c
/* DO NOT DO THIS - Error-prone and unnecessary! */
double G_code = GRAVITY * (1.0/1000.0) * (1.0/1000.0) *
                CM_PER_MPC * hubble_h * (1.0/SOLAR_MASS) * (1.0/1e10);
/* This is how the reionization bug happened! */
```

### Pattern 2: Cooling Rate Conversion

**✅ GOOD** - Convert cooling coefficient from physical to code units:
```c
/* In sage_cooling module */
/* Calculate cooling coefficient in physical units */
double x_cgs = PROTONMASS * BOLTZMANN * temp / lambda;  /* CGS */

/* Convert to code units for use in simulation */
double x_code = x_cgs / (ctx->params->UnitDensity_in_cgs *
                         ctx->params->UnitTime_in_s);

/* Now use x_code in rate calculations */
double tcool = x_code / (density * cooling_function);
```

**❌ BAD** - Mix physical and code units:
```c
/* DO NOT DO THIS - Mixing unit systems! */
double x = PROTONMASS * BOLTZMANN * temp / lambda;  /* CGS */
double tcool = x / (density * cooling_function);     /* density in code units! */
/* Units don't match - result is wrong! */
```

### Pattern 3: Mass/Energy Conservation

**✅ GOOD** - Track masses in code units consistently:
```c
/* Mass transfer from cold gas to stars */
double sfr = epsilon * ColdGas / t_dyn;  /* All in code units */
double delta_cold = -sfr * dt;
double delta_stars = sfr * dt;

/* Update properties (still in code units) */
ColdGas += delta_cold;
StellarMass += delta_stars;

/* Conservation check */
assert(fabs(delta_cold + delta_stars) < EPSILON_SMALL);
```

**❌ BAD** - Convert mid-calculation:
```c
/* DO NOT DO THIS - Unnecessary and error-prone! */
double sfr_msun_per_yr = epsilon * ColdGas * 1e10 / t_dyn_years;
double delta_stars_msun = sfr_msun_per_yr * dt_years;
double delta_stars_code = delta_stars_msun / 1e10;
/* Why convert? Just work in code units throughout! */
```

### Pattern 4: Accessing Module Context

**✅ GOOD** - Access parameters via context:
```c
void my_module_process(struct Halo *halo, const struct ModuleContext *ctx) {
  /* Cosmological parameters */
  double omega = ctx->params->Omega;
  double omega_lambda = ctx->params->OmegaLambda;
  double hubble_h = ctx->params->Hubble_h;

  /* Pre-computed constants */
  double G = ctx->params->G;

  /* Current snapshot info */
  double redshift = ctx->redshift;
  double time = ctx->time;  /* In code units */
}
```

**❌ BAD** - Global variable access (old SAGE pattern):
```c
/* DO NOT DO THIS in Mimic - Use ctx->params instead! */
extern double Omega;  /* Global from MimicConfig */
double some_calc = Omega * something;
/* Mimic passes parameters explicitly via ModuleContext */
```

### Pattern 5: Shared Physics Utilities

**✅ GOOD** - Use shared utilities:
```c
/* In your module */
#include "../shared/metallicity.h"
#include "../shared/disk_radius.h"

/* Use shared calculation */
double Z_cold = calculate_metallicity(MetalsColdGas, ColdGas);
double R_disk = calculate_disk_radius(lambda_spin, Vvir, Rvir);
```

**❌ BAD** - Duplicate physics calculations:
```c
/* DO NOT DO THIS - Check shared/ first! */
/* Reimplementing metallicity calculation that already exists */
double Z_cold = (ColdGas > 0) ? MetalsColdGas / ColdGas : 0.0;
/* Same calculation exists in shared/metallicity.h */
```

---

## Adding New Properties

### Step 1: Choose Property Category

**Core property** (`src/core/halo_properties.yaml`) if:
- Halo tracking or structural property
- Needed by core infrastructure
- Not dependent on specific physics modules

**Galaxy property** (`src/modules/model_properties.yaml`) if:
- Baryonic physics component
- Specific to physics modules
- Should only exist when modules are enabled

### Step 2: Add Property to YAML

Example (galaxy property):
```yaml
- name: MyNewProperty
  type: float
  units: 1e10 Msun/h        # Internal code units
  description: My new physics property
  output: true
  init_source: default
  init_value: 0.0
  output_source: galaxy_property
  range: [0.0, 10000.0]     # Validation range
  sentinels: [0.0]          # Values to exclude from validation
```

### Step 3: Regenerate Code

```bash
make generate
```

This updates:
- `src/include/generated/property_defs.h` (struct definitions)
- `src/include/generated/init_galaxy_properties.inc` (initialization)
- `src/include/generated/copy_to_output.inc` (output marshalling)
- `src/include/generated/hdf5_field_definitions.inc` (HDF5 schema)
- `output/mimic-plot/generated/dtype.py` (Python dtypes)

### Step 4: Use in Your Module

```c
/* Get property value */
float value = get_MyNewProperty(galaxy);

/* Set property value */
set_MyNewProperty(galaxy, new_value);

/* Or direct access (if you prefer) */
galaxy->MyNewProperty = new_value;
```

### Step 5: Add Output Conversion (If Needed)

If your property needs unit conversion for output (rare):
```yaml
- name: MyTimeProperty
  type: float
  units: seconds                    # Internal code units
  output_convert: "UnitTime_in_s / SEC_PER_MEGAYEAR"  # Convert to Myr
  description: My time property
  # ... rest of fields
```

Generator will create:
```c
/* In copy_to_output.inc */
if (g->MyTimeProperty == -1.0) {  /* Sentinel handling */
  o->MyTimeProperty = -1.0;
} else {
  o->MyTimeProperty = g->MyTimeProperty * (UnitTime_in_s / SEC_PER_MEGAYEAR);
}
```

---

## Unit Conversions

### Where Conversions Happen

**1. Initialization** (`src/core/init.c`):
```c
/* Compute derived units from base units */
MimicConfig.UnitTime_in_s = UnitLength_in_cm / UnitVelocity_in_cm_per_s;
MimicConfig.G = GRAVITY / pow(UnitLength_in_cm, 3) *
                UnitMass_in_g * pow(UnitTime_in_s, 2);
```

**2. Physics Calculations** (specific points):
```c
/* Example: Cooling rate calculation */
/* Compute in CGS, then convert to code units */
double rate_cgs = some_physics_function(...);  /* CGS result */
double rate_code = rate_cgs / (ctx->params->UnitDensity_in_cgs *
                                ctx->params->UnitTime_in_s);
```

**3. Output** (auto-generated):
```c
/* In copy_to_output.inc (RARE - only dT currently) */
o->dT = g->dT * (UnitTime_in_s / SEC_PER_MEGAYEAR);
```

### Accessing Unit Conversions

All unit conversion factors available via `ctx->params`:

```c
struct ModuleContext *ctx;  /* Passed to your module */

/* Base units */
ctx->params->UnitLength_in_cm;
ctx->params->UnitMass_in_g;
ctx->params->UnitVelocity_in_cm_per_s;

/* Derived units */
ctx->params->UnitTime_in_s;
ctx->params->UnitTime_in_Megayears;
ctx->params->UnitDensity_in_cgs;
ctx->params->UnitPressure_in_cgs;
ctx->params->UnitCoolingRate_in_cgs;
ctx->params->UnitEnergy_in_cgs;

/* Pre-computed constants */
ctx->params->G;           /* Gravitational constant in code units */
ctx->params->RhoCrit;     /* Critical density */
ctx->params->Hubble;      /* Hubble constant */
```

### Conversion Examples

**Mass: code units ↔ physical units**:
```c
/* Code units (10^10 Msun/h) to physical (Msun) */
double mass_code = 1.5;  /* 1.5 × 10^10 Msun/h */
double mass_msun = mass_code * 1e10 / hubble_h;  /* Msun (no h) */

/* Physical (Msun) to code units (10^10 Msun/h) */
double mass_msun = 1.5e11;  /* 1.5 × 10^11 Msun */
double mass_code = mass_msun * hubble_h / 1e10;  /* code units */
```

**Time: code units ↔ physical units**:
```c
/* Code units to Gyr */
double time_code = 5.0;  /* In code time units */
double time_gyr = time_code * ctx->params->UnitTime_in_s / SEC_PER_GIGAYEAR;

/* Gyr to code units */
double time_gyr = 2.0;
double time_code = time_gyr * SEC_PER_GIGAYEAR / ctx->params->UnitTime_in_s;
```

**Density: code units ↔ CGS**:
```c
/* Code units to CGS */
double rho_code = 100.0;  /* In code density units */
double rho_cgs = rho_code * ctx->params->UnitDensity_in_cgs;

/* CGS to code units */
double rho_cgs = 1.5e-24;  /* g/cm³ */
double rho_code = rho_cgs / ctx->params->UnitDensity_in_cgs;
```

---

## Testing Your Module

### Unit Validation Tests

Add to `tests/scientific/test_scientific.py`:
```python
def test_my_module_units():
    """Verify my module uses correct units."""
    # Load test output
    data = load_binary_halos("tests/data/test_output.dat")

    # Check G value from config
    config = load_config("tests/data/test_params.yaml")
    assert abs(config['G'] - 43.0071) < 0.001, "G not in code units"

    # Check property ranges (in code units)
    assert np.all(data['MyProperty'] >= 0), "Negative mass"
    assert np.all(data['MyProperty'] < 1e5), "Unrealistic mass"
```

### Debug Unit Issues

**Print unit information**:
```c
/* In your module */
void my_module_init(const struct ModuleContext *ctx) {
  if (MimicConfig.VerboseLevel > 0) {
    printf("[my_module] Unit system:\n");
    printf("  G = %.6f (code units)\n", ctx->params->G);
    printf("  UnitLength = %.3e cm = 1 Mpc/h\n", ctx->params->UnitLength_in_cm);
    printf("  UnitMass = %.3e g = 1e10 Msun\n", ctx->params->UnitMass_in_g);
    printf("  UnitTime = %.3e s = %.3f Myr\n",
           ctx->params->UnitTime_in_s,
           ctx->params->UnitTime_in_Megayears);
  }
}
```

**Verify calculations**:
```c
/* Sanity check a calculation */
double t_merge = 2.0 * Rvir * Rvir * Vvir / (ctx->params->G * Msat);

/* Should be in code time units (comparable to snapshot intervals) */
if (t_merge < 0 || t_merge > 1e5) {
  fprintf(stderr, "WARNING: Suspicious merger time: %.3e\n", t_merge);
  fprintf(stderr, "  Rvir=%.3f, Vvir=%.3f, Msat=%.3f, G=%.3f\n",
          Rvir, Vvir, Msat, ctx->params->G);
}
```

### Compare Against SAGE

If porting from SAGE:
```python
# In scientific test
def test_compare_to_sage():
    """Compare module output to SAGE reference."""
    mimic_data = load_mimic_output("output/test.dat")
    sage_data = load_sage_output("sage_reference/test.dat")

    # Property should match (in same units)
    np.testing.assert_allclose(
        mimic_data['ColdGas'],
        sage_data['ColdGas'],
        rtol=0.01,  # 1% relative tolerance
        err_msg="ColdGas doesn't match SAGE"
    )
```

---

## Debugging Unit Issues

### Common Symptoms

**1. Results off by factor of 10^10**:
- Likely mixing code mass units (10^10 Msun/h) with physical (Msun)
- Check: Are you multiplying/dividing by 1e10 when you shouldn't?

**2. Results off by factor of ~43**:
- Likely computing G incorrectly instead of using `ctx->params->G`
- Check: `printf("G = %.6f\n", your_G_value);` should be ~43.0071

**3. Results off by factor of h**:
- Likely mixing h-full (Msun/h) with h-free (Msun) units
- Check: Hubble_h usage - is it canceling out where it should?

**4. Timescales completely wrong**:
- Likely time units mixed up (code units vs Myr vs Gyr)
- Check: Convert to physical units and verify reasonableness

**5. Extreme values (NaN, Inf, or huge numbers)**:
- Division by zero or unit mismatch in calculation
- Check: Add assertions before divisions, print intermediate values

### Debugging Workflow

1. **Print the unit system** at module initialization (see above)
2. **Verify G value** matches ~43.0071
3. **Check intermediate calculations** with physical reasoning:
   ```c
   printf("t_dyn = %.3e (code) = %.3f Myr (physical)\n",
          t_dyn, t_dyn * ctx->params->UnitTime_in_Megayears);
   ```
4. **Compare to SAGE** if porting (exact same inputs should give same outputs)
5. **Use scientific tests** to catch unit bugs early

### Getting Help

If you're stuck on unit issues:
1. Check this guide for similar patterns
2. Look at existing modules (sage_cooling is well-documented)
3. Check `src/modules/shared/` for utilities
4. Ask in developer channels with:
   - What calculation you're doing
   - What units you expect
   - What you're actually getting
   - Relevant code snippet

---

## Summary

**Remember**:
1. ✅ All internal calculations in code units
2. ✅ Use `ctx->params->G` (never recompute)
3. ✅ Convert only at boundaries (input/output/physics coefficients)
4. ✅ Document assumptions clearly
5. ✅ Test against known values
6. ✅ When in doubt, check existing modules

**Vision Principles**:
- **Physics-Agnostic Core**: Auto-generated code maintains separation
- **Metadata-Driven**: Units declared in YAML, enforced by validation
- **KISS**: One unit system, clear rules, minimal conversions

**For more information**:
- Property metadata: `docs/architecture/property-metadata-schema.md`
- Module development: `docs/developer/module-developer-guide.md`
- Testing: `docs/developer/testing.md`
