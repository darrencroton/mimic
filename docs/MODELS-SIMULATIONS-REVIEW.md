# Mimic `models/` & `simulations/` Systems Review (code-simplifier)

**Date**: 2026-06-11
**Scope**: Every hand-written file under `models/` (sage16: 19 physics modules + shared headers + metadata + module tests + plots; sham: 1 module + plots) and `simulations/` (mini-millennium, millennium). Generated files and `plot/mimic-plot/` library code are context only.
**Method**: Holistic, system-by-system code-level review against `docs/VISION.md`, mirroring the `src/` review implemented in commit `d6d0187` (`git show d6d0187:docs/SRC-SYSTEMS-REVIEW.md`). Goal: simplification, clarity, and maintainability with **no behavior change**, enforced by the sage16 full-physics byte-identical baseline (`test_scientific_sage_physics_baseline.py`). All dead-code claims were verified by repo-wide grep; the unused-import claims were verified by AST scan plus manual spot-check.
**Status**: PROPOSED. Roadmap in §6; per-batch commits with user approval.

**Standing decisions** (confirmed with the user before this review):

1. **Packages stay self-contained.** The 9 byte-identical figure files shared by sage16/sham (~2,200 lines) and the near-identical test/metadata pairs in the two simulation packages (~1,600 lines) are intentional per VISION.md. They are documented in §5 (What Not to Change) and edited only in mirror.
2. **Per-batch commits**, each verified against the full test suite before the user approves the commit.
3. **Tests are in scope** for boilerplate reduction, but coverage and assertions are never weakened.

---

## 1. System Map

| # | System | Files |
|---|---|---|
| M1 | sage16 physics modules | `models/sage16/modules/sage_*/sage_*.c`, `cooling_tables.c/.h`, `sage_merger_ops.h` (~2,930 lines) |
| M2 | sage16 shared physics headers | `models/sage16/shared/*.h` (7 headers, ~640 lines) |
| M3 | sage16 metadata & inputs | `model_properties.yaml` (414 lines), 19× `module_info.yaml`, `shared/module_info.yaml`, `input/*.yaml`, READMEs |
| M4 | sage16 module tests | `modules/*/_tests/*.c/.py`, `modules/_tests/*`, `shared/_tests/*` (~16,000 lines C + integration Python; includes the byte-gate baseline) |
| M5 | sage16 plots | `plots/figures/*.py` (22 figures + `__init__.py` registry, 268 lines), `plots/profiles/*.yaml` |
| M6 | sham package | `models/sham/` (1 module 215 lines, `model_properties.yaml` 129 lines, 11 figures + registry, 1 input YAML) |
| M7 | simulation packages | `simulations/{mini-millennium,millennium}/` (simulation_info, halo_properties, a_list, plot_profile, manifest, `_tests/unit` 316 lines each + `_tests/integration` ~2,200 lines each) |

---

## 2. Bugs Found During Review

**B1 — Wrong printf length modifier for `HaloNr` (undefined behavior on log paths).**
`struct Halo.HaloNr` is `int` (`src/include/generated/property_defs.h:26`), but `sage_resolve_mergers_and_disruption.c:101,109,132` format it with `%lld` (`"Invalid immediate-merger dt for halo %lld"`, `"Satellite %lld has unset MergTime"`, `"Satellite %lld has non-finite MergTime"`). Reading a `long long` from an `int` vararg is undefined behavior; on AArch64 it prints garbage upper bits at best. Every other module logs `HaloNr` with `%d` (e.g. `sage_initialise_merger_clock.c:59`). Fix: `%d`. *Why the compiler missed it*: `log_message` (`src/util/error.h:44`) lacks `__attribute__((format(printf, ...)))`, so `-Wformat` never sees these strings — adding the attribute is a small src-side follow-up outside this review's scope, flagged separately.

