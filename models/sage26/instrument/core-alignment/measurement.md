# Core-alignment set: measurement at R6

The i02-alignment-set node's measurement of the frozen core-alignment set (`series`, `series.sha256`) against SAGE26's R6 reference and the T1 reference, under the vertical driver, at the launch base `66a3e9498561e7f2f2207267a94986e9b097b03c`. Slice 1 recorded sections 1 to 7; Slice 2 recorded section 8, each item's contribution at R6 left out of the full set, with the deltas committed under `measurement/deltas/`.

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
- The one `pre_timestep` module between `sage26_determine_regime` and `sage26_determine_ffb_regime` is `sage26_prepare_infall_budget` (read in full at the launch base: `sage26_prepare_infall_budget_process` `:254-286`, `infall_recipe` `:67-218`). It writes only gas reservoirs, `ICS` fields and `InfallingGas`; it assigns no `Type` (`git grep -nE -e '->Type *=' -e '\.Type *='` at the launch base finds no assignment in the production source `sage26_prepare_infall_budget.c`, only the `Type == 3` read at `:83`; the only assignments are in its `_tests/`), receives `ngal` by value (it cannot add or remove a row), creates no record (`git grep module_create_record` over `models/sage26` outside `_tests`: none), and skips the same rows (`:83`). Search output: `output/i02-analysis/row-set-checks.txt`.

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

## 8. Each Item's Contribution at R6, Left Out (Slice 2)

Each item's effect here is its **conditional aggregate effect left out of the full set**: the ten-patch set in series order without that item, compared with SAGE26's R6 reference at T1's eight snapshots, against `all` (which is bit for bit at R6: 0 failed thresholds, 536/536 float and 48/48 integer property-snapshots bit-identical on every pair, `matched = n_ref = n_cand` at every snapshot). Effects are not additive. They are never summed or shared out among items, and these runs show no mechanism; none is claimed.

### 8.1 Runs and Labels

Each run is in list mode at T1's eight snapshots, at the node head `b91dcfd266d4f37599beb01d125175eeb1f0c4fb` (`node_head` in each `context.txt`). The patches did not change after the set's commit `0c184929` (`git diff --name-only 0c184929 b91dcfd2` lists only `README.md` and `measurement.md` of this directory). Each run used the sources below, in this order (`ca:` the committed file; `x01:` and `x02:` the pinned variants). Every run ran once, and every role's first label is its name. No run failed and none was repeated. The tenth role, `loo-ffb-discarded-draw`, maps to Slice 1's `nine` run (01 to 09, the same build), with no new run.

