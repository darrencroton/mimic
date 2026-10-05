# Mimic Distributed Snapshot Operations Implementation Plan

**Purpose:** Make the horizontal driver run across MPI ranks, each rank holding only its own forests' rows of every snapshot slab, with snapshot-global modules obtaining their whole-population quantities through a small set of core collectives, so that a horizontal run's per-process memory is a function of the rank count and the largest forest rather than of the largest slab, and so that the same run file produces the same galaxies whatever the rank count.

**Status:** Planning contract, revision 3 (2026-10-05). Nothing below is implemented. The owner's architectural decisions are frozen under [Frozen Decisions](#frozen-decisions) after an investigation of the code at the planning baseline (seven read-only investigations of the horizontal driver, the horizontal reader and format, today's MPI and output paths, the snapshot-callback contract and the two snapshot modules, the test and gate infrastructure, the converter's row ordering, and the house plan conventions). Revision 1 was reviewed by an independent panel (Codex `gpt-6.1-sol` and Claude Fable 5.1, both high effort, read-only); every finding was verified against the repository and revision 2 resolves them: the collectives slice now precedes the driver slice so no accepted commit runs an unported snapshot module across ranks; the collective gate admits direct calls from unit tests; the rank collective requires local id uniqueness and states cross-rank uniqueness as the caller's precondition; the `-np 1` assertions are separated from the multi-rank ones; the heavy-forest test assertion that contradicted D3 was replaced by the exact greedy packing; the unit runner's explicit source list is in the surfaces that add core sources; `source_format` is published by the reader; the non-existent FoF self-index check was deleted; the rank prefix lives in the shared log emitter; the memory bound is stated through local counts; the sham fixture gate must assign and mask; a small MPI control test covers the collectives' error agreement; the serial references are captured once at run preparation; and the wrong anchors were corrected. Revision 3 followed the panel's focused re-review: the Slice 6 callback-gate test now exempts the root predicate and names the probe-module pattern it uses, the Slice 9 gate runs once in its CI form, the MPI control test's link set is stated, one anchor was corrected, and the pathway's last stale sentence about reworking the decomposition was amended. This plan is the implementation plan for the requirements brief [`MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`](MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md), roadmap step 3; where the two differ, this plan governs, because the brief predates the investigation.

**Planning baseline:** the last commit that changes anything outside this file, [`MIMIC-DEVELOPMENT-PATHWAY.md`](MIMIC-DEVELOPMENT-PATHWAY.md) and the status note of the brief. Every `path:line` anchor below was read at that baseline. The hash is deliberately not written here: PM binds a run to the SHA-256 of this file's bytes, so recording a hash that moves with unrelated commits would force a re-freeze. Recheck drift against the anchors before executing a slice; do not silently rebase a contract onto a changed interface.

**Owner:** [`MIMIC-DEVELOPMENT-PATHWAY.md`](MIMIC-DEVELOPMENT-PATHWAY.md) → step 3. Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md).

---

## Frozen Decisions

