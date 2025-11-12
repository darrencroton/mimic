# Phase 4.2 Remaining Tasks

**Current Status**: sage_infall module implementation complete (6/26 tasks done)

**Date**: 2025-11-12

---

## Immediate Next Steps

### Module 1: sage_infall - Testing & Documentation (Tasks 7-10)

#### Task 7: Write Unit Tests for sage_infall
**File**: `tests/unit/test_sage_infall.c`

**Tests Needed**:
1. **test_reionization_suppression**: Test `do_reionization()` function
   - Verify suppression factor calculation for different halo masses
   - Test all three regimes (before UV, partial, full reionization)
   - Edge cases: very low mass, very high mass halos

2. **test_infall_calculation**: Test `infall_recipe()` function
   - Mass conservation: total baryons should match baryon fraction × Mvir
   - Negative infall handling (mass loss)
   - Satellite baryon consolidation

3. **test_satellite_stripping**: Test `strip_from_satellite()` function
   - Verify gradual stripping over STRIPPING_STEPS
   - Metal preservation during stripping
   - Boundary conditions (zero hot gas, etc.)

4. **test_gas_transfer**: Test `add_infall_to_hot()` function
   - Positive infall (simple addition)
   - Negative infall (ejected reservoir first, then hot gas)
   - Non-negative mass enforcement

**Estimated Time**: 1-2 days

---

#### Task 8: Write Integration Tests for sage_infall
**File**: `tests/integration/test_sage_infall.py`

**Tests Needed**:
1. **test_module_loads**: Verify module registers and initializes
2. **test_hot_gas_appears**: Run pipeline, check HotGas in output
3. **test_baryon_fraction**: Verify HotGas ≈ BaryonFrac × Mvir (statistical)
4. **test_parameter_configuration**: Test all module parameters work
5. **test_reionization_toggle**: Test ReionizationOn = 0 vs 1
6. **test_memory_safety**: No leaks with module enabled

**Estimated Time**: 1 day

---

#### Task 9: Write Scientific Validation Tests
**File**: `tests/scientific/test_sage_infall.py`

**Approach**: Compare against SAGE reference outputs

**Tests Needed**:
1. **test_hot_gas_distribution**: Compare HotGas distribution to SAGE
2. **test_reionization_effect**: Verify suppression at low masses matches SAGE
3. **test_satellite_stripping_rate**: Compare stripping efficiency
4. **test_mass_conservation**: Total baryons conserved within tolerance

**Data Needed**:
- May need to generate SAGE reference run on same trees
- Or use existing SAGE outputs if available

**Estimated Time**: 2-3 days

---

#### Task 10: Document sage_infall Module
**Files**:
- `docs/physics/sage-infall.md` - Physics documentation
- `docs/user/module-configuration.md` - Update with sage_infall
- `docs/architecture/module-implementation-log.md` - Add implementation entry

**Content**:

**Physics Documentation**:
```markdown
# SAGE Infall Module

## Overview
Implements cosmological gas infall onto dark matter halos following SAGE model.

## Physics Equations
[Detailed equations from Gnedin (2000), Kravtsov et al. (2004)]

## Parameters
- SageInfall_BaryonFrac (default: 0.17)
- SageInfall_ReionizationOn (default: 1)
- SageInfall_Reionization_z0 (default: 8.0)
- SageInfall_Reionization_zr (default: 7.0)
- SageInfall_StrippingSteps (default: 10)

## Example Configuration
[.par file snippet]

## Known Limitations
[Any simplifications or assumptions]

## References
- Gnedin (2000) - Reionization model
- Kravtsov et al. (2004) - Filtering mass
- Croton et al. (2016) - SAGE model
```

**Implementation Log Entry**:
Document:
- Implementation timeline (actual vs estimated)
- Challenges encountered (unit conversion, constant naming)
- Solutions applied
- What went well (template worked perfectly)
- What could be improved (manual registration is painful - see infrastructure needs!)
- Infrastructure gaps discovered (automatic module discovery needed!)

**Estimated Time**: 1 day

---

## Module 2: sage_cooling (Tasks 11-22)

### Task 11: Copy Cooling Tables
```bash
cp -r sage-code/CoolFunctions input/
```

**Verify**: 8 files present, ASCII format readable

**Estimated Time**: 5 minutes

---

### Task 12: Port Cooling Function Infrastructure

**Approach**: Port SAGE `core_cool_func.c` as module helpers

**Location**: `src/modules/sage_cooling/cooling_tables.c` and `.h`

**Functions to Port**:
1. `read_cooling_functions()` - Load 8 metallicity tables
2. `get_metaldependent_cooling_rate()` - 2D interpolation
3. Helper: `get_rate()` - 1D interpolation within table

**Considerations**:
- Use Mimic memory system: `malloc_tracked(size, MEM_UTILITY)`
- Make cooling table path configurable
- Error handling for missing files
- Validate table format on load

**Estimated Time**: 1-2 days

---

### Task 13: Unit Tests for Cooling Infrastructure

**File**: `tests/unit/test_cooling_tables.c`

**Tests**:
1. **test_table_loading**: Load all 8 tables successfully
2. **test_temperature_interpolation**: Verify 1D interpolation
3. **test_metallicity_interpolation**: Verify 2D interpolation
4. **test_cooling_rate_values**: Compare to known values from tables
5. **test_edge_cases**: Primordial gas, super-solar metallicity, extreme temps

**Estimated Time**: 1 day

---

### Task 14: Add Cooling Properties

**File**: `metadata/properties/galaxy_properties.yaml`

