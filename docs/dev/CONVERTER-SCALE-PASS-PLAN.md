# Converter Scale-Engineering Pass — Implementation Plan (D4 / JR §6 item 7)

**Status:** Frozen 2026-08-26, revision 3 after two rounds of external panel review (codex/gpt-5.6-sol and opencode/opencode-go/hy3, both read-only, both at high effort) and an author fresh-eyes pass. Scoped from the Session C subset rehearsal's measurements, not from the superseded analytic projections.
**Owner checklist item:** `POST-PHASE-5-JOINT-REVIEW.md` §6 item 7 (F-13 / D4). Full scope statement: `SHIN-UCHUU-CONVERSION-PLAN.md` → "Pre-conversion obligation".
**Blocks:** the Shin-Uchuu production conversion (pathway step P2), and through it every remaining step to a production `sage16` run.

---

## Purpose

The shipped converter cannot execute the production conversion. This is not a performance complaint. At 22.9 × 10⁹ halos three stages exceed installed memory — the `links` rank pass (≈4.30 TB projected), the `validate` battery and the `crosscheck` comparator, all of which are full-dataset-resident — the workdir exceeds installed disk, and the scatter resume model is incompatible with the only viable transfer strategy. This plan turns six open items into eight bounded, separately gateable code slices plus an acceptance gate.

The rehearsal that scopes it converted a 406,668,896-halo subset end to end on the **unmodified** shipped converter, deliberately, so every figure below describes the tool this plan re-engineers.

---

## What is in scope, and what is already settled

Eight items were identified. Two are closed before this plan starts and are recorded here so they are not re-opened:

| Item | State |
|---|---|
| 5 — fix-up stage sequential satellite scan | **Answered: RETAIN.** Measured ≈1.28 µs/satellite (1.37 s at snapshot 44, 2.49 s at snapshot 69), projecting to ≈1.2–1.6 h across all 70 snapshots at production on a one-time multi-day conversion. Not worth rewriting code whose exact `fix_upid` reference parity is load-bearing. No slice. |
| 8 — `recommended_multiplier()` power-of-ten search | **CLOSED 2026-08-26** in `0ab453fe`, landed early and independently. No slice. |

The six remaining items map onto the slices below:

| Item | Measured problem | Slice |
|---|---|---|
| 7 | `manifest.save()` after every source file; 38.2 KB/entry → 104.9 MB at 2,744 files, cost quadratic in file count | 1 |
| 4 | Phase 0 forest map pickled per pool task; ~176 MB × 2,744 here, ~5 GB × 2,744 at production | 2 |
| 3 | `run_scatter` requires every source file to exist at start and re-stats completed files; the batched transfer breaks resume in **both** directions | 3 |
| 1 | `compute_identity()` in-memory rank pass: **187.84 B/halo measured → ≈4.30 TB** at production, 8.4× installed RAM | 4, 5 |
| 2 | `validate.py` **and** `crosscheck.py` full-dataset-resident: 73.27 GB and **251.32 GB** on a 1.8% subset | 6, 7 |
| 6 | Deletion stops after concat; `fixups`/`links`/`write` delete nothing → 277 B/halo coexisting = 6.34 TB at production | 8 |

Slice 9 is the pass's own acceptance gate.

---

## Measured ground truth this plan is scoped against

All from the Session C rehearsal, 406,668,896 halos, recorded in `HANDOFF.md` §7 and §7a. Production figures scale to 22.9 × 10⁹ halos.

| Quantity | Measured | Production projection |
|---|---|---|
| Rank pass (`links`) peak RSS | **76.39 GB** = 187.84 B/halo | **≈4.30 TB** — 8.4× installed RAM |
| `validate` battery peak RSS | 73.27 GB | full-dataset-resident; unbounded |
| `crosscheck compare` peak RSS | **251.32 GB** on a 1.8% subset | not runnable — and not merely on memory grounds; see Slice 7 |
| Scatter aggregate throughput | **39.0 MB/s**, pool workers at 12–25% CPU | ~83 h in scatter alone |
| Storage bandwidth / per-core parse | ~385 MB/s / ~105 MB/s | the headroom items 4 and 7 exist to recover |
| Manifest growth | 38.2 KB per source file → 104.9 MB at 2,744 | **same 2,744 files** — does not shrink with a smaller dataset |
| Coexisting workdir intermediates | 277 B/halo | **6.34 TB** |
| Emitted dataset | 100.7 B/halo | ≈2.31 TB |
| Python / multiprocessing | 3.13.2; `spawn` is the **platform default** on this macOS host (Python ≥ 3.8) and is **not** set anywhere in `scripts/convert/` | worker globals are not fork-inherited here |

**Two figures moved during the rehearsal and the old ones must not be used:** the rank pass is ≈4.30 TB, not the recorded 1.10 TB; terminal workdir is 6.34 TB, not the recorded ≈8.60 TB (which double-counted `snap_NNN.bin`, deleted by sort).

---

## Target platform and storage envelope

The conversion runs on the **Mac Studio M3 Ultra, 32 cores, 512 GB** (`SHIN-UCHUU-CONVERSION-PLAN.md` → "Where The Work Runs"). Every measurement above is from that host, and the acceptance gate's memory profile is to be taken there. `tooarrana1` is a 4-core, 251 GB shared login node with no scheduler; it runs exactly two streaming steps and is not a conversion target.

**Storage is the binding constraint on the whole pass, and it is why the slices are ordered as they are.** The primary scratch volume (`/Volumes/LaCie`) has 7.37 TB free against an 11.61 TB source, so the source can never be fully staged: the transfer must be batched and consumed as it is scattered. Every converter-owned byte competes in one envelope — the staged source batch, workdir intermediates, the rank spill files and on-disk identity arrays this plan introduces, any validation index Slice 6 introduces, and the ≈2.31 TB emitted dataset. Slice 8 owns that envelope and its absolute ceiling. Cross-check artifacts are deliberately outside it, for the reason Slice 7 gives.

**Remote working and output locations** (created 2026-08-26): `/fred/oz214/dcroton/shin-uchuu/working/` for working data and `/fred/oz214/dcroton/shin-uchuu/snapshot-trees/` for the archived converted trees. The canonical location beside the source — `/fred/oz214/simulations/uchuu/shinuchuu/` — is owned by `msinha` with group `oz214` at mode `drwxr-sr-x`, i.e. **not writable** by `dcroton`; a verified `mkdir` fails there. Using it instead requires the owner to grant write access or create the directories, which is an operator action outside this plan.

---

## Conventions every slice inherits

- **`scripts/convert/` is stdlib + numpy + h5py only.** No pandas, no new third-party dependency. The remote venv has no pandas and `subset.py` is numpy-only for that reason.
- **`class Manifest` lives in `scripts/convert/scatter.py:246`** and is imported by `fixups`, `links`, `sort_index`, `hdf5_writer` and `report`. It owns `register_intermediate`, `verify_intermediate` and `remove_intermediate` — the intermediate-ownership contract. A slice that changes manifest behaviour therefore authorizes `scatter.py` even when it makes no change to the scatter *stage*.
- **The converter test suite is stdlib `unittest`**, discovered under `scripts/convert/tests/`, independent of `MODEL`/`SIMULATION`. Run it with `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`. Baseline at `0ab453fe`: **365 tests, exit 0**.
- **Reference semantics must not move.** Every slice preserves the emitted dataset byte-for-byte on micro-Uchuu unless its own acceptance says otherwise. Where a slice changes an algorithm, the binding evidence is a before/after comparison on the same input, not an argument.
- **The delete-after-verify protocol is `sort_index.py:86-129`, and it is stated here exactly** because a slice below depends on the detail: the successor is re-read from disk and checked for row count, id checksum and index agreement; it is then `register_intermediate`-d (which is where its manifest checksum is first computed); the manifest is saved; the predecessor is `remove_intermediate`-d; the manifest is saved again. A `_retry_*_cleanup` helper converges a crash between unlink and save. Manifest **checksum** verification applies to resumed or skip-trusted artifacts (`sort_index.py:40-49`), not to a successor being created for the first time — requiring it there would mean a redundant multi-terabyte re-read.
- **Style:** `docs/STYLE-GUIDE.md`. Python is black + isort at 100 columns via `./scripts/beautify.sh`. Module and public-helper docstrings explain contract and units, not mechanics. Prefer explicit failures over natural-language output parsing.
- **No new CLI flag unless the slice's receipt names it.** Public CLI flags are a risky surface; flags are introduced deliberately, with a default that preserves current behaviour where the change is destructive.

