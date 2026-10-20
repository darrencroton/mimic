# Shin-Uchuu Simulation Package — Horizontal HDF5 (version 3)

This package declares the horizontal HDF5 on-disk record, `format_version 3`, for the Shin-Uchuu halo catalog: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays, plus the `forests.h5` provenance sidecar. Its production dataset is the **cut** version 3 dataset (every forest partitioned into its z = 0 FoF groups; see [How the production dataset was made](#how-the-production-dataset-was-made)). The on-disk contract is in [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md); this package conforms to that specification, never the other way around.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: the RawHalo field contract — every `/halos` dataset of format version 3, with names and types matching the specification exactly. Deliberately omits `ForestIndex` and `HaloRankInForest` (see file header), mirroring `micro-uchuu-ascii-horizontal`.
- `shin-uchuu.a_list`: 70 snapshot scale factors (a=0.04773 to a=0.99998)
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults for the halo plots (spatial distribution over the 140 Mpc/h box; halo mass function limits from `models/sage16/plots/profiles/shin-uchuu_plot_profile.yaml`); `mimic-plot.py` discovers it from `simulations/<simulation.name>/`
- `snapshots/`: symlink to the production (cut version 3) dataset directory (machine-local, not tracked)
- `_tests/`: a synthetic contract fixture, the schema-conformance test and the subset gate (see "Maintenance notes")

This package's dataset is not primary data: it is produced offline from the full Shin-Uchuu Consistent-Trees ASCII catalog (source: the Shin-Uchuu Consistent-Trees ASCII merger trees as held on the OzSTAR system, 2744 `tree_*.dat` files, 11.61 TB, 315,004,242 z=0 halos, 70 snapshots). The ASCII catalog was converted once to version 2 by the legacy converter (since removed), applying the reference reader's value conventions (spin normalisation, `Len` derivation, `fix_upid`) and rewriting global-id links as snapshot-local indices; that dataset was then migrated to version 3 (by the rewriter's `migrate` operation, since retired) and cut by `convert/mimic-convert/rewrite_trees.py cut`, neither step reading ASCII. The route available from the repository today is `convert_trees.py ingest --cut-table <cut table>`, which converts the catalogue directly into its cut form (see the [converter README](../../convert/mimic-convert/README.md#rewriting-a-horizontal-dataset-with-rewrite_treespy)); its equality with the rewriter's cut is shown on the 2,744-file subset and on micro-Uchuu only, not on the full catalogue. `MostBoundID` is always positive. Cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774) is the Uchuu/Planck-2015 family; particle mass (8.97×10⁵ Msun/h) and box size (140 Mpc/h) are specific to Shin-Uchuu.

## Setting up the snapshots symlink

```bash
ln -s /path/to/shin-uchuu-horizontal simulations/shin-uchuu/snapshots
```

`snapshots/` is machine-local and gitignored (`.gitignore` matches `simulations/*/snapshots`); it must hold `snapshot_000.h5` … `snapshot_069.h5` and `forests.h5`.

## Running this package

Runnable end to end through the horizontal driver (`run_horizontal_driver()`), with shipped run files pairing it with both `halos-only` and `sage16`:

```bash
make MODEL=halos-only SIMULATION=shin-uchuu
./mimic models/halos-only/input/halos-only_shin-uchuu.yaml
```

Horizontal runs are HDF5-only and do not support `--skip`; both are rejected at configuration time. The production dataset is forest-blocked version 3, so a multi-rank configuration (`NTask > 1`) and a chunked one (`input.forest_chunks` above 1) are both accepted by the driver. The shipped run files set neither (one task, one chunk), which holds the widest snapshot of 519,342,987 halos in memory; for a small-memory host set `input.forest_chunks`, as the laptop-class proxy measurement below did (128 chunks on one task). The shipped run files write output snapshots 69, 48, 44, 37, 32, 30, 27 and 24; the measured runs below used the output snapshots 69, 52, 43, 40, 20, 10, 5, 2, 1 and 0 (a set that differs from the shipped files', so a shipped run's galaxy totals are not those quoted here). See [`docs/USER-GUIDE.md`](../../docs/USER-GUIDE.md) → "Running Horizontal Input" and [`docs/DEVELOPER-GUIDE.md`](../../docs/DEVELOPER-GUIDE.md) → "The Horizontal Driver".

## Maintenance notes

- **`unique_galaxy_id_multiplier: 20000000000` (2×10¹⁰)** must match `simulations/shin-uchuu-ascii/`, or `UniqueGalaxyID` diverges between the two packages. Confirmed against the uncut forests' measured `max_halo_rank_in_forest` ≈ 1.265×10¹⁰, the bound the shared multiplier must cover (the cut dataset's is 126,380,325).
- **`Spin` range `[-1000, 1000]`.** Measured over the full production dataset: max `|Spin|` = 416.69, zero non-finite.
- **`deltaMvir` range `[-1000000.0, 1000000.0]`** (declared in `src/core/core_properties.yaml`, not this package; widened after a measured 4.77e4 on mini-Uchuu). Measured over the version 2 `sage16` production run: max `|deltaMvir|` = 12,432.
- **`_tests/` ships a synthetic contract fixture, a schema-conformance test and the subset gate.** `_tests/data/worked_ascii/` holds the committed version 3 fixture (three forests holding four trees in all, over six snapshots, with its a_list) and `_tests/input/test_simulation.yaml` points the generic test tiers at it. `_tests/data/regenerate.sh` rebuilds it: it calls `micro-uchuu-ascii-horizontal/_tests/data/source/convert_worked_ascii.sh`, which generates the synthetic Consistent-Trees ASCII source of `generate_source.py` at this package's last six scale factors into a temporary directory and converts it with `convert_trees.py` under the `shin-uchuu-ascii` profile and metadata, runs the producer validation battery on the result, installs it under `worked_ascii/` and fails if the installed `snapshot_*.h5` files are not exactly the converted set. `_tests/integration/test_schema_conformance.py` checks the declarations against the converter's version 3 `/schema` and, when `snapshots/` holds a dataset, against its files. `_tests/scientific/test_cross_format_identity.py` is the subset gate (see "The subset gate" below).

### Largest forest in the widest slab

In the version 2 dataset and in its uncut version 3 migration, the widest slab is snapshot 34, which holds 519,342,987 halos, and the percolation super-forest (source forest id 26551468179 in the Consistent-Trees catalogue) holds 321,253,424 of them, 61.86%. A forest is never split across tasks or chunks, so that share was a floor: no `NTask` and no `input.forest_chunks` could put fewer of the widest slab's rows (about 321 million halos) in one process. The cut removes the floor: in the production dataset the largest forest is that same forest, keeping `ForestIndex` 0, with 126,380,326 halos in all (558,306 trees), peaking at 3,415,844 rows at snapshot 31.

## How the production dataset was made

The dataset under `snapshots/` is the **cut version 3 dataset**: 22,503,649,037 halos in 70 snapshot files plus the `forests.h5` sidecar (71 files, 3,156,784,461,544 B, 140.28 B per halo), 244,953,607 forests, `links_adjacent 1`, identity multiplier 2×10¹⁰. It comes from the legacy version 2 conversion by two operations of `convert/mimic-convert/rewrite_trees.py`, a migration to version 3 (`migrate`, since retired) and a cut (`cut`, still offered), neither of which reads the ASCII catalogue.

### The version 2 conversion (history)

The first production dataset was made by the legacy version 2 converter (`convert_ctrees.py`, since removed) from the full 2,744-file Consistent-Trees ASCII catalogue under the production `forests.list` (md5 `60dbf14a99ae9f23ca7c334474477a95`) and `locations.dat` (md5 `75442d7ae5b1f2465b485e60720e6df7`). The dataset was 71 files of 2,251,984,563,372 B holding 22,503,649,037 halos in 166,547,771 forests, with its largest snapshot, 34, at 519,342,987 halos. Its rows were sorted by `MostBoundID`, so it was not forest-blocked and the horizontal driver refused to distribute or chunk it.

### The migration: the uncut version 3 dataset

| | |
|---|---|
| Tool and commit | the rewriter's `migrate` operation (since retired), commit `b4903283994b066c811cabf83ae2c2ea1ed39a9c` |
| Inputs | the version 2 dataset and its conversion manifest; the production `forests.list` and `locations.dat` (md5s above); the `shin-uchuu-ascii` package's `simulation_info.yaml`, a_list and column map; multiplier 2×10¹⁰ |
| Result | 71 files, 3,154,903,285,150 B; 22,503,649,037 halos, 315,004,242 trees, 166,547,771 forests, `max_halo_rank_in_forest` 12,646,607,900 |
| Checks | root correspondence (315,004,242 index roots, 315,004,242 terminal roots) and per-tree conservation against the version 2 manifest (2,744 files, 0 disagreeing) pass; the bounded battery (`convert_trees.py validate --bounded`) gives `validation: PASS`, 20 of 20 checks |

`sage16` on the uncut dataset (one task, `forest_chunks: 2`, the version 2 run's output snapshots 69, 52, 43, 40, 20, 10, 5, 2, 1 and 0) is bitwise identical to the recorded version 2 run: 2,058,518,068 galaxies over 10 output snapshots in all 42 fields, with identical `UniqueGalaxyID` sets, and every z = 0 science value (313,766,518 galaxies, 244,953,607 centrals) equals the version 2 reference's.

### The cut: the production dataset

| | |
|---|---|
| Tool and commit | `rewrite_trees.py cut` of the uncut dataset, commit `c4a73f70`; same index files and multiplier |
| Cut table | `forests.list` md5 `6e9f2fb03b26a36ce51c5392e7bcf164` (its record md5 `432b157965cebf9420993e62d074377d`) |
| Rule | every forest partitioned into its z = 0 FoF groups |
| Pieces | 166,547,771 forests become 244,953,607; 8,053,562 forests are split into 86,459,398 pieces, 78,405,836 of them fresh; the super-forest keeps `ForestIndex` 0 with 126,380,326 halos (558,306 trees), its next two pieces holding 71,150,152 and 55,080,340; `max_halo_rank_in_forest` falls to 126,380,325 |
| Promotions | 481,296,108 halos become centrals, none at snapshot 69; 155,039,609 groups lose members; the progenitor order changes for 254,554 descendants |
| Checks | the bounded battery gives `validation: PASS`, 20 of 20 checks, and the census's per-snapshot predictions (promotions and progenitor-order changes) equal the cut's in every snapshot |

**The rule.** Each tree joins the forest of its terminal halo's z = 0 FoF group, with no free parameter. This is exactly the partition that cuts every co-membership (a pair of trees, one holding a FoF member of a group centred in the other) that ended before the final snapshot and keeps every one present at it. "Ended" is an operational definition fixed by the dataset's final snapshot, not a claim that an encounter was an artificial join or a physical flyby. **The structural invariant** is that every z = 0 FoF group of the input is preserved exactly (every final-snapshot central/member relation, by `MostBoundID`: 315,004,242 halos and 244,953,607 centrals in both datasets, 0 relations differing); no halo is promoted at z = 0 and no tree is split. A forest's largest piece keeps its id and `ForestIndex`; every other piece gets a fresh id above the catalogue's maximum, in descending size.

**`sage16` against the uncut run.** Compared through the catalogue-key comparator, which pairs tree galaxies by (`MostBoundID`, `SnapNum`), 2,058,426,737 galaxies pair over 10 output snapshots, and every galaxy outside the split forests is identical in all 42 fields, bitwise except `UniqueGalaxyID`, the pairing key, with `UniqueCentralGalaxyID` compared through the pairing. Inside the 8,053,562 split forests the differences are by design, reported and not gated: of their records, 1,338,523,829 are identical, 180,067,108 differ, 91,331 are only in the uncut run and 1,120,641 only in the cut run, and 7,791,093 of those forests hold a difference. At z = 0, 160,691,061 galaxies are in unchanged forests and identical; of 153,015,126 paired galaxies in split forests, 76,093,915 differ. The fields differing most are `ColdGas` (144,137,859 records), `HotGas` (89,961,604) and `Cooling` (87,609,902); `Type`, `Mvir`, `UniqueCentralGalaxyID` and `Vvir` each differ in about 29.8 million records, because the promoted halos become FoF centrals.

**Science checks at z = 0.** The cut run keeps every central and its halo mass function is identical in every bin; the satellites and baryon totals change, only inside the split forests. No numerical gate is set on these totals.

| z = 0 (snapshot 69) | Uncut | Cut | Cut − uncut |
|---|---|---|---|
| Galaxies | 313,766,518 | 313,753,033 | −13,485 (−0.004%) |
| Type 0 (centrals) | 244,953,607 | 244,953,607 | 0 |
| Type 1 | 68,812,911 | 68,799,426 | −13,485 |
| Global baryon fraction | 0.123655 | 0.123676 | +0.017% |
| Stellar mass (10¹⁰ Msun/h, summed) | 252,945.75 | 253,619.37 | +0.27% |
| Cold gas | 102,958.12 | 102,231.07 | −0.71% |
| Hot gas | 1,065,197.50 | 1,064,341.97 | −0.08% |
| Ejected gas | 191,823.20 | 193,140.09 | +0.69% |
| Intracluster stars | 102,497.79 | 102,383.33 | −0.11% |
| Black holes | 102.910 | 105.223 | +2.25% |

The mean baryon fraction by halo mass differs in all 42 bins, by at most 0.091% (relative), with the same halo count in every bin. No identity of `sham` or `hod` is claimed on the cut dataset: `sham`, and `hod` with its audit, need `forest_chunks: 1`, no production run of either was made on it, and at subset scale their output differs inside split forests, measured and reported, not gated.

### Running it chunked, and the laptop-class proxy

`sage16` on the cut dataset at one task and `input.forest_chunks: 128`, with the version 2 run's output snapshots, completed and peaked at **6,170,116,096 B (6.170 GB) RSS**, 0.575 of the 10 GiB (10,737,418,240 B) usable budget of a 16 GiB laptop class; this is a **proxy**: a Mac measurement under `MallocLargeCache=0`, which becomes the final laptop acceptance only when a laptop of the 16 GiB class runs it.

At subset scale (the cut version of the 2,744-file subset: 7,206,195 forests, widest slab 9,006,294 halos), serial, `forest_chunks: 8` and `mpirun -np 4` runs of `halos-only` and `sage16` write bitwise identical galaxies, and chunking in 8 lowers the peak RSS (measured under `MallocLargeCache=0`) from 9.012 to 1.503 GB (`halos-only`) and from 9.712 to 1.777 GB (`sage16`). No unchunked or distributed run of the cut production dataset was made, so that identity is measured at subset scale only.

### The subset gate

`_tests/scientific/test_cross_format_identity.py` compares the vertical `shin-uchuu-ascii` reader over the 2,744-file subset (406,668,896 halos, 6,011,205 forests) with the horizontal reader over a version 3 conversion of exactly that subset, for `halos-only` and `sage16`, each with fixed and dynamic timesteps. It runs only when `MIMIC_SHIN_UCHUU_SUBSET_DATASET` names the subset's dataset (and `MIMIC_SHIN_UCHUU_SUBSET_MANIFEST` the subset's `convert_trees.py` conversion manifest, whose source dependencies name the 2,744 tree files), because this package's `snapshots/` holds the production dataset. On the subset migrated from its version 2 conversion all four legs pass bitwise in every field: `halos-only` 92,413,007 galaxies over 8 output snapshots in all 20 fields, fixed and dynamic; `sage16` 60,016,178 (fixed) and 60,016,233 (dynamic) galaxies in all 42 fields. The gate covers the migration at subset scale; it excludes the production super-forest and says nothing about the cut. The gate is a plain `ParityGate` over `tests/framework/parity_gate.py`, which threads the named dataset through every stage; with the dataset unset the tier still runs a data-free check that the pins name the subset's dataset and that each committed `shin-uchuu` run file's dataset variant differs from its vertical counterpart in exactly the authorized keys, and skips the comparison with its reason.

## Related packages

- `simulations/shin-uchuu-ascii/` — a subset of the same catalog in Consistent-Trees ASCII, read by the vertical driver
- `simulations/shin-uchuu-lhalo/` — this package's cut dataset written as L-Halo binary trees by `convert/mimic-convert/write_vertical_trees.py` and read by the vertical driver; its gate compares it with this package over the cut subset, and a vertical run there assigns the `UniqueGalaxyID`s this package assigns
- `simulations/micro-uchuu-ascii-horizontal/` — the worked exemplar this package's structure and conventions mirror
