# Mimic Snapshot-HDF5 Format Specification

**Purpose**: Define the frozen on-disk contract for snapshot-ordered HDF5 merger-tree input — the format produced by external converters and consumed by Mimic's `snapshot_hdf5` reader and snapshot-ordered driver.

**Status**: Frozen at `format_version = 1`. Every normative statement in this document is part of the contract. Any change that alters the meaning, layout, ordering, or validation rules of files on disk requires incrementing `format_version` and updating this specification; readers must reject files whose `format_version` they do not support. Corrections that bring the wording into line with the semantics `format_version = 1` always denoted are recorded under [Errata](#errata) instead of bumping the version — see that section for the rule and the full list.

---

## Table of Contents

1. [Role and Scope](#role-and-scope)
2. [File Set and Naming](#file-set-and-naming)
3. [Header Attributes](#header-attributes)
4. [Halo Datasets](#halo-datasets)
5. [Link Scope](#link-scope)
6. [Format Invariants](#format-invariants)
7. [Ordering Contracts](#ordering-contracts)
8. [Galaxy Identity Encoding](#galaxy-identity-encoding)
9. [Validation Requirements](#validation-requirements)
10. [Storage Layout](#storage-layout)
11. [Simulation Package Integration](#simulation-package-integration)
12. [Producing Snapshot-HDF5 Data](#producing-snapshot-hdf5-data)
13. [Versioning Policy](#versioning-policy)
14. [Errata](#errata)

---

## Role and Scope

Snapshot-ordered input groups halos by snapshot rather than by forest, so the working set of a run is one snapshot's halo population instead of one forest's history. This is the input format for runs declaring `input.processing_order: snapshot_ordered` with `input.tree_type: snapshot_hdf5`.

Mimic never converts between orderings internally. Snapshot-HDF5 data is produced offline by an external converter (see [Producing Snapshot-HDF5 Data](#producing-snapshot-hdf5-data)) and validated both at conversion time and again by the reader at load time. The format carries everything the snapshot driver needs to reproduce tree-ordered results exactly — topology links, chain orderings, and galaxy-identity components are converter-owned facts recorded in the file, never reconstructed heuristically by Mimic.

This document owns the format. The reader and driver that consume it, and any converter that produces it, conform to this specification — not the other way around.

## File Set and Naming

A snapshot-HDF5 dataset consists of:

- **One HDF5 file per snapshot**: `snapshot_NNN.h5`, where `NNN` is the zero-padded snapshot index from `000` to the final snapshot, in ascending scale-factor order. Every snapshot in the run's snapshot list must have a file, including snapshots containing zero halos.
- **One run-level sidecar**: `forests.h5`, written once per dataset. Provenance only; Mimic never reads it.

Each `snapshot_NNN.h5` contains exactly two HDF5 objects: the group `/header` (scalar metadata as HDF5 attributes) and the group `/halos` (one dataset per halo field, all of length `n_halos`, stored as a struct-of-arrays).

## Header Attributes

All scalar metadata lives as HDF5 attributes on the `/header` group:

| Attribute | Type | Semantics |
|---|---|---|
| `format_version` | int32 | Contract version of this file; this specification defines version 1 |
| `links_adjacent` | int32 | Always 1. Declares the adjacency invariant (see [Format Invariants](#format-invariants)); asserted by producer and reader |
| `scale_factor` | float64 | Scale factor *a* of this snapshot |
| `snapshot_number` | int32 | Snapshot index; must equal the `NNN` in the filename |
| `n_halos` | int64 | Number of halos in this file; must equal the length of every `/halos` dataset |
| `n_forests_total` | int64 | Run-scoped total forest count; identical in every file of the dataset (identity bound check) |
| `max_halo_rank_in_forest` | int64 | Run-scoped maximum `HaloRankInForest`; identical in every file of the dataset (identity bound check) |
| `box_size_mpc_h` | float64 | Simulation box size, Mpc/h comoving |
| `particle_mass_msun_h` | float64 | Simulation particle mass, Msun/h |
| `omega_matter` | float64 | Ωm |
| `omega_lambda` | float64 | ΩΛ |
| `hubble_h` | float64 | Dimensionless Hubble parameter h |

`n_forests_total` and `max_halo_rank_in_forest` are properties of the whole dataset, not of one snapshot; producers stamp the same run-scoped values into every file so any single file suffices for identity bounds validation at startup.

## Halo Datasets

All datasets live under `/halos`, each of length `n_halos` (vectors are `[n_halos, 3]`). **Dataset names and types are normative**: for every dataset except `ForestIndex` and `HaloRankInForest`, they must match what the consuming simulation package's `halo_properties.yaml` declares, so the generated `RawHalo` struct and accessors consume the file directly. `ForestIndex` and `HaloRankInForest` are snapshot-format identity metadata, not catalog halo properties (see [Simulation Package Integration](#simulation-package-integration)): the reader consumes them directly by dataset name into `struct SnapshotSlab`'s own `forest_index`/`halo_rank_in_forest` arrays, so they are exempt from the `halo_properties.yaml` declaration rule. The names below follow the established Consistent-Trees bridge contract (as in `simulations/micro-uchuu-ascii/halo_properties.yaml`).

| Dataset | Type | Semantics |
|---|---|---|
| `Descendant` | int32[N] | Index in the snapshot N+1 file of this halo's descendant; −1 if none. Not consumed by the driver (progenitor gathering uses `FirstProgenitor`/`NextProgenitor`); kept as the round-trip validation key |
| `FirstProgenitor` | int32[N] | Index in the snapshot N−1 file of the main progenitor; −1 if none |
| `NextProgenitor` | int32[N] | Index in **this** snapshot's file of the next sibling progenitor (a halo sharing this halo's descendant); −1 if no next sibling |
| `FirstHaloInFOFgroup` | int32[N] | Index in this snapshot of the FoF central; self-index for the central itself |
| `NextHaloInFOFgroup` | int32[N] | Index in this snapshot of the next FoF-group member; −1 if last |
| `Len` | int32[N] | Particle count: `round(Mvir_native × 1e-10 / particle_mass)` with `particle_mass` in 1e10 Msun/h. Zero is legal (treated downstream as the orphan sentinel); negative is not |
| `SnapNum` | int32[N] | Snapshot index; every value must equal the header `snapshot_number` |
| `M_Crit200` | float32[N] | Halo mass in **native Msun/h** (the generated accessor converts to Mimic's 1e10 Msun/h reference basis) |
| `Pos` | float32[N,3] | Position, Mpc/h comoving |
| `Vel` | float32[N,3] | Peculiar velocity, km/s |
| `Spin` | float32[N,3] | Dimensionless spin J/Mvir (normalisation applied by the producer; components of zero-mass halos are carried unnormalised) |
| `VelDisp` | float32[N] | Velocity dispersion, km/s |
| `Vmax` | float32[N] | Maximum circular velocity, km/s |
| `MostBoundID` | int64[N] | Source-catalog halo id (Consistent-Trees `id`), negated for flyby-demoted centrals per the reference reader semantics (see [Ordering Contracts](#ordering-contracts)) |
| `ForestIndex` | int64[N] | Dense run-scoped forest number in `[0, n_forests_total)`; identity component consumed directly by `UniqueGalaxyID` |
| `HaloRankInForest` | int64[N] | Within-forest halo index in reference tree-driver order; identity component for `UniqueGalaxyID`. int64 because percolation super-forest ranks exceed int32 |

The `forests.h5` sidecar contains one dataset, `/ForestID` (int64, length `n_forests_total`), mapping each dense `ForestIndex` to the original source-catalog forest id. Provenance and debugging only.

## Link Scope

Every link field is a **snapshot-local integer index**; no dataset stores global ids as links. The consumer must resolve each link type against the correct file:

| Link field | Points into |
|---|---|
| `Descendant` | snapshot N+1 file (validation only) |
| `FirstProgenitor` | snapshot N−1 file |
| `NextProgenitor` | snapshot N file (same file) |
| `FirstHaloInFOFgroup` | snapshot N file (same file) |
| `NextHaloInFOFgroup` | snapshot N file (same file) |

## Format Invariants

Violating any invariant makes a file invalid. Producers and consumers **abort on violation; nothing repairs**.

1. **Adjacency.** Every non-null `Descendant` link points exactly one snapshot forward, and therefore every progenitor of a snapshot-N halo lives at snapshot N−1. All halos in the final snapshot have `Descendant = −1`. `links_adjacent = 1` declares this in every file. Sources with snapshot gaps (e.g. L-Halo trees) cannot be represented in format version 1; Consistent-Trees sources are adjacent by construction because ctrees writes its own interpolated phantom halos. There is no phantom or bridge insertion anywhere in this pipeline.
2. **int32 topology bounds.** Link fields are int32; no snapshot may contain more than 2,147,483,647 halos. Producers assert this. Consumers nevertheless use 64-bit indices and counts internally.
3. **Slab ordering.** Within a file, halos appear in ascending order of the magnitude of `MostBoundID` (the original source-catalog id, whose sign may have been flipped by the flyby convention), and those magnitudes are unique within the snapshot. This makes files deterministic, reproducible, and binary-searchable by id.
4. **Identity uniqueness and density.** `(ForestIndex, HaloRankInForest)` pairs are unique across the entire dataset. `ForestIndex` values are dense over `[0, n_forests_total)` across the dataset. Within each forest, `HaloRankInForest` values are dense over `[0, forest halo count)` across all snapshots.
5. **Header consistency.** `n_halos` equals every dataset's length; `snapshot_number` matches the filename; all `SnapNum` values equal `snapshot_number`; `n_forests_total` and `max_halo_rank_in_forest` are identical across all files and match the measured data.
6. **Link validity.** Every non-null link value is a valid index in its target file (see [Link Scope](#link-scope)). FoF chains are cycle-free, terminate at −1, and every `FirstHaloInFOFgroup` names a halo whose own `FirstHaloInFOFgroup` is itself. Every non-null `FirstProgenitor` has a `Descendant` pointing back at its owner.

## Ordering Contracts

Cross-format identity — a snapshot-ordered run reproducing a tree-ordered run's galaxies exactly — depends on orderings the driver cannot derive at runtime. They are producer-owned facts carried by the format:

1. **Progenitor chain order.** For each descendant, `FirstProgenitor` is the most massive progenitor (reference tie-break: first encountered in reference order wins). The `NextProgenitor` chain is built by the reference reader's literal incremental-insertion loop (`ctrees_utils.c` `assign_mergertree_indices`): progenitors are visited in reference encounter order, and each one either replaces the current chain head when its Mvir is *strictly* greater (demoting the old head to second place) or is appended at the tail. When a mid-chain head replacement occurs (three or more progenitors), the resulting order is therefore *not* the remaining progenitors in plain encounter order — it is exactly what that loop produces. Chain order fixes workspace layout and merger processing order, so a conforming producer must replicate the loop, not a paraphrase of it.
2. **FoF chain order.** `FirstHaloInFOFgroup`/`NextHaloInFOFgroup` chains replicate the reference FoF member ordering, which fixes subhalo slice order and central selection.
3. **Forest enumeration.** Dense `ForestIndex` assignment replicates the reference run-scoped forest enumeration order (for Consistent-Trees sources: ascending forest id).
4. **Within-forest rank.** `HaloRankInForest` is the halo's index in reference tree-driver traversal order of its forest, computed after all host/flyby fix-ups.
5. **Flyby convention.** Flyby-demoted centrals carry a negated `MostBoundID`, replicating the reference reader's marker.

"Reference" throughout means the semantics of Mimic's tree-ordered Consistent-Trees ASCII reader (`src/io/tree/read_ctrees_ascii.c` and `src/io/tree/ctrees_utils.c`: `fix_flybys()`, `fix_upid()`, `assign_mergertree_indices()` and the associated sort orders). A conforming producer replicates those semantics exactly and proves it by cross-checking its output topology against that reader on a common dataset (by stable halo id, not by array index).

## Galaxy Identity Encoding

`UniqueGalaxyID = HaloRankInForest + multiplier × (ForestIndex + 1)`

The multiplier is per-simulation metadata declared in the simulation package (`simulation_info.yaml`; default 10⁹) and recorded in output provenance. It must exceed the dataset's `max_halo_rank_in_forest`, and `multiplier × (n_forests_total + 1)` must fit in int64 — both checked at startup against this format's header attributes. Because `ForestIndex` and `HaloRankInForest` are carried explicitly in reference order, snapshot-ordered and tree-ordered runs compute identical `UniqueGalaxyID`s from identical components with no runtime id mapping.

## Validation Requirements

**Producers** must verify before declaring a dataset valid: total halo count conservation against the source; every [format invariant](#format-invariants); progenitor round-trip closure (`FirstProgenitor`/`Descendant` mutual consistency); `NextProgenitor` same-file scope; FoF chain integrity; identity uniqueness/density and header bounds; `Len ≥ 0` with zero-count logged.

**The reader** must validate at open: `format_version` is supported, `links_adjacent = 1`, and header consistency (invariant 5). At slab load it must validate link ranges against the target file's `n_halos` (invariant 6's range component). Full chain-topology re-validation is a producer obligation, not a per-run cost.

## Storage Layout

- Datasets are chunked, **uncompressed**, struct-of-arrays: chunk shape `(65536,)` for 1D datasets and `(65536, 3)` for vectors. Chunked uncompressed layout is required for production data — slab reads are the hot path and compression measurably hurts them.
- Producers should write with the HDF5 latest-version file format bounds available to them; consumers must not depend on chunk boundaries, only on dataset shape and type.

## Simulation Package Integration

A simulation package shipping snapshot-HDF5 data declares:

- `simulation_info.yaml` — box size, particle mass, cosmology (matching the header attributes), the `UniqueGalaxyID` multiplier, and the snapshot list; run files select `input.tree_type: snapshot_hdf5` and `input.processing_order: snapshot_ordered`.
- `halo_properties.yaml` — the on-disk record for every `/halos` dataset, with `provides_core_role` mappings (`M_Crit200` → HaloMass, plus Descendant, FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup, SnapNum, Len). `ForestIndex` and `HaloRankInForest` are snapshot-format identity metadata, not catalog halo properties, so declaring them here is unnecessary rather than forbidden: the reader consumes them directly by dataset name (`src/io/snapshot/read_snapshot_hdf5.c`) into `struct SnapshotSlab`'s own `forest_index`/`halo_rank_in_forest` arrays regardless of whether `halo_properties.yaml` also declares them. `simulations/micro-uchuu-snapshot/halo_properties.yaml` exercises the exemption and omits both.
- An `a_list` snapshot file whose entries match the per-file `scale_factor` attributes, and a `snapshots/` link to the HDF5 files.

Field names and types in `halo_properties.yaml` must match this specification exactly; the generated reader-side code consumes those datasets by name. `ForestIndex` and `HaloRankInForest` are consumed by name directly by the reader's own schema table regardless, so a package need not declare them in `halo_properties.yaml` for the reader to read them correctly.

## Producing Snapshot-HDF5 Data

Converters live outside Mimic's run path (converter tooling is maintained under `scripts/convert/` in this repository) and perform the forest-ordered → snapshot-ordered reorganisation offline, once per source dataset. A conforming converter:

1. Reads a source whose links are adjacent by construction (Consistent-Trees output; gap-ful sources are out of scope for format version 1).
2. Applies the reference value conventions (spin normalisation, `Len` derivation, flyby/host fix-ups) exactly as the reference reader does.
3. Rewrites global-id links as snapshot-local indices with the chain orderings of the [Ordering Contracts](#ordering-contracts).
4. Runs the full producer [validation battery](#validation-requirements) and emits a conversion report (counts, measured identity bounds, validation outcomes) from which the simulation package's identity multiplier is set.

## Versioning Policy

`format_version` is a single int32 ratchet. Version 1 is this document. Readers reject files with an unrecognised version; producers stamp the version they implement. Additive changes (new optional datasets or attributes) also require a version bump — version 1 consumers are entitled to assume the exact object set specified here.

## Errata

A **correction** is an edit that changes what this document *says* without changing which files on disk conform: the reference semantics it describes were always the contract, and the previous wording described them inaccurately. Corrections do not bump `format_version` — a bump would tell every existing reader and producer that the bytes changed, which would be false, and would strand conforming version 1 data. They are recorded here instead, dated, so that anyone who implemented against the earlier wording can see exactly what moved and when. An edit that changes which files conform is not a correction: it bumps the version.

| Date | Section | Correction |
|---|---|---|
| 2026-07-24 | [Ordering Contracts](#ordering-contracts) item 1 | The `NextProgenitor` chain order was described as "the remaining progenitors in reference encounter order". That paraphrase is wrong whenever a descendant has three or more progenitors and a mid-chain head replacement occurs. The text now states the reference reader's literal incremental-insertion loop (`assign_mergertree_indices`), which is what `format_version = 1` always denoted and what both the reader and `scripts/convert/links.py` have always implemented. A producer that had implemented the paraphrase literally would have emitted non-conforming chain order; the converter's `topology-chains` cross-check compares this order directly against the reference reader. |
| 2026-08-11 | [Halo Datasets](#halo-datasets), [Simulation Package Integration](#simulation-package-integration) | `ForestIndex` and `HaloRankInForest` were described as datasets a consuming simulation package's `halo_properties.yaml` must declare, like every other `/halos` dataset. That declaration requirement was real (`simulations/micro-uchuu-snapshot/halo_properties.yaml` did declare both, and the generated `struct RawHalo` did carry them as members, prior to 2026-08-11) but was never necessary to the format: the reader's *validation* path — the dataset-set/dtype checks and the `open_run` identity scans — has always consumed both datasets by name through its own schema table (`src/io/snapshot/read_snapshot_hdf5.c`), independent of `halo_properties.yaml`. Only the *load* path depended on the declaration, materialising both values into `struct RawHalo` from the generated property list; it now reads them directly into `struct SnapshotSlab`'s own `forest_index`/`halo_rank_in_forest` arrays instead, so the declaration is no longer needed there either. The wording now states that these two identity datasets are exempt from the `halo_properties.yaml` declaration rule, consistent with `simulations/micro-uchuu-snapshot/halo_properties.yaml` no longer declaring them. Nothing on disk changed: both datasets remain required, read, and validated exactly as before, and `format_version` stays 1. |

---

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [VISION.md](../VISION.md): architectural principles and design boundaries
- [USER-GUIDE.md](../USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [DEVELOPER-GUIDE.md](../DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [STYLE-GUIDE.md](../STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
