# Shin-Uchuu ctrees ASCII → Snapshot HDF5 Conversion Plan

**Status:** Active — all previously open design decisions resolved (joint plan review 2026-07-02, decisions D1–D12; review record archived at `archive/dev-plans/dual-driver-plan-review.md`). Earlier revision reviewed twice by Codex gpt-5.5 (2026-06-27).
**Date:** 2026-07-02
**Context:** This plan is one sequence with `MIMIC-DUAL-DRIVER-PLAN.md`. The converter is **not** blocked on the snapshot driver: it is blocked only on the frozen format contract, and it is built and validated first, against micro-Uchuu ASCII, using the existing tree-ordered `read_ctrees_ascii.c` reader as the reference — zero new Mimic code required. The full 5.6 TB Shin-Uchuu conversion runs exactly once, after the dual-driver Phase 5 identity gate is green. Mimic itself performs no internal conversion.

---

## Problem Statement

The Shin-Uchuu merger trees exist at `/fred/oz004/simulations/uchuu_suite/shinuchuu/mergertrees` in Consistent-Trees ASCII format. Two structural problems prevent running Mimic's existing `consistent_trees_ascii` reader on this data:

1. **Percolation super-forest**: forest `26551468179` contains 104,845,278 tree roots — 33% of all trees — almost certainly a ctrees linking artifact. The tree driver must load a forest as a single in-memory unit; it cannot. This also rules out the uchuutools forests-HDF5 packaging.
2. **Index memory wall**: the ASCII reader loads a global `forests.list` index (315M entries) into every MPI rank at startup, costing ~18 GB per rank before a single halo is processed.

Both problems are structural consequences of forest-ordered processing and both disappear in the snapshot driver, which processes one snapshot's halo population at a time.

---

## Source Data Summary

| Parameter | Value |
|---|---|
| Format | Consistent-Trees ASCII |
| Total size | 5.6 TB across 2,747 `.dat` files |
| Largest file | 88 GB (`tree_8_12_10.dat`) |
| `locations.dat` | 13 GB (symlink to `locations_no_extra_columns.dat`) |
| `forests.list` | 17 GB, 315,004,242 lines |
| Total halos (z=0) | 315,004,242 |
| Total forests | 166,547,771 |
| Estimated total halos (all snapshots) | ~15–18 billion (file-size estimate: 5.6 TB / ~350 bytes/line) |
| Snapshots | 70 (a = 0.04773 to 0.99998) |
| Box size | 140 Mpc/h |
| Particle mass | ~8.97 × 10⁴ Msun/h (inferred; **confirm from simulation docs before freezing the package — `Len` derives from it**) |
| Cosmology | Planck 2015 (Ωm=0.3089, h=0.6774) — identical to rest of Uchuu suite |

---

## Target Format: Snapshot-Ordered HDF5