---

## Implementation Profiles

- Recommended for frontier/senior implementer: run Batch A, then Slice 3, then Batch B, then Batch C, then Slice 8, then Slice 9.
- Recommended for standard implementer: run slices individually.
- Recommended for weaker implementer: run atomic slices one at a time; do not attempt Slices 4–5 without the batch context.

## Slice Batches

- Batch A: Slices 1–2 — both are localized changes to `run_scatter`'s dispatch and persistence machinery with no algorithmic effect on emitted data; one diff and one review cover them coherently.
- Batch B: Slices 4–5 — the external-merge core and its wiring are one algorithm split at a testability seam; the second is unreviewable without the first.
- Batch C: Slices 6–7 — the same streaming-window transformation applied to the two validation consumers.

**Slices 3, 8 and 9 are never batched.** Slice 3 changes resume semantics on an irreplaceable multi-day run; Slice 8 destroys data; Slice 9 is the gate that judges the others.

---

## Machine load and model selection

Each slice carries this as a banner under its own heading; the table is the at-a-glance version. **The banner is authoritative** — if the two ever disagree, fix the table.

**Machine classes.** These describe what the slice's *verification* demands, not its editing.

| | Meaning |
|---|---|
| 🟢 **Free** | Under ~10 GB and no gated timing. Run local models or other large jobs alongside without affecting the result. |
| 🟡 **Quiet** | Memory is modest, but the slice records a number a contended box would corrupt — wall clock, worker CPU, or a disk envelope. Develop freely; free the box for the measurement run. |
| 🔴 **Full box** | Needs a large fraction of the 512 GB and/or the LaCie volume. **No local models, no other large jobs.** |

| Slice | What it is | Machine | Peak to expect | Developer |
|---|---|---|---|---|
| 1 | Batch the scatter manifest saves | 🟢 Free | < 10 GB | **Sonnet, high** |
| 2 | Forest-map distribution | 🟡 Quiet (timing) | < 10 GB | **Sonnet, high** |
| 3 | Batch-aware source inventory | 🟢 Free | fixture-scale | **Opus, high** |
| 4 | External merge-sort rank core | 🟢 Free | bounded by design | **Opus, high** |
| 5 | Wire the rank pass | 🔴 **Full box** | 76.39 GB baseline → ≤ 24 GB target | **Opus, high** |
| 6 | Stream the validation battery | 🔴 **Full box** | 73.27 GB baseline | **Opus, medium** |
| 7 | Stream the topology cross-check | 🔴 **Full box** | **251.32 GB baseline — the largest here** | **Opus, high** |
| 8 | Consumptive deletion | 🟡 Quiet (disk/I-O) | modest memory; needs LaCie headroom | **Opus, high** |
| 9 | Acceptance gate and documentation | 🔴 **Full box** | re-measures everything, incl. the 251 GB cross-check | **Opus, high** |

**Four of the nine slices leave the machine free** — 1, 2, 3 and 4 — and they are deliberately the first four in execution order, so the early part of this pass can overlap other work on the box. **Slices 5, 6, 7 and 9 need the machine to themselves.** Slice 7 is the one to plan around: at 251 GB it is about half the installed memory, for a dataset that is 1.8% of the production box.

**Two reasons the 🟡 class exists, and only one is obvious.** `ru_maxrss` is per-process, so another job cannot inflate a slice's reported peak RSS — the memory numbers stay valid on a busy box. What a busy box *does* corrupt is **wall clock and I/O throughput**, and Slices 2 and 8 each record one. This is the same distinction `HANDOFF.md` draws for the rehearsal tasks, and it is why "quiet" is a real class rather than a weaker "full box".

**Project manager: Opus, high effort.** PM makes accept/reject calls on slices whose failure modes are irreversible data deletion (8), a silently mixed multi-day conversion (3), and a producer check weakened without anyone noticing (6, 7). It also owns the two approval gates and must commission independent review on the five elevated-risk slices. **Opus at medium effort is acceptable while executing Batch A only** — Slices 1 and 2 carry no approval gate and no elevated-audit requirement — but escalate before Slice 3.

**How this relates to the Implementation Profiles above.** Those govern *batching* — how many slices share one implementation and review loop. This table governs *model and effort per slice*. They are independent: a frontier implementer running Batch A should still do so on Sonnet, because Slices 1 and 2 are small and crisply oracled, and spending Opus on them buys nothing.

---

## Ordering, and why it is this order

Scatter first (Slices 1–3), then the rank pass (Slices 4–5), then validation (Slices 6–7), then deletion (Slice 8), then the gate (Slice 9).

**Scatter leads on calendar grounds.** Slice 3 is the prerequisite for the batched consumptive transfer, and the local volumes cannot stage the 11.61 TB source any other way. Landing Slices 1–3 early lets the ~29 h transfer (P1) start and overlap the remaining slices, which is the only place in this plan where wall clock can be recovered.

**Deletion comes last on correctness grounds, and this is a change from revision 1.** Slice 4 introduces rank spill files and Slice 5 introduces on-disk identity arrays — new intermediates with new lifetimes. A deletion contract frozen before they exist would be incomplete by construction, and re-auditing it afterwards costs more than sequencing it correctly. Slice 8 therefore sees the final set of intermediates and can enumerate a deletion table and a storage envelope over all of them at once.

---

## Slice 1: Batch the scatter manifest saves (item 7)

> **Machine:** 🟢 **Free** — fixture-scale tests plus one micro-Uchuu battery run; under ~10 GB. Run local models freely alongside. *(The parent-side JSON speed-up is a stated behaviour, not a gated number, so it needs no quiet box.)*
>
> **Developer model:** **Sonnet, high effort** — a small, well-bounded change to one callback, with byte-equality of the final manifest as an unambiguous oracle.

### Intended Change
- `run_scatter`'s `record()` callback calls `manifest.save()` once per completed source file, rewriting the entire manifest each time. Each source entry carries 70 per-snapshot counts, 70 per-snapshot checksums and 70 observed `(SnapNum, scale)` pairs, so the manifest grew a measured 38.2 KB per file and reached 104.9 MB at 2,744 files; the rewrite cost is quadratic in source-file count.
- Replace the per-file save with a bounded-interval save: accumulate completed source entries in the in-memory manifest and persist on a save policy (every N completed files and/or every T seconds), with an unconditional save before `_finalize_scatter` and on exit from the pool loop.
- **Only one durability direction is preserved, and the plan says so explicitly.** Batching necessarily means worker artifacts exist on disk before the manifest records them. That is the safe direction. The unsafe direction — the manifest naming an artifact that was never durably written — must remain impossible.

### Acceptance Criteria
- Inputs: an unchanged `run_scatter` call signature plus a save-policy control; the policy's default must bound worst-case re-work to a small, stated number of source files.
- Outputs: a manifest whose final content is **byte-identical** to what the per-file-save implementation produces for the same input set.
- User-visible behaviour: scatter completes with the same manifest, same intermediates, and the same per-file conservation enforcement; wall clock for the parent-side JSON is materially reduced.
- Behaviour that must not change:
  - [ ] Per-file conservation (independent pre-count == scattered rows) is still enforced **before** a completion is recorded, for every file.
  - [ ] `validate_observed_pairs` still runs per file, before that file's completion is recorded.
  - [ ] Worker-scratch, observed-roots and forest-max intermediates are still registered for every completed file.
  - [ ] A resumed run still skips exactly those files whose manifest entry is `completed` and whose size/mtime match.
  - [ ] The emitted dataset is unchanged.
