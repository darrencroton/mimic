# Mimic HDF5 Read-Path Optimisation — Implementation Plan

**Status:** Implemented through Slice 4 on `feature/hdf5-io-optimisation`.
**Date:** 2026-06-22
**Companion findings:** `docs/dev/MIMIC-HDF5-IO-OPTIMISATION.md` (root-cause analysis and benchmark evidence).
**Scope:** Read performance of the Consistent-Trees forests-HDF5 reader (`src/io/tree/read_ctrees_hdf5.c`). Anchored to `docs/VISION.md` §5 (bounded memory), §6 (format-agnostic I/O / reproducible output), §7 (validation and fast failure).

---

## Objective and target

Deliver the **maximum** available read-path speedup, not the conservative one. The findings doc measured ~18× for persistent dataset handles and ~48× for bulk contiguous-slab reads. This plan implements the **bulk-slab end state (~48×)** with a **bounded read window**, building it on top of the persistent-handle work as ordered, independently-shippable slices.

Two project directives shape the plan:

- **Highest gain even with more work.** The destination is Slice 3 (bounded windowed bulk-slab reads). Slices 1–2 are not an alternative deliverable; they are the foundation Slice 3 stands on, and each is byte-identical and shippable on its own.
- **No backward compatibility, no dead/stale code.** This is single-user, active pre-v1.0 development. Each slice *replaces* the prior I/O pattern outright — no parallel "old path" kept as a runtime fallback or config toggle. The one retained per-forest read primitive (giant-forest path in Slice 3) is a live, required bounded-memory branch that reuses the Slice-2 primitive, not a compatibility shim.

---

## Verification of the findings (done during planning)

Confirmed against `src/io/tree/read_ctrees_hdf5.c` and `src/core/main.c` at the current `main` head:

1. **Per-forest dataset reopen is real and slightly *understated* by the findings doc.** Each forest triggers ~**21** dataset open/close cycles, not 19:
   - 19 in `ct_read_forest_array()` (one `H5Dopen2`→`H5Dget_space`→extent-validate→`H5Sselect_hyperslab`→`H5Screate_simple`→`H5Dget_type`→`H5Tget_size`→`H5Dread`→4×close per field), invoked via the `CT_READ_*` macros in `read_contiguous_forest_ctrees_h5()`;
   - **+1** redundant `H5Dopen2("Mvir")` inside `validate_ctrees_hdf5_forest_slab()` (line ~199) — called once per forest from `load_unit`;
   - **+1** `H5Dopen2("ForestInfo")` block in `load_unit_ctrees_hdf5()` (lines ~885–916) — per forest.
2. **`Forests` groups are already held open per file for the partition lifetime** (`open_partition_ctrees_hdf5` → `close_partition_ctrees_hdf5`, stored in `CTH.h5_forests_group[]`). They are the natural anchor for cached field handles; no lifetime change is needed.
3. **`load_unit(unit)` is driven strictly sequentially** — `src/core/main.c:376` `for (unit = 0; unit < Ntrees; unit++) load_unit(unit);`. Forests are loaded in ascending, forward-scan order, so a refill-on-advance read window hits the bulk-read fast path on essentially every forest. **Correctness does not depend on call order** (a window refill always covers at least the requested forest); only the speedup does.
4. **All 19 read fields are 8 bytes on disk** in this layout (doubles for Mvir/x/y/z/vrms/vmax/vx/vy/vz/Jx/Jy/Jz; int64 for the 5 links, `id`, and the snap field whether stored int or float). A window buffer is therefore uniform `19 × capacity × 8` bytes — one allocation, field `F` at `base + F*capacity`.
5. **`ForestInfo` consolidation was available.** At planning time, `read_nhalos_per_forest()` already read `ForestNhalos` globally for the weighted-distribution path, while the per-forest `foresthalosoffset`/`forestnhalos` needed by `load_unit` were not cached and were re-read per forest.
6. **Bounded memory holds.** Cached handles are a fixed small set; the task-range `ForestInfo` cache is 32 B/forest over the task's files; the read window is a single bounded buffer. All are bounded by task scope, satisfying §5.