**B2 — Cooling-table metallicity array corrupted by re-initialization.**
`cooling_tables.c:75-77` converts `metallicities[]` from [Fe/H] to absolute log Z by mutating the static array in place (`metallicities[i] += log10(0.02)`). `cooling_tables_cleanup()` (line 147) resets only `tables_initialized`, so any init → cleanup → init cycle shifts the array a second time, silently corrupting all subsequent metallicity interpolation brackets. This cycle never happens in a production run (one init per process) — the byte gate is unaffected — but the unit tests run exactly this cycle repeatedly in one process (`test_unit_sage_calculate_cooling_budget.c` calls `cooling_tables_cleanup()` seven times), so every test after the first interpolates against a double-shifted table. Fix: convert into a separate `static double metallicities_logz[8]` at init (or guard the conversion with its own once-flag), keeping the source array immutable.

**B3 — Satellite stripping metal accounting asymmetry (verify against SAGE; default: defer).**
`sage_satellite_stripping.c:84-98`: `strippedMetals` is computed from the *unclamped* `strippedGas`, then both are clamped independently (gas to `HotGas`, metals to `MetalsHotGas`); the satellite then loses the clamped `strippedMetals` while the central gains `strippedGas * metallicity` recomputed from the *clamped* gas. When either clamp engages, the satellite's metal loss and the central's metal gain differ — metals are created or destroyed. The surrounding comments claim SAGE parity for the float/double discipline; whether SAGE's `strip_from_satellite` has the same asymmetry must be checked against `sage-model` before touching this, because a "fix" would break the byte gate. **Action: document now, verify against SAGE, fix only with user approval + baseline regeneration.**

**B4 — Inconsistent central-galaxy contract (latent crash path or dead defensiveness — pick one).**
`sage_apply_star_formation_supernova.c:100` and `sage_apply_metal_enrichment.c:88` dereference `ctx->central_galaxy->galaxy` unconditionally; `sage_calculate_supernova_feedback.c:117` dereferences `ctx->central_galaxy->Vvir` unconditionally. Meanwhile `sage_satellite_stripping.c:62-65` and `sage_starburst_feedback.c:205-211` treat `ctx->central_galaxy == NULL` as a live, handled case. Either the pipeline guarantees a non-NULL central for by-galaxy dispatch (then the checks are dead weight and the contract should be stated once in `module_interface.h` docs) or it doesn't (then three modules can crash). Resolution belongs with the core contract documentation; the conservative behavior-preserving move is to align the three unchecked modules with the checked style.

---

## 3. Per-System Review

### M1. sage16 Physics Modules

What is good: the calculate/apply split is consistently executed; SAGE-parity decisions are documented at the point of divergence risk (e.g. the orphan-cooling rationale in `sage_calculate_cooling_budget.c:112-116`, the rate-denominator explanation in `sage_starburst_feedback.c:225-229`, the Hubble-units trap in `sage_reionization.c:79-81`, the 1.414 literal in `sage_set_disk_scale_radius.c:45-48`); dependency checks in `init()` catch mis-wired pipelines with actionable messages; `LOAD_AND_VALIDATE_*` use is uniform; `module_info.yaml` parameter declarations exactly match the `LOAD_AND_VALIDATE` calls in all 19 modules (verified mechanically).

**M1-1. Hand-rolled find-central loops where a shared helper exists.** `sage_prepare_infall_budget.c:131-137` and `sage_apply_infall.c:46-52` re-implement the Type-0 scan that `mimic_find_fof_central_index()` (`shared/central_link.h:9`) already provides and that the merger modules already use. Replace both loops with the helper.

**M1-2. `sage_reionization.c:124-139` — two identical branches.** The Type 0 and non-Type-0 branches compute exactly the same thing (`calculate_reionization_modifier(ctx, halos[i].Mvir)` × `GLOBAL_BARYON_FRAC`); only the `DEBUG_LOG` text differs. Collapse to one branch with one log line.