- [ ] The persisted manifest never references an artifact that was not durably written. This is the binding invariant.
- [ ] Unsaved-but-written artifacts are explicitly permitted, and a re-scatter of the owning source file overwrites them deterministically — asserted by a test that leaves such an artifact behind and re-runs.
- [ ] After a simulated crash between two saves, a resumed run re-scatters only the unsaved files and reaches a final manifest byte-identical to an uninterrupted run's.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/scatter.py`
  - `scripts/convert/tests/test_scatter.py`
- Functions/classes/components allowed to change: `run_scatter`, its local `record` callback, and `Manifest.save` only if a batching helper is genuinely better placed on the class.
- Tests allowed or expected to change: `scripts/convert/tests/test_scatter.py`.

### Explicit Non-Goals
- Do not change the manifest schema, the per-source-entry content, or what is checksummed.
- Do not change the resume *semantics* (that is Slice 3).
- Do not touch the pool dispatch or the forest-map passing (that is Slice 2).
- Do not make the manifest append-only or introduce a second on-disk file; batching the existing atomic whole-file save is sufficient and preserves `os.replace` atomicity.

### Risk Flags
- Risky surfaces touched: persistence (manifest durability), crash-resume behaviour.
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: final-manifest equality against a per-file-save run; crash-and-resume convergence; the orphaned-artifact overwrite test above.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: on the micro-Uchuu battery, confirm identical `manifest.json` content against a pre-change run.

### Rollback Path
- Single-file revert of `scripts/convert/scatter.py`; the manifest format is unchanged, so a workdir written by either version remains readable by both.

---

## Slice 2: Distribute the forest map without per-task pickling (item 4)

> **Machine:** 🟡 **Quiet for the measurement only** — memory is modest, but this slice records scatter wall clock and worker CPU against the 39.0 MB/s and 12–25% baselines, and a contended box corrupts timings even when it cannot touch peak RSS. Develop and test alongside anything; free the box for the before/after timing run.
>
> **Developer model:** **Sonnet, high effort** — localized to the pool dispatch, with a per-worker transfer-count assertion as the gate rather than a judgement call.

### Intended Change
- `run_scatter` builds `args = [(path, i, scratch_dir, forest_map, chunksize) ...]` and hands them to `Pool.imap_unordered`, so the whole `ForestMap` is pickled once **per task**: ~176 MB × 2,744 tasks at rehearsal scale, ~5 GB × 2,744 at production. Measured effect: pool workers at 12–25% CPU while the parent serializes.
- Give each worker the forest map **once per worker instead of once per task**, via a `Pool` initializer. The mechanism is deliberately left open: calling the existing `load_forests_list(path)` in the initializer is sufficient and introduces no new on-disk representation. A shared or memory-mapped representation is permitted if per-worker residency proves too costly at production scale, but it is not required, and it should not be built speculatively.
- **The start method is `spawn` on this host** — Python 3.13.2 on macOS, where `spawn` has been the default since 3.8. It is not set anywhere in `scripts/convert/`, so this is a platform default rather than a repository guarantee. Under `spawn`, worker globals are not fork-inherited and an initializer that loads from a path is required; the same design also works unchanged under a `fork` default.

### Acceptance Criteria
- Inputs: the same `forests_list_path`; the map reaches each worker once per worker.
- Outputs: identical `FileScatterResult` for every source file.
- User-visible behaviour: scatter throughput improves; nothing about the emitted data or the manifest changes.
- Behaviour that must not change:
  - [ ] `ForestMap.lookup_forest_ids` returns identical results in workers as in the parent, for every source file.
  - [ ] `forest_map.md5` recorded in provenance is unchanged and still computed from the exact bytes the loader parsed.
  - [ ] `validate_root_coverage` still runs in `_finalize_scatter` over the concatenation of all observed roots.
  - [ ] The serial path (`pool_size <= 1` or a single pending file) still works and takes the same code path for the lookup.
  - [ ] The emitted dataset and the manifest are unchanged.
- [ ] The forest map is transferred to each worker process at most once per worker, asserted by a test that counts initializer invocations or serialization events rather than by inspecting wall clock.
- [ ] If — and only if — the implementation materializes a temporary file for sharing, it is created inside the workdir and removed on both success and failure paths. The per-worker `load_forests_list` route creates no such artifact, and then this criterion and its test do not apply.
- [ ] Scatter wall clock and worker CPU are recorded before and after against the 39.0 MB/s and 12–25% baselines. This is a **recorded measurement, not a gate** — the gate is the per-worker transfer count above.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/scatter.py`
  - `scripts/convert/tests/test_scatter.py`
- Functions/classes/components allowed to change: `ForestMap`, `load_forests_list`, `_scatter_worker`, `run_scatter`'s pool dispatch, and a new worker-initializer helper.
- Tests allowed or expected to change: `scripts/convert/tests/test_scatter.py`.

### Explicit Non-Goals
- Do not change `scatter_one_file`'s per-file algorithm or its outputs.
- Do not change the manifest or its save policy (Slice 1 owns that).
- Do not set a global multiprocessing start method, and do not make the design depend on `fork` semantics.
- Do not build a shared-memory or memmap representation unless per-worker loading is measured to be inadequate; if it is built, say what measurement justified it.

### Risk Flags
- Risky surfaces touched: concurrency, worker lifecycle, temporary-file ownership.
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: worker-side and parent-side `lookup_forest_ids` agreement over a synthetic map; initializer path equals serial path; per-worker transfer-count assertion; and, only if a shared artifact is introduced, its removal on a failing run.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: micro-Uchuu battery green; record scatter wall clock and worker CPU before/after.

### Rollback Path
- Single-file revert of `scripts/convert/scatter.py`. No on-disk format changes, so no workdir migration.

---

## Slice 3: Batch-aware source inventory for the interleaved transfer (item 3)

> **Machine:** 🟢 **Free** — fixture-scale throughout; the two-batch cycle runs against a copied fixture dataset, not real data. Run local models freely alongside.
>
> **Developer model:** **Opus, high effort** — a resume state machine on an irreplaceable multi-day run. Approval-gated, independent audit required; the failure mode is a silently mixed conversion.

### Intended Change
- The batched transfer needs resume to survive a source file being absent for **two different reasons**, and the shipped converter refuses both. `run_scatter` raises if any listed source file is missing at start (`scatter.py:521-524`), freezes the ordered source-file list into provenance and refuses to resume if it changes (`scatter.py:541-556`), and `Manifest.source_completed()` calls `Path(path).stat()` (`scatter.py:361-366`), which raises once a completed source has been deleted.
- Introduce an explicit **batch mode** that distinguishes the two absences:
  - **Deferred** — a file in the frozen inventory that has not been transferred yet. In batch mode this is skipped for now, not an error; the run scatters what has arrived and exits without finalizing.
  - **Consumed** — a file whose scatter completed and whose bytes have since been released. It satisfies resume without being re-stat-ed or re-scattered, and its recorded identity (size, mtime, md5, counts, checksums) remains the frozen record of what was processed.
- **The complete ordered inventory is still frozen once, at first run, and every batch-mode invocation must supply that same complete list.** It is carried by the existing positional `tree_files` argument — the operator passes the full 2,744-entry inventory every time, not the subset currently on disk — so the frozen-set guard (`scatter.py:541-556`) compares like with like and still refuses a genuinely different conversion. No new index artifact or format is introduced; what changes is that entries whose bytes are absent are classified rather than rejected.
- **`_finalize_scatter` itself must accept a consumed entry.** It currently raises unless every source entry's status is exactly `completed` (`scatter.py:635-639`), so a batched run that released its early batches could never finalize. It must accept `completed` or `consumed` and reject anything else, including `deferred`.
- **Finalization becomes an explicit operator action in batch mode.** Today `run_scatter` finalizes unconditionally once its pending list is exhausted (`scatter.py:627`), which in batch mode would finalize the moment the last batch completes — before that batch could be released, and `_finalize_scatter` deletes worker intermediates (`scatter.py:753-765`) that a later release would then fail to verify. In batch mode `run_scatter` therefore scatters and stops; a separate explicit finalize step runs when every inventory entry is `completed` or `consumed` and none is `deferred`.
- **Consumption is an explicit operator action with its own CLI subcommand**, not a side effect and not an inference from a stat failure. Releasing a source file's bytes requires that its entry be `completed` and that all of its registered intermediates verify first; the command records the transition and is what makes it safe for the operator to delete those bytes.
- Outside batch mode every current behaviour is unchanged, including the hard error for a missing file.

### Acceptance Criteria
- Inputs: the frozen ordered inventory; the subset of its files currently present on disk; an explicit batch-mode selector.
- Outputs: an unchanged manifest schema apart from the added states, and an unchanged emitted dataset.
- User-visible behaviour: a conversion can be driven as `transfer batch → scatter → release → transfer next batch`, resuming correctly at every step, without ever holding the whole source locally.
- Behaviour that must not change:
  - [ ] Outside batch mode, a missing source file is still a hard error naming the file.
  - [ ] A file that is present and not completed is still scattered, in both modes.
  - [ ] A file that is present, completed, and whose size/mtime still match is still skipped, in both modes.
  - [ ] A file that is present, completed, and whose size or mtime **differ** is still an error in both modes — silent substitution must remain impossible.
  - [ ] A change to the frozen inventory's membership or order still refuses to resume, in both modes.
  - [ ] `a_list`, `forests_list` and `simulation_info` md5 mismatches still refuse to resume.
  - [ ] The pending-files-after-snapshots-finalized guard still fires.
- [ ] In batch mode `run_scatter` never finalizes: it scatters what has arrived and exits, reporting how many entries remain `deferred`. Finalization is reachable only through the explicit finalize step, which refuses to run while any entry is `deferred` and proceeds when every entry is `completed` or `consumed`. Outside batch mode finalization still happens automatically, exactly as today.
- [ ] Root-coverage validation in `_finalize_scatter` sees every source file's observed roots, including consumed ones, because those are read from registered intermediates rather than from source bytes.
- [ ] The release command refuses a file whose entry is not `completed`, and refuses one whose registered intermediates do not verify.
- [ ] A full batched cycle over a fixture dataset — scatter batch 1, release batch 1, scatter batch 2, release batch 2, finalize — produces an **emitted dataset byte-identical** to a single all-at-once run, and a manifest identical in provenance and every per-source content field (size, mtime, md5, counts, checksums, observed pairs). The source-entry *lifecycle state* legitimately differs (`consumed` versus `completed`) and that difference alone must not fail the comparison.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/scatter.py`
  - `scripts/convert/convert_ctrees.py`
  - `scripts/convert/tests/test_scatter.py`
  - `scripts/convert/README.md`
