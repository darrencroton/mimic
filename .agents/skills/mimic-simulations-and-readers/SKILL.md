---
name: mimic-simulations-and-readers
description: Mimic simulation packages and merger-tree readers - the input side of the pipeline. Load when a task involves anything under simulations/ (simulation_info.yaml, halo_properties.yaml, a_list snapshot lists, snapshots/ data symlinks), tree formats and readers (lhalo_binary, lhalo_hdf5, consistent_trees_ascii, consistent_trees_hdf5, tree_type, tree_name, processing_order), src/io/tree/, struct RawHalo or TreeReader, partition models, forest distribution and chunked output planning (target_file_size_mb, forests_per_file), adding a new simulation package, adding a new tree reader/format, cosmology or particle-mass metadata, micro-Uchuu cross-format validation, or fix_flybys / flyby topology questions.
---

# Mimic Simulations and Readers

A simulation package (`simulations/<name>/`) wraps one merger-tree catalog with its cosmology, units, snapshot list, and on-disk field description. A tree reader (`src/io/tree/`) teaches Mimic to parse one *format* — one reader serves every catalog written in that format. This skill covers both sides of the input boundary and how to extend each.

## When to use / when NOT to use

Use for: simulation package work, reader/format work, input-data layout, chunk planning, cross-format questions.

Do NOT use for:
- The property YAML schema itself (field keys, types, units grammar) — see the `mimic-properties` skill.
- Running Mimic on a simulation — see the `mimic-run-and-operate` skill.
- What halo quantities mean physically — see the `mimic-sam-reference` skill.
- Core driver/dispatch architecture — see the `mimic-architecture-contract` skill.

## First actions

1. Read the target package's `README.md` and `simulation_info.yaml` — packages document their own data provenance and maintenance obligations.
2. Check the data actually exists: `ls -lL simulations/<name>/snapshots/` — every package's `snapshots/` is a machine-local, gitignored symlink except mini-millennium (downloadable via `./scripts/first_run.sh`). Tests that need absent production data must skip cleanly.
3. Always pass both selectors for the run you intend to test. Creating or editing a package does nothing until the executable is regenerated with that selector pair, e.g. `make MODEL=halos-only SIMULATION=<name> generate && make MODEL=halos-only SIMULATION=<name>`.
4. Smoke-test cheaply before physics: run the `halos-only` model on the package first (`make MODEL=halos-only SIMULATION=<name>` + the matching run file) — the package READMEs prescribe this. **Exception:** this does not apply to a snapshot-ordered package. `micro-uchuu-snapshot` is input-only until the Phase 5 driver exists — no model can run on it, and it ships no run file — so its equivalent of a smoke test is the fixture unit tier, `MODEL=halos-only SIMULATION=micro-uchuu-snapshot tests/unit/run_tests.sh` (see section 1).

## 1. Package anatomy and the shipped eight

Required files: `simulation_info.yaml` (paths, cosmology, units, chunking defaults), `halo_properties.yaml` (the on-disk record — see section 3), `<name>.a_list`. Conventional: `snapshots/` (data dir or symlink), `plot_profile.yaml`, `README.md`, `_tests/`.

| Package | tree_type | files | tree_name | box (Mpc/h) | Cosmology family |
|---|---|---|---|---|---|
| mini-millennium | lhalo_binary | 0–7 | `trees_063` | 62.5 | Millennium (Ωm 0.25, ΩΛ 0.75, h 0.73, mp 0.0860657) |
| millennium | lhalo_binary | 0–511 | `trees_063` | 500.0 | Millennium |
| micro-uchuu | lhalo_binary | 0–3 | `Uchuu100_Planck_lhalo_binary` | 100.0 | Uchuu/Planck-2015 (Ωm 0.3089, ΩΛ 0.6911, h 0.6774, mp 0.0325) |
| micro-uchuu-hdf5 | consistent_trees_hdf5 | 0–0 | `MicroUchuu_mergertree_info.h5` | 100.0 | Uchuu |
| micro-uchuu-ascii | consistent_trees_ascii | 0–0 | `tree_0_0_0.dat` | 100.0 | Uchuu |
| mini-uchuu | lhalo_binary | 0–127 | `Uchuu400_Planck_lhalo_binary` | 400.0 | Uchuu |
| uchuu | consistent_trees_hdf5 | 0–1999 | `mergertree_info.h5` | 2000.0 | Uchuu |
| micro-uchuu-snapshot | snapshot_hdf5 | 0–0 | `snapshot_%03d.h5` | 100.0 | Uchuu |

