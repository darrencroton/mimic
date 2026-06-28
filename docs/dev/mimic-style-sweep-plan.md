# Mimic Style Sweep Plan

**Status:** Active pre-v1.0 quality plan.
**Date:** 2026-06-27

**Purpose:** organise the repository-wide style sweep into manageable, ownership-based chats. Each chat should apply `docs/STYLE-GUIDE.md`, keep scientific behavior unchanged, fix style issues in scope, and avoid unrelated whole-repo cleanup.

## Working Rule for Each Sweep Chat

Each chat should:

- Review only the listed paths unless a directly related helper must be checked.
- Read the **example files** for the batch first — these are Mimic's strongest existing patterns in that area and should model the standard to apply.
- Apply `docs/STYLE-GUIDE.md` to comments, naming, documentation, metadata, tests, logging, generated-code boundaries, and local organisation. Light touch: fix comments, docstrings, YAML descriptions, README accuracy, log message clarity, and naming inconsistencies. Do not restructure code, rename functions, or change scientific behaviour.
- Stay within the batch paths; note any out-of-scope findings in the summary instead of fixing them.
- Run `./scripts/beautify.sh` before finishing.
- Run the narrowest relevant validation/test command for the batch (listed under each batch below).
- **Before the final commit**, update this plan file — append the following to the batch section:

```
Status: ✓ Complete — YYYY-MM-DD
Style debt: <bullet list of unresolved issues with file path and reason, or "none">
```

Include the plan update in the same commit as the style changes.

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

---

### 1. Baseline and Guardrails

Scope: `docs/STYLE-GUIDE.md`, `AGENTS.md`, formatter config (`.clang-format`, `pyproject.toml`), generated-code rules.
Approx: 4 files.
Goal: confirm the rules are clear and self-consistent before applying them to code. This batch does not sweep code — it verifies and tightens the rule documents themselves.
Examples: `docs/STYLE-GUIDE.md` is the reference; `.clang-format` and `pyproject.toml` show the mechanical style configuration.
Run: `make check-docs`.

Status: ✓ Complete — 2026-06-27
Style debt: none

---

### 2. Core Execution

Scope: `src/core/`, `src/include/`.
Approx: 22 files.
Focus: comments, ownership/lifetime docs, logging, fatal/error paths, generated boundaries, naming consistency.
Examples:
- `src/core/build_model.c` — gold standard for a core .c file: Doxygen header with key-functions list and explicit ownership statement; section banners where needed.
- `src/io/tree/interface.h` — compact per-function Doxygen (`@brief` only when the name is not self-explanatory; no prose padding).
Run: `make`, targeted unit tests touching config/core if changed.

Status: ✓ Complete — 2026-06-27
Style debt:
- `src/core/module_registry.c`: remaining `// Enable verbose formatting` and `// Enable debug level logging` comments in `parse_cli()` explain obvious behaviour; the block was in `src/core/main.c`, where the comments have been removed. ✓ resolved — 2026-06-27
- `src/core/module_registry.h` and `module_registry.c`: `register_all_modules()` Doxygen says "Implementation: Auto-generated from module metadata" — true but slightly misleading (the function body in module_init.c is generated, not the declaration). The declaration now points to the generated module-init source without implying the header is generated. ✓ resolved — 2026-06-27
- `src/include/types.h`: inline `// Flag:` comments on remaining struct fields (e.g. `MaxTreeDepth`, `ProcessingOrder`, `ForestDistributionScheme`) were left in place because they carry non-obvious default values; no harm.

---

### 3. Utilities

Scope: `src/util/`.
Approx: 14 files.
Focus: allocator/error/logging contracts, public header comments, comment quality, memory ownership.
Examples:
- `src/util/error.h` — enum with per-value inline `/* ... */` descriptions; short function prototype block.
- `src/util/memory.h` — same enum-description pattern applied to `MemoryCategory`.
- `src/util/memory.c` — Doxygen header with key-functions list; tight section banner for global state (one or two lines explaining the non-obvious invariant, not a migration essay).
Run: relevant unit tests, especially memory/error/numeric tests.

Status: ✓ Complete — 2026-06-27
Style debt: none

---

### 4. Tree and Output I/O

