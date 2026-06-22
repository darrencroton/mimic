# Mimic HDF5 I/O Optimisation — Implementation Record

**Status:** Implemented (Slices 1–3) on `feature/hdf5-io-optimisation`.
**Date:** 2026-06-22
**Implementation plan:** `docs/dev/MIMIC-HDF5-IO-OPTIMISATION-PLAN.md`
**Scope:** Read performance of the Consistent-Trees forests-HDF5 reader (`src/io/tree/read_ctrees_hdf5.c`), with secondary notes on the L-Halo HDF5 reader and the HDF5 output writer. Anchored to `docs/VISION.md` §5 (bounded memory), §6 (format-agnostic I/O and reproducible output), and §7 (validation and fast failure).

---

## Executive summary

The forests-HDF5 reader was many times slower than the L-Halo *binary* reader on the same trees, and the cause was **not** the partition model — it was the reader's I/O pattern. For every forest it reopened each halo-field dataset from scratch (`H5Dopen → H5Dget_space → H5Dget_type → hyperslab → H5Dread → close ×N`), ~19 times per forest. On a micro-Uchuu-scale workload that meant millions of dataset open/close cycles.

A direct micro-benchmark (below) shows that **keeping the field dataset handles open across forests within a file is ~18× faster**, and reading each field's contiguous slab once and slicing in memory is **~48× faster**, with no change to output values.

The on-disk organisation (SOA, contiguous, field datasets shared across all forests in a file) is already well suited to fast reading; before this work the reader simply did not exploit it. sage-model has the identical per-forest reopen pattern, so there was no faster recipe to copy from sage — this was a genuine improvement opportunity beyond the upstream code.

**Implemented outcome:** the reader now uses a task-range `ForestInfo` cache, persistent per-file field dataset handles, and a bounded bulk-slab read window. The window is fixed at `CTREES_READ_WINDOW_BYTES = 128 MiB` per rank, with forests larger than the window read through the cached-handle direct path. Slice validation showed byte-identical output on the ctrees fixtures and a debug smoke run with no allocator leaks; no NT scale timing was gathered during the implementation slices.

---

## Evidence

Micro-benchmark via h5py (same libhdf5 underneath; dataset reopen maps to `H5Dopen`). Synthetic forests-HDF5 file matching the real layout: 80,000 forests, 2,479,226 halos, 19 fields per halo, contiguous datasets, ~380 MB.

| Strategy | Time | Throughput | vs current |
|---|---|---|---|
| **A — current:** reopen every dataset, per field, per forest | 61.6 s | 1,298 forests/s | 1× |
| **B — persistent handles:** open field datasets once per file, per-forest hyperslab read | 3.45 s | 23,168 forests/s | **~18×** |
| **C — bulk slab:** one read per field across the contiguous forest range, slice in memory | 1.29 s | 61,984 forests/s | **~48×** |

The absolute numbers are h5py, not the C reader, but the *ratios* are representative: the C reader pays the same per-field `H5Dopen`/`H5Dget_space`/`H5Dget_type`/`H5Sselect_hyperslab`/close cost per forest that strategy A pays here.

---

## Original root cause

Before the optimisation, `read_contiguous_forest_ctrees_h5()` was called once per forest and, through the `CT_READ_FOREST_ARRAY` macro (`ct_read_forest_array()`), for **each** of the ~19 fields it:

1. `H5Dopen2` the field dataset,
2. `H5Dget_space` + validate extent,
3. `H5Sselect_hyperslab`,
4. `H5Screate_simple` (memspace),
5. `H5Dget_type` + size check,
6. `H5Dread`,
7. closes the datatype, memspace, filespace, dataset.

Steps 1, 2, 5 and the closes are **invariant across forests** — the dataset, its filespace extent, and its datatype do not change within a file. Only the hyperslab offset/count and the destination buffer change per forest. Repeating the invariant work per forest is the entire overhead.

Separately, `load_unit_ctrees_hdf5()` read the forest's `ForestInfo` row with a **per-forest** `H5Dopen2 + hyperslab + H5Dread + close` on the `ForestInfo` dataset. `ForestInfo` is small and was already read up front for the weighted distribution path, making it a good target for once-per-file caching.

