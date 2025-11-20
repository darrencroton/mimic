# SAGE Star Formation & Feedback Module

**Module Name**: `sage_starformation_feedback`
**Version**: 1.0.0
**Author**: Mimic Team (ported from SAGE)
**Category**: Star Formation
**Status**: Production (Phase 4.2)

## Overview

This module implements star formation in galaxy disks and supernova-driven feedback (reheating and ejection) from the SAGE (Semi-Analytic Galaxy Evolution) model. It handles the conversion of cold gas into stars and the subsequent feedback processes that regulate star formation.

## Physics

### Star Formation

Star formation follows the **Kennicutt-Schmidt law** with a **critical gas surface density threshold** (Kauffmann 1996):

**Effective Star-Forming Radius**:
```
r_eff = 3 * R_disk
```

**Dynamical Time**:
```
τ_dyn = r_eff / V_vir
```

**Critical Gas Mass** (Kauffmann 1996 eq. 7):
```
M_crit = 0.19 * V_vir * r_eff
```

**Star Formation Rate**:
```
SFR = ε_SF * (M_cold - M_crit) / τ_dyn    if M_cold > M_crit
    = 0                                    otherwise
```

**Stars Formed per Timestep**:
```
Δm_* = SFR * Δt
```

where:
- `ε_SF` is the star formation efficiency (parameter: `SfrEfficiency`)
- `M_cold` is the cold gas mass
- `M_crit` is the critical gas mass for star formation
- `V_vir` is the virial velocity
- `R_disk` is the disk scale radius
- `Δt` is the timestep

### Supernova Feedback

#### Reheating

Cold gas is reheated to the hot phase within the halo:

```
m_reheat = ε_reheat * Δm_*
```

where `ε_reheat` is the reheating efficiency (parameter: `FeedbackReheatingEpsilon`).

**Mass Flow**: Cold gas → Hot gas (within halo)

#### Ejection

Hot gas is ejected from the halo using an **energy-driven outflow model**:

```
m_eject = (η * ε_eject * E_SN / V_vir² - ε_reheat) * Δm_*
```

where:
- `η` is the supernova efficiency (parameter: `EtaSNcode`)
- `ε_eject` is the ejection efficiency (parameter: `FeedbackEjectionEfficiency`)
- `E_SN` is the supernova energy (parameter: `EnergySNcode`)
- `V_vir` is the virial velocity of the central halo

The term `η * E_SN` represents the specific energy (energy per unit stellar mass) deposited by supernovae. When this exceeds the binding energy of the halo (∝ V_vir²), gas can be ejected.

**Mass Flow**: Hot gas → Ejected reservoir (outside halo)

#### Mass Conservation

The module ensures that the total gas consumed (stars + reheating) never exceeds the available cold gas:

```
if Δm_* + m_reheat > M_cold:
    scale = M_cold / (Δm_* + m_reheat)
    Δm_* *= scale
    m_reheat *= scale
```

### Metal Enrichment

New metals are produced from star formation using the **instantaneous recycling approximation**:

**Mass-Dependent Metal Distribution** (Krumholz & Dekel 2011):
```
f_leave = f_0 * exp(-M_vir / 30)
```

**Metal Production**:
```
ΔZ_cold = Y * (1 - f_leave) * Δm_*
ΔZ_hot  = Y * f_leave * Δm_*
```

where:
- `Y` is the metal yield (parameter: `Yield`)
- `f_0` is the normalization (parameter: `FracZleaveDisk`)
- `M_vir` is the virial mass of the central halo

In massive halos, most metals stay in the cold gas. In low-mass halos, metals preferentially escape to the hot gas.

### Recycling

A fraction of stellar mass is immediately returned to the ISM through stellar winds and Type II supernovae:

```
M_cold  -= (1 - f_rec) * Δm_*
M_stars += (1 - f_rec) * Δm_*
```

where `f_rec` is the recycling fraction (parameter: `RecycleFraction`, typically 0.43 for Chabrier IMF).

### Disk Structure

Disk scale radius is computed using the **Mo, Mao & White (1998) model**:

```
λ = |J| / (√2 * V_vir * R_vir)    (Bullock-style spin parameter)
R_disk = (λ / √2) * R_vir
```

where `|J|` is the magnitude of the halo's angular momentum (spin) vector.

## Dependencies

### Required Modules (Upstream)
- **sage_infall**: Provides `HotGas`, `MetalsHotGas`, `EjectedMass`, `MetalsEjectedMass`
- **sage_cooling**: Provides `ColdGas`, `MetalsColdGas`

