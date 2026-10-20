# Mimic Performance Implementation Plan (Step 4, Tier A)

**Purpose:** Execute pathway step 4.1 and 4.2 from [`OPTIMISATION-SPECTRUM.md`](OPTIMISATION-SPECTRUM.md): the bit-identical, cheap tier of Mimic's performance options, re-measured on current `main` and taken in value order, with every slice gated on byte-identical galaxy output under both drivers. Thread-per-forest parallelism (step 4.3, spectrum item 16) is not in this plan; the closing slice re-measures and hands it a fresh baseline for its own plan.

**Status:** Planning contract, revision 4 (2026-10-21: revision 3 after three rounds of an independent two-reviewer panel, then re-based on the research-scale measurement of [`MIMIC-PERFORMANCE-REFERENCE.md`](MIMIC-PERFORMANCE-REFERENCE.md)). Not yet executed. Nine Mode B slices on one feature branch.

**Planning baseline:** `5daa24ee` on `main` (v1.5 plus the cut, vertical Shin-Uchuu), clean tree. Every `path:line` anchor below was read at that baseline, and the mini-Millennium measurement was taken on it on 2026-10-20; the mini-Uchuu measurement of 2026-10-21 was taken at `8613bded`, the plan commit, with no code change between (see [Frozen Facts and Evidence](#frozen-facts-and-evidence)). PM binds a run to the SHA-256 of this file's bytes; recheck drift against the anchors before executing a slice, and never silently rebase a contract onto a changed interface.

**Owner:** [Development pathway, step 4](MIMIC-DEVELOPMENT-PATHWAY.md#step-4-performance-next). Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md). Options catalogue: [`OPTIMISATION-SPECTRUM.md`](OPTIMISATION-SPECTRUM.md). Prior profile and method: [`BENCHMARK-SAGE16-MINI-MILLENNIUM.md`](BENCHMARK-SAGE16-MINI-MILLENNIUM.md). Evidence of record for this plan, written by its slices: `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md` (created by Slice 1).

---

## Decisions for the owner

Each row is a decision this plan takes as frozen unless the owner changes it before the run starts. The recommendation is the plan's text below; an alternative chosen here changes the slices named.

| # | Decision | Recommendation | Alternatives and their cost |
|---|---|---|---|
| 1 | **Scope.** The pathway's step 4 has four parts: re-measure, Tier A, thread-per-forest, re-measure. | **This plan is 4.1 and 4.2 plus the build-flag evaluation (spectrum item 13) as an approval-gated final code slice. Thread-per-forest (4.3, spectrum item 16) gets its own plan**, written against the re-measurement this plan's closing slice records. Tier A is days to weeks and bit-identical; threading is four to eight weeks of instancing global state and deserves its own contract and review. | Folding item 16 in: one plan of two to three months whose later slices would be written against a profile the earlier ones change. |
| 2 | **Horizontal driver coverage.** The spectrum was written against the vertical driver only. | **Both drivers, throughout.** Measured today, the horizontal driver has the same shape (module dispatch 28.4% against 30.0% vertical; the logging predicate 3.13% against 3.30%; physics 48.0% against 46.1%), and both reach `execute_phase` through the one `process_halo_evolution` path (`src/core/halo_evolution.c:209-251`). Every slice's identity gate and wall-time record covers both drivers; the vertical-only items (10, 12) say so. | Vertical only: cheaper gates, but the production path for the largest datasets would ship unmeasured. |
| 3 | **The `--no-output` diagnostic flag** (spectrum "Missing Instruments" 1). | **Not built.** Output I/O is measured at 4.31% (vertical) and 2.75% (horizontal) of the default run, so the question the flag answers is already answered to within the sampling floor, and a run mode that omits provenance is a Principle 6 exception for nothing. | Build it: a new CLI flag, documentation and tests for a 4% question. |
| 4 | **Dispatch counters** (spectrum "Missing Instruments" 2). | **Not built as product code.** The callback count is taken once, in Slice 2's scratch build, and recorded; permanent counters would be new global mutable state for step 4.3 to instance, a util-to-core dependency if printed from the run profile, and the panel and this plan agree they are the least valuable item. | A permanent line in the run memory profile: four static counters plus a push-style `run_profile_note_callbacks()` from core at run end, a User Guide and skill entry. |
| 5 | **The bit-identity gate.** Every slice must prove byte-identical galaxy output; the existing `tests-snapshot-global-identity` battery cannot serve, because it refuses a reference commit that carries the snapshot-global feature symbols (`tests/manual/test_snapshot_disabled_identity.py:240`). | **A new manual test, `tests/manual/test_reference_identity.py` with `make tests-reference-identity REFERENCE_COMMIT=<hash>` (Slice 1):** builds the reference commit and HEAD and requires per-`UniqueGalaxyID` byte identity (through the unchanged comparator) for `{halos-only, sage16} × {vertical mini-Millennium, horizontal mini-Millennium} × {fixed, dynamic}` on the real mini-Millennium data both packages ship symlinks to. The existing battery's worktree, reference and output-comparison helpers move into `tests/framework/parity_gate.py`, parameterised where they read battery-local state, and both tests import them (DRY); the gate therefore compares HDF5 objects, attributes and datasets as the battery does, not galaxy fields alone. The same gate serves the build-flag slice and, later, thread-per-forest. | A documented comparator procedure against archived reference runs: no new code, but a machine-local reference directory that nothing verifies and that every later plan would re-invent. |
| 6 | **Spectrum items not taken**, each with the reason measured or verified today. | **Items 3, 4, 6, 7, 8, 9, 11, 14 and 15 are not taken**; the closing slice annotates the spectrum. Item 3 (substep-invariant hoists) and 6 (reionization memo) need a value cached across callbacks, which is mutable module state that step 4.3 would then have to instance per thread and that step 6 restructures anyway; the radio-mode "run constant" claim is false (`black_hole_mass` multiplies first, `sage_radio_mode_heating.c:147-149`). Item 4's exact parts are a duplicated temperature bracket inside a small static helper the compiler likely merges at `-O2` (`cooling_tables.c:57-76`), with no profile evidence it survives; its other parts reassociate. Item 7 changes `module_emit_event`'s behaviour for a contract violation (`module_registry.c:1060-1070`) for an unquantified return. Item 8 (hoisting one store per callback) is unmeasurable against 1.6% noise. Item 9's module-side change breaks a tested no-central contract (`test_unit_sage_apply_infall.c:648-668`) and its core fusions are each under 1%, below the noise. Item 11's allocator share is 0.35% once Slice 6 removes the zero-fill. Item 15's two `popen` calls run at the end of the run, not at startup (`src/core/main.c:508-510`), and the profile's utilities breakout attributes about 0.1% to them (`__wait4_nocancel` 0.06%, `__posix_spawn` 0.02%, `version.c` no self-time). Item 14 (the HDF5 create, close and reopen per partition) measured 2.4% on mini-Millennium but 0.05% (vertical) and 0.02% (horizontal) on the full mini-Uchuu box, because it is paid per partition file, not per halo; the master file's reopen of every partition (`master_hdf5.c:123`) is in any case provenance that rank 0 cannot carry in memory under MPI. | Reinstate any item as a slice; each is independent. |
| 7 | **Build flags** (item 13). | **An approval-gated measurement slice (Slice 8):** six named flag sets measured on both drivers; a set joins the default `CFLAGS` only if the identity gate passes with HEAD built under it against the `-O2` reference and the gate shows a resolved saving. No `make release` target: the decision is what the default build is. Linux identity is not proven by this plan; the pull request's CI run at merge (scientific tier at rtol 1e-3) is the Linux check, and the owner's approval of this slice is where that is weighed. | Leave the default at `-O2` and record the measurements only. |
| 8 | **The vertical output-buffer seed** (items 10 and 12). | **One shared seed function for both drivers** (the horizontal rule: halos plus 5% headroom floored at 1000 rows, moved from `horizontal_driver.c` into `output_buffer.h` as a `static inline`, so the converter suite's standalone link is untouched), `MAXHALOFAC` removed, the whole-buffer zero-fill removed on the evidence that its zeros are never read, and **the FoF workspace seed pinned at its current value** (half the forest's halos, floored at `INITIAL_FOF_HALOS`), because today it is derived from the output seed and would otherwise shrink five-fold as a side effect (Slice 6). | `MAXHALOFAC 2`: keeps the name and the memset, halves the waste instead of removing it. Or let the FoF seed shrink too, with its own measurement. |
| 9 | **Measuring the super-forest.** The pathway records the Shin-Uchuu largest forest's seed as 116.27 GB of a 164.8 GB `sage16` peak and the saving as "not measured". | **Measure it in the closing slice:** `sage16` on `shin-uchuu-lhalo` file 0 alone (the super-forest), `MallocLargeCache=0`, peak RSS before (the recorded figure) and after Slice 6. Minutes to tens of minutes of wall time on the Mac Studio, which holds the dataset on the LaCie. | Leave the projection (about 70 GB) unmeasured. |
| 11 | **The research-scale check and the owner's 10% bar.** A saving under 10% may not be worth the work unless the change also simplifies, hardens or extends the code. | **Measured on the full mini-Uchuu box under both drivers (2026-10-21), the per-galaxy costs the plan targets are scale-free and the per-file costs vanish**: module dispatch 30.8% / 29.6%, the logging predicate 3.7% / 3.6%, the vertical zero-fill 2.2%, against the HDF5 reopen at 0.05% / 0.02%. On that evidence the plan keeps the dispatch pair (Slices 3 and 4, 7 to 12% together, and a dispatch loop that reads like its contract), the buffer slice (Slice 6, 2% vertical plus a halving of vertical memory on large forests and one seed rule instead of two) and the flag measurement (Slice 8, configuration only); it drops the HDF5 slice; and it keeps the exp short-circuit (Slice 7, 0.6%, no other benefit) only because deleting a slice is the owner's call, with this plan's recommendation to delete it. | Delete Slice 7 before the run. Or drop everything under 10% and run only Slices 1, 3, 4 and 8. |
| 12 | **Step 4.3, thread-per-forest, after the MPI measurement.** The pathway estimates it at 6 to 9× and four to eight weeks of instancing global state. | **Measured on the full mini-Uchuu box (2026-10-21): `mpirun` already scales both drivers near-linearly, 13.8× vertical and 13.5× horizontal at 16 ranks (85 to 86% efficiency, 89 to 91% at 4 and 8), and horizontal memory per rank falls from 54 GB to 7 GB.** The plan's recommendation is therefore not to plan 4.3 as performance work: threads would buy the 14% efficiency gap and intra-node balance, not a new order of magnitude, and the pathway's next step after this plan should be the emulator or the coupled-rate spike, with 4.3 left as an unscheduled candidate for a machine where memory rather than cores is the bound (where `input.forest_chunks` already applies). The owner decides at the pathway. | Keep 4.3 as planned, for shared memory on a few-core, small-memory node, or for finer balance than file striping gives the vertical driver. |
| 10 | **A diagnostic slice before the dispatch rewrite.** `-O2` folds static helpers into `execute_phase`, so the 30% attributed to it is not decomposed. | **Slice 2 decomposes it before any dispatch code changes:** a `-fno-inline` profile for per-helper attribution (qualitative: which helpers carry the time, not transferable percentages), a scratch build with `DEBUG_LOG` compiled out for the logging ceiling, the callback count from a scratch counter, and a `code-health` read of `module_registry.c` as the complexity baseline for Slice 4. Slice 5 (dispatch residual) decides go or no-go from its own profile after Slices 3 and 4, not from Slice 2's. | Skip it and implement items 1 and 2 on the spectrum's estimates. |

---

## Outcome and Limits

**Delivered when the plan closes:**

- A reusable bit-identity gate against any ancestor commit (`make tests-reference-identity`), run green by every slice against the planning baseline.
- A `DEBUG_LOG` that costs nothing when debug output is off; a dispatch loop that walks only the modules of the mode it is dispatching; a vertical output buffer seeded like the horizontal one and no longer zero-filled; one exact module-local saving; and whichever build flags proved bit-identical and faster.
- A measured record, `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`: before-and-after wall time and the cost of every slice on both drivers, the dispatch decomposition, the re-profile at the close, the super-forest memory before and after, and the spectrum annotated with what was taken, what was not and why.
- The guides, skills and changelog describing the new gate and the changed memory profile; the pathway advanced to step 4.3.

**Expected speed-up, stated so it can be checked:** 8 to 14% of the default run's wall time on both drivers from Slices 3 to 7 together, with Slice 8 unquantified on top, and the same on the full mini-Uchuu box, where the targeted shares are equal or larger; the per-area figures and their evidence are in [Value by area](#value-by-area). Every number is from one machine and one dataset; the closing slice measures the composite.

**Limits, stated so they are not overclaimed:** no galaxy record changes by a byte, and HDF5 objects, datasets and attributes are identical apart from the named provenance attributes (`RunProperties/Version` build fields, `RunEndTime`, `metadata/version_info.json`); no new dispatch mode, no SoA, no batching, no threading; no scientific change to any module (the module-local slice is arithmetic-identical and gated on the physics baseline as well as the byte gate); measured figures are single-machine (Mac Studio, Apple M3 Ultra) on mini-Millennium, with the super-forest memory the one production-scale number; bit-identity of adopted build flags is proven on macOS arm64 Clang only.

---

## Value by area

Each area, its measured share of the default run today (2026-10-20, commit `5daa24ee`, vertical / horizontal), the expected saving and the evidence. "Measured" is a sampled self-time; "estimated" is derived from measured counts and shares. Run-to-run noise is 1.6% (one standard deviation over 14 repeats; the standard error of a before-after difference of means is about 0.6%), so a saving is proven by the profile fact its slice names, with the wall clock recording it and guarding against regression.

| Area | Share today (V / H) | Expected saving | Evidence | Slices |
|---|---|---|---|---|
| **`DEBUG_LOG` predicate in the hot path** | 3.30% / 3.13% for `is_debug_log_rate_limiting_enabled` alone; the PASS-2 macro site (`module_registry.c:1598`) is a further 2.98% / 2.94% hot line | **3.5 to 6%**; Slice 2's `DEBUG_LOG`-off build measures the ceiling | Measured: hot lines 4 and 5 of both profiles; the macro calls a cross-TU function before any level test (`src/util/error.h:78-95`), every by-galaxy dispatch, even under `-q` | 2, 3 |
| **Mode re-scan and empty loops in `execute_phase`** | 30.0% / 28.4% for `module_registry.c` as a whole, of which the mode-test lines 1586, 1587 and 1591 are 1.83 + 1.35 + 1.11% (V), the galaxy-loop entry 1582 is inside the `:0` residual, and `dispatch_events_range`'s early return is 1.32% | **3 to 6%** from per-call compaction plus skipping the call when nothing is pending; the residual after Slices 3 and 4 is what Slice 5 measures | Measured shares; counted waste: per galaxy per FoF-snapshot 54 of 164 PASS-2 iterations fail the mode test (32.9%), PASS 1 wastes 130 iterations per FoF group, and two of four phases run the per-galaxy loop dispatching nothing (shipped `sage16_mini-millennium.yaml`) | 2, 4, 5 |
| **Vertical tree-load zero-fill and 5× seed** | `__bzero` 2.14% (V), of which `interface.c` pays 2.12 points; 0.28% (H) | **About 2% (V), 0 (H)**; memory: the output seed falls from 5× to 1.05× the forest's halos, 116 GB to about 24 GB on the Shin-Uchuu super-forest (projected, measured in Slice 9) | Measured; the zeros are never read (`output_buffer.c:31-48` grows without zero-filling, readers are bounded by `NumProcessedHalos`: `build_model.c:332`, `binary.c:104`, `hdf5.c:386`); growth is covered by `tests/unit/test_output_buffer.c:216-248`; the horizontal seed rule at `horizontal_driver.c:1499-1514` | 6 |
| **HDF5 create, close, reopen per partition** | `__open` 2.36% / 2.21% on mini-Millennium; 0.05% / 0.02% on mini-Uchuu | **Not taken**: a fixed cost per partition file that vanishes at research scale | Measured on both datasets (the mini-Millennium sampled call graph put 33 of 35 samples under `open_hdf5_output_file` in the single reopen `open()` at `hdf5.c:146`) | — |
| **Exact module-local saving** | `exp` 0.61% / 0.62% plus its stub, paid mostly by `sage_apply_metal_enrichment` (libm payer 0.75% / 0.71%) | **About 0.6%** | Measured payers; `FracZleaveDisk: 0.0` is shipped (`sage16_mini-millennium.yaml:91`) and multiplies the `exp` by zero (`sage_apply_metal_enrichment.c:108-111`); 0.64% / 0.63% payer share on mini-Uchuu | 7 |
| **Build flags** | libm 4.65% / 4.64%; the PLT stubs 0.80% / 0.85%; cross-TU calls the compiler cannot inline | **Unquantified; 3 to 8% claimed** (not additive with Slice 3, whose win is what LTO would also buy) | `CFLAGS = -g -O2` (`Makefile:154`), `EXTRA_CFLAGS`/`EXTRA_LDFLAGS` hooks exist (`:162-172`), no release target | 8 |
| **Physics module arithmetic** (46% / 48%) | the largest share | **Not taken in this plan** | Every change beyond the exact one above is scientific (items 22 to 30) or a cross-callback memo (items 3, 6); see Decision 6 | — |
| **Fixed per-run cost** | 0.50 s of 2.63 s (19%) by the `SubSteps: 1` differential: startup, tree build, inheritance, output | Slice 6 takes about 0.06 s of it | Measured: 0.7136 s at `SubSteps: 1` against 2.6335 s at 10, 0.2133 s per substep; on mini-Uchuu the fixed share is 17% of both drivers and is per-snapshot work, not per-run startup | 6 |

The composite is the product of the per-area ratios, not their sum, and is measured once at the close (Slice 9), not inferred, on mini-Millennium and on the full mini-Uchuu box.

**At research scale** (`sage16` on all 128 mini-Uchuu files, vertical / horizontal, 2026-10-21; the full tables are in [`MIMIC-PERFORMANCE-REFERENCE.md`](MIMIC-PERFORMANCE-REFERENCE.md)): module dispatch 30.82% / 29.60%, the logging predicate 3.71% / 3.56%, `__bzero` 2.16% / 0.52%, the math library 4.53% / 4.45%, output I/O 1.07% / 0.41% with kernel `__open` 0.05% / 0.02%, physics 49.53% / 49.62%. Every per-galaxy share held or rose across a factor of 950 in halos; the per-file shares vanished.

---

## Frozen Facts and Evidence

The numbers this plan designs against, each with its source. A slice that finds one wrong stops rather than silently rebases.

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

The same `execute_phase` lines lead the hot-line list on both datasets; `sage_disk_instability` is the one module whose share grows with box size (4.6% to 7.8%). Under MPI (`USE-MPI=yes`, Open MPI 5.0.9, same box): vertical 2160.4, 607.5, 307.8 and 156.7 s at 1, 4, 8 and 16 ranks (13.8× at 16, 86% efficiency); horizontal 2288.5, 631.0, 322.6 and 169.3 s (13.5×, 85%), rank 0 peak RSS 54.3, 22.4, 9.6 and 7.1 GB.

**Where the spectrum was wrong, verified at the baseline.** (1) `execute_phase` is called from a test: `tests/unit/test_event_buffer_growth.c:176,237,244`. (2) The radio-mode Eddington coefficient is not a run constant (`sage_radio_mode_heating.c:147-149`). (3) The two `popen` calls run at the end of the run (`main.c:508-510`), not at startup. (4) The horizontal driver's seed is a fixed rule, halos plus 5% headroom floored at 1000 rows (`horizontal_driver.c:1499-1514`), not a measured ratio. (5) The master file opens each partition once (`master_hdf5.c:123`). (6) The `sage_starburst_physics.h` `exp` site runs per merger event and, through `sage_starburst_feedback`'s by-galaxy entry (`sage_starburst_feedback.c:277-296`, configured at `sage16_mini-millennium.yaml:51`), per unstable disk per substep, not per star-forming galaxy. (7) Skipping event-dispatch arming changes `module_emit_event`'s violation behaviour (`module_registry.c:1060-1070`). (8) The `EXTRA_LDFLAGS` hook exists (`Makefile:167-172`). (9) No test asserts on the `DEBUG_LOG` suppression message or the `DEBUG_LOG_MAX_CALLS` budget; the only mentions are comments (`horizontal_driver.c:1337`, `tests/integration/test_processing_order.py:384`), and no `DEBUG_LOG` argument in the tree has a side effect.

**The dispatch contract that must survive** (`docs/DEVELOPER-GUIDE.md:505-523`, `src/core/module_registry.h:169-203`): within a phase, every `process_full_halo` module in YAML order; an event a producer emits is delivered immediately, inside the producer's callback, to the subscribed `process_per_event` consumers in YAML order, against the committed rows (`module_registry.c:1117-1150`); when the callback returns, the records it created are committed to the workspace (`:1574`) and then any undelivered event is dispatched (`:1577`); then every `process_by_galaxy` module galaxy-major (for each galaxy, each by-galaxy module in YAML order), skipping rows with `galaxy == NULL` or `Type == 3` (`:1582`); `ws->halos` and `ws->count` re-read at every callback; `ctx->active_event == NULL` for full-halo and by-galaxy calls (`module_interface.h:296-301`); `execute_phase` fails fatally on an unresolved entry when it reaches it (`:1545-1549`, `:1592-1596`); phase configuration read from `ctx->params`, never from a global. Both drivers arm `DEBUG_LOG` rate limiting around their whole processing loop (`vertical_driver.c:650,683`; `horizontal_driver.c:2385,2435`).

**The output-buffer facts.** `load_unit()` seeds `ProcessedHalos` at `MAXHALOFAC (5) × InputTreeNHalos` floored at `MIN_HALO_ARRAY_GROWTH`, derives the FoF workspace seed from it (a tenth of `MaxProcessedHalos`, floored at `INITIAL_FOF_HALOS`, clamped at `MAX_HALO_ARRAY_SIZE`: `src/io/vertical/interface.c:163-173`, documented at `docs/DEVELOPER-GUIDE.md:210`), `memset`s the whole output seed and notes the capacity to the run profile (`interface.c:177-183`; `src/include/constants.h:22-25`); the buffer grows by `HALO_ARRAY_GROWTH_FACTOR` without zero-filling the tail (`src/core/output_buffer.c:31-48`) and `C` is the capacity the buffer reached, seeded or grown (`output_buffer.c:183-189`); `marshal_workspace_to_output_buffer` copies whole structs and tests `Type == 3`, never `galaxy == NULL` on the buffer; every reader of `ProcessedHalos` is bounded by a count. On the default run `C` is 74,345 against `P` 14,648; `docs/USER-GUIDE.md:155,165` show that `C` and say it "normally exceeds `P` by a wide margin". The `galaxy == NULL` tests that matter are on the FoF workspace (`module_registry.c:1582`), a separate allocation (`interface.c:186-189`, growth tail zeroed at `fof_workspace.c:48`, growth logged at `:44`). `docs/DEVELOPER-GUIDE.md:217,231` and `.agents/skills/mimic-diagnostics-and-tooling/SKILL.md:51` document the 5× seed.

**The identity instruments.** `scripts/compare_cross_format_identity.py` default mode: no duplicate ids, same schema and snapshot set, identical id sets, every galaxy field byte for byte; it does not compare HDF5 attributes or non-galaxy datasets; exit 0 identical, 1 differ, 2 input error, 3 comparator failure; its self-test is `tests/scientific/test_compare_cross_format_identity.py`. `tests/framework/parity_gate.py` holds the worktree, build, run and variant helpers (`build_pair` `:719`, `run_logged` `:687`, `dynamic_variant` `:411`, `RunOutput` `:873`, `ParityGate.link_machine_local` `:1613-1622`, which resolves a worktree's `snapshots` link once at creation); `tests/manual/test_snapshot_disabled_identity.py:230-297` holds `resolve_reference`, `runtime_tree_differences`, `worktree_env` and `cached_worktree` (reading battery-local globals: `REFERENCE_COMMIT_DEFAULT`, `has_feature_symbols`, `required_paths`, `RUNTIME_PATHS`, `WORKTREE_ROOT`, `:88-94,159,166,200-224`; `cached_worktree` refuses a worktree whose HEAD is not the given commit, `:289-296`), and `:488-760` its output comparison (`objects_of`, `compare_attributes`, `compare_hdf5_file`, `compare_galaxies`, `compare_metadata_file`, `compare_outputs`: HDF5 objects, attributes with named provenance and path-prefix exclusions at `:541-548`, byte-equal datasets, galaxies through the comparator, `metadata/`). `tests/unit/tools/build_topology_dump.sh:89-104` deliberately excludes `core/output_buffer.c` while linking `io/vertical/interface.c`, so a symbol `load_unit()` needs from `output_buffer.c` would fail that standalone link; `src/` uses no variable-length arrays; a FoF phase may list one module any number of times (the parser sizes each phase by its entry count, `src/core/read_parameter_file.c:1191-1203`, and only `post_snapshot` rejects duplicates, `module_registry.c:651-655`), so `MAX_MODULES` (32, distinct registered modules) bounds nothing per phase. The physics baseline (`models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py`, rtol 1e-6) and the core baseline guard one snapshot each at tolerance; the byte gate is the binding one. `make tests-converter` (about 6 min) compiles util, core and vertical-reader sources standalone through `tests/unit/tools/build_topology_dump.sh:88-109` and must be run for any change under `src/`, `scripts/`, `tests/framework/`, `tests/unit/tools/` or the `Makefile` (`docs/DEVELOPER-GUIDE.md:1301`); CI runs it, and `.github/workflows/ci.yml:3-7` triggers only on pushes and pull requests to `main` and `develop`, so a feature branch alone never runs CI.

**Machine.** Mac Studio, Apple M3 Ultra, 32 cores, 512 GiB, macOS 27.0.1. Real mini-Millennium data for both drivers resolves through the packages' `snapshots` symlinks (`/Volumes/Internal/data/millennium/mini-millennium-lhalo`, 8 files; `.../mini-millennium-horizontal`, 65 HDF5 files, 493 MB). The cut Shin-Uchuu vertical dataset (`shin-uchuu-lhalo`, 361 files) is on the LaCie; file 0 holds the 126,380,326-halo super-forest alone and needs about 165 GB resident for `sage16` today.

---

## Design

### The reference identity gate (Slice 1)

`tests/manual/test_reference_identity.py`, run by `make tests-reference-identity REFERENCE_COMMIT=<hash>`. `REFERENCE_COMMIT` is required (no pinned default: this is an instrument, not a feature's own gate) and must be an ancestor of HEAD. Eight legs: `{halos-only, sage16} × {vertical, horizontal} × {fixed, dynamic}`, each the committed run file (`models/<model>/input/<model>_mini-millennium.yaml` and `..._mini-millennium-horizontal.yaml`) with only `output_directory` redirected, and the dynamic leg the `dynamic_variant()` of it. The reference is built in a worktree cached per `(commit, model, simulation)` under the existing cache root and name shape, verified a clean checkout; HEAD is built in one reusable worktree per pair, detached to HEAD and rebuilt incrementally on every run, so commits do not accumulate worktrees. Every worktree's `snapshots` link must resolve to the same target as the main tree's, asserted on every run, and the runs use the real data: the test **fails, never skips**, when a dataset is absent (naming the path), because a gate whose only defence is the data it verifies must not report success on an empty comparison. Each leg requires both runs to record their model, simulation, commit and timestep scheme in `RunProperties`, the same output files and master links, and comparator exit 0 in default mode. Two optional environment variables, `REFERENCE_IDENTITY_HEAD_EXTRA_CFLAGS` and `REFERENCE_IDENTITY_HEAD_EXTRA_LDFLAGS`, are passed as `EXTRA_CFLAGS`/`EXTRA_LDFLAGS` to the HEAD build only and recorded in the log, so a candidate build flag can be gated against the reference without committing it (Slice 8). Every leg emits one `MIMIC_RESULT:` marker; any FAIL, ERROR, SKIP or NA fails the target; the log is `build/reference_identity.log`; nothing is removed. The helpers the existing battery already has for this move into `tests/framework/parity_gate.py`, behaviour-preserving but parameterised where they read battery-local state (repository and worktree roots, the reference checks, the runtime paths): `runtime_tree_differences`, `worktree_env`, `cached_worktree` (for the reference) plus one new helper for the reusable HEAD worktree, a `resolve_reference` whose feature-symbol checks become the battery's own wrapper, and the output comparison (`objects_of`, `compare_attributes`, `compare_hdf5_file`, `compare_galaxies`, `compare_metadata_file`, `compare_outputs`, with the battery-only pinned-description and empty-list checks left in the battery), which every leg of the new gate runs once (it runs the comparator itself), so objects, attributes (named provenance and path prefixes excepted), datasets and `metadata/` are compared as the battery compares them. Both tests import them, with `make tests-snapshot-global-identity` run green (about a minute) as the proof the move changed nothing. It does not repeat the existing battery's mutation self-check: the comparator's ability to fail is proved by `tests/scientific/test_compare_cross_format_identity.py`.

### `DEBUG_LOG` level-first (Slice 3)

The macro tests whether debug output is enabled before anything else, through an inline test of exported state (today both the level and the rate-limit flag are `static` in `error.c:44,57` behind function calls; the level is tested first, and the rate-limit flag may be exported too so the armed branch makes no call either); when debug output is off, the macro evaluates no function call and none of its arguments. The rate-limit budget (`DEBUG_LOG_MAX_CALLS` per site) is consumed only when a message is emitted; this is internal (the level is set once at startup and raised to INFO only at run end), so with `--debug` the first five messages per site and the suppression line are unchanged and nothing a user sees differs. `VERBOSE_LOG` is out of scope.

### Per-call compaction in `execute_phase` (Slice 4)

At the top of `execute_phase()`, one pass over the phase's `num_modules` entries fills three local arrays sized by a new `MAX_PHASE_ENTRIES` (64, in `module_registry.c`; `MAX_MODULES` bounds distinct modules, not a phase's entries), with the full-halo modules, the by-galaxy modules and the per-event consumers each in YAML order, and the existing unresolved-entry `FATAL_ERROR` raised during that pass (so an invalid direct call fails before its first callback rather than when the entry is reached). A FoF phase (`pre_timestep`, a named substep phase, `post_timestep`; `post_snapshot` does not compact) with more entries than `MAX_PHASE_ENTRIES` is refused at `module_system_init()` with a message naming the phase and the limit, a new, documented startup limit in place of today's unbounded repetition; the compaction pass re-checks the bound defensively, so a direct `execute_phase` call cannot overflow either. PASS 1 walks the full-halo array; the galaxy loop runs only when the by-galaxy array is non-empty and walks only it; `dispatch_events_range()` walks the per-event array (handed through the phase's event-dispatch state, which already holds the phase's config, and cleared by `end_phase_event_dispatch()` so no pointer to the local array outlives the call). The safety dispatch after each full-halo module is called only when `last_dispatched_event < event_count`. No lifecycle, no new struct in `types.h`, no change to `execute_phase`'s signature or to any test, and the per-callback `ctx->active_event = NULL` store stays. The order above is the dispatch contract verbatim; `test_galaxy_major_loop.py`, `test_phase_execution.py`, `test_event_routing.py` and `test_processing_modes.py` are its tests and must pass unchanged. The rewrite is a simplification (the `code-simplifier` discipline: same behaviour, less branching per galaxy), not a layer.

### The vertical output buffer (Slice 6)

`horizontal_output_seed_capacity()` (`horizontal_driver.c:1499-1514`, with its overflow and `MAX_HALO_ARRAY_SIZE` branches) becomes a `static inline` function in `output_buffer.h` that both drivers call (a header, so the converter suite's standalone link of `interface.c` without `output_buffer.c` is unaffected), with its overflow test written inline in the header with the same arithmetic (`horizontal_checked_sum` stays a static helper of the horizontal driver's footprint code); its overflow return is a `FATAL_ERROR` naming the forest in `load_unit()`, as it is in the horizontal caller; its constant is renamed `OUTPUT_SEED_HEADROOM` (one test, `tests/unit/test_horizontal_retention_budget.c:355`, follows the rename) and `MAXHALOFAC` is deleted. The vertical `ProcessedHalos` `memset` is removed, with the comment at `interface.c:180-182` replaced by one stating why no zero is read (the facts above). The FoF workspace seed is pinned at its current value, half the forest's halos floored at `INITIAL_FOF_HALOS` and clamped at `MAX_HALO_ARRAY_SIZE`, as its own named rule in `load_unit()` no longer derived from the output seed, so no FoF group that fits today grows tomorrow. The FoF-workspace zero-fill stays. `run_profile_note_output_buffer` keeps noting the seeded capacity (`C` is a capacity term). Growth on a tree whose output exceeds the seed goes through the existing `myrealloc_cat` path, covered by `test_output_buffer.c:216-248`.

### Exact module-local saving (Slice 7)

`sage_apply_metal_enrichment.c:108-111` and `shared/sage_starburst_physics.h:134-139` take an exact-zero branch when `FracZleaveDisk == 0.0` (true for both signed zeros) that adds what the full expression adds for zero, and evaluate the unchanged expression for every other value, NaN included (NaN passes the inclusive range validation at `src/module_system/parameter_helpers.h:125-137` and must keep its present result). The cooling-table bracket is not touched.

### Build flags (Slice 8)

Six flag sets: `-fno-math-errno`; `-flto`; `-O3`; `-fno-math-errno -flto`; `-fno-math-errno -O3`; `-fno-math-errno -flto -O3` (`-flto` on both compiler and linker). Each is measured with `run_wall.py` on both drivers (builds through `EXTRA_CFLAGS`/`EXTRA_LDFLAGS`) and gated with `make tests-reference-identity` against the `-O2` reference, the HEAD build taking the set through `REFERENCE_IDENTITY_HEAD_EXTRA_CFLAGS`/`_LDFLAGS`. A set is adopted into `Makefile:154` (and `LDFLAGS` for LTO) only if its gate passed and its after mean is below the before mean by more than two standard errors on both drivers; the adopted set is then gated once more as committed. `scripts/benchmark_mimic.sh` gains `EXTRA_LDFLAGS` pass-through, and `scripts/profiling/README.md:14` and the build skill say what the default build is. Linux note: GCC's default `-ffp-contract=fast` can fuse multiply-add across inlining on targets with FMA; CI's `ubuntu-latest` x86-64 default target has none, so CI green is not proof for an aarch64 Linux host, and the changelog says so. LTO also blurs the per-unit profile attribution; Slice 9 notes the confound, and Slice 7's after-profile is the last `-O2` attribution.

---

## Repository Evidence

Anchors a slice relies on, at the planning baseline.

- Logging: `src/util/error.h:78-95` (`DEBUG_LOG`), `:135` (`DEBUG_LOG_MAX_CALLS`); `src/util/error.c:44` (level), `:57` (flag), `:149`, `:167`, `:195-211`, `:382-392` (`log_message`, level test at `:386`). 103 `DEBUG_LOG(` sites in `src/` and `models/`.
- Dispatch: `src/core/module_registry.c:43` (`MAX_MODULES`), `:926-962` (begin/end event dispatch), `:971-1040` (`dispatch_events_range`), `:1117-1150` (`module_emit_event`'s immediate delivery), `:1160-1162` (`workspace_callback_count`), `:1529-1613` (`execute_phase`), `:1615-1666` (`execute_post_snapshot`, untouched), `:1685-1706` (`execute_module_pipeline`), `:777-781` (resolution in `build_pipeline_visitor`); `src/core/module_registry.h:56-62` (`struct PhaseModuleConfig`), `:80-84` (`struct ModulePhaseConfig`), `:169-203` (contract and declaration); `src/include/types.h:212-222` (the phase arrays in `MimicConfig`); `src/core/module_interface.h:296-301`.
- Output buffer: `src/io/vertical/interface.c:163-196`, `:211-218`; `src/include/constants.h:22-25,48-51`; `src/core/output_buffer.c:31-48,183-189`; `src/core/horizontal_driver.c:1499-1514`, `:1751-1757`, `:1844`; `tests/unit/test_horizontal_retention_budget.c:355`; `tests/unit/test_output_buffer.c:216-248`.
- HDF5 writer: `src/io/output/hdf5.c:80-153,171-181`; `src/io/output/util.c:70`; `src/core/vertical_driver.c:357`; `src/core/horizontal_driver.c:1262-1324`; `src/io/output/master_hdf5.c:123,207`; `src/io/output/metadata_hdf5.c:677-681` (`RunEndTime`).
- Modules: `sage_apply_metal_enrichment.c:35,43,108-111`; `shared/sage_starburst_physics.h:134-139`; `sage_starburst_feedback.c:277-296`.
- Build: `Makefile:154,162-172,356,379`; `tests/unit/run_tests.sh:118-122,133-137` (a new `src/core` or `src/util` source must be added to its lists); `tests/unit/tools/build_topology_dump.sh:88-109`; `scripts/benchmark_mimic.sh:180-181,438-441`; `scripts/profiling/README.md:14,64`; `.github/workflows/ci.yml:3-7`.
- Run profile: `src/util/run_profile.h:92-112`; `tests/integration/test_full_pipeline.py:262-282` (the block survives `--quiet`); `tests/integration/test_record_creation.py:101,499-500` (`C` must not fall below the created rows, never its exact value); `docs/USER-GUIDE.md:150-170`.
- Gates and tools: `scripts/profiling/README.md`; `scripts/compare_cross_format_identity.py`; `tests/framework/parity_gate.py`; `tests/manual/test_snapshot_disabled_identity.py`; `Makefile:920-921` (the manual-target idiom); `tests/README.md:65`; `.agents/skills/mimic-validation-and-qa/SKILL.md:46` (the manual gates table).

---

## Execution Structure, Implementation Profiles and Run Preparation

**Mode B, one branch.** Create `feature/performance-tier-a` from `main` at the plan commit. Slices run in order; each is independently gateable (PM ignores batches, and none are declared). `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` is the byte gate of every code slice; the reference worktrees are built once and cached.

**The measurement protocol every code slice follows.** Before the change (at the slice's `before_head`) and after it, on the default build: `python3 scripts/profiling/run_wall.py models/sage16/input/sage16_mini-millennium.yaml 15 <slice>-<before|after> -o archive/performance/<slice>/wall_vertical_<before|after>.json` and the same for `sage16_mini-millennium-horizontal.yaml`; the mean, σ, min and max over repeats 1 to 14 go into the slice's `validation.md` and the acceptance record, with the difference of means and its standard error (SE, the two σ in quadrature over √14). The binding acceptance is the profile fact the slice names; the wall pair must show no regression (after mean at most before mean plus 2 SE) and is recorded as the slice's measured benefit, "unresolved within noise" being an honest result where the expected saving is under 2 SE. Nothing else may run on the machine during a measurement, and the two repeats are taken in the same session. A profile (`sample_runs.sh` 20 repeats plus `attribute.py`, about 3 min) is required where a slice's acceptance criterion names a profile fact.

**The cost side, recorded beside the benefit.** Every code slice records in the acceptance record, next to its wall-time pair, what it cost: lines added and removed (`git diff --stat before_head..HEAD`), the differential `code-health` figures for the files it touched (run by the Developer against `before_head`; the PM re-runs it), and a one-sentence verdict. The standing preference is the simplest implementation that gives an equivalent or better result (KISS) with nothing duplicated (DRY): where two designs measure the same, the smaller diff wins; a slice that adds more complexity than it removes must say in its record why the saving justifies it, and the reviewers are asked to weigh that justification. Slice 2's health read of `module_registry.c` is the baseline Slice 4's delta is measured from.

**The converter suite.** Any slice that changes a file under `src/`, `scripts/`, `tests/framework/`, `tests/unit/tools/` or the `Makefile` runs `make tests-converter` (delegated, about 6 min, exit code recorded) before it is submitted, because its acceptance tests link util, core and vertical-reader sources standalone; `make tests` does not cover that link.

**The acceptance record.** `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md` is created by Slice 1 with the baseline table above and gains one dated section per slice: what changed, the gate result (`build/reference_identity.log` summary), the wall-time pair, the cost line, and any finding. The closing slice adds the re-profile, the super-forest measurement and the spectrum annotation. Nothing outside `docs/dev/` cites it.

**User-facing documentation (binding on every slice).** A slice that changes what a user sees (the memory profile's values or terms, the build flags, a new make target) updates `docs/USER-GUIDE.md` or `docs/DEVELOPER-GUIDE.md` in the same slice and adds its `CHANGELOG.md` line under Unreleased; a slice that changes a documented constant or contract updates the guide lines named in its surface and the skill that teaches it. `make check-docs` after every documentation edit. Nothing outside `docs/dev/` may reference this plan or the acceptance record.

**Developer seats** (`Recommended Developer:` per slice; full model ids for the PM): `claude-sonnet-5-5` for the tightly specified slices, `claude-opus-5-5` for the dispatch and buffer slices and the diagnostic read. Efforts are stated per slice.

**Reviewers.** The drift auditor and code reviewers are the panel the launcher names; the slices marked `Independent audit required: yes` (3, 4, 5, 6 and 8) must have both reviews fresh at the final commit.

---

## Resource Profile

| Slice | Wall time (Developer) | Machine time | Data |
|---|---|---|---|
| 1 | hours | gate about 2 min per run (eight legs, cached reference builds) plus the existing battery once (about a minute); converter suite 6 min | real mini-Millennium, both layouts |
| 2 | hours | three profiles of about 3 min each | default pair |
| 3, 7 | hours each | gate, suite, converter suite: under 15 min | default pair |
| 4, 6 | a day each | gate, suite, converter suite, `tests-distributed` (minutes), fuzzer (minutes, Slice 4) | default pair, `forest_blocks` fixture |
| 5 | hours, or none | one profile, gate, suite | default pair |
| 8 | hours | six builds, six wall pairs, six gate runs: about an hour | default pair |
| 9 | a day of attention, about 12 h of machine time | two profiles; the mini-Uchuu wall pair (3 repeats per driver, about 36 min each); the cross-format gate (hours); the super-forest run (minutes to tens of minutes, about 165 GB then about 75 GB resident) | mini-Uchuu, both layouts; micro-Uchuu ASCII pair; `shin-uchuu-lhalo` file 0 |

---

## Slice 1: The reference identity gate

### Intended Change
- Add `tests/manual/test_reference_identity.py` and the `make tests-reference-identity` target as designed under [The reference identity gate](#the-reference-identity-gate-slice-1): eight legs of per-`UniqueGalaxyID` byte identity between a required `REFERENCE_COMMIT` and HEAD on the real mini-Millennium data, both drivers, both models, fixed and dynamic, through the unchanged comparator and the shared parity-gate helpers.
- Move the worktree, reference and output-comparison helpers from `tests/manual/test_snapshot_disabled_identity.py` into `tests/framework/parity_gate.py`, behaviour-preserving and parameterised where they read battery-local state, add one helper for the reusable HEAD worktree, and import them from both tests.
- Create `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md` with the baseline measurement table from this plan and the gate's first green run.
- Document the target in `tests/README.md` (the manual-tests paragraph), the Makefile help, the Developer Guide's Testing section, the validation skill's manual-gates table and the changelog.

### Acceptance Criteria
- [ ] `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` exits 0 at the slice's commit, emitting exactly eight `MIMIC_RESULT: PASS` leg markers and no FAIL, ERROR, SKIP or NA, with `build/reference_identity.log` written.
- [ ] Running without `REFERENCE_COMMIT`, with a commit that is not an ancestor of HEAD, or with a runtime tree that has uncommitted edits under `src/`, `models/`, `simulations/`, `scripts/` or `Makefile` exits non-zero before any build, naming the reason.
- [ ] With the vertical or horizontal `snapshots` link pointed at an empty directory, the affected legs FAIL naming the path; no leg SKIPs; and a worktree whose `snapshots` link does not resolve to the main tree's target fails the run naming both targets.
- [ ] Each leg asserts both runs' `RunProperties` model, simulation, commit and `TimestepScheme` before comparing, then runs the moved output comparison once (galaxies through `scripts/compare_cross_format_identity.py` in default mode requiring exit 0, then objects, attributes, datasets and `metadata/`), permitting only the named provenance attributes and path prefixes; the battery-only pinned-description and empty-list checks stay in the battery.
- [ ] Reference worktrees are cached per `(commit, model, simulation)` under `output/snapshot-global-identity/worktrees/` with the existing name shape and verified clean; HEAD uses one reusable worktree per pair, detached to HEAD on every run; a HEAD build under `REFERENCE_IDENTITY_HEAD_EXTRA_CFLAGS`/`_LDFLAGS` is a clean rebuild and the log names the flags; nothing is removed by the test.
- [ ] The moved helpers exist once each, in `tests/framework/parity_gate.py`, with the battery's feature-symbol checks kept in a wrapper in the battery; `make tests-snapshot-global-identity` exits 0 after the move.
- [ ] `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md` exists with the baseline table and the first gate run's summary; `tests/README.md`, `make help`, the Developer Guide's Testing section, the validation skill and the changelog describe the target.
- [ ] `make tests-converter` exits 0 (the framework changed).
- Inputs: the two real datasets resolve on the Mac Studio; the comparator is unchanged.
- Behaviour that must not change: `scripts/compare_cross_format_identity.py`; the existing battery's legs, checks and outputs.

### Authorized Surface
- Files allowed to change:
  - `tests/manual/test_reference_identity.py` (new file)
  - `tests/framework/parity_gate.py`
  - `tests/manual/test_snapshot_disabled_identity.py`
  - `Makefile`
  - `tests/README.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md` (new file)
- Functions/classes/components allowed to change: the new test module; the moved helpers (relocation, parameterisation of battery-local state, the battery's wrapper and imports) and one new HEAD-worktree helper; the Makefile's `.PHONY` list, help text and one new target; the manual-tests paragraph of `tests/README.md`; one sentence under Testing in the Developer Guide; one row in the validation skill's manual-gates table; one changelog line.
- Tests allowed or expected to change: the new manual test; the existing battery's imports and its wrapper around the moved reference check.

### Explicit Non-Goals
- No change to the comparator; no behavioural change to the existing battery; no mutation self-check; no fixture legs; no CI wiring; no C change.

### Risk Flags
- Risky surfaces touched: none (a manual test, a framework relocation and a Makefile target).
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: the new manual test; the battery's imports and wrapper.
- Commands to run: `make tests-reference-identity REFERENCE_COMMIT=5daa24ee`; `make tests-snapshot-global-identity`; the negative cases above by hand with their exit codes recorded; `make tests-converter`; `make check-docs`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read `build/reference_identity.log` and confirm eight legs ran on the real data (the `sage16` runs' profile blocks report `P` of 14,648 vertical and 37,425 horizontal, as in the baseline table).

### Rollback Path
- Delete the new files, move the helpers back and revert the Makefile and documentation edits; nothing else depends on them.

---

## Slice 2: Dispatch cost decomposition and health read (diagnostic)

### Intended Change
- Produce the measurements the spectrum's "Missing Instruments" 2 and 3 ask for, in scratch worktrees under `output/` or `archive/`, changing no tracked file but the acceptance record: (a) a profile of the default run built with `EXTRA_CFLAGS="-fno-inline"` so `begin_/end_phase_event_dispatch`, `dispatch_events_range`, `enter_/leave_callback`, `workspace_callback_count`, `commit_created_records` and the `DEBUG_LOG` sites attribute separately, read as a ranking of which helpers carry the time, not as percentages transferable to the `-O2` build; (b) a wall-time pair for a scratch build in which `DEBUG_LOG` expands to nothing and nothing else differs, giving the ceiling Slice 3 can reach; (c) the number of full-halo, per-event and by-galaxy callbacks in the default run from a separate, untimed instrumented build, so per-call costs can be stated in nanoseconds without a counter in either timed build; (d) a `code-health` read of `src/core/module_registry.c` (complexity and duplication) as the baseline Slice 4's differential is measured from.
- Record a qualitative decomposition of the 30% `module_registry.c` share (logging macro, mode re-scan, per-callback bookkeeping, indirect call) and the per-call cost.

### Acceptance Criteria
- [ ] The acceptance record carries the `-fno-inline` per-function table for `module_registry.c`, the `DEBUG_LOG`-off wall pair (mean ± σ, n = 14, both drivers, with its SE), the three callback counts, and the health read's figures for `module_registry.c`.
- [ ] The record states the decomposition and the per-callback cost (default run wall divided by the by-galaxy count, and the `DEBUG_LOG`-off saving divided by the same count), each labelled measured or estimated.
- [ ] `git status --short` shows no change outside `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`; scratch builds live under `output/` or `archive/` only; the default build is restored (`make` and `make check-generated` exit 0).
- Inputs: the default pair; the committed profiling harness.
- Behaviour that must not change: anything tracked.

### Authorized Surface
- Files allowed to change:
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`
- Functions/classes/components allowed to change: none.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No optimisation, no refactor, no tracked edit; scratch worktrees are not committed; no go/no-go for Slice 5 (that is Slice 5's own measurement).

### Risk Flags
- Risky surfaces touched: none.
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-opus-5-5, medium effort.

### Validation Plan
- Tests to add/update: none.
- Commands to run: `scripts/profiling/` per its README in each scratch worktree; `code-health` skill in absolute mode over `src/core/module_registry.c`; `make` and `make check-generated` on the main tree afterwards.
- Lint (differential, via the `lint` skill): not required (no linted file changes).
- Manual checks: the `-fno-inline` profile's `mapped_fraction` is 1.0 and its `Unattributed` share under 1%.

### Rollback Path
- None needed; the record section is deleted if the slice is abandoned.

---

## Slice 3: `DEBUG_LOG` tests the level first

### Intended Change
- Make `DEBUG_LOG` free when debug output is off, as designed under [`DEBUG_LOG` level-first](#debug_log-level-first-slice-3): an inline predicate over an exported variable tested first; no call and no argument evaluation when it is false; the per-site rate-limit budget consumed only when a message is emitted.
- Add a C unit test of the macro's two contracts.

### Acceptance Criteria
- [ ] With the log level above DEBUG, a `DEBUG_LOG` whose argument has a side effect leaves the side effect unexecuted and calls no function in `error.c` (unit test with a counter argument and the log output redirected through `set_log_output`).
- [ ] With the level at DEBUG and rate limiting armed, a site emits its first `DEBUG_LOG_MAX_CALLS` messages and then exactly one suppression line, as today, and the same site called first with the level above DEBUG and then at DEBUG still emits its full budget; with rate limiting disarmed every call emits (unit test counting lines in the redirected output).
- [ ] A 20-sample profile of the default vertical run after the slice attributes no self-time to `is_debug_log_rate_limiting_enabled` (the symbol is absent from the hot-line table and `error.c`'s share is under 0.5%).
- [ ] `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` exits 0; `make tests` passes; `make tests-converter` passes; `tests/unit/run_tests.sh` builds every unit test (a new exported symbol is visible to the runner's `UTIL_SRCS`).
- [ ] The wall-time pair is recorded with its SE; the after mean is at most the before mean plus 2 SE, and the saving is compared with Slice 2's `DEBUG_LOG`-off ceiling.
- [ ] The acceptance record's cost line for this slice is filled (lines added and removed, differential `code-health` for the files touched, the wall pair, one-sentence verdict).
- Behaviour that must not change: `INFO_LOG`, `WARNING_LOG`, `ERROR_LOG`, `FATAL_ERROR`, `VERBOSE_LOG`, the I/O logging macros, `log_message`'s formatting, and the text of every debug message.

### Authorized Surface
- Files allowed to change:
  - `src/util/error.h`
  - `src/util/error.c`
  - `tests/unit/test_debug_log_gate.c` (new file)
  - `docs/DEVELOPER-GUIDE.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`
- Functions/classes/components allowed to change: the `DEBUG_LOG` macro, the level and rate-limit state and their accessors in `error.c`, one new inline predicate; the Developer Guide's logging paragraph if it describes the predicate.
- Tests allowed or expected to change: the new unit test.

### Explicit Non-Goals
- No change to any `DEBUG_LOG` call site; no change to `VERBOSE_LOG`; no change to where the drivers arm rate limiting; no user-visible claim about the budget in the changelog (the change is internal).

### Risk Flags
- Risky surfaces touched: a header used by 103 call sites (global state exposed through an extern).
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: `tests/unit/test_debug_log_gate.c`.
- Commands to run: the measurement protocol; the 20-sample profile; `make tests-reference-identity REFERENCE_COMMIT=5daa24ee`; `make tests summary`; `make tests-converter`; `./mimic --debug models/sage16/input/sage16_mini-millennium.yaml 2>&1 | grep -c "suppressed"` compared with the same count at `before_head`; `make check-docs`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: reading `src/util/error.h` shows the macro tests the level before touching any rate-limit state and makes no function call on the disabled path.

### Rollback Path
- Revert the slice commit; the macro returns to the cross-TU call.

---

## Slice 4: Per-call compaction in `execute_phase`

### Intended Change
- Replace the per-galaxy mode re-scan with one compaction pass per `execute_phase` call as designed under [Per-call compaction](#per-call-compaction-in-execute_phase-slice-4): three local per-mode arrays, the galaxy loop entered only when a by-galaxy module exists, event delivery over the per-event array, and the safety dispatch called only when an event is pending; simplify `execute_phase` and `dispatch_events_range` in the process.

### Acceptance Criteria
- [ ] `execute_phase` performs no `processing_mode` comparison inside the galaxy loop: the per-mode arrays (sized `MAX_PHASE_ENTRIES`) are filled once per call, PASS 1 iterates only full-halo modules, the galaxy loop is entered only when the phase has a by-galaxy module and iterates only by-galaxy modules, and `dispatch_events_range` iterates only per-event consumers; `end_phase_event_dispatch()` clears the per-event array pointer.
- [ ] A FoF phase with more than `MAX_PHASE_ENTRIES` entries is refused at `module_system_init()` naming the phase and the limit, the compaction pass re-checks the bound, and a phase with up to that many, including a module repeated, runs every entry (verified by reading both checks); the limit is stated in the Developer Guide's phase-configuration section and the changelog.
- [ ] The safety dispatch after a full-halo module runs only when `last_dispatched_event < event_count`, and every emitted event is still delivered exactly once in emission order (`tests/unit/test_event_buffer_growth.c` and `tests/integration/test_event_routing.py` unchanged and passing).
- [ ] Dispatch order is unchanged: `tests/integration/test_galaxy_major_loop.py`, `test_phase_execution.py`, `test_event_routing.py`, `test_processing_modes.py`, `test_record_creation.py` and `tests/unit/test_record_creation.c`, `test_event_buffer_growth.c`, `test_snapshot_collectives.c` pass with no edit; `execute_phase`'s signature is unchanged.
- [ ] An unresolved entry in a non-empty phase is still a `FATAL_ERROR` (the existing guard, now raised in the compaction pass before the phase's first callback).
- [ ] `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` exits 0; `make tests` passes; `make tests-converter` passes; `make tests-distributed` passes; `python3 scripts/fuzz_pipeline.py --runs 200 --sampling valid-subset` reports no failure.
- [ ] A 20-sample profile after the slice shows `module_registry.c`'s self-time share reduced from the Slice 3 after-profile, with the former mode-test lines (`:1586`, `:1587`, `:1591` at the baseline) under 0.3% combined in the attributed line list; the wall-time pair is recorded with its SE and the after mean is at most the before mean plus 2 SE.
- [ ] `module_registry.h`'s `execute_phase` documentation still states the driver-neutral contract (rows re-read per callback, created records committed before the next module, immediate event delivery) and describes the compaction in one sentence.
- [ ] The acceptance record's cost line for this slice is filled (lines added and removed, differential `code-health` against Slice 2's baseline, the wall pair, one-sentence verdict); the registry's complexity does not rise.
- Behaviour that must not change: `execute_post_snapshot`; `module_create_record()` and the commit; `module_emit_event`'s rules and immediate delivery; `execute_module_pipeline`'s phase sequence; the per-callback `ctx->active_event = NULL` store.

### Authorized Surface
- Files allowed to change:
  - `src/core/module_registry.c`
  - `src/core/module_registry.h`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`
- Functions/classes/components allowed to change: `execute_phase`, `dispatch_events_range`, `begin_/end_phase_event_dispatch` and the `phase_event_state` fields they set; the new `MAX_PHASE_ENTRIES` constant and its checks in `module_system_init()` and the compaction pass; the `execute_phase` doc comment; one sentence in the Developer Guide's phase-configuration section; the architecture skill's data-flow lines for `execute_phase`.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No change to the dispatch order, to which rows are skipped, to event semantics, to `execute_post_snapshot`, to the module ABI, to `struct ModuleContext`, to `types.h` or to any test; no caching of `ws->halos` across callbacks; no init-time plan, heap scratch or new lifecycle; no variable-length array; no new dispatch mode.

### Risk Flags
- Risky surfaces touched: the core dispatch engine shared by both drivers; global state (`phase_event_state`).
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: none (the existing dispatch tests are the contract).
- Commands to run: the measurement protocol and a 20-sample profile; `make tests-reference-identity REFERENCE_COMMIT=5daa24ee`; `make tests summary`; `make tests-converter`; `make tests-distributed` (then `make`); `python3 scripts/fuzz_pipeline.py --runs 200 --sampling valid-subset`; `make check-docs`; `make check-format`; differential `code-health` against `before_head`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the rewritten `execute_phase` against the dispatch contract line by line.

### Rollback Path
- Revert the slice commit; the per-galaxy re-scan returns.

---

## Slice 5: Dispatch residual (conditional on its own measurement)

### Intended Change
- First measure: a 20-sample profile of the default run at this slice's `before_head` (after Slices 3 and 4), decomposed with Slice 2's ranking. If at least 2% of the run is attributable to per-callback bookkeeping in `module_registry.c` that can be removed without contract change (candidates: the checked narrowing in `workspace_callback_count()` recomputed per full-halo module rather than once per phase and after each commit; the `record_creation.ctx/ws` stores per module; `enter_/leave_callback` layout), remove it, inside `module_registry.c` only. Otherwise the slice closes with the profile in the record and no code change.

### Acceptance Criteria
- [ ] The acceptance record holds this slice's `before_head` profile and the go or no-go with the 2% threshold applied to named lines.
- [ ] Either the verdict was go and every removal is one the record named, with Slice 4's dispatch contract tests passing unchanged, or the verdict was no-go and the slice changed no file but the acceptance record.
- [ ] If code changed: `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` exits 0, `make tests` passes, `make tests-converter` passes, `make tests-distributed` passes, the wall-time pair is recorded with its SE and the after mean is at most the before mean plus 2 SE, and the cost line is filled.
- Behaviour that must not change: everything Slice 4 lists; `module_registry_running_callback()` must still report the running kind and module during every callback.

### Authorized Surface
- Files allowed to change:
  - `src/core/module_registry.c`
  - `src/core/module_registry.h`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`
- Functions/classes/components allowed to change: the dispatch helpers the record named.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No removal the record did not name; no change to the record-creation gate's correctness.

### Risk Flags
- Risky surfaces touched: the core dispatch engine.
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-opus-5-5, medium effort.

### Validation Plan
- Tests to add/update: none.
- Commands to run: the 20-sample profile first; then, if code changes, as Slice 4 without the fuzzer.
- Lint (differential, via the `lint` skill): required unless no file changed.
- Manual checks: the removed bookkeeping against the record's list.

### Rollback Path
- Revert the slice commit.

---

## Slice 6: The vertical output buffer's seed and zero-fill

### Intended Change
- Share the output-buffer seed rule between the drivers, seed the vertical `ProcessedHalos` buffer by it, remove its zero-fill, pin the FoF workspace seed at its current value, and delete `MAXHALOFAC`, as designed under [The vertical output buffer](#the-vertical-output-buffer-slice-6); update the guide lines, the User Guide's profile example and the diagnostics skill sentence that document the 5× seed.

### Acceptance Criteria
- [ ] One `static inline` seed function in `output_buffer.h` is called by both `load_unit()` and the horizontal driver, with the horizontal driver's seed arithmetic and overflow handling unchanged and `load_unit()` treating the overflow return as a `FATAL_ERROR` naming the forest (`tests/unit/test_horizontal_retention_budget.c` passes with only the constant's name changed); `MAXHALOFAC` and `HORIZONTAL_OUTPUT_SEED_HEADROOM` no longer exist and `OUTPUT_SEED_HEADROOM` does.
- [ ] The FoF workspace seed in `load_unit()` equals, for every tree, the value it had at `before_head` (half the forest's halos floored at `INITIAL_FOF_HALOS`, clamped), as its own named rule; the default vertical run logs no new `Growing FoF workspace` line (counted against `before_head`).
- [ ] The `memset` of `ProcessedHalos` in `load_unit()` is gone and a comment at the allocation states why no zero is read, naming the growth path and the bounded readers; the FoF-workspace zero-fill is unchanged.
- [ ] The default vertical run's profile block reports `C` below 74,345 and at least `P` (expected 15,869 for the largest tree unless a tree grew), with `P` unchanged at 14,648; the value is recorded.
- [ ] `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` exits 0; `make tests` passes; `make tests-converter` passes; `make tests-distributed` passes.
- [ ] A 20-sample profile of the default vertical run attributes under 0.5% to `__bzero` (the FoF-workspace zero-fill remains); the wall-time pair is recorded with its SE for both drivers and the after means are at most the before means plus 2 SE.
- [ ] `docs/DEVELOPER-GUIDE.md` lines 210, 217 and 231, `docs/USER-GUIDE.md`'s profile example and its `C` row, and the diagnostics skill no longer describe a 5× seed or a FoF seed derived from it, and the changelog states the new seed and that `C` on vertical runs now tracks the forest size.
- [ ] The acceptance record's cost line for this slice is filled (lines added and removed, differential `code-health` for the files touched, the wall pair, one-sentence verdict).
- Behaviour that must not change: output record order and content; `run_profile_note_output_buffer` semantics; the horizontal driver's seed value; the FoF workspace seed value.

### Authorized Surface
- Files allowed to change:
  - `src/include/constants.h`
  - `src/io/vertical/interface.c`
  - `src/core/output_buffer.h`
  - `src/core/horizontal_driver.c`
  - `tests/unit/test_horizontal_retention_budget.c`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/USER-GUIDE.md`
  - `.agents/skills/mimic-diagnostics-and-tooling/SKILL.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`
- Functions/classes/components allowed to change: `load_unit()`; the constants block; `horizontal_output_seed_capacity()` (moved into the header as the shared function) and its one caller and the comment at `horizontal_driver.c:1751-1757`; the test's constant name; the named documentation lines.
- Tests allowed or expected to change: `tests/unit/test_horizontal_retention_budget.c` (constant name only).

### Explicit Non-Goals
- No change to the horizontal driver's buffer `memset` (`horizontal_driver.c:1844`), to the FoF workspace's seed value or zero-fill, to `HaloAux`, to buffer reuse across trees (spectrum item 11) or to `MAX_HALO_ARRAY_SIZE`.

### Risk Flags
- Risky surfaces touched: a shared constant and the vertical driver's memory profile (every vertical package).
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-opus-5-5, high effort.

### Validation Plan
- Tests to add/update: the constant rename in one test.
- Commands to run: the measurement protocol and a 20-sample profile; `make tests-reference-identity REFERENCE_COMMIT=5daa24ee`; `make tests summary`; `make tests-converter`; `make tests-distributed` (then `make`); `./mimic models/sage16/input/sage16_mini-millennium.yaml 2>&1 | grep -c "Growing FoF workspace"` at `before_head` and after; `make check-docs`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: `rg -n "MAXHALOFAC|HORIZONTAL_OUTPUT_SEED_HEADROOM" .` finds nothing outside `docs/dev/`, `archive/` and the changelog line that records the removal.

### Rollback Path
- Revert the slice commit; the 5× seed, the derived FoF seed and the memset return.

---

## Slice 7: Exact module-local saving

### Intended Change
- Take the exact-zero branch for `FracZleaveDisk` at both sites, as designed under [Exact module-local saving](#exact-module-local-saving-slice-7).

### Acceptance Criteria
- [ ] `sage_apply_metal_enrichment.c` and `shared/sage_starburst_physics.h` evaluate no `exp()` when `FracZleaveDisk == 0.0` and evaluate the unchanged expression for every other value, NaN included; the zero branch's additions are the ones the full expression makes for zero.
- [ ] `models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py` passes at the strict local rtol (1e-6), and `make tests-reference-identity REFERENCE_COMMIT=5daa24ee` exits 0.
- [ ] The two modules' own unit tests and the model package's `_tests/` pass unchanged; `make validate-modules` and `make lint-parameters` exit 0.
- [ ] A 20-sample profile shows `sage_apply_metal_enrichment`'s libm payer share under 0.2%; the wall-time pair is recorded with its SE.
- [ ] The acceptance record's cost line for this slice is filled (lines added and removed, differential `code-health` for the files touched, the wall pair, one-sentence verdict).
- Behaviour that must not change: results for any `FracZleaveDisk` other than zero; every `SAGE parity:` note in the files.

### Authorized Surface
- Files allowed to change:
  - `models/sage16/modules/sage_apply_metal_enrichment/sage_apply_metal_enrichment.c`
  - `models/sage16/shared/sage_starburst_physics.h`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`
- Functions/classes/components allowed to change: the two enrichment sites.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No change to the cooling tables, no reciprocal, no reassociation, no hoisting across callbacks (items 3, 6), no change to `module_info.yaml`, no change to any other module.

### Risk Flags
- Risky surfaces touched: none (module-local, arithmetic-identical).
- Approval needed before implementation: no
- Difficulty: easy
- Recommended Developer: claude-sonnet-5-5, medium effort.

### Validation Plan
- Tests to add/update: none.
- Commands to run: the measurement protocol and a 20-sample profile; `make tests-reference-identity REFERENCE_COMMIT=5daa24ee`; `make tests summary`; `make validate-modules`; `make lint-parameters`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: diff the two sites against `before_head` to confirm the non-zero expression is textually the same.

### Rollback Path
- Revert the slice commit.

---

## Slice 8: Build flags, measured and gated

### Intended Change
- Measure the six flag sets as designed under [Build flags](#build-flags-slice-8), adopt into the default `CFLAGS` (and `LDFLAGS` for LTO) only a set that passed the identity gate and showed a resolved saving, give `scripts/benchmark_mimic.sh` an `EXTRA_LDFLAGS` pass-through, and record every measurement whether or not a set is adopted.

### Acceptance Criteria
- [ ] The acceptance record holds, for each of the six named flag sets, the wall-time pair on both drivers with its SE and the gate's exit status with HEAD built under that set through `REFERENCE_IDENTITY_HEAD_EXTRA_CFLAGS`/`_LDFLAGS`.
- [ ] A set is in the default `CFLAGS`/`LDFLAGS` only if its gate run passed and its after mean is below the before mean by more than 2 SE on both drivers; the adopted set is gated again from the committed `CFLAGS` with no extra flags, and `make tests` passes under it; the changelog names each adopted flag with its measured saving and the Linux FMA caveat, and each rejected set with the reason.
- [ ] `scripts/benchmark_mimic.sh` passes `EXTRA_LDFLAGS` through to `make` and records it in its JSON, with `--help` updated.
- [ ] `make check-format`, `make check-generated`, `make tests` and `make tests-converter` pass at the slice's commit; `tests/unit/run_tests.sh` still builds unit tests at `-O0`; `scripts/profiling/README.md`, the User Guide's build section and the build skill state the default build.
- [ ] The acceptance record's cost line for this slice is filled (lines added and removed, the wall pairs, one-sentence verdict).
- Behaviour that must not change: `EXTRA_CFLAGS`/`EXTRA_LDFLAGS` as caller hooks; the warning set; `-g`.

### Authorized Surface
- Files allowed to change:
  - `Makefile`
  - `scripts/benchmark_mimic.sh`
  - `scripts/profiling/README.md`
  - `docs/USER-GUIDE.md`
  - `.agents/skills/mimic-build-and-env/SKILL.md`
  - `.agents/skills/mimic-diagnostics-and-tooling/SKILL.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`
- Functions/classes/components allowed to change: the `CFLAGS`/`LDFLAGS` definitions and the build-config stamp; the benchmark script's environment handling and help; the build documentation.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No `-ffast-math`, no `-march=native` in the default, no new `make release` target, no CI workflow change, no push, no change to the unit-test build flags.

### Risk Flags
- Risky surfaces touched: release/deployment configuration (the default build for every user and CI).
- Approval needed before implementation: yes
- Independent audit required: yes
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: none.
- Commands to run: per flag set, `make clean && make EXTRA_CFLAGS=... EXTRA_LDFLAGS=...` for the measurement and `REFERENCE_IDENTITY_HEAD_EXTRA_CFLAGS=... REFERENCE_IDENTITY_HEAD_EXTRA_LDFLAGS=... make tests-reference-identity REFERENCE_COMMIT=5daa24ee` for the gate; after adoption, `make clean && make`, `make tests-reference-identity REFERENCE_COMMIT=5daa24ee`, `make tests summary`, `make tests-converter`; `make check-generated`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: `make info` shows the adopted flags.

### Rollback Path
- Revert the slice commit; the default returns to `-g -O2`.

---

## Slice 9: Close out, re-measure and hand over

### Intended Change
- Re-profile both drivers with the committed harness (20 samples, 15 wall repeats) and record the composite against the baseline table; measure the research-scale composite with 3 wall repeats per driver of `sage16` on all 128 mini-Uchuu files (the run files of `MIMIC-PERFORMANCE-REFERENCE.md`) against that document's 2167.0 s and 2267.1 s, and its memory profile against that document's; noting, if LTO was adopted, that per-unit attribution is blurred and that Slice 7's after-profile is the last `-O2` attribution; measure `sage16` on `shin-uchuu-lhalo` file 0 alone with `MallocLargeCache=0` (peak RSS and wall) against the recorded 164.8 GB; run the micro-Uchuu ASCII cross-format identity gate (`make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-scientific`) as the real-data evidence for the shared dispatch and writer changes; annotate `OPTIMISATION-SPECTRUM.md` (status, each Tier A item's outcome, the items not taken and why, the measurements superseded); advance the pathway to step 4.3 with the new baseline; consolidate the changelog's Unreleased entries into one performance entry with measured claims and limits; sweep the skills the slices touched.

### Acceptance Criteria
- [ ] The acceptance record holds the closing profile tables for both drivers (components, hot lines, libm payers) and the wall-time composite (mean ± σ, n = 14, with SE) against the baseline, with the per-slice ratios multiplied and compared with the composite.
- [ ] The acceptance record holds the mini-Uchuu composite for both drivers (mean ± σ of 3 repeats against 2167.0 s and 2267.1 s, with the peak RSS and `C` against 1.82 GB and 3,853,400 vertical, 55 to 63 GB and 41,788,163 horizontal), and `MIMIC-PERFORMANCE-REFERENCE.md` gains a dated after-the-plan section stating them.
- [ ] The super-forest measurement is recorded (peak RSS, wall, `C` and `P`) and the pathway's residual on the vertical seed is rewritten with the measured figure.
- [ ] The cross-format gate's four legs PASS, recorded with their log path.
- [ ] `OPTIMISATION-SPECTRUM.md` carries a dated outcome section and per-item outcome notes for items 1 to 15; `MIMIC-DEVELOPMENT-PATHWAY.md` shows step 4.1 and 4.2 done, 4.3 next with the new baseline, and a changelog row; `CHANGELOG.md` Unreleased has one performance entry stating measured savings per driver, the memory profile change, the new gate, and what is not claimed.
- [ ] `make check-docs`, `make check-format`, `make check-generated` and `make tests` pass; the skills the slices edited are consistent with the code (`mimic-diagnostics-and-tooling`, `mimic-architecture-contract`, `mimic-build-and-env`, `mimic-validation-and-qa`, `mimic-run-and-operate` where the profile block is described).
- Inputs: the datasets named under [Resource Profile](#resource-profile).
- Behaviour that must not change: no code change in this slice.

### Authorized Surface
- Files allowed to change:
  - `docs/dev/OPTIMISATION-SPECTRUM.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-PERFORMANCE-ACCEPTANCE.md`
  - `docs/dev/MIMIC-PERFORMANCE-REFERENCE.md`
  - `CHANGELOG.md`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-diagnostics-and-tooling/SKILL.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-build-and-env/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
- Functions/classes/components allowed to change: documentation only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No code change; no new claim beyond what the measurements show; no thread-per-forest planning beyond the pathway's next-step pointer.

### Risk Flags
- Risky surfaces touched: none.
- Approval needed before implementation: no
- Difficulty: moderate
- Recommended Developer: claude-sonnet-5-5, high effort.

### Validation Plan
- Tests to add/update: none.
- Commands to run: the profiling harness on both drivers; `scripts/profiling/run_wall.py` with 3 repeats on the two mini-Uchuu run files after one warm-up each; `MallocLargeCache=0 ./mimic <scratch run file for file 0 of shin-uchuu-lhalo> -q`; the cross-format gate; `make check-docs`; `make check-format`; `make check-generated`; `make tests summary`.
- Lint (differential, via the `lint` skill): not required (Markdown only) unless a linted file changes.
- Manual checks: every number in the changelog entry traces to a table in the acceptance record.

### Rollback Path
- Revert the documentation commit.

---

## Next Chat Prompts

### Mode A — Assisted run (default, checkpointed)

```md
Plan file: docs/dev/MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md
Slices or batch this session: Slice 1
Reviewers (optional): drift auditor codex gpt-6.1-sol high; code reviewers codex gpt-6.1-sol high; claude claude-opus-5-5 high

Read the full plan file first. If a selected slice or batch receipt is incomplete or the plan state is unclear, stop and tell me before coding.

Work on the current feature branch for this plan; if none exists, create one and tell me the name.

Use orchestrator as the controlling skill. Act as the Developer: keep implementation, validation, Git operations, and commits local. Use a read-only Reviewer (the reviewers above, when given) only for investigation, evidence gathering, the hostile drift-audit skill, and an independent code-review skill pass. If no Reviewer is configured or available, perform Developer self-audit and record that provenance explicitly.

For each selected slice or batch, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. If any included slice's Risk Flags mark approval-needed, stop and get my approval before coding.
3. apply the scoped-implementation skill against the selected contract.
4. apply the drift-audit skill using a read-only Reviewer when available; otherwise perform Developer self-audit. Report the authorization gate result and who performed it before any quality review.
5. If the gate passes: for a broad or structural change, first run the code-health skill differentially against the slice's starting commit and supply its report as review evidence. Then apply the code-review skill using the code reviewers when available, in parallel if there are several; otherwise perform Developer self-audit through the code-review skill. Record who performed it. If the drift gate fails, fix the drift and re-audit.
6. Collect every finding from the current audit or review round before fixing, surface them to me, fix them as one batch, then re-run the relevant gate. If consecutive reviews return only minor findings and have clearly converged, record residuals in the slice summary and proceed.
7. Ask me before committing. On my approval, commit the selected slice or batch with the commit skill.

After the selected slice(s) or batch are committed, use the handoff skill to record state, audit provenance (Reviewer tool/label or Developer self-audit and fallback context), and the next slice or batch to resume from. Do not continue past the selected scope.

Confirm before starting: plan file read, selected slice(s) or batch, branch, and the first slice. Then begin.
```

### Mode B — Supervised autonomy (alternative)

```md
Plan file: docs/dev/MIMIC-PERFORMANCE-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: claude per plan (each slice's Recommended Developer line)
Drift auditor: codex gpt-6.1-sol high
Code reviewers: codex gpt-6.1-sol high; claude claude-opus-5-5 high
Run tag (optional): performance-tier-a

Use the project-manager skill. You are the PM: the accountable supervisor of this run. You never write slice code.

Handoff: HANDOFF.md in the repo root, git-ignored (add it to .git/info/exclude if it is not), is the sole entry point for a restart and for my post-run review. If it names an unfinished run of this plan, read it fully and resume that run (I will supply the token). Otherwise create it after init. Update it after every slice decision and before any stop with: run identity (never the token), seats, branch, commit authorisation, review protocol, slice log, plan decisions, residuals and follow-ups, and PM lessons.

For a new run, prepare, then confirm with me once:
- Read the whole plan, run check-plan with repo context, and check that the repo still matches what the plan assumes. Init freezes the plan, so raise any drift or warning now.
- Require a clean worktree with the plan committed. Propose the plan's feature branch, or a new branch name to create.
- Confirm: plan read, check-plan result, seats, branch, first slice, any approval-flagged slices (each stops the run until I approve it), and my standing authorisation for Developers to commit each slice.
After my go-ahead, run init with full model ids, efforts where the harness supports them, and the run tag. Give me the run id and token, then begin.

Per slice, in plan order:
1. start-slice with the Developer seat's model and effort. Wait with one long background `observe --wait`; never poll.
2. Run finalize, then your own checks: pane, diff, validation output, full test suite, lint, differential code-health if the structure changed, and a style-guide audit (the project's guide, otherwise the baseline). Treat newly introduced, in-contract style deviations as cleanup to steer.
3. Run the review panel when the plan or risk calls for it. Run the drift audit first, then read and rate it. Then start all code reviewers ~90 s apart, sharing one `--adjudicated-file`. Let reviews run to completion without watching them.
4. Rate every report, order the code panel, and score the Developer (criteria met, confirmed defects).
5. Decide: accept, steer or stop. Steer only once every review is in, with all findings batched into one correction. Each follow-up round re-runs the panel, focused on the change since the last round through an adjudication line.
6. Update `pm notes` for the next Developer, and HANDOFF.md for me.

Plan defects: where the plan's goals make one resolution natural and its impact stays within the slice or touches only a few slices, decide it yourself and record it as a delegated plan decision. Never edit the plan, widen the surface except by grant, or relax a floor fact. Stop for anything serious, contested or cross-cutting, and whenever the floor or a plan gate needs a human.

Finish: run `status --report`, confirm the ledger was written, run `ledger render`, and read `run-report.md` end to end. Record the run's actual status in HANDOFF.md. Report:
- total run time, quoted from the report with its endpoints
- commits (unpushed)
- what was accepted, and on what evidence
- what stopped, and why
- plan defects, with the decisions you took and the amendments you recommend
- residual risks and follow-ups, with a convergence read
- the leaderboard path
```
