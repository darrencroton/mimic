# Mimic Development Roadmap

**Purpose**: Clear, actionable roadmap for Mimic development priorities

**Status**: Core infrastructure complete (v3.0 multi-phase pipeline). SAGE physics modules implemented. Test suite updated for new architecture.

**Last Updated**: 2025-12-09

---

## Executive Summary

**Mission**: Mimic is a physics-agnostic galaxy evolution framework with runtime-configurable physics modules.

**Current State**:
- ✅ **Core Infrastructure Complete (v3.0)**: Multi-phase pipeline with time sub-stepping, metadata-driven property system, comprehensive testing framework, runtime module configuration, automatic module discovery, decentralized parameter system
- ✅ **All SAGE Physics Implemented**: 6 production modules ported from SAGE (infall, cooling, star formation & feedback, reincorporation, mergers, disk instability)
- ✅ **Multi-Phase Pipeline Operational**: 4-phase execution (pre_timestep, phase_1, phase_2, post_timestep), galaxy-major loops, time sub-stepping support (commit 58d51e4)
- ✅ **Core Test Suite Updated**: All 14 unit tests passing, core integration tests passing, test framework supports new configuration
- 🔄 **Module Integration Tests**: Need update for new multi-phase configuration format

**Path Forward**:
1. **Current Priority**: Complete module integration test updates, SAGE scientific validation
2. **Future Work**: Build-time module selection, alternative physics models, performance optimization

---

## Architectural Foundations

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
sage_calculate_infall → provides: HotGas, MetalsHotGas
                 ↓ (via galaxy property system)
sage_cooling → requires: HotGas, MetalsHotGas
               provides: ColdGas, MetalsColdGas, BlackHoleMass
                 ↓ (via galaxy property system)
sage_starformation_feedback → requires: ColdGas, MetalsColdGas
                              provides: StellarMass, MetalsStellarMass
```

**Shared Physics Utilities**:
- `src/modules/_shared/` contains header-only utilities for reusable physics calculations
- These are **NOT** module-to-module communication - they're shared computational tools
- Modules include them via relative paths: `#include "../_shared/my_utility.h"`
- See [src/modules/_shared/README.md](../../src/modules/_shared/README.md)

**Key Insight**: This architecture maintains module independence while enabling complex physics pipelines through well-defined data contracts (property metadata).

---

## Completed Infrastructure

The foundational work is complete. Key capabilities now available:

### Metadata-Driven Property System
- Define properties once in YAML (`model_properties.yaml`, `halo_properties.yaml`)
- Auto-generate C structs, accessors, output code, Python dtypes
- 54 properties (23 galaxy, 31 halo) managed via single source of truth
- **Impact**: Adding properties reduced from 30 min/8 files to 2 min/1 file

### Module-Owned Parameter System
- Modules read parameters directly from input YAML file via type-safe accessors
- Physics-based validation in each module's init function
- 20+ physics parameters across 7 modules
- **No defaults**: All parameters MUST be explicitly specified in input file for reproducible science
- **Module responsibility**: Each module reads and validates its own parameters
- **Impact**: Simple, direct parameter access with physics context for validation

### Comprehensive Testing Framework
- Three test tiers: unit (C), integration (Python), scientific (Python)
- Centralized test harness eliminates duplication
- Bit-identical regression tests prevent silent breakage
- 14 unit tests, comprehensive integration/scientific suites
- **Impact**: Catches critical bugs early

### Multi-Phase Pipeline Architecture (v3.0)
- **4 execution phases**: pre_timestep, phase_1, phase_2, post_timestep
- **2 processing modes**: PROCESSING_MODE_FULL_HALO (array processing), PROCESSING_MODE_BY_GALAXY (galaxy-major loop)
- **Time sub-stepping**: SubSteps parameter enables numerical stability
- **Configuration-driven**: Pipeline structure defined in input YAML
- **Physics-free mode**: Supported (pure halo tracking, no parameters needed)
- **YAML configuration**: Industry-standard libyaml DOM parser
- **Impact**: Researchers configure execution structure and physics without recompilation
- **Breaking change**: Clean architecture with no backwards compatibility (commit 58d51e4)

### Automatic Module Discovery
- Module metadata (`module_info.yaml`) drives code generation
- Auto-generates registration, test configuration, documentation
- Topological sort for dependency resolution
- Validation ensures property dependencies exist
- **Impact**: 75% reduction in manual synchronization errors

### Infrastructure Quality
- Module developer guide, template, implementation log
- Physics-agnostic test infrastructure (test_fixture module)
- Generated files organized in `generated/` subdirectories
- Enhanced validation (halo+galaxy properties, ERROR-level blocking)

---

## SAGE Physics Modules

All 6 SAGE physics modules implemented and merged (November 2025):

