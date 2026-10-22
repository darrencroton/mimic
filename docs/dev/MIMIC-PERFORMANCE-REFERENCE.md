# Mimic Performance Reference: sage16 on mini-Uchuu

**Purpose:** The measured cost model of Mimic at research scale: where a full-box `sage16` run spends its time and memory under each driver, how that compares with the mini-Millennium profile the optimisation work was planned on, how the run scales under MPI, and the per-halo and per-callback rates that let a new run be estimated before it starts. It is the research-scale companion of [`BENCHMARK-SAGE16-MINI-MILLENNIUM.md`](BENCHMARK-SAGE16-MINI-MILLENNIUM.md) and the evidence the Tier A performance plan drew on (`MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md`, archived under `archive/dev-plans/`).

**Status:** Measurement record, 2026-10-21, with an after-the-plan section added 2026-10-22 ([After the Tier A plan](#after-the-tier-a-plan-2026-10-22)); every section above it describes `8613bded`, before the plan's changes. It reports where time and memory go and proposes no change. Every number is perishable (one machine, one commit, one dataset) and nothing outside `docs/dev/` may cite it; re-measure before acting on any of it.

---

## Table of Contents

1. [What Was Measured](#what-was-measured)
2. [Headline Results](#headline-results)
3. [Component Breakdown](#component-breakdown)
4. [Physics Modules](#physics-modules)
5. [Hot Lines](#hot-lines)
6. [MPI Scaling](#mpi-scaling)
7. [Comparison With mini-Millennium](#comparison-with-mini-millennium)
8. [The Cost Model](#the-cost-model)
9. [Structurally Notable Findings](#structurally-notable-findings)
10. [Caveats and Blind Spots](#caveats-and-blind-spots)
11. [Reproducing This Measurement](#reproducing-this-measurement)
12. [After the Tier A plan (2026-10-22)](#after-the-tier-a-plan-2026-10-22)

---

## What Was Measured

| Aspect | Value |
|---|---|
| Repository | `main` at `8613bded` (`v1.5-30-g8613bded`); the plan's baseline `5daa24ee` plus one documentation commit, no code change between them |
| Build | `cc -g -O2 -Wall -Wextra -Wshadow -Wformat-security -Wundef`, HDF5 1.14.6, libyaml 0.2.5, MPI off for the profile (the MPI scaling pass used `USE-MPI=yes` builds, Open MPI 5.0.9) |
| Packages | `MODEL=sage16` with `SIMULATION=mini-uchuu` (vertical, L-Halo binary) and `SIMULATION=mini-uchuu-horizontal` (horizontal, version 3 HDF5) |
| Data | The whole mini-Uchuu box, 400 Mpc/h: 128 L-Halo files, 141 GB, and their horizontal conversion, 50 slabs plus `forests.h5`, 190 GB; 1,451,359,554 halos over 50 snapshots, 25,843,142 forests, largest snapshot 39,798,251 halos, largest forest 770,680 halos (`simulations/mini-uchuu-horizontal/README.md`) |
| Run files | `models/sage16/input/sage16_mini-uchuu.yaml` with `last_file` raised from the shipped 3 to 127 (a scratch copy, nothing else changed), and `sage16_mini-uchuu-horizontal.yaml` as shipped (0 to 127); both `SubSteps: 10`, eight output snapshots (49, 28, 23, 16, 12, 10, 8, 7), HDF5 output, `-q` |
| Machine | Mac Studio, Apple M3 Ultra (32 cores), 512 GiB, macOS 27.0.1; data and output on the internal SSD; nothing else running |
| Wall-clock instrument | `scripts/profiling/run_wall.py`, 3 serial repeats per driver after one warm-up run (the warm-up also warms the file cache; at 512 GiB the datasets stay cached) |
| Profile instrument | `/usr/bin/sample -wait mimic 14400 5 -mayDie`, a scratch copy of `scripts/profiling/sample_runs.sh` with the duration cap raised from 10 s to 4 h and the interval from 1 ms to 5 ms; 2 sampled runs per driver, aggregated by `scripts/profiling/attribute.py` with the sampler-free wall mean; the realised interval was about 6.3 ms, so 693,878 (vertical) and 710,722 (horizontal) self-samples in all |
| Differential | 2 runs per driver at `SubSteps: 1` |
| MPI scaling | `mpirun -np 1, 4, 8, 16` on `USE-MPI=yes` builds of both pairs, one run at one rank and two at each other count, 2026-10-21 07:32 to 10:00 |
| Serialisation | Every run executed alone; the MPI pass started only after the profiling pass had exited |
| Attribution checks | `source_attribution.mapped_fraction` 1.0 on both drivers; Unattributed 0.01% (vertical) and 0.32% (horizontal, almost all one `memcpy` stub under the horizontal reader) |

---

## Headline Results

| Metric | Vertical | Horizontal |
|---|---|---|
| Wall, mean of 3 | **2167.0 s** (36.1 min), σ 7.1 s, min 2160.6, max 2174.6 | **2267.1 s** (37.8 min), σ 73.1 s, min 2221.1, max 2351.4; the first repeat was 125 s slower than the other two, whose mean is 2225.0 s |
| Halos per second | 669,700 | 640,200 (652,300 without the slow repeat) |
| Peak process RSS | 1.82 GB (timed repeats 1.76 to 1.94) | 63.4 GB on the warm-up, 55.1 to 55.2 GB on the timed repeats |
| Output buffer capacity `C` | 3,853,400 records (0.71 GB): 5 × the largest forest | 41,788,163 records (7.69 GB): the largest slab plus 5% |
| Largest live-buffer population `P` | 743,295 (one forest) | 39,452,969 (one snapshot) |
| Galaxy pool high-water `G` | 786,875 galaxies (0.14 GB) | 40,831,220 galaxies (7.19 GB) |
| Retained generations `R` | not applicable | at most 2; retention pool at most 43.19 GB |
| `SubSteps: 1` wall, mean of 2 | 548.3 s (561.3, 535.2) | 573.2 s (568.5, 577.9) |
| Per-substep cost (linear model) | 179.9 s | 188.2 s |
| Fixed per-run cost | 368.4 s (17.0%) | 385.0 s (17.0%) |
| Output written | 8 snapshots, 53 GB per run directory | 8 snapshots, 51 GB per run directory |

Under MPI both drivers reach 13.5 to 13.8× at 16 ranks (see [MPI Scaling](#mpi-scaling)). The two drivers process the same halos in nearly the same time; the horizontal driver pays about 4.6% more (2.7% against the two clean repeats), all of it in slab handling (its input reader plus `memmove`, below), and holds about thirty times the memory, because its working set is the two widest slabs rather than one forest.

---

## Component Breakdown

Grouped by the ownership boundaries in [VISION.md](../VISION.md). Percentages are of all self-samples; milliseconds use the sampler-free wall mean. Poisson uncertainty on a component is at most ±0.08 points.

| Component (VISION owner) | Vertical % | Vertical s | Horizontal % | Horizontal s |
|---|---|---|---|---|
| **Physics modules** (`models/sage16/modules/**`) | **49.53** | 1073 | **49.62** | 1125 |
| **Core execution** (`src/core/**`) | **35.66** | 773 | **34.14** | 774 |
| **Utilities** (`src/util/**`) | 5.24 | 114 | 5.10 | 116 |
| **Math library** | 4.53 | 98 | 4.45 | 101 |
| **Runtime/system** | 2.78 | 60 | 2.85 | 65 |
| **Input I/O** (`src/io/vertical/**` or `src/io/horizontal/**` and their syscalls) | 0.87 | 19 | 2.85 | 65 |
| **Output I/O** (`src/io/output/**`, libhdf5, write syscalls) | 1.07 | 23 | 0.41 | 9 |
| **Model shared helpers** | 0.31 | 7 | 0.28 | 6 |
| **Unattributed** | 0.01 | 0.2 | 0.32 | 7 |

### Core execution, broken out

| Sub-part | Vertical % | Horizontal % | What it is |
|---|---|---|---|
| Module dispatch (`module_registry.c`) | **30.82** | **29.60** | `execute_phase`'s per-module and per-galaxy loops, event bookkeeping, the `DEBUG_LOG` call sites |
| `inheritance.c` | 1.60 | 1.45 | galaxy inheritance across snapshots |
| `build_model.c` | 1.22 | — | FoF workspace construction, recursive tree walk (vertical only) |
| `horizontal_driver.c` | — | 1.20 | slab sweep, generation retention, rebase |
| `virial.c` | 0.66 | 0.62 | derived halo quantities |
| `output_buffer.c` / marshalling | 0.61 | 0.56 | workspace to output record |
| `halo_evolution.c` | 0.50 | 0.55 | per-FoF evolution driving |
| `galaxy_pool.c`, `fof_workspace.c`, drivers, syscalls | 0.26 | 0.16 | |

### Utilities, runtime and I/O, broken out

| Sub-part | Vertical % | Horizontal % | Principal payer |
|---|---|---|---|
| `error.c` (`is_debug_log_rate_limiting_enabled`) | **3.71** | **3.56** | every `DEBUG_LOG` site in the dispatch loop, even under `-q` |
| `numeric.c` | 1.37 | 1.40 | |
| `memory.c`, `run_profile.c`, `progress.c`, `version.c` | 0.16 | 0.13 | `version.c` has no samples: its two `popen` calls are invisible at this scale |
| `__bzero` | **2.16** | 0.52 | vertical: the per-forest zero-fill of the 5× output seed in `src/io/vertical/interface.c` (2.14 points by the malloc-payer table) |
| `_platform_memmove` | 0.09 | **2.28** | horizontal: slab copies under `read_horizontal_hdf5.c`, plus a 0.31% `memcpy` stub left Unattributed |
| allocator and other platform | 0.53 | 0.05 | |
| `read_horizontal_hdf5.c` and `pread` | — | 2.75 | the horizontal reader's own code and its reads |
| `__read_nocancel` (L-Halo reads) | 0.78 | — | |
| HDF5 output: `hdf5.c`, `pwrite`, `close`, `__open` | 0.45, 0.22, 0.19, 0.05 | 0.01, 0.19, 0.08, 0.02 | the per-partition create, close and reopen that cost 2.4% on mini-Millennium is 0.05% here |

### Math library

| Function | Vertical % | Horizontal % |
|---|---|---|
| `log10` (plus stub) | 1.95 | 1.96 |
| `__exp10` (plus stub) | 0.99 | 0.95 |
| `exp` (plus stub) | 0.64 | 0.64 |
| `cbrt` (plus stub) | 0.58 | 0.55 |
| `pow` (plus stub) | 0.37 | 0.36 |

Payers: `sage_calculate_cooling_budget` 2.91% / 2.88%, `sage_apply_metal_enrichment` 0.64% / 0.63%, `sage_reionization` 0.36% / 0.36%, core virial 0.29% / 0.29%, marshalling 0.18% / 0.16%.

---

## Physics Modules

Self-time per module instance, both drivers. Add the libm payer share above for a module's true cost.

| Module | Vertical % | Horizontal % |
|---|---|---|
| `sage_calculate_cooling_budget` | 9.86 | 10.30 |
| `sage_disk_instability` | 7.78 | 7.15 |
| `sage_apply_star_formation_supernova` | 5.15 | 4.89 |
| `sage_calculate_star_formation` | 5.03 | 5.16 |
| `sage_radio_mode_heating` | 4.40 | 5.06 |
| `sage_apply_infall` | 3.24 | 3.18 |
| `sage_starburst_feedback` | 2.41 | 2.29 |
| `sage_reincorporation` | 1.99 | 2.20 |
| `sage_apply_metal_enrichment` | 1.92 | 1.85 |
| `sage_resolve_mergers_and_disruption` | 1.41 | 1.38 |
| `sage_calculate_supernova_feedback` | 1.35 | 1.27 |
| `sage_quasar_mode` | 1.15 | 1.07 |
| `sage_satellite_stripping` | 1.12 | 1.10 |
| `sage_apply_cooling` | 0.96 | 0.95 |
| `sage_initialise_merger_clock` | 0.71 | 0.73 |
| `sage_reionization` | 0.47 | 0.46 |
| `sage_prepare_infall_budget` | 0.30 | 0.31 |
| `sage_set_disk_scale_radius` | 0.29 | 0.26 |

The ranking is the mini-Millennium ranking with one move: `sage_disk_instability` rises from fifth (4.6%) to second (7.8%), the one module whose share grows with box size (more unstable discs in a larger, better-sampled population).

---

## Hot Lines

Top 20 by self-time, vertical driver (the horizontal list is the same set; its own additions are `_platform_memmove` at 2.28% in eighth place and `cooling_tables.c:0` slightly higher). `:0` is a frame with no line number (entry or epilogue), not a parse failure. Per-line attribution inside a translation unit is soft under `-O2`.

| % | Location | Symbol |
|---|---|---|
| 5.33 | `sage_calculate_cooling_budget.c:132` | `sage_calculate_cooling_budget_process` |
| 5.02 | `src/core/module_registry.c:1603` | `execute_phase` (the by-galaxy `process()` call) |
| 3.80 | `sage_disk_instability.c:67` | `sage_disk_instability_process` |
| 3.71 | `src/util/error.c:211` | `is_debug_log_rate_limiting_enabled` |
| 3.62 | `sage_radio_mode_heating.c:248` | `sage_radio_mode_heating_process` |
| 3.30 | `src/core/module_registry.c:1598` | `execute_phase` (the `DEBUG_LOG` site) |
| 3.01 | `src/core/module_registry.c:0` | `execute_phase` |
| 2.31 | `sage_apply_star_formation_supernova.c:194` | `..._process` |
| 2.16 | libsystem_platform | `__bzero` |
| 2.09 | `src/core/module_registry.c:1586` | `execute_phase` (the per-galaxy module loop) |
| 2.06 | `sage_calculate_star_formation.c:0` | `..._process` |
| 1.97 | `sage_disk_instability.c:64` | `..._process` |
| 1.62 | libsystem_m | `log10` |
| 1.56 | `sage_calculate_star_formation.c:99` | `..._process` |
| 1.52 | `src/core/module_registry.c:1587` | `execute_phase` (the mode test) |
| 1.46 | `src/core/module_registry.c:1604` | `execute_phase` |
| 1.41 | `src/core/module_registry.c:971` | `dispatch_events_range` (the early return) |
| 1.31 | `src/core/module_registry.c:1591` | `execute_phase` |
| 1.27 | `src/core/module_registry.c:1535` | `execute_phase` |
| 1.22 | `cooling_tables.c:0` | `get_metaldependent_cooling_rate` |

Ten of the twenty hottest lines are in `execute_phase` or the logging predicate it calls, as on mini-Millennium.

---

## MPI Scaling

`USE-MPI=yes` builds of both pairs, Open MPI 5.0.9, `mpirun -np N ./mimic <run file> -q` on the 32-core machine, one run at one rank and two at each other count, started only after the profiling pass had finished. Speed-up is against the one-rank MPI run; "vs serial" against the non-MPI mean above (2167.0 s and 2267.1 s). Rank 0's peak RSS is from the run memory profile, which only rank 0 prints.

| Ranks | Vertical wall (s) | Speed-up | Efficiency | Rank 0 peak RSS | Horizontal wall (s) | Speed-up | Efficiency | Rank 0 peak RSS |
|---|---|---|---|---|---|---|---|---|
| 1 | 2160.4 | 1.00 | 100% | 2.65 GB | 2288.5 | 1.00 | 100% | 54.3 GB |
| 4 | 607.5 (609.8, 605.2) | 3.56 | 89% | 1.96 GB | 631.0 (633.6, 628.4) | 3.63 | 91% | 22.4 GB |
| 8 | 307.8 (308.6, 307.0) | 7.02 | 88% | 2.12 GB | 322.6 (321.3, 323.9) | 7.09 | 89% | 9.6 GB |
| 16 | 156.7 (156.5, 156.9) | 13.79 | 86% | 1.49 GB | 169.3 (169.2, 169.3) | 13.52 | 85% | 7.1 GB |

- The MPI binary at one rank costs the same as the non-MPI one (vertical 2160.4 s against 2167.0 s; horizontal 2288.5 s against 2267.1 s, within the horizontal run's spread).
- Both drivers scale near-linearly to 16 ranks with 85 to 91% parallel efficiency. The vertical driver stripes the 128 input files across ranks (8 per rank at 16), so its residual is the imbalance of file sizes plus the serial parts of a run (startup, the master file); the horizontal driver shards forests by peak occupancy, and its residual is the forest-weight imbalance plus task 0's partition scans.
- Horizontal memory divides with the ranks: rank 0 falls from 54.3 GB to 7.1 GB at 16 ranks, because each rank retains only its own forests' rows of the two live generations. Vertical memory is per forest and does not change with rank count (the 1.5 to 2.6 GB spread is the allocator's large-block cache).
- Output identity under MPI was not re-checked here; it is gated elsewhere (the fixture gates at 1, 2, 3, 4 and 8 ranks and the micro-Uchuu runs at 4 ranks, `CHANGELOG.md` v1.5), and this pass timed the runs only.

---

## Comparison With mini-Millennium

Shares of CPU for the same commit lineage (`5daa24ee` for mini-Millennium, `8613bded` for mini-Uchuu, no code change between them), same machine, same build. The mini-Millennium run is 2.63 s (vertical) and 2.73 s (horizontal) over 1.53 million halos; mini-Uchuu is 950 times the halos.

| Share | mini-Millennium V | mini-Uchuu V | mini-Millennium H | mini-Uchuu H |
|---|---|---|---|---|
| Physics modules | 46.11 | 49.53 | 48.02 | 49.62 |
| Core execution | 34.55 | 35.66 | 32.80 | 34.14 |
| of which `module_registry.c` | 30.00 | 30.82 | 28.40 | 29.60 |
| `error.c` | 3.30 | 3.71 | 3.13 | 3.56 |
| Math library | 4.65 | 4.53 | 4.64 | 4.45 |
| Output I/O | 4.31 | 1.07 | 2.75 | 0.41 |
| of which kernel `__open` | 2.36 | 0.05 | 2.21 | 0.02 |
| `__bzero` | 2.14 | 2.16 | 0.28 | 0.52 |
| `_platform_memmove` | 0.26 | 0.09 | 2.00 | 2.28 |
| Input I/O | 0.90 | 0.87 | 3.16 | 2.85 |
| Fixed cost by the `SubSteps: 1` differential | 19.0% | 17.0% | not measured | 17.0% |

**What scales with what.** Every cost that is paid per galaxy per substep per module keeps its share across a factor of 950 in halos: dispatch (30 to 31%), the logging predicate (3.3 to 3.7%), the physics (46 to 50%), the math library (4.5 to 4.6%). The vertical zero-fill keeps its share too, because it is paid per forest in proportion to the forest's halos. The costs that shrink are the ones paid per file or per run: HDF5 partition creation and reopen falls from 2.4% to 0.05%, and output I/O as a whole from 4.3% to 1.1%, because mini-Uchuu writes 8 partitions for 1.45 billion halos where mini-Millennium wrote 8 for 1.5 million. The physics share rises by the 3 points the I/O gave up, which is dilution, not new physics cost. The 17% fixed cost that remains at research scale is not file I/O; it is the per-snapshot work outside the substep loop (tree or slab build, inheritance, marshalling and output), and it scales with the halo count like everything else.

---

## The Cost Model

Rates derived from the two measurements, for estimating a run before it starts. Each is labelled measured or estimated.

| Quantity | Value | Basis |
|---|---|---|
| Halos per second, `sage16`, 10 substeps, one process | 670,000 (vertical), 640,000 (horizontal) | measured: 1,451,359,554 halos over 2167.0 s and 2267.1 s; mini-Millennium gives 582,000 and 562,000 on a run too short to amortise startup |
| Module callbacks per halo-snapshot at the shipped pipeline | about 140 | estimated: 11 by-galaxy modules × 10 substeps per galaxy, plus 34 full-halo calls per FoF group (3 per substep and 4 pre-timestep) at about 0.89 FoF groups per halo (the mini-Millennium ratio) |
| Callbacks per run | about 2.0 × 10¹¹ | estimated from the row above |
| Cost per callback, all in (physics plus framework) | about 9.0 ns | estimated: the substep-scaled time (2167.0 − 368.4 = 1798.6 s vertical) over the callback count; 10 ns on mini-Millennium |
| Framework overhead per callback (dispatch plus logging predicate) | about 3.1 ns, about a third of every callback | estimated: 34.5% of the run (30.8% dispatch, 3.7% logging) at 9.0 ns per callback |
| Fixed cost per halo (outside the substep loop) | 254 ns (vertical), 265 ns (horizontal) | measured: 368.4 s and 385.0 s over the halo count; 17% of a 10-substep run |
| Time per substep per halo | 124 ns (vertical), 130 ns (horizontal) | measured: 179.9 s and 188.2 s over the halo count |
| Vertical peak memory | about 2.4 kB per halo of the largest forest (1.82 GB for a 770,680-halo forest), of which 0.92 kB is the 5× output seed | measured; the forest, not the box, sets vertical memory |
| Horizontal peak memory | about 1.4 kB per halo of the widest slab (55 to 63 GB for 39.8 million halos); the retention pool alone 1.09 kB per halo (43.2 GB over two generations) | measured; the widest slab, not the box, sets horizontal memory |
| Output | about 36 B per halo (53 GB for 1.45 billion halos over 8 output snapshots) | measured |
| Run-to-run noise | 0.3% (vertical, σ over 3), but a single repeat can sit 5% out (the horizontal first repeat) | measured |
| Parallel efficiency under MPI | 85 to 91% from 4 to 16 ranks on both drivers: wall ≈ serial / (0.86 × N) at 16 ranks | measured |
| Horizontal memory per rank | about 54 GB / N plus a 3 to 4 GB floor on this box (22.4, 9.6 and 7.1 GB at 4, 8 and 16 ranks) | measured |

**Worked example, the cut Shin-Uchuu dataset** (22,503,649,037 halos). The rates predict a vertical `sage16` run of about 9.3 hours in one process (15.5 times the mini-Uchuu halos at 670,000 per second), or about 40 minutes at 16 ranks at the measured efficiency if the 361 files balance, and about 800 GB of output over the ten output snapshots its run files request, and a horizontal run of about 9.8 hours before the chunked sweep's re-reads. Its largest forest (126,380,326 halos) predicts a vertical peak of about 300 GB by the per-halo figure above, against about 165 GB measured with `MallocLargeCache=0` (`CHANGELOG.md`, Unreleased): the per-halo memory of a forest falls as forests grow, because the galaxy pool and output buffer grow with the output population rather than with halos, so the per-halo figure is an upper bound for forests larger than mini-Uchuu's. The horizontal driver at one task and 128 chunks peaked at 6.170 GB on that dataset (`simulations/shin-uchuu/README.md`), which the widest-slab rule explains: each chunk holds 1/128 of a 519-million-halo slab.

---

## Structurally Notable Findings

Reported as evidence, with no recommendation attached.

1. **The framework's per-callback overhead is a third of every callback, and it is scale-free.** Module dispatch plus the logging predicate are 34.5% (vertical) and 33.2% (horizontal) of a research-scale run, the same as on the 2.6 s benchmark, because they are paid per galaxy per substep per module. Nothing in them depends on box size, so a saving measured on mini-Millennium is a saving on any dataset.
2. **The two drivers cost the same per halo to within 5%.** The horizontal driver's extra 2.7 to 4.6% is slab handling: its reader (2.85%) and `memmove` (2.28%) against the vertical reader's 0.87% and `__bzero` 2.16%. Each driver has one input-side cost of about 2%: the vertical zero-fill of its 5× output seed, the horizontal slab copy.
3. **Memory is set by the largest unit, not the box.** Vertical: 1.82 GB for the whole 400 Mpc/h box, because a forest of 770,680 halos is the working set. Horizontal: 55 to 63 GB, because two generations of a 39.8-million-halo slab are. The vertical driver's 5× output seed is 0.71 of its 1.82 GB.
4. **Per-file I/O costs vanish at scale.** The HDF5 create, close and reopen per partition is 0.05% here against 2.4% on mini-Millennium; the two end-of-run `popen` calls register no samples at all. Output I/O in total is 1.1% (vertical) and 0.4% (horizontal).
5. **One physics module grows with the box.** `sage_disk_instability` is 7.8% here against 4.6% on mini-Millennium; every other module keeps its share to within a point.
7. **MPI already delivers the parallel speed-up the pathway assigned to thread-per-forest.** 13.8× (vertical) and 13.5× (horizontal) at 16 ranks on a 32-core node, bitwise identity being gated elsewhere, with horizontal memory dividing by the rank count. What threads would add over this is the 14% efficiency gap and intra-node balance, not a new order of magnitude.
6. **The fixed 17% is per-snapshot work, not startup.** On mini-Millennium 19% of the run was outside the substep loop; at 950 times the halos it is still 17%, so it is tree and slab construction, inheritance, marshalling and output, all proportional to halo count, not a per-run floor.

---

## Caveats and Blind Spots

- **One machine, one build, one commit.** Apple M3 Ultra, `-g -O2`, no LTO. Shares on a Linux GCC node will differ in the math library and the allocator; the dispatch and logging shares should not.
- **Two sampled runs per driver** at a realised 6.3 ms interval give about 350,000 samples each, enough for every component above 0.1% but not for a line below that.
- **The horizontal first repeat is an outlier** (+3.7% on the mean, 5.5% slower than the other two), so the horizontal wall mean carries ±42 s of doubt; the shares do not depend on it.
- **The `SubSteps: 1` runs are not a like-for-like pair**: `SubSteps` changes the physics slightly (743,118 against 743,295 records vertical), so the differential is a cost model, not an identity; and with two runs per driver the vertical pair differs by 4.7%.
- **Warm file cache throughout**: at 512 GiB the 141 GB and 190 GB datasets stay cached after the warm-up run, so input I/O here is a memory-bandwidth figure, not a disk figure. A first cold read of either dataset is not represented.
- **Peak RSS under the macOS allocator** includes its large-block cache; the retention-pool figure (43.2 GB) is the accounted horizontal working set.
- **The callback counts are estimated**, from the shipped pipeline and the mini-Millennium FoF ratio, not counted; a scratch counter would make them measured.

---

## Reproducing This Measurement

The committed harness is `scripts/profiling/` (see its README). Two things differ for a 36-minute run and should be done by hand until the sampler script takes them as options:

```bash
# Build the pair, warm up once, time three repeats (vertical shown; the horizontal pair
# uses SIMULATION=mini-uchuu-horizontal and the shipped run file unchanged).
make MODEL=sage16 SIMULATION=mini-uchuu -j"$(sysctl -n hw.ncpu)"
sed -e 's/^  last_file: 3$/  last_file: 127/' \
    -e 's#^  output_directory: .*#  output_directory: output/perf-mini-uchuu/vertical#' \
    models/sage16/input/sage16_mini-uchuu.yaml > /tmp/sage16_mini-uchuu_all.yaml
./mimic /tmp/sage16_mini-uchuu_all.yaml -q                       # warm-up, read its memory block
python3 scripts/profiling/nm_index.py
python3 scripts/profiling/run_wall.py /tmp/sage16_mini-uchuu_all.yaml 3 mini-uchuu -o /tmp/wall.json

# Sample: raise the duration cap and coarsen the interval in a scratch copy of the script.
sed 's#/usr/bin/sample -wait mimic 10 1#/usr/bin/sample -wait mimic 14400 5#' \
    scripts/profiling/sample_runs.sh > /tmp/sample_runs_long.sh && chmod +x /tmp/sample_runs_long.sh
MIMIC_REPO=$PWD /tmp/sample_runs_long.sh /tmp/sage16_mini-uchuu_all.yaml 2 /tmp/samples
python3 scripts/profiling/attribute.py /tmp/samples /tmp/agg.json --wall-mean-s <mean from wall.json>

# Restore the default pair before any other work.
make MODEL=sage16 SIMULATION=mini-millennium && make check-generated
```

Run the two drivers one after the other, never together, and leave the machine alone while they run. Each run writes about 53 GB of output; the four run directories of the profile pass (both drivers, `SubSteps` 10 and 1) took 208 GB and the two of the MPI pass about 160 GB more, under `output/perf-mini-uchuu/`, left in place for the owner to clear. For the MPI pass: `make USE-MPI=yes MODEL=sage16 SIMULATION=<pair>`, then `mpirun -np N ./mimic <run file> -q` per count, and `make MODEL=sage16 SIMULATION=mini-millennium` afterwards to restore the non-MPI default build. `nm_index.json` indexes the last build made, so regenerate it before attributing a different pair.

---

## After the Tier A plan (2026-10-22)

The same measurement, repeated on the plan's closing code: the branch `feature/performance-tier-a` at `e481658a`, whose code changes against the `8613bded` of the sections above are the `DEBUG_LOG` level-first gate, the init-time dispatch plans, the vertical output-buffer seed (`MAXHALOFAC` removed, no zero-fill of `ProcessedHalos`), and, for the benchmark script only, an `EXTRA_LDFLAGS` pass-through; the default build is unchanged (`-g -O2`, no flag set was adopted), and every galaxy is byte-identical to `5daa24ee` on the mini-Millennium gate. The method is [What Was Measured](#what-was-measured)'s: `sage16` on all 128 mini-Uchuu files, the vertical run file's `last_file` raised to 127 in a scratch copy, `scripts/profiling/run_wall.py`, one warm-up repeat and three timed repeats per driver, run alone on the same machine and build configuration, and, as a drift check, one run of a `5daa24ee` build (built for `mini-uchuu` and `mini-uchuu-horizontal`) in the same session. The record of the measurement, its scripts and the whole plan's evidence is `MIMIC-PERFORMANCE-ACCEPTANCE.md`, Slice 9.

| Metric | Vertical, before the plan | Vertical, after | Horizontal, before the plan | Horizontal, after |
|---|---|---|---|---|
| Wall, mean of 3 | 2167.0 s, σ 7.1 s | **1924.4 s**, σ 32.1 s (min 1891.6, max 1955.8) | 2267.1 s, σ 73.1 s | **1983.6 s**, σ 4.2 s (min 1978.9, max 1986.6) |
| Change in wall | | **−11.19%** (−242.6 s) | | **−12.50%** (−283.5 s) |
| Same-session `5daa24ee`, one run | | 2173.3 s (+0.29% against the 2167.0 s) | | 2293.2 s (+1.15% against the 2267.1 s) |
| Change against the same-session `5daa24ee` | | −11.45% | | −13.50% |
| Halos per second | 669,700 | 754,183 | 640,200 | 731,672 |
| Peak process RSS | 1.82 GB | **0.67 to 0.70 GB** | 55 to 63 GB | 55.2 GB timed, 65.3 GB warm-up (a same-session `5daa24ee` run read 74.8 GB: allocator variance, no claim) |
| Output buffer capacity `C` | 3,853,400 records (0.71 GB) | **809,214** records (0.15 GB): the largest forest plus 5% | 41,788,163 | 41,788,163 (unchanged) |
| Largest live-buffer population `P` | 743,295 | 743,295 | 39,452,969 | 39,452,969 |
| Galaxy pool high-water `G` | 786,875 | 786,875 | 40,831,220 | 40,831,220 |
| Retained generations `R` / retention pool | not applicable | not applicable | at most 2 / at most 43.19 GB | at most 2 / at most 43.19 GB |

The per-galaxy costs the plan targeted held at research scale, as the sections above predicted: the saving on the full box (11.2% and 12.5% against the reference, 11.5% and 13.5% against a same-session build) is the size of the mini-Millennium composite (11.8% and 10.3%). The output buffer's seed is what changed the vertical memory profile: `C` is no longer five times the largest forest, and peak RSS fell by about 1.1 to 1.3 GB. The horizontal driver's memory profile is unchanged. **The Shin-Uchuu super-forest** (`sage16` on `shin-uchuu-lhalo` file 0, `MallocLargeCache=0`): peak RSS **70.96 GB**, against the 164.8 GB recorded before the plan; wall 165.4 s; `C` 132,699,342 records (24.4 GB, against 116.3 GB at the old seed); `P` 121,907,647, unchanged.

**What was not re-measured.** The research-scale component profile, the hot-line list and the MPI scaling table of the sections above were not repeated: the closing component profile in the acceptance record is mini-Millennium's (both drivers, 20 samples each), and the sections above remain the research-scale profile of the code before the plan, where `module_registry.c` was 30.82% (vertical) and 29.60% (horizontal) and the logging predicate 3.71% and 3.56%. They no longer describe HEAD (on mini-Millennium `module_registry.c` is 26.01% and 24.21% and `error.c` has no self-time), and the cost model above is the old code's. One `5daa24ee` run per driver is a drift check, not a paired design, and the vertical σ of 32.1 s is wider than the reference's 7.1 s.