### Required Halo Properties
- `Mvir`: Virial mass
- `Vvir`: Virial velocity
- `Rvir`: Virial radius
- `Spin[3]`: Angular momentum vector
- `dT`: Timestep

### Execution Order
This module must run **after** `sage_infall` and `sage_cooling` to have access to the gas reservoirs.

## Properties

### Modified Properties
- **StellarMass** (`float`): Stellar mass (1e10 Msun/h) - incremented by star formation

### Created Properties
- **MetalsStellarMass** (`float`): Metal mass in stars (1e10 Msun/h)
- **DiskScaleRadius** (`float`): Disk scale radius (Mpc/h)
- **OutflowRate** (`float`): Gas outflow rate from feedback (1e10 Msun/h / time_unit)

## Parameters

### Star Formation Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `SFprescription` | int | 0 | [0, 0] | Star formation recipe (0 = Kennicutt-Schmidt with threshold, only option) |
| `SfrEfficiency` | double | 0.02 | [0.0, 1.0] | Star formation efficiency ε_SF |
| `RecycleFraction` | double | 0.43 | [0.0, 1.0] | Fraction of stellar mass recycled to ISM |

### Feedback Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `SupernovaRecipeOn` | int | 1 | [0, 1] | Enable supernova feedback (0=off, 1=on) |
| `FeedbackReheatingEpsilon` | double | 3.0 | [0.0, 100.0] | Reheating efficiency ε_reheat |
| `FeedbackEjectionEfficiency` | double | 0.3 | [0.0, 100.0] | Ejection efficiency ε_eject |
| `EnergySNcode` | double | 1.0 | [0.0, 100.0] | Supernova energy in code units |
| `EtaSNcode` | double | 0.5 | [0.0, 10.0] | Supernova efficiency in code units (SNe per unit stellar mass) |

### Metal Enrichment Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `Yield` | double | 0.03 | [0.0, 1.0] | Metal yield from star formation |
| `FracZleaveDisk` | double | 0.3 | [0.0, 1.0] | Normalization for metals leaving disk |

### Other Parameters

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `DiskInstabilityOn` | int | 0 | [0, 1] | Enable disk instability checks (currently stubbed) |

## Configuration Example

```
# Star formation and feedback configuration
EnabledModules  sage_infall,sage_cooling,sage_starformation_feedback

# Star formation parameters
SageStarformationFeedback_SFprescription  0
SageStarformationFeedback_SfrEfficiency  0.02
SageStarformationFeedback_RecycleFraction  0.43

# Feedback parameters
SageStarformationFeedback_SupernovaRecipeOn  1
SageStarformationFeedback_FeedbackReheatingEpsilon  3.0
SageStarformationFeedback_FeedbackEjectionEfficiency  0.3
SageStarformationFeedback_EnergySNcode  1.0
SageStarformationFeedback_EtaSNcode  0.5

# Metal enrichment
SageStarformationFeedback_Yield  0.03
SageStarformationFeedback_FracZleaveDisk  0.3

# Disk instability (deferred to future module)
SageStarformationFeedback_DiskInstabilityOn  0
```

## Implementation Details

### Module Lifecycle
1. **Init**: Reads parameters, validates ranges, logs configuration
2. **Process**: For each halo:
   - Computes disk scale radius
   - Calculates star formation rate
   - Computes feedback (reheating and ejection)
   - Updates galaxy properties
   - Enriches gas with metals
3. **Cleanup**: No persistent memory to free

### Key Implementation Choices

1. **No Sub-Stepping**: Unlike SAGE, Mimic doesn't use sub-timesteps. Star formation history arrays (`SfrDisk[STEPS]`) are omitted in this version. They can be added later if needed for stellar population synthesis.

2. **Disk Instability Stub**: Calls to `check_disk_instability()` are stubbed out. This will be implemented in the `sage_disk_instability` module (Priority 6).

3. **Shared Utilities**: Uses `mimic_get_metallicity()` for metallicity calculations and `mimic_get_disk_radius()` for disk structure.

4. **Central Galaxy Feedback**: For satellite galaxies, feedback products (reheated gas, ejected gas, metals) go to the central galaxy's reservoirs, not the satellite itself.

### Simplifications vs. SAGE

1. **No SFR history tracking**: `SfrDisk[STEPS]` arrays not implemented (not needed without stellar population synthesis)
2. **No disk instability**: Deferred to `sage_disk_instability` module
3. **Code units parameters**: `EnergySNcode` and `EtaSNcode` are direct parameters instead of being derived from `EnergySN` and `EtaSN` with unit conversions

