# Post-Phase-5 Joint Review — Code Review + Simplification Pass

**Status:** Final (Revision 3) — external-panel convergence reached with no P0/P1 outstanding (opencode: ACCEPT at round 2; codex: ACCEPT at round 3 after its F-14-breadth P1 was fixed). Includes the owner-raised output-partitioning decision (D5). Owner has accepted D1(a), D2(a), D3(a), D4(a) as of 2026-08-13; D5 awaits an option choice.
**Date:** 2026-08-13, at branch `feature/ctrees-snapshot-reader`, HEAD `77c23b7f`.
**Scope:** A holistic joint code review and code-simplification pass over (a) the completed dual-driver Phase 5 change set (`ae22d278..77c23b7f`, ~8,600 insertions across 90 files) and (b) the remaining post-Phase-5 work recorded in `MIMIC-DUAL-DRIVER-PLAN.md`, `POST-PHASE-5-WORK.md`, and `SHIN-UCHUU-CONVERSION-PLAN.md`. Commissioned as the readiness check before the Shin-Uchuu production conversion.
**Method:** Five parallel internal review workstreams (core driver + memory; I/O + identity encoding; identity-gate harness; open-work-record verification; holistic simplification), each against the frozen plan contracts, plus the full validation-gate run (standard tiers, `USE-HDF5=no` build contract, and the manual cross-format identity gate). The resulting draft was then reviewed by an independent external panel — `codex` (gpt-5.6-sol, high effort) and `opencode` (opencode-go/hy3, high effort), both read-only — and every contested or new panel claim was independently re-verified against the repository before integration (§7). Phase 5 was reviewed slice-by-slice under the PM process during execution, so this pass deliberately targeted what slice-local review cannot see: cross-slice interactions, production-scale ceilings, and the accuracy of the open-work record.

---

## 1. Verdict

**The Phase 5 Mimic code is high quality and production-ready at the scale it has been proven at.** The state rotation, driver parity behaviours, int64 contract, identity encoding (proven overflow-safe at multiplier 10¹⁰ and ranks ~9×10⁹), non-HDF5 build contract, serial-only enforcement, output seam, and memory lifecycle all held up under adversarial reading by seven independent reviewers (five internal, two external). The identity-gate harness is unusually rigorous — every vacuous-pass path probed is closed by an independent check — and the gate re-passed 8/8 on today's HEAD with the tree path re-confirmed byte-identical to the pre-Phase-5 baseline. `POST-PHASE-5-WORK.md` is accurate on essentially every checkable claim.

**However, the review found three production-blocking items, plus one record correction, that the pre-Shin-Uchuu checklist does not capture.** None is a defect at currently-proven scale — nothing the identity gate tests can reach them — which is exactly why slice-local review missed them:

1. **The galaxy pool's chunk design exhausts the allocator's block table at Shin-Uchuu slab scale** (F-1) — a mid-run abort, days into the one-shot conversion run.
2. **The converter, as implemented, cannot execute the production conversion** (F-13): the identity/rank pass and the required validation battery are in-memory designs needing ~600+ GB at 15–18B halos on a 512 GB machine, and the scatter phase's resume model is incompatible with the plan's own batched, consumptive-delete transfer strategy. These limits are honestly recorded in `scripts/convert/README.md` as "deferred to a future production pass" — but no plan or checklist schedules that pass, and the record's §6 opens with "the machinery is ready".
3. **Critical HDF5 statuses are ignored on the output finalization path** (F-14) — close statuses everywhere, and create/write statuses throughout master construction — so a deferred or immediate output error (e.g. ENOSPC) can turn a damaged multi-day, no-resume run into a success exit with the incomplete-output safety net already disarmed.

And one **record correction** (not itself a projected abort): **the write path's true first ceiling is 10⁹, not the recorded 2.1×10⁹** (F-2), which changes what §2.3's population check must compare against.

The simplification pass rated the change set lean, with one consolidation candidate that needs an owner decision (D1). The external panel confirmed every internal finding's substance, corrected two (F-5 scope, F-10's `deltaMvir` bullet), rated two as overstated in magnitude (F-3's traffic estimate, D1/D3's urgency), and contributed F-13/F-14/F-15 — all five panel P1s (four from codex, one from opencode) independently verified before inclusion here.

**Bottom line:** the Mimic runtime needs two small fixes (F-1/D2, F-14) plus the owner-raised output-partitioning slice (D5), and the converter needs a scheduled scale-engineering pass (F-13/D4), before the Shin-Uchuu conversion. Everything else is hygiene that can ride along or wait. §6 is the updated checklist.

---

## 2. Findings

Severity is calibrated to the stated next step: a P1 here means "would defeat or seriously damage the Shin-Uchuu production conversion or run", not "broken today". Nothing found is a defect at currently-proven scale.

### F-1 [P1 — blocks Shin-Uchuu] Galaxy-pool chunk count exhausts the allocator's block table at production slab scale

`src/core/galaxy_pool.c:23`, `src/core/snapshot_driver.c:775`, against `src/util/memory.h:24` and `src/util/memory.c:192-196`.

