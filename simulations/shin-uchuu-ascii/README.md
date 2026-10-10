# Shin-Uchuu Simulation Package — Consistent-Trees ASCII (subset)

This package runs Mimic against a subset of the Shin-Uchuu merger trees in Consistent-Trees ASCII format (`consistent_trees_ascii` reader). It is permanently subset-only: the full box contains one percolating "super-forest" of 104.8 million tree roots (33% of all trees) that the vertical driver cannot hold in memory as a single unit, so the full box can never run vertical. This package's subset excludes that forest.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: RawHalo field contract for the ctrees readers (mirrors `micro-uchuu-ascii`'s ctrees bridge contract; see file header for the key unit difference from L-Halo binary — M_Crit200 in native Msun/h)
- `shin-uchuu.a_list`: 70 snapshot scale factors (a=0.04773 to a=0.99998), extracted from the halo data rather than ctrees file headers
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults for the halo plots (spatial distribution over the 140 Mpc/h box; halo mass function limits from `models/sage16/plots/profiles/shin-uchuu_plot_profile.yaml`); `mimic-plot.py` discovers it from `simulations/<simulation.name>/`
- `snapshots/`: symlink to the subset ASCII tree data directory
- `_tests/`: not present (see "Maintenance notes")

The subset holds 8,000,198 tree roots in 6,011,205 whole forests (2.54% of the full box's 315,004,242 z=0 halos; 406,668,896 halos measured over the converted dataset) — a fixed-seed random sample of tractable forests plus the top 20 forests by measured root `Mvir` within the sampled pool.

**Data files required in `snapshots/`:**

- `forests.list` — subset forest/tree-root ids (one line per selected tree root)
- `locations.dat` — file id, byte offset, and filename for each selected tree root
- `tree_0_0_0.dat` … `tree_13_13_13.dat` — the full 2744-file production layout is preserved; every file contributes at least one selected tree, so `tree_0_0_0.dat` (declared as `input.tree_name`, used only for the shared Consistent-Trees column header) is guaranteed present

**Setting up the snapshots symlink:**

```bash
ln -s /path/to/shin-uchuu-subset-ascii simulations/shin-uchuu-ascii/snapshots
```

`snapshots/` is machine-local and gitignored (`.gitignore` matches `simulations/*/snapshots`).

## Converting the subset to horizontal HDF5

The subset converts with either converter in `convert/mimic-convert/` (see its [README](../../convert/mimic-convert/README.md)). Give the 2,744 tree files in `locations.dat`'s FileID order, which is the natural (`sort -V`) order of the file names, not their plain lexical order. The order given becomes the forest sidecar's `SourceFileOrdinal`.

Measured on the Mac Studio, source and workdir on one external disk, with the `shin-uchuu-ascii` converter profile, multiplier 2×10¹⁰ and `--memory-budget-mb 8192`. Peak RSS is from `/usr/bin/time -l` with `MallocLargeCache=0`.

| Conversion | Wall-clock | Peak RSS | Disk |
|---|---|---|---|
| `convert_trees.py` to version 3: `inspect`, `ingest`, `transpose`, `write`, `validate`, `report` | 11,460 s (ingest 7,542 s) | 9.99 GB (ingest) | dataset 57.8 GB (142.1 B per halo); the volume's usage rose by at most 387 GB, the workdir's intermediates plus the battery's 76 GB spill; workdir 311 GB after `report` without consumption |
| `convert_ctrees.py` to version 2: `scatter` to `report` | 16,102 s (scatter 13,131 s, I/O-bound at about 16 MB/s) | 8.98 GB (links) | workdir 41.4 GB with consumptive deletion |
| `rewrite_trees.py migrate`, version 2 to 3 | 449 s | 2.09 GB | dataset 57.4 GB, scratch peak 1.72 GB |

Both version 3 datasets pass the producer battery (20 of 20 checks) and agree in every `/halos` column, header attribute, `/schema` attribute and sidecar value. In the version 3 sidecar, 12,694 of the 6,011,205 forests span files and carry −1/−1 ordinals; every other forest carries its file's ordinal and its unit ordinal in that file.

## Data provenance

Source: `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` on OzSTAR — 2744 `tree_*.dat` files, 11.61 TB, Consistent-Trees ASCII format, 70 snapshots. Cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774) is the Uchuu/Planck-2015 family shared with `micro-uchuu-ascii`; particle mass (8.97×10⁵ Msun/h) and box size (140 Mpc/h) are specific to Shin-Uchuu. The subset was extracted with `convert/mimic-convert/subset.py`, which selects whole forests without reading the bulk tree data, then copies only the selected byte ranges. The same subset was the rehearsal-scale dataset for the production conversion; how that conversion was run is in [`shin-uchuu/README.md`](../shin-uchuu/README.md#how-the-production-dataset-was-made).

## Cross-format sibling

- `simulations/shin-uchuu/` — the horizontal HDF5 conversion of the full production catalog, produced by `convert/mimic-convert/` using this reader's value conventions as the reference.

**Mirror maintenance:** `halo_properties.yaml` is an intentional mirror of `simulations/shin-uchuu/halo_properties.yaml` (both use the ctrees RawHalo contract, adjusted only for the 140 Mpc/h box's `Pos` range). Keep them in sync.

## Maintenance notes

- **`MostBoundID` is always positive.** A forest's final snapshot can legitimately hold many independent FoF centrals; `verify_fof_centrals_present()` aborts if it has zero.
- **`Spin` range `[-1000, 1000]`.** Measured over this subset's 406,668,896 halos: `[-11.673591, +17.797567]`, zero non-finite.
- **`unique_galaxy_id_multiplier: 20000000000` (2×10¹⁰)** must match `simulations/shin-uchuu/`, or `UniqueGalaxyID` diverges between the two packages.
- **`plot_profile.yaml` is minimal.** It carries the box-size axis limits and the halo mass function limits only; galaxy-plot limits live in the model-level profiles (`models/sage16/plots/profiles/shin-uchuu-ascii_plot_profile.yaml`) and belong to a real run's dynamic range.
- **No `_tests/`.** No committed fixture-sized test data or integration scaffolding yet.
