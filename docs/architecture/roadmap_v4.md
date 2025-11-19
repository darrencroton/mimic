# Mimic Modular Architecture: Implementation Plan v4

**Purpose**: To provide a clear, actionable plan for completing the transformation of Mimic into a modular framework for galaxy physics. This document leverages the validated patterns from two successful Proof-of-Concept (PoC) rounds to define the remaining work.

**Audience**: The development team (AI or human) responsible for implementation.

**Status**: The foundational infrastructure is **COMPLETE**. The critical, high-risk discovery work is done. We are now in the execution phase, building upon a proven architecture.

### Executive Summary

Mimic's core halo-tracking infrastructure is robust. The goal of this initiative is to enable it to accept interchangeable galaxy physics modules. Through two PoC rounds, we have validated the architecture and established a clear, low-risk path to completion.

**Current State: A Solid Foundation**
The initial, most difficult phases are complete. We now have:
1.  A **Metadata-Driven Property System** that makes adding new physical properties trivial (2 minutes vs. 30 minutes).
2.  A **Comprehensive Testing Framework** that prevents critical bugs and ensures scientific validity.
3.  **Runtime Module Configuration** allowing scientists to enable, disable, and order physics modules from a parameter file without recompiling.

**The Path Forward: What's Next**
The remaining work focuses on leveraging this new infrastructure to implement production-quality science and add deployment flexibility.

1.  **Phase 4: SAGE Physics Module Implementation** - Systematically port production physics modules from SAGE into Mimic's modular architecture.
2.  **Phase 5: Build-Time Module Selection** - Optimize for deployment by allowing modules to be compiled in or out.

**Revised Timeline**: **6-12 months** for complete SAGE physics implementation (6 modules). Phase 5 can proceed in parallel after first 2-3 modules.

---

### Current State & Key Achievements (What's Done)

This section summarizes the capabilities of the codebase *right now*. The detailed PoC findings and implementation notes from the original v3 roadmap have been archived but are no longer required for the developer to proceed.

#### ✅ **1. Metadata-Driven Property System** (Formerly Phase 1)
-   **Capability**: Adding a new galaxy property (e.g., `StellarMass`, `ColdGas`) is now a fast, error-proof process.
-   **How**: Define the property once in a `galaxy_properties.yaml` file. All necessary C code, output configurations, and plotting script data types are auto-generated.
-   **Impact**: Eliminates the primary bottleneck identified in the PoCs, where adding one property required manually editing 8+ files. This unblocks rapid development of new physics modules.

#### ✅ **2. Comprehensive Testing Framework** (Formerly Phase 2)
-   **Capability**: We have unit, integration, and scientific validation tests integrated into our CI pipeline.
-   **How**: A combination of a minimal C unit test framework and a powerful Python-based integration/scientific testing suite. It includes bit-identical regression tests.
-   **Impact**: Prevents critical, hard-to-find bugs like the `dT` calculation error discovered during the PoCs. Ensures that new development is safe, and the science is correct.

#### ✅ **3. Runtime Module Configuration** (Formerly Phases 3 & 3.5)
-   **Capability**: Users can select which physics modules to run, in what order, and configure their parameters directly from the `.par` file.
-   **How**: The `EnabledModules` parameter controls the execution pipeline. Module-specific parameters (e.g., `SimpleCooling_BaryonFraction`) are parsed and passed to the correct module.
-   **Impact**: Provides maximum scientific flexibility. Researchers can easily compare different physics combinations without needing to recompile the code. Mimic can also be run in a "physics-free" mode for pure halo tracking.

#### ✅ **4. Developer Infrastructure for Module Implementation** (Phase 4.1 - Completed 2025-11-12)
-   **Capability**: Complete development environment for creating new physics modules.
-   **Components**:
    - **Module Developer Guide** (`docs/developer/module-developer-guide.md`): Comprehensive 500+ line guide covering module lifecycle, patterns, testing, and best practices
    - **Module Template** (`src/modules/_system/template/`): Annotated boilerplate code for rapid module creation
    - **Module Implementation Log** (`docs/architecture/module-implementation-log.md`): Structured framework for capturing lessons learned
-   **Impact**: Developers can create new modules following proven patterns with clear documentation. Template provides working boilerplate code. Implementation log ensures institutional knowledge is preserved across module implementations.

#### ✅ **5. Automatic Module Discovery System** (Phase 4.2.5 - Completed 2025-11-12)
-   **Capability**: Metadata-driven automatic module registration, eliminating manual synchronization.
-   **Components**:
    - **Module Metadata Schema** (`docs/developer/module-metadata-schema.md`): Complete YAML schema specification for module metadata
    - **Validation Script** (`scripts/validate_modules.py`): Comprehensive validation with dependency checking, circular dependency detection, and schema validation
    - **Generator Script** (`scripts/generate_module_registry.py`): Automatic code generation with topological sort for dependency resolution
    - **Build Integration**: Makefile targets for `generate-modules`, `validate-modules`, automatic regeneration on YAML changes
    - **Module Metadata Files**: `module_info.yaml` for all existing modules (sage_infall, simple_cooling, simple_sfr)
-   **Generated Outputs**:
    - `src/modules/_system/generated/module_init.c` - Auto-generated registration code
    - `tests/generated/module_sources.mk` - Test build configuration
    - `docs/generated/module-reference.md` - Module documentation (symlinked from docs/user/)
    - `build/generated/module_registry_hash.txt` - Validation hash