- Functions/classes/components allowed to change: `Manifest.source_entry`, `Manifest.source_completed`, new deferred/consumed state transitions on `Manifest`, `run_scatter`'s existence check, pending computation and finalize gating, `_finalize_scatter`'s per-entry status acceptance, and the two new subcommands in `convert_ctrees.py` — `release` (record a completed source as consumed) and an explicit `finalize`.
- Tests allowed or expected to change: `scripts/convert/tests/test_scatter.py`.

### Explicit Non-Goals
- Do not delete source files from inside the converter. The release command records that a file *may* be deleted and verifies it is safe; the deletion itself stays with the operator or the transfer script.
- Do not weaken the frozen-inventory guard or the md5 provenance guards.
- Do not touch stage-intermediate deletion (Slice 8).
- Do not make batch mode the default.

### Risk Flags
- Risky surfaces touched: persistence, resume correctness on an irreplaceable multi-day run, provenance guards that exist to prevent mixing two conversions, and two new public CLI subcommands — one of which authorizes the operator to delete source bytes.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan
- Tests to add/update: the full batched-cycle equivalence test above; deferred-vs-missing behaviour in both modes; consumed-source resume; size-changed completed source still an error; reordered inventory still refuses; finalize gating; release-command refusals.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: micro-Uchuu battery green; a manual two-batch cycle against a copied fixture dataset.

### Rollback Path
- Revert `scripts/convert/scatter.py` and `scripts/convert/convert_ctrees.py`. A manifest recording a deferred or consumed entry would not be understood by the reverted code, so rollback also requires discarding any workdir driven in batch mode — state that in the commit message.

---

## Slice 4: External merge-sort rank core (item 1, pure logic)

> **Machine:** 🟢 **Free** — synthetic inputs, and the slice's own acceptance bounds resident records by the configured budget. **The best slice in the plan to run beside a local model.**
>
> **Developer model:** **Opus, high effort** — external merge with exact tie-breaking and rank assignment is subtle even with the existing `lexsort` as an oracle.

### Intended Change
- `compute_identity()` builds five int64 columns concatenated over **all** snapshots, runs one global `np.lexsort`, then ranks within forest groups. Measured 187.84 B/halo against an analytic 48 B/halo — the excess is concatenation temporaries, `lexsort`'s internal copies and process residency. At 22.9 × 10⁹ halos that is **≈4.30 TB**.
- Add a self-contained external merge-sort core that produces the identical global ordering under a bounded memory budget: generate sorted runs from bounded chunks, spill them to disk, then k-way merge them while assigning `HaloRankInForest` in one streaming pass over the merged key order.
- This slice adds the core and its tests only. It does **not** rewire `compute_identity` — Slice 5 does that, so this slice's correctness can be judged against the existing in-memory implementation as an oracle.

### Acceptance Criteria
- Inputs: a sequence of per-snapshot key records `(forest_id, snap, upid, pid, id)` with a stable global position per record, and an explicit memory budget. Key order is `(forest_id ascending, snap descending, upid ascending, pid ascending, id ascending)` on post-fix values — the reference tree-driver order. All key fields are int64; behaviour on any other dtype is unspecified and must be rejected rather than coerced.
- Outputs: for every input record, its rank within its forest, and the per-forest group boundaries, identical to what the current `np.lexsort` path produces.
- User-visible behaviour: none yet — this slice adds no call site.
- Behaviour that must not change:
  - [ ] Nothing. No existing call site is modified in this slice.