(Units: mp in 1e10 Msun/h. For ASCII, first/last_file are metadata-only; the reader follows `forests.list`/`locations.dat`.)

`micro-uchuu-snapshot` is the odd one out: the same micro-Uchuu catalog converted to the snapshot-ordered HDF5 format (`docs/dev/SNAPSHOT-HDF5-FORMAT.md`), so it is the only package declaring `input.processing_order: snapshot_ordered` and the only one whose `tree_name` is a format-fixed literal rather than a name the user chooses. Its first/last_file are metadata only — the reader derives its file set from the snapshot list. It ships small re-chunked fixtures under `_tests/data/` plus a committed generator and conformance checker under `_tests/input/`; the full 50-snapshot dataset is a machine-local gitignored `snapshots` symlink. It cannot run a model yet (no snapshot driver), so it ships no run file and is deliberately absent from `scripts/discovery.py`'s `FULL_MODEL_TEST_SIMULATIONS` and `PRODUCTION_TEST_CONFIG_SIMULATIONS`, and it also declares `simulation.unique_galaxy_id_multiplier` — see the `mimic-config-and-flags` skill.

**The a_list contract**: one scale factor per line, earliest→latest (increasing a, decreasing z); the line count defines snapshot indices 0..N-1; the last line is normally a=1.0 (z=0). All redshift and timestep math derives from this file — ordering is critical.

## 2. The micro-Uchuu triplet — the cross-format validation asset

`micro-uchuu`, `micro-uchuu-hdf5`, `micro-uchuu-ascii` are the SAME catalog in three formats, and the test system deliberately runs full model validation on all three (see `mimic-validation-and-qa`) so the L-Halo binary, ctrees-HDF5, and ctrees-ASCII read paths validate against each other. Use the triplet whenever you need to discriminate "reader effect" from "physics effect".

**Known, accepted divergence (not a bug)**: the ASCII reader calls `fix_flybys()` (`src/io/tree/ctrees/ctrees_utils.c`) during topology reconstruction — at the final snapshot a ctrees forest can hold multiple FoF groups from historical flybys; the fix keeps the most massive as sole central, demotes the rest, and negates their `MostBoundID` as a marker (~55k halos at micro-Uchuu snap 49; z=0 Type-0 counts drop ~10–25% per mass bin vs the other two readers). All earlier snapshots are byte-identical across the three formats. Documented in `simulations/micro-uchuu-ascii/README.md`; do not "fix" the difference away.

## 3. halo_properties.yaml — the on-disk contract

The `halo_properties:` list describes the complete on-disk record and generates `struct RawHalo`. Two styles exist, matching the two reader families:

- **L-Halo style** (mini-millennium etc.): every field of the fixed binary record, **in on-disk order** — order and types ARE the binary layout. Inert bookkeeping fields (`M_TopHat`, `FileNr`, ...) are listed purely to preserve the record layout, with a `notes:` line saying so.
- **ctrees style** (micro-uchuu-hdf5/-ascii, uchuu): only the fields the bridge writes — the readers convert their native `struct halo_data` into `RawHalo` by field name via `bridge_halo_data_to_rawhalo()` (shared by ASCII and HDF5 readers), so no padding fields are needed. Native units are declared per field (e.g. `M_Crit200` in `Msun/h`, converted ×1e-10 to reference units by the generated accessors); `Len` is derived by the reader as `round(Mvir × 1e-10 / particle_mass)` (stated in the package YAML header and implemented in `read_ctrees_ascii.c`).

