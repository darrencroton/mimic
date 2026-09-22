# Converter Generalisation Implementation Plan

**Purpose:** Convert the supported vertical tree formats for mini-Uchuu, micro-Uchuu, Uchuu, mini-Millennium and Millennium into a lossless horizontal representation, with declarative field selection and preserved source topology.

**Status:** Proposed implementation contract, 2026-09-22, inspected at `20a0db125725a1c7bc6a4324d27dfccf26b871c9`. No implementation has started. The owner's explicit scope expansion supersedes the earlier Consistent-Trees-ASCII-only proposal. Mode B is recommended; every slice below has an effort label.

**Decision required before execution:** This draft delivers new conversion capability, not new runnable horizontal simulations: the current C reader rejects v3 and the driver cannot retain state across skipped snapshots or load slabs above `INT_MAX` (`src/io/horizontal/read_horizontal_hdf5.c`, `src/core/horizontal_driver.c`). Independent review recommends a runnable, gapped mini-Millennium pilot before broad conversion rollout. The owner must choose that expanded runtime scope or approve conversion-first with the consumer-design gate below. Until then, this is a reviewable draft, not an unattended-launch instruction. A runtime pilot requires a revised slice sequence; PM must not infer permission to add one.

## Current repository state

The checkout was clean on `main`, tracking `origin/main`, before this planning work. The local feature branch `feature/ctrees-snapshot-reader` ends at `94c4f22e`, already an ancestor of current main. The development pathway's “merge F2 next” banner is stale: integrating that branch is no longer a prerequisite. No remote fetch was performed.

[VISION.md](../VISION.md) governs the work: the core is physics-agnostic, conversion happens offline, metadata defines structural truth, readers and drivers are separate, memory ownership is explicit, and invalid input fails rather than being silently repaired. Generalising a converter must not turn format translation into a new scientific prescription.

Recent completed work is visible in code and Git history:

- Both vertical and horizontal drivers exist, sharing inheritance and physics services. The cross-format identity gate is implemented; its historical scientific results were not rerun in this planning session.
- `a654c228` removed the scientifically incorrect `fix_flybys` operation and introduced horizontal format v2. `5005cd49` advanced the identity-gate reference. Version 1 remains invalid.
- `94c4f22e` corrected the six Uchuu-family packages' particle mass to `0.0327` in reference units. Shin-Uchuu has its own `0.0000897` mass and `20000000000` identity multiplier; those are not generic converter defaults.
- `f4651465` pinned Python dependencies and fixed crosscheck buffering. The analogous scratch-file writer in `scatter_one_file()` still uses default buffering and has a TODO for this work.
- `6eafbe1c` made Shin-Uchuu documentation/configuration standalone. `a518bb0f` renamed processing selectors to `horizontal`/`vertical` and the reader to `horizontal_hdf5`, without changing v2 bytes. `20a0db12` removed a dead test.
- The pathway records Shin-Uchuu's v2 production conversion, science acceptance and retirement of v1 artifacts as complete. Snapshot-global modules, distributed processing, performance, emulator, coupled rates, model building and optional embedding remain future work. Chunked slab streaming is a separate, still-needed runtime project.

**Fresh baseline:** `mimic_venv/bin/python -m unittest discover -s scripts/convert/tests` passed **609 tests**, exit **0**, in **68.735 s** unittest time (72.20 s wall). The delegated log is `/tmp/mimic-converter-tests-20260922.log`, a local diagnostic artifact, not a durable source for future scientific claims. No full C/runtime suite was run for this documentation task. `make info` reports default `MODEL=sage16 SIMULATION=mini-millennium`, HDF5 available and MPI disabled.

### Verified converter and reader boundaries

| Boundary | Current fact |
|---|---|
| ASCII parser | `parse_header_line()`/`resolve_columns()` already match names, case-insensitively, in indexed and `#fields:` headers. Required column selection/types are fixed; column positions are not. |
| Scratch | `RECORD_DTYPE` has 20 fields / 108 bytes: 18 retained source columns and two attributed fields. `scale` is an additional parsed-but-not-retained column, making 19 parsed source columns. |
| State | `scatter.Manifest` version 2 binds source inventory, metadata hashes, artifact checksums and dtype tags. Batch/release/finalize and consumed intermediates make schema identity essential to restart safety. |
| Fixups/links | Fixed records are 120 bytes; separate links records are 36 bytes. The external rank sort already avoids total-catalog resident arrays, but other work still scales with the largest snapshot. |
| Writer/validator | `hdf5_writer.py` and `validate.py` enforce a fixed v2 object set, core types and adjacency. |
| C consumer | `read_horizontal_hdf5.c` rejects extra datasets and unsupported versions before generated field population. `horizontal_driver.c` retains only two generations and explicitly refuses slabs above `INT_MAX`. |
| L-Halo | `binary.c` reads the per-file tree-count header and native binary records, then uses stored local links without ctrees host reconstruction. Shipped L-Halo layouts are 104 bytes, with mass in `1e10 Msun/h`, supplied Len, and already-specific Spin. |
| Forests-HDF5 | `read_ctrees_hdf5.c` reads `ForestInfo` and `Forests/*`, preserving stored links and forest row order; it applies only the shared ctrees value conventions afterwards. |

The concept note correctly identifies hardcoded selection, but its proposed remedy is too narrow for the clarified task. This now needs input adapters, a general topology representation, wider links, and a distinct stable row identity. It is not a matter of adding column names to the existing ASCII dtype.

### Source coverage and the two hard constraints

| Requested simulation | Selected input | Repository evidence / acceptance source |
|---|---|---|
| mini-Millennium | `lhalo_binary` | Eight `trees_063.*` files; real data available locally |
| Millennium | `lhalo_binary` | 512 configured files; only files 0–15 are present locally |
| micro-Uchuu | `lhalo_binary`, plus existing ASCII regression | Four binary files; real binary/ASCII datasets available; forests-HDF5 fixture also available |
| mini-Uchuu | `lhalo_binary` | 128 configured files; only files 0–15 are present locally; README identifies binary as preferred |
| full Uchuu | `consistent_trees_hdf5` | Existing 2000-file forests-HDF5 package; committed external-link fixture; production `snapshots/` absent locally |

Three adapters suffice: retain Consistent-Trees ASCII, add L-Halo binary, add Consistent-Trees forests-HDF5. Do not require downloading or manufacturing another representation of full Uchuu merely to share an adapter. L-Halo HDF5 is not used by the named packages and is not required in this iteration.

**Measured skipped links:** a read-only scan of all eight local mini-Millennium binary files, with record size and complete file length checked, found **29,585 trees, 1,533,122 halos, 1,495,274 non-null descendant links, and 29,291 links spanning more than one snapshot; maximum span 2**. Thus an adjacency-only converter cannot satisfy even mini-Millennium. Rejecting those valid links, dropping halos, or inventing phantom halos would not fulfil this request.

**Full Uchuu width:** its package README records approximately 181.5 billion halos over 50 snapshots. If those recorded totals hold, at least one snapshot necessarily contains at least approximately 3.63 billion halos, above `INT32_MAX`. This is arithmetic from documented totals, not a fresh scan of the unavailable production data. The format and converter must support int64 snapshot-local links. Merely changing an HDF5 dtype while retaining int32 intermediate arrays is insufficient.

**The widening does not depend on confirming that arithmetic, and no slice can confirm it.** Full Uchuu's production data is not mounted here, so Slice 1 will report its absence rather than resolve it, and the 181.5 billion figure stays an inference from a package README throughout execution. The int64 requirement stands regardless: v3 is defined as the general lossless format, and a format that cannot represent a snapshot above `INT32_MAX` carries a ceiling defect independent of which simulation first meets it. A PM who cannot verify the README's totals should record that limitation and proceed — not re-open C3, and not treat the unverified figure as licence to narrow the links back.

### Resource envelope: what v3 costs, and what that rules out

