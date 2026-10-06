# mini-Millennium Simulation Package — Horizontal HDF5 (version 3)

This package declares the horizontal HDF5 on-disk record for the mini-Millennium halo catalog: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays. The on-disk contract is version 3 of [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#version-3) (`format_version = 3`); this package conforms to that specification, never the other way around.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size, and particle mass — identical to `simulations/mini-millennium/simulation_info.yaml`'s physical values
- `halo_properties.yaml`: the payload field contract — every `/halos` dataset the converter's `/schema` declares, with the same type, units and `h_convention` as `simulations/mini-millennium/converter_columns.yaml` selects, plus the five link roles declared `long long` (version 3 stores links as int64 snapshot-local indices) and checked against the v3 format's fixed table rather than `/schema`. `SourceHaloID`, the three target-snapshot columns and the `ForestIndex`/`HaloRankInForest` identity arrays are reader-owned arrays whose type the version 3 format table fixes, not the package, so they are deliberately not declared here
- `mini-millennium.a_list`: 64 snapshot scale factors, an exact copy of `simulations/mini-millennium/mini-millennium.a_list`
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, a copy of `simulations/mini-millennium/plot_profile.yaml` (same box) with the package name changed; `mimic-plot.py` discovers it from `simulations/<simulation.name>/`
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked; `snapshots/` itself is gitignored)

## Data provenance

The dataset is not primary data. It was produced offline by `convert/mimic-convert/convert_trees.py` from the same `trees_063.0`–`trees_063.7` L-Halo binary files that `simulations/mini-millennium/` reads, at converter commit `83e3781ba1a47eb8f1eb41e4bae78bf891e274c0`, using the `simulations/mini-millennium/converter_columns.yaml` profile (`column_mapping_sha256 5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1`). Links, offsets and remapping keys are carried at native converter precision (int64); payload fields keep their native L-Halo storage type and units, in particular `M_Crit200` as float32 in `1e10 Msun/h`. No `fix_flybys`-style demotion or spin renormalisation is applied — version 3 preserves the source's own topology and values exactly.

Cosmology, box size and particle mass match `simulations/mini-millennium/simulation_info.yaml` exactly: Ωm 0.25, ΩΛ 0.75, h 0.73, box 62.5 Mpc/h, particle mass 0.0860657 × 10¹⁰ Msun/h.

Converted from all eight locally present `trees_063.<N>` files (the package's complete inventory, `first_file: 0`–`last_file: 7` in `simulations/mini-millennium/simulation_info.yaml`): 1,533,122 halos across 64 snapshots and 29,585 forests, with 29,291 gapped `Descendant` links (longest span 2, `links_adjacent = 0`). The producer validation battery (`convert/mimic-convert/convert_trees.py validate`) passed all 20 checks.

## Regenerating the dataset

Run the converter phases in order against the vertical mini-Millennium package, emitting to a workdir outside the repository, then install the written snapshot files at the permanent destination:

```bash
S=simulations/mini-millennium
W=/path/to/scratch/convert-workdir-v3
D=/path/to/mini-millennium-horizontal

mimic_venv/bin/python convert/mimic-convert/convert_trees.py ingest --workdir "$W" --source-format lhalo_binary \
    --simulation-info "$S/simulation_info.yaml" --a-list "$S/mini-millennium.a_list" \
    --column-map "$S/converter_columns.yaml" --halo-properties "$S/halo_properties.yaml" \
    --source-dir "$S/snapshots" --tree-name trees_063 --first-file 0 --last-file 7
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
ln -s /path/to/mini-millennium-horizontal simulations/mini-millennium-horizontal/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_063.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the converted dataset.

## Running this package

Shipped run files pair this package with both `halos-only` and `sage16`, and `make generate`/`make validate-modules` pass for both:

```bash
make MODEL=halos-only SIMULATION=mini-millennium-horizontal
./mimic models/halos-only/input/halos-only_mini-millennium-horizontal.yaml
```

**This route is runnable and gated.** The dataset carries 29,291 gapped `Descendant` links (`links_adjacent = 0`, longest span 2). The horizontal driver keeps each generation until its descendants' snapshot has been processed, so this dataset holds at most three generations at once. Its package-local gate, `_tests/scientific/test_cross_format_identity.py`, shows horizontal output bitwise identical per `UniqueGalaxyID` to the vertical `lhalo_binary` reader over the same eight files, on all four `{halos-only, sage16} × {fixed, dynamic}` legs. The recorded gate of 2026-09-29 passed on real data with 29,291 gapped `Descendant` links: `halos-only` matched 292,163 galaxies over output snapshots 16, 18, 20, 23, 27, 32, 37 and 63 in all 20 fields under fixed and dynamic timesteps, and `sage16` matched 187,832 (fixed) and 187,817 (dynamic) galaxies in all 42 fields, every one bitwise identical per `UniqueGalaxyID` with no tolerance, compared by `scripts/compare_cross_format_identity.py`. Run it on a machine holding both datasets with `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific`.

## Test fixtures

Five committed version 3 fixtures live under `_tests/data/`, each converted by `convert/mimic-convert/convert_trees.py` from a synthetic L-Halo source written by `_tests/data/source/generate_sources.py` (with mini-Millennium's cosmology, box and particle mass) and rebuilt together by `_tests/data/regenerate.sh`:

- `worked_graph/` — one forest of five halos over five snapshots with gapped links, a FoF satellite and an empty snapshot; the generic test tiers run on it (`_tests/input/test_simulation.yaml`).
- `three_snapshot_chain/` — one progenitor chain spanning three snapshots.
- `adjacent/` — an all-adjacent dataset (`links_adjacent = 1`).
- `forest_blocks/` — six forests of unequal size (71 halos) over seven snapshots with snapshot 3 empty, gapped links, FoF satellites that merge and become orphans, and one forest holding 41% of the widest snapshot; the distributed identity gate (`make tests-distributed`) runs `halos-only`, `sage16`, `sham` and `hod` on it serially and under MPI through the `_tests/input/forest_blocks_<model>.yaml` run files, and adds its chunked legs (`input.forest_chunks` 2, 3 and 8, including a `hod` variant without the snapshot audit, `_tests/input/forest_blocks_hod_chunked.yaml`); `_tests/integration/test_chunked_sweep.py` (run by `make tests-horizontal-v3`) runs `halos-only` on it at `forest_chunks` 1, 2, 3 and 8, checks identity and partition row order against the `G = 1` run, and tests the multi-visit failure window on a broken copy.
- `wide_slab/` — one forest over two snapshots whose snapshot 0 holds 8,600 halos (one FoF group and one progenitor chain), wider than the reader's 8,192-row block, so `tests/unit/test_horizontal_v3_reader.c` reads ranges and scans `ForestIndex` across a block boundary.

The first three and `wide_slab` are used by `make tests-horizontal-v3`.

## Related packages

- `simulations/mini-millennium/` — the same halos in L-Halo binary, the conversion source and the cosmology reference
