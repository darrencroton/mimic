# Test Selection Strategy

## Purpose

Mimic is approaching v1.0 with a deliberately more flexible package model: one executable is built from a selected `MODEL` and `SIMULATION`, while run files choose modules and input ranges at runtime. The test system must support that flexibility without hiding package-boundary bugs, without forcing full production catalogs through fast test tiers, and without duplicating baseline logic in many places.

This document records the intended KISS and DRY testing model. It complements the architectural constraints in [VISION.md](../../VISION.md): the core stays physics-agnostic, metadata is the source of structural truth, selected model/simulation packages must be validated together, and failing tests represent real problems.

This document also captures design decisions made for v1.0 and flags known gaps for the team to revisit after release (or sooner if the test suite grows materially). See [**Post-v1.0 Notes**](#post-v10-notes-and-known-gaps) at the bottom.

## Core Rule

Tests must respect the selected compiled package pair. Do not ignore `MODEL` or `SIMULATION` in order to make tests pass.

The correct simplification is not "always run tests against mini-Millennium." The correct simplification is "always compile and validate the selected package pair, but use a test-sized input fixture unless a test explicitly opts into production-scale data."

That distinction matters:

- Compiling with `SIMULATION=millennium` must generate and validate code from `simulations/millennium/halo_properties.yaml`.
- Fast tests may run that executable on a small fixture catalog if the simulation package declares that fixture as suitable for testing.
- Baseline tests may compare against committed outputs only when the selected package pair matches the baseline package pair.

## Test Categories

### Core Invariant Tests

Core invariant tests validate framework behavior that should hold for any selected package pair: configuration loading, module registration, processing modes, event routing, phase execution, output writing, binary/HDF5 round trips, and memory cleanup.

These tests belong under `tests/unit/`, `tests/integration/`, and `tests/scientific/`.

They should run for the selected `MODEL` and `SIMULATION` because generated structs, schema writers, module registries, and validation ranges depend on those selectors.

They should use generated test run files from `build/generated/test_inputs/<MODEL>/<SIMULATION>/`. Those run files may point to a package-declared small fixture rather than production tree files.

### Selected-Simulation Tests

Selected-simulation tests validate catalog- or simulation-specific behavior: tree loading, tree-to-output property preservation, selected-simulation spatial sanity checks, and ID behavior that depends on catalog structure.

These tests belong under `simulations/<simulation>/_tests/`.

They should run only when `SIMULATION=<simulation>` is selected. They must not assume a hard-coded input tree path. They should derive the input tree from the generated run file or from the simulation package's test fixture metadata.

If a simulation package has production data that is too large for fast tests, it should provide `simulations/<simulation>/_tests/input/test_simulation.yaml`. That file declares the fast fixture used by normal test tiers while the executable remains compiled against the selected simulation's halo properties.

### Selected-Model Tests

Selected-model tests validate model-owned module behavior and model-local contracts. These tests belong under `models/<model>/modules/.../_tests/` or model-local shared test collections such as `models/sage/modules/_tests/`.

They should run only when their model is selected. Most model tests should use generated test inputs and therefore remain fast and independent of production catalog size.

Model-owned exact-output baselines are a special case. If a baseline was captured from the default package pair, the test must skip when the selected package pair differs.

### Default Baseline Regression Tests

Baseline tests compare current output to committed reference files. They are not general correctness tests for arbitrary selected simulations. They are reproducibility checks for the baseline package pair.

For v1.0, the committed repository baselines are defined to belong to the Makefile defaults:

- `DEFAULT_MODEL`
- `DEFAULT_SIMULATION`

When `MODEL` or `SIMULATION` differs from those defaults, baseline comparison tests should skip with an explicit message. Non-baseline smoke tests in the same file may still run.

This avoids false failures such as comparing full Millennium output to mini-Millennium baseline files, while still preserving the stronger exact-output regression for the default release path.

### Slow And Full-Data Tests

Full production data tests are valuable but should be explicit. They validate scale, performance, memory limits, and production data availability. They should not run as part of normal `tests-integration` or `tests-scientific`.

Preferred future shape:

- `make tests-slow`
- `make tests-full SIMULATION=millennium`
- optional per-simulation slow tests under `simulations/<simulation>/_tests/slow/`

These tests should be documented as requiring production data and longer runtimes.

## Fixture Policy

A simulation package can provide a fast fixture config at:

```text
simulations/<simulation>/_tests/input/test_simulation.yaml
```

If present, `scripts/generate_test_inputs.py` should use it for generated test run files. The generated run still points `simulation.halo_properties` at the selected simulation package, so compiled schema validation remains meaningful.

If no package fixture is present, the generator may fall back to the simulation's production `simulation_info.yaml` and cap `first_file`/`last_file` for basic testing. That fallback is acceptable only when the capped range is known to be small enough for fast tiers. For large catalogs, the package should declare a fixture explicitly.

The shared `tests/data/` mini fixture is acceptable when the selected simulation uses the same tree reader and property schema shape. That use should be explicit through the package test config rather than hidden in the registry.

**Known limitation (Millennium fixture):** `simulations/millennium/_tests/input/test_simulation.yaml` currently reuses the shared mini-Millennium tree data from `tests/data/input/`. This keeps CI fast without requiring production Millennium catalog access. What it tests is that the Millennium-compiled binary can load LHalo trees and complete the pipeline. What it does not test is Millennium-specific halo property values or production-scale behavior. This was a deliberate v1.0 shortcut. See [Post-v1.0 Notes](#post-v10-notes-and-known-gaps).

## Applicability Rules

The test harness should centralize applicability decisions so individual tests do not duplicate Makefile parsing or selector logic.

Required helpers (all in `tests/framework/harness.py`, exported via `tests/framework/__init__.py`):

- `compiled_model()`: selected model for this test run.
- `compiled_simulation()`: selected simulation for this test run.
- `default_model()`: `DEFAULT_MODEL` from the Makefile.
- `default_simulation()`: `DEFAULT_SIMULATION` from the Makefile.
- `is_default_baseline_combo()`: true when selected packages match the committed baseline package pair.
- `skip_non_default_baseline(test_name)`: raises `TestSkipped` with a consistent skip reason for default-only baselines.

Initial consumers:

- `tests/integration/test_output_formats.py`: binary and HDF5 baseline comparisons are default-combo-only; format execution/loading and binary-vs-HDF5 equivalence remain selected-combo tests.
- `models/sage/modules/_tests/test_scientific_sage_physics_baseline.py`: SAGE full-physics baseline is default-combo-only unless a future per-combo baseline is added.
- `simulations/<simulation>/_tests/integration/test_tree_preservation.py`: selected-simulation test, derives its input tree from the generated run file.

**Skip visibility:** `skip_non_default_baseline()` now raises `TestSkipped`, and the standard Python test runners emit `MIMIC_RESULT: SKIP ...` markers. Summary-mode output and test counts can distinguish deliberate skips from passes without requiring pytest.

## Near-Term Implementation Steps

All of these were completed for v1.0:

1. ~~Keep `mini-millennium` as the Makefile default and committed baseline package.~~
2. ~~Add `simulations/millennium/_tests/input/test_simulation.yaml` so full Millennium fast tiers run against a small declared fixture while still compiling with `SIMULATION=millennium`.~~
3. ~~Restore package fixture discovery in `scripts/generate_test_inputs.py`.~~
4. ~~Add default-baseline helpers to `tests/framework/harness.py`.~~
5. ~~Gate committed baseline comparisons with `is_default_baseline_combo()`.~~
6. ~~Make tree-preservation tests derive input tree paths from generated run files.~~
7. ~~Keep slow/full production validation out of normal fast tiers until an explicit slow target exists.~~

## Longer-Term Requirements

Before or after v1.0, the test registry should make test intent obvious without requiring every engineer to remember implicit conventions. A minimal metadata extension is enough; avoid a complex test framework.

Recommended future metadata fields:

```yaml
tests:
  integration:
    - path: _tests/test_tree_preservation.py
      scope: selected-simulation
      fixture: package-fast
    - path: _tests/test_full_millennium_memory.py
      scope: full-data
      tier: slow
```

For now, filename/location conventions plus centralized harness helpers are sufficient. Add metadata only when the conventions stop being clear.

## Non-Goals

- Do not maintain exact baselines for every model/simulation pair unless that pair becomes release-critical.
- Do not make normal fast tests process full production Millennium data.
- Do not bypass package selection by silently forcing `SIMULATION=mini-millennium`.
- Do not duplicate baseline skip logic in every test file.

## Expected Behavior Examples

`make tests` with defaults:

- Builds `MODEL=sage16 SIMULATION=mini-millennium`.
- Runs core invariant tests on the fast mini fixture.
- Runs mini-Millennium selected-simulation tests.
- Runs default baseline comparisons.
- Runs SAGE default full-physics baseline.

`make tests-integration SIMULATION=millennium`:

- Builds with `simulations/millennium/halo_properties.yaml`.
- Generates test run files from `simulations/millennium/_tests/input/test_simulation.yaml`.
- Runs core invariant tests on the declared fast fixture (which uses mini-Millennium tree data).
- Runs Millennium selected-simulation tests against that same fixture.
- Skips committed mini-Millennium baseline comparisons with an explicit message.

Future `make tests-full SIMULATION=millennium`:

- Builds with `simulations/millennium/halo_properties.yaml`.
- Uses production `simulations/millennium/simulation_info.yaml`.
- Runs explicit full-data checks and accepts long runtime/resource requirements.

---

## Post-v1.0 Notes and Known Gaps

These are the things the team deliberately deferred to ship v1.0. Nothing here is a blocker for the release, but each item represents a real limitation that will need addressing as the package model grows.

### 1. The Millennium fast fixture tests the wrong thing

`simulations/millennium/_tests/input/test_simulation.yaml` reuses the shared mini-Millennium tree data. When `SIMULATION=millennium` is selected, fast-tier tests including `test_tree_preservation.py` are running the Millennium-compiled binary against mini-Millennium data.

**What this means in practice:** The tree preservation tests pass for Millennium because the LHalo binary format is the same across Millennium variants. What they do not verify is that Millennium-specific halo properties are correctly mapped, that Millennium-scale IDs behave correctly, or that the Millennium property schema produces valid output for Millennium-origin input.

**What to do:** When production Millennium data is available in CI or a declared test catalog exists, provide a genuine Millennium fixture — a single small tree file from an actual Millennium run — and update `test_simulation.yaml` to point at it. At that point the fixture comment and this note can both be removed.

**Risk if not addressed:** Adding a new Millennium-specific halo property and forgetting to wire it correctly will not be caught by fast-tier tests; the test will pass because it's reading mini-Millennium data where that field doesn't matter.

### 2. No `make tests-slow` target exists yet

The doc describes `make tests-slow` and `make tests-full SIMULATION=millennium` as the preferred future shape for production-scale validation. Neither target exists. Full-data tests cannot currently be run in a structured, documented way.

**What to do:** Add `tests-slow` as a Makefile target. Gate it on an environment variable or explicit opt-in flag. Document which tests belong to it, what data they require, and expected runtime. Per-simulation `simulations/<simulation>/_tests/slow/` directories can hold the scripts; the Makefile target discovers and runs them.

### 3. Skip visibility is now explicit

`skip_non_default_baseline()` raises `TestSkipped`, and Python test runners emit structured skip markers. Summary-mode output now surfaces deliberate skips separately from passes.

**What to do:** Keep new Python tests on the standard marker loop (`TestSkipped` plus `result_skip`) so skip visibility remains consistent.

### 4. The `_makefile_default` parser is fragile

`harness.py` reads `DEFAULT_MODEL` and `DEFAULT_SIMULATION` from the Makefile by scanning for `:=` assignments. This is intentionally narrow — it handles the current Makefile structure correctly — but would silently fall back to hardcoded values if the Makefile used `?=`, multi-line assignment, or an `include` to set those variables.

**What to do:** If the Makefile structure changes, verify that `default_model()` and `default_simulation()` still return correct values in tests. The fallback values ("sage", "mini-millennium") happen to match the current defaults, so a silent failure would not cause test errors — it would just mean the harness always thinks it's on the default combo. Add a sanity assertion if this becomes a concern: compare `_makefile_default("DEFAULT_MODEL", None)` against `None` and fail loudly if the Makefile can't be parsed.

### 5. No per-combo baselines

The baseline gate is binary: either you're on the default combo and baselines run, or you're not and they skip. There is no mechanism to commit a Millennium baseline and have it run only when `SIMULATION=millennium` is selected.

**What to do:** This is only worth adding if a non-default combo becomes release-critical (e.g., a Millennium release). The pattern would be: add `tests/baselines/<MODEL>/<SIMULATION>/` directories, extend `is_default_baseline_combo()` into a more general `baseline_dir_for_combo()` helper, and update baseline tests to check whether a combo-specific baseline directory exists before comparing. If it exists, run the comparison; if not, skip. This keeps the same DRY baseline-gating logic without requiring baselines for every combo.
