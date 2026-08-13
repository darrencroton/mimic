# Snapshot-Run Output Partitioning (D5(a)) — Implementation Plan

**Status:** Frozen (Revision 5, amended mid-execution — see the amendment record at the end). Slices 1–2 executed and accepted; Slices 3–4 outstanding.
**Date:** 2026-08-13
**Owns:** Pre-Shin-Uchuu checklist item 4 (`POST-PHASE-5-JOINT-REVIEW.md` §6.4), the decided-but-unbuilt D5(a).
**Scope:** One HDF5 output partition file per requested output snapshot for snapshot-ordered runs. No size knob, no new configuration surface, no change to tree-ordered behaviour.

---

## Why this exists, and why now

A `sage16` micro-Uchuu run writes five `model_NNN.hdf5` partitions of ~130–200 MB on the tree-ordered side (`forests_per_file` chunking) but a single 823 MB `model_000.hdf5` on the snapshot-ordered side — the Phase 5 Output Contract's deliberate single-partition design. Shin-Uchuu's z=0 slab alone is ~100× micro-Uchuu's entire output, so a production run would produce one file in the hundreds-of-GB-to-TB range: operationally unmanageable, and the worst possible blast radius for a deferred-write failure on a one-shot, no-resume run whose output layout is frozen the day it launches.

