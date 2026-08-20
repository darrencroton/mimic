# Benchmark: sage16 on mini-Millennium

**Date**: 2026-08-20 · measured at commit `48ffc244`

**Purpose**: A component-level CPU time breakdown of Mimic's default run, separated along the architectural boundaries in [VISION.md](../VISION.md), so that later performance work can be aimed at a specific owner (physics module, core, I/O, utility) without blurring those boundaries.

**Status**: Measurement only. This document reports where time is spent. It proposes no changes and no code was modified to produce it.

---

## Table of Contents

1. [What Was Measured](#what-was-measured)
2. [Headline Result](#headline-result)
3. [Component Breakdown](#component-breakdown)
4. [Physics Modules](#physics-modules)
5. [Hot Lines](#hot-lines)
6. [Differential Experiments](#differential-experiments)
7. [Validation of the Attribution](#validation-of-the-attribution)
8. [Structurally Notable Findings](#structurally-notable-findings)
9. [Optimisation Surface and Vision Constraints](#optimisation-surface-and-vision-constraints)
10. [Caveats and Blind Spots](#caveats-and-blind-spots)
11. [Reproducing This Benchmark](#reproducing-this-benchmark)

---

## What Was Measured

**Command** (the shipped default configuration):

```bash
./mimic ./models/sage16/input/sage16_mini-millennium.yaml -q
```

| Aspect | Value |
|---|---|
| Repository | `feature/ctrees-snapshot-reader` at commit `48ffc244` |
| Build | existing `./mimic`, default `cc -g -O2 -Wall -Wextra -Wshadow -Wformat-security -Wundef` |
| Packages | `MODEL=sage16`, `SIMULATION=mini-millennium` |
| Libraries | HDF5 1.14.6, libyaml 0.2.5, MPI off |
| Machine | macOS 26.6.2, arm64, 10 cores; single-threaded, single-rank |
| Workload | 8 partitions, 8 output snapshots, `SubSteps: 10`, **187,832 galaxy records** written, 62 MB HDF5 |
| Run-profile terms | galaxy-pool high-water G = 15,525; peak output-buffer population P = 14,648; buffer capacity C = 74,345 |
| Wall-clock instrument | `time.perf_counter()` around `subprocess.run`, 15 repeats (first discarded); corroborated with hyperfine 1.19.0 |
| Profile instrument | `/usr/bin/sample -wait mimic 10 1`, 20 serial repeats, 56 820 self-samples |
| Serialisation | every run executed alone — no two `mimic` processes concurrently |

**Prerequisite fixed during this work**: `simulations/mini-millennium/snapshots` pointed at mini-Uchuu data, so the shipped run file silently processed zero trees. It now points at `/Volumes/Internal/data/millennium/millennium-mini`, matching the convention used by every other simulation package. The path is gitignored, so no tracked file changed.

**Attribution method**: `sample`'s call graph annotates most frames with `file.c:line`. Self-time per frame is the frame's sample count minus the sum of its direct children, and each frame is assigned to exactly one component by source path. Library frames are assigned by rule: `libsystem_m` to Math library (with a secondary table naming the calling component that pays for it), `libhdf5` and its syscalls to Output I/O, other kernel syscalls to the nearest source-annotated ancestor, allocator and platform frames to Runtime/system. Frames that resist attribution are reported in an explicit `Unattributed` bucket — 0.004% of the run, a single `DYLD-STUB$$malloc` — and are never folded into a named component.

Percentages are fractions of total samples, converted to milliseconds using the **sampler-free** wall mean (3.3229 s), because the realised sampling interval (~1.27 ms) differs from the nominal 1 ms.

---

## Headline Result

| Metric | Value |
|---|---|
| Wall time (mean of 14) | **3.3229 s** ± 0.0102 s (min 3.3101, median 3.3207, max 3.3461) |
| hyperfine cross-check | 3.339 s ± 0.005 s (user 3.219 s, sys 0.071 s) |
| Peak RSS | 64 520 192 B (0.0645 GB) |
| IPC | ≈ 3.43 (34.3e9 instructions / 10.0e9 cycles) |

The run is CPU-bound and single-threaded: 97% of wall time is user CPU, and I/O of every kind accounts for under 5%.

---

## Component Breakdown

Grouped by the ownership boundaries in VISION.md. Uncertainty is ±1σ Poisson on the aggregate sample count.

| Component (VISION owner) | % of CPU | ±σ | ms |
|---|---|---|---|
| **Physics modules** — `models/sage16/modules/**` | **47.92%** | 0.29 | 1592 |
| **Core execution** — `src/core/**` | **31.87%** | 0.24 | 1059 |
| **Utilities** — `src/util/**` | **5.87%** | 0.10 | 195 |
| **Math library** — `libsystem_m` (paid for by callers, see below) | **4.74%** | 0.09 | 158 |
| **Runtime/system** — platform, allocator, dyld, other kernel | **4.55%** | 0.09 | 151 |
| **Output I/O** — `src/io/output/**` + libhdf5 + write syscalls | **3.87%** | 0.08 | 128 |
| **Tree input I/O** — `src/io/tree/**` + its read/open syscalls | **1.01%** | 0.04 | 33 |
| **Model shared helpers** — `models/sage16/shared/**` | **0.17%** | 0.02 | 5.6 |
| **Module system framework** — `src/module_system/**` | **0.00%** | — | 0 |
| **Unattributed** | 0.004% | — | 0.1 |

### Core execution, broken out

| Sub-part | % of CPU | ms | What it is |
|---|---|---|---|
| Module dispatch — `module_registry.c` | **27.12%** | 901 | `execute_phase` per-module and per-galaxy dispatch loops, event bookkeeping |
| `build_model.c` | 1.83% | 61 | FoF workspace construction, recursive tree walk |
| `inheritance.c` | 1.20% | 40 | galaxy inheritance across snapshots |
| virial helpers | 0.67% | 22 | derived halo quantities |
| `output_buffer.c` / marshalling | 0.44% | 15 | workspace → output record |
| `halo_evolution.c` | 0.29% | 9.5 | per-halo evolution driving |
| `galaxy_pool.c` | 0.20% | 6.6 | galaxy slot allocation |
| `tree_driver.c` | 0.10% | 3.4 | partition claiming and traversal |
| `main.c`, `init.c`, config parsing | <0.02% | <0.6 | startup |

### Utilities, broken out

| Sub-part | % of CPU | ms | What it is |
|---|---|---|---|
| Logging / error — `error.c` | **4.15%** | 138 | almost entirely `is_debug_log_rate_limiting_enabled` |
| Version — `version.c` | 1.35% | 45 | two startup `popen` calls, `sw_vers` and `md5` — **not git** (fixed cost per run) |
| Numeric | 0.27% | 9.1 | |
| Memory, progress, run profile, I/O helpers | 0.10% | 3.4 | |

### Runtime/system, broken out

| Sub-part | % of CPU | ms | Principal payer |
|---|---|---|---|
| `__bzero` | 3.15% | 105 | `src/io/tree/interface.c` (3.14 points) — load-buffer zero-fill |
| Allocator internals | 0.86% | 29 | `src/util/memory.c` |
| `memmove` / `memset` / `strcmp` | 0.51% | 17 | mixed |

### Output I/O, broken out

| Sub-part | % of CPU | ms |
|---|---|---|
| `open`/`close`/`pwrite` issued from inside libhdf5 | 2.21% | 74 |
| libhdf5 internals | 1.14% | 38 |
| `src/io/output/*.c` | 0.51% | 17 |

### Who pays for the math library

`libsystem_m` self-time attributed to the calling component. Add these to a module's own figure for its true cost.

| Payer | % of CPU |
|---|---|
| `sage_calculate_cooling_budget` | 3.31% |
| `sage_apply_metal_enrichment` | 0.53% |
| `sage_reionization` | 0.31% |
| core: virial | 0.26% |
| core: marshalling | 0.14% |
| core: `build_model` | 0.14% |
| everything else | <0.05% |

By function: `log10` 1.88%, `__exp10` 0.76%, `cbrt` 0.42%, `exp` 0.42%, `pow` 0.30%, PLT stubs 0.94%.

### Inclusive sanity frame

`main` 100% → `tree_driver` 97.9% → `build_model` 88.8% → `halo_evolution` 83.8% → `module_registry` 83.6%. Tree reader `interface.c` 4.8% inclusive; output `hdf5.c` 4.0% inclusive.

---

## Physics Modules

Self-time per module instance. Add the libm payer column above where applicable.

| Module | % of CPU | ±σ | ms |
|---|---|---|---|
| `sage_calculate_cooling_budget` | 10.22% | 0.13 | 340 |
| `sage_apply_star_formation_supernova` | 8.46% | 0.12 | 281 |
| `sage_radio_mode_heating` | 5.72% | 0.10 | 190 |
| `sage_calculate_star_formation` | 4.10% | 0.08 | 136 |
| `sage_apply_infall` | 2.89% | 0.07 | 96 |
| `sage_starburst_feedback` | 2.84% | 0.07 | 94 |
| `sage_quasar_mode` | 2.53% | 0.07 | 84 |
| `sage_reincorporation` | 1.98% | 0.06 | 66 |
| `sage_resolve_mergers_and_disruption` | 1.67% | 0.05 | 56 |
| `sage_apply_metal_enrichment` | 1.63% | 0.05 | 54 |
| `sage_disk_instability` | 1.47% | 0.05 | 49 |
| `sage_apply_cooling` | 1.18% | 0.05 | 39 |
| `sage_calculate_supernova_feedback` | 1.16% | 0.05 | 39 |
| `sage_satellite_stripping` | 0.91% | 0.04 | 30 |
| `sage_reionization` | 0.56% | 0.03 | 18.5 |
| `sage_set_disk_scale_radius` | 0.23% | 0.02 | 7.5 |
| `sage_prepare_infall_budget` | 0.21% | 0.02 | 6.9 |
| `sage_initialise_merger_clock` | 0.18% | 0.02 | 6.1 |

Cooling budget is the most expensive prescription: 10.22% self plus 3.31% libm ≈ 13.5%, which matches its 13.53% inclusive share. Its cooling-table interpolation (`cooling_tables.c:154 get_metaldependent_cooling_rate`) is 3.25% of the run on its own.

---

## Hot Lines

Top 20 by self-time. `:0` means the frame carried no line number (function entry/epilogue), not a parse failure.

| % | ms | Location | Symbol |
|---|---|---|---|
| 6.14 | 204 | `src/core/module_registry.c:882` | `execute_phase` |
| 5.00 | 166 | `sage_radio_mode_heating.c:248` | `..._process` |
| 4.87 | 162 | `sage_calculate_cooling_budget.c:132` | `..._process` |
| 4.15 | 138 | `src/util/error.c:211` | `is_debug_log_rate_limiting_enabled` |
| 3.15 | 105 | libsystem_platform | `__bzero` |
| 2.77 | 92 | `src/core/module_registry.c:870` | `execute_phase` |
| 2.10 | 70 | `sage_apply_star_formation_supernova.c:165` | `..._process` |
| 2.10 | 70 | libsystem_kernel | `__read_nocancel` |
| 1.96 | 65 | `src/core/module_registry.c:835` | `execute_phase` |
| 1.88 | 62 | libsystem_m | `log10` |
| 1.84 | 61 | libsystem_kernel | `__open` (via libhdf5) |
| 1.73 | 58 | `cooling_tables.c:154` | `get_metaldependent_cooling_rate` |
| 1.56 | 52 | `src/core/module_registry.c:886` | `execute_phase` |
| 1.52 | 51 | `cooling_tables.c:0` | `get_metaldependent_cooling_rate` |
| 1.52 | 50 | `sage_calculate_star_formation.c:0` | `..._process` |
| 1.36 | 45 | `sage_quasar_mode.c:65` | `..._process` |
| 1.34 | 44 | `sage_starburst_feedback.c:0` | `..._process` |
| 1.29 | 43 | `sage_calculate_star_formation.c:99` | `..._process` |
| 1.29 | 43 | `src/core/module_registry.c:846` | `execute_phase` |
| 1.28 | 43 | `src/core/module_registry.c:875` | `execute_phase` |

Seven of the twenty hottest lines are in `execute_phase`.

---

## Differential Experiments

Configuration variants, 7 serial repeats each, first discarded. Each variant was checked for real work — a config that silently processes nothing is not a data point.

| Variant | mean | stdev | Δ vs baseline | % of baseline | verification |
|---|---|---|---|---|---|
| baseline (shipped run file) | 3.3229 s | 0.0102 | — | 100% | 187,832 records, 62 MB HDF5 |
| **C1** empty module pipeline | 0.5630 s | 0.0197 | **−2.760 s** | 16.9% | accepted, rc=0, 292,163 records, 83 MB |
| **C2** `output_format: binary` | 3.2755 s | 0.0253 | −0.047 s | 98.6% | 187,832 records × 264 B = 48 MB |
| **C3** all 64 snapshots output | 4.0196 s | 0.0494 | +0.697 s | 121.0% | 1,518,968 records, 480 MB HDF5 |
| **C4** `SubSteps: 1` | 0.9459 s | 0.0042 | −2.377 s | 28.5% | 187,773 records |
| `--skip` with outputs present | 0.080 s | 0.056 | — | 2.4% | **zero galaxies — not a physics data point** |

hyperfine agrees with all four variants to within 1.2%.

**Interpretation:**

- **The empty pipeline is valid** — VISION principle 1 holds in practice: rc=0, halo tracking without galaxy physics. It writes *more* records than the baseline (292,163 vs 187,832) because no merger or disruption module removes galaxies, so C1 does more output work than the baseline. The −2.760 s delta is therefore a **lower bound** on physics-plus-dispatch cost.
- **Output volume is not the bottleneck**: 8.1× the records (187,832 → 1,518,968) and 7.7× the bytes (62 → 480 MB) cost only +21% wall time. RSS rises 0.062 → 0.159 GB.
- **Substep scaling**: dropping 9 substeps saves 2.377 s, i.e. **0.2641 s per substep**. Substep-scaled work is therefore ≈2.641 s (79.5%) and per-snapshot fixed work ≈0.682 s (20.5%).
- **There is no output-suppression flag.** `--skip` resumes by skipping partitions whose outputs already exist, and `--compress` gzips HDF5; neither can be used to time "run without writing". The 0.080 s `--skip` figure measures only the startup/config/version/shutdown floor.

---

## Validation of the Attribution

The sampled breakdown and the differential experiment must tell the same story, and initially appear not to:

| Quantity | Value |
|---|---|
| Sampled physics + libm + model shared | 52.83% (1.756 s) |
| Measured baseline − C1 (empty pipeline) | 83.06% (2.760 s) |

The 30-point gap is not an error — it is the point of the exercise. Turning off the modules also turns off the framework work done *on their behalf*: the per-module, per-galaxy dispatch in `module_registry.c` (27.12%) and the `DEBUG_LOG` predicate reached only from those dispatch sites (4.15%).

| Quantity | Value |
|---|---|
| Sampled physics + libm + shared + dispatch + logging predicate | **84.10%** |
| Measured baseline − C1 | **83.06%** |

Agreement to **1.0 percentage point (~35 ms)**, and the residual has the expected sign, since C1 writes 1.56× the baseline's records. The attribution is validated top-down.

**The distinction that matters for planning**: "running the physics" costs ~84% of the run, but only ~53 points of that is physics-module code. About 31 points is framework overhead incurred per module per galaxy per substep.

---

## Structurally Notable Findings

Reported as evidence, with no recommendation attached.

1. **Module dispatch is the single most expensive translation unit in the build.** `src/core/module_registry.c` is 27.12% ± 0.22 of all CPU (901 ms), essentially all of it self-time in `execute_phase` spread across lines 831–895 — the full-halo pass, and the by-galaxy pass whose inner loop re-scans all configured modules for every galaxy and filters on `processing_mode`. That is 2.7× the cost of the most expensive physics prescription.
2. **A logging predicate runs in the hot path even under `-q`.** `src/util/error.c:211 is_debug_log_rate_limiting_enabled` is 4.15% ± 0.09 (138 ms). `DEBUG_LOG` (`src/util/error.h:78-80`) calls it *before* any log-level test, and of `execute_phase`'s two `DEBUG_LOG` sites, the PASS-2 one (`:882`) fires once per by-galaxy module × galaxy × substep while the PASS-1 one (`:846`) fires once per full-halo module × substep × FoF group. It lives in a different translation unit and the build has no LTO, so `-O2` can neither inline nor elide it. Its caller-side argument setup is folded into `execute_phase`'s 27%.
3. **Startup spawns two subprocesses.** `src/util/version.c` costs 1.35% (45 ms) via `popen`/`fgets`/`posix_spawn`/`wait4`: `sw_vers -productVersion` (`:135`) and `md5 -q` on the run YAML (`:234`). **Not git** — git provenance comes from build-time `git_version.h` by design, so it describes the compiled binary rather than the working directory. A fixed per-run cost, negligible for long runs but dominant in the 80 ms floor.
4. **Zero-filling load buffers costs more than the tree reader itself.** `__bzero` is 3.15% (105 ms), of which 3.14 points is paid by `src/io/tree/interface.c` — three times the reader's own 1.01%.
5. **HDF5 output is cheap, and most of what it costs is syscalls rather than code.** Of the 3.87% Output I/O, 2.21 points are `open`/`close`/`pwrite` issued from inside libhdf5 (61 ms in `__open` alone) against only 0.51 points of `src/io/output/*.c`. C2 confirms the writer is not a bottleneck: the binary writer saves 47 ms (1.4%).
6. **`src/module_system/**` contributes 0.00% at runtime.** The framework infrastructure is genuinely build-time and registration-only; all runtime dispatch cost sits in `src/core/module_registry.c`.

---

## Optimisation Surface and Vision Constraints

Where measured time actually is, and which VISION principle constrains any change there. This section names the surface; it does not endorse any specific change.

| Surface | Size | Governing principle and constraint |
|---|---|---|
| `execute_phase` dispatch loops | 27.1% | **Principle 4 (one coherent processing model)** and **Principle 2 (runtime modularity)**. Dispatch must remain generic function-pointer calls over registered modules — no physics-specific special-casing, no compile-time binding of the pipeline. Precomputing per-phase mode-partitioned module lists preserves both, since it changes *how the same list is walked*, not what may be configured. Any change must preserve documented phase and dispatch ordering. |
| Hot-path logging predicate | 4.2% | **Principle 1 (physics-agnostic core)** is unaffected; this is a pure core-utility concern. Constraint is behavioural: `--debug` output, the rate-limiting semantics, and `DEBUG_LOG`'s argument-evaluation contract must not change. |
| `sage_calculate_cooling_budget` + cooling tables | 13.5% | **Principle 2** — module-owned physics. Any change here is a scientific change and falls under `mimic-scientific-method`: it needs baseline comparison and a defensible tolerance, not just a timing win. |
| Other physics modules | ~39% | Same as above. Module-local, per-module, individually small. |
| Tree-load buffer zero-fill | 3.1% | **Principle 6 (format-agnostic I/O)** and **Principle 5 (bounded memory, explicit ownership)**. Zeroing may be initialisation the property system relies on — check `init_source` semantics in `mimic-properties` before assuming it is redundant. |
| Startup `sw_vers` + `md5` subprocesses | 1.4% | **Principle 6 (reproducible output)**. OS version and the parameter-file digest are provenance requirements, not overhead to delete; only *how* they are obtained is negotiable, and a replacement digest must stay byte-identical to `md5`/`md5sum`. |
| Output writers (HDF5 and binary) | 3.9% | **Principle 6**. Small return, and metadata completeness must not regress. |

The two largest single opportunities are core-owned, not physics-owned, and both are compatible with the vision in principle. The largest physics-owned cost is a scientific-change question rather than an engineering one.

Note also that 79.5% of the run scales with `SubSteps`, so the per-galaxy-per-substep path is the multiplier that governs everything else.

---

## Caveats and Blind Spots

- **`-O2` inlining folds static helpers into their enclosing function and translation unit.** Any static function in `module_registry.c` is invisible and counted as `execute_phase` self-time; module statics fold into `<module>_process`. **Per-component totals are safe; per-function attribution inside a translation unit is not.** Before optimising a specific line in `execute_phase`, confirm what actually lives there.
- **Sampling floor**: nominal 1 ms, realised ~1.27 ms (2841 samples/run). Work under ~1 ms per occurrence that does not repeat is invisible — per-module `init()`/`cleanup()` falls below the floor. Entries under ~0.05% are "present, not quantified".
- **Sampler overhead**: sampled runs averaged 3.606 s against 3.323 s unsampled (+8.5%). The cost lands in the sampler, but stack-walk pauses are not uniform in call depth, so the deeply recursive `build_halo_tree_from_view` (~40 levels) may be mildly over-represented.
- **Warm file cache**: every timed run except the discarded warm-up read the tree files from cache. Tree input I/O at 1.01% is a warm-cache number; a cold first read is not represented.
- **Recursion**: self-time is unaffected; inclusive figures use an outermost-occurrence rule and must not be summed with their callees.
- **Scope**: one dataset, one model, single rank, MPI off. Shares will move with simulation size (deeper trees → more `build_model`), snapshot count (C3), and substep count (C4). This is not a claim about Uchuu-scale runs.
- **Not done**: no xctrace cross-check; no rebuild, so no `-O3`, `-fno-inline`, or LTO comparison; no cold-cache run; no cache-miss or branch-miss counters beyond whole-run IPC ≈ 3.43.

---

## Reproducing This Benchmark

The harness is committed at [`scripts/profiling/`](../../scripts/profiling/) — runner, `sample` call-graph parser, `nm` symbol index, and the attribution pass — with a README covering preconditions, attribution rules, and the (absent) Linux path. **It is macOS-only**: component attribution depends on `/usr/bin/sample`'s `file.c:LINE` annotations, which `perf` does not reproduce.

**Check the input data first.** `simulations/<sim>/snapshots` is a gitignored symlink; if it points at the wrong dataset the run still exits 0, processes zero trees, and finishes in ~0.08 s. That trap cost real time when this profile was taken.

To reproduce the headline numbers without the harness:

```bash
# wall clock, warm cache
hyperfine --warmup 1 -r 10 './mimic ./models/sage16/input/sage16_mini-millennium.yaml -q'

# per-symbol profile of one run
./mimic ./models/sage16/input/sage16_mini-millennium.yaml -q & \
  sample $! 10 1 -f /tmp/mimic_sample.txt; wait
```

The "Sort by top of stack" section of the `sample` report reproduces the leaf ranking directly; the "Call graph" section carries the `file.c:line` annotations that the component attribution is built from.