Scope: `src/io/`.
Approx: 31 files.
Focus: file-format boundaries, I/O logging macros, error messages, reader/writer ownership, HDF5 guards.
Examples:
- `src/io/tree/reader.h` — architectural header: explains the two partition models (`PARTITION_PER_FILE`, `PARTITION_ENUMERATED`) in the file-level comment; enum values carry inline explanations; ownership is explicit.
- `src/io/output/hdf5.c` — file header states what the file *owns* (lifecycle, layout, schema source) rather than selling the format. Use this as the template for all output writer headers.
Run: `make`, output-format and tree-reader tests as relevant.

Status: ✓ Complete — 2026-06-27
Style debt:
- `src/io/tree/read_ctrees_ascii.c`: uses `fprintf(stderr, "Error: ...")` rather than Mimic logging macros — vendored pattern inherited from the Consistent-Trees parser; out-of-scope for a light-touch pass, flagged for a future ctrees-boundary audit.
- `src/io/output/hdf5.c`: `// Create datatypes for different size arrays` comment and `array3f_tid` variable in `calc_hdf5_props()` — mild describe-the-code noise; the comment now states that the type is shared by generated vector fields and closed during HDF5 cleanup. ✓ resolved — 2026-06-27

---

### 5. Module System Infrastructure

Scope: `src/module_system/`.
Approx: 28 files.
Focus: template quality, public contracts, test fixtures, event/module metadata boundaries.
Examples:
- `src/module_system/physical_constants.h` — constants file template: one constant per line, inline comment carries value/symbol/reference; section dividers for topic groups.
- `tests/framework/test_framework.h` — framework header template: Doxygen file header with usage example; per-macro `@def`/`@brief`; short WHY comments on non-obvious framework invariants.
Run: `make validate-modules`, module-system tests.

Status: ✓ Complete — 2026-06-27
Style debt:
- `src/module_system/test_fixture/_tests/test_integration_test_fixture.py`: does not emit `MIMIC_RESULT:` structured markers — uses raw `assert` with a catch/count pattern that predates the marker system. The file now uses `run_test_suite()` so each case emits one structured marker. ✓ resolved — 2026-06-27

---

### 6. Scripts and Generators

Scope: `scripts/`.
Approx: 11 files.
Focus: docstrings, CLI usage, path handling, error messages, generated-code source of truth.
Examples:
- `scripts/generate_properties.py` — module docstring template: one-line purpose, then `Usage:`, `Reads:`, `Generates:` sections; `REPO_ROOT`/`discovery` import pattern; `sys.exit(1)` on missing deps.
Run: `make validate-modules`, `make check-generated` if generator-related files change.

Status: ✓ Complete — 2026-06-27
Style debt:
- `scripts/beautify.sh`: used fixed `/tmp/black_errors.log` and `/tmp/isort_errors.log` paths for temp files; now uses `mktemp` plus `trap` cleanup. ✓ resolved — 2026-06-28
- `scripts/benchmark_mimic.sh`: inline Python heredoc for YAML parsing is complex but load-bearing; the script overall is well-structured and all major style nits are absent.

---

### 7. Top-Level Test Framework

Scope: `tests/framework/`, `tests/unit/`.
Approx: 30-ish files.
Focus: structured markers, skip reasons, test section comments, assertion clarity.
Examples:
- `tests/framework/test_framework.h` — framework Doxygen with usage example in the header; per-macro `@def`/`@brief`; WHY note on per-TU state.
- `models/sage16/modules/_tests/sage_test_fixtures.h` — shared fixture header: brief explaining the boilerplate contract, `SAGE_TEST_LOCAL_RESET_CONFIG` opt-out pattern, purpose boundary ("This header carries fixtures only: it must never weaken or absorb test assertions").
- `models/sage16/modules/sage_apply_cooling/_tests/test_unit_sage_apply_cooling.c` — unit test file template: Doxygen header listing all test cases; `SETUP` / `EXECUTE` / `VALIDATE` / `CLEANUP` section comments where they improve scanability.
Run: targeted C unit tests; full `make tests-unit summary` if changes are broad.

Status: ✓ Complete — 2026-06-27
Style debt:
- `test_module_configuration.c` — `main()` called `print_allocated()` directly instead of `check_memory_leaks()`; now uses the standard leak-check helper. ✓ resolved — 2026-06-28
- `test_ctrees_support.c` and `test_galaxy_id_encoding.c` — `@test` docblocks use inline `@test  description` format instead of the canonical `@test` + `@brief` on separate lines; functionally equivalent but subtly inconsistent with the reference pattern. ✓ resolved — 2026-06-28
- Several static helper functions in unit test files (`write_lhalo_binary_header`, `assert_sequence_halo`, `configure_driver_defaults`, etc.) have no Doxygen; omitted because the style guide restricts comments to non-obvious WHY, and these names are self-documenting.
- `test_module_configuration.c` — `set_test_fixture_params` helper has no `@brief`; left because it is file-internal and clearly named.

