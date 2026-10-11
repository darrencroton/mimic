# Shin-Uchuu Version 3 — Acceptance Record

**Status:** Started 2026-10-08 by Slice 5 of [`MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md`](MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md) (F11: one section per stage). This file records measurements; it is not a plan and makes no claim beyond the runs below. Every number here is quoted from a log named beside it; the logs live outside the repository, under `/Volumes/Internal/results/mimic/shin-uchuu-v3-stage-a/` (where `output/` resolves on this host, abbreviated `$A` below) and the LaCie conversion workdir.

**Host** (`$A/logs/host.log`): macOS (Darwin 27.0.0), Mac Studio (`Mac15,14`), 32 cores, 512 GiB RAM; Open MPI 5.0.9; HDF5 1.14.6; `mimic_venv` Python 3.14.6 with h5py 3.15.1 and numpy 2.3.4.

---

## Stage A — micro-Uchuu on the forest-blocked ASCII route

**Code under test.** The conversion, the C dump and both harness comparisons ran at converter commit `6ae714829a3b68f934e88ed1464a97532e671f27` (branch `feature/shin-uchuu-v3`, Slice 4's last commit; `$A/logs/convert-00-provenance.log` records the commit and the start time, 2026-10-08T05:48:49Z). The harness record `$A/harness-record.json` takes the commit, a count of `git status --porcelain` lines and the harness's own SHA-256 after each command ends. All four entries record `6ae71482` and harness SHA-256 `50311f52c78cdd75b1e49b7d62bc576d90954da63bf29fed1a133cfa4f8cf67e`, the digest of the harness file at `6ae71482`.

**Working-tree state.** Only `build-dump` (ended 05:49:10Z) records `git_dirty_paths 0`. `dump` (ended 05:51:04Z), `compare` (ended 06:03:50Z) and `compare-extras` (ended 06:05:23Z) each record `git_dirty_paths 5`. The five entries are this slice's own edits, all made between 05:49:48Z and 05:50:59Z (file times in `$A/logs/harness-tree-state.log`) and committed unchanged as `2ac07bfe` at 06:09:57Z. A `git status --short` taken in this session at 06:06:18Z, just before installation, showed exactly these five entries and nothing else; that check is in no log:

- modified: `simulations/micro-uchuu-ascii-horizontal/halo_properties.yaml`;
- modified: `simulations/micro-uchuu-ascii-horizontal/_tests/scientific/test_cross_format_identity.py`;
- modified: `simulations/micro-uchuu-ascii-horizontal/_tests/unit/test_unit_horizontal_reader_realdata.c`;
- untracked: `models/sham/input/sham_micro-uchuu-ascii-horizontal-realdata.yaml`;
- untracked: the new directory `simulations/micro-uchuu-ascii-horizontal/_tests/integration/`, holding `test_schema_conformance.py` (porcelain reports the directory as one entry).

None of the five can affect the conversion, the C dump or either comparison:

- the converter reads `convert/mimic-convert/` and `simulations/micro-uchuu-ascii/`, which no commit of this slice touches (`git diff --name-only 6ae71482 HEAD` is in the same log);
- the harness is pinned by its recorded SHA-256;
- the dump tool was built for `halos-only` × `micro-uchuu-ascii` by `build-dump` while the record shows `git_dirty_paths 0`, and the dump reads only that package and `$A/dump_run.yaml`;
- the five paths belong to the horizontal package, its tests and a SHAM run file, which none of these steps reads.

The conversion stages after `inspect` overlapped the edits (`ingest` started 05:49:44Z), but they read only those unchanged files, and the converter writes no tree state of its own.

**Later commits.** The parity gate and the distributed, chunked and SHAM legs ran at `2ac07bfe37fcf4c6ef8c29deb80dac4aa67a9e24`, Slice 5's first commit. It changes only the package's declarations, its tests and the new SHAM run file on top of `6ae71482`, with no converter, reader or driver source.

Slice 5's second commit, `eb5c950f10c00c94c524138b1b39f9b793a27d67`, touched only:

- the package README's prose;
- this record;
- the expected-message needles of the eight link cases in `test_unit_horizontal_reader_open.c`;
- one comment line of the package's `simulation_info.yaml`.

No run above reads any of them, so no leg was re-run at it. `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-unit tests-integration` passed (`Unit Test Summary: passed=316 failed=0 skipped=0 n/a=18`, `Integration Test Summary: passed=116 failed=0 skipped=0 n/a=16`, `$A/logs/tiers-unit-integration-2.log`). It ran on the working tree that `eb5c950f` then committed, and the log does not name a commit. That tree differed from the commit only by the formatter's later re-wrap of one link case, which changes no string; the record's own prose changed too, and no test reads it. A third commit corrected four statements of this record and changed nothing else.

### Conversion

Source: `simulations/micro-uchuu-ascii/snapshots` → `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii/` (`tree_0_0_0.dat`, 11,515,537,257 bytes, 22,580,924 rows, 561,266 `#tree` markers, per `$A/logs/convert-01-inspect.log`), with the package's profile `simulations/micro-uchuu-ascii/converter_columns.yaml` (`column_mapping_sha256 727d13f529fa80305f261b933612b6087aa52ade5dde7899bb18f8f4450f8f6d`, no extra fields; `identity_scheme forest-rank`). Workdir `/Volumes/LaCie/data/uchuu/micro-uchuu/stage-a-v3-workdir` (kept, with its `manifest.json`, `configuration_sha256 417738812775f4d8f823896cbc06bb73f032674e079272462bd6c0782a631253`); battery spill `/Volumes/LaCie/data/uchuu/micro-uchuu/stage-a-v3-spill`. Default memory budget (2048 MiB), `pool_size` 1 (the adapter parameters in `$A/logs/convert-01-inspect.log`). The commands are the `convert_trees.py` sequence in the package README with `--spill-dir` added to `validate` and `report`.

| Stage | Exit | Wall-clock | Peak RSS | Log |
|---|---|---|---|---|
| `inspect` | 0 | 55.15 s | 0.15 GB | `$A/logs/convert-01-inspect.log` |
| `ingest` | 0 | 289.08 s | 4.52 GB | `$A/logs/convert-02-ingest.log` |
| `transpose` | 0 | 106.14 s | 3.71 GB | `$A/logs/convert-03-transpose.log` |
| `write` | 0 | 27.48 s | 0.88 GB | `$A/logs/convert-04-write.log` |
| `validate` | 0 | 33.79 s | 0.71 GB | `$A/logs/convert-05-validate.log` |
| `report` | 0 | 32.95 s | 0.77 GB | `$A/logs/convert-06-report.log` |

Peak RSS is `/usr/bin/time -l`'s maximum resident set size (GB = 10⁹ B).

**Dataset** (`report`, `<workdir>/conversion_report.{txt,json}`): 22,580,924 halos in 50 snapshot files, all populated; 440,651 forests; `links_adjacent 1` (measured 1), 0 gapped `Descendant` links, longest span 1; `SourceHaloID` in [1, 22,580,924]; maximum `ForestIndex` 440,650; maximum rank 350,074; largest snapshot 27 with 621,360 halos; 0 `Len == 0` halos; 3,364,750,153 bytes emitted (149.01 B/halo). Forest blocking is recorded by the report's identity conventions (`source_halo_id`: "1-based position in (ForestIndex, HaloRankInForest) order") and by its battery outcome `identity: PASS`, the check that, for an unsampled ASCII dataset, every halo's `SourceHaloID` is exactly that position (`validate_v3.py`, `_V3_POSITION_IDENTITY`).

**Battery verdict** (`validate`, `$A/logs/convert-05-validate.log`): `validation: PASS`, all 20 checks PASS: file-set, object-set, sidecar-object-set, schema-binding, manifest-binding, header-values, run-scoped-headers, row-values, len-nonnegative, field-finiteness, position-bounds, link-targets, links-adjacent, topology-closure, chain-cycles (10 pointer-jumping rounds), source-key-coverage, identity, header-bounds, sidecar-content, count-conservation. `report` re-ran the same 20 checks, all PASS, `validation: PASS` (`<workdir>/conversion_report.{txt,json}`, `$A/logs/convert-06-report.log`).

**Digests.** `forests.h5` SHA-256 `2addb65076291cba9831d867eae850cbed905c11340c5c71d556972a81d10203`. The 51 files' SHA-256 values are listed in `$A/logs/install-sha256-installed.txt` (that list's own SHA-256 is `73189046aff714709204debbfae0106278108bd35c355b6bf9cf60d71d572149`), and every one equals the workdir manifest's recorded `sha256` for the same `write/attempt_001/` artefact.

