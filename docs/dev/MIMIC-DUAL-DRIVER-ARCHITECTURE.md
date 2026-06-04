# Mimic Dual-Driver Architecture

**Status:** Design vision (proposed). Extends `docs/VISION.md`.
**Companion:** `docs/dev/MIMIC-DUAL-DRIVER-CHANGE-MAP.md` (phased migration plan).
**Context:** Read `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` first for sequencing, baseline assumptions, and the named substep phase prerequisite.
**Date:** 2026-06-02

---

## 1. Purpose

Define the architecture that lets Mimic process merger trees in **two input orderings** — *tree-ordered* and *snapshot-ordered* — through two purpose-built drivers that share one physics-agnostic core, and that additionally let the physics engine be **called from an external application** to run physics modules on halos the host supplies.

This is an additive evolution of the current design, not a rewrite. Today's behaviour is the tree-ordered driver; the snapshot-ordered driver and the embeddable engine are new front-ends onto the same shared core.

---

## 2. Motivation

The single architectural fact that gates Mimic's method coverage is that the core processes **one FoF workspace at a time** in depth-first tree order, with per-tree bounded memory. That is ideal for per-history physics but structurally cannot express operations that need a whole snapshot's population co-resident: global abundance matching (true SHAM), HOD-style statistical population, a synchronous reionization radiation field, environment-dependent physics, and on-the-fly lightcone assembly.

A **snapshot-ordered** processing model — where all halos at a redshift are processed together — is the established way those methods are implemented (L-Galaxies, UniverseMachine, EMERGE are all snapshot-synchronized). Rather than bolt a global stage onto the tree driver, we make the **input format, the driver, and the memory model a single coherent choice**:

- A **tree-ordered** file feeds the **tree driver** with **per-forest** memory.
- A **snapshot-ordered** file feeds the **snapshot driver** with **per-snapshot** memory, making a snapshot's population co-resident and global operations natural.

Producing the two file orderings is the job of an **external, standalone tree converter** — not Mimic. Mimic reads exactly one ordering per run, declared in the input YAML, and fails fast on mismatch. For snapshot-ordered inputs, the converter also owns any required phantom/bridge halo insertion so the snapshot driver receives a temporally complete adjacent-snapshot representation.

---

## 3. Layered Model

Three layers. The middle and bottom layers are shared by every front-end; only the top layer is format-specific.

```
┌──────────────────────────────────────────────────────────────────────┐
│  FRONT-ENDS (format-specific; one selected per run, or external host)  │
│                                                                        │
│   Tree-ordered driver      Snapshot-ordered driver     External host   │
│   - tree-ordered reader     - snapshot-ordered reader   (hydro / SHAM / │
│   - depth-first traversal   - snapshot loop +            SBI / notebook)│
│   - per-forest memory         descendant-link lookup    - owns its own  │
│   - per-tree output buffer  - per-snapshot memory          halos+loop   │
│                             - per-snapshot output buffer - calls engine │
└───────────────┬───────────────────────┬───────────────────┬───────────┘
                │                        │                   │
                ▼                        ▼                   ▼
┌──────────────────────────────────────────────────────────────────────┐
│  SHARED ENGINE (physics-agnostic; format-neutral)                      │
│                                                                        │
│   • Inheritance / tracking service  (internal-only; shared by drivers) │
│       inherit_descendant(progenitor_galaxies[], n, descendant_props)   │
│       — type transitions, orphans, infall capture, merger clock,       │
│         snapshot-accumulator reset, central selection                  │
│                                                                        │
│   • Physics execution engine  (internal AND external entry point)      │
│       run_phases(ctx, halos, ngal)  ← configured phase sequence        │
└───────────────────────────────────────┬──────────────────────────────┘
                                         │
                                         ▼
┌──────────────────────────────────────────────────────────────────────┐
│  SHARED CORE SERVICES (format-neutral)                                 │
│   init / config / units / cosmology / Age-ZZ tables                    │
│   galaxy + output structs (Halo, GalaxyData, HaloOutput)               │
│   output schema + provenance + writers (binary / HDF5)                 │
│   memory system, error handling, module registry                       │
└──────────────────────────────────────────────────────────────────────┘
```

### 3.1 What is per-driver

Each driver owns, and may implement however suits its format:

