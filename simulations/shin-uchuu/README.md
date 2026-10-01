# Shin-Uchuu Simulation Package — Horizontal HDF5

This package declares the horizontal HDF5 on-disk record for the Shin-Uchuu halo catalog: one `snapshot_NNN.h5` file per snapshot holding that snapshot's whole halo population as a struct-of-arrays, plus the `forests.h5` provenance sidecar. The on-disk contract is frozen in [`convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md); this package conforms to that specification, never the other way around.

- `simulation_info.yaml`: input paths, snapshot list path, cosmology, units, box size, and particle mass
- `halo_properties.yaml`: the RawHalo field contract — every `/halos` dataset of the frozen format, with names and types matching the specification exactly. Deliberately omits `ForestIndex` and `HaloRankInForest` (see file header), mirroring `micro-uchuu-ascii-horizontal`.
- `shin-uchuu.a_list`: 70 snapshot scale factors (a=0.04773 to a=0.99998)
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked)
- `_tests/`: not present (see "Maintenance notes")

This package's dataset is not primary data: it is produced offline by `convert/mimic-convert/` from the full Shin-Uchuu Consistent-Trees ASCII catalog (source: `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` on OzSTAR, 2744 `tree_*.dat` files, 11.61 TB, 315,004,242 z=0 halos, 166,547,771 forests, 70 snapshots), applying the reference reader's value conventions (spin normalisation, `Len` derivation, `fix_upid`) and rewriting global-id links as snapshot-local indices. `MostBoundID` is always positive. Cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774) is the Uchuu/Planck-2015 family; particle mass (8.97×10⁵ Msun/h) and box size (140 Mpc/h) are specific to Shin-Uchuu.

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

Horizontal runs are HDF5-only, serial-only (`NTask == 1`; multi-rank horizontal execution is not implemented), and do not support `--skip` — all three are rejected at configuration time. See [`docs/USER-GUIDE.md`](../../docs/USER-GUIDE.md) → "Running Horizontal Input" and [`docs/DEVELOPER-GUIDE.md`](../../docs/DEVELOPER-GUIDE.md) → "The Horizontal Driver".

## Maintenance notes

- **`unique_galaxy_id_multiplier: 20000000000` (2×10¹⁰)** must match `simulations/shin-uchuu-ascii/`, or `UniqueGalaxyID` diverges between the two packages. Confirmed against this catalog's measured `max_halo_rank_in_forest` ≈ 1.265×10¹⁰.
- **`Spin` range `[-1000, 1000]`.** Measured over the full production dataset: max `|Spin|` = 416.69, zero non-finite.
- **`deltaMvir` range `[-1000000.0, 1000000.0]`** (declared in `src/core/core_properties.yaml`, not this package; widened after a measured 4.77e4 on mini-Uchuu). Measured over the `sage16` production run: max `|deltaMvir|` = 12,432.
- **`_tests/` ships a synthetic contract fixture, not a parity gate.** `_tests/data/` holds the committed version 2 fixture (three forests over six snapshots, rebuilt by `_tests/data/regenerate.sh` with the generator in `micro-uchuu-ascii-horizontal/_tests/input/`) and `_tests/input/test_simulation.yaml` points the generic test tiers at it. There is no package-local conformance check, schema test or cross-format identity gate yet; `make check-horizontal-fixture` covers only the `micro-uchuu-ascii-horizontal` fixture.

## How the production dataset was made

The dataset under `snapshots/` was produced by the legacy version 2 converter, `convert/mimic-convert/convert_ctrees.py`, from the full 2,744-file Consistent-Trees ASCII catalogue (measured total 22,503,649,037 halos; the largest snapshot, 34, holds 519,342,987). This section is what a maintainer re-running that conversion needs: the command sequence with every flag it depends on, the storage envelope and its measured basis, and the memory term. The tool's general behaviour — consumptive deletion, batch mode, `--pool-size`, the bounded memory of each stage — is in the [converter manual](../../convert/mimic-convert/README.md#legacy-consistent-trees-ascii-to-version-2-with-convert_ctreespy). Before the production run, a fresh end-to-end conversion of real micro-Uchuu by the same converter reproduced 22,580,924 halos, 50 snapshots and 440,651 forests, with the producer battery at 15/15 and the topology cross-check at 8/8.

