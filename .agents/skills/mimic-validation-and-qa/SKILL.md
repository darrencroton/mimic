---
name: mimic-validation-and-qa
description: "Mimic's test system and evidence standards - what counts as proof that a change works. Load when a task involves running or writing tests (make tests, tests-unit, tests-integration, tests-scientific, summary mode), MIMIC_RESULT markers, TEST_RUN/TEST_ASSERT/TEST_SKIP_WITH macros, tests/framework helpers, test registration (module_info.yaml tests: keys, generated test manifests), test placement decisions, the test_fixture module, baselines (tests/data/output/baseline, physics-binary baseline, regenerate_baseline.sh), tolerances (rtol, MIMIC_BASELINE_RTOL, CI 1e-3), a test that fails/skips/regresses, or \"how do I test this change\"."
---

# Mimic Validation and QA

Mimic has a three-tier test system with a deterministic structured-marker protocol, generated test manifests keyed to the compiled MODEL/SIMULATION pair, and two committed golden baselines. This skill is how to run it, read it, extend it, and not be fooled by it.

## When to use / when NOT to use

Use for: running suites, writing/registering tests, placement decisions, baseline questions, tolerance questions, interpreting FAIL/SKIP/WARN.

Do NOT use for:
- Diagnosing why a specific failure happened — see the `mimic-debugging-playbook` skill.
- The measurement toolbox (inspectors, comparators as standalone tools, fuzzer, benchmarks) — see the `mimic-diagnostics-and-tooling` skill.
- Designing the scientific evidence for a physics claim (what to measure, what tolerance is justified) — see the `mimic-scientific-method` skill.
- Commit gating order — see the `mimic-change-control` skill.

## First actions

1. Note the compiled pair: tests are generated for one `MODEL`/`SIMULATION` (defaults `sage16`/`mini-millennium`). Use the same pair on every command.
2. Activate the venv before Python-backed tiers: `source mimic_venv/bin/activate`. Unit tests are C, but integration, scientific, `make tests`, formatting checks, and plotting tests need the pinned Python packages from `mimic_venv`.
3. For any suite run, capture and check the exit code — never judge from scrolling output:

```bash
mkdir -p archive/test-logs
make tests summary > archive/test-logs/tests.log 2>&1; rc=$?
rg -n "^MIMIC_RESULT: (FAIL|SKIP|WARN|ERROR)" archive/test-logs/tests.log
echo "exit_code=$rc"   # non-zero = failure regardless of log text
```

4. Read every SKIP line and its reason. A skip is not a pass — a guard checking a stale model name once silently skipped the physics baseline test for weeks.
5. Failing tests are real problems. Never weaken or simplify one to pass (`mimic-change-control`, non-negotiable C).

## 1. The three tiers

| Tier | Command | Language/scope | Runtime |
|---|---|---|---|
| Unit | `make tests-unit` | C; direct function calls, no full pipeline | up to ~3 min |
| Integration | `make tests-integration` | Python; real `./mimic` runs via run files; keep each test under ~30 s | up to ~3 min |
| Scientific | `make tests-scientific` | Python; physics contracts vs reference data and metadata-declared ranges | ~30 s |
| Converter | `make tests-converter` | Python stdlib-unittest suite for `scripts/convert/`; package-independent, needs `mimic_venv` | ~10 s |
| Fixture conformance | `make check-snapshot-fixture` | Structural check of the committed snapshot-HDF5 fixture against the frozen format spec | seconds |

`make tests` composes: clean → generate test registry → one build → `check-docs` → `validate-modules` → `check-snapshot-fixture` → `tests-converter` → all three tiers, accumulating failures in `build/.test_failures` and printing a final verdict block. Append the `summary` goal to any test target to filter output to `MIMIC_RESULT:` FAIL/SKIP/WARN/ERROR lines (PASS suppressed; infra steps silenced on success, dumped in full on failure; a crashing test that emits no markers dumps its full log). Unit and integration are long with large output — when orchestrating from a main agent context, delegate the run and act on the summarized report (AGENTS.md testing strategy).

**Two traps worth knowing before you trust a result.** `summary` suppresses PASS, so a summary log can only evidence that nothing failed — it can never evidence that a particular test *passed*. To prove a specific test passes, run its file directly (`python3 tests/integration/<file>.py`) and read its markers. Separately, run `make check-generated` **without** `TEST_BUILD=yes`: `check_generated.py` and `generate_properties.py` hash different input sets under a test build, so a test-build run reports drift that is not there.

## 2. The marker protocol

Every test case emits exactly one line:

```text
MIMIC_RESULT: PASS|FAIL|SKIP|WARN|ERROR <test_name> [-- <reason>]
```

