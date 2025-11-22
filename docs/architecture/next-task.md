# Next Tasks: Phase 4.4 Scientific Validation (Priority Reordered)

**Current Phase**: Phase 4.4 - SAGE Scientific Validation
**Priority**: Validate working modules FIRST, then fix blocked modules, then reorganize
**Last Updated**: 2025-11-22

---

## Current State

### ✅ Modules Ready for Scientific Validation
- **sage_infall**: Gas accretion, satellite stripping, metallicity (5 unit tests passing)
- **sage_cooling**: Cooling tables, AGN heating, BH growth (9 unit tests passing)
- **sage_starformation_feedback**: Kennicutt-Schmidt SF, SN feedback, metal enrichment (6 unit tests passing)
- **sage_reincorporation**: Gas return from ejected reservoir (6 unit tests passing)

### ⚠️ Modules Blocked (Fix After Initial Validation)
- **sage_mergers**: Complete but untestable (requires merger triggering in core)
- **sage_disk_instability**: Needs shared starburst functions (deferred gas processing)

---

## PRIORITY 1: Scientific Validation of Working Modules

### Step 1: Validate sage_infall (Week 1)

**Goal**: Verify infall physics matches SAGE reference implementation.

**Validation Tests**:
1. **Gas Accretion Rates**
   - Compare infall rates vs halo mass
   - Verify cosmic baryon fraction (Omega_b/Omega_m)
   - Check metallicity tracking in accreted gas

2. **Reionization Suppression**
   - Test suppression at high redshift (z > z_reion)
   - Verify smooth transition through reionization epoch
   - Compare suppression strength vs SAGE

3. **Satellite Stripping**
   - Ram pressure stripping in satellites
   - Verify mass loss rates match SAGE
   - Check metallicity conservation during stripping

4. **Mass Conservation**
   - Total baryon budget (hot + cold + stars + ejected + ICS)
   - Metallicity budget (metals tracked correctly)

**Output**: `tests/scientific/test_scientific_sage_infall.py` with full validation suite

**Timeline**: 3-5 days

---

### Step 2: Validate sage_cooling (Week 1-2)

**Goal**: Verify cooling physics and AGN feedback match SAGE.

**Validation Tests**:
1. **Cooling Function Tables**
   - Sutherland & Dopita (1993) interpolation accuracy
   - Temperature and metallicity dependence
   - Primordial vs metal-enriched gas cooling rates

2. **Cooling Rates**
   - Compare cooling rates vs halo mass and redshift
   - Verify hot → cold gas conversion
   - Check cooling time vs dynamical time

3. **AGN Heating (Three Modes)**
   - Mode 0: No AGN (disabled)
   - Mode 1: Empirical (radio mode efficiency)
   - Mode 2: Bondi-Hoyle accretion
   - Mode 3: Cold cloud accretion
   - Compare BH growth rates vs SAGE for each mode

4. **Black Hole Growth**
   - BH mass vs halo mass relation
   - Accretion rates vs cooling rates
   - Radio-mode heating efficiency

5. **Mass & Energy Conservation**
   - Hot gas depletion = cold gas increase + heating
   - Metal conservation in cooling

**Output**: `tests/scientific/test_scientific_sage_cooling.py` with full validation suite

**Timeline**: 4-6 days

---

### Step 3: Validate sage_starformation_feedback (Week 2-3)

**Goal**: Verify star formation and feedback physics match SAGE.

**Validation Tests**:
1. **Star Formation**
   - Kennicutt-Schmidt law implementation
   - SFR vs cold gas mass
   - Efficiency (SFReff parameter)
   - Threshold density

2. **Supernova Feedback**
   - Reheating: cold gas → hot gas
   - Ejection: hot gas → ejected reservoir
   - Feedback efficiency (epsilon parameter)
   - Energy budget (EnergySNcode, EtaSNcode)

3. **Metal Enrichment**
   - Stellar yields (Yield parameter)
   - Metal production from SF
   - FracZleaveDisk (metals to hot gas)
   - Metallicity evolution

4. **Recycling**
   - RecycleFraction implementation
   - Mass return from dying stars
   - Metal return