- **Reader** — tree-ordered (`src/io/tree/*`, today) vs. a new snapshot-ordered reader. Both sit behind the existing format-reader interface (`src/io/tree/interface.h`), widened to admit a snapshot-grouped data model.
- **Traversal** — depth-first recursion (`build_halo_tree`, `build_model.c:55`) vs. a snapshot loop that, at snapshot *N*, looks up each halo's progenitor galaxies produced at *N−1*. This adjacent-snapshot assumption is an input contract: skipped links must have been filled by converter-produced phantom/bridge halos before Mimic reads the snapshot-ordered file.
- **Input bookkeeping** — `RawHalo`/`HaloAux` and the `FirstProgenitor`/`NextProgenitor`/`FirstHaloInFOFgroup` links (tree) vs. snapshot slabs plus a descendant/progenitor index (snapshot).
- **Memory + output lifecycle** — per-forest load/process/free/save (`main.c:432–471`) vs. per-snapshot.

### 3.2 What is shared

- **Inheritance / tracking service.** The *science* of how a galaxy is inherited from its progenitors — Type 0/1/2/3 transitions, orphan creation, infall-property capture, merger-clock handling, snapshot-scoped accumulator reset (`build_model.c:238`), subhalo-local central selection (`set_halo_centrals`, `build_model.c:359`) — **must be identical regardless of input ordering**. It is extracted from the current tree-index-coupled inheritance (`copy_progenitor_halos`/`join_progenitor_halos`/`find_most_massive_progenitor`, `build_model.c:128–435`) into a format-neutral function that takes *already-processed progenitor galaxies + the descendant halo's properties* and returns the inherited workspace. Both drivers gather progenitors their own way and call the same service. This is internal only — it is **not** part of the external API.
- **Physics execution engine.** `execute_phase()` (`module_registry.c:769`) is already format-neutral: it runs one configured phase over a `(ctx, halos, ngal)` triple and knows nothing of trees. The engine surface should run the configured lifecycle: fixed optional `pre_timestep`, the ordered set of named substep phases, and fixed optional `post_timestep`. Both drivers call that lifecycle identically; it is also the external entry point (§5). See `MIMIC-NAMED-SUBSTEP-PHASES.md` for the current phase-configuration contract that this extraction must preserve.
- **Core services.** Init/config/units/cosmology/time tables; the galaxy and output structs; the output schema, provenance, and binary/HDF5 writers; memory, error handling, module registry. Both drivers write the **same** output schema; only the *buffer that feeds the writer* is per-driver.

---

## 4. Format → Driver → Memory: one coherent choice

