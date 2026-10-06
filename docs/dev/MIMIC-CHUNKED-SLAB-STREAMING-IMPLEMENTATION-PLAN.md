# Mimic Chunked Slab Streaming Implementation Plan

**Purpose:** Bound the horizontal driver's peak memory by a size the run file chooses, down to the largest forest's share of the widest slab, rather than by the widest slab a dataset happens to produce: each task sweeps its contiguous forest range in `input.forest_chunks` sequential sub-ranges, each sub-range swept through every snapshot with its own retained generations, so that peak resident memory is a function of the widest chunk, and the same run file produces the same galaxies whatever the chunk count or task count.

**Status:** Planning contract, revision 3 (2026-10-06). Nothing below is implemented. The owner's architectural decisions are frozen under [Frozen Decisions](#frozen-decisions) after three read-only investigations of the code at the planning baseline (the horizontal driver's loop, state and retention pool; the output lifecycle, the reader's ranged read and the configuration template; the gate, fixture, test and documentation surfaces) and a direct reading of the driver's sweep, partition and output functions. Revision 1 was reviewed by an independent panel (Codex `gpt-6-astra` and Claude Fable 5.1, both high effort, read-only); every finding was verified against the repository and this revision resolves them: the created-record identity path is now gated under chunking through a `hod` run-file variant without the snapshot audit; the `forest_blocks` fixture's chunk cuts were computed and the false "one forest per chunk" leg deleted; the four width refusals and warning are reworded separately, with the per-partition `INT_MAX` cap stated as not lifted by chunking; Slice 2 names every table-stride site; Slice 3 tests the writer at its public seam with no driver hook and the failure window moves to Slice 4's integration test; the cleanup registry is allocation-free; the memory bound counts spare pool capacity; the parse test follows the ceiling test's observable; the vertical-reader double report, the stale-comment sites, three more documentation surfaces, the `--compress` append, and the wording slips were fixed. Revision 3 followed the panel's focused re-review (no serious finding): the `INT_MAX` output refusal is stated as a limit this plan leaves unchanged (it judges the global snapshot count, so neither chunks nor tasks lift it); the Slice 3 writer case lives in the harness that already stubs and counts the per-file metadata writer; Slice 4's failure-window test writes its own fixture-copy helper; the `hod` restriction is qualified to configurations that carry the snapshot audit; and the last revision-1 leftovers were corrected; a closing confirmation round found only a qualification of the `hod` wording in Slices 5 and 6 and two anchors to adjust, both applied. This plan promotes the concept note [`MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`](MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md); where the two differ, this plan governs, because the note predates roadmap step 3 and the investigation.

**Planning baseline:** the last commit that changes anything outside this file, [`MIMIC-DEVELOPMENT-PATHWAY.md`](MIMIC-DEVELOPMENT-PATHWAY.md) and the status note of the concept note. Every `path:line` anchor below was read at that baseline. The hash is deliberately not written here: PM binds a run to the SHA-256 of this file's bytes, so a hash that moves with unrelated commits would force a re-freeze. Recheck drift against the anchors before executing a slice; do not silently rebase a contract onto a changed interface.

**Owner:** [`MIMIC-DEVELOPMENT-PATHWAY.md`](MIMIC-DEVELOPMENT-PATHWAY.md) → current focus. Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md).

---

## Frozen Decisions

