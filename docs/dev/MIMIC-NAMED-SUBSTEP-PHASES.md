# Mimic Named Substep Phases

**Status:** Design context note, not a complete implementation plan.
**Context:** Read `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` first for sequencing. This note should inform the implementation plan that replaces fixed `phase_1`/`phase_2` substep slots with user-named middle phases.
**Date:** 2026-06-04

---

## Purpose

Mimic currently fixes the module pipeline shape as `pre_timestep` -> `phase_1` -> `phase_2` -> `post_timestep`, with `phase_1` and `phase_2` repeated inside each substep. This note proposes a small generalisation: keep `pre_timestep` and `post_timestep` as fixed optional lifecycle phases, but let the input file define an arbitrary ordered set of named phases in between.

This is not a request to add an automatic scheduler or to make module metadata decide scientific order. The input file remains the source of execution ordering. The goal is to let model authors name the repeated middle phases according to their physical meaning, while removing the hard-coded assumption that every model needs exactly two substep phases named `phase_1` and `phase_2`.

---

## Motivation

The current two middle-phase names are implementation labels, not scientific language. In SAGE, `phase_1` currently carries cooling, star formation, feedback, disk instability, and related local updates, while `phase_2` carries merger/disruption work and event consumers. Other models may naturally want a different decomposition: cooling, star formation, enrichment, mergers, environment, luminosities, or any other physically meaningful sequence.

Named substep phases make the configuration more readable and more faithful to the model being expressed:

- The YAML pipeline documents the scientific structure directly.
- The core no longer needs duplicated `phase_1`/`phase_2` plumbing.
- Future tools, especially the model-builder workflow, can map paper processes to named Mimic phases without forcing them into generic numbered buckets.
- The dual-driver work gets a cleaner engine boundary: drivers run the configured phase sequence rather than knowing about two special middle slots.

This is an investment in expressiveness, but it should remain modest. It should not turn the module system into a dependency resolver or a general workflow engine.

---

## Proposed End State

The outer timestep lifecycle stays fixed:

1. `pre_timestep` runs once before substeps.
2. User-named middle phases run in input order once per substep.
3. `post_timestep` runs once after substeps.

`pre_timestep` and `post_timestep` remain skippable by leaving them empty, as today.

Use a dedicated `phases:` mapping under `modules:` for the arbitrary middle phases. This avoids ambiguity with reserved keys such as `parameters`, and it keeps parsing simple.

```yaml
modules:
  pre_timestep:
    - sage_reionization: process_full_halo
    - sage_prepare_infall_budget: process_full_halo

  phases:
    infall_and_cooling:
      - sage_apply_infall: process_full_halo
      - sage_reincorporation: process_full_halo
      - sage_satellite_stripping: process_by_galaxy
      - sage_calculate_cooling_budget: process_by_galaxy
      - sage_radio_mode_heating: process_by_galaxy
      - sage_apply_cooling: process_by_galaxy

    star_formation_feedback:
      - sage_calculate_star_formation: process_by_galaxy
      - sage_calculate_supernova_feedback: process_by_galaxy
      - sage_apply_star_formation_supernova: process_by_galaxy

    instabilities_and_mergers:
      - sage_disk_instability: process_by_galaxy
      - sage_quasar_mode: process_by_galaxy
      - sage_starburst_feedback: process_by_galaxy
      - sage_resolve_mergers_and_disruption: process_full_halo
      - sage_quasar_mode: process_per_event
      - sage_starburst_feedback: process_per_event

  post_timestep: []

  parameters:
    BaryonFrac: 0.17
    RecycleFraction: 0.43
```

The exact SAGE split above is illustrative, not a required migration. The implementer should preserve current SAGE behaviour unless an explicit scientific review approves a changed split.

---

## Recommended Dispatch Rule

Within each named phase, keep the current dispatch contract:

1. Full-halo modules run first, in YAML order.
2. Events emitted by full-halo producers are dispatched to same-phase per-event consumers.
3. Galaxy-local modules run after full-halo/event work, in galaxy-major order, preserving YAML order within the by-galaxy group.

Use this wording in the user-facing docs: **full-halo/event work precedes galaxy-local work**.

This preserves the current clear processing model while giving users physically meaningful phase names and an arbitrary number of repeated middle phases. It also avoids hiding traversal changes behind YAML line order.

---

## Rejected For Now: Raw Mixed Mode Ordering

Do not initially make raw YAML line order control traversal mode interleaving inside a phase. For example:

```yaml
phases:
  example:
    - local_a: process_by_galaxy
    - local_b: process_by_galaxy
    - group_c: process_full_halo
    - local_d: process_by_galaxy
```

This is implementable, but it changes a deep semantic rule. The least surprising implementation would group adjacent runs of the same traversal style: run `local_a` and `local_b` galaxy-major over all galaxies, then run `group_c` once on the FoF workspace, then run `local_d` galaxy-major. That is more expressive, but it adds another execution model and makes event timing and reproducibility harder to explain.

