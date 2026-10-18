# Shin-Uchuu Cut-Vertical — Acceptance Record

**Status:** Started 2026-10-18 by Slice 6 of [`MIMIC-SHIN-UCHUU-CUT-VERTICAL-IMPLEMENTATION-PLAN.md`](MIMIC-SHIN-UCHUU-CUT-VERTICAL-IMPLEMENTATION-PLAN.md) (the subset rehearsal). This file records measurements; it is not a plan and makes no claim beyond the runs below. Every number here is quoted from a log named beside it, printed by a named script into a log (chiefly `$V/logs/record-figures.txt`, `projections.txt`, `metrics-<step>.json`, `gate-metrics.txt` and `gate-rss-bounds.txt`), or quoted from another named file (such as `$V/prod/file_plan.json`, a manifest, or another record), as stated beside it. The logs live outside the repository, under `/Volumes/LaCie/data/uchuu/shin-uchuu-cut-vertical-rehearsal/` (abbreviated `$V` below; logs `$V/logs/`, scripts `$V/scripts/`, ledger `$V/LEDGER.md`).

**Host** (`$V/logs/host.txt`): macOS (Darwin 27.0.0), Mac Studio (`Mac15,14`, Apple M3 Ultra), 32 cores, 549,755,813,888 B (512 GiB) RAM; Open MPI 5.0.9; `mimic_venv` Python 3.14.6 with h5py 3.15.1 (HDF5 1.14.6) and numpy 2.3.4. GB = 10⁹ B, TB = 10¹² B, MB/s = 10⁶ B/s.

---

## Rehearsal

The whole route at subset scale on the Stage R cut subset, plus a cold measurement on two production files, run on 2026-10-17/18 UTC (23:17Z to 03:05Z).

### Method

**Step wrapper.** Every step ran through `$V/scripts/step.sh <name> <command>` (adapted from the Stage C procedure's wrapper), inside a sequence script under `$V/scripts/` that was launched detached under `caffeinate -i` (the launch itself is not logged), as `MallocLargeCache=0 /usr/bin/time -l <command>`. Each log `$V/logs/<name>.log` records in its header the UTC start, the commit and branch, `git status --porcelain`, the md5 of the converter modules and the instruments (below), the cwd, `TMPDIR`, the command, `df` of both volumes, the llama router's processes and the LaCie's cumulative read and write bytes (from `ioreg`); every output line is prefixed with the Unix time it reached the log, so phases inside a step can be timed when the command flushes its output (the converter does; Mimic buffers its output to the pipe, so its lines all carry the time of its exit); the footer records the exit status, `df` after, the volumes' used-KiB ranges, the maximum summed RSS of the step's processes, the minimum free-memory percentage and any `du -sk` asked for. Beside each log a sampler writes, every 5 s (the subset steps `s01` to `s03` and the vertical runs `s11` and `s12`), 2 s (the gate) or 10 s (the rest, including the cold conversion `s06`): `<name>.samples` (both volumes' used KiB, summed RSS, free memory, the LaCie's byte counters), `<name>.procs` (every process above 200 MB: pid, ppid, `%cpu`, RSS, command) and `<name>.iostat` (`iostat -d -w <s> disk9`). The LaCie's physical disk is `disk9`; `disk10` is its synthesized APFS container, which `iostat` refuses (`$V/logs/host.txt`). Every step appended a row to `$V/LEDGER.md`. Peak RSS is `/usr/bin/time -l`'s maximum resident set size, which for a step with child processes is the largest single process; sums across processes come from the sampler. Steps ran one at a time; the only overlap is the two binary builds (CPU; they wrote to the repository's build tree), which ran during `compare`. Figures derived by arithmetic (ratios, rates, differences, plan statistics) are printed with their inputs by `$V/scripts/record_figures.py` into `$V/logs/record-figures.txt`.

**Code under test.** Every step ran at `56ad1b8c0b7bc0e4a0e8abdee63d25d79797cf9d` on `feature/shin-uchuu-cut-vertical` with an empty `git status --porcelain` (every log header). Source md5s, identical in every log: `write_vertical_trees.py` `81974e7701b5c305b4b6d8572210abb3`, `vertical/__init__.py` `3597a7c65e324ca24072bee45797c966`, `vertical/assemble.py` `cb966fd488770e363c021290bcebd70a`, `vertical/plan.py` `5b5ea7495b8a6ca42f68362527ed8027`, `vertical/record.py` `d8233ed4be93520f895857247b43c942`, `vertical/writer.py` `1f1c0f3efa0ab976c26bc7261d9e9703`, `horizontal_dataset.py` `9f899a8c226438747035f8da41aa3b42`, `tests/run_generalisation_acceptance.py` `a9b50c11531abe006d87061590fd85da`, `tests/framework/parity_gate.py` `8d09a71d8408fc2771950a4c9a7b369b`, `scripts/compare_cross_format_identity.py` `be9eb0c7c8aa71c398edb3a4a7b86e62`.

