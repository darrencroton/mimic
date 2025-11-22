# Mimic Modular Architecture: Roadmap v5

**Purpose**: Clear, actionable roadmap for completing Mimic's transformation into a production-ready galaxy evolution framework.

**Status**: Phase 4 substantially complete. All SAGE physics modules implemented. Now in integration and validation phase.

**Last Updated**: 2025-11-22

---

## Executive Summary

**Mission**: Transform Mimic into a physics-agnostic galaxy evolution framework with runtime-configurable SAGE physics modules.

**Current State**:
- ✅ **Core Infrastructure Complete**: Metadata-driven property system, comprehensive testing framework, runtime module configuration, automatic module discovery
- ✅ **All SAGE Physics Implemented**: 6 production modules ported from SAGE (infall, cooling, star formation & feedback, reincorporation, mergers, disk instability)
- ⏸️ **Integration In Progress**: Modules merged, unit tests passing (14/14), but full pipeline integration and scientific validation pending

**Path Forward**:
1. **Phase 4.4** (Current - Priority Reordered): SAGE scientific validation (physics accuracy verification)
   - Validate 4 working modules FIRST (infall, cooling, SF+feedback, reincorporation)
   - Then fix blocked modules (merger triggering, shared functions)
   - Then validate remaining 2 modules (mergers, disk instability)
   - Finally evaluate module reorganization
2. **Phase 5**: Build-time module selection (deployment optimization)

**Timeline**: 8-10 weeks for Phase 4.4, then production-ready for scientific use.

---

## Architectural Foundations (Complete)

### Core Principles

Mimic is built on 8 architectural principles (see [vision.md](vision.md)):

1. **Physics-Agnostic Core** - Zero knowledge of specific physics in core infrastructure
2. **Runtime Modularity** - Configure physics without recompilation
3. **Metadata-Driven** - YAML defines system structure, not hardcoded
4. **Single Source of Truth** - GalaxyData is authoritative state
5. **Unified Processing** - One consistent merger tree processing model
6. **Memory Efficiency** - Bounded, predictable, safe memory usage
7. **Format-Agnostic I/O** - Multiple input/output formats via unified interfaces
8. **Type Safety** - Compile-time validation, auto-generated type-safe accessors

### Module Communication Architecture

**IMPORTANT**: Modules do **NOT** communicate directly with each other.

**How Modules Interact**:
- **Galaxy Property System**: Modules communicate exclusively through the galaxy property system (single source of truth)
- **Read Properties**: Modules declare `requires:` in metadata (properties they read)
- **Write Properties**: Modules declare `provides:` in metadata (properties they populate)
- **Execution Order**: Core calls modules in configured order, passing galaxy state
- **No Direct Calls**: Modules cannot call other module functions directly

**Example**:
```
sage_infall → provides: HotGas, MetalsHotGas
                 ↓ (via galaxy property system)
sage_cooling → requires: HotGas, MetalsHotGas
               provides: ColdGas, MetalsColdGas, BlackHoleMass
                 ↓ (via galaxy property system)
sage_starformation_feedback → requires: ColdGas, MetalsColdGas
                              provides: StellarMass, MetalsStellarMass
```

**Shared Physics Utilities**:
- `src/modules/shared/` contains header-only utilities (e.g., `metallicity.h`, `disk_radius.h`)
- These are **NOT** module-to-module communication - they're reusable physics calculations
- Modules include them via relative paths: `#include "../shared/metallicity.h"`
- See [src/modules/shared/README.md](../../src/modules/shared/README.md)

**Key Insight**: This architecture maintains module independence while enabling complex physics pipelines through well-defined data contracts (property metadata).

---

## Phase 1-3: Foundational Infrastructure (Complete)

The foundational work is done. Key capabilities now available:

### Metadata-Driven Property System
- Define properties once in YAML (`galaxy_properties.yaml`, `halo_properties.yaml`)
- Auto-generate C structs, accessors, output code, Python dtypes
- 54 properties (23 galaxy, 31 halo) managed via single source of truth
- **Impact**: Adding properties reduced from 30 min/8 files to 2 min/1 file

### Comprehensive Testing Framework
- Three test tiers: unit (C), integration (Python), scientific (Python)
- Centralized test harness eliminates duplication
- Bit-identical regression tests prevent silent breakage
- 14 unit tests, comprehensive integration/scientific suites
- **Impact**: Catches critical bugs early (e.g., dT calculation error in PoC)

