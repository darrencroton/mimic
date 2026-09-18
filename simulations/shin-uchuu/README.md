# Shin-Uchuu Simulation Package — Horizontal HDF5

This package declares the horizontal HDF5 on-disk record for the Shin-Uchuu halo catalog: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays, plus the `forests.h5` provenance sidecar. The on-disk contract is frozen in [`docs/dev/HORIZONTAL-HDF5-FORMAT.md`](../../docs/dev/HORIZONTAL-HDF5-FORMAT.md); this package conforms to that specification, never the other way around.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: the RawHalo field contract — every `/halos` dataset of the frozen format, with names and types matching the specification exactly. Deliberately omits `ForestIndex` and `HaloRankInForest` (see file header), mirroring `micro-uchuu-horizontal`.
- `shin-uchuu.a_list`: 70 snapshot scale factors (a=0.04773 to a=0.99998)
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked)
- `_tests/`: not present (see "Maintenance notes")

This package's dataset is not primary data: it is produced offline by `scripts/convert/` from the full Shin-Uchuu Consistent-Trees ASCII catalog (source: `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` on OzSTAR, 2744 `tree_*.dat` files, 11.61 TB, 315,004,242 z=0 halos, 166,547,771 forests, 70 snapshots), applying the reference reader's value conventions (spin normalisation, `Len` derivation, `fix_upid`) and rewriting global-id links as snapshot-local indices. `MostBoundID` is always positive. Cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774) is the Uchuu/Planck-2015 family; particle mass (8.97×10⁵ Msun/h) and box size (140 Mpc/h) are specific to Shin-Uchuu.

## Setting up the snapshots symlink

```bash
ln -s /path/to/shin-uchuu-snapshot simulations/shin-uchuu/snapshots
```

`snapshots/` is machine-local and gitignored (`.gitignore` matches `simulations/*/snapshots`); it must hold `snapshot_000.h5` … `snapshot_069.h5` and `forests.h5`.

## Running this package

Runnable end to end through the horizontal driver (`run_horizontal_driver()`), with shipped run files pairing it with both `halos-only` and `sage16`:

```bash
make MODEL=halos-only SIMULATION=shin-uchuu
./mimic models/halos-only/input/halos-only_shin-uchuu.yaml
```

Horizontal runs are HDF5-only, serial-only (`NTask == 1`; see [`MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`](../../docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md) for multi-rank execution), and do not support `--skip` — all three are rejected at configuration time. See [`docs/USER-GUIDE.md`](../../docs/USER-GUIDE.md) → "Running Horizontal Input" and [`docs/DEVELOPER-GUIDE.md`](../../docs/DEVELOPER-GUIDE.md) → "The Horizontal Driver".

## Maintenance notes

- **`unique_galaxy_id_multiplier: 20000000000` (2×10¹⁰)** must match `simulations/shin-uchuu-ascii/`, or `UniqueGalaxyID` diverges between the two packages. Confirmed against this catalog's measured `max_halo_rank_in_forest` ≈ 1.265×10¹⁰.
- **`Spin` range `[-1000, 1000]`.** Measured over the full production dataset: max `|Spin|` = 416.69, zero non-finite.
- **`deltaMvir` range `[-20000, 20000]`** (declared in `src/core/core_properties.yaml`, not this package). Measured over the `sage16` production run: max `|deltaMvir|` = 12,432.
- **`_tests/` is not shipped.** No committed contract fixtures, fixture generator, or conformance checker yet; `micro-uchuu-horizontal/_tests/` is the reference layout to follow.

## Related packages

- `simulations/shin-uchuu-ascii/` — a subset of the same catalog in Consistent-Trees ASCII, read by the vertical driver
- `simulations/micro-uchuu-horizontal/` — the worked exemplar this package's structure and conventions mirror
