# Implementation Plan — Chunked Output for Consistent-Trees (Solution A)

**Status:** plan-first artifact, not yet implemented.
**Author context:** follows the output-workflow analysis of 2026-06-23. Implements "Solution A" with **LPT (Largest-Processing-Time greedy) as the default chunk→task assignment**.
**External review:** critiqued by Codex GPT-5.5 (read-only, `xhigh`) on 2026-06-23 against the live codebase; verified findings folded in (see "Review corrections" below).
**Branch:** create a dedicated feature branch before Slice 1; do not work on `main`.

**v1.0 cleanup intent:** this plan may preserve old behaviour during intermediate slices for validation and rollback, but the v1.0 end state must not retain backwards-compatibility code for the ctrees per-task output model being replaced. Once both ctrees readers use chunked enumerated output, stale `PARTITION_PER_TASK` code, docs, tests, and consumer assumptions must be removed rather than supported indefinitely.

---

## Problem recap

The output **partition** is the unit of output: the core opens one set of output files per partition (`src/io/tree/reader.h:17-21`). Today there are two partition models:

- **`PARTITION_PER_FILE`** (L-Halo binary + HDF5): one partition per input file. Output file count tracks the input. Fine. **Note:** this path is literally *input-file-backed* — the driver stats a tree file per output id and skips missing ones (`src/core/main.c:504-515`, `:611`, `:630`), and `build_partition_file_offsets` counts trees per input file (`src/core/main.c:517-555`). It cannot be reused for synthetic chunk ids without generalization (see Slice 3).
- **`PARTITION_PER_TASK`** (both Consistent-Trees readers): one partition per MPI task (`src/core/main.c:584-595`). Output file count = `NTask`, **independent of data volume**. Serial → 1 file; 8 cores → 8 files. For full Uchuu (2000 input files, 37 TB compressed) this yields a handful of unmanageable multi-hundred-GB files, and the output layout changes with core count (non-reproducible).

## Target end state

Make the Consistent-Trees readers **enumerate global output chunks** and drive them through a **generalized enumerated-partition path**:

1. **Chunking (output size, `NTask`-independent):** cut the global forest list into many chunks, each targeted at an output-size budget. A forest is atomic (never split). Chunk ids are **contiguous and global**, depending only on data + budget → **reproducible output files across any core count**.
2. **Assignment (load balance, `NTask`-dependent):** assign chunk ids to tasks by **LPT greedy** on per-forest compute cost (reusing the existing `compute_forest_cost_from_nhalos` weighting), with a **deterministic tie-break by ascending chunk id**. Each rank processes its assigned chunks in **ascending chunk-id order** (deterministic naming/logging). With `nchunks ≫ NTask`, balance is finer than today's `NTask`-piece cut. The monster-forest floor (a single forest costing more than a fair share) is identical to today.
3. **Identity invariant:** galaxy IDs derive from the **global forest index** via `GlobalForestOffset + unit` (`src/core/build_model.c:268-282`), encoded from `halonr` and `forestnr_global` (`src/include/galaxy_id.h:29`). Each chunk publishes its own `GlobalForestOffset`, so `UniqueGalaxyID` (and the derived `UniqueCentralGalaxyID`) and the source-derived `MostBoundID` are **invariant under re-chunking for every galaxy** — the global forest index does not depend on how forests are grouped into files. **Two caveats:** (a) `FileNum = output_id` (`src/core/main.c:372`) becomes the *chunk id*, not the task or input-file number — anything keyed on `FileNum` changes meaning; (b) the current encoding maps `(halonr=0, forestnr_global=0) → UniqueGalaxyID == 0`, and SHAM treats `0` as "absent," falling back to a `FileNum`/`TreeID` key (`models/sham/modules/sham_assign_stellar_mass/sham_assign_stellar_mass.c:68-72`). Slice 5 resolves this before v1.0 by reserving `0` as a true sentinel in the encoder.

This **collapses `PARTITION_PER_TASK` into a generalized enumerated model** (a chunk is a generalized partition). The per-task driver branch and the per-task master-file special case retire once both ctrees readers move over.

## Vision alignment (modular and scalable)

This work is explicitly designed to advance the architectural vision, not just fix a symptom:

- **Modular (principles 1 & 6 — physics-agnostic core, format-agnostic I/O):** the `PARTITION_ENUMERATED` contract (Slice 3) is a clean reader seam — a reader declares *how it enumerates output chunks and their cost*, and the core driver/writers/master are format-agnostic. Adding a future reader means implementing the hooks, with no core edits. The chunk planner (`chunk_plan`) is reader-agnostic (operates on sizes/costs), so the same logic serves HDF5 and ASCII and any later format.
- **Scalable (principle 5 — bounded memory, explicit ownership):** output file count is governed by data volume (a size budget), not core count; per-chunk processing memory is bounded by the chunk, not the catalogue; and the planner is **streaming by design** so its working set never grows with total forest count. The current full-array forest-distribution path (`read_ctrees_hdf5.c:1325`) is a latent violation of "long runs do not accumulate memory with the number of forests" — this plan brings the ctrees readers into compliance (Slices 1 + 7) while making full Uchuu/Shin-Uchuu first-class rather than special-cased.
- **Reproducible (principle 6):** chunk boundaries depend only on data + budget, so the same science run yields the same output files on any core count — output layout is decoupled from the parallel decomposition.

## Review corrections folded in (from the Codex GPT-5.5 pass)

