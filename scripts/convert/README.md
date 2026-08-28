# ctrees ASCII → Snapshot-HDF5 Converter

External converter that transforms Consistent-Trees ASCII output (forest-ordered) into Mimic's snapshot-ordered HDF5 input format. The on-disk output contract is frozen in `docs/dev/SNAPSHOT-HDF5-FORMAT.md` (`format_version = 1`); the algorithm is specified by `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`. The sliced implementation plan that built this tool is complete and archived under `archive/dev-plans/` (search there for the converter implementation plan if the slice-level history is needed). The converter is a standalone tool: it never touches Mimic source, packages, or run files, and it never deletes source data — cleanup is restricted to manifest-owned intermediates it created under the workdir.

**Status:** complete and validated on the real micro-Uchuu ASCII data; re-gated end to end on **2026-08-28** after the converter scale-engineering pass rebuilt its scale-critical machinery (see "The 2026-08-28 acceptance gate" below), and re-validated end to end on 2026-08-03 on a fully regenerated dataset (observed stack: pandas 3.0.5, numpy 2.4.6 — the stack used for the original 2026-07-24 run was not recorded, so this is a re-gate on a different-and-unknown-delta stack rather than a measured upgrade): the 327-test suite passes, the three totals the original gate recorded are reproduced exactly (22,580,924 halos, 50 snapshots, 440,651 forests), the producer battery passes all 15 checks, and the cross-check passes every check including `topology-chains`. `max_halo_rank_in_forest = 350074` is recorded here for the first time; no earlier value exists to compare against. Phases 0–4 (scatter, sort/index, fixups, links, HDF5 emission + producer validation battery + conversion report) plus the cross-check instrument, including the optional `topology-chains` check against an independent reference-topology dump (see below). The full pipeline ran end to end on the real micro-Uchuu ASCII tree (22,580,924 halos across 50 snapshots, 440,651 forests); the producer validation battery passes all invariants, and the cross-check against a Mimic `halos-only` reference run passes all seven checks — identity, FoF central, flyby signs, values, occupancy, and direct chain-order (`topology-chains`) — with zero unexplained mismatches. The topology-order gate is therefore fully discharged: `topology-chains` compared links, `ForestIndex`/`HaloRankInForest`, and the signed `MostBoundID` per halo over an asserted-complete dump of all 22,580,924 halos.

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
# through a per-snapshot pending buffer). --memory-budget-mb bounds the rank
# pass's working set (default 2048); it trades memory against spill I/O and
# changes no emitted value
mimic_venv/bin/python scripts/convert/convert_ctrees.py links \
    --workdir output/convert/micro-uchuu

# Phase 4: emit snapshot_NNN.h5 + forests.h5 per docs/dev/SNAPSHOT-HDF5-FORMAT.md
# (one file per a_list snapshot, including empty ones; default <workdir>/hdf5)
mimic_venv/bin/python scripts/convert/convert_ctrees.py write \
    --workdir output/convert/micro-uchuu \
    --a-list simulations/micro-uchuu-ascii/micro-uchuu.a_list \
    --simulation-info simulations/micro-uchuu-ascii/simulation_info.yaml

# Consumptive deletion (off by default; see "Consumptive deletion of
# intermediates" below). Add --consume-intermediates to fixups, links and write
# when the workdir cannot hold every intermediate at once. IRREVERSIBLE.
mimic_venv/bin/python scripts/convert/convert_ctrees.py fixups --consume-intermediates \
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
    snap_NNN.bin           concatenated per-snapshot records (always deleted after sort verifies)
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
    links_identity_*/      transient per-invocation rank/identity scratch: the
                           external merge sort's spill runs plus two int64
                           arrays (ForestIndex, HaloRankInForest) indexed by
                           global position, 8 B/halo each. Created by the links
                           stage itself and removed by it on both the success
                           and the failure path, so it is never a manifest
                           intermediate. Removal is attempted, not guaranteed:
                           if it fails the stage keeps ownership, retries, and
                           warns naming the directory and its size
    roots_src_I.npy        observed #tree roots per source file
    forest_max_src_I.npy   per-file forest max-snapshot aggregates