The on-disk SOA/contiguous layout is already near-optimal for slab reads; **no on-disk format change is in scope.** The entire change lives in the reader.

---

## Repository state

The implementation landed as ordered commits on `feature/hdf5-io-optimisation`: `71547fc9` cached task-range `ForestInfo`, `a9e1aaa2` cached per-file field handles, and `9115bf26` added the bounded read window. Slice 4 closes the documentation and skill sweep. The bundled fixtures `simulations/micro-uchuu-hdf5/_tests/data/` and `simulations/uchuu/_tests/data/` remain the local ctrees safety net for byte-identical output checks.

---

## Design overview (end state)

A single source-of-truth **field table** (the 18 fixed `Forests/<field>` names in read order + the runtime-detected snap field name) drives both cached handles and window slabs. The partition struct `CTH` gains:

- a **per-task-file `ForestInfo` cache** (Slice 1): full 4×int64 rows for files in `[start_filenum, end_filenum]`, indexed by tree row;
- a **per-task-file cached handle set** (Slice 2): for each of the 19 fields, `{dataset, filespace, datatype, extent}`, opened and validated once at file-open time;
- a **bounded read window** (Slice 3): one `19 × capacity × 8 B` buffer plus `{current_file, window_start_halo, window_len}` cursor state.

`load_unit` becomes: look up `(offset, nhalos)` from the cached `ForestInfo` (no HDF5 calls) → validate the slab against the cached extent (integer compare, no HDF5 calls) → if `nhalos ≤ window_capacity` serve from the window (refilling with one `H5Dread` per field when the forest leaves the current window), else read the giant forest directly via the Slice-2 cached-handle primitive. Per-forest link/snap validation and the `apply_ctrees_value_conventions` step are unchanged and still run over each forest's slice.

---

## Slice 1: Cache per-task-file `ForestInfo`; remove the per-forest `ForestInfo` reopen

### Intended Change
- Read each task-range file's full `ForestInfo` (4×int64: `forestid`, `foresthalosoffset`, `forestnhalos`, `forestntrees`) **once** into `CTH`, indexed by tree row, during `setup_forests_io_ctrees_hdf5` (after `start_filenum`/`end_filenum` are known).
- Rewrite `load_unit_ctrees_hdf5` to read `(halosoffset, nhalos)` from the cache instead of the per-forest `H5Dopen2("ForestInfo")`→hyperslab→read→close block (lines ~881–916).
- Keep `read_nhalos_per_forest` for the *global* weighted-distribution pass (different scope: all `[firstfile,lastfile]` before the task range is known; freed immediately after distribution). Add a one-line comment recording the deliberate split: minimal global `ForestNhalos` read for distribution vs full task-range `ForestInfo` cache for loading. Do **not** widen the global read to full `ForestInfo` (would grow a transient global allocation against §5).

### Acceptance Criteria
- Inputs: same forests-HDF5 files and run YAML as today.
- Outputs: byte-identical binary/HDF5 output to pre-change for the ctrees fixtures and the local baseline.
- User-visible behaviour: unchanged output; one `ForestInfo` `H5Dread` per task file at setup instead of one per forest.
- Behaviour that must not change: forest→(file,row) mapping, weighted distribution, all fast-fail validations (negative/oversized nhalos, row-in-range, record-size mismatch). Fast-failure should move *earlier* (cache build validates all rows at setup), never later.

### Authorized Surface
- Files allowed to change: `src/io/tree/read_ctrees_hdf5.c`.
- Functions/structs allowed to change: `struct ctrees_hdf5_partition` (add `ForestInfo` cache members), `setup_forests_io_ctrees_hdf5` (populate cache for the task file range), `load_unit_ctrees_hdf5` (consume cache, delete the inline `ForestInfo` read block), `close_partition_ctrees_hdf5` (free cache), and a new static helper (e.g. `load_forestinfo_cache_ctrees_hdf5`). Reuse `struct ctrees_forestinfo` (already defined).
- Tests allowed/expected to change: `tests/unit/test_ctrees_support.c` and/or the HDF5 reader test (`test_ctrees_hdf5_reader`), plus a new `MIMIC_TEST_BUILD` entry point if needed to exercise the cache path.

