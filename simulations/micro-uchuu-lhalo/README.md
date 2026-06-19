# micro-Uchuu Simulation Package — L-Halo Binary

This package runs Mimic against the micro-Uchuu merger trees in L-Halo binary format (`lhalo_binary` reader). It is one of three micro-Uchuu packages that cover all three supported formats so the readers can be cross-validated against the same underlying halo catalog.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: 26-field L-Halo struct, identical layout to `simulations/mini-millennium/` (only cosmology and box size differ)
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951)
- `snapshots/`: symlink to the tree data directory — see `snapshots.txt` for the NT path and `ln -s` command
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults
- `_tests/`: integration test scaffolding (runs once the symlink and data are in place)

**Data files required in `snapshots/`:**

Four binary files (~2.2 GB total), converted from the Consistent-Trees ASCII trees by sage-model:
```
Uchuu100_Planck_lhalo_binary.0
Uchuu100_Planck_lhalo_binary.1
Uchuu100_Planck_lhalo_binary.2
Uchuu100_Planck_lhalo_binary.3
```

**Cross-validation siblings:**

- `simulations/micro-uchuu-hdf5/` — same halos via uchuutools forests-HDF5 (`consistent_trees_hdf5`)
- `simulations/micro-uchuu-ascii/` — same halos via Consistent-Trees ASCII (`consistent_trees_ascii`)

**Mirror maintenance:** `halo_properties.yaml` follows the `simulations/mini-millennium/` layout (same 26-field L-Halo struct). When changing the lhalo field schema, apply the same change to mini-millennium and the Millennium packages. The ctrees packages (hdf5, ascii) use a different, smaller RawHalo and are not mirrored here.

See `docs/dev/CTREES-UCHUU-VALIDATION.md` for the full investigation report, format notes, and validation checklist.
