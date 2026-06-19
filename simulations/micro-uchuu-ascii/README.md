# micro-Uchuu Simulation Package — Consistent-Trees ASCII

This package runs Mimic against the micro-Uchuu merger trees in Consistent-Trees ASCII format (`consistent_trees_ascii` reader). It is one of three micro-Uchuu packages that cover all three supported formats so the readers can be cross-validated against the same underlying halo catalog.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: RawHalo field contract for the ctrees readers (see file header for the key unit difference from L-Halo binary: M_Crit200 in native Msun/h)
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951)
- `snapshots/`: symlink to the tree data directory — see `snapshots.txt` for the NT path and `ln -s` command
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults
- `_tests/`: integration test scaffolding (runs once the symlink and data are in place)

**Data files required in `snapshots/`:**

- `forests.list` — 440,651 forest/tree ids
- `locations.dat` — symlink to `MicroUchuu.locations.dat`
- `tree_0_0_0.dat` — symlink to `MicroUchuu.trees` (~44 GB monolithic ASCII tree)

**Cross-validation siblings:**

- `simulations/micro-uchuu-hdf5/` — same halos via uchuutools forests-HDF5 (`consistent_trees_hdf5`)
- `simulations/micro-uchuu-lhalo/` — same halos via L-Halo binary (`lhalo_binary`)

**Mirror maintenance:** `halo_properties.yaml` and `micro-uchuu.a_list` are intentional mirrors of the `micro-uchuu-hdf5` package (both use the ctrees RawHalo contract). Keep them in sync. The lhalo package uses a different field set (26-field L-Halo struct) and is not mirrored.

See `docs/dev/CTREES-UCHUU-VALIDATION.md` for the full investigation report, format notes, and validation checklist.
