# micro-Uchuu (from Consistent-Trees ASCII) Simulation Package — Horizontal HDF5 (version 3)

This package declares the horizontal HDF5 on-disk record for the micro-Uchuu halo catalog converted from its Consistent-Trees ASCII packaging: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays, plus the `forests.h5` provenance sidecar. The on-disk contract is version 3 of [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md#version-3) (`format_version = 3`); this package conforms to that specification, never the other way around. It is one package per (simulation, source format) because a version 3 file's `/schema` declares the source's native units and precision and the reader checks this package's `halo_properties.yaml` against it, so each source format needs its own package. Its source is `simulations/micro-uchuu-ascii/` (Consistent-Trees ASCII, `consistent_trees_ascii`), and its results are promised equal to that package's only.

**Evidence: complete real data.** The dataset is a conversion of the whole micro-Uchuu Consistent-Trees ASCII catalogue (`tree_0_0_0.dat` with its `forests.list` and `locations.dat`), the vertical package's complete inventory, and the package's parity gate compares it against that package's own `consistent_trees_ascii` reader under `halos-only` and `sage16`.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size and particle mass — identical to `simulations/micro-uchuu-ascii/simulation_info.yaml`'s physical values. `first_file`/`last_file` (0–0) are metadata; the horizontal reader derives its file set from the snapshot list
- `halo_properties.yaml`: the payload field contract — every `/halos` dataset the converter's `/schema` declares for this route, with the same type, units and `h_convention` — in particular `M_Crit200` as float `Msun/h`, the native Consistent-Trees `Mvir`, **not** the L-Halo `1e10 Msun/h`, plus the five link roles declared `long long` (version 3 stores links as int64 snapshot-local indices) and checked against the version 3 format's fixed table rather than `/schema`. `SourceHaloID`, the three target-snapshot columns and the `ForestIndex`/`HaloRankInForest` identity arrays are reader-owned arrays whose type the version 3 format table fixes, not the package, so they are deliberately not declared here. Declaration order mirrors the vertical package's, because it fixes the output record's field order
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951), an exact copy of the `micro-uchuu-ascii` list
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, a copy of `simulations/micro-uchuu-ascii/plot_profile.yaml` (same box) with the package name changed; `mimic-plot.py` discovers it from `simulations/<simulation.name>/`
- `_tests/integration/test_schema_conformance.py`: checks the compiled declarations against the converter's own `/schema` derivation for this route (no data needed) and against the real dataset's `/schema` when present
- `_tests/scientific/test_cross_format_identity.py`: the parity gate (below)
- `_tests/unit/test_unit_horizontal_reader_realdata.c`: opens the real dataset through the horizontal reader and pins its format version, source format, halo and forest counts (skips when `snapshots/` does not resolve)
- `_tests/data/`, `_tests/input/`: the committed contract fixtures, which are still version 2 (below)
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked; `snapshots/` itself is gitignored)

## Data provenance

The dataset is not primary data. It was produced offline by `convert/mimic-convert/convert_trees.py` at converter commit `6ae714829a3b68f934e88ed1464a97532e671f27` from the same `tree_0_0_0.dat`, `forests.list` and `locations.dat` that `simulations/micro-uchuu-ascii/` reads, using that package's converter profile (`simulations/micro-uchuu-ascii/converter_columns.yaml`, `column_mapping_sha256 727d13f529fa80305f261b933612b6087aa52ade5dde7899bb18f8f4450f8f6d`, no extra fields). The route applies the vertical reader's value conventions (spin normalisation, `Len` derivation, the `fix_upid` host fix-up, no `fix_flybys`): payload fields keep their native Consistent-Trees types and units, in particular `M_Crit200` as float32 `Mvir` in `Msun/h`; `Len` is `round(Mvir * 1e-10 / particle_mass)` and `Spin` is J/Mvir; links, offsets and remapping keys are int64.

Converted: 22,580,924 halos across 50 snapshots (all populated) and 440,651 forests, with no gapped links (`links_adjacent = 1`, longest descendant span 1); the largest snapshot (27) holds 621,360 halos and `max_halo_rank_in_forest` is 350,074. The producer validation battery (`convert_trees.py validate`) passed all 20 of its checks, including the `identity` check that every halo's `SourceHaloID` is its 1-based (`ForestIndex`, `HaloRankInForest`) position, which for a complete dataset proves every slab is grouped by forest. The dataset was also compared with `convert/mimic-convert/tests/run_generalisation_acceptance.py`: `compare` against a source dump through Mimic's own `consistent_trees_ascii` reader matched all 22,580,924 halos with no failure in any link, target snapshot, payload field or `SourceHaloID`, and `compare-extras` against independent Python extraction of the ASCII file passed with no finding.

