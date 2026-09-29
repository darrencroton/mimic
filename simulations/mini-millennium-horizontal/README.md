# mini-Millennium Simulation Package — Horizontal HDF5 (version 3)

This package declares the horizontal HDF5 on-disk record for the mini-Millennium halo catalog: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays. The on-disk contract is the draft frozen in [`docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md`](../../docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md) at `format_version = 3`; this package conforms to that specification, never the other way around.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size, and particle mass — identical to `simulations/mini-millennium/simulation_info.yaml`'s physical values
- `halo_properties.yaml`: the payload field contract — every `/halos` dataset the converter's `/schema` declares, with the same type, units and `h_convention` as `simulations/mini-millennium/converter_columns.yaml` selects, plus the five link roles declared `long long` (Gate R0-2(a)) and checked against the v3 format's fixed table rather than `/schema`. `SourceHaloID`, the three target-snapshot columns and the `ForestIndex`/`HaloRankInForest` identity arrays are reader-owned format-table arrays and are deliberately not declared here (Gate R0-3(a) and the identity-array precedent)
- `mini-millennium.a_list`: 64 snapshot scale factors, an exact copy of `simulations/mini-millennium/mini-millennium.a_list`
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked; `snapshots/` itself is gitignored)

## Data provenance

The dataset is not primary data. It was produced offline by `scripts/convert/convert_trees.py` from the same `trees_063.0`–`trees_063.7` L-Halo binary files that `simulations/mini-millennium/` reads, at converter commit `83e3781ba1a47eb8f1eb41e4bae78bf891e274c0` (the runtime plan's Slice 6 fresh conversion, which replaced Slice 3's first conversion at `209087ee`; both used the same profile and digest), using the `simulations/mini-millennium/converter_columns.yaml` profile (`column_mapping_sha256 5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1`). Links, offsets and remapping keys are carried at native converter precision (int64); payload fields keep their native L-Halo storage type and units, in particular `M_Crit200` as float32 in `1e10 Msun/h`. No `fix_flybys`-style demotion or spin renormalisation is applied — version 3 preserves the source's own topology and values exactly.

Cosmology, box size and particle mass match `simulations/mini-millennium/simulation_info.yaml` exactly: Ωm 0.25, ΩΛ 0.75, h 0.73, box 62.5 Mpc/h, particle mass 0.0860657 × 10¹⁰ Msun/h.

Converted from all eight locally present `trees_063.<N>` files (the package's complete inventory, `first_file: 0`–`last_file: 7` in `simulations/mini-millennium/simulation_info.yaml`): 1,533,122 halos across 64 snapshots and 29,585 forests, with 29,291 gapped `Descendant` links (longest span 2, `links_adjacent = 0`). The producer validation battery (`scripts/convert/convert_trees.py validate`) passed all 20 checks.

## Regenerating the dataset

Run the converter phases in order against the vertical mini-Millennium package, emitting to a workdir outside the repository, then install the written snapshot files at the permanent destination:

```bash
S=simulations/mini-millennium
W=/path/to/scratch/convert-workdir-v3
D=/path/to/mini-millennium-horizontal

mimic_venv/bin/python scripts/convert/convert_trees.py ingest --workdir "$W" --source-format lhalo_binary \
    --simulation-info "$S/simulation_info.yaml" --a-list "$S/mini-millennium.a_list" \
    --column-map "$S/converter_columns.yaml" --halo-properties "$S/halo_properties.yaml" \
    --source-dir "$S/snapshots" --tree-name trees_063 --first-file 0 --last-file 7
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
ln -s /path/to/mini-millennium-horizontal simulations/mini-millennium-horizontal/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_063.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the converted dataset.

## Running this package

Shipped run files pair this package with both `halos-only` and `sage16`, and `make generate`/`make validate-modules` pass for both:

```bash
make MODEL=halos-only SIMULATION=mini-millennium-horizontal
./mimic models/halos-only/input/halos-only_mini-millennium-horizontal.yaml
```

**This route is runnable and gated.** The dataset carries 29,291 gapped `Descendant` links (`links_adjacent = 0`, longest span 2). The horizontal driver keeps each generation until its descendants' snapshot has been processed, so this dataset holds at most three generations at once. Its package-local gate, `_tests/scientific/test_cross_format_identity.py`, shows horizontal output bitwise identical per `UniqueGalaxyID` to the vertical `lhalo_binary` reader over the same eight files, on all four `{halos-only, sage16} × {fixed, dynamic}` legs ([`MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md`](../../docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md) §1–§8). Run it on a machine holding both datasets with `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific`.

## Related packages

- `simulations/mini-millennium/` — the same halos in L-Halo binary, the conversion source and the cosmology reference
