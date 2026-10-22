# Shin-Uchuu Simulation Package — L-Halo binary (cut dataset)

This package runs Mimic's vertical driver on the **cut** Shin-Uchuu merger trees, the same trees as the `shin-uchuu` package's production version 3 dataset, written as L-Halo binary files that the `lhalo_binary` reader consumes. Every forest is one unit, read in one allocation and one read; forests are stored in ascending `ForestIndex` and each forest's halos in `HaloRankInForest` order, so a run here assigns every halo the same `UniqueGalaxyID` a horizontal run of `shin-uchuu` assigns it.

- `simulation_info.yaml`: input paths (`tree_name: shin-uchuu-cut`, files `shin-uchuu-cut.0` onwards), snapshot list path, cosmology, units, box size, particle mass and the identity multiplier
- `halo_properties.yaml`: the on-disk L-Halo record, which generates `struct RawHalo` (see [The record](#the-record))
- `shin-uchuu.a_list`: 70 snapshot scale factors (a=0.04773 to a=0.99998), a copy of the sibling packages' list
- `plot_profile.yaml`: simulation-specific plotting axis limits and defaults for the halo plots (spatial distribution over the 140 Mpc/h box; halo mass function limits from `models/sage16/plots/profiles/shin-uchuu-lhalo_plot_profile.yaml`, the model-level profile `mimic-plot.py` loads by simulation name); `mimic-plot.py` discovers it from `simulations/<simulation.name>/`
- `snapshots/`: symlink to the converted dataset directory (machine-local, not tracked)
- `_tests/`: a committed test-sized fixture, the record-layout and reader unit tests and the fixture's halo-level identity test (see [Tests](#tests))

Cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774), box size (140 Mpc/h) and particle mass (8.97×10⁵ Msun/h) are those of `shin-uchuu` and `shin-uchuu-ascii`.

## The record

Each file is the traditional headerless L-Halo layout: `int ntrees`, `int totNHalos`, `int` halo counts for each of the `ntrees` forests, then every forest's records back to back. The record is this package's own, not the 104-byte SAGE L-Halo record: the fourteen fields of `halo_properties.yaml` in that order, laid out as the C compiler lays out `struct RawHalo` (little-endian on the platforms Mimic supports), 88 bytes per halo.

| Offset | Field | Type | Units |
|---:|---|---|---|
| 0 | `Descendant` | int32 | forest-local index, −1 for none |
| 4 | `FirstProgenitor` | int32 | forest-local index, −1 for none |
| 8 | `NextProgenitor` | int32 | forest-local index, −1 for none |
| 12 | `FirstHaloInFOFgroup` | int32 | forest-local index, never −1 |
| 16 | `NextHaloInFOFgroup` | int32 | forest-local index, −1 for none |
| 20 | `Len` | int32 | particles |
| 24 | `M_Crit200` | float32 | Msun/h |
| 28 | `Pos[3]` | float32 | Mpc/h, comoving |
| 40 | `Vel[3]` | float32 | km/s |
| 52 | `Spin[3]` | float32 | (Mpc/h)(km/s) |
| 64 | `VelDisp` | float32 | km/s |
| 68 | `Vmax` | float32 | km/s |
| 72 | `MostBoundID` | int64 | Consistent-Trees halo id |
| 80 | `SnapNum` | int32 | snapshot index |
| 84 | (padding) | 4 bytes | |

Every value is the horizontal dataset's own, copied without a unit, type or convention change. Masses are float32 in **Msun/h**, not the SAGE convention of 1e10 Msun/h; Mimic's generated accessor converts them to its reference unit on read. `Len` and `Spin` are the stored values (the particle count `round(Mvir × 1e-10 / particle_mass)` and the specific angular momentum `J/Mvir`, raw `J` for a zero-mass halo), not re-derived. A halo's index is its position in its forest's records, which is its `HaloRankInForest` in the horizontal dataset: descending snapshot first, then each FoF group with its central first. `SnapNum` therefore never increases along a forest's records.

A change to `halo_properties.yaml` changes the record on disk: every converted dataset must then be rewritten, and the layout test below fails until its pinned offsets are updated.

## Obtaining the data

The dataset is converted from the `shin-uchuu` package's version 3 dataset by `convert/mimic-convert/write_vertical_trees.py` (see the [converter README](../../convert/mimic-convert/README.md)), which derives the record from this package's `halo_properties.yaml`:

```bash
mimic_venv/bin/python convert/mimic-convert/write_vertical_trees.py plan \
    --input /path/to/shin-uchuu-dataset \
    --halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml \
    --output /path/to/shin-uchuu-lhalo
mimic_venv/bin/python convert/mimic-convert/write_vertical_trees.py convert \
    --input /path/to/shin-uchuu-dataset \
    --halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml \
    --output /path/to/shin-uchuu-lhalo --tree-name shin-uchuu-cut --workers 2
ln -s /path/to/shin-uchuu-lhalo simulations/shin-uchuu-lhalo/snapshots
```

`plan` cuts the forests into files of contiguous `ForestIndex` ranges (by default at most 100,000,000 halos and 1,000,000 forests per file; a forest larger than the halo cap occupies its file alone) and writes `file_plan.json`; `convert` writes and verifies every planned file and records the conversion in `manifest.json`. `convert` re-derives every forest's halo total at the start of each invocation, so a rerun reads the dataset's `ForestIndex` and `SourceHaloID` columns again before it skips completed files. `--tree-name` must be `shin-uchuu-cut`, the package's `tree_name`. The committed `input.last_file` is 360, the last file of the production dataset's plan (see [How the production dataset was made](#how-the-production-dataset-was-made)); a conversion with other caps or another input has its own last file number in `file_plan.json`, which `input.last_file` in `simulation_info.yaml` (or in a run file's `input` section) must then name.

**Every file of the range must be present.** The reader skips a missing file, and the forests after it then take other forest numbers, and so other `UniqueGalaxyID`s, than the horizontal dataset gives them.

`snapshots/` is machine-local and gitignored (`.gitignore` matches `simulations/*/snapshots`).

The converter prints its per-worker memory bound before it starts: about 20 GB for a file whose largest forest holds 1.25 million halos and about 84 GB for the file of the dataset's 126-million-halo forest, at 70 slabs and the default `--window-rows` (the converter manual gives the terms). On a rotational disk one or two workers saturate the conversion, which is seek-bound, and more can be slower (the converter manual gives the rest). Output does not depend on the worker count or the window. It exits 0 when every selected file is complete, 1 when a file fails verification and 2 on any refusal or other failure.

## How the production dataset was made

The package's production dataset is the `shin-uchuu` package's cut version 3 dataset written as L-Halo binary by `convert/mimic-convert/write_vertical_trees.py` (`plan`, then `convert`).

| | |
|---|---|
| Input | the cut version 3 dataset of [`simulations/shin-uchuu/`](../shin-uchuu/README.md): 244,953,607 forests and 22,503,649,037 halos in 70 snapshot files, `links_adjacent 1`; `forest_id_sha256` `3c215f7a37eebe1fb174ed52e1c993fb1a9693b0a89ed1bdef688abd19393f22`, `column_mapping_sha256` `727d13f529fa80305f261b933612b6087aa52ade5dde7899bb18f8f4450f8f6d` |
| Tool and commit | `write_vertical_trees.py` at commit `944364d41044eda4201916bc38d1aaa00e2bb418`; every file was written at that commit with the converter's directory unmodified, each re-read and its SHA-256 checked before it was renamed |
| Caps | 100,000,000 halos and 1,000,000 forests per file; a forest larger than the halo cap occupies its file alone |
| Files | 361, `shin-uchuu-cut.0` to `shin-uchuu-cut.360`, 1,981,300,932,572 B in all; file 0 holds the 126,380,326-halo forest alone |
| Record | 88 bytes, 14 fields, layout digest `ab04dcbae52ffd11b1e773fe3c9aafecc22371f4583eeadf2353930e5bdf5e64` (`halo_properties.yaml` md5 `095cc9a62a460f61530989325d8d9e24`) |
| Plan | `file_plan.json` md5 `071b080f67ed9d2557c934605fc32859` |
| Totals | 244,953,607 forests and 22,503,649,037 halos, the input's |

### Identity with the horizontal run

A `sage16` run on this dataset with the vertical driver (the committed `sage16` pipeline, `SubSteps: 10`, output snapshots 69, 52, 43, 40, 20, 10, 5, 2, 1 and 0, under `mpirun -np 4`) is identical to the cut horizontal `sage16` run of the `shin-uchuu` package, which was made with the same pipeline and output snapshots: 2,059,547,378 galaxies over the ten output snapshots, all 42 fields byte for byte, identical `UniqueGalaxyID` sets and no duplicates, compared with `scripts/compare_cross_format_identity.py` in its default mode.

| Snapshot | Galaxies |
|---:|---:|
| 0 | 1,854,727 |
| 1 | 3,219,989 |
| 2 | 5,147,402 |
| 5 | 16,421,826 |
| 10 | 66,306,470 |
| 20 | 281,932,492 |
| 40 | 490,934,902 |
| 43 | 473,283,408 |
| 52 | 406,693,129 |
| 69 | 313,753,033 |

Snapshot 69 is z = 0: 313,753,033 galaxies, the horizontal run's count. The 22 `mimic-plot` figures of the vertical run, drawn with the `sage16` `shin-uchuu` plot profile (`plotting.profile: models/sage16/plots/profiles/shin-uchuu_plot_profile.yaml`), are byte-identical to the horizontal run's.

The identity is claimed for `sage16`. `halos-only` is gated at subset scale (see [Evidence](#evidence)) and ran at production scale on file 0 alone, without an identity comparison; `hod` and `sham` cannot run here (see [Created-record limit](#created-record-limit)).

## Running this package

Shipped run files pair it with `halos-only` and `sage16`; they differ from the `shin-uchuu` run files only in the header comment, `simulation.name` and `output_directory`, and declare no `input` section, so the package's `simulation_info.yaml` supplies the file range:

```bash
make MODEL=sage16 SIMULATION=shin-uchuu-lhalo
./mimic models/sage16/input/sage16_shin-uchuu-lhalo.yaml
```

### Memory

The vertical driver holds one forest at a time, and a run's peak memory is set by the largest forest it processes, the 126,380,326-halo forest of file 0. Serial runs on that file, measured on one macOS host with the allocator's large-block cache disabled (`MallocLargeCache=0`; the default allocator can report more), peaked at 70.96 GB of resident memory for `sage16` (measured 2026-10-22); `sage16` needs a host with that much memory free. Of that peak, 24.4 GB is the output buffer (132,699,342 records of 184 B), sized to the forest's halos plus 5%. `halos-only` on that forest needed about 143 GB (142.6 GB), measured before the output buffer was resized and not re-measured since. Under `mpirun` each rank's peak follows the largest forest that rank holds, so the rank holding file 0 needs the same memory as a serial run.

## Identity with the sibling packages

`unique_galaxy_id_multiplier: 20000000000` (2×10¹⁰) must equal the value in `shin-uchuu` and `shin-uchuu-ascii`, or `UniqueGalaxyID` diverges between the packages. The vertical driver numbers forests by the prefix sum of the files' tree counts in file-number order, so with every file present and the files in plan order a forest's number is its `ForestIndex`, and a halo's id is `HaloRankInForest + 2×10¹⁰ × (ForestIndex + 1)`, the id the horizontal driver gives it.

## Created-record limit

A module that creates records during processing names each one in a separate negative id space sized by the run's forest count times its largest forest (times 1,024 records per host). At the production dataset's 244,953,607 forests and its largest forest of 126,380,326 halos that space does not fit a signed 64-bit id, so a module that creates records cannot run on this package under the vertical driver: the run logs the verdict at startup (`Created-record identity space (vertical, reader 'lhalo_binary'): ... does not fit int64; this run cannot create records`) and refuses every `module_create_record()` call with an error naming the module. `halos-only` and `sage16` create no records and are unaffected.

## Tests

- `_tests/data/worked_ascii_lhalo/` is the committed fixture: the three synthetic forests (13 halos) of the `shin-uchuu` package's `worked_ascii` version 3 fixture, converted by `write_vertical_trees.py` into two files (forests 0 and 1 in `shin-uchuu-cut.0`, forest 2 in `shin-uchuu-cut.1`), with the conversion's `file_plan.json` and the source fixture's a_list. `_tests/data/regenerate.sh` rebuilds it and fails if the installed data files are not exactly the converted set; the conversion's `manifest.json` is not kept, since it records the converting commit and run and would change on every rebuild. `_tests/input/test_simulation.yaml` points the generic test tiers at the fixture.
- `_tests/unit/test_record_layout.c` pins the compiled `struct RawHalo` to the record above: size 88 and every member's offset, as the converter derives them.
- `_tests/unit/test_tree_loading.c` reads both fixture files through the `lhalo_binary` reader and checks each file's tree and halo counts against its `file_plan.json`, every link inside its forest, `FirstHaloInFOFgroup` never −1, `SnapNum` non-increasing along each forest, and that reading leaks nothing.
- `_tests/integration/test_fixture_halo_identity.py` builds the reference dump tool for the compiled model and this package, dumps the fixture through the reader and proves with the generalisation acceptance harness's `compare` that it says exactly what its source dataset says: every halo, link and payload bit.

Each test runs only with `SIMULATION=shin-uchuu-lhalo`:

```bash
make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests
```

## Evidence

The evidence is of two kinds. At production scale, a `sage16` run on the production dataset is identical to the cut horizontal run (see [How the production dataset was made](#how-the-production-dataset-was-made)). At subset scale, over the cut Shin-Uchuu subset (406,668,896 halos in 7,206,195 forests, the production dataset's layout and cut rule), the halo-level check and the gate below cover `halos-only` and `sage16`. No `hod` or `sham` claim is made.

- **Halo-level check.** The subset converted to L-Halo binary reads, through this package's reader and the acceptance harness's dump tool, exactly as its version 3 horizontal dataset says: all 25 checks of `run_generalisation_acceptance.py compare --source-format lhalo_binary --dataset-source-format consistent_trees_ascii` pass over every halo, link and payload bit. Converting with 1, 4 and 8 workers gives byte-identical files.
- **Cross-format identity gate.** `_tests/scientific/test_cross_format_identity.py` runs the vertical driver on the converted subset and the horizontal driver on the dataset it came from, for `halos-only` and `sage16`, each with fixed and dynamic timesteps, and requires identical `UniqueGalaxyID` sets and byte-identical fields in every output snapshot. All four legs pass: `halos-only` 92,429,377 galaxies in both schemes, `sage16` 60,033,537 (fixed) and 60,033,573 (dynamic), over eight output snapshots. A vertical `sage16` run under `mpirun -np 4` is bitwise identical to the serial run.

The gate is a plain `ParityGate` over `tests/framework/parity_gate.py` with both `vertical_dataset` and `horizontal_dataset` set, because neither package's `snapshots/` holds the subset. It runs the comparison only when `MIMIC_SHIN_UCHUU_CUT_SUBSET_LHALO` names the subset's L-Halo conversion directory (which must hold the conversion's `file_plan.json` and `manifest.json`, the manifest proving the conversion's input and file list) and `MIMIC_SHIN_UCHUU_CUT_SUBSET_DATASET` the subset's horizontal dataset; with either unset it checks, without data, that its pins name the subset and that the committed run files differ in exactly the authorized keys, and skips the comparison with its reason. It needs 150 GiB (about 161 GB) free for its worktrees and runs, and the subset converted to at least two files:

```bash
MIMIC_SHIN_UCHUU_CUT_SUBSET_LHALO=/path/to/subset-lhalo \
MIMIC_SHIN_UCHUU_CUT_SUBSET_DATASET=/path/to/subset-dataset \
    make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests-scientific
```

The gate covers the conversion and the vertical reader at subset scale. It does not exercise a forest of the production dataset's largest size; the production `sage16` run does.

## Related packages

- `simulations/shin-uchuu/`: the horizontal version 3 dataset this package's dataset is converted from, read by the horizontal driver.
- `simulations/shin-uchuu-ascii/`: the vertical Consistent-Trees ASCII subset package. It shares this package's fields and units but reads an uncut subset, not the cut trees.
