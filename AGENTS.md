# AGENTS.md — Mimic

This file provides guidance to AI coding assistants (Claude Code, Codex CLI, etc.) working in the Mimic repository. Global behavioural rules (code quality, git workflow, file organisation) are defined in the global AGENTS.md and apply here without repetition.

---

## Quick Setup

For new repository clones, use the automated setup script:

```bash
# Complete setup from fresh clone
./scripts/first_run.sh

# Creates directories, downloads data, sets up Python environment
# Creates mimic_venv/ virtual environment with plotting dependencies
```

---

## Build Commands

```bash
# Basic compilation
make

# Note: Property code auto-regenerates during `make` when YAML changes.
# MODEL selects the model set and SIMULATION selects the simulation/catalog
# property package. They default to DEFAULT_MODEL (sage16) and DEFAULT_SIMULATION
# (mini-millennium) in the Makefile, so plain `make` builds the defaults; override
# both per invocation when testing other combinations. If either package is
# missing (renamed/removed), make fails loudly with "Unknown MODEL" or
# "Unknown SIMULATION" rather than mis-building. Mimic builds one model set and
# one simulation package at a time.
#
# IMPORTANT: use the same MODEL/SIMULATION pair for generate, validate-modules,
# tests, and make. Mixing selectors produces silently inconsistent output.

# Show build configuration (library detection, compiler, enabled features)
make info

# Regenerate all code from metadata (smart — only regenerates what changed)
# Run after editing property YAML files or module metadata
make generate

# Verify generated code is up-to-date (CI check)
make check-generated

# Validate documentation links and anchors
make check-docs

# Validate module metadata (checks dependencies, properties, files)
make validate-modules

# HDF5 is enabled by default; disable with:
make USE-HDF5=no

# With MPI support for parallel processing
make USE-MPI=yes

# Parallel build (faster — choose a local core count)
make -j$(sysctl -n hw.ncpu)  # macOS
make -j$(nproc)              # Linux

# Clean build artifacts
make clean

# Remove object files but keep executable
make tidy
```

---

## Code Architecture

For comprehensive details see `docs/DEVELOPER-GUIDE.md` (architecture, module development, API reference) and `docs/USER-GUIDE.md` (configuration, pipeline setup, output formats). Architecture planning documents live in `docs/dev/` — consult these before making structural changes.

Standalone modules (a single `.c` file directly under `models/<model>/modules/`) are valid for prototyping. Convert to a directory module (its own subdirectory with `module_info.yaml`) once tests, metadata, events, or `compilation_requires` matter. See the `mimic-modules` skill for the full module development workflow.

### Directory Structure

```
src/
├── core/          Core execution (main, init, build_model, parameter reading)
│   └── core_properties.yaml    Core halo property metadata (auto-generates C code)
├── io/
│   ├── tree/      Tree readers (binary, HDF5 formats)
│   └── output/    Output writers (binary, HDF5)
├── util/          Utilities (memory, error, numeric, version, I/O)
├── module_system/ Framework infrastructure (do not modify)
│   ├── physical_constants.h  Universal physical constants (G, c, Z_sun, etc.)
│   ├── output_helpers.h      Output formatting utilities
│   ├── generated/            Auto-generated module registration
│   ├── template/             Template for creating new modules
│   └── test_fixture/         Infrastructure testing module
└── include/       Headers (types, globals, constants)
    └── generated/ Auto-generated property code and model parameter validation

models/
├── sage16/
│   ├── input/    SAGE run parameter YAML files
│   ├── model_properties.yaml
│   ├── modules/  SAGE physics modules and module-local tests
│   ├── shared/   SAGE-local helper APIs
│   └── plots/    SAGE plotting figures and profiles
├── sham/
│   ├── input/    SHAM run parameter YAML files
│   ├── model_properties.yaml
│   ├── modules/  SHAM physics modules and module-local tests
│   └── plots/    SHAM plotting figures and profiles
└── halos-only/   Empty pipeline: halo tracking only, no galaxy physics

simulations/
├── mini-millennium/    mini-Millennium metadata, halo properties, and snapshot lists
├── millennium/         full Millennium metadata (tree data not bundled; symlink your own)
├── micro-uchuu/        micro-Uchuu L-Halo binary package
├── micro-uchuu-hdf5/   micro-Uchuu Consistent-Trees HDF5 package
├── mini-uchuu/         mini-Uchuu package
└── uchuu/              full Uchuu Consistent-Trees HDF5 package

build/generated/     Build-time generated files (git_version.h, test lists, module registry)
tests/               Unit, integration, and scientific tests
plot/mimic-plot/     Plotting system (registry is model-local; sage16 ships 18 snapshot + 4 evolution plots)
  ├── tests/         Plotting system tests (unit and integration)
  └── output_schema.py  Run-local schema reader for binary outputs
```

### Key Concepts