### Harness verdicts

`convert/mimic-convert/tests/run_generalisation_acceptance.py`, record `$A/harness-record.json`, child logs under `$A/logs/harness/`.

| Subcommand | Verdict | Wall-clock | Peak RSS | Report |
|---|---|---|---|---|
| `build-dump` (`halos-only` × `micro-uchuu-ascii`) | exit 0 | 3.6 s | 0.05 GB | `$A/logs/harness-01-build-dump.log` |
| `dump` (`--source-payload`, run file `$A/dump_run.yaml`: the committed `halos-only_micro-uchuu-ascii.yaml` with `output_format: binary` and a scratch output directory) | exit 0 | 109.4 s | 0.76 GB | `$A/micro-uchuu-ascii.dump` (3.8 GB, `$A/logs/host.log`) |
| `compare` against the C dump | **PASS**, `failed_checks []` | 334.3 s | 0.79 GB | `$A/compare-report.json` |
| `compare-extras` against independent extraction | **PASS**, `failed_checks []` | 91.9 s | 0.50 GB | `$A/compare-extras-report.json` |

`compare` matched 22,580,924 converted rows to 22,580,924 reference rows with 0 failures in each of its 25 findings: the five links, the three target snapshots, `snapnum`, the eight payload fields and payload storage, row coverage, both duplicate checks, dump integrity, converter link encoding and targets, and **`source_halo_id` applicable and passing** ("SourceHaloID is the 1-based (ForestIndex, rank) position", 22,580,924 compared, 0 failures). `compare-extras` (the shipped zero-extras profile) matched 22,580,924 rows on the catalogue key (`SnapNum`, `MostBoundID`) with 0 failures in row coverage and both duplicate checks; `source_identity` is not applicable for ASCII by design ("its SourceHaloID binding is compare's source_halo_id finding").

### Installation

The dataset was installed by copying `<workdir>/write/attempt_001/{snapshot_*.h5,forests.h5}` (51 files, `cp -p`) to `/Volumes/LaCie/data/uchuu/micro-uchuu/micro-uchuu-ascii-horizontal-v3` and repointing the gitignored symlink `simulations/micro-uchuu-ascii-horizontal/snapshots` from `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii-horizontal` to it (`$A/logs/install-symlink.log`). The copy's SHA-256 list is byte-identical to the workdir's (`$A/logs/install-sha256-workdir.txt`, `$A/logs/install-sha256-installed.txt`).

