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
make tests-converter     # ctrees->horizontal-HDF5 converter self-tests (convert/mimic-convert/tests/)
make tests-horizontal-v3  # v3 reader, retention and identity battery + package tests (including the chunked-sweep test) on fixtures (own CI job, which also runs `make tests-snapshot-global`)
make tests-snapshot-global  # post_snapshot phase, typed callback/schema, sham_rank_match and hod_populate batteries on fixtures
make tests-snapshot-global-sham  # sham_rank_match unit and end-to-end tests (also run by tests-snapshot-global)
make tests-snapshot-global-hod  # hod_populate unit and end-to-end tests (also run by tests-snapshot-global)
make tests-snapshot-global-identity  # manual: disabled-mode output identity vs the pre-feature reference commit
make tests-distributed  # manual, needs MPI: serial vs MPI identity at -np 1,2,3,4,8, plus chunked legs (forest_chunks 2,3,8 and -np 2x2, 3x3), on the forest_blocks fixture (MPIRUN="mpirun --oversubscribe" on small machines)
make check-horizontal-fixture  # Committed horizontal-fixture conformance vs the frozen format spec
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

The `make MODEL=<name> SIMULATION=<name> tests-unit`, `tests-integration`, `tests-scientific`, and `tests` targets run core tests, selected-simulation tests, and, for full-validation simulations, tests declared by the selected model package; because the tiers run a TEST_BUILD executable, their registries are generated with `MIMIC_TEST_BUILD=1`, which also admits the framework fixtures' own declared tests (currently `src/module_system/test_fixture/_tests/`). Empty generated lists are valid; a tier with no model or simulation tests still runs the core tests and exits successfully. `make tests` additionally runs the two package-independent checks: the converter's stdlib-unittest suite (`tests-converter`, ~10 s, needs the `mimic_venv` Python stack) and the committed horizontal-fixture conformance check (`check-horizontal-fixture`). `make tests-horizontal-v3` is separate and needs no real dataset: it builds `MODEL=halos-only SIMULATION=mini-millennium-horizontal` itself and runs the version 3 reader and retention C tests plus the package's gap-retention, schema-conformance and chunked-sweep tests on the committed fixtures (`_tests/integration/test_chunked_sweep.py` runs `halos-only` on `forest_blocks` at `forest_chunks` 1, 2, 3 and 8 and 2 with `--compress`, requiring identity and partition row order against `G = 1`, idle chunks at 8, and the multi-visit failure window), failing on any unexpected skip (these tests skip under the default pair, so `make tests` cannot cover them); CI runs it as its own job. `make tests-snapshot-global` is likewise separate and needs no real dataset; it calls `tests/manual/run_snapshot_global_battery.py`, which in sequence builds `MODEL=halos-only` on `micro-uchuu-ascii-horizontal` and `mini-millennium-horizontal` as test builds and runs `tests/integration/test_snapshot_phase.py` on each, runs the typed-callback C unit test (`test_snapshot_module_contract`) and `tests/integration/test_snapshot_module_schema.py`, and finishes with the `sham` group (the `sham_rank_match` unit and end-to-end tests under `MODEL=sham` on the micro-Uchuu fixture), which `make tests-snapshot-global-sham` runs alone (`--only sham`), and the `hod` group (the `hod_populate` unit test and the fixture cases of its end-to-end test under `MODEL=hod` on the micro-Uchuu fixture; the test file's vertical cases are not in this group and run in the integration tier of a `MODEL=hod SIMULATION=mini-millennium` build), which `make tests-snapshot-global-hod` runs alone (`--only hod`). It takes about 30 s with warm builds (30 s measured on 2026-10-04 across its four test-build pairs, four groups and 109 cases); a cold first run rebuilds each pair and takes longer. Marker policy for both battery targets (the identity test gates only on exit status and on FAIL, ERROR and SKIP markers, with no declared count): a non-zero exit, any `MIMIC_RESULT: FAIL`, `ERROR` or `SKIP` (these batteries have no legitimate skip), or a PASS-plus-WARN count different from the cases the test file declares (`def test_` or `TEST_RUN(`; for the `hod` integration file only its `test_fixture_*` cases, which the group selects with `--cases fixture`) fails the step; a `WARN` is surfaced in the summary but is not fatal, as in `make tests`. The output goes to `build/snapshot_global_tests.log` (or `build/snapshot_global_sham_tests.log` or `build/snapshot_global_hod_tests.log`). The runner regenerates the caller's `MODEL`/`SIMULATION` generated code in a `try`/`finally`, with SIGHUP, SIGINT, SIGQUIT and SIGTERM converted to `SystemExit` so the restore also runs on an interrupt, and a failed restore fails the target; the one residual is SIGKILL, after which `make generate` with your `MODEL` and `SIMULATION` restores them. The executable is left a TEST_BUILD binary (rebuild it with `make`). It never launches a real-data gate or the reference-commit comparison. The snapshot-phase tests run `test_snapshot_fixture`, which probes `module_emit_event` from inside its callback; the core registry logs one expected `ERROR` line per snapshot (`module_emit_event called during post_snapshot dispatch`) when it rejects the probe, while the fixture logs only its INFO marker; that line is the assertion's evidence, not a failure. Model tests are not registered for horizontal packages, so these targets invoke the declared tests by path instead of widening `FULL_MODEL_TEST_SIMULATIONS`.

