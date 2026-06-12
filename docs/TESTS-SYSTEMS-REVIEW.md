# Mimic `tests/` & Testing-Framework Systems Review (code-simplifier)

**Date**: 2026-06-12
**Scope**: Every hand-written file in the testing framework: `tests/framework/` (incl. the three templates), `tests/unit/` (11 core C tests + `test_stubs.c` + `run_tests.sh`), `tests/integration/` (9 core Python tests), `tests/scientific/`, the test plumbing (Makefile test targets, `scripts/generate_test_registry.py`, `scripts/generate_test_inputs.py`, the `module_sources.mk` emission), `tests/data/` organization, `.github/workflows/ci.yml` (test invocation), and `plot/mimic-plot/tests/` (~10,300 lines of test source plus ~1,500 lines of plumbing). Module-local tests under `models/` and `simulations/` were reviewed in the M4/M7 pass and are treated as **consumers only**: they are touched here only where a framework change ripples into them.
**Method**: Holistic, system-by-system code-level review against `docs/VISION.md` (especially Principle 7: invalid states fail early; failing tests are real problems), mirroring the implemented `src/` review (`archive/SRC-SYSTEMS-REVIEW.md`, commit `d6d0187`) and `models/`+`simulations/` review (`archive/MODELS-SIMULATIONS-REVIEW.md`). Goal: simplification, clarity, and maintainability with **no behavior change** and **no weakening of coverage or assertions** (strengthening is allowed and several findings do exactly that). All dead-code and duplication claims were verified by repo-wide grep; duplicate-function claims by content hash.
**Status**: REVIEW COMPLETE — implementation pending (§6 batches, one user-approved commit each).

**Standing decisions** (confirmed with the user before this review):

1. **Plotting tests are in scope** (`plot/mimic-plot/tests/` + `test_plotting.sh`); their divergence from the marker protocol is itself review material.
2. **Module-local tests are consumers only** — no content re-review; mirror-edits only where a framework API they use changes.
3. **Coverage gaps get an additive batch** (new tests only), including the open M4-3 SHAM unit test.
4. Inherited: per-batch commits with full-suite verification and user approval; removed files go to `archive/`; `./scripts/beautify.sh` before every commit.
5. **The framework serves future models, not only sage16/sham** (added 2026-06-12). Per VISION.md, new physics model packages must be able to bring their own tests on the existing rails. Findings were re-checked through this lens: the framework must be model-count-agnostic, but speculative "what if" generality is explicitly out of scope.

---

## 1. System Map

| # | System | Files (lines) |
|---|---|---|
| T1 | Framework core | `tests/framework/test_framework.h` (239), `markers.py` (60), `parity_trace.h` (89), `test_phase_config.h` (85), `__init__.py` (84) |
| T2 | Python harness & data loading | `tests/framework/harness.py` (664), `data_loader.py` (221) — plus the framework-grade helpers currently trapped in `test_output_formats.py` |
| T3 | Templates | `c_unit_test_template.c` (482), `python_integration_test_template.py` (491), `python_scientific_test_template.py` (328) |
| T4 | Core C unit tests | 11 × `tests/unit/test_*.c` (~2,780) + `test_stubs.c` (101) |
| T5 | Unit runner | `tests/unit/run_tests.sh` (368) |
| T6 | Core integration tests | 9 × `tests/integration/test_*.py` (~4,230; `test_output_formats.py` alone 1,191) |
| T7 | Scientific tier | `tests/scientific/test_scientific.py` (689) + `tests/generated/property_ranges.json` (build product) |
| T8 | Plumbing | Makefile test targets + summary filter + `TEST_BUILD` machinery, `scripts/generate_test_registry.py` (301), `scripts/generate_test_inputs.py` (252), `tests/generated/module_sources.mk` emission, `.github/workflows/ci.yml` (81) |
| T9 | Test data & baselines | `tests/data/` (18 MB tree fixture; committed baselines under `output/baseline/{binary,hdf5}` with bundled metadata; gitignored regenerated output) |
| T10 | Plotting tests | `plot/mimic-plot/tests/` (4 Python files, 710 lines) + `test_plotting.sh` (99) |

63 registered tests (31 unit, 30 integration, 2 scientific) flow through one pipeline: `generate_test_registry.py` discovers core + selected-simulation + module-declared tests into `build/generated/*_tests.txt`; `run_tests.sh` compiles/runs C tests; the Makefile `RUN_PYTHON_TEST_REGISTRY` macro runs Python tests; every test emits `MIMIC_RESULT:` markers that the summary filter greps exactly.

---

## 2. Bugs Found During Review

These are correctness issues in the *testing system itself* — checks that cannot fail, or scaffolding that produces broken output.

**B1 — `check_no_memory_leaks()` is vacuous: it scans log files that no Mimic run ever writes.**
`harness.py:599-645` globs `output_dir/metadata/*.log`, but nothing in `src/` writes `.log` files anywhere (verified by grep; the metadata dir contains only YAML/JSON, and the allocator's leak report — `WARNING_LOG("Memory leak detected: …")`, `src/util/memory.c:509` — goes to the run's stderr). The glob matches nothing, the function returns `True` unconditionally, and every caller — `test_full_pipeline.py:162-182` plus ~10 module integration tests — asserts on a check that cannot fail. Fix: scan the *captured stdout+stderr of the run* for `"Memory leak detected"` (excluding the `"No memory leak"` success text). This changes the helper's input from a directory to run output; the module-test call sites update in mirror (permitted ripple). Verify the fix catches a deliberately injected leak before landing.

