# SAGE Reincorporation Module

**Version**: 1.0.0
**Author**: Mimic Team (ported from SAGE)
**Category**: Gas Physics
**Phase**: 4.2 (SAGE Physics Module Implementation)
**Status**: ✅ COMPLETE (Software Quality) | ⏸️ DEFERRED (Physics Validation)

---

## Overview

The SAGE reincorporation module implements the return of gas from the ejected reservoir back to the hot halo gas. Gas that was previously ejected by supernova feedback can be recaptured by halos with sufficiently high virial velocities. This process is crucial for the gas cycling through galaxies: hot gas → cold gas → stars → ejected gas → hot gas.

**Physics Summary:**
- Ejected gas can be recaptured by massive halos (Vvir > Vcrit ≈ 445 km/s)
- Reincorporation rate depends on halo mass and dynamical time
- More massive halos reincorporate gas more efficiently
- Metallicity is preserved during gas transfer

**Mass Flow:**
```
EjectedMass      → HotGas
MetalsEjectedMass → MetalsHotGas
```

---

## Physics Description

### Physical Motivation

Supernova-driven winds can eject gas from galaxies with characteristic velocities of ~630 km/s. This gas escapes the galaxy and enters the ejected reservoir. However, halos with sufficiently deep gravitational potentials can recapture this gas over time.

The reincorporation rate depends on the comparison between:
- **Halo escape velocity**: V_esc = √2 × V_vir
- **Supernova wind velocity**: V_SN ≈ 630 km/s

When the escape velocity exceeds the wind velocity, gas can be gravitationally recaptured.

### Critical Velocity Threshold

The critical virial velocity for reincorporation is:

```
Vcrit = V_SN / √2 = 630 km/s / 1.414 ≈ 445.48 km/s
```

This can be modified by a tunable parameter:

```
Vcrit = 445.48 km/s × ReIncorporationFactor
```

**Only halos with Vvir > Vcrit can reincorporate gas.**

### Reincorporation Rate

The reincorporation rate is:

```
dM_reinc/dt = (Vvir/Vcrit - 1) × M_ejected × (Vvir/Rvir) × dt
```

Where:
- **Vvir**: Halo virial velocity (km/s)
- **Vcrit**: Critical velocity threshold (km/s)
- **M_ejected**: Mass in ejected reservoir (10^10 Msun/h)
- **Rvir**: Virial radius (Mpc/h)
- **dt**: Timestep (Gyr)

The rate is proportional to:
1. **Velocity factor**: (Vvir/Vcrit - 1) - more massive halos reincorporate faster
2. **Ejected mass**: M_ejected - more ejected gas → more reincorporation
3. **Dynamical rate**: Vvir/Rvir = 1/t_dyn - shorter dynamical times → faster reincorporation

### Metallicity Preservation

The metallicity of the ejected gas is preserved during reincorporation:

```
Z_ejected = MetalsEjectedMass / EjectedMass
dM_metals/dt = Z_ejected × dM_reinc/dt
```

This ensures that enriched gas from stellar feedback maintains its metal content when returning to the hot halo.

---

## Implementation Details

### Module Dependencies

**Requires (Input Properties):**
- `EjectedMass` (from sage_infall or sage_starformation_feedback)
- `MetalsEjectedMass` (from sage_infall or sage_starformation_feedback)
- `HotGas` (from sage_infall)
- `MetalsHotGas` (from sage_infall)
- Core halo properties: `Vvir`, `Rvir`, `dT`

**Provides (Output Properties):**
- None (modifies existing properties only)

**Upstream Modules:**
- sage_infall ✅ (provides hot gas and ejected reservoirs)
- sage_starformation_feedback ⏳ (populates ejected reservoir with feedback)

### Algorithm

For each central galaxy (Type == 0):

1. **Check critical velocity**:
   ```c
   if (Vvir <= Vcrit) continue;  // Skip if below threshold
   ```

2. **Calculate reincorporation rate**:
   ```c
   velocity_factor = (Vvir / Vcrit) - 1.0;
   dynamical_rate = Vvir / Rvir;
   reincorporated = velocity_factor × EjectedMass × dynamical_rate × dt;
   ```

3. **Limit to available mass**:
   ```c
   if (reincorporated > EjectedMass)
       reincorporated = EjectedMass;
   ```

4. **Calculate metallicity**:
   ```c
   Z = MetalsEjectedMass / EjectedMass;  // Using mimic_get_metallicity()
   reincorporated_metals = Z × reincorporated;
   ```