If a future model genuinely needs interleaving, add explicit traversal blocks rather than interpreting arbitrary mixed line order:

```yaml
phases:
  example:
    - by_galaxy:
        - local_a
        - local_b
    - full_halo:
        - group_c
    - by_galaxy:
        - local_d
```

That future design should be justified by a concrete physics requirement, not added pre-emptively.

---

## Consistency With The Vision

This proposal supports the current architectural principles:

- **Physics-agnostic core:** phase names are user-facing configuration labels, not physics hard-coded into the core.
- **Runtime modularity:** module combinations and scientific phase order remain selected by the input file.
- **Metadata as structural truth:** metadata still declares module capabilities and contracts; it does not sort the scientific pipeline.
- **One coherent processing model:** drivers still gather FoF workspaces, then run the configured phase sequence. The only change is the number and names of repeated middle phases.
- **Bounded memory:** the proposal does not change the FoF-scoped memory model.
- **Reproducible output:** phase names should be recorded in run provenance so the physical pipeline can be recovered from outputs.
- **Validation and fast failure:** unknown modules, unsupported modes, duplicate or invalid phase names, and broken event subscriptions should fail at startup.

The strongest KISS/DRY version is an internal representation like:

```c
struct ModulePhaseConfig {
  char name[MAX_STRING_LEN];
  struct PhaseModuleConfig *modules;
  int num_modules;
};
```

`MimicConfig` can then hold fixed `pre_timestep` and `post_timestep` phase configs plus `substep_phases[]`. Parsing, validation, registration, provenance, cleanup, and execution should iterate the same array rather than duplicating per-phase code paths.

---

## Compatibility And Migration Notes

The first implementation should be behaviour-preserving for existing inputs. Support the current `phase_1` and `phase_2` keys as a compatibility form, internally translating them into:

```yaml
phases:
  phase_1: [...]
  phase_2: [...]
```

Once the new structure is documented and used by the shipped input files, the old top-level `phase_1`/`phase_2` keys can be deprecated with a clear warning before any removal.

The real migration cost is not the parser. It is module and test code that currently inspects hard-coded phase arrays such as `MimicConfig.phase_1` and `MimicConfig.phase_2` during `init()`. Replace those checks with phase-aware helpers that ask questions in terms of the configured pipeline:

- Is this module configured anywhere in the substep phase sequence?
- Is this module configured in the same phase as another module?
- Does module A precede module B in the same phase?
- Does a per-event consumer have its full-halo producer in the same phase?
- What is the name of the phase containing this module/mode pairing?

These helpers keep module validation explicit while avoiding numbered-phase coupling.

---

## Placement In The Longer-Term Roadmap

This work should sit after the v1.0 golden baseline is established and before the dual-driver migration starts in earnest. It is small enough to be implemented as its own behaviour-preserving configuration refactor, and doing it first simplifies later engine extraction.

Recommended sequence:

1. Finish v1.0 preparation.
2. Tag v1.0 and establish the golden SAGE baseline.
3. Generalise substep phase configuration to fixed `pre_timestep`/`post_timestep` plus named middle phases, preserving current dispatch semantics and output identity.
4. Proceed with the dual-driver change map, using the named phase sequence as the engine contract.
5. Later, revise the model-builder proposal against the named phase contract rather than the old four-phase wording.

This proposal should not block v1.0. It should also not be bundled into the high-risk inheritance extraction in the dual-driver plan. It is a cleaner, lower-risk stepping stone between them.

---

## Review Points For The Implementation Plan

The team writing the complete plan should decide and document:

- The exact YAML compatibility rules when both legacy `phase_1`/`phase_2` and new `phases:` are present. Recommended: fail fast rather than merge.
- Phase-name validation rules. Recommended: non-empty strings, unique names, no reserved names (`pre_timestep`, `post_timestep`, `parameters`, `phases`), and stable output-safe characters.
- Provenance format for named phases in binary schema metadata and HDF5 run properties.
- Whether module dependency helpers should search by module name only or by `(module name, processing mode)` where modules can appear more than once.
- How to report validation failures so users see physical phase names, not internal indices.
- The deprecation timeline for legacy `phase_1`/`phase_2` input keys.
- The minimal regression fixture proving legacy and translated named-phase inputs produce byte-identical output.

---

## Non-Goals

- No automatic sorting from metadata dependencies.
- No DAG scheduler.
- No raw mixed traversal ordering as the first implementation.
- No change to the module ABI: `process(ctx, halos, ngal)` stays fixed.
- No change to tree traversal, inheritance, output schemas, or physical prescriptions.
- No `VISION.md` amendment until this behaviour actually exists and the docs can describe implemented reality rather than intent.