**B2 — CI's "Check for memory leaks" step is vacuous for the same reason.**
`ci.yml:64-71` greps `tests/data/output/binary/metadata/*.log` with `2>/dev/null` hiding the "no such file" error, so the step always prints "No memory leaks detected". The failure-artifact upload (`ci.yml:80`) points at the same nonexistent files. Once B1 makes the in-test gate real, this CI step is redundant — remove it (or repoint it at captured suite output) and fix the artifact path.

**B3 — The C unit-test template generates syntactically invalid code.**
clang-format has mangled the substitution placeholders in `c_unit_test_template.c`: `extern int[module_name] _init(void);` (lines 56-58) and call sites like `[module_name] _process(&ctx, …)` (lines 241, 258, 298, 340, 376-384) produce syntax errors even after correct placeholder substitution — the space belongs after the bracket (`[module_name]_init`). The include block (lines 29-37) also mixes 3-up framework paths with `../core/…` paths that resolve from neither documented copy destination (`models/<model>/modules/<module>/_tests/` needs 4-up). And the template re-declares all the fixture boilerplate (`reset_config`, `ensure_modules_registered`, counters, the `set_test_model_parameters` extern) that the M4-1 `sage_test_fixtures.h` extraction removed from real tests. Anyone copying this template today gets non-compiling, out-of-date code.

**B4 — A permanently skipped C test reports PASS.**
`test_module_configuration.c:252-264` (`test_unknown_module_error`) prints "SKIPPED (requires process isolation)" but returns `TEST_PASS`, so `TEST_RUN` emits `MIMIC_RESULT: PASS` — the skip is invisible in summary mode and in counts. Root cause is a framework gap: `TEST_MARKER_SKIP/WARN/ERROR` exist in `test_framework.h` but have **zero C users** (grep-verified) because `TEST_RUN` offers no skip pathway. Fix: add a `TEST_SKIP` sentinel return value, have `TEST_RUN` emit the SKIP marker and count it separately, and convert this test. Reported unit counts change from 32 pass to 31 pass + 1 skip — more honest, not weaker.

**B5 — A C test that fails without an assertion emits no FAIL marker.**
`TEST_RUN`'s failure branch (`test_framework.h:181-184`) increments `failed` but emits nothing; only `TEST_ASSERT*` emit FAIL markers. A test returning 1 directly would fail the binary while summary mode shows no failing marker for it (run_tests.sh's no-marker fallback only triggers when the *whole binary* emitted none, `run_tests.sh:292-298`). Latent today — every current test uses `TEST_ASSERT` — but a one-line `TEST_MARKER_FAIL(#test_func, "test returned failure")` in the else-branch closes the hole.

**B6 — `compiled_simulation()` ignores the Makefile default that its siblings honour.**
`harness.py:30-32` falls back to a hardcoded `"mini-millennium"`, while `compiled_model()` (`harness.py:25-27`) and `generate_test_inputs.selected_simulation()` (`generate_test_inputs.py:43-48`) read `DEFAULT_*` from the Makefile. If `DEFAULT_SIMULATION` ever changes, env-less harness runs silently disagree with the generators. One-line fix; see also T2-2 (the `_makefile_default` helper itself is duplicated across the two files).

**B7 — The framework's validation helpers are silently discarded at both call sites.**
`validate_no_nans/validate_no_infs/validate_range` (`data_loader.py:105-221`) return result dicts instead of asserting. Their only consumers — `sage_initialise_merger_clock` and `sage_satellite_stripping` integration tests — call them bare (`validate_no_nans(halos)` with no use of the return), so lines that read like assertions check nothing. Fix at the framework level: add asserting wrappers (`assert_no_nans(halos)` etc.) and switch the two call sites in mirror — a strict strengthening.

---

## 3. Per-System Review

### T1. Framework Core

What is good: the marker protocol is exactly right — one prefixed line per result, matched exactly by the filter, documented in DEVELOPER-GUIDE.md:788-789; `parity_trace.h` is a tidy, bounds-checked single-purpose helper (one consumer, the merger-ordering-parity test — fine); `test_phase_config.h` documents its allocation/ownership contract precisely and has 15 consumers.

**T1-1. `TEST_ASSERT*` macros are bare `if` statements.** (`test_framework.h:98-158`) No `do { } while (0)` wrapper, so `if (x) TEST_ASSERT(...); else ...` binds the `else` to the macro's internal `if`. Standard hardening; no call-site changes.

**T1-2. Generic ANSI color macro names in a shared header.** `BLUE/GREEN/RED/YELLOW/NC` (`test_framework.h:46-50`) are collision bait for any including translation unit, and the same five literals are re-declared in `run_tests.sh:27-31`, `check_no_memory_leaks()` (`harness.py:617-619`), and at module level in **36 Python test files** plus in-function re-declarations at five more sites inside `test_output_formats.py` (:124-126, :633-635, :677-679, :881-884, :1067-1070). C side: prefix (`TEST_COLOR_*`) or keep with a header comment claiming the names. Python side: export the constants from `tests/framework` once (see T6-5).