### Explicit Non-Goals
- No field-read changes (still per-forest, per-field reopen — that is Slice 2).
- No window/bulk read.
- No change to `read_nhalos_per_forest`'s global pass beyond a clarifying comment.

### Risk Flags
- Risky surfaces touched: global reader state (`CTH`), I/O reader seam, memory accounting (`MEM_IO`). Single-threaded per task; no concurrency.
- Approval needed before implementation: no (byte-identical, contained). Proceed under the baseline gate.

### Validation Plan
- Tests to add/update: extend the ctrees HDF5 reader test to assert cached `(offset, nhalos)` per row equals a direct `ForestInfo` read on a fixture; keep existing `ctrees_hdf5_test_*` entry points green.
- Commands to run: `make MODEL=halos-only SIMULATION=micro-uchuu-hdf5` (build the ctrees-HDF5 path), `tests/unit/run_tests.sh test_ctrees_support` (and the HDF5 reader test), `make MODEL=halos-only SIMULATION=micro-uchuu-hdf5 validate-modules`, then the **byte-identical baseline gate** on the ctrees fixtures before/after.
- Manual checks: a `--debug` run reports no `MEM_IO` leak; confirm the per-forest `ForestInfo` open no longer appears (e.g. count `H5Dopen2` via a temporary trace, or reason from the diff).

### Rollback Path
- Revert the single-file commit; `read_ctrees_hdf5.c` returns to per-forest `ForestInfo` reads. No schema/state migration to undo.

---

## Slice 2: Persistent per-file field-dataset handles + cached extents/types (Tier 1 read path)

### Intended Change
- Introduce a single source-of-truth **field table**: the 18 fixed `Forests/<field>` names in current read order, plus the runtime snap field (`CTH.snap_field_name`). Define a field count constant and an order (enum or `const char*[]`) used by both the read sequence and (later) the window.
- Add a **per-task-file cached handle set** to `CTH`: for each field, `{hid_t dataset, hid_t filespace, hid_t datatype, hsize_t extent}`. Open and validate them **once** per task-range file, immediately after `detect_snap_field` (so the snap field name/type is known), in `setup_forests_io_ctrees_hdf5`.
- Move per-field validation to open time (§7 fast failure): 1-D extent read, datatype size == 8 B, equal extents across fields in a file. A schema/layout mismatch now fails **before** the forest loop, not mid-stream.
- Rewrite `ct_read_forest_array` to take a cached field handle and only `H5Sselect_hyperslab` on the cached filespace + `H5Screate_simple` (memspace) + `H5Dread` + close memspace. No `H5Dopen2`/`H5Dget_space`/`H5Dget_type`/extent read per call.
- Collapse `validate_ctrees_hdf5_forest_slab` to a cached-extent integer bounds check (drop its per-forest `H5Dopen2("Mvir")`).
- Keep `read_contiguous_forest_ctrees_h5` as the per-forest read primitive, now sourcing cached handles. Split "read field slab" from "assign into `halo_data`" cleanly (assignment macros take a source pointer), so Slice 3 can reuse the assign step against a window buffer.
- Close all cached handles in `close_partition_ctrees_hdf5`.

### Acceptance Criteria
- Inputs: unchanged.
- Outputs: byte-identical (same reads, same conversions, same order).
- User-visible behaviour: ~18× faster ctrees-HDF5 read path (per the findings benchmark, strategy B); identical output and identical fast-fail errors (now raised earlier).
- Behaviour that must not change: every existing validation (link forest-local bounds, snap range, extent/type/size checks) and `apply_ctrees_value_conventions`. The set of error conditions is unchanged; only *when* schema-level checks fire moves earlier.

