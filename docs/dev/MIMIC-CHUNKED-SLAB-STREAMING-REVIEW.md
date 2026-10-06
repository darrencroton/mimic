# Chunked Slab Streaming — Post-Implementation Code Review

**Purpose:** An independent, whole-branch review of the chunked slab streaming work (`feature/chunked-slab-streaming`, `main..04449622`, 19 commits, 37 files, +2,841/−525) against the goals of `MIMIC-CHUNKED-SLAB-STREAMING-IMPLEMENTATION-PLAN.md`, with correctness and code quality first and a secondary simplification pass now that the plan is executed. It also disposes of every delegated resolution, plan defect and carried open item the PM recorded in its run notes.

**Reviewer:** Claude Fable 5.1, 2026-10-07, in a fresh session; read the full diff, followed every changed interface one hop outward, re-ran the build, format, generated-code and test gates (see [Validation evidence](#validation-evidence)), ran differential `code-health` against `main`, and delegated a documentation-truth sweep to a subagent whose findings were checked before inclusion.

**Standing of this document:** a review record under `docs/dev/`; it may be archived with the plan on merge. It is not user documentation and no document outside `docs/dev/` may reference it.

---

## Executive summary

**Verdict: PASS WITH RISKS.** The plan's goals are met and the core is correct. No P0 or P1 finding survived verification. The branch is mergeable as it stands; the items below are improvements, not blockers.

**What was delivered and proven.** `input.forest_chunks: G` sweeps each task's forest range in `G` contiguous chunks, each through every snapshot with its own retained generations, appending per chunk into the unchanged per-`(snapshot, task)` partition files. The acceptance predicate (C9) holds with no tolerance: byte-identical galaxies per `UniqueGalaxyID` at `G = 2, 3, 8` serially and `-np 2 × G = 2`, `-np 3 × G = 3` under MPI on the fixture for `halos-only`, `sage16` and the `hod` variant (created rows included), and on real micro-Uchuu data for `sage16` and `halos-only` at `G = 1, 2, 4, 8` and `-np 2 × G = 2` (18 of 18 comparisons identical, serial `G = 1` identical to the pre-plan references). Measured peak RSS on micro-Uchuu at `G = 8`: `sage16` 2.452 → 0.307 GB (0.125), `halos-only` 4.998 → 1.453 GB (0.291), wall-clock flat within 6%. The distributed gate grew from 158 to 260 checks with no skips.

**Where the risk sits.** Five P2 items, all small and all fixable in one short follow-up commit: (1) the shipped converter still prints "needs chunked slab streaming, which Mimic does not implement" on every `convert_trees.py` stage (`convert/mimic-convert/runtime_routes.py:24`), a stale external claim the plan's inventory missed, and the converter README now describes output the tool no longer produces; (2) the only evidence that an *empty first visit* of a partition works (chunk 0 holds no rows of an output snapshot, then later chunks append) is a manual PM run at `G = 1024`, not a committed test, although the path is reachable on real datasets; (3) the driver has no defence of its own against the snapshot scope under chunking, so a harness that builds `MimicConfig` without the parser would silently run `post_snapshot` on partial populations; (4) the rebase abort messages, the `HorizontalGatherContext.task` field and the `proto.h` prototype still say "task" where a *range* (task's chunk) is meant, so a defective-dataset abort at `G > 1` names the wrong unit; (5) the user guide's "three deliberate restrictions" paragraph and the run-and-operate skill still scope the forest-blocked refusal to multi-rank runs, when a one-task chunked run is refused the same way. Everything else is P3: a one-shot, const-but-mutating partition API that invites misuse, a vacuous test assertion, stale Makefile help text, a handful of documentation wording nits, and small test-hygiene items.

**Independent validation.** Re-run in this session on `04449622`: clean default build with zero warnings, `make check-generated`, `make check-format`, `make check-docs`, `make tests-unit` 57/57, `make tests-horizontal-v3` (the new test's 7 markers all PASS), `make tests-snapshot-global-identity`, and `MPIRUN="mpirun --oversubscribe" make tests-distributed` at 260 checks with no skips (41 s). A `-Wall -Wextra -Wshadow -Wformat-security -Wundef -Wconversion` build puts no warning in `horizontal_driver.c` or `horizontal_partition.c`, and none on any line this branch changed.

**Recommendation.** Merge after the one follow-up commit described under [Recommended actions](#recommended-actions) (items 1–5 are each under 30 lines). Record the PM's plan-amendment lessons in the next implementation-plan template rather than editing the executed plan.

---

## Scope and method

- **Target:** `git diff main..HEAD` at `04449622`, every file; surrounding code in `src/core/horizontal_driver.c`, `src/core/horizontal_partition.{c,h}`, `src/io/output/{hdf5.c,util.c}`, `src/core/read_parameter_file.c`, the four changed unit tests, the new integration test and the gate script.
- **Requirement sources:** the implementation plan (frozen decisions C1–C12, six slice receipts), `docs/VISION.md` (Principles 1, 4, 5), `docs/STYLE-GUIDE.md`, the PM run notes and `HANDOFF.md`.
- **Authorization status:** every slice had a fresh drift audit and a three-model code-review panel on its exact final commit (PM run `20261006T110723Z-df016f`, status complete). This review does not re-audit authorization; it reviews the end result.
- **Dimensions checked:** requirements fit, functional correctness, boundary conditions, state and lifetime (the cleanup registry, the HDF5 handle, the retention pool across chunks), interface contracts (range vs task indexing at every table site), MPI interplay, numerical and scientific validity (identity, no tolerance), error handling and recovery (the multi-visit failure window), tests, performance (per-chunk costs), portability, maintainability, documentation.
- **Not re-done here:** mechanical lint (each slice ran `lint.py check`; `make check-format` re-run below) and a full style-guide audit.

---

## Goal attainment

| Plan goal | Evidence | Status |
|---|---|---|
| Peak memory bounded by the widest *chunk*, chosen by the run file (C1, C8) | Acceptance record: `sage16` 0.495/0.235/0.125 of `G = 1` at `G = 2/4/8`; `halos-only` 0.740/0.495/0.291 | Met. `halos-only` falls more slowly than `1/G`; the record does not attribute why (see Open questions) |
| Same galaxies whatever `G` or task count (C9) | 260-check gate; 18/18 real-data comparisons bitwise identical, created rows included | Met |
| Serial `G = 1` byte- and log-identical to before (C10) | References re-verified identical; `test_processing_order.py` lifecycle pins pass; `g1` log carries no partition/chunk line | Met |
| Output layout, names, master and `TotHalosPerSnap` unchanged at every `G` (C6) | `test_chunked_sweep.py` file-set and `TotHalosPerSnap` checks; gate layout checks at every leg | Met |
| Failure removes every non-final partition, keeps finalised ones (C6) | `test_failure_keeps_finalised_partitions_and_removes_the_rest` (snapshots 2, 4 survive; 5, 6 and master removed) | Met |
| Snapshot scope refused under chunking (C5) | Parser rejection, unit-tested; gate `chunked_refused_sham`, `chunked_refused_hod` | Met at configuration; no driver-side defence (P2-2) |
| Version 2 and non-forest-blocked data refused when chunked (C2) | Driver refusals name both counts; `test_horizontal_retention_budget.c` wording asserts | Met |
| No format, reader or converter change (C11) | Diff touches none of `src/io/horizontal/`, `convert/` code | Met |
| Documentation states the delivered contract, numbers trace to the record (Slice 6) | See [Documentation review](#documentation-review) | See below |

---

## Findings

Severity follows the code-review skill: P0/P1 require a stated reachability path; none qualified.

### P2

0. **[P2] `convert/mimic-convert/runtime_routes.py:24`, `:70`; `convert/mimic-convert/convert_trees.py:350-352`; `convert/mimic-convert/README.md:18` — the converter still prints the claim the branch retracts.** `FULL_UCHUU_NOT_RUNNABLE` reads "Full Uchuu is not runnable: it exceeds whole-slab memory and needs chunked slab streaming, which Mimic does not implement", and every `convert_trees.py` stage prints it as its `runtime support:` line; `convert_trees.py`'s width note says "chunked slab streaming is not implemented". Slice 6 reworded the README sentence to C12's position ("not claimed … storage-bound … its largest forest bounds any chunk") but the README still says the stage "prints a `runtime support:` line naming these routes", so the README now describes output the tool does not produce, and a user who runs the converter is told Mimic lacks the feature this branch ships. The plan's Slice 6 inventory missed these Python strings (the final drift audit noted `convert_trees.py:352` as an out-of-surface residual). Four test assertions pin the old text (`convert/mimic-convert/tests/test_cli.py:213`, `tests/test_runtime_routes.py:48`, and their neighbours). **Fix:** reword the constant, the width note and `report.py:335`'s "whole-slab memory decides whether it can run" to the README's wording, and update the four assertions. The documentation subagent rated this P1; it is rated P2 here because it changes no computation and misleads rather than breaks, but it is the first thing to fix.

1. **[P2] `tests/unit/test_hdf5_write_attrs.c:236` — the empty-first-visit path has no committed test.** A partition whose chunk 0 holds no rows of an output snapshot is created empty by `prepare_output_files()`, receives nothing from `save_halos_hdf5()` (the loop at `src/io/output/hdf5.c:385-399` appends nothing and allocates no buffer), and is appended to by later chunks through `reopen_hdf5_output_file()`. This is reachable on any dataset where a chunk's forests have no halo at an output snapshot (forests appear late; chunk 0 of a late-forming region at an early output snapshot). The committed `forest_blocks` fixture cannot express it (the plan says so), and the only evidence is the PM's manual `G = 1024` run on micro-Uchuu, which is not in the acceptance record. **Fix:** add one case to the writer seam test with `split = 0` (first visit writes nothing, second visit writes all 2,500 rows) asserting the same bytes, row order and `TotHalosPerSnap` as the one-visit file; it is a five-line addition to `write_partition_in_visits()`'s callers. Optionally extend `test_chunked_sweep.py` with a fixture copy whose first forests are removed from one output snapshot, but the unit case is the cheap, sufficient guard.

2. **[P2] `src/core/horizontal_driver.c:2393` — no driver-side guard on the snapshot scope under chunking.** `run_horizontal_driver()` computes `chunked` and, when `nchunk > 1`, `horizontal_sweep_chunk()` still calls `horizontal_run_post_snapshot(cur)` (`:1891`) once per chunk per snapshot. The parser's rejection in `validate_and_postprocess()` is the only thing that prevents a snapshot module from running over a chunk's partial population, which would be a silent violation of Principle 4 (the snapshot scope is defined over the complete population). Reachability: every run file is caught by the parser, so this is unreachable from the CLI; it is reachable from any harness that fills `MimicConfig` by hand, which `tests/unit/test_horizontal_retention_budget.c:400` already does (it sets `ForestChunks = 1`). Capped at P2 for that reason. **Fix:** one `FATAL_ERROR` beside the `chunked` computation: `if (chunked && MimicConfig.num_post_snapshot > 0)` naming the phase and the chunk count. Mimic's rule is fail-fast on impossible processing contracts; a second, cheap check at the point of use is in keeping with the retention-budget and INT_MAX refusals that already duplicate configuration-time knowledge.

3. **[P2] `src/core/horizontal_driver.c:823`, `:835`; `src/include/types.h:320`; `src/include/proto.h:125` — "task" where a range is meant.** `horizontal_rebase_slab_links()` is now passed a range index (`state->range`, `t * nchunk + c`) but its two aborts still read "for task %d of a %d-task partition" and "task %d's partition range there is [lo, hi)", so a defective dataset (a link that leaves its forest, which the forest-blocking scan cannot catch) aborting at `G > 1` reports, for example, "task 3" on a one-task `G = 4` run. `HorizontalGatherContext.task` is documented as "this task's rank in `partition`" but is set to `state->range` at `:2157`, and the exported prototype names the parameter `task`. The PM recorded all three as out-of-surface residuals (Slice 4 withdrew the direction; the pinned needle lives in `tests/unit/test_horizontal_distribution.c`). Now that the plan is complete there is no surface constraint. **Fix:** rename the field and parameter to `range`, reword the two aborts to "range %d (task %d's chunk %d) of a %d-range partition" and update the two pinned needles in `test_horizontal_distribution.c` (the `other_chunk_link` case already asserts "task 3's rows [3, 5)", which is the wrong unit and should become "range 3").

3b. **[P2] `docs/USER-GUIDE.md:461-465`; `.agents/skills/mimic-run-and-operate/SKILL.md:125` — the restriction list still scopes the forest-blocked refusal to multi-rank runs.** The user guide says a horizontal run has "three deliberate restrictions … the third is enforced once the dataset has been opened" and heads it "Multi-rank runs need a forest-blocked version 3 dataset. Under `mpirun` with more than one rank…"; the skill says "Two restrictions are enforced at config time … and a third applies to multi-rank runs". `horizontal_partition_run()` (`src/core/horizontal_driver.c:1066-1073`) and the forest-blocking scan (`:982-986`) refuse a one-task run at `forest_chunks > 1` identically, and the snapshot-scope rejection is a fourth, configuration-time rule. A user following the list would expect a `forest_chunks: 4` run on a version 2 dataset to work. **Fix:** retitle the bullet "Multi-rank or chunked runs …", say so in the lead-in, and add the chunk-specific configuration rejection (or point at the `forest_chunks` bullets that already describe it).

### P3

4. **[P3] `src/core/horizontal_partition.c:152`, `src/core/horizontal_partition.h:153-176` — the two-step cut is a one-shot API that takes a `const` partition and mutates it.** `horizontal_partition_cut_forests()` writes compact task cuts to entries `0..ntask`; `horizontal_partition_cut_chunks()` then relocates them in place to entries `t * nchunk` ("walking down") and sub-cuts. The header documents it as non-idempotent and warns that a second call corrupts the table; the `const` qualifier is honoured only because `forest_cuts` is a pointer member. This was the PM's delegated resolution of a contract contradiction (Slice 2) and was the right call under the frozen plan; it is not the right end state. See [Simplification](#simplification-pass) item S1.

5. **[P3] `simulations/mini-millennium-horizontal/_tests/integration/test_chunked_sweep.py:353` — vacuous assertion.** `g2.index("Loaded snapshot ", boundary) > boundary` cannot fail: `str.index(sub, start)` returns at least `start`, and equality would need the "Sweeping" line to begin with "Loaded snapshot". The adjacent `before == 7 and after == 7` counts already carry the ordering claim. **Fix:** delete it, or make it assert something: the first "Loaded snapshot" after the boundary carries "chunk 1 of 2".

6. **[P3] `tests/unit/test_parameter_parsing.c:244`, `:1983`, `:1990` — helper hygiene.** `add_post_snapshot_phase()` leaves `<path>.tmp` behind on its failure returns and re-implements the header match of `write_null_phase_fixture()`; the occurrence-count loop is duplicated (code-health flagged the block at `:1872` as a new exact duplicate). **Fix:** `unlink(tmp_path)` on the failure paths and a `count_occurrences(haystack, needle)` helper.

7. **[P3] `Makefile:509`, `:898-903` — `make help` and the `tests-distributed` comment under-describe the gate.** Both still say "serial-versus-MPI identity at -np 1, 2, 3, 4 and 8 … plus the version 2 refusal" and omit the chunked legs and refusal checks. No slice's surface covered them; a user reading `make help` learns nothing about the chunk legs. **Fix:** add "chunked legs at forest_chunks 2, 3, 8 serial and -np 2 × 2, -np 3 × 3, and the chunked refusal".

8. **[P3] `src/core/horizontal_driver.c:219` — the registry's `ready` flag is not asserted at arm time.** `horizontal_arm_partition_output_path()` checks `horizontal_partition_output_armed()`, which returns 0 whenever `ready == 0`, then writes the entry without setting `ready`. Today every arm follows `horizontal_open_output()`, which sets `ready`, so this is a future-caller hazard only: an arm before the reset would succeed and the entry would be invisible to the failure handler. **Fix:** `if (!horizontal_partition_output_entries_ready) FATAL_ERROR(...)` at the top of the arm function.

9. **[P3] `tests/manual/test_distributed_identity.py:427`, `:669` — `row_order_` is gated on the serial chunked legs only.** The C9 predicate asks for row order on serial legs, so this is met; but the MPI × chunk legs' row order is unverified and the documentation had to be scoped to say so. Tasks own ascending forest ranges and chunks ascending sub-ranges, so concatenating each snapshot's `_taskNNN` partitions in task order must reproduce the serial file order. **Fix (post-plan):** extend `read_row_order()` to concatenate task partitions per snapshot and add the marker to `chunked_mpi()`; a handful of lines.

10. **[P3] `tests/unit/test_hdf5_write_attrs.c:260-300` — the test helpers leak HDF5 handles on a mid-helper assertion failure** (no `goto cleanup` discipline). Test hygiene only; the framework's other HDF5 tests share the pattern.

### No change justified (examined and accepted)

- `horizontal_widest_snapshot()` now runs for unpartitioned runs too (`:2389`): O(snapshot count) over published counts, and it keeps the chunk line's data and the partition's weighting in one place.
- The `MAX_HALO_ARRAY_SIZE` warning repeats per chunk: it judges the chunk's rows, which differ per chunk, so one line per chunk is the correct granularity.
- The `MPI_Bcast` size guard moved into `horizontal_partition_create()` (its message changed; the plan's "no message change" clause in Slice 2 is unreachable at > 2³¹ row cuts and nothing pins it).
- `horizontal_sweep_chunk()` has cyclomatic complexity 21 (code-health): it is the former loop body moved out of `run_horizontal_driver()` (which dropped by 15) plus the chunk branches; splitting it further would scatter the retention invariants the comments explain.
- `validate_and_postprocess()` is at complexity 47 (+6 from the two new rules) in a 200-line function. Pre-existing hotspot; the two rules follow its template exactly. A future extraction of the driver-rule block into `validate_driver_rules()` is reasonable but not this branch's debt.
- The `Wrote snapshot N output (K galaxies)` line now reports the accumulated `TotHalosPerSnap` rather than `cur->processed.count`. At `G = 1` the two are equal (every processed halo of generation `N` has `SnapNum == N`), and the integration tests that pin the line pass.

---

## Correctness notes (what was traced and found sound)

- **Range indexing is complete.** Every table-stride site in the driver goes through `horizontal_partition_range_count()` or `horizontal_row_cut(partition, s, range)`: `horizontal_global_row`, `horizontal_task_rows_note`, `horizontal_task_rows`, `horizontal_rebase_link`, `horizontal_rebase_slab_links`, `horizontal_partition_resident_bytes`, the compute-partition log loop, the end-fill hardening check and both broadcasts. `state->range` is set once per chunk at the top of `horizontal_sweep_chunk()` and read everywhere else; `state->task` is used only to compute it and to filter output partitions. No bare `ntask + 1` stride remains.
- **The in-place relocation in `cut_chunks()` is correct.** Writing `forest_cuts[t * nchunk] = forest_cuts[t]` for `t = ntask .. 1` never clobbers an unread entry (`t * nchunk ≥ t > t'`), and the per-task `forest_hi` is read before `cut_forests()` overwrites `task_cuts[nchunk]`. The brute-force oracle over 87,381 weight vectors × 3 task counts × 3 chunk counts confirms the optimum and the nesting; the `ntask == 1, nchunk == G` equals `ntask == G` property is proven.
- **Retention across chunks.** Each chunk ends with `retained_count == 0` (fatal otherwise, naming the chunk); `horizontal_release_generation()` resets both `gen->snapnum` and the lookup slot, so `horizontal_acquire_generation()`'s "already retained" check holds at the next chunk's first load. Spare pools, workspace and scratch carry over by construction; `max_retained_*` are run-wide maxima.
- **Identity.** `rows_per_unit` is evaluated once from the global largest slab; `identity.row_offset` is the chunk's `row_cuts[s][range]`; the created-record path is gated with positive created-row counts on both sides (`hod` variant).
- **Output lifecycle.** `rows_before` is captured before the save so the "Appended" count is exact; the attribute is stamped once (a second `H5Acreate` would fail, and the comment says so); every visit closes the file with the fatal-on-close check; the entry is released only after the last close. A `reopen` failure or a mid-visit abort leaves the entry armed, so `horizontal_driver_remove_incomplete_outputs()` rebuilds the path from `MimicConfig` and unlinks it; nothing is allocated, so the leak check sees nothing. A failure *before* `horizontal_open_output()` finds `ready == 0` and removes nothing, which is right because nothing has been created.
- **MPI interplay.** A chunked one-task run on an MPI build has `effective_task_count() == 1`, computes its own partition and skips both broadcasts; a distributed chunked run broadcasts `nranges + 1` forest cuts and `snapshot_count × (nranges + 1)` row cuts, both bounded to `int` at creation. The driver's only MPI calls are still the two broadcasts; collectives of the snapshot scope are unreachable under chunking (modulo P2-2).
- **Progress bar and overflow.** `snapshot_count × nchunk` and `chunk × snapshot_count + snapnum` are `int64_t`; `ntask × nchunk + 1 ≤ INT_MAX` is enforced at creation.
- **Configuration.** `forest_chunks` is parsed with the ceiling's strictness; `0`, negatives, fractions, suffixes, sequences, `> INT_MAX` and the misspelling are each fatal with the key named; the vertical and snapshot-scope rejections are `ERROR_LOG` counted into `errors`, and a vertical run with the phase keeps a single report.

---

## Simplification pass

Applied with the `code-simplifier` discipline: behaviour, contracts and accepted edge cases are fixed; the end result trumps pre-implementation plan text. Differential `code-health` against `main` (Lizard 1.23.0 covered C and Python; no unmeasured family relevant to the change) surfaced the candidates below; each was read in context.

- **S1 — One entry point for the two-level cut.** Replace the externally visible `cut_forests` → `cut_chunks` sequence with `horizontal_partition_cut(const int64_t *weights, struct HorizontalForestPartition *partition)` that computes the task cuts into a tracked scratch array of `ntask + 1` entries, writes them directly to `forest_cuts[t * nchunk]`, and sub-cuts each task's range. The relocation step, the one-shot warning and the misleading `const` all disappear; `horizontal_partition_cut_forests()` stays public for the oracle tests. `horizontal_compute_partition()` makes one call. Behaviour-preserving (the brute-force tests and the `nchunk == 1` whole-table equality pin it).
- **S2 — `horizontal_widest_rows()`'s out-parameters in the task log loop.** The loop at `horizontal_compute_partition()` passes an `unused` out-parameter twice to get a task's `[lo, hi)`. A two-line inline (`row_lo = horizontal_row_cut(p, widest, first_range); row_hi = horizontal_row_cut(p, widest, first_range + nchunk)`) with the existing `snapshot_count > 0` guard reads more plainly. Cosmetic; optional.
- **S3 — Test boilerplate duplication.** Code-health's new exact-duplicate blocks are the `find_repo_root()` and framework-import preamble that every `simulations/*/_tests/integration/*.py` shares (`test_chunked_sweep.py:51-83`) and the occurrence-count loops in `test_parameter_parsing.c` (P3-6). The former is an established, intentional pattern; the latter is worth a helper.
- **S4 — Nothing else.** The registry rewrite is smaller and clearer than the two-slot original; the `HORIZONTAL_CHUNKS_LEVER` macro keeps the three width messages consistent; the gate's `compare()`/`run_marker()`/`LegStopped` refactor removed duplication rather than adding it.

---

## PM delegated resolutions and plan defects — assessment

Each numbered item is the PM's, assessed here for whether the resolution was right and whether anything remains.

| # | Resolution | Assessment | Remaining action |
|---|---|---|---|
| 1 | Slice 2: `cut_chunks` relocates compact task cuts to `t·nchunk` rather than the plan's "exactly as `cut_forests` wrote them" | Correct and necessary under the frozen plan; the alternative (changing `cut_forests`'s output layout) was forbidden by Slice 4's non-goals. | Now that the plan is executed, collapse into one entry point (S1 / P3-4). Plan-template lesson: specify table layouts once, in the data-structure contract, not per criterion. |
| 2 | Slice 2: broadcast-size abort moved into `create()`, changing its message against criterion 4 | Tolerable: unreachable (> 2³¹ row cuts), nothing pins it, and `create()` is the right owner. | None. Lesson: "no message change" clauses should exempt checks a criterion relocates. |
| 3 | Slice 3: comment-only corrections to `open_hdf5_output_file()`'s doc and `proto.h`'s registry block | Correct; a change that falsifies a comment must be allowed to fix it. | None. Lesson adopted: allow comment corrections the slice's own change falsifies. |
| 4, 6, 7 | Slice 4: reword rebase messages to name the range; withdrawn when the pinned needle proved to be outside the surface | PM's withdrawal was procedurally right (notes cannot widen a frozen surface). The defect is real and now has no surface constraint. | **P2-3**: fix now. |
| 5 | Slice 4: stale line citations; "a generation is loaded exactly once" is a runtime FATAL, reworded | Correct reading; the only coherent one. | None. Lesson: cite by quoted text, never by line. |
| 8 | Slice 5: record measured at the gate commit, committed in descendants | Correct; the record states it unambiguously. | None. |
| 9 | Slice 6: grants for `simulations/uchuu/README.md:35` and `convert/mimic-convert/README.md:18` | Correct and necessary; the plan's inventory missed them, and it also missed the converter's Python strings that print the same sentence (P2-0), which a `git grep` over code as well as Markdown would have caught. | **P2-0**: fix the converter strings now. Lesson: inventory stale sentences with `git grep` across code and prose at plan time. |

**Plan amendments:** the plan is digest-bound and executed; do not edit it. Carry the five lessons above into the `implementation-plan` skill's template (or its next revision) where they generalise.

---

## Open items carried — disposition

| Item (PM notes / HANDOFF) | Disposition |
|---|---|
| `add_post_snapshot_phase()` leaves `.tmp` and duplicates a matcher (Slice 1, Fable P3) | Fix in the follow-up commit (P3-6). |
| Rebase messages, `HorizontalGatherContext.task` doc, `proto.h:125` parameter name (Slice 4 residuals) | Fix now (P2-3). |
| No driver-side `post_snapshot` guard under chunking (Slice 4 residual) | Fix now (P2-2). |
| Zero-row first visit verified only by PM on micro-Uchuu `G = 1024` (Slice 5, closed by PM) | Re-opened as a test gap (P2-1): PM evidence is not a committed test. |
| `Makefile:509` and `:898-903` under-describe the gate (Slice 5 residual) | Fix in the follow-up commit (P3-7). |
| Record's summary line drops "; log: <path>"; `chunked_serial` docstring "recorded as not run"; `read_row_order` raises outside markers on a malformed partition (Slice 5 P3s) | Accept as-is: cosmetic, and the malformed-partition path is unreachable after a passing launch. |
| `test_chunked_sweep.py:353` vacuous; MAX_HALO warning per chunk; widest slab at `G = 1` (Slice 4 P3s) | Fix the first (P3-5); the other two are no change justified. |
| Arm does not check `ready`; `output_path_hdf5` could FATAL inside atexit; unit fixture's NaN columns; helper handle leaks (Slice 3 P3s) | Fix the first (P3-8, one line); the `output_path_hdf5` case is unreachable (same buffer size, path built at arm time); the NaN columns are stable bytes compared both ways and do not weaken the test; handle leaks are P3-10. |
| `cut_chunks` takes `const` but writes (Slice 2 P3) | Resolve through S1. |
| `halos-only` peak RSS exceeds `sage16`'s on micro-Uchuu at `G = 1` (4.998 vs 2.452 GB) and falls more slowly with `G` | Not a defect; see Open questions. Worth one profiling session. |

---

## Validation evidence

Independent re-run in this review session on `04449622`, strictly sequential, by a subagent that captured every log under `archive/test-logs/review-gates-20261007-064758/` and returned summaries; no tracked file was modified and the default build was restored afterwards.

| Step | Command | Exit | Time | Result |
|---|---|---|---|---|
| 1 | `make clean && make -j` (defaults) | 0 | 2 s | 0 `warning:` lines |
| 1b | same with `EXTRA_CFLAGS="-Wall -Wextra -Wshadow -Wformat-security -Wundef -Wconversion"` | 0 | 2 s | no warning in `horizontal_driver.c` or `horizontal_partition.c`; the 2 in `read_parameter_file.c` (`:877`, `:1203`) and 4 in `hdf5.c` (`:59`, `:106`, `:210`, `:298`) are `-Wsign-conversion` on lines this branch did not touch |
| 2 | `make check-generated` | 0 | < 1 s | clean |
| 3 | `make check-format` | 0 | 225 s | clang-format, black (235 files unchanged), isort clean |
| 3b | `make check-docs` | 0 | 1 s | links and anchors resolve; no unresolved markers |
| 4 | `make tests-unit summary` | 0 | 33 s | 57 passed, 0 failed; 16 SKIP markers (15 v3-reader tests that need `SIMULATION=mini-millennium-horizontal`, 1 process-isolation case covered elsewhere), all expected under the default build |
| 5 | `make tests-horizontal-v3` | 0 | 20 s | `PASS: tests-horizontal-v3 (4 C tests, 3 Python tests, no unexpected skips)`; `test_chunked_sweep.py` 7/7 PASS (files, comparator identity, row order, compressed append, idle chunks, two-chunk log, failure window); 61 PASS markers, 1 allowlisted SKIP |
| 6 | `make tests-snapshot-global-identity` | 0 | 59 s | PASS, 16 total; one housekeeping WARN (126 cached worktree registrations under `output/snapshot-global-identity/worktrees`, prune suggested; not a failure) |
| 7 | `MPIRUN="mpirun --oversubscribe" make tests-distributed` | 0 | 41 s | `PASS: tests-distributed (260 checks: MPI control test, 4 model(s) at -np 1, 2, 3, 4, 8, chunked legs (halos-only, sage16, hod) at forest_chunks 2, 3, 8 serial and -np 2 x 2, -np 3 x 3, chunked refusal (sham, hod), version 2 refusal; no skips)` |
| 8 | `make clean && make -j` | 0 | 2 s | default build restored |

Steps 5–7 were run twice (the first pass's detailed logs were removed by the step 8 `make clean`); both passes agree. Differential `code-health` against `main` ran with full C and Python coverage (Lizard 1.23.0); its 15 candidates are discussed under [Simplification pass](#simplification-pass) and "No change justified". Not re-run here: the real-data stage on micro-Uchuu (the acceptance record is the evidence; its `G = 1` reference identity was re-verified by the PM at the run's start).

---

## Documentation review

A subagent swept every changed document against the code, the acceptance record and the repository's rules; each finding below was re-verified in this session before inclusion. **Verified correct:** every measured number in the user guide, developer guide, CHANGELOG, skills and pathway traces to the acceptance record (peak RSS and ratios at every `G`, the `-np 2 × 2` ranks, the 6% wall-clock bound, the dataset census, the 1.60% floor, galaxy counts, 18 comparisons, 260/158 checks); every quoted log line, refusal message, YAML key and table formula matches the code; which models and run files are refused is stated correctly; `make check-docs` passes; no committed file outside `docs/dev/` names a `docs/dev/` document; no newly hard-wrapped prose; tables well formed.

Findings (the two material ones are P2-0 and P2-3b above):

- **[P3] Provenance of the 61.86% Shin-Uchuu figure.** Quoted in `docs/USER-GUIDE.md:516`, `docs/DEVELOPER-GUIDE.md:1246`, `CHANGELOG.md:23`, `.agents/skills/mimic-docs-and-writing/SKILL.md:73` and the pathway, but recorded in no acceptance record (`MIMIC-SHIN-UCHUU-V3-PLAN.md` says it is in `MIMIC-DISTRIBUTED-SNAPSHOT-ACCEPTANCE.md`; it is not). The figure pre-dates this branch (`CHANGELOG.md:11` on `main`), so this is inherited, but the branch repeats it in four new places. **Fix:** record the measurement (321,253,424 of 519,342,987 halos in the widest slab) in `simulations/shin-uchuu/README.md` or an acceptance record, and cite that.
- **[P3] `docs/DEVELOPER-GUIDE.md:1246` — "leaves room to chunk nearly linearly" cites only `sage16`'s curve.** The user guide (`:515`, `:527`) and the docs skill carry the `halos-only` caveat (0.291 at `G = 8`); the developer guide does not. **Fix:** add the caveat, as the record does.
- **[P3] Headline condition.** `docs/USER-GUIDE.md:515` ("under `mpirun`, `Distributed horizontal partition: …`") and `docs/DEVELOPER-GUIDE.md:1245` ("`Distributed horizontal partition` under MPI"): the code (`horizontal_driver.c:1009`) picks "Distributed" when `ntask > 1`, so `mpirun -np 1` at `G > 1` logs "Chunked". **Fix:** "with more than one rank".
- **[P3] `docs/DEVELOPER-GUIDE.md:1224`** — "a pure function of (dataset, `NTask`)" now describes tables that also depend on `G`; only the task cuts are `G`-independent. **Fix:** say so.
- **[P3] `docs/DEVELOPER-GUIDE.md:1214`** — "released once its file closes successfully" is unconditional; under `G > 1` only the last visit's close releases the entry (the next sentence says so). **Fix:** "once its file is finalised (at `G = 1`, its close)".
- **[P3] `docs/DEVELOPER-GUIDE.md:1247`** — the proof paragraph says `test_chunked_sweep.py` "covers the serial legs"; it covers `halos-only` only (the gate covers three models). **Fix:** name what it covers.
- **[P3] `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md:429`** — "a second consumer … This is a dependency of one consumer" contradicts itself. **Fix:** "a second Mimic mode depending on forest blocking".
- **[P3] `docs/VISION.md:92`** — "still never on simulation depth or total halo count" appears twice in the one bullet. **Fix:** drop the first.
- **[P3] `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md:8`** — "full Uchuu was not runnable at v1.2 until chunked slab streaming existed" reads as if chunking made it runnable. **Fix:** "was recorded at v1.2 as needing chunked slab streaming".
- **[P3] `.agents/skills/mimic-architecture-contract/SKILL.md:110`** — "the same at every task count" omits "and chunk count" (the next row has it).
- **[P3] Troubleshooting.** `docs/USER-GUIDE.md:787` ("Module-pipeline rejection at startup") and the debugging playbook skill list neither chunk rejection; the playbook also lacks the forest-blocked and version 2 startup refusals, which pre-dates this branch. **Fix:** add rows quoting the two messages.
- **[P3] `test_chunked_sweep.py:202`, `:226`, `:257`, `:303`, `:391`** — docstrings cite plan decision IDs ("C6", "C9"), which become opaque once the plan is archived and are an indirect `docs/dev/` reference. **Fix:** replace with the plain statement each ID stands for.

---

## Open questions

- **Why does `halos-only` chunk more slowly than `sage16`?** At `G = 8` the ratios are 0.291 vs 0.125, and `halos-only`'s `G = 1` peak (4.998 GB) is double `sage16`'s on the same slabs. The accounted terms are identical in shape, so the difference lies in something per-process that the retention accounting excludes (the HDF5 write buffer, output-buffer seeding at `nhalos + max(5%, 1000)` rows per generation, allocator fragmentation across the many short generations of a no-physics run, or reader-side per-`load_slab` file I/O). This is a measurement question for a short follow-up with `scripts/profiling`, not a correctness concern; the record correctly refuses to attribute it.
- **`chunk_log_` under MPI depends on the `task 0:` log prefix.** The gate's headline match prefixes the partition line with the MPI `task 0:` log prefix and passed on every leg, so the prefix is as assumed; if the MPI log prefix format ever changes, this needle fails loudly, which is the desired direction.

---

## Resolution (2026-10-07)

The owner accepted every finding and asked for implementation. What landed, by finding; implementation was delegated to Claude Sonnet 5.5 subagents in three bounded packages (core C; tests and gate; converter and documentation) and every diff was reviewed line by line by the Developer (this session) before acceptance, with two Developer-authored tidy-ups noted below.

| Finding | Resolution |
|---|---|
| P2-0 converter prints the retracted claim | `convert/mimic-convert/runtime_routes.py`, `convert_trees.py`, `report.py` reworded to the "not claimed" position; the four pinned converter tests updated to the new text; `make tests-converter` passes. |
| P2-1 empty first visit untested | `tests/unit/test_hdf5_write_attrs.c`: the two-visit case now writes the multi-visit file for splits 1,300 and 0 and compares each to the one-visit file (bytes, row order, `TotHalosPerSnap`, per-file metadata written once). |
| P2-2 no driver-side snapshot-scope guard | `run_horizontal_driver()` aborts when `chunked && num_post_snapshot > 0`, in the parser's vocabulary, stating the parser was bypassed. |
| P2-3 "task" where a range is meant | `HorizontalGatherContext.task` → `range`; `horizontal_rebase_slab_links()`'s parameter and both aborts, and `horizontal_rebase_link()`'s abort, name the range (the range-bounds abort also spells out `range = task * forest_chunks + chunk`; the reachable cut-forest abort names the range number, which the partition lines logged at startup map to a task and chunk); the five pinned needles in `test_horizontal_distribution.c` updated, none weakened. |
| P2-3b restriction list scope | User guide and run-and-operate skill: the forest-blocked refusal applies to multi-rank and chunked runs; the snapshot-scope rejection is listed as the fourth restriction. |
| P3-4 / S1 two-step const-mutating cut | `horizontal_partition_cut(weights, partition)` replaces the `cut_forests` → `cut_chunks` sequence: task cuts into a tracked scratch array, written at `t·nchunk`, then each task's range sub-cut in place; `cut_chunks` and its one-shot warning are gone; `cut_forests` stays public for the oracle. Developer tidy: the sub-cut loop is a plain `if (nchunk > 1)` block. |
| P3-5 vacuous assertion | Replaced: the first "Loaded snapshot" line after the chunk boundary must carry "this task's chunk 1 of 2". |
| P3-6 helper hygiene | `add_post_snapshot_phase()` unlinks its temp file on failure; `count_occurrences()` replaces the two counting loops. |
| P3-7 Makefile strings | `make help` and the `tests-distributed` comment name the chunked legs and refusal. |
| P3-8 registry `ready` | `horizontal_arm_partition_output_path()` aborts if the registry was not reset. |
| P3-9 row order on MPI × chunk legs | `read_row_order()` keys by snapshot and concatenates an MPI run's task partitions in task order; `chunked_mpi()` emits `row_order_` per pair. Developer tidy: the serial and MPI blocks share one `check_row_order()` helper. The gate is now **266 checks** (260 + 2 pairs × 3 models). |
| P3-10 handle leaks on mid-helper failure | Accepted as-is (shared pattern of the HDF5 unit tests; a failure path only). |
| Documentation P3s | All applied as listed under [Documentation review](#documentation-review); the 61.86% measurement is now recorded in `simulations/shin-uchuu/README.md` and the guides link to it; decision IDs removed from `test_chunked_sweep.py` docstrings (two more than listed were found and replaced). |

The acceptance record keeps its 260-check figure: it is a measurement at the commit it names. The CHANGELOG's Unreleased entry and the pathway state the current count.

**Validation of the implemented tree** (strictly sequential, logs under `archive/test-logs/final-gates-20261007-073909/`): default build and a `USE-MPI=yes` `mini-millennium-horizontal` build with 0 warnings; `make check-generated`, `check-format`, `check-docs`, `validate-modules`; differential lint against `main` (ruff, markdownlint, codespell, clang-format, cppcheck) with no new finding; unit 57/57, integration and scientific tiers PASS with only the expected configuration skips; `make tests-horizontal-v3` PASS (the new test 7/7); `MPIRUN="mpirun --oversubscribe" make tests-distributed` PASS at 266 checks, no skips; `make tests-converter` 1,573 tests OK. `make tests-snapshot-global-identity` refuses an uncommitted runtime tree by design and is rerun after the commit.

### External panel review of the implementation

An independent read-only panel reviewed the implemented tree through the orchestrator (Codex `gpt-6-sol` and Claude Opus 5.5, both high effort, launched in parallel with staggered starts, each embedding the `code-review` skill). **Round 1 (2026-10-07): both PASS WITH RISKS, no P0 or P1, no defect in the C, test or gate changes; eight wording findings, every one verified against the files and fixed:** Codex P2, the profiling write-up claimed "no change to wall-clock" from single runs whose times differed by 1.2 s (claim removed here and in the acceptance addendum); Codex P3, the converter's width note kept an unqualified whole-slab clause (now "at `forest_chunks: 1`"); Codex P3, `HorizontalGatherContext.partition`'s comment still said "NULL if serial" (now "unpartitioned"); Codex P3, one user-guide sentence still scoped the row-order check to serial legs; Claude P3, the two version 2 package READMEs still said "run with one rank" and the new Shin-Uchuu subsection read as if chunking ran on that version 2 dataset and named a source forest id as a `ForestIndex` (all three reworded; a playbook row for the version 2 / not-forest-blocked startup refusal added); Claude P3, the converter's width note and report limitation claimed chunking for every wide output although Consistent-Trees ASCII output cannot be chunked (both qualified by source route); Claude P3, "falls as `1/G`" overstated ratios of 0.148 and 0.146 (now "nearly as `1/G`, 0.15 at `G = 8`" everywhere) and the glibc expectation ignored that `G = 8` chunk buffers straddle glibc's mmap threshold (scoped accordingly); Claude P3, the slab-range abort said "(this task's chunk)" even on an unchunked distributed run (parenthetical dropped, needle updated) and the Resolution table overstated what the cut-forest abort prints (corrected above). **Round 2 (tight, same sessions): Claude PASS, all eight findings fully resolved at every site named, no new defect; Codex PASS WITH RISKS on one P3, the `HorizontalGatherContext` block comment still said "under distribution" and "serial runs" above the corrected field comment (fixed: "partitioned (distributed or chunked)" and "unpartitioned").** The panel closed there; the one remaining item was a two-line comment, and the gates below were rerun on the final tree.

### Profiling the `halos-only` memory gap (open question 1)

**Answer: the gap is not Mimic's. It is freed memory that macOS libc's large-allocation cache keeps mapped and dirty. With that cache disabled, peak RSS tracks the accounted retention for both models and falls nearly as `1/G` (0.15 at `G = 8`).** Measured on this host (macOS 27.0.0, Apple libmalloc) on `micro-uchuu-horizontal` through the shipped run files, single runs, strictly sequential, output bitwise identical to the acceptance runs (comparator PASSED, 4,409,643 galaxies):

| Run | Peak RSS, default malloc | Peak RSS, `MallocLargeCache=0` | Accounted retention (`Retention pool resident`) | Tracked allocator peak | Wall-clock (default / no cache) |
|---|---|---|---|---|---|
| `halos-only` `G = 1` | 4.987 GB | **0.655 GB** | 0.738 GB | 0.738 GB (703.90 MiB) | 7.50 s / 6.35 s |
| `halos-only` `G = 8` | 1.435 GB | **0.097 GB** | 0.093 GB | 0.095 GB | 6.53 s / 6.78 s |
| `sage16` `G = 1` | 2.471 GB | **0.698 GB** | 0.810 GB | 0.811 GB | 40.84 s / 41.61 s |
| `sage16` `G = 8` | 0.316 GB | **0.102 GB** | 0.099 GB | 0.101 GB | 40.43 s / 39.04 s |

With the cache disabled the `G = 8 / G = 1` ratio is 0.148 for `halos-only` and 0.146 for `sage16`, against the 0.125 a perfect split would give (the remainder is the fixed per-process cost and the largest-forest floor). Peak RSS sits within 15% of the accounted figure in every run (slightly below it at `G = 1`, because the accounting counts seeded capacities and only touched pages are resident).

**How it was established.** (1) Mimic's run memory profile and tracked allocator agree with each other (tracked peak 703.90 MiB = 0.738 GB = the accounted retention) and are far below RSS. (2) An A/B run with one output snapshot instead of eight gave the same 4.9 GB peak and the same monotonic climb, so the output path is not involved. (3) A temporary in-process probe (removed again; nothing committed) sampled Mach `task_info` and the process's VM regions after each snapshot: the growth is anonymous, not file-backed, not marked reusable, and lives entirely in regions tagged `VM_MEMORY_MALLOC_LARGE` (5.98 GB mapped at the end of the `halos-only` run) while every malloc zone reported at most 0.42 GB in use, so the regions hold freed blocks the allocator kept. (4) `MallocLargeCache=0` (and equally `MallocSpaceEfficient=1`) removes the whole gap with the output bitwise identical; the wall-clock differences in the table (at most 1.2 s on `halos-only`, under 2 s on `sage16`) are single-run spread and support no timing conclusion either way. `halos-only` shows it most because it frees and reallocates a different-sized generation buffer every ~0.1 s for 50 snapshots; `sage16` sees the same cache partly drained after its widest generations are released.

**Consequences.**
- The acceptance record's peak-RSS table is a correct measurement of RSS on macOS, but its ratios describe the allocator cache as much as Mimic's working set; the record now carries a dated addendum saying so, and the guides state the attribution. The accounted retention and the `MallocLargeCache=0` RSS are the figures to reason from when choosing `G`.
- On Linux (glibc), the production target, blocks above the dynamic mmap threshold (which rises with freed blocks up to 32 MiB) are returned to the kernel on `free`. At `G = 1` every generation buffer here is 80 to 270 MB, well above it, so the cache effect is not expected; at `G = 8` the chunk buffers are roughly an eighth of that and straddle the threshold, so some retention may return at exactly the `G` values users choose. Both statements are expectations from glibc's documented behaviour, not measurements, and should be confirmed the first time a chunked run is timed on a Linux host.
- No code change is warranted in this branch. The retention design already reuses galaxy pools across generations; reusing slab and output buffers the same way would remove the churn that feeds the cache, but it is a design change with its own risks and is recorded as a candidate, not scheduled.
- Operationally on macOS, run with `MallocLargeCache=0` when peak RSS is what you are measuring or budgeting against.

---

## Recommended actions

Ordered; items 1–6 belong in one follow-up commit on this branch before merge, items 7–9 are post-merge.

1. **P2-0** Reword the converter's `FULL_UCHUU_NOT_RUNNABLE` constant, the `convert_trees.py` width note and `report.py`'s limitation to the README's C12 wording; update the four pinned assertions in `convert/mimic-convert/tests/`; run `make tests-converter`.
2. **P2-3** Rename `task` → `range` in `HorizontalGatherContext`, `horizontal_rebase_slab_links()`'s prototype and its two aborts; update the two pinned needles in `test_horizontal_distribution.c`.
3. **P2-2** Add the driver-side `FATAL_ERROR` for `chunked && num_post_snapshot > 0` beside the `chunked` computation in `run_horizontal_driver()`.
4. **P2-1** Add the `split = 0` empty-first-visit case to `test_hdf5_write_attrs.c`.
5. **P2-3b and the documentation P3s** Fix the user guide's and skill's restriction scope; apply the wording items under [Documentation review](#documentation-review) (each one sentence); record the 61.86% measurement in a committed place and cite it.
6. **P3-5, P3-6, P3-7, P3-8** Remove the vacuous assertion; `unlink` the `.tmp` and add `count_occurrences()`; refresh the two Makefile strings; assert `ready` at arm time.
7. **S1 / P3-4** Collapse the two-step cut into `horizontal_partition_cut()`; drop the `const`; keep `cut_forests` public for the oracle.
8. **P3-9** Extend `row_order_` to the MPI × chunk legs by concatenating task partitions in task order.
9. **Open question 1** One profiling session on `halos-only` at `G = 1` and `G = 8` on micro-Uchuu to attribute the un-accounted resident memory; record the result beside the acceptance record.

After the follow-up commit: `make clean && make`, `make check-generated`, `make check-format`, `make check-docs`, `make tests-converter`, `make tests-unit` (subagent), `make tests-horizontal-v3`, `MPIRUN="mpirun --oversubscribe" make tests-distributed` (expect 260 checks, or more if item 8 is included), then the `mimic-change-control` pre-commit gate, including a skill-staleness sweep for the documentation items.
