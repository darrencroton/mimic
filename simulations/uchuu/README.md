# Full Uchuu Simulation Package — uchuutools forests-HDF5

This package runs Mimic against the full Uchuu merger trees in uchuutools forests-HDF5 format (`consistent_trees_hdf5` reader). Full Uchuu is the largest tier of the Uchuu suite: 2000 Mpc/h box, ~3.22 billion forests, ~181.5 billion halos across all 50 snapshots.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: RawHalo field contract for the ctrees HDF5 reader (M_Crit200 in native Msun/h; see file header for unit conventions and the float64 Snap_idx note)
- `uchuu.a_list`: 50 snapshot scale factors (a=0.06686 to a=0.99998)
- `snapshots/`: symlink to your local copy of the tree data directory
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults
- `_tests/`: integration test scaffolding, including a tiny synthetic ExternalLink forests-HDF5 fixture for fast core and reader smoke tests

**Data files required in `snapshots/`:**

2001 HDF5 files (37 TB total):
```
mergertree_info.h5          (110 KB index file — set as tree_name)
mergertree_0.h5             (~44 GB, File0: 1.53M forests)
mergertree_1.h5
...
mergertree_1999.h5
```

All files must remain co-located. Each `File<n>` group in `mergertree_info.h5` is an HDF5 ExternalLink to the root of `mergertree_N.h5`; the HDF5 library resolves the link transparently using relative paths.

**HDF5 external link architecture:** This dataset uses ExternalLinks (not VDS virtual datasets as in `simulations/micro-uchuu-hdf5/`). The package-local fixture exercises this layout: `mergertree_info.h5` links `File0` to `/` of `mergertree_0.h5`, with contiguous halo properties under `Forests/` and `Snap_idx` stored as `float64`. Run an explicit halos-only smoke test before any production use so the mounted 37 TB catalog is checked in place.

**MPI requirements:** 3.22 billion forests at the 1M forest/task galaxy-id limit requires at minimum ~3,220 MPI tasks. The `forest_distribution_scheme: linear` setting weights forests by halo count for better MPI load balance across the highly unequal forest sizes.

**Mirror maintenance:** `halo_properties.yaml` follows the `simulations/micro-uchuu-hdf5/` RawHalo contract. When changing the ctrees HDF5 field schema, apply the same change there. Only the `Pos` range differs between them.

See the `_tests/` fixtures for the ExternalLink HDF5 layout exercised by the reader smoke tests.

**Production-scale smoke:** default integration tests use the tiny ExternalLink fixture and do not touch the 37 TB production catalog. To smoke-test the mounted production files explicitly, build for this package and run `./mimic models/halos-only/input/halos-only_uchuu.yaml`; production-scale processing requires the full 37 TB catalog mounted under `snapshots/` and sufficient MPI resources for a full run.
