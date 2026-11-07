# Core-alignment set: measurement at R6

The i02-alignment-set node's measurement of the frozen core-alignment set (`series`, `series.sha256`) against SAGE26's R6 reference and the T1 reference, under the vertical driver, at the launch base `66a3e9498561e7f2f2207267a94986e9b097b03c`. Slice 1 recorded sections 1 to 6; Slice 2 adds each item's contribution at R6.

Every figure below is copied from the runner's files under the node worktree's git-ignored `output/i02/<label>/` (summaries `summary-*.txt`, comparator JSON `compare-*.json`, `context.txt`, `apply.log`, `apply-paths.txt`, the `.status` files); analysis outside the runner's directories is under `output/i02-analysis/`. "Bit for bit" means bit-identical in the instrument's mapped properties at SAGE26's storage precision with signed zero normalised, as `models/sage26/instrument/compare_sage26.py` compares them.

## 1. Provenance

- Launch base: `66a3e9498561e7f2f2207267a94986e9b097b03c` (c13's merge into `integration/sage26`).
- The set: committed at `0c184929331de4775a70c8ea9808085ea163cd16` (the node head every run measured; `node_head` in each `context.txt`). That commit is the source of `10-ffb-discarded-draw.patch`.
- SAGE26: `11b1ae851524baadb76cfd17e0b38f8a89c07224`, staged read only at `/Volumes/Internal/results/sage26-campaign/reference-src` (`R/` below).
- References: R6 `/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/out/R6/model_0.hdf5` (SHA-256 `819ba947eda92211a3ec078bd7133c5e9cfe58359b012d7db454ed231bbd5df3`) and the T1 reference `/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/out/ref/model_0.hdf5` (SHA-256 `19977093c0497339b954cb4fc0417ff249fde0de794319c224a8b9ea9e001d18`), as each `context.txt` records them.
- Runner `run.sh` SHA-256 `8e5b2cadc6f9f935a2eb7a7a09949c70f04d3c07a66a7b593ca7ee130e7221e9`; `summarize.py` `74ce8b5a0b9b4cfd5af312b73cf1d5fb785837b4b1c0feca6d5ee72e0895352a`; comparator `44c4ada726e2c2120951e7e89789a0c8950072da95f629c8b151b9c2185f5ea5`; field map `7a64d601380a612187cb5bf03b418bcdfcc0e85a673f3666e118d1fabfc2f4bb` (each `context.txt`).
- Platform: `Darwin djcmacstudio 27.0.0 ... arm64`, `Apple clang version 21.0.0 (clang-2100.4.1.1)` (each `context.txt`).

## 2. Roles and Labels

Every role ran once; no run failed and none was repeated.

| Role | Label | Mode | Sources | Snapshots | Runner exit | `clone_head` | Manifest check (`OK` / lines) | Outputs check (`OK` / lines) |
|---|---|---|---|---|---|---|---|---|
| `nine` | `nine` | list | `ca:01` … `ca:09` | T1's eight | 0 | `29c9d84cc6fc6d7d2a5d148c03213592169d1834` | 39 / 39 | 15 / 15 |
| `core7` | `core7` | list | `ca:01` … `ca:07` | T1's eight | 0 | `e79408288338558e3999086549b8f3c4eac03ba3` | 37 / 37 | 15 / 15 |
| `core7-ffb-draw` | `core7-ffb-draw` | list | `ca:01` … `ca:07`, `ca:10` | T1's eight | 0 | `5bb9a384667d284d76844e74ff4fd5b10e2f2dd5` | 38 / 38 | 15 / 15 |
| `all` | `all` | series (`--all-snapshots`) | `series` (01 … 10) | every snapshot written; compared at T1's eight and at 8 to 63 | 0 | `8ff64464be9d0211bc0dcbd8cdc817f590fdb1ed` | 47 / 47 | 15 / 15 |
| `all-list` | `all-list` | list | `ca:01` … `ca:10` | T1's eight | 0 | `31b1b63e0b37ab0bfee7c9da252eacb1f0defd01` | 40 / 40 | 15 / 15 |
| `all-repeat` | `all-repeat` | series | `series` (01 … 10) | T1's eight | 0 | `2e7cfcd5ec3c24dbff0607b2ccd4815dea9f1a01` | 30 / 30 | 15 / 15 |

Manifest checks: `cd output/i02/<label> && shasum -a 256 -c manifest.sha256`; outputs checks: `cd output/i02/<label>/clone/output/sage26-mini-millennium && shasum -a 256 -c ../../../outputs.sha256`; both outputs under `output/i02-analysis/checks/`. Every check printed `OK` on every line and exited 0.

## 3. The Consumption Map at R6

Every citation was read at the launch base (Mimic, through `git show 66a3e9498561e7f2f2207267a94986e9b097b03c:<path>`) or in `R/`; the reads are kept in `output/i02-analysis/spot-read-mimic.txt` and `output/i02-analysis/spot-read-sage26.txt`. R6's parameter file (`/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/par/R6.par`) sets `CGMrecipeOn 1` (`:54`) and `EnhancedStarFormationOn 1` (`:58`), with `FirstFile 0` and `LastFile 7` (`:9-10`). This table extends x02's (its report section 3.1 at `8fd27634`, which mapped R4) to R6. `src/`, the Makefile, `simulations/mini-millennium/`, `sage26_determine_regime.c` and `sage_keyed_draw.h` are unchanged between x02's base `5e91c808` and the launch base (`git diff --stat` empty for them), so x02's core citations hold at the launch base; each was re-read.

### 3.1 Draw Sites

| Order in a FoF evolution | SAGE26 site | Executes at R6 when | Count per FoF evolution | Mimic counterpart after the set |
|---|---|---|---|---|
| 1 | `init_galaxy`'s two discarded `(void)rand();` (`R/src/model_misc.c:91-92`, in `init_galaxy` `:65`) | the FoF creates a galaxy: its cumulative galaxy count is still 0 after the join of its first halo (`R/src/core_build_model.c:347-349`); no flag gates it | 2 if the FoF creates a galaxy, else 0 | 09: two discarded `(void)rand();` in `init_new_halo` (`src/core/inheritance.c:97-107` at the launch base), called when the FoF central's slice is empty (`:186-188`), during the join (`src/core/build_model.c:152`) |
| 2 | the CGM classifier's useful draw `(double)rand() / (double)RAND_MAX` (`R/src/model_regimes.c:94`) | `CGMrecipeOn == 1` (`R/src/core_build_model.c:410-411`; R6 sets 1), once per FoF at the start of `evolve_galaxies`; per row with `mergeType == 0` (`R/src/model_regimes.c:67`) and `mass_ratio` not `<= 0.0` (`:77`) | 1 per row with `Mvir > 0`; an orphan's `Mvir` is 0 (`R/src/core_build_model.c:325`), so it draws nothing | 08: `mimic_sage_cgm_regime_deviate` (`models/sage26/shared/sage_keyed_draw.h:142-145`) returns `(double)rand() / (double)RAND_MAX`; called by `sage26_determine_regime`'s `classify_row` (`sage26_determine_regime.c:187`) for each row it does not skip (`:236`, `:239`) with ratio not `<= 0.0` (`:170`) |
| — | `infall_recipe` (`R/src/core_build_model.c:415`) | always | 0: `R/src/model_infall.c` holds no `rand` (search below) | `sage26_prepare_infall_budget`: no draw |
| 3 | the FFB classifier's discarded `(void)rand();` (`R/src/model_regimes.c:224`) | `EnhancedStarFormationOn >= 1` (`R/src/core_build_model.c:425-426`; R6 sets 1), once per FoF after `infall_recipe`, before the substeps; per row with `mergeType == 0` (`R/src/model_regimes.c:212-213`), before that row's `halo_uniform` (`:225`) | 1 per row with `mergeType == 0`, orphans included (no `Mvir` condition) | 10: `(void)rand();` in `sage26_determine_ffb_regime`'s `classify_row` (`sage26_determine_ffb_regime.c:152-155` marks the site at the launch base), called for each row `sage26_determine_ffb_regime_process` does not skip (`:227-245`, skip `:231`), before the keyed draw (`:157-159`) |

The module order in both run files' `pre_timestep` at the launch base (`models/sage26/input/sage26_mini-millennium.yaml:31-37` and `models/sage26/input/sage26_mini-millennium-horizontal.yaml:31-37`, identical): `sage26_initialise_merger_clock`, `sage26_set_halo_concentration`, `sage26_set_disk_scale_radius`, `sage26_reionization`, `sage26_determine_regime`, `sage26_prepare_infall_budget`, `sage26_determine_ffb_regime`, each `process_full_halo`. The phase runs once per FoF (`execute_module_pipeline`, `src/core/module_registry.c:1836-1843`) from `process_halo_evolution`, after every join of the FoF (`src/core/build_model.c:152`, `:162`). So within a FoF both codes consume: the creation draws (during the join), then every CGM draw in row order, then every FFB draw in row order.

### 3.2 Order Across FoFs

As x02 recorded (its report section 3.2), with the core unchanged: one process in both; forests in the same order (`R/src/sage.c`, `FirstFile` 0 to `LastFile` 7; Mimic's non-MPI partition loop `src/core/vertical_driver.c:471` and unit loop `:305`); halos in index order (`:324-326`); the same depth-first recursion and FoF loop (`src/core/build_model.c:149-159`); progenitor rows in the same order (`gather_progenitor_galaxies`, `:324-341`); only `mergeType == 0` galaxies stored for the next snapshot in SAGE26 (`R/src/core_build_model.c:651`) and `Type == 3` rows neither emitted (`src/core/output_buffer.c:24-31`) nor inherited (`src/core/inheritance.c:170-172`) in Mimic.

The reference is one process: `reference-build.log` (node directory, SHA-256 `148b844e489a8a93885dc6c1958ad6b25c592420d80de4dcaff1b17d6e82e5de`) holds 35 `clang` command lines, none with `-DMPI` and none mentioning MPI, and links the executable `sage` with `clang`.

### 3.3 Skip Sets and the Modules Between the Classifiers

- SAGE26's FFB classifier skips only `mergeType > 0` (`R/src/model_regimes.c:213`), as its CGM classifier does (`:67`). Its workspace at either classifier holds no `mergeType > 0` row: only `mergeType == 0` galaxies are stored (`R/src/core_build_model.c:651`), and a Type 0/1 galaxy with `mergeType != 0` is marked Type 3 and its slot reused at the join (`:240-245`, `continue` before `ngal++`). Nothing between `:411` and `:426` writes `mergeType`: `infall_recipe` (`R/src/model_infall.c`) holds no `mergeType` (search: none).
- The package's FFB classifier skips a row with no galaxy or `Type == 3` (`sage26_determine_ffb_regime.c:231`), the same test as its CGM classifier (`sage26_determine_regime.c:239`), whose row set x02 showed equal to SAGE26's (its report section 3.3) and measured bit for bit at R4 (campaign evidence E35).
- The one `pre_timestep` module between `sage26_determine_regime` and `sage26_determine_ffb_regime` is `sage26_prepare_infall_budget` (read in full at the launch base: `sage26_prepare_infall_budget_process` `:254-286`, `infall_recipe` `:67-218`). It writes only gas reservoirs, `ICS` fields and `InfallingGas`; it assigns no `Type` (`git grep` for `->Type =` / `.Type =` in it: none), receives `ngal` by value (it cannot add or remove a row), creates no record (`git grep module_create_record` over `models/sage26` outside `_tests`: none), and skips the same rows (`:83`). Search output: `output/i02-analysis/row-set-checks.txt`.

So the two codes draw for the same rows in the same order: SAGE26's FFB skip set equals the package's.

### 3.4 Generator-Call Search

Pattern (ERE, a portable word boundary): `(^|[^A-Za-z0-9_])(rand|srand|random|drand48|rand_r)[[:space:]]*\(`. Commands: `(cd R && grep -rnE "$PAT" src tests)` and `git grep -nE "$PAT" 66a3e9498561e7f2f2207267a94986e9b097b03c -- src models simulations` (never the working tree); then a whole-word search (`grep -rnwE` and `git grep -nwE` for the same five names, any use: 22 hits in `R/`, 259 at the launch base). Full output: `output/i02-analysis/generator-search.txt`.

SAGE26 (`R/src`, `R/tests`):

| Hit | Class |
|---|---|
| `src/model_misc.c:91`, `:92` `(void)rand();` | production call (creation draws) |
| `src/model_regimes.c:94` `(double)rand() / (double)RAND_MAX` | production call (CGM useful draw) |
| `src/model_regimes.c:224` `(void)rand();` | production call (FFB discarded draw; executes at R6) |
| `src/model_misc.c:78`, `src/model_regimes.c:180`, `:181`, `:222` | production file, comment only |
| `tests/test_ffb_concentration.c:437, 483, 523, 571, 613, 669, 712, 721` `srand(...)` | test-only (a test program outside the reference binary) |
| `tests/test_ffb_concentration.c:433, 467, 477, 703` | test-only, comment or test name |

No `srand`, `random`, `drand48` or `rand_r` call in `R/src`.

Mimic at the launch base (`src/`, `models/`, `simulations/`): no production generator call.

| Hit | Class |
|---|---|
| `models/sage26/modules/_tests/oracle/oracle_c10.c:338` and `oracle_c13.c:399` define `int rand(void)`; their other hits are comments or strings | test-only: oracle queue providers, linked only into the oracle executables; `make` excludes `*/_tests/*` (`Makefile:113`) |
| `models/sage26/modules/_tests/oracle/README.md`, `build.sh`, `regenerate.sh`; the `_tests/oracle/*.in` fixtures and `_tests/test_unit_*.c` of `sage26_determine_regime` and `sage26_determine_ffb_regime` | test-only: documentation, comments or fixture text |
| `models/sage26/modules/sage26_determine_ffb_regime/sage26_determine_ffb_regime.c:152`, `:154`; `models/sage26/modules/sage26_determine_regime/sage26_determine_regime.c:181`; `models/sage26/shared/sage_keyed_draw.h:6`, `:135`; `models/sage26/shared/sage_ffb_regime.h:33` | production file, comment only |
| `models/sage26/model_properties.yaml:500`; the READMEs of `sage26_determine_ffb_regime`, `sage26_determine_regime` and `models/sage26/shared` | production metadata or documentation, text only |

The whole-word search adds only comments and documentation, Python plotting scripts (`models/*/plots/figures/*.py`, outside the `mimic` binary), the HOD package's counter-based generator `models/hod/shared/hod_random.h` (another package, not built with `MODEL=sage26`, and not libc) and strings in `src/io/output/python_example.c:52`, `:262` (Python text the writer emits). None is a production libc generator call in the `sage26` build.

### 3.5 Corrections to the Plan's Reading

None changes the reading. Precisions: the CGM useful draw also needs `mass_ratio` not `<= 0.0` (`R/src/model_regimes.c:77`), so an orphan (`Mvir` 0) draws in the FFB classifier but not in the CGM classifier; the module order cited by the run files' comments sits at `:31-37` in both run files at the launch base (x02 cited `:27-31` at its own base).

## 4. The `nine` Control

`sh <node dir>/run.sh nine ca:01-particle-mass.patch … ca:09-creation-draws.patch` at the node head. The control: `summary-r6.txt` must give `tables_sha256=df43e46326c5fe13f474d9db9cd9d8bdcf96e3be2331ea6590088bb0558b7c38`, the CM's aligned R6 check with the nine-patch series at c13's merge. It does: **the control holds.**

`output/i02/nine/context.txt`:

```text
label=nine
mode=list
node_head=0c184929331de4775a70c8ea9808085ea163cd16
base=66a3e9498561e7f2f2207267a94986e9b097b03c
clone_head=29c9d84cc6fc6d7d2a5d148c03213592169d1834
date_utc=2026-11-07T01:49:22Z
all_snapshots=0
reference_r6=/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/out/R6/model_0.hdf5
reference_r6_sha256=819ba947eda92211a3ec078bd7133c5e9cfe58359b012d7db454ed231bbd5df3
reference_t1=/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/out/ref/model_0.hdf5
reference_t1_sha256=19977093c0497339b954cb4fc0417ff249fde0de794319c224a8b9ea9e001d18
runner_sha256=8e5b2cadc6f9f935a2eb7a7a09949c70f04d3c07a66a7b593ca7ee130e7221e9
summarize_sha256=74ce8b5a0b9b4cfd5af312b73cf1d5fb785837b4b1c0feca6d5ee72e0895352a
instrument_compare_sha256=44c4ada726e2c2120951e7e89789a0c8950072da95f629c8b151b9c2185f5ea5
instrument_field_map_sha256=7a64d601380a612187cb5bf03b418bcdfcc0e85a673f3666e118d1fabfc2f4bb
patch_1=01-particle-mass.patch node-head:models/sage26/instrument/core-alignment/01-particle-mass.patch ef734759c5a0a3af6d67947cfca16b24b6f326faac7e19e204d16201d165bc72
patch_2=02-cosmic-time.patch node-head:models/sage26/instrument/core-alignment/02-cosmic-time.patch 0287fd00235e2dd40a9ed6e7b4ac7889e0c4d56dc620b92ac3c52cde3e9ee10d
patch_3=03-written-dt.patch node-head:models/sage26/instrument/core-alignment/03-written-dt.patch 9cbdc747792c8f904bdac9ed1b28eeb3767fe0e30f27c5d7a79dbdcd127f91ee
patch_4=04-substep-count.patch node-head:models/sage26/instrument/core-alignment/04-substep-count.patch 7d3d19ab38b6a0fb8c2bdd470a0650f36f99fdf1d3769be24445fc51813e4c65
patch_5=05-float-halo-state.patch node-head:models/sage26/instrument/core-alignment/05-float-halo-state.patch 644f5fb4f1a431ed3c607cfc0fbdce66bda6d4de8e87d8c785105a2b8fa266e9
patch_6=06-inheritance-resets.patch node-head:models/sage26/instrument/core-alignment/06-inheritance-resets.patch 38abd04677cb70e58e0c455215b952e300a63872003c54943dec5bd6a8409f51
patch_7=07-written-halo-values-stacked.patch node-head:models/sage26/instrument/core-alignment/07-written-halo-values-stacked.patch 9141dcf750acc88c40fb1effe4c1a933878cbd4609ab36666f845557390d8694
patch_8=08-cgm-useful-draw.patch node-head:models/sage26/instrument/core-alignment/08-cgm-useful-draw.patch 1ab0498c9a2e28a79a7807de336abd0f7c11573f90062556f1135502a89741c2
patch_9=09-creation-draws.patch node-head:models/sage26/instrument/core-alignment/09-creation-draws.patch eb46418407feb6de3ef0379b7c93e65541512b36aee1d3e758c3b5c3f5a877c6
patch_count=9
uname=Darwin djcmacstudio 27.0.0 Darwin Kernel Version 27.0.0: Tue Aug 11 21:05:42 PDT 2026; root:xnu-13432.1.9~1/RELEASE_ARM64_T6031 arm64
cc=Apple clang version 21.0.0 (clang-2100.4.1.1)
```

`output/i02/nine/summary-r6.txt` (first two lines and the `snap` lines; `summary-t1.txt` gives the same `tables_sha256`):

```text
verdict=fail
tables_sha256=df43e46326c5fe13f474d9db9cd9d8bdcf96e3be2331ea6590088bb0558b7c38
snap 63 n_ref=36534 n_cand=36533 matched=36532 unmatched_ref=2 unmatched_cand=1 failed_thresholds=49 float_bit_identical=24/67 int_bit_identical=5/6
snap 37 n_ref=37203 n_cand=37203 matched=37203 unmatched_ref=0 unmatched_cand=0 failed_thresholds=17 float_bit_identical=24/67 int_bit_identical=5/6
snap 32 n_ref=35882 n_cand=35882 matched=35882 unmatched_ref=0 unmatched_cand=0 failed_thresholds=16 float_bit_identical=24/67 int_bit_identical=5/6
snap 27 n_ref=30551 n_cand=30551 matched=30551 unmatched_ref=0 unmatched_cand=0 failed_thresholds=15 float_bit_identical=25/67 int_bit_identical=5/6
snap 23 n_ref=22022 n_cand=22022 matched=22022 unmatched_ref=0 unmatched_cand=0 failed_thresholds=11 float_bit_identical=25/67 int_bit_identical=5/6
snap 20 n_ref=13543 n_cand=13543 matched=13543 unmatched_ref=0 unmatched_cand=0 failed_thresholds=4 float_bit_identical=30/67 int_bit_identical=5/6
snap 18 n_ref=8195 n_cand=8195 matched=8195 unmatched_ref=0 unmatched_cand=0 failed_thresholds=3 float_bit_identical=31/67 int_bit_identical=5/6
snap 16 n_ref=3908 n_cand=3908 matched=3908 unmatched_ref=0 unmatched_cand=0 failed_thresholds=3 float_bit_identical=34/67 int_bit_identical=5/6
```

For orientation only, how far the nine patches are from R6 at T1's eight snapshots (from `output/i02/nine/compare-r6.json`; extraction in `output/i02-analysis/nine-orientation.txt`): 118 failed thresholds in total; float properties bit-identical on every matched pair in 217 of 536 property-snapshots (24, 24, 24, 25, 25, 30, 31, 34 of 67 at snapshots 63, 37, 32, 27, 23, 20, 18, 16), integer properties in 5 of 6 at every snapshot (40 of 48); at snapshot 63 `matched` 36532 of `n_ref` 36534 and `n_cand` 36533 (2 reference and 1 candidate rows unmatched), complete matching at the other seven. `FFBRegime` is equal on every matched pair at every snapshot (its draw is keyed, `halo_uniform`); `Regime` differs on 773, 787, 628, 423, 171, 55, 21 and 4 matched galaxies at snapshots 63, 37, 32, 27, 23, 20, 18 and 16 (of 36532, 37203, 35882, 30551, 22022, 13543, 8195 and 3908 matched).

## 5. The Null Control

`core7` (`ca:01` to `ca:07`) and `core7-ffb-draw` (the same, then `ca:10-ffb-discarded-draw.patch`). With the package's keyed CGM draw in place nothing reads the stream, so 10 must change no galaxy.

- `tables_sha256`: `core7` `8b4e90296611b5895b05867f8922818dbed48a562d1bf2b3a759e3904807205c` (`summary-r6.txt` and `summary-t1.txt`); `core7-ffb-draw` the same in both. Equal.
- Galaxy datasets, by x02's script (`git show 8fd27634:campaign-reports/x02-rng-alignment/tools/compare_galaxy_datasets.py`, saved as `output/i02-analysis/compare_galaxy_datasets.py`, SHA-256 `ebb16b60e49aeb78d4679087fd98d3a04d835a2990cfe66e8b0cfe4cad1b1186`), run from `output/i02-analysis/` with `mimic_venv/bin/python -B -I`; output `output/i02-analysis/null-control-datasets.txt`:

```text
$ cd output/i02-analysis && ../../mimic_venv/bin/python -B -I compare_galaxy_datasets.py ../i02/core7/clone/output/sage26-mini-millennium ../i02/core7-ffb-draw/clone/output/sage26-mini-millennium
file model.hdf5 galaxy_datasets=0 differences=0
file model_000.hdf5 galaxy_datasets=16 differences=0
file model_001.hdf5 galaxy_datasets=16 differences=0
file model_002.hdf5 galaxy_datasets=16 differences=0
file model_003.hdf5 galaxy_datasets=16 differences=0
file model_004.hdf5 galaxy_datasets=16 differences=0
file model_005.hdf5 galaxy_datasets=16 differences=0
file model_006.hdf5 galaxy_datasets=16 differences=0
file model_007.hdf5 galaxy_datasets=16 differences=0
excluded attributes of RunProperties: differs (RunProperties@RunEndTime)
excluded attributes of RunProperties/Version: differs (RunProperties/Version@git_commit, RunProperties/Version@version)
excluded dataset RunProperties/Parameters: equal
files=9 galaxy_datasets=128
verdict=identical
[exit 0]
```

**The null control holds**: 10 changes no galaxy when nothing reads the stream. The excluded items that differ are the named exclusions (run end time, and the clone commit in the version record).

## 6. The Full Set: `all`, `all-list`, `all-repeat`

### 6.1 `all` (series mode, every snapshot)

`sh <node dir>/run.sh --all-snapshots all --series`. Every status file is 0: `apply`, `build`, `clone`, `compare-r6`, `compare-r6-all`, `compare-t1`, `compare-t1-all`, `instrument`, `links`, `outputs`, `run`, `runfile`, `summary-r6`, `summary-r6-all`, `summary-t1`, `summary-t1-all`. `run-file.diff` changes only `snapshot_list` (`[63, 37, 32, 27, 23, 20, 18, 16]` to `[]`).

`output/i02/all/apply.log` (apply.sh's output; its ten digest lines, extracted in series order, equal `series.sha256` line for line: `diff` exit 0, `output/i02-analysis/all.digests`):

```text
apply.sh: applied 10 patch(es), staged in the index, nothing committed:
ef734759c5a0a3af6d67947cfca16b24b6f326faac7e19e204d16201d165bc72  01-particle-mass.patch
0287fd00235e2dd40a9ed6e7b4ac7889e0c4d56dc620b92ac3c52cde3e9ee10d  02-cosmic-time.patch
9cbdc747792c8f904bdac9ed1b28eeb3767fe0e30f27c5d7a79dbdcd127f91ee  03-written-dt.patch
7d3d19ab38b6a0fb8c2bdd470a0650f36f99fdf1d3769be24445fc51813e4c65  04-substep-count.patch
644f5fb4f1a431ed3c607cfc0fbdce66bda6d4de8e87d8c785105a2b8fa266e9  05-float-halo-state.patch
38abd04677cb70e58e0c455215b952e300a63872003c54943dec5bd6a8409f51  06-inheritance-resets.patch
9141dcf750acc88c40fb1effe4c1a933878cbd4609ab36666f845557390d8694  07-written-halo-values-stacked.patch
1ab0498c9a2e28a79a7807de336abd0f7c11573f90062556f1135502a89741c2  08-cgm-useful-draw.patch
eb46418407feb6de3ef0379b7c93e65541512b36aee1d3e758c3b5c3f5a877c6  09-creation-draws.patch
a5f75f89ff545814ed84431c689d9fe64746357ef4f2814b13016b94b868a425  10-ffb-discarded-draw.patch
apply.sh exit 0
```

`output/i02/all/apply-paths.txt` (every path within the runner's allowed set):

```text
Makefile
models/sage26/modules/sage26_determine_ffb_regime/sage26_determine_ffb_regime.c
models/sage26/shared/sage_keyed_draw.h
simulations/mini-millennium/simulation_info.yaml
src/core/halo_evolution.c
src/core/inheritance.c
src/core/init.c
src/io/output/util.c
```

Eight-snapshot verdicts: `compare-r6.json` `verdict` `pass`, `t1_eligible` `false` (R6 is not the T1 reference); `compare-t1.json` `verdict` `pass`, `t1_eligible` `true`. `tables_sha256` at T1's eight snapshots: `9780514254dee9247fadcd288c091c44f1cd570af2a76e251d05d712788713c7` in both `summary-r6.txt` and `summary-t1.txt`; at 8 to 63: `73b4d37cd2fc8e6f055586291b8182a49c1e222e798656c67af5449574d1c079` in both `summary-r6-all.txt` and `summary-t1-all.txt`. Outliers files: `outliers-r6.csv`, `outliers-t1.csv`, `outliers-r6-all.csv` and `outliers-t1-all.csv` hold 1 line each (the header only).

`output/i02/all/summary-r6-all.txt`, the 56 `snap` lines (8 to 63):

```text
snap 8 n_ref=2 n_cand=2 matched=2 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 9 n_ref=8 n_cand=8 matched=8 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 10 n_ref=29 n_cand=29 matched=29 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 11 n_ref=121 n_cand=121 matched=121 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 12 n_ref=305 n_cand=305 matched=305 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 13 n_ref=674 n_cand=674 matched=674 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 14 n_ref=1359 n_cand=1359 matched=1359 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 15 n_ref=2366 n_cand=2366 matched=2366 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 16 n_ref=3908 n_cand=3908 matched=3908 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 17 n_ref=5821 n_cand=5821 matched=5821 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 18 n_ref=8195 n_cand=8195 matched=8195 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 19 n_ref=10808 n_cand=10808 matched=10808 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 20 n_ref=13543 n_cand=13543 matched=13543 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 21 n_ref=16461 n_cand=16461 matched=16461 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 22 n_ref=19278 n_cand=19278 matched=19278 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 23 n_ref=22022 n_cand=22022 matched=22022 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 24 n_ref=24557 n_cand=24557 matched=24557 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 25 n_ref=26810 n_cand=26810 matched=26810 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 26 n_ref=28878 n_cand=28878 matched=28878 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 27 n_ref=30551 n_cand=30551 matched=30551 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 28 n_ref=32092 n_cand=32092 matched=32092 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 29 n_ref=33506 n_cand=33506 matched=33506 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 30 n_ref=34453 n_cand=34453 matched=34453 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 31 n_ref=35247 n_cand=35247 matched=35247 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 32 n_ref=35882 n_cand=35882 matched=35882 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 33 n_ref=36393 n_cand=36393 matched=36393 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 34 n_ref=36834 n_cand=36834 matched=36834 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 35 n_ref=36985 n_cand=36985 matched=36985 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 36 n_ref=37103 n_cand=37103 matched=37103 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 37 n_ref=37203 n_cand=37203 matched=37203 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 38 n_ref=37349 n_cand=37349 matched=37349 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 39 n_ref=37428 n_cand=37428 matched=37428 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 40 n_ref=37380 n_cand=37380 matched=37380 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 41 n_ref=37265 n_cand=37265 matched=37265 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 42 n_ref=37153 n_cand=37153 matched=37153 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 43 n_ref=36965 n_cand=36965 matched=36965 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 44 n_ref=36909 n_cand=36909 matched=36909 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 45 n_ref=36727 n_cand=36727 matched=36727 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 46 n_ref=36556 n_cand=36556 matched=36556 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 47 n_ref=36485 n_cand=36485 matched=36485 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 48 n_ref=36388 n_cand=36388 matched=36388 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 49 n_ref=36319 n_cand=36319 matched=36319 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 50 n_ref=36241 n_cand=36241 matched=36241 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 51 n_ref=36141 n_cand=36141 matched=36141 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 52 n_ref=36050 n_cand=36050 matched=36050 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 53 n_ref=36019 n_cand=36019 matched=36019 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 54 n_ref=35938 n_cand=35938 matched=35938 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 55 n_ref=35939 n_cand=35939 matched=35939 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 56 n_ref=35907 n_cand=35907 matched=35907 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 57 n_ref=35899 n_cand=35899 matched=35899 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 58 n_ref=35884 n_cand=35884 matched=35884 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 59 n_ref=35981 n_cand=35981 matched=35981 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 60 n_ref=35969 n_cand=35969 matched=35969 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 61 n_ref=36071 n_cand=36071 matched=36071 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 62 n_ref=36167 n_cand=36167 matched=36167 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 63 n_ref=36534 n_cand=36534 matched=36534 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
```

`output/i02/all/summary-t1-all.txt`, the 56 `snap` lines (identical to the R6 lines: `diff` exit 0):

```text
snap 8 n_ref=2 n_cand=2 matched=2 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 9 n_ref=8 n_cand=8 matched=8 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 10 n_ref=29 n_cand=29 matched=29 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 11 n_ref=121 n_cand=121 matched=121 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 12 n_ref=305 n_cand=305 matched=305 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 13 n_ref=674 n_cand=674 matched=674 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 14 n_ref=1359 n_cand=1359 matched=1359 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 15 n_ref=2366 n_cand=2366 matched=2366 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 16 n_ref=3908 n_cand=3908 matched=3908 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 17 n_ref=5821 n_cand=5821 matched=5821 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 18 n_ref=8195 n_cand=8195 matched=8195 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 19 n_ref=10808 n_cand=10808 matched=10808 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 20 n_ref=13543 n_cand=13543 matched=13543 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 21 n_ref=16461 n_cand=16461 matched=16461 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 22 n_ref=19278 n_cand=19278 matched=19278 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 23 n_ref=22022 n_cand=22022 matched=22022 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 24 n_ref=24557 n_cand=24557 matched=24557 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 25 n_ref=26810 n_cand=26810 matched=26810 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 26 n_ref=28878 n_cand=28878 matched=28878 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 27 n_ref=30551 n_cand=30551 matched=30551 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 28 n_ref=32092 n_cand=32092 matched=32092 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 29 n_ref=33506 n_cand=33506 matched=33506 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 30 n_ref=34453 n_cand=34453 matched=34453 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 31 n_ref=35247 n_cand=35247 matched=35247 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 32 n_ref=35882 n_cand=35882 matched=35882 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 33 n_ref=36393 n_cand=36393 matched=36393 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 34 n_ref=36834 n_cand=36834 matched=36834 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 35 n_ref=36985 n_cand=36985 matched=36985 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 36 n_ref=37103 n_cand=37103 matched=37103 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 37 n_ref=37203 n_cand=37203 matched=37203 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 38 n_ref=37349 n_cand=37349 matched=37349 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 39 n_ref=37428 n_cand=37428 matched=37428 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 40 n_ref=37380 n_cand=37380 matched=37380 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 41 n_ref=37265 n_cand=37265 matched=37265 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 42 n_ref=37153 n_cand=37153 matched=37153 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 43 n_ref=36965 n_cand=36965 matched=36965 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 44 n_ref=36909 n_cand=36909 matched=36909 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 45 n_ref=36727 n_cand=36727 matched=36727 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 46 n_ref=36556 n_cand=36556 matched=36556 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 47 n_ref=36485 n_cand=36485 matched=36485 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 48 n_ref=36388 n_cand=36388 matched=36388 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 49 n_ref=36319 n_cand=36319 matched=36319 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 50 n_ref=36241 n_cand=36241 matched=36241 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 51 n_ref=36141 n_cand=36141 matched=36141 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 52 n_ref=36050 n_cand=36050 matched=36050 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 53 n_ref=36019 n_cand=36019 matched=36019 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 54 n_ref=35938 n_cand=35938 matched=35938 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 55 n_ref=35939 n_cand=35939 matched=35939 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 56 n_ref=35907 n_cand=35907 matched=35907 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 57 n_ref=35899 n_cand=35899 matched=35899 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 58 n_ref=35884 n_cand=35884 matched=35884 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 59 n_ref=35981 n_cand=35981 matched=35981 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 60 n_ref=35969 n_cand=35969 matched=35969 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 61 n_ref=36071 n_cand=36071 matched=36071 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 62 n_ref=36167 n_cand=36167 matched=36167 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
snap 63 n_ref=36534 n_cand=36534 matched=36534 unmatched_ref=0 unmatched_cand=0 failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6
```

### 6.2 `all-list` (list mode, T1's eight snapshots)

The ten patches as `ca:` sources in series order; `context.txt` lists them with the digests of `series.sha256`. `tables_sha256` `9780514254dee9247fadcd288c091c44f1cd570af2a76e251d05d712788713c7` in `summary-r6.txt` and `summary-t1.txt`, equal to `all`'s eight-snapshot value for both references: the procedure applies exactly the measured patches, and writing every snapshot changes no galaxy at T1's snapshots.

### 6.3 `all-repeat` (series mode, T1's eight snapshots)

`apply.log`'s ten digest lines equal `series.sha256` (`diff` exit 0, `output/i02-analysis/all-repeat.digests`). `tables_sha256` `9780514254dee9247fadcd288c091c44f1cd570af2a76e251d05d712788713c7` in `summary-r6.txt` and `summary-t1.txt`, equal to `all-list`'s. Galaxy datasets against `all-list`'s, by the same script (`output/i02-analysis/repeat-datasets.txt`):

```text
$ cd output/i02-analysis && ../../mimic_venv/bin/python -B -I compare_galaxy_datasets.py ../i02/all-list/clone/output/sage26-mini-millennium ../i02/all-repeat/clone/output/sage26-mini-millennium
file model.hdf5 galaxy_datasets=0 differences=0
file model_000.hdf5 galaxy_datasets=16 differences=0
file model_001.hdf5 galaxy_datasets=16 differences=0
file model_002.hdf5 galaxy_datasets=16 differences=0
file model_003.hdf5 galaxy_datasets=16 differences=0
file model_004.hdf5 galaxy_datasets=16 differences=0
file model_005.hdf5 galaxy_datasets=16 differences=0
file model_006.hdf5 galaxy_datasets=16 differences=0
file model_007.hdf5 galaxy_datasets=16 differences=0
excluded attributes of RunProperties: differs (RunProperties@RunEndTime)
excluded attributes of RunProperties/Version: differs (RunProperties/Version@git_commit, RunProperties/Version@version)
excluded dataset RunProperties/Parameters: equal
files=9 galaxy_datasets=128
verdict=identical
[exit 0]
```

The build and run are deterministic: every galaxy dataset of `all-repeat` equals `all-list`'s.

### 6.4 R6 Against T1, Every Run

| Run | `summary-r6` / `summary-t1` `tables_sha256` (T1's eight snapshots) | `summary-r6-all` / `summary-t1-all` (8 to 63) |
|---|---|---|
| `nine` | `df43e46326c5fe13f474d9db9cd9d8bdcf96e3be2331ea6590088bb0558b7c38` / the same | — |
| `core7` | `8b4e90296611b5895b05867f8922818dbed48a562d1bf2b3a759e3904807205c` / the same | — |
| `core7-ffb-draw` | `8b4e90296611b5895b05867f8922818dbed48a562d1bf2b3a759e3904807205c` / the same | — |
| `all` | `9780514254dee9247fadcd288c091c44f1cd570af2a76e251d05d712788713c7` / the same | `73b4d37cd2fc8e6f055586291b8182a49c1e222e798656c67af5449574d1c079` / the same |
| `all-list` | `9780514254dee9247fadcd288c091c44f1cd570af2a76e251d05d712788713c7` / the same | — |
| `all-repeat` | `9780514254dee9247fadcd288c091c44f1cd570af2a76e251d05d712788713c7` / the same | — |

## 7. Outcome

At every snapshot 8 to 63, `all`'s `summary-r6-all.txt` shows `failed_thresholds=0 float_bit_identical=67/67 int_bit_identical=6/6` with `matched = n_ref = n_cand` (and `unmatched_ref=0 unmatched_cand=0`), `outliers-r6-all.csv` holds only its header, and `summary-t1-all.txt` shows the same (`output/i02-analysis/all-condition-check.txt`: 56 `snap` lines in each, none failing the condition).

**The set reproduces SAGE26's R6 reference, payload-identical to the T1 reference, bit for bit in every compared property at every snapshot with complete matching, under the vertical driver, at the launch base `66a3e9498561e7f2f2207267a94986e9b097b03c`.** At T1's eight snapshots the T1 comparison passes and is T1-eligible (`compare-t1.json`: `verdict` `pass`, `t1_eligible` `true`). T1's thresholds therefore need no change, and SAGE26's own floor (campaign evidence E24, quoted in Slice 2) is not engaged. No change to the comparison rules is proposed.