-   **Impact**:
    - **75% reduction in error opportunities** (5 manual steps → 1 metadata file)
    - **Time savings**: 20-50 minutes/module eliminated through automation
    - **Architectural integrity**: Implements Principles 3 (Metadata-Driven), 4 (Single Source of Truth), 5 (Unified Processing), 8 (Type Safety & Validation)
    - **Foundation for Phase 5**: Build-time module selection infrastructure ready
-   **Status**: ✅ **PRODUCTION READY** - All tests passing, build system integrated, documentation complete

#### ✅ **6. Test Infrastructure Architecture Fix** (Phase 4.2.6 - Completed 2025-11-13)
-   **Problem**: Infrastructure tests violated Vision Principle #1 (Physics-Agnostic Core) by hardcoding production module names (simple_cooling, simple_sfr). Archiving modules required updating core test code, breaking zero-touch modularity.
-   **Solution**: Created `test_fixture` module - a minimal, stable test module for infrastructure testing only. Infrastructure tests now use test_fixture, making them physics-agnostic.
-   **Impact**:
    - **Architectural Integrity**: Eliminated 15 violations across 9 test methods in `tests/unit/test_module_configuration.c` and `tests/integration/test_module_pipeline.py`
    - **Zero-Touch Modularity**: Production modules can now be archived with ZERO infrastructure test changes
    - **Validation**: Core infrastructure validates Vision Principle #1 (physics-agnostic) - tests decouple from production physics
-   **Deliverables**:
    - `src/modules/_system/test_fixture/` - Minimal test module (~150 lines)
    - Updated infrastructure tests use test_fixture exclusively
    - `docs/developer/testing.md` - Infrastructure Testing Conventions section
    - `src/modules/_system/test_fixture/README.md` - Test fixture documentation
-   **Lesson Learned**: Early architectural violations caught early prevent downstream pain. Test fixtures are industry standard for separating infrastructure tests from production code.

#### ✅ **7. Generated Files Reorganization** (Phase 4.2.7 - Completed 2025-11-13)
-   **Problem**: Generated files were scattered across the codebase in inconsistent locations, mixing with source code and making it unclear which files were auto-generated vs hand-written. This obscured the metadata-driven architecture principle.
-   **Solution**: Comprehensive reorganization consolidating all 16 generated files into consistent `generated/` subdirectories with self-documenting README files.
-   **Changes**:
    - `src/modules/_system/generated/` - Module registration code (was: src/modules/generated/)
    - `output/mimic-plot/generated/` - Python dtypes (was: output/mimic-plot/)
    - `build/generated/` - Build artifacts including git_version.h (was: mixed locations)
    - `tests/generated/` - Test metadata (was: tests/unit/)
    - `docs/generated/` - Generated documentation (was: docs/user/)
    - Archived unused `src/generated/` directory
-   **Impact**:
    - **Architectural Clarity**: Clear visual separation of generated vs source code reinforces Vision Principle #3 (Metadata-Driven Architecture)
    - **Self-Documenting**: 5 README files explain generation process in each generated/ subdirectory
    - **Build Hygiene**: Build artifacts properly isolated in build/ directory
    - **Maintainability**: Future developers immediately understand file organization
    - **Documentation Accuracy**: Fixed existing bugs in property-metadata-schema.md, updated 10+ documentation files
-   **Technical Changes**:
    - Added `-Ibuild/generated` to compiler flags for git_version.h
    - Updated all generator scripts with new output paths
    - Fixed module_sources.mk to reference new module_init.c path
    - Created symlink for module-reference.md user discoverability
    - Updated Python imports: `from generated.dtype import ...`
-   **Validation**: ✅ All tests passing (unit: 6/6, integration: all, scientific: all), clean build, executable runs successfully
-   **Deliverables**:
    - 32 files modified: 8 generators, 7 property files, 6 docs, 4 test files, 3 plotting files, 2 READMEs, Makefile, .gitignore
    - 5 new README files in generated/ subdirectories
    - Comprehensive commit documenting all changes
-   **Architectural Alignment**: Strongly enhances Vision Principles #3 (Metadata-Driven) and #4 (Single Source of Truth)
-   **Lesson Learned**: Consistent structure is as important as functionality. Clear separation of concerns prevents confusion and accidental editing of generated files.

#### ✅ **8. Metadata System Cleanup** (Phase 4.2.8 - Completed 2025-11-19)
-   **Problem**: Redundant auto-generated module documentation violated Vision Principle #4 (Single Source of Truth) by duplicating information already maintained in hand-written module README.md files. Additionally, several metadata fields served only documentation purposes without runtime function.
-   **Solution**: Removed auto-generated documentation system and streamlined metadata to eliminate redundancy while preserving essential system metadata.
-   **Changes**:
    - **Documentation Generation Removed**: Deleted `generate_module_reference_md()` function from `scripts/generate_module_registry.py` (~140 lines removed)
    - **Module Metadata Streamlined**: Removed 3 fields from all `module_info.yaml` files (10 modules updated):
      - `category` - Used only for organizing auto-generated docs
      - `docs.user_guide_section` - Auto-doc section title
      - `references` - Better maintained in module README.md files
    - **Property Metadata Streamlined**: Removed `created_by` field from `metadata/galaxy_properties.yaml` (23 properties updated) - redundant with `provides` field in module metadata
    - **Validation Updated**: Removed validation for eliminated fields from `scripts/validate_modules.py`
    - **Template Updated**: Updated `src/modules/_system/template/module_info.yaml.template` to reflect simplified schema
    - **Documentation Updated**: 5 documentation files updated to remove references to eliminated fields (module-metadata-schema.md, property-metadata-schema.md, module-developer-guide.md, testing.md, and this roadmap)
