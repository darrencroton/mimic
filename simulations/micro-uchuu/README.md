# micro-Uchuu Simulation Package — L-Halo Binary

This package runs Mimic against the micro-Uchuu merger trees in L-Halo binary format (`lhalo_binary` reader). It is one of three micro-Uchuu packages that cover all three supported formats so the readers can be cross-validated against the same underlying halo catalog.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: 26-field L-Halo struct, identical layout to `simulations/mini-millennium/` and `simulations/mini-uchuu/` (only cosmology, box size, and position range differ)
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951)
- `snapshots/`: symlink to your local copy of the tree data directory
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults
- `_tests/`: integration test scaffolding, including a tiny synthetic L-Halo binary fixture for fast core and reader smoke tests

**Data files required in `snapshots/`:**

Four binary files (~2.2 GB total), converted from the Consistent-Trees ASCII trees by sage-model:
```
Uchuu100_Planck_lhalo_binary.0
Uchuu100_Planck_lhalo_binary.1
Uchuu100_Planck_lhalo_binary.2
Uchuu100_Planck_lhalo_binary.3
```

**Cross-validation siblings:**

- `simulations/micro-uchuu-hdf5/` — same halos via uchuutools forests-HDF5 (`consistent_trees_hdf5`; kept for cross-format validation)

**Mirror maintenance:** `halo_properties.yaml` follows the `simulations/mini-millennium/` and `simulations/mini-uchuu/` layout (same 26-field L-Halo struct). When changing the lhalo field schema, apply the same change to mini-millennium, mini-uchuu, and the Millennium packages. The ctrees packages use a different, smaller RawHalo and are not mirrored here.

**Known z=0 behaviour difference from the ascii format:**

The Consistent-Trees ASCII reader applies a `fix_flybys()` step that collapses multiple z=0 FoF groups within a ctrees forest into one, demoting the non-dominant ones from Type 0 (central) to Type 1 (satellite) and negating their `MostBoundID`. This reader does not do that — the L-Halo binary was produced per ctrees tree (one L-Halo tree per z=0 FoF root), so flyby FoF groups naturally appear as independent Type 0 centrals. At snap49 (z=0) approximately 55,362 halos are therefore Type 1 in the HDF5/ASCII ctrees output but Type 0 here. All snapshots before snap49 are byte-identical between the L-Halo and ctrees-HDF5 formats.

**Production-scale smoke:** default integration tests use the tiny fixture and do not touch the 2.2 GB production catalog. To smoke-test the mounted production files explicitly, build for this package and run `./mimic models/halos-only/input/halos-only_micro-uchuu.yaml`.