### Runtime Module Configuration
- `EnabledModules` parameter controls execution pipeline
- Module-specific parameters: `ModuleName_ParameterName` format
- Physics-free mode supported (pure halo tracking)
- YAML parameter files (industry-standard libyaml DOM parser)
- **Impact**: Researchers configure physics without recompilation

### Automatic Module Discovery
- Module metadata (`module_info.yaml`) drives code generation
- Auto-generates registration, test configuration, documentation
- Topological sort for dependency resolution
- Validation ensures property dependencies exist
- **Impact**: 75% reduction in manual synchronization errors

### Infrastructure Quality
- Module developer guide (500+ lines), template, implementation log
- Physics-agnostic test infrastructure (test_fixture module)
- Generated files organized in `generated/` subdirectories
- Streamlined metadata (removed redundant documentation generation)
- Property terminology accurate (provides vs creates)
- Enhanced validation (halo+galaxy properties, ERROR-level blocking)

**All foundational infrastructure complete as of November 2025.**

---

## Phase 4: SAGE Physics Modules (Substantially Complete)

### Phase 4.1: Developer Infrastructure ✅ Complete
- Module developer guide, template, implementation log
- Clear patterns for lifecycle, parameters, testing
- All tools ready for module development

### Phase 4.2: Module Implementation ✅ Complete

All 6 SAGE physics modules implemented and merged (November 2025):

| Module | Physics | Status | Tests |
|--------|---------|--------|-------|
| **sage_infall** | Gas accretion, satellite stripping, metallicity | ✅ Production Ready | 5 unit + integration |
| **sage_cooling** | Cooling tables, AGN heating, BH growth | ✅ Production Ready | 9 unit + integration |
| **sage_starformation_feedback** | Kennicutt-Schmidt SF, SN feedback, metal enrichment | ✅ Merged, Integration Pending | 6 unit + integration |
| **sage_reincorporation** | Gas return from ejected reservoir | ✅ Merged, Integration Pending | 6 unit + integration |
| **sage_mergers** | Dynamical friction, starbursts, BH growth, morphology | ⚠️ Blocked (merger triggering) | 3 expected warnings |
| **sage_disk_instability** | Disk stability, bulge formation | ✅ Merged, Partial | 6 unit tests |

**Code Quality**:
- All modules use centralized `safe_div()` utility (18 divisions updated)
- Compiler warnings resolved (expected warnings documented)
- All unit tests passing (14/14)
- Professional coding standards throughout

**Architecture Validations**:
- Metadata-driven registration works for all 8 modules
- Property system handles 23 galaxy properties without conflicts
- Build system manages full SAGE physics + system modules
- Module isolation maintained (physics-agnostic core)

### Phase 4.4: SAGE Scientific Validation 🔄 IN PROGRESS (Priority Reordered)

**Goal**: Verify physics accuracy against published SAGE results.

**Status**: **Active** - Validating working modules FIRST, then fixing/validating blocked modules.

**Priority Order** (Changed Nov 22, 2025):
1. **Validate 4 working modules** (Weeks 1-4)
2. **Fix blocked modules** (Weeks 5-6)
3. **Validate remaining 2 modules** (Weeks 7-8)
4. **Evaluate module reorganization** (Weeks 9-10)

---

#### Priority 1: Validate Working Modules (Weeks 1-4)

**Modules Ready for Validation**:
- ✅ **sage_infall**: Gas accretion, satellite stripping, metallicity
- ✅ **sage_cooling**: Cooling tables, AGN heating, BH growth
- ✅ **sage_starformation_feedback**: Kennicutt-Schmidt SF, SN feedback, metal enrichment
- ✅ **sage_reincorporation**: Gas return from ejected reservoir

**Validation Approach**:
1. **Week 1**: Validate sage_infall + sage_cooling individually
   - Gas accretion rates, reionization suppression, satellite stripping
   - Cooling rates, AGN modes, BH growth, mass/metal conservation
2. **Week 2-3**: Validate sage_starformation_feedback
   - Star formation law, supernova feedback, metal enrichment, recycling
   - Disk scale radius, mass conservation