- **The enumerated driver path is input-file-backed** → added **Slice 3** to introduce a real `PARTITION_ENUMERATED` model with explicit hooks (`num_partitions`, `partition_output_id`, `partition_exists`, `count_partition_units`, `global_forest_offset`) and a driver that does not stat input files for enumerated readers; tested with a **synthetic enumerated reader** before any ctrees change.
- **No `consistent_trees_ascii` fixture exists** (`micro-uchuu` is `lhalo_binary`) → Slice 6 gains a **precursor task to create a tiny ctrees ASCII fixture**; the HDF5 vehicle `micro-uchuu-hdf5` (`consistent_trees_hdf5`) is valid for Slice 5.
- **The size proxy is not a bound** (orphans can make processed halos exceed input halos; `--compress`/HDF5 metadata decouple record bytes from file bytes) → reframed as a **soft target**; renamed knob to `output.target_file_size`; `output.forests_per_file` is the **exact, deterministic** knob (preferred for tests).
- **`output.target_file_size` needs a wide-integer parser** (current `get_strict_int_value` returns `int`, rejecting > INT_MAX; `MimicConfig` has no wide byte field) → added to Slice 2.
- **"Small sims → no behavior change" is false for MPI ctrees** (today `NTask` files; one default chunk → 1 file) → corrected; default chunking *does* change MPI ctrees file count (fewer, bounded — the intended improvement). Serial single-chunk output is unchanged.
- **New config fields must be persisted** in `src/io/output/metadata_hdf5.c` `config_params[]` and run metadata → added to Slice 2.
- **Master file reopens every output file per snapshot** (`O(NOUT × nchunks)` metadata opens; `master_hdf5.c:94-148`) and is **HDF5-only** → Slice 3 generalizes *and* restructures it to open each chunk once; binary downstream handled in Slices 5/8.
- **Empty chunk/rank handling** (`write_hdf5_attrs` fatals on `Ntrees<=0`, `hdf5.c:329`) → planner never emits empty chunks; driver skips ranks with no assigned chunk; `NTask > nchunks` test added.
- **`mimic-plot` binary loader assumes output ids = input file numbers** (`plot/mimic-plot/mimic-plot.py:748,792,938`) → new **Slice 8** for downstream consumers (binary plotting, schema/python-example/master readers).
- **Memory at Uchuu scale**: the one-time global prepare uses **prefix sums** over per-file `Nforests`, and per-chunk *processing* state covers only the active range → explicit constraint in Slices 4/5. (Run-scoped *planning* arrays remain `O(totnforests)`; this was refined by the second-review note below — see line "No resident all-forest arrays was overstated.")
- **`compute_forest_cost_from_nhalos`** is `static inline` and its quadratic path multiplies `int64_t` before casting to `double` (overflow risk for Shin-Uchuu's huge `nhalos`) → Slice 5 de-statics it, declares it in the header, and casts to `double` before multiply.

## Second review (Codex GPT-5.5, effort=high) corrections folded in

- **No per-partition cost hook existed in the proposed interface.** The driver needs per-partition cost for LPT, but the Slice 3 hook list had only count/existence/offset. → Slice 3 adds an explicit `partition_cost(partition)` hook (or a bulk cost accessor); the synthetic reader and both ctrees readers publish cost through it.
- **The 32-bit forest-index limit blocks full Uchuu today, independent of this work** (see "Scope boundary" below). → Made an explicit precondition; the plan no longer implies chunking alone enables full Uchuu.
- **"No resident all-forest arrays" was overstated.** Per-forest planning arrays (`nhalos_per_forest`, sizes, boundaries) are inherently `O(totnforests)` and the current reader already allocates `nhalos_per_forest` for all forests (`src/io/tree/read_ctrees_hdf5.c:1325`). → Reworded to the real invariant: per-forest *planning* arrays are `O(totnforests)` (bounded by the INT_MAX cap until it is lifted); per-chunk *processing* state (ForestInfo cache, field handles, read window, halo payloads) is staged per chunk, not held for all forests. Memory model made explicit in Slices 1/4/5.
- **The "always nonzero `UniqueGalaxyID`" assertion was wrong** (the (0,0) galaxy encodes to 0). → Replaced with the correct invariant + the id==0 sentinel resolution (Slice 5).
- **Slice 4 is a run-scoped lifecycle change, not a local function split** — `open_partition_ctrees_hdf5` currently opens, allocates, stages, caches, and `close_partition` frees *all* state per partition (`src/io/tree/read_ctrees_hdf5.c:1477`, `:1556`). → Slice 4 scope clarified to introduce run-scoped prepare/teardown distinct from per-chunk open/close, with an explicit run-scoped-vs-chunk-scoped cache split (ForestInfo cache `:148`, field handles, file/forest groups `:152/:161`, read window).
- **Master "byte-identical" is not a valid oracle** — the master writes `RunEndTime` (`src/io/output/metadata_hdf5.c:604-609`). → Slice 3 validation compares link structure + galaxy datasets, excluding run-timestamp metadata.
- **Anchor fixes:** the binary plot bug is `plot/mimic-plot/mimic-plot.py:748,792` (the `:938` HDF5 fallback already globs partitions); `docs/DEVELOPER-GUIDE.md:891` is the `partition_model` enum comment and `.agents/skills/mimic-simulations/SKILL.md:123` is task-range cache vocabulary — both need task→chunk updates but are not "file-count" claims. The real "one output file per task" prose is `docs/USER-GUIDE.md:378`.

## Scope boundary and preconditions

**The 32-bit forest-count guard blocks full Uchuu today, but it is a contained guard — not a deep type refactor — and is scheduled here as Slice 7.** Both ctrees readers FATAL when `totnforests >= INT_MAX` (`src/io/tree/read_ctrees_hdf5.c:1315-1318`, `src/io/tree/read_ctrees_ascii.c:288-290`). Full Uchuu has **~3.22 billion forests** (`simulations/uchuu/README.md:3`), exceeding INT_MAX (~2.147 billion), so it cannot run today. Investigation (2026-06-23) shows the guard is **conservative, tied to the pre-chunking per-task staging model**, not a structural width problem:

- The `UniqueGalaxyID` encoding already supports billions of forests: `max_forests = LLONG_MAX / TREE_MUL_FAC` (`src/include/galaxy_id.h:15`), and `mimic_unique_galaxy_id_total_forests_valid(totnforests)` is checked *before* the INT_MAX guard and passes for 3.22 B. The code comments call the INT_MAX check "the 32-bit staging limit … binding today." **No width or forest-count-range encoding change is needed**; the separate `0`-sentinel cleanup remains part of Slice 5.
- The global path is already `int64_t`: `totnforests`, `GlobalForestOffset`, `count_partition_trees`, and the HDF5 reader's per-forest iteration/allocation.
- `int` truncation occurs **only at per-partition granularity** (`Ntrees = (int)nforests_this_task` `:1345`, `TreeID`/`unit`, reader hooks, HDF5 `Ntrees`/`TotHalosPerSnap`/`TreeHalosPerSnap` `H5T_NATIVE_INT` attributes, master counters). Today one task can own the whole forest list, so the guard is necessary. **Chunking bounds every per-partition count to the chunk size (≪ INT_MAX), making the guard safe to relax to a per-chunk check.**

Therefore lifting the limit is **(a)** relaxing the global guard to per-chunk (small, contained) **plus (b)** a streaming/two-pass planner so the `O(totnforests)` weighting array (~26 GB at 3.22 B forests, `src/io/tree/read_ctrees_hdf5.c:1325`) is never resident (a memory, not a width, issue). Both are now **Slice 7** of this plan. It must land **after** Slices 5–6 (chunking must bound per-partition counts first); it cannot be a "Slice 0" because the unchunked per-task path would overflow `Ntrees` without it.

Until Slice 7 lands, this plan targets the **manageable-file-size** problem for sims at or below the current limit (mini-Uchuu, micro-Uchuu, and per-run-feasible subsets); Slice 7 then makes full Uchuu/Shin-Uchuu genuinely runnable.

## Decisions already resolved (override during implementation if desired)

- **Assignment policy: LPT greedy, default on**, deterministic tie-break by ascending chunk id. (User-confirmed.)
- **Knobs:** `output.target_file_size` (bytes, **soft target**, default 4 GiB) and `output.forests_per_file` (exact forest count per chunk; default 0 = derive from `target_file_size`). `forests_per_file` is the deterministic knob used in tests. **Neither knob bounds the *halo* count in a chunk** (forests/bytes ≠ halos), so a per-chunk halo-count guard is required — see Slice 5.
- **Per-forest size estimate (HDF5 only):** `nhalos_forest × sizeof(struct HaloOutput)`. This is an **estimate, not a bound** — orphan growth can raise the processed-halo count above input halos, and only halos at output snapshots are written, and `--compress`/HDF5 metadata change on-disk bytes. Chunk sizing therefore targets, and does not guarantee, `target_file_size`. ASCII cannot pre-count halos → it uses `forests_per_file` only (and fatals if it is unset; see Slice 6).
- **`UniqueGalaxyID == 0` sentinel resolution:** reserve `0` as a pure sentinel by making the encoder 1-based in the global forest index (`src/include/galaxy_id.h:29`), so no real galaxy ever encodes to `0`. This is consumer-agnostic (fixes SHAM and any future id==0 consumer at once) and keeps the identity invariant exact for every galaxy. This intentionally changes the id encoding contract before v1.0 rather than preserving the current ambiguous `0` value.

---

## Slice 1: Chunk-planning module (pure, unit-tested)

### Intended Change
- Add a format-agnostic chunk planner with two pure functions:
  - `chunk_plan_build_boundaries(int64_t nforests, const double *size_per_forest, double size_budget, int64_t forests_per_file_override, ...)` → contiguous chunk boundaries (forest ranges), `NTask`-independent and deterministic. **Never emits an empty chunk.** A forest whose size alone exceeds `size_budget` becomes its own single-forest chunk. When `forests_per_file_override > 0`, chunk by fixed forest count instead of size.
  - `chunk_plan_assign_lpt(int64_t nchunks, const double *cost_per_chunk, int ntasks, int *task_of_chunk)` → LPT greedy (sort chunks by cost descending; **stable tie-break by ascending chunk id**; assign each to the least-loaded task). Deterministic across ranks/platforms.
- **Scalable-by-design API (Vision principle 5 — bounded memory).** Design the boundary builder to consume forest sizes as a **stream** (a pull callback / per-file feed `(nforests_in_file, sizes)`), emitting `O(nchunks)` chunk descriptors, so its working set never scales with `totnforests`. Provide a thin full-array convenience wrapper for tests and the sub-limit path, but make the streaming signature the primary one so Slice 7 only swaps the data source (per-file reads) — it does **not** rework the API. The assignment step is `O(nchunks)` and `nchunks ≪ totnforests`, so it is never the memory bottleneck. This keeps the planner compliant with "long runs do not accumulate memory with the number of forests" from day one, rather than retrofitting it.

### Acceptance Criteria
- Inputs: **the primary boundary-builder signature is a streaming feed** (a pull callback / per-file `(nforests, sizes)` feed) plus size budget / forest-count override; a thin **full-array wrapper** (`size_per_forest[]`) is provided for tests and the sub-limit path. Assignment takes a per-chunk cost array + task count.
- Outputs: contiguous non-empty chunk boundary list (as `O(nchunks)` descriptors); `task_of_chunk` assignment array. The streaming and full-array forms produce identical boundaries on the same data.
- User-visible behaviour: none yet (not wired in).
- Behaviour that must not change: nothing wired; build stays green.

### Authorized Surface
- Files: **new** `src/io/tree/chunk_plan.h`, **new** `src/io/tree/chunk_plan.c`, **new** `tests/unit/test_chunk_plan.c`, and `tests/unit/run_tests.sh` (add `src/io/tree/chunk_plan.c` to `IO_SRCS` so `test_chunk_plan.c` links — the runner hardcodes the shared-source list, `tests/unit/run_tests.sh:170-186`; a new production source is **not** auto-linked).
- Tests: new `test_chunk_plan.c`; regenerate `build/generated/unit_tests.txt` via `make generate` (never hand-edit it).

### Explicit Non-Goals
- No driver, reader, config, or output changes; no exposure of `compute_forest_cost_from_nhalos` yet (Slice 5).

### Risk Flags
- Risky surfaces touched: none (pure module).
- Approval needed: no.

### Validation Plan
- Tests: single forest > budget (own chunk); even chunking; **never empty chunk**; `forests_per_file` override; zero forests; `ntasks==1` (all → task 0); `ntasks > nchunks` (some tasks get none); `nchunks ≫ ntasks` LPT makespan within `4/3` of optimal on a spiky distribution; determinism (same inputs → same boundaries and assignment, independent of `ntasks`); tie-break stability. Emit `MIMIC_RESULT:` markers.
- Commands: `tests/unit/run_tests.sh test_chunk_plan`; `make check-format`.

### Rollback Path
- Delete the three new files; rerun `make generate`.

---

## Slice 2: Output chunking config knobs (plumbing + metadata persistence, inert, tested)

### Intended Change
- Add `output.target_file_size` (bytes, soft target, default 4 GiB) and `output.forests_per_file` (default 0). Because 4 GiB > INT_MAX, add a **strict wide-integer parser** (`get_strict_int64_value` or size-suffix aware) — do not reuse `get_strict_int_value` (`src/core/read_parameter_file.c:422`, returns `int`).
- Add `int64_t`/`size_t` fields to `MimicConfig` (`src/include/types.h`), parse + validate in `parse_output_section` (`src/core/read_parameter_file.c:553-612`, extend `valid_keys`), set defaults in the defaults block (`src/core/main.c:301-308`).
- **Persist both fields** in the HDF5 `config_params[]` table (`src/io/output/metadata_hdf5.c:512`) and confirm they reach run metadata (`src/core/main.c:687`, `write_run_metadata`). The `ConfigParamDescriptor` type today supports only `INT`/`DOUBLE`/`STRING` (`src/io/output/metadata_hdf5.c:57-60`), so **add an `INT64` descriptor type writing `H5T_NATIVE_INT64`** for `target_file_size` (and `forests_per_file` if stored wide).
- Document both keys in the User Guide output section.

### Acceptance Criteria
- Inputs: `output.target_file_size`, `output.forests_per_file`.
- Outputs: populated `MimicConfig` fields (wide int); fatal on negative/garbage; values appear in HDF5 `RunProperties`.
- User-visible behaviour: keys parse, echo at DEBUG, and are recorded in metadata; **no output-layout change** yet (not consumed until Slice 5).
- Behaviour that must not change: all existing run files parse unchanged.

### Authorized Surface
- Files: `src/include/types.h`, `src/core/read_parameter_file.c` (parse + new wide parser + `valid_keys`), `src/core/main.c` (defaults only), `src/io/output/metadata_hdf5.c` (config_params table), `docs/USER-GUIDE.md` (output section ~388-397).
- Tests: extend `tests/unit/test_parameter_parsing.c` (defaults, parse, fatal on bad input, > INT_MAX accepted).

### Explicit Non-Goals
- Do not consume the values in any reader/driver/output logic; no naming changes.

### Risk Flags
- Risky surfaces touched: **public YAML contract**, **wide-int parsing**, **metadata schema (new RunProperties params)**.
- Approval needed: **yes**.

### Validation Plan
- Tests: `test_parameter_parsing.c` (incl. a 4 GiB value round-tripping through the wide parser).
- Commands: `tests/unit/run_tests.sh test_parameter_parsing`; `make check-docs`; `make check-format`; a small HDF5 run to confirm the new params land in `RunProperties` (`h5ls -d`).

### Rollback Path
- Revert the listed files; keys become unknown again (rejected by `reject_unknown_keys`).

---

## Slice 3: Generalized enumerated-partition driver contract (`PARTITION_ENUMERATED`) — synthetic-reader tested

### Intended Change
- Add a **`PARTITION_ENUMERATED`** partition model to `src/io/tree/reader.h` with explicit hooks: `num_partitions`, `partition_output_id`, `partition_exists` (replaces the input-file `access()` assumption), `count_partition_units`, `global_forest_offset(partition)`, and **`partition_cost(partition)`** (the per-partition compute cost the driver feeds to `chunk_plan_assign_lpt`; a bulk accessor is acceptable). Keep `PARTITION_PER_FILE` and `PARTITION_PER_TASK` working unchanged alongside it.
- **Add nullable run-lifecycle hooks `prepare_run(void)` / `teardown_run(void)` to `TreeReader`** (today's hooks are only per-partition `open_partition`/`load_unit`/`close_partition`, `src/io/tree/reader.h:92-98`, called inside `process_partition`, `src/core/main.c:373`/`:451`). The driver (`run_tree_driver`) calls `prepare_run` once before the partition loop and `teardown_run` once after it (including when a rank is assigned no partitions), so a reader can hold run-scoped state across many `open_partition`/`close_partition` cycles without `close_partition` having to guess whether more chunks remain. Readers that need neither leave both `NULL` (L-Halo unchanged). This is the API that Slice 4 implements.
- Generalize the driver (`src/core/main.c` `run_tree_driver`, `build_partition_file_offsets`, the MPI stride and serial loops): for enumerated readers, **do not stat input files**; obtain existence via `partition_exists`, offsets via `global_forest_offset`, and assignment via Slice 1's LPT (`chunk_plan_assign_lpt`). Each rank processes its assigned partition ids in **ascending order**. Handle **empty ranks** (a rank assigned no partitions does nothing and never calls output finalization).
- Generalize `write_master_file` (`src/io/output/master_hdf5.c`) to enumerate ids via `num_partitions`/`partition_output_id` (drop the `PARTITION_PER_TASK` special case at `:64-70`) and **restructure to open each output file once**, reading all snapshot totals in a single pass instead of `O(NOUT × nchunks)` reopens (`:94-148`).
- **Extract the driver loop into a linkable module so it is unit-testable.** Today `run_tree_driver`, `process_partition`, and `claim_and_process_partition` are `static` inside `src/core/main.c` (`:560`, `:368`, `:468`), which also defines `main()` — so unit tests cannot link them (that is why `tests/unit/test_stubs.c` exists). Move the enumerated driver loop into a new non-`main` translation unit (e.g. `src/core/tree_driver.c` + header); `main()` just calls it. This is also a modularity win (Vision principle 1 — a thinner `main`). Add `src/core/tree_driver.c` to `tests/unit/run_tests.sh` `CORE_SRCS`.
- Provide a **synthetic enumerated test reader** (test-only) so the driver + master contract is exercised against the extracted module without HDF5 tree data. (If the team prefers not to extract, the fallback is to validate the driver via an **integration** test running the real binary — but extraction is recommended and assumed below.)

### Acceptance Criteria
- Inputs: a synthetic enumerated reader with N partitions and known unit/offset/cost.
- Outputs: driver iterates LPT-assigned partitions per rank in ascending id; master file links exactly the existing partition ids; one-pass total accounting.
- User-visible behaviour: none for production readers (ctrees still `PARTITION_PER_TASK`, L-Halo still `PARTITION_PER_FILE`) — both unchanged.
- Behaviour that must not change: L-Halo and ctrees output byte-identical; existing master-file structure/links identical for L-Halo.

### Authorized Surface
- Files: `src/io/tree/reader.h`, `src/io/tree/interface.c` (shared enumerated helpers beside the per-file helpers), **new** `src/core/tree_driver.{c,h}` (extracted driver loop), `src/core/main.c` (call the extracted driver; lifecycle hook wiring), `src/io/output/master_hdf5.c` (enumeration + one-pass totals), `tests/unit/run_tests.sh` (add `src/core/tree_driver.c` to `CORE_SRCS`), **new** test-only synthetic reader + `tests/unit/test_enumerated_driver.c` (or extend `test_tree_reader_counts.c`).
- Tests: new synthetic-reader unit test; L-Halo regression (master links unchanged).

### Explicit Non-Goals
- Do not change the ctrees readers yet.
- Do not remove `PARTITION_PER_TASK`.
- No new config consumption (that is Slice 5).

### Risk Flags
- Risky surfaces touched: **shared reader API contract, core driver, MPI decomposition, master-file aggregation**.
- Approval needed: **yes**.

### Validation Plan
- Tests: synthetic-reader unit test (LPT assignment honored, ascending processing, empty ranks, master one-pass totals). **Lifecycle:** assert `prepare_run` is called exactly once before the partition loop and `teardown_run` exactly once after the last assigned partition — including the idle-rank case (no partitions) and the `--skip` case (some partitions skipped). `make tests-unit summary` (delegate, capture, check exit code).
- Regression: run mini-Millennium **L-Halo binary + HDF5** before/after; galaxy datasets and master link structure identical (compare data + links, **not** raw bytes — the master writes a wall-clock `RunEndTime`, `src/io/output/metadata_hdf5.c:604`).
- Progress display: confirm the serial global progress bar and the MPI per-partition fallback log lines still behave for enumerated partitions (`src/core/main.c:376-388`, `:621-637`).
- Commands: `make USE-MPI=yes` build; `make check-format`.

### Rollback Path
- Revert this slice; the new enumerated model and synthetic reader are inert without a production reader using them.

---

## Slice 4: ctrees HDF5 internal refactor — one-time prefix-sum prepare + range-scoped staging (output identical)

### Intended Change
- **This is a lifecycle restructure, not a local function split.** Today `open_partition_ctrees_hdf5` (`src/io/tree/read_ctrees_hdf5.c:1477`) opens the metadata file, allocates, stages, and caches *all* per-partition state, and `close_partition_ctrees_hdf5` (`:1556`) frees *all* of it. Split this into:
  - a **run-scoped prepare** implemented as the `prepare_run` hook added in Slice 3 (called once by the driver before the partition loop) that reads `Nfiles` + per-file `Nforests` and builds **prefix sums** for `totnforests` and per-forest cost (when weighting). This state outlives individual partition opens and is freed by the `teardown_run` hook (called once after the last partition, including idle ranks).
  - a **chunk-scoped stage/unstage** (`open_partition`/`close_partition`) that, given `[start_forest, end_forest)`, sets `GlobalForestOffset`, `Ntrees`, the per-forest `(file, treenr)` lookup, and rebuilds the per-chunk caches that today are per-partition: the `ForestInfo` cache (`:148`), per-file `Forests/<field>` handles, open file/forest groups (`:152`, `:161`), and the read window (`:163`). `close_partition` must free **only** chunk-scoped state and leave run-scoped state intact.
- **Memory invariant:** run-scoped per-forest arrays (`nhalos_per_forest`, prefix sums) are `O(totnforests)` and allocated once (not per partition); chunk-scoped caches and halo payloads cover only the active range. No per-forest *halo payload* or full `(file, treenr)` map for all forests is held resident.
- Keep `partition_model = PARTITION_PER_TASK` and output **byte-identical**: `open_partition(ThisTask)` stages the task's whole weighted range exactly as today. Output-preserving — no interface or output change.

### Acceptance Criteria
- Inputs/Outputs: **byte-identical** ctrees HDF5 output (serial and MPI) vs pre-change.
- Behaviour that must not change: galaxy IDs, halo counts, per-task ranges, weighted-distribution results, memory categories, file layout.
- New invariant: **chunk-scoped** caches and halo payloads (ForestInfo cache, field handles, read window, per-forest `(file,treenr)` for the active range) are bounded by the active range; **run-scoped planning arrays** (`nhalos_per_forest`, prefix sums) remain `O(totnforests)` and are allocated once (not per partition). This is the consistent memory model — it does not claim all-forest memory is eliminated, only that per-chunk processing state is range-bounded (full elimination of the `O(totnforests)` arrays is the separate streaming/int64 work in "Scope boundary").

### Authorized Surface
- Files: `src/io/tree/read_ctrees_hdf5.c` (+ `.h`), `src/io/tree/read_ctrees_common.h` (only if shared staging helpers are factored). The `CTH` cache struct and new static prepare/stage helpers.
- Tests: extend `tests/unit/test_ctrees_hdf5_reader.c` / `test_ctrees_support.c`.

### Explicit Non-Goals
- No chunk enumeration, no driver/master/config consumption, no ASCII change.

### Risk Flags
- Risky surfaces touched: tree-reading correctness, memory ownership (`MEM_IO`/`MEM_TREES`), `GlobalForestOffset`.
- Approval needed: **yes**.

### Validation Plan
- Tests: reader unit tests; regression that staged `[start,end)` arrays for the full range equal the pre-refactor task-range arrays. **Repeated-staging test:** stage two non-adjacent ranges in sequence (touching different HDF5 files) and verify the `ForestInfo` cache, field handles, file/forest groups, and read window are rebuilt for each and that `close_partition` frees only chunk-scoped state (run-scoped prefix sums survive).
- Run **micro-uchuu-hdf5** (`consistent_trees_hdf5`) serially and on N ranks before/after; `h5diff` galaxy datasets / compare `UniqueGalaxyID` sets → identical. `make tests-unit summary` (delegate, check exit code).
- `--debug` confirms run-scoped metadata read **once** across multiple stages; chunk-scoped caches are released between stages while run-scoped prefix-sum arrays persist.

### Rollback Path
- Revert `read_ctrees_hdf5.{c,h}`.

---

## Slice 5: Switch ctrees HDF5 to chunk enumeration + LPT (consumes Slices 1–4)

### Intended Change
- Implement the `PARTITION_ENUMERATED` hooks on the ctrees HDF5 reader using Slice 4's prefix-sum prepare and Slice 1's planner: `num_partitions` (= nchunks from `chunk_plan_build_boundaries`, size proxy `nhalos × sizeof(struct HaloOutput)` at `target_file_size`, or `forests_per_file`), `partition_output_id` (= global chunk id), `partition_exists`, `count_partition_units`, `global_forest_offset(chunk)`, and per-chunk cost for LPT. `open_partition(chunk_id)` stages that chunk's forest range (Slice 4) and sets `GlobalForestOffset` to the chunk's first global forest. Switch `partition_model` to `PARTITION_ENUMERATED`. **Feed the planner via the full-array wrapper here (an acknowledged sub-limit interim); Slice 7 switches the feed to true per-file streaming.**
- **Per-chunk output-halo guard.** `forests_per_file`/`target_file_size` bound *forests*/*bytes*, not *halos*; a chunk holding a few very large forests (Shin-Uchuu monster trees) could emit more than `INT_MAX` halos in a single snapshot, overflowing the `int` per-snapshot counters and output attributes (`TotHalosPerSnap`/`InputHalosPerSnap`, `src/include/globals.h:110-111`; `H5T_NATIVE_INT` attrs `src/io/output/hdf5.c:301-336`; binary header `src/io/output/binary.c:191-204`; master `src/io/output/master_hdf5.c:35`). Add a **pre-increment overflow check at the writer counters** — the authoritative site, since processed/orphan halos are only known at write time — at `TotHalosPerSnap[n]++`/`InputHalosPerSnap[n][tree]++` (`src/io/output/hdf5.c:422-423`, `src/io/output/binary.c:148-150`), fataling with a clear message naming the chunk and snapshot if a counter would exceed `INT_MAX`. This is a bounded safety precondition, not a global int64 output refactor.
- De-`static` `compute_forest_cost_from_nhalos`, declare it in `src/io/tree/ctrees/forest_utils.h`, and **cast to `double` before the quadratic multiply** (overflow safety for huge `nhalos`).
- Ensure **binary output** is correct under chunking: one file per snapshot per chunk id, finalize headers with chunk-local `Ntrees`, partial-snapshot-set behavior preserved.
- **Resolve the `UniqueGalaxyID == 0` sentinel ambiguity** so SHAM (and any future id==0 consumer) is reproducible under chunking. Make the encoding reserve `0` as a pure sentinel (e.g. 1-based `forestnr_global`), updating `src/include/galaxy_id.h` and `mimic_unique_galaxy_id_components_valid`. This intentionally changes the pre-v1.0 id encoding contract instead of preserving a one-galaxy ambiguity tied to `FileNum`/`TreeID`.

### Acceptance Criteria
- Inputs: ctrees HDF5 run + `output.target_file_size` / `output.forests_per_file`.
- Outputs: output file count = `ceil(estimated_size / target_file_size)` (or `ceil(nforests / forests_per_file)`), **independent of `NTask`**; one self-contained chunk per id (HDF5 and binary); master links all chunk ids.
- User-visible behaviour: large ctrees runs produce many bounded files instead of `NTask` huge files; `--skip` resumes per chunk. **MPI ctrees file count changes** (fewer, bounded files) — intended.
- Behaviour that must not change: **`UniqueGalaxyID` set + per-field values byte-identical** across serial vs MPI and across `NTask`/chunk sizes; L-Halo readers unaffected.

### Authorized Surface
- Files: `src/io/tree/read_ctrees_hdf5.c` (+ `.h`), `src/io/tree/ctrees/forest_utils.{c,h}` (de-static + overflow fix), `src/io/tree/reader.h` (only if a hook signature needs adjusting), `src/io/output/hdf5.c` and `src/io/output/binary.c` (per-chunk halo-counter overflow guard at the increment sites), `src/io/output/util.c`/`util.h` (only for a naming-semantics comment — output id is already an int).
- Tests: ctrees HDF5 integration reproducibility test; reader/galaxy-id unit tests.

### Explicit Non-Goals
- No ASCII change (Slice 6); do not remove `PARTITION_PER_TASK` (Slice 9); no dynamic/work-stealing balancing (future); no L-Halo change.

### Risk Flags
- Risky surfaces touched: **galaxy-ID identity, `FileNum` semantics (now chunk id), output layout, binary + HDF5 writers, MPI decomposition, id-encoding limits**.
- Approval needed: **yes** (highest-risk slice).

### Validation Plan
- Integration (`tests/integration/`): run **micro-uchuu-hdf5** at `NTask = 1, 2, 4` with a small `forests_per_file` forcing multiple chunks; assert the union of all chunk files has an identical `UniqueGalaxyID` set + per-field values across all runs, **chunk file contents identical between runs**, and file count = expected and `NTask`-independent. Include `NTask > nchunks` (idle ranks). Run once each in **HDF5 and binary** output.
- **`--skip`/resume:** complete a multi-chunk run, delete a subset of chunks, rerun with `--skip`; assert only the missing chunks are reprocessed, the master file is rebuilt to include all chunks, and a partial chunk (some-but-not-all binary snapshot files present) is rejected per `claim_and_process_partition` (`src/core/main.c:468-491`). HDF5 master regeneration happens on rank 0 after the barrier (`src/core/main.c:811`, `:823`) — confirm it links skipped-then-present chunks.
- Identity coupling: assert the `UniqueGalaxyID` **set + per-field values are invariant across chunkings/`NTask`** (the real oracle). Do **not** assert IDs are always nonzero — the (forest 0, halo 0) galaxy encodes to `0` (`src/include/galaxy_id.h:29`). Specifically test the SHAM path: with the id==0 sentinel resolved (see Intended Change), the first galaxy's stellar mass must also be invariant; without the fix, document that exactly that one galaxy's SHAM mass tracks the chunk id via `FileNum` (`sham_assign_stellar_mass.c:68`).
- Encoding: `test_galaxy_id_encoding.c` — per-chunk offsets within `mimic_unique_galaxy_id_*` limits.
- Commands: `make USE-MPI=yes`; delegate `make tests-unit summary` + `make tests-integration summary` (capture to `archive/test-logs/`, check exit codes); `make check-generated`; `make check-format`.
- Manual: `--debug` prints the chunk plan (nchunks, target, assignment) and one metadata read; `h5ls -r` master links to every chunk id.

### Rollback Path
- Revert this slice; ctrees HDF5 returns to Slice 4 state (`PARTITION_PER_TASK`, identical behavior). Slices 1–4 stay (inert without this slice).

---

## Slice 6: ctrees ASCII — chunk-by-forest-count (graceful degradation)

### Intended Change
- `consistent_trees_ascii` is a live required reader for the upcoming Shin-Uchuu simulation; this slice replaces only its output partitioning model, not the reader or its registration.
- **Fixture precursor: RESOLVED.** The `simulations/micro-uchuu-ascii/` package has been restored (restored from archive 2026-06-25). The production ASCII tree data is mounted at `/Volumes/Internal/data/uchuu/microuchuu/micro-uchuu-ascii` (11 GB, 561,266 forests) via the `snapshots/` symlink. This is the test vehicle for Slice 6 — no separate tiny synthetic fixture is needed because the ASCII reader has no HDF5-style random-access fixture format; the integration test skips gracefully when the data is not mounted. The existing `_tests/integration/test_reader_smoke.py` validates the reader loads and produces sensible halo counts; Slice 6 adds a new `test_ascii_chunks.py` that sets `forests_per_file` and validates chunk file count and `UniqueGalaxyID` set identity.
- Apply the Slice 4/5 pattern to `src/io/tree/read_ctrees_ascii.c`: one-time prepare for global forest counts (it already computes `totnforests` at runtime, `src/io/tree/read_ctrees_ascii.c:275-290`), chunk enumeration by **forest count** (`output.forests_per_file`; ASCII cannot pre-count *halos*, so there is no `target_file_size` proxy), `PARTITION_ENUMERATED` hooks, `open_partition(chunk_id)` staging one chunk's forests, **uniform** per-chunk cost (LPT degrades to round-robin — no regression vs today's uniform ASCII split).
- **Define the default-config behavior:** because ASCII cannot derive a chunk count from `target_file_size`, an ASCII run with `output.forests_per_file == 0` (the global default) must **fatal at startup with a clear message** instructing the user to set `output.forests_per_file` for the `consistent_trees_ascii` reader. (Do not silently fall back to one-chunk-per-task — that reintroduces the original problem.) Add a test for this fatal path.

### Acceptance Criteria
- Outputs: file count = `ceil(nforests / forests_per_file)`, `NTask`-independent; master links all chunk ids.
- Behaviour that must not change: galaxy-ID set identical across `NTask`; ASCII still cannot byte-balance (documented).

### Authorized Surface
- Files: `src/io/tree/read_ctrees_ascii.c` (+ `.h`), `src/io/tree/read_ctrees_common.h` (shared staging only if factored), **new** `simulations/micro-uchuu-ascii/_tests/integration/test_ascii_chunks.py`.
- Tests: ASCII reader integration tests against `simulations/micro-uchuu-ascii/` (skip when data not mounted).

### Explicit Non-Goals
- No size-weighted ASCII chunking (unsupported); no HDF5-reader/driver/master changes beyond Slices 3/5.

### Risk Flags
- Risky surfaces touched: tree-reading correctness, galaxy-ID identity, MPI (ASCII).
- Approval needed: **yes**.

### Validation Plan
- Integration: `simulations/micro-uchuu-ascii/` at `NTask = 1` (and `NTask = 2` if MPI) with small `forests_per_file`; identical galaxy-ID set + reproducible chunk contents; file count = expected. Tests skip when data not mounted.
- Commands: delegate `make tests-integration summary` + `make tests-unit summary`; `make check-format`.

### Rollback Path
- Revert the ASCII reader changes; ASCII returns to `PARTITION_PER_TASK` (still supported only until Slice 9 cleanup). The `simulations/micro-uchuu-ascii/` package stays (it is independent of the reader changes).

---

## Pre-Slice 7 checkpoint: refresh intentional UniqueGalaxyID baselines

### Intended Change
- Refresh committed baseline outputs after the Slice 5 sentinel-reserving `UniqueGalaxyID` encoding change and the Slice 6 ASCII checkpoint, before starting Slice 7. This is a baseline maintenance checkpoint, not a numbered implementation slice.
- First validate that the current outputs differ from the old baselines only in `UniqueGalaxyID` and `UniqueCentralGalaxyID`; all other compared fields must remain identical within the existing baseline tolerances.
- After that validation is green, regenerate and commit the affected baseline files so `make tests` is green before the Slice 7 streaming/scale work begins. Detailed task steps live in `docs/dev/pre-slice-7-baseline-refresh.md`.

### Acceptance Criteria
- `tests/integration/test_output_formats.py` current binary/HDF5 outputs match their committed baselines for all compared fields except `UniqueGalaxyID` and `UniqueCentralGalaxyID` before refresh.
- `models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py` current SAGE physics output matches its committed baseline for all fields except `UniqueGalaxyID` and `UniqueCentralGalaxyID` before refresh.
- After refreshing the baseline files, both tests pass normally without excluding any fields.

### Authorized Surface
- Files: committed baseline artifacts under `tests/data/output/baseline/{binary,hdf5}/` and `models/sage16/modules/_tests/baseline/physics-binary/`.
- Documentation: `docs/dev/pre-slice-7-baseline-refresh.md` and this plan note.

### Explicit Non-Goals
- No production code changes; no test weakening or permanent exclusion of the ID fields; no Slice 7 streaming/guard work.

### Risk Flags
- Risky surfaces touched: committed scientific/reference data.
- Approval needed: yes, before committing refreshed baselines.

### Validation Plan
- Run the pre-refresh exclusion check described in `docs/dev/pre-slice-7-baseline-refresh.md`.
- Refresh the baseline artifacts from freshly generated current outputs.
- Rerun `python3 tests/integration/test_output_formats.py` and `python3 models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py`; both must pass normally.

### Rollback Path
- Revert the baseline-refresh commit; the old baselines return and the known ID-only failures recur.

---

## Slice 7: Scale past the 32-bit forest-count limit — per-chunk guard + streaming planner

### Intended Change
- **Relax the guard, don't widen output structs.** Replace the global `totnforests >= INT_MAX` FATAL in both ctrees readers (`src/io/tree/read_ctrees_hdf5.c:1315`, `src/io/tree/read_ctrees_ascii.c:288`) with a **per-chunk** guard (each chunk's forest count < INT_MAX, guaranteed by the planner) plus the existing `int64` `mimic_unique_galaxy_id_total_forests_valid(totnforests)` encoding check as the real global limit. Per-partition `int` counts (`Ntrees`, `TreeID`, HDF5 `Ntrees`/`TotHalosPerSnap`/`TreeHalosPerSnap` attributes) stay `int32` because chunks are bounded — no output-schema width change, no encoding change (the `UniqueGalaxyID` encoding already supports billions of forests, `src/include/galaxy_id.h:15`).
- **Make the planner streaming/two-pass (the scalable design, per Vision principle 5 "bounded memory").** Eliminate the resident `O(totnforests)` per-forest weighting array (~26 GB at 3.22 B forests, `src/io/tree/read_ctrees_hdf5.c:1325`). Pass 1 streams per-file forest metadata (`nhalos`) to compute chunk boundaries (running size sum) and per-chunk cost, emitting only **`O(nchunks)` chunk descriptors** (forest range + cost); per-file metadata is read and discarded. Pass 2 stages and processes one chunk at a time (Slice 4/5 machinery). Resident memory is then `O(nchunks) + O(per-file) + O(active chunk)`, **independent of total forest count** — the property the vision asks for. **Switch the ctrees readers' feed to the streaming `chunk_plan` API (the primary form delivered in Slice 1) and stream the forest-distribution cost computation (`forest_utils.c`)**, replacing the Slice 5 full-array wrapper so no `O(totnforests)` array is materialized.
- Also validate that **`nchunks` fits `int`** (the `num_partitions` return type): the planner fatals if the configured chunk size is so small that `nchunks >= INT_MAX` (a config-validation limit, not just "sane minimum chunk size" prose).
- Keep `num_partitions` returning `int`; document a **sane minimum chunk size** so `nchunks` stays well within `int` (millions of chunks at most; sub-thousand-forest chunks are an inode anti-pattern regardless).

### Acceptance Criteria
- Inputs: ctrees metadata with `totnforests > INT_MAX` (mocked — no real 26 GB run needed); `target_file_size` / `forests_per_file`.
- Outputs: planning + processing proceed without the global FATAL; resident memory bounded by chunk scope + `O(nchunks)`, not `O(totnforests)`.
- User-visible behaviour: full Uchuu/Shin-Uchuu-scale forest counts become runnable (subject to disk/walltime); output for sims below the old limit is unchanged.
- Behaviour that must not change: `UniqueGalaxyID` set + per-field values identical to a chunked run that stayed under the old limit; per-chunk output attributes remain `int32`.

### Authorized Surface
- Files: `src/io/tree/chunk_plan.{c,h}` (streaming variant), `src/io/tree/read_ctrees_hdf5.c` (+ `.h`), `src/io/tree/read_ctrees_ascii.c`, `src/io/tree/ctrees/forest_utils.{c,h}` (streaming distribution), `src/io/tree/reader.h` (only if a streaming-prepare hook signature is needed).
- Tests: mocked over-limit planning test; memory-bound assertion; reproducibility vs a sub-limit chunked run.

### Explicit Non-Goals
- No additional `UniqueGalaxyID` encoding change beyond the Slice 5 sentinel cleanup; no widening of per-partition/output `int32` counts.
- No driver/master/plotting changes beyond what Slices 3/5/8 cover.

### Risk Flags
- Risky surfaces touched: **galaxy-ID identity at scale, memory model, MPI decomposition, the encoding-limit guard**.
- Approval needed: **yes**.

### Validation Plan
- Unit: streaming planner produces identical boundaries/assignment to the full-array planner on sub-limit inputs (equivalence), and handles a mocked `> INT_MAX` forest count with bounded memory.
- Integration: micro-uchuu-hdf5 reproducibility unchanged (streaming vs full-array path give identical chunk files).
- Commands: `make USE-MPI=yes`; delegate `make tests-unit summary` + `make tests-integration summary`; `make check-format`.

### Rollback Path
- Revert this slice; the global INT_MAX guard and full-array planner return (sims below the limit unaffected).

---

## Slice 8: Downstream consumers — binary plotting, schema, examples, master readers

### Intended Change
- Fix the `mimic-plot` **binary** loader, which assumes output ids are input file numbers `first_file..last_file` (`plot/mimic-plot/mimic-plot.py:748`, `:792`). Make it discover chunk ids by globbing actual partition files, mirroring the HDF5 path (the HDF5 fallback at `:938` already globs partitions and is partition-aware — use it as the model). The generated binary example `example_Mvir_Len_plot.py` already globs (`src/io/output/python_example.c:202`), so the main loader is the gap.
- **Numeric-sort chunk ids in consumers.** Output paths zero-pad to 3 digits (`%03d`, `src/io/output/util.c:37-39`; `File%03d` in the master), but consumers concatenate `sorted()` HDF5 keys/globs (`plot/mimic-plot/hdf5_reader.py`, `src/io/output/python_example.c:161-170`). Above 999 chunks lexical order ≠ numeric order, so make consumers sort partition ids numerically (preferred over widening the pad, which would change L-Halo output naming). Order does not affect aggregate plots, but fix it for correctness/robustness.
- Verify the run-local `metadata/output_schema.json`, `output_schema.py` reader, and the HDF5 master consumer all behave with many chunk files and chunk-id naming. Update where they assume the input-file range. Note `FieldMetadata` is written once per file (`src/io/output/metadata_hdf5.c`), so it is duplicated across many chunks — confirm consumers read it from any one chunk or the master, not by file index.

### Acceptance Criteria
- User-visible behaviour: plotting and example scripts work on chunked ctrees output (binary and HDF5) without manual file-range edits.
- Behaviour that must not change: L-Halo and small single-chunk outputs plot exactly as before.

### Authorized Surface
- Files: `plot/mimic-plot/mimic-plot.py`, `plot/mimic-plot/output_schema.py` (if it assumes file range), `src/io/output/python_example.c` (if it bakes the file range); plotting tests under `plot/mimic-plot/tests/`.
- Tests: a plotting integration test over a multi-chunk ctrees output (binary + HDF5).

### Explicit Non-Goals
- No new plot types; no reader/driver changes.

### Risk Flags
- Risky surfaces touched: downstream analysis tooling (public-facing outputs).
- Approval needed: no (gate on a green plotting test), but flag if the binary discovery needs a manifest written by the writer (would reach back into Slice 5).

### Validation Plan
- Run `mimic-plot` over a multi-chunk micro-uchuu-hdf5 output (binary and HDF5); confirm figures generate and total galaxy counts match a single-chunk run.
- Run the generated `example_Mvir_Len_plot.py` against multi-chunk binary and HDF5 outputs; confirm it loads all chunks and matches the single-chunk count.
- Commands: plotting tests; `make check-format` (black/isort).

### Rollback Path
- Revert the plotting/example changes; chunked HDF5 still plots via the master, only binary chunked plotting regresses.

---

## Slice 9: Docs, skills sweep, and retire `PARTITION_PER_TASK` (v1.0 blocker)

### Intended Change
- With both ctrees readers enumerated, remove the dead `PARTITION_PER_TASK` enum value and branches (`src/core/main.c`, `src/io/output/master_hdf5.c`), update `src/io/tree/reader.h` docs, and keep `PARTITION_PER_FILE` for live input-file-backed readers. This slice is required before v1.0; the replaced ctrees per-task output model must not remain as a compatibility path.
- **Correct the stale "one output file per task" prose** at `docs/USER-GUIDE.md:378` (the actual claim). Separately, update the **task→chunk vocabulary** in `docs/DEVELOPER-GUIDE.md:891` (the `partition_model` enum comment — drop `PARTITION_PER_TASK`) and `.agents/skills/mimic-simulations/SKILL.md:123` (describes "task-range `ForestInfo`" caching — now chunk-range). Document chunked output, `output.target_file_size` / `output.forests_per_file`, reproducible layout, per-chunk resume, and the soft-target caveat. Update `docs/VISION.md` data-flow note if needed; sweep `mimic-debug` for partition-model references.

### Acceptance Criteria
- No `PARTITION_PER_TASK` references remain; docs/skills describe the chunk model accurately; no ctrees code path, docs, skills, tests, or plotting logic assumes output partition equals MPI task.
- Behaviour unchanged.

### Authorized Surface
- Files: `src/io/tree/reader.h`, `src/core/main.c`, `src/io/output/master_hdf5.c`, `src/io/tree/read_ctrees_*.c` (enum refs only), `docs/USER-GUIDE.md`, `docs/DEVELOPER-GUIDE.md`, `docs/VISION.md`, `.agents/skills/mimic-simulations*`, `.agents/skills/mimic-debug*`.

### Risk Flags
- Risky surfaces touched: enum/API cleanup (compile breakage if a ref is missed).
- Approval needed: no (gate on green full build + suite).

### Validation Plan
- `make` (no warnings under `-Wall -Wextra -Wshadow`), `make USE-MPI=yes`, delegate full `make tests summary`, `make check-docs`, `make check-format`, `make validate-modules`; `rg PARTITION_PER_TASK src/` returns nothing.

### Rollback Path
- Revert Slice 9; the enum and branches return (harmlessly dead).

---

## Cross-cutting validation (run after Slice 5, again after Slice 6)

- **Identity invariant (non-negotiable):** `UniqueGalaxyID` set + per-field values identical across `NTask ∈ {1,2,4}` and across chunk-size settings. With Slice 5's sentinel-reserving encoder change, every real galaxy id is nonzero and every galaxy's SHAM mass is invariant under chunking.
- **Reproducible layout:** chunk file contents identical regardless of `NTask`.
- **Bounded files:** max chunk file ≈ `target_file_size` except a single forest exceeding it (documented); use `forests_per_file` for exact, deterministic counts in tests.
- **No L-Halo regression:** mini-Millennium L-Halo binary + HDF5 output and master file unchanged.
- **Both output formats:** HDF5 and binary chunking each validated (naming, headers, master/plotting).
- **Empty ranks:** `NTask > nchunks` runs cleanly (no `Ntrees<=0` fatal).
- **Master scalability:** many-chunk master build is one open per chunk, not `O(NOUT × nchunks)`.
- **Forest-index precondition (Slices 5–6 window):** until Slice 7 lands, the existing `totnforests >= INT_MAX` FATAL still fires with a clear message (chunking does not silently bypass it); a mocked over-limit metadata case is covered so the limitation is visible to anyone attempting full Uchuu after Slices 5/6 but before Slice 7 lifts it.
- **Per-chunk halo-count guard:** no chunk emits `> INT_MAX` halos in any output snapshot (forced by a too-large `forests_per_file` with monster forests), validated with a mocked oversized-forest case.
- **Load balance:** spiky synthetic distribution within the `4/3` LPT bound; micro-uchuu-hdf5 per-task halo spread no worse than today's weighted split.

## Open decision to confirm during implementation

- **LPT (scattered chunks, best balance — default) vs contiguous-cost-balanced spans (today's I/O locality).** Plan defaults to **LPT** per your instruction, with chunk ids kept contiguous and processed ascending per rank so naming/logging stay deterministic. If Uchuu profiling shows the extra `File%d/Forests` group-opens hurt, a contiguous-cost-balanced assignment is a drop-in alternative behind the Slice 1 assignment hook — not a separate slice unless profiling demands it.

---

## Next Chat Prompt (Mode A — Assisted)

```md
Plan file: docs/dev/chunked-output-plan.md
Slices this session: Slice 1 (then stop for checkpoint)

Read the full plan file first. If a selected slice receipt is incomplete or the plan state is unclear, stop and tell me before coding.

Work on the current feature branch for this plan; if none exists, create one and tell me the name.

Use ai-orchestrator as the controlling skill. Keep the implementation local; delegate per that skill's guidance when independence or context economy helps — primarily hostile drift-audit, independent code-review, and long-running tests.

For each selected slice, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. If the slice's Risk Flags mark approval-needed, stop and get my approval before coding.
3. Apply scoped-implementation against the slice contract.
4. Apply drift-audit. Report the authorization gate result before any quality review.
5. If the gate passes, apply code-review. If it fails, fix the drift and re-audit.
6. Surface drift and review findings to me, fix them, then re-run the relevant gate.
7. Ask me before committing. On my approval, commit that slice with the commit skill.

After the selected slice(s) are committed, use handoff to record state and the next slice to resume from. Do not continue past the selected slice(s).

Confirm before starting: plan file read, selected slice(s), branch, and the first slice.
```

> Nine slices. Slices 2–7 are **approval-needed** (public config + wide-int parsing + metadata schema; shared API + driver + MPI; data correctness; galaxy-ID identity; new fixtures; scale/streaming + encoding-limit guard). Run them one at a time in Mode A with a checkpoint between each. Slices 8–9 may batch if their tests are green. Slice 6 has a **blocking precursor**: create a ctrees ASCII fixture first. **Slice 7 is the scalability slice** that lifts the 32-bit forest-count limit (full Uchuu/Shin-Uchuu); it depends on Slices 5–6.
