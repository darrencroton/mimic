# SAGE Disk Instability Module

**Status:** ✅ PARTIAL IMPLEMENTATION (v1.0.0)
**Physics:** Disk stability criterion and stellar mass redistribution
**Category:** Stellar Physics

## Overview

This module implements disk instability detection and mass redistribution from the SAGE galaxy evolution model. It evaluates disk stability using the Mo, Mao & White (1998) criterion and transfers unstable mass from disk to bulge components.

**Key Physics:**
- Disk stability analysis based on self-gravity vs. rotation support
- Direct stellar mass transfer from disk to bulge
- Metallicity preservation during mass transfers
- Disk scale radius calculation

## Implementation Status

### ✅ Implemented (v1.0.0)

1. **Disk Stability Criterion**
   - Mo, Mao & White (1998) critical mass calculation
   - Mcrit = Vmax² × (3 × DiskScaleRadius) / G
   - Comparison of disk mass to critical threshold

2. **Stellar Mass Transfer**
   - Direct transfer of unstable disk stars to bulge
   - Metallicity preservation (disk → bulge)
   - Mass conservation checks and validation

3. **Disk Scale Radius**
   - Empirical scaling with virial radius
   - Rd ≈ 0.03 × Rvir (typical for disk galaxies)
   - Property tracking and initialization

4. **Property Management**
   - BulgeMass (stellar mass in bulge)
   - MetalsBulgeMass (metal mass in bulge)
   - MetalsStellarMass (total stellar metals)
   - DiskScaleRadius (disk scale length)

### ⏸ Deferred (Requires sage_mergers Module)

The following components depend on functions from the `sage_mergers` module (Priority 5) which is not yet implemented:

1. **Starburst Triggering**
   - Needs: `collisional_starburst_recipe()` function
   - Physics: Converts unstable gas to stars in bulge
   - Mode: 1 (disk instability-induced burst)
   - Location: sage-code/model_mergers.c:395

2. **Black Hole Growth**
   - Needs: `grow_black_hole()` function
   - Physics: Accretes unstable gas onto supermassive black hole
   - Triggers: Quasar-mode AGN feedback
   - Location: sage-code/model_mergers.c:173

3. **Gas Processing**
   - Currently: Unstable gas remains in ColdGas reservoir
   - Future: Will be converted to stars via starburst recipe
   - Requires: Supernova feedback, metal enrichment infrastructure

**Why Deferred:**
- `collisional_starburst_recipe()` itself calls `check_disk_instability()` (circular dependency)
- Both functions are merger-specific physics best kept in sage_mergers module
- Follows architectural principle: avoid premature abstraction

## Physics Description

### Disk Stability Criterion

Disk instability occurs when the self-gravity of the galactic disk dominates over the gravitational support from the dark matter halo. The criterion follows Mo, Mao & White (1998):

```
disk_mass = ColdGas + (StellarMass - BulgeMass)
Mcrit = Vmax² × Reff / G
```

where:
- `Reff = 3 × DiskScaleRadius` (effective disk radius, ~3 scale lengths)
- `Vmax` = maximum circular velocity (km/s)
- `G` = gravitational constant in code units

**Stability Condition:**
- `disk_mass ≤ Mcrit`: Stable disk (no action)
- `disk_mass > Mcrit`: Unstable disk (transfer excess mass to bulge)

### Mass Redistribution (Current Implementation)

When instability is detected:

1. **Calculate Unstable Masses:**
   ```
   gas_fraction = ColdGas / disk_mass
   star_fraction = (StellarMass - BulgeMass) / disk_mass
   unstable_gas = gas_fraction × (disk_mass - Mcrit)
   unstable_stars = star_fraction × (disk_mass - Mcrit)
   ```

2. **Transfer Unstable Stars:**
   ```
   metallicity = MetalsDisk / DiskStellarMass
   BulgeMass += unstable_stars
   MetalsBulgeMass += metallicity × unstable_stars
   ```

3. **Track Unstable Gas:**
   - Gas instability detected and logged
   - Processing deferred to sage_mergers starburst recipe
   - Currently remains in ColdGas reservoir

### Disk Scale Radius

The disk scale radius is calculated using an empirical scaling:

```
DiskScaleRadius = 0.03 × Rvir
```

Typical values:
- Dwarf galaxies: Rvir ~ 0.1 Mpc/h → Rd ~ 3 kpc/h
- Milky Way: Rvir ~ 0.2 Mpc/h → Rd ~ 6 kpc/h
- Massive galaxies: Rvir ~ 1 Mpc/h → Rd ~ 30 kpc/h

**Future Enhancement:**
Full Mo, Mao & White (1998) spin-dependent formula:
```
Rd = (λ / √2) × (jd / md) × Rvir
```
where λ is spin parameter (requires spin from halo properties).

## Module Configuration

### Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `DiskInstabilityOn` | int | 1 | [0, 1] | Enable disk instability physics (0=off, 1=on) |
| `DiskRadiusFactor` | double | 3.0 | [1.0, 10.0] | Factor for disk effective radius (N × scale radius) |