Losslessness is not free, and the plan should say what it costs before a PM starts a conversion that does not fit. V2 emits **exactly 100 B/halo** for its core field set — five `int32` links, `Len`, `SnapNum`, `M_Crit200`, three `float32[3]` vectors, `VelDisp`, `Vmax` and three `int64` identity fields — which is why Shin-Uchuu's production dataset measured 100.01 B/halo. C3's v3 layout widens the five links to `int64` (+20 B), adds three `int32` target-snapshot columns (+12 B) and an `int64 SourceHaloID` (+8 B): **≈140 B/halo, +40%**, before any declaratively selected extra field.

| Target | Halos | v3 output at ≈140 B/halo | Status |
|---|---|---|---|
| mini-Millennium | 1,533,122 (measured, all 8 local files) | ≈0.21 GB | fits trivially |
| micro-Uchuu | 22,580,924 (measured, ASCII default) | ≈3.2 GB | fits trivially |
| Millennium | unmeasured; 16 of 512 files local | fill from Slice 1 | partial source only |
| mini-Uchuu | unmeasured; 16 of 128 files local | fill from Slice 1 | partial source only |
| full Uchuu | ≈181.5 × 10⁹ (package README) | **≈25.4 TB** | see below |

**Full Uchuu is storage-bound, not merely unmounted.** At ≈140 B/halo its emitted dataset is ≈25.4 TB, and at the converter's measured peak-workdir envelope of 192.99 B/halo with consumptive deletion enabled (`scripts/convert/README.md` → storage envelope) its peak working set is ≥35 TB — v3's wider scratch and link records push that higher still. Local capacity of record is **7.71 + 3.00 = 10.71 TB** with a 7.0 TB working ceiling (`SHIN-UCHUU-CONVERSION-PLAN.md`). A full-Uchuu conversion is therefore short by roughly 3.5× on output alone, on hardware, independently of whether the production `snapshots/` mount ever appears. This does not change any slice: Slice 10's full-Uchuu evidence is fixture-based by design and says so. What it changes is the reading of "done" — **completing all eleven slices delivers a full-Uchuu conversion *route*, not a converted full Uchuu**, and no later slice or runtime follow-on should be read as bringing one within reach without a storage plan that does not exist today.

