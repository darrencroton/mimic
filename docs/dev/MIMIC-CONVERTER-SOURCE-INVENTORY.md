# Converter Source Inventory (Slice 1)

**Status:** Slice 1 deliverable of
[`MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md)
("Source inspection and reproducible capability inventory"). Read-only
evidence produced by `scripts/convert/inspect_sources.py`; no source
directory was written to. Captured on 2026-09-22 on host `djcmacstudio`.

Two different commits matter here, and they are not the same one: the
**data/converter state this report characterises** is `f5cc6004` (this
slice's starting commit -- no converter code existed at that commit, only
the source data and the shipped ASCII pipeline this report inspects
alongside it). The **commit at which the reproduction commands below
actually run** is whichever commit landed `inspect_sources.py` itself (this
slice's own commit, on top of `f5cc6004`) -- the tool that reproduces these
figures did not exist until then.

Reproduce any figure below (at the commit that added this tool) with:

```bash
mimic_venv/bin/python scripts/convert/inspect_sources.py survey --json /tmp/survey.json
mimic_venv/bin/python scripts/convert/inspect_sources.py inspect \
    --source-format lhalo_binary \
    --simulation-info simulations/mini-millennium/simulation_info.yaml \
    --a-list simulations/mini-millennium/mini-millennium.a_list
```

## 1. Adapter routes for the five requested packages

Three adapters cover all five named simulations, per C1 of the
implementation plan (ASCII retained, L-Halo binary added, Consistent-Trees
forests-HDF5 added). `inspect_sources.py`'s `ADAPTER_ROUTES` table maps
`input.tree_type` to the route; every package below resolves to exactly one.

| Package | `input.tree_type` | Adapter route |
|---|---|---|
| mini-Millennium | `lhalo_binary` | `lhalo_binary` |
| Millennium | `lhalo_binary` | `lhalo_binary` |
| micro-Uchuu | `lhalo_binary` | `lhalo_binary` |
| mini-Uchuu | `lhalo_binary` | `lhalo_binary` |
| full Uchuu | `consistent_trees_hdf5` | `ctrees_hdf5` |

micro-Uchuu also ships in `consistent_trees_ascii` (`micro-uchuu-ascii`) and
`consistent_trees_hdf5` (`micro-uchuu-hdf5`) packaging; both were inspected
below as cross-format regression anchors, in addition to the `lhalo_binary`
route the plan selects for micro-Uchuu's primary conversion.

## 2. Reachability

Free space is reported two ways, because the acceptance text ("free space on
the volume any later conversion would write to") and the source's own volume
are not always the same disk: **source volume** is where `inspect_sources.py`
read from; **default output volume** is where the converter's dev workdir
(`output/`, a symlink to `/Volumes/Internal/results/mimic`) would write a
conversion today. A production Shin-Uchuu-scale conversion instead targets
`/Volumes/LaCie` (`scripts/convert/README.md`); that distinction is the
implementation plan's, not new information from this slice.

Default output volume free space at capture time: **1.01 TiB**
(`/Volumes/Internal`, `df -k`, 2026-09-22) — same figure for every row below,
since all five packages would write to the same default dev workdir.

For the `lhalo_binary` route, "declared" is `last_file - first_file + 1`
file partitions and "present" counts the same units, so the two columns
compare directly. The `ctrees_hdf5` route's units differ (declared: 1 info
file + the info file's own `Nfiles` attribute of external-link targets;
present: same units, but only readable once the info file itself is
reachable) -- `check_hdf5_reachability` derives `declared_file_count` from
the info file's `Nfiles` attribute when it can read it, and falls back to
`last_file - first_file + 1` with an explicit note when it cannot (as for
full Uchuu below, where the info file itself is absent).

`free_bytes_on_volume` is `null` whenever the requested path does not exist
-- `free_space_bytes` deliberately does not walk up to a parent directory
and report that ancestor's real free space, which would not be reproducible
evidence about the (absent) requested path itself.

| Package | `simulation_dir` | Host | Files present / declared | Bytes present | Source-volume free | Notes |
|---|---|---|---|---|---|---|
| mini-Millennium | `simulations/mini-millennium/snapshots` (symlink) | djcmacstudio | 8 / 8 | 159,563,092 | 1.01 TiB | complete |
| Millennium | `simulations/millennium/snapshots` (symlink) | djcmacstudio | 16 / 512 | 2,468,668,284 | 1.01 TiB | 16 of 512 declared files present (first_file=0, last_file=511) |
| micro-Uchuu | `simulations/micro-uchuu/snapshots` | djcmacstudio | 4 / 4 | 2,350,178,732 | 1.01 TiB | complete |
| mini-Uchuu | `simulations/mini-uchuu/snapshots` | djcmacstudio | 16 / 128 | 18,856,486,728 | 1.01 TiB | 16 of 128 declared files present (first_file=0, last_file=127); the pinned run file (`halos-only_mini-uchuu.yaml`) exercises only files 0-3, narrower than what is on disk |
| full Uchuu | `simulations/uchuu/snapshots` | djcmacstudio | 0 / 2000 | 0 | `null` (path absent) | **`simulations/uchuu/snapshots` does not exist at all** -- confirmed absent, not a dead symlink (`Path.exists()` is `False` and there is no symlink entry to resolve). `declared_file_count` here is the `first_file`/`last_file` fallback (the info file cannot be read to get `Nfiles`), and the tool records that fallback explicitly in `notes` rather than presenting it as comparable to `present_file_count`'s units. |

micro-Uchuu forests-HDF5 (below) is a real reachable `ctrees_hdf5`-route
example where the units line up: `declared_file_count=2` (1 info file + its
own `Nfiles=1` attribute) against `present_file_count=2` (the info file plus
its one resolvable external-link target, the real 13 GB data file).

micro-Uchuu's ASCII and forests-HDF5 packagings, inspected separately as
regression anchors:

| Package | Path | Present | Bytes | Declared / present (ctrees_hdf5 units) |
|---|---|---|---|---|
| micro-Uchuu ASCII | `simulations/micro-uchuu-ascii/snapshots/{forests.list,locations.dat,tree_0_0_0.dat}` | all 3 | 10,102,809 + 20,966,804 + (tree file symlinked via `locations.dat`) | n/a (ASCII route) |
| micro-Uchuu forests-HDF5 | `simulations/micro-uchuu-hdf5/snapshots/{MicroUchuu_mergertree_info.h5,MicroUchuu_mergertree.h5}` | both | 13,085,010,936 (info file is 1.2 KB of the total; the 13 GB is the real backing data file) | 2 / 2 |

`MicroUchuu_mergertree_info.h5`'s `File0` is **not** embedded data -- it is
an `h5py.ExternalLink` to the sibling `MicroUchuu_mergertree.h5` (confirmed
by the tool's own `link_type` field, `"ExternalLink"`, and consistent with
the package README's own description of that layout). An earlier draft of
this document said "embedded directly", which was wrong; corrected here and
in §5 below.

The full-Uchuu **fixture** (as distinct from the absent production data) is
present and was inspected directly:
`simulations/uchuu/_tests/data/mergertree_info.h5` -> `ExternalLink` to
`mergertree_0.h5`, `TotNforests=3`, reachable, `n_halos=6`. This is the
committed external-link fixture C1 requires and confirms the adapter route's
external-link handling has something real to exercise even though production
data is absent.

## 3. Per-source counts and link-span summary

Link-span is `SnapNum[descendant target] - SnapNum[source]`; span 1 is
adjacent, span > 1 is a valid forward gap (counted, not rejected), span < 1
is flagged as an anomaly (`non_forward_or_zero_span`) and none occurred in
any real dataset inspected here.

Identity bounds: for L-Halo, the largest single tree in any file
(`max_tree_halo_count`) and the `MostBoundID` range across every halo
scanned (not just linked ones); for forests-HDF5, the largest single forest
(`max_forest_nhalos`, from `ForestInfo.ForestNhalos`). Both are cheap
by-products of the scans already performed above -- no extra pass.

| Source | Trees / forests | Halos | Non-null descendant links | Forward gaps (span > 1) | Max span | Non-forward anomalies | Max tree/forest size | MostBoundID range |
|---|---|---|---|---|---|---|---|---|
| mini-Millennium (8/8 files) | 29,585 | 1,533,122 | 1,495,274 | **29,291** | **2** | 0 | 14,869 | 1 .. 19,683,000 |
| Millennium (16/512 files, partial) | 443,945 | 23,720,119 | 23,138,787 | 470,782 | 2 | 0 | 116,122 | 46 .. 10,070,876,211 |
| micro-Uchuu binary (4/4 files) | 440,651 | 22,580,924 | 22,019,658 | 0 | 0 | 0 | 350,075 | 76 .. 28,706,645 |
| mini-Uchuu (16/128 files, partial) | 3,230,400 | 181,188,125 | 176,995,335 | 0 | 0 | 0 | 397,280 | -1,812,693,745 .. 1,839,941,549 |
| micro-Uchuu forests-HDF5 (File0) | 440,651 forests | 22,580,924 | 22,019,658 | 0 | 0 | 0 | 350,075 halos | n/a (no single-field identity column) |
| full-Uchuu fixture (File0) | 3 forests | 6 | 2 | 0 | 0 | 0 | 4 halos | n/a |
| micro-Uchuu ASCII (via baseline capture) | 440,651 forests | 22,580,924 | n/a (existing pipeline output, not this tool) | n/a | n/a | n/a | 350,074 (`max_halo_rank_in_forest`, `MIMIC-CONVERTER-BASELINE-REFERENCE.md`) | n/a |

micro-Uchuu binary's `max_tree_halo_count = 350,075` matches forests-HDF5's
`max_forest_nhalos = 350,075` for the same underlying dataset, and is one
more than the ASCII pipeline's `max_halo_rank_in_forest = 350,074` (a
0-indexed rank vs. a count of the same largest tree/forest) -- three
independent formats of the same source agreeing, not a discrepancy.
mini-Uchuu's negative `MostBoundID` values are expected: L-Halo carries
`MostBoundID` as signed int64 catalog/particle-identifier data with no
positivity requirement (C1), not a derived index.

**mini-Millennium reproduces the plan's measured reference exactly**: 8
files, 1,533,122 halos, 1,495,274 non-null descendant links, 29,291 forward
gaps, maximum span 2. No source-identity discrepancy was found -- this is
the same figure the plan text records from its own independent scan.

The Millennium and mini-Uchuu rows are **partial-file-range evidence**, not
whole-package totals: only the locally present files were scanned (16 of 512
for Millennium, 16 of 128 for mini-Uchuu), consistent with the data
availability section's warning about a "partial inventory trap". Whole-
package figures require the owner to supply the remaining files (see docs/
dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md's data-availability
section, which already records this gap).

micro-Uchuu is **gap-free** in every packaging inspected (binary and
forests-HDF5 both show zero forward gaps), unlike mini-Millennium. This
matches the plan's framing of micro-Uchuu as a "new-format and regression
anchor" separate from mini-Millennium's role as "the gap-containing
acceptance gate" -- the two sources are complementary evidence, not
redundant.

## 4. Available fields/types/units

### L-Halo binary (all four `lhalo_binary` packages)

Fixed 104-byte record, field order/widths taken directly from
`src/include/generated/raw_halo_defs.h` (`struct RawHalo`), reproduced in
`scripts/convert/adapters/source_inventory.py:LHALO_FIELDS`:

| Field | Type | Shape |
|---|---|---|
| Descendant, FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup | int32 | scalar |
| Len | int32 | scalar |
| M_Mean200, M_Crit200, M_TopHat | float32 | scalar |
| Pos | float32 | [3] |
| Vel | float32 | [3] |
| VelDisp, Vmax | float32 | scalar |
| Spin | float32 | [3] |
| MostBoundID | int64 | scalar |
| SnapNum, FileNr, SubhaloIndex | int32 | scalar |
| SubHalfMass | float32 | scalar |

Units per `simulation_info.yaml`: masses in `1e10 Msun/h`, positions in
`Mpc/h`, velocities in `km/s`, box size `Mpc/h`, all `h_convention: carried`.
Byte order: little-endian on this host; `read_lhalo_header`/
`lhalo_record_dtype` take byte order as an explicit argument (`'<'`/`'>'`),
never numpy's native (`'='`) packing that would silently follow host
architecture instead of the file's actual encoding.

### Consistent-Trees forests-HDF5 (`uchuu`, `micro-uchuu-hdf5`)

The real 13 GB `MicroUchuu_mergertree.h5`'s `Forests/` group exposes 69
fields (`A_x`, `Breadth_first_ID`, `Descendant`, ..., `z`), covering the
five stored links, the `Snap_num`/`Snap_idx` snapshot column (this dataset
uses `Snap_num`, int64; the full-Uchuu fixture and README document `Snap_idx`
as float64 for other packagings -- `inspect_ctrees_hdf5_source` resolves
whichever is present, matching `src/io/vertical/read_ctrees_hdf5.c:178-179`),
and many Consistent-Trees-native extras (`Mvir`, `Rvir`, `vmax`, `Spin`,
`desc_scale`, `pid`/`upid`, etc.) beyond the fixed set the current C reader
consumes. All are physically backed (`is_virtual: False` for every field
checked, both in the fixture and the real dataset) -- confirms the plan's
"No VDS requirement" finding (C1) against the real dataset, not just the
fixture.

### Consistent-Trees ASCII (`micro-uchuu-ascii`)

Reused directly from `ctrees_parser.py` (not modified by this slice):
required int columns `id, desc_id, pid, upid`; required float columns
`scale, desc_scale, mvir, vrms, vmax, x, y, z, vx, vy, vz, jx, jy, jz`;
snapshot column spelled `snap_idx` or `snap_num`. Full per-forest/per-
snapshot counts for this source come from the pre-change ASCII pipeline run
captured in `MIMIC-CONVERTER-BASELINE-REFERENCE.md`, which exercises the
same parser exhaustively rather than duplicating it here.

## 5. Source dependencies and resource estimates

- **L-Halo binary**: self-contained per file (header + payload); no external
  dependency beyond the file itself and the package's `simulation_info.yaml`/
  `a_list`.
- **Consistent-Trees forests-HDF5**: the info file's `FileN` groups are
  `ExternalLink`s to sibling `mergertree_N.h5`-style files -- for full Uchuu,
  2000 of them; for micro-Uchuu, one (`MicroUchuu_mergertree.h5`, confirmed
  as an `ExternalLink`, not embedded data, per the correction in §4 above).
  `check_hdf5_reachability` enumerates every `ExternalLink` target and
  reports any that are absent; none were absent for the two reachable
  sources (micro-Uchuu forests-HDF5, the full-Uchuu fixture).
- **Consistent-Trees ASCII**: depends on `forests.list` and `locations.dat`
  alongside the tree file(s) they index.
- **Resource estimate for a full pass**: `inspect_sources.py inspect` with
  link scanning enabled read every present byte once. Measured wall-clock on
  this host: mini-Millennium (8 files, 152 MB) < 1 s; micro-Uchuu binary (4
  files, 2.2 GB) ~8 s; Millennium (16 files, 2.3 GB, partial) ~8 s; mini-
  Uchuu (16 files, 17.6 GB, partial) ~53 s; micro-Uchuu forests-HDF5 (22.6 M
  halos) ~1 s. Memory: the L-Halo scan is bounded by the largest tree in a
  file (not the whole file); the forests-HDF5 scan loads the snapshot column
  and forest-offset arrays in full (three int64/float64 arrays sized to the
  file's halo count -- about 540 MB for micro-Uchuu's 22.6 M halos), which is
  acceptable for one-off inspection but is explicitly **not** the bounded
  streaming discipline C4 requires of the production adapters (Slices 3-6).

## 6. Sources unreachable for a later slice's acceptance

**Full Uchuu's production data is unreachable and blocks Slice 11's
acceptance evidence on real data for that package.**
`simulations/uchuu/snapshots` does not exist on this host; the plan's own
resource envelope section already documents that even if it existed, this
machine's ~8.4 TB of free space (LaCie + Scratch + Internal) is short of the
~35 TB peak working set a full-Uchuu conversion would need, and identifies
this as a provisioning question for the owner (a mount, plus a large-volume
conversion host such as NT or dedicated storage). This slice adds no new
information on that point beyond confirming, by direct inspection today,
that the gap is exactly as the plan already described it: the fixture-based
adapter evidence (Section 2 above) is real and reachable; the production
37 TB catalogue is not, and nothing in this slice's authorized surface
(read-only inspection) can change that. Raised here per this slice's
acceptance criterion, not deferred to Slice 11.

Millennium and mini-Uchuu are reachable but **partial** (16 of 512 and 16 of
128 files respectively); whole-package acceptance evidence for those two
packages needs the owner to supply the remaining files, exactly as
`MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`'s "Known gaps at the
time of writing" table already states. No new gap is introduced here.

## 7. Invalid-input handling

Exercised in `scripts/convert/tests/test_inspect_sources.py` against
synthetic fixtures (not real data): truncated header, truncated tree-count
table, a header claiming far more trees than the file could hold (rejected
against the already-known file size *before* attempting the multi-GiB read
that count would imply), negative `Ntrees`, per-tree counts summing to
something other than the header's `totNHalos`, truncated payload, an
out-of-tree `Descendant` index (binary), an out-of-forest `Descendant` index
(forests-HDF5), a `Descendant` value below the `-1` null sentinel (e.g. `-2`,
both routes -- matches `read_ctrees_hdf5.c`'s `CT_ASSIGN_LINK`, which accepts
exactly `[-1, nhalos)`), overlapping/out-of-order `ForestInfo` offset rows, a
missing `Snap_num`/`Snap_idx` column, and a non-integral `Snap_idx` value
(checked by an exact `floor(v) == v`, not a tolerant `np.allclose` that would
accept non-integral values at large magnitudes) -- all raise `ConverterError`
and abort. A missing `h5py` is reported as `MissingDependencyError` and
propagates to a nonzero process exit for `inspect`; for `survey`, a
per-package exception (broadened to cover the realistic numpy/h5py failure
modes on untrusted input, not just this tool's own declared error types) is
recorded as that package's `inspection_error` without discarding the other
packages' already-gathered results, and the overall process still exits
nonzero if any package hit one. A valid forward gap (mini-Millennium's
29,291 of them) is counted, not rejected, confirmed by the exact-reproduction
test above.

A mistyped `--simulation-info`/`--a-list` path or a `--json` path into a
missing directory now also surfaces as the tool's own `error: ...` message
and a nonzero exit, not a raw Python traceback (`main()` catches
`ConverterError`, `MissingDependencyError`, `OSError` and `ValueError`).