**M1-3. Missing-central severity is inconsistent.** Same condition, three responses: `sage_prepare_infall_budget.c:140` logs `ERROR_LOG` and returns 0 (error level, success code); `sage_apply_infall.c:55` logs `DEBUG_LOG`; `sage_initialise_merger_clock.c:45-47` returns silently. Pick one convention (silent return 0 with a one-line comment matches the "absent central is a legal no-op" semantics the merger modules already use) and apply it.

**M1-4. The `ngal != 1` / NULL-galaxy preamble repeats in 12 by-galaxy modules.** ~10 lines each (`if (ngal != 1) ERROR_LOG(...); return -1; ... if (halo->galaxy == NULL) return 0;`). A `static inline` guard in a shared header could centralize it, but each occurrence carries a module-specific SAGE-parity comment for *why* NULL/Type-2 is skipped, and those comments are the valuable part. Recommendation: extract only the mechanical `ngal != 1` check into a one-line helper (or leave entirely); do not collapse the NULL-galaxy comments into a generic macro.

**M1-5. Cross-file magic numbers that repeat should be named once.** Three families genuinely repeat across files and should move to a small `shared/sage_constants.h` (values unchanged, byte gate green):
- the MergTime sentinel protocol: `999.9` (unset, also `model_properties.yaml:373`), `> 999.0` / `<= 999.0` (tests), `998.0` (ceiling), `-1.0` (immediate) — spread over `sage_initialise_merger_clock.c:58-122`, `sage_resolve_mergers_and_disruption.c:108`, and the property file, with no single statement of the protocol;
- the metal-ejection scale `30.0` and cold-gas threshold `1.0e-8`, duplicated between `sage_apply_metal_enrichment.c:101-103` and `shared/sage_starburst_physics.h:126-128`;
- the virial-temperature coefficient `35.9` (K per (km/s)²), duplicated between `sage_calculate_cooling_budget.c:41` and `sage_radio_mode_heating.c:69`.
Single-site literals that already carry why-comments (Eddington constants at `sage_radio_mode_heating.c:147-166`, Gnedin/Kravtsov constants at `sage_reionization.c:37-68`, Somerville `0.56/0.7`, Kauffmann `0.19`, BH-suppression `280.0`) should stay as they are — naming them adds indirection without adding information.

**M1-6. `MinNumPartSatHalo = 10`** (`sage_initialise_merger_clock.c:91`) is a scientific threshold buried as a function-local; it belongs with the other named constants (M1-5) or at file scope with its SAGE-parity comment.

**M1-7. Stale documentation references.** Three modules cite "§7 of SAGE-MODULE-REVIEW.md" (`sage_calculate_supernova_feedback.c:47`, `sage_apply_star_formation_supernova.c:41`, `sage_starburst_feedback.c:103`) — that document was archived in commit `e720275`. `sage_apply_cooling.c:6` says "calculated by sage_calculate_cooling" (module is `sage_calculate_cooling_budget`); `sage_disk_instability.c:18` names a module "sage_collisional_starburst" that doesn't exist (it is `sage_starburst_feedback`). Fix the names, drop the dead citations (the in-code explanations are self-sufficient).

**M1-8. Unused includes.** `sage_prepare_infall_budget.c:14-15`, `sage_reionization.c:13-14`, `sage_satellite_stripping.c:12-13`, and `sage_radio_mode_heating.c:12-13` include `<stdio.h>`/`<stdlib.h>` without using them; `sage_apply_cooling.c:14` includes `module_system/physical_constants.h` without using any constant. Sweep once with a compile check per removal.

**M1-9. Cleanup-log levels are arbitrary.** `cleanup()` uses `VERBOSE_LOG` in nine modules, `INFO_LOG` in five, and nothing in two. Pick `VERBOSE_LOG` (cleanup is not a milestone) and align.

