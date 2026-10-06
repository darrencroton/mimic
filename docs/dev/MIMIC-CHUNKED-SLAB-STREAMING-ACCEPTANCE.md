# Chunked Slab Streaming — Acceptance Record

**Status:** Recorded 2026-10-07, for Slice 5 of `MIMIC-CHUNKED-SLAB-STREAMING-IMPLEMENTATION-PLAN.md` (decision C9, the acceptance predicate). This file records measurements; it is not a plan and makes no claim beyond the runs below.

**Code under test:** branch `feature/chunked-slab-streaming` at `8306f224` (Slice 5's gate commit; the runtime is Slice 4's, `47b8db35`, since Slice 5 changes no runtime source). Every run below was built from `8306f224` (the run logs' `Commit` line), so the `G = 1` row of each memory table was measured at the same commit as the others. This record is committed in the next commit, which adds only this file.

**Host:** macOS 27.0.1 (Darwin 27.0.0), 32 cores, 512 GB RAM; Open MPI 5.0.9; HDF5 1.14.6; `mimic_venv` Python 3.14 with h5py 3.15.1. Runs were strictly sequential: no build, test or other run overlapped any timed run.

---

## Fixture gate (`make tests-distributed`)

Run in its CI form, `MPIRUN="mpirun --oversubscribe" make tests-distributed`, with log `build/distributed_tests.log`, on the gate committed at `8306f224`. It passed with **245 checks** (158 before Slice 5), no FAIL, ERROR, WARN or SKIP markers. Its summary line was `PASS: tests-distributed (245 checks: MPI control test, 4 model(s) at -np 1, 2, 3, 4, 8, chunked legs (halos-only, sage16, hod) at forest_chunks 2, 3, 8 serial and -np 2 x 2, -np 3 x 3, chunked refusal (sham, hod), version 2 refusal; no skips)`.

The 87 new checks come from 18 new launches of `./mimic` on the committed `forest_blocks` fixture (each a sub-second run) and 15 new comparator runs:

- `halos-only` and `sage16` (26 checks each): serial runs at `forest_chunks` 2, 3 and 8, each byte-identical per `UniqueGalaxyID` to the serial `forest_chunks: 1` run through `scripts/compare_cross_format_identity.py --compare-created` (47 or 49 galaxies compared) and holding every partition's `UniqueGalaxyID` column in the reference's file order; MPI runs at `-np 2` with `forest_chunks: 2` and `-np 3` with `forest_chunks: 3`, with the task-layout, master-link and partition-log checks and the same comparison.
- `hod` (34 checks): the same legs on `simulations/mini-millennium-horizontal/_tests/input/forest_blocks_hod_chunked.yaml` (the fixture run file without `modules.post_snapshot`) against a serial `forest_chunks: 1` run of that variant (57 galaxies, of which 10 created rows on both sides at every leg), plus the shipped `forest_blocks_hod.yaml` refused at configuration at `forest_chunks: 2`.
- `sham` (1 check): the shipped run file refused at configuration at `forest_chunks: 2`, with the snapshot-scope message and no `Opened horizontal run` line.

Every chunked run logged its chunked partition line (`Chunked horizontal partition: 6 forests over 1 task in G chunks each` serially, `Distributed horizontal partition: 6 forests over N tasks in G chunks each` under MPI); the `G = 8` and `-np 3 × G = 3` runs swept idle chunks.

---

## Real-data stage (`micro-uchuu-horizontal`)

The dataset holds 440,651 forests and 22,580,924 halos over 50 snapshots (`format_version 3`); its widest slab is snapshot 27 with 621,360 halos. Each model ran through its shipped run file `models/<model>/input/<model>_micro-uchuu-horizontal.yaml` with the output directory redirected and `input.forest_chunks` set (omitted at `G = 1`). The serial runs used `make MODEL=<model> SIMULATION=micro-uchuu-horizontal TEST_BUILD=no` (non-MPI); the MPI run used the same with `USE-MPI=yes`.

Commands:

```bash
/usr/bin/time -l -o time.txt ./mimic run.yaml
/usr/bin/time -l -o launcher.txt mpirun -np 2 sh -c \
  'exec /usr/bin/time -l -o rank_${OMPI_COMM_WORLD_RANK}.txt ./mimic run.yaml'
mimic_venv/bin/python scripts/compare_cross_format_identity.py <left>/<base> <right>/<base> --compare-created
```

### Identity

Every run was compared with the serial reference in `archive/distributed-references/<model>/` (`sage16` captured at `fe0b9cad`, `halos-only` at `95c1d642`, both non-MPI and before any slice of this plan) and with this commit's serial `G = 1` run. All 18 comparisons exited 0.

| Model | Output snapshots | Galaxies compared | Created rows | Fields | Runs compared (each against the reference and against `G = 1`) |
|---|---|---|---|---|---|
| `sage16` | 7, 8, 10, 12, 16, 23, 28, 49 | 3,112,186 | 0 | 42 | `G = 1` (reference only), 2, 4, 8; `-np 2 × G = 2` |
| `halos-only` | 7, 8, 10, 12, 16, 23, 28, 49 | 4,409,643 | 0 | 20 | `G = 1` (reference only), 2, 4, 8; `-np 2 × G = 2` |

Every galaxy is bitwise identical in every field, with identical `UniqueGalaxyID` sets and no duplicates. The serial `G = 1` output at this commit is identical to the pre-plan references.

### Peak memory and wall-clock

Peak RSS is `/usr/bin/time -l`'s maximum resident set size, per process (GB = 1e9 B). Wall-clock is its `real` time; for the MPI run, each rank's own and the launcher's.

`sage16`:

| Run | Peak RSS | Ratio to `G = 1` | Wall-clock |
|---|---|---|---|
| serial `G = 1` | 2.452 GB | 1.000 | 39.33 s |
| serial `G = 2` | 1.215 GB | 0.495 | 38.49 s |
| serial `G = 4` | 0.577 GB | 0.235 | 38.70 s |
| serial `G = 8` | 0.307 GB | 0.125 | 38.77 s |
| `-np 2 × G = 2`, rank 0 | 0.615 GB | 0.251 | 22.97 s |
| `-np 2 × G = 2`, rank 1 | 0.772 GB | 0.315 | 22.97 s (launcher 23.06 s) |

`halos-only`:

| Run | Peak RSS | Ratio to `G = 1` | Wall-clock |
|---|---|---|---|
| serial `G = 1` | 4.998 GB | 1.000 | 6.28 s |
| serial `G = 2` | 3.701 GB | 0.740 | 6.43 s |
| serial `G = 4` | 2.477 GB | 0.495 | 6.43 s |
| serial `G = 8` | 1.453 GB | 0.291 | 5.92 s |
| `-np 2 × G = 2`, rank 0 | 2.288 GB | 0.458 | 6.60 s |
| `-np 2 × G = 2`, rank 1 | 2.238 GB | 0.448 | 6.60 s (launcher 6.70 s) |

Serial peak RSS falls with `G` in both models; serial wall-clock stays within 6% of the `G = 1` run's on these single runs. For `sage16` the peak falls nearly as `1/G`; for `halos-only` it falls more slowly (0.29 at `G = 8`). This record does not attribute either curve to particular memory terms.

### Partition and chunk log

Task 0's partition at serial `G = 4`, identical for the two models:

```text
Chunked horizontal partition: 440651 forests over 1 task in 4 chunks each, weighted by the widest slab, snapshot 27 (621360 halos)
Partition task 0: forests [0, 440651), widest-slab weight 621360 (snapshot 27 rows [0, 621360))
Partition task 0 chunk 0: forests [0, 101620), widest-slab weight 155340 (snapshot 27 rows [0, 155340))
Partition task 0 chunk 1: forests [101620, 209418), widest-slab weight 155340 (snapshot 27 rows [155340, 310680))
Partition task 0 chunk 2: forests [209418, 314723), widest-slab weight 155340 (snapshot 27 rows [310680, 466020))
Partition task 0 chunk 3: forests [314723, 440651), widest-slab weight 155340 (snapshot 27 rows [466020, 621360))
Partition tables hold 2080 B on every task (counted in its retention accounting); the forest weights held 3525208 B on task 0 while it was cut
```

The four chunks are exactly the four task ranges of the step 3 `-np 4` partition (`MIMIC-DISTRIBUTED-SNAPSHOT-ACCEPTANCE.md`), as decision C3 requires. At `-np 2 × G = 2` task 0 logged task 0 as forests `[0, 209418)` with chunks `[0, 101620)` and `[101620, 209418)`, and task 1 as forests `[209418, 440651)` with chunks `[209418, 314723)` and `[314723, 440651)`: the same four ranges.

A separate, untimed `--verbose` rerun at serial `G = 8` for each model logged 56 `Appended ... (not yet final)` lines (seven later chunks times eight output snapshots), none of them `Appended 0 galaxies`. Chunk 0 loaded halos at every output snapshot (for example 10,915 of snapshot 7's 83,505), so on this dataset no partition's first visit was empty.

### Largest-forest floor

Read from snapshot 27's `ForestIndex` column (non-decreasing throughout): the largest forest, `ForestIndex` 237997, holds 9,940 of the widest slab's 621,360 halos, **1.60%**. No `(NTask, G)` can put less than that share of the widest slab in one chunk, since a forest is never split (decision C8).
