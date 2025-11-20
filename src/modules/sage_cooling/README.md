# SAGE Cooling & AGN Heating Module

**Module Name:** `sage_cooling`
**Version:** 1.0.0
**Category:** Gas Physics
**Author:** Mimic Team (ported from SAGE)

---

## Overview

The `sage_cooling` module implements gas cooling from hot dark matter halos onto cold galactic disks, with AGN radio-mode feedback that suppresses cooling in massive galaxies. This module is a core component of the SAGE (Semi-Analytic Galaxy Evolution) model.

### Key Processes

1. **Gas Cooling** - Hot halo gas cools onto the central galaxy disk
   - Two cooling regimes: cold accretion (rapid) vs hot halo cooling (gradual)
   - Metallicity-dependent cooling rates from Sutherland & Dopita (1993) tables
   - Temperature-dependent cooling using virial properties

2. **AGN Feedback** - Supermassive black hole growth and heating
   - Three accretion modes: empirical (default), Bondi-Hoyle, cold cloud
   - Eddington-limited accretion
   - Radio-mode heating suppresses cooling
   - Cumulative heating radius tracks feedback history

3. **Energy Budget Tracking** - Cooling and heating energies
   - `Cooling` property tracks cumulative cooling energy
   - `Heating` property tracks cumulative AGN heating energy
   - Energy conservation maintained throughout

---

## Physics Implementation

### Cooling Recipe

The cooling model follows White & Frenk (1991) with an isothermal density profile:

**Virial Temperature:**
```
T_vir = 35.9 × V_vir²   (K, with V_vir in km/s)
```

**Cooling Function:**
```
Λ(T, Z) = Sutherland & Dopita (1993) tables
```
- 8 metallicity bins: primordial, -3.0, -2.0, -1.5, -1.0, -0.5, 0.0, +0.5 dex
- 91 temperature points: log(T) = 4.0 to 8.5 (steps of 0.05 dex)
- 2D interpolation in (log T, log Z) space

**Cooling Radius:**
```
r_cool = sqrt(ρ₀ / ρ_rcool)

where:
  ρ₀ = HotGas / (4π R_vir)           (central density, isothermal profile)
  ρ_rcool = (m_p k_B T / Λ) / t_cool × 0.885
  t_cool = R_vir / V_vir              (dynamical time)
```

**Two Cooling Regimes:**

1. **Cold Accretion** (r_cool > R_vir):
   ```
   coolingGas = HotGas × (V_vir / R_vir) × dt
   ```
   All hot gas cools rapidly on dynamical timescale

2. **Hot Halo Cooling** (r_cool < R_vir):
   ```
   coolingGas = (HotGas / R_vir) × (r_cool / 2t_cool) × dt
   ```
   Only gas within cooling radius cools

### AGN Feedback

**Three Accretion Modes:**

1. **Empirical (default, mode=1):**
   ```
   AGNrate = η_radio × (M_BH / 0.01) × (V_vir / 200)³ × (HotGas/Mvir / 0.1)
   ```

2. **Bondi-Hoyle (mode=2):**
   ```
   AGNrate = 2.5π G × (0.375 × 0.6 × x) × M_BH × η_radio
   ```
   where `x` is the cooling coefficient from cooling_recipe

3. **Cold Cloud (mode=3):**
   ```
   AGNrate = 0.0001 × (coolingGas / dt)    if M_BH > M_thresh
   AGNrate = 0                              otherwise

   M_thresh = 0.0001 × M_vir × (r_cool / R_vir)³
   ```

**Eddington Limit:**
```
L_Edd = 1.3×10³⁸ (M_BH / M_sun) erg/s
EDDrate = L_Edd / (0.1 × c² efficiency)
AGNrate = min(AGNrate, EDDrate)
```

**Heating Suppression:**
```
AGNcoeff = (1.34×10⁵ / V_vir)²
AGNheating = AGNcoeff × AGNaccreted
coolingGas_final = max(coolingGas - AGNheating, 0)
```

