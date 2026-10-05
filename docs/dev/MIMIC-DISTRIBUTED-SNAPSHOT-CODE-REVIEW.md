# Distributed Snapshot Operations — Post-Run Code Review

**Purpose:** An independent, evidence-based review of the work delivered by the eleven-slice run of [`MIMIC-DISTRIBUTED-SNAPSHOT-IMPLEMENTATION-PLAN.md`](MIMIC-DISTRIBUTED-SNAPSHOT-IMPLEMENTATION-PLAN.md) (commits `fe0b9cad..b2c135b8` on `feature/distributed-snapshot`, 21 commits, 88 files, +7,210/−449 lines), judged against the plan's goals, [VISION.md](../VISION.md) and [STYLE-GUIDE.md](../STYLE-GUIDE.md). It covers correctness and quality first, a behaviour-preserving simplification pass second, and it assesses every decision, defect and carried item the project manager recorded during the run. The end result is what is reviewed; where a pre-implementation plan choice and the delivered code differ, the delivered code is judged on its merits.

**Reviewed:** 2026-10-06, at `b2c135b8`. Reviewer: Claude Fable 5.1, with three delegated sub-reviews (test and gate surface; generators and documentation sweep; build, lint, code-health and test battery), each verified against the source before inclusion.

---

## Executive Summary