### Where it runs

The repository's `output/` is a development convenience and cannot hold this conversion: the workdir peaks at several terabytes (see the envelope below). The volume used was an external disk mounted at `/Volumes/LaCie`, laid out as:

```text
workdir:             /Volumes/LaCie/convert/shin-uchuu
production dataset:  /Volumes/LaCie/data/shin-uchuu/production-snapshot   (write --output-dir)
retained subset:     /Volumes/LaCie/data/shin-uchuu/subset-snapshot       (must survive)
```

Check free space before starting (`df -k <volume>`): the cold-start floor is **7.0 TB**, the ceiling the envelope below is derived against. It is a one-time preflight, not an invariant to re-enforce between batches. Worker scratch accumulates across every batch and is deleted only once, by `sort`, after `finalize`, so free space legitimately trends down through the batch loop (the production run saw about 5.2 TB free between batches without trouble); use a **1.0 TB** floor for the between-batch re-check.

### The command sequence

The order is enforced by the code: `write` refuses any snapshot not yet `linked`, and `sort` → `fixups` → `links` are what carry a snapshot there. Every flag is per invocation and carries no manifest state, so an omission is silent and must be repeated on every batch.

Two operator-made list files drive the batch loop. `inventory.txt` is the complete inventory: every one of the 2,744 `tree_*.dat` paths under the staged source directory, one per line, in natural numeric order (`ls "$SRC"/tree_*.dat | sort -V`). The converter freezes that set and order on the first `scatter --batch` run and refuses any later invocation whose list differs, so the same file is passed on every batch, whether or not its bytes have arrived. `batch_N.txt` is the subset of `inventory.txt` whose bytes are on disk for batch `N`, in the same order; it is what `release` is given once that batch has been scattered. Neither file is read by the converter itself; they only expand into its positional arguments.

```bash
WORKDIR=/Volumes/LaCie/convert/shin-uchuu
DATASET=/Volumes/LaCie/data/shin-uchuu/production-snapshot
A_LIST=simulations/shin-uchuu/shin-uchuu.a_list
SIM_INFO=simulations/shin-uchuu/simulation_info.yaml
FORESTS=/path/to/production/forests.list   # 7.56 GB, transferred before anything else;
                                           # the subset's index is NOT usable here

# 1. Scatter, once per transferred batch. Every invocation is handed the COMPLETE
#    frozen inventory (all 2,744 files, in the frozen order), not the subset on
#    disk; entries whose bytes have not arrived are deferred, and a --batch run
#    never finalizes. --pool-size defaults to 1.
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py scatter --batch \
    --pool-size 8 \
    --workdir "$WORKDIR" \
    --forests-list "$FORESTS" --a-list "$A_LIST" --simulation-info "$SIM_INFO" \
    $(cat inventory.txt)

# 2. Release that batch, before finalizing and before deleting its source bytes.
#    Repeat 1-2 per batch, and delete each released batch before step 4.
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py release \
    --workdir "$WORKDIR" $(cat batch_1.txt)

# 3. Finalize, once, after every inventory entry is scattered and released.
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py finalize \
    --workdir "$WORKDIR" --forests-list "$FORESTS"

# 4. Sort. Always deletes the concatenated file once its successors verify.
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py sort --workdir "$WORKDIR"

# 5-7. Fix-ups, links, write, each with --consume-intermediates (IRREVERSIBLE).
#      links takes no --a-list; write emits straight to the permanent path.
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py fixups --consume-intermediates \
    --workdir "$WORKDIR" --a-list "$A_LIST" --simulation-info "$SIM_INFO"
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py links --consume-intermediates \
    --workdir "$WORKDIR"
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py write --consume-intermediates \
    --workdir "$WORKDIR" --a-list "$A_LIST" --simulation-info "$SIM_INFO" \
    --output-dir "$DATASET"

# 8. Report: runs the producer battery over the dataset recorded in the manifest.
mimic_venv/bin/python convert/mimic-convert/convert_ctrees.py report \
    --workdir "$WORKDIR" --a-list "$A_LIST" \
    --multiplier 20000000000
```

Three flags are the ones whose omission costs days:

