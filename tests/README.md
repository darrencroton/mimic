# Mimic Test Suite

Quick reference for running tests. See [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md#testing) for complete testing documentation.

## Quick Start

```bash
# Run all tests (from mimic root directory)
make tests

# Run specific test tiers
make tests-unit          # C unit tests
make tests-integration   # Python integration tests
make tests-scientific    # Physics validation
```

Append "summary" to suppress most output and only show warnings, failures, skipped tests, and final suite outcomes (e.g. `make tests summary`).

NOTE: `MODEL` and `SIMULATION` default to `sage16` and `mini-millennium`. Change `DEFAULT_MODEL` and `DEFAULT_SIMULATION` in the `Makefile`, or override them per command, when you want a different package pair.

## Test Tiers

**Unit Tests** (`tests/unit/`)
- C-based tests for individual functions and modules
- Fast (<10 seconds total)
- Core tests cover memory management, I/O, generated properties, and infrastructure
- Selected-simulation tests come from `simulations/<SIMULATION>/_tests/unit/`
- Selected-model tests come from `models/<MODEL>/modules/**/_tests/` and `models/<MODEL>/modules/_tests/`

**Integration Tests** (`tests/integration/`)
- Python-based end-to-end workflow tests
- Medium speed (<1 minute total)
- Core tests cover pipeline execution, output formats, and model-neutral contracts
- Selected-simulation tests come from `simulations/<SIMULATION>/_tests/integration/`
- Selected-model integration tests come from `models/<MODEL>/modules/**/_tests/`

**Scientific Tests** (`tests/scientific/`)
- Python-based physics validation
- Slower (<5 minutes total)
- Core scientific tests validate model-neutral scientific contracts
- Selected-simulation tests come from `simulations/<SIMULATION>/_tests/scientific/`
- Selected-model scientific tests come from `models/<MODEL>/modules/**/_tests/`

## Running Individual Tests

Run commands from the repository root so relative paths match the test fixtures.

For full or long-running test sessions, capture a log and check the exit code explicitly:

```bash
mkdir -p archive/test-logs
make tests > archive/test-logs/tests.log 2>&1
test_rc=$?
tail -n 80 archive/test-logs/tests.log
rg -n -i "failed|error|traceback" archive/test-logs/tests.log
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

The `make MODEL=<name> SIMULATION=<name> tests-unit`, `tests-integration`, `tests-scientific`, and `tests` targets run core tests, selected-simulation tests, and tests declared by the selected model package. Empty generated lists are valid; a tier with no model or simulation tests still runs the core tests and exits successfully.

## Directory Structure

```
tests/
├── unit/               # C unit tests
├── integration/        # Python integration tests
├── scientific/         # Core scientific validation tests
├── framework/          # Shared test utilities
│   └── data_loader.py  # Binary/HDF5 data loading
├── data/               # Shared mini simulation data, output fixtures, baselines
└── generated/          # Auto-generated test metadata
```

User-facing model run files live under `models/<model>/input/`. Model-owned test inputs live beside the owning model tests, for example under `models/<model>/modules/_tests/input/`.
Simulation-owned tests live under `simulations/<simulation>/_tests/`.
Generated shared test run files live under `build/generated/test_inputs/<MODEL>/<SIMULATION>/`. Run `make MODEL=<name> SIMULATION=<name> generate-test-inputs` to materialize them manually; direct Python harness usage also generates missing files on demand.

## Test Data

Test suite uses mini-Millennium simulation data, automatically downloaded by `./scripts/first_run.sh`.

Location: `simulations/mini-millennium/snapshots/`

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