-   **Impact**:
    - **Architectural Integrity**: Strongly reinforces Vision Principle #4 (Single Source of Truth) - module documentation now lives exclusively in module README.md files
    - **Reduced Complexity**: Removed ~200 lines of code from generators, validators, and metadata files
    - **Clearer Boundaries**: Clear separation between system metadata (for build/runtime) and user documentation (in README files)
    - **Easier Maintenance**: No duplication between auto-generated docs and hand-written README files
    - **Metadata Simplification**: Module metadata reduced from 18 to 15 fields; property metadata reduced from 12 to 11 fields
-   **Files Modified**: 33 total
    - 2 Python scripts: `generate_module_registry.py`, `validate_modules.py`
    - 11 metadata files: `galaxy_properties.yaml`, 10x `module_info.yaml`
    - 1 template: `module_info.yaml.template`
    - 5 documentation files: `module-metadata-schema.md` (-51 lines), `property-metadata-schema.md` (-21 lines), `module-developer-guide.md` (-3 lines), `testing.md` (-2 lines), `roadmap_v4.md` (this file)
    - 1 file deleted: `docs/generated/module-reference.md`
    - 1 README updated: `docs/generated/README.md`
-   **Validation**: ✅ All changes implemented, no remaining references to removed fields
-   **Lesson Learned**: Eliminate duplication early. Auto-generated documentation that duplicates hand-maintained content creates synchronization burden without benefit. Keep metadata focused on system needs; user-facing documentation belongs in dedicated README files.

#### ✅ **9. SAGE Physics Modules** (Phase 4.2 - All Merged, Code Quality Complete)
-   **Goal**: Systematically port production-quality SAGE physics modules into Mimic's modular architecture.
-   **Status**: All 6 SAGE module branches merged to main (November 18, 2025). Code quality improvements complete. Integration testing in progress.

-   **Code Quality Improvements** (Completed November 18, 2025):
    - ✅ **safe_div() utility integration**: All 4 new modules updated to use centralized safe division with epsilon-based zero detection
      - sage_starformation_feedback: 4 divisions updated
      - sage_reincorporation: 2 divisions updated
      - sage_mergers: 7 divisions updated, removed duplicate safe_div() function
      - sage_disk_instability: 5 divisions updated
    - ✅ **Compiler warnings resolved**: Fixed unused parameter warnings (ctx), documented expected warnings in sage_mergers
    - ✅ **Unit test framework updates**: All module unit tests updated to current framework API, all passing

-   **Fully Integrated Modules** (2/6 - Production Ready):
    1. **sage_infall** - Gas accretion from dark matter halos
        - Properties: `HotGas`, `MetalsHotGas`, `EjectedMass`, `MetalsEjectedMass`, `ICS`, `MetalsICS`
        - Features: Reionization suppression, ram pressure stripping, metallicity tracking
        - Testing: ✅ 5 unit tests + 7 integration tests, all passing
        - Status: ✅ Production ready
    2. **sage_cooling** - Gas cooling + AGN feedback
        - Properties: `ColdGas`, `MetalsColdGas`, `BlackHoleMass`, `Cooling`, `Heating`, `r_heat`
        - Features: Sutherland & Dopita cooling tables, 2D interpolation (T, Z), 3 AGN accretion modes
        - Testing: ✅ 9 unit tests + 7 integration tests, all passing
        - Status: ✅ Production ready

-   **Merged Modules** (4/6 - Ready for Integration):
    3. **sage_starformation_feedback** - Star formation & supernova feedback
        - Properties: `MetalsStellarMass`, `DiskScaleRadius`, `OutflowRate`
        - Features: Kennicutt-Schmidt SF law, SN feedback, metal enrichment
        - Testing: ✅ 6 unit tests + 7 integration tests, all passing
        - Status: ⏸️ Ready for integration testing
    4. **sage_reincorporation** - Gas return from ejected reservoir
        - Properties: None (modifies existing)
        - Features: Velocity-dependent gas reincorporation
        - Testing: ✅ 6 unit tests + 7 integration tests, all passing
        - Status: ⏸️ Ready for integration testing
    5. **sage_mergers** - Galaxy merger physics
        - Properties: `BulgeMass`, `MetalsBulgeMass`, `QuasarModeBHaccretionMass`, `TimeOfLastMajorMerger`, `TimeOfLastMinorMerger`
        - Features: Dynamical friction, starbursts, BH growth, quasar feedback, morphology
        - Testing: ⚠️ 3 expected warnings (functions blocked by Phase 4.3 merger triggering)
        - Status: ⚠️ **BLOCKED** - Requires core merger triggering implementation
    6. **sage_disk_instability** - Disk instability & bulge formation
        - Properties: Uses `BulgeMass`, `MetalsBulgeMass` from sage_mergers
        - Features: Stability criterion (Mo, Mao & White 1998), stellar mass transfer
        - Testing: ✅ 6 unit tests, all passing (physics tests deferred to integration)
        - Status: ⏸️ Partial - Gas processing deferred (needs sage_mergers functions)

