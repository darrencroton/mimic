# Mimic Snapshot Driver Implementation Plan (Dual-Driver Phase 5)

**Status:** Frozen implementation plan for pathway item 4 (dual-driver Phase 5: snapshot driver + cross-format identity gate). Planned 2026-08-10 against `feature/ctrees-snapshot-reader` at `ae22d278`. Every `file:line` reference in this plan was re-verified against that commit during planning; where this plan and the repository disagree, the repository wins, and an implementer who finds such a disagreement stops and reports rather than improvising.
**Contract inputs:** `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md` **Phase 5 section** (the sole architectural input — every design decision there is settled and is not reopened here), `docs/dev/SNAPSHOT-HDF5-FORMAT.md` (frozen on-disk contract, `format_version = 1`, consumed unchanged except for the one narrow Simulation Package Integration edit that the dual-driver plan's recorded input 3 authorizes), and `docs/VISION.md` (architectural principles).
**Scope:** Add `run_snapshot_driver()` and everything the dual-driver plan's Phase 5 section assigns to it: the explicit-input-view refactor, the identity-field slab arrays, physical header agreement, the instanced galaxy pool, the `int64_t` output-width contract, the configured identity multiplier, the driver-neutral output seam, the snapshot driver itself, and the cross-format identity gate on micro-Uchuu. Shin-Uchuu production conversion, snapshot-global module contracts, MPI/domain decomposition, and the embedded engine are out of scope and owned by their own plans.
**Execution mode:** Mode B (`project-manager`). Eleven atomic slices, in plan order, no batches. Every slice is independently gateable.

**Supersession banner (D5(a), 2026-08-13).** D5(a) (`docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md`) superseded two design statements this plan makes repeatedly in current tense: that a snapshot-ordered run writes a **single** output partition, and that incomplete-output cleanup is **all-or-nothing**. What shipped instead: `MimicConfig.NOUT` partition files, one per requested output snapshot (output id = the snapshot number), plus a master; and per-partition cleanup — a partition file that has closed cleanly survives a later failure, only the in-flight partition and the never-written master are removed. Every statement below phrased in either of those two now-superseded ways carries its own adjacent dated "Superseded by D5(a)" note; none is rewritten. Affected line references (re-verify against the live document if it has moved since this banner was written): partition-source contract (`:569`, noted `:571`); master-file external link (`:573`, noted `:575`); source-contract test pin (`:588`, noted `:590`); manual-check reference (`:634`, noted `:636`); the Slice 9 Loop bullet (`:647`, noted `:649`); the Slice 9 incomplete-output cleanup bullet — **failure semantics, read this one carefully** (`:653`, noted `:655`); the fixture acceptance criterion (`:663`, noted `:665`); the interruption acceptance criterion — **failure semantics** (`:669`, noted `:671`); and the Definition of Done closing summary (`:874`, noted `:876`). See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.

---

## Context

Phase 4b (the snapshot reader) is complete: `snapshot_hdf5` is registered behind its own `SnapshotReader` vtable, validates a dataset fully at `open_run`, and loads slabs under a defined lifecycle — but has **no caller in `src/`**; a snapshot-ordered configuration validates cleanly and aborts at `run_processing_driver()` (`src/core/tree_driver.c:526-537`). The three pre-Phase-5 items and the wider W1–W8 residual list all landed by 2026-08-10 (commits `77ab8462` through `ae22d278`), including the `tests/unit/` package-dependence sweep, the git-worktree Makefile fix (`Makefile:310`, `:328`), and the `UniqueGalaxyID` description's `+ 1` correction. The independent readiness review returned PASS WITH RISKS with no P0/P1, and its three findings are implemented. Phase 5 therefore starts from a proven reader, a regenerated 50-snapshot micro-Uchuu dataset (22,580,924 halos, 440,651 forests, `max_halo_rank_in_forest = 350074`), and a green default-pair suite.

What makes this phase different from Phase 4b, and what shapes the slice order below: Phase 4b was structurally unable to change a scientific result, because the driver aborted before any physics ran. Phase 5 has no such protection — it touches virial coupling, output marshalling, and identity encoding at once. The slices are therefore ordered so that (a) every shared-seam change lands **before** any snapshot-driver code exists and is proven neutral by a bitwise tree-path check plus targeted unit tests, and (b) an end-to-end snapshot run with a first identity comparison happens as early as the code allows (Slice 9), so the full gate (Slice 10) measures against a known-good baseline instead of bisecting a divergence across several interacting changes.

---

## The Gate, Stated Once

Running the same micro-Uchuu data through the tree-ordered driver and through the new snapshot-ordered driver must produce, **for every output snapshot, the same set of `UniqueGalaxyID`s, and for each ID every output field identical as raw bytes** (bitwise; any tolerance would be an explicitly reviewed exception, and none is granted by this plan). The tree-ordered path must stay byte-identical at the galaxy-record level throughout, with **exactly four** permitted HDF5 metadata deltas (enumerated in the Definition of Done) and exactly two expected fields changed in the run-local `metadata/output_schema.json` (the `UniqueGalaxyID` description and `source_md5`).

---

## Discovery Record

Facts verified against the repository at `ae22d278` on 2026-08-10. The slices rely on them; each slice re-verifies the references it builds on before coding.

**The dispatch seam.** `run_processing_driver()` (`src/core/tree_driver.c:526-537`) switches on `MimicConfig.ProcessingOrder`; the `INPUT_PROCESSING_ORDER_SNAPSHOT` case is `FATAL_ERROR("The snapshot-ordered driver is not implemented yet")`. `main.c:413` calls it unconditionally on every rank; `MPI_Barrier` follows at `main.c:420`; `NTask` is set at `main.c:313`, before `read_parameter_file()` at `main.c:372`, so config-time validation can see the rank count. `--skip` is parsed at `main.c:255-256` into `MimicConfig.OverwriteOutputFiles = 0` and is consumed only by the tree driver's partition-claim logic (`tree_driver.c:273-292`). Config validation is `validate_and_postprocess()` (`src/core/read_parameter_file.c:1346-1495`); the reader/order compatibility branch at `:1410-1434`, the snapshot `tree_name` literal check at `:1439-1444`, and the tree-ordered non-default-multiplier rejection at `:1450-1457` all live there, and new snapshot-ordered config rejections belong beside them. The Phase 4b tests covering that block live in `tests/integration/test_processing_order.py`, which contains the tree-ordered non-default-multiplier rejection test and both multiplier precedence cases (`:309-408`; rejection at `:351-372`, precedence at `:375-408`) — any slice that changes the block's behaviour must be authorized to edit that file.

**The global-array coupling (recorded input 1).** Every generated accessor in `src/include/generated/tree_property_accessors.h` reads the file-scope global `InputTreeHalos` (`src/core/allvars.c:31`) with an `int` index. `get_virial_mass`/`get_virial_velocity`/`get_virial_radius` (`src/core/virial.c:44`, `:68`, `:99`) reach halo data only through those accessors; declarations in `src/include/proto.h:37-39`. Three tree-driver sites read the **progenitor** generation while processing a descendant: `find_most_massive_progenitor` (`src/core/build_model.c:183-206`), `count_progenitor_galaxies` (`:232`), and `gather_progenitor_galaxies` (`:355-370`). `output_rvir_conditional`/`output_vvir_conditional` (`src/module_system/output_helpers.h:64`, `:78`) recompute virial quantities during output conversion inside `prepare_halo_for_output` (`src/io/output/util.c:67`), whose only callers are `save_halos` (`src/io/output/binary.c:104`) and `save_halos_hdf5` (`src/io/output/hdf5.c:304`). The generator's `calculate` emission writes `payload.{name} = {func}(halonr);` (`scripts/generate_properties.py:1065`), and the tree payload populator is generated by `generate_populate_halo_payload_from_tree` (`:1022`, registered at `:1928-1929`). `scripts/check_generated.py:46-61` holds the hand-maintained `PROPERTY_GENERATED_FILES` list. No physics module calls the accessors or the virial helpers directly (verified by grep over `src/` and `models/`; the only `models/` hits are comments), so the module ABI is untouched by the view refactor. `src/io/tree/hdf5.c:194` uses accessors in a debug dump and must pass the view too. Generated files under `src/include/generated/` are untracked build artefacts (gitignored), so "regenerate" never produces a tracked diff.

**Identity encoding.** `src/include/galaxy_id.h` helpers (`:15-17`, `:30-32`) are hard-coded to `TREE_MUL_FAC` (`src/include/constants.h:38`); the encoder is `halonr + TREE_MUL_FAC × (forestnr_global + 1)`. The tree side computes IDs in `make_unique_galaxy_id` (`src/core/build_model.c:268-282`), whose diagnostic at `:278` prints the compile-time constant. Three tree-reader sites also guard against the constant directly: `src/io/tree/read_ctrees_ascii.c:684`, `src/io/tree/read_ctrees_hdf5.c:387`, `:770`. `MimicConfig.UniqueGalaxyIDMultiplier` is seeded at `read_parameter_file.c:155` and already parsed with precedence; the snapshot reader bounds-checks it at `open_run` via `snapshot_identity_bounds_valid()` (`src/io/snapshot/interface.c`), whose bound is `n_forests_total <= INT64_MAX / multiplier - 1` — one stricter than `galaxy_id.h:15-17`'s `(LLONG_MAX - (M-1))/M` exactly when the multiplier divides 2⁶³, equal at 10⁹ and 10¹⁰. `src/core/core_properties.yaml:79` describes `UniqueGalaxyID` as `creation_halonr + 10^9 * (forestnr_global + 1)` — the `+ 1` is correct (fixed 2026-08-10); the hard-coded `10^9` is Phase 5's to generalise.

**Output widths and writers.** `struct OutputBuffer` uses `int count`/`int capacity` and `struct OutputBufferSegment` carries `int` ranges (`src/core/output_buffer.h:15-28`); growth is capped at `MAX_HALO_ARRAY_SIZE = 1000000000` (`src/include/constants.h:43`, enforced at `src/core/output_buffer.c:48-52`). `inherit_descendant_halos` takes and returns `int` counts (`src/core/inheritance.h:39-42`) and calls the singleton `galaxy_pool_alloc()` at `src/core/inheritance.c:25` and `:101`; `galaxy_pool.h:38` takes no pool argument, and the pool API's other production caller is the tree reader's `free_unit_halos()` (`src/io/tree/interface.c:183` calls `galaxy_pool_reset()`), with direct test callers in `tests/unit/test_inheritance.c` and `tests/unit/test_output_buffer.c`. The per-snapshot output total is the `int` global `TotHalosPerSnap[]` (`src/core/allvars.c:51`, declared `src/include/globals.h:110`), incremented through the overflow-guarded counter at `src/io/output/util.c:52-58` and written raw as a 4-byte `int` into every binary output header (`src/io/output/binary.c:148`). Every snapshot group in per-file HDF5 output unconditionally writes an `int` `Ntrees` (`src/io/output/hdf5.c:203-206`), an `int` `TotHalosPerSnap` (`:210-214`), and the `TreeHalosPerSnap` dataset (`:225-233`); the master file links that dataset unconditionally (`src/io/output/master_hdf5.c:112-114`) and reads/republishes `TotHalosPerSnap` as `H5T_NATIVE_INT` (`:128-145`). `MimicConfig.reader` is dereferenced unguarded at `master_hdf5.c:46-55`, `:77-80`, `:151-152` and `metadata_hdf5.c:582` — all provably unreachable today because the driver aborts first. The `hdf5_format_version` string is `"1.1"` with the increment rule stated at `metadata_hdf5.c:110-115`. Per-file metadata is written by `write_perfile_metadata()` (`metadata_hdf5.c:435-456`) and the master's configuration table by `store_run_properties()` (`:477-524`), which already has an int64-attribute precedent (`CONFIG_PARAM_INT64`, `:551-554`). The metadata version group also stamps four **per-build provenance attributes** — `git_commit`, `git_branch`, `git_date`, `build_date` (`metadata_hdf5.c:85-110`) — and the master's `/RunProperties` stamps the wall-clock `RunEndTime` (`metadata_hdf5.c:599-606`); all five are legitimately build/run-variant provenance (they may coincide between two runs, but carry no scientific content and cannot be pinned), so every before/after HDF5 metadata comparison in this plan **excludes exactly those five attributes and nothing else**; each slice's check restates this. HDF5 output filenames are `<output_directory>/<output_filename>_%03d.hdf5` per partition and `<output_directory>/<output_filename>.hdf5` for the master (`src/io/output/util.c:34-38`, `master_hdf5.c:33-39`). Incomplete-output cleanup is tree-driver-static (the `current_output_paths` registry and its functions at `tree_driver.c:41-80`), invoked from the failure-exit hook at `src/core/main.c:132`; the master file is written only after the driver returns (`main.c:432`), so a cleanup lifecycle that disarms when the driver exits cannot cover a master-write failure.

**Snapshot reader state.** `struct SnapshotSlab` is `{int64_t snapnum, int64_t nhalos, struct RawHalo *halos}` with `snapnum == SNAPSHOT_SLAB_NO_SNAPSHOT` as the empty-state marker (`src/io/snapshot/reader.h:76-96`); the vtable is `open_run`/`close_run`/`snapshot_halo_count`/`load_slab`/`release_slab` (`:98-118`). `snapshot_reader_open_run()` (`interface.c:50`) has no caller outside the fixture unit tests. The reader's on-disk schema table names `ForestIndex` and `HaloRankInForest` at `read_snapshot_hdf5.c:153-154`; per-operation handles are opened by `snapshot_h5_open_scan()` (~`:527`) and `snapshot_h5_read_column()` (~`:631`); the measured-identity scans run at `~:996-1006`. The fixture unit test names both `RawHalo` members directly (`CHECK_I64` at `simulations/micro-uchuu-snapshot/_tests/unit/test_unit_snapshot_reader_open.c:1221-1222`) and fails to compile if they leave the struct. Unit conversions: the header stores `particle_mass_msun_h` in native Msun/h, written by the converter as configured `particle_mass × 1e10` (`scripts/convert/hdf5_writer.py:127`), while `MimicConfig.PartMass` is parsed in `1e10 Msun/h` (`read_parameter_file.c:972-974`) and `BoxSize` in Mpc/h (`:966`).

**Test and build mechanics.** `Makefile:112` discovers all of `src/` recursively; `Makefile:272` filters out `%hdf5.c` under `USE-HDF5=no`, so a plainly-named `snapshot_driver.c` compiles in every build and must not make unguarded HDF5 writer calls. `tests/unit/run_tests.sh` maintains hand-edited **shared-source** lists (`CORE_SRCS` includes `tree_driver.c` at `:157`; `IO_SRCS` at `:158-160`) — a new production source a shared object references must be added there or the unit runner fails to link — while the unit **tests** themselves are glob-discovered (`scripts/generate_test_registry.py:39`), so new test files need no registration. The generator's accessor emission is pinned by `tests/integration/test_unit_contract_generation.py:138-145`, which asserts the emitted text contains `InputTreeHalos[halonr].<field>` — any accessor-emission change must update those assertions. The identity helpers have callers beyond the three guard sites: `src/core/tree_driver.c:182-185` and `tests/unit/test_galaxy_id_encoding.c` (which exercises all four helpers with today's signatures). The standalone topology-dump tool `tests/unit/tools/dump_ctrees_topology.c:116` calls `galaxy_pool_init(0)` and is built by `make dump-ctrees-topology-tool` (`Makefile:829-834`) with its own hand-maintained source list (`tests/unit/tools/build_topology_dump.sh:96-108`, which deliberately excludes `tree_driver.c`, `build_model.c`, and `main.c`); `free_unit_halos()` is also called by the package-local loading tests in the two Millennium packages. The per-tree input counter `InputHalosPerSnap` is allocated only by the tree reader (`src/io/tree/interface.c:54`), incremented unconditionally by the output counter helper (`src/io/output/util.c:59`), and written as `TreeHalosPerSnap` (`src/io/output/hdf5.c:240`) — a snapshot run that reaches the save path without a tree-only guard would dereference `NULL`. `MaxProcessedHalos` is sized and narrowed at `src/io/tree/interface.c:149-160`. `tests/framework/data_loader.py:354` defaults `expected_format_version="1.1"` and `tests/integration/test_output_formats.py` applies it to fresh output and to the tracked 1.1 baselines — **both halves superseded 2026-08-14; see the dated note under Slice 8's format-version bullet below.** The scientific tier (`Makefile:808-827`) builds exactly **one** ambient `MODEL`/`SIMULATION` pair and then runs the scripts in the generated registry; `scripts/generate_test_registry.py` registers `tests/scientific/test_*.py` unconditionally and `simulations/<selected>/_tests/scientific/test_*.py` for the selected package only. `tests/framework/harness.py:227` hard-codes the repo-root `mimic` executable, and no existing test builds a second pair. `scripts/discovery.py:41-56` lists `micro-uchuu-ascii` (but not `micro-uchuu-snapshot`) in both `FULL_MODEL_TEST_SIMULATIONS` and `PRODUCTION_TEST_CONFIG_SIMULATIONS`; nothing in the scientific tier consumes those lists. The committed run files `models/halos-only/input/halos-only_micro-uchuu-ascii.yaml` (`SubSteps: 1`) and `models/sage16/input/sage16_micro-uchuu-ascii.yaml` (`SubSteps: 10`) both use `output_format: hdf5`, `snapshot_list: [49, 28, 23, 16, 12, 10, 8, 7]`, and no `TimestepScheme` key (fixed scheme). `simulations/micro-uchuu-ascii/simulation_info.yaml:18` sets `forests_per_file: 100000` against 440,651 forests, so tree-side micro-Uchuu output has **five** numbered partitions. `scripts/convert/crosscheck.py`'s `load_reference_galaxies()` (`:172-224`) is the partition-aggregation pattern to follow. The bitwise tree-path vehicle is the generated `build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml` (`output_format: binary`), exactly as Phase 4b's Slice 4 froze it; worktree builds work since the `Makefile:310`/`:328` fix.

