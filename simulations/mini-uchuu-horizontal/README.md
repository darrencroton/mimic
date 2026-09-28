# mini-Uchuu (sampled files 0–15) Simulation Package — Horizontal HDF5 (version 3)

This package declares the horizontal HDF5 on-disk record for a sampled subset of the mini-Uchuu halo catalog, converted from its L-Halo binary files: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays. The on-disk contract is the draft frozen in [`docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md`](../../docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md) at `format_version = 3`; this package conforms to that specification, never the other way around. It is one package per (simulation, source format), per Gate R0-10 of [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](../../docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md): its source is `simulations/mini-uchuu/` (L-Halo binary, `lhalo_binary`), and its results are promised equal to that package's only.

**Evidence: a sampled subset.** The dataset is a conversion of `Uchuu400_Planck_lhalo_binary.0`–`.15` only: 16 of the 128 files `simulations/mini-uchuu/simulation_info.yaml` declares, and every file present on the machine that made it. Every L-Halo tree is complete within one file, so every tree in the sample is complete, but **this package is not the whole mini-Uchuu simulation** and no whole-simulation conversion or runtime claim is made for it. Whole-simulation evidence needs the owner to supply files 16–127. The package's parity gate compares the subset against the vertical `lhalo_binary` reader run over the identical file range, files 0–15. The shipped `halos-only_mini-uchuu.yaml` reads only files 0–3, so the gate runs a scratch copy of it with `input.last_file` set to 15 and nothing else changed, and it checks that both runs record the range 0–15.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size and particle mass — identical to `simulations/mini-uchuu/simulation_info.yaml`'s physical values. `first_file`/`last_file` (0–15, the sampled subset) are metadata naming the converted source files; the horizontal reader derives its file set from the snapshot list
- `halo_properties.yaml`: the payload field contract — every `/halos` dataset the converter's `/schema` declares for this route, with the same type, units and `h_convention` — in particular `M_Crit200` as float `1e10 Msun/h`, plus the five link roles declared `long long` (Gate R0-2(a)) and checked against the v3 format's fixed table rather than `/schema`. `SourceHaloID`, the three target-snapshot columns and the `ForestIndex`/`HaloRankInForest` identity arrays are reader-owned format-table arrays and are deliberately not declared here (Gate R0-3(a) and the identity-array precedent). Declaration order mirrors the vertical package's, because it fixes the output record's field order
- `mini-uchuu.a_list`: 50 snapshot scale factors, an exact copy of `simulations/mini-uchuu/mini-uchuu.a_list`
- `_tests/integration/test_schema_conformance.py`: checks the compiled declarations against the converter's own `/schema` derivation for this route (no data needed) and against the real dataset's `/schema` when present
- `_tests/scientific/test_cross_format_identity.py`: the parity gate (below)
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked; `snapshots/` itself is gitignored)

## Data provenance

The dataset is not primary data. It was produced offline by `scripts/convert/convert_trees.py` at converter commit `4c9518d3e7ad8c942b95155a5e37ebbb4ff4131b` from `Uchuu400_Planck_lhalo_binary.0`–`.15`, the same files that `simulations/mini-uchuu/` reads, using that package's converter profile (`column_mapping_sha256 5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1`). Links, offsets and remapping keys are carried at native converter precision (int64); payload fields keep their native L-Halo binary storage type and units, in particular `M_Crit200` as float32 in `1e10 Msun/h`.

Converted: 181,188,125 halos across 50 snapshots and 3,230,400 forests, with no gapped links (`links_adjacent = 1`); the largest snapshot holds 4,970,910 halos. The producer validation battery (`scripts/convert/convert_trees.py validate`) passed. These figures describe files 0–15 only.

Cosmology, box size and particle mass match `simulations/mini-uchuu/simulation_info.yaml` exactly.

## Regenerating the dataset

Run the converter phases in order against the vertical package, emitting to a workdir outside the repository, then install the written snapshot files at the permanent destination:

```bash
S=simulations/mini-uchuu
W=/path/to/scratch/convert-workdir
D=/path/to/mini-uchuu-horizontal

mimic_venv/bin/python scripts/convert/convert_trees.py ingest --workdir "$W" --source-format lhalo_binary \
    --simulation-info "$S/simulation_info.yaml" --a-list "$S/mini-uchuu.a_list" \
    --column-map "$S/converter_columns.yaml" \
    --halo-properties "$S/halo_properties.yaml" --source-dir "$S/snapshots" \
    --tree-name Uchuu400_Planck_lhalo_binary --first-file 0 --last-file 15
mimic_venv/bin/python scripts/convert/convert_trees.py transpose --workdir "$W"
mimic_venv/bin/python scripts/convert/convert_trees.py write --workdir "$W" --simulation-info "$S/simulation_info.yaml"
mimic_venv/bin/python scripts/convert/convert_trees.py validate --workdir "$W"
mimic_venv/bin/python scripts/convert/convert_trees.py report --workdir "$W"

mkdir -p "$D"
cp "$W"/write/attempt_*/snapshot_*.h5 "$W"/write/attempt_*/forests.h5 "$D"/
```

`scripts/convert/README.md` documents the workdir layout, resume semantics, memory budgeting and independent comparison tooling.

## Setting up the snapshots symlink

```bash
ln -s /path/to/mini-uchuu-horizontal simulations/mini-uchuu-horizontal/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_049.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the converted dataset.

## Running this package

A shipped run file pairs this package with `halos-only`; `make generate` and `make validate-modules` pass for it. There is no `sage16` run file for this package.

```bash
make MODEL=halos-only SIMULATION=mini-uchuu-horizontal
./mimic models/halos-only/input/halos-only_mini-uchuu-horizontal.yaml
```

## Parity gate

`_tests/scientific/test_cross_format_identity.py` builds `halos-only` for `mini-uchuu` and for this package in isolated worktrees at HEAD, runs both over files 0–15 (the vertical side through a scratch copy of `halos-only_mini-uchuu.yaml` overriding its shipped 0–3 range to 0–15) under fixed and dynamic timesteps, and requires per-`UniqueGalaxyID` bitwise identity of every output field at every output snapshot, compared by `scripts/compare_cross_format_identity.py`. It fails, rather than skipping, when either dataset is absent or is not the pinned conversion. It is registered only when this package is selected:

```bash
make MODEL=halos-only SIMULATION=mini-uchuu-horizontal tests-scientific
```

The recorded result is in [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md`](../../docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md). It is `halos-only` evidence for this route against its own source format only: no identity with any other micro-Uchuu, Millennium or Uchuu packaging is claimed, and no `sage16` parity is claimed.

## Related packages

- `simulations/mini-uchuu/` — the same halos in L-Halo binary, the conversion source and the cosmology reference
- `simulations/micro-uchuu-lhalo-horizontal/` — the complete micro-Uchuu L-Halo version 3 package
