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

# Batch mode (item 3): scatter a source that is never all local at once. Every
# invocation is handed the COMPLETE frozen inventory — not the subset on disk —
# so the frozen-source-set guard keeps comparing like with like; entries whose
# bytes have not arrived are deferred, and the run does not finalize.
mimic_venv/bin/python scripts/convert/convert_ctrees.py scatter --batch \
    --workdir output/convert/shin-uchuu \
    --forests-list .../forests.list --a-list .../shin-uchuu.a_list \
    --simulation-info .../simulation_info.yaml \
    $(cat inventory.txt)          # all 2,744 files, in the frozen order

# Record a scattered batch as consumed: verifies every intermediate that batch
# produced, then records that its source bytes may be deleted. The converter
# never deletes source data itself — the deletion stays with the operator.
mimic_venv/bin/python scripts/convert/convert_ctrees.py release \
    --workdir output/convert/shin-uchuu $(cat batch_1.txt)

# Explicit Phase 1 finalize, once no inventory entry is deferred any more
mimic_venv/bin/python scripts/convert/convert_ctrees.py finalize \
    --workdir output/convert/shin-uchuu --forests-list .../forests.list

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

```text
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

Re-running `scatter` skips source files whose manifest entry is complete and unchanged (size + mtime), so a crashed run resumes where it stopped. A source entry is `completed` or, in batch mode, `consumed`; `deferred` and `pending` are classified per run from the inventory plus what is on disk and are deliberately never written to the manifest, so a file that arrives later needs no state cleared. Per-file conservation — the pandas-independent row pre-count must equal the parsed and scattered row count exactly — is enforced before a file is recorded as complete. The manifest is bound to its input identities (a_list, forests.list, and the ordered source set are checksummed at first run); changing any of them, or changing a source file after snapshots were finalized, refuses to resume — use a fresh workdir. Every intermediate is verified against its registered content checksum before it is consumed, skip-trusted, or deleted, and non-finite input values (NaN/inf, or float64 values that overflow float32) abort the parse.

### Batch mode: the interleaved consumptive transfer

For a source too large to stage locally in one piece, `scatter --batch` supports the cycle `transfer batch → scatter → release → delete → transfer next batch`, resuming correctly at every step, with `finalize` last of all — every batch must be released before the conversion is finalized. Batch mode is off by default and changes nothing outside itself; inside it, the two different reasons a source file can be legitimately absent are told apart:

| State | Meaning | Effect |
|---|---|---|
| `deferred` | in the frozen inventory, bytes not transferred yet | skipped for now, not an error; the run scatters what has arrived and exits **without finalizing** |
| `consumed` | scatter completed, intermediates verified, bytes released by `release` | satisfies resume without being re-stat-ed or re-scattered; its recorded identity (size, mtime, md5, counts, checksums, observed pairs) stays the frozen record of what was processed |

Rules the cycle depends on:

- **The complete ordered inventory is frozen once, at first run, and every batch-mode invocation must supply all of it** through the positional `tree_files` argument. Passing only the subset currently on disk changes the frozen set and is refused. There is no new index artifact and no second copy of the list. "First run" includes a batch-mode scatter issued *before any bytes have arrived* — it scatters nothing, reports every entry as deferred, and still writes the frozen inventory and the metadata identities to the manifest, so the very next invocation is already guarded. That is one extra whole-manifest save per invocation, not per file.
- **Batch mode never finalizes, and release must come before finalize.** `_finalize_scatter` deletes the worker intermediates a later `release` has to verify, so finalizing when the last batch completes would make that batch impossible to release — and that is enforced, not merely advised: `release` **refuses** a source whose intermediates finalization has already deleted. Once they are gone the rows live in the concatenated snapshot, which the sort stage deletes in turn, so there is no artifact the release path could verify instead; releasing anyway would authorize deleting irreplaceable source bytes with nothing checked. Finalization is reachable only through `finalize`, which refuses to run while any entry is deferred. Outside batch mode `scatter` still finalizes automatically, exactly as before.
- **Consumption is an explicit operator action, never inferred from a missing file.** A `completed` entry whose bytes are gone but which was never released is an error naming the file: nothing verified that its intermediates survived. `release` is the way out (it verifies the intermediates, not the source bytes, so it still works once the bytes are gone).
- **`release` refuses** an entry that is not `completed`, an entry already `consumed`, a source whose on-disk size/mtime no longer match what was scattered, any registered intermediate that does not verify, and any source-owned intermediate that finalization has already deleted (release before finalizing, not after — including after a finalization that was interrupted part-way). Nothing is skipped: every intermediate the source produced is verified, or the release is refused. It is atomic across the files it is given, so a refusal on any of them leaves the persisted manifest untouched.
- Root-coverage validation at finalize reads each source file's observed roots from its registered sidecar, so it still sees every file's roots when no source byte is left on disk.
- A conversion driven this way emits a dataset byte-identical to a single all-at-once run, and a manifest identical in provenance and every per-source content field; only the lifecycle state differs (`consumed` versus `completed`).

**Shin-Uchuu-scale notes (production conversion, out of scope here):** the Phase 0 forest map is currently passed to each pool task by pickling — at the ~5 GB Shin-Uchuu map size that needs a worker initializer with shared or memory-mapped storage; per-chunk per-snapshot boolean scans, whole-file concat reads, and the in-memory sort (~350 B/row peak) are likewise sized for micro-Uchuu, with a chunked external-merge fallback deferred to a future production pass. The fix-up stage's satellite chain resolution is a sequential per-satellite scan (reference-order in-place rewrites, required for exact fix_upid parity); it is a few seconds per snapshot at micro-Uchuu scale but would need revisiting for Shin-Uchuu. The link stage's rank pass groups every snapshot's sort keys in memory; the Shin-Uchuu super-forest needs a deferred chunked external-merge rank sort instead. The validation battery and cross-check similarly load the full emitted dataset (and reference galaxy output) into memory. Concurrent converter invocations on one workdir are not locked.

## Building a subset of a very large dataset

`subset.py` selects a tractable, representative **whole-forest** subset of a ctrees dataset far too
large to convert in one pass, and extracts it byte-exactly. It never reads the bulk tree data:
forests are ranked from `forests.list`, `locations.dat` and a `stat` size inventory, then one root
row is read per candidate tree, then only the selected byte ranges are copied. Design:
`docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md` → "Subset Selection and Extraction".

Stages alternate between the analysis machine and the machine holding the data, because root-row
sampling needs the source bytes and those must not be transferred in bulk. `subset.py` is
**numpy-only** for exactly this reason — the data node has no pandas.

```bash
# 1. local: per-tree and per-forest tables + the top-M candidate pool by byte extent
mimic_venv/bin/python scripts/convert/subset.py plan-candidates \
    --index <dir with forests.list, locations.dat, filesizes.tsv> --out <work> --m <M>