**C** (`tests/framework/test_framework.h`): `TEST_RUN(fn)` runs a function returning `TEST_PASS` (0) / `TEST_FAIL` (1) / `TEST_SKIP` (2) and emits the marker; `TEST_ASSERT`, `TEST_ASSERT_EQUAL`, `TEST_ASSERT_DOUBLE_EQUAL(a,b,tol,msg)`, `TEST_ASSERT_STRING_EQUAL` emit FAIL markers automatically; a failing test with no assertion marker still gets a generic FAIL so nothing is invisible in summary mode. `return TEST_SKIP_WITH("reason")` for configurations where the test genuinely cannot run. Finish with `TEST_SUMMARY()`/`TEST_RESULT()`.

**Python** (`tests/framework`): `result_pass/result_fail/result_skip/result_warn/result_error` from `markers.py`; raise `TestSkipped("reason")` to skip. The shared loop `run_test_suite` (`tests/framework/runner.py`) catches `TestSkipped` → `result_skip`, and a test function that *returns a non-empty string* is surfaced as a WARN with that string as the reason (verified at `runner.py:72-73`) — use that for soft findings that shouldn't fail the suite. **`run_test_suite` also takes an opt-in `abort_on_failure=False`**: leave it off for ordinary suites, where independent tests are more useful run to completion, and turn it on only for a suite whose entries are ordered *stages* that each depend on the previous (the cross-format identity gate is the one such caller). Aborted entries emit SKIP naming the stage that stopped the suite, so every test still produces exactly one marker.

## 3. Where a test lives (placement decision table)

| What it validates | Location | Registered by |
|---|---|---|
| Core framework behavior (dispatch, substeps, output formats, memory) | `tests/{unit,integration,scientific}/` | Globbed automatically (`test_*.c` / `test_*.py`) |
| One module's physics | `models/<model>/modules/<module>/_tests/` | `tests:` keys in that module's `module_info.yaml` |
| Cross-module model behavior (pipeline contracts, parity) | `models/<model>/modules/_tests/` | That directory's `module_info.yaml` |
| Simulation/reader behavior | `simulations/<sim>/_tests/{unit,integration,scientific}/` | Directory placement |

**The model-neutral rule**: core tests must never name production physics modules — they use the `test_fixture` module (`src/module_system/test_fixture/`, compiled only in `TEST_BUILD=yes` builds via `MIMIC_TEST_BUILD=1`). This upholds the physics-agnostic-core principle: archiving or changing a sage16 module can never break framework tests.

Registration flows through `scripts/generate_test_registry.py` into `build/generated/{unit,integration,scientific}_tests.txt` (refresh: `make generate-test-registry`; `--strict` fails on declared-but-missing test files). Shared test run files are materialized under `build/generated/test_inputs/<MODEL>/<SIMULATION>/` (`make generate-test-inputs`), with snapshot indices derived from the simulation's a_list — never hardcoded.

**A core test runs against EVERY package, so it must assert nothing package-specific.** `generate_test_registry.py` globs `tests/{unit,integration,scientific}/` into the core tier for whichever pair is selected, so a hard-coded box size, particle mass, snapshot count or halo count passes vacuously under the package it was written against and fails spuriously under another — and a `if SIMULATION == "…"` conditional is the same defect wearing a disguise. Derive the value from the selected package's own configuration instead (`resolve_sim_config_path`; precedent `tests/integration/test_unique_galaxy_id_encoding.py`), or assert a property true of every package. This applies to *physical trends* as much as to literals: an expectation that holds across the 64-snapshot mini-Millennium fixture can be vacuous on a two-snapshot smoke fixture. `test_dynamic_timestep_computes_substeps` is the worked example — it asserts per-group dispatch and a configured bound rather than a redshift trend, precisely because the trend was not a contract of the algorithm.

## 4. Selector gating — what actually runs

Model physics tests run only when the selected simulation is in `FULL_MODEL_TEST_SIMULATIONS` (`scripts/discovery.py`): `mini-millennium`, `micro-uchuu`, `micro-uchuu-hdf5`, `micro-uchuu-ascii`. The three micro-Uchuu packages deliberately use their production `simulation_info.yaml` so one small catalog exercises the L-Halo binary, ctrees-HDF5, and ctrees-ASCII reader paths. Larger packages (`millennium`, `mini-uchuu`, `uchuu`) run core + simulation-owned tests on fixtures and skip model physics. Additional per-test guards: `compiled_model()` checks (e.g. the physics baseline self-skips unless the executable was compiled for sage16), baseline tests skip for non-default pairs, HDF5-dependent unit tests skip without HDF5 dev libs. All of these appear as SKIP markers with reasons — audit them after every suite run.

## 5. The golden inventory (baselines)

Two committed baselines; both are recorded run outputs — hand-editing one is fabricating evidence.

