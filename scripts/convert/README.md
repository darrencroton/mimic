# ctrees ASCII → Snapshot-HDF5 Converter

External converter that transforms Consistent-Trees ASCII output (forest-ordered) into Mimic's snapshot-ordered HDF5 input format. The on-disk output contract is frozen in `docs/dev/SNAPSHOT-HDF5-FORMAT.md` (`format_version = 1`); the algorithm is specified by `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`. The sliced implementation plan that built this tool is complete and archived under `archive/dev-plans/` (search there for the converter implementation plan if the slice-level history is needed). The converter is a standalone tool: it never touches Mimic source, packages, or run files, and it never deletes source data — cleanup is restricted to manifest-owned intermediates it created under the workdir.

**Status:** complete and validated on the real micro-Uchuu ASCII data, and re-validated end to end on 2026-08-03 on a fully regenerated dataset (observed stack: pandas 3.0.5, numpy 2.4.6 — the stack used for the original 2026-07-24 run was not recorded, so this is a re-gate on a different-and-unknown-delta stack rather than a measured upgrade): the 327-test suite passes, the three totals the original gate recorded are reproduced exactly (22,580,924 halos, 50 snapshots, 440,651 forests), the producer battery passes all 15 checks, and the cross-check passes every check including `topology-chains`. `max_halo_rank_in_forest = 350074` is recorded here for the first time; no earlier value exists to compare against. Phases 0–4 (scatter, sort/index, fixups, links, HDF5 emission + producer validation battery + conversion report) plus the cross-check instrument, including the optional `topology-chains` check against an independent reference-topology dump (see below). The full pipeline ran end to end on the real micro-Uchuu ASCII tree (22,580,924 halos across 50 snapshots, 440,651 forests); the producer validation battery passes all invariants, and the cross-check against a Mimic `halos-only` reference run passes all seven checks — identity, FoF central, flyby signs, values, occupancy, and direct chain-order (`topology-chains`) — with zero unexplained mismatches. The topology-order gate is therefore fully discharged: `topology-chains` compared links, `ForestIndex`/`HaloRankInForest`, and the signed `MostBoundID` per halo over an asserted-complete dump of all 22,580,924 halos.

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

# Phase 4: emit snapshot_NNN.h5 + forests.h5 per docs/dev/SNAPSHOT-HDF5-FORMAT.md
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

# Cross-check vs a halos-only reference run: 'prepare' writes a scratch run
# file listing all snapshots, 'run-reference' captures the run log + exit
# code, 'compare' runs the check
mimic_venv/bin/python scripts/convert/crosscheck.py prepare \
    --run-file models/halos-only/input/halos-only_micro-uchuu-ascii.yaml \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list
mimic_venv/bin/python scripts/convert/crosscheck.py run-reference \
    --mimic ./mimic --run-file output/convert/micro-uchuu/reference_run.yaml \
    --log output/convert/micro-uchuu/reference_run.log
mimic_venv/bin/python scripts/convert/crosscheck.py compare \
    output/convert/micro-uchuu/hdf5 output/convert/micro-uchuu/reference-output \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml \
    --reference-topology output/convert/micro-uchuu/topology.dump  # optional, see below
```

Canonical metadata comes from explicit `--simulation-info` and `--a-list` paths, keeping the converter simulation-agnostic. Observed `(SnapNum, scale)` pairs from the data are cross-validated against the a_list (absolute tolerance 1e-4; an unknown pair aborts the run).

**Emitting to a final data location.** The commands above use `write`'s default output directory, `<workdir>/hdf5`. To place a dataset somewhere permanent instead, pass `write --output-dir <dir>` and emit there directly — do **not** move the files afterwards. The manifest records the emitted paths, and the battery's `manifest-binding` check compares the directory against them, so a post-hoc `mv` breaks validation. `report` reads the dataset location from `manifest["outputs_dir"]`, so it validates the real destination with no extra argument; `validate.py` and `crosscheck.py compare` take the dataset directory as their positional argument, so pass the destination in place of `<workdir>/hdf5` in those two commands. The 2026-08-03 micro-Uchuu regeneration used exactly this route, emitting straight to `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-snapshot/`.

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
                           snapshots included), per docs/dev/SNAPSHOT-HDF5-FORMAT.md
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

**Shin-Uchuu-scale notes (production conversion, out of scope here):** the Phase 0 forest map is currently passed to each pool task by pickling — at the ~5 GB Shin-Uchuu map size that needs a worker initializer with shared or memory-mapped storage; per-chunk per-snapshot boolean scans, whole-file concat reads, and the in-memory sort (~350 B/row peak) are likewise sized for micro-Uchuu, with a chunked external-merge fallback deferred to a future production pass. The fix-up stage's satellite chain resolution is a sequential per-satellite scan (reference-order in-place rewrites, required for exact fix_upid parity); it is a few seconds per snapshot at micro-Uchuu scale but would need revisiting for Shin-Uchuu. The link stage's rank pass groups every snapshot's sort keys in memory; the Shin-Uchuu super-forest needs a deferred chunked external-merge rank sort instead. The validation battery and cross-check similarly load the full emitted dataset (and reference galaxy output) into memory. Concurrent converter invocations on one workdir are not locked.

## Reference-topology proof

The six-check cross-check (above) establishes identity, rank, and central resolution by matching galaxies to halos. It does not, by itself, directly compare the *order* of the converter's `FirstProgenitor`/`NextProgenitor`/`NextHaloInFOFgroup` chains against another implementation reading the same source data — rank equality constrains the underlying sort but does not prove chain construction.

`tests/unit/tools/dump_ctrees_topology.c` closes that gap: a read-only harness that loads a Consistent-Trees-ASCII package through Mimic's own `consistent_trees_ascii` reader (the same reader code the converter's algorithm mirrors) and dumps every halo's link fields, by stable ctrees id, to a plain-text file. Build it with:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-ascii dump-ctrees-topology-tool
tests/unit/tools/build/dump_ctrees_topology <run_param_file> <output_dump_path>
```