-   **Integration Challenges Identified**:
    - **Merger Triggering**: Core doesn't detect when satellites should merge (CRITICAL BLOCKER)
    - **Module Execution Pipeline**: Need systematic validation of module ordering and property flow
    - **Shared Functions**: Need architecture for functions used by multiple modules
    - **Property Ownership**: Module metadata needs verification (created_by vs modifies)
    - **Cross-Module Testing**: Need comprehensive pipeline validation tests

-   **Architecture Validations**:
    - ✅ Metadata-driven registration works for all 8 modules
    - ✅ Property conflicts resolved during merge (MetalsStellarMass, DiskScaleRadius, OutflowRate)
    - ✅ Duplicate properties removed (BulgeMass, MetalsBulgeMass)
    - ✅ Build system handles 6 SAGE modules + 2 system modules successfully
    - ✅ All modules compile (with expected unused function warnings in sage_mergers)

---

### Quick Start Guide for Phase 4 Developers

**Starting Phase 4?** Here's your roadmap:

1. **Understand the Foundation**:
   - Read `docs/architecture/vision.md` - understand Mimic's architectural principles
   - Review PoC modules: `src/modules/simple_cooling/` and `src/modules/simple_sfr/`
   - Study `docs/user/module-configuration.md` - see how modules are configured

2. **Set Up Development Environment**:
   - Ensure you can build: `make` and `make tests`
   - Clone SAGE reference: `git clone https://github.com/darrencroton/sage`
   - Familiarize yourself with SAGE's `code/` directory structure

3. **Begin Phase 4**:
   - Start with Section 4.1: Create developer infrastructure (template, guide, log)
   - Then proceed to Section 4.3: Implement first module using the standard workflow
   - Refer to Section 4.2 for module priority list

4. **Key Resources**:
   - Module examples: `src/modules/simple_*/`
   - Property system: `metadata/galaxy_properties.yaml`
   - Testing guide: `docs/developer/testing.md`
   - Coding standards: `docs/developer/coding-standards.md`

**Philosophy**: Take time to understand before coding. Document as you go. Capture lessons learned for future developers.

---

## The Critical Path: What's Next

This is the actionable to-do list. Work should proceed in this order.

### **Phase 4: SAGE Physics Module Implementation**

**Goal**: Systematically port production-quality physics modules from the SAGE codebase into Mimic's modular architecture, establishing the workflow and tooling for all future module development.

**Physics Source**: SAGE model at https://github.com/darrencroton/sage (the SAGE `code/` directory has been copied into the mimic `sage-code/` for convenient access).

**Philosophy**: This phase serves dual purposes:
1. Implement production physics, transforming Mimic from infrastructure to complete galaxy evolution model
2. Refine the development process through practice, capturing lessons learned for future developers

Each module implementation validates the architecture, discovers requirements, and improves documentation and tooling for subsequent work.

---

#### **4.1 Pre-Implementation: Developer Infrastructure (1-2 weeks)**

**Goal**: Create the foundation and tooling for systematic module development.

Before implementing physics modules, establish the developer experience:

**Key Deliverables**:

1. **Module Developer Guide** (`docs/developer/module-developer-guide.md`)
   - Complete walkthrough of module development lifecycle
   - Property system integration patterns
   - Parameter handling best practices
   - Testing requirements and strategies
   - Example code snippets and anti-patterns
   - References to PoC modules as examples

2. **Module Template** (`src/modules/_system/template/`)
   - Skeleton `.c` and `.h` files with complete boilerplate
   - Annotated comments explaining each section
   - Standard init/process/cleanup structure
   - Parameter reading patterns
   - Property access examples
   - Logging examples (INFO/DEBUG/ERROR)

3. **Module Implementation Log** (`docs/architecture/module-implementation-log.md`)
   - Structured template for documenting each module implementation
   - Captures: Overview → Challenges → Solutions → Recommendations → Infrastructure Needs
   - Serves as institutional memory for development patterns
   - Informs updates to developer guide and template

**Acceptance Criteria**:
- [x] Developer can copy template and have compilable module skeleton
- [x] Developer guide answers "how do I create a module?" completely
- [x] Implementation log has clear format ready for first module entry

**Status**: ✅ **COMPLETED 2025-11-12**

**Deliverables Created**:
- `docs/developer/module-developer-guide.md` - 500+ line comprehensive guide
- `src/modules/_system/template/` - Fully annotated template with README
- `docs/architecture/module-implementation-log.md` - Structured logging framework
- `Makefile` - Updated to exclude template from build

**Verification**: All tests passing (unit: 6/6, integration: 11/11, scientific: all passed)

**Notes**: This infrastructure will be refined throughout Phase 4 as real implementation reveals gaps and needs.

---

#### **4.2 SAGE Physics Modules: Implementation Plan**

**Source Repository**: https://github.com/darrencroton/sage

The following physics modules from SAGE's `code/` directory has been ported to Mimic in  `sage-code/` for convenient access:

| Priority | SAGE Source File | Mimic Module Name | Physics | Est. Time | Status |
|----------|-----------------|-------------------|---------|-----------|--------|
| 1 | `model_infall.c` | `sage_infall` | Gas accretion from halos | 2-3 weeks | ✅ **COMPLETE** (Nov 2025) |
| 2 | `model_cooling_heating.c` | `sage_cooling` | Gas cooling + AGN heating | 2-3 weeks | ✅ **COMPLETE** (Nov 2025) |
| 3 | `model_starformation_and_feedback.c` | `sage_starformation_feedback` | SF + SN/AGN feedback | 3-4 weeks | **NEXT** |
| 4 | `model_reincorporation.c` | `sage_reincorporation` | Gas return to galaxies | 1-2 weeks | Not Started |
| 5 | `model_mergers.c` | `sage_mergers` | Galaxy merger physics | 2-3 weeks | Not Started |
| 6 | `model_disk_instability.c` | `sage_disk_instability` | Disk instability/bulges | 2-3 weeks | Not Started |

**Completed Modules (2/6)**:
- ✅ **sage_infall**: Full implementation with reionization suppression, stripping, metallicity tracking. 5 unit tests + 7 integration tests passing.
- ✅ **sage_cooling**: Full cooling physics + 3 AGN modes (empirical, Bondi-Hoyle, cold cloud), Sutherland & Dopita cooling tables, black hole growth. 9 unit tests + 7 integration tests passing.

**Priority Rationale**:
- Modules 1-2: Foundation gas processes (infall, cooling) - COMPLETE
- Module 3: Star formation & feedback - central to galaxy formation physics
- Module 4: Reincorporation - returns ejected gas (depends on SF&F)
- Modules 5-6: Mergers and disk instability - complex but less interdependent

**Total Estimated Time**: 6-12 months for complete SAGE physics port (2/6 modules complete, ~3-4 months elapsed)

---

#### **4.3 Standard Module Implementation Workflow**

This workflow applies to each module. Refine based on lessons learned.

##### **Step 1: Physics Analysis & Design (1-3 days)**

**Goal**: Understand the SAGE physics implementation thoroughly before coding.

**Tasks**:
- Read and annotate the SAGE source code (`sage-code/model_*.c`)
- Document physics equations, assumptions, and algorithms
- Identify required galaxy properties (inputs and outputs)
- Map dependencies on other modules
- Note required external data (cooling tables, yields, etc.)
- List all parameters and their default values
- Review relevant SAGE documentation/papers

**Deliverables**:
- Physics specification (can be section in implementation log)
- List of new properties needed
- List of module parameters
- Dependency diagram (if complex)

##### **Step 2: Property & Parameter Design (1 day)**

**Goal**: Define the data contract for this module.

**Tasks**:
- Add new properties to `metadata/galaxy_properties.yaml`
- Define property types, units, descriptions
- Run `make generate` to create property accessors
- Verify generated code compiles
- Define module parameters (naming: `ModuleName_ParameterName`)
- Document parameter ranges and defaults

**Deliverables**:
- Updated `galaxy_properties.yaml`
- Generated property accessors (auto-generated)
- Parameter specification document

##### **Step 3: Module Implementation (3-7 days)**

**Goal**: Translate SAGE physics into Mimic module.

**Tasks**:
- Copy module template to `src/modules/module_name/`
- Implement `module_name_init()`:
  - Read module parameters using `module_get_*()` functions
  - Load external data files (if needed)
  - Allocate persistent data structures
  - Log module configuration
- Implement `module_name_process()`:
  - Iterate through halos
  - Access properties via `halos[i].galaxy->PropertyName`
  - Implement physics calculations (port from SAGE)
  - Update galaxy properties
  - Add DEBUG logging for validation
- Implement `module_name_cleanup()`:
  - Free allocated resources
  - Clean up external data
- Register module in `src/modules/module_init.c` (manual until Phase 5)
- Add comprehensive code documentation

**Deliverables**:
- Compilable, executable module
- Module registered and accessible via `EnabledModules`

**Reference**: See `src/modules/simple_cooling/` for working example

##### **Step 4: Visual Validation & Plotting (1-2 days, optional)**

**Goal**: Create diagnostic figures for module validation and scientific analysis.

**Tasks**:
- Discuss with developer which figures are relevant for this module
- Review `output/mimic-plot/README.md` for implementation patterns
- Add figure functions to `output/mimic-plot/mimic-plot.py`
- Test figures with module output
- Use figures for visual validation of physics

**Notes**:
- mimic-plot supports both core and physics-specific figures
- Figures automatically skip if required properties unavailable
- Physics figures co-exist with core figures
- Useful for spotting anomalies and comparing to observations

**Deliverables**:
- Module-relevant diagnostic figures (if appropriate)
- Visual validation of physics implementation

##### **Step 5: Testing & Validation (2-4 days)**

**Goal**: Verify physics correctness and integration, where possible.

**Test Tiers**:

1. **Unit Tests** (`tests/unit/`)
   - Test pure physics calculations in isolation
   - Example: cooling rate interpolation matches known values
   - Example: parameter parsing handles edge cases
   - Fast, deterministic tests

2. **Integration Tests** (`tests/integration/`)
   - Full pipeline with module enabled
   - Verify properties appear in output
   - Check module execution order respected
   - Memory leak checking

3. **Scientific Validation** (`tests/scientific/`)
   - Compare outputs to SAGE reference results
   - Mass conservation tests (where applicable)
   - Reproduce published results (e.g., Croton+2006)
   - Statistical tests on distributions