`sage-code/sage-model/io/read_tree_consistentrees_hdf5.c` uses the same `READ_PARTIAL_FOREST_ARRAY` macro per field per forest — i.e. **sage shares the anti-pattern**. sage's `buffered_io.c` is a user-space write buffer for *binary* output (`save_gals_binary.c`) only; it is not an HDF5 read technique. So there is no sage recipe to import here.

---

## Implemented read path

### Slice 1 — Task-range `ForestInfo` cache

`ForestInfo` is read once per task-range file during `setup_forests_io_ctrees_hdf5()` and stored as `CTH.forestinfo_cache[filenum][row]`. `load_unit_ctrees_hdf5()` now obtains each forest's `(ForestHalosOffset, ForestNhalos)` from that cache rather than reopening the `ForestInfo` dataset per forest. The global weighted-distribution pass still reads the minimal `ForestNhalos` vector separately because it runs before the task file range is known.

### Slice 2 — Persistent per-file field handles

The reader now defines a single field table for the 18 fixed fields plus the runtime-detected snapshot field and opens each `Forests/<field>` dataset once per task-range file. Each cached field handle stores the dataset, filespace, datatype, element size, and extent. Schema-level extent/type/size validation happens when the cache opens, so malformed files fail before the forest loop. The per-forest direct read primitive remains live for forests larger than the read window.

### Slice 3 — Bounded bulk-slab read window

Normal forests are served from a bounded SOA read window. The window buffer is allocated as `CTREES_H5_FIELD_COUNT × capacity_halos × 8` bytes in `MEM_IO`, where `capacity_halos = CTREES_READ_WINDOW_BYTES / (CTREES_H5_FIELD_COUNT × 8)`. With the current 19-field table and a 128 MiB per-rank budget, the window holds roughly 883k halo rows per field. When the requested forest is outside the current window, the reader refills by issuing one `H5Dread` per field over the next contiguous halo slab, then assigns the forest's subrange from memory. Per-forest link validation, snapshot validation, and `apply_ctrees_value_conventions()` still run per forest.

Forests larger than `capacity_halos` use the cached-handle direct primitive rather than the window, preserving the hard window bound. This is not a compatibility fallback; it is the required bounded-memory branch for unusually large forests that must be materialised as `halo_data` anyway.

### Implementation constants and memory

- `CTREES_READ_WINDOW_BYTES`: fixed 128 MiB per rank.
- Window storage: `MEM_IO`, freed in `close_partition_ctrees_hdf5()`.
- Task-range `ForestInfo` cache: `MEM_IO`, 32 bytes per forest row in the task's files.
- Field handle cache: fixed HDF5 dataset/filespace/datatype handles per field per task-range file.
- Giant-forest path: reuses the cached-handle direct read and does not enlarge the persistent window.

---

## On-disk organisation — unchanged

The forests-HDF5 SOA layout (one contiguous dataset per field, shared across all forests in a file, `contiguous-halo-props == 1`) is already near-optimal for slab reads: contiguous storage means each hyperslab is a direct seek+read with no chunk-cache involvement. **Do not chunk or compress the tree *input*** for read speed — chunking would add cache pressure and decompression cost to what are currently direct reads. No on-disk format change is recommended; the fix is entirely in the reader.

---

## Write path — already optimised, not a priority

`src/io/output/hdf5.c` already does the right things:

- one chunked `H5TBmake_table` per snapshot (chunk = 1000 records),
- a cross-tree append buffer of `HDF5_WRITE_BUFFER_RECORDS = 8192` records per snapshot (`save_halos_hdf5` → `write_hdf5_halo_batch`), flushing on fill or end-of-file, which drops appends from `O(Ntrees × NOUT)` to `O(total_records / 8192)`,
- the file handle held open across the partition (`HDF5_current_file_id`).

This is the HDF5 equivalent of sage's binary `buffered_io`. The only future tunables worth noting (not blockers): the table `chunk_size` (1000) and the append buffer (8192) could be made parameter-file-configurable for HPC filesystems (Lustre/NFS), and compression remains opt-in via `--compress`. No write-path change is recommended for v1.0.

---

## L-Halo HDF5 reader — separate, lower-priority