### Authorized Surface
- Files allowed to change: `src/io/tree/read_ctrees_hdf5.c`.
- Functions/structs allowed to change: `struct ctrees_hdf5_partition` (add field-handle cache + field table support), `setup_forests_io_ctrees_hdf5` (open/validate handles for the task file range), `ct_read_forest_array` and the `CT_READ_*`/`CT_ASSIGN_*` macros (decouple read from assign; take cached handle + source pointer), `read_contiguous_forest_ctrees_h5`, `validate_ctrees_hdf5_forest_slab` (cached-extent check), `detect_snap_field` (may record snap datatype/handle), `close_partition_ctrees_hdf5`, and the `MIMIC_TEST_BUILD` entry points (`ctrees_hdf5_test_read_forest` etc.) to build/use the cache so tests exercise the cached path.
- Tests allowed/expected to change: `tests/unit/test_ctrees_support.c` and the HDF5 reader test.

### Explicit Non-Goals
- No window/bulk read (Slice 3).
- No on-disk format, property, or generated-code change.
- No new run-YAML parameter.

### Risk Flags
- Risky surfaces touched: HDF5 handle lifetime (leaks if close paths miss a field), global reader state, I/O seam, memory accounting. Single-threaded.
- Approval needed before implementation: no, but treat handle-leak accounting as a hard gate — `--debug` must show zero leaked HDF5 objects and zero `MEM_IO`/`MEM_TREES` leaks. Proceed under the baseline gate.

### Validation Plan
- Tests to add/update: a test that opens a fixture file's cache, reads several forests through cached handles, and asserts values equal the pre-change per-forest read; a negative test that a deliberately mistyped/short field fails at open time (earlier fast-fail).
- Commands to run: build + `tests/unit/run_tests.sh test_ctrees_support` (+ HDF5 reader test) + `validate-modules` + **byte-identical baseline gate** on `simulations/micro-uchuu-hdf5/_tests/data/` and `simulations/uchuu/_tests/data/`.
- Manual checks: `--debug` run — no leaked HDF5 identifiers (optionally assert via `H5Fget_obj_count` at close in a debug build) and no allocator leaks. Optional NT scale check (see Slice 3 plan) to confirm the ~18× before layering the window.

### Rollback Path
- Revert the single-file commit. Slice 1's `ForestInfo` cache is independent and remains.

---

## Slice 3: Bounded windowed bulk-slab reads (Tier 2 — the high-gain target)

### Intended Change
- Add a **bounded read window** to `CTH`: one buffer of `field_count × capacity_halos × 8` bytes (`MEM_IO`), plus cursor state `{int current_file; hsize_t window_start_halo; hsize_t window_len;}`. `capacity_halos = CTREES_READ_WINDOW_BYTES / (field_count × 8)`.
- Define `CTREES_READ_WINDOW_BYTES` as a named constant (recommend **128 MiB per rank**; rationale below). **Decision for this plan: fixed `#define`, not a YAML parameter** — mirrors the existing fixed `HDF5_WRITE_BUFFER_RECORDS = 8192`, keeps the run-YAML schema and generated parameter code untouched (a risky surface), and still satisfies §5 (predictable, bounded). YAML/HPC tunability is an explicit non-goal here and a one-parameter follow-up if needed.
- Rewrite `load_unit_ctrees_hdf5` dispatch:
  - lookup `(offset, nhalos)` from the Slice-1 cache; validate slab via Slice-2 cached extent;
  - **giant forest** (`nhalos > capacity_halos`): read directly via the Slice-2 `read_contiguous_forest_ctrees_h5` primitive (bounded by the forest, which must be materialised as `halo_data` anyway) — a required bounded-memory branch, not a fallback toggle;
  - **normal forest**: ensure the window covers `[offset, offset+nhalos)` for `current_file`; refill if the file changed or the forest leaves the window by reading each field's `[window_start, window_start+window_len)` slab in **one** `H5Dread` into `base + F*capacity` (one read per field per refill, ~`ceil(file_halos / capacity)` refills per file); then assign each forest from `base + F*capacity + (offset - window_start)` using the Slice-2 decoupled assign step (per-forest link/snap validation + conventions unchanged).
- Free the window in `close_partition_ctrees_hdf5`.