Every pool chunk is one tracked allocator block; `mymalloc_cat` is fatal at `DEFAULT_MAX_MEMORY_BLOCKS = 50000`. The chunk capacity is fixed at creation (8,192 galaxies, ~1.44 MB — `galaxy_pool_create(0)` at both snapshot-driver sites) and never grows; `galaxy_pool_reset()` rewinds but never frees, so each pool's chunk count is a permanent high-water mark. The snapshot driver holds **two** pools live by design. The pools alone hit the cap at ≈ 50,000 × 8,192 ≈ 409M combined live galaxies — ≈205M per generation — and since the cap counts *all* tracked blocks (slabs, buffers, module allocations), the real threshold is lower still. The projected Shin-Uchuu z=0 slab is ~315M halos, i.e. ≈38,500 chunks per pool, ≈77,000 blocks from the pools alone — **at least 1.5× over the cap** — and the abort lands mid-run at the first snapshot pair crossing the threshold, days into the conversion's one shot, with a message (`Increase DEFAULT_MAX_MEMORY_BLOCKS`) that names the weakest fix. The tree driver never approaches this (one forest's galaxies), and micro-Uchuu peaks around 1.2M records, so no existing test can see it. Independently verified by three reviewers, including line-level confirmation that chunk capacity never grows and reset never frees.

**Fix direction (decision D2):** grow `chunk_capacity` geometrically in `galaxy_pool_alloc()` (block count becomes O(log G); the pool contract of stable pointers + bulk reset is preserved because chunks are independent and never move; galaxy output is untouched), with explicit guards on capacity overflow and maximum single-chunk size. Do **not** pass the slab size as one chunk (a single ~55 GB contiguous block), and raising `DEFAULT_MAX_MEMORY_BLOCKS` alone is the worst option — `find_block_index()` is a linear scan and `galaxy_pool_destroy()` would go quadratic.

### F-2 [P2 — corrects the open-work record] The write path's first ceiling is `MAX_HALO_ARRAY_SIZE` (10⁹), not the recorded `INT_MAX` counter cap

`src/include/constants.h:43`, enforced at `src/core/output_buffer.c:58-70`.

`POST-PHASE-5-WORK.md` §2.3 records the `INT_MAX` (≈2.1e9) cap in `output_increment_halo_counters_checked()` as the per-snapshot output ceiling to check before Shin-Uchuu. There is a lower one: output-buffer growth is clamped to `MAX_HALO_ARRAY_SIZE = 1000000000` and FATALs when it cannot grow past it. A snapshot whose output population (slab galaxies + accumulated orphans) exceeds 10⁹ aborts in the marshaller before the recorded counter guard is consulted — at roughly half the headroom §2.3 assumes. The constant was not revisited when `OutputBuffer` widened to int64, and the fatal's message presents it as a structural invariant rather than a tunable. **This is a record correction, not a projected abort**: the projected ~315M z=0 slab sits well under 10⁹, so this ceiling fires only if the output population exceeds the slab by ~3× (orphan accumulation would have to be extreme). §2.3's projected-population check must simply compare against 10⁹, not 2.1e9; if widening is ever needed, both ceilings move together (and the fatal's `%d` on what would become an `int64_t` needs `PRId64`).

### F-3 [P2 — performance at production scale; benchmark before acting] `save_halos_hdf5()` rescans the entire output buffer once per requested output snapshot

`src/io/output/hdf5.c:302-326`, reached from `src/core/snapshot_driver.c:592`.

The `for n in NOUT { for i in NumProcessedHalos }` shape is natural for the tree driver (one buffer spans all snapshots) but on the snapshot path the buffer is homogeneous in `SnapNum` by construction, so exactly one `n` matches and the other `NOUT−1` passes are waste — ~15.75 billion wasted predicate iterations per z=0-scale snapshot write at 50 requested outputs. Output is bit-identical either way; the cost is wall-clock only, and the original draft's terabyte-scale traffic estimate was unsupported (the scan touches one field per record; actual traffic depends on cache-line behaviour and must be measured, per the external panel). Fix direction if a benchmark justifies it: invert the loops via a `SnapNum → output index` lookup, or have the snapshot driver pass its single matching output index down. Timing decision at D3 — both external reviewers recommend deferring this; the shared writer is proven code and the run merely wastes time.

### F-4 [P2 — divergence-surface risk; simplification] `setup_module_context` and `process_halo_evolution` are duplicated across the two drivers

`src/core/snapshot_driver.c:413-479` vs `src/core/build_model.c:473-567`.

Independently verified: after normalising `state->workspace` ↔ `FoFWorkspace` and `const`/loop-variable style, the bodies are semantically identical — 65 lines owning `ctx->time_interval`, dynamic substep counts, `substep_dt`, and `UniqueCentralGalaxyID` propagation. These are precisely the quantities the identity gate exists to protect, and drift between the copies is caught **only** by a manual ~6-minute gate that no CI runs. The duplication rationale documented in `snapshot_driver.c:20-26` ("what is replicated here rather than reused is the part that is written in tree indices") was retired by Phase 5's own explicit-view migration for these two functions, so the change set copied them under a justification it had itself invalidated. Unlike the progenitor-lookup trio (genuinely different index spaces — correctly replicated), merging these two is a pure parameterization: give the tree-side pair a `struct Halo *workspace` parameter and delete the snapshot copies. There is **no present correctness defect** — the copies are in sync and identity-gated today. The external panel split on urgency (one endorses merging now, one says it must not gate Shin-Uchuu); decision D1.

Two smaller members of the same family, same treatment if D1 is taken: `count_fof_subhalos` and `make_halo_init_payload` (`snapshot_driver.c:331-355` vs `build_model.c:324-363`) are exact copies with no tree coupling at all, and the duplicated payload builder instantiates the generated populator in two translation units — the populator the dual-driver plan explicitly kept singular.

### F-5 [P3] `snapshot_acquire_generation()`'s full-capacity `memset` makes the whole slab-sized output buffer resident; the §2.2 memory recompute must count it

`src/core/snapshot_driver.c:683-692`. The buffer is seeded at `nhalos + MIN_HALO_ARRAY_GROWTH` records — at a z=0 Shin-Uchuu slab that seed alone is ~315M × 264 B ≈ 78 GiB — and the `memset` touches all of it immediately, so a `sizeof`-based §2.2 memory projection that assumes lazy residency will underestimate the true peak by that amount. **Do not remove the memset** (external-panel correction accepted: it is defensive zeroing of records the marshaller is contractually expected to overwrite, and removing it buys no correctness); the actionable content is purely that §2.2's recompute must count full seed capacity as resident.

### F-6 [P3] Snapshot runs record a hard-coded `"snapshot_hdf5"` literal as output provenance instead of the resolved reader's name

`src/core/tree_driver.c:583`, consumed as the `TreeType` provenance attribute at `src/io/output/metadata_hdf5.c:613-620`. The tree branch correctly uses `reader->name`; `MimicConfig.snapshot_reader->name` is equally available. Harmless today (one snapshot reader exists); silently wrong provenance the day a second is registered. One-line fix, opportunistic timing.

### F-7 [P3] Cyclic link chains in a corrupt snapshot input hang or overflow rather than aborting with a diagnostic

`src/core/snapshot_driver.c:245-252`, `:287-297`, `:345-355`; reader validation at `src/io/snapshot/read_snapshot_hdf5.c:865-925`. The reader validates every link's range but not acyclicity: a cyclic `NextProgenitor` chain hangs the progenitor count; a cyclic `NextHaloInFOFgroup` chain overflows an `int` counter (signed overflow, UB) before the `members_processed != nhalos` guard can fire. Context the external panel added: the frozen format deliberately assigns full topology checking to the producer, and the converter's validator does contain cycle checks (`scripts/convert/validate.py:461`, `:529`) — so the defence exists, on the producer side, and the F-13 scale work must preserve it. A bounded-iteration guard (`count > nhalos` → FATAL) in the two driver chain walks remains cheap optional defence-in-depth for a multi-day run; not a prerequisite.

### F-8 [P3] `open_run` validates the file against the format table but never checks the selected package's generated read-list against it

`src/io/snapshot/read_snapshot_hdf5.c:145-164` vs `:690-734`. A mismatched package (e.g. `mini-millennium` declaring `tree_type: snapshot_hdf5` against a conforming dataset) passes the entirety of `open_run` — including the full per-snapshot identity scans over every file — and dies only inside the first raw `H5Dopen2` at `load_slab`. Loud and diagnosable, hence P3, but it contradicts the file's own "open_run validates the whole dataset" contract, and at Shin-Uchuu scale the wasted pre-failure scan is a full pass over ~315M×N halos. Cheap fix: iterate the generated `READ_TREE_PROPERTY*` name list against `snapshot_h5_dataset_spec_by_name()` once at `open_run`. Opportunistic timing.

### F-9 [P3] Slab staging buffers are over-sized by ~3.8 GB at production slab scale

`src/io/snapshot/read_snapshot_hdf5.c:678-728`. Both staging buffers are allocated at the widest element size (8 B) and held simultaneously for the whole fill (~10.1 GB transient at a 315M-halo slab), but every multi-dim catalog field in every current package is float32. Free headroom for the §2.2 budget; not a feasibility item on its own.

### F-10 [P3 — documentation corrections to the open-work record and plans]

- `POST-PHASE-5-WORK.md` §2.1 table: `micro-uchuu`'s Bullock-spin range is `[-200, 200]` (`simulations/micro-uchuu/halo_properties.yaml:131`), not `[-20, 20]` as tabulated; the other three packages in that row are correct.
- `POST-PHASE-5-WORK.md` §2.2 cites `SHIN-UCHUU-CONVERSION-PLAN.md:486-490` for the memory trigger; the trigger sentence is at `:496`.
- **Correction to this report's own first draft (external panel):** the draft flagged the `deltaMvir` calibration entry (`SHIN-UCHUU-CONVERSION-PLAN.md:445`, `POST-PHASE-5-WORK.md` §2.5) as stale because no simulation package declares it. That was wrong: `deltaMvir` is a **core output property** (`src/core/core_properties.yaml:131-139`, range `[-20000, 20000]`, annotated "to accommodate Uchuu-scale mass swings"), computed during inheritance. Its range check is legitimate pre-Shin-Uchuu work; the only defensible edit is a clarifying note that it is a core-level output range, not a package catalog range like `Len`/`Spin`.

### F-11 [P3 — gate/comparator hygiene, next time the files are legitimately open]

- `test_cross_format_identity.py:721-732` line-scans run YAML for `output_directory:` with `startswith`/`split` while the same file is parsed twice elsewhere with `yaml.safe_load`; the scanner decides which directory the gate compares. No mis-parse exists on the six committed run files (verified by running both parsers), but it is the one remaining single-parser weakness in a file that was hardened against exactly this class. Replace with the existing `yaml.safe_load` path.
- `test_cross_format_identity.py:125`: `EXCLUDED_PROVENANCE_ATTRS` is dead and misleadingly named as the operative exclusion list while `classify()` actually consults `PROVENANCE_ATTR_PATHS`. Delete (with `scripts/compare_cross_format_identity.py:150`'s unused `self.spec` and `:161-162`'s uncalled `RunIndex.rows()`).

### F-12 [P3 — smaller code-health notes, explicitly non-urgent; not pre-Shin-Uchuu work]

- The halo-array growth policy (grow ×1.5, clamp, minimum increment) now exists in three places (`build_model.c:275-297`, `snapshot_driver.c:170-194`, `output_buffer.c:58-71` — the third already carries a "mirrors build_model.c" comment). A four-line shared helper would keep the two drivers' allocation behaviour from drifting. Opportunistic only.
- `snapshot_driver.c` introduces thirteen hard-coded `file.c:LINE` cross-references (all currently accurate — verified — but nothing enforces them, and they decay on the first edit to `build_model.c`). Keep the function names, drop the line numbers. If D1 is taken, six of these disappear anyway.
- `g_partition_source_reader` (`src/core/tree_driver.c:536-559`): the partition-source seam stashes the reader in a mutable file-static so its `partition_exists` hook can find it later, making the returned struct not self-contained. Correct today (one reader, serial); flagged independently by two reviewers; fine to leave until the seam is next open.

### F-13 [P1 — blocks Shin-Uchuu; external panel finding, independently verified] The converter cannot execute the production conversion as implemented

Three distinct limits, all real, all confirmed against the code, and all *already recorded* in `scripts/convert/README.md`'s "Shin-Uchuu-scale notes" as "deferred to a future production pass" — but scheduled by no plan and absent from the pre-Shin-Uchuu checklist, while `POST-PHASE-5-WORK.md` §6 opens with "the machinery is ready":

1. **The identity/rank pass is in-memory over all snapshots.** `compute_identity()` (`scripts/convert/links.py:411-450`) concatenates five int64 columns over every halo in the dataset and `np.lexsort`s them: at 15–18B halos that is ≈600–720 GB for the columns plus ≈120–144 GB for the sort's order array, on a 512 GB machine. Its own docstring says "the external-merge sort is a production concern". The conversion plan's risk table ("measured key volume ~150–250 GB fits RAM") is inconsistent with the implementation as written and must be re-derived.
2. **The required producer validation battery is also in-memory.** `scripts/convert/validate.py` loads and retains full-dataset columns (e.g. `:305`, `:616`, `:814`), and producer validation is part of the format contract and the plan's Definition of Done — it cannot be skipped, so it must be made streaming/per-snapshot capable.
3. **The scatter phase's resume model is incompatible with the plan's own transfer strategy.** The plan requires batched fetch → scatter → consumptive delete because 5.6 TB cannot coexist locally (`SHIN-UCHUU-CONVERSION-PLAN.md:117`); `run_scatter` requires every listed source file to exist at start (`scripts/convert/scatter.py:524`), freezes the ordered source set into the manifest and refuses resume when it changes (`:552-556`), and `source_completed()` re-stats completed files (`:360`) — so deleted batches break resume. A batch-aware source inventory is required.

Also recorded in the same README note and confirmed relevant: the ~5 GB Phase-0 forest map is passed to pool workers by pickling, and the fix-up stage's per-satellite sequential Python scan (`scripts/convert/fixups.py:344-354`, up to 31 searches per satellite) "would need revisiting for Shin-Uchuu" — the D4 pass must include a production-scale benchmark of it and an explicit retain/optimize decision, measurement-first, rather than prescribing a rewrite. **Fix direction (decision D4):** schedule the converter scale-engineering pass as its own planned, gated piece of work before the production conversion, with the micro-Uchuu validation battery + topology cross-check re-run green as its acceptance gate (the converter's reference semantics must not move while its machinery is rebuilt).

### F-14 [P1 — production trust; external panel finding, independently verified; widened in panel round 2] Output-finalization HDF5 statuses are ignored — closes everywhere, and create/write results throughout master construction — so an output error can end a damaged no-resume run with exit success

Close statuses: `src/core/snapshot_driver.c:616` (`H5Fclose(HDF5_current_file_id)` in `snapshot_finalize_output`, discarded), `src/io/output/master_hdf5.c:184` (master close, discarded; also `:153`), `src/io/output/hdf5.c:108` (per-partition close on the shared path, discarded). Create/write statuses (panel round 2, verified): master construction ignores the results of `H5Gcreate`/`H5Acreate`/`H5Awrite` for the per-snapshot `Redshift` attributes (`master_hdf5.c:62-71`) and the republished `TotHalosPerSnap` attributes (`:141-150`), among other creation/open calls in the same file. And `src/core/main.c:435-441` disarms the snapshot driver's incomplete-output cleanup immediately after `write_master_file()` returns, without any of these statuses having been checked. The two halves compose into the same failure: an *immediate* metadata-write failure need not make the later `H5Fclose` fail, and a *deferred* flush failure (HDF5 buffers metadata until close; ENOSPC is the canonical case) surfaces only at close — either way a serial, no-resume, multi-day run can leave damaged output *and* a success exit code *and* the "incomplete output" safety net already disarmed. The fix is small and behaviour-preserving on the success path: checked failure propagation (`FATAL_ERROR` on any negative status) for the critical HDF5 create/open/write/close operations across partition finalization and master construction, with cleanup remaining armed until all of them have succeeded. This should land with F-1's fix under the same evidence run.

### F-15 [P2 — readiness-checklist gap; external panel finding] The pre-Shin-Uchuu checklist has no complete-run smoke test on a Shin-Uchuu subset

The record's §2.6 requires running the *identity gate* on a Shin-Uchuu subset, which by construction runs both drivers end to end on that subset — but the checklist nowhere states that this subset must include the deep/z=0 snapshots where the range FATALs (§2.1 `Spin`), link validation, and memory behaviour actually bite, nor that a full `sage16` physics pass (not just `halos-only`) must complete on it. Given the production run is serial, no-resume, and multi-day, any config-time or mid-run FATAL is a full restart; the cheapest insurance is making the §2.6 subset run explicitly a *complete* rehearsal: both models, both the earliest and the z=0 end of the snapshot range, output written and read back. Folded into the updated checklist wording (§6) rather than added as a separate item.

---

## 3. What was verified as sound (so it is not re-litigated)

- **State rotation:** slot arithmetic cannot alias; previous-generation buffers are never mutated during the current sweep; release order matches the frozen contract exactly; `snapshot_count == 0` and `== 1` terminate correctly; the `ProcessedHalos` globals never outlive their buffer (cleared on all three paths).
- **Driver parity:** all seven contract behaviours (CentralMvir stamping, `SnapNum = current−1` + `dT` sentinel, pre-marshal `time_interval`/substeps, `UniqueCentralGalaxyID` propagation, marshal-time `SnapNum`, `lenoccmax = −1` tie-break pinning, chain-then-range gather order) verified line-by-line against `build_model.c`. The one deliberate asymmetry (reading `FirstProgenitor` through `view` but `Len`/`NextProgenitor` through `prev`) is correct for the format's adjacent-link scope.
- **Integer widths:** `slab.nhalos > INT_MAX` guarded before any narrowing; the only narrowings are explicit and checked; `OutputBuffer` int64 end-to-end; `size_t` casts precede every `count × sizeof` product; `TotHalosPerSnap` coherently int64 across write, read-back, and republish.
- **Identity encoding:** overflow-safe at multiplier 10¹⁰ with ranks 5–9×10⁹ (worst-case id ≈ 9.22e18 < INT64_MAX); the snapshot bound delegates to the same `galaxy_id.h` expression the encoder uses, so the two cannot drift; all three former `TREE_MUL_FAC` tree-reader guard sites plus the run-scoped totals and the diagnostic all use the configured value; the lifted tree-ordered rejection leaves no silent hole (ctrees readers fail fast per-forest; the lhalo path fails loudly at encode via `components_valid`).
- **Reader validation:** structure→values→shapes→bounded scans ordering; every rank provably `< multiplier` before any encode; empty-dataset sentinel cross-checked per-file and run-scoped; physical-header agreement implemented exactly as specified (multiply up by 1e10, 16·DBL_EPSILON relative, non-finite rejected); identity arrays allocated/validated/freed with the slab; error paths are `FATAL_ERROR → exit`, so leak-on-failure is moot by design.
- **Output seam:** no file under `src/io/output/` reads `MimicConfig.reader`; snapshot runs write one partition, omit `Ntrees`/`TreeHalosPerSnap` (absent, not zeroed); the multiplier provenance attribute is on both per-file and master paths; `hdf5_format_version` 1.2 and the Python loader default moved together.
- **Config gating:** all three snapshot rejections reachable and correctly ordered (`NTask` set before `read_parameter_file()`; non-MPI builds see `NTask == 0`, so the check is not vacuous); two-registry resolution fails closed; two ranks cannot reach one file.
- **Non-HDF5 build:** `make USE-HDF5=no` builds and links clean at 0 warnings; `snapshot_driver.c` compiles standalone with no `HDF5` macro under `-Wall -Wextra -Wshadow` clean.
- **Gate and comparator:** all four vacuous-pass fixes from `486ed505` present and correctly wired; the preservation stage binds every permitted delta to attribute name + object path + exact before/after transition and pins literal values; the comparator's duplicate-ID scan runs before anything trusts uniqueness; byte-level comparison genuinely defeats NaN-payload and signed-zero hiding; preflight derives its counts independently of the comparator and cross-checks them.
- **Open-work record:** every checkable claim in `POST-PHASE-5-WORK.md` verified against the code, including reproducing the `dump-ctrees-topology-tool` link failure verbatim; no missing deferred item found across the git log, the driver plan's records, and the carried Phase 4b entries — the F-13 converter limits live in the converter README and conversion plan rather than in any Phase 5 record, which is why the §6 checklist missed them.
- **Format contract:** the frozen 16-column `/halos` set matches the Shin-Uchuu conversion plan's bridge contract exactly; no `format_version` bump is needed for the production conversion.

The simplification pass additionally recorded fourteen considered-and-rejected items (progenitor-lookup trio correctly replicated across index spaces; the four `ensure_*_scratch` helpers; the `src/io/snapshot` ↔ `src/io/tree` duplication whose downgrade rationale still holds exactly; `OutputPartitionSource` being minimal, not over-general; the triple `snapshot_clear_output_globals()` being deliberate belt-and-braces; and others) so they are not rediscovered. Overall simplification verdict: **lean** — no singleton-pool remnants, no stranded `TREE_MUL_FAC` hard-coding, no vestigial `InputTreeHalos` reads, no redundant parameters from the int64 widening.

---

## 4. Decisions needed (options, recommendation, consequences)

The plan goals these decisions serve: two drivers with provably identical scientific behaviour and a minimal divergence surface between them; a Shin-Uchuu production conversion and run that cannot fail mid-flight for a foreseeable structural reason; no bloat.

**Owner decisions recorded 2026-08-13:** D1(a), D2(a), D3(a), and D4(a) are accepted as recommended. D5 below was raised by the owner after reviewing the first draft and awaits an explicit option choice.

### D1 — Merge the duplicated physics-timing functions (F-4): ride along with D2's evidence run, or defer past Shin-Uchuu?

The external panel split: opencode endorses merging now as sound and non-speculative; codex says it must not gate Shin-Uchuu because there is no present defect and no operational benefit to the run itself. Both are right about their halves, which shapes the options:

- **(a) Ride-along (recommended):** D2's pool fix must land before Shin-Uchuu regardless and requires the full evidence run (bitwise tree-path preservation + tiers + identity gate). Landing the F-4 merge in the same change costs no additional evidence run and shrinks the divergence surface by ~120 lines before the long production-focus window during which manual mirroring is most likely to be forgotten. Condition: if the merge turns out to be anything more than the verified pure parameterization, stop and defer rather than debug it pre-production.
- **(b) Defer until after Shin-Uchuu:** production runs on exactly the bits that passed today's gate; every intervening edit to `build_model.c`'s timing logic must be manually mirrored, with only a manual gate to catch a miss.
- **(c) Keep the duplication permanently:** coherent for the progenitor-lookup trio, but for byte-identical functions it converts a compiler-checkable identity into a process obligation. Not recommended.

Recommendation: **(a)**, strictly as a passenger on D2's already-required evidence run — never as its own pre-production refactor (that half of codex's objection is accepted).