**Obligation on the PM.** Slice 1 already produces per-source halo counts; fill the two unmeasured rows above from its report rather than leaving them blank, and re-derive the whole table from the *measured* v3 B/halo once Slice 8 or Slice 10 emits real files. Per [Adjusting the plan as facts change](#adjusting-the-plan-as-facts-change) category 3, that replacement is expected work, not a deviation.

## Goals and completion boundary

1. Provide conversion paths for all five named simulations using their shipped source formats.
2. Select additional numeric fields declaratively, retaining their precision, native units and halo association.
3. Preserve source-defined topology, chain order, forest enumeration, within-forest row identity and scientific values. Apply ctrees-only conventions only to ctrees inputs.
4. Support skipped-snapshot links and snapshot populations exceeding int32 in the new output contract.
5. Preserve the existing ASCII-to-v2 workflow, existing valid v2 datasets, and the permanent rejection of defective v1 data.
6. Retain reproducible provenance, safe restart, bounded bulk processing and independent producer validation.

**Conversion versus execution is an explicit boundary.** This plan freezes the converter work, including lossless files for gapped and wide inputs. The current runtime cannot execute those files unchanged. Reader/driver support for cross-snapshot pending state and wide indices is a separate architectural prerequisite for running them, not a hidden small change to the converter. The final slice produces a concrete runtime follow-on plan from the implemented format. Do not call the result “runnable for all five simulations” until that follow-on is implemented and its parity gates pass. This conversion-first boundary is the proposed sequencing; if the owner selects combined conversion/runtime implementation, amend and approve the plan before Mode B starts, rather than letting PM invent runtime scope.

No production-output replacement/reconversion campaign, remote transfer, source deletion, production symlink replacement, top-up of existing outputs, physics change, synthetic halo insertion, new dependency, or baseline regeneration is authorised. Fresh acceptance conversions from read-only real sources into isolated disposable workdirs are authorised at the populations specified in Slice 10; they never replace production outputs. A missing full-Uchuu production mount limits production evidence, not fixture-tested adapter capability; report those separately.

## Frozen architecture and contracts

### C1 — Adapter-owned scientific semantics

Introduce small converter-local adapters under `scripts/convert/adapters/`, selected by an explicit source-format key. Their common output is chunked canonical records plus source-keyed topology, not fake ctrees ASCII.

A canonical halo has source identity `(source_file_ordinal, unit_ordinal, row_ordinal)`, snapshot number, original catalog identifiers, core physical values, selected extra values, and the five topology relationships. Adapter inventory defines a deterministic total order. Assign a distinct positive int64 `SourceHaloID` by prefix-summing source halo counts in that order. It is not MostBoundID and is never used to replace a physical particle/catalog identifier. Abort on int64 overflow.

`ForestIndex` and `HaloRankInForest` retain the selected vertical reader's identity convention: for L-Halo, the file-prefix tree number and original within-tree row index; for forests-HDF5, file-prefix ForestInfo row number and original within-forest row index; for ASCII, the existing dense forest-id enumeration and post-fixup reference ordering. Identity is relative to the selected source representation. Do not promise identical UniqueGalaxyID across L-Halo and ctrees forest packaging, which can enumerate different forest sets.

Inventory scope is explicit: preserve numeric source-file ordering and record the selected file range and unit selection. Reference and converter comparisons use the same inventory, `first_file`/`last_file`, snapshot list and particle mass. For sampled trees, retain the parent inventory's prefix counts and original unit ordinals; do not compact sample identities and compare them to unsampled references. Reject missing requested files rather than silently narrowing the conversion. Identical leading subsets preserve their existing prefix identities; changing earlier inventory members can change later ForestIndex values.

- **ASCII:** retain current marker attribution, header dialects, adjacency validation, host fixups, float64-to-float32 parse boundaries, spin J/Mvir normalization, Len rounding, and exact reference chain order. No resurrection of fix_flybys.
- **L-Halo binary:** preserve all five stored local links and their order, supplied Len, native float32 mass in `1e10 Msun/h`, Spin already expressed as specific angular momentum, and signed int64 MostBoundID as data. Do not derive Len, renormalise Spin, apply ctrees host fixups, assume particle identifiers unique, or sort the source tree to derive identity. Negative mass sentinels remain data under the existing core policy, not parse errors.
- **Forests-HDF5:** follow current C reader's file/ForestInfo enumeration, forest-local link interpretation, numeric cast order, integer or integral-float snapshot handling, and shared value conventions. Support the full-Uchuu external-link organisation and the micro-Uchuu FileN layout, including VDS-backed datasets. Verify referenced source availability rather than allowing missing virtual sources to become plausible zero-filled data.

Reject invalid headers, extent/count disagreement, out-of-unit links, cross-forest topology, non-forward descendants, inconsistent progenitor round trips, cycles and invalid FoF membership. **A valid forward gap is not malformed.** FoF links remain same-snapshot. Preserve actual chains instead of regenerating them by mass for already-linked inputs.

### C2 — Metadata and mapping

Use explicit `--source-format`, `--simulation-info`, `--a-list`, source inventory, and optional `--column-map`. A new generic entry point is `scripts/convert/convert_trees.py`; existing `convert_ctrees.py` invocations remain supported, defaulting to the unchanged ASCII/v2 path.

A profile has exactly `schema_version: 1`, `source_format`, `required_columns`, `extra_fields`, and, for binary profiles only, `binary_layout`. `source_format` is `consistent_trees_ascii`, `consistent_trees_hdf5`, or `lhalo_binary`. `required_columns` maps the adapter's fixed canonical input names to nonempty alias lists: ASCII uses the current parser's required names with `snap` as the snapshot role; forests-HDF5 uses the five stored link names, `Mvir`, `x/y/z`, `vx/vy/vz`, `Jx/Jy/Jz`, `vrms`, `vmax`, `id`, and `snap`; binary uses the five link names, `Len`, `M_Crit200`, `Pos`, `Vel`, `Spin`, `VelDisp`, `Vmax`, `MostBoundID`, and `SnapNum`. Structural metadata such as tree headers and ForestInfo is adapter-owned and cannot be remapped or omitted. Alias matching is suffix-stripped/case-insensitive for ASCII and exact for HDF5/binary names. Exactly one alias must resolve for each role in each file; a selected field must exist, with no guessed defaults or silent fills.

Extras declare output name, source component(s), type, units, h convention and description. Types are `int`, `long long`, `float`, `double`, `vec3_int`, `vec3_float`; sources number one or three accordingly. Names are ASCII `[A-Za-z][A-Za-z0-9_]*`, maximum 63 bytes; reject duplicates and collisions with reserved topology/identity/core names. Reject unknown/duplicate YAML keys, unsupported types, ambiguous aliases and malformed definitions. Integers never pass through floating point; test exact values above 2^53 and all range boundaries. Float parsing/casts reject non-finite input and overflow; preserve signed zero. Finite underflow follows the declared NumPy cast. No expression language or inferred unit conversion.

The exact extra-entry keys are `name`, `sources`, `type`, `units`, `h_convention`, and `description`. Each source component is `{field: <name>, component: <0|1|2>}` for an element of a stored vector, or `{field: <name>}` for a scalar; `sources` contains one entry for scalar output and three for vector output. This handles native binary vectors without pretending they are independent columns. `units` and `description` are nonempty strings; `h_convention` is `carried`, `free`, or `none`. Reusing a source field for an extra is allowed and must preserve its pre-convention value. Binary layout keys are `byte_order` (`little` or `big`), `itemsize` and `offsets` (source field name to byte offset); every source field is covered and checked against the ordered property types, with no overlaps or out-of-record extents.

For binary inputs, load the ordered source `halo_properties.yaml` plus an explicit binary-layout profile: endianness, itemsize and offsets must be validated, not guessed from host architecture. The shipped profile is the observed 104-byte L-Halo layout with fixed-width scalars and vectors. Verify full header/count/payload byte length and test opposite-endian fixtures. Extra selection does not alter the on-disk source stride.

Canonical serialization uses sorted-key compact UTF-8 JSON, no NaN, canonical normalized aliases, stable component order and sorted output definitions, hashed with SHA-256. Freeze adapter, complete mapping, source layout, unit conventions, snapshot list identity and source inventory in the manifest. Comments/key ordering/extra-definition ordering are not semantic changes; field types, sources, units or conventions are.

### C3 — Horizontal format v3

The following is the proposed producer contract, subject to the Slice 2 consumer-design approval gate before becoming normative. That gate must trace each proposed field, unit, ordering and qualified link through the generated input view, gap-state ownership and future bounded-slab access. It must include an independently constructed mini-Millennium-like mixed-gap graph and a native-unit consumer metadata example, without implementing runtime changes. If the review requires a semantic schema change, stop and amend this plan before implementation continues; do not silently freeze an unconsumable format. No production v3 conversion is authorised by this plan.

Keep v2 exact and supported by the existing workflow. Define v3 as the lossless general format; never label extended/gapped/wide files v2. Use v3 by default for the generic CLI. V1 remains rejected. A dataset never mixes versions.

One file per a_list snapshot, including empty snapshots, still named `snapshot_NNN.h5`, plus `forests.h5`. V3 has exactly `/header`, `/halos`, `/schema`. Existing physical header values retain their units. `links_adjacent` is int32 0 or 1, measured across the entire dataset and identical in every file; 1 asserts all non-null links are adjacent. Add scalar fixed ASCII `source_format` (32 bytes) and `column_mapping_sha256` (64 lowercase hex bytes).

V3 topology under `/halos`:

- `Descendant`, `FirstProgenitor`, `NextProgenitor`, `FirstHaloInFOFgroup`, `NextHaloInFOFgroup`: int64 snapshot-local row indices, -1 null where allowed.
- `DescendantSnapshot`, `FirstProgenitorSnapshot`, `NextProgenitorSnapshot`: int32 target snapshot, -1 if and only if the corresponding row index is -1. FirstProgenitor points backwards; Descendant forwards; NextProgenitor can target a different earlier snapshot than its sibling. All siblings name the same descendant.
- FoF links stay in the current snapshot; FirstHaloInFOFgroup is never null and self-references for a central.
- `SourceHaloID`, `ForestIndex`, `HaloRankInForest`: int64; SourceHaloID positive and globally unique, the identity pair unique/dense by the adapter's declared source order.
- `SnapNum`: int32; `Len`: int32; no negative Len. Retain MostBoundID as the original signed int64 catalog/particle identifier, without uniqueness or positivity requirements in the general format.

Rows are sorted by ascending SourceHaloID within a snapshot; output topology is remapped, never reordered semantically. `M_Crit200`, Pos, Vel, Spin, VelDisp, Vmax and selected extras retain adapter-declared storage precision and units. In particular L-Halo mass stays float32 in `1e10 Msun/h`; converting it through float32 Msun/h and back would destroy bit parity.

This deliberately differs from legacy v2's MostBoundID row ordering: duplicate/signed particle identifiers are not valid general remapping keys. SourceHaloID order is a deterministic storage order, not progenitor priority. Snapshot-qualified descendants retain lossless gap topology even though today's driver does not consume descendants. `/schema` records the emitted file's interpretation; it does not replace compiled `halo_properties.yaml`. Reports emit a metadata fragment for the payload types/units/core-role bindings a future consumer requires, explicitly incomplete for runtime topology support. The consumer-design gate checks this fragment against the property generator's vocabulary.

`/schema` has one subgroup for every physical/catalog payload field (including Len, SnapNum and MostBoundID), each with exactly scalar variable-length UTF-8 attributes `type`, `units`, `h_convention`, `description`; no children. Topology and the three identity arrays are governed by the fixed format table and are not redeclared. Reject missing/extra declarations and type/shape disagreement. Schema, source format and mapping digest are identical across every snapshot. Extra output objects cannot be external/soft links.

Dataset types are explicit little-endian, uncompressed, chunked at 65536 scalar rows or [65536,3] for vectors. Validators handle empty arrays and use bounded reads. The forest sidecar carries source provenance as int64 arrays `ForestID`, `SourceFileOrdinal`, `SourceUnitOrdinal`, all length n_forests_total. For ASCII forests spanning files, the two ordinal arrays use -1 and ForestID carries the original forest id; the manifest retains the full source membership. For binary, ForestID is the dense run forest number; file/unit ordinals disambiguate it. For forests-HDF5 retain the source ForestID, even if only file-local unique.

Changing an envelope rule requires another format bump; adding a new conforming declared field does not. The v3 specification must explicitly replace v2's ctrees-specific adjacency, MostBoundID ordering and fixed-unit assumptions **only for v3**, not silently loosen v2 validation.

### C4 — Bounded transpose and restart

Adapters stream source chunks; a single super-forest must not require a whole-forest Python object graph. Spool source-key relationships and field values; use bounded external sort/merge to construct the source-key → (snapshot,row) mapping and resolve all five links. Prelinked formats retain their source chains. ASCII topology reconstruction remains its own adapter-specific preparation and retains its current per-snapshot bounds; the new prelinked path must not inherit ASCII's int32 or largest-forest restriction.

No catalog-sized dictionaries/arrays of ids, halo payloads or links. Rank/identity verification may retain the existing explicitly budgeted bitset/O(forest-count) terms, but report them and fail before allocation if they exceed the configured budget; use external verification when necessary for the prelinked large-catalog path. The generic `--memory-budget-mb` bounds merge/chunk working buffers, not the entire Python interpreter RSS. Record actual widths in chunk arithmetic; wider extras reduce rows per chunk. Scratch writer buffering is explicitly 8192 bytes.

New generic manifests use version 3, embed the full schema/digest/adapter identity, dtype descriptors with shapes, deterministic source inventory including referenced HDF5 files, content evidence, stage state and artifact hashes. Resume derives its schema from that manifest, not a mutable profile file. Reject same-width schema substitutions before mutation. Retain legacy v2 manifest resume as the exact implicit ASCII-default schema; never invent recorded provenance for old data. V1/unknown manifests fail.

Keep `scatter.Manifest` as the legacy state owner; `conversion_manifest.py` owns only the new adapter-neutral stage state. Reuse its proven hashing/atomic-write/containment helpers where their contracts fit, without making a second legacy state machine or changing historical release semantics. Likewise reuse bounded sort primitives from `rank_sort.py` where compatible; do not build a general plugin or sorting framework merely to host three adapters.

General commands: `inspect` (read-only capability/count/layout report), `ingest`, `transpose`, `write`, `validate`, `report`. Stage transitions are explicit and resume-safe. Keep existing ASCII scatter/release/finalize and consumptive behaviour unchanged; do not extend source release to the new adapters this round. Generic source files are always read-only. Generic scratch cleanup is opt-in, manifest-owned and verify-before-consume; tests use disposable fixtures, never user data.

### C5 — Evidence and support claims

Compare conversion against the **selected format's** vertical interpretation, not against a different source format assumed equivalent. Use source coordinates/SourceHaloID and (ForestIndex,HaloRankInForest), never MostBoundID alone. Verify every link target and exact chain traversal; compare payload bits at the adapter's declared cast boundary. Independent source extraction and C reader dumps must not share the converter's remapping code.

Real mini-Millennium is the gap-containing acceptance gate. Real micro-Uchuu binary plus ASCII provide new-format and regression anchors. For mini-Uchuu and Millennium, convert deterministic complete trees from available files and record sample coverage; fixture/sample evidence is not a claim of having converted every file. Full Uchuu's external-link fixture is mandatory; full production conversion/validation is explicitly unperformed without its source and resources. Exercise >2^31 indices using virtual/synthetic bounded tests and external-sort joins, without allocating billions of records.

## Execution policy and effort

Mode B runs the following slices in order, fresh session per slice. No batches are recommended. High effort means architectural/scientific/persistence reasoning requiring a strong Developer and independent Reviewer; medium suits a standard strong Developer; low suits routine documentation. PM chooses and records actual models.

| Slice | Deliverable | Effort |
|---|---|---|
| 1 | Source inspection and capability inventory | medium |
| 2 | Canonical schema, identities and v3 specification | high |
| 3 | L-Halo binary adapter | high |
| 4 | Consistent-Trees forests-HDF5 adapter | high |
| 5 | Declarative ASCII fields and adapter bridge | high |
| 6 | Bounded transpose and 64-bit link remapping | high |
| 7 | Generic manifests and restart | high |
| 8 | V3 writer, validator and reports | high |
| 9 | Generic CLI and simulation profiles | medium |
| 10 | Independent real-data acceptance | high |
| 11 | Operating documentation and runtime follow-on plan | medium |

There is no wholly low-effort slice in this expanded scope: the final documentation slice also includes architectural planning. PM may delegate its routine prose portion cheaply, but should retain a stronger Developer for the runtime handoff.

Approval to execute this frozen plan includes its specified converter/format changes, not runtime changes or remote side effects. Commit the planning documents with explicit owner permission before PM starts clean. Stay on the current branch unless told otherwise. Do not amend or bypass hooks. Mode B's launcher provides prospective permission for slice commits when the owner submits it.

All code slices run targeted tests, differential lint, `make tests-converter`, `make check-horizontal-fixture`, `make check-docs`, `make check-format`, and `git diff --check`. Use mimic_venv, capture exit codes, delegate suites longer than a minute, and never run suites concurrently. Before every commit run beautify, reread the diff against STYLE-GUIDE and sweep relevant Mimic skills; record all three results. Do not carry unrelated formatter edits. Generated files are never hand-edited, and no failing test is weakened.

The contracts C1–C5 are binding references, but each slice's acceptance list below also states its essential requirements so a pinned receipt stands alone. The plan is immutable during PM execution in the sense defined by [Adjusting the plan as facts change](#adjusting-the-plan-as-facts-change) below: the PM may not rewrite it, but bounded adjustment inside a slice is expected and must be recorded. New semantic scope or an unresolved contract conflict stops execution; PM cannot author a runtime design or silently change preservation rules.

Slice 2 is an explicit human gate for consumer-design approval; Slice 11 gates changes to agent skill instructions. All other `no` flags assume the owner has already approved the overall plan and conversion-first boundary. A parser success is structural validation, not approval of those decisions.

## Adjusting the plan as facts change

No plan written before the work survives contact with the data unchanged, and this one is no exception: several of its numbers are projections, its adapter descriptions rest on shipped layouts rather than on the full production sources, and two of its target simulations are not mounted here. The failure mode to guard against is **silent drift**, not adjustment. A PM who adjusts and records is doing the job; a PM who adjusts quietly, or who stalls on a plan sentence that the evidence has overtaken, is not.

**The PM may do these without asking, and must record each one in the slice receipt:**

1. **Tactics inside the slice's authorized surface** — file organisation, helper decomposition, naming, test structure, and the order of steps within the slice.
2. **Effort and model assignment**, and splitting an over-large slice into sequential sub-slices that keep the *same* acceptance criteria and the *same* authorized surface. Never merge slices, never move or skip a gate.
3. **Replacing a projected number in this plan with a measured one**, and re-deriving whatever depended on it. This is expected work, not an exception — measurement outranks the plan's own arithmetic every time.
4. **Tightening** — adding tests, adding validation, narrowing a surface, failing earlier or louder than the plan asked.
5. **Reordering slices whose stated dependencies allow it** when a blocked slice would otherwise idle, provided no gate is crossed early and no later slice's evidence is assumed.
6. **Fixing an incidental defect inside the authorized surface** when it genuinely blocks that slice's own acceptance — that defect only, recorded as such, never as a foothold for adjacent cleanup.

**These always stop execution and go to the owner, regardless of how small the change looks:** new semantic scope; any runtime, reader or driver change; anything touching preservation, losslessness or format semantics; a C1–C5 conflict that cannot be resolved by reading the contracts; anything on a slice's Explicit Non-Goals list; remote side effects, production-output replacement or source deletion; baseline regeneration; a new dependency; and the Slice 2 and Slice 11 human gates.

**Amend rather than drift.** If the plan itself is wrong — a contract that cannot be satisfied, an acceptance criterion that the evidence has invalidated, a slice boundary that does not survive the code — stop the slice, state the problem with its evidence in the receipt, and propose the amendment. The owner amends this document; the PM restarts the slice from the amended text. The plan is immutable *to the PM*, not immutable to evidence.

**What the record must support.** Every adjustment gets one line in the slice receipt: what changed, why, what evidence forced it, and which category above it falls under. A reviewer must be able to reconstruct the delta between plan-as-written and work-as-done from the receipts alone, without reading the diff.

**A worked example of category 3, from this repository.** The `links` stage's per-snapshot memory window was recorded as ≈225–235 GB — a two-point extrapolation fitted against an *assumed* production largest slab of ≈3.546 × 10⁸ halos. The production report then measured the real largest slab at 519,342,987, 46.5% higher, which on the same fit gives ≈326–342 GB. The instruction to re-derive it was written down and never discharged, and the unrefreshed figure was later quoted in a planning note as though it had been measured. Both halves of that are the thing to avoid: leave a projection sitting long enough and it acquires the authority of a measurement. When this plan's numbers meet real data, replace them and say so.

## Slice 1: Source inspection and reproducible capability inventory

**Effort: medium.** No dependencies.

### Intended Change

- Implement read-only source inspection for the three selected adapters and commit a capability report with reproducible counts/layout/gap evidence.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Inputs: explicit source format, simulation metadata, a_list and inventory; binary layout is the shipped 104-byte record with explicit endianness, not a guessed NumPy packing.
- [ ] Outputs: per-source/per-snapshot counts, link-span summary, identity bounds, available fields/types/units, source dependencies and resource estimates; inspection never writes into source directories.
- [ ] Reproduce mini-Millennium's 1,533,122 halos and 29,291 forward gaps across eight files, maximum span 2, or report a source-identity discrepancy rather than change expectations.
- [ ] All five requested packages have a declared adapter route; absent full-Uchuu production data is reported separately from its available external-link fixture.
- [ ] Invalid/truncated headers, bad count totals and missing HDF5 dependencies fail; a valid skipped-snapshot link is counted, not rejected.
- [ ] Existing ASCII behaviour and all runtime code remain unchanged.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/inspect_sources.py`
  - `scripts/convert/adapters/source_inventory.py`
  - `scripts/convert/tests/test_inspect_sources.py`
  - `scripts/convert/tests/data/source_formats/`
  - `docs/dev/MIMIC-CONVERTER-SOURCE-INVENTORY.md`
- Functions/classes/components allowed to change: read-only inventory/layout/capability helpers.
- Tests allowed or expected to change: malformed-file and count/gap reporting tests.

### Explicit Non-Goals

- No conversion, runtime design changes, remote access or production file rewrite.

### Risk Flags

- Risky surfaces touched: untrusted binary/HDF5 input, read-only.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Run the new test module and all common code-slice gates; inspect real mini-Millennium and the full-Uchuu fixture.
- Lint (differential, via the `lint` skill): required.
- Manual checks: report names exact source files, byte order, layout and inspected population; unavailable production data is not marked PASS.

### Rollback Path

- Revert the inspection addition in a new authorised commit; retain evidence in archive. No source state changed.

## Slice 2: Canonical source schema, identities and mapping profiles

**Effort: high.** Dependency: 1.

### Intended Change

- Implement pure canonical record/topology definitions, adapter contracts, mapping validation and schema identity. Publish the v3 specification as a producer contract; runtime support remains explicitly pending.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Inputs: adapter-specific required roles and selected scalar/vec3 numeric extras; duplicate/unknown keys, reserved names, invalid component counts and unsupported types fail.
- [ ] Outputs: immutable canonical schema, deterministic SHA-256 identity, explicit widths/shapes/units and source-coordinate identities; harmless YAML presentation changes preserve identity.
- [ ] Distinguish SourceHaloID from MostBoundID; preserve signed/duplicate particle identifiers without using them as unique remapping keys.
- [ ] Specify all five int64 links, three target-snapshot columns, source-relative forest/rank identity, and native payload units exactly as C1–C3.
- [ ] Binary source schema preserves full ordered layout even when fields are not selected; supported endianness/offset/itemsize is explicit.
- [ ] V3 specification keeps v2 rules exact and v1 rejected; it neither claims current runtime support nor permits phantom insertion or gap removal.
- [ ] Before publishing v3 as normative, record owner approval of a consumer-design review covering mixed-snapshot chains, SourceHaloID ordering, native payload units/metadata, generated input views, retained gap-state ownership and wide/chunked access. Include a worked mixed-gap graph and consumer metadata fragment; any required semantic change stops for a plan amendment, not a silent spec edit.
- [ ] No existing pipeline changes its results in this slice.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/column_schema.py`
  - `scripts/convert/adapters/base.py`
  - `scripts/convert/profiles/`
  - `scripts/convert/tests/test_column_schema.py`
  - `scripts/convert/tests/test_adapter_contract.py`
  - `scripts/convert/tests/data/column_maps/`
  - `docs/dev/HORIZONTAL-HDF5-FORMAT.md`
  - `docs/dev/MIMIC-V3-CONSUMER-DESIGN-REVIEW.md`
- Functions/classes/components allowed to change: pure schemas, serializers, profile loader, canonical batch/link contracts.
- Tests allowed or expected to change: schema, bounds and source identity tests.

### Explicit Non-Goals

- No generic converter execution, C consumer change or new dependency.

### Risk Flags

- Risky surfaces touched: public format/schema and scientific preservation contract.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Test every supported scalar/vector type, invalid profiles, deterministic identity, same-width schema differences, and values/indices above 2^31 and 2^53.
- Run common gates; inspect normative v2/v3 sections for contradictions.
- Lint (differential, via the `lint` skill): required.
- Manual checks: mapping/schema examples are complete and runnable through the pure loader.

### Rollback Path

- Revert in a new authorised commit before any producer relies on v3; do not change the meaning of already emitted data.

## Slice 3: Lossless L-Halo binary adapter

**Effort: high.** Dependencies: 1–2.

### Intended Change

- Stream fixed-record L-Halo binary trees with their count header (`Ntrees`, total halo count, then one count per tree) into canonical batches with stored topology and source-relative identities.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Inputs: the binary layouts used by mini/micro-Uchuu and both Millennium packages; explicit byte order, header/count/payload lengths and fixed offsets are validated.
- [ ] Outputs preserve supplied Len, float32 native mass, already-normalised Spin, MostBoundID, source row order/rank and all five source chains; no ctrees fixups or mass round trip is applied.
- [ ] Forward gaps survive as exact source-key edges; non-forward/out-of-tree links, cycles, inconsistent reciprocal chains and invalid FoF membership fail.
- [ ] SourceHaloID is deterministic from complete ordered inventory, distinct from catalog identifiers; file-prefix tree numbers match vertical identity enumeration.
- [ ] Selected extra binary fields survive with declared precision; unselected fields remain part of source stride validation.
- [ ] Reads are chunked even for a forest larger than the configured chunk; no whole-catalog or mandatory whole-forest object array.
- [ ] Existing ASCII conversion and runtime are unchanged.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/adapters/lhalo_binary.py`
  - `scripts/convert/adapters/source_inventory.py`
  - `scripts/convert/tests/test_lhalo_adapter.py`
  - `scripts/convert/tests/data/source_formats/`
- Functions/classes/components allowed to change: binary adapter and shared read-only inventory refinements within the frozen contract.
- Tests allowed or expected to change: endian/layout, chunk-boundary, source-link and value-preservation cases.

### Explicit Non-Goals

- No L-Halo HDF5 adapter, source repair, invented intermediate halos or runtime integration.

### Risk Flags

- Risky surfaces touched: binary layout, scientific values and source identity.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Test zero-tree/empty-file cases, truncation/trailing bytes, opposite endian, multi-file identities, duplicate particle ids, mass sentinels and mixed-span progenitor chains.
- Independently compare selected complete real mini-Millennium trees; run common gates.
- Use direct structured binary extraction with hand-declared expected values/edges as the pre-writer oracle, not the adapter's own schema/remap helpers. The generalised C dump is added in Slice 10, not assumed available here.
- Lint (differential, via the `lint` skill): required.
- Manual checks: verify emitted values against direct binary reads, not adapter helper calls.

### Rollback Path

- Revert the unused adapter in a new authorised commit; preserve source data and test evidence.

## Slice 4: Consistent-Trees forests-HDF5 adapter

**Effort: high.** Dependencies: 1–2.

### Intended Change

- Support the existing full-Uchuu external-link and micro-Uchuu forests-HDF5 organisations without an ASCII intermediate.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Inputs: FileN/ForestInfo/Forests data, external-link or VDS-backed fields, integer or integral float snapshot columns; missing referenced sources fail before accepting rows.
- [ ] Outputs preserve file-prefix ForestInfo enumeration, original forest row ranks and stored link/chain order, using forest-local references exactly as the C reader.
- [ ] Apply ctrees mass/spin/Len cast and arithmetic conventions in the same order as the current vertical HDF5 path, not the L-Halo convention.
- [ ] Keep all offsets/counts/remapping keys int64 and reads bounded; a super-forest does not force a whole-forest allocation.
- [ ] Validate extent/offset/count agreement, integral snapshots, source links and selected numeric fields; additional fields are declarative.
- [ ] Pin all physical HDF5 backing dependencies in inventory/provenance; absent virtual source data cannot silently pass as fill values.
- [ ] Both committed source-layout fixtures pass; full-Uchuu production-scale validation remains explicitly unperformed until its source exists.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/adapters/ctrees_hdf5.py`
  - `scripts/convert/adapters/source_inventory.py`
  - `scripts/convert/tests/test_ctrees_hdf5_adapter.py`
  - `scripts/convert/tests/data/source_formats/`
- Functions/classes/components allowed to change: HDF5 adapter and source-dependency inspection.
- Tests allowed or expected to change: forest-offset, VDS/external-link, dtype/cast and invalid-reference fixtures.

### Explicit Non-Goals

- No remote fetch, whole full-Uchuu conversion, vertical reader rewrite or reconstruction of already-stored links.

### Risk Flags

- Risky surfaces touched: HDF5 dependency graphs, offsets, precision and source topology.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Test both committed fixtures, missing backing files, nonintegral float snapshots, nonzero forest offsets, chunk splits within forests and links above int32 using sparse fixtures.
- Compare convention results against independently specified expected values; run common gates.
- Name the existing fixture inputs explicitly: `simulations/micro-uchuu-hdf5/_tests/data/MicroUchuu_test_mergertree_info.h5` and `simulations/uchuu/_tests/data/mergertree_info.h5`. Read expected source fields/links independently of adapter helpers; fixture existence alone does not prove its dependency graph was tested.
- Lint (differential, via the `lint` skill): required.
- Manual checks: confirm field aliases/layout come from actual fixtures and reader code rather than inferred names.

### Rollback Path

- Revert adapter integration in a new authorised commit; no source file or existing output changes.

## Slice 5: Declarative ASCII fields and canonical adapter bridge

**Effort: high.** Dependency: 2.

### Intended Change

- Generalise ASCII selected columns, preserve extra fields through existing topology preparation, and expose its results through the canonical adapter while retaining the v2 route.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Both existing header dialects, alias ambiguity rejection, independent row pre-count and tree-marker attribution remain.
- [ ] Default records/core output are bit-identical; float64 parse then existing float32 rounding, Len/spin arithmetic, host fixups, tie order and multiple surviving FoF centrals are preserved.
- [ ] Arbitrary declared numeric extras survive parsing, sorting and fixed-record copying; raw J extras are not normalised with core Spin.
- [ ] Integers above 2^53 remain exact, fractions/overflow and non-finite floats fail with row/column context.
- [ ] Canonical source keys and existing forest/rank identity are both retained; ASCII still rejects nonadjacent source descendants rather than inventing a new ctrees prescription.
- [ ] Extended scratch dtypes and memory arithmetic use actual itemsize; separate rank/link data do not acquire payload fields.
- [ ] Scratch file handles use explicit 8192-byte buffering, including pooled execution.
- [ ] The legacy CLI remains operational and no extended data can be silently written as v2.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/ctrees_parser.py`
  - `scripts/convert/adapters/ctrees_ascii.py`
  - `scripts/convert/scatter.py`
  - `scripts/convert/sort_index.py`
  - `scripts/convert/fixups.py`
  - `scripts/convert/links.py`
  - `scripts/convert/hdf5_writer.py`
  - `scripts/convert/tests/test_parser.py`
  - `scripts/convert/tests/test_scatter.py`
  - `scripts/convert/tests/test_sort_index.py`
  - `scripts/convert/tests/test_fixups.py`
  - `scripts/convert/tests/test_links.py`
  - `scripts/convert/tests/test_hdf5_writer.py`
  - `scripts/convert/tests/test_ascii_adapter.py`
  - `scripts/convert/tests/fixtures.py`
  - `scripts/convert/tests/data/column_maps/`
- Functions/classes/components allowed to change: parser selection/dtypes, extra propagation and canonical bridge; writer only to reject incompatible extensions until v3 emission exists.
- Tests allowed or expected to change: parser, stage and adapter regressions.

### Explicit Non-Goals

- No topology algorithm replacement, new header grammar, source transfer change or generic runtime support.

### Risk Flags

- Risky surfaces touched: established converter path, precision, memory accounting and multiprocessing.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Run stage tests and all common gates; compare serial/pooled records and zero-extra output to pre-change reference.
- Test extra fields across chunk/tree boundaries, raw-versus-normalised J, wide schemas and resumed default runs.
- Use literal extra-column expectations and the pre-change default pipeline as independent oracles; v3 physical row order differs from v2, so compare values/links by source identity, not equal output row numbers.
- Lint (differential, via the `lint` skill): required.
- Manual checks: retain 108/120/36-byte default layouts and existing dtype tags for legacy compatibility.

### Rollback Path

- Preserve extended scratch artifacts; old code must reject them. Revert in a new authorised commit; never relabel records to make a legacy resume work.

## Slice 6: Bounded transpose and 64-bit topology remapping

**Effort: high.** Dependencies: 3–5.

### Intended Change

- Implement common snapshot partition/sort and source-key joins for canonical adapter data, preserving mixed-snapshot chains.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Inputs: canonical payloads and five source-key relationships; output rows are ascending SourceHaloID per snapshot.
- [ ] Resolve all links to int64 row plus target snapshot where required, preserving exact progenitor/FoF chain order and source forest/rank identities.
- [ ] Gaps, mixed-age sibling progenitors, empty intermediate snapshots, early-ending trees and duplicate MostBoundID values survive correctly.
- [ ] All link arithmetic, sorting offsets and counts remain int64 beyond 2^31; overflow fails before narrowing or writing corrupt output.
- [ ] Bounded external merge obeys the configured working-buffer budget and accounts for actual schema widths; no full-catalog id dictionary or required whole-forest allocation.
- [ ] Independent closure checks reject missing/cross-forest targets, cycles, duplicate source keys and inconsistent chains.
- [ ] No phantom rows, topology repair, scientific rescaling or new identity enumeration occurs.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/transpose.py`
  - `scripts/convert/source_keys.py`
  - `scripts/convert/rank_sort.py`
  - `scripts/convert/tests/test_transpose.py`
  - `scripts/convert/tests/test_source_keys.py`
  - `scripts/convert/tests/test_rank_sort.py`
- Functions/classes/components allowed to change: bounded remap/sort/join machinery; extend rank_sort only where reusable primitives preserve its existing contracts.
- Tests allowed or expected to change: cross-snapshot topology and bounded-index join tests.

### Explicit Non-Goals

- No writer, CLI, persistence policy or C changes.

### Risk Flags

- Risky surfaces touched: ordering, 64-bit indices, external-memory algorithm and identity.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Independently hand-specify a gapped/mixed-progenitor graph and compare all targets/chains; force multiple external merge passes and tiny budgets.
- Exercise >2^31 row values without billion-row allocations; run common gates.
- Lint (differential, via the `lint` skill): required.
- Manual checks: account for in-memory structures by lifetime and width; distinguish buffer budget from total RSS.

### Rollback Path

- Preserve incomplete scratch, restart from compatible canonical input after an authorised rollback; do not consume its only copy.

## Slice 7: Schema-bound generic manifests and restart

**Effort: high.** Dependency: 6.

### Intended Change

- Add explicit ingest/transpose/write stage state with schema/source dependency binding and verified restart, preserving the legacy manifest path.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] New generic manifests use version 3 with adapter/mapping/layout identity, full dtype descriptors, source inventory/dependencies, stage records and content checksums.
- [ ] Resume uses embedded configuration even if the profile file disappears; explicit same-width-but-different schema or changed source dependencies fail before mutation.
- [ ] Ingest and transpose can resume after forced interruption without duplicated/dropped halos or different SourceHaloID/forest enumeration.
- [ ] Skip-trust paths verify artifacts before accepting them; generic optional cleanup is manifest-contained and verify-before-consume.
- [ ] Source data are always read-only; no release/transfer feature is added for new adapters.
- [ ] Genuine v2 default ASCII manifests remain resumable in their original representation; v1/unknown manifests fail; legacy provenance is not fabricated.
- [ ] Failed operations do not mark stages complete and report the offending source/artifact.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/conversion_manifest.py`
  - `scripts/convert/pipeline.py`
  - `scripts/convert/scatter.py`
  - `scripts/convert/tests/test_conversion_manifest.py`
  - `scripts/convert/tests/test_pipeline.py`
  - `scripts/convert/tests/test_scatter.py`
  - `scripts/convert/tests/data/legacy_manifest_v2/`
- Functions/classes/components allowed to change: generic orchestration/state, legacy compatibility boundary only.
- Tests allowed or expected to change: interruption, dependency drift, checksum and containment cases.

### Explicit Non-Goals

- No source deletion, production-workdir migration, new writer or CLI.

### Risk Flags

- Risky surfaces touched: durable state, crash recovery and cleanup.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Inject failures before/after artifact write, verification and manifest save for each stage; retry and compare exact outputs.
- Capture a genuine pre-change v2 fixture; test schema substitutions and changed HDF5 backing files; run common gates.
- Until the Slice 8 writer lands, use small literal canonical input and independent expected transposed arrays as the stage/restart oracle; test write-state transitions with a deterministic stub, not a premature duplicate writer.
- Lint (differential, via the `lint` skill): required.
- Manual checks: destructive test cases touch only their own temporary fixtures.

### Rollback Path

- Preserve new manifests/workdirs and use compatible code or fresh workdirs after rollback; never downgrade version labels.

## Slice 8: V3 writer, independent producer validation and reports

**Effort: high.** Dependency: 7.

### Intended Change

- Write the frozen v3 schema and validate its topology, fields, identity and provenance with bounded scans.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Emit every a_list snapshot, including empty ones, exact C3 root/header/schema/dataset objects and sidecar; no additional undeclared objects.
- [ ] Preserve source-specific native units and precision, signed MostBoundID, 64-bit links, explicit target snapshots, stable row identity and exact chain order.
- [ ] Measure links_adjacent and run identity bounds; schema/source-format/digest agree across all files.
- [ ] Validate count conservation, source-key coverage, every link target, chain closure/cycles, forest/rank density, field finiteness where required and schema/manifest binding.
- [ ] Write verification includes all payload and topology before success/cleanup; corruption in an extra field is not ignored.
- [ ] V2 production/validation behaviour remains exact; v1/mixed/unknown versions fail. A legacy C reader continues to reject v3 clearly rather than reading it incorrectly.
- [ ] Reports identify source format, actual format version, mapping/layout, link-gap counts, index bounds, resource measurements and runtime compatibility limitations.
- [ ] Report a consumer payload-metadata fragment with native units, types and applicable core roles; label it insufficient to enable runtime execution and do not write simulation packages automatically.
- [ ] Validator tests use independent expected graphs and field values, not only writer-owned schema helpers as their oracle.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/hdf5_writer.py`
  - `scripts/convert/validate.py`
  - `scripts/convert/report.py`
  - `scripts/convert/pipeline.py`
  - `scripts/convert/tests/test_hdf5_writer.py`
  - `scripts/convert/tests/test_validate.py`
  - `scripts/convert/tests/test_report.py`
  - `scripts/convert/tests/test_pipeline.py`
