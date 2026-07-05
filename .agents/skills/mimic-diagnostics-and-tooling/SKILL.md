---
name: mimic-diagnostics-and-tooling
description: "Mimic's measurement toolbox - how to MEASURE instead of eyeball. Load when a task needs to inspect or quantify anything: validator exit codes (validate-modules, check-generated, lint-parameters, check-docs, check-format, make info), grepping MIMIC_RESULT markers from logs, memory leak reports and valgrind, inspecting HDF5 or binary galaxy output (h5ls, h5py, output_schema.json), NaN/Inf/range scanning, baseline comparison tools (compare_halos_comprehensive, baseline_rtol), the pipeline fuzzer (fuzz_pipeline.py), benchmarking (benchmark_mimic.sh), or git-history inspection recipes. This is the toolbox; for WHERE to look when something is broken, use mimic-debugging-playbook."
---

# Mimic Diagnostics and Tooling

Every claim about Mimic's behavior should rest on a measurement, and the repo ships most of the instruments. This skill catalogs them with interpretation guides, plus one composed inspector in this skill's `scripts/`.

## When to use / when NOT to use

Use for: picking and running the right instrument, interpreting its output.

Do NOT use for:
- Triage flow when something is broken (which instrument first, what the symptom means) — see the `mimic-debugging-playbook` skill.
- Writing/registering tests or baseline policy — see the `mimic-validation-and-qa` skill.
- Designing scientific evidence and tolerances — see the `mimic-scientific-method` skill.

## First actions

1. `make info` — the configuration instrument: compiler, selected MODEL/SIMULATION packages, full CFLAGS, YAML/HDF5/MPI detection (with method and version), module/source counts. Read it before trusting any other measurement.
2. Capture, don't scroll: every long-running measurement goes to a file with its exit code recorded (`cmd > log 2>&1; echo rc=$?`).
3. Prefer the repo's own instruments (below) over hand-rolled scripts; compose only when nothing fits.

## 1. The validator battery

| Command | Measures | Exit codes / interpretation |
|---|---|---|
| `make validate-modules` | Module metadata schema + consistency | 1 schema error, 2 declared file missing, 3 dependency (unknown property / unused declared unit), 4 naming violation; 0 clean |
| `make check-generated` | Generated-code drift | Compares the `Source MD5:` hash embedded in each generated file against a recomputation from the YAML + generator; on failure prints the exact `make MODEL=<m> generate` fix. Never diff generated files by eye |
| `make lint-parameters` | Module parameter honesty | 1 = parameter used in C but not declared in `module_info.yaml` (error), 2 = declared but never used (warning), 0 clean. Runs automatically before every build |
| `make check-docs` | Markdown link/anchor integrity + leftover review markers | Non-zero lists each broken link/anchor with file:line |
| `make check-format` | clang-format + black + isort conformance | What CI enforces; fix with `./scripts/beautify.sh` |

## 2. Test markers as a diagnostic surface

Captured suite logs are machine-readable via the marker protocol (`mimic-validation-and-qa` owns the protocol):

```bash
rg -n "^MIMIC_RESULT: (FAIL|ERROR)" archive/test-logs/tests.log     # hard failures
rg -n "^MIMIC_RESULT: SKIP" archive/test-logs/tests.log             # READ EVERY REASON
rg -c "^MIMIC_RESULT: PASS" archive/test-logs/tests.log             # pass count sanity
```

A SKIP audit is a real measurement: a guard checking a stale name once silently skipped the sage16 physics baseline test for weeks while suites reported green.

## 3. Memory measurement

- End-of-run leak report: `check_memory_leaks()` runs at exit and reports leaked block counts and MB per category (`MEM_GALAXIES`, `MEM_HALOS`, `MEM_TREES`, `MEM_IO`, `MEM_UTILITY`); a clean run logs "No memory leaks detected" under `--verbose`. Nonzero blocks at exit = leak; growth during a run is often design (`ProcessedHalos` grows with orphan count × snapshot depth; the galaxy pool bulk-resets per tree — see `mimic-architecture-contract`).
- In-code instruments: `print_allocated()` / `print_allocated_by_category()` (`src/util/memory.h`) for point-in-time snapshots.
- External: `valgrind --leak-check=full ./mimic <run.yaml>` (Developer Guide recipe) when the tracked allocator isn't enough (untracked/system allocations).
- Integration tests assert on the captured leak report via `check_no_memory_leaks(stdout)` from `tests/framework`.

## 4. Output inspection

**HDF5 quick looks** (structure first, values second):

```bash
h5ls -r output/sage16-mini-millennium/model_000.hdf5 | head -30
python3 - <<'EOF'
import h5py
with h5py.File("output/sage16-mini-millennium/model_000.hdf5") as f:
    print([m.decode() for m in f["RunProperties/EnabledModules"]["module_name"]])
    print({r["name"].decode(): r["value"].decode() for r in f["RunProperties/Parameters"][:6]})
    print({r["field_name"].decode(): r["units"].decode() for r in list(f["RunProperties/FieldMetadata"])[:6]})
EOF
```

(`RunProperties` also carries `EventContracts`, `Redshifts`, and `Version` — the full reproducibility record; see `mimic-run-and-operate`.)

**Binary**: always through the run's own `metadata/output_schema.json` via `plot/mimic-plot/output_schema.py` (`load_schema`, `dtype_from_schema(binary=True)`, `units_from_schema`).

