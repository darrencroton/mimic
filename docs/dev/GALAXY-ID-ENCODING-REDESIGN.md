# UniqueGalaxyID Encoding Redesign

**Status:** Implemented for v1.0 on the `feature/galaxy-id-encoding` branch.
**Date:** 2026-06-21; implementation notes updated 2026-06-22.
**Context:** Triggered by running one treefile of the full Uchuu simulation. Covers both Consistent-Trees HDF5/ASCII and L-Halo binary readers. Required before any production full-Uchuu run.

---

## Table of Contents

1. [Triggering Incident](#triggering-incident)
2. [Previous Encoding Scheme](#previous-encoding-scheme)
3. [Root Cause Analysis](#root-cause-analysis)
4. [Both Partition Models Are Broken at Scale](#both-partition-models-are-broken-at-scale)
5. [Scaling Context: Full Uchuu and Shin-Uchuu](#scaling-context-full-uchuu-and-shin-uchuu)
6. [Implemented Solution: Global Forest Index](#implemented-solution-global-forest-index)
7. [Reader Interface Design](#reader-interface-design)
8. [TREE_MUL_FAC Analysis](#tree_mul_fac-analysis)
9. [Properties of the New Scheme](#properties-of-the-new-scheme)
10. [Baseline and Test Impact](#baseline-and-test-impact)
11. [Implementation Checklist](#implementation-checklist)
12. [Open Questions](#open-questions)

---

## Triggering Incident

Running Mimic against a single treefile of the full Uchuu forests-HDF5 package (`simulations/uchuu/`, `consistent_trees_hdf5` reader) on one MPI task produced the following fatal error at startup, before any tree processing began:

```
Error in file: src/io/tree/read_ctrees_hdf5.c  func: setup_forests_io_ctrees_hdf5  line: 712
Error: task 0 was assigned 1530564 forests, at or above the unique-galaxy-id limit of 1000000;
       run with more MPI tasks
FATAL: Failed to set up the Consistent-Trees HDF5 reader on task 0
```

The error message's suggested workaround — run with more MPI tasks — is not an acceptable production answer. It embeds a parallelisation requirement inside the ID scheme, makes single-task runs and small-cluster runs impossible for large simulations, and will recur for any sufficiently large dataset regardless of the task count chosen. This document analyses the root cause and proposes a permanent fix.

Observed data point: `mergertree_0.h5` (the first treefile, 44 GB apparent size) contains 1,530,564 forests. The full Uchuu merger-tree directory (`/fred/oz214/simulations/uchuu/U2000/mergertree/`) contains 2,002 HDF5 files, which break down as:

- **2,000 data files**: `mergertree_0.h5` through `mergertree_1999.h5` — the actual merger trees to be processed
- **`mergertree_info.h5`**: 110 KB index file (not a tree file; not processed by Mimic)
- **`mergertree_173_gz4.h5`**: 9.9 GB compressed copy of file 173 (a gzip4 repack test artifact, also not a new dataset)

Apparent file sizes (as reported by `ls`, which is what the HDF5 reader processes) run from 39.2 GB to 64.5 GB, with 79% of files in the 40–50 GB range, a mean of ~51 GB, and a total apparent size of ~102 TB. The `du -sh` filesystem report of 37 TB is the compressed disk footprint; the Swinburne GPFS filesystem applies transparent block-level compression (~2.8:1 ratio on this data), so both figures are correct for their respective measures.

Forest count: at 34,786 forests/GB apparent (derived from file 0), the mean-file estimate gives ~1.77 million forests per file and ~3.55 billion total. The prior metadata-based estimate in `CTREES-UCHUU-VALIDATION.md §8f` is 3.22 billion. Given file-size variation and the instruction to err larger, this document uses **~4 billion forests** as the conservative planning figure for the capacity analysis.

---

## Previous Encoding Scheme

Before the v1.0 redesign, `UniqueGalaxyID` was a `long long` (int64_t) assigned per galaxy in `make_unique_galaxy_id()` in `src/core/build_model.c`. It was written to every output record and was intended to be the stable identifier for a galaxy across all output files from a single run. The previous formula was:

```
UniqueGalaxyID = halonr  +  TREE_MUL_FAC × unit  +  file_mul_fac × partition_output_id
```

where:

| Term | Symbol | Value | Meaning |
|---|---|---|---|
| `TREE_MUL_FAC` | 10⁹ | defined in `src/include/constants.h` | stride between forests |
| `FILENR_MUL_FAC` | 10¹⁵ | defined in `src/include/constants.h` | stride between partitions |
| `file_mul_fac` | 10¹⁵ or 10¹⁴ | `FILENR_MUL_FAC` or `/10` when `LastFile ≥ 10000` | partition stride |
| `halonr` | < `TREE_MUL_FAC` | halo index within the current forest | |
| `unit` | local index | forest or tree index within the current partition | |
| `partition_output_id` | file number or task rank | partition identity | |

The two supported partition models applied this formula differently:

**PARTITION_PER_TASK (Consistent-Trees readers):** one partition per MPI task. `unit` = forest index within the task (0-based), `partition_output_id` = `ThisTask`. `file_mul_fac` always equalled `FILENR_MUL_FAC` (10¹⁵). Uniqueness required:
- `halonr < 10⁹` (halos per forest)
- `unit < FILENR_MUL_FAC / TREE_MUL_FAC = 10⁶` (forests per task)
- `ThisTask < LLONG_MAX / FILENR_MUL_FAC ≈ 9,223` (task count)

These bounds were enforced by guards in `src/io/tree/read_ctrees_common.h` (`CTREES_MAX_FORESTS_PER_TASK`, `CTREES_MAX_TASK_ID`), both of which were removed when the task-rank term left the ID scheme.

**PARTITION_PER_FILE (L-Halo binary, L-Halo HDF5):** one partition per input file. `unit` = tree index within the file (0-based), `partition_output_id` = file number. `file_mul_fac` was 10¹⁵ when `LastFile < 10000`, else 10¹⁴. Uniqueness required `unit < file_mul_fac / TREE_MUL_FAC = 10⁶` (trees per file). This bound was not guarded, so the reader could silently produce colliding IDs if it was exceeded.

---

## Root Cause Analysis

The three-term encoding partitions the `int64_t` space (max ≈ 9.22 × 10¹⁸) across three independent dimensions:

```
9 decimal digits (halos/forest)  ×  6 digits (forests/partition)  ×  4 digits (partitions)
        10⁹                      ×          10⁶                   ×        10⁴          = 10¹⁹
```

10¹⁹ slightly exceeds `INT64_MAX` — the scheme is already tight as designed. The critical observation is that the **6-digit forests-per-partition slot** is entirely consumed by the choice of how work is divided across files or tasks. It is an engineering artefact, not a property of the data. Changing MPI task count or converting between formats changes how many forests land on each partition and therefore whether the ID scheme is valid.

This means:
- The same simulation data can be valid or invalid depending on MPI configuration or file format.
- IDs are not reproducible across MPI configurations: the same galaxy on 1 task versus 2 tasks has different IDs.
- The useful physical content of the 6-digit slot (the forest's identity within the run) is squandered on a partition boundary.

---

## Both Partition Models Are Broken at Scale

### PARTITION_PER_TASK — explicit crash

Full Uchuu: approximately 3.5–4 billion forests across 2,000 files (see §5 for the estimate derivation). On a single MPI task, `unit` for the very first file already reaches 1,530,564 > `CTREES_MAX_FORESTS_PER_TASK = 10⁶`. The guard fires and Mimic aborts. With 2 tasks, each task still gets ≈ 1.53 million forests — still above 10⁶. Under the previous scheme, a minimum of ~3,200 tasks was needed, with a maximum of ~9,000 before the task-ID term overflowed int64. This was a narrow and fragile window that made serial validation impossible and added a hard MPI-count dependency to what should be a data property.

### PARTITION_PER_FILE — silent ID collision

If full Uchuu trees were converted to L-Halo binary at the same file granularity (1.5M trees per file), the L-Halo binary reader would silently produce corrupt output. With `FILENR_MUL_FAC = 10¹⁵` and `TREE_MUL_FAC = 10⁹`:

```
tree_mul for unit 1,000,000 = 10⁹ × 10⁶ = 10¹⁵ = FILENR_MUL_FAC
```

Forest 1,000,000 in file 0 and forest 0 in file 1 get the same ID. There is no guard. The output is wrong and silent.

---

## Scaling Context: Full Uchuu and Shin-Uchuu

Two simulations motivate the redesign's requirements:

**Full Uchuu (`simulations/uchuu/`):**
- 2,000 data files (`mergertree_0.h5` – `mergertree_1999.h5`), forests-HDF5 format
- Apparent file sizes: 39.2–64.5 GB, mean ~51 GB, total ~102 TB apparent; compressed disk footprint is 37 TB (GPFS transparent compression — both figures are correct)
- Conservative forest count estimate: **~4 billion** (file-size scaling gives ~3.55B; metadata estimate from `CTREES-UCHUU-VALIDATION.md §8f` is 3.22B; we use the larger figure for capacity planning)
- Forest sizes are predominantly small-to-moderate; even the most massive cluster merger tree across 50 snapshots is unlikely to have more than ~10⁶ halos
- L-Halo binary conversion is being considered; the fix must work identically for both formats

**Shin-Uchuu (planned import):**
- Far fewer total forests than full Uchuu
- Orders of magnitude more halos per forest: Shin-Uchuu resolves significantly more substructure per merger tree
- The exact per-forest size is not yet confirmed; the TREE_MUL_FAC bound must be validated against actual data before finalising implementation (see §8 and §12)

The two simulations stress the encoding in orthogonal directions: full Uchuu stresses total forest count; Shin-Uchuu stresses per-forest halo count. A correct fix must handle both independently.

---

## Implemented Solution: Global Forest Index

The implemented fix removes partition identity from the encoding entirely. The active formula is:

```
UniqueGalaxyID  =  halonr  +  TREE_MUL_FAC × forestnr_global
```

`forestnr_global` is the **0-based index of this forest in the globally sorted list across the entire run**, independent of how forests are divided across files or MPI tasks. The three-term encoding becomes a two-term encoding: halo-within-forest and forest-in-run.

### Mathematical analysis

With TREE_MUL_FAC = 10⁹ (unchanged):

```
max UniqueGalaxyID  ≈  total_forests × TREE_MUL_FAC  +  (TREE_MUL_FAC - 1)
```

The scheme is governed by a single product constraint:

```
total_forests_global × TREE_MUL_FAC  <=  INT64_MAX  (≈ 9.22 × 10¹⁸)
```

Full Uchuu and Shin-Uchuu are concrete reference points for the scale the scheme must handle; they are not the design ceiling.

| Case | Total forests | Max halos/forest | Max ID | vs INT64_MAX (9.22 × 10¹⁸) |
|---|---|---|---|---|
| Full Uchuu (conservative est.) | ~4 × 10⁹ | ~10⁶ | ≈ 4 × 10¹⁸ | 2.3× |
| Shin-Uchuu | unknown until import | unknown until import | measure before import | open |
| 2× full-Uchuu-scale simulation | ~8 × 10⁹ | ~10⁶ | ≈ 8 × 10¹⁸ | 1.2× |
| Scheme hard limit | 9.22 × 10⁹ | 10⁹ | = INT64_MAX | at limit |

The hard limit is 9.22 billion total forests (at TREE_MUL_FAC = 10⁹). Full Uchuu sits at roughly 4 billion, leaving 2.3× headroom. Shin-Uchuu remains an open measurement question because its maximum per-forest halo count is not yet known.

This constraint is checked at startup as a fast-fail guard before any processing begins. It replaces the previous per-task and per-file partition-stride guards with a single principled capacity check, while per-forest halo counts are still validated against `TREE_MUL_FAC`.

### Comparison with previous scheme

| Property | Previous (3-term) | Active (2-term global) |
|---|---|---|
| Full Uchuu serial run (reference) | **CRASH** (forests/task > 10⁶) | Max ID ≈ 4 × 10¹⁸ ✓ |
| Full Uchuu L-Halo binary (reference) | **Silent collision** (trees/file > 10⁶) | Max ID ≈ 4 × 10¹⁸ ✓ |
| Shin-Uchuu (reference) | Works today | Requires measured per-forest halo count before import |
| Scheme ceiling (total forests) | 10⁶ per partition | **9.22 billion total** |
| IDs reproducible across MPI configs | **No** | **Yes** |
| IDs stable across format conversion | **No** (different formula paths) | Yes (same formula) |
| Partition identity in ID | Yes (task rank or file number) | **No** |
| `FILENR_MUL_FAC` needed for IDs | Yes | **No** |
| Hard per-partition forest limit | 10⁶ | None (product guard only) |
| `make_unique_galaxy_id` complexity | 6 lines, 2 branches | 1 line |

---

## Reader Interface Design

### Core concept

Each reader, after setting up its partition, exposes the global offset of that partition's first forest. `make_unique_galaxy_id` then computes:

```
forestnr_global = GlobalForestOffset + unit
```

`GlobalForestOffset` is a global variable declared alongside `Ntrees`, `ThisTask`, `NTask`, etc. (in `src/include/globals.h`). Every reader sets it before the driver calls `build_halo_tree`.

### Runtime state

```c
/* src/include/globals.h */
int64_t GlobalForestOffset;  /* 0-based global index of current partition's first forest */
```

### Reader interface field

One function pointer was added to `struct TreeReader` (`src/io/tree/reader.h`), required for PARTITION_PER_FILE readers only:

```c
/* PARTITION_PER_FILE only (NULL for PARTITION_PER_TASK readers).
   Returns the number of trees/forests in partition `output_id`, reading
   only the file header without allocating per-unit halo arrays. Used by
   the driver to build the global forest offset prefix-sum table at startup. */
int64_t (*count_partition_trees)(int output_id);
```

### PARTITION_PER_TASK readers (Consistent-Trees HDF5 and ASCII)

No new function pointer is needed. The `start_forestnum` variable computed during `setup_forests_io_ctrees_hdf5` / the ASCII equivalent is already the correct global offset: because both readers compute the global-sorted position of their task's first forest during the forest-distribution step, `start_forestnum` is already `forestnr_global` for `unit = 0`.

Implemented behaviour in each ctrees reader:
1. `start_forestnum` is retained in the reader's state struct (`CTH.start_forestnum` for HDF5, `CT.start_forestnum` for ASCII).
2. After the forest-distribution calculation, the reader sets `GlobalForestOffset = CTH.start_forestnum` (or `CT.start_forestnum`).
3. The previous `CTREES_MAX_FORESTS_PER_TASK` and `CTREES_MAX_TASK_ID` guards are removed; the shared total-forest capacity helper replaces the partition-stride checks.

### PARTITION_PER_FILE readers (L-Halo binary, L-Halo HDF5)

Each reader implements `count_partition_trees(output_id)`: open the file for the given output id, read the tree count from the header (one integer, already read by `open_partition`), close the file, return the count. This is a fast, allocation-free operation.

The driver (`src/core/main.c`) performs a one-time prefix-sum scan before the partition loop:

```c
/* Build global forest offset table (PARTITION_PER_FILE only). */
int npartitions = reader->num_partitions();
int64_t *g_forest_offsets = mymalloc_cat(npartitions * sizeof(*g_forest_offsets), MEM_IO);
int64_t running = 0;
for (int p = 0; p < npartitions; p++) {
    int output_id = reader->partition_output_id(p);
    g_forest_offsets[p] = running;
    running += reader->count_partition_trees(output_id);
}
int64_t totnforests_global = running;
XRETURN(totnforests_global <= LLONG_MAX / TREE_MUL_FAC, FAILURE,
        "Total forest count %" PRId64 " would overflow UniqueGalaxyID encoding "
        "(limit: %" PRId64 " forests for TREE_MUL_FAC=%lld)",
        totnforests_global, (int64_t)(LLONG_MAX / TREE_MUL_FAC), (long long)TREE_MUL_FAC);
```

Before each `open_partition(output_id)` call in the partition loop:
```c
GlobalForestOffset = g_forest_offsets[partition_idx];
```

In MPI runs, all tasks independently compute the identical prefix sum from the shared filesystem — no inter-task communication is required.

### `make_unique_galaxy_id` after the change

```c
static int64_t make_unique_galaxy_id(int halonr, int unit) {
    int64_t forestnr_global = GlobalForestOffset + (int64_t)unit;
    return mimic_encode_unique_galaxy_id((int64_t)halonr, forestnr_global);
}
```

The `partition_output_id` parameter is removed. The `file_mul_fac` branch and the `FILENR_MUL_FAC`/`LastFile` logic are removed. Call sites (`join_progenitor_halos`, `build_halo_tree`) drop the `partition_output_id` argument.

### Total partition-scan cost for L-Halo binary

For 2,000 L-Halo binary files: reading one integer from each file header = negligible I/O. Files will already be in OS page cache by the time the main partition loop begins. For the forests-HDF5 reader (PARTITION_PER_TASK), there is no scan: `start_forestnum` is already computed during the existing setup.

---

## TREE_MUL_FAC Analysis

`TREE_MUL_FAC = 10⁹` is defined in `src/include/constants.h` and sets the maximum number of halos in a single forest. With the implemented two-term scheme, it is the only multiplier governing the encoding.

### Full Uchuu

Full Uchuu forest sizes are expected to be modest — the simulation has 12,800³ particles in a 2 Gpc/h box. Even the most massive cluster merger tree across 50 snapshots, including all substructure, is unlikely to approach 10⁶ halos, let alone 10⁹. The existing per-forest guard at `read_ctrees_hdf5.c:189` (`nhalos < TREE_MUL_FAC`) will catch any violation during processing.

### Shin-Uchuu

Shin-Uchuu is expected to have more halos per forest than full Uchuu, driven by resolving more substructure at higher resolution. The precise maximum is unknown until the data is in hand, so measure the actual maximum per-forest halo count before changing the multiplier.

### The fundamental constraint

With either choice of `TREE_MUL_FAC`, the encoding validity reduces to a single product:

```
max_halos_per_forest  ×  total_forests  <  INT64_MAX  (≈ 9.22 × 10¹⁸)
```

This product captures a genuine physical trade-off: volume-complete simulations with many millions of small haloes produce many small merger trees (many forests, small individual sizes); high-resolution resimulations or zoom-ins produce few but extremely rich trees (few forests, large individual sizes). A simulation that simultaneously had billions of forests AND billion-halo individual trees would require a simulation volume and resolution far beyond what is planned for the foreseeable future. The scheme is thus practically unlimited within its `int64_t` type, with `TREE_MUL_FAC` as the single tunable knob that trades per-forest budget against total-forest capacity.

---

## Properties of the New Scheme

**Reader-agnostic:** The formula `halonr + TREE_MUL_FAC × forestnr_global` is identical for all readers. Converting full Uchuu from forests-HDF5 to L-Halo binary produces the same `UniqueGalaxyID` values for every galaxy, provided the global forest ordering is consistent between formats. This directly supports VISION.md §6 (format-agnostic I/O and reproducible output).

**MPI-config reproducible:** Galaxy IDs no longer depend on `ThisTask` or `NTask`. The same galaxy in the same simulation gets the same ID in a serial run, a 4-task run, or a 3,000-task run. This is a strict correctness improvement over the previous scheme. VISION.md §6 requires outputs to carry enough metadata to interpret a run without external information; MPI-dependent IDs violated this in subtle ways.

**No partition-count limit:** The per-task forest limit (10⁶) and the per-file tree limit (10⁶) disappear entirely. The only limit is the total-forest product guard checked at startup.

**Fast failure:** The global guard runs before any forest is loaded. If total_forests × TREE_MUL_FAC would overflow, Mimic fails immediately with a specific count and the encoding capacity, not deep into a multi-hour run.

**Minimal interface surface:** One new reader function pointer (`count_partition_trees` for PARTITION_PER_FILE), one new global (`GlobalForestOffset`), one simplified function (`make_unique_galaxy_id`), one pre-scan loop in the driver (PARTITION_PER_FILE only). The ctrees reader changes are limited to promoting a local variable and setting the global.

**`FILENR_MUL_FAC` removed from the ID scheme:** The constant has no active v1.0 runtime purpose after the two-term formula and was removed rather than retained as compatibility scaffolding. The `file_mul_fac` / `LastFile >= 10000` branch in `make_unique_galaxy_id` is deleted.

---

## Baseline and Test Impact

**Ctrees readers (PARTITION_PER_TASK):** UniqueGalaxyIDs change. There are no committed ctrees byte-identical baselines (ctrees IDs were already MPI-task-count dependent, so no stable baseline existed). Integration and scientific tests that use ctrees fixtures will need to be re-run after the change to confirm correctness, but no committed baseline binary needs updating.

**L-Halo binary and L-Halo HDF5 readers (PARTITION_PER_FILE):** UniqueGalaxyIDs change for multi-file runs because file number is no longer part of the ID. The committed mini-Millennium file-0 data values remain byte-identical, but schema metadata and HDF5 field metadata must be refreshed so committed artifacts describe the active formula. Process used for the v1.0 refresh:

1. Implement and verify the new scheme on a clean build.
2. Run the default binary/HDF5 output-format tests to regenerate current `tests/data/output/{binary,hdf5}` artifacts.
3. Replace committed baseline schemas under `tests/data/output/baseline/`, and replace HDF5 baseline files when `FieldMetadata` changes.
4. Regenerate the SAGE physics binary output and refresh its committed `metadata/output_schema.json`.
5. Confirm the scientific and integration gates pass on the refreshed baselines.

The byte-identical test logic is unchanged. Data values change only when the selected fixture actually exercises a non-zero global forest offset.

---

## Implementation Checklist

This is the implementation record for the sequenced work.

- [x] **Add `GlobalForestOffset` to `src/include/globals.h`** (declare `extern int64_t GlobalForestOffset` alongside `Ntrees`, `ThisTask`)
- [x] **Initialise `GlobalForestOffset = 0` in runtime state** (defined in `src/core/allvars.c` and assigned per partition before processing)
- [x] **Add `count_partition_trees` to `struct TreeReader` in `src/io/tree/reader.h`** (set to NULL in PARTITION_PER_TASK reader registrations; mandatory for PARTITION_PER_FILE readers)
- [x] **Implement `count_partition_trees` in `src/io/tree/binary.c` and `src/io/tree/hdf5.c`** (open file, read tree count from metadata/header, close file, return count without staging halo arrays)
- [x] **Add prefix-sum scan to the PARTITION_PER_FILE path in `src/core/main.c`** with startup capacity and accumulation guards; set `GlobalForestOffset = global_forest_offsets[partition_idx]` before each `open_partition()` call
- [x] **Publish ctrees global starts** from both ctrees readers (`CTH.start_forestnum` for HDF5, `CT.start_forestnum` for ASCII); set `GlobalForestOffset` and add matching total-forest capacity checks after the forest-distribution calculation inside `open_partition`
- [x] **Simplify `make_unique_galaxy_id` in `src/core/build_model.c`** to the two-term formula; remove `partition_output_id` parameter; update all call sites
- [x] **Remove `CTREES_MAX_FORESTS_PER_TASK` guard** from both ctrees readers (replaced by the startup product guard)
- [x] **Remove the `CTREES_MAX_TASK_ID` guard** from both ctrees readers (task-rank term no longer exists in the ID)
- [x] **Remove the `CTREES_MAX_FORESTS_PER_TASK` and `CTREES_MAX_TASK_ID` defines in `src/io/tree/read_ctrees_common.h`**
- [x] **Remove `FILENR_MUL_FAC` from `src/include/constants.h`** because it no longer has an active runtime purpose
- [x] **Update `docs/dev/CTREES-UCHUU-VALIDATION.md §8f`** to replace the old MPI identity constraint section with the new run-scoped formula
- [x] **Update `docs/DEVELOPER-GUIDE.md`** unique galaxy ids and reader-interface sections
- [x] **Regenerate committed mini-Millennium baseline metadata/HDF5 field metadata** for the active formula

---

## Open Questions

**1. Maximum halos per forest in Shin-Uchuu.** This is the key unknown. Before the Shin-Uchuu package is imported, run a scan of the raw Shin-Uchuu forests files to measure the actual maximum per-forest halo count, then evaluate whether the current `TREE_MUL_FAC` remains appropriate.

**2. Global forest ordering under weighted distribution.** The ctrees HDF5 reader supports weighted forest distribution across tasks (`forest_distribution_scheme: linear` in the Uchuu run YAML). This changes which forests land on which task but not the global sorted order (`start_forestnum` is already computed in the global-sorted frame for all distribution schemes). Confirm that `start_forestnum` is stable under both `uniform_in_forests` and `linear` distribution before closing the implementation.

**3. Cross-run identity contract.** The dual-driver plan (`MIMIC-DUAL-DRIVER-PLAN.md §Phase 5`) notes that a future snapshot driver must reproduce the same per-galaxy identity as the tree driver. With the two-term global-forest-index scheme, identity is fully determined by `(forestnr_global, halonr)` — a property of the data, not of the driver. This makes the Phase 5 cross-format identity contract significantly simpler to specify.

**4. Total forest count for full Uchuu.** The figure 3.22 billion forests (from `CTREES-UCHUU-VALIDATION.md §8f`) is a pre-run estimate. The actual total will be confirmed on the first full-run attempt once the ID encoding is fixed. The startup bound guard will print the measured total at run time; log it and update this document.