**Heating Radius Update:**
```
r_heat_new = (AGNheating / coolingGas) × r_cool
r_heat = max(r_heat_old, r_heat_new)
```

---

## Properties

### Required Inputs

| Property | Source | Description |
|----------|--------|-------------|
| `HotGas` | sage_infall | Hot gas mass in halo (10¹⁰ M_sun/h) |
| `MetalsHotGas` | sage_infall | Metal mass in hot gas (10¹⁰ M_sun/h) |
| `Mvir` | Halo tracking | Virial mass (10¹⁰ M_sun/h) |
| `Rvir` | Halo tracking | Virial radius (Mpc/h) |
| `Vvir` | Halo tracking | Virial velocity (km/s) |
| `dT` | Halo tracking | Time since progenitor (Myr) |

### Created/Modified Outputs

| Property | Type | Units | Description |
|----------|------|-------|-------------|
| `ColdGas` | float | 10¹⁰ M_sun/h | Cold gas mass (disk) |
| `MetalsColdGas` | float | 10¹⁰ M_sun/h | Metal mass in cold gas |
| `BlackHoleMass` | float | 10¹⁰ M_sun/h | Central SMBH mass |
| `Cooling` | double | (km/s)² × 10¹⁰ M_sun/h | Cumulative cooling energy |
| `Heating` | double | (km/s)² × 10¹⁰ M_sun/h | Cumulative AGN heating energy |
| `r_heat` | float | Mpc/h | Heating radius from past feedback |

---

## Parameters

### Configuration

Add to your parameter file:

```ini
EnabledModules    sage_infall,sage_cooling

# SAGE Cooling Parameters
SageCooling_RadioModeEfficiency    0.01    # AGN feedback efficiency (default: 0.01)
SageCooling_AGNrecipeOn            1       # AGN mode: 0=off, 1=empirical, 2=Bondi, 3=cold cloud
```

### Parameter Details

**`SageCooling_RadioModeEfficiency`** (default: 0.01)
- Controls strength of AGN feedback
- Higher values → stronger feedback → more cooling suppression
- Typical range: 0.001 - 0.1
- Default matches SAGE best-fit value

**`SageCooling_AGNrecipeOn`** (default: 1)
- Selects black hole accretion model
- 0 = No AGN (cooling only, no feedback)
- 1 = Empirical scaling (default, recommended)
- 2 = Bondi-Hoyle accretion (gas density dependent)
- 3 = Cold cloud accretion (threshold-triggered)

---

## Dependencies

### Required Modules

**`sage_infall`** (or equivalent) - Must run before `sage_cooling`
- Provides `HotGas` and `MetalsHotGas` properties
- Without infall, no hot gas exists to cool

### Module Execution Order

```
sage_infall  →  sage_cooling
```

The module system automatically enforces this order based on property dependencies declared in `module_info.yaml`.

---

## Usage Examples

### Basic Cooling (No AGN)

```ini
EnabledModules    sage_infall,sage_cooling

SageInfall_BaryonFrac          0.17
SageCooling_RadioModeEfficiency    0.0
SageCooling_AGNrecipeOn            0
```

### Full SAGE Cooling + AGN (Recommended)

```ini
EnabledModules    sage_infall,sage_cooling

SageInfall_BaryonFrac          0.17
SageInfall_ReionizationOn      1

SageCooling_RadioModeEfficiency    0.01
SageCooling_AGNrecipeOn            1
```

### Strong AGN Feedback

```ini
EnabledModules    sage_infall,sage_cooling

SageInfall_BaryonFrac          0.17
SageCooling_RadioModeEfficiency    0.1    # 10x stronger feedback
SageCooling_AGNrecipeOn            1
```

### Bondi-Hoyle Accretion Mode

```ini
EnabledModules    sage_infall,sage_cooling

SageInfall_BaryonFrac          0.17
SageCooling_RadioModeEfficiency    0.01
SageCooling_AGNrecipeOn            2      # Bondi-Hoyle mode
```

