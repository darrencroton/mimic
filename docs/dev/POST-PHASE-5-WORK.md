# Post-Phase-5 Work

**Status:** Open. Written 2026-08-12 at branch `feature/ctrees-snapshot-reader`, HEAD `168aa7fd`, immediately after dual-driver Phase 5 closed.
**Purpose:** The single place every item deferred, reported-but-not-fixed, or newly discovered during Phase 5 is recorded with enough context to act on without re-reading the plan or its run records. Nothing here blocks Phase 5's closure; several items block or shape the Shin-Uchuu production conversion, and those are marked.
**Provenance:** Phase 5 ran under `project-manager` (Mode B) across four PM runs — `20260811T055422Z-02e67d` (Slices 1–9), `20260812T072526Z-bbdfaf` (Slice 10), `20260812T130721Z-bd8cff` (Slice 11), plus an earlier run whose slices were attested forward. Their assessments, validation records and review reports live under `.pm/runs/<run-id>/` and are the evidence behind every claim below.

---

## 1. What Phase 5 actually delivered

Recorded here because several items below only make sense against it, and because the Shin-Uchuu executor needs it in one place.

- **A second live driver.** `run_snapshot_driver()` (`src/core/snapshot_driver.c`), dispatched from `run_processing_driver()` (`src/core/tree_driver.c:615`) when `input.processing_order: snapshot_ordered`. That case was previously a `FATAL_ERROR`. Weak points **W1 and W2 are closed**.
- **A cross-format identity gate that passes bitwise.** Tree-ordered versus snapshot-ordered over the full micro-Uchuu dataset: identical `UniqueGalaxyID` sets and per-ID byte-identical fields for every output snapshot, for `halos-only` (4,409,282 records, 20 fields) and `sage16` (3,111,793 fixed / 3,111,759 dynamic, 42 fields), under both timestep schemes, aggregating the tree side's five output partitions against the snapshot side's one. **No tolerance of any kind** — the comparison is raw bytes.
- **The tree-ordered path is unchanged.** Galaxy records byte-identical to the pre-Phase-5 baseline `ae22d278`, with exactly four permitted HDF5 metadata deltas (`UniqueGalaxyIDMultiplier` attribute; `TotHalosPerSnap` widened to int64; the `UniqueGalaxyID` description; `hdf5_format_version` 1.1 → 1.2) and `output_schema.json` differing in exactly the description and `source_md5`.
- **Supporting seams:** an explicit `struct HaloInputView` replacing the `InputTreeHalos` global; identity fields on reader-owned slab arrays; physical-header agreement at `open_run`; an instanced `struct GalaxyPool *`; int64 output widths; a configured `UniqueGalaxyID` multiplier honoured by **both** drivers and recorded in output provenance; a driver-neutral output-partition seam.
- **Snapshot-run constraints, enforced at config time** (`src/core/read_parameter_file.c:1452-1466`): HDF5 output only, serial only (`NTask > 1` rejected), `--skip` rejected. Snapshot runs write **one** output partition plus a master, carry no `Ntrees` and no `TreeHalosPerSnap`, and use int64 `TotHalosPerSnap`.

