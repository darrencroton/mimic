# Shin-Uchuu ctrees ASCII → Snapshot HDF5 Conversion Plan

**Status:** Converter built and micro-Uchuu-validated 2026-07-24 — the external converter described here exists under `scripts/convert/` and passed its micro-Uchuu acceptance gate (full pipeline over the real 22,580,924-halo / 50-snapshot ASCII data; producer validation battery + a seven-check cross-check against a `halos-only` reference run, topology-order proof fully discharged, zero unexplained mismatches). That gate was re-run end to end on 2026-08-03 on a fully regenerated micro-Uchuu dataset, now placed at `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-snapshot/` for snapshot-reader development: same three totals, producer battery 15/15, cross-check green including `topology-chains`. **P1–P3 of the production conversion ran to completion 2026-08-29 to 2026-09-03** — see "The Production Execution Sequence" → "Production run complete" for the measured totals. Remaining: P3b onward (re-pointing the package at the production dataset, then the production `sage16` run and its science checks). All previously open design decisions resolved (joint plan review 2026-07-02, decisions D1–D12; review record archived at `archive/dev-plans/dual-driver-plan-review.md`). Earlier revision reviewed twice by Codex gpt-5.5 (2026-06-27).
**Date:** 2026-07-02 · **substantially revised 2026-08-25** — source data re-measured at the operative path (total size, row width and halo count all corrected), the conversion machine and storage layout decided and recorded, and the previously unspecified rehearsal subset selection designed. See "Source Data Summary" → measurement note, "Where The Work Runs", "Feasibility" and "Subset Selection and Extraction". **Extended 2026-08-29** with "The Production Execution Sequence" — the operational steps P1–P9, moved here from the retired root `HANDOFF.md`, with their constraints, acceptance rules and code evidence. This document now owns both the design and its execution.
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

The Mac Studio's internal APFS container is **4.00 TB with ≈1.17 TB free**, not the 8 TB this plan previously assumed — `/Volumes/Internal` holds ≈1.82 TB of other data. Conversion storage is therefore external:

| Volume | Free | Capacity | Role |
|---|---|---|---|
| `/Volumes/LaCie` | **7.71 TB** | 8.00 TB | **Primary scratch and output.** Case-sensitive APFS, USB |
| `/Volumes/Scratch` | **3.00 TB** | 3.00 TB | **Overflow only**, if the converter's terminal accumulation is not fixed first (see below) |
| `/Volumes/Internal` | **1.17 TB** | 4.00 TB | Existing datasets (micro-Uchuu, mini-Uchuu). **Not** available for Shin-Uchuu |

**Every figure in this table was re-measured with `df -k` on 2026-08-29** and is quoted on this document's 10⁹ B/TB scale. It supersedes the 7.3 / 2.7 TB and 142 GB recorded here previously: LaCie gained space when the converter scale pass's own scratch workdirs were removed, and the internal container gained space when other data was moved off it. `/Volumes/Internal` shares its APFS container with the boot volume, so the free space shown for it is the **container's**, not a private quota — which is also why its 1.82 TB of data plus 1.17 TB free does not add up to the 4.00 TB capacity: the boot volume and its snapshots hold the balance.

**Was LaCie alone enough? Not as the converter stood before `b2ae9601`, and the reason was a defect rather than a budget.** The question below is worked against the 7.3 + 2.7 TB recorded at the time, which is what makes its margins historical rather than current. Deletion stopped after the concat stage: `scatter.py` removed its verified worker parts and `sort_index.py` removed the unsorted scratch once the sort verified, but `fixups.py`, `links.py` and `hdf5_writer.py` deleted nothing — the fix-up stage registered the new `fixed` artifact and retained its `sorted` input — so at the end of Phase 4 the workdir still held every intermediate at once, **≈8.60 TB** at the corrected 22.9-billion-halo scale (see the Disk table). That exceeded LaCie alone and fit only by spanning both external volumes, with ~1.4 TB of margin on a one-shot run — against the 7.3 + 2.7 TB recorded at the time; the same 8.60 TB has ~2.1 TB of margin against the 7.71 + 3.00 TB measured now.

**Consumptive deletion of the fixups, links and write intermediates landed in `b2ae9601` and closes this.** It is opt-in as `--consume-intermediates` on those three stages, the emitted dataset is bitwise identical either way, and the measured envelope is **6.89 TB against a 7.0 TB ceiling**, binding at scatter through the staged source batch, on the three preconditions listed under "The Production Execution Sequence" below. The per-stage table and the current file-by-file deletion contract are in `scripts/convert/README.md` → "Consumptive deletion of intermediates" and "Storage envelope and the production memory term"; the paragraph above is kept only as the reason the flag exists, and its line-level description of the code no longer holds.

**The envelope's "7.37 TB volume" and this table's 7.71 TB are the same volume at two times, not a contradiction.** 7.37 TB was LaCie's free space when the storage envelope was derived; **7.71 TB** is what `df -k` measures on 2026-08-29, after the scale pass's own scratch workdirs were removed. Wherever this document or `scripts/convert/README.md` says "0.48 TB under the 7.37 TB volume", read it as the headroom the envelope was derived with. The envelope's conclusion is unaffected and its headroom is now **larger, not smaller**: the projected 6.89 TB peak sits **0.82 TB** under the volume as measured today, against the 7.0 TB policy ceiling either way. Nothing downstream of the envelope changes, so the 7.37 TB derivation is left standing rather than recomputed.

### Remote working and output space on OzSTAR

**Decided 2026-08-26, and constrained by permissions rather than preference.** The converted dataset is produced on the Mac Studio and must stay there for the production `sage16` run, which projects to ≈476.6 GB of peak RSS against `tooarrana1`'s 251 GB. Archiving a copy back to OzSTAR afterwards is worthwhile — it is durable, shareable, and ≈2.31 TB at the measured ≈110 MB/s is ≈5.8 h in the background — but it is a **post-P6 step**, not an alternative to holding the dataset locally.

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

**The incompatibility this strategy used to have with the converter is closed.** `run_scatter` once required every listed source file to exist at start, froze the ordered source set into the manifest, and re-stat-ed completed files, so consumptive deletes broke resume. The batch-aware scatter inventory landed in `9ad19662`: `scatter --batch` distinguishes `deferred` (in the frozen inventory, bytes not arrived) from `consumed` (scattered, intermediates verified, bytes released by `release`), never finalizes on its own, and `release` refuses a source whose intermediates finalization has already deleted. See `scripts/convert/README.md` → "Batch mode: the interleaved consumptive transfer", and "The Production Execution Sequence" below for how P1 and P2 interleave.

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

**Rank pass (between Phase 2 and Phase 3, or fused into Phase 3 bookkeeping):** ranks are per-forest over all snapshots, so they need a forest-major view once. For ordinary forests this is cheap grouping. For the super-forest (~7–8 billion halos at the measured 22.9-billion total), it is one large deterministic sort of ~(scale, upid, pid, id, slab-slot) keys — ~150–250 GB of key data: in-RAM on this machine or a chunked external merge sort on scratch. **Implemented 2026-08-28 by the converter scale-engineering pass** (`3d52446c`, `c5573d0c`). Until then the shipped `compute_identity()` (`scripts/convert/links.py`) concatenated and lexsorted the key columns over *all* snapshots globally — **~1.10 TB** analytically at the measured production scale, and **≈4.30 TB** once the rehearsal measured it at 187.84 B/halo. It now ranks through the external merge sort in `scripts/convert/rank_sort.py` under `--memory-budget-mb` (default 2 GiB), spilling sorted runs to scratch and keeping `(ForestIndex, HaloRankInForest)` in on-disk arrays: measured **9.76–10.01 GB peak RSS on the 406,668,896-halo rehearsal subset**, emitting link-stage artifacts md5-identical to the pre-change ones. See the "Pre-conversion obligation" subsection. It runs once, its output is the `HaloRankInForest` column plus the run-scoped `max_halo_rank_in_forest`/`n_forests_total` header values, and it is the direct input to setting the shin-uchuu identity multiplier (see below).

**Memory at peak** (snap 68 processing, which loads snap 68 + snap 69 index):
- snap_68 sorted data: ~33 GB
- snap_69.idx: 315M × 8 bytes = 2.5 GB
- snap_68 working arrays (sort keys, resolved upid, groupby, progenitor inversion): ~2× data ≈ 66 GB
- snap_69 FirstProgenitor pending buffer: 315M × 4 bytes = 1.3 GB
- HDF5 write buffer: ~2 GB
- **Realistic peak: ~140–170 GB — SUPERSEDED 2026-08-28 by measurement.** The term-by-term derivation above is kept as history; it is no longer the planning figure. Measured end to end, the same quantity — the link stage's peak process residency, whose binding term is exactly this per-snapshot window — extrapolates to **≈225–235 GB** at the projected production largest slab (two points: 4.55 GB at micro-Uchuu's 621,360-halo largest slab, 9.76–10.01 GB at the rehearsal subset's 9,006,294; see "Pre-conversion obligation" → "Pass complete"). **That is 55–95 GB above this bullet — material on a 512 GB box — and the gap is unattributed rather than reconciled: plan against ≈235 GB, the top of the measured range, not against 170.** The super-forest rank sort peaks separately at ~150–250 GB *as designed*, and since 2026-08-28 the shipped rank pass is bounded below that by construction: it is an external merge sort under an explicit memory budget rather than a global lexsort. Re-derive the window from the production conversion report's own per-snapshot counts before the run.

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
| **Rank pass** | ~~**~1.10 TB as implemented**~~ → **bounded 2026-08-28** | 5 concatenated int64 columns over *all* snapshots (916 GB) + the lexsort order array (183 GB), over installed RAM by 2.1× analytically and by 8.4× once measured. Replaced by the external merge sort under `--memory-budget-mb`: **9.76–10.01 GB measured at 406,668,896 halos** |
| Phase 3 remap | ~200–250 GB (peak, snaps 68+69) | Two snapshots + working arrays + pending buffer. **Superseded by measurement: ≈225–235 GB projected** — see the per-snapshot-window bullet under "Memory at peak" |
| Phase 4 validate | ~~~60 GB~~ → **bounded 2026-08-28** | Was a full-dataset-resident battery; now streams one snapshot at a time. **3.25 GB measured at 406,668,896 halos** against a 73.27 GB in-memory baseline |

**The rank pass was the binding constraint, and the converter scale-engineering pass removed it.** The figures above are kept as the reason that pass was mandatory: the superseded estimate was 600–720 GB against 512 GB, recomputed at 22.9 billion halos it was **~1.10 TB**, and once the rehearsal measured 187.84 B/halo it was **≈4.30 TB**. The external-merge rank sort landed 2026-08-28 (`3d52446c`, `c5573d0c`) and the streaming battery with it (`ce2a3cf2`) — see "Pre-conversion obligation" → "Pass complete" for the measured record. **The binding memory term is now the per-snapshot window**, ≈225–235 GB projected at the production largest slab, to be re-derived from the production conversion report at step P5. Note the subset rehearsal was unaffected either way: **the rehearsal ran on the shipped converter with no D4 work at all**, which is exactly what D9's ordering assumed. Its rank pass was *projected* at ~17.5 GB and *measured* at **76.39 GB peak RSS at 406,668,896 halos** — the projection was 4.4× low, which is what took the production figure from 1.10 TB to 4.30 TB. Quote 76.39 GB, never ~17.5 GB, as the pre-pass rehearsal-scale figure.

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
| Output HDF5 (uncompressed, incl. identity columns) | **2.31 TB** | — |
| **Phase 1 peak** | **≈4.95 TB** | worker files alongside their concat output |
| **Terminal accumulation, as the converter stands** | **≈8.60 TB** | LaCie alone is insufficient; needs LaCie + Scratch |
| **Terminal accumulation, with D4 consumptive deletes** | **≈6.04 TB** — the **2026-08-25 analytic projection**, made before the deletion landed | Superseded as a planning figure by the **measured 6.89 TB envelope** (which includes the ≤4.4 TB staged source batch); see "Pass complete". Kept here as the analytic row this table's other rows are consistent with |

### Time

| Phase | Bottleneck | Estimate |
|---|---|---|
| Transfer (batched rsync, overlapped with Phase 1) | ssh throughput, **measured 110 MB/s** | **~29 h** |
| Phase 1 ASCII parse | Tokenisation over 11.61 TB | 4–12 h compute |
| Phase 1 concat | External SSD write | 1–2 h |
| Phase 2 sort + index | I/O ~5 TB | 1–2 h |
| Rank pass | External-merge sort over 22.9e9 keys (post-D4) | 3–8 h |
| Phase 3 remap + write | Merge-join + FoF + inversion + I/O | 4–8 h |
| Phase 4 validate | Sequential reads over 2.31 TB | 2–4 h |
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
2. **The converter refuses a mismatched index.** `validate_root_coverage()` (`scatter.py:192-216`) enforces **one-to-one** coverage between observed `#tree` roots and `forests.list` — surplus listed roots abort just as loudly as missing ones. A subset therefore needs its **own** `forests.list`, and the tree-ordered reference run needs its own `locations.dat` to match.
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

