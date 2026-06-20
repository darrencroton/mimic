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

**Known z=0 behaviour difference from lhalo and hdf5 formats:**

The Consistent-Trees ASCII reader calls `fix_flybys()` (`src/io/tree/ctrees/ctrees_utils.c`) during forest topology reconstruction. At the final snapshot (snap49, z=0), a ctrees forest can contain multiple FoF groups — halos that share a forest because they had a close flyby interaction at some earlier time but never merged. `fix_flybys` picks the most massive z=0 FoF group as the sole central and demotes all others to satellites by modifying their host-pointer before `FirstHaloInFOFgroup` is computed. It also negates their `MostBoundID` as a flyby marker, which propagates into the output HDF5.

In the micro-Uchuu dataset this affects approximately 55,362 halos at snap49 (around 12.5% of forests contain at least one flyby FoF group). The effect is strictly confined to snap49: at all earlier snapshots, each flyby halo's progenitor chain carries its original FoF-central topology and all three readers produce identical output.

The L-Halo binary and HDF5 readers do not apply this fix. The L-Halo binary was produced by sage-model per ctrees tree (one L-Halo tree per z=0 FoF root), so flyby halos naturally appear as Type 0 centrals. The HDF5 reader reads pre-stored uchuutools topology columns directly and also preserves them as centrals.

Practical consequences:
- **Halo mass function at z=0:** this reader produces ~10–25% fewer Type 0 halos per mass bin at snap49 relative to lhalo/hdf5. The total halo count and shape are correct; the normalization reflects the flyby reclassification.
- **MostBoundID:** the 55,362 demoted halos carry a negated `MostBoundID` in the output. This does not affect `UniqueGalaxyID` or `UniqueCentralGalaxyID` (neither depends on MostBoundID), and sage16 modules do not use MostBoundID. The SHAM model uses MostBoundID in its RNG seed, so SHAM stellar masses for flyby halos will differ from the lhalo/hdf5 outputs.
- **sage16 physics:** flyby halos at z=0 pass through one timestep of satellite physics (Type 1 code path) instead of central physics. Their progenitor histories at all earlier snapshots are unaffected. This matches sage-model's own ctrees ASCII behaviour.
- **Cross-format comparison:** restrict snap49 comparisons to total halo counts (all types) or use snap48. All snapshots before snap49 are byte-identical across the three formats.

See `docs/dev/CTREES-UCHUU-VALIDATION.md §5` for the full analysis and numbers.
