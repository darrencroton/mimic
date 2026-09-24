# Converter Generalisation Acceptance Evidence (Slice 11)

**Status:** Slice 11 deliverable of [`MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md) ("Acceptance evidence on real data and source fixtures"). Every figure below was produced on 2026-09-24/25 on host `djcmacstudio` by the Slice 10 instrument, `scripts/convert/tests/run_generalisation_acceptance.py`, at commit `e011625ccb7d5aa20f79f2dcd0dcc521b9bcce21` with a clean working tree (every one of the 62 recorded runs reports `git_dirty_paths: 0` and the same harness SHA-256). No converter, reader, comparator or test code was changed to produce it.

**What this is and is not.** It is conversion evidence: each version 3 dataset below says exactly what the selected source format's own vertical interpretation says, halo by halo, link by link and bit by bit. It is **not** runtime evidence. The current Mimic rejects version 3 (`src/io/horizontal/read_horizontal_hdf5.c`), and every conversion report below opens with `NOT RUNNABLE BY THE CURRENT MIMIC`. No v3 runtime-parity claim is made here; that belongs to the runtime follow-on.

## 1. Populations: real, sampled, fixture, unavailable

| Population | Kind | Inventory used | Halos | Why this range |
|---|---|---|---|---|
| mini-Millennium, L-Halo binary | **real, complete** | `trees_063.0`–`.7` (8 of 8) | 1,533,122 | whole package |
| micro-Uchuu, L-Halo binary | **real, complete** | `Uchuu100_Planck_lhalo_binary.0`–`.3` (4 of 4) | 22,580,924 | whole package |
| micro-Uchuu, forests-HDF5 | **real, complete** | `MicroUchuu_mergertree_info.h5` → ExternalLink `MicroUchuu_mergertree.h5` (13 GB) | 22,580,924 | whole package |
| micro-Uchuu, Consistent-Trees ASCII | **real, complete** | `forests.list` + `tree_0_0_0.dat` (md5 `45b72a4f910831482a7bf3e9d2163ab3`) | 22,580,924 | whole package; v2 regression and v3 route |
| Millennium, L-Halo binary | **real, sampled subset** | files 0–15 of 512 declared | 23,720,119 | every locally present file; the same range `halos-only_millennium.yaml` pins |
| mini-Uchuu, L-Halo binary | **real, sampled subset** | files 0–15 of 128 declared | 181,188,125 | every locally present file, **wider** than the 0–3 that `halos-only_mini-uchuu.yaml` pins (the plan's "partial inventory trap"); the dump run file overrides only `first_file`/`last_file` and the output keys |
| full Uchuu, forests-HDF5 | **committed fixture only** | `simulations/uchuu/_tests/data/mergertree_info.h5` → ExternalLink `mergertree_0.h5` | 6 (3 forests) | the production catalogue is not on this host |
| full Uchuu production (≈181.5 × 10⁹ halos, 2000 files) | **unavailable** | `simulations/uchuu/snapshots` does not exist | — | not converted, not validated, not claimed |

**Sample definition.** A sample here is a leading file range, `first_file = 0` through `last_file = 15`. Every L-Halo tree is complete within one file, so every tree in the sample is a complete tree. The sample starts at file 0, so its `SourceHaloID` prefix sums, tree numbers and file/unit ordinals are exactly the parent inventory's own: C1's "identical leading subsets preserve their existing prefix identities". **These are subsets, not whole-package conversions.** Nothing here claims Millennium or mini-Uchuu was converted in full; the owner has not supplied files 16–511 or 16–127.

## 2. Results by route

Each route ran `dump` (the C harness `dump_ctrees_topology --source-payload`, built for that simulation with `MODEL=halos-only`), `convert` (`convert_trees.py` ingest → transpose → write → validate → report into a fresh workdir), `compare` (dataset against the dump) and `compare-extras` (SourceHaloID identity and any selected extras against independent source extraction). Every run exited 0 and every verdict is PASS, with zero failures in every check.

| Route | Profile | Rows matched (`compare`) | `compare` checks | `compare-extras` checks | Producer battery |
|---|---|---|---|---|---|
| mini-Millennium | shipped `converter_columns.yaml` | 1,533,122 / 1,533,122 | 25 / 25 PASS | identity PASS | PASS |
| mini-Millennium | `profiles/lhalo_binary_extras_example.yaml` | 1,533,122 | 25 / 25 PASS | 6 extras + identity PASS | PASS |
| micro-Uchuu L-Halo | shipped | 22,580,924 | 25 / 25 PASS | identity PASS | PASS |
| micro-Uchuu forests-HDF5 (real 13 GB) | shipped | 22,580,924 | 25 / 25 PASS | identity PASS | PASS |
| micro-Uchuu forests-HDF5 (real 13 GB) | `profiles/consistent_trees_hdf5_extras_example.yaml` | 22,580,924 | 25 / 25 PASS | 3 extras + identity PASS | PASS |
| micro-Uchuu ASCII → v3 | shipped | 22,580,924 | 24 PASS + `source_halo_id` not applicable (0 compared, by design) | identity PASS | PASS |
| Millennium files 0–15 | shipped | 23,720,119 | 25 / 25 PASS | identity PASS | PASS |
| mini-Uchuu files 0–15 | shipped | 181,188,125 | 25 / 25 PASS | identity PASS | PASS |
| full-Uchuu fixture | shipped | 6 | 25 / 25 PASS | identity PASS | PASS |

The 25 `compare` checks are `row_coverage` (none dropped, none extra), `duplicate_reference_rows`, `duplicate_converted_rows`, `dump_integrity`, `snapnum`, `source_halo_id` (inventory prefix sum), `converter_link_encoding`, `converter_link_targets`, the five `link_*` checks (every link resolves to the reference's target halo, which is how a reordered progenitor or FoF chain would show up), the three `target_snapshot_*` checks, `payload_Len`, `payload_MostBoundID`, the six binary32-bit `payload_*` float checks (`M_Crit200`, `Pos`, `Vel`, `Spin`, `VelDisp`, `Vmax`) and `payload_storage`. Topology is compared in the reference's own key space, `(ForestIndex, HaloRankInForest)`; `MostBoundID` is compared as data, never used as a key. **Which independent source settles what:** links, chains, target snapshots, `SnapNum` and the core payload are settled against the C dump, which is Mimic's unmodified vertical reader extracting the source bytes itself and shares no converter code. `SourceHaloID` identity, the native catalog identifier and the selected extras are settled against the harness's own Python extractors (L-Halo records through a layout recomputed from `halo_properties.yaml`, raw `h5py`, a plain ASCII parse). The Python extractors do not re-derive links; that is by the Slice 10 design, and this slice changes no comparator.

**Selected extras on real data.** mini-Millennium carried `M_Mean200`, `M_TopHat`, `SubHalfMass`, `SourceFileNr` (from `FileNr`), `SubhaloIndex` and `PosX` (component 0 of `Pos`). The real forests-HDF5 dataset carried `CatalogRvir` (`Rvir`, double), `SpinParameter` (the dimensionless ctrees `Spin`, not the J/Mvir payload) and `ParentID` (`pid`, int64). Every extra agreed bit for bit with the independent extractor over every row: 1,533,122 per mini-Millennium extra and 22,580,924 per HDF5 extra. The shipped simulation profiles select no extras, so "selected payload field" on those routes means the core payload, all of which is covered by `compare`.

**Why ASCII's `source_halo_id` is not applicable.** For ASCII the vertical reader's forest enumeration is the dense forest-id order, not the inventory order, so the dump cannot state an inventory prefix sum. `compare-extras` verifies ASCII `SourceHaloID` identity instead, by independent text extraction in inventory order: 22,580,924 rows compared, 0 failures. Its process-lifetime peak RSS was 3.83 GiB, which measures the memory risk Slice 10 flagged for this extractor. The run met it with no failure on this 512 GiB host.

### 2.1 mini-Millennium: the gap-containing gate

| Figure | Plan / Slice 1 | Converter report | Independent census of the emitted HDF5 |
|---|---|---|---|
| Halos | 1,533,122 | 1,533,122 | 1,533,122 |
| Non-null descendants | 1,495,274 | 1,495,274 | 1,495,274 |
| Skipped (span > 1) descendant links | 29,291 | 29,291 | 29,291 |
| Maximum span | 2 | 2 | 2 |
| Snapshot files (empty) | — | 64 (8 empty) | 64 (8 empty) |

The census is a raw-`h5py` scan that imports nothing from the converter. It reads `SnapNum`, `Descendant` and `DescendantSnapshot` per file and asserts every non-null descendant points forward. `row_coverage` matched every one of the 1,533,122 dumped halos to exactly one converted row, and the dataset holds 1,533,122 rows, so **no row was inserted**. `links_adjacent = 0` is measured. The report also records 26,210 gapped FirstProgenitor links and 3,979 NextProgenitor links off their owner's snapshot, all preserved.

### 2.2 Source-relative identities and unit ordinals

`source_halo_id` passes on every non-ASCII route: `SourceHaloID` equals the prefix sum over the declared source order, starting at 1. As an additional check, the rank-0 row of every forest in each dump (the C reader's file number and within-file unit) was compared with `forests.h5`'s `SourceFileOrdinal`/`SourceUnitOrdinal` at that `ForestIndex`:

| Route | Sidecar forests | Dump forests | Ordinal mismatches | File-ordinal range |
|---|---|---|---|---|
| mini-Millennium | 29,585 | 29,585 | 0 | 0–7 |
| micro-Uchuu L-Halo | 440,651 | 440,651 | 0 | 0–3 |
| micro-Uchuu forests-HDF5 | 440,651 | 440,651 | 0 | 0–0 |
| full-Uchuu fixture | 3 | 3 | 0 | 0–0 |
| Millennium 0–15 | 443,945 | 443,945 | 0 | 0–15 |
| mini-Uchuu 0–15 | 3,230,400 | 3,230,400 | 0 | 0–15 |
| micro-Uchuu ASCII → v3 | 440,651 | 440,651 | not comparable | 0–0 |

For ASCII, the C reader's "unit" is its dense forest enumeration, while the converter's `SourceUnitOrdinal` is the forest's position in the source file. The two are different definitions, so this pair is not a check. The ASCII sidecar was confirmed to hold a complete permutation of 0–440,650 (no −1, the single-file case) with `ForestID` strictly ascending. Reference and converted sides used the same run-file inventory in every case: the dump's run file and the converter's `--first-file`/`--last-file` name the same range.

### 2.3 Missing requested files fail

Millennium's file 16 is absent. Requesting files 0–16 fails on every side, and each failure is recorded:

- `convert`: ingest exits 1, `source dependency .../trees_063.16 cannot be pinned: [Errno 2] No such file or directory`.
- `compare-extras`: exits 2, `requested L-Halo file simulations/millennium/snapshots/trees_063.16 is missing`.
- `dump`: exits 1, `FATAL: Requested input file 16 is missing; a source dump never narrows the configured FirstFile..LastFile inventory`. Its partial output file ends without the mandatory `# end rows N forests F` trailer, so the comparator rejects it as truncated.

The two samples are therefore deliberate, labelled subsets of what is present, never a silent narrowing of a larger request.

### 2.4 Full-Uchuu external-link fixture

The info file's `File0` is an ExternalLink to `mergertree_0.h5`. The C harness read it through the vertical `consistent_trees_hdf5` reader (partition model `enumerated`); the converter read it through the forests-HDF5 adapter. All 25 `compare` checks and `compare-extras` pass over its 6 halos in 3 forests. **This is the only full-Uchuu evidence.** Production-scale conversion and validation of full Uchuu remain unperformed: the source is not mounted and the host lacks the storage (§5). Nothing here should be read as a full-Uchuu pass.

## 3. Default ASCII: v2 regression, totals and the Slice 1 comparison

A fresh default-v2 conversion used the unchanged six-phase `convert_ctrees.py` sequence from [`MIMIC-CONVERTER-BASELINE-REFERENCE.md`](MIMIC-CONVERTER-BASELINE-REFERENCE.md). It ran in `/Volumes/Internal/results/mimic/convert/slice11-ascii-v2`, the same volume as Slice 1's capture, followed by the standalone battery and the crosscheck (reference run built with `make MODEL=halos-only SIMULATION=micro-uchuu-ascii`, topology dump from the v1 mode of the same-pair harness):

- Producer battery (`validate.py`): **15 / 15 PASS** (`file-set`, `object-set`, `sidecar-object-set`, `manifest-binding`, `header-values`, `run-scoped-headers`, `slab-order`, `link-ranges`, `fof-chains`, `progenitor-closure`, `identity`, `header-bounds`, `sidecar-content`, `len-nonnegative`, `count-conservation`).
- Crosscheck (`crosscheck.py compare --reference-topology`): **8 / 8 PASS** (`reference-sanity`, `identity-forest`, `identity-creation`, `fof-central`, `mostboundid-positive`, `values`, `occupancy`, `topology-chains`).

| Total | 2026-08-28 record | Slice 1 (2026-09-22) | This run | Repeat run |
|---|---|---|---|---|
| Halos | 22,580,924 | 22,580,924 | **22,580,924** | 22,580,924 |
| Populated snapshots | 50 | 50 | **50** | 50 |
| Forests | 440,651 | 440,651 | **440,651** | 440,651 |
| `max_halo_rank_in_forest` | 350074 | 350074 | **350074** | 350074 |

**Nothing moved, so no discrepancy needs attributing.** Inventory on each side: the 2026-08-28 record (`scripts/convert/README.md`) names the same `simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat` but records no content hash; Slice 1 records md5 `45b72a4f910831482a7bf3e9d2163ab3`; this slice measured the same md5 before comparing. Both of this slice's ASCII runs therefore used Slice 1's exact inventory.

**Timing against Slice 1** (`/usr/bin/time -l` there; the harness's `wait4` child accounting here; both read the child's own `ru_maxrss`):

| Phase | Slice 1 wall (s) | This run | Repeat | Change | Slice 1 peak RSS (GiB) | This run | Repeat |
|---|---|---|---|---|---|---|---|
| scatter | 123.33 | 132.4 | 130.9 | +7.4% / +6.1% | 2.41 | 2.41 | 2.41 |
| sort | 15.36 | 17.3 | 17.5 | +12.6% / +13.9% | 1.96 | 1.96 | 1.96 |
| fixups | 13.01 | 15.5 | 15.5 | +19.1% / +19.1% | 2.02 | 2.01 | 2.01 |
| links | 44.21 | 42.8 | 41.7 | −3.2% / −5.7% | 4.23 | 4.25 | 4.25 |
| write | 10.11 | 11.7 | 11.7 | +15.7% / +15.7% | 1.27 | 1.27 | 1.31 |
| report | 6.07 | 6.8 | 6.9 | +12.0% / +13.7% | 0.35 | 0.35 | 0.35 |
| **total** | **212.09** | **226.5** | **224.2** | **+6.8% / +5.7%** | **4.23 peak** | **4.25 peak** | **4.25 peak** |

**No phase regresses by more than 20%**, and peak RSS is unchanged (+0.4%, the `links` phase). The closest is `fixups`, a repeatable +19.1% in both runs, about +2.5 s. It is below the gate and was not investigated further. Recording it here means a later change that pushes it over the line starts from a measured number rather than from Slice 1's.

The generic ASCII → v3 route was also run as additional evidence (§2): all 25 applicable `compare` checks and the `compare-extras` identity check pass over 22,580,924 halos. Its report's totals equal the v2 run's: 22,580,924 halos, 50 snapshots, 440,651 forests, max rank 350074.

## 4. Times, peak RSS, storage and spill

Peak RSS is the child's own for `dump` and `convert` stages, and the comparator process's lifetime peak for `compare`/`compare-extras`. Workdir figures are allocated bytes after each stage. Transient spill is reported separately: the transpose spills into its own stage directory and removes it, so the figure after the stage excludes it.

| Route | Dump wall / RSS | Convert wall (ingest+transpose+write+validate+report) | Largest stage RSS | Workdir after write | Transpose spill | `compare` wall / RSS / spill |
|---|---|---|---|---|---|---|
| mini-Millennium | 6.1 s / 0.03 GiB | 39.9 s | 2.38 GiB (transpose) | 0.97 GB | 0.51 GB | 18.3 s / 3.95 GiB / 0 |
| micro-Uchuu L-Halo | 43.1 s / 0.67 GiB | 502.7 s | 3.26 GiB (transpose) | 10.09 GB | 10.17 GB | 437.6 s / 4.19 GiB / 14.93 GB |
| micro-Uchuu forests-HDF5 | 43.5 s / 0.87 GiB | 549.9 s | 3.23 GiB (transpose) | 10.11 GB | 10.17 GB | 440.2 s / 4.19 GiB / 14.93 GB |
| micro-Uchuu ASCII → v3 | 103.1 s / 0.59 GiB | 1,087.2 s | 4.82 GiB (ingest) | 17.57 GB | 10.17 GB | 611.5 s / 4.19 GiB / 14.93 GB |
| Millennium 0–15 | 46.2 s / 0.39 GiB | 541.2 s | 3.05 GiB (transpose) | 10.57 GB | 10.88 GB | 432.8 s / 4.74 GiB / 15.69 GB |
| mini-Uchuu 0–15 | 359.1 s / 1.17 GiB | 4,704.6 s | 3.28 GiB (transpose) | 78.66 GB | 84.72 GB | 9,539.3 s / 5.02 GiB / 240.10 GB |
| full-Uchuu fixture | 0.4 s / 0.01 GiB | 4.6 s | 0.11 GiB | 0.02 GB | — | 0.1 s / 0.05 GiB / 0 |

Every conversion used the CLI default `--memory-budget-mb 2048`; the reports record transpose peak resident terms of 2,147,475,315–2,147,483,116 bytes, just under that 2 GiB (2,147,483,648-byte) ceiling. The comparators ran with `--budget-mb 4096`. Their external sorts record fixed widths of 208 B per halo record and 40 B per link request/resolved link. Dump text size is ≈170 B/halo (mini-Uchuu's is 31 GB). Code identities: harness SHA-256 `a1b99cdbea13769fccdc1ead044c86d988dd0753407486131b39612562e748bc`; one dump executable per simulation, each SHA-256 recorded at build time; Apple clang 21.0.0; Python 3.14.6, numpy 2.3.4, h5py 3.15.1 (`mimic_venv`).

## 5. Measured v3 B/halo and the re-derived resource envelope

This section supersedes the plan's ≈140 B/halo **estimate** with measurement. The plan's own table is frozen for the run and is not edited; carrying this into it is a post-run amendment for the owner.

**Logical width** (sum of `/halos` dataset item widths): exactly **140 B/halo** for the default schema on all three adapters, as the plan estimated. The example extras profiles add 24 B each, giving **164 B/halo**. **Emitted bytes on disk**, from the files actually written (apparent size of every `snapshot_*.h5` plus `forests.h5`, divided by halos):

| Dataset | Halos | Emitted bytes | On-disk B/halo |
|---|---|---|---|
| mini-Millennium | 1,533,122 | 517,006,506 | 337.22 |
| micro-Uchuu (L-Halo, forests-HDF5 and ASCII → v3: byte-identical totals) | 22,580,924 | 3,416,361,093 | 151.29 |
| Millennium files 0–15 | 23,720,119 | 3,600,190,457 | 151.78 |
| mini-Uchuu files 0–15 | 181,188,125 | 25,669,713,672 | 141.67 |
| micro-Uchuu forests-HDF5, 3 extras | 22,580,924 | 4,000,077,296 | 177.14 |
| mini-Millennium, 6 extras | 1,533,122 | 605,480,490 | 394.93 |

The excess over 140 B/halo is HDF5's whole-chunk allocation: every dataset's final 65,536-row chunk is allocated in full, which dominates small snapshots (mini-Millennium's 56 populated files average 27 k halos). It falls toward the logical floor as snapshots grow, from 151 B/halo at ≈0.45 M halos per snapshot to 141.67 B/halo at ≈3.6 M.

**Re-derived envelope** (measured rows are measured; every other figure is labelled as extrapolation):

| Target | Halos | v3 output | Basis | Status |
|---|---|---|---|---|
| mini-Millennium | 1,533,122 (measured, 8/8 files) | **0.517 GB** | measured | fits trivially |
| micro-Uchuu | 22,580,924 (measured) | **3.42 GB** | measured | fits trivially |
| Millennium | 23,720,119 in files 0–15 (measured); whole package unmeasured | **3.60 GB** for 0–15; ≈0.11 TB for 512 files | measured subset; whole package is a ×32 file-count extrapolation at ≈142–152 B/halo, not a measurement (file sizes vary) | owner supplies files 16–511 for whole-simulation evidence |
| mini-Uchuu | 181,188,125 in files 0–15 (measured); whole package unmeasured | **25.67 GB** for 0–15; ≈0.21 TB for 128 files | measured subset; ×8 file-count extrapolation | owner supplies files 16–127 for whole-simulation evidence |
| full Uchuu | ≈181.5 × 10⁹ (package README, unverified) | ≈25.4 TB logical floor; **≈25.7 TB** at the measured large-snapshot rate of 141.67 B/halo | extrapolation from README totals and measured B/halo | needs a mount and a conversion host |

**Working storage, measured.** Without consumptive deletion, the workdir after `write` holds 434 B/halo for mini-Uchuu 0–15 and 447 B/halo for micro-Uchuu (ingest chunks + transposed snapshots + dataset). Transpose spill adds a transient ≈467 B/halo on top while that stage runs (84.72 GB for mini-Uchuu). The producer battery spilled a further ≈298 B/halo (53.98 GB) in its own temporary area. At full-Uchuu scale that is ≈79 TB of workdir plus ≈85 TB transient transpose spill without `--consume-transposed`. That is roughly 10–20× the ≈8.4 TB of free space the plan records for this host. This confirms and sharpens the plan's conclusion: full Uchuu is a **provisioning question for the owner**, not something this host or any slice can resolve.

## 6. Unchanged state

- **Sources:** no file under any `simulations/*/snapshots` source directory used here is newer than 2026-09-24 (the mini-Millennium files date from 2025-07-23). The ASCII tree file's md5 is unchanged from Slice 1. Every source was only ever read.
- **Symlinks:** every `simulations/*/snapshots` symlink resolves exactly as before (`/Volumes/Internal/data/...`); none was created, removed or retargeted.
- **Baselines and repository:** `git status --short` was empty after every run; no baseline or tracked generated file changed. All workdirs, dumps and spill live outside the repository.
- **Runtime:** nothing here claims or demonstrates v3 runtime parity.

## 7. Reproducing

Evidence root: `/Volumes/Scratch/mimic-slice11`. It holds `record.json` (the harness's own 62-entry measurement record, SHA-256 `968bfdfe6ac55dcdcf227d3e225427c5f6790b6fbfdef537ebf3ee0d4fd4e03e`, including every command line, exit code, wall/CPU time, peak RSS, input file identity and storage width), `reports/*.json`, `logs/`, `dumps/`, `work/` and the bash drivers under `scripts/`. It is disposable scratch, not a durable archive; the figures above are quoted in full so this document stands without it. The drivers reduce to the following, run from the repository root with `mimic_venv/bin/python`, `R="--record $E/record.json"`, and `TMPDIR=$E/spill`:

```bash
H=scripts/convert/tests/run_generalisation_acceptance.py
# once per SIMULATION in mini-millennium micro-uchuu micro-uchuu-hdf5 micro-uchuu-ascii
#   millennium mini-uchuu uchuu:
python $H build-dump $R --model halos-only --simulation $SIM --build-dir $E/dump-build/$SIM
# dump run file = models/halos-only/input/halos-only_$SIM.yaml with output_format: binary and a
# scratch output_directory; mini-uchuu also sets last_file: 15; uchuu points simulation_dir at
# simulations/uchuu/_tests/data (tree_name mergertree_info.h5, files 0-0)
python $H dump $R --tool $E/dump-build/$SIM/dump_ctrees_topology --run-file dump_$SIM.yaml \
    --out $E/dumps/$SIM.dump

# L-Halo route (mini-millennium 0-7, micro-uchuu 0-3, millennium 0-15, mini-uchuu 0-15):
S=simulations/$SIM
python $H convert $R --workdir $E/work/$TAG --simulation-info $S/simulation_info.yaml -- \
    --source-format lhalo_binary --simulation-info $S/simulation_info.yaml --a-list $S/$SIM.a_list \
    --column-map $S/converter_columns.yaml --source-dir $S/snapshots --tree-name $TREE \
    --halo-properties $S/halo_properties.yaml --first-file $FIRST --last-file $LAST
python $H compare $R --dataset $E/work/$TAG/write/attempt_001 --dump $E/dumps/$SIM.dump \
    --source-format lhalo_binary --budget-mb 4096 --spill-dir $E/spill --report $TAG.compare.json
python $H compare-extras $R --dataset $E/work/$TAG/write/attempt_001 --source-format lhalo_binary \
    --column-map $S/converter_columns.yaml --source-dir $S/snapshots --tree-name $TREE \
    --first-file $FIRST --last-file $LAST --halo-properties $S/halo_properties.yaml \
    --budget-mb 4096 --spill-dir $E/spill --report $TAG.extras.json

# forests-HDF5 route: --source-format consistent_trees_hdf5 --info-file <info.h5>
#   --first-file 0 --last-file 0 in place of the L-Halo inventory, on both convert and
#   compare-extras. ASCII v3 route: --source-format consistent_trees_ascii
#   --forests-list $S/snapshots/forests.list --tree-file $S/snapshots/tree_0_0_0.dat.
# Extras runs swap --column-map for scripts/convert/profiles/<format>_extras_example.yaml.
# ASCII v2 regression: the six convert_ctrees.py phases of MIMIC-CONVERTER-BASELINE-REFERENCE.md,
#   then validate.py and crosscheck.py prepare / run-reference / compare --reference-topology as in
#   scripts/convert/README.md, each wrapped in `python $H exec $R --label <phase> -- ...`.
```