---

### 8. Integration and Scientific Tests

Scope: `tests/integration/`, `tests/scientific/`, `tests/README.md`.
Approx: 20-ish files.
Focus: marker helpers, `TestSkipped`, fixture cleanup, output capture, meaningful failure messages.
Examples:
- `tests/integration/test_full_pipeline.py` — module docstring states what the test validates; framework helpers imported by name; test function docstrings name the expectation and what is validated.
- `tests/scientific/test_scientific.py` — module docstring documents validation rules source (manifest path, how to regenerate); inline comments explain metadata-driven logic.
Run: targeted scripts; full `make tests-integration summary` only when needed.

Status: ✓ Complete — 2026-06-28
Style debt:
- `test_full_pipeline.py`: `test_stdout_content` asserts only that `"Mimic"` appears somewhere in combined output — very weak check. Strengthening it would require knowing stable log phrasing; left as a future functional improvement, not a style issue.
- `test_galaxy_major_loop.py`, `test_phase_execution.py`, `test_processing_modes.py`, `test_substeps.py`: use `# ===== SETUP/EXECUTE/VALIDATE/CLEANUP =====` section banners inside Python test functions — pattern is internally consistent across these four files; diverges from `test_full_pipeline.py` reference but is not wrong. Left in place.
- `test_module_pipeline.py`, `test_processing_order.py`: `TestSkipped` guards on `MIMIC_EXE.exists()` are in `main()` rather than per-test. Consistent with the local pattern; adding per-test guards is a functional change beyond the sweep scope.

---

### 9. SAGE Shared Helpers and Metadata

Scope: `models/sage16/shared/`, `models/sage16/model_properties.yaml`, `models/sage16/input/`, `models/sage16/README.md`.
Approx: 14 files.
Focus: property descriptions, units, sentinel comments, shared helper API comments, package docs.
Examples:
- `models/sage16/shared/metallicity.h` — shared helper header template: `@brief`, `@note` (SAGE parity inline), `@param`/`@return`, code example in `@code` block; single responsibility per helper.
- `simulations/mini-millennium/halo_properties.yaml` — YAML property template: `# --- topic ---` section dividers; every entry has `units`, `description`; inert fields carry `notes:` explaining why they are read-only; `h_convention` explicit where physical.
Run: `make MODEL=sage16 SIMULATION=mini-millennium validate-modules`.

Status: ✓ Complete — 2026-06-28
Style debt:
- `model_properties.yaml` `MergTime.units: Internal` — not a standard unit value; the field is internal and sentinel-heavy so "Internal" is intentional, but ideally would be `Gyr/h` with a note that the value range is dominated by sentinel protocol. Left because changing the unit value risks confusing the generator or the output schema for internal fields.
- `test_unit_metallicity.c` — `@test test_name` inline format (instead of `@test` + `@brief` on separate lines); now follows the canonical unit-test docblock form. ✓ resolved — 2026-06-28

---

### 10. SAGE Modules Batch A: Infall, Cooling, Reionization

Scope: `models/sage16/modules/sage_prepare_infall_budget/`, `models/sage16/modules/sage_apply_infall/`, `models/sage16/modules/sage_reionization/`, `models/sage16/modules/sage_calculate_cooling_budget/`, `models/sage16/modules/sage_apply_cooling/`, `models/sage16/modules/sage_radio_mode_heating/`.
Approx: 32 files.
Focus: module README versus `module_info.yaml`, lifecycle layout, ordering checks, SAGE parity comments.
Examples:
- `models/sage16/modules/sage_apply_cooling/sage_apply_cooling.c` — module .c template: brief Doxygen header; `HELPER FUNCTIONS` / `MODULE LIFECYCLE FUNCTIONS` section banners; WHY comments on sentinel handling (dT sentinel) and transport properties.
- `models/sage16/modules/sage_apply_cooling/README.md` — module README template: Processing Contract / Ordering / Properties / Parameters / Notes structure; ordering enforcements stated explicitly with consequence if violated.
- `models/sage16/modules/sage_apply_cooling/module_info.yaml` — module YAML template: explicit `parameters: []` when none; complete dependency listing.
Run: module-owned unit/integration tests touched.

