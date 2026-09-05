# Optimisation Spectrum: sage16 on mini-Millennium

**Date**: 2026-08-20 · derived from measurements at commit `48ffc244`

**Purpose**: The complete set of meaningful performance options for Mimic, ordered by expected value, each classified by VISION principle compatibility, scientific risk, and cost. This is an options catalogue, not a plan and not a recommendation to implement.

**Basis**: the measured profile in [BENCHMARK-SAGE16-MINI-MILLENNIUM.md](BENCHMARK-SAGE16-MINI-MILLENNIUM.md) (default run, 3.3229 s, single-threaded, `-g -O2`, no LTO), plus four independent code analyses of the core/dispatch, physics-module, I/O-memory-build, and architectural layers.

**Ordering rule**: expected value = (upside × confidence) ÷ (cost + scientific risk). Every entry is labelled **measured**, **estimated**, or **speculative**.

**Status**: Options catalogue — **step 4** in [`MIMIC-DEVELOPMENT-PATHWAY.md`](MIMIC-DEVELOPMENT-PATHWAY.md) → "The Ordered Road". Not scheduled, not a plan, and nothing here is authorised to be built. Promoting any of it requires a frozen implementation plan first.

---

## Read This First

**When this becomes current.** After pathway steps 1–3 (Shin-Uchuu, snapshot-global modules, distributed snapshot operations) and before step 5 (emulator). The reasoning for that placement is in the pathway under "Why performance is step 4"; the short version is that repeated-run activities downstream — emulator campaigns of 10²–10³ runs, the coupled-rate measurement spike — multiply any single-run saving, and that the dominant hot spot sits in dispatch machinery that steps 2 and 3 are about to modify.

**Re-measure before acting on anything.** These numbers were taken at commit `48ffc244` against a tree-driver run. By the time this is current, steps 2 and 3 will have changed the dispatch path and the rank model. Re-run the profile before promoting any item, and note the standing caveat: `-O2` folds static helpers into their translation unit, so **per-component totals are trustworthy but per-line attribution inside `execute_phase` is not**. Confirm what lives at `module_registry.c:882`, `:870` and `:835` before acting on items 1, 2 or 32.

**Two prerequisites that are constraints rather than code.**