**T1-3. The five marker macros are five copies of one pattern.** (`test_framework.h:61-85`) Could collapse to one internal `TEST_MARKER(status, name, suffix)`; cosmetic, do only while in the file for B4/B5.

**T1-4. `TEST_ASSERT_EQUAL` truncates to `int` in its diagnostic.** (`test_framework.h:120`) Comparing `long long`s (e.g. `UniqueGalaxyID`) reports wrong values on mismatch. Cast to `long long` and use `%lld` — display-only fix.

**T1-5. `TEST_FAIL` constant has zero users** (grep-verified); `TEST_PASS` is used everywhere. B4's skip support adds `TEST_SKIP`; keep all three then for a complete return vocabulary, documented in the header.

**T1-6. Stale process headers.** `@author Mimic Development Team / @date 2025-11-08 / @version 1.0 (Phase 2: Testing Framework)` (`test_framework.h:32-34`) and the same `Phase:`/`Author:`/`Date:` fields across `harness.py:7-9`, `data_loader.py:8-9`, `run_tests.sh:18-20`, `generate_test_registry.py:18-20`, and the older T4/T6 test files. Same class as M4-2/S1-8: git knows; sweep them out, keep `@brief` content.

### T2. Python Harness & Data Loading

What is good: `run_mimic_fresh()`'s stale-output rationale (`harness.py:249-282`) is exactly the right kind of why-comment; the baseline tolerance constants document their numerical reasoning (`harness.py:77-86`); generated-input regeneration is guarded by a manifest check so MODEL/SIMULATION switches can't serve stale inputs (`harness.py:136-170`); `final_snapshot_index_from_a_list` validates its input thoroughly.

**T2-1.** B1 (vacuous leak check).
**T2-2.** B6, plus `_makefile_default()` duplicated between `harness.py:35-48` and `generate_test_inputs.py:23-36`. `scripts/lib/defaults.sh` already exists for shell consumers; for Python, one shared home (e.g. `scripts/discovery.py`, which both already import or can) ends the drift risk.
**T2-3. `skip_non_default_baseline(test_name)` never uses its argument** (`harness.py:66-74`), and does a local `TestSkipped` import although no cycle exists. Drop the parameter (two call sites in `test_output_formats.py`) or use it in the message; import at top.
**T2-4. `harness.__all__` has drifted** (`harness.py:649-664`): it lists 14 names while `__init__.py` imports 22 from the module (missing `baseline_rtol`, the `BASELINE_*` constants, `default_model/simulation`, `is_default_baseline_combo`, `skip_non_default_baseline`, `input_tree_file_for_run`, `read_param_file`). `__init__.py` is the real export surface; delete `harness.__all__` rather than maintaining a second copy.
**T2-5. `core_input_file`/`simulation_input_file` duplicate their ensure-regenerate-raise choreography** (`harness.py:173-195`) — one private `_generated_input(relative_parts, error_hint)` helper.
**T2-6. `validate_no_nans`/`validate_no_infs` are twins whose scalar/vector branches are byte-identical.** (`data_loader.py:105-166`) The `if data.ndim == 1 / else` arms run the same `np.sum(np.isnan(data))`; the two functions differ only in `isnan` vs `isinf`. One predicate-parameterized helper. Also `validate_range`'s docstring documents a key `example_violations` that the function does not return (it returns `examples_below`/`examples_above`, `data_loader.py:186/213-221`).
**T2-7.** B7 (asserting wrappers).
**T2-8. Framework-grade helpers are trapped in `test_output_formats.py`.** `load_hdf5_halos` (:114-173), `decode_hdf5_string` (:176-182), `assert_hdf5_schema_layout` (:185-222), and the 230-line `compare_halos_comprehensive` family (:225-513) are general-purpose, and HDF5 reading is hand-rolled separately in the simulation packages' `test_satellite_spatial_distribution.py` and one sage16 module test (grep-verified). Move them to the framework (e.g. `data_loader.py` + a new `comparison.py`), import back into `test_output_formats.py`; other consumers may adopt later (consumers-only).
**T2-9. Minor:** `read_param_file`/`create_test_param_file` import `yaml` in-function while test files import it at top — pick one convention (top-level; PyYAML is already a hard dependency of the harness).
**T2-10. `create_test_param_file` cannot set `SubSteps`,** forcing every caller that needs it to re-open and rewrite the generated YAML — 15 copies of that block across T6 (see T6-2). Add a `substeps=None` parameter that sets `config["SubSteps"]` directly.

### T3. Templates

What is good: the instructional sections (what to validate, what *not* to test at each tier, tolerance guidance) are genuinely useful onboarding material; the integration template models the `TestSkipped` runner contract correctly.