**The run file must declare `output_format: binary`.** `build_topology_dump.sh` compiles `-DHDF5` into only three sources — `io/tree/registry.c`, `io/tree/hdf5.c`, and `io/tree/read_ctrees_hdf5.c` — so `src/core/read_parameter_file.c` is built without it and its `#ifndef HDF5` guard (`src/core/read_parameter_file.c:658-661`) rejects `output_format: hdf5` with `OutputFormat 'hdf5' requires HDF5 support`. The harness exits 1 before creating the dump file. This is purely a compile-flag consequence of the harness's deliberately minimal source set: the harness links no output writer and would never have written galaxies anyway. `crosscheck.py prepare` inherits whatever the source run file declares, and every committed `halos-only` run file uses `hdf5`, so the prepared `reference_run.yaml` cannot be passed to the harness directly. Copy it and override only the output format and directory. This cannot change the dumped topology: the harness reads only the `input`/`simulation` configuration, and emits every halo of every forest tagged with that halo's own `SnapNum` — it never consults the output snapshot list at all.

```bash
mimic_venv/bin/python - <<'PY'
import pathlib, yaml
w = pathlib.Path("output/convert/micro-uchuu")
d = yaml.safe_load((w / "reference_run.yaml").read_text())
d["output"]["output_format"] = "binary"
d["output"]["output_directory"] = str(w / "topology-scratch-output")
(w / "topology_run.yaml").write_text(yaml.safe_dump(d))
PY
```

Diff the parsed YAML of the two files afterwards and confirm `output_format` and `output_directory` are the only differences, so the harness is provably reading the same tree as the reference run.

The dump format is three header lines (format marker, column names, NA-sentinel value) followed by one row per halo: `forestnr rank id snapnum desc_id first_prog_id next_prog_id first_fof_id next_fof_id`, all fields int64, with the NA sentinel (`INT64_MIN`) marking "no link". Nothing else may appear: a `#` line after the header means a malformed dump (two runs concatenated, a re-run appended with `>>`) and is rejected rather than skipped. The harness exits non-zero if it could not write the dump completely, so a full disk cannot produce a short dump that looks finished.

Pass the dump to `crosscheck.py compare --reference-topology <dump>` to run the additional `topology-chains` check. It first asserts **coverage** — the dump must name every converter halo exactly once at every snapshot, with no duplicate `|MostBoundID|` — because without that the check would compare cleanly over whatever subset a truncated dump happened to contain and report `PASS`. Then, per halo, it compares:

- the five **links** (`Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`, `NextHaloInFOFgroup`), resolving each converter link index to an id via the target snapshot's ascending-`|MostBoundID|` order and comparing it against the dump's own recorded id — the chain-**order** proof;
- the two **identity** fields (`ForestIndex`, `HaloRankInForest`), which extends rank conformance from `identity-creation`'s first-appearance subset to every halo, including halos that never seed a galaxy;
- the halo's own **signed** `MostBoundID`, since matching is by magnitude and the `flyby-signs` check only compares signs over the matched Type 0/1 population.

Failures are reported as one counted summary line per (snapshot, field) with example ctrees ids, never one line per halo.

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
| `crosscheck.py`     | six-check cross-check vs a halos-only reference run (matching by \|MostBoundID\|, identity decode, FoF central, flyby signs, bit-exact values, occupancy predicate), an optional seventh `topology-chains` check against a reference-topology dump (coverage, links, identity, sign), + reference-run plumbing |
| `tests/`            | stdlib-unittest suite; synthetic fixture generator (`fixtures.py`); mock reference builder (`mock_reference.py`); committed golden fixtures under `tests/data/` |

## Tests

```bash
mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v
```

The dump harness's own format test lives with the simulation package it reads, not under `scripts/convert/tests/`: `simulations/micro-uchuu-ascii/_tests/integration/test_topology_dump_format.py`, part of `make SIMULATION=micro-uchuu-ascii MODEL=halos-only tests-integration`.