3. **Week 3**: Validate sage_reincorporation
   - Reincorporation timescales, gas return, mass/metal conservation
4. **Week 4**: Integrated 4-module pipeline validation
   - Property flow across modules
   - Complete mass/metal budgets
   - Statistical comparison vs SAGE (4-module outputs)
   - Performance profiling

**Deliverables**: Scientific test files for each module, validation report

---

#### Priority 2: Fix Blocked Modules (Weeks 5-6)

**Blockers to Resolve**:

1. **Merger Triggering** (1-2 weeks)
   - Implement dynamical friction timescale in core
   - Mark satellites for merger when t_merge < t_current
   - Expose merger status to sage_mergers module
   - Unit tests for merger detection logic

2. **Shared Function Architecture** (1-2 days)
   - Extract starburst functions to `src/modules/shared/starburst.h`
   - Move `collisional_starburst_recipe()` and `grow_black_hole()` from sage_mergers
   - Update sage_disk_instability to use shared functions
   - Unit tests for shared starburst utilities

**Deliverables**: Working merger triggering, shared starburst utilities

---

#### Priority 3: Validate Blocked Modules (Weeks 7-8)

**Modules to Validate** (after fixes complete):
- **sage_mergers**: Dynamical friction, starbursts, BH growth, quasar feedback, morphology
- **sage_disk_instability**: Disk stability criterion, bulge formation, gas processing

**Validation Tasks**:
1. **Week 7**: Validate sage_mergers
   - Merger detection rates, merger physics, starbursts, BH growth
   - Quasar feedback, morphological transformations, mass conservation
2. **Week 7-8**: Validate sage_disk_instability
   - Stability criterion, disk→bulge transfer, gas processing, frequency
3. **Week 8**: Full 6-module pipeline validation
   - Complete mass/metal budgets (all components)
   - SAGE comparison (stellar mass function, gas distributions, metallicities)
   - Performance on full Millennium data

**Deliverables**: Scientific test files for sage_mergers + sage_disk_instability, comprehensive SAGE comparison report

---

#### Priority 4: Module Reorganization (Weeks 9-10)

**Goal**: Based on validation experience, decide if modules should be split for better configurability.

**Potential Splits to Evaluate**:

1. **sage_infall** → `sage_infall` + `sage_reionization`
   - Rationale: Reionization is separate process, can disable independently
   - Benefit: Easier testing, clearer physics separation
   - Effort: 2-3 days

2. **sage_starformation_feedback** → `sage_starformation` + `sage_feedback`
   - Rationale: SF and feedback are distinct processes
   - Benefit: Can disable feedback for testing, easier validation
   - Effort: 3-4 days

3. **sage_cooling** → `sage_cooling` + `sage_agn`
   - Rationale: Gas cooling vs AGN feedback are separate
   - Benefit: Can disable AGN independently, easier testing
   - Effort: 3-4 days

4. **sage_mergers** → `sage_mergers` + `sage_starbursts`
   - Rationale: Starbursts occur outside mergers (disk instability)
   - Benefit: Shared physics between modules, better reusability
   - Effort: 2-3 days (already in shared/, formalize as module)

5. **sage_disk_instability** → Keep as-is (focused module)
   - Evaluation: Probably doesn't need splitting

6. **sage_reincorporation** → Keep as-is (already minimal)
   - Evaluation: Simple module, splitting would over-engineer

**Decision Process**:
- Review validation experience (what was hard to test/debug?)
- Consider user configurability needs (what should be toggleable?)
- Evaluate maintenance burden (is splitting worth complexity?)
- Only split if clear benefits demonstrated

**Timeline**: 1 day evaluation + 2-4 days per split

---

**Phase 4.4 Success Criteria**:
- All 6 modules validated against SAGE reference
- Physics accuracy verified (statistical tests pass)
- Merger triggering implemented and tested
- Shared function architecture resolved
- Complete mass/metal conservation across full pipeline
- SAGE comparison complete with quantified differences
- Module reorganization decisions documented
- Any needed splits implemented and re-validated
- Performance acceptable on Millennium data

**Timeline**: 8-10 weeks total

---

## Phase 5: Build-Time Module Selection (Planned)

**Goal**: Compile-time module selection for deployment optimization.

**Why**: Enables lean, specialized binaries for specific science cases.

