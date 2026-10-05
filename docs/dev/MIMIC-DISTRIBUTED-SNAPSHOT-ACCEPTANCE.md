# Distributed Snapshot Operations — Acceptance Record

**Status:** Recorded 2026-10-06, for Slice 9 of [`MIMIC-DISTRIBUTED-SNAPSHOT-IMPLEMENTATION-PLAN.md`](MIMIC-DISTRIBUTED-SNAPSHOT-IMPLEMENTATION-PLAN.md) (decision D12, the acceptance predicate). This file records measurements; it is not a plan and makes no claim beyond the runs below.

**Code under test:** branch `feature/distributed-snapshot` at `f2eb3155` (Slice 8). Slice 9 adds only the fixture, the gate, the MPI control test and this record, and changes no runtime source, so the runs measure the Slice 8 runtime.

**Host:** macOS (Darwin 27.0.0), 32 cores, 512 GB RAM; Open MPI 5.0.9 (`mpicc` wrapping Apple clang 21); HDF5 1.14.6; `mimic_venv` Python 3.14 with h5py 3.15.1.

---

## Fixture gate (`make tests-distributed`)

Run in its CI form, `MPIRUN="mpirun --oversubscribe" make tests-distributed`, from a clean tree (after `make clean`, with no `build/generated/git_version.h`), with log `build/distributed_tests.log`. The final gate revision, committed in Slice 9's last commit (the child of `8babca3d`), passed with **102 checks**, no FAIL, ERROR, WARN or SKIP markers. Its summary line was `PASS: tests-distributed (102 checks: MPI control test, 4 model(s) at -np 1, 2, 3, 4, 8, version 2 refusal; no skips)`. The checks were:

- the MPI control test `tests/mpi/test_snapshot_collectives_mpi.c` at `-np 3`: 9 of 9 cases;
- `halos-only`, `sage16`, `sham` and `hod` on `mini-millennium-horizontal`'s committed `forest_blocks` fixture (six forests, 71 halos, seven snapshots with snapshot 3 empty, ten gapped `Descendant` links; the largest forest holds 7 of the widest snapshot's 17 halos, 41%): serial non-MPI output against the MPI build at `-np 1, 2, 3, 4, 8`, every count byte-identical per `UniqueGalaxyID` through `scripts/compare_cross_format_identity.py --compare-created` (for `hod` this includes 10 created rows, and the gate requires the comparator to report created rows on both sides at every count), with the expected file layout at every count and the serial `sham` audit showing `candidates=7 assigned=4 masked=3` on every output snapshot;
- the version 2 refusal: `halos-only` on `simulations/micro-uchuu-ascii-horizontal/_tests/data/generic/` at `-np 2` stopped at startup with the format_version 2 message.

Earlier revisions of the same gate also passed in CI form: the `c38cd1d9` gate with 97 checks (without the five `hod` created-row checks), and the `8babca3d` gate with 102 checks. The runtime under test is the same in every revision; the later revisions harden the gate itself (non-MPI serial builds whatever the environment, process-group timeouts, the created-row requirement, a clean-tree `git_version.h`).

At `-np 4` and `-np 8` the partition leaves trailing tasks idle (forest ranges `[6, 6)`), so empty tasks take part in every collective and write empty partitions; at `-np 3` and above the dominant forest is alone on task 1.

---

## Real-data stage (`micro-uchuu-horizontal`)

The dataset holds 440,651 forests and 22,580,924 halos over 50 snapshots. Each model was built with `make MODEL=<model> SIMULATION=micro-uchuu-horizontal TEST_BUILD=no USE-MPI=yes`, run from the repository root through its shipped run file `models/<model>/input/<model>_micro-uchuu-horizontal.yaml` (output directory redirected), and compared with the serial reference captured at run preparation (`archive/distributed-references/<model>/`, non-MPI builds at `fe0b9cad`).

