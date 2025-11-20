# Simple Star Formation Module

**Module**: `simple_sfr`
**Version**: 0.1.0
**Status**: Proof-of-Concept (testing infrastructure only, not production physics)

---

## Overview

The `simple_sfr` module is a **placeholder module** designed for testing Mimic's module system infrastructure. It implements a simplified Kennicutt-Schmidt-like star formation law to demonstrate:

- Runtime module configuration
- Inter-module dependencies (reads `ColdGas` from cooling module)
- Time evolution with proper timesteps
- Property modification and dependencies

**This is NOT a realistic star formation model.** It serves as a working example for module development and testing.

---

## Physics Description

### Star Formation Law

The module implements a simplified star formation prescription:

```
ΔStellarMass = ε_SF * ColdGas * (Vvir/Rvir) * Δt
```

Where:
- `ε_SF` is the star formation efficiency parameter
- `ColdGas` is the cold gas mass (provided by cooling module)
- `Vvir/Rvir` represents inverse dynamical time
- `Δt` is the timestep between snapshots

### Processing Logic

For each central galaxy (Type=0):
1. Check if cold gas is available
2. Calculate inverse dynamical time: `Vvir/Rvir`
3. Compute star formation: `ΔStellarMass = ε_SF * ColdGas * (Vvir/Rvir) * Δt`
4. Clamp to available gas (prevent negative values)
5. Update properties:
   - `ColdGas -= ΔStellarMass` (deplete gas)
   - `StellarMass += ΔStellarMass` (form stars)

**Simplifications**:
- No metallicity tracking
- No stellar feedback
- No recycling
- Only processes central galaxies
- No threshold or suppression effects

---

## Module Parameters

All parameters are configured in the YAML file under `modules.parameters.SimpleSFR`.

| Parameter | Type | Default | Range | Description |
|-----------|------|---------|-------|-------------|
| `SimpleSFR_Efficiency` | double | 0.02 | [0.0, 1.0] | Star formation efficiency (ε_SF) |

---

## Example Configuration

### Basic Usage

```
% Enable simple_sfr module (requires simple_cooling)
EnabledModules          simple_cooling,simple_sfr

% Use default efficiency
SimpleSFR_Efficiency    0.02
```

### Higher Efficiency

```
% More aggressive star formation
EnabledModules          simple_cooling,simple_sfr
SimpleSFR_Efficiency    0.05
```

---

## Dependencies

### Required Properties
- `ColdGas` - Cold gas mass from cooling module
- `Vvir` - Virial velocity from halo tracking
- `Rvir` - Virial radius from halo tracking
- `Type` - Halo type (0=central, 1=satellite)
- `dT` - Timestep calculated by core

### Provides Properties
- `StellarMass` - Stellar mass in galaxy

### Module Execution Order
The `simple_sfr` module **must** run after a cooling module that provides `ColdGas`:

```
EnabledModules  simple_cooling,simple_sfr
```

---

## Known Limitations

This is a **Proof-of-Concept module** with intentional simplifications:

1. **No Metallicity**: Does not track metal enrichment from star formation
2. **No Feedback**: Stars form but don't affect the gas reservoir (no supernova feedback)
3. **No Recycling**: No mass return from stellar evolution
4. **Central Only**: Satellites don't form stars (would need more complex physics)
5. **No Thresholds**: Forms stars from any amount of cold gas (no density thresholds)
6. **Simplified Timescale**: Uses simple `Vvir/Rvir` instead of proper cooling/dynamical time comparison

**For realistic star formation**, use the SAGE star formation module (to be implemented in Phase 4.3+).

---

## Testing Status

### Software Quality Testing ✅

**Unit Tests** (`test_unit_simple_sfr.c`):
- ✅ Module registration and initialization
- ✅ Parameter reading and configuration
- ✅ Basic functionality

**Integration Tests** (`test_integration_simple_sfr.py`):
- ✅ Module loads and executes
- ✅ Interacts correctly with cooling module
- ✅ Parameters configurable via YAML files

**Scientific Tests** (`test_scientific_simple_sfr.py`):
- ✅ Mass conservation
- ✅ Non-negative properties
- ✅ Reasonable star formation rates

**Note**: Scientific validation is limited for this PoC module. Full physics validation will be performed on the SAGE star formation module.

---

## Implementation Notes

### Purpose in Mimic Development

This module was created during **Phase 3** (Module System Infrastructure) to validate:
- Runtime module configuration
- Module dependency resolution
- Parameter system functionality
- Multi-module execution pipeline

### Relationship to SAGE

This module is **NOT** based on SAGE physics. SAGE star formation will be implemented as a separate module (`sage_starformation_feedback`) in Phase 4.3+, which will include:
- Proper cooling time vs dynamical time comparison
- Supernova feedback (reheating and ejection)
- Metal enrichment and tracking
- Stellar mass recycling
- Satellite star formation

---

## References

This module implements a simplified Kennicutt-Schmidt-like relation for testing purposes only.

For realistic star formation modeling, see:
- **Kennicutt (1998)** - "The Global Schmidt Law in Star-forming Galaxies"
  - DOI: 10.1086/305588
- **Croton et al. (2016)** - "The SAGE Model of Galaxy Formation"
  - DOI: 10.3847/0067-0049/222/2/22

---

## Contact & Support

For questions about this module:
- See `docs/developer/module-developer-guide.md` for module development patterns
- Check `test_integration_simple_sfr.py` for usage examples
- This module serves as a reference implementation for creating new modules