### Example Configuration

```ini
# Enable disk instability module
EnabledModules = sage_infall, sage_cooling, sage_disk_instability

# Module parameters
SageDiskInstability_DiskInstabilityOn = 1
SageDiskInstability_DiskRadiusFactor = 3.0
```

## Dependencies

### Required Properties (Input)

| Property | Source | Description |
|----------|--------|-------------|
| `ColdGas` | sage_cooling | Cold gas mass available for star formation |
| `MetalsColdGas` | sage_cooling | Metal mass in cold gas |
| `StellarMass` | star formation | Total stellar mass |
| `Vmax` | halo properties | Maximum circular velocity |

### Provided Properties (Output)

| Property | Type | Units | Description |
|----------|------|-------|-------------|
| `BulgeMass` | float | 1e10 Msun/h | Stellar mass in bulge component |
| `MetalsBulgeMass` | float | 1e10 Msun/h | Metal mass in bulge |
| `MetalsStellarMass` | float | 1e10 Msun/h | Total metal mass in stars |
| `DiskScaleRadius` | float | Mpc/h | Disk scale radius (exponential disk) |

## Testing

### Unit Tests

`test_unit_sage_disk_instability.c` tests:
- ✓ Stability criterion calculation
- ✓ Critical mass formula validation
- ✓ Stellar mass transfer mechanics
- ✓ Metallicity preservation
- ✓ Edge cases (zero mass, pure disk, pure bulge)

### Integration Tests

`test_integration_sage_disk_instability.py` validates:
- ✓ Module initialization and parameter reading
- ✓ Property creation and tracking
- ✓ Mass conservation during transfers
- ✓ Bulge growth over multiple timesteps

### Scientific Validation

`test_scientific_sage_disk_instability.py` will verify:
- Bulge mass fractions match SAGE benchmarks
- Disk scale radii are physically reasonable
- Stability criterion agrees with SAGE implementation

## Future Roadmap

### Version 2.0.0 (When sage_mergers is Implemented)

1. **Add Starburst Triggering:**
   ```c
   if (unstable_gas > 0.0) {
     double unstable_gas_fraction = unstable_gas / galaxy->ColdGas;
     collisional_starburst_recipe(unstable_gas_fraction, p, centralgal,
                                  time, dt, halonr, mode=1, step);
   }
   ```

2. **Add Black Hole Growth:**
   ```c
   if (AGN_RECIPE_ON && unstable_gas > 0.0) {
     double unstable_gas_fraction = unstable_gas / galaxy->ColdGas;
     grow_black_hole(p, unstable_gas_fraction);
   }
   ```

3. **Complete Gas Processing:**
   - Integrate with supernova feedback system
   - Handle metal enrichment from starbursts
   - Track energy budgets

### Version 3.0.0 (Spin-Dependent Disk Sizes)

1. **Implement Full MMW98 Disk Model:**
   - Calculate disk size from spin parameter
   - Include angular momentum conservation
   - Handle disk growth over cosmic time

2. **Add Disk Morphology:**
   - Track disk vs bulge formation history
   - Implement disk truncation physics
   - Model pseudo-bulges from secular evolution

## References

### Primary References

1. **Mo, Mao & White (1998)** - *MNRAS, 295, 319*
   - Disk formation in CDM halos
   - Stability criterion derivation
   - Angular momentum model

2. **Efstathiou, Lake & Negroponte (1982)** - *MNRAS, 199, 1069*
   - Disk instability in N-body simulations
   - Critical surface density for stability

3. **Croton et al. (2016)** - *ApJS, 222, 22*
   - SAGE model overview
   - Implementation details

### SAGE Source Code

- `sage-code/model_disk_instability.c` - Original SAGE implementation
- `sage-code/model_mergers.c:173` - `grow_black_hole()` function
- `sage-code/model_mergers.c:395` - `collisional_starburst_recipe()` function

## Development Notes

### Why Partial Implementation?

This module was implemented as Priority 6 in the roadmap but depends on functions from Priority 5 (sage_mergers). Rather than:
1. Wait months to implement everything in order
2. Create stub functions that don't work
3. Extract merger physics to shared utilities (architectural violation)

We chose to:
1. Implement what's possible NOW (core stability physics)
2. Defer components with clear dependencies
3. Document implementation status transparently
4. Provide clear upgrade path when dependencies resolve

### Architectural Decisions

1. **Self-Contained Module:** All disk instability logic in one module
2. **Shared Utilities:** Uses `mimic_get_metallicity()` from shared/
3. **Clear Dependencies:** Documented in module_info.yaml
4. **Testable Implementation:** Core physics has comprehensive unit tests
5. **Future-Proof:** Easy to extend when sage_mergers exists

### Development Timeline

- **v1.0.0** (Current): Core stability criterion and stellar transfer
- **v2.0.0** (After sage_mergers): Full gas processing and AGN
- **v3.0.0** (Future): Spin-dependent disk sizes and morphology

---

**Last Updated:** 2025-11-17
**Module Version:** 1.0.0
**Implementation:** Partial (Core Physics Only)