### D2 — Fix shape for the galaxy-pool block-cap ceiling (F-1) — must close before Shin-Uchuu

- **(a) Geometric chunk growth in `galaxy_pool_alloc()` (recommended):** cap chunk size at a few million galaxies; block count becomes O(log G); tree path behaviourally unchanged; no driver knowledge needed in the pool. Add explicit guards for capacity overflow and maximum single-chunk allocation (panel note). Verify with existing pool unit tests + tree-path preservation + the identity gate — the gate re-run is **mandatory** for this fix even though micro-Uchuu cannot reach the ceiling, because the fix changes allocation behaviour on the exact path the gate certifies.
- **(b) Size pools from the run's maximum slab count at driver startup:** simplest arithmetic, but couples the pool to reader metadata and still allocates the peak up front even for small early snapshots.
- **(c) Raise `DEFAULT_MAX_MEMORY_BLOCKS`:** weakest — linear block-table scans make destruction quadratic, and the ceiling merely moves.

Recommendation: **(a)**. F-14's close-status checks should land in the same change set and evidence run.

### D3 — Timing for the output-writer loop inversion (F-3)

- **(a) Defer past Shin-Uchuu; benchmark first (now recommended — reversed from the first draft on the panel's argument):** the inversion is perf-only, output is bit-identical, and it modifies the proven shared marshaller immediately before a no-resume production run — exactly the class of change the project's own discipline defers. The run merely wastes wall-clock; measure the actual cost on the F-15 subset rehearsal and decide with data.
- **(b) Fold into the pre-Shin-Uchuu work:** only if the subset rehearsal measures the waste as genuinely material (multiple hours), in which case it joins D2's evidence run.

### D4 — Approach for the converter scale-engineering pass (F-13) — must close before the production conversion

- **(a) A dedicated, planned converter-scale slice (recommended):** implement the external-merge rank sort (already named as the deferred design in the converter README), streaming/per-snapshot validation, a batch-aware scatter inventory compatible with consumptive deletes, and shared/memory-mapped forest-map distribution; benchmark the fix-up stage's sequential satellite scan at projected scale and make an explicit retain/optimize call from the measurement (panel round 2 addition); acceptance gate = the full micro-Uchuu validation battery + topology cross-check re-run green (the converter's reference semantics must not move), plus a measured memory profile of the rank pass at projected Shin-Uchuu scale.
- **(b) Try to fit in RAM by measurement first:** re-derive the actual key volume (the plan's 150–250 GB figure vs the implementation's ~600+ GB) and attempt column-width reduction (e.g. int32 where provably safe) before building external-merge machinery. Cheaper if it works, but it leaves validation and scatter batching unsolved — at best it shrinks option (a), it does not replace it.
- Either way, this work should get its own frozen implementation plan (it is converter-side, gate-checkable, and well-bounded); it is the single largest unscheduled item between here and Shin-Uchuu.