**Binaries.** `sage16 × shin-uchuu-lhalo` was built once serial and once `USE-MPI=yes` at `56ad1b8c` on a clean tree (`make clean`, `make … generate`, `make … TEST_BUILD=no -j16`; 0 warning and 0 error lines each) by `$V/scripts/build.sh` and copied out of the tree with its `make info`, the commit and its SHA-256: serial `68c6f983e5ad7be6c2ce814457a61b178d5e9fb836656feea6a439f9f611c83b` (`cc`, `-g -O2`, MPI disabled; `$V/bin/sage16__shin-uchuu-lhalo/`), MPI `bae32638f82a23875ed5b56ae1be369de5d0fb9a16f4de1c6240f91a7811692b` (`mpicc`, `-g -O2 … -DMPI`; `$V/bin/sage16__shin-uchuu-lhalo__mpi/`). The vertical runs ran these binaries from the repository root. The default pair's build was restored afterwards (`make clean && make generate && make -j16`; the log `$V/logs/restore-default-build.log` ends `Build complete` with 0 errors and 0 warnings).

**Environment.** `TMPDIR` was `$V/tmp` (outside the repository) for every step, as each log header shows. The llama router was running and idle throughout: `llama-server` held 78,784 KiB before each of the 20 steps, and no router process ever crossed the samplers' 200 MB threshold. Inputs, read only: the Stage R cut subset `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-r/cut/dataset` (abbreviated `$S`, 71 files, `du -sk` 56,090,604 KiB) and the production cut dataset `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-c/cut/dataset` (`du -sk` 3,082,797,480 KiB; both in `$V/logs/du-inputs-and-reference.txt`). Neither was written. The converter ran from the repository root with `--halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml`.

### The subset conversions (criterion 1)

**Plan and caps.** `plan --input $S --halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml --output <dir> --max-halos-per-file 12500000` (the forest cap left at its default, 1,000,000) gives **33 files**, the largest file 3 with 12,499,998 halos in 248,138 forests (`$V/logs/s02-subset-plan-w1.log`; the plan took 3.5 to 5.8 s at 0.401 GB in each of its four runs, and all four `file_plan.json` files have md5 `0b2d1af9183ab871931f8a0f823cb1e2`). The halo cap was chosen to give several files per worker at eight workers; a single-file conversion would not do, because the gate's stage 1 refuses one when a vertical dataset is named (a plan defect recorded for this run).

