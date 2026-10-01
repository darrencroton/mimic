# micro-Uchuu (from Consistent-Trees ASCII) Simulation Package — Horizontal HDF5 (version 2)

This package declares the horizontal HDF5 on-disk record for the micro-Uchuu halo catalog: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays, plus the `forests.h5` provenance sidecar. The on-disk contract is frozen in [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md) at `format_version = 2`; this package conforms to that specification, never the other way around.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: the RawHalo field contract — every `/halos` dataset of the frozen format, with names and types matching the specification exactly
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951), an exact copy of the `micro-uchuu-ascii` list
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults, a copy of `simulations/micro-uchuu-ascii/plot_profile.yaml` (same box) with the package name changed; `mimic-plot.py` discovers it from `simulations/<simulation.name>/`
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked)
- `_tests/data/`: committed contract fixtures — a tiny, self-validating dataset
- `_tests/input/`: the fixture generator and the fixture conformance checker

## Data provenance

The dataset is not primary data. It is produced offline by the converter under `convert/mimic-convert/` from the same micro-Uchuu Consistent-Trees ASCII trees that `simulations/micro-uchuu-ascii/` reads, applying the reference reader's value conventions (spin normalisation, `Len` derivation, `fix_upid`) and rewriting global-id links as snapshot-local indices. `fix_flybys()`, which demoted every FoF central except the most massive at each forest's final snapshot and so merged unrelated groups, was removed from the Consistent-Trees ASCII reader and the converter, and the three micro-Uchuu readers now agree on population, Types and `MostBoundID` sets (measured at snap49 when the fix landed; at earlier snapshots `fix_flybys` never acted); the converter no longer demotes independent FoF centrals at a forest's maximum scale, and `MostBoundID` is always positive. The converted micro-Uchuu dataset is 22,580,924 halos across 50 snapshots and 440,651 forests (~2.3 GB).

Cosmology, box size and particle mass therefore match `simulations/micro-uchuu-ascii/simulation_info.yaml` exactly: Ωm 0.3089, ΩΛ 0.6911, h 0.6774, box 100 Mpc/h, particle mass 0.0327 × 10¹⁰ Msun/h.

## Regenerating the full dataset

Run the converter phases in order against the ASCII package, emitting straight to the permanent destination — the manifest records the emitted paths, so files must never be moved afterwards:

```bash
W=output/convert/micro-uchuu
A=simulations/micro-uchuu-ascii/micro-uchuu.a_list
S=simulations/micro-uchuu-ascii/simulation_info.yaml
D=/path/to/micro-uchuu-snapshot

mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py scatter --workdir $W \
    --forests-list simulations/micro-uchuu-ascii/snapshots/forests.list \
    --a-list $A --simulation-info $S \
    simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py sort --workdir $W
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py fixups --workdir $W \
    --a-list $A --simulation-info $S
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py links --workdir $W
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py write --workdir $W \
    --a-list $A --simulation-info $S --output-dir $D
mimic_venv/bin/python convert/mimic-convert/validate.py $D --a-list $A --manifest $W/manifest.json
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py report --workdir $W --a-list $A
```

`convert/mimic-convert/README.md` documents the workdir layout, the resume semantics, and the cross-check against a `halos-only` reference run.

## Setting up the snapshots symlink

```bash
ln -s /path/to/micro-uchuu-snapshot simulations/micro-uchuu-ascii-horizontal/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_049.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the 2.3 GB dataset.

## Running this package

The package is runnable end to end through the horizontal driver (`run_horizontal_driver()`), with shipped run files pairing it with both `halos-only` and `sage16`:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal
./mimic models/halos-only/input/halos-only_micro-uchuu-ascii-horizontal.yaml
```

Horizontal runs are HDF5-only, serial-only (multi-rank horizontal execution is not implemented; a horizontal configuration requires `NTask == 1`), and do not support `--skip` — all three are rejected at configuration time. See [`docs/USER-GUIDE.md`](../../docs/USER-GUIDE.md) → "Running Horizontal Input" and [`docs/DEVELOPER-GUIDE.md`](../../docs/DEVELOPER-GUIDE.md) → "The Horizontal Driver".

## The cross-format identity gate

`_tests/scientific/test_cross_format_identity.py` proves the horizontal driver agrees with the vertical driver reading the same catalogue (`micro-uchuu-ascii`): for every output snapshot, the same `UniqueGalaxyID` set and per-ID bitwise-identical fields, under both `halos-only` and `sage16`, under both fixed and dynamic timestep schemes, with no tolerance of any kind.

It is a **manual, dataset-present operation**:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-scientific
```

on a machine holding both the `micro-uchuu-ascii` and `micro-uchuu-ascii-horizontal` datasets. It is registered only when this package is selected, so it never runs as part of the default-pair suite or CI, and no automated tier runs it. A missing dataset fails the gate loudly, naming the path it looked for, rather than skipping. The comparator is [`scripts/compare_cross_format_identity.py`](../../scripts/compare_cross_format_identity.py); full mechanics are in [`docs/DEVELOPER-GUIDE.md`](../../docs/DEVELOPER-GUIDE.md) → "The cross-format identity gate".

## Committed contract fixtures

`_tests/data/` holds a tiny conforming dataset — six snapshots (one of them empty), three forests, and topology chosen to exercise the cases a reader must handle: a descendant with three progenitors, a two-member FoF group, and a flyby-demoted central carrying a negated `MostBoundID`.

Regenerate it with:

```bash
mimic_venv/bin/python simulations/micro-uchuu-ascii-horizontal/_tests/input/create_snapshot_fixture.py
```

The generator never hand-writes HDF5 content. In a scratch temporary workdir it synthesises a tiny Consistent-Trees ASCII tree, runs the full `convert/mimic-convert/` pipeline over it in production layout, runs the producer validation battery (`convert/mimic-convert/validate.py`) and aborts unless it exits 0, then copies the validated datasets here and asserts that every dataset element, every header attribute, and every `/ForestID` value is identical to the validated production-layout file. Re-running regenerates byte-identical files and a byte-identical `fixture_manifest.json` (a canonical, path-independent record; the converter's own `manifest.json` records absolute paths and source `mtime_ns` and is not committable).

**Why the committed fixture is re-chunked.** Production data uses the contract chunk shape `(65536,)` / `(65536, 3)`. HDF5 allocates a chunk in full as soon as any element in it is written, so a snapshot file holding even one halo would cost 6.25 MiB, and a committed production-layout fixture would dwarf the repository. The frozen specification makes chunk layout a storage detail — "consumers must not depend on chunk boundaries, only on dataset shape and type" — so the committed copy is re-chunked small. Values, dtypes, shapes, the object set, and every header attribute are preserved exactly; chunk shape is the only permitted difference, which the generator enforces on every run.

Check conformance of the committed fixture (everything the producer battery asserts structurally, except chunk shape):

```bash
mimic_venv/bin/python simulations/micro-uchuu-ascii-horizontal/_tests/input/check_fixture_conformance.py
```

## Related packages

- `simulations/micro-uchuu-ascii/` — the same halos in Consistent-Trees ASCII, the conversion source and the cosmology reference
- `simulations/micro-uchuu-hdf5/` — the same halos in uchuutools forests-HDF5
- `simulations/micro-uchuu/` — the same halos in L-Halo binary
- `simulations/micro-uchuu-horizontal/` — the same simulation converted from L-Halo binary at version 3; no identity with this package is claimed
- `simulations/micro-uchuu-hdf5-horizontal/` — the same simulation converted from forests-HDF5 at version 3; no identity with this package is claimed