**T3-1.** B3 — the C template is broken three ways (mangled placeholders, wrong include depths, pre-M4-1 boilerplate). Rewrite using a formatter-safe placeholder convention (e.g. a real compilable name like `mymodule` with a rename instruction, which also lets the template be compile-checked) and the current fixture pattern.
**T3-2. The scientific template stubs out what the framework already provides.** `load_halos()` raises `NotImplementedError` with a TODO (`python_scientific_test_template.py:46-60`) although `framework.load_binary_halos` has existed since the data loader was written, and the commented import suggests `from mimic_plot import load_binary_data, load_hdf5_data` — functions that do not exist. Use the framework import like the integration template does.
**T3-3. pytest does not exist in this repo** but is referenced as a supported runner in the scientific template (:201, :285-291) and `test_full_pipeline.py:244`. Remove; `make tests-*` and direct `python3` are the two supported invocations.
**T3-4. Template hygiene:** unused `os`/`numpy` imports in the integration template; five near-identical 40-line test bodies — keep one fully worked example and reduce the rest to documented skeletons; stale `Phase: [ROADMAP PHASE…]` header fields.
**T3-5. The templates are referenced nowhere** — no doc, script, or README mentions them (grep-verified across `*.md`). After fixing, add one pointer in DEVELOPER-GUIDE's testing section; undocumented templates rot invisibly (this review is the proof).
**T3-6.** Once T6-1's shared runner exists, both Python templates should demonstrate it instead of the copied loop.

### T4. Core C Unit Tests

What is good: the newer files — `test_galaxy_pool.c`, `test_output_buffer.c`, `test_inheritance.c` — are the model to follow: each test asserts a named invariant of the engine, the why-comments carry design knowledge (chunk stability, Type-3 pointer-clear ownership, SnapNum parity), and headers are free of boilerplate. `test_virial_properties.c` checks real physics including the SAGE-parity `Mvir=0` central edge case.

