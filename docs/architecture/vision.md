# Mimic Architectural Vision

**Purpose**: Define the architectural principles and design philosophy for Mimic, a physics-agnostic galaxy evolution framework.

---

## Vision Statement

Mimic is a **physics-agnostic core with runtime-configurable physics modules**, enabling scientific flexibility, maintainability, and extensibility.

This architecture enables researchers to easily experiment with different physics combinations, developers to work independently on core infrastructure and physics modules, and the system to evolve gracefully as new scientific understanding emerges.

---

## 8 Core Architectural Principles

These principles guide all design decisions and implementation choices in Mimic:

### 1. Physics-Agnostic Core Infrastructure

**Principle**: The core Mimic infrastructure has zero knowledge of specific physics implementations.

**Requirements**:
- Core systems (memory management, I/O, tree processing, configuration) operate independently of physics.
- Physics modules interact with the core only through well-defined interfaces.
- The core can execute successfully with no physics modules loaded (physics-free mode).
- Physics modules are pure add-ons that extend core functionality.

**Benefits**: Enables independent development of physics and infrastructure, simplifies testing, allows for physics module hot-swapping, and reduces complexity in core systems.

**In Practice**: The core evolution loop does not contain `#include "physics_module.h"` or direct calls to specific physics functions. Instead, it iterates through registered physics modules and calls a generic `execute()` function on each.

### 2. Runtime Modularity

**Principle**: Physics module combinations are configurable at runtime without recompilation.

**Requirements**:
- Module selection is done via configuration files, not compile-time flags.
- Modules self-register and declare their capabilities and dependencies.
- The execution pipeline adapts dynamically to the loaded module set.
- An empty pipeline (core-only execution) is a valid configuration.

**Benefits**: Provides scientific flexibility for different research questions, makes it easy to experiment with physics combinations, offers deployment flexibility, and simplifies testing of different physics scenarios.

**In Practice**: Users can switch from one physics model to another, or disable specific physics modules entirely, by changing a configuration file and re-running the executable.

### 3. Metadata-Driven Architecture

**Principle**: The system's structure is defined by metadata rather than hardcoded implementations.

**Requirements**:
- Galaxy properties (e.g., `StellarMass`, `ColdGas`) are defined in metadata files (YAML), not hardcoded in C structs.
- Physics modules are defined in metadata with automatic registration generation.
- Parameters are defined in metadata with automatic validation generation.
- The build system generates type-safe C code (headers, accessors, registration) from metadata.
- Output formats adapt automatically to properties defined in metadata.

**Benefits**: Reduces code duplication, eliminates manual synchronization between different representations, enables build-time optimization, and simplifies maintenance by creating a single source of truth.

**In Practice**: Adding a new galaxy property requires editing a single YAML file and running `make generate`. Adding a new physics module requires creating `module_info.yaml` and running `make generate`. All C code, documentation, and build configuration is automatically generated. The `make generate` command is smart - it only regenerates files when their source metadata has changed.

### 4. Single Source of Truth

**Principle**: Galaxy data has one authoritative representation with consistent access patterns.

**Requirements**:
- No dual property systems or synchronization code.
- All access to galaxy data goes through a unified property system.
- The property system is type-safe and allows for compile-time optimization.
- Property lifecycle (creation, modification, destruction) is managed consistently.

**Benefits**: Eliminates synchronization bugs, simplifies debugging by having a single data path, reduces memory overhead, and improves performance through unified access patterns.

### 5. Unified Processing Model

**Principle**: Mimic has one consistent, well-understood method for processing merger trees.

**Requirements**:
- A single tree traversal algorithm handles all scientific requirements.
- Consistent galaxy inheritance and property calculation methods.
- Robust orphan galaxy handling that prevents data loss.
- Clear separation between tree traversal logic and physics calculations.

**Benefits**: Eliminates complexity from maintaining multiple processing modes, simplifies validation, reduces bug surface area, and makes the system easier to understand and modify.

### 6. Memory Efficiency and Safety

**Principle**: Memory usage is bounded, predictable, and safe.

**Requirements**:
- Memory usage is bounded and does not grow with the total number of forests processed.
- Memory management for galaxy arrays and properties is automatic where possible.
- Memory is allocated on a per-forest scope with guaranteed cleanup after processing.
- Tools for memory leak detection and prevention are built-in.

**Benefits**: Allows reliable processing of large simulations, reduces debugging overhead by preventing memory-related bugs, improves performance predictability, and enables processing of datasets larger than available RAM.

**In Practice**: Memory for a merger tree (halos, galaxies, module-specific data) is allocated at the start of processing and guaranteed to be freed upon completion.

### 7. Format-Agnostic I/O

**Principle**: Mimic supports multiple input/output formats through unified interfaces.

**Requirements**:
- A common, abstract interface for all tree reading operations.
- A property-based output system that adapts to data available at runtime.
- Proper handling of cross-platform issues like endianness.
- Graceful fallback mechanisms for unsupported features in certain formats.

**Benefits**: Ensures scientific compatibility with different simulation codes and analysis tools, future-proofs against format changes, simplifies validation across formats, and eases integration with external tools.

### 8. Type Safety and Validation

**Principle**: Data access is type-safe with automatic validation.