- [ ] For randomised synthetic inputs, the core's ranks are **element-wise equal** to the ranks computed by the existing in-memory `lexsort` formulation.
- [ ] Equality holds when the memory budget forces many runs (at least: budgets producing 1, 2 and ≥ 8 runs over the same input).
- [ ] Equality holds for the degenerate cases: a single forest; every record in its own forest; ties in `upid`/`pid` broken only by `id`; a snapshot contributing zero records; a single record overall.
- [ ] Ranks are dense within each forest: for every forest the emitted ranks are exactly `0 … count-1`, each once.
- [ ] Resident record count during a merge is bounded by the budget and does not scale with total record count; the test asserts the number of records held, not wall clock.
- [ ] Peak spill bytes on disk are reported by the core so Slice 8 can include them in the storage envelope.
- [ ] Every spill file the core creates is removed by the core itself, on both success and failure paths, once the rank results have been written to their backing store and verified. **The core owns spill lifetime end to end**; no later slice deletes a spill file, and spills are not registered as manifest intermediates.
- [ ] The core is numpy + stdlib only.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/rank_sort.py` (new file)
  - `scripts/convert/tests/test_rank_sort.py` (new file)
- Functions/classes/components allowed to change: only the new module's own contents.
- Tests allowed or expected to change: only the new test module.

### Explicit Non-Goals
- Do not modify `links.py` in this slice.
- Do not change the key order, the rank definition, or `verify_identity`'s contract.
- Do not generalise the core into a reusable sorting library; it exists for this one key.
- Do not add a CLI flag.

### Risk Flags
- Risky surfaces touched: none — new file, no call sites.
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: the oracle-equality tests above, across budgets and degenerate shapes; a resident-record-count bound test; a spill-file cleanup test on a forced failure.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: none — this slice is judged by its tests.

### Rollback Path
- Delete the two new files; nothing else references them.

---

## Slice 5: Wire the rank pass onto the external core (item 1, integration)

> **Machine:** 🔴 **Full box — no local models.** Re-runs `links` over the 406,668,896-halo rehearsal workdir, whose baseline peak is **76.39 GB**, and the ≤24 GB target must be measured **warm and repeated** (Session D-prep saw ~17.7 GB of run-to-run variance at this scale from page cache alone). Also needs the LaCie clone and a second real dataset for the 4× comparison.
>
> **Developer model:** **Opus, high effort** — the highest-risk slice here. Every `UniqueGalaxyID` derives from this pass, and `verify_identity` is re-implemented rather than re-called.

### Intended Change
- Replace `compute_identity()`'s in-memory concatenate-and-lexsort with the Slice 4 core.
- **The return contract must change too, and this is half the memory problem.** `compute_identity` currently returns `{snap: (forest_index, ranks)}` — views into two int64 arrays covering every snapshot, 16 B/halo, ≈366 GB at production — and `run_links` passes that whole dict into `link_one_snapshot` for every snapshot. Replace it with an accessor backed by on-disk arrays so that only the current adjacent working set is resident.
- `forest_index` is `np.searchsorted(forest_table, forest)` and needs no global pass; compute it per snapshot. **The accessor must serve an adjacent pair, not a single snapshot**: `link_one_snapshot` passes `identity[snap]` and `identity[snap + 1]` to `verify_descendant_forests` (`links.py:536-540`), so a strictly one-snapshot accessor would break linking.
- **`verify_identity` must be re-implemented, not merely re-called.** It is the second half of the memory wall: it runs its own global `np.lexsort` over full-length `forest_index` and `ranks` plus several full-size derived arrays (`links.py:331-360`), and its ForestIndex-density condition is defined over the whole run, so calling it per snapshot is not equivalent. Replace it with an exact bounded verifier that preserves all three of its conditions — pair uniqueness, ForestIndex dense over `[0, n_forests)`, per-forest rank density — and its `ConverterError` message shapes including the five-example lists.
- Preserve the observed-forests-versus-`forest_index_table` reconciliation, including its "listed forests with no halos / observed forests unlisted" diagnostic with examples.

### Acceptance Criteria
- Inputs: an existing workdir whose snapshots are all at status `fixed` or `linked`.
- Outputs: `n_forests_total` and `max_halo_rank_in_forest` identical to the current implementation; per-snapshot `(forest_index, ranks)` identical element-wise.
- User-visible behaviour: `links` completes with the same manifest values and the same emitted dataset; peak RSS is bounded by the configured budget rather than by total halo count.
- Behaviour that must not change:
  - [ ] `_validate_monotonic_pairs` still runs before any ranking work.
  - [ ] The forest-table reconciliation still fires on both mismatch directions, with the same message shape and examples.
  - [ ] The identity verification still runs over the computed values and still asserts all three of its conditions with unchanged `ConverterError` message shapes. Its *implementation* changes — see Intended Change — but nothing it detects may be lost.
  - [ ] `run_links`' refuse-not-repair comparison against `manifest.data["links"]` still holds.
  - [ ] `link_one_snapshot` receives, for each snapshot, exactly the arrays it receives today, and its skip-trust path for an already-`linked` snapshot is unchanged.
  - [ ] Snapshot subsets remain unsupported; snapshots are still linked in ascending order.
  - [ ] The emitted dataset is **bitwise identical** to a pre-change run on the same input.
- [ ] `links` peak RSS at the 406,668,896-halo rehearsal scale is **≤ 24 GB** with the default budget, against the 76.39 GB baseline. The target is absolute and stated here rather than chosen after the fact. It is a ceiling, not a goal: a lower figure is better, and a figure above it fails the slice and must be explained before the budget default is changed to meet it.
- [ ] Memory does not scale with total halo count. The evidence is a fixed-budget comparison across **two real emitted datasets differing by at least 4× in halo count** — micro-Uchuu against the 406,668,896-halo rehearsal subset is the intended pair — not two budgets over one input and not synthetic fixtures, which need never exercise the spill and merge paths. Alternatively, instrument every resident buffer so a total-count-sized allocation cannot hide outside the merge.
- [ ] Peak on-disk bytes for the spill files and the identity backing arrays are reported, for Slice 8's envelope.
- [ ] The bitwise links comparison is run against a **fresh post-fixups, pre-links workdir** (a clone of the retained rehearsal workdir rolled back to `fixed` status, or a fresh conversion). Re-running `links` over an already-`linked` workdir is vacuous: `link_one_snapshot` verifies and returns without rewriting, so the comparison would compare files with themselves.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/links.py`
  - `scripts/convert/rank_sort.py`
  - `scripts/convert/convert_ctrees.py`
  - `scripts/convert/tests/test_links.py`
  - `scripts/convert/README.md`
- Functions/classes/components allowed to change: `compute_identity`, `verify_identity` (implementation and signature, not its assertions or messages), `run_links`, `link_one_snapshot`'s identity parameter, `_load_fixed` if streaming requires it, and the memory-budget control in the CLI.
- Tests allowed or expected to change: `scripts/convert/tests/test_links.py`.

### Explicit Non-Goals
- Do not change the link-building algorithms (`build_fof_chains`, `build_descendants`, `build_progenitor_links`) or the pending-buffer `FirstProgenitor` flow.
- Do not change `verify_descendant_forests`, and do not change what `verify_identity` asserts or how it reports — only how it computes it.
- Do not alter the `fixed` record dtype or the manifest schema.
- Do not "improve" the reference key order.

### Risk Flags
- Risky surfaces touched: the identity/rank pass that every `UniqueGalaxyID` derives from; a new public CLI flag for the memory budget.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: fixture-scale end-to-end equality of `identity`, `n_forests_total` and `max_halo_rank_in_forest` against recorded expectations; forest-table mismatch diagnostics in both directions; budget-invariance of the output; the halo-count-scaling memory evidence.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: run `links` on a rolled-back clone of `/Volumes/LaCie/convert/shin-uchuu-subset/`; compare emitted `snap_NNN_links.bin` byte-for-byte against the retained originals; record peak RSS, wall clock and peak spill bytes.

### Rollback Path
- Revert `links.py` and `convert_ctrees.py`; Slice 4's core becomes dormant but harmless. No on-disk format change, so a workdir remains usable by either version.

---

## Slice 6: Stream the producer validation battery (item 2a)

> **Machine:** 🔴 **Full box — no local models.** Runs the battery over the retained rehearsal dataset, baseline peak **73.27 GB**, and the bounded figure must be measured warm and repeated.
>
> **Developer model:** **Opus, medium effort** — a mechanical windowing transformation, but the failure mode is silently weakening a producer check, and the injected-defect equivalence tests carry most of the risk.

