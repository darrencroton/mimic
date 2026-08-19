# Shin-Uchuu ctrees ASCII → Snapshot HDF5 Conversion Plan

**Status:** Converter built and micro-Uchuu-validated 2026-07-24 — the external converter described here exists under `scripts/convert/` and passed its micro-Uchuu acceptance gate (full pipeline over the real 22,580,924-halo / 50-snapshot ASCII data; producer validation battery + a seven-check cross-check against a `halos-only` reference run, topology-order proof fully discharged, zero unexplained mismatches). That gate was re-run end to end on 2026-08-03 on a fully regenerated micro-Uchuu dataset, now placed at `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-snapshot/` for snapshot-reader development: same three totals, producer battery 15/15, cross-check green including `topology-chains`. Remaining: the one-time Shin-Uchuu production conversion. **Its precondition is met — the dual-driver Phase 5 identity gate passed 2026-08-12** (both models, both timestep schemes, per-ID bitwise), so the production conversion is unblocked; see `POST-PHASE-5-WORK.md` §2 for the items to close first. All previously open design decisions resolved (joint plan review 2026-07-02, decisions D1–D12; review record archived at `archive/dev-plans/dual-driver-plan-review.md`). Earlier revision reviewed twice by Codex gpt-5.5 (2026-06-27).
**Date:** 2026-07-02
**Context:** This plan is one sequence with `MIMIC-DUAL-DRIVER-PLAN.md`. The converter is **not** blocked on the snapshot driver: it is blocked only on the frozen format contract, and it is built and validated first, against micro-Uchuu ASCII, using the existing tree-ordered `read_ctrees_ascii.c` reader as the reference — zero new Mimic code required. The full 5.6 TB Shin-Uchuu conversion runs exactly once, after the dual-driver Phase 5 identity gate is green (**green 2026-08-12**). Mimic itself performs no internal conversion.

---

## Problem Statement