5. **Mass Conservation**
   - Cold gas → stars → recycling → cold/hot gas
   - Metal budgets (production + recycling)
   - Outflow rates vs SFR

6. **Disk Scale Radius**
   - Mo, Mao & White (1998) implementation
   - Radius vs spin parameter and halo properties

**Output**: `tests/scientific/test_scientific_sage_starformation_feedback.py`

**Timeline**: 5-7 days

---

### Step 4: Validate sage_reincorporation (Week 3)

**Goal**: Verify gas reincorporation physics match SAGE.

**Validation Tests**:
1. **Reincorporation Timescales**
   - Velocity-dependent reincorporation rate
   - Compare timescales vs halo properties vs SAGE

2. **Gas Return**
   - Ejected reservoir → hot gas transfer
   - Mass return rates
   - Metal conservation during reincorporation

3. **Integration Over Timestep**
   - Verify correct integration of reincorporation rate
   - Check dt handling

4. **Mass Conservation**
   - Ejected mass depletion = hot gas increase
   - Metals tracked correctly

**Output**: `tests/scientific/test_scientific_sage_reincorporation.py`

**Timeline**: 2-3 days

---

### Step 5: Integrated Pipeline Validation (Week 4)

**Goal**: Verify all 4 modules work correctly together.

**Tests**:
1. **Property Flow Validation**
   - sage_infall provides → sage_cooling requires
   - sage_cooling provides → sage_starformation_feedback requires
   - sage_starformation_feedback modifies → sage_reincorporation uses
   - sage_reincorporation modifies → sage_cooling uses (next timestep)

2. **Full Mass Budget**
   - Track all baryons: hot + cold + stars + ejected + ICS
   - Verify conservation across all 4 modules
   - Check at each timestep

3. **Full Metal Budget**
   - Track all metals across all components
   - Production (SF) + destruction (feedback) + recycling
   - Verify conservation across pipeline

4. **Statistical Validation**
   - Compare vs SAGE reference outputs (4-module pipeline)
   - Galaxy stellar mass function
   - Gas mass distributions
   - Metallicity distributions
   - Star formation rate distributions

5. **Performance**
   - Profile on full Millennium data
   - Memory usage check
   - Execution time

**Output**: `tests/integration/test_infall_cooling_sf_reinc_pipeline.py`

**Timeline**: 3-4 days

---

## PRIORITY 2: Fix Blocked Modules

### Step 6: Implement Merger Triggering (Week 5-6)

**Goal**: Implement core infrastructure for merger detection and triggering.

**Decision Needed**: Architecture choice (make this first)

**Option 1: Core Merger Detection Module** (RECOMMENDED)
- Create `src/core/merger_detection.c`
- Implements dynamical friction timescale calculation
- Marks satellites for merger when t_merge < t_current
- Maintains physics-agnostic core (calculation is semi-analytic, not physics module)

**Option 2: Integrated into build_model.c**
- Add merger detection during halo processing
- More direct but adds physics knowledge to core

**Option 3: Two-pass module approach**
- sage_mergers runs twice: once to mark, once to execute
- Keeps physics in modules but complex execution model

**Implementation Tasks**:
1. Design data structures (merger queue, satellite flags)
2. Implement dynamical friction timescale (Jiang et al. 2008 or similar)
3. Add merger marking during tree processing
4. Expose merger status to modules
5. Write unit tests for merger detection
6. Document architecture

**Timeline**: 1-2 weeks

---

### Step 7: Resolve Shared Function Architecture (Week 6)

**Goal**: Enable sage_disk_instability to use starburst functions.

**Decision**: Extract shared physics to `src/modules/shared/`

**Functions to Extract**:
1. `collisional_starburst_recipe()` - from sage_mergers
2. `grow_black_hole()` - from sage_mergers

**Implementation**:
1. Create `src/modules/shared/starburst.h` (header-only)
2. Move functions with `static inline` or proper headers
3. Update sage_mergers to include shared header
4. Update sage_disk_instability to include shared header
5. Write unit tests for shared functions
6. Document shared utilities

**Timeline**: 1-2 days

---