**How to re-run the gate** — a manual, dataset-present operation; no automated tier and no CI runs it:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate
make MODEL=halos-only SIMULATION=micro-uchuu-snapshot
make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific
```

Roughly six minutes and ~7 GiB of scratch on the development machine. It builds four isolated git worktrees, runs eight comparisons plus a baseline preservation stage, and cleans up on every exit path.

---

## 2. Blocks or shapes the Shin-Uchuu conversion

Work through this section before converting Shin-Uchuu.

### 2.1 `simulations/uchuu/halo_properties.yaml` declares `Spin` with a bound its own data will exceed — **highest priority**

`Spin` in the Consistent-Trees packages is **J/Mvir** — specific angular momentum in (Mpc/h)(km/s) — despite a `dimensionless` units label. It scales with halo size. `simulations/uchuu/halo_properties.yaml` bounds it at `[-20.0, 20.0]`.

micro-Uchuu, in a far smaller box, already reaches **270.3** — the z-component for the box's single largest halo (Mvir = 14039.99 in 1e10 M⊙/h, Len = 432000), 1 of 3,559,002 Spin values, extremes −84.2 to 270.3. Phase 5 widened the three micro-Uchuu Consistent-Trees packages to `[-1000.0, 1000.0]` (commit `23eab57e`, ~3.7× the observed maximum). **`uchuu` was deliberately left alone** because its correct bound is a separate scientific call.

A production-volume box will host far more massive clusters than micro-Uchuu, so `[-20, 20]` will fail. Decide the bound for `uchuu` and for the new `shin-uchuu` package before either is run at scale.

**Related, and worth reconciling once:** `Spin` names *two different quantities* across packages, and the declared ranges do not track the definitions.

| Package(s) | Definition | Declared range |
|---|---|---|
| `micro-uchuu-{ascii,hdf5,snapshot}` | J/Mvir | `[-1000, 1000]` (widened 2026-08-12) |
| `uchuu` | J/Mvir | `[-20, 20]` ⚠️ |
| `micro-uchuu` | Bullock spin parameter λ (order 0.03–0.05) | `[-200, 200]` (`simulations/micro-uchuu/halo_properties.yaml:131`) |
| `mini-uchuu`, `millennium`, `mini-millennium` | Bullock spin parameter λ (order 0.03–0.05) | `[-20, 20]` |

Note that range metadata reaches only `tests/generated/property_ranges.json` — it never enters `output_schema.json` or the HDF5 `FieldMetadata` table — so changing a range produces no output-schema or metadata delta.

### 2.2 Recompute the driver's memory peak before the production run

The snapshot driver holds **two complete raw slabs unconditionally**, plus two processed generations, two galaxy pools, and the output and HDF5 buffers. **There is no memory-projection fallback branch in the code** — Phase 5 deliberately built none.

`SHIN-UCHUU-CONVERSION-PLAN.md`'s "Delegated obligation" subsection already carries this obligation, including the 85%-of-RAM trigger sentence: if the peak exceeds ~85% of installed RAM (≈435 GB on a 512 GB machine), replace the retained *previous* raw slab with a compact `{int32_t Len, int32_t NextProgenitor}` projection. Four corrections/additions to that text, the first two carried from the original recompute obligation and the last two from the joint review (`POST-PHASE-5-JOINT-REVIEW.md` F-5, F-9):

- Its `struct RawHalo` figure of **104 B is the default pair's** (mini-millennium/sage16). The ctrees-bridge catalog that Shin-Uchuu will use measures **88 B**, so the full-second-slab cost is ≈315e6 × 88 B ≈ **27.7 GB**, not ≈32.8 GB. Re-derive from the `shin-uchuu` package's own `sizeof(struct RawHalo)`, not from either recorded number.
- Neither the two processed generations nor the two galaxy pools are quantified anywhere; the plan's only numeric memory tables are converter-side.
- **The snapshot driver's output buffer is seeded at slab size, and `memset` makes the full seed capacity resident immediately** (`src/core/snapshot_driver.c:683-692`): the buffer is seeded at `nhalos + MIN_HALO_ARRAY_GROWTH` records, ~315M × 264 B ≈ 78 GiB at a z=0 Shin-Uchuu slab, and the seeding `memset` touches all of it immediately — do not remove the `memset` (it is defensive zeroing the marshaller is contractually expected to overwrite), but a `sizeof`-based projection that assumes lazy residency will underestimate the true peak by this amount. The recompute must count full seed capacity as resident, not just the bytes the marshaller happens to fill.
- **The reader's two staging buffers total ~10 GB transient at a 315M slab and are held simultaneously during a slab fill** (`src/io/snapshot/read_snapshot_hdf5.c:678-728`): both buffers are allocated at the widest element size (8 B) and held together for the whole fill; every multi-dim catalog field in every current package is float32, so this is free headroom for the recompute rather than a feasibility item on its own, but it must be counted, not silently assumed away.

Two briefs currently rest on a superseded estimate and should not be trusted until this recompute happens: `MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md:17` ("~300–450 GB peak estimated") and `MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md:14` ("fits comfortably"). `MIMIC-DUAL-DRIVER-PLAN.md:186` already retired that number for omitting the retained previous slab.

### 2.3 Two output-path ceilings below the projected z=0 population — check against the lower one first

**First ceiling (record correction, joint review F-2): `MAX_HALO_ARRAY_SIZE = 1000000000`** (`src/include/constants.h:43`), enforced in output-buffer growth (`src/core/output_buffer.c:58-70`). Output-buffer growth is clamped to this constant and FATALs when it cannot grow past it. A snapshot whose output population (slab galaxies + accumulated orphans) exceeds 10⁹ aborts in the marshaller **before** the second ceiling below is ever consulted — at roughly half the headroom this section previously assumed. The constant was not revisited when `OutputBuffer` widened to int64, and the fatal's message presents it as a structural invariant rather than a tunable (its `%d` on what would become an `int64_t` also needs `PRId64` if this is ever widened). **The projected-population check must compare against 10⁹, not 2.1e9; if widening is ever needed, both ceilings move together.**

**Second ceiling: `output_increment_halo_counters_checked()` still caps at `INT_MAX`.** `src/io/output/util.c` FATALs when `TotHalosPerSnap[snap_index] == INT_MAX` (both branches), even though Phase 5 widened the schema to int64. Unreachable at micro-Uchuu scale — the largest per-snapshot total is ~1.19M against a 2.1e9 cap — and it was deliberately left unfixed because relaxing it changes an error contract no Phase 5 slice specified. This ceiling only matters once the first one above it is cleared.

**At Shin-Uchuu scale this needs a decision.** The projected ~315M z=0 slab sits well under 10⁹, so the first ceiling fires only if the output population exceeds the slab by ~3× (orphan accumulation would have to be extreme) — this is a record correction, not a projected abort. Still, check the projected z=0 output population against **10⁹** before running; if it is within an order of magnitude of either ceiling, widen the counter and its guard to int64 first, moving both ceilings together.

### 2.4 Confirm the Shin-Uchuu particle mass before freezing the package

`SHIN-UCHUU-CONVERSION-PLAN.md:34` records the particle mass as *inferred*, with `:449` noting that a wrong value corrupts every `Len`. Still live. The failure mode has improved: the reader now compares `particle_mass_msun_h`, `box_size_mpc_h` and the three cosmology values against `MimicConfig` **in every snapshot file** and aborts on mismatch (`src/io/snapshot/read_snapshot_hdf5.c:1045-1055`, mass compared as `PartMass × 1e10`). A disagreement now aborts at `open_run` rather than running silently — but an agreeing pair of *wrong* values still runs.

### 2.5 Calibrate the remaining property ranges for `shin-uchuu`

`SHIN-UCHUU-CONVERSION-PLAN.md:439` lists `deltaMvir`, `Len` (floor is 1 at this resolution) and `Spin` as requiring calibration from a test run. For reference, the micro-Uchuu packages now ship `Spin [-1000, 1000]`, `Vel [-5000, 5000]`, `VelDisp [0, 5000]`, `Vmax [10, 5000]`.

**Clarification (joint review F-10):** `deltaMvir` is not a package catalog range like `Len`/`Spin` — it is a **core-level output property** (`src/core/core_properties.yaml`, range `[-20000.0, 20000.0]`, already annotated "symmetric wide bounds to accommodate Uchuu-scale mass swings"), computed during inheritance rather than read from any simulation package's `halo_properties.yaml`. Calibrating it is legitimate pre-Shin-Uchuu work — the range check still needs confirming against a Shin-Uchuu test run — but it is checked and edited in `core_properties.yaml`, not in `simulations/shin-uchuu/halo_properties.yaml`.

### 2.6 The identity encoding's cross-reader equivalence has no unit test

The gate's correctness rests on the snapshot reader's `forest_index` / `halo_rank_in_forest` matching the tree reader's `HaloRankInForest` and `GlobalForestOffset + unit` for the same physical halo. **This is validated only by the gate itself** — no unit test pins it. It has now held bitwise across two models, two timestep schemes and 7.5M+ galaxy records on micro-Uchuu, which is strong empirical evidence, but the equivalence is a property of the *converter and reader pair* and Shin-Uchuu is a new conversion.

**Run the identity gate on a Shin-Uchuu subset before trusting a full production run.** That is the cheapest possible check against a conversion-side indexing error.

---

## 3. Correctness and hygiene items (do not block Shin-Uchuu)

### 3.1 `make dump-ctrees-topology-tool` is broken

Broken since `5cd28b94`, which predates Phase 5, and Phase 5 added a second breakage. Current link failure:

```text
Undefined symbols for architecture arm64:
  "_narrow_int64_to_int_checked", referenced from: _load_unit in src_io_tree_interface.o
  "_snapshot_reader_lookup",      referenced from: _parse_input_section in src_core_read_parameter_file.o