| Module | Physics | Status | Tests |
|--------|---------|--------|-------|
| **sage_calculate_infall** | Gas accretion, satellite stripping, metallicity | ✅ Production Ready | 5 unit + integration |
| **sage_cooling** | Cooling tables, AGN heating, BH growth | ✅ Production Ready | 9 unit + integration |
| **sage_starformation_feedback** | Kennicutt-Schmidt SF, SN feedback, metal enrichment | ✅ Merged, Integration Pending | 6 unit + integration |
| **sage_reincorporation** | Gas return from ejected reservoir | ✅ Merged, Integration Pending | 6 unit + integration |
| **sage_mergers** | Dynamical friction, starbursts, BH growth, morphology | ⚠️ Blocked (merger triggering) | 3 expected warnings |
| **sage_disk_instability** | Disk stability, bulge formation | ✅ Merged, Partial | 6 unit tests |

**Code Quality**:
- All modules use centralized `safe_div()` utility
- Compiler warnings resolved (expected warnings documented)
- All unit tests passing (14/14)
- Professional coding standards throughout

**Architecture Validations**:
- Metadata-driven registration works for all modules
- Property system handles 23 galaxy properties without conflicts
- Build system manages full SAGE physics + system modules
- Module isolation maintained (physics-agnostic core)

---

## Current Priorities

### Scientific Validation (In Progress)

**Goal**: Verify physics accuracy against published SAGE results.

**Approach**:
1. **Validate working modules first**: infall, cooling, star formation & feedback, reincorporation
2. **Fix blocked modules**: Implement merger triggering, extract shared starburst functions
3. **Validate remaining modules**: mergers, disk instability
4. **Evaluate module organization**: Consider splitting combined modules for better configurability

**Validation Tasks**:
- Gas accretion rates, reionization suppression, satellite stripping
- Cooling rates, AGN modes, BH growth, mass/metal conservation
- Star formation law, supernova feedback, metal enrichment, recycling
- Reincorporation timescales, gas return
- Merger detection, starbursts, quasar feedback, morphology
- Disk stability criterion, bulge formation

**Outstanding Blockers**:
1. **Merger Triggering**: Implement dynamical friction timescale in core
2. **Shared Function Architecture**: Extract starburst functions to `src/modules/_shared/starburst.h`

**Success Criteria**:
- All 6 modules validated against SAGE reference
- Physics accuracy verified (statistical tests pass)
- Complete mass/metal conservation across full pipeline
- Performance acceptable on Millennium data

---

## Future Work

### Build-Time Module Selection

**Goal**: Compile-time module selection for deployment optimization.

**Why**: Enables lean, specialized binaries for specific science cases.

**Key Deliverables**:
1. **Build Profiles**: `make PROFILE=minimal`, `make PROFILE=process_by_galaxy`
2. **Custom Module Selection**: `make MODULES="cooling_SD93 stellar_mass"`
3. **Automatic Code Generation**: Registration code generated only for selected modules
4. **Clean Errors**: Runtime error if disabled module requested in parameter file

**Infrastructure Ready**: Module metadata system already supports this.

### Scientific Extensions

**As needs arise**:
- Alternative physics models (different cooling, SF recipes)
- Parameter optimization tools
- Advanced validation (statistical comparison, visualization)

### Infrastructure Enhancements

**As demonstrated needs arise**:
- Performance regression testing
- Shared physics libraries (ODE solvers, interpolation)
- Advanced module interfaces (shared RNG, cosmology utilities)

**Approach**: Let demonstrated needs drive enhancements, not speculation.

---

## Key Success Metrics

**Scientific Achievement**:
- ✅ Complete SAGE physics implementation
- 🔄 Scientifically validated against published results
- ⏸️ Production-ready for research use

**Architectural Achievement**:
- ✅ Physics-agnostic core with runtime modularity
- ✅ Metadata-driven property system at scale
- ✅ Centralized model parameter system
- ✅ Comprehensive testing at all levels

**Developer Achievement**:
- ✅ Battle-tested development workflow
- ✅ Comprehensive documentation
- 🔄 Proven scientific validation workflow

---

## Summary

**What's Done**:
- All foundational infrastructure
- Decentralized parameter system v2.0 (22 parameters, no defaults, smart validation, complete module independence)
- All SAGE physics modules implemented
- Developer tooling and documentation complete

**What's Next**:
- Fix outstanding blockers (merger triggering, shared functions)
- Complete scientific validation
- Build-time optimization (when needed)

**Philosophy**: Professional code quality, thorough testing, and comprehensive documentation are the foundation for long-term scientific productivity.

---

## Resources

- **Vision & Principles**: [docs/architecture/vision.md](vision.md)
- **Module Development**: [docs/developer/module-developer-guide.md](../developer/module-developer-guide.md)
- **Module Metadata Schema**: [docs/developer/module-metadata-schema.md](../developer/module-metadata-schema.md) (includes parameter definitions)
- **Testing Guide**: [docs/developer/testing.md](../developer/testing.md)
- **Module Configuration**: [docs/user/module-configuration.md](../user/module-configuration.md)
- **Next Tasks**: [docs/architecture/next-task.md](next-task.md)