- Functions/classes/components allowed to change: version-specific emission/verification/battery/report and final stage wiring.
- Tests allowed or expected to change: exact-object, schema drift, cross-snapshot link, dtype and corruption cases.

### Explicit Non-Goals

- No C reader/driver change, v2 data rewrite, compression experiment or physics changes.

### Risk Flags

- Risky surfaces touched: persistent public output format and validity claims.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Corrupt each contract component independently, including empty-snapshot schema and target-snapshot mismatch; require nonzero verdicts.
- Run common gates; inspect real emitted HDF5 and human/machine reports. Compare arrays rather than HDF5 container bytes.
- Lint (differential, via the `lint` skill): required.
- Manual checks: report does not claim unsupported runtime executability.

### Rollback Path

- Preserve v3 files and manifests; old software rejects them. Never stamp them v2 as a rollback shortcut.

## Slice 9: Generic CLI and profiles for all requested simulations

**Effort: medium.** Dependency: 8.

### Intended Change

- Expose inspect/ingest/transpose/write/validate/report and package-local conversion profiles for the selected inputs.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] All five requested simulations have an explicit documented route: four L-Halo binary packages and full-Uchuu forests-HDF5; ASCII remains supported.
- [ ] Generic CLI requires explicit format/metadata/inventory and defaults to v3; legacy convert_ctrees commands remain default-v2 compatible.
- [ ] Profiles select source fields/layout rather than changing production run YAML, halo properties or snapshots symlinks.
- [ ] User-defined extra field selection is exercised end to end for binary, HDF5 and ASCII, without hardcoded names in stage logic.
- [ ] Invalid formats/profiles and conflicting resume inputs fail early; no silent source-format inference or profile fallback.
- [ ] CLI output clearly distinguishes successful conversion from current runtime support and identifies nonadjacent/wide output.
- [ ] Generic memory budget and stage restart behaviour are described accurately; no new transfer/release semantics.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/convert_trees.py`
  - `scripts/convert/convert_ctrees.py`
  - `scripts/convert/tests/test_cli.py`
  - `scripts/convert/tests/test_generalisation.py`
  - `simulations/mini-millennium/converter_columns.yaml`
  - `simulations/millennium/converter_columns.yaml`
  - `simulations/micro-uchuu/converter_columns.yaml`
  - `simulations/mini-uchuu/converter_columns.yaml`
  - `simulations/uchuu/converter_columns.yaml`
  - `simulations/micro-uchuu-ascii/converter_columns.yaml`
- Functions/classes/components allowed to change: CLI argument dispatch, profile examples and subprocess integration tests.
- Tests allowed or expected to change: full small-fixture CLI paths, defaults and failures.

### Explicit Non-Goals

- No runtime packages, deployment, production conversion or automatic metadata rewriting.

### Risk Flags

- Risky surfaces touched: public CLI and package-local converter configuration.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Run every new CLI stage on fixtures for all three adapters, from a non-repository working directory as well; run common gates.
- Lint (differential, via the `lint` skill): required.
- Manual checks: inspect help and all six profile files against source metadata.

### Rollback Path

- Revert CLI/profile addition in a new authorised commit; retain outputs and default-v2 usability.

## Slice 10: Independent acceptance on real data and source fixtures

**Effort: high.** Dependency: 9. Long-running gates require delegated execution.

### Intended Change

- Build a repeatable acceptance harness and record evidence for the named source formats, including real gapped input and default-path preservation.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Fresh complete mini-Millennium conversion conserves 1,533,122 halos and all 29,291 skipped descendant links; no extra rows are inserted.
- [ ] Every source link/chain, source-relative identity and selected payload field agrees by source coordinates with independent binary extraction and the vertical reader's interpretation.
- [ ] Fresh micro-Uchuu L-Halo conversion passes equivalent checks; fresh ASCII default-v2 conversion passes the existing 15-check producer battery and eight-check topology crosscheck.
- [ ] ASCII default totals remain 22,580,924 halos, 50 snapshots, 440,651 forests and maximum forest rank 350074, or acceptance stops to investigate.
- [ ] Deterministic complete-tree samples from local mini-Uchuu and Millennium pass; record exact sample inventory and make no full-production claim.
- [ ] Reference and converted identities use identical source inventory/ranges and original source unit ordinals; sampled trees retain parent prefix identities. Missing requested files fail, while deliberately selected available subsets are labelled as subsets.
- [ ] Extend the existing dump harness/build script rather than copying their startup and reader loop. Preserve its default v1 ctrees dump and legacy crosscheck; add a separately versioned source-coordinate/payload mode. Handle per-file readers with count-prefix offsets (L-Halo has no `global_forest_offset` hook) and enumerated readers with their existing hook.
- [ ] Full-Uchuu external-link and micro-Uchuu HDF5 fixtures pass independent C-reader/value/topology comparisons; absent full-Uchuu production data remains an explicit validation limitation, not a fake skip-pass.
- [ ] Generic 64-bit join/validation tests exercise indices above 2^31 and keys above 2^53 with bounded resources.
- [ ] Extras are checked by independent source extraction, not by round-tripping converter helper output. Signed/duplicate particle identifiers and negative mass sentinels are covered synthetically.
- [ ] Record times, peak RSS, spill/storage widths, code/source identities, commands and exit codes; compare default ASCII against the pre-change same-host reference and investigate repeatable regressions over 20%.
- [ ] All production sources, symlinks and baselines remain unchanged. No v3 runtime-parity claim is made before the runtime follow-on.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/tests/run_generalisation_acceptance.py`
  - `scripts/convert/tests/test_generalisation_acceptance.py`
  - `tests/unit/tools/dump_ctrees_topology.c`
  - `tests/unit/tools/build_topology_dump.sh`
  - `docs/dev/MIMIC-CONVERTER-GENERALISATION-ACCEPTANCE.md`
