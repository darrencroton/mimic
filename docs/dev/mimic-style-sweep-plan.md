# Mimic Style Sweep Plan

**Status:** Active pre-v1.0 quality plan.
**Date:** 2026-06-23

**Purpose:** organise the repository-wide style sweep into manageable, ownership-based chats. Each chat should apply `docs/STYLE-GUIDE.md`, keep scientific behavior unchanged, fix style issues in scope, and avoid unrelated whole-repo cleanup.

## Working Rule for Each Sweep Chat

Use a prompt like:

> Sweep only these paths against `docs/STYLE-GUIDE.md`; fix style issues in scope; do not change scientific behavior; run formatter and targeted validation; report remaining style debt.

Each chat should:

- Review only the listed paths unless a directly related helper must be checked.
- Apply `docs/STYLE-GUIDE.md` to comments, naming, documentation, metadata, tests, logging, generated-code boundaries, and local organisation.
- Run `./scripts/beautify.sh` before finishing.
- Run the narrowest relevant validation/test command.
- Report any style issue deliberately left unresolved.
- Avoid broad renames, comment delimiter churn, or unrelated documentation reshuffles unless they fix real inconsistency or confusion.

## Review Surface

Approximate reviewable file volume after excluding `build/`, `mimic_venv/`, `sage-code/`, `archive/`, `output/`, `benchmarks/`, and `generated/` directories:

| Area | Approx files | Notes |
| --- | ---: | --- |
| `src/` | 104 | Core C infrastructure, highest architectural risk |
| `models/sage16/` | 140 | Largest area; modules are the main sweep workload |
| `tests/` | 53 | Framework, C unit, Python integration/scientific |
| `models/sham/` | 24 | Smaller model package, useful after SAGE patterns settle |
| `models/halos-only/` | 14 | Small package, mostly plots/input/metadata |
| `plot/mimic-plot/` | 13 | Plotting tool and helpers |
| `scripts/` | 11 | Generators, validators, discovery tooling |
| `simulations/` | 19 | Metadata plus simulation-owned tests |
| `docs/` | 8 | Guides and repo-level documentation |

## Ordered Sweep Batches

1. **Baseline and Guardrails**
   Scope: `docs/STYLE-GUIDE.md`, `AGENTS.md`, formatter config, generated-code rules.
   Goal: confirm the rules are clear before applying them.
   Run: `make check-docs`.

2. **Core Execution**
   Scope: `src/core/`, `src/include/`.
   Approx: 22 files.
   Focus: comments, ownership/lifetime docs, logging, fatal/error paths, generated boundaries, naming consistency.
   Run: `make`, targeted unit tests touching config/core if changed.

3. **Utilities**
   Scope: `src/util/`.
   Approx: 14 files.
   Focus: allocator/error/logging contracts, public header comments, comment quality, memory ownership.
   Run: relevant unit tests, especially memory/error/numeric tests.

4. **Tree and Output I/O**
   Scope: `src/io/`.
   Approx: 31 files.
   Focus: file-format boundaries, I/O logging macros, error messages, reader/writer ownership, HDF5 guards.
   Run: `make`, output-format and tree-reader tests as relevant.

5. **Module System Infrastructure**
   Scope: `src/module_system/`.
   Approx: 28 files.
   Focus: template quality, public contracts, test fixtures, event/module metadata boundaries.
   Run: `make validate-modules`, module-system tests.

6. **Scripts and Generators**
   Scope: `scripts/`.
   Approx: 11 files.
   Focus: docstrings, CLI usage, path handling, error messages, generated-code source of truth.
   Run: `make validate-modules`, `make check-generated` if generator-related files change.

7. **Top-Level Test Framework**
   Scope: `tests/framework/`, `tests/unit/`.
   Approx: 30-ish files.
   Focus: structured markers, skip reasons, test section comments, assertion clarity.
   Run: targeted C unit tests; full `make tests-unit summary` if changes are broad.

8. **Integration and Scientific Tests**
   Scope: `tests/integration/`, `tests/scientific/`, `tests/README.md`.
   Approx: 20-ish files.
   Focus: marker helpers, `TestSkipped`, fixture cleanup, output capture, meaningful failure messages.
   Run: targeted scripts; full `make tests-integration summary` only when needed.

