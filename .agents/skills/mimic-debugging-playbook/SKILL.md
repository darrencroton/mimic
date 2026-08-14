---
name: mimic-debugging-playbook
description: Symptom-to-triage playbook for every Mimic failure mode. Load when something is broken, failing, crashing, or behaving unexpectedly - build errors (Unknown MODEL, libyaml/HDF5/mpicc not found), stale generated code, module startup FATALs, YAML config rejections, model/simulation mismatch, tree reader errors (cannot open input files, unknown tree_type), output/schema mismatches, plotting import errors or skipped plots, scientific baseline regressions, memory leak reports, or "why did my run/test/plot fail". Also load before starting any debugging session to follow the first-response protocol.
---

# Mimic Debugging Playbook

Symptom → likely cause → first command, for every failure mode Mimic is known to produce. Follow the first-response protocol before hypothesizing; most "mysterious" failures are one of the known causes below.

## When to use / when NOT to use

Use this skill when something is failing and you need to triage it: build errors, startup FATALs, config rejections, reader errors, test regressions, plot failures, memory reports.

Do NOT use for:
- History questions ("why is this quirk here?", "was this fought before?") → see the `mimic-failure-archaeology` skill.
- Writing or registering tests, tolerances, baselines → see the `mimic-validation-and-qa` skill.
- Measurement tooling (validators, fuzzer, benchmarks, HDF5/binary inspection recipes) → see the `mimic-diagnostics-and-tooling` skill.
- Environment setup from scratch (prerequisites, venv, first_run.sh) → see the `mimic-build-and-env` skill.
- Config axis reference (every YAML key, Make var, CLI flag) → see the `mimic-config-and-flags` skill.

## First actions (first-response protocol)

Run these IN ORDER before forming any hypothesis. They eliminate the four most common non-bugs (metadata drift, generated-code drift, environment drift, wrong selector) in under two minutes. Use the same `MODEL=<name> SIMULATION=<name>` pair everywhere (defaults: `sage16` + `mini-millennium`, so plain `make ...` is valid for the defaults).

```bash
cd <repo-root>
make validate-modules || exit $?   # 1. metadata sane? (exit 1 schema, 2 file, 3 dep, 4 naming)
make check-generated || exit $?    # 2. generated code in sync with YAML metadata?
make info || exit $?               # 3. what does the build actually see? (libs, compiler, features)
./mimic --debug <run.yaml> > debug.log 2>&1       # 4. captured max-verbosity rerun
rc=$?
tail -n 80 debug.log
echo "exit_code=$rc"              # 5. ALWAYS check the exit code, never just eyeball the log
```

Only after step 5: hypothesize. If check-generated fails, fix that first (`make generate && make clean && make`) — chasing a "bug" that is stale generated code wastes hours.

## Master symptom table

### Build failures

| Symptom | Likely cause | First command |
|---|---|---|
| `Unknown MODEL '<x>'` or `Unknown SIMULATION '<x>'` from make | Package renamed/removed, or typo in selector | `ls models/ simulations/` |
| Make errors about lowercase `model=` / `simulation=` | Typo guard: selectors are uppercase `MODEL=` / `SIMULATION=` | rerun with `MODEL=... SIMULATION=...` |
| libyaml not found at link/compile | libyaml not installed or not detected | `make info` (shows detection); see `mimic-build-and-env` |
| HDF5 headers/libs not found | HDF5 missing (default is `USE-HDF5=yes`) | `make info`; either install HDF5 or `make USE-HDF5=no` |
| `mpicc` not found | `USE-MPI=yes` without an MPI toolchain | drop `USE-MPI=yes` or install MPI; `make info` |

Note: `clean tidy help check-docs check-format test-clean summary` are model-free targets and never trigger the Unknown-MODEL guard; everything else does.

### Stale generated code

| Symptom | Likely cause | First command |
|---|---|---|
| `make check-generated` fails; or behavior ignores your YAML edit | Generated code out of sync with property/module metadata | `make MODEL=<m> SIMULATION=<s> generate && make clean && make MODEL=<m> SIMULATION=<s>` |
| Weird build errors after switching MODEL/SIMULATION | Generated files from the other package still on disk | same fix as above, with the pair you intend to run |

Never hand-edit anything under `*/generated/` — edit the YAML/metadata source and regenerate. See the `mimic-properties` skill for which YAML feeds which generated file.

