# Mimic Coupled Rate: Acceptance Record (Step 7, Stage 1)

**Status:** Evidence of record, written by Slice 12 of [`MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`](MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md) against the gate of the brief ([`MIMIC-COUPLED-RATE-FORMULATION-PLAN.md`](MIMIC-COUPLED-RATE-FORMULATION-PLAN.md#gate)). It states what was run, what it returned, and what did not pass. It loosens nothing: no tolerance, baseline, test or threshold was changed to obtain any figure below.

**Result in one paragraph.** Every command the Slice 12 criteria name exited 0, and both cost bounds held on both schemes. Gate items 1 to 6 have evidence below. Gate item 7 (the documents of record) is not met by this record: it is Slice 13's, and the documents still lag the branch as the plan's decision 11 says they do until then. Nothing failed; the caveats are listed under [What did not pass, and caveats](#what-did-not-pass-and-caveats).

## What was measured

| Item | Value |
|---|---|
| Branch | `feature/coupled-rate-mode` |
| Commit measured | `2d34a8e24aa9da7fabb542abdae2f3a8029de2ee` (the cost driver and the battery's `--cost` option on top of `544e83c4`, the end of Slice 11). Slice 12 changes no runtime source and no `Makefile`: `git diff 544e83c4 2d34a8e2 --stat` names `tests/manual/rate_cost.c` and `tests/manual/run_rate_battery.py` only. The tree was clean for every run. |
| Reference commit for the byte gate | `39a8d28e` (decision 17) |
| Machine | Apple M3 Ultra, 32 cores, 512 GB, macOS 27.2.1 (Darwin 27.2.0, arm64) |
| Compiler | Apple clang 21.0.0 (clang-2100.4.1.1), the Makefile's `-g -O2 -Wall -Wextra -Wshadow -Wformat-security -Wundef`; the cost driver is compiled `-O2` with the same warnings |
| MPI | Open MPI 5.0.9, launched as plain `mpirun` (no `--oversubscribe`) |
| Python | `mimic_venv` (system `python3` lacks `h5py`; the battery imports it at module level, so `--cost` also needs `mimic_venv/bin` first on `PATH`) |
| Environment | `MIMIC_BASELINE_RTOL` unset for every run (baselines strict); default `TMPDIR`; real mini-Millennium data under `/Volumes/Internal/data/millennium/` in both layouts |
| Logs | `archive/test-logs/slice12/` (gitignored), with the driver scripts `gates.sh`, `wall.sh` and `make_wall_configs.py` that ran the commands below, one at a time |

Timing conditions: nothing of this slice's ran beside a timing. The machine was not otherwise idle: macOS background services (Microsoft Defender, CloudKit, Photos) were at a few percent of one core each throughout, and load average was 2.8 to 4. The timings are therefore as measured on an ordinary desktop, not on a quiet one.

## The seven gate items

| # | Gate item | Evidence | Where it lives |
|---|---|---|---|
| 1 | Every fixture problem meets its criterion | Hand-built problems: unit tests `test_rate_integrator` (chain, cycle, three sinks, source-sink, stiff pair, bimolecular, threshold, two intervals, star, failures), `test_rate_tracers` (dilution, yield, empty donor, tracer bound) and `test_rate_implicit` (block solve against a dense solve, extreme rates, stiff pair flat in step count, bimolecular first order, linear chain, yield). Real dispatch: `test_rate_dispatch`, `tests/integration/test_rate_phase.py`, the battery. All passed in the unit and integration tiers of each of the five `make tests` runs below (`test_rate_dispatch`, `test_rate_implicit`, `test_rate_integrator`, `test_rate_mode_contract`, `test_rate_tracers`, `test_rate_transfer_tables` each `PASSED`) | `tests/unit/test_rate_*.c`, `tests/integration/test_rate_*.py`; logs `tests-*.log` |
| 2 | Permutation, driver, task-count and chunk-count identity | `make tests-rate`: 467 checks over `cross-driver` 3 cases, `task-chunk` 10, `permutation` 46 (24 orders under each scheme, each against the first), `record-creation` 5; every leg first asserted non-zero fixture properties; no skips. Cross-driver identity is per `UniqueGalaxyID` on the real mini-Millennium data (tree rows only, plan decision 14), with identical step statistics and identical FoF row order | `tests/manual/run_rate_battery.py`; `tests-rate.log`, `build/rate_tests.log` |
| 3 | The ledger closes and a planted imbalance is detected | Segment ledger: `test_closure_check_detects_imbalance` (a planted 1e-6 relative imbalance, the closure check called on its own), the closed-cycle, source-sink, many-external-flux and bimolecular cases (total within `tol_F`; the bimolecular closure ratios were 3.7e-3 to 2.1e-4 of `tol_F`). Discrete-phase ledger: `test_undeclared_change_is_fatal` and `test_undeclared_change_beside_a_declaring_module` (an undeclared change to a conserved total is fatal), `test_lossy_jump_is_counted`, `test_lossless_jump_records_no_change`, and the `test_ledger_*` unit cases. All passed | `tests/unit/test_rate_integrator.c`, `tests/unit/test_rate_dispatch.c`, `tests/integration/test_rate_phase.py` |
| 4 | Failures exit non-zero, name the cause and commit nothing | Unit: `test_failure_causes`, `test_rejection_budget`, `test_failures_leave_state_unchanged` (each failing segment returns its cause and leaves the input state bitwise unchanged). Dispatch, unit (`tests/unit/test_rate_dispatch.c`): `test_nan_law_is_fatal`, `test_failure_file_holds_the_segment`, `test_purity_probe_catches_impure_laws`. Run, integration (`tests/integration/test_rate_phase.py`): `test_failing_laws_write_a_failure_file` and `test_purity_probe_catches_the_impure_law` (non-zero exit, one-screen message naming the cause, `rate_failure_task<N>.txt`). All passed | same files |
| 5 | Every existing package and gate is green with the mode unused | Every command in [Commands and exit codes](#commands-and-exit-codes) exited 0: `make tests` for the five shipped-package runs, `generate` and `check-generated` for all 50 distinct pairs, the byte gate against `39a8d28e`, and `tests-distributed`, `tests-snapshot-global`, `tests-snapshot-global-identity`, `tests-horizontal-v3`, `tests-converter`, `tests-bench`. Strict baselines: `MIMIC_BASELINE_RTOL` was unset for the five `make tests` runs, covering the packages the byte gate does not (`sage26`, `hod`, `sham`) as well as `sage16` and `halos-only` | `archive/test-logs/slice12/summary.txt` |
| 6 | The cost of the mode on the fixture is measured | [Cost, integrator alone](#cost-integrator-alone) and [Cost, real dispatch](#cost-real-dispatch). No production figure is claimed | below; `cost.log`, `wall/` |
| 7 | The documents of record describe the mode | **Not met by this slice.** Slice 13 owns it; until it lands `docs/VISION.md`, the Developer Guide's processing narrative, `AGENTS.md`, `CHANGELOG.md` and the pathway lag the branch (decision 11). Per-key reference rows and owning-skill entries were written by the slices that added the keys | Slice 13 |

## Commands and exit codes

Run one at a time on `2d34a8e2`, each followed by `make generate && make` for the default pair (`sage16`, `mini-millennium`), which is not a gate and also exited 0 every time. Elapsed times are wall clock on the machine above, with warm caches.

| Command | Exit | Key lines |
|---|---|---|
| `make MODEL=sage16 SIMULATION=mini-millennium tests` | 0 | unit 53 passed; scientific passed=114 failed=0 skipped=0 n/a=0 warned=1; `ALL TESTS AND CHECKS PASSED`; 151 s |
| `make MODEL=sage26 SIMULATION=mini-millennium tests` | 0 | unit 217 passed; scientific passed=273 failed=0 skipped=0 warned=1; 193 s |
| `make MODEL=halos-only SIMULATION=mini-millennium tests` | 0 | scientific passed=46 failed=0 skipped=0 warned=1; 120 s |
| `make MODEL=hod SIMULATION=mini-millennium tests` | 0 | scientific passed=46 failed=0 skipped=0 warned=1; 119 s |
| `make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal tests` | 0 | scientific passed=56 failed=0 skipped=0 warned=1; 3,112,152 galaxies bitwise identical in 42 fields across 8 snapshots in its cross-format gate; 468 s |
| `make MODEL=<m> SIMULATION=<s> generate && make MODEL=<m> SIMULATION=<s> check-generated`, production mode, for each of the 50 distinct pairs of the 51 run files under `models/*/input/` | 50 of 50 exit 0 | pair list `pairs.txt`, per-pair exits `check-generated-pairs.exits`; the default pair restored afterwards (exit 0) |
| `make tests-reference-identity REFERENCE_COMMIT=39a8d28e` | 0 | 11 passed, 0 failed; every leg's only schema difference is `metadata/output_schema.json: source_md5 build provenance only` (8 legs: `{halos-only, sage16} × {vertical, horizontal} × {fixed, dynamic}`), 0 galaxy differences, 0 errors; 38 s |
| `make tests-distributed` | 0 | 264 checks, 4 models at `-np 1, 2, 3, 4, 8`, chunked legs, chunked refusals; no skips; 34 s |
| `make tests-snapshot-global` | 0 | 115 cases, no skips; 38 s |
| `make tests-snapshot-global-identity` | 0 | all 8 legs passed; 77 s |
| `make tests-horizontal-v3` | 0 | 4 C tests, 3 Python tests, no unexpected skips or N/A; 19 s |
| `make tests-rate` | 0 | 467 checks; cross-driver 3/3, task-chunk 10/10, permutation 46/46, record-creation 5/5 cases; 68 s |
| `make tests-converter` | 0 | passed=1211 failed=0 skipped=0 n/a=0; 339 s |
| `make tests-bench` | 0 | passed=269 failed=0 skipped=0 n/a=0; 314 s |
| `PATH=$PWD/mimic_venv/bin:$PATH python3 tests/manual/run_rate_battery.py --cost` | 0 | every bound held; below |
| `scripts/profiling/run_wall.py <run.yaml> 5 <label>`, six configurations | 0 each | below |

The standing checks, run on the final tree of the slice: `./scripts/beautify.sh` exit 0; `make check-format` exit 0; `make check-docs` exit 0 (internal links and anchors resolve); `make generate && make check-generated` on the default pair (`sage16`, `mini-millennium`), production mode, exit 0 ("Generated code is up-to-date with property and module metadata"); the differential lint over the slice's files (`lint.py check --base 544e83c4`) exit 0, verdict `pass`; a search of `src`, `tests`, `docs` (outside `docs/dev`) and `.agents` for run-bookkeeping labels printed nothing, and nothing outside `docs/dev` refers to this record.

## Cost, integrator alone

`tests/manual/rate_cost.c`, compiled and run by `python3 tests/manual/run_rate_battery.py --cost` against an explicit source list (`src/core/rate_integrator.c`, `src/util/error.c`); no `Makefile` target. Hand-built uniform stars of the unit tests' shape: a central (`Δ` 1.25) with N satellites (`Δ` 1.1), two reservoirs per galaxy, satellite gas to the central's hot gas (cross-record, 1.5) and to its own stars (0.75), the central's hot gas to its cold gas (0.9), the central's content scaled with N so the dynamics in each entry's own scale do not depend on N. Constant coefficients, so a law evaluation is a copy and what is timed is the integrator. Production default settings (relative tolerance 1e-4, absolute 1e-7), adaptive, one total. Per size and scheme: one segment run and discarded, then seven timed repeats. The driver sizes a repeat from the discarded segment's duration: it runs as many back-to-back segments as would bring the integrator's time to 0.05 s at that first, cold, timing (at least one, at most 100,000), so a repeat can fall short of 0.05 s when the warm segments run faster than the cold one did. Each segment is timed with `CLOCK_MONOTONIC` around the integrator call alone, and the segments per repeat are in the driver's output. The table reports the nanoseconds per galaxy per law evaluation as the median over the seven repeats (the statistic the bounds are judged on, fixed before measuring) with the minimum and maximum. The accepted, rejected and law-evaluation counts were identical in every segment of every size (the driver checks this).

| Scheme | Satellites | Galaxies | Workspace B/galaxy | Accepted steps per segment | Rejected | Law evaluations | Median ns | Min ns | Max ns |
|---|---|---|---|---|---|---|---|---|---|
| explicit | 10 | 11 | 418.91 | 29 | 0 | 117 | 15.20 | 12.43 | 40.38 |
| explicit | 100 | 101 | 409.19 | 29 | 0 | 117 | 11.85 | 11.83 | 11.97 |
| explicit | 1,000 | 1,001 | 408.12 | 29 | 0 | 117 | 13.27 | 13.16 | 13.44 |
| explicit | 10,000 | 10,001 | 408.01 | 29 | 0 | 117 | 13.29 | 13.26 | 13.62 |
| implicit | 10 | 11 | 637.09 | 263 | 0 | 264 | 54.44 | 53.50 | 54.63 |
| implicit | 100 | 101 | 618.61 | 263 | 0 | 264 | 50.22 | 50.19 | 50.32 |
| implicit | 1,000 | 1,001 | 616.26 | 263 | 0 | 264 | 52.32 | 52.13 | 54.54 |
| implicit | 10,000 | 10,001 | 616.03 | 263 | 0 | 264 | 54.46 | 53.11 | 55.38 |

| Bound | Scheme | Measured | Limit | Result |
|---|---|---|---|---|
| Time per galaxy per law evaluation, 10,000 against 100 (median) | explicit | 1.121 (best case 1.121) | at most 1.5 | passed |
| Time per galaxy per law evaluation, 10,000 against 100 (median) | implicit | 1.084 (best case 1.058) | at most 1.5 | passed |
| Workspace bytes per galaxy, 10,000 against 1,000 | explicit | 0.9997 | within 10% | passed |
| Workspace bytes per galaxy, 10,000 against 1,000 | implicit | 0.9996 | within 10% | passed |

Reading the table: work is linear in galaxies for both schemes (the per-galaxy per-evaluation time rises by 12% and 8% over two decades of size, the rise coming between 100 and 1,000 satellites; its cause was not investigated). The 10-satellite explicit row's maximum (40 ns) is one outlying repeat of the 10-satellite explicit star, whose segment takes tens of microseconds, not a trend. The implicit scheme takes about nine times the accepted steps of the explicit scheme at equal tolerances (263 against 29; first order against third) and, per law evaluation, about four times the integrator time, so its segment costs about nine times the explicit one's here (263 × 54 ns against 117 × 13 ns per galaxy); the scheme is never the default. The step counts are a property of this star at the default tolerances, not of the fixture's production use. Two earlier runs of the same driver (before the gates; not the table above) gave median ratios of 1.088 and 1.121 (explicit) and 1.029 and 1.040 (implicit), so across three runs the ratios spread by about 0.03 (explicit) and 0.06 (implicit).

## Cost, real dispatch

The rate fixture pipeline of the battery on the real mini-Millennium data, serial, under both drivers, against the empty `halos-only` pipeline of the same run file. The rate pipeline is `test_rate_supply`, `test_rate_formation`, `test_rate_feedback`, `test_rate_stripping` in one rate phase and `test_rate_jump` in a discrete phase after it, with the parameters of `forest_blocks_rate.yaml`, under `scheme: explicit` and under `scheme: implicit`, fixed time-stepping, HDF5 output, snapshot list 63, 37, 32, 27, 23, 20, 18, 16. The run files were produced from the committed `models/halos-only/input/halos-only_mini-millennium{,-horizontal}.yaml` by `make_wall_configs.py` (the battery's own `real_config` and `rate_config`, with only `output.output_directory` changed). All six configurations ran on one test-build binary per layout (`MODEL=halos-only TEST_BUILD=yes USE-MPI=`, built from the committed tree), because the fixtures exist in test builds only. After each build, one untimed run per configuration warmed the file cache, then `scripts/profiling/run_wall.py <run.yaml> 5 <label>` ran five timed repeats from the repository root. Wall time is `run_wall.py`'s `time.perf_counter` per run; peak RSS is the `Peak process RSS` each run prints at exit. The peak buffered population is the run profile's `Output population P`, a run-level maximum of one output buffer's population (vertical driver) or one generation's (horizontal driver), so it is not a count of records written and is not comparable between the drivers. The Galaxies rows saved were counted from the saved HDF5 output of the last repeat of each configuration (every `Galaxies` table of the run, summed). No threshold applies.

| Driver | Pipeline | Wall median (s) | Wall min to max (s) | Spread (max − min, s) | Median against empty | Peak RSS min to max (MB) | Peak buffered population | Galaxies rows saved |
|---|---|---|---|---|---|---|---|---|
| vertical | empty | 0.437 | 0.415 to 0.464 | 0.049 | 1.0× | 74 to 76 | 40,537 | 292,163 |
| vertical | explicit rate | 4.204 | 3.996 to 4.244 | 0.248 | 9.6× | 40 to 61 | 16,731 | 204,974 |
| vertical | implicit rate | 14.321 | 13.812 to 14.391 | 0.579 | 32.8× | 54 to 74 | 16,725 | 204,920 |
| horizontal | empty | 0.492 | 0.472 to 0.497 | 0.025 | 1.0× | 456 to 461 | 82,137 | 292,163 |
| horizontal | explicit rate | 4.111 | 4.060 to 4.254 | 0.195 | 8.4× | 81 to 89 | 39,665 | 204,974 |
| horizontal | implicit rate | 13.629 | 13.571 to 13.676 | 0.105 | 27.7× | 89 to 97 | 39,644 | 204,920 |

How to read it, and what it does not say. On this fixture the rate phase costs about eight to ten times the empty pipeline's wall time under the explicit scheme and about 28 to 33 times under the implicit one. The comparison is not like for like, in two ways the table makes visible. First, the rate pipeline's `test_rate_jump` merges satellites into their centrals, so the explicit rate pipeline saves 204,974 `Galaxies` rows against the empty pipeline's 292,163 (the implicit one 204,920), the same under both drivers, and the rate pipelines hold a smaller peak buffered population (horizontal 39,665 against 82,137; vertical 16,731 against 40,537). Their peak RSS is lower than the empty pipelines' in the same direction, so the figure here follows the population held and not the rate phase's own memory, whose integrator workspace (about 0.4 to 0.6 kB per galaxy of the largest component, [above](#cost-integrator-alone)) is far below the noise of these figures. The size of the horizontal difference (about 460 MB against about 85 MB) is larger than the population ratio alone suggests, and was not investigated. Second, each law evaluation copies `rows × transfers` coefficients for the failure file even with the purity probe off (every evaluation, whatever the probe setting), and the fixture's laws, the dispatch's gather and write-back, and the discrete-phase ledger are all inside the rate figure. The fixture's coefficients are test values chosen to make every term active, so the stiffness, and with it the step count, is the fixture's; **no production figure is claimed** and none can be read from these. The two drivers save the same rows (292,163 empty, 204,974 explicit rate), but their peak buffered populations differ because the figure measures a different unit in each driver, so each driver's rate rows are compared against its own empty baseline and never against the other driver's. Peak RSS repeats are bimodal for the rate pipelines (for example 40 or 61 MB, vertical explicit) and the figure is the allocator's high-water, so the range is reported rather than a mean.

## Amendments and readings the owner or the run took

Stated here because the brief's text or the plan's, as frozen, differs from what was built and verified.

- **Bimolecular implicit criterion (owner amendment).** The plan's criterion required the A − B drift at 16, 64, 256 and 1024 fixed steps to fall by a factor whose base-4 logarithm is at least 0.9 per quadrupling. The owner amended it: the total within `tol_F` at every n, and the logarithm at least 0.9 on each quadrupling from n = 64; the 16 to 64 figure is pre-asymptotic, measured and recorded, not asserted. Measured in `test_bimolecular_drift_first_order` (`tests-sage16-mini-millennium.log`): drift 6.9242e-02, 2.0375e-02, 5.3269e-03, 1.3471e-03 at n = 16, 64, 256, 1024; log₄ of the ratio 0.8824 (16 to 64, not asserted), 0.9677 (64 to 256, at least 0.9) and 0.9917 (256 to 1024, at least 0.9); closure 3.7e-3, 4.8e-4, 6.1e-4 and 2.1e-4 of `tol_F`. No averaging.
- **First-step rule (owner amendment, both schemes).** Design 3.5's first-step estimate leaves out, as well as zero-weight entries, entries whose galaxy's own total (over the relevant total's non-tracer entries; a tracer uses its parent's) is zero at segment start; error control then governs them from the first step through the trial-end scale. The owner flagged this for the post-implementation review; it is stated here so that review has it.
- **Positivity step count.** Design 3.5's `N_pos = ceil(Δ·K_max/(σ·C))` applies `Δ` once, because `K_i` already carries the row's `Δ` (the criterion's `K_max = kb + 1` fixes the units). Any use of `N_pos` in the statistics follows this reading.
- **Implicit `L`.** Under the implicit scheme `L` counts the reservoir and tracer entries on one row as well as the inflow and outflow terms on one entry.
- **Dispatch readings.** A source is clocked on its recipient row; ties among one `to_record: central` source evaluated on several rows are broken by the assembly's row-by-row canonical listing; a first-snapshot sentinel row's law time is the context time (`dT` taken as 0, `Δ` 0); a module listed twice in the rate phase is refused; a purity-probe failure writes the failure file with cause `impure_law`; the discrete ledger covers FoF-workspace phases only, not `post_snapshot`, and its tolerance operands are the larger of before and after the unit; `test_rate_jump` merges orphan (`Type 2`) satellites only, so its output depends on orphans existing in the run.
- **Known limits carried, not defects.** Duplicate observers on one flux are not refused anywhere. The generator legends in `scripts/generate_module_registry.py` do not yet mention the rate mode. Both are owner follow-ups outside the plan.

## What did not pass, and caveats

Nothing failed. In full:

- **Gate item 7** is not met by this slice and is not claimed (Slice 13).
- **`test_zero_values` warning.** Each of the five `make tests` runs reported one `MIMIC_RESULT: WARN test_zero_values` (a scientific-tier warning about output fields that are zero in the run: 3 fields for `halos-only`, 4 for `hod`; not a failure, no run's exit status depended on it). It was not compared against the reference commit in this slice.
- **`NA` markers.** The `make tests` logs carry 39 to 50 `NA` markers (a test not applicable to the build or package) and no `SKIP` or `FAIL` markers; the batteries, which fail on any `SKIP` or `NA`, reported none.
- **Timing machine.** Not idle (see [What was measured](#what-was-measured)). The cost-driver ratios spread by 0.03 to 0.06 across three runs; the real-dispatch spread is in the table.
- **Cost driver and `python3`.** The literal `python3 tests/manual/run_rate_battery.py --cost` fails with an ImportError for `h5py` under a Homebrew `python3` without the project's virtual environment; it runs with `mimic_venv/bin` first on `PATH`, as `make tests-rate` does through `$(PYTHON)`. The module's top-level imports predate this slice.
- **Scope of the cost figures.** One star shape, constant coefficients, default tolerances; one fixture pipeline on one dataset, one machine. They bound the integrator's own scaling and show the fixture's cost; they predict no production model's.
- **Limits of the plan, unchanged** ([Outcome and Limits](MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md#outcome-and-limits)): no model package gains a rate module and no production output changes by a byte; no located events; no auxiliary state in rate phases; one rate phase per run file; bit-identity per binary; cross-driver identity for tree rows only; conservation to a stated rounding tolerance; the two schemes' tolerances are not comparable.