**CLOSED 2026-08-26 by the subset rehearsal — and the fallback is DEFERRED, not required. This supersedes the 2026-08-13 recompute above.** `C`, `G` and peak RSS are now measured, so this obligation no longer stays open on an unmeasured `G`. Two `sage16` snapshot-ordered runs differing only in slab scale give `C`/N = **1.00011** (the buffer never grew — `C` = N + `MIN_HALO_ARRAY_GROWTH` exactly), `G`/N = **1.02262**, and peak RSS **2.184 GB at N = 621,360** and **14.301 GB at N = 9,006,294**.

Three findings change what this obligation should do:

- **The peak is set by the largest slab, not the z=0 slab.** Measured 1.1258× larger on a broad plateau (snapshots 41–48 within 1%), so the projection runs at N ≈ 3.546 × 10⁸, not 315,004,242. The superseded ≈317 GB figure above used the z=0 slab *and* the parametric form; both understate.
- **Projected peak ≈470.4 GB**, from a fit of measured RSS with the galaxy pool's *allocated* slots separated (slack is 27–39% at these scales, ~1% at production). The parametric form gives 358.9 GB at the same slab and understates by design.
- **Now ≈476.6 GB**, updated 2026-08-26: JR §6 item 11's output-buffer seed headroom landed at 5%, moving `C`/N from 1.00011 to 1.05 and adding 6.24 GB across both live generations. That is the deliberate price of removing the growth cliff `P`/N = 0.99701 sat 0.3% away from; the deferral of the compact previous-slab projection is conditional on it and now stands. Re-project with `C`/N = 1.05.
- **Units.** `hw.memsize` is 549,755,813,888 B = exactly **512 GiB**; the projection is in GB = 10⁹ B, in which that is **549.76 GB**. The "≈435 GB (85% of a 512 GB machine)" trigger above is therefore **79% of capacity**, and a literal 85% is 467.3 GB.

**Decision (owner, 2026-08-26): do NOT implement the compact previous-slab projection as a precondition.** It saves 80 B/halo = **28.4 GB against a 35.4 GB overshoot** — it does not close the gap — and the trigger's 15% headroom assumption does not match this host: measured committed memory outside Mimic is **≈37.7 GB** (31.0 GB anonymous + 6.7 GB wired; the 170.8 GB of file-backed pages is evictable cache), so a lean box leaves Mimic **≈540 GB** and 470.4 GB sits at ~87% with ~70 GB spare. **Margins updated 2026-08-26:** 470.4 GB is the **pre-mitigation** fit; with JR §6 item 11's seed headroom the current decision figure is **476.6 GB**, i.e. a **41.6 GB** overshoot against the 435 GB trigger, ~**88%** of a lean box's ≈540 GB, with ~**63 GB** spare. The conclusion is unchanged — the compact projection still saves only 28.4 GB and still does not close the gap.

**The pre-run decision gate is instead:** (1) run the production job on an otherwise-idle machine, no local LLM; (2) **re-project from the production conversion report's own per-snapshot slab counts** — that report supplies the real largest slab and removes this projection's one unavoidable extrapolation; (3) implement the compact projection only if that re-projection comes in materially higher. It stays specified above and available.

**One mitigation is required regardless, and it is tracked as `POST-PHASE-5-JOINT-REVIEW.md` §6 item 11.** `sage16` never grows its output buffer and measured `P`/N is **0.99701** — 0.3% below the point where growth triggers. Crossing it steps `C`/N to 1.5 *and* exposes a `realloc` transient holding the old and new blocks together; that is a step no amount of free RAM absorbs, and it is the one risk the deferral does not cover. Full derivation: `POST-PHASE-5-WORK.md` §2.2.

**Correction 2026-08-12.** That 104 B is the **default pair's** `RawHalo` (`sage16`/`mini-millennium`), not this catalog's. The ctrees-bridge catalog the snapshot packages use measures **88 B** (verified against `micro-uchuu-snapshot`), so a full second slab is ≈315e6 × 88 B ≈ **27.7 GB**, not ≈32.8 GB. Re-derive the figure from the `shin-uchuu` package's own `sizeof(struct RawHalo)` rather than from either number quoted here. Note also that neither the two processed generations nor the two galaxy pools are quantified anywhere in this plan — the numeric memory tables above are converter-side only — so they must be measured, not assumed, during the recompute. Phase 5 shipped **no** memory-projection branch: the driver holds two complete raw slabs unconditionally, so the projection described here would have to be implemented if it were required. **[SUPERSEDED 2026-08-26: the trigger was crossed and the projection was still deferred — see the CLOSED block above for the operative pre-run gate. The absence of the branch is still accurate; the implied obligation is not.]**

### Pre-conversion obligation: converter scale-engineering pass (2026-08-13)

The dual-driver Phase 5 joint review (`docs/dev/POST-PHASE-5-JOINT-REVIEW.md` F-13/D4) found that the converter as implemented **cannot execute the production conversion**, independently of the runtime readiness this plan otherwise reports. Three distinct limits, all confirmed against the code and all already recorded in `scripts/convert/README.md`'s "Shin-Uchuu-scale notes" as deferred to a future production pass, but scheduled by no plan until now: the identity/rank pass (`compute_identity()` in `links.py`) is in-memory over all snapshots — at the measured 22.9B halos that is **916 GB** for its five concatenated int64 columns plus **183 GB** for the lexsort's order array on a 512 GB machine, ≈1.10 TB in total (the figures recorded here in 2026-08-13, ≈600–720 GB plus ≈120–144 GB, assumed 15–18B halos), and its own docstring defers the external-merge sort as a production concern (this also corrects the risk-table row above); the required producer validation battery (`validate.py`) is likewise in-memory, loading and retaining full-dataset columns, and cannot be skipped because producer validation is part of the format contract and this plan's own Definition of Done; and the scatter phase's resume model is incompatible with this plan's own "Getting the Data to the Mac" transfer strategy — `run_scatter` requires every listed source file to exist at start, freezes the ordered source set into the manifest and refuses resume when it changes, and `source_completed()` re-stats completed files, so batches deleted under the batched, consumptive-delete transfer this plan requires break resume. Also recorded in the same README note: the ~5 GB Phase 0 forest map is passed to pool workers by pickling, and the fix-up stage's sequential per-satellite scan (up to 31 searches per satellite) "would need revisiting for Shin-Uchuu."

**Scope of the pass:** external-merge rank sort (replacing the in-memory lexsort); streaming/per-snapshot validation (replacing the full-dataset-resident battery); a batch-aware scatter inventory compatible with consumptive deletes (replacing the frozen-source-set resume model); shared or memory-mapped forest-map distribution (replacing per-worker pickling); and a production-scale benchmark of the fix-up stage's sequential satellite scan with an explicit, measurement-first retain/optimize decision.

**Added to the scope 2026-08-25 — consumptive deletion of stage intermediates.** Deletion stops after the concat stage: `scatter.py` consumes its worker parts and `sort_index.py` consumes the unsorted scratch, but `fixups.py`, `links.py` and `hdf5_writer.py` delete nothing, so the workdir accumulates every intermediate to **≈8.60 TB** at the measured 22.9-billion-halo scale — more than the primary scratch volume, which held 7.3 TB free when this was written (7.71 TB on 2026-08-29; see "Local storage") — fitting only by spanning a second volume with ~1.4 TB of margin on a one-shot run. Adding delete-after-verify to the fixups, links and write stages (the pattern `sort_index.py` already implements, including its crash-between-unlink-and-save recovery) brings the peak to **≈6.04 TB — the 2026-08-25 analytic projection, recorded before the change landed** — and restores single-volume operation. **The measured post-implementation envelope is 6.89 TB**, including the ≤4.4 TB staged source batch this projection did not carry; that is the operative figure, and it is derived under "Pass complete" below. Two peaks appear in this document for that reason and they are not in conflict: **6.04 TB is the pre-measurement projection, 6.89 TB is the measured envelope, and the 6.89 TB is what to plan against.** This belongs here rather than in the operator's head: the same manifest machinery already exists, and a storage-exhaustion failure part way through a multi-day no-resume run is exactly the class of loss this pass exists to prevent.

**Also re-derived 2026-08-25:** the rank pass figure this obligation is built on is **~1.10 TB, not 600–720 GB** — see the Feasibility section. The conclusion is unchanged and now holds by a wider margin.

> ### Scope updated 2026-08-26 from the subset rehearsal — read this before planning the pass
>
> The rehearsal (`POST-PHASE-5-JOINT-REVIEW.md` §6 item 6) measured the shipped converter end to end on a 406,668,896-halo subset. Three changes to the scope above, and **two new items**:
>
> - **Item 1's target is 4× larger than the analytic figure.** The rank pass was measured at **76.39 GB peak RSS at 406,668,896 halos = 187.84 B/halo**, against the ~48 B/halo the 1.10 TB figure assumes (five int64 key columns plus the order array). Scaling the *measurement* to 22.9 × 10⁹ halos gives **≈4.30 TB, not 1.10 TB** — i.e. **8.4× installed RAM rather than 2.1×**. The gap is temporaries during concatenation, `lexsort`'s internal copies and process residency, none of which the analytic terms model. Plan against 4.30 TB.
> - **Item 2 must cover `crosscheck.py`, not only `validate.py`.** The cross-check loads the emitted dataset, the reference galaxy output (109.7 GB) and the topology dump (42 GB) simultaneously: measured **251.32 GB peak for a 1.8% subset**. It is the converter's own acceptance instrument (D10), so it has to survive production scale too.
> - **Item 5 is answered — retain.** The fix-up stage's sequential per-satellite scan measures a stable **≈1.28 µs/satellite** (1.37 s at snapshot 44, 2.49 s at snapshot 69), projecting to **≈1.2–1.6 h** across all 70 snapshots at production, on a one-time multi-day conversion. Not worth rewriting code whose exact `fix_upid` reference parity is load-bearing. Note `fix_flybys` is the larger per-snapshot term where demotions are heavy: 7.92 s for 1,194,990 demotions at snapshot 69, the **one** snapshot at which it was timed. **Corrected 2026-08-28 — the "~5 min at production" originally recorded here stated no basis and is withdrawn.** Scaled naively by the 56.3× production/rehearsal halo ratio that single snapshot alone is **≈7.4 min**; the all-snapshot total was never measured and is necessarily larger, so ≈7 min is a per-snapshot figure and a lower bound on the whole-run cost. The retain decision is unaffected — the term is minutes against a multi-day conversion either way.
> - **NEW item 7 — the per-file whole-manifest rewrite.** `run_scatter` calls `manifest.save()` after **every** source file, rewriting the entire manifest; each source entry carries 70 per-snapshot checksums plus 70 observed `(SnapNum, scale)` pairs, so the manifest grew a measured **38.2 KB per file** and reached **104.9 MB** at 2,744 files. The cost is quadratic in source-file count. Measured effect: scatter ran at **39.0 MB/s** against ~385 MB/s of storage and 8 × ~105 MB/s of parse capacity, with pool workers at **12–25% CPU**. The production dataset has the *same* 2,744 files, so this term does not shrink with data volume. Batch the saves, or make the manifest append-only per source file.
> - **~~NEW item 8 — `recommended_multiplier()` searches only powers of ten.~~ CLOSED 2026-08-26**, landed early and independently of the rest of this pass exactly as this item advised. The search now derives the feasible window from the two conditions the reader itself enforces and returns the prescribed 2 × 10¹⁰ on the production figures; the report also records the window. See "Simulation package changes required" above.
>
> **Terminal disk moves down**, for once: measured **277 B/halo** of coexisting intermediates (`sorted` 108 + `idx` 8 + `fixed` 120 + `links` 36 + `pending_fp` 4) gives **6.34 TB** at production rather than the recorded ≈8.60 TB, which counted `snap_NNN.bin` as coexisting when sort deletes it. The emitted dataset measured **100.7 B/halo** at rehearsal scale, which puts the production output at 100.7 × 22.9 × 10⁹ = **≈2.31 TB** — the figure every other row of this document now carries, superseding the earlier ≈2.29 TB analytic projection.