1. **Physics-free core baseline** — `tests/data/output/baseline/{binary,hdf5}/`. Protects deterministic halo tracking, independent of any model. Regenerate the HDF5 side with `./scripts/regenerate_baseline.sh`, which asserts an HDF5 build, asserts the generated test run file is physics-free (empty module lists), backs up the old baseline under `archive/baseline-backups/`, reruns, and re-validates. It installs the baseline as one coherent set — shard, master, and the whole `metadata/` directory — because a baseline whose parts came from different runs is a defect; it refuses any MODEL/SIMULATION other than the pair owning the committed baseline (the comparison test skips for others, so the result would never be checked); and it exits non-zero rather than reporting success when validation cannot run. Its own header: regenerate only after deliberate, validated changes to core halo tracking — "Never regenerate it merely to silence a failing test."
2. **Full-physics sage16 baseline** — `models/sage16/modules/_tests/baseline/physics-binary/` (`model_z0.000_0` + `metadata/output_schema.json`). Protects the whole baryonic pipeline at rtol 1e-6 across ALL properties and halos. Refresh procedure is in the docstring of `test_scientific_sage_physics_baseline.py`: after a DELIBERATE, validated science change, rerun its input (`models/sage16/modules/_tests/input/test_physics_binary.yaml`) and `cp` the fresh output file and `output_schema.json` into the baseline directory — with the justification in the commit (see `mimic-change-control` non-negotiable F).

Baseline regeneration is a scientific act: it requires the numbers that justify it (`mimic-scientific-method`) and travels in the same commit as the change that made it necessary.

## 6. Tolerances

- Framework defaults (`tests/framework/harness.py`): `BASELINE_RTOL_DEFAULT = 1e-6`, `BASELINE_ATOL_DEFAULT = 1e-10`; `baseline_rtol()` honors a validated `MIMIC_BASELINE_RTOL` env override.
- The comparator `compare_halos_comprehensive` (`tests/framework/comparison.py`) supports a `warn_rtol` band: diffs passing a relaxed gate but exceeding the strict one surface as WARN (worst offenders listed) without failing — used so CI relaxation doesn't hide drift.
- CI runs scientific tests with `MIMIC_BASELINE_RTOL=1e-3` because the sage16 baseline was generated on macOS and Linux libm reproduces it only to ~7e-4 relative; locally the strict 1e-6 applies. A local failure at 1e-6 that CI would pass is still a finding — investigate before relaxing anything.
- Choosing new tolerances (scientific template guidance): exact conservation laws ~1e-10; approximate physical relations ~1e-2; literature comparisons ~0.5 dex-scale. Justify every tolerance in the test's docstring; see `mimic-scientific-method` for tolerance design.

## 7. Adding a test (per tier)

Always start from a template — `tests/framework/c_unit_test_template.c`, `python_integration_test_template.py`, `python_scientific_test_template.py`; each header documents where to copy it and what its tier should and should not validate.

**C unit**: copy template to `models/<model>/modules/<mod>/_tests/test_unit_<mod>.c` (or `tests/unit/` for core). New sage16 tests include `models/sage16/modules/_tests/sage_test_fixtures.h` instead of re-declaring boilerplate. Every allocating path ends with `check_memory_leaks()`. Register in `module_info.yaml` `tests.unit`. Run: `make generate-test-registry`, then `tests/unit/run_tests.sh test_unit_<mod>` — **never execute the built `.test` binary directly**; the runner refreshes generated registries and rebuilds. Add `MODEL=<m> SIMULATION=<s>` env for non-default pairs.

**Footgun — a new `src/` source breaks every C unit test until the runner knows about it.** `tests/unit/run_tests.sh` builds against explicit `UTIL_SRCS` / `CORE_SRCS` / `IO_SRCS` lists, not the Makefile's `find`. Add a new `src/util/*.c` or `src/core/*.c` to the matching list or every unit test fails at link with `Undefined symbols`, which reads like a broken test rather than a missing source entry.

**Python integration**: copy template to the module's `_tests/`; it locates the repo root itself and imports `create_test_param_file`, `run_mimic`, `load_binary_halos`, `check_no_memory_leaks` (asserts against the run's captured leak report), `run_test_suite` from `tests/framework`. Register in `tests.integration`. Run directly: `python3 path/to/test_integration_<mod>.py` from the repo root.

**Python scientific**: copy template; state the physical contract and tolerance in the docstring; use `run_mimic_fresh` + generated inputs; reference data lives beside the test or in `tests/data/`. Register in `tests.scientific`.

Then: run the individual test → run its tier with `summary` → confirm your markers appear (including the SKIP path if you wrote one).

## 8. The cross-format identity gate (package-local, manual)

`simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py` is a third kind of scientific-tier evidence, distinct from the two baselines in section 5: it proves the snapshot-ordered driver agrees with the tree-ordered driver, not that either matches a fixed golden output. It is registered by the scientific-tier registry only when `SIMULATION=micro-uchuu-snapshot` is selected (package-local placement, section 3), so it is invisible to the default pair and to CI.

- **What it checks**: for every output snapshot, the same `UniqueGalaxyID` set and per-ID bitwise-identical fields between `micro-uchuu-ascii` (tree-ordered) and `micro-uchuu-snapshot` (snapshot-ordered), aggregated across every output partition, under both `halos-only` and `sage16`, under both `TimestepScheme: fixed` and `dynamic` — no tolerance of any kind.
- **Dataset preconditions**: both the full `micro-uchuu-ascii` and `micro-uchuu-snapshot` datasets must resolve through their machine-local gitignored `snapshots` symlinks. Unlike an ordinary test, a missing dataset **fails** the gate (naming the path it looked for) rather than skipping — a gate whose only defense is the dataset it verifies must never silently report success on an empty comparison.
- **Cost**: it builds four executables in isolated git worktrees (`{halos-only, sage16} × {ascii, snapshot}`) and performs nine full runs — on the order of hours. Run it deliberately, not as part of routine iteration.
- **How to run it**: `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific`, on a machine holding both datasets.
- **Comparator**: `scripts/compare_cross_format_identity.py` — duplicate-ID assertion, then ID-set equality, then per-field raw-byte comparison; the one implementation of the frozen algorithm, consumed by nothing else.
- **Comparator self-test**: `tests/scientific/test_compare_cross_format_identity.py` — synthesises its own tiny HDF5 runs and asserts the comparator's exit status for each way two runs can disagree (perturbed value, dropped id, duplicated id on either side, signed zero, matching and differing NaN payloads, schema and snapshot-set mismatches, unreadable input). It is in the **core** tier, not beside the gate, deliberately: the gate needs multi-gigabyte datasets and hours so it never runs in CI, while this is package-neutral and runs in seconds on the default pair. **A gate is only as trustworthy as its comparator's ability to fail**, and the gate itself cannot demonstrate that. When changing the comparator, mutation-test it: break it deliberately and confirm this file catches it — coverage of the comparison *predicate* is not coverage of which fields, snapshots and runs are actually visited.
- **Stage 8 carries two hand-maintained allow-lists, and a change that moves emitted metadata must extend them in the same commit.** Tree-path preservation compares HEAD against the pre-Phase-5 baseline and permits only specific, individually pinned metadata deltas: `PERMITTED_DELTAS` plus `classify()` for HDF5 attributes and `FieldMetadata` columns, and `assert_output_schema_delta`'s expected set for the run-local `metadata/output_schema.json`. Both lists must move together — a delta appearing in one and not the other means the HDF5 writer and the schema writer have diverged. **This is a real failure mode, not a hypothetical**: D8's `Spin` relabel (2026-08-14) moved `Spin.units` and `Spin.description`, neither list was extended, and the gate sat red at stage 8 for twelve days and four commits because nobody re-ran it. Register the delta *and* pin its exact before/after transition; registering the name alone would silently accept any future relabel.
- **A failing stage's detail is truncated in the log.** `run_test_suite` reports assertion failures through `_first_line(exc)` (`tests/framework/runner.py:82`), so a multi-line reason — stage 8 lists up to 20 offending deltas — shows only its first line, and the gate removes its worktrees and scratch on every exit path, leaving nothing to inspect. To recover the detail, re-run the gate from a driver that monkeypatches `_first_line` to `str` in-process; do not edit the test to find out why the test failed.

Full driver mechanics and the parity checklist the gate is checking: `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and "The cross-format identity gate".

## Provenance and maintenance

Verified against the live repo 2026-07-04; the cross-format identity gate (section 8) added 2026-08-12 once the snapshot driver landed. Re-verify drift-prone specifics:

```bash
sed -n '41,56p' scripts/discovery.py                       # FULL_MODEL_TEST_SIMULATIONS membership
grep -n "BASELINE_RTOL_DEFAULT\|BASELINE_ATOL_DEFAULT" tests/framework/harness.py
grep -n "MIMIC_BASELINE_RTOL" .github/workflows/ci.yml     # CI tolerance still 1e-3
sed -n '65,95p' tests/framework/runner.py                  # string-return -> WARN; abort_on_failure
grep -n -i "never regenerate" scripts/regenerate_baseline.sh
sed -n '15,30p' models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py  # refresh recipe
ls tests/framework/*template* models/sage16/modules/_tests/sage_test_fixtures.h
grep -n "strict" scripts/generate_test_registry.py | head -3
ls simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py scripts/compare_cross_format_identity.py tests/scientific/test_compare_cross_format_identity.py
```

Tier runtimes and the gating set are the most drift-prone facts; the marker protocol and placement rules are framework architecture and durable.
