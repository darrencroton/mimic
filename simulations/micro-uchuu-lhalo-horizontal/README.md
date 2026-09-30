# micro-Uchuu (from L-Halo binary) Simulation Package — Horizontal HDF5 (version 3)

This package declares the horizontal HDF5 on-disk record for the micro-Uchuu halo catalog converted from its L-Halo binary packaging: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays. The on-disk contract is version 3 of [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#version-3) (`format_version = 3`); this package conforms to that specification, never the other way around. It is one package per (simulation, source format) because a version 3 file's `/schema` declares the source's native units and precision and the reader checks this package's `halo_properties.yaml` against it, so each source format needs its own package. Its source is `simulations/micro-uchuu/` (L-Halo binary, `lhalo_binary`), and its results are promised equal to that package's only.

**Evidence: complete real data.** The dataset is a conversion of all four `Uchuu100_Planck_lhalo_binary` files, the vertical package's complete inventory, and the package's parity gate compares it against that package's own `lhalo_binary` reader under `halos-only`.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size and particle mass — identical to `simulations/micro-uchuu/simulation_info.yaml`'s physical values. `first_file`/`last_file` (0–3) are metadata naming the converted source files; the horizontal reader derives its file set from the snapshot list
- `halo_properties.yaml`: the payload field contract — every `/halos` dataset the converter's `/schema` declares for this route, with the same type, units and `h_convention` — in particular `M_Crit200` as float `1e10 Msun/h`, plus the five link roles declared `long long` (version 3 stores links as int64 snapshot-local indices) and checked against the v3 format's fixed table rather than `/schema`. `SourceHaloID`, the three target-snapshot columns and the `ForestIndex`/`HaloRankInForest` identity arrays are reader-owned arrays whose type the version 3 format table fixes, not the package, so they are deliberately not declared here. Declaration order mirrors the vertical package's, because it fixes the output record's field order
- `micro-uchuu.a_list`: 50 snapshot scale factors, an exact copy of `simulations/micro-uchuu/micro-uchuu.a_list`
- `_tests/integration/test_schema_conformance.py`: checks the compiled declarations against the converter's own `/schema` derivation for this route (no data needed) and against the real dataset's `/schema` when present
- `_tests/scientific/test_cross_format_identity.py`: the parity gate (below)
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked; `snapshots/` itself is gitignored)

## Data provenance

The dataset is not primary data. It was produced offline by `convert/mimic-convert/convert_trees.py` at converter commit `4c9518d3e7ad8c942b95155a5e37ebbb4ff4131b` from the same `Uchuu100_Planck_lhalo_binary.0`–`.3` files that `simulations/micro-uchuu/` reads, using that package's converter profile (`column_mapping_sha256 5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1`). Links, offsets and remapping keys are carried at native converter precision (int64); payload fields keep their native L-Halo binary storage type and units, in particular `M_Crit200` as float32 in `1e10 Msun/h`.

Converted: 22,580,924 halos across 50 snapshots and 440,651 forests, with no gapped links (`links_adjacent = 1`). The producer validation battery (`convert/mimic-convert/convert_trees.py validate`) passed.

Cosmology, box size and particle mass match `simulations/micro-uchuu/simulation_info.yaml` exactly.

## Regenerating the dataset

Run the converter phases in order against the vertical package, emitting to a workdir outside the repository, then install the written snapshot files at the permanent destination:

```bash
S=simulations/micro-uchuu
W=/path/to/scratch/convert-workdir
D=/path/to/micro-uchuu-lhalo-horizontal

mimic_venv/bin/python convert/mimic-convert/convert_trees.py ingest --workdir "$W" --source-format lhalo_binary \
    --simulation-info "$S/simulation_info.yaml" --a-list "$S/micro-uchuu.a_list" \
    --column-map "$S/converter_columns.yaml" \
    --halo-properties "$S/halo_properties.yaml" --source-dir "$S/snapshots" \
    --tree-name Uchuu100_Planck_lhalo_binary --first-file 0 --last-file 3
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
ln -s /path/to/micro-uchuu-lhalo-horizontal simulations/micro-uchuu-lhalo-horizontal/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_049.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the converted dataset.

## Running this package

A shipped run file pairs this package with `halos-only`; `make generate` and `make validate-modules` pass for it. There is no `sage16` run file for this package.

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-lhalo-horizontal
./mimic models/halos-only/input/halos-only_micro-uchuu-lhalo-horizontal.yaml
```

## Parity gate

`_tests/scientific/test_cross_format_identity.py` builds `halos-only` for `micro-uchuu` and for this package in isolated worktrees at HEAD, runs both over the same four files under fixed and dynamic timesteps, and requires per-`UniqueGalaxyID` bitwise identity of every output field at every output snapshot, compared by `scripts/compare_cross_format_identity.py`. It fails, rather than skipping, when either dataset is absent or is not the pinned conversion. It is registered only when this package is selected:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-lhalo-horizontal tests-scientific
```

The recorded gate of 2026-09-29 passed: 4,409,643 galaxies over output snapshots 7, 8, 10, 12, 16, 23, 28 and 49, bitwise identical per `UniqueGalaxyID` in all 20 fields with no tolerance, under fixed and dynamic timesteps, on the complete L-Halo files 0–3. It is `halos-only` evidence for this route against its own source format only: no identity with any other micro-Uchuu, Millennium or Uchuu packaging is claimed, and no `sage16` parity is claimed.

## Related packages

- `simulations/micro-uchuu/` — the same halos in L-Halo binary, the conversion source and the cosmology reference
- `simulations/micro-uchuu-hdf5-horizontal/` — the same simulation converted from forests-HDF5; its `M_Crit200` is in `Msun/h`, and no identity with this package is claimed
- `simulations/micro-uchuu-horizontal/` — the version 2 conversion from Consistent-Trees ASCII
