# Mimic v3 Consumer-Design Review

**APPROVED 23-09-2026 by Darren Croton**

**Purpose**: Trace every field, unit, ordering and snapshot-qualified link proposed by [`HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md`](HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md) through the consumer side — the generated input view, gap-state ownership and bounded slab access — and establish whether that draft can be frozen as a producer contract without stranding the consumer that must eventually read it.

> **Status: APPROVED** — see the dated line above.
>
> This document is the artifact of **Gate G1** in [`MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md). It was authored as a Slice 2 deliverable; authoring it was not approving it. The owner approved it by adding the dated `**APPROVED <date> by owner**` line at the top of this file and committing it — nothing else counted, not a chat message, not a project-manager judgement, and not the existence of this document on its own. With that line committed, the v3 draft is normative for the purposes this gate covers, and later slices may build against it.

**Scope**: this is a design review, not an implementation. It changes no runtime code and proposes none. Where it identifies work the consumer will need, that work belongs to the separate reader/driver project the plan already names, not to the converter slices.

---

## Table of Contents

1. [Method and sources of evidence](#method-and-sources-of-evidence)
2. [A worked mixed-gap graph](#a-worked-mixed-gap-graph)
3. [Mixed-snapshot chains](#mixed-snapshot-chains)
4. [Retained gap-state ownership](#retained-gap-state-ownership)
5. [SourceHaloID ordering](#sourcehaloid-ordering)
6. [Native payload units and metadata](#native-payload-units-and-metadata)
7. [Generated input views](#generated-input-views)
8. [Wide and chunked access](#wide-and-chunked-access)
9. [A consumer metadata fragment](#a-consumer-metadata-fragment)
10. [Findings](#findings)
11. [Recommendation to the owner](#recommendation-to-the-owner)

---

## Method and sources of evidence

Every claim below about current consumer behaviour was read out of the shipped source at the commit this review was written against, not recalled:

| Claim | Read from |
|---|---|
| Unsupported versions are rejected | `src/io/horizontal/read_horizontal_hdf5.c`, `HORIZONTAL_HDF5_FORMAT_VERSION` |
| The five links are declared int32 | `read_horizontal_hdf5.c`, `HORIZONTAL_H5_HALO_DATASETS` — 16 entries, links `HORIZONTAL_H5_I32` |
| Unexpected objects are fatal | `read_horizontal_hdf5.c` — fatal on an unexpected root object, `/halos` dataset, or `/header` attribute |
| `MostBoundID` is read but never used as a lookup key | `read_horizontal_hdf5.c` — it appears in the dataset table and nowhere else |
| The driver holds exactly two generations | `src/core/horizontal_driver.c` — `prev` and the current generation; progenitor gather/count read `prev` |
| Progenitor chains are assumed to stay in the previous slab | `horizontal_driver.c` — the cycle guard bounds chain steps by `prev->view.count` |
| Slabs above `INT_MAX` are refused | `horizontal_driver.c` — explicit check on `gen->slab.nhalos` |
| Declared units drive a generated conversion to the reference basis | `scripts/generate_properties.py` — `_input_convert` / `_linear_conversion_expr` |
| Link fields materialise as C `int` | `src/include/generated/raw_halo_defs.h` — `int Descendant; int FirstProgenitor; ...` |
| `NextProgenitor` direction on real data | read-only scan of all eight `simulations/mini-millennium/snapshots/trees_063.*` files: 51,270 non-null links — 2,980 earlier than the owner, 47,291 the same snapshot, 999 later; all 51,270 strictly earlier than the shared `DescendantSnapshot`, all 51,270 naming the owner's own descendant |

The worked graph below is constructed independently for this review. It is *mini-Millennium-like* — it has the same maximum forward-gap span of 2 that a scan of all eight local mini-Millennium files measured — but it is not extracted from that data, so it exercises the format's rules rather than reproducing one dataset's accidents.

## A worked mixed-gap graph

Five snapshots (`a_list` indices 0–4) and five halos, from a single L-Halo source file (file ordinal 0) holding one tree (unit ordinal 0). Source rows are in the tree's own stored order, descendant-first:

| Source row | Halo | Snapshot | Mass | Note |
|---|---|---|---|---|
| 0 | E | 4 | 60 | final halo |
| 1 | D | 2 | 50 | |
| 2 | A | 0 | 30 | |
| 3 | B | 1 | 20 | FoF central at snapshot 1 |
| 4 | C | 1 | 10 | satellite of B |

Topology as the source stores it:

- `A(0) → D(2)` — **forward gap, span 2**
- `B(1) → D(2)` — adjacent
- `C(1) → D(2)` — adjacent
- `D(2) → E(4)` — **forward gap, span 2, crossing an empty snapshot 3**
- D's progenitors, most massive first: A, then B, then C.
- At snapshot 1, B is the FoF central and C its satellite.
- Snapshot 3 contains no halos at all.

`SourceHaloID` is the prefix sum over the declared source order, starting at 1: since there is one file with one unit, `SourceHaloID = 1 + row_ordinal`. So `E = 1, D = 2, A = 3, B = 4, C = 5`. Rows within each emitted snapshot are then sorted by ascending `SourceHaloID`.

The emitted dataset, in full:

**`snapshot_000.h5`** — `n_halos = 1`

| row | `SourceHaloID` | `HaloRankInForest` | `Descendant` | `DescSnap` | `FirstProg` | `FPSnap` | `NextProg` | `NPSnap` | `FirstFOF` | `NextFOF` |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 (A) | 3 | 2 | 0 | **2** | −1 | −1 | 0 | **1** | 0 | −1 |

**`snapshot_001.h5`** — `n_halos = 2`

| row | `SourceHaloID` | `HaloRankInForest` | `Descendant` | `DescSnap` | `FirstProg` | `FPSnap` | `NextProg` | `NPSnap` | `FirstFOF` | `NextFOF` |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 (B) | 4 | 3 | 0 | 2 | −1 | −1 | 1 | 1 | 0 | 1 |
| 1 (C) | 5 | 4 | 0 | 2 | −1 | −1 | −1 | −1 | 0 | −1 |

**`snapshot_002.h5`** — `n_halos = 1`

| row | `SourceHaloID` | `HaloRankInForest` | `Descendant` | `DescSnap` | `FirstProg` | `FPSnap` | `NextProg` | `NPSnap` | `FirstFOF` | `NextFOF` |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 (D) | 2 | 1 | 0 | **4** | 0 | **0** | −1 | −1 | 0 | −1 |

**`snapshot_003.h5`** — `n_halos = 0`. Every `/halos` dataset has length 0. The file exists, carries the full header and the identical `/schema`, and is valid.

**`snapshot_004.h5`** — `n_halos = 1`

| row | `SourceHaloID` | `HaloRankInForest` | `Descendant` | `DescSnap` | `FirstProg` | `FPSnap` | `NextProg` | `NPSnap` | `FirstFOF` | `NextFOF` |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 (E) | 1 | 0 | −1 | −1 | 0 | **2** | −1 | −1 | 0 | −1 |

`links_adjacent = 0` in **every** file, because the dataset contains gaps — not only in the two files that hold a gapped link.

Three properties of the format are visible here and are worth naming, because each is a place a version 2 reading would go wrong:

1. **D's progenitor chain spans two snapshots.** `FirstProgenitor(D) = A` at snapshot **0**, and the chain continues to B and C at snapshot **1**. A consumer that assumes a progenitor chain lives in one slab loses B and C, or loses A.
2. **`NextProgenitor` crosses a snapshot boundary, and points *forward* relative to its owner.** `NextProgenitor(A) = B` — A is at snapshot 0 and B at snapshot 1, so the link's `NextProgenitorSnapshot` (1) is *later* than the owner's `SnapNum` (0). That is legal and ordinary: the constraint on `NextProgenitor` is relative to the **shared descendant**, not to the owner. This is C3's "`NextProgenitor` can target a different earlier snapshot than its sibling" — "earlier" meaning earlier than the descendant every sibling shares. A draft invariant written owner-relative is what finding 11 below records and the draft now corrects.
3. **Every sibling names the same descendant.** A, B and C all carry `Descendant = 0, DescendantSnapshot = 2`. That is the invariant a consumer can check cheaply and the one that makes a mixed-snapshot chain well-defined at all.

## Mixed-snapshot chains

**What the format asks of a consumer.** To gather the progenitors of a halo at snapshot *N*, follow `FirstProgenitor` into the file named by `FirstProgenitorSnapshot`, then follow each `NextProgenitor` into the file named by that halo's own `NextProgenitorSnapshot`. The chain terminates at −1. Nothing else changes: chain order, tie-breaks and FoF structure are all as before.

**What the current driver does.** `horizontal_driver.c` holds one previous generation, `prev`, and reads every progenitor through it. Its cycle guard bounds the walk by `prev->view.count`, which encodes the same assumption a second time: the chain is inside the previous slab. Applied to the worked graph, processing D at snapshot 2 with only snapshot 1 retained would find B and C and never reach A, because A is not in `prev` — and the chain *head* is A, so the walk would not even start correctly.

**What a gap-capable consumer needs.** Three things, all of them consumer-side:

1. Resolve each link through its snapshot column rather than assuming N−1.
2. Retain more than one previous generation — as many as the data demands, bounded (see the next section).
3. Bound the chain walk by the total retained population rather than by one slab's count.

**What it does not need.** No new field, no new attribute and no change to the draft. The three snapshot columns are precisely the information a chain walk requires, and they are already there.

## Retained gap-state ownership

This is the question the review was most likely to answer badly, so it is worth being explicit: **how does a consumer know, at snapshot *k*, whether snapshot *k*'s state must survive into the future — before it has read the future?**

It reads `DescendantSnapshot` on the slab it is already holding. A halo at snapshot *k* whose `DescendantSnapshot` is *m* must have its state available when the consumer reaches *m*. Taking the maximum over the slab gives a per-snapshot **retention horizon**:

```text
horizon(k) = max(DescendantSnapshot[i]) over rows i of snapshot k with Descendant[i] != -1
           = k                                   (if the snapshot is empty or has no descendants)
```

Snapshot *k*'s retained state may be released once the consumer has finished processing `horizon(k)`. In the worked graph: `horizon(0) = 2`, `horizon(1) = 2`, `horizon(2) = 4`, `horizon(3) = 3` (empty), `horizon(4) = 4`. So while processing snapshot 2 the consumer holds the retained state of snapshots 0, 1 and 2 — three generations, not two — and releases 0 and 1 immediately afterwards. While processing snapshot 4 it holds only 2 and 4; snapshot 3 was released as soon as it was passed, because nothing points past it.

This is a local, forward-only, exact rule computed from data the consumer has already loaded. It needs no extra header attribute, no backward scan and no whole-dataset pre-pass. **That is the finding that keeps this draft freezable**: had the retention bound been underivable from the file, v3 would have needed a new attribute, and that would be a semantic schema change requiring a plan amendment rather than a Slice 2 deliverable.

**Ownership.** Mimic requires memory ownership to be explicit, so the retained pool needs one owner, not a shared cache. The natural shape, consistent with the current driver, is that the **driver** owns a pool of retired generations keyed by snapshot number, each entry carrying the release horizon computed when that generation was loaded, and the reader stays stateless with respect to retention. The driver already owns the single `prev` generation; this generalises that ownership rather than introducing a second owner. A worst case remains worth measuring before implementation: a dataset whose horizons are all far ahead retains many generations at once, and the memory bound is therefore a property of the data, not of a constant. `links_adjacent = 1` collapses the whole mechanism back to today's two generations, which is why measuring and stamping it honestly matters.

## SourceHaloID ordering

**What changes.** Version 2 sorts rows within a snapshot by ascending `MostBoundID` and guarantees those values unique and positive. Version 3 sorts by ascending `SourceHaloID` and makes no uniqueness, positivity or ordering claim about `MostBoundID` at all.

**Why.** L-Halo's `MostBoundID` is a signed most-bound *particle* identifier. It is not guaranteed unique across a catalog, and the plan forbids assuming it is or sorting the source tree to derive identity. A key that may repeat cannot order rows deterministically, and a converter that silently deduplicated or re-signed it would be changing the science to fit the format. `SourceHaloID` is the converter's own key: positive, globally unique, derived by prefix-summing the declared inventory, and exactly invertible back to `(file, unit, row)`.

**What it costs the consumer.** In the shipped reader, nothing measurable: `MostBoundID` appears in the dataset schema table and nowhere else — the reader never binary-searches by it, and the driver never looks it up. Version 2's "binary-searchable by id" is a property of the files, not a mechanism anything currently uses. A *future* consumer wanting id lookup gets a better key, since `SourceHaloID` is dense and globally unique while `MostBoundID` may not be either.

**What it costs comparison tooling.** Nothing, and it removes a trap. The plan's C5 already requires comparisons to use source coordinates and `SourceHaloID`, never `MostBoundID` alone, precisely because the latter cannot join rows in a source where it repeats.

**One consequence to state plainly.** Rows of a v3 file are in a different order from rows of a v2 file converted from the same Consistent-Trees source. Row index *i* in one is not row index *i* in the other. Any comparison between a v2 dataset and a v3 dataset must join on identity, never on position — which is the rule the plan already imposes.

## Native payload units and metadata

**What changes.** Version 2 fixes the payload's units in the format: `M_Crit200` is native Msun/h because the only source was Consistent-Trees. Version 3 carries the adapter's native units and declares them in `/schema`. An L-Halo-derived dataset therefore carries `M_Crit200` as float32 in `1e10 Msun/h`; a Consistent-Trees-derived one carries float32 `Msun/h`. Same dataset name, same dtype, different basis.

**Why the producer must not normalise.** A round trip through the other basis is not value-preserving in float32, and bit parity against the selected format's own vertical reader is the acceptance evidence for the whole conversion. Making the two sources look alike would destroy exactly the evidence the work depends on.

**How the consumer copes today, unchanged.** Units are already consumer-configurable: a simulation package's `halo_properties.yaml` declares each field's `units` and `h_convention`, and `scripts/generate_properties.py` emits a linear `_input_convert` from those declarations to Mimic's internal reference basis. A package declaring `Msun/h` gets a `1e-10` factor; one declaring `1e10 Msun/h` gets `1.0`. This mechanism already ships and already distinguishes the two cases — `simulations/mini-millennium/halo_properties.yaml` declares `1e10 Msun/h` and `simulations/micro-uchuu-horizontal/halo_properties.yaml` declares `Msun/h`, today.

**What follows for packages.** A v3 dataset needs a simulation package whose declarations match the file's `/schema`. That is not a new burden — it is the existing rule — but it does mean **one package per (simulation, source format)**, not one per simulation. Converting mini-Millennium from L-Halo binary and from a Consistent-Trees packaging would produce two datasets needing two packages with different `M_Crit200` units. The `/schema` group is what makes that checkable rather than a silent mismatch: a startup comparison of package declarations against the file's own declarations turns a wrong-by-1e10 mass into an abort.

**The gap worth naming.** An extra field's `units` is any nonempty string by contract, and a unit outside `generate_properties.py`'s `UNIT_REGISTRY` cannot be declared by a package until it is added there. The converter will not substitute a known unit in its place. This affects selected extras only, never the core payload — every core payload unit in the draft is already in the registry, which the converter test suite asserts mechanically against the generator's own tables.

## Generated input views

Tracing each v3 field into `struct RawHalo` and the generated accessors:

| v3 field | Generated view today | Status for a v3 consumer |
|---|---|---|
| `M_Crit200`, `Pos`, `Vel`, `Spin`, `VelDisp`, `Vmax` | declared in `halo_properties.yaml`, converted by `_input_convert` | **Works as-is**, given a package matching `/schema` |
| `Len`, `SnapNum` | core roles `Len`, `SnapNum`, C `int` | **Works as-is**; both are int32 in v3 |
| `MostBoundID` | `long long` | **Works as-is**; v3 only relaxes what may be *in* it |
| Five topology links | core roles; materialise as C `int` (`raw_halo_defs.h`) | **Needs widening.** v3 stores int64; the generated struct and the reader's dataset table are int32 |
| `DescendantSnapshot`, `FirstProgenitorSnapshot`, `NextProgenitorSnapshot` | no equivalent | **New.** Either new reader-owned arrays, like `ForestIndex`/`HaloRankInForest`, or new declared properties |
| `SourceHaloID` | no equivalent | **New**, and best treated the same way as the other two identity arrays: read by name into reader-owned state, not through `RawHalo` |
| `ForestIndex`, `HaloRankInForest` | already read by name into `struct SnapshotSlab` | **Works as-is** |
| Selected extras | not declared anywhere | **Opt-in.** A package that declares an extra materialises it; one that does not, ignores it — but the reader's current "exactly this dataset set" rule must become "at least this set, plus whatever `/schema` declares" |

The pattern the format already established for `ForestIndex` and `HaloRankInForest` — format-owned identity metadata read directly by name into reader-owned slab arrays, exempt from `halo_properties.yaml` — is the right precedent for `SourceHaloID` and the three snapshot columns. It keeps format metadata out of the catalog property vocabulary, which is what the 2026-08-11 erratum in the v2 specification settled.

Every row marked **Needs widening** or **New** is reader/driver work, explicitly outside the converter plan's scope.

## Wide and chunked access

**What v3 permits.** A snapshot above `INT32_MAX` rows. If the full-Uchuu package README's ≈181.5 × 10⁹ halos over 50 snapshots holds, at least one snapshot necessarily exceeds `INT32_MAX` — arithmetic from documented totals, not a fresh scan, since that data is not mounted here. The format must not carry a ceiling that only one simulation happens to hit, so the links are int64 regardless of whether that figure is ever confirmed.

**What the consumer does today.** `horizontal_driver.c` explicitly refuses a slab above `INT_MAX`, with a clear error. That refusal is honest and should stay until chunked slab streaming exists; silently widening the type without bounding the working set would replace a clean abort with an allocation failure.

**What the format does to help.** Datasets are chunked at 65536 scalar rows (or `[65536, 3]` for vectors) and uncompressed, so a consumer can read any contiguous row range at bounded cost without decompressing neighbours. Validators are required to use bounded reads and to handle empty arrays, so producer-side validation does not need a whole-snapshot resident array either. Nothing in the format requires a consumer to materialise a whole slab; that is a property of today's driver, not of the file.

**What is still missing, and is not this plan's work.** Chunked slab streaming in the driver — holding a window of a snapshot rather than all of it — is a separate architectural project the plan already records as still needed. Gap retention (above) interacts with it: retaining three generations of *windows* is a different memory problem from retaining three generations of whole slabs, and the two should be designed together rather than one after the other.

## A consumer metadata fragment

Produced by `CanonicalSchema.consumer_metadata_fragment()` in `scripts/convert/column_schema.py`, from the shipped `profiles/lhalo_binary_extras_example.yaml` against `simulations/mini-millennium/halo_properties.yaml`. Abridged below for readability — the payload entries between `Pos` and `SubhaloIndex` follow the same shape — and reproducible in full by loading that profile and calling the method.

```json
{
  "source_format": "lhalo_binary",
  "column_mapping_sha256": "a01e140cea18564c80db862c8a58e021781a1f4bc80d4adeb62e9d4ee813d367",
  "complete": false,
  "incomplete_because": "runtime topology support for gapped links and int64 slab indices is not described here; see docs/dev/MIMIC-V3-CONSUMER-DESIGN-REVIEW.md",
  "identity_conventions": {
    "forest_index": "file-prefix tree number",
    "halo_rank_in_forest": "original within-tree row index",
    "forest_id": "dense run forest number, disambiguated by the two ordinals",
    "ordinals": "file ordinal and within-file tree ordinal"
  },
  "halo_properties": [
    {
      "name": "Len", "type": "int", "units": "particles", "h_convention": "none",
      "description": "Particle count supplied by the source; never negative",
      "provides_core_role": "Len"
    },
    {
      "name": "SnapNum", "type": "int", "units": "dimensionless", "h_convention": "none",
      "description": "Snapshot index of this halo",
      "provides_core_role": "SnapNum"
    },
    {
      "name": "M_Crit200", "type": "float", "units": "1e10 Msun/h", "h_convention": "carried",
      "description": "Native L-Halo M_Crit200; kept float32 in 1e10 Msun/h with no unit round trip",
      "provides_core_role": "HaloMass"
    },
    {
      "name": "MostBoundID", "type": "long long", "units": "dimensionless", "h_convention": "none",
      "description": "Signed most-bound particle identifier carried as data; duplicates are legal",
      "provides_core_role": null
    },
    {
      "name": "M_Mean200", "type": "float", "units": "1e10 Msun/h", "h_convention": "carried",
      "description": "Mass within the radius enclosing 200x the mean matter density",
      "provides_core_role": null
    }
  ],
  "format_table_fields": [
    {"name": "Descendant", "type": "long long", "provides_core_role": "Descendant",
     "description": "Snapshot-local row index of the descendant, -1 if none"},
    {"name": "DescendantSnapshot", "type": "int", "provides_core_role": null,
     "description": "Snapshot of the Descendant target, -1 iff Descendant is -1"},
    {"name": "SourceHaloID", "type": "long long", "provides_core_role": null,
     "description": "Positive, globally unique converter row key from the adapter's declared source order"}
  ]
}
```

Three things about this fragment are deliberate:

- **`"complete": false` is machine-readable, not a footnote.** The fragment describes the payload a `halo_properties.yaml` would declare. It does **not** describe runtime topology support, and a tool that consumed it as a complete consumer specification would be wrong. It says so in its own output.
- **Every `type`, `units` and `h_convention` is drawn from the property generator's vocabulary.** `EXTRA_TYPES` is asserted equal to `generate_properties.TYPE_MAP`, `H_CONVENTIONS` equal to the generator's, and every core payload unit present in `UNIT_REGISTRY` with the same `h_convention` the registry assigns it. Those are mechanical tests against the generator itself, not a transcribed copy.
- **`format_table_fields` is separate from `halo_properties`.** Each entry names the core role it provides (the five links provide their own; the target-snapshot and identity fields provide none), so a consumer can see every core role the file satisfies without inferring it from names. Topology and identity are format-owned and are not offered as catalog properties to declare — the distinction the v2 erratum of 2026-08-11 established for `ForestIndex` and `HaloRankInForest`.

## Findings

| # | Finding | Kind | Owner |
|---|---|---|---|
| 1 | Progenitor chains may span snapshots; the driver's one-`prev` model and its chain cycle guard both assume one slab | Runtime prerequisite | Reader/driver project |
| 2 | Retention horizons are exactly derivable from `DescendantSnapshot` on an already-loaded slab; no new field or attribute is needed | **Resolved in the draft** | — |
| 3 | Retained-generation ownership should sit with the driver, generalising its existing `prev` ownership; worst-case retention is a property of the data and should be measured | Runtime design note | Reader/driver project |
| 4 | The five links are int64 in v3 and int32 in `struct RawHalo` and the reader's dataset table | Runtime prerequisite | Reader/driver project |
| 5 | `SourceHaloID` and the three snapshot columns have no generated-view equivalent; the `ForestIndex`/`HaloRankInForest` precedent fits them | Runtime design note | Reader/driver project |
| 6 | The reader's "exactly this dataset set" rule must become "at least this set, plus whatever `/schema` declares" before extras can be consumed | Runtime prerequisite | Reader/driver project |
| 7 | `MostBoundID` row ordering is unused by the shipped reader, so dropping it costs the current consumer nothing | **No action** | — |
| 8 | A v3 dataset needs one simulation package per (simulation, source format) because payload units differ by source; `/schema` makes the mismatch checkable | Packaging consequence | Documented here and in the draft |
| 9 | An extra declaring a unit outside `UNIT_REGISTRY` converts but cannot be declared by a package until that unit is added | Known limitation | Stated in the profile README |
| 10 | Slabs above `INT_MAX` remain refused by the driver; chunked slab streaming and gap retention interact and should be designed together | Runtime prerequisite | Reader/driver project |
| 11 | **The draft's Invariant 1 originally constrained `NextProgenitor` relative to its *owner* ("strictly earlier"), which contradicted this review's own worked graph and rejects 94.2% of real links.** Drafting error in the invariant's prose, found during Slice 2's review and corrected in the draft | **Corrected in the draft** | — |

**No finding requires a change to the v3 *data model*, and one required a correction to the draft's *prose*.** Findings 1, 3, 4, 5, 6 and 10 are consumer-side work that the plan already places outside the converter scope and that the draft's [Runtime support status](HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md#runtime-support-status) section already declines to claim. Findings 8 and 9 are consequences to document, and both are documented. Findings 2 and 7 close.

**Finding 11 is the exception, and it is worth being exact about what it was.** The draft's Invariant 1 originally read "every non-null `FirstProgenitor` and `NextProgenitor` targets a strictly earlier [snapshot]", constraining `NextProgenitor` relative to the halo that owns it. That is false. It contradicted this review's own worked graph — `NextProgenitor(A) = B` points *forward* from snapshot 0 to snapshot 1 — and a read-only scan of all eight real mini-Millennium L-Halo files measured **51,270** non-null `NextProgenitor` links, of which only **2,980 (5.8%)** target a strictly earlier snapshot than their owner; **47,291 (92.2%)** target the *same* snapshot and **999 (1.9%)** a *later* one. An owner-relative invariant would therefore have rejected **94.2%** of conforming real input, including the ordinary same-snapshot case version 2 already carried.

The correct constraint, which the draft now states, is relative to the shared descendant: a `NextProgenitor` target is another progenitor of the owner's own `Descendant`/`DescendantSnapshot`, unconstrained relative to the owner, and strictly earlier than that shared `DescendantSnapshot`. The same scan confirms this reading against the same data: **all 51,270 links satisfy it, with zero violations, and all 51,270 name the same descendant as their owner.**

**This was a drafting error in one invariant's prose, not a change to the v3 field or type contract.** The data model is unchanged — the same five int64 links, the same three int32 target-snapshot columns, the same identity arrays, the same payload rules. Only a stated constraint on one of those columns was wrong, and it was wrong in a direction that no producer could ever have satisfied. It was found and fixed inside Slice 2, before the draft went to the owner, which is what this review exists to do.

## Recommendation to the owner

**Recommended: approve the v3 draft as a producer contract, with the conversion-first boundary the plan already states.**

**Finding 11 does not change this recommendation.** It was a wrong sentence in the draft, corrected before the draft reached the owner, and it moved no field, type or ordering. If anything it is evidence that the gate is doing its job: an invariant that no real producer could satisfy was caught by walking a worked example and a real catalog against the text, which is exactly the trace this review was asked to perform.

The reasoning in one paragraph: the draft is losslessly sufficient for the sources in scope, every field it adds is one a consumer demonstrably needs, the hardest consumer question — how to bound retained state without reading the future — is answerable from the file as drafted, and nothing in it claims a runtime capability Mimic has. The cost of approving is that a format exists which nothing can yet execute; the cost of not approving is that the adapters in Slices 3–5 have no target, since every one of them must emit *something*, and the only alternatives are a format that drops mini-Millennium's 29,291 gapped links or one that invents phantom halos to close them. Both are scientific changes disguised as format choices, and the plan forbids both.

**What approval does and does not authorise.** It makes the draft normative for the converter slices. It does **not** authorise any reader or driver change, any production v3 conversion, or any claim that the five named simulations are runnable. Those remain what the plan says they are: a separate architectural project, and a follow-on plan produced by the final slice.

**If the owner instead requires a semantic change to the format** — a new attribute, a different ordering, a different field set — the correct path is a plan amendment, not an edit to the draft in place. The plan says so explicitly, and the Slice 2 contract forbids the Developer from resolving it any other way.

---

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [VISION.md](../VISION.md): architectural principles and design boundaries
- [HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md](HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md): the draft this review examines
- [HORIZONTAL-HDF5-FORMAT.md](HORIZONTAL-HDF5-FORMAT.md): the frozen, normative version 2 specification
- [MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md): the plan and Gate G1
- [MIMIC-CONVERTER-SOURCE-INVENTORY.md](MIMIC-CONVERTER-SOURCE-INVENTORY.md): the measured source capability inventory this review draws its gap counts from
