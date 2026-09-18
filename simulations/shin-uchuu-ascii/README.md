# Shin-Uchuu Simulation Package — Consistent-Trees ASCII (subset)

This package runs Mimic against a subset of the Shin-Uchuu merger trees in Consistent-Trees ASCII format (`consistent_trees_ascii` reader). It is permanently subset-only: the full box contains one percolating "super-forest" of 104.8 million tree roots (33% of all trees) that the tree-ordered driver cannot hold in memory as a single unit, so the full box can never run tree-ordered. This package's subset excludes that forest.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: RawHalo field contract for the ctrees readers (mirrors `micro-uchuu-ascii`'s ctrees bridge contract; see file header for the key unit difference from L-Halo binary — M_Crit200 in native Msun/h)
- `shin-uchuu.a_list`: 70 snapshot scale factors (a=0.04773 to a=0.99998), extracted from the halo data rather than ctrees file headers
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

## Data provenance

Source: `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` on OzSTAR — 2744 `tree_*.dat` files, 11.61 TB, Consistent-Trees ASCII format, 70 snapshots. Cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774) is the Uchuu/Planck-2015 family shared with `micro-uchuu-ascii`; particle mass (8.97×10⁵ Msun/h) and box size (140 Mpc/h) are specific to Shin-Uchuu. The subset was extracted with `scripts/convert/subset.py`, which selects whole forests without reading the bulk tree data, then copies only the selected byte ranges.

## Cross-format sibling

- `simulations/shin-uchuu/` — the snapshot-ordered HDF5 conversion of the full production catalog, produced by `scripts/convert/` using this reader's value conventions as the reference.

**Mirror maintenance:** `halo_properties.yaml` is an intentional mirror of `simulations/shin-uchuu/halo_properties.yaml` (both use the ctrees RawHalo contract, adjusted only for the 140 Mpc/h box's `Pos` range). Keep them in sync.

## Maintenance notes

- **`MostBoundID` is always positive.** A forest's final snapshot can legitimately hold many independent FoF centrals; `verify_fof_centrals_present()` aborts if it has zero.
- **`Spin` range `[-1000, 1000]`.** Measured over this subset's 406,668,896 halos: `[-11.673591, +17.797567]`, zero non-finite.
- **`unique_galaxy_id_multiplier: 20000000000` (2×10¹⁰)** must match `simulations/shin-uchuu/`, or `UniqueGalaxyID` diverges between the two packages.
- **No `plot_profile.yaml`.** Axis limits belong to a real run's dynamic range; add one from measured output when plotting this package matters.
- **No `_tests/`.** No committed fixture-sized test data or integration scaffolding yet.
