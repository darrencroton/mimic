# SAGE Infall Module

**Module**: `sage_infall`
**Version**: 1.0.0
**Phase**: Phase 4.2 (SAGE Physics Module Implementation)
**Status**: Software testing complete, physics validation deferred to Phase 4.3+

---

## Overview

The `sage_infall` module implements cosmological gas infall and satellite stripping processes following the SAGE (Semi-Analytic Galaxy Evolution) model. It handles the accretion of baryons onto dark matter halos from the cosmic web and the environmental effects on satellite galaxies.

**Key Processes**:
- Cosmological gas infall onto central galaxies
- Reionization suppression of gas accretion on low-mass halos
- Environmental stripping of hot gas from satellite galaxies
- Baryon redistribution between galaxy components

**Note**: Physics validation (comparing reionization suppression curves, infall amounts, stripping rates to SAGE reference outputs) is deferred to Phase 4.3+ when downstream modules (cooling, star formation, reincorporation) are implemented, enabling end-to-end mass flow validation.

---

## Physics Description

### Gas Infall

Central galaxies accrete baryonic gas from the cosmic web proportional to the growth of their dark matter halos:

```
infallingMass = f_reion * BaryonFrac * Mvir - (total baryons)
HotGas += infallingMass
```

Where:
- `f_reion` is the reionization suppression factor (see below)
- `BaryonFrac` is the cosmic baryon fraction (Ωb/Ωm ≈ 0.17)
- `Mvir` is the virial mass of the halo
- `total baryons` is the sum of all baryonic components in the galaxy

The infall can be positive (net accretion) or negative (net mass loss from feedback processes).

### Reionization Suppression

After cosmic reionization, the increased Jeans mass prevents low-mass halos from accreting gas. The suppression follows the Gnedin (2000) model with Kravtsov et al. (2004) fitting formulas:

**Three Epochs**:

1. **Before UV background** (a ≤ a₀):
   ```
   f_reion = 1.0  (no suppression)
   ```

2. **Partial reionization** (a₀ < a < ar):
   ```
   f_reion = [1 + (2^(α/3) - 1) * (a - a₀)/(ar - a₀)]^(-3/α)
   α = -0.3 * [(Vvir/Vfilt) - 1]  (Vvir > Vfilt)
   α = 0.0  (Vvir ≤ Vfilt)
   ```

3. **Full reionization** (a ≥ ar):
   ```
   f_reion = (Vvir/Vfilt)^3  (Vvir > Vfilt)
   f_reion = 0.0  (Vvir ≤ Vfilt)
   ```

