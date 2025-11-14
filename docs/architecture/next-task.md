# Phase 4.2 Next Tasks

**Current Status**: 2 SAGE modules COMPLETE (sage_infall + sage_cooling)

**Date**: 2025-11-14

**Next Module**: sage_starformation_feedback (Priority 3, most complex)

---

## ✅ Completed Modules

### Module 1: sage_infall ✅
- **Status**: COMPLETE (November 2025)
- **Tests**: 5 unit + 7 integration, all passing
- **Location**: `src/modules/sage_infall/`
- **Physics**: Gas infall, reionization suppression, satellite stripping
- **Properties**: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS

### Module 2: sage_cooling ✅
- **Status**: COMPLETE (November 2025)
- **Tests**: 9 unit + 7 integration, all passing
- **Location**: `src/modules/sage_cooling/`
- **Physics**: Gas cooling, AGN feedback (3 modes), black hole growth
- **Properties**: ColdGas, MetalsColdGas, BlackHoleMass, Cooling, Heating, r_heat
- **Data**: 16 cooling tables co-located in `src/modules/sage_cooling/CoolFunctions/`

---

## 📋 Immediate Next Steps

### Module 3: sage_starformation_feedback

**Source**: `sage-code/model_starformation_and_feedback.c` (~1000 lines)

**Complexity**: HIGH - Most complex SAGE module (star formation + SN + winds + metal enrichment)

**Estimated Time**: 3-4 weeks

---

### Step 1: Physics Analysis (2-3 days)

**Goal**: Understand SAGE SF&F implementation thoroughly

**Tasks**:
- Read and annotate `sage-code/model_starformation_and_feedback.c`
- Document SF physics (Kennicutt-Schmidt law, timescale approach)
- Document SN feedback (energy injection, mass ejection, metal enrichment)
- Document wind physics (gas ejection from disk)
- Map metal yield tables and recycling fractions
- Identify all parameters and defaults
- List dependencies on other modules

**Key Physics Components**:
1. **Star Formation**: Cold gas → stars (Kennicutt-Schmidt)
2. **SN Feedback**: Energy/mass return from stellar evolution
3. **Metal Enrichment**: Track metal production from SNII, SNIa, AGB
4. **Gas Ejection**: Winds remove gas from disk to hot halo
5. **Recycling**: Instantaneous vs delayed recycling fractions

**Deliverables**:
- Physics specification document (can be in implementation log)
- List of new properties needed
- List of parameters (expect 10-15)
- Dependency map

---

### Step 2: Property & Parameter Design (1 day)

**Properties to Add** (`metadata/galaxy_properties.yaml`):
```yaml
- name: StellarMass
  type: float
  units: "1e10 Msun/h"
  description: "Total stellar mass in galaxy"

- name: MetalsStellarMass
  type: float
  units: "1e10 Msun/h"
  description: "Metal mass in stars"

- name: SfrDisk
  type: float
  units: "Msun/yr"
  description: "Star formation rate in disk"

- name: StarBurstFlag
  type: int
  units: "dimensionless"
  description: "Starburst activity flag"
```

**Parameters** (expect ~12-15):
- SF efficiency, timescale
- Feedback energy fractions
- Wind ejection parameters
- Metal yield parameters
- Recycling fractions

**Tasks**:
- Add properties to YAML
- Run `make generate`
- Define all module parameters

---

### Step 3: Implementation (5-7 days)

**Location**: `src/modules/sage_starformation_feedback/`

**Approach**: Port from SAGE systematically

**Sub-components** (implement in order):
1. **Star Formation** (2 days)
   - Cold gas consumption
   - Stellar mass growth
   - SFR calculation

2. **SN Feedback** (2 days)
   - Energy injection
   - Mass return (recycled gas)
   - Metal enrichment (SNII, SNIa, AGB)

3. **Gas Ejection** (1-2 days)
   - Wind mass ejection
   - Hot gas heating
   - Metallicity tracking

4. **Integration** (1 day)
   - Combine all components
   - Conservation checks
   - Debug logging

**Key Challenges**:
- Metal yield tables (may need data files)
- Multiple feedback channels (SN, winds)
- Conservation: mass + metals + energy
- Timestep handling (instantaneous vs continuous)

---

### Step 4: Visual Validation & Plotting (1-2 days)

**Goal**: Create diagnostic figures for visual validation and scientific analysis

**Location**: `output/mimic-plot/`

**Potential Figures** (discuss with developer):
1. **Star Formation Main Sequence**: SFR vs StellarMass (compare to observations)
2. **Stellar Mass Function**: Distribution of stellar masses at different redshifts
3. **Gas Depletion Time**: ColdGas/SFR timescale vs stellar mass
4. **Metal Enrichment**: Metallicity vs stellar mass (mass-metallicity relation)
5. **Baryon Census**: Pie chart of baryonic components (stars, cold gas, hot gas, ejected)
6. **SFR History**: SFR vs redshift for sample galaxies

