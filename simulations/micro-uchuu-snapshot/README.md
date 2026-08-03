# micro-Uchuu Simulation Package — Snapshot-Ordered HDF5

This package declares the snapshot-ordered HDF5 on-disk record for the micro-Uchuu halo catalog: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays, plus the `forests.h5` provenance sidecar. The on-disk contract is frozen in [`docs/dev/SNAPSHOT-HDF5-FORMAT.md`](../../docs/dev/SNAPSHOT-HDF5-FORMAT.md) at `format_version = 1`; this package conforms to that specification, never the other way around.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: the RawHalo field contract — every `/halos` dataset of the frozen format, with names and types matching the specification exactly
- `micro-uchuu.a_list`: 50 snapshot scale factors (a=0.06688 to a=0.99951), an exact copy of the `micro-uchuu-ascii` list
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked)
- `_tests/data/`: committed contract fixtures — a tiny, self-validating dataset
- `_tests/input/`: the fixture generator and the fixture conformance checker

## Data provenance

The dataset is not primary data. It is produced offline by the converter under `scripts/convert/` from the same micro-Uchuu Consistent-Trees ASCII trees that `simulations/micro-uchuu-ascii/` reads, applying the reference reader's value conventions (spin normalisation, `Len` derivation, `fix_flybys`/`fix_upid`) and rewriting global-id links as snapshot-local indices. The converted micro-Uchuu dataset is 22,580,924 halos across 50 snapshots and 440,651 forests (~2.3 GB).

Cosmology, box size and particle mass therefore match `simulations/micro-uchuu-ascii/simulation_info.yaml` exactly: Ωm 0.3089, ΩΛ 0.6911, h 0.6774, box 100 Mpc/h, particle mass 0.0325 × 10¹⁰ Msun/h.

## Regenerating the full dataset

Run the converter phases in order against the ASCII package, emitting straight to the permanent destination — the manifest records the emitted paths, so files must never be moved afterwards:

```bash
W=output/convert/micro-uchuu
A=simulations/micro-uchuu-ascii/micro-uchuu.a_list
S=simulations/micro-uchuu-ascii/simulation_info.yaml
D=/path/to/micro-uchuu-snapshot

mimic_venv/bin/python scripts/convert/convert_ctrees.py scatter --workdir $W \
    --forests-list simulations/micro-uchuu-ascii/snapshots/forests.list \
    --a-list $A --simulation-info $S \
    simulations/micro-uchuu-ascii/snapshots/tree_0_0_0.dat
mimic_venv/bin/python scripts/convert/convert_ctrees.py sort --workdir $W
mimic_venv/bin/python scripts/convert/convert_ctrees.py fixups --workdir $W \
    --a-list $A --simulation-info $S
mimic_venv/bin/python scripts/convert/convert_ctrees.py links --workdir $W
mimic_venv/bin/python scripts/convert/convert_ctrees.py write --workdir $W \
    --a-list $A --simulation-info $S --output-dir $D
mimic_venv/bin/python scripts/convert/validate.py $D --a-list $A --manifest $W/manifest.json
mimic_venv/bin/python scripts/convert/convert_ctrees.py report --workdir $W --a-list $A
```

`scripts/convert/README.md` documents the workdir layout, the resume semantics, and the cross-check against a `halos-only` reference run.

## Setting up the snapshots symlink

```bash
ln -s /path/to/micro-uchuu-snapshot simulations/micro-uchuu-snapshot/snapshots
```

The directory must hold `snapshot_000.h5` … `snapshot_049.h5` and `forests.h5`. The symlink is machine-local and gitignored; nothing in the repository ships the 2.3 GB dataset.

## Committed contract fixtures

`_tests/data/` holds a tiny conforming dataset — six snapshots (one of them empty), three forests, and topology chosen to exercise the cases a reader must handle: a descendant with three progenitors, a two-member FoF group, and a flyby-demoted central carrying a negated `MostBoundID`.

Regenerate it with:

```bash
mimic_venv/bin/python simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py
```

The generator never hand-writes HDF5 content. In a scratch temporary workdir it synthesises a tiny Consistent-Trees ASCII tree, runs the full `scripts/convert/` pipeline over it in production layout, runs the producer validation battery (`scripts/convert/validate.py`) and aborts unless it exits 0, then copies the validated datasets here and asserts that every dataset element, every header attribute, and every `/ForestID` value is identical to the validated production-layout file. Re-running regenerates byte-identical files and a byte-identical `fixture_manifest.json` (a canonical, path-independent record; the converter's own `manifest.json` records absolute paths and source `mtime_ns` and is not committable).

**Why the committed fixture is re-chunked.** Production data uses the contract chunk shape `(65536,)` / `(65536, 3)`. HDF5 allocates a chunk in full as soon as any element in it is written, so a snapshot file holding even one halo would cost 6.25 MiB, and a committed production-layout fixture would dwarf the repository. The frozen specification makes chunk layout a storage detail — "consumers must not depend on chunk boundaries, only on dataset shape and type" — so the committed copy is re-chunked small. Values, dtypes, shapes, the object set, and every header attribute are preserved exactly; chunk shape is the only permitted difference, which the generator enforces on every run.

Check conformance of the committed fixture (everything the producer battery asserts structurally, except chunk shape):

```bash
mimic_venv/bin/python simulations/micro-uchuu-snapshot/_tests/input/check_fixture_conformance.py
```

## Related packages

- `simulations/micro-uchuu-ascii/` — the same halos in Consistent-Trees ASCII, the conversion source and the cosmology reference
- `simulations/micro-uchuu-hdf5/` — the same halos in uchuutools forests-HDF5
- `simulations/micro-uchuu/` — the same halos in L-Halo binary