## PRIORITY 3: Validate Blocked Modules

### Step 8: Validate sage_mergers (Week 7)

**Prerequisites**: Merger triggering implemented (Step 6), shared functions extracted (Step 7)

**Validation Tests**:
1. **Merger Detection**
   - Dynamical friction timescales
   - Merger triggering rates vs SAGE
   - Major vs minor merger classification

2. **Merger Physics**
   - Mass transfer (satellite → central)
   - Gas merger handling
   - Metal transfer

3. **Collisional Starbursts**
   - Trigger conditions (major mergers)
   - Burst strength vs mass ratio
   - Gas consumption rates

4. **Black Hole Growth**
   - BH mass transfer during mergers
   - Quasar-mode accretion
   - BH-BH mergers

5. **Quasar Feedback**
   - QuasarModeEfficiency implementation
   - Gas ejection from quasar winds
   - Energy budget

6. **Morphological Transformations**
   - Disk → bulge mass transfer (major mergers)
   - Bulge mass tracking
   - Merger time tracking

7. **Mass Conservation**
   - All components conserved during merger
   - Metals tracked correctly

**Output**: `tests/scientific/test_scientific_sage_mergers.py`

**Timeline**: 5-7 days

---

### Step 9: Validate sage_disk_instability (Week 7-8)

**Prerequisites**: Shared functions available (Step 7)

**Validation Tests**:
1. **Stability Criterion**
   - Mo, Mao & White (1998) implementation
   - Stability parameter vs observations
   - Disk mass vs bulge mass thresholds

2. **Disk → Bulge Transfer**
   - Mass transfer rates when unstable
   - Stellar mass conservation
   - Metal conservation

3. **Gas Processing** (if enabled)
   - Gas consumption in instabilities
   - Starburst triggering (uses shared function)
   - BH growth (uses shared function)

4. **Frequency**
   - Instability occurrence rates vs SAGE
   - Redshift dependence

**Output**: `tests/scientific/test_scientific_sage_disk_instability.py`

**Timeline**: 3-4 days

---

### Step 10: Full 6-Module Pipeline Validation (Week 8)

**Goal**: Verify complete SAGE physics implementation.

**Tests**:
1. **All Modules Integrated**
   - Enable all 6 SAGE modules
   - Verify execution order
   - Check property flow across entire pipeline

2. **Complete Mass Budget**
   - Track baryons across all 6 modules
   - All components: hot, cold, stars, bulge, ejected, ICS, BH
   - Verify conservation

3. **Complete Metal Budget**
   - Track metals across all components
   - Production, destruction, recycling, transfer
   - Verify conservation

4. **SAGE Comparison**
   - Run on Millennium data
   - Compare to SAGE reference outputs
   - Statistical tests:
     - Stellar mass function
     - Cold gas mass function
     - Star formation rate distribution
     - Metallicity distributions
     - Bulge-to-total mass ratios
     - Black hole mass function
   - Quantify differences (mean, scatter, outliers)

5. **Performance**
   - Full Millennium run time
   - Memory usage
   - Scalability test

**Output**: Comprehensive validation report comparing Mimic vs SAGE

**Timeline**: 4-5 days

---

## PRIORITY 4: Module Reorganization (If Needed)

### Step 11: Evaluate Module Splitting (Week 9)

**Goal**: Based on validation experience, decide if modules should be split for better configurability and validation.

**Potential Splits to Consider**:

#### Option A: Split sage_infall
- **sage_infall** → `sage_infall` + `sage_reionization`
- **Rationale**: Reionization is separate physical process from infall
- **Benefits**:
  - Can disable reionization independently
  - Easier to test reionization suppression in isolation
  - Clearer separation of physics
- **Effort**: 2-3 days

#### Option B: Split sage_starformation_feedback
- **sage_starformation_feedback** → `sage_starformation` + `sage_feedback`
- **Rationale**: Star formation and feedback are distinct processes
- **Benefits**:
  - Can disable feedback while keeping SF (useful for tests)
  - Easier to validate each independently
  - Clearer module responsibilities
- **Effort**: 3-4 days (more complex, shared parameters)

