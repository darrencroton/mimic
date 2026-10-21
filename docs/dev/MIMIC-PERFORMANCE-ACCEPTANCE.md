# Mimic Performance Acceptance Record (Step 4, Tier A)

**Purpose:** The evidence of record for [`MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md`](MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md): the baseline every slice is measured against, and one dated section per slice with its gate result, wall-time pair, cost line and findings. Nothing outside `docs/dev/` may cite this file.

**Status:** Open. Created by Slice 1 (2026-10-21); each later slice appends its section, and the closing slice adds the re-profile, the super-forest measurement and the spectrum annotation.

---

## Table of Contents

1. [Baseline](#baseline)
2. [Slice 1: The reference identity gate](#slice-1-the-reference-identity-gate)
3. [Slice 2: Dispatch cost decomposition and health read](#slice-2-dispatch-cost-decomposition-and-health-read)
4. [Slice 3: `DEBUG_LOG` tests the level first](#slice-3-debug_log-tests-the-level-first)

---

## Baseline

The numbers the plan was designed against, copied from its Frozen Facts and Evidence. Every figure is from one machine (Mac Studio, Apple M3 Ultra, 32 cores, 512 GiB, macOS 27.0.1) and one build (`cc -g -O2`, HDF5 1.14.6, libyaml 0.2.5, MPI off); run-to-run noise on the default pair is 1.6% (one standard deviation over 14 repeats).

**The measurement of 2026-10-20** (commit `5daa24ee`, Mac Studio, Apple M3 Ultra, macOS 27.0.1, `cc -g -O2`, HDF5 1.14.6, libyaml 0.2.5, MPI off; `scripts/profiling/` harness per its README: `run_wall.py` 15 repeats with the first discarded, `sample_runs.sh` 20 repeats with the first discarded, `attribute.py --skip-first --wall-mean-s`; `source_attribution.mapped_fraction` 1.0 on both). Not comparable with the 2026-08-20 figures (3.3229 s), which were taken on a different machine.

| | Vertical (`sage16_mini-millennium.yaml`) | Horizontal (`sage16_mini-millennium-horizontal.yaml`) |
|---|---|---|
| Wall, mean ± σ (n = 14) | 2.6335 ± 0.0427 s (min 2.5866, max 2.7466) | 2.7251 ± 0.0403 s (min 2.6904, max 2.8469) |
| Peak RSS (run profile) | 0.066 to 0.074 GB | 0.176 to 0.185 GB |
| Largest live-buffer population `P` / galaxy pool high-water | 14,648 (one tree) / 15,525 | 37,425 (one snapshot) / 38,743 |
| Physics modules | 46.11% | 48.02% |
| Core execution, of which `module_registry.c` | 34.55%, 30.00% | 32.80%, 28.40% |
| Utilities, of which `error.c` | 5.86%, 3.30% | 5.67%, 3.13% |
| Math library | 4.65% | 4.64% |
| Output I/O, of which kernel `__open` via libhdf5 | 4.31%, 2.36% | 2.75%, 2.21% |
| Runtime/system, of which `__bzero` | 3.34%, 2.14% | 2.44%, 0.28% |
| Input I/O | 0.90% | 3.16% |
| `SubSteps: 1` differential | 0.7136 ± 0.0208 s (n = 6), so 0.2133 s per substep and about 0.50 s fixed | not measured |

Hottest lines (vertical, %): `module_registry.c:1603` 5.06 (the by-galaxy `process()` call), `sage_calculate_cooling_budget.c:132` 4.83, `sage_radio_mode_heating.c:248` 4.34, `error.c:211` 3.30, `module_registry.c:1598` 2.98, `sage_apply_star_formation_supernova.c:194` 2.77, `module_registry.c:0` 2.71, `module_registry.c:1604` 2.56, kernel `__open` 2.36, `__bzero` 2.14. The horizontal list is the same set in a slightly different order. Per-line attribution inside a translation unit is soft under `-O2` (the harness's own caveat), which is why Slice 2 exists. The raw profiles are machine-local scratch and are not cited; the facts above are what they showed.

**The research-scale measurement of 2026-10-21** (commit `8613bded`, same machine and build, `sage16` on all 128 mini-Uchuu files, 1,451,359,554 halos, the vertical run file's `last_file` raised to 127 in a scratch copy; 3 wall repeats after a warm-up, 2 sampled runs at a 5 ms interval, about 700,000 samples per driver; full tables, method and the derived cost model in [`MIMIC-PERFORMANCE-REFERENCE.md`](MIMIC-PERFORMANCE-REFERENCE.md)).

| | Vertical (`mini-uchuu`) | Horizontal (`mini-uchuu-horizontal`) |
|---|---|---|
| Wall, mean of 3 | 2167.0 s, σ 7.1 s | 2267.1 s, σ 73.1 s (one slow repeat; the other two average 2225.0 s) |
| Peak RSS | 1.82 GB | 55 to 63 GB (retention pool at most 43.2 GB, 2 generations) |
| `C` / `P` | 3,853,400 / 743,295 (the 5× seed on a 770,680-halo forest) | 41,788,163 / 39,452,969 |
| `SubSteps: 1` | 548.3 s, so 179.9 s per substep and 368.4 s (17%) fixed | 573.2 s, so 188.2 s per substep and 385.0 s (17%) fixed |
| Physics modules | 49.53% | 49.62% |
| `module_registry.c` | 30.82% | 29.60% |
| `error.c` | 3.71% | 3.56% |
| Math library | 4.53% | 4.45% |
| Output I/O, of which kernel `__open` | 1.07%, 0.05% | 0.41%, 0.02% |
| `__bzero` / `_platform_memmove` | 2.16% / 0.09% | 0.52% / 2.28% |
| Input I/O | 0.87% | 2.85% |

---

## Slice 1: The reference identity gate

**What changed.** `make tests-reference-identity REFERENCE_COMMIT=<hash>` (`tests/manual/test_reference_identity.py`) requires per-`UniqueGalaxyID` byte identity, and identical files, HDF5 objects, attributes (apart from the named provenance) and `metadata/`, between a required ancestor commit and HEAD on the real mini-Millennium data: `{halos-only, sage16} x {vertical, horizontal} x {fixed, dynamic}`, eight legs. The worktree, reference and output-comparison helpers moved from `tests/manual/test_snapshot_disabled_identity.py` into `tests/framework/parity_gate.py` (parameterised where they read battery-local state), plus one helper for the reusable HEAD worktree (`head__<model>__<simulation>`), and both tests import them.

**Gate result, first green run** (2026-10-21, `REFERENCE_COMMIT=5daa24ee`). HEAD was `faa2228b`, the slice's starting commit, whose `src/`, `models/`, `simulations/`, `scripts/` and `Makefile` equal the reference's, so the two builds are the same code and this run proves the instrument, not a change. The slice's `Makefile` target was not yet committed, and the gate refuses a runtime tree with uncommitted edits, so the Makefile edit was set aside and the script was run directly as `python3 tests/manual/test_reference_identity.py`. Exit 0; eleven markers, all `PASS` (resolution, builds, eight legs, verdicts); 69.5 s with both sides' eight builds from cold (the HEAD worktrees were created by the run). Evidence: `archive/reference-identity/20261021T024358Z-ref5daa24ee-headfaa2228b/evidence.json` (machine-local).

| Leg | Verdict | Galaxies compared | Output snapshots | Fields | Master links | Galaxy differences | `P` (both runs) |
|---|---|---|---|---|---|---|---|
| `halos_only__vertical__fixed` | PASS | 292,163 | 8 | 20 | 128 | 0 | 40,537 |
| `halos_only__vertical__dynamic` | PASS | 292,163 | 8 | 20 | 128 | 0 | 40,537 |
| `halos_only__horizontal__fixed` | PASS | 292,163 | 8 | 20 | 8 | 0 | 82,137 |
| `halos_only__horizontal__dynamic` | PASS | 292,163 | 8 | 20 | 8 | 0 | 82,137 |
| `sage16__vertical__fixed` | PASS | 187,832 | 8 | 42 | 128 | 0 | 14,648 |
| `sage16__vertical__dynamic` | PASS | 187,817 | 8 | 42 | 128 | 0 | 14,643 |
| `sage16__horizontal__fixed` | PASS | 187,832 | 8 | 42 | 8 | 0 | 37,425 |
| `sage16__horizontal__dynamic` | PASS | 187,817 | 8 | 42 | 8 | 0 | 37,424 |

The `sage16` fixed legs report `P` of 14,648 (vertical, one tree) and 37,425 (horizontal, one snapshot), the baseline table's figures, so the legs ran on the real data. The only differences any leg permitted were `RunProperties/Version@git_commit`, `@git_date` and `@version`, `metadata/version_info.json`, and `RunProperties@RunEndTime`, which a leg names only when the two runs' end times differ: seven legs named it, and `halos_only__horizontal__dynamic` did not, because both runs ended within the same second.

**Gate result at the final code commit** (2026-10-21, `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` at `b250d010`, the last commit to change code; the acceptance record was committed afterwards, and `docs/dev/` is not a runtime path). Exit 0; eleven markers, all `PASS`, of which the eight `test_leg_` markers are the legs; no FAIL, ERROR, SKIP or NA; `HEAD build flags: none`; 47.4 s with both sides' worktrees already built. Every leg compared the same galaxy counts as the first run with zero galaxy differences, and the `sage16` fixed legs again report `P` of 14,648 (vertical) and 37,425 (horizontal). The permitted differences were `RunProperties/Version@git_commit`, `@git_date` and `@version` and `metadata/version_info.json` in every leg, and `RunProperties@RunEndTime` in five of the eight (not in `halos_only__vertical__dynamic`, `halos_only__horizontal__fixed` or `halos_only__horizontal__dynamic`, whose two runs ended in the same second), which is why the set a leg names varies from run to run. Evidence: `archive/reference-identity/20261021T041301Z-ref5daa24ee-headb250d010/evidence.json` (machine-local).

**Cost.** One new test file, one moved-and-parameterised helper block in `tests/framework/parity_gate.py` (about 500 lines moved out of the battery, with three optional hooks in the comparison for the battery-only pinned-description and empty-list allowances), one Makefile target, documentation. No C change, no comparator change.

**Findings.** (1) The comparator's default mode excludes created rows (`UniqueGalaxyID` below zero) from the byte comparison and only counts them; the two models on mini-Millennium create none, so nothing is unchecked here, but a later plan whose slice creates records must use the comparator's `--compare-created`. (2) The gate refuses a runtime tree with uncommitted edits by design, so a slice's gate run is taken at its commit; a fix after a failed gate is a new commit.

---

## Slice 2: Dispatch cost decomposition and health read

**What changed.** No tracked file but this record. Four scratch variants of `74d9376d` (the slice's starting commit; `src/` equals `5daa24ee`'s), each a detached worktree under `output/perf-slice2/` with its own build per simulation package (`MODEL=sage16`, `SIMULATION=mini-millennium` and `mini-millennium-horizontal`) and its `snapshots` links pointed at the main tree's real data: `base` (the default `-g -O2` build, unmodified), `nodbg` (`DEBUG_LOG` in `src/util/error.h` replaced by `do { } while (0)`, nothing else changed), `noinl` (the default source built with `EXTRA_CFLAGS="-fno-inline"`) and `count` (untimed: static counters and an `atexit` report added to `execute_phase`, `dispatch_events_range` and `module_emit_event` in `src/core/module_registry.c`). Measured 2026-10-21 on the plan's machine with nothing else running; each wall pair was taken back to back. Evidence: `archive/performance/slice-002/` (machine-local).

### Callback counts (measured, untimed `count` build)

The two drivers dispatch exactly the same callbacks on the default run; every count below was identical vertical and horizontal.

| Count | Default run |
|---|---|
| Full-halo callbacks | 46,426,014 |
| Per-event callbacks | 48,044 (24,022 events emitted, each delivered to two consumers; no unsubscribed consumer skipped) |
| By-galaxy callbacks | 171,932,057 |
| All module callbacks | 218,406,115 |
| `execute_phase` calls / of which empty (early return) | 30,040,362 / 1,365,471 |
| PASS 1 module-loop iterations / failing the full-halo mode test | 223,937,244 / 177,511,230 (79.3%) |
| PASS 2 rows visited / skipped (`galaxy == NULL` or `Type == 3`) | 32,857,209 / 77,867 |
| PASS 2 module-loop iterations / failing the by-galaxy mode test | 256,204,525 / 84,272,468 (32.9%, 7.8 iterations per row visited) |
| `dispatch_events_range` calls / early returns | 46,450,036 / 46,426,014 (every post-callback safety dispatch returned early; the 24,022 that ran are the immediate deliveries) |

The `DEBUG_LOG` sites in `module_registry.c` are reached once per callback (line 1554 per full-halo, 1598 per by-galaxy, the per-event site per delivery), so the dispatch path alone evaluates the rate-limit predicate 218,406,115 times a run; the module-side `DEBUG_LOG` sites were not counted.

### `DEBUG_LOG`-off wall pair (measured, n = 14, repeats 1 to 14 of 15)

| | Vertical | Horizontal |
|---|---|---|
| `base` (default build) | 2.7201 ± 0.0337 s (min 2.6401, max 2.7634) | 2.7786 ± 0.0336 s (min 2.7195, max 2.8488) |
| `nodbg` (`DEBUG_LOG` compiled out) | 2.5130 ± 0.0546 s (min 2.4475, max 2.6855) | 2.5988 ± 0.0313 s (min 2.5224, max 2.6381) |
| Saving (difference of means) | 0.2071 s, 7.61% | 0.1798 s, 6.47% |
| SE of the difference | 0.0171 s (12.1 SE) | 0.0123 s (14.6 SE) |

Every repeat exited 0 and reported `P` of 14,648 (vertical) and 37,425 (horizontal). The `nodbg` galaxies are byte-identical to `base` on both drivers (`scripts/compare_cross_format_identity.py` default mode, exit 0, 187,832 galaxies over 8 snapshots, 42 fields). This is the ceiling Slice 3 can reach, and it is above the plan's 3.5 to 6% estimate: compiling the macro out removes every one of the 103 sites (module and core) and their argument evaluation, not only the predicate call the profile attributes to `error.c`. The `base` means are 3.3% (vertical) and 2.0% (horizontal) above the plan's baseline table; the scratch builds live on `/Volumes/Internal` through `output/`, and the pair's two sides share that placement, so the difference is the comparison and the absolute level is not.

### `-fno-inline` profile of `module_registry.c` (measured; a ranking, not `-O2` percentages)

`sample_runs.sh` 20 repeats, `attribute.py --skip-first --wall-mean-s` (19 reports per driver; 50,262 vertical and 52,075 horizontal self-samples). `source_attribution.mapped_fraction` 1.0 on both; `Unattributed` 0.03% (vertical) and 0.30% (horizontal). The `-fno-inline` build's own wall mean, the `--wall-mean-s` argument: 3.0463 ± 0.0550 s vertical, 3.1233 ± 0.0344 s horizontal, so suppressing inlining costs about 12% and inflates every helper's share by its call overhead. `module_registry.c` totals 29.47% (vertical) and 28.13% (horizontal), against 30.00% / 28.40% at `-O2`. The per-function self-samples were summed from the reports with the harness's own parser (`parse_sample.py`); they equal the harness's `module_registry.c` total exactly. `attribute.py`'s hot-line list stops at 60 entries, which is why it was not used for this table.

| Function | Vertical % (ms) | Horizontal % (ms) | Hottest lines, vertical % |
|---|---|---|---|
| `execute_phase` (self) | 17.21 (524) | 16.69 (521) | 1598 `DEBUG_LOG` by-galaxy 2.56, 1603 by-galaxy `process()` 2.05, 1586 PASS 2 module loop 1.62, 1540 PASS 1 mode test 1.33, 1581 galaxy loop 1.23, 1587 PASS 2 mode test 0.88, 1591 `resolved` load 0.87, 1554 `DEBUG_LOG` full-halo 0.73 |
| `enter_callback` | 2.84 (86) | 2.48 (77) | 127 2.22 |
| `leave_callback` | 2.34 (71) | 2.28 (71) | 132 2.34 |
| `dispatch_events_range` | 1.98 (60) | 1.99 (62) | 1041 1.08, 971 0.90 (entry and early return) |
| `end_phase_event_dispatch` | 1.63 (50) | 1.47 (46) | 961 0.86, 948 0.51 |
| `commit_created_records` | 1.47 (45) | 1.37 (43) | 1465 0.67, 1484 0.53 (the `staged == 0` early return) |
| `begin_phase_event_dispatch` | 0.96 (29) | 0.94 (29) | 940 0.71 |
| `execute_module_pipeline` | 0.64 (20) | 0.59 (18) | 1705 0.30 |
| `update_context_for_substep` | 0.35 (11) | 0.27 (9) | 1669 0.28 |
| `begin_record_creation_step` | 0.04 (1) | 0.05 (2) | |
| `workspace_callback_count` | 0.01 (0) | 0.00 (0) | |
| `module_emit_event`, `find_module_by_id`, `ensure_phase_event_capacity` | 0.00 | 0.00 | |
| `is_debug_log_rate_limiting_enabled` (`error.c`, for reference) | 5.68 (173) | 5.75 (180) | `error.c:211` |

### Decomposition (qualitative ranking from the `-fno-inline` profile; each share measured in that build, its `-O2` equivalent estimated)

The `module_registry.c` self-time plus the predicate in `error.c`, grouped by what the line does (vertical / horizontal, % of the `-fno-inline` profile):

1. **Per-callback bookkeeping**, 14.80 / 13.91%: `enter_`/`leave_callback`, `begin_`/`end_phase_event_dispatch`, the always-early safety `dispatch_events_range`, `commit_created_records`, which returns at its `staged == 0` test on every call because the default run creates no record (the comparator counts zero created rows), the per-callback state stores and result test, and `execute_phase`'s entry test. Largest under `-fno-inline` because every helper became a call; at `-O2` these fold into stores in `execute_phase` and the share is smaller (estimated), but the counts show the work: 46.4 million safety dispatches and 46.4 million commits that find nothing to do, and 30.0 million begin/end pairs.
2. **Logging macro**, 8.97 / 8.85%: the predicate call (5.68 / 5.75%) plus the two dispatch sites (1598, 1554). Measured directly at `-O2` by the wall pair above: 7.6% / 6.5% of the run for all 103 sites.
3. **Mode re-scan**, 4.55 / 4.39%: the PASS 1 and PASS 2 module loops and their mode tests (lines 1539, 1540, 1586, 1587), with 79.3% of PASS 1 and 32.9% of PASS 2 iterations failing the test (counted); plus the galaxy loop and row filter (1581, 1582) at 1.80 / 1.90%, which also runs in the phases that dispatch no by-galaxy module (two of four on the shipped run file, per the plan), and the `resolved` load and check (1544, 1591) at 1.02 / 1.04%.
4. **Indirect call**, 2.55 / 2.48%: the `mod->process()` call lines (1563, 1603); the irreducible cost of the module ABI.
5. **Pipeline and substep loop**, 0.99 / 0.86%, and line 0, 0.47 / 0.45%.

### Per-callback cost

| Figure | Vertical | Horizontal | Label |
|---|---|---|---|
| Default run wall ÷ by-galaxy callbacks | 15.82 ns (2.7201 s; 15.32 ns on the baseline's 2.6335 s) | 16.16 ns (2.7786 s; 15.85 ns on 2.7251 s) | measured |
| `DEBUG_LOG`-off saving ÷ by-galaxy callbacks | 1.205 ± 0.099 ns | 1.046 ± 0.072 ns | measured |
| The same saving ÷ all 218,406,115 callbacks | 0.948 ns | 0.823 ns | measured (an upper bound for the dispatch sites, since the saving includes the module sites) |
| `module_registry.c` at `-O2` ÷ all callbacks | 3.62 ns (30.00% of 2.6335 s) | 3.54 ns (28.40% of 2.7251 s) | estimated (baseline share × baseline wall ÷ counted callbacks) |

### `code-health` read of `src/core/module_registry.c` (the baseline for Slice 4's differential)

`health.py analyze --all --json` at `74d9376d` (absolute mode; Lizard 1.23.0 covers C cyclomatic complexity; duplication is the skill's stdlib lexical scan with a 6-line minimum window). Evidence: `archive/performance/slice-002/health_all.json` (machine-local).

| Measure | Value |
|---|---|
| Lines (physical / code / comment / blank) | 1,998 / 1,322 / 451 / 225 |
| Functions | 67 |
| Cyclomatic complexity, sum / mean / median / max | 351 / 5.24 / 4 / 17 |
| Functions at CCN 10 or more / above 15 | 11 / 3 |
| `execute_phase` CCN | 17 (joint maximum with `module_emit_event`; `free_phase_configuration` 16) |
| `dispatch_events_range` CCN | 14 |
| `begin_`/`end_phase_event_dispatch`, `commit_created_records`, `execute_module_pipeline` CCN | 2, 2, 3, 3 |
| `enter_`/`leave_callback`, `workspace_callback_count` CCN | 1, 1, 1 |
| Repository context (all functions Lizard measured in the run) | 2,285 functions, CCN 90th percentile 8, 99th percentile 20 |
| Duplicate windows touching the file | 10 signatures: 7 within the file, 4 with another file (one is both) |
| Duplicate inside `execute_phase` | lines 1541 and 1588, 8 lines: the unresolved-entry `FATAL_ERROR` block repeated in PASS 1 and PASS 2 |
| Include fan-out / fan-in | 12 / 0 |

The other in-file duplicates are in the phase-query and validation helpers (lines 521/562, 537/576, 591/711, 628/698, 379/447/1968, 1944/1976) and in the parameter getters, outside the dispatch path. Interpretation: `execute_phase`'s CCN of 17 lies between the repository's 90th (8) and 99th (20) percentiles, high for a dispatcher whose contract is six sentences; the repeated unresolved-entry block is the one duplicate Slice 4's single compaction pass removes by design.

**Cost.** No tracked change but this section. Scratch: eight worktrees under `output/perf-slice2/` (kept for Slice 3's comparison; registered with `git worktree`, not committed), and evidence under `archive/performance/slice-002/`. The main tree's default build was rebuilt afterwards: `make` and `make check-generated` exit 0.

**Findings.** (1) The `DEBUG_LOG` ceiling is 7.6% / 6.5% at 12 to 15 SE, above the plan's 3.5 to 6%, because compiling the macro out removes all 103 sites; Slice 3 keeps the macro and should expect to land below this ceiling, by the residual cost of a level test per site. (2) Every post-callback safety `dispatch_events_range` call on the default run returns early (46,426,014 of 46,426,014), and every `commit_created_records` returns at `staged == 0` (no record is created), which supports Slice 4's skip-when-nothing-pending. (3) The two drivers dispatch the identical callback sequence counts, so per-callback figures carry across drivers. No go or no-go for Slice 5 is drawn here.

---

## Slice 3: `DEBUG_LOG` tests the level first

**What changed.** `DEBUG_LOG` (`src/util/error.h`) tests an inline predicate, `debug_log_enabled()`, over the exported `mimic_log_level` before anything else, and its armed branch reads the exported `mimic_debug_log_rate_limiting` flag directly. With debug output off the macro makes no function call and evaluates none of its arguments; the per-site budget (`DEBUG_LOG_MAX_CALLS`) is spent only by a message that is emitted. `src/util/error.c` exports the two variables (renamed from the static `current_log_level` and `debug_log_rate_limiting_enabled`); `set_log_level`, `get_log_level`, `enable_`/`disable_`/`is_debug_log_rate_limiting_enabled` keep their signatures and behaviour. No call site changed. New unit test `tests/unit/test_debug_log_gate.c` (four cases, output redirected through `set_log_output`): above DEBUG a side-effecting argument is never evaluated and nothing is written, armed or not, at INFO and at FATAL; at DEBUG disarmed every call emits once; at DEBUG armed a site emits `DEBUG_LOG_MAX_CALLS` messages and exactly one suppression line; and a site called 50 times above DEBUG and then at DEBUG still emits its full budget plus the one suppression line, evaluating its argument only for the emitted messages. `docs/DEVELOPER-GUIDE.md` (Logging Macros) and `CHANGELOG.md` (a developer-facing line, no budget claim) describe it. The code commit is `f6502a90`; this record follows in a later commit, `docs/dev/` not being a runtime path.

**Gate result** (2026-10-21, `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` at `f6502a90`). Exit 0; eleven markers all `PASS`, of which the eight `test_leg_` markers are the legs; no FAIL, ERROR, SKIP or NA; `HEAD build flags: none`; all eight legs report zero galaxy differences and the permitted provenance differences only. Evidence: `archive/reference-identity/20261021T051059Z-ref5daa24ee-headf6502a90/evidence.json` (machine-local). The `--debug` output of the default vertical run is also unchanged: 616 lines before and after, 27 `suppressed` lines before and after, and a diff showing only the version string (`-dirty`), start time, run-time stamps, the `error.c` line number in the start-up message (93 to 96) and peak RSS.

**Other validation.** `make tests summary` exit 0 (unit 603 passed, 27 n/a; integration 257 passed, 12 n/a; scientific 47 passed); `make tests-converter` exit 0 (1655 passed); `make check-docs` and `make check-format` pass; the differential `lint` verdict is `pass`. `tests/unit/run_tests.sh` builds every unit test, `test_debug_log_gate` included, with `error.c` in its `UTIL_SRCS`.

### Wall-time pair (measured, n = 14, repeats 1 to 14 of 15, the plan's protocol from the main tree)

`before` is the default build at `978de076` (source equal to `5daa24ee`'s), `after` the same tree with this slice, both built and run in the main tree, back to back, nothing else running.

| | Vertical | Horizontal |
|---|---|---|
| Before | 2.7804 ± 0.0124 s (min 2.7515, max 2.8029) | 2.8763 ± 0.0114 s (min 2.8510, max 2.8898) |
| After | 2.6004 ± 0.0136 s (min 2.5844, max 2.6389) | 2.7085 ± 0.0101 s (min 2.6962, max 2.7329) |
| Saving (difference of means) | 0.1800 s, 6.47% | 0.1678 s, 5.83% |
| SE of the difference | 0.0049 s (36.5 SE) | 0.0041 s (41.2 SE) |

Every repeat exited 0. The after mean is below the before mean on both drivers, so the no-regression bound (at most before plus 2 SE) holds with a large margin. **Against Slice 2's ceiling** (`DEBUG_LOG` compiled out: 7.61% vertical, 6.47% horizontal) the slice recovers 6.47 of 7.61 points (85%) on the vertical driver and 5.83 of 6.47 (90%) on the horizontal one; the remainder is the level test left at each of the 103 sites and the still-present macro code at each, as Slice 2 predicted. The before means are 2.2% (vertical) and 3.5% (horizontal) above Slice 2's `base` pair and 5.6% above the plan's baseline table on both drivers, so the machine's absolute level drifts between sessions and builds in different trees; only the within-session differences are comparable, and the after means are within 1.3% of the baseline table.

### Profile after (measured, 20 repeats, `attribute.py --skip-first`, vertical)

`sample_runs.sh` 20 repeats (19 reports, 41,880 self-samples), `--wall-mean-s 2.6004`; `source_attribution.mapped_fraction` 1.0, `Unattributed` 0.03%. `is_debug_log_rate_limiting_enabled` is in no report (zero of the 19 sample files name it), it is absent from the 60-line hot-line table, and `error.c` has no self-time row at all in the per-file attribution (0.00%, against 3.30% at the baseline). `module_registry.c` is 28.10% (30.00% at the baseline), of which the by-galaxy `process()` call line (1603) is 2.63% and the loop and mode-test lines 1586, 1587, 1591 are 1.91, 1.50, 1.32% in the hot-line table. The Utilities component is 2.76% (numeric 1.25%, kernel `read` 1.20%, memory 0.08%). Evidence: `archive/performance/slice-003/agg.json` (machine-local).

**Cost.** `git diff --stat 978de076..f6502a90`: 5 files, 275 lines added and 23 removed; code: `error.h` +31 -11, `error.c` +15 -12 (renames and two comments), the new test 226 lines, documentation 3 lines. Differential `code-health` (`--base 978de076`, Lizard 1.23.0 covered): `error.h` 97 to 102 code lines with one new function (`debug_log_enabled`, CCN 1); `error.c` unchanged in structure (19 functions, CCN sum 45, max 16 as before); the test file is 142 code lines, 10 functions, CCN sum 20, max 3, and two six-line duplicate blocks inside it (the site loops and their line-count asserts), left as they are because each scenario's setup is its own contract. No new dependency, include edge or file in `src/`. Verdict: a 23-line change to one header and its translation unit buys 5.8 to 6.5% of the default run on both drivers at 36 to 41 SE, with no call site touched and the output byte-identical.

**Findings.** (1) The exported-variable design needs no new symbol beyond the two variables and the inline predicate in `error.h`; `error.c` was already in the unit runner's `UTIL_SRCS` and in the converter suite's standalone links, and both builds passed. (2) `make tests` fails two cases of `tests/integration/test_parity_gate_helpers.py` when the process's `TMPDIR` lies inside the repository tree (this run's `TMPDIR` is the slice directory under `.pm/`): the failing messages show the helper's worktree re-rooting a path that sits inside the repository, and the horizontal run then reads the original snapshot list. With `TMPDIR` outside the repository the same target exits 0 (257 integration cases passed), and the test file passes on its own. It was not re-run at the starting commit; the test exercises the `tests/framework/parity_gate.py` helpers, not logging, and the failure follows `TMPDIR`, so it is recorded as a property of the supervised environment rather than fixed, `parity_gate.py` being outside this slice's surface. (3) `scripts/profiling/nm_index.json` (a gitignored, machine-local cache) still lists the logging accessors; it is regenerated by the harness and holds no claim.
