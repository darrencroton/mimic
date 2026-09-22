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
| micro-Uchuu ASCII | `simulations/micro-uchuu-ascii/snapshots/{forests.list,locations.dat,tree_0_0_0.dat}` | all 3 | 10,102,809 + 20,966,804 + 11,515,537,257 = 11,546,606,870 (~10.75 GiB total) | n/a (ASCII route) |
| micro-Uchuu forests-HDF5 | `simulations/micro-uchuu-hdf5/snapshots/{MicroUchuu_mergertree_info.h5,MicroUchuu_mergertree.h5}` | both | 13,085,010,936 (info file is 1.2 KB of the total; the 13 GB is the real backing data file) | 2 / 2 |

`tree_0_0_0.dat` is the ASCII route's largest file by far (11,515,537,257
bytes, `ls -la`) -- a real regular file, not a symlink. An earlier draft of
this table both omitted its size and mischaracterised it as "symlinked via
`locations.dat`"; `locations.dat`'s `Filename` column *references*
`tree_0_0_0.dat` by name (`#TreeRootID FileID Offset Filename` /
`28456576 0 3663 tree_0_0_0.dat`, confirmed by reading the file directly) --
that is an index entry pointing at a real file, not a filesystem symlink.

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

Per-field record units come from `simulations/<package>/halo_properties.yaml`
(verified by reading `simulations/mini-millennium/halo_properties.yaml`
directly), not `simulation_info.yaml`: `M_Mean200`/`M_Crit200`/`M_TopHat`/
`SubHalfMass` in `1e10 Msun/h` (`h_convention: carried`); `Pos` in `Mpc/h`
(`carried`); `Vel`/`VelDisp`/`Vmax` in `km/s` (`h_convention: none` --
peculiar velocities, not comoving-scaled); `Spin` in `Mpc/h km/s` (no
`h_convention` entry in the YAML, i.e. the raw specific-angular-momentum
product, not independently h-scaled); `Len` in `particles`; the five link
fields, `SnapNum`, `FileNr` and `SubhaloIndex` are `dimensionless`.
`simulation_info.yaml` carries only two of the figures the earlier draft of
this section attributed to it: `particle_mass` (`1e10 Msun/h`, `carried`)
and `box_size` (`Mpc/h`, `carried`) -- global simulation metadata, not
per-record field units. Byte order: little-endian on this host;
`read_lhalo_header`/
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

**Units for the core fields the C reader consumes**, per
`simulations/uchuu/halo_properties.yaml` and
`simulations/micro-uchuu-hdf5/halo_properties.yaml` (structurally identical
to each other; verified by reading both directly -- neither is
`simulation_info.yaml`, matching the L-Halo units correction above):

| Source field(s) | RawHalo field | Units | `h_convention` |
|---|---|---|---|
| `Mvir` | `M_Crit200` | native `Msun/h` (**not** `1e10 Msun/h` -- the generated accessor applies the x1e-10 conversion) | carried |
| `x, y, z` | `Pos` | `Mpc/h` (comoving) | carried |
| `vx, vy, vz` | `Vel` | `km/s` (peculiar velocity) | none |
| `Jx, Jy, Jz` | `Spin` | `Mpc/h km/s` -- specific angular momentum `J/Mvir` after `apply_ctrees_value_conventions()` for non-zero Mvir; zero-mass halos retain raw `J` | (not h-scaled independently) |
| `vrms` | `VelDisp` | `km/s` | none |
| `vmax` | `Vmax` | `km/s` | none |
| `id` | `MostBoundID` | dimensionless (carried-through catalog/particle identifier) | n/a |
| `Snap_num`/`Snap_idx` | `SnapNum` | dimensionless | n/a |
| -- (derived) | `Len` | `particles`, `round(Mvir x 1e-10 / particle_mass)` | n/a |

### Consistent-Trees ASCII (`micro-uchuu-ascii`)

Reused directly from `ctrees_parser.py` (not modified by this slice):
required int columns `id, desc_id, pid, upid`; required float columns
`scale, desc_scale, mvir, vrms, vmax, x, y, z, vx, vy, vz, jx, jy, jz`;
snapshot column spelled `snap_idx` or `snap_num`. Units are identical to the
forests-HDF5 table just above -- `simulations/micro-uchuu-ascii/halo_properties.yaml`
is, by its own header comment, "structurally identical to
micro-uchuu-hdf5/halo_properties.yaml" (verified by reading it directly),
since both readers share the same bridge and RawHalo contract. Full per-forest/per-
snapshot counts for this source come from the pre-change ASCII pipeline run
captured in `MIMIC-CONVERTER-BASELINE-REFERENCE.md`, which exercises the
same parser exhaustively rather than duplicating it here -- full link-span/
topology/gap reconstruction for ASCII stays out of this slice's scope (that
duplicates Slice 5's job).