**Halo Data Structures:** tree input/gather → `FoFWorkspace` (processing workspace) → shared output buffer → binary/HDF5 writers. The tree driver backs the output buffer with `ProcessedHalos`, which also provides already-processed progenitor state.

**Property System:** Properties are defined in YAML (`src/core/core_properties.yaml`, `simulations/<SIMULATION>/halo_properties.yaml`, `models/<MODEL>/model_properties.yaml`) and generated via `make MODEL=<name> SIMULATION=<name> generate` into C structs, init/output logic, HDF5 metadata writers, and run-local binary output schemas.

**Model/Simulation Boundary:** Mimic compiles one model set and one simulation property package at a time via `MODEL=<name>` and `SIMULATION=<name>`. Run files declare `model.name` and `simulation.name`; Mimic derives package paths and property metadata paths from `models/<model>/` and `simulations/<simulation>/`. A model package must be self-contained for running and plotting: run parameter YAML files, properties, modules, model-local `shared/` helpers, tests, and plot figures should live under `models/<model>/`. A simulation package owns catalog halo properties and tree-format fixtures under `simulations/<simulation>/`. To mix modules from different model families, create a new model package and reconcile property names, parameter names, units, dependencies, tests, and plots there.

**Reader/Driver Boundary:** `input.tree_type` selects the on-disk merger-tree reader format. `input.processing_order` selects the processing driver and defaults to `tree_ordered`; `snapshot_ordered` is recognized but fails fast until the snapshot driver exists. Current registered readers declare `tree_ordered` compatibility; do not overload `tree_type` with processing-order meaning.

**Module System:** Runtime-configurable physics modules execute through fixed optional `pre_timestep`/`post_timestep` lifecycle phases plus ordered user-named substep phases under `modules.phases:`. Supported processing modes are `PROCESSING_MODE_FULL_HALO`, `PROCESSING_MODE_PER_EVENT`, and `PROCESSING_MODE_BY_GALAXY`. Module lifecycle: `init()` → `process()` → `cleanup()`.

**Parameters:** Model parameters come from the input YAML and are accessed via typed getters (`model_get_double()`, `model_get_int()`, `model_get_string()`), with module-local validation in each module `init()`.

**Memory:** Custom allocator with leak detection and categorised tracking (`MEM_GALAXIES`, `MEM_HALOS`, `MEM_TREES`, `MEM_IO`, `MEM_UTILITY`).

**Output:** Binary/HDF5 structure documented in `docs/USER-GUIDE.md#output`.

---

## Running Mimic

```bash
# Basic execution
./mimic models/sage16/input/sage16_mini-millennium.yaml
./mimic models/sham/input/sham_mini-millennium.yaml    # SHAM model

# Verbosity options
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml    # Most verbose (debug output + context)
./mimic --verbose models/sage16/input/sage16_mini-millennium.yaml  # Add context (timestamp, file:line)
./mimic --quiet models/sage16/input/sage16_mini-millennium.yaml    # Warnings/errors only
./mimic --skip models/sage16/input/sage16_mini-millennium.yaml     # Skip existing output files
./mimic --compress models/sage16/input/sage16_mini-millennium.yaml # gzip HDF5 galaxy output (off by default)
```

---

## Testing

**Test templates:** `tests/framework/c_unit_test_template.c`, `tests/framework/python_integration_test_template.py`, `tests/framework/python_scientific_test_template.py` — start from these when writing new tests. See the `mimic-tests` skill for tier selection, location rules, and registration.

Long-running tests should be captured to a log file; check the exit code explicitly:

```bash
mkdir -p archive/test-logs
make tests-unit > archive/test-logs/tests-unit.log 2>&1
test_rc=$?
tail -n 60 archive/test-logs/tests-unit.log
rg -n -i "failed|error|traceback" archive/test-logs/tests-unit.log
echo "exit_code=${test_rc}"
# Treat any non-zero exit code as failure, regardless of log text
```

### Testing Strategy

Unit and integration tests each take up to 3 minutes and produce large output. Scientific tests are faster (~30s). **Delegate unit and integration to a subagent**: have it capture and summarise the results, then act on the report in the main context rather than filling it with raw test output.

```bash
# Run all tests (validates metadata first, then all tiers)
make tests

# Show only warnings, failures, skipped tests, and final suite outcomes
make tests summary
make tests-unit summary
make tests-integration summary
make tests-scientific summary

# Run specific tiers
make validate-modules   # Validate module metadata only
make tests-unit          # C unit tests (up to 3min, large output — delegate)
make tests-integration   # Python integration tests (up to 3min, large output — delegate)
make tests-scientific    # Python scientific validation (fast, ~30s)
```

Individual tests: use `tests/unit/run_tests.sh <test_name>` for C unit tests and `python3 path/to/test.py` for integration/scientific scripts.

### Structured Marker Protocol