`src/io/tree/hdf5.c` (`load_unit_hdf5`) reopens each field via `read_dataset` per tree, but the L-Halo HDF5 layout stores **per-tree groups** (`tree_NNN/<field>`), so the field datasets differ per tree and handles cannot be cached across units the way the shared `Forests/<field>` datasets can. The achievable win is therefore smaller and bounded by the per-tree-group layout. It is a real but secondary item; the forests-HDF5 reader is where the leverage is, and it is the format that matters for the Uchuu suite.

---

## Vision alignment

- **§6 Format-agnostic I/O / reproducible output:** pure performance change behind the existing reader seam; output schema, values, and IDs are untouched. Strongly aligned.
- **§5 Bounded memory:** persistent memory is bounded by task-range metadata plus one fixed 128 MiB per-rank read window; no whole-file slab is retained.
- **§7 Validation and fast failure:** per-field extent/type/size checks now happen at field-cache open time, so malformed/schema-mismatched files fail before the forest loop, not mid-stream. Net improvement.
- **Determinism:** unchanged; the read order and values are identical, so cross-format identity gates still hold.

---

## Implementation surface

| File | Change |
|---|---|
| `src/io/tree/read_ctrees_hdf5.c` | Added task-range `ForestInfo` cache, per-file field handle cache, fixed 128 MiB per-rank read window, direct giant-forest branch, and cache/window cleanup. |
| `src/io/tree/read_ctrees_hdf5.h` | Exposed narrow `MIMIC_TEST_BUILD` hooks for cache/window tests. |
| `tests/unit/test_ctrees_hdf5_reader.c` | Added coverage for `ForestInfo` cache validation, field-cache schema validation, window refill, and giant-forest direct-path behaviour. |
| `docs/dev/CTREES-UCHUU-VALIDATION.md` | Records the reader description and validation gates, including cached handles and windowed reads. |
| `docs/USER-GUIDE.md`, `docs/DEVELOPER-GUIDE.md` | Added reader-memory notes explaining that the 128 MiB per-rank read window is internal, not a run-YAML option. |
| `.agents/skills/mimic-simulations/SKILL.md`, `.agents/skills/mimic-debug/SKILL.md` | Updated project-local skill guidance for cached handles, the bounded read window, and expected `MEM_IO` debug accounting. |
| External auto-memory entry | Updated the local `mimic-hdf5-io-optimisation` memory record and index to mark the optimisation implemented. |

No generated files, property schemas, output schemas, run YAML parameters, or on-disk tree formats changed.

---

## Validation performed

- **Build/generated:** `make MODEL=halos-only SIMULATION=micro-uchuu-hdf5`, `make MODEL=halos-only SIMULATION=micro-uchuu-hdf5 check-generated`, and `make MODEL=halos-only SIMULATION=micro-uchuu-hdf5 validate-modules` passed after Slice 3.
- **Unit:** `MODEL=halos-only SIMULATION=micro-uchuu-hdf5 tests/unit/run_tests.sh test_ctrees_hdf5_reader` passed with 7 tests, and `MODEL=halos-only SIMULATION=micro-uchuu-hdf5 tests/unit/run_tests.sh test_ctrees_support` passed with 13 tests.
- **Byte-identical output gate:** Snap payload comparison against the Slice 2 baseline commit passed for `micro-uchuu-hdf5` and `uchuu` fixtures after Slice 3.
- **Memory/debug:** `./mimic --debug build/generated/test_inputs/halos-only/micro-uchuu-hdf5/simulations/micro-uchuu-hdf5/test_hdf5.yaml` passed with no allocator leaks and peak memory about 129.23 MB, consistent with the 128 MiB read window plus normal run overhead.
- **Scale check:** not gathered during the implementation slices. The benchmark ratios above remain the planning evidence for the expected read-path speedup; a production NT timing can be added later if needed.

---

## Appendix — benchmark reproduction

The numbers above were produced with a self-contained h5py script that builds a synthetic forests-HDF5 file (80k forests, 19 fields, contiguous datasets) and times: (A) reopening every dataset per field per forest, (B) opening field datasets once and doing per-forest hyperslab reads, (C) one bulk read per field then in-memory slicing. Run under `mimic_venv`. The script is short enough to re-derive from this section; it is not committed to avoid carrying a one-off benchmark in the tree.