- Functions/classes/components allowed to change: acceptance harness/comparator and the existing independent vertical dump tool/build script. Reuse the existing `dump-ctrees-topology-tool` target; no Makefile edit is needed. The new acceptance harness consumes the new dump mode; existing crosscheck consumes unchanged v1 output.
- Tests allowed or expected to change: comparator/harness failure cases; existing source reader and science predicates remain unchanged.

### Explicit Non-Goals

- No production algorithm fixes hidden in acceptance, tolerance relaxation, baseline refresh or remote conversion.

### Risk Flags

- Risky surfaces touched: scientific evidence tooling and reference builds.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Self-test the comparator against dropped/duplicated rows, wrong target snapshots, reordered chains, changed fields and ignored extra columns.
- Execute acceptance in isolated workdirs; build references with the corresponding MODEL=halos-only/SIMULATION pair, consistently. Use the pre-change commit for default ASCII regression and current vertical readers for source interpretation.
- Delegate long suites, capture logs and inspect all skips; run common gates and a final default `make MODEL=sage16 SIMULATION=mini-millennium tests summary`. Restore default generated state.
- Lint (differential, via the `lint` skill): required.
- Manual checks: report states precisely which real populations and fixtures were tested, and which production data were unavailable.

### Rollback Path

- Preserve failed evidence; return defects to their owning slice/surface. A failed acceptance is not permission to modify source data or relax preservation.