**M1-10. Checked and deliberately left alone:** the cooling-energy expression in `sage_apply_cooling.c:47` and the heating-energy expression in `sage_radio_mode_heating.c:193` look like duplicates but accumulate different properties with different gating — a shared helper would couple two distinct diagnostics for a one-line saving. The `record_action` hook in the merger resolver (`:29-42`) is a test seam, not dead code (used by the ordering-parity test).

### M2. sage16 Shared Physics Headers

What is good: this is the strongest code in the model package. `central_link.h` and `time_parity.h` encode genuinely subtle SAGE-parity rules once with clear contracts; `sage_starburst_physics.h:11-15` explicitly justifies its single-consumer placement; the quasar-wind unit comment (`sage_agn_physics.h:79-83`) documents a previously fixed sev-5 bug at the exact point someone might "simplify" it back in. **The parity report's top finding (quasar wind `C_KM_S` bug) is already fixed** — `docs/MIMIC-SAGE-PARITY-ANALYSIS.md` is stale on this point.

**M2-1. `shared/sage_merger_event_contract.h` is dead.** Self-declared DEPRECATED, and a repo-wide grep finds zero `#include` or use of `SageEventCode`/`SAGE_EVENT_MERGER` in code (the only textual mentions are two input-YAML comments, see M3-4). The payload contract it documents (value0 = mass ratio, value1 = source dt) is the one piece worth keeping — move those two lines into `sage_merger_ops.h` or the producer's `module_info.yaml`, then archive the header.

**M2-2. `metallicity.h:5-7` names modules that no longer exist** ("sage_calculate_infall, sage_calculate_cooling"). Update to current names or drop the enumeration (it will rot again).

**M2-3. `shared/module_info.yaml` comments reference `make MODEL=sage`** (lines 8-10) — the model is `sage16`. Also `make MODEL=sage generate-test-registry` is not a real target spelling.

### M3. sage16 Metadata & Inputs

What is good: `model_properties.yaml` is exemplary — every transport property documents its writer/reader chain in a `role: transport` comment, and every SAGE float/double-precision decision is justified inline; this file is the model's de-facto dataflow documentation.

**M3-1. `model_properties.yaml:23` contradicts itself**: `sentinels: [-1.0]  # No special sentinels - should always be set by reionization module`. The sentinel is real (`init_value: -1.0`, tested by `sage_prepare_infall_budget.c:150`). Fix the comment.

**M3-2. The MergTime sentinel protocol is undocumented at its source.** `model_properties.yaml:367-374` gives `init_value: 999.9` and `range: [0.0, 1000.0]` with no mention that >999 means unset, 998 is the ceiling, and -1 forces immediate merger. One comment block here (plus the named constants of M1-5) makes the protocol discoverable.

**M3-3. `module_info.yaml` schema drift vs sham.** sham's module declares `display_name` and `version`; no sage16 module does. Both fields are optional with auto-generated defaults (`scripts/validate_modules.py:172-173,267-268`), and sham's explicit values are exactly the defaults. Drop them from sham (M6) rather than adding 19× two lines of redundancy to sage16.

**M3-4. Input YAML comment names a dead identifier.** `sage16_mini-millennium.yaml:66` and `sage16_millennium.yaml:70`: "emits SAGE_EVENT_MERGER" — the generated, producer-scoped name is `SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER`; say "emits its merger event" and let the module_info contract be the reference.

**M3-5. `sage16_millennium.yaml` declares `first_file: 0 / last_file: 15`** while `simulations/millennium/simulation_info.yaml` declares 0-511. Probably a deliberate local-subset override (tree data is symlinked, not bundled), but it deserves a one-line comment saying so — to a new user it reads like an error. **Confirm intent with the user before annotating.**

### M4. sage16 Module Tests

What is good: coverage is deep (every module has unit + integration tests; physics edge cases, conservation, error paths); the structured `MIMIC_RESULT:` marker protocol is followed; the model-level tests (`test_unit_sage_dependency_contracts.c`, `test_unit_mixed_dt_parity.c`, the physics baseline) test cross-module behavior that no per-module test could.

