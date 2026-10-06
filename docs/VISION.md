# Mimic Architectural Vision

**Purpose**: Define the architectural principles and design boundaries for Mimic, a physics-agnostic galaxy evolution framework.

---

## Vision Statement

Mimic is a **physics-agnostic core with runtime-configurable physics modules**. The core owns execution, memory, I/O, metadata, and validation. Physics modules own astrophysical prescriptions and may be combined at runtime through configuration files.

This architecture lets researchers compare physics models without recompiling, lets developers work on infrastructure and physics independently, and keeps scientific behavior reproducible through explicit metadata and output provenance.

The key design claim is that scientific flexibility and engineering discipline support each other. Mimic should make experiments easier to run while making hidden assumptions, stale duplicated state, and silent configuration errors harder to introduce.

---

## Table of Contents

1. [Vision Statement](#vision-statement)
2. [Core Architectural Principles](#core-architectural-principles)
3. [Data Flow](#data-flow)
4. [Documentation Directory](#documentation-directory)

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

**Principle**: Physics combinations are selected at runtime from the compiled model set.

**Requirements**:
- Mimic is compiled against one internally consistent model set and one simulation/catalog property package, selected with `make MODEL=<name> SIMULATION=<name>`.
- Module selection and processing mode are declared in the input YAML file.
- Runtime selection is limited to modules in the selected `models/<model>/` package and halo/catalog properties in the selected `simulations/<simulation>/` package; cross-model experiments should be made explicit by creating a new model package.
- Modules declare supported processing modes and event contracts in metadata when using the directory-module pattern.
- The pipeline can run with any valid combination of configured modules, including no modules.
- Scientific ordering remains explicit in configuration. Metadata validation catches wiring errors but does not replace scientific judgement.

**In practice**: Users can disable supernova feedback, switch an AGN mode, or run halo tracking only by editing the YAML configuration and rerunning the executable built for that model and simulation package. Users who want to mix modules from different model families copy them into a new `models/<model>/` package and reconcile properties, parameters, units, tests, and plots there.

### 3. Metadata as the Source of Structural Truth

**Principle**: Repeated structural definitions should be generated from metadata rather than hand-maintained in multiple files.

**Requirements**:
- Halo and galaxy properties are defined in YAML metadata.
- Directory modules define registration metadata in `module_info.yaml`.
- Generated code provides C struct fields, output schema writers, HDF5 field metadata, module registration, and event identifiers.
- Documentation should explain generated systems, but should avoid duplicating exhaustive generated lists unless the copy is small and stable.

**In practice**: Adding a galaxy property requires editing the selected model package property file, such as `models/sage16/model_properties.yaml`, then running `make generate` for the default package pair. Adding a catalog halo property requires editing the selected simulation package property file, such as `simulations/mini-millennium/halo_properties.yaml`, and regenerating with the same selectors when using non-default packages. Production runtime modules should use a module directory under `models/<model>/modules/` containing the C implementation and `module_info.yaml`. Package-local standalone source modules under `models/<model>/modules/*.c` are supported for simple prototypes, but should be converted to directory modules once metadata, tests, dependencies, or event contracts matter.

### 4. One Coherent Processing Model

**Principle**: Mimic should expose one clear model for processing merger trees.

**Requirements**:
- Each snapshot interval is processed through a single traversal model.
- Physics modules operate on FoF workspaces containing the central galaxy and any satellites for that FoF system.
- `process_full_halo`, `process_by_galaxy`, and `process_per_event` are dispatch modes within this model, not separate tree-processing algorithms.
- Galaxy inheritance, orphan handling, and property reset rules are centralized and documented.
- Physics dispatched through this model must be deterministic given a halo's own data: stochastic modules seed from stable per-halo or per-FoF keys, never from a global RNG stream consumed in traversal order, so identical input data produces identical physics output regardless of which driver or traversal order processed it. The one declared exception is the `UniqueGalaxyID` of a record a module creates: it encodes its host's catalogue position, so it is deterministic for a fixed dataset, driver and run file but differs between drivers (see the Developer Guide's Record Creation Contract); stochastic modules therefore key their draws on a host's tree ID, never on a created record's.
- Modules may create galaxy records, but only through one core creation operation and only from full-halo callbacks. Created records join the FoF workspace when the creating callback returns and are marshalled inside their host's output segment; core owns their defaults, identity, linkage, and memory, and the module owns their physics and lifetime. The snapshot scope remains topology-immutable.
- The one additive exception is the snapshot scope: a `process_snapshot` module configured under `modules.post_snapshot` runs once per snapshot, after every FoF workspace of that snapshot, over the snapshot's whole processed population. Its determinism is given the complete snapshot population, not a single FoF input: identical populations (the same galaxies with the same properties) must give identical results whatever the order of FoF groups or of entries within the population, and the result may legitimately depend on galaxies in other FoF groups. Only the horizontal driver holds a complete snapshot, collectively across its ranks, so only it runs this scope, and a snapshot module reaches whole-population quantities (a global rank, a sum, an extent) through the core's snapshot collectives, whose results are functions of the complete population (exactly so for ranks, integer sums, extents and agreement; a floating-point sum may differ in its last bits with the reduction order). Further snapshot-wide modes, such as rate or batch callbacks, remain prospective and would each need their own explicit contract.

**In practice**: A full-halo module receives the whole FoF workspace. A by-galaxy module receives one galaxy at a time from that same workspace. Event consumers receive one event target after a full-halo producer emits a subscribed event. Mimic ships two drivers over this one processing model — a vertical driver (per-forest, depth-first) and a horizontal driver (per-snapshot, increasing time order) — sharing the same inheritance, physics-execution, and output-marshalling services. A snapshot module receives a borrowed view of the horizontal driver's current snapshot population after the FoF sweep and before that snapshot is inherited from or written; it may update galaxy properties in place but never the snapshot's topology, and it can neither emit nor consume FoF events. A full-halo module that needs galaxies the merger trees do not supply, such as the synthetic satellites of a halo occupation model, asks core to create them: core appends them to the FoF workspace when the callback returns, so every later module, phase, and substep of that snapshot sees them as ordinary entries, and the next snapshot inherits them like any other orphan.

### 5. Bounded Memory and Explicit Ownership

**Principle**: Memory use should be predictable, bounded by the current processing scope, and visible during debugging.

**Requirements**:
- Processing allocates halo, galaxy, tree, I/O, and utility memory with explicit categories.
- Per-tree or per-forest working memory is cleaned up after processing.
- Long runs should not accumulate memory with the number of forests processed.
- Module-owned allocations must be released by module cleanup.
- Each driver's working-set bound matches its own processing scope: the vertical driver bounds memory to one forest at a time; the horizontal driver bounds memory to the snapshot generations its input's links still need: each generation (raw slab, processed state and galaxies) is retained until the latest snapshot any of its halos names as a descendant has been processed, then released, and under MPI each rank bounds its memory to its own forests' rows of the retained generations; a task may also sweep its forests in several chunks, each chunk bounding memory to its own rows of the retained generations, so the bound then depends on the widest chunk rather than the widest slab, still never on simulation depth or total halo count. For adjacent input that is two live generations, the current one and the previous one; gapped input retains a generation across the snapshots its descendants skip (mini-Millennium holds at most three, because its longest descendant span is 2). The bound depends on the widest slab and the longest gap, never on simulation depth or total halo count, and an optional run-file ceiling bounds only the retention pool's admitted payload: it refuses a generation before allocation when the pool would exceed it, and does not bound in-sweep growth, the driver's workspace or process RSS. Memory a snapshot module allocates for its callback is outside that accounting too: it is module-owned, released by the module (before the callback returns or in `cleanup()`), and no ceiling bounds it.

**In practice**: The allocator tracks memory categories and can report leaks during debug runs. Created records are accounted in the galaxy pool and output buffer like any row.

### 6. Format-Agnostic I/O and Reproducible Output

**Principle**: Input and output formats should be handled through common interfaces, and outputs should carry enough metadata to interpret a run.

**Requirements**:
- Vertical readers and output writers are isolated behind format-specific implementations.
- Output schema follows property metadata rather than hand-written duplicate structs.
- HDF5 output records field metadata, enabled modules, model parameters, redshift mapping, version information, and event contracts when present.
- Binary output remains compact and is interpreted through the run-local `metadata/output_schema.json` written by the executable that produced it.

**In practice**: Users should be able to inspect an HDF5 file and recover the active module pipeline and field units without reading the input YAML separately.

### 7. Validation, Type Safety, and Fast Failure

**Principle**: Invalid configuration or metadata should fail early with useful errors.

**Requirements**:
- Generated code gives modules typed access to declared properties.
- Module metadata validation catches missing files, invalid processing modes, unknown property dependencies, and event wiring mistakes.
- Module parameter validation happens in module `init()` because only the module knows its physical constraints.
- Failing tests are treated as real problems, not documentation or test-suite noise.

**In practice**: `make MODEL=<name> SIMULATION=<name> validate-modules`, `make MODEL=<name> SIMULATION=<name> check-generated`, and startup validation provide fast feedback before a long scientific run begins.

---

## Data Flow

1. **Configuration loading**: The input YAML is parsed into runtime configuration, including output settings, input tree settings, simulation units, cosmology, module phases, and model parameters.
2. **Metadata generation**: Property and module metadata for the selected model and simulation generate C structs, output metadata writers, module registration, and event identifiers.
3. **Module registration**: The generated registry registers available runtime modules and their supported modes.
4. **Pipeline validation**: The configured phases are checked against registered modules, supported modes, and event contracts.
5. **Tree processing**: The core loads merger trees and builds FoF workspaces for each snapshot interval.
6. **Module execution**: For each FoF workspace, configured modules run in phase order and dispatch mode order. Under the horizontal driver, configured `post_snapshot` modules then run once over the whole snapshot population, before it is inherited from or written.
7. **Output generation**: The generated output schema writes binary or HDF5 output with metadata appropriate to the selected format.

For implementation details, see [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md#architecture-overview). For run and configuration guidance, see [USER-GUIDE.md](USER-GUIDE.md).

---

## Documentation Directory

- [README.md](../README.md): project overview and shortest path to a first result
- [USER-GUIDE.md](USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [STYLE-GUIDE.md](STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): detailed plotting manual
- [convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md](../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md): on-disk contract for horizontal HDF5 merger-tree input
- [convert/mimic-convert/README.md](../convert/mimic-convert/README.md): merger-tree converter manual
- [tests/README.md](../tests/README.md): test-suite quick reference
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
