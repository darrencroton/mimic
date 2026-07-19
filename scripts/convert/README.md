# ctrees ASCII → Snapshot-HDF5 Converter

External converter that transforms Consistent-Trees ASCII output (forest-ordered) into Mimic's snapshot-ordered HDF5 input format. The on-disk output contract is frozen in `docs/SNAPSHOT-HDF5-FORMAT.md` (`format_version = 1`); the algorithm and its implementation slices are specified by `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md` and `docs/dev/MIMIC-CONVERTER-IMPLEMENTATION-PLAN.md`. The converter is a standalone tool: it never touches Mimic source, packages, or run files, and it never deletes source data — cleanup is restricted to manifest-owned intermediates it created under the workdir.

**Status:** phases 0–4 implemented (scatter, sort/index, fixups, links, HDF5 emission + producer validation battery + conversion report — plan Slices 2–7) plus the cross-check instrument (Slice 8, synthetic-fixture validated). The real micro-Uchuu end-to-end gate run is plan Slice 9 and has not run yet. The optional `--reference-topology` chain-order mode is not implemented: it depends on the approval-gated Slice 10 harness, so the cross-check currently implements the plan's six-check partial topology gate.

## Requirements

Python 3.9+, `numpy`, `pandas`, `PyYAML` — all installed into `mimic_venv` by `pip install -r requirements.txt` from the repository root.

## Usage

```bash
# Phase 0 + 1: forest map, scatter ctrees files into per-snapshot scratch binaries
mimic_venv/bin/python scripts/convert/convert_ctrees.py scatter \
    --workdir output/convert/micro-uchuu \
    --forests-list simulations/micro-uchuu-ascii/snapshots/forests.list \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml \
    simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat

# Phase 2: per-snapshot sort by halo id + id index
mimic_venv/bin/python scripts/convert/convert_ctrees.py sort \
    --workdir output/convert/micro-uchuu

# Phase 3 steps 1-5: adjacency validation, spin/Len conventions,
# fix_flybys/fix_upid equivalents (reference semantics, D12)
mimic_venv/bin/python scripts/convert/convert_ctrees.py fixups \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml

# Phase 3 steps 6-9: FoF chains, descendant/progenitor links, within-forest
# ranks, identity fields (always all snapshots — FirstProgenitor flows forward
# through a per-snapshot pending buffer)
mimic_venv/bin/python scripts/convert/convert_ctrees.py links \
    --workdir output/convert/micro-uchuu

# Phase 4: emit snapshot_NNN.h5 + forests.h5 per docs/SNAPSHOT-HDF5-FORMAT.md
# (one file per a_list snapshot, including empty ones; default <workdir>/hdf5)
mimic_venv/bin/python scripts/convert/convert_ctrees.py write \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml

# Producer validation battery (standalone; non-zero exit on any failure;
# --manifest is required — count conservation against the independent
# pre-counts is a mandatory part of the battery)
mimic_venv/bin/python scripts/convert/validate.py output/convert/micro-uchuu/hdf5 \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --manifest output/convert/micro-uchuu/manifest.json

# Conversion report (runs the battery, writes conversion_report.{json,txt};
# exits 1 if validation failed)
mimic_venv/bin/python scripts/convert/convert_ctrees.py report \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list

# Micro-Uchuu cross-check vs a halos-only reference run (plan Slice 9 executes
# this on real data; 'prepare' writes a scratch run file listing all snapshots,
# 'run-reference' captures the run log + exit code, 'compare' is the six-check gate)
mimic_venv/bin/python scripts/convert/crosscheck.py prepare \
    --run-file models/halos-only/input/halos-only_micro-uchuu-ascii.yaml \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list
mimic_venv/bin/python scripts/convert/crosscheck.py run-reference \
    --mimic ./mimic --run-file output/convert/micro-uchuu/reference_run.yaml \
    --log output/convert/micro-uchuu/reference_run.log
mimic_venv/bin/python scripts/convert/crosscheck.py compare \
    output/convert/micro-uchuu/hdf5 output/convert/micro-uchuu/reference-output \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list
```

Canonical metadata comes from explicit `--simulation-info` and `--a-list` paths, keeping the converter simulation-agnostic. Observed `(SnapNum, scale)` pairs from the data are cross-validated against the a_list (absolute tolerance 1e-4; an unknown pair aborts the run).

## Workdir layout