D5(a)'s own rationale is that this is the snapshot-major analogue of the tree driver's per-forest chunking — the existing mechanism applied to the other driver, not new machinery. (Vision Principle 5 governs *memory* working sets rather than output topology, so it motivates this change by analogy rather than mandating it; the mandate is the owner's D5(a) decision.)

It must land **before** the Shin-Uchuu subset rehearsal (checklist item 6), because the rehearsal certifies an output contract that has to be final (D9).

**Owner decisions taken 2026-08-13, before this plan was frozen:**

1. **Partition output id = the snapshot number.** Requesting `[49, 1]` produces `model_049.hdf5` and `model_001.hdf5`. `output.snapshot_list` is validated for range and uniqueness but never sorted (`src/core/read_parameter_file.c:1525-1535`), so dense index numbering would put `Snap049` in `model_000.hdf5` and `Snap001` in `model_001.hdf5` — filenames that tell an operator nothing. Snapshot-number ids make every filename self-describing.
2. **Per-partition cleanup disarm.** A partition's cleanup registration is released once its file closes successfully, exactly as the tree driver releases its own on the success path (`src/core/tree_driver.c:309`; note `:295` is the different `--skip` branch). A failure removes only the in-flight partition and the master; completed snapshot files survive. This **supersedes** the Phase 5 "all-or-nothing" cleanup contract, which was written when a snapshot run produced exactly one file and "partial" meant "one truncated file". With per-snapshot files a closed file is never partial, and destroying weeks of finished output on a late failure is the larger hazard.
3. **No `hdf5_format_version` bump.** The rule at `src/io/output/metadata_hdf5.c:135` ties the version to the output *schema* — the groups, datasets, attributes, and dtypes **within a file** — and none of those change. What does change is the run-level *file topology*: a snapshot-run partition now carries one `Snap%03d` group instead of all of them. That distinction is the whole basis of the decision, and the documents of record must state it plainly rather than implying nothing changed. Bumping would stamp tree-ordered output too, adding a fifth metadata delta beyond the four the Phase 5 record certifies, and would churn `tests/framework/data_loader.py:354`'s default plus the tracked HDF5 baselines.

---

## Repository state at freeze

Clean working tree, branch `feature/ctrees-snapshot-reader`, HEAD `99b99cff` ("Close post-Phase-5 checklist item 1, record decisions D5-D10, fix the topology harness"). No relevant file is dirty. Every line reference below was read at that commit and re-verified after the panel review.

---

## What the code does today (verified, not assumed)

| Piece | Location | State |
|---|---|---|
| `struct OutputPartitionSource` | `src/io/output/util.h:71-83` | Driver-neutral for *enumerating* partitions; carries **no** partition→snapshot mapping |
| Snapshot partition source | `src/core/tree_driver.c:570-593` | Hard-coded: count 1, output id 0, always exists |
| Output-file creation | `src/io/output/hdf5.c:78-118` (`prep_hdf5_file`) | Unconditionally creates **all** `MimicConfig.NOUT` groups and tables |
| Per-file `RunProperties` | `src/io/output/hdf5.c:204-208` | Written when `n == 0`, i.e. only for the partition owning `ListOutputSnaps[0]`. Under partitioning every other partition file would be left without its provenance record |
| Record buffering | `src/io/output/hdf5.c:336-360` (`save_halos_hdf5`) | Loops all `NOUT` indices per call |
| Flush / finalize | `src/io/output/hdf5.c:326-334`; `src/core/snapshot_driver.c:537-558` | One flush, at run finalization |
| Master links | `src/io/output/master_hdf5.c:99-197` | Cartesian: every partition × every requested snapshot |
| Snapshot cleanup registry | `src/core/snapshot_driver.c:90-137` | Two fixed slots registered in one call; disarmed only after `write_master_file()` (`src/core/main.c:437-441`) |
| Tree per-partition entry points | `src/core/tree_driver.c:199` (`process_partition`), `:289` (`claim_and_process_partition`) | Both take `output_id` only; the loop's partition index is discarded before the writers are reached |

**Three consumers need no change**, which is why this slice is small:

- `scripts/compare_cross_format_identity.py:161-203` enumerates every partition and scans whichever `Snap###` groups each one holds, aggregating per snapshot.
- The identity gate's `recorded_records()` (`simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py:1057-1081`) sums `TotHalosPerSnap` over whatever `File###` groups exist under each `Snap###` group.
- `plot/mimic-plot/hdf5_reader.py:50-55` iterates the actual `File*` subgroups rather than assuming `File000`, and `plot/mimic-plot/mimic-plot.py:945-957` enumerates whatever `model_*.hdf5` partitions exist, tolerating a snapshot being absent from a given partition.

**Constraints already enforced elsewhere**, so this plan need not re-handle them: snapshot-ordered runs reject `output_format: binary` (`src/core/read_parameter_file.c:1453`), reject `--skip` (`:1457`), and reject `NTask > 1` (`:1461`). `MimicConfig.NOUT ≥ 1` always (`validate_output_snapshots()`, `:1511-1518`). `calc_hdf5_props()` runs at `src/core/main.c:414`, before either driver, so the HDF5 field tables exist before any partition file is opened. `snapshot_h5_validate_links()` is called only from `load_slab_snapshot_hdf5()` (`src/io/snapshot/read_snapshot_hdf5.c:1297`), never from `open_run` — which is what makes the deterministic fault injection in Slice 2 possible.

---

## Frozen design

**One partition = one requested output snapshot.** Each partition file holds only its own `Snap%03d` group. It is created when that snapshot finishes processing, written, stamped, and closed immediately, so a finished snapshot file is final the moment it closes and the driver never holds more than one writable output file open. (Master aggregation, which runs afterwards in `main.c`, separately holds the master plus one read-only partition handle — `src/io/output/master_hdf5.c:47`, `:113`.)

The seam gains one hook and one small type:

```c
/* src/io/output/util.h */
struct OutputSnapshotSelection {
  int count;           /* requested output snapshots this partition carries */
  const int *indices;  /* indices into MimicConfig.ListOutputSnaps, ascending */
};

/* new member of struct OutputPartitionSource */
struct OutputSnapshotSelection (*partition_snapshots)(int partition);
```

A tree-ordered partition returns every requested snapshot (`count == NOUT`), which is what it already writes; a snapshot-ordered partition returns exactly one. Both are served by a single file-static ascending index table in `src/core/tree_driver.c`, filled in `get_output_partition_source()`.

Two consequences are named here so they are not mistaken for scope creep at audit:

- The `NOUT`-way loop in `save_halos_hdf5()` collapses to one iteration on the snapshot path — roughly 50× fewer passes over a ~100M-record buffer at Shin-Uchuu scale. This falls out of honouring the selection. It is **not** the F-3 writer-loop inversion (D3, deliberately deferred), which is a different change to the shared marshaller and is out of scope.
- Peak memory drops slightly: one live HDF5 write buffer instead of up to `NOUT` lazily allocated ones, and one live file's metadata cache instead of one held open for the whole run. This helps still-open checklist item 3 rather than perturbing it.

### Why size-targeted splitting is not here

D5(b) — splitting one snapshot's records across several size-targeted files — is deliberately excluded, and this section records why so it is not rediscovered.

The measurement that decides it arrives one step later: checklist item 6, the subset rehearsal, measures the real z=0 output population, and D5(b)'s own stated trigger is that (a)'s z=0 file "proves genuinely unworkable" (estimated ~80 GB). Building a size knob now would size it against a projection when the measurement is the very next item.

Splitting also buys less than it appears to. It does **not** reduce peak memory: the driver marshals a snapshot's entire processed buffer before any byte is written, so the in-memory ceiling is identical however many files that buffer lands in. That ceiling is checklist item 3's business. What splitting buys is file-handling convenience and a smaller corruption blast radius — and the second is arguable, since a cleanly-lost snapshot is easier to reason about than a snapshot missing one unidentified fraction of its galaxies.

The seam frozen here is already the right shape for it: a split-snapshot world is several partitions that each return the same single-snapshot selection, and the master file, the comparator, and the plot reader already aggregate many `File###` groups per `Snap###` group. What D5(b) would additionally have to build, beyond the size knob itself: the snapshot-number naming would need a sub-id or a running counter (decision 1), and — the one piece that is real work rather than convention — record-range routing in the writer, because `save_halos_hdf5()` scans the whole lent buffer (`src/io/output/hdf5.c:336-359`) and calling it once per same-snapshot partition would otherwise write every record into every file. Nothing in this plan makes that follow-up harder, but it is not free.

**Observed and deliberately out of scope:** `write_run_metadata()` runs at `src/core/main.c:479`, *after* the driver, so a run that dies at week three leaves no `metadata/` directory at all. Moving it earlier would improve provenance for crashed runs and would also prove output-directory writability, but it changes the run sequence for both drivers and belongs to its own change. Recorded here so it is not lost.

---

## Implementation Profiles

- **Recommended project-manager seat: `--model opus --effort high`.** Slice 2 changes the output contract of the production run and revises a frozen cleanup semantic; the supervisor has to judge identity-gate evidence and tree-path preservation evidence on their merits, and to recognise a vacuous pass (the class of defect commit `486ed505` had to close in this very gate). That is not a mechanical accept/reject.
- **Recommended for a frontier/senior implementer:** run the three slices individually. They are already the natural seams, and Slice 1's whole value is that it is provably inert on its own evidence — batching it with Slice 2 destroys that property.
- **Recommended for a standard implementer:** the same, one slice at a time.
- **No slice batches are defined.** Slice 1 and Slice 2 must not share a review: Slice 1's acceptance is "zero observable change anywhere", and Slice 2's is "exactly this observable change". Merging them means neither can be checked. (One panel reviewer recommended merging them; see the panel record below for why that was declined.)

Per-slice developer seats are stated in each receipt.

---

## Slice 1: Partition→snapshot selection through the output seam

**Developer seat:** `--model sonnet --effort high`. The contract is fully specified and the change is mechanical signature threading; its risk is concentrated in "did the tree path move?", which the evidence answers mechanically rather than by judgement.

### Intended Change

- Add `struct OutputSnapshotSelection` and the `partition_snapshots` hook to `struct OutputPartitionSource` in `src/io/output/util.h`, and update that struct's doc comment so it no longer describes the snapshot side as a fixed single-partition shape.
- In `src/core/tree_driver.c`, add a file-static ascending index table (`0..NOUT-1`) filled in `get_output_partition_source()`, and supply `partition_snapshots` from both constructors. The tree constructor returns the whole table (`count == MimicConfig.NOUT`); the snapshot constructor **still returns the whole table** in this slice, so its single partition keeps carrying every requested snapshot.
- Thread the selection explicitly (no ambient state) through `prepare_output_files()`, `open_hdf5_output_file()`, `prep_hdf5_file()`, `save_halos_hdf5()`, and `flush_hdf5_buffers()`, so each iterates the selection rather than `0..NOUT-1`.
- Propagate the partition's selection to those writer calls in the tree driver. The writers are reached from `process_partition()` (`src/core/tree_driver.c:199`), which is called via `claim_and_process_partition()` (`:289`); neither receives the partition index today, so both take the selection as a new parameter, resolved once per partition at the two enumeration loops that already hold the index. Do **not** reuse `output_id` as a partition index: they are not the same value for either driver after Slice 2.
- On the binary branch of `prepare_output_files()`, add a one-line comment recording that the selection is not consulted because binary output is tree-ordered-only (rejected at configuration time for snapshot runs) and therefore always carries every requested snapshot. No runtime guard: the mismatch is unreachable under the frozen configuration validation, and defensive scaffolding for an unreachable state is against house convention.
- Move the per-file `RunProperties` write out of `write_hdf5_attrs()`'s `n == 0` branch and into `open_hdf5_output_file()`, immediately after the file is opened for writing. This deletes a conditional rather than adding state, and makes "per-file metadata" a property of opening a file rather than of a snapshot index. It is complete because `open_hdf5_output_file()` (`src/io/output/hdf5.c:134`) is the only site in the codebase that assigns `HDF5_current_file_id`, and safe because `calc_hdf5_props()` runs before either driver (`src/core/main.c:414`) and the metadata does not depend on finalized galaxy counts (`src/io/output/metadata_hdf5.c:492-517`).
- Adapt `write_master_file()`'s inner loop (`src/io/output/master_hdf5.c:120-191`) to iterate the partition's own selection. The outer `Snap%03d` group-creation loop (`:64-97`) is unchanged.

### Acceptance Criteria

- Inputs: unchanged run YAMLs; no new configuration keys, Make variables, or CLI flags.
- Outputs: tree-ordered binary output byte-identical; tree-ordered HDF5 output identical in schema, data, and metadata; snapshot-ordered output unchanged in file count, names, and content.
- User-visible behaviour: none. This slice is inert by construction.
- Behaviour that must not change: everything.

- [ ] `struct OutputPartitionSource` carries `partition_snapshots`, and every construction site in `src/core/tree_driver.c` supplies it; no *partition-local* HDF5 writer loop among the functions threaded above computes its snapshot range from `MimicConfig.NOUT` directly any more. The master file's run-global outer group-creation loop (required to stay over all `NOUT` by the criterion below) and the binary writers (excluded by this slice's non-goals) are the contract's own stated exceptions.
- [ ] `prep_hdf5_file()` creates a `Snap%03d` group and `Galaxies` table for exactly the selection's indices, in the order given.
- [ ] `save_halos_hdf5()` and `flush_hdf5_buffers()` iterate exactly the selection's indices; no other index is buffered, flushed, or freed by a call.
- [ ] Per-file `RunProperties` is written exactly once per output file, at open, on **every** path that opens an output file — and `write_hdf5_attrs()` no longer writes it under any condition.
- [ ] `write_master_file()` links, for each partition, exactly the snapshots in that partition's selection, and creates the `Snap%03d` groups themselves for all `MimicConfig.NOUT` requested snapshots regardless of partitioning.
- [ ] A default-pair (`sage16`/`mini-millennium`) **binary** run's galaxy records are byte-identical to the pre-slice run.
- [ ] A default-pair **HDF5** run's partition file and master file show **zero** `h5dump -A` deltas against the pre-slice run, other than the five always-excluded provenance attributes (`git_commit`, `git_branch`, `git_date`, `build_date`, `RunEndTime`), and their `Galaxies` datasets are record-for-record identical. Raw HDF5 file bytes are deliberately **not** asserted: moving the metadata write to file-open changes object allocation order inside the file without changing any stored value, and the project's preservation standard has always been record and metadata identity, not container bytes.
- [ ] A snapshot-ordered fixture run still writes exactly one partition file named `model_000.hdf5` plus the master, with unchanged group contents.
- [ ] `make USE-HDF5=no` builds and links clean.