### Intended Change
- `validate.py`'s `load_dataset` reads **every** snapshot file's complete `/halos` arrays into memory before any check runs, then passes the whole list to each check: measured **73.27 GB** on a 1.8% subset, and unbounded at production. Producer validation is part of the format contract and cannot be skipped.
- Convert the battery to a bounded window. The widest per-record data dependency among the checks is an adjacent pair `(snap, snap+1)` — `check_link_ranges` (which needs only the neighbour's record count) and `check_progenitor_closure` (which loads the adjacent pair) — so a two-snapshot window covers every per-record check.
- **Three checks are global in scope and each needs its own bounded treatment**, and naming them is the point of this slice:
  - `check_count_conservation` sums emitted halos across all snapshots — a running accumulator reproduces it exactly.
  - The run-scoped header comparison checks scalar header agreement across all files — a carried scalar reproduces it exactly.
  - `check_identity` is the hard one, below.
- **`check_identity` enforces three independent conditions, not one**, and the streaming reformulation must retain all three (`validate.py:616-663`): (a) `ForestIndex` dense over `[0, n_forests_total)`, a cross-forest property; (b) per-forest ranks dense and unique over `0 … count-1`; (c) the measured maximum rank equal to the header's `max_halo_rank_in_forest`.
- **Exactness is required and constrains the mechanism.** Recognising an arbitrary streamed multiset as exactly `{0 … count-1}` cannot be done with O(1) state per forest — sums, XORs, hashes and extrema all admit collisions — and a probabilistic filter would weaken the format contract. An **exact** structure sized by halo count is therefore permitted and expected: a bitset indexed by `forest_offset[forest] + rank` after a counting pass costs 1 bit per halo (≈2.86 GB at 22.9 × 10⁹ halos), which is bounded and affordable. An external ordering of `(ForestIndex, rank)` is an equally acceptable alternative. What is forbidden is dropping a condition or approximating one.

### Acceptance Criteria
- Inputs: an emitted dataset directory and the same arguments the battery takes today, including `--multiplier`.
- Outputs: for any dataset, the **same set of check outcomes and the same failure messages** as the current implementation.
- User-visible behaviour: identical PASS/FAIL/SKIP lines and exit code; resident memory bounded by the window plus the identity structure rather than by the dataset.
- Behaviour that must not change:
  - [ ] Every check that exists today still runs, under the same name, in the same order.
  - [ ] All three `check_identity` conditions (a), (b) and (c) above are still enforced, each still able to fail independently.
  - [ ] Structural-conformance failure still causes the semantic checks to SKIP with the existing message.
  - [ ] Failure messages keep their existing shape, including example lists and their truncation.
  - [ ] `check_manifest_binding` still compares the directory against the recorded paths.
  - [ ] `check_header_bounds` still validates against the supplied multiplier, not a default.
  - [ ] Exit codes are unchanged for pass, fail and input-error cases.
- [ ] On a dataset with a deliberately injected defect of each detectable class — including one defect per `check_identity` condition — the streaming battery reports the same failure as the current battery.
- [ ] Resident (in-RAM) bytes are bounded by the two-snapshot window plus forest-count-sized metadata plus the identity structure, asserted by a test rather than by observation.
- [ ] Any halo-count-sized identity structure is exact, is disk-backed or bit-packed, has its peak bytes reported for Slice 8's envelope, and is removed on both success and failure paths.
- [ ] Peak RSS on a full dataset is recorded against the 73.27 GB baseline as a measurement.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/validate.py`
  - `scripts/convert/tests/test_validate.py`
  - `scripts/convert/README.md`
- Functions/classes/components allowed to change: `load_dataset` and the check functions' signatures, `check_identity`, `check_count_conservation`, the run-scoped header check, and the battery driver.
- Tests allowed or expected to change: `scripts/convert/tests/test_validate.py`.

### Explicit Non-Goals
- Do not add, remove or rename a check.
- Do not weaken any check to make streaming easier, and do not substitute a probabilistic structure for an exact one.
- Do not change the emitted format or the manifest.
- Do not touch `crosscheck.py` (Slice 7).

### Risk Flags
- Risky surfaces touched: the producer validation battery is part of the format contract; weakening it silently would remove the converter's own safety net.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: injected-defect equivalence per check class and per identity condition; resident-bytes bound assertion; identity-structure cleanup test.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: run the battery on the retained rehearsal dataset; diff its full output against the recorded Session C output; record peak RSS.

### Rollback Path
- Single-file revert of `scripts/convert/validate.py`. No on-disk change.

---

## Slice 7: Stream the topology cross-check (item 2b)

> **Machine:** 🔴 **Full box — no local models. The single largest memory demand in this plan.** `crosscheck compare` peaks at **251.32 GB** at rehearsal scale — about half the machine — and the retained artifacts it reads are 141 GB on LaCie.
>
> **Developer model:** **Opus, high effort** — two exact bounded mechanisms must be designed from scratch, for global state the current code holds by growing an array.

### Intended Change
- `crosscheck.py compare` loads the emitted dataset, the full reference galaxy output (**109.7 GB**, 13 partitions) and the entire 42 GB topology dump simultaneously: measured **251.32 GB peak on a 1.8% subset**, with a 229.5 GB transient during `np.loadtxt` of the dump. It is the converter's own acceptance instrument (D10) and must be bounded, because 251 GB for 1.8% of a box is absurd at any scale.
- **What this slice does NOT promise, and the recorded scope was wrong about this.** `HANDOFF.md` says the cross-check must become "runnable at production scale". It cannot be, and no amount of comparator streaming changes that: the cross-check's reference side is a **tree-ordered `halos-only` run over the same data** (`crosscheck.py run-reference`), and the tree driver loads a forest as one in-memory unit, so the production super-forest alone projects to ≈15.9 TB of reader preallocation (C5/C6 of the rehearsal handoff). The reference artifact cannot be produced at production scale, independently of memory in `compare`. Storage says the same thing: scaling the measured 109.7 GB reference output and 42 GB dump by the 56.3× production ratio gives ≈6.2 TB and ≈2.4 TB, which do not fit the conversion volume either.
- **The binding cross-check gate is therefore micro-Uchuu**, exactly as `SHIN-UCHUU-CONVERSION-PLAN.md`'s Definition of Done item 2 states, with the rehearsal subset as the largest scale it is ever run at. This slice delivers a bounded-memory comparator, not a production-scale instrument, and production cross-check artifacts are **not** part of Slice 8's storage envelope.
- Restructure `compare` around a per-snapshot window: build one snapshot's `SnapMatch`, run the checks against it, accumulate failures, release it.
- **Two pieces of genuinely global state block a naive per-snapshot rewrite, and this slice owns both:**
  - `check_identity_creation` accumulates every previously seen `UniqueGalaxyID` across snapshots (`seen = np.union1d(seen, t01_ugid)`, `crosscheck.py:372-403`). A one-snapshot window does not bound it. It needs an exact bounded membership mechanism — a disk-backed sorted structure, an external join, or a bit-packed presence set over the dense identity domain. A growing in-memory set is not acceptable, and neither is a probabilistic filter.
  - `load_reference_topology_dump` materializes the whole dump with `np.loadtxt` and then builds a full global sort permutation before any per-snapshot processing (`crosscheck.py:651-698`, `:751-781`). It needs per-snapshot partitioning or indexing so that only one snapshot's rows are resident.

### Acceptance Criteria
- Inputs: the same `prepare` / `run-reference` / `compare` interface and arguments, including `--multiplier`.
- Outputs: the same seven checks plus `reference-sanity`, the same outcomes, the same `crosscheck_report.json` content for a given input.
- User-visible behaviour: identical check results and exit code; resident memory bounded by the window plus the bounded global structures.
- Behaviour that must not change:
  - [ ] All eight reported outcomes (`reference-sanity`, `identity-forest`, `identity-creation`, `fof-central`, `flyby-signs`, `values`, `occupancy`, `topology-chains`) still run and report identically.
  - [ ] `check_identity_creation`'s **suppression** semantics are reproduced exactly. The check decodes only IDs appearing for the first time — `candidate = ~np.isin(t01_ugid, seen) & (match.matched >= 0)` (`crosscheck.py:388`) — because a galaxy that persists across snapshots legitimately keeps its `UniqueGalaxyID`, and re-checking it would be noise, not signal. The bounded replacement for `seen` must exclude exactly the same galaxies at exactly the same snapshots. Proved by a test with a galaxy persisting across several snapshots, asserting it is decoded once at first appearance and never again.
  - [ ] The coverage assertion that the dump accounts for every halo of every snapshot still holds and still fails loudly when the dump is truncated.
  - [ ] The dump reader still rejects the appended-re-run case it currently guards against, rather than silently skipping lines.
  - [ ] `_validate_reference_dtype` still rejects an unexpected reference dtype.
  - [ ] `crosscheck_report.json` keeps its schema, its `"passed"` semantics and its report ordering.
  - [ ] Exit codes are unchanged.
- [ ] Resident bytes are bounded by the per-snapshot window plus the two bounded global structures, asserted by a test.
- [ ] Any on-disk partition or index the slice creates is removed on both success and failure paths, and its peak bytes are recorded. **They are recorded for this slice's own footprint, not for Slice 8's production envelope**: no production cross-check runs, so no cross-check byte belongs in the conversion's storage ceiling.
- [ ] A truncated dump and a wrong-multiplier run each still fail, verified by test.
- [ ] Peak RSS is recorded against the 251.32 GB baseline, like for like on the rehearsal subset. **Verified present 2026-08-26**: `/Volumes/LaCie/rehearsal/crosscheck/` holds 141 GB (`reference-output`, `topology.dump`, `topology-scratch-output`), so this comparison is available and is required. Should those artifacts be deleted before this slice runs, record the comparison as unavailable and rely on the bounded-state assertions — a micro-Uchuu figure is not comparable to a 1.8%-subset baseline.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/crosscheck.py`
  - `scripts/convert/tests/test_crosscheck.py`
  - `scripts/convert/tests/mock_reference.py`
  - `scripts/convert/README.md`
- Functions/classes/components allowed to change: `load_reference_galaxies`, `load_reference_topology_dump`, `build_matches`, `check_identity_creation`, the other check functions' signatures, and `run_crosscheck`.
- Tests allowed or expected to change: `scripts/convert/tests/test_crosscheck.py`, `scripts/convert/tests/mock_reference.py`.

### Explicit Non-Goals
- Do not change what any check asserts.
- Do not change the dump format or the `dump-ctrees-topology-tool` harness.
- Do not weaken the coverage assertion to accommodate streaming.
- Do not substitute a probabilistic membership structure for an exact one.
- Do not touch `validate.py` (Slice 6).

### Risk Flags
- Risky surfaces touched: the converter's acceptance instrument — a cross-check that passes vacuously is worse than none.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: per-check equivalence against the current implementation on fixture data; the persistent-galaxy suppression test above; truncated-dump and wrong-multiplier failure tests; resident-bytes bound assertion.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: re-run `compare` against the retained rehearsal cross-check artifacts if they exist, otherwise against a regenerated micro-Uchuu reference; record peak RSS.

### Rollback Path
- Single-file revert of `scripts/convert/crosscheck.py` plus its test changes. No on-disk change.

---

## Slice 8: Consumptive deletion of stage intermediates (item 6)

> **Machine:** 🟡 **Quiet — disk and I/O, not memory.** The micro-Uchuu battery runs at modest memory, but this slice measures a stage-by-stage storage envelope, so it needs LaCie headroom and an uncontended disk. Local models are fine on memory; keep other large I/O off the box.
>
> **Developer model:** **Opus, high effort** — irreversible deletion with crash-recovery semantics. Approval-gated, independent audit required.

### Intended Change
- Deletion stops after the concat stage: `scatter.py` consumes its worker parts and `sort_index.py` consumes the unsorted scratch, but `fixups.py`, `links.py` and `hdf5_writer.py` delete nothing. Measured, the coexisting set after `links` is `sorted` 108 + `idx` 8 + `fixed` 120 + `links` 36 + `pending_fp` 4 = **277 B/halo**, i.e. **6.34 TB** at production, before this plan's own new intermediates are counted.
- Add delete-after-verify to the stages that consume an intermediate and no longer need it, following the protocol stated once in *Conventions* above, including a `_retry_*_cleanup` for a crash between unlink and manifest save.
- **Freeze the deletion table against the real consumer sets, because two intermediates have more than one consumer.** `fixed` is read by `links` (`_load_fixed`, via `compute_identity`) **and** by the writer (`_load_snapshot_scratch`, `hdf5_writer.py:308-334`, called at `:428`); `links` is likewise read by the writer. Neither may be deleted inside `links`. The writer is the terminal consumer of both.
- **Deletion is additionally bounded by re-run reachability, not by last read**, because a consumed input must still leave its stage resumable. The two places that breaks today, and the exact fix for each, are named in the deletion table and the Authorized Surface below.
- Deletion is **opt-in via a CLI flag defaulting to off**. Destroying intermediates on a multi-day, no-resume run must be a deliberate operator choice.

### Acceptance Criteria
- Inputs: an existing workdir and an explicit opt-in flag.
- Outputs: the same emitted dataset; a workdir whose peak footprint meets an absolute ceiling.
- User-visible behaviour: with the flag off, byte-for-byte the current behaviour including every retained intermediate. With it on, predecessors are removed once their terminal consumer is done with them.
- Behaviour that must not change:
  - [ ] With the flag off, no intermediate is deleted that is not already deleted today.
  - [ ] Every deletion goes through `manifest.remove_intermediate`, never a bare `unlink`.
  - [ ] A crash between unlink and manifest save converges on the next run to "removed", not to an error.
  - [ ] Re-running a stage whose inputs were consumed and whose outputs are complete is a **skip**, with a message naming what was consumed — not a stat failure or a checksum error.
  - [ ] `run_links`' refuse-not-repair comparison of run-scoped identity values still holds wherever a `links` re-run remains reachable.
  - [ ] The emitted dataset is bitwise identical with the flag on and off.
- [ ] The plan's deletion table is implemented exactly and enumerated in the commit message:
  - `sorted_N` once snapshot N's `fixed` output is verified and registered. **This reaches into `sort_index.py`**: its skip-trust path re-verifies `sorted_file` whenever a snapshot is already at `sorted` or `fixed` status (`sort_index.py:40-49`), so once `sorted_N` is deleted a re-run of sort fails on a file the pipeline deliberately consumed. That path must treat a recorded, verified consumption as a skip rather than a missing artifact.
  - **`idx_N` only once snapshot N−1 has been linked**, not when `fixed_N` is verified. `link_one_snapshot(snap)` verifies and loads snapshot `snap + 1`'s `index_file` to resolve descendants (`links.py:522-525`), so `idx_N`'s consumer is `link_one_snapshot(N-1)` and deleting it earlier fails linking at the first descendant-bearing snapshot. The boundaries follow from that relation and are the opposite way round from the intuition: the highest snapshot's index is consumed normally, by the snapshot below it, while **any `idx_N` whose `N−1` has no manifest entry has no consumer at all** — linking iterates only recorded snapshots (`links.py:624`) — and is deletable as soon as linking starts. `idx_0` is always such an index; a gap in the recorded snapshot set creates others.
  - `pending_fp_N` once the snapshot that consumes it is linked.
  - `fixed_N` and `links_N` once snapshot N's emitted HDF5 is verified and recorded — **the writer is the terminal consumer of both** (`hdf5_writer.py:308-334`, called at `:428`), so neither may be deleted inside `links`.
  - identity backing arrays once `links` completes. Rank spill files are **not** in this table: Slice 4's core owns them end to end.
- [ ] Deleting only a subset of that table is a failure of this slice, not a partial pass. A test asserts each listed intermediate is actually gone at the stated point.
- [ ] **An absolute storage ceiling is met and measured, not merely "lower than before".** The envelope covers every converter-owned byte coexisting at any instant — the staged source batch, workdir intermediates, rank spill files (Slice 4), identity backing arrays (Slice 5), any validation index (Slice 6), and the emitted dataset — and the projected production peak must fit the 7.37 TB primary volume with headroom, i.e. **≤ 7.0 TB**. Record the stage-by-stage envelope, not just the maximum.
- [ ] The recorded envelope names the three preconditions it assumed, because the ceiling is not satisfiable without them, and the measured maximum staged-batch size is recorded as a number the transfer plan can be held to: (a) **deletion enabled** — with the flag off the plan's own arithmetic is ≈9.9 TB at production and the ceiling is unreachable by design; (b) **a bounded staged source batch**, since during scatter the current batch coexists with the accumulating per-snapshot binaries — record the maximum batch size the envelope admits; (c) **cross-check artifacts excluded**, per Slice 7: the production cross-check is not run, so its reference output and dump are not in this envelope.
- [ ] The measured peak workdir bytes/halo with the flag on is recorded against the 277 B/halo baseline.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/fixups.py`
  - `scripts/convert/links.py`
  - `scripts/convert/hdf5_writer.py`
  - `scripts/convert/sort_index.py`
  - `scripts/convert/convert_ctrees.py`
  - `scripts/convert/scatter.py`
  - `scripts/convert/tests/test_fixups.py`
  - `scripts/convert/tests/test_links.py`
  - `scripts/convert/tests/test_hdf5_writer.py`
  - `scripts/convert/tests/test_sort_index.py`
  - `scripts/convert/README.md`
- Functions/classes/components allowed to change: the per-snapshot completion paths in `fixups`, `links` and the writer; `sort_one_snapshot`'s skip-trust path so a consumed `sorted_N` is a skip rather than a verification failure; a short-circuit in `run_links` ahead of `compute_identity`, **conditioned on the `fixed` inputs being recorded as consumed** — not on every snapshot merely being `linked`, which would drop the refuse-not-repair comparison that runs today whenever those inputs are still present; a shared retry-cleanup helper; `Manifest.remove_intermediate` and any shared deletion helper on the class (which is why `scatter.py` is authorized — `class Manifest` is defined there, at `scatter.py:246`); the CLI flag in `convert_ctrees.py`.
- Tests allowed or expected to change: the four named test modules.

### Explicit Non-Goals
- Do not delete anything the producer validation battery or the topology cross-check still needs.
- Do not delete source tree files — Slice 3's release command owns that boundary.
- Do not change any stage's data-producing algorithm or its emitted content. The skip, resume and short-circuit paths named in the Intended Change are explicitly in scope — changing them is how a consumed input stays resumable.
- Do not make deletion the default.

### Risk Flags
- Risky surfaces touched: irreversible data deletion, crash-recovery semantics, resume paths, a new public CLI flag.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan
- Tests to add/update: flag-off retains everything; flag-on deletes each table entry at its stated point; crash-between-unlink-and-save converges; re-running a consumed stage skips with a clear message; emitted dataset identical under both flag states.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests`; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: full micro-Uchuu battery with the flag on and off, comparing emitted datasets; record the stage-by-stage storage envelope in both.

### Rollback Path
- Revert the six authorized source files. Because deletion is opt-in and off by default, a workdir produced with the flag off is unaffected; a workdir produced with it on has lost intermediates and must be re-run from the last surviving stage — state that in the commit message.

---

## Slice 9: Pass acceptance gate and documentation

> **Machine:** 🔴 **Full box — no local models, and the longest slice.** Re-measures everything: `links` at 406,668,896 halos, the battery, and the cross-check against its **251.32 GB** baseline, each **warm and repeated**, plus a fresh end-to-end micro-Uchuu conversion.
>
> **Developer model:** **Opus, high effort** — the judgement call is whether the measurements actually support closing JR §6 item 7, which is a scientific-evidence decision, not a coding one.

### Intended Change
- Run the pass's own acceptance gate — the full micro-Uchuu producer validation battery **and** the topology cross-check, both green, proving the converter's reference semantics did not move while its machinery was rebuilt — plus a measured memory profile of the rank pass at Shin-Uchuu subset scale.
- **The gate must run against a dataset this pass actually produced.** `validate` and `crosscheck` both load an existing dataset from disk, so running them over the Session C output would go green without executing a single line of changed converter code. Slice 9 therefore begins with a fresh end-to-end micro-Uchuu conversion, in a new workdir, using the post-Slice-8 code.
- Record the measured outcomes in this plan, in `SHIN-UCHUU-CONVERSION-PLAN.md`'s pre-conversion-obligation section, in `POST-PHASE-5-JOINT-REVIEW.md` §6 item 7 (closing the item in the style of the already-closed items), and in `HANDOFF.md` §7's ledger.
- Update `scripts/convert/README.md`'s "Shin-Uchuu-scale notes", which currently describes every one of these limits as a deferred production concern.

### Acceptance Criteria
- Inputs: the committed tree with Slices 1–8 landed.
- Outputs: recorded, reproducible measurements and a closed checklist item.
- User-visible behaviour: none — this slice changes documentation and records evidence.
- Behaviour that must not change:
  - [ ] No source file under `scripts/convert/` changes in this slice except `README.md`.
- [ ] A **fresh** micro-Uchuu conversion is run end to end in a new workdir with the post-Slice-8 code, and the battery and cross-check are run against that newly emitted dataset.
- [ ] The full micro-Uchuu validation battery runs green.
- [ ] The topology cross-check runs green, all seven checks plus `reference-sanity`, zero unexplained mismatches.
- [ ] The `links` stage's peak RSS and B/halo at 406,668,896 halos are measured and recorded against the 76.39 GB / 187.84 B/halo baseline, with the implied production projection.
- [ ] The stage-by-stage storage envelope from Slice 8 is recorded, with the production projection against the 7.0 TB ceiling.
- [ ] Scatter throughput is re-measured and recorded against the 39.0 MB/s baseline.
- [ ] `validate` and `crosscheck` peak RSS are recorded against their 73.27 GB and 251.32 GB baselines, subject to Slice 7's like-for-like availability caveat.
- [ ] Every measurement is taken **warm and repeated**, per the Session D-prep page-cache finding: run-to-run variance of ~17.7 GB was observed at Shin-Uchuu scale, so a single cold figure is not evidence.
- [ ] `make check-docs` and `make check-format` pass.

### Authorized Surface
- Files allowed to change:
  - `docs/dev/CONVERTER-SCALE-PASS-PLAN.md`
  - `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`
  - `docs/dev/POST-PHASE-5-JOINT-REVIEW.md`
  - `docs/dev/POST-PHASE-5-WORK.md`
  - `HANDOFF.md`
  - `scripts/convert/README.md`
- Functions/classes/components allowed to change: none — documentation only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- Do not fix code defects discovered here inside this slice; record them and open a new slice.
- Do not start the production transfer or conversion.
- Do not close any checklist item the measurements do not actually support.

### Risk Flags
- Risky surfaces touched: the documentation of record for a production decision.
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: none.
- Commands to run: a fresh micro-Uchuu conversion; the battery and cross-check against it; `make check-docs`; `make check-format`; `./scripts/beautify.sh`.
- Lint (differential, via the `lint` skill): required if any linted file changes.
- Manual checks: re-read each edited document for internal consistency against the measurements recorded.

### Rollback Path
- Revert the documentation commit; no runtime effect.

---

## What this pass deliberately does not do

- **It does not touch the fix-up stage's satellite scan** (item 5, answered: retain).
- **It does not touch `fix_flybys`**, which is the larger per-snapshot term where demotions are heavy (7.92 s for 1,194,990 demotions at snapshot 69) but projects to only ~5 min at production.
- **It does not change the emitted format.** `SNAPSHOT-HDF5-FORMAT.md` `format_version = 1` is frozen; nothing here touches it.
- **It does not set the production identity multiplier.** That is pathway step P3: raise both `simulations/shin-uchuu*/simulation_info.yaml` to **2 × 10¹⁰**, confirmed against the production conversion report, with no re-conversion needed.
- **It does not run the conversion.** Steps P1–P8 follow this pass.

---

## Next Chat Prompt

```md
Plan file: docs/dev/CONVERTER-SCALE-PASS-PLAN.md
Slices or batch this session: Batch A (Slices 1–2)

Read the full plan file first. If a selected slice or batch receipt is incomplete or the plan state is unclear, stop and tell me before coding.

Work on the current feature branch for this plan; if none exists, create one and tell me the name.

Use orchestrator as the controlling skill. Act as the Developer: keep implementation, validation, Git operations, and commits local. Use a read-only Reviewer only for investigation, evidence gathering, the hostile drift-audit skill, and an independent code-review skill pass. If no Reviewer is configured or available, perform Developer self-audit and record that provenance explicitly.

For each selected slice or batch, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. If any included slice's Risk Flags mark approval-needed, stop and get my approval before coding.
3. apply the scoped-implementation skill against the selected contract.
4. apply the drift-audit skill using a read-only Reviewer when available; otherwise perform Developer self-audit. Report the authorization gate result and who performed it before any quality review.
5. If the gate passes: for a broad or structural change, first run the code-health skill differentially against the slice's starting commit and supply its report as review evidence. Then apply the code-review skill using a read-only Reviewer when available; otherwise perform Developer self-audit through the code-review skill. Record who performed it. If the drift gate fails, fix the drift and re-audit.
6. Surface drift and review findings to me, fix them, then re-run the relevant gate. If consecutive reviews return only minor findings and have clearly converged record residuals in the slice summary and proceed.
7. Ask me before committing. On my approval, commit the selected slice or batch with the commit skill.

After the selected slice(s) or batch are committed, use the handoff skill to record state, audit provenance (Reviewer tool/label or Developer self-audit and fallback context), and the next slice or batch to resume from. Do not continue past the selected scope.

Confirm before starting: plan file read, selected slice(s) or batch, branch, and the first slice. Then begin.
```