| Item left out | Role | Label | Sources | Runner exit | `clone_head` | Status files | Manifest check (`OK` / lines) | Outputs check (`OK` / lines) | `tables_sha256` (`summary-r6` = `summary-t1`) |
|---|---|---|---|---|---|---|---|---|---|
| 01 | `loo-particle-mass` | `loo-particle-mass` | `ca:02` … `ca:10` | 0 | `4bd9df38e729731e34146438d6dacad223c1412a` | all 0; `compare-r6` 1, `compare-t1` 1 | 39 / 39 | 15 / 15 | `ac9a7ff8018250313d32dd0df063eac3e7f8c6191479a65dbd1b671c2014dbf7` |
| 02 | `loo-cosmic-time` | `loo-cosmic-time` | `ca:01`, `ca:03` … `ca:10` | 0 | `0a6e6b9954fb44fa0b481104f77057f75ebfb512` | all 0 | 39 / 39 | 15 / 15 | `5a1bc713b9c66fe21ece6dd1b0f0d9043457823e7ebc591d4bce317a8f3b46b3` |
| 03 | `loo-written-dt` | `loo-written-dt` | `ca:01`, `ca:02`, `ca:04`, `ca:05`, `ca:06`, `x01:07-written-halo-values.patch`, `ca:08`, `ca:09`, `ca:10` | 0 | `5c95cbbffe221b6834c51bbd0189f1dad4e78b12` | all 0 | 39 / 39 | 15 / 15 | `d843b9a1fb25ac043c0d373ef2f88807379c916614b6cb364ac71ece49cc0937` |
| 04 | `loo-substep-count` | `loo-substep-count` | `ca:01` … `ca:03`, `ca:05` … `ca:10` | 0 | `a0b5beb7326aeac6fc23c7b8a42b69d244fbb0a4` | all 0; `compare-r6` 1, `compare-t1` 1 | 39 / 39 | 15 / 15 | `aa4c4f679f491cc808d4115b9d6cc6ea9251618960755aeecac565f6f5a32302` |
| 05 | `loo-float-halo-state` | `loo-float-halo-state` | `ca:01` … `ca:04`, `ca:06`, `ca:07`, `ca:08`, `x02:09-creation-draws-without-float-halo-state.patch`, `ca:10` | 0 | `249762cacb5d9a80706dbff6195fa7e2ea4562b2` | all 0; `compare-r6` 1, `compare-t1` 1 | 39 / 39 | 15 / 15 | `c0ca9721cc2c9636eb7d5880c6888902716e0466f96dbbc64c4bf69a8083daff` |
| 06 | `loo-inheritance-resets` | `loo-inheritance-resets` | `ca:01` … `ca:05`, `ca:07` … `ca:10` | 0 | `fcf0dfe5f96c8428f07e2038de2a1faecbbf1af9` | all 0 | 39 / 39 | 15 / 15 | `9780514254dee9247fadcd288c091c44f1cd570af2a76e251d05d712788713c7` |
| 07 | `loo-written-halo-values` | `loo-written-halo-values` | `ca:01` … `ca:06`, `ca:08` … `ca:10` | 0 | `c221dc1e396991c63c4df2357ed27babd50fc1b2` | all 0 | 39 / 39 | 15 / 15 | `9780514254dee9247fadcd288c091c44f1cd570af2a76e251d05d712788713c7` |
| 08 | `loo-cgm-useful-draw` | `loo-cgm-useful-draw` | `ca:01` … `ca:07`, `ca:09`, `ca:10` | 0 | `ecd4be08b6557dfa3662e4be90b08e55637b6690` | all 0; `compare-r6` 1, `compare-t1` 1 | 39 / 39 | 15 / 15 | `8b4e90296611b5895b05867f8922818dbed48a562d1bf2b3a759e3904807205c` |
| 09 | `loo-creation-draws` | `loo-creation-draws` | `ca:01` … `ca:08`, `ca:10` | 0 | `d60fc8a60f41d66f8561f0d9dd7ed25b93b7daca` | all 0; `compare-r6` 1, `compare-t1` 1 | 39 / 39 | 15 / 15 | `21b91688f7e1e06dc77362fb729c2975a2a44e450de9150c4339b2c0c9f3284e` |
| 10 | `loo-ffb-discarded-draw` | `nine` (Slice 1) | `ca:01` … `ca:09` | 0 | `29c9d84cc6fc6d7d2a5d148c03213592169d1834` | all 0; `compare-r6` 1, `compare-t1` 1 | 39 / 39 | 15 / 15 | `df43e46326c5fe13f474d9db9cd9d8bdcf96e3be2331ea6590088bb0558b7c38` |