Every test — C unit, Python integration, and Python scientific — emits structured result lines to stdout that the summary filter grabs deterministically:

```
MIMIC_RESULT: PASS <test_name>
MIMIC_RESULT: FAIL <test_name> [-- <reason>]
MIMIC_RESULT: SKIP <test_name> [-- <reason>]
MIMIC_RESULT: WARN <test_name> [-- <reason>]
MIMIC_RESULT: ERROR <test_name> [-- <reason>]
```

Summary mode filters structured markers directly: pass markers are suppressed, while fail, skip, warning, and error markers are shown. New tests **must** emit these markers; the protocol lives in:

- **C tests:** `TEST_MARKER_PASS / TEST_MARKER_FAIL` macros in `tests/framework/test_framework.h` — emitted automatically by `TEST_RUN` and `TEST_ASSERT*`. No per-test work required. A test that cannot run in this configuration returns `TEST_SKIP_WITH("reason")` and is reported as a SKIP, not a pass.
- **Python tests:** `result_pass / result_fail / result_skip / result_warn / result_error` helpers in `tests/framework/markers.py` (re-exported via `tests/framework/__init__.py`). Call them in each test's `main()` loop. Raise `TestSkipped` (also from `tests/framework`) to skip a test — the loop catches it and calls `result_skip` automatically.

```bash
# Test data loader
cd tests && python -c "from framework import load_binary_halos; print(load_binary_halos.__doc__)"
```

---

## Code Style

Follow `docs/STYLE-GUIDE.md` for naming, comments, documentation, metadata, tests, generated-code boundaries, logging, and review conventions. The formatter owns mechanical layout; the style guide owns human readability and consistency.

### C
- 2-space indent, LLVM base style, 100-character line limit — enforced by `.clang-format` in the repo root; editors discover it automatically
- **Never hand-edit files under `*/generated/`** — they are produced by `make generate` and will be overwritten; the formatter excludes them automatically
- Code must compile clean under `-Wall -Wextra -Wshadow -Wformat-security -Wundef`; do not introduce new warnings
- Comments explain **why**, not what — one short line maximum; `@file`/`@brief` Doxygen headers are fine on public API files
- `// SAGE parity:` comments mark code that intentionally mirrors legacy SAGE behaviour — do not "fix" without checking the parity baseline first
- Universal physical constants (G, c, Z_sun, etc.) must come from `src/module_system/physical_constants.h`; model-specific scientific constants belong in model-local shared headers
- Never use `printf` in module code — use `DEBUG_LOG`, `VERBOSE_LOG`, `INFO_LOG`, `WARNING_LOG`, `ERROR_LOG`, or `FATAL_ERROR` from `src/util/error.h`

### Python
- black (line-length 100) + isort (profile black) — configuration in `pyproject.toml`
- Scripts must run under Python 3.9+

### Markdown
- Prose must not be manually hard-wrapped — write full paragraphs as single long lines; editors and rendered views soft-wrap automatically
- The 100-character guideline applies to code blocks and YAML/shell examples within Markdown; keep examples readable without horizontal scrolling
- Applies to all `.md` files in the repository: docs, READMEs, and skill files

### Line length
C and Python both use **100 characters**, enforced by the formatter. Markdown code blocks follow the same guideline manually.

### Checking without modifying (what CI runs)
```bash
make check-format
```

---

## Pre-Commit Checklist

Before committing any change, complete all three steps and report the outcome in your final response:

1. **Format** — run `./scripts/beautify.sh`. Run the full formatter regardless of which languages you touched; CI (`make check-format`) checks both C and Python.

2. **Style sweep** — re-read the diff against `docs/STYLE-GUIDE.md`. Fix sub-par local style in touched files even if that style predates your change. Do not expand into whole-repo cleanup. State that the sweep was done, or explain any remaining issue.

3. **Skill sweep** — for any task that touched modules, tests, properties, simulations, plots, or core architecture, review each relevant `.agents/skills/mimic-*` skill file. Update, correct, or add content that is now stale or missing; remove content that is no longer accurate. State that the sweep was done, or note any skill needing a larger update.

---

## Benchmarking

See `./scripts/benchmark_mimic.sh --help`; results are saved to `benchmarks/` (gitignored). For analysis guidance, see `docs/DEVELOPER-GUIDE.md`.

---

## Documentation Reference

- **docs/VISION.md** — Architectural principles and design philosophy
- **docs/USER-GUIDE.md** — Complete user guide (installation, configuration, running simulations)
- **docs/DEVELOPER-GUIDE.md** — Complete developer guide (architecture, modules, testing)
- **docs/dev/** — Architecture planning documents; consult before structural changes
- **`.agents/skills/mimic-*`** — Project-local skills for modules, tests, properties, plots, simulations, and debugging; load when working in those domains
- `models/<model>/README.md` — model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md` — simulation-package data, units, snapshot lists, and maintenance notes