### Authorized Surface

- Files allowed to change:
  - `src/io/output/util.h`
  - `src/io/output/util.c`
  - `src/io/output/hdf5.h`
  - `src/io/output/hdf5.c`
  - `src/io/output/master_hdf5.c`
  - `src/core/tree_driver.c`
  - `src/core/snapshot_driver.c`
  - `tests/unit/test_hdf5_write_attrs.c`
  - `tests/unit/test_master_hdf5_partitions.c`
- Functions/classes/components allowed to change: `struct OutputPartitionSource` and its doc comment, the new `struct OutputSnapshotSelection`, `get_output_partition_source()` and both partition-source constructors, `process_partition()` and `claim_and_process_partition()` (signature and call sites only), the tree driver's two partition enumeration loops (`run_per_file_driver`, `run_enumerated_driver`) at the point they call `claim_and_process_partition()`, `prepare_output_files()`, `prep_hdf5_file()`, `open_hdf5_output_file()`, `save_halos_hdf5()`, `flush_hdf5_buffers()`, `write_hdf5_attrs()` (removal of its per-file-metadata branch only), `write_master_file()`, and the snapshot driver's existing calls into those writers.
- Tests allowed or expected to change: `tests/unit/test_hdf5_write_attrs.c:132-138`, whose assertion pins a trigger this slice moves — it must become an assertion that `write_hdf5_attrs()` writes per-file metadata zero times. That proves removal only; **relocation** is proved by the existing end-to-end assertion that a partition file carries `RunProperties` (`tests/integration/test_processing_order.py:310-312`) and by Slice 2's non-zero-index criterion, so no new unit test is added for it. `tests/unit/test_master_hdf5_partitions.c` changes only as far as supplying the new hook to its injected sources.

### Explicit Non-Goals

- No change to the number, names, or contents of any output file on either path.
- No change to the snapshot driver's output lifecycle (still one file, opened at run start, closed at run end).
- No `hdf5_format_version` change.
- No threading of the selection through the binary writers (`create_binary_output_files`, `save_halos`, `finalize_halo_file`), and no runtime guard on the binary branch.
- No F-3 writer-loop inversion, no `TotHalosPerSnap` changes, no documentation-of-record edits (Slice 3 owns those).

### Risk Flags

- Risky surfaces touched: shared output writers on the Phase-5-certified tree path; the master-file layout; generated-metadata write ordering inside HDF5 files.
- Approval needed before implementation: no
- Independent audit required: no

  *(Both panel reviewers judged an independent audit disproportionate here: this slice's acceptance is "zero observable change", and its evidence — the bitwise binary comparison and the `h5dump -A` diffs — is mechanical rather than a matter of reviewer judgement. The standard drift-audit and code-review gates still run.)*

### Validation Plan

- Tests to add/update: update `tests/unit/test_hdf5_write_attrs.c`'s per-file-metadata expectation as described. **No new unit test file** — selective group creation is not exercisable here (this slice's selection is always the full set) and is proved end to end by Slice 2's integration test.
- Commands to run:
  - `make check-generated`, `make validate-modules`, `make check-format` (all must exit 0)
  - `make -j$(sysctl -n hw.ncpu)` for the default pair
  - `make tests-unit`, `make tests-integration`, `make tests-scientific` (delegate the long tiers to a subagent that returns a pass/fail summary)
  - `make clean && make USE-HDF5=no -j$(sysctl -n hw.ncpu)`, then restore the default build
  - Tree-path preservation, using the vehicle Phase 4b froze and Phase 5 reused: the generated `build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml` for the bitwise binary comparison, and an `output_format: hdf5` copy of it for the `h5dump -A` and record-level comparisons, run before and after the slice from separate worktrees
- Lint (differential, via the `lint` skill): required
- Manual checks: read the master-file diff for the tree branch and confirm the emitted link set is identical, not merely equivalent.

### Rollback Path

Revert the single slice commit. No generated files, baselines, schema versions, or on-disk formats are touched, so nothing else unwinds.

---

## Slice 2: One partition file per requested output snapshot

**Developer seat:** `--model opus --effort high`. This slice rewrites the snapshot driver's output lifecycle, changes file-unlink semantics, and is certified by a multi-hour identity gate whose failure modes include silently vacuous passes. It needs judgement, not just contract-following.

### Intended Change

- Flip the snapshot partition source in `src/core/tree_driver.c` to the partitioned shape: `num_partitions()` returns `MimicConfig.NOUT`; `partition_output_id(p)` returns `MimicConfig.ListOutputSnaps[p]`; `partition_exists(p)` stays `1`; `partition_snapshots(p)` returns the single index `p`.
- Restructure `src/core/snapshot_driver.c`'s output path:
  - `snapshot_is_output_snapshot()` becomes an index lookup returning the `ListOutputSnaps` index or `-1`.
  - `snapshot_open_output()` no longer creates an output file: it zeroes `TotHalosPerSnap[0..NOUT-1]` and arms the master path for cleanup.
  - A new per-snapshot output function performs, for one requested snapshot: set `FileNum`, arm the in-flight partition path, `prepare_output_files()`, lend the `ProcessedHalos` globals for the `save_halos_hdf5()` call and clear them again, `flush_hdf5_buffers()`, `write_hdf5_attrs()`, close the file with its `H5Fclose` status checked (the F-14 pattern, preserved verbatim), then disarm the in-flight partition path.
  - `snapshot_finalize_output()` and the `SNAPSHOT_OUTPUT_ID` constant retire.
  - The `#else /* !HDF5 */` stubs are adjusted to match the new function set and keep failing loudly.