## Slice 11: Document operation and prepare the runtime follow-on

**Effort: medium.** Dependency: 10. Operational documentation is low effort; the runtime requirements handoff needs architectural care.

### Intended Change

- Publish tested converter use and a separate implementation plan for consuming the implemented v3 contract. Update the pathway without presenting pending runtime work as delivered.

### Acceptance Criteria

- [ ] Read and satisfy this plan's C1–C5 contracts and execution policy, including the common validation gates; these are incorporated into this pinned receipt by reference to `docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`. If a referenced requirement conflicts with this slice's surface/non-goals, stop for a plan correction rather than inventing a rule.
- [ ] Document commands and profiles for all five requested simulations, arbitrary numeric selection, native units/precision, source identity, gaps, int64 bounds and manifest restart.
- [ ] State conversion capability, sample/fixture evidence and production evidence separately; v2 remains runnable, and new v3 runtime support is pending.
- [ ] Runtime follow-on explicitly covers generated payload schemas, v3 reader validation, snapshot-qualified progenitor lookup, pending processed/galaxy state across gaps, ownership/lifetime, int64 indices throughout the input/driver seam, and full-Uchuu memory constraints.
- [ ] The follow-on prohibits synthetic gap halos and preserves inheritance source times, source chain order and source-relative UniqueGalaxyID; requires per-ID bitwise vertical/horizontal parity on real gapped mini-Millennium before acceptance.
- [ ] The follow-on distinguishes width from memory: int64 links alone do not make full-Uchuu slabs fit. Reconcile with chunked-slab planning rather than promising a whole-snapshot low-memory run.
- [ ] Freeze the runtime plan only after inspecting the final converter/schema; unresolved architectural decisions are named for owner review, never decided by PM outside this conversion contract.
- [ ] Relevant skills/manuals describe implemented converter facts accurately; this frozen plan is not edited/archived during the PM run.