Each `context.txt` lists exactly these sources with their SHA-256s, in this order (`patch_count=9`). The check compares each run's `patch_N` source and SHA-256 with the list built from `series.sha256` less the item and with the variant in place. Script `output/i02-analysis/slice2/check-sources.sh`, output `output/i02-analysis/slice2/check-sources.txt`: `equal` for all ten. Every `apply.status` is 0. Every `apply-paths.txt` lists only paths the runner allows. No `build.log` holds a `warning:` line (`output/i02-analysis/slice2/build-warnings.txt`). The comparator's exit 1 is its `fail` verdict. Console output for each run: `output/i02-analysis/runner-console/<label>.txt`; the driver: `output/i02-analysis/slice2/loo-runs.sh`, log `output/i02-analysis/slice2/loo-runs.log`. Manifest and outputs checks: `output/i02-analysis/checks/<label>-manifest-check.txt` and `<label>-outputs-check.txt`. `nine`'s were re-run for this slice into `output/i02-analysis/slice2/nine-manifest-recheck.txt` and `nine-outputs-recheck.txt`. Summary: `output/i02-analysis/slice2/checks-summary.txt`. Every check printed `OK` on every line and exited 0. R6 and T1 `tables_sha256` are equal for every run (`output/i02-analysis/slice2/r6-t1-tables.txt`).

Two equalities of tables, nothing more: `loo-inheritance-resets` and `loo-written-halo-values` give `all`'s `tables_sha256` (`9780514…`), and `loo-cgm-useful-draw` gives `core7`'s (`8b4e9029…`, section 5). Equal instrument tables are not equal galaxy datasets, and no dataset comparison was made for these runs.

### 8.2 Deltas

For each role: `python3 /Users/dcroton/Local/git-repos/campaigns/sage26/nodes/i02-alignment-set/summarize.py delta output/i02/all/compare-r6.json output/i02/<label>/compare-r6.json`. The base is the `all` role's label `all` (section 2). Output: `output/i02-analysis/<role>-delta-vs-all.txt`. The committed copies sit in `measurement/deltas/<role>-delta-vs-all.txt` beside this file and are byte-identical (`cmp` exit 0 for each; `output/i02-analysis/slice2/deltas.txt`). Figures read from the runner's `compare-r6.json` and from the delta files with `output/i02-analysis/slice2/extract.py` (output `output/i02-analysis/slice2/extract.txt`).

### 8.3 Contribution at R6, Left Out

Denominators: 67 float properties × 8 T1 snapshots = 536 float property-snapshots; 73 compared properties (67 float, 6 integer) × 8 = 584 property-snapshot lines. `matched` in `all` at snapshots 63, 37, 32, 27, 23, 20, 18, 16 is 36534, 37203, 35882, 30551, 22022, 13543, 8195, 3908, each equal to `n_ref` and `n_cand`. Failed thresholds and bit-identical counts are totals over one run's eight snapshots, never across items. "Changed counts" are the `bit_identical`, `within_tolerance`, `within_tolerance_sig` and `exact_equal` changes on the delta's lines. Every one is a decrease, in every role (`count_changes_up=0`). `M_sig` is given as the number of delta lines on which it changed, with the largest absolute change and the direction.

| Item left out | Failed thresholds (`all` 0) | Float bit-identical on every pair (`all` 536/536) | Integer bit-identical (`all` 48/48) | Changed prop-snapshot lines | Snap 63 `n_ref` / `n_cand` / `matched` (unmatched ref, cand) | Lines where `matched` changed | `M_sig` changed: lines (largest; direction) | Changed counts (all decreases) |
|---|---|---|---|---|---|---|---|---|
| 01 particle mass | 250 | 152/536 | 48/48 | 384/584 | 36534 / 36534 / 36534 (0, 0) | 0 | 74 (22; all up) | 1151 |
| 02 cosmic time | 0 | 305/536 | 48/48 | 231/584 | 36534 / 36534 / 36534 (0, 0) | 0 | 0 | 407 |
| 03 written `dT` | 0 | 532/536 | 48/48 | 4/584 | 36534 / 36534 / 36534 (0, 0) | 0 | 0 | 4 |
| 04 substep count | 180 | 213/536 | 48/48 | 323/584 | 36534 / 36534 / 36534 (0, 0) | 0 | 41 (16; all up) | 969 |
| 05 float halo state | 14 | 242/536 | 48/48 | 294/584 | 36534 / 36534 / 36534 (0, 0) | 0 | 15 (1; all up) | 876 |
| 06 inheritance resets | 0 | 536/536 | 48/48 | 0/584 | 36534 / 36534 / 36534 (0, 0) | 0 | 0 | 0 |
| 07 written halo values | 0 | 536/536 | 48/48 | 0/584 | 36534 / 36534 / 36534 (0, 0) | 0 | 0 | 0 |
| 08 CGM useful draw | 116 | 219/536 | 40/48 | 325/584 | 36534 / **36535** / 36534 (0, **1**) | 0 | 173 (426; all up) | 967 |
| 09 creation draws | 114 | 221/536 | 40/48 | 352/584 | 36534 / 36534 / **36533** (**1**, **1**) | 73 (snapshot 63) | 198 (447; 167 up, 31 down) | 1043 |
| 10 FFB discarded draw | 118 | 217/536 | 40/48 | 356/584 | 36534 / **36533** / **36532** (**2**, **1**) | 73 (snapshot 63) | 212 (440; 181 up, 31 down) | 1055 |