- **Cleanup registry, pinned rather than left to the implementer.** The two-slot array stays; what changes is that the slots acquire independent lifetimes, which the existing single `snapshot_register_output_paths()` call cannot express. Replace it with three file-static helpers and nothing more: one that arms the master path at run start (slot 1), one that arms the in-flight partition path for a given output id (slot 0), and one that clears slot 0 after a successful close. `snapshot_output_path_count` stops being a running count and becomes the fixed extent of the array, with `snapshot_driver_remove_incomplete_outputs()` continuing to skip empty slots — it already does (`src/core/snapshot_driver.c:104-110`). The two exported functions keep their names, signatures, and call sites in `src/core/main.c`; only `snapshot_driver_clear_output_paths()`'s meaning widens from "disarm both" to "disarm whatever remains", which is what `main.c:441` already wants.
- **Output-directory writability probe**, at the top of `run_snapshot_driver()` **before** the `snapshot_reader_open_run()` call (`src/core/snapshot_driver.c:690`), rather than inside `snapshot_open_output()` (defined at `:492`, called at `:707`): `open_run` validates the whole dataset first and is not instant at production scale, so a probe behind it is not the early failure this is for. `src/core/main.c:393` proves the output directory can be *created*, not written to; until this slice, the driver's up-front `prepare_output_files()` caught an unwritable directory immediately, and afterwards the first partition file appears only when its snapshot completes — which for a z=0-only request is at the end of a multi-week run. Create, write one byte to, close, and unlink a collision-safe probe file under `MimicConfig.OutputDir`, checking every status and failing fast. This restores a property the slice would otherwise remove; it is not new capability.
- Update the run banner from `"→ 1 output file"` to the actual partition count.
- Tighten the identity gate: `simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py:1149-1160` currently only *logs* the snapshot-side partition count. Assert it equals the number of requested output snapshots, so the new contract is proved rather than observed. The existing `tree_partitions < 2` guard stays.
- Update the two stale in-source contract comments the new behaviour invalidates: `src/io/output/util.h`'s description of the snapshot partition source, and `src/include/proto.h:52-62`, whose cleanup comment promises "the single partition file" and that a failure "leaves no output behind".

### Acceptance Criteria

- Inputs: unchanged run YAMLs; no new configuration keys, Make variables, or CLI flags. Requested snapshots come from `output.snapshot_list`, which is already validated for range and uniqueness and may be unsorted; behaviour is defined for any list that validation admits.
- Outputs: a snapshot-ordered run writes one `<basename>_<snapnum>.hdf5` per requested output snapshot, plus the master `<basename>.hdf5`.
- User-visible behaviour: snapshot-run output file count and names change as above; tree-ordered runs are unaffected.
- Behaviour that must not change: every galaxy record on both paths; the tree-ordered output layout, naming, and master structure; the per-snapshot HDF5 schema (no `Ntrees`, no `TreeHalosPerSnap`, `TotHalosPerSnap` as `int64`); `hdf5_format_version` at `1.2`.

- [ ] A snapshot-ordered run writes exactly one partition file per requested output snapshot, named by that snapshot's number, plus one master file — no other output files.
- [ ] Each partition file contains exactly one `Snap%03d` group: its own. No empty groups for other snapshots exist in it.
- [ ] Each partition file's `Snap%03d/Galaxies` carries a `TotHalosPerSnap` attribute equal to its own row count, no `Ntrees` attribute, and no `TreeHalosPerSnap` dataset, and the file carries `RunProperties` including `UniqueGalaxyIDMultiplier` — including for a partition whose requested-snapshot index is not `0`, which is what proves the Slice 1 metadata relocation.
- [ ] The master file creates one `Snap%03d` group per requested output snapshot, each holding exactly one `File%03d` subgroup — named for that snapshot — whose `Galaxies` is an external link into that snapshot's own partition file, with a `TotHalosPerSnap` attribute matching it and no `TreeHalosPerSnap` link.
- [ ] This holds for an **unsorted** `output.snapshot_list`: each file's name matches the snapshot it contains, on every path.
- [ ] A requested output snapshot with zero galaxies still produces its partition file, its empty `Galaxies` table, and its master link.
- [ ] The driver holds at most one writable output file open at a time; each partition file is closed before the next is created.
- [ ] After a failure that occurs once at least one partition file has closed, every closed partition file survives and no master file exists.
- [ ] After a failure that occurs once the in-flight partition path has been armed, that path is removed — the two halves of the registry contract are checked by separate injections, because a failure during slab loading happens before the partition file exists and cannot exercise removal.
- [ ] An unwritable output directory fails the run before the dataset is opened, not at the first requested output snapshot.
- [ ] The tree-ordered path is unchanged: byte-identical binary galaxy records, and zero `h5dump -A` deltas beyond the five always-excluded provenance attributes.
- [ ] The cross-format identity gate passes 8/8 stages on the real micro-Uchuu dataset — both models × both timestep schemes — with the comparator reporting a non-zero record count that matches what the runs' own master files record.
- [ ] No in-source comment still describes the snapshot side as producing a single output partition, or its cleanup as all-or-nothing. **Scope of this criterion:** it binds the files in this slice's authorized surface. `scripts/compare_cross_format_identity.py:213` also contains the phrase "a single-partition snapshot-ordered run", but its technical claim survives this change — one snapshot's records still land in one partition file, which is what its memory argument concerns — so it is out of scope here and needs no edit.
- [ ] No test in the mandatory validation commands derives a snapshot-run output filename from a fixed partition index. Every such filename is derived from the run file's `output.snapshot_list`.
- [ ] `make USE-HDF5=no` builds and links clean.

### Authorized Surface

- Files allowed to change:
  - `src/core/tree_driver.c`
  - `src/core/snapshot_driver.c`
  - `src/io/output/util.h`
  - `src/include/proto.h`
  - `tests/unit/test_master_hdf5_partitions.c`
  - `tests/integration/test_processing_order.py`
  - `simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py`
  - `tests/scientific/test_scientific.py`