Cosmology, box size and particle mass match `simulations/micro-uchuu-ascii/simulation_info.yaml` exactly: Ωm 0.3089, ΩΛ 0.6911, h 0.6774, box 100 Mpc/h, particle mass 0.0327 × 10¹⁰ Msun/h.

## Regenerating the dataset

Run the converter phases in order against the vertical package, emitting to a workdir outside the repository, then install the written snapshot files at the permanent destination:

```bash
S=simulations/micro-uchuu-ascii
W=/path/to/scratch/convert-workdir
D=/path/to/micro-uchuu-ascii-horizontal
SRC="--source-format consistent_trees_ascii \
    --simulation-info $S/simulation_info.yaml --a-list $S/micro-uchuu.a_list \
    --column-map $S/converter_columns.yaml \
    --forests-list $S/snapshots/forests.list --tree-file $S/snapshots/tree_0_0_0.dat"

mimic_venv/bin/python convert/mimic-convert/convert_trees.py inspect $SRC
mimic_venv/bin/python convert/mimic-convert/convert_trees.py ingest --workdir "$W" $SRC
mimic_venv/bin/python convert/mimic-convert/convert_trees.py transpose --workdir "$W"
mimic_venv/bin/python convert/mimic-convert/convert_trees.py write --workdir "$W" --simulation-info "$S/simulation_info.yaml"
mimic_venv/bin/python convert/mimic-convert/convert_trees.py validate --workdir "$W"
mimic_venv/bin/python convert/mimic-convert/convert_trees.py report --workdir "$W"

A=$(mimic_venv/bin/python -I -c 'import json, sys; print(json.load(open(sys.argv[1]))["stages"]["write"]["directory"])' "$W/manifest.json")
mkdir -p "$D"
cp "$W/$A"/snapshot_*.h5 "$W/$A"/forests.h5 "$D"/
```

`$A` is the write attempt the manifest records (`stages.write.directory`, for example `write/attempt_001`); it is the attempt `validate` and `report` bound the dataset to, so copy from that directory. Verify the installed files against the manifest: each `<attempt>/<file>` artefact under `artifacts` in `manifest.json` records its `sha256`, which confirms the copy.

Keep the workdir: its `manifest.json` is what `validate` and `report` bind the dataset to. `convert/mimic-convert/README.md` documents the workdir layout, resume semantics, memory budgeting and the independent comparison tooling.

## Setting up the snapshots symlink

```bash
ln -s /path/to/micro-uchuu-ascii-horizontal simulations/micro-uchuu-ascii-horizontal/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_049.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the converted dataset (about 3.4 GB).

## Running this package

The package is runnable end to end through the horizontal driver (`run_horizontal_driver()`), with shipped run files pairing it with `halos-only` and `sage16`:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal
./mimic models/halos-only/input/halos-only_micro-uchuu-ascii-horizontal.yaml
```