**The serial side of every table below is that run-preparation reference, built from `fe0b9cad`, the code before any slice of this plan; it was not re-run at `f2eb3155`.** The identity table is therefore a whole-feature parity result: pre-feature serial output against post-feature output at `-np 4`. The memory and wall-clock ratios likewise compare those two builds (the pre-feature serial binary and the post-feature MPI binary), not a serial and an MPI run of the same commit. (Serial output at the feature commits is held to pre-feature output separately: on the committed fixtures for `halos-only` and `sage16` by `make tests-snapshot-global-identity`, and on this dataset for these three models by Slice 8's serial bit-identity check against the same references.)

Commands:

```bash
/usr/bin/time -l -o launcher.txt mpirun -np 4 sh -c \
  'exec /usr/bin/time -l -o rank_${OMPI_COMM_WORLD_RANK}.txt ./mimic run.yaml'
mimic_venv/bin/python scripts/compare_cross_format_identity.py \
  archive/distributed-references/<model>/<base> <output>/<base> --compare-created
```

The inner `/usr/bin/time` measures each rank's own process; the outer one gives the run's wall-clock (the launcher's own peak memory footprint was about 10 MB).

### Identity

| Model | Output snapshots | Galaxies compared | Created rows | Fields | Comparator exit |
|---|---|---|---|---|---|
| `sage16` | 7, 8, 10, 12, 16, 23, 28, 49 | 3,112,186 | 0 | 42 | 0 |
| `sham` | 49 | 557,669 | 0 | 24 | 0 |
| `hod` | 7, 8, 10, 12, 16, 23, 28, 49 | 3,114,016 | 1,487 (both runs) | 21 | 0 |

Every galaxy is bitwise identical in every field, with identical `UniqueGalaxyID` sets and no duplicates. The `-np 4` audit lines are byte-identical to the serial references': `SHAM audit z=0.0005 candidates=74596 assigned=74596 masked=0`, and all eight `HOD audit` lines (D12's permitted last-bit difference in `module_snapshot_sum_f64()` did not occur here).

### Peak memory and wall-clock

Peak RSS is `/usr/bin/time -l`'s maximum resident set size, per process (GB = 1e9 B). The serial columns are the `fe0b9cad` reference runs (their `time.txt`); the rank and `-np 4` columns are the `f2eb3155` MPI build.

| Model | Serial peak RSS | Rank 0 | Rank 1 | Rank 2 | Rank 3 | Largest rank / serial | Serial wall-clock | `-np 4` wall-clock |
|---|---|---|---|---|---|---|---|---|
| `sage16` | 2.452 GB | 0.615 GB | 0.638 GB | 0.644 GB | 0.732 GB | 0.30 | 41.32 s | 11.24 s |
| `sham` | 2.316 GB | 0.534 GB | 0.569 GB | 0.570 GB | 0.656 GB | 0.28 | 8.49 s | 2.25 s |
| `hod` | 2.237 GB | 0.535 GB | 0.567 GB | 0.578 GB | 0.662 GB | 0.30 | 6.26 s | 2.34 s |

Every rank's peak is below the serial peak, at 23% to 30% of it. The per-rank peaks exceed a quarter of the serial peak, which is expected because each process carries costs that do not divide (the executable, the HDF5 library and the reader's per-snapshot tables) and because D3 splits only the widest slab exactly, the others approximately (D11); this record does not attribute the excess further. Rank 3 is the largest in all three models.

### Partition (task 0's log, identical for the three models)

```text
task 0: Distributed horizontal partition: 440651 forests over 4 tasks, weighted by the widest slab, snapshot 27 (621360 halos)
task 0: Partition task 0: forests [0, 101620), widest-slab weight 155340 (snapshot 27 rows [0, 155340))
task 0: Partition task 1: forests [101620, 209418), widest-slab weight 155340 (snapshot 27 rows [155340, 310680))
task 0: Partition task 2: forests [209418, 314723), widest-slab weight 155340 (snapshot 27 rows [310680, 466020))
task 0: Partition task 3: forests [314723, 440651), widest-slab weight 155340 (snapshot 27 rows [466020, 621360))
task 0: Partition tables hold 2080 B on every task (counted in its retention accounting); the forest weights held 3525208 B on task 0 while it was cut
```

The widest slab is split exactly into four equal weights of 155,340 halos.

### Super-forest floor

Read from snapshot 27's `ForestIndex` column (non-decreasing throughout): the largest forest, `ForestIndex` 237997, holds 9,940 of the widest slab's 621,360 halos, **1.60%**, the same figure the plan measured at its baseline. No rank count can put less than that share of the widest slab on one rank, so micro-Uchuu splits nearly linearly well beyond four ranks.