**Acceptance gate:** the full micro-Uchuu validation battery and topology cross-check must re-run green (the converter's reference semantics must not move while its machinery is rebuilt), plus a measured memory profile of the rank pass **at the rehearsal subset scale the frozen plan specifies — the retained 406,668,896-halo subset, 1.8% of production — together with the production figure that measurement implies.** No measurement at full Shin-Uchuu scale is required by this gate, and none was taken: the production projection is derived from the subset measurement and is labelled as a projection wherever it appears.

This pass was converter-side, gate-checkable, and well-bounded, and it got its own frozen implementation plan — eight code slices plus an acceptance gate, landed as `184424df`, `36e17512`, `9ad19662`, `3d52446c`, `c5573d0c`, `ce2a3cf2`, `b3368a8d` and `b2ae9601`, plus `0ab453fe` early. The plan itself is complete and archived to `archive/dev-plans/` (gitignored local history), so the commits above, this section and `scripts/convert/README.md` are the durable record.

#### Pass complete — the gate ran green on 2026-08-28

Measured on the conversion host (Mac Studio M3 Ultra, 32 cores, 512 GB) with Python 3.13.2, numpy 2.4.6, h5py 3.16.0, pandas 3.0.5. **The gate ran against a dataset the rebuilt converter itself produced**, not against the rehearsal's output: `validate` and `crosscheck` both load a dataset from disk, so re-running them over an existing one would go green without executing a line of the changed code. A fresh end-to-end micro-Uchuu conversion in a new workdir came first.

- **Fresh conversion** (`scatter` → `sort` → `fixups` → `links` → `write` → `report`; 119.65 / 15.35 / 13.37 / 38.69 / 10.09 / 6.36 s) reproduced the recorded totals exactly: **22,580,924 halos, 50 populated snapshots, 440,651 forests, `max_halo_rank_in_forest` = 350074**, 51 emitted files, report `validation PASS`.
- **Producer battery on that fresh dataset: 15/15 PASS**, three runs, byte-identical stdout, 0.374–0.399 GB peak RSS.
- **Topology cross-check on that fresh dataset: 8/8 PASS** — the seven checks `crosscheck.py` always runs, `reference-sanity` among them (`CHECK_NAMES`, `crosscheck.py:1443-1451`), plus `topology-chains`, which is added only when `--reference-topology` is passed — here against a 2.01 GB dump covering all 22,580,924 halos — **zero unexplained mismatches**, three runs, byte-identical stdout and `crosscheck_report.json`, 3.61–3.62 GB peak RSS. Definition of Done item 2 is therefore discharged again on the rebuilt converter.

**Memory, measured at rehearsal scale (406,668,896 halos), warm and repeated** — three runs each, one cold and two warm, `/usr/bin/time -l` peak RSS. The cold figure is quoted but never used alone, and the spread is reported as measured rather than explained. `POST-PHASE-5-WORK.md` §2.2 records ~17.7 GB of run-to-run variance for Mimic itself at this scale (34.445 GB cold then 16.752 GB warm, same binary and dataset) and labels its own page-cache explanation "a hypothesis, not a measurement". **The cross-check here does not reproduce that pattern — it inverts it**, 10.09 GB cold against 15.84 and 16.56 GB warm. Opposite signs are not one mechanism, so none is asserted. §2.2's instruction is the one followed here: measure warm, repeat, and **plan against the highest observed figure** — 10.01 GB, 3.258 GB and 16.56 GB for the three stages below.

| Stage | Before the pass | Measured 2026-08-28 | Factor |
|---|---:|---:|---:|
| `links` rank pass | 76.39 GB = 187.84 B/halo | **9.76 / 9.81 / 10.01 GB = 24.00 / 24.11 / 24.62 B/halo** | 7.6–7.8× |
| `validate` battery | 73.27 GB | **3.258 / 3.246 / 3.245 GB** | 22.5× |
| `crosscheck compare` | 251.32 GB | **16.56 / 15.84 GB warm, 10.09 GB cold** | 15.2–15.9× warm |

The link stage ran in 961 / 896 / 885 s at the shipped 2 GiB default budget (15 sorted runs, 0 merge passes, 19,520,107,008 B of transient spill and 6,506,702,336 B of identity arrays on disk), and **its output did not move**: all 139 link-stage artifacts were md5-identical to the retained rehearsal's, and the run-scoped identity values reproduced exactly (`n_forests_total` = 6,011,205, `max_halo_rank_in_forest` = 8,312,565). The battery ran in 166 / 102 / 102 s holding a 50,833,612-byte identity bitset with nothing spilled to disk; the cross-check in 660 / 650 s warm against 1,528 s cold.

**Storage.** Consumptive deletion of stage intermediates is implemented and opt-in (`--consume-intermediates` on `fixups`, `links` and `write`), and the emitted dataset is bitwise identical either way. Measured stage by stage on micro-Uchuu it takes the peak workdir from 300.99 to **192.99 B/halo** and the terminal intermediates from 276.99 to **0.99 B/halo**. Projected to 22.9 × 10⁹ halos the whole conversion peaks at **6.89 TB against the 7.0 TB ceiling** — 0.48 TB under the 7.37 TB volume — with **scatter the binding stage through the staged source batch, whose maximum this envelope admits is 4.4 TB**, so the 11.61 TB source needs at least three batches. Three preconditions hold it: deletion enabled, a bounded staged batch, and cross-check artifacts excluded. The full per-stage table is in `scripts/convert/README.md` → "Storage envelope and the production memory term".

**Scatter throughput was re-measured warm and repeated at 91.3–92.0 MB/s** against the rehearsal's 39.0 MB/s. Four runs of the 11,515,537,257-byte source into a fresh workdir each gave 125.27 / 125.21 / 126.15 / 125.76 s = 91.93 / 91.97 / 91.28 / 91.57 MB/s — a 0.69 MB/s spread — while the gate conversion's own scatter run, the figure first published, was 119.65 s = **96.24 MB/s**, the fastest of the five and ~5% above every repeat. **Plan against 91.3 MB/s**, the slowest observed; for a throughput figure the conservative end is the low one, as the high end is for a memory figure. **But the comparison with 39.0 MB/s is not like-for-like, and this is not evidence that item 7 is fixed at production scale.** micro-Uchuu is a single source file, and one file takes the serial branch (`scatter.py:1075-1077`), so what is measured here is the parse path, not the pool — and at this size the stage is CPU-bound, 119–120 s of user time in a 125 s wall.

**Both scatter branches are now measured at the real production file topology, and the pooled path is no longer an unmeasured carry-forward.** Two runs over the retained `subset-ascii` — **2,744 `tree_*.dat` files, the exact production file count**, 210,572,730,148 B (210.57 GB) — on the Mac Studio, each into a fresh workdir under `/usr/bin/time -l`. **Each is a single run at this scale**, not warm and repeated, and each is rehearsal *data* volume at production *file* topology.

| Run | Flag | Wall | Throughput | Peak RSS | Final manifest |
|---|---|---:|---:|---:|---:|
| serial | `--pool-size 1` (the default) | 4,495 s | **46.8 MB/s** | 3,026,649,088 B | 105,050,511 B |
| pooled | `--pool-size 8` | 2,929 s | **71.9 MB/s** | 2,886,500,352 B | 103,305,447 B |

**Projected onto the 11.61 TB source this is the pass's single largest operational consequence.** 39.0 MB/s, the pre-pass pooled baseline, gives **≈82.7 h** — independently reproducing the "~83 h in scatter alone" recorded for the pre-pass code; 46.8 MB/s (post-pass, serial) gives **≈68.9 h**; 71.9 MB/s (post-pass, `--pool-size 8`) gives **≈44.9 h**. The pass roughly halves scatter wall clock, **but only if the operator passes `--pool-size`, which defaults to 1** — about 24 hours of the production conversion rests on one flag. See `scripts/convert/README.md` → "Scatter parallelism: `--pool-size`" and the P2 step below.

**The manifest sizes above evidence the per-entry projection, not the save cadence, and their difference is not a content difference.** 105,050,511 / 2,744 = **38.28 KB per source file**, which reproduces the recorded **38.2 KB per file** exactly. The **total is 105.05 MB** — its own measurement at this scale, not a reproduction of the rehearsal's 104.9 MB, which it exceeds by 0.14%. A final size cannot distinguish the two save policies — `Manifest.save()` serialises the whole manifest on every call, and `scripts/convert/tests/test_scatter.py:895` (`test_manifest_byte_identical_regardless_of_save_policy`) asserts the per-file and batched cadences produce a byte-identical final manifest. The 1,745,064 B gap between the two runs is workdir path length: absolute paths are stored, the workdirs were `pm9-pooled-scatter` (18 characters) and `pm9-pool8` (9), and the pooled manifest carries 193,896 occurrences of its workdir name — 193,896 × 9 = 1,745,064 B, exactly 105,050,511 − 103,305,447.

**What this pass does not close, and must be carried into the production run:**

- **The per-snapshot window is now the binding memory term in the link stage.** The two measured points (4.55 GB at micro-Uchuu's 621,360-halo largest slab, 9.76–10.01 GB at the rehearsal's 9,006,294) fit ≈620–650 B per largest-slab halo above a ≈4.2 GB floor, putting the link stage at **≈225–235 GB** at the joint review's projected production largest slab of ≈3.546 × 10⁸ halos — inside the machine, against ≈4.30 TB before, but no longer negligible. It is a two-point extrapolation across two datasets differing in more than slab size: **re-derive it from the production conversion report's own per-snapshot counts before committing to the run.**
- **A production-scale topology cross-check remains impossible in principle**, not merely expensive: its reference side is a tree-ordered `halos-only` run over the same data, and the ctrees reader preallocates 152,000 B per tree for a whole forest before reading a halo, so the super-forest's 104,845,278 tree roots alone project to ≈15.9 TB of reader preallocation. **The binding cross-check gate is micro-Uchuu**, as the Definition of Done already says.
- **The production identity multiplier is still unset** — step P3 below: raise both `simulations/shin-uchuu*/simulation_info.yaml` to 2 × 10¹⁰, confirmed against the production conversion report, with no re-conversion needed.

---

## The Production Execution Sequence

**This section is the operational sequence for the production conversion and everything after it.** It moved here on 2026-08-28 from a gitignored root `HANDOFF.md`, which was archived at the same time; this document is the conversion's design authority, so the steps live with the design they execute. `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` → "Step 1: Shin-Uchuu" is the index that points here. Everything below is stated as a fact with its code evidence — nothing here defers to an untracked file.

> ### Production run complete 2026-09-03 — P1–P3 executed and measured; read this before P3b
>
> P1 (transfer) and P2 (production conversion) ran to completion 2026-08-29 to 2026-09-03, and P3 (the identity multiplier) is set and committed (`5506ef88`). The measured totals from the production `conversion_report.json`, independently re-verified against the raw report rather than taken from any summary — `validation_passed: true`, all 70 snapshots present with non-zero halo counts, `outputs_dir` matches the intended path exactly: **22,503,649,037 halos**, `n_forests_total = 166,547,771`, `max_halo_rank_in_forest = 12,646,607,900`. The emitted dataset measures **2.0 TB** (`du -sh` on `/Volumes/LaCie/data/shin-uchuu/production-snapshot`), ≈89 B/halo — **this supersedes the ≈2.31 TB / 100.7 B/halo pre-run projection** recorded above under "Pass complete", which was extrapolated from the 1.8%-scale rehearsal rather than measured at full production scale. P1+P2 total wall clock was ≈138.3 hours (5.76 days) against this document's own "2–4 days" estimate for P2 alone: the batch-mode transfer cycle being strictly sequential rather than overlapped, and several stages (`finalize` especially) running well past naive extrapolation from rehearsal-scale figures, both contributed. **Two operational findings from the production run are not yet reflected anywhere else in this document or in `scripts/convert/README.md`**: `rsync --checksum` over a `--files-from` list of hundreds of files is unusable at this scale (a bare `--dry-run` with it enabled did not finish in 3.5+ minutes; without it, the same dry-run completed in 0.6 s), and the storage envelope's "require ≥7.0 TB free... re-check between batches" wording cannot hold literally across the whole batch loop, since worker scratch accumulates until `sort` runs once at the end — free space measured as low as ~5.2 TB between batches with no problem. The exact batching commands used and the full operational record currently live only in that session's gitignored `HANDOFF.md`; migrating the durable parts into `scripts/convert/README.md` before that file is archived is outstanding, not yet done. **P3b (re-pointing `simulations/shin-uchuu/snapshots` at this dataset) has not been done** — both packages' `snapshots` symlinks still point at the rehearsal subset. Start there.

> ### P3b, P4 done and measured 2026-09-04 — P5 stops the sequence here; do not launch P6 as planned
>
> **P3b executed and verified.** `simulations/shin-uchuu/snapshots` re-pointed at `/Volumes/LaCie/data/shin-uchuu/production-snapshot`; `simulations/shin-uchuu-ascii/snapshots` left on the subset per step 5. The verification script (above) passed on every count: 70/70 snapshot files present, zero missing, zero count mismatches against the report, `forests.h5` present, `n_forests_total` and `max_halo_rank_in_forest` both equal to the report's, and `totals.halos` reads 22,503,649,037 — confirming the production dataset, not the 406,668,896-halo subset. The P8 operator decision (which z=1/z=2 pair) was also settled: **nearest-redshift, 52 (z=1.032) and 43 (z=2.028)**, over bracketing-from-below (53/44). Both candidate pairs were confirmed non-empty on both the production report and the retained subset before the choice was made; all four `shin-uchuu` run files' `output.snapshot_list` are updated identically to `[69, 52, 43, 40, 20, 10, 5, 2, 1, 0]` (commit `06d81228`).
>
> **P4 (`Spin` pre-run scan) passed cleanly.** Full scan of `/halos/Spin` over all 70 production snapshot files: **22,503,649,037 halos scanned** (matches the report's total exactly), **min/max = −397.185547 / +416.694580**, **zero non-finite components**. `max |Spin|` = 416.69, comfortably inside the declared `[-1000, 1000]` bound and well clear of even the "thinner margin" `[680, 1000)` band the mass-matched anchor had flagged as plausible — no range widening, no regeneration, no re-conversion needed. Elapsed ≈13.6 min (815.7 s), close to the ≈20 min estimate. This is the first production-scale measurement of this quantity; record it as the basis for any future `Spin` bound decision.
>
> **P5 (memory re-projection) does not clear, and blocks P6 as currently specified.** Using `N = max(entry["rows"] for entry in report["per_snapshot"].values())` from the real production `conversion_report.json`: the largest slab is **snapshot 34 at 519,342,987 halos** — not the pre-measurement projection of ≈3.546 × 10⁸, but **46.5% higher**, on a broader/taller plateau (snapshots 31–40 all in the 494–519M range) than the rehearsal predicted. Applying this document's own fit, **peak ≈ 1.221 GB + 1,340.6 B/halo × N ≈ 697 GB** unmitigated, and **≈639 GB** even with the deferred compact-previous-slab mitigation applied (`peak_mitigated ≈ 1.221 GB + 1,228.6 B/halo × N`) — both well above the documented ≈589 GB ceiling above which "the post-mitigation peak is still over the ~540 GB available," and both far above the ~540 GB this host actually has. Per this document's own three-way branch (below): **STOP. Do not launch P6 on this host as planned.** This needs either a larger host or a slab strategy this document does not specify (streaming the largest slab, splitting it, or a smaller retained generation) — none of which is decided here. See `MIMIC-DEVELOPMENT-PATHWAY.md`'s Completed Work / open-work record for the resulting decision once made.

### Where the work runs

Recorded above under "Where The Work Runs, And Getting The Data There" and unchanged: the **Mac Studio (M3 Ultra, 32 cores, 512 GB)** for everything needing memory or CPU; `tooarrana1` (`ssh dcroton@nt.swin.edu.au`, 251 GB, 4 cores) as a source and streaming host only. Source data is `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` — **2,744 `tree_*.dat`, 11.61 TB apparent**, read-only to us. Remote working and archive space is `/fred/oz214/dcroton/shin-uchuu/{working,snapshot-trees}/`, *not* beside the source, for the permission reason recorded above. Local scratch and output is `/Volumes/LaCie`; `/Volumes/Scratch` is overflow.

**Local storage cannot stage the source in one piece.** LaCie and Scratch together hold ≈10.7 TB free against an 11.61 TB source — **7.71 + 3.00 = 10.71 TB, `df -k` measured 2026-08-29**, the figures in the "Local storage" table above — so the transfer must be batched and consumed as it is scattered. The storage envelope independently caps the staged batch at **4.4 TB**, so at least **three batches** are needed regardless.

**Every Python snippet below runs under `mimic_venv`.** `h5py`, `numpy`, `pandas`, `PyYAML` and `matplotlib` are installed into `mimic_venv` by `pip install -r requirements.txt`, not into the system Python (`scripts/convert/README.md` → Requirements). The P3b, P4, P7 and P8 snippets are therefore written as `mimic_venv/bin/python`; activating the venv first and running plain `python3` is equivalent, but the bare system `python3` fails on `ImportError` before reading a byte of data.

### The steps

⚠️⚠️ = needs the machine's full memory and an otherwise-idle box; ⚠️ = keep the box quiet; unmarked runs anywhere.

| # | Step | Est. wall clock | Memory | Blocks the rest? |
|---|---|---|---|---|
| **P1** | **Transfer 11.61 TB** of `tree_*.dat` from OzSTAR at the measured ≈110 MB/s single ssh stream, **plus the production `forests.list` first** — 7.56 GB, ≈1.1 min (7.56 GB ÷ 110 MB/s = 68.7 s), and a hard prerequisite of the first `scatter` (P2 step 0). **Batched and consumed as it is scattered** — see the storage paragraph above | **≈29 h**, overlappable with P2's early batches | low | YES |
| **P2** | **Production conversion on `/Volumes/LaCie`** — `--workdir /Volumes/LaCie/convert/shin-uchuu`, never the repository's `output/` symlink (P2 step 1 below). `scatter --batch` with **`--pool-size 8`** (the only measured value), `--consume-intermediates` on `fixups`/`links`/`write`, then `release` per batch, `finalize`, `sort`, `fixups`, `links`, **`write` with `--output-dir` set to the permanent production dataset path** (`--output-dir` is a `write` flag and nothing else — `convert_ctrees.py:189-193`), `report` **with `--multiplier 20000000000`**. **Assert the snapshot-span gate** (below) from the conversion report's own per-snapshot counts | **2–4 days**; scatter alone is ≈44.9 h pooled against ≈68.9 h serial | ⚠️⚠️ `links` ≈235 GB projected | YES |
| **P3** | **Set the identity multiplier to 2 × 10¹⁰** in **both** `simulations/shin-uchuu/simulation_info.yaml:36` and `simulations/shin-uchuu-ascii/simulation_info.yaml:33`, kept identical, confirmed against the report's own `max_halo_rank_in_forest`. **No re-conversion** | minutes | low | YES |
| **P3b** | **Point the package at the production dataset, and verify the re-point before anything reads it.** `simulations/shin-uchuu/snapshots` points at the **rehearsal subset** today. Re-point it, preserve the subset, and check per-snapshot counts against the conversion report — **not** header agreement, which the subset also passes. Full procedure below; skipping it runs P6 against the subset | ~30 min | low | **YES — P6 is wrong without it** |
| **P4** | **`Spin` pre-run scan** over the production dataset — a separate few-line `h5py` scan, because the conversion report records no value extrema. Acceptance rule and the scan itself are below | **≈20 min** (**measured 2026-08-29 on the retained rehearsal dataset**: the scan read `/halos/Spin` from a **40.96 GB** emitted dataset of 71 files — **≈4.88 GB** of `Spin` values, 406,668,896 halos × 3 components × 4 B — in **19.38 s**; scaled by halo count to production that is **≈18 min**, rounded up here. Both bases scale linearly with halo count, so the projection is the same whichever one you scale) | low | YES |
| **P5** | **Re-project the run's memory peak** from the report's real per-snapshot slab counts, using the measured-RSS fit and `C`/N = 1.05, then take the three-way branch below: proceed, reinstate the compact previous-slab projection, or **stop and escalate** if the re-projection is so high the mitigation cannot close the gap. The fit and both thresholds are below | ~1 h | low | YES |
| **P6** | **Production `sage16` run**, against the **P3b-verified** dataset. Build and run commands below; `MODEL`/`SIMULATION` are compile-time (C11). Run detached through a wrapper that **records the true exit code** — P7 must not start on a failed run. Projected peak **≈476.6 GB** | **≈8–16 h (estimate; the rehearsal wall clock is not committed)** | ⚠️⚠️ **the maximum-memory task of the whole sequence** | YES |
| **P7** | **`deltaMvir` post-run range validation** on the production output against `[-20000, 20000]`, **after asserting P6 exited 0 and the expected snapshot set is complete**. Scan method below | ~30 min | ⚠️ | YES |
| **P8** | **Science checks** — HMF and GSMF at z = 0, 1, 2 through `plot/mimic-plot/mimic-plot.py`. **The shipped run file's `output.snapshot_list` cannot produce them** — see below | 0.5–1 day | low | YES |
| **P9** | **Archive the converted trees** to `/fred/oz214/dcroton/shin-uchuu/snapshot-trees/` — ≈2.31 TB at ≈110 MB/s | ≈5.8 h, background | low | No |
| **F1** | `POST-PHASE-5-JOINT-REVIEW.md` §6 **item 10** — the Uchuu-family particle mass (3.25 → 3.27 × 10⁸ Msun/h across six packages). **Sequenced last on purpose** | 1 session | low | No |
| **F2** | Merge `feature/ctrees-snapshot-reader` → `main` | — | — | — |

**Roughly 1–2 weeks in total; the machine-bound stretch is P1 → P6, about 4–7 days.** The two maximum-memory moments, in order of severity, are P6 (≈476.6 GB projected — close applications, no local LLM, nothing else on the box) and P2's `links` stage (≈225–235 GB projected, to be re-derived at P5 before it is trusted).

### P2 — the workdir, the preflight, and the three flags an omission costs days

**Everything in this step runs on `/Volumes/LaCie`. Do not use the repository's `output/` shorthand.** `output` is a symlink to `/Volumes/Internal/results/mimic` (`readlink output`), and `/Volumes/Internal` is the volume the "Local storage" table above marks **not available for Shin-Uchuu**: `df -k` measures **1.17 TB free** there on 2026-08-29, against **2.47 TB for the scatter worker scratch alone and 2.56 TB at finalize** (`scripts/convert/README.md` → "Storage envelope and the production memory term"). An operator who copies the micro-Uchuu examples from that README verbatim exhausts the internal container part way through a multi-day scatter. Those examples use `output/convert/...` as a **development convenience on a 22.6-million-halo dataset**; they are not the production path. Pin the production paths explicitly:

```text
workdir:             /Volumes/LaCie/convert/shin-uchuu
production dataset:  /Volumes/LaCie/data/shin-uchuu/production-snapshot   (write --output-dir)
retained subset:     /Volumes/LaCie/data/shin-uchuu/subset-snapshot       (must survive, P3b step 3)
```

**Step 0 — fetch the production `forests.list` before anything else.** Both `scatter` and `finalize` take it as a **required** argument (`convert_ctrees.py:94`, `:131`), and the production index lives only on OzSTAR: **7.56 GB, 315,004,242 lines** (recorded in "Source Data Summary" above). P1's transfer scope is the 11.61 TB of `tree_*.dat`, so this file is **not** covered by it — at the measured ≈110 MB/s it is **7.56 GB ÷ 110 MB/s = 68.7 s ≈ 1.1 min** on its own, negligible against P1's ≈29 h, and without it the very first batch-mode `scatter` cannot be issued at all. **The rehearsal subset's `forests.list` is a subset index and is unusable here**: `validate_root_coverage()` enforces one-to-one coverage between the observed `#tree` roots and the listed roots, and surplus listed roots abort as loudly as missing ones (`scripts/convert/scatter.py:192-216`, called at finalize from `:1174`). Confirm the local `a_list` and `simulation_info.yaml` paths in the same pass — `simulations/shin-uchuu/shin-uchuu.a_list` and `simulations/shin-uchuu/simulation_info.yaml`, both already in the repository.

**Step 1 — preflight the volume, and record what it says.** The storage envelope projects a **6.89 TB peak against a 7.0 TB policy ceiling**, binding at scatter through the staged source batch, so **require ≥ 7.0 TB free on the workdir volume before the first batch** and re-check between batches:

```bash
readlink output || echo "no output symlink"   # confirm you are NOT about to use /Volumes/Internal
df -k /Volumes/LaCie                          # need >= 7.0 TB free; measured 7.71 TB on 2026-08-29
df -k /Volumes/Internal                       # 1.17 TB — for contrast; not usable for this conversion
```

**Step 2 — the three flags.** Each is per invocation, none has manifest state, and each omission is either a wasted day or a hard error:

| Flag | Subcommand | Why |
|---|---|---|
| `--pool-size 8` | **`scatter` only** (`convert_ctrees.py:99`) | Defaults to `1`. Omitted, scatter runs serially — ≈68.9 h against ≈44.9 h, about 24 h across the source, and undetectable from the manifest afterwards |
| `--output-dir <dataset>` | **`write` only** (`convert_ctrees.py:189-193`) | It is **not** a `scatter` flag; passing it there is an argparse error. Defaults to `<workdir>/hdf5`, which consumptive deletion empties and the operator then tears down (P3b step 1) |
| `--multiplier 20000000000` | **`report`** (`convert_ctrees.py:203-208`) | Defaults to 10⁹ (`scripts/convert/validate.py:69-70`), which **fails the C15 rank bound at production scale** and makes `report` exit 1. `type=int`, so `2e10` is rejected — pass the integer |

**The whole sequence, as runnable commands, is `scripts/convert/README.md` → "The production sequence, end to end"** — `scatter --batch --pool-size 8` → `release` (per batch) → `finalize` → `sort` → `fixups --consume-intermediates` → `links --consume-intermediates` → `write --consume-intermediates --output-dir` → `report --multiplier`. The order is not a convention: `write` refuses any snapshot whose status is not `linked` (`scripts/convert/hdf5_writer.py:436-440`), and `sort`/`fixups`/`links` are what produce that status.

**Step 3 — the production `report` invocation, in full.** This is the command P3, P3b, P5 and P7 all depend on the output of:

```bash
mimic_venv/bin/python scripts/convert/convert_ctrees.py report \
    --workdir /Volumes/LaCie/convert/shin-uchuu \
    --a-list simulations/shin-uchuu/shin-uchuu.a_list \
    --multiplier 20000000000
```

It exits 1 if the producer battery failed (`convert_ctrees.py:273-280`); **treat a non-zero exit as a stop, not a warning.** Then copy `conversion_report.json` and `conversion_report.txt` out of the workdir before the workdir is torn down (P3b step 2), and assert the snapshot-span gate below from the report's own per-snapshot counts.

### P3b — connecting P2's output to what P6 reads (do not skip this)

**P2 produces a dataset. P6 reads whatever `simulations/shin-uchuu/snapshots` points at. Today those are different datasets, and nothing in this sequence connects them automatically.** The symlink points at `/Volumes/LaCie/data/shin-uchuu/subset-snapshot` — the retained **406,668,896-halo rehearsal subset, 1.8% of production**. Verify with `readlink simulations/shin-uchuu/snapshots` before and after this step. `simulations/shin-uchuu/README.md` already states the requirement ("After the production conversion, `snapshots/` is re-pointed at the production dataset"); this step is where it is executed and checked.

**Nothing raises an error if the re-point is forgotten.** The subset is a structurally valid dataset of the same format, and the reader's open-time checks — `format_version`, `links_adjacent`, exact `scale_factor` agreement with the package `a_list`, identity-multiplier bounds, and agreement of the five physical header values with the package — **all pass on the subset**, because it was converted from the same box with the same cosmology, box size, particle mass and `a_list`. So P6 starts normally, finishes in hours instead of days, and emits subset science under a production label. Header agreement is not the discriminator; **per-snapshot halo counts are**.

**1 — Name the production output directory at P2; do not let it default.** `write --output-dir` defaults to `<workdir>/hdf5` (`scripts/convert/convert_ctrees.py:189-193`, resolved at `scripts/convert/hdf5_writer.py:446`), and the workdir is exactly what consumptive deletion empties and the operator then tears down. Use a permanent path beside the retained subset so the two are distinguishable by name:

```text
production dataset:  /Volumes/LaCie/data/shin-uchuu/production-snapshot
retained subset:     /Volumes/LaCie/data/shin-uchuu/subset-snapshot     (must survive)
```

It sits on the same volume the storage envelope is computed against, and that envelope already counts the emitted dataset (`scripts/convert/README.md` → "Storage envelope and the production memory term", the `emitted dataset` and `report / validate` rows), so this placement is envelope-neutral. `write` records the resolved path into the manifest as `outputs_dir` and `report` copies it into `conversion_report.json` (`scripts/convert/hdf5_writer.py:488`, `scripts/convert/report.py:126`) — that is what makes the re-point *checkable* rather than remembered.

**2 — Copy the conversion report out of the workdir before tearing the workdir down.** `report` writes `conversion_report.json` and `conversion_report.txt` **under the workdir** (`scripts/convert/report.py:13-14`, `:31-32`), not under the output directory. P3, P3b, P5 and P7 all depend on it. Copy both next to the dataset first.

**3 — Preserve the subset; never write through the existing symlink.** `--output-dir simulations/shin-uchuu/snapshots` (or any path resolving through it) writes the production dataset into `subset-snapshot` and destroys the rehearsal artifact. That subset is the anchor for every rehearsal-scale measurement in this document and for `simulations/shin-uchuu-ascii/`'s paired ASCII copy; regenerating it means re-running selection, extraction and conversion.

**4 — Re-point, then verify against the report.** Replace the symlink and check per-snapshot counts:

```bash
DS=/Volumes/LaCie/data/shin-uchuu/production-snapshot
ln -sfn "$DS" simulations/shin-uchuu/snapshots
readlink simulations/shin-uchuu/snapshots        # must print $DS, not .../subset-snapshot
```

```bash
mimic_venv/bin/python - "$DS" "$DS/conversion_report.json" <<'PY'
import json, pathlib, sys
import h5py

ds = pathlib.Path(sys.argv[1]).resolve()
report = json.load(open(sys.argv[2]))
assert pathlib.Path(report["outputs_dir"]).resolve() == ds, "report is not for this dataset"
assert report["validation_passed"], "conversion report records a FAILED producer battery"

mismatched, missing = [], []
for snap, entry in sorted(report["per_snapshot"].items(), key=lambda kv: int(kv[0])):
    path = ds / "snapshot_{:03d}.h5".format(int(snap))
    if not path.exists():
        missing.append(snap)
        continue
    with h5py.File(path, "r") as handle:
        header = handle["/header"].attrs
        n_halos = int(header["n_halos"])
        forests = int(header["n_forests_total"])
        max_rank = int(header["max_halo_rank_in_forest"])
    if n_halos != entry["rows"]:
        mismatched.append((snap, n_halos, entry["rows"]))

print("snapshots in report:", len(report["per_snapshot"]))
print("missing files:", missing)
print("count mismatches:", mismatched[:10], "of", len(mismatched))
print("forests.h5 present:", (ds / "forests.h5").exists())
print("n_forests_total:", forests, "report:", report["n_forests_total"])
print("max_halo_rank_in_forest:", max_rank, "report:", report["max_halo_rank_in_forest"])
print("total halos:", report["totals"]["halos"])
PY
```

**The acceptance rule.** The report's own `validation_passed` is **true** — the script asserts it first, and a re-point that proceeds on a failed report is the same silent-wrong-dataset failure this whole step exists to prevent (`report` already exits 1 on it at P2 step 3, so a `false` here means that exit was ignored) — then 70 snapshot files present, zero missing, zero count mismatches, `forests.h5` present, the two run-scoped identity values equal to the report's, and `totals.halos` on the order of 22.9 × 10⁹ rather than 4.07 × 10⁸. **The total-halo figure is the one-line sanity check that separates the two datasets** — if it reads 406,668,896 you are still on the subset. Confirm also that the multiplier set at P3 exceeds the `max_halo_rank_in_forest` printed here (C15); it is the same check the reader makes at startup, done early rather than at the top of a multi-day run.

**5 — `simulations/shin-uchuu-ascii/snapshots` stays on the subset**, at `/Volumes/LaCie/data/shin-uchuu/subset-ascii`. That package declares `tree_type: consistent_trees_ascii` and is usable *only* on a subset: C5/C6 put the production super-forest ≈15.9 TB beyond the tree driver's reach, so there is no production ASCII dataset for it to point at and there never will be. Its `unique_galaxy_id_multiplier` still moves at P3 — the two packages must declare the identical value or they compute different `UniqueGalaxyID`s and the cross-format identity gate compares incomparable sets.

**What skipping this costs, on both branches.** Left alone, P6 runs the multi-day "production" job against the 1.8% subset: it finishes early, passes every check, and yields wrong science with nothing in the logs to say so. Written through, the production conversion overwrites the retained subset, and the rehearsal artifact this document's measured record rests on is gone.

### The transfer host (P1) — four operational facts about `nt.swin.edu.au`

These are properties of the machine and its shell, not of Mimic, and each one silently breaks a plausible P1 command rather than failing loudly.

- **`nt.swin.edu.au` round-robins two login nodes**: `tooarrana1` = 136.186.1.203 and `tooarrana2` = 136.186.1.204, 4 cores and 251 GB each. A `tmux` session lives on **one** node, so a detached transfer job must be launched against a **pinned IP** or a reconnect can land on the other node and find nothing running. Sentinel files written under `/fred` are shared between the nodes and poll correctly from either, so use a `/fred` sentinel for completion rather than a process check.
- **macOS ships `openrsync`, not GNU rsync**, and it does **not** support `--info=progress2`. Use plain `rsync -aL` per file, or `scp`; a script written against GNU rsync's progress output fails on the local side of the transfer.
- **The remote shell is zsh with `nomatch` set**, so `ls /path/glob_*` **errors** when nothing matches instead of printing nothing. A poll loop of the form `ls … | wc -l` therefore reports 0 both when no files exist yet and when the path is wrong, and silently breaks on the error path. Use `find <dir> -name 'glob_*' | wc -l` instead.
- **`pkill -f "<pattern>"` run over ssh kills its own shell** whenever the remote command line contains the pattern — the `ssh` then returns 255. Match on something narrower than the invocation, or stop the job with `tmux kill-session`.

### P4 — the `Spin` pre-run scan, its acceptance rule, and what to do if it fails

**Why it is a separate scan.** `Spin` is the only declared property range whose production value can be checked *before* the run, because it comes from the converted catalog rather than from Mimic's physics. The conversion report carries totals, per-snapshot counts and identity bounds but no value extrema (`build_report`, `scripts/convert/report.py:94-143`), so the report cannot answer it.

**The bound, and the expectation to test it against.** Both Shin-Uchuu packages declare `range: [-1000.0, 1000.0]` for `Spin` — `simulations/shin-uchuu/halo_properties.yaml:113` and `simulations/shin-uchuu-ascii/halo_properties.yaml:108` (decision D7). Two figures bound what to expect, and they are at different scales:

- **Rehearsal scale (406,668,896 halos, the subset):** measured `[-11.673591, +17.797567]`, zero non-finite (`POST-PHASE-5-WORK.md` §2.1, §6 item 2), and **re-measured on 2026-08-29 over the retained rehearsal dataset, reproducing both extrema and the zero non-finite count exactly** — which is where this step's ≈20 min cost estimate comes from. Its ~56× margin is **an artefact of the subset** — the subset excludes the percolation super-forest and so tops out at 1.2177 × 10¹³ Msun/h against the box's sampled ≥1.2288 × 10¹⁵. Do not read it as a production projection.
- **Production expectation:** the mass-matched anchor is micro-Uchuu's 270.30 at a 3.09 × 10¹⁴ Msun/h maximum, scaled by Mvir^(2/3), which puts the production maximum near **680** against the declared 1000 — a **~1.5× margin**, not 56× (`POST-PHASE-5-WORK.md` §6, the item-2 carry-forward).

**`Spin` is a 3-vector, not a scalar column, and every recorded extremum is over all three components.** It is declared `type: vec3_float` (`simulations/shin-uchuu/halo_properties.yaml:106-107`), written as `np.column_stack((fixed["Jx"], fixed["Jy"], fixed["Jz"]))` (`scripts/convert/hdf5_writer.py:198`), and read back as `float32` with shape `(3,)` per row by the cross-check's field table (`scripts/convert/crosscheck.py:139`). `/halos/Spin` therefore has shape `(N, 3)`: the retained rehearsal dataset's 406,668,896 halos hold **1,220,006,688** `Spin` values, which is the population the rehearsal extrema were measured over (`POST-PHASE-5-JOINT-REVIEW.md` §6 item 2 records exactly that count). Any scan that treats the dataset as one value per halo reads a third of it.

**The scan.** One pass over `/halos/Spin` (float32`[N,3]`, `docs/dev/SNAPSHOT-HDF5-FORMAT.md`) in each of the 70 `snapshot_NNN.h5` files, chunk-wise so it stays bounded. It is written for the `(N, 3)` shape: it takes the halo count from `spin.shape[0]`, chunks by row so a chunk is 3 × its row count in values, and reduces `min`/`max`/`isfinite` over every component of every row — so `halos scanned` below is a halo count and `non-finite components` is a component count.

```bash
mimic_venv/bin/python - /Volumes/LaCie/data/shin-uchuu/production-snapshot <<'PY'
import pathlib, sys
import numpy as np
import h5py

ds = pathlib.Path(sys.argv[1])
lo, hi, nonfinite, total = np.inf, -np.inf, 0, 0
for snap in range(70):
    path = ds / "snapshot_{:03d}.h5".format(snap)
    with h5py.File(path, "r") as handle:
        spin = handle["/halos/Spin"]
        n = spin.shape[0]
        total += n
        for start in range(0, n, 4 << 20):          # 4 Mi rows per chunk
            block = spin[start : start + (4 << 20)]
            finite = np.isfinite(block)
            nonfinite += int(finite.size - finite.sum())
            if finite.any():
                lo = min(lo, float(block[finite].min()))
                hi = max(hi, float(block[finite].max()))
print("halos scanned:", total)
print("Spin min/max:", lo, hi)
print("max |Spin|:", max(abs(lo), abs(hi)))
print("non-finite components:", nonfinite)
PY
```

**The acceptance rule.**

| Scan result | Verdict |
|---|---|
| `max abs(Spin) < 1000` and zero non-finite | **Pass — proceed to P5.** Record the extrema; they are the first production-scale measurement of this quantity |
| `max abs(Spin)` in `[680, 1000)` | **Pass, but record it explicitly** — the margin is thinner than the anchor predicted, and the number becomes the basis for any future bound decision |
| `max abs(Spin) ≥ 1000`, **finite** | **Range exceeded — see below.** **Blocks P6** until the declared range is widened on the measured extrema, `make generate` re-run and the binary rebuilt; **no re-conversion** |
| **Any non-finite component** (`inf` or `NaN`) | **HARD STOP before P6, pending diagnosis.** Not a range question — see below |

**These are two different findings and they are deliberately not one row.** A finite exceedance says the declared bound was set too tight; a non-finite value says the *producer* emitted something no bound can describe, and widening a range does not make it representable. Both are reachable: `normalise_spin` computes `J[k] × (1/Mvir)` in float64 and casts to float32 under `np.errstate(over="ignore")`, deliberately carrying a float32-overflowing result the way the reference `(float)` cast would (`scripts/convert/fixups.py:166-191`), so **finite inputs can produce `±inf` here**.

**A finite exceedance blocks P6 until the metadata is widened and regenerated, but it requires no re-conversion.** Those are two halves of one rule and they rest on different facts. *No re-conversion*, because the `Spin` bound is **validation-tier**: declared ranges are consumed from `tests/generated/property_ranges.json` by `test_physical_ranges()` (`tests/scientific/test_scientific.py:399`, manifest path at `:61`) and by no runtime assertion, so an exceedance says the declared bound is wrong, not that the emitted values are — the dataset is fine and stays. *Blocks P6*, because that same fact means nothing in the run reports the violation: launching on a bound already known to be wrong spends 8–16 h to produce output whose range check is guaranteed to fail afterwards, while widening and regenerating costs minutes. The response is therefore, in this order and **before** P6 is started:

1. Widen the declared range in **both** `simulations/shin-uchuu/halo_properties.yaml` and `simulations/shin-uchuu-ascii/halo_properties.yaml`, keeping the two consistent — the ASCII package stays on the subset (P3b step 5) but describes the same box, and D8's precedent is that a `Spin` change is applied across packages rather than to one.
2. Re-run `make generate MODEL=sage16 SIMULATION=shin-uchuu` so `property_ranges.json` carries the new bound, then rebuild before P6.
3. Record the **measured** extrema as the basis for the new value, the way D7 was set. Do not widen on an extrapolation.

**Non-finite values are a different finding and are not a range question.** The rehearsal measured zero of them over 406,668,896 halos; any at production points at the producer's `Spin` normalisation (`docs/dev/SNAPSHOT-HDF5-FORMAT.md` records that zero-mass halos' components are carried unnormalised), not at the declared bound. Investigate rather than widen.

### P5 — re-projecting the run's memory peak, and what "materially higher" means as a number

**Use the measured-RSS fit, not the parametric form.** The fit is (`POST-PHASE-5-WORK.md` §2.2, correction 1 at `:195`):

> **RSS = 1.221 GB + 960.9 B/halo × N + 2 × 176 B × pool_allocated(N)**

It reproduces both measured `sage16` points exactly — **2.184 GB at N = 621,360** (micro-Uchuu scale) and **14.301 GB at N = 9,006,294** (rehearsal scale) — and it separates the galaxy pool's *allocated* slots from its high-water `G`, because pool slack is 27–39% at those scales and only ~1% at production. A single naive slope through the two points overstates the production peak; the effective total slope falls from 1,550 B/halo at micro-Uchuu to 1,452 at the rehearsal and **1,322 at production**.

**What N to use, and why it is not the z=0 slab.** The peak is set by the **largest** slab, not z=0: the rehearsal's largest was snapshot 44 at 1.1258× its own z=0 slab, on a broad plateau (snapshots 41–48 within 1% of each other), so both live generations sit at it. The projected production largest slab is **N ≈ 3.546 × 10⁸**, and the whole point of P5 is that the report replaces this projection with the real per-snapshot counts. Take `N = max(entry["rows"] for entry in report["per_snapshot"].values())` from `conversion_report.json` — the same file P3b step 2 copied out of the workdir — and note whether the plateau is as broad as the rehearsal's. That expression is valid against the report schema as written: `build_report` fills `per_snapshot` as a **dict** keyed by the string snapshot number, one entry per a_list snapshot with explicit zeros for empty ones, each carrying exactly `rows`, `flyby_demotions` and `len_zero_count` (`scripts/convert/report.py:107-114`).

**The point forecast this replaces.** At N ≈ 3.546 × 10⁸ the fit gives **470.4 GB**, and **≈476.6 GB** after §6 item 11's 5% output-buffer seed headroom. `C`/N moves from the measured 1.00011 to 1.05, which costs **176 B × 0.05N ≈ 3.1 GB per generation and ≈6.2 GB across the two live generations** — that total is the 470.4 → 476.6 GB increment, and it is the only figure this document quotes for the seed headroom. (JR §6 item 11's original "≈2–3 GB per generation" was the pre-measurement estimate; the measured 3.12 GB per generation supersedes it — `POST-PHASE-5-JOINT-REVIEW.md:296`.) 476.6 GB is the figure the steps table carries for P6. Including the seed headroom, the production form is linear in the slab count:

> **peak ≈ 1.221 GB + 1,340.6 B/halo × N**  — derived here from the two figures above ((476.6 − 1.221) × 10⁹ ÷ 3.546 × 10⁸), not a new measurement

**"Materially higher" as a number: a re-projected peak above ≈500 GB, i.e. a largest slab above ≈3.72 × 10⁸ halos — and there is an upper bound at ≈589 GB above which the mitigation no longer closes the gap.** The derivation of both, from figures already recorded in `POST-PHASE-5-WORK.md` §2.2:

- The **435 GB trigger is already crossed** by the point forecast (476.6 GB, over by ≈41.6 GB) and the compact previous-slab projection was **deferred by owner decision anyway** (`:208`). So the trigger is not the operative discriminator at P5 — it has already fired, and re-firing it says nothing new.
- The trigger is also more conservative than it reads: `hw.memsize` is 549,755,813,888 B = 512 GiB = **549.76 GB** on this document's 10⁹-byte scale, so "85% of 512 GB" is really 79% of capacity (`:210`). On the otherwise-idle box P6 already requires, non-Mimic committed memory is **~37.7 GB**, leaving Mimic **~540 GB** (`:212`).
- The deferred mitigation is worth **≈39.7 GB at the projected slab**: the compact `{int32_t Len, int32_t NextProgenitor}` projection saves 80 B/halo ≈ 28.4 GB (470.4 → ~442.0 GB), and also releasing the retained generation's two int64 identity arrays and its `SnapshotHaloAux` (32 B/halo, 11.3 GB) reaches **~430.7 GB** (`:208`). **The credit is 112 B/halo (80 + 32) and therefore scales with N** — 112 B × 3.546 × 10⁸ = 39.7 GB reproduces the recorded figure, but it is not a fixed 39.7 GB at a larger slab. That is what sets the upper bound below.
- Therefore the deferral stands while the re-projected peak leaves at least that much slack against the ~540 GB actually available, and becomes **mandatory** above **540 − 39.7 ≈ 500 GB** — the point at which the deferred change is the only specified lever that brings the run back under the box. Inverting the linear form, 500 GB corresponds to **N ≈ 3.72 × 10⁸**, i.e. **4.9% above the projected 3.546 × 10⁸**.
- **The mitigation also has a ceiling, and above it there is no specified lever left.** Subtracting the 112 B/halo credit from the linear form gives the post-mitigation projection **peak_mitigated ≈ 1.221 GB + 1,228.6 B/halo × N**. Setting that equal to the ~540 GB available and solving: N = (540 − 1.221) × 10⁹ ÷ 1,228.6 ≈ **4.385 × 10⁸**, which is **23.7% above the projected 3.546 × 10⁸**. The *unmitigated* forecast at that same slab is 1.221 + 1,340.6 × 4.385 × 10⁸ ≈ **589 GB**. So an unmitigated re-projection at or above ≈589 GB is **still over the box after the mitigation is applied**, and reinstating the compact projection would be a fix that cannot close the gap.