**M4-1. Fixture boilerplate is copy-pasted across all 17 C test files** (~16,000 lines total). Each file re-declares: the `passed/failed` counters, `reset_config()` (17×), `ensure_modules_registered()` + static flag (15×), `extern void set_test_model_parameters(void)` (10×), and a local `create_test_halo()`/`free_test_halo()` pair (per-file variants differing only in which fields the test cares about). Extract a header-only fixture file at `models/sage16/modules/_tests/sage_test_fixtures.h` providing `reset_config`, `ensure_modules_registered`, a parameterizable `create_test_halo` (taking type/mvir/vvir and zeroing the rest, as today), and `free_test_halo`; include it from each test. Estimated saving ~800–1,000 lines with zero assertion changes. The deep relative include paths (`../../../../tests/framework/...`) are ugly but are the framework's documented pattern — leave them.

**M4-2. `@author Mimic Development Team / @date 2025-12-18` header fields** add nothing (git knows); drop them during the M4 pass rather than as their own change.

**M4-3. Coverage gaps worth recording (additive work, not simplification):** `models/sham/modules/sham_assign_stellar_mass` declares no tests at all (`module_info.yaml:37-40`); of the shared headers only `metallicity.h` has a direct unit test (`shared/_tests/`), though `central_link`/`time_parity`/merger-ops are exercised heavily through module tests. A SHAM unit test mirroring the sage16 conventions is the single highest-value addition.

### M5. sage16 Plots

What is good: every figure is genuinely self-contained (constants, requirements, plot function); validation flows through the shared `output_utils` helpers consistently; the registry maps (`SNAPSHOT_PLOTS`, `PLOT_REQUIREMENTS`, `PLOT_FUNCS`) are easy to extend.

**M5-1. Unused imports in every figure file.** AST scan (manually spot-checked against `halo_mass_function.py`): all 22 sage16 figures and all 9 sham mirror figures carry 3–7 unused imports each — `matplotlib.pyplot as plt` (the figures use `setup_figure`/`save_and_close_figure` instead), `setup_plot_fonts`, `IN_FIGURE_TEXT_SIZE`, `LEGEND_FONT_SIZE`, and per-file stragglers (`check_field_has_values`, `validate_filtered_data`, `warn`, `MultipleLocator`, ...). ≈180 dead import lines across 31 files. Mechanical sweep; mirror-edit the 9 shared files so they stay byte-identical with sham.

