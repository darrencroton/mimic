# Mimic Horizontal-HDF5 Format Specification

**Purpose**: Define the frozen on-disk contract for horizontal HDF5 merger-tree input — the format produced by external converters and consumed by Mimic's `horizontal_hdf5` reader and horizontal driver.

**Status**: Frozen at `format_version = 2`. Every normative statement in this document is part of the contract. Any change that alters the meaning, layout, ordering, or validation rules of files on disk requires incrementing `format_version` and updating this specification; readers must reject files whose `format_version` they do not support. Corrections that bring the wording into line with the semantics a version's `format_version` always denoted are recorded under [Errata](#errata) instead of bumping the version — see that section for the rule and the full list.

**Version 2 supersedes version 1 outright; there is no legacy-read path.** Version 1 was produced and consumed with `fix_flybys()` still live in the reference Consistent-Trees reader (`src/io/vertical/ctrees/ctrees_utils.c`): at each forest's final snapshot it collapsed every independent FoF group but the most massive into satellites of that survivor, marking the demotion by negating the demoted centrals' `MostBoundID`. This was found to be scientifically wrong — not a tolerable approximation — when it collapsed 33% of the Shin-Uchuu z=0 population into one bogus FoF group and truncated the z=0 halo mass function by ~2 dex (`docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md`). `fix_flybys()` was deleted from the reader and the converter; `MostBoundID` is therefore always positive in version 2, and every version 1 dataset is rejected outright by a version-2 reader (`HORIZONTAL_HDF5_FORMAT_VERSION` in `src/io/horizontal/read_horizontal_hdf5.c`), naming the file and the version found rather than silently reinterpreting it.

