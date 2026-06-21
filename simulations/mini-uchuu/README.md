# mini-Uchuu Simulation Package — L-Halo Binary

This package runs Mimic against the mini-Uchuu merger trees in L-Halo binary format (`lhalo_binary` reader). mini-Uchuu is the intermediate-box tier of the Uchuu suite: 400 Mpc/h box, same particle mass and cosmology as micro-Uchuu, ~33.5 million forests.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: 26-field L-Halo struct, identical layout to `simulations/micro-uchuu/` and `simulations/mini-millennium/` (only cosmology, box size, and position range differ)
- `mini-uchuu.a_list`: 50 snapshot scale factors (a=0.066964 to a=0.999887)
- `snapshots/`: symlink to your local copy of the tree data directory
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults
- `_tests/`: integration test scaffolding, including a tiny synthetic L-Halo binary fixture for fast core and reader smoke tests

**Data files required in `snapshots/`:**

128 binary files (~92 GB total), converted from the Consistent-Trees ASCII trees by sage-model:
```
Uchuu400_Planck_lhalo_binary.0
Uchuu400_Planck_lhalo_binary.1
...
Uchuu400_Planck_lhalo_binary.127
```

**Mirror maintenance:** `halo_properties.yaml` follows the `simulations/micro-uchuu/` and `simulations/mini-millennium/` layout (same 26-field L-Halo struct). When changing the lhalo field schema, apply the same change to those packages. Only the `Pos` range differs between them.

**Preferred format for this tier:** L-Halo binary is the preferred mini-Uchuu production format (92 GB, compact and validated format). No forests-HDF5 packaging exists for mini-Uchuu; if needed it must be generated from the Consistent-Trees ASCII trees.

**Production-scale smoke:** default integration tests use the tiny fixture and do not touch the 92 GB production catalog. To smoke-test the mounted production files explicitly, build for this package and run `./mimic models/halos-only/input/halos-only_mini-uchuu.yaml`.