### D5 — Snapshot-run output partitioning before the production run (owner-raised, 2026-08-13)

**Observation (owner, verified):** a `sage16` micro-Uchuu run writes five `model_NNN.hdf5` partitions of ~130–200 MB on the tree-ordered side (`forests_per_file` chunking) but one 823 MB `model_000.hdf5` on the snapshot-ordered side — the Phase 5 Output Contract's deliberate single-partition design. Shin-Uchuu's z=0 slab alone is ~100× micro-Uchuu's entire output, so a production run would produce a single file in the hundreds-of-GB-to-TB range: operationally unmanageable, and the worst possible blast radius for exactly the failure class F-14 addresses, on a one-shot no-resume run whose output layout is frozen the day it launches.

**Interaction with the decisions above: none conflict.** D2 doesn't touch the writer; F-14's checks are layout-independent and any partitioning must preserve them; D3 is independent of file layout; D4 is converter-side. The work is well-contained by Phase 5's own architecture, with three qualifications from the panel's round-3 check (all verified) so the slice is scoped honestly rather than assumed free: the `OutputPartitionSource` seam is driver-neutral for *enumerating* partitions (`src/io/output/util.h:57`) but the snapshot implementation is fixed to one partition and has no partition→snapshot mapping yet; the driver *buffers* each requested snapshot separately (`snapshot_driver.c:585`, `:839`) but performs its one explicit flush only at finalization (`:607`, `:849`), so per-partition flush/finalize is new work, not reuse; and the master links partitions under every snapshot in a Cartesian loop (`master_hdf5.c:80-101`), which one-snapshot-per-file partitioning must adapt. Master-based consumers remain unaffected by a split, and the identity-gate comparator already aggregates numbered partitions per side (`scripts/compare_cross_format_identity.py:98`, `:165`, `:210`).