`capacity_halos` must be sized to hold at least the window slab; the giant-forest branch handles any forest larger than `capacity_halos`, so the window bound is hard (`128 MiB/rank`) regardless of the largest forest.

**Window-budget rationale (§5):** memory is **per rank**; on a 20-rank node a 128 MiB window is ~2.5 GiB/node. 128 MiB ⇒ `capacity ≈ 128·2^20 / (19×8) ≈ 880k halos`, collapsing a file of millions of halos to a handful of refills while staying node-friendly. A larger default (e.g. 256–512 MiB) trades node memory for fewer refills; the constant documents this and the per-rank nature.

### Acceptance Criteria
- Inputs: unchanged.
- Outputs: byte-identical (a bulk read sliced in memory yields the same bytes/order as per-forest sub-slab reads).
- User-visible behaviour: ~48× faster ctrees-HDF5 read path overall (findings strategy C); identical output and identical errors.
- Behaviour that must not change: per-forest link/snap validation and `apply_ctrees_value_conventions` (still per forest, per slice); bounded peak memory (window stays within `CTREES_READ_WINDOW_BYTES`/rank plus the unavoidable per-forest `halo_data`/`RawHalo` materialisation; giant-forest path adds only the reused `nhalos×8` field buffer).

### Authorized Surface
- Files allowed to change: `src/io/tree/read_ctrees_hdf5.c`; `docs/dev/CTREES-UCHUU-VALIDATION.md` (note the windowed read path in the reader description, §4).
- Functions/structs allowed to change: `struct ctrees_hdf5_partition` (window buffer + cursor), `setup_forests_io_ctrees_hdf5` (allocate window; compute `capacity_halos`), `load_unit_ctrees_hdf5` (window dispatch + refill + giant-forest branch), a new static helper (e.g. `window_refill_ctrees_hdf5` / `assign_forest_from_window`), `close_partition_ctrees_hdf5` (free window). Reuse the Slice-2 field table, cached handles, and decoupled assign step.
- Tests allowed/expected to change: `tests/unit/test_ctrees_support.c` / HDF5 reader test, with window-specific cases.

### Explicit Non-Goals
- No YAML parameter for the window (fixed `#define` this plan).
- No collective/parallel-HDF5 (MPI-IO) reads — major complexity, not needed for the per-task independent-slab pattern; out of scope.
- No change to the SOA→AOS copy / bridge cost (CPU, not the I/O bottleneck the 48× addresses); note as a possible future profile target, not in scope.
- No on-disk format/chunking/compression change to tree input.

### Risk Flags
- Risky surfaces touched: bounded-memory contract (§5) — the window is the explicit constraint; global reader state; concurrency none (single-threaded forward scan). Correctness is order-independent (refill covers the requested forest); only speed depends on the confirmed ascending `load_unit` order.
- **Approval needed before implementation: yes — this slice introduces the bounded read window (the §5-flagged surface) and the new per-rank memory constant.** Confirm `CTREES_READ_WINDOW_BYTES` (default 128 MiB/rank) before coding.

### Validation Plan
- Tests to add/update: (a) window-boundary — forests straddling a refill return identical halos to a direct read; (b) refill correctness across a synthetic file larger than the window (multiple refills); (c) giant-forest branch — a forest with `nhalos > capacity_halos` returns identical halos via the direct path; (d) bounded peak — window allocation never exceeds the constant.
- Commands to run: build + `tests/unit/run_tests.sh test_ctrees_support` (+ HDF5 reader test) + `validate-modules` + **byte-identical baseline gate** on both ctrees fixtures.
- Manual checks: `--debug` run confirms `MEM_IO`/`MEM_TREES` accounting, peak within the window, zero leaks. **Scale check (optional, recommended):** time a `halos-only` micro-Uchuu-HDF5 pass before/after on `dcroton@nt.swin.edu.au` (the ~13 GB / ~440k-forest dataset) to confirm the real-world speedup; avoid full Uchuu for timing (~1 hr/file).

