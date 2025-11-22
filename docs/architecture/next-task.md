# Next Tasks: Phase 4.3 Module Integration

**Current Phase**: Phase 4.3 - Module Integration & Validation
**Status**: All 6 SAGE modules merged and compiled. Ready for systematic integration.
**Last Updated**: 2025-11-22

---

## Current State

### ✅ Completed
- All 6 SAGE physics modules implemented and merged to main
- All module code compiles successfully
- Unit tests passing (14/14)
- Code quality improvements complete (safe_div integration, warnings resolved)
- Module metadata system working (8 modules registered)

### ⏸️ Current Blockers
- **Merger Triggering**: Core doesn't detect when satellites should merge (blocks sage_mergers testing)
- **Integration Strategy**: Need systematic approach to enable modules incrementally
- **Property Verification**: Module metadata needs audit (provides vs requires)
- **Shared Functions**: Architecture for functions used by multiple modules

---

## Immediate Next Steps

### Step 1: Minimal Pipeline Validation (This Week)

**Goal**: Verify that the two production-ready modules work together.

**Tasks**:
1. Edit `input/millennium.yaml`:
   - Set `EnabledModules: [sage_infall, sage_cooling]`
   - Remove all other modules
2. Run full pipeline: `./mimic input/millennium.yaml`
3. Verify execution completes successfully
4. Check property flow (HotGas → ColdGas)
5. Validate no memory leaks
6. Document baseline behavior

**Success Criteria**:
- Pipeline completes without errors
- Both modules execute in correct order
- Properties appear in output
- Memory management clean
- Performance acceptable

**Timeline**: 1 day

---

### Step 2: Architectural Decisions (This Week)

Need to resolve critical architectural questions before proceeding:

#### Decision A: Merger Triggering Architecture

**Question**: Where and how should merger detection be implemented?

**Context**:
- SAGE uses dynamical friction timescale to determine when satellite merges
- Currently: Core calls modules but doesn't mark satellites for merger
- sage_mergers module is complete but can't be tested without triggering

**Options to Evaluate**:
1. **New core module** (`src/core/merger_detection.c`)
   - Pro: Clean separation, physics-agnostic core maintained
   - Con: Adds new core component

2. **Integrated into build_model.c**
   - Pro: Natural fit in halo processing
   - Con: Adds physics knowledge to core

3. **Two-pass module approach**
   - First pass: sage_mergers marks satellites for merger
   - Second pass: sage_mergers executes mergers on marked halos
   - Pro: Keeps physics in modules
   - Con: More complex module execution model

**To Decide**:
- Which approach best maintains physics-agnostic core?
- What data structures needed (merger queue, flags)?
- When in timestep do mergers execute?
- How to handle multiple mergers per timestep?

**Timeline**: 2-3 days discussion + design document

---

#### Decision B: Shared Function Architecture

**Question**: How should functions used by multiple modules be shared?

**Context**:
- sage_disk_instability needs functions from sage_mergers:
  - `collisional_starburst_recipe()`
  - `grow_black_hole()`
- Current pattern: `src/modules/shared/` for header-only utilities
- Need to decide if this extends to more complex physics

**Options**:
1. **Extract to shared utilities** (current approach)
   - Move functions to `src/modules/shared/starburst.h`
   - Both modules include shared header
   - Pro: Maintains single source of truth
   - Con: Utility becomes more complex

2. **Keep in owning module**
   - sage_mergers exports functions via header
   - sage_disk_instability includes sage_mergers header
   - Pro: Clear ownership
   - Con: Creates module dependency

3. **Accept duplication**
   - Copy functions to both modules
   - Pro: Complete module independence
   - Con: Violates DRY principle

**Recommendation**: Extract to shared utilities (maintains architecture)

**Timeline**: 1 day

---

#### Decision C: Module Integration Strategy

**Question**: How to systematically enable and validate modules?

**Options**:
1. **Incremental (Recommended)**
   - Start: infall + cooling (validated above)
   - Add: sage_starformation_feedback
   - Add: sage_reincorporation
   - Add: sage_disk_instability (requires shared functions)
   - Add: sage_mergers (requires merger triggering)
   - Validate after each addition

2. **Groups**
   - Foundation: infall + cooling
   - Core: + starformation_feedback + reincorporation
   - Advanced: + disk_instability + mergers

3. **All at once**
   - Enable all modules simultaneously
   - Debug issues as they arise

**Recommendation**: Incremental (option 1) - catches integration issues early

**Timeline**: Planning 1 day, execution 1-2 weeks

---

### Step 3: Property Metadata Audit (Next Week)

**Goal**: Verify all module metadata is correct.

**Tasks**:
1. Review `galaxy_properties.yaml`:
   - Check all 23 properties have clear descriptions
   - Verify units are correct
   - Consider logical grouping (gas, stars, metals, etc.)

2. Audit each module's `module_info.yaml`:
   - Verify `provides` lists all properties module populates
   - Verify `requires` lists all properties module reads
   - Check for missing dependencies
   - Validate execution order implications

3. Create property flow diagram:
   - Document which modules provide each property
   - Document which modules require each property
   - Identify any circular dependencies
   - Validate execution order makes sense

4. Check for issues:
   - Properties provided by multiple modules (conflicts?)
   - Properties required but never provided (missing?)
   - Properties provided but never required (unused?)

**Success Criteria**:
- All module metadata accurate
- Property flow documented
- No missing/conflicting dependencies
- Execution order validated

**Timeline**: 2-3 days

---

## Medium-Term Tasks (Next Month)

