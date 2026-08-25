# Shin-Uchuu Simulation Package — Snapshot-Ordered HDF5 (rehearsal subset)

This package declares the snapshot-ordered HDF5 on-disk record for the Shin-Uchuu halo catalog: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays, plus the `forests.h5` provenance sidecar. The on-disk contract is frozen in [`docs/dev/SNAPSHOT-HDF5-FORMAT.md`](../../docs/dev/SNAPSHOT-HDF5-FORMAT.md); this package conforms to that specification, never the other way around.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: the RawHalo field contract — every `/halos` dataset of the frozen format, with names and types matching the specification exactly. Deliberately omits `ForestIndex` and `HaloRankInForest` (see file header), mirroring `micro-uchuu-snapshot`.
- `shin-uchuu.a_list`: 70 snapshot scale factors (a=0.04773 to a=0.99998), an exact copy of the `shin-uchuu-ascii` list
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked)
- `_tests/`: not present (see "Maintenance notes" below)

**This package currently points at a SUBSET, not the full box.** It is converted, via `scripts/convert/`, from the same rehearsal subset that `simulations/shin-uchuu-ascii/` reads: 8,000,198 tree roots in 6,011,205 whole forests (2.54% of the box's 315,004,242 z=0 halos, ≈416 million halos), selected as a fixed-seed random sample of tractable forests, whose forest-size distribution is validated against the whole population, plus the top 20 forests by measured root `Mvir` within the byte-selected candidate pool. Both claims are bounded: the supplement is the most massive systems *in that pool*, not provably the global top 20 outside the excluded super-forest, and forest-size representativeness is a proxy for, not a direct test of, low halo mass. See `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`, "Subset Selection and Extraction", for how that subset was built, and "Simulation package changes required" for why two packages (this one and `shin-uchuu-ascii`) exist rather than one.

**After the production conversion, `snapshots/` is re-pointed at the production dataset.** Nothing else in this package is expected to change: `simulation_info.yaml`'s cosmology, box size, and particle mass are already the confirmed production values, not subset-specific placeholders. The identity multiplier (`unique_galaxy_id_multiplier`) may need re-confirming against the production conversion report before that re-point (see "Maintenance notes" below).

## Data provenance

Source: `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` on OzSTAR (login node `tooarrana`) — 2744 `tree_*.dat` files, 11.61 TB, Consistent-Trees ASCII format, 70 snapshots, 315,004,242 total halos (z=0), 166,547,771 total forests. Cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774) is the shared Uchuu/Planck-2015 family; the particle mass (8.97×10⁵ Msun/h, 362× smaller than micro-Uchuu's) and box size (140 Mpc/h) are confirmed for Shin-Uchuu specifically, not carried over.

This package's own dataset is not primary data: it is produced offline by the converter under `scripts/convert/` from `simulations/shin-uchuu-ascii/`'s subset, applying the reference reader's value conventions (spin normalisation, `Len` derivation, `fix_flybys`/`fix_upid`) and rewriting global-id links as snapshot-local indices — the same pipeline used for `micro-uchuu-snapshot`, run against Shin-Uchuu inputs instead.

## Setting up the snapshots symlink

```bash
ln -s /path/to/shin-uchuu-subset-snapshot simulations/shin-uchuu/snapshots
```

`snapshots/` is machine-local and gitignored (`.gitignore` matches `simulations/*/snapshots`), so no symlink is committed with this package; create it locally once the conversion has run. The directory must hold `snapshot_000.h5` … `snapshot_069.h5` and `forests.h5`.

## Running this package

The package is runnable end to end through the snapshot-ordered driver (`run_snapshot_driver()`), with shipped run files pairing it with both `halos-only` and `sage16`:

```bash
make MODEL=halos-only SIMULATION=shin-uchuu
./mimic models/halos-only/input/halos-only_shin-uchuu.yaml
```

Snapshot-ordered runs are HDF5-only, serial-only (`NTask == 1`, see [`MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`](../../docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md) for multi-rank execution), and do not support `--skip` — all three are rejected at configuration time. See [`docs/USER-GUIDE.md`](../../docs/USER-GUIDE.md) → "Running Snapshot-Ordered Input" and [`docs/DEVELOPER-GUIDE.md`](../../docs/DEVELOPER-GUIDE.md) → "The Snapshot Driver".

## The cross-format identity gate

Following the `micro-uchuu-snapshot` pattern, a `test_cross_format_identity.py`-style gate should compare runs over this package against `simulations/shin-uchuu-ascii/` snapshot for snapshot: the same `UniqueGalaxyID` set and per-ID bitwise-identical fields, under both `halos-only` and `sage16`. The shipped run files (`halos-only_shin-uchuu.yaml`, `halos-only_shin-uchuu-ascii.yaml`, `sage16_shin-uchuu.yaml`, `sage16_shin-uchuu-ascii.yaml`) deliberately request the identical `output.snapshot_list` on both sides — `[69, 40, 20, 10, 5, 2, 1, 0]`, the earliest three snapshots plus a spread of mid-range snapshots plus z=0 (snapshot 69) — precisely so that gate can compare them.

This is not yet wired up as a package-specific test (see "Maintenance notes" below); it is the intended purpose of running both packages side by side during the rehearsal.

## Maintenance notes

- **`unique_galaxy_id_multiplier: 10000000000` (10¹⁰)** is set from the start (not the framework default), per `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`. At production scale the **forest** bound is confirmed (`mimic_unique_galaxy_id_max_forests(10^10) = 922,337,202` against the measured 166,547,771 forests), but the **rank** bound (`HaloRankInForest` must stay below 10¹⁰) is not yet confirmed — check it against the production conversion report's measured max `HaloRankInForest` before the production run.
- **`Spin` range `[-1000, 1000]` is provisional** (decision D7). Nothing before a full production scan bounds the z=0 spin maximum from above; recalibrate if a run reports an out-of-range value.
- **`deltaMvir`, `Len`, and `Spin` ranges need calibration from a real rehearsal run.** `deltaMvir` lives in `src/core/core_properties.yaml` (core-level output property, not in this package's `halo_properties.yaml`) and is already annotated for Uchuu-scale mass swings; `Len`'s floor is 1 at this resolution.
- **`_tests/` is not shipped.** Unlike `micro-uchuu-snapshot`, this package has no committed contract fixtures, fixture generator, or conformance checker yet. Build those once the rehearsal's actual dataset shape (halo counts per snapshot, forest count, measured `HaloRankInForest`) is known from a real conversion run, following the `micro-uchuu-snapshot/_tests/` layout as the reference.

## Related packages

- `simulations/shin-uchuu-ascii/` — the same rehearsal-subset halos in Consistent-Trees ASCII, the conversion source
- `simulations/micro-uchuu-snapshot/` — the worked exemplar this package's structure and conventions mirror