### Module startup failures (pipeline validation at `module_system_init`)

All of these fail fast at startup, before any tree is processed — that is by design (no parameter defaults, no silent fallbacks).

| Symptom | Likely cause | First command |
|---|---|---|
| FATAL: unknown module named in pipeline, message lists all available modules | Typo in `modules.phases`/`pre_timestep`/`post_timestep` in the run YAML, or module not registered for this MODEL | compare run YAML against the FATAL's available-modules list |
| ERROR: unsupported processing mode | Run YAML asks for a mode not in the module's `supported_processing_modes` | check `module_info.yaml` for that module |
| ERROR: per-event module has no subscriptions | `process_per_event` module consumes no event any producer emits | check `events.consumes` vs generated `src/module_system/generated/event_contracts.h` |
| ERROR: event producer constraint | Producer must be full-halo AND in the SAME phase as its consumers | check phase placement in run YAML |
| Module init fails: missing parameter | Parameters have NO defaults; every `model_get_*`/`LOAD_PARAM_*` param must be in `modules.parameters` | add the parameter to the run YAML |

### YAML config failures

`src/core/read_parameter_file.c` validates fixed-schema sections and rejects unknown keys there with a fatal `Unknown key '<section>.<key>'` (the `modules` section variant adds `; supported keys are pre_timestep, ...`). Top-level timestep keys have no whitelist, so typos like `substeps:` can be silently ignored; route top-level key questions through the `mimic-config-and-flags` no-whitelist trap.

| Symptom | Likely cause | First command |
|---|---|---|
| `Unknown key '<section>.<key>'` | Typo, or key belongs to a different section | check the key against a shipped run file, e.g. `models/sage16/input/sage16_mini-millennium.yaml` |
| Startup rejection: run file model/simulation vs compiled binary | Binary compiled with one `MODEL`/`SIMULATION`, run YAML declares another (`model.name`, `simulation.name`) | rebuild with the matching pair, or fix the run file |

### Tree reader failures

| Symptom | Likely cause | First command |
|---|---|---|
| Cannot open input tree files | Wrong `input.simulation_dir` / `tree_name` / `first_file`/`last_file`, or `snapshots/` symlink missing on this machine | `ls <simulation_dir>` with the exact path from the debug log |
| Unknown/unregistered `tree_type` | Typo, or format not in the reader registry | check `src/io/tree/registry.c` for registered names |
| FATAL: `The snapshot-ordered driver is not implemented yet` | `input.processing_order: snapshot_ordered` parses but fails fast (v1.0 has only `tree_ordered`) | use `tree_ordered`; see `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` for driver status |
| HDF5 reader requested in a `USE-HDF5=no` build | `lhalo_hdf5` / `consistent_trees_hdf5` need HDF5 compiled in | rebuild without `USE-HDF5=no` |
| Relative paths in run file resolve "wrong" | Run-file relative paths resolve from the invocation CWD, not the run-file's directory | rerun from repo root or use absolute paths |

### Output / schema issues

| Symptom | Likely cause | First command |
|---|---|---|
| Binary output "corrupt" / fields misaligned when read | Reading with the wrong schema — read ONLY via that run's own `metadata/output_schema.json`, never the current checkout's metadata | inspect `<output_dir>/metadata/output_schema.json` |
| `--skip` run dies: `Partial output exists for partition <N> (<n> of <m> files)...` | `--skip` skips only when ALL files of a partition exist; partial → FATAL by design (`src/core/tree_driver.c`) | remove the partial partition's output files (archive, don't delete, per repo rules), rerun |

See the `mimic-run-and-operate` skill for output layout and reading recipes.

### Plotting failures

| Symptom | Likely cause | First command |
|---|---|---|
| ImportError / ModuleNotFoundError running mimic-plot | Virtualenv not active | `source mimic_venv/bin/activate` |
| Plot silently missing from output | Two distinct skips: missing-property skip (printed pre-call with the missing fields) vs in-plot validation skip (reason shown only with `--verbose`) | rerun with `--verbose`; read every skip reason |
| Profile axis override has no effect | Trap (e): profile uses `xlim`/`ylim` keys but `get_profile_axes` reads only `xmin`/`xmax`/`ymin`/`ymax` scalars — `xlim`/`ylim` are silently ignored | verify keys in `plot/mimic-plot/output_utils.py` `get_profile_axes` |

