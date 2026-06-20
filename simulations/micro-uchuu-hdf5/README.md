# micro-Uchuu Simulation Package — uchuutools forests-HDF5

This package runs Mimic against the micro-Uchuu merger trees in uchuutools forests-HDF5 format (`consistent_trees_hdf5` reader). It is one of three micro-Uchuu packages that cover all three supported formats so the readers can be cross-validated against the same underlying halo catalog.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: RawHalo field contract for the ctrees readers (see file header for the key unit difference from L-Halo binary: M_Crit200 in native Msun/h)
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951)
- `snapshots/`: symlink to the tree data directory — see `snapshots.txt` for the NT path and `ln -s` command
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults
- `_tests/`: integration test scaffolding (runs once the symlink and data are in place)

**Data files required in `snapshots/`:**

Both `MicroUchuu_mergertree_info.h5` (1.2 KB index) and `MicroUchuu_mergertree.h5` (13 GB data) must be present. The reader is pointed at the info file; HDF5 external links transparently resolve into the data file.

**Cross-validation siblings:**

- `simulations/micro-uchuu-ascii/` — same halos via Consistent-Trees ASCII (`consistent_trees_ascii`)
- `simulations/micro-uchuu-lhalo/` — same halos via L-Halo binary (`lhalo_binary`)

**Mirror maintenance:** `halo_properties.yaml` and `micro-uchuu.a_list` are intentional mirrors of the `micro-uchuu-ascii` package (both use the ctrees RawHalo contract). Keep them in sync. The lhalo package uses a different field set (26-field L-Halo struct) and is not mirrored.

**Known z=0 behaviour difference from the ascii format:**

The Consistent-Trees ASCII reader applies a `fix_flybys()` step that collapses multiple z=0 FoF groups within a ctrees forest into one, demoting the non-dominant ones from Type 0 (central) to Type 1 (satellite) and negating their `MostBoundID`. This reader reads `FirstHaloInFOFgroup` and `NextHaloInFOFgroup` directly from the pre-stored uchuutools columns and does not apply that fix, so flyby FoF groups appear as independent Type 0 centrals — consistent with the lhalo format. At snap49 (z=0) approximately 55,362 halos are therefore Type 1 in the ascii output but Type 0 here. All snapshots before snap49 are byte-identical between the three formats. See `simulations/micro-uchuu-ascii/README.md` and `docs/dev/CTREES-UCHUU-VALIDATION.md §5` for the full analysis.

See `docs/dev/CTREES-UCHUU-VALIDATION.md` for the full investigation report, format notes, and validation checklist.