**Warm cache.** These runs read a 57 GB subset that the page cache holds. A timed read of every subset file (`$V/logs/s01-subset-prewarm.log`, `cat $S/* > /dev/null`) read 57,437,212,672 B from the LaCie in 146.90 s (391 MB/s). The four conversions then read between 2,478,080 and 8,679,424 B from the device in all (the step logs' `ioreg` counters): the page cache served every read, so the rates below are CPU and write rates, not the LaCie's read rate.

**The runs.** `convert --input $S --halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml --output $V/conv/subset-w<W> --tree-name shin-uchuu-cut --workers <W>`, at 1, 4 and 8 workers (`$V/scripts/subset_convert.sh`). Each converted all 33 files, 7,206,195 forests and 406,668,896 halos, and wrote 35,815,687,892 B (the manifest's file sizes). The totals pass is the time from the "deriving every forest's halo total" line to the printed bound; the conversion phase runs from "converting 33 file(s)" to "33 of 33 selected file(s) complete". The logical read is 406,668,896 halos × 132 B (the columns the assembler reads) = 53,680,294,272 B. Figures are extracted by `$V/scripts/conv_metrics.py` into `$V/logs/metrics-<step>.json`.

| Workers | Wall | Totals pass | Conversion phase | User / sys | Peak RSS (largest process) | Summed RSS (sampled) | Forests/s (wall) | Halos/s (phase) | Logical read, phase | Write, phase | Device write | `iostat` mean / max | Worker `%cpu` mean (sampled) | Log |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 259.59 s | 2.88 s | 254.95 s | 134.25 s / 52.18 s | 9.262 GB | 9.122 GB | 27,760 | 1,595,080 | 210.6 MB/s | 140.5 MB/s | 35,832,078,336 B | 131.3 / 209.65 MB/s | 73.4 | `$V/logs/s03-subset-convert-w1.log` |
| 4 | 128.99 s | 2.89 s | 125.35 s | 151.57 s / 59.54 s | 9.160 GB | 34.953 GB | 55,866 | 3,244,345 | 428.3 MB/s | 285.7 MB/s | 35,835,564,032 B | 266.6 / 393.27 MB/s | 43.2 to 46.3 | `$V/logs/s03-subset-convert-w4.log` |
| 8 | 115.70 s | 2.85 s | 112.09 s | 155.79 s / 64.72 s | 9.102 GB | 68.700 GB | 62,283 | 3,628,186 | 478.9 MB/s | 319.5 MB/s | 35,832,668,160 B | 297.0 / 403.06 MB/s | 19.8 to 27.4 | `$V/logs/s03-subset-convert-w8.log` |

Every run printed the same bound: `memory bound per worker: 20120830767 bytes (18.74 GiB): the assembler's 18.66 GiB for --window-rows 1048576 over 70 slabs, 132 B per row and 88 B per record, with the plan's largest forest of 1250835 halos; the 64.0 MiB re-read buffer, the 8.0 MiB write buffer and 7207935 B of header for the plan's most forests in a file` (8 workers: 149.91 GiB). Each worker peaked at 8.60 to 9.04 GB in the samples (`workers_sampled` in the metrics files), under half its bound.

**Worker scaling (warm).** Four workers are 2.01 times as fast as one, eight 2.24 times. At eight workers the LaCie took writes at a 309 MB/s device average over the step while the workers averaged 20 to 27 % CPU, consistent with a warm conversion bound by the LaCie's writes at about 300 to 400 MB/s (inferred from CPU and `iostat`; no run isolates the write path), so worker scaling above four is not measured here.

**Byte identity.** `$V/scripts/sha_subset.sh` (`$V/logs/s04-subset-sha256.log`) computed the SHA-256 of every file independently (`$V/logs/sha256-subset-<run>.txt`): the 1-, 4- and 8-worker conversions are identical in all 33 files, and each directory's `manifest.json` records exactly those digests.

**Window evidence (an addition from the run notes, not a criterion).** One more warm conversion, at 1 worker with `--window-rows 4194304` (`$V/logs/s03-subset-convert-w1-window4m.log`): 327.37 s wall (conversion phase 323.68 s), 22,012 forests/s, 1,256,388 halos/s, peak RSS 32.073 GB against a printed bound of 79,419,497,931 B (73.97 GiB), and byte-identical to the default-window conversion in all 33 files. On a warm cache the larger window only costs memory and time; its read-rate benefit on a cold LaCie is not measured here.

The conversion directories are `$V/conv/subset-w1`, `subset-w4`, `subset-w8` and `subset-w1-window4m`; every later step reads `$V/conv/subset-w1`.

### The production file plan and the cold measurement (criterion 2)

**The plan.** From the repository root (`$V/logs/s05-production-plan.log`):

```text
mimic_venv/bin/python convert/mimic-convert/write_vertical_trees.py plan \
  --input /Volumes/LaCie/data/uchuu/shin-uchuu-stage-c/cut/dataset \
  --halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml \
  --output /Volumes/LaCie/data/uchuu/shin-uchuu-cut-vertical-rehearsal/prod
```

Default caps (100,000,000 halos and 1,000,000 forests). 1,210.49 s wall (89.07 s user, 92.86 s sys), peak RSS 5.993 GB; it read 362,831,982,592 B from the LaCie (300 MB/s), the dataset's two columns cold. The kept plan is `$V/prod/file_plan.json` (md5 `071b080f67ed9d2557c934605fc32859`, SHA-256 `47836fef1c18a7a1aecbef8a6258cbf80411ad42cabb30b1727161c69b517e7b`), summarised by `$V/scripts/plan_stats.py` (`$V/logs/prod-plan-stats.txt`):

- **361 files**, 22,503,649,037 halos, 244,953,607 forests. The plan's Decision 4 expected about 260 to 320 files.
- **The largest file is file 0**: 126,380,326 halos in 1 forest (the super-forest), the only file above the halo cap.
- **The last file is file 360**: forests 244,931,335 to 244,953,606, 22,272 forests and 79,630 halos.
- **Distribution of halos per file:** minimum 79,630; 5th percentile 31,599,384; 25th 44,130,329; median 47,184,604; 75th 99,122,762; 95th 99,999,447; maximum 126,380,326. By bin: 1 file under 1 M halos, 1 of 1 M to 10 M, 7 of 10 M to 25 M, 208 of 25 M to 50 M, 21 of 50 M to 75 M, 8 of 75 M to 90 M, 114 of 90 M to 100 M and 1 above 100 M. 238 files close at the forest cap (1,000,000 forests), and 3 files hold a single forest (files 0, 168 and 169).
- **Forests per file.** Files 1 to 166 each close at the forest cap: 1,000,000 forests and 39,764,185 to 52,340,316 halos (median 45,664,453). File 167 holds 547,771 forests and 94,474,527 halos. Files 168 to 360 (14,678,205,284 halos) hold the cut's later `ForestIndex` range and vary widely: the median file holds 22,272 forests, 16 hold 10 forests or fewer (file 168 a single forest of 55,080,340 halos, file 180 seven forests of 95,564,067 halos in all), and files 288 to 359 (72 files) again close at the forest cap. The halos-per-file split by forest count depends strongly on the threshold: over the whole plan, files of at least 1,000 forests hold 15,466,838,732 halos and files of at least 100,000 forests 12,167,202,741; within files 168 to 360 the same thresholds give 7,767,775,305 and 4,468,139,314. So the plan does not divide into two clean regimes (`$V/logs/record-figures.txt`).

**The two files.** Two mid-plan files from the two ends of the forests-per-file range: **file 166** (forests 165,000,001 to 166,000,000: 1,000,000 forests, 44,817,424 halos, its largest forest 251,883 halos) and **file 180** (the plan's middle file: 7 forests, 95,564,067 halos, its largest 14,091,088). Together they hold 140,381,491 halos. Two files run on at most two workers, so the subset's worker choice (eight) could not apply; they ran on 2 workers into the plan's own directory (`$V/logs/s06-production-convert-166-180.log`):

```text
mimic_venv/bin/python convert/mimic-convert/write_vertical_trees.py convert \
  --input /Volumes/LaCie/data/uchuu/shin-uchuu-stage-c/cut/dataset \
  --halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml \
  --output /Volumes/LaCie/data/uchuu/shin-uchuu-cut-vertical-rehearsal/prod \
  --tree-name shin-uchuu-cut --workers 2 --only-files 166,180
```

- **Wall and phases.** 505.26 s wall (117.41 s user, 72.64 s sys), exit 0. The totals pass took 134.44 s and read between 70,873,088 B and 584,466,432 B from the device, bracketed by the 10 s samples 6.2 s before and 3.8 s after the bound line (`$V/logs/record-figures.txt`): the plan step had just read the same two columns, so it was warm, not cold. The conversion phase took 365.81 s and read the files' other columns cold.
- **Per file** (the manifest): file 166 in 321.5 s (worker 97392; 3,947,933,320 B) and file 180 in 365.1 s (worker 97391; 8,409,637,932 B). Per worker that is 139,382 and 261,728 halos/s, and 3,110 forests/s for file 166.
- **Rates over the conversion phase:** 383,752 halos/s and 2,734 forests/s with both workers (1,979 forests/s over the wall); file 166 finished 43.6 s before file 180, so both workers ran together for only about 88 % of the phase and this aggregate understates a fully concurrent pair; logical read 18,530,356,812 B (50.7 MB/s); written 12,357,571,252 B (33.8 MB/s).
- **Device traffic over the step:** 38,318,759,936 B read and 12,363,681,792 B written; `iostat` mean 96.1 MB/s over the whole step including the warm totals pass, maximum 287.37 MB/s (50 samples); over the conversion phase alone the device moved 136.95 to 138.35 MB/s (read less the totals pass's bracketed read, plus write, over 365.813 s; `$V/logs/projections.txt`). The workers averaged 9.2 % (file 180) and 6.3 % (file 166) CPU in the 10 s samples, consistent with a cold conversion bound by the LaCie's interleaved reads rather than by CPU (an inference, not a measurement).
- **Memory.** Peak RSS 18.356 GB, the largest process; `/usr/bin/time` does not name it, but it is probably file 180's worker, whose sampled peak (16.13 GB in 10 s samples) exceeds file 166's (10.10 GB).
- **The bounds.** The printed bound is for the plan's largest forest, whatever `--only-files` selects: `memory bound per worker: 86202994080 bytes (80.28 GiB) … with the plan's largest forest of 126380326 halos; … 21000000 B of header for the plan's most forests in a file; 2 worker(s): 160.57 GiB`. Each file's own bound, from `vertical.assemble.memory_bound` and the writer's buffers with its largest forest read from its own header (`$V/scripts/bounds.py`, `$V/logs/bounds-prod-166-180.txt`), is 19,922,666,604 B for file 166 against a sampled 10.10 GB, and 26,893,276,563 B for file 180 against the step's 18.356 GB peak.
- **Read excess.** The step's device read is 2.07 times the two files' logical read (38,318,759,936 B against 18,530,356,812 B). Netting out the totals pass's bracketed read leaves 19.20 to 19.72 GB of excess. Halved, that is a two-file average of 9.60 to 9.86 GB per file (9.894 GB before netting). One window per slab is 70 × 1,048,576 rows × 132 B = 9.689 GB, which lies inside that bracket: the residual is −0.087 to +0.170 GB, or 0.205 GB before netting (`$V/logs/record-figures.txt`). This is consistent with the bound's "one window of rows read beyond" each slab's block. The inference comes from the counters, not from a trace, and two files cannot show how the excess varies across the plan. It does not show on the warm subset.
- **Digests.** SHA-256 (`$V/logs/sha256-prod-166-180.txt`, equal to the manifest's) `a75acb56eb38a9b22aed4ecf00ca243913718d27a3494d99624f0859cbfede62` (file 166) and `510df7cea1672fb27273041f05933cf4282fa95d9537cec2618f966e7c8f5b6e` (file 180).

**Live-signal baselines for the procedure.** Per-worker CPU is in each conversion step's `.procs` and its means and maxima in its metrics file; the LaCie's throughput is in each step's `.iostat`. Cold, two workers: the LaCie moved 137 to 138 MB/s over the conversion phase (96 MB/s over the whole step with its warm totals pass; up to 287 MB/s), at 6 to 9 % CPU per worker. Warm, eight workers: the LaCie took writes at about 300 MB/s, at 20 to 27 % CPU per worker. Sequential reads: 391 MB/s (pre-warm) and 300 MB/s (the plan's two-column pass).

### The halo-level proof (criterion 3)

`convert/mimic-convert/tests/run_generalisation_acceptance.py`, record `$V/dump/harness-record.json` (every entry: commit `56ad1b8c`, `git_dirty_paths 0`, harness SHA-256 `7c299c7c55a8b9c8c4bfa3399a5d4ccefcd538ef1bc1be1c4bd43d8df950028d`), child logs `$V/dump/logs/` (`$V/scripts/halo_proof.sh`, then `halo_proof_b.sh`).

| Subcommand | Verdict | Wall | Peak RSS | Log |
|---|---|---|---|---|
| `build-dump --model halos-only --simulation shin-uchuu-lhalo --build-dir $V/build-dump` | exit 0; tool SHA-256 `450413e1335fa56cf983459454185c15ef1645dfd9188c3e3e06f4c0b4dda6c3` | 5.04 s | 0.056 GB | `$V/logs/s07-build-dump.log` |
| `dump --tool $V/build-dump/dump_ctrees_topology --run-file $V/runfiles/dump_halos-only_shin-uchuu-lhalo.yaml --out $V/dump/subset.dump` (`--source-payload`, which the harness passes) | exit 0; 69,997,171,255 B, SHA-256 `4654f501639c5d1e8f789b8d1de6206c54202303986cd0c358f8db0371aac221` | 682.36 s (the harness records 654.29 s for the tool itself) | 1.430 GB | `$V/logs/s08b-dump.log` |
| `compare --dataset $S --source-format lhalo_binary --dataset-source-format consistent_trees_ascii --dump $V/dump/subset.dump --spill-dir $V/spill --budget-mb 16384 --report $V/dump/compare-report.json` | **PASS** (exit 0), `failed_checks []` | 6,081.23 s (5,000.89 s user) | 17.513 GB | `$V/logs/s09b-compare.log` |

**The dump's run file.** `$V/runfiles/dump_halos-only_shin-uchuu-lhalo.yaml` is the committed `halos-only_shin-uchuu-lhalo.yaml` plus an `input` section (`simulation_dir: $V/conv/subset-w1`, `first_file: 0`, `last_file: 32`), with a scratch `output_directory` and `output_format: binary`. The first `dump` (`$V/logs/s08-dump.log`, exit 1 in 0.31 s, kept in the harness record) used `output_format: hdf5` and stopped with `ERROR: OutputFormat 'hdf5' requires HDF5 support`, because the dump tool is built without HDF5. The binary format matches Stage A's dump run file. The rerun is the row above.

**What `compare` checked.** `compare` matched 406,668,896 converted rows to 406,668,896 reference rows. Each of its 25 named checks has 0 failures. Every check below compared 406,668,896 rows except where another count is given:

- `converter_link_encoding`;
- `converter_link_targets`, 1,240,070,419 links;
- `dump_integrity`;
- `duplicate_converted_rows` and `duplicate_reference_rows`, each reporting 0 compared and 0 failures (no duplicate found);
- the five links, `link_Descendant`, `link_FirstHaloInFOFgroup`, `link_FirstProgenitor`, `link_NextHaloInFOFgroup` and `link_NextProgenitor`;
- the eight payload fields, `payload_Len`, `payload_M_Crit200`, `payload_MostBoundID`, `payload_Pos`, `payload_Spin`, `payload_Vel`, `payload_VelDisp` and `payload_Vmax`;
- `payload_storage`, 420 datasets;
- `row_coverage`;
- `snapnum`;
- `source_halo_id`;
- the three target snapshots, `target_snapshot_Descendant`, `target_snapshot_FirstProgenitor` and `target_snapshot_NextProgenitor`.

**Spill.** The four external sorts each merged in one pass: both halo sorts in 44 runs, 84,587,130,368 B each; the link-request sort in 37 runs and the resolved-link sort in 42, 49,602,816,760 B each. In all they spilled 268,379,894,256 B (the report's `resources`). The spill directory, sampled every 30 s (`$V/logs/s09b-compare.spill`), peaked at 264,235,912 KiB, and LaCie use rose by 264,154,720 KiB during the step. The spill directory was empty afterwards. The budget of 16,384 MiB (the default is 512) was chosen to keep the merges to one pass; it splits into 4,294,967,296 B per sorter.

### The gate (criterion 4)

`MIMIC_SHIN_UCHUU_CUT_SUBSET_LHALO=$V/conv/subset-w1 MIMIC_SHIN_UCHUU_CUT_SUBSET_DATASET=$S make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests-scientific` (`$V/scripts/gate.sh`, `$V/logs/s10-gate.log`): exit 0 in 2,804.42 s, peak RSS 9.712 GB (the largest process), `Scientific Test Summary: passed=55 failed=0 skipped=0 n/a=0 warned=1`. The warning is the tier's `test_zero_values` ("2 field(s) with zero values"), not the gate. Every gate stage passed (`stage_preconditions`, `stage_dataset_provenance`, `stage_run_files`, `stage_builds`, the four legs, `stage_leg_verdicts`), and all four legs PASS. The leg wall time runs from the leg's banner to its PASS line. Run times are the gate's `done in` lines. Counts and times are extracted by `$V/scripts/gate_metrics.py` into `$V/logs/gate-metrics.txt`.

**Peak RSS per leg is a bounded measurement.** The gate neither measures RSS nor keeps its runs, so no per-leg high-water mark exists. The lower bound is, for each process of the leg (its vertical `./mimic` run, its horizontal run and its comparator), the largest RSS among the 2 s samples that caught the process above the sampler's 200 MB threshold, with the number of samples it rests on. These come from `$V/logs/s10-gate.procs`; processes are assigned to legs by run key and by the leg's time window (`$V/scripts/gate_rss_bounds.py`, `$V/logs/gate-rss-bounds.txt`). The upper bound for every leg is the gate step's `/usr/bin/time -l` peak, 9.712 GB, the largest single process in the step.

| Leg | Galaxies (fields, snapshots) | Leg wall | Vertical run wall | Horizontal run wall | Peak RSS lower bounds (samples): vertical / horizontal / comparator | Peak RSS upper bound |
|---|---|---|---|---|---|---|
| `halos-only`/fixed | **92,429,377** (20, 8) | 337.9 s | 183 s | 92 s | 0.889 GB (1) / 7.438 GB (39) / 6.473 GB (27) | 9.712 GB |
| `halos-only`/dynamic | 92,429,377 (20, 8) | 349.1 s | 187 s | 99 s | 1.125 GB (5) / 8.938 GB (44) / 6.425 GB (28) | 9.712 GB |
| `sage16`/fixed | **60,033,537** (42, 8) | 1,236.4 s | 629 s | 543 s | 1.649 GB (262) / 9.711 GB (242) / 5.207 GB (27) | 9.712 GB |
| `sage16`/dynamic | 60,033,573 (42, 8) | 840.8 s | 429 s | 346 s | 1.649 GB (178) / 9.711 GB (153) / 5.219 GB (27) | 9.712 GB |

The fixed legs' counts equal the cut subset's recorded horizontal figures (92,429,377 and 60,033,537); the dynamic counts are as measured. Galaxies per output snapshot, from the PASS lines:

- `halos-only`, both schemes: {24: 5,521,907, 27: 7,044,956, 30: 9,096,015, 32: 10,527,664, 37: 12,775,053, 44: 14,752,995, 48: 15,524,201, 69: 17,186,586}.
- `sage16`/fixed: {24: 4,678,915, 27: 5,730,433, 30: 7,089,852, 32: 7,938,095, 37: 8,710,659, 44: 8,983,501, 48: 8,908,915, 69: 7,993,167}.
- `sage16`/dynamic: {24: 4,678,891, 27: 5,730,394, 30: 7,089,846, 32: 7,938,087, 37: 8,710,675, 44: 8,983,544, 48: 8,908,968, 69: 7,993,168}.

Each vertical run read 33 partitions against the horizontal run's 8. The gate's worktrees and runs were in the repository's `output/` on Internal, whose use rose by at most 121,879,540 KiB during the gate and ended 47,560 KiB above its starting value once the gate had removed them. These timings are context; the projections use the kept serial run below.

### The vertical `sage16` runs (criterion 5)

**The run files.** Both are the committed `sage16_shin-uchuu-lhalo.yaml` with an `input` section (`simulation_dir: $V/conv/subset-w1`, `first_file: 0`, `last_file: 32`) and `output_directory` set to `$V/runs/sage16-subset-serial` or `$V/runs/sage16-subset-np4` (`$V/runfiles/`). Both ran from the repository root through `$V/scripts/vertical_runs.sh`.

| Run | Wall | User / sys | Peak RSS | Halos/s | Output (`du -sk`) | Log |
|---|---|---|---|---|---|---|
| serial (`$V/bin/sage16__shin-uchuu-lhalo/mimic`) | 662.39 s | 542.59 s / 81.68 s | 1.684 GB | 613,942 | 16,022,220 KiB | `$V/logs/s11-sage16-serial.log` |
| `mpirun -np 4 -x MallocLargeCache=0` (`$V/bin/sage16__shin-uchuu-lhalo__mpi/mimic`, each rank under `$V/scripts/rank_time.sh`) | 206.20 s (each rank 202.96 s) | per rank 158.09 to 161.56 s / 22.39 to 25.03 s | per rank: 0: 1.695 GB, 1: 1.310 GB, 2: 1.445 GB, 3: 1.253 GB | 1,972,206 | 16,017,376 KiB | `$V/logs/s12-sage16-np4.log`, `$V/logs/s12-sage16-np4-rank<k>.time` |

**Serial memory.** The serial run's own memory profile: `Peak process RSS: 1.684 GB`, output buffer capacity 6,254,175 records (1.151 GB), output population 1,241,489, galaxy-pool high-water 1,300,702 galaxies (0.229 GB). It processed 33 input files and 7,206,195 forests (the largest 1,250,835 halos), 10,879 forests/s. Its galaxy counts per snapshot, from the serial-against-`-np 4` comparator's log (`$V/logs/s13-compare-serial-np4.log`), equal the gate's `sage16`/fixed counts.

**Serial against `-np 4`.** `scripts/compare_cross_format_identity.py $V/runs/sage16-subset-serial/model $V/runs/sage16-subset-np4/model --left-label serial --right-label np4`, default mode (`$V/logs/s13-compare-serial-np4.log`, 64.79 s, 5.317 GB) reported: `PASSED: 60033537 galaxies over 8 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates`. Under `-np 4` the run was 3.21 times as fast as serial.

### Scratch ledger

Measured with `du -sk` at the end (`$V/logs/du-end-detail.txt`; the step logs' own `du` lines differ by under 1 %). No scratch directory was moved to the trash or deleted, and every one is left in place; the gate removed its own worktrees and runs, and the restore's `make clean` cleaned the build tree.

| Scratch | Where | `du -sk` |
|---|---|---|
| Subset conversions `$V/conv/subset-w1`, `subset-w4`, `subset-w8`, `subset-w1-window4m` (33 files, `file_plan.json`, `manifest.json` each) | LaCie | 35,194,008; 35,089,320; 35,129,088; 35,165,928 KiB |
| The production plan and the two cold files, `$V/prod` (`file_plan.json`, `manifest.json`, `shin-uchuu-cut.166`, `shin-uchuu-cut.180`) | LaCie | 12,068,008 KiB |
| The dump and the harness record and report, `$V/dump` | LaCie | 68,356,660 KiB |
| The comparator's spill, `$V/spill` | LaCie | 0 KiB after; 264,235,912 KiB at its sampled peak |
| The kept vertical runs, `$V/runs/sage16-subset-serial` and `sage16-subset-np4` | LaCie | 16,022,220 and 16,017,376 KiB |
| Binaries, the dump tool, logs and scripts (`$V/bin`, `$V/build-dump`, `$V/logs`, `$V/scripts`) | LaCie | 1,244; 1,040; 18,752; 64 KiB (`scripts` in `$V/logs/du-end.txt`) |
| The gate's worktrees and runs, under the repository's `output/` | Internal | removed by the gate on exit; peak 121,879,540 KiB |

**Free space.** The whole `$V` holds 253,063,716 KiB (`$V/logs/du-end.txt`). LaCie free space was 4,249,402,324 KiB before the rehearsal (the first log's header, `$V/logs/test-wrapper.log`) and 3,996,303,076 KiB after (`$V/logs/df-end.txt`). Its use peaked at 4,049,069,596 KiB during `compare`, 485,491,476 KiB (497.1 GB) above the first log's 3,563,578,120 KiB. Internal free space was 442,215,272 KiB before (the first log's header) and 442,215,484 KiB after.

### Production projections

Computed by `$V/scripts/projections.py` (`$V/logs/projections.txt`) from the figures above. They are projections for planning, not measurements.

**The owner's stop rule:** none: projections are for planning only (the owner's decision of 2026-10-18, recorded by PM in this run's notes). The non-time stops stand: a verification or identity failure, or a peak above the ledger's allowance.

**The conversion.** 22,503,649,037 halos in 361 files. Every projection beyond two concurrent workers is an extrapolation: the cold measurement ran two files on two workers, and the subset runs were warm.

- **At the measured two-worker aggregate** (383,752 halos/s, which understates a fully concurrent pair): 16.29 h.
- **Bracketed by the two per-worker rates.** Two files support no split of the plan into regimes, so the whole dataset is bracketed by the two measured per-worker rates: at file 166's 139,382 halos/s it needs 44.85 worker-hours (22.42 h on 2 workers, 11.21 h on 4, 5.61 h on 8), and at file 180's 261,728 halos/s 23.88 worker-hours (11.94, 5.97 and 2.99 h), if each worker kept its two-worker rate.
- **The device bound.** A worker can keep its rate only while the disk has capacity left, and the cold workers appear to have waited on the LaCie (6 to 9 % CPU). The device traffic is 2.970 TB of logical reads, plus 361 × 9.60 to 9.86 GB of read excess at the two-file average (6.437 to 6.529 TB read in all), plus 1.981 TB written: 8.418 to 8.511 TB. That is 17.07 to 17.09 h at the two-worker conversion phase's combined 136.95 to 138.35 MB/s, 13.44 to 13.59 h at the 174 MB/s the plan cites for interleaved 8 MB reads, 7.79 to 7.88 h at the 300 MB/s of the plan's two-column pass, and 5.98 to 6.05 h at the 391 MB/s of the sequential pre-warm read.
- **The range.** The device-bound figures span 5.98 to 17.09 h; with more workers the conversion is expected to be device-bound inside that range rather than at the CPU-ideal 3 to 6 h on 8 workers. This is a judgement on the figures above, not a measurement.
- **The totals pass.** Every `convert` invocation re-derives the totals first: 1,210.49 s cold (the plan step); 134.44 s warm.
- **The read excess.** Extrapolated from the two-file average, it more than doubles the logical read at production scale (6.437 to 6.529 TB against 2.970 TB, 2.17 to 2.20 times; about 3.5 to 3.6 TB over 361 files). It costs time, not correctness.

**The super-forest.** Its conversion bound from the assembler's function, for its 126,380,326 halos at the default window over 70 slabs, is 86,106,496,608 B. A worker converting file 0 holds 86,181,994,101 B (86.18 GB) with the writer's buffers and its one-forest header. The bound the converter prints for every production invocation, 86,202,994,080 B, adds the 21,000,000 B header of the plan's most forests in a file. At `--window-rows 4194304` the worker bound is 144,315,047,541 B (144.32 GB). On the subset the largest process peaked at 0.45 to 0.46 of its bound at the default window, and at 0.40 of it at 4,194,304 rows.

**The vertical run.** At the kept serial run's 613,942 halos/s, the 22,503,649,037 halos take 10.18 h serial. The cut horizontal reference took 44,401 s (12.33 h; [`MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md`](MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md), line 690). At the `-np 4` subset rate, 1,972,206 halos/s, they take 3.17 h, if memory allowed four ranks at the super-forest. Both assume cost per halo does not grow with forest size. This rehearsal's largest forest is 1,250,835 halos; nothing here measures the super-forest's run.

**The disk ledger.**

| Item | Size |
|---|---|
| The vertical dataset: 88 B × 22,503,649,037 records + 8 B × 361 file headers + 4 B × 244,953,607 forest counts (the formula reproduces files 166 and 180 to the byte) | 1,981,300,932,572 B (1.981 TB), plus `file_plan.json` and `manifest.json` |
| The `sage16` output at the reference's output snapshots [69, 52, 43, 40, 20, 10, 5, 2, 1, 0] (the reference's `metadata/run.yaml`, `output.snapshot_list`; the plan's "The reference" paragraph, line 62): the reference itself (`/Volumes/Internal/results/mimic/sage16-shin-uchuu-v3-cut`), on the plan's expectation that the vertical run holds the same galaxies and fields, not a measurement | 531,055,128 KiB (0.544 TB; `$V/logs/du-inputs-and-reference.txt`); the vertical run writes one output file per input file (361) rather than one per snapshot |
| LaCie free before the rehearsal / after it | 4,249,402,324 KiB (4.351 TB) / 3,996,303,076 KiB (4.092 TB) |
| LaCie free after the production dataset and output, with this rehearsal's scratch kept | about 1.567 TB (4.092 − 1.981 − 0.544); 1.826 TB if the scratch is trashed |
| Internal free before / after | 442,215,272 KiB / 442,215,484 KiB; the reference stays there |

### Deviations and findings

- **The file count.** The production plan has 361 files, against the plan's expectation of about 260 to 320. Files 168 to 360 (193 files) hold the cut's later `ForestIndex` range, from single forests to 1,000,000 forests per file.
- **The read excess.** The cold conversion read a two-file average of 9.60 to 9.86 GB per file above its rows, netted of the totals pass (the step's device read is 2.07 times the logical read), consistent with one window per slab read beyond the file's last forest. At production it would add about 3.5 to 3.6 TB of reads, an extrapolation from two files. This is a performance finding for the owner, not a correctness defect: the two production files passed the converter's write-verify and the subset conversion passed the halo-level proof and the gate. No code was changed.
- **"Cold" holds for the conversion phase only.** The totals pass reread two columns that the plan step had just left in the page cache.
- **The spill.** `compare`'s spill was 264,235,912 KiB (270.6 GB) at its sampled peak (268.4 GB spilled in all by its four sorts), against the plan's estimate of about 170 GB, at a budget of 16,384 MiB rather than the default.
- **The dump run file** needed `output_format: binary`; the failed first attempt is kept in its log and the harness record.
- **The gate's per-leg peaks** are bounds: lower bounds from 2 s samples (as few as 1 for the `halos-only`/fixed vertical run), and the gate step's `/usr/bin/time -l` peak (9.712 GB) as the upper bound, because the gate neither measures RSS nor keeps its runs.
- **Criterion 2's worker count.** Criterion 2 asks for the two cold files "at the worker count chosen from the subset runs". Two files run on at most two workers, so the subset's eight could not apply, and they ran on two. This is a plan defect that the owner accepted on 2026-10-18; every projection beyond two workers is an extrapolation.
- **The wrapper's smoke test.** `$V/LEDGER.md`'s first row (`test-wrapper`, `sleep 2`) tested the wrapper before the first step.