---

## Testing

### Unit Tests

Run unit tests:
```bash
cd tests/unit
./test_unit_sage_cooling.test
```

Tests cover:
- Module registration and initialization
- Cooling table loading (8 metallicity tables)
- Temperature interpolation accuracy
- 2D metallicity-dependent interpolation
- Edge cases: primordial gas, super-solar Z, extreme temperatures
- Memory safety (no leaks)
- Parameter reading and validation

### Integration Tests

Run integration tests:
```bash
cd tests/integration
python test_integration_sage_cooling.py
```

Tests cover:
- Module loading and pipeline integration
- sage_infall → sage_cooling property flow
- Hot gas → cold gas transfer
- Black hole growth via AGN accretion
- All three AGN modes
- Energy budget tracking
- Memory leak detection
- Module ordering dependency

### Expected Results

✅ **9 unit tests** - All should pass
✅ **11 integration tests** - All should pass
✅ **No memory leaks** - Verified by test framework
✅ **Clean exit** - Return code 0

---

## Implementation Notes

### Cooling Tables

The cooling function tables are stored in:
```
src/modules/sage_cooling/CoolFunctions/
```

**Files:**
- `stripped_mzero.cie` - Primordial (Z = 0)
- `stripped_m-30.cie` - [Fe/H] = -3.0
- `stripped_m-20.cie` - [Fe/H] = -2.0
- `stripped_m-15.cie` - [Fe/H] = -1.5
- `stripped_m-10.cie` - [Fe/H] = -1.0
- `stripped_m-05.cie` - [Fe/H] = -0.5
- `stripped_m-00.cie` - [Fe/H] = 0.0 (solar)
- `stripped_m+05.cie` - [Fe/H] = +0.5 (super-solar)

These tables are **model-specific** and stored with the module (not in global `input/` directory) following Mimic's architectural principle that physics modules are self-contained.

### Time Step Handling

The module uses the `dT` halo property (time since progenitor in Myr) for the cooling time step:

```c
dt = halo.dT / UnitTime_in_Megayears  // Convert Myr to code units
```

This ensures physically correct cooling integration across snapshots.

### Central vs Satellite Galaxies

**Only central galaxies (Type == 0) cool:**
- Centrals have fresh gas infall from the cosmic web
- Satellites have truncated infall (handled by sage_infall)
- Satellites can only use their existing cold gas reservoir

### Memory Management

All cooling tables are **static arrays** (not dynamic):
- Allocated once during `sage_cooling_init()`
- No per-tree allocation/deallocation
- Zero memory leaks (verified by tests)

### Unit Conversions

The module carefully handles unit conversions:
- **Temperature:** K (from V_vir² relation)
- **Cooling rate:** erg cm³ s⁻¹ (from tables) → code units
- **Time:** Myr (from dT) → code units
- **Mass:** 10¹⁰ M_sun/h (code units)
- **Energy:** (km/s)² × 10¹⁰ M_sun/h (code units)

---

## References

### Primary Papers

1. **White & Frenk (1991)** - Cooling model framework
   - Isothermal density profile
   - Cooling radius concept
   - ApJ, 379, 52

2. **Croton et al. (2006)** - AGN feedback implementation
   - Radio-mode feedback
   - Empirical accretion scaling
   - MNRAS, 365, 11

3. **Sutherland & Dopita (1993)** - Cooling function tables
   - Collisional ionization equilibrium
   - Metallicity-dependent cooling
   - ApJS, 88, 253

### SAGE Model

4. **Croton et al. (2016)** - SAGE model description
   - Complete model overview
   - Parameter calibration
   - ApJS, 222, 22

5. **SAGE Source Code**
   - `sage-code/model_cooling_heating.c` - Main cooling/AGN physics
   - `sage-code/core_cool_func.c` - Cooling table infrastructure
   - GitHub: https://github.com/darrencroton/sage