The Shin-Uchuu merger trees are in Consistent-Trees ASCII format. **Operative location, verified 2026-08-20: `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` on OzSTAR** (`ssh dcroton@nt.swin.edu.au`) — 5.6 TB across 2,744 `tree_*.dat` files plus `forests.list` and `locations.dat`, alongside the producer's own `shinuchuu.par`. This document previously recorded `/fred/oz004/simulations/uchuu_suite/shinuchuu/mergertrees`; that path was never verified in the 2026-08-20 sweep and should be treated as historical unless an operator confirms it is a live mirror. Use the oz214 path. Two structural problems prevent running Mimic's existing `consistent_trees_ascii` reader on this data:

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
| Particle mass | **8.97 × 10⁵ Msun/h** — confirmed 2026-08-14 from Ishiyama et al. 2021, the Uchuu suite paper ([arXiv:2007.14720](https://arxiv.org/abs/2007.14720)): "262 billion (6400³) particles in a box of side-length 140 Mpc/h, with particle mass 8.97 × 10⁵ M☉/h". **Corrects the value previously recorded here as ~8.97 × 10⁴, which was low by exactly a factor of 10** and had propagated into two derived claims elsewhere. Independently cross-checked against this table's own 140 Mpc/h box and the package cosmology (Ω_m = 0.3089): Ω_m ρ_crit L³ / N = 0.3089 × 2.77537 × 10¹¹ × 140³ / 6400³ = 8.97 × 10⁵ Msun/h. `Len` derives from this, so it is now fixed rather than inferred |
| Cosmology | Planck 2015 (Ωm=0.3089, h=0.6774) — identical to rest of Uchuu suite |

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

**Rank pass (between Phase 2 and Phase 3, or fused into Phase 3 bookkeeping):** ranks are per-forest over all snapshots, so they need a forest-major view once. For ordinary forests this is cheap grouping. For the super-forest (~5–9 billion halos), it is one large deterministic sort of ~(scale, upid, pid, id, slab-slot) keys — ~150–250 GB of key data: in-RAM on this machine or a chunked external merge sort on scratch. **Design figure, not implemented:** the shipped `compute_identity()` (`scripts/convert/links.py`) instead concatenates and lexsorts the key columns over *all* snapshots globally (~600–720 GB at production scale) — see the risk table and the "Pre-conversion obligation" subsection. It runs once, its output is the `HaloRankInForest` column plus the run-scoped `max_halo_rank_in_forest`/`n_forests_total` header values, and it is the direct input to setting the shin-uchuu identity multiplier (see below).

**Memory at peak** (snap 68 processing, which loads snap 68 + snap 69 index):
- snap_68 sorted data: ~33 GB
- snap_69.idx: 315M × 8 bytes = 2.5 GB
- snap_68 working arrays (sort keys, resolved upid, groupby, progenitor inversion): ~2× data ≈ 66 GB
- snap_69 FirstProgenitor pending buffer: 315M × 4 bytes = 1.3 GB
- HDF5 write buffer: ~2 GB
- **Realistic peak: ~140–170 GB** — comfortable within 512 GB. The super-forest rank sort peaks separately at ~150–250 GB *as designed*, also within budget — but the shipped rank pass does not implement that design (it sorts all snapshots globally, ~600–720 GB; see the risk table and the "Pre-conversion obligation" subsection).

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
| Rank pass (super-forest) | ~150–250 GB (design; shipped code sorts all snapshots globally, ~600–720 GB — see "Pre-conversion obligation") | One-time key sort; chunked external sort as fallback |
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
- `halo_properties.yaml`: ctrees bridge contract mirroring `micro-uchuu-ascii` (`M_Crit200` → `HaloMass`, `Len`, `SnapNum`, `Pos` range `[0.0, 140.0]`). **Do not declare `ForestIndex` or `HaloRankInForest`** — corrected 2026-08-12. They are snapshot-format identity metadata rather than catalog halo properties, and are exempt from the declaration rule (`SNAPSHOT-HDF5-FORMAT.md` errata 2026-08-11): the reader consumes both directly by dataset name into `struct SnapshotSlab`'s own `forest_index`/`halo_rank_in_forest` arrays. Declaring them is unnecessary rather than forbidden, but the working exemplar `simulations/micro-uchuu-snapshot/halo_properties.yaml` omits both, and mirroring it is the safe course
- `shin-uchuu.a_list`: from Phase 1
- `snapshots/`: symlink to the 70 HDF5 files

**Run constraints for snapshot-ordered runs** (added 2026-08-12; enforced at config time in `src/core/read_parameter_file.c:1452-1466`, so a production run that violates one aborts at startup rather than part way through):

- **HDF5 output only** — `output_format: binary` is rejected.
- **Serial only** — `NTask > 1` is rejected; there is no MPI path (see `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`).
- **No `--skip`** — a partially completed run cannot be resumed by re-running with `--skip`; plan for a single uninterrupted run, or for restarting it.
- Output is written as **one HDF5 partition file per requested output snapshot** (named by that snapshot's number, `model_<snapnum>.hdf5`) **plus a master**, carries no `Ntrees` and no `TreeHalosPerSnap`, and uses int64 `TotHalosPerSnap`. `hdf5_format_version` is `1.2`.

Property ranges requiring calibration from a test run: `deltaMvir`, `Len` (floor is 1 at this resolution), `Spin`. Note `deltaMvir` is a **core-level output property** (`src/core/core_properties.yaml`, range `[-20000.0, 20000.0]`, already annotated for Uchuu-scale mass swings), not an entry in `simulations/shin-uchuu/halo_properties.yaml` like `Len`/`Spin` — its calibration is checked and edited in `core_properties.yaml`.

---

## Risks and Mitigations

| Risk | Severity | Mitigation |
|---|---|---|
| fix_flybys / fix_upid divergence from reference | Critical | Micro-Uchuu topology cross-check by stable halo identity gates the converter before any Mimic code |
| Non-adjacent `desc_scale` links in Shin-Uchuu | High | Abort — corrupt data by definition; no repair path exists or is wanted |
| ~~Particle mass inferred, not documented~~ — **RETIRED 2026-08-14** | ~~High~~ | Confirmed as **8.97 × 10⁵ Msun/h** from Ishiyama et al. 2021 ([arXiv:2007.14720](https://arxiv.org/abs/2007.14720)) and cross-checked against the 140 Mpc/h box, 6400³ particles and Ω_m = 0.3089. The risk was real: the recorded value was low by exactly 10×, and `Len = round(Mvir_native × 1e-10 / PartMass)` would have been inflated 10× for every halo. Use the confirmed value when freezing `simulation_info.yaml` |
| Super-forest rank sort resource surprise | **Blocker (until the converter scale pass lands)** | The "measured key volume ~150–250 GB fits RAM" figure below is inconsistent with the implementation as written: `compute_identity()` (`scripts/convert/links.py`) concatenates five int64 columns over *all* snapshots (~600–720 GB at 15–18B halos) plus the lexsort's order array (~120–144 GB), and the function's own docstring defers the external-merge sort as a "production concern" rather than implementing it. Mitigation: the scheduled converter scale-engineering pass (joint review D4) — see the "Pre-conversion obligation" subsection below — re-derives the actual key volume and implements the external-merge rank sort before this row can be downgraded. |
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
5. **Then** the one-time 5.6 TB Shin-Uchuu production conversion (this plan at full scale)
6. `simulations/shin-uchuu/` package; sage16 end to end; HMF/GSMF sanity at z = 0, 1, 2

Shin-Uchuu is the primary scientific motivation for the snapshot pathway — and the only way Mimic can process it at all, because of the super-forest.

### Delegated obligation: the snapshot-driver memory fallback (owned here, decided elsewhere)

`MIMIC-DUAL-DRIVER-PLAN.md` Phase 5 item 6 decides that the snapshot driver keeps **two complete raw slabs** unconditionally and builds no alternative, because the micro-Uchuu gate is nowhere near any ceiling. It then explicitly assigns the production-scale fallback to *this* plan — "This trigger belongs to `SHIN-UCHUU-CONVERSION-PLAN.md`, not to Phase 5" — on the grounds that its inputs are design outputs Phase 5 has not produced yet. Recorded here so it is not rediscovered under time pressure at the production step (added 2026-08-10; it was decided 2026-08-04 but never written into this plan).

**Before the production run**, recompute the adjacent-snapshot peak from *actual* allocation capacities, not estimates: both retained raw slabs, both processed generations, the galaxy pools, the output and HDF5 buffers, and allocator growth. Include the reader's transient staging buffers — `snapshot_h5_fill_halos()` allocates one scalar and one vector buffer at the widest native element size, 32 bytes per halo in total (≈10 GB at the projected 315M-halo z = 0 slab), live only during a slab load but concurrent with the slab it is filling.

**If the peak exceeds 85% of installed RAM** (≈435 GB on a 512 GB machine), replace the retained *previous* raw slab with a compact projection of `{int32_t Len, int32_t NextProgenitor}` — all the previous generation is ever read for — costing ≈315e6 × 8 B ≈ 2.5 GB against a full second slab. Struct sizes for this catalog, re-measured 2026-08-13: `struct RawHalo` **88 B**, `struct Halo` 176 B, `struct GalaxyData` 176 B, `struct HaloOutput` 264 B. (The 2026-08-04 audit recorded 104 B for `RawHalo`; that was the default pair's, superseded below.)

**Recompute 2026-08-13 — clear for the production configuration under measured ratios, but not yet closed.** Struct sizes were measured (not estimated) against the generated headers for `MODEL=sage16 SIMULATION=micro-uchuu-snapshot`: `RawHalo` 88 B, `Halo` 176 B, `GalaxyData` 176 B, `HaloOutput` 264 B. Counting both live generations at the projected 315,004,242-halo z=0 slab — raw slabs, identity and aux arrays, the fully-resident processed output buffers, both galaxy pools — plus the reader's staging transients and an allowance for write buffers and allocator overhead, the **projected peak is ≈317 GB against the ≈435 GB trigger** for `sage16`, whose measured output population is 0.99× the slab on micro-Uchuu. **The projection is parametric, not fixed**: per generation it is `120·N + 176·C + 176·G`, where `C` is the processed buffer's realised capacity (it grows ×1.5 past its seed) and `G` the galaxy pool's allocation high-water (which the output count does not bound, since Type 3 galaxies are allocated but never emitted). At `C = G = 1.5N` the peak reaches ≈428 GB, and in a `halos-only`-like regime (2.11× measured at z=0) ≈579 GB — over installed RAM. **`G` is unmeasured**, so this obligation stays open: instrument `C` and `G` at the subset rehearsal and re-derive before the production run. The full derivation, the micro-Uchuu measurements, and the sensitivity table are in `POST-PHASE-5-WORK.md` §2.2. Note the joint review's F-5 sized the output buffer at 264 B/record; that is `struct HaloOutput`, while the buffer holds `struct Halo` at 176 B — the recompute corrects it. Re-measure `sizeof(struct RawHalo)` once `simulations/shin-uchuu/` exists.

**Correction 2026-08-12.** That 104 B is the **default pair's** `RawHalo` (`sage16`/`mini-millennium`), not this catalog's. The ctrees-bridge catalog the snapshot packages use measures **88 B** (verified against `micro-uchuu-snapshot`), so a full second slab is ≈315e6 × 88 B ≈ **27.7 GB**, not ≈32.8 GB. Re-derive the figure from the `shin-uchuu` package's own `sizeof(struct RawHalo)` rather than from either number quoted here. Note also that neither the two processed generations nor the two galaxy pools are quantified anywhere in this plan — the numeric memory tables above are converter-side only — so they must be measured, not assumed, during the recompute. Phase 5 shipped **no** memory-projection branch: the driver holds two complete raw slabs unconditionally, so the projection described here would have to be implemented if the trigger fires.

### Pre-conversion obligation: converter scale-engineering pass (2026-08-13)

The dual-driver Phase 5 joint review (`docs/dev/POST-PHASE-5-JOINT-REVIEW.md` F-13/D4) found that the converter as implemented **cannot execute the production conversion**, independently of the runtime readiness this plan otherwise reports. Three distinct limits, all confirmed against the code and all already recorded in `scripts/convert/README.md`'s "Shin-Uchuu-scale notes" as deferred to a future production pass, but scheduled by no plan until now: the identity/rank pass (`compute_identity()` in `links.py`) is in-memory over all snapshots — at 15–18B halos that is ≈600–720 GB for its five concatenated int64 columns plus ≈120–144 GB for the lexsort's order array on a 512 GB machine, and its own docstring defers the external-merge sort as a production concern (this also corrects the risk-table row above); the required producer validation battery (`validate.py`) is likewise in-memory, loading and retaining full-dataset columns, and cannot be skipped because producer validation is part of the format contract and this plan's own Definition of Done; and the scatter phase's resume model is incompatible with this plan's own "Getting the Data to the Mac" transfer strategy — `run_scatter` requires every listed source file to exist at start, freezes the ordered source set into the manifest and refuses resume when it changes, and `source_completed()` re-stats completed files, so batches deleted under the batched, consumptive-delete transfer this plan requires break resume. Also recorded in the same README note: the ~5 GB Phase 0 forest map is passed to pool workers by pickling, and the fix-up stage's sequential per-satellite scan (up to 31 searches per satellite) "would need revisiting for Shin-Uchuu."

**Scope of the pass:** external-merge rank sort (replacing the in-memory lexsort); streaming/per-snapshot validation (replacing the full-dataset-resident battery); a batch-aware scatter inventory compatible with consumptive deletes (replacing the frozen-source-set resume model); shared or memory-mapped forest-map distribution (replacing per-worker pickling); and a production-scale benchmark of the fix-up stage's sequential satellite scan with an explicit, measurement-first retain/optimize decision.

**Acceptance gate:** the full micro-Uchuu validation battery and topology cross-check must re-run green (the converter's reference semantics must not move while its machinery is rebuilt), plus a measured memory profile of the rank pass at projected Shin-Uchuu scale.

This pass is converter-side, gate-checkable, and well-bounded — it deserves its own frozen implementation plan, and is the single largest unscheduled item between here and the Shin-Uchuu production conversion.

---

## Definition of Done

1. 70 HDF5 snapshot files produced and validated (halo count, adjacency, round-trip progenitor check, FoF chain integrity, NextProgenitor same-file scope, identity uniqueness, Len non-negative)
2. **Topology cross-check passes on micro-Uchuu** by stable halo identity (the converter acceptance gate, completed long before the production run)
3. The conversion report exists and the shin-uchuu identity multiplier is set from its measured counts
4. `simulations/shin-uchuu/` package registered and building clean with the snapshot reader
5. Mimic runs `sage16` on Shin-Uchuu end-to-end: no assertion failures, no broken links, no memory errors
6. HMF and GSMF plots produced and sanity-checked at z=0, z=1, z=2
