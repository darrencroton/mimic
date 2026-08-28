# Shin-Uchuu ctrees ASCII → Snapshot HDF5 Conversion Plan

**Status:** Converter built and micro-Uchuu-validated 2026-07-24 — the external converter described here exists under `scripts/convert/` and passed its micro-Uchuu acceptance gate (full pipeline over the real 22,580,924-halo / 50-snapshot ASCII data; producer validation battery + a seven-check cross-check against a `halos-only` reference run, topology-order proof fully discharged, zero unexplained mismatches). That gate was re-run end to end on 2026-08-03 on a fully regenerated micro-Uchuu dataset, now placed at `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-snapshot/` for snapshot-reader development: same three totals, producer battery 15/15, cross-check green including `topology-chains`. Remaining: the one-time Shin-Uchuu production conversion. **Its precondition is met — the dual-driver Phase 5 identity gate passed 2026-08-12** (both models, both timestep schemes, per-ID bitwise), so the production conversion is unblocked; see `POST-PHASE-5-WORK.md` §2 for the items to close first. All previously open design decisions resolved (joint plan review 2026-07-02, decisions D1–D12; review record archived at `archive/dev-plans/dual-driver-plan-review.md`). Earlier revision reviewed twice by Codex gpt-5.5 (2026-06-27).
**Date:** 2026-07-02 · **substantially revised 2026-08-25** — source data re-measured at the operative path (total size, row width and halo count all corrected), the conversion machine and storage layout decided and recorded, and the previously unspecified rehearsal subset selection designed. See "Source Data Summary" → measurement note, "Where The Work Runs", "Feasibility" and "Subset Selection and Extraction".
**Context:** This plan is one sequence with `MIMIC-DUAL-DRIVER-PLAN.md`. The converter is **not** blocked on the snapshot driver: it is blocked only on the frozen format contract, and it is built and validated first, against micro-Uchuu ASCII, using the existing tree-ordered `read_ctrees_ascii.c` reader as the reference — zero new Mimic code required. The full 11.61 TB Shin-Uchuu conversion runs exactly once, after the dual-driver Phase 5 identity gate is green (**green 2026-08-12**). Mimic itself performs no internal conversion.

---

## Problem Statement

The Shin-Uchuu merger trees are in Consistent-Trees ASCII format. **Operative location, verified 2026-08-20: `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` on OzSTAR** (`ssh dcroton@nt.swin.edu.au`) — 11.61 TB apparent (5.6 TB as `du` reports it; see the measurement note in Source Data Summary) across 2,744 `tree_*.dat` files plus `forests.list` and `locations.dat`, alongside the producer's own `shinuchuu.par`. This document previously recorded `/fred/oz004/simulations/uchuu_suite/shinuchuu/mergertrees`; that path was never verified in the 2026-08-20 sweep and should be treated as historical unless an operator confirms it is a live mirror. Use the oz214 path. Two structural problems prevent running Mimic's existing `consistent_trees_ascii` reader on this data:

1. **Percolation super-forest**: forest `26551468179` contains 104,845,278 tree roots — 33% of all trees — almost certainly a ctrees linking artifact. The tree driver must load a forest as a single in-memory unit; it cannot. This also rules out the uchuutools forests-HDF5 packaging.
2. **Index memory wall**: the ASCII reader loads a global `forests.list` index (315M entries) into every MPI rank at startup, costing ~18 GB per rank before a single halo is processed.

Both problems are structural consequences of forest-ordered processing and both disappear in the snapshot driver, which processes one snapshot's halo population at a time.

---

## Source Data Summary