### Authorized Surface

- Files allowed to change:
  - `scripts/convert/README.md`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-CONVERTER-GENERALISATION-PLAN.md`
  - `docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md`
  - `simulations/mini-millennium/README.md`
  - `simulations/millennium/README.md`
  - `simulations/micro-uchuu/README.md`
  - `simulations/mini-uchuu/README.md`
  - `simulations/uchuu/README.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
- Functions/classes/components allowed to change: documentation and runtime planning only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No C runtime changes, physics changes, unmeasured claims or auto-execution of the follow-on plan.

### Risk Flags

- Risky surfaces touched: future architecture proposal, no runtime mutation.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Use implementation-plan for the runtime follow-on and run its PM check-plan; unresolved design gates must be explicit.
- Run `make check-docs`, `make check-format`, `git diff --check`; verify all command examples against actual help.
- Lint (differential, via the `lint` skill): not required for Markdown-only changes.
- Manual checks: style/skill sweep and no hard-wrapped prose; public documentation never equates conversion with runtime support.

### Rollback Path

- Revert documentation in a new authorised commit while retaining acceptance evidence. Runtime execution requires a separately approved plan.

## Independent plan review — 2026-09-22

Claude Opus 5 at high effort, launched through orchestrator, endorsed the three-adapter/canonical-record design and preservation goals but recommended proving consumption with a gapped mini-Millennium pilot before wider rollout. A separate GPT-5.6 Luna medium-effort review checked PM execution clarity. This revision corrects the scratch-field count and local source availability, clarifies subset-relative identities and isolated acceptance writes, reuses the existing dump tool, incorporates shared requirements in every receipt, and adds explicit human gates for v3 consumer-design approval and skill-contract updates.