Notes on the table:

- **Denominator changes.** At snapshots 37 to 16 every run has `matched = n_ref = n_cand`, equal to `all`'s. At snapshot 63, three runs differ from `all` (counts in bold):
  - Without 08, the candidate has one more row (`n_cand` 36535) and one row is unmatched; `matched` stays 36534.
  - Without 09, one row is unmatched on each side, so `matched` is 36533 over `n_ref = n_cand` = 36534. Equal row counts are not equal row sets.
  - Without 10, two reference rows and one candidate row are unmatched, so `matched` is 36532.
  Every snapshot-63 count in those two runs (09 and 10) is over a changed `matched`, which the 73 snapshot-63 lines of each delta carry. An unchanged count over a changed denominator is a changed fraction.
- **Without 03:** only `dT` changes, at snapshots 63, 32, 23 and 20: `bit_identical` 36534→336, 35882→35076, 22022→0 and 13543→13347, over unchanged `matched` and `M_sig`.
- **The integer property** not bit-identical without 08, 09 or 10 is `Regime`, at every snapshot. Galaxies whose `Regime` differs (`matched` − `exact_equal` for `Regime`) at snapshot 63: 748 of 36534 without 08, 758 of 36533 without 09, 773 of 36532 without 10.
- **Without 06 or 07:** no line changes.

### 8.4 Per Snapshot at R6, Left Out

Cells: `failed_thresholds, float_bit_identical/67` of the left-out run (`all` is `0, 67` at every snapshot). Denominator `matched` per snapshot, `all`'s: 36534, 37203, 35882, 30551, 22022, 13543, 8195, 3908. The left-out runs' `matched` equals it everywhere except at snapshot 63 without 09 (36533) and without 10 (36532). Without 08, snapshot 63 has `n_cand` 36535 and `matched` 36534.

| Item left out | 63 | 37 | 32 | 27 | 23 | 20 | 18 | 16 |
|---|---|---|---|---|---|---|---|---|
| 01 | 63, 17 | 54, 17 | 48, 17 | 36, 17 | 17, 19 | 13, 22 | 9, 21 | 10, 22 |
| 02 | 0, 29 | 0, 28 | 0, 33 | 0, 33 | 0, 41 | 0, 44 | 0, 46 | 0, 51 |
| 03 | 0, 66 | 0, 67 | 0, 66 | 0, 67 | 0, 66 | 0, 66 | 0, 67 | 0, 67 |
| 04 | 40, 24 | 52, 24 | 53, 24 | 22, 24 | 7, 25 | 3, 27 | 3, 26 | 0, 39 |
| 05 | 12, 21 | 0, 24 | 0, 25 | 0, 25 | 1, 28 | 0, 31 | 1, 41 | 0, 47 |
| 06 | 0, 67 | 0, 67 | 0, 67 | 0, 67 | 0, 67 | 0, 67 | 0, 67 | 0, 67 |
| 07 | 0, 67 | 0, 67 | 0, 67 | 0, 67 | 0, 67 | 0, 67 | 0, 67 | 0, 67 |
| 08 | 48, 24 | 17, 24 | 16, 24 | 15, 24 | 9, 25 | 5, 30 | 3, 31 | 3, 37 |
| 09 | 48, 24 | 17, 24 | 16, 24 | 15, 25 | 9, 25 | 3, 30 | 3, 31 | 3, 38 |
| 10 | 49, 24 | 17, 24 | 16, 24 | 15, 25 | 11, 25 | 4, 30 | 3, 31 | 3, 34 |