`inspect_ascii_source` does add one cheap real count, via the parser's own
existing independent pre-count helper (`ctrees_parser.prescan_file`: one
stream pass, no pandas, no topology work -- the same helper the scatter
stage already uses for its own pre-count) rather than reporting nothing
between "file exists" and "the baseline capture's totals": a total row count
and `#tree`-marker count per tree file, unless `--no-link-scan` skips it (a
single pass over 22.6M lines takes real time -- see §5). Measured on real
micro-uchuu-ascii data: `total_rows=22,580,924`, `total_tree_markers=561,266`
(`tree_0_0_0.dat`, md5 `45b72a4f910831482a7bf3e9d2163ab3`) -- exact matches
to the ASCII pipeline's own halo total and the forests-HDF5 dataset's
`TotNtrees`, and the same md5 the baseline capture's own
`conversion_report.txt` records for its source file. Three independent
tools (this prescan, the full pipeline, and the forests-HDF5 inspection)
agreeing is itself evidence these are the same underlying dataset correctly
identified across formats.

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
  halos) ~1 s; micro-Uchuu ASCII prescan (22.6 M lines, one stream pass,
  no pandas) ~49 s -- this is the one `--no-link-scan` gates for the ASCII
  route. Memory: the L-Halo scan is bounded by the largest tree in a
  file (not the whole file); the forests-HDF5 scan loads **four** int64/
  float64 arrays sized to the file's halo count in full (`raw_snap`, its
  `all_snap` int64 copy, `row_offset`, `forest_count_for_row` -- an earlier
  draft of this section said "three... about 540 MB", undercounting by one
  array), roughly 722 MB by arithmetic (4 x 8 bytes x 22.6 M) for
  micro-Uchuu's 22.6 M halos, **measured at 962.6 MB peak** via
  `tracemalloc` around the same `inspect_ctrees_hdf5_source` call (the
  measured figure is higher than the four-array arithmetic because it also
  captures `forest_info`'s structured array, `np.unique`'s output, and other
  transient allocations the simple count misses). Acceptable for one-off
  inspection but explicitly **not** the bounded streaming discipline C4
  requires of the production adapters (Slices 3-6).

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
that count would imply), negative `Ntrees`, **a negative individual per-tree
halo count that still sums correctly against the header's declared total**
(`5 + (-2) == 3` -- the header-total check alone would accept this; only an
explicit per-tree check catches it), truncated payload, an out-of-tree
`Descendant` index (binary), an out-of-forest `Descendant` index
(forests-HDF5), a `Descendant` value below the `-1` null sentinel (e.g. `-2`,
both routes -- matches `read_ctrees_hdf5.c`'s `CT_ASSIGN_LINK`, which accepts
exactly `[-1, nhalos)`), overlapping/out-of-order `ForestInfo` offset rows
(**validated unconditionally, even with `--no-link-scan`** -- it is
O(n_forests), not the O(n_halos) work that flag is meant to skip), a missing
`Snap_num`/`Snap_idx` column, a missing `Descendant` dataset (raised as
`ConverterError` with file/field context, mirroring the snap-column
handling, rather than a bare `KeyError`), a non-integral `Snap_idx` value
(checked by an exact `floor(v) == v`, not a tolerant `np.allclose` that would
accept non-integral values at large magnitudes), and an out-of-range
snapshot value on **either** the integer or the float path (`< 0` or
`> INT_MAX`, matching `read_ctrees_hdf5.c`'s `CT_ASSIGN_SNAP_INT`/
`CT_ASSIGN_SNAP_DOUBLE` exactly, narrowed only by dropping that macro's
additional `<= LastSnapshotNr` term, which comes from the a_list this tool
does not load) -- all raise `ConverterError` and abort. A missing `h5py` is
reported as `MissingDependencyError` and propagates to a nonzero process
exit for `inspect`; for `survey`, a per-package exception (broadened to
cover the realistic numpy/h5py failure modes on untrusted input, not just
this tool's own declared error types) is recorded as that package's
`inspection_error` without discarding the other packages' already-gathered
results, and the overall process still exits nonzero if any package hit
one. A valid forward gap (mini-Millennium's 29,291 of them) is counted, not
rejected, confirmed by the exact-reproduction test above; likewise a
non-forward link (`Descendant` pointing to the same or an earlier snapshot)
is counted into `non_forward_or_zero_span`, not rejected -- exercised
directly on both routes this round, not just asserted as zero on
gap-containing real data.

`max_span` specifically means the maximum *forward-gap* span (the largest
value among spans > 1); it reads as 0 for a gap-free source with millions of
adjacent links, which is correct, not "no links were scanned."

A mistyped `--simulation-info`/`--a-list` path or a `--json` path into a
missing directory now also surfaces as the tool's own `error: ...` message
and a nonzero exit, not a raw Python traceback (`main()` catches
`ConverterError`, `MissingDependencyError`, `OSError`, `ValueError` and
`TypeError`).