The pilot recommendation remains an owner decision, not an implemented scope expansion. Conversion-only output is a legitimate intermediate deliverable but not runnable simulation support. The proposed source-qualified identity, gap fields, native units and embedded schema have preservation/provenance reasons; the consumer-design gate must still establish their usability. Existing 609-test evidence is a measured baseline at the named commit, not an inferred count from older README/pathway text. Neither reviewer reran the suites or full production conversions.

## Next Chat Prompts

### Mode A — checkpointed

```text
Plan: docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Scope: Slice 1 only.

Read the full plan and relevant Mimic skills. Stay on the current branch.
Before Slice 1, confirm the owner approved conversion-first; otherwise stop.
If the owner selected a runnable pilot, stop until the plan is revised for it.
Use orchestrator and scoped-implementation; keep implementation and Git local.
Use an independent read-only Reviewer for drift-audit, then code-review.
Report the authorization gate before quality review; run differential lint and
investigate code-health for structural changes. Fix findings within the contract.
Stop on approval gates, unavailable required review, or unresolved plan conflicts.
Ask before committing; never amend or bypass hooks. Then use handoff to record
evidence/provenance and the next slice. Do not start Slice 2.
Confirm plan, branch, slice and Reviewer, then begin.
```

### Mode B — recommended conversion-first run

The owner must choose and approve the conversion-first/runtime-follow-on boundary and commit the planning changes before using this launcher. If a runnable pilot is selected, replace the slice sequence before starting. Slices 2 and 11 have additional explicit approval gates; the launcher does not waive them. The prompt below supplies prospective commit permission only when actually submitted by the owner.

```text
Plan: docs/dev/MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: harness <choose> model <choose>
Reviewer: harness <choose> model <choose>

I approve the frozen conversion-first plan, including L-Halo binary, forests-HDF5,
ASCII, lossless v3 output and the separate runtime follow-on. I authorise local
per-slice commits needed by Mode B. Stay on the current branch; no new branch,
amend, push, production-data mutation, remote operation or runtime implementation.

Use project-manager. You are the accountable PM and never write slice code.
Read the plan; run check-plan with repository context; start only on a clean tree
with planning changes committed. Keep the run token out of subordinate sessions.
Choose Developer effort from each slice's label; use fresh sessions in plan order.
Use one long asynchronous observe --wait, remaining available for progress updates.
Assess actual committed diffs and validation, run differential lint, investigate
structural code-health, and commission independent drift-audit before code-review.
Both reviews must be fresh for every slice. Report authorization before quality.
Accept, steer or stop on evidence; never waive missing required real-data gates,
edit the frozen plan, or interpret converter completion as runtime completion.
Delegate long test suites with captured logs and never run suites concurrently.
Confirm plan, branch, models and first slice, then begin.
Report elapsed time from the run record, accepted slices/evidence, stops, plan
defects, reviewer provenance and residual validation/runtime limitations.
```
