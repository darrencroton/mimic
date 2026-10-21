# Mimic Performance Acceptance Record (Step 4, Tier A)

**Purpose:** The evidence of record for [`MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md`](MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md): the baseline every slice is measured against, and one dated section per slice with its gate result, wall-time pair, cost line and findings. Nothing outside `docs/dev/` may cite this file.

**Status:** Open. Created by Slice 1 (2026-10-21); each later slice appends its section, and the closing slice adds the re-profile, the super-forest measurement and the spectrum annotation.

---

## Table of Contents

1. [Baseline](#baseline)
2. [Slice 1: The reference identity gate](#slice-1-the-reference-identity-gate)
3. [Slice 2: Dispatch cost decomposition and health read](#slice-2-dispatch-cost-decomposition-and-health-read)
4. [Slice 3: `DEBUG_LOG` tests the level first](#slice-3-debug_log-tests-the-level-first)
5. [Slice 4: Init-time phase plans in `execute_phase`, measured against the guards](#slice-4-init-time-phase-plans-in-execute_phase-measured-against-the-guards)
6. [Slice 5: Dispatch residual (no-go)](#slice-5-dispatch-residual-no-go)
7. [Slice 6: The vertical output buffer's seed and zero-fill](#slice-6-the-vertical-output-buffers-seed-and-zero-fill)

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

---

## Slice 4: Init-time phase plans in `execute_phase`, measured against the guards

**What changed.** The init-time plan was committed, by the measured rule below. `module_system_init()`, as its last step after every module's `init()` has returned 0, builds one plan per non-empty FoF phase (`pre_timestep`, each named substep phase, `post_timestep`; `post_snapshot` skipped by name) through `for_each_phase()`. A plan is a private `struct PhasePlan` in a table bounded by `MAX_SUBSTEP_PHASES` + 2, keyed by the phase's `struct PhaseModuleConfig *` array, recording the phase's entry count and its resolved full-halo, per-event and by-galaxy modules, each in YAML order, a repeated entry once per occurrence, in one `mymalloc_cat` allocation of `num_entries` slots (`MEM_UTILITY`) per plan. A NULL `resolved` entry met while building is a `FATAL_ERROR` naming the module and the phase. One helper, `release_phase_plans()`, frees them: it runs at the top of `free_phase_configuration()`, before the phase arrays are freed (so both of `module_system_cleanup()`'s branches release them), and at the entry of `module_system_init()`, where it releases only what an earlier successful initialisation left (the table is populated only by a successful initialisation, and the call is a no-op otherwise). `execute_phase()` keeps the `num_modules == 0` early return, then looks the plan up through `find_phase_plan()` (private; returns NULL when the array has no plan or its `num_modules` differs from the recorded entry count) and raises one `FATAL_ERROR` before dispatching anything when there is none; PASS 1 walks the full-halo list, the safety dispatch after each full-halo callback runs only when `last_dispatched_event < event_count`, and the galaxy loop is entered only when the by-galaxy list is non-empty and walks only that list. `dispatch_events_range()` walks the per-event list, handed over through two `phase_event_state` fields (`per_event`, `num_per_event`, replacing `phase_config` and `num_modules`) that `begin_phase_event_dispatch()` sets and `end_phase_event_dispatch()` clears. Neither function contains a `processing_mode` comparison. `execute_phase`'s signature, `types.h`, `execute_post_snapshot`, `module_create_record()` and the commit, `module_emit_event`, `execute_module_pipeline` and every test are unchanged; the per-callback `ctx->active_event = NULL` store and the row filter (`galaxy == NULL || Type == 3`) stay. The code commit is `0b76a45b`; this record follows in a later commit.

**The duplicated unresolved-entry `FATAL_ERROR` block** that Slice 2's health read found in PASS 1 and PASS 2 (lines 1541/1588) is removed: an unresolved entry is now caught once, while building the plan, and `execute_phase` refuses a phase with no plan. The guards alone would not have removed it.

### Three-build comparison (measured, interleaved, n = 40 per build per driver)

Three full builds of their sources at `before_head` `614e7ff4`, in scratch worktrees `output/perf-slice4-r6/{base,guards,plan}-{v,h}` (one build per simulation package per candidate; `make -j16 MODEL=sage16 SIMULATION=<sim>`, every build compiling `module_registry.c` with 0 warnings). Before timing, each binary was checked against its source: the plan binaries carry the plan's `FATAL_ERROR` string and the others do not, and the three `module_registry.c` objects differ from each other. Harness: `archive/performance/slice-004-r6/s4-three-build.py` (machine-local), after PM's `s4-interleave.py`: the repository root as the working directory, every binary by absolute path with `-q`, 41 rounds of which round 0 was a discarded warm-up; within a round each driver's three builds ran round-robin, the first build rotating each round. Every run exited 0; nothing else ran on the machine. SE is the two σ in quadrature over √40.

| | Vertical | Horizontal |
|---|---|---|
| `before_head` | 2.4576 ± 0.0153 s (min 2.4189, max 2.4977) | 2.5656 ± 0.0192 s (min 2.5006, max 2.5951) |
| Guards only | 2.4164 ± 0.0522 s (min 2.3586, max 2.6483) | 2.5104 ± 0.0199 s (min 2.4653, max 2.5779) |
| Init-time plan | 2.3455 ± 0.0369 s (min 2.2913, max 2.5456) | 2.4541 ± 0.0252 s (min 2.3954, max 2.5786) |
| Guards − `before_head` | −0.0412 s, −1.68%, SE 0.0086 (−4.8 SE) | −0.0552 s, −2.15%, SE 0.0044 (−12.6 SE) |
| Plan − `before_head` | −0.1122 s, −4.56%, SE 0.0063 (−17.7 SE) | −0.1114 s, −4.34%, SE 0.0050 (−22.2 SE) |
| Plan − guards | −0.0710 s, −2.94%, SE 0.0101 (−7.0 SE) | −0.0563 s, −2.24%, SE 0.0051 (−11.1 SE) |

**Decision.** The rule: the init-time plan is committed if and only if its mean is below the guards-only mean by more than 2 SE on both drivers. It is below by 7.0 SE (vertical) and 11.1 SE (horizontal), so **the init-time plan is committed**. The committed variant is below `before_head` by 17.7 SE on the vertical driver and is not above it on the horizontal one (it is 22.2 SE below), so the comparison's wall bound holds. The guards measured −1.68% here against revision 5's −2.3% (n = 30, a different session).

**The candidate not committed** (guards only): `git diff --stat` `src/core/module_registry.c | 11 ++++++++---`, 1 file changed, 8 insertions, 3 deletions; patch `archive/performance/slice-004-r6/guards-only.patch` (machine-local), the Design section's edit verbatim. **The measured patch** of the committed variant is `archive/performance/slice-004-r6/init-time-plan.patch` (`src/core/module_registry.c | 209`, 159 insertions, 50 deletions). The committed executable code equals it: `src/core/module_registry.c` at `0b76a45b` differs from the measured file only in comments (three comment additions, after the measurement), and the two files are equal with comments and whitespace removed (`archive/performance/slice-004-r6/strip_compare.py`, output `executable-equal`); the build configuration is the default.

**Gate result** (2026-10-21, `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` at `0b76a45b`). Exit 0; eight `PASS test_leg_` markers and no FAIL, ERROR, SKIP or NA; `HEAD build flags: none`; all eight legs passed in 39 s. Evidence: `archive/reference-identity/20261021T114614Z-ref5daa24ee-head0b76a45b/evidence.json` (machine-local).

**Other validation.** At the code's final source, before the commit, with `TMPDIR` outside the repository: `make tests summary` exit 0 (unit 603 passed, 27 n/a; integration 257 passed, 12 n/a; scientific 47 passed, one `WARN test_zero_values`, a non-failing marker on output the byte gate shows identical); `make tests-converter` exit 0 (1655 passed); `make tests-distributed` exit 0 (264 checks, no skips), then `make` exit 0; `scripts/fuzz_pipeline.py --runs 200 --sampling valid-subset` 200 passed, 0 failed (run with the project venv's `python3`; the system `python3` has no PyYAML and the fuzzer refused to start under it). `make check-docs`, `make check-format` and the differential `lint` (verdict `pass`) are clean. **Memory leaks:** the full unit-tier log (`make tests-unit`) reports `Memory leak detected` 14 times at `before_head` and 14 times with the plan, with identical surrounding lines, so the slice adds no leak report (both logs exit 0, 603 passed). No unit test resets the memory system with live plans; the tests that build phase configurations by hand and call `module_system_init()`/`module_system_cleanup()` (`test_module_configuration.c`, `test_snapshot_collectives.c`, `test_event_buffer_growth.c`, the dependency-contract tests) pass unchanged.

### Wall-time pair (measured, n = 14, repeats 1 to 14 of 15, the plan's protocol from the main tree)

`before` is the default build of `before_head` (`614e7ff4`), `after` the code commit `0b76a45b`, each rebuilt in the main tree (the rebuild verified to recompile `module_registry.c` and the binary checked for the plan's string) and timed back to back with `run_wall.py`, vertical then horizontal, nothing else running.

| | Vertical | Horizontal |
|---|---|---|
| Before | 2.5749 ± 0.1187 s (min 2.4310, max 2.8482) | 2.6332 ± 0.0350 s (min 2.5765, max 2.6870) |
| After | 2.4478 ± 0.0847 s (min 2.3176, max 2.6085) | 2.5069 ± 0.0295 s (min 2.4610, max 2.5448) |
| Saving (difference of means) | 0.1271 s, 4.94% | 0.1263 s, 4.80% |
| SE of the difference | 0.0390 s (3.3 SE) | 0.0122 s (10.3 SE) |

Every repeat exited 0. The after mean is below the before mean on both drivers, so the no-regression bound holds. The vertical pair was noisier in this session (σ 0.12 and 0.08 s against 0.01 to 0.05 s elsewhere), which is why its SE is larger; the interleaved comparison above is the more precise figure for the saving.

### Profile after (measured, 20 repeats, `attribute.py --skip-first`, vertical)

`sample_runs.sh` 20 repeats of the default build at `0b76a45b` (19 reports, 34,682 self-samples), `--wall-mean-s 2.4478`; `source_attribution.mapped_fraction` 1.0, `Unattributed` 0.02%. **`module_registry.c` is 25.79% ± 0.27** of the vertical run, against Slice 3's after-profile 28.10% (`archive/performance/slice-003/agg.json`) and 30.00% at the baseline; 631 ms equivalent against 731. Its hottest lines are now the by-galaxy `process()` call (3.88%) and the by-galaxy list loop (2.72%), then the function entry (1.72%), the full-halo list loop (1.45%), the plan lookup (`find_phase_plan`, inlined, 1.31%) and `begin_phase_event_dispatch` (1.09%); no mode-test line exists. Evidence: `archive/performance/slice-004-r6/agg.json` (machine-local).

**Cost.** `git diff --stat 614e7ff4..0b76a45b`: 5 files, 184 lines added and 57 removed; `module_registry.c` +166 −51 (of the added lines, 43 are comments and 13 blank), `module_registry.h` +6 (documentation), and 12 documentation lines in the guide, skill and changelog. Differential `code-health` (`health.py analyze --base 614e7ff4`, Lizard 1.23.0; `module_registry.c` and `.h` are byte-identical between `614e7ff4` and Slice 2's `74d9376d`, so Slice 2's figures are this slice's baseline): `module_registry.c` 1,322 to 1,386 code lines, 67 to 71 functions; **`execute_phase` CCN 17 to 16** (below its baseline), `dispatch_events_range` 14 to 12, `module_system_init` (6), `free_phase_configuration` (16), `module_system_cleanup` (4) and the begin/end pair (2, 2) unchanged in CCN; four new helpers, `build_phase_plan_visitor` 6, `find_phase_plan` 4, `collect_phase_mode` 3, `release_phase_plans` 2. **The file's CCN sum rises from 351 to 363** (+12: +15 in the new helpers, −3 in the two dispatch functions); the maximum stays 17 (`module_emit_event`). Duplicate windows touching the file fall from 10 signatures to 9: the removed one is the `execute_phase` unresolved-entry block. No new include, dependency or file. **Justification of the rise:** the +12 is startup-only code (building, finding and releasing the plans), run once per initialisation and once per `execute_phase` call for the lookup, against a measured saving over the guards alone of 2.94% (vertical) and 2.24% (horizontal) at 7 to 11 SE, and of 4.6% / 4.3% over `before_head`; the hot path itself lost complexity (CCN −3) and its duplicated block. Verdict: a 115-line net change to one translation unit takes about 4.5% off the default run on both drivers at 17 to 22 SE, removes every per-call mode test and the duplicated unresolved-entry block, and keeps the output byte-identical, at the price of a small read-only startup table with one owner and one release path.

**Findings.** (1) The guards alone measured −1.68% (V) and −2.15% (H); the plan's extra saving over them (−2.94% / −2.24%) is the per-call mode re-scan of PASS 1 and PASS 2 that the guards leave in place. (2) The plan lookup is now a visible line (1.31%): a linear search over at most three keys on the default run, measured inside the totals above; Slice 5 measures what remains. (3) `execute_phase` now raises `FATAL_ERROR` for a non-empty phase configuration it has no plan for, including when called with an empty workspace or a NULL context, where it previously returned; every caller in the tree passes a phase that has been through `module_system_init()`, and no test needed a change. (4) The supervised environment's `python3` lacks PyYAML; the fuzzer must be run with `mimic_venv/bin/python3`. (5) Scratch worktrees `output/perf-slice4-r6/*` (registered with `git worktree`, not committed) and evidence under `archive/performance/slice-004-r6/` are kept, machine-local.

---

## Slice 5: Dispatch residual (no-go)

**Verdict: no-go.** The per-callback bookkeeping in `module_registry.c` that the plan names as removable is below the 2% threshold on the default run (measured 2026-10-21). No source file changed; this section is the slice's only tracked change. Differential `lint` (verdict `pass`), `make check-docs` and `make check-format` passed on it.

### Profile at `before_head` (measured, 20 repeats, `attribute.py --skip-first`, vertical)

`before_head` is `75e2a9bb`, whose `src/`, `models/`, `simulations/`, `scripts/` and `Makefile` equal Slice 4's code commit `0b76a45b`. The default build was rebuilt in the main tree (`make -j16`, exit 0; the binary carries the plan's `FATAL_ERROR` string), `nm_index.py` refreshed, then `run_wall.py` 15 repeats (repeats 1 to 14: 2.3491 ± 0.0467 s, min 2.3180, max 2.5079) and `sample_runs.sh` 20 repeats (19 reports, 33,311 self-samples), attributed with `--wall-mean-s 2.3491`; `source_attribution.mapped_fraction` 1.0, `Unattributed` 0.02%.

| Component | Share |
|---|---|
| Physics modules | 51.76% ± 0.39 |
| Core execution, of which `module_registry.c` | 30.61% ± 0.30, 25.27% ± 0.28 (594 ms equivalent) |
| Math library | 5.15% |
| Output I/O | 4.69% |
| Runtime/system | 3.60% |
| Utilities, of which `numeric.c` | 3.16%, 1.59% |
| Vertical input I/O | 0.81% |

`module_registry.c` is 25.27% against Slice 4's after-profile 25.79% (`archive/performance/slice-004-r6/agg.json`), the same code within the sampling error. Its hottest lines: the by-galaxy `process()` call (1715) 3.92%, the by-galaxy list loop (1707) 2.58%, the function entry (1648) 1.60%, the full-halo list loop (1667) 1.55%, the function exit (1725) 1.51%, the plan lookup (1653) 1.23%, the by-galaxy `DEBUG_LOG` level test (1710) 1.21%; `execute_module_pipeline`'s phase loop (1810, 1811, 1817) is 1.62% together. Evidence: `archive/performance/slice-005/agg.json`, the per-line decomposition `registry_lines.out` and the caller attribution of `numeric.c` `narrow_callers.out` (machine-local).

### The 2% threshold applied to the named lines

Line numbers are `before_head`'s. Each share is self-time as a percentage of the run's self-samples. `narrow_int64_to_int_checked()` is a cross-unit call into `src/util/numeric.c`, so its own time is attributed there and is added back to its caller line below. The objdump of `execute_phase` (`archive/performance/slice-005/execute_phase.dis`) names the instructions on each line.

| Named candidate | Lines | Share on those lines | Removable without contract change |
|---|---|---|---|
| `workspace_callback_count()` per full-halo module | 1670 | 0.102%, plus 0.862% in `narrow_int64_to_int_checked` called from 1670: **0.964%** | All of it: the count is already narrowed into `phase_event_state.ngal` once per phase (1664) and after each commit (`commit_created_records`). The per-phase call at 1664 (0.985% + 0.357%, shared with the inlined `begin_phase_event_dispatch`) is once per phase and not a candidate |
| `record_creation.ctx` / `.ws` stores per module | 1678, 1679 (one `stp`) | **0.000%** (no sample) | All of it, for no measurable gain |
| `enter_callback` / `leave_callback` per callback | 1680 and 1682 (full-halo), 1714 and 1716 (by-galaxy) | 0.925% + 0.000% + 0.648% + 0.934% = 2.507% | Only the save of the prior state (one `ldp` per callback, inside 1680 and 1714) and its restore (one `stp`, the whole of 1682 and 1716). The other instructions on 1680 and 1714 load the module's name and store the kind and the name, which `module_registry_running_callback()` must report during every callback. 1716's `stp` is the first instruction after the indirect call returns, so its samples may include the return's own cost, which no layout removes; that attribution is uncertain (the identical post-return `stp` in PASS 1, line 1682, has no sample, so it is likely a sampler artefact), and the decision rests on the counterfactual below, not on it |

**Sum.** With the save counted as its line's instruction share (one of five on 1680 and on 1714) and 1716's post-return samples as return cost: 0.964 + 0 + 0.185 + 0.130 = **1.28%**. Counting 1716 in full as removable: **2.21%**. The profile alone therefore brackets the threshold, so the decision was settled by measuring the removal itself.

### Counterfactual: every named removal at its maximum (measured, interleaved)

A scratch build of `before_head` with all three candidates removed as far as possible (`archive/performance/slice-005/counterfactual-max-removal.patch`, machine-local): `ngal` read from `phase_event_state.ngal`; the two `record_creation` stores once per phase; no per-callback save or restore of the running-callback state in either pass, the kind and name still stored before every callback and the outer state restored once at the end of `execute_phase`. This goes beyond what the contract keeps (the header documents `RUNNING_CALLBACK_NONE` outside any callback, and this leaves the last callback's state set between callbacks), so it bounds the saving from above. Both builds are in scratch worktrees `output/perf-slice5/{base,cut}-v` at `75e2a9bb` (`make -j16 MODEL=sage16 SIMULATION=mini-millennium`, 0 warnings each; the two `module_registry.o` differ, and the cut `execute_phase` makes one call fewer). Harness `archive/performance/slice-005/s5-counterfactual.py`: the repository root as the working directory, binaries by absolute path with `-q`, round 0 a discarded warm-up, the starting build alternating each round; every run exited 0; nothing else ran.

| Session | `before_head` | Maximal removal | Difference |
|---|---|---|---|
| 1 (n = 40) | 2.3526 ± 0.0528 s (min 2.3117, max 2.5599) | 2.2973 ± 0.0215 s (min 2.2687, max 2.4058) | −0.0553 s, −2.35%, SE 0.0090 (−6.1 SE) |
| 2 (n = 60) | 2.3372 ± 0.0127 s (min 2.3145, max 2.3920) | 2.3025 ± 0.0306 s (min 2.2774, max 2.4521) | −0.0346 s, −1.48%, SE 0.0043 (−8.1 SE) |
| Pooled (n = 100) | 2.3433 ± 0.0354 s | 2.3005 ± 0.0273 s | **−0.0429 s, −1.83%, SE 0.0045 (0.19%)**; medians −1.77% |

Session 1's mean is lifted by four slow `before_head` runs (2.456 to 2.560 s); its paired median difference is −1.86%.

**Decision.** The removable bookkeeping is 1.28% by the profile's instruction-level reading, and every named removal taken to its maximum, beyond what the contract allows, measures 1.83% ± 0.19% (pooled) and 1.77% by medians. Both are below 2%, so the slice closes with no code change, as the plan's rule prescribes. The saving is real (about 10 SE) but small; a later plan that wants it would take the narrowing hoist (about 1%, contract-neutral) on its own.

**Cost.** `git diff --stat 75e2a9bb..HEAD`: this file only. No source, test, generated or build file changed, so no byte gate, suite, converter or distributed run is required (the plan's criterion 3 applies only when code changes). Scratch: the two worktrees `output/perf-slice5/{base,cut}-v` (registered with `git worktree`, not committed) and evidence under `archive/performance/slice-005/` (machine-local). The main tree's default build is the unchanged `before_head` source.

**Findings.** (1) The dispatch residual is now dominated by the indirect `process()` calls and the loops that drive them (lines 1715, 1707, 1667: 8.05%), not by bookkeeping; what remains removable without changing the dispatch contract is under 2% of the run. (2) The one candidate with a clean, contract-neutral gain is the per-module `workspace_callback_count()` in PASS 1 (0.96% by profile), which duplicates the count `begin_phase_event_dispatch` and `commit_created_records` already keep. (3) Sessions differ: the same pair measured −2.35% and −1.48% in consecutive sessions, so a threshold decision near 2% needs more than one session.

---

## Slice 6: The vertical output buffer's seed and zero-fill

**Status: done, under delegated plan decision D4.** The slice first stopped on its profile criterion's `__bzero` bound (under 0.5% of the vertical run), which the contract makes unreachable because the FoF-workspace zero-fill it keeps costs 0.93% on its own; PM resolved it by D4 (under Findings) with no change to the code. Measured 2026-10-22, `before_head` `75b63649`; the code commit is `da2ba01c`, and this record follows in a later commit.

**What changed.** `output_buffer_seed_capacity()`, a `static inline` function in `src/core/output_buffer.h`, is the seed rule both drivers call: the input halos plus `OUTPUT_SEED_HEADROOM` (5%, the renamed `HORIZONTAL_OUTPUT_SEED_HEADROOM`) of them, the headroom floored at `MIN_HALO_ARRAY_GROWTH`, clamped at `MAX_HALO_ARRAY_SIZE` unless the input is already past it, when only the minimum increment is added; 0 on `int64_t` overflow, tested inline with the arithmetic of `horizontal_checked_sum`. It is `horizontal_output_seed_capacity()` moved and renamed; the horizontal driver's footprint calls it in its place and keeps `horizontal_checked_sum` for its other terms. `load_unit()` seeds `ProcessedHalos` by it, with the overflow return a `FATAL_ERROR` naming the tree and its halo count, no longer zero-fills it (a comment at the allocation names the growth path and the bounded readers), and seeds the FoF workspace by its own rule, `InputTreeNHalos / 2` floored at `INITIAL_FOF_HALOS` and clamped at `MAX_HALO_ARRAY_SIZE`, with its zero-fill unchanged. `MAXHALOFAC` is deleted. The FoF rule equals the former one (a tenth of the floored five-fold output seed, through `double`) for every non-negative `int` halo count: an exhaustive scratch comparison over 0 to 2,147,483,647 found no difference.

**Gate result** (2026-10-22, `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` at `da2ba01c`). Exit 0; eleven markers all `PASS`, of which exactly eight are `test_leg_` markers (`halos-only` and `sage16`, vertical and horizontal, fixed and dynamic); no FAIL, ERROR or SKIP; `HEAD build flags: none`; all eight legs passed in 37 s. Evidence: `archive/reference-identity/20261021T220159Z-ref5daa24ee-headda2ba01c/evidence.json` (machine-local).

**Other validation** (the code's final source before the commit, `TMPDIR` outside the repository): `make tests summary` exit 0 (unit 603 passed, 27 n/a; integration 257 passed, 12 n/a; scientific 47 passed, one `WARN test_zero_values`); `make tests-converter` exit 0 (1655 passed); `make tests-distributed` exit 0 (264 checks, no skips), then `make` exit 0; `make check-docs`, `make check-format` and the differential `lint` (verdict `pass`) are clean. `rg -n "MAXHALOFAC|HORIZONTAL_OUTPUT_SEED_HEADROOM" .` outside `docs/dev/` and `archive/` finds only the changelog line that records the removal.

### Memory profile and FoF growth (measured, default runs)

| | `before_head` | After |
|---|---|---|
| Vertical `C` / `P` | 74,345 / 14,648 | **15,869** / 14,648 |
| Vertical peak RSS | 0.071 GB | 0.037 GB |
| Vertical `Growing FoF workspace` lines | 0 | 0 |
| Horizontal `C` / `P` | 39,694 / 37,425 | 39,694 / 37,425 |

`C` after is the largest tree's seed (14,869 halos plus the 1,000-record floor), so no tree's output outgrew its seed; galaxy pool, `R` and the retention pool are unchanged. Galaxy output of the before and after binaries compared with `scripts/compare_cross_format_identity.py --compare-created`: 187,832 galaxies over 8 snapshots, all 42 fields byte-identical, under both drivers.

### Wall-time pair (measured, n = 14, repeats 1 to 14 of 15, the plan's protocol from the repository root)

`before` and `after` are default builds (`SIMULATION=mini-millennium` and `mini-millennium-horizontal`) of `before_head` and of the slice, each copied to `./mimic` in turn and timed with `run_wall.py` back to back, vertical before and after, then horizontal before and after; nothing else ran.

| | Vertical | Horizontal |
|---|---|---|
| Before | 2.4004 ± 0.1202 s (min 2.3313, max 2.7963) | 2.4553 ± 0.0205 s (min 2.4263, max 2.5023) |
| After | 2.3056 ± 0.0278 s (min 2.2641, max 2.3646) | 2.4393 ± 0.0155 s (min 2.4127, max 2.4707) |
| Difference of means | −0.0948 s, −3.95% | −0.0160 s, −0.65% |
| SE of the difference | 0.0330 s (−2.9 SE) | 0.0069 s (−2.3 SE) |

Every repeat exited 0, and the no-regression bound holds on both drivers. The vertical before mean carries one slow repeat (2.7963 s); without it the before mean is 2.3700 s and the saving 2.7%, in line with the 1.29% of `__bzero` removed plus the page faults of the untouched four-fifths of the old seed. The horizontal driver's seed arithmetic is unchanged, so its −0.65% is session drift or code layout, not attributed to the slice.

### Profile after (measured, 20 repeats, `attribute.py --skip-first`, vertical)

`sample_runs.sh` 20 repeats of the after build (19 reports, 33,463 self-samples), `--wall-mean-s 2.3056`; `source_attribution.mapped_fraction` 1.0, `Unattributed` 0.02%. **`__bzero` is 0.97% ± 0.05** (324 samples), against 2.29% (762) in Slice 5's profile of the same `before_head` source (`archive/performance/slice-005/agg.json`). By the parent frame of each `__bzero` sample:

| Caller | `before_head` | After |
|---|---|---|
| `load_unit`, `ProcessedHalos` `memset` | 430 samples, 1.29% | gone |
| `load_unit`, FoF-workspace `memset` | 323 samples, 0.97% | 312 samples, 0.93% |
| libhdf5 | 9 samples | 12 samples |

**Finding: the plan's `__bzero` figure was not all the output buffer's.** The baseline's 2.14% (2.29% at `before_head`) was split 1.29 / 0.97 between the two zero-fills, so removing the output buffer's leaves the FoF workspace's 0.93%, which the slice must keep (non-goal: no change to the FoF workspace's seed value or zero-fill). The criterion's literal bound of 0.5% therefore cannot be met inside the contract; D4 below resolves it. The FoF zero-fill costs nearly as much as the output one, although it covers a tenth of the bytes, because it touches fresh pages each tree; the measurement does not separate page-fault cost from the fill itself. Evidence: `archive/performance/slice-006/` (machine-local: `agg.json`, the `samples/`, the wall JSON, the before/after binaries and outputs, `compare-{v,h}.out`).

**Cost.** `git diff --numstat` against `75b63649`: `output_buffer.h` +37, `horizontal_driver.c` +8 −30, `interface.c` +22 −14, `constants.h` +6 −10, the test +1 −1, documentation +9 −8; 74 code lines added and 55 removed. Differential `code-health` (`health.py analyze --base 75b63649`, Lizard 1.23.0; CCN sums by `lizard` per file): `horizontal_driver.c` 270 to 266 (the seed function, CCN 4, leaves it), `output_buffer.h` 0 to 5 (`output_buffer_seed_capacity`, its overflow test now inline), `interface.c` 19 to 19 (`load_unit` unchanged in CCN); code lines `horizontal_driver.c` −15, `output_buffer.h` +20, `interface.c` −1, `constants.h` −1. No new include cycle; `interface.c` gains one include of `output_buffer.h`. The health tool also lists two `horizontal_driver.c` functions at lines 1238 and 1283 as cyclomatic deltas; `lizard` by function name shows neither changed, so they are line-shift artefacts of the tool's matching. Verdict: a net 19-line change replaces two seed rules with one, halves the vertical run's peak RSS on mini-Millennium (the seed falls to 21% of the old one on trees of 20,000 halos or more; on trees under 250 halos it is up to 250 records larger, the 1,000-record floor now being added to the halos rather than replacing them), removes the larger of the two zero-fills, and keeps the output byte-identical; the literal profile bound is unreachable while the FoF zero-fill stays (D4).

**Findings.** (1) **Delegated plan decision D4** (PM, under the owner's delegated authority), recorded verbatim: "Criterion 6's '< 0.5% to \_\_bzero (the FoF-workspace zero-fill remains)' was written against the frozen fact that the baseline \_\_bzero was the output-buffer seed's zero-fill. Measured at before_head it is split 1.29% (ProcessedHalos memset) and 0.97% (the FoF-workspace memset the contract keeps unchanged), so the literal bound cannot be met inside the contract. PM ruled that the bound applies to the zero-fill this slice removes: the ProcessedHalos memset contributes no \_\_bzero samples after the slice (it no longer exists), and total \_\_bzero fell from 2.29% to 0.97%, all of the remainder being the kept FoF zero-fill. Recommended amendment: 'the profile attributes no \_\_bzero to the ProcessedHalos allocation; total \_\_bzero is recorded before and after.'" Under D4 the criterion is met: no `__bzero` sample has `load_unit`'s `ProcessedHalos` allocation as its parent after the slice, and total `__bzero` is 2.29% before and 0.97% after. (2) The FoF-workspace zero-fill (0.93%) is the remaining `__bzero` cost of the vertical run; changing it is outside this plan. (3) Renaming the shared function also updated one comment's identifier inside `horizontal_acquire_generation()`, which named the old function. (4) A comment-only edit to `interface.c` landed while `make tests` was running; the converter and distributed suites, the format check and lint ran after it, and the byte gate ran at the commit.

---

## Slice 8: Build flags, measured and gated

**Status: done, no flag set adopted.** Measured 2026-10-22 at `before_head` `3be425e3`; the code commit is `ccfc6113` (the benchmark script's `EXTRA_LDFLAGS` pass-through and the documentation), and this record follows in a later commit. The default `CFLAGS` is unchanged, `-g -O2` plus the warning set, and `LDFLAGS` is unchanged; the Makefile is untouched.

**Method.** Six sets, as designed under [Build flags](MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md#build-flags-slice-8): `-fno-math-errno`; `-flto` (compiler and linker); `-O3`; `-fno-math-errno -flto`; `-fno-math-errno -O3`; `-fno-math-errno -flto -O3`. For each set and each driver the default build was made and timed, then the set's build was made and timed, back to back: `make clean && make -j32 SIMULATION=<package> EXTRA_CFLAGS=... EXTRA_LDFLAGS=...` in the main tree (builds take 1 to 3 s; all 24 compiled with 0 warnings), then `python3 scripts/profiling/run_wall.py <run file> 15 <label>` from the repository root, `sage16` on mini-Millennium (`SIMULATION=mini-millennium` and `mini-millennium-horizontal`); mean, σ, min and max over repeats 1 to 14, every repeat exiting 0. Each set was then gated with `REFERENCE_IDENTITY_HEAD_EXTRA_CFLAGS=... REFERENCE_IDENTITY_HEAD_EXTRA_LDFLAGS=... make tests-reference-identity REFERENCE_COMMIT=5daa24ee` at the clean tree (each log records `HEAD build flags: EXTRA_CFLAGS='...'`). Scripts, wall JSON, builds' logs and gate logs: `archive/performance/slice-008/` (machine-local). The machine's load average was 9.9 when the session started, from other users of the machine that this run could not control; the wide σ of some runs (s1 horizontal after, s6 horizontal both sides) is consistent with interference, and a result close to a threshold would need a repeat session.

### Wall-time pairs (measured, n = 14, repeats 1 to 14 of 15, the plan's protocol from the repository root)

Difference of means after minus before (negative is faster) and its SE (the two σ in quadrature over √n).

| Set | Driver | Before | After | Difference | SE | Gate (exit, legs) |
|---|---|---|---|---|---|---|
| `-fno-math-errno` | V | 2.3624 ± 0.0330 s (2.2902 to 2.4075) | 2.3351 ± 0.0669 s (2.2787 to 2.4843) | −0.0273 s, −1.16% | 0.0199 s (−1.4 SE) | exit 0, 8 of 8 |
| | H | 2.4351 ± 0.0227 s (2.4071 to 2.4783) | 2.5913 ± 0.2182 s (2.4005 to 3.1438) | +0.1562 s, +6.41% | 0.0586 s (+2.7 SE) | |
| `-flto` | V | 2.3641 ± 0.0400 s (2.2910 to 2.4176) | 2.4066 ± 0.1000 s (2.3128 to 2.6644) | +0.0425 s, +1.80% | 0.0288 s (+1.5 SE) | exit 0, 8 of 8 |
| | H | 2.4752 ± 0.0379 s (2.3862 to 2.5234) | 2.4725 ± 0.0376 s (2.4034 to 2.5209) | −0.0027 s, −0.11% | 0.0143 s (−0.2 SE) | |
| `-O3` | V | 2.3692 ± 0.0610 s (2.2688 to 2.4712) | 2.3457 ± 0.0761 s (2.2522 to 2.5786) | −0.0235 s, −0.99% | 0.0261 s (−0.9 SE) | exit 0, 8 of 8 |
| | H | 2.4563 ± 0.0385 s (2.3881 to 2.5129) | 2.4105 ± 0.0283 s (2.3799 to 2.4637) | −0.0458 s, −1.86% | 0.0128 s (−3.6 SE) | |
| `-fno-math-errno -flto` | V | 2.3558 ± 0.0431 s (2.2808 to 2.4388) | 2.3188 ± 0.0437 s (2.2741 to 2.4179) | −0.0370 s, −1.57% | 0.0164 s (−2.3 SE) | exit 0, 8 of 8 |
| | H | 2.4429 ± 0.0399 s (2.3758 to 2.5244) | 2.4770 ± 0.0290 s (2.4444 to 2.5339) | +0.0341 s, +1.39% | 0.0132 s (+2.6 SE) | |
| `-fno-math-errno -O3` | V | 2.3544 ± 0.0569 s (2.2855 to 2.5262) | 2.2887 ± 0.0331 s (2.2455 to 2.3492) | −0.0657 s, −2.79% | 0.0176 s (−3.7 SE) | exit 0, 8 of 8 |
| | H | 2.4592 ± 0.0477 s (2.4013 to 2.5866) | 2.4412 ± 0.0426 s (2.3717 to 2.5365) | −0.0180 s, −0.73% | 0.0171 s (−1.1 SE) | |
| `-fno-math-errno -flto -O3` | V | 2.3501 ± 0.0432 s (2.3003 to 2.4516) | 2.3386 ± 0.0416 s (2.2625 to 2.4240) | −0.0115 s, −0.49% | 0.0160 s (−0.7 SE) | exit 0, 8 of 8 |
| | H | 2.5382 ± 0.0916 s (2.4422 to 2.8277) | 2.5317 ± 0.1087 s (2.4241 to 2.8476) | −0.0065 s, −0.26% | 0.0380 s (−0.2 SE) | |

The gate column applies to the set (one run per set; both drivers' legs are in it): each log shows `HEAD build flags` as named and `all 8 legs passed`, with eight `PASS test_leg_` markers and `Failed: 0`.

### Decision under the plan's rule

A set is adopted only if its gate passed **and** its after mean is more than 2 SE below the before mean on **both** drivers. All six gates passed; no set meets the timing half. The closest are `-fno-math-errno -O3` (vertical −3.7 SE, horizontal −1.1 SE) and `-O3` (vertical −0.9 SE, horizontal −3.6 SE): each is resolved on one driver and unresolved on the other, so each fails the rule, which is asymmetric by design. `-fno-math-errno` alone and `-fno-math-errno -flto` are slower on the horizontal driver (+2.7 and +2.6 SE); `-flto` and the three-flag set are unresolved on both drivers. No set is therefore committed, and criterion 2's second run (the adopted set gated again from the committed `CFLAGS`) has no adopted set to run on. The default build was gated and tested as committed instead: see below.

Reasons recorded in the changelog: `-fno-math-errno` no resolved vertical saving and a slower horizontal mean; `-flto` no saving; `-O3` no resolved vertical saving; `-fno-math-errno -flto` a slower horizontal mean; `-fno-math-errno -O3` no resolved horizontal saving; `-fno-math-errno -flto -O3` no saving. The saving the sets show on one driver is small beside the 1.6% run-to-run noise the plan quotes, and a different session could resolve or lose it; a reader who wants `-O3` or `-fno-math-errno -O3` should repeat the pair, with the Linux caveat below. Slice 3's `DEBUG_LOG` level test already removes the cross-translation-unit call that LTO would have bought, which is a likely reason `-flto` finds nothing more.

**Linux and FMA caveat.** Bit-identity is proven on macOS arm64 Clang only. GCC's default `-ffp-contract=fast` can fuse multiply-add across inlining on targets with FMA, and CI's `ubuntu-latest` x86-64 default target has none, so a green CI run is not proof for an aarch64 Linux host. Nothing is adopted, so the default build's identity is unchanged.

### Default build as committed

At `ccfc6113`, default build, no extra flags, `TMPDIR` outside the repository: `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` exit 0 (`HEAD build flags: none`, eight `PASS test_leg_` markers, all 8 legs passed in 37 s; evidence `archive/reference-identity/20261021T224139Z-ref5daa24ee-headccfc6113/evidence.json`, machine-local); `make tests summary` exit 0 (unit 603 passed, 27 n/a; integration 257 passed, 12 n/a; scientific 47 passed, one `WARN test_zero_values`); `make tests-converter` exit 0 (1655 passed); `make check-generated`, `make check-format` and `make check-docs` pass; the differential `lint` verdict is `pass` (shellcheck's three findings pre-exist). `tests/unit/run_tests.sh` is untouched and still builds unit tests at `-O0` (line 117). `make info` prints `-g -O2 -Wall -Wextra -Wshadow -Wformat-security -Wundef` and no `-flto` or `-march`. A functional run of `scripts/benchmark_mimic.sh` with `EXTRA_CFLAGS=-flto EXTRA_LDFLAGS=-flto` linked with `-flto`, printed `Extra LDFLAGS: -flto` and wrote `"extra_ldflags": "-flto"` to the result JSON.

**Cost.** `git diff --numstat 3be425e3..dbe8ff0e` outside `docs/dev/`: `scripts/benchmark_mimic.sh` +27 −4, `scripts/profiling/README.md` +1 −1, `docs/USER-GUIDE.md` +2, `CHANGELOG.md` +2, the two skills +1 −1 each; 34 lines added and 7 removed, no source, test or Makefile line. Of these, the PM correction commit `dbe8ff0e` (after the drift audit and two code reviews) is +18 −4 (excluding the record): a `json_escape` helper so `extra_cflags` and `extra_ldflags` stay valid JSON for a flag with quotes or a backslash, probed with `json.loads` on unset, simple, multiple, quote-and-backslash and newline-and-tab values; the build skill's unit-test sentence (the unit-test build appends `EXTRA_CFLAGS`/`EXTRA_LDFLAGS`); and the outlier wording below. The gate (exit 0, `HEAD build flags: none`, 8 of 8), `make tests summary` (603/257/47) and `make tests-converter` (1655) were re-run at `dbe8ff0e`, exit 0 each. Wall pairs: the table above. Verdict: the slice buys a script option and a recorded, repeatable measurement of six flag sets and adopts nothing, because no set resolved a saving on both drivers; the default build every user and CI builds is unchanged.

**Findings.** (1) Builds take 1 to 3 s on this machine, so a repeat session of the two near-miss sets is cheap. (2) The `-fno-math-errno` horizontal result (+6.41%) rests on after runs with two elevated repeats within repeats 1 to 14 (3.035 s and 3.144 s; the minimum, 2.4005 s, is below the before minimum), the two elevated repeats inflate the figure, but the other twelve after-runs still average 2.5083 s against the before mean of 2.4351 s (about +3%), so the horizontal result is not explained away by the outliers; the set fails the rule on the vertical driver in any case. (3) The compile and exec markers (`build/.last_compile_mode`, `build/.last_exec_mode`) already track `EXTRA_CFLAGS` and `EXTRA_LDFLAGS`, so an extra-flag build after a default build recompiles and relinks without `make clean`; the sets were built from a clean tree anyway.
