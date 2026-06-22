# Mimic HDF5 I/O Optimisation — Findings and Recommendations

**Status:** Recommendations for team review (pre-v1.0). Not yet implemented.
**Date:** 2026-06-22
**Scope:** Read performance of the Consistent-Trees forests-HDF5 reader (`src/io/tree/read_ctrees_hdf5.c`), with secondary notes on the L-Halo HDF5 reader and the HDF5 output writer. Anchored to `docs/VISION.md` §5 (bounded memory), §6 (format-agnostic I/O and reproducible output), and §7 (validation and fast failure).

---

## Executive summary

The forests-HDF5 reader is many times slower than the L-Halo *binary* reader on the same trees, and the cause is **not** the partition model — it is the reader's I/O pattern. For every forest it reopens each halo-field dataset from scratch (`H5Dopen → H5Dget_space → H5Dget_type → hyperslab → H5Dread → close ×N`), ~19 times per forest. On a micro-Uchuu-scale workload that is millions of dataset open/close cycles.

A direct micro-benchmark (below) shows that **keeping the field dataset handles open across forests within a file is ~18× faster**, and reading each field's contiguous slab once and slicing in memory is **~48× faster**, with no change to output values.

The on-disk organisation (SOA, contiguous, field datasets shared across all forests in a file) is already well suited to fast reading; the reader simply does not exploit it. sage-model has the identical per-forest reopen pattern, so there is no faster recipe to copy from sage — this is a genuine improvement opportunity beyond the upstream code.

**Headline recommendation:** adopt persistent per-file dataset handles (Tier 1) as the v1.0 read-path fix. It is low-risk, byte-identical, and captures the bulk of the win. Treat bulk-slab reads (Tier 2) as an optional follow-on with a bounded buffer.

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

## Root cause

In `read_ctrees_hdf5.c`, `read_contiguous_forest_ctrees_h5()` is called once per forest and, through the `CT_READ_FOREST_ARRAY` macro (`ct_read_forest_array()`), for **each** of the ~19 fields it:

1. `H5Dopen2` the field dataset,
2. `H5Dget_space` + validate extent,
3. `H5Sselect_hyperslab`,
4. `H5Screate_simple` (memspace),
5. `H5Dget_type` + size check,
6. `H5Dread`,
7. closes the datatype, memspace, filespace, dataset.

Steps 1, 2, 5 and the closes are **invariant across forests** — the dataset, its filespace extent, and its datatype do not change within a file. Only the hyperslab offset/count and the destination buffer change per forest. Repeating the invariant work per forest is the entire overhead.

Separately, `load_unit_ctrees_hdf5()` reads the forest's `ForestInfo` row with a **per-forest** `H5Dopen2 + hyperslab + H5Dread + close` on the `ForestInfo` dataset. `ForestInfo` is small and read entirely up front for the weighted distribution path anyway; it should be read once per file and indexed in memory.

`sage-code/sage-model/io/read_tree_consistentrees_hdf5.c` uses the same `READ_PARTIAL_FOREST_ARRAY` macro per field per forest — i.e. **sage shares the anti-pattern**. sage's `buffered_io.c` is a user-space write buffer for *binary* output (`save_gals_binary.c`) only; it is not an HDF5 read technique. So there is no sage recipe to import here.

---

## Recommendations

### Tier 1 — Persistent per-file dataset handles (recommended for v1.0)

Open each `Forests/<field>` dataset (and cache its filespace and datatype) **once** when the file's `Forests` group is opened, store the handles in the partition state alongside the existing `h5_forests_group[]`, and in the per-forest read only do `H5Sselect_hyperslab` + `H5Sset_extent`/`H5Screate_simple` (memspace) + `H5Dread`. Read `ForestInfo` once per file into an in-memory array and index it by tree row.

- **Expected gain:** ~18× on the read path (benchmark strategy B).
- **Memory cost:** negligible — a fixed set of open dataset/space/type handles per file in the task's range (the reader already holds one `Forests` group handle per file).
- **Risk:** low. Output values are unchanged (same reads, same conversions). Keep the existing extent/datatype/size validations — perform them once per file at open time instead of once per forest, which also makes a schema mismatch fail *before* the forest loop starts (better fast-failure, §7).
- **Determinism:** unchanged; identical bytes out.