Core-role binding: fields that satisfy core required inputs declare `provides_core_role` — the roles are `Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`, `NextHaloInFOFgroup`, `SnapNum`, `Len`, `HaloMass`. The generator emits `mimic_tree_get_<Role>()` accessors so core traversal never hard-codes catalog names. Tree-link/index/count roles must bind to scalar integers; mass roles to scalar numerics. Full key grammar: `mimic-properties`.

## 4. Readers, formats, and partition models

Registered readers (`reader_table[]` in `src/io/tree/registry.c`; case-insensitive lookup by `tree_type`):

| tree_type | File | Partition model | Build |
|---|---|---|---|
| `lhalo_binary` | `src/io/tree/binary.c` | PARTITION_PER_FILE | any |
| `consistent_trees_ascii` | `src/io/tree/read_ctrees_ascii.c` | PARTITION_ENUMERATED | any |
| `lhalo_hdf5` | `src/io/tree/hdf5.c` | PARTITION_PER_FILE | HDF5 only (`#ifdef HDF5` guards) |
| `consistent_trees_hdf5` | `src/io/tree/read_ctrees_hdf5.c` | PARTITION_ENUMERATED | HDF5 only |

Note: `lhalo_hdf5` is registered and documented but no shipped package currently uses it.

There is a **second registry** for snapshot-ordered readers (`snapshot_reader_table[]` in `src/io/snapshot/registry.c`, same case-insensitive lookup by `tree_type`, names disjoint from the tree registry):

| tree_type | File | Shape | Build |
|---|---|---|---|
| `snapshot_hdf5` | `src/io/snapshot/read_snapshot_hdf5.c` | run open/close + per-snapshot slabs (no partitions) | HDF5 only |

`tree_name` interpretation is reader-specific: prefix before the file number for `lhalo_binary` (`trees_063.0`); literal filename for both ctrees readers; explicit filename or `%d` pattern for `lhalo_hdf5`; and for `snapshot_hdf5` exactly the literal `snapshot_%03d.h5` — the format fixes the convention, so any other value is a startup error.

`input.tree_type` selects the FORMAT; `input.processing_order` selects the DRIVER (default `tree_ordered`) — never overload one with the other. Both registries are consulted for one `tree_type`, and every reader declares the single driver it feeds, so startup validation rejects a mismatched pair in either direction. A snapshot reader now exists, so `snapshot_ordered` no longer fails at configuration: a `snapshot_hdf5` + `snapshot_ordered` run passes configuration validation and then fails at exactly one place — `run_processing_driver()` in `src/core/tree_driver.c` — because the snapshot driver itself is Phase 5 work. Distinguish the two rejections by whether the output also contains "Parameter validation failed". **The dataset is never opened on that path:** the reader's `open_run` validation and its slab-load link checks are implemented and unit-tested, but nothing in `src/` calls them yet (the Phase 5 driver will), so a missing or corrupt `snapshot_NNN.h5` fails with the same not-implemented error as a good one. Exercise the reader's checks through the fixture unit tests, not through `./mimic`.

**Partition models**: a partition is the unit of output (one output file set per partition, named by its output id); a unit is one independently processed tree/forest. `PARTITION_PER_FILE` = one partition per input file, output id = file number (L-Halo readers). `PARTITION_ENUMERATED` = the reader publishes a deterministic chunk list with costs; the driver assigns chunks to MPI ranks but output ids stay the reader's chunk ids, independent of task count (ctrees readers). Where the model is observed outside readers (unique-ID offsets, offset scan, HDF5 master file): `mimic-architecture-contract`.