**Manual tests (`tests/manual/`).** Files under `tests/manual/` are explicitly invoked and never auto-discovered: `scripts/generate_test_registry.py` globs only `tests/unit`, `tests/integration`, `tests/scientific` and the packages' own `_tests/` directories, never `tests/manual/`, so nothing here enters `make tests`. `tests/manual/test_snapshot_disabled_identity.py` (run by `make tests-snapshot-global-identity`) and `tests/manual/test_distributed_identity.py` (run by `make tests-distributed`, described below) are the two such tests (`tests/manual/run_snapshot_global_battery.py` beside them is the battery runner, not a test). The disabled-mode identity test requires, for `halos-only` and `sage16`, fixed and dynamic timestepping, on the committed vertical, version 2 and gapped version 3 fixtures, that a run with `modules.post_snapshot` absent and a run with `post_snapshot: []` each reproduce the reference run's per-ID bytes (through the unchanged `scripts/compare_cross_format_identity.py`), files, HDF5 objects, master-file external links, datasets, attributes and metadata, apart from named provenance differences, and it proves its own comparator against 38 mutation and control cases (32 injected defects rejected, 6 controls accepted). The reference is pinned in the test as `REFERENCE_COMMIT_DEFAULT` (the pre-feature commit `501bac12f654d9622b797bc9b26c536e5385aca2`, with an in-file re-anchor procedure); `REFERENCE_COMMIT=<hash>` overrides it outright, and either way the commit must be an ancestor of HEAD with no feature symbol, carry every package and run-file path the legs select, and ship the same `sage16_mini-millennium.yaml` as HEAD. It needs no real dataset and takes about a minute; it writes `build/snapshot_global_identity.log` itself and exits non-zero on any FAIL, ERROR or SKIP marker and surfaces WARN (it has no declared case count). Build worktrees are cached by commit under `output/snapshot-global-identity/worktrees/<commit12>__<model>__<simulation>` (the gates' scratch parent), created only when missing, verified to be a clean checkout of their commit (otherwise the test stops rather than reuse one) and rebuilt incrementally, so a new HEAD adds six registrations and the reference adds none. Runs, logs, mutation copies and `evidence.json` go under `archive/snapshot-global-identity/<stamp>/`. Nothing is removed by the test: when more than 50 `snapshot-global-identity` worktree registrations exist it reports a `WARN`, and the clean-up is manual — move old worktree directories and stamps to cold storage, then run `git worktree prune`, which drops the registrations whose directories are gone without deleting anything. Add a manual test only for evidence that must not run with the tiers, and give it a `make` target.

**Distributed identity gate (`make tests-distributed`).** `tests/manual/test_distributed_identity.py` proves the distributed horizontal driver's acceptance predicate on committed data and needs MPI (`mpicc`, `mpirun`) but no real dataset. It first compiles `tests/mpi/test_snapshot_collectives_mpi.c` with `mpicc -DMPI` against `src/core/snapshot_collectives.c` and the util sources only (the test stubs the running-callback accessor as NONE) and runs it at `-np 3`: the rank collective against a brute-force oracle with an empty task and cross-task ties, NaN and duplicate-id errors agreed on every task, and the reductions and `any` against hand sums. Then, for `halos-only`, `sage16`, `sham` and `hod` on `mini-millennium-horizontal`'s `forest_blocks` fixture (six forests over seven gapped snapshots, through `simulations/mini-millennium-horizontal/_tests/input/forest_blocks_<model>.yaml`), it builds the non-MPI binary and runs serially (requiring, for `sham`, that every `SHAM audit` line assigns and masks at least one rank), then builds `USE-MPI=yes` and runs under `$MPIRUN -np N` for N in 1, 2, 3, 4 and 8: at `-np 1` the output keeps today's unsuffixed names and logs no partition; above it the master holds `File<snap>_task<t>` for every task whose `TotHalosPerSnap` sum to the serial count, and task 0 logs the partition; every count passes `scripts/compare_cross_format_identity.py --compare-created` against the serial output, and for `hod` the comparator must report created rows on both sides at every count. Both binaries are production builds (`TEST_BUILD=no`); the serial one is built with `USE-MPI=` given explicitly and any `USE-MPI` removed from the environment, so it is non-MPI whatever the caller exported. Then come the chunk legs (`input.forest_chunks`): for `halos-only`, `sage16` and `hod` (the last through `_tests/input/forest_blocks_hod_chunked.yaml`, the fixture run file without its `post_snapshot` audit, against a serial run of that variant) it runs serial legs at `forest_chunks` 2, 3 and 8 and MPI legs at `-np 2` with 2 chunks and `-np 3` with 3, each through the comparator with `--compare-created` and a `row_order_` check that every partition's `UniqueGalaxyID` column is in the `G = 1` run's file order (serial legs), with a `chunk_log_` check that the partition headline names the chunk count so a leg cannot pass without chunking, and a `chunked_refused_` check that `sham` and the shipped `hod` run file fail at configuration at `forest_chunks: 2` without reaching "Opened horizontal run". It ends with the version 2 refusal (`halos-only` on `simulations/micro-uchuu-ascii-horizontal/_tests/data/generic/` at `-np 2` must fail at startup with the format_version 2 message). `MPIRUN` (environment, default `mpirun`) is the launcher; use `MPIRUN="mpirun --oversubscribe"` on a machine with fewer than eight cores, as CI does. Every build and launch runs in its own process group under a timeout; a timeout or an interrupt terminates the whole group (SIGTERM, then SIGKILL) before the gate continues or restores generated code. Every check emits a `MIMIC_RESULT:` marker, and any FAIL or SKIP fails the target (it has no legitimate skip). Running the script directly with `--only <model>` is a development aid, not the gate: it ends with a `PARTIAL:` summary and exit status 3. The log is `build/distributed_tests.log`, run outputs go to `output/distributed-identity/gate/` (replaced each run), the caller's generated code is restored in a `finally`, and the executable is left the last MPI build (rebuild it with `make`).

Full model validation runs for `mini-millennium`, `micro-uchuu`, `micro-uchuu-hdf5`, and `micro-uchuu-ascii`. The three micro-Uchuu packages intentionally use their production `simulation_info.yaml` files so the same small catalogue validates the L-Halo binary, Consistent-Trees HDF5, and Consistent-Trees ASCII reader paths. Larger packages such as `millennium`, `mini-uchuu`, and `uchuu` run core and selected-simulation tests against fixture-sized inputs and skip selected-model physics tests; they rely on the default and micro catalogues for full model validation.

**Horizontal packages.** The generic tiers take a package's shape from its own metadata (`input.processing_order`, read by `scripts/discovery.py`), never from its name. For a horizontal package `scripts/generate_test_inputs.py` writes HDF5-only run files with no input file range, pointed at the package's committed fixture by `simulations/<package>/_tests/input/test_simulation.yaml` and requesting that fixture's last snapshot (plus `core/test_binary.yaml`, which only the C unit tier parses); `create_test_param_file()` and `default_run_file()` then pick the HDF5 run file. A test that needs the vertical path (binary output, `--skip`, an input file range, forest partitioning, MPI ranks or a vertical reader) skips through `skip_if_selected_package_is_horizontal()` with a reason naming what it needs. Six horizontal packages run the generic unit and integration tiers on committed, test-sized fixtures: `mini-millennium-horizontal` (`_tests/data/worked_graph/`), `micro-uchuu-ascii-horizontal` (`_tests/data/generic/`, the contract fixture's forests at the package's own snapshot spacing), `millennium-horizontal`, `mini-uchuu-horizontal` and `micro-uchuu-horizontal` (each the synthetic `worked_graph` forest converted under its source package's own metadata, rebuilt by its `_tests/data/regenerate.sh`) and `shin-uchuu` (the version 2 micro-Uchuu fixture forests under Shin-Uchuu's metadata and snapshot spacing, `_tests/data/regenerate.sh`). These fixtures exercise each package's compiled schema, reader and driver, not its halo population. `micro-uchuu-hdf5-horizontal` ships no fixture, because no existing generator writes a Consistent-Trees HDF5 source of test size; its generated manifest records the skip reason and every run-file-driven test skips with it, so nothing opens its production dataset. No horizontal package runs its model's own tests (`FULL_MODEL_TEST_SIMULATIONS` in `scripts/discovery.py`): the sage16 module integration tests read binary output. Runtime claims for a horizontal package still rest on its parity gate (`make MODEL=halos-only SIMULATION=<package> tests-scientific`, on a machine holding both datasets), `make tests-horizontal-v3` and `make check-horizontal-fixture`.

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
- A horizontal package's parity gate (`_tests/scientific/test_cross_format_identity.py`) is one `GatePackage` over `tests/framework/parity_gate.py`, and its schema test (`_tests/integration/test_schema_conformance.py`) one `SchemaPackage` over `tests/framework/schema_conformance.py`; add checks to the framework module, not to a package file. The pure run-file helpers are tested in the core tier by `tests/integration/test_parity_gate_helpers.py`.

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

Scientific validation has two repository-level scripts in `tests/scientific/`: `test_scientific.py` (model-neutral scientific contracts) and `test_compare_cross_format_identity.py` (an adversarial self-test of `scripts/compare_cross_format_identity.py`, the comparator behind the cross-format identity gate — it synthesises its own HDF5, needs no dataset and no Mimic run, and takes seconds). Future simulation or module scientific tests can be run the same way with `python3 path/to/test.py`.

## Directory Structure

```text
tests/
├── unit/               # C unit tests
├── integration/        # Python integration tests
├── scientific/         # Core scientific validation tests
├── manual/             # Explicitly invoked tests; never auto-discovered (see "Manual tests")
├── framework/          # Shared test utilities (harness, markers, runner, data loader,
│                       #   comparison helpers, C/Python test templates, and framework headers;
│                       #   parity_gate.py and schema_conformance.py carry the horizontal
│                       #   packages' parity gates and schema-conformance tests)
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

**Wrong model or simulation at runtime**: Rebuild with the same selectors as the run file, for example `make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal` for the SHAM/micro-Uchuu fixture package pair. Mimic fails fast if a run file selects a model or simulation property package that does not match the executable.

**Unit test command not found**: Do not run `./test_memory_system.test` directly from `tests/unit/`; use `MODEL=<name> SIMULATION=<name> tests/unit/run_tests.sh <test_name>` so the binary is rebuilt with current generated sources.

**Need more detail**: See [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md#testing)

## Documentation Directory

- [README.md](../README.md): project overview and shortest path to a first result
- [docs/VISION.md](../docs/VISION.md): architectural principles and design boundaries
- [docs/USER-GUIDE.md](../docs/USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [docs/STYLE-GUIDE.md](../docs/STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- [plot/mimic-plot/README.md](../plot/mimic-plot/README.md): detailed plotting manual
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