**T4-1. Assertions that cannot fail.** `test_module_registry_init` has no assertions at all (`test_module_configuration.c:76-90`); `TEST_ASSERT(GENERATED_INIT_REPEAT_PROPERTY_COUNT >= 0, …)` is tautological (`test_property_reset.c:50`); `test_property_metadata.c`'s "field should be accessible" assignments-then-reads (:48-93, :132-157) and `sizeof(galaxy) == sizeof(struct GalaxyData)` (:111) cannot fail at runtime — their real value is compile-time field existence. Don't delete (the compile check is the coverage); reword the comments to say so honestly, and drop the tautologies that document nothing.
**T4-2. Duplicated micro-fixtures.** `test_binary_param_file()` is byte-duplicated in `test_parameter_parsing.c:43-48` and `test_virial_properties.c:47-52`; `reset_config()`/`ensure_modules_registered()` in `test_module_configuration.c:44-52` repeat the pattern the model package centralized in M4-1. Create a small `tests/framework/core_test_fixtures.h` (counters stay per-file; the helpers move), which the rebuilt template (T3) also uses.
**T4-3. Header test-case lists have drifted in 5 files** (e.g. `test_memory_system.c` lists 5, runs 6; `test_property_metadata.c` 5/6; `test_parameter_parsing.c` 5/6). These lists are rot magnets that duplicate `main()`; delete the lists, keep the prose description.
**T4-4.** Stale `Phase:`/`@author`/`@date` headers on the seven older files (T1-6 sweep).
**T4-5. `set_test_model_parameters()` carries sage16 parameter names in a core file.** (`test_stubs.c:44-101`) Fifteen `strcpy` pairs of SAGE-specific names live in `tests/unit/`, linked into every unit test of every model — yet grep shows its only consumers are sage16 module tests (core tests use the framework fixture's own `set_test_fixture_params`). Under standing decision 5 this is a model-boundary violation with a clean fix: move the function into the sage16 package (e.g. provided by `models/sage16/modules/_tests/sage_test_fixtures.h`, which already declares the extern), as a `{name, value}` table loop; `test_stubs.c` shrinks to the model-agnostic `myexit`. A future model package then brings its own parameter setup exactly as it brings its own fixture header — the SHAM unit test in Batch 10 is the proof of the pattern. The sage16 test files update in mirror (permitted ripple).
**T4-6. Recorded gap (no change now):** `check_memory_leaks()` returns `void`, so `test_leak_detection` (`test_memory_system.c:194-219`) can only eyeball the warning. Making the count observable is a `src/` API change — out of scope, recorded for the next src follow-up.
**T4-7. `test_consistency` guards its assertions behind the very conditions under test** (`test_numeric_utilities.c:266-281`): if `is_less(5,10)` returned false, the test would pass vacuously. Assert the premises directly (`TEST_ASSERT(is_less(a,b),…)` first) — a strengthening.

### T5. Unit Runner (`run_tests.sh`)

What is good: summary mode is deterministic (exact marker grep, full-log fallback for marker-less crashes, `run_tests.sh:286-302`); the post-run regeneration without `MIMIC_TEST_BUILD` (:327-333) is correctly reasoned and commented; failure recording integrates cleanly with the Makefile aggregate.

**T5-1. Every test recompiles all ~30 shared sources from scratch.** `compile_and_run_test` passes `$ALL_SRCS` to the compiler per test (:260, :270) — 31 tests × a full library build is the dominant cost of the "up to 3 minutes" unit tier. Compile the shared sources once into `tests/unit/build/*.o` (or a static archive) at the top, then per-test compile only the test file (+ its module `-I`) and link. Behavior-identical output, large wall-clock win; verify identical pass/fail/marker counts.
**T5-2. `MODEL` is hard-required while `SIMULATION` silently defaults.** (:132-142) The script exits without `MODEL` yet defaults `SIMULATION` "mirroring DEFAULT_SIMULATION" — and `scripts/lib/defaults.sh` exists precisely to provide both (already sourced by `test_plotting.sh:14`). Source it and default both, keeping the env overrides.
**T5-3. The documented exit-code contract is wrong.** Header says "2 - Compilation error" (:16) but compile errors take the common `exit 1` path (:360-368); only generation-preamble failures exit 2. Fix the header (or the code — header is cheaper and the distinction has no consumer).
**T5-4. Dead variable `GENERATED_SRCS=""`** (:155) and the stale Author/Date/Phase header (:18-20).
**T5-5. Fallback `git_version.h` is written and then immediately overwritten** when git is available (:106-129), duplicating the Makefile's version-header logic. Restructure to one branch (write real info if available, else fallback).
**T5-6. `module_sources.mk` is a make fragment that make never includes.** The Makefile only assigns its path to `MODULE_SOURCES_MK` (:407) and echoes it in help text (:500); the sole functional consumer is this script's grep/sed parse (:160-171). Either have `generate_module_registry.py` emit a plain `module_sources.txt` (one path per line) for the runner and drop the unused Makefile variable, or actually `include` the fragment — recommend the plain list (simpler consumer, no fake make syntax).

### T6. Core Integration Tests

What is good: the event-routing and event-schema suites are modern and sharp — the schema tests exercise the generator's pure validation functions with no subprocess (`test_event_schema_validation.py:78-82`); the fail-fast tests assert exact error text so diagnostics are themselves under test (`test_processing_modes.py:331-377`); `compare_halos_comprehensive` with worst-first ranking, a relaxed-gate warning band, and a documented atol floor is the best-engineered comparison code in the repo; the unique-ID contract test checks a real cross-halo invariant.

**T6-1. The `main()` runner loop is copy-pasted across 30 Python test files** (~45 lines each: banner, loop, `TestSkipped`/`AssertionError`/`Exception` triage, summary, exit code) — the Python-side M4-1. Add `framework.run_test_suite(tests, title) -> int` and adopt it in the 10 core files and both Python templates (≈450 lines removed in scope; the 20 module-local copies may adopt in a mirror batch or stay — decision at batch time).
**T6-2. The SubSteps rewrite block appears 15 times** (open YAML, set `config["SubSteps"]`, rewrite — e.g. `test_phase_execution.py:113-120` ×5, `test_substeps.py` ×4, `test_processing_modes.py` ×3, `test_galaxy_major_loop.py` ×2), each with an in-function `import yaml`. Subsumed by T2-10's `substeps=` parameter.
**T6-3. `parse_test_fixture_executions` exists in 4 copies** (3 byte-identical, 1 differing only in comments — hash-verified) across `test_phase_execution.py`, `test_substeps.py`, `test_processing_modes.py`, `test_galaxy_major_loop.py`. One framework helper (it is the public face of the test fixture's `TEST_FIXTURE_EXEC` log contract, so the framework is its natural home, next to a comment naming that contract).
**T6-4. `check_hdf5_support()` performs a full Mimic run as a capability probe — uncached, five times.** (`test_output_formats.py:71-94`, called from :739, :785, :858, :955, :1036) Each call is a complete run of `test_hdf5.yaml`; with the per-test `run_mimic_fresh` calls this file alone executes Mimic ~16 times. Memoize the probe (module-level cache) — removes 4 full runs with zero semantic change. The per-test `run_mimic_fresh` regenerations are deliberate staleness guards (see §5) and stay.
**T6-5. ANSI color constants are re-declared in 36 files** plus five in-function re-declarations inside `test_output_formats.py` that shadow the module's own constants. Export `BLUE/GREEN/RED/YELLOW/NC` from `tests/framework` (markers.py is a natural home) and delete the local blocks in the 10 core files + templates.
**T6-6. `test_processing_modes.py` is written in pre-rename vocabulary.** Docstrings, prints, the suite title, and the test *names* say `PROCESSING_MODE_ONCE`/`PROCESSING_MODE_ALL` (:1-25, :84-91, :156-163, :227-234, :396) — modes that no longer exist (`process_full_halo`/`process_by_galaxy`/`process_per_event`). Renaming the test functions changes their marker names; that is reporting-only and acceptable.
**T6-7. Stale references:** `test_event_schema_validation.py:9` cites `docs/EVENT-SYSTEM-IMPROVEMENTS.md` (archived; file absent); `test_module_pipeline.py:88` builds `ref_param_file` from `input/mini-millennium.yaml` (directory does not exist) and the attribute is never read — delete it, along with the unused `subprocess` and `read_param_file` imports (:20, :33); pytest mentions (T3-3).
**T6-8. `test_module_pipeline.py` is the lone unittest-based suite,** requiring its own `_MarkerTestResult` adapter (:49-66) to speak the marker protocol. Convert to the standard function-list runner during T6-1 adoption so one runner convention serves all core tests (assertion-for-assertion port; method names preserved as function names so marker names survive).
**T6-9. Overclaiming docstrings and one circular assertion.** `test_multiple_modules_galaxy_major` prints "Each galaxy processed by both modules before next galaxy" but only counts calls (`test_galaxy_major_loop.py:126-147` — ordering is not observable from the shared counter); `test_substep_dt_calculation`'s "total time" check compares `sum(dts)` against `first_dt * 10` after asserting all dts equal — circular (`test_substeps.py:214-220`). Reword the claims to what is actually verified; replacing the circular check with a comparison against the snapshot interval is recorded as optional Phase-C strengthening. Also the unused `num_fof_groups` at `test_phase_execution.py:290`.
**T6-10. Small cleanups:** double negative `has_leaks = not check_no_memory_leaks(…); assert not has_leaks` (`test_full_pipeline.py:178-180`); ANSI codes embedded inside assert messages throughout `test_full_pipeline.py` (failure text in logs gains escape noise); header test-case lists drifted (4 listed / 6 run in `test_full_pipeline.py`, 6/9 in `test_output_formats.py`) — same fix as T4-3.

### T7. Scientific Tier

What is good: metadata-driven validation is the design the Vision asks for — properties added to YAML are validated with no test edit; sentinel awareness is threaded through every check; the unit-consistency test (virial relation, c sanity, dT sanity) is exactly the class of check that catches unit bugs like the quasar-wind one.

**T7-1. Local re-implementations of framework facilities.** `test_scientific.py:52-68` redefines `TEST_DATA_DIR`, `MIMIC_EXE`, and a byte-copy of `ensure_output_dirs()` while importing eight other names from the same framework. Import them.
**T7-2. The same output is regenerated fresh four times.** All four tests call `regenerate_output()` (:81-98) on the identical config. The fresh-run guard protects against stale files from *other* runs/models; within one suite process, the first regeneration establishes that guarantee. Memoize per process (cache the returned path) — three full Mimic runs saved, guard intact.
**T7-3. `check_nans_infs` (:101-148) re-implements `data_loader.validate_no_nans/infs`** with examples added. After T2-6 consolidates the loader pair, fold this in as the one implementation (examples support included) and have both tiers share it.
**T7-4. Third runner dialect.** Tests here return `(ok, count)` tuples and `main()` maps any passing nonzero count to a hardcoded "field(s) with zero values" warn reason (:643-655) — correct only because `test_zero_values` happens to be the only warn-capable test. When adopting T6-1's shared runner, let tests call `result_warn` themselves and return normally, removing the tuple dialect and the mislabeled-reason trap.

### T8. Plumbing (Makefile, generators, CI)

What is good: the `TEST_BUILD` separation is exemplary infrastructure — separate object tree with the stale-link rationale written down (`Makefile:96-106`); `RUN_SUMMARY_AWARE`/`RUN_SUMMARY_AWARE_RECORD` give infrastructure steps the right silent-on-success/full-on-failure behavior; registry generation is hash-stamped; `generate_test_inputs.py` is the cleanest script in scope.

**T8-1. The `make tests` preamble silences its own failures.** `Makefile:695-696` runs `clean` and `generate-test-registry` with `> /dev/null 2>&1`; a failure aborts the target with zero diagnostic output. This is the exact issue recorded as a follow-up in the models review (Batch 3 addendum). Route both through `RUN_SUMMARY_AWARE`.
**T8-2. CI gaps.** `ci.yml` never runs `make check-docs` or `make validate-modules`, both of which `make tests` treats as first-class suites (`Makefile:701-703`) — a doc-link or metadata regression passes CI today. Add both steps. Plus B2 (vacuous leak step) and the dead artifact path (:80).
**T8-3. Registry + inputs are regenerated up to four times per `make tests`** (the `tests` preamble, each of the three tier targets, and `run_tests.sh` again for unit). Each run is ~1 s and idempotent, and standalone tier invocation requires it — acceptable redundancy; note only, no change.
**T8-4. `tests-integration` and `tests-scientific` recipes are near-identical** (`Makefile:737-757`, differing in one word ×3) — fold into one parameterized canned recipe alongside the existing macros.
**T8-5. `test-clean`'s `tests/**/__pycache__` glob does not recurse** under `/bin/sh` (`Makefile:766-767`) — only one directory level is cleaned. Use `find tests -name __pycache__ -type d` / `-name '*.pyc'`.
**T8-6.** T5-6's `module_sources` format change lands here too (generator emission + Makefile variable removal).
**T8-7. `generate_test_registry.py` internal triplication.** The unit/integration/scientific processing blocks (:181-205) and the three manifest-write blocks (:211-233) are copy-triples — loop over `[("unit", …), …]` with one `write_manifest()` helper. The module-level mutable `missing_tests` global (:57, cleared in `__main__`) should be a return value. Stale Phase header (T1-6).
**T8-8. `generate_test_inputs.py` hardcodes snapshot indices that are only correct for 64-snapshot simulations.** `snapshot_list=[63]` and `[62, 63]` (:193, :204, :215, :226, :235) silently assume the mini-Millennium snapshot count; a future simulation package with a different a_list would get test inputs requesting snapshots that may not exist (standing decision 5). Derive the last (and second-last) snapshot index from the configured simulation's a_list — byte-identical generated YAML for both current packages, correct for any future one.

### T9. Test Data & Baselines

What is good: git tracking is exactly right — only `output/baseline/**` is committed; regenerated outputs, `physics-binary/` (the model-owned byte gate's directory), and `tests/generated/` are ignored; every baseline carries its own `metadata/` (schema, configs, version) so loaders never guess a dtype — the metadata "duplication" across format dirs is the self-containment contract, not waste.

**T9-1. No README explains the baseline contract.** The regeneration procedure and the update policy ("only after a deliberate core change, with user approval") live solely in two test docstrings (`test_output_formats.py:603-606, 847-848`) and a memory note. Add a short `tests/data/README.md`: directory layout, what each baseline gates (physics-free core determinism here; the full-physics gate lives in `models/sage16/.../test_scientific_sage_physics_baseline.py` against `physics-binary/`), the exact regeneration commands for binary and HDF5 (including the metadata copy), and the policy. This is the M7-2 move: convert an implicit contract into a written one.
**T9-2.** Nothing else. The 18 MB tree fixture is the price of realistic integration tests and is shared by both simulation packages by design.

### T10. Plotting Tests

What is good: the unit tests target genuinely tricky seams — profile inheritance is tested through `runpy` because `mimic-plot.py`'s filename is unimportable (clever and commented), and the SAGE-native HDF5 reader tests build synthetic per-rank and master files with `unittest.skipUnless(HAVE_H5PY)`.

**T10-1. `test_plotting.sh` never runs `test_validation_helpers.py`.** Tests 6-8 (:86-93) run the other three unit files; the fourth is reachable only by the manual invocation documented in AGENTS.md. Add it to the script.
**T10-2. The plotting suite speaks no marker protocol and uses three runner styles** (two hand-rolled `run_all_tests()` variants, one unittest). The suite is *correctly* outside `make tests` — it needs the venv and real plot output — but its Python files can still emit `MIMIC_RESULT:` markers cheaply by importing `tests/framework` (markers only; or the T6-1 shared runner). Adopt markers + the shared runner in the three non-unittest files; add a header comment to `test_plotting.sh` stating why this suite is standalone (venv + generated plot data prerequisites).
**T10-3. Style nits:** `assert x == True/False` comparisons in `test_validation_helpers.py` (`assert is_valid` / `assert not is_valid`); the unittest file's shebang is `#!/usr/bin/env python` (no 3).

---

## 4. Cross-Cutting Themes

1. **The leak-gate illusion (B1/B2).** Two independent mechanisms — the harness helper and the CI step — both grep log files that have never existed, so the project has *no working automated leak gate* outside the C unit tests' in-process checks, despite a dozen call sites that read as if it does. This is the review's most important fix: a green suite must mean what it says (Vision Principle 7).
2. **Vacuous checks hide inside green suites.** The fake C skip (B4), the discarded `validate_*` returns (B7), the tautological asserts (T4-1), the circular dt check (T6-9) — each individually small, together a pattern: assertions that cannot fail are worse than no assertions because they document coverage that does not exist.
3. **Extract-the-third-copy, Python edition.** The runner loop (×30), ANSI block (×36), `parse_test_fixture_executions` (×4), SubSteps rewrite (×15), `_makefile_default` (×2), `test_binary_param_file` (×2), `ensure_output_dirs` (×2), NaN/Inf checkers (×3 implementations). The framework exists precisely to hold these; roughly 1,500–1,800 lines disappear with zero assertion changes.
4. **One protocol, three dialects.** C tests cannot skip; core Python has two runner styles (function-list and unittest) plus the scientific tuple dialect; plotting has no markers at all. After this review: one C pathway with skip support, one Python `run_test_suite`, markers everywhere.
5. **Stale references rot fastest — in tests too.** Pre-rename mode names baked into test function names, citations of archived docs, a dead `input/` path, pytest instructions for a repo that never adopted pytest, and "Phase N" headers from five process generations ago. Same theme as M1-7/M2-2; same fix discipline.
6. **The wall-clock budget is spent on redundant runs, not on checks.** Per-test full recompilation (T5-1), an uncached full-run capability probe (T6-4), and 4× regeneration of identical output (T7-2) dominate the "up to 3 minutes" tiers. All three fixes are behavior-preserving and could roughly halve `make tests` time — which matters because this suite gates every batch of every future review.
7. **The framework's rails must be model-count-agnostic** (standing decision 5). Most of the machinery already is — registry discovery via `module_info.yaml`, the physics-free shared baseline, the framework test-fixture modules, `skip_non_default_baseline`. The residue this review fixes: sage16 parameters in a core stub (T4-5), hardcoded snapshot indices in the input generator (T8-8), and templates that no longer show a working pattern for the next model author (B3/T3). Deliberately *not* generalized: the `model_z0.000_0` output-filename literals in core tests assume snapshot 63 = z=0 for the selected simulation; a name-derivation helper would touch ~15 call sites for a need no current or planned package has — recorded, not scheduled.

---

## 5. What Not to Change

- **Per-test `run_mimic_fresh` regeneration** in baseline/loading/contract tests is a deliberate guard against stale outputs from a different MODEL satisfying assertions (documented at every site). The only sanctioned reductions are the *capability probe* memoization (T6-4) and *within-one-process* reuse in the scientific tier (T7-2) — both keep the first-run-regenerates guarantee.
- **`MIMIC_TEST_BUILD` reset + regeneration after unit runs** (`run_tests.sh:327-333`) and the post-suite `make generate` inside `RUN_PYTHON_TEST_REGISTRY` (`Makefile:635`): both restore production-mode generated files so `make check-generated` cannot false-fail. Leave intact.
- **The separate `build/test` object tree** and the test executable keeping the production name `mimic` (`Makefile:96-106`) — both have written rationale.
- **Registry/input regeneration in every tier target** (T8-3): redundancy is what makes standalone `make tests-integration` correct.
- **The exact summary grep `^MIMIC_RESULT: (FAIL|SKIP|WARN|ERROR)`** in both the Makefile macro and `run_tests.sh:print_markers` — two consumers in two languages of one wire protocol; change only in lockstep with both emitters.
- **The committed baseline metadata directories per format** — self-containment contract, not duplication (each output is interpretable through its own `output_schema.json`).
- **`parity_trace.h` staying in `tests/framework/` with one consumer** — it is the framework-side half of a model-test contract.
- **Per-test `init_memory_system(0)` without explicit teardown** in C tests — re-init is the allocator's documented test pattern.
- **The `tests/data/test_simulation.yaml` shared mini-catalog recipe** and the 18 MB tree fixture.
- **`G_CODE` literal in `test_scientific.py:505`** — Python cannot include `physical_constants.h`; the comment states the units; an independent restatement is part of the cross-check's value.

---

## 6. Suggested Implementation Order

Each batch is independently shippable, one commit, user-approved. After every batch: `./scripts/beautify.sh`; full `make tests summary` (delegated to a subagent) green with **identical test inventory** (`build/generated/*_tests.txt` counts unchanged; Batch 2 changes 32 pass → 31 pass + 1 skip as specified; Batch 10 only adds); the byte-identical physics baseline runs every batch even though production code is untouched. Mechanical batches additionally verify by diff-grep that no `TEST_ASSERT*`/`TEST_RUN`/`result_*`/`self.assert*` assertion lines were weakened. Plumbing batches (7, 8) additionally verify `make check-generated`, `make validate-modules`, both normal and `summary` modes, and that a deliberately failing scratch test is caught (exit code + visible marker) before the scratch test is removed. The plotting batch (9) runs the plotting unit tests, `./test_plotting.sh`, and full plot generation for both models. Removed files go to `archive/removed-tests/`.

| Batch | Content | Risk |
|---|---|---|
| 1. Leak-gate repair | B1 rework `check_no_memory_leaks` to scan run output; update core caller (T6-10 double negative) + ~10 module-test call sites in mirror; B2 fix CI step + artifact path. Verify with an injected leak. | Medium — framework API change across ~12 files |
| 2. C framework honesty | B4 skip pathway (`TEST_SKIP`, counter, marker) + convert fake skip; B5 fail-marker in `TEST_RUN`; T1-1 do-while; T1-3, T1-4, T1-5; T4-7 premise asserts | Low — counts change as documented |
| 3. Dead code & stale references | T6-7 (dead `ref_param_file`, unused imports, archived-doc cite), T6-6 mode-name modernization, T3-3 pytest removals, T5-3/T5-4, T4-3/T6-10 header-list drops, T1-6 Phase/@author sweep, T2-3 | Low — grep-verified |
| 4. Python framework dedup | T6-1 `run_test_suite` + adoption in 10 core files, T6-8 unittest conversion, T6-5 color export, T6-3 parse helper, T2-10 `substeps=` + T6-2 sweep, T2-4, T2-5, T2-6 (+docstring fix), T7-1, T7-4, B6 + T2-2 `_makefile_default` consolidation, T2-9 | Medium — wide but mechanical; assertion diff-grep is the gate |
| 5. Comparison & probe consolidation | T2-8 move HDF5 loader/schema/comparator into framework, T6-4 memoize `check_hdf5_support`, T7-2 memoize `regenerate_output`, T7-3 unify NaN/Inf checkers | Medium — touches the baseline-comparison path; baseline tests must stay green in both normal and relaxed-rtol modes |
| 6. Templates | B3 rewrite C template (formatter-safe, correct includes, model-owned fixture-header pattern, framed for new-model authors), T3-2, T3-4, T3-6, T3-5 DEVELOPER-GUIDE pointer | Low — templates compile-checked manually |
| 7. Unit runner & generators | T5-1 shared-object compilation, T5-2 defaults.sh adoption, T5-5, T5-6/T8-6 `module_sources.txt`, T8-7 registry-script cleanup, T8-8 a_list-derived snapshot indices, T4-2 core fixture header, T4-5 relocate sage16 params to the model package (sage16 tests mirror-updated) | Medium — runner rework; verify identical per-test results + deliberate-failure drill |
| 8. Make/CI & data | T8-1 preamble un-silencing, T8-2 CI steps (check-docs, validate-modules), T8-4 recipe fold, T8-5 find-based clean, T9-1 `tests/data/README.md` | Low |
| 9. Plotting | T10-1 wire missing test, T10-2 markers + shared runner + standalone rationale, T10-3 | Low |
| 10. Additive coverage (Phase C) | M4-3 SHAM unit test (sage16 conventions, markers, registered via `module_info.yaml`); B7 asserting wrappers + mirror adoption in the two module tests; optional T6-9 real dt-interval check | Low — additive only; inventory count increases |

**Recorded, not scheduled:** T4-6 (`check_memory_leaks` return value — src API change); T8-3 (regeneration redundancy — deliberate); module-local adoption of `run_test_suite` beyond the two B7 files (optional mirror batch, owner's call); an output-filename derivation helper for the `model_z0.000_0` literals in core tests (theme 7 — no current need).

---

## 7. Implementation Addendum

*(Filled in as batches land, recording deviations discovered during implementation.)*