- **`--pool-size 8` on every batch-mode `scatter`.** Measured over the same 2,744-file layout (210.57 GB of subset data, one run each), scatter ran at 46.8 MB/s serial and 71.9 MB/s at `--pool-size 8`, which projects onto the 11.61 TB source as ≈68.9 h against ≈44.9 h. 8 is the only pooled value measured.
- **`--consume-intermediates` on all three of `fixups`, `links` and `write`.** Without it the conversion does not fit the volume (precondition (a) below).
- **`--multiplier 20000000000` on `report`.** The default 10⁹ fails this catalogue's rank bound (`max_halo_rank_in_forest` ≈ 1.265 × 10¹⁰) and exits 1. The argument is an integer, so `2e10` is rejected.

No cross-check step appears: the cross-check's reference side is a vertical run over the same data and cannot be built at this scale (the manual explains why), so it was run on micro-Uchuu and on the `shin-uchuu-ascii` subset instead, and no cross-check artifact belongs in this envelope.

### Storage envelope

The per-halo terms are the ones measured stage by stage on micro-Uchuu with deletion on (the [storage table](../../convert/mimic-convert/README.md#storage-per-stage) in the manual), except that `emitted` uses 100.7 B/halo, measured on the 406,668,896-halo `shin-uchuu-ascii` subset, because small snapshots carry proportionally more HDF5 chunk overhead than this catalogue's, and the rank spill uses 88.8 B/halo (the measured 48.00 × a 1.85 worst case), because at this size the default memory budget makes the merge multi-pass. Per-halo basis: projected before the run at 22.9 × 10⁹ halos; the measured total was 22,503,649,037. Decimal TB:

| Stage, deletion ON | Coexisting terms (B/halo) | Projected peak |
|---|---|---:|
| scatter, last batch | worker scratch 108 + staged batch S + O(forests) tables ≈0.012 TB | **2.47 TB + S** |
| finalize | 111.96 (S already released) | 2.56 TB |
| sort | 119.67 | 2.74 TB |
| fixups | 131.40 | 3.01 TB |
| **links, rank pass** | fixed 120 + idx 8 + identity 16 + spill 88.8 = 232.8 | **5.33 TB** |
| links, linking phase | fixed 120 + links 36 + identity 16 + pending 4 = 176 | 4.03 TB |
| write | fixed 120 + links 36 + one snapshot's emitted 1.4 = 157.4 | 3.61 TB |
| report / validate | emitted 100.7 (+2.86 GB RAM bitset, 0 B disk) | 2.31 TB |

**Scatter binds, through the staged source batch.** With a staged batch `S ≤ 4.4 TB` the peak is `2.47 + 4.4 + 0.012 =` **6.89 TB**, inside the 7.0 TB ceiling; every other stage peaks at or below 5.33 TB. The volume used measured 7.71 TB free before the run. The envelope holds only under three preconditions:

- **(a) deletion enabled.** With the flag off the peak is 377.7 B/halo (micro-Uchuu's 384.86 with this catalogue's emitted figure substituted), ≈8.65 TB before the staged batch, which does not fit.
- **(b) a bounded staged batch.** 4.4 TB is the most this envelope admits, so the 11.61 TB source needs at least three batches, and every released batch must be deleted before `sort` begins.
- **(c) cross-check artifacts excluded**, as above.

### Memory

The link stage's production peak was measured at about **235 GB** RSS during the production conversion; plan against that figure, on a host with well over it (the production run used a 512 GB machine). Converter memory at this scale is a per-snapshot term, not a per-dataset one, so do not scale the micro-Uchuu ≈24 B/halo to this catalogue; the pre-run estimate was a two-point fit of the link stage's peak against the largest snapshot on micro-Uchuu and the 406,668,896-halo subset. Re-derive the term from the conversion report's own per-snapshot counts before a re-run. The producer battery additionally holds a one-bit-per-halo identity bitset, about 2.8 GB at the measured total.

## Related packages

- `simulations/shin-uchuu-ascii/` — a subset of the same catalog in Consistent-Trees ASCII, read by the vertical driver
- `simulations/micro-uchuu-ascii-horizontal/` — the worked exemplar this package's structure and conventions mirror