**Composed inspector (this skill's `scripts/`)** — one command for "is this run output sane?":

```bash
mimic_venv/bin/python3 .agents/skills/mimic-diagnostics-and-tooling/scripts/inspect_run_output.py \
    <run_output_dir> --ranges tests/generated/property_ranges.json
```

Prints per-field min/max/NaN/Inf plus range-violation counts (sentinel-aware), and — critically — refuses to read any partition file whose header-implied size disagrees with the schema, reporting `SCHEMA MISMATCH` instead of garbage. That guard exists because mixed-era directories are real: `tests/data/output/baseline/binary/` contains intentionally frozen old-schema `model_uniquegalid_*` fixtures alongside current-schema files (verified 2026-07-04: the inspector loads 9265 records from the current file and correctly rejects the two frozen ones). Exit 0 clean, 1 on any finding.

**Framework loaders as instruments** (importable with `cd tests` or `sys.path` on `tests/`): `load_binary_halos`, `load_hdf5_halos`, `load_hdf5_run_properties`, `assert_hdf5_schema_layout(expected_format_version="1.1")`, `validate_no_nans` / `validate_no_infs` / `find_nonfinite(halos, max_examples=5)` (rich per-field NaN/Inf report), `validate_range` / `assert_range` — all in `tests/framework/data_loader.py`.

## 5. Regression comparison tools

- `compare_halos_comprehensive(...)` (`tests/framework/comparison.py`): all-property, all-halo comparison with `rtol` / `atol` and a `warn_rtol` band that surfaces worst offenders as warnings without failing — the instrument behind the physics baseline test, reusable for any two same-schema outputs.
- `baseline_rtol()` (`tests/framework/harness.py`): strict 1e-6 unless `MIMIC_BASELINE_RTOL` overrides (validated). Tolerance policy: `mimic-validation-and-qa` §6; design rationale: `mimic-scientific-method` §3.
- Fast regression measurement without the full suite: run the two baseline tests directly (`python3 tests/scientific/test_scientific.py`; `python3 models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py`).
- `tests/generated/property_ranges.json` is the generated physical-range manifest (dict keyed by property name, entries carrying `range` and `sentinels`) — the metadata-driven core scientific test consumes it; so does the composed inspector above.

## 6. The pipeline fuzzer

`scripts/fuzz_pipeline.py` stress-tests the module system's validation and dispatch (not scientific correctness — it only checks exit codes and ERROR/FATAL lines):

```bash
python3 scripts/fuzz_pipeline.py --runs 200                     # random pipelines
python3 scripts/fuzz_pipeline.py --sampling valid-subset --hours 2
python3 scripts/fuzz_pipeline.py --seed 12345                   # replay one failure
```

Options: `--runs/--hours`, `--seed`, `--model`/`--simulation`, `--timeout` (120 s default), `--sampling random|valid-subset` (random stresses the validation boundary; valid-subset builds dependency-closed executable pipelines), `--strict` (count ordering-validation rejections as failures), `--log-dir`, `--progress-every`. Failure artifacts (config + stdout/stderr + result) land under `archive/fuzz-logs/failures/<seed>/` — note `archive/` is a gitignored machine-local dir; create it if absent. Reach for the fuzzer after changes to dispatch, phase parsing, or pipeline validation.

## 7. Benchmarking

`./scripts/benchmark_mimic.sh [--verbose] [--compress] [--param-file FILE]` — clean-builds, times a full run (`/usr/bin/time`, platform-aware), and writes timestamped JSON under `benchmarks/` (gitignored machine-local symlink; results include git SHA, system, runtime, peak memory). Env vars: `MIMIC_FLAGS`, `EXTRA_CFLAGS` (e.g. `-O3 -march=native`), `MAKE_FLAGS` (e.g. `USE-HDF5=no`), `MPI_RUN_COMMAND` (e.g. `mpirun -np 4`), `MIMIC_EXECUTABLE`. Discipline: same machine, before AND after, for any performance-sensitive change; a faster generated test input exists via `make generate-test-inputs` + `--param-file build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml`.

## 8. Git-history instruments

```bash
git log --oneline --grep="<topic>" -i             # has this been worked on?
git log -S "<identifier>" --oneline               # when did this symbol appear/vanish?
git show <hash>^:<path>                           # read a deleted file (archived plans)
git log --diff-filter=D --name-only --oneline | head -40   # what was removed when
git tag                                           # v0.1-beta, v0.5, v0.9-pre-release, v1.0
```

Search history BEFORE re-investigating anything; the settled conclusions live in `mimic-failure-archaeology`.

## Provenance and maintenance

Verified against the live repo 2026-07-04; the composed inspector was run against `tests/data/output/baseline/binary/` with the output shown above. Re-verify drift-prone specifics:

```bash
mimic_venv/bin/python3 scripts/validate_modules.py --help >/dev/null; echo rc=$?   # validator alive
grep -n "def compare_halos_comprehensive" tests/framework/comparison.py
grep -n "^def " tests/framework/data_loader.py                         # loader instrument set
mimic_venv/bin/python3 scripts/fuzz_pipeline.py --help | head -20       # fuzzer flags
sed -n '160,180p' scripts/benchmark_mimic.sh                           # benchmark env vars
mimic_venv/bin/python3 .agents/skills/mimic-diagnostics-and-tooling/scripts/inspect_run_output.py \
    tests/data/output/baseline/binary/ --ranges tests/generated/property_ranges.json  # self-test
```

If the inspector's self-test stops rejecting the `model_uniquegalid_*` fixtures, either the fixtures were regenerated to the current schema (fine — update section 4's note) or the schema guard broke (fix it).
