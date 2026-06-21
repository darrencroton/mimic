# micro-Uchuu Simulation Package — uchuutools forests-HDF5

This package runs Mimic against the micro-Uchuu merger trees in uchuutools forests-HDF5 format (`consistent_trees_hdf5` reader). It is the HDF5 cross-validation sibling of the L-Halo binary package (`simulations/micro-uchuu/`) — both cover the same halo catalog so the readers can be validated against each other.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: RawHalo field contract for the ctrees readers (see file header for the key unit difference from L-Halo binary: M_Crit200 in native Msun/h)
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951)
- `snapshots/`: symlink to your local copy of the tree data directory
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults
- `_tests/`: integration test scaffolding, including a tiny synthetic forests-HDF5 fixture for fast core and reader smoke tests

**Data files required in `snapshots/`:**

Both `MicroUchuu_mergertree_info.h5` (1.2 KB index) and `MicroUchuu_mergertree.h5` (13 GB data) must be present. The package sets `tree_name: MicroUchuu_mergertree_info.h5`; HDF5 external links transparently resolve into the data file.

**Cross-validation sibling:**

- `simulations/micro-uchuu/` — same halos via L-Halo binary (`lhalo_binary`)

**Mirror maintenance:** `halo_properties.yaml` uses the same ctrees RawHalo contract as the `simulations/uchuu/` package. When changing the ctrees HDF5 field schema, apply the same change there. The L-Halo package (`simulations/micro-uchuu/`) uses a different field set (26-field L-Halo struct) and is not mirrored here.

**Known z=0 behaviour difference from the ASCII format:**

The Consistent-Trees ASCII reader applies a `fix_flybys()` step that collapses multiple z=0 FoF groups within a ctrees forest into one, demoting the non-dominant ones from Type 0 (central) to Type 1 (satellite) and negating their `MostBoundID`. This reader reads `FirstHaloInFOFgroup` and `NextHaloInFOFgroup` directly from the pre-stored uchuutools columns and does not apply that fix, so flyby FoF groups appear as independent Type 0 centrals — consistent with the L-Halo format. At snap49 (z=0) approximately 55,362 halos are therefore Type 1 in ASCII output but Type 0 here. All snapshots before snap49 are byte-identical between L-Halo and HDF5 formats.

**Production-scale smoke:** default integration tests use the tiny fixture and do not touch the 13 GB production catalog. To smoke-test the mounted production files explicitly, build for this package and run `./mimic models/halos-only/input/halos-only_micro-uchuu-hdf5.yaml`.