Status: ✓ Complete — 2026-06-28
Style debt:
- `sage_prepare_infall_budget/README.md`: "Ordering" section added with soft note (no init() enforcement exists); could be strengthened by adding an init() check mirroring `sage_apply_infall`, but that is a functional change beyond sweep scope.
- `sage_reionization/README.md`: same — ordering note is soft guidance, not enforced.
- `sage_radio_mode_heating/README.md`: missing "Ordering" section — soft ordering guidance now records the budget dependency without implying runtime enforcement. ✓ resolved — 2026-06-28
- `sage_calculate_cooling_budget/_tests/test_unit_sage_calculate_cooling_budget.c`: `test_module_registration` function uses a different structure (runs `init` inside the test rather than using `TEST_SKIP_WITH` guard) — pattern is load-bearing for the test logic; not a style issue to resolve without changing behavior.

---

### 11. SAGE Modules Batch B: Star Formation, Supernova, Metals

Scope: `models/sage16/modules/sage_calculate_star_formation/`, `models/sage16/modules/sage_calculate_supernova_feedback/`, `models/sage16/modules/sage_apply_star_formation_supernova/`, `models/sage16/modules/sage_apply_metal_enrichment/`.
Approx: 18 files.
Focus: scientific comments, transport properties, parameter validation, no obvious-comment noise.
Examples: same as Batch 10 (`sage_apply_cooling/` directory). For batch B the key discipline is that comments on physical quantities explain the formula or assumption, not just the variable name.
Run: module-owned tests.

Status: ✓ Complete — 2026-06-28
Style debt:
- `sage_apply_metal_enrichment/module_info.yaml`: `tests.unit` and `tests.integration` are blank (null) — no test files exist for this module. Left as-is; adding tests would be a functional change beyond sweep scope.
- `sage_calculate_supernova_feedback/_tests/test_unit_sage_calculate_supernova_feedback.c`: file-header Doxygen uses markdown `**bold**` within C comment blocks — renders in some Doxygen toolchains but is non-standard; left in place as it is consistent within the file and harmless. ✓ resolved — 2026-06-28

---

### 12. SAGE Modules Batch C: Mergers, Satellites, Disk/AGN

Scope: `models/sage16/modules/sage_initialise_merger_clock/`, `models/sage16/modules/sage_resolve_mergers_and_disruption/`, `models/sage16/modules/sage_satellite_stripping/`, `models/sage16/modules/sage_disk_instability/`, `models/sage16/modules/sage_quasar_mode/`, `models/sage16/modules/sage_starburst_feedback/`, `models/sage16/modules/sage_reincorporation/`, `models/sage16/modules/sage_set_disk_scale_radius/`.
Approx: 41 files.
Focus: event/ordering docs, lifecycle consistency, helper extraction, README contracts.
Examples:
- `models/sage16/modules/sage_resolve_mergers_and_disruption/sage_resolve_mergers_and_disruption.c` — event-emitting module: parameter loading with `LOAD_AND_VALIDATE_RANGE_*`; ordering check in `init()` with a detailed `ERROR_LOG` message that explains the consequence of the missing module; static action-hook pattern.
Run: module-owned tests plus merger/event tests if touched.

Status: ✓ Complete — 2026-06-28
Style debt:
- `test_integration_sage_initialise_merger_clock.py` — verbose per-function docstrings (`Expected:` / `Validates:` boilerplate) left as-is; not incorrect, low priority. ✓ resolved — 2026-06-28
- `models/sage16/modules/sage_resolve_mergers_and_disruption/sage_merger_ops.h` — out of scope for per-file sweep (shared header with no issues), no changes needed.

---

### 13. SAGE Cross-Module Tests

Scope: `models/sage16/modules/_tests/`.
Approx: 8 files.
Focus: processing-mode contract tests, scientific baseline clarity, marker/skip behavior.
Examples:
- `models/sage16/modules/_tests/test_unit_sage_dependency_contracts.c` — cross-module test template: section banner with naming convention and ERROR vs WARNING test semantics documented; `@test`/`@brief` per test function; helper with `@brief` and truncation note.
- `models/sage16/modules/_tests/sage_test_fixtures.h` — see Batch 7.
Run: targeted files or SAGE package test tier.