9. **SAGE Shared Helpers and Metadata**
   Scope: `models/sage16/shared/`, `models/sage16/model_properties.yaml`, `models/sage16/input/`, `models/sage16/README.md`.
   Approx: 14 files.
   Focus: property descriptions, units, sentinel comments, shared helper API comments, package docs.
   Run: `make MODEL=sage16 SIMULATION=mini-millennium validate-modules`.

10. **SAGE Modules Batch A: Infall, Cooling, Reionization**
    Scope: `models/sage16/modules/sage_prepare_infall_budget/`, `models/sage16/modules/sage_apply_infall/`, `models/sage16/modules/sage_reionization/`, `models/sage16/modules/sage_calculate_cooling_budget/`, `models/sage16/modules/sage_apply_cooling/`, `models/sage16/modules/sage_radio_mode_heating/`.
    Approx: 32 files.
    Focus: module README versus `module_info.yaml`, lifecycle layout, ordering checks, SAGE parity comments.
    Run: module-owned unit/integration tests touched.

11. **SAGE Modules Batch B: Star Formation, Supernova, Metals**
    Scope: `models/sage16/modules/sage_calculate_star_formation/`, `models/sage16/modules/sage_calculate_supernova_feedback/`, `models/sage16/modules/sage_apply_star_formation_supernova/`, `models/sage16/modules/sage_apply_metal_enrichment/`.
    Approx: 18 files.
    Focus: scientific comments, transport properties, parameter validation, no obvious-comment noise.
    Run: module-owned tests.

12. **SAGE Modules Batch C: Mergers, Satellites, Disk/AGN**
    Scope: `models/sage16/modules/sage_initialise_merger_clock/`, `models/sage16/modules/sage_resolve_mergers_and_disruption/`, `models/sage16/modules/sage_satellite_stripping/`, `models/sage16/modules/sage_disk_instability/`, `models/sage16/modules/sage_quasar_mode/`, `models/sage16/modules/sage_starburst_feedback/`, `models/sage16/modules/sage_reincorporation/`, `models/sage16/modules/sage_set_disk_scale_radius/`.
    Approx: 41 files.
    Focus: event/ordering docs, lifecycle consistency, helper extraction, README contracts.
    Run: module-owned tests plus merger/event tests if touched.

13. **SAGE Cross-Module Tests**
    Scope: `models/sage16/modules/_tests/`.
    Approx: 8 files.
    Focus: processing-mode contract tests, scientific baseline clarity, marker/skip behavior.
    Run: targeted files or SAGE package test tier.

14. **Plotting System**
    Scope: `plot/mimic-plot/`, `models/*/plots/`.
    Approx: 52 files.
    Best split into two chats if needed: plotting engine first, figure modules second.
    Focus: docstrings, validation messages, `(plot_path, skip_message)` convention, duplicated plot boilerplate.
    Run: plotting unit tests and selected plot generation.

15. **SHAM and Halos-Only Packages**
    Scope: `models/sham/`, `models/halos-only/`.
    Approx: 38 files.
    Focus: consistency with SAGE package conventions without overfitting to SAGE physics.
    Run: package-specific validation/tests where available.

16. **Simulation Packages**
    Scope: `simulations/mini-millennium/`, `simulations/millennium/`.
    Approx: 19 files.
    Focus: halo property metadata, units, snapshot/profile docs, simulation-owned tests.
    Run: simulation-local tests and metadata validation.

17. **Project Documentation Final Pass**
    Scope: `README.md`, `docs/`, `tests/README.md`, package READMEs touched during prior sweeps.
    Focus: stale references, duplicated generated lists, links, consistency with style guide.
    Run: `make check-docs`.

18. **Final Integration Pass**
    Scope: repo-level status after all sweep batches.
    Run: `./scripts/beautify.sh`, `make check-format`, `make check-docs`, `make validate-modules`, `make check-generated`, then selected broader tests.

## Notes

- The largest single area is `models/sage16/modules/`, so keep it split into multiple physics-themed batches.
- The core infrastructure batches should happen before model-package sweeps, so module changes can align with settled project conventions.
- The plotting system can be split if one chat gets too large.
- Use focused edits. The goal is a professional, consistent codebase, not style churn for its own sake.
