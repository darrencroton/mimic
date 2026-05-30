# Mimic Architectural Vision

**Purpose**: Define the architectural principles and design boundaries for Mimic, a physics-agnostic galaxy evolution framework.

---

## Vision Statement

Mimic is a **physics-agnostic core with runtime-configurable physics modules**. The core owns execution, memory, I/O, metadata, and validation. Physics modules own astrophysical prescriptions and may be combined at runtime through configuration files.

This architecture lets researchers compare physics models without recompiling, lets developers work on infrastructure and physics independently, and keeps scientific behavior reproducible through explicit metadata and output provenance.

The key design claim is that scientific flexibility and engineering discipline support each other. Mimic should make experiments easier to run while making hidden assumptions, stale duplicated state, and silent configuration errors harder to introduce.

---

## Core Architectural Principles

These principles guide design decisions and implementation choices in Mimic.

### 1. Physics-Agnostic Core Infrastructure

**Principle**: Core infrastructure must not depend on a specific physics implementation.

**Requirements**:
- Core systems for memory management, tree processing, configuration, logging, and I/O operate independently of SAGE or any other model.
- Physics modules interact with the core only through documented interfaces.
- An empty module pipeline is valid and performs halo tracking without galaxy physics.
- Infrastructure tests use framework fixtures rather than production physics modules.

**In practice**: The core evolution loop iterates over registered modules and calls generic function pointers. It does not include SAGE headers or call SAGE functions directly.

### 2. Runtime Modularity

**Principle**: Physics combinations are selected at runtime from configuration, not fixed at compile time.

**Requirements**:
- Module selection and processing mode are declared in the input YAML file.
- Modules declare supported processing modes and event contracts in metadata when using the directory-module pattern.
- The pipeline can run with any valid combination of configured modules, including no modules.
- Scientific ordering remains explicit in configuration. Metadata validation catches wiring errors but does not replace scientific judgement.

**In practice**: Users can disable supernova feedback, switch an AGN mode, or run halo tracking only by editing the YAML configuration and rerunning the executable.

### 3. Metadata as the Source of Structural Truth

**Principle**: Repeated structural definitions should be generated from metadata rather than hand-maintained in multiple files.

**Requirements**:
- Halo and galaxy properties are defined in YAML metadata.
- Directory modules define registration metadata in `module_info.yaml`.
- Generated code provides C struct fields, output schema, HDF5 field metadata, Python dtypes, module registration, and event identifiers.
- Documentation should explain generated systems, but should avoid duplicating exhaustive generated lists unless the copy is small and stable.

**In practice**: Adding a galaxy property requires editing the model package property file, such as `models/sage/model_properties.yaml`, then running `make generate`. Production runtime modules should use a module directory under `models/<model>/modules/` containing the C implementation and `module_info.yaml`. Package-local standalone source modules under `models/<model>/modules/*.c` are supported for simple prototypes, but should be converted to directory modules once metadata, tests, dependencies, or event contracts matter.

### 4. One Coherent Processing Model

**Principle**: Mimic should expose one clear model for processing merger trees.

**Requirements**:
- Each snapshot interval is processed through a single traversal model.
- Physics modules operate on FoF workspaces containing the central galaxy and any satellites for that FoF system.
- `process_full_halo`, `process_by_galaxy`, and `process_per_event` are dispatch modes within this model, not separate tree-processing algorithms.
- Galaxy inheritance, orphan handling, and property reset rules are centralized and documented.

**In practice**: A full-halo module receives the whole FoF workspace. A by-galaxy module receives one galaxy at a time from that same workspace. Event consumers receive one event target after a full-halo producer emits a subscribed event.

### 5. Bounded Memory and Explicit Ownership

**Principle**: Memory use should be predictable, bounded by the current processing scope, and visible during debugging.

**Requirements**:
- Processing allocates halo, galaxy, tree, I/O, and utility memory with explicit categories.
- Per-tree or per-forest working memory is cleaned up after processing.
- Long runs should not accumulate memory with the number of forests processed.
- Module-owned allocations must be released by module cleanup.

**In practice**: The allocator tracks memory categories and can report leaks during debug runs.

### 6. Format-Agnostic I/O and Reproducible Output

**Principle**: Input and output formats should be handled through common interfaces, and outputs should carry enough metadata to interpret a run.

**Requirements**:
- Tree readers and output writers are isolated behind format-specific implementations.
- Output schema follows property metadata rather than hand-written duplicate structs.
- HDF5 output records field metadata, enabled modules, model parameters, redshift mapping, version information, and event contracts when present.
- Binary output remains compact but requires the generated dtype that matches the current property metadata.

**In practice**: Users should be able to inspect an HDF5 file and recover the active module pipeline and field units without reading the input YAML separately.

### 7. Validation, Type Safety, and Fast Failure

**Principle**: Invalid configuration or metadata should fail early with useful errors.

**Requirements**:
- Generated code gives modules typed access to declared properties.
- Module metadata validation catches missing files, invalid processing modes, unknown property dependencies, and event wiring mistakes.
- Module parameter validation happens in module `init()` because only the module knows its physical constraints.
- Failing tests are treated as real problems, not documentation or test-suite noise.

**In practice**: `make validate-modules`, `make check-generated`, and startup validation provide fast feedback before a long scientific run begins.

---

## Data Flow

1. **Configuration loading**: The input YAML is parsed into runtime configuration, including output settings, input tree settings, simulation units, cosmology, module phases, and model parameters.
2. **Metadata generation**: Property and module metadata generate C structs, output metadata, Python dtype helpers, module registration, and event identifiers.
3. **Module registration**: The generated registry registers available runtime modules and their supported modes.
4. **Pipeline validation**: The configured phases are checked against registered modules, supported modes, and event contracts.
5. **Tree processing**: The core loads merger trees and builds FoF workspaces for each snapshot interval.
6. **Module execution**: For each FoF workspace, configured modules run in phase order and dispatch mode order.
7. **Output generation**: The generated output schema writes binary or HDF5 output with metadata appropriate to the selected format.

For implementation details, see [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md#architecture-overview). For run and configuration guidance, see [USER-GUIDE.md](USER-GUIDE.md).