- Functions/classes/components allowed to change: the snapshot partition-source constructor and its four hooks in `src/core/tree_driver.c`; in `src/core/snapshot_driver.c`, the output helpers (`snapshot_open_output`, `snapshot_write_output`, `snapshot_finalize_output` and their non-HDF5 stubs), the cleanup-registry helpers, `snapshot_is_output_snapshot`, and the output-related statements inside `run_snapshot_driver()`; comment text only in `src/io/output/util.h` and `src/include/proto.h`.
- Tests allowed or expected to change: `tests/unit/test_master_hdf5_partitions.c`'s `test_snapshot_output_partition_source_is_trivial_single_partition` (`:381-403`, `:465`), which pins the shape this slice replaces, plus a new master-file case proving the snapshot branch links each snapshot to its own file; `tests/integration/test_processing_order.py` at `:288-291` (the single-partition file-set assertion), `:295-312` (the per-file content loop, which currently reads every requested snapshot from `partitions[0]` and must fan out across partition files; its `RunProperties` assertion is at `:310-312`), and `:314-326` (the `File000` master assertions); the gate's partition-count assertion; and `tests/scientific/test_scientific.py`'s `regenerate_output()` (`:132-140`), whose snapshot-ordered branch hard-codes `tests/data/output/hdf5/model_000.hdf5` and carries the comment "filenr 0: a snapshot-ordered run writes a single output partition". Derive that filename from the run file's `output.snapshot_list` — `simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py:857-869` already has the helper pattern — and retire the comment. This file is unavoidably inside the mandatory identity-gate command: `scripts/generate_test_registry.py`'s `core_scientific_tests()` globs **every** `tests/scientific/test_*.py` into the scientific tier for **every** simulation, and `tests/scientific/` contains only this file. The generated snapshot-pair vehicle requests `snapshot_list: [49]` (`scripts/generate_test_inputs.py:211`, `snapshot_list=[last_snap]`, against a 50-entry `a_list`), so this slice renames the file it reads to `model_049.hdf5`. Do **not** change the tree-side branch of that function.

### Explicit Non-Goals

- No `--skip` or resume support for snapshot runs, and no relaxation of the existing `--skip`, binary-output, or `NTask > 1` rejections.
- No size-targeted splitting within a snapshot (D5(b)); no `target_file_size_mb` analogue; no new knob of any kind.
- No changes to the tree driver's partitioning, claim/skip logic, or output naming.
- No `hdf5_format_version` bump, no `TotHalosPerSnap` changes, no F-3 writer-loop inversion, no move of `write_run_metadata()`.
- No documentation-of-record edits — Slice 3 owns those, and this slice is not complete work until Slice 3 lands.
- No new unit test for selective group creation: the integration test proves it against a real run, and a unit-level duplicate would test the same statement twice.
- No new committed corrupt fixture: the failure-injection check below builds its corrupted copy in a temporary directory at test time.
- No change to `tests/integration/test_output_formats.py`, which carries the same hard-coded-filename defect at three live sites. It is unreachable from this slice's mandatory commands (they run the integration tier only on the tree-ordered default pair), and Slice 4 owns it with its own gate. Do not fix it here, where nothing this slice runs would verify the fix.
- No relocation of the `FileNum` assignment. **Recorded consequence, not a defect:** `FileNum` has exactly one read site in physics — `models/sham/modules/sham_assign_stellar_mass/sham_assign_stellar_mass.c:86`, an RNG-seed fallback that fires only when `halo->UniqueGalaxyID == 0`. Today a snapshot run holds `FileNum == 0` throughout; afterwards it is re-pointed per partition. This is unreachable in the current repository — `sham` has run files for `millennium` and `mini-millennium` only, both tree-ordered, and the identity gate's `MODELS` are `halos-only` and `sage16` — so it becomes live only if someone later writes a `sham` run file for a snapshot-ordered package. Implement the assignment where this contract pins it and do not work around this.

### Risk Flags

- Risky surfaces touched: the production output contract for a one-shot 5.6 TB run; file-unlink semantics on failure; the snapshot driver's output lifecycle; the certified cross-format identity gate.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Tests to add/update:
  - `tests/integration/test_processing_order.py`: extend the existing requested list (`:241`, already deliberately unsorted at `[nsnapshots - 1, 1]`) to include snapshot `0`, which the fixture documents as empty (`simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py:164-167`) — that one change exercises both the unsorted-naming and the zero-galaxy criteria. Fan the per-file assertions out across partition files, and add the master's per-snapshot link-target assertions.
  - `tests/integration/test_processing_order.py`, retention case: copy the fixture to a temporary directory and set `FirstHaloInFOFgroup[0] = n_halos` in a **later populated** snapshot's `snapshot_NNN.h5`. That field is bounded by its own snapshot's halo count (`src/io/snapshot/read_snapshot_hdf5.c:849`, `:884-886`), so the value is guaranteed out of range, and `snapshot_h5_validate_links()` is reached only from `load_slab_snapshot_hdf5()` (`:1297`) — never from `open_run` — so the abort lands mid-sweep rather than at startup. Request output snapshots before and after it, and assert the run exits non-zero with the earlier partition files present and no master.
  - `tests/integration/test_processing_order.py`, in-flight removal case: pre-create the target path of a **later** requested output snapshot (`model_<snap>.hdf5`) as a read-only regular file containing a marker byte. The driver arms the in-flight slot, `H5Fcreate` then fails on the unwritable path, and cleanup must unlink it — `unlink()` needs write permission on the directory, not the file. Assert the marker file is gone, earlier partition files survive, and no master exists. This is the only injection that reaches the removal half of the registry contract.
  - `tests/integration/test_processing_order.py`, writability case: make the output directory read-only, run, and assert a non-zero exit with the probe's message before any dataset validation output.
  - The two permission-based cases above must skip when the effective uid is `0`, where mode bits do not deny access.
  - `tests/unit/test_master_hdf5_partitions.c`: replace the single-partition source case and add the snapshot master-link case.
  - `tests/scientific/test_scientific.py`: derive the snapshot-ordered partition filename as described in the authorized surface above. Establish that this test is **green before** your change on the snapshot pair (`MODEL=halos-only SIMULATION=micro-uchuu-snapshot python3 tests/scientific/test_scientific.py`) and green after, so the edit is proved to preserve a passing test rather than to rescue a broken one.
  - The gate's partition-count assertion.