One HDF5 file per snapshot, named `snapshot_000.h5` through `snapshot_069.h5` (per-snapshot files are decided, not open: partial recovery, per-snapshot parallelism, and the driver's access pattern all favour them). All topology links are snapshot-local integer indices (no global IDs). Scalar metadata lives in HDF5 **attributes** on the `/header` group. **Field names and types on disk must match what `simulations/shin-uchuu/halo_properties.yaml` declares**, so the generated `RawHalo`/accessors consume the file directly; the names below already match the existing `micro-uchuu-ascii` bridge contract (`M_Crit200`→`HaloMass`, `Len`, `SnapNum`, `MostBoundID`, spin conventions). The contract is **frozen** at [`docs/SNAPSHOT-HDF5-FORMAT.md`](../SNAPSHOT-HDF5-FORMAT.md) (`format_version = 1`, 2026-07-18), which is now authoritative; this section remains as the working draft it was promoted from — if they ever disagree, the spec wins.

```
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
                                      (the reader may later synthesize this from the header
                                      instead of reading ~70 GB of redundant column; decide
                                      during reader design — the column stays in the contract
                                      until then)
    M_Crit200             float32[N]  native Msun/h (generated accessor converts to 1e10 Msun/h)
    Pos                   float32[N,3] Mpc/h comoving
    Vel                   float32[N,3] km/s peculiar
    Spin                  float32[N,3] dimensionless J/Mvir (applied during conversion)
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

## Getting the Data to the Mac

Source is on the HPC filesystem; conversion runs on the Mac Studio (M3 Ultra, 512 GB unified memory, 8 TB internal SSD). Landing all 5.6 TB of ASCII locally alongside scratch and output breaks the disk budget (5.6 + 1.9 + 1.1 > 8 TB), so the transfer is batched and consumptive:

1. Fetch `.dat` files in batches with `rsync --checksum` (the 88 GB largest file is a single-batch item).
2. Phase 1 scatters each fetched file, then **deletes the local ASCII copy**.
3. A resume manifest records per-file completion: name, size, halo count, checksum. Re-running skips completed files; a crashed batch re-fetches cleanly.

Peak local disk stays ≈ scratch (1.9 TB) + output (1.1 TB) + in-flight batch (~100–200 GB). At ssh-realistic 50–100 MB/s the transfer is ~16–31 h, overlappable with parsing; total wall clock comfortably 1–3 days, which is acceptable — the design optimises for restartability and few failure points, not speed.

Fallback if ssh throughput disappoints badly: run Phase 1 scatter HPC-side and transfer the ~1.9 TB scratch instead (3× less data, at the cost of a second execution environment). Not the default.

---

## Algorithm

The conversion is an external sort over the snapshot dimension: forest-ordered ASCII → snapshot-ordered HDF5. Pipeline: **scatter → sort/index → remap → write**. (Directly streaming trees into final snapshot files cannot work alone: every link field is a snapshot-local index that does not exist until the destination slab's order is fixed, and FoF/flyby fixes need the whole forest-at-snapshot population visible. Scatter-then-finalize is the robust realization of the streaming idea.)

### Phase 0: Provenance pre-pass

Stream `forests.list` once to build the tree-root-id → forest-id map (315M × 16 B as sorted arrays ≈ 5 GB, in-memory), and assign each forest its dense run-scoped `ForestIndex`. **The enumeration rule is ascending ctrees forest id** — verified against the reader: the ASCII reader sorts tree locations by `(forestid, fileid, offset)` and groups forests from that sorted order (`ctrees_utils.c:270-297`, consumed in `read_ctrees_ascii.c`), so the run-scoped dense forest order that defines `forestnr_global` is ascending forest id. The micro-Uchuu cross-check re-confirms this end to end. Write the dense-index → ctrees-forest-id table to the `forests.h5` sidecar. This pre-pass is required: identity fields cannot be produced without it.

### Phase 1: Scatter (ASCII → per-snapshot binary)

Stream-read all 2,747 ctrees ASCII files using a bounded worker pool. `locations.dat` is never needed.

**Parsing ctrees ASCII correctly**: ctrees files contain a `#fields: ...` header line and `#tree <id>` block markers. Do not use `comment='#'` with pandas default `header='infer'`. Correct approach:
1. Read the first non-empty line beginning with `#fields:` to get column names; strip the prefix and split on whitespace.
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
| **Total** | | **~116 bytes** | |

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

```
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

**Rank pass (between Phase 2 and Phase 3, or fused into Phase 3 bookkeeping):** ranks are per-forest over all snapshots, so they need a forest-major view once. For ordinary forests this is cheap grouping. For the super-forest (~5–9 billion halos), it is one large deterministic sort of ~(scale, upid, pid, id, slab-slot) keys — ~150–250 GB of key data: in-RAM on this machine or a chunked external merge sort on scratch. It runs once, its output is the `HaloRankInForest` column plus the run-scoped `max_halo_rank_in_forest`/`n_forests_total` header values, and it is the direct input to setting the shin-uchuu identity multiplier (see below).

**Memory at peak** (snap 68 processing, which loads snap 68 + snap 69 index):
- snap_68 sorted data: ~33 GB
- snap_69.idx: 315M × 8 bytes = 2.5 GB
- snap_68 working arrays (sort keys, resolved upid, groupby, progenitor inversion): ~2× data ≈ 66 GB
- snap_69 FirstProgenitor pending buffer: 315M × 4 bytes = 1.3 GB
- HDF5 write buffer: ~2 GB
- **Realistic peak: ~140–170 GB** — comfortable within 512 GB. The super-forest rank sort peaks separately at ~150–250 GB, also within budget.

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

## Feasibility: Back-of-Envelope Calculations

**Hardware:** Mac Studio M3 Ultra, 512 GB unified memory, ~8 TB internal NVMe SSD.

### Memory

| Phase | Realistic working set | Notes |
|---|---|---|
| Phase 0 pre-pass | ~5–10 GB | forests.list as sorted arrays |
| Phase 1 scatter | ~15–25 GB | Bounded pool × per-worker parse state + write buffers |
| Phase 1 concat | ~5 GB | Sequential cat; I/O-bound |
| Phase 2 sort | ~74 GB (peak, snap 69) | Data + sort temporaries |
| Rank pass (super-forest) | ~150–250 GB | One-time key sort; chunked external sort as fallback |
| Phase 3 remap | ~140–170 GB (peak, snap 68+69) | Two snapshots + working arrays + pending buffer |
| Phase 4 validate | ~40 GB | One snapshot at a time |
| **Peak** | **~250 GB** | **≥2× margin against 512 GB** |

### Disk

| Artifact | Size |
|---|---|
| Source ASCII (in-flight batches only; deleted after scatter) | ~100–200 GB at a time |
| Phase 1 scratch (post-concat; replaced by sorted scratch per snapshot in Phase 2) | ~1.9–2.2 TB |
| Phase 1 worker files before concat | up to ~1× scratch = ~2.2 TB peak (transient) |
| Phase 2 sorted scratch (replaces unsorted; +1 snapshot transient during each swap) | ~1.9–2.2 TB |
| Phase 2 index files (70 snapshots) | ~120–145 GB |
| Output HDF5 (~100 B/halo × 15–18 B halos, uncompressed, incl. identity columns) | ~1.5–1.8 TB |
| **Peak simultaneous working storage** | **~5–6 TB** — fits the internal 8 TB SSD only because every stage is consumptive (delete-after-verify); keeping any stage's input alongside its output does not fit |

### Time

| Phase | Bottleneck | Estimate |
|---|---|---|
| Transfer (batched rsync, overlapped with Phase 1) | ssh throughput 50–100 MB/s | 16–31 h |
| Phase 1 ASCII parse | Tokenisation; ~500 MB/s pandas, ~2 GB/s C | 1–8 h compute |
| Phase 1 concat | SSD ~5 GB/s | 20–40 min |
| Phase 2 sort + index (parallel) | I/O ~4 TB at ~5 GB/s | 30–45 min |
| Rank pass | Super-forest sort | 1–4 h |
| Phase 3 remap + write | Merge-join + FoF + inversion + I/O | 2–4 h |
| Phase 4 validate | Sequential reads | 1–2 h |
| **Total wall clock** | Transfer-dominated | **~1–3 days** |

Acceptable by design: the pipeline optimises for restartability and few failure points, not speed.

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

A new `simulations/shin-uchuu/` package:
- `simulation_info.yaml`: 140 Mpc/h box, confirmed particle mass, `tree_type: snapshot_hdf5`, `processing_order: snapshot_ordered` in run files, 70-snapshot list, and the identity multiplier from the conversion report (D9)
- `halo_properties.yaml`: ctrees bridge contract mirroring `micro-uchuu-ascii` (`M_Crit200` → `HaloMass`, `Len`, `SnapNum`, `Pos` range `[0.0, 140.0]`); `ForestIndex` and `HaloRankInForest` as identity fields
- `shin-uchuu.a_list`: from Phase 1
- `snapshots/`: symlink to the 70 HDF5 files

Property ranges requiring calibration from a test run: `deltaMvir`, `Len` (floor is 1 at this resolution), `Spin`.

---

## Risks and Mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| fix_flybys / fix_upid divergence from reference | Critical | Micro-Uchuu topology cross-check by stable halo identity gates the converter before any Mimic code |
| Non-adjacent `desc_scale` links in Shin-Uchuu | High | Abort — corrupt data by definition; no repair path exists or is wanted |
| Particle mass inferred, not documented | High | Confirm from simulation metadata **before** freezing the package; wrong value corrupts every Len |
| Super-forest rank sort resource surprise | Medium | Measured key volume ~150–250 GB fits RAM; chunked external merge sort is the fallback; one-time cost |
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
3. Dual-driver Phase 4b: snapshot reader against the micro-Uchuu fixtures
4. Dual-driver Phase 5: snapshot driver + cross-format identity gate on micro-Uchuu
5. **Then** the one-time 5.6 TB Shin-Uchuu production conversion (this plan at full scale)
6. `simulations/shin-uchuu/` package; sage16 end to end; HMF/GSMF sanity at z = 0, 1, 2

Shin-Uchuu is the primary scientific motivation for the snapshot pathway — and the only way Mimic can process it at all, because of the super-forest.

---

## Definition of Done

1. 70 HDF5 snapshot files produced and validated (halo count, adjacency, round-trip progenitor check, FoF chain integrity, NextProgenitor same-file scope, identity uniqueness, Len non-negative)
2. **Topology cross-check passes on micro-Uchuu** by stable halo identity (the converter acceptance gate, completed long before the production run)
3. The conversion report exists and the shin-uchuu identity multiplier is set from its measured counts
4. `simulations/shin-uchuu/` package registered and building clean with the snapshot reader
5. Mimic runs `sage16` on Shin-Uchuu end-to-end: no assertion failures, no broken links, no memory errors
6. HMF and GSMF plots produced and sanity-checked at z=0, z=1, z=2
