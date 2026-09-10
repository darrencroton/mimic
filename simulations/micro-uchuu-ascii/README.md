# micro-Uchuu Simulation Package — Consistent-Trees ASCII

This package runs Mimic against the micro-Uchuu merger trees in Consistent-Trees ASCII format (`consistent_trees_ascii` reader). It is one of three micro-Uchuu packages that cover all three supported formats so the readers can be cross-validated against the same underlying halo catalog.

- `simulation_info.yaml`: tree input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: RawHalo field contract for the ctrees readers (see file header for the key unit difference from L-Halo binary: M_Crit200 in native Msun/h)
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951)
- `snapshots/`: symlink to the tree data directory
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults
- `_tests/`: integration test scaffolding (runs once the symlink and data are in place)

**Data files required in `snapshots/`:**

- `forests.list` — 561,266 forest/tree ids
- `locations.dat` — symlink to `MicroUchuu.locations.dat`
- `tree_0_0_0.dat` — symlink to `MicroUchuu.trees` (~11 GB monolithic ASCII tree)

**Setting up the snapshots symlink:**

```bash
ln -s /path/to/micro-uchuu-ascii simulations/micro-uchuu-ascii/snapshots
```

**Cross-validation siblings:**

- `simulations/micro-uchuu-hdf5/` — same halos via uchuutools forests-HDF5 (`consistent_trees_hdf5`)
- `simulations/micro-uchuu/` — same halos via L-Halo binary (`lhalo_binary`)

**Mirror maintenance:** `halo_properties.yaml` and `micro-uchuu.a_list` are intentional mirrors of the `micro-uchuu-hdf5` package (both use the ctrees RawHalo contract). Keep them in sync. The lhalo package uses a different field set (26-field L-Halo struct) and is not mirrored.

**Historical z=0 behaviour difference from lhalo and hdf5 formats (removed):**

Until it was deleted (`docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md`, decision D1), the Consistent-Trees ASCII reader called `fix_flybys()` (`src/io/tree/ctrees/ctrees_utils.c`) during forest topology reconstruction. At the final snapshot (snap49, z=0), a ctrees forest can contain multiple FoF groups — halos that share a forest because they had a close flyby interaction at some earlier time but never merged. `fix_flybys` picked the most massive z=0 FoF group as the sole central and demoted all others to satellites by modifying their host-pointer before `FirstHaloInFOFgroup` was computed, negating their `MostBoundID` as a flyby marker that propagated into the output HDF5.

This was found to be scientifically wrong, not a tolerable approximation, when the same mechanism collapsed 33% of the Shin-Uchuu z=0 population into one bogus FoF group and truncated the z=0 halo mass function by ~2 dex (addendum §3.2). `fix_flybys` was therefore deleted from the reader and the converter, `MostBoundID` is always positive, and this reader now agrees with the L-Halo binary and HDF5 readers on FoF grouping and Type classification at every snapshot, including snap49 (addendum §3.4, the A/B removal experiment).

In the micro-Uchuu dataset the demotion used to affect approximately 55,362 halos at snap49 (around 12.5% of forests contained at least one flyby FoF group), producing ~10–25% fewer Type 0 halos per mass bin there relative to lhalo/hdf5, a negated `MostBoundID` on the demoted halos (which fed into the SHAM model's RNG seed, so its stellar masses for those halos used to differ from the lhalo/hdf5 outputs), and one timestep of satellite rather than central sage16 physics for the demoted halos at z=0. None of that occurs any longer.

**Correction to an earlier claim in this file:** this README previously stated "all snapshots before snap49 are byte-identical across the three formats." That was never true of the `sage16` galaxy output — a pre-existing float32-ULP `Mvir` divergence between the ASCII and HDF5/L-Halo readers (unrelated to `fix_flybys`, present at every snapshot including well before snap49, and amplified chaotically by `sage16`) was found during the flyby investigation (addendum §3.4). It may have held at the raw halo level, or may never have been checked at the galaxy level. This divergence is real, is not fixed by removing `fix_flybys`, and remains open and unscoped by the flyby remediation; do not chase it here.