**Requirements**:
- Type-safe property accessors (macros or functions) are generated from metadata.
- Automatic bounds checking and validation where appropriate.
- Fast failure with clear error messages upon invalid data access.

**Benefits**: Reduces runtime errors by catching problems at compile-time, improves debugging with clear messages, catches problems early, and increases confidence in scientific accuracy.

---

## Implementation Philosophy

### Metadata-Driven Development
- **Single Source of Truth**: YAML metadata prevents synchronization bugs between code representations.
- **Property Metadata** (`metadata/*.yaml`): Galaxy/halo properties auto-generate C structs, accessors, output code, and Python dtypes.
- **Module Metadata** (`src/modules/*/module_info.yaml`): Physics modules auto-generate registration code, test configuration, and documentation.
- **Code Generation**: Type-safe C code is automatically generated from metadata definitions.
- **Build Integration**: Code generation is integrated into the build system with automatic regeneration on metadata changes.

### Physics-Agnostic Core
- **Zero Physics Knowledge**: Core infrastructure has no understanding of specific physics processes.
- **Interface-Based Interaction**: Physics modules interact with core only through well-defined interfaces.
- **Independent Development**: Core infrastructure and physics modules are developed independently.

### Type Safety First
- **Compile-Time Validation**: Errors are caught at compile-time rather than runtime.
- **Generated Access Patterns**: Type-safe property access is generated from metadata.
- **IDE Integration**: Full autocomplete, go-to-definition, and refactoring support in modern IDEs.

### Standard Tools
- **Industry Standards**: Proven tools (CMake, HDF5, JSON) rather than custom solutions.
- **Professional Workflow**: Modern development environment with IDE integration.
- **Debugging Support**: All standard debugging tools (GDB, Valgrind) work out of the box.

---

## System Architecture

### Component Structure

```
┌─────────────────────────────────────────────────────────────┐
│                    Mimic Application                       │
├─────────────────────────────────────────────────────────────┤
│  Configuration System     │  Module System                  │
│  - JSON/Legacy .par       │  - Runtime loading              │
│  - Schema validation      │  - Dependency resolution        │
├─────────────────────────────────────────────────────────────┤
│                  Physics-Agnostic Core                     │
│  ┌─────────────────┬─────────────────┬─────────────────┐   │
│  │ Memory Mgmt     │ Property System │ I/O System      │   │
│  │ - Scoped alloc  │ - Type-safe     │ - Format unified│   │
│  │ - Auto cleanup  │ - Generated     │ - Cross-platform│   │
│  └─────────────────┴─────────────────┴─────────────────┘   │
│  ┌─────────────────┬─────────────────┬─────────────────┐   │
│  │ Tree Processing │ Pipeline Exec   │ Test Framework  │   │
│  │ - Unified model │ - Configurable  │ - Multi-level   │   │
│  │ - Inheritance   │ - Module phases │ - Scientific    │   │
│  └─────────────────┴─────────────────┴─────────────────┘   │
├─────────────────────────────────────────────────────────────┤
│                    Physics Modules                         │
│  ┌─────────────────┬─────────────────┬─────────────────┐   │
│  │ Module A        │ Module B        │ Module C        │   │
│  │ Module D        │ Module E        │ Module F        │   │
│  └─────────────────┴─────────────────┴─────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### Data Flow

1. **Configuration Loading**: Configuration files are loaded and validated.
2. **Module Registration**: Physics modules auto-register from metadata in dependency-resolved order.
3. **Pipeline Creation**: An execution pipeline is built from registered modules based on configuration.
4. **Tree Processing**: The core loads and processes merger trees using a unified algorithm.
5. **Module Execution**: Physics modules execute in dependency-resolved order.
6. **Output Generation**: A property-based output system adapts to available properties.

---

## Quality Attributes

### Maintainability
- **Modularity**: Clear separation of concerns with well-defined interfaces.
- **Documentation**: Comprehensive documentation for developers and users.
- **Code Quality**: Professional coding standards with consistent style.
- **Testing**: Automated testing covering all major functionality.

### Extensibility
- **Module Development**: Clear patterns for adding new physics modules.
- **Format Support**: Straightforward process for adding new I/O formats.
- **Property Extension**: Easy addition of new galaxy properties via metadata.
- **Configuration**: Flexible configuration system supporting new use cases.

### Reliability
- **Error Handling**: Robust error detection and recovery mechanisms.
- **Validation**: Comprehensive input and state validation.
- **Memory Safety**: Automatic memory management preventing leaks and corruption.
- **Debugging**: Clear error messages and debugging capabilities.

### Usability
- **Scientific Workflow**: Intuitive configuration for different scientific use cases.
- **Performance Analysis**: Built-in tools for understanding system performance.
- **Debugging Support**: Clear diagnostics for troubleshooting problems.

---

## Design Philosophy

This architectural vision defines Mimic as a modern, maintainable scientific software framework. The eight core principles provide a clear foundation for all development decisions, ensuring the system remains focused on its primary goals: scientific accuracy, developer productivity, and long-term maintainability.

The key insight is that **scientific accuracy and architectural elegance are not mutually exclusive**. By applying proven software engineering principles to scientific computing, Mimic accelerates scientific discovery through improved flexibility, reliability, and maintainability.

Success is measured not just by technical metrics, but by the system's ability to enable new scientific insights through easier experimentation, more reliable results, and faster development of new physics models.