The row for 10 is Slice 1's `nine` row (section 4).

### 8.5 Beside It: The Same Item at R4 and at R0

Quoted from x02's report at `8fd27634` (`campaign-reports/x02-rng-alignment.md`, SHA-256 `0794079a368328dd024c2d510c78b2dd2c120cfadce41ceb145807ea54570293`) and x01's report at `a3063d58` (`campaign-reports/x01-core-alignment.md`, SHA-256 `013eb569d6b1460a3637926a6739d22d60e3a473e7fa5b33a59a0e396848947e`), both read with `git show` (copies `output/i02-analysis/slice2/x02-report.md` and `x01-report.md`, digests checked). Each cell: failed thresholds; float property-snapshots bit-identical on every pair; changed property-snapshot lines; snapshot 63 `unmatched_cand`, as the report's table gives them. R6 is this slice's (8.3). R4's base is x02's `all` (0 failed, 536/536, at R4; its `matched` at snapshot 63 is 36533, x02 section 7.4). R0's base for "left out" is x01's `all` (0 failed, 536/536); for "alone" it is x01's `base` (260 failed, 214/536). x01 section 7.4 says that in each of its runs `matched` equals `n_ref` at every snapshot and never changes.

| Item | R6 left out (this slice) | R4 left out (x02 §11.3 for 01–07) | R0 left out (x01 §7.4) | R0 alone (x01 §7.4) |
|---|---|---|---|---|
| 01 (h) particle mass | 250; 152/536; 384/584; 0 | 0 → 249; 536/536 → 156/536; 380/584; 0 → 0 | 0 → 177; 536/536 → 229/536; 307/584; 0 → 1 | 260 → 151; 214/536 → 257/536; 304/584; 1 → 0 |
| 02 (d) cosmic time | 0; 305/536; 231/584; 0 | 0 → 0; 536/536 → 315/536; 221/584; 0 → 0 | 0 → 0; 536/536 → 337/536; 199/584; 0 → 0 | 260 → 260; 214/536 → 214/536; 184/584; 1 → 1 |
| 03 (c) written `dT` | 0; 532/536; 4/584; 0 | 0 → 0; 536/536 → 532/536; 4/584; 0 → 0 | 0 → 0; 536/536 → 532/536; 4/584; 0 → 0 | 260 → 260; 214/536 → 218/536; 4/584; 1 → 1 |
| 04 (a) substep count | 180; 213/536; 323/584; 0 | 0 → 181; 536/536 → 213/536; 323/584; 0 → 0 | 0 → 142; 536/536 → 279/536; 257/584; 0 → 0 | 260 → 177; 214/536 → 224/536; 253/584; 1 → 1 |
| 05 (b) float halo state | 14; 242/536; 294/584; 0 | 0 → 13; 536/536 → 236/536; 300/584; 0 → 0 | 0 → 10; 536/536 → 291/536; 245/584; 0 → 0 | 260 → 260; 214/536 → 214/536; 156/584; 1 → 1 |
| 06 (e) inheritance resets | 0; 536/536; 0/584; 0 | 0 → 0; 536/536 → 536/536; 0/584; 0 → 0 | 0 → 0; 536/536 → 536/536; 0/584; 0 → 0 | 260 → 260; 214/536 → 214/536; 0/584; 1 → 1 |
| 07 (f) written halo values | 0; 536/536; 0/584; 0 | 0 → 0; 536/536 → 536/536; 0/584; 0 → 0 | 0 → 0; 536/536 → 536/536; 0/584; 0 → 0 | 260 → 260; 214/536 → 214/536; 0/584; 1 → 1 |
| 08 (i) CGM useful draw | 116; 219/536; 325/584; 1 | §7.3: `delta all item-creation-draws` (`all` without 08) `changed_prop_snapshots=348`; §14.1: "Left out of `all` (`item-creation-draws`, which equals `series`): 348 lines change, and every T1 snapshot loses bit-identity (section 7.3)" | not measured (x01 holds 01–07 only) | not measured |
| 09 (i) creation draws | 114; 221/536; 352/584; 1 | §7.3: `delta all item-useful-draw` (`all` without 09) `changed_prop_snapshots=316`; §14.2: "Left out of `all` (`item-useful-draw`): 316 of 584 property-snapshot lines change on the T1 snapshots, z = 0 total `StellarMass` +0.090601% (sections 7.3 and 13.2)" | not measured | not measured |
| 10 (i) FFB discarded draw | 118; 217/536; 356/584; 1 | not measured (the draw first executes at R6; not in x02) | not measured | not measured |

