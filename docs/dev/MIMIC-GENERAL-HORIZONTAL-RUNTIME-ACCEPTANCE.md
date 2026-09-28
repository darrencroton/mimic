# General Horizontal Runtime Acceptance Evidence (Slice 6)

**Status:** Slice 6 deliverable of [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md) ("Real gapped mini-Millennium parity gate"), the plan's acceptance gate. Every figure below was produced on 2026-09-29 on host `djcmacstudio`. The fresh conversion ran at commit `83e3781ba1a47eb8f1eb41e4bae78bf891e274c0` with a clean working tree. Both gates first ran at commit `2a9669319c26c585bcb2d3a62b39b63c93cb6585`, which adds only the gate harness to that commit. The hardened gate (comparator and timestep scheme pinned) then ran again at commit `dc2a9d1bac9be77d8bc3d8ab77339c71d7a21de6`, which changes only the harness and this document. Every run started from a clean working tree. The figures in §1 are identical on both runs, and the verdict lines quoted there are from the `dc2a9d1b` run. No reader, driver, model, comparator, baseline or run-file change was made to produce any of it.

**What this is and is not.** It is runtime evidence for **one route**: L-Halo mini-Millennium, all eight `trees_063.*` files, converted to horizontal-HDF5 version 3 with `simulations/mini-millennium/converter_columns.yaml`, then run through the horizontal driver and compared per `UniqueGalaxyID`, bit for bit, against the same files run through the vertical `lhalo_binary` reader. It makes no claim for any other simulation, source format or file range. It makes no claim for full Uchuu, which stays out of this plan ([Width is not memory](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md#width-is-not-memory)). The remaining routes are Slice 7's.

## 1. Result

All four parity legs passed with no tolerance, no field exclusion and no sampling. For every output snapshot, the `UniqueGalaxyID` sets are identical and every output field is identical as raw bytes.

| Leg | Verdict | Galaxies compared | Fields | Output snapshots |
|---|---|---|---|---|
| `halos-only` / fixed | **PASS** | 292,163 | 20 | 16, 18, 20, 23, 27, 32, 37, 63 |
| `halos-only` / dynamic | **PASS** | 292,163 | 20 | 16, 18, 20, 23, 27, 32, 37, 63 |
| `sage16` / fixed | **PASS** | 187,832 | 42 | 16, 18, 20, 23, 27, 32, 37, 63 |
| `sage16` / dynamic | **PASS** | 187,817 | 42 | 16, 18, 20, 23, 27, 32, 37, 63 |

The harness's own verdict lines, verbatim:

```text
[gate   0m28s]   LEG halos-only/fixed: PASS
[gate   0m28s]   LEG halos-only/dynamic: PASS
[gate   0m28s]   LEG sage16/fixed: PASS
[gate   0m28s]   LEG sage16/dynamic: PASS
[gate   0m28s]   all 4 parity legs PASS
```

The comparator, `scripts/compare_cross_format_identity.py` (unchanged), gave these summary lines, one per leg in the same order:

```text
PASSED: 292163 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates
PASSED: 292163 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates
PASSED: 187832 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates
PASSED: 187817 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates
```

Per-snapshot galaxy counts, identical on both sides of each leg:

| Snapshot | 16 | 18 | 20 | 23 | 27 | 32 | 37 | 63 |
|---|---|---|---|---|---|---|---|---|
| `halos-only` (both schemes) | 4,068 | 8,751 | 15,089 | 26,127 | 40,011 | 53,455 | 62,525 | 82,137 |
| `sage16` / fixed | 3,908 | 8,195 | 13,543 | 22,022 | 30,551 | 35,881 | 37,202 | 36,530 |
| `sage16` / dynamic | 3,908 | 8,195 | 13,543 | 22,022 | 30,551 | 35,881 | 37,202 | 36,515 |

**Divergences:** none. The comparator reported no id-set difference and no differing field at any snapshot on any leg, so there is nothing to trace to an owning slice. The fixed and dynamic `sage16` runs differ from each other at snapshot 63 (36,530 against 36,515 galaxies). That is the timestep scheme's physical effect, and each scheme's two drivers agree bit for bit on it.

The plan's Gate R0 review asked how a non-gap `sage16` divergence would be adjudicated, given the vertical baseline's ≈0.1% chaotic threshold flips. The question did not arise: no record differed on any leg, so there was no divergence to adjudicate.

## 2. The horizontal dataset: a fresh conversion

The converter ran at commit `83e3781ba1a47eb8f1eb41e4bae78bf891e274c0` into a new workdir outside the repository, `/Volumes/Internal/data/millennium/convert-workdir-v3-slice6`. Every phase exited 0:

```bash
S=simulations/mini-millennium
W=/Volumes/Internal/data/millennium/convert-workdir-v3-slice6
mimic_venv/bin/python scripts/convert/convert_trees.py ingest --workdir "$W" --source-format lhalo_binary \
    --simulation-info "$S/simulation_info.yaml" --a-list "$S/mini-millennium.a_list" \
    --column-map "$S/converter_columns.yaml" --halo-properties "$S/halo_properties.yaml" \
    --source-dir "$S/snapshots" --tree-name trees_063 --first-file 0 --last-file 7
mimic_venv/bin/python scripts/convert/convert_trees.py transpose --workdir "$W"
mimic_venv/bin/python scripts/convert/convert_trees.py write --workdir "$W" --simulation-info "$S/simulation_info.yaml"
mimic_venv/bin/python scripts/convert/convert_trees.py validate --workdir "$W"
mimic_venv/bin/python scripts/convert/convert_trees.py report --workdir "$W"
```

The report, `$W/conversion_report.txt`, records these figures verbatim:

```text
source format:  lhalo_binary
format version: [3] (declared by the emitted files)
mapping:        column_mapping_sha256 5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1
totals: 1533122 halo(s) in 64 snapshot file(s) (56 populated); 29585 forest(s); 0 Len==0 halo(s)
links: links_adjacent=0 (measured 0); 29291 gapped Descendant link(s), longest span 2; 26210 gapped FirstProgenitor link(s); 3979 NextProgenitor link(s) off their owner's snapshot
validation: PASS
```

The same report opens with `NOT RUNNABLE BY THE CURRENT MIMIC`, and its runtime-compatibility section says the reader rejects version 3 and the driver cannot carry state across a gap. That is converter-era text that `scripts/convert/report.py` prints unconditionally. This gate's result supersedes it, and the plan schedules its update for Slice 8.

All 20 producer validation checks passed. The 65 written files (`snapshot_000.h5` to `snapshot_063.h5` and `forests.h5`) were copied with `/bin/cp -f` into `/Volumes/Internal/data/millennium/mini-millennium-horizontal`, replacing the uncommitted conversion that Slice 3 made there. Slice 3 also created the `simulations/mini-millennium-horizontal/snapshots` symlink, which already targets that directory and was not touched. The plan's Slice 6 criterion text says "Slice 5" here, a known plan slip. A `cmp` of every installed file against the workdir copy found no difference. Digests of the installed dataset:

- `forests.h5`: `dc404c941666d7e2d2ece668d538556e2dddbb194df4844ba6b13c4d551835e0`
- `snapshot_063.h5`: `89c9070f01b56b5a01d978a9b235cb733835ef1dd88fdc51efff91492d6ad60c`
- SHA-256 of the full `shasum -a 256 forests.h5 snapshot_*.h5` listing (65 lines, run in the dataset directory): `50b76241a541563a738c5e0d5ede7dda1bcda5e5621bdefc74fb3e862661e82b`

The gate does not trust the report; it re-measures the dataset itself (stage 2, below) and printed:

```text
64 version 3 files (56 populated) from lhalo_binary, column_mapping_sha256 5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1, links_adjacent 0
1533122 halos, 29291 gapped descendant links, longest descendant span 2
conversion inventory: 29585 forests from source files [0, 1, 2, 3, 4, 5, 6, 7], exactly the vertical package's trees_063.0..trees_063.7
```

## 3. Both sides read the same eight files

- **Conversion inventory.** The workdir manifest's adapter sources are `trees_063.0` … `trees_063.7` (ordinals 0–7) under `/Volumes/Internal/data/millennium`. Its inventory sums to 1,533,122 halos in 29,585 units: 174,845 / 151,216 / 244,615 / 305,830 / 167,008 / 193,592 / 142,055 / 153,961 halos from files 0–7. `forests.h5`'s `SourceFileOrdinal` holds exactly {0, …, 7}.
- **Vertical side.** `simulations/mini-millennium/simulation_info.yaml` declares `tree_name: trees_063`, `first_file: 0` and `last_file: 7`. `simulations/mini-millennium/snapshots` resolves to `/Volumes/Internal/data/millennium`, and every manifest source path is the same file (`os.path.samefile`) as the vertical package's `trees_063.<N>`. In every leg, the gate read the vertical run's own `RunProperties` (`TreeName`, `FirstFile`, `LastFile`, `SimulationDir`) and logged, four times: `vertical run read trees_063.0..trees_063.7 from /Volumes/Internal/data/millennium, the files the conversion inventory names`.
- **Source digests** (SHA-256, taken after the conversion): `trees_063.0` `f24229a9…aca1`, `.1` `4ca40244…0be4`, `.2` `74cf4efa…8775`, `.3` `19549830…a1f5`, `.4` `5a25e8c3…5a19`, `.5` `ee38d224…1ad8`, `.6` `1b710b58…1c91`, `.7` `22705d9c…4c04`. Sizes and July 2025 modification times are unchanged.

## 4. The gate

`simulations/mini-millennium-horizontal/_tests/scientific/test_cross_format_identity.py` is registered only when `SIMULATION=mini-millennium-horizontal` is selected. It is modelled on the micro-Uchuu v2 gate (`simulations/micro-uchuu-horizontal/_tests/scientific/test_cross_format_identity.py`). One invocation builds the four `{halos-only, sage16} × {mini-millennium, mini-millennium-horizontal}` pairs in detached worktrees at HEAD and runs all four legs. Its nine stages all passed (`MIMIC_RESULT: PASS`):

1. `stage_preconditions`: both datasets resolve, including all eight tree files, 64 snapshot files and `forests.h5`.
2. `stage_dataset_provenance`: every file is version 3 from `lhalo_binary` with the pinned `column_mapping_sha256` and `links_adjacent = 0`. The dataset holds exactly 1,533,122 halos and 29,291 gapped descendant links with a longest span of 2, and the inventory names exactly the vertical package's files.
3. `stage_run_file_diffs`: `scripts/compare_cross_format_identity.py` matches its HEAD copy byte for byte, and so do all four run files. Each horizontal run file differs from its vertical counterpart only in `simulation.name` and `output.output_directory` (plus the leading comment). The comparator is run and imported from the working tree, so this check is repeated before every comparison: a dirty comparator cannot certify parity. The `dc2a9d1b` run logged `scripts/compare_cross_format_identity.py matches its committed HEAD copy`.
4. `stage_build_worktrees`.
5.–8. `stage_halos_only_fixed`, `stage_halos_only_dynamic`, `stage_sage16_fixed`, `stage_sage16_dynamic`. Each leg runs both drivers from the worktrees' own run files. The dynamic variant is the committed file plus the single line `TimestepScheme: dynamic`. The committed file must carry no `TimestepScheme` key of its own, because Mimic's parameter reader takes the first matching key. Each leg checks that the vertical run read the inventory's files, and that snapshot lists, redshifts, cosmology, `UniqueGalaxyIDMultiplier`, field names, units and record schema are exactly equal. It then runs the comparator and checks that each run wrote exactly the requested snapshots, none empty, and that the comparator compared every record both runs wrote. The vertical side wrote 8 partitions and the horizontal side 8, one per requested snapshot.
9. `stage_leg_verdicts`: names each leg's verdict and fails unless all four passed.

Two deliberate departures from the micro-Uchuu gate:

- **Every leg runs whatever the others' outcome.** An aborted stage is reported as SKIP, and this gate must fail, never skip, when a leg does not run. A setup failure makes every later stage fail its own prerequisite check, and the verdict stage reports a leg that never ran as `FAIL: leg did not run`.
- **The comparator runs before the count checks.** A divergence is therefore always reported by snapshot, field and example id, never cut short by a count mismatch.

Negative checks exercised the failure paths. Each of these raised a FAIL: a missing dataset, a leg whose builds did not complete, a verdict stage with no leg results, a working-tree file that differs from HEAD, and a run file that already declares `TimestepScheme`.

## 5. The v2 micro-Uchuu gate on the same commit

`make MODEL=halos-only SIMULATION=micro-uchuu-horizontal tests-scientific` at `2a966931` exited 0 (`ALL SCIENTIFIC TESTS PASSED`, 400 s). All eight gate stages passed. The identity stages compared, bitwise, 4,409,643 records in 20 fields (`halos-only`, fixed and dynamic), and 3,112,186 (fixed) and 3,112,152 (dynamic) records in 42 fields (`sage16`). Stage 8 reported `4409643 galaxy records byte-identical to the aedded2f baseline`, `6 HDF5 files walked; 13 excluded provenance attribute difference(s)` and `output_schema.json differs in exactly []`. It was not rerun at `dc2a9d1b`, whose only changes are to this package's gate harness and this document. That harness is registered only under `SIMULATION=mini-millennium-horizontal`, and neither change touches any code or data the v2 gate builds, runs or reads.

## 6. The rest of the scientific tier, and one pre-existing failure

`make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific` exited 2, at both `2a966931` and `dc2a9d1b`, because of **one failure outside the gate**: `tests/scientific/test_scientific.py`'s `test_physical_ranges`.

```text
✗ FAIL: Spin component(s) outside [-20.0, 20.0] inclusive
  Actual range: -37.39 to 43.35 (across all components)
MIMIC_RESULT: FAIL test_physical_ranges -- 1 failure(s)
```

The tier's other suites passed: the comparator's own 15 tests, and the gate's 9 stages. This failure is not a parity divergence and was not introduced by this slice:

- **It predates the slice.** The same tier, run from a clean worktree at the slice's starting commit `83e3781b`, fails identically: same check, same range.
- **It is in the source data.** `snapshot_063.h5`'s `Spin` holds exactly 20 components outside [−20, 20] out of 37,028 × 3, with a range of −37.394 to 43.354. The failure's "20 out of 246411 values" is those 20 halo spins carried to z = 0 galaxies (82,137 × 3 values).
- **Why the vertical package passes.** The same test passes under `SIMULATION=mini-millennium` only because, there, it reads the first of the vertical run's eight partition files (`Loaded 9265 halos from 3432 trees`), while under the horizontal package it reads the whole snapshot (`Loaded 82137 halos from 1 trees`). The gate shows the two runs' `Spin` bytes are identical, so the vertical output carries the same 20 values in its other partitions.

Both packages declare `Spin` with `range: [-20.0, 20.0]` in their `halo_properties.yaml`. Deciding whether that declared range is wrong for real L-Halo data, or the generic test's partition sampling is, is outside this slice's surface. Both files belong to other owners, and this slice changes neither.

## 7. Unchanged state

- **Repository.** `git diff 83e3781b` touches only the gate harness and this document. No source, run file, baseline, generated file or comparator changed.
- **Data.** The eight `trees_063.*` source files are unchanged: same sizes and modification times, and the conversion only reads them. The only data written is the new workdir and the replaced contents of `/Volumes/Internal/data/millennium/mini-millennium-horizontal`, both outside the repository.
- **Symlinks.** Every `simulations/*/snapshots` symlink has the target it had before the slice. The gate's worktrees and run outputs lived under the gitignored `output/` and were removed on exit.

## 8. Reproducing

With the real mini-Millennium trees behind `simulations/mini-millennium/snapshots` and a version 3 conversion behind `simulations/mini-millennium-horizontal/snapshots` (commands in §2), run:

```bash
make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific   # this gate (about 40 s here)
make MODEL=halos-only SIMULATION=micro-uchuu-horizontal tests-scientific       # the v2 gate
```

Run them one after the other, never concurrently. The gate fails, rather than skipping, if either dataset is absent or is not the real complete conversion.
