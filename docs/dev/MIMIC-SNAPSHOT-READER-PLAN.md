# Mimic Snapshot Reader Implementation Plan (Dual-Driver Phase 4b)

**Status:** Frozen implementation plan for pathway item 3 (snapshot reader). Planned 2026-08-03 against `feature/ctrees-snapshot-converter` at `1c91f2b1`. Revision 5. Revision 3 followed three independent read-only review rounds by Codex `gpt-5.6-sol` (xhigh effort), each finding independently verified against the repository before adoption. Revision 4 (2026-08-03) records that both operator preconditions are discharged (see [Preconditions](#preconditions)); the only change to frozen slice content is that Slice 4's real-data test becomes a required check rather than an accepted SKIP, which strengthens a gate. No slice scope, authorized surface, or non-goal changed.

**Revision 5 (2026-08-04) — operator-authorized amendment made mid-run.** Slices 1–3 were executed and accepted under Revision 4 before this amendment; their frozen content is unchanged and their acceptance stands. Slice 4 stopped for a human because its own Validation Plan required a command that no in-contract implementation could satisfy. The operator authorized this amendment; it is deliberately the narrowest change that unblocks the slice.

1. **Slice 4's authorized surface gains `tests/unit/test_parameter_parsing.c`, restricted to `test_default_processing_order` and `test_explicit_tree_ordered_processing_order` and their shared fixture helper `write_processing_order_fixture()`.** Reason: core unit tests under `tests/unit/` are driven by the generated `build/generated/test_inputs/<model>/<sim>/core/test_binary.yaml`, whose simulation configuration is the **selected** package's `simulation_info.yaml`. Those two tests therefore assert the selected package's values while appearing to assert package-independent defaults — `test_default_processing_order` hard-asserts `INPUT_PROCESSING_ORDER_TREE`, and `test_explicit_tree_ordered_processing_order` forces `tree_ordered` over a package whose `tree_type` is `snapshot_hdf5`, which Slice 4's reader/order compatibility check correctly rejects. The assumption was invisible until a non-tree-ordered package existed (Slice 1). No other file gains authorization, and no acceptance criterion is weakened.
2. **Slice 4 gains an acceptance criterion:** the full fixture-pair unit suite must exit 0. This *strengthens* the gate — Revision 4 required the command in the Validation Plan but stated no criterion for its exit code.
3. **Slice 5's authorized surface gains `scripts/check_docs.py`, restricted to adding `".pm"` to the existing `SKIP_DIRS` set.** Reason: Slice 5's first acceptance criterion is that `make check-docs` exits 0, and it cannot, because the `project-manager` toolkit's `.pm/runs/<run-id>/review-*-prompt.md` artifacts embed the review skill's bundle whose relative links do not resolve from `.pm/`. All 32 failures are inside `.pm/`. `SKIP_DIRS` already skips `.orchestrator` and `.ai-orchestrator` for exactly this reason; the Discovery Record relies on that behaviour but names only those two directories. Same defect class as item 1 — a stated expectation no in-contract implementation could satisfy.
4. **Two referential corrections** carrying no scope change: the Build wiring paragraph's justification for the registry filename is restated as forward-looking (the unconditional `snapshot_reader_lookup()` call lands in Slice 4, not before it), and Slice 2's `tests/unit/run_tests.sh` line references are refreshed to the file's actual line numbers. Both were resolved on the record during Slices 2–3 and are corrected here so the document matches the repository.

**Recorded but deliberately NOT amended:** the Invariant 5 enumeration in Slice 2 names only "sentinel carried with halos present aborts" and "zero forest count with a non-empty dataset aborts", and never the third combination — a wholly empty dataset carrying non-sentinel metadata — which the shipped reader therefore accepts. Specifying it would add validation to an already-accepted slice, so it is carried as a Phase 5 follow-up rather than amended in retrospectively. See [Deferred and Out of Scope](#deferred-and-out-of-scope-recorded-so-nothing-is-silently-dropped).
**Contract inputs:** `docs/dev/SNAPSHOT-HDF5-FORMAT.md` (frozen on-disk contract, `format_version = 1`) and `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md` Phase 4b.
**Scope:** Add the `SnapshotReader` interface, registry, and `snapshot_hdf5` implementation, plus the fixture simulation package that declares the on-disk record. The snapshot **driver** (Phase 5), the cross-format identity gate, and Shin-Uchuu production are out of scope. The tree-ordered path must remain byte-identical throughout.
**Execution mode:** Mode B (`project-manager`). Five atomic slices, no batches.

---

## Context

Mimic v1.0 processes merger trees in one ordering: forest-ordered, depth-first, with per-forest bounded memory. `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md` adds a second front end — snapshot-ordered input feeding a snapshot-ordered driver — over the shared inheritance, physics-execution, and output-marshalling seams that v1.0 already extracted. The scientific payoff is snapshot-synchronous methods (true global SHAM, HOD, environment-dependent physics, a synchronous radiation field). The hard capacity motivation is Shin-Uchuu, whose percolation super-forest holds 33% of all tree roots in a single forest and therefore cannot be loaded as a unit by any per-forest memory model.

Pathway items 1 and 2 are complete. The format contract was frozen on 2026-07-18. The external converter was built and validated end to end on the real micro-Uchuu ASCII data on 2026-07-24: 22,580,924 halos across 50 snapshots and 440,651 forests, producer validation battery passing every invariant, and a seven-check cross-check against a `halos-only` reference run with zero unexplained mismatches. The topology-order gate is fully discharged — the Slice 10 reference-topology dump harness (`tests/unit/tools/dump_ctrees_topology.c`) let `crosscheck.py --reference-topology` compare `FirstProgenitor`/`NextProgenitor`/`NextHaloInFOFgroup` chain order, `ForestIndex`/`HaloRankInForest`, and the signed `MostBoundID` per halo, over an asserted-complete dump of every halo.

This plan is item 3. It is deliberately reader-only: the driver still fails fast, so nothing here can change a scientific result. Its job is to make the frozen format readable, validated, and unit-tested, so Phase 5 begins from a proven input path.

---

## Preconditions

1. **Fast-forward the working tree — DONE 2026-08-03.** The branch is at `1c91f2b1`; `docs/dev/SNAPSHOT-HDF5-FORMAT.md`, the Slice 10 harness, and `scripts/lib/hdf5.sh` are all present. Every path and line reference in this plan is verified against that commit.
2. **Converter Python dependency — SATISFIED 2026-08-03.** `pandas>=2.0.0` (`requirements.txt:6`) is installed in this machine's `mimic_venv` at 3.0.5, alongside numpy 2.4.6, and the converter's own suite (327 tests) passes against that stack. Slice 1's fixture generator can run. Should a later machine report `ModuleNotFoundError`, restore it with `mimic_venv/bin/pip install -r requirements.txt`.
3. **Full micro-Uchuu dataset regeneration — DONE 2026-08-03.** The dataset was regenerated from the intact 12 GB ASCII source and now lives at `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-snapshot/` (50 `snapshot_NNN.h5` files plus `forests.h5`, 2.3 GB), with the machine-local symlink `simulations/micro-uchuu-snapshot/snapshots` in place (`simulations/*/snapshots` is covered by `.gitignore:7`). The three totals the 2026-07-24 gate recorded are reproduced exactly — 22,580,924 halos, 50 snapshots, 440,651 forests — and the full gate was re-run rather than assumed: the producer validation battery passes all 15 checks, and the cross-check against a fresh `halos-only` reference run passes every check including the optional `topology-chains` chain-order proof over an asserted-complete dump of all 22,580,924 halos. `max_halo_rank_in_forest = 350074` and the recommended identity multiplier `1000000000` (equal to `TREE_MUL_FAC`, which Slice 4 makes the parsed default) are recorded here for the first time; no earlier values exist to compare against, so treat them as this run's measurements rather than as reproductions.
   Four consequences bind the run. First, **Slice 4's real-data test must actually run; a SKIP is a failure for this run** (see that slice's criterion). Second, `simulations/micro-uchuu-snapshot/` already exists as an untracked directory holding only that gitignored symlink, so Slice 1 populates it rather than creating it — no tracked file is present, `git status` is clean, and default-pair `check-generated` and `check-docs` both pass with it in place. Third, two skill re-verification commands glob `simulations/*/simulation_info.yaml` and will emit a "No such file" line for this directory until Slice 1 adds the file; the globs were made tolerant on 2026-08-03, but the package tables in `.agents/skills/mimic-simulations-and-readers/SKILL.md` still describe seven packages and are Slice 5's to update. Fourth, the `Unknown SIMULATION` guard at `Makefile:76-78` is a bare directory-existence test, so on this machine `make SIMULATION=micro-uchuu-snapshot <target>` now clears that guard and fails later with a less direct error until Slice 1 lands the package files.

---

## Selector Discipline (binds every slice)

Generated files are untracked, ignored build artefacts regenerated per `MODEL`/`SIMULATION` pair, and each embeds a `Source MD5` computed from that pair's YAML inputs (`scripts/check_generated.py:73-91`, `:207-243`). `make check-generated` compares that embedded hash against the *currently selected* pair's inputs, and does not itself regenerate. Consequently, a bare `make check-generated` run after generating for another selector fails deterministically.

Therefore, in every slice: **pass `MODEL=` and `SIMULATION=` explicitly on every `make` and test command, and immediately before any default-pair check, first run `make MODEL=sage16 SIMULATION=mini-millennium generate`.** The default pair is `sage16`/`mini-millennium`; the fixture pair is `halos-only`/`micro-uchuu-snapshot`. Never mix selectors within one check sequence.

---

## Discovery Record

Facts verified against the repository at `1c91f2b1` on 2026-08-03, and re-verified after independent review. The slices rely on them.

**No generator change is required.** `src/include/generated/read_tree_hdf5_properties.inc` is a pure macro list generated per simulation package from that package's `halo_properties.yaml`: `READ_TREE_PROPERTY(Field, "dataset_name", READ_AS_*, ctype)`, with a `_MULTIPLEDIM` variant for `vec3_*` fields. `src/io/tree/hdf5.c:185` includes it after defining its own macros (`hdf5.c:116-143`). The snapshot reader defines its own macros targeting `/halos/<dataset_name>` with `int64_t` counts and its own slab array, and includes the same generated file. Verified in the generator: `generate_read_tree_hdf5_properties_inc` iterates `catalog_by_name.values()` — the whole catalog, not only fields with `init_source: copy_from_tree*` (`scripts/generate_properties.py:946-961`) — and `_read_type_for_catalog` maps `long long` to `READ_AS_LLONG` (`:936-943`). `ForestIndex` and `HaloRankInForest` therefore appear automatically. Slice 1 makes this a mechanically checked acceptance criterion rather than an assumption.

`populate_halo_payload_from_snapshot.inc` is a **Phase 5** deliverable and is not in this plan. `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md` is self-contradictory here — `:135` assigns it to the snapshot driver while `:147` lists it under Phase 4b — and Slice 5 resolves that contradiction in favour of `:135`.

**Reader/driver seam.** `MimicConfig.reader` (`src/include/types.h:106`) is resolved by `tree_reader_lookup()` (`src/io/tree/registry.c:32-42`, case-insensitive) at a single site, `src/core/read_parameter_file.c:789`, which also copies `reader->file_extension` into `MimicConfig.TreeExtension` at `:797`. Config validation is `read_parameter_file.c:1350-1368`: `input.tree_name` is required **unconditionally** at `:1350-1352`, then an `else if` chain at `:1354-1368` whose reader/order compatibility check at `:1360` is currently **dead** for `snapshot_ordered`, because the blanket rejection at `:1357` short-circuits first. The block ends in `FATAL_ERROR("Parameter validation failed")` at `:1382-1384`. The driver fail-fast is `src/core/tree_driver.c:531-532`, reached from `main.c:413`. `input.processing_order` is already an accepted key in both the run file and `simulation_info.yaml` (`read_parameter_file.c:754-763`, parsed at `:323-334`, assigned at `:802-807`). The `simulation:` key whitelist is `read_parameter_file.c:877`.

Because `tree_name` is required unconditionally, the snapshot package **declares one** and the reader uses it to select its filename convention. It is **not** treated as a general pattern: configuration copies arbitrary user text into `MimicConfig.TreeName` (`src/core/read_parameter_file.c:781-785`), and the only existing formatter recognises a literal `%d`, not `%03d` (`src/io/tree/hdf5.c:213-241`), so passing configured text to `printf` would be both a format-string hazard and a silent-mismatch hazard. Instead the snapshot reader accepts exactly the literal string `snapshot_%03d.h5`, rejects every other value during configuration validation, and builds each filename with a fixed internal format string with truncation checking. `format_version = 1` fixes the filename convention, so nothing is lost by refusing alternatives. No change to `:1350-1352` is needed or authorized.

**`MimicConfig.reader` becomes legitimately `NULL` for snapshot configurations.** Every consumer was enumerated: `tree_driver.c:488`, `src/io/tree/interface.c:51,88,118,132`, `src/io/output/master_hdf5.c:46-55,76-80,151-152`, and `src/io/output/metadata_hdf5.c:582`. All are reached only *after* `run_processing_driver()`, which for a snapshot configuration calls `FATAL_ERROR` and `myexit(1)` (`tree_driver.c:531-532`). They are therefore provably unreachable in Phase 4b, and none is touched here. Both output writers must be made reader-kind-neutral by Phase 5's driver-neutral output seam before any snapshot run can produce output; Slice 5 records this in the dual-driver plan.

**`REQUIRE_READER_HOOK` is local to `src/core/tree_driver.c:123-129`,** and `open_partition`/`load_unit`/`close_partition` are dereferenced unguarded in `src/io/tree/interface.c:51,118,132`. The snapshot side puts its equivalent macro in its own `interface.c` and checks each hook at its point of use, closing that gap rather than copying it.

**`struct TreeReader` has 12 function-pointer hooks** (`src/io/tree/reader.h:68-111`), plus four data fields. (`docs/dev/MIMIC-DUAL-DRIVER-PLAN.md:78` says 14; Slice 5 corrects it.) They are partition/unit-shaped, which is why the snapshot side gets its own small vtable.

**Build wiring.** `Makefile:112` discovers sources recursively with `find`, so a new `src/io/snapshot/` directory compiles with no Makefile edit. `Makefile:272` excludes HDF5 sources from `USE-HDF5=no` builds by the filename pattern `%hdf5.c` — the reader implementation file **must** be named to end in `hdf5.c`; the registry and interface must **not**, because from Slice 4 onwards `read_parameter_file.c` calls `snapshot_reader_lookup()` in every build. (Before Slice 4 that call does not yet exist; the naming rule is independently required by the `%hdf5.c` filter-out combined with the unconditional `IO_SRCS` entry Slice 2 adds, so it binds from Slice 2.) `tests/unit/run_tests.sh` maintains three hand-edited lists: the unconditional `IO_SRCS` (`:158`) with an HDF5-only group appended at `:160`, a per-source `-DHDF5` `case` list (`:191-198`), and an HDF5-availability skip-name check (`:292-293`). It compiles all shared sources before any test-name skipping (`:186-205`), so a source omitted from the unconditional group produces an undefined symbol in non-HDF5 builds regardless of which tests are skipped. HDF5 detection is shared via `scripts/lib/hdf5.sh` and needs no change.

**Test gating.** `live_simulation_roots()` (`scripts/discovery.py:111-128`) returns only the *selected* package, and `generate_test_registry.py:57-65` registers `simulations/<selected>/_tests/unit/*.c`, so **package-local** tests compile and run only under `SIMULATION=micro-uchuu-snapshot`. Core tests under `tests/unit/` and `tests/integration/` are registered unconditionally (`generate_test_registry.py:35-48`, `:115-124`), so Slice 4's edit to `tests/integration/test_processing_order.py` is part of the default suite and must keep it green.

**Core roles are closed.** `src/core/core_properties.yaml:28-36` declares exactly eight mandatory `required_inputs` roles, each claimable by one field, with hard errors for unknown roles and for unbound roles (`scripts/generate_properties.py:477-521`). There is no optional-role mechanism, so `ForestIndex` and `HaloRankInForest` ship as ordinary catalog entries with no `provides_core_role`. How the snapshot *driver* reaches them is Phase 5's decision.

**Snapshot count comes from config.** The a_list is loaded by `read_snap_list()` into `MimicConfig.Snaplistlen` / `MimicConfig.MAXSNAPS` (`src/include/types.h:86-87`), and the data directory from `MimicConfig.SimulationDir` (`types.h:46`). Unit tests do not call `init()`, so they set these fields directly — the established pattern in `tests/unit/test_tree_reader_counts.c:21-32`, which `memset`s `MimicConfig` and pokes sentinel values into globals.

**Fixture storage arithmetic — this drives the fixture design.** The converter writes chunked, uncompressed datasets with `CHUNK_1D = (65536,)` and `CHUNK_VEC = (65536, 3)` (`scripts/convert/hdf5_writer.py:59-60`, `:215-218`), and `scripts/convert/validate.py:205-209` asserts those exact chunk shapes. HDF5 allocates a chunk in full as soon as any element in it is written, so a snapshot file containing even one halo allocates 6,553,600 bytes: seven int32 scalars and three float32 scalars at 262,144 B each, three int64 scalars at 524,288 B each, and three float32 vectors at 786,432 B each. Empty snapshots allocate no chunks. A committed fixture in production layout would therefore cost ~6.25 MiB per populated snapshot against a ~32.5 MB tracked repository — unacceptable. The frozen spec resolves this: the `(65536,)` chunking is stated as "required for production data", and "consumers must not depend on chunk boundaries, only on dataset shape and type" (`docs/dev/SNAPSHOT-HDF5-FORMAT.md:139-141`). Slice 1 therefore validates real production-layout converter output in a scratch workdir and commits a re-chunked copy, which is both small and a stronger reader test.

**The `UniqueGalaxyID` encoder is hard-coded to `TREE_MUL_FAC`.** `src/include/galaxy_id.h` (not `src/util/`) defines `mimic_unique_galaxy_id_max_forests`, `mimic_unique_galaxy_id_total_forests_valid`, `mimic_unique_galaxy_id_components_valid`, and `mimic_encode_unique_galaxy_id`, and every one of them uses the compile-time `TREE_MUL_FAC` directly; none takes a configured multiplier. Two consequences bind this plan. First, a configured non-default multiplier would be parsed and stored but silently ignored by the encoder, which is a silent wrong value and therefore forbidden — Slice 4 rejects a non-default multiplier for tree-ordered configurations until Phase 5 replaces the encoder. Second, `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md:84`'s claim that "the existing `galaxy_id.h` bounds validation enforces it at startup" is **false** as written, and Slice 5 corrects it.

**The producer compares `scale_factor` to the snapshot list exactly.** `scripts/convert/validate.py:347` uses `float(header["scale_factor"]) != float(a_list[snap])` with no tolerance, so a reader applying a tolerance would accept files the producer rejects. The reader therefore compares exactly.

**`check_memory_leaks()` returns `void` and only logs** (`src/util/memory.c:486-521`), so calling it proves nothing on its own. Tests must capture the run output and fail on the leak diagnostic.

**Baseline tolerances are not byte identity.** `tests/framework/harness.py:71-90` uses `rtol=1e-6`/`atol=1e-10` with an environment override, so passing the baseline suite is not proof of the byte-identical tree-path claim. Slice 4 adds an explicit bitwise comparison.

**Fixture precedent.** Every package except the Millennium pair ships a git-tracked fixture under `simulations/<name>/_tests/data/`, ranging from 644 B (`simulations/micro-uchuu/_tests/data/Uchuu100_test_lhalo_binary.0`) to 13,296 B (`simulations/micro-uchuu-hdf5/_tests/data/MicroUchuu_test_mergertree_info.h5`), produced by a committed generator under `_tests/input/`.

**Docs tooling.** `scripts/check_docs.py:26-33` skips `.orchestrator` and `.ai-orchestrator`, so reviewer evidence directories do not break `make check-docs`.

**Discovered Phase 5 risk, recorded not actioned.** `src/core/virial.c:45-51,106` (`get_virial_mass` and its siblings) call `mimic_tree_get_*` internally and are therefore tree-index coupled today. Phase 5's payload populator will need either snapshot-flavoured virial helpers or a `virial.c` refactor taking explicit arguments. This is the largest hidden coupling found while planning; it does not affect this plan.

---

## Design Decisions (frozen for this plan)

1. **Separate vtable, separate registry.** `struct SnapshotReader` is a new, small vtable in `src/io/snapshot/reader.h`, not a widening of `struct TreeReader`, whose 12 hooks are partition/unit-shaped and whose `REQUIRE_READER_HOOK` fail-fast checks two disjoint hook sets would defeat. `enum InputProcessingOrder` and `input_processing_order_name()` are reused from `src/io/tree/reader.h`.
2. **Two-registry lookup, not reordered parsing.** `read_parameter_file.c:789` tries `tree_reader_lookup()` and then `snapshot_reader_lookup()`. Reader names are disjoint, and Slice 4 asserts that disjointness. This does not reorder existing tree-path parsing. Exactly one of `MimicConfig.reader` and `MimicConfig.snapshot_reader` is non-`NULL` after a successful resolution.
3. **One "not implemented" point.** After Slice 4, a valid snapshot-ordered configuration parses and validates cleanly and fails only at `run_processing_driver()`.
4. **Reader-owned slabs with an explicit lifecycle.** `load_slab` allocates through `mymalloc_cat(..., MEM_TREES)`; `release_slab` frees. A slab handle has a defined empty state, must be empty before `load_slab` fills it, and must be released before `close_run`.
5. **`int64_t` throughout** for slab indices, counts, and offset arithmetic. `int` state is a tree-driver idiom that does not carry over; peak production slabs are 315M halos.
6. **Invariant 5 is validated in full at `open_run`, and the format spec is not edited.** This covers the per-row `SnapNum` check and the measured-data checks on `max_halo_rank_in_forest` and `n_forests_total`. All three are performed as **fixed-size hyperslab scans**: the reader reads each column in bounded blocks and accumulates a running maximum or range, and must not allocate any buffer proportional to `n_halos`. The aggregate read across micro-Uchuu is 22,580,924 rows × (4 + 8 + 8) bytes ≈ 452 MB, once. Revision 1 proposed moving the per-row check to slab load and recording it as an Errata. Independent review rejected that on two grounds, both accepted: the spec's Status paragraph reserves version bumps for changes to "validation rules", while its Errata rule covers only wording that misdescribed semantics version 1 always denoted (`docs/dev/SNAPSHOT-HDF5-FORMAT.md:5`, `:166-172`); and load-time-only checking means a malformed snapshot a run never loads is never detected. The cost basis was also wrong — the true Shin-Uchuu figure is ~60–72 GB across the dataset (15–18 billion halos × 4 B), not the ~4.4 GB previously stated. At micro-Uchuu scale the `SnapNum` component alone is ~90 MB and all three scans total ~452 MB, once, which is negligible. If the production figure later proves prohibitive, that is a deliberate `format_version = 2` decision with measurements behind it, not a Phase 4b convenience.
7. **No refactor of the tree readers.** The reusable-quality HDF5 read primitives (`ct_read_attribute`, the rank-1 extent helper, the field-handle cache) are `static` inside `src/io/tree/read_ctrees_hdf5.c`. Lifting them into a shared utility would touch a battle-tested production reader whose byte-identical output is this phase's gate. The snapshot reader carries its own small equivalents; the shared-utility refactor is recorded as follow-up.
8. **Identity multiplier lands here, and is enforced here.** `simulation.unique_galaxy_id_multiplier` (dual-driver decision D9) is plumbed in Slice 4, because the frozen spec requires a snapshot package to declare it (`docs/dev/SNAPSHOT-HDF5-FORMAT.md:126-130`, `:145-151`) and `parse_simulation_section` hard-FATALs on unknown `simulation.*` keys. The spec requires both identity bounds to be checked at startup, so `open_run` performs the check rather than deferring it. Changing the `UniqueGalaxyID` *encoding* remains Phase 5.
9. **Chain construction is producer-owned.** The spec's Ordering Contracts are converter obligations already discharged and proven by the topology gate. The reader reads those links and validates their index ranges; it never reconstructs or reorders them.
10. **The reader validates structure before reading data.** Object set, attribute set and scalar dtypes, dataset set, dtypes, ranks, and shapes are checked before any bulk read, so a non-conforming file is rejected rather than read into a buffer sized from different assumptions.

---

## Implementation Profiles

- Mode B (`project-manager`) executes all five slices atomically in plan order. Batches are not used and are not defined.
- Slices 2, 3, 4, and 5 carry `Independent audit required: yes` — a new architectural seam, the data path the Phase 5 identity gate will depend on, the startup-validation path every run traverses, and documentation of record including corrections to an active plan.
- Slice 1 is suitable for a cheaper implementer model. Slices 2–4 deserve a strong one.

---

## Slice 1: Snapshot fixture simulation package and committed contract fixtures

### Intended Change
- Create `simulations/micro-uchuu-snapshot/`, declaring the snapshot-HDF5 on-disk record for the micro-Uchuu catalog.
- `halo_properties.yaml`: every `/halos` dataset in the frozen spec's Halo Datasets table, in the ctrees-bridge style of `simulations/micro-uchuu-ascii/halo_properties.yaml` — copy that file's key order, `units`, `h_convention`, `range`, `output`, `init_source`, `output_source`, and `provides_core_role` bindings verbatim for every shared field — plus two new catalog-only entries, `ForestIndex` and `HaloRankInForest`, each `type: long long` with `units: dimensionless` and a `description`, and with **no** `provides_core_role`, `init_source`, or `output` key, matching how the catalog-only link fields are declared at `simulations/micro-uchuu-ascii/halo_properties.yaml:30-58`. No `source:` keys are needed: the spec's dataset names are normative and equal the property names.
- `simulation_info.yaml`: `input.tree_type: snapshot_hdf5`, `input.processing_order: snapshot_ordered`, `input.tree_name: snapshot_%03d.h5`, `input.simulation_dir: simulations/micro-uchuu-snapshot/snapshots`, `input.snapshot_list_file`, `input.first_file: 0`, `input.last_file: 0`, box 100 Mpc/h, `particle_mass` 0.0325 `1e10 Msun/h`, Uchuu cosmology (Ωm 0.3089, ΩΛ 0.6911, h 0.6774) — matching `simulations/micro-uchuu-ascii/simulation_info.yaml`. `tree_name` is declared because `read_parameter_file.c:1350-1352` requires it unconditionally; it must be exactly the literal `snapshot_%03d.h5`, which Slice 4 enforces.
- `micro-uchuu.a_list`: byte-identical copy of `simulations/micro-uchuu-ascii/micro-uchuu.a_list` (50 lines), following the `micro-uchuu-hdf5` naming precedent.
- `README.md`: data provenance, the converter commands that regenerate the full dataset, the `snapshots/` symlink instruction, the fixture-regeneration command, an explanation of why the committed fixture is re-chunked, and a pointer to `docs/dev/SNAPSHOT-HDF5-FORMAT.md`.
- `_tests/input/create_snapshot_fixture.py`: a committed generator that, in a scratch temporary workdir, (a) synthesises a tiny ctrees ASCII tree together with the `forests.list`, a_list, and `simulation_info.yaml` that the converter pipeline requires, (b) runs the full `scripts/convert/` pipeline over it in production layout, (c) runs `scripts/convert/validate.py` against that output and aborts unless it exits 0, (d) rewrites the validated datasets into `_tests/data/` with small chunk shapes, preserving the object set, dataset names, dtypes, shapes, and every header attribute value exactly, and (e) **re-opens the committed copy and asserts that every dataset's element values, every header attribute value, and the `/ForestID` sidecar values are identical to the validated production-layout file**, aborting on any difference. Chunk shape is the only permitted difference between the two. The fixture's header cosmology, box size, and particle mass are taken from the package's own `simulation_info.yaml` so the fixture is self-consistent with the package that ships it. The generator writes a canonical `fixture_manifest.json` recording only stable, path-independent fields; converter manifests are unsuitable to commit verbatim because they record absolute paths and source `mtime_ns` (`scripts/convert/scatter.py:532-558`, `:604-615`; `scripts/convert/hdf5_writer.py:283-286`, `:448-449`).
- `_tests/data/`: the re-chunked `snapshot_NNN.h5` files, `forests.h5`, the tiny a_list, and `fixture_manifest.json`.
- `_tests/input/check_fixture_conformance.py`: a committed structural checker asserting everything `scripts/convert/validate.py` asserts about the committed fixture **except** chunk shape — exact object set, exact header attribute names/dtypes/values, exact `/halos` dataset set with contract dtypes and ranks/shapes, absence of compression, and the `/ForestID` sidecar shape. Chunk shape is deliberately excluded, per `docs/dev/SNAPSHOT-HDF5-FORMAT.md:139-141`.

### Acceptance Criteria
- Inputs: the frozen spec; `simulations/micro-uchuu-ascii/` as the style and cosmology reference; `scripts/convert/` as the fixture producer. Requires precondition 2 (`pandas` installed).
- Outputs: a registered simulation package and a committed, self-validating fixture dataset.
- [ ] `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate` succeeds and `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot check-generated` exits 0.
- [ ] Generated `src/include/generated/raw_halo_defs.h` contains every `/halos` dataset named in the spec with the spec's types, including `ForestIndex` and `HaloRankInForest` as `long long`.
- [ ] Generated `src/include/generated/read_tree_hdf5_properties.inc` contains a `READ_TREE_PROPERTY` entry for every `/halos` dataset, with `READ_AS_LLONG` for `MostBoundID`, `ForestIndex`, and `HaloRankInForest`, and `READ_TREE_PROPERTY_MULTIPLEDIM` for `Pos`, `Vel`, and `Spin`.
- [ ] All eight core `required_inputs` roles are bound exactly once; generation completes with no warnings.
- [ ] The generator's intermediate production-layout output passes `scripts/convert/validate.py` with exit code 0, and the generator aborts if it does not. The generator's log records that this check ran.
- [ ] The generator's value-equality assertion passes: every dataset element, every header attribute, and every `/ForestID` value in the committed copy is identical to the validated production-layout file, with chunk shape the only permitted difference. A negative test — perturbing one element of one dataset in the copy — makes the generator abort.
- [ ] `_tests/input/check_fixture_conformance.py` exits 0 against the committed `_tests/data/`, and exits non-zero when run against a fixture with a deliberately wrong dataset dtype, a missing header attribute, or an extra `/halos` dataset.
- [ ] The fixture set contains at least four snapshots, of which at least one has zero halos and at least one is populated; at least one descendant with three or more progenitors; at least one FoF group with two or more members; and at least one halo with a negative `MostBoundID`.
- [ ] Total committed size under `simulations/micro-uchuu-snapshot/_tests/data/` is under 200 KB.
- [ ] Re-running `create_snapshot_fixture.py` regenerates byte-identical `.h5` files and a byte-identical `fixture_manifest.json`.
- [ ] Behaviour that must not change: after `make MODEL=sage16 SIMULATION=mini-millennium generate`, the default-pair `check-generated` and build succeed and the default-pair unit, integration, and scientific tiers are green. No existing simulation package is modified.

### Authorized Surface
- Files allowed to change:
  - `simulations/micro-uchuu-snapshot/`
- Functions/classes/components allowed to change: all new, within that directory.
- Tests allowed or expected to change: new fixtures, the fixture generator, and the conformance checker under `simulations/micro-uchuu-snapshot/_tests/`.

### Explicit Non-Goals
- No C code. No `src/`, `Makefile`, `scripts/`, or repo-tier `tests/` changes.
- No `simulation.unique_galaxy_id_multiplier` key — it does not parse until Slice 4 and would FATAL any run that read this package.
- No `snapshots/` symlink (machine-local, gitignored, operator-created), no `plot_profile.yaml`, no `_tests/integration/`.
- No edits to `scripts/discovery.py`'s `FULL_MODEL_TEST_SIMULATIONS` or `PRODUCTION_TEST_CONFIG_SIMULATIONS` — the package cannot run a model until Phase 5.
- No change to `docs/dev/SNAPSHOT-HDF5-FORMAT.md` or to `scripts/convert/`. If the spec, the converter, and the generator disagree, stop and report; do not deviate locally.

### Risk Flags
- Risky surfaces touched: property YAML, which drives generated code for this selector. Isolated to a new package; no existing package and no default-pair generated file changes.
- Approval needed before implementation: no

### Validation Plan
- Tests to add/update: the fixture generator, the committed fixtures, and the conformance checker with its three negative cases.
- Commands to run:
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate`
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot check-generated`
  - `mimic_venv/bin/python simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py` — regenerates the fixtures and runs the producer battery plus the value-equality assertion inline
  - `mimic_venv/bin/python simulations/micro-uchuu-snapshot/_tests/input/check_fixture_conformance.py`
  - reproducibility, using the gitignored `output/` scratch area:
    `rm -rf output/fixture-check && cp -R simulations/micro-uchuu-snapshot/_tests/data output/fixture-check`
    `mimic_venv/bin/python simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py`
    `diff -r output/fixture-check simulations/micro-uchuu-snapshot/_tests/data` — must print nothing and exit 0
  - negative cases: `check_fixture_conformance.py` takes the directory to check as its single positional argument, and `create_snapshot_fixture.py` takes an optional `--compare-against <dir>` that runs only the value-equality assertion. For each defect, mutate a copy under `output/fixture-negative/` and confirm a non-zero exit naming that defect: a wrong dataset dtype, a missing header attribute, and an extra `/halos` dataset through `check_fixture_conformance.py output/fixture-negative`; and one perturbed dataset element through `create_snapshot_fixture.py --compare-against output/fixture-negative`
  - `du -sk simulations/micro-uchuu-snapshot/_tests/data` — must report under 200 KB
  - `rm -rf output/fixture-check output/fixture-negative` to leave the tree clean
  - `make MODEL=sage16 SIMULATION=mini-millennium generate`
  - `make MODEL=sage16 SIMULATION=mini-millennium check-generated`
  - `make MODEL=sage16 SIMULATION=mini-millennium`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-unit`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-integration`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - (capture every tier's log under `archive/test-logs/` and check each exit code explicitly)
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: `h5ls -rv` one populated and one empty committed fixture file against the spec's dataset and attribute tables; diff `halo_properties.yaml` against `simulations/micro-uchuu-ascii/halo_properties.yaml` and confirm every difference is spec-mandated.

### Rollback Path
- Revert the commit; the directory is self-contained and nothing else references it.

## Slice 2: SnapshotReader interface, registry, and run-level open/close

### Intended Change
- `src/io/snapshot/reader.h`: `struct SnapshotReader` — `name`, `processing_order`, and the hooks `open_run`, `close_run`, `snapshot_halo_count`, `load_slab`, `release_slab`; the value types `struct SnapshotRunInfo` (snapshot count, `format_version`, `n_forests_total`, `max_halo_rank_in_forest`) and `struct SnapshotSlab` (`snapnum`, `nhalos` as `int64_t`, `struct RawHalo *halos`) with a defined empty state and an initializer; and `snapshot_reader_lookup(const char *name)`. Reuse `enum InputProcessingOrder` and `input_processing_order_name()` from `src/io/tree/reader.h`.
- `src/io/snapshot/registry.c`: static table mirroring `src/io/tree/registry.c` — case-insensitive lookup, `#ifdef HDF5` guarding both the `extern` and the table row so a non-HDF5 build compiles an empty table and the lookup returns `NULL`. **This file must not be named `*hdf5.c`**, because `read_parameter_file.c` calls `snapshot_reader_lookup()` in every build (`Makefile:272`).
- `src/io/snapshot/interface.c`: the `REQUIRE_SNAPSHOT_READER_HOOK` macro plus thin dispatchers, each verifying at its point of use that the hooks it needs are non-`NULL` before calling them. Point-of-use checking is deliberate: it matches the tree driver's idiom and lets Slice 2 land a reader whose Slice 3 hooks are not yet implemented. Also must not be named `*hdf5.c`.
- `src/io/snapshot/read_snapshot_hdf5.c`: the `snapshot_hdf5` reader's run lifecycle and count table. For each of `MimicConfig.Snaplistlen` snapshots, `open_run` builds the filename `snapshot_NNN.h5` under `MimicConfig.SimulationDir` using a **fixed internal format string with truncation checking** — configured text is never passed as a `printf` format argument — opens the file, validates it, and publishes a `SnapshotRunInfo` plus a per-snapshot halo-count table served by `snapshot_halo_count`. Slice 4 adds the configuration-time check that `MimicConfig.TreeName` is exactly the literal `snapshot_%03d.h5`; this slice does not read `TreeName` at all. `close_run` releases everything. Wrapped in `#ifdef HDF5` and named to end in `hdf5.c`.
- Validation performed at `open_run`, aborting with the file path, the object or attribute name, and the offending value:
  - **Structure, before any bulk read:** exactly the `/header` and `/halos` groups; exactly the specified header attribute set, each scalar and of the contract dtype; exactly the specified `/halos` dataset set, each of the contract dtype, with rank 1 for scalars and shape `[n_halos, 3]` for `Pos`, `Vel`, and `Spin`.
  - **Header values:** `format_version` is supported (1); `links_adjacent == 1`; `snapshot_number` equals the filename index; `0 <= n_halos <= INT32_MAX`; `n_halos` equals the length of every `/halos` dataset in that file; `n_forests_total` and `max_halo_rank_in_forest` are identical across all files and are either both non-negative or exactly the empty-dataset sentinel `(0, -1)` that the converter emits when a dataset contains no halos (`scripts/convert/links.py:468`); every configured snapshot has a file.
  - **Agreement with the snapshot list:** each file's `scale_factor` equals the a_list entry for its snapshot **exactly**, matching the producer's own comparison at `scripts/convert/validate.py:347`. The other physical header fields (`box_size_mpc_h`, `particle_mass_msun_h`, and the three cosmology values) are **not** compared against configuration in this phase: the spec assigns the reader `format_version`, `links_adjacent`, header consistency, and link ranges, and none of those values is consumed by anything the reader does here. Comparing them would also require a units conversion the plan would have to specify — the header stores native Msun/h while `MimicConfig.PartMass` is in 1e10 Msun/h (`src/core/read_parameter_file.c:932-936`). Deferred to Phase 5, where `get_virial_mass` actually consumes `PartMass`.
  - **Invariant 5 in full:** every `SnapNum` value equals the file's `snapshot_number`; `max_halo_rank_in_forest` equals the measured maximum of `HaloRankInForest` over the dataset; and every `ForestIndex` lies in `[0, n_forests_total)`, with the measured maximum equal to `n_forests_total - 1` whenever the dataset contains at least one halo. A wholly empty dataset carries the sentinel `(n_forests_total, max_halo_rank_in_forest) == (0, -1)`, must contain no halos in any snapshot, and skips the measured-maximum equality checks; any other combination of a zero forest count with a non-empty dataset aborts. Each check is a fixed-size hyperslab scan accumulating a running maximum or range, with no buffer proportional to `n_halos` (design decision 6). Note the scope deliberately: this verifies the header *bounds* against measured data, which is what invariant 5 asks of a header value. Full **density** of `ForestIndex` over `[0, n_forests_total)` and of `HaloRankInForest` within each forest (invariant 4) remains a producer obligation, discharged by the converter's identity battery and not re-derived here.
- `tests/unit/run_tests.sh`: add `src/io/snapshot/interface.c` and `src/io/snapshot/registry.c` to the **unconditional** `IO_SRCS` list at `:158`; add `src/io/snapshot/read_snapshot_hdf5.c` to the HDF5-only group at `:160`; add `snapshot/registry.c` and `snapshot/read_snapshot_hdf5.c` to the per-source `-DHDF5` `case` list at `:196-205`; add the new test name to the HDF5-availability skip check at `:300-302`.
- `scripts/lib/hdf5.sh`: add a single test-only override at the top of `detect_hdf5` — when `MIMIC_TEST_DISABLE_HDF5=1` is set, leave `HDF5_AVAILABLE=0` with empty flags and return immediately. Without it the non-HDF5 link path is untestable on any machine that has HDF5 installed, because `run_tests.sh:144` calls `detect_hdf5` unconditionally and the helper consults nothing else (`scripts/lib/hdf5.sh:27`). Note that `USE-HDF5` is a **Make** variable and cannot be used as a shell-command prefix: `USE-HDF5=no cmd` is not a valid shell assignment (the hyphen is not permitted in a variable name) and bash reports "command not found". The new environment variable follows the existing `MIMIC_TEST_BUILD` / `MIMIC_BASELINE_RTOL` convention.
- `simulations/micro-uchuu-snapshot/_tests/unit/test_unit_snapshot_reader_open.c`: unit tests against the Slice 1 fixtures, setting `MimicConfig` fields directly per the `tests/unit/test_tree_reader_counts.c:21-32` pattern.

### Acceptance Criteria
- Inputs: the Slice 1 fixture dataset, addressed through a `MimicConfig` the test populates directly.
- Outputs: a registered `snapshot_hdf5` reader that opens a dataset, validates it fully, and publishes run metadata and per-snapshot counts.
- [ ] `snapshot_reader_lookup("snapshot_hdf5")` returns the reader in an HDF5 build and `NULL` in a `USE-HDF5=no` build; lookup is case-insensitive and returns `NULL` for an unknown name and for a `NULL` argument.
- [ ] `open_run` on the fixture dataset publishes the correct snapshot count, `format_version`, `n_forests_total`, and `max_halo_rank_in_forest`, and `snapshot_halo_count` returns the correct count for every snapshot, including `0` for the empty one.
- [ ] `snapshot_halo_count` aborts for any index outside `[0, snapshot_count)`, tested at `-1` and at `snapshot_count`.
- [ ] Each of these corrupt-fixture cases aborts with a message naming the file and the offending object, attribute, or value, and none is silently repaired: an unsupported `format_version`; `links_adjacent != 1`; a `snapshot_number` disagreeing with the filename; an `n_halos` disagreeing with a dataset length; a negative `n_halos`; an `n_forests_total` differing between two files; a `max_halo_rank_in_forest` differing between two files; a missing `snapshot_NNN.h5` for a configured snapshot; a missing `/halos` dataset; an extra `/halos` dataset; a `/halos` dataset of the wrong dtype; a vector dataset of shape `[n_halos, 4]`; a missing header attribute; a header attribute of the wrong dtype; a `scale_factor` differing from the a_list entry by any amount; a `SnapNum` value disagreeing with the header.
- [ ] Two further corrupt cases exercise the measured-identity scan, each with mutually consistent headers that disagree with the data: a `max_halo_rank_in_forest` larger and smaller than the true maximum of `HaloRankInForest`, and an `n_forests_total` disagreeing with the measured `ForestIndex` range. Both abort.
- [ ] Structural validation runs before any bulk dataset read, so a wrong-shape dataset is rejected rather than read.
- [ ] Calling a dispatcher whose required hook is `NULL` aborts with a message naming that hook, proven with a deliberately incomplete test-local `SnapshotReader`, not with the real reader.
- [ ] `open_run` followed by `close_run` over the fixture dataset produces no leak: the captured unit-run output contains no `Memory leak detected` diagnostic (`src/util/memory.c:486-521`).
- [ ] `make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no clean && make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no` builds clean with `read_snapshot_hdf5.c` excluded.
- [ ] `MIMIC_TEST_DISABLE_HDF5=1 MODEL=sage16 SIMULATION=mini-millennium tests/unit/run_tests.sh test_tree_reader_counts` compiles the shared objects with `HDF5_AVAILABLE=0` and links without an undefined `snapshot_reader_lookup`, proving the registry is reachable in a non-HDF5 build.
- [ ] With `MIMIC_TEST_DISABLE_HDF5` unset, HDF5 detection behaves exactly as before this slice.
- [ ] Behaviour that must not change: no file under `src/io/tree/`, `src/core/`, or any existing simulation package is modified; the default-pair build and all three test tiers are green; `input.processing_order: snapshot_ordered` still fails fast at config validation exactly as before this slice.

### Authorized Surface
- Files allowed to change:
  - `src/io/snapshot/`
  - `tests/unit/run_tests.sh`
  - `scripts/lib/hdf5.sh`
  - `simulations/micro-uchuu-snapshot/_tests/unit/`
- Functions/classes/components allowed to change: all new under `src/io/snapshot/`. In `tests/unit/run_tests.sh`, only the `IO_SRCS` lists, the per-source `-DHDF5` `case` list, and the HDF5 skip-name check. In `scripts/lib/hdf5.sh`, only the `MIMIC_TEST_DISABLE_HDF5` early-return guard at the top of `detect_hdf5`; the probe sequence and its acceptance conditions must not change.
- Tests allowed or expected to change: new C unit tests under the fixture package.

### Explicit Non-Goals
- No slab loading (Slice 3), no config wiring (Slice 4), no documentation (Slice 5).
- No modification of `struct TreeReader`, `src/io/tree/registry.c`, or any tree reader.
- No lifting of `read_ctrees_hdf5.c`'s static HDF5 helpers into shared code — carry small local equivalents (design decision 7).
- No new `input.*` or `simulation.*` YAML keys, and no change to `read_parameter_file.c`.
- No reconstruction, reordering, or re-derivation of any chain order (design decision 9).
- No dependence on chunk shape or chunk boundaries anywhere in the reader.

### Risk Flags
- Risky surfaces touched: a new architectural seam (a second reader vtable and registry) and the hand-maintained unit-test build script shared by every C unit test.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: `test_unit_snapshot_reader_open.c`, covering registry lookup, a successful open against the fixtures, the out-of-range count contract, every corrupt-input abort listed above, the missing-hook abort via a test-local reader, and the leak check.
- Commands to run:
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot`
  - `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh test_unit_snapshot_reader_open`, with output captured, checking both the exit code and the absence of `Memory leak detected`
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium check-generated && make MODEL=sage16 SIMULATION=mini-millennium validate-modules`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-unit`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-integration`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no clean && make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no`
  - `MIMIC_TEST_DISABLE_HDF5=1 MODEL=sage16 SIMULATION=mini-millennium tests/unit/run_tests.sh test_tree_reader_counts` — proves the shared objects link without an undefined `snapshot_reader_lookup` in a non-HDF5 build
  - `MODEL=sage16 SIMULATION=mini-millennium tests/unit/run_tests.sh test_tree_reader_counts` — same test with detection unchanged, confirming the override is inert when unset
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: confirm `git diff --stat` shows zero changes under `src/io/tree/` and `src/core/`; confirm the new sources compile warning-free under `-Wall -Wextra -Wshadow -Wformat-security -Wundef`.

### Rollback Path
- Revert the commit. `src/io/snapshot/` is entirely new, and the `run_tests.sh` edits are additive list entries.

## Slice 3: Slab loading and link validation

### Intended Change
- `read_snapshot_hdf5.c` gains `load_slab` and `release_slab`.
- `load_slab(snapnum, slab)` requires the destination slab to be in its empty state and aborts otherwise; allocates `struct RawHalo[n_halos]` via `mymalloc_cat(..., MEM_TREES)`; and fills every field by including `src/include/generated/read_tree_hdf5_properties.inc` under snapshot-flavoured `READ_TREE_PROPERTY` and `READ_TREE_PROPERTY_MULTIPLEDIM` macros that read `/halos/<dataset_name>` with `int64_t` counts into the slab array. This is the mechanism `src/io/tree/hdf5.c:116-185` already uses; no generator change.
- `release_slab(slab)` frees the array and returns the handle to its empty state. Releasing a slab already in the empty state is a no-op. `close_run` aborts if any slab is still loaded.
- Validation performed at load: `FirstProgenitor` is `-1` or a valid index into snapshot `N-1`; `NextProgenitor` and `NextHaloInFOFgroup` are `-1` or valid indices into snapshot `N`; `FirstHaloInFOFgroup` is a valid index into snapshot `N` and never `-1`; `Descendant` is `-1` or a valid index into snapshot `N+1`, and is `-1` for every halo in the final snapshot. Loading snapshot `0` treats the non-existent snapshot `-1` as having zero halos, so any non-`-1` `FirstProgenitor` there is out of range and aborts.
- Diagnostics are bounded counted summaries — one line per snapshot and field, carrying the count and the first offending halo index and value — never one line per halo.
- Full chain-topology re-validation (cycle-freedom, FoF self-reference, progenitor round-trip closure) is **not** repeated at read: the spec assigns it to the producer, and the converter's battery and topology gate discharged it.

### Acceptance Criteria
- Inputs: the Slice 1 fixture dataset; a snapshot index in `[0, snapshot count)`. An out-of-range snapshot index aborts; it is not unspecified.
- Outputs: a populated, validated, reader-owned `RawHalo` slab.
- [ ] Loading each fixture snapshot yields an `nhalos` matching the header and field values matching the fixture bit-for-bit for every dataset, including the three `vec3` fields and both `long long` identity fields.
- [ ] Loading the empty snapshot succeeds, yields `nhalos == 0`, and releases cleanly.
- [ ] Each of these corrupt-fixture cases aborts, naming the field, the halo index, and the offending value: a `FirstProgenitor` outside `[-1, n_halos(N-1))`; a `NextProgenitor` outside `[-1, n_halos(N))`; a `FirstHaloInFOFgroup` outside `[0, n_halos(N))`; a `FirstHaloInFOFgroup` of `-1`; a `NextHaloInFOFgroup` outside `[-1, n_halos(N))`; a `Descendant` outside `[-1, n_halos(N+1))`; a non-`-1` `Descendant` in the final snapshot; a non-`-1` `FirstProgenitor` in snapshot `0`.
- [ ] A systematic failure produces bounded diagnostics — one counted summary per snapshot and field — not one line per halo.
- [ ] Lifecycle: `load_slab` into a non-empty slab aborts; `release_slab` on an empty slab is a no-op; a second `release_slab` is safe; `close_run` with a slab still loaded aborts.
- [ ] `load_slab` then `release_slab` for every fixture snapshot produces no `Memory leak detected` diagnostic in the captured unit-run output.
- [ ] All slab indices, counts, and offsets are `int64_t` on the load path; no `int` truncation.
- [ ] Behaviour that must not change: Slice 2's `open_run`/`close_run`/`snapshot_halo_count` semantics and all its validation; no tree-path file is modified; the default-pair suite is green.

### Authorized Surface
- Files allowed to change:
  - `src/io/snapshot/`
  - `simulations/micro-uchuu-snapshot/_tests/unit/`
- Functions/classes/components allowed to change: the `read_snapshot_hdf5.c` load path, the `SnapshotSlab` contract in `reader.h`, and the snapshot `interface.c` dispatchers.
- Tests allowed or expected to change: the fixture package's C unit tests.

### Explicit Non-Goals
- No re-validation of chain topology beyond index ranges (design decision 9).
- No config wiring, no driver, no payload populator, no `UniqueGalaxyID` work.
- No generator change, and no edit to any file under a `generated/` directory.
- No caching, prefetching, read windowing, or other performance work.
- No weakening of any Slice 2 validation.

### Risk Flags
- Risky surfaces touched: the data path the Phase 5 cross-format identity gate depends on. A silent misread here would surface much later as an unexplained physics difference.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: slab-content equality against the fixtures for every dataset; the empty-snapshot case; every abort listed above; the full lifecycle cases; leak checks on every allocating path.
- Commands to run:
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot`
  - `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh`, with output captured, checking the exit code and the absence of `Memory leak detected`
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium check-generated && make MODEL=sage16 SIMULATION=mini-millennium validate-modules`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-unit`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-integration`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the snapshot macro definitions side by side with `src/io/tree/hdf5.c:116-143` and confirm the only differences are the dataset path, the destination array, and the `int64_t` counts; independently read one fixture snapshot with `h5py` and compare several values against the C test's expectations.

### Rollback Path
- Revert the commit; Slice 2's open/close reader remains functional.

## Slice 4: Startup wiring, identity multiplier, and real-data opt-in test

### Intended Change
- `src/core/read_parameter_file.c:789`: resolve `input.tree_type` against `tree_reader_lookup()` first and `snapshot_reader_lookup()` second, storing the match in `MimicConfig.reader` or `MimicConfig.snapshot_reader` respectively and clearing the other. Extend the "Unknown tree_type" message to name both registries. `MimicConfig.TreeExtension` is set from `reader->file_extension` for a tree reader and left empty for a snapshot reader, which derives its own paths from `MimicConfig.TreeName`.
- `src/include/types.h`: add `const struct SnapshotReader *snapshot_reader` and `int64_t UniqueGalaxyIDMultiplier` to `MimicConfig`.
- `src/core/read_parameter_file.c:1354-1368`: restructure the `else if` chain so that it reports a missing or unrecognised `tree_type` when *neither* reader resolved; otherwise checks the resolved reader's `processing_order` against `MimicConfig.ProcessingOrder` and reports the existing compatibility message on a mismatch. The blanket `snapshot_ordered` rejection at `:1357-1359` is removed, leaving `run_processing_driver()` as the single "not implemented yet" point. The unconditional `tree_name` requirement at `:1350-1352` is **not** changed.
- Add `unique_galaxy_id_multiplier` to `parse_simulation_section`'s `valid_keys` (`src/core/read_parameter_file.c:877`) and parse it into `MimicConfig.UniqueGalaxyIDMultiplier`. **Seeding and precedence must be explicit**, because `parse_simulation_section` runs twice — once for the simulation package's `simulation_info.yaml` and again for the run file (`src/core/read_parameter_file.c:144-175`, `:961-980`). Seed the default `(int64_t)TREE_MUL_FAC` (`src/include/constants.h`) exactly once, before either parse, and assign inside the parser **only when the key is present**, so a value set by `simulation_info.yaml` survives a run file that omits it, and an explicit run-file value wins. Reject a non-positive value at config time.
- **Reject a non-default multiplier for tree-ordered configurations.** Every helper in `src/include/galaxy_id.h` — including `mimic_encode_unique_galaxy_id` — is hard-coded to `TREE_MUL_FAC` and takes no configured value, so a tree-ordered run declaring a different multiplier would silently emit IDs computed from the compile-time constant. Until Phase 5 replaces the encoder, a tree-ordered configuration whose multiplier differs from the default fails at config time with a message saying the configurable multiplier is not yet honoured by the tree-ordered identity encoder.
- Add `snapshot_identity_bounds_valid(const struct SnapshotRunInfo *info, int64_t multiplier)` to the snapshot reader interface, and **call it from `open_run` before publishing `SnapshotRunInfo`**, per `docs/dev/SNAPSHOT-HDF5-FORMAT.md:126-130`. It requires `multiplier > 0`; accepts the empty-dataset sentinel `(n_forests_total, max_halo_rank_in_forest) == (0, -1)` unconditionally; and otherwise requires `n_forests_total >= 0`, `max_halo_rank_in_forest >= 0`, `multiplier > max_halo_rank_in_forest`, and `n_forests_total <= INT64_MAX / multiplier - 1` — expressed so the check itself cannot overflow.
- Validate `input.tree_name` for the snapshot reader: accept exactly the literal `snapshot_%03d.h5` and reject every other value at config time with a message naming the accepted literal. Filenames are built with a fixed internal format string and truncation checking; configured text is never passed to a `printf`-family format argument.
- `simulations/micro-uchuu-snapshot/simulation_info.yaml`: declare `unique_galaxy_id_multiplier`.
- `tests/integration/test_processing_order.py`: update the snapshot-ordered expectation and add the two reader/order mismatch cases. Because `read_parameter_file.c:1357-1359` and `tree_driver.c:531-532` carry identical text, the test must additionally assert that `Parameter validation failed` (`read_parameter_file.c:1382-1384`) is **absent** from the output, which is what distinguishes the driver rejection from the old config rejection.
- `simulations/micro-uchuu-snapshot/_tests/unit/test_unit_snapshot_reader_realdata.c`: an opt-in C unit test that calls `open_run` through the snapshot interface against the full 50-snapshot dataset when `simulations/micro-uchuu-snapshot/snapshots` resolves, and returns `TEST_SKIP_WITH` naming the missing path when it does not. It is a C unit test, not a Python integration test, because Phase 4b adds no runtime caller that could reach the reader from a `./mimic` run.
- A bitwise tree-path regression check, with the procedure frozen so the two sides are comparable. No committed sage16 run file under `models/sage16/input/` emits binary output — every one sets `output_format: hdf5`; the only committed binary sage16 input is the physics baseline's `models/sage16/modules/_tests/input/test_physics_binary.yaml`, which is reserved for that baseline — so the vehicle is the generated `build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml`, which `make generate` produces with `output_format: binary`. Both sides use the production build (`TEST_BUILD` unset) at `MODEL=sage16 SIMULATION=mini-millennium`, run a scratch copy of that run file whose `output_directory` points into the gitignored `output/` area, and compare only the galaxy record files. HDF5 output cannot be used: it embeds per-run provenance. The `metadata/` directory is excluded for the same reason.
  Capture "before" from the unmodified tree at the start of the slice. If that was missed, reconstruct it from `before_head` in an isolated worktree — mini-millennium needs no data symlinks because its input is the committed `tests/data/input/trees_063.0`:
  `git worktree add output/bitwise-base <before_head> && (cd output/bitwise-base && make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium)`
  Then, for each side, copy the generated run file with `output_directory` rewritten to `output/bitwise-before/` or `output/bitwise-after/`, run `./mimic <that run file>`, and finally compare pairwise:
  `for f in output/bitwise-before/model_*; do cmp "$f" "output/bitwise-after/$(basename "$f")" || echo "DIFF $f"; done`
  Every `cmp` must be silent, the two directories must contain the same file names, and the loop must print no `DIFF` line. Both run files, both output paths, and the loop's output go into the slice's validation evidence. Clean up with `git worktree remove output/bitwise-base` and `rm -rf output/bitwise-before output/bitwise-after`.

### Acceptance Criteria
- Inputs: run YAMLs and simulation packages; the fixture dataset; and the full converted dataset, which precondition 3 makes present rather than optional for this run.
- Outputs: a snapshot-ordered configuration that validates at startup and stops only at the driver.
- [ ] `tree_type: snapshot_hdf5` with `processing_order: snapshot_ordered` passes config validation and fails at the driver: the output contains "The snapshot-ordered driver is not implemented yet" and does **not** contain "Parameter validation failed".
- [ ] `tree_type: snapshot_hdf5` with `processing_order: tree_ordered` fails with the reader/order compatibility message.
- [ ] `tree_type: consistent_trees_ascii` with `processing_order: snapshot_ordered` fails with the reader/order compatibility message.
- [ ] An unknown `tree_type` fails with a single message naming both registries.
- [ ] In a `USE-HDF5=no` build, `tree_type: snapshot_hdf5` fails as an unrecognised type rather than dereferencing `NULL`.
- [ ] No name is registered in both registries; a test asserts the two name sets are disjoint.
- [ ] Exactly one of `MimicConfig.reader` and `MimicConfig.snapshot_reader` is non-`NULL` after any successful configuration.
- [ ] `simulation.unique_galaxy_id_multiplier` parses, defaults to `(int64_t)TREE_MUL_FAC` when absent, and a zero or negative value is rejected at config time with a clear message.
- [ ] Precedence holds across both parser passes: a value set only in `simulation_info.yaml` survives a run file that omits the key, and a value set in the run file overrides the package value. Both directions are tested.
- [ ] A tree-ordered configuration declaring a multiplier different from `(int64_t)TREE_MUL_FAC` is rejected at config time, with a test asserting the rejection and its message.
- [ ] `input.tree_name` for a snapshot configuration is accepted only as the exact literal `snapshot_%03d.h5`; `snapshot_%d.h5`, `snapshot_%s.h5`, an empty pattern, and an arbitrary string are each rejected at config time.
- [ ] `open_run` calls `snapshot_identity_bounds_valid` before publishing run info, and aborts when it fails.
- [ ] `snapshot_identity_bounds_valid` accepts the fixture dataset's bounds with the default multiplier, accepts the empty-dataset sentinel `(0, -1)`, and rejects: a non-positive multiplier; a negative `n_forests_total`; a negative `max_halo_rank_in_forest` outside the sentinel; a multiplier equal to `max_halo_rank_in_forest`; and a multiplier below `max_halo_rank_in_forest`.
- [ ] The division boundary is tested exactly. For each of `multiplier = 1`, `multiplier = (int64_t)TREE_MUL_FAC`, and `multiplier = INT64_MAX`: `n_forests_total = INT64_MAX / multiplier - 1` is accepted and `n_forests_total = INT64_MAX / multiplier` is rejected. Non-positive multipliers are rejected before any division is performed, so the check itself never divides by zero or overflows.
- [ ] The opt-in real-data test opens all 50 snapshots and reports their halo counts. Precondition 3 is satisfied, so **this test must actually run: a `MIMIC_RESULT: SKIP` is a Slice 4 failure for this run**, indicating a missing dataset or symlink rather than a met criterion. The `TEST_SKIP_WITH` path named in this slice's Intended Change must still be implemented and must name the missing path, so the test remains correct on machines without the dataset; no mechanism for exercising that branch is prescribed here, and adding one is not required.
- [ ] A default-pair tree-ordered binary run's galaxy output is byte-for-byte identical before and after this slice, and the comparison command and its result are recorded in the slice's validation evidence.
- [ ] **Revision 5.** The full fixture-pair unit suite exits 0: `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh` with no test name, exit code checked explicitly. This is the criterion Revision 4 omitted — the command was in the Validation Plan but no criterion bound its exit code. Both restricted `test_parameter_parsing.c` tests must also still pass under the default pair, so the default-pair unit tier stays green.
- [ ] Behaviour that must not change: every existing tree-ordered run file parses, validates, and runs exactly as before; the recorded HDF5 provenance `TreeType` value is unchanged; the default-pair three-tier suite is green; no output, schema, or physics path changes.

### Authorized Surface
- Files allowed to change:
  - `src/core/read_parameter_file.c`
  - `src/include/types.h`
  - `src/io/snapshot/`
  - `simulations/micro-uchuu-snapshot/simulation_info.yaml`
  - `simulations/micro-uchuu-snapshot/_tests/unit/`
  - `tests/integration/test_processing_order.py`
  - `tests/unit/run_tests.sh`
  - `tests/unit/test_parameter_parsing.c` — **Revision 5, restricted.** Only `test_default_processing_order`, `test_explicit_tree_ordered_processing_order`, and their shared fixture helper `write_processing_order_fixture()`. No other test in that file may change, no assertion may be deleted or weakened, and neither test may be removed from its `TEST_RUN` registration.
- Functions/classes/components allowed to change: `parse_input_section`'s reader resolution, `parse_simulation_section`'s key list and parsing, the config validation block at `read_parameter_file.c:1354-1368`, `MimicConfig`, and new snapshot-interface helpers. In `tests/unit/run_tests.sh`, **only** the HDF5-availability skip-name check at `:300-302`, to register the new HDF5-dependent real-data test; no other line of that script may change in this slice. In `tests/unit/test_parameter_parsing.c`, only the three components named above.
- Tests allowed or expected to change: `tests/integration/test_processing_order.py`, the fixture package's C unit tests, and the two restricted `tests/unit/test_parameter_parsing.c` tests.

**Revision 5 — required fix in `tests/unit/test_parameter_parsing.c`.** Both tests currently assert the *selected simulation package's* configuration while appearing to assert a package-independent default, because their fixture is derived from the generated `build/generated/test_inputs/<model>/<sim>/core/test_binary.yaml`, whose simulation configuration is the selected package's `simulation_info.yaml`. Make them package-agnostic: `test_default_processing_order` must assert the compiled package's **declared** processing order instead of a hard-coded `INPUT_PROCESSING_ORDER_TREE`, and `write_processing_order_fixture()` must override `input.tree_type` consistently with whichever order it forces — a tree-ordered reader when it forces `tree_ordered`, and `snapshot_hdf5` with `input.tree_name: snapshot_%03d.h5` when it forces `snapshot_ordered`. Both tests must then pass under **both** the default pair and the fixture pair. This is a correction to a latent test defect that only a non-tree-ordered package could expose; it is not a licence to touch anything else in that file.

### Explicit Non-Goals
- No change to `read_parameter_file.c:1350-1352`; `input.tree_name` stays unconditionally required and the snapshot package declares one.
- No change to `run_processing_driver()` beyond leaving its existing FATAL as the sole not-implemented point; no snapshot driver.
- No change to `UniqueGalaxyID` encoding, `src/include/galaxy_id.h`, or the `TREE_MUL_FAC` definition — only the configured multiplier value and its bounds check.
- **No change to `src/io/output/metadata_hdf5.c` or `src/io/output/master_hdf5.c`.** Both dereference `MimicConfig.reader` unguarded, and both are provably unreachable in Phase 4b because the driver aborts and exits first. Making them reader-kind-neutral belongs to Phase 5's driver-neutral output seam, as one change rather than a partial fix.
- No HDF5-only output enforcement, no output-partition seam work, no galaxy-pool refactor.
- No new `input.*` keys; `processing_order` is already in the accepted set.

### Risk Flags
- Risky surfaces touched: the startup configuration and validation path traversed by every run, including all production tree-ordered runs.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: the config-outcome cases above, the registry-disjointness assertion, the multiplier parse/default/reject cases, the identity-bounds unit test with its overflow cases, and the opt-in real-data C unit test.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium generate && make MODEL=sage16 SIMULATION=mini-millennium check-generated && make MODEL=sage16 SIMULATION=mini-millennium validate-modules`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-unit`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-integration`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - (delegate the long tiers, capture every log under `archive/test-logs/`, check each exit code explicitly)
  - `make MODEL=halos-only SIMULATION=micro-uchuu-snapshot generate && make MODEL=halos-only SIMULATION=micro-uchuu-snapshot && MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh`
  - the bitwise tree-path check, exactly as specified in Intended Change: capture both sides against `build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml` with rewritten output directories, then
    `for f in output/bitwise-before/model_*; do cmp "$f" "output/bitwise-after/$(basename "$f")" || echo "DIFF $f"; done`
    which must print nothing, followed by the worktree and scratch cleanup
  - provenance regression: `make MODEL=halos-only SIMULATION=micro-uchuu-ascii generate && make MODEL=halos-only SIMULATION=micro-uchuu-ascii`, then `./mimic models/halos-only/input/halos-only_micro-uchuu-ascii.yaml`, then read the attribute from the emitted HDF5 file with
    `mimic_venv/bin/python -c "import h5py,sys; print(h5py.File(sys.argv[1])['/RunProperties'].attrs['TreeType'])" <master output file>`
    and confirm it still reads `consistent_trees_ascii`. The attribute is written to the `RunProperties` group of the master file (`src/io/output/metadata_hdf5.c:529-530`, `:581-585`)
  - `make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no clean && make MODEL=sage16 SIMULATION=mini-millennium USE-HDF5=no`
  - `./scripts/beautify.sh`
- Lint (differential, via the `lint` skill): required.
- Manual checks: read the `read_parameter_file.c` diff line by line against the pre-change file and confirm no tree-path branch changed behaviour; confirm the sage16 physics baseline test ran rather than skipped.

### Rollback Path
- Revert the commit. The reader from Slices 2–3 remains present but unreachable from configuration, exactly as before this slice.

## Slice 5: Documentation and pathway closeout

### Intended Change
- `docs/DEVELOPER-GUIDE.md`: a snapshot-reader subsection beside the tree-reader material — the `SnapshotReader` vtable, its registry, the two-registry `tree_type` resolution, the validation `open_run` performs, the slab lifecycle, and the new `simulation.unique_galaxy_id_multiplier` key.
- `docs/USER-GUIDE.md` and `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`: document `simulation.unique_galaxy_id_multiplier` with its named default, as `.agents/skills/mimic-config-and-flags/SKILL.md:157-168` requires for any new configuration key; note that a non-default value is currently accepted only for snapshot-ordered configurations. Both documents must also update `input.tree_name`, which they currently describe only as a base name the reader extends (`all-config-keys.md:36-43`): its meaning is now reader-specific, and the `snapshot_hdf5` reader accepts exactly the literal `snapshot_%03d.h5`.
- `.agents/skills/mimic-config-and-flags/SKILL.md`: add the new key to the skill's own tables.
- `.agents/skills/mimic-simulations-and-readers/SKILL.md`: add `micro-uchuu-snapshot` to the package table and `snapshot_hdf5` to the reader table; add a short "adding a snapshot reader" note; update the `input.tree_type` / `input.processing_order` paragraph now that a snapshot reader exists.
- `.agents/skills/mimic-architecture-contract/SKILL.md`: update weak point W1 — the snapshot reader now exists and is validated, and the driver still fails fast — and the reader/driver seam section for the second registry.
- `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: mark sequence item 3 done with its gate evidence, and item 4 (Phase 5) as next.
- `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`: record Phase 4b complete with its gate result, and resolve the five places where it now disagrees with the repository or with this plan:
  - `:78` — registry selection is tree-registry-first then snapshot, on disjoint names, rather than driven by processing order; and `struct TreeReader` has 12 function-pointer hooks, not 14.
  - `:84` — the claim that "the existing `galaxy_id.h` bounds validation enforces it at startup" is false: every helper in `src/include/galaxy_id.h` is hard-coded to `TREE_MUL_FAC` and takes no configured multiplier. Replace it with the Phase 4b header-bounds validator (`snapshot_identity_bounds_valid`, called from `open_run`) and state that replacing the encoder remains Phase 5.
  - `:118` — the file-inventory row bundles reader wiring, the HDF5-only output check, and the identity multiplier into one item. Split it: reader resolution, the multiplier key with its header-bounds validation, and the tree-ordered non-default rejection land in Phase 4b; the HDF5-only output check, encoder replacement, and output provenance remain Phase 5.
  - `:147` — the payload populator is a Phase 5 deliverable, consistent with `:135`, not Phase 4b.
  - `:157` — multiplier *parsing* and *header-bounds validation* land in Phase 4b; encoding changes and output provenance remain Phase 5.
  Also carry forward the recorded Phase 5 inputs: the `virial.c` tree-index coupling, the shared-HDF5-helper refactor, the identity-field access mechanism, the `src/include/galaxy_id.h` encoder replacement, and the unguarded `MimicConfig.reader` dereferences in `metadata_hdf5.c` and `master_hdf5.c` that the driver-neutral output seam must fix.

### Acceptance Criteria
- Inputs: the committed state after Slices 1–4.
- Outputs: documentation of record and skills consistent with the shipped code.
- [ ] `make check-docs` exits 0.
- [ ] Every reader, registry, config key, and file path named in the new documentation exists in the repository as stated.
- [ ] `simulation.unique_galaxy_id_multiplier` appears in `docs/USER-GUIDE.md`, `.agents/skills/mimic-config-and-flags/SKILL.md`, and `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`, each stating the named default.
- [ ] The dual-driver plan no longer contradicts the shipped behaviour at `:78`, `:84`, `:118`, `:147`, or `:157`, and states 12 function-pointer hooks.
- [ ] All five recorded Phase 5 inputs — the `virial.c` coupling, the shared-HDF5-helper refactor, the identity-field access mechanism, the `galaxy_id.h` encoder replacement, and both output-writer dereferences — appear in the dual-driver plan.
- [ ] `docs/USER-GUIDE.md` and `all-config-keys.md` both describe the reader-specific meaning of `input.tree_name` and the snapshot reader's exact accepted literal.
- [ ] The pathway's item 3 entry states the gate that was actually met, and item 4 is marked next.
- [ ] `docs/dev/SNAPSHOT-HDF5-FORMAT.md` is unchanged: `git diff --stat` shows no modification to it, and `format_version` remains 1.
- [ ] Each edited skill's "Provenance and maintenance" re-verification commands still return what that skill claims.
- [ ] No prose is hard-wrapped; the 100-character guideline applies to code blocks only.
- [ ] Behaviour that must not change: no code, YAML, test, or fixture change of any kind **apart from the single Revision 5 `SKIP_DIRS` entry in `scripts/check_docs.py`**; the full default-pair suite is green and unchanged.
- [ ] **Revision 5.** After the `SKIP_DIRS` addition, `make check-docs` exits 0, and link checking still works: a deliberately broken relative link introduced temporarily under `docs/` is still reported, proving the skip did not disable the check. Remove the deliberate break afterwards and confirm the tree is clean.

### Authorized Surface
- Files allowed to change:
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/USER-GUIDE.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-DUAL-DRIVER-PLAN.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `scripts/check_docs.py` — **Revision 5, restricted to adding `".pm"` to the existing `SKIP_DIRS` set.** Nothing else in that file may change.
- Functions/classes/components allowed to change: only the `SKIP_DIRS` literal in `scripts/check_docs.py`, per the note below.
- Tests allowed or expected to change: none.

**Revision 5 — `make check-docs` cannot pass without this.** `scripts/check_docs.py` walks the repository and validates relative links in every Markdown file it finds, skipping the directories in `SKIP_DIRS`. The `project-manager` toolkit writes its human-facing mirror to `.pm/runs/<run-id>/`, including one `review-*-prompt.md` per commissioned review; those prompts embed the review skill's bundle, whose relative links (`references/review-matrix.md`, `references/scientific-and-language-priorities.md`) do not resolve from `.pm/`. With reviews commissioned for Slices 1–4, `make check-docs` reports 32 broken-link failures, every one of them inside `.pm/`, and exits 2. `SKIP_DIRS` already contains `.orchestrator` and `.ai-orchestrator` for exactly this reason — agent/reviewer evidence directories are not repository documentation — and this plan's own Discovery Record relies on that behaviour but names only those two directories, because it was written against a different toolkit layout. Add `".pm"` to `SKIP_DIRS` alongside them. This is a one-entry addition to a set that already encodes the same rule twice; it neither weakens link checking for any documentation of record nor changes any other behaviour. Verify afterwards that `make check-docs` exits 0 **and** that a deliberately broken link introduced under `docs/` is still caught, so the skip cannot be mistaken for disabling the check.

### Explicit Non-Goals
- No code, YAML, Makefile, or test change, **with the single Revision 5 exception of adding `".pm"` to `SKIP_DIRS` in `scripts/check_docs.py`** as the Authorized Surface note requires. No production code path, no `src/`, no build system, and no test may change.
- **No edit to `docs/dev/SNAPSHOT-HDF5-FORMAT.md`.** The frozen contract is consumed, not amended, by this plan.
- No `docs/VISION.md` edit — the dual-driver plan defers the vision review until the Phase 5 identity gate passes.
- No archiving or deletion of any plan file — this plan is archived after Phase 5.
- `AGENTS.md` / `CLAUDE.md` are not authorized; if the documentation map needs a new row, report it rather than editing.

### Risk Flags
- Risky surfaces touched: documentation of record and four project skills, including corrections to an active plan other work depends on.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan
- Tests to add/update: none.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium check-docs`
  - `./scripts/beautify.sh`
  - `make MODEL=sage16 SIMULATION=mini-millennium generate`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-unit`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-integration`
  - `make MODEL=sage16 SIMULATION=mini-millennium tests-scientific`
  - `git diff --stat` — confirms documentation and skills only, with `docs/dev/SNAPSHOT-HDF5-FORMAT.md` absent from the list
- Lint (differential, via the `lint` skill): required.
- Manual checks: follow every new or changed link and anchor by hand; run each edited skill's re-verification commands and confirm the output matches the skill's claims.

### Rollback Path
- Revert the commit; code is unaffected.

---

## Deferred and Out of Scope (recorded so nothing is silently dropped)

- **Full micro-Uchuu dataset regeneration** — operator precondition 3, **done 2026-08-03** and no longer deferred. Phase 5's prerequisite is satisfied, and Slice 4's opt-in test is now a real check rather than a SKIP.
- **`populate_halo_payload_from_snapshot.inc`** and the accompanying snapshot accessor family — Phase 5. Adding it also requires an entry in `scripts/check_generated.py`'s hand-maintained file list, or it will silently drift.
- **`virial.c` tree-index coupling** — `get_virial_mass` and its siblings call `mimic_tree_get_*` internally, so Phase 5 must either emit snapshot-flavoured virial helpers or refactor them to take explicit arguments. The largest hidden coupling found while planning.
- **Unguarded `MimicConfig.reader` dereferences in the output writers** — `src/io/output/metadata_hdf5.c:582` and `src/io/output/master_hdf5.c:46-55`, `:76-80`, `:151-152`. Provably unreachable in Phase 4b because the driver aborts and exits first; both must be made reader-kind-neutral by Phase 5's driver-neutral output seam before any snapshot run can produce output.
- **Identity-field access mechanism** — `ForestIndex` and `HaloRankInForest` have no core role; Phase 5 decides between an optional-role concept in `core_properties.yaml` and direct `RawHalo` access from the snapshot driver.
- **Shared HDF5 read utilities** — lifting `ct_read_attribute`, the rank-1 extent helper, and the field-handle cache out of `read_ctrees_hdf5.c` into `src/io/hdf5_read_utils.{c,h}`. Deliberately not done here: it would touch a production tree reader whose byte-identical output is this phase's gate.
- **Open-time full-column validation cost at production scale** — the spec requires the reader to verify invariant 5 at open, which means scanning `SnapNum` (~60–72 GB across a Shin-Uchuu dataset: 15–18 billion halos × 4 B) plus the two int64 identity columns for the measured-bounds check (a further ~240–290 GB). Each scan is streaming and needs no memory beyond one column buffer, but the aggregate startup read is substantial. This plan pays it, because narrowing a stated reader obligation for convenience is what revision 1 was rejected for. If measurement at production scale shows it is prohibitive, relaxing or re-scoping it is a deliberate `format_version = 2` decision with converter, fixture, and reader changes — and both costs should be reconsidered together.
- **Physical header agreement with configuration** — comparing `box_size_mpc_h`, `particle_mass_msun_h`, and the cosmology values against the configured simulation is not a reader obligation in the spec and nothing in Phase 4b consumes them. Phase 5 should add it where `get_virial_mass` consumes `MimicConfig.PartMass`, and must handle the unit difference: the header stores native Msun/h while `PartMass` is in 1e10 Msun/h.
- **`src/include/galaxy_id.h` encoder replacement** — every helper there is hard-coded to `TREE_MUL_FAC`. Phase 5 must take the configured multiplier, at which point Slice 4's rejection of a non-default multiplier for tree-ordered runs can be lifted.
- **Test-gating membership** — adding `micro-uchuu-snapshot` to `FULL_MODEL_TEST_SIMULATIONS` and `PRODUCTION_TEST_CONFIG_SIMULATIONS` in `scripts/discovery.py`, once a driver exists to run a model.
- **MPI behaviour, chunked output, `--skip` resume, and HDF5-only output enforcement** for snapshot runs — all Phase 5.
- **Converter test-tier integration** — deferred by the converter plan until the snapshot reader exists; now unblocked, but out of this plan's scope.
- **Package-dependent assertions in core unit tests (Revision 5, structural — the most important item here).** Core tests under `tests/unit/` are driven by the generated `build/generated/test_inputs/<model>/<sim>/core/test_binary.yaml`, whose simulation configuration is the **selected** package's `simulation_info.yaml`. Any core test asserting a "default" is therefore asserting the selected package's value. Revision 5 fixes the two instances this plan tripped over in `test_parameter_parsing.c`; it does **not** sweep the rest. Phase 5 adds further non-tree-ordered packages and will keep tripping this, so Phase 5 should begin with a sweep of `tests/unit/` for assertions that look package-independent but are not.
- **Reader accepts a wholly empty dataset carrying non-sentinel identity metadata.** Slice 2's Invariant 5 enumeration names only "sentinel carried with halos present aborts" and "zero forest count with a non-empty dataset aborts", so the shipped reader accepts a dataset with zero halos in every snapshot whose header declares, say, `(3, 6)`. No value is misread and Phase 4b's driver aborts before consuming it. Deliberately not amended in retrospectively (see Revision 5); Phase 5 should decide whether to require the sentinel and add the check plus a corrupt-fixture case.
- **Reader strictness gaps tolerated in Phase 4b.** Objects nested beneath `/header` are not rejected (only the root object set and the header attribute set are enumerated), and `snapshot_h5_type_matches()` compares datatype class, storage size and integer signedness rather than exact standard/IEEE types, so a same-size non-contract datatype is accepted and converted through the native memory type. Neither misreads a value. A Phase 5 hardening pass should tighten both, with corrupt-fixture cases.
- **Fixture-tooling and test-harness hardening.** `simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py`'s `regenerate()` is not transactional — it unlinks the committed `.h5` set before rewriting, so a mid-run failure leaves a partial set beside a stale manifest, contradicting `FixtureError`'s own docstring; `check_fixture_conformance.py`'s `check_filters()` docstring claims it enforces "chunked and UNFILTERED" while only checking filters; neither the conformance checker nor the generator is wired into any test target, so the committed fixture is protected only by manual re-runs; `expect_fatal()` in the fixture unit test drains at most 16383 bytes of child stderr before `waitpid`, so a future child emitting ≥16 KB would deadlock the unit tier; the comparison buffers there are hard-sized from `FIXTURE_MAX_HALOS`, with no assertion tying them to the loaded slab; and the link-range validator reads members via `*(const int *)` with `offsetof` and no `_Static_assert` tying those members to the generated width, so a future package typing a link field wider would silently validate a truncated value. All are latent or unreachable under the frozen contract and none is a Phase 4b defect.

---

## Standard Gate

```bash
make MODEL=sage16 SIMULATION=mini-millennium generate
make MODEL=sage16 SIMULATION=mini-millennium check-generated
make MODEL=sage16 SIMULATION=mini-millennium validate-modules
make MODEL=sage16 SIMULATION=mini-millennium tests-unit
make MODEL=sage16 SIMULATION=mini-millennium tests-integration
make MODEL=sage16 SIMULATION=mini-millennium tests-scientific
```

Long-running test output is captured under `archive/test-logs/`, exit codes are checked explicitly, and any non-zero exit code is a failure regardless of log text. The selector discipline above binds every command.

---

## Definition of Done

- `simulations/micro-uchuu-snapshot/` exists, generates clean, and ships small committed fixtures whose production-layout source passed the converter's producer validation battery and whose committed form passes a structural conformance check.
- `snapshot_hdf5` is registered behind its own `SnapshotReader` vtable, validates a dataset's structure, headers, snapshot-list agreement, invariant 5 in full (including the measured identity bounds), and the identity-multiplier bounds at open, loads and releases slabs under a defined lifecycle, and enforces the format's link-range rules by aborting, never repairing.
- A configured `UniqueGalaxyID` multiplier is parsed with defined precedence across both parser passes, bounds-checked against the dataset headers, and refused for tree-ordered runs until Phase 5 replaces the hard-coded encoder.
- A snapshot-ordered configuration validates at startup and stops at exactly one place: the unimplemented driver.
- Reader unit tests pass under `SIMULATION=micro-uchuu-snapshot`, covering every corrupt-input abort and showing no leak diagnostic.
- The tree-ordered path is byte-identical across Slice 4, demonstrated by an explicit bitwise comparison, and the default-pair three-tier suite is green.
- Documentation of record, the configuration references, the pathway, the dual-driver plan's corrected Phase 4b/5 boundaries, and the affected skills describe what shipped. The frozen format specification is unchanged.

---

## Next Chat Prompt

Execute under `project-manager` (Mode B). Use the single authoritative Mode B launcher in `project-manager`'s `SKILL.md` ("Launcher"), with its first line filled in as:

```text
Plan file: docs/dev/MIMIC-SNAPSHOT-READER-PLAN.md
```

Preconditions 2 and 3 were both discharged on 2026-08-03. Re-confirm each is still true before starting, and record the evidence: `pandas` imports in `mimic_venv`, and `simulations/micro-uchuu-snapshot/snapshots` resolves to 50 `snapshot_NNN.h5` files plus `forests.h5`. Slice 4's real-data test must therefore run rather than SKIP.