Status: ✓ Complete — 2026-06-28
Style debt:
- `sage_test_fixtures.h`: no issues found; `set_test_model_parameters()` block comment is 3 lines (above the one-line guideline) but carries meaningful contract information (model-owned, scope boundary) — left in place.
- `test_integration_processing_mode_contracts.py`: uses raw `assert` inside `assert_invalid_mode_rejected()` helper rather than framework `result_fail()` — correct pattern for a helper that raises AssertionError caught by the outer loop; not a style issue.

---

### 14. Plotting System

Scope: `plot/mimic-plot/`, `models/*/plots/`.
Approx: 52 files.
Best split into two chats if needed: plotting engine first, figure modules second.
Focus: docstrings, validation messages, `(plot_path, skip_message)` convention, duplicated plot boilerplate.
Examples:
- `plot/mimic-plot/mimic-plot.py` — Python script docstring template: full CLI usage at the top (options table with defaults); structured option sections; `HDF5_AVAILABLE`/`SAGE_NATIVE_AVAILABLE` guarded-import pattern.
Run: plotting unit tests and selected plot generation.

Status: ✓ Complete — 2026-06-28
Style debt:
- `plot/mimic-plot/tests/test_sage_native_hdf5.py` — uses `unittest.main()` instead of `run_test_suite()`; does not emit `MIMIC_RESULT:` markers; should be migrated to the framework pattern before v1.0.

---

### 15. SHAM and Halos-Only Packages

Scope: `models/sham/`, `models/halos-only/`.
Approx: 38 files.
Focus: consistency with SAGE package conventions without overfitting to SAGE physics.
Examples:
- `models/sham/modules/sham_assign_stellar_mass/sham_assign_stellar_mass.c` — brief module header (references only); static parameter variables with `init()` loading; inline comment on non-obvious RNG algorithm (`splitmix64`).
- For README and module_info.yaml: use `models/sage16/modules/sage_apply_cooling/` as the structural reference.
Run: package-specific validation/tests where available.

Status: ✓ Complete — 2026-06-28
Style debt:
- `models/halos-only/modules/_tests/test_integration_halos_only_package.py`: `test_runtime_reports_physics_free_mode` fails when the executable is built with `MODEL=halos-only` because `create_test_param_file` generates a sage16-based run file. Pre-existing failure, unrelated to this sweep; needs `create_test_param_file` to accept a model override parameter.
- `models/sham/model_properties.yaml` `ShamOrphanAge.units: Myr/h` — may be incorrect (orphan age is accumulated from `dT * UnitTime_in_s / SEC_PER_MEGAYEAR` which yields physical Myr, but `UnitTime_in_s` carries h-convention from `Mpc/h / (km/s)`); left in place pending a formal unit contract review.
- Several plot figure files (`halo_mass_function.py`, `hmf_evolution.py`, `spin_distribution.py`, `velocity_distribution.py`, `spatial_distribution.py`) are duplicated verbatim between `models/sham/plots/figures/` and `models/halos-only/plots/figures/` — deduplication would require a shared model-neutral plot library, which is a structural change beyond sweep scope.

---

### 16. Simulation Packages

Scope: `simulations/mini-millennium/`, `simulations/millennium/`.
Approx: 19 files.
Focus: halo property metadata, units, snapshot/profile docs, simulation-owned tests.
Examples:
- `simulations/mini-millennium/halo_properties.yaml` — the canonical reference for halo property YAML: `# --- topic ---` section dividers, `units`, `h_convention`, `provides_core_role`, `notes` on inert fields, `description` for every entry.
Run: simulation-local tests and metadata validation.

---

### 17. Project Documentation Final Pass

Scope: `README.md`, `docs/`, `tests/README.md`, package READMEs touched during prior sweeps.
Focus: stale references, duplicated generated lists, links, consistency with style guide.
Examples:
- `docs/VISION.md` — project doc template: short purpose statement, ToC, principle-per-section structure with bold principle name followed by requirements and an "In practice" note.
- `models/sage16/modules/sage_apply_cooling/README.md` — module README template: Processing Contract / Ordering / Properties / Parameters / Notes.
Run: `make check-docs`.

---

### 18. Final Integration Pass

Scope: repo-level status after all sweep batches.
Goal: confirm the sweep is complete and consistent before tagging v1.0.
Run: `./scripts/beautify.sh`, `make check-format`, `make check-docs`, `make validate-modules`, `make check-generated`, then `make tests summary`.