```
<workdir>/
  manifest.json            resume manifest: source files (size/mtime/md5, independent
                           pre-count, parsed count, per-snapshot counts and id checksums),
                           every intermediate the converter created, snapshot status
  forest_max_snap.npy      per-forest max-snapshot table (Nx2 int64: forest_id, max snap)
  forest_index_table.npy   dense ForestIndex -> ctrees forest id (ascending forest id);
                           emitted as forests.h5 by the Phase 4 writer
  conversion_report.json   durable conversion report (totals, per-snapshot counts,
  conversion_report.txt    identity bounds, observed pairs, validation outcomes,
                           recommended identity multiplier)
  hdf5/
    snapshot_NNN.h5        emitted dataset, one file per a_list snapshot (empty
                           snapshots included), per docs/SNAPSHOT-HDF5-FORMAT.md
    forests.h5             /ForestID sidecar (dense ForestIndex -> ctrees forest id)
  scratch/
    snap_NNN.bin           concatenated per-snapshot records (deleted after sort verifies)
    snap_NNN_sorted.bin    records sorted by ascending halo id
    snap_NNN.idx           sorted int64 id array for Phase 3 merge-joins
    snap_NNN_fixed.bin     fixed records (120-byte dtype: frozen fields + Len +
                           MostBoundID; Jx/Jy/Jz now carry normalised Spin)
    snap_NNN_links.bin     link/identity records (36-byte dtype: Descendant,
                           FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup,
                           NextHaloInFOFgroup int32; ForestIndex,
                           HaloRankInForest int64), row-aligned with the fixed file
    snap_NNN_pending_fp.bin pending FirstProgenitor buffer for snapshot NNN
                           (int32, written while snapshot NNN-1 is resident)
    roots_src_I.npy        observed #tree roots per source file
    forest_max_src_I.npy   per-file forest max-snapshot aggregates
```

Scratch records use the frozen 108-byte packed little-endian dtype defined in `ctrees_parser.py` (`RECORD_DTYPE`); the manifest records the dtype tag and refuses to resume across a dtype change.

Re-running `scatter` skips source files whose manifest entry is complete and unchanged (size + mtime), so a crashed run resumes where it stopped. Per-file conservation — the pandas-independent row pre-count must equal the parsed and scattered row count exactly — is enforced before a file is recorded as complete. The manifest is bound to its input identities (a_list, forests.list, and the ordered source set are checksummed at first run); changing any of them, or changing a source file after snapshots were finalized, refuses to resume — use a fresh workdir. Every intermediate is verified against its registered content checksum before it is consumed, skip-trusted, or deleted, and non-finite input values (NaN/inf, or float64 values that overflow float32) abort the parse.

**Shin-Uchuu-scale notes (production conversion, out of scope here):** the Phase 0 forest map is currently passed to each pool task by pickling — at the ~5 GB Shin-Uchuu map size that needs a worker initializer with shared or memory-mapped storage; per-chunk per-snapshot boolean scans, whole-file concat reads, and the in-memory sort (~350 B/row peak) are likewise sized for micro-Uchuu, with the chunked external-merge fallback deferred by the plan. The fix-up stage's satellite chain resolution is a sequential per-satellite scan (reference-order in-place rewrites, required for exact fix_upid parity); it is a few seconds per snapshot at micro-Uchuu scale but would need revisiting for Shin-Uchuu. The link stage's rank pass groups every snapshot's sort keys in memory; the Shin-Uchuu super-forest needs the plan's deferred chunked external-merge rank sort instead. The validation battery and cross-check similarly load the full emitted dataset (and reference galaxy output) into memory. Concurrent converter invocations on one workdir are not locked.

## Module map

| File | Role |
|---|---|
| `convert_ctrees.py` | CLI: per-phase subcommands |
| `ctrees_parser.py`  | frozen record dtype; indexed/`#fields:` header dialects; `#tree` marker tracking; chunked reads; independent pre-count |
| `scatter.py`        | Phase 0 forests.list map + dense ForestIndex; Phase 1 scatter/concat; resume manifest; cleanup containment guard |
| `sort_index.py`     | Phase 2 per-snapshot sort + id index; verify-then-delete |
| `fixups.py`         | Phase 3 steps 1–5: a_list adjacency validation; spin `J/Mvir` and Len conventions; `fix_flybys`/`fix_upid` reference equivalents |
| `links.py`          | Phase 3 steps 6–9: FoF chains, descendant merge-join, progenitor chains (literal `assign_mergertree_indices` insertion semantics), within-forest ranks, identity fields |
| `hdf5_writer.py`    | Phase 4: `snapshot_NNN.h5` + `forests.h5` emission per the frozen contract (empty snapshots included; write-verify-record) |
| `validate.py`       | producer validation battery (standalone CLI): structural conformance, all six format invariants, progenitor round-trip closure, FoF chain walk, identity density, header bounds, count conservation vs the independent pre-counts |
| `report.py`         | conversion report emission (`conversion_report.{json,txt}`) including battery outcomes and the recommended identity multiplier |
| `crosscheck.py`     | six-check cross-check vs a halos-only reference run (matching by \|MostBoundID\|, identity decode, FoF central, flyby signs, bit-exact values, occupancy predicate) + reference-run plumbing |
| `tests/`            | stdlib-unittest suite; synthetic fixture generator (`fixtures.py`); mock reference builder (`mock_reference.py`); committed golden fixtures under `tests/data/` |

## Tests

```bash
mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v
```