- **(a) One partition file per requested output snapshot, pre-Shin-Uchuu, as its own small frozen slice (recommended):** the snapshot-major analogue of per-forest chunking; file count = NOUT; each file bounded by its snapshot's population (z=0 still ~80 GB at Shin-Uchuu scale — large but manageable). Separate slice from D2 (D2 is surgical; this is a design change), certified by its own tree-path preservation + tiers + identity-gate run, scheduled back-to-back with D2's. Note the distributed-snapshot plan was nominally assigned "partitioned snapshot output" but its text does not record that obligation — this slice supersedes that assignment for the serial case and should say so in both documents.
- **(b) Size-targeted splitting within snapshots (mirroring `target_file_size_mb`):** uniform few-GB files, but requires a snapshot's records to span files — materially more machinery immediately before production. Build only if (a)'s z=0 file size proves genuinely unworkable.
- **(c) Defer; accept one massive file for the production run:** no engineering risk now, but the layout is frozen at launch, repackaging afterwards means re-reading TBs, and the single-file corruption blast radius remains.

Recommendation: **(a)**. Not yet owner-decided; raised by the owner with a stated later task to fix — this review's addition is that it must move **before** the production run, not after.

### Standing decision reiterated from the record (unchanged by this review)

The `Spin` bound for `uchuu` and `shin-uchuu` (`POST-PHASE-5-WORK.md` §2.1) remains a scientific call only the owner can make; this review adds only the table correction in F-10. The I/O review's question about gate coverage at multiplier 10¹⁰ is answered by the record's own §2.6 (the Shin-Uchuu subset gate run exercises 10¹⁰ end to end); no extra gate variant is needed before then.