**ctrees-HDF5 memory model** (internal, no YAML knobs — the Developer Guide forbids adding any without a new plan): fixed 128 MiB per-rank read window (`CTREES_READ_WINDOW_BYTES`), per-partition cached dataset handles validated at open, oversized forests served by direct read through the same cached handles.

**Forest distribution** (`input.forest_distribution_scheme`: `uniform`|`linear`|`quadratic`|`exponent`|`generic_power`, + `exponent_forest_dist_scheme`): weights MPI chunk assignment by per-forest halo counts; honored by `consistent_trees_hdf5` (per-forest counts known up front); the ASCII reader's chunk costs are uniform.

## 5. Chunked output planning

Two keys, legal in both `simulation_info.yaml` (as catalog-scale defaults) and the run file (as overrides): `output.target_file_size_mb` (soft MiB target for HDF5 chunk-size estimation from input halo counts; default 4096) and `output.forests_per_file` (exact deterministic forest count per chunk; 0 = derive from the size target). **`consistent_trees_ascii` requires `forests_per_file > 0`** — it cannot estimate sizes (enforced with an explicit ERROR in `read_ctrees_ascii.c`; micro-uchuu-ascii ships 100000). Chunk ids are independent of `NTask`, so serial and MPI runs produce identical output layout; `--skip` resumes complete chunks and FATALs on partial ones.

## 6. Adding a new simulation package

1. `mkdir -p simulations/<name>/snapshots` and place/symlink the tree data.
2. Write `simulation_info.yaml` (copy the closest shipped package: same tree format = same shape). Required at validation: `input.simulation_dir`, `input.tree_name`, `input.tree_type`, `input.snapshot_list_file`, nonzero `simulation.box_size` and `simulation.cosmology.hubble_h`. Dimensioned scalars use `{value, units, h_convention}`; a bare number means already-reference-units.
3. Write `<name>.a_list` (section 1 contract) and `halo_properties.yaml` (section 3; copy the style matching your format — micro-uchuu-ascii is the ctrees reference, mini-millennium the L-Halo reference).
4. Write `README.md` (data provenance, units, how to obtain data) and `_tests/` with fixture-sized inputs that skip cleanly when production data is absent.
5. Create a run file under `models/<model>/input/<model>_<name>.yaml` — `model.name` + `simulation.name` derive all package paths; `simulation.config` is only for alternate metadata fixtures.
6. Regenerate and build WITH THE SELECTOR — the executable embeds the catalog: `make MODEL=<model> SIMULATION=<name> generate && make MODEL=<model> SIMULATION=<name>`.
7. Smoke-test halos-only first, then physics; then `make MODEL=<model> SIMULATION=<name> tests summary` (captured, exit code checked).

## 7. Adding a new tree reader (format)

1. Create `src/io/tree/read_<format>.c` implementing one `const struct TreeReader` (vtable in `src/io/tree/reader.h`). Fields exist only because a wired reader uses them — no speculative callbacks; fold setup/teardown into `open_partition`/`close_partition`. Set `.processing_order = INPUT_PROCESSING_ORDER_TREE` for the current driver.
2. Bridge the format's records into the generated `struct RawHalo` **by field name**, declaring native units in the simulation package YAML — never hardcode unit conversions in reader C (the generated accessors convert at the boundary).
3. Add one row to `reader_table[]` in `src/io/tree/registry.c`; guard both the `extern` and the row with `#ifdef HDF5` if HDF5-dependent (lookup then returns NULL in non-HDF5 builds → clean fail-fast).
4. Pick the partition model that matches the format's on-disk organization; the ctrees readers are the worked `PARTITION_ENUMERATED` reference (chunk planning, costs, `GlobalForestOffset`).
5. Keep it warning-clean under the project flags and exercise it from `tests/unit/` (HDF5 reader sources compile in the unit harness when dev libs exist); validate end-to-end against a fixture package.
6. No run-YAML changes are needed beyond `tree_type` for a tree-ordered format.