**Retained version 2 dataset (for Slice 16's G3; Slice 15 before plan revision 6):** `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii-horizontal` (50 snapshot files and `forests.h5`), untouched.

### Parity gate

`make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-scientific` (log `$A/logs/gate-halos-only.log`, exit 0, 414.80 s wall-clock including the tier's own build and other scientific tests) and the same with `MODEL=sage16` (log `$A/logs/gate-sage16.log`, exit 0, 428.58 s). Both runs of the gate built every worktree at `2ac07bfe` and passed every stage. Stage 2 confirmed 50 version 3 files from `consistent_trees_ascii` with the pinned digest, `links_adjacent 1`, 22,580,924 halos, 0 gapped links, and a conversion inventory of 440,651 forests from source file 0, exactly the vertical side's. The comparator's PASSED line for each leg, from `gate-halos-only.log` (identical in `gate-sage16.log`):

| Leg | Comparator line |
|---|---|
| `halos-only` fixed | `PASSED: 4409643 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates` |
| `halos-only` dynamic | `PASSED: 4409643 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates` |
| `sage16` fixed | `PASSED: 3112186 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates` |
| `sage16` dynamic | `PASSED: 3112152 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates` |

Output snapshots 7, 8, 10, 12, 16, 23, 28 and 49; the vertical side wrote 5 partition files and the horizontal side 8 in every leg (the multi-partition check). **Stage 8** (vertical-path preservation against `aedded2f`): 4,409,643 galaxy records byte-identical to the baseline; 6 HDF5 files walked, 19 excluded provenance attribute differences, the one permitted delta (`UniqueGalaxyID description`, 6 occurrences) observed and nothing else; `output_schema.json` differs in exactly `.fields[UniqueGalaxyID].description`. Each tier ended `Scientific Test Summary: passed=35 failed=0 skipped=0 n/a=0 warned=1`; the warning is the tier's generic `test_zero_values` check (two `Mvir = 0.0` halos), not a gate stage.

### Distributed and chunked legs

Per model, from the shipped run file `models/<model>/input/<model>_micro-uchuu-ascii-horizontal.yaml` with only `output.output_directory` changed (and `input.forest_chunks: 4` for the chunked run), at `2ac07bfe`. The serial reference is a non-MPI production build (`TEST_BUILD=no`, `USE-MPI=`), written to `archive/distributed-references/<model>-ascii/` with `COMMIT.txt` (`2ac07bfe…`, 2026-10-08), `make_info.txt` (MPI disabled), `run.log` and `time.txt`. The `-np 4` run is a `USE-MPI=yes` build under `mpirun --oversubscribe -np 4`. Each run was compared with its serial reference by `scripts/compare_cross_format_identity.py`. Logs under `$A/logs/legs/`.

| Model | Run | Comparator line | Wall-clock | Peak RSS |
|---|---|---|---|---|
| `halos-only` | serial reference | — | 7.05 s | 5.006 GB |
| `halos-only` | serial, `forest_chunks: 4` | `PASSED: 4409643 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates` | 6.98 s | 2.500 GB |
| `halos-only` | `-np 4` | `PASSED: 4409643 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates` | 2.73 s | not measured per rank |
| `sage16` | serial reference | — | 39.93 s | 2.475 GB |
| `sage16` | serial, `forest_chunks: 4` | `PASSED: 3112186 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates` | 38.93 s | 0.684 GB |
| `sage16` | `-np 4` | `PASSED: 3112186 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates` | 13.76 s | not measured per rank |

Wall-clock and peak RSS are `/usr/bin/time -l` (`<model>-<run>-time.txt`); for `-np 4` it timed the `mpirun` launcher, so only its wall-clock is quoted. The driver's startup forest-blocking check (the scan of every slab's `ForestIndex`, which aborts with "is not forest-blocked" otherwise) passed in every distributed and chunked run; its headline, logged only after the scan of all 50 slabs:

- `-np 4` (`halos-only-np4-run.log`, `sage16-np4-run.log`): `task 0: Distributed horizontal partition: 440651 forests over 4 tasks, weighted by the widest slab, snapshot 27 (621360 halos)`
- `forest_chunks: 4` (`halos-only-chunks4-run.log`, `sage16-chunks4-run.log`): `Chunked horizontal partition: 440651 forests over 1 task in 4 chunks each, weighted by the widest slab, snapshot 27 (621360 halos)`

### SHAM leg (version 2 against version 3)

One `MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal` non-MPI production build at `2ac07bfe` (`$A/logs/sham/make_info.txt`, `commit.txt`) ran two run files: the committed `models/sham/input/sham_micro-uchuu-ascii-horizontal-realdata.yaml` on the installed version 3 dataset, and the scratch `$A/sham/sham_micro-uchuu-ascii-horizontal-v2.yaml` (the same file with `simulation.config` set to `$A/sham/simulation_info_v2.yaml`, the package's `simulation_info.yaml` with `input.simulation_dir` set to the retained version 2 dataset, and a scratch output directory). Each run logged its dataset (`format_version 2` and `format_version 3` respectively, 440,651 forests, max rank 350,074) and the same audit line, `SHAM audit z=0.0005 candidates=74596 assigned=74596 masked=0` (`$A/logs/sham/v2-run.log`, `v3-run.log`).

| Run | Wall-clock | Peak RSS |
|---|---|---|
| version 2 | 5.84 s | 2.241 GB |
| version 3 | 5.93 s | 2.306 GB |

Comparator (`$A/logs/sham/compare.log`, exit 0): `PASSED: 557669 galaxies over 1 output snapshot(s) are bitwise identical in all 24 field(s), with identical UniqueGalaxyID sets and no duplicates`. A module that ranks and breaks ties on `UniqueGalaxyID` therefore gives byte-identical output on the version 2 dataset and on its forest-blocked version 3 reconversion. HOD's leg waits for Slice 14's lineage comparator.

---

## Stage B — the forest census and the decided cut table

Stage B's measurements and the owner's decision are recorded in [`MIMIC-SHIN-UCHUU-FOREST-CENSUS.md`](MIMIC-SHIN-UCHUU-FOREST-CENSUS.md), the census record (F11); this section only points to it and names how Stage B ran.

- **Run 2** (2026-10-08/09) accepted Slices 7 and 8 (the census's readers, occupancy, effective trees, partition simulation, and the candidate-rule machinery over the co-membership graph). Its Slice 9 measured the production census up to the graph and a rule preview, and was **stopped** on 2026-10-09 while the twelve-rule candidate `cut` ran, when the owner decided the cut rule: every forest is cut at its z = 0 FoF groups.
- **Revision 5** of the implementation plan (commit `40a448d5`) recorded that decision (F6, "The decided table"), retired the candidate-rule exploration (the Final-State Inventory), and rewrote Slice 9.
- **Run 2b** ran the revised Slice 9: the decided table for the whole simulation and its measured cost, the micro-Uchuu rehearsal on the version 3 and version 2 datasets, the production census reusing run 2's `occupancy`, `trees` and `partition` aggregates after four checks, and the census record with its two empty decision slots. Run 2's retired `graph/` and candidate `cut/` outputs were moved aside, not deleted, to `/Volumes/LaCie/data/uchuu/shin-uchuu-census-run2-retired/`.

---

## Stage R — the subset conversions and the gates

Slice 16 of the implementation plan (revision 6). Every step ran detached under `MallocLargeCache=0 /usr/bin/time -l` through a step wrapper that records, in each log's header, the commit, `git status --porcelain`, the command, and `df` of both volumes; at the end it records the exit status, the used-KiB range of both volumes and the summed RSS sampled every 30 s, and appends a row to the ledger. Logs live under `/Volumes/Internal/results/mimic/shin-uchuu-v3-stage-r/slice-16/` (abbreviated `$R`; ledger `$R/LEDGER.md`, logs `$R/logs/`, step scripts `$R/scripts/`). Workdirs are under `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-r/` (abbreviated `$W`). Peak RSS is `/usr/bin/time -l`'s maximum resident set size (GB = 10⁹ B). One LaCie-bound job ran at a time. System free memory (`memory_pressure`) was at least 96%, about 490 GiB of the 512 GiB, in every sample of every step that recorded it. Two early steps, `ref-01-inspect` and `g3c-fixture-leg`, ran under an older wrapper that sampled disk use but not memory or RSS. Their peak RSS is still `/usr/bin/time`'s, 0.104 GB and 0.342 GB.

**Code under test.** Every step ran at `a3d0ecee9f9e1a345551203d9ffdd0ed159cd572` (branch `feature/shin-uchuu-v3`), as each log's header records. Every step up to and including `rc-02-transpose` records a clean tree. From `rc-03-write` through `g4-subset-gate`, the headers show two modified files, `convert/mimic-convert/README.md` and `simulations/shin-uchuu-ascii/README.md`. From `run-sham-v2` on, they show three, this record being the third. All three are this slice's documentation, which no step reads. Mimic binaries were built once per (model, simulation, MPI) pair at that commit on a clean tree (`make clean`, `make generate`, `make TEST_BUILD=no [USE-MPI=yes] -j16`) and copied out of the tree with `make_info`, the commit and their SHA-256 (`$R/bin/<model>__<simulation>[__mpi]/`), so a later rebuild of the tree cannot change a run. The subset gate builds its own worktrees at the same commit.

**Sources.** micro-Uchuu: `simulations/micro-uchuu-ascii/snapshots` (`tree_0_0_0.dat`), its retained version 2 dataset `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii-horizontal` (Stage A, "Retained version 2 dataset") and Slice 5's installed version 3 dataset `/Volumes/LaCie/data/uchuu/micro-uchuu/micro-uchuu-ascii-horizontal-v3`. The subset: `simulations/shin-uchuu-ascii/snapshots` → `/Volumes/LaCie/data/uchuu/shin-uchuu-subset-ascii`, 2,744 tree files given to both converters in `locations.dat`'s FileID order 0–2743 (`$R/scripts/subset_tree_files.txt`, `$R/logs/subset-fileid-map.txt`: the natural `sort -V` order of the names), with the profile `simulations/shin-uchuu-ascii/converter_columns.yaml` (digest `727d13f5…`). Every version 3 `validate` and `report` (`convert_trees.py`) ran with an explicit `--memory-budget-mb` (8192 on the subset, 4096 on micro-Uchuu) and `--spill-dir` on the LaCie. On the subset every one ran with `--multiplier 20000000000`. The version 2 battery (`validate.py` and `convert_ctrees.py report` on the version 2 subset) ran with `--multiplier 20000000000` only: for a version 2 dataset `validate.py` ignores `--memory-budget-mb`, and neither command has a spill option. On micro-Uchuu they ran with the package's multiplier 10⁹, and the migrated dataset's `validate` ran once more at 2×10¹⁰; the contract asks for 2×10¹⁰ everywhere, and the slice's `validation.md` records that reading under Contract defects.

### micro-Uchuu G3: migrating the retained version 2 dataset

| Step | Exit | Wall-clock | Peak RSS | Log |
|---|---|---|---|---|
| `rewrite_trees.py migrate` (multiplier 10⁹) | 0 | 37.67 s | 0.251 GB | `$R/logs/micro-g3-01-migrate.log` |
| column-by-column comparison with Slice 5's dataset | 0 | 12.62 s | 0.103 GB | `$R/logs/micro-g3-02-compare.log` |
| `validate` (multiplier 10⁹) | 0 | 30.65 s | 3.144 GB | `$R/logs/micro-g3-03-validate.log` |
| `validate` (multiplier 2×10¹⁰) | 0 | 30.73 s | 3.143 GB | `$R/logs/micro-g3-04-validate-2e10.log` |
| `report` | 0 | 31.34 s | 3.143 GB | `$R/logs/micro-g3-05-report.log` |
| HOD on version 2, on (M), comparison | 0, 0, 0 | 8.88 s, 9.01 s, 56.72 s | 0.453, 0.487, 0.886 GB | `$R/logs/micro-g3-hod-*.log` |

- **(M) ≡ Slice 5's dataset.** The comparison script (`$R/scripts/compare_datasets.py`) compares the file sets, and per snapshot the object set, every `/halos` column (dtype, shape and values), every header attribute (value and dtype) and every `/schema` attribute, and then every sidecar dataset; with `--bytes` it also compares every file's SHA-256. Result `VERDICT: IDENTICAL`: 51 files, 22,580,924 rows, 1,000 column comparisons, 700 header attributes, 450 schema groups, the three sidecar datasets; 12 of 51 files byte-identical, the rest differing in HDF5 layout only.
- **Remap store.** The run's `rewrite_summary.json` records a peak of 59,587,424 B, which is 95.9 B per row of the widest slab (621,360 rows). The source is the ledger, which quotes it; the workdir was later trashed.
- **Battery.** Both `validate` runs and `report` give `validation: PASS` with all 20 checks passing, `manifest-binding`, `schema-binding` and `count-conservation` among them and none skipped.
- **HOD, version 2 against version 3.** One `hod × micro-uchuu-ascii-horizontal` build ran the committed `hod_micro-uchuu-ascii-horizontal.yaml` modules and parameters, writing every snapshot, on the retained version 2 dataset and on (M). `scripts/compare_cross_format_identity.py --match-created-by-lineage` reports: `PASSED: 22431070 galaxies over 50 output snapshot(s) are identical under the lineage comparator: all 21 field(s) bitwise except UniqueGalaxyID, the pairing key, and UniqueCentralGalaxyID, compared through the pairing, with identical key sets and no duplicates (tree rows by UniqueGalaxyID, created rows by lineage (MostBoundID, SnapNum, ordinal); 16379 created row(s) compared)`.
- **HOD, version 2 against Slice 5's dataset itself.** The same build and parameters were run on Slice 5's dataset, with only the dataset and output directory changed (`$R/runs/micro-hod/hod_s5.yaml`; 30.95 s, 0.487 GB; the run log reports `format_version 3`, 440,651 forests and max rank 350,074). The same comparator against the version 2 run gives the identical PASSED line: 22,431,070 galaxies identical under the lineage comparator, with 16,379 created rows compared (`$R/logs/micro-g3-hod-run-s5.log`, `micro-g3-hod-compare-s5.log`).

### The subset reference conversion

`convert_trees.py` over the 2,744 files, the Slice 10 profile, `--memory-budget-mb 8192`, workdir `$W/ref` on the LaCie (`$R/scripts/ref_convert.sh`).

| Stage | Exit | Wall-clock | Peak RSS | Log |
|---|---|---|---|---|
| `inspect` | 0 | 834.32 s | 0.104 GB | `$R/logs/ref-01-inspect.log` |
| `ingest` | 0 | 7,541.54 s | 9.993 GB | `$R/logs/ref-02-ingest.log` |
| `transpose` | 0 | 1,655.25 s | 7.126 GB | `$R/logs/ref-03-transpose.log` |
| `write` | 0 | 335.79 s | 3.594 GB | `$R/logs/ref-04-write.log` |
| `validate` | 0 | 548.28 s | 6.898 GB | `$R/logs/ref-05-validate.log` |
| `report` | 0 | 544.86 s | 6.901 GB | `$R/logs/ref-06-report.log` |

- **Wall-clock.** The six stages took 11,460 s (3.18 h) in all.
- **Inspect.** 406,668,896 rows, 8,000,198 `#tree` markers and 210,572,730,148 source bytes.
- **Ingest.** 426 chunks and 6,011,205 forests, with maximum rank 8,312,565. The rank pass ran within its 8 GiB budget in 4 sorted runs with 0 merge passes; its peak spill was 19,520,107,008 B, and the identity stores took 6,506,702,336 B.
- **Transpose.** The largest snapshot holds 9,006,294 halos.
- **Dataset.** `write/attempt_001` measured 56,416,836 KiB (142.1 B per halo) by `du -sk` right after `write`; the source is the ledger. In the volume trash, the same directory now measures 56,062,956 KiB.
- **Battery.** `validate` gives `validation: PASS`, 20 of 20 checks, and `report` the same.
- **Storage.**
  - The battery's spill was 74,648,504 KiB (76.4 GB), measured as the rise in LaCie use during `validate`; the spill directory is empty afterwards.
  - The workdir's own transient peak was during `transpose`: LaCie use rose 376,039,632 KiB (385.1 GB) above its value before `ingest`.
  - The volume's largest rise was 377,963,248 KiB (387.0 GB), during `validate`. That is the finished workdir plus the battery's spill, not the workdir alone.
  - The workdir measured 303,283,620 KiB after `report` (`ref-06-report.log`).

### The version 2 subset

`convert_ctrees.py` over the same files in the same order, multiplier 2×10¹⁰, consumptive deletion (`--consume-intermediates`) in `fixups`, `links` and `write` (not in `sort`, and `scatter` has none), `links --memory-budget-mb 8192`, `scatter --pool-size 8`, workdir `$W/v2-subset` (`$R/scripts/v2_convert.sh`).

| Stage | Exit | Wall-clock | Peak RSS | Log |
|---|---|---|---|---|
| `scatter` | 0 | 13,130.63 s | 1.984 GB | `$R/logs/v2-01-scatter.log` |
| `sort` | 0 | 568.37 s | 3.539 GB | `$R/logs/v2-02-sort.log` |
| `fixups` | 0 | 584.70 s | 3.515 GB | `$R/logs/v2-03-fixups.log` |
| `links` | 0 | 1,081.95 s | 8.984 GB | `$R/logs/v2-04-links.log` |
| `write` | 0 | 510.75 s | 4.066 GB | `$R/logs/v2-05-write.log` |
| `validate.py` | 0 | 111.62 s | 0.898 GB | `$R/logs/v2-06-validate.log` |
| `report` | 0 | 114.93 s | 1.101 GB | `$R/logs/v2-07-report.log` |

- **Wall-clock.** The conversion took 16,103 s in all (the seven `real` values sum to 16,102.95 s).
- **Scatter.** It was I/O-bound, with 2,681 s user and 591 s system time against its 13,131 s wall-clock. That is about 16 MB/s over the source, with source and workdir on the same disk, against the manual's 71.9 MB/s pooled figure. The figure is recorded as measured.
- **Battery.** `validate.py` gives `validation: PASS`, 15 of 15 checks.
- **Retained workdir.** It holds `hdf5/` (the dataset, 40,004,580 KiB), `manifest.json`, `scratch/`, `forest_index_table.npy`, `forest_max_snap.npy` and `conversion_report.{json,txt}`. It measured 40,462,472 KiB at the end of `report` (`v2-07-report.log`, 08:50:42Z on 2026-10-10), and 40,420,160 KiB by `du -sk` between 13:40Z and 13:45Z the same day, and again at 20:25:36Z. The 42,312 KiB fall came with no step writing to it; its cause is not established.

### The rewrites of the subset

Every rewrite ran under `rewrite_trees.py` with the package's `simulation_info.yaml`, a_list and index files, multiplier 2×10¹⁰, and its own scratch on the LaCie (`$R/scripts/subset_m.sh`, `$R/scripts/subset_ncc.sh`). The scratch peak is from a `du -sk` of the scratch sampled every 30 s (`$R/logs/<step>.dusamples`).

| Step | Exit | Wall-clock | Peak RSS (planned) | Remap store peak | Scratch peak | Log |
|---|---|---|---|---|---|---|
| (M) `migrate` of `$W/v2-subset/hdf5`, `--conversion-manifest $W/v2-subset/manifest.json`, output `$W/migrated` | 0 | 449.38 s | 2.092 GB (1.836 GB) | 864,236,928 B | 1,680,292 KiB | `$R/logs/m-01-migrate.log` |
| (N) `noop` of the reference, output `$W/noop` | 0 | 500.17 s | 2.076 GB (1.836 GB) | 864,236,928 B | 1,680,292 KiB | `$R/logs/n-01-noop.log`, `$R/logs/n-01-noop.rewrite_summary.json` |
| (C) `cut` of the reference under the subset's decided table, output `$W/cut` | 0 | 625.31 s | 4.170 GB (4.061 GB) | 1,296,188,984 B | 2,191,436 KiB | `$R/logs/c-01-cut.log` |

**The rewriter's measured bytes per halo** (from each run's `rewrite_summary.json` and the scratch samples, over 406,668,896 halos with a widest slab of 9,006,294 rows):

| | Remap store | Scratch (temporary) | Output dataset |
|---|---|---|---|
| (M) | 95.96 B per widest-slab row (formula 96 B); 2.13 B per halo | 191.0 B per widest-slab row; 4.23 B per halo | 56,062,956 KiB, 141.2 B per halo |
| (C) | 143.92 B per widest-slab row (formula 144 B); 3.19 B per halo | 249.2 B per widest-slab row; 5.52 B per halo | 56,090,604 KiB, 141.2 B per halo |

The scratch is emptied when a run completes. During (M), LaCie use rose by at most 57,240,024 KiB (58.6 GB), which covers the dataset and the scratch. A snapshot is written under a temporary name, verified and renamed, so at most one file is temporary in `dataset/` at a time.

**G3 on the subset.**

| Check | Result | Log |
|---|---|---|
| (M) against the reference, column by column | `VERDICT: IDENTICAL` over 71 files, 406,668,896 rows, 1,400 column comparisons, 980 header attributes, 630 schema groups and the sidecar's ForestID, SourceFileOrdinal and SourceUnitOrdinal (6,011,205 rows each). 9 of 71 files are byte-identical; the rest differ in layout only. | `$R/logs/m-02-g3-compare.log` |
| Per-tree inventory conservation | `rewriter.pass0.check_conservation` against the reference's record: 2,744 files, 0 disagreeing, 406,668,896 halos on both sides, verdict pass. The reference's `conversion_report.json` carries no per-file counts, so the record is its ASCII preparation manifest, whose `source_files` carry the route's parsed counts. (M)'s own check, against the version 2 manifest, also passes. The check can be reproduced from `$W/ref-manifest/ascii_preparation-manifest.json` (SHA-256 `fdf2e5f4b0410bde2edfab31f8a0004bc1361b72375af26660692ca9fa2d6144`), copied back out of the volume trash after the cleanup, together with `$W/migrated/tree_inventory.npy` and the package's `locations.dat`. | `$R/logs/m-03-conservation.log` |
| Root correspondence | 8,000,198 terminal roots, equal to the index's roots: pass. | `rewrite_summary.json` |
| `validate` and `report` on (M) | `validation: PASS`, 20 of 20 checks (543.91 s and 541.11 s, 6.893 GB each). | `$R/logs/m-04-validate.log`, `$R/logs/m-05-report.log` |

**G3b.** (N) of the reference against the reference gives `VERDICT: IDENTICAL` in every value over the same 71 files, columns, attributes and sidecar (`$R/logs/n-02-compare.log`). 9 of 71 files are byte-identical; the input was written by the converter, so the remainder differ in layout only.

**Spanning forests.** These were read with h5py (`$R/logs/sidecar-spanning.log`). The reference and (M) each hold 12,694 −1/−1 forests of 6,011,205, and the cut subset holds 827 of 7,206,195. The gate's stage 2 finds these are exactly the 12,694 that the index files place in several files.

### G3c: the cut

**micro-Uchuu.** Slice 9's two tables are the whole-simulation table (md5 `75ebf157…`) and the restricted table (md5 `1759395b…`), both confirmed in `$R/logs/micro-g3c-00-table-md5.txt`. Under each table:

- (C) of Slice 5's dataset ran and was validated.
- The route converted `micro-uchuu-ascii` under `ingest --cut-table` with the same table (`ingest`, `transpose`, `write`, `validate`).
- The two datasets were compared (`$R/logs/micro-g3c-<table>-0[1-7]-*.log`).

| Table | (C) | Route (`ingest` to `validate`) | Comparison |
|---|---|---|---|
| whole simulation | 44.34 s, 0.395 GB; 440,651 → 496,374 forests | 415.15 s in all, peak RSS 3.152 GB (`validate`; 1.958 GB before it) | `VERDICT: IDENTICAL` (22,580,924 rows; 12 of 51 files byte-identical) |
| restricted | 46.44 s, 0.392 GB | 416.36 s in all, peak RSS 3.155 GB (`validate`; 1.979 GB before it) | `VERDICT: IDENTICAL` |

All four validations exit 0. The per-snapshot promotions agree over 50 snapshots across the rewriter's summary, the route's preparation manifest (`cut_promotions`) and, for the whole table, the census (`$R/logs/micro-g3c-promotions.log`). The whole table promotes 216,922 halos and the restricted table 11,699, with none at the final snapshot.

**The subset's decided table.** The census ran over the reference dataset, with every subcommand exiting 0 within 30 s and 1.71 GB (`$R/logs/census-0[1-5]-*.log`, aggregate `$W/census-subset`):

- **Trees.** The root correspondence passes, and conservation passes against the reference's preparation manifest.
- **Table** (whole simulation). It has md5 `9d26873760092bf0268ee1e7d48203ce` (`$R/logs/census-06-table-md5.txt`) and is copied with its record and summary to `$W/cut-table/`.
  - Forests go from 6,011,205 to 7,206,195.
  - 290,679 forests are split, into 1,485,669 pieces; 1,194,990 of those pieces are fresh, and the split forests hold 213,890,405 halos.
  - The table is non-trivial.
- **Cut.**
  - 5,339,202 halos are promoted, none at the final snapshot.
  - 2,702,688 groups lose members.
  - 1,111 descendants change progenitor order (16 of them their first progenitor).
  - The stored-chain check finds 0 mismatches over 28,208 descendants.

**(C) and the route on the subset.**

| Step | Exit | Wall-clock | Peak RSS | Log |
|---|---|---|---|---|
| (C) `validate` | 0 (PASS 20/20; spill 75.6 GB, the volume's rise during the step) | 527.11 s | 6.892 GB | `$R/logs/c-02-validate.log` |
| (C) `report` | 0 (PASS) | 541.39 s | 6.893 GB | `$R/logs/c-03-report.log` |
| route `ingest --cut-table $W/cut-table/forests.list` (workdir `$W/route-cut`) | 0 | 11,838.08 s | 10.198 GB | `$R/logs/rc-01-ingest.log` |
| route `transpose` | 0 | 1,661.79 s | 7.246 GB | `$R/logs/rc-02-transpose.log` |
| route `write` | 0 | 327.80 s | 4.172 GB | `$R/logs/rc-03-write.log` |
| route `validate` | 0 (PASS 20/20; spill 77.1 GB, the volume's rise during the step) | 528.10 s | 6.898 GB | `$R/logs/rc-04-validate.log` |
| route against (C), column by column | 0 | 248.88 s | 0.498 GB | `$R/logs/rc-05-g3c-compare.log` |

- **The comparison.** `VERDICT: IDENTICAL` over 71 files, 406,668,896 rows, 1,400 column comparisons, 980 header attributes, 630 schema groups and the sidecar (7,206,195 rows); 9 of 71 files are byte-identical.
- **Promotions.** The per-snapshot promotions agree over 70 snapshots across the rewriter, the route and the census: 5,339,202 each, with 0 at the final snapshot (`$R/logs/subset-g3c-promotions.log`).
- **FoF links.** The cut subset's `NextHaloInFOFgroup` link count is 36,064,127, against 41,403,329 in the reference. The difference, 5,339,202, is one link per promoted halo.
- **Storage.** The route's workdir measured 303,452,924 KiB, and its transient LaCie peak was 386.1 GB.

**The independent reference leg** (`$R/logs/g3c-fixture-leg.log`, script copy `$R/logs/g3c-fixture-leg.script.py`, 25.28 s, 0.342 GB):

- **Fixtures.** Slice 12's seven adversarial cut fixtures, each in a one-file layout and a two-file layout with a piece spanning the files.
- **Method.** The route converts each fixture and (C) rewrites it under the fixture's table. Slice 13's `tests/tools/transform_ascii_fixture.py` transforms the same ASCII under the same table. `halos-only` runs vertically on the transformed files through the unmodified `consistent_trees_ascii` reader, and the horizontal driver runs on (C)'s output. The two runs are compared per `UniqueGalaxyID`.
- **Result.** `SUMMARY: 14 of 14 cases PASS`, every field bitwise.
- **Negative control.** In each case the horizontal driver on the uncut dataset fails the comparison (exit 1).

### Determinism and resume

| Run | Result | Log |
|---|---|---|
| (M) on the subset, second run | `VERDICT: IDENTICAL`, 71 of 71 files byte-identical | `$R/logs/m-06-second-run.log`, `m-07-second-run-compare.log` |
| (M) SIGKILLed after 30 verified slabs (69 … 40), then `--resume` ("continuing after snapshot 40 with pass 1 done through 39", 185.91 s) | `VERDICT: IDENTICAL` with the uninterrupted run, 71 of 71 byte-identical | `$R/logs/m-08-interrupt.log`, `m-09-resume.log`, `m-10-resume-compare.log` |
| (C) on micro-Uchuu, whole-simulation table, second run | `VERDICT: IDENTICAL`, 51 of 51 byte-identical | `$R/logs/micro-cdet-01-second-run.log`, `micro-cdet-02-*.log` |
| (C) SIGKILLed after 20 verified slabs (49 … 30), then `--resume` ("continuing after snapshot 30 with pass 1 done through 29") | `VERDICT: IDENTICAL`, 51 of 51 byte-identical | `$R/logs/micro-cdet-03-interrupt.log`, `micro-cdet-05-resume.log`, `micro-cdet-06-*.log` |
| the interrupted (C) resumed with the restricted table | refused, exit 2: "this run was started with another configuration (a2609cdc…); refusing to resume it with different arguments" | `$R/logs/micro-cdet-04-resume-changed-table.log` |

### G4: the subset gate on the migrated subset

`MIMIC_SHIN_UCHUU_SUBSET_DATASET=$W/migrated/dataset MIMIC_SHIN_UCHUU_SUBSET_MANIFEST=$W/ref/manifest.json make MODEL=halos-only SIMULATION=shin-uchuu tests-scientific` (`$R/logs/g4-subset-gate.log`): exit 0, 16,598.70 s, peak RSS 12.395 GB, free memory at least 96% in every sample. The gate built its four worktrees at `a3d0ecee`.

- **Stage 2 confirms the gate's pins.**
  - 70 version 3 files (70 populated) from `consistent_trees_ascii`, `column_mapping_sha256 727d13f5…`, `links_adjacent 1`.
  - 406,668,896 halos, 0 gapped descendant links, longest span 1.
  - The conversion inventory from the reference's manifest: 2,744 source files, exactly those `locations.dat` lists. Inside them are 5,998,511 single-file forests, plus 12,694 −1/−1 forests, which are all 12,694 that the index files place in several files.
  - No pin needed changing.
- **Stage 3** finds the committed comparator and run files unchanged.

The vertical side ran through `consistent_trees_ascii` on the subset and wrote 13 partition files; the horizontal side read the migrated subset and wrote 8.

| Leg | Comparator line | Vertical | Horizontal |
|---|---|---|---|
| `halos-only` fixed | `PASSED: 92413007 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates` | 9,004 s | 232 s |
| `halos-only` dynamic | `PASSED: 92413007 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates` | 1,762 s | 98 s |
| `sage16` fixed | `PASSED: 60016178 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates` | 2,196 s | 538 s |
| `sage16` dynamic | `PASSED: 60016233 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates` | 2,016 s | 332 s |

The gate ends "all 4 parity legs PASS", and the tier ends `Scientific Test Summary: passed=45 failed=0 skipped=0 n/a=0 warned=1`. The warning is the tier's generic `test_zero_values` on its five-halo fixture, not a gate stage.

### G4: SHAM and HOD on the subset

Frozen `sham × shin-uchuu` and `hod × shin-uchuu` builds, serial and `USE-MPI=yes`, ran the modules and parameters of the committed `sham_micro-uchuu-ascii-horizontal-realdata.yaml` and `hod_micro-uchuu-ascii-horizontal.yaml` (`$R/scripts/make_runfile.py`; SHAM output snapshot 69, HOD output snapshots 69, 48, 44, 37, 32, 30, 27 and 24). The simulation is `shin-uchuu`, with `simulation_dir` set to the dataset. The `-np 4` runs are `mpirun --oversubscribe -np 4`. Under `mpirun`, `/usr/bin/time -l`'s maximum resident set size is the largest single process among the launcher and every rank it reaped, so for these runs the Peak RSS column gives the largest per-process peak, not a total. Compare each run log's `task 0: Peak process RSS` line:

- for the migrated subset's SHAM and HOD runs, the figure equals task 0's (1.769 and 1.735 GB);
- for the cut subset's `halos-only` and `sage16` runs, it exceeds task 0's (2.318 and 2.466 GB), so it is another rank's peak.

Where the wrapper's 30 s samples caught the ranks running, their summed RSS is given as well, as the job's aggregate. The other runs ended before a second sample. Run files are under `$R/runs/subset/<model>-<dataset>/` (the outputs were trashed; see Cleanup), and logs are `$R/logs/run-<model>-<dataset>.log`.

| Run | Wall-clock | Peak RSS |
|---|---|---|
| SHAM, version 2 subset | 177.90 s | 6.390 GB |
| SHAM, migrated subset | 83.33 s | 6.758 GB |
| SHAM, migrated subset, `-np 4` | 28.50 s | 1.769 GB, the largest per-process peak |
| HOD, version 2 subset | 88.10 s | 6.300 GB |
| HOD, migrated subset | 89.74 s | 6.669 GB |
| HOD, migrated subset, `-np 4` | 31.89 s | 1.735 GB, the largest per-process peak; the aggregate sampled across processes was 4,871,584 KiB |

| Comparison | Comparator line | Log |
|---|---|---|
| SHAM, version 2 against migrated (default mode) | `PASSED: 7997949 galaxies over 1 output snapshot(s) are bitwise identical in all 24 field(s), with identical UniqueGalaxyID sets and no duplicates` | `$R/logs/g4-sham-v2-vs-mig.log` |
| HOD, version 2 against migrated (`--match-created-by-lineage`) | `PASSED: 60090185 galaxies over 8 output snapshot(s) are identical under the lineage comparator: all 21 field(s) bitwise except UniqueGalaxyID, the pairing key, and UniqueCentralGalaxyID, compared through the pairing, with identical key sets and no duplicates (…; 28 created row(s) compared)` | `$R/logs/g4-hod-v2-vs-mig.log` |
| SHAM, serial against `-np 4`, migrated | `PASSED: 7997949 galaxies over 1 output snapshot(s) are bitwise identical in all 24 field(s), with identical UniqueGalaxyID sets and no duplicates` | `$R/logs/g4-sham-serial-vs-np4.log` |
| HOD, serial against `-np 4`, migrated (`--compare-created`) | `PASSED: 60090185 galaxies over 8 output snapshot(s) are bitwise identical in all 21 field(s), with identical UniqueGalaxyID sets and no duplicates (tree rows by UniqueGalaxyID, created rows by UniqueGalaxyID; 28 created row(s) compared)` | `$R/logs/g4-hod-serial-vs-np4.log` |

The three serial SHAM runs (version 2, migrated, cut) log the same audit line, `SHAM audit z=0.0000 candidates=2807 assigned=2807 masked=0`.

### The cut subset: serial, chunked and distributed

The cut subset `$W/cut/dataset` was run with the committed `halos-only_shin-uchuu.yaml` and `sage16_shin-uchuu.yaml` (output snapshots 24, 27, 30, 32, 37, 44, 48 and 69). Each model ran serially, serially with `input.forest_chunks: 8`, and under `mpirun --oversubscribe -np 4`; the `-np 4` peak RSS follows the per-process rule stated above.

| Model | Run | Wall-clock | Peak RSS | Comparator line against serial |
|---|---|---|---|---|
| `halos-only` | serial | 169.02 s | 9.012 GB | — |
| `halos-only` | `forest_chunks: 8` | 95.19 s | 1.503 GB | `PASSED: 92429377 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates` |
| `halos-only` | `-np 4` | 27.59 s | 2.453 GB, the largest per-process peak | `PASSED: 92429377 galaxies over 8 output snapshot(s) are bitwise identical in all 20 field(s), with identical UniqueGalaxyID sets and no duplicates` |
| `sage16` | serial | 559.66 s | 9.712 GB | — |
| `sage16` | `forest_chunks: 8` | 568.66 s | 1.777 GB | `PASSED: 60033537 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates` |
| `sage16` | `-np 4` | 159.11 s | 2.854 GB, the largest per-process peak; the aggregate sampled across processes was 9,916,416 KiB (max of six samples) | `PASSED: 60033537 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates` |

Logs are `$R/logs/run-<model>-cut[-chunks8|-np4].log` and `$R/logs/cut-<model>-serial-vs-{chunks8,np4}.log`.

**The driver's partition lines.** These are identical for both models; the serial runs log none.

- **`forest_chunks: 8`.** The headline is `Chunked horizontal partition: 7206195 forests over 1 task in 8 chunks each, each forest weighted by its peak occupancy over all slabs (its largest row count in any one slab)`, followed by `Partition task 0: forests [0, 7206195), peak weight 13594860 (widest slab snapshot 44 rows [0, 9006294))`. The eight chunk lines are:

  ```text
  Partition task 0 chunk 0: forests [0, 973649), peak weight 1699832 (widest slab snapshot 44 rows [0, 1111115))
  Partition task 0 chunk 1: forests [973649, 1988492), peak weight 1699832 (widest slab snapshot 44 rows [1111115, 2220100))
  Partition task 0 chunk 2: forests [1988492, 3066339), peak weight 1699804 (widest slab snapshot 44 rows [2220100, 3326386))
  Partition task 0 chunk 3: forests [3066339, 4111201), peak weight 1699832 (widest slab snapshot 44 rows [3326386, 4435078))
  Partition task 0 chunk 4: forests [4111201, 5132428), peak weight 1699832 (widest slab snapshot 44 rows [4435078, 5547231))
  Partition task 0 chunk 5: forests [5132428, 6011245), peak weight 1699830 (widest slab snapshot 44 rows [5547231, 6669381))
  Partition task 0 chunk 6: forests [6011245, 6049358), peak weight 1699825 (widest slab snapshot 44 rows [6669381, 7739016))
  Partition task 0 chunk 7: forests [6049358, 7206195), peak weight 1696073 (widest slab snapshot 44 rows [7739016, 9006294))
  Partition tables hold 5152 B on every task (counted in its retention accounting); the forest weights held 57649560 B on task 0 while it was cut
  ```

- **`-np 4`.** The lines are prefixed `task 0:`:

  ```text
  Distributed horizontal partition: 7206195 forests over 4 tasks, each forest weighted by its peak occupancy over all slabs (its largest row count in any one slab)
  Partition task 0: forests [0, 1988482), peak weight 3399654 (widest slab snapshot 44 rows [0, 2220092))
  Partition task 1: forests [1988482, 4111206), peak weight 3399654 (widest slab snapshot 44 rows [2220092, 4435085))
  Partition task 2: forests [4111206, 6011245), peak weight 3399654 (widest slab snapshot 44 rows [4435085, 6669381))
  Partition task 3: forests [6011245, 7206195), peak weight 3395898 (widest slab snapshot 44 rows [6669381, 9006294))
  Partition tables hold 2880 B on every task (counted in its retention accounting); the forest weights held 57649560 B on task 0 while it was cut
  ```

The migrated subset's `-np 4` SHAM and HOD runs split its 6,011,205 forests at 1,260,843, 2,834,267 and 4,382,771, with peak weights of 3,235,390, 3,235,385, 3,235,390 and 3,235,013.

### Decision 7's measurement at subset scale: SHAM and HOD on the cut subset

Serial SHAM and HOD ran on the cut subset (82.07 s at 6.758 GB, and 90.03 s at 6.671 GB). Each was compared with the same model's serial run on the migrated subset through the cut-difference report: `--match-by-catalogue-id --match-created-by-lineage --multiplier 20000000000`, with the two sidecars and `--cut-record $W/cut-table/record.json`, and `--report-all-forests` for SHAM. These differences are reported, not failed, and both reports end in PASSED (`$R/logs/rev5-sham-cut-report.log`, 40.43 s; `$R/logs/rev5-hod-cut-report.log`, 203.06 s). The per-forest lines are in those logs: 6,011,205 forests for SHAM and the 290,679 split forests for HOD. `$R/logs/rev5-per-forest-summary.log` summarises them.

| | SHAM (snapshot 69) | HOD (8 snapshots) |
|---|---|---|
| Split forests with records | 290,679 | 290,679 |
| … with a difference or an unmatched record | 70,842 | 97,322 |
| Records in split forests: identical / differing | 1,967,864 / 230,021 | 35,076,340 / 519,576 |
| Only in migrated / only in cut | 0 / 99 | 23 / 14,585 |
| Differing per field | `ShamMpeak` 161,326; `deltaMvir` 52,229; `infallVvir` 16,825; `infallVmax` 14,240; `ShamVpeak` 14,002; `infallMvir` 12,406; `StellarMass` 544 | `deltaMvir` 467,726; `infallVvir` 344,639; `infallVmax` 342,920; `infallMvir` 340,267; `Type`, `UniqueCentralGalaxyID`, `Mvir`, `Rvir` and `Vvir` 318,828 each; `CentralMvir` 318,538; `HODGhost` 113; `Pos`, `Vel` 5 each |
| Forests with differing records, by count of differing records: 1 / 2–10 / 11–100 / 101–1,000 / over 1,000 | 55,510 / 13,606 / 1,577 / 107 / 23 | 61,589 / 27,723 / 3,913 / 350 / 33 |
| Largest per-forest differing count | 8,484 (forest 26582413341, with 60,990 identical) | 20,905 (the same forest, with 1,368,769 identical) |
| **Outside the split forests** | **5,720,526 forests with records, 0 with a difference; 5,800,064 records identical** | every record held to the strict rule identical, per snapshot (e.g. 5,800,064 at snapshot 69) |

Created rows: the migrated HOD run has 28 and the cut run 19.

### Retained datasets

Three subset datasets are retained on the LaCie for Stage D and Slice 17. Their workdirs and the table measure together 153,768,520 KiB (157.5 GB, about 0.16 TB; measured at 20:25:36Z on 2026-10-10):

| Dataset | For | Workdir | Manifest | Dataset directory (`du -sk`) | Workdir `du -sk` |
|---|---|---|---|---|---|
| version 2 subset | Stage D | `$W/v2-subset` | `$W/v2-subset/manifest.json` (beside `scratch/`, `forest_index_table.npy`, `forest_max_snap.npy`, `conversion_report.{json,txt}`) | `$W/v2-subset/hdf5`, 40,004,580 KiB | 40,420,160 KiB (40,462,472 KiB after `report`; see above) |
| migrated subset ((M)) | Stage D | `$W/migrated` | `$W/migrated/manifest.json` (beside `rewrite_summary.json`, `tree_inventory.npy`, `conversion_report.{json,txt}`) | `$W/migrated/dataset`, 56,062,956 KiB | 56,501,264 KiB |
| cut subset ((C)), with its table | Slice 17 | `$W/cut` | `$W/cut/manifest.json` (beside `rewrite_summary.json`, `tree_inventory.npy`, `cut_pieces.npy`, `conversion_report.{json,txt}`) | `$W/cut/dataset`, 56,090,604 KiB | 56,609,920 KiB; the table, record and summary in `$W/cut-table` (237,176 KiB, md5 `9d26873760092bf0268ee1e7d48203ce`) |

**Cleanup.** Once this record was written, every other workdir was moved to the volume's trash with `trash`, never `rm`. Each entry, with its size, is logged in `$R/TRASHED.md`. The trash keeps their space until it is emptied, which is left to the owner.

- **The reference conversion.** `$W/ref` (301,231,688 KiB). Its `manifest.json` (SHA-256 `f97892ee…`) and `conversion_report.{json,txt}` were first copied to `$W/ref-manifest/`: this is the manifest the subset gate read through `MIMIC_SHIN_UCHUU_SUBSET_MANIFEST`. The copy has not itself been run through the gate. After the cleanup, its `ascii_preparation/manifest.json` was copied (not moved) back out of the volume trash to `$W/ref-manifest/ascii_preparation-manifest.json`, the record that the G3 conservation check reads.
- **The cut-route conversion.** `$W/route-cut` (303,452,924 KiB).
- **The other rewrites.** The no-op `$W/noop`, the determinism runs `$W/migrated-run2` and `$W/migrated-int`, and every rewriter scratch (`$W/*-scratch`, each empty after its run).
- **The rest of the work root.** The battery spill directories (`$W/spill-*`, empty), the subset census aggregate `$W/census-subset`, and every micro-Uchuu rewrite, route conversion and spill (`$W/micro`).
- **On the internal volume.** The model runs' outputs (`$R/runs/subset/*/out`, `$R/runs/micro-hod/{v2,m,s5}`; their run files are kept) and three trial directories of the fixture leg.

What remains under `$W` is the three retained datasets, the cut table and `ref-manifest/` (121,900 KiB). No source data was touched.

---

## Stage C — the bounded battery and the production procedure

### Preamble: the bounded battery (Slice 17)

Slice 17 of the implementation plan (revision 6) added the version 3 battery's bounded mode, `convert_trees.py validate|report --bounded` (`convert/mimic-convert/validate_bounded.py`), which the production procedure's steps 4 and 9 run. It accepts adjacent datasets only (every snapshot file declaring `links_adjacent` 1; anything else is refused by name before any long pass), sorts and spills nothing, and holds at most a ceiling it prints at start and refuses above `--memory-budget-mb`. The external-sort mode is unchanged and stays the default. The obligation-to-algorithm table is in `validate_bounded.py`'s docstring and in the converter manual's "Restart, cleanup and memory". Logs: `/Volumes/Internal/results/mimic/shin-uchuu-v3-stage-c/slice-17/` (abbreviated `$C`).

**The ceiling** (bytes): the larger of the row pass, 8`F` + `B`(4`R` + 256), and the pair pass, 64`W` + ⌈`W`/8⌉ + ⌈`H`/8⌉ + ⌈`N`/8⌉ + 8`F` + 256`B` + 2,097,152 (the fixed 2 MiB diagnostic workspace, `DIAGNOSTIC_WORKSPACE_BYTES`, for the windows that count a bitset's set bits and find its first clear ones), with `W` the widest snapshot's rows, `N` the dataset's halos, `H` the inventory's `total_halos`, `F` the headers' `n_forests_total`, `R` the bytes of one row of every `/halos` column, and `B` = max(1, min(`block_rows`, `W`)), `block_rows` 1,048,576 by default: the one block size every read and window uses, the sidecar's forest rows included. The 64 bytes per row of the widest snapshot are five int64 columns (`Descendant`, `FirstHaloInFOFgroup`, `NextProgenitor`, `NextHaloInFOFgroup` as global positions and `ForestIndex`) and two int32 in-link counters of the target snapshot, plus `FirstProgenitor` and `ForestIndex` of the snapshot after it. ⌈`W`/8⌉ is the chain pass's visited bitset; ⌈`H`/8⌉ and ⌈`N`/8⌉ are the `SourceHaloID` and (`ForestIndex`, rank) position bitsets; 8`F` is the per-forest totals.

**At Shin-Uchuu's numbers** (the census record and the plan's decision 6: `W` 519,342,987 rows at snapshot 34, `N` = `H` 22,503,649,037, `F` 166,547,771 uncut and 244,953,607 after the decided cut; `R` 140, the default schema's row, which the subset datasets also measured), evaluated with

```bash
mimic_venv/bin/python convert/mimic-convert/validate_bounded.py ceiling \
    --widest-rows 519342987 --n-halos 22503649037 --n-forests-total <F>
```

| Dataset | `F` | Pair pass = ceiling | Terms | `--memory-budget-mb` at least |
|---|---|---|---|---|
| uncut | 166,547,771 | 40,531,696,078 B (40.53 GB, 37.75 GiB) | 64 × 519,342,987 = 33,237,951,168; visited bitset 64,917,874; two bitsets 2 × 2,812,956,130; 8 × `F` = 1,332,382,168; 256 × 1,048,576 = 268,435,456; workspace 2,097,152 | 38655 |
| cut | 244,953,607 | 41,158,942,766 B (41.16 GB, 38.33 GiB) | as above with 8 × `F` = 1,959,628,856 | 39253 |

The row pass is 2,188,020,184 B (uncut) and 2,815,266,872 B (cut). Both ceilings are below 64 GB. They bound the battery's working arrays, not the interpreter's total RSS; on the subset the measured peak RSS stayed below the printed ceiling (below), and the procedure's budget should leave headroom above it.

**The agreement run on this host.** Both modes, in sequence, over the two retained subset datasets (`$W/migrated` and `$W/cut` of Stage R), each `validate --workdir <dir> --multiplier 20000000000 --spill-dir /Volumes/LaCie/data/uchuu/shin-uchuu-stage-c-scratch/slice-17-spill`, the bounded mode with `--bounded --memory-budget-mb 2048` and the external-sort mode with `--memory-budget-mb 8192` (Stage R's budget), each detached under `MallocLargeCache=0 /usr/bin/time -l`, with the spill directory's `du -sk` sampled every 15 s (`$C/<run>.spill.log`), and nothing else LaCie-bound alongside. The external-sort runs are `$C/agreement.sh`'s; the bounded rows are the reruns on the committed code by `$C/bounded-final3.sh` (below). `validate` only: no `report` was run, so the retained `conversion_report.{json,txt}` files are untouched.

| Run | Verdicts | Printed ceiling | Wall | Peak RSS | Peak spill (sampled) | Log |
|---|---|---|---|---|---|---|
| bounded, migrated | 20 PASS | 997,818,075 B | 206.08 s | 933,298,176 B | 0 | `$C/validate-bounded-final3-migrated.{log,stderr}` |
| external-sort, migrated | 20 PASS | — | 561.56 s | 6,892,568,576 B | 75,917,360 KiB (77.74 GB) | `$C/validate-external-migrated.{log,stderr}` |
| bounded, cut | 20 PASS | 1,007,377,995 B | 221.36 s | 945,717,248 B | 0 | `$C/validate-bounded-final3-cut.{log,stderr}` |
| external-sort, cut | 20 PASS | — | 538.39 s | 6,892,486,656 B | 74,042,828 KiB (75.82 GB) | `$C/validate-external-cut.{log,stderr}` |

The two modes' verdict sets are identical check by check on both datasets (every one of the 20 checks PASS in both). The subset's widest snapshot holds 9,006,294 rows (`W`), `N` = `H` = 406,668,896, `F` 6,011,205 (migrated) and 7,206,195 (cut). The bounded mode spilled nothing (the spill directory stayed at 0 KiB through every sample). The external-sort figures agree with Stage R's: its `validate` and `report` runs peaked at 6.89–6.90 GB RSS (`/usr/bin/time`), and the retained reports' `peak_spill_bytes` are 77.7 GB (migrated, 77,739,111,152 B) and 77.1 GB (cut, 77,141,120,528 B); the 15 s sampling here can miss the briefest peaks. The spill directory was empty after every run. Nothing was trashed.

**Code under test.** The external-sort runs ran the working tree at `b18b5d17` with the slice's first uncommitted changes (`$C/code-identity.txt`); the external-sort mode's code (`validate_v3.py` beyond its dispatch) is unchanged since, so they are not repeated. The bounded rows ran the code the slice's final commit holds (`$C/code-identity-final3.txt`, unchanged at the end of the reruns: `validate_bounded.py` md5 `870556281608622e6db197a4f3c518ff`, `validate_v3.py` `484750f7f2b68fd7b911fb56eed00616`, `convert_trees.py` `8ec4a591dc3dc91e96ba7eacc6603b4c`, `report.py` `8edc1a982ca691324aecea85144a8bd4`). They supersede three earlier bounded rounds on earlier code, each with the same verdicts, no spill and RSS within 0.1 %: `$C/validate-bounded-{migrated,cut}.*` (270.81 s and 269.31 s), `$C/validate-bounded-final-{migrated,cut}.*` (216.57 s and 196.40 s) and `$C/validate-bounded-final2-{migrated,cut}.*` (159.21 s and 162.01 s). The first two printed ceilings 2,097,152 B lower, before the ceiling gained the fixed diagnostic workspace; the last printed the same ceilings as these. The wall times vary between rounds by up to 40 % (159–271 s) with the external volume's read rate, while the CPU times stay at 116–122 s user.