**Version 3 is also normative (2026-09-29).** It is specified in full under [Version 3](#version-3). Every section before that one specifies version 2 exactly as frozen, and where those sections say "this format" they mean version 2. Mimic's `horizontal_hdf5` reader accepts versions 2 and 3, dispatching on each file's `format_version`, and rejects every other version.

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
12. [Producing Horizontal-HDF5 Data](#producing-horizontal-hdf5-data)
13. [Versioning Policy](#versioning-policy)
14. [Errata](#errata)
15. [Version 3](#version-3)

---

## Role and Scope

Horizontal input groups halos by snapshot rather than by forest, so the working set of a run is one snapshot's halo population instead of one forest's history. This is the input format for runs declaring `input.processing_order: horizontal` with `input.tree_type: horizontal_hdf5`.

Mimic never converts between orderings internally. Horizontal-HDF5 data is produced offline by an external converter (see [Producing Horizontal-HDF5 Data](#producing-horizontal-hdf5-data)) and validated both at conversion time and again by the reader at load time. The format carries everything the horizontal driver needs to reproduce vertical results exactly — topology links, chain orderings, and galaxy-identity components are converter-owned facts recorded in the file, never reconstructed heuristically by Mimic.

This document owns the format. The reader and driver that consume it, and any converter that produces it, conform to this specification — not the other way around.

## File Set and Naming

A horizontal-HDF5 dataset consists of:

- **One HDF5 file per snapshot**: `snapshot_NNN.h5`, where `NNN` is the zero-padded snapshot index from `000` to the final snapshot, in ascending scale-factor order. Every snapshot in the run's snapshot list must have a file, including snapshots containing zero halos.
- **One run-level sidecar**: `forests.h5`, written once per dataset. Provenance only; Mimic never reads it.

Each `snapshot_NNN.h5` contains exactly two HDF5 objects: the group `/header` (scalar metadata as HDF5 attributes) and the group `/halos` (one dataset per halo field, all of length `n_halos`, stored as a struct-of-arrays).

## Header Attributes

All scalar metadata lives as HDF5 attributes on the `/header` group:

| Attribute | Type | Semantics |
|---|---|---|
| `format_version` | int32 | Contract version of this file; this specification defines version 2 |
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

All datasets live under `/halos`, each of length `n_halos` (vectors are `[n_halos, 3]`). **Dataset names and types are normative**: for every dataset except `ForestIndex` and `HaloRankInForest`, they must match what the consuming simulation package's `halo_properties.yaml` declares, so the generated `RawHalo` struct and accessors consume the file directly. `ForestIndex` and `HaloRankInForest` are horizontal-format identity metadata, not catalog halo properties (see [Simulation Package Integration](#simulation-package-integration)): the reader consumes them directly by dataset name into `struct SnapshotSlab`'s own `forest_index`/`halo_rank_in_forest` arrays, so they are exempt from the `halo_properties.yaml` declaration rule. The names below follow the established Consistent-Trees bridge contract (as in `simulations/micro-uchuu-ascii/halo_properties.yaml`).

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
| `Spin` | float32[N,3] | Specific angular momentum J/Mvir, `Mpc/h km/s` (normalisation applied by the producer; components of zero-mass halos are carried unnormalised) |
| `VelDisp` | float32[N] | Velocity dispersion, km/s |
| `Vmax` | float32[N] | Maximum circular velocity, km/s |
| `MostBoundID` | int64[N] | Source-catalog halo id (Consistent-Trees `id`). Always strictly positive: version 1's flyby-demotion sign convention (see [Versioning Policy](#versioning-policy)) is gone |
| `ForestIndex` | int64[N] | Dense run-scoped forest number in `[0, n_forests_total)`; identity component consumed directly by `UniqueGalaxyID` |
| `HaloRankInForest` | int64[N] | Within-forest halo index in reference vertical-driver order; identity component for `UniqueGalaxyID`. int64 because percolation super-forest ranks exceed int32 |

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

1. **Adjacency.** Every non-null `Descendant` link points exactly one snapshot forward, and therefore every progenitor of a snapshot-N halo lives at snapshot N−1. All halos in the final snapshot have `Descendant = −1`. `links_adjacent = 1` declares this in every file. Sources with snapshot gaps (e.g. L-Halo trees) cannot be represented in this format; Consistent-Trees sources are adjacent by construction because ctrees writes its own interpolated phantom halos. There is no phantom or bridge insertion anywhere in this pipeline.
2. **int32 topology bounds.** Link fields are int32; no snapshot may contain more than 2,147,483,647 halos. Producers assert this. Consumers nevertheless use 64-bit indices and counts internally.
3. **Slab ordering.** Within a file, halos appear in ascending order of `MostBoundID` (the original source-catalog id, always positive — see [Versioning Policy](#versioning-policy)), and those values are unique within the snapshot. This makes files deterministic, reproducible, and binary-searchable by id.
4. **Identity uniqueness and density.** `(ForestIndex, HaloRankInForest)` pairs are unique across the entire dataset. `ForestIndex` values are dense over `[0, n_forests_total)` across the dataset. Within each forest, `HaloRankInForest` values are dense over `[0, forest halo count)` across all snapshots.
5. **Header consistency.** `n_halos` equals every dataset's length; `snapshot_number` matches the filename; all `SnapNum` values equal `snapshot_number`; `n_forests_total` and `max_halo_rank_in_forest` are identical across all files and match the measured data.
6. **Link validity.** Every non-null link value is a valid index in its target file (see [Link Scope](#link-scope)). FoF chains are cycle-free, terminate at −1, and every `FirstHaloInFOFgroup` names a halo whose own `FirstHaloInFOFgroup` is itself. Every non-null `FirstProgenitor` has a `Descendant` pointing back at its owner.

## Ordering Contracts

Cross-format identity — a horizontal run reproducing a vertical run's galaxies exactly — depends on orderings the driver cannot derive at runtime. They are producer-owned facts carried by the format:

1. **Progenitor chain order.** For each descendant, `FirstProgenitor` is the most massive progenitor (reference tie-break: first encountered in reference order wins). The `NextProgenitor` chain is built by the reference reader's literal incremental-insertion loop (`ctrees_utils.c` `assign_mergertree_indices`): progenitors are visited in reference encounter order, and each one either replaces the current chain head when its Mvir is *strictly* greater (demoting the old head to second place) or is appended at the tail. When a mid-chain head replacement occurs (three or more progenitors), the resulting order is therefore *not* the remaining progenitors in plain encounter order — it is exactly what that loop produces. Chain order fixes workspace layout and merger processing order, so a conforming producer must replicate the loop, not a paraphrase of it.
2. **FoF chain order.** `FirstHaloInFOFgroup`/`NextHaloInFOFgroup` chains replicate the reference FoF member ordering, which fixes subhalo slice order and central selection.
3. **Forest enumeration.** Dense `ForestIndex` assignment replicates the reference run-scoped forest enumeration order (for Consistent-Trees sources: ascending forest id).
4. **Within-forest rank.** `HaloRankInForest` is the halo's index in reference vertical-driver traversal order of its forest, computed after all host fix-ups.

Version 1 additionally required a fifth item — a flyby convention under which flyby-demoted centrals carried a negated `MostBoundID` — that item is deleted as of version 2; see [Versioning Policy](#versioning-policy).

"Reference" throughout means the semantics of Mimic's vertical Consistent-Trees ASCII reader (`src/io/vertical/read_ctrees_ascii.c` and `src/io/vertical/ctrees/ctrees_utils.c`: `fix_upid()`, `assign_mergertree_indices()` and the associated sort orders — explicitly excluding `fix_flybys()`, which no longer exists). A conforming producer replicates those semantics exactly and proves it by cross-checking its output topology against that reader on a common dataset (by stable halo id, not by array index).

## Galaxy Identity Encoding

`UniqueGalaxyID = HaloRankInForest + multiplier × (ForestIndex + 1)`

The multiplier is per-simulation metadata declared in the simulation package (`simulation_info.yaml`; default 10⁹) and recorded in output provenance. It must exceed the dataset's `max_halo_rank_in_forest`, and `multiplier × (n_forests_total + 1)` must fit in int64 — both checked at startup against this format's header attributes. Because `ForestIndex` and `HaloRankInForest` are carried explicitly in reference order, horizontal and vertical runs compute identical `UniqueGalaxyID`s from identical components with no runtime id mapping.

## Validation Requirements

**Producers** must verify before declaring a dataset valid: total halo count conservation against the source; every [format invariant](#format-invariants); progenitor round-trip closure (`FirstProgenitor`/`Descendant` mutual consistency); `NextProgenitor` same-file scope; FoF chain integrity; identity uniqueness/density and header bounds; `Len ≥ 0` with zero-count logged.

**The reader** must validate at open: `format_version` is supported, `links_adjacent = 1`, header consistency (invariant 5), exact `scale_factor` agreement with the package's `a_list`, the identity-multiplier bounds against the measured `n_forests_total` and `max_halo_rank_in_forest`, and — for every snapshot file — agreement of the five physical header values (`box_size_mpc_h`, `particle_mass_msun_h`, `omega_matter`, `omega_lambda`, `hubble_h`) with the configured simulation package, aborting on mismatch. At slab load it must validate link ranges against the target file's `n_halos` (invariant 6's range component). Full chain-topology re-validation is a producer obligation, not a per-run cost.

## Storage Layout

- Datasets are chunked, **uncompressed**, struct-of-arrays: chunk shape `(65536,)` for 1D datasets and `(65536, 3)` for vectors. Chunked uncompressed layout is required for production data — slab reads are the hot path and compression measurably hurts them.
- Producers should write with the HDF5 latest-version file format bounds available to them; consumers must not depend on chunk boundaries, only on dataset shape and type.

## Simulation Package Integration

A simulation package shipping horizontal-HDF5 data declares:

- `simulation_info.yaml` — box size, particle mass, cosmology (matching the header attributes), the `UniqueGalaxyID` multiplier, and the snapshot list; run files select `input.tree_type: horizontal_hdf5` and `input.processing_order: horizontal`.
- `halo_properties.yaml` — the on-disk record for every catalog halo dataset (the two identity datasets optionally — see below), with `provides_core_role` mappings (`M_Crit200` → HaloMass, plus Descendant, FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup, NextHaloInFOFgroup, SnapNum, Len). `ForestIndex` and `HaloRankInForest` are horizontal-format identity metadata, not catalog halo properties, so declaring them here is unnecessary rather than forbidden: the reader consumes them directly by dataset name (`src/io/horizontal/read_horizontal_hdf5.c`) into `struct SnapshotSlab`'s own `forest_index`/`halo_rank_in_forest` arrays regardless of whether `halo_properties.yaml` also declares them. `simulations/micro-uchuu-horizontal/halo_properties.yaml` exercises the exemption and omits both.
- An `a_list` snapshot file whose entries match the per-file `scale_factor` attributes, and a `snapshots/` link to the HDF5 files.

Field names and types in `halo_properties.yaml` must match this specification exactly; the generated reader-side code consumes those datasets by name. `ForestIndex` and `HaloRankInForest` are consumed by name directly by the reader's own schema table regardless, so a package need not declare them in `halo_properties.yaml` for the reader to read them correctly.

## Producing Horizontal-HDF5 Data

Converters live outside Mimic's run path (converter tooling is maintained under `scripts/convert/` in this repository) and perform the forest-ordered → horizontal reorganisation offline, once per source dataset. A conforming converter:

1. Reads a source whose links are adjacent by construction (Consistent-Trees output; gap-ful sources are out of scope for this format).
2. Applies the reference value conventions (spin normalisation, `Len` derivation, host fix-ups) exactly as the reference reader does.
3. Rewrites global-id links as snapshot-local indices with the chain orderings of the [Ordering Contracts](#ordering-contracts).
4. Runs the full producer [validation battery](#validation-requirements) and emits a conversion report (counts, measured identity bounds, validation outcomes) from which the simulation package's identity multiplier is set.

## Versioning Policy

`format_version` is a single int32 ratchet. Readers reject files with an unrecognised version; producers stamp the version they implement. Additive changes (new optional datasets or attributes) also require a version bump — consumers of a given version are entitled to assume the exact object set that version specifies.

**Version 3.** A `format_version = 3` contract — lossless skipped-snapshot links, int64 snapshot-local indices, source-qualified row identity and an embedded payload schema — is specified in full, and is normative, under [Version 3](#version-3). It is a new version on this ratchet, not a change to version 2: nothing in it alters version 2's bytes, validation rules or the ratchet above. It was first proposed in [`MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md) and promoted into this document by decision R0-11 of [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md).

### Version 2 (2026-09-10)

This document now specifies version 2. Version 1 is superseded outright, not extended: there is no legacy-read path, and a version 1 file is rejected by a version-2 reader with an error naming the file and the version found (`HORIZONTAL_HDF5_FORMAT_VERSION` in `src/io/horizontal/read_horizontal_hdf5.c`).

**What changed.** Version 1's Ordering Contracts item 5 required the reference reader's `fix_flybys()` convention: at each forest's final snapshot, every FoF central but the most massive was demoted to a satellite of the survivor, marked by negating the demoted centrals' `MostBoundID`. This was found to be scientifically wrong, not merely a tolerable approximation: on the Shin-Uchuu production catalog it collapsed 33% of the z=0 population into one bogus FoF group and truncated the z=0 halo mass function by ~2 dex. Full diagnosis and decision record: `docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md`.

`fix_flybys()` was deleted from the reference Consistent-Trees ASCII reader (`src/io/vertical/ctrees/ctrees_utils.c`) and from the converter (`scripts/convert/fixups.py`). Concretely, relative to version 1:

- Ordering Contracts item 5 (the flyby convention) is deleted outright; there were only ever five items, now four.
- `MostBoundID` is always strictly positive — see the [Halo Datasets](#halo-datasets) and [Format Invariants](#format-invariants) (invariant 3) entries above.
- The "reference semantics" definition below names `fix_upid()` and `assign_mergertree_indices()` and explicitly excludes `fix_flybys()`, which no longer exists.
- No dataset was added, removed, or retyped, and no other ordering, invariant, or validation rule changed (`docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md`, decision D6: no new columns this round).

Every version 1 dataset — including the pre-remediation Shin-Uchuu production dataset — carries the defect this version removes and cannot be reinterpreted as version 2 data; it must be reconverted from source.

## Errata

A **correction** is an edit that changes what this document *says* without changing which files on disk conform: the reference semantics it describes were always the contract, and the previous wording described them inaccurately. Corrections do not bump `format_version` — a bump would tell every existing reader and producer that the bytes changed, which would be false, and would strand conforming version 1 data. They are recorded here instead, dated, so that anyone who implemented against the earlier wording can see exactly what moved and when. An edit that changes which files conform is not a correction: it bumps the version.

| Date | Section | Correction |
|---|---|---|
| 2026-07-24 | [Ordering Contracts](#ordering-contracts) item 1 | The `NextProgenitor` chain order was described as "the remaining progenitors in reference encounter order". That paraphrase is wrong whenever a descendant has three or more progenitors and a mid-chain head replacement occurs. The text now states the reference reader's literal incremental-insertion loop (`assign_mergertree_indices`), which is what `format_version = 1` always denoted and what both the reader and `scripts/convert/links.py` have always implemented. A producer that had implemented the paraphrase literally would have emitted non-conforming chain order; the converter's `topology-chains` cross-check compares this order directly against the reference reader. |
| 2026-08-11 | [Halo Datasets](#halo-datasets), [Simulation Package Integration](#simulation-package-integration) | `ForestIndex` and `HaloRankInForest` were described as datasets a consuming simulation package's `halo_properties.yaml` must declare, like every other `/halos` dataset. That declaration requirement was real (`simulations/micro-uchuu-horizontal/halo_properties.yaml` did declare both, and the generated `struct RawHalo` did carry them as members, prior to 2026-08-11) but was never necessary to the format: the reader's *validation* path — the dataset-set/dtype checks and the `open_run` identity scans — has always consumed both datasets by name (`src/io/horizontal/read_horizontal_hdf5.c`) — the dataset-set/dtype checks through its own schema table, the identity scans by literal dataset name — independent of `halo_properties.yaml`. Only the *load* path depended on the declaration, materialising both values into `struct RawHalo` from the generated property list; it now reads them directly into `struct SnapshotSlab`'s own `forest_index`/`halo_rank_in_forest` arrays instead, so the declaration is no longer needed there either. The wording now states that these two identity datasets are exempt from the `halo_properties.yaml` declaration rule, consistent with `simulations/micro-uchuu-horizontal/halo_properties.yaml` no longer declaring them. Nothing on disk changed: both datasets remain required, read, and validated exactly as before, and `format_version` stays 1. |
| 2026-08-12 | [Validation Requirements](#validation-requirements) | The reader's open-time validation list named only `format_version`, `links_adjacent = 1`, and header consistency (invariant 5). That understated the shipped and now load-bearing consumer contract: the reader also requires exact `scale_factor` agreement with the package's `a_list`, the identity-multiplier bounds against the measured `n_forests_total`/`max_halo_rank_in_forest`, and — added in dual-driver Phase 5 — agreement of the five physical header values (`box_size_mpc_h`, `particle_mass_msun_h`, `omega_matter`, `omega_lambda`, `hubble_h`) with the configured simulation package, checked in **every** snapshot file and fatal on mismatch (`src/io/horizontal/read_horizontal_hdf5.c`, particle mass compared as `MimicConfig.PartMass × 1e10`). A producer or package author reading only the old list would not have known that a package disagreeing with the file headers aborts the run. This is a consumer-integration statement: nothing on disk changed and `format_version` stays 1. |
| 2026-08-14 | [Halo Datasets](#halo-datasets) | `Spin` was described as "Dimensionless spin J/Mvir", which is self-contradictory: a quantity named by its units (`J/Mvir`, i.e. `(Mpc/h)(km/s)`) cannot also be dimensionless. The wording now names it correctly as specific angular momentum with units `Mpc/h km/s`, matching the `Mpc/h km/s` unit label every simulation package's `halo_properties.yaml` now declares for `Spin`. Nothing on disk changed: every producer already emitted (and every reader already consumed) the same `J/Mvir` values this document always specified; only the label describing them was wrong. `format_version` stays 1. |
| 2026-09-18 | Whole document | The processing order this format feeds was renamed from "snapshot-ordered" to **horizontal**, and its counterpart from "tree-ordered" to **vertical**, so that the name states the structure: a vertical run walks one forest's history downwards, a horizontal run walks one snapshot's whole population across. The format is now the **horizontal-HDF5** format, this document was renamed from `SNAPSHOT-HDF5-FORMAT.md`, the reader selector `input.tree_type` changed from `snapshot_hdf5` to `horizontal_hdf5`, and `input.processing_order` values changed from `snapshot_ordered`/`tree_ordered` to `horizontal`/`vertical`. **Nothing on disk changed.** Every file name (`snapshot_NNN.h5`, `forests.h5`), every `/header` attribute name, every `/halos` dataset name, every type, every ordering contract, and every validation rule is byte-for-byte as before — per-snapshot file naming remains correct because each file still holds exactly one snapshot. Existing version 2 datasets are read unchanged and `format_version` stays 2; only the configuration spelling that selects this format moved. |

## Version 3

**Status: normative since 2026-09-29**, for producers and consumers alike. Version 3 is the lossless general contract: it represents skipped-snapshot links, snapshot populations above `INT32_MAX`, non-Consistent-Trees sources and declaratively selected extra fields, none of which version 2 can represent. Its producer contract has been in force since Gate G1 (2026-09-23; [`MIMIC-V3-CONSUMER-DESIGN-REVIEW.md`](MIMIC-V3-CONSUMER-DESIGN-REVIEW.md)), when `scripts/convert/convert_trees.py` began emitting it by default. It was first written as a separate draft, which was moved here, as a normative section, once Mimic's reader and driver consumed it and the real-data parity gate had passed (decision R0-11 of [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md)). [`HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md`](HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md) is kept only as a pointer that maps its old section names onto the sections below.

**Version 2 is unaffected.** Every section above this one specifies version 2, and nothing here alters version 2's bytes, ordering, invariants, validation rules or errata, or loosens them. Every relaxation below applies **only to files declaring `format_version = 3`**.

### V3 Role and Scope

Horizontal input groups halos by snapshot rather than by forest, so the working set of a run is one snapshot's halo population instead of one forest's history. Version 3 keeps that structure and drops three assumptions version 2 inherited from its single Consistent-Trees ASCII source:

- that every non-null `Descendant` link spans exactly one snapshot;
- that `MostBoundID` is a unique, positive, sortable row key;
- that the payload's units and precision are fixed by the format rather than declared by the producer.

Each of those is false for at least one source this format must now carry. A read-only scan of the eight local mini-Millennium L-Halo files found **29,291 descendant links spanning more than one snapshot, maximum span 2** — so an adjacency-only format cannot represent even mini-Millennium losslessly. L-Halo `MostBoundID` is a signed particle identifier with no uniqueness guarantee. L-Halo mass is float32 in `1e10 Msun/h` while Consistent-Trees mass is `Msun/h`; converting one into the other and back would destroy bit parity.

**Lossless means lossless.** Version 3 exists so that conversion is a change of layout, not of content. It does not permit dropping a halo, rejecting a valid link, inventing an intermediate halo, or closing a gap. In particular:

- **No phantom insertion.** Nothing in this pipeline creates a halo that the source does not contain. Consistent-Trees writes its own interpolated phantoms and those are carried through as source halos like any other; a gapped source is *not* repaired by manufacturing the missing generations.
- **No gap removal.** A valid forward gap is recorded as a gap. Collapsing `A(snap 0) → D(snap 2)` into two adjacent edges, or discarding it because a consumer cannot follow it, would be a silent scientific change.

Version 3 is produced offline by the converter under `scripts/convert/`, exactly as version 2 is, validated at conversion time by an independent producer validator, and validated again by Mimic's `horizontal_hdf5` reader when a run opens it (see [V3 Validation Requirements](#v3-validation-requirements)).

### What Version 3 Changes

Relative to version 2, and **only** for files declaring `format_version = 3`:

| Area | Version 2 | Version 3 |
|---|---|---|
| Link width | int32 snapshot-local indices | int64 snapshot-local indices |
| Link span | adjacency required; `links_adjacent` always 1 | gaps allowed; `links_adjacent` is measured, 0 or 1 |
| Target snapshot | implied (N±1) | explicit: three int32 target-snapshot columns |
| Row order | ascending `MostBoundID` | ascending `SourceHaloID` |
| `MostBoundID` | unique, strictly positive | source catalog data; signed, duplicates legal |
| Row identity | `(ForestIndex, HaloRankInForest)` | adds `SourceHaloID`; identity is source-relative |
| Payload units | fixed by the format | declared by the producer in `/schema` |
| Object set | `/header`, `/halos` | `/header`, `/halos`, `/schema` |
| Extra fields | none | declaratively selected, declared in `/schema` |

Version 2's adjacency invariant, `MostBoundID` ordering and fixed-unit assumptions are **replaced for version 3 and left exactly as they are for version 2**. A version 2 file that satisfied those rules yesterday satisfies them today; a version 2 producer or validator must not be loosened to accommodate anything below.

**Version 1 remains rejected outright**, for the reason recorded in the [Versioning Policy](#versioning-policy) above: version 1 data was produced with the scientifically incorrect `fix_flybys()` operation live. Version 3 changes nothing about that. A version 1 dataset must be reconverted from source.

**A dataset never mixes versions.** Every file of one dataset declares the same `format_version`.

### V3 File Set and Naming

A version 3 dataset consists of:

- **One HDF5 file per snapshot in the package's `a_list`**: `snapshot_NNN.h5`, zero-padded, in ascending scale-factor order. Every snapshot has a file, **including snapshots containing zero halos** — an empty snapshot is a real, valid file with `n_halos = 0` and zero-length datasets, not an absent one. Gapped sources make empty snapshots ordinary rather than exotic.
- **One run-level sidecar**: `forests.h5`, written once per dataset. Provenance only; see [Forest Sidecar](#v3-forest-sidecar).

Each `snapshot_NNN.h5` contains **exactly three HDF5 objects**: the group `/header` (scalar metadata as attributes), the group `/halos` (one dataset per field, struct-of-arrays) and the group `/schema` (the producer's payload declarations). No other root object is permitted.

### V3 Header Attributes

All scalar metadata lives as attributes on the `/header` group. The version 2 attributes keep their names, types, units and meaning:

| Attribute | Type | Semantics |
|---|---|---|
| `format_version` | int32 | Contract version; this document defines version 3 |
| `links_adjacent` | int32 | `0` or `1`, **measured across the entire dataset over `Descendant` links only**, and identical in every file. `1` asserts that every non-null `Descendant` link in the whole dataset targets the very next snapshot |
| `scale_factor` | float64 | Scale factor *a* of this snapshot |
| `snapshot_number` | int32 | Snapshot index; must equal the `NNN` in the filename |
| `n_halos` | int64 | Number of halos in this file; equals the length of every `/halos` dataset |
| `n_forests_total` | int64 | Run-scoped total forest count; identical in every file |
| `max_halo_rank_in_forest` | int64 | Run-scoped maximum `HaloRankInForest`; identical in every file |
| `box_size_mpc_h` | float64 | Simulation box size, Mpc/h comoving |
| `particle_mass_msun_h` | float64 | Simulation particle mass, Msun/h |
| `omega_matter` | float64 | Ωm |
| `omega_lambda` | float64 | ΩΛ |
| `hubble_h` | float64 | Dimensionless Hubble parameter *h* |

Version 3 adds exactly two:

| Attribute | Type | Semantics |
|---|---|---|
| `source_format` | fixed-length ASCII, 32 bytes | The adapter that produced this dataset: `consistent_trees_ascii`, `consistent_trees_hdf5` or `lhalo_binary` |
| `column_mapping_sha256` | fixed-length ASCII, 64 bytes | Lowercase hex SHA-256 of the canonical mapping schema (see below), identical in every file |

`links_adjacent` is a **measurement of the whole dataset**, not of one file. A dataset whose every `Descendant` link happens to be adjacent may declare `1`; a dataset with a single gap anywhere declares `0` in every one of its files. It is not a request, a hint or a per-file property, and a producer may not stamp `1` on a file whose dataset contains a gap elsewhere.

**It ranges over `Descendant` links only** — and therefore over `FirstProgenitor`, which is their inverse. This is version 2's own scope carried forward unchanged, not a new rule: v2 defines adjacency over `Descendant` links, and "gap" throughout this format's history means a gapped descendant edge (mini-Millennium's measured 29,291 of them). A producer must **not** read it as "all five link types differ by exactly one snapshot", which would stamp `0` on essentially every real dataset: `NextProgenitor` is descendant-relative and carries no direction constraint against its owner at all (see invariant 2), and on real mini-Millennium data 92.2% of its links sit at the *same* snapshot as their owner — the ordinary case, not an edge case. The FoF links are same-snapshot by definition and say nothing about adjacency either.

`column_mapping_sha256` is the digest computed by `scripts/convert/column_schema.py` over the canonical serialization of the **declared mapping profile**. It covers exactly these, and nothing else:

- the profile's `schema_version` and `source_format`;
- the normalised alias list declared for every required role;
- for every payload field: `name`, `type`, `units`, `h_convention` and `description`;
- for every selected extra: `name`, `sources` (each source's field and, where present, its component index), `type`, `units`, `h_convention` and `description`;
- for a binary source, the whole layout: `byte_order`, `itemsize`, and for each entry its `name`, `type`, `units`, `offset`, `itemsize`, `n_components`, `numpy_dtype` and whether it is selected.

`description` is in the digest deliberately, for both payload fields and extras: it is the only record of what a column *means*, and two datasets whose columns mean different things should not share an identity. The corollary is that editing a description is a schema change, not a comment.

The digest identifies the profile, **not any one file's concrete resolution** — which alias actually matched in a given file is deliberately outside it, so that one frozen profile yields one digest across files that legitimately resolve differently (a package carrying `Snap_num` and one carrying `Snap_idx` share a digest, as they must). Presentation — comments, YAML key order, alias order, extra-definition order — is normalised away and does not move the digest; anything that changes how a value is read, typed, named or labelled does, **including two types that share a width**. Details: [`scripts/convert/profiles/README.md`](../../scripts/convert/profiles/README.md).

### V3 Halo Datasets

All datasets live under `/halos`, each of length `n_halos`; vectors are `[n_halos, 3]`.

#### V3 Topology

| Dataset | Type | Semantics |
|---|---|---|
| `Descendant` | int64[N] | Row index **in the `DescendantSnapshot` file** of this halo's descendant; −1 if none |
| `FirstProgenitor` | int64[N] | Row index in the `FirstProgenitorSnapshot` file of the main progenitor; −1 if none |
| `NextProgenitor` | int64[N] | Row index in the `NextProgenitorSnapshot` file of the next sibling progenitor; −1 if no next sibling |
| `FirstHaloInFOFgroup` | int64[N] | Row index **in this snapshot** of the FoF central; self-index for a central. **Never −1** |
| `NextHaloInFOFgroup` | int64[N] | Row index in this snapshot of the next FoF-group member; −1 if last |
| `DescendantSnapshot` | int32[N] | Snapshot number the `Descendant` index refers to; −1 **if and only if** `Descendant` is −1 |
| `FirstProgenitorSnapshot` | int32[N] | Snapshot number the `FirstProgenitor` index refers to; −1 **if and only if** `FirstProgenitor` is −1 |
| `NextProgenitorSnapshot` | int32[N] | Snapshot number the `NextProgenitor` index refers to; −1 **if and only if** `NextProgenitor` is −1 |

Direction and scope:

- `Descendant` points **forward** in time: `DescendantSnapshot > SnapNum`.
- `FirstProgenitor` points **backward**: `FirstProgenitorSnapshot < SnapNum`.
- `NextProgenitor` is **descendant-relative, not owner-relative**. It targets another progenitor of the owner's own descendant, and **may target a different earlier snapshot than its sibling does**. Its target's snapshot is unconstrained relative to the *owner* — it may be earlier, the same, or later — because siblings in one progenitor chain need not share a snapshot. What every chain member must share is the descendant: every halo in a `NextProgenitor` chain names the same `Descendant`, in the same `DescendantSnapshot`, and therefore every one of them (the owner included) lies strictly before that shared `DescendantSnapshot`.
- FoF links stay **within the current snapshot**. `FirstHaloInFOFgroup` is never null and self-references for a central.

Only −1 is null. Any other negative value is structurally invalid, not "no link".

#### V3 Identity

| Dataset | Type | Semantics |
|---|---|---|
| `SourceHaloID` | int64[N] | Positive, globally unique converter row key; see [Source Identity](#v3-source-identity) |
| `ForestIndex` | int64[N] | Source-relative forest identity |
| `HaloRankInForest` | int64[N] | Source-relative within-forest row identity |

#### V3 Payload

| Dataset | Type | Semantics |
|---|---|---|
| `SnapNum` | int32[N] | Snapshot index; every value equals the header `snapshot_number` |
| `Len` | int32[N] | Particle count. Never negative. Zero is legal |
| `M_Crit200` | producer-declared | Halo mass, in the producer's declared precision and units |
| `Pos` | producer-declared[N,3] | Position |
| `Vel` | producer-declared[N,3] | Peculiar velocity |
| `Spin` | producer-declared[N,3] | Specific angular momentum |
| `VelDisp` | producer-declared | Velocity dispersion |
| `Vmax` | producer-declared | Maximum circular velocity |
| `MostBoundID` | int64[N] | The **source catalog's own** halo/particle identifier, carried through as signed data |
| *(selected extras)* | producer-declared | Any declaratively selected extra field |

`MostBoundID` carries **no uniqueness and no positivity requirement** in the general format. L-Halo's most-bound particle identifier is signed and may repeat; Consistent-Trees' `id` happens to be positive and unique but the format does not depend on that. It is data, never a key. The key is `SourceHaloID`.

Each payload field's precision and units are the **adapter's native ones**, declared in `/schema` and not converted by the producer. For the three shipped adapters today:

| Field | `lhalo_binary` | `consistent_trees_ascii` / `consistent_trees_hdf5` |
|---|---|---|
| `M_Crit200` | float32, `1e10 Msun/h` | float32, `Msun/h` |
| `Pos` | float32[3], `Mpc/h` | float32[3], `Mpc/h` |
| `Vel` | float32[3], `km/s` | float32[3], `km/s` |
| `Spin` | float32[3], `Mpc/h km/s`, already specific | float32[3], `Mpc/h km/s`, producer-normalised J/Mvir |
| `VelDisp`, `Vmax` | float32, `km/s` | float32, `km/s` |
| `Len` | int32, supplied by the source | int32, derived as `round(Mvir × 1e-10 / particle_mass)` |

L-Halo mass in particular **stays float32 in `1e10 Msun/h`**. Converting it into float32 `Msun/h` and back would not round-trip, and bit parity against the vertical L-Halo reader is the acceptance evidence this format exists to support.

### V3 Schema Group

`/schema` records **the emitted file's own interpretation of its payload**. It does not replace, override or duplicate a consuming simulation package's compiled `halo_properties.yaml`; it is the producer stating what it wrote, so a consumer can check that what it expects and what it got agree.

- One subgroup per physical/catalog payload field — **including `Len`, `SnapNum` and `MostBoundID`** — plus one per selected extra.
- Each subgroup carries **exactly** four scalar variable-length UTF-8 string attributes: `type`, `units`, `h_convention`, `description`. No others, and **no children**.
- `type` is one of six values and `h_convention` is `carried`, `free` or `none`. These are the property generator's own vocabularies (`scripts/generate_properties.py`), so anything declarable here is declarable by a simulation package. Each `type` fixes the dataset's storage exactly:

  | `type` | Dataset dtype | Dataset shape | Bytes per row |
  |---|---|---|---|
  | `int` | int32 | `[N]` | 4 |
  | `long long` | int64 | `[N]` | 8 |
  | `float` | float32 | `[N]` | 4 |
  | `double` | float64 | `[N]` | 8 |
  | `vec3_int` | int32 | `[N, 3]` | 12 |
  | `vec3_float` | float32 | `[N, 3]` | 12 |

  All are explicit little-endian (see [Storage Layout](#v3-storage-layout)). A declaration whose `type` disagrees with the dataset's actual dtype or shape makes the file invalid.
- **Topology and the three identity arrays are not redeclared.** They are governed by the fixed format tables above. Declaring them in `/schema` would create two sources of truth for one contract.
- A missing declaration, an extra declaration, or a declaration whose `type` disagrees with the dataset's actual dtype or shape makes the file invalid.
- `/schema`, `source_format` and `column_mapping_sha256` are **identical across every snapshot file** of a dataset.
- No object under `/halos` or `/schema` may be an HDF5 external or soft link. Every dataset is physically present in the file that names it.

### V3 Link Scope

Version 2 could name a link's target file from the link type alone. Version 3 cannot, and says so explicitly:

| Link field | Points into |
|---|---|
| `Descendant` | the file for `DescendantSnapshot` |
| `FirstProgenitor` | the file for `FirstProgenitorSnapshot` |
| `NextProgenitor` | the file for `NextProgenitorSnapshot` |
| `FirstHaloInFOFgroup` | this snapshot's file |
| `NextHaloInFOFgroup` | this snapshot's file |

A consumer resolves every non-FoF link through its companion snapshot column. Assuming N±1 is a version 2 reading and is wrong for version 3 whenever `links_adjacent = 0`.

### V3 Format Invariants

Violating any invariant makes a file invalid, and **nothing repairs**. The producer validates every invariant and aborts on violation; a consumer aborts on any violation it detects. Which invariants the producer validates and which the runtime enforces, at open and at slab load, is set out in [V3 Validation Requirements](#v3-validation-requirements).

1. **Forward descendants, backward progenitors.** Every non-null `Descendant` targets a strictly later snapshot, and every non-null `FirstProgenitor` targets a strictly earlier one. A valid forward gap is **not** malformed.
2. **`NextProgenitor` is constrained relative to the shared descendant, not to its owner.** Every non-null `NextProgenitor` targets another progenitor of the owner's own `Descendant`/`DescendantSnapshot`. That target's snapshot is unconstrained relative to the owner — earlier, the same, or later are all valid — but, like every member of the chain including the owner, it is strictly earlier than the shared `DescendantSnapshot`, which invariant 1 already guarantees. **A rule stated relative to the owner would be wrong**: a read-only scan of all eight real mini-Millennium L-Halo files measured 51,270 non-null `NextProgenitor` links, of which 2,980 (5.8%) target an earlier snapshot than their owner, 47,291 (92.2%) the same snapshot, and 999 (1.9%) a *later* one. The same-snapshot majority is the ordinary adjacent case that version 2 already carried, so an owner-relative constraint would reject conforming input rather than catch an edge case. All 51,270 satisfy this descendant-relative rule, and all 51,270 name the same descendant as their owner.
3. **Snapshot-column biconditional.** Each of the three target-snapshot columns is −1 if and only if its companion index is −1, and otherwise names a snapshot that exists in the dataset.
4. **int64 topology bounds.** Link fields are int64. A snapshot may contain more than `INT32_MAX` halos. Producers must not narrow indices to int32 anywhere in the pipeline.
5. **Slab ordering.** Within a file, rows are in ascending `SourceHaloID`. Those values are unique across the entire dataset, not merely within a snapshot.
6. **Identity uniqueness and density.** `SourceHaloID` is positive and globally unique. `(ForestIndex, HaloRankInForest)` pairs are unique and dense by the adapter's declared source order.
7. **Header consistency.** `n_halos` equals every dataset's length; `snapshot_number` matches the filename; every `SnapNum` equals `snapshot_number`; `n_forests_total`, `max_halo_rank_in_forest`, `links_adjacent`, `source_format` and `column_mapping_sha256` are identical across all files and match the measured data.
8. **Link validity.** Every non-null link is a valid row index in its resolved target file. FoF chains are cycle-free, terminate at −1, and every `FirstHaloInFOFgroup` names a halo whose own `FirstHaloInFOFgroup` is itself. Every non-null `FirstProgenitor` has a `Descendant` pointing back at its owner, and every member of a `NextProgenitor` chain names that same descendant.
9. **No negative `Len`.** Zero is legal.
10. **Object set.** Exactly `/header`, `/halos`, `/schema`; exactly the declared datasets; no external or soft links.

Note what is **not** an invariant in version 3: adjacency, `MostBoundID` uniqueness, `MostBoundID` positivity, and fixed payload units. All four remain invariants of version 2.

### V3 Ordering Contracts

Cross-format identity depends on orderings a consumer cannot derive at runtime. They are producer-owned facts carried by the format.

1. **Chain order is the source's.** For an already-linked source (L-Halo binary, forests-HDF5), the producer preserves the source's stored `FirstProgenitor`/`NextProgenitor`/FoF chains exactly, in their stored order. It does not re-derive them by mass, and does not apply another format's tie-break rules.
2. **Consistent-Trees keeps its reference reconstruction.** For Consistent-Trees ASCII, chain order remains what the reference reader's literal incremental-insertion loop produces (`ctrees_utils.c` `assign_mergertree_indices`), with `fix_upid()` host fix-ups and without `fix_flybys()`, exactly as version 2 requires.
3. **Row order is a storage order, not a priority.** Rows are sorted by ascending `SourceHaloID` within a snapshot. That is a deterministic, reproducible storage order derived from the adapter's declared source order. It is **not** progenitor priority, not mass order and not a semantic reordering: output topology is *remapped* to the new row positions, never reordered in meaning.
4. **Forest enumeration and within-forest rank follow the selected source representation.** See [Source Identity](#v3-source-identity).

Version 2 sorted rows by `MostBoundID`. Version 3 deliberately does not, because duplicate and signed particle identifiers are not valid general remapping keys. This is a version 3 rule; version 2's ordering contract is untouched.

### V3 Source Identity

`SourceHaloID` is assigned by **prefix-summing source halo counts over the adapter's declared total order** — ascending source file ordinal, then ascending within-file unit ordinal (an L-Halo tree, a forests-HDF5 `ForestInfo` row, or an ASCII forest) — starting at 1 so every value is positive. Assignment aborts on int64 overflow rather than wrapping.

Three consequences matter:

- **It is not `MostBoundID`,** and it never replaces a physical particle or catalog identifier. Both are carried; they answer different questions.
- **It inverts exactly** back to `(source_file_ordinal, unit_ordinal, row_ordinal)`, because the ids are a contiguous prefix sum. That is what independent comparison tooling uses to compare a converted row against its source row without sharing the converter's remapping code.
- **It depends on the whole inventory.** Identical leading subsets keep their prefix identities; changing an earlier inventory member changes later ids. A sampled conversion must retain the parent inventory's prefix counts and original unit ordinals rather than compacting them, or its identities cannot be compared against an unsampled reference.

`ForestIndex`, `HaloRankInForest` and the sidecar `ForestID` retain the **selected source representation's** identity convention:

| Source format | `ForestIndex` | `HaloRankInForest` | `ForestID` |
|---|---|---|---|
| `lhalo_binary` | file-prefix tree number | original within-tree row index | dense run forest number |
| `consistent_trees_hdf5` | file-prefix `ForestInfo` row number | original within-forest row index | source `ForestID`, even if only file-local unique |
| `consistent_trees_ascii` | dense forest-id enumeration | post-fix-up reference ordering | original source forest id |

**Identity is relative to the representation, and equal identities across representations are not promised.** An L-Halo packaging and a Consistent-Trees forests-HDF5 packaging of the same simulation can enumerate different forest sets, so a `UniqueGalaxyID` computed from one is not expected to equal one computed from the other. Comparisons are made against the *selected* format's own vertical interpretation, never against a different source format assumed equivalent.

### V3 Galaxy Identity Encoding

Unchanged from version 2:

`UniqueGalaxyID = HaloRankInForest + multiplier × (ForestIndex + 1)`

The multiplier is per-simulation metadata declared in the simulation package and recorded in output provenance. It must exceed the dataset's `max_halo_rank_in_forest`, and `multiplier × (n_forests_total + 1)` must fit in int64 — both checked at startup against this format's header attributes.

### V3 Forest Sidecar

`forests.h5` carries source provenance for every forest, as three int64 datasets each of length `n_forests_total`, **all three at the file root**, with no enclosing group:

| Dataset | Type | Semantics |
|---|---|---|
| `ForestID` | int64[n_forests_total] | The source forest identifier, per the table in [Source Identity](#v3-source-identity) |
| `SourceFileOrdinal` | int64[n_forests_total] | Inventory file ordinal owning the forest |
| `SourceUnitOrdinal` | int64[n_forests_total] | Within-file unit ordinal of the forest |

The object paths are exactly `/ForestID`, `/SourceFileOrdinal` and `/SourceUnitOrdinal`. `forests.h5` contains those three objects and nothing else — no `/header`, no group, no attributes. This keeps `/ForestID` at the same path version 2 puts it, so a tool that reads a version 2 sidecar's `/ForestID` reads a version 3 one's unchanged; version 3 only adds two datasets beside it.

For a Consistent-Trees ASCII forest that **spans files**, both ordinals are −1 and `ForestID` carries the original forest id; the conversion manifest retains the full source membership, which a three-column sidecar cannot express. For `lhalo_binary`, `ForestID` is the dense run forest number and the two ordinals disambiguate it. For `consistent_trees_hdf5`, the source `ForestID` is retained even where it is only unique within a file — the ordinals make the pair unique.

Mimic never reads this sidecar. It exists for provenance, debugging and independent comparison.

### V3 Validation Requirements

**Producers** must verify before declaring a dataset valid:

- total halo count conservation against the source inventory;
- every [format invariant](#v3-format-invariants);
- progenitor round-trip closure across snapshots, resolved through the target-snapshot columns rather than assumed adjacent;
- `NextProgenitor` chain membership: every member names the same `Descendant`/`DescendantSnapshot` as its owner, and every member's own snapshot is strictly earlier than that shared `DescendantSnapshot`. Checking the target's snapshot against the *owner's* snapshot instead is the mistake to avoid: on real data the target is most often at the same snapshot as its owner and is sometimes later, and both are valid;
- FoF chain integrity within each snapshot;
- `SourceHaloID` positivity, global uniqueness and ascending row order;
- identity uniqueness and density, and the header bounds;
- `/schema` agreement with the actual dataset dtypes and shapes, and with the canonical mapping digest;
- `Len ≥ 0`, with zero counts logged.

Validation uses **bounded reads** and handles empty arrays. A validator must not require a whole-snapshot resident array to check a snapshot above `INT32_MAX` rows, and must behave correctly on a zero-halo file rather than skipping it.

**A consumer** must validate at open, at minimum: `format_version` is supported; header consistency; `/schema` agreement with what it intends to read; `scale_factor` agreement with the package's `a_list`; identity-multiplier bounds; and agreement of the five physical header values with the configured simulation package. At slab load it must validate link ranges against the resolved target file's `n_halos`.

Mimic's `horizontal_hdf5` reader (`src/io/horizontal/read_horizontal_hdf5.c`) reads both versions, dispatching on each file's `format_version`; the version 2 path is unchanged. For version 3 it enforces the checks below and aborts on any violation it detects, naming the file, object and value. It does not re-check every invariant; the producer-only obligations are listed after them.

- **At open**, for every file: the object set `/header`, `/halos`, `/schema` with no soft or external link anywhere under the root, `/halos` or `/schema`; the header attribute set and dtypes; `links_adjacent` of 0 or 1; header consistency across files, including `source_format`, `column_mapping_sha256` and an identical `/schema`; exact `scale_factor` agreement with the package's `a_list`; the five physical header values against the package; the identity-multiplier bounds; `n_halos` as int64 with no int32 ceiling; and, by bounded scans, every `SnapNum` equal to `snapshot_number`, every `ForestIndex` in `[0, n_forests_total)` with measured maximum `n_forests_total − 1`, and the measured maximum `HaloRankInForest` equal to `max_halo_rank_in_forest`.
- **`/schema` against the package**, a one-way rule: every field the consuming package declares in `halo_properties.yaml` (the core payload it consumes plus any extra it declares) must be declared in `/schema` and present in `/halos` with the same `type`, `units` and `h_convention` as the package's compiled declaration, and a matching dataset dtype and shape. Topology and identity fields are checked against the fixed tables above instead. A `/schema` field the package does not declare is checked for internal consistency (a valid vocabulary entry and a dataset matching its own declared `type`) and is not materialised; being undeclared is never itself an abort.
- **At slab load**, once per snapshot as the driver loads it: only −1 is null; every non-null link is a valid row of the file its target-snapshot column names, every `-1`-iff-`-1` biconditional holds, and the direction rules of invariants 1 and 2 hold. The three target-snapshot columns and `SourceHaloID` are loaded into reader-owned slab arrays; the five links fill the generated `struct RawHalo` fields, which a consuming package declares `long long`.

Scans are bounded hyperslab reads: nothing is allocated in proportion to a snapshot's `n_halos` except the slab itself.

**Producer-only obligations.** These invariants are validated by the producer (the converter's version 3 battery, `scripts/convert/validate_v3.py`) and are **not** re-checked by the runtime: `SourceHaloID` positivity, global uniqueness and ascending row order (invariants 5 and 6); `(ForestIndex, HaloRankInForest)` uniqueness and density (invariant 6, beyond the bounds scanned at open); `Len ≥ 0` (invariant 9); and chain topology — FoF cycle-freedom, termination and central self-reference, progenitor round-trip closure, and `NextProgenitor` chain membership (invariants 2 and 8, beyond the direction and range checks at load) — plus count conservation and the mapping digest's agreement with the profile. A dataset violating one of them is invalid even though the reader may not detect it; that is why only datasets that passed the producer battery are consumed.

### V3 Storage Layout

- Datasets are **explicit little-endian**, **uncompressed** and chunked, struct-of-arrays: chunk shape `(65536,)` for scalar datasets and `(65536, 3)` for vectors. Explicit endianness is a contract, not a host artefact: a file written on one architecture is read identically on another.
- Uncompressed is deliberate and unchanged from version 2 — slab reads are the hot path and compression measurably hurts them. Chunking is what makes a bounded row-range read possible without touching neighbouring rows, which is how a snapshot above `INT32_MAX` can be read at bounded cost.
- A zero-halo snapshot has zero-length datasets of the correct type and shape, not absent ones.
- Producers should write with the HDF5 latest-version file format bounds available to them; consumers must not depend on chunk boundaries, only on dataset shape and type.

### V3 Runtime Support

Mimic reads and runs version 3 (from 2026-09-29). Being able to read the format is not the same as having validated a route: **a route is supported only where a recorded parity gate has passed**, comparing the horizontal version 3 run bit for bit, per `UniqueGalaxyID`, against the vertical reader of the **same source format over the same source files**. The evidence is [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md).

| Route (simulation package) | Source format | Data | Gated models and timestep schemes | Evidence |
|---|---|---|---|---|
| `mini-millennium-horizontal` | `lhalo_binary` | complete real data, gapped (29,291 gapped `Descendant` links) | `halos-only` and `sage16`, fixed and dynamic | acceptance record §1–§8 (Slice 6) |
| `micro-uchuu-lhalo-horizontal` | `lhalo_binary` | complete real data, adjacent | `halos-only`, fixed and dynamic | acceptance record §9–§13 (Slice 7) |
| `micro-uchuu-hdf5-horizontal` | `consistent_trees_hdf5` | complete real data, adjacent | `halos-only`, fixed and dynamic | acceptance record §9–§13 (Slice 7) |
| `millennium-horizontal` | `lhalo_binary` | **sampled subset**: `trees_063.0`–`.15` only, gapped | `halos-only`, fixed and dynamic, on files 0–15 only | acceptance record §9–§14 (Slice 7) |
| `mini-uchuu-horizontal` | `lhalo_binary` | **sampled subset**: `Uchuu400_Planck_lhalo_binary.0`–`.15` only, adjacent | `halos-only`, fixed and dynamic, on files 0–15 only | acceptance record §9–§14 (Slice 7) |

No other route, model or file range is claimed. In particular:

- **No cross-source-format identity is promised.** Each route is compared against its own source format's vertical reader, never against another source format assumed equivalent (see [V3 Source Identity](#v3-source-identity)).
- **Millennium and mini-Uchuu are sampled.** Nothing is claimed for their whole simulations; that needs the remaining source files (Millennium 16–511, mini-Uchuu 16–127).
- **`sage16` is gated only on mini-Millennium.**
- **Full Uchuu is not runnable.** Its largest snapshot cannot be held with whole slabs resident on any host this project has; running it needs chunked slab streaming ([`MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`](MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md)), a capability Mimic does not implement. It has no version 3 package.

The schema rule is one-way: every field a consuming simulation package declares must match the files' `/schema`, while a `/schema` field the package does not declare is validated and ignored. Because the declared payload's units and precision are the source format's own, there is still **one simulation package per (simulation, source format)** (the shipped version 3 packages happen to declare every payload field their converter profile selects): an L-Halo and a forests-HDF5 conversion of the same simulation need two packages, because their `M_Crit200` units differ (`1e10 Msun/h` against `Msun/h`). Gapped datasets make the horizontal driver retain more than two snapshot generations at once; the retention model and its optional memory ceiling are described in the [User Guide](../USER-GUIDE.md) and [Developer Guide](../DEVELOPER-GUIDE.md).

[`MIMIC-V3-CONSUMER-DESIGN-REVIEW.md`](MIMIC-V3-CONSUMER-DESIGN-REVIEW.md) traces each field, unit, ordering and qualified link through the generated input view, gap-state ownership and bounded slab access, with a worked mixed-gap graph. It was approved at Gate G1 on 2026-09-23. [`MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`](MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md) is the plan that implemented the consumer.

### V3 Versioning Policy

`format_version` is a single int32 ratchet. Readers reject files with an unrecognised version; producers stamp the version they implement.

- **Changing an envelope rule requires another format bump.** The object set, the header attribute set, the fixed topology and identity tables, the ordering contracts and the invariants are the envelope.
- **Adding a new conforming declared field does not.** A selected extra is declared in `/schema`, validated like any other payload field, and is exactly what the envelope already provides for. This is the one respect in which version 3 is deliberately more additive than version 2, whose policy required a bump for any new dataset — because version 2 had no schema group in which to declare one.
- **Version 1 is rejected**, permanently, for the reason given in the [Versioning Policy](#versioning-policy) above.
- **A dataset never mixes versions.**

---

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [VISION.md](../VISION.md): architectural principles and design boundaries
- [USER-GUIDE.md](../USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [DEVELOPER-GUIDE.md](../DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [STYLE-GUIDE.md](../STYLE-GUIDE.md): naming, comments, documentation, metadata, tests, and review conventions
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes
- [MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md](MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md): the parity evidence behind every version 3 runtime route
- [HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md](HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md): the former version 3 draft, now a pointer into [Version 3](#version-3)
