# Millennium Simulation Package — Horizontal HDF5 (version 3)

This package declares the horizontal HDF5 on-disk record for the Millennium halo catalog, converted from its L-Halo binary files: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays. The on-disk contract is version 3 of [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#version-3) (`format_version = 3`); this package conforms to that specification, never the other way around. It is one package per (simulation, source format) because a version 3 file's `/schema` declares the source's native units and precision and the reader checks this package's `halo_properties.yaml` against it, so each source format needs its own package. Its source is `simulations/millennium/` (L-Halo binary, `lhalo_binary`), and its results are promised equal to that package's only.

**Coverage: the whole simulation.** The dataset converts all 512 files `simulations/millennium/simulation_info.yaml` declares, `trees_063.0`–`trees_063.511`, so every tree is present (every L-Halo tree is complete within one file). The shipped vertical run file `halos-only_millennium.yaml` reads only files 0–15 so that it stays quick to run, so the package's parity gate runs a scratch copy of it with the input range set to 0–511 and nothing else changed, and checks that both runs record that range.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size and particle mass — identical to `simulations/millennium/simulation_info.yaml`'s physical values. `first_file`/`last_file` (0–511) declare the full simulation extent, as the vertical package does; the horizontal reader ignores them and derives its file set from the snapshot list, and `mimic-plot` uses the extent as the denominator of the volume fraction (the run file's `input.first_file`/`last_file` give the numerator, the files processed)
- `halo_properties.yaml`: the payload field contract — every `/halos` dataset the converter's `/schema` declares for this route, with the same type, units and `h_convention` — in particular `M_Crit200` as float `1e10 Msun/h`, plus the five link roles declared `long long` (version 3 stores links as int64 snapshot-local indices) and checked against the v3 format's fixed table rather than `/schema`. `SourceHaloID`, the three target-snapshot columns and the `ForestIndex`/`HaloRankInForest` identity arrays are reader-owned arrays whose type the version 3 format table fixes, not the package, so they are deliberately not declared here. Declaration order mirrors the vertical package's, because it fixes the output record's field order
- `millennium.a_list`: 64 snapshot scale factors, an exact copy of `simulations/millennium/millennium.a_list`
- `_tests/integration/test_schema_conformance.py`: checks the compiled declarations against the converter's own `/schema` derivation for this route (no data needed) and against the real dataset's `/schema` when present
- `_tests/scientific/test_cross_format_identity.py`: the parity gate (below)
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, a copy of `simulations/millennium/plot_profile.yaml` (same box) with the package name changed; `mimic-plot.py` discovers it from `simulations/<simulation.name>/`
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked; `snapshots/` itself is gitignored)

## Data provenance

The dataset is not primary data. It was produced offline by `convert/mimic-convert/convert_trees.py` at converter commit `cc434c716b2c880d9af5895381f160d113306f9e` on 2026-10-01 from `trees_063.0`–`.511`, all 512 files, the same files that `simulations/millennium/` reads, using that package's converter profile (`column_mapping_sha256 5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1`). Links, offsets and remapping keys are carried at native converter precision (int64); payload fields keep their native L-Halo binary storage type and units, in particular `M_Crit200` as float32 in `1e10 Msun/h`.

Converted: 760,667,000 halos across 64 snapshots (59 populated) and 14,329,882 forests, with 15,026,757 gapped `Descendant` links (longest span 2, `links_adjacent = 0`), every one preserved as a gap. The largest snapshot holds 18,619,466 halos and the largest `HaloRankInForest` is 514,193. The producer validation battery passed, and the dataset occupies 107.1 GB (140.8 B/halo). These figures are the whole simulation's.

Cosmology, box size and particle mass match `simulations/millennium/simulation_info.yaml` exactly.

## Regenerating the dataset

Run the converter phases in order against the vertical package, emitting to a workdir outside the repository, then install the written snapshot files at the permanent destination. The converter's default `--memory-budget-mb` of 2048 is refused for this inventory (its per-tree inventory term alone is larger), so pass a larger budget on `ingest`; the value is recorded in the workdir and `transpose` reuses it.

```bash
C="mimic_venv/bin/python convert/mimic-convert/convert_trees.py"
S=simulations/millennium
W=/path/to/scratch/convert-workdir
D=/path/to/millennium-horizontal

$C ingest --workdir "$W" --source-format lhalo_binary --memory-budget-mb 16384 \
    --simulation-info "$S/simulation_info.yaml" --a-list "$S/millennium.a_list" \
    --column-map "$S/converter_columns.yaml" \
    --halo-properties "$S/halo_properties.yaml" --source-dir "$S/snapshots" \
    --tree-name trees_063 --first-file 0 --last-file 511
$C transpose --workdir "$W"
$C write --workdir "$W" --simulation-info "$S/simulation_info.yaml"
$C report --workdir "$W" --memory-budget-mb 8192 --spill-dir /path/to/scratch/spill

mkdir -p "$D"
cp "$W"/write/attempt_*/snapshot_*.h5 "$W"/write/attempt_*/forests.h5 "$D"/
```

`report` runs the producer validation battery itself and writes `conversion_report.{json,txt}`, so a separate `validate` would repeat it. Do not browse the workdir in Finder while the conversion runs: macOS writes `.DS_Store` files into the directories the converter checks, and the converter refuses a directory that holds anything it did not write.

Measured on the 2026-10-01 conversion (Apple M3 Ultra, 512 GB): `ingest` 26 min, `transpose` 59 min, `write` 9 min and `report` 17 min, with peak RSS 7.7, 23.3, 7.2 and 9.5 GiB. The ingest chunks (152 B/halo), the transposed snapshots (140 B/halo) and the dataset (107.1 GB) made the workdir 307 GB after `write`; `transpose` spilled up to 355 GB more and the report's validation battery 147 GB, so size the volume for the sum.

`convert/mimic-convert/README.md` documents the workdir layout, resume semantics, memory budgeting and independent comparison tooling.

## Setting up the snapshots symlink

```bash
ln -s /path/to/millennium-horizontal simulations/millennium-horizontal/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_063.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the converted dataset.

## Running this package

Shipped run files pair this package with `halos-only` and `sage16`; `make generate` and `make validate-modules` pass for the `halos-only` pairing, and the package's parity gate covers `halos-only` only. The run files declare `input.first_file: 0` and `last_file: 511`, the files the dataset covers; the horizontal reader ignores them, and they set the range the output records and the files-processed numerator of `mimic-plot`'s volume fraction.

```bash
make MODEL=halos-only SIMULATION=millennium-horizontal
./mimic models/halos-only/input/halos-only_millennium-horizontal.yaml
```

## Parity gate

`_tests/scientific/test_cross_format_identity.py` builds `halos-only` for `millennium` and for this package in isolated worktrees at HEAD, runs both over files 0–511 (the vertical side through a scratch copy of `halos-only_millennium.yaml` that overrides its shipped 0–15 range to 0–511) under fixed and dynamic timesteps, and requires per-`UniqueGalaxyID` bitwise identity of every output field at every output snapshot, compared by `scripts/compare_cross_format_identity.py`. It fails, rather than skipping, when either dataset is absent or is not the pinned conversion, and it needs the scratch space its `required_free_bytes` pins. It is registered only when this package is selected:

```bash
make MODEL=halos-only SIMULATION=millennium-horizontal tests-scientific
```

It is `halos-only` evidence for this route against its own source format only: no identity with any other micro-Uchuu, Millennium or Uchuu packaging is claimed, and no `sage16` parity is claimed.

## Related packages

- `simulations/millennium/` — the same halos in L-Halo binary, the conversion source and the cosmology reference
- `simulations/mini-millennium-horizontal/` — the complete mini-Millennium version 3 package, the reference gapped route, gated under both `halos-only` and `sage16`