This is the clean, vision-aligned v1.0 fix and should be done regardless of whether Tier 2 follows.

### Tier 2 — Bulk contiguous-slab reads (optional follow-on)

Because each task processes a **contiguous** forest range and forests are stored contiguously by halo offset, a task's halos form one contiguous slab `[start_offset, end_offset)` in every field dataset. Read each field for the whole slab (or for a bounded sub-slab window) in a single `H5Dread`, then hand each forest its sub-range from memory.

- **Expected gain:** a further ~2.7× over Tier 1 (benchmark strategy C; ~48× over current).
- **Memory cost:** must be **bounded** to honour Vision §5. A whole-file slab for full Uchuu (~91 M halos/file × 19 fields × 8 B ≈ 14 GB) is too large to hold at once. Read in fixed-size windows (e.g. a few hundred MB of halos across all fields), refilling as the forest cursor advances. This keeps memory bounded by the window, not by file size.
- **Risk:** medium — introduces a windowing buffer and its accounting (`MEM_IO`/`MEM_TREES`). Justified only if Tier 1 proves insufficient for production full-Uchuu walltime.

### On-disk organisation — keep as-is

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
- **§5 Bounded memory:** Tier 1 adds only a fixed handle set. Tier 2 must use a bounded window, not a whole-file buffer — this is the explicit constraint on Tier 2.
- **§7 Validation and fast failure:** move the per-field extent/type/size checks to file-open time so a malformed/schema-mismatched file fails before the forest loop, not mid-stream. Net improvement.
- **Determinism:** unchanged; the read order and values are identical, so cross-format identity gates still hold.

---

## Files that would be touched (Tier 1)

| File | Change |
|---|---|
| `src/io/tree/read_ctrees_hdf5.c` | Add cached per-file field dataset handles (+ filespace/datatype) to `struct ctrees_hdf5_partition`; open them when each `Forests` group is opened; rewrite `ct_read_forest_array`/`read_contiguous_forest_ctrees_h5` to reuse handles and only reselect the hyperslab; read `ForestInfo` once per file and index in `load_unit`; close handles in `close_partition_ctrees_hdf5`. |
| `tests/unit/test_ctrees_support.c` | Extend the existing forest-read/slab tests to cover the cached-handle path (the `MIMIC_TEST_BUILD` entry points already exercise reads against a fixture). |
| `docs/dev/CTREES-UCHUU-VALIDATION.md` | Note the read-path optimisation in the reader description (§4). |

No generated files, no property/schema changes, no output changes.

---

## Validation plan

- **Byte-identical output gate:** run the local output-format baseline and the ctrees fixtures (`simulations/uchuu/_tests/data/`, `simulations/micro-uchuu-hdf5/_tests/data/`) before/after; require exact identity (Vision baseline contract).
- **Unit:** `tests/unit/run_tests.sh test_ctrees_support` against the bundled fixtures.
- **Scale check (NT, optional):** time a `halos-only` micro-Uchuu-HDF5 pass before/after on `dcroton@nt.swin.edu.au` (the 13 GB / ~440k-forest dataset) to confirm the real-world speedup. Avoid full Uchuu for timing (~1 hr/file).
- **Memory:** confirm `MEM_IO`/`MEM_TREES` accounting and no leaks under a debug run; for Tier 2, confirm peak stays within the configured window.

---

## Appendix — benchmark reproduction

The numbers above were produced with a self-contained h5py script that builds a synthetic forests-HDF5 file (80k forests, 19 fields, contiguous datasets) and times: (A) reopening every dataset per field per forest, (B) opening field datasets once and doing per-forest hyperslab reads, (C) one bulk read per field then in-memory slicing. Run under `mimic_venv`. The script is short enough to re-derive from this section; it is not committed to avoid carrying a one-off benchmark in the tree.