**So the P5 decision is a three-way branch on the re-projected peak.** Re-project with the report's real largest slab, then:

| Re-projected unmitigated peak | Largest slab N | Action |
|---|---|---|
| Below **≈500 GB** | below ≈3.72 × 10⁸ | **Proceed to P6** on a lean box, deferral intact |
| **≈500 – 589 GB** | ≈3.72 – 4.385 × 10⁸ | **Reinstate the compact previous-slab projection before P6.** A mid-run OOM on a serial, no-resume, multi-day job is a full restart (C10) |
| **At or above ≈589 GB** | at or above ≈4.385 × 10⁸ | **Stop. Do not launch, and do not reinstate the mitigation and proceed** — by the arithmetic above the post-mitigation peak is still over the ~540 GB available. **Escalate:** this needs a larger host, or a slab strategy this document does not specify (streaming the slab, splitting the largest snapshot, or a smaller retained generation). Reinstating the compact projection here buys ≈49 GB against a ≥49 GB shortfall and closes nothing |

**Record the re-projected figure in every branch** — it is what the production run's own measured RSS is checked against.

**Both thresholds are derived here, not owner decisions.** The ≈500 GB and ≈589 GB cut-overs are arithmetic on figures already recorded in `POST-PHASE-5-WORK.md` §2.2 and `:208`–`:212`, computed in this document. The only recorded owner decision in this area is the deferral of the compact previous-slab projection (2026-08-26, "Delegated obligation: the snapshot-driver memory fallback" above). If a re-projection lands in the second or third band, that owner decision is what is being revisited — treat the thresholds as the trigger to take it back to the owner, not as a standing authority to proceed.