**Prerequisites**: Phase 4 complete (all modules validated).

**Key Deliverables**:
1. **Build Profiles**: `make PROFILE=minimal`, `make PROFILE=all`
2. **Custom Module Selection**: `make MODULES="cooling_SD93 stellar_mass"`
3. **Automatic Code Generation**: Registration code generated only for selected modules
4. **Clean Errors**: Runtime error if disabled module requested in parameter file

**Infrastructure Ready**: Module metadata system already supports this (foundation in Phase 4.2.5).

**Timeline**: 1 week implementation once Phase 4 validated.

---

## Current Priorities (November 2025 - Reordered)

### Immediate: Scientific Validation First (Weeks 1-4)
1. **Validate sage_infall**: Gas accretion, reionization, satellite stripping vs SAGE
2. **Validate sage_cooling**: Cooling rates, AGN modes, BH growth vs SAGE
3. **Validate sage_starformation_feedback**: SF law, SN feedback, metal enrichment vs SAGE
4. **Validate sage_reincorporation**: Gas return timescales vs SAGE
5. **4-Module Pipeline**: Integrated validation, mass/metal budgets, SAGE comparison

### Short-Term: Fix Blocked Modules (Weeks 5-6)
6. **Merger Triggering**: Implement dynamical friction timescale in core
7. **Shared Functions**: Extract starburst utilities to `src/modules/shared/`

### Medium-Term: Complete Validation (Weeks 7-8)
8. **Validate sage_mergers**: Mergers, starbursts, BH growth, quasar feedback vs SAGE
9. **Validate sage_disk_instability**: Stability criterion, bulge formation vs SAGE
10. **6-Module Pipeline**: Full SAGE comparison, performance profiling

### Long-Term: Optimize Structure (Weeks 9-10)
11. **Evaluate Module Splits**: Infall+reionization, SF+feedback, cooling+AGN, mergers+starbursts
12. **Implement Splits**: Only if validation reveals clear benefits
13. **Re-validate**: Ensure splits don't break physics

---

## Future Work (Post-Phase 5)

**Scientific Extensions**:
- Alternative physics models (different cooling, SF recipes)
- Parameter optimization tools
- Advanced validation (statistical comparison, visualization)

**Infrastructure Enhancements** (as needs arise):
- Performance regression testing
- Shared physics libraries (ODE solvers, interpolation)
- Advanced module interfaces (shared RNG, cosmology utilities)

**Approach**: Let demonstrated needs drive enhancements, not speculation.

---

## Key Success Metrics

**Scientific Achievement**:
- ✅ Complete SAGE physics implementation
- ⏸️ Scientifically validated against published results
- ⏸️ Production-ready for research use

**Architectural Achievement**:
- ✅ Physics-agnostic core with runtime modularity
- ✅ Metadata-driven property system at scale
- ✅ Comprehensive testing at all levels

**Developer Achievement**:
- ✅ Battle-tested development workflow
- ✅ Comprehensive documentation
- ⏸️ Proven scientific validation workflow

---

## Summary

**What's Done**:
- All foundational infrastructure (Phases 1-3)
- All SAGE physics modules implemented (Phase 4.2)
- Developer tooling and documentation complete

**What's Next**:
- Architectural decisions and merger triggering (Phase 4.3)
- Full pipeline integration and validation (Phase 4.3)
- Scientific accuracy verification (Phase 4.4)
- Build-time optimization (Phase 5)

**Timeline to Production**:
- 2-4 months to complete Phases 4.3-4.4
- Then production-ready for scientific research
- Phase 5 adds deployment flexibility (1 week)

**Philosophy**: Take time to make good architectural decisions. Rush integration leads to rework. Professional code quality, thorough testing, and comprehensive documentation are the foundation for long-term scientific productivity.

---

## Resources

- **Vision & Principles**: [docs/architecture/vision.md](vision.md)
- **Module Development**: [docs/developer/module-developer-guide.md](../developer/module-developer-guide.md)
- **Testing Guide**: [docs/developer/testing.md](../developer/testing.md)
- **Module Configuration**: [docs/user/module-configuration.md](../user/module-configuration.md)
- **Next Tasks**: [docs/architecture/next-task.md](next-task.md)
- **Implementation Log**: [docs/architecture/module-implementation-log.md](module-implementation-log.md)
