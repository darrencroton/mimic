# Mimic Horizontal-HDF5 Format — Version 3 (DRAFT)

**Purpose**: Define the proposed lossless general on-disk contract for horizontal HDF5 merger-tree input — one that can represent skipped-snapshot links, snapshot populations above `INT32_MAX`, non-Consistent-Trees sources and declaratively selected extra fields, none of which version 2 can represent.

> **Status: DRAFT. Not normative. Nothing in this document is a contract yet.**
>
> This is the producer contract proposed by [`MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md) (contract C3), published here as a draft so it can be reviewed before anything is built against it. It becomes normative only when the owner clears **Gate G1**, the v3 consumer-design approval gate, by adding a dated `**APPROVED <date> by owner**` line to [`MIMIC-V3-CONSUMER-DESIGN-REVIEW.md`](MIMIC-V3-CONSUMER-DESIGN-REVIEW.md) and committing it. Until then no later slice may treat this file as normative, no producer may emit version 3 data, and this specification may still change.
>
> **Mimic cannot read version 3 today, and this document does not claim otherwise.** The reader rejects unsupported versions (`HORIZONTAL_HDF5_FORMAT_VERSION` in `src/io/horizontal/read_horizontal_hdf5.c`), its fixed dataset table stores links as int32, the horizontal driver retains exactly two snapshot generations (`src/core/horizontal_driver.c`), and it refuses a slab above `INT_MAX`. Reader and driver support for gapped links and wide indices is a separate architectural project, not a hidden part of this format. See [Runtime support status](#runtime-support-status).
>
> **Version 2 is unaffected.** [`HORIZONTAL-HDF5-FORMAT.md`](HORIZONTAL-HDF5-FORMAT.md) remains the frozen, normative specification of version 2. Nothing here alters version 2's bytes, ordering, invariants or validation rules, and nothing here loosens them. Every relaxation below applies **only to version 3**.

---

## Table of Contents

1. [Role and Scope](#role-and-scope)
2. [What Version 3 Changes](#what-version-3-changes)
3. [File Set and Naming](#file-set-and-naming)
4. [Header Attributes](#header-attributes)
5. [Halo Datasets](#halo-datasets)
6. [The Schema Group](#the-schema-group)
7. [Link Scope](#link-scope)
8. [Format Invariants](#format-invariants)
9. [Ordering Contracts](#ordering-contracts)
10. [Source Identity](#source-identity)
11. [Galaxy Identity Encoding](#galaxy-identity-encoding)
12. [Forest Sidecar](#forest-sidecar)
13. [Validation Requirements](#validation-requirements)
14. [Storage Layout](#storage-layout)
15. [Runtime support status](#runtime-support-status)
16. [Versioning Policy](#versioning-policy)

---

## Role and Scope

Horizontal input groups halos by snapshot rather than by forest, so the working set of a run is one snapshot's halo population instead of one forest's history. Version 3 keeps that structure and drops three assumptions version 2 inherited from its single Consistent-Trees ASCII source:

- that every non-null `Descendant` link spans exactly one snapshot;
- that `MostBoundID` is a unique, positive, sortable row key;
- that the payload's units and precision are fixed by the format rather than declared by the producer.

Each of those is false for at least one source this format must now carry. A read-only scan of the eight local mini-Millennium L-Halo files found **29,291 descendant links spanning more than one snapshot, maximum span 2** — so an adjacency-only format cannot represent even mini-Millennium losslessly. L-Halo `MostBoundID` is a signed particle identifier with no uniqueness guarantee. L-Halo mass is float32 in `1e10 Msun/h` while Consistent-Trees mass is `Msun/h`; converting one into the other and back would destroy bit parity.

**Lossless means lossless.** Version 3 exists so that conversion is a change of layout, not of content. It does not permit dropping a halo, rejecting a valid link, inventing an intermediate halo, or closing a gap. In particular:

- **No phantom insertion.** Nothing in this pipeline creates a halo that the source does not contain. Consistent-Trees writes its own interpolated phantoms and those are carried through as source halos like any other; a gapped source is *not* repaired by manufacturing the missing generations.
- **No gap removal.** A valid forward gap is recorded as a gap. Collapsing `A(snap 0) → D(snap 2)` into two adjacent edges, or discarding it because the driver cannot yet consume it, would be a silent scientific change.

Version 3 is produced offline by the converter under `scripts/convert/`, exactly as version 2 is, and validated at conversion time by an independent producer validator.

## What Version 3 Changes

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

**Version 1 remains rejected outright**, for the reason recorded in the version 2 specification: version 1 data was produced with the scientifically incorrect `fix_flybys()` operation live. Version 3 changes nothing about that. A version 1 dataset must be reconverted from source.

**A dataset never mixes versions.** Every file of one dataset declares the same `format_version`.

## File Set and Naming

A version 3 dataset consists of:

- **One HDF5 file per snapshot in the package's `a_list`**: `snapshot_NNN.h5`, zero-padded, in ascending scale-factor order. Every snapshot has a file, **including snapshots containing zero halos** — an empty snapshot is a real, valid file with `n_halos = 0` and zero-length datasets, not an absent one. Gapped sources make empty snapshots ordinary rather than exotic.
- **One run-level sidecar**: `forests.h5`, written once per dataset. Provenance only; see [Forest Sidecar](#forest-sidecar).

Each `snapshot_NNN.h5` contains **exactly three HDF5 objects**: the group `/header` (scalar metadata as attributes), the group `/halos` (one dataset per field, struct-of-arrays) and the group `/schema` (the producer's payload declarations). No other root object is permitted.

## Header Attributes

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

## Halo Datasets

All datasets live under `/halos`, each of length `n_halos`; vectors are `[n_halos, 3]`.

### Topology

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

### Identity

| Dataset | Type | Semantics |
|---|---|---|
| `SourceHaloID` | int64[N] | Positive, globally unique converter row key; see [Source Identity](#source-identity) |
| `ForestIndex` | int64[N] | Source-relative forest identity |
| `HaloRankInForest` | int64[N] | Source-relative within-forest row identity |

### Payload

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

## The Schema Group

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

  All are explicit little-endian (see [Storage Layout](#storage-layout)). A declaration whose `type` disagrees with the dataset's actual dtype or shape makes the file invalid.
- **Topology and the three identity arrays are not redeclared.** They are governed by the fixed format tables above. Declaring them in `/schema` would create two sources of truth for one contract.
- A missing declaration, an extra declaration, or a declaration whose `type` disagrees with the dataset's actual dtype or shape makes the file invalid.
- `/schema`, `source_format` and `column_mapping_sha256` are **identical across every snapshot file** of a dataset.
- No object under `/halos` or `/schema` may be an HDF5 external or soft link. Every dataset is physically present in the file that names it.

## Link Scope

Version 2 could name a link's target file from the link type alone. Version 3 cannot, and says so explicitly:

| Link field | Points into |
|---|---|
| `Descendant` | the file for `DescendantSnapshot` |
| `FirstProgenitor` | the file for `FirstProgenitorSnapshot` |
| `NextProgenitor` | the file for `NextProgenitorSnapshot` |
| `FirstHaloInFOFgroup` | this snapshot's file |
| `NextHaloInFOFgroup` | this snapshot's file |

A consumer resolves every non-FoF link through its companion snapshot column. Assuming N±1 is a version 2 reading and is wrong for version 3 whenever `links_adjacent = 0`.

## Format Invariants

Violating any invariant makes a file invalid. Producers and consumers **abort on violation; nothing repairs**.

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

## Ordering Contracts

Cross-format identity depends on orderings a consumer cannot derive at runtime. They are producer-owned facts carried by the format.

1. **Chain order is the source's.** For an already-linked source (L-Halo binary, forests-HDF5), the producer preserves the source's stored `FirstProgenitor`/`NextProgenitor`/FoF chains exactly, in their stored order. It does not re-derive them by mass, and does not apply another format's tie-break rules.
2. **Consistent-Trees keeps its reference reconstruction.** For Consistent-Trees ASCII, chain order remains what the reference reader's literal incremental-insertion loop produces (`ctrees_utils.c` `assign_mergertree_indices`), with `fix_upid()` host fix-ups and without `fix_flybys()`, exactly as version 2 requires.
3. **Row order is a storage order, not a priority.** Rows are sorted by ascending `SourceHaloID` within a snapshot. That is a deterministic, reproducible storage order derived from the adapter's declared source order. It is **not** progenitor priority, not mass order and not a semantic reordering: output topology is *remapped* to the new row positions, never reordered in meaning.
4. **Forest enumeration and within-forest rank follow the selected source representation.** See [Source Identity](#source-identity).

Version 2 sorted rows by `MostBoundID`. Version 3 deliberately does not, because duplicate and signed particle identifiers are not valid general remapping keys. This is a version 3 rule; version 2's ordering contract is untouched.

## Source Identity

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

## Galaxy Identity Encoding

Unchanged from version 2:

`UniqueGalaxyID = HaloRankInForest + multiplier × (ForestIndex + 1)`

The multiplier is per-simulation metadata declared in the simulation package and recorded in output provenance. It must exceed the dataset's `max_halo_rank_in_forest`, and `multiplier × (n_forests_total + 1)` must fit in int64 — both checked at startup against this format's header attributes.

## Forest Sidecar

`forests.h5` carries source provenance for every forest, as three int64 datasets each of length `n_forests_total`, **all three at the file root**, with no enclosing group:

| Dataset | Type | Semantics |
|---|---|---|
| `ForestID` | int64[n_forests_total] | The source forest identifier, per the table in [Source Identity](#source-identity) |
| `SourceFileOrdinal` | int64[n_forests_total] | Inventory file ordinal owning the forest |
| `SourceUnitOrdinal` | int64[n_forests_total] | Within-file unit ordinal of the forest |

The object paths are exactly `/ForestID`, `/SourceFileOrdinal` and `/SourceUnitOrdinal`. `forests.h5` contains those three objects and nothing else — no `/header`, no group, no attributes. This keeps `/ForestID` at the same path version 2 puts it, so a tool that reads a version 2 sidecar's `/ForestID` reads a version 3 one's unchanged; version 3 only adds two datasets beside it.

For a Consistent-Trees ASCII forest that **spans files**, both ordinals are −1 and `ForestID` carries the original forest id; the conversion manifest retains the full source membership, which a three-column sidecar cannot express. For `lhalo_binary`, `ForestID` is the dense run forest number and the two ordinals disambiguate it. For `consistent_trees_hdf5`, the source `ForestID` is retained even where it is only unique within a file — the ordinals make the pair unique.

Mimic never reads this sidecar. It exists for provenance, debugging and independent comparison.

## Validation Requirements

**Producers** must verify before declaring a dataset valid:

- total halo count conservation against the source inventory;
- every [format invariant](#format-invariants);
- progenitor round-trip closure across snapshots, resolved through the target-snapshot columns rather than assumed adjacent;
- `NextProgenitor` chain membership: every member names the same `Descendant`/`DescendantSnapshot` as its owner, and every member's own snapshot is strictly earlier than that shared `DescendantSnapshot`. Checking the target's snapshot against the *owner's* snapshot instead is the mistake to avoid: on real data the target is most often at the same snapshot as its owner and is sometimes later, and both are valid;
- FoF chain integrity within each snapshot;
- `SourceHaloID` positivity, global uniqueness and ascending row order;
- identity uniqueness and density, and the header bounds;
- `/schema` agreement with the actual dataset dtypes and shapes, and with the canonical mapping digest;
- `Len ≥ 0`, with zero counts logged.

Validation uses **bounded reads** and handles empty arrays. A validator must not require a whole-snapshot resident array to check a snapshot above `INT32_MAX` rows, and must behave correctly on a zero-halo file rather than skipping it.

**A consumer** must validate at open, at minimum: `format_version` is supported; header consistency; `/schema` agreement with what it intends to read; `scale_factor` agreement with the package's `a_list`; identity-multiplier bounds; and agreement of the five physical header values with the configured simulation package. At slab load it must validate link ranges against the resolved target file's `n_halos`. Mimic implements none of this for version 3 today — see below.

## Storage Layout

- Datasets are **explicit little-endian**, **uncompressed** and chunked, struct-of-arrays: chunk shape `(65536,)` for scalar datasets and `(65536, 3)` for vectors. Explicit endianness is a contract, not a host artefact: a file written on one architecture is read identically on another.
- Uncompressed is deliberate and unchanged from version 2 — slab reads are the hot path and compression measurably hurts them. Chunking is what makes a bounded row-range read possible without touching neighbouring rows, which is how a snapshot above `INT32_MAX` can be read at bounded cost.
- A zero-halo snapshot has zero-length datasets of the correct type and shape, not absent ones.
- Producers should write with the HDF5 latest-version file format bounds available to them; consumers must not depend on chunk boundaries, only on dataset shape and type.

## Runtime support status

**Explicitly pending. This document does not claim Mimic can run version 3 data.**

Producing a lossless file and executing it are separate problems, and this format solves only the first. The known consumer-side prerequisites, none of which is in scope for the converter work:

| Prerequisite | Current state |
|---|---|
| Accept `format_version = 3` | Reader rejects it (`read_horizontal_hdf5.c`) |
| int64 link datasets | Reader's fixed dataset table declares the five links int32 |
| `/schema` group and extra datasets | Reader rejects any unexpected root object or `/halos` dataset |
| Cross-snapshot retained state | Driver holds exactly two generations; progenitor gathering reads only the previous slab (`horizontal_driver.c`) |
| Slabs above `INT_MAX` | Driver refuses them explicitly |
| Native payload units per source | A package's `halo_properties.yaml` must declare the units the file declares |

[`MIMIC-V3-CONSUMER-DESIGN-REVIEW.md`](MIMIC-V3-CONSUMER-DESIGN-REVIEW.md) traces each proposed field, unit, ordering and qualified link through the generated input view, gap-state ownership and bounded slab access, with a worked mixed-gap graph. It is the input to Gate G1 and is unapproved at the time of writing.

## Versioning Policy

`format_version` is a single int32 ratchet. Readers reject files with an unrecognised version; producers stamp the version they implement.

- **Changing an envelope rule requires another format bump.** The object set, the header attribute set, the fixed topology and identity tables, the ordering contracts and the invariants are the envelope.
- **Adding a new conforming declared field does not.** A selected extra is declared in `/schema`, validated like any other payload field, and is exactly what the envelope already provides for. This is the one respect in which version 3 is deliberately more additive than version 2, whose policy required a bump for any new dataset — because version 2 had no schema group in which to declare one.
- **Version 1 is rejected**, permanently, for the reason given in the version 2 specification.
- **A dataset never mixes versions.**

---

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [VISION.md](../VISION.md): architectural principles and design boundaries
- [HORIZONTAL-HDF5-FORMAT.md](HORIZONTAL-HDF5-FORMAT.md): the frozen, normative version 2 specification
- [MIMIC-V3-CONSUMER-DESIGN-REVIEW.md](MIMIC-V3-CONSUMER-DESIGN-REVIEW.md): the Gate G1 consumer-design review of this draft
- [MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md): the plan whose contract C3 this draft implements
- [MIMIC-CONVERTER-SOURCE-INVENTORY.md](MIMIC-CONVERTER-SOURCE-INVENTORY.md): measured source capability inventory
- [`scripts/convert/profiles/README.md`](../../scripts/convert/profiles/README.md): the mapping-profile grammar and schema identity