```

Scratch records use the frozen 108-byte packed little-endian dtype defined in `ctrees_parser.py` (`RECORD_DTYPE`); the manifest records the dtype tag and refuses to resume across a dtype change.

Re-running `scatter` skips source files whose manifest entry is complete and unchanged (size + mtime), so a crashed run resumes where it stopped. A source entry is `completed` or, in batch mode, `consumed`; `deferred` and `pending` are classified per run from the inventory plus what is on disk and are deliberately never written to the manifest, so a file that arrives later needs no state cleared. Per-file conservation — the pandas-independent row pre-count must equal the parsed and scattered row count exactly — is enforced before a file is recorded as complete. The manifest is bound to its input identities (a_list, forests.list, and the ordered source set are checksummed at first run); changing any of them, or changing a source file after snapshots were finalized, refuses to resume — use a fresh workdir. Every intermediate is verified against its registered content checksum before it is consumed, skip-trusted, or deleted, and non-finite input values (NaN/inf, or float64 values that overflow float32) abort the parse.

### Consumptive deletion of intermediates

Five of the intermediates above are retained by default — the sorted, index, fixed,
links and pending-buffer files — so the workdir holds all five for every snapshot at
once, a measured 277 bytes per halo. (The other two are not flag-gated: `snap_NNN.bin`
is deleted once sort verifies its successors, and the link stage removes
`links_identity_*/` itself, on its success and its failure path alike. The second of
those is *attempted* rather than guaranteed, exactly as the table below records: a
removal that does not leave the directory absent keeps the stage's ownership, is
retried and is warned about, never assumed (`links.py:644-685`).) `--consume-intermediates` deletes each one at the point its
**terminal** consumer is finished with it. It is **off by default and irreversible**:
turning it on trades resumability for storage, and is only worth doing when the volume
cannot hold the full set.

The flag is per invocation and belongs on `fixups`, `links` and `write`. With it off,
those stages delete nothing they do not already delete. The emitted dataset is
bitwise identical either way.

| Intermediate | Terminal consumer | Deleted by | Point of deletion |
|---|---|---|---|
| `snap_NNN.bin` (concatenated) | `sort` | `sort` | after the sorted file and index verify — **always**, flag or no flag |
| `snap_NNN_sorted.bin` | `fixups` | `fixups` | once snapshot N's fixed output is verified and registered |
| `snap_NNN.idx` | `link_one_snapshot(N-1)` | `links` | once snapshot **N−1** is linked; an index whose predecessor is not a recorded snapshot has no consumer at all and goes as soon as linking starts |
| `snap_NNN_pending_fp.bin` | `link_one_snapshot(N)` | `links` | once snapshot N is linked |
| `snap_NNN_fixed.bin` | the **writer** | `write` | once snapshot N's emitted HDF5 is verified and recorded |
| `snap_NNN_links.bin` | the **writer** | `write` | once snapshot N's emitted HDF5 is verified and recorded |
| `links_identity_*/` | the link stage itself | `links` | on both the success and the failure path, flag or no flag; never a manifest intermediate. A removal that fails is retried and warned about, not silently assumed |

Two of those consumer relations are easy to get wrong, and both are load-bearing. The
fixed file is read **twice** — by `links` through `_load_fixed` and by the writer
through `_load_snapshot_scratch` — so the writer, not `links`, is its terminal
consumer; the same holds for the links file. And `snap_NNN.idx` is read by the link of
the snapshot *below* it, to resolve descendants, so the highest snapshot's index is
consumed perfectly normally while `snap_000.idx` is the one with no consumer.

The mechanics follow the converter's delete-after-verify discipline throughout: the
successor is re-read from disk and verified, registered in the manifest, and the
manifest saved, *before* the predecessor is removed; the removal itself goes through
`Manifest.remove_intermediate`, which re-checks ownership, workdir containment and the
registered content checksum, and the manifest is saved again. `remove_intermediate` is
the only place in the converter that unlinks a manifest-owned intermediate, so no
deletion here can bypass that guard. A crash between the unlink and the second save leaves an
entry recorded `present` with no file on disk; the next run converges it to `removed`
and continues, in either flag state, because those bytes are already gone.

**What consumption costs you.** Deletion is bounded by re-run reachability, not by last
read. Which re-runs stay reachable depends on the snapshot's recorded status, so the
promises below are stated per status rather than in general — a skip that is claimed
more broadly than it holds is worse than no claim at all.

- **`sort`** does the work at `concatenated`, skips at `sorted` and `fixed`, and skips
  at `linked` **only when consumptive deletion has actually taken one of that
  snapshot's artifacts** — with nothing consumed it refuses a `linked` snapshot exactly
  as it always did, so a flag-off workdir behaves as before. Its own two outputs — the
  sorted file and the index — are accepted as consumed at two of those three, not at
  all of them: at `sorted` neither consumer can have run yet, so both are verified
  outright there, and only at `fixed` and `linked` is a consumption accepted
  (`sort_index.py:76-81`). The fixed file does not exist yet at `sorted`, must still be on
  disk at `fixed` — where it is verified outright, because the writer that consumes it
  runs only once *every* snapshot is linked — and is accepted as consumed only at
  `linked`, where the links file joins it on the same terms. An
  artifact that merely went missing, or whose checksum moved, is still the hard error it
  has always been at every status.
- **`fixups`** does the work at `sorted`, skips at `fixed`, and skips at `linked` under
  the same consumption gate, on the same split: strict verification of the fixed file at
  `fixed`, fixed and links accepted as consumed at `linked`.
- **`links`** requires every snapshot at `fixed` or `linked`. When they are all `linked`
  and **any** fixed input is recorded consumed it skips, because the rank pass streams
  **every** fixed file and so cannot run again once even one is gone — an interrupted
  writer is enough. Before skipping it verifies every links output still on disk, then
  drains any deletion an earlier flag-off run or an interrupted writer left undone, so
  turning the flag on late still reclaims the indexes and pending buffers. Only while
  **every** fixed input is present does the pass run as before, including its
  refuse-not-repair comparison of the run-scoped identity values.
- **`write`** skips a snapshot whose emitted file is already recorded and unchanged, and
  needs no scratch to do it. That is the skip it always had.
- **A `links` run interrupted part-way still resumes**: an index or pending buffer is
  deleted only after the snapshot that reads it has been linked.
- **`finalize`, and a non-batch `scatter`** (which finalizes at the end), skip a
  snapshot at `concatenated`, `sorted` or `fixed`, accepting a consumed sorted file or
  index — the same helper `sort` uses, so the two cannot drift apart. **They do not
  handle a snapshot at `linked`**: such a snapshot falls through to the concat path,
  which needs the per-source worker scratch that finalization itself deleted, and the
  run aborts naming that file. **This is not a consequence of consumptive deletion** —
  it reproduces exactly with the flag off, and has always been so. The rule it implies
  is simply: **once `links` has run, do not re-run `finalize` or a non-batch `scatter`
  on that workdir.**

One rule decides every "accepted as consumed" above: **an artifact is accepted as
consumed exactly at the statuses where its terminal consumer has provably run**, and is
verified outright everywhere else. `sorted` is taken by `fixups`, which saves the
`fixed` status before removing it; `idx` is taken by `links`, which will not start until
every snapshot is at least `fixed`; `fixed` and `links` are taken by the writer, which
runs only once every snapshot is `linked`. A manifest claiming a consumption earlier
than that describes a premature deletion, and is refused rather than skipped.

**The supported sequences**, then, are the documented order —
`scatter` → `release` → `finalize` → `sort` → `fixups` → `links` → `write` → `report` —
and a re-run of any stage from `sort` onward at any point after it, with one exception
the bullets above already state: with nothing consumed, `sort` and `fixups` refuse a
snapshot `links` has carried to `linked` (`sort_index.py:45-52`, `fixups.py:577-583`).
That is the pre-Slice-8 behaviour, deliberately preserved, so on a **flag-off** workdir
those two stages are re-runnable only before `links` has run; `links` and `write` are
re-runnable in either flag state. What is not supported is re-entering `finalize` or a non-batch `scatter` after
`links`, for the pre-existing reason above.

**What it forecloses.** Once a stage's inputs are gone that stage cannot be re-executed,
only skipped — if an emitted file is later deleted or corrupted, the conversion must be
re-run from the last surviving stage, and if nothing survives, from the source. That is
the whole cost, and it is why the flag is opt-in.

`release` is unaffected either way: it verifies the per-source sidecars and worker
scratch, none of which are in the table above, and still refuses a source whose own
intermediates finalization has deleted.

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

### Shin-Uchuu-scale notes

The converter scale-engineering pass (`docs/dev/CONVERTER-SCALE-PASS-PLAN.md`; joint review F-13/D4) rebuilt the machinery these notes used to defer to a future production pass, and its acceptance gate ran green on 2026-08-28 — see "The 2026-08-28 acceptance gate" below. This section is what each stage does now and what is still sized for the dataset.

**Every figure here is labelled with the scale it was taken at, and none of the rehearsal figures is a production projection.** *Rehearsal scale* is the retained 406,668,896-halo Shin-Uchuu subset — 1.8% of the production dataset — and *micro-Uchuu scale* is 22,580,924 halos. The remaining scale term in the link stage, the battery and the cross-check alike is the **per-snapshot window**, which grows with the largest snapshot rather than with the total halo count; production figures come from the production conversion's own per-snapshot counts, not from scaling these.

- **Scatter.** The Phase 0 forest map is no longer pickled into every pool task: a `Pool` initializer (`_init_scatter_worker`, `scatter.py:819`) loads `forests.list` once per worker process and binds each worker's independent load to the parent's `ForestMap.md5`, so the ~5 GB production map is never a per-task argument. The manifest is persisted on a bounded-interval policy (`save_every_n_files`, default 25 — `scatter.py:54-55`) rather than rewritten after every source file, which cost a measured 38.2 KB per file and reached 104.9 MB at 2,744 files, quadratic in file count. The frozen-source-set resume model is batch-aware (see "Batch mode" above), so the source can be consumed as it is scattered. Per-chunk per-snapshot scans and whole-file concat reads are unchanged.
- **Fix-ups.** The satellite chain resolution is still a sequential per-satellite scan doing reference-order in-place rewrites, because exact `fix_upid` parity is load-bearing. It was measured rather than rewritten: ≈1.28 µs/satellite at rehearsal scale (1.37 s at snapshot 44, 2.49 s at snapshot 69), projecting to ≈1.2–1.6 h across all 70 snapshots at production on a one-time multi-day conversion. **Answered: retain.** `fix_flybys` is the larger per-snapshot term where demotions are heavy (7.92 s for 1,194,990 demotions at snapshot 69) and projects to only ~5 min at production.
- **The rank pass (`links`)** is bounded (plan Slice 5). It ranks through the external merge sort in `rank_sort.py` under `--memory-budget-mb`, derives `ForestIndex` per snapshot, keeps `(ForestIndex, HaloRankInForest)` in on-disk arrays, holds only the adjacent snapshot pair the link stage is working on, and verifies identity exactly with one bit per halo. Measured at rehearsal scale on 2026-08-28, three runs at the shipped 2 GiB default budget (one cold, two warm): **9.76 / 9.81 / 10.01 GB peak RSS = 24.00 / 24.11 / 24.62 B/halo**, against the in-memory baseline's **76.39 GB = 187.84 B/halo** — a 7.6–7.8× reduction — in 961 / 896 / 885 s, with 15 sorted runs, 0 merge passes, 19,520,107,008 B of transient spill and 6,506,702,336 B of identity arrays on disk. All 139 link-stage artifacts it produced were md5-identical to the retained rehearsal's, and the run-scoped identity values reproduced exactly (`n_forests_total` = 6,011,205, `max_halo_rank_in_forest` = 8,312,565).
- **The producer validation battery** is bounded (plan Slice 6). It streams the emitted dataset instead of loading it, holding one snapshot for the per-snapshot checks and the adjacent pair for progenitor closure, carrying scalars for count conservation and the run-scoped header comparison, and settling `check_identity` exactly with one bit per halo rather than with a global sort. ForestIndex values outside `[0, n_forests_total)` keep their own groups, as the whole-dataset lexsort gave them, through a transient external ordering on disk rather than a per-value table in memory, because a structurally conformant dataset may carry one distinct such value per halo. Every check still runs, in the same order, and reports the same outcomes and messages as the whole-dataset formulation it replaced, which measured 73.27 GB on the same 1.8% subset. Measured at rehearsal scale on 2026-08-28, three runs (one cold, two warm): **3.258 / 3.246 / 3.245 GB peak RSS** against that 73.27 GB baseline, in 166 / 102 / 102 s, holding a 50,833,612-byte identity bitset and spilling nothing to disk. At the production 22.9 × 10⁹ halos that bitset is 2.86 GB; the per-snapshot window is the term that has to be re-derived there.
- **The topology cross-check** is bounded (plan Slice 7). `compare` walks the snapshots in ascending order holding one snapshot's converter arrays and one snapshot's reference galaxies, keeps the cross-snapshot `UniqueGalaxyID` suppression set that `identity-creation` needs as a disk-backed sorted union merged one block at a time (exact, never a growing in-memory set and never a probabilistic filter), and partitions the reference-topology dump by snapshot on disk as it parses it instead of materialising the whole dump and a global sort permutation. All eight checks still run in the same order and report the same outcomes, messages and `crosscheck_report.json` content as the whole-dataset formulation, which measured 251.32 GB on the same 1.8% subset with a 229.5 GB transient inside `np.loadtxt`. Measured on the retained rehearsal artifacts on 2026-08-28 (a 109.7 GB, 13-chunk reference output and a 42.07 GB dump), three runs (one cold, two warm): **16.56 / 15.84 GB warm and 10.09 GB cold peak RSS** against that 251.32 GB baseline, in 660 / 650 s warm and 1,528 s cold, holding a 274,967,800-byte suppression set and a 29,280,160,512-byte dump partition on disk, both removed on the success and failure paths alike. The cold run's peak is the **lowest** of the three, not the highest: on this host the page cache reading a 109.7 GB reference and a 42.07 GB dump off an external volume is charged to the process footprint once it is warm, which is the ~6 GB spread here and the ~17.7 GB spread recorded for Mimic itself at this scale (`docs/dev/POST-PHASE-5-WORK.md` §2.2: 34.445 then 16.752 GB, same binary and dataset). That is why a single cold figure is not evidence, in either direction. Both on-disk structures live under `TMPDIR`; set it to a volume with room for roughly 72 bytes per dumped halo before passing `--reference-topology` on a large dataset.
- **The cross-check is a micro-Uchuu-scale gate, not a production-scale instrument**, and the rehearsal subset is the largest scale it is ever run at: its reference side is a tree-ordered `halos-only` run over the same data, and the ctrees reader preallocates 152,000 B per tree for a whole forest before reading a halo (`src/io/tree/read_ctrees_ascii.c:647-655`), so the production super-forest's 104,845,278 tree roots alone put ≈15.9 TB of reader preallocation between it and a production reference artifact. No cross-check artifact belongs in a production conversion's storage envelope.
- **Still sized for the dataset:** the per-snapshot slabs the link algorithms, the battery and the cross-check each need — see the projection under "Storage envelope and the production memory term" below — and concurrent converter invocations on one workdir, which are not locked.

### Storage envelope and the production memory term

**Measured stage-by-stage on micro-Uchuu, 22,580,924 halos** (plan Slice 8). Peaks are a 1 Hz sampler's maximum over the workdir, so the fast stages carry a few B/halo of run-to-run jitter; the table quotes the maximum observed across all runs.

| Stage | flag OFF peak workdir | B/halo | flag ON peak workdir | B/halo |
|---|---:|---:|---:|---:|
| scatter (incl. finalize concat) | 2,527,916,819 | 111.95 | 2,528,011,965 | 111.96 |
| sort | 2,702,225,085 | 119.67 | 2,702,223,892 | 119.67 |
| fixups | 5,351,349,417 | 236.99 | 2,967,077,514 | 131.40 |
| links | 6,796,529,166 | **300.99** | 4,357,788,920 | **192.99** |
| write / report | (no new peak) | — | (no new peak) | — |
| terminal workdir intermediates | 6,254,645,786 | 276.99 | 22,310,275 | **0.99** |
| emitted dataset | 2,435,905,536 | 107.87 | 2,435,905,536 | 107.87 |
| **peak total (workdir + emitted)** | 8,690,538,506 | **384.86** | 4,357,788,920 | **192.99** |

**Production projection, 22.9 × 10⁹ halos, decimal TB.** Per-halo terms are the measured ones above, except that `emitted` uses the rehearsal-measured 100.7 B/halo rather than micro-Uchuu's 107.87 (small snapshots carry proportionally more HDF5 chunk overhead), and the rank spill uses 88.8 B/halo = the measured 48.00 × a 1.85 worst case, because at production the default budget makes the merge multi-pass rather than the single pass measured at micro-Uchuu scale.

| Stage, deletion ON | Coexisting terms (B/halo) | Projected peak |
|---|---|---:|
| scatter, last batch | worker scratch 108 + staged batch S + O(forests) tables ≈0.012 TB | **2.47 TB + S** |
| finalize | 111.96 (S already released) | 2.56 TB |
| sort | 119.67 | 2.74 TB |
| fixups | 131.40 | 3.01 TB |
| **links, rank pass** | fixed 120 + idx 8 + identity 16 + spill 88.8 = 232.8 | **5.33 TB** |
| links, linking phase | fixed 120 + links 36 + identity 16 + pending 4 = 176 | 4.03 TB |
| write | fixed 120 + links 36 + one snapshot's emitted ≈ 157.4 | 3.61 TB |
| report / validate | emitted 100.7 (+2.86 GB RAM bitset, 0 B disk) | 2.31 TB |

**The binding stage is scatter, through the staged source batch.** With `S ≤ 4.4 TB` the production peak is `2.47 + 4.4 + 0.012 =` **6.89 TB**, inside the 7.0 TB ceiling and 0.48 TB under the 7.37 TB volume. Every other stage peaks at or below 5.33 TB. Three preconditions come with it: **(a) deletion enabled** — with the flag off the measured peak is 384.86 B/halo, ≈8.65 TB at production before the staged batch, which exceeds the volume outright; **(b) a bounded staged source batch** — 4.4 TB is the maximum this envelope admits, so the 11.61 TB source needs at least three batches, and each released batch must be deleted before `sort` begins; **(c) cross-check artifacts excluded**, per the bullet above.

**Memory at production is now a per-snapshot term, not a per-dataset one, and that is measured rather than argued.** The link stage's peak RSS rose only 2.1–2.2× (4.55 → 9.76–10.01 GB) for 18.0× the halos between micro-Uchuu and the rehearsal subset, while B/halo fell 8.2–8.4× (201.5 → 24.0–24.6). **Do not scale 24 B/halo to production**: the term that scaled with total halo count is the one this pass moved to disk. Fitting the two measured points against the largest snapshot slab instead (621,360 halos at micro-Uchuu, 9,006,294 at the rehearsal) gives ≈620–650 B per largest-slab halo on top of a ≈4.2 GB floor, which at the joint review's projected production largest slab of ≈3.546 × 10⁸ halos puts the link stage at **≈225–235 GB** — inside the 512 GB machine, against the ≈4.30 TB the in-memory rank pass projected, but no longer negligible. That is a two-point extrapolation across two datasets that differ in more than slab size, so treat it as an order-of-magnitude bound and **re-derive it from the production conversion report's own per-snapshot counts before the production run**.

### The 2026-08-28 acceptance gate

The pass's acceptance gate is the full micro-Uchuu producer battery **and** the topology cross-check re-running green, on a dataset the rebuilt converter itself produced, plus a memory profile at rehearsal scale. Run on the Mac Studio M3 Ultra (32 cores, 512 GB) with Python 3.13.2, numpy 2.4.6, h5py 3.16.0, pandas 3.0.5:

- **A fresh end-to-end conversion in a new workdir** — `scatter` → `sort` → `fixups` → `links` → `write` → `report`, 119.65 / 15.35 / 13.37 / 38.69 / 10.09 / 6.36 s — reproduced the recorded totals exactly: **22,580,924 halos, 50 populated snapshots, 440,651 forests, `max_halo_rank_in_forest` = 350074**, 51 emitted files, report `validation PASS`.
- **The producer battery on that fresh dataset: all 15 checks PASS**, three runs, byte-identical stdout, 0.374–0.399 GB peak RSS.
- **The topology cross-check on that fresh dataset: all eight checks PASS** — `reference-sanity` plus the seven checks, including `topology-chains` against a 2.01 GB dump of all 22,580,924 halos — with zero mismatches, three runs, byte-identical stdout and `crosscheck_report.json`, 3.61–3.62 GB peak RSS.
- **Scatter throughput was 96.2 MB/s** (11,515,537,257 B in 119.65 s). It is **not** comparable to the rehearsal's pooled 39.0 MB/s: micro-Uchuu is a single source file, and one file takes the serial branch (`scatter.py:1071-1073`), so this measures the parse path rather than the pool. The pooled path's throughput is re-measured by the production transfer itself.
- The converter's own suite passes at that commit: **610 tests, exit 0** (`mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`), up from 327 at the 2026-08-03 re-validation.
- The rehearsal-scale memory profiles are the `links`, battery and cross-check figures quoted above.

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