**Properties to Add**:
```yaml
- name: CoolingRate
  type: float
  units: "Msun/yr"
  description: "Current gas cooling rate"
  output: true

- name: Cooling
  type: double  # Accumulates, needs precision
  units: "(km/s)^2 * 1e10 Msun/h"
  description: "Cumulative cooling energy"
  output: true

- name: rcool
  type: float
  units: "Mpc/h"
  description: "Cooling radius"
  output: true
```

Then: `make generate`

**Estimated Time**: 30 minutes

---

### Tasks 15-18: Implement sage_cooling Module

**Similar process to sage_infall**:
1. Copy template
2. Implement init (load tables, read parameters)
3. Implement process (cooling physics, NO AGN)
4. Register module

**Key Physics**:
- Calculate virial temperature: T_vir = 35.9 × V_vir²
- Get metallicity: Z = MetalsHotGas / HotGas
- Interpolate cooling function: λ(T, Z)
- Calculate cooling radius: r_cool
- Determine regime (cold accretion vs hot halo)
- Transfer gas: HotGas → ColdGas

**Estimated Time**: 3-4 days

---

### Tasks 19-22: Testing & Documentation

**Similar to sage_infall**:
- Unit tests (cooling rate calculations, rcool, conservation)
- Integration tests (pipeline with both modules)
- Scientific validation (compare to SAGE)
- Documentation (physics, user guide, implementation log)

**Estimated Time**: 3-4 days

---

## Task 23: Joint Module Pipeline Testing

**Goal**: Verify sage_infall → sage_cooling chain works correctly

**Tests**:
1. Module execution order matters
2. HotGas from infall feeds cooling
3. Mass flows: Mvir → HotGas → ColdGas
4. End-to-end mass conservation
5. Metallicity tracking through pipeline

**Parameter File**:
```
EnabledModules  sage_infall,sage_cooling
SageInfall_BaryonFrac  0.17
SageCooling_CoolFunctionsDir  input/CoolFunctions
```

**Estimated Time**: 1 day

---

## Task 24: Performance Profiling

**Test on Full Millennium Data**:
```bash
./mimic input/millennium.par
```

**Metrics**:
- Total runtime (compare to SAGE)
- Memory usage (`print_allocated_by_category()`)
- Cooling table interpolation cost (profile)
- Memory leaks (valgrind if needed)

**Optimization Targets** (if needed):
- Cooling table lookup caching
- Reduce allocation churn
- Vectorization opportunities

**Estimated Time**: 1 day

---

## Task 25: Complete Documentation

**Files to Update**:
- `docs/physics/sage-cooling.md` - New file
- `docs/user/module-configuration.md` - Add both modules
- `docs/architecture/module-implementation-log.md` - Both module entries
- `README.md` - Update features list

**Include**:
- Side-by-side parameter comparison with SAGE
- Validation plots (if generated)
- Performance benchmarks
- Known limitations vs SAGE

**Estimated Time**: 1 day

---

## Task 26: Update Developer Infrastructure

**Based on Lessons Learned**:
- Update `docs/developer/module-developer-guide.md` with real examples
- Refine `src/modules/_template/` based on actual usage
- Add cooling table utilities to shared infrastructure (if pattern emerges)
- Document common pitfalls (unit conversions, constant naming)

**Estimated Time**: 1 day

---

## Timeline Estimate

**Optimistic**: 2.5 weeks
**Realistic**: 3-4 weeks (as originally estimated)
**Conservative**: 5 weeks (if significant SAGE comparison issues arise)

**Critical Path**:
1. sage_infall testing (Tasks 7-9): 4-6 days
2. Cooling infrastructure (Tasks 11-13): 3-4 days
3. sage_cooling implementation (Tasks 15-18): 3-4 days
4. sage_cooling testing (Tasks 20-22): 3-4 days
5. Integration & documentation (Tasks 23-26): 4 days

**Parallelization Opportunities**:
- Documentation can be done incrementally
- Performance profiling can overlap with validation

---

## Success Criteria

Phase 4.2 is complete when:

- [ ] sage_infall passes all test tiers (unit, integration, scientific)
- [ ] sage_cooling passes all test tiers
- [ ] Joint pipeline validated against SAGE
- [ ] All 6 test suites pass (including new module tests)
- [ ] Memory leak-free execution on full data
- [ ] Performance acceptable (within 2× of SAGE runtime)
- [ ] Complete documentation for both modules
- [ ] Implementation log captures lessons learned
- [ ] Code follows coding standards (`./scripts/beautify.sh`)

---

## Notes for Next Session

**What's Working Well**:
- Module template is excellent - minimal adaptation needed
- Property system works flawlessly
- Test infrastructure is robust
- Development workflow is smooth

**Pain Points Discovered**:
1. **Manual module registration** (module_init.c, test script) - CRITICAL GAP
   - Violates metadata-driven architecture principle
   - Error-prone manual synchronization
   - Should be auto-generated from module presence
   - **Recommendation**: Implement Phase 5 module discovery NOW

2. **Unit conversion complexity**
   - Need better documentation of Mimic vs SAGE units
   - Consider adding unit conversion helpers

3. **Constant naming differences**
   - SAGE uses MEGAPARSEC, Mimic uses CM_PER_MPC
   - Document mapping between codebases

**Infrastructure Improvements Needed**:
- **Automatic module discovery and registration** (see infrastructure report below)
- Shared physics utilities (if patterns emerge across multiple modules)
- Better SAGE-to-Mimic mapping documentation

---

## Future Modules (Phase 4.3+)

After sage_infall and sage_cooling are complete:

**Priority 3**: sage_reincorporation (1-2 weeks)
**Priority 4**: sage_starformation_feedback (3-4 weeks - most complex)
**Priority 5**: sage_mergers (2-3 weeks)
**Priority 6**: sage_disk_instability (2-3 weeks)

**Total Phase 4 Estimate**: 6-12 months for all 6 SAGE modules
