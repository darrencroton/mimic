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
    - **Module Template** (`src/modules/_template/`): Annotated boilerplate code for rapid module creation
    - **Module Implementation Log** (`docs/architecture/module-implementation-log.md`): Structured framework for capturing lessons learned
-   **Impact**: Developers can create new modules following proven patterns with clear documentation. Template provides working boilerplate code. Implementation log ensures institutional knowledge is preserved across module implementations.

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
   - Property system: `metadata/properties/galaxy_properties.yaml`
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

2. **Module Template** (`src/modules/_template/`)
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
- `src/modules/_template/` - Fully annotated template with README
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
| 1 | `model_cooling_heating.c` | `cooling_heating` | Gas cooling/heating processes | 2-3 weeks | Not Started |
| 2 | `model_infall.c` | `infall` | Gas accretion from halos | 2-3 weeks | Not Started |
| 3 | `model_reincorporation.c` | `reincorporation` | Gas return to galaxies | 1-2 weeks | Not Started |
| 4 | `model_starformation_and_feedback.c` | `starformation_feedback` | SF + SN/AGN feedback | 3-4 weeks | Not Started |
| 5 | `model_mergers.c` | `mergers` | Galaxy merger physics | 2-3 weeks | Not Started |
| 6 | `model_disk_instability.c` | `disk_instability` | Disk instability/bulges | 2-3 weeks | Not Started |

**Priority Rationale**:
- Start with gas processes (cooling, infall, reincorporation) as foundation
- Star formation and feedback builds on gas physics
- Mergers and disk instability are more complex but less interdependent

**Total Estimated Time**: 6-12 months for complete SAGE physics port

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
- Add new properties to `metadata/properties/galaxy_properties.yaml`
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

##### **Step 4: Testing & Validation (2-4 days)**

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

##### **Step 5: Documentation (1-2 days)**

**Goal**: Complete documentation for users and future developers.

**Tasks**:
- **Physics Documentation** (`docs/physics/module-name.md`):
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

##### **Step 6: Review & Refinement (1-2 days)**

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

#### **4.5 Success Criteria for Phase 4**

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

1.  **Define Properties (2 min)**: Add property definitions to `metadata/properties/galaxy_properties.yaml`.
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