| Parameter | Value |
|---|---|
| Format | Consistent-Trees ASCII |
| Total size | **11.61 TB** (10.56 TiB) across **2,744** `tree_*.dat` files — measured 2026-08-25, see the measurement note below |
| Largest file | 93.49 GB / 87.07 GiB (`tree_8_12_10.dat`); smallest 287.4 MB (`tree_2_7_0.dat`) |
| `locations.dat` | 13.75 GB (symlink to `locations_no_extra_columns.dat`), 315,004,242 rows at 43.6 B/row |
| `forests.list` | **7.56 GB**, 315,004,242 lines at exactly 24.0 B/line |
| Total halos (z=0) | 315,004,242 (= the tree count; every tree root is a z=0 halo) |
| Total forests | 166,547,771 |
| Mean ASCII data-row width | **506.3 B** — measured over 240 MB sampled at five depths in each of 12 randomly chosen files (474,031 rows); three whole-file midpoint samples gave 505.4 / 508.3 / 512.9 B |
| Estimated total halos (all snapshots) | **≈22.9 billion** (11.61 TB ÷ 506.3 B/row; the ~315M `#tree` marker lines contribute ~5.7 GB, immaterial). ≈72.8 halos per tree |
| Snapshots | 70 (a = 0.04773 to 0.99998) |
| Box size | 140 Mpc/h |
| Particle mass | **8.97 × 10⁵ Msun/h** — confirmed 2026-08-14 from Ishiyama et al. 2021, the Uchuu suite paper ([arXiv:2007.14720](https://arxiv.org/abs/2007.14720)): "262 billion (6400³) particles in a box of side-length 140 Mpc/h, with particle mass 8.97 × 10⁵ M☉/h". **Corrects the value previously recorded here as ~8.97 × 10⁴, which was low by exactly a factor of 10** and had propagated into two derived claims elsewhere. Independently cross-checked against this table's own 140 Mpc/h box and the package cosmology (Ω_m = 0.3089): Ω_m ρ_crit L³ / N = 0.3089 × 2.77537 × 10¹¹ × 140³ / 6400³ = 8.97 × 10⁵ Msun/h. `Len` derives from this, so it is now fixed rather than inferred |
| Cosmology | Planck 2015 (Ωm=0.3089, h=0.6774) — identical to rest of Uchuu suite |

**Measurement note, 2026-08-25 — three of the figures above were wrong, and two of them matter.** All values in this table were re-measured directly against `/fred/oz214/simulations/uchuu/shinuchuu/` on the dates shown; the superseded figures are recorded here because downstream projections were derived from them.

- **Total size was recorded as 5.6 TB; the apparent size is 11.61 TB.** Both numbers are real and describe the same data: `du -sh` reports **5.6 T** and `du -sh --apparent-size` reports **11 T**, because this Lustre filesystem under-reports `st_blocks` (the largest file stats at 93.49 GB apparent against 50.26 GB of reported 512-B blocks, a ratio near 0.54 across files of very different sizes). **The apparent size is the operative one**: it is what `read()`, `rsync` and the parser see. The data was checked for genuine sparseness and is dense — 60 probes of 4 MB each, at five depths in each of 12 randomly chosen files, returned **zero NUL bytes** and a uniform ~506 B row width throughout. Every byte-count, transfer-time and parse-time estimate must use 11.61 TB.
- **`forests.list` was recorded as 17 GB; it is 7.56 GB.** This one is a transcription error with no downstream effect, and its own arithmetic confirms the line count that *does* matter: 7,560,101,829 B ÷ 315,004,242 lines = exactly 24.0 B/line, the width of a `<11-digit id> <11-digit id>\n` row. That corroborates the **315,004,242 tree count** — one row per tree — and nothing more. The **166,547,771 forest count is a count of distinct forest IDs, which no row-width arithmetic can confirm**; it remains inherited and is measured for the first time by Step 1 of subset selection.
- **Total halos were estimated at 15–18 billion from an assumed ~350 B/line; the measured width is 506.3 B and the count is ≈22.9 billion.** Roughly +30% on the figure that drives the output size, the scratch budget, the rank-pass memory and the wall clock. The Feasibility section below has been recomputed from 22.9 billion throughout; anything elsewhere in `docs/dev/` still reasoning from 15–18 billion is stale.

**Where the source data lives, and the one thing it is not.** `ssh dcroton@nt.swin.edu.au` reaches `tooarrana1`, a **data node**: 251 GB RAM, 4 cores, no Slurm, `/fred` writable with no user quota. It holds the data and nothing else — it is not a candidate conversion machine. Exactly two plan steps run there, both streaming and both sub-1 GB: the root-row sampling (`sample-roots`) and the byte-range extraction (`extract`); see "Subset Selection and Extraction". The conversion machine is the Mac Studio; see "Where the work runs" below.

---

## Target Format: Snapshot-Ordered HDF5

One HDF5 file per snapshot, named `snapshot_000.h5` through `snapshot_069.h5` (per-snapshot files are decided, not open: partial recovery, per-snapshot parallelism, and the driver's access pattern all favour them). All topology links are snapshot-local integer indices (no global IDs). Scalar metadata lives in HDF5 **attributes** on the `/header` group. **Field names and types on disk must match what `simulations/shin-uchuu/halo_properties.yaml` declares** — except `ForestIndex` and `HaloRankInForest`, which are exempt (see the package section below and `SNAPSHOT-HDF5-FORMAT.md` errata 2026-08-11) — so the generated `RawHalo`/accessors consume the file directly; the names below already match the existing `micro-uchuu-ascii` bridge contract (`M_Crit200`→`HaloMass`, `Len`, `SnapNum`, `MostBoundID`, spin conventions). The contract is **frozen** at [`docs/dev/SNAPSHOT-HDF5-FORMAT.md`](SNAPSHOT-HDF5-FORMAT.md) (`format_version = 1`, 2026-07-18), which is now authoritative; this section remains as the working draft it was promoted from — if they ever disagree, the spec wins.

```text
snapshot_NNN.h5
  /header                             HDF5 group
    format_version        int32       attribute; contract version, validated by the reader
    links_adjacent        int32       attribute; always 1 — the adjacency invariant is part
                                      of the format, asserted by converter and reader
    scale_factor          float64     attribute
    snapshot_number       int32       attribute
    n_halos               int64       attribute
    n_forests_total       int64       attribute; run-scoped forest count (identity bound check)
    max_halo_rank_in_forest int64     attribute; run-scoped max rank (identity bound check)
    box_size_mpc_h        float64     attribute
    particle_mass_msun_h  float64     attribute
    omega_matter          float64     attribute
    omega_lambda          float64     attribute
    hubble_h              float64     attribute
  /halos/
    Descendant            int32[N]    index in snapshot N+1 file; -1 if none. Not consumed by
                                      the driver (gather uses FirstProgenitor/NextProgenitor);
                                      kept as the round-trip validation key
    FirstProgenitor       int32[N]    index in snapshot N-1 file of main progenitor; -1 if none
    NextProgenitor        int32[N]    index in THIS snapshot (N) of the next sibling
                                      progenitor (sharing the same descendant as this halo);
                                      -1 if no next sibling. This is a same-file index.
    FirstHaloInFOFgroup   int32[N]    index in this snapshot of the FoF central; self-index
                                      for the central halo
    NextHaloInFOFgroup    int32[N]    index in this snapshot of the next FoF member; -1 if last
    Len                   int32[N]    round(Mvir_native * 1e-10 / PartMass); required core role
    SnapNum               int32[N]    snapshot index, matching the file header snapshot_number
                                      (decision closed 2026-08-04 when the reader shipped:
                                      SnapNum is a required, read catalog field. Changing that
                                      now would bump format_version)
    M_Crit200             float32[N]  native Msun/h (generated accessor converts to 1e10 Msun/h)
    Pos                   float32[N,3] Mpc/h comoving
    Vel                   float32[N,3] km/s peculiar
    Spin                  float32[N,3] Mpc/h km/s; J/Mvir (applied during conversion)
    VelDisp               float32[N]  km/s
    Vmax                  float32[N]  km/s
    MostBoundID           int64[N]    ctrees halo ID; negated for flyby-demoted halos
                                      (reference semantics are replicated — see Phase 3)
    ForestIndex           int64[N]    dense run-scoped forest number; identity component
                                      consumed directly by UniqueGalaxyID (no runtime
                                      id→index mapping in Mimic). Must replicate the ASCII
                                      reader's run-scoped forest enumeration order (verify
                                      against forest_utils.c; the micro-Uchuu identity gate
                                      depends on it)
    HaloRankInForest      int64[N]    per-forest halo index in reference tree-driver order
                                      (int64: super-forest ranks exceed int32); identity
                                      component for UniqueGalaxyID

forests.h5 (run-level sidecar, written once)
  /ForestID               int64[n_forests_total]   dense ForestIndex → original ctrees
                                                   forest ID (provenance/debugging only;
                                                   Mimic never reads it)
```

**Link scope — the reader must open the correct file for each link type:**

| Link field | Points into |
|---|---|
| `Descendant` | snapshot N+1 file (validation only) |
| `FirstProgenitor` | snapshot N-1 file |
| `NextProgenitor` | snapshot N file (same file) |
| `FirstHaloInFOFgroup` | snapshot N file (same file) |
| `NextHaloInFOFgroup` | snapshot N file (same file) |

Topology links are int32; assert at conversion time that no snapshot exceeds 2,147,483,647 halos. Mimic-side reader/driver internals nevertheless use `int64_t` indices and counts.

Do **not** use gzip or any HDF5 compression for production files — chunked uncompressed SOA datasets are faster for slab reads. Use `chunks=(65536,)` for 1D arrays, `chunks=(65536, 3)` for vectors.

**Adjacency is an invariant, not a policy.** ctrees inserts its own phantom halos, so every descendant link in the source is one snapshot forward by construction. The converter asserts this and aborts on violation — a violation means corrupt input. There is no phantom/bridge insertion anywhere in this pipeline.

---

## Where The Work Runs, And Getting The Data There

**Decided 2026-08-25, and it is not a preference — it is the only machine that fits.** Every step of this plan that needs memory or CPU — the whole conversion, every Mimic run, and all subset planning — happens on the **Mac Studio (M3 Ultra, 32 cores, 512 GB unified memory)**. `tooarrana1` holds the source data but has 251 GB of RAM and 4 cores, roughly half what the rank pass and the snapshot driver need at production scale, so it is a **source and streaming host only**. Exactly **two** plan steps execute remotely, both described under "Subset Selection and Extraction", both streaming and both under 1 GB: the root-row sampling (`sample-roots`) and the byte-range extraction (`extract`). Nothing else does.

This supersedes the assumption in `POST-PHASE-5-WORK.md` §2.2's platform audit that the rehearsal would run on Linux. It runs on macOS, where `run_profile_peak_rss_bytes()` takes the **bytes** branch of `ru_maxrss` — the branch every recorded measurement to date has used and the one already cross-checked byte-exact against `/usr/bin/time -l`. The audit's Linux `× 1024` confirmation stays valid as evidence about the instrument; it is simply no longer the branch in play.

### Local storage

The Mac Studio's internal APFS container is **3.6 TB with ~142 GB free**, not the 8 TB this plan previously assumed — `/Volumes/Internal` already holds 2.7 TB of other data. Conversion storage is therefore external:

| Volume | Free | Role |
|---|---|---|
| `/Volumes/LaCie` | 7.3 TB | **Primary scratch and output.** Case-sensitive APFS, USB |
| `/Volumes/Scratch` | 2.7 TB | **Overflow only**, if the converter's terminal accumulation is not fixed first (see below) |
| `/Volumes/Internal` | 142 GB | Existing datasets (micro-Uchuu, mini-Uchuu). **Not** available for Shin-Uchuu |

**Is 7.3 TB enough? Not as the converter stands, and the reason is a defect rather than a budget.** Deletion stops after the concat stage: `scatter.py` removes its verified worker parts (`scatter.py:753-780`) and `sort_index.py` removes the unsorted scratch once the sort verifies (`sort_index.py:116`, `:129`), but `fixups.py`, `links.py` and `hdf5_writer.py` delete nothing — `fixups.py:573-580` registers the new `fixed` artifact and retains its `sorted` input — so at the end of Phase 4 the workdir still holds every intermediate at once — **≈8.60 TB** at the corrected 22.9-billion-halo scale (see the Disk table). That exceeds LaCie alone and fits only by spanning both external volumes, with ~1.4 TB of margin on a one-shot run. **Adding consumptive deletes to the fixups, links and write stages drops the peak to ≈6.0 TB and makes LaCie sufficient on its own**, which is why it is now scoped into the converter scale-engineering pass (D4) below rather than left to operator vigilance.

### Remote working and output space on OzSTAR

**Decided 2026-08-26, and constrained by permissions rather than preference.** The converted dataset is produced on the Mac Studio and must stay there for the production `sage16` run, which projects to ≈476.6 GB of peak RSS against `tooarrana1`'s 251 GB. Archiving a copy back to OzSTAR afterwards is worthwhile — it is durable, shareable, and ≈2.29 TB at the measured ≈110 MB/s is ≈5.8 h in the background — but it is a **post-P6 step**, not an alternative to holding the dataset locally.

| Path | Role |
|---|---|
| `/fred/oz214/dcroton/shin-uchuu/working/` | Working data for the production conversion |
| `/fred/oz214/dcroton/shin-uchuu/snapshot-trees/` | The archived converted snapshot trees |

**These are not beside the source, and that is a permission constraint.** The natural home — `/fred/oz214/simulations/uchuu/shinuchuu/`, parallel to `mergertrees/` — is owned by `msinha` with group `oz214` at mode `drwxr-sr-x`: the group has read and execute but **no write**, and a real `mkdir` there fails with `Permission denied`. Placing the converted trees beside the source requires the directory owner or an `oz214` administrator to grant write access or create the directories; that is an operator action, and there is time to arrange it because the archive step follows the production run.

### Transfer

**Measured 2026-08-25, single ssh stream from `tooarrana1`:** 287 MB in 2.66 s (108 MB/s) and 4 GiB in 37.5 s (**114.5 MB/s**). A rough four-stream test moved 4.84 GB in 39.2 s (123 MB/s) but two streams ran short, so treat parallelism as offering **no demonstrated gain** until measured properly. Use **≈110 MB/s** for planning — at the top of the 50–100 MB/s this plan previously assumed, which partly offsets the doubled data volume.

At 110 MB/s the full 11.61 TB is **≈29 hours** of transfer. The batched, consumptive strategy is unchanged and still right:

1. Fetch `.dat` files in batches with `rsync --checksum` (the 93.5 GB largest file is a single-batch item).
2. Phase 1 scatters each fetched file, then **deletes the local ASCII copy**.
3. A resume manifest records per-file completion: name, size, halo count, checksum. Re-running skips completed files; a crashed batch re-fetches cleanly.

**But note the standing incompatibility, unchanged and still open:** `run_scatter` requires every listed source file to exist at start, freezes the ordered source set into the manifest, and `source_completed()` re-stats completed files — so consumptive deletes break resume today. That is D4's batch-aware scatter inventory, and it is a hard prerequisite for this transfer strategy, not an optimisation.

Fallback if throughput degrades: run Phase 1 scatter on the source side and transfer the ~2.5 TB of scratch instead (4.7× less data, at the cost of a second execution environment) — **except that `tooarrana1` has neither the memory nor the cores to do it well**, so this fallback is weaker than it was when the plan assumed a capable remote host. Not the default.

---

## Algorithm

The conversion is an external sort over the snapshot dimension: forest-ordered ASCII → snapshot-ordered HDF5. Pipeline: **scatter → sort/index → remap → write**. (Directly streaming trees into final snapshot files cannot work alone: every link field is a snapshot-local index that does not exist until the destination slab's order is fixed, and FoF/flyby fixes need the whole forest-at-snapshot population visible. Scatter-then-finalize is the robust realization of the streaming idea.)

### Phase 0: Provenance pre-pass

Stream `forests.list` once to build the tree-root-id → forest-id map (315M × 16 B as sorted arrays ≈ 5 GB, in-memory), and assign each forest its dense run-scoped `ForestIndex`. **The enumeration rule is ascending ctrees forest id** — verified against the reader: the ASCII reader sorts tree locations by `(forestid, fileid, offset)` and groups forests from that sorted order (`ctrees_utils.c:270-297`, consumed in `read_ctrees_ascii.c`), so the run-scoped dense forest order that defines `forestnr_global` is ascending forest id. The micro-Uchuu cross-check re-confirms this end to end. Write the dense-index → ctrees-forest-id table to the `forests.h5` sidecar. This pre-pass is required: identity fields cannot be produced without it.

### Phase 1: Scatter (ASCII → per-snapshot binary)

Stream-read all 2,744 ctrees ASCII files using a bounded worker pool. `locations.dat` is never needed.

**Parsing ctrees ASCII correctly**: ctrees files contain a header line and `#tree <id>` block markers. Do not use `comment='#'` with pandas default `header='infer'`. **Erratum (2026-07-18, verified against `tree_0_0_0.dat`):** the Uchuu-suite files use an **indexed** first-line header — `#scale(0) id(1) desc_scale(2) … Snap_num(31) …` — with `(N)` column-number suffixes and mixed case, not a `#fields:` line; the reference parser strips the suffixes and matches names case-insensitively (`src/io/tree/ctrees/parse_ctrees.h`). Correct approach:
1. Read the first header line; strip `(N)` suffixes; match column names case-insensitively (support a `#fields:` dialect as secondary if encountered).
2. Use `pd.read_csv(..., header=None, names=<derived_names>, sep=r'\s+', comment='#', dtype=<pre_specified>)` so `#tree` markers and the field header are skipped uniformly — but track `#tree <id>` boundaries separately (a light pre-scan of marker byte offsets per chunk) because every halo record must carry its tree root id.
3. Validate that parsed row count plausibly matches file size before continuing.

**Snapshot column**: ctrees files use either `snap_idx` (newer) or `snap_num` (older). Check both — matching `read_ctrees_ascii.c`'s `setup_column_info()`.

**Per-halo record written to scratch** (~108 bytes): the extraction table below **plus** `tree_root_id` (int64, from the `#tree` marker) and `forest_id` (int64, joined from the Phase 0 map — or joined later from `tree_root_id`; joining during scatter keeps Phase 3 simpler).

| ctrees column | Type | Bytes | Purpose |
|---|---|---|---|
| `id` | int64 | 8 | link key; becomes MostBoundID |
| `desc_id` | int64 | 8 | descendant link key |
| `desc_scale` | float64 | 8 | validate adjacency (tolerance 1e-4) |
| `pid` | int64 | 8 | immediate FoF host ID (−1 if central) |
| `upid` | int64 | 8 | ultimate host ID |
| `snap_idx` / `snap_num` | int32 | 4 | partition key |
| `Mvir` | float32 | 4 | HaloMass, Len, Spin normalisation |
| `X`, `Y`, `Z` | float32×3 | 12 | position |
| `VX`, `VY`, `VZ` | float32×3 | 12 | velocity |
| `Jx`, `Jy`, `Jz` | float32×3 | 12 | angular momentum (raw; Spin = J/Mvir in Phase 3) |
| `vrms` | float32 | 4 | VelDisp |
| `vmax` | float32 | 4 | Vmax |
| `tree_root_id` | int64 | 8 | provenance (from `#tree` marker) |
| `forest_id` | int64 | 8 | provenance (Phase 0 join) |
| **Total** | | **108 bytes packed** (erratum 2026-07-18: earlier "~116" total was arithmetic drift; the frozen dtype now lives in the converter code, `scripts/convert/ctrees_parser.py` `RECORD_DTYPE`, which is authoritative) | |

**Worker isolation**: bounded process pool (8–16 workers), each writing per-snapshot scratch files (`scratch/snap_NNN.worker_K.bin`). After all workers finish, concatenate per-snapshot worker files into `scratch/snap_NNN.bin` — a sequential read+write of ~1.9–2.2 TB, ~20–40 minutes at 5 GB/s. The simpler worker-files + concat approach is chosen over the memory-mapped SOA alternative: concat costs ~30 minutes of sequential I/O and has near-zero failure surface, and time is not the binding constraint.

**Fallback**: if pandas throughput is below ~500 MB/s effective, replace the tokeniser with a compiled C parser (same worker interface, Python remainder unaffected).

**Output:** 70 per-snapshot scratch files, ~1.9–2.2 TB total, plus per-forest aggregates needed by Phase 3 (per-forest max snapshot, accumulated during scatter or in a cheap pass over scratch).

### Phase 2: Sort and index

For each of 70 snapshot files independently (trivially parallelisable):
- Load `scratch/snap_NNN.bin` into a NumPy structured array
- Assert all `id` values are unique within this snapshot; abort with examples if not
- Sort by `id` (int64, ascending) — deterministic, reproducible ordering
- Write sorted array to `scratch/snap_NNN_sorted.bin`
- Write index file `scratch/snap_NNN.idx`: the sorted `id` array as int64 (for merge-join in Phase 3)
- **Verify the sorted file (row count + id checksum), then delete `scratch/snap_NNN.bin`.** Sorted scratch replaces unsorted scratch; keeping both would add ~2 TB and blow the disk budget.

z=0 snapshot: 315M halos × ~116 bytes ≈ 37 GB. Sort temporaries add ~1× → ~74 GB peak for this step alone.

### Phase 3: Link remapping (global IDs → snapshot-local indices)

Process snapshots in **forward time order** (snap 0 → snap 69). `FirstProgenitor`/`NextProgenitor` for snap N+1 halos are computed while snap N is resident; maintain a persistent pending buffer.

**Pass structure per snapshot N:**

```text
Load snap_N sorted data. Load snap_N.idx.

=== 1. Adjacency validation ===
For all halos with desc_id != −1:
    Find desc_scale in the a_list using absolute tolerance 1e-4.
    Assert desc_snap == snap_N + 1.
    Any violation: report count + examples, ABORT. (ctrees guarantees adjacency
    via its own phantoms; a violation means corrupt input, and there is no
    repair policy by design.)
Halos at snap 69: assert all desc_id == −1.

=== 2. Spin normalisation (matching apply_ctrees_value_conventions exactly) ===
For each halo where Mvir != 0.0:
    Spin[k] = J[k] / Mvir    (native Msun/h; no r_vir factor)
For halos where Mvir == 0.0:
    leave Spin unchanged     (match reference: do NOT zero it)

=== 3. Len derivation ===
Len = round(Mvir_native * 1e-10 / PartMass)   (PartMass is in 1e10 Msun/h)
Halos where Len == 0: log count; preserve zero rather than asserting > 0
(reference reader allows zero; core treats zero as orphan sentinel).

=== 4. fix_flybys equivalent (reference: ctrees_utils.c:335-409) ===
RESOLVED (D12): reference semantics are replicated exactly. Ordering is
verified against the code: the reference runs fix_flybys FIRST, then
fix_upid, then assign_mergertree_indices (read_ctrees_ascii.c:692-700), so
this step runs before the upid resolution below. Scope is per forest at
that FOREST'S maximum scale factor — snap 69 for almost all forests, but
NOT for forests whose branches all die early. The reference algorithm
(verified at ctrees_utils.c:376-409) makes ONE central per forest at max
scale, not one per flyby group:

For each forest F whose max snapshot == N (per-forest max-snapshot table
from Phase 1), over F's halos at snap N:
  Count halos with pid == -1. If <= 1: nothing to do.
  Else:
      Pick the most massive pid==-1 halo by Mvir as the sole surviving
      central (strict >, so ties go to the first encountered; the reference
      scans its scale-desc/id-sorted forest array, which within one
      snapshot is ascending id — the id-sorted slab order matches this).
      For EVERY other halo of F at snap N (centrals AND satellites):
          set upid = chosen central's ctrees id
      Additionally, for each demoted central (pid was -1):
          set pid = chosen central's ctrees id
          negate its MostBoundID (ctrees_utils.c:395-400)
  Log flyby halo counts per snapshot.

=== 5. fix_upid equivalent (reference: ctrees_utils.c:419-482) ===
Runs AFTER fix_flybys, matching the reference order.
  a. For halos with pid == -1: set upid = id (these are FoF centrals).
  b. For halos with pid != -1: follow upid chain (depth limit 30, matching
     the reference hard limit). At each step look up the current upid target
     in snap_N.idx. If the target is not found within this snapshot: fall
     back to following pid (matching reference fallback at
     ctrees_utils.c:457-482). If still not found: abort with examples.
  c. Resolved upid is the ultimate FoF group owner for this halo.

=== 6. FoF group links (matching assign_mergertree_indices reference order) ===
Reference sort key before list construction (ctrees_utils.c:524-547):
    primary: descending scale factor (constant within a slab)
    secondary: upid; tertiary: pid; quaternary: ascending id
Replicate this sort for the NextHaloInFOFgroup chain order — required for
cross-format identity (workspace order and central selection depend on it).

For each group (same resolved upid):
    Central: the halo with upid == id (pid == -1)
    All members → FirstHaloInFOFgroup = local index of central (self-ref for central)
    Satellites sorted per reference key → NextHaloInFOFgroup linked list ending at -1
Isolated halos: FirstHaloInFOFgroup = self, NextHaloInFOFgroup = -1

=== 7. Descendant links: snap_N → snap_N+1 ===
Load snap_N+1.idx (sorted int64 id array).
Merge-join snap_N.id (sorted) with snap_N+1.idx on desc_id:
    for each snap_N halo with desc_id != -1:
        pos = searchsorted(snap_N+1.idx, desc_id)
        if pos >= len(snap_N+1.idx) or snap_N+1.idx[pos] != desc_id:
            ABORT with examples — a desc_id with no target at snap N+1 is
            corrupt input under the adjacency invariant, exactly like a
            non-adjacent desc_scale. Never silently rewrite to -1: that
            would drop a merger link and still pass later validation.
        else:
            Descendant = pos
    for snap_N halos with desc_id == -1: Descendant = -1

=== 8. Progenitor links: snap_N+1 ← snap_N ===
For each snap_N+1 halo D (by local index):
    collect all snap_N halos whose Descendant == D's index
    if none: FirstProgenitor[D] = -1
    else:
        find max-Mvir progenitor → P_main (tie-break: encounter order, first
        wins — verify against reference)
        set FirstProgenitor[D] = P_main's local index in snap_N
        remaining progenitors: promote max-Mvir to front, append remainder in
        encounter order to tail — NOT fully mass-sorted (reference at
        ctrees_utils.c:667-705)
        build NextProgenitor chain within snap_N (same-file indices)
    ERRATUM (2026-07-24): "promote max-Mvir to front, append remainder in
    encounter order" is an imprecise paraphrase. The reference builds the chain
    by a LITERAL incremental insertion loop (ctrees_utils.c:667-706): each
    progenitor, in reference encounter order, either replaces the current head
    when its Mvir is STRICTLY greater (demoting the old head to second place) or
    is appended at the tail. When a mid-chain head replacement occurs (3+
    progenitors), the resulting NextProgenitor order is NOT the remaining
    progenitors in encounter order. The converter (links.py) and the reference
    reader both implement the literal loop; the cross-check's topology-chains
    proof confirms the order matches exactly. The frozen spec's Ordering
    Contract item 1 (docs/dev/SNAPSHOT-HDF5-FORMAT.md) has been corrected to describe
    this loop precisely; the same paraphrase also appeared in the converter
    implementation plan, now archived under archive/dev-plans/.
IDENTITY TRAP — "encounter order" is NOT slab order. Slabs are id-sorted
(Phase 2), but the reference encounters halos in its forest sort order,
which within one snapshot reduces to (upid, pid, ascending id) — the same
key step 6 already uses for FoF chains. Enumerate progenitors in that
reference order when building chains; getting this wrong silently reorders
the inheritance workspace (merger processing order, SAGE-parity tie-breaks)
and fails the identity gate. Verify the exact key against
ctrees_utils.c:524-547 and 667-705 during implementation.
Store snap_N+1's FirstProgenitor values in the pending buffer.

=== 9. Identity fields ===
ForestIndex[i]: dense run-scoped index, carried from scatter (Phase 0/1 join).
HaloRankInForest[i]: the within-forest index in REFERENCE tree-driver order —
the assign_mergertree_indices sort (descending scale, upid, pid, ascending
id) applied per forest, using the POST-fix upid/pid values (the reference
sorts after fix_flybys/fix_upid have rewritten them). This order is
well-defined for every forest, including the super-forest the tree driver
can never load. Computed in a dedicated pass (see below), not per-snapshot.
Assert (ForestIndex, HaloRankInForest) pairs are globally unique after the join.

=== 10. Write snapshot_NNN.h5 ===
Apply pending FirstProgenitor values (computed during snap N-1 processing).
Write all fields in the target schema; stamp header attributes including
format_version and links_adjacent.
Release snap_N arrays; retain snap_N.idx until snap_N+1 completes.
```

**Rank pass (between Phase 2 and Phase 3, or fused into Phase 3 bookkeeping):** ranks are per-forest over all snapshots, so they need a forest-major view once. For ordinary forests this is cheap grouping. For the super-forest (~7–8 billion halos at the measured 22.9-billion total), it is one large deterministic sort of ~(scale, upid, pid, id, slab-slot) keys — ~150–250 GB of key data: in-RAM on this machine or a chunked external merge sort on scratch. **Implemented 2026-08-28 by the converter scale-engineering pass** (`CONVERTER-SCALE-PASS-PLAN.md` Slices 4–5). Until then the shipped `compute_identity()` (`scripts/convert/links.py`) concatenated and lexsorted the key columns over *all* snapshots globally — **~1.10 TB** analytically at the measured production scale, and **≈4.30 TB** once the rehearsal measured it at 187.84 B/halo. It now ranks through the external merge sort in `scripts/convert/rank_sort.py` under `--memory-budget-mb` (default 2 GiB), spilling sorted runs to scratch and keeping `(ForestIndex, HaloRankInForest)` in on-disk arrays: measured **9.76–10.01 GB peak RSS on the 406,668,896-halo rehearsal subset**, emitting link-stage artifacts md5-identical to the pre-change ones. See the "Pre-conversion obligation" subsection. It runs once, its output is the `HaloRankInForest` column plus the run-scoped `max_halo_rank_in_forest`/`n_forests_total` header values, and it is the direct input to setting the shin-uchuu identity multiplier (see below).

**Memory at peak** (snap 68 processing, which loads snap 68 + snap 69 index):
- snap_68 sorted data: ~33 GB
- snap_69.idx: 315M × 8 bytes = 2.5 GB
- snap_68 working arrays (sort keys, resolved upid, groupby, progenitor inversion): ~2× data ≈ 66 GB
- snap_69 FirstProgenitor pending buffer: 315M × 4 bytes = 1.3 GB
- HDF5 write buffer: ~2 GB
- **Realistic peak: ~140–170 GB** — comfortable within 512 GB. The super-forest rank sort peaks separately at ~150–250 GB *as designed*, also within budget, and since 2026-08-28 the shipped rank pass is bounded below that by construction: it is an external merge sort under an explicit memory budget rather than a global lexsort (see the "Pre-conversion obligation" subsection). **The per-snapshot window in this bullet is now the binding memory term for the link stage**, and the measured two-point projection puts it at ≈225–235 GB at the projected production largest slab — the same order as the ~140–170 GB estimated here, and to be re-derived from the production conversion report's own per-snapshot counts before the run.

### Phase 4: Validation

1. **Halo count**: sum of `n_halos` attributes across 70 files must equal the Phase 1 scatter total (which must match the manifest totals)
2. **Adjacency**: all non-null `Descendant` links point exactly one snapshot forward
3. **Progenitor round-trip**: every `FirstProgenitor != -1` has a `Descendant` back to the source halo
4. **FoF chains**: no cycles; all `NextHaloInFOFgroup` chains terminate at -1; all `FirstHaloInFOFgroup` self-reference centrals
5. **NextProgenitor scope**: all non-null `NextProgenitor` values are valid indices within the same snapshot file
6. **Identity**: `(ForestIndex, HaloRankInForest)` unique globally; ranks dense per forest; header identity bounds match measured maxima; sidecar `forests.h5` row count equals `n_forests_total`
7. **Len**: zero count logged; no negative values
8. **Topology cross-check on micro-Uchuu**: apply the converter to `micro-uchuu-ascii` (local, 11 GB, existing package). Compare FoF central assignments, progenitor structure, and value conventions against the existing `read_ctrees_ascii.c` reader output, by stable halo identity (ctrees id), not raw local index. This is the converter's acceptance gate and runs **before** any Mimic snapshot code exists.

**Conversion report (durable artifact):** total halos, per-snapshot counts, forest count, measured max `HaloRankInForest`, flyby counts, Len-zero counts, validation outcomes, and the recommended identity multiplier. The `simulations/shin-uchuu/` package sets its `UniqueGalaxyID` multiplier (dual-driver decision D9: per-simulation metadata; expected 10¹⁰) from this report, never from an assumption.

---

## Feasibility: Recomputed 2026-08-25 Against Measured Inputs

**Hardware:** Mac Studio M3 Ultra, 32 cores, 512 GB unified memory; external storage per "Where The Work Runs" above. **These tables were recomputed from N = 22.9 × 10⁹ halos** (the measured count, ~30% above the superseded 15–18 billion) and from the converter's **actual** on-disk record sizes read out of the code rather than estimated:

| Stage artifact | Source of truth | Bytes/halo |
|---|---|---|
| `snap_NNN.bin`, `snap_NNN_sorted.bin` | `RECORD_DTYPE`, `ctrees_parser.py:30` | 108 |
| `snap_NNN.idx` | int64 halo id | 8 |
| `snap_NNN_fixed.bin` | `FIXED_RECORD_DTYPE`, `fixups.py:47` | 120 |
| `snap_NNN_links.bin` | `LINKS_RECORD_DTYPE`, `links.py:67` | 36 |
| `snap_NNN_pending_fp.bin` | int32 | 4 |
| `snapshot_NNN.h5` `/halos` | `SNAPSHOT-HDF5-FORMAT.md`, 16 datasets | **100 exactly** |

### Memory

| Phase | Working set | Notes |
|---|---|---|
| Phase 0 pre-pass | ~10–15 GB | `forests.list` as sorted arrays; 315M rows |
| Phase 1 scatter | ~15–25 GB | Bounded pool × per-worker parse state + write buffers |
| Phase 2 sort | ~110 GB (peak, largest snapshot) | Scales with the largest slab, not the total |
| **Rank pass** | **~1.10 TB as implemented** | 5 concatenated int64 columns over *all* snapshots (916 GB) + the lexsort order array (183 GB). **Over installed RAM by 2.1×** |
| Phase 3 remap | ~200–250 GB (peak, snaps 68+69) | Two snapshots + working arrays + pending buffer |
| Phase 4 validate | ~60 GB | Full-dataset-resident battery, one snapshot at a time only after D4 |

**The rank pass is the binding constraint and it got worse, not better.** The superseded figure was 600–720 GB against 512 GB; recomputed at 22.9 billion halos it is **~1.10 TB**. This is not a margin question — the external-merge rank sort in D4 is mandatory, and the measurement strengthens rather than merely confirms that conclusion. Note the subset rehearsal is unaffected: at its ~3.6 × 10⁸ halos the same in-memory pass needs ~17.5 GB, so **the rehearsal can run on the shipped converter with no D4 work at all**, which is exactly what D9's ordering assumed.

### Disk

At N = 22.9 × 10⁹, on the external volumes described above:

| Artifact | Size | Deleted when consumed? |
|---|---|---|
| Source ASCII (in-flight batches only) | ~100–200 GB at a time | yes, by the transfer strategy |
| Phase 1 worker files (pre-concat, transient) | ~2.47 TB | yes, at concat |
| `snap_NNN.bin` (concatenated) | 2.47 TB | **yes** — `sort_index.py` removes it after the sort verifies |
| `snap_NNN_sorted.bin` | 2.47 TB | **no** |
| `snap_NNN.idx` | 0.18 TB | **no** |
| `snap_NNN_fixed.bin` | 2.75 TB | **no** |
| `snap_NNN_links.bin` | 0.82 TB | **no** |
| `snap_NNN_pending_fp.bin` | 0.09 TB | **no** |
| Output HDF5 (uncompressed, incl. identity columns) | **2.29 TB** | — |
| **Phase 1 peak** | **≈4.95 TB** | worker files alongside their concat output |
| **Terminal accumulation, as the converter stands** | **≈8.60 TB** | LaCie alone is insufficient; needs LaCie + Scratch |
| **Terminal accumulation, with D4 consumptive deletes** | **≈6.04 TB** | fits LaCie alone with ~1.3 TB spare |

### Time

| Phase | Bottleneck | Estimate |
|---|---|---|
| Transfer (batched rsync, overlapped with Phase 1) | ssh throughput, **measured 110 MB/s** | **~29 h** |
| Phase 1 ASCII parse | Tokenisation over 11.61 TB | 4–12 h compute |
| Phase 1 concat | External SSD write | 1–2 h |
| Phase 2 sort + index | I/O ~5 TB | 1–2 h |
| Rank pass | External-merge sort over 22.9e9 keys (post-D4) | 3–8 h |
| Phase 3 remap + write | Merge-join + FoF + inversion + I/O | 4–8 h |
| Phase 4 validate | Sequential reads over 2.29 TB | 2–4 h |
| **Total wall clock** | Transfer-dominated | **~2–4 days** |

Acceptable by design: the pipeline optimises for restartability and few failure points, not speed. **Every row above is a projection except the transfer rate**; the subset rehearsal measures the per-halo constants that turn them into estimates worth trusting.

---

## Implementation Notes

### Technology stack

- **Home:** `scripts/convert/` in the Mimic repo, with converter tests alongside. It is a long-lived, versioned science tool whose output contract co-evolves with the snapshot reader — not a one-off script.
- **Phase 1 parser**: `pandas.read_csv` with corrected header handling; C tokeniser fallback below ~500 MB/s.
- **Phase 2–3 numerics**: NumPy structured arrays; merge-join via scan on sorted arrays (benchmark at snap 69 scale before committing).
- **HDF5 output**: h5py, uncompressed chunked datasets.
- **Parallelism**: Phase 1 — bounded `multiprocessing.Pool`; Phase 2 — independent per-snapshot jobs; Phase 3 — serial (adjacent snapshots resident together).

### Spin normalisation

Exact match to `apply_ctrees_value_conventions()` (`src/io/tree/read_ctrees_ascii.c:96-106`):
```python
nonzero = halos['Mvir'] != 0.0
for k in range(3):
    halos['Spin_k'][nonzero] /= halos['Mvir'][nonzero]
# Zero-mass halos: leave Spin unchanged (do NOT zero it)
```

### MostBoundID convention

`MostBoundID` is set from the ctrees `id` field (`convert_ctrees_to_lht` in `read_ctrees_ascii.c:145-150`), with the fix_flybys negation applied per reference semantics (Phase 3 step 4).

### a_list extraction

The 70 Shin-Uchuu scale factors are extracted from unique `(SnapNum, scale)` pairs across the actual halo data (collected during Phase 1 scatter), not from ctrees file headers. Sort ascending to produce `shin-uchuu.a_list`.

### Simulation package changes required

**Two packages, not one — recorded 2026-08-25.** The rehearsal runs the cross-format identity gate, which by construction needs a *tree-ordered* run over the same data as the snapshot-ordered run. That mirrors the micro-Uchuu pair (`micro-uchuu-ascii` + `micro-uchuu-snapshot`) and this plan previously named only the snapshot half.

- **`simulations/shin-uchuu-ascii/`** — `tree_type: consistent_trees_ascii`, `snapshots/` pointing at the **subset** ASCII directory. This package is only ever usable on a subset: the full dataset cannot be processed tree-ordered at all, which is this plan's entire premise. Its `halo_properties.yaml` mirrors `micro-uchuu-ascii`'s ctrees bridge contract.
- **`simulations/shin-uchuu/`** — the snapshot package below. Its `snapshots/` symlink points at the **subset** HDF5 during the rehearsal and is re-pointed at the production dataset after the production conversion.

Set `unique_galaxy_id_multiplier: 10000000000` (10¹⁰) in **both** from the start, so the rehearsal exercises it end to end as `POST-PHASE-5-JOINT-REVIEW.md` §4 says it must. The **forest** bound holds at production scale: `mimic_unique_galaxy_id_max_forests(10¹⁰)` = `INT64_MAX / 10¹⁰ − 1` = 922,337,202 forests against the box's 166,547,771.

> **The rank bound does NOT hold at production scale — resolved 2026-08-26 by the rehearsal, and 10¹⁰ must be raised before the production run.** The rehearsal established by exhaustive scan that `max_halo_rank_in_forest` is *the largest forest's whole-run halo count minus one* (measured 8,312,565 = 8,312,566 − 1 over 406,668,896 halos). The percolation super-forest holds **12,834,657,130** halos, so production `max_halo_rank_in_forest` ≈ **1.2834657129 × 10¹⁰**, which exceeds 10¹⁰ and violates `halonr < multiplier` (`src/include/galaxy_id.h:41-45`).
>
> A valid multiplier exists. Combining `halonr < M` with `forestnr_global < INT64_MAX/M − 1` at 166,547,771 forests gives the inclusive window
>
> > **12,834,657,130 ≤ M ≤ 55,379,738,354**
>
> **Use 2 × 10¹⁰** — 1.56× clear of the rank bound and 2.77× below the forest ceiling. 10¹¹ does *not* work: it overflows the forest term. Set the same value in **both** packages or `UniqueGalaxyID` differs between them and the cross-format identity gate fails. **No re-conversion is needed**: the multiplier is not stored in the dataset, the reader composes `UniqueGalaxyID` from the package value at run time.
>
> **`report.py` now finds it for you — CLOSED 2026-08-26 (Task 11 item 8).** It previously searched only powers of ten, so from 10⁹ it reached 10¹¹, found `10¹¹ × (166,547,771 + 1) > INT64_MAX`, and raised `no valid identity multiplier` — aborting the production conversion report even though the window above is non-empty and 4.3 × 10¹⁰ wide. `recommended_multiplier()` now derives the window from the same two conditions the reader enforces at run time in `snapshot_identity_bounds_valid()` (`src/io/snapshot/interface.c:100-129`) — `multiplier > max_halo_rank_in_forest` and `n_forests_total <= INT64_MAX/multiplier - 1` — and searches powers of ten first, then the 1/2/5 ladder that fills the gaps between them. On the production figures it returns exactly the **2 × 10¹⁰** prescribed above, and the report additionally records the whole window as `identity_multiplier_window` so the value can be confirmed rather than recomputed by hand. Still confirm the real `max_halo_rank_in_forest` against the production report's own measured counts.

A new `simulations/shin-uchuu/` package:
- `simulation_info.yaml`: 140 Mpc/h box, confirmed particle mass, `tree_type: snapshot_hdf5`, `processing_order: snapshot_ordered` in run files, 70-snapshot list, and the identity multiplier from the conversion report (D9)
- `halo_properties.yaml`: ctrees bridge contract mirroring `micro-uchuu-ascii` (`M_Crit200` → `HaloMass`, `Len`, `SnapNum`, `Pos` range `[0.0, 140.0]`). **Do not declare `ForestIndex` or `HaloRankInForest`** — corrected 2026-08-12. They are snapshot-format identity metadata rather than catalog halo properties, and are exempt from the declaration rule (`SNAPSHOT-HDF5-FORMAT.md` errata 2026-08-11): the reader consumes both directly by dataset name into `struct SnapshotSlab`'s own `forest_index`/`halo_rank_in_forest` arrays. Declaring them is unnecessary rather than forbidden, but the working exemplar `simulations/micro-uchuu-snapshot/halo_properties.yaml` omits both, and mirroring it is the safe course
- `shin-uchuu.a_list`: from Phase 1
- `snapshots/`: symlink to the 70 HDF5 files

**Run constraints for snapshot-ordered runs** (added 2026-08-12; enforced at config time in `src/core/read_parameter_file.c:1475-1488`, so a production run that violates one aborts at startup rather than part way through):

- **HDF5 output only** — `output_format: binary` is rejected.
- **Serial only** — `NTask > 1` is rejected; there is no MPI path (see `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`).
- **No `--skip`** — a partially completed run cannot be resumed by re-running with `--skip`; plan for a single uninterrupted run, or for restarting it.
- Output is written as **one HDF5 partition file per requested output snapshot** (named by that snapshot's number, `model_<snapnum>.hdf5`) **plus a master**, carries no `Ntrees` and no `TreeHalosPerSnap`, and uses int64 `TotHalosPerSnap`. `hdf5_format_version` is `1.2`.

Property ranges requiring calibration from a test run: `deltaMvir`, `Len` (floor is 1 at this resolution), `Spin`. Note `deltaMvir` is a **core-level output property** (`src/core/core_properties.yaml`, range `[-20000.0, 20000.0]`, already annotated for Uchuu-scale mass swings), not an entry in `simulations/shin-uchuu/halo_properties.yaml` like `Len`/`Spin` — its calibration is checked and edited in `core_properties.yaml`.

---

## Subset Selection and Extraction (the rehearsal's input)

**Recorded 2026-08-25. This closes a genuine gap:** `POST-PHASE-5-JOINT-REVIEW.md` D9 states the rehearsal subset's *composition constraint* — most massive forests **and** a representative low-mass sample — in four places across three documents, but **no document anywhere specified how to choose or extract such a subset, and no tool exists to do it.** A sweep of `docs/`, `scripts/convert/` and `archive/dev-plans/` found the constraint restated and never operationalised. The design below is the answer; building it is the first task of the rehearsal.

### What makes this non-trivial

Six facts, all verified against the code and the data, constrain any solution:

1. **A subset of *files* is not a subset of *forests*.** `locations.dat` places each tree by file and byte offset and forests may span files, so picking whole files yields partial forests — and `fix_flybys`/`fix_upid` operate with per-forest max-snapshot scope (D12), so a partial forest converts differently from the same forest in the full run. **Select whole forests.**
2. **The converter refuses a mismatched index.** `validate_root_coverage()` (`scatter.py:155`) enforces **one-to-one** coverage between observed `#tree` roots and `forests.list` — surplus listed roots abort just as loudly as missing ones. A subset therefore needs its **own** `forests.list`, and the tree-ordered reference run needs its own `locations.dat` to match.
3. **The per-file tree-count header line is checked.** `scatter.py:423-428` aborts when the count line disagrees with the number of `#tree` markers. It is fixed-width space-padded (`45002` followed by blanks, at byte 3,659 of `tree_0_0_0.dat`), so the extractor must rewrite it in place at the same width.
4. **`locations.dat` offsets point at the first *data row*, not at the `#tree` line.** Verified byte-exact: in `tree_0_0_0.dat` the marker `#tree 26551522494` starts at 3,678 and ends at 3,695; the recorded offset is 3,696. The extractor must therefore re-emit the `#tree <root>\n` line itself and copy the body from the recorded offset.
5. **The reader requires file IDs to be contiguous from 0 *and* the file count to be a perfect cube.** `read_locations()` asserts `max_fileid + 1 == numfiles` and then `round(cbrt(numfiles))³ == numfiles` (`src/io/tree/ctrees/ctrees_utils.c:236-246`). **2,744 = 14³** — the full dataset satisfies this exactly, which means the subset must too. An earlier draft of this design claimed sparse original `FileID`s were safe because the fd table grows on demand; **that was wrong** — the table does grow, but both assertions fire afterwards. See the file-coverage rule below.
6. **A forest's reader memory has a floor set by its *tree count*, independent of its halo count.** `load_unit_ctrees_ascii()` preallocates `1000 × ntrees` records in **both** `halo_data` and `additional_info` before reading anything (`src/io/tree/read_ctrees_ascii.c:647-655`). Measured exactly: `sizeof(struct halo_data)` = 104 B and `sizeof(struct additional_info)` = 48 B, so the preallocation is **152,000 B per tree**. At the measured 72.8 halos/tree this over-allocates by ~13.7×, and it dominates — but the arrays still **grow** if a forest's actual halo count exceeds that capacity (`src/io/tree/ctrees/parse_ctrees.h:365-384`), so tree count sets a floor rather than a ceiling. That is why the gate below has two halves. The super-forest would demand 104,845,278 × 152,000 B ≈ **15.9 TB**, which settles its exclusion beyond argument — but it also means a forest with many *small* trees can be intractable while its halo estimate looks modest.

There is also a **cost** fact that shapes the whole approach: ranking forests by *size* needs only the index files — **21.3 GB, about 3 minutes at the measured 110 MB/s** — and touches none of the 11.61 TB of tree data. Beyond that, the tree bytes are read remotely and sparsely: one root row per candidate in Stage 2 (~1 GB), then the selected byte ranges in Stage 4. The bulk 11.61 TB is never read for selection at all.

### The two-host split

**Four stages, alternating hosts.** An earlier draft had a single local `plan` step and one remote `extract`; that was not executable, because the root-row sampling in Stage 2 needs the remote `.dat` bytes, which are not local and must not be transferred in bulk. The subcommand boundaries below follow the host boundaries.

| # | Subcommand | Where | Reads | Writes | Memory |
|---|---|---|---|---|---|
| 1 | `subset.py plan-candidates` | **Mac Studio** | `forests.list`, `locations.dat`, **`filesizes.tsv`** | `forest_table.npy`, `candidates.npy` | ~20 GB |
| 2 | `subset.py sample-roots` | **`tooarrana1`** | `candidates.npy` (36 MB, shipped up), **`shinuchuu_scalefactor.txt`** (for the scale assertion), + one line at each candidate offset | `root_values.npy` | **< 1 GB** |
| 3 | `subset.py finalize` | **Mac Studio** | `forest_table.npy` + `candidates.npy` + `root_values.npy` (~40 MB down) | `selection.json` + `selection.npy` | ~20 GB |
| 4 | `subset.py extract` | **`tooarrana1`** | `selection.npy` (≈140 MB, shipped up) + the selected byte ranges | subset `tree_*.dat`, `forests.list`, `locations.dat` under `/fred/oz214` | **< 1 GB** |

**Fetch four things in the index step, not two.** An earlier draft transferred only the two index files, which left three stages unable to run:

| Artifact | Size | Why it is needed, and by whom |
|---|---|---|
| `forests.list` | 7.56 GB | Stage 1 — tree → forest |
| `locations.dat` | 13.75 GB | Stage 1 — tree → file, offset |
| **`filesizes.tsv`** | ~100 KB | **Stage 1 cannot compute the last tree's extent without it.** `[offset, file_size)` needs the file size, and neither index file carries it. Produce with one `stat` pass over the 2,744 files |
| **`shinuchuu_scalefactor.txt`** | 490 B | **Stage 2's per-row assertion needs the final a_list scale** to check that each sampled row really is a z=0 root — Stage 2 reads it on the remote host, where it already lives. It is also the source of `shin-uchuu.a_list`, so fetch a local copy now rather than after extraction |

**Artifact schemas, because the stage boundaries are where a wrong assumption hides.** `candidates.npy` must carry `(tree_root_id int64, forest_id int64, file_id int32, offset int64, extent int64)` — **`forest_id` included**, or Stage 3 cannot map a measured root back to its forest, `forest_table.npy` being forest-aggregated. At M = 10⁶ that is 36 MB. `root_values.npy` carries `(tree_root_id int64, mvir float64, jx float64, jy float64, jz float64)`, row-aligned with `candidates.npy` and keyed by root id so the join is checkable rather than positional. `selection.npy` carries the per-file tree lists as `(file_id int32, tree_root_id int64, offset int64, extent int64)` — 28 B per selected tree, ≈140 MB at the 5 × 10⁶-tree target — sorted by `(file_id, offset)`.

**Numeric contract for `root_values.npy`, because "reproduce the convention" is not precise enough.** The production path parses the ASCII columns into **float32** scratch before `apply_ctrees_value_conventions()` widens them to divide and stores a float32 result (`scripts/convert/ctrees_parser.py:433-453`; `src/io/tree/read_ctrees_ascii.c:96-106`). A sampler that parses straight to float64 can therefore disagree with the converter at ties and threshold boundaries. **Quantize each parsed column to float32 first, then apply the float64 divide and float32 store, and record in the artifact that its float64 fields hold widened float32 reader-visible values — not raw parsed text.**

Then transfer the ~184 GB subset down. Both remote stages are chosen precisely because they need no memory — they seek, read, and write. `tooarrana1` has no Slurm, so run them under `tmux`/`nohup`; both are I/O-bound and single-threaded by design. The artifacts crossing hosts are small (tens of MB) in every direction except the final subset.

### Step 1 — build the forest table (Mac Studio)

Parse both index files into `numpy` arrays and join on `TreeRootID`: `forests.list` → `(root_id, forest_id)` int64 (5.0 GB); `locations.dat` → `(root_id, file_id, offset)` (5.7 GB). Then per source file, sort its trees by offset and derive each tree's byte extent:

- `body_end` = the next tree's `offset` − `len("#tree " + str(next_root) + "\n")`;
- for the last tree in a file, `body_end` = file size. This rests on files ending with a newline after the final data row and carrying no trailer, which was **spot-checked on one file only** — the extractor must verify it per file (see the extraction checks).

Aggregate to `(forest_id, n_trees, total_bytes)` — 166.5M rows, ~4 GB — and convert bytes to an estimated halo count at the measured **506.3 B/row**. Keep this table; it is a durable artifact and the cheapest possible answer to "how big is forest X" for the rest of this work.

### Step 2 — the tractability gate, and the file-coverage rule

Report the distribution, then apply **two** gates. Both are binding; the first is the one that actually bites.

**Gate A — per-forest tree count.** The reader preallocates 152,000 B per tree (constraint 6 above), so a forest's peak reader allocation is `152,000 × n_trees` **before** any halo is read. Cap selection at **n_trees ≤ 5 × 10⁵** (≈76 GB) for comfortable headroom inside 512 GB, and record the largest selected forest's projected allocation explicitly. This gate, not the halo count, is what excludes the super-forest: 104,845,278 trees × 152,000 B ≈ **15.9 TB**.

**Gate B — per-forest halo count.** At roughly 500 B/halo across `RawHalo`, `Halo`, `GalaxyData` and the output buffer, 512 GB is an absolute ceiling near 1 × 10⁹ halos; cap at **1 × 10⁸ halos per forest**. Retained as a second bound because Gate A's 1000-halos-per-tree assumption is an over-allocation for typical forests but an *under*-estimate for an unusually deep one.

**The file-coverage rule (constraint 5).** The subset's `locations.dat` must reference file IDs `0 … 2743` **contiguously**, and 2,744 must remain the file count, because 2,744 = 14³ and the reader asserts both contiguity and cubeness. So:

> **Every one of the 2,744 source files must contribute at least one selected tree, and the subset must emit all 2,744 files.**

At a 5 × 10⁶-tree target spread over 2,744 files this is satisfied ~1,800× over *on average* — but average is not proof, and a coverage hole would surface only at Task 6's tree-ordered reference run, after extraction and transfer are paid for. **Make it an explicit acceptance assertion.**

**Closing a hole must not break the whole-forest invariant.** An earlier draft said to "force-add the smallest tree" from a missed file. That is wrong: adding one tree creates a *partial* forest, which is exactly what constraint 1 forbids, and it would change `fix_flybys`/`fix_upid` semantics for that forest. The correct rule:

> For each missed file, add the **smallest complete forest that touches it and passes both gates**. Adding a forest adds all of its trees, which may themselves close other files, so **iterate to closure** — the process is monotone in coverage and converges quickly. Re-run Gate A, Gate B and the balance rule after closure, because the added forests change the totals.

**And name the failure case rather than assuming it away:** if some file is touched only by intractable forests — in the limit, only by the super-forest — no complete-forest closure exists and this route is blocked for that file. With ~115,000 trees per file on average and the super-forest holding 33% of trees, a file that is *entirely* super-forest is implausible, but implausible is not checked. If it happens, **treat it as a blocker and report it** — do not improvise. Renumbering to a different perfect cube (1,728 = 12³, 2,197 = 13³) is arithmetically reader-compatible, but it is not a ready fallback: it requires a fully specified repartition-and-rewrite procedure, and both the extractor and the acceptance gates above currently assume the original 2,744 filenames and IDs are preserved end to end. Specifying that procedure is its own piece of work, to be scoped only if the case actually arises.

### Step 2b — root-row sampling: exact values for sampled rows, and a rigorous lower bound

Byte size is a proxy for mass, and a proxy is not what D9 asks for. It can be improved cheaply, though **not** turned into a global guarantee — be precise about which.

**The mechanism.** The first data row of a tree is its z=0 root — ctrees writes each tree depth-first from the root, and the recorded `locations.dat` offset points at exactly that row (constraint 4). Spot-verified: the first data row of `tree_0_0_0.dat` carries `scale = 0.99998`. So take the top `M` candidate trees by byte size (suggest **M = 10⁶**), seek to each recorded offset and read to the end of that line. Per candidate this yields the exact z=0 `Mvir` (column 10) and `Jx, Jy, Jz` (columns 23–25) from the header's own index.

**What this establishes, and what it does not.** The distinction matters because an earlier draft overclaimed both halves:

| Claim | Status |
|---|---|
| Values are exact **for sampled rows** | **Yes** — read from the data, not derived |
| `max \|J_k\| / Mvir` over the sample is a **lower bound** on the box's z=0 `Spin` maximum | **Yes, rigorously.** If it already exceeds 1000, D7's bound is refuted on the spot — a real result |
| It is an **upper bound**, or "the" extremum | **No.** `Spin ∝ Mvir^(2/3)` is a population scaling, not an extremal bound, and `POST-PHASE-5-WORK.md` §2.1 already records scatter at fixed mass (σ in ln λ ≈ 0.4–0.5) and extremal statistics as unresolved. A high-spin halo below the byte cut can exceed the sampled maximum |
| The top-`K` forests by root `Mvir` are **globally** the top `K` | **No.** The candidate pool is chosen by a byte proxy, so exactness inside the pool says nothing about what the proxy excluded |

**So validate the proxy rather than assume it — on data we already hold — and make the result a gate, not a note.** micro-Uchuu is fully local in both ASCII and converted form, so measure there: the Spearman rank correlation between tree byte extent and root `Mvir`, and the **recovery fraction** — what fraction of the true top-200 forests by maximum root `Mvir` a byte-extent prefilter of relative depth `M/N_trees` recovers.

> **Acceptance: recovery ≥ 0.90 at the chosen relative depth.** If a prefilter at relative depth `M/N` recovers less than 90% of the true top-200, **increase `M` until it does**, and carry that calibrated relative depth to Shin-Uchuu — `M = ceil(depth × 315,004,242)`. If no depth up to 1% of trees reaches 0.90, the proxy is too weak to support a high-mass stratum: **stop and report**, rather than proceeding with an untested heuristic and a D9 claim that rests on it.

The suggested `M = 10⁶` (≈0.3% of trees) is a **starting point pending this measurement**, not a decided value. Do the measurement as part of Task 1's micro-Uchuu round trip; it costs one extra pass over data already on disk, and it is the only thing standing between "byte size correlates with mass" and an assumption.

**Required per-row assertions.** The universal first-row-is-root premise is verified on exactly one tree, and the reader never checks it — `assign_forest_ids()` cross-checks root ids between `forests.list` and `locations.dat` only (`src/io/tree/ctrees/ctrees_utils.c:299-312`). So the sampler must assert, on **every** sampled row, that the row's `id` equals the `#tree` marker's root id and that its `scale` is the final a_list scale, and it must read to the newline rather than assume 1 KB suffices. A violated assertion means the premise is false for this dataset and the design needs revisiting — far better learned here than at Task 6.

**And reproduce the production `Spin` convention, or the numbers are not comparable.** `apply_ctrees_value_conventions()` divides `J` by `Mvir` in float64 and stores float32, and **deliberately leaves zero-mass halos carrying raw `J`** (`src/io/tree/read_ctrees_ascii.c:96-106`, mirrored in `scripts/convert/fixups.py:158-183`). A sampler that divides naively, or that divides zero-mass rows, will not produce the values the range check will later see.

### Step 2c — the residual `Spin` limitation, stated plainly

Even with Step 2b, the *rehearsal run* still cannot exercise a maximum that lives in an excluded forest, and Step 2b measures z=0 only rather than all 70 snapshots. Three things bound what remains, in this order:

1. The `Spin` range is a **validation-tier** check, not a runtime FATAL (`POST-PHASE-5-WORK.md` §2.1) — a too-narrow bound cannot abort the production run, it fails a test afterwards.
2. **Step 2b returns a rigorous *lower* bound on the z=0 `Spin` maximum**, drawn from a candidate pool that includes super-forest trees. That is genuinely informative — if it exceeds 1000, D7 is refuted immediately — but it is **not** an upper bound and does not close the question. Step 1's forest table separately records what fraction of halos the excluded forests hold; that is exposure *population*, not a magnitude bound either. **Nothing before the production scan bounds the maximum from above.**
3. The **production** conversion sees every halo at every snapshot, so the **final** `Spin` bound is set from a scan of the production dataset **before** the production run. `report.py` records totals, per-snapshot counts and identity bounds but **not** value extrema (`scripts/convert/report.py:79-97`), so this is a separate `h5py` scan over the 70 files' `Spin` datasets — and it is a **binding pre-run gate**, not an optional check.

The rehearsal's job for `Spin` is to confirm the bound is not wildly wrong and to exercise the code path. Step 2b makes that confirmation quantitative; step 3 of this list still sets the production value.

### Step 3 — stratified selection

Two strata, both recorded in the manifest so every measurement can be attributed:

- **Random stratum (the bulk).** A fixed-seed random sample of tractable forests, drawn from the full distribution rather than from its small end, sized to reach the tree-count target. Random draw is what makes the orphan statistics — and therefore `C` and `G` — representative of the box rather than of an extreme.
- **High-mass supplement.** The top `K` tractable forests by **measured maximum root `Mvir`** from Step 2b (suggest K = 200) — not by `total_bytes`, which is only a proxy. These are the most massive systems **in the byte-selected candidate pool** — the best available proxy for "most massive outside the super-forest", with its recovery fraction measured on micro-Uchuu rather than assumed — and they exercise the largest-forest tree-driver memory path.

**Target: ≈5 × 10⁶ tree roots**, i.e. ~1.6% of the box's z=0 halos. At the measured 72.8 halos/tree that is **≈3.6 × 10⁸ halos**, ~184 GB of ASCII, ~36 GB of converted HDF5, and a z=0 slab near 5 × 10⁶ halos — about 9× micro-Uchuu's. Large enough for stable ratios, small enough to run in hours and to **re-run**, which item 1.2 may require.

**Balance rule.** The supplement biases `C/N` and `G/N` *upward* (massive forests host more satellites, hence more Type 2/3 galaxies), which is conservative for a memory ceiling — but only while it stays small. **If the supplement contributes more than ~15% of subset halos, raise the random stratum rather than cutting `K`**, so the bias stays bounded and the ratios stay usable for extrapolation. Record both strata's halo counts either way.

> **Measured 2026-08-25 (Session B): at Shin-Uchuu scale `K` = 200 is far too large, and the stated remedy alone is not affordable.** The top 200 tractable forests by measured root `Mvir` hold 262,670,594 halos — **66.2%** of a 5 × 10⁶-tree subset, not a modest overshoot. Reaching 15% by raising the random stratum *alone*, as this rule literally directs, needs ≈33.9 × 10⁶ trees, ≈1.75 × 10⁹ halos and **887 GB** of ASCII: 4.4× the subset size this plan designs for, against its own requirement that the subset stay small enough to run in hours and to re-run. The selection therefore used **both** levers — random stratum 5 × 10⁶ → 8 × 10⁶ trees **and** `K` 200 → 20 — giving a **14.2%** supplement at 210 GB. Crucially this costs nothing in the memory path the supplement exists to exercise: the largest selected forest is the same 69,532-tree forest either way. What it does narrow is the composition claim, from "top 200" to "top 20 within the byte-selected candidate pool" — which is why the package READMEs state the bounded form. **Treat `K` = 200 as calibrated-out; start from `K` ≈ 20 at this scale.**

**What "representative" has to mean, concretely — with the binning frozen, or the test is unfalsifiable.** "A random sample is representative" is an assertion until it is tested, and a test whose bin edges are chosen after seeing the data can be made to pass or fail at will. Freeze the algorithm:

- **Bins:** half-decade edges on `log10(n_trees)`, i.e. `[1, √10), [√10, 10), [10, 10√10), …`, over the tractable population only, starting at `n_trees = 1`.
- **Statistic:** for each bin, the **population share** — that bin's forests as a fraction of all tractable forests — compared against the same share within the random stratum. (Not the per-bin sampling *rate*, which is trivially uniform for a uniform draw and therefore tests nothing.)
- **Tail handling:** bins holding fewer than 1,000 population forests are pooled rightward into the next bin before the comparison, so a handful of giant forests cannot fail the test on counting noise.
- **Acceptance:** every populated bin after pooling is represented in the sample, and no bin's sampled share departs from its population share by more than a factor of two.

Also record the median and 90th-percentile `n_trees` for both. **State the limitation honestly in the manifest:** this validates the sample's *forest-size* distribution, which is what drives the reader workload and the orphan statistics `C` and `G` depend on. It is not a direct test of low halo *mass*, which no index file carries. D9's rationale is about the low-mass population; forest size is the available proxy for it, and calling it that is better than implying more.

**Two selection acceptance assertions that are easy to forget and expensive to miss:**

- **File coverage** — all 2,744 original file IDs contribute at least one tree (Step 2's file-coverage rule). Close a hole with the smallest **complete** forest touching that file, never a lone tree, and iterate to closure; see the rule under Step 2.
- **Snapshot span** — D9 requires the earliest snapshots through z=0. `locations.dat` cannot show which snapshots a forest occupies, so this **cannot** be asserted at selection time; it is asserted after conversion instead, from the conversion report's per-snapshot counts. Carry it forward as an explicit gate rather than assuming it: a subset with no early-snapshot halos still produces all 70 files (the writer emits empty snapshots) and can still pass a naive identity comparison.

### Step 4 — extract (`tooarrana1`), then verify before transferring

For each source file holding selected trees: copy the header up to (not including) its first `#tree` line; **rewrite the tree-count line at the same field width**; then for each selected tree in ascending offset order write `"#tree <root>\n"` followed by the body bytes `[offset, body_end)`, recording the new offset. Emit the subset `tree_*.dat` under their **original filenames**, plus a subset `forests.list` and a subset `locations.dat` carrying the new offsets with the original `FileID` and filename preserved. **All 2,744 files must be emitted**, including any that end up holding a single forced-in tree: `read_locations()` grows its fd table on demand but then asserts `max_fileid + 1 == numfiles` and that `numfiles` is a perfect cube (`src/io/tree/ctrees/ctrees_utils.c:236-246`), and 2,744 = 14³.

**Verify remotely, before spending 28 minutes transferring it.** Every check below is cheap; the failure each prevents is not.

- every selected root appears exactly once in both index files;
- **all 2,744 files exist, file IDs are contiguous `0 … 2743`, and the count is a perfect cube** — the assertion the reader itself will make;
- each rewritten count line equals that file's `#tree` marker count;
- each emitted body's md5 equals the md5 of its source byte range;
- each recorded offset is immediately preceded by its own `#tree <root>\n` and begins a well-formed data row;
- **each source file ends with a newline after its final data row** — this is an extractor *precondition*, checked per file rather than assumed, because the last tree's body is taken as `[offset, file_size)` and a trailer or a missing newline would corrupt it. It was spot-checked on one file only when this design was written. A subset that fails any of these fails the converter later and much more expensively.

### Tooling and placement

`scripts/convert/subset.py`, with the four subcommands of the staged table above — `plan-candidates` and `finalize` local, `sample-roots` and `extract` on the data node — alongside its own tests — the same home and the same standards as the rest of the converter, which `scripts/convert/README.md` already describes as a long-lived versioned science tool rather than a one-off. **It must be `numpy`-only:** the checkout on `tooarrana1` (`~/Science/mimic`, tracking this branch, with `mimic_venv`) has `numpy` and `h5py` but **no `pandas`**.

---

## Risks and Mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| fix_flybys / fix_upid divergence from reference | Critical | Micro-Uchuu topology cross-check by stable halo identity gates the converter before any Mimic code |
| Non-adjacent `desc_scale` links in Shin-Uchuu | High | Abort — corrupt data by definition; no repair path exists or is wanted |
| ~~Particle mass inferred, not documented~~ — **RETIRED 2026-08-14** | ~~High~~ | Confirmed as **8.97 × 10⁵ Msun/h** from Ishiyama et al. 2021 ([arXiv:2007.14720](https://arxiv.org/abs/2007.14720)) and cross-checked against the 140 Mpc/h box, 6400³ particles and Ω_m = 0.3089. The risk was real: the recorded value was low by exactly 10×, and `Len = round(Mvir_native × 1e-10 / PartMass)` would have been inflated 10× for every halo. Use the confirmed value when freezing `simulation_info.yaml` |
| ~~Super-forest rank sort resource surprise~~ — **RETIRED 2026-08-28**, on this row's own stated condition: the converter scale-engineering pass landed and re-derived the key volume by measurement (**9.76–10.01 GB peak RSS at 406,668,896 halos**, against 76.39 GB before). | ~~Blocker~~ | The risk was real and is now spent. As shipped, `compute_identity()` (`scripts/convert/links.py`) concatenated five int64 columns over *all* snapshots (**916 GB at the measured 22.9B halos**) plus the lexsort's order array (**183 GB**) — **~1.10 TB, 2.1× installed RAM** analytically, and **≈4.30 TB** once the rehearsal measured it at 187.84 B/halo — while its own docstring deferred the external-merge sort as a "production concern". The converter scale-engineering pass (joint review D4) implemented it and re-derived the volume by measurement: the rank pass now runs under an explicit memory budget at **9.76–10.01 GB on 406,668,896 halos**, emitting link-stage artifacts md5-identical to the pre-change ones. See "Pre-conversion obligation" → "Pass complete". **The successor risk is the per-snapshot window**, projected at ≈225–235 GB at the production largest slab and to be re-derived from the production conversion report. |
| Transfer stalls / partial batches | Medium | rsync --checksum, per-file resume manifest, consumptive deletes keep disk bounded |
| Phase 1 parser too slow | Medium | C tokeniser fallback; same worker interface |
| snap_num vs snap_idx naming variant | Medium | Check both column names (matching setup_column_info) |
| Missing upid targets within a snapshot | High | Abort by default after the reference pid fallback |
| Zero-mass halos producing Len=0 | Low | Log and preserve; do not assert > 0 |

---

## Resolved Decisions (formerly "Open Design Decisions")

All resolved in the 2026-07-02 joint review (archived at `archive/dev-plans/dual-driver-plan-review.md`):

1. **FoF semantics** — replicate reference `fix_flybys`/`fix_upid` exactly, including MostBoundID negation, reference list ordering, and per-forest max-snapshot scope (D12). Snapshot-native semantics would invalidate the cross-format identity gate.
2. **Non-adjacent descendants** — abort; adjacency is a format invariant, phantoms are ctrees' job and already done (D1).
3. **UniqueGalaxyID** — snapshot runs reproduce tree-driver IDs exactly via dense `ForestIndex` + reference-ordered `HaloRankInForest` and the per-simulation identity multiplier (D9).
4. **Reader contract** — the schema above is the frozen draft; promoted to a durable `docs/` spec in dual-driver Phase 4a, with `format_version` guarding evolution.
5. **Converter home** — `scripts/convert/` in this repo (D5).

---

## Relation to the Dual-Driver Plan

One sequence (see `MIMIC-DUAL-DRIVER-PLAN.md`):

1. Freeze the format contract (this plan's schema → durable `docs/` spec)
2. **Build this converter; validate on micro-Uchuu ASCII** (topology cross-check vs `read_ctrees_ascii.c`) — no new Mimic code needed
3. ~~Dual-driver Phase 4b: snapshot reader against the micro-Uchuu fixtures~~ — **done 2026-08-04** (`MIMIC-SNAPSHOT-READER-PLAN.md`); the format this plan emits is now readable and validated by Mimic
4. Dual-driver Phase 5: snapshot driver + cross-format identity gate on micro-Uchuu — **done 2026-08-12** (gate green: both models, both timestep schemes, per-ID bitwise)
5. **Then** the one-time 11.61 TB Shin-Uchuu production conversion (this plan at full scale)
6. `simulations/shin-uchuu/` package; sage16 end to end; HMF/GSMF sanity at z = 0, 1, 2

Shin-Uchuu is the primary scientific motivation for the snapshot pathway — and the only way Mimic can process it at all, because of the super-forest.

### Delegated obligation: the snapshot-driver memory fallback (owned here, decided elsewhere)

`MIMIC-DUAL-DRIVER-PLAN.md` Phase 5 item 6 decides that the snapshot driver keeps **two complete raw slabs** unconditionally and builds no alternative, because the micro-Uchuu gate is nowhere near any ceiling. It then explicitly assigns the production-scale fallback to *this* plan — "This trigger belongs to `SHIN-UCHUU-CONVERSION-PLAN.md`, not to Phase 5" — on the grounds that its inputs are design outputs Phase 5 has not produced yet. Recorded here so it is not rediscovered under time pressure at the production step (added 2026-08-10; it was decided 2026-08-04 but never written into this plan).

**Before the production run**, recompute the adjacent-snapshot peak from *actual* allocation capacities, not estimates: both retained raw slabs, both processed generations, the galaxy pools, the output and HDF5 buffers, and allocator growth. Include the reader's transient staging buffers — `snapshot_h5_fill_halos()` allocates one scalar and one vector buffer at the widest native element size, 32 bytes per halo in total (≈10 GB at the projected 315M-halo z = 0 slab), live only during a slab load but concurrent with the slab it is filling.

**[SUPERSEDED 2026-08-26 — see the CLOSED block below, which is the operative instruction. The peak was measured, the trigger was crossed, and the replacement was deliberately NOT made a precondition. Do not act on this paragraph's imperative.]** *As originally written:* **if the peak exceeds 85% of installed RAM** (≈435 GB on a 512 GB machine), replace the retained *previous* raw slab with a compact projection of `{int32_t Len, int32_t NextProgenitor}` — all the previous generation is ever read for — costing ≈315e6 × 8 B ≈ 2.5 GB against a full second slab. Struct sizes for this catalog, re-measured 2026-08-13: `struct RawHalo` **88 B**, `struct Halo` 176 B, `struct GalaxyData` 176 B, `struct HaloOutput` 264 B. (The 2026-08-04 audit recorded 104 B for `RawHalo`; that was the default pair's, superseded below.)

**Recompute 2026-08-13 — clear for the production configuration under measured ratios, but not yet closed.** Struct sizes were measured (not estimated) against the generated headers for `MODEL=sage16 SIMULATION=micro-uchuu-snapshot`: `RawHalo` 88 B, `Halo` 176 B, `GalaxyData` 176 B, `HaloOutput` 264 B. Counting both live generations at the projected 315,004,242-halo z=0 slab — raw slabs, identity and aux arrays, the fully-resident processed output buffers, both galaxy pools — plus the reader's staging transients and an allowance for write buffers and allocator overhead, the **projected peak is ≈317 GB against the ≈435 GB trigger** for `sage16`, whose measured output population is 0.99× the slab on micro-Uchuu. **The projection is parametric, not fixed**: per generation it is `120·N + 176·C + 176·G`, where `C` is the processed buffer's realised capacity (it grows ×1.5 past its seed) and `G` the galaxy pool's allocation high-water (which the output count does not bound, since Type 3 galaxies are allocated but never emitted). At `C = G = 1.5N` the peak reaches ≈428 GB, and in a `halos-only`-like regime (2.11× measured at z=0) ≈579 GB — over installed RAM. **`G` is unmeasured**, so this obligation stays open: instrument `C` and `G` at the subset rehearsal and re-derive before the production run. The full derivation, the micro-Uchuu measurements, and the sensitivity table are in `POST-PHASE-5-WORK.md` §2.2. Note the joint review's F-5 sized the output buffer at 264 B/record; that is `struct HaloOutput`, while the buffer holds `struct Halo` at 176 B — the recompute corrects it. Re-measure `sizeof(struct RawHalo)` once `simulations/shin-uchuu/` exists.

**CLOSED 2026-08-26 by the subset rehearsal — and the fallback is DEFERRED, not required. This supersedes the 2026-08-13 recompute above.** `C`, `G` and peak RSS are now measured, so this obligation no longer stays open on an unmeasured `G`. Two `sage16` snapshot-ordered runs differing only in slab scale give `C`/N = **1.000111** (the buffer never grew — `C` = N + `MIN_HALO_ARRAY_GROWTH` exactly), `G`/N = **1.02262**, and peak RSS **2.184 GB at N = 621,360** and **14.301 GB at N = 9,006,294**.

Three findings change what this obligation should do:

- **The peak is set by the largest slab, not the z=0 slab.** Measured 1.1258× larger on a broad plateau (snapshots 41–48 within 1%), so the projection runs at N ≈ 3.546 × 10⁸, not 315,004,242. The superseded ≈317 GB figure above used the z=0 slab *and* the parametric form; both understate.
- **Projected peak ≈470.4 GB**, from a fit of measured RSS with the galaxy pool's *allocated* slots separated (slack is 27–39% at these scales, ~1% at production). The parametric form gives 358.9 GB at the same slab and understates by design.
- **Now ≈476.6 GB**, updated 2026-08-26: JR §6 item 11's output-buffer seed headroom landed at 5%, moving `C`/N from 1.000111 to 1.05 and adding 6.24 GB across both live generations. That is the deliberate price of removing the growth cliff `P`/N = 0.99701 sat 0.3% away from; the deferral of the compact previous-slab projection is conditional on it and now stands. Re-project with `C`/N = 1.05.
- **Units.** `hw.memsize` is 549,755,813,888 B = exactly **512 GiB**; the projection is in GB = 10⁹ B, in which that is **549.76 GB**. The "≈435 GB (85% of a 512 GB machine)" trigger above is therefore **79% of capacity**, and a literal 85% is 467.3 GB.

**Decision (owner, 2026-08-26): do NOT implement the compact previous-slab projection as a precondition.** It saves 80 B/halo = **28.4 GB against a 35.4 GB overshoot** — it does not close the gap — and the trigger's 15% headroom assumption does not match this host: measured committed memory outside Mimic is **≈37.7 GB** (31.0 GB anonymous + 6.7 GB wired; the 170.8 GB of file-backed pages is evictable cache), so a lean box leaves Mimic **≈540 GB** and 470.4 GB sits at ~87% with ~70 GB spare. **Margins updated 2026-08-26:** 470.4 GB is the **pre-mitigation** fit; with JR §6 item 11's seed headroom the current decision figure is **476.6 GB**, i.e. a **41.6 GB** overshoot against the 435 GB trigger, ~**88%** of a lean box's ≈540 GB, with ~**63 GB** spare. The conclusion is unchanged — the compact projection still saves only 28.4 GB and still does not close the gap.

**The pre-run decision gate is instead:** (1) run the production job on an otherwise-idle machine, no local LLM; (2) **re-project from the production conversion report's own per-snapshot slab counts** — that report supplies the real largest slab and removes this projection's one unavoidable extrapolation; (3) implement the compact projection only if that re-projection comes in materially higher. It stays specified above and available.

**One mitigation is required regardless, and it is tracked as `POST-PHASE-5-JOINT-REVIEW.md` §6 item 11.** `sage16` never grows its output buffer and measured `P`/N is **0.99701** — 0.3% below the point where growth triggers. Crossing it steps `C`/N to 1.5 *and* exposes a `realloc` transient holding the old and new blocks together; that is a step no amount of free RAM absorbs, and it is the one risk the deferral does not cover. Full derivation: `POST-PHASE-5-WORK.md` §2.2.

**Correction 2026-08-12.** That 104 B is the **default pair's** `RawHalo` (`sage16`/`mini-millennium`), not this catalog's. The ctrees-bridge catalog the snapshot packages use measures **88 B** (verified against `micro-uchuu-snapshot`), so a full second slab is ≈315e6 × 88 B ≈ **27.7 GB**, not ≈32.8 GB. Re-derive the figure from the `shin-uchuu` package's own `sizeof(struct RawHalo)` rather than from either number quoted here. Note also that neither the two processed generations nor the two galaxy pools are quantified anywhere in this plan — the numeric memory tables above are converter-side only — so they must be measured, not assumed, during the recompute. Phase 5 shipped **no** memory-projection branch: the driver holds two complete raw slabs unconditionally, so the projection described here would have to be implemented if it were required. **[SUPERSEDED 2026-08-26: the trigger was crossed and the projection was still deferred — see the CLOSED block above for the operative pre-run gate. The absence of the branch is still accurate; the implied obligation is not.]**

### Pre-conversion obligation: converter scale-engineering pass (2026-08-13)

The dual-driver Phase 5 joint review (`docs/dev/POST-PHASE-5-JOINT-REVIEW.md` F-13/D4) found that the converter as implemented **cannot execute the production conversion**, independently of the runtime readiness this plan otherwise reports. Three distinct limits, all confirmed against the code and all already recorded in `scripts/convert/README.md`'s "Shin-Uchuu-scale notes" as deferred to a future production pass, but scheduled by no plan until now: the identity/rank pass (`compute_identity()` in `links.py`) is in-memory over all snapshots — at the measured 22.9B halos that is **916 GB** for its five concatenated int64 columns plus **183 GB** for the lexsort's order array on a 512 GB machine, ≈1.10 TB in total (the figures recorded here in 2026-08-13, ≈600–720 GB plus ≈120–144 GB, assumed 15–18B halos), and its own docstring defers the external-merge sort as a production concern (this also corrects the risk-table row above); the required producer validation battery (`validate.py`) is likewise in-memory, loading and retaining full-dataset columns, and cannot be skipped because producer validation is part of the format contract and this plan's own Definition of Done; and the scatter phase's resume model is incompatible with this plan's own "Getting the Data to the Mac" transfer strategy — `run_scatter` requires every listed source file to exist at start, freezes the ordered source set into the manifest and refuses resume when it changes, and `source_completed()` re-stats completed files, so batches deleted under the batched, consumptive-delete transfer this plan requires break resume. Also recorded in the same README note: the ~5 GB Phase 0 forest map is passed to pool workers by pickling, and the fix-up stage's sequential per-satellite scan (up to 31 searches per satellite) "would need revisiting for Shin-Uchuu."

**Scope of the pass:** external-merge rank sort (replacing the in-memory lexsort); streaming/per-snapshot validation (replacing the full-dataset-resident battery); a batch-aware scatter inventory compatible with consumptive deletes (replacing the frozen-source-set resume model); shared or memory-mapped forest-map distribution (replacing per-worker pickling); and a production-scale benchmark of the fix-up stage's sequential satellite scan with an explicit, measurement-first retain/optimize decision.

**Added to the scope 2026-08-25 — consumptive deletion of stage intermediates.** Deletion stops after the concat stage: `scatter.py` consumes its worker parts and `sort_index.py` consumes the unsorted scratch, but `fixups.py`, `links.py` and `hdf5_writer.py` delete nothing, so the workdir accumulates every intermediate to **≈8.60 TB** at the measured 22.9-billion-halo scale — more than the 7.3 TB primary scratch volume, fitting only by spanning a second volume with ~1.4 TB of margin on a one-shot run. Adding delete-after-verify to the fixups, links and write stages (the pattern `sort_index.py` already implements, including its crash-between-unlink-and-save recovery) brings the peak to **≈6.04 TB** and restores single-volume operation. This belongs here rather than in the operator's head: the same manifest machinery already exists, and a storage-exhaustion failure part way through a multi-day no-resume run is exactly the class of loss this pass exists to prevent.

**Also re-derived 2026-08-25:** the rank pass figure this obligation is built on is **~1.10 TB, not 600–720 GB** — see the Feasibility section. The conclusion is unchanged and now holds by a wider margin.

> ### Scope updated 2026-08-26 from the subset rehearsal — read this before planning the pass
>
> The rehearsal (`POST-PHASE-5-JOINT-REVIEW.md` §6 item 6) measured the shipped converter end to end on a 406,668,896-halo subset. Three changes to the scope above, and **two new items**:
>
> - **Item 1's target is 4× larger than the analytic figure.** The rank pass was measured at **76.39 GB peak RSS at 406,668,896 halos = 187.84 B/halo**, against the ~48 B/halo the 1.10 TB figure assumes (five int64 key columns plus the order array). Scaling the *measurement* to 22.9 × 10⁹ halos gives **≈4.30 TB, not 1.10 TB** — i.e. **8.4× installed RAM rather than 2.1×**. The gap is temporaries during concatenation, `lexsort`'s internal copies and process residency, none of which the analytic terms model. Plan against 4.30 TB.
> - **Item 2 must cover `crosscheck.py`, not only `validate.py`.** The cross-check loads the emitted dataset, the reference galaxy output (109.7 GB) and the topology dump (42 GB) simultaneously: measured **251.32 GB peak for a 1.8% subset**. It is the converter's own acceptance instrument (D10), so it has to survive production scale too.
> - **Item 5 is answered — retain.** The fix-up stage's sequential per-satellite scan measures a stable **≈1.28 µs/satellite** (1.37 s at snapshot 44, 2.49 s at snapshot 69), projecting to **≈1.2–1.6 h** across all 70 snapshots at production, on a one-time multi-day conversion. Not worth rewriting code whose exact `fix_upid` reference parity is load-bearing. Note `fix_flybys` is the larger per-snapshot term where demotions are heavy (7.92 s for 1,194,990 demotions) and projects to only ~5 min at production.
> - **NEW item 7 — the per-file whole-manifest rewrite.** `run_scatter` calls `manifest.save()` after **every** source file, rewriting the entire manifest; each source entry carries 70 per-snapshot checksums plus 70 observed `(SnapNum, scale)` pairs, so the manifest grew a measured **38.2 KB per file** and reached **104.9 MB** at 2,744 files. The cost is quadratic in source-file count. Measured effect: scatter ran at **39.0 MB/s** against ~385 MB/s of storage and 8 × ~105 MB/s of parse capacity, with pool workers at **12–25% CPU**. The production dataset has the *same* 2,744 files, so this term does not shrink with data volume. Batch the saves, or make the manifest append-only per source file.
> - **~~NEW item 8 — `recommended_multiplier()` searches only powers of ten.~~ CLOSED 2026-08-26**, landed early and independently of the rest of this pass exactly as this item advised. The search now derives the feasible window from the two conditions the reader itself enforces and returns the prescribed 2 × 10¹⁰ on the production figures; the report also records the window. See "Simulation package changes required" above.
>
> **Terminal disk moves down**, for once: measured **277 B/halo** of coexisting intermediates (`sorted` 108 + `idx` 8 + `fixed` 120 + `links` 36 + `pending_fp` 4) gives **6.34 TB** at production rather than the recorded ≈8.60 TB, which counted `snap_NNN.bin` as coexisting when sort deletes it. The emitted dataset measured **100.7 B/halo**, confirming the ≈2.29 TB output projection.

**Acceptance gate:** the full micro-Uchuu validation battery and topology cross-check must re-run green (the converter's reference semantics must not move while its machinery is rebuilt), plus a measured memory profile of the rank pass at projected Shin-Uchuu scale.

This pass was converter-side, gate-checkable, and well-bounded, and it got its own frozen implementation plan — `CONVERTER-SCALE-PASS-PLAN.md`, eight code slices plus an acceptance gate.

#### Pass complete — the gate ran green on 2026-08-28

Measured on the conversion host (Mac Studio M3 Ultra, 32 cores, 512 GB) with Python 3.13.2, numpy 2.4.6, h5py 3.16.0, pandas 3.0.5. **The gate ran against a dataset the rebuilt converter itself produced**, not against the rehearsal's output: `validate` and `crosscheck` both load a dataset from disk, so re-running them over an existing one would go green without executing a line of the changed code. A fresh end-to-end micro-Uchuu conversion in a new workdir came first.

- **Fresh conversion** (`scatter` → `sort` → `fixups` → `links` → `write` → `report`; 119.65 / 15.35 / 13.37 / 38.69 / 10.09 / 6.36 s) reproduced the recorded totals exactly: **22,580,924 halos, 50 populated snapshots, 440,651 forests, `max_halo_rank_in_forest` = 350074**, 51 emitted files, report `validation PASS`.
- **Producer battery on that fresh dataset: 15/15 PASS**, three runs, byte-identical stdout, 0.374–0.399 GB peak RSS.
- **Topology cross-check on that fresh dataset: 8/8 PASS** — `reference-sanity` plus the seven checks, `topology-chains` included, against a 2.01 GB dump covering all 22,580,924 halos — **zero unexplained mismatches**, three runs, byte-identical stdout and `crosscheck_report.json`, 3.61–3.62 GB peak RSS. Definition of Done item 2 is therefore discharged again on the rebuilt converter.

**Memory, measured at rehearsal scale (406,668,896 halos), warm and repeated** — three runs each, one cold and two warm, `/usr/bin/time -l` peak RSS. The cold figure is quoted but never used alone: ~17.7 GB of run-to-run variance was recorded for Mimic itself at this scale (§2.2 of `POST-PHASE-5-WORK.md`: 34.445 then 16.752 GB, same binary and dataset), consistent with cold page cache being charged to the process footprint. The cross-check reproduces that pattern here, and its cold run reads **lower** than both warm runs.

| Stage | Before the pass | Measured 2026-08-28 | Factor |
|---|---:|---:|---:|
| `links` rank pass | 76.39 GB = 187.84 B/halo | **9.76 / 9.81 / 10.01 GB = 24.00 / 24.11 / 24.62 B/halo** | 7.6–7.8× |
| `validate` battery | 73.27 GB | **3.258 / 3.246 / 3.245 GB** | 22.5× |
| `crosscheck compare` | 251.32 GB | **16.56 / 15.84 GB warm, 10.09 GB cold** | 15.2–15.9× warm |

The link stage ran in 961 / 896 / 885 s at the shipped 2 GiB default budget (15 sorted runs, 0 merge passes, 19,520,107,008 B of transient spill and 6,506,702,336 B of identity arrays on disk), and **its output did not move**: all 139 link-stage artifacts were md5-identical to the retained rehearsal's, and the run-scoped identity values reproduced exactly (`n_forests_total` = 6,011,205, `max_halo_rank_in_forest` = 8,312,565). The battery ran in 166 / 102 / 102 s holding a 50,833,612-byte identity bitset with nothing spilled to disk; the cross-check in 660 / 650 s warm against 1,528 s cold.

**Storage.** Consumptive deletion of stage intermediates is implemented and opt-in (`--consume-intermediates` on `fixups`, `links` and `write`), and the emitted dataset is bitwise identical either way. Measured stage by stage on micro-Uchuu it takes the peak workdir from 300.99 to **192.99 B/halo** and the terminal intermediates from 276.99 to **0.99 B/halo**. Projected to 22.9 × 10⁹ halos the whole conversion peaks at **6.89 TB against the 7.0 TB ceiling** — 0.48 TB under the 7.37 TB volume — with **scatter the binding stage through the staged source batch, whose maximum this envelope admits is 4.4 TB**, so the 11.61 TB source needs at least three batches. Three preconditions hold it: deletion enabled, a bounded staged batch, and cross-check artifacts excluded. The full per-stage table is in `scripts/convert/README.md` → "Storage envelope and the production memory term".

**Scatter throughput was re-measured at 96.2 MB/s** (11,515,537,257 B in 119.65 s) against the rehearsal's 39.0 MB/s — **but the two are not comparable, and this is not evidence that item 7 is fixed at production scale.** micro-Uchuu is a single source file, and one file takes the serial branch (`scatter.py:1071-1073`), so what is measured here is the parse path, not the pool. The pooled path's throughput is re-measured by the production transfer itself.

**What this pass does not close, and must be carried into the production run:**

- **The per-snapshot window is now the binding memory term in the link stage.** The two measured points (4.55 GB at micro-Uchuu's 621,360-halo largest slab, 9.76–10.01 GB at the rehearsal's 9,006,294) fit ≈620–650 B per largest-slab halo above a ≈4.2 GB floor, putting the link stage at **≈225–235 GB** at the joint review's projected production largest slab of ≈3.546 × 10⁸ halos — inside the machine, against ≈4.30 TB before, but no longer negligible. It is a two-point extrapolation across two datasets differing in more than slab size: **re-derive it from the production conversion report's own per-snapshot counts before committing to the run.**
- **A production-scale topology cross-check remains impossible in principle**, not merely expensive: its reference side is a tree-ordered `halos-only` run over the same data, and the ctrees reader preallocates 152,000 B per tree for a whole forest before reading a halo, so the super-forest's 104,845,278 tree roots alone project to ≈15.9 TB of reader preallocation. **The binding cross-check gate is micro-Uchuu**, as the Definition of Done already says.
- **The production identity multiplier is still unset** — pathway step P3: raise both `simulations/shin-uchuu*/simulation_info.yaml` to 2 × 10¹⁰, confirmed against the production conversion report, with no re-conversion needed.

---

## Definition of Done

1. 70 HDF5 snapshot files produced and validated (halo count, adjacency, round-trip progenitor check, FoF chain integrity, NextProgenitor same-file scope, identity uniqueness, Len non-negative)
2. **Topology cross-check passes on micro-Uchuu** by stable halo identity (the converter acceptance gate, completed long before the production run)
3. The conversion report exists and the shin-uchuu identity multiplier is set from its measured counts
4. `simulations/shin-uchuu/` package registered and building clean with the snapshot reader
5. Mimic runs `sage16` on Shin-Uchuu end-to-end: no assertion failures, no broken links, no memory errors
6. HMF and GSMF plots produced and sanity-checked at z=0, z=1, z=2