---

## Code Review Checkpoints

After every group of batches, run a code review checkpoint on the accumulated diff of `style/pre-v1.0-sweep` against `main`. This verifies consistency and drift before the sweep continues, and clears accumulated style debt from completed batch sections.

| # | After batch | Group swept | Review focus |
| ---: | ---: | --- | --- |
| 1 | 5 | All of `src/` | Comment quality, Doxygen contracts, enum descriptions, section banners |
| 2 | 9 | Scripts, tests, SAGE shared/metadata | Python docstrings, YAML descriptions, marker discipline |
| 3 | 13 | All SAGE modules | Module README/yaml/c consistency, ordering comment quality |
| 4 | 17 | Plotting, SHAM, simulations, docs | Package conventions vs SAGE reference, doc accuracy |
| 5 | 18 | Final integration | Full gate before main merge |

---

## Sweep Prompt Template

Open a new Sonnet chat for each batch. Replace `[N]` with the batch number.

```
Batch: [paste number and name here, e.g. "2. Core Execution"]

You are applying the pre-v1.0 style sweep to Mimic — the batch named above.

Read before starting (in order):
1. `docs/VISION.md` — one pass to anchor architecture ownership in mind
2. `docs/STYLE-GUIDE.md` — full read; this is the rule set
3. `docs/dev/mimic-style-sweep-plan.md` — find the section for this batch; read scope, focus, and example files

Branch: `style/pre-v1.0-sweep`. Create off `main` if it doesn't exist; commit to it.

Scope: paths listed for this batch in the plan. Read the example files listed there before touching anything else.

Pass: light touch only. Fix comments, docstrings, YAML descriptions, README accuracy, log message clarity, and naming inconsistencies in scope. Do not restructure code, rename functions, or change scientific behaviour. Stay within the batch paths; note any out-of-scope findings in your summary instead of fixing them.

Run (in order):
1. `./scripts/beautify.sh`
2. The test/validation command listed for this batch in the plan.

Summary: list every file changed, what was fixed, and any style debt deliberately left unresolved.

Update plan: append the following to this batch's section in `docs/dev/mimic-style-sweep-plan.md`:

  Status: ✓ Complete — YYYY-MM-DD
  Style debt: <bullet list of unresolved issues with file path and reason, or "none">

Commit everything — style changes and plan update together — to `style/pre-v1.0-sweep`, with a message listing changed files grouped by type.
```

## Code Review Prompt Template

Open a new Sonnet chat at each checkpoint. Paste the checkpoint label from the `#` column of the table above, e.g. `"1 — after Batch 5: All of src/"`.

```
Checkpoint: [paste label, e.g. "1 — after Batch 5: All of src/"]

You are running a style-sweep code review checkpoint for Mimic — the checkpoint named above.

Read before starting:
1. `docs/dev/mimic-style-sweep-plan.md` — find this checkpoint in the Code Review Checkpoints table; note which batches are covered and the review focus. Then read every covered batch section and collect all Style debt entries into a working list.
2. `docs/STYLE-GUIDE.md` — refresh the rule set.

Perform a /code-review of the accumulated diff on `style/pre-v1.0-sweep` against `main`, scoped to commits from the batches in this checkpoint group. Apply the review focus from the checkpoints table.

After the review:
1. Work through the style debt list — fix straightforward items clearly within the covered batch paths. Note any that need larger changes or belong to a future batch.
2. If any files were changed, run `./scripts/beautify.sh` and the relevant narrow test command for the affected area.
3. Run `make tests summary` to confirm no regressions.

Update plan: in each covered batch section, mark resolved debt items with "✓ resolved — YYYY-MM-DD" inline. Leave unresolved items unchanged; add a brief note if the reason has changed.

Commit any fixes and the plan update together to `style/pre-v1.0-sweep`, listing changed files grouped by type.
```

---

## Notes

- The largest single area is `models/sage16/modules/`, so keep it split into multiple physics-themed batches.
- The core infrastructure batches should happen before model-package sweeps, so module changes can align with settled project conventions.
- The plotting system can be split if one chat gets too large.
- Use focused edits. The goal is a professional, consistent codebase, not style churn for its own sake.
- The per-batch validation commands are narrow by design. Reserve `make clean && make tests` for code review checkpoints.