- Commands to run:
  - `make check-generated`, `make validate-modules`, `make check-format` (exit 0)
  - default-pair build, then `make tests-unit`, `make tests-integration`, `make tests-scientific` (long tiers delegated to a subagent returning a pass/fail summary)
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific` — **the mandatory identity-gate re-run**, 8/8 stages
  - `make clean && make USE-HDF5=no -j`, then restore the default build
  - Tree-path preservation by the same before/after worktree vehicle as Slice 1
- Lint (differential, via the `lint` skill): required
- Manual checks: `h5ls -r` one snapshot-run master and two partition files; confirm each partition holds only its own group and each master link resolves to the right file.

**If the real micro-Uchuu dataset is not present**, the gate cannot run and this slice cannot be certified. Stop and report rather than accepting the slice on the fixture alone — the gate is a manual, dataset-present operation by design.

### Rollback Path

Revert the slice commit; Slice 1's inert seam remains and is harmless on its own. No baselines, generated files, or schema versions are touched. Output already written by a partitioned run stays readable — the master file and per-file `RunProperties` are self-describing.

---

## Slice 3: Documentation of record and checklist closure

**Developer seat:** `--model sonnet --effort high`. Prose only, but this project's documents of record are load-bearing and cross-referenced; the work is finding every stale statement, not writing new ones.

### Intended Change

- `docs/USER-GUIDE.md`: rewrite the snapshot-output bullet at `:405` for the new layout, and add a snapshot-run master-file example beside the tree-run example at `:509-515`.
- `docs/DEVELOPER-GUIDE.md`: update the "Output-partition seam" (`:1049`) and "Snapshot output schema" (`:1051`) paragraphs; extend the partition-model discussion at `:933` to record that a partition is one input chunk on the tree side and one requested output snapshot on the snapshot side; note in "The Snapshot Driver" that per-partition cleanup now matches the tree driver's.
- `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md:443`: the production run-constraints list still states "Output is written as **one partition plus a master**". Correct it — this is the document the production conversion is executed from, so a stale statement here misdirects the exact operation D5 exists to de-risk.
- `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`: add a dated supersession note to the Phase 5 Output Contract (`:92`) and item 5 (`:184-185`) recording that D5(a) replaces the single-partition and all-or-nothing-cleanup statements. Follow the document's existing correction-note convention (`:129`); do not rewrite its history.
- `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`: record that serial partitioned snapshot output is delivered here, superseding that plan's nominal assignment for the serial case — D5 explicitly requires this to be stated in both documents.
- `docs/dev/POST-PHASE-5-JOINT-REVIEW.md`: close §6 item 4 with the evidence, and update the checklist-state paragraph in §8.
- `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: mark item 4 closed in the sequence-item-5 table and move the "next step" pointer to item 5 (the D8 `Spin` units-label slice).
- `.agents/skills/mimic-run-and-operate/SKILL.md`: its "What a run produces" section describes per-file HDF5 outputs as carrying `Snap<NNN>/Galaxies` and `Snap<NNN>/TreeHalosPerSnap` "per output snapshot", which is wrong for a snapshot run both before and after this change. Correct both points. Sweep `mimic-architecture-contract` and `mimic-diagnostics-and-tooling` for the same class of statement.
- `docs/dev/MIMIC-SNAPSHOT-DRIVER-PLAN.md:856`: its closing summary still states that snapshot-ordered runs "write a single output partition". Add a dated supersession note in the same style as the `MIMIC-DUAL-DRIVER-PLAN.md` notes; do not rewrite the historical statement. (Added by the Revision 5 amendment — see the amendment record.)

### Acceptance Criteria

- Inputs: none.
- Outputs: documentation only.
- User-visible behaviour: no code behaviour changes.
- Behaviour that must not change: all of it — no source file is in this slice's authorized surface.

- [ ] No document of record or skill still states that a snapshot-ordered run writes exactly one output partition.
- [ ] No document of record still states that any failure removes every output file a snapshot run created.
- [ ] `SHIN-UCHUU-CONVERSION-PLAN.md`'s run-constraints list describes the delivered output layout.
- [ ] Both `MIMIC-DUAL-DRIVER-PLAN.md` and `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md` record the supersession, as D5 requires.
- [ ] The new layout is documented with a worked master-file example a reader can match against real output.
- [ ] The documented `hdf5_format_version` remains `1.2`, and the text states plainly that the per-file schema is unchanged while the run's file topology is not — the distinction the decision rests on.
- [ ] `make check-docs` exits 0 (no broken links or unresolved markers).
- [ ] The pathway's §6 table and next-step pointer reflect item 4 closed and item 5 next.

### Authorized Surface

