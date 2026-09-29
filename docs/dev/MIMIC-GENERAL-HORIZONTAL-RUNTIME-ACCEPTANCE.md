# General Horizontal Runtime Acceptance Evidence (Slices 6 and 7)

**Status:** the evidence record for Slices 6 (§1–§8) and 7 (§9–§15) of [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md) ("Real gapped mini-Millennium parity gate"), the plan's acceptance gate. Every figure below was produced on 2026-09-29 on host `djcmacstudio`. The fresh conversion ran at commit `83e3781ba1a47eb8f1eb41e4bae78bf891e274c0` with a clean working tree. Both gates first ran at commit `2a9669319c26c585bcb2d3a62b39b63c93cb6585`, which adds only the gate harness to that commit. The hardened gate (comparator and timestep scheme pinned) then ran again at commit `dc2a9d1bac9be77d8bc3d8ab77339c71d7a21de6`, which changes only the harness and this document. Every run started from a clean working tree. The figures in §1 are identical on both runs, and the verdict lines quoted there are from the `dc2a9d1b` run. No reader, driver, model, comparator, baseline or run-file change was made to produce any of it.

**What this is and is not.** It is runtime evidence for **one route**: L-Halo mini-Millennium, all eight `trees_063.*` files, converted to horizontal-HDF5 version 3 with `simulations/mini-millennium/converter_columns.yaml`, then run through the horizontal driver and compared per `UniqueGalaxyID`, bit for bit, against the same files run through the vertical `lhalo_binary` reader. It makes no claim for any other simulation, source format or file range. It makes no claim for full Uchuu, which stays out of this plan ([Width is not memory](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md#width-is-not-memory)). The remaining routes are Slice 7's.

**Slice 7 addendum.** The remaining routes' evidence is recorded below, in [§9–§15](#9-slice-7-remaining-version-3-package-routes). §1–§8 are Slice 6's record and are unchanged.

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

## 9. Slice 7: remaining version 3 package routes

Slice 7 of [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md) adds four version 3 packages, one per (simulation, source format) as Gate R0-10 names them, and gates each under `halos-only` against its **own** vertical package. Every figure below was produced on 2026-09-29 on host `djcmacstudio`. The four conversions ran at commit `4c9518d3e7ad8c942b95155a5e37ebbb4ff4131b`, the slice's starting commit. The four gates, the Slice 6 gate, the v2 gate and the default-pair suite all ran at commit `6cd0c448714f77f33d6bcaadd82d36a9769548d8`, which adds the four packages, their run files and their tests to that commit. That run started and ended with a clean working tree. A correction round then committed `7900080cac56817e5d436b2cd8f778699aa55dc6`, which changes only `mini-uchuu-horizontal`'s declared `Spin` range and the four schema-agreement tests (§12), and reran the affected suites there (§12, §13). No reader, driver, comparator, baseline, existing package or existing run file was changed.

**What this is and is not.** It is `halos-only` runtime evidence for four more routes, each compared bit for bit per `UniqueGalaxyID` against the vertical reader of **the same source format over the same files**. It makes no cross-source-format identity claim: the two micro-Uchuu packages are not claimed equal to each other, or to the version 2 `micro-uchuu-horizontal`. It makes no `sage16` claim for these four routes. Two of the four are **sampled subsets** and support no whole-simulation claim.

| Package | Vertical package (reader) | Evidence | Source files converted |
|---|---|---|---|
| `simulations/micro-uchuu-lhalo-horizontal/` | `micro-uchuu` (`lhalo_binary`) | **complete real data** | `Uchuu100_Planck_lhalo_binary.0`–`.3` (4 of 4) |
| `simulations/micro-uchuu-hdf5-horizontal/` | `micro-uchuu-hdf5` (`consistent_trees_hdf5`) | **complete real data** | `MicroUchuu_mergertree_info.h5` → `MicroUchuu_mergertree.h5`, `FileN` 0–0 (the whole catalogue) |
| `simulations/millennium-horizontal/` | `millennium` (`lhalo_binary`) | **sampled subset of named files** | `trees_063.0`–`.15` (16 of 512) |
| `simulations/mini-uchuu-horizontal/` | `mini-uchuu` (`lhalo_binary`) | **sampled subset of named files** | `Uchuu400_Planck_lhalo_binary.0`–`.15` (16 of 128) |
| full Uchuu | — | **no package** | none; see §14 |

## 10. Slice 7 result

Every leg of every gate passed with no tolerance, no field exclusion and no record sampling. For every output snapshot, the `UniqueGalaxyID` sets are identical and every output field is identical as raw bytes.

| Package | Leg | Verdict | Galaxies compared | Fields | Output snapshots | File range, both sides |
|---|---|---|---|---|---|---|
| `micro-uchuu-lhalo-horizontal` | `halos-only` / fixed | **PASS** | 4,409,643 | 20 | 7, 8, 10, 12, 16, 23, 28, 49 | 0–3 |
| `micro-uchuu-lhalo-horizontal` | `halos-only` / dynamic | **PASS** | 4,409,643 | 20 | 7, 8, 10, 12, 16, 23, 28, 49 | 0–3 |
| `micro-uchuu-hdf5-horizontal` | `halos-only` / fixed | **PASS** | 4,409,643 | 20 | 7, 8, 10, 12, 16, 23, 28, 49 | 0–0 |
| `micro-uchuu-hdf5-horizontal` | `halos-only` / dynamic | **PASS** | 4,409,643 | 20 | 7, 8, 10, 12, 16, 23, 28, 49 | 0–0 |
| `millennium-horizontal` (sampled) | `halos-only` / fixed | **PASS** | 4,662,552 | 20 | 16, 18, 20, 23, 27, 32, 37, 63 | 0–15 |
| `millennium-horizontal` (sampled) | `halos-only` / dynamic | **PASS** | 4,662,552 | 20 | 16, 18, 20, 23, 27, 32, 37, 63 | 0–15 |
| `mini-uchuu-horizontal` (sampled) | `halos-only` / fixed | **PASS** | 37,332,916 | 20 | 7, 8, 10, 12, 16, 23, 28, 49 | 0–15 |
| `mini-uchuu-horizontal` (sampled) | `halos-only` / dynamic | **PASS** | 37,332,916 | 20 | 7, 8, 10, 12, 16, 23, 28, 49 | 0–15 |

Each gate's own verdict lines, verbatim (identical text in all four logs):

```text
LEG halos-only/fixed: PASS
LEG halos-only/dynamic: PASS
all 2 parity legs PASS
```

The comparator, `scripts/compare_cross_format_identity.py` (unchanged, and checked against HEAD before every comparison), gave these summary lines, twice each (fixed, then dynamic):

```text
micro-uchuu-lhalo-horizontal: PASSED: 4409643 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates
micro-uchuu-hdf5-horizontal:  PASSED: 4409643 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates
millennium-horizontal:        PASSED: 4662552 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates
mini-uchuu-horizontal:        PASSED: 37332916 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates
```

Per-snapshot galaxy counts, identical on both sides of every leg and for both schemes:

| Package | Counts by output snapshot |
|---|---|
| micro-Uchuu (both packages) | 7: 83,876 · 8: 131,622 · 10: 257,895 · 12: 391,661 · 16: 598,585 · 23: 822,183 · 28: 937,126 · 49: 1,186,695 |
| Millennium files 0–15 | 16: 73,260 · 18: 150,807 · 20: 253,519 · 23: 427,804 · 27: 642,563 · 32: 846,508 · 37: 985,912 · 63: 1,282,179 |
| mini-Uchuu files 0–15 | 7: 863,791 · 8: 1,303,629 · 10: 2,404,590 · 12: 3,511,611 · 16: 5,149,383 · 23: 6,857,857 · 28: 7,715,746 · 49: 9,526,309 |

**Divergences:** none, on any leg of any gate. The two micro-Uchuu packages happen to agree in galaxy counts, as their vertical packages already did at these snapshots. That is an observation, not a cross-format identity claim; their inputs differ (for example, `M_Crit200` is `1e10 Msun/h` in one and `Msun/h` in the other), and no comparison between them was made.

**Gap coverage.** Millennium files 0–15 carry 470,782 gapped `Descendant` links (longest span 2, `links_adjacent = 0`), so its gate is a second real gapped-retention route after mini-Millennium. The other three datasets are adjacent (`links_adjacent = 1`), and their gates exercise the two-generation path on real data.

## 11. The Slice 7 conversions

Every conversion ran `scripts/convert/convert_trees.py` at commit `4c9518d3e7ad8c942b95155a5e37ebbb4ff4131b`, from the repository root with `mimic_venv/bin/python`, into a fresh workdir outside the repository, and every phase exited 0:

```bash
S=simulations/<vertical package>
mimic_venv/bin/python scripts/convert/convert_trees.py ingest --workdir "$W" --source-format <format> \
    --simulation-info "$S/simulation_info.yaml" --a-list "$S/<a_list>" \
    --column-map "$S/converter_columns.yaml" <inventory flags>
mimic_venv/bin/python scripts/convert/convert_trees.py transpose --workdir "$W"
mimic_venv/bin/python scripts/convert/convert_trees.py write --workdir "$W" --simulation-info "$S/simulation_info.yaml"
mimic_venv/bin/python scripts/convert/convert_trees.py validate --workdir "$W"
mimic_venv/bin/python scripts/convert/convert_trees.py report --workdir "$W"
```

| Package | Inventory flags | Workdir |
|---|---|---|
| micro-Uchuu L-Halo | `--source-dir $S/snapshots --tree-name Uchuu100_Planck_lhalo_binary --halo-properties $S/halo_properties.yaml --first-file 0 --last-file 3` | `/Volumes/Internal/data/uchuu/micro-uchuu/convert-workdir-v3-slice7-lhalo` |
| micro-Uchuu forests-HDF5 | `--info-file $S/snapshots/MicroUchuu_mergertree_info.h5 --first-file 0 --last-file 0` | `/Volumes/Internal/data/uchuu/micro-uchuu/convert-workdir-v3-slice7-hdf5` |
| Millennium 0–15 | `--source-dir $S/snapshots --tree-name trees_063 --halo-properties $S/halo_properties.yaml --first-file 0 --last-file 15` | `/Volumes/Internal/data/millennium/convert-workdir-v3-slice7-millennium` |
| mini-Uchuu 0–15 | `--source-dir $S/snapshots --tree-name Uchuu400_Planck_lhalo_binary --halo-properties $S/halo_properties.yaml --first-file 0 --last-file 15` | `/Volumes/Internal/data/uchuu/convert-workdir-v3-slice7-mini-uchuu` |

The micro-Uchuu L-Halo and mini-Uchuu conversions started from a clean working tree. The forests-HDF5 and Millennium conversions started while the eight new Slice 7 paths (four package directories and four run files) were already present and untracked. No converter file, profile, or vertical package file differed from `4c9518d3`.

Each report, `$W/conversion_report.txt`, records these figures. The producer validation battery passed all 20 of its checks on every route (`validation: PASS`).

| Package | `source_format` | `column_mapping_sha256` | Halos | Snapshot files (populated) | Forests | `links_adjacent` | Gapped `Descendant` links (longest span) | Largest snapshot |
|---|---|---|---|---|---|---|---|---|
| micro-Uchuu L-Halo | `lhalo_binary` | `5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1` | 22,580,924 | 50 (50) | 440,651 | 1 | 0 (1) | 621,360 |
| micro-Uchuu forests-HDF5 | `consistent_trees_hdf5` | `241f277cf059378077c19c16c4584ff171a8f2b9fba55e395a7808e6033bcbce` | 22,580,924 | 50 (50) | 440,651 | 1 | 0 (1) | 621,360 |
| Millennium 0–15 | `lhalo_binary` | `5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1` | 23,720,119 | 64 (58) | 443,945 | 0 | 470,782 (2) | 579,910 |
| mini-Uchuu 0–15 | `lhalo_binary` | `5a74a2e07eca5f3a5fef5e02b75e15c821f94370606d7608b25f082f9f5654b1` | 181,188,125 | 50 (50) | 3,230,400 | 1 | 0 (1) | 4,970,910 |

Millennium also reports 419,392 gapped `FirstProgenitor` links and 64,663 `NextProgenitor` links off their owner's snapshot. These totals match the converter-plan acceptance evidence ([`MIMIC-CONVERTER-GENERALISATION-ACCEPTANCE.md`](MIMIC-CONVERTER-GENERALISATION-ACCEPTANCE.md) §1) for the same routes and file ranges. Conversion wall times, summed over the five phases, were 189 s, 215 s, 198 s and 1,584 s, with peak phase RSS of 3.74, 3.72, 3.91 and 3.98 GB (`/usr/bin/time -l` peak memory footprint), in table order.

The written files (`snapshot_*.h5` plus `forests.h5`) were copied with `/bin/cp -f` into new directories parallel to each vertical package's data. A `cmp` of every installed file against its workdir copy found no difference. A new gitignored `snapshots` symlink in each package points at its directory. The SHA-256 digests below are of the `shasum -a 256 forests.h5 snapshot_*.h5` listing, run in each dataset directory:

| Package | Dataset directory (`snapshots` target) | Files | Listing SHA-256 | `forests.h5` SHA-256 |
|---|---|---|---|---|
| micro-Uchuu L-Halo | `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-lhalo-horizontal` | 51 | `7b9230d7d5d535d610c145920d82f3834b50cecf77cab38e3d9a6694f68bbcb2` | `39c455966793221b0d7bb9e50aa44e74c85d1f635a9328e6b18c409c9a279519` |
| micro-Uchuu forests-HDF5 | `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-hdf5-horizontal` | 51 | `17c63b4b1540196ee69b9e0dfe706f88924ea3e574804f9616b4e75658e4d7a1` | `dfb11f79a84c75452fb2d0d3e17b2e2d6941711c4433bd109af0c8ae66703c40` |
| Millennium 0–15 | `/Volumes/Internal/data/millennium/millennium-horizontal` | 65 | `f8c2c9c0e817a8aaf272dac1d4527450dd43054392358a3a6b161663f0f4a150` | `f6634457d44f9fbbada267135def442c5715aef1df33481d5c28fb0dc6b01e90` |
| mini-Uchuu 0–15 | `/Volumes/Internal/data/uchuu/mini-uchuu-horizontal` | 51 | `2926d19340c5747ca7d9b7930385264718be19f5ca0fcc78f31ac91aa3ac4bac` | `ddb3c4b4d89ac6879e5ad482bbd057b70c8ccc1cf9b8ef6cbb76ca0b3d09cf8c` |

The reports still open with `NOT RUNNABLE BY THE CURRENT MIMIC`. That is the converter-era text §2 describes; the plan schedules its update for Slice 8.

## 12. The Slice 7 packages and gates

**Declarations.** Each `halo_properties.yaml` declares exactly the `/schema` payload its route writes, with the same type, units and `h_convention`. For the L-Halo routes, `M_Crit200` is float `1e10 Msun/h`. For forests-HDF5 it is float `Msun/h`, the native Consistent-Trees `Mvir`. The five links are `long long` (R0-2(a)). `SourceHaloID`, the three target-snapshot columns and the identity arrays are not declared (R0-3(a)). Declaration order mirrors the vertical package. The first smoke run of the forests-HDF5 package showed why: with `Spin` declared after `Vmax`, the comparator refused the pair for mismatched record schemas, because the vertical `micro-uchuu-hdf5` declares `Spin` before `VelDisp`. Reordering the declaration fixed the record schema, and no field value changed. Every declared output range mirrors the vertical package's exactly. At `6cd0c448`, `mini-uchuu-horizontal` wrongly declared `Spin` as `[-1000.0, 1000.0]` where `simulations/mini-uchuu/` declares `[-20.0, 20.0]`. That hid that package's real-data `Spin` outliers from `test_physical_ranges`. `7900080c` restores `[-20.0, 20.0]`. Ranges are metadata and not part of the output record, and the gate still passes (§13). Each `simulation_info.yaml` carries its vertical package's cosmology, box size and particle mass, and each a_list is a byte copy of the vertical one.

**Schema-agreement test.** `_tests/integration/test_schema_conformance.py` in each package passed 4 of 4 in `make MODEL=halos-only SIMULATION=<package> tests-integration`, at `6cd0c448` and again at `7900080c`. It checks the declarations against the converter's own `/schema` derivation (`scripts/convert/column_schema.py` over the vertical package's profile), whose digest must equal the pinned `column_mapping_sha256`. It also checks them against the build's compiled `catalog_field_metadata.inc`. Since `7900080c`, a missing file, or one generated for another package, fails rather than warns, because the test runs only with its own package selected and the registered tier regenerates first. The two negative checks failed with `catalog_field_metadata.inc is missing; run 'make MODEL=<model> SIMULATION=mini-uchuu-horizontal generate'` and `... was generated from simulations/mini-uchuu-horizontal/halo_properties.yaml, not simulations/millennium-horizontal/halo_properties.yaml`. It checks the declarations against the real dataset's `/schema` in every snapshot file when the data is present. A mutation check, setting the L-Halo package's `M_Crit200` to `Msun/h`, failed the YAML check with `M_Crit200: declared units 'Msun/h' != /schema '1e10 Msun/h'`.

**Parity gate.** `_tests/scientific/test_cross_format_identity.py` in each package is the Slice 6 harness restricted to `halos-only`, with its package-specific values in one parameter block. The four copies are identical below that block. It has seven stages:

- **Preconditions.**
- **Dataset provenance.** The format version, `source_format`, `column_mapping_sha256` and `links_adjacent` of every file must match the pinned values, as must the halo, forest and gapped-link totals and the longest span. The inventory's `SourceFileOrdinal` set must equal the vertical side's file range.
- **Committed comparator and run files.** The horizontal run file must differ from the effective vertical one in exactly `simulation.name` and `output.output_directory`, checked on both the parsed keys and the comment-free lines.
- **HEAD worktree builds.**
- **Two parity legs**, fixed and dynamic. Both runs must record the same `FirstFile`–`LastFile`, and the vertical run must record the inventory's `TreeName` and data directory.
- **Leg verdicts.**

It fails, never skips, when data is missing or is not the pinned conversion.

**Identical file ranges on both sides.**

- **Millennium.** The shipped `halos-only_millennium.yaml` already reads files 0–15, so both sides use committed run files. The gate logged `file range: 0-15 on both sides (committed vertical run file range 0-15)`.
- **mini-Uchuu.** The shipped `halos-only_mini-uchuu.yaml` reads files 0–3. The gate runs the vertical side from a scratch copy that differs from it in `input.last_file` alone (3 → 15), and verifies that only the range keys changed. It logged `file range: 0-15 on both sides (committed vertical run file range 0-3, overridden in a scratch copy)` and, in each leg, `vertical run read Uchuu400_Planck_lhalo_binary.0..Uchuu400_Planck_lhalo_binary.15 from /Volumes/Internal/data/uchuu/mini-uchuu, the files the conversion inventory names; both runs record FirstFile-LastFile 0-15`. The shipped run file is unchanged.
- **Horizontal run files.** For the two sampled packages, the horizontal run file carries the same `input.first_file: 0`/`last_file: 15`. It is metadata only for the horizontal reader, and it records the range in the output's `RunProperties`.

Gate wall times at `6cd0c448`: 43 s, 46 s, 47 s and 255 s, including builds. At `7900080c` the mini-Uchuu gate reran in 297 s: `LEG halos-only/fixed: PASS`, `LEG halos-only/dynamic: PASS`, `all 2 parity legs PASS`, 37,332,916 galaxies bitwise identical in both legs. Before committing, a manual run of each pair measured the horizontal `halos-only` process. Millennium 0–15 retained at most 3 generations (1.05 GB resident, 7.05 GB peak memory footprint). mini-Uchuu 0–15 retained at most 2 generations (5.87 GB resident, 36.4 GB peak memory footprint), against 1.58 GB for its vertical run. These are single measurements, not a model.

## 13. Other suites on the same commit

All at `6cd0c448`, run one after another, never concurrently:

- **Slice 6 gate** (`make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific`): all four legs PASS (`LEG halos-only/fixed`, `halos-only/dynamic`, `sage16/fixed`, `sage16/dynamic: PASS`, `all 4 parity legs PASS`), with the same counts as §1. The tier exited 2 only because of the pre-existing `test_physical_ranges` Spin failure described in §6.
- **v2 micro-Uchuu gate** (`make MODEL=halos-only SIMULATION=micro-uchuu-horizontal tests-scientific`): exit 0, `ALL SCIENTIFIC TESTS PASSED`, 400 s. Every gate stage passed, including `stage_tree_path_preservation`, which reported `4409643 galaxy records byte-identical to the aedded2f baseline`, `6 HDF5 files walked; 13 excluded provenance attribute difference(s)` and `output_schema.json differs in exactly []`.
- **Default pair** (`make tests summary`): exit 0, `=== TLDR: ALL TESTS AND CHECKS PASSED ===`. Unit 50/50. The integration and scientific tiers passed.
- **Integration tier under each new package.** The only failures are core tests that cannot run under a horizontal package. Under `micro-uchuu-lhalo-horizontal` and `micro-uchuu-hdf5-horizontal` the tier fails 39 tests, the same 39 recorded for horizontal packages in earlier slices, mostly `output_format is 'binary', but horizontal runs are HDF5-only`. Under `millennium-horizontal` and `mini-uchuu-horizontal` it fails 42, exactly the set that fails under the accepted `mini-millennium-horizontal` on the same commit. The three extra tests in `tests/integration/test_processing_order.py` run the committed micro-Uchuu version 2 fixture under the selected package. Its header is correctly refused at open, for example `header attribute 'box_size_mpc_h' is 100 but the configured simulation value is 400`. No new package's own test failed. The four tiers reran at `7900080c` with identical failure sets (39, 39, 42, 42), no ERROR results, and each package's schema test at 4 of 4.

**Scientific tier: real-data range failures, not parity divergences.** Under `millennium-horizontal` and `mini-uchuu-horizontal`, `tests-scientific` exited 2, because `tests/scientific/test_scientific.py`'s `test_physical_ranges` failed. Millennium fails on `Spin`. mini-Uchuu fails on `deltaMvir` and, once its `Spin` range mirrors the vertical package (`7900080c`), on `Spin` too. The Millennium lines are from `6cd0c448`; the mini-Uchuu lines are from the `7900080c` rerun (`test_physical_ranges -- 2 failure(s)`):

```text
millennium-horizontal: ✗ FAIL: Spin component(s) outside [-20.0, 20.0] inclusive
  Actual range: -143.9 to 149.5 (across all components)
  Failed: 292 out of 3846537 values (0.0%)
mini-uchuu-horizontal: ✗ FAIL: deltaMvir outside [-20000.0, 20000.0] inclusive
  Actual range: -4.771e+04 to 8860
  Failed: 1 out of 9526309 halos (0.0%)
mini-uchuu-horizontal: ✗ FAIL: Spin component(s) outside [-20.0, 20.0] inclusive
  Actual range: -165.6 to 228.1 (across all components)
  Failed: 1371 out of 28578927 values (0.0%)
```

These are the same class as §6's mini-Millennium Spin failure. The generic test reads one partition file. Under a horizontal package that is a whole snapshot (`Loaded 1282179 halos from 1 trees`; `Loaded 9526309 halos from 1 trees`); under the vertical package it is one of 16 per-file partitions. Both gates show every field, `Spin` and `deltaMvir` included, byte-identical per `UniqueGalaxyID` at every output snapshot, so the vertical output carries exactly the same values. Each package's `Spin` range is `[-20.0, 20.0]`, the one its vertical package (`simulations/millennium/`, `simulations/mini-uchuu/`) declares, mirrored unchanged. The `deltaMvir` range is the core property's own (`src/core/core_properties.yaml`). This slice changes neither. Whether those declared ranges are wrong for real data, or the test's partition sampling is, is the owner-escalated follow-up §6 already names. The two micro-Uchuu packages pass the whole tier (`ALL SCIENTIFIC TESTS PASSED`).

## 14. Full Uchuu, and what remains a gap

- **Full Uchuu has no version 3 package.** R0-10 names none, and none is in Slice 7's surface. The slice criterion allows at most a fixture-level package test, so none was added. Its production conversion and run remain explicitly **unperformed**. They are out of scope per [Width is not memory](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md#width-is-not-memory), and nothing here says it runs.
- **Millennium and mini-Uchuu are sampled subsets of named files**, `trees_063.0`–`.15` and `Uchuu400_Planck_lhalo_binary.0`–`.15`. No whole-simulation conversion or runtime claim is made for either. **Named gap of this plan:** whole-simulation evidence needs the owner to supply Millennium files 16–511 and mini-Uchuu files 16–127.
- **Only `halos-only` is gated for these four routes.** No `sage16` run file exists for them (`models/sage16/` is outside this slice's surface), so no `sage16` parity is claimed.

## 15. Slice 7 unchanged state and reproduction

- **Repository.** `git diff 4c9518d3 6cd0c448` adds only the four package directories and four `models/halos-only/input/*-horizontal.yaml` run files. This record is a follow-up commit. `7900080c` then changes only `simulations/mini-uchuu-horizontal/halo_properties.yaml`'s `Spin` range and the four `test_schema_conformance.py` copies. `scripts/convert/tests/test_generalisation.py`'s check that no `models/*/input/*uchuu*.yaml` or `*millennium*.yaml` names a converter profile passes.
- **Data.** Source files were only read. The new data are the four workdirs and four dataset directories listed in §11, all outside the repository. No existing dataset directory was written.
- **Symlinks.** Four new gitignored `snapshots` symlinks were created, one per new package. Every existing `simulations/*/snapshots` symlink is untouched.

With the source data behind each vertical package's `snapshots` and each conversion behind its package's `snapshots` (commands in §11 and in each package README), run these one after another:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-lhalo-horizontal tests-scientific
make MODEL=halos-only SIMULATION=micro-uchuu-hdf5-horizontal tests-scientific
make MODEL=halos-only SIMULATION=millennium-horizontal tests-scientific
make MODEL=halos-only SIMULATION=mini-uchuu-horizontal tests-scientific
```
