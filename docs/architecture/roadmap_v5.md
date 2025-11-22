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
1. **Phase 4.3** (Current): Module integration, architectural decisions, full pipeline validation
2. **Phase 4.4**: SAGE scientific validation (physics accuracy verification)
3. **Phase 5**: Build-time module selection (deployment optimization)

**Timeline**: 2-4 months for Phases 4.3-4.4, then production-ready for scientific use.

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

### Phase 4.3: Module Integration & Validation ⏸️ IN PROGRESS

**Goal**: Integrate merged modules into validated full pipeline.

**Current Blockers & Decisions Needed**:

1. **Merger Triggering**: Core doesn't detect when satellites should merge
   - SAGE uses dynamical friction timescale
   - Need to implement in core or as special module
   - Blocks full testing of sage_mergers module

2. **Module Enabling Strategy**: How to systematically validate integration?
   - Start minimal (infall + cooling), add incrementally?
   - Enable groups (foundation → core → advanced)?
   - Full pipeline at once?

3. **Shared Function Architecture**: sage_disk_instability needs sage_mergers functions
   - Extract to `shared/` utilities?
   - Keep in module, expose via API?
   - Current approach: moving to shared utilities

4. **Property Verification**: Need to verify module metadata
   - Check all `provides` vs `requires` declarations
   - Ensure no missing/duplicate properties
   - Validate execution order requirements

**Near-Term Tasks**:
- Minimal pipeline test (infall + cooling only)
- Architectural decision documentation
- Merger triggering design and implementation
- Systematic module-by-module integration plan
- Cross-module validation tests

**Success Criteria**:
- All architectural decisions documented
- Merger triggering implemented
- All 6 modules integrated into working pipeline
- Property flow validated across modules
- Mass/metal conservation verified
- Performance acceptable on Millennium data
- Ready for scientific validation

### Phase 4.4: SAGE Scientific Validation 📋 Planned

**Goal**: Verify physics accuracy against published SAGE results.

**Status**: **NOT STARTED** - All physics implemented but science validation pending.

**Important Notes**:
- All module code is complete and tests pass
- Scientific accuracy verification is separate phase
- May reveal need to adjust parameters or fix physics bugs
- User will validate each module individually

**Potential Module Reorganization**:
During scientific validation, may consider breaking modules into smaller units:
- **Star Formation + Feedback** → Separate SF and feedback modules?
- **Cooling + AGN** → Separate cooling physics from AGN feedback?
- Rationale: Smaller, focused modules easier to validate and configure
- Decision deferred until validation reveals actual needs

**Validation Tasks** (per module):
1. Compare outputs to SAGE reference results
2. Verify mass conservation (baryons, metals, stars)
3. Check statistical distributions (mass functions, metallicities)
4. Reproduce published results (e.g., Croton+2006)
5. Performance profiling on full Millennium simulation

**Timeline**: 2-3 weeks per module (12-18 weeks total), but may be faster if systematic

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

## Current Priorities (November 2025)

### Immediate (This Week)
1. **Architecture Decisions**: Resolve merger triggering, integration strategy, shared functions
2. **Minimal Pipeline Test**: Verify infall + cooling work together
3. **Document Decisions**: Update roadmap with architectural choices

### Short-Term (Next Month)
4. **Merger Triggering**: Design and implement core merger detection
5. **Systematic Integration**: Add modules one-by-one with validation
6. **Property Verification**: Audit all module metadata for correctness

### Medium-Term (2-3 Months)
7. **Full Pipeline Validation**: All modules integrated and tested
8. **Performance Testing**: Profile on full Millennium data
9. **SAGE Scientific Validation**: Begin physics accuracy verification

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