**One thing the fit cannot absorb.** `sage16`'s measured `P`/N = 0.99701 sits 0.3% below the output buffer's growth threshold. Crossing it steps `C`/N to 1.5 — roughly 55 GB more resident **plus** a `realloc` transient briefly holding both blocks — a cliff no amount of free RAM absorbs. Item 11's 5% seed headroom is what removes it, at the ≈3.1 GB per generation / ≈6.2 GB total already counted in the 476.6 GB forecast above; confirm it is in the build P6 uses.

### P6 — building and running the production `sage16` job

**`MODEL` and `SIMULATION` are compile-time, so the binary must be built for this pair (C11).** Mimic compiles one model package against one simulation package (`Makefile:47-78`, `:155-157`); there is no runtime switch. Use the **same** selectors for `generate` and for the build — mixing them produces silently inconsistent generated code.

```bash
# 1. Regenerate from the YAML edited at P3 (multiplier) and possibly P4 (Spin range).
make generate MODEL=sage16 SIMULATION=shin-uchuu

# 2. Build. USE-HDF5 defaults to yes (Makefile:216-217) and is required —
#    snapshot-ordered runs are HDF5-only (C10).
make MODEL=sage16 SIMULATION=shin-uchuu -j$(sysctl -n hw.ncpu)

# 3. Confirm what was actually built before committing days to it.
make info MODEL=sage16 SIMULATION=shin-uchuu     # HDF5 ✓, compiler, features
```