### Rollback Path
- Revert the single-file commit; the reader returns to the Slice-2 per-forest cached-handle path (still ~18×, still byte-identical). Slices 1–2 remain intact.

---

## Slice 4: Documentation, validation-doc, and skill sweep

**Implementation status:** complete in the Slice 4 docs commit. No code or test behaviour changed. The external auto-memory entry `mimic-hdf5-io-optimisation` was updated under the local Claude project memory store; the repo-tracked implemented state is recorded in this plan and in `docs/dev/MIMIC-HDF5-IO-OPTIMISATION.md`.

### Intended Change
- Flip `docs/dev/MIMIC-HDF5-IO-OPTIMISATION.md` status to "Implemented (Slices 1–3)"; cross-link this plan; record the realised speedup (and NT scale-check numbers if gathered).
- Finalise the `docs/dev/CTREES-UCHUU-VALIDATION.md` §4 note (if not already landed in Slice 3) describing cached handles + windowed reads.
- Pre-commit **skill sweep** (per project checklist): update `.agents/skills/mimic-simulations` (and `mimic-debug` if it documents the ctrees reader I/O pattern) to reflect cached-handle + windowed reads and the `CTREES_READ_WINDOW_BYTES` constant. Mention the per-rank window in `docs/USER-GUIDE.md`/`DEVELOPER-GUIDE.md` only if a reader-memory note belongs there.
- Update the auto-memory `mimic-hdf5-io-optimisation` entry to "implemented".

### Acceptance Criteria
- Inputs/Outputs: docs only; no code or test behaviour change.
- User-visible behaviour: none.
- Behaviour that must not change: everything (docs-only slice).

### Authorized Surface
- Files allowed to change: `docs/dev/MIMIC-HDF5-IO-OPTIMISATION.md`, `docs/dev/CTREES-UCHUU-VALIDATION.md`, `.agents/skills/mimic-simulations/*`, `.agents/skills/mimic-debug/*` (only if stale), `docs/USER-GUIDE.md`/`docs/DEVELOPER-GUIDE.md` (only if a reader-memory note is warranted), and the auto-memory file.
- Tests allowed/expected to change: none.

### Explicit Non-Goals
- No code change; if a doc claim no longer matches code, fix the doc, not the code, in this slice.

### Risk Flags
- Risky surfaces touched: none (docs).
- Approval needed before implementation: no.

### Validation Plan
- Commands to run: `make check-docs` (link/anchor validation).
- Manual checks: re-read each touched doc against the landed code.

### Rollback Path
- Revert the docs commit.

---

## Cross-cutting validation (all code slices)

- **Byte-identical gate is mandatory** for Slices 1–3: run the local output-format baseline and both ctrees fixtures (`simulations/micro-uchuu-hdf5/_tests/data/`, `simulations/uchuu/_tests/data/`) before/after and require exact identity. This is the core safety net; any byte difference blocks the slice.
- **Leak gate:** every code slice must pass a `--debug` run with zero allocator leaks and zero leaked HDF5 identifiers.
- **Fast-failure preserved or improved (§7):** schema/extent/type mismatches must still fail, and should fail *no later* than before (Slices 1–2 move them to setup time).
- **Determinism (§6):** read order and values unchanged; cross-format identity gates still hold.
- Use the same `MODEL`/`SIMULATION` pair across generate/build/test (e.g. `MODEL=halos-only SIMULATION=micro-uchuu-hdf5` for the ctrees-HDF5 path).

---

## Slice ordering and rationale

1 → 2 → 3 → 4. Slice 1 removes the per-forest `ForestInfo` reopen and yields the `(offset, nhalos)` cache the window needs. Slice 2 establishes the field table, cached handles, and the read/assign split that the window reuses. Slice 3 layers the bounded window for the headline ~48×. Slice 4 closes the docs/skills/memory loop. Each code slice is independently shippable, byte-identical, and leaves no dead path: Slice 3 *reuses* the Slice-2 primitive for giant forests rather than keeping an old path alive.

---

## Completion Note

All four slices in this plan are complete. Future HDF5 reader work should start from a new narrow plan or issue, not by resuming this slice sequence.
