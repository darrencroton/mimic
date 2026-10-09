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

**Retained version 2 dataset (for Slice 15's G3):** `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii-horizontal` (50 snapshot files and `forests.h5`), untouched.

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