```bash
# 4. Run. Serial only — no mpirun, no --skip; both are config-time rejections (C10).
#    Detached, because this runs for hours and no command cap survives it — and
#    wrapped, because a bare `nohup ... &` discards the exit status that P7 gates on.
#    The wrapper writes the true exit code to a sentinel, atomically, on every path.
RUNDIR=/Volumes/LaCie/runs/shin-uchuu-p6

# 4a. Guarantee the run directory starts EMPTY. A repeat attempt — and a failed
#     first attempt is a full restart (C10), so repeats are expected — must not
#     leave the previous attempt's exit.code where the poll below can read it:
#     a stale `0` makes a running job look finished and lets P7 start on it.
#     Refuse to touch the directory while a previous attempt is still live —
#     moving it out from under a running job loses that job's log and sentinel.
pgrep -x mimic > /dev/null && { echo "a mimic process is already running"; exit 1; }
if [ -e "$RUNDIR" ]; then
  mv "$RUNDIR" "$RUNDIR.$(date -u +%Y%m%dT%H%M%SZ)"    # keeps the previous run.log
fi
mkdir -p "$RUNDIR"
test ! -e "$RUNDIR/exit.code" || { echo "sentinel present in $RUNDIR — do not launch"; exit 1; }

nohup sh -c '
  ./mimic models/sage16/input/sage16_shin-uchuu.yaml > "$0/run.log" 2>&1
  printf "%d\n" "$?" > "$0/exit.tmp"
  mv "$0/exit.tmp" "$0/exit.code"
' "$RUNDIR" > /dev/null 2>&1 &
```

**Poll the sentinel, not the process.** `exit.code` appears exactly once, only when *this* run has finished — and that holds only because step 4a guarantees the directory started without one. The `mv` inside the wrapper is what makes a half-written file unobservable; the `mv` in step 4a is what makes the file's presence mean *this* attempt rather than a previous one. Without 4a the polls below are not a completion test at all, they are a test that some attempt once finished.

```bash
test -f "$RUNDIR/exit.code" && cat "$RUNDIR/exit.code"    # absent = still running
```

**P7 does not start until that file reads `0`.** A failed run leaves the partitions it had already finished **intact and complete on disk**: `bye()` unlinks only the partition in flight, "so a crash never leaves partial output files behind **and never deletes completed ones**" (`src/core/tree_driver.c:42-46`, `tree_driver_remove_incomplete_outputs()` at `:59-65`). Every surviving file is therefore individually well-formed, and no per-file check distinguishes that set from a complete run's — which is why P7 asserts the exact expected snapshot set as well as the exit code. Diagnose from `run.log` and re-run: snapshot-ordered runs are not resumable (C10), so a failed run is a full restart, not a continuation.

**Before starting it, four checks that each cost seconds and each save days:**

1. `readlink simulations/shin-uchuu/snapshots` prints the production dataset (P3b). **This is the one that silently produces wrong science.**
2. `simulation_info.yaml` declares the P3 multiplier, and it exceeds the report's `max_halo_rank_in_forest` (C15).
3. `output.snapshot_list` in the run file is the set P8 actually needs — see P8, **including the operator decision flagged there**, which is open and must be settled before this point. Output snapshots are frozen at launch and the run is not resumable (C10), so a missing snapshot means re-running the whole job.
4. The box is otherwise idle: **≈476.6 GB projected peak** (P5) against ~540 GB available. Close applications, no local LLM, nothing else resident.

**Note on the run file's own title.** `models/sage16/input/sage16_shin-uchuu.yaml:1` describes itself as the "rehearsal subset" configuration. Which dataset it reads is decided **entirely** by the `snapshots` symlink, not by the run file, so that comment is stale rather than wrong about behaviour — do not treat it as evidence that the run is pointed at the subset, and do not treat re-pointing the symlink as something the run file records.

**Expected wall clock is ≈8–16 h, and that is an estimate rather than a scaled measurement.** The rehearsal `sage16` run's wall clock was never committed to this repository — no document or commit records it — so there is no figure here to scale by the 56.3× production/rehearsal halo ratio. Treat the range as an order-of-magnitude planning bound only, and **record the production run's own wall clock**, which is the first committed measurement of this quantity. `print_run_memory_profile()` reports `C`, `P`, `G`, pool slack and peak RSS at run end (`src/util/run_profile.h`); capture it, because it is the measurement P5's re-projection is validated against.

### P7 — the `deltaMvir` post-run range validation

**Why it can only be a post-run scan.** `deltaMvir` is created during inheritance (`src/core/inheritance.c:56`, `:82`) and exists only in Mimic's output, never in the input catalog — so unlike `Spin` there is nothing to scan beforehand. The bound is `range: [-20000.0, 20000.0]` in units of 1e10 Msun/h (`src/core/core_properties.yaml:143`).

**The expectation, at two scales.** The rehearsal measured `[-58.55, +78.72]` for `sage16` — **a rehearsal-scale figure**, 254× inside the bound, and that margin belongs to the subset, whose most massive halo is only 1.2177 × 10¹³ Msun/h. The honest anchor is micro-Uchuu, which reaches `[-2409, +3860]` at a 3.09 × 10¹⁴ Msun/h maximum; `deltaMvir` tracks mass and the production box is 3.98× more massive at the top, so a **production** maximum of **1.5–2 × 10⁴ is plausible — at or near ±20000** (`POST-PHASE-5-JOINT-REVIEW.md` F-10 / §6 item 9, `POST-PHASE-5-WORK.md` §2.5). The bound was **deliberately not widened** on that extrapolation.

**The scan.** A snapshot-ordered run writes **one `model_NNN.hdf5` per requested output snapshot, carrying that snapshot and nothing else**, where `NNN` is the snapshot *number* rather than a dense index — `snapshot_output_partition_output_id()` returns `MimicConfig.ListOutputSnaps[partition]` (`src/core/tree_driver.c:608-617`), and the filename is formatted at `src/io/output/util.c:36`, alongside the master `model.hdf5` (`src/io/output/master_hdf5.c:36`). Galaxies live in the compound dataset `SnapNNN/Galaxies` (`plot/mimic-plot/hdf5_reader.py:32`, `:42-44`).

**Two preconditions, both asserted before a byte is scanned.** First, **P6 exited 0** — read the P6 sentinel, do not infer success from the presence of output. Second, the emitted files are **exactly** the run file's `output.snapshot_list`, no more and no fewer; a failed run's surviving partitions are complete files (see P6), so scanning "whatever is there" validates a truncated run and reports it clean.

```bash
test "$(cat /Volumes/LaCie/runs/shin-uchuu-p6/exit.code)" = "0" || {
  echo "P6 did not exit 0 — do not run P7"; exit 1; }
```

```bash
# Second argument: the run file's output.snapshot_list, as emitted-file numbers.
mimic_venv/bin/python - output/sage16-shin-uchuu "69,52,43,40,20,10,5,2,1,0" <<'PY'
import pathlib, sys
import numpy as np
import h5py

outdir = pathlib.Path(sys.argv[1])
expected = {int(s) for s in sys.argv[2].split(",")}
found = {int(p.name[6:9]) for p in outdir.glob("model_[0-9][0-9][0-9].hdf5")}
assert found == expected, "snapshot set mismatch: missing {}, unexpected {}".format(
    sorted(expected - found), sorted(found - expected)
)

LO, HI = -20000.0, 20000.0
for path in sorted(outdir.glob("model_[0-9][0-9][0-9].hdf5")):
    with h5py.File(path, "r") as handle:
        for group in handle:
            if not group.startswith("Snap"):
                continue
            data = handle[group]["Galaxies"]
            lo, hi, bad, nonfinite, n = np.inf, -np.inf, 0, 0, data.shape[0]
            for start in range(0, n, 1 << 22):
                block = data[start : start + (1 << 22)]["deltaMvir"]
                finite = np.isfinite(block)
                nonfinite += int(finite.size - finite.sum())
                block = block[finite]
                if block.size:
                    lo = min(lo, float(block.min()))
                    hi = max(hi, float(block.max()))
                    bad += int(((block < LO) | (block > HI)).sum())
            print(path.name, group, "n=", n, "min/max=", lo, hi,
                  "out-of-range=", bad, "non-finite=", nonfinite)
PY
```

The expected set above is written as the shipped `[69, 40, 20, 10, 5, 2, 1, 0]` plus the 52/43 pair P8 records as the pre-P6 default. **Copy it from the run file P6 actually used**, not from here — the z = 1 / z = 2 pair is an open operator decision (P8), and hard-coding the wrong pair turns this assertion into a false alarm.

**The acceptance rule.** P6 exited 0, the emitted snapshot set matches the run file exactly, and zero out-of-range and zero non-finite values across every emitted snapshot. **The range part is validation-tier** — the same `test_physical_ranges()` path as P4 — so a violation is a failed test on a finished run, not an aborted one.

**If it is exceeded, do not reflexively widen the bound.** `deltaMvir` is a **core** property, so widening it moves every simulation package's contract, and the joint review's position is that the range still has real discriminating power. Establish first whether the extremum is a genuine Uchuu-scale mass swing — check the offending galaxy's `Mvir` and `CentralMvir` against the box's maximum — or a defect. Only then decide, and record the measured basis either way.

### P8 — the science checks: entry point, plot selection, and the snapshot problem

**The entry point** is `plot/mimic-plot/mimic-plot.py`, driven from the same run file the run used (`plot/mimic-plot/README.md` → Usage). HMF and GSMF are the snapshot-tier figures `halo_mass_function` and `stellar_mass_function` (`models/sage16/plots/figures/`, listed in `models/sage16/plots/profiles/default.yaml`), and `--snapshot` selects one snapshot number at a time (`mimic-plot.py:996-998`):

```bash
cd plot/mimic-plot

# PLOTROOT must be ABSOLUTE. A relative --output-dir is resolved against the
# repository root, not the current directory (`resolve_relative_path`,
# mimic-plot.py:195-212) — from inside plot/mimic-plot that is a silent surprise.
PLOTROOT=/absolute/path/to/shin-uchuu-p8-plots

# The whole step runs in a subshell so a failed assertion sets $? without
# closing an interactive shell.
(
  # PLOTROOT must be FRESH. Six stale PDFs from an earlier attempt would
  # satisfy the assertion below while this run produced nothing, so refuse to
  # start unless the root is absent or empty.
  if [ -n "$(ls -A "$PLOTROOT" 2>/dev/null)" ]; then
    echo "PLOTROOT is not empty: $PLOTROOT — move it aside or pick a new path" >&2
    exit 1
  fi

  # 69 52 43 is the documented default pair plus z=0. If the operator chose the
  # bracketing pair (53 44) instead, use that — take the list from the run file
  # P6 actually used, not from here. See the operator decision below.
  for SNAP in 69 52 43; do
    ../../mimic_venv/bin/python mimic-plot.py \
      --param-file=../../models/sage16/input/sage16_shin-uchuu.yaml \
      --plots=halo_mass_function,stellar_mass_function \
      --snapshot-plots \
      --output-dir="$PLOTROOT/snap$SNAP" \
      --snapshot=$SNAP --format=.pdf --verbose || exit 1
  done

  # The six exact paths this loop is supposed to have written — by name, not by
  # a recursive PDF count. Missing any of them exits non-zero.
  status=0
  for SNAP in 69 52 43; do
    for FIG in HaloMassFunction StellarMassFunction; do
      PDF="$PLOTROOT/snap$SNAP/$FIG.pdf"
      if [ -s "$PDF" ]; then
        echo "ok      $PDF"
      else
        echo "MISSING $PDF"
        status=1
      fi
    done
  done
  if [ "$status" -ne 0 ]; then
    echo "P8 PLOTS MISSING" >&2
    exit 1
  fi
  echo "P8 plots OK"
)
```

**Two things in that loop are load-bearing; without either, it silently produces the wrong result.**

**`--output-dir` per snapshot, because the figure filenames carry no snapshot number.** Both figures save under a fixed name — `save_and_close_figure(fig, output_dir, "HaloMassFunction", …)` at `models/sage16/plots/figures/halo_mass_function.py:111` and `"StellarMassFunction"` at `models/sage16/plots/figures/stellar_mass_function.py:233-234`. Pointed at one directory, each iteration overwrites the last, and after the loop **only snapshot 43's two PDFs exist** while the operator believes six were produced — destroying the z = 0 and z = 1 checks, which are the point of the whole pathway. `--output-dir` is a real flag (`mimic-plot.py:1008-1011`, documented at `plot/mimic-plot/README.md:118`) and the directory is created if absent (`os.makedirs(..., exist_ok=True)`, `:1215-1216`, `:1224`), so a per-snapshot subdirectory is all that is needed. The assertion block above is the check that this actually happened; a bare eyeball of the last run's summary cannot distinguish six figures from two.