| # | Decision | Owner's choice (2026-10-05) |
|---|---|---|
| D1 | **Decomposition unit** | **The forest, as a contiguous row range per slab.** Rank `r` owns the forests whose `ForestIndex` lies in `[forest_cuts[r], forest_cuts[r+1])`, and in every snapshot slab those forests' rows are one contiguous range `[row_cuts[s][r], row_cuts[s][r+1])`. Nothing a rank processes needs a row another rank holds: progenitor, descendant and FoF links never leave a forest (producer-enforced, `convert/mimic-convert/links.py:394-420`), inheritance is pull-based through those links, created records marshal inside their host's segment, and the step 6 transfer graph lives inside one FoF workspace. There are no ghost regions and no communication inside the FoF sweep. Spatial decomposition is rejected for this plan: it cuts forests and would need per-snapshot exchange of progenitor state. |
| D2 | **Precondition: forest-blocked slabs** | A dataset is **forest-blocked** when, in every slab, `ForestIndex` is non-decreasing along the row order. Version 3 datasets converted from `lhalo_binary` or `consistent_trees_hdf5` sources have this by construction: `ForestIndex` is the file-prefix unit number (`HORIZONTAL-HDF5-FORMAT.md:437-443`) and rows are sorted by `SourceHaloID`, a prefix sum over the same (file, unit) order (`:407`, `:429`); the producer checks the resulting per-forest contiguity for those two sources (`convert/mimic-convert/validate_v3.py:117-120` declares them, `:1285-1300` checks). It is **not a format guarantee**, so a distributed run verifies it at startup over every slab and aborts naming the dataset's `source_format` when it fails. Version 2 (rows sorted by `MostBoundID`, `:114`) and version 3 datasets from `consistent_trees_ascii` (dense forest-id enumeration, file-spanning forests split into per-file units) are not forest-blocked; a distributed run refuses them with a message. Measured on a scratch version 3 conversion of the micro-Uchuu ASCII trees: every forest is one contiguous block and the forest order is the same in every slab, but `ForestIndex` descends 4,707 times per slab, so the check refuses it; a converter change that made the ASCII route's `ForestIndex` follow unit order would make such datasets distributable with no runtime change, and is recorded for the pathway's version 3 follow-up. Serial runs (`NTask <= 1`) are unaffected by the precondition and never run the check. No format change: the format document gains a non-normative note recording the consumer dependency, nothing else. |
| D3 | **Partition rule** | **Minimum-makespan contiguous partition on the widest slab's per-forest halo counts**, computed on rank 0 and broadcast. Weights `w_f` are forest `f`'s halo count in the widest slab (largest header `n_halos`; ties → the lowest snapshot number); forests absent from it weigh 0. `forest_cuts` is the contiguous partition into `NTask` ranges that minimises the largest range weight, found by binary search on the capacity `M` over `[max w_f, Σ w_f]` with left-to-right greedy packing (a range closes when the next forest would exceed `M`); at the minimal feasible `M*` the greedy packing is taken as is, so trailing ranks may be idle when fewer than `NTask` ranges are needed, and a heavy forest may share its range with lighter neighbours that fit under `M*` (weights `[1, 5, 1]` on two ranks give `M* = 6` and the ranges `[1, 5]` and `[1]`). `row_cuts[s][r]` is the first row of slab `s` whose `ForestIndex >= forest_cuts[r]` (`row_cuts[s][NTask] = n_halos(s)`). The partition minimises the widest slab's largest per-rank weight exactly; for the other slabs it is a proxy. The result is a pure function of (dataset, `NTask`). |
| D4 | **Reader interface** | `load_slab` takes an explicit row range `[row_lo, row_hi)` and publishes `row_offset` on the slab; `open_run` takes an options struct whose `validate_columns` flag lets ranks other than 0 skip the whole-column data scans (the structural and header checks still run on every rank) and publishes the version 3 header's `source_format` in `struct HorizontalRunInfo` (empty for version 2); a new `scan_forest_index(snapnum, visitor, user)` hook streams one slab's `ForestIndex` column in blocks. The reader stays MPI-free and partition-agnostic: link validation remains against the dataset's global counts and no reader check compares a link to the row's own index (none exists today and none is added), so the serial driver passing the whole range leaves a serial run's reader behaviour unchanged. |
| D5 | **Link rebasing in the driver** | After loading its range the driver rewrites the five link fields to **local row indices** (FoF links minus the slab's `row_offset`; progenitor and descendant links minus the target snapshot's `row_cuts[t][r]`) through generated role setters `mimic_tree_set_<role>()`, and aborts on any link that leaves the rank's range of its target snapshot ("forest cut"). Everything downstream — FoF discovery, `count_fof_subhalos`, `horizontal_resolve_progenitor`, inheritance, the virial helpers, marshalling, output — then runs unchanged on a slab that is locally self-consistent. Serial runs load the whole slab with `row_offset = 0` and skip the rebase pass entirely. |
| D6 | **Identity** | Tree-row `UniqueGalaxyID` is unchanged: it is read from the slab's `ForestIndex`/`HaloRankInForest` columns (`src/core/horizontal_driver.c:516-530`). Created-record identity stays `-(1 + ordinal + 1024 × (row + rows_per_unit × unit))` with `unit = snapnum` and `rows_per_unit` = the largest global slab (`:541-553`), and `struct RecordIdentitySpace` gains one appended member `row_offset` so that `row` is the host's **global** slab row (`HaloNr + row_offset`) at the two sites that encode and guard it (`src/core/module_registry.c:1373-1376`, `:1399-1403`). `HaloNr` itself stays the local row (it indexes the view; it is `output: false`). Created IDs are therefore identical across rank counts. |
| D7 | **Output layout** | Under `NTask > 1` every rank writes one HDF5 partition per requested output snapshot, named `<base>_<snap:03d>_task<task:03d>.hdf5`, and rank 0 writes the master after the existing barrier with external links under `Snap<snap:03d>/File<snap:03d>_task<task:03d>`; a rank holding no galaxies at a snapshot still writes its (empty) partition. Under `NTask <= 1`, including an MPI build run at `-np 1`, output is byte-for-byte today's (`<base>_<snap:03d>.hdf5`, `File<snap:03d>`, no partition log). `struct OutputPartitionSource` gains `partition_task(p)` (−1 for no task component, which the vertical source returns); the identity comparator accepts the optional `_task<digits>` suffix and gains a flag to compare created rows when both runs are the same driver. |
| D8 | **Snapshot collectives** | Snapshot-global modules obtain every whole-population quantity through **core collectives**: `module_snapshot_rank()` (exact global rank of `(double value, int64 id)` keys, descending value, ascending id), `module_snapshot_sum_i64()`, `module_snapshot_sum_f64()`, `module_snapshot_min_max_f64()`, `module_snapshot_any()` (logical or, for collective failure agreement) and `module_snapshot_is_root_task()`. They are **refused while an `init`, full-halo, per-event, by-galaxy or `cleanup` callback is running** (those are not synchronised across ranks) and legal during snapshot dispatch and when no callback is running (so unit tests may call a `process_snapshot` entry point directly, as they do today). Serial and non-MPI builds implement them as the identity (rank = local sort order). The MPI rank is a sample-sort whose result is a function of the multiset of keys only, so it is identical for every rank count; ids must be unique within each caller's keys (checked, error on every rank) and across ranks (the caller's precondition, which `UniqueGalaxyID` gives by construction since forests are disjoint). Every collective must be reached by every rank in the same order; a module branches only on snapshot-level facts or on a previous collective's result. `struct SnapshotContext` and the `process_snapshot` signature are unchanged. |
| D9 | **Module declaration** | A `process_snapshot` module declares `snapshot_distribution: collective` in `module_info.yaml` when it reaches every whole-population quantity through D8's collectives; the default, `serial_only`, means it assumes the complete population is resident. Startup refuses `NTask > 1` with a `serial_only` module configured under `modules.post_snapshot`, naming the module. This refusal lands **before** the driver's serial guard is lifted, so no accepted commit can run an unported snapshot module across ranks. `sham_rank_match` and `hod_populate` are ported and declare `collective`; the framework fixtures stay `serial_only`. |
| D10 | **MPI lifecycle and thread model** | Ranks are **single-threaded**, `MPI_Init` as today (`MPI_THREAD_SINGLE`, `src/core/main.c:317`). Every MPI call the horizontal driver makes sits between FoF sweeps: the partition broadcast at startup, the D8 collectives inside `execute_post_snapshot()`, and the existing pre-master barrier; none inside a callback, so the step 4 thread-per-forest work needs only `MPI_THREAD_FUNNELED`. Three hygiene fixes that today's vertical MPI runs also lack: `myexit()` calls `MPI_Abort` when `NTask > 1` and the exit code is non-zero (today a failing rank finalises alone and the others hang at the barrier); `write_run_metadata()` runs on rank 0 only (today every rank truncates the same files, `src/core/main.c:497`); every log line (both `log_message()` and `log_io_error()`, through their shared emitter) carries a `task <n>:` prefix, followed by a space, when `NTask > 1`. |
| D11 | **Memory accounting** | `input.retention_memory_ceiling_mb` and the retention accounting are **per rank**: footprints are computed from the rank's local row counts, `rows_per_unit` and the INT_MAX emittability check from the global ones. A rank's resident bound is what the existing accounting computes from its local counts: the sum over its retained generations of local rows times the per-row slab, aux, output-buffer and pool costs, plus the partition tables (`n_forests_total × 8` bytes transiently on rank 0; `snapshot_count × (NTask + 1) × 8` bytes everywhere), with the same exclusions as today (in-sweep growth, workspace, module scratch). D3 minimises the widest slab's largest per-rank weight, which bounds that slab's term exactly and the others approximately. A single forest cannot be split, so the largest forest's share of the widest slab is a floor on what any rank count achieves. |
| D12 | **Acceptance predicate** | **Per-`UniqueGalaxyID` byte equality of every field, tree and created rows, every output snapshot, between a serial non-MPI build and an MPI build at `-np 1, 2, 3, 4, 8`**, through `scripts/compare_cross_format_identity.py`, with no tolerance and no environment override (the files differ in layout; the values do not, which is the pathway's standing "per-`UniqueGalaxyID` equality, not byte equality" constraint). The one documented exception is log-only: `module_snapshot_sum_f64()` may differ from the serial sum in the last bits (reduction order), which affects `hod_populate`'s audit line and no output field. The gate runs on a new committed multi-forest gapped version 3 fixture (CI) and by hand on the real `micro-uchuu-horizontal` dataset (440,651 forests), with per-rank peak RSS recorded. Serial output must stay byte-identical to the pre-plan reference (`make tests-snapshot-global-identity` and the reference outputs captured at run preparation). |
| D13 | **Out of scope, recorded** | Spatial decomposition and ghost regions; environment measures, synchronous radiation fields and lightcone assembly (the brief lists them as motivations; they need spatial primitives this plan does not build); distributing version 2 or non-forest-blocked version 3 data; parallel HDF5; threaded ranks; chunked slab streaming inside a rank; load balancing on anything but the widest slab; output-time filtering; pair identity for created records; any change to the vertical driver beyond D10's hygiene. Also rejected for this plan, with its cost recorded: a sub-forest (FoF-group or row-range) decomposition with a per-snapshot exchange of progenitor galaxies between ranks, which is the only decomposition that would split a percolation super-forest; it needs scattered row selection (FoF groups are not contiguous rows), an all-to-all of galaxy records every snapshot, and a new created-record and output contract. |

---

## Outcome and Limits

After this plan a horizontal run of a forest-blocked version 3 dataset launched with `mpirun -np N ./mimic <run file>` processes each snapshot in parallel over `N` ranks, each holding only its forests' rows of every retained generation, writes `N` partition files per requested snapshot plus one master, and produces, galaxy for galaxy, the bytes the serial run produces. `sham_rank_match` ranks the whole box across ranks and `hod_populate` audits the whole box across ranks. Per-rank memory is what the retention accounting computes from the rank's local rows, which the partition keeps near `1/N` of the widest slab wherever no single forest dominates.

**The super-forest floor, measured.** A forest is never split, so the largest forest's share of the widest slab bounds the memory reduction any rank count can deliver. Measured at the baseline: mini-Millennium 0.96%, micro-Uchuu 1.60%, Millennium 0.068%, mini-Uchuu 0.055%, so those datasets split nearly linearly with rank count; but the production Shin-Uchuu dataset's percolation super-forest (`ForestIndex 0`, source forest `26551468179`) holds 321,253,424 of the widest slab's 519,342,987 halos, 61.86%, so forest sharding reduces Shin-Uchuu's per-rank peak by at most about 1.6× whatever the rank count. **This plan therefore does not, by itself, put Shin-Uchuu on small-memory nodes.** For Shin-Uchuu the levers are the chunked-slab-streaming follow-on inside the super-forest's rank, a converter-side treatment of the linking artifact, or a sub-forest decomposition (D13); which to take is the owner's decision at the pathway, not this plan's. For every other dataset measured, and for full Uchuu whose resolution matches mini-Uchuu's, forest sharding is the right lever.

This plan does not implement: any decomposition that splits a forest; distribution of version 2 datasets or of version 3 datasets from Consistent-Trees ASCII sources as the converter emits them today (so the existing version 2 Shin-Uchuu dataset is not distributable, and a future Shin-Uchuu version 3 conversion is distributable only if the converter's ASCII route emits forest-blocked rows — a converter decision recorded for the pathway's version 3 follow-up, not made here); any reduction of what one rank needs beyond its forests (chunked slab streaming remains the follow-on plan, and D4's range read is the primitive it will reuse); threaded ranks; spatial queries; a load balance on wall-clock; a format change; a CI gate on a real dataset.

---

## Repository Evidence and Design Decisions

Read at the planning baseline:

- **The serial guard.** `src/core/read_parameter_file.c:1582-1587` rejects `NTask > 1` for any non-vertical reader ("horizontal runs are serial"); `tests/unit/test_parameter_parsing.c:849-870` pins the message by re-executing the test with `NTask` forced to 2 (the re-exec helper is at `:593-610`). `docs/DEVELOPER-GUIDE.md:1180` and `docs/USER-GUIDE.md:463` state the restriction.
- **Driver state is instanced and the walk is order-free.** `struct HorizontalDriverState` (`src/core/horizontal_driver.c:227-257`) is a stack local of `run_horizontal_driver()`; `struct HorizontalGeneration` (`:194-201`) holds the slab, `aux`, the processed buffer and the pool. FoF groups are discovered by `FirstHaloInFOFgroup == halonr` over slab order and walked by `NextHaloInFOFgroup` (`:1620-1637`, `src/core/halo_evolution.c:54-75`); nothing assumes FoF or forest row contiguity; nothing reads `ThisTask`/`NTask`.
- **Links are slab-global row indices**, resolved into a retained generation by `horizontal_resolve_progenitor()` (`:308-332`, bound `prog >= generation->view.count`), with `horizontal_first_progenitor()` (`:336-350`) and `horizontal_next_progenitor()` (`:357-374`) supplying target snapshots from the version 3 columns or `snapnum − 1` for version 2. Cross-generation indexing happens at `:429`, `:436-439`, `:457`, `:490-492`; the virial helpers compare `halonr` to `FirstHaloInFOFgroup` (`src/core/virial.c:50`) and the output conversion indexes the slab view with `HaloNr` (`src/module_system/output_helpers.h:68`, `:82`); inheritance sets `HaloNr` from the local descendant row (`src/core/inheritance.c:101`, `:167`). The horizon is the largest `DescendantSnapshot` over the slab's rows (`:845-861`). No output property copies a row index (`CentralHalo` and `HaloNr` are `output: false`, `src/core/core_properties.yaml:66-71`, `:73-79`).
- **Identity.** `horizontal_make_unique_galaxy_id()` (`:516-530`) reads the slab's `halo_rank_in_forest` and `forest_index`; `horizontal_evaluate_record_identity_space()` (`:541-553`) sets `rows_per_unit` to the largest reader halo count and the loop sets `state.identity.unit = snapnum` (`:1598`). `mimic_encode_created_galaxy_id()` (`src/include/galaxy_id.h:116-125`) takes `row = host->HaloNr` at `src/core/module_registry.c:1399-1403`, guarded at `:1373-1376`. `struct RecordIdentitySpace` (`src/include/types.h:50-56`) documents that members are only ever appended; `record_identity_space_evaluate()` returns a designated-initialiser literal (`src/core/halo_evolution.c:112-137`), so an appended member is zero for the vertical driver and every hand-built space.
- **Reader.** `struct HorizontalReader` (`src/io/horizontal/reader.h:136-160`) has five hooks, dispatched through `REQUIRE_HORIZONTAL_READER_HOOK` (`src/io/horizontal/interface.c:39`); `struct SnapshotSlab` (`:108-118`) carries the raw halos and the identity, target-snapshot and `SourceHaloID` columns. `load_slab` (`src/io/horizontal/read_horizontal_hdf5.c:2652-2736`) reads the `RawHalo` members in 8192-row hyperslab blocks (`fill_member`, `:1675-1726`; `horizontal_h5_read_block` already takes `(offset, count)`) but reads the six aux columns whole with `H5S_ALL` (`horizontal_h5_read_column`, `:1612-1644`), so a range read needs hyperslab reads for those too. `open_run` (`:2293-2609`) scans the `SnapNum`, `HaloRankInForest` and `ForestIndex` columns of every snapshot (`:2536-2563`), keeps the header's `source_format` privately (`:316`) and publishes `struct HorizontalRunInfo` (`reader.h:47-58`), which does not include it. Link validation (`:2172-2224`) ranges links against the per-snapshot `halo_counts[]` table (`:345-354`) and never compares a link to its own row; FoF self-reference is explicitly a producer obligation (`:53-58`). The driver calls `open_run` once (`src/core/horizontal_driver.c:1553`) and `load_slab` once (`:1182`).
- **Format.** Version 3 rows are sorted by `SourceHaloID` (`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md:407`, `:422`), a prefix sum over (source file ordinal, within-file unit ordinal, row ordinal) (`:429`); `ForestIndex` is the file-prefix unit number for `lhalo_binary` and `consistent_trees_hdf5` and a dense forest-id enumeration for `consistent_trees_ascii` (`:437-443`); the sidecar's ordinals are −1 for ASCII forests spanning files (`:457-467`); version 2 rows are sorted by `MostBoundID` (`:114`). The version 3 header carries `source_format` (`:277-280`); the three target-snapshot columns are mandatory in version 3 (`:306-313`). The producer's unit-forest contiguity check is `convert/mimic-convert/validate_v3.py:1285-1300` (declaration at `:117-120`); cross-forest links are rejected by `convert/mimic-convert/links.py:394-420`. Every local version 3 dataset (`mini-millennium-horizontal`, `micro-uchuu-horizontal`, `millennium-horizontal`, `mini-uchuu-horizontal` from L-Halo; `micro-uchuu-hdf5-horizontal` from forests-HDF5) is forest-blocked by construction.
- **MPI today.** `MPI_Init`/`Comm_rank`/`Comm_size` at `src/core/main.c:317-319` (no `MPI_Init_thread`); `MPI_Finalize` in `bye()` (`:127`); the one collective is the pre-master `MPI_Barrier` (`:426`); the master and `horizontal_driver_clear_output_paths()` run on rank 0 (`:436-443`); `write_run_metadata()` runs on every rank (`:497`); `myexit()` (`:108-115`) has no `MPI_Abort`; `FATAL_ERROR` is `src/util/error.h:99-103`; `log_message()` (`src/util/error.c:312-322`) and `log_io_error()` (`:334-348`) both format through `emit_log()` (`:239`). The vertical driver assigns partitions without communication (`src/core/vertical_driver.c:401`, `:441-496`), its `effective_task_count()`/`current_task_id()` are file-static (`:119-121`), and `NTask` is recorded as `RunProperties/NCores` (`src/io/output/metadata_hdf5.c:666-667`). No horizontal file reads `ThisTask` or `NTask`.
- **Output.** `struct OutputPartitionSource` (`src/io/output/util.h:89-102`); the horizontal source is one partition per requested snapshot with the snapshot number as output id (`src/core/vertical_driver.c:692-731`); `output_path_hdf5()` is `<base>_%03d.hdf5` (`src/io/output/util.c:35-41`); the master links `Snap%03d/File%03d/Galaxies` by output id and republishes each file's `TotHalosPerSnap` (`src/io/output/master_hdf5.c:119-193`); the driver writes each partition inside `horizontal_write_output()` (`src/core/horizontal_driver.c:730-782`) and arms the master path at `:156`. The plot reader iterates every `File*` subgroup (`plot/mimic-plot/hdf5_reader.py:14-15`, `:83`), so task-suffixed group names need no plot change. The one-file-per-snapshot contract is asserted at `tests/framework/parity_gate.py:1557-1566`, `tests/integration/test_processing_order.py:484-487` and `tests/unit/test_master_hdf5_partitions.c:445`, `:633`.
- **Snapshot contract.** `process_snapshot(const struct SnapshotContext *, const struct Halo *, int64_t)` (`src/core/module_interface.h:559-560`), context at `:303-315`; `execute_post_snapshot()` (`src/core/module_registry.c:1601-1643`) dispatches with `RUNNING_CALLBACK_SNAPSHOT` (enum at `:114-122`, entered at `:1633`) over exactly `cur->processed` (`src/core/horizontal_driver.c:1260-1273`, `:1639`), with no count shortcut. `creation_caller_allowed()` (`:1250-1257`) is the precedent for a dispatch-gated core API. The mode descriptors are `src/core/processing_modes.c:39-50` and `scripts/module_modes.py:89-103`; `validate_modules.py:480-495` validates the mode list; `scripts/generate_module_registry.py:770-781` emits each module's registration and `:1030-1040` the unit-test module source list. The module template is FoF-only (`src/module_system/template/README.md:60`); `src/module_system/` is marked do-not-modify for framework code.
- **The two snapshot modules.** `sham_rank_match` ranks by `(ShamVpeak desc, UniqueGalaxyID asc)` (`models/sham/modules/sham_rank_match/sham_rank_match.c:560-570`) over a scratch copy of the population (`:608-676`, duplicate-id check over the whole population at `:622-630`), assigns `StellarMass` from the integer rank alone through `rank_density()` (`:259-262`, which uses `MimicConfig.BoxSize`, `:420`), writes nothing on any failure (`:688-721`) and logs `SHAM audit z= candidates= assigned= masked=` (`:725-726`), which its integration test parses (`_tests/test_integration_sham_rank_match.py:111`); its unit test shuffles the population against a brute-force rank (`_tests/test_unit_sham_rank_match.c:739-835`) and calls `sham_rank_match_process_snapshot()` directly outside any dispatch (`:217`). `hod_populate`'s audit is its `process_snapshot` (`models/hod/modules/hod_populate/hod_populate.c:798-839`): a min/max scan fixes the bin layout (`:669-716`, `:819-825`), a second pass fills bins (`:727-770`), the log prints `%.6e` sums, and there are three failure returns (`audit_scan` at `:815-817`, no host at `:827-830`, `fill_audit_bins` at `:832`); its unit test also calls the entry point directly (`_tests/test_unit_hod_populate.c:1645`).
- **Comparator, gates and the unit runner.** `scripts/compare_cross_format_identity.py` is byte-exact per `UniqueGalaxyID`, matches `^<base>_(\d+)\.hdf5$` (`:105-128`), compares tree rows only (`:311-330`) and exits 0/1/2. The real-data gates are `simulations/<pkg>/_tests/scientific/test_cross_format_identity.py` over `tests/framework/parity_gate.py`; the snapshot-global battery is `tests/manual/run_snapshot_global_battery.py` (`make tests-snapshot-global`, `Makefile:888-900`, marker policy at `:30-33`); the disabled-mode identity gate is `make tests-snapshot-global-identity`. Fixture tests derive their run files from `models/<model>/input/<model>_mini-millennium-horizontal.yaml` with the fixture path substituted (`simulations/mini-millennium-horizontal/_tests/integration/test_gap_retention.py:106-138`). Fixtures: `simulations/mini-millennium-horizontal/_tests/data/{worked_graph,three_snapshot_chain,adjacent}/`, each one forest, regenerated from L-Halo sources by `_tests/data/regenerate.sh` and `_tests/data/source/generate_sources.py` (one tree per file, `:118-125`). No committed run-able fixture has more than one forest and gapped links. `tests/unit/run_tests.sh` links an explicit `CORE_SRCS` list (`:142`), so a new core source must be added there to be linked into unit tests; the framework fixture modules compile only under `TEST_BUILD=yes` (`Makefile:86-93`).
- **CI and environment.** `.github/workflows/ci.yml:95-135` (`horizontal-v3`) installs `libhdf5-dev libyaml-dev pkg-config` only, no MPI; nothing in `tests/` runs under `mpirun`. Locally `mpicc`/`mpirun` are Open MPI 5.0.9; the Makefile's `USE-MPI=yes` sets `CC=mpicc` and `-DMPI` (`Makefile:286-297`) and its compile-mode marker rebuilds on a `USE-MPI` switch (`:353-375`). `tests/unit/run_tests.sh` compiles with plain `gcc` and no `-DMPI`, so unit tests exercise the serial paths only.

**Measured at the baseline (2026-10-05, this host):** `mini-millennium-horizontal` 29,585 forests and 1,533,122 halos over 64 snapshots (`links_adjacent = 0`); `micro-uchuu-horizontal` 440,651 forests and 22,580,924 halos over 50 snapshots, 3.2 GB; `mini-uchuu-horizontal` largest slab 39,798,251 halos (`simulations/mini-uchuu-horizontal/README.md:19`); Shin-Uchuu (version 2) largest slab 519,342,987 halos (`simulations/shin-uchuu/README.md:42`), 68,294,028 distinct forests in that slab, 166,547,771 forests in the dataset. Largest forest's share of the widest slab, read from each dataset's `ForestIndex` column: mini-Millennium 362 of 37,804 (0.96%), micro-Uchuu 9,940 of 621,360 (1.60%), Millennium 12,585 of 18,619,466 (0.068%), mini-Uchuu 21,955 of 39,798,251 (0.055%), Shin-Uchuu 321,253,424 of 519,342,987 (61.86%). `ForestIndex` was non-decreasing in every slab of every local version 3 dataset (64 + 50 + 50 + 64 + 50 slabs) and in neither version 2 dataset.

---

## The Distribution Contract

Frozen for Slices 1 to 9 and repeated in the receipts that bind each part.

**Startup (driver, `NTask > 1` only).** After `horizontal_reader_open_run()` on every rank (rank 0 with `validate_columns = 1`, the others with 0), the driver refuses `format_version == 2` with a message that distribution needs a forest-blocked version 3 dataset. Rank 0 then (a) scans the widest slab's `ForestIndex` into per-forest weights, (b) computes `forest_cuts` by D3, (c) scans every slab's `ForestIndex`, verifying it is non-decreasing (abort on the first violation, naming the snapshot, the two rows, their values and the run info's `source_format`) and recording `row_cuts[s][r]`, and (d) broadcasts `forest_cuts` and `row_cuts`. Every rank then processes every snapshot with its own range. Serial runs (`NTask <= 1`) take none of these steps and load whole slabs.

**Per snapshot (every rank).** `load_slab(snapnum, row_cuts[s][r], row_cuts[s][r+1], &slab)`; rebase the five links (D5), aborting on a link outside the rank's range of its target snapshot; `state.identity.row_offset = row_cuts[s][r]`; the FoF sweep, coverage check (`members_processed == local nhalos`), `execute_post_snapshot()`, publication, release and output proceed as today on local indices. Empty ranges are ordinary empty generations and still take part in every collective.

**Collectives (core, `src/core/snapshot_collectives.h`).** Two kinds of return. *Status* functions return 0 on success and −1 on error: `module_snapshot_rank(ctx, const struct SnapshotRankKey *keys, int64_t count, int64_t *ranks)`, `module_snapshot_sum_i64(ctx, int64_t *values, int n)`, `module_snapshot_sum_f64(ctx, double *values, int n)`, `module_snapshot_min_max_f64(ctx, double *minima, double *maxima, int n)` (all reduce in place). *Predicates*: `module_snapshot_any(ctx, int flag)` returns 1 or 0 for the logical or and −1 on error; `module_snapshot_is_root_task(void)` returns 1 on rank 0 and in serial, else 0, and cannot fail. Every function except `is_root_task` is refused (`ERROR_LOG`, −1) while the running callback is `INIT`, `FULL_HALO`, `PER_EVENT`, `BY_GALAXY` or `CLEANUP`, and allowed during `SNAPSHOT` and `NONE`. `struct SnapshotRankKey { double value; int64_t id; }`; rank 0 is the largest value; ties by ascending `id`; a NaN value or a duplicate `id` within the caller's keys is an error on every rank; cross-rank uniqueness is the caller's precondition. In serial or non-MPI builds every function is the identity (rank = position in the local sort). MPI algorithm for the rank: each rank sorts locally and checks local uniqueness; `NTask − 1` splitters are chosen from regular samples (`min(count, 256)` per rank, allgathered); keys go to their bucket's owner (`MPI_Alltoallv`); each owner sorts its bucket and assigns `global_offset(bucket) + position`, where the offsets come from an exclusive scan of bucket sizes (a duplicate id meeting inside a bucket is also reported); ranks travel back and are unpermuted; errors are agreed with `MPI_Allreduce`/`MPI_LOR` so every rank returns the same status. The reductions are `MPI_Allreduce` wrappers (the f64 sum is identical on every rank of one run and may differ from a serial sum in the last bits). Rule for module authors: every rank reaches every collective in the same order; branch only on `ctx` facts or on a prior collective's result; never on local counts.

**Output (`NTask > 1`).** Rank `t` writes `<base>_<snap:03d>_task<t:03d>.hdf5` for every requested snapshot; rank 0 writes the master after the barrier, linking `Snap<snap>/File<snap>_task<t>/Galaxies` for every `(snap, t)` and republishing each file's `TotHalosPerSnap`; `RunProperties/NCores` records `NTask` as today. Under `NTask <= 1` names and master groups are today's.

**Footguns stated to developers.** A link that leaves a rank's range is a dataset defect or a partition bug, never something to repair. `rows_per_unit` is the global largest slab on every rank; computing it from local counts changes created IDs. `HaloNr` stays local: the output conversion indexes the slab view with it. The reader validates links against global counts and the driver rebases afterwards; do not validate against local counts in the reader. All ranks run `open_run`, so any abort there is identical on every rank; after the partition broadcast, a rank-local abort reaches the others only through `MPI_Abort` (D10). Non-MPI unit builds compile the serial paths, so MPI paths are proven by the gate and its MPI control test (Slice 9), not by `run_tests.sh`. A new core source is linked into unit tests only if `tests/unit/run_tests.sh`'s `CORE_SRCS` names it.

---

## Implementation Profiles and Run Preparation

| Slice | Recommended Developer | Effort | Reason |
|---|---|---|---|
| 1 | Claude Sonnet | high | Pure partition logic with a brute-force oracle test; no I/O, no MPI |
| 2 | Claude Opus | high | Reader vtable change across 2,800 lines of HDF5 code; range reads for every column |
| 3 | Claude Sonnet | medium | Five generated setters in one generator function plus `make generate` |
| 4 | Claude Sonnet | high | Three small cross-cutting lifecycle fixes in the core error, logging and metadata paths |
| 5 | Claude Opus | medium | Output partition source, naming, master links, comparator regex, pinned contracts |
| 6 | Claude Opus | high | The collective API, the sample-sort rank and the metadata key through generator and validator |
| 7 | Claude Opus | high | The driver's distributed path: partition, range load, link rebase, identity, lifting the guard |
| 8 | Claude Opus | medium | Porting two modules to the collectives with bit-identical serial results |
| 9 | Claude Opus | medium | A multi-forest fixture through the converter, the distributed gate, the MPI control test, the real-data stage |
| 10 | Claude Sonnet | medium | One CI job gains MPI and the fixture gate |
| 11 | Claude Sonnet | high | Vision, guides, format note, skills, pathway, changelog; the closeout record |

PM executes each slice separately in plan order; no batching. Use the installed `opus` and `sonnet` aliases, record the resolved model versions, and pass the table's model and effort explicitly to `start-slice`. Reviewer recommendation: Claude Fable 5.1 (`claude-fable-5-1`), high effort, in fresh sessions for drift audit and code review. An unavailable model is a setup blocker, never permission to substitute silently.

Slices 2, 4, 5, 6, 7, 8, 9, 10 and 11 require recorded human approval because they change shared interfaces, core execution, model physics, build or test entry points, CI or the vision. Approval may be recorded for all of them before the run with PM's `approve` command; this document grants none. Slices 1 and 3 run unattended.

Before PM initialisation, in order:

1. Commit this plan, the pathway update and the brief's status note. PM's `init` refuses a tree dirty outside `.pm/` and binds the run to this file's bytes; any later edit stops the run.
2. Run `python3 ~/.claude/skills/project-manager/scripts/pm.py check-plan --plan docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-IMPLEMENTATION-PLAN.md --repo .` and resolve every warning.
3. Confirm the environment can run `make`, `make USE-MPI=yes` (Open MPI `mpicc` and `mpirun` on PATH), `mimic_venv`, HDF5 and the committed fixtures, and that `simulations/micro-uchuu-horizontal/snapshots` resolves.
4. Confirm `make tests-snapshot-global-identity` and `make tests-horizontal-v3` pass at the baseline.
5. Capture the serial reference outputs Slices 8 and 9 compare against: at the baseline, non-MPI builds of `sage16`, `sham` and `hod` run on `micro-uchuu-horizontal` through their `models/<model>/input/<model>_micro-uchuu-horizontal.yaml` run files into `archive/distributed-references/<model>/` (gitignored `archive/`), with the commit hash and `make info` output recorded beside them.

Run on a feature branch the owner names at `init` (PM refuses implicit `main`); no implementation session may edit this plan, change dependency manifests or licences, regenerate baselines, or push; remote CI is read by the owner after the run. Every slice uses one `MODEL`/`SIMULATION` pair per command sequence and restores the default pair's generated code before its final default-tier run. Format check: run the full unmodified `./scripts/beautify.sh`, then `make check-format`; a failure attributable only to files outside the repository tree is recorded as pre-existing and not fixed.

---

## Slice 1: Forest-block partition logic

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. No prerequisites beyond the planning baseline.
- Add the MPI-free, I/O-free partition module that turns per-forest weights into `forest_cuts` (D3) and a streamed `ForestIndex` column into a monotonicity verdict plus `row_cuts`, link it into the unit-test build, and test it against a brute-force oracle.

### Acceptance Criteria

- [ ] `src/core/horizontal_partition.h` declares `struct HorizontalForestPartition { int ntask; int64_t snapshot_count; int64_t n_forests_total; int64_t *forest_cuts; int64_t *row_cuts; }` (`forest_cuts` has `ntask + 1` entries; `row_cuts` has `snapshot_count × (ntask + 1)` entries, row-major by snapshot) with `horizontal_partition_create(ntask, snapshot_count, n_forests_total)` and `horizontal_partition_destroy()` using tracked `MEM_HALOS` allocations.
- [ ] `horizontal_partition_cut_forests(const int64_t *weights, int64_t n_forests_total, int ntask, int64_t *forest_cuts)` implements D3 exactly: binary search on the capacity over `[max w_f, Σ w_f]` with left-to-right greedy packing, `forest_cuts[0] = 0`, `forest_cuts[ntask] = n_forests_total`, non-decreasing cuts, trailing idle ranges allowed; `ntask == 1` yields `[0, n_forests_total]`; `n_forests_total == 0` yields all-zero cuts; a weight sum that would overflow int64 is a `FATAL_ERROR`.
- [ ] `horizontal_partition_accumulate_weights(int64_t *weights, int64_t n_forests_total, const int64_t *values, int64_t count)` adds one to each forest's weight for each streamed `ForestIndex` value and aborts on a value outside `[0, n_forests_total)`.
- [ ] A streaming scan object `struct HorizontalForestScan` with `horizontal_forest_scan_begin(scan, partition, snapnum)`, `horizontal_forest_scan_visit(scan, first_row, const int64_t *values, int64_t count)` and `horizontal_forest_scan_end(scan)`: `scan_end` returns 0 when the column was non-decreasing and fills `row_cuts[snapnum][0..ntask]` as `lower_bound` of each `forest_cuts[r]` (`row_cuts[snapnum][ntask]` = rows seen), and −1 after recording the first violation (`row`, previous value, offending value) in the scan object.
- [ ] `tests/unit/run_tests.sh` adds `src/core/horizontal_partition.c` to `CORE_SRCS`; the Makefile's `src/core/*.c` glob picks the source up for the executable without a Makefile change (if it does not, stop and report rather than editing the build).
- [ ] `tests/unit/test_horizontal_partition.c` (picked up by the unit-test glob) checks, one `MIMIC_RESULT:` marker per `TEST_RUN` case: the computed makespan equals a brute-force optimum over all contiguous partitions for every weight vector of length ≤ 8 with weights in `{0, 1, 2, 5}` and `ntask ∈ {1, 2, 3, 4}`; weights `[1, 5, 1]` on two ranks give cuts `[0, 2, 3]` (makespan 6); zero-weight forests are assigned and never cut the row order; `ntask` greater than the number of non-zero forests leaves idle ranges with `row_cuts[s][r] == row_cuts[s][r+1]`; `row_cuts` equals `lower_bound` on hand-built non-decreasing columns delivered in blocks of varying size including a block boundary inside a forest run; a decreasing pair is reported with its row and values; an empty column yields `row_cuts[s][r] == 0` for every `r`.
- [ ] Required evidence: clean default build, `make tests-unit` via a subagent (the new test passes, nothing else changes), `./scripts/beautify.sh`, `make check-format`; no generated file touched.

### Authorized Surface

- Files allowed to change:
  - `src/core/horizontal_partition.h` (new)
  - `src/core/horizontal_partition.c` (new)
  - `tests/unit/test_horizontal_partition.c` (new)
  - `tests/unit/run_tests.sh` (the `CORE_SRCS` list only)
- Functions/classes/components allowed to change: the new module and the one source-list line. No change to the driver, the reader or the Makefile.
- Tests allowed or expected to change: the new unit test.

### Explicit Non-Goals

- No MPI call, no HDF5 call, no driver wiring, no documentation beyond the header's Doxygen contract.

### Risk Flags

- Risky surfaces touched: none
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: `tests/unit/test_horizontal_partition.c`.
- Commands to run: `make clean && make`; `make tests-unit` (subagent, pass/fail summary); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the brute-force oracle to confirm it enumerates every contiguous partition, not a sample.

### Rollback Path

- Revert the slice commit; the module is additive and nothing calls it yet.

---

## Slice 2: Reader row ranges, column scan hook and open options

### Intended Change

- Recommended Developer: Claude Opus; effort: high. No prerequisites beyond the planning baseline.
- Extend the horizontal reader interface per D4: `load_slab` over an explicit row range publishing `row_offset`, `open_run` taking an options struct with `validate_columns` and publishing `source_format`, and a `scan_forest_index` streaming hook; keep the serial driver's behaviour byte-identical by passing the whole range.

### Acceptance Criteria

- [ ] `src/io/horizontal/reader.h`: `struct HorizontalOpenOptions { int validate_columns; }`; `open_run(const struct HorizontalOpenOptions *options, struct HorizontalRunInfo *info)`; `struct HorizontalRunInfo` gains `char source_format[33]` (the version 3 header string, empty for version 2).
- [ ] `load_slab(int64_t snapnum, int64_t row_lo, int64_t row_hi, struct SnapshotSlab *slab)` with the contract `0 <= row_lo <= row_hi <= snapshot_halo_count(snapnum)` (abort otherwise), `slab->nhalos == row_hi − row_lo` and `slab->row_offset == row_lo` (new `int64_t row_offset` member, 0 in the empty state).
- [ ] `typedef void (*horizontal_forest_index_visitor)(int64_t first_row, const int64_t *values, int64_t count, void *user)`; `scan_forest_index(int64_t snapnum, horizontal_forest_index_visitor visit, void *user)` streams the whole `ForestIndex` column of one snapshot in ascending row order in blocks of at most `HORIZONTAL_HDF5_SCAN_BLOCK` rows without allocating per halo. The dispatchers in `src/io/horizontal/interface.c` gain the new hook with the `REQUIRE_HORIZONTAL_READER_HOOK` fail-fast and the new arguments.
- [ ] `src/io/horizontal/read_horizontal_hdf5.c`: every `/halos` dataset (the generated `RawHalo` members, `ForestIndex`, `HaloRankInForest`, the three version 3 target-snapshot columns, `SourceHaloID`) is read for exactly rows `[row_lo, row_hi)` through hyperslab selections, including the six aux columns that today use `H5S_ALL`; link validation keeps ranging links against the global `halo_counts[]`; no check compares a link to the row's own index and none is added.
- [ ] When `options->validate_columns == 0` the per-snapshot `SnapNum`, `HaloRankInForest` and `ForestIndex` data scans are skipped and the header values are published as the measured ones, while every structural, header, schema and identity-bound check still runs; a NULL `options` aborts.
- [ ] The serial driver (`src/core/horizontal_driver.c`) passes `validate_columns = 1` at its one `open_run` call and the range `[0, snapshot_halo_count(snapnum))` at its one `load_slab` call, so `make tests-snapshot-global-identity` and `make tests-horizontal-v3` pass unchanged.
- [ ] `tests/unit/test_horizontal_v3_reader.c` gains cases on the committed `tests/data/horizontal_v3/dataset/` and `simulations/mini-millennium-horizontal/_tests/data/worked_graph/` fixtures: a range read equals the corresponding rows of a whole read for every column including the aux columns; `row_offset` is published; an empty range `[k, k)` yields `nhalos == 0` and releases cleanly; `row_hi > count` and `row_lo > row_hi` abort; `scan_forest_index` delivers the full column in order with the expected row offsets; `validate_columns = 0` opens a dataset the full scan also accepts and publishes the header's `max_halo_rank_in_forest` and `n_forests_total`; `source_format` reads `lhalo_binary` for the fixtures. Existing callers of `load_slab`/`open_run` in tests are updated to the new signatures with the whole range.
- [ ] Required evidence: clean default build, `make tests-horizontal-v3`, the three default tiers via a subagent, `make tests-snapshot-global-identity`, `./scripts/beautify.sh`, `make check-format`; no baseline refresh, no test weakened.

### Authorized Surface

- Files allowed to change:
  - `src/io/horizontal/reader.h`
  - `src/io/horizontal/interface.c`
  - `src/io/horizontal/read_horizontal_hdf5.c`
  - `src/core/horizontal_driver.c` (the `open_run` call and the `load_slab` call only)
  - `tests/unit/test_horizontal_v3_reader.c`
  - `tests/unit/` (existing tests adapted to the new signatures only)
  - `simulations/mini-millennium-horizontal/_tests/unit/` (existing tests adapted to the new signatures only)
  - `simulations/micro-uchuu-ascii-horizontal/_tests/` (existing C reader tests adapted to the new signatures only)
- Functions/classes/components allowed to change: the vtable, the dispatchers, `open_run`, `load_slab`, the block and column readers, and the driver's two call sites.
- Tests allowed or expected to change: the reader unit test and signature-only updates to existing callers.

### Explicit Non-Goals

- No partition logic, no MPI, no link rebasing, no change to what is validated beyond the `validate_columns` skip, no change to the format document, no change to version 2 semantics.

### Risk Flags

- Risky surfaces touched: reader vtable (shared interface), slab contract.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: `tests/unit/test_horizontal_v3_reader.c`; signature updates to existing callers.
- Commands to run: `make clean && make`; `make tests-horizontal-v3`; `make tests-unit`, `make tests-integration`, `make tests-scientific` (subagent); `make tests-snapshot-global-identity`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required. Run differential code-health against the slice's starting commit and supply it as review evidence (interface change).
- Manual checks: compare the whole-read and range-read code paths for the hyperslab offsets of vector datasets (`Pos`, `Vel`, `Spin`).

### Rollback Path

- Revert the slice commit; the serial path is the whole range, so nothing downstream depends on the new arguments yet.

---

## Slice 3: Generated link-role setters

### Intended Change

- Recommended Developer: Claude Sonnet; effort: medium. No prerequisites beyond the planning baseline.
- Have the property generator emit `mimic_tree_set_<role>(struct HaloInputView view, int64_t halonr, int64_t value)` for the five link roles beside the existing getters, so a driver can rewrite link fields by role without naming package-specific field names.

### Acceptance Criteria

- [ ] `scripts/generate_properties.py` (`generate_tree_property_accessors_h`) emits, for each of `Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup` and `NextHaloInFOFgroup`, a `static inline void mimic_tree_set_<role>(struct HaloInputView view, int64_t halonr, int64_t value)` that stores `value` cast to the field's declared C type into the field the package maps to that role; the view's `halos` pointer is `const struct RawHalo *`, so the generated setter casts away const once, with a one-line comment stating the driver owns the slab it rebases.
- [ ] `make generate` regenerates `src/include/generated/tree_property_accessors.h` for the default pair and for `MODEL=halos-only SIMULATION=mini-millennium-horizontal`; `make check-generated` passes for both; the default build compiles with no new warning.
- [ ] Required evidence: `make generate`, `make check-generated`, clean default build, `make tests-unit` via a subagent, `./scripts/beautify.sh`, `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `scripts/generate_properties.py`
  - `src/include/generated/tree_property_accessors.h` (regenerated only, never hand-edited)
- Functions/classes/components allowed to change: `generate_tree_property_accessors_h` and its helpers.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No setters for non-link roles, no use of the setters yet, no change to the getters or the view type.

### Risk Flags

- Risky surfaces touched: generated accessors header.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make generate`; `make check-generated`; `make MODEL=halos-only SIMULATION=mini-millennium-horizontal generate check-generated` then restore the default pair; `make clean && make`; `make tests-unit` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: diff the regenerated header; only the five setters are new.

### Rollback Path

- Revert the slice commit and run `make generate`.

---

## Slice 4: MPI lifecycle hygiene

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. No prerequisites beyond the planning baseline.
- Apply D10's three fixes, each independent of the horizontal driver and visible to today's vertical MPI runs: `MPI_Abort` on a failing rank, rank-0-only run metadata, and a task prefix on log lines under `NTask > 1`.

### Acceptance Criteria

- [ ] `myexit()` (`src/core/main.c`) under `#ifdef MPI`: when `NTask > 1` and the exit code is non-zero it calls `MPI_Abort(MPI_COMM_WORLD, code)` after printing its message; a zero exit code and serial runs behave as today.
- [ ] `write_run_metadata()` is called only when `ThisTask == 0` (serial runs have `ThisTask == 0` and are unchanged); the call stays after the memory-system cleanup so the guard is a plain comparison.
- [ ] `emit_log()` (`src/util/error.c`, shared by `log_message()` and `log_io_error()`) prefixes every line with `task <ThisTask>:` and a space when `NTask > 1`, and emits exactly today's bytes when `NTask <= 1` (the integration tests that assert `VERBOSE_LOG` retention lines pass unchanged). Progress-bar output is not a log line and is out of scope.
- [ ] An MPI build (`make USE-MPI=yes`) of the default pair runs `mpirun -np 2 ./mimic models/sage16/input/sage16_mini-millennium.yaml` to completion with one `metadata/version_info.json` written once, and a deliberate `FATAL_ERROR` provoked on one rank (for example an unreadable `output.output_directory` injected only for the check, not committed) terminates the whole job instead of hanging; the two observations are recorded in the slice summary.
- [ ] Required evidence: clean default build, the three default tiers via a subagent, the MPI build and the two runs above, `./scripts/beautify.sh`, `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `src/core/main.c`
  - `src/util/error.c`
  - `src/util/error.h` (only if a declaration is needed for the prefix)
- Functions/classes/components allowed to change: `myexit`, the `write_run_metadata` call site, `emit_log`.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No change to the horizontal driver, the vertical driver's partition logic, the progress bar, or `bye()`; no new logging macro; no rank prefix in serial runs.

### Risk Flags

- Risky surfaces touched: core error path, run metadata.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make clean && make`; default tiers (subagent); `make clean && make USE-MPI=yes`; the two `mpirun -np 2` runs; `make clean && make` to restore the non-MPI build; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the `mpirun -np 2` log and confirm every log line (INFO, WARNING, I/O) carries a task prefix and the serial log carries none.

### Rollback Path

- Revert the slice commit; the three changes are local and independent.

---

## Slice 5: Task-aware output partitions and comparator

### Intended Change

- Recommended Developer: Claude Opus; effort: medium. No prerequisites beyond the planning baseline (the driver still refuses `NTask > 1`; this slice is proven by unit tests with `NTask` set by hand, as `tests/unit/test_enumerated_driver.c` does).
- Implement D7: a `partition_task` hook on the partition source, task-suffixed partition names and master links under `NTask > 1`, the horizontal source enumerating `NOUT × NTask` partitions of which a rank writes its own, and the comparator's acceptance of the new names plus created-row comparison.

### Acceptance Criteria

- [ ] `struct OutputPartitionSource` (`src/io/output/util.h`) gains `int (*partition_task)(int partition)` returning −1 for a partition with no task component; the vertical source returns −1; `output_path_hdf5(char *buf, size_t size, int filenr, int task)` produces `<dir>/<base>_<filenr:03d>.hdf5` for `task < 0` and `<dir>/<base>_<filenr:03d>_task<task:03d>.hdf5` otherwise; every caller passes the partition's task.
- [ ] `effective_task_count()` and `current_task_id()` move from `src/core/vertical_driver.c` into a new header `src/core/task_layout.h` as static inline functions used by both drivers.
- [ ] The horizontal source (`src/core/vertical_driver.c`) under `effective_task_count() > 1` enumerates `NOUT × NTask` partitions with `partition_output_id(p) = ListOutputSnaps[p % NOUT]`, `partition_task(p) = p / NOUT`, `partition_exists = 1`, single-snapshot selections; under `NTask <= 1` it is exactly today's source with `partition_task = −1`.
- [ ] `horizontal_write_output()` and the in-flight path arming (`src/core/horizontal_driver.c`) write only partitions whose task is `current_task_id()` (serial: all), using the task-suffixed name; `horizontal_arm_master_output_path()` arms on rank 0 only.
- [ ] The master (`src/io/output/master_hdf5.c`) names the group `Snap<snap:03d>/File<filenr:03d>` for `task < 0` and `Snap<snap:03d>/File<filenr:03d>_task<task:03d>` otherwise, links the task-suffixed relative file name, and republishes `TotHalosPerSnap` per group as today; serial master bytes are unchanged (`make tests-snapshot-global-identity` passes).
- [ ] `scripts/compare_cross_format_identity.py`: `partition_files()` matches `^<base>_(\d+)(?:_task(\d+))?\.hdf5$` and orders by `(snapshot, task)`; mixing suffixed and unsuffixed names in one run is an input error (exit 2); a new `--compare-created` flag compares rows with negative `UniqueGalaxyID` by the same byte-exact per-ID rule (default off, so existing gates are unchanged) and duplicate detection covers created ids when the flag is on; `tests/scientific/test_compare_cross_format_identity.py` gains cases for task-suffixed names, mixed names, and `--compare-created` on equal and differing created rows.
- [ ] `tests/unit/test_master_hdf5_partitions.c` gains a case with `NTask = 3`, `NOUT = 2` set by hand: six partitions, ids and tasks as specified, names and master group names as specified, `partition_task == −1` for the vertical source; the existing one-partition-per-snapshot case still passes with `NTask` unset.
- [ ] Required evidence: clean default build, the three default tiers via a subagent, `make tests-snapshot-global-identity`, `./scripts/beautify.sh`, `make check-format`; no baseline refresh, no test weakened. (The plot reader already iterates every `File*` subgroup, `plot/mimic-plot/hdf5_reader.py:14-15`, `:83`; no plot change.)

### Authorized Surface

- Files allowed to change:
  - `src/io/output/util.h`
  - `src/io/output/util.c`
  - `src/io/output/master_hdf5.c`
  - `src/io/output/hdf5.c` (call sites of `output_path_hdf5` only)
  - `src/core/vertical_driver.c`
  - `src/core/horizontal_driver.c`
  - `src/core/task_layout.h` (new)
  - `scripts/compare_cross_format_identity.py`
  - `tests/scientific/test_compare_cross_format_identity.py`
  - `tests/unit/test_master_hdf5_partitions.c`
- Functions/classes/components allowed to change: the partition source structs and both sources, the path helper and its callers, the master writer, the horizontal write and arming helpers, the comparator's file discovery and row selection, the two task helpers.
- Tests allowed or expected to change: the two tests named above.

### Explicit Non-Goals

- No lifting of the `NTask > 1` guard, no partition logic, no change to binary output, no change to per-file `RunProperties`, no change to `parity_gate.py` or `test_processing_order.py` (serial contract untouched), no change to the output schema, no plot change.

### Risk Flags

- Risky surfaces touched: output file naming (user-visible layout), master file structure, shared output struct.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: `tests/unit/test_master_hdf5_partitions.c`, `tests/scientific/test_compare_cross_format_identity.py`.
- Commands to run: `make clean && make`; default tiers (subagent); `make tests-snapshot-global-identity`; `mimic_venv/bin/python tests/scientific/test_compare_cross_format_identity.py`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: `h5ls -r` a serial master before and after the slice and confirm identical object names.

### Rollback Path

- Revert the slice commit; serial naming is unchanged, so no output written before the slice is affected.

---

## Slice 6: Snapshot collectives and the distribution declaration

### Intended Change

- Recommended Developer: Claude Opus; effort: high. No prerequisites beyond the planning baseline (the collectives depend on `NTask`, not on the driver).
- Add D8's collective API with serial identity and MPI implementations, and D9's `snapshot_distribution` metadata key through validator, generator, registry and the startup refusal, so that when Slice 7 lifts the serial guard an unported snapshot module is already refused under `NTask > 1`.

### Acceptance Criteria

- [ ] `src/core/snapshot_collectives.h` declares `struct SnapshotRankKey { double value; int64_t id; }` and the six functions of the Distribution Contract with its signatures and return kinds (status functions 0/−1; `module_snapshot_any` 1/0/−1; `module_snapshot_is_root_task` 1/0, no error path).
- [ ] `src/core/snapshot_collectives.c`: every function except `is_root_task` consults the registry's running-callback kind and is refused (`ERROR_LOG`, −1) during `INIT`, `FULL_HALO`, `PER_EVENT`, `BY_GALAXY` and `CLEANUP`, allowed during `SNAPSHOT` and `NONE`; under `NTask <= 1` or a non-MPI build `module_snapshot_rank` sorts locally by `(value desc, id asc)` and returns positions, the reductions return their inputs, `any` returns `flag != 0`, `is_root_task` returns 1; a NaN value or a duplicate `id` within the caller's keys returns −1; scratch is tracked `MEM_UTILITY` and freed before return.
- [ ] Under MPI (`#ifdef MPI`, `NTask > 1`): the rank is the sample-sort of the Distribution Contract with local uniqueness checked before the exchange and every error agreed through `MPI_Allreduce`/`MPI_LOR` so all ranks return the same status; the reductions are `MPI_Allreduce` with `MPI_SUM`, `MPI_MIN` and `MPI_MAX`; `any` is `MPI_Allreduce` with `MPI_LOR`; `is_root_task` is `ThisTask == 0`.
- [ ] `module_info.yaml` accepts `snapshot_distribution: serial_only | collective` only when `supported_processing_modes` contains `process_snapshot` (`scripts/validate_modules.py` errors otherwise, and on any other value); the default is `serial_only`; `scripts/generate_module_registry.py` emits `.snapshot_distribution = SNAPSHOT_DISTRIBUTION_<VALUE>` into each module's registration (new `enum SnapshotDistribution` and `struct Module` member in `src/core/module_interface.h`).
- [ ] `validate_post_snapshot_entries()` (`src/core/module_registry.c`) refuses, with `ERROR_LOG` naming the module and `NTask`, any `serial_only` module configured under `modules.post_snapshot` when `NTask > 1`.
- [ ] The template documentation (`src/module_system/template/README.md` and the comments of `template_module_info.yaml`) and `scripts/module_modes.py`'s key documentation mention `snapshot_distribution`; no framework code under `src/module_system/` changes beyond regeneration. `make validate-modules` passes for every model package; `make generate` and `make check-generated` pass for the default pair and for `MODEL=sham SIMULATION=micro-uchuu-horizontal`.
- [ ] `tests/unit/run_tests.sh` adds `src/core/snapshot_collectives.c` to `CORE_SRCS`.
- [ ] `tests/unit/test_snapshot_collectives.c` (non-MPI build) checks: rank equals a brute-force rank on random keys with ties, including all-equal values; a duplicate id returns −1; a NaN returns −1; `count == 0` succeeds; the reductions and `any` are identities; every function except `module_snapshot_is_root_task` returns −1 while a full-halo callback kind is active (entered through a hand-built probe module driven by `execute_module_pipeline()`, as `tests/unit/test_record_creation.c:171-197` does, never through a test-only setter) and succeeds with none active, while `module_snapshot_is_root_task` returns 1 in both states; and, with `NTask = 2` set by hand as `tests/unit/test_enumerated_driver.c:237-238` does, `validate_post_snapshot_entries()` refuses a `serial_only` module and accepts a `collective` one.
- [ ] `tests/integration/test_snapshot_module_schema.py` gains cases for the key's acceptance, rejection on a non-snapshot module, and rejection of an unknown value.
- [ ] Required evidence: clean default build, `make check-generated`, `make validate-modules`, the three default tiers via a subagent, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, `./scripts/beautify.sh`, `make check-format`; no baseline refresh, no test weakened.

### Authorized Surface

- Files allowed to change:
  - `src/core/snapshot_collectives.h` (new)
  - `src/core/snapshot_collectives.c` (new)
  - `src/core/module_interface.h`
  - `src/core/module_registry.h` (the running-callback accessor if one must be exported)
  - `src/core/module_registry.c`
  - `scripts/validate_modules.py`
  - `scripts/generate_module_registry.py`
  - `scripts/module_modes.py`
  - `src/module_system/generated/` (regenerated only)
  - `src/module_system/template/README.md`
  - `src/module_system/template/template_module_info.yaml` (comments only)
  - `tests/unit/run_tests.sh` (the `CORE_SRCS` list only)
  - `tests/unit/test_snapshot_collectives.c` (new)
  - `tests/integration/test_snapshot_module_schema.py`
  - `tests/unit/test_snapshot_module_contract.c` (only if the new `struct Module` member needs a designated initialiser there)
- Functions/classes/components allowed to change: the new collectives, `validate_post_snapshot_entries`, the validator and generator for the one key, the template documentation.
- Tests allowed or expected to change: the three tests named above.

### Explicit Non-Goals

- No change to `struct SnapshotContext` or the `process_snapshot` signature, no module ported yet, no collective outside the six, no change to the horizontal driver, no MPI test in `run_tests.sh` (the MPI path is proven in Slice 9).

### Risk Flags

- Risky surfaces touched: module-facing API, module metadata schema, generated registry.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the three tests named above.
- Commands to run: `make generate`; `make check-generated`; `make validate-modules`; `make clean && make`; default tiers (subagent); `make tests-snapshot-global`; `make tests-snapshot-global-identity`; `make MODEL=sham SIMULATION=micro-uchuu-horizontal generate check-generated` then restore the default pair; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the sample-sort once end to end for the empty-rank and single-bucket cases and for the error-agreement path.

### Rollback Path

- Revert the slice commit and run `make generate`; nothing calls the collectives yet.

---

## Slice 7: The distributed horizontal driver

### Intended Change

- Recommended Developer: Claude Opus; effort: high. Requires Slices 1 to 6.
- Wire the distribution contract into the horizontal driver: startup partitioning on rank 0 with broadcast, version 2 and non-forest-blocked refusals, per-rank range loads, link rebasing with the forest-cut abort, the identity `row_offset`, per-rank memory accounting, and lift the config-time guard. The first real `mpirun` fixture runs happen here.

### Acceptance Criteria

- [ ] `src/core/read_parameter_file.c` no longer rejects `NTask > 1` for horizontal readers; `tests/unit/test_parameter_parsing.c` replaces `test_ntask_multi_rejects_horizontal_processing_order` with a case asserting the configuration is accepted (the vertical-unaffected case stays).
- [ ] `run_horizontal_driver()` under `NTask > 1` (MPI builds; the non-MPI build takes the serial path unconditionally): calls `open_run` with `validate_columns = (ThisTask == 0)`; aborts with `FATAL_ERROR` naming `format_version 2` and the forest-blocked requirement when `info.format_version == 2`; on rank 0 computes weights from the widest slab (lowest snapshot on ties) through `scan_forest_index` and `horizontal_partition_accumulate_weights`, computes `forest_cuts` with `horizontal_partition_cut_forests`, scans every slab through `horizontal_forest_scan_*` and aborts on a monotonicity violation naming the snapshot, both rows and values, and `info.source_format`; broadcasts `forest_cuts` and `row_cuts` with `MPI_Bcast`; logs the partition (per-rank forest range and widest-slab weight) on rank 0 at `INFO_LOG`.
- [ ] Under `NTask <= 1` none of the startup steps run, whole slabs are loaded with `row_offset = 0`, no partition line is logged, and output names are today's.
- [ ] Per snapshot each rank loads `[row_cuts[s][r], row_cuts[s][r+1])`, then rebases: `FirstHaloInFOFgroup` and `NextHaloInFOFgroup` (non-negative values) minus `slab.row_offset`; `FirstProgenitor`, `NextProgenitor` and `Descendant` (non-negative values) minus `row_cuts[t][r]` where `t` is the link's target snapshot from the version 3 columns; a rebased value outside `[0, row_cuts[t][r+1] − row_cuts[t][r])` is a `FATAL_ERROR` naming the link, the snapshot, the global row and the target snapshot ("the forest is cut by the partition"). The rebase uses `mimic_tree_set_<role>()`. Serial runs skip the pass.
- [ ] `struct RecordIdentitySpace` gains an appended `int64_t row_offset`; the driver sets `state.identity.row_offset = row_cuts[s][r]` beside `.unit = snapnum`; `module_registry.c` guards and encodes with `host->HaloNr + ws->identity.row_offset`; the vertical driver and every hand-built space leave it 0; `tests/unit/test_record_creation.c` gains a case showing a non-zero `row_offset` shifts the host key by exactly that amount.
- [ ] Footprints, the retention ceiling, `aux` and output-buffer sizing use the local count; `rows_per_unit` and the INT_MAX emittability check use the global `halo_counts`; the coverage check compares against the local count.
- [ ] `simulations/mini-millennium-horizontal/_tests/unit/test_unit_horizontal_retention.c` or a new `tests/unit/test_horizontal_distribution.c` covers, in the non-MPI build by driving the rebase helper directly on a hand-built slab: FoF and progenitor links rebased to local indices; a link outside the range aborts (through the fork-and-re-execute pattern of `tests/unit/test_parameter_parsing.c:593-610`); `row_offset = 0` leaves a slab unchanged.
- [ ] MPI build gate for this slice: `make MODEL=halos-only SIMULATION=mini-millennium-horizontal USE-MPI=yes`, with a run file derived from `models/halos-only/input/halos-only_mini-millennium-horizontal.yaml` pointing at the `worked_graph` fixture as `test_gap_retention.py` does: `mpirun -np 1` writes today's unsuffixed partitions and no partition log and its output compares equal to the serial non-MPI run; `mpirun -np 2` (one forest, so rank 1 is idle) logs the partition, writes `_task000` and `_task001` partitions and a master linking both, and compares equal to the serial run through `compare_cross_format_identity.py`; the same two runs for `sage16`; results recorded in the slice summary.
- [ ] Required evidence: clean default build, `make check-generated`, the three default tiers via a subagent, `make tests-horizontal-v3`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, the MPI gate above, `./scripts/beautify.sh`, `make check-format`; no baseline refresh, no test weakened.

### Authorized Surface

- Files allowed to change:
  - `src/core/horizontal_driver.c`
  - `src/core/read_parameter_file.c`
  - `src/core/module_registry.c` (the two identity sites only)
  - `src/include/types.h` (`struct RecordIdentitySpace`)
  - `src/core/halo_evolution.c` (only if `record_identity_space_evaluate` must initialise the new member)
  - `tests/unit/test_parameter_parsing.c`
  - `tests/unit/test_record_creation.c`
  - `simulations/mini-millennium-horizontal/_tests/unit/test_unit_horizontal_retention.c`
  - `tests/unit/test_horizontal_distribution.c` (new, optional)
  - `docs/DEVELOPER-GUIDE.md` (the one sentence at `:1180` stating horizontal runs are serial only; the full section is Slice 11's)
  - `docs/USER-GUIDE.md` (the one sentence at `:463`)
- Functions/classes/components allowed to change: the driver's startup, acquisition, rebase and identity code; the config validator's guard; the two identity sites.
- Tests allowed or expected to change: the four tests named above.

### Explicit Non-Goals

- No module changes, no fixture, no CI, no change to the reader beyond calling the Slice 2 hooks, no load balancing beyond D3, no distributing version 2, no change to the collectives.

### Risk Flags

- Risky surfaces touched: core execution path, identity scheme, configuration validation, MPI.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the four tests named above.
- Commands to run: `make clean && make`; `make check-generated`; default tiers (subagent); `make tests-horizontal-v3`; `make tests-snapshot-global`; `make tests-snapshot-global-identity`; the MPI fixture gate (`USE-MPI=yes` builds of `halos-only` and `sage16` on `mini-millennium-horizontal`, `mpirun -np 1` and `-np 2`, comparator); `make clean && make` to restore; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required. Run differential code-health against the slice's starting commit and supply it as review evidence (structural change).
- Manual checks: read the rank-0 partition log line on the `-np 2` fixture run and confirm the forest range and row ranges match a hand count of the fixture's `ForestIndex` column.

### Rollback Path

- Revert the slice commit; the guard returns and the serial path is untouched by construction (every new step is behind `NTask > 1`).

---

## Slice 8: Port `sham_rank_match` and `hod_populate` to the collectives

### Intended Change

- Recommended Developer: Claude Opus; effort: medium. Requires Slice 7.
- Replace both modules' whole-population assumptions with the collectives, declare `snapshot_distribution: collective`, and keep every serial result bit-identical.

### Acceptance Criteria

- [ ] `sham_rank_match.c`: the whole-population duplicate-id check stays local as today; candidates' keys `{(double)ShamVpeak, UniqueGalaxyID}` go through `module_snapshot_rank()` and the integer rank feeds the unchanged `rank_density`/`mass_at_density` path; a failure flag (validation, unrepresentable mass, `FLT_MAX`, collective error) is agreed with `module_snapshot_any()` before any write, so no rank writes when any rank failed; the audit counts are summed with `module_snapshot_sum_i64()` and logged when `module_snapshot_is_root_task()`; every rank reaches every collective in the same order, including when its local candidate count is 0; `module_info.yaml` declares `snapshot_distribution: collective`.
- [ ] `hod_populate.c`: the three audit failure sites (`audit_scan`, the no-host case, `fill_audit_bins`) set a flag instead of returning, so that `module_snapshot_min_max_f64()` (on `log_min`/`log_max`, before the bin layout is fixed), `module_snapshot_sum_i64()` (on `hosts[]`, `realised[]` and the scalar int counts), `module_snapshot_sum_f64()` (on `expected[]` and the scalar double sums) and the final `module_snapshot_any()` are reached unconditionally by every rank in that order; the log is written by the root task; `module_info.yaml` declares `collective`; the README's audit paragraph states the f64 sums are reduction-order dependent in the last bits.
- [ ] Serial bit-identity: `make tests-snapshot-global` passes unchanged (the brute-force oracle and permutation tests in both unit tests, which call the entry points directly, and the fixture integration tests), and the real-data runs `models/sham/input/sham_micro-uchuu-horizontal.yaml` and `models/hod/input/hod_micro-uchuu-horizontal.yaml` produce output identical per `UniqueGalaxyID` (tree and created rows, `--compare-created`) to the run-preparation references in `archive/distributed-references/`, through `compare_cross_format_identity.py`.
- [ ] Both READMEs document the collective step and the `snapshot_distribution` declaration in one short paragraph each.
- [ ] Required evidence: `make validate-modules`, the two package builds, `make tests-snapshot-global`, the two real-data identity comparisons, `./scripts/beautify.sh`, `make check-format`, `make check-docs`; no test weakened.

### Authorized Surface

- Files allowed to change:
  - `models/sham/modules/sham_rank_match/sham_rank_match.c`
  - `models/sham/modules/sham_rank_match/sham_rank_match.h`
  - `models/sham/modules/sham_rank_match/module_info.yaml`
  - `models/sham/modules/sham_rank_match/README.md`
  - `models/sham/README.md`
  - `models/hod/modules/hod_populate/hod_populate.c`
  - `models/hod/modules/hod_populate/hod_populate.h`
  - `models/hod/modules/hod_populate/module_info.yaml`
  - `models/hod/modules/hod_populate/README.md`
  - `models/hod/README.md`
- Functions/classes/components allowed to change: `rank_into_scratch`, `sham_rank_match_process_snapshot` and helpers; `audit_scan`, `fill_audit_bins`, `hod_populate_process_snapshot` and helpers.
- Tests allowed or expected to change: none (the existing tests are the identity oracle).

### Explicit Non-Goals

- No change to either module's physics, parameters, properties or `pre_timestep`/`post_timestep` steps; no change to the collectives; no change to the test fixtures.

### Risk Flags

- Risky surfaces touched: model physics code paths (results must be bit-identical).
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make validate-modules`; `make MODEL=sham SIMULATION=micro-uchuu-horizontal` and `make MODEL=hod SIMULATION=micro-uchuu-horizontal` builds and runs; `compare_cross_format_identity.py --compare-created` against the run-preparation references; `make tests-snapshot-global`; `make check-docs`; `./scripts/beautify.sh`; `make check-format`; restore the default pair's generated code last.
- Lint (differential, via the `lint` skill): required.
- Manual checks: list every collective call in each module in dispatch order and confirm no call is conditional on a local count.

### Rollback Path

- Revert the slice commit; both modules return to their resident-population implementations and `serial_only`, which Slice 6's startup check refuses under `NTask > 1`.

---

## Slice 9: Multi-forest fixture, the distributed identity gate and the MPI control test

### Intended Change

- Recommended Developer: Claude Opus; effort: medium. Requires Slice 8.
- Add a committed forest-blocked gapped version 3 fixture with several forests, the gate script that proves D12 on it for the four model packages, a small MPI control test of the collectives' error agreement, the make target, and the manual real-data stage on `micro-uchuu-horizontal` with per-rank memory recorded.

### Acceptance Criteria

- [ ] `simulations/mini-millennium-horizontal/_tests/data/source/generate_sources.py` gains a `trees_forest_blocks` source: one L-Halo file holding six trees of unequal size over at least six snapshots (one empty), with at least one gapped link, at least one FoF satellite that becomes an orphan, at least three FoF groups at the final snapshot, and one tree holding at least 40% of the widest snapshot's halos; `regenerate.sh` converts it like the others into `simulations/mini-millennium-horizontal/_tests/data/forest_blocks/` (snapshot files plus `forests.h5`), committed.
- [ ] `simulations/mini-millennium-horizontal/_tests/input/` gains fixture run files for `halos-only`, `sage16`, `sham` and `hod`, each derived from `models/<model>/input/<model>_micro-uchuu-horizontal.yaml` or `<model>_mini-millennium-horizontal.yaml` (whichever exists for the model) with the fixture's paths substituted and at least the final two snapshots requested as output; the `sham` run file sets `ShamTargetLogMassFloor` so that the serial fixture run's `SHAM audit` line shows `assigned >= 1` and `masked >= 1`, which the gate parses and requires.
- [ ] `tests/mpi/test_snapshot_collectives_mpi.c` (new, compiled by the gate with `mpicc -DMPI` and linked with `src/core/snapshot_collectives.c`, the util sources and a test stub supplying the running-callback accessor as `NONE`, not with `module_registry.c`; run at `-np 3`): a brute-force rank oracle across three ranks with one empty rank and cross-rank ties; a NaN on one rank returns −1 on every rank; a duplicate id within one rank returns −1 on every rank; the reductions and `any` match hand sums; no case hangs (the gate runs it under a timeout).
- [ ] `tests/manual/test_distributed_identity.py` (modelled on `run_snapshot_global_battery.py`): for each of `halos-only`, `sage16`, `sham`, `hod` on `mini-millennium-horizontal`: builds the non-MPI binary and runs the fixture serially; builds with `USE-MPI=yes` and runs under `$MPIRUN` (default `mpirun`) at `-np 1, 2, 3, 4, 8`; at `-np 1` requires today's unsuffixed partition names and no partition log; at every other count requires `NTask` `File###_task###` groups per requested snapshot in the master whose `TotHalosPerSnap` sum to the serial file's, and the rank-0 partition log line; for every count runs `compare_cross_format_identity.py --compare-created` against the serial output and requires exit 0; runs the MPI control test; runs the version 2 negative case (`halos-only` on `simulations/micro-uchuu-ascii-horizontal/_tests/data/generic/` under `-np 2` fails at startup with the version 2 message); restores the caller's generated code in `finally`; emits `MIMIC_RESULT:` markers and fails on any SKIP. The serial and MPI binaries of one leg are built with the same `TEST_BUILD` setting (production builds). `Makefile` gains `tests-distributed` running it; `tests/README.md` lists the target.
- [ ] The gate is run once locally, in its CI form `MPIRUN="mpirun --oversubscribe"`, green; log under `build/distributed_tests.log`.
- [ ] Manual real-data stage, run by hand and recorded in `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-ACCEPTANCE.md` (new): `sage16`, `sham` and `hod` on `micro-uchuu-horizontal`, the run-preparation serial references versus `mpirun -np 4` of the MPI build, comparator exit 0 with `--compare-created`; per-rank peak RSS measured per process (`mpirun -np 4 /usr/bin/time -l ./mimic <run file>` times each rank, or an equivalent per-process measurement, not the launcher) and the serial peak RSS tabulated; wall-clock of each run; the rank-0 partition log line (forest ranges and widest-slab weights) quoted; the largest forest's share of the widest slab stated.
- [ ] Required evidence: the gate's own log, the acceptance record, `make tests-horizontal-v3`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, `./scripts/beautify.sh`, `make check-format`, `make check-docs`.

### Authorized Surface

- Files allowed to change:
  - `simulations/mini-millennium-horizontal/_tests/data/source/generate_sources.py`
  - `simulations/mini-millennium-horizontal/_tests/data/source/trees_forest_blocks.0` (new, generated)
  - `simulations/mini-millennium-horizontal/_tests/data/source/forest_blocks.a_list` (new)
  - `simulations/mini-millennium-horizontal/_tests/data/regenerate.sh`
  - `simulations/mini-millennium-horizontal/_tests/data/forest_blocks/` (new, converter output)
  - `simulations/mini-millennium-horizontal/_tests/input/` (new run files)
  - `simulations/mini-millennium-horizontal/README.md` (the fixture list)
  - `tests/mpi/test_snapshot_collectives_mpi.c` (new)
  - `tests/manual/test_distributed_identity.py` (new)
  - `tests/README.md`
  - `Makefile` (the `tests-distributed` target only)
  - `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-ACCEPTANCE.md` (new)
- Functions/classes/components allowed to change: the source generator, the regenerate script, the new gate script, the MPI control test, the make target.
- Tests allowed or expected to change: the new gate and control test.

### Explicit Non-Goals

- No CI change, no change to `parity_gate.py` or the cross-format gates, no change to the converter, no new comparator semantics, no fixture for `micro-uchuu-hdf5-horizontal`, no `serial_only` refusal case under MPI (Slice 6's unit test covers it).

### Risk Flags

- Risky surfaces touched: build/test entry point (Makefile target), committed fixture data.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: `tests/manual/test_distributed_identity.py`, `tests/mpi/test_snapshot_collectives_mpi.c`.
- Commands to run: `simulations/mini-millennium-horizontal/_tests/data/regenerate.sh`; `MPIRUN="mpirun --oversubscribe" make tests-distributed`; `make tests-horizontal-v3`; `make tests-snapshot-global`; `make tests-snapshot-global-identity`; the real-data stage by hand; `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: inspect the fixture's `ForestIndex` column per snapshot (`h5dump` or h5py) and confirm the six blocks and the 40% forest; read the serial `SHAM audit` line and confirm both assigned and masked are non-zero; read the acceptance record's RSS table and confirm the per-rank peak is below the serial peak.

### Rollback Path

- Revert the slice commit; the fixture, scripts and target are additive.

---

## Slice 10: CI runs the distributed gate

### Intended Change

- Recommended Developer: Claude Sonnet; effort: medium. Requires Slice 9.
- Give the `horizontal-v3` CI job an Open MPI build and the fixture gate.

### Acceptance Criteria

- [ ] `.github/workflows/ci.yml`: the `horizontal-v3` job installs `libopenmpi-dev openmpi-bin` beside the existing packages, gains a `timeout-minutes` value, and runs `make tests-distributed` with `MPIRUN="mpirun --oversubscribe"` as its last step; the `test` job is unchanged.
- [ ] The workflow file parses as YAML and the new step's command matches the Slice 9 target and environment variable by inspection; no local re-run of the gate is required (Slice 9 ran it in the CI form).
- [ ] The slice summary records the owner's instruction that CI is read after the run (no push by any session).

### Authorized Surface

- Files allowed to change:
  - `.github/workflows/ci.yml`
- Functions/classes/components allowed to change: the `horizontal-v3` job's steps.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No change to the `test` job, no MPI in the unit-test build, no matrix.

### Risk Flags

- Risky surfaces touched: CI configuration.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `mimic_venv/bin/python -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml'))"`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: read the diff; two package names, one timeout, one step.

### Rollback Path

- Revert the slice commit.

---

## Slice 11: Documentation, skills and pathway closeout

### Intended Change

- Recommended Developer: Claude Sonnet; effort: high. Requires Slice 10.
- Record the delivered contract where it permanently lives, narrow the vision wording, add the format document's non-normative note, refresh the skills, update the changelog and the chunked-streaming note, and close step 3 in the pathway.

### Acceptance Criteria

- [ ] `docs/VISION.md`: Principle 5's horizontal sentence gains one clause stating that under MPI each rank bounds memory to its own forests' rows of the retained generations; Principle 4's snapshot-scope paragraph replaces "Only the horizontal driver holds a complete snapshot, so only it runs this scope" with wording that the horizontal driver holds the complete snapshot collectively across its ranks and that a snapshot module reaches whole-population quantities through the core's snapshot collectives, whose results are functions of the complete population; no other vision change.
- [ ] `docs/DEVELOPER-GUIDE.md`: "The Horizontal Driver" gains a "Distributed operation" subsection stating D1 to D7, D10 and D11 (the partition rule, the forest-blocked precondition and its check, the rebase, per-rank accounting, output layout, thread model); "Record Creation Contract" records `row_offset`; "Snapshot Callback Contract" documents the six collectives, their return kinds, the callback-kind gate, the ordering rule and `snapshot_distribution`; the reader interface section documents the three hook changes; the MPI section states the three lifecycle fixes.
- [ ] `docs/USER-GUIDE.md`: running a horizontal run under `mpirun`, which datasets qualify (forest-blocked version 3; version 2 and ASCII-sourced version 3 refused with the messages), the output layout and the master, `retention_memory_ceiling_mb` per rank, the super-forest floor, and `make tests-distributed`.
- [ ] `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`: one non-normative note under Version 3 recording that Mimic's distributed mode depends on rows being grouped by forest in ascending `ForestIndex`, that unit-forest sources produce this and the ASCII route does not, and that this is a consumer dependency, not a format rule; dated in the errata table as a documentation addition with no version change.
- [ ] `docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`: a two-line status note that step 3 landed (so distribution no longer waits on anything), that D4's range read is the primitive it will reuse, and that output is now per `(snapshot, task)` partition.
- [ ] Skills: `.agents/skills/mimic-architecture-contract/SKILL.md` (sections 3, 4 and 5: the row-offset identity rule, per-rank ownership, the collectives, the reader hooks), `.agents/skills/mimic-run-and-operate/SKILL.md` (MPI horizontal runs and output names), `.agents/skills/mimic-modules/SKILL.md` (`snapshot_distribution` and the collectives), `.agents/skills/mimic-simulations-and-readers/SKILL.md` (the vtable), `.agents/skills/mimic-validation-and-qa/SKILL.md` (`tests-distributed`), `.agents/skills/mimic-config-and-flags/SKILL.md` (the module_info key); each sweep is a few sentences, no restating of the guides.
- [ ] `CHANGELOG.md` Unreleased: the feature, the two refusals, the output naming under MPI, the three lifecycle fixes, the comparator flag, the new make target.
- [ ] `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: step 3 marked complete with the measured outcomes from the acceptance record (per-rank RSS, wall-clock, rank counts gated), the recorded limits (super-forest floor; version 2 and ASCII version 3 not distributable; the converter decision the version 3 follow-up must make before a Shin-Uchuu version 3 conversion), the plan listed for archiving; `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md` status set to executed with a pointer to the acceptance record. Do not move or archive this plan.
- [ ] `make check-docs` passes; `./scripts/beautify.sh` and `make check-format` pass.

### Authorized Surface

- Files allowed to change:
  - `docs/VISION.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/USER-GUIDE.md`
  - `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`
  - `docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-modules/SKILL.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`
  - `README.md` (only if it lists MPI support or horizontal limits)
- Functions/classes/components allowed to change: prose only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code, no new document beyond the acceptance record Slice 9 wrote, no archiving, no change to the format's normative text, no restating of generated lists.

### Risk Flags

- Risky surfaces touched: vision wording, format document.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: every number in the pathway's outcome line traces to the acceptance record; the vision diff is two paragraphs.

### Rollback Path

- Revert the slice commit; documentation only.

---

## Next Chat Prompts

### Mode A — Checkpointed alternative

```text
Plan file: docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-IMPLEMENTATION-PLAN.md
Slices this session: Slice 1

Read the full plan. Stop before coding if any receipt is incomplete or baseline drift
changes its meaning. Work on the current branch; do not create a branch without my instruction.
Use orchestrator as the controlling skill. Act as the Developer: implementation, validation,
Git and commits stay local. Use the slice's recommended Developer model/effort and a fresh
read-only Claude Fable 5.1 high-effort Reviewer for each independent review. Do not substitute
or self-audit.

For the selected slice: restate the frozen contract; stop for my approval if its Risk Flags say
approval is needed; apply scoped-implementation; apply drift-audit through the Reviewer and
report the authorization result before any quality review; on a passing gate apply code-review
through the Reviewer (run differential code-health first where the plan says so); fix findings
and re-run the relevant gate; ask me before committing; commit with the commit skill; then use
the handoff skill to record state and the next slice.

Confirm the plan, selected slice, branch and model/effort before beginning.
```

### Mode B — Supervised execution

```text
Plan file: docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: harness claude model sonnet effort high initially
Reviewer: harness claude model claude-fable-5-1 effort high

Use project-manager. You are the accountable PM and never write slice code.
Read the complete frozen plan; run check-plan with repo context. Require a clean committed
planning baseline and resolve any drift against the plan's anchors first. Create the feature
branch I name at init; never run on main.
Record human approval for each flagged slice (2, 4, 5, 6, 7, 8, 9, 10, 11) before starting it;
neither this launcher nor the plan grants those approvals.
Ask for commit authorization before launch unless I have already explicitly granted it; no
session pushes. Keep PM_RUN_TOKEN private to the PM seat. Preflight make, make USE-MPI=yes
(Open MPI), mimic_venv, HDF5, the committed fixtures, the micro-uchuu-horizontal dataset and
the run-preparation references under archive/distributed-references/ without changing source
data.

For each slice in order:
1. Launch a fresh Developer, explicitly passing that receipt's model and effort to
   start-slice (Sonnet high for 1, 4 and 11; Sonnet medium for 3 and 10; Opus high for 2, 6
   and 7; Opus medium for 5, 8 and 9).
2. Wait with one long observe --wait rather than repeated checks.
3. Check the mechanical floor, then the frozen authorization contract and the actual evidence;
   run lint yourself; rerun the slice's validation commands where risk or doubt warrants.
4. For elevated slices commission fresh independent drift-audit; read and judge it and report
   authorization before commissioning fresh code-review. Both must cover the exact final
   commit. Record reviewer and developer judgments.
5. A contract defect stops the run; do not amend this plan. Never waive the fixture gate or the
   real-data stage, and never accept a serial-identity regression.

Confirm the plan, branch, Developer/Reviewer models and effort, approvals and first slice.
After all slices are decided, read the PM report and report its verified total elapsed time,
accepted commits and evidence, audit provenance, plan defects, stops and residual limitations.
```