#### Option C: Split sage_cooling
- **sage_cooling** → `sage_cooling` + `sage_agn`
- **Rationale**: Gas cooling and AGN feedback are separate physics
- **Benefits**:
  - Can disable AGN independently
  - Easier to test cooling in isolation
  - AGN modes become separate module configurations
- **Effort**: 3-4 days

#### Option D: Extract starbursts from sage_mergers
- **sage_mergers** → `sage_mergers` + `sage_starbursts`
- **Rationale**: Starbursts can occur outside mergers (disk instability)
- **Benefits**:
  - Shared starburst physics between mergers and disk instability
  - Clearer physics separation
  - Better reusability
- **Effort**: 2-3 days (already in shared utilities, just formalize)

#### Option E: Consider sage_disk_instability
- **sage_disk_instability** → Keep as-is or split further?
- **Evaluation**: Does it need splitting? Probably not - focused module
- **Effort**: N/A unless validation reveals issues

#### Option F: Consider sage_reincorporation
- **sage_reincorporation** → Keep as-is (already minimal)
- **Evaluation**: Simple, focused module - splitting would be over-engineering
- **Effort**: N/A

**Decision Process**:
1. Review validation experience (what was hard to test?)
2. Consider user configurability needs (what do users want to toggle?)
3. Evaluate maintenance burden (is splitting worth the complexity?)
4. Prioritize splits with clear benefits
5. Document decision rationale

**Timeline**: 1 day discussion + implementation time (2-4 days per split)

---

### Step 12: Implement Chosen Module Splits (Week 9-10)

**For Each Split Chosen**:

1. **Create new module directory** (`src/modules/new_module/`)
2. **Extract code** from original module
3. **Update module metadata** (`module_info.yaml`)
   - Adjust `provides` and `requires`
   - Define new parameters (if needed)
4. **Update tests**
   - Split tests between original and new module
   - Create new test files for new module
5. **Update property metadata** (if needed)
6. **Generate and rebuild** (`make generate && make`)
7. **Run validation suite** (verify still matches SAGE)
8. **Update documentation**
   - Module README files
   - User configuration guide
   - Architecture documentation

**Success Criteria Per Split**:
- New modules compile and register correctly
- All tests pass (unit, integration, scientific)
- Physics results identical to pre-split (bit-identical if possible)
- Documentation updated
- No performance regression

**Timeline**: 2-4 days per split

---

## Success Criteria

### Phase 4.4 Complete When:

- [ ] Scientific validation tests written for all 6 modules
- [ ] All modules validated against SAGE reference
- [ ] Merger triggering implemented and tested
- [ ] Shared function architecture resolved
- [ ] Full 6-module pipeline validated
- [ ] SAGE comparison complete (statistical tests pass)
- [ ] Module reorganization decisions made and documented
- [ ] Any needed module splits implemented and validated
- [ ] Performance acceptable on Millennium data
- [ ] Comprehensive validation report written

**Then**: Move to Phase 5 (build-time module selection) - production ready!

---

## Timeline Summary

| Week | Tasks | Priority |
|------|-------|----------|
| 1 | Validate sage_infall + sage_cooling | P1 |
| 2 | Validate sage_starformation_feedback | P1 |
| 3 | Validate sage_reincorporation + 4-module pipeline | P1 |
| 4 | Integrated 4-module validation | P1 |
| 5-6 | Implement merger triggering + shared functions | P2 |
| 7 | Validate sage_mergers | P3 |
| 7-8 | Validate sage_disk_instability | P3 |
| 8 | Full 6-module pipeline validation | P3 |
| 9 | Evaluate module splits + implement | P4 |
| 10 | Complete any remaining splits + final validation | P4 |

**Total**: 8-10 weeks to complete Phase 4.4

---

## Notes

- **Validate early, validate often** - Don't wait until the end
- **Document issues as you find them** - Physics bugs, parameter problems, edge cases
- **Compare to SAGE frequently** - Catch divergence early
- **Module splits are optional** - Only if validation reveals clear benefits
- **Performance matters** - Track execution time throughout validation

**Philosophy**: Scientific accuracy first. Get the physics right, then optimize structure.