**Implementation**:
- Review `output/mimic-plot/README.md` for how to add figures
- Add figure functions to `output/mimic-plot/mimic-plot.py`
- Figures auto-skip if required properties missing
- Test with full pipeline output

**Benefits**:
- Visual validation of physics (spot anomalies)
- Scientific diagnostics for comparison to observations
- Presentation-ready figures for papers

**Note**: mimic-plot handles both core and physics figures. Physics-specific figures live alongside core figures - system skips them when properties unavailable.

---

### Step 6: Testing (3-4 days)

**Unit Tests** (`test_unit_sage_starformation_feedback.c`):
- SF rate calculation (given cold gas)
- Conservation (mass, metals, energy) - ask
- Edge cases (zero gas, high SFR, etc.)

**Integration Tests** (`test_integration_sage_starformation_feedback.py`):
- Full pipeline: infall → cooling → SF&F
- Property flow verification
- Mass conservation through full chain
- Module ordering dependencies

**Scientific Tests** (`test_scientific_sage_starformation_feedback.py`):
- Check stellar mass growth
- Verify metal enrichment
- Mass conservation tolerances - ask
- Statistical distributions (if applicable)

**Critical**: This module has complex mass flows - rigorous conservation testing essential

---

### Step 7: Documentation (1-2 days)

**Module README** (`src/modules/sage_starformation_feedback/README.md`):
- SF physics (Kennicutt-Schmidt)
- Feedback physics (SN, winds)
- Metal enrichment model
- Parameter descriptions
- References (Croton+2006, Springel+2003, etc.)

**User Docs**: Update `docs/user/module-configuration.md`, review all other docs, README.md files, CLAUDE.md

**Implementation Log**: Add entry to `docs/architecture/module-implementation-log.md`
- Challenges (expect: metal yields complexity, conservation validation)
- Solutions
- Lessons learned

---

### Step 8: Joint Pipeline Validation (2 days)

**Test File**: `tests/integration/test_infall_cooling_sfr_pipeline.py`

**Validate Full Chain**: sage_infall → sage_cooling → sage_starformation_feedback

**Tests**:
1. Execution order correct
2. Property flow: HotGas → ColdGas → StellarMass
3. Mass conservation: Mvir → all baryonic components
4. Metallicity tracking: through all phases

**Success Criteria**:
- Mass conservation within 1e-6
- All tests passing
- No memory leaks
- Performance acceptable

---

## Module Priority Reordered

**New Order** (based on physics dependencies):
1. ✅ sage_infall - Foundation (gas supply)
2. ✅ sage_cooling - Gas processing
3. **sage_starformation_feedback** - Core galaxy formation (NEXT)
4. sage_reincorporation - Gas return (depends on SF&F)
5. sage_mergers - Galaxy interactions
6. sage_disk_instability - Disk physics

**Rationale**: SF&F is central to galaxy formation. Implementing it enables better validation of infall+cooling chain and provides foundation for reincorporation (which returns ejected gas).

---

## Success Criteria for sage_starformation_feedback

Module complete when:
- [ ] All physics components implemented (SF, SN, winds, metals)
- [ ] Unit tests passing (expect 10-15 tests)
- [ ] Integration tests passing (expect 8-10 tests)
- [ ] Full pipeline validation passes
- [ ] Mass + metal + energy conservation verified
- [ ] Scientific comparison to SAGE acceptable
- [ ] Documentation complete
- [ ] No memory leaks
- [ ] Performance acceptable on full Millennium data

---

## Notes

**Module Developer Guide Updated** (2025-11-14):
- Pattern 6: Multi-file module organization (see sage_cooling)
- Pattern 7: Module-specific data files (cooling tables example)
- Scientific test deferral guidelines

**Reference Implementation**:
- sage_infall: Simple module, good starting template
- sage_cooling: Complex module with data files, helper functions

**Development Workflow**:
1. Copy template: `cp -r src/modules/_template src/modules/sage_starformation_feedback`
2. Create `module_info.yaml` from template
3. Implement incrementally (SF first, then feedback)
4. Test continuously (don't wait until end)
5. Run `make generate-modules` for auto-registration
6. Document as you go

---

## After sage_starformation_feedback

**Next Modules**:
- sage_reincorporation (1-2 weeks) - Returns ejected gas
- sage_mergers (2-3 weeks) - Galaxy mergers
- sage_disk_instability (2-3 weeks) - Bulge formation

**Total Remaining**: 3-4 months for all 6 SAGE modules complete
