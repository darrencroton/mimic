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

1.  **Phase 4: Implement First Realistic Science Module** - Validate the entire system with a production-grade cooling module.
2.  **Phase 5: Implement Build-Time Module Selection** - Optimize for deployment by allowing modules to be compiled in or out.

**Revised Timeline**: **4-6 weeks** to complete the core transformation.

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

---

## The Critical Path: What's Next

This is the actionable to-do list. Work should proceed in this order.

### **Phase 4: Implement First Realistic Science Module (2-3 weeks)**

**Goal**: Validate the entire modular architecture by implementing a production-quality cooling module based on published physics (e.g., Sutherland & Dopita 1993).

**Why this is next**: The PoC modules were simple placeholders. This phase proves the system can handle real-world complexity, including managing external data (cooling tables) and producing scientifically verifiable results. It will also serve as the template for all future modules.

**Key Deliverables**:
1.  **New Properties**: Add `GasMetallicity`, `Temperature`, and `CoolingRate` to the property metadata system.
2.  **Cooling Module (`cooling_SD93`)**:
    -   Implement the module lifecycle (`init`, `process`, `cleanup`).
    -   `init()`: Load cooling rate tables (temperature, metallicity, redshift) from a data file.
    -   `process()`: For each halo, calculate the virial temperature, interpolate the cooling rate from the tables, and compute the mass of cooled gas.
    -   Update the `ColdGas` property based on the cooling calculation.
3.  **Scientific Validation Tests**:
    -   A unit test verifying the cooling rate interpolation matches known values from the source paper at specific temperatures.
    -   An integration test comparing the final gas fractions against published results (e.g., from Croton+2006).
    -   A mass conservation test ensuring no baryonic mass is created or lost.
4.  **Documentation**:
    -   `docs/developer/module-developer-guide.md`: A comprehensive guide for future developers, using this module as the primary example.
    -   `docs/physics/cooling-module.md`: A document explaining the physics, assumptions, and data sources for the module.

**Acceptance Criteria**:
-   [ ] The module can be enabled and disabled via the parameter file.
-   [ ] The new properties (`Temperature`, `CoolingRate`, etc.) appear correctly in the HDF5 output.
-   [ ] All scientific validation tests pass in CI.
-   [ ] The module runs without memory leaks on the standard Millennium test case.
-   [ ] The developer guide is clear enough for a new team member to build another module.

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

These items were considered but have been intentionally deferred based on PoC findings. They are not on the critical path and should only be addressed when a clear need arises.

-   **Module Interface Enrichment**: The PoC proved the minimal `ModuleContext` is sufficient for realistic physics. We will add new context (e.g., shared random number generators, integrators) only when a future module demonstrates a concrete need.
-   **Memory Management Refactor**: The PoC showed the existing memory allocator is robust and performs well with the addition of new galaxy properties. A refactor is premature optimization and will only be reconsidered if profiling reveals a bottleneck with >10-20 properties.

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