See the `mimic-plots-and-analysis` skill for the figure contract and profile stack.

### Scientific regressions

| Symptom | Likely cause | First command |
|---|---|---|
| Scientific baseline test fails locally | Real physics change, OR stale baseline, OR precision change with un-chased local copies (trap a) | rerun with the exact failing tolerance printed; then `git log` on touched physics files |
| Passes locally (macOS), fails in CI (Linux) or vice versa | Tolerance story: local default rtol 1e-6 / atol 1e-10; CI runs `MIMIC_BASELINE_RTOL=1e-3` because the baseline was generated on macOS and Linux libm differs by up to ~7e-4 | compare against both tolerances before declaring a regression |
| Need a looser/tighter comparison to bisect | `MIMIC_BASELINE_RTOL` env override on the scientific tier | `MIMIC_BASELINE_RTOL=1e-3 make tests-scientific summary` |

Numbers before claims: never declare "regression" or "fixed" from eyeballing plots — quote the measured per-property deltas and tolerance. See the `mimic-scientific-method` skill.

### Memory

| Symptom | Likely cause | First command |
|---|---|---|
| Nonzero blocks in the exit leak report | Real leak in the named category (`MEM_GALAXIES`/`MEM_HALOS`/`MEM_TREES`/`MEM_IO`/`MEM_UTILITY`) | read `check_memory_leaks()` per-category report in the run log |
| Memory grows during a run | Usually expected growth, not a leak — see next section | compare two runs of different sizes (below) |
| Need allocation-site detail | Tracked allocator narrows category; valgrind narrows call site | `valgrind --leak-check=full ./mimic <run.yaml>` |

## Expected growth vs real leak

Do not "fix" memory growth without discriminating first — some growth is designed in:

- **ProcessedHalos grows BY DESIGN.** Orphan galaxies emit one record per surviving snapshot, so ProcessedHalos scales with orphan count × snapshot depth. Bigger/deeper trees → more memory. Not a leak.
- **The galaxy pool bulk-resets per tree.** Per-tree galaxy allocations are reclaimed wholesale between trees; within-tree growth followed by reset is normal.
- **A real leak = nonzero tracked blocks in the exit report** from `check_memory_leaks()`, broken down by category.

Discriminator experiment: run a small input (e.g. one file of mini-millennium) and a larger one; if peak memory scales with tree count/depth but the exit leak report is clean both times, it is expected growth. If the exit report shows unfreed blocks — even on the small run — it is a leak; the category tells you which subsystem, then `valgrind --leak-check=full` on the SMALL run gives the call site.

## Verbosity ladder and capture discipline

| Flag | Meaning |
|---|---|
| (default) | Normal progress output |
| `-v` / `--verbose` | Adds context (timestamp, file:line) and VERBOSE_LOG messages |
| `-d` / `--debug` | Most verbose: DEBUG_LOG output plus everything above |
| `-q` / `--quiet` | Warnings and errors only |

- `DEBUG_LOG` is rate-limited to 5 messages per call site — a debug message going quiet mid-run means the limit tripped, not that the code path stopped executing.
- Always capture the program's own exit status, not the status of `tee`: `./mimic --debug <run.yaml> > debug.log 2>&1; rc=$?; tail -n 80 debug.log; echo "exit_code=$rc"`. A log without an exit code is not evidence.
- Logging macros live in `src/util/error.h` (`DEBUG_LOG`, `VERBOSE_LOG`, `INFO_LOG`, `WARNING_LOG`, `ERROR_LOG`, `FATAL_ERROR`).

## Time-costing traps (each cost real time once — do not repeat)

