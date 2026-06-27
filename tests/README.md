# Mimic Test Suite

Quick reference for running tests. See [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md#testing) for complete testing documentation.

## Table of Contents

1. [Quick Start](#quick-start)
2. [Test Tiers](#test-tiers)
3. [Structured Markers and Summary Mode](#structured-markers-and-summary-mode)
4. [Running Individual Tests](#running-individual-tests)
5. [Directory Structure](#directory-structure)
6. [Test Data](#test-data)
7. [Writing Tests](#writing-tests)
8. [Troubleshooting](#troubleshooting)
9. [Documentation Directory](#documentation-directory)

## Quick Start

```bash
# Run all tests (from mimic root directory)
make tests

# Run specific test tiers
make tests-unit          # C unit tests
make tests-integration   # Python integration tests
make tests-scientific    # Scientific validation
```

Append "summary" to suppress most output and only show warnings, failures, skipped tests, and final suite outcomes (e.g. `make tests summary`).

NOTE: `MODEL` and `SIMULATION` default to `sage16` and `mini-millennium`. Change `DEFAULT_MODEL` and `DEFAULT_SIMULATION` in the `Makefile`, or override them per command, when you want a different package pair.

## Test Tiers

**Unit Tests** (`tests/unit/`)
- C-based tests for individual functions and modules
- Can take up to about 3 minutes for the selected package pair
- Core tests cover memory management, I/O, generated properties, and infrastructure
- Selected-simulation tests come from `simulations/<SIMULATION>/_tests/unit/`
- Selected-model tests come from `models/<MODEL>/modules/**/_tests/` and `models/<MODEL>/modules/_tests/`

**Integration Tests** (`tests/integration/`)
- Python-based end-to-end workflow tests
- Can take up to about 3 minutes for the selected package pair
- Core tests cover pipeline execution, output formats, and model-neutral contracts
- Selected-simulation tests come from `simulations/<SIMULATION>/_tests/integration/`
- Selected-model integration tests come from `models/<MODEL>/modules/**/_tests/`

**Scientific Tests** (`tests/scientific/`)
- Python-based physics validation
- Usually quicker than the other tiers for the shipped configuration, around tens of seconds
- Core scientific tests validate model-neutral scientific contracts
- Selected-simulation tests come from `simulations/<SIMULATION>/_tests/scientific/`
- Selected-model scientific tests come from `models/<MODEL>/modules/**/_tests/`

The `make MODEL=<name> SIMULATION=<name> tests-unit`, `tests-integration`, `tests-scientific`, and `tests` targets run core tests, selected-simulation tests, and, for full-validation simulations, tests declared by the selected model package. Empty generated lists are valid; a tier with no model or simulation tests still runs the core tests and exits successfully.

Full model validation runs for `mini-millennium`, `micro-uchuu`, `micro-uchuu-hdf5`, and `micro-uchuu-ascii`. The three micro-Uchuu packages intentionally use their production `simulation_info.yaml` files so the same small catalogue validates the L-Halo binary, Consistent-Trees HDF5, and Consistent-Trees ASCII reader paths. Larger packages such as `millennium`, `mini-uchuu`, and `uchuu` run core and selected-simulation tests against fixture-sized inputs and skip selected-model physics tests; they rely on the default and micro catalogues for full model validation.

## Structured Markers and Summary Mode

Every C unit, Python integration, and Python scientific test should emit a structured result marker:

```text
MIMIC_RESULT: PASS <test_name>
MIMIC_RESULT: FAIL <test_name> [-- <reason>]
MIMIC_RESULT: SKIP <test_name> [-- <reason>]
MIMIC_RESULT: WARN <test_name> [-- <reason>]
MIMIC_RESULT: ERROR <test_name> [-- <reason>]
```

Summary mode filters these markers directly. Pass markers are suppressed; failures, skips, warnings, and errors stay visible.

- C tests use `TEST_MARKER_*`, `TEST_RUN`, and `TEST_ASSERT*` from `tests/framework/test_framework.h`. Use `return TEST_SKIP_WITH("reason")` when a test cannot run in the current configuration.
- Python tests use `result_pass`, `result_fail`, `result_skip`, `result_warn`, `result_error`, and `TestSkipped` from `tests/framework`.

## Running Individual Tests

Run commands from the repository root so relative paths match the test fixtures.

For full or long-running test sessions, capture a log and check the exit code explicitly:

```bash
mkdir -p archive/test-logs
make tests > archive/test-logs/tests.log 2>&1
test_rc=$?
tail -n 80 archive/test-logs/tests.log
rg -n "^MIMIC_RESULT: (FAIL|SKIP|WARN|ERROR)" archive/test-logs/tests.log
rg -n -i "traceback|fatal|segmentation fault" archive/test-logs/tests.log
echo "exit_code=${test_rc}"
```

Treat any non-zero exit code as a failure.

**Unit tests**:
```bash
tests/unit/run_tests.sh test_memory_system
tests/unit/run_tests.sh test_unit_sage_apply_cooling
```

Unit tests are compiled on demand through the runner. Use the test name without `.c`; the runner refreshes generated module/test registries before building. Add `MODEL=<name> SIMULATION=<name>` when testing a non-default package pair.

**Integration tests**:
```bash
python3 tests/integration/test_full_pipeline.py
python3 tests/integration/test_output_formats.py
python3 models/sage16/modules/sage_apply_cooling/_tests/test_integration_sage_apply_cooling.py
```

Integration tests are plain Python scripts. You can run core tests under `tests/integration/`, simulation-owned tests under `simulations/<simulation>/_tests/integration/`, or module-specific scripts under `models/<model>/modules/<module>/_tests/`. Shared core and simulation test run files are generated under `build/generated/test_inputs/<MODEL>/<SIMULATION>/`; set both explicitly when the built executable is not the default sage16/mini-Millennium build.

**Scientific tests**:
```bash
python3 tests/scientific/test_scientific.py
```

Scientific validation currently has one repository-level script in `tests/scientific/`. Future simulation or module scientific tests can be run the same way with `python3 path/to/test.py`.

## Directory Structure

```
tests/
├── unit/               # C unit tests
├── integration/        # Python integration tests
├── scientific/         # Core scientific validation tests
├── framework/          # Shared test utilities (harness, markers, runner, data loader,
│                       #   comparison helpers, C/Python test templates, and framework headers)
├── data/               # Shared mini simulation data, output fixtures, baselines
└── generated/          # Auto-generated test metadata
```

User-facing model run files live under `models/<model>/input/`. Model-owned test inputs live beside the owning model tests, for example under `models/<model>/modules/_tests/input/`.
Simulation-owned tests live under `simulations/<simulation>/_tests/`.
Generated shared test run files live under `build/generated/test_inputs/<MODEL>/<SIMULATION>/`. Run `make MODEL=<name> SIMULATION=<name> generate-test-inputs` to materialize them manually; direct Python harness usage also generates missing files on demand.

## Test Data

The default full suite uses mini-Millennium simulation data, automatically downloaded by `./scripts/first_run.sh`.

Location: `simulations/mini-millennium/snapshots/`

The micro-Uchuu full-validation suites use their package `simulation_info.yaml` data paths. Tests that require locally mounted production data should skip cleanly when that data is absent.

## Writing Tests

See [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md#testing) for:
- Writing unit tests
- Writing integration tests
- Writing scientific tests
- Test framework utilities

## Troubleshooting

**Tests fail after code changes**: Run `make clean && make` before testing

**Missing test data**: Run `./scripts/first_run.sh` to download

**Integration or scientific tests fail**: Ensure Python environment activated (`source mimic_venv/bin/activate`)

**Wrong model or simulation at runtime**: Rebuild with the same selectors as the run file, for example `make MODEL=sham SIMULATION=mini-millennium` for the SHAM/mini-Millennium package pair. Mimic fails fast if a run file selects a model or simulation property package that does not match the executable.

**Unit test command not found**: Do not run `./test_memory_system.test` directly from `tests/unit/`; use `MODEL=<name> SIMULATION=<name> tests/unit/run_tests.sh <test_name>` so the binary is rebuilt with current generated sources.

**Need more detail**: See [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md#testing)

## Documentation Directory

- [README.md](../README.md): project overview and shortest path to a first result
- [docs/VISION.md](../docs/VISION.md): architectural principles and design boundaries
- [docs/USER-GUIDE.md](../docs/USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): detailed plotting manual
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