**Struct sizes** (measured 2026-08-04 on the default pair, recorded in the dual-driver plan): `RawHalo` 104 B, `Halo` 176 B, `GalaxyData` 176 B, `HaloOutput` 264 B. Phase 5 implements two complete raw slabs unconditionally; the micro-Uchuu fixture is nowhere near any memory ceiling, and the projection fallback belongs to `SHIN-UCHUU-CONVERSION-PLAN.md`, not here.

---

## Design Decisions (frozen for this plan; architecture already settled by the dual-driver plan)

These are planning-level mechanics the dual-driver plan's Phase 5 section explicitly leaves to this plan, plus naming. No architectural decision is made here.

1. **The input view is `struct HaloInputView { const struct RawHalo *halos; int64_t count; }`**, defined in `src/include/types.h`, passed by value. The accessor family keeps its `mimic_tree_get_*` names (the prefix is historical; renaming ~40 generated symbols would bloat every diff this phase produces) and gains the view as its first parameter. No new runtime checks are added in this refactor — behaviour must be provably unchanged, and a bounds assert would be a behaviour change on corrupt input.
2. **The shared populator is renamed** `populate_halo_payload_from_tree.inc` → `populate_halo_payload.inc`, since after recorded input 1 it is view-based and driver-neutral. The rename lands in Slice 1 together with the `scripts/check_generated.py` list update (dual-driver Phase 5 item 4's registration requirement).
3. **The identity comparator is a committed script**, `scripts/compare_cross_format_identity.py`, landing in Slice 9 (where the first comparison runs) and consumed by Slice 10's harness. One implementation of the frozen comparison algorithm, used everywhere.
4. **The identity-gate test is package-local**: `simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py`. It is discovered by the existing scientific-tier registry only when `SIMULATION=micro-uchuu-snapshot` is selected, which reconciles the frozen gate's "fail loudly rather than skip when either dataset is absent" with the default-pair suite and CI, where the datasets do not exist: a default-pair `make tests` never registers the test, and a fixture-pair `make tests-scientific` on a machine without the datasets fails loudly, as specified. It is "in the scientific tier" in exactly the sense the tier is defined: registered and run by `RUN_PYTHON_TIER`'s scientific registry.
5. **The gate harness builds each pair in an isolated git worktree.** The scientific tier provides one ambient build, and the gate needs four (`{halos-only, sage16} × {micro-uchuu-ascii, micro-uchuu-snapshot}`); rebuilding pairs in the working tree would corrupt the ambient build that the rest of the tier assumes. The harness creates one worktree per pair at the current `HEAD`, recreates the machine-local gitignored `snapshots` symlinks inside it (resolved from the main tree's own symlinks), builds with explicit selectors, and runs both timestep schemes from it. The worktree Makefile fix (pre-Phase-5 item 2) exists precisely so this works.
6. **The skeleton driver (Slice 4) ends in a deliberate FATAL.** Until the full driver lands, a snapshot-ordered run must never exit 0 without output — a "successful" run producing nothing would be a silent lie. The skeleton opens, validates, loads and releases every slab under the two-generation rotation pattern, then aborts with an explicit "cannot yet produce output" message.
7. **The `hdf5_format_version` bump (delta 4) lands in the same slice as the `TotHalosPerSnap` widening (delta 2)** — Slice 8 — because the Definition of Done ties the bump to that schema change. Slice 7's new provenance attribute (delta 1) therefore briefly precedes the bump on the branch; this transient is internal to the phase and the end state is exactly the four enumerated deltas.

The following Phase 4b deferred entries are **explicitly not Phase 5 scope**, because the dual-driver plan's Phase 5 section (the sole planning input) does not require them: the shared-HDF5-read-utilities refactor (downgraded to optional by that plan, and nothing in this plan independently requires an identical primitive); the empty-dataset-with-non-sentinel-metadata reader check; the reader strictness gaps (nested `/header` objects, same-size datatype acceptance); and `scripts/discovery.py` test-gating membership for `micro-uchuu-snapshot` (nothing in the gate consumes those lists). All four remain recorded in the Phase 4b plan's Deferred section and survive its archival via Slice 11's closeout notes. Nothing else from that section is silently dropped: every other entry is either done (dated in place) or in scope below.

---

## Selector Discipline (binds every slice)

Generated files are untracked build artefacts regenerated per `MODEL`/`SIMULATION` pair, and `make check-generated` compares against the *currently selected* pair's inputs. Therefore, in every slice: **pass `MODEL=` and `SIMULATION=` explicitly on every `make` and test command, and immediately before any default-pair check, first run `make MODEL=sage16 SIMULATION=mini-millennium generate`.** The default pair is `sage16`/`mini-millennium`; the fixture pair is `halos-only`/`micro-uchuu-snapshot`; the gate pairs are `{halos-only,sage16}` × `{micro-uchuu-ascii,micro-uchuu-snapshot}`. Never mix selectors within one check sequence. Delegate the long unit/integration tiers to a subagent that captures logs under `archive/test-logs/` and returns pass/fail with exit codes; never run test suites in parallel.

---

## Implementation Profiles

- Mode B (`project-manager`) executes all eleven slices atomically in plan order. Batches are not used and are not defined.
- Slices 1, 7, 9, and 10 deserve the strongest available implementer (generator + physics coupling; identity encoding; the driver itself; the gate harness). Slices 3, 4, and 11 are suitable for a cheaper implementer. Slices 2, 5, 6, and 8 sit between.
- That tiering is recorded per slice as a **Developer seat** line under each slice heading, in `pm start-slice` flag form. The lines are the PM's recommended default, not an acceptance criterion: the PM may raise a seat on evidence and records the choice per slice either way.
- Suggested run defaults: PM seat `opus` at medium effort (the seat is turn-heavy and judgement-bound, not frontier-reasoning-bound; low effort degrades exactly the diff reading it exists for). `init --model sonnet --effort high` as the Developer default, overridden per the seat lines. `--max-attempts 5`, so a struggling slice surfaces to the human rather than grinding.
- Every slice except Slice 3 carries `Independent audit required: yes` — this phase can move galaxy numbers, and the cost of an unaudited drift here is a divergence discovered at the gate. Review cost therefore dominates this run, which is the main argument for the stronger Developer seats above: on an elevated slice a single steer costs an attempt *plus* re-running both mandatory reviews, since any tree change stales them.

---

## Slice 1: Explicit input view through the accessor, virial, payload, and output-conversion paths

**Developer seat:** `--model opus --effort high` — Strongest implementer (plan profile): generator emission, the virial/payload/output-conversion threading, and a bitwise-neutrality obligation on the tree path.

### Intended Change
- Define `struct HaloInputView { const struct RawHalo *halos; int64_t count; }` in `src/include/types.h`.
- `scripts/generate_properties.py`: generate the `mimic_tree_get_*` accessor family (`src/include/generated/tree_property_accessors.h`) to take `struct HaloInputView view` as the first parameter and read `view.halos[halonr]` instead of the global `InputTreeHalos`; change the `init_source: calculate` emission at `scripts/generate_properties.py:1065` from `payload.{name} = {func}(halonr);` to `payload.{name} = {func}(view, halonr);`; make the payload populator view-based and rename it `populate_halo_payload_from_tree.inc` → `populate_halo_payload.inc` (one shared populator; no `_from_snapshot` sibling will ever exist, per the dual-driver plan's recorded input 1); make the generated `copy_to_output.inc` emission pass the view to the conditional output helpers.
- `scripts/check_generated.py`: update the `PROPERTY_GENERATED_FILES` entry (`:54`) for the rename.
- `src/core/virial.c` and `src/include/proto.h:37-39`: `get_virial_mass`/`get_virial_velocity`/`get_virial_radius` (and any sibling in that file reached from them) take `struct HaloInputView view` as their first parameter and thread it to every accessor call.
- `src/module_system/output_helpers.h:64`, `:78`: `output_rvir_conditional`/`output_vvir_conditional` take the view and pass it to the virial helpers.
- `src/io/output/util.{c,h}` and `src/include/proto.h`: `prepare_halo_for_output(struct HaloInputView view, const struct Halo *g, struct HaloOutput *o)`; `src/io/output/binary.c:104` and `src/io/output/hdf5.c:304` thread the view through `save_halos(int filenr, int tree, struct HaloInputView view)` / `save_halos_hdf5(...)` (exact threading signature is the implementer's, provided no global is read below the driver), updating their public declarations in `src/io/output/binary.h:19` and `src/io/output/hdf5.h:24`; `src/core/tree_driver.c:236-241` passes the tree driver's view at the save call sites.
- `src/core/build_model.c`: construct the tree driver's view once per loaded unit from `InputTreeHalos` and the loaded unit's halo count, and pass it at every accessor/virial/populator call site, including `find_most_massive_progenitor`, `count_progenitor_galaxies`, `gather_progenitor_galaxies`, and the descendant-side virial/time precomputation. `InputTreeHalos` remains the tree driver's storage; nothing below the driver reads it as a global any more.
- `src/io/tree/hdf5.c:194`: the debug dump passes the view it just loaded.
- `tests/integration/test_unit_contract_generation.py:138-145`: the accessor-emission assertions update from `InputTreeHalos[halonr].<field>` to the view-based emission (`view.halos[halonr].FirstProg`, `.Snap`, `.NPart`, `.Mass200`) — these assertions pin the generator contract and fail deterministically otherwise. Only those emission-text assertions may change.
- `docs/DEVELOPER-GUIDE.md:1189`: the generated-files table row naming `populate_halo_payload_from_tree.inc` updates to the renamed `populate_halo_payload.inc` — this one cell only, so the documentation of record never names a file that no longer exists.
- Direct unit tests for view correctness (see Validation Plan): the bitwise tree-path check alone cannot catch a wrapper that still reads the global or a current/previous view swapped where indices happen to overlap.

### Acceptance Criteria
- Inputs: `struct RawHalo` arrays as loaded by the existing tree readers; `halonr` indices in `[0, view.count)`. Behaviour for out-of-range indices is unchanged from today (no new checks are added; this is a plumbing refactor with provably identical behaviour).
- Outputs: identical galaxy output, byte for byte, on the tree path.
- [ ] No function outside `src/core/build_model.c`, `src/core/tree_driver.c`, and the tree readers reads `InputTreeHalos`; `grep -rn "InputTreeHalos" src/ --include='*.c' --include='*.h'` output is recorded in the slice evidence and every remaining hit is in those files or `allvars.{c,h}`.
- [ ] Generated `tree_property_accessors.h` accessors take the view; no generated accessor names `InputTreeHalos`.
- [ ] The generated populator is `populate_halo_payload.inc`, view-based; `populate_halo_payload_from_tree.inc` is no longer generated or referenced; `make MODEL=sage16 SIMULATION=mini-millennium check-generated` exits 0.
- [ ] The `init_source: calculate` emission produces `func(view, halonr)` and the default-pair build compiles it clean.
- [ ] `prepare_halo_for_output` takes the view and no file under `src/io/output/` reads `InputTreeHalos` or calls a virial helper without a view.
- [ ] New unit tests populate **two** `RawHalo` arrays with deliberately different values at the same indices and prove: (a) accessor and virial reads through view A never return view B's values and vice versa, interleaved in one test; (b) `prepare_halo_for_output` conversion through view A is unaffected by pointing the unrelated `InputTreeHalos` global at view B's array; (c) the payload populator fills `HaloInitPayload` from the view it is handed, not from the global. All three pass under both the default pair and the fixture pair.
- [ ] A default-pair tree-ordered binary run's galaxy output is byte-for-byte identical before and after this slice (procedure in the Validation Plan), and the comparison evidence is recorded.
- [ ] Behaviour that must not change: the default-pair unit, integration, and scientific tiers are green; `make USE-HDF5=no` builds clean; no physics value, output record, or schema field changes; the module ABI (`process(ctx, halos, ngal)`) is untouched.

### Authorized Surface
- Files allowed to change:
  - `scripts/generate_properties.py`
  - `scripts/check_generated.py`
  - `src/include/types.h`
  - `src/include/proto.h`
  - `src/core/virial.c`
  - `src/core/build_model.c`
  - `src/core/tree_driver.c`
  - `src/module_system/output_helpers.h`
  - `src/io/output/util.c`
  - `src/io/output/util.h`
  - `src/io/output/binary.c`
  - `src/io/output/binary.h`
  - `src/io/output/hdf5.c`
  - `src/io/output/hdf5.h`
  - `src/io/tree/hdf5.c`
  - `tests/unit/test_virial_properties.c`
  - `tests/unit/test_input_view.c` (new file)
  - `tests/integration/test_unit_contract_generation.py`
  - `docs/DEVELOPER-GUIDE.md`
- Functions/classes/components allowed to change: the generator's accessor/populator/`calculate`/`copy_to_output` emission functions and the `PROPERTY_GENERATED_FILES` list; the three virial helpers and their declarations; the view-threading call chain named in Intended Change (`find_most_massive_progenitor`, `count_progenitor_galaxies`, `gather_progenitor_galaxies`, the workspace/context setup that precomputes virial quantities, the save call sites, `save_halos`, `save_halos_hdf5`, `prepare_halo_for_output`, the two conditional output helpers, the tree-HDF5 debug dump). In `tests/integration/test_unit_contract_generation.py`, only the accessor-emission text assertions at `:138-145`; in `docs/DEVELOPER-GUIDE.md`, only the one generated-files table cell at `:1189`. New unit tests are glob-discovered and need no registration.
- Tests allowed or expected to change: `tests/unit/test_virial_properties.c` (rewritten to drive explicit views), new `tests/unit/test_input_view.c`, and the restricted `test_unit_contract_generation.py` assertions.

### Explicit Non-Goals
- No snapshot-driver code, no `src/core/snapshot_driver.c`, no change under `src/io/snapshot/`.
- No new runtime bounds checks, assertions, or diagnostics anywhere on the refactored paths.
- No change to `struct RawHalo`, any property YAML, `galaxy_id.h`, the galaxy pool, `output_buffer.{c,h}`, or `inheritance.{c,h}`.
- No accessor renames beyond the populator file rename; no module-facing API change.
- No removal of the `InputTreeHalos` global itself — it remains the tree driver's storage.

### Risk Flags
- Risky surfaces touched: generated-code emission consumed by every build; the physics-coupled virial path; the output-conversion path shared by both output formats.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: `tests/unit/test_input_view.c` (the two-array cases in the Acceptance Criteria); `tests/unit/test_virial_properties.c` updated to the view signatures with its existing assertions preserved.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium check-generated && make MODEL=sage16 SIMULATION=mini-millennium validate-modules`
  - `make MODEL=sage16 SIMULATION=mini-millennium && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific` (delegate the long tiers; capture logs under `archive/test-logs/`; check exit codes)
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot && MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh`
  - `make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no clean && make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no`
  - Bitwise tree-path check (the Phase 4b Slice 4 procedure, repeated verbatim): capture "before" from the unmodified tree at the slice start — `git worktree add output/bitwise-base <before_head>`, then in the worktree `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium`; on each side run a scratch copy of `build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml` with `output_directory` rewritten to `output/bitwise-before/` or `output/bitwise-after/`; then `for f in output/bitwise-before/model_*; do cmp "$f" "output/bitwise-after/$(basename "$f")" || echo "DIFF $f"; done` must print nothing and both directories must hold the same file names. Clean up the worktree and scratch dirs afterwards.
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the generated accessor header diff (before/after `make generate`) and confirm the only change per accessor is the view parameter and array source; confirm `git diff --stat` touches no file outside the authorized surface.

### Rollback Path
- Revert the commit and re-run `make generate` for the active pair; generated files are untracked, so the revert restores the emission and the next generate restores the artefacts.

## Slice 2: Identity fields out of the property system, onto reader-owned slab arrays

**Developer seat:** `--model sonnet --effort high` — Mechanical but atomic: a YAML removal plus reader-owned slab arrays and their tests, fully specified and touching no core/IO tree-path file.

### Intended Change
- Remove the `ForestIndex` and `HaloRankInForest` entries from `simulations/micro-uchuu-snapshot/halo_properties.yaml`, so they are no longer catalog fields, no longer `RawHalo` members, and generate nothing (dual-driver recorded input 3, option D).
- `src/io/snapshot/reader.h`: `struct SnapshotSlab` gains `int64_t *forest_index` and `int64_t *halo_rank_in_forest`, reader-owned parallel arrays allocated, read, validated, and freed with the slab; the empty state, `SNAPSHOT_SLAB_INIT`, and `snapshot_slab_is_empty()` cover them.
- `src/io/snapshot/read_snapshot_hdf5.c`: `load_slab` reads the two columns into the new arrays via the existing per-column read path, reusing the schema-table entries at `:153-154` rather than adding a second pair of string/type literals; `release_slab` frees them; the existing `open_run` validation scans (which read the columns by dataset name) are unchanged.
- `simulations/micro-uchuu-snapshot/_tests/unit/test_unit_snapshot_reader_open.c`: the `CHECK_I64` comparisons at `:1221-1222` move from `slab->halos[h].ForestIndex`/`.HaloRankInForest` to `slab->forest_index[h]`/`slab->halo_rank_in_forest[h]`; lifecycle and leak tests cover the new allocations.
- `docs/dev/SNAPSHOT-HDF5-FORMAT.md`: the one authorized narrow correction, covering exactly the two places the dual-driver plan's recorded input 3 cites as requiring `halo_properties.yaml` to declare these fields — the *Halo Datasets* normative sentence (`:68`, which must now exempt the two identity datasets from the "must match what the consuming simulation package's `halo_properties.yaml` declares" rule, since the reader consumes them directly by dataset name) and the *Simulation Package Integration* section (`:143-151`) — plus one Errata row recording the correction with today's date. Nothing on disk changes; both datasets remain required, read, and validated exactly as before; `format_version` stays `1`.
- The whole change lands atomically in this one slice (YAML removal, slab arrays, test updates, regeneration, spec correction), because any partial landing fails to compile or misdescribes the contract.

### Acceptance Criteria
- Inputs: the committed fixture dataset and, opt-in, the full micro-Uchuu dataset.
- Outputs: slabs carrying identity components in reader-owned arrays; a `RawHalo` without the two identity members.
- [ ] `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate` succeeds; the generated `raw_halo_defs.h` no longer contains `ForestIndex` or `HaloRankInForest`; `check-generated` exits 0 for the fixture pair.
- [ ] After regeneration for both micro-Uchuu packages, the generated `RawHalo` field names, types, and order are **identical** between `micro-uchuu-ascii` and `micro-uchuu-snapshot` (mechanical diff of the two `raw_halo_defs.h` artefacts recorded in evidence).
- [ ] Loading each fixture snapshot yields `forest_index`/`halo_rank_in_forest` values equal to the on-disk columns bit-for-bit; the empty snapshot yields empty arrays and releases cleanly.
- [ ] Lifecycle holds: both arrays are `NULL` in the empty state; `release_slab` frees them and returns the handle to empty; double release is safe; `close_run` with a loaded slab still aborts; no `Memory leak detected` diagnostic in captured unit output.
- [ ] Every existing `open_run` validation and corrupt-fixture abort behaves exactly as before this slice (the scans read by dataset name and are unaffected).
- [ ] The committed fixture files are byte-identical across this slice (`git status` clean under `_tests/data/`; no fixture regeneration).
- [ ] `docs/dev/SNAPSHOT-HDF5-FORMAT.md` contains the corrected Halo Datasets normative sentence, the narrowed Simulation Package Integration wording, and one dated Errata row; `format_version` remains 1; no other section of the spec changes (diff recorded in evidence).
- [ ] `simulations/micro-uchuu-snapshot/_tests/input/check_fixture_conformance.py` still exits 0 against the committed fixtures (it owns an independent format-level table and needs no change; if it fails, stop and report rather than editing it).
- [ ] Behaviour that must not change: no file under `src/core/`, `src/io/tree/`, or `src/io/output/` changes; the default-pair three-tier suite is green; the tree path needs no bitwise check because no tree-path file is touched, and the slice evidence records `git diff --stat` proving it.

### Authorized Surface
- Files allowed to change:
  - `simulations/micro-uchuu-snapshot/halo_properties.yaml`
  - `src/io/snapshot/reader.h`
  - `src/io/snapshot/interface.c`
  - `src/io/snapshot/read_snapshot_hdf5.c`
  - `simulations/micro-uchuu-snapshot/_tests/unit/`
  - `docs/dev/SNAPSHOT-HDF5-FORMAT.md`
- Functions/classes/components allowed to change: the `SnapshotSlab` contract and its lifecycle helpers; `load_slab`/`release_slab` and their internals; the fixture package's unit tests. In the spec, only the Halo Datasets normative sentence (`:68`), the Simulation Package Integration section, and the Errata table.
- Tests allowed or expected to change: the fixture package's C unit tests.

### Explicit Non-Goals
- No change to the on-disk format, the committed fixtures, the converter, or `check_fixture_conformance.py`.
- No `format_version` bump and no edit to any spec location other than the three named (the `:68` sentence, Simulation Package Integration, and Errata).
- No driver code; nothing consumes the new arrays yet.
- No change to the `open_run` validation semantics, the schema table's dataset set, or any abort behaviour.

### Risk Flags
- Risky surfaces touched: property YAML driving generated code for the fixture selector; the frozen format specification (one authorized narrow edit); the identity data path the gate depends on.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: slab-array content equality against the fixtures; lifecycle and leak cases for the new arrays.
- Commands to run:
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot && MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh` (captured, exit code checked, no leak diagnostic)
  - `mimic_venv/bin/python simulations/micro-uchuu-snapshot/_tests/input/check_fixture_conformance.py simulations/micro-uchuu-snapshot/_tests/data`
  - `raw_halo_defs.h` identity diff: `make MODEL=halos-only SIMULATION=micro-uchuu-ascii generate && cp src/include/generated/raw_halo_defs.h output/rawhalo-ascii.h && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && diff output/rawhalo-ascii.h src/include/generated/raw_halo_defs.h` — must print nothing
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium check-generated && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `make MODEL=sage16 SIMULATION=mini-millennium check-docs`
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the spec diff word by word against recorded input 3's cost paragraph; confirm the Errata row states that nothing on disk changed.

### Rollback Path
- Revert the commit and regenerate for the fixture pair; the change is confined to the fixture package, the snapshot reader, and one spec section.

## Slice 3: Physical header agreement with configuration at open_run

**Developer seat:** `--model sonnet --effort high` — Cheapest tier (plan profile) and the only slice needing no independent audit; self-contained header validation plus corrupt-fixture tests. Drop to `medium` if trimming cost.

### Intended Change
- `src/io/snapshot/read_snapshot_hdf5.c` (`open_run`): for **every** snapshot file, compare the physical header attributes against the configured simulation and **abort** on mismatch (dual-driver Phase 5 item 8): `box_size_mpc_h` against `MimicConfig.BoxSize`; `omega_matter`, `omega_lambda`, `hubble_h` against the three configured cosmology values; and `particle_mass_msun_h` against `MimicConfig.PartMass * 1e10` — multiplying the configured value up to native units, matching the producer's own operation, never dividing the header down.
- Comparison rule, applied per value: reject non-finite values; require exact equality when both values are zero; otherwise accept iff `fabs(header - configured) <= 16 * DBL_EPSILON * fmax(fabs(header), fabs(configured))`. This is a rounding tolerance asserting the two are the same number, not a scientific tolerance.
- Corrupt-fixture unit tests for each attribute, plus the two named traps: a particle mass differing by exactly the 1e10 unit factor must abort (the naive-comparison trap), and a NaN or infinity in any compared attribute must abort.

### Acceptance Criteria
- Inputs: the fixture dataset (whose headers were stamped from the package's own `simulation_info.yaml`, so it passes by construction) and mutated corrupt copies.
- Outputs: an `open_run` that refuses a dataset paired with the wrong simulation package.
- [ ] `open_run` on the unmodified fixture dataset succeeds under the fixture package's configuration.
- [ ] Each of these aborts naming the file, the attribute, and both values: a perturbed `box_size_mpc_h`; a perturbed `omega_matter`; a perturbed `omega_lambda`; a perturbed `hubble_h`; a perturbed `particle_mass_msun_h`; a `particle_mass_msun_h` equal to the configured `PartMass` **without** the 1e10 factor; a NaN in any one compared attribute; an infinity in any one compared attribute.
- [ ] The check runs for every snapshot file, not only snapshot 0: a mutation in the **last** fixture snapshot's header aborts.
- [ ] A relative perturbation at `4 * DBL_EPSILON` on one compared attribute is accepted (inside the tolerance), and one at `1e-9` relative is rejected — pinning the tolerance from both sides.
- [ ] The existing `scale_factor` exact comparison and every other `open_run` validation are unchanged.
- [ ] Behaviour that must not change: no file outside `src/io/snapshot/` and the fixture package's tests changes; the default-pair three-tier suite is green; the opt-in real-data test still opens all 50 snapshots (the real dataset's headers were stamped from the same package values).

### Authorized Surface
- Files allowed to change:
  - `src/io/snapshot/read_snapshot_hdf5.c`
  - `src/io/snapshot/reader.h`
  - `simulations/micro-uchuu-snapshot/_tests/unit/`
- Functions/classes/components allowed to change: `open_run`'s validation sequence and one new static comparison helper; the fixture package's unit tests.
- Tests allowed or expected to change: the fixture package's C unit tests.

### Explicit Non-Goals
- No exact-equality comparison for the five physical values (the tolerance is frozen as stated), and no tolerance on `scale_factor` (stays exact).
- No warning-instead-of-abort path and no override mechanism.
- No change to the identity-bounds validation, the structural validation, or the gate-side preflight (which is deliberately stricter and lives in Slice 10).

### Risk Flags
- Risky surfaces touched: the snapshot startup-validation path.
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan
- Tests to add/update: the corrupt-header cases and the two tolerance-pinning cases above.
- Commands to run:
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot && MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh` (captured; exit code; no leak diagnostic; the opt-in real-data test must run, not SKIP)
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: confirm the comparison helper multiplies `PartMass` up rather than dividing the header down; confirm every compared value is read as float64.

### Rollback Path
- Revert the commit; the reader returns to Phase 4b validation semantics.

## Slice 4: Snapshot run gating and the driver skeleton (open_run wired into the run path)

**Developer seat:** `--model sonnet --effort high` — Cheaper tier (plan profile): a skeleton driver behind a deliberate FATAL, but it edits `read_parameter_file.c` and `tree_driver.c`, so the bitwise tree-path check is load-bearing.

### Intended Change
- `src/core/read_parameter_file.c` (`validate_and_postprocess()`, beside the existing snapshot-configuration checks at `:1410-1457`): a snapshot-ordered configuration is rejected at config time when (a) `output_format` is `binary` (message: snapshot-ordered runs are HDF5-only), (b) `--skip` was given (`MimicConfig.OverwriteOutputFiles == 0`; message: resume is not supported for snapshot-ordered runs), or (c) `NTask > 1` (message: snapshot-ordered runs are serial in this phase; multi-rank execution belongs to the distributed plan, `docs/dev/MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md`).
- New `src/core/snapshot_driver.c` (deliberately **not** named `*hdf5.c`, so it compiles in every build): `run_snapshot_driver()`, called from the `INPUT_PROCESSING_ORDER_SNAPSHOT` case in `run_processing_driver()` (`src/core/tree_driver.c:526-537`), replacing the "not implemented yet" FATAL. `tests/unit/run_tests.sh` adds `src/core/snapshot_driver.c` to its hand-maintained `CORE_SRCS` list (`:157`) — `tree_driver.c` is in that list and now references `run_snapshot_driver()`, so the unit runner fails to link without it. The skeleton: call `snapshot_reader_open_run()` (its first run-path caller); log the published run metadata; iterate snapshots in increasing time order holding **two** slabs under the production rotation pattern (load slab N while slab N−1 is live, then release N−1); after the final snapshot release the last slab and `close_run`; then `FATAL_ERROR` with a message stating the driver cannot yet produce output. A snapshot-ordered run must never exit 0 in this slice.
- `src/include/proto.h` (or a small driver header): declare `run_snapshot_driver()`. The skeleton makes no HDF5 library calls of its own (the reader interface symbols link in every build; a non-HDF5 build rejects `tree_type: snapshot_hdf5` at configuration long before the driver), so no compilation guard is needed yet; the guard obligation for HDF5 writer calls binds Slice 9 and is restated there.
- `tests/integration/test_processing_order.py`: update the snapshot-ordered expectation — a valid snapshot configuration over the fixture package now opens and validates its dataset, loads and releases every slab, and aborts with the new cannot-yet-produce-output message; add the binary-output and `--skip` config-rejection cases.
- `tests/unit/test_parameter_parsing.c`: a deterministic serial-only check using this file's existing expect-fatal pattern — set the global `NTask` (`src/include/globals.h:16`) to 2, parse a snapshot-ordered fixture, and assert the config-time rejection with the serial-only message; and with `NTask = 2` and a tree-ordered fixture, assert validation still passes. Restore `NTask` after each case.

### Acceptance Criteria
- Inputs: run YAMLs over the fixture package; the committed fixture dataset.
- Outputs: a snapshot-ordered run path that exercises the full Phase 4b + Slice 2/3 reader validation end to end, and fails honestly at output.
- [ ] A valid snapshot-ordered run over the fixture package reaches the driver, opens the dataset (all `open_run` validation runs on the run path for the first time), loads and releases every slab with the two-generation rotation, and aborts with the cannot-yet-produce-output message; the output does **not** contain "Parameter validation failed" and does **not** contain "The snapshot-ordered driver is not implemented yet".
- [ ] During the loop, exactly two slabs are ever live at once, proven by a driver debug log line asserting the live-slab count and by no leak diagnostic after the abort path is exercised in the C unit or integration harness where capturable.
- [ ] `output_format: binary` with a snapshot-ordered configuration fails at config time with the HDF5-only message.
- [ ] `--skip` with a snapshot-ordered configuration fails at config time with the no-resume message.
- [ ] With `NTask = 2`, a snapshot-ordered configuration fails at config time with the serial-only message naming the distributed plan, and a tree-ordered configuration validates exactly as before — both pinned by the new unit cases, which run under both the default pair and the fixture pair.
- [ ] A tree-ordered configuration with `--skip`, binary output, and any `NTask` behaves exactly as before this slice.
- [ ] `make USE-HDF5=no` (default pair) builds and links clean with `snapshot_driver.c` compiled in.
- [ ] A default-pair tree-ordered binary run's galaxy output is byte-for-byte identical before and after this slice (this slice edits `read_parameter_file.c` and `tree_driver.c`, both on the tree path), with evidence recorded.
- [ ] Behaviour that must not change: every existing tree-ordered run file parses, validates, and runs exactly as before; the default-pair three-tier suite is green.

### Authorized Surface
- Files allowed to change:
  - `src/core/read_parameter_file.c`
  - `src/core/snapshot_driver.c` (new file)
  - `src/core/tree_driver.c`
  - `src/include/proto.h`
  - `tests/integration/test_processing_order.py`
  - `tests/unit/test_parameter_parsing.c`
  - `tests/unit/run_tests.sh`
  - `simulations/micro-uchuu-snapshot/_tests/unit/`
  - `tests/framework/core_test_fixtures.h` (amendment 2026-08-11, see below)
  - `tests/unit/test_input_view.c` (amendment 2026-08-11, see below)
  - `tests/unit/test_virial_properties.c` (amendment 2026-08-11, see below)
  - `scripts/generate_test_inputs.py` (amendment 2026-08-11, see below)
- Functions/classes/components allowed to change: `validate_and_postprocess()`'s snapshot-configuration branch only; the `INPUT_PROCESSING_ORDER_SNAPSHOT` case of `run_processing_driver()`; everything in the new `snapshot_driver.c`. In `tests/unit/test_parameter_parsing.c`, the two new `NTask` cases and their registration, plus whatever the shared-fixture correction below requires of that file's setup helpers — no existing assertion may be weakened or removed. In `tests/unit/run_tests.sh`, only the `CORE_SRCS` addition of `src/core/snapshot_driver.c`.
- Tests allowed or expected to change: `tests/integration/test_processing_order.py`; the two new `NTask` unit cases; fixture-package unit tests if a slab-rotation case is best expressed there; and the shared C unit-test fixture correction below.

**Authorized surface amendment, 2026-08-11 (human-approved during the Mode B run that stopped on this slice).** The three config-time rejections above make the fixture pair's *generated* shared C unit-test configuration invalid, and the original surface could not reach the fix. Two distinct breakages, both collateral rather than disagreements about intended behaviour:

1. `MimicConfig.OverwriteOutputFiles` is set to `1` only by `main.c:216` and to `0` by `--skip` at `main.c:256`, both inside the CLI path that no C unit test calls. Under a unit test the field therefore sits at its `memset` zero, bit-for-bit indistinguishable from the specified `--skip was given` condition, so rejection (b) fires on every C unit test that parses a snapshot-ordered configuration.
2. `scripts/generate_test_inputs.py` writes `core/test_binary.yaml` with `output_format: binary` for **every** `MODEL`/`SIMULATION` pair, while `simulations/micro-uchuu-snapshot/simulation_info.yaml` declares `processing_order: snapshot_ordered`. Under the fixture pair that generated file is snapshot-ordered *with binary output*, which rejection (a) correctly refuses — so the file every shared C unit test loads via `test_binary_param_file()` (`tests/framework/core_test_fixtures.h:37-42`) is invalid by construction. `tests/unit/test_parameter_parsing.c`, `tests/unit/test_input_view.c` and `tests/unit/test_virial_properties.c` all load it purely to obtain cosmology.

The four added files exist to fix exactly this. **All three rejections stay as specified — none is relaxed, relocated, or moved out of config time**, and no production behaviour may change. The correction must be the minimum that makes the shared C unit-test fixtures valid for a snapshot-ordered package: `core/test_hdf5.yaml` is already generated for every pair (`scripts/generate_test_inputs.py:205`), so a valid configuration already exists and no new generated artefact is required. Prefer a central fix in `tests/framework/core_test_fixtures.h` over per-test edits, and touch `scripts/generate_test_inputs.py` only if the chosen approach genuinely requires it — a snapshot-ordered package having a `test_binary.yaml` at all is the root cause and will recur for every future snapshot-ordered package, so fixing it there is legitimate. Note that the unit runner detects HDF5 (`tests/unit/run_tests.sh:149`) rather than requiring it, so the correction must not make the default (tree-ordered) pair's unit tier depend on HDF5 being present.

### Explicit Non-Goals
- No FoF walk, no gather, no inheritance, no physics, no output of any kind, no galaxy pool use.
- No change to any tree-ordered validation branch, to the tree driver's lifecycle, or to any file under `src/io/output/`.
- No `#ifdef HDF5` machinery beyond what compilation actually requires in this slice.
- No memory-projection machinery; two full raw slabs, unconditionally.

### Risk Flags
- Risky surfaces touched: the startup configuration path traversed by every run; the driver dispatch seam.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: the integration-test updates above; a slab-rotation unit case if placed package-locally.
- Commands to run:
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot`, then `./mimic` with a scratch snapshot-ordered run file over the fixture package, capturing output and asserting the new message sequence
  - `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh`
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no clean && make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no`
  - The bitwise tree-path check, exactly as specified in Slice 1's Validation Plan (worktree "before" build, scratch `test_binary.yaml` runs, silent `cmp` loop, cleanup), with evidence recorded.
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the `validate_and_postprocess()` diff and confirm no tree-ordered branch changed; run the fixture-package snapshot run once with `-v` and eyeball the slab load/release sequence.

### Rollback Path
- Revert the commit; the dispatch case returns to the Phase 4b FATAL and the config rejections disappear with it.

## Slice 5: Instanced galaxy pool and the explicit pool handle through inheritance

**Developer seat:** `--model sonnet --effort high` — Middle tier: instancing the galaxy pool and threading an explicit handle through inheritance — wide but mechanical, with a bitwise check as the backstop.

### Intended Change
- `src/core/galaxy_pool.{c,h}`: refactor the file-static singleton to an instanced handle API — `struct GalaxyPool`, with `galaxy_pool_create(initial_capacity)` / `galaxy_pool_alloc(pool)` / `galaxy_pool_reset(pool)` / `galaxy_pool_destroy(pool)` (names indicative; the discipline — chunked allocation, stable pointers, bulk reset, `NULL`-means-no-galaxy — carries over unchanged, per dual-driver Phase 5 item 7 and the Phase 3 learning).
- `src/core/inheritance.{c,h}`: `inherit_descendant_halos` gains a leading `struct GalaxyPool *pool` parameter, threaded to the two allocation sites (`inheritance.c:25`, `:101`). One new parameter, no behaviour change.
- Every production caller of the pool API updates to the handle: the tree-side init/reset/destroy lifecycle (wherever it lives among `src/core/build_model.c`, `src/core/tree_driver.c`, `src/core/main.c`), and the tree reader's `free_unit_halos()` (`src/io/tree/interface.c:183` calls `galaxy_pool_reset()`). The tree driver holds exactly one pool instance with the same initial capacity and the same reset points as the current singleton.
- **`free_unit_halos()` gains the explicit pool parameter**: `free_unit_halos(struct GalaxyPool *pool)` (`src/io/tree/interface.h:28`), resetting the given pool at the existing `interface.c:183` point; `NULL` is legal and means the caller allocated no galaxies, skipping the reset. This is the same explicit-ownership resolution the dual-driver plan chose for inheritance (its item 7) — a hidden reset-target reference inside the reader would recreate the scoped-active-pool shape that plan explicitly rejected. Callers: `src/core/tree_driver.c:243` passes the driver's instance; the topology-dump tool (`tests/unit/tools/dump_ctrees_topology.c:157`) and the two package-local loading tests (`simulations/millennium/_tests/unit/test_tree_loading.c`, `simulations/mini-millennium/_tests/unit/test_tree_loading.c`, four calls each) pass `NULL`, since none of them allocates galaxies; the tool's now-purposeless `galaxy_pool_init(0)` call (`:116`) is removed. No accessor and no new file-static state anywhere.
- Direct test callers update to the handle: `tests/unit/test_galaxy_pool.c`, `tests/unit/test_inheritance.c` (`:111` and its pool setup/teardown), `tests/unit/test_output_buffer.c` (`:93`, `:104`), plus the `free_unit_halos` callers above. The topology-dump tool is built by `make dump-ctrees-topology-tool` (`Makefile:829-834`).

### Acceptance Criteria
- Inputs: unchanged tree-driver processing.
- Outputs: identical galaxy output, byte for byte, on the tree path.
- [ ] `inherit_descendant_halos` takes the pool handle; no function in `inheritance.c` or `galaxy_pool.c` reaches pool state through a file-static any more (`grep` evidence recorded).
- [ ] The tree driver's single pool instance is created, reset, and destroyed at exactly the same points in the run lifecycle as the singleton was (call-site diff recorded in evidence).
- [ ] New unit tests prove pool-instance independence: allocations from two pools interleave without interference; resetting one pool leaves the other's pointers valid and its contents intact; destroy on one leaves the other functional; no leak diagnostic.
- [ ] A default-pair tree-ordered binary run's galaxy output is byte-for-byte identical before and after this slice, evidence recorded.
- [ ] Behaviour that must not change: the default-pair three-tier suite is green; the fixture-pair unit suite is green; allocation category accounting (`MEM_*`) is unchanged.

### Authorized Surface
- Files allowed to change:
  - `src/core/galaxy_pool.c`
  - `src/core/galaxy_pool.h`
  - `src/core/inheritance.c`
  - `src/core/inheritance.h`
  - `src/core/build_model.c`
  - `src/core/tree_driver.c`
  - `src/core/main.c`
  - `src/core/allvars.c`
  - `src/include/globals.h`
  - `src/io/tree/interface.c`
  - `tests/unit/test_galaxy_pool.c`
  - `tests/unit/test_inheritance.c`
  - `tests/unit/test_output_buffer.c`
  - `tests/unit/tools/dump_ctrees_topology.c`
  - `src/io/tree/interface.h`
  - `simulations/millennium/_tests/unit/test_tree_loading.c`
  - `simulations/mini-millennium/_tests/unit/test_tree_loading.c`
- Functions/classes/components allowed to change: the pool API and its internals; `inherit_descendant_halos`'s signature and its two allocation sites; the tree-side pool lifecycle call sites; `free_unit_halos()`'s signature and its reset call; the `free_unit_halos` call sites in the topology-dump tool and the two package-local loading tests (mechanical `NULL` updates only); in `src/core/allvars.c` / `src/include/globals.h`, only what hosting the tree driver's pool instance and updating the pool documentation comments requires — if nothing there needs to change, leave both untouched.
- Tests allowed or expected to change: the six test-side files named above (mechanical signature updates plus the new two-pool cases; no assertion weakened). New unit tests are glob-discovered and need no registration.

### Explicit Non-Goals
- No second pool instance anywhere (the ping-pong pair is Slice 9's).
- No change to inheritance semantics, workspace layout, type transitions, or any physics-adjacent behaviour.
- No scoped active-pool mechanism (explicitly rejected by the dual-driver plan).
- No change to chunk sizing, growth policy, or reset semantics.

### Risk Flags
- Risky surfaces touched: the shared inheritance seam both drivers will consume; memory ownership (vision principle 5).
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: the two-pool independence cases; signature updates in any direct-caller tests.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh` (after fixture-pair generate/build)
  - `make MODEL=sage16 SIMULATION=mini-millennium dump-ctrees-topology-tool` — the standalone tool must still compile and link with the instanced pool API
  - `MODEL=sage16 SIMULATION=millennium tests/unit/run_tests.sh test_tree_loading` after a `millennium`-pair generate — compiles the millennium package's updated loading test (a data-absent `TEST_SKIP` at runtime is acceptable; a compile or link failure is not)
  - The bitwise tree-path check, exactly as specified in Slice 1's Validation Plan, evidence recorded.
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: confirm with the memory-report debug output that pool allocations land in the same category as before.

### Rollback Path
- Revert the commit; the singleton returns.

## Slice 6: int64 output widths through the buffer, segments, and marshalling

**Developer seat:** `--model sonnet --effort high` — Middle tier: `int` → `int64_t` widening through buffer, segments, and marshalling. Broad, repetitive, and precisely enumerated.

### Intended Change
- `src/core/output_buffer.{c,h}`: `struct OutputBuffer.count`/`.capacity` become `int64_t`; `struct OutputBufferSegment.output_first`/`.output_count` become `int64_t`; `workspace_start`/`workspace_count` remain `int` (FoF-local, bounded by one FoF group, per dual-driver Phase 5 item 1's resolution). Growth arithmetic and the `MAX_HALO_ARRAY_SIZE` cap (`output_buffer.c:48-52`) are performed in `int64_t`.
- `src/core/build_model.c` / `src/core/tree_driver.c` / output writers: every consumer of the widened fields updates its types; where a widened value is narrowed back to `int` (e.g. syncing segment ranges into the tree driver's `int` `HaloAux.FirstHalo`/`NHalos`, or passing counts to the existing writer signatures), the narrowing is **explicit and checked** — an out-of-range value FATALs rather than truncates. `NumProcessedHalos`/`MaxProcessedHalos` (`src/core/allvars.c:46-49`, declared `src/include/globals.h:105-106`) widen with the buffer they mirror.
- The accumulated per-snapshot output total widens with them: `TotHalosPerSnap[]` becomes `int64_t` (`src/core/allvars.c:51`, `src/include/globals.h:110`). The guarded counter (`src/io/output/util.c:52-58`) **keeps its `INT_MAX` fatal guard with identical semantics** — that guard is the explicit, checked ceiling protecting the `int` consumers downstream, so its behaviour (and `tests/unit/test_output_counters.c`'s `INT_MAX` fatal expectations at `:75-93`) does not change. The two on-disk consumers keep today's bytes exactly: the binary header write stays a 4-byte `int` **permanently** (`src/io/output/binary.c:148` — binary is tree-only and byte-frozen), and the HDF5 attribute write stays `H5T_NATIVE_INT` **until Slice 8 widens the dtype**; both are behind the counter guard, so neither can truncate. `InputHalosPerSnap` (per-tree input counts) stays `int`, untouched.
- The tree reader's sizing site updates with the widened globals: `src/io/tree/interface.c:149-160` computes `MaxProcessedHalos = (int)(MAXHALOFAC * InputTreeNHalos[unit])` and derives `MaxFoFWorkspace` from it — the arithmetic and stores update to the widened types with any remaining narrowing explicit and checked, values unchanged for all reachable inputs.
- `src/core/inheritance.{c,h}`: counts stay `int` (FoF-local workspace counts), unchanged — recorded here so nobody "helpfully" widens them.
- No HDF5 attribute change in this slice: the on-disk `TotHalosPerSnap` **dtype** widening is Slice 8's (it is an output-schema change and carries the format-version bump); this slice widens only the runtime global behind it.

### Acceptance Criteria
- Inputs: unchanged tree-driver processing.
- Outputs: identical galaxy output, byte for byte, on the tree path; identical HDF5 metadata (no delta in this slice).
- [ ] `struct OutputBuffer` and the segment output ranges are `int64_t`; `workspace_start`/`workspace_count` and the `inherit_descendant_halos` counts are still `int`; header diff recorded.
- [ ] `TotHalosPerSnap[]`, `NumProcessedHalos`, and `MaxProcessedHalos` are `int64_t` in both definition and declaration; the counter's `INT_MAX` fatal guard is semantically unchanged and `test_output_counters.c`'s fatal cases still pass; the binary header still writes exactly 4 bytes per count; the HDF5 attribute is still written as `H5T_NATIVE_INT`; `InputHalosPerSnap` is untouched; the `src/io/tree/interface.c:149-160` sizing produces identical values for all reachable inputs.
- [ ] Every narrowing from the widened fields to `int` is through one checked helper (or an equivalent explicit check at each site) that FATALs on overflow; grep evidence lists each narrowing site and its check.
- [ ] Compiler warnings are clean under `-Wall -Wextra -Wshadow -Wformat-security -Wundef` for every touched file (implicit-conversion regressions surface here).
- [ ] A default-pair tree-ordered binary run's galaxy output is byte-for-byte identical before and after this slice, evidence recorded.
- [ ] A default-pair tree-ordered **HDF5** run's metadata is identical before and after this slice, excluding only the five provenance attributes (`git_commit`, `git_branch`, `git_date`, `build_date` on the version group; the wall-clock `RunEndTime` on the master's `/RunProperties`), which are legitimately build/run-variant provenance carrying no scientific content — no other attribute, dtype, or table change anywhere (the four permitted deltas belong to Slices 7 and 8, not here).
- [ ] Behaviour that must not change: the default-pair three-tier suite is green; the fixture-pair unit suite is green; `MAX_HALO_ARRAY_SIZE` still caps growth at the same numeric value.

### Authorized Surface
- Files allowed to change:
  - `src/core/output_buffer.c`
  - `src/core/output_buffer.h`
  - `src/core/build_model.c`
  - `src/core/tree_driver.c`
  - `src/core/allvars.c`
  - `src/include/globals.h`
  - `src/include/types.h`
  - `src/include/proto.h`
  - `src/io/output/binary.c`
  - `src/io/output/hdf5.c`
  - `src/io/output/util.c`
  - `src/io/output/util.h`
  - `src/io/tree/interface.c`
  - `tests/unit/` (existing tests that name the widened fields, including `test_output_counters.c`)
- Functions/classes/components allowed to change: the buffer/segment structs and every direct consumer of the widened fields; `marshal_workspace_to_output_buffer`'s types; the narrowing helper; in `src/io/tree/interface.c`, only the `MaxProcessedHalos`/`MaxFoFWorkspace` sizing and allocation lines (`:149-160`).
- Tests allowed or expected to change: existing unit tests that name the widened fields (type updates only; the `INT_MAX` fatal expectations in `test_output_counters.c` must still pass unchanged in meaning).

### Explicit Non-Goals
- No on-disk or HDF5-visible change of any kind (that is Slice 8).
- No widening of FoF-local workspace counts, `inherit_descendant_halos`, or `struct HaloAux`.
- No change to `MAX_HALO_ARRAY_SIZE`'s value.
- No behavioural change to marshalling: same Type-3 clearing, same snapshot stamping, same segment recording.

### Risk Flags
- Risky surfaces touched: the shared output-marshalling seam; integer-width changes with silent-truncation potential.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: type updates in existing buffer/marshal tests; one new case exercising the checked narrowing FATAL with an artificially large value if a test seam exists without production-code contortion (if not, record why).
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - The bitwise tree-path check, exactly as specified in Slice 1's Validation Plan.
  - HDF5 metadata identity check: run the default-pair generated `test_binary.yaml` copy switched to `output_format: hdf5` before and after the slice (same worktree pattern), then compare the two masters' and partitions' attribute/dataset structure with `h5dump -A` diffs — identical except the five excluded provenance attributes (`git_commit`, `git_branch`, `git_date`, `build_date`, `RunEndTime`).
  - `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh` (after fixture-pair generate/build)
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: audit every `printf`-family format touched for `%d` → `%` PRId64 correctness.

### Rollback Path
- Revert the commit.

## Slice 7: Configured identity multiplier through the encoder, and its output provenance

**Developer seat:** `--model opus --effort high` — Strongest implementer (plan profile): identity encoding and its output provenance — this slice can move galaxy IDs.

### Intended Change
- `src/include/galaxy_id.h`: every helper takes the multiplier as an explicit `int64_t` parameter instead of using `TREE_MUL_FAC` — `mimic_unique_galaxy_id_max_forests(multiplier)`, `mimic_unique_galaxy_id_total_forests_valid(multiplier, total_forests)`, `mimic_unique_galaxy_id_components_valid(multiplier, halonr, forestnr_global)`, `mimic_encode_unique_galaxy_id(multiplier, halonr, forestnr_global)`. The bound expression is unified onto the snapshot form: `max_forests = INT64_MAX / multiplier - 1` (equal to the old form at 10⁹ and 10¹⁰, one stricter exactly when the multiplier divides 2⁶³ — the safe direction, and the frozen format's stated startup contract). Component validation derives from the same helper: `halonr` in `[0, multiplier)`, `forestnr_global` in `[0, max_forests)`.
- `src/core/build_model.c`: `make_unique_galaxy_id` (`:268-282`) passes `MimicConfig.UniqueGalaxyIDMultiplier`; the diagnostic at `:278` prints the configured value, not the compile-time constant.
- Three tree-reader guard sites migrate from `TREE_MUL_FAC` to the configured multiplier (or to the common helper): `src/io/tree/read_ctrees_ascii.c:684`, `src/io/tree/read_ctrees_hdf5.c:387`, `:770` — so a tree-ordered run declaring 10¹⁰ genuinely accepts forests of 10⁹ halos rather than accepting the configuration and ignoring it.
- Every other caller of the `galaxy_id.h` helpers updates to the new signatures, passing `MimicConfig.UniqueGalaxyIDMultiplier`: `src/core/tree_driver.c:182-185` (`_total_forests_valid`/`_max_forests` in the L-Halo forest-count check) and the helper calls at `src/io/tree/read_ctrees_ascii.c:462-465` and `src/io/tree/read_ctrees_hdf5.c:1561-1587`. Before coding, the implementer greps for all four helper names and confirms the caller inventory is exactly these plus `make_unique_galaxy_id` and the unit test; a caller outside the authorized surface is a stop-and-report.
- `tests/unit/test_galaxy_id_encoding.c` (existing; exercises all four helpers with today's signatures) updates to the new signatures and **gains** the bound/equivalence cases below — no separate new test file.
- `src/io/snapshot/interface.c`: `snapshot_identity_bounds_valid()` delegates its bound arithmetic to the unified `galaxy_id.h` helper (one bound expression in the codebase, not two); its accept/reject behaviour is unchanged.
- `src/core/read_parameter_file.c:1446-1457`: the tree-ordered non-default-multiplier rejection **and its explanatory comment** are removed — the encoder now honours the configured value on both paths.
- `src/include/types.h:113-116`: the `UniqueGalaxyIDMultiplier` field comment currently states the encoder is hard-coded and that configuration rejects a non-default value for tree-ordered runs; both claims become false in this slice, so the comment is corrected in the same commit. No other stale statement of this claim exists in `src/` (verified by grep during planning).
- Output provenance (permitted HDF5 delta 1): an `int64` attribute named `UniqueGalaxyIDMultiplier` on the `/RunProperties` group, written to **both** the per-file metadata path (`write_perfile_metadata`, `src/io/output/metadata_hdf5.c:435-456`) and the master's configuration table (`store_run_properties`, `:477-524`, using the existing `CONFIG_PARAM_INT64` precedent at `:551-554`).
- `src/core/core_properties.yaml:79` (permitted HDF5 delta 3): the `UniqueGalaxyID` description's hard-coded `10^9` is replaced so it names the configured multiplier. Frozen wording: `Persistent run-scoped unique galaxy identifier across all snapshots (creation_halonr + multiplier * (forestnr_global + 1), where multiplier is simulation.unique_galaxy_id_multiplier, default 10^9, provenance attribute UniqueGalaxyIDMultiplier)`.

  **Amendment 2026-08-11 (human-approved).** The originally frozen wording ended `recorded in HDF5 output provenance as UniqueGalaxyIDMultiplier)` and was **260 characters**. `struct FieldMetadata`'s description column is `char description[256]` (`src/include/generated/hdf5_field_metadata.inc:24`, emitted by `scripts/generate_properties.py:1451`), and the table is a C brace-initialiser, so a 260-character literal both truncates and trips a compiler diagnostic under the project's `-Wall -Wextra`. Worse, the truncated text ended `…UniqueGalaxyIDMultipl`, which is **not** the attribute's name — so the description failed at the one thing delta 3 exists to do. The wording above is 243 characters, leaving 12 characters of headroom, and preserves every element of the original intent: the formula, the configuration key, the default, and the exact attribute name. **Any future edit to this string must keep it at 255 characters or fewer.**
- `docs/DEVELOPER-GUIDE.md`, `docs/USER-GUIDE.md`, `.agents/skills/mimic-config-and-flags/SKILL.md`, `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`: the four places that state "a non-default multiplier is accepted only for snapshot-ordered configurations" are corrected in the same commit that changes the behaviour, so documentation of record never contradicts the code.

### Acceptance Criteria
- Inputs: multiplier values from `simulation_info.yaml` / run files; the declared domain is positive `int64` values already enforced at parse time (`> 0`), with the header-bounds check owning dataset compatibility. Behaviour for a multiplier of 0 or below remains a config-time rejection, unchanged.
- Outputs: identical IDs and identical output bytes everywhere the default multiplier is in effect; a working non-default multiplier on both paths.
- [ ] No helper in `galaxy_id.h` names `TREE_MUL_FAC`; `grep -rn "TREE_MUL_FAC" src/` output is recorded and every remaining hit is the `constants.h` definition or the config-default seeding/reporting in `read_parameter_file.c`.
- [ ] Unit tests pin the unified bound: for multipliers 1, 2, 10⁹, and 10¹⁰, `max_forests == INT64_MAX / M - 1` exactly, and encoding at the extreme accepted components does not overflow (checked arithmetically in the test).
- [ ] Unit tests pin encoder equivalence: for the default multiplier, `mimic_encode_unique_galaxy_id(TREE_MUL_FAC-as-value, h, f)` equals the pre-slice formula `h + 10^9 × (f + 1)` across representative and boundary components.
- [ ] A tree-ordered configuration declaring `unique_galaxy_id_multiplier: 10000000000` now passes config validation, runs, and produces IDs encoded with 10¹⁰ (integration or unit evidence), and its forests are guarded against 10¹⁰, not 10⁹, at all three migrated reader sites (unit or targeted evidence per site).
- [ ] `snapshot_identity_bounds_valid()` behaviour is unchanged across this slice: the Phase 4b unit tests for it (sentinel, boundary divisions at 1/10⁹/`INT64_MAX`, rejections) all still pass without edit.
- [ ] Both HDF5 metadata paths carry the new `UniqueGalaxyIDMultiplier` int64 attribute with the configured value: read back from a per-file `/RunProperties` and from the master's `/RunProperties` after a default-pair HDF5 run and after a micro-uchuu-ascii run (values 10⁹ in both).
- [ ] A default-pair tree-ordered binary run's galaxy output is byte-for-byte identical before and after this slice (default multiplier ⇒ identical IDs), evidence recorded.
- [ ] A default-pair tree-ordered HDF5 metadata comparison before/after this slice shows **exactly two** deltas beyond the five always-excluded provenance attributes (`git_commit`, `git_branch`, `git_date`, `build_date`, `RunEndTime`): the new `UniqueGalaxyIDMultiplier` attribute (in both per-file and master metadata) and the changed `UniqueGalaxyID` description in `FieldMetadata` — and nothing else.
- [ ] The run-local `metadata/output_schema.json` changes in exactly two fields relative to before the slice: the `UniqueGalaxyID` description and `source_md5`.
- [ ] Behaviour that must not change: the default-pair three-tier suite is green; the fixture-pair unit suite is green; the physics baseline is unchanged.

### Authorized Surface
- Files allowed to change:
  - `src/include/galaxy_id.h`
  - `src/core/build_model.c`
  - `src/core/read_parameter_file.c`
  - `src/io/tree/read_ctrees_ascii.c`
  - `src/io/tree/read_ctrees_hdf5.c`
  - `src/io/snapshot/interface.c`
  - `src/io/output/metadata_hdf5.c`
  - `src/core/core_properties.yaml`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/USER-GUIDE.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `src/core/tree_driver.c`
  - `src/include/types.h`
  - `tests/unit/test_galaxy_id_encoding.c`
  - `tests/integration/test_processing_order.py`
  - `simulations/micro-uchuu-snapshot/_tests/unit/`
  - `tests/unit/test_ctrees_hdf5_reader.c` (amendment 2026-08-11, see below)
- Functions/classes/components allowed to change: the four `galaxy_id.h` helpers; `make_unique_galaxy_id` and its diagnostic; the three reader guard expressions and their messages; the enumerated helper call sites in `tree_driver.c` and both ctrees readers; `snapshot_identity_bounds_valid`'s internals (not its behaviour); the removal of the `:1446-1457` rejection block and comment; the `types.h:113-116` field comment only; `write_perfile_metadata` and `store_run_properties` additions; the one YAML description string; the four documentation statements named above. In `tests/unit/test_ctrees_hdf5_reader.c`, ONLY the multiplier seeding described below — no existing assertion may be weakened or removed.

**Authorized surface amendment, 2026-08-11 (human-approved during the Mode B run that stopped on this slice).** Migrating the three tree-reader forest-size guards to `MimicConfig.UniqueGalaxyIDMultiplier` (bullet 3 above) breaks 8 previously-green assertions in `tests/unit/test_ctrees_hdf5_reader.c`. That file drives the ctrees HDF5 reader through its `ctrees_hdf5_test_*` seams and never calls `read_parameter_file()` (verified: zero references), so the multiplier sits at its zero-initialised value and every migrated guard compares against 0, reporting `at or above the unique-galaxy-id limit of 0`. No fix existed inside the original surface: seeding a fallback inside reader code would reintroduce `TREE_MUL_FAC` into `src/io/tree/` and directly fail this slice's acceptance criterion 1, and the original surface contained no file (`allvars.c` is absent) able to give `MimicConfig` a non-zero default the test would observe. The file is added so its `main()` (or its per-test setup) can seed `MimicConfig.UniqueGalaxyIDMultiplier = TREE_MUL_FAC`, following the established precedents `install_output_chunking_defaults_for_test()` (`tests/unit/test_parameter_parsing.c:48`) and `install_overwrite_output_default_for_test()` (`tests/framework/core_test_fixtures.h`, added by Slice 4). This is a test-fixture seeding change only: no production behaviour changes, no acceptance criterion is relaxed, and criterion 1's `grep -rn "TREE_MUL_FAC" src/` evidence is unaffected because the seeding lives under `tests/`.
- Tests allowed or expected to change: `tests/unit/test_galaxy_id_encoding.c` (signature updates plus the new bound/equivalence cases; no assertion weakened); in `tests/integration/test_processing_order.py`, the tree-ordered non-default-multiplier rejection test (which inverts to an acceptance case) and the two multiplier precedence cases (`:309-408`) that relied on the rejection for observability — no other test in that file may change.

### Explicit Non-Goals
- No change to `TREE_MUL_FAC`'s definition or the config default (10⁹).
- No change to the parsed precedence semantics (seeded default, package value, run-file override) — encoder-side only.
- No `hdf5_format_version` bump (that is Slice 8's, tied to the dtype widening).
- No snapshot-driver code; the snapshot side still cannot emit output.

### Risk Flags
- Risky surfaces touched: the identity encoding every output record carries; HDF5 provenance written by every HDF5 run; user-visible metadata text.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: the bound/equivalence unit tests; the inverted multiplier-acceptance case; reader-guard cases per migrated site where the existing test harnesses reach them.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium check-generated && make MODEL=sage16 SIMULATION=mini-millennium && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - The bitwise tree-path check, exactly as specified in Slice 1's Validation Plan.
  - HDF5 metadata delta check: default-pair HDF5 runs before/after (worktree pattern), `h5dump -A` diff of one partition file and the master — exactly the two enumerated deltas beyond the five always-excluded provenance attributes.
  - Provenance read-back: `mimic_venv/bin/python -c "import h5py,sys; f=h5py.File(sys.argv[1]); print(f['/RunProperties'].attrs['UniqueGalaxyIDMultiplier'])" <file>` for one per-file output and the master.
  - `make MODEL=halos-only SIMULATION=micro-uchuu-ascii generate && make MODEL=halos-only SIMULATION=micro-uchuu-ascii && ./mimic models/halos-only/input/halos-only_micro-uchuu-ascii.yaml` then the same read-back.
  - `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh` (after fixture-pair generate/build)
  - `make MODEL=sage16 SIMULATION=mini-millennium check-docs`
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the `galaxy_id.h` diff against the dual-driver plan's item 3 bound analysis; confirm the description string matches the frozen wording exactly.

### Rollback Path
- Revert the commit and regenerate; the rejection block returns with it.

## Slice 8: Driver-neutral output partition seam and the snapshot output schema

**Developer seat:** `--model sonnet --effort high` — Middle tier, but the heaviest of them: the driver-neutral partition seam, the snapshot output schema, the `TotHalosPerSnap` widening, and the `hdf5_format_version` bump land together.

### Intended Change
- New `struct OutputPartitionSource` (in `src/io/output/util.h` or a small new header under `src/io/output/`): supplies partition count, partition output id, partition existence, and the resolved reader-format name. The tree side populates it from `MimicConfig.reader`'s hooks (including `prepare_run`/`teardown_run` pass-through); the snapshot side is a trivial single-partition implementation (count 1, output id 0, always exists, name `snapshot_hdf5`). Construction lives **outside** `src/io/output/` — a `get_output_partition_source()` beside the driver dispatch in `src/core/tree_driver.c` switches on the resolved reader kind — so that after this slice **no file under `src/io/output/` reads `MimicConfig.reader`** (`master_hdf5.c:46-55`, `:77-80`, `:151-152`; `metadata_hdf5.c:582`, which now takes the format name from the source, so a snapshot run's provenance records `snapshot_hdf5`).

   **Superseded by D5(a), 2026-08-13.** "The snapshot side is a trivial single-partition implementation (count 1, output id 0, always exists, name `snapshot_hdf5`)" describes the design this item specified before D5(a). What shipped instead: the snapshot partition source returns `MimicConfig.NOUT` partitions, one per requested output snapshot, with `partition_output_id(p)` returning `MimicConfig.ListOutputSnaps[p]` — the snapshot number, not a fixed 0 or a dense index — and `partition_snapshots(p)` returning that partition's single selection index; format name `snapshot_hdf5` is unaffected. See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.
- `src/io/output/hdf5.c` (`write_hdf5_attrs`): for snapshot-ordered runs, each `Snap%03d` group **omits `Ntrees` and the `TreeHalosPerSnap` dataset entirely** (absent, not zero/empty) and keeps `TotHalosPerSnap`; for all runs, the `TotHalosPerSnap` attribute **widens to `int64`** (permitted HDF5 delta 2), writing the Slice 6-widened `int64_t` global directly and removing Slice 6's temporary checked narrowing at this site (the binary header's permanent narrowing is untouched). Tree-ordered output keeps `Ntrees` and `TreeHalosPerSnap` unchanged.
- `src/io/output/master_hdf5.c`: partition enumeration, existence, and teardown go through the source; `TotHalosPerSnap` is read and republished as int64 (`:128-145`); for a snapshot run the master writes exactly one `Snap%03d` group per requested output snapshot, each containing a single external link to the one output file, with no per-tree table and no partition loop; the unconditional `TreeHalosPerSnap` link (`:112-114`) becomes tree-runs-only.

   **Superseded by D5(a), 2026-08-13.** "Each containing a single external link to the one output file" above describes the design this item specified before D5(a). What shipped instead: each requested snapshot's `Snap%03d` group still holds a single `File%03d` external link, but it resolves into that snapshot's **own** partition file, not into one shared output file — there are `MimicConfig.NOUT` partition files, not one. See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.
- `src/io/output/metadata_hdf5.c:110-115`: `hdf5_format_version` increments `"1.1"` → `"1.2"` (permitted HDF5 delta 4 — delta 2 is an output-schema change and the stated rule requires the bump), written to both per-file and master metadata.
- **The per-tree counter path becomes tree-only.** `output_increment_halo_counters_checked()` (`src/io/output/util.c:52-60`) unconditionally increments `InputHalosPerSnap[snap][tree]`, which only the tree reader allocates (`src/io/tree/interface.c:54`) — a snapshot run reaching the save path would dereference `NULL`. The counter helper (or the save path's use of it) is split so snapshot-ordered runs increment only the snapshot total and never touch `InputHalosPerSnap` or write `TreeHalosPerSnap` (`src/io/output/hdf5.c:240`); tree-run counting is unchanged.
- Format-version test expectations follow the bump: `tests/framework/data_loader.py:354` defaults `expected_format_version="1.1"`, and `tests/integration/test_output_formats.py` applies it both to fresh output and to the tracked 1.1 baselines. Every call site states the version it expects explicitly — fresh output asserts `"1.2"`, the tracked baselines assert `"1.1"` — and the committed baselines are **not** regenerated. Whether the framework default moves to `"1.2"` or is simply always passed explicitly is the implementer's; what is frozen is that no call site relies on a default that silently disagrees with what it reads.

   **Superseded by D8, 2026-08-14.** "The tracked baselines assert `\"1.1\"`... and the committed baselines are **not** regenerated" describes the expectation this item froze before D8. What shipped instead: D8 Slice 2 (`f81e2385`) regenerated both tracked HDF5 baselines via the parent plan's mandated copy-only procedure, so they now also carry `hdf5_format_version` `"1.2"`; `docs/dev/D8-FOLLOWUP-RECONCILIATION-PLAN.md` Slice 1 moved both `test_output_formats.py` call sites' expectation from `"1.1"` to `"1.2"` to match. Fresh output and the tracked baselines both assert `"1.2"` today. See `docs/dev/D8-SPIN-UNITS-RECONCILIATION-PLAN.md` and `docs/dev/D8-FOLLOWUP-RECONCILIATION-PLAN.md`.

   **Also superseded, 2026-08-14: `assert_hdf5_schema_layout` has no default at all.** This item and the "Test and build mechanics" paragraph above both record that `tests/framework/data_loader.py` *defaults* `expected_format_version` to `"1.1"`. The default was removed in `2385b480` — no call site had ever used it, and its docstring already declared there was no safe default, so the signature now requires the argument. The frozen intent above ("no call site relies on a default that silently disagrees with what it reads") is therefore enforced by the signature rather than by convention. Do not mine either passage for the current default; there is none.
- The snapshot-side branches land now but stay unreachable until Slice 9 (the driver still aborts before output); the tree side is exercised immediately.

### Acceptance Criteria
- Inputs: unchanged tree-driver processing; the snapshot branches are exercised by unit tests against the source abstraction plus Slice 9's end-to-end runs.
- Outputs: tree-path output unchanged except deltas 2 and 4; a seam the snapshot driver can emit through.
- [ ] `grep -rn "MimicConfig.reader" src/io/output/` returns nothing; evidence recorded.
- [ ] A default-pair tree-ordered binary run's galaxy output is byte-for-byte identical before and after this slice, evidence recorded.
- [ ] A default-pair tree-ordered HDF5 metadata comparison before/after this slice shows **exactly two** deltas beyond the five always-excluded provenance attributes (`git_commit`, `git_branch`, `git_date`, `build_date`, `RunEndTime`): `TotHalosPerSnap`'s dtype (`int` → `int64`, same values, in both per-file attrs and the master's republication) and `hdf5_format_version` (`1.1` → `1.2`, per-file and master) — and nothing else; `Ntrees` and `TreeHalosPerSnap` are byte-identically present for tree runs.
- [ ] A micro-uchuu-ascii tree run (five partitions) still produces a master enumerating all five partitions with correct external links and per-snapshot totals equal to the sum over partitions.
- [ ] Unit or harness-level tests pin the snapshot-source contract: count 1, output id 0, exists, format name `snapshot_hdf5`; and pin that the snapshot attr path writes no `Ntrees` and no `TreeHalosPerSnap` (testable at the writer level with a temporary file even before the driver emits).

   **Superseded by D5(a), 2026-08-13.** "Count 1, output id 0" above describes the design this criterion specified before D5(a). What shipped instead: the pinned contract is `MimicConfig.NOUT` partitions, one per requested output snapshot, `partition_output_id(p) == MimicConfig.ListOutputSnaps[p]`; the no-`Ntrees`/no-`TreeHalosPerSnap` attr-path pin is unaffected. See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.
- [ ] The snapshot-mode counting path never reads or writes `InputHalosPerSnap`, pinned by a unit case exercising the split counter with no tree-side allocation present (no NULL dereference, snapshot total still counted).
- [ ] The default-pair integration tier is green with fresh output asserting `hdf5_format_version == "1.2"` and the tracked baselines still asserting `"1.1"`; no committed baseline file changes (`git status` clean under `tests/data/`).

   **Superseded by D8, 2026-08-14.** "The tracked baselines still asserting `\"1.1\"`; no committed baseline file changes" describes this criterion's condition before D8. D8 Slice 2 regenerated the tracked HDF5 baselines (see the supersession note above); both `test_output_formats.py` call sites now assert `"1.2"`, matching the regenerated files. See `docs/dev/D8-SPIN-UNITS-RECONCILIATION-PLAN.md` and `docs/dev/D8-FOLLOWUP-RECONCILIATION-PLAN.md`.
- [ ] Behaviour that must not change: the default-pair three-tier suite is green; binary output is untouched in structure and bytes; tree-run partition naming (`<basename>_%03d.hdf5`, master `<basename>.hdf5`) is unchanged.

### Authorized Surface
- Files allowed to change:
  - `src/io/output/hdf5.c`
  - `src/io/output/master_hdf5.c`
  - `src/io/output/metadata_hdf5.c`
  - `src/io/output/util.h`
  - `src/io/output/util.c`
  - `src/core/tree_driver.c`
  - `src/core/snapshot_driver.c`
  - `src/include/proto.h`
  - `src/include/types.h`
  - `tests/unit/` (new writer/source tests)
  - `tests/integration/` (HDF5 attr and format-version expectations)
  - `tests/framework/data_loader.py`
- Functions/classes/components allowed to change: `write_hdf5_attrs`, `write_master_file` and its helpers, `write_perfile_metadata`/`store_run_properties`'s format-name source, the version string, the counter helper's tree-only split, the new source struct and its two constructors, the dispatch-side `get_output_partition_source()`. In `tests/framework/data_loader.py`, only the `expected_format_version` handling of `assert_hdf5_schema_layout`.
- Tests allowed or expected to change: new source/writer/counter unit tests; any test asserting `hdf5_format_version` or the old dtype, with every call site stating its expected version explicitly. That includes `tests/unit/test_master_hdf5_partitions.c`, which injects `MimicConfig.reader` directly (`:114`) and asserts `TotHalosPerSnap` as `H5T_NATIVE_INT` (`:161`, `:224-230`) — its injection mechanism moves to the partition source and its dtype expectations to int64.

### Explicit Non-Goals
- No change to galaxy record layout, field metadata content (beyond what Slice 7 already changed), or binary output.
- No snapshot-driver emission logic (Slice 9 wires the driver to this seam).
- No change to partition planning (`forests_per_file` / `target_file_size_mb`) on the tree side.
- No renaming of `TotHalosPerSnap` (kept deliberately, per the frozen schema decision).

### Risk Flags
- Risky surfaces touched: the HDF5 output writers used by every HDF5 run; the master-file contract existing consumers read.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: the source-contract and snapshot-attr-path unit tests; dtype/version expectation updates.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - The bitwise tree-path check, exactly as specified in Slice 1's Validation Plan.
  - HDF5 metadata delta check: default-pair HDF5 runs before/after (worktree pattern), `h5dump -A` diffs — exactly the two enumerated deltas beyond the five always-excluded provenance attributes.
  - `make MODEL=halos-only SIMULATION=micro-uchuu-ascii generate && make MODEL=halos-only SIMULATION=micro-uchuu-ascii && ./mimic models/halos-only/input/halos-only_micro-uchuu-ascii.yaml`, then verify the five-partition master structure and totals with `h5ls -r` and a short `h5py` sum check.
  - `make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no clean && make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no`
  - `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh` (after fixture-pair generate/build)
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the master-file diff for the snapshot single-partition branch against the frozen schema text in the dual-driver plan's item 5.

  **Superseded by D5(a), 2026-08-13.** "The snapshot single-partition branch" above describes the design this manual check was written against before D5(a). What shipped instead: the master-file diff to read is the multi-partition snapshot branch — `MimicConfig.NOUT` partition files, one per requested output snapshot. See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.

### Rollback Path
- Revert the commit; the version string and dtype revert with it.

## Slice 9: The snapshot driver end to end, the identity comparator, and the first cross-format comparison

**Developer seat:** `--model opus --effort high` — Strongest implementer (plan profile) and the hardest slice in the phase: the driver end to end, the committed comparator, and the first cross-format identity comparison. Escalate a relaunch to `--model fable` only if `opus` burns two attempts here.

### Intended Change
- `src/core/snapshot_driver.c` grows from the Slice 4 skeleton into the full driver (dual-driver Phase 5 items 1, 2, 4, 6):
  - **Loop:** snapshots in increasing time order. For snapshot N: load slab N (slab N−1 retained); walk FoF groups in slab order (`FirstHaloInFOFgroup`/`NextHaloInFOFgroup` chains, each group processed when its central is first met in slab order); per subhalo, gather progenitor galaxies via `FirstProgenitor` into slab N−1 and `NextProgenitor` within slab N−1, using a per-slab aux array (prev-slab halo index → processed range, `int64_t` fields) that mirrors the tree driver's `HaloAux` role; build `InheritanceDescendant` from the shared view-based populator over the slab-N view; call `inherit_descendant_halos` with the current generation's pool; run the shared physics engine with a snapshot-side context setup mirroring `setup_module_context`'s slab-based virial/time quantities; marshal through the shared output buffer; and, for snapshots in `ListOutputSnaps`, write per-snapshot HDF5 output through the Slice 8 seam (single partition, output id 0).

    **Superseded by D5(a), 2026-08-13.** "Single partition, output id 0" above describes the design this bullet specified before D5(a). What shipped instead: per-snapshot HDF5 output is written through the Slice 8 seam's delivered form — `MimicConfig.NOUT` partitions, one per requested output snapshot, output id equal to the snapshot number. See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.
  - **Parity checklist, replicated exactly (identity-gate load-bearing):** most-massive-progenitor selection pins an occupied `FirstProgenitor` (`lenoccmax = -1`) and otherwise replaces only on strict `Len >` in chain order, exactly as `find_most_massive_progenitor` (`src/core/build_model.c:183-206`); gather visits each progenitor chain entry in chain order, then that halo's processed range in order, taking `source_time` from the source galaxy's stored `SnapNum`, exactly as `gather_progenitor_galaxies` (`:355-370`); `CentralMvir` is stamped from the FoF-central catalog mass onto every workspace member before physics; new-object init sets `SnapNum = current − 1` and the `dT` sentinel (`Age[snap−1] − Age[snap]`, −1.0 at snap 0); `ctx->time_interval` and dynamic substep counts derive from the workspace's pre-marshal progenitor `SnapNum`; `UniqueCentralGalaxyID` propagates from the FoF Type 0 central to all members before physics; output `SnapNum` is stamped at marshal time by the shared marshal, not before.
  - **State rotation (item 2, exactly):** retain raw slab N−1 and its processed/aux generation while processing N; once every FoF at N has deep-copied its surviving progenitor galaxies, release processed generation N−1 **and** raw slab N−1; write snapshot-N output using raw slab N (the output-conversion virial recomputation needs the slab-N view live); carry raw slab N plus processed generation N into N+1; release the final slab only after final-snapshot output. Two pools, ping-pong bulk reset per snapshot; two full raw slabs, unconditionally, with no projection branch.
  - **Identity:** `UniqueGalaxyID` computed from `slab->halo_rank_in_forest[i]` and `slab->forest_index[i]` with `MimicConfig.UniqueGalaxyIDMultiplier` through the Slice 7 helpers; component validation through the same helpers; `struct Halo.HaloNr` holds the halo's `int` slab index and **never** `HaloRankInForest`.
  - **Incomplete-output cleanup, covering the master.** The snapshot driver owns its own registered-path mechanism (the equivalent of the tree driver's `current_output_paths` registry, `tree_driver.c:41-80`): it registers the partition file at creation **and the master-file path before the run can be considered complete**, and the failure-exit hook at `src/core/main.c:132` calls the snapshot driver's remove-incomplete function alongside the tree driver's. Because the master is written only after the driver returns (`main.c:432`), the registration is **not** disarmed when `run_snapshot_driver()` exits — for snapshot-ordered runs it is disarmed only after `write_master_file()` succeeds, so a failure at any point up to and including the master write removes every registered output file. Tree-run cleanup behaviour is untouched.

    **Superseded by D5(a), 2026-08-13 — failure semantics changed, read before relying on this bullet.** "A failure at any point up to and including the master write removes every registered output file" above describes the all-or-nothing cleanup this bullet specified before D5(a). What shipped instead: `snapshot_clear_partition_output_path()` disarms each partition's cleanup registration the moment that partition's file closes cleanly, exactly as the tree driver disarms its own on the success path — so a later failure removes only the in-flight partition and the (never-written) master. **Every already-closed partition file survives a later failure.** This is the design a Shin-Uchuu operator relies on: a late failure does not destroy already-finished snapshot output. See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" → "Per-partition cleanup" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.
  - **Build contract:** HDF5-dependent writer calls compile only under `#ifdef HDF5`, with a fail-fast stub keeping `make USE-HDF5=no` green (the stub is normally unreachable — a non-HDF5 build rejects the configuration long before the driver).
- New committed comparator, `scripts/compare_cross_format_identity.py`, implementing the frozen comparison algorithm: aggregate every numbered output partition per side (following `scripts/convert/crosscheck.py:172-224`'s pattern); per output snapshot, **assert no duplicate `UniqueGalaxyID` within either run** before anything else; assert ID-set equality, reporting the symmetric difference with counts; then for each shared ID compare every field's **raw bytes** (NaN payloads and signed zeros as bit patterns; `numpy.allclose`/`==` do not implement the gate); non-zero exit on any difference, with a bounded per-field difference report.
- First cross-format identity comparison, run as this slice's gate evidence: `halos-only` on the full micro-Uchuu dataset, fixed timesteps, tree-ordered (`models/halos-only/input/halos-only_micro-uchuu-ascii.yaml`) versus snapshot-ordered (a scratch run file identical except `simulation.name` and `output.output_directory`), compared with the committed comparator. A divergence is fixed inside this slice — that is the point of running it here.

### Acceptance Criteria
- Inputs: the committed fixture dataset (unit/integration level) and the full regenerated micro-Uchuu dataset (gate evidence; its absence fails this slice rather than skipping).
- Outputs: a complete snapshot-ordered run producing HDF5 output; a committed comparator; a passing first identity comparison.
- [ ] A snapshot-ordered `halos-only` run over the **fixture** dataset completes with exit 0, writes one partition file plus a master through the Slice 8 seam, and its per-snapshot `TotHalosPerSnap` totals equal the marshalled counts; no `Ntrees`, no `TreeHalosPerSnap`, `TreeType == "snapshot_hdf5"`, `UniqueGalaxyIDMultiplier` present in both per-file and master `/RunProperties`.

  **Superseded by D5(a), 2026-08-13.** "Writes one partition file plus a master" above describes the design this criterion specified before D5(a). What shipped instead: the fixture run writes one partition file **per requested output snapshot** plus a master; everything else in this criterion (per-snapshot `TotHalosPerSnap` totals, no `Ntrees`/`TreeHalosPerSnap`, `TreeType`, `UniqueGalaxyIDMultiplier`) is unaffected. See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.
- [ ] A snapshot-ordered `halos-only` run over the **full micro-Uchuu** dataset completes with exit 0 under the two-slab rotation, with no leak diagnostic on a debug-level run over the fixture (leak checks at full scale are not required).
- [ ] The comparator, run on two copies of the same output, passes; run on outputs with one perturbed field byte, one dropped ID, one duplicated ID (synthetic fixtures), it fails naming the snapshot, the ID or field, and the count — each of the three failure modes is demonstrated.
- [ ] **First identity comparison passes:** `halos-only`, fixed timesteps, micro-Uchuu tree-ordered vs snapshot-ordered — same `UniqueGalaxyID` set per output snapshot, every field bitwise identical per ID, across all five tree-side partitions aggregated. The comparator log is recorded in the slice evidence.
- [ ] Interrupting a snapshot run mid-write (e.g. SIGTERM during output, or a forced-failure test seam) leaves no partial output file behind — demonstrated once during per-snapshot output and once during the master write, with neither the partition file nor the master surviving; evidence from a scripted check.

  **Superseded by D5(a), 2026-08-13 — failure semantics changed, read before relying on this criterion.** "With neither the partition file nor the master surviving" above describes the all-or-nothing cleanup criterion this bullet specified before D5(a). What shipped instead: an interruption mid-write on an **in-flight** partition, or during the master write, still leaves no partial file behind for that in-flight file or the master — but any **already-closed** partition file from an earlier snapshot in the same run **does survive**, by design (`snapshot_clear_partition_output_path()` disarms each partition's cleanup registration once its file closes cleanly). See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" → "Per-partition cleanup" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.
- [ ] `make USE-HDF5=no` (default pair) builds and links clean.
- [ ] A default-pair tree-ordered binary run's galaxy output is byte-for-byte identical before and after this slice, and a default-pair HDF5 metadata comparison shows **no** delta in this slice beyond the five always-excluded provenance attributes.
- [ ] Behaviour that must not change: the default-pair three-tier suite is green; the fixture-pair unit suite is green; tree-ordered micro-uchuu-ascii output is byte-identical before/after this slice at the galaxy-record level.

### Authorized Surface
- Files allowed to change:
  - `src/core/snapshot_driver.c`
  - `src/core/main.c`
  - `src/include/proto.h`
  - `src/include/types.h`
  - `scripts/compare_cross_format_identity.py` (new file)
  - `simulations/micro-uchuu-snapshot/_tests/unit/`
  - `tests/integration/test_processing_order.py`
- Functions/classes/components allowed to change: everything in `snapshot_driver.c`; in `src/core/main.c`, only the failure-exit hook (adding the snapshot cleanup call beside `tree_driver_remove_incomplete_outputs()` at `:132`) and the post-master disarm call after `write_master_file()` at `:432`; the integration test's snapshot-run expectation (the Slice 4 cannot-yet-produce-output abort becomes a completing run over the fixture). New tests are glob- or package-discovered and need no registration.
- Tests allowed or expected to change: fixture-package unit tests for driver-internal pieces exposed for testing; `tests/integration/test_processing_order.py`.

### Explicit Non-Goals
- No change to shared seams (`inheritance.c`, `output_buffer.c`, `module_registry.c`, `galaxy_pool.c`, the writers, the generator) — the driver **consumes** them; a needed seam change means stop and report, not a local edit.
- No snapshot-global operation hooks, no MPI paths, no `--skip` support, no binary output, no memory-projection branch.
- No committed scientific-tier test yet (Slice 10) and no committed snapshot run files under `models/*/input/` (Slice 10).
- No tolerance of any kind in the comparator.

### Risk Flags
- Risky surfaces touched: the new driver owning physics execution order, identity encoding, and output emission — the highest-risk slice of the phase.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: driver-piece unit tests (FoF walk order, gather order, rotation lifecycle) where exposable without contorting the driver; comparator self-tests (the three synthetic failure modes); integration expectations.
- Commands to run:
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot`, then `./mimic` over a scratch fixture-package snapshot run file; inspect output structure with `h5ls -r` and the read-backs in the criteria
  - `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh`
  - The first identity comparison: build and run `halos-only`/`micro-uchuu-ascii` (committed run file) and `halos-only`/`micro-uchuu-snapshot` (scratch run file differing only in `simulation.name` and `output.output_directory`, mechanical diff recorded), then `mimic_venv/bin/python scripts/compare_cross_format_identity.py <tree output dir+basename> <snapshot output dir+basename>` — exit 0, log recorded. Use per-pair worktree builds (Slice 10's pattern) or sequential in-tree builds with full regenerate between; never mix selectors.
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - The bitwise tree-path check, exactly as specified in Slice 1's Validation Plan, plus the no-delta HDF5 metadata check from Slice 6's plan.
  - `make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no clean && make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no`
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the driver's gather and most-massive-progenitor code side by side with `build_model.c:183-206` and `:355-370`, and the snapshot-side context setup side by side with `setup_module_context` (`build_model.c:439` onward, including the `dT`/`time_interval` derivation), confirming semantic identity line by line; verify with `-v` logs that rotation releases happen at the specified points and that the cleanup registry is disarmed only after `write_master_file()` returns.

### Rollback Path
- Revert the commit; the Slice 4 skeleton behaviour (validate, load, abort before output) returns.

## Slice 10: The cross-format identity gate in the scientific tier

**Developer seat:** `--model opus --effort high` — Strongest implementer (plan profile): the gate harness spans four `MODEL`/`SIMULATION` pairs and the scientific tier's single-pair build assumption.

### Intended Change
- Two new committed run files: `models/halos-only/input/halos-only_micro-uchuu-snapshot.yaml` and `models/sage16/input/sage16_micro-uchuu-snapshot.yaml`, each **identical to its `_micro-uchuu-ascii` counterpart except `simulation.name` and `output.output_directory`** (exactly two changed *functional* keys, plus — optionally — the leading header comment block, which carries no functional weight and would otherwise force each file to misdescribe its own input format; the mechanical diff is asserted by the harness, not merely eyeballed). **Amendment 2026-08-12 (human-approved):** the rule was originally "exactly two changed lines", which forced both new files to retain their ascii counterpart's header comment and therefore to describe themselves as Consistent-Trees ASCII. The identity guarantee the rule exists to protect is that the two runs differ in nothing *functional*; that guarantee is unchanged.
- **Core scientific-tier compatibility with snapshot-ordered packages.** `tests/scientific/test_scientific.py` obtains its output through `core_input_file("test_binary.yaml")`, which for a snapshot-ordered package is a snapshot-ordered run with binary output — correctly refused by Slice 4's HDF5-only rejection — so the four core scientific tests fail and the tier cannot exit 0 for *any* snapshot-ordered package, independently of this plan. The test selects the HDF5 configuration (`core_input_file("test_hdf5.yaml")`, generated for every pair by `scripts/generate_test_inputs.py:205`) and reads it through `load_hdf5_halos()` (`tests/framework/data_loader.py:259`) when the selected package cannot produce binary output, exactly as Slice 4 did for the C unit tier via `test_cosmology_param_file()`. The checks themselves (NaN/Inf, zeros, physical ranges, unit consistency) are format-agnostic and are not otherwise altered; the default pair continues to use binary. **Amendment 2026-08-12 (human-approved):** added because criterion 1 below is otherwise unsatisfiable by any in-contract implementation, and because Shin-Uchuu production — the pathway item following this plan — is snapshot-ordered and would inherit a scientific tier that cannot run at all.
- `simulations/micro-uchuu-snapshot/_tests/scientific/test_cross_format_identity.py`: the frozen gate, registered by the existing scientific-tier discovery when `SIMULATION=micro-uchuu-snapshot` is selected. The harness, in order:
  1. **Loud preconditions:** both datasets must resolve (`simulations/micro-uchuu-ascii`'s tree data and `simulations/micro-uchuu-snapshot/snapshots`); a missing dataset **fails** the test with a named path — never a skip.
  2. **Run-file diff assertion:** for each model, the committed snapshot run file differs from the ascii run file in exactly the two authorized functional keys, plus at most the leading header comment block.
  3. **Isolated per-pair builds:** one git worktree per pair at the current `HEAD` — `{halos-only, sage16} × {micro-uchuu-ascii, micro-uchuu-snapshot}` — recreating the machine-local `snapshots` symlinks inside each worktree from the main tree's resolved targets, then `make MODEL=<m> SIMULATION=<s> generate && make MODEL=<m> SIMULATION=<s>` per worktree. The ambient tier build is never touched.
  4. **Eight runs:** for each model × ordering, the committed (fixed-substep) run file and a constructed dynamic variant (`TimestepScheme: dynamic` added; otherwise byte-identical, diff asserted), each into its own scratch output directory.
  5. **Preflight equality (gate-specific, exact, deliberately stricter than Slice 3's tolerant runtime check):** the two packages' a_list files are byte-equal; and per run pair, the recorded `Redshifts` datasets (the derived `ZZ` table, `metadata_hdf5.c:180-196`) are exactly equal element-wise, the derived scale-factor table reconstructed as `1/(1+z)` per entry is exactly equal (covering the frozen gate's derived `AA`/`ZZ` assertion), `BoxSize`, `PartMass`, the three cosmology values, and `UniqueGalaxyIDMultiplier` attributes are exactly equal, and the `FieldMetadata` field names/dtypes/order are identical.
  6. **Comparison:** `scripts/compare_cross_format_identity.py` per model × scheme, aggregating all tree-side partitions first (the tree side writes five; opening one file would silently compare a fifth of the run). `halos-only` runs and compares first (fixed then dynamic), then `sage16` — a `halos-only` divergence is a driver bug and must be reported before the sage16 cost is paid; a `halos-only` pass alone is not the gate.
  7. **Tree-path preservation evidence (Definition of Done support):** one tree-ordered micro-uchuu-ascii `halos-only` run from a worktree at the recorded pre-Phase-5 baseline commit versus the current HEAD worktree run — galaxy-record datasets byte-identical, and the HDF5 metadata diff exactly the four permitted deltas beyond the five always-excluded provenance attributes (`git_commit`, `git_branch`, `git_date`, `build_date`, `RunEndTime`), which are legitimately build/run-variant provenance carrying no scientific content. (The binary-vehicle bitwise checks per slice already cover the record stream; this exercises the four deltas end to end.)
- The harness cleans up its worktrees and scratch outputs on every exit path, and prints per-stage progress so a multi-hour run is observable.

### Acceptance Criteria
- Inputs: both micro-Uchuu datasets present; the current branch state with Slices 1–9 landed.
- Outputs: a committed, repeatable gate.
- [ ] `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific` runs the gate and exits 0 with all eight runs compared; the log (captured under `archive/test-logs/`) shows the two-tier order (halos-only before sage16) and per-comparison PASS lines including compared-record and compared-field counts.
- [ ] With either dataset symlink temporarily absent, the test **fails** naming the missing path (demonstrated once each way, then restored) — it does not skip.
- [ ] The run-file diff assertion fails if any third *functional* line differs (demonstrated with a scratch perturbed copy, not by committing one); a differing header comment alone does not fail it.
- [ ] The preflight assertions fail loudly on an artificial mismatch (demonstrated by perturbing a scratch copy of one side's a_list in a worktree, not the committed file).
- [ ] The identity gate passes: for both models and both timestep schemes, every output snapshot has equal `UniqueGalaxyID` sets and per-ID bitwise-equal fields.
- [ ] The tree-path preservation evidence shows galaxy records byte-identical to the pre-Phase-5 baseline and, beyond the five always-excluded provenance attributes, exactly the four permitted HDF5 metadata deltas (`UniqueGalaxyIDMultiplier` attribute; `TotHalosPerSnap` int64; the `UniqueGalaxyID` description; `hdf5_format_version` 1.2) — each of the four **observed**, none merely excused; and the run-local `output_schema.json` differs in exactly the description and `source_md5`.
- [ ] The default-pair suite is untouched: `make MODEL=sage16 SIMULATION=mini-millennium tests-scientific` neither registers nor runs the gate.
- [ ] The default pair's own scientific tier is unaffected by the `tests/scientific/test_scientific.py` change: `make MODEL=sage16 SIMULATION=mini-millennium tests-scientific` still exits 0 and still validates **binary** output, and the four core tests are demonstrated to still fail on an artificially corrupted output (the format switch must not have turned them into no-ops).
- [ ] Behaviour that must not change: no `src/` change of any kind in this slice; `git diff --stat` proves the slice touches only the two run files, the new test, `tests/scientific/test_scientific.py`, `scripts/compare_cross_format_identity.py`, and (if needed for pathing helpers) the fixture package's `_tests/` area. **Amendment 2026-08-12 (human-approved):** this enumeration previously omitted `scripts/compare_cross_format_identity.py`, which the Authorized Surface below explicitly grants — a direct internal contradiction — and did not yet cover the core scientific-tier fix added above.

### Authorized Surface
- Files allowed to change:
  - `models/halos-only/input/halos-only_micro-uchuu-snapshot.yaml` (new file)
  - `models/sage16/input/sage16_micro-uchuu-snapshot.yaml` (new file)
  - `simulations/micro-uchuu-snapshot/_tests/scientific/` (new directory)
  - `scripts/compare_cross_format_identity.py`
  - `tests/scientific/test_scientific.py` (**amendment 2026-08-12, human-approved**; output-format selection only)
- Functions/classes/components allowed to change: the new test and its helpers; comparator refinements strictly limited to reporting/CLI ergonomics discovered while wiring the harness (no comparison-semantics change); in `tests/scientific/test_scientific.py`, only the output-format selection and the corresponding loader call — the four checks' logic, thresholds and assertions are untouched.
- Tests allowed or expected to change: the new scientific test, and `tests/scientific/test_scientific.py`'s output-format selection only.

### Explicit Non-Goals
- No `src/` change. A gate failure is fixed by stopping and reporting (the defect belongs to an earlier slice's surface), not by editing driver code under this slice's contract.
- No tolerance, no field exclusions, no sampling: every ID, every field, raw bytes.
- No registration of `micro-uchuu-snapshot` in `scripts/discovery.py`'s gating lists, and no CI wiring (the datasets are machine-local; CI cannot run the gate).
- No changes to committed `_micro-uchuu-ascii` run files.

### Risk Flags
- Risky surfaces touched: none in `src/`; the risk is evidential (a mis-built harness passing vacuously), addressed by the negative demonstrations in the criteria.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: the gate test itself.
- Commands to run:
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific` (captured; exit code checked; multi-hour run delegated to a subagent that reports the tail and exit code)
  - The three negative demonstrations (missing dataset, perturbed run-file copy, perturbed preflight input), each restored afterwards with `git status` clean
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read one comparator PASS log end to end and confirm the compared-field count equals the output schema's field count; confirm worktree cleanup left `git worktree list` clean.

### Rollback Path
- Revert the commit; no production code is affected.

## Slice 11: Documentation of record, narrow vision update, and phase closeout

**Developer seat:** `--model sonnet --effort high` — Cheapest tier (plan profile): documentation of record, a narrow vision update, and phase closeout. Drop to `medium` if trimming cost.

### Intended Change
- `docs/DEVELOPER-GUIDE.md`: the snapshot driver (loop, rotation, two pools, parity behaviours at a summary level), the explicit input view, the instanced pool, the output-partition seam and snapshot output schema, the configured multiplier now honoured on both paths, and the identity gate's location and how to run it.
- `docs/USER-GUIDE.md`: running snapshot-ordered inputs end to end — HDF5-only output, no `--skip`, serial-only with the distributed-plan pointer, the multiplier's meaning, and reading snapshot-run output (no `Ntrees`/`TreeHalosPerSnap`; int64 `TotHalosPerSnap`; `hdf5_format_version` 1.2).
- Skills: `.agents/skills/mimic-architecture-contract/SKILL.md` (the reader/driver seam now has two live drivers; weak point W1 closes; the view, pool, and seam contracts); `.agents/skills/mimic-simulations-and-readers/SKILL.md` (snapshot package now runnable; gate location); `.agents/skills/mimic-config-and-flags/SKILL.md` and `references/all-config-keys.md` (snapshot-ordered rejection rules; anything Slice 7's edits left incomplete); `.agents/skills/mimic-run-and-operate/SKILL.md` (snapshot run mechanics); `.agents/skills/mimic-validation-and-qa/SKILL.md` (the scientific-tier gate and its dataset preconditions).
- `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: item 4 marked done with its gate evidence; item 5 (Shin-Uchuu production) marked next; the ephemeral-document dispositions executed.
- `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`: Phase 5 marked done with the gate result; any statement that now disagrees with the shipped repository is corrected (with the same discipline Phase 4b's Slice 5 used: cite the line, state what shipped).
- `docs/VISION.md`: the narrow post-gate update the dual-driver plan authorizes — per-driver memory bounds, determinism as an invariant, and a pointer to the implemented dual-driver architecture. Nothing else.
- `simulations/micro-uchuu-snapshot/README.md`: updated to state the package is now runnable end to end (snapshot driver), and where the identity gate lives and how to run it — stating explicitly that the gate is a **manual, dataset-present operation** (`make MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests-scientific` on a machine holding both micro-Uchuu datasets); no automated tier runs it once Phase 5 closes.
- Archival (project convention: never delete; completed plans move to gitignored `archive/dev-plans/`): `docs/dev/MIMIC-SNAPSHOT-READER-PLAN.md` (retained only until Phase 5 closes — that is now), `docs/dev/PHASE-4B-REVIEW-AND-PRE-PHASE-5-WORK.md`, and `docs/dev/PRE-PHASE-5-READINESS-REVIEW.md` (both marked "archive after Phase 5 planning"). Before archiving the reader plan, its four still-deferred entries (shared HDF5 utilities; empty-dataset non-sentinel metadata; reader strictness gaps; `discovery.py` gating membership) are copied into the dual-driver plan's follow-up notes (or the pathway) so archival drops no live item.

### Acceptance Criteria
- Inputs: the committed state after Slices 1–10, including the recorded gate evidence.
- Outputs: documentation of record consistent with the shipped code; a closed phase.
- [ ] `make check-docs` exits 0.
- [ ] Every driver behaviour, config rejection, output-schema statement, and command named in the new documentation exists in the repository as stated (spot-verified per claim; the verification list is recorded in evidence).
- [ ] The pathway's item 4 states the gate actually met (both models, both schemes, per-ID bitwise) and item 5 is marked next; the three archived documents no longer appear under `docs/dev/` and the four deferred entries survive in an active document.
- [ ] The dual-driver plan contains no statement contradicting the shipped repository (its Phase 5 section is marked executed with a pointer to this plan).
- [ ] The `docs/VISION.md` diff touches only the three authorized aspects.
- [ ] No prose is hard-wrapped; relative links and anchors all resolve.
- [ ] Behaviour that must not change: no code, YAML, Makefile, script, or test change of any kind; the full default-pair suite is green and unchanged; `git diff --stat` shows documentation, skills, and the archival moves only.

### Authorized Surface
- Files allowed to change:
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/USER-GUIDE.md`
  - `docs/VISION.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`
  - `docs/dev/MIMIC-SNAPSHOT-READER-PLAN.md` (removal by archival move only)
  - `docs/dev/PHASE-4B-REVIEW-AND-PRE-PHASE-5-WORK.md` (removal by archival move only)
  - `docs/dev/PRE-PHASE-5-READINESS-REVIEW.md` (removal by archival move only)
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `simulations/micro-uchuu-snapshot/README.md`
- Functions/classes/components allowed to change: documentation content only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals
- No code, build, script, or test change of any kind.
- No edit to `docs/dev/SNAPSHOT-HDF5-FORMAT.md` (Slice 2 made the only authorized edit; `format_version` stays 1).
- No vision rewrite beyond the three authorized aspects.
- No archival of `MIMIC-DUAL-DRIVER-PLAN.md` itself (it stays until the Shin-Uchuu step consumes it) and no edit to `SHIN-UCHUU-CONVERSION-PLAN.md`.
- `AGENTS.md`/`CLAUDE.md` are not authorized; if the documentation map needs a new row, report it.

### Risk Flags
- Risky surfaces touched: documentation of record and six skills, including status changes to the active plan hierarchy.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: none.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium check-docs`
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium tests-unit && make MODEL=sage16 SIMULATION=mini-millennium tests-integration && make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `git diff --stat` (documentation/skills/archival only)
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: follow every new or changed link by hand; run each edited skill's re-verification commands; confirm the archived files exist under `archive/dev-plans/` locally.

### Rollback Path
- Revert the commit and restore the archived files from it; code is unaffected.

---

## Standard Gate (binds every slice; selector discipline above applies)

With `<model>`/`<simulation>` the pair under test (default pair `sage16`/`mini-millennium` unless the slice says otherwise):

```bash
make MODEL=<model> SIMULATION=<simulation> generate
make MODEL=<model> SIMULATION=<simulation> check-generated
make MODEL=<model> SIMULATION=<simulation> validate-modules
make MODEL=<model> SIMULATION=<simulation> tests-unit
make MODEL=<model> SIMULATION=<simulation> tests-integration
make MODEL=<model> SIMULATION=<simulation> tests-scientific
```

Long-running test output is captured under `archive/test-logs/`, exit codes are checked explicitly, and any non-zero exit code is a failure regardless of log text.

---

## Definition of Done

- The explicit input view, the instanced galaxy pool with the explicit inheritance handle, and the `int64_t` output widths are in place, each proven neutral on the tree path by a bitwise binary comparison and by the direct unit tests its slice specifies.
- `snapshot_reader_open_run()` and `load_slab` have run-path callers; a snapshot-ordered configuration validates its dataset end to end at startup, including the physical-header agreement check with its rounding tolerance.
- `ForestIndex`/`HaloRankInForest` are reader-owned slab arrays, not catalog properties; the two micro-Uchuu packages generate identical `RawHalo` layouts; the format spec carries the dated Errata row and `format_version` is still 1.
- The `UniqueGalaxyID` encoder takes the configured multiplier on both paths (one bound expression, the snapshot form); the three tree-reader guard sites honour it; the tree-ordered non-default rejection is lifted; the multiplier is recorded as an int64 `UniqueGalaxyIDMultiplier` attribute on `/RunProperties` in both per-file and master HDF5 metadata.
- Snapshot-ordered runs are HDF5-only, reject `--skip` and `NTask > 1` at config time, write a single output partition with per-snapshot int64 `TotHalosPerSnap` and no per-tree structures, clean up incomplete output on failure, and build (with a fail-fast stub) under `USE-HDF5=no`.

   **Superseded by D5(a), 2026-08-13.** "A single output partition" and "clean up incomplete output on failure" (all-or-nothing) above describe the design this item specified before D5(a). What shipped instead: a snapshot-ordered run writes one HDF5 partition file per requested output snapshot (named by that snapshot's number) plus the master, each still with per-snapshot int64 `TotHalosPerSnap` and no per-tree structures; cleanup is per-partition, not all-or-nothing — a partition file that has closed successfully survives a later failure, and only the in-flight partition and the (never-written) master are removed. See `docs/DEVELOPER-GUIDE.md` → "The Snapshot Driver" and `docs/dev/SNAPSHOT-OUTPUT-PARTITIONING-PLAN.md` for the delivered design.
- No file under `src/io/output/` reads `MimicConfig.reader`; snapshot-run provenance records `snapshot_hdf5`.
- The cross-format identity gate passes on micro-Uchuu for `halos-only` **and** `sage16`, under fixed **and** dynamic timestep schemes: per output snapshot, equal `UniqueGalaxyID` sets and per-ID bitwise-equal fields over all aggregated partitions, with the preflight package-equality assertions green. The gate lives in the scientific tier (package-local to `micro-uchuu-snapshot`) and fails loudly when a dataset is absent.
- The tree-ordered path is byte-identical at the galaxy-record level to the pre-Phase-5 baseline throughout and at the end, with **exactly four** HDF5 metadata deltas beyond the five always-excluded provenance attributes (`git_commit`, `git_branch`, `git_date`, `build_date`, `RunEndTime`), all four observed by the preservation evidence: (1) the `UniqueGalaxyIDMultiplier` provenance attribute; (2) `TotHalosPerSnap` widened to int64; (3) the `UniqueGalaxyID` field description naming the configured multiplier; (4) `hdf5_format_version` 1.1 → 1.2. The run-local `metadata/output_schema.json` differs in exactly the description and `source_md5`. Binary output remains byte-identical as a whole file.
- Documentation of record, the vision's narrow post-gate update, and the skills describe what shipped; the reader plan and the two ephemeral pre-Phase-5 documents are archived with no live item dropped.

---

## Risks and Mitigations

| Risk | Mitigation |
|---|---|
| View plumbing subtly wrong while bitwise check passes | Slice 1's two-array unit tests target exactly that class (wrapper-reads-global, swapped views, output-conversion global independence) |
| Seam changes drift behaviour | Every seam slice (1, 5, 6) gates on the bitwise binary comparison plus the full tiers before any driver code exists |
| Driver parity misses an undocumented behaviour | The parity checklist is enumerated in Slice 9's contract verbatim from the dual-driver plan; the halos-only first-comparison catches driver-level divergence before sage16 physics is in play |
| Divergence discovered only at the full gate | Slice 9 runs the first identity comparison the moment output exists; Slice 10 only widens coverage (sage16, dynamic scheme) |
| Comparator masks defects | Duplicate-ID assertion before set comparison; raw-byte field comparison; the three demonstrated failure modes; partition aggregation before anything else |
| Vacuously passing harness | Negative demonstrations are acceptance criteria (missing dataset, perturbed run file, perturbed preflight) |
| Unattended implementer invents a decision | Every slice's contract states its frozen mechanics; anything genuinely unspecified is a stop-and-report, stated in the slice non-goals |
| HDF5 metadata creep | Per-slice metadata-delta checks (none in 1–6 and 9; exactly two each in 7 and 8; four cumulative at the gate) |

---

## Next Chat Prompt

Execute under `project-manager` (Mode B). Use the single authoritative Mode B launcher in `project-manager`'s `SKILL.md` ("Launcher"), with its first line filled in as:

```text
Plan file: docs/dev/MIMIC-SNAPSHOT-DRIVER-PLAN.md
```

Before starting, re-confirm and record: `simulations/micro-uchuu-snapshot/snapshots` resolves to 50 `snapshot_NNN.h5` files plus `forests.h5`; the micro-uchuu-ascii tree data resolves through its package; `mimic_venv` imports `h5py` and `numpy`; and the branch is `feature/ctrees-snapshot-reader` at or after `ae22d278` with a clean tree. Slices 9 and 10 cannot meet their criteria without both datasets — their absence is a run blocker, not a skip.