---

## 5. Validation record

Run 2026-08-13 on the development machine, in sequence, exit codes checked. Logs under the session scratchpad (`validation/*.log`).

| Step | Command | Exit | Verdict |
|---|---|---|---|
| 1 | `make info` | 0 | PASS (toolchain/library detection recorded) |
| 2 | `make check-generated` | 0 | PASS — generated code up to date |
| 3 | `make validate-modules` | 0 | PASS |
| 4 | `make check-format` | 0 | PASS |
| 5 | `make check-docs` | 0 | PASS — no broken links, no unresolved markers |
| 6 | `make -j` (default pair) | 0 | PASS — build complete |
| 7 | `make tests-unit` | 0 | PASS |
| 8 | `make tests-integration` | 2 → 0 | PASS on clean re-run (see note) |
| 9 | `make tests-scientific` | 0 | PASS |
| 10 | `make clean && make USE-HDF5=no -j` | 0 | PASS — non-HDF5 build contract holds |
| 11 | Cross-format identity gate (`MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific`) | 0 | **PASS — 8/8 stages** |
| 12 | Restore default build; `git status` clean | 0 | PASS |

**Step 8 note.** The first integration attempt failed with `tests/integration/test_substeps.py` reporting `Mimic executable not found` for four cases mid-suite — the executable transiently vanished during the validation session (a sequencing artifact of the session itself, consistent with a concurrent rebuild step; every earlier suite in the same run had used the binary successfully). The immediate clean re-run passed all integration tests with zero failures. No code defect is indicated; recorded for honesty.

**Step 11 detail.** The gate re-ran in full on the real micro-Uchuu dataset: all eight identity comparisons passed (both models × both timestep schemes), and the tree-path preservation stage confirmed 4,409,282 galaxy records byte-identical to the `ae22d278` baseline with exactly the four permitted HDF5 metadata deltas observed at their expected before/after values, and `output_schema.json` differing in exactly the description and `source_md5`. This independently reconfirms the Phase 5 closeout claim on today's HEAD.

