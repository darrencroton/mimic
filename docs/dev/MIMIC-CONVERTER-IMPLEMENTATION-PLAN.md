# Ctrees-ASCII → Snapshot-HDF5 Converter Implementation Plan

**Status:** Frozen implementation plan for pathway item 2 (converter build + micro-Uchuu validation). Reviewed by Codex `gpt-5.6-sol` (high reasoning effort, read-only sandbox) on 2026-07-18; all 11 findings (3 Critical, 6 Major, 2 Minor) verified against the repo and incorporated below.
**Date:** 2026-07-18
**Contract inputs:** `docs/SNAPSHOT-HDF5-FORMAT.md` (frozen on-disk contract, `format_version = 1`) and `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md` (algorithm, phases, reference semantics, resolved decisions D1–D12).
**Scope:** Build the external converter under `scripts/convert/` and pass its acceptance gate on micro-Uchuu ASCII. The Shin-Uchuu production conversion, the snapshot reader (dual-driver Phase 4b), and any production `src/` change are explicitly out of scope. Slice 10 (optional, approval-gated) is the one deliberate exception to the "no new Mimic C code" sequencing: a read-only test-tier topology dump harness — see the Topology Proof section.

---

## Context and Discovery Record

Facts verified against the live repo on 2026-07-18 (initial discovery plus review-driven verification); slices below rely on them.

**Environment.** `mimic_venv` is Python 3.13.2 with `numpy 2.3.4` and `h5py 3.15.1` installed; **`pandas` and `pytest` are NOT installed**. Dependencies are managed through the root `requirements.txt` (installed by `scripts/first_run.sh`), floor-pinned (`numpy>=1.20.0` style). Converter unit tests use stdlib `unittest`; the Phase 1 parser needs `pandas`, which is Slice 1's isolated, approval-gated dependency change. Python style: black + isort, line length 100, must run under 3.9+.

**Fixture data.** `simulations/micro-uchuu-ascii/` is a registered package whose `snapshots/` symlink points at local data: one `tree_0_0_0.dat` (12 GB), `forests.list` (10 MB), `locations.dat` (21 MB). 50 snapshots (`micro-uchuu.a_list`), box 100 Mpc/h, `particle_mass: 0.0325` in 1e10 Msun/h (so the header attribute `particle_mass_msun_h` is `3.25e8` — unit conversion is explicit, never implicit). `models/halos-only/input/halos-only_micro-uchuu-ascii.yaml` exists, outputs HDF5, and lists 8 snapshots — the cross-check runner generates a scratch copy listing all 50 snapshots rather than editing the repo run file.

**Real ctrees header format (review finding 1, verified `head -1`).** Micro-Uchuu tree files begin with an **indexed** header line — `#scale(0) id(1) desc_scale(2) … Snap_num(31) …` — not a `#fields:` line. Column names carry `(N)` suffixes and mixed case. The reference parser strips the suffixes and matches names case-insensitively (`src/io/tree/ctrees/parse_ctrees.h`), and accepts both `snap_idx` and `snap_num` spellings (`read_ctrees_ascii.c` `setup_column_info`). The converter parser must do the same; the conversion plan's `#fields:` description is corrected by an erratum note in that plan.

**Reference semantics live in:** `src/io/tree/read_ctrees_ascii.c` (`setup_column_info`; `apply_ctrees_value_conventions` — spin J/Mvir with the Mvir==0 carve-out, float64 intermediates cast back to float32; `convert_ctrees_to_lht` — MostBoundID from ctrees `id`, Len derivation with finiteness/negativity/INT_MAX validation and C `round()` half-away-from-zero) and `src/io/tree/ctrees/ctrees_utils.c` (`fix_flybys` — **aborts on zero centrals at the forest's max scale**, returns unchanged on exactly one, demotes on multiple; `fix_upid` — resolved satellites get **both `upid` and `pid`** set to the ultimate central's id; `assign_mergertree_indices` and its sort keys). The conversion plan's Phase 3 pseudocode is the authoritative algorithm statement; where this plan is more precise (review findings 4–5), this plan wins.

**Cross-check design (review finding 2 corrected).** `UniqueGalaxyID` is a **persistent creation-halo identity**, not a per-snapshot halo label: `copy_progenitor_galaxy()` in `src/core/inheritance.c` copies the whole `struct Halo` (including `UniqueGalaxyID`) to the descendant, `apply_descendant_properties()` refreshes current-halo properties like `MostBoundID` without touching the ID, and only `init_new_halo()` — reached when a FoF central inherits **zero** progenitor galaxies — stamps a fresh `encode(halonr, forestnr)` (`core_properties.yaml` UniqueGalaxyID description; `build_model.c:282`; `TREE_MUL_FAC = 10⁹` in `constants.h`). The cross-check therefore verifies, per snapshot, matching Type 0/1 galaxies to converter halos by `|MostBoundID|`:

1. **Forest component, all matched galaxies:** `decode_forest(UniqueGalaxyID)` equals the converter `ForestIndex` of the current halo (galaxies never change forest).
2. **Full (forest, rank) decode, creation subset:** for every galaxy whose `UniqueGalaxyID` first appears at snapshot N (all 50 snapshots are in the output, so first appearance = creation), the decode equals the converter `(ForestIndex, HaloRankInForest)` of its matched halo at N. This directly validates reference rank order on every lineage-starting halo.
3. **FoF central assignment:** for each matched galaxy, locate the Type 0 galaxy whose `UniqueGalaxyID == UniqueCentralGalaxyID` (central-ID propagation, `build_model.c`); that galaxy's current `|MostBoundID|` must equal the ctrees id of the converter's `FirstHaloInFOFgroup` target for the matched halo.
4. **Flyby set:** the set of negative `MostBoundID` values matches exactly per snapshot.
5. **Values:** `Pos`/`Vel`/`Spin`/`VelDisp`/`Vmax`/`Mvir`/`Len` under the frozen comparison rules (below).
6. **Unmatched halos:** the set of converter halos with no matched Type 0/1 galaxy must equal exactly the set predicted by the reference occupancy rule computed from converter links — `occupied(H) = any(progenitor of H occupied) OR H is FoF central` (forward induction over snapshots, from `inheritance.c` creation semantics). Reference-side galaxies with no converter halo must number zero. No free-form explanations: exact set equality, with counts and example ids on failure.

**Frozen comparison rules (review finding 9):** integer, link-derived, and sign facts compare exactly. float32-carried fields (`Pos`, `Vel`, `Spin`, `VelDisp`, `Vmax`) compare bit-exact; any mismatch is stop-and-report (a parse-path ULP difference is a finding to investigate, never a tolerance to widen silently). Derived fields compare exactly after replicating the documented reference arithmetic (`Len` integer-exact; `Mvir` after the ×1e-10 conversion in float64 cast to the output dtype).

**Topology proof status (review finding 3).** Checks 1–6 pin identity components, forest enumeration, rank order (on the creation subset), FoF central resolution, flyby demotion, and value conventions — but they do **not** directly compare `FirstProgenitor`/`NextProgenitor`/`NextHaloInFOFgroup` chain order against the reference reader. Rank equality constrains the underlying sort but does not prove chain construction. Two closure paths, decided at Slice 10's approval gate:

- **With Slice 10 (recommended):** a minimal read-only test-tier C harness dumps the reference reader's per-halo topology (id, descendant id, ordered progenitor chain, ordered FoF chain, forestnr, rank) on micro-Uchuu; `crosscheck.py` compares chains directly by stable id. Pathway item 2's topology gate is then fully discharged before any snapshot-reader work.
- **Without Slice 10:** the plan's gate is explicitly **partial** — chain-order conformance remains unproven until the dual-driver Phase 5 cross-format identity gate, and the pathway status update in Slice 9 must say so in those words.

**Repo state note:** pathway item 1 (the frozen spec) is committed as `ac50eafb`. This plan file and `.orchestrator/` reviewer evidence are the only uncommitted artifacts at plan-freeze time.

**Converter package shape** (established by Slice 2, filled in by later slices):

```
scripts/convert/
  README.md              tool overview, usage, workdir layout, pointer to the frozen contract
  convert_ctrees.py      CLI: per-phase subcommands + full pipeline; --workdir, --simulation-info,
                         --a-list; resume manifest
  ctrees_parser.py       Phase 1 parsing: indexed/#fields headers, #tree markers, chunked reads,
                         frozen record dtype, independent row pre-count
  scatter.py             Phase 0 pre-pass + Phase 1 scatter/concat/manifest/aggregates
  sort_index.py          Phase 2 per-snapshot sort + id index
  fixups.py              Phase 3 fix-ups: adjacency, conventions, fix_flybys/fix_upid equivalents
  links.py               Phase 3 links: FoF chains, descendant/progenitor remap, ranks, identity
  hdf5_writer.py         snapshot_NNN.h5 + forests.h5 emission per the frozen contract
  validate.py            Phase 4 producer validation battery (CLI)
  crosscheck.py          micro-Uchuu topology cross-check vs a halos-only reference run
  report.py              conversion report emission
  tests/                 unittest suite + synthetic ctrees fixture generator + tiny committed fixtures
```