`M_sig` beside them, quoted: R4 (x02 §11.3) changed on 67 lines (largest change 20) for (h), 0 for (d) and (c), 33 (largest 15) for (a), 14 (largest 2) for (b), and 0 for (e) and (f). At R4, `matched` is unchanged on every line of every delta, and `matched = n_ref = n_cand` at every T1 snapshot of every `loo-x01` run. R0 (x01 §7.4): `matched` changed on 0 lines; `M_sig` on 59 lines (largest 16) for (h), 0 for (d), 0 for (c), 53 (largest 4) for (a) and 22 (largest 16) for (b). R6 (8.3): 74 (22) for 01, 0 for 02 and 03, 41 (16) for 04, 15 (1) for 05, 0 for 06 and 07, 173 (426) for 08, 198 (447) for 09, 212 (440) for 10.

For 08 and 09 at R4, x02 §7.3 says "each run's per-snapshot figures are its column in 7.1 (`item-creation-draws` equals `series`)". From those columns, totalled here over one run's eight snapshots (x02 gives no total):
- Without 08 (`series`'s column): 116 failed thresholds; 225/536 float property-snapshots bit-identical on every pair; at snapshot 63 `matched` 36532 of `n_ref` 36533 and `n_cand` 36535 (unmatched 1 and 3).
- Without 09 (`item-useful-draw`'s column): 118 failed thresholds; 228/536; at snapshot 63, 36533 matched with none unmatched.
x02 §7.3's example at snapshot 63 is `StellarMass` `bit_identical` 36533→31966 without 09 and 36533→32013 without 08. The same delta lines here (R6) are 36534→32002 without 09 (`matched` 36534→36533) and 36534→32013 without 08 (`matched` unchanged).

**Attribution.** The three columns differ in more than the rung. R0 is x01's base (the integration head `b0071b07`) against SAGE26's R0 reference, with x01's seven items around the item. R4 is x02's launch base `5e91c808` against R4, with the nine. R6 is this node's launch base `66a3e949` (c13's package state, with the FFB classifier) against R6, with the ten. So a difference between columns is not attributed to any one cause: not to the rung, not to the package state, not to the base, and not to the set around the item.

### 8.6 The Random-Number Items Beside SAGE26's Own Sensitivity

SAGE26's own sensitivity is campaign evidence E24, measured by e06 on SAGE26 itself in the T1 configuration. It is quoted here as this node's plan quotes it (Slice 2), and the node did not re-measure it. Beside it, the R6 left-out effects of 08, 09 and 10 (8.3, and `output/i02-analysis/slice2/rng-percent.txt`). **The two sides differ in configuration.** E24 compares SAGE26 with SAGE26 under one perturbation. The R6 columns compare the package without one item with SAGE26's R6 reference. The table is descriptive only. E24 reports dataset counts (of 5250) and total stellar mass; the instrument measures neither, so those cells say "not measured here". Percentages in the R6 columns are computed from the counts beside them.

| Measure | E24: SAGE26 against itself (quoted) | R6, package without 08 | R6, package without 09 | R6, package without 10 |
|---|---|---|---|---|
| Perturbation | one extra `rand()` at the start of `main()`; separately, the two creation draws removed; separately, a build without FMA contraction | the CGM useful draw left out (CGM classifier draws nothing from the stream) | the two creation draws left out | the FFB discarded draw left out |
| Datasets changed | extra `rand()`: 2032 of 5250; creation draws removed: 2901 of 5250 | not measured here (325 of 584 property-snapshot lines changed) | not measured here (352 of 584) | not measured here (356 of 584) |
| Failed thresholds | creation draws removed: 114 | 116 (T1's eight snapshots) | 114 | 118 |
| `Regime` differs at snapshot 63 | extra `rand()`: 102 of 36534 | 748 of 36534 | 758 of 36533 | 773 of 36532 |
| `HotGas` `M_sig` gate | extra `rand()`: failed at snapshots 63 to 18; 1824/2137, 85.4%, at 63 | failed at 63 to 16 (all eight); 14/2503 (0.6%) at 63 | failed at 63 to 16; 11/2529 (0.4%) | failed at 63 to 16; 9/2522 (0.4%) |
| `MetalsHotGas` `M_sig` gate | extra `rand()`: failed at 63 to 18 | failed at 63 to 16; 13/2503 at 63 | failed at 63 to 16; 9/2529 | failed at 63 to 16; 9/2521 |
| `mdot_stream` `M_sig` gate | extra `rand()`: failed at 63 to 18; 243/315, 77.1%, at 63 | failed at 63 to 16; 0/493 (0.0%) at 63 | failed at 63 to 16; 1/503 (0.2%) | failed at 63 to 16; 1/506 (0.2%) |
| `mdot_cool` `M_sig` gate | extra `rand()`: failed at 63 to 32 | failed at 63 to 23; 8/2131 at 63 | failed at 63 to 23; 7/2144 | failed at 63 to 23; 5/2142 |
| `SfrBulge` `M_sig` gate | no FMA contraction: failed at 63; 175/202 | failed at 63 to 27; 87/224 (38.8%) at 63 | failed at 63 to 27; 90/224 (40.2%) | failed at 63 to 23; 89/217 (41.0%) |
| `QuasarModeBHaccretionMass` `M_sig` gate | no FMA contraction: failed at 63; 280/324 | failed at 63 to 23; 146/348 (42.0%) at 63 | failed at 63 to 23; 148/343 (43.1%) | failed at 63 to 23; 145/343 (42.3%) |
| z = 0 total stellar mass | creation draws removed: +0.99% | not measured here | not measured here | not measured here |

"Failed at 63 to 16" means at each of T1's eight snapshots 63, 37, 32, 27, 23, 20, 18, 16; "63 to 23" means at 63, 37, 32, 27 and 23; and "63 to 27" means at 63, 37, 32 and 27. In the R6 columns the fraction is `within_tolerance_sig` over `M_sig` at snapshot 63 from the left-out run's `compare-r6.json`. In `all`, these properties' `M_sig` at snapshot 63 are 2082 (`HotGas`, `MetalsHotGas`), 282 (`mdot_stream`), 1797 (`mdot_cool`), 202 (`SfrBulge`) and 324 (`QuasarModeBHaccretionMass`), each with `within_tolerance_sig` equal to `M_sig` (`output/i02-analysis/slice2/all-msig-63.txt`).

No conclusion about T1's tolerances is drawn from this table (campaign decision D13), and no change to T1's comparison rules, thresholds, field map or uncompared list is proposed (campaign decision D15).