### Task 4: Implement Merger Triggering

**Based on**: Architectural decision from Step 2

**Estimated Tasks**:
1. Design data structures (merger queue, satellite flags)
2. Implement merger detection logic (dynamical friction)
3. Add merger triggering to core execution flow
4. Update sage_mergers to use triggering system
5. Write tests (unit + integration)
6. Document architecture

**Timeline**: 2-4 weeks (depending on complexity of chosen approach)

**Success Criteria**:
- Satellites marked for merger based on physics
- sage_mergers executes on marked satellites
- Tests validate merger physics
- Documentation complete

---

### Task 5: Systematic Module Integration

**Incremental Integration Steps**:

**Step 5.1: Add sage_starformation_feedback**
- Update parameter file
- Run pipeline
- Verify property flow: ColdGas → StellarMass
- Check mass conservation (gas → stars)
- Validate metal enrichment
- Performance check

**Step 5.2: Add sage_reincorporation**
- Update parameter file
- Run pipeline
- Verify gas return: EjectedMass → HotGas
- Check mass/metal conservation
- Validate timestep integration
- Performance check

**Step 5.3: Add sage_disk_instability**
- Resolve shared function architecture first
- Move starburst/BH functions to shared/
- Update both modules
- Run pipeline
- Verify disk instability physics
- Check bulge formation
- Performance check

**Step 5.4: Add sage_mergers**
- Requires merger triggering complete
- Update parameter file
- Run full pipeline (all 6 modules)
- Verify merger physics
- Check BH growth, starbursts
- Validate morphology changes
- Full mass/metal conservation check
- Performance profiling

**Timeline**: 1-2 weeks (depends on architectural decisions and merger triggering)

**Success Criteria Each Step**:
- Module executes without errors
- Properties appear in output with valid ranges
- Mass/metal conservation maintained
- No memory leaks
- Performance acceptable
- Integration tests pass

---

### Task 6: Cross-Module Validation Tests

**Goal**: Verify modules work correctly together.

**Test Types**:

1. **Property Flow Tests**
   - Verify data flows correctly between modules
   - Example: `test_integration_infall_cooling_pipeline.py`
   - Check each module gets expected input from previous

2. **Conservation Tests**
   - Mass conservation across all modules
   - Metal conservation across all modules
   - Energy budget (if applicable)

3. **Full Pipeline Tests**
   - All 6 modules enabled
   - Verify execution order
   - Check for anomalies in output
   - Performance benchmarking

**Timeline**: 1 week

---

## Long-Term Tasks (2-3 Months)

### Task 7: SAGE Scientific Validation (Phase 4.4)

**Goal**: Verify physics accuracy against published SAGE results.

**Per-Module Validation**:
1. sage_infall: Gas accretion rates, reionization suppression
2. sage_cooling: Cooling rates, AGN heating, BH growth
3. sage_starformation_feedback: SFR, feedback efficiency, metal enrichment
4. sage_reincorporation: Gas return timescales
5. sage_mergers: Merger rates, starbursts, BH growth
6. sage_disk_instability: Disk instability frequency, bulge formation

**Validation Methods**:
- Compare to SAGE reference outputs (statistical distributions)
- Reproduce published results (e.g., Croton+2006 mass functions)
- Mass/metal budgets match SAGE
- Parameter sensitivity tests

**Important Notes**:
- All physics code complete, but scientific accuracy unverified
- May reveal parameter tuning needs or physics bugs
- User will validate each module individually
- May identify need to split modules (SF+feedback, cooling+AGN)

**Timeline**: 2-3 weeks per module (can parallelize some validation tasks)

---

### Task 8: Module Reorganization (If Needed)

**Context**: During scientific validation, may discover modules should be split.

**Potential Splits**:
1. **sage_starformation_feedback** → `sage_starformation` + `sage_feedback`
   - Rationale: Separate star formation physics from feedback physics
   - Benefit: Easier to configure/validate independently

2. **sage_cooling** → `sage_cooling` + `sage_agn`
   - Rationale: Separate gas cooling from AGN heating
   - Benefit: Can disable AGN while keeping cooling

**Decision Point**: After initial scientific validation reveals actual needs

**Timeline**: 1-2 weeks per split (if needed)

---

## Success Criteria (Phase 4.3 Complete)

Phase 4.3 complete when:

- [ ] All architectural decisions documented
- [ ] Merger triggering implemented and tested
- [ ] All 6 SAGE modules integrated into working pipeline
- [ ] Module execution order validated
- [ ] Property flow verified across modules
- [ ] Mass/metal conservation verified across pipeline
- [ ] Performance acceptable on Millennium data
- [ ] Cross-module integration tests passing
- [ ] Ready to begin SAGE scientific validation (Phase 4.4)

**Then**: Move to Phase 4.4 (scientific validation) and Phase 5 (build-time optimization)

---

## How to Use This Document

1. **Start at Step 1**: Minimal pipeline validation
2. **Make Decisions**: Resolve architectural questions in Step 2
3. **Execute Plan**: Follow Steps 3-6 sequentially
4. **Update as You Go**: Mark tasks complete, add new tasks as discovered
5. **Reference Roadmap**: See roadmap_v5.md for big picture

**This is a living document** - update as decisions are made and work progresses.

---

## Notes

- **Don't rush**: Good architectural decisions prevent rework
- **Test incrementally**: Catch issues early by adding one module at a time
- **Document decisions**: Future developers need to understand choices
- **Validate thoroughly**: Scientific accuracy is paramount

**Philosophy**: Professional software development takes time. Do it right the first time.