Where:
- `a` is the cosmological scale factor (a = 1/(1+z))
- `a₀ = 1/(1+z₀)` is the scale factor when the UV background turns on
- `ar = 1/(1+zr)` is the scale factor at full reionization
- `Vvir` is the virial velocity of the halo
- `Vfilt` is the filtering velocity (from halo's Mvir at a₀, evaluated at current z)

The filtering mass concept represents the mass scale below which baryons are prevented from collapsing by the increased pressure of the reionized IGM.

### Satellite Stripping

Satellite galaxies orbiting within larger halos experience environmental effects that gradually strip their hot gas reservoirs:

```
stripped_amount = excess_hot_gas / STRIPPING_STEPS
excess_hot_gas = HotGas - (BaryonFrac * Mvir - other_baryons)
```

The stripping is:
- **Gradual**: Occurs over `STRIPPING_STEPS` timesteps (default: 10)
- **Metal-preserving**: Metallicity of stripped gas is maintained
- **Conservative**: Only removes "excess" hot gas above the equilibrium amount
- **Transferred**: Stripped gas is added to the central galaxy's hot gas reservoir

### Baryon Redistribution

The module consolidates satellite baryonic components to their central galaxies:
- **Ejected gas** from satellites → central's ejected reservoir
- **Intracluster stars** (ICS) from satellites → central's ICS
- Metallicity is tracked and preserved in all transfers

---

## Module Parameters

All parameters are configured in the `.par` file with the `SageInfall_` prefix.

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `SageInfall_BaryonFrac` | double | 0.17 | [0.0, 1.0] | Cosmic baryon fraction (Ωb/Ωm) |
| `SageInfall_ReionizationOn` | int | 1 | [0, 1] | Enable (1) or disable (0) reionization suppression |
| `SageInfall_Reionization_z0` | double | 8.0 | [0.0, 20.0] | Redshift when UV background turns on |
| `SageInfall_Reionization_zr` | double | 7.0 | [0.0, 20.0] | Redshift of full reionization |
| `SageInfall_StrippingSteps` | int | 10 | [1, 100] | Number of substeps for gradual satellite stripping |

**Typical Values**:
- Baryon fraction from Planck cosmology: 0.17
- Reionization redshifts from observations: z₀ ≈ 8.0, zr ≈ 7.0
- Stripping steps: 10 provides smooth stripping without excessive computation

---

## Example Configuration

### Basic Usage

```
% Enable sage_infall module
EnabledModules          sage_infall

% Use default parameters (optional - these are defaults)
SageInfall_BaryonFrac            0.17
SageInfall_ReionizationOn        1
SageInfall_Reionization_z0       8.0
SageInfall_Reionization_zr       7.0
SageInfall_StrippingSteps        10
```

### Disable Reionization

```
% Turn off reionization suppression (for testing or comparison)
EnabledModules          sage_infall
SageInfall_ReionizationOn        0
```

### Custom Reionization History

```
% Earlier reionization with different parameters
EnabledModules          sage_infall
SageInfall_Reionization_z0       10.0
SageInfall_Reionization_zr       8.0
```

### Different Cosmology

```
% Higher baryon fraction (e.g., older cosmology)
EnabledModules          sage_infall
SageInfall_BaryonFrac            0.18
```

---

## Dependencies

### Required Halo Properties
- `Mvir` - Virial mass from halo tracking
- `Vvir` - Virial velocity from halo tracking
- `Type` - Halo type (0=central, 1=satellite)
- `SnapNum` - Snapshot number for redshift calculation

### Provides Galaxy Properties
- `HotGas` - Hot gas in halo atmosphere
- `MetalsHotGas` - Metals in hot gas
- `EjectedMass` - Gas ejected from galaxy (consolidated from satellites)
- `MetalsEjectedMass` - Metals in ejected gas
- `ICS` - Intracluster stars (consolidated from satellites)
- `MetalsICS` - Metals in ICS
- `TotalSatelliteBaryons` - Tracking variable for satellite baryons

### Module Execution Order
The `sage_infall` module should run **early** in the pipeline as it provides the `HotGas` reservoir that other modules (e.g., cooling, star formation) depend on.

**Typical Order**:
```
EnabledModules  sage_infall,sage_cooling,sage_starformation_feedback,...
```

---

## Known Limitations

### Compared to SAGE

1. **No Black Hole Mass**: Mimic does not yet track black hole mass, so it is omitted from baryon accounting. This is a minor effect (<1% of baryons).

2. **Simplified Comparison Functions**: SAGE uses custom comparison functions (`is_greater()`, `is_less()`); Mimic uses standard `<`, `>` operators. Functionally equivalent.

### Numerical Approximations

1. **Gradual Stripping**: The `STRIPPING_STEPS` parameter controls the smoothness of satellite stripping. Lower values (e.g., 1) give instantaneous stripping; higher values (e.g., 10) give smoother evolution at slightly higher computational cost.

2. **Timestep Dependence**: Infall amounts depend on the timestep (ΔMvir). Results are consistent as long as timesteps are reasonable (Δz ~ 0.1 typical).

---

## Implementation Details

### Baryon Conservation

The module enforces strict baryon conservation:
```
total_baryons = StellarMass + ColdGas + HotGas + EjectedMass + ICS
```

For centrals:
```
infallingMass = BaryonFrac * Mvir - total_baryons
```

Negative infall (mass loss) is handled by depleting:
1. **First**: The ejected reservoir
2. **Then**: The hot gas reservoir

This ensures physically reasonable behavior when feedback processes eject more gas than is accreted.

### Metallicity Tracking

All gas transfers preserve metallicity:
```
new_MetalsHotGas = old_MetalsHotGas + transferred_gas * (MetalsGas / Gas)
```

This ensures metal enrichment history is correctly tracked through all baryon cycling processes.

### Numerical Stability

- All mass values are enforced to be non-negative
- Metallicity fractions are clamped to [0, 1]
- Division by zero is protected with `is_zero()` checks
- Floating-point comparisons use tolerance-based comparisons

---

## Testing Status

### Software Quality Testing ✅

**Unit Tests** (`tests/unit/test_sage_infall.c`):
- ✅ Module registration and initialization
- ✅ Parameter reading and configuration
- ✅ Memory safety (no leaks)
- ✅ Property access patterns

**Integration Tests** (`tests/integration/test_sage_infall.py`):
- ✅ Module loads and executes
- ✅ Output properties appear in files
- ✅ Parameters configurable via .par files
- ✅ No memory leaks
- ✅ Works in multi-module pipeline

### Physics Validation ⏸️

**Status**: Deferred to Phase 4.3+

**Rationale**: Physics validation requires comparing complete mass flows through the full galaxy formation pipeline. With only `sage_infall` implemented, we cannot validate:
- Correct HotGas amounts (requires cooling module to consume it)
- Baryon cycling (requires star formation and feedback)
- Reionization suppression effects (requires seeing impact on galaxy populations)

**Plan**: After `sage_cooling`, `sage_starformation_feedback`, and `sage_reincorporation` are implemented, conduct comprehensive physics validation:
- Compare Mimic vs SAGE outputs on identical trees
- Validate mass conservation through full pipeline
- Check reionization suppression curves
- Verify statistical galaxy properties (stellar mass functions, etc.)

---

## References

### Primary References

1. **Croton et al. (2016)** - "The SAGE Model of Galaxy Formation" (SAGE model description)
   - DOI: 10.3847/0067-0049/222/2/22
   - Full model description and parameter values

2. **Gnedin (2000)** - "Effect of Reionization on Structure Formation in the Universe"
   - DOI: 10.1086/308823
   - Reionization suppression model and filtering mass concept

3. **Kravtsov, Gnedin & Klypin (2004)** - "The Tumultuous Lives of Galactic Dwarfs"
   - DOI: 10.1086/425058
   - Fitting formulas for filtering mass and suppression factor

### Implementation Reference

4. **SAGE Source Code** - `sage-code/model_infall.c`
   - Reference implementation in Mimic repository
   - Available at: https://github.com/darrencroton/sage

---

## Version History

**v1.0.0** (2025-11-12):
- Initial implementation
- Software quality testing complete
- Physics validation deferred to Phase 4.3+
- All parameters configurable
- Full metadata-driven registration

---

## Contact & Support

For questions about this module:
- See `docs/developer/module-developer-guide.md` for development patterns
- See `docs/architecture/module-implementation-log.md` for implementation notes
- Check `tests/integration/test_sage_infall.py` for usage examples