# 2. on the data node: one root row per candidate (ship candidates.npy AND filemap.json)
mimic_venv/bin/python scripts/convert/subset.py sample-roots \
    --candidates <work>/candidates.npy --filemap <work>/filemap.json \
    --trees <tree dir> --a-list <scale factor list> --out <work>/root_values.npy

# 3. local: tractability gates, strata, file-coverage closure, selection manifest
mimic_venv/bin/python scripts/convert/subset.py finalize \
    --tree-table <work>/tree_table.npy --forest-table <work>/forest_table.npy \
    --candidates <work>/candidates.npy --root-values <work>/root_values.npy \
    --filemap <work>/filemap.json --out <work>/selection \
    --target-trees <n> --k <supplement size> --seed <fixed>

# 4. on the data node: stream the selected ranges out, then verify before transferring
mimic_venv/bin/python scripts/convert/subset.py extract \
    --selection <work>/selection --trees <tree dir> --out <subset dir>
```

**Whole forests, always.** `fix_flybys`/`fix_upid` work with per-forest max-snapshot scope, so a
partial forest converts differently from the same forest in a full run. When a source file would
otherwise contribute no selected tree — which `read_locations()` will not tolerate, since it asserts
file ids are contiguous from 0 and that the file count is a perfect cube — the gap is closed by
adding the smallest complete forest touching it, never a lone tree, iterating until closed.

**`extract` verifies before you pay for the transfer**, and the verification is not optional: body
md5s against the source ranges, marker placement, the rewritten count line, that no body contains a
`#tree` marker and every body ends on a newline (an extent off by even one byte is caught here rather
than by the converter after transfer), and one-to-one root coverage in the emitted index files.

**`calibrate-proxy` is not a production step.** Byte extent is a *proxy* for root mass, and the
recovery fraction that measures how good a proxy it is needs the true top-`K` forests over every
tree. Run it on a calibration dataset small enough to sample exhaustively (`plan-candidates --m 0`,
then `sample-roots`), and carry the calibrated relative depth to the production dataset as
`M = ceil(depth × n_trees)`.

Exit codes: **0** success; **1** the run completed but a `finalize` acceptance assertion or an
`extract` verification failed; **2** fatal — a violated invariant, bad input, or an unreadable
artifact.

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
| `subset.py`         | whole-forest subset selection and byte-exact extraction from a very large ctrees dataset, driven entirely from the index files: `plan-candidates` / `sample-roots` / `calibrate-proxy` / `finalize` / `extract` |
| `crosscheck.py`     | six-check cross-check vs a halos-only reference run (matching by \|MostBoundID\|, identity decode, FoF central, flyby signs, bit-exact values, occupancy predicate), an optional seventh `topology-chains` check against a reference-topology dump (coverage, links, identity, sign), + reference-run plumbing |
| `tests/`            | stdlib-unittest suite; synthetic fixture generator (`fixtures.py`); mock reference builder (`mock_reference.py`); committed golden fixtures under `tests/data/` |

## Tests

```bash
mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v
```

The dump harness's own format test lives with the simulation package it reads, not under `scripts/convert/tests/`: `simulations/micro-uchuu-ascii/_tests/integration/test_topology_dump_format.py`, part of `make SIMULATION=micro-uchuu-ascii MODEL=halos-only tests-integration`.
