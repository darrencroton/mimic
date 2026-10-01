# micro-Uchuu (from forests-HDF5) Simulation Package — Horizontal HDF5 (version 3)

This package declares the horizontal HDF5 on-disk record for the micro-Uchuu halo catalog converted from its uchuutools forests-HDF5 packaging: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays. The on-disk contract is version 3 of [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#version-3) (`format_version = 3`); this package conforms to that specification, never the other way around. It is one package per (simulation, source format) because a version 3 file's `/schema` declares the source's native units and precision and the reader checks this package's `halo_properties.yaml` against it, so each source format needs its own package. Its source is `simulations/micro-uchuu-hdf5/` (forests-HDF5, `consistent_trees_hdf5`), and its results are promised equal to that package's only.

**Evidence: complete real data.** The dataset is a conversion of the whole `MicroUchuu_mergertree` forests-HDF5 catalogue (its single `FileN` group, files 0–0), the vertical package's complete inventory, and the package's parity gate compares it against that package's own `consistent_trees_hdf5` reader under `halos-only`.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size and particle mass — identical to `simulations/micro-uchuu-hdf5/simulation_info.yaml`'s physical values. `first_file`/`last_file` (0–0) are metadata naming the converted source files; the horizontal reader derives its file set from the snapshot list
- `halo_properties.yaml`: the payload field contract — every `/halos` dataset the converter's `/schema` declares for this route, with the same type, units and `h_convention` — in particular `M_Crit200` as float `Msun/h`, the native Consistent-Trees `Mvir`, **not** the L-Halo `1e10 Msun/h`, plus the five link roles declared `long long` (version 3 stores links as int64 snapshot-local indices) and checked against the v3 format's fixed table rather than `/schema`. `SourceHaloID`, the three target-snapshot columns and the `ForestIndex`/`HaloRankInForest` identity arrays are reader-owned arrays whose type the version 3 format table fixes, not the package, so they are deliberately not declared here. Declaration order mirrors the vertical package's, because it fixes the output record's field order
- `micro-uchuu.a_list`: 50 snapshot scale factors, an exact copy of `simulations/micro-uchuu-hdf5/micro-uchuu.a_list`
- `_tests/integration/test_schema_conformance.py`: checks the compiled declarations against the converter's own `/schema` derivation for this route (no data needed) and against the real dataset's `/schema` when present
- `_tests/scientific/test_cross_format_identity.py`: the parity gate (below)
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, a copy of `simulations/micro-uchuu-hdf5/plot_profile.yaml` (same box) with the package name changed; `mimic-plot.py` discovers it from `simulations/<simulation.name>/`
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked; `snapshots/` itself is gitignored)

## Data provenance

The dataset is not primary data. It was produced offline by `convert/mimic-convert/convert_trees.py` at converter commit `4c9518d3e7ad8c942b95155a5e37ebbb4ff4131b` from the same `MicroUchuu_mergertree_info.h5` (ExternalLink into `MicroUchuu_mergertree.h5`) that `simulations/micro-uchuu-hdf5/` reads, using that package's converter profile (`column_mapping_sha256 241f277cf059378077c19c16c4584ff171a8f2b9fba55e395a7808e6033bcbce`). Links, offsets and remapping keys are carried at native converter precision (int64); payload fields keep their native forests-HDF5 storage type and units, in particular `M_Crit200` as float32 Consistent-Trees `Mvir` in `Msun/h`; `Len` is derived as `round(Mvir * 1e-10 / particle_mass)` and `Spin` as J/Mvir, exactly as the vertical reader derives them.

Converted: 22,580,924 halos across 50 snapshots and 440,651 forests, with no gapped links (`links_adjacent = 1`). The producer validation battery (`convert/mimic-convert/convert_trees.py validate`) passed.

Cosmology, box size and particle mass match `simulations/micro-uchuu-hdf5/simulation_info.yaml` exactly.

## Regenerating the dataset

Run the converter phases in order against the vertical package, emitting to a workdir outside the repository, then install the written snapshot files at the permanent destination:

```bash
S=simulations/micro-uchuu-hdf5
W=/path/to/scratch/convert-workdir
D=/path/to/micro-uchuu-hdf5-horizontal

mimic_venv/bin/python convert/mimic-convert/convert_trees.py ingest --workdir "$W" --source-format consistent_trees_hdf5 \
    --simulation-info "$S/simulation_info.yaml" --a-list "$S/micro-uchuu.a_list" \
    --column-map "$S/converter_columns.yaml" \
    --info-file "$S/snapshots/MicroUchuu_mergertree_info.h5" --first-file 0 --last-file 0
mimic_venv/bin/python convert/mimic-convert/convert_trees.py transpose --workdir "$W"
mimic_venv/bin/python convert/mimic-convert/convert_trees.py write --workdir "$W" --simulation-info "$S/simulation_info.yaml"
mimic_venv/bin/python convert/mimic-convert/convert_trees.py validate --workdir "$W"
mimic_venv/bin/python convert/mimic-convert/convert_trees.py report --workdir "$W"

mkdir -p "$D"
cp "$W"/write/attempt_*/snapshot_*.h5 "$W"/write/attempt_*/forests.h5 "$D"/
```

`convert/mimic-convert/README.md` documents the workdir layout, resume semantics, memory budgeting and independent comparison tooling.

## Setting up the snapshots symlink

```bash
ln -s /path/to/micro-uchuu-hdf5-horizontal simulations/micro-uchuu-hdf5-horizontal/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_049.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the converted dataset.

## Running this package

Shipped run files pair this package with `halos-only` and `sage16`; `make generate` and `make validate-modules` pass for the `halos-only` pairing, and this route is validated for `halos-only` only.

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-hdf5-horizontal
./mimic models/halos-only/input/halos-only_micro-uchuu-hdf5-horizontal.yaml
```

## Parity gate

`_tests/scientific/test_cross_format_identity.py` builds `halos-only` for `micro-uchuu-hdf5` and for this package in isolated worktrees at HEAD, runs both over the same catalogue under fixed and dynamic timesteps, and requires per-`UniqueGalaxyID` bitwise identity of every output field at every output snapshot, compared by `scripts/compare_cross_format_identity.py`. It fails, rather than skipping, when either dataset is absent or is not the pinned conversion. It is registered only when this package is selected:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-hdf5-horizontal tests-scientific
```

The recorded gate of 2026-09-29 passed: 4,409,643 galaxies over output snapshots 7, 8, 10, 12, 16, 23, 28 and 49, bitwise identical per `UniqueGalaxyID` in all 20 fields with no tolerance, under fixed and dynamic timesteps, on the complete forests-HDF5 catalogue. The counts match the L-Halo route's, but no comparison between the two micro-Uchuu version 3 packages was made; their units differ (`M_Crit200` in `Msun/h` here, `1e10 Msun/h` there). It is `halos-only` evidence for this route against its own source format only: no identity with any other micro-Uchuu, Millennium or Uchuu packaging is claimed, and no `sage16` parity is claimed.

## Related packages

- `simulations/micro-uchuu-hdf5/` — the same halos in forests-HDF5, the conversion source and the cosmology reference
- `simulations/micro-uchuu-horizontal/` — the same simulation converted from L-Halo binary; its `M_Crit200` is in `1e10 Msun/h`, and no identity with this package is claimed
- `simulations/micro-uchuu-ascii-horizontal/` — the version 2 conversion from Consistent-Trees ASCII