5. **Update reservoirs**:
   ```c
   EjectedMass -= reincorporated;
   MetalsEjectedMass -= reincorporated_metals;
   HotGas += reincorporated;
   MetalsHotGas += reincorporated_metals;
   ```

### Key Implementation Notes

**Central galaxies only:**
- Reincorporation only occurs for Type == 0 (central) galaxies
- Satellites and orphans do not reincorporate (they don't have access to the halo-scale ejected reservoir)

**Safe division:**
- Uses `EPSILON_SMALL` to prevent division by zero
- Validates timestep (dt > 0) and virial radius (Rvir > 0)

**Metallicity utility:**
- Uses shared utility `mimic_get_metallicity()` from `src/modules/shared/metallicity.h`
- Handles edge cases (zero gas mass) automatically

**Bounded transfers:**
- Reincorporation cannot exceed available ejected mass
- Prevents negative masses or unphysical transfers

---

## Configuration

### Parameters

**SageReincorporation_ReIncorporationFactor**
- **Type**: double
- **Default**: 1.0
- **Range**: [0.0, 10.0]
- **Units**: dimensionless
- **Description**: Tunable parameter multiplying critical virial velocity for reincorporation
- **Physics**:
  - Default (1.0): Vcrit = 445.48 km/s
  - Lower values (e.g., 0.5): More reincorporation in lower-mass halos (Vcrit = 222.74 km/s)
  - Higher values (e.g., 2.0): Less reincorporation, only very massive halos (Vcrit = 890.96 km/s)

### Example Configuration

**Typical SAGE configuration:**
```
EnabledModules  sage_infall,sage_cooling,sage_starformation_feedback,sage_reincorporation

# Reincorporation parameters
SageReincorporation_ReIncorporationFactor  1.0
```

**Low-mass halo reincorporation (more efficient):**
```
SageReincorporation_ReIncorporationFactor  0.5
```

**High-mass halo reincorporation only:**
```
SageReincorporation_ReIncorporationFactor  2.0
```

---

## Testing Status

### Software Quality Testing ✅

**Unit Tests**: ✅ 6 tests passing
- Module registration and initialization
- Parameter reading and validation
- Invalid parameter rejection
- Memory safety (no leaks)
- Property access patterns

**Integration Tests**: ✅ 7 tests passing
- Module loads correctly
- Parameters configurable
- Memory safety in full pipeline
- Execution completes successfully
- Multi-module pipeline integration (with sage_infall)
- Property modification (EjectedMass → HotGas)
- Critical velocity threshold behavior

### Physics Validation ⏸️

**Status**: Deferred

**Rationale**: Physics validation requires complete gas cycling pipeline. Reincorporation physics can only be validated when:
1. sage_infall ✅ provides hot gas
2. sage_cooling ✅ cools hot → cold gas
3. sage_starformation_feedback ⏳ forms stars and ejects gas (populates ejected reservoir)
4. sage_reincorporation (THIS MODULE) returns ejected → hot gas

Without star formation and feedback, EjectedMass remains at zero, so reincorporation cannot be tested.

**Plan**: After sage_starformation_feedback is implemented:
- Compare Mimic vs SAGE outputs on identical trees
- Validate mass conservation through full gas cycling pipeline
- Check reincorporation rate dependence on Vvir
- Verify critical velocity threshold behavior
- Validate metallicity preservation
- Check dynamical timescale dependence
- Statistical validation: gas fraction distributions, cycling timescales

---

## Scientific Context

### Key Physics Papers

**Reincorporation and Gas Recycling:**
- Croton et al. (2006) - Original SAGE AGN feedback implementation
- Croton et al. (2016) - SAGE model description with reincorporation details
- Guo et al. (2011) - Gas reincorporation timescales in Millennium simulation
- Henriques et al. (2013) - Munich model reincorporation implementation
- Somerville & Davé (2015) - Review of gas cycling in SAMs

**Supernova-Driven Winds:**
- Dekel & Silk (1986) - Supernova feedback in dwarf galaxies
- Mac Low & Ferrara (1999) - Starburst-driven mass loss
- Oppenheimer & Davé (2006) - Enriched outflows and reincorporation

### Physical Scales

**Critical halo mass** (where Vvir ≈ 445 km/s):
- At z=0: M_vir ≈ 10^12 Msun/h
- At z=2: M_vir ≈ 10^11.5 Msun/h

**Reincorporation timescales:**
- Low-mass halos (Vvir ~ 500 km/s): t_reinc ~ 2-4 Gyr
- Massive halos (Vvir ~ 1000 km/s): t_reinc ~ 0.5-1 Gyr

**Physical interpretation:**
- Milky Way-mass halos (Vvir ~ 150 km/s): **no reincorporation** (winds escape permanently)
- Group-scale halos (Vvir ~ 500 km/s): **slow reincorporation** (multi-Gyr timescales)
- Cluster-scale halos (Vvir ~ 1500 km/s): **rapid reincorporation** (sub-Gyr timescales)

---

## Known Limitations

### Current Implementation

1. **Simple reincorporation model**: Rate depends only on Vvir and t_dyn. More sophisticated models could include:
   - Redshift dependence (IGM pressure)
   - Environmental effects (stripping)
   - Spatial structure of ejected reservoir

2. **No spatial information**: Ejected gas is treated as a single reservoir, not spatially distributed

3. **Instant metallicity mixing**: Reincorporated metals immediately mix with hot gas (no delay)

4. **No delay time**: Gas reincorporates continuously, no time delay for gas to return

### SAGE Model Consistency

This implementation is **identical** to SAGE's reincorporation model:
- Same critical velocity calculation
- Same rate formula
- Same metallicity treatment
- Same Type==0 (central) restriction

This ensures bit-identical results with SAGE (when validated after sage_starformation_feedback is complete).

---

## Future Work

### After sage_starformation_feedback Implementation

**Physics Validation** (Priority 1):
- Full pipeline comparison: Mimic vs SAGE
- Mass conservation validation
- Reincorporation rate testing
- Metallicity preservation verification

**Scientific Validation** (Priority 2):
- Reproduce published results (Croton et al. 2016)
- Statistical tests on galaxy populations
- Gas fraction distributions
- Stellar mass functions

### Potential Enhancements (Phase 5+)

**Alternative reincorporation models**:
- Redshift-dependent reincorporation (changing IGM conditions)
- Environmental modulation (stripping in groups/clusters)
- Delayed reincorporation (time lag for gas return)
- Spatial tracking of ejected reservoir

**Parameter optimization**:
- Constrain ReIncorporationFactor using observations
- Calibrate against galaxy gas fractions
- Tune to match CGM observations

---

## References

### SAGE Model

- **Croton et al. (2016)**: *Seminalytic model of galaxy formation (SAGE)*, ApJS, 222, 22
- **SAGE source code**: `sage-code/model_reincorporation.c`

### Reincorporation Physics

- **Guo et al. (2011)**: *From dwarf spheroidals to cD galaxies: simulating the galaxy population in a ΛCDM cosmology*, MNRAS, 413, 101
- **Henriques et al. (2013)**: *Galaxy formation in the Planck cosmology - I. Matching the observed evolution of star formation rates, colours and stellar masses*, MNRAS, 431, 3373
- **Oppenheimer & Davé (2006)**: *Cosmological simulations of intergalactic medium enrichment from galactic outflows*, MNRAS, 373, 1265

### Supernova Feedback

- **Dekel & Silk (1986)**: *The origin of dwarf galaxies, cold dark matter, and biased galaxy formation*, ApJ, 303, 39
- **Mac Low & Ferrara (1999)**: *Starburst-driven mass loss from dwarf galaxies: efficiency and metal ejection*, ApJ, 513, 142

### Reviews

- **Somerville & Davé (2015)**: *Physical Models of Galaxy Formation in a Cosmological Framework*, ARA&A, 53, 51

---

## Development Notes

**Implementation Date**: 2025-11-17
**SAGE Source**: `sage-code/model_reincorporation.c`
**Lines of Code**: ~250 (module) + ~350 (tests)
**Development Time**: 1 day (estimated 1-2 weeks from roadmap)

**Key Lessons Learned**:
1. Simple physics module - no new properties needed (only modifies existing)
2. Shared utilities (`mimic_get_metallicity()`) reduce code duplication
3. Deferring physics validation is acceptable when dependencies incomplete
4. Clear documentation of deferral rationale prevents confusion

**Architecture Validation**:
- ✅ Physics-Agnostic Core: Module interacts only through defined interfaces
- ✅ Runtime Modularity: Fully configurable via parameter file
- ✅ Metadata-Driven: Automatic registration via module_info.yaml
- ✅ Single Source of Truth: Updates GalaxyData properties only

---

## Contact

For questions or issues with this module:
- Review this README for physics and implementation details
- Check `docs/developer/module-developer-guide.md` for module patterns
- Examine SAGE source code: `sage-code/model_reincorporation.c`
- See `docs/architecture/module-implementation-log.md` for lessons learned