| # | Decision | Owner's choice (2026-10-06) |
|---|---|---|
| C1 | **The chunk is a contiguous sub-range of a task's forests, swept through every snapshot in turn.** | A run with `input.forest_chunks = G` divides each task's forest range `[forest_cuts[t], forest_cuts[t+1])` into `G` contiguous sub-ranges (chunks) and runs the whole snapshot loop once per chunk, in chunk order, loading only that chunk's rows `[row_cuts[s][t·G + c], row_cuts[s][t·G + c + 1])` of every slab. A chunk is the same object a task's range is today (a contiguous forest range whose rows are one contiguous range of every slab), so progenitor, descendant and FoF links never leave it (the producer obligation step 3 relies on, `convert/mimic-convert/links.py:394-420`), inheritance stays pull-based inside it, and created records marshal inside their host's segment. The three candidate approaches of the concept note are settled by this: within-snapshot row-range streaming (candidate 1) and the compact previous-slab projection (candidate 3) bound only the raw-slab term, about 140 of the roughly 530 B each retained row costs (see [Repository Evidence](#repository-evidence-and-design-decisions)), and candidate 2 is this decision with the sub-partition made uniform across snapshots. |
| C2 | **Precondition: forest-blocked version 3 input, as for distribution.** | A chunked run (`G > 1`) has the same precondition as a distributed run (decision D2 of step 3): a version 3 dataset whose `ForestIndex` is non-decreasing in every slab. The driver's existing refusals (`format_version 2`; a slab that is not forest-blocked) apply to `G > 1` on one task exactly as they apply to `NTask > 1`, with their messages widened to name the chunk count beside the task count, and the serial `G = 1` run never runs the checks. Consequence, stated plainly: the production version 2 Shin-Uchuu dataset cannot be chunked, exactly as it cannot be distributed; a Shin-Uchuu version 3 conversion is chunkable only if the converter's ASCII route emits forest-blocked rows (the open converter decision recorded in the pathway's version 3 follow-up). |
| C3 | **Two-level partition: tasks first, then chunks within each task.** | The task cuts are computed exactly as today (minimum-makespan over the widest slab's per-forest weights, step 3 decision D3), so `G` never changes which forests a task owns or which rows its partition files hold. Each task's range is then cut into `G` chunks by the same algorithm over that range's weights. `struct HorizontalForestPartition` gains `nchunk`; its tables hold `ntask·nchunk + 1` forest cuts and `snapshot_count × (ntask·nchunk + 1)` row cuts, the task cuts being every `nchunk`-th entry. Two properties follow and are tested: `G = 1` reproduces today's tables entry for entry; and on one task, `G` chunks are the ranges `G` tasks would own (both are the minimum-makespan `G`-partition of the same weights), so a serial run at `forest_chunks: G` loads exactly the rows `mpirun -np G` loads, one range at a time. Trailing idle chunks are allowed, as trailing idle tasks are. The partition is computed on task 0 and broadcast as today, with the larger tables. |
| C4 | **The run-file knob is `input.forest_chunks`, an integer at least 1, default 1.** | Parsed beside `retention_memory_ceiling_mb` in `parse_input_section` with the same strictness (integer scalar; zero, negative, fractional, non-numeric and overflowing values are fatal at configuration; omit for one chunk), stored as `MimicConfig.ForestChunks`, rejected for a vertical reader (which sweeps no slab), and not recorded in `RunProperties`, like the ceiling and unlike `NCores`, because it changes no output. Automatic derivation from `input.retention_memory_ceiling_mb` or from host memory is **out of scope**: the ceiling keeps its one job (refuse a generation before allocation), now applied per chunk, and the driver's existing `--verbose` sizing lines are what a user reads to choose `G`. |
| C5 | **The snapshot scope is refused under chunking.** | A chunked sweep never holds a whole snapshot's population at once, within a task or collectively, so `modules.post_snapshot` with `forest_chunks > 1` is rejected at configuration time in `validate_and_postprocess()`, naming the phase, the first module and the chunk count, and saying that the snapshot scope needs the snapshot co-resident (distribute over tasks instead). This is not a limitation to engineer around: Principle 4 defines the snapshot scope's determinism over the complete population, and the collectives synchronise tasks at one moment, which sequential chunks never share. `sham_rank_match` therefore runs only with `forest_chunks: 1`, and so does `hod_populate` whenever its whole-box audit is configured (its full-halo population step alone, with the `post_snapshot` phase omitted, is chunkable, which is how C9 gates record creation); `sage16` and `halos-only`, the production case this plan exists for, are unaffected. |
| C6 | **Output stays per `(snapshot, task)` partition and is appended per chunk.** | The file layout, names, master links and `TotHalosPerSnap` are exactly today's at every `G`. A partition is created (with its empty `Galaxies` table and per-file `RunProperties`) when chunk 0 reaches its snapshot, reopened read-write and appended by each later chunk, and stamped with `TotHalosPerSnap` and finalised by the last chunk, through the writer's existing batched `H5TBappend_records` path (`src/io/output/hdf5.c:159-188`, which the vertical driver already calls many times per snapshot group before one close). The driver still holds at most one writable output file open. Because chunks are ascending row ranges and the sweep visits FoF groups in row order, the rows a partition holds are in the same order at every `G`, which the gates check. The cleanup registry gains one in-flight slot per requested output snapshot: a partition is armed at creation and released at finalisation, so under `G > 1` a failure removes every partition of this task that is not yet final (at `G = 1` the window is today's, creation to close). |
| C7 | **Retention, sizing, the ceiling and identity are per chunk.** | Within a chunk the retention pool behaves exactly as today for a task: horizons, release at the horizon, the spare pool stack, `horizontal_require_generation_fits()` from the chunk's local counts, and `input.retention_memory_ceiling_mb` against the chunk's pool. Every generation is released by the end of each chunk's sweep (the existing "still retained" check runs per chunk), the spare pools and the FoF workspace carry over, and the run's `R` term and `max_retained_*` are maxima over chunks. `rows_per_unit` stays the global largest slab and `state.identity.row_offset` is the chunk's `row_cuts[s][range]`, so created-record ids are the same at every `G` and every task count (step 3 decision D6 needs no change). Modules are initialised and cleaned up once per process as today; no shipped FoF module keeps cross-snapshot or cross-group state (verified, [Repository Evidence](#repository-evidence-and-design-decisions)), so a sweep that returns to snapshot 0 for the next chunk changes nothing a module sees. |
| C8 | **The memory bound and its floor.** | A task's accounted payload is what the existing accounting computes while it sweeps a chunk: the sum over the chunk's retained generations of local rows times the per-row slab, aux and output-buffer costs, plus the resident capacity of every galaxy pool, active and spare (a released pool keeps its chunks, `src/core/galaxy_pool.c:155-165`, so spare capacity carries an earlier chunk's high-water mark into later chunks), plus the partition tables, with the same exclusions as today (in-sweep growth, the run-wide workspace, the HDF5 write buffer, module scratch, process RSS). Chunking splits the widest slab's share exactly (by C3) and the other slabs' approximately, as distribution does. **A forest is never split, so the largest forest's share of the widest slab is a floor on what any `(NTask, G)` achieves**, exactly the floor step 3 recorded: micro-Uchuu 1.60%, so it chunks nearly linearly; Shin-Uchuu's percolation super-forest 61.86%, so chunking alone cannot bring Shin-Uchuu below that forest's two generations on one process. This plan therefore corrects the pathway's live wording that Shin-Uchuu on small-memory nodes is "served only in combination with chunked slab streaming or a converter-side treatment" (`MIMIC-DEVELOPMENT-PATHWAY.md:14`, `:123`) and the archived step 3 plan's expectation of a chunked follow-on inside the super-forest's rank: the lever for Shin-Uchuu is the converter-side treatment of the linking artefact (the open decision), after which this plan's chunking applies to the many forests it yields. Sub-forest chunking is rejected for this plan: the retained generation's processed rows and galaxies are reached by arbitrary row through progenitor links, so splitting a forest needs either an out-of-core retained store (an OS-paged bound Mimic does not control, against Principle 5) or a descendant-locality row order with a cross-boundary reach (a format and converter change). |
| C9 | **Acceptance predicate.** | **Per-`UniqueGalaxyID` byte equality of every field, tree and created rows, every output snapshot, between the serial run at `forest_chunks: 1` and (a) serial runs at `forest_chunks: 2, 3, 8` and (b) MPI runs at `-np 2` with `forest_chunks: 2` and `-np 3` with `forest_chunks: 3`**, through `scripts/compare_cross_format_identity.py --compare-created` with no tolerance; plus, for the serial legs, equality of each partition's `Galaxies` row order (the `UniqueGalaxyID` column read in file order) with the `G = 1` run's. On the committed `forest_blocks` fixture for `halos-only`, `sage16` and a `hod` variant whose run file omits the snapshot audit (so its full-halo creation path, the only user of the created-record `row_offset`, is gated with positive created-row counts on both sides), with the shipped `sham` and `hod` run files' refusal under C5 checked as a startup failure. The fixture's widest-slab weights `[2, 1, 7, 2, 2, 3]` give the cuts `[0, 3, 6]` at `G = 2` and `[0, 2, 3, 6, 6, ...]` for every `G >= 3` (the greedy packing at the heaviest forest's capacity), so the fixture exercises exactly two distinct non-trivial partitions and, at `G = 8`, idle chunks; and, in Slice 5's real-data stage, on the real `micro-uchuu-horizontal` dataset for `sage16` and `halos-only` at `G = 1, 2, 4, 8` serial and `-np 2 × G = 2`, with peak RSS and wall-clock recorded per run at one commit. Serial `G = 1` output must stay byte-identical to the pre-plan references. |
| C10 | **Logging.** | At `G = 1` every log line, count and wording the integration tests assert stays as it is. At `G > 1`: task 0 logs the partition's chunk cuts beside its task lines at startup (forest range and widest-slab weight per chunk); each chunk's sweep begins with one `INFO` line naming the chunk, its forest range and its widest-slab rows; the "Loaded snapshot" row note names the chunk; the progress bar spans `snapshot_count × G` steps; and the four width messages are reworded separately: the ceiling refusal and the int64 overflow refusal name `input.forest_chunks` as the lever (subject to the largest-forest floor), the `MAX_HALO_ARRAY_SIZE` warning likewise (it judges the chunk's rows), and the `INT_MAX` output refusal says that the record cap is a limit of the output path that neither chunking nor distribution lifts (it judges the snapshot's global count, `horizontal_driver.c:1593-1597`, and every chunk appends to the same per-task partition whose `TotHalosPerSnap` accumulates, `src/io/output/util.c:90-94`); the guard itself is unchanged. |
| C11 | **No format change, no reader change, no converter change.** | The reader's `load_slab(snapnum, row_lo, row_hi)`, `scan_forest_index` and `open_run` are used as they are (step 3 decision D4); the format document's non-normative forest-blocking note gains one clause naming chunked runs as a second consumer; the converter is untouched. |
| C12 | **Out of scope, recorded.** | Sub-forest chunking (C8); automatic `G` (C4); the snapshot scope under chunking (C5); chunking version 2 or non-forest-blocked version 3 data (C2); any change to the vertical driver; the compact previous-slab projection (superseded: it saves about 112 B of about 530 B per retained row and this plan bounds the whole row); parallel HDF5; threaded chunks; a resume (`--skip`) for horizontal runs; output-time filtering; recording `forest_chunks` in `RunProperties`; a CI gate on a real dataset; any claim about full Uchuu, which is storage-bound before it is memory-bound. |

---

## Outcome and Limits

After this plan a horizontal run of a forest-blocked version 3 dataset with `input.forest_chunks: G` in its run file sweeps each task's forests in `G` passes, each pass holding only its chunk's rows of every retained generation, writes exactly the partition files a `G = 1` run writes, and produces, galaxy for galaxy and in the same row order, the bytes the `G = 1` run produces, serially or under `mpirun`. The accounted payload per process is what the retention accounting computes while it sweeps its widest chunk (C8), near `1/(NTask·G)` of the widest slab wherever no single forest dominates, at the cost of `G` file opens per snapshot per task, `G` reopenings of each partition and, for idle chunks, empty sweeps; the measured peak RSS at each `G` is the acceptance record's to report. `sage16` and `halos-only` gain this; `sham`, and `hod` with its audit configured, keep `forest_chunks: 1`.

**What it does not do.** It does not split a forest (C8): Shin-Uchuu's super-forest holds 61.86% of its widest slab, so on today's conversion neither distribution nor chunking puts Shin-Uchuu on a small-memory host, and this plan says so where the pathway and step 3 implied otherwise. It does not chunk the production version 2 Shin-Uchuu dataset (C2). It does not run a snapshot module under chunking (C5). It does not choose `G` for the user (C4). It makes no claim about full Uchuu.

---

## Repository Evidence and Design Decisions

Read at the planning baseline:

- **The sweep is already range-keyed.** `run_horizontal_driver()` (`src/core/horizontal_driver.c:1975-2177`) opens the run once, computes the partition when distributed (`:2005-2007`, `horizontal_partition_run()` `:917-947`), evaluates the identity space once (`:2008`), opens output once (`:2025`, `horizontal_open_output()` `:1092-1098`: arms the master and zeroes `TotHalosPerSnap`), allocates the retention pool (`:2028`) and the FoF workspace (`:2030-2033`), then loops over snapshots (`:2047-2158`). Every per-snapshot step reads the task's rows through `horizontal_task_rows()` (`:667-676`, whole slab when `state->partition == NULL`, else `row_cuts[s][task..task+1]`), sets `identity.unit` and `identity.row_offset` (`:2049-2052`), acquires the generation (`horizontal_acquire_generation()` `:1574-1673`: sizing `:1597`, ranged `load_slab` `:1599`, rebase when partitioned `:1638-1640`, horizon `:1642`), sweeps FoF groups in row order (`:2083-2087`), runs `post_snapshot` (`:2100`), measures and warns (`:2104-2128`), publishes (`:2130`), releases expired generations (`:2136`), writes the task's partition(s) (`:2140-2151`) and releases the generation at its horizon (`:2155-2157`). Teardown asserts nothing is retained (`:2164-2167`) and logs the maxima (`:2168-2171`). Inside the loop only the partition filter at `:2146` consults `current_task_id()`; every row range is the partition's.
- **Driver state.** `struct HorizontalDriverState` (`:232-269`) is a stack local; per-run members are the reader handle, the generation, lookup and spare-pool arrays sized `snapshot_count`, the maxima and the once-per-run warning flag, the grow-only workspace, progenitor scratch and segments, the identity space, and `partition`/`task`. The only file statics are the output-path registry (`:119-126`), the failure-handler state (`:1854`) and the `atexit` guard (`:1870`). The retention pool is keyed by snapshot number (`:1515-1538`), releases at the horizon in ascending order (`:1769-1777`), and returns pools to a spare stack (`:1542-1555`).
- **Partition.** `struct HorizontalForestPartition { int ntask; int64_t snapshot_count; int64_t n_forests_total; int64_t *forest_cuts; int64_t *row_cuts; }` (`src/core/horizontal_partition.h`), with `horizontal_partition_create/destroy`, `horizontal_partition_cut_forests()` (minimum makespan by binary search with greedy packing, `src/core/horizontal_partition.c:88-130`), `horizontal_partition_accumulate_weights()` and the `horizontal_forest_scan_*` streaming scan that verifies forest blocking and records `row_cuts` as lower bounds. `horizontal_compute_partition()` (`horizontal_driver.c:831-908`) weights by the widest slab, cuts, scans every slab, logs each task's range and the table bytes, and frees the weights; the two `MPI_Bcast` calls (`:944-945`) are the driver's only MPI calls. `horizontal_row_cut()`, `horizontal_task_rows_note()` (`:323-339`, the " (this task's rows [lo, hi) of the snapshot's N)" qualifier) and `horizontal_rebase_slab_links(slab, partition, task)` (`:737-802`, exported for `tests/unit/test_horizontal_distribution.c:168`) all index the tables by task.
- **What a retained generation is read for.** Across generations the driver reads the raw slab's `Len` and `NextProgenitor` (`:423`, `:501-503`), the reader-owned `next_progenitor_snapshot` (`:431-433`), the aux map `NHalos`/`FirstHalo` (`:494-557`) and the processed rows with their pool galaxies, deep-copied by `copy_progenitor_galaxy()` (`src/core/inheritance.c:23-34`) from `inherit_descendant_halos()` (`:148`, call at `:166`). Every link resolves inside the owning range (`horizontal_resolve_progenitor()` `:364-395`, bound `prog < generation->view.count`), which is why a chunk, like a task, needs no row of any other chunk.
- **Per-row cost (default build, mini-Millennium catalogue, measured with the compiler).** `sizeof(struct RawHalo)` 104, `struct Halo` 184, `struct GalaxyData` 176, `struct HaloOutput` 264, `struct HorizontalHaloAux` 16. `slab_row_bytes` is 140 B for version 3 (`src/io/horizontal/read_horizontal_hdf5.c:2316-2322`); the output buffer is seeded at `nhalos + max(5%, 1000)` rows of 184 B (`horizontal_driver.c:1324-1339`); a pool row is 176 B. A fully populated retained generation therefore costs about 530 B per row, of which the raw slab is about 140 B: the two narrower candidates of the concept note bound a quarter of the cost, and the Shin-Uchuu fit of 1,340 B/halo over two generations is consistent with this.
- **Output lifecycle today.** `horizontal_write_output()` (`:1125-1177`) arms the single in-flight slot (`:177-183`), creates the partition through `prepare_output_files()` (`src/io/output/util.c:67-80` → `open_hdf5_output_file()` `src/io/output/hdf5.c:137-150` → `prep_hdf5_file()` `:79-120`: `H5Fcreate(H5F_ACC_TRUNC)`, one `Snap%03d` group, an empty `H5TBmake_table` with chunk size 1000, then `write_perfile_metadata()` `src/io/output/metadata_hdf5.c:480-505`), lends the generation's buffer to `save_halos_hdf5()` (`hdf5.c:348-374`, batches of `HDF5_WRITE_BUFFER_RECORDS = 8192` rows flushed through `H5TBappend_records`), flushes (`:337-346`, which frees the static buffer), writes `TotHalosPerSnap` with `H5Acreate` (`write_hdf5_attrs()` `:202-253`), closes `HDF5_current_file_id` with a fatal on a close error (`:1152-1165`), and releases the slot (`:1167`). `TotHalosPerSnap[n]` is zeroed once per run (`:1095-1097`) and incremented per row (`util.c:82-96`), so it already accumulates across several saves. The vertical driver opens a partition once and appends per tree before one close (`src/core/vertical_driver.c:283-372`). The master reads each partition's attribute after the barrier (`src/io/output/master_hdf5.c:169-174`). `bye()` and `myexit()` unlink every armed slot (`src/core/main.c:119`, `:150`).
- **Configuration template.** `input.retention_memory_ceiling_mb`: key table `src/core/read_parameter_file.c:805-818`, strict parse `:919-939` (`get_strict_int64_value`, `:533-559`), seed `:172-174`, `MimicConfig.RetentionMemoryCeiling` at `src/include/types.h:135-140`, vertical rejection `:1575-1584`, the vertical `post_snapshot` rejection `:1585-1593`, unit tests `tests/unit/test_parameter_parsing.c:1689-1846` (registered `:1909-1913`, fatal-message helper `:593-610`), and the horizontal-only rejections' comment `:1556-1558`. `validate_post_snapshot_entries()` (`src/core/module_registry.c:649-700`) refuses `serial_only` under `NTask > 1`.
- **Modules keep no cross-sweep state.** Every non-const static in `models/sage16/modules/**/*.c`, `models/hod/modules/hod_populate/hod_populate.c` and `models/sham/modules/sham_rank_match/sham_rank_match.c` is a parameter or table set in `init()` and cleared in `cleanup()`, or per-FoF-step scratch (`hod_satellites`, `hod_populate.c:73`); `halos-only` has no module. Module `init()`/`cleanup()` run once per process from `main.c`, outside the driver. The two snapshot modules (`process_snapshot`, `snapshot_distribution: collective`) are the only ones whose semantics need the whole population (C5).
- **Gates and fixtures.** `make tests-distributed` (`Makefile:897-906`) runs `tests/manual/test_distributed_identity.py`: a `Gate` class with `build()`, `launch()`, `leg(model)` (`:278-349`: serial reference, `serial_halos_` non-vacuity marker, then `-np 1, 2, 3, 4, 8` with layout checks, the comparator and `galaxies_compared_`/`created_rows_compared_` markers), `version_2_refusal()` (`:405-425`, edits the parsed run file with `config.setdefault("input", {}).update(...)`) and `write_run_file()` (`:468-477`, replaces only the output directory). Run files are `simulations/mini-millennium-horizontal/_tests/input/forest_blocks_<model>.yaml` on the committed `forest_blocks` fixture (six forests, 71 halos, seven snapshots with snapshot 3 empty, per-snapshot counts `[6, 8, 9, 0, 14, 17, 17]`, per-forest totals `[11, 3, 30, 6, 8, 13]`, largest forest 41% of the widest slab, output snapshots `[6, 5, 4, 2]`). The gate's summary line prints the live check count (the "102" in the step 3 records is stale; the hardened gate emits more). CI runs it in `horizontal-v3` (`.github/workflows/ci.yml:140-157`, 45-minute job, `RUN_TIMEOUT = 60` per launch). `make tests-horizontal-v3` (`Makefile:863-885`) runs the four `HV3_UNIT_TESTS` and the two `HV3_PY_TESTS` on `mini-millennium-horizontal`. `tests/integration/test_processing_order.py:329-570` asserts the serial lifecycle lines (`Loaded snapshot N (`, `; K slab(s) live`, exactly `nsnapshots` `Released snapshot` lines, `Retained at most`), one partition per requested snapshot and the master layout; `tests/unit/test_horizontal_retention_budget.c` asserts the sizing strings and the four width messages that say chunked slab streaming does not exist (the `MAX_HALO_ARRAY_SIZE` warning `:665-669`, the `INT_MAX` output refusal `:697-703`, the ceiling refusal `:756-758`, the int64 overflow refusal `:857-858`, against `horizontal_driver.c:1421-1436`, `:1479-1483`, `:1497-1506`) and drives `run_horizontal_driver()` in forked children, compiling `horizontal_driver.c` and `hdf5.c` itself (`tests/unit/run_tests.sh:312-321`). The real `micro-uchuu-horizontal` dataset (440,651 forests, 22,580,924 halos, 50 snapshots) resolves on this host, and the step 3 serial references are in `archive/distributed-references/<model>/` (non-MPI builds at `fe0b9cad`, held identical by the gates since).
- **Documentation surfaces.** Vision Principle 5's horizontal sentence (`docs/VISION.md:92`); user guide "Retained generations and memory" and the distributed bullets (`docs/USER-GUIDE.md:490-520`); developer guide "The Horizontal Driver" and "Distributed operation" (`docs/DEVELOPER-GUIDE.md:1191-1234`) and the full-Uchuu sentence (`:1105`); the format note (`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md:428`, errata `:201`); skills `mimic-architecture-contract` (`:114`, `:130-133`), `mimic-config-and-flags` (`:112`, `references/all-config-keys.md:37`, `:50`), `mimic-run-and-operate` (`:129-131`, `:230-237`), `mimic-validation-and-qa` (`:47`); `tests/README.md:68`; `CHANGELOG.md` Unreleased (`:9-17`, flat bold-lead bullets); the pathway's current-focus box, inventory row (`:209`), named follow-up (`:138-149`) and memory residual (`:245`).

**Measured at the baseline (2026-10-06, this host):** the per-row widths above; the `forest_blocks` fixture census above; micro-Uchuu's widest slab is snapshot 27 with 621,360 halos, its largest forest 9,940 of them (1.60%), and its four-task partition splits that slab into four ranges of 155,340 (`MIMIC-DISTRIBUTED-SNAPSHOT-ACCEPTANCE.md`); Shin-Uchuu's super-forest 321,253,424 of 519,342,987 (61.86%).

---

## The Chunking Contract

Frozen for Slices 1 to 5 and repeated in the receipts that bind each part.

**Configuration.** `input.forest_chunks` (C4): absent means 1; an integer scalar `>= 1`; `0`, negative, fractional, non-numeric, a sequence, or a value outside `int` are fatal at parse time naming the key; rejected in `validate_and_postprocess()` with `ERROR_LOG` for a vertical reader when `> 1`; rejected there with `ERROR_LOG`, for a horizontal reader only (a vertical run with the phase is already rejected, and the chunk rule must not add a second report), when `> 1` and `MimicConfig.num_post_snapshot > 0` (C5). Accepted by the shared parser in `simulation_info.yaml`'s `input:` section as every `input.*` key is; the value is host-specific, so no package sets it and the documentation says so.

**Startup (driver).** Let `distributed = run_is_distributed()` and `chunked = MimicConfig.ForestChunks > 1`. When `distributed || chunked`: after `open_run` the driver refuses `format_version == 2` and, on task 0, computes the two-level partition (C3) with `ntask = effective_task_count()` and `nchunk = MimicConfig.ForestChunks`, scanning every slab and aborting on the first forest-blocking violation, then broadcasts the tables when distributed. The refusal messages keep the substrings the gate and the tests pin (`this is a format_version 2 dataset`; `is not forest-blocked`) and name both counts. When neither, no partition exists and the run is today's serial run.

**Per chunk (every task).** For `c` in `0 .. nchunk-1`, with `range = task·nchunk + c`: log the chunk line (C10); run the snapshot loop over every snapshot with `row_lo = row_cuts[s][range]`, `row_hi = row_cuts[s][range+1]`, `identity.row_offset = row_lo`, the rebase against `range`, sizing and the ceiling from the chunk's counts; after the last snapshot assert `retained_count == 0` (fatal otherwise, naming the chunk); keep the spare pools, workspace and scratch for the next chunk. The coverage check compares `members_processed` with the chunk's `nhalos`.

**Output (every task).** For each requested output snapshot and the task's partition: chunk 0 creates the file and writes its rows; chunks `1 .. nchunk-2` reopen it read-write and append; chunk `nchunk-1` reopens (when `nchunk > 1`), appends, stamps `TotHalosPerSnap` and finalises. A chunk with no rows at that snapshot still takes its turn (so an empty partition is still created and finalised). The in-flight registry is allocation-free: a static table of `ABSOLUTEMAXSNAPS` armed output ids (−1 when free) and the task component, beside the master's path; a slot is armed at creation and released at finalisation, and `horizontal_driver_remove_incomplete_outputs()` rebuilds each armed slot's path through `output_path_hdf5()` and unlinks it. Every partition slot is free when the driver returns on the success path (the last chunk finalised each), on every task. Under `nchunk == 1` the sequence is create, write, stamp, close, release, as today.

**Identity.** Unchanged: tree rows from `ForestIndex`/`HaloRankInForest`; created rows from `(unit = snapnum, rows_per_unit = largest global slab, row = HaloNr + row_offset)`.

**Footguns stated to developers.** `rows_per_unit` is the global largest slab in every chunk; computing it from chunk counts changes created ids. A link that leaves a chunk's range is a dataset defect or a partition bug, never something to repair. `TotHalosPerSnap` is zeroed once per run, not per chunk, and stamped once, by the last chunk; stamping it earlier would `H5Acreate` twice. The reader opens and closes the snapshot file in every `load_slab`, so a chunked run costs `nchunk` opens per snapshot and nothing else in the reader. `scan_forest_index` runs once per slab at startup whatever `nchunk` is. Non-MPI unit builds compile the serial paths, so the `-np N × G` legs are proven by the gate, not by `run_tests.sh`. `test_horizontal_retention_budget` compiles `horizontal_driver.c` and `hdf5.c` itself; a new output helper in another file must be reachable from its link line.

---

## Implementation Profiles and Run Preparation

| Slice | Recommended Developer | Effort | Reason |
|---|---|---|---|
| 1 | Claude Sonnet 5.5 | medium | One strict integer key, two validation rules and their unit tests, on an exact template |
| 2 | Claude Sonnet 5.5 | high | Pure partition logic with a brute-force oracle and two equality properties; mechanical driver call-site updates |
| 3 | Claude Opus 5.5 | high | The HDF5 output lifecycle split into create, reopen-append and finalise, and a multi-slot cleanup registry, with byte-identical output at `G = 1` |
| 4 | Claude Opus 5.5 | high | The chunk loop in the driver: partition on one task, refusals, per-chunk retention, identity, sizing, logging, and the serial identity test |
| 5 | Claude Opus 5.5 | medium | New gate legs with non-vacuity guards, the `hod` variant, the refusal check, and the real-data memory and identity stage (run by the slice's Developer on this host) with its record |
| 6 | Claude Sonnet 5.5 | high | Vision, guides, format note, skills, changelog, pathway closeout; every number traced to the acceptance record |

PM executes each slice separately in plan order; no batching. Pass the table's model and effort explicitly to `start-slice` and record the resolved model versions. Reviewer recommendation: Claude Fable 5.1 (`claude-fable-5-1`), high effort, in fresh sessions for drift audit and code review. An unavailable model is a setup blocker, never permission to substitute silently.

Slices 1, 3, 4, 5 and 6 require recorded human approval because they change a public run-file key, core execution, the output path, test entry points or the vision. Approval may be recorded for all of them before the run with PM's `approve` command; this document grants none. Slice 2 runs unattended.

Before PM initialisation, in order:

1. Commit this plan, the pathway update and the concept note's status note. PM's `init` refuses a tree dirty outside `.pm/` and binds the run to this file's bytes; any later edit stops the run.
2. Run `python3 ~/.claude/skills/project-manager/scripts/pm.py check-plan --plan docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-IMPLEMENTATION-PLAN.md --repo .` and resolve every warning.
3. Confirm the environment can run `make`, `make USE-MPI=yes` (Open MPI `mpicc` and `mpirun` on PATH), `mimic_venv` with `h5py`, HDF5 and the committed fixtures, and that `simulations/micro-uchuu-horizontal/snapshots` resolves.
4. Confirm `make tests-horizontal-v3`, `make tests-snapshot-global-identity` and `MPIRUN="mpirun --oversubscribe" make tests-distributed` pass at the baseline, and record the distributed gate's live check count.
5. Confirm `archive/distributed-references/{sage16,halos-only}/` exist or capture them: at the baseline, non-MPI builds of `sage16` and `halos-only` run on `micro-uchuu-horizontal` through `models/<model>/input/<model>_micro-uchuu-horizontal.yaml` into `archive/distributed-references/<model>/` with the commit hash and `make info` output beside them (the `sage16` reference from step 3 may be reused; `halos-only` is new).

Run on a feature branch the owner names at `init` (PM refuses implicit `main`); no implementation session may edit this plan, change dependency manifests or licences, regenerate baselines, or push; remote CI is read by the owner after the run. Every slice uses one `MODEL`/`SIMULATION` pair per command sequence and restores the default pair's generated code before its final default-tier run. Format check: run the full unmodified `./scripts/beautify.sh`, then `make check-format`; a failure attributable only to files outside the repository tree is recorded as pre-existing and not fixed.

---

## Slice 1: The `input.forest_chunks` key

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: medium. No prerequisites beyond the planning baseline.
- Add the run-file key, its strict parsing, its two configuration-time rejections and their unit tests, on the exact template of `input.retention_memory_ceiling_mb`. Nothing consumes the value yet.

### Acceptance Criteria

- [ ] `struct MimicConfig` (`src/include/types.h`) gains `int ForestChunks` beside `RetentionMemoryCeiling`, with a comment stating its meaning (the number of contiguous forest sub-ranges each task sweeps in turn), its default of 1, and that it is not recorded in output metadata.
- [ ] `parse_input_section()` lists `forest_chunks` in `valid_keys` (so the misspelling `forest_chunk` is still "Unknown key 'input.forest_chunk'") and parses it through `get_strict_int64_value(node, "input.forest_chunks")`; a value `< 1` or `> INT_MAX` is a `FATAL_ERROR` naming the key and the value and saying to omit the key for one chunk; the seed is 1, set where `RetentionMemoryCeiling` is seeded, so a second parse in one process starts from the default.
- [ ] `validate_and_postprocess()` adds, beside the ceiling's vertical rejection, an `ERROR_LOG` (counted into `errors`) when `is_vertical_reader && MimicConfig.ForestChunks > 1`, naming the key, the value and the reader; and an `ERROR_LOG` when `!is_vertical_reader && MimicConfig.ForestChunks > 1 && MimicConfig.num_post_snapshot > 0`, naming `modules.post_snapshot`, the module count, the first module and the chunk count, and stating that a chunked sweep never holds a whole snapshot's population at once so the snapshot scope needs `forest_chunks: 1` (distribute over MPI tasks to reduce memory instead). The vertical-reader gate keeps a vertical run with the phase at its existing single report.
- [ ] `tests/unit/test_parameter_parsing.c` gains, registered beside the retention cases and one `MIMIC_RESULT:` marker each: the default is 1 when the key is absent; the parsed value is observed through the vertical rejection message, which names it (the ceiling test's pattern, `tests/unit/test_parameter_parsing.c:1716-1749`: no harness configuration both carries the key and passes validation, because the horizontal fixture fails on binary output), and a horizontal fixture carrying `forest_chunks: 4` raises nothing about the key; `forest_chunks: 2` with a non-empty `modules.post_snapshot` under a horizontal reader is rejected with the message naming the phase, the module and the count, while `forest_chunks: 1` with the same phase raises nothing about the key or the phase (the harness cannot parse a horizontal configuration to success, `tests/unit/test_parameter_parsing.c:730-736`, `:1720-1724`); the malformed values `0`, `-1`, `two`, `1.5`, `4mb`, `[4]`, a value above `INT_MAX`, and the misspelled key are each fatal with the expected message, through the existing fork-and-re-execute helper.
- [ ] Required evidence: clean default build, `make tests-unit` via a subagent (the new cases pass, nothing else changes), `./scripts/beautify.sh`, `make check-format`; no generated file touched; no documentation beyond the struct comment (Slice 6 owns the guides and skills).

### Authorized Surface

- Files allowed to change:
  - `src/core/read_parameter_file.c`
  - `src/include/types.h`
  - `tests/unit/test_parameter_parsing.c`
- Functions/classes/components allowed to change: `parse_input_section()`, the configuration seed, `validate_and_postprocess()`'s horizontal/vertical rejection block, `struct MimicConfig`, the new test cases and their registration.
- Tests allowed or expected to change: `tests/unit/test_parameter_parsing.c`.

### Explicit Non-Goals

- No consumer of `ForestChunks` in the driver, no partition change, no documentation, no change to the ceiling's own parsing or messages, no `RunProperties` entry.

### Risk Flags

- Risky surfaces touched: public run-file configuration key, configuration validation.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: the cases listed above in `tests/unit/test_parameter_parsing.c`.
- Commands to run: `make clean && make`; `make tests-unit` (subagent, pass/fail summary); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: the two new `ERROR_LOG` messages read as one sentence each with the key, the value and the reason; a vertical fixture with the phase and `forest_chunks: 2` reports the vertical rejections only, once each.

### Rollback Path

- Revert the slice commit; the key disappears from the valid list and the parser rejects it as unknown again.

---

## Slice 2: Chunk sub-ranges in the partition

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. No prerequisite slice (independent of Slice 1).
- Extend the MPI-free, I/O-free partition module with the two-level cut (C3) and move every table-stride site in the driver from a task index to a range index, with `nchunk = 1` everywhere so behaviour is unchanged. Test the sub-partition against a brute-force oracle and the two equality properties.

### Acceptance Criteria

- [ ] `struct HorizontalForestPartition` gains `int nchunk` after `ntask`; `forest_cuts` has `ntask·nchunk + 1` entries and `row_cuts` has `snapshot_count × (ntask·nchunk + 1)` entries, row-major by snapshot; the header's Doxygen contract states the layout, that entry `t·nchunk` is task `t`'s first cut, and that range `t·nchunk + c` is task `t`'s chunk `c`. `horizontal_partition_create(ntask, nchunk, snapshot_count, n_forests_total)` takes the chunk count (abort on `nchunk < 1`, and on a table size that overflows `int64_t` or `int`, the latter because the row cuts travel in one `MPI_Bcast`).
- [ ] A new `horizontal_partition_cut_chunks(const int64_t *weights, const struct HorizontalForestPartition *partition)` (or an equivalent name in the module's style) fills the chunk cuts: for every task `t`, `horizontal_partition_cut_forests()` is applied to the weights of `[forest_cuts[t·nchunk], forest_cuts[(t+1)·nchunk])` with `nchunk` ranges and the results are offset into place; task cuts are left exactly as `horizontal_partition_cut_forests()` wrote them over all forests with `ntask` ranges. `nchunk == 1` is a no-op.
- [ ] The streaming scan fills `row_cuts[snapnum][0 .. ntask·nchunk]` as the lower bound of every cut (its `next_cut` walks all `ntask·nchunk + 1` cuts); `horizontal_forest_scan_end()`'s contract is unchanged otherwise.
- [ ] The header gains one accessor for the range count (`ntask · nchunk`) and every table-stride site uses it instead of `ntask + 1` or a bare `->ntask`: in `src/core/horizontal_partition.c` the scan's stride, cut bound and end fill (`:172-174`, `:193`, `:204-209`); in `src/core/horizontal_driver.c` `horizontal_global_row()` (`:313-319`), `horizontal_task_rows_note()` (`:323-339`), `horizontal_row_cut()`, `horizontal_partition_resident_bytes()` (`:656-663`), `horizontal_rebase_link()`/`horizontal_rebase_slab_links()` (`:687-802`), `horizontal_compute_partition()`'s row-count read and per-task log loop (`:871`, `:884-896`), the `MPI_Bcast` counts (`:940-945`) and the create call (`horizontal_partition_create(ntask, 1, ...)`), with `horizontal_compute_partition()` calling the chunk cut after the task cut (a no-op at 1). Each indexes by a range, and the driver passes `state.task` as the range everywhere (equal to it at `nchunk == 1`). No behaviour, message or log line changes.
- [ ] `tests/unit/test_horizontal_partition.c` gains cases, one `MIMIC_RESULT:` marker each: with `nchunk == 1` every table equals the baseline computation entry for entry over the existing small-vector enumeration; for every weight vector of length `<= 8` with weights in `{0, 1, 2, 5}`, `ntask ∈ {1, 2, 3}` and `nchunk ∈ {1, 2, 3}`, each task's chunk makespan equals a brute-force optimum over all contiguous `nchunk`-partitions of that task's sub-array, chunk cuts nest inside task cuts, and cuts never decrease; with `ntask == 1`, the `nchunk == G` chunk cuts equal the `ntask == G, nchunk == 1` task cuts for every vector; idle trailing chunks give `row_cuts[s][r] == row_cuts[s][r+1]`; the scan's row cuts equal `lower_bound` at every one of the `ntask·nchunk + 1` cuts on hand-built columns delivered in blocks of varying size; `create` rejects `nchunk < 1`.
- [ ] `tests/unit/test_horizontal_distribution.c` passes `nchunk = 1` and a range index where it built partitions and called the rebase; one added case rebases a hand-built slab against a chunk range `t·nchunk + c` with `nchunk = 2` and checks the local indices and the "forest cut" abort for a link in the task's other chunk.
- [ ] Required evidence: clean default build, `make tests-unit` via a subagent, `make tests-horizontal-v3`, `MPIRUN="mpirun --oversubscribe" make tests-distributed` (the live check count unchanged from run preparation), `./scripts/beautify.sh`, `make check-format`; no generated file touched.

### Authorized Surface

- Files allowed to change:
  - `src/core/horizontal_partition.h`
  - `src/core/horizontal_partition.c`
  - `src/core/horizontal_driver.c`
  - `tests/unit/test_horizontal_partition.c`
  - `tests/unit/test_horizontal_distribution.c`
- Functions/classes/components allowed to change: the partition module; in the driver only the partition's consumers named above and the create/broadcast sites, with no new behaviour.
- Tests allowed or expected to change: the two unit tests.

### Explicit Non-Goals

- No chunk loop, no consumer of `MimicConfig.ForestChunks`, no change to messages or logs, no reader change, no documentation beyond the header contract.

### Risk Flags

- Risky surfaces touched: core driver call sites (mechanical, behaviour-preserving).
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Tests to add/update: `tests/unit/test_horizontal_partition.c`, `tests/unit/test_horizontal_distribution.c`.
- Commands to run: `make clean && make`; `make tests-unit` (subagent); `make tests-horizontal-v3`; `MPIRUN="mpirun --oversubscribe" make tests-distributed`; `make clean && make` to restore the serial build; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the brute-force oracle to confirm it enumerates every contiguous `nchunk`-partition of each task's sub-array; confirm the `nchunk == 1` equality case compares whole tables, not makespans; grep both files to confirm no `ntask + 1` stride or bare `->ntask` table index remains (a wrong stride is invisible at `nchunk == 1` and surfaces only in Slice 4).

### Rollback Path

- Revert the slice commit; the driver returns to the task-indexed tables and nothing else depends on `nchunk`.

---

## Slice 3: Appendable output partitions

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. No prerequisite slice.
- Split the horizontal partition write into create, reopen-and-append and finalise so a partition can receive rows in several visits, and give the cleanup registry one in-flight slot per requested output snapshot (C6). At one visit per partition, which is every run until Slice 4, the files are byte-identical to today's.

### Acceptance Criteria

- [ ] `src/io/output/hdf5.c` gains `reopen_hdf5_output_file(int filenr, int task)` (declared in `src/io/output/hdf5.h`): it builds the partition path as `open_hdf5_output_file()` does, opens it with `H5Fopen(H5F_ACC_RDWR)` into `HDF5_current_file_id`, fatal on failure naming the path, and writes no metadata and no table; `open_hdf5_output_file()` and `prep_hdf5_file()` are unchanged.
- [ ] `horizontal_write_output()` takes two flags (or an equivalent small enum), `first_visit` and `last_visit`: on `first_visit` it arms the partition's slot and creates the file through `prepare_output_files()`, otherwise it reopens through the new function; it then lends the buffer, saves, clears the globals and flushes as today; on `last_visit` it stamps `write_hdf5_attrs()`; it always closes the file with the existing fatal-on-close-error check; on `last_visit` it releases the slot and logs the existing "Wrote snapshot" line with the partition's accumulated row count, otherwise it logs a `VERBOSE_LOG` naming the rows appended and the partition. The driver passes `first_visit = last_visit = 1` (one visit per partition).
- [ ] The cleanup registry replaces the single in-flight path slot with an allocation-free table: `ABSOLUTEMAXSNAPS` static entries holding an armed partition's output id (−1 when free) and its task component, beside the master's path buffer as today. `horizontal_arm_partition_output_path(output_index, output_id, task)` arms entry `output_index`, `horizontal_clear_partition_output_path(output_index)` frees it, `horizontal_driver_remove_incomplete_outputs()` rebuilds each armed entry's path through `output_path_hdf5()` (which reads only `MimicConfig`) and unlinks it, and `horizontal_driver_clear_output_paths()` frees every entry and the master. Nothing is allocated, so no task leaks anything into the leak check. The registry's comment (`:103-118`) and the write function's comment (`:1100-1124`) are rewritten for the two lifetimes (a partition from creation to finalisation; the master as before) and for the multi-visit sequence, stating that a failure removes every partition not yet final.
- [ ] `TotHalosPerSnap[n]` accumulates across visits (it is zeroed once per run today and that stays) and is stamped once, on the last visit; the master's republished value is unchanged.
- [ ] `tests/unit/test_hdf5_write_attrs.c` (which links `src/io/output/hdf5.c`, drives the writer directly, builds its own `Galaxies` fixture and counts calls to its `write_perfile_metadata()` stub, `:36-46`, `:134-139`; the retention-budget harness instead stubs that writer with an `abort()`, `tests/unit/test_horizontal_retention_budget.c:85-89`, so it cannot host this case) gains one case exercising the writer at its public seam, with no driver hook: after `calc_hdf5_props()`, on a hand-built output buffer it runs `open_hdf5_output_file()` → `save_halos_hdf5()`/`flush_hdf5_buffers()` → `H5Fclose` → `reopen_hdf5_output_file()` → save/flush of the remaining rows → `write_hdf5_attrs()` → close, and asserts the `Galaxies` table holds every row in the original order, `TotHalosPerSnap` equals the row count, `perfile_metadata_calls == 1` (written on the first visit, not on the reopen), and the table's bytes equal a one-visit file's. The existing failure tests in `tests/integration/test_processing_order.py:590-708` keep passing unchanged; the multi-visit failure window is tested in Slice 4, where a second visit first exists.
- [ ] Serial identity: `make tests-snapshot-global-identity`, `make tests-horizontal-v3` and `make tests-distributed` pass; `tests/integration/test_processing_order.py` passes unchanged (every lifecycle line and count as today).
- [ ] Required evidence: clean default build, `make check-generated`, the three default tiers via a subagent, the gates above, `./scripts/beautify.sh`, `make check-format`; no test weakened.

### Authorized Surface

- Files allowed to change:
  - `src/core/horizontal_driver.c`
  - `src/io/output/hdf5.c`
  - `src/io/output/hdf5.h`
  - `src/include/proto.h` (only if a registry prototype changes)
  - `tests/unit/test_hdf5_write_attrs.c`
- Functions/classes/components allowed to change: the registry, `horizontal_open_output()`, `horizontal_write_output()` and its call site, the new reopen function, the new test.
- Tests allowed or expected to change: `tests/unit/test_hdf5_write_attrs.c`.

### Explicit Non-Goals

- No chunk loop, no second visit on the production path, no change to file names, master links, the vertical writer, `prep_hdf5_file()`, the per-file metadata, or `output_increment_halo_counters_checked()`; no change to the `--skip` rejection.

### Risk Flags

- Risky surfaces touched: output path, core driver, failure cleanup.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the one case in `tests/unit/test_hdf5_write_attrs.c`.
- Commands to run: `make clean && make`; `make check-generated`; default tiers (subagent); `make tests-horizontal-v3`; `make tests-snapshot-global-identity`; `MPIRUN="mpirun --oversubscribe" make tests-distributed`; `make clean && make` to restore; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required. Run differential code-health against the slice's starting commit and supply it as review evidence (structural change).
- Manual checks: `h5dump -H` of a one-visit partition before and after the slice shows the same objects and attributes; the registry's failure semantics comment matches the code; the retention-budget harness, which zero-fills `MimicConfig` without the parser (`:400`), still runs with `ForestChunks == 0` because nothing in this slice reads it.

### Rollback Path

- Revert the slice commit; the single-slot registry and the one-call write return, and nothing calls the reopen function.

---

## Slice 4: The chunked sweep

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slices 1, 2 and 3.
- Wire the chunking contract into the driver: the partition on a single task when `forest_chunks > 1`, the widened refusals, the per-chunk snapshot loop with per-chunk retention, identity, sizing and logging, the multi-visit output, and a committed serial identity test on the `forest_blocks` fixture.

### Acceptance Criteria

- [ ] `run_horizontal_driver()` computes `chunked = MimicConfig.ForestChunks > 1` and runs the partition path when `distributed || chunked`, with `nchunk = MimicConfig.ForestChunks`; the `format_version 2` refusal and the forest-blocking refusal fire for a chunked serial run, their messages keeping the pinned substrings and naming both the task count and the chunk count; the broadcast runs only when distributed. A `G = 1` serial run takes none of these steps, loads whole slabs with `row_offset = 0`, logs no partition line and is today's run.
- [ ] Task 0's partition log adds, when `nchunk > 1`, one line per task and chunk (forest range and widest-slab weight, like the task lines) and the tables line reflects the larger tables; each chunk's sweep starts with one `INFO_LOG` naming the task's chunk `c` of `G`, its forest range and its widest-slab row range.
- [ ] The snapshot loop runs once per chunk with `range = task·nchunk + c`: `horizontal_task_rows()` and `identity.row_offset` use the range; the rebase, the coverage check, sizing (`horizontal_require_generation_fits()`), the ceiling and the in-sweep warning use the chunk's counts; after each chunk's last snapshot a `FATAL_ERROR` names the chunk if `retained_count != 0`; `max_retained_count`, `max_retained_bytes` and `run_profile_note_retention()` accumulate maxima across chunks; the progress bar spans `snapshot_count × nchunk`; spare pools, the workspace, scratch and segments carry over; the row note reads " (this task's chunk c of G, rows [lo, hi) of the snapshot's N)" when `nchunk > 1` and is unchanged otherwise.
- [ ] Output: for each requested snapshot the task's partition is visited once per chunk with `first_visit = (c == 0)` and `last_visit = (c == nchunk − 1)`, including chunks holding no rows at that snapshot, so names, master links and `TotHalosPerSnap` are today's at every `G`.
- [ ] The four width messages and the comment at `:1412` are reworded as C10 states: the ceiling refusal (`:1497-1506`) and the int64 overflow refusal (`:1479-1483`) name `input.forest_chunks` as the lever, subject to the largest-forest floor; the `MAX_HALO_ARRAY_SIZE` warning (`:1430-1436`) likewise; the `INT_MAX` output refusal (`:1421-1427`) says the record cap is a limit of the output path that neither chunking nor distribution lifts, with the guard and its global-count input unchanged. The four assertions in `tests/unit/test_horizontal_retention_budget.c` (`:665-669`, `:697-703`, `:756-758`, `:857-858`) are updated to the new wording, nothing else in them; the harness's zero-filled `MimicConfig` (`:400`) is given `ForestChunks = 1` where it is initialised. The comments at `:258-268` ("NULL for a serial run"), `:624-638` ("only an MPI build with more than one task") and `:1578-1581` ("a generation is loaded exactly once") are rewritten for chunked runs.
- [ ] A new `simulations/mini-millennium-horizontal/_tests/integration/test_chunked_sweep.py`, added to `HV3_PY_TESTS` in the Makefile, runs `halos-only` on the `forest_blocks` fixture (from `_tests/input/forest_blocks_halos-only.yaml` with the output directory redirected, as the gate does) at `forest_chunks: 1`, then at `2`, `3` and `8`, plus `2` with `--compress` (the compressed table appended across file sessions), and asserts with `MIMIC_RESULT:` markers: each run exits 0 with the same four partition files and master; the comparator reports identity with the `G = 1` run and a positive galaxy count; each partition's `UniqueGalaxyID` column read in file order equals the `G = 1` run's; the `G = 8` log shows idle chunks; the `G = 2` log carries the chunk lines and `2 × nsnapshots` "Loaded snapshot" lines; and the multi-visit failure window: `forest_chunks: 2` on a copy of `_tests/data/forest_blocks` made by the test's own helper (modelled on `fixture_copy_with_broken_fof_link` in `tests/integration/test_processing_order.py:194-220`, which copies the package default fixture and always breaks row 0, so it cannot be reused), setting `FirstHaloInFOFgroup` of row 12 of snapshot 5 (forest 4, inside chunk 1's rows `[10, 17)`; the reader validates only the rows it loads, so chunk 0's load of rows `[0, 10)` passes) to that slab's halo count, exits with a failure, keeps the partitions chunk 1 had already finalised (snapshots 2 and 4) and removes the two not yet final (snapshots 5 and 6). The `sham`/`hod` refusal is not tested here (`tests-horizontal-v3` builds `halos-only`, so a `sham` run file would also fail the model-name check); it is unit-tested in Slice 1 and gate-tested on real builds in Slice 5.
- [ ] MPI gate for this slice (recorded in the slice summary; Slice 5 automates the rest): `make MODEL=sage16 SIMULATION=mini-millennium-horizontal USE-MPI=yes` with the `forest_blocks_sage16.yaml` run file at `forest_chunks: 2`: `mpirun -np 2` writes today's `_task000`/`_task001` partitions and the master and compares equal to the serial `G = 1` run through the comparator.
- [ ] Required evidence: clean default build, `make check-generated`, the three default tiers via a subagent, `make tests-horizontal-v3` (including the new test), `make tests-snapshot-global`, `make tests-snapshot-global-identity`, `MPIRUN="mpirun --oversubscribe" make tests-distributed` (count unchanged), the MPI gate above, `./scripts/beautify.sh`, `make check-format`; no baseline refresh, no test weakened.

### Authorized Surface

- Files allowed to change:
  - `src/core/horizontal_driver.c`
  - `tests/unit/test_horizontal_retention_budget.c` (the four wording assertions and the harness's `ForestChunks` default only)
  - `simulations/mini-millennium-horizontal/_tests/integration/test_chunked_sweep.py` (new)
  - `Makefile` (the `HV3_PY_TESTS` line only)
  - `tests/integration/test_processing_order.py` (only if a `G = 1` assertion must name a changed helper; no weakening)
- Functions/classes/components allowed to change: the driver's startup, partition logging, chunk loop, row note, output visits, width messages and the three comments; the new test; the one Makefile line.
- Tests allowed or expected to change: the three files named above.

### Explicit Non-Goals

- No change to the partition module, the reader, the writer beyond calling Slice 3's functions, the configuration parser, the collectives, any module, or the manual gate (Slice 5); no automatic `G`; no documentation.

### Risk Flags

- Risky surfaces touched: core execution path, output path, MPI, identity.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: `test_chunked_sweep.py` (new), the four wording assertions and the harness default in `test_horizontal_retention_budget.c`.
- Commands to run: `make clean && make`; `make check-generated`; default tiers (subagent); `make tests-horizontal-v3`; `make tests-snapshot-global`; `make tests-snapshot-global-identity`; `MPIRUN="mpirun --oversubscribe" make tests-distributed`; the MPI gate (`USE-MPI=yes` build of `sage16` on `mini-millennium-horizontal`, `-np 2 × G = 2`, comparator); `make clean && make` to restore; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required. Run differential code-health against the slice's starting commit and supply it as review evidence (structural change).
- Manual checks: on the `G = 2` fixture log, the chunk lines' forest ranges and widest-slab rows match a hand count of the fixture's `ForestIndex` column; every "Released snapshot" line of chunk 0 precedes chunk 1's first "Loaded snapshot" line.

### Rollback Path

- Revert the slice commit; every new step is behind `chunked`, so the serial and distributed paths return to Slice 3's state by construction.

---

## Slice 5: Gate legs and the real-data acceptance stage

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: medium. Requires Slice 4.
- Extend the distributed identity gate with chunked legs and the refusal check, keeping its non-vacuity guards, and record the real-data memory and identity measurements at one commit.

### Acceptance Criteria

- [ ] A new run file `simulations/mini-millennium-horizontal/_tests/input/forest_blocks_hod_chunked.yaml`, derived from `forest_blocks_hod.yaml` with the `post_snapshot` phase omitted (the module's `init()` accepts that, `models/hod/modules/hod_populate/hod_populate.c:276-283`) and a header comment saying why it exists, so the full-halo creation path runs under chunking.
- [ ] `tests/manual/test_distributed_identity.py` gains, inside `leg(model)` for `halos-only`, `sage16` and `hod` (the `hod` chunked legs from the variant above, against a serial reference of the same variant): serial runs at `forest_chunks ∈ (2, 3, 8)` (a dict edit like the version 2 refusal's, in a run directory named `chunks<G>`), each compared through the comparator with the existing `identity_` and `galaxies_compared_` markers, the `created_rows_compared_` marker for `hod`, and a new `row_order_` marker reading each partition's `UniqueGalaxyID` column in file order against the reference; and MPI runs at `-np 2` with `forest_chunks: 2` and `-np 3` with `forest_chunks: 3` (directories `np2_chunks2`, `np3_chunks3`), with the existing task-layout check and comparator markers. For `sham` and the shipped `hod` run file it adds one serial launch at `forest_chunks: 2` that must fail at configuration with the Slice 1 message and without "Opened horizontal run" (a `chunked_refused_` marker).
- [ ] The gate's summary line and the module docstring state the new legs; `RUN_TIMEOUT` and the CI job budget are unchanged, and the added launches are counted in the slice summary (each is a sub-second fixture run).
- [ ] Real-data stage, run by the slice's Developer on this host at the slice's commit and recorded in a new `docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-ACCEPTANCE.md` with the structure of `MIMIC-DISTRIBUTED-SNAPSHOT-ACCEPTANCE.md` (fixture gate, real-data stage: identity, peak memory and wall-clock, partition and chunk log, floor): for `sage16` and `halos-only` on `micro-uchuu-horizontal` through the shipped run files with the output directory redirected, serial non-MPI builds at `forest_chunks` 1, 2, 4 and 8, each under `/usr/bin/time -l` (peak RSS and wall-clock), each compared with `archive/distributed-references/<model>/` and with the `G = 1` run through the comparator; and the MPI build at `-np 2` with `forest_chunks: 2` with per-rank `/usr/bin/time -l`, compared the same way. The record states the host, the commit, the dataset census, the chunk partition task 0 logged at `G = 4`, the measured peak RSS at each `G` as a table with the ratio to `G = 1`, and the largest-forest floor (1.60%), and claims nothing beyond those runs.
- [ ] Required evidence: `MPIRUN="mpirun --oversubscribe" make tests-distributed` passes in CI form with the new legs and no skips, its live check count recorded; the real-data record committed; `./scripts/beautify.sh`, `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `tests/manual/test_distributed_identity.py`
  - `simulations/mini-millennium-horizontal/_tests/input/forest_blocks_hod_chunked.yaml` (new)
  - `docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-ACCEPTANCE.md` (new)
- Functions/classes/components allowed to change: the gate script's legs, markers, docstring and summary; the new run file; the new record.
- Tests allowed or expected to change: the gate script.

### Explicit Non-Goals

- No runtime change, no CI workflow change, no new fixture, no change to the comparator, no documentation outside the record, no real-data run of `sham` or `hod` under chunking (`sham` and the shipped `hod` run file are refused by C5; the `hod` variant is fixture-only).

### Risk Flags

- Risky surfaces touched: test entry point run by CI.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the gate's new legs.
- Commands to run: `MPIRUN="mpirun --oversubscribe" make tests-distributed`; the real-data commands of the record (`make MODEL=<model> SIMULATION=micro-uchuu-horizontal TEST_BUILD=no` and `USE-MPI=yes`, `/usr/bin/time -l ./mimic run.yaml`, the `mpirun -np 2 sh -c 'exec /usr/bin/time -l -o rank_${OMPI_COMM_WORLD_RANK}.txt ./mimic run.yaml'` form, `mimic_venv/bin/python scripts/compare_cross_format_identity.py <reference> <output> --compare-created`); `make clean && make` to restore; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: every new marker can fail (no leg passes on empty output: the `galaxies_compared_` guard is on every chunked leg and `created_rows_compared_` on the `hod` ones); the memory table's `G = 1` row was measured at the same commit as the others.

### Rollback Path

- Revert the slice commit; the gate returns to its Slice 4 form and the record disappears.

---

## Slice 6: Documentation, skills and pathway closeout

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. Requires Slice 5.
- Record the delivered contract where it permanently lives, narrow the vision wording, add the format note's clause, refresh the skills and changelog, correct the pathway's and the guides' expectations about Shin-Uchuu, and close the focus.

### Acceptance Criteria

- [ ] `docs/VISION.md`: Principle 5's horizontal sentence gains one clause stating that a task may sweep its forests in several chunks, each chunk bounding memory to its own rows of the retained generations, so the bound depends on the widest chunk rather than the widest slab, still never on simulation depth or total halo count; no other vision change.
- [ ] `docs/DEVELOPER-GUIDE.md`: "The Horizontal Driver" gains a "Chunked sweeps" subsection stating C1, C3, C5, C6, C7, C8 and C10 (the sub-partition, the per-chunk loop, the per-visit output and registry, per-chunk accounting, the snapshot-scope refusal, the floor) and the two-level table layout; "Distributed operation" cross-references it; the full-Uchuu sentence at `:1105` and the ceiling sentence at `:1199` say what now exists; the "Record Creation Contract" notes `row_offset` is the chunk's first row.
- [ ] `docs/USER-GUIDE.md`: the full-Uchuu sentence at `:486` is reworded to C12's position (chunking exists; full Uchuu stays unclaimed because it is storage-bound); "Retained generations and memory" documents `input.forest_chunks` (what it does, the YAML, the rejections, which datasets qualify, that `sham`, and `hod` with its audit configured, need `forest_chunks: 1`, the output being unchanged, the failure window under `G > 1`, how to read the `--verbose` sizing lines to choose `G`, the super-forest floor, and the measured micro-Uchuu table from the acceptance record); the distributed bullets say chunks and tasks compose.
- [ ] `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`: the non-normative note at `:428` names chunked runs as a second consumer of forest blocking; one errata row dated for the documentation addition, no version change.
- [ ] Skills: `.agents/skills/mimic-architecture-contract/SKILL.md` (sections 3 and 4: the chunk as the range unit, per-chunk ownership, the registry's lifetimes, the refusal), `mimic-config-and-flags/SKILL.md` and its `references/all-config-keys.md` (the key, its rules, the valid-keys list), `mimic-run-and-operate/SKILL.md` (choosing `G`, the floor, the refusal), `mimic-validation-and-qa/SKILL.md` (the gate's chunk legs and `test_chunked_sweep.py`), `mimic-modules/SKILL.md` (snapshot modules need `forest_chunks: 1`), `mimic-simulations-and-readers/SKILL.md` (the full-Uchuu sentence at `:58`), `mimic-docs-and-writing/SKILL.md` (the external-claims list at `:74` moves chunked streaming and distributed operation to supported-with-limits, naming the limits); each a few sentences, no restating of the guides.
- [ ] `CHANGELOG.md` Unreleased: the feature, the key, the two configuration rejections, the widened startup refusals, the per-visit output and its failure window, the new gate legs and integration test, and the recorded floor; the v1.2 "Not claimed" list is left as history.
- [ ] `tests/README.md`: the distributed gate paragraph and the `tests-horizontal-v3` description name the chunk legs and the new test.
- [ ] `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: the current-focus box records chunked slab streaming complete with the measured outcomes from the acceptance record (peak RSS per `G`, identity, the floor) and the recorded limits (version 2 and ASCII version 3 not chunkable; snapshot modules excluded; the super-forest floor is shared with distribution, so Shin-Uchuu needs the converter-side treatment and the earlier "in combination with chunked slab streaming" wording is corrected); the "Why distributed operation is step 3" paragraph and the named follow-up are corrected the same way; the inventory lists this plan and the concept note for archiving on merge and the acceptance record as standing evidence; the memory residual at `:245` is updated. Do not move or archive this plan or the concept note.
- [ ] `docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`: status set to executed with a pointer to this plan and the acceptance record.
- [ ] `make check-docs` passes; `./scripts/beautify.sh` and `make check-format` pass.

### Authorized Surface

- Files allowed to change:
  - `docs/VISION.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/USER-GUIDE.md`
  - `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-modules/SKILL.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
  - `CHANGELOG.md`
  - `tests/README.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`
  - `simulations/mini-millennium-horizontal/README.md` (only to name `test_chunked_sweep.py` beside the fixture's other uses)
  - `README.md` (only if it lists horizontal limits)
- Functions/classes/components allowed to change: prose only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code, no new document beyond what Slice 5 wrote, no archiving, no change to the format's normative text, no restating of generated lists, no claim about full Uchuu or Shin-Uchuu beyond the measured floor.

### Risk Flags

- Risky surfaces touched: vision wording, format document.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: every number in the pathway's outcome line and the user guide's table traces to the acceptance record; the vision diff is one clause; no document outside `docs/dev/` references a `docs/dev/` document.

### Rollback Path

- Revert the slice commit; documentation only.

---

## Next Chat Prompts

### Mode A — Checkpointed alternative

```text
Plan file: docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-IMPLEMENTATION-PLAN.md
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
Plan file: docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: harness claude model sonnet effort medium initially
Reviewer: harness claude model claude-fable-5-1 effort high

Use project-manager. You are the accountable PM and never write slice code.
Read the complete frozen plan; run check-plan with repo context. Require a clean committed
planning baseline and resolve any drift against the plan's anchors first. Create the feature
branch I name at init; never run on main.
Record human approval for each flagged slice (1, 3, 4, 5, 6) before starting it; neither this
launcher nor the plan grants those approvals.
Ask for commit authorization before launch unless I have already explicitly granted it; no
session pushes. Keep PM_RUN_TOKEN private to the PM seat. Preflight make, make USE-MPI=yes
(Open MPI), mimic_venv with h5py, HDF5, the committed fixtures, the micro-uchuu-horizontal
dataset and the references under archive/distributed-references/ without changing source data.

For each slice in order:
1. Launch a fresh Developer, explicitly passing that receipt's model and effort to
   start-slice (Sonnet medium for 1; Sonnet high for 2 and 6; Opus high for 3 and 4;
   Opus medium for 5).
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