**Tasks**:
- Write tests at all three tiers, as appropriate
- Add tests to CI pipeline (update `Makefile` test targets)
- Run full test suite and verify passing
- Profile performance on full Millennium simulation
- Check memory usage with `print_allocated_by_category()`

**Deliverables**:
- Comprehensive passing test suite
- Performance baseline measurements

##### **Step 6: Documentation (1-2 days)**

**Goal**: Complete documentation for users and future developers.

**Tasks**:
- **Physics Documentation** (module's `README.md` in module directory):
  - Physics equations and assumptions
  - References to papers and SAGE implementation
  - Parameter descriptions and typical values
  - Known limitations or simplifications

- **User Documentation**: Update `docs/user/module-configuration.md`:
  - Add module to available modules list
  - Document all parameters with examples
  - Show typical parameter file configuration

- **Implementation Log**: Add section to `docs/architecture/module-implementation-log.md`:
  - Implementation overview and timeline
  - Challenges encountered and solutions
  - Lessons learned (what went well, what didn't)
  - Recommended improvements for next module
  - Infrastructure gaps discovered

- **Developer Guide Updates**:
  - Add new patterns discovered
  - Update examples if better patterns emerged
  - Note any anti-patterns to avoid

**Deliverables**:
- Complete module documentation
- Updated user and developer guides
- Implementation log entry

##### **Step 7: Review & Refinement (1-2 days)**

**Goal**: Ensure production quality and identify improvements.

**Tasks**:
- Code review against `docs/developer/coding-standards.md`
- Performance profiling on full-scale simulation
- Memory leak verification
- Review test coverage and add missing tests
- Identify infrastructure enhancements needed:
  - Common utilities that could be abstracted
  - Missing capabilities in `ModuleContext`
  - Testing framework gaps
  - Documentation improvements
- File issues for deferred improvements
- Update module status table in this roadmap

**Deliverables**:
- Production-ready, reviewed module
- List of infrastructure improvements (for periodic review)
- Updated roadmap status tracking

**Total Time Per Module**: 2-4 weeks depending on complexity

---

#### **4.4 Infrastructure Evolution Strategy**

**Philosophy**: Let real implementation needs drive infrastructure evolution, not speculation.

**Process**:

1. **Continuous Capture**: During each module implementation, document in implementation log:
   - Missing infrastructure capabilities
   - Repeated patterns that could be abstracted
   - Performance bottlenecks
   - Testing framework gaps
   - Documentation weaknesses

2. **Periodic Review**: After every 2-3 modules, pause for "lessons learned review":
   - Review implementation log entries
   - Identify common patterns across modules
   - Evaluate infrastructure improvement proposals
   - Prioritize enhancements with clear ROI

3. **Selective Refactoring**:
   - Extract common utilities when ≥3 modules need same functionality
   - Enhance `ModuleContext` when multiple modules require same data
   - Improve testing framework when gaps affect multiple modules
   - Update templates and guides based on refined patterns

**Example Infrastructure Enhancements** (to be evaluated as needs arise):
- Shared interpolation utilities (if multiple modules need table lookups)
- Integration/ODE solvers (if multiple modules solve equations)
- Cosmology utilities (redshift conversions, times, etc.)
- Shared physics constants and unit conversions
- Testing helpers for common validation patterns

**This ensures**: Infrastructure evolves based on demonstrated need, avoiding premature optimization and over-engineering.

---

#### **4.5 Testing Framework Enhancements (Continuous)**

**Context**: Following comprehensive code review of testing framework (2025-11-13), several refinements have been implemented and others deferred to appropriate phases.

**Philosophy**: Let demonstrated needs during module implementation drive testing improvements. Avoid premature optimization of test infrastructure.

##### **Completed (Phase 4.2 - November 2025)**

**Centralized Test Harness** (`tests/framework/harness.py`):
- Eliminated code duplication across integration tests
- Centralized utilities: `run_mimic()`, `create_test_param_file()`, `read_param_file()`, `ensure_output_dirs()`, `check_no_memory_leaks()`
- Documented in `docs/developer/testing.md` with usage examples
- **Impact**: Reduced test boilerplate, consistent patterns for test development

**Baseline Management** (`scripts/regenerate_baseline.sh`):
- Automated HDF5 baseline regeneration with validation
- Documented when/how to regenerate baseline
- Enforces physics-free mode requirement
- Provides git commit instructions for auditability
- **Impact**: Prevents baseline corruption, ensures reproducibility

##### **Scheduled (Phase 4.2 - During sage_cooling)**

**Cross-Module Physics Validation** (Task 23 in next-task.md):
- Test file: `tests/integration/test_infall_cooling_pipeline.py`
- Validates sage_infall → sage_cooling property flow
- Checks: execution order, mass conservation, metallicity tracking
- **Timeline**: After sage_cooling Tasks 11-22 complete
- **Impact**: Validates scientific correctness of module interactions

##### **Deferred to Phase 4.3 (After 2-3 More SAGE Modules)**

**Module Test Automation Script** (`scripts/create_module_tests.sh`):
- **Why defer**: Need 2-3 more modules to identify stable patterns
- **Benefit**: Auto-generate test skeletons for new modules
- **Timing**: After sage_reincorporation and sage_starformation_feedback
- **Estimated effort**: 1 day
- **ROI**: Streamlines module development, reduces copy-paste errors

**C Test Runner Improvements** (optional refinement):
- **Why defer**: Current `run_tests.sh` is functional, no demonstrated pain points
- **Potential improvements**: Better error messages, more defensive parsing
- **Timing**: Opportunistic - if pain points emerge
- **Estimated effort**: 1 day
- **Approach**: Incremental improvements, not full rewrite to Python

##### **Deferred to Phase 5+ (Post-SAGE Implementation)**

**Performance Regression Testing**:
- **Why defer**: Performance optimization premature before all physics implemented
- **Implementation**: `make test-performance` target with baseline tracking
- **Benefit**: Automated detection of performance regressions (>10-15% slower)
- **Timing**: After all 6 SAGE modules complete and performance matters
- **Estimated effort**: 2-3 days

**HDF5 Input Tree Testing**:
- **Why defer**: Low impact - most users use binary format, no reported issues
- **Implementation**: Integration test for `genesis_lhalo_hdf5` reader
- **Benefit**: Edge case coverage for HDF5 input format
- **Timing**: Opportunistic or when user reports issues
- **Estimated effort**: 1-2 days

##### **Recommendations Not Adopted**

**C Framework Setup/Teardown Functions**:
- **Reasoning**: Current explicit pattern is clear and idiomatic for C
- **Boilerplate**: Minimal (~2-3 lines per test)
- **Decision**: Only reconsider if boilerplate grows significantly (>10 lines/test)

**Kitchen Sink Integration Test**:
- **Reasoning**: Conflicts with modular testing philosophy
- **Alternative**: Module-specific tests + integration tests provide better coverage
- **Maintenance burden**: Would break whenever any module changes
- **Decision**: Rejected - current approach is preferable

##### **Process for Evaluating Future Enhancements**

1. **Document needs** in `module-implementation-log.md` during Phase 4
2. **Periodic reviews** after every 2-3 modules
3. **Prioritize** based on demonstrated ROI, not speculation
4. **Implement** when ≥3 modules need same capability or pain point affects multiple modules

**This approach ensures**: Testing framework evolves based on real needs, not premature optimization.

---

#### **4.6 Phase 4.3: Module Integration & Validation** (Current Phase - November 2025)

**Status**: All 6 SAGE modules merged to main. Now entering systematic integration and validation phase.

**Goal**: Transform merged module code into a fully integrated, validated SAGE physics pipeline.

**Current Situation**:
- ✅ All module code merged and compiling
- ✅ 2/6 modules production-ready (sage_infall, sage_cooling)
- ⏸️ 4/6 modules require integration work
- ⚠️ Critical blocker: Core merger triggering not implemented

**Integration Approach** (To Be Designed):
The exact integration strategy is still being determined. Key decisions needed:

1. **Module Enabling Strategy**:
   - Start with minimal working pipeline (infall → cooling)
   - Add modules one at a time, validating each addition
   - Document module ordering requirements
   - Identify and resolve module interdependencies

2. **Core Infrastructure Decisions**:
   - **Merger Triggering**: How/where to implement in core (design TBD)
   - **Module Calling Pipeline**: Enhance execution flow (design TBD)
   - **Shared Functions**: Architecture for cross-module function calls (design TBD)
   - **Property Lifecycle**: Verify created_by vs modifies semantics

3. **Validation Strategy**:
   - **Property Organization**: Review and verify all galaxy property definitions
   - **Module Dependencies**: Map and validate execution order requirements
   - **Mass Conservation**: Cross-module conservation validation
   - **Performance**: Profile full pipeline on Millennium data

**Known Challenges**:
- sage_mergers requires core merger triggering (cannot test until implemented)
- sage_disk_instability needs sage_mergers functions (architectural decision needed)
- Module metadata may need corrections (created_by values)
- Cross-module testing infrastructure needs design

**Open Questions** (To Be Resolved):
- How to handle functions needed by multiple modules?
- Where should merger detection logic live in core?
- How to enhance module calling pipeline for better diagnostics?
- What validation tests are needed for full pipeline?
- How to organize galaxy properties for clarity?

**Next Immediate Steps** (To Be Defined in next-task.md):
1. Design systematic module integration plan
2. Start with minimal pipeline validation
3. Identify architectural decisions needed
4. Create integration task list

**Timeline**: TBD (depends on architectural decisions and merger triggering complexity)

---

#### **4.7 Success Criteria for Phase 4**

Phase 4 is complete when:

- [ ] Developer infrastructure complete (template, guide, log)
- [ ] All 6 SAGE physics modules successfully ported
- [ ] Each module passes scientific validation against SAGE results
- [ ] Module developer guide is comprehensive and battle-tested
- [ ] Implementation log captures clear patterns and lessons
- [ ] Infrastructure improvements identified and documented
- [ ] Test coverage adequate for scientific confidence
- [ ] Performance acceptable on full-scale simulations
- [ ] Documentation complete for users and developers

**At this point**: Mimic is a complete, validated galaxy evolution model with all SAGE physics in modular form, ready for scientific use and future extension.

### **Phase 5: Implement Build-Time Module Selection (1 week)**

**Goal**: Allow developers to select which modules are compiled into the final executable, reducing binary size and complexity for specific deployments.

**Why this is next**: This is a key optimization for performance and deployment. It allows for creating lean, specialized versions of Mimic (e.g., for a specific scientific model) and formalizes the module discovery process, removing the last piece of manual registration code.

**Key Deliverables**:
1.  **Module Metadata File**: Each module directory will contain a `module_info.yaml` file describing itself.
2.  **Code Generation Script**: A script (`scripts/generate_module_table.py`) that scans the `src/modules` directory and generates the C code to register all selected modules. This replaces the manual registration calls in `main.c`.
3.  **Makefile Integration**:
    -   Update the `Makefile` to use the code generation script.
    -   Add a `PROFILE` variable (e.g., `make PROFILE=minimal`, `make PROFILE=all`) to build predefined sets of modules.
    -   Add a `MODULES` variable for custom one-off builds (e.g., `make MODULES="cooling_SD93 stellar_mass"`).

**Acceptance Criteria**:
-   [ ] Running `make` automatically generates the module registration code.
-   [ ] `make PROFILE=minimal` produces a smaller binary that only contains the PoC modules.
-   [ ] `make MODULES="cooling_SD93"` produces a binary that *cannot* run the `stellar_mass` module, even if it's in the parameter file (and gives a clean error).
-   [ ] The manual module registration calls in `main.c` are removed.

---

### Deferred & Future Work

These items are intentionally deferred until demonstrated need arises during Phase 4 module implementations. They will be evaluated during periodic "lessons learned reviews" (Section 4.4).

**Infrastructure Enhancements** (evaluated as needs arise):
-   **Module Interface Enrichment**: The PoC proved the minimal `ModuleContext` is sufficient for simple physics. Additional context (shared random number generators, integrators, cosmology utilities) will be added only when multiple modules demonstrate concrete need.
-   **Memory Management Refactor**: The existing memory allocator is robust and performs well. A refactor is premature optimization and will only be reconsidered if profiling reveals bottlenecks with many properties (>10-20).
-   **Shared Physics Utilities**: Common functionality (interpolation, integration, unit conversions) will be extracted into shared utilities only when ≥3 modules need the same capability.
-   **Testing Framework Extensions**: Additional testing infrastructure (fixtures, mocks, specialized validators) will be added as gaps are discovered during module testing.

**Scientific Extensions** (post-Phase 4):
-   **Alternative Physics Models**: After SAGE physics port, framework enables easy addition of alternative implementations (different cooling models, SF recipes, etc.)
-   **Parameter Optimization**: Infrastructure for automated parameter fitting and optimization
-   **Advanced Validation**: Statistical comparison tools, visualization pipelines for model-data comparison

**Approach**: Document needs in `module-implementation-log.md` during Phase 4. Prioritize and implement based on demonstrated ROI after periodic reviews.

---

### Reference: Key Architectural Principles & Patterns

This section contains essential context for the developer.

#### **Core Vision Principles**
1.  **Physics-Agnostic Core**: The core infrastructure has zero knowledge of specific physics.
2.  **Runtime Modularity**: Physics can be reconfigured without recompiling.
3.  **Metadata-Driven**: System structure is defined in YAML, not hardcoded.
4.  **Single Source of Truth**: `GalaxyData` is the authoritative state, managed via the property system.
5.  **Validated Patterns**: All work should follow the patterns proven effective in the PoCs.

#### **Validated Development Workflow**
This is the established pattern for adding new physics.

1.  **Define Properties (2 min)**: Add property definitions to `metadata/galaxy_properties.yaml`.
2.  **Generate Code (5 sec)**: Run `make generate`.
3.  **Implement Module Logic**: Create a new directory in `src/modules/` and implement the `init`, `process`, and `cleanup` functions. Access galaxy data via the auto-generated `halos[i].galaxy->PropertyName` accessors.
4.  **Write Tests**: Add unit tests for pure logic and scientific validation tests in Python to verify physical outputs.
5.  **Configure & Run**: Add the new module to the `EnabledModules` list in your `.par` file and run the simulation.

---

## Summary: Vision for Phase 4 Completion

When Phase 4 is complete, Mimic will have achieved its core mission:

**Scientific Achievement**:
- Complete, validated galaxy evolution model with all SAGE physics
- Scientifically verified against published results
- Ready for production research use

**Architectural Achievement**:
- Proven physics-agnostic core with 6+ production physics modules
- Runtime-configurable physics enables flexible scientific exploration
- Metadata-driven property system demonstrated at scale

**Developer Achievement**:
- Battle-tested module development workflow
- Comprehensive documentation capturing institutional knowledge
- Clear patterns for future module developers
- Infrastructure that evolves based on demonstrated needs

**Key Success Metrics**:
- Scientific validation: Mimic results match SAGE reference within acceptable tolerances
- Code quality: All modules pass comprehensive testing at unit, integration, and scientific levels
- Performance: Full-scale simulations complete in reasonable time with bounded memory
- Maintainability: Clear, documented code following professional standards
- Extensibility: New developers can implement additional modules using established patterns

**The Path Forward**:
Phase 4 is not just implementation—it's discovery and refinement. Each module teaches us something new. By capturing lessons learned and evolving our infrastructure based on real needs, we build not just a galaxy evolution code, but a sustainable framework for ongoing scientific development.

**Remember**: Take time to do it right. Professional code quality, thorough testing, and comprehensive documentation are not overhead—they are the foundation for long-term scientific productivity.