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

---

## Production

The owner-operated production procedure (the plan's "The production procedure", steps 1 to 10), run in Mode A from 2026-10-18 UTC. Logs live under `/Volumes/LaCie/data/uchuu/shin-uchuu-cut-vertical/procedure/` (abbreviated `$P` below; logs `$P/logs/`, scripts `$P/scripts/`, frozen binaries `$P/bin/`, run files `$P/runfiles/`, ledger `$P/LEDGER.md`, trash log `$P/TRASH.md`). The vertical dataset is `/Volumes/LaCie/data/uchuu/shin-uchuu-cut-vertical/dataset` (abbreviated `$D`) and the runs are under `/Volumes/LaCie/data/uchuu/shin-uchuu-cut-vertical/runs` (abbreviated `$O`).

### Owner decisions before step 1

Taken on 2026-10-18 before step 1 (HANDOFF's "Before the procedure"):

- **The converter's read excess: proceed as is.** The converter is used unchanged at `944364d4`, with the same source md5s as the rehearsal. The fix (clamping each slab's last window to the file's last row) stays a recommended follow-up. The trade-off weighed was this. If the conversion is disk-bound at the cold baseline of about 137 to 175 MB/s, the fix would save a gross 5.5 to 7 h, or about 3 h at 300 MB/s. A separate reviewed slice would cost 2 to 3 h. That left a likely net gain of 3 to 5 h, against the risk of touching the assembler before production.
- **The llama router: stopped by the owner for the whole procedure.** The service `com.dcroton.llama-router` is no longer loaded and `llama-server` is gone (`$P/logs/host.txt`). The router's lightweight front-end `llm-gate.py` (pids 914 and 959, 22,912 and 48,096 KiB) stays running; every step log's header records it.
- **Step 6 runs serial**, confirmed against step 3's measured file-0 run (Decision 6). The owner is asked again only if the projection exceeds 24 h. **Changed by the owner at 08:52Z (19:52 AEDT): step 6 runs under `mpirun -np 4`**, the decision Decision 6 leaves to the owner. Serial had been taken from the 24 h threshold; nothing measured argued against MPI. The case: the rehearsal's `-np 4` run was bitwise identical to serial on the subset and 3.21 times as fast. The serial projection of 8.6 to 10.2 h becomes about 3 to 3.5 h. Memory per rank is about 1,304 B per halo of the rank's largest forest (step 3), so file 0 on rank 0 coincides only with small-forest files, about 225 GB. The big-forest files 168 to 171, reached together under round-robin striding, total about 300 GB, both inside 549.8 GB. These are the pre-launch estimates; the header check that replaced them is in step 6. Eight ranks were rejected: the big-forest files 168 to 175 together would need about 450 to 500 GB, and the LaCie could not feed eight ranks. Before launch, each rank's largest forests are read from the converted files' headers. If the worst concurrent peak leaves under 150 GB free, the run falls back to `-np 2`.
- **The dataset directory** is `$D`, separate from the rehearsal's `$V/prod`. The procedure's log root is `$P`, and the runs go under `$O`.
- **The rehearsal scratch `$V` is kept** through the procedure.
- **Step 7's one-line commit is pre-approved** by the owner. No other commit is.
- **Step 9 (the optional science table) is skipped.** Step 8's identity implies it.
- **Step 10's NT transfers are run by the assistant.** They run one at a time, detached and checksummed.
- **The dataset's transfer runs beside steps 6 to 8** (changed by the owner at 09:05Z, 20:05 AEDT). The dataset's transfer starts as soon as step 4's manifest proves the dataset complete, rather than after step 8. This relaxes the procedure's own "one LaCie-bound job at a time" discipline; the plan's contracts are unchanged. The reasons:
  - Every file is write-verified and its SHA-256 recorded before step 6 starts, so the dataset is final.
  - The transfer is network-bound at about 105 MB/s of sequential reads.
  - A slower step 6 matters less than the longer network chain.
  - A failed step 8 would not invalidate the dataset's copy, and nothing on NT is deleted without the owner.

  The output's transfer follows the dataset's, and only after step 8 passes. The dataset is checked on NT against the conversion manifest's SHA-256s, with no separate local hashing pass. The four `.stale-*` partials are excluded.
- **Step 6's start is chained to step 4's end** by `$P/scripts/chain.sh`, launched at 09:09:56Z:
  1. wait for step 4's exit 0;
  2. run `$P/scripts/verify_manifest.py`, which checks that every planned file is complete at its recorded size, that no `.partial` remains, and that the totals are 244,953,607 forests and 22,503,649,037 halos, then writes the transfer's SHA-256 list;
  3. run `$P/scripts/rank_memory.py`, which takes the rank count from the converted files' headers;
  4. start the dataset's `rsync` and its NT check in the background;
  5. run step 6.

  A failed gate stops the chain. The chain was dry-run with stubbed actions before launch. Step 7, step 8 and the output's transfer are run by hand.

### Method

The discipline is the rehearsal's (see "Rehearsal", Method):

- Every step runs through `$P/scripts/step.sh <name> <command>`. This is the rehearsal's wrapper with the log root changed, and with `rsync` and `shasum` added to the sampler's process pattern. Each step runs as `MallocLargeCache=0 /usr/bin/time -l <command>`.
- Long steps run inside a sequence script under `$P/scripts/`, launched detached under `caffeinate -i`.
- Every step appends a row to `$P/LEDGER.md`.
- `TMPDIR` is `$P/tmp`, outside the repository.
- Only one LaCie-bound job runs at a time, except the dataset's NT transfer beside steps 6 to 8 (owner's decision above).
- The wrapper's smoke test is `$P/logs/p00-test-wrapper.log`.

### Step 1: preconditions and ledger

**Code under test.** Each log header records the commit and `git status --porcelain`. Steps 0 to 8, and step 10's `p10a` and `p10c`, ran at `944364d41044eda4201916bc38d1aaa00e2bb418` on `feature/shin-uchuu-cut-vertical`. The later steps ran at step 7's commit `5dd6fcfe`, which changes only `last_file`: `p09a`, `p09b`, `p10a2`, `p10b`, `p10d`, `p10e`, `p11a` and `p11b`. The only dirty path at any step is this record. Step 7's package tests ran with `simulation_info.yaml` also modified, before its commit. The source md5s are the rehearsal's, identical in every log (`$P/logs/p00-test-wrapper.log` onwards): `write_vertical_trees.py` `81974e7701b5c305b4b6d8572210abb3`, `vertical/assemble.py` `cb966fd488770e363c021290bcebd70a`, `vertical/plan.py` `5b5ea7495b8a6ca42f68362527ed8027`, `vertical/record.py` `d8233ed4be93520f895857247b43c942`, `vertical/writer.py` `1f1c0f3efa0ab976c26bc7261d9e9703`, `horizontal_dataset.py` `9f899a8c226438747035f8da41aa3b42`, `scripts/compare_cross_format_identity.py` `be9eb0c7c8aa71c398edb3a4a7b86e62`.

**Tests at the procedure's commit.** All ran with both `MIMIC_SHIN_UCHUU_CUT_SUBSET_*` variables unset:

| Command | Result | Log |
|---|---|---|
| `make tests-converter` | `passed=1620 failed=0 skipped=1`. The skip is the case-variant inventory test on this case-sensitive filesystem. | `$P/logs/p01-tests-converter.log` |
| `make MODEL=sage16 SIMULATION=shin-uchuu-lhalo tests` | `ALL TESTS AND CHECKS PASSED`. validate-modules `Found 20 module(s)` and `Validation PASSED`. Converter `passed=1620`, unit `passed=304`, integration `passed=123`, scientific `passed=47 skipped=1 warned=1`. The skip is the cross-format gate's `test_subset_not_named`, as designed. The warning is `test_zero_values`. | `$P/logs/p01-tests-sage16-shin-uchuu-lhalo.log` |
| `make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests-unit tests-integration tests-scientific` | unit `passed=304`, integration `passed=123`, scientific `passed=47 skipped=1 warned=1` (the same skip and warning) | `$P/logs/p01-tests-halos-only-shin-uchuu-lhalo.log` |

The default pair's build was restored afterwards (`$P/logs/p01-restore-default-build.log`).

**Inputs resolve.** The cut dataset `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-c/cut/dataset` holds 71 files. The reference `/Volumes/Internal/results/mimic/sage16-shin-uchuu-v3-cut/` holds `model.hdf5`, ten `model_<snap>.hdf5` and `metadata/` with `run.yaml`, `simulation_info.yaml`, `shin-uchuu.a_list`, `version_info.json` and `output_schema.json`.

**The production file plan is reproduced.** The command is the rehearsal's exactly, with `--output $D`, run from the repository root with `--halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml` (`$P/logs/p01-plan.log`). It exited 0 in 1,965.11 s (90.02 s user, 89.01 s sys) at a peak RSS of 5.993 GB. It read 363,432,349,696 B from the LaCie, 184.9 MB/s over the step. `$D/file_plan.json` has md5 `071b080f67ed9d2557c934605fc32859`, identical to `$V/prod/file_plan.json`, and SHA-256 `47836fef1c18a7a1aecbef8a6258cbf80411ad42cabb30b1727161c69b517e7b` (`$P/logs/p01-plan-md5.txt`). It plans 361 files, 22,503,649,037 halos and 244,953,607 forests (`$P/logs/p01-ledger.txt`).

The pass was 1.6 times slower than the rehearsal's 1,210.49 s (300 MB/s). The cause is a deviation of this procedure: the test suites above ran at the same time with `TMPDIR` on the LaCie (`$P/tmp`), so a second LaCie-bound job overlapped the plan's read. Microsoft Defender's scanner also averaged 30.3 % CPU in the plan's samples, against 13.5 % in the rehearsal's (`$P/logs/p01-plan.procs`). The md5 predicate is unaffected. From here on, `TMPDIR` for test and build work is on Internal (`/Volumes/Internal/tmp-mimic-procedure`), and no test runs beside a LaCie-bound step.

**The ledger** (`$P/scripts/ledger.py`, output in `$P/logs/p01-ledger.txt`):

| Retained (`du -sk`) | Size |
|---|---|
| The cut horizontal dataset (read only) | 3,082,797,480 KiB |
| The rehearsal scratch `$V` (kept) | 252,263,396 KiB |
| The procedure root, before step 3 | 4,428 KiB |
| The reference (Internal) | 531,055,128 KiB |

Free space at step 1 was 3,999,408,712 KiB (4.095 TB) on the LaCie and 442,044,232 KiB on Internal. The vertical dataset is 1,981,300,932,572 B by the plan's formula; file 0 is 11,121,468,700 B. The LaCie keeps its required free space at every step:

| After step | LaCie free | Required |
|---|---|---|
| 3 | 4.082 TB | — |
| 4 (with up to 40 GB of stale files from one restart at 4 workers) | 2.074 TB | 200 GB |
| 6 (with an output the reference's size) | 1.530 TB | 300 GB |

Memory has 549.8 GB of RAM with the router stopped. Against the rehearsal's coefficients:
- step 3's conversion bound is 86.18 GB;
- the file-0 runs are modelled at 166 to 205 GB (allow 250 GB);
- step 4's largest-first ceiling at 4 workers is about 290 GB;
- step 8 needs about 297 GB.

Each leaves at least about 250 GB of headroom.

### Step 2: reconciliation

**Configuration.** Read against the reference's `metadata/` (`run.yaml`, `simulation_info.yaml`, `shin-uchuu.a_list`, `version_info.json`; built at `b4903283`, `v1.2-197-gb4903283`).

These are identical:
- the `modules` block (pre-timestep, both phases and their processing modes, an empty post-timestep, every parameter) and `SubSteps: 10` of the committed `models/sage16/input/sage16_shin-uchuu-lhalo.yaml` (compared as parsed YAML);
- `output_filename: model` and `output_format: hdf5`;
- the a_list (md5 `044f1b6c0cbe3f2df92a6b15eac83228` in the reference, in `simulations/shin-uchuu-lhalo/` and in `simulations/shin-uchuu/`);
- the package's whole `simulation:` block (multiplier, cosmology, box, particle mass), compared with the reference's `simulation_info.yaml`.

The expected differences are:
- `simulation.name` (`shin-uchuu-lhalo` against `shin-uchuu`);
- the `input` section (the vertical dataset and file range against the horizontal `forest_chunks: 128`);
- the output directory.

**Code.** `git diff --stat b4903283 944364d4 -- src models/sage16` touches five files:
- `src/core/horizontal_driver.c`, `src/io/horizontal/read_horizontal_hdf5.c` and `src/io/horizontal/reader.h`: the horizontal reader and driver (the deletion of the version 2 path, `e217b01b`, `adf982bb`);
- `src/include/types.h`: comments only, in the horizontal structs' documentation;
- `models/sage16/input/sage16_shin-uchuu-lhalo.yaml`: this plan's run file.

Outside these paths:
- `Makefile` changes only test targets and help text (the `check-horizontal-fixture` removal);
- `simulations/shin-uchuu/halo_properties.yaml` changes only comments and `description` strings of the horizontal package.

None reaches the vertical `sage16` path. `src/io/vertical/`, the vertical driver, the modules and the generator are unchanged.

**Binaries.** `$P/scripts/build.sh` (the rehearsal's) built `sage16 × shin-uchuu-lhalo` and `halos-only × shin-uchuu-lhalo` once each, serial, at `944364d4` on a clean tree. This record was stashed for the build and restored byte-identical afterwards (`cmp` against a copy, in the session). Each build ran `make clean`, `make … generate` and `make … TEST_BUILD=no -j16`, with 0 warning and 0 error lines. Both use `cc` with `-g -O2`, HDF5 enabled and MPI disabled (`$P/bin/<tag>/make_info.txt`):

| Binary | SHA-256 | Where |
|---|---|---|
| `sage16` | `5f6930a9cb2b6bbcf099e918f8ecffdde3d4ddcd55235582d329b299228abdf6` | `$P/bin/sage16__shin-uchuu-lhalo/` |
| `halos-only` | `7d86d2a7b337270284440ecfbe4734e9edbf6afbded4a8bdeada9d77b292070c` | `$P/bin/halos-only__shin-uchuu-lhalo/` |

The MPI build for step 6 (owner's decision above) was made the same way with `USE-MPI=yes` while step 4 ran (CPU and Internal only; the record stashed and restored byte-identical; the default build restored afterwards, `$P/logs/p06-restore-default-build-after-mpi.log`): `sage16 × shin-uchuu-lhalo` MPI, `mpicc`, `-g -O2 … -DMPI`, 0 warning and 0 error lines, SHA-256 `cfad8fcd05384ac5a12609d01cce17d0c2f4bbe0a25f80e17168afca176e2a13` (`$P/bin/sage16__shin-uchuu-lhalo__mpi/`). Against the reference it differs in task count and the MPI compiler wrapper only, which the rehearsal showed leaves the output bitwise identical. The reference's `cc -O2` serial build matches in compiler, optimisation and task count; its `version_info.json` names the compiler `gcc 4.2.1`, Apple clang's compatibility string. The default pair's build was restored afterwards (`$P/logs/p02-restore-default-build.log`).

**Run files** (`$P/runfiles/`) are each the committed run file with `output_directory` under `$O` and an `input` section (`simulation_dir: $D`, `first_file: 0`, `last_file`):

| Run file | Source | `last_file` | Snapshot list | md5 |
|---|---|---|---|---|
| `sage16-vertical.yaml` | `sage16` | 360 | the reference's [69, 52, 43, 40, 20, 10, 5, 2, 1, 0] | `3ba9e0e15b64c46510f101390c414d21` |
| `file0-sage16.yaml` | `sage16` | 0 | the committed list | `b8bdfd1df15137e2717383f9b249c8c3` |
| `file0-halos-only.yaml` | `halos-only` | 0 | the committed list | `39cd7637a5a1a1854479a15b138b372f` |

### Step 3: file 0 alone

The router was stopped and memory was 98 % free (`memory_pressure`) at launch. The sequence is `$P/scripts/s03_file0.sh`, sampled every 5 s.

| Step | What | Wall | Peak RSS | Log |
|---|---|---|---|---|
| `p03a-convert-file0` | `convert --input <cut dataset> --halo-properties simulations/shin-uchuu-lhalo/halo_properties.yaml --output $D --tree-name shin-uchuu-cut --workers 1 --only-files 0`, exit 0 | 329.25 s (105.18 s user, 67.93 s sys) | 69.344 GB | `$P/logs/p03a-convert-file0.log` |
| `p03b-sha256-file0` | `shasum -a 256 $D/shin-uchuu-cut.0`, exit 0 | 23.53 s | 0.008 GB | `$P/logs/p03b-sha256-file0.log` |
| `p03c-file0-halos-only` | `$P/bin/halos-only__shin-uchuu-lhalo/mimic $P/runfiles/file0-halos-only.yaml`, exit 0 | 63.62 s (34.70 s user, 8.17 s sys) | 142.576 GB | `$P/logs/p03c-file0-halos-only.log` |
| `p03d-file0-sage16` | `$P/bin/sage16__shin-uchuu-lhalo/mimic $P/runfiles/file0-sage16.yaml`, exit 0 | 173.16 s (156.00 s user, 7.76 s sys) | **164.795 GB** | `$P/logs/p03d-file0-sage16.log` |

**The super-forest's conversion.** The totals pass took 134.70 s, from the "deriving" line to the printed bound: it was warm, because step 1 had just read the same columns. The file then converted in 191.96 s (the manifest's `wall_seconds` 191.963), which is 658,358 halos/s for one worker.

The step read 27,191,918,592 B and wrote 15,257,976,832 B on the LaCie (the `ioreg` counters). The printed bound was 86,202,994,080 B (80.28 GiB). The peak, 69,344,296,960 B, is 0.80 of file 0's own bound, 86,181,994,101 B (the rehearsal's figure, "Production projections", from the bound formula with a one-forest header). That ratio is higher than the subset's 0.45 to 0.46, because a single forest of 126,380,326 halos dominates the bound.

`$D/shin-uchuu-cut.0` is 11,121,468,700 B, the plan formula's size exactly. The manifest records SHA-256 `9d6934365ab642f4d140f6161da7cdd3678701b24ebc5e36595421459e2154a8`, and the independent `shasum` gives the same value.

**The runs on file 0.** Each binary printed this memory profile:

| | `halos-only` | `sage16` |
|---|---|---|
| Peak process RSS | 142.576 GB | **164.795 GB** |
| Output buffer capacity | 631,901,630 records (116.270 GB) | 631,901,630 records (116.270 GB) |
| Output population | 330,467,278 records | 121,907,647 records |
| Galaxy-pool high-water | 330,467,278 galaxies at 1 B | 128,809,345 galaxies at 176 B (22.670 GB) |
| Rate | 1,986,487 halos/s | 729,847 halos/s |
| Output (`du -sk`) | 7,323,848 KiB | 5,308,804 KiB |

The identity-space line reads `units=1, rows_per_unit=126380326 (largest forest), radix=1024: fits int64`.

The `sage16` peak, 164.795 GB, lies just below the plan's model of 166 to 205 GB and under the 250 GB allowance, so no stop applies. As modelled, the output buffer's seed of five times the forest's halos is most of it.

**The serial decision (Decision 6).** On the super-forest, `sage16` ran at 729,847 halos/s, faster than the rehearsal's 613,942 halos/s on the subset. So cost per halo does not grow with forest size, at least up to 126 M halos. The whole run projects to 8.56 h at the file-0 rate and 10.18 h at the subset rate. Both are under 24 h, so step 6 runs serial, as the owner decided. The peak memory of the whole run is file 0's, measured here.

**Step 4's memory, re-calibrated.** At file 0's ratio of 0.80, each worker's peak is about 0.8 of its file's bound, (2 × 70 × 1,048,576 + L) × 132 + max(1,048,576, L) × 396 B for the file's largest forest L. The four largest forests after file 0 hold 71,150,152, 55,080,340, 51,040,732 and 49,139,058 halos. Their bounds sum to about 197 GB, or about 158 GB at 0.8, if all four ever ran together. That is well inside 549.8 GB, so step 4 starts at 4 workers, as HANDOFF recommends.

### Step 4: the conversion

The command is step 3's with no `--only-files` (`$P/scripts/s04_convert.sh`, sampled every 10 s), so file 0 is checked against its manifest entry and skipped. Only `--workers` changes between attempts. The window is the default 1,048,576 rows throughout.

**Attempt 1: 4 workers** (`$P/logs/p04-convert.log`), 06:32:14Z to 07:04:35Z.
- The totals pass took 458.75 s, from the "deriving" line to the printed bound. It was partly cold.
- The bound printed 86,202,994,080 B per worker (321.13 GiB for 4 workers).
- The conversion phase started at 06:40:12.978Z. File 0 `matches its manifest entry; skipped`.
- Files 285, 275, 278 and 286 (the largest first; 99,999,963 to 99,999,990 halos each) completed in 889.5, 963.7, 983.5 and 1,024.2 s. That is a median of 102,724 halos/s per worker, about 411,000 halos/s for the four.

**Live adjustment 1** (owner's real-time authority; HANDOFF's step-4 throughput check). The first check, at 07:00:25Z (the session's check time; `p04-checks.txt` holds no timestamps) over the preceding 1,188 s (`$P/scripts/check4.py`, appended to `$P/logs/p04-checks.txt`), showed:
- the workers at a mean of 3.3 to 4.7 % CPU;
- the LaCie moving 82.1 MB/s read and 31.1 MB/s write (113.2 MB/s in all, from `ioreg`);
- `iostat` on `disk9` at a mean of 107.7 MB/s (68.5 to 335.4);
- the step's whole `iostat` series at 600 KB per transfer and 221 transfers/s.

The workers sat in state `U`, uninterruptible disk wait (`ps`). This is disk-bound, and worse than the rehearsal's 2-worker cold baseline of 137 to 138 MB/s: seek-bound interleaving of four workers' reads. Microsoft Defender averaged 17.7 % CPU over the conversion phase, comparable to the rehearsal, so it is not the cause.

Following HANDOFF's recipe (disk-bound: restart with fewer workers), the conversion was restarted with `--workers 2`. A larger `--window-rows` was ruled out because the read excess (about one window per slab per file) grows with the window: about 39 GB per file at 4,194,304 rows.

**The interrupt.** HANDOFF's recipe of SIGINT to the process group did not work for a detached step. A job launched with `nohup … &` from a non-interactive shell inherits SIGINT as ignored, so Python never installs its handler. `kill -INT` at 07:01:53Z was a no-op: the workers kept writing, as `sample` showed. At 07:04:35Z the four workers were sent SIGTERM. The parent then logged that a worker process died, marked files 269, 282, 284 and 287 `in flight when a worker process died`, wrote its manifest (5 of 361 complete: 0, 275, 278, 285 and 286) and exited 2. The step logged its footer and ledger row (1,941.37 s; peak RSS 9.765 GB, the largest process). No process remained (`pgrep`).

The four `.partial` files left behind were 0, 0, 301,669,220 and 4,862,148,624 B (files 269, 282, 284 and 287). The restart moves them aside as `.stale-<timestamp>`; they are logged in `$P/TRASH.md`.

**Attempt 2: 2 workers** (`$P/logs/p04b-convert-w2.log`), launched 07:04:42Z.

The restart's totals pass took 422.53 s, from the "deriving" line at 07:05:04Z to the bound at 07:12:06.843Z. The conversion phase began at 07:12:06.954Z. Completed files were re-hashed against the manifest and skipped. Files 287, 269, 282 and 284's `.partial` files were moved aside as `.partial.stale-20261018T071206Z` between 07:15:44Z and 07:22:53Z (the converter's lines in `$P/logs/p04b-convert-w2.log`; logged in `$P/TRASH.md` as they were noticed) and their files rebuilt.

**Check at 07:38:57Z** (2 workers, over the preceding 890 s; `$P/logs/p04-checks.txt`):
- the workers averaged 7.7 and 7.8 % CPU;
- the LaCie moved 84.9 MB/s read and 40.4 MB/s write (125.3 MB/s), and `iostat` averaged 120.4 MB/s (61.0 to 216.1);
- 11 of 361 files were complete (1,126,379,552 halos, 5.01 %);
- files 281 and 250 (99,999,843 and 99,999,869 halos), the first with no data left warm by attempt 1, took 452.1 and 531.8 s, about 188,000 to 221,000 halos/s per worker. Files 287, 269, 284 and 282 took 280.1 to 373.9 s; part of their columns was still in the page cache from attempt 1.

The aggregate is about the 4 workers' (about 400,000 to 450,000 halos/s) with half the contention. Device throughput rose from 113 to 125 MB/s. The conversion stays disk-bound at about 120 MB/s of mixed seek-bound reads and writes, so the worker count stays at 2. Four workers measurably lowered throughput, and one would leave the disk idle while it computes. The read excess is the larger share of reads for small-forest files, which is the owner's accepted cost.

**Later checks**, every one or two hours (`$P/logs/p04-checks.txt`), held steady:
- the workers at 5.6 to 8.9 % CPU;
- the LaCie at 110 to 125 MB/s;
- 311,000 to 389,000 halos/s (the change in `files complete` halos between consecutive checks over the session's check intervals; the file holds no timestamps);
- summed RSS at most 51.9 GB (50,675,456 KiB);
- free memory at 98 %.

The worker count was not changed again.

**Attempt 2's result** (`$P/logs/p04b-convert-w2.log`): exit 0 at 2026-10-19T00:00:11Z, `361 of 361 selected file(s) complete`.

| Measure | Value |
|---|---|
| Wall | 60,928.31 s (5,823.67 s user, 3,331.06 s sys) |
| Conversion phase | 60,484.56 s (16.80 h) |
| Peak RSS | 43.802 GB (the largest process; a big-forest file's worker). Summed RSS peaked at 50,675,456 KiB in the 10 s samples. |
| Halos converted in this attempt | 21,977,268,812 (all but file 0 and attempt 1's four files), 363,353 halos/s over the phase |
| Re-hash on restart | 5 complete files matched their manifest entries and were skipped |
| Device read | 5,470,249,594,880 B (`ioreg`) |
| Device write | 1,958,914,269,184 B (`ioreg`) |
| Device throughput | 121.9 MB/s combined over the step |
| LaCie use | from 3,868,348,684 to 5,751,851,160 KiB at its peak, so 2,061,965,124 KiB (2.11 TB) free at the peak |

The logical read of the converted rows is 2,900,999,483,184 B (132 B per halo). The device read exceeds it by 2.569 TB. That includes the totals pass (about 0.36 TB of two columns, partly cold) and the re-hash of 5 files (46,328,761,984 B, the manifest's `bytes` for files 0, 275, 278, 285 and 286). The rest, about 2.16 TB, is about 6.1 GB per converted file, the read excess. This is an inference from the counters, not a trace.

Per file (manifest `wall_seconds`, one worker each):

| Files | Count | Halos | Median halos/s per worker | Worker-seconds |
|---|---|---|---|---|
| 1 to 166 (1,000,000 forests each) | 166 | 7,604,588,900 | 163,823 | 47,996 |
| 167 to 360 | 194 | 14,772,679,811 | 196,074 | 76,298 |

The slowest file was 286, 1,024.2 s, in attempt 1.

**The dataset is complete** (`$P/scripts/verify_manifest.py`, `$P/logs/p05-manifest-check.txt`, run by `$P/scripts/chain.sh` at 00:00:35Z): `PASS`.
- 361 files, 244,953,607 forests and 22,503,649,037 halos, the dataset's totals.
- 1,981,300,932,572 B, the plan formula's size exactly.
- Every file at its recorded size and no `.partial` left.
- `du -sk $D` is 1,940,488,940 KiB.
- The manifest records three invocations, all at `944364d4`: step 3's 1 worker, attempt 1's 4 and attempt 2's 2. Those wrote 1, 4 and 356 of the files.
- The four `.stale-*` partials are kept in `$D` and excluded from the transfer.

Each file's SHA-256 was computed while writing and verified on a whole re-read before its rename (the converter's write-verify). These form the transfer's list, `$P/transfers/vertical-dataset.sha256sum`, 363 lines with `file_plan.json` and `manifest.json` hashed at that point.

### Step 5: the dataset is the run's

No separate check of the dataset precedes the run (the plan's step 5). The proof is three things:
- step 4's per-file write-verify and the manifest check above;
- the reader's own startup scan in step 6;
- step 8's identity.

Step 6's log (`$P/logs/p06-sage16-np4.log`, every task) records the scan as `Processing 361 input files (first_file=0, last_file=360)` and `Created-record identity space (vertical, reader 'lhalo_binary'): units=244953607, rows_per_unit=126380326 (largest forest)`. That is the forest total, 244,953,607, and the largest unit, 126,380,326 halos.

### Step 6: the vertical `sage16` run

**Ranks** (`$P/scripts/rank_memory.py` on the converted files' headers, `$P/logs/p06-rank-memory.txt`). The largest forests are:

| File | Halos | Rank under `-np 4` |
|---|---|---|
| 0 | 126,380,326 | 0 |
| 167 | 71,150,152 | 3 |
| 168 | 55,080,340 | 0 |
| 169 | 51,040,732 | 1 |
| 170 | 49,139,058 | 2 |
| 171 | 39,190,542 | 3 |

Under `-np 4` at step 3's 1,304 B per halo of a rank's largest forest (164,794,810,368 B / 126,380,326), the worst concurrent peak on an equal-rate timeline is 225.4 GB: ranks 1 to 3 on files 169, 170 and 167. The strict bound, every rank at its own largest at once, is 295.2 GB, or 388.2 GB with file 0 in place of rank 0's largest otherwise. Both are within the 399.8 GB limit (549.8 GB less 150 GB). Choice: `-np 4`.

**Launch.** The dataset's transfer started at 00:01:07Z and step 6 at 00:01:10Z (2026-10-19, 11:01 AEDT). Step 6 is `$P/scripts/s06_sage16.sh`: `mpirun -np 4 -x MallocLargeCache=0 $P/scripts/rank_time.sh $P/logs/p06-sage16-np4 $P/bin/sage16__shin-uchuu-lhalo__mpi/mimic $P/runfiles/sage16-vertical.yaml`, from the repository root, logged in `$P/logs/p06-sage16-np4.log` with each rank's `/usr/bin/time -l` in `$P/logs/p06-sage16-np4-rank<k>.time`.

**First check, 00:17Z** (over the preceding 900 s of `$P/logs/p06-sage16-np4.samples` and `.procs`, and NT's `du`):

| Signal | Value |
|---|---|
| LaCie | 115.6 MB/s read and 32.4 MB/s write |
| The four ranks | 0 to 2 % CPU at that instant, in disk wait; over the preceding 900 s their means were about 57, 55, 36 and 6 %. Rank 0 had peaked at 160,934,720 KiB (164.797 GB) in the samples while on file 0. Summed RSS 25.6 to 26.2 GB. |
| The dataset's `rsync` | 21,058,564 KiB on NT after about 16 min. That is NT's `du -sk`, Lustre's compressed allocation, about 1.54 times smaller than the bytes (step 10); so about 34 MB/s. |

With five concurrent readers, the LaCie is seek-bound at 148 MB/s combined, a little above step 4's 110 to 125 MB/s, so step 6 and the transfer share it rather than adding throughput. Both stay running. Keeping the disk saturated beats the transfer alone, which the network caps at about 105 MB/s. The time saved by the overlap is smaller than projected.

**Result** (`$P/logs/p06-sage16-np4.log`, `$P/logs/p06-sage16-np4-rank<k>.time`): exit 0 at 2026-10-19T05:12:16Z (16:12 AEDT).

| Measure | Value |
|---|---|
| Wall | 18,666.18 s (5.18 h) |
| User / sys | 32,835.96 s / 4,628.31 s (mpirun's outer `/usr/bin/time`); the four ranks' own sum is 32,835.77 s / 4,628.11 s |
| Rate | 1,205,584 halos/s for the 22,503,649,037 halos |
| Work split | Each rank logged `Processing 361 input files (first_file=0, last_file=360) → 361 output files across 4 tasks` and `Mimic completed successfully` |
| Identity-space line | `units=244953607, rows_per_unit=126380326 (largest forest), radix=1024: does not fit int64; this run cannot create records`. This is the plan's stated limit: no `hod` or `sham` claim. `sage16` creates no records. |
| Summed RSS (10 s samples) | at most 184,159,328 KiB (188.6 GB), against the 225.4 GB timeline model; free memory never below 97 % |
| Output | `du -sk $O/sage16-vertical` 542,827,068 KiB |
| LaCie use | 6,294,184,684 KiB at its peak during the step; 6,294,184,244 KiB at its end |

Each rank's `/usr/bin/time -l` and rank 0's memory profile:

| Rank | Wall | User | Sys | Peak RSS |
|---|---|---|---|---|
| 0 | 18,665.74 s | 8,756.04 s | 1,160.01 s | **164.806 GB** (file 0; output buffer 631,901,630 records, 116.270 GB; population 121,907,647; galaxy-pool high-water 128,809,345, 22.670 GB) |
| 1 | 18,665.74 s | 8,346.28 s | 1,155.27 s | 66.652 GB (file 169's 51,040,732 halos) |
| 2 | 18,665.74 s | 8,001.59 s | 1,159.72 s | 64.159 GB (file 170's 49,139,058) |
| 3 | 18,665.74 s | 7,731.86 s | 1,153.11 s | 93.000 GB (file 167's 71,150,152) |

Rank 0's peak equals step 3's serial file-0 run's to within 0.012 GB. Over the step, the LaCie read 2,265,331,220,480 B and wrote 556,554,326,016 B (`ioreg`). The reads include the dataset transfer's.

The ranks ran disk-bound at 47.6 to 53.1 % CPU each over the run, beside the transfer ((user + sys) / wall from each rank's `.time`). The 5.18 h compares with the 3 to 3.5 h projected without the transfer, and 8.6 to 10.2 h serial.

### Step 7: the package goes to production

- **The symlink.** `simulations/shin-uchuu-lhalo/snapshots` (gitignored) now points at `$D`, which holds the 361 files `shin-uchuu-cut.0` to `shin-uchuu-cut.360`.
- **The edit.** `simulations/shin-uchuu-lhalo/simulation_info.yaml`'s `last_file` changed from 0 to 360, the plan's last file. This was done after step 6 had finished, so the run's metadata could not see the edit.
- **Tests.** The package's tiers ran on the edited working tree (`$P/logs/p07-package-tests.log`; `TMPDIR` on Internal; both `MIMIC_SHIN_UCHUU_CUT_SUBSET_*` unset), with `make MODEL=sage16 SIMULATION=shin-uchuu-lhalo tests-unit tests-integration tests-scientific`:
  - unit `passed=304 failed=0 skipped=0 n/a=28`;
  - integration `passed=123 failed=0 skipped=0 n/a=15`;
  - scientific `passed=47 failed=0 skipped=1 warned=1`. The skip is the gate's designed `test_subset_not_named`, and the warning is `test_zero_values`.
  - No test of this package reads `snapshots`: its tests run on the committed fixture. So the plan's "real-data half of any test that reads `snapshots`" has no instance here.
- **Default build.** Restored afterwards.
- **Pre-commit gate.** `./scripts/beautify.sh` exited 0 (the session's `rc=0`; its output is `$P/logs/p07-beautify.log`), and `git status --porcelain` afterwards showed only the two expected paths. The lint skill's `check --base HEAD` gave `no new findings`, `verdict: pass` (session output; not logged).
- **The commit** (owner pre-approved): `5dd6fcfe158cc7d5f3847d3cbaeb107370074666`, "Point shin-uchuu-lhalo at the production dataset's last file, 360". It changes that one line and nothing else. It is on `feature/shin-uchuu-cut-vertical` and not pushed.

### Step 8 and the transfers: live adjustment 2

**What was seen.** By 07:46Z (18:46 AEDT) step 8 had run 2.5 h with no verdict:
- **The comparator** (`$P/logs/p08-compare.procs`) sat in state `U` at about 1 % CPU with 5 to 7 GB of RSS. It had the vertical output's `model_163.hdf5` open (`lsof`).
- **Why it reads so much.** Reading any one field of Mimic's HDF5 output reads the whole galaxy records, so every comparator pass reads the full 542.8 GB vertical output from the LaCie.
- **The dataset's `rsync`** had slowed while sharing the disk with the comparator. NT's `du -sk` grew from 357,491,312 to 582,553,302 KiB in the preceding 3.45 h. That is compressed allocation, so the bytes sent were about 1.54 times more, about 28 MB/s.
- **The LaCie** read about 96 MB/s in all over the last 10 min (`$P/logs/p08-compare.samples`). Two interleaved sequential readers thrashed it, as in step 4.

**What changed.** At 07:47:28Z the dataset's `rsync` (pid 60800) was sent SIGTERM. Its step ended with rsync's exit 20 (`$P/LEDGER.md`, `p10a-rsync-dataset-to-nt`, 27,981.34 s), the chain logged `DATASET-RSYNC-FAILED` and `CHAIN-DONE`, and step 8 kept the disk.

`$P/scripts/s10_after8.sh` (log `$P/logs/after8.out`; its first launch at 07:48Z failed at once with `Permission denied` because the script was not executable, `$P/logs/after8-failed-launch.out`, found at the 08:02Z check and relaunched at 08:03Z after `chmod +x`) then runs one job at a time, stopping at the first failure:
1. wait for step 8's exit 0;
2. the owner's plots (`$P/scripts/s09_plots.sh`);
3. the dataset's `rsync` again (`p10a2`; `rsync -a` skips the files already complete on NT);
4. the dataset's NT check;
5. the output's `rsync` and its NT check.

The output's SHA-256 list was already made beside step 8: `$P/logs/p10c-sha256-output.log`, 5,538.02 s, exit 0; `$P/transfers/vertical-output.sha256sum`.

With the `rsync` stopped, the comparator ran at 100 % CPU and 155,755,952 KiB (159.5 GB) of RSS, and the LaCie read 139.2 MB/s (08:02Z check, session output). The stopped `rsync` left no temporary file on NT: 152 files, 579,208,696 KiB by NT's `du -sk`, compressed allocation.

### Step 8: the comparison

`$P/scripts/s08_compare.sh` ran `scripts/compare_cross_format_identity.py --left-label reference --right-label vertical /Volumes/Internal/results/mimic/sage16-shin-uchuu-v3-cut/model $O/model`, default mode, with its report redirected to `$P/reports/compare-reference-vs-vertical.txt` (14 lines, md5 `3b746bbc240bc810d77170fbd1aba5d6`). The step log is `$P/logs/p08-compare.log`.

- **Exit 0** at 2026-10-19T09:24:00Z, in 15,067.54 s (2,050.17 s user, 563.56 s sys).
- **Peak RSS 291.183 GB** (the v3 record measured 297.353 GB for this mode at this scale).
- The LaCie read 1,747,708,272,640 B over the step. That includes the dataset's `rsync` until 07:47:28Z and the output's SHA-256 pass until 06:45:10Z.
- The report reads the reference as 10 partition files and the vertical run as 361.

| Snapshot | Galaxies | Fields |
|---|---|---|
| 0 | 1,854,727 | all 42 byte-identical |
| 1 | 3,219,989 | all 42 byte-identical |
| 2 | 5,147,402 | all 42 byte-identical |
| 5 | 16,421,826 | all 42 byte-identical |
| 10 | 66,306,470 | all 42 byte-identical |
| 20 | 281,932,492 | all 42 byte-identical |
| 40 | 490,934,902 | all 42 byte-identical |
| 43 | 473,283,408 | all 42 byte-identical |
| 52 | 406,693,129 | all 42 byte-identical |
| **69** | **313,753,033** | all 42 byte-identical |

The report's verdict:

> `PASSED: 2059547378 galaxies over 10 output snapshot(s) are bitwise identical in all 42 field(s), with identical UniqueGalaxyID sets and no duplicates (tree rows by UniqueGalaxyID, created rows not compared; 0 created row(s) compared)`

The z = 0 count, 313,753,033, is the reference's (the plan's "The reference"). **The vertical `sage16` run on the converted dataset is identical, galaxy for galaxy and byte for byte, to the cut horizontal run at every one of the ten output snapshots.**

### Step 9

Skipped by the owner's decision (see "Owner decisions before step 1"); step 8's identity implies the z = 0 table of `simulations/shin-uchuu/README.md`. The owner instead asked for `mimic-plot`'s snapshot and evolution figures of the vertical run (`$P/scripts/s09_plots.sh`, into `$O/sage16-vertical-plots`, outside the run's output directory), to compare with the cut horizontal run's figures. They run after step 8, because the horizontal run's snapshot plots peaked at 223 GB.

### The owner's figures

`$P/scripts/s09_plots.sh` runs the owner's `run-plots.sh` for the cut horizontal run, from the procedure's run file, `--verbose` (`$P/logs/after8.out`, `PLOTS-DONE` at 10:34:09Z):

| Step | Command | Result | Wall | Peak RSS |
|---|---|---|---|---|
| `p09a-mimic-plot-snapshot` | `mimic-plot.py --snapshot-plots` | `Generated 18 snapshot plots`, 0 skipped | 1,798.19 s | 210.317 GB |
| `p09b-mimic-plot-evolution` | `mimic-plot.py --evolution-plots` | `Generated 4 evolution plots`, 0 skipped | 2,401.35 s | 53.689 GB |

The figures are in `$O/sage16-vertical-plots/`. The owner's cut horizontal run took 1,745.98 s at 223.3 GB for the snapshot figures and 195.50 s at 57.4 GB for the evolution figures (`/usr/bin/time -l` in its `snapshot-plots.log` and `evolution-plots.log`, kept with those figures); the evolution figures here read the vertical output from the LaCie.

**Compared with the cut horizontal run's figures** (the owner's `sage16-shin-uchuu-v3-cut-plots`; `$P/logs/p09c-plot-comparison.txt`, by md5, then pixel by pixel):
- The same 22 files.
- 6 byte-identical: `HaloMassFunction`, `HaloMassFunction_Evolution`, `SFR_Density_Evolution`, `Stellar_Mass_Density_Evolution`, `SpatialDistribution`, `SpinDistribution`.
- 16 differ. Their difference boxes span the axes and the plotting area.

The cause is the plot profile, not the data. `models/sage16/plots/profiles/` holds a model-and-simulation profile for `shin-uchuu` and for `shin-uchuu-ascii` but not for `shin-uchuu-lhalo`, so the vertical run's figures took the default axis ranges. For example, `StellarMassFunction` spans log M* 4 to 12.5 for the horizontal run and 8 to 12.5 here. Where the ranges overlap, the curves agree by eye.

The `shin-uchuu` profile sets axes for exactly the 16 figures that differ, and for none of the 6 identical ones except `halo_mass_function`, whose values equal the simulation profile's. Step 8's byte identity of every field already establishes that the plotted data are the same.

A redraw with that profile (`$P/scripts/s11_replot.sh`: the procedure's run file plus `plotting.profile: models/sage16/plots/profiles/shin-uchuu_plot_profile.yaml`, into `$O/sage16-vertical-plots-shin-uchuu-profile`) is queued after the transfers, so it does not share the LaCie with them. Its comparison is below.

**The redraw** (`$P/logs/replot.out`). Before it ran, the merged profile stacks were built both ways with `mimic-plot.py`'s own `configure_plot_profile` (re-run and logged in `$P/logs/p11d-profile-stack-check.txt`):
- **The owner's cut horizontal run** loaded `plot/mimic-plot/profiles/default.yaml`, `models/sage16/plots/profiles/default.yaml`, `simulations/shin-uchuu/plot_profile.yaml` and `models/sage16/plots/profiles/shin-uchuu_plot_profile.yaml`.
- **The redraw** loaded the same with `simulations/shin-uchuu-lhalo/plot_profile.yaml` in third place, then the configured `shin-uchuu_plot_profile.yaml`.

The merged profiles are equal. The first vertical stack lacked the last layer, and with it the axes of `baryon_fraction`, `baryonic_mass_function`, `baryonic_tully_fisher`, `black_hole_bulge_relation`, `bulge_mass_fraction`, `cold_gas_function`, `gas_fraction`, `gas_mass_function`, `halo_occupation`, `mass_reservoir_scatter`, `metallicity`, `quiescent_fraction`, `specific_sfr`, `stellar_mass_function` and `velocity_distribution`.

| Step | Result | Wall | Peak RSS |
|---|---|---|---|
| `p11a-replot-snapshot` | 18 figures | 2,150.74 s | 221.995 GB |
| `p11b-replot-evolution` | 4 figures | 3,593.92 s | 53.689 GB |

Both exited 0, into `$O/sage16-vertical-plots-shin-uchuu-profile`. `$P/scripts/compare_pngs.py` against the owner's `sage16-shin-uchuu-v3-cut-plots` (`$P/logs/p11c-plot-comparison.txt`, md5 `9c80f7e96d7a0c5ba4b00f4dcd58a5ef`) reports the same 22 files and `22 identical (bytes or pixels), 0 differ`. **Every one of the 22 figures is byte-identical (md5) to the cut horizontal run's.** A copy is on the owner's Desktop as `sage16-shin-uchuu-v3-cut-vertical-plots`.

### Step 10: settle

`$P/scripts/s10_after8.sh` (`$P/logs/after8.out`, `AFTER8-DONE` at 15:43:52Z) ran one job at a time:

| Step | What | Result | Wall |
|---|---|---|---|
| `p10a2-rsync-dataset-to-nt` | `rsync -a --exclude=.DS_Store --exclude='*.stale-*' --exclude='*.partial' $D/ nt.swin.edu.au:/fred/oz214/dcroton/shin-uchuu/vertical-trees-lhalo-cut/`, resuming past the 152 files the stopped attempt had sent | exit 0 | 10,388.54 s |
| `p10b-nt-sha256check-dataset` | `nice sha256sum -c` on the NT login node against `$P/transfers/vertical-dataset.sha256sum` (363 lines: the conversion manifest's 361 SHA-256s, plus `file_plan.json` and `manifest.json` hashed after step 4), output in NT's `verify/vertical-dataset-sha256check.out` | `sha256sum rc=0`, `lines 363`, `ok 363` | 2,321.15 s |
| `p10d-rsync-output-to-nt` | `rsync -a --exclude=.DS_Store $O/sage16-vertical/ nt.swin.edu.au:/fred/oz214/dcroton/shin-uchuu/sage16-shin-uchuu-lhalo-cut/` | exit 0 | 5,312.02 s |
| `p10e-nt-sha256check-output` | against `$P/transfers/vertical-output.sha256sum` (368 lines, made beside step 8) | `sha256sum rc=0`, `lines 368`, `ok 368` | 557.18 s |

**Sizes and integrity.** During its resumed run the LaCie read 1,080,200,425,472 B (`ioreg`, `p10a2`, the only reader), 104.0 MB/s over 10,388.54 s, the network's rate. So about 0.90 TB had reached NT before the stop. The output's transfer read 554,123,300,864 B in 5,312.02 s, 104.3 MB/s. NT's `du -sk` (Lustre, compressed: 1,260,268,495 KiB for the dataset's 1,981,301,179,042 B) is not a byte count; the mid-run NT figures above are corrected for it. NT holds 363 dataset files and no `.stale-*`.

| Copy | Local (`stat`) | NT (`du -sb --apparent-size`) |
|---|---|---|
| Dataset | 1,981,301,125,282 B, excluding the stale partials | 1,981,301,179,042 B |
| Output | 554,103,301,177 B in 368 files | 554,103,364,665 B in 368 files |

The NT figures are larger by the directories' own sizes. No output file is newer than the SHA-256 list (`find -newer`). The local `du -sk` of the output moved from 542,827,068 to 541,117,228 KiB with no file changed: APFS block accounting.

**Dispositions.** Nothing local was deleted. `$P/TRASH.md` lists only the four stale partials, kept.

| Directory | Where | Disposition |
|---|---|---|
| `$D`, the vertical dataset (361 files, `file_plan.json`, `manifest.json`, four stale partials) | LaCie | kept; NT copy verified (without the partials) |
| `$O/sage16-vertical`, the production run | LaCie | kept; NT copy verified |
| `$O/file0-halos-only`, `$O/file0-sage16` (step 3's runs, 7,319,844 and 5,303,056 KiB) | LaCie | kept; no NT copy |
| `$O/sage16-vertical-plots`, `$O/sage16-vertical-plots-shin-uchuu-profile` (the figures) | LaCie | kept; the matched set also on the owner's Desktop |
| `$P`, the procedure's logs, scripts, binaries, run files, reports and transfer lists (121,828 KiB) | LaCie | kept; no NT copy |
| `$V`, the rehearsal scratch (252,263,396 KiB, 258.3 GB) | LaCie | kept, the owner's call later |
| The cut horizontal dataset | LaCie | read only, untouched |
| The reference | Internal | read only, untouched |

Free space at the end was 1,520,475,540 KiB on the LaCie and 441,474,356 KiB on Internal (`$P/logs/p11b-replot-evolution.log`, df after).

### Outcome

The production procedure is complete. The cut Shin-Uchuu merger trees in L-Halo binary form (`$D`, 361 files, 244,953,607 forests, 22,503,649,037 halos) give, under `sage16` and the vertical driver, a run bitwise identical in every field to the cut horizontal run at all ten output snapshots: 2,059,547,378 galaxies, 313,753,033 at z = 0. Its `mimic-plot` figures are byte-identical to the horizontal run's.