```

Both are traceable:

- `snapshot_reader_lookup` is defined in `src/io/snapshot/registry.c`, which `tests/unit/tools/build_topology_dump.sh` does not list (nor `src/io/snapshot/interface.c`).
- `narrow_int64_to_int_checked` is defined in `src/core/output_buffer.c:27`, which the script **deliberately excludes** to keep its dependency surface small — but Phase 5's int64 work made `src/io/tree/interface.c:154` call it.

**Suggested fix:** add `src/io/snapshot/registry.c`, and rather than pulling in `output_buffer.c`, consider relocating `narrow_int64_to_int_checked` to `src/util/numeric.c`, where a general numeric-narrowing helper belongs and which the script already builds. That fixes the tool without widening its dependency surface.

### 3.2 Stale comment misdescribing the multiplier contract

`tests/framework/core_test_fixtures.h:213-217` states the shared unit-test config forces `simulation.unique_galaxy_id_multiplier` to `TREE_MUL_FAC` "since the tree-ordered branch these overrides now route through hard-rejects any other value". **Both clauses became false at Slice 7:** the encoder takes the configured multiplier and the tree-ordered rejection was lifted. The forcing behaviour is harmless — it forces the still-valid default — but the comment misdescribes the code. This had no in-plan home: outside Slice 7's surface and outside Slice 11's documentation-only surface.

### 3.3 `Makefile:405` lists a generated file nothing generates

`$(GEN_DIR)/init_halo_properties.inc` is listed as a dependency, but no script under `scripts/` emits it any more. The file exists on disk as a leftover artifact. Remove the stale dependency (and the orphaned artifact).

### 3.4 `scripts/discovery.py` has no `micro-uchuu-snapshot` entry

`FULL_MODEL_TEST_SIMULATIONS` and `PRODUCTION_TEST_CONFIG_SIMULATIONS` (`:41-56`) list `micro-uchuu`, `micro-uchuu-hdf5` and `micro-uchuu-ascii` but not `micro-uchuu-snapshot`. Carried forward from the Phase 4b reader plan's deferred list. Nothing in the identity gate consumes these lists, so this is a gating-completeness question, not a correctness one.

### 3.5 Gate hardening: an unreachable gap and a runner annoyance

- `simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py` — stage 8's `assert_records_byte_identical()` returns a record count but never asserts it is greater than zero. **Not reachable today:** stage 8 requires `identity:halos-only:fixed` (`:1507`), and `compare_pair` hard-fails when `compared_records <= 0` (`:1124`) or when it mismatches the independently derived count (`:1129`). A two-line guard whenever this file is next legitimately open.
- `tests/framework/runner.py` does not abort the suite on a failed stage: it records the failure and continues, so every later stage still spins up and fails its prerequisite check. Exit status stays non-zero, so there is no correctness impact — but on a multi-stage gate it wastes time and buries the root cause under redundant failures.

### 3.6 Test-coverage gaps recorded during the run

- **No committed self-test for `scripts/compare_cross_format_identity.py`.** Its three synthetic failure modes (perturbed byte, dropped ID, duplicated ID) plus signed-zero, NaN-payload, one-sided-duplicate and corrupt-input cases were all demonstrated as run evidence and independently re-derived by the PM, but nothing committed exercises them. The natural home is beside the gate.
- **No committed regression test for the `UniqueGalaxyIDMultiplier` provenance attribute.** Read-back evidence exists for per-file and master on two packages; a committed test needs `tests/integration/test_output_formats.py`.
- **The Consistent-Trees reader guards lack durable discriminating-multiplier tests.** The three forest-size guards were proven by a temporary reseed experiment and by multiplier-4 rejections on real data, then reverted.
- **The quote-stripping branch in `tests/framework/core_test_fixtures.h` has no test.**

### 3.7 Smaller items, verified and recorded

- **`h_convention` producer/consumer asymmetry.** A future package declaring `units: Mpc/h` with `h_convention: free` would be stamped `100.0`, held as `67.74`, and abort at `open_run` on correctly produced data. Unreachable today; would bite whoever first declares that combination.
- **`snapshot_h5_fill_identity`** derives its memory type from the schema table but hard-codes destination `int64_t` / `ncols=1`. Documented in-code, deliberately unguarded.
- **`expect_success` duplicates `expect_fatal_capture`** and drops its truncation guard.
- **`VIEW_TAKING_OUTPUT_FUNCTIONS`** in `scripts/generate_properties.py` crosses the core/model boundary.
- **`.agents/skills/mimic-properties/SKILL.md:93`** was outside Slice 11's authorized surface; re-check it against the shipped property system.

---

## 4. Carried forward from the Phase 4b reader plan

`docs/dev/MIMIC-SNAPSHOT-READER-PLAN.md` was archived to `archive/dev-plans/` at Phase 5 closeout. Its four still-live deferred entries survive in `MIMIC-DEVELOPMENT-PATHWAY.md:31` and `MIMIC-DUAL-DRIVER-PLAN.md:153`, and are repeated here so one document holds everything:

1. The shared HDF5 read-utilities refactor (downgraded to optional; nothing independently requires it).
2. The empty-dataset-with-non-sentinel-metadata reader check.
3. Reader strictness gaps — nested `/header` objects, same-size datatype acceptance.
4. `scripts/discovery.py` test-gating membership for `micro-uchuu-snapshot` (see 3.4).

---

## 5. Plan defects recorded during execution

Kept for whoever writes the next plan; each cost real time.

| # | Defect | Disposition |
|---|---|---|
| 1 | Slice 7's Tests-allowed clause said "the **two** multiplier precedence cases"; the file held one precedence test plus a rejection test | Resolved on the record. Reword to "the precedence case and the rejection case"; refresh the span. |
| 2 | Slice 8's authorized surface omitted `tests/unit/run_tests.sh` while granting the `tests/unit/` directory, making a criterion unsatisfiable | Resolved on the record. Name the file explicitly, as Slice 4's surface already did. |
| 3 | Slice 10 criterion 1 required `make … tests-scientific` to exit 0, which no in-contract implementation could satisfy — `tests/scientific/test_scientific.py` was hard-bound to `test_binary.yaml`, which a snapshot-ordered package correctly refuses | **Amended** (`cf59271f`, human-approved). The tier now selects HDF5 output when the package cannot write binary. Without this, Shin-Uchuu would have inherited a scientific tier that could not run at all. |
| 4 | Slice 10's "exactly two changed lines" rule forced both new run files to keep their ASCII counterpart's header comment, so each described itself as Consistent-Trees ASCII | **Amended** (`cf59271f`). Now two functional keys plus an optional header comment. |
| 5 | Slice 10 criterion 8's file enumeration omitted `scripts/compare_cross_format_identity.py` while the Authorized Surface two lines below granted it | **Amended** (`cf59271f`). |

**Process lesson for the next Mode B run.** A `start-slice` *relaunch* preserves the original `before_head`; it does not re-record it. Any commit made between slices therefore lands inside the next slice's recorded diff window and fails the floor's surface check. Finalize the in-flight attempt first, then commit, then start the slice fresh.

---

## 6. Shin-Uchuu readiness

**Superseded ordering per `docs/dev/POST-PHASE-5-JOINT-REVIEW.md` §6 (2026-08-13).**

**The Mimic runtime is ready pending the two fixes now landing, and the converter needs a scheduled scale-engineering pass before the production conversion.** Both drivers are live, the identity gate passes bitwise on two models and two timestep schemes with no tolerance, the tree-ordered path is provably unchanged, and the scientific tier now runs for snapshot-ordered packages — but the joint review found that the galaxy pool's chunk design exhausts the allocator's block table at Shin-Uchuu slab scale (a mid-run abort), that critical HDF5 statuses are ignored on the output finalization path (a damaged run can exit success), and that the converter as implemented — in-memory identity/rank pass, in-memory validation battery, a scatter resume model incompatible with this plan's own batched consumptive-delete transfer — cannot execute the production conversion at all. These limits are recorded in `scripts/convert/README.md`'s "Shin-Uchuu-scale notes" as deferred to a future production pass, but no plan previously scheduled that pass (joint review F-13).

**Before converting and running Shin-Uchuu, close these in order** (renumbered and widened from the six-item list this section previously carried; items newly added or substantively changed by the joint review are marked **[new]**; closing status reflects the change set this documentation edit ships alongside):

1. Decide `Spin`'s bound for `uchuu` and `shin-uchuu` (§2.1) — this *will* fail otherwise. With the §2.1 table correction now applied. **Open** — a scientific call only the owner can make.
2. **[new] Fix the galaxy-pool block-cap ceiling and the ignored output-finalization statuses in one change set, certified by one evidence run: bitwise tree-path preservation + full tiers + a mandatory identity-gate re-run.** The galaxy pool's chunk design exhausts the allocator's block table at Shin-Uchuu slab scale (fix: geometric chunk growth in `galaxy_pool_alloc()`); critical HDF5 statuses are ignored on the output finalization path — close statuses everywhere, plus create/write results throughout master construction — so a deferred or immediate output error can end a damaged run with exit success (fix: checked failure propagation, cleanup armed until all succeed). The duplicated physics-timing functions (`setup_module_context`, `process_halo_evolution`, and the two smaller members of the same family) ride along in the same change set and evidence run, with a stop-and-defer guard if the merge turns out to be anything more than the verified pure parameterization. **Being closed by this change set** (galaxy-pool fix, HDF5 status checks, duplication consolidation).
3. **[new] Schedule and execute the converter scale-engineering pass** — external-merge rank sort, streaming/per-snapshot validation, a batch-aware scatter inventory compatible with consumptive deletes, shared/memory-mapped forest-map distribution, and a measured retain/optimize decision on the fix-up stage's sequential per-satellite scan — gated on the micro-Uchuu validation battery + topology cross-check re-running green, plus a measured memory profile of the rank pass at projected Shin-Uchuu scale. See `SHIN-UCHUU-CONVERSION-PLAN.md`'s "Pre-conversion obligation" subsection. **Open** — deserves its own frozen implementation plan; the single largest unscheduled item between here and Shin-Uchuu.
4. **[new] Partition the snapshot-run output before the production run** (recommended: one file per requested output snapshot) — its own small frozen slice with its own gate re-run, scheduled back-to-back with item 2. A `sage16` micro-Uchuu run already writes five partitions on the tree-ordered side but one single-partition file on the snapshot-ordered side; at Shin-Uchuu scale that single file would be hundreds of GB to TB, operationally unmanageable and the worst blast radius for exactly the failure class item 2's HDF5-status fix addresses. **Open — pending the owner's option choice** (D5 in the joint review; raised by the owner after the first review draft, not yet decided).
5. Recompute the driver's memory peak (§2.2) — now explicitly counting the output buffer's full-seed-capacity residency and the reader's staging-buffer transients (both folded into §2.2 above), and recording the conclusion against the 85%-of-RAM projection trigger rather than leaving it implicit. **Open.**
6. Check the projected z=0 output population against **10⁹** (`MAX_HALO_ARRAY_SIZE`, §2.3), not only the `INT_MAX` counter cap. **Open** (record correction only — the projected 315M z=0 slab sits well under 10⁹).
7. Confirm the particle mass from the simulation documentation (§2.4). **Open.**
8. Calibrate the remaining property ranges (§2.5) — with the clarification that `deltaMvir` is a core output range, not a package catalog range. **Open.**
9. **Run the identity gate on a Shin-Uchuu subset before the full run, as a complete rehearsal** (§2.6) — both models including a full `sage16` pass, a subset spanning the earliest snapshots through z=0, output written and read back; this is also the first end-to-end exercise of the 10¹⁰ multiplier and the natural place to measure the output-writer loop's cost and the §2.2 memory profile. **[new — widened]** the rehearsal scope beyond the identity-only run this section previously described. **Open.**

Nothing in §3 blocks the conversion.