---

## 6. Effect on the pre-Shin-Uchuu checklist

If the findings above are accepted, `POST-PHASE-5-WORK.md` §6 is superseded by this ordering (new items **bold**):

1. Decide `Spin`'s bound for `uchuu`/`shin-uchuu` (§2.1, unchanged) — with the F-10 table correction applied.
2. **Fix the galaxy-pool block-cap ceiling (F-1 / D2) and the ignored output-finalization statuses (F-14: closes everywhere, plus create/write results in master construction, cleanup armed until all succeed), in one change set, certified by one evidence run: bitwise tree-path preservation + full tiers + a mandatory identity-gate re-run.** D1(a) is accepted, so the F-4 consolidation rides in the same run (with its stop-and-defer guard).
3. **Schedule and execute the converter scale-engineering pass (F-13 / D4)** — external-merge rank sort, streaming validation, batch-aware scatter inventory, forest-map distribution, and a measured retain/optimize decision on the fix-up stage — gated on the micro-Uchuu battery + topology cross-check re-running green.
4. **Partition the snapshot-run output before the production run (D5, pending the owner's option choice; recommended: one file per requested output snapshot)** — its own small frozen slice with its own gate re-run, scheduled back-to-back with item 2's.
5. Recompute the driver's memory peak (§2.2, unchanged in substance) — now explicitly counting F-5's full-seed-capacity residency and F-9's staging transients, and recording the conclusion against the 85%-of-RAM projection trigger rather than leaving it implicit.
6. Check the projected z=0 output population against **10⁹** (`MAX_HALO_ARRAY_SIZE`, F-2), not only the recorded 2.1e9 counter cap (§2.3).
7. Confirm the particle mass (§2.4, unchanged).
8. Calibrate the remaining property ranges (§2.5) — with the F-10 clarification that `deltaMvir` is a core output range, not a package catalog range.
9. Run the identity gate on a Shin-Uchuu subset (§2.6) — **as a complete rehearsal (F-15): both models including a full `sage16` pass, a subset spanning the earliest snapshots through z=0, output written and read back; this is also the first end-to-end exercise of the 10¹⁰ multiplier and the natural place to measure F-3's writer cost (D3) and the §2.2 memory profile.**

Nothing in `POST-PHASE-5-WORK.md` §3 blocks the conversion, unchanged from before. The P3 findings here (F-5 through F-12) are opportunistic or fold into the items above; none gates the conversion on its own.

---

## 7. External panel record (Phase B)

**Panel:** two independent read-only reviewers, launched via the orchestrator with the full report, plan documents, and load-bearing sources: `codex` (gpt-5.6-sol, high effort) and `opencode` (opencode-go/hy3, high effort). Both were asked to verify every finding, hunt for missed Shin-Uchuu-scale issues, and flag bloat. Artifacts under `.orchestrator/runs/delegates-20260813-012208-86432/`.

**Verdicts on the first draft:** codex — REJECT (0 P0 / 4 P1 against the report, all completeness gaps); opencode — ENDORSE WITH CORRECTIONS (0 P0 / 1 P1). Every panel P1 and every contested rating was independently re-verified against the repository by the lead reviewer before this revision; nothing was accepted on authority.

| Panel claim | Adjudication |
|---|---|
| codex: converter scatter/rank/validation cannot run at production scale (3× P1) | **Accepted** — verified line-level (`links.py:411-450` in-memory lexsort with its own "production concern" docstring; `scatter.py:524/:552/:360` frozen source set + re-stat of deleted files; converter README's own deferred-scale note). Integrated as F-13 + D4. |
| codex: output-path `H5Fclose` statuses ignored, cleanup disarmed early (P1) | **Accepted** — verified at `snapshot_driver.c:616`, `master_hdf5.c:153/:184`, `hdf5.c:108`, `main.c:435-441`. Integrated as F-14, folded into checklist item 2. |
| codex: F-10's `deltaMvir` bullet incorrect | **Accepted** — `deltaMvir` is a core output property with an Uchuu-annotated range (`core_properties.yaml:131-139`); the draft's "drop it" correction was itself wrong and is corrected in F-10. (opencode had confirmed the draft's bullet from a `simulations/`-only grep — a miss the cross-panel disagreement caught.) |
| codex: F-3's traffic/time estimate unsupported; D3 should be benchmark-first | **Accepted** — F-3 reworded, D3 recommendation reversed to defer/benchmark-first (opencode concurred independently). |
| codex: F-4/D1 overstated; drop from pre-Shin gate | **Partially accepted** — the "no present defect" half is accepted and now explicit in F-4; the recommendation stands only in ride-along form on D2's already-required evidence run (D1(a)), never as its own pre-production refactor. opencode endorsed the merge outright; the split is recorded in D1 for the owner's call. |
| codex: bloat list (drop worktree-prune note, F-12 near-term work, runtime cycle tests as prerequisite) | **Accepted** — F-7 reframed as optional defence-in-depth with the producer-side checks noted; F-11's prune note dropped; F-12 marked explicitly not pre-Shin-Uchuu work. |
| opencode: F-2 overstated as a projected abort (315M < 10⁹) | **Accepted** — F-2 now states explicitly it is a record correction with conditional reachability, not a projected abort. |
| opencode: F-5's "remove the memset" is wrong; zeroing is defensive | **Accepted** — F-5 rewritten to keep the memset and carry only the residency-accounting consequence. (Both the draft and the round-1 panel text mis-stated part of the memset's size arithmetic — the seed capacity is itself slab-sized; the accepted conclusion is unaffected.) |
| opencode: F-6 line citation off (583 not 571) | **Accepted** — corrected. |
| opencode M1: checklist lacks a complete-run subset rehearsal (P1) | **Accepted** — integrated as F-15 and folded into checklist item 9. |
| opencode M2/M3: record the §2.2 conclusion explicitly; gate re-run mandatory after the pool fix | **Accepted** — folded into checklist items 5 and 2 respectively. |

**Round 1 rejections:** none — every round-1 panel claim was accepted, in whole or in the adjudicated form above. (An earlier revision of this table claimed codex had framed the F-13 items as previously unknown defects; that was a misreading — codex's own finding already identified the converter README's recorded deferrals and criticized their absence from the readiness plan, the same framing F-13 uses. Corrected here at codex's round-2 request.)

**Round 2 (on Revision 2):** opencode — **ACCEPT**, no P0/P1 remaining, all four production-blocking items re-verified line-level against the code. codex — **REVISE**, one P1: F-14's integration required checking only close statuses while master construction also ignores `H5Gcreate`/`H5Acreate`/`H5Awrite` results (`master_hdf5.c:62-71`, `:141-150`), so an immediate metadata-write failure could still reach cleanup-disarm with a broken master and exit 0. **Accepted and verified** — F-14 and checklist item 2 widened to checked failure propagation across partition finalization and master construction, cleanup armed until all succeed. Codex's round-2 P2 (D4 must include a measured retain/optimize decision on the fix-up stage, `scripts/convert/fixups.py:344-354`) and P3 (§1 wording separating the three blockers from the F-2 ceiling correction) also accepted and applied. One opencode round-2 note **rejected with evidence**: it claimed the tree branch also hard-codes the format literal, but `tree_driver.c:557` sets `.format_name = reader->name` on the tree side versus the `"snapshot_hdf5"` literal at `:583` — F-6 stands as written. Both panels confirmed the round-1 adjudications faithful apart from the two record corrections above, and neither found new bloat in the revision.

**Round 3 (on Revision 3):** codex — **ACCEPT, 0 P0 / 0 P1**; confirmed its round-2 P1 faithfully resolved (F-14 now covers immediate create/write failures and deferred close failures with cleanup armed until success) and the D4 fix-up addition faithful. Its D5 fact-check confirmed the comparator's partition aggregation and the recommendation's soundness, and qualified three containment claims (partition→snapshot mapping absent from the seam; per-partition flush is new work; the master's Cartesian partition-link loop needs adapting) — all verified and folded into D5's text, so the slice is scoped as implementation work rather than assumed-existing behaviour. **Panel convergence: both reviewers accept with no P0/P1 outstanding.** Decision D5 itself was raised by the owner after the first draft; its factual basis was verified directly by the lead reviewer and covered by the codex round-3 pass.

---

## 8. Implementation record (2026-08-13)

The accepted recommendations were implemented in the change set committed alongside this report, under the same discipline as the review itself: subagent-authored edits with disjoint file ownership, every diff verified by the lead reviewer against the report's contracts and `docs/STYLE-GUIDE.md`, and the result re-reviewed by the same external panel to convergence.

**What landed.** D2(a): geometric chunk growth in the galaxy pool (doubling to a `GALAXY_POOL_MAX_CHUNK` cap of 2²² galaxies; block count well under 200 chunks per pool at Shin-Uchuu scale), with a new stable-pointer/reuse unit test. F-14: checked failure propagation (FATAL on negative status) for the critical HDF5 create/open/write/close operations across `src/io/output/hdf5.c`, `master_hdf5.c`, `metadata_hdf5.c`, the snapshot driver's partition close, and — added at panel round 2 of the implementation review — the tree driver's own partition close, which the original sweep missed. D1(a): the four duplicated functions consolidated into a new shared `src/core/halo_evolution.c` (`process_halo_evolution`, static `setup_module_context`, `count_fof_subhalos`, `make_halo_init_payload`) rather than staying in `build_model.c` — the file split keeps `build_model.c` tree-driver-specific per the dual-driver plan's file inventory and lets the unit harness link the shared adapters without the tree traversal it deliberately stubs; both drivers now pass their own workspace explicitly, and the generated payload populator has exactly one instantiation. F-6: snapshot provenance records the resolved reader's name. F-7: bounded-iteration cycle guards on the driver-local progenitor walks (both traversals) and in shared `count_fof_subhalos` (int64 guard counter, safe at the format's INT32_MAX slab ceiling); the second FoF traversal deliberately relies on the first's certification, with a comment. F-8: `open_run` validates the compiled-in generated read list against the format table via the same X-macro include the slab fill uses, failing fast with the package name before any per-snapshot scan. F-11: gate-harness hygiene (single `yaml.safe_load` parse, dead code removed). F-10 and §6: `POST-PHASE-5-WORK.md` and `SHIN-UCHUU-CONVERSION-PLAN.md` corrected and re-ordered as this report's §6 specifies, with the conversion plan's stale ~150–250 GB rank-sort figures annotated everywhere they appear and the converter scale-engineering pass recorded as a pre-conversion obligation. Two test-harness fixes the validation ladder forced: `tests/unit/run_tests.sh` links `halo_evolution.c` (the `build_halo_tree` stub in `test_stubs.c` stays, now with a comment explaining the design), and `test_master_hdf5_partitions.c` supplies a resolved snapshot-reader fixture, mirroring its tree-side sibling.

**Evidence.** Default-pair binary galaxy output proven bitwise-identical across all 64 files before and after the change (the D1 merge's neutrality proof); 45/45 unit, 7/7 integration, and the scientific tier + physics baseline green; `make USE-HDF5=no` clean at 0 warnings; `check-format`, `check-generated`, `validate-modules`, `check-docs` all pass. Implementation panel: codex round 1 REVISE (1 P1 — the tree-driver close gap — plus two P2 cycle-guard refinements and style corrections, all accepted, verified, and fixed), opencode round 1 ACCEPT (0 P0/P1), codex round 2 **ACCEPT (0 P0/P1, all round-1 findings verified resolved)**. The mandatory cross-format identity gate re-run (checklist item 2) is a post-commit operation by construction — the gate builds from committed worktrees — and its result is recorded in the commit history immediately following this change set.

**Checklist state after this change set.** Item 2 of §6 is closed (pool ceiling, output-finalization statuses, D1 consolidation — pending the post-commit gate re-run). Items 1, 3–9 remain open: the `Spin` decision, the converter scale-engineering pass (D4 — the largest remaining item), the D5 output-partitioning choice, the memory recompute, the 10⁹ population check, the particle mass, the range calibration, and the Shin-Uchuu subset rehearsal.