Horizontal runs are HDF5-only and do not support `--skip`; both are rejected at configuration time. The dataset is forest-blocked (every slab's rows are grouped by forest in ascending `ForestIndex`), so the horizontal driver accepts it distributed over several ranks (`NTask > 1`, a `USE-MPI=yes` build under `mpirun`) and swept in chunks (`input.forest_chunks` above 1), after checking the blocking of every slab at startup. On 2026-10-08 the shipped `halos-only` and `sage16` run files were run on this dataset serially, at `mpirun -np 4` (a `USE-MPI=yes` build) and serially at `input.forest_chunks: 4`, and each distributed and chunked output was bitwise identical per `UniqueGalaxyID` to the serial one (4,409,643 `halos-only` and 3,112,186 `sage16` galaxies over the eight output snapshots); no other rank or chunk count was measured on this dataset. See [`docs/USER-GUIDE.md`](../../docs/USER-GUIDE.md) → "Running Horizontal Input" and [`docs/DEVELOPER-GUIDE.md`](../../docs/DEVELOPER-GUIDE.md) → "The Horizontal Driver".

`models/sham/input/sham_micro-uchuu-ascii-horizontal-realdata.yaml` runs `sham` on this dataset (the shipped `sham_micro-uchuu-ascii-horizontal.yaml` reads the committed fixture). On 2026-10-08 its output on this dataset was bitwise identical per `UniqueGalaxyID` to the same run on the version 2 conversion this dataset replaced (557,669 galaxies at snapshot 49, every field).

## The cross-format identity gate

`_tests/scientific/test_cross_format_identity.py` proves the horizontal driver agrees with the vertical driver reading the same catalogue (`micro-uchuu-ascii`): it builds both packages in isolated worktrees at HEAD and requires, for every output snapshot, the same `UniqueGalaxyID` set and per-ID bitwise-identical fields, under both `halos-only` and `sage16`, under both fixed and dynamic timestep schemes, with no tolerance of any kind, compared by [`scripts/compare_cross_format_identity.py`](../../scripts/compare_cross_format_identity.py). It also requires the vertical run to write several partition files, and its Stage 8 rebuilds the vertical side at a pinned reference commit (`BASELINE_COMMIT`) and requires byte-identical galaxy records, so it is also the check that the vertical ASCII reader still produces the reference galaxies.

It is a **manual, dataset-present operation**:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-scientific
```

on a machine holding both the `micro-uchuu-ascii` and `micro-uchuu-ascii-horizontal` datasets. It is registered only when this package is selected, so it never runs as part of the default-pair suite or CI, and no automated tier runs it. A missing dataset, or one that is not the pinned conversion (format version 3, `source_format consistent_trees_ascii`, the profile digest above, the halo and forest counts, `links_adjacent 1`), fails the gate loudly rather than skipping. Full mechanics are in [`docs/DEVELOPER-GUIDE.md`](../../docs/DEVELOPER-GUIDE.md) → "The cross-format identity gate".

The recorded gate of 2026-10-08 passed on this version 3 dataset: under `halos-only`, 4,409,643 galaxies over output snapshots 7, 8, 10, 12, 16, 23, 28 and 49, bitwise identical per `UniqueGalaxyID` in all 20 fields under both fixed and dynamic timesteps; under `sage16`, 3,112,186 galaxies (fixed) and 3,112,152 (dynamic) in all 42 fields; and Stage 8's 4,409,643 vertical galaxy records byte-identical to the reference commit's. It is evidence for this route against its own source format only: no identity with the L-Halo or forests-HDF5 micro-Uchuu packagings is claimed.

## Committed contract fixtures

`_tests/data/` holds a tiny version 2 dataset, and `_tests/data/generic/` the generic-tier copy of the same synthetic forests at the last six scale factors of this package's snapshot list (rebuilt by its `regenerate.sh`): six snapshots (one of them empty), three forests, and topology chosen to exercise the cases a reader must handle: a descendant with three progenitors, a two-member FoF group, and a flyby-demoted central carrying a negated `MostBoundID`. **The committed fixtures are version 2, unlike the real dataset**, and stay version 2 until the version 2 format is retired (Stage D of the version 3 migration), when a version 3 fixture converted by `convert_trees.py` from a committed synthetic ASCII source replaces them. Until then the package's generic test tiers (`_tests/input/test_simulation.yaml`) and the two fixture-based C unit tests run on version 2 data, which the reader still accepts with this package's `long long` link declarations.

Regenerate it with:

```bash
mimic_venv/bin/python simulations/micro-uchuu-ascii-horizontal/_tests/input/create_snapshot_fixture.py
```

The generator never hand-writes HDF5 content. In a scratch temporary workdir it synthesises a tiny Consistent-Trees ASCII tree, runs the version 2 `convert/mimic-convert/convert_ctrees.py` pipeline over it in production layout, runs the version 2 producer validation battery (`convert/mimic-convert/validate.py`) and aborts unless it exits 0, then copies the validated datasets here and asserts that every dataset element, every header attribute, and every `/ForestID` value is identical to the validated production-layout file. Re-running regenerates byte-identical files and a byte-identical `fixture_manifest.json` (a canonical, path-independent record; the converter's own `manifest.json` records absolute paths and source `mtime_ns` and is not committable).

**Why the committed fixture is re-chunked.** Production data uses the contract chunk shape `(65536,)` / `(65536, 3)`. HDF5 allocates a chunk in full as soon as any element in it is written, so a snapshot file holding even one halo would cost 6.25 MiB, and a committed production-layout fixture would dwarf the repository. The frozen specification makes chunk layout a storage detail — "consumers must not depend on chunk boundaries, only on dataset shape and type" — so the committed copy is re-chunked small. Values, dtypes, shapes, the object set, and every header attribute are preserved exactly; chunk shape is the only permitted difference, which the generator enforces on every run.

Check conformance of the committed fixture (everything the version 2 producer battery asserts structurally, except chunk shape):

```bash
mimic_venv/bin/python simulations/micro-uchuu-ascii-horizontal/_tests/input/check_fixture_conformance.py
```

## Related packages

- `simulations/micro-uchuu-ascii/` — the same halos in Consistent-Trees ASCII, the conversion source and the cosmology reference
- `simulations/micro-uchuu-hdf5/` — the same halos in uchuutools forests-HDF5
- `simulations/micro-uchuu/` — the same halos in L-Halo binary
- `simulations/micro-uchuu-horizontal/` — the same simulation converted from L-Halo binary at version 3; its `M_Crit200` is in `1e10 Msun/h`, and no identity with this package is claimed
- `simulations/micro-uchuu-hdf5-horizontal/` — the same simulation converted from forests-HDF5 at version 3; no identity with this package is claimed