**M5-2. `figures/__init__.py` has drifted between the packages in style only.** The sham registry (145 lines) is the cleaner rewrite of the same functions (direct returns, no redundant comment-plus-pseudo-docstring); the sage16 one (268 lines) retains stray string literals used as comments (lines 8, 180, 204, 215, 243 — bare `"""..."""` expressions that aren't docstrings) and `x_label = ...; return x_label` indirection. Align sage16's shared portion to sham's style; sage16 keeps its 13 extra label helpers and larger registries.

**M5-3. Observational data without provenance.** The Baldry+2008 array in `stellar_mass_function.py` (mirrored in sham) is 49 rows of literals with no version/source comment beyond the variable name. Add a one-line citation comment (table, units, IMF assumption). Check the other figures with embedded data arrays during the same pass.

**M5-4. Left alone deliberately:** per-figure `BINWIDTH_DEX`/axis-limit constants repeat across files but are part of each figure's self-contained contract — centralizing them couples figures for no behavioral gain (and Decision 1 applies to the sham mirrors).

### M6. sham Package

What is good: `sham_assign_stellar_mass.c` is the cleanest module in the repo — deterministic scatter via splitmix64 keyed on stable IDs, every numeric guard explicit, helpers small and single-purpose. `model_properties.yaml` is concise and consistent with the sage16 conventions.

**M6-1.** Drop the redundant `display_name`/`version` from `module_info.yaml` (see M3-3).
**M6-2.** Unused-import sweep + `__init__.py` style source of truth (see M5-1/M5-2).
**M6-3.** Test gap recorded as M4-3.

### M7. Simulation Packages

What is good: both packages carry the same structure on purpose; `simulations/millennium/_tests/input/test_simulation.yaml` documents *why* it reuses mini-Millennium data (CI speed without the production catalog) — exactly the right kind of comment.

**M7-1. The mirrors have drifted in documentation only** (verified by diff): `tree_loader.py` — the millennium copy lacks the mini copy's `Example:` docstring block (after line 98) and the two copies show different example paths (`:174,181`); the unit and integration tests differ only in simulation-name comment strings. Unify each pair to the better text where it is simulation-neutral; keep genuinely simulation-specific lines (box size in `test_satellite_spatial_distribution.py:35`) as they are.

**M7-2. State the mirror-maintenance rule where it will be seen.** Both package READMEs should carry one line: "The `_tests/` suites and `halo_properties.yaml` are intentional near-mirrors of the other LHaloTree package; apply changes to both." This converts implicit duplication into a documented contract (the compromise Decision 1 chose over shared infrastructure).

**M7-3. Nothing to do for hygiene**: no `.DS_Store`/`__pycache__`/`.pyc` files are tracked, and `.gitignore:47,50` covers them — the working-tree clutter is local-only.

---

## 4. Cross-Cutting Themes

1. **The parity-comment discipline is the asset to protect.** The most valuable lines in M1/M2 are the "SAGE parity: ..." comments at every deliberate divergence point. No simplification below may delete one; several findings (M1-7, M2-2) exist because a comment *pointed at the wrong name*, which is worse than no comment.
2. **Use the shared helpers that already exist** (M1-1). The shared directory solved find-central, target resolution, and dt-handling once; two older modules predate it and never migrated.
3. **Name only the constants that repeat across files** (M1-5/M1-6). This codebase's single-site literals are generally well-commented; the cross-file repeats (MergTime protocol, 30.0, 1e-8, 35.9) are the real maintenance hazard because they can drift independently.
4. **Stale references rot fastest** (M1-7, M2-2, M2-3, M3-4, M5-3 and the stale parity-report claim in M2). Where a comment must name another file/module, prefer naming the contract ("the merger event producer") over the identifier.
5. **The test suite's cost is in its fixtures, not its assertions** (M4-1). One header removes ~6% of the model package's total line count without touching a single check.
6. **`log_message` should gain the printf format attribute** (src follow-up from B1) — it would have caught B1 at compile time and protects every future module.

---

## 5. What Not to Change

- **Per-module parameter loading "duplication"** (`GlobalBaryonFraction` loaded by three modules, `FeedbackReheatingEpsilon` by two, etc.) is the VISION Principle 7 design: validation lives in each module's `init()` because only the module knows its constraints. Do not centralize.
- **The 9 byte-identical figure files and near-identical simulation test pairs** are the self-containment contract (Decision 1). Edit only in mirror; never half-edit one side.
- **`sage_starburst_physics.h` stays in `shared/` despite one consumer** — its header explains why (lines 11-15).
- **The 1.414 literal** (`sage_set_disk_scale_radius.c:45-49`) and other truncated SAGE literals are required for output parity; the comments say so.
- **`record_action` hook** in the merger resolver is a live test seam.
- **Conditional parameter loading in `sage_starburst_feedback_init`** (params loaded only when the relevant follow-up channel is configured) looks baroque but correctly avoids demanding parameters the pipeline doesn't use.
- **The cooling/heating energy expressions** (M1-10) are intentionally parallel, not duplicated.

---

## 6. Suggested Implementation Order

Each batch is independently shippable. After every batch: `./scripts/beautify.sh`; zero-warning builds for `MODEL=sage16` and `MODEL=sham` (spot-check `USE-HDF5=no` on C batches); `make validate-modules` + `make check-generated`; full `make tests summary` via subagent — the scientific tier's byte-identical physics baseline is the no-behavior-change proof. Plot batches additionally run the plotting test suites and a full plot generation for both models. Removed files go to `archive/removed-models/` / `archive/removed-simulations/`. One commit per batch, each with user approval.

| Batch | Content | Risk |
|---|---|---|
| 1. Bug fixes | B1 (`%lld`→`%d`), B2 (cooling-table conversion guard); B4 alignment (add the two missing NULL checks, conservative direction) | Low — local; byte gate must stay green |
| 2. Dead code & stale references | M2-1 archive dead header (+contract relocation), M1-7, M1-8, M2-2, M2-3, M3-4, M5-3 citation, stale parity-report note | Low — grep-verified |
| 3. Constants & sentinel protocol | M1-5, M1-6 `shared/sage_constants.h` (values bit-identical), M3-2 property-file protocol comment, M3-1 comment fix | Low — byte gate verifies |
| 4. M1 gas-cycle simplification | M1-1 helper adoption, M1-2 branch collapse, M1-3 severity alignment, M1-9 log levels | Low-medium — touches physics files; byte gate verifies |
| 5. M3/M6 metadata alignment | M3-3/M6-1 drop redundant fields, M3-5 input-file comment (after user confirms intent) | Low |
| 6. M4 test fixture extraction | M4-1 shared fixture header across 17 files, M4-2 header trim | Medium — wide but mechanical; all suites must stay green with identical counts |
| 7. Plots | M5-1 unused-import sweep (31 files, mirrored), M5-2 `__init__` style alignment | Low — plotting tests + full generation for both models |
| 8. Simulations polish | M7-1 docstring unification, M7-2 README mirror rule | Low |

**Deferred, pending decisions:** B3 (stripping metal asymmetry — verify against SAGE first; any fix needs explicit approval + baseline regeneration); M1-4 preamble helper (marginal value; revisit if a 20th module appears); the `log_message` format attribute (src change, separate task).

---

## 7. Implementation Addendum

*(Filled in as batches land, recording deviations discovered during implementation.)*

**Batch 2 (2026-06-11)** — Dead code & stale references, implemented as specified: archived `shared/sage_merger_event_contract.h` to the local `archive/` (its payload contract was already recorded in the producer's `module_info.yaml`, so no relocation was needed); removed unused `<stdio.h>`/`<stdlib.h>`/`<string.h>`/`physical_constants.h` includes from eight module files (verified symbol-free, then compile-checked in all three build configurations including `USE-HDF5=no`); fixed the three archived-doc citations, two renamed-module references, the `metallicity.h` module enumeration (replaced with a rot-proof description), the `MODEL=sage` spellings in `shared/module_info.yaml`, and the `SAGE_EVENT_MERGER` comments in both input YAMLs; added the Baldry, Glazebrook & Driver (2008) citation to `stellar_mass_function.py`, mirror-copied so the sage16 and sham files remain byte-identical. Verified: unit 32/32, integration, scientific incl. byte-identical baseline; zero-warning builds (sage16 HDF5, sage16 no-HDF5, sham); validate-modules and check-generated clean.

**Batch 1 (2026-06-11)** — B1, B2, B4 implemented as specified. One deviation: fixing B2 exposed that `test_unit_sage_calculate_cooling_budget.c` (`test_super_solar_metallicity`) had encoded the corruption — its "maximum table" probe `log10(0.063) ≈ -1.2007` sits just *below* the true top-table boundary `0.5 + log10(0.02) ≈ -1.19897` and only matched while repeated init/cleanup cycles shifted the boundary down to ≈ −4.6. The probe was corrected to the exact boundary value `0.5 + log10(0.02)` (assertion strengthened, not weakened). Verified: unit 32/32, integration suite, scientific suite incl. the byte-identical physics baseline (42 properties × 4,196 halos) all pass; zero-warning builds for `MODEL=sage16` and `MODEL=sham`; `validate-modules` and `check-generated` clean.