## Testing Status

### Software Quality Testing ✅

**Unit Tests** (9 tests passing):
- Module registration and lifecycle
- Parameter reading and validation
- Memory safety (no leaks)
- Property access patterns
- Disk radius calculation
- Basic star formation logic
- Basic feedback logic

**Integration Tests** (7 tests passing):
- Module loads correctly
- Output properties present (StellarMass, MetalsStellarMass, DiskScaleRadius, OutflowRate)
- Parameters configurable via YAML files
- Feedback toggle works (SupernovaRecipeOn)
- No memory leaks
- Full pipeline execution
- Three-module integration (sage_infall + sage_cooling + sage_starformation_feedback)

### Physics Validation ⏸️

**Status**: Deferred

**Rationale**: Physics validation requires complete SAGE module chain (including sage_reincorporation, sage_mergers, sage_disk_instability) to validate full baryon cycle and mass conservation.

**Plan**: After downstream modules complete:
- Compare Mimic vs SAGE outputs on identical trees
- Validate stellar mass functions against observations
- Check star formation rate distributions
- Verify mass conservation through full pipeline
- Test feedback suppression in low-mass halos

## References

### Key Papers

1. **Kennicutt (1998)**: "The Global Schmidt Law in Star-forming Galaxies"
   - ApJ, 498, 541
   - Establishes the Kennicutt-Schmidt star formation law

2. **Kauffmann et al. (1996)**: "The Formation and Evolution of Galaxies Within Merging Dark Matter Haloes"
   - MNRAS, 283, L117
   - Critical gas density threshold for star formation

3. **Krumholz & Dekel (2011)**: "Star Formation in Atomic Gas"
   - ApJ, 740, 74
   - Mass-dependent metal ejection (Equation 22)

4. **Mo, Mao & White (1998)**: "Analytic model for the formation and evolution of disk galaxies"
   - MNRAS, 295, 319
   - Disk scale radius model (spin-based)

5. **Croton et al. (2016)**: "Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results"
   - ApJS, 222, 22
   - Complete SAGE model description

### SAGE Source Code
- **Original Implementation**: `sage-code/model_starformation_and_feedback.c`

## Known Limitations

1. **No SFR History**: Star formation rate history arrays not implemented (can be added if stellar population synthesis is needed)

2. **Disk Instability Stub**: Disk instability checks are stubbed out and will be implemented in a future module

3. **Simplified Energy Units**: Uses code-unit parameters directly instead of SAGE's complex unit conversion system

4. **No Bulge Star Formation**: This module implements disk star formation only. Bulge star formation from disk instability will be handled by the `sage_disk_instability` module.

## Future Enhancements

1. **Star Formation History**: Add `SfrDisk[STEPS]` arrays if stellar population synthesis is needed for magnitude calculations

2. **Variable IMF**: Support for different initial mass functions (currently assumes Chabrier IMF)

3. **Metallicity-Dependent SF**: Star formation efficiency could depend on gas metallicity

4. **H2-Based SF**: Alternative star formation recipes based on molecular hydrogen fraction

## Developer Notes

### Adding New Star Formation Recipes

To add a new SF prescription (e.g., `SFprescription=1`):

1. Add new case to `if (SF_PRESCRIPTION == ...)` block in `sage_starformation_feedback_process()`
2. Implement alternative SF rate calculation
3. Update parameter validation in `init()` to accept new prescription value
4. Document new recipe in this README

### Modifying Feedback Model

To modify feedback:

1. Edit `update_from_feedback()` helper function
2. Adjust ejection formula or reheating efficiency
3. Ensure mass conservation is maintained
4. Test with full module chain to verify baryon cycle

### Code Organization

```
sage_starformation_feedback/
├── sage_starformation_feedback.c       # Main implementation
├── sage_starformation_feedback.h       # Public interface
├── module_info.yaml                     # Module metadata (auto-registration)
├── README.md                            # This file
├── test_unit_sage_starformation_feedback.c            # C unit tests
├── test_integration_sage_starformation_feedback.py    # Python integration tests
└── test_scientific_sage_starformation_feedback_validation.py  # Physics validation (deferred)
```

## Contact

For questions or issues:
- Check module implementation: `src/modules/sage_starformation_feedback/sage_starformation_feedback.c`
- Review test cases: `test_unit_sage_starformation_feedback.c`
- Consult SAGE source: `sage-code/model_starformation_and_feedback.c`
- See architectural documentation: `docs/architecture/vision.md`