### Additional Physics

6. **Kauffmann et al. (1999)** - Cooling implementation details
   - MNRAS, 303, 188

7. **Gnedin (2000)** - Reionization effects on cooling
   - (Used by sage_infall, affects HotGas available for cooling)
   - ApJ, 542, 535

---

## Known Limitations

### Compared to SAGE

1. **Linear Interpolation**: Cooling tables use linear interpolation in both log(T) and log(Z) dimensions. SAGE uses the same approach, so results are equivalent.

2. **Timestep Dependence**: Cooling and AGN feedback rates depend on the timestep Δt (from halo property `dT`). Results are physically consistent as long as timesteps are reasonable (Δt << tcool for most halos).

3. **Static Cooling Tables**: The cooling function tables are loaded at initialization and remain fixed. Temperature and metallicity evolution within a single timestep are not tracked dynamically.

### Numerical Approximations

1. **Isothermal Density Profile**: Assumes hot gas follows an isothermal sphere (ρ ∝ 1/r²). This is a standard approximation that matches SAGE.

2. **Instantaneous Energy Balance**: AGN heating is applied instantaneously within each timestep. The heating radius (`r_heat`) tracks cumulative effects across timesteps.

3. **Cooling Radius Calculation**: Uses the approximate relation rcool = sqrt(ρ₀/ρ_rcool) rather than solving the full integral. This matches SAGE's implementation.

### Physical Simplifications

1. **No Partial Ionization**: Assumes fully ionized gas (μ = 0.59). Neutral/molecular gas cooling not modeled separately.

2. **No Metal Cooling Physics**: While metallicity affects the cooling rate via tables, individual metal species and their ionization states are not tracked.

3. **Single-Phase Gas Model**: Hot gas is treated as a single-temperature phase. Temperature stratification and multi-phase structure not resolved.

### Known Differences from SAGE

1. **String Parameters**: The `CoolFunctionsDir` parameter is now runtime-configurable (previously hardcoded in original SAGE).

2. **Error Handling**: Mimic provides more detailed error messages for missing cooling table files with full path information.

3. **Memory Management**: Uses Mimic's centralized memory tracking system instead of SAGE's direct malloc/free.

---

## Version History

### v1.0.0 (2025-11-13)
- Initial implementation
- Full SAGE cooling physics
- Three AGN accretion modes
- Comprehensive test suite (20 tests)
- Self-contained cooling tables
- Uses core dT property for time steps

---

## Developer Notes

### Adding New AGN Modes

To add a new AGN accretion recipe:

1. Add new mode number to `AGNrecipeOn` parameter documentation
2. Implement in `do_AGN_heating()` function:
   ```c
   else if (AGN_RECIPE_ON == 4) {
       // Your new accretion model here
       AGNrate = your_calculation(...);
   }
   ```
3. Update this README
4. Add test case in integration tests

### Modifying Cooling Tables

To use different cooling tables:

1. Replace files in `CoolFunctions/` directory
2. Ensure format matches: 12 columns, 91 rows per file
3. Update metallicity values in `cooling_tables.c` if needed
4. Re-run unit tests to verify interpolation

### Performance Optimization

Current implementation is optimized for clarity over performance:
- Cooling table lookups: ~O(1) with interpolation
- Per-halo processing: O(n) where n = number of centrals
- No dynamic memory allocation in hot path
- Suitable for Millennium-scale simulations

Potential optimizations (if needed):
- Cache cooling rates for common (T, Z) pairs
- Vectorize halo processing loop
- Pre-compute virial temperatures

---

## Support

For questions, issues, or contributions:
- Review `docs/developer/module-developer-guide.md`
- Check test cases for usage examples
- See `docs/architecture/vision.md` for design principles

---

**Module Status:** ✅ Production Ready
**Test Coverage:** ✅ Comprehensive (20 tests)
**Documentation:** ✅ Complete
**Physics Validation:** ⚠️  User responsibility (compare with SAGE outputs)
