---
name: mimic-debugging-playbook
description: Symptom-to-triage playbook for every Mimic failure mode. Load when something is broken, failing, crashing, or behaving unexpectedly - build errors (Unknown MODEL, libyaml/HDF5/mpicc not found), stale generated code, module startup FATALs, YAML config rejections, model/simulation mismatch, vertical reader errors (cannot open input files, unknown tree_type), output/schema mismatches, plotting import errors or skipped plots, scientific baseline regressions, memory leak reports, or "why did my run/test/plot fail". Also load before starting any debugging session to follow the first-response protocol.
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

### Module startup failures (run-file parse and pipeline validation)

All of these except the mid-run rows (the callback-failure row and the `module_create_record`, event-index and marshal rows, which fire when a module first creates or emits) fail fast at startup, before any tree is processed — that is by design (no parameter defaults, no silent fallbacks).

| Symptom | Likely cause | First command |
|---|---|---|
| FATAL: unknown module named in pipeline, message lists all available modules | Typo in `modules.phases`/`pre_timestep`/`post_timestep` in the run YAML, or module not registered for this MODEL | compare run YAML against the FATAL's available-modules list |
| ERROR: unsupported processing mode | Run YAML asks for a mode not in the module's `supported_processing_modes` | check `module_info.yaml` for that module |
| ERROR: per-event module has no subscriptions | `process_per_event` module consumes no event any producer emits | check `events.consumes` vs generated `src/module_system/generated/event_contracts.h` |
| ERROR: event producer constraint | Producer must be full-halo AND in the SAME phase as its consumers | check phase placement in run YAML |
| Module init fails: missing parameter | Parameters have NO defaults; every `model_get_*`/`LOAD_PARAM_*` param must be in `modules.parameters` | add the parameter to the run YAML |
| `Module '<m>' is configured with processing mode 'process_snapshot', which is not a FoF mode ...` | `process_snapshot` placed in `pre_timestep`, `post_timestep` or a `phases:` entry; it belongs only in `modules.post_snapshot` | move the entry under `modules.post_snapshot` |
| `Module '<m>' is configured with processing mode '<x>'; only process_snapshot is allowed in this phase` | A FoF mode (`process_full_halo`/`process_per_event`/`process_by_galaxy`) listed under `modules.post_snapshot` | change the mode to `process_snapshot` (the module must declare it in `supported_processing_modes`) or move the module to a FoF phase |
| `modules.post_snapshot lists N module(s) (first: '<m>'), but it runs only under the horizontal driver ...` | Non-empty `post_snapshot` with a vertical `tree_type`; the vertical driver never holds a complete snapshot | use a horizontal package/run file, or remove the phase (absent, `null` and `[]` are accepted) |
| `modules.post_snapshot lists N module(s) (first: '<m>') with input.forest_chunks N, but a chunked sweep never holds a whole snapshot's population at once, so the snapshot scope needs forest_chunks: 1 ...` | Snapshot scope under chunking: a chunked sweep never holds a whole snapshot, so a non-empty `post_snapshot` cannot run with `forest_chunks` above 1 | set `forest_chunks: 1`, or distribute over MPI tasks to reduce memory |
| `input.forest_chunks is N, but it splits only the horizontal driver's slab sweep and reader '<r>' feeds the vertical driver; omit the key for one chunk` | `forest_chunks` above 1 with a vertical reader (`tree_type` other than `horizontal_hdf5`); the key splits only the horizontal driver's slab sweep | omit `input.forest_chunks` (one chunk), or use a horizontal package/run file |
| `A distributed or chunked horizontal run (NTask = N, input.forest_chunks = G) needs a forest-blocked format_version 3 dataset ... this is a format_version 2 dataset ...` or `Snapshot S is not forest-blocked: ForestIndex falls from ... A distributed or chunked horizontal run (NTask = N, input.forest_chunks = G) needs every slab's rows grouped by forest ...` | Startup refusal after `open_run`: more than one rank or `forest_chunks` above 1 on a version 2 dataset, or on a version 3 dataset whose rows are not grouped by forest (a Consistent-Trees ASCII dataset converted before the 2026-10-07 ruling) | reconvert with the current converter (every route is forest-blocked since the ruling), or run with one rank and `forest_chunks: 1` |
| `Configuration error in phase 'post_snapshot':` then `Module '<m>' is listed more than once` | Duplicate module in `post_snapshot`; each entry runs once per snapshot | list the module once |
| `Phase '<phase>': entry N lists M modules (...); each entry must be exactly one 'name: mode' pair` | Two `name: mode` pairs in one sequence item, in any phase (FoF or `post_snapshot`), which would otherwise silently drop one | give each module its own `- name: mode` item |
| `Unknown module '<m>' in phase 'post_snapshot' of the pipeline configuration` | Typo, or the module is not in the compiled `MODEL`'s registry (core test fixtures such as `test_snapshot_fixture` live under `src/module_system/` and exist only in a `TEST_BUILD`) | `make info`; check the module directory and `module_info.yaml` under the compiled model |
| `sham_rank_match must be configured exactly once in modules.pre_timestep as process_full_halo (found ...)`, `sham_rank_match is configured in modules.post_timestep` or `... in substep phase '<name>'`, or `sham_rank_match must be configured in modules.post_snapshot as process_snapshot` | The module's two-phase contract is broken: its per-FoF step (peak tracking, retirement, non-member reset) belongs once in `pre_timestep` as `process_full_halo`, and its whole-snapshot rank match once in `post_snapshot` as `process_snapshot` (horizontal packages only; a vertical reader rejects a non-empty `post_snapshot` first) | fix the run file to match `models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml`; see `models/sham/modules/sham_rank_match/README.md` |
| `sham_rank_match: output snapshot N has z = ... above ShamTargetRedshiftMax` | The target is cross-sectional and an output snapshot lies above the window (an empty `output.snapshot_list` means every snapshot) | list only snapshots inside the window under `output.snapshot_list` (the real-box file uses `[49]`, `z = 0.0005`) or raise `ShamTargetRedshiftMax` deliberately |
| `sham_rank_match` init rejects a parameter (a `ShamTarget*` or `ShamMinVpeak` value not finite or malformed, `ShamTargetPhi1`/`ShamTargetPhi2` not positive, `ShamTargetHubble` outside `(0, 2)`, `ShamMinVpeak` not positive, or `BoxSize` not finite and positive) | The strict parser rejects `1e400`, `1,0` and `abc` (strtod plus a trailing-character and ERANGE check); `nan`, `inf` and `infinity` parse but are rejected by the module's own finite check before any range check (`<param> = nan is not finite`) | read the named parameter in the ERROR line; compare with the shipped `sham_micro-uchuu-ascii-horizontal.yaml` |
| Run stops mid-run naming a snapshot and a non-zero `process_snapshot` return, or `sham_rank_match: snapshot N (z=..., M entries) failed; no stellar mass is assigned for it`, or `sham_rank_match: FoF step at snapshot N failed; nothing was written` | A callback returned an error (for `sham_rank_match`: invalid ID/Type/peaks, a duplicate `UniqueGalaxyID`, a peak mass above the float range, or a rank whose inversion fails); the snapshot callback writes nothing unless every candidate passed, no master file is left behind (it is written only after the driver returns), and partition files of snapshots already closed are kept | the ERROR lines before the FATAL name the rank/`UniqueGalaxyID` and values |
| `hod_populate must be configured exactly once in modules.post_timestep as process_full_halo (found N entries, M as process_full_halo)`, `hod_populate is configured in modules.pre_timestep` or `... in substep phase '<name>'`, or `hod_populate: modules.post_snapshot is configured but does not contain hod_populate as process_snapshot` | The module's placement contract is broken: its retire, reset and draw step belongs once in `post_timestep` as `process_full_halo` (anywhere else it redraws the step per entry or per substep), and a configured `post_snapshot` phase must carry it as `process_snapshot` (the vertical driver has no such phase, so a vertical run file omits it and runs without the audit) | fix the run file to match `models/hod/input/hod_micro-uchuu-horizontal.yaml` (horizontal) or `hod_mini-millennium.yaml` (vertical); see `models/hod/modules/hod_populate/README.md` |
| `hod_populate` init rejects a parameter (`<name> = nan is not finite`, `HODSigmaLogM = <v> must be > 0`, `HODAlpha = <v> must be >= 0`, `HODSeed = <v> must be >= 0`, `HODConcA = <v> must be > 0`, a mass parameter whose `10^x` is not a finite positive normal double, or `hod_populate needs a finite positive BoxSize ...`) | The strict parser rejects `1e400`, `1,0` and `abc`; `nan` and `inf` parse but fail the module's own finite check before any range check | read the named parameter in the ERROR line; compare with `models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml` |
| `module_create_record refused for module '<m>': called <from init() \| from a process_by_galaxy callback \| from a process_per_event callback \| from a process_snapshot callback \| from cleanup() \| outside any module callback> ...; records can only be created from a running process_full_halo callback` | A module asked for a record from a callback that may not create. A module cannot tell its dispatch mode, so the usual cause is a creating module (`hod_populate`, `TestFixtureCreateRecords > 0`) configured `process_by_galaxy` or `process_per_event`. Two more refusals share the prefix: `row pointer is NULL` and `mismatched ModuleContext` (the module passed a NULL row pointer, or a `ModuleContext` that is not the running callback's) | configure the creating module `process_full_halo` |
| `module_create_record refused ...: host_index=<i> is outside the <n> committed rows`, `... is a record created in this FoF step (created rows start at <b>); created rows cannot host`, `... is Type <t>; only a Type 0 or 1 row can host`, `... has no galaxy`, `... has CentralHalo <c>; only the Type 0/1 row inheritance assembled for a subhalo slice (CentralHalo equal to its own row) can host`, or `... already has 1024 created records in this FoF step, the identity radix` | The host argument is wrong, or a module tried to create more than `MAX_CREATED_RECORDS_PER_HOST` (1024) records on one host in one FoF step (creating once per substep in a substep phase multiplies the count) | pass a committed Type 0/1 row below `ngal`; guard substep creation on `ctx->substep_number`; `lambda >= 1024` in `hod_populate` is the same limit |
| `module_create_record refused ...: the <vertical\|horizontal> driver's created-record identity space does not fit int64 (units=<u>, rows_per_unit=<r>, radix=1024), so this run cannot create records`, preceded at startup by the INFO line `Created-record identity space (...): ... does not fit int64; this run cannot create records` | `1024 x units x rows_per_unit` exceeds int64. Only a run that actually creates is affected, and it stops at its first creation. The vertical driver on Shin-Uchuu ASCII (the ASCII reader answers "unknown" for its largest forest, so `rows_per_unit` falls back to the package's 2e10 identity multiplier) and on full Uchuu exceeds it (Shin-Uchuu's tree IDs already use about 61 bits; full Uchuu's startup budget is also over int64; read the logged numbers), so `hod` cannot run there under the vertical driver; the fixtures, mini-Millennium, full Millennium under either driver fit (no horizontal Uchuu package exists, so its budget is not evaluated). Runs that create nothing (every other module, `halos-only`, `sage16`, `sham`) log the verdict and carry on | use the horizontal driver for such a dataset; the recorded escape beyond the scalar budget is pair identity (source-halo ID plus an ordinal column), which is not implemented |
| `module_emit_event invalid source_index=<i>` or `target_index=<i>: outside the <n> committed rows (a record created in the running callback cannot be named until it returns)` | An event names a row the same callback just created; created rows join the workspace when the callback returns | emit the event from a later callback or phase, where the row is an ordinary entry |
| FATAL `Marshalled <p> of <n> created records: a created record's host lies in no output segment` or `FoF workspace created rows are inconsistent` | A core-invariant breach in the marshal merge, not a module error: the created-host map and the output segments disagree (a driver or workspace change) | the Record Creation Contract in `docs/DEVELOPER-GUIDE.md`; run `tests/unit/test_record_creation.c` |

### YAML config failures

`src/core/read_parameter_file.c` validates fixed-schema sections and rejects unknown keys there with a fatal `Unknown key '<section>.<key>'` (the `modules` section variant adds `; supported keys are pre_timestep, ...`). Top-level timestep keys have no whitelist, so typos like `substeps:` can be silently ignored; route top-level key questions through the `mimic-config-and-flags` no-whitelist trap.

| Symptom | Likely cause | First command |
|---|---|---|
| `Unknown key '<section>.<key>'` | Typo, or key belongs to a different section | check the key against a shipped run file, e.g. `models/sage16/input/sage16_mini-millennium.yaml` |
| Startup rejection: run file model/simulation vs compiled binary | Binary compiled with one `MODEL`/`SIMULATION`, run YAML declares another (`model.name`, `simulation.name`) | rebuild with the matching pair, or fix the run file |

### Vertical reader failures

| Symptom | Likely cause | First command |
|---|---|---|
| Cannot open input tree files | Wrong `input.simulation_dir` / `tree_name` / `first_file`/`last_file`, or `snapshots/` symlink missing on this machine | `ls <simulation_dir>` with the exact path from the debug log |
| Unknown/unregistered `tree_type` | Typo, or format not in the reader registry | check `src/io/vertical/registry.c` for registered names |
| `Reader '<name>' is compatible with processing_order '<x>', but input.processing_order is '<y>'` | `tree_type` and `processing_order` disagree — a vertical reader under `horizontal`, or `horizontal_hdf5` under `vertical` | pair them: `horizontal_hdf5` ↔ `horizontal`, the four forest-ordered readers ↔ `vertical` |
| HDF5 reader requested in a `USE-HDF5=no` build | `lhalo_hdf5` / `consistent_trees_hdf5` need HDF5 compiled in | rebuild without `USE-HDF5=no` |
| Relative paths in run file resolve "wrong" | Run-file relative paths resolve from the invocation CWD, not the run-file's directory | rerun from repo root or use absolute paths |

### Output / schema issues

| Symptom | Likely cause | First command |
|---|---|---|
| Binary output "corrupt" / fields misaligned when read | Reading with the wrong schema — read ONLY via that run's own `metadata/output_schema.json`, never the current checkout's metadata | inspect `<output_dir>/metadata/output_schema.json` |
| `--skip` run dies: `Partial output exists for partition <N> (<n> of <m> files)...` | `--skip` skips only when ALL files of an unmarked partition exist; partial → FATAL by design (`src/core/vertical_driver.c`). A partition whose `<OutputDir>/<base>_<NNN>.inflight` marker exists was left mid-write by a kill or `MPI_Abort`, and a `--skip` resume redoes it instead of accepting its files | remove the partial partition's output files (archive, don't delete, per repo rules), rerun |

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
- **(b) Tests can skip silently after renames.** The physics baseline test skipped for a while because its model-name guard checked `"sage"` after the sage→sage16 rename. ALWAYS read the SKIP reasons in `make tests ... summary` output — a green suite with silent skips proves nothing. NA (not applicable) lines are hidden there entirely; watch each tier's `n/a=` count and run a suspect test file directly to see its markers.
- **(c) Unit tests run ONLY via the runner.** Use `tests/unit/run_tests.sh <test_name>`, with `MODEL=<m> SIMULATION=<s>` env vars for non-default pairs. Never execute the `.test` binary directly — it misses the runner's environment setup.
- **(d) CI tolerance is 1e-3, local is 1e-6.** A value drift between 1e-6 and 1e-3 fails locally but passes CI (or the reverse across platforms). Know which tolerance produced the verdict you are reading before acting on it.
- **(e) Plot profile `xlim`/`ylim` keys are silently ignored.** `get_profile_axes` (`plot/mimic-plot/output_utils.py`) reads `xmin`/`xmax`/`ymin`/`ymax` scalars; shipped profiles still contain list-style `xlim`/`ylim` keys that do nothing. If an axis override "doesn't work", this is why.
- **(f) Stale `__pycache__` / orphaned `.pyc` can mislead.** A `.pyc` without its source (e.g. after a figure module rename) can keep old behavior alive or mask an ImportError. `find . -name __pycache__ -exec rm -rf {} +` before trusting weird Python behavior.
- **(g) Some "stale" generated files are not drift.** `tests/generated/module_sources.mk` exists on disk but NO current generator writes it (legacy leftover; the Makefile does not name it). `make check-generated` passing while it looks old is NOT a bug. `init_halo_properties.inc` and `init_galaxy_properties.inc` were the same class and were removed from `GENERATED_HEADERS` and archived on 2026-08-13; `reset_galaxy_properties.inc` was archived on 2026-08-14. Since `src/include/generated/` is gitignored, an older worktree may still show any of these.
- **(h) Run-file relative paths resolve from the invocation CWD**, not from the run file's location. Same run file, different directory → different behavior.
- **(i) `fix_flybys` has been deleted — this symptom can now only come from a build predating the fix, or a stale converted dataset.** It used to collapse every z=0 FoF group into one (negated `MostBoundID`) on `consistent_trees_ascii` and `horizontal_hdf5` data; the lhalo/ctrees-HDF5 readers never had it. If a z=0 diagnostic still looks wrong this way — baryon fraction above cosmic, a truncated Type-0 halo mass function, absurd halo occupation, or any negated `MostBoundID` — check two things: (1) is the executable built from before the fix landed; (2) for a `horizontal_hdf5` dataset specifically, is it stamped `format_version = 1` — such a dataset is rejected outright by the reader, naming the file and version found, so it cannot silently produce this symptom; there is no equivalent stamp on the `consistent_trees_ascii` path, so an ASCII-derived run showing this symptom means an old build, not an old file. A restored corrupt-input guard, `verify_fof_centrals_present()` (both the C reader and the converter), now FATALs instead if a forest's final snapshot has zero FoF centrals — don't confuse that abort with the old defect. The record of the defect is `mimic-failure-archaeology` incident 9.

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
grep -n "compatible with processing_order" src/core/read_parameter_file.c  # reader/order mismatch
git log --oneline 6cbeafe4 -1                                # trap (a) story
ls models/halos-only/                                        # empty-pipeline model exists
```

The tolerance values (local rtol 1e-6 / atol 1e-10, CI `MIMIC_BASELINE_RTOL=1e-3`) live in `tests/framework/harness.py` and `.github/workflows/ci.yml` — check there if a tolerance dispute arises.