## 8. Adding a snapshot reader (different family, different vtable)

Snapshot-ordered readers are a separate family with their own small vtable — `struct SnapshotReader` in `src/io/snapshot/reader.h`: `name`, `processing_order`, and the hooks `open_run`, `close_run`, `snapshot_halo_count`, `load_slab`, `release_slab`. There are no partitions and no units; the working set is one snapshot's halo population (a *slab* of `struct RawHalo`, `int64_t` counts throughout). Do NOT widen `struct TreeReader` — its 12 hooks are partition/unit-shaped and its `REQUIRE_READER_HOOK` fail-fast would be defeated by two disjoint hook sets.

1. Implement one `const struct SnapshotReader` in `src/io/snapshot/read_<format>.c`. **Filename rule (footgun):** an HDF5-dependent reader file *must* end in `hdf5.c` (`Makefile` drops that pattern from `USE-HDF5=no` builds), while `registry.c` and `interface.c` must *not*, because the configuration path calls `snapshot_reader_lookup()` in every build.
2. Append one row to `snapshot_reader_table[]` in `src/io/snapshot/registry.c`, `#ifdef HDF5`-guarding both the `extern` and the row when needed; a non-HDF5 build then registers nothing and the lookup returns `NULL`. Keep the name disjoint from the tree registry.
3. Validate the whole dataset at `open_run` — structure first, then header values, then agreement with the a_list, then measured identity bounds via bounded hyperslab scans — and abort with path, object, and value rather than repairing. Fill slabs by including the generated `read_tree_hdf5_properties.inc` under your own macros; no generator change is needed.
4. Honour the slab lifecycle: `load_slab` requires an empty destination handle, `release_slab` returns it to empty and is a no-op when already empty, `close_run` aborts if a slab is still loaded.
5. Ship a fixture package with committed small fixtures and C unit tests under `simulations/<name>/_tests/unit/` (`micro-uchuu-snapshot` is the worked reference), and register new sources in the hand-maintained lists in `tests/unit/run_tests.sh` — the unconditional `IO_SRCS` (`:158`), the HDF5-only group appended to it (`:160`), the per-source `-DHDF5` `case` list (`:199-200`), and the HDF5-availability skip-name check (`:302-303`). Anything HDF5-independent must go in the *unconditional* group, or a non-HDF5 build fails to link regardless of which tests are skipped.

Full walkthrough, including what `open_run` checks and the identity multiplier: `docs/DEVELOPER-GUIDE.md` → "Snapshot-ordered readers".

## Provenance and maintenance

Verified against the live repo 2026-07-04; the snapshot-reader material added 2026-08-04. Re-verify drift-prone specifics:

```bash
sed -n '16,31p' src/io/tree/registry.c                                  # registered tree readers
grep -n "snapshot_reader_table\|\.name = " src/io/snapshot/registry.c src/io/snapshot/read_snapshot_hdf5.c   # snapshot registry
sed -n '/^struct SnapshotReader {/,/^};/p' src/io/snapshot/reader.h      # the snapshot vtable
grep -n "CTREES_READ_WINDOW_BYTES" src/io/tree/read_ctrees_hdf5.c        # 128 MiB window
grep -n "forests_per_file > 0" src/io/tree/read_ctrees_ascii.c           # ASCII requirement
grep -n "bridge_halo_data_to_rawhalo" src/io/tree/read_ctrees_*.c        # shared bridge
for s in simulations/*/simulation_info.yaml; do grep -H "tree_type" "$s"; done   # package table
grep -n -i "fix_flybys" simulations/micro-uchuu-ascii/README.md          # accepted divergence doc
sed -n '/^required_inputs/,/^halo_properties/p' src/core/core_properties.yaml   # core roles
```

The package table (boxes, file ranges) drifts when packages are added; reader internals drift only with `src/io/tree/`; the format-vs-driver separation and partition-model semantics are architectural and durable.