| | Tree-ordered driver | Snapshot-ordered driver |
|---|---|---|
| Input file ordering | Forests stored contiguously across all snapshots | Halos grouped by snapshot, with descendant/progenitor links |
| Traversal | Depth-first per forest | Increasing-time loop over snapshots |
| Resident working set | One forest | One snapshot's population (per MPI domain) |
| Progenitor lookup | Tree links within the forest | Descendant index into previous snapshot's processed galaxies |
| Unlocks | Per-history physics (today's capability) | Global ranking/SHAM, HOD, synchronous radiation/environment fields, lightcones |
| Memory bound | O(forest) | O(halos per snapshot per domain) |

The input YAML declares the ordering:

```yaml
TreeFormat: tree_ordered      # or: snapshot_ordered
```

Mimic validates the declared format against the reader and the requested driver at startup and **fails fast** on any mismatch (Vision Principle 7). There is no runtime auto-detection and no internal conversion: a standalone external converter produces whichever ordering is needed.

### 4.1 Snapshot input contract

The snapshot driver does not repair vertical-tree skips. A snapshot-ordered input must already be a temporally complete sequence after conversion, with phantom or bridge halos inserted where required by the converter. This follows the standard approach used by snapshot-consistent tree products: the converter owns temporal completion; Mimic owns execution on a valid declared ordering. Startup validation should check declared ordering, reader compatibility, snapshot/link consistency, and enough metadata to catch obvious mismatches, but ordinary driver logic may assume adjacent-snapshot progenitor state.

---

## 5. The embeddable physics engine (external use)

**Scope decision:** the external API exposes **physics execution only**. A host hands Mimic halos (with galaxy state) and asks it to run the configured physics modules on them for a timestep. The host keeps its own halo finding, progenitor tracking, ordering, and I/O. Mimic's inheritance service is **not** exposed externally — it remains an internal detail shared by the two drivers.

The seam already exists. `execute_phase(phase_config, num_modules, ctx, halos, ngal)` is the core per-phase call both internal drivers ultimately need, and the module unit-test harnesses already drive modules with a hand-built `ModuleContext` + `Halo[]` + pipeline config and **no merger tree** — an existing proof of concept for external invocation. The public engine should sit one level above this per-phase helper so external hosts run the same configured phase lifecycle as internal drivers.

The host contract:

1. Initialise the shared core: config (cosmology, units, model parameters), unit globals, time tables (`Age`/`ZZ`), memory system, and the module registry (`register_all_modules()` + `module_system_init()`).
2. Present `struct Halo` objects with `.galaxy` populated and a `ModuleContext` (redshift, time, dt, central).
3. Map host fields ↔ Mimic property schema (units, names) — the same Model-Set Boundary reconciliation that applies to any model package.
4. Call the engine to run the configured phase lifecycle over the supplied halos.

### 5.1 Engine state: serve both single-instance and reentrant hosts

The engine entry points are designed to take **explicit engine state, with a default global instance**:

- Single-instance, single-threaded hosts (and both internal drivers) use the default instance and are unaffected — current behaviour is preserved.
- A future threaded or multi-instance host (a hydro code calling from many threads; thousands of independent SHAM cells; several Mimic instances at once) passes its own state handle.

Achieving true reentrancy requires moving the remaining global singletons (`MimicConfig`, the unit globals, `Age`/`ZZ`, the module registry — see `src/include/globals.h`) into that handle. The architecture **does not foreclose** this, but the cost is paid only when a reentrant host actually needs it. Until then, the default global instance keeps the change surface small.

**Caveat — reentrancy is not purely a `ModuleContext` change.** Threading state through `ModuleContext` covers the `process()` path, but the module ABI also includes `init(void)` (`module_interface.h:367`) and `cleanup(void)` (`module_interface.h:407`), which take **no arguments** and read globals directly. The standard module-dependency idiom does exactly this at init time: current SAGE modules inspect the process-global `MimicConfig` through phase-aware helpers such as `module_in_substep_phase(...)`, `modules_in_same_substep_phase(...)`, and `module_precedes_in_substep_phase(...)`. Because `init`/`cleanup` signatures are part of the frozen ABI (§5.2), a truly reentrant host cannot reach instance config through them via a signature change. Reaching it would require either an init-time "current engine instance" mechanism (e.g. a thread-local set around init) or accepting that **init-time configuration remains process-global** while only per-timestep state is instanced. Either is acceptable; the point is that "carry engine state in `ModuleContext`" alone does **not** close the reentrancy gap, and this doc should not imply otherwise. This is explicitly deferred (§8, Phase 6) and called out here only so the future implementer does not under-scope it.

### 5.2 The module interface is a frozen contract

The physics-module ABI — `process(struct ModuleContext *ctx, struct Halo *halos, int ngal)` (`module_interface.h:395`), the `Module` registration struct, and the YAML→C property/metadata generation — is a **stability boundary this whole design must not perturb**. Two classes of consumer depend on it:

- Every existing model package (`models/sage`, `models/sham`) and any future one.
- The planned autonomous model-builder (`docs/galaxy-model-builder-design.md`), whose entire gate stack and per-module fan-out assume this interface and the generated-code contracts are stable.

Concretely: when engine state becomes explicit (§5.1), it must be carried **inside the `ModuleContext`** (or remain global), **not** by changing the `process()` signature. The dual-driver refactor changes how halos are *produced and ordered*, never how a module is *called*. A change that touches the module ABI is out of scope for this work and would require its own migration for both existing models and the builder.

---

## 6. Consistency with `VISION.md`

- **Principle 1 (physics-agnostic core):** unchanged and reinforced. The engine and inheritance service contain no model-specific physics; physics stays in modules. The external API runs modules without the core knowing which exist.
- **Principle 2 (runtime modularity):** unchanged. Module selection remains YAML-driven; the driver is one additional runtime selection (`TreeFormat`).
- **Principle 3 (metadata as truth):** unchanged. Both drivers emit the same generated output schema and provenance.
- **Principle 4 (one coherent processing model):** refined. There is still one processing *model* — gather progenitors → inherit → run phases → emit. The two drivers differ only in **ordering and progenitor lookup**, not in the model. The shared inheritance service is what keeps the model single.
- **Principle 5 (bounded memory):** consciously generalised. The bound becomes *per-driver*: O(forest) for the tree driver (unchanged), O(halos-per-snapshot-per-domain) for the snapshot driver. Bounded memory is preserved; the bound is explicitly different and larger for the snapshot driver, and that trade is documented rather than silent.
- **Principle 6 (format-agnostic I/O, reproducible output):** extended. A second input ordering joins the format-reader abstraction; output writers and provenance are shared, so a run remains self-describing regardless of driver.
- **Principle 7 (validation, fast failure):** reinforced. `TreeFormat` is validated against the reader and driver at startup with a fast, explicit failure.

### 6.1 When to review `VISION.md`

`VISION.md` remains the guiding source of truth and is **not** edited by this work. It should be reviewed and amended **once the snapshot driver (change-map Phase 5) lands and passes the cross-format identity gate** — that is the point at which the "one coherent processing model" (Principle 4) and "bounded memory" (Principle 5) statements have demonstrably generalised to two drivers and the determinism invariant (§7.1) has earned first-class status. The amendment should be small: record the per-driver memory bound, name determinism as an invariant, and point to this document. Do not pre-emptively edit the vision before the behaviour it would describe actually exists.

---

## 7. A built-in correctness check: cross-format identity

Because an external converter can emit both orderings of the *same* underlying trees, and because both drivers share the inheritance service and physics engine, the two drivers must produce **identical galaxies** for equivalent input (modulo documented snapshot-only global physics that the tree driver cannot express, which is disabled for the comparison). Cross-format agreement becomes a standing regression test of the shared core and both front-ends, and the primary acceptance gate for the snapshot driver.

### 7.1 The invariant this gate depends on: determinism

Byte-for-byte cross-format identity is achievable **only because Mimic's physics is currently deterministic and per-FoF independent**:

- No random number stream is consumed during computation. The RNG is seeded but unused (`init.c:59`, `main.c:450` both note this explicitly).
- Per-FoF work is independent, with per-tree-bounded memory; there is no cross-FoF accumulator, global reduction, or order-dependent floating-point sum.

Given those two facts, identical inputs produce identical outputs regardless of the order in which FoF systems are visited, so the tree and snapshot drivers can agree exactly. **This is a named invariant, not an incidental property.** Two consequences the implementer must hold to:

1. **Any future stochastic module must seed deterministically from a per-halo /per-FoF key** (e.g. a hash of the halo ID), never from a global stream consumed in traversal order. A global RNG stream would be consumed in different orders by the two drivers and would break byte-identity *permanently*, demoting this gate from exact to statistical.
2. This invariant intersects the planned model-builder (`docs/galaxy-model-builder-design.md`), which anticipates stochastic physics with "deterministic seeds." The two designs agree **only** under per-halo seeding; that constraint belongs in both documents and is recorded here as the normative source.

If exact identity ever becomes genuinely unreachable (e.g. a legitimate, science-neutral floating-point reordering inside the shared engine), the gate degrades to a documented numeric tolerance with a recorded justification rather than a silent pass — see the byte-identity discussion in the companion change map.

---

## 8. Scope boundaries

**In scope:** two drivers behind one shared core; format-neutral inheritance service; physics-only embeddable engine; `TreeFormat` selection with fast failure; engine-state design that admits future reentrancy.

**Out of scope (explicitly):**
- Tree-format conversion inside Mimic (external converter owns it).
- Phantom/bridge halo insertion inside Mimic (external converter owns temporal completion for snapshot-ordered inputs).
- Exposing the inheritance service externally (internal-only by decision).
- Full de-globalisation / thread-safety now (designed-for, not built now).
- A production per-snapshot collective module ABI for global SHAM/HOD/radiation-field/lightcone operations. The snapshot driver makes those operations expressible, but the cross-format identity driver must land first with ordinary FoF-scoped physics; the collective contract needs its own design and validation once the driver exists.
- MPI-distributed global operations in the snapshot driver (a later phase; the single-node snapshot driver is globally correct over the whole box first, with cross-domain communication for distributed global ranking added afterwards).
- New physics. This is an execution-architecture change; module science is untouched.

---

## 9. Glossary

- **Tree-ordered / snapshot-ordered** — the two on-disk merger-tree orderings, each read by exactly one driver.
- **Driver** — a format-specific front-end owning reader, traversal, memory, and output buffering; it gathers progenitors and delegates inheritance + physics to the shared engine.
- **Inheritance / tracking service** — the format-neutral, internal shared function that turns a progenitor galaxy set + descendant halo properties into the inherited FoF workspace.
- **Physics engine** — the format-neutral module-execution call (`execute_phase`), shared internally and exposed externally as the embeddable API.
- **Engine state / handle** — the (currently global, optionally explicit) configuration + units + time + registry state the engine reads.
