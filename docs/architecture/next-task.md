# Phase 4.2 Remaining Tasks

**Current Status**: sage_infall module COMPLETE with test architecture refactor (10/26 tasks done)

**Date**: 2025-11-13

---

## ✅ Infrastructure Fix: Test Fixture Architecture (2025-11-13)

**Problem Discovered**: Infrastructure tests (unit and integration) violated Vision Principle #1 by hardcoding production module names (`simple_cooling`, `simple_sfr`). This meant archiving modules required updating core test code.

**Solution Implemented**: Created `src/modules/test_fixture/` - a minimal, stable test module for infrastructure testing only. Updated all infrastructure tests to use `test_fixture` instead of production modules.

**Impact**: Production modules can now be archived with ZERO infrastructure test changes. Validates physics-agnostic core principle.

**Next**: Ready to archive `simple_cooling` (being replaced by `sage_cooling`) to `src/modules/_archive/` with zero test breakage.

---

## ⚠️ Important: Automatic Module Discovery Now Available

**NEW (Phase 4.2.5 - Completed 2025-11-12)**: Module registration is now **fully automatic** via metadata!

**Old Workflow** (manual - DON'T DO THIS):
- ❌ Manually edit `src/modules/module_init.c` to add includes and registration
- ❌ Manually edit `tests/unit/run_tests.sh` to add module sources
- ❌ Manually update documentation

**New Workflow** (automatic - DO THIS):
1. ✅ Create module implementation (`module_name.c`, `module_name.h`)
2. ✅ Create `module_info.yaml` from template
3. ✅ Run `make generate-modules` - everything auto-generated!

See `docs/developer/module-metadata-schema.md` for complete metadata documentation.

---

## Completed Tasks

### Module 1: sage_infall - Testing & Documentation (Tasks 7-10) ✅

**Status**: COMPLETE - All tests passing, documentation written

**Completed**:
- ✅ Task 7: Unit tests (5 tests, software quality focus)
- ✅ Task 8: Integration tests (7 tests, software quality focus)
- ✅ Task 9: Scientific validation tests (deferred to Phase 4.3+, placeholder created)
- ✅ Task 10: Documentation (physics docs, user guide, implementation log)

**Bonus Infrastructure Work**:
- ✅ Implemented test registry system (metadata-driven test discovery)
- ✅ Co-located tests with modules (physics-agnostic core maintained)
- ✅ Updated test runners for auto-discovery
- ✅ Updated testing documentation

**Key Decision**: Physics validation deferred until downstream modules (cooling, star formation) are implemented. Current tests focus on software quality: module lifecycle, memory safety, parameter handling, integration.

---

## Immediate Next Steps

### Module 2: sage_cooling (Tasks 11-22)

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

**Workflow (using automatic module discovery)**:
1. Copy template: `cp -r src/modules/_template src/modules/sage_cooling`
2. Implement `sage_cooling.c` and `sage_cooling.h`
   - `sage_cooling_init()` - Load cooling tables, read parameters
   - `sage_cooling_process()` - Cooling physics (NO AGN)
   - `sage_cooling_cleanup()` - Free cooling tables
3. Create `module_info.yaml` from template
   - Define module metadata (name, description, version)
   - List dependencies: requires HotGas (from sage_infall), provides ColdGas
   - Define parameters: CoolFunctionsDir, etc.
4. Run `make generate-modules` - Automatic registration!
5. Build and test: `make clean && make`

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

## Task 23: Joint Module Pipeline Testing (Cross-Module Validation)

**Goal**: Verify sage_infall → sage_cooling chain works correctly

**Context**: This addresses the "Cross-Module Physics Validation" recommendation from the comprehensive testing framework code review (2025-11-13). While `test_module_pipeline.py` tests that modules can run together, this validates the **scientific interaction** between modules.

**Test File**: `tests/integration/test_infall_cooling_pipeline.py`

**Validation Points**:
1. **Execution Order**: sage_infall must execute before sage_cooling
2. **Property Flow**: HotGas produced by sage_infall feeds into sage_cooling
3. **Mass Conservation**: Track mass flow: Mvir → HotGas → ColdGas
4. **Metallicity Tracking**: MetalsHotGas → MetalsColdGas flow is correct
5. **Physics Consistency**: Cooling respects virial properties from infall

**Test Structure**:
```python
def test_infall_cooling_execution_order():
    """Test that sage_infall runs before sage_cooling."""
    # Verify order in stdout logs
    # Check that HotGas exists before cooling starts

def test_hotgas_property_flow():
    """Test that HotGas from infall feeds cooling."""
    # Run with both modules
    # Verify HotGas > 0 after infall
    # Verify ColdGas increases after cooling

def test_mass_conservation_chain():
    """Test mass conservation: Mvir → HotGas → ColdGas."""
    # Sum all gas components
    # Verify total matches baryon fraction * Mvir
    # Allow tolerance for numerical precision

def test_metallicity_tracking():
    """Test metallicity flow through pipeline."""
    # Verify MetalsHotGas/HotGas ratio maintained
    # Check MetalsColdGas/ColdGas consistent after cooling

def test_virial_property_consistency():
    """Test cooling respects virial properties."""
    # Verify cooling uses correct Vvir, Rvir from infall
    # Check cooling radius calculation uses virial values
```

**Parameter File**:
```
EnabledModules  sage_infall,sage_cooling
SageInfall_BaryonFrac  0.17
SageInfall_ReionizationOn  1
SageCooling_CoolFunctionsDir  input/CoolFunctions
```

**Success Criteria**:
- All 5 validation tests pass
- Mass conservation within 1e-6 relative tolerance
- Metallicity ratios consistent across gas phases
- Module execution order verified in logs

**Estimated Time**: 1 day

**Note**: This test validates the **software quality** of module integration. Full physics validation (comparing to SAGE reference) will be added in Phase 4.3+ after more modules are implemented.

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
- `src/modules/sage_cooling/README.md` - Physics documentation in module directory
- `docs/user/module-configuration.md` - Add both modules
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
1. ✅ **Manual module registration** - **RESOLVED (Phase 4.2.5 - 2025-11-12)**
   - Was violating metadata-driven architecture principle
   - Implemented automatic module discovery system
   - Modules now auto-register via module_info.yaml metadata
   - See `docs/developer/module-metadata-schema.md` for details

2. **Unit conversion complexity**
   - Need better documentation of Mimic vs SAGE units
   - Consider adding unit conversion helpers

3. **Constant naming differences**
   - SAGE uses MEGAPARSEC, Mimic uses CM_PER_MPC
   - Document mapping between codebases

**Infrastructure Improvements**:
- ✅ **Automatic module discovery and registration** - COMPLETED (Phase 4.2.5)
  - Full metadata-driven module system operational
  - 75% reduction in error opportunities when adding modules
  - All documentation auto-generated from metadata
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