- **(a) Precision widening leaves stale local float copies.** Widening a struct field to double is incomplete until every local `float` copy in every consumer is chased — `sage_reincorporation.c` locals silently re-narrowed a widened field and only the FULL test suite caught it (commit 6cbeafe4). After any precision change: run the full suite, not just the touched tier.
- **(b) Tests can skip silently after renames.** The physics baseline test skipped for a while because its model-name guard checked `"sage"` after the sage→sage16 rename. ALWAYS read the SKIP reasons in `make tests ... summary` output — a green suite with silent skips proves nothing.
- **(c) Unit tests run ONLY via the runner.** Use `tests/unit/run_tests.sh <test_name>`, with `MODEL=<m> SIMULATION=<s>` env vars for non-default pairs. Never execute the `.test` binary directly — it misses the runner's environment setup.
- **(d) CI tolerance is 1e-3, local is 1e-6.** A value drift between 1e-6 and 1e-3 fails locally but passes CI (or the reverse across platforms). Know which tolerance produced the verdict you are reading before acting on it.
- **(e) Plot profile `xlim`/`ylim` keys are silently ignored.** `get_profile_axes` (`plot/mimic-plot/output_utils.py`) reads `xmin`/`xmax`/`ymin`/`ymax` scalars; shipped profiles still contain list-style `xlim`/`ylim` keys that do nothing. If an axis override "doesn't work", this is why.
- **(f) Stale `__pycache__` / orphaned `.pyc` can mislead.** A `.pyc` without its source (e.g. after a figure module rename) can keep old behavior alive or mask an ImportError. `find . -name __pycache__ -exec rm -rf {} +` before trusting weird Python behavior.
- **(g) Some "stale" generated files are not drift.** `tests/generated/module_sources.mk` exists on disk but NO current generator writes it (legacy leftover; the Makefile does not name it). `make check-generated` passing while it looks old is NOT a bug. `init_halo_properties.inc` and `init_galaxy_properties.inc` were the same class and were removed from `GENERATED_HEADERS` and archived on 2026-08-13; `reset_galaxy_properties.inc` was archived on 2026-08-14. Since `src/include/generated/` is gitignored, an older worktree may still show any of these.
- **(h) Run-file relative paths resolve from the invocation CWD**, not from the run file's location. Same run file, different directory → different behavior.
- **(i) The ctrees_ascii final-snapshot topology difference is accepted, not a bug.** The ASCII reader's `fix_flybys` collapses multiple z=0 FoF groups (negated MostBoundID); the lhalo/HDF5 readers do not. Pre-final snapshots are byte-identical across the micro-uchuu packages. Do not "fix" it; see the `mimic-failure-archaeology` skill.

## Discriminating experiments

When the cause is ambiguous, run the experiment that splits the hypothesis space in half:

| Question | Experiment |
|---|---|
| Model physics bug vs core framework bug? | Run the `halos-only` model (empty pipeline, no galaxy physics): `make MODEL=halos-only SIMULATION=mini-millennium && ./mimic models/halos-only/input/halos-only_mini-millennium.yaml`. Fails → core/reader; passes → model physics |
| Reader bug vs driver/core bug? | Run the micro-uchuu triplet (`micro-uchuu` lhalo-binary, `micro-uchuu-hdf5`, `micro-uchuu-ascii`) on the same data; only one reader wrong → reader; all wrong → driver/core. Remember trap (i) for the final snapshot |
| My bug vs stale generation? | `make MODEL=<m> SIMULATION=<s> check-generated`; if it fails, regenerate/clean/rebuild and re-test before debugging anything else |
| Physics bug vs config mistake? | `make validate-modules` and `make lint-parameters` FIRST; a mis-declared parameter or metadata error masquerades as wrong physics |
| Leak vs designed growth? | Two runs of different sizes + exit leak report, as in the memory section above |

## Provenance and maintenance

Facts verified against the live repo on 2026-07-04. Re-verify before trusting anything volatile:

```bash
grep -n "Unknown key" src/core/read_parameter_file.c        # unknown-key rejection messages
grep -rn "Unknown MODEL" Makefile                            # build selector guard
grep -n "def get_profile_axes" plot/mimic-plot/output_utils.py  # axis key trap (e)
grep -n "DEBUG_LOG_MAX_CALLS" src/util/error.h               # DEBUG_LOG rate limit (5/site)
grep -rn "check_memory_leaks" src/util/                      # leak report entry point
grep -rn "not implemented" src/core/ | grep -i snapshot      # snapshot_ordered fail-fast
git log --oneline 6cbeafe4 -1                                # trap (a) story
ls models/halos-only/                                        # empty-pipeline model exists
```

The tolerance values (local rtol 1e-6 / atol 1e-10, CI `MIMIC_BASELINE_RTOL=1e-3`) live in `tests/framework/harness.py` and `.github/workflows/ci.yml` — check there if a tolerance dispute arises.