- Files allowed to change:
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`
  - `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`
  - `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`
  - `docs/dev/POST-PHASE-5-JOINT-REVIEW.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-SNAPSHOT-DRIVER-PLAN.md`
  - `.agents/skills/mimic-run-and-operate/**`
  - `.agents/skills/mimic-architecture-contract/**`
  - `.agents/skills/mimic-diagnostics-and-tooling/**`
- Functions/classes/components allowed to change: none.
- Tests allowed or expected to change: none.
- **This plan file is deliberately *not* in the surface** (Revision 5). Recording execution status inside the contract being executed changes the frozen plan digest mid-run, which the supervising toolkit treats as a non-waivable integrity failure. Plan-closure bookkeeping happens after the run, outside any slice.

### Explicit Non-Goals

- No source, test, or generated-file changes.
- No `docs/VISION.md` change: the dual-driver vision update landed at Phase 5 closeout, and this is an implementation of existing principles rather than a new one.
- No archival of any plan document; `MIMIC-DUAL-DRIVER-PLAN.md` stays under `docs/dev/` until the Shin-Uchuu step consumes it.
- No rewriting of historical records — supersession notes only.

### Risk Flags

- Risky surfaces touched: none.
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs` (exit 0).
- Lint (differential, via the `lint` skill): required if any linted file changes; expected to be a no-op for Markdown-only edits.
- Manual checks: re-read each edited passage against the shipped behaviour, and confirm no hard-wrapped prose was introduced.

### Rollback Path

Revert the slice commit. Documentation-only.

---

## Slice 4: Retire the last fixed-partition-index filename assumptions in the integration tier

**Developer seat:** `--model sonnet --effort high`. Narrow, fully specified, and its risk is concentrated in "did the tree path move?", which the default-pair tier answers mechanically.

Added by the Revision 4 amendment. `tests/integration/test_output_formats.py` carries the same hard-coded `model_000.hdf5` assumption that blocked Slice 2, but it is unreachable from Slice 2's mandatory commands, so fixing it there would have shipped unvalidated edits inside the plan's riskiest slice. It gets its own slice and its own gate instead.

### Intended Change

- In `tests/integration/test_output_formats.py`, derive the HDF5 output filename from the run file's `output.snapshot_list` — the same derivation Slice 2 applies to `tests/scientific/test_scientific.py` — at exactly these three live sites: `test_hdf5_format_loading()` (`:333`), `test_hdf5_compression_equivalence()` (`:434`), and `test_unique_id_contract()` (`:601`). Retire the now-false `# filenr 0` comment at `:333`.
- Give `test_format_equivalence()` (`:645-771`, filename at `:697`) a processing-order guard so it **skips** on a snapshot-ordered package, following the pattern already in `tests/scientific/test_scientific.py`'s `selected_package_writes_binary()` (`:86-109`). This test compares binary against HDF5 output, and a snapshot-ordered package cannot produce binary at all — the run is rejected at configuration time — so the correct behaviour is a documented skip, not a filename fix.
- **Leave `test_hdf5_baseline_comparison()` (`:468-577`) alone**, including both its `model_000.hdf5` references (`:509`, `:523`). It already guards on `skip_non_default_baseline()` / `is_default_baseline_combo()` (`:495`) and therefore never runs on a snapshot pair, and its filenames are correct for the committed tree-ordered baseline under `tests/data/output/baseline/hdf5/`. Changing them would break a passing test.
- Update the docstring references at `:317`, `:472-473`, `:488` and `:650` only where the surrounding test's behaviour actually changed.

### Acceptance Criteria

- Inputs: unchanged run YAMLs; no new configuration keys, Make variables, or CLI flags.
- Outputs: no change to any Mimic output. Test-code and documentation changes only.
- User-visible behaviour: none.
- Behaviour that must not change: every galaxy record on both paths; the tree-ordered default pair's integration results.

- [ ] No test in `tests/integration/test_output_formats.py` derives a snapshot-run output filename from a fixed partition index.
- [ ] `test_hdf5_baseline_comparison()` is byte-for-byte unchanged.
- [ ] `test_format_equivalence()` skips on a snapshot-ordered package with a message naming the reason, and is unchanged in behaviour on a tree-ordered one.
- [ ] The default pair's `make tests-integration` result is unchanged from before this slice — same pass count, same skip count, zero failures.
- [ ] On the snapshot pair, the four HDF5-format tests named above each pass or skip for a documented reason, and none fails with a missing `model_000.hdf5`.
- [ ] No source file changed.

### Authorized Surface

- Files allowed to change:
  - `tests/integration/test_output_formats.py`
- **This plan file is deliberately *not* in the surface** (Revision 5), for the reason recorded in Slice 3's Authorized Surface. Marking the plan fully executed happens after the run, outside any slice.
- Functions/classes/components allowed to change: `test_hdf5_format_loading`, `test_hdf5_compression_equivalence`, `test_unique_id_contract`, `test_format_equivalence`, and their docstrings.
- Tests allowed or expected to change: the four named above only.

### Explicit Non-Goals

- No source, generated-file, or baseline changes; no new fixture.
- No change to `test_hdf5_baseline_comparison()`, or to any binary-format test.
- No attempt to make the snapshot pair's whole integration tier pass. That tier carries pre-existing failures for that pair — tests that default to `output_format: binary`, which snapshot-ordered runs reject at `src/core/read_parameter_file.c:1453` — and repairing them is a separate change with its own justification.
- No new helper in `tests/framework/`; reuse the existing pattern in place.

### Risk Flags

- Risky surfaces touched: none. Test code only, with the tree-ordered path protected by an unchanged-result criterion.
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: as described above.
- Commands to run:
  - Default pair: `make tests-integration` **before and after**, and show the two result lines side by side. This is the criterion that proves the tree path did not move.
  - Snapshot pair: `MODEL=halos-only SIMULATION=micro-uchuu-snapshot python3 tests/integration/test_output_formats.py`, before and after. Quote the `MIMIC_RESULT` lines for the four named tests both times. Restore the default pair afterwards.
  - `make check-format` (exit 0).
- Lint (differential, via the `lint` skill): required.
- Manual checks: confirm `test_hdf5_baseline_comparison()` is untouched in the diff.

### Rollback Path

Revert the slice commit. Test code and documentation only; no source, baseline, generated file, or on-disk format is involved.

---

## Amendment record

### Revision 5 — 2026-08-14, amended mid-execution by the supervising PM at the owner's explicit instruction

Slices 1 and 2 were executed and accepted against Revision 4 (`cb660208` + `b5b969d7`; `3e31cc0c` + `7b68e01d`). Slice 3 then executed its contract faithfully and **failed the supervising toolkit's mechanical floor anyway**, on a defect in this plan rather than in the change.

**The defect.** Slice 3's Intended Change ended with "Mark Slices 1–3 executed and record where their evidence lives", and Slice 3's Authorized Surface listed this plan file. Both were satisfiable only by editing the frozen contract during its own execution, which changes the plan digest the run froze at `init` — floor fact 1, which is non-waivable. The two facts contradicted each other in the same floor run: fact 5 (changed files ⊆ authorized surface) **passed**, because the file was genuinely authorized, while fact 1 **failed**, because that same file *is* the contract. No in-contract implementation could satisfy both. Slice 4 carried the identical defect: this plan file in its surface, and "Mark this plan fully executed" in its Intended Change.

The owner was stopped for, and authorized this amendment and the continuation.

**Changes made:**

1. **This plan file is removed from Slice 3's and Slice 4's authorized surfaces**, with the reason recorded in both, so no future execution of either slice can break its own digest.
2. **Both self-marking instructions are dropped** — Slice 3's "Mark Slices 1–3 executed" and Slice 4's "Mark this plan fully executed". Plan-closure bookkeeping is a post-run action, outside any slice. Execution status is already recorded durably in the PM run report, the per-slice assessments, and the commit history.
3. **`docs/dev/MIMIC-SNAPSHOT-DRIVER-PLAN.md` is added to Slice 3's authorized surface.** Its closing summary (`:856`) still states that snapshot-ordered runs "write a single output partition", which Slice 3's own acceptance criterion 1 forbids — but the file was absent from Slice 3's surface, so no in-contract implementation could fix it. This is the same defect class the external panel corrected four times in Revision 1 and once more in Revision 4: a stale contract surface the plan invalidates but never authorizes. Found by the PM while verifying Slice 3's criteria, and folded in here under the owner's standing pre-approval for genuinely-required surface expansion.

**Standing caveat, unchanged from Revision 4 and applying equally here.** Revisions 4 and 5 were written by the PM that supervises execution against them, not by the arm's-length external panel that converged Revisions 1–3. Reviewers commissioned on Slices 3–4 should be told which text is PM-authored so they read it as a contract under test rather than as settled ground.

### Revision 4 — 2026-08-13, amended mid-execution by the supervising PM at the owner's instruction

Revision 3 was frozen and Slice 1 was executed and accepted against it (commits `cb660208` and `b5b969d7`). Slice 2 then **stopped before any file was edited**: its Developer established that the slice could not pass its own mandatory validation without changing a file outside its authorized surface. The owner authorised this amendment; the scope below was the owner's choice from options put to them.

**The blocker, verified independently by the PM against the repository:**

`tests/scientific/test_scientific.py`'s `regenerate_output()` hard-codes `tests/data/output/hdf5/model_000.hdf5` for a snapshot-ordered package, and Slice 2 necessarily renames that file to `model_049.hdf5`. All four links were checked directly: the hard-coded path exists and does not skip for snapshot-ordered packages; `scripts/generate_test_registry.py`'s `core_scientific_tests()` puts every `tests/scientific/test_*.py` into the scientific tier for every simulation, and that directory contains only this file, so it is unavoidably inside the mandatory identity-gate command; the generated vehicle requests `snapshot_list: [49]` because `scripts/generate_test_inputs.py:211` uses `snapshot_list=[last_snap]` against a 50-entry `a_list`; and the file appeared in neither of Slice 2's two closed enumerations. No second implementable reading exists — owner decision 1 pins the partition output id to the snapshot number precisely to reject dense index numbering.

This is the same defect class the external panel already fixed for four other stale contract surfaces in Revision 1. Two further instances were missed; a new acceptance criterion in Slice 2 now guards the class rather than the instances.

**Changes made:**

1. **Slice 2 authorized surface** gains `tests/scientific/test_scientific.py`, with the required fix, the reason it is unavoidable, and the instruction not to touch its tree-side branch.
2. **Slice 2 acceptance criteria** gain "no test derives a snapshot-run output filename from a fixed partition index", and the in-source-comment criterion gains an explicit scope ruling that excludes `scripts/compare_cross_format_identity.py:213` — its technical claim survives the change, so it needs no edit.
3. **Slice 2 validation plan** requires the affected test to be shown green *before* the change as well as after, so the edit is proved to preserve a passing test rather than to rescue a broken one.
4. **Slice 2 non-goals** now record the `FileNum` consequence explicitly. `FileNum` has exactly one read site in physics (`sham_assign_stellar_mass.c:86`, an RNG-seed fallback), and it is unreachable today because `sham` has no snapshot-ordered run file. It is a recorded consequence, not a defect, and the implementer is told not to work around it.
5. **Slice 1's criterion at `:126`** is narrowed to the reading the PM ruled on during execution. As literally written it was contradicted by the same slice's own binding requirements — the master's outer loop must stay over all `NOUT`, and the binary writers are excluded — so no in-contract implementation could satisfy it. Slice 1 is already accepted on the narrowed reading; this edit is record hygiene and requires no re-run. The defect was found by the `codex` code reviewer, which correctly addressed it to the PM rather than filing it as implementer drift.
6. **Slice 3** now marks Slices 1–3 executed rather than the whole plan, since Slice 4 closes it.
7. **Slice 4 is new**, owning `tests/integration/test_output_formats.py`. That file carries the same defect at three live sites plus one test needing a processing-order skip, but it is unreachable from Slice 2's mandatory commands, so folding it into Slice 2 would have meant unvalidated edits inside the plan's riskiest slice. It gets its own gate: the default pair's integration result must be unchanged, and the snapshot pair's four HDF5 tests must pass or skip for a documented reason. `test_hdf5_baseline_comparison()` is explicitly excluded — it already skips on non-default combos and its `model_000.hdf5` references are correct for the committed tree-ordered baseline.

**Standing caveat on this revision.** Revisions 1–3 were converged by two independent external reviewers. Revision 4 was written by the PM that supervises execution against it, so the amended clauses have not had that arm's-length review. The clauses concerned are listed in change 1–7 above; reviewers commissioned on Slices 2–4 should be told which text is PM-authored so they read it as a contract under test rather than as settled ground.

---

## External panel record

Revision 1 of this plan (sha256 `b7ad11b0…`) was reviewed by two independent read-only reviewers launched through the orchestrator, both at high effort, against the repository at HEAD `99b99cff`: `codex` (gpt-5.6-sol) and `opencode` (opencode-go/hy3). Artifacts under `.orchestrator/runs/delegates-20260813-164520-98226/`. Both were asked to fact-check every claim, hunt for defects that would cost an implementation round, and flag bloat, overtesting, and low-value work.

**Verdicts:** codex REJECT (0 P0, 2 P1); opencode ENDORSE WITH CORRECTIONS (0 P0, 0 P1). Every finding below was independently re-verified against the repository before being accepted.

**Accepted and fixed in this revision:**

- **P1 — the selection could not reach the writers.** `process_partition()` (`:199`) and `claim_and_process_partition()` (`:289`) take only `output_id`; neither was in the authorized function list, so the threading Slice 1 requires was unauthorized. Now authorized, with the propagation specified and reuse of `output_id` as a partition index explicitly forbidden. (codex)
- **P1 — Slice 1 carried an unsatisfiable criterion.** "RunProperties in a partition whose first requested snapshot index is not 0" cannot occur while both sources return the full index table. Moved to Slice 2, where it is the criterion that proves the metadata relocation. (codex)
- **The cleanup-registry refactor was under-specified**, leaving the implementer to invent a per-slot API with `bye()` semantics at stake. The three helpers and the widened meaning of `snapshot_driver_clear_output_paths()` are now pinned. (opencode)
- **Four stale contract surfaces were unauthorized**: `src/include/proto.h:52-62`, `src/io/output/util.h`, `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md:443`, and the `mimic-run-and-operate` skill. All now authorized in the slice that invalidates them, with acceptance criteria. (codex)
- **Four wrong or imprecise references** — the `[49,1]` filename example, `tree_driver.c:295` (the `--skip` branch, not the success-path disarm at `:309`), the "`Snap049` would never get metadata" claim (`n` is the list index, so with `[49,1]` it is `0`), and the integration-test line range. All corrected; this project treats stale references inside a frozen plan as real defects. (both)
- **"At most one output file handle live" was false** during master aggregation, which holds the master plus one read-only partition. Reworded to the driver's writable-file invariant. (codex)
- **Three acceptance paths had no executable check.** The empty-snapshot criterion was unexercised (the fixture's empty snapshot 0 is now added to the requested list — one change covering two criteria); "force a mid-run failure" named no reproducible fault (now a deterministic link corruption that aborts at `load_slab`, not `open_run`); and the probe had no failure check (now added). (codex)
- **Bloat, dropped:** the binary-branch fail-fast (unreachable under the frozen configuration validation — both reviewers independently called it speculative scaffolding, and the `USE-HDF5=no` stub precedent it cited exists for a linker reason, not a defensive one); and the `lsof` handle check (platform-sensitive and contradicted by the legitimate two-handle master phase).
- **The writability probe moves before `snapshot_reader_open_run()`** — behind it, dataset validation runs first and the failure is no longer early. (codex)
- **Vision framing corrected:** Principle 5 governs memory working sets, not output topology; it now motivates by analogy rather than being cited as a mandate. The no-bump rationale now distinguishes per-file schema (unchanged) from run-level file topology (changed) instead of implying nothing changed. (codex)
- **Slice 1's `Independent audit required` downgraded to `no`** — both reviewers judged it disproportionate for a slice whose acceptance is "zero observable change" proved by mechanical diffs.

**Declined, with reasons:**

- **Merging Slice 1 into Slice 2** (codex). Opencode argued the opposite and the split is retained: Slice 1's value is that its inertness is *provable*, and a merged diff makes a tree-path delta attributable to either cause — with the multi-hour identity gate then running against an unbisected change. Codex's strongest supporting argument, the unsatisfiable criterion, is fixed independently by moving it. The duplicated cost is one extra tier run and one extra preservation run; the gate runs only in Slice 2 either way.
- **"Branch clean at HEAD `99b99cff` is unverified"** (codex). A limitation of the reviewer's read-only access, not a plan defect; verified directly by the Developer.

**Round 2** (Revision 2, sha256 `61b651af…`, same two reviewers, same effort). **Both endorsed with no surviving P0 or P1**: codex ENDORSE WITH CORRECTIONS (0/0), opencode ENDORSE (0/0), the latter stating plainly that no further round is warranted on defect grounds. Both independently re-verified that the deterministic fault-injection recipe works against this reader — link values are range-checked only from `load_slab_snapshot_hdf5()` (`src/io/snapshot/read_snapshot_hdf5.c:1297`), never during `open_run` — and both confirmed every round-1 fix landed. **Panel convergence reached.**

Revision 3 applies their residuals:

- **The retention test could not prove in-flight removal** (codex P2, opencode P3 — the same finding from both). A failure during slab loading happens before that snapshot's output file exists, so the "in-flight partition file is removed" half of the registry contract was unexercised. The criterion is now split in two, with a second injection (a read-only file pre-created at the target partition path) that reaches the removal path.
- **The fault-injection mutation is now pinned** to `FirstHaloInFOFgroup[0] = n_halos` (codex), which is bounded by its own snapshot's halo count and therefore cannot accidentally be in range.
- **The D5(b) note was too strong** (codex): splitting one snapshot across files also needs record-range routing in the writer, since `save_halos_hdf5()` scans the whole lent buffer. Named in the follow-up note.
- **Three more references corrected** — the per-file `RunProperties` assertion is at `tests/integration/test_processing_order.py:310-312` and the master block at `:314-326` (codex; opencode passed the earlier values, and the Developer's own check settled it), and `snapshot_open_output()` is defined at `src/core/snapshot_driver.c:492` though called at `:707` (opencode).