**Verdict: PASS WITH RISKS.** No P0 or P1 finding. The plan's two goals are met and were independently re-verified in this review: per-rank memory is bounded by the rank's forests rather than the widest slab (per-rank peak RSS 23–30% of serial at `-np 4` on micro-Uchuu, per the acceptance record), and the same run file produces bitwise-identical galaxies at every tested rank count (`make tests-distributed` rerun here: 102 checks green, exit 0, including `hod`'s 10 created rows and the version 2 refusal).

**Validation rerun in this review, all exit 0:** clean default build and `USE-MPI=yes` build with zero compiler warnings; `check-generated` for the default pair and three non-default pairs; `validate-modules`, `check-format`, `check-docs`; differential lint against `fe0b9cad` (no new findings); unit 577 PASS / 0 FAIL / 15 configuration SKIP; integration 245 / 0 / 12; scientific 25 / 0 / 0; `tests-horizontal-v3` PASS; `tests-snapshot-global-identity` 15 PASS (one housekeeping WARN about cached worktrees); `tests-distributed` 102 checks. The tree stayed clean and on-branch throughout; the detached-HEAD hazard the PM recorded did not recur.

**Core code quality is high.** The partition module, the sample-sort rank, the reader range reads, the link rebase, the identity `row_offset`, the task-aware output layout and the two module ports are correct on every path I traced, including the empty-rank, all-empty, single-bucket, error-agreement and `n == 0` cases. Failure handling is uniformly fail-closed and agreed across tasks. Documentation and skills were swept thoroughly; the remaining stale text is small and listed below.

**The risks are in the gate and in one operational seam, not in the runtime:**

1. **P2 — `--skip` resume after an aborted multi-rank vertical run can accept a truncated partition.** D10's `MPI_Abort` kills sibling ranks mid-write and `--skip` checks only that the file exists. The SIGXCPU stop now takes this path, so the time-limit-and-resubmit workflow is degraded for MPI runs. Documented as a caveat; needs a completion marker or rename-on-finalize.
2. **P2 — The CI gate can time out before it prints its log.** Per-launch timeouts of 300 s across 46 launches exceed the job's 45-minute budget in a systematic MPI hang, and the `cat` of the log is on the failure branch of the same step, so a hang leaves no diagnostics.
3. **P2 — Two false-green paths in the distributed gate.** The `halos-only` and `sage16` legs pass when both runs are equally empty, and above `-np 1` the gate never checks that every task's partition file exists on disk.
4. **P2 — The multi-block range-read path has no committed test.** No committed fixture exceeds the 8,192-row scan block, so CI exercises only single-block range reads; the real-data stage (155,340 rows per rank) is the only evidence and is manual.

Everything else is P3: duplicated helpers (`distributed()` twice, the HDF5 partition name formatted four times), two exported test hooks without a header prototype, the split `stderr` writes of the task prefix, and a short list of stale comments and skill lines.

**Recommended next steps, in order:** (1) a small gate-hardening slice closing items 2 and 3 above (an afternoon's work, no runtime change); (2) a `--skip` completion-marker slice for the vertical driver; (3) a multi-block range-read unit test that writes a synthetic 20,000-row version 3 dataset; (4) the simplification pass in §6 and the stale-text sweep in §7, which can share one commit.

---

## 1. Authorization Status

- Every elevated slice (2–11) carried a drift audit that passed before its code review; slices 1 and 3 were standard-risk and PM-reviewed. Three plan defects were resolved on the record, one through an exact-file surface grant (`src/core/vertical_driver.c`, the positional initialiser at `:58`). I did not re-audit authorization; I read the final diff for quality.
- One PM resolution widened behaviour beyond a criterion's literal text: `myexit()` removing the aborting rank's in-flight outputs before `MPI_Abort` (`src/core/main.c:116-121`). I judge it correct and necessary (§7, decision 2).

## 2. Plan Goals Against the Delivered Code

| Goal (plan Purpose) | Status | Evidence |
|---|---|---|
| Each rank holds only its own forests' rows of every slab | **Met** | `horizontal_task_rows()` and the range load at `src/core/horizontal_driver.c:1599-1610`; `load_slab` reads exactly `[row_lo, row_hi)` through hyperslabs for every column including the six aux columns (`src/io/horizontal/read_horizontal_hdf5.c:1620-1665`, `:2764-2786`) |
| Snapshot-global modules reach whole-population quantities through core collectives | **Met** | Six collectives in `src/core/snapshot_collectives.h`; `sham_rank_match` and `hod_populate` ported and declared `collective`; startup refuses `serial_only` under `NTask > 1` (`src/core/module_registry.c:690-699`) |
| Per-process memory a function of rank count and largest forest, not largest slab | **Met, with the stated floor** | Acceptance record: 0.53–0.73 GB per rank against 2.24–2.45 GB serial; super-forest floor 1.60% for micro-Uchuu, 61.86% for Shin-Uchuu (not distributable in version 2 anyway) |
| Same run file, same galaxies, whatever the rank count | **Met** | Fixture gate at `-np 1, 2, 3, 4, 8` for four models (re-run here); real data at `-np 3` and `-np 4` with `--compare-created`; serial output byte-identical to the pre-feature references |
| No accepted commit runs an unported snapshot module across ranks | **Met** | Slice 6 (`b4b48c6f`, `1c08f6a2`) precedes Slice 7 (`34fead7a`) in history |

## 3. Findings

Severity follows the house review standard: P0/P1 require a stated reachability path; nothing here reached that bar.

### P2

1. **[P2] `src/core/main.c:115-121`, `src/core/vertical_driver.c:361-369` — `--skip` can resume over a partition truncated by `MPI_Abort`.** Under `NTask > 1` any non-zero `myexit()` removes only the aborting rank's in-flight files and then aborts the job; sibling ranks are killed wherever they are, including mid-write. `claim_and_process_partition()` honours `--skip` by file existence alone, so a resume skips the truncated partition and the master then links it (or fails opening it, depending on how much HDF5 metadata was flushed). Reachability: the SIGXCPU stop is a `FATAL_ERROR` (`vertical_driver.c:286-289`), so a time-limited MPI run that previously met in `MPI_Finalize` now aborts its siblings; the resubmit-with-`--skip` workflow is exactly the one that then reads the partial file. The USER-GUIDE caveats at `docs/USER-GUIDE.md:398` and `:518` are correct but place the burden on the operator. **Fix:** write partitions to `<name>.partial` and rename on clean close, or write a sidecar completion marker, and make `--skip` require the final name. Consider letting the XCPU path exit 0 on every rank so the job stops cleanly rather than aborting.

2. **[P2] `tests/manual/test_distributed_identity.py:120-121`, `.github/workflows/ci.yml:98,145` — the gate's timeout budget exceeds the job's, and the log dump is unreachable on a hang.** `RUN_TIMEOUT = 300` applies to 26 launches and 20 comparator runs; a collective deadlock at one rank count repeats in every model, so 16 × 300 s = 80 min against `timeout-minutes: 45`, which also covers the earlier battery steps. The `|| { cat build/distributed_tests.log; exit 1; }` is in the same step, so GitHub kills the job before it runs. **Fix:** `RUN_TIMEOUT` of about 60 s (fixture runs take seconds), stop a leg's remaining counts after its first timeout, and print or upload the log in a separate `if: always()` step (optionally under `::group::`).

3. **[P2] `tests/manual/test_distributed_identity.py:268,339`, `scripts/compare_cross_format_identity.py:404-406,573` — the `halos-only` and `sage16` legs pass on equally empty output.** `serial_totals` is compared for equality only, and the comparator prints `both runs are empty` / `PASSED: 0 galaxies` once any `Snap###/Galaxies` dataset exists. Only `hod` (created rows > 0) and `sham` (`assigned >= 1`) have a non-vacuity guard. **Fix:** require a positive serial total on every leg (the fixture holds 57 halos at the requested snapshots, so `halos-only` can pin the exact count) and a positive `PASSED: N`.

4. **[P2] `tests/manual/test_distributed_identity.py:331-350` — above `-np 1` the gate never checks that every task's partition file exists.** `check_task_layout()` reads the master's `File###_task###` groups and `TotHalosPerSnap`, not the directory. At `-np 4` and `-np 8` the idle tasks' empty partitions could be missing with dangling external links and the identity check would still pass. D7's "a rank holding no galaxies still writes its (empty) partition" is therefore asserted by the acceptance record but not by the gate. **Fix:** glob `{base}_{snap:03d}_task{t:03d}.hdf5` for every `(snap, t)` and resolve each master link (`h5py` follows external links on access).

5. **[P2] test coverage — no committed test exercises a multi-block range read.** `HORIZONTAL_HDF5_SCAN_BLOCK` is 8,192 rows and the largest committed version 3 snapshot holds 17; `horizontal_h5_fill_member()`'s `row_lo + offset` arithmetic (`read_horizontal_hdf5.c:1720-1725`) is proven only by the PM's scratch harness and the manual real-data stage. A regression in the block offset would pass CI. **Fix:** a unit test under `tests/unit/test_horizontal_v3_reader.c` that writes a synthetic version 3 snapshot of about 20,000 rows with h5py-equivalent C calls (or a committed generated fixture of that size, about 2 MB) and compares a mid-block range read against the whole read.

### P3

6. **`src/core/horizontal_driver.c:649`, `src/core/snapshot_collectives.c:462`, `src/io/output/hdf5.c:142` — `distributed()` is written three times.** The driver and the collectives each define a static `effective_task_count() > 1` under `#ifdef MPI`; the HDF5 writer re-derives the same predicate inline. One `run_is_distributed()` in `src/core/task_layout.h` would make the single definition the PM already recommended.
7. **`src/io/output/util.c:36-44`, `src/io/output/master_hdf5.c:125-131`, `src/core/horizontal_driver.c:170-174` — the HDF5 partition and master file names are formatted in four places.** The master writer rebuilds the relative file name and the group name with `sprintf`, and the driver formats the master path itself with a comment explaining why it does not share a helper. Two helpers in `output/util.c` (`output_partition_basename()`, `output_master_path_hdf5()`) would hold the naming contract once; `tests/unit/test_master_hdf5_partitions.c` already pins the names.
8. **`src/core/horizontal_driver.c:673,754` — two non-static test hooks have no header prototype.** `horizontal_partition_resident_bytes()` and `horizontal_rebase_slab_links()` are hand-declared in `tests/unit/test_horizontal_distribution.c:48-50`, so a signature change compiles on both sides without complaint. Declare them in `src/include/proto.h` beside `horizontal_driver_remove_incomplete_outputs()`.
9. **`src/util/error.c:268` — the task prefix is a separate `fprintf` on an unbuffered stream.** On `stderr` the prefix and the message body can interleave with another rank's line. Format each line into one buffer and write it once; this also fixes the cosmetic `task N: task N of M` progress fallback.
10. **`src/io/horizontal/read_horizontal_hdf5.c:2027,2040` — link-validation diagnostics print slab-local `bad_index`.** Under distribution the row named is `row_offset + i`; the message should say so or add the offset. Matters only on a corrupt dataset.
11. **`src/core/horizontal_driver.c:848-880` — rank 0 scans every `ForestIndex` column twice at startup** (once in `open_run` with `validate_columns = 1`, once for the partition), plus the widest slab a third time, while every other rank waits at the broadcast. Fine for micro-Uchuu (22.6 M rows); for mini-Uchuu-scale datasets this is minutes of single-rank I/O. D4 deliberately keeps the reader partition-agnostic, so leave it, but record it as the first thing to merge if startup time matters.
12. **`src/core/horizontal_partition.c:30` — `filled + weights[f]` could overflow when the weight sum exceeds `INT64_MAX / 2`.** Unreachable for halo counts; a `weights[f] > capacity - filled` comparison would close it for free.
13. **`scripts/validate_modules.py:741-746`, `scripts/generate_module_registry.py:259-262`, `tests/integration/test_snapshot_module_schema.py:578-580` — the utility-module rejection is narrower than its comments say.** A utility module that lists `process_snapshot` and the key passes both tools; the key is then silently dropped because utilities never reach the registry. Either reject the key whenever `is_utility` is true or reword the three comments.
14. **`tests/manual/test_distributed_identity.py:231,233,413-421` — the control test links `-lyaml` through a `pkg-config` fallback it does not need.** None of its sources include `yaml.h`; without `pkg-config` a Homebrew machine fails to link for nothing (spurious red, not false green). Remove the flags.
15. **`tests/manual/test_distributed_identity.py:243` — the control test's expected case count is the number of `TEST_RUN(` substrings in its source.** Use a constant or parse the summary line.
16. **`tests/unit/test_master_hdf5_partitions.c:658,688-689` — stale comment ("the driver itself still refuses NTask > 1") and `NTask` not reset on an early assertion return.** Red, not green, but a cascade.
17. **`scripts/compare_cross_format_identity.py:557` — the created-row count is `size − count(id > 0)`, so it includes zero-id rows** (which already fail). Count `ids < 0`. The comparator test lacks a zero-id case under `--compare-created` (`tests/scientific/test_compare_cross_format_identity.py:283`).
18. **`Makefile:481-528` — `make help` does not list `tests-distributed`.**
19. **Model code: `models/hod/modules/hod_populate/hod_populate.c:818-899` has cyclomatic complexity 28 (+14).** Correct, and the collective ordering comment above it is exactly what a reader needs, but the `failed` / `collective_failed` / `local_count` interplay would read better as a small struct of outcomes or as two functions (local audit, then the reduce-and-report tail). No behaviour change needed.

## 4. Correctness Notes (what was checked and found sound)

- **Sample-sort rank** (`snapshot_collectives.c:280-430`): splitters affect only load balance; bucket order plus `MPI_Exscan` offsets give one total order of unique keys, so the result depends only on the key multiset. Empty ranks, all-empty (`total_samples == 0`), single bucket, `count` or bucket beyond `INT_MAX`, local NaN, local and in-bucket duplicate ids: every failure is agreed through `MPI_Allreduce` before any later collective, and scratch is freed on every path through the single `cleanup:` label. `mymalloc_cat(0)` returns an 8-byte block (`src/util/memory.c:181-182`), so the zero-count allocations are safe.
- **Reductions**: `agree_reduction_arguments()` folds validity and both extremes of `n` into one `MPI_MAX`, so every task takes the same branch; `n == 0` skips the `MPI_IN_PLACE` call identically everywhere.
- **Partition** (`horizontal_partition.c`): exact minimum makespan by binary search on the greedy capacity; zero-weight forests never force a cut; idle trailing ranges; `lower_bound` row cuts across block boundaries; the brute-force oracle in `tests/unit/test_horizontal_partition.c` enumerates every contiguous partition for weights of length ≤ 8.
- **Rebase** (`horizontal_driver.c:754-846`): FoF links rebased by the slab's own `row_offset`, progenitor and descendant links by `row_cuts[t][task]`, every value range-checked before the unguarded generated setter writes it; negative links untouched; the empty-slab case skips the target-column check.
- **Identity**: `rows_per_unit` stays the global largest slab (`horizontal_driver.c:606-627`); the creation guard is written as `HaloNr >= rows_per_unit − row_offset` so it cannot overflow (`module_registry.c:1384-1388`); `row_offset` is appended to `struct RecordIdentitySpace` and zero for every other space.
- **Output**: each task writes only partitions whose task matches (`horizontal_driver.c:2150-2158`); the master is armed and written on task 0 only after the existing barrier; `partition_task` is required by the master writer (fail-fast); serial names and master groups are byte-identical (`tests-snapshot-global-identity` green).
- **Modules**: `sham_rank_match` reaches `rank → sum_i64 → any` and `hod_populate` reaches `min_max → sum_i64 ×2 → sum_f64 ×2 → any` unconditionally on every task; the bin layout depends only on the reduced extent, so `n` agrees; pricing by global rank with the first masked candidate masking the rest is identical to the former position-based loop; NaN cannot enter the f64 reductions because the extent is folded with `fmin`/`fmax`.
- **Lifecycle**: `validate_columns = 0` ranks never use identity values before task 0's full open has succeeded, because the broadcast follows it and a task 0 abort reaches them through `MPI_Abort`. The format-version refusal reads a header every task has, so all tasks abort together.

## 5. Test Adequacy Against the Plan

| Slice | Verdict | Gap |
|---|---|---|
| 1 Partition | Met | Exhaustive oracle; abort paths covered |
| 2 Reader | Met with a gap | Single-block ranges only (finding 5) |
| 3 Setters | Met | Regeneration pinned for two pairs; the rebase test reads every role back |
| 4 Lifecycle | Met | Manual MPI runs recorded; serial bytes unchanged by the integration suite |
| 5 Output | Met with gaps | Zero-id under `--compare-created` untested (17); stale comment (16) |
| 6 Collectives | Met | Gate refusals through the real pipeline including nested per-event; `NTask = 2` refusal and acceptance; schema cases |
| 7 Driver | Met | Rebase, abort, `row_offset = 0`, identity shift, resident bytes, boundary tests |
| 8 Ports | Met | Serial bit-identity on fixtures and real data; `hod`'s continue-after-failure path has no unit test (P3) |
| 9 Gate | Met with gaps | Findings 3, 4, 14, 15 |
| 10 CI | Met with a gap | Finding 2 |
| 11 Docs | Met | Residual stale text in §7 |

## 6. Simplification Pass (behaviour-preserving recommendations)

Applying the code-simplifier standard: the implemented behaviour, its accepted edge cases and the test meaning are fixed. None of these was applied; each is safe to do in one small commit with `tests-unit`, `tests-horizontal-v3`, `tests-snapshot-global-identity` and `tests-distributed` as the proof.

1. **One distribution predicate.** Add `static inline int run_is_distributed(void)` to `src/core/task_layout.h` (MPI build and `effective_task_count() > 1`); delete `horizontal_run_is_distributed()` and `distributed()`; use it at `hdf5.c:142`.
2. **One home for HDF5 partition naming.** `output_partition_basename(buf, size, filenr, task)` and `output_master_path_hdf5(buf, size)` in `output/util.c`; `output_path_hdf5()` joins the directory to the former; the master writer and the driver's arming code call them. Removes four `sprintf`/`snprintf` copies and the driver comment that apologises for not sharing one.
3. **Thread the partition task to the writer instead of re-deriving it.** `prepare_output_files()` → `open_hdf5_output_file()` could take the task the driver already has from `partition_task()`; the pinning test `test_writer_task_matches_partition_source` then becomes unnecessary and plan defect 1 disappears. This touches a signature outside the original plan surface, which is now legitimate.
4. **Fold the two scalar reductions into one helper.** `module_snapshot_sum_i64()` and `module_snapshot_sum_f64()` differ only in `MPI_Datatype`; a static `reduce_in_place(function, buf, n, type, op)` would hold the gate, the argument agreement and the `n == 0` skip once.
5. **Replace the driver's two diagnostic statics with a pointer in the gather context**, or accept them: `horizontal_diagnostic_partition` / `horizontal_diagnostic_task` (`horizontal_driver.c:319-320`) are hidden global state the style guide discourages; they exist because `struct HorizontalGatherContext` is shared with unit tests. Adding a `const struct HorizontalForestPartition *partition; int task;` pair to that struct (NULL / 0 in tests) is the cleaner shape.
6. **Gate script**: merge `master_groups()` and `master_totals()` into one pass; share an expected-layout builder between `check_serial_layout()` and `check_task_layout()` that also globs files (this is finding 4); drop the yaml flags (finding 14).
7. **Comparator**: return tree and created counts from `compare_snapshot()` instead of recounting in `compare_runs()` (this is finding 17); one `write_run()` taking a name function replaces `write_task_run()` in its test.
8. **`hod_populate_process_snapshot()`**: split into a local-audit helper returning `{failed, totals, bins}` and the collective tail (finding 19).

## 7. Assessment of the Project Manager's Record

### Decisions under owner authorization

| # | Decision | Assessment |
|---|---|---|
| 1 | One `task <n>:` prefix per log emission, not per physical line | **Agree.** Multi-line messages are rare and the first line carries the tag; a per-line split would mean parsing the formatted body. |
| 2 | `myexit()` removes the aborting rank's in-flight outputs before `MPI_Abort` | **Agree, and it was necessary**: without it a rank-0 FATAL after arming the master would leave a half-written master. The residual (siblings' partials and `--skip`) is finding 1 and is the top follow-up. |
| 3 | NaN in the f64 reductions is the caller's precondition | **Agree.** `MPI_MIN`/`MPI_MAX` on NaN is implementation-defined; a check would need its own agreed error path. The header documents the `fmin`/`fmax` idiom and `hod_populate` follows it. |
| 4 | Skip `MPI_Allreduce` when the agreed `n` is 0 | **Agree.** Avoids a zero-count `MPI_IN_PLACE` on NULL that strict libraries reject; the call sequence is identical because `n` is agreed first. |
| 5 | VISION Principle 4 qualifier for the floating-point sum | **Agree.** The mandated wording was not literally true; the qualifier is one clause inside the same paragraph and the DEVELOPER-GUIDE overclaim was removed. |

### Plan defects

| # | Defect | Assessment |
|---|---|---|
| 1 | "Every caller passes the partition's task" could not hold for `open_hdf5_output_file()` | Resolved correctly for the surface; the re-derivation is pinned by a unit test. Simplification 3 removes the duplication now that surfaces no longer bind. |
| 2 | Positional initialiser at `vertical_driver.c:58` broke the designated-initialiser claim | Resolved by a one-line grant; the plan's repository evidence was wrong, not the code. Nothing further. |
| 3 | VISION wording not literally true for `sum_f64` | Resolved; see decision 5. |

### Carried items (disposition)

| Item | Disposition in this review |
|---|---|
| `--skip` accepts killed ranks' partials (Slice 4 P2) | **Finding 1 (P2).** Follow-up slice recommended. |
| `emit_log` split writes (P3) | Finding 9. |
| Cleanup pair duplicated in `myexit` and `bye`; stale comments at `proto.h:103-112` and `vertical_driver.c:43-45` | Confirmed stale: both comments say outputs are removed "from bye()" only. Add `myexit()`; consider one `remove_incomplete_outputs()` wrapper the two call. |
| Progress fallback `task N: task N of M`; untagged `run_log` banners | Cosmetic; folded into finding 9. |
| Detached-HEAD during `tests-snapshot-global-identity` (tooling hazard) | **Did not recur** in this review's run (branch verified before and after). Cause still unidentified; the gate only calls `git worktree add --detach` into `output/`. Keep the standing rule to verify the branch before committing; investigate separately with `GIT_TRACE=1`. |
| `task_layout.h` overstates that every reader goes through the helpers | Confirmed: raw `NTask`/`ThisTask` remain at `main.c:115,153,448,500,509`, `vertical_driver.c:277,399`, `init.c:183`, `progress.c:178`, `read_ctrees_hdf5.c:1931-1932`, `metadata_hdf5.c:667`. Reword the comment to "the horizontal driver, the collectives and the output writers use these helpers". |
| Naming format written three times | Finding 7 / simplification 2 (it is four, counting the master path). |
| Comparator created-row count includes zero ids | Finding 17. |
| Utility-rejection comments overstate the rule | Finding 13. |
| Refusal message hardcodes `serial_only` on a fail-closed branch | Accurate today (the enum has two values); make the message print the enum name if a third value is ever added. |
| Duplicate-id scan is O(n log n) per call | Acceptable: it runs once per output snapshot on candidates, dominated by the sort it already does. |
| Stale docs from Slice 7 inside Slice 11's surface | **All fixed** (DEVELOPER-GUIDE `:1108,1204,331,1227,1124-1139,915,1206,296-309`; USER-GUIDE `:461,503-521`; CHANGELOG `:11-16`; the six skills). |
| Stale docs outside every surface | **Still stale, fix in one sweep:** `.agents/skills/mimic-config-and-flags/references/all-config-keys.md:60` (says `NTask > 1` is rejected at startup) and `:50` (ceiling not per rank); `.agents/skills/mimic-properties/SKILL.md:44` (getters only); `src/core/module_interface.h:424-425` (created-ID formula lacks `row_offset`); `simulations/shin-uchuu/README.md:31` and `simulations/micro-uchuu-ascii-horizontal/README.md:62` (serial-only, rejected at config time); `src/include/globals.h:34` ("IMMUTABLE INPUT", now rebased by the horizontal driver); `src/io/horizontal/reader.h:135-136` ("or an empty range"); `docs/DEVELOPER-GUIDE.md:1113` (`main.c` line numbers drifted before and during the run). |
| No permanent multi-block range-read test | **Finding 5 (P2).** |
| `hod` continue-after-failure path untested | P3; a unit case that fails `audit_scan` mid-population and asserts the module returns −1 and writes nothing would close it. |
| `hod` README collective paragraph longer than "one short paragraph" | Accept; it is accurate and the length is justified by the ordering rule. |
| Serial invalid-population path in `sham` logs two error lines | Accept; both lines are true and the second names the snapshot. |
| Gate libyaml fallback lacks Homebrew paths | Finding 14: the flags are unnecessary, so remove them rather than extend them. |
| `halos-only`/`sage16` legs lack a non-empty guard | **Finding 3 (P2).** |
| Control-test compile flags differ from production; warnings do not fail | P3: add `-O2`; production has no `-Werror` either, so parity holds on the second point. |
| `make help` omits `tests-distributed` | Finding 18. |
| CI timeouts exceed the job budget; log dump unreachable | **Finding 2 (P2).** |
| First Linux/GCC/Open MPI CI run unverified | Still true; the owner reads remote CI after pushing. The apt Open MPI on `ubuntu-latest` needs `--oversubscribe` for `-np 8`, which the step sets. |
| Task 0 does not assert `row_cuts[s][ntask] == halo_count(s)` | Unreachable today (the scan sees the whole column); one comparison in `horizontal_compute_partition()` is free hardening. |
| MPI control test should cover `n == 0` and a NaN-free `min_max` | Covered: 9 cases including both (`tests/mpi/test_snapshot_collectives_mpi.c`). |

### Rejected reviewer claim

- Qwen's claim (Slice 9) that the fixture-bounded test surface is a contract defect: **rejection upheld**. The criteria were satisfiable and the surface permitted stronger tests; findings 3 and 4 show the stronger tests are still available inside that surface.

## 8. Vision and Style Alignment

- **Physics-agnostic core**: the collectives are generic reductions and a rank; no model header enters `src/core/`. The `snapshot_distribution` declaration is metadata, validated and generated, consistent with Principle 3.
- **One coherent processing model**: the FoF sweep is untouched; distribution is confined to startup, the range load, the rebase and `execute_post_snapshot()`. The VISION edit is two paragraphs and is accurate.
- **Bounded memory and explicit ownership**: footprints and the ceiling are per rank; the partition tables are counted (`horizontal_retained_resident_bytes()`); scratch is `MEM_UTILITY` and freed per call; the weights array is freed before any generation is sized.
- **Fast failure**: every new refusal names the module, task count, snapshot, rows and `source_format`; the unknown-callback-kind branch fails closed.
- **Style**: zero compiler warnings under `-Wall -Wextra -Wshadow -Wformat-security -Wundef` for both builds; `check-format` clean; Doxygen contracts on every new public header; `MIMIC_RESULT` markers in every new test. The two hidden statics (`horizontal_diagnostic_*`) are the one style-guide tension, discussed in simplification 5.

## 9. Coverage Summary

- **Scope reviewed:** every file of `git diff fe0b9cad..HEAD` (88 files); the new core sources read in full; the driver, reader, output, registry, lifecycle and module diffs read line by line; tests, gate, comparator, generators and documentation through delegated sub-reviews, each claim re-verified at its `file:line` before inclusion.
- **Requirements checked against:** the plan's frozen decisions D1–D13, the Distribution Contract, and every slice's acceptance criteria; VISION Principles 1, 3, 4, 5, 7; the STYLE-GUIDE sections on comments, logging, tests and generated code.
- **Dimensions checked:** correctness, boundary conditions (empty ranks, all-empty, single bucket, `n == 0`, NaN, duplicate ids, overflow), ownership and cleanup on every error path, MPI ordering and agreement, numerical identity, output layout, tests and false-green paths, portability (macOS Clang and Linux GCC/Open MPI), maintainability, documentation.
- **Validation run:** listed in the Executive Summary; logs under `archive/test-logs/review-2026-10-06/` (gitignored). Differential code-health reported no include cycles and 15 structural candidates; the two that matter are `run_horizontal_driver()` (CC 30, +10) and `hod_populate_process_snapshot()` (CC 28, +14), both addressed in §6.
- **Not run:** the manual real-data stage on `micro-uchuu-horizontal` (accepted on the Slice 9 record); remote CI on Linux.

## 10. Verdict

**PASS WITH RISKS.** The runtime is correct, bit-identical across rank counts, and well documented. The four P2 items are a `--skip` operational seam the owner already knows about, two cheap gate-hardening gaps, one CI timeout budget, and one missing test for a code path the real-data stage has proven. None blocks merging the branch; findings 2–4 should land before the gate is relied on in CI, and finding 1 before multi-rank time-limited runs are resubmitted with `--skip`.

---

## 11. Resolution (2026-10-06)

The owner accepted every finding and recommendation above. They were implemented the same day in six bounded work packages, each delegated to a lower-power model under the reviewer's supervision and reviewed diff by diff before acceptance; the independent external panel's review follows this section's commit. What landed, by finding:

| Finding | Resolution |
|---|---|
| 1 (P2) `--skip` over truncated partitions | `src/core/vertical_driver.c`: an in-flight marker `<OutputDir>/<base>_<NNN>.inflight` per partition, created before the outputs are claimed, registered with the failure-cleanup registry and unlinked once the outputs are closed; `--skip` redoes a marked partition, skips an unmarked complete one and stays fatal on an unmarked partial one. Rename-on-finalize was rejected because the binary writer reopens its files by name per batch. Pinned by two cases in `tests/unit/test_enumerated_driver.c`; proven end to end by a SIGKILL mid-run and a `--skip` resume whose output is data-identical to a clean run (`h5diff`; only HDF5 timestamps differ). USER-GUIDE, CHANGELOG, the debugging and run-and-operate skills updated. |
| 2 (P2) CI timeout budget and log | `RUN_TIMEOUT` 60 s; the first timed-out launch ends its model's leg; the log prints in a separate `if: always()` step under `::group::`. |
| 3 (P2) empty-output false green | Every leg requires a positive serial `TotHalosPerSnap` sum and every count a positive `PASSED: N galaxies`. |
| 4 (P2) partition files unchecked | Above `-np 1` the gate requires every `<base>_<snap>_task<t>.hdf5` on disk and resolves every master `Galaxies` link; one expected-layout builder serves both layout checks and one pass reads the master. |
| 5 (P2) multi-block range read untested | A committed `wide_slab` fixture (one forest, snapshot 0 of 8,600 rows, 1.2 MB plus its 0.9 MB L-Halo source) and `test_v3_multi_block_range_reads_match_whole_reads`, which checks six ranges across and inside the second block, the two-block `ForestIndex` scan, and the whole read against the fixture's known chain. |
| 6 (P3) | `run_is_distributed()` in `src/core/task_layout.h`; both statics deleted; the writer no longer re-derives the predicate. |
| 7 (P3) | `output_partition_basename()` and `output_master_path_hdf5()` in `src/io/output/util.c`, a static group-name helper in `master_hdf5.c`; the driver's hand-formatted master path is gone. |
| 8 (P3) | Both test hooks declared in `src/include/proto.h`; the hand-written prototypes removed. |
| 9 (P3) | `emit_log()` assembles each line in one buffer (`struct LogLine`) and writes it with one call, falling back to the multi-call path only for a line over 4 KB; byte identity proven by a harness diff over NTask 0, 1 and 4 in all three verbosity modes and by the integration tier. |
| 10 (P3) | Both link validators take `row_lo` and print the snapshot row. |
| 11 (P3) | Recorded as a pathway residual; no code change by design (D4). |
| 12 (P3) | `weights[f] > capacity - filled`; task 0 also asserts `row_cuts[s][ntask]` equals the slab's halo count. |
| 13 (P3) | The key is rejected on any utility module; the comments and the integration test say and cover it. |
| 14, 15 (P3) | The libyaml flags are gone, the control compile uses `-O2`, and the case count is a constant. |
| 16 (P3) | Comment fixed; the `NTask = 3` case and the new writer case reset their globals through one cleanup path. |
| 17 (P3) | `compare_snapshot()` returns its counts; the created-row summary counts `ids < 0`; the zero-id case runs with and without `--compare-created`. |
| 18 (P3) | `make help` lists `tests-distributed`. |
| 19 (P3) | `hod_populate_process_snapshot()` is a scan, the extent collective, a layout-and-fill step and a reduce-and-report tail, with the collective order unchanged; a new unit case fails the scan part-way and asserts the module returns −1, writes nothing and frees its bins; serial identity on micro-Uchuu re-proven (3,114,016 galaxies, 1,487 created rows, audit lines identical). |
| §6.3 | `prepare_output_files()` and `open_hdf5_output_file()` take the partition task; the pinning test became a test that the writer names the file by the task it is given. |
| §6.4 | One `reduce_in_place()` and a shared `reduction_prepare()` behind the three reductions. |
| §6.5 | The two diagnostic statics became `partition` and `task` members of `struct HorizontalGatherContext`; the two test files that build the struct zero-initialise it. |
| §7 stale text | Every listed location fixed, plus `docs/DEVELOPER-GUIDE.md:1231` and `mimic-run-and-operate` `:236`, which still described the old `--skip` residual. |

The pathway records the implementation and selects chunked slab streaming as the next focus. This document, the implementation plan and the brief are archived to `archive/dev-plans/` on merge; the acceptance record stays in `docs/dev/`.