1. **Do not add a dispatch mode here.** Batched dispatch (item 17) and the SoA generator (item 19) are the natural next lever after Tier A, and both are deferred out of step 4 deliberately. The pathway already accepts the cost of extending the mode-and-metadata machinery twice — once for snapshot-global modules, once for the coupled rate formulation — with a standing warning that this risks two incompatible notions of "a mode". The mode machinery is **step 2's**, extended by step 6; a batch mode must be designed against what step 2 shipped and co-designed with step 6's rate mode, never as an independent third extension.
2. **The coupled-rate work deletes state that SoA would transpose.** Eight `output: false` transport-scratch properties (`InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, `Rcool`, the cooling Λ diagnostic, the unstable-gas fraction) stop being galaxy state at all under that formulation. Transposing them first is work thrown away.

**What is safe to take in step 4, subject to re-measurement**: Tier A items 1–12, 14 and 15 (bit-identical, no baseline regeneration, no vision change; item 13's build flags are a separate promotion gated on the physics baseline) and thread-per-forest parallelism (item 16, also bit-identical, and its determinism precondition is already met and verified). Everything else should wait for a fresh measurement.

**If both a batch mode and a rate mode proceed**, amend `docs/VISION.md` Principle 4 **once** — to make dispatch modes an open, metadata-declared set rather than a closed list of three — rather than twice, in two incompatible ways.

---

## Table of Contents

1. [Structural Facts That Drive The Ordering](#structural-facts-that-drive-the-ordering)
2. [Tier A: Bit-Identical, Cheap](#tier-a-bit-identical-cheap)
3. [Tier B: Bit-Identical, Structural](#tier-b-bit-identical-structural)
4. [Tier C: Changes Results](#tier-c-changes-results)
5. [Tier D: Rejected, With Evidence](#tier-d-rejected-with-evidence)
6. [Composition and Dependencies](#composition-and-dependencies)
7. [Missing Instruments](#missing-instruments)
8. [Corrections To Earlier Claims](#corrections-to-earlier-claims)

---

## Structural Facts That Drive The Ordering

Five measured facts explain why the ordering below is not the obvious one.

**1. Mimic is overhead-bound on ~10⁸ tiny work items, not flop-bound.** Derived from tree-file counts: 1,533,122 halo-snapshots × 10 substeps × **11** by-galaxy modules, plus 1,365,471 FoF groups × 10 × 3 full-halo ≈ **2.10 × 10⁸ module invocations in 3.32 s ≈ 15.8 ns (~55 cycles) each**. Roughly ~29 cycles of physics, ~15 of dispatch, ~6 of logging overhead (the cycle split is **estimated**, the counts measured). This argues *for* amortising per-call cost and *against* GPU offload. (measured counts, estimated cycle split)

**2. 94.5% of FoF groups are singletons.** Measured by parsing all 8 mini-Millennium LHaloTree files directly:

| quantity | value |
|---|---|
| forests | 29,585 |
| halo-snapshots | 1,533,122 |
| FoF groups | 1,365,471 |
| singleton groups | 1,290,455 (94.5%) |
| **halo-snapshots in singleton groups** | **84.2%** |
| work in groups ≤ 4 members | 94.2% |
| largest group (z=0) | 110 members |
| forest sizes | median 29, mean 51.8, p99 461, max 14,869 (0.97% of all work) |

The singleton majority is uniform, coupling-free, and perfectly batchable; the forest distribution is near-ideal for load balancing. (measured)

**3. Intra-FoF coupling is a genuine sequential dependence, and it confines the parallel unit to the FoF group — but governs only ~16% of work.** Three prescriptions write into the shared central galaxy: `sage_satellite_stripping.c:107-108`, `sage_apply_star_formation_supernova.c:151-152`, `sage_apply_metal_enrichment.c:111`. Worse than reassociation: `sage_apply_star_formation_supernova.c:159` clamps against a central value earlier satellites already mutated. And `sage_resolve_mergers_and_disruption.c:99,244` set `Type = 3` mid-run, changing the live set between substeps. Parallelism must therefore take the **FoF group** as its atomic unit — which still leaves 84.2% of work in singletons where every cross-write is a self-write. (measured)

**4. Determinism prerequisites for parallelism are already satisfied.** No `rand/srand/random/drand48/gsl_rng/arc4random/time(NULL)` anywhere in `src/` or `models/`. The only stochastic site in the repo (`models/sham/.../sham_assign_stellar_mass.c:52-86`) is a stateless `splitmix64` keyed on `UniqueGalaxyID`, falling back to `FileNum`/`TreeID`/`HaloNr`/`MostBoundID` when that is 0 — deterministic, but keyed on two of the globals item 16 must instance. Every module-level `static` in sage16 is write-once at `init()`, so `process()` is pure with respect to galaxy state; the one exception is the per-call-site `DEBUG_LOG` rate-limit counters (`error.h:83-84`), which item 16 also lists as a threading blocker. `GlobalForestOffset` is computed upfront by every rank, so `UniqueGalaxyID` is stable across task counts. **VISION Principle 4 is honoured; there is no correctness bug here and no blocker to aggressive parallelism.** (measured)

**5. There is no locality problem on the tree driver.** Per-FoF working set is 1.11 galaxies × 352 B ≈ **391 bytes** — seven cache lines, L1-resident for the whole 10-substep × 18-module residency — and whole-run IPC is **3.43**. Galaxy-major ordering is *already* the cache optimisation. This is why SoA and hot/cold splitting score near zero on their own merits. (measured)

---

## Tier A: Bit-Identical, Cheap

Items 1–12, 14 and 15 change no output byte. **Two carve-outs**: item 4's precomputed reciprocal is reassociation-only, and item 13 (build flags) is possibly FP-perturbing and must be promoted separately against the physics baseline. Together, plausibly ~15–25% of runtime for days-to-weeks of work. (estimated)

### 1. Gate `DEBUG_LOG` at the call site so the predicate is inlinable
`src/util/error.h:78-94` calls the cross-TU `is_debug_log_rate_limiting_enabled()` (`error.c:211`) *before* any log-level test. Fix: reorder to test the level first via an `extern` variable, and/or make the predicate `static inline` in the header. **Upside: 4.15% measured** (the predicate's own self-time), plus an unquantified share of the 6.14% attributed to `module_registry.c:882`. Principles 1 and 7 compatible — logging stays core-owned, modules still see only the macro. Cost: trivial.

**Two caveats.** Arguments *are* evaluated for the first `DEBUG_LOG_MAX_CALLS = 5` calls per site, and outside the armed window (module `init()` sites) they are evaluated every time — the "never evaluated" property holds only in the steady state inside the driver's armed region. And level-first ordering changes observable `--debug` behaviour: Today each call site burns its 5-message budget even at levels where nothing prints, because the counters increment outside the level test. Level-first means budgets are consumed only when messages emit — arguably a bug fix, but grep the integration tests for suppression-message assertions first.

**Archaeology**: commit `7638d4da` (2025-11-26) introduced this macro claiming *"No performance impact when debug mode not enabled."* The profile falsifies that claim. This is why the cost went unnoticed for nine months.

### 2. Precompute mode-partitioned phase plans at `module_system_init`
`execute_phase` (`src/core/module_registry.c:865-891`) re-scans all configured modules per galaxy and filters on `processing_mode`. Measured waste per FoF-snapshot: `54·L` inner iterations that do nothing but fail a mode test — **33% of all PASS-2 iterations** — plus 130 wasted PASS-1 iterations, and two of the four configured phases run the entire per-galaxy loop while dispatching nothing at all. Fix: build per-(phase, mode) compact arrays once at startup from the runtime YAML, exactly as `PhaseModuleConfig.resolved` already does.

**Upside: 4-7% (estimated** from exact iteration counts × an assumed cost ratio). Principle 2 **compatible and reinforcing** — derived state computed *from* the runtime configuration, no compile-time binding. Principle 4 compatible provided YAML order within each mode and the galaxy-major law (`module_registry.h:127-136`) are preserved. Principle 7 *improved*: the "not resolved" `FATAL_ERROR` moves from per-dispatch to startup. Cost: moderate — and unusually low, because **`execute_phase` is called from one function (three call sites, `module_registry.c:928/935/941`) and from no test**, so its signature is free to change — though it is a published API at `module_registry.h:145` whose documented driver-neutral contract must move with it.

### 3. Hoist substep-invariant scalars out of the substep loop
Halo properties other than `Type` are read-only to modules, so `Vvir`, `Rvir`, `Mvir`, `Vmax`, `dT` and `DiskScaleRadius` (written only in `pre_timestep`, `sage_set_disk_scale_radius.c:99`) are fixed across all 10 substeps. Verified instances recomputed per galaxy per substep:

| site | invariant recomputed |
|---|---|
| `sage_calculate_cooling_budget.c:38-54` | `temp = k·Vvir²`, `tcool = Rvir/Vvir`, **`log10(temp)`** (`:38-47`), `4π·Rvir` (`:54`) |
| `sage_calculate_star_formation.c:88-92` | `reff`, `tdyn = reff/Vvir`, `cold_crit` |
| `sage_calculate_supernova_feedback.c:121-124` | the whole ejection coefficient (per FoF, per snapshot) |
| `sage_radio_mode_heating.c:147-149` | the entire `EDDrate` coefficient is a **run constant**. (`unit_conv` at `:44-45` is also constant but sits in `calculate_agn_rate_empirical`, dead under the shipped `AGNrecipe: 2`) |
| `sage_apply_metal_enrichment.c:109` | `exp(−central Mvir/scale)` |
| `shared/sage_disk_instability_physics.h:48-49` | `Vmax²·reff/G` |
| `shared/time_parity.h:55-71` | `halo->dT / num_substeps`, recomputed by ≥5 modules × 10 substeps |

**Upside: 3-6% (estimated)**. Bit-identical **only if** the hoisted expression is textually identical and evaluated in the same association order. Cost: days per site, plus one architectural decision about *where* the cached value lives — module-local memo, a declared transport property written by a `pre_timestep` module (the sanctioned mechanism, Principle 3), or core-provided per-galaxy scratch. **Prefer the first or third**: step 6 deletes transport-scratch properties and requires side-effect-free rate evaluation, so a new declared transport property is work thrown away and a contract to unwind. The `pre_timestep` phase already runs exactly once per snapshot interval (`module_registry.c:928`) and is the natural hook.

### 4. Restructure the cooling-table lookup (exact, not a surrogate)
`cooling_tables.c:137-165`. Four separable changes: replace the linear metallicity scan at `:154` (the measured hot line) with a direct index or branchless select over the fixed 8-entry grid; cache the temperature-side interpolation, which per item 3 is invariant across substeps; make it `static inline` or enable LTO so it stops being a cross-TU call with a PLT stub; precompute the 7 bracket reciprocals at init. **Upside: 1.5-4.5% (estimated)** against its measured 3.25% self + 1.88% `log10` + 0.76% `__exp10`. First three are bit-identical; the reciprocal is reassociation-only. Cost: medium-low.

### 5. Short-circuit `exp()` when `FracZleaveDisk == 0.0`
`sage_apply_metal_enrichment.c:107-114` and `shared/sage_starburst_physics.h:134-142` compute an `exp()` and multiply it by zero for every star-forming galaxy every substep, because the shipped config sets `FracZleaveDisk: 0.0`. **Upside: 0.5-0.9% (estimated)** with the current parameter. Bit-identical (`0.0 · finite = 0.0`). Cost: an hour. Best saving-to-effort ratio in the catalogue.

### 6. Hoist the redshift-only part of the reionization modifier out of the halo loop
`sage_reionization.c:37-96` is called per halo, but everything from `:39` to `:91` (`f_of_a`, `Mjeans`, `Mfiltering`, `Vchar`, `omegaZ`, `deltacritZ`, `HubbleZ`, `Mchar`) depends only on redshift and cosmology — not on `mvir`, which enters only at `:92-94`. Removes ~7 `sqrt` + 1 `pow` per halo. **Upside: ~0.7% (estimated)** against 0.56% self + 0.31% libm. Bit-identical. Cost: an hour.

### 7. Arm event-dispatch state only for phases that have consumers
`begin_/end_phase_event_dispatch` (`module_registry.c:599-632`) write 10 scalar fields each on every phase entry and exit — 21 phase entries per FoF-snapshot (`post_timestep` is empty and returns at `:827` before `begin`) — including for phases with no producer and no consumer, which is knowable at startup. Part of the unattributed residual inside `execute_phase`'s 27%. Bit-identical, compliant, days. (estimated, unquantified)

### 8. Hoist `ctx->active_event = NULL` out of the inner loop
`module_registry.c:885` stores to escaped memory once per dispatch; it is already NULL there (set by `begin_phase_event_dispatch`, restored by `dispatch_events_range` after every batch, and PASS 2 runs strictly after event dispatch). ≲1%, bit-identical, trivial — but **document the invariant at the hoisted store**, since `module_interface.h:236-238` contractually promises `active_event == NULL` for by-galaxy calls.

### 9. Remove redundant per-call recomputation in core and shared helpers
`mimic_find_fof_central_index()` scans (`sage_apply_infall.c:47`, `sage_resolve_mergers_and_disruption.c:151`) duplicate what `ctx->central_galaxy` already holds (`halo_evolution.c:84`). `mimic_tree_get_FirstHaloInFOFgroup` is recomputed for the same `halonr` in `build_halo_tree_from_view` (`build_model.c:116`, `:137`, and `:161` inside the per-subhalo loop). Two O(ngal) passes plus a linear central search per FoF-snapshot are fusable (`halo_evolution.c:155-174`, `build_model.c:163-165`). Also: `sage_radio_mode_heating.c:68-71` recomputes *exactly* the `x` that `sage_calculate_cooling_budget.c:48-49` just computed with the same inputs. Each ≲1%, bit-identical, trivial.

### 10. Eliminate or narrow the tree-load zero-fill
`src/io/tree/interface.c:163-166` and `:171-172` `memset` the whole `ProcessedHalos` and `FoFWorkspace` allocations per tree. `ProcessedHalos` is sized `MAXHALOFAC(5) × InputTreeNHalos` (`src/include/constants.h:25`), so 5× the eventual population is zeroed every tree. This is **3.14 of the measured 3.15% `__bzero`** — three times the tree reader's own code. **Gated on a correctness audit**: the zeroing is what guarantees `workspace[p].galaxy` reads NULL for untouched halos, which `marshal_workspace_to_output_buffer` relies on, and other `struct Halo` fields may depend on zero-init as an implicit `init_source` default. Narrow it correctly and it is bit-identical; get it wrong and it silently changes results. Cost: small patch, non-trivial verification.

### 11. Reuse tree-scoped buffers instead of alloc/clear/free per tree
`interface.c:131-179` allocates fresh per tree; `:189-202` frees per tree. Grow-to-high-water reuse is the idiom the codebase already endorses (`galaxy_pool.c:12-22`, `build_model.c:339-350`). Folds into item 10's saving plus part of the 0.86% allocator share. **1-3% combined (estimated)**. Principle 5 still satisfied — high-water-bounded reuse is bounded — but the per-tree cleanup contract in `free_unit_halos`'s docstring must be revised deliberately. Needs a test for a size-decreasing tree sequence.

### 12. Retune `MAXHALOFAC`
Output buffer capacity 74,345 vs population 14,648 — 5× over-allocation, which currently costs *time* through item 10's memset, not just RSS. Trades initial clear cost against `myrealloc_cat` frequency. Bit-identical (pure sizing constant), trivial to change, but it is shared code affecting every simulation package, so it needs re-benchmarking across tree-size distributions.

### 13. Enable LTO; evaluate `-O3` and `-fno-math-errno`
`Makefile:153` is `-g -O2`; `LDFLAGS` is initialised empty (`:166`) and accumulates only `-L` paths, and the link line (`:376`) passes no `CFLAGS`, so **there is no hook for link-time optimisation flags** — an `EXTRA_LDFLAGS` hook is the clean fix. LTO is the one flag with a *named* mechanism here: it inlines `is_debug_log_rate_limiting_enabled` across the TU boundary and lets the compiler prove it reads one global, and it also collapses the 0.94% of PLT stubs and lets `get_metaldependent_cooling_rate` inline. **3-8% claimed, genuinely unquantified**, and **not additive with item 1** — LTO's main win here *is* item 1. `-fno-math-errno` is bit-identical and cheap and deserves evaluation given libm is 4.7% of the run. There is **no `make release` target**, so adopting any of these as a default is a Makefile design decision, not a flag flip. Treat `-O3` and LTO as *possibly* FP-perturbing (inlining changes FMA-contraction opportunities) and gate on the baseline.

### 14. Collapse the HDF5 create/close/reopen sequence
`src/io/output/hdf5.c:79-146` creates each per-filenr file, closes it, then reopens it; `master_hdf5.c:114,197` reopens every finished file again to read `TotHalosPerSnap` back out. Up to 3 opens × 8 files, against a measured 61 ms in `__open`. Small here; **scales with file count, not row count**, so it is an early warning for Uchuu-scale runs with thousands of files. Principle 6 constrains the master-file half: that reopen is provenance work, and removing it means carrying the count forward in memory instead.

### 15. Remove the two startup `popen` calls
`src/util/version.c:135` shells out to `sw_vers -productVersion`; `:239` shells out to `md5 -q` on the run YAML. **1.35% (45 ms) measured**, all of it in the per-snapshot fixed cost — a real 1.35% of this 3.3 s benchmark, and noise on a 3-hour run. `sysctlbyname("kern.osproductversion")` replaces the first cleanly. The second needs an in-tree MD5 producing a **byte-identical digest** to `md5`/`md5sum`, or Principle 6's cross-platform provenance comparability breaks. Metadata-only risk, but under Principle 6 that is not a lesser category.

---

## Tier B: Bit-Identical, Structural

Weeks to months, no output change, no vision change.

### 16. Thread-per-forest parallelism on the tree driver
**The largest bit-identical win available (estimated).** 29,585 independent forests, median 29 halos, largest forest only 0.97% of all work — near-ideal balance for any core count up to ~100 with dynamic scheduling. 97% of wall time is user CPU on a 10-core machine. **Upside: 6-9× (estimated)**.

The determinism precondition is already met (structural fact 4). The blocker is enumerable global mutable state: from `src/include/globals.h` — `FoFWorkspace`, `ProcessedHalos`, `InputTreeHalos`, `HaloAux`, `TreeGalaxyPool`, `Ntrees`, `NumProcessedHalos`, `MaxProcessedHalos`, `MaxFoFWorkspace`, `TreeID`, `FileNum`, `GlobalForestOffset`, the `HDF5_*` writer block; plus `module_registry.c`'s `phase_event_state`, `registered_modules[]`, `execution_pipeline[]`; plus `src/util/memory.c`'s eleven unlocked tracking statics (`:44-54`) (the file's own header says *"Not thread-safe... Add locking before any shared-memory migration"*); plus the `DEBUG_LOG` per-call-site rate-limit statics and the progress bar.

Two facts make this cheaper than it looks. Module `init()`/`cleanup()` state is process-global but read-only during processing, so it can stay global — only per-unit state needs instancing. And the galaxy pool is *already* an instanced handle API (`galaxy_pool_create/alloc/reset/destroy`), while `snapshot_driver.c` already holds its state in a `struct SnapshotDriverState` — that refactor is partly done.

Output determinism is solvable while preserving bit-identity: per-thread buffers concatenated in canonical forest order, or two-pass count-then-scatter. Principle 5 needs per-thread arenas and a thread-aware tracker. Cost: 4-8 weeks. **Compatible with every principle.**

### 17. Batched dispatch mode plus SoA
A new `PROCESSING_MODE_BATCH_BY_GALAXY` where `process()` receives an array of independent galaxies, fed from the **singleton-group population** (84.2% of work, provably free of cross-galaxy writes because the central *is* the galaxy). Multi-member groups keep the scalar path.

Two independent gains, and the first is the larger: **overhead amortisation** — batching ~64 singletons collapses the per-galaxy dispatch, mode re-scan, `active_event` store, indirect call and logging gate by ~64×, worth **20-22% of runtime with no vector instruction at all** (estimated); and **vectorisation**, bounded by the 52.8% physics+libm share but capped hard by ISA width in `double` — **arm64 NEON is only 2-wide** (no SVE on Apple M), so realistically 8-13% there versus 20-25% on AVX-512. Combined: **1.4-1.6× arm64, 1.8-2.1× AVX-512 (estimated)**. Scales linearly to Uchuu.

Batching with preserved order is bit-identical; a vector libm is ULP-level and needs the tolerance treatment. Principle 4 compatible as a **fourth dispatch mode within the one traversal** — never a replacement for `PROCESSING_MODE_BY_GALAXY`. Principle 3 mandatory: the mode must be declared in `module_info.yaml` and flow through the generator. Principle 7: the validator must reject a batch module in a phase where a by-galaxy module writes cross-galaxy state, or structural fact 3's hazard returns by the back door. Requires SoA (item 19). Works on the **existing tree driver** — no snapshot driver needed. Cost: 3-5 months.

### 18. Snapshot-slab parallelism
Width = FoF groups per slab: 31,739 at mini-Millennium z=0, ~2.7 × 10⁸ projected for Shin-Uchuu. Groups depend only on immutable slab N and the previous generation's state. The only parallel axis that both scales to Uchuu and serves a device port. Requires item 16's context work plus lifting the snapshot driver's deliberate serial-only `NTask > 1` gate (`docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`), which is `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md` territory. Bit-identical. Cost: 2-4 months on top of item 16.

### 19. SoA generator and index-based linkage
`struct Halo` (176 B) holds a *pointer* to a separately pool-allocated `struct GalaxyData` (176 B), so every galaxy touch chases a pointer across two allocations. Replacing the pointer with an index removes that chase and is a precondition for both SIMD and any device port (device pointers cannot be host pointers). Co-allocating the two structs is the cheapest variant and needs no generator change.

**SoA itself is worth ~0 standalone** (structural fact 5) — its entire value is as substrate for items 17 and 20. **Principle 3 governs and is supportive**: this is a change to `scripts/generate_properties.py`, never a hand edit of `src/include/generated/`. Note `struct RawHalo` field order **is** the on-disk binary layout, so `halo_properties.yaml` order is frozen by the file format even though `Halo`/`GalaxyData` order is not. Bit-identical if operation order is unchanged. Cost: 4-8 weeks. **Fund only as slice one of item 17, never on its own merits** — and sequence it after the coupled-rate decision, because that work deletes eight of the transport-scratch properties SoA would transpose.

### 20. Field reordering by co-access
Each module touches roughly 3-8 of `struct GalaxyData`'s 32 fields, and the generated `float`/`double` interleaving scatters them across all three cache lines. Reordering so the fields the eleven by-galaxy modules share (`HotGas`, `ColdGas`, `MetalsHotGas`, `MetalsColdGas`, `StellarMass`, `NewStellarMass`) sit on one line is a YAML reorder plus `make generate`. Bit-identical. Upside unquantified and capped by structural fact 5 — measure `L1-dcache-load-misses` before investing.

### 21. Read/compute/write pipelining
Overlap next-tree read with current-tree compute. **Ceiling ~5%, and that is a warm-cache number** — tree input is 1.01% and output 3.87% here. Worth more cold-cache and at Uchuu scale, unmeasured. Portability constrains the mechanism: `posix_fadvise` is Linux-only (macOS needs `fcntl(F_RDAHEAD)`), `io_uring` is Linux-only, so a portable version means a pthread prefetcher — which raises the same Principle 5 ownership question as item 11, doubled.

---

## Tier C: Changes Results

Every item here requires the `mimic-scientific-method` evidence bar. Several are scientific projects with a speed side effect, not optimisations.

### 22. Coupled-rate / error-controlled integrator
**The largest single lever in the program.** Measured: `SubSteps: 1` runs in 0.9459 s vs 3.3229 s, so substep-scaled work is 79.5% at 0.2641 s per substep. An integrator achieving the required accuracy in one step would be **3.5×**; in two, 2.7×.

Already scheduled as pathway step 6 ([`MIMIC-COUPLED-RATE-FORMULATION-PLAN.md`](MIMIC-COUPLED-RATE-FORMULATION-PLAN.md)) and **correctly not framed as an optimisation** — it may well be a *more expensive* starting point, since the integration domain becomes the whole FoF group whenever any satellite is star-forming. It is materially result-changing by design: the six `min(requested, available)` clamps become step-rejection conditions, so it cannot be validated against sage16 by parity. It also extends Principle 4's closed list of dispatch modes. Note the brief currently assesses Principle 4 as *preserved* and drafts a Principle **2** refinement instead — that assessment needs revisiting, since a rate mode is a fourth entry in a list VISION states as closed. Cost: 9-18 months, gated on a measurement spike. Do not sell it as performance work.

### 23. Reducing the effective substep count
Mechanically a one-line YAML change; scientifically the worst-risk item in the catalogue. Four findings, three dispositive:

- **The existing dynamic scheme is slower, not faster.** `TimestepScheme: dynamic` is fully implemented (`read_parameter_file.c:213-236`, `timestep.c:11-28`, `halo_evolution.c:114-127`) and merely unused. Its formula is `ceil(dT · SubSteps / t_dyn)` — `SubSteps` means *substeps per dynamical time*. A recoverable design note (`git show 9a6f4322^:docs/dev/DYNAMIC-TIMESTEP-CONVERGENCE-NOTES.md`) tabulates unclamped `N` for mini-Millennium: **337,344 at z=80**, 19,026 at z≈20, 1,321 at z≈12, 270 at z≈5.3, 67 at z≈2.4, 3 at z=0. With `MaxDynamicSubsteps = 200` it runs clamped at 200 through roughly z ≳ 5 (the first ~20 of 63 snapshot transitions) and stays well above the fixed 10 for most of the rest — 67 at z≈2.4, 20 at z≈1.1, falling to 3 only at z=0. **Adaptive substepping is an accuracy lever misfiled as a performance lever.** There is no error estimator, no step rejection and no tolerance anywhere in the code.
- **One prescription is deliberately non-convergent in N.** `sage_satellite_stripping.c:74-83` strips `1 − (1 − 1/N)^N` per interval — 100% at N=1, → 1−1/e as N grows, **independent of `dT`**. Documented as deliberate parity behaviour and pinned by a regression test.
- **Convergence has never been measured**, and the coupled-rate plan says so explicitly. Calibrated parameters are compensating for numerics at `STEPS = 10`.
- **`float` reservoirs make the error curve U-shaped**: past the point where per-substep increments fall below half a ULP, `+=` silently no-ops. Empirical z=0 convergence was found between `SubSteps` 50 and 100.

### 24. Move per-snapshot-budget modules out of the substep loop
Two of eighteen (`sage_apply_infall`, `sage_satellite_stripping`) divide a per-snapshot budget by `num_substeps` every substep rather than integrating a rate, and the classification doc names `sage_apply_infall` as *"explicitly the interval-averaged rate"* of a `pre_timestep` budget. Whether a given module can move to `pre_timestep` is a per-module scientific question — for stripping the answer is no, per item 23 — but **the question has never been asked module by module**, and the YAML makes the move a one-line change. Unquantified; potentially several percent per module moved.

### 25. Reassociation-class arithmetic cleanups
`sage_apply_infall.c:60` and `sage_satellite_stripping.c:83` divide by `num_substeps` per call (a hoisted reciprocal is *not* bit-identical for N=10); the terminal `exp10` in the cooling path (`cooling_tables.c:164`) can be folded into the caller (**~1-1.5%**) — not a clean cancellation, since `logZ` is still needed for bracket selection at `:144-156` — and it sits directly upstream of the `rcool > Rvir` regime branch. **1-2% combined (estimated)**. These are last-bit changes, which per `mimic-scientific-method` discrete thresholds can amplify to O(1) for individual galaxies — possibly including a z=0 count difference. **This is the first tier-C item that costs a regenerated baseline with explained residuals.**

### 26. Tighten module guards where a store is skipped
Six modules do real work for zero-baryon galaxies: `sage_calculate_cooling_budget.c:136-138` unconditionally stores three fields; `sage_calculate_star_formation.c:88-104` computes `reff`/`tdyn`/`cold_crit` and always stores `NewStellarMass` with no `ColdGas` gate; `sage_calculate_supernova_feedback.c:121-132` computes the full ejection expression and stores both transport fields with no `stars > 0` gate; `sage_disk_instability.c:63` always stores; `sage_starburst_feedback.c:198-207` builds an 8-field parameter struct before the trigger test at `:209`; `sage_apply_metal_enrichment.c:98` always stores zero. Skipping a *computation* is bit-identical; **skipping a *store* into an `init_repeat` transport field is not** — that field is reset per snapshot, not per substep, so a skipped store leaves the previous substep's value live. Each site needs its own argument. Upside depends on the zero-baryon galaxy fraction, which is unmeasured.

### 27. Metadata-declared work-skipping predicates
The principled version of item 26: a `module_info.yaml` key declaring a skip condition over named declared properties, which the generator turns into a core-evaluable predicate — the core evaluates offsets, never field names it knows. Principle 3 aligned, Principle 1 preserved (a hard-coded core test on `HotGas` would be a flat violation). Two hazards: the modules already have these guards cheaply at the top of `process()`, so moving the test from callee to caller saves the call and not the test; and a *chain*-level skip is unsafe because transport properties are `init_repeat` per snapshot, so skipping module 2 while running module 4 re-applies stale state. Only an all-or-nothing chain skip is consistent — which is what the existing `galaxy == NULL || Type == 3` guard already is. Large machinery for an unquantified and probably small return.

### 28. Precision changes in a new model package
sage16's `float` reservoirs exist *only* for byte-parity with legacy sage-model (`models/sage16/model_properties.yaml:7-9`, which itself says *"New models without that constraint should prefer `double`"*). The `float`↔`double` conversion chains in the hottest code (e.g. `sage_apply_star_formation_supernova.c:127-160`, ~10 serially-dependent read-modify-writes) are a *consequence* of that settled policy, not an oversight. Widening would plausibly speed them up **and destroy parity** — so it belongs in a new package, never in sage16. Archaeology battles 2-4 close this; do not re-litigate.

### 29. GPU offload
The physics is genuinely independent per FoF group, and the snapshot-ordered driver already exposes slab-wide parallelism. But the arithmetic is hostile at this scale: ~31,700 threads each running ~27 cycles × 12 modules × 10 substeps of branchy `double` scalar code, total device flops order 10¹⁰ for the whole run — launch-latency and divergence bound. **Upside on this workload: negative to ~2×, i.e. worse than 10 CPU threads for vastly more complexity.**

At Shin-Uchuu scale (315,004,242 halos in the z=0 slab) it inverts: ~2.7 × 10⁸ independent groups per slab, device compute order 10¹³ flop per snapshot. Then transfer binds — 315M × 176 B ≈ 55 GB of state, ~110 GB per snapshot round trip, irreducible because all 10 substeps can run device-resident. On PCIe 5 that is ~1.7 s/snapshot against ~0.4 s of compute; on unified memory ~0.12 s. **Verdict: a unified-memory-APU play, not a discrete-PCIe play, and it does not pay below roughly 10⁸ halos per slab.** (flop/bandwidth figures are order-of-magnitude **speculation** from measured struct sizes and halo counts)

`float32` on device is rejected on *scientific* grounds, not performance: sage16 is dense with threshold branches that halving mantissa bits flips into O(1) divergence. Backend, given macOS-Clang + Linux-GCC/mpicc: **OpenMP `target`** is the only option keeping one C source tree with a no-op host fallback (CUDA excludes macOS, Metal excludes Linux, SYCL/Kokkos force C++). Even in `double` with preserved per-group order, device libm differs from host libm — the project already measures ~7e-4 macOS↔Linux divergence, hence CI's `MIMIC_BASELINE_RTOL=1e-3` — so this lands as *changes results within a defensible tolerance*, not bit-identical. Requires items 17 and 19 first. Cost: 6-12 months.

### 30. Learned surrogates for prescriptions
An ML surrogate for the cooling chain (~19% combined) removes flops but **changes the model** and fails the evidence bar at the threshold-flip clause: its approximation error feeds `rcool > Rvir`-style branches that convert it to O(1) per-galaxy divergence. It would also destroy the property that makes Mimic scientifically useful — that the prescription in the code *is* the published equation. **Recommend against for the production path.**

Note what the repo's own emulator brief actually is: `docs/dev/MIMIC-EMULATOR-PLAN.md` proposes Bayes linear emulation with history matching as an *instrument for scientific diagnosis* ("the deliverable is a diagnosis, not a calibration"), not a speed technique. It explicitly kills the in-process-engine performance argument, noting a 3 s run against campaigns of 10²-10³ evaluations that parallelise across cores with no shared state — *"process spawn is noise at that ratio."* It must not be recruited as a performance option.

---

## Tier D: Rejected, With Evidence

Negative findings, each with the measurement that makes it negative.

### 31. Module-major loop interchange — **illegal**
Swapping the nests at `module_registry.c:865-891` **violates Principle 4** outright: galaxy-major ordering *is* the documented processing model (`module_registry.h:127-136`, `module_interface.h:66-75`). It also changes results through four verified cross-galaxy write channels into the central's `HotGas`/`MetalsHotGas`/`EjectedGas`/`MetalsEjectedGas` (`sage_satellite_stripping.c:107-108`; `sage_apply_star_formation_supernova.c:151-152,168-173`; `sage_starburst_physics.h:117-141`; `sage_apply_metal_enrichment.c:111,113`). The central is normally workspace index 0, so under galaxy-major it cools *before* satellites strip into it — and `sage_satellite_stripping.c:40-46` documents that as deliberate SAGE parity. Not an optimisation; a redefinition of the framework contract.

### 32. Static pipeline fusion and runtime JIT — ~2% after the cheap fixes
Decomposing the measured 27.12% by line — **(estimated, and subject to the `-O2` line-attribution caveat above)**: `DEBUG_LOG` at lines 882+846 is 7.43%, the re-scan/mode-filter/`resolved` load ~6.0%, event-state bookkeeping part of the residual, and **the indirect call itself only 1.56%**. The quantified removals available without touching Principle 2 — item 1 (4.15% measured) plus item 2 (4-7% estimated), with item 7 unquantified — come to **roughly 8-11 points** of that ~31%, and possibly more if the residual is mostly bookkeeping. What is left for static fusion is bounded below by the 1.56% indirect call and is **not bounded above on present evidence** — which is exactly why the residual must be measured before anyone spends a Principle-2 concession on it.

Build-time specialisation of the configured pipeline **violates Principle 2** ("selected at runtime from the compiled model set... declared in the input YAML"), and a fallback interpreter does not repair it — it makes the fast path compile-time-bound and the runtime-selectable path second-class, which is exactly what the principle forbids. Generating all fused variants is infeasible arithmetic (18 modules × 3 modes × arbitrary user-named phases). Runtime JIT satisfies Principle 2's letter but adds a compiler to a project whose dependency floor is libyaml + HDF5, breaks Principle 6 provenance, and cannot work on toolchain-free compute nodes. A computed-goto interpreter is fully compliant and worth the same ~1.6%. **Take the compliant version (item 2) and reject the rest: Principle 2 costs almost nothing to keep.**

### 33. Language change (C++ / Rust / array DSL) — targets the 2%
The thing C++ templates buy is item 32, whose prize is ~2%. Rust buys compile-time proof of what item 16 needs, but item 16's blocker is *rewriting* the driver — same cost in either language — not reasoning about aliasing. The generator emits C; the style guide, the warning discipline and the `mpicc` story are all C-shaped. The project already rejected the closest analogue (a JAX/AD backend) with reasons that transfer verbatim: ragged topology-changing trees suit array compilers poorly, the dependency floor rises to a pinned Python stack, the MPI model does not carry over, and two-language physics needs a permanent parity harness. **No** — and the two cheap things one reaches to C++ for (LTO, `static inline` hot predicates) are items 1 and 13.

### 34. SoA and hot/cold splitting as standalone optimisations
Structural fact 5: 391-byte per-FoF working set, IPC 3.43. Galaxy-major ordering already delivers near-perfect temporal locality, and SoA only pays under the illegal module-major nest — it would turn today's seven warm lines into 32 cold streams per galaxy. Separately, the union of the declared property dependencies across the eleven by-galaxy modules (which the generator already emits as comments at `module_system/generated/module_init.c:478-495`) covers nearly all of `GalaxyData`, so a packed per-module view packs almost the whole struct. Good idea, no room for it here.

### 35. Iterative conversion of `build_halo_tree_from_view` — <1%
`build_model.c` is 1.83% *total*, including FoF assembly, `count_fof_subhalos`, `CentralMvir` stamping, segment bookkeeping and `HaloAux` writeback — the recursion overhead is a fraction of a fraction, and the frame is already tiny. Recursion is bounded per forest by `MaxTreeDepth` with a hard `FATAL_ERROR`, and every scratch buffer is already run-persistent, so it is **already Principle 5 compliant**. Disproportionate correctness risk (the nested progenitor/FoF walk order and `HaloAux` flag sequencing are load-bearing) for under 1%.

### 36. `-DNDEBUG` — ≤0.5%, argued against
`CFLAGS` never defines `NDEBUG`, so asserts in `inheritance.c:145-158`, `output_buffer.c:22-25`, `halo_evolution.c` and `galaxy_pool.c` all execute. But `inheritance.c` is only 1.20% total, and these are cheap invariant checks in exactly the code path Principle 7 protects — the archaeology records the `Rvir`/`Vvir` precision bug living in `apply_descendant_properties`. Not the place to buy half a percent.

### 37. Merging tightly-coupled modules
The SF/SN trio (13.7%) and cooling trio (17.1%) would save 4 indirect dispatches and the transport round-trips. But the splits are **not artefacts**: the file headers advertise them as swappable prescriptions, each pair is protected by `module_precedes_in_substep_phase` init guards, and the cooling split is *load-bearing* — `sage_radio_mode_heating` must run **between** budget and apply to suppress `CoolingGas`. `SAGE16-PRESCRIPTION-CLASSIFICATION.md` records the calculate/apply split as an operator-splitting boundary the coupled-rate work intends to dissolve properly. Merging now spends Principle 2 to anticipate a rewrite already scheduled. Bit-identical only if the merged code preserves the exact float round-trips through the transport properties — easy to get subtly wrong.

### 38. Galaxy-pool chunk slack, `phase_event_state` size, `MimicConfig` size, allocation churn
Four things that look bad and are fine. The pool's 36.8% chunk slack is deliberate geometric growth documented at `galaxy_pool.c:12-22` to keep tracked-block count low; it costs resident memory, not cycles. `phase_event_state` is a small static holding a pointer to a heap event buffer plus its hot scalars; the buffer grows to the high-water mark of any single `execute_phase()` call and is never memset — ~460 stores per FoF-snapshot to one warm line on mini-millennium. `MimicConfig` is ~530 KB but its hot scalars are at the front and the giant `ModelParams` array is last. And there is **no allocation churn in the hot path** — every scratch buffer is already run-persistent grow-to-high-water. Stated so nobody "fixes" them.

### 39. `-ffast-math` — disqualified before performance is considered
Enables reassociation and flush-to-zero and removes the NaN/Inf semantics the test framework actively checks for. It would break the very tests that detect the failure modes it introduces. Listed only for completeness.

### 40. HDF5 chunk/cache tuning and output-format changes
Measured negative: switching to the binary writer saves **47 ms (1.4%)**, and the existing 8192-record write buffer already decouples write granularity from tree boundaries. Of output's 3.87%, only 0.51 points is Mimic's own writer code. Do not expect format-level output work to move wall time on this workload. This very likely inverts at Uchuu scale, where open/close counts scale with file count.

---

## Composition and Dependencies

**The cheap frontier.** Items 1-15 are largely independent, additive, and bit-identical. Realistically **~15-25% for days-to-weeks of work**, with no vision change and no baseline regeneration. Items 1 and 2 alone attack the largest single translation unit in the build.

**The multiplier.** Item 16 (thread-per-forest) multiplies with everything in Tier A: the cheap frontier at ~1.2-1.3× followed by 6-9× threading is roughly **7-12× on this machine with byte-identical output — a compounded estimate, both factors estimated rather than measured**. It is the best return-per-unit-risk in the catalogue, and it has **no unmet precondition**: the determinism prerequisite is verified, and the cost is the 4-8 weeks of instancing global state.

**Prerequisite chain.**

```text
1, 2, 7  (logging gate, phase plan, event arming)
    └── independent; do first — they rescale every estimate downstream

19 (SoA + index linkage)  ── enables ──►  17 (batched dispatch + SIMD)
                                                └── enables ──►  29 (GPU)
19 ────────────────────────────────────────────────────────────►  29 (GPU)

16 (thread-per-forest)  ── requires ──►  globals instanced per thread
    │                                     (partly done: pool is an instanced
    │                                      handle API; snapshot driver already
    │                                      holds state in a struct)
    └── independent of 17 and 19; composes multiplicatively with both

18 (snapshot-slab)  ── requires ──►  16's context work + lifting the
                                      snapshot driver's serial-only gate

22 (coupled rate)  ── constrains, BACKWARDS ──►  19 and 17
    (it deletes eight `output: false` transport-scratch properties, so
     transposing them into SoA first is work thrown away; and it adds a
     "rate" dispatch mode, so 17's "batch" mode must be designed as one
     of an OPEN set of modes, not a fourth hard-coded one)
```

**The one backwards dependency is the important one.** The coupled-rate work is scheduled last but constrains the design of items 17 and 19. Those constraints are knowledge rather than code, so they can be honoured in advance — and should be.

**On Principle 4 and dispatch modes.** VISION Principle 4 names exactly three dispatch modes. The **first** work to break that list is not this document — it is step 2, whose brief proposes "a new processing mode or a per-snapshot phase hook". Items 17 and 22 would be the second and third. The amendment therefore belongs at step 2, made **once**, declaring dispatch modes an open metadata-declared set; steps 4 and 6 then extend an already-open set rather than re-amending a closed one. Note the consequence for this document: with the set opened at step 2, the "a third independently-designed extension is worse" argument for deferring item 17 weakens considerably, and the remaining reason to defer is the transport-scratch one below.

---

## Missing Instruments

Three gaps make several estimates above softer than they need to be.

1. **No `--no-output` flag.** `--skip` resumes by skipping existing partitions and `--compress` gzips; neither can time "run without writing". Principle 6 constrains what a *run* must carry, not what a diagnostic mode may omit.
2. **No dispatch counter.** A counter at `module_registry.c:886` dumped at exit would replace the estimated dispatch-cost split with a measurement.
3. **No instruction-level attribution inside `execute_phase`.** The benchmark's own caveat applies: `-O2` folds static helpers into their enclosing function, so per-component totals are safe but per-line attribution inside a TU is not. **Confirm what actually lives at lines 882, 870 and 835 before acting on items 1, 2 or 32.**

---

## Corrections To Earlier Claims

Recorded so they are not re-derived.

- **`DEBUG_LOG` does not evaluate its arguments in the hot path.** Rate limiting is armed for the whole processing phase (`tree_driver.c:512`, `snapshot_driver.c:841`), so after `DEBUG_LOG_MAX_CALLS = 5` per site the macro takes a branch that never references `__VA_ARGS__`. The cost is the bare cross-TU call plus two static loads and two branches — ~1.8 ns (≈6 cycles) averaged over the hot evaluations, consistent with structural fact 1 — not argument marshalling. The fix and its ~4.2% are unchanged; the mechanism is narrower.
- **There is no git subprocess at startup.** Git provenance comes from build-time `git_version.h`, deliberately, so it describes the compiled binary rather than the cwd. The two `popen` calls are `sw_vers` and `md5`.
- **Adaptive substepping is fully implemented and shipped, not stubbed** — and would make the default run dramatically *slower*, not faster.
- **The measured 27.1% in `execute_phase` is not mostly the indirect call.** The call itself is 1.56%; the rest is logging and bookkeeping. This is why fusion and language changes score poorly.