**The assertion is name-exact, fresh-root-guarded, and exits non-zero — deliberately, on all three counts.** A recursive PDF count under `$PLOTROOT` proves nothing: six leftovers from an abandoned attempt satisfy it while the current run wrote nothing, which is exactly the failure the check exists to catch. So the block refuses to start on a non-empty root and then tests the six exact paths by name. Those names are fixed and predictable: `save_and_close_figure()` writes `os.path.join(output_dir, f"{filename}{output_format}")` (`plot/mimic-plot/output_utils.py:248`), `--format` is passed through verbatim with no dot inserted (`mimic-plot.py:1012`, `:1387`), and `--output-dir` becomes the plots directory itself with no snapshot subdirectory added (`:1216`) — so `--format=.pdf` gives `$PLOTROOT/snap<N>/HaloMassFunction.pdf` and `.../StellarMassFunction.pdf` and nothing else. The final `exit 1` matters because this block is the guard on the overwrite hazard above: `test … && echo … || echo …` returns success on both branches, so a check written that way announces the failure and still reports OK to anything reading its status.

**`--snapshot-plots`, because omitting it doubles the read on the largest snapshot.** With neither tier flag given the parser enables **both** tiers (`mimic-plot.py:1044-1046`). Neither requested figure is an evolution plot, but `generate_evolution_plots()` does not learn that until after it has read the data: with `--snapshot` set it selects that same single snapshot (`:1422-1427`), calls `read_data()` on it (`:1474`), and only then filters against `--plots` and finds nothing to draw (`:1505`). On a production snapshot that is a large read performed twice for no output. `--snapshot-plots` (`:1005-1007`) confines the run to the snapshot tier and skips the second read entirely (`:1582-1591`).

**Profile selection is by fallback, and the fallback is the right one here.** `configure_plot_profile()` stacks `plot/mimic-plot/profiles/default.yaml` → the model default → a simulation-level `plot_profile.yaml` → a `<simulation>_plot_profile.yaml` under the model → any `plotting.profile` in the run file (`mimic-plot.py:309-339`). For this pair only the first two exist: `simulations/shin-uchuu/` has no `plot_profile.yaml`, `models/sage16/plots/profiles/` holds only `default.yaml`, `mini-millennium_plot_profile.yaml` and `millennium_plot_profile.yaml`, and `sage16_shin-uchuu.yaml` declares no `plotting:` section. The effective mode is therefore **`exploration`** (`models/sage16/plots/profiles/default.yaml:27`) — auto-scaled axes — which is what a box whose limits have never been plotted needs. The `validation` mode the mini-millennium profile sets pins axis limits to that box and would be wrong here.

**The shipped `output.snapshot_list` cannot produce z = 1 or z = 2, and this must be fixed before P6, not at P8.** `models/sage16/input/sage16_shin-uchuu.yaml:18` requests `[69, 40, 20, 10, 5, 2, 1, 0]`. Against `simulations/shin-uchuu/shin-uchuu.a_list` (70 entries, snapshot *n* = line *n+1*):

| Target | Nearest snapshot | `a` | `z` |
|---|---|---|---|
| z = 0 | **69** — already requested | 0.99998 | 0.00002 |
| z = 1 | **52** — *not requested* | 0.49218 | 1.032 |
| z = 2 | **43** — *not requested* | 0.33027 | 2.028 |

The shipped list's nearest neighbours to z = 1 are snapshot 69 (z = 0) and snapshot 40 (`a` = 0.28918, z = 2.458) — nothing within Δz ≈ 0.4 of z = 1, and snapshot 40 is 23% off in z from the z = 2 target. **Add 52 and 43 to `output.snapshot_list` before launching P6.** Output snapshots are frozen at launch, the run rejects `--skip` (C10), and there is no way to add a snapshot afterwards short of re-running the multi-day job.

**Change all four `shin-uchuu` run files together.** That run file's own comment records why: the cross-format identity gate compares the run files snapshot for snapshot and requires every compared snapshot to be non-empty on both sides, so the four must carry an identical `snapshot_list`. Confirm 52 and 43 are non-empty on the converted side from the conversion report's per-snapshot counts before relying on them — the same counts P3b already reads.

> ### ⚠️ Operator decision required before P6 — two open calls, not decided in this document
>
> Both are science-configuration choices, both belong to whoever runs the conversion, and both must be settled **before launch**: output snapshots are frozen at launch and the run is not resumable (C10), so neither can be revisited at P8. Adding 52 and 43 is recorded above as the documented pre-P6 action; it is a default to confirm or overrule, not a decision already taken. **No run file is amended by this document.**
>
> **1 — Which snapshots to add for z = 1 and z = 2.** Two defensible pairs, from `simulations/shin-uchuu/shin-uchuu.a_list` (snapshot *n* = line *n+1*):
>
> | Option | z = 1 | z = 2 | Rationale |
> |---|---|---|---|
> | **Nearest in redshift** (the one recorded above) | **52** — `a` = 0.49218, z = 1.032 | **43** — `a` = 0.33027, z = 2.028 | Minimises \|Δz\| — 0.032 and 0.028 |
> | **Bracketing from below** | **53** — `a` = 0.51449, z = 0.944 | **44** — `a` = 0.34529, z = 1.896 | Keeps the plotted redshift *below* the nominal target — later in cosmic time, never earlier — at a larger \|Δz\|, 0.056 and 0.104 |
>
> **2 — Whether adding them disturbs the cross-format identity gate.** All four run files must carry an identical `snapshot_list`, and the gate requires every compared snapshot to be non-empty **on both sides** — but the two sides will not be the same dataset: the HDF5 package moves to production at P3b while the **ASCII package deliberately stays on the rehearsal subset** (P3b step 5). The run file's comment asserts non-emptiness for the snapshots listed **today**; it says nothing about 52/43 or 53/44. So whichever pair is chosen, confirm it is non-empty against **both** the production conversion report's per-snapshot counts **and** the retained subset, before the pair goes into any run file.
>
> Record the decision and its evidence here when it is made, the way the decisions above are recorded.

### The constraints these steps run under, with their evidence

These are the facts a session executing the sequence must not rediscover. Original constraint IDs are preserved so this table lines up with the joint review and the archived record.

| # | Constraint | Evidence |
|---|---|---|
| **C4** | **File IDs must be contiguous from 0, *and* the file count must be a perfect cube.** `read_locations()` asserts `max_fileid + 1 == numfiles`, then that `round(cbrt(numfiles))³ == numfiles`. **2,744 = 14³**, which is why any subset must close file coverage rather than drop a file | `src/io/tree/ctrees/ctrees_utils.c:236-246` |
| **C5** | **A forest's tree-driver reader memory has a floor set by its *tree count*, independent of halo count.** `load_unit_ctrees_ascii()` preallocates `1000 × ntrees` records in both `halo_data` and `additional_info` — 152,000 B per tree. The percolation super-forest's 104,845,278 tree roots alone demand ≈15.9 TB | `src/io/tree/read_ctrees_ascii.c:647-655` |
| **C6** | **The tree driver loads a forest as one in-memory unit**, so the super-forest can never be read tree-ordered. This is why the snapshot driver exists, why the rehearsal subset excluded that forest, and why a production-scale topology cross-check is impossible in principle rather than merely expensive | C5 quantifies it |
| **C9** | **`FirstProgenitor` flows forward through a pending buffer, so snapshot *completeness* matters.** `build_progenitor_links` emits snapshot N's pending buffer for N+1 and `link_one_snapshot` writes it there; `_validate_monotonic_pairs` iterates only over the **observed** pairs, so it enforces strict ascending scale-per-snapshot but **cannot see a snapshot that produced no pair at all**. A missing adjacent snapshot is silently treated as a lineage boundary — which is what makes P2's snapshot-span gate binding | `scripts/convert/links.py:318-367`, `:599-622`, `:1086-1131` |
| **C10** | **Snapshot-ordered runs are HDF5-only, serial-only, and cannot use `--skip`.** All three are config-time rejections | `src/core/read_parameter_file.c:1475-1488` |
| **C11** | **One MODEL + one SIMULATION per build** — the selection is compiled in. Every cross-format comparison needs a rebuild between runs | `Makefile:47-78`, `:155-157` |
| **C12** | **The `Spin` and `deltaMvir` range checks are validation-tier, not runtime FATALs.** A wrong bound fails a **test on a finished run**; it cannot abort the production run, and nothing in the run will say a value left its declared range. This is why P4 is a pre-run scan and P7 a post-run one — the scans, not the runtime, are the enforcement | Ranges declared at `simulations/shin-uchuu/halo_properties.yaml:113`, `simulations/shin-uchuu-ascii/halo_properties.yaml:108` and `src/core/core_properties.yaml:143`; the bounds reach the generated `tests/generated/property_ranges.json` and are **enforced by exactly one check**, `test_physical_ranges()` in the **scientific tier** (`tests/scientific/test_scientific.py:399`, comparing at `:437-500` — the scalar `below`/`above` sums at `:450-451` and the per-component ones at `:480-481` — manifest path at `:61`). **No runtime consumer exists** — `grep -rn property_ranges src/` returns nothing |
| **C15** | **The declared `unique_galaxy_id_multiplier: 10000000000` is INVALID at production scale.** `mimic_unique_galaxy_id_components_valid()` requires `halonr < multiplier`, and production `max_halo_rank_in_forest` ≈ **1.2834657129 × 10¹⁰** (the super-forest's whole-run halo count minus one). Feasible window **12,834,657,130 ≤ M ≤ 55,379,738,354**; **use 2 × 10¹⁰**. 10¹¹ overflows the forest term | `src/include/galaxy_id.h:24-45`; declared values at `simulations/shin-uchuu/simulation_info.yaml:36` and `simulations/shin-uchuu-ascii/simulation_info.yaml:33` |

**P2's snapshot-span gate, stated in full.** Assert from the conversion report's own per-snapshot counts that **every a_list snapshot has a non-zero halo count**. C9 is why: a gapped manifest produces wrong `FirstProgenitor` links with no error raised. Counting output files proves nothing — the writer emits one file per a_list snapshot **including empty ones** (`scripts/convert/hdf5_writer.py:10-11`, `:394`), so "70 files were produced" is satisfied by a dataset with a hole in it.

**P2's storage envelope, and its three preconditions.** The projected production peak is **6.89 TB against a 7.0 TB policy ceiling** (0.48 TB under the 7.37 TB volume — LaCie's free space when the envelope was derived; it measures 7.71 TB on 2026-08-29, so the headroom against the volume is now 0.82 TB, see "Local storage"), binding at scatter through the staged source batch. It holds only if: **(a) consumptive deletion is enabled** — with the flag off the projected peak is ≈8.65 TB, which does not fit the volume at all; **(b) the staged source batch is ≤ 4.4 TB and each released batch is deleted before `sort` begins** — hence at least three batches; **(c) no cross-check artifact is in the envelope**, which C5/C6 make automatic since a production-scale cross-check cannot be run. Per-stage table: `scripts/convert/README.md` → "Storage envelope and the production memory term".

**The multiplier default is 10⁹ in three places and must be overridden explicitly.** `validate.py`, `crosscheck.py compare` and `convert_ctrees.py report` all default `--multiplier` to 10⁹ (`scripts/convert/validate.py:70`, `scripts/convert/crosscheck.py:1657-1662`, `scripts/convert/convert_ctrees.py:203-208` and `:277`), while the packages declare 10¹⁰ today and must be at 2 × 10¹⁰ at production (C15, P3). **Pass `--multiplier` explicitly to all three, as an integer** — the arguments are `type=int`, so `--multiplier 1e9` is rejected by `argparse`.

**Why C12's two range checks split across P4 and P7.** `deltaMvir` cannot be a pre-run scan at all: it is created during inheritance (`src/core/inheritance.c:56`, `:82`) and exists only in Mimic's output, never in the input catalog. The conversion report carries totals, per-snapshot counts and identity bounds but **no value extrema** (`build_report`, `scripts/convert/report.py:94-143`), so P4 is a separate `h5py` scan rather than a report field.

**Long-running steps exceed any 10-minute command cap.** P1, P2's phases and P6 all run for hours. Run them detached — background, `nohup` or `tmux` with a sentinel file — and poll. A foreground invocation killed mid-write leaves a partially written intermediate the manifest will then refuse to resume from.

---

## Definition of Done

1. 70 HDF5 snapshot files produced and validated (halo count, adjacency, round-trip progenitor check, FoF chain integrity, NextProgenitor same-file scope, identity uniqueness, Len non-negative)
2. **Topology cross-check passes on micro-Uchuu** by stable halo identity (the converter acceptance gate, completed long before the production run)
3. The conversion report exists and the shin-uchuu identity multiplier is set from its measured counts
4. `simulations/shin-uchuu/` package registered and building clean with the snapshot reader
5. Mimic runs `sage16` on Shin-Uchuu end-to-end: no assertion failures, no broken links, no memory errors
6. HMF and GSMF plots produced and sanity-checked at z=0, z=1, z=2 — **six figures, two per snapshot, each snapshot written to its own `--output-dir`** (P8: the figure filenames carry no snapshot number, so a shared directory silently leaves only the last snapshot's pair)