All scratch and output artifacts live under a user-supplied `--workdir` (suggested: `output/convert/micro-uchuu/` — inside the repository's gitignored `output/` run-output area, review finding 8). **The converter never deletes source data in this plan.** Cleanup is restricted to manifest-owned intermediate files the converter itself created: the manifest records each intermediate's absolute path and checksum, and deletion refuses any path not recorded there or not under the workdir. Consumptive source deletion is a Shin-Uchuu production concern, deferred to its own future approval-gated slice.

**Canonical metadata inputs (review finding 6, open question resolved):** the converter takes explicit CLI paths — `--simulation-info` (a `simulation_info.yaml`) and `--a-list` — rather than resolving a registered package itself, keeping it simulation-agnostic. For the micro-Uchuu gate these point at the package files. Observed `(SnapNum, scale)` pairs from the data are cross-validated against the a_list (absolute tolerance 1e-4; unknown pair aborts); snapshots present in the a_list but empty in the data still get (empty) snapshot files, as the contract requires. Header attributes derive from `simulation_info.yaml` with explicit unit conversion (`particle_mass_msun_h = particle_mass[1e10 Msun/h] × 1e10`); the Len formula keeps the 1e10-units value. The observed-pair table is emitted in the conversion report (it is how a future Shin-Uchuu a_list gets drafted, but drafting is not this plan's job).

---

## Implementation Profiles

- Recommended for frontier/senior implementer: Slice 1 alone (approval-gated), then Batch A, then Slices 5 and 6 individually, then Batch B, then Slice 10 decision + Slice 9.
- Recommended for standard implementer: run slices individually in order; confirm the Batch A contract explicitly before batching.
- Recommended for weaker implementer: atomic slices one at a time; do not batch; treat Slices 5–6 and 8 as requiring the strongest available model (scientific-correctness-critical).

## Slice Batches

- Batch A: Slices 2–4 — mechanical data plumbing (parser, scatter, sort) sharing one synthetic-fixture test suite; one review can see the whole scratch-file lifecycle.
- Batch B: Slices 7–8 — HDF5 emission + validation battery, then the cross-check implementation that consumes contract-conformant fixture files; both are testable on synthetic fixtures without the real-data run.

---

## Slice 1: Add pandas dependency

### Intended Change
- Add `pandas>=2.0.0` to `requirements.txt` (floor-pin style matching existing entries) and install it into `mimic_venv`.

### Acceptance Criteria
- Inputs: none.
- Outputs: `requirements.txt` gains exactly one line; `mimic_venv/bin/python -c "import pandas"` succeeds.
- User-visible behaviour: none (tooling dependency only).
- Behaviour that must not change: existing scripts, build, and test suites are unaffected; no other requirement line changes.

### Authorized Surface
- Files allowed to change:
  - `requirements.txt`
- Functions/classes/components allowed to change: none.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No version bumps to existing dependencies; no lockfile introduction; no converter code.

### Risk Flags
- Risky surfaces touched: dependency manifest (`requirements.txt`).
- Approval needed before implementation: yes

### Validation Plan
- Tests to add/update: none.
- Commands to run: `mimic_venv/bin/pip install -r requirements.txt`, `mimic_venv/bin/python -c "import pandas; print(pandas.__version__)"`.
- Manual checks: diff shows exactly one added line.

### Rollback Path
- Revert the one-line commit; `pip uninstall pandas` in the venv.

## Slice 2: Converter scaffold, ctrees parser, synthetic fixtures

### Intended Change
- Create the `scripts/convert/` package skeleton and the **frozen scratch-record dtype**: NumPy structured dtype, little-endian, packed (`align=False`), `itemsize = 108` — `id i8, desc_id i8, desc_scale f8, pid i8, upid i8, snap i4, Mvir f4, X/Y/Z f4×3, VX/VY/VZ f4×3, Jx/Jy/Jz f4×3, vrms f4, vmax f4, tree_root_id i8, forest_id i8` (review finding 11; the conversion plan's "~116 bytes" total is superseded). The dtype is asserted in tests and recorded in every scratch-file manifest entry.
- Phase 1 parser: **indexed Consistent-Trees headers** (`#scale(0) id(1) …`) as the primary dialect, `#fields:` as a secondary dialect; strip `(N)` suffixes; match column names case-insensitively; accept `snap_idx`/`snap_num`/`Snap_num` variants; abort on duplicate or missing required columns; `#tree <id>` block-marker tracking so every halo row carries its tree root id; chunked `pandas.read_csv` with pre-specified dtypes; parse floats to float64 and cast to float32 at record assembly (matching the reference parse path).
- **Independent row pre-count** (review finding 7): during the marker pre-scan, count valid non-comment data rows per source file by a code path independent of the pandas parse; parsed-row count must equal the pre-count exactly before a file's result is accepted.
- `tests/` with a synthetic ctrees ASCII fixture generator (hand-specified tiny forests with known topology: multi-tree forests, multi-progenitor halos, mass ties, flyby configurations, zero-central-at-max-scale forests, early-dying forests, zero-mass halos, sub-subhalos whose `pid` differs from their ultimate host) plus small committed golden fixtures covering: indexed header, `#fields:` header, `Snap_num` vs `snap_idx` casing, duplicate columns (abort), malformed rows (abort), `#tree` boundaries across chunk boundaries.

### Acceptance Criteria
- Inputs: synthetic ctrees ASCII files in both header dialects.
- Outputs: parsed structured arrays in the frozen dtype; every row attributed to the correct tree root id; per-file independent row counts.
- User-visible behaviour: none yet (library + tests only).
- Behaviour that must not change: nothing outside `scripts/convert/` is touched.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/`
- Functions/classes/components allowed to change: all new within that directory.
- Tests allowed or expected to change: new tests under `scripts/convert/tests/`.

### Explicit Non-Goals
- No multiprocessing, no scratch-file I/O, no HDF5, no fix-up logic; no `src/`, `Makefile`, or `tests/` (repo tier) changes.

### Risk Flags
- Risky surfaces touched: none (new isolated directory).
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: parser unit tests — both header dialects, suffix stripping, case-insensitivity, snapshot-column variants, duplicate/malformed aborts, tree-boundary attribution across chunk boundaries, dtype itemsize/layout assertion, pre-count vs parse-count equality and deliberate mismatch detection.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v`; `./scripts/beautify.sh`.
- Manual checks: parse the first 10,000 lines of the real `tree_0_0_0.dat` header/rows successfully (read-only spot check).

### Rollback Path
- Revert the commit; the directory is self-contained.

## Slice 3: Phase 0 pre-pass, Phase 1 scatter, resume manifest

### Intended Change
- Phase 0: stream `forests.list` into a sorted tree-root-id → forest-id map; assign dense run-scoped `ForestIndex` by ascending ctrees forest id; **validate one-to-one coverage between observed `#tree` roots and `forests.list`** (missing or duplicate roots abort, review finding 7). The `ForestID` sidecar table is produced here as data; the `forests.h5` file itself is emitted by Slice 7's writer (single HDF5 owner, review finding 6).
- Phase 1: bounded `multiprocessing.Pool` scatter of ctrees files into per-snapshot worker binaries (frozen dtype, `forest_id` joined during scatter), post-pool concat into `scratch/snap_NNN.bin`, per-file resume manifest (name, size, independent pre-count, parsed count, checksum, dtype tag), per-forest max-snapshot aggregates, and observed `(SnapNum, scale)` pair collection cross-validated against the canonical `--a-list` (tolerance 1e-4; unknown pair aborts).
- CLI skeleton `convert_ctrees.py` with per-phase subcommands, `--workdir`, `--simulation-info`, `--a-list`.
- **Cleanup discipline:** delete-after-verify applies only to converter-created intermediates recorded in the manifest and located under the workdir; a path-containment guard refuses anything else. **No source-data deletion exists in this plan** (review finding 8).

### Acceptance Criteria
- Inputs: ctrees ASCII files + `forests.list` + `simulation_info.yaml` + a_list paths.
- Outputs: per-snapshot scratch binaries, manifest, ForestID table, per-forest max-snapshot table, observed-pair table.
- User-visible behaviour: CLI runs phase 0–1 end to end on synthetic fixtures; re-running skips completed files (resume works after a simulated crash); per-file conservation (pre-count == scattered rows) enforced before completion is recorded.
- Behaviour that must not change: frozen record dtype from Slice 2.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/`
- Functions/classes/components allowed to change: all within that directory.
- Tests allowed or expected to change: new unit tests for the Phase 0 map, root-coverage validation, ForestIndex enumeration order, scatter conservation, manifest resume, aggregate correctness, cleanup containment guard.

### Explicit Non-Goals
- No sorting, no link remapping, no HDF5 emission; no rsync/transfer tooling; no source-consumption option (deferred to a future Shin-Uchuu production slice with its own approval gate).

### Risk Flags
- Risky surfaces touched: none (isolated directory; deletion restricted to manifest-owned workdir intermediates with containment guards).
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: as above; a crash-resume test (kill mid-scatter, re-run, verify totals); a containment-guard test (manifest entry pointing outside the workdir must be refused).
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v`; `./scripts/beautify.sh`.
- Manual checks: manifest human-readable; ForestIndex order equals ascending forest id on fixtures.

### Rollback Path
- Revert the commit; delete workdir artifacts.

## Slice 4: Phase 2 sort and index

### Intended Change
- Per-snapshot: load scratch binary, assert within-snapshot `id` uniqueness (abort with examples), sort by ascending `id`, write `snap_NNN_sorted.bin` and `snap_NNN.idx` (sorted int64 id array), verify (row count + id checksum against manifest totals) then delete the unsorted file under the Slice 3 cleanup discipline. Independent per-snapshot jobs, parallelisable.

### Acceptance Criteria
- Inputs: Slice 3 scratch binaries.
- Outputs: sorted binaries + index files; unsorted scratch removed only after verification.
- User-visible behaviour: `convert_ctrees.py sort` subcommand.
- Behaviour that must not change: record dtype; manifest semantics; conservation totals.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/`
- Functions/classes/components allowed to change: all within that directory.
- Tests allowed or expected to change: new unit tests — sort determinism, duplicate-id abort, verify-then-delete discipline.

### Explicit Non-Goals
- No link logic; no memory-mapped alternatives beyond plain load-sort-write.

### Risk Flags
- Risky surfaces touched: none.
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: as above.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v`; `./scripts/beautify.sh`.
- Manual checks: none beyond tests.

### Rollback Path
- Revert the commit.

## Slice 5: Phase 3 fix-ups (adjacency, conventions, fix_flybys, fix_upid)

### Intended Change
- Implement, exactly per the conversion plan's Phase 3 steps 1–5, the cited reference functions, and the review-verified precise semantics:
- **Adjacency validation** against the canonical a_list (absolute tolerance 1e-4; abort with counts + examples; final snapshot must have all `desc_id == −1`).
- **Numeric contract (review finding 5):** inputs are float64-parsed and float32-cast (Slice 2); spin normalisation computes `J/Mvir` in float64 and casts the result to float32, only where `Mvir != 0` (zero-mass halos carried unnormalised) — matching `apply_ctrees_value_conventions`. `Len` uses half-away-from-zero rounding (C `round()` semantics, NOT NumPy banker's rounding) on `Mvir_native × 1e-10 / PartMass[1e10 Msun/h]`; require positive finite particle mass at startup; abort before int32 conversion on non-finite, negative, or `> INT32_MAX` derived values; `Len == 0` preserved and logged.
- **`fix_flybys` equivalent (review finding 4):** scoped per forest at that forest's max snapshot; **zero `pid == −1` centrals at max scale aborts** (reference `ctrees_utils.c` returns an error — corrupt input); exactly one central returns unchanged; multiple centrals invoke demotion — sole survivor chosen by strict-greater Mvir in ascending-id scan order, `upid` rewritten to the survivor for every other forest member at that snapshot, demoted centrals additionally get `pid` rewritten and `MostBoundID` negated; per-snapshot flyby logging.
- **`fix_upid` equivalent (review finding 4):** centrals get `upid = id`; satellite upid chains followed to depth 30 with the reference pid fallback; abort with examples on unresolved targets; **every resolved satellite gets BOTH `upid` and `pid` set to the ultimate central's id** (reference `ctrees_utils.c` final assignment) — this matters because Slice 6's sort key uses post-fix `pid`.

### Acceptance Criteria
- Inputs: sorted per-snapshot arrays + per-forest max-snapshot table + a_list.
- Outputs: per-snapshot arrays with resolved upid/pid, corrected MostBoundID signs, normalised Spin, derived Len.
- User-visible behaviour: `convert_ctrees.py` gains the fix-up stage; abort messages carry counts and concrete examples.
- Behaviour that must not change: slices 2–4 outputs; fix_flybys strictly before fix_upid (reference execution order).

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/`
- Functions/classes/components allowed to change: all within that directory.
- Tests allowed or expected to change: new unit tests driven by the synthetic fixtures: flyby demotion with mass ties (strict `>` semantics), zero-central abort, forests dying before the final snapshot, deep upid chains, pid-fallback cases, sub-subhalo whose original `pid` differs from its ultimate host (post-fix `pid` must equal the ultimate central), unresolved-target abort, Mvir==0 spin carve-out, half-integer Len rounding (away-from-zero), Len overflow/non-finite aborts, Len-zero preservation.

### Explicit Non-Goals
- No chain construction, ranks, or identity fields (Slice 6); no snapshot-native FoF semantics — reference replication only (resolved decision D12).

### Risk Flags
- Risky surfaces touched: scientific-correctness-critical logic (identity gate depends on it), isolated to the new directory.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: as above, plus a hand-computed golden fixture where the expected post-fix upid/pid/MostBoundID arrays are written out explicitly in the test.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v`; `./scripts/beautify.sh`.
- Manual checks: re-read the implementation side-by-side against `ctrees_utils.c` `fix_flybys`/`fix_upid` before review.

### Rollback Path
- Revert the commit.

## Slice 6: Phase 3 links, ranks, and identity fields

### Intended Change
- Implement, per the conversion plan's Phase 3 steps 6–9: the reference sort key (descending scale, upid, pid, ascending id) over post-fix values; FoF chain construction (`FirstHaloInFOFgroup` self-reference for centrals, `NextHaloInFOFgroup` linked list in reference order); descendant remap via merge-join on sorted ids with abort-on-missing-target; progenitor links (`FirstProgenitor` = max-Mvir with first-encountered-in-reference-order tie-break; `NextProgenitor` chains in reference encounter order — the (upid, pid, ascending id) key, NOT slab order — with the max-Mvir progenitor promoted to front and the remainder in encounter order); the cross-snapshot pending buffer; the rank pass (`HaloRankInForest` per forest in reference order over all snapshots, computed from post-fix values); `ForestIndex` carry-through; global `(ForestIndex, HaloRankInForest)` uniqueness assertion; run-scoped `n_forests_total`/`max_halo_rank_in_forest` values.

### Acceptance Criteria
- Inputs: Slice 5 outputs.
- Outputs: complete per-snapshot link/identity arrays ready for HDF5 emission.
- User-visible behaviour: `convert_ctrees.py` runs Phase 3 end to end on fixtures.
- Behaviour that must not change: fix-up semantics from Slice 5; abort-never-repair discipline.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/`
- Functions/classes/components allowed to change: all within that directory.
- Tests allowed or expected to change: new unit tests: chain order against hand-computed reference order on fixtures (including tie cases), merge-join abort on dangling `desc_id`, pending-buffer correctness across three snapshots, rank density per forest, identity uniqueness, encounter-order-not-slab-order regression test.

### Explicit Non-Goals
- No HDF5 emission (Slice 7); no chunked external-merge rank sort (Shin-Uchuu-scale concern; in-memory grouping is sufficient for micro-Uchuu and fixtures — the external-sort fallback is deferred to the production-conversion phase and noted in README).

### Risk Flags
- Risky surfaces touched: scientific-correctness-critical logic (identity gate depends on it), isolated to the new directory.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: as above.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v`; `./scripts/beautify.sh`.
- Manual checks: verify the encounter-order key against `ctrees_utils.c` `assign_mergertree_indices` sort (the conversion plan's IDENTITY TRAP note) before review.

### Rollback Path
- Revert the commit.

## Slice 7: HDF5 emission, producer validation battery, conversion report

### Intended Change
- `hdf5_writer.py`: emit `snapshot_NNN.h5` files exactly per `docs/SNAPSHOT-HDF5-FORMAT.md` — dataset names/types, header attributes (with explicit unit conversions from `simulation_info.yaml`: `particle_mass_msun_h = particle_mass[1e10] × 1e10`, box size, cosmology, per-file `scale_factor` from the canonical a_list), ascending-|MostBoundID| slab order, chunk shapes `(65536,)`/`(65536, 3)`, no compression — **including empty snapshot files for a_list snapshots with zero halos** (zero-length chunked datasets; explicitly fixture-tested, review findings 6 and risk list). Emit the `forests.h5` sidecar from Slice 3's ForestID table and validate its exact object set (single HDF5 owner).
- `validate.py`: the full producer battery from the spec's Validation Requirements section — **count conservation against the independent per-file pre-counts from Slice 2, not the (circular) parser-derived manifest totals alone** (review finding 7) — plus all six format invariants, progenitor round-trip closure, `NextProgenitor` same-file scope, FoF chain integrity/cycle-freedom, identity uniqueness/density, header bounds, a_list↔`scale_factor` consistency, `Len ≥ 0` with zero-count logging; standalone CLI over a directory of snapshot files, non-zero exit on failure.
- `report.py`: conversion report (totals, per-snapshot counts, forest count, measured `max_halo_rank_in_forest`, flyby counts, Len-zero counts, observed-pair table, validation outcomes, recommended identity multiplier).

### Acceptance Criteria
- Inputs: Slice 6 arrays + canonical metadata inputs.
- Outputs: contract-conformant HDF5 files (including empty snapshots); validation battery pass/fail with non-zero exit on failure; durable report artifact.
- User-visible behaviour: `convert_ctrees.py write`, `validate.py <dir>`, report emission.
- Behaviour that must not change: the frozen contract is consumed, never modified — any mismatch discovered is a converter bug or a spec erratum to raise to the user, not a silent local deviation.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/`
- Functions/classes/components allowed to change: all within that directory.
- Tests allowed or expected to change: new contract-conformance unit test asserting the exact object set, dtypes, attribute names/types/values (including converted units), chunking, and absence of compression against fixture output; empty-snapshot fixture test; battery unit tests with deliberately corrupted files (each invariant violated once, battery must catch every one); sidecar object-set test.

### Explicit Non-Goals
- No spec edits (`docs/SNAPSHOT-HDF5-FORMAT.md` is frozen; erratum = stop and report); no compression options.

### Risk Flags
- Risky surfaces touched: the on-disk contract surface (files must match the frozen spec exactly in structure).
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: as above.
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v`; `./scripts/beautify.sh`.
- Manual checks: `h5ls -rv` inspection of one fixture snapshot file (including an empty one) against the spec tables.

### Rollback Path
- Revert the commit.

## Slice 8: Cross-check implementation (synthetic validation only)

### Intended Change
- `crosscheck.py` implementing the corrected six-check design from the Context section: per-snapshot matching by `|MostBoundID|` over Type 0/1 galaxies; forest-decode equality for all matches; full (forest, rank) decode equality on the creation subset (first appearance of each `UniqueGalaxyID`); FoF central verification via the `UniqueGalaxyID == UniqueCentralGalaxyID` galaxy's `|MostBoundID|` against the converter's `FirstHaloInFOFgroup` target; exact flyby-sign set equality; frozen value-comparison rules (integers/links/signs exact; float32 fields bit-exact with stop-and-report; derived fields exact after documented arithmetic); occupancy-predicate computation (`occupied(H) = any(progenitor occupied) OR FoF central`, forward induction) with exact unmatched-set equality both directions.
- Reference-run plumbing: build/run helpers that copy the halos-only run file into the workdir with all 50 snapshots listed and output redirected to the workdir (repo files untouched), capture exit codes and durable logs.
- If Slice 10 was approved and landed: a `--reference-topology <dump>` mode comparing `FirstProgenitor`/`NextProgenitor`/`NextHaloInFOFgroup` chain order directly by stable id.
- Unit tests against synthetic fixtures with hand-built mock reference outputs (small HDF5 files in the reference output schema): every check must catch a deliberately injected violation (wrong rank, wrong central, missing flyby sign, value flip, occupancy mismatch).

### Acceptance Criteria
- Inputs: converter output directory + reference-run output directory (mock in tests).
- Outputs: cross-check report with per-check pass/fail, counts, and example ids; non-zero exit on any failure.
- User-visible behaviour: documented CLI; no real-data execution in this slice.
- Behaviour that must not change: no repo run files or packages modified; comparison rules exactly as frozen in Context (changing them requires plan revision, not implementer judgment).

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/`
- Functions/classes/components allowed to change: all within that directory.
- Tests allowed or expected to change: new crosscheck unit tests as above.

### Explicit Non-Goals
- No real micro-Uchuu run (Slice 9); no tolerance widening; no free-form acceptance of unmatched halos.

### Risk Flags
- Risky surfaces touched: scientific-correctness-critical logic (this IS the acceptance instrument), isolated to the new directory.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: as above (injected-violation coverage for every check).
- Commands to run: `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v`; `./scripts/beautify.sh`.
- Manual checks: review the occupancy predicate side-by-side against `src/core/inheritance.c` inheritance/creation semantics.

### Rollback Path
- Revert the commit.

## Slice 9: Micro-Uchuu end-to-end gate run and closeout

### Intended Change
- Run the full pipeline on the real micro-Uchuu ASCII data (12 GB, 50 snapshots) into `output/convert/micro-uchuu/`; run the producer validation battery; emit the conversion report.
- Build and run the reference: `make MODEL=halos-only SIMULATION=micro-uchuu-ascii`, then `./mimic` on the workdir scratch run file (all 50 snapshots); capture exit codes and logs under the workdir.
- Run `crosscheck.py` (with `--reference-topology` if Slice 10 landed); every check must pass with zero unexplained mismatches.
- Documentation closeout, ONLY after all gates pass: `scripts/convert/README.md` (usage, workdir layout, gate results); mark pathway item 2 done in `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` — **stating explicitly whether the topology gate is fully discharged (Slice 10 ran) or partial pending the Phase 5 identity gate (Slice 10 declined)**; update the conversion-plan status header to record the converter's existence and gate outcome.

### Acceptance Criteria
- Inputs: real micro-Uchuu ASCII package data.
- Outputs: 50 validated snapshot HDF5 files + `forests.h5` + conversion report + cross-check report — the reader-development fixtures for dual-driver Phase 4b.
- User-visible behaviour: documented, reproducible commands for the whole gate; captured logs with explicit exit codes.
- Behaviour that must not change: repo run files, packages, and source data are read-only inputs; the tree-ordered reader and all existing tests remain untouched and green.

### Authorized Surface
- Files allowed to change:
  - `scripts/convert/`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`
- Functions/classes/components allowed to change: converter directory only (bug fixes found by the real-data run belong here, each re-gated); docs status lines only.
- Tests allowed or expected to change: none beyond converter-directory fixes with accompanying tests.

### Explicit Non-Goals
- No `tests/` (repo tier) or `Makefile` integration of converter tests (deferred until the snapshot reader lands); no Shin-Uchuu transfer/production tooling; no new simulation package; no status update before gates pass.

### Risk Flags
- Risky surfaces touched: none in-repo beyond docs status lines; heavy local compute/disk in the workdir (~tens of GB); the reference run rebuilds with non-default MODEL/SIMULATION selectors (use the same pair for every command, per the selector invariant).
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: none new (this is the operational gate).
- Commands to run: full pipeline CLI; `validate.py`; reference build + run; `crosscheck.py`; `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests -v`; `make check-docs`; `./scripts/beautify.sh`.
- Manual checks: read the cross-check report end to end; spot-check one forest's chain order by hand against the reference sort key; confirm the pathway wording matches the actual topology-gate status.

### Rollback Path
- Revert the commit; delete the workdir. Converted fixtures are regenerable from source data at any time.

## Slice 10: Reference topology dump harness (optional, approval-gated)

### Intended Change
- A minimal, read-only C harness in Mimic's test tier that loads micro-Uchuu forests through the existing `consistent_trees_ascii` reader and dumps, per halo by stable ctrees id: descendant id, ordered progenitor chain ids, ordered FoF chain ids, forest number, and within-forest rank — the direct reference evidence for chain-order conformance. Exact placement/registration mechanism decided at implementation with the `mimic-validation-and-qa` skill; no production `src/` file changes; the tree-ordered runtime path is untouched.

### Acceptance Criteria
- Inputs: micro-Uchuu ASCII package data via the existing reader.
- Outputs: a deterministic topology dump file consumed by `crosscheck.py --reference-topology`.
- User-visible behaviour: a test-tier tool/target; documented in `scripts/convert/README.md`.
- Behaviour that must not change: all existing tests green; no reader/driver code modified; byte-identical tree-driver output preserved.

### Authorized Surface
- Files allowed to change:
  - `tests/`
  - `scripts/convert/`
  - `Makefile`
- Functions/classes/components allowed to change: new test-tier harness only; `Makefile` only if a target registration is unavoidable.
- Tests allowed or expected to change: the harness itself plus a small-fixture test of its dump format.

### Explicit Non-Goals
- No production `src/` changes; no snapshot reader; no changes to existing tests.

### Risk Flags
- Risky surfaces touched: Mimic C test tier and possibly `Makefile` — this is the plan's one deliberate exception to the "no new Mimic code before the converter is proven" sequencing, and the user must decide it explicitly (review finding 3 / open question).
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan
- Tests to add/update: harness dump-format test on a tiny fixture.
- Commands to run: full standard gate (`make check-generated && make validate-modules`, all three test tiers via subagent capture); the harness run on micro-Uchuu.
- Manual checks: confirm zero diff to production `src/`; confirm existing suite untouched.

### Rollback Path
- Revert the commit; `crosscheck.py` degrades gracefully to the six-check partial gate.

---

## Deferred and Out of Scope (recorded so nothing is silently dropped)

- **C tokeniser fallback** (if pandas parse throughput < ~500 MB/s): a Shin-Uchuu-scale concern; micro-Uchuu completes regardless. Trigger and design live in the conversion plan.
- **Chunked external-merge rank sort** for the Shin-Uchuu super-forest: not needed at micro-Uchuu scale.
- **Transfer tooling and consumptive source deletion** (batched rsync, `--consume-source`): production-conversion concerns for a future approval-gated Shin-Uchuu slice (review finding 8).
- **Shin-Uchuu a_list drafting** from the observed-pair table: production concern; the table is emitted by the report either way.
- **Makefile/test-tier integration** of converter tests: after the snapshot reader exists.

## Next Chat Prompt

```md
Plan file: docs/dev/MIMIC-CONVERTER-IMPLEMENTATION-PLAN.md
Slices or batch this session: Slice 1 (approval-gated), then Batch A (Slices 2–4)

Read the full plan file first. If a selected slice or batch receipt is incomplete or the plan state is unclear, stop and tell me before coding.

Work on the current feature branch for this plan; if none exists, create one and tell me the name.

Use orchestrator as the controlling skill. Act as the Developer: keep implementation, validation, Git operations, and commits local. Use a read-only Reviewer only for investigation, evidence gathering, the hostile drift-audit skill, and an independent code-review skill pass. If no Reviewer is configured or available, perform Developer self-audit and record that provenance explicitly.

For each selected slice or batch, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. If any included slice's Risk Flags mark approval-needed, stop and get my approval before coding.
3. apply the scoped-implementation skill against the selected contract.
4. apply the drift-audit skill using a read-only Reviewer when available; otherwise perform Developer self-audit. Report the authorization gate result and who performed it before any quality review.
5. If the gate passes, apply the code-review skill using a read-only Reviewer when available; otherwise perform Developer self-audit through the code-review skill. Record who performed it. If the drift gate fails, fix the drift and re-audit.
6. Surface drift and review findings to me, fix them, then re-run the relevant gate. If consecutive reviews return only minor findings and have clearly converged record residuals in the slice summary and proceed.
7. Ask me before committing. On my approval, commit the selected slice or batch with the commit skill.

After the selected slice(s) or batch are committed, use the handoff skill to record state, audit provenance (Reviewer tool/label or Developer self-audit and fallback context), and the next slice or batch to resume from. Do not continue past the selected scope.

Confirm before starting: plan file read, selected slice(s) or batch, branch, and the first slice.
```
