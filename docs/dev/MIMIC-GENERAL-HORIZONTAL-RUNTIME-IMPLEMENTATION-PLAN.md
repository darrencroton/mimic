# General Horizontal Runtime Implementation Plan

**Purpose:** Make Mimic's horizontal reader and driver consume horizontal-HDF5 format version 3, the lossless output of the generalised converter, so that gapped and wide merger-tree datasets can be run, and prove it by per-`UniqueGalaxyID` bitwise parity against the vertical driver on real gapped mini-Millennium data.

**Status:** Proposed implementation contract, written 2026-09-25 as the runtime follow-on that Slice 12 of [`MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md`](MIMIC-CONVERTER-GENERALISATION-IMPLEMENTATION-PLAN.md) requires. It was written after inspecting the final converter and schema at `16101d5b26af00815417b07f33a34f42317559c0` (the commit that closed that plan's acceptance evidence). The converter was subsequently reviewed and cleaned up on 2026-09-28 ([`MIMIC-CONVERTER-GENERALISATION-CODE-REVIEW.md`](MIMIC-CONVERTER-GENERALISATION-CODE-REVIEW.md)) without changing the v3 format, the CLI's command lines or the acceptance figures this plan relies on; its v3 writer now lives in `scripts/convert/hdf5_writer_v3.py` and its v3 battery in `scripts/convert/validate_v3.py`, and the consumer metadata fragment's `format_table_fields` now name the core role each provides. That review's four questions for Gate R0 are recorded under [Independent review notes for Gate R0](#independent-review-notes-for-gate-r0-2026-09-28). **It is not approved for execution.** Nothing here has been implemented, and nothing here authorises implementation: [Gate R0](#gate-r0-owner-decisions-before-any-slice) names the architectural decisions the owner must make first, and every slice that touches the runtime is approval-gated. No project-manager or Developer session may start this plan on its own initiative, and nothing in the converter plan's Mode B run authorises it.

**What this plan is for, in one sentence:** conversion already works; running the result does not, and the gap between the two is a reader, a driver and an input seam that were all built for adjacent, int32-indexed, fixed-unit input.

---

## Table of Contents

1. [Inspected state](#inspected-state)
2. [What the runtime must consume](#what-the-runtime-must-consume)
3. [Preservation rules](#preservation-rules)
4. [Width is not memory](#width-is-not-memory)
5. [Gate R0: owner decisions before any slice](#gate-r0-owner-decisions-before-any-slice)
6. [Execution policy](#execution-policy)
7. [Slices 1–8](#slice-1-int64-indices-through-the-input-and-driver-seam)
8. [Next Chat Prompts](#next-chat-prompts)

---

## Inspected state

Every runtime fact below was read from the source at `16101d5b`, not recalled. Line numbers drift; the symbols do not.

| Area | Current fact | Where |
|---|---|---|
| Format version | The reader accepts exactly `format_version == 2` and rejects anything else, then separately requires `links_adjacent == 1` | `read_horizontal_hdf5.c`, `HORIZONTAL_HDF5_FORMAT_VERSION`, `open_run` header checks |
| Object set | The reader rejects any root object other than `/header` and `/halos`, and any `/halos` dataset outside its fixed table, so a v3 `/schema` group or a selected extra is fatal | `read_horizontal_hdf5.c`, structure scan |
| Link width | The five links are declared `HORIZONTAL_H5_I32` in the reader's dataset table | `HORIZONTAL_H5_HALO_DATASETS` |
| Slab width | `n_halos` above `INT32_MAX` is rejected at open; the driver separately refuses a slab above `INT_MAX` because halo indices are `int` | `open_run`; `horizontal_acquire_generation()` |
| Generated link type | The property generator requires every `tree_link`, `index` and `count` core role to be an `int` catalog field | `scripts/generate_properties.py`, core-role type check |
| Generated view | `struct RawHalo` links are `int`; every `mimic_tree_get_*` accessor takes and returns `int` | `src/include/generated/raw_halo_defs.h`, `tree_property_accessors.h` (generated, untracked) |
| Halo indices | `struct Halo.HaloNr` is `int` (declared in `src/core/core_properties.yaml`); `struct HaloAuxData` uses `int`; the vertical driver's tree walk and the horizontal driver's slab loop use `int halonr` | `core_properties.yaml`, `src/include/types.h`, `build_model.c`, `horizontal_driver.c` |
| Generations | The driver holds exactly two `struct HorizontalGeneration` (raw slab, aux, processed output buffer, galaxy pool), ping-ponging on snapshot parity, and releases N−1 as soon as snapshot N's FoF groups have deep-copied what they inherit | `horizontal_driver.c`, `run_horizontal_driver()` |
| Progenitor walk | Most-massive-progenitor, count and gather all read the single `prev` context, and the cycle guard bounds the walk by `prev->view.count` | `horizontal_find_most_massive_progenitor()`, `horizontal_count_progenitor_galaxies()`, `horizontal_gather_progenitor_galaxies()` |
| Inheritance time | The timestep interval comes from the workspace halo's **progenitor** `SnapNum` (`Age[prev_snap] - Age[snap]`), carried unchanged by inheritance until marshalling | `src/core/halo_evolution.c`, `setup_module_context()` |
| Identity | `UniqueGalaxyID = HaloRankInForest + multiplier × (ForestIndex + 1)`, read from reader-owned `SnapshotSlab.forest_index`/`halo_rank_in_forest` arrays | `horizontal_make_unique_galaxy_id()`, `src/io/horizontal/reader.h` |

Converter and format facts this plan builds on, all measured or specified in committed documents:

- The v3 specification is [`HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md`](HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md), made normative for the converter by Gate G1 ([`MIMIC-V3-CONSUMER-DESIGN-REVIEW.md`](MIMIC-V3-CONSUMER-DESIGN-REVIEW.md), approved 2026-09-23). The review's findings 1, 3, 4, 5, 6 and 10 are the runtime prerequisites this plan exists to discharge.
- `scripts/convert/convert_trees.py` emits v3 for all three adapters. Acceptance evidence is [`MIMIC-CONVERTER-GENERALISATION-ACCEPTANCE.md`](MIMIC-CONVERTER-GENERALISATION-ACCEPTANCE.md): real complete mini-Millennium (1,533,122 halos, 29,291 forward-gapped descendant links, maximum span 2, 8 empty snapshot files of 64) and real complete micro-Uchuu in three source formats (22,580,924 halos), sampled Millennium and mini-Uchuu (files 0–15), and the full-Uchuu fixture only.
- The v3 logical row is exactly 140 B/halo for the default schema on every adapter; on-disk figures are 141.67–337.22 B/halo depending on snapshot population.
- Every v3 `ForestIndex`/`HaloRankInForest` for L-Halo is the file-prefix tree number and original within-tree row index, which is what the vertical `lhalo_binary` reader uses for `UniqueGalaxyID`. The acceptance evidence compared per-forest file/unit ordinals against the C dump with zero mismatches on every non-ASCII route.

## What the runtime must consume

The consumer obligations below come from the v3 specification's own "A consumer must validate at open" paragraph and from the design review's findings, restated as runtime requirements:

- **Generated payload schemas.** Payload units and precision are the adapter's native ones, declared per file in `/schema`. L-Halo mass is float32 `1e10 Msun/h`; Consistent-Trees mass is float32 `Msun/h`. A consuming package's `halo_properties.yaml` must declare exactly what the file declares, which means **one simulation package per (simulation, source format)**. The runtime compares the package's compiled payload declarations against `/schema` at open and aborts on any disagreement (topology and identity fields are checked against the format's fixed table instead), so a wrong-by-10¹⁰ mass is a startup failure rather than a silent result.
- **v3 reader validation.** At open: supported version, object set `/header` + `/halos` + `/schema`, header consistency across files, `/schema` agreement, `source_format` and `column_mapping_sha256` identical across files, scale factors against the a_list, the five physical header values against the package, identity-multiplier bounds. At slab load: every non-null link is a valid row of the file its target-snapshot column names, and every biconditional (`*Snapshot == -1` iff the index is `-1`) holds.
- **Snapshot-qualified progenitor lookup.** `FirstProgenitor` resolves into the `FirstProgenitorSnapshot` file and each `NextProgenitor` into its own `NextProgenitorSnapshot` file. A chain may span several earlier snapshots. N−1 is never assumed when `links_adjacent == 0`.
- **Pending processed and galaxy state across gaps.** A halo at snapshot *k* whose `DescendantSnapshot` is *m* > *k* + 1 must keep its processed halo and galaxies alive until *m* is processed. The review's retention horizon — `horizon(k) = max(DescendantSnapshot)` over snapshot *k*'s rows with a descendant, or *k* if none — is exact, forward-only and computable from the slab already loaded.
- **Ownership and lifetime.** One owner for retained state: the driver, generalising its existing two-generation ownership into a pool of retired generations keyed by snapshot number, each released once its horizon has been processed. The reader stays stateless with respect to retention.
- **int64 indices throughout the input/driver seam.** Links, halo numbers, aux offsets, workspace and scratch capacities, loop counters, and every accessor on the path that reads a v3 link — generated or reader-owned, as R0-2 decides. Changing the HDF5 dtype alone while any of these stays `int` would narrow silently or refuse at the old bound. R0-4 decides only whether vertical-only code, which never sees a slab index, is widened too.
- **Full-Uchuu memory constraints.** See [Width is not memory](#width-is-not-memory). int64 is necessary for full Uchuu and nowhere near sufficient.

## Preservation rules

These bind every slice. They are the runtime counterpart of the converter plan's C1–C3 and the v3 draft's "Lossless means lossless".

1. **No synthetic gap halos.** The runtime never inserts a halo, a phantom generation or an interpolated state to close a gap. A gapped descendant edge is consumed as a gapped edge.
2. **No gap removal.** The runtime never rewrites `A(k) → D(k+2)` as two adjacent edges, never drops a gapped link, and never treats a gap as malformed input.
3. **Inheritance source times are the source's.** A galaxy inherited across a gap evolves over `Age[k] → Age[m]` using its progenitor's own `SnapNum` *k*, exactly as `setup_module_context()` already derives it; never over `Age[m-1] → Age[m]`. Dynamic substep counts follow the same interval.
4. **Source chain order is the source's.** Progenitor walks follow the stored `FirstProgenitor`/`NextProgenitor` order. Most-massive-progenitor selection and its tie-break are the vertical path's (`find_most_massive_progenitor()` in `build_model.c`), applied to the stored chain. `SourceHaloID` row order is a storage order, never a progenitor priority, and is never used to reorder a chain.
5. **Source-relative `UniqueGalaxyID`.** Identity comes from the v3 `ForestIndex`/`HaloRankInForest` arrays through the existing encoding, never from `SourceHaloID`, `MostBoundID` or a row index. Equality is promised against the **selected source format's own vertical reader** (L-Halo mini-Millennium against L-Halo mini-Millennium), never across source formats.
6. **Version 2 is unchanged.** Every v2 rule, byte and validation outcome stays exactly as it is. `micro-uchuu-horizontal` stays a v2 package and its cross-format identity gate stays green. Version 1 stays rejected.
7. **Abort, never repair.** Every validation failure names the file, object and value and stops the run.

**The acceptance gate for the whole plan:** per-`UniqueGalaxyID` bitwise equality of every output field, at every output snapshot, between vertical `mini-millennium` (`lhalo_binary`) and horizontal v3 mini-Millennium, under `halos-only` and `sage16`, with fixed and dynamic timesteps, on the **real** complete eight-file dataset with its 29,291 gaps. A fixture pass is not acceptance, and no runtime support claim may be made before this gate passes.

## Width is not memory

Widening links to int64 lets the runtime **name** a row above `INT32_MAX`. It does nothing to make that row, or the slab holding it, **fit in memory**. The plan keeps the two apart.

**What the numbers say.** The only measured horizontal-driver memory fit on record is Shin-Uchuu's production figure, `peak ≈ 1.221 GB + 1,340.6 B/halo × N` for largest slab N ([`MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`](MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md), from the Shin-Uchuu conversion plan's P5). Full Uchuu's package README records ≈181.5 × 10⁹ halos over 50 snapshots, so at least one snapshot holds at least ≈3.63 × 10⁹ halos. Multiplying the two gives ≈4.9 TB for the largest-slab term alone, against a 512 GB host. **That figure is an extrapolation**: a v2-width fit from a different simulation, applied to an unverified README total. v3's wider rows and gap retention can only raise it. Its order of magnitude is the point, not its digits: full Uchuu cannot run with whole slabs resident on any host this project has.

**What follows.**

- This plan makes the input/driver seam int64-correct, so a wide slab can never narrow silently or overflow an index. **What happens when it does not fit in memory depends on R0-5**, which this plan does not decide. Under R0-5(a), wide slabs and over-large retention **fail loudly and early** against a declared memory ceiling, before allocation. Under R0-5(b), the driver reports the bytes a slab or retention set needs, and running out of memory surfaces as an allocator failure. That trade is the owner's. It does **not** deliver a whole-snapshot low-memory run of full Uchuu, and no slice, document or report may say it does.
- Running full Uchuu needs chunked slab streaming ([`MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md`](MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md)), still a concept note. The design review's finding 10 says gap retention and chunked streaming interact — retaining several generations of *windows* is a different memory problem from retaining several whole slabs — and should be designed together. Whether to do that before this plan, or to land gap retention on whole slabs first and accept a later redesign, is owner decision R0-7.
- Gap retention has its own memory cost, bounded by the data rather than by a constant. For mini-Millennium, maximum span 2 means at most three generations are live at once (whole generations under R0-6(a), projections under R0-6(b)); the retained population per snapshot is measurable from the dataset before a run and is reported, not assumed.
- `links_adjacent == 1` collapses retention to today's two generations, which is why the converter measures and stamps it honestly.

## Gate R0: owner decisions before any slice

These are architectural decisions this plan deliberately does not make. Each has a recommendation, but a recommendation is not a decision: the owner records each outcome in this section, dated, before Slice 1 starts. A project-manager may not decide any of them, and a decision that changes a slice's surface or criteria is a plan amendment, not an in-run adjustment. Every slice below is written so that its criteria hold under **every** option listed here, branching explicitly where options differ. Where an option cannot be delivered inside the slices as written, it is marked **requires amendment** and says what must change. It remains a legitimate choice, but recording it means the owner amends the named slice before that slice starts.

| # | Decision | Options | Recommendation |
|---|---|---|---|
| R0-1 | **Where v3 is read.** `tree_type` is public configuration. | (a) Extend `horizontal_hdf5` to accept v2 and v3, dispatching on `format_version`, with v2's validation path byte-for-byte unchanged. (b) Register a second horizontal reader name for v3. | (a): the format ratchet already names the version in every file, and `tree_name` stays the literal `snapshot_%03d.h5`. |
| R0-2 | **How wide links enter the generated view.** The generator requires `tree_link` roles to be `int`, and `RawHalo` is shared by every reader. | (a) Allow `long long` `tree_link` fields and widen accessors to `int64_t` everywhere, so vertical packages keep `int` storage but all index arithmetic is int64. (b) Widen `RawHalo` links to int64 for every package — **requires amendment**: `src/io/vertical/binary.c` `fread`s L-Halo records directly into `struct RawHalo`, so this changes the vertical readers' record handling and every package's `halo_properties.yaml`, none of which is in Slice 1's surface. (c) Keep `RawHalo` int32 and hold v3 links in reader-owned int64 slab arrays — **requires amendment**: v3 links would then never populate `RawHalo`, but every caller of the five generated link accessors reads `RawHalo`. That includes shared helpers used on both drivers — `count_fof_subhalos()` (`src/core/halo_evolution.c`) walks `mimic_tree_get_NextHaloInFOFgroup`, `get_virial_mass()` (`src/core/virial.c`) reads `mimic_tree_get_FirstHaloInFOFgroup` — and the horizontal driver's own FoF-central loop, not only Slice 4's progenitor walk. On a v3 run they would silently read unpopulated fields, giving wrong FoF grouping and wrong virial masses. This plan has no design for how those callers reach the reader-owned links; one must be written and added to the plan before (c) can be executed. | (a): per-package storage width with one int64 index type; the vertical path's bytes and results must stay identical, which Slice 1 proves. |
| R0-3 | **Where the three target-snapshot columns and `SourceHaloID` live.** | (a) Reader-owned slab arrays, following the `ForestIndex`/`HaloRankInForest` precedent the v2 erratum of 2026-08-11 settled. (b) Declared catalog properties — **requires amendment**: Slice 2's reader tests would need a package declaring them, which does not exist until Slice 3, so Slices 2 and 3 would swap order. | (a), as the design review recommends: they are format metadata, not catalog properties. |
| R0-4 | **Scope of the int64 index change beyond the seam.** Every path a horizontal slab index reaches must be int64 under either option: the horizontal driver, `struct Halo.HaloNr` in the shared workspace, the link accessors R0-2 selects, and the shared inheritance, output-buffer and virial services. What is open is vertical-only code, which never sees a slab index: the vertical driver's `struct HaloAuxData` and `build_model.c`'s tree walk. | (a) Widen the vertical-only code too, so there is one index type. (b) Leave vertical-only code `int`, since a vertical unit is one forest. | (a): one index type, nothing to audit at a boundary; its cost (struct growth on the vertical path) is measured in Slice 1 before acceptance. |
| R0-5 | **Retained-generation memory policy.** | (a) A run-file memory ceiling for retained generations, checked before each retention and failing before allocation. (b) No ceiling; report the measured retention and let the allocator fail. | (a). A new run-file key is a public configuration surface, which is why this is the owner's call. |
| R0-6 | **Retained footprint per generation.** | (a) Retain whole generations (raw slab, aux, processed buffer, pool). (b) Retain a projection of the fields a later snapshot reads, generalising the deferred compact previous-slab projection. | (a) for this plan, since it is the smallest change with a clean parity argument; (b) belongs with chunked streaming. |
| R0-7 | **Relationship to chunked slab streaming.** | (a) Land gap retention on whole slabs now; design chunking separately later and accept that it may rework retention. (b) Design chunked streaming and gap retention together first, before Slice 4 — **requires amendment**: Slice 4 is written to proceed directly from Slice 3, so (b) starts only after the owner has amended Slice 4 from an approved joint chunked-streaming and gap-retention design. | Owner judgement. (a) reaches a runnable gapped mini-Millennium soonest; (b) honours the design review's finding 10 more strictly. Under either, full-Uchuu execution stays out of this plan. |
| R0-8 | **Selected extras.** | (a) Validate every `/schema` declaration but materialise only extras the package declares, ignoring undeclared ones. (b) Require the package to declare every extra present. | (a): extras are opt-in by design; an undeclared extra must still be validated, never silently trusted. |
| R0-9 | **Output provenance for v3 inputs — deferred, outside this plan.** Recording `source_format` and `column_mapping_sha256` in `RunProperties` changes the output schema and `hdf5_format_version`, which touches every driver's output and the cross-format gate's metadata allow-lists. **No slice here delivers it**, and every slice's non-goals exclude output-schema changes. | (a) Schedule it as its own output-schema plan after this one lands. (b) Do not record it. | (a): a run should be traceable to the exact mapping it consumed, but through a separately planned and gated output-schema change, not folded into the runtime work. Recording either option here commits no slice of this plan to anything. |
| R0-10 | **Package naming.** | One package per (simulation, source format). Proposed: `simulations/mini-millennium-horizontal/` (Slices 3–6), and `micro-uchuu-lhalo-horizontal/`, `micro-uchuu-hdf5-horizontal/`, `millennium-horizontal/` and `mini-uchuu-horizontal/` (Slice 7). | Accept the names or give others; a different name is a mechanical plan amendment to the slices that list it. |
| R0-11 | **Promoting the v3 specification.** | (a) Move the approved draft into `HORIZONTAL-HDF5-FORMAT.md` as a normative version 3 section once runtime support ships. (b) Keep two files. | (a), in Slice 8 only, after the parity gate. |

**Recorded decisions:** none yet. The owner adds one dated line per decision here.

## Independent review notes for Gate R0 (2026-09-28)

These are questions for the owner to weigh when recording Gate R0, not decisions, and they change none of this plan's frozen text above.

- R0-4(a) widens vertical-only index code to int64 for uniformity, growing `struct HaloAuxData` on the hot vertical path with no functional need — should (b) be the recommendation instead, or should an acceptance ceiling on the measured cost be added if (a) is kept?
- R0-10's one-package-per-(simulation, source format) rule yields five near-mirror packages — should `converter_columns.yaml` or the emitted `/schema` generate the horizontal package's `halo_properties.yaml`, to avoid the metadata duplication `VISION.md` warns against?
- R0-7(a) accepts reworking retention later — given the ≈4.9 TB largest-slab estimate, is a short joint design note before Slice 4 cheaper than the rework?
- Slice 6's four-leg bitwise parity gate relies on `sage16`, whose vertical baseline already carries ≈0.1% chaotic threshold flips — how is a non-gap divergence adjudicated (the `mimic-scientific-method` skill's chaos-versus-bug discipline)?

## Execution policy

- **Not executable yet.** Every slice below requires Gate R0 to be fully recorded first. Slices 1, 2, 4, 5 and 8 are also approval-gated individually.
- Slices run in order; each is independently gateable. Every slice records its prerequisite check first — datasets, their paths and file counts, the C toolchain (libyaml + HDF5), `mimic_venv` — and stops for the owner if anything critical is missing, exactly as the converter plan's data-availability section requires.
- Every runtime slice builds and tests with a consistent `MODEL=`/`SIMULATION=` pair, runs `make tests summary` on the default pair (delegated, logs captured), `make check-generated`, `make validate-modules`, `make check-format`, `make check-docs` and `git diff --check`, plus differential lint. Long suites are delegated and never run concurrently.
- **Vertical bit-identity is a standing gate.** Any slice that touches shared code proves the vertical path unchanged against the committed baselines, with no tolerance change and no baseline regeneration.
- **The v2 cross-format identity gate stays green** (`micro-uchuu-ascii` vertical against `micro-uchuu-horizontal` horizontal) after any slice that touches the horizontal reader or driver, on a machine holding both datasets.
- No failing test is weakened; generated files are never hand-edited; no dependency changes; no production data, symlink or baseline is modified.
- **Effort:** Slices 1, 2, 4, 5 and 6 are high effort (shared seam, public format consumption, ownership, memory, scientific evidence) and want a strong Developer and an independent Reviewer. Slices 3, 7 and 8 are medium.

## Slice 1: int64 indices through the input and driver seam

### Intended Change

- Make every halo index, link value, count and capacity that a horizontal slab index reaches int64 — and, under R0-4(a), the vertical-only ones too — in the form R0-2's recorded option takes, without changing any vertical or v2 horizontal result.

### Acceptance Criteria

- [ ] Gate R0 is fully recorded in this plan, and this slice implements R0-2 and R0-4 as recorded.
- [ ] Link access matches R0-2's recorded option on every path. Under R0-2(a) or (b), the generated `mimic_tree_get_*` link accessors take and return `int64_t`. Under R0-2(b) this slice starts only after the owner has amended its surface (see R0-2). Under R0-2(c) this slice likewise starts only after that amendment (see R0-2), and then the generated link accessors keep `int` because v3 links never enter `RawHalo`. The int64 reader-owned link arrays are created in Slice 2, and this slice widens every index parameter and return type the driver will use to read them. In every case the generator accepts the link storage type R0-2 allows and still rejects every other type for `tree_link`, `index` and `count` roles.
- [ ] On every path a horizontal slab index reaches, these are int64 and no silent narrowing remains: `HaloNr`, the horizontal driver's aux pairs, progenitor/FoF walk variables, workspace and scratch capacities, the shared inheritance/output-buffer/virial index parameters, and the horizontal slab loop. Under R0-4(a) the vertical-only `struct HaloAuxData` offsets and `build_model.c` tree-walk variables are int64 too. Under R0-4(b) they stay `int`, and no horizontal index passes through them.
- [ ] Vertical output on the default pair and every committed baseline is bitwise identical to the pre-change commit.
- [ ] v2 horizontal output on the `micro-uchuu-horizontal` fixture is bitwise identical to the pre-change commit.
- [ ] The measured change in `sizeof(struct RawHalo)`, `sizeof(struct Halo)` and default-pair peak RSS is recorded in the slice receipt.
- [ ] The driver's `INT_MAX` slab refusal is removed only where every index it guarded is now int64; the reader's v2 `n_halos <= INT32_MAX` rule is unchanged, because it is a v2 format rule.

### Authorized Surface

- Files allowed to change:
  - `scripts/generate_properties.py`
  - `src/core/core_properties.yaml`
  - `src/include/types.h`
  - `src/core/build_model.c`
  - `src/core/halo_evolution.c`
  - `src/core/horizontal_driver.c`
  - `src/core/inheritance.c`
  - `src/core/inheritance.h`
  - `src/core/output_buffer.c`
  - `src/core/output_buffer.h`
  - `src/core/virial.c`
  - `tests/unit/`
  - `tests/integration/test_unit_contract_generation.py`
- Functions/classes/components allowed to change: generated accessor and struct emission for index-typed roles; index-typed fields and locals on the input/driver seam; no physics module.
- Tests allowed or expected to change: accessor-width, narrowing and struct-layout unit tests; generator type-rule tests.

### Explicit Non-Goals

- No v3 reading, no gap retention, no new configuration key, no physics or output-schema change, no baseline regeneration.

### Risk Flags

- Risky surfaces touched: generated code shared by every reader and both drivers; shared inheritance and output-buffer services.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: accessor width tests with values above 2³¹ in a synthetic view; generator rejection tests.
- Commands to run: `make generate`, `make check-generated`, `make tests summary` (delegated), baseline comparison on every committed baseline, the `micro-uchuu-horizontal` fixture tests, `make check-format`, `make check-docs`, `git diff --check`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: grep the seam for remaining `int halonr`/`int prog` declarations and account for every survivor.

### Rollback Path

- Revert in a new authorised commit; no data or baseline changed.

## Slice 2: Version 3 reader validation and slab loading

### Intended Change

- Teach Mimic to open, validate and load v3 datasets per the v3 specification and the recorded R0-1, R0-2, R0-3 and R0-8, leaving v2 validation exactly as it is. Under R0-1(a) this extends `horizontal_hdf5`; under R0-1(b) it adds a second horizontal reader and leaves `horizontal_hdf5` unchanged.

### Acceptance Criteria

- [ ] v2 datasets take exactly the v2 validation path; every existing v2 reader test passes unchanged, and a v2 file with `links_adjacent != 1` is still rejected.
- [ ] v1, unknown and mixed-version datasets are rejected, naming the file and version. Under R0-1(b), `read_horizontal_hdf5.c` is byte-for-byte unchanged, the new reader rejects every version other than 3, and its registered name is disjoint from every vertical and horizontal reader name.
- [ ] v3 `open_run` enforces the object set `/header`, `/halos`, `/schema`; header consistency across files; `source_format` and `column_mapping_sha256` identical across files; scale factors against the a_list exactly; the five physical header values against the package; identity-multiplier bounds; `n_halos` as int64 with no int32 ceiling.
- [ ] **Fields the package declares** (every core payload field it consumes plus every extra it declares): v3 `open_run` compares each one's `/schema` declaration (`type`, `units`, `h_convention`) and dataset dtype/shape with the package's compiled `halo_properties.yaml`, and aborts on any disagreement, on a declared field missing from `/schema` or `/halos`, or on a type/shape mismatch. Topology and identity fields are excluded from this comparison even when the package declares them, because `/schema` never declares them: the five link roles always, and `SourceHaloID` and the target-snapshot columns under R0-3(b). They are checked against the v3 fixed format table instead.
- [ ] **`/schema` fields the package does not declare** are governed by R0-8, not by the criterion above. Under R0-8(a) each one's declaration is checked for internal consistency (a valid `type`/`h_convention` vocabulary entry and a dataset whose dtype and shape match its own declared `type`), is not materialised, and is never an abort merely for being undeclared. Under R0-8(b) an undeclared field aborts at open, naming it.
- [ ] A `/schema` or `/halos` object that is an external or soft link is rejected, whether or not the package declares it.
- [ ] `load_slab` fills the five int64 links where R0-2's recorded option puts them: the generated `RawHalo` fields under (a) or (b), or reader-owned int64 arrays under (c). It fills the three target-snapshot columns and `SourceHaloID` where R0-3's recorded option puts them: reader-owned slab arrays under (a), or the package's declared catalog properties under (b), whose types are checked against the v3 fixed format table because `/schema` does not declare them. It then validates every non-null link against the `n_halos` of the file its target column names, plus every `-1`-iff-`-1` biconditional, with bounded counted diagnostics.
- [ ] Validation scans use bounded hyperslab reads; nothing allocates proportional to a snapshot's `n_halos` except the slab itself.
- [ ] Committed v3 fixtures are small, produced by `scripts/convert/convert_trees.py` from committed source fixtures, and include a gap, an empty snapshot, a cross-snapshot `NextProgenitor` and one selected extra. The extra exercises R0-8's recorded outcome for a field the test package does not declare: not materialised under (a), abort at open under (b).

### Authorized Surface

- Files allowed to change:
  - `src/io/horizontal/read_horizontal_hdf5.c`
  - `src/io/horizontal/reader.h`
  - `src/io/horizontal/interface.c`
  - `src/io/horizontal/registry.c` (R0-1(b) only)
  - `src/io/horizontal/read_horizontal_v3_hdf5.c` (new; R0-1(b) only — the `hdf5.c` suffix is required by the Makefile's HDF5 source rule)
  - `simulations/micro-uchuu-horizontal/_tests/unit/`
  - `tests/data/horizontal_v3/`
  - `tests/unit/`
- Functions/classes/components allowed to change: horizontal reader validation, slab loading and slab-array ownership.
- Tests allowed or expected to change: v2 regression, v3 conformance and corruption cases.

### Explicit Non-Goals

- No driver change beyond what compiles against the new slab arrays; no gap retention; no new simulation package; no change to what a v2 file must contain.

### Risk Flags

- Risky surfaces touched: public on-disk contract consumption, untrusted HDF5 input.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: one negative case per v3 invariant a consumer checks; v2 regression unchanged.
- Commands to run: reader unit tests under the fixture pair, `make tests summary` (delegated), `make check-format`, `make check-docs`, `git diff --check`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: build the pre-change binary and confirm the new fixtures are rejected there and accepted here.

### Rollback Path

- Revert in a new authorised commit; v3 files remain valid converter output and are simply rejected again.

## Slice 3: A version 3 simulation package for mini-Millennium

### Intended Change

- Add the first v3 simulation package (name per R0-10), whose `halo_properties.yaml` declares exactly what the converter's mini-Millennium `/schema` declares, with run files for `halos-only` and `sage16`.

### Acceptance Criteria

- [ ] `halo_properties.yaml` declares every v3 payload field with the `/schema` type, units and `h_convention` of a dataset converted with `simulations/mini-millennium/converter_columns.yaml` — in particular `M_Crit200` as float `1e10 Msun/h`.
- [ ] Link-role declarations follow R0-2's recorded option: `long long` under (a); as the amended Slice 1 specifies under (b); under (c) as the amendment R0-2(c) requires specifies (see R0-2). Under R0-3(b) the package also declares `SourceHaloID` and the three target-snapshot columns as catalog properties, with the v3 format table's types. Under R0-3(a) it declares none of them.
- [ ] `simulation_info.yaml` declares `tree_type`/`processing_order` per R0-1, the mini-Millennium cosmology, box size and particle mass unchanged, and the a_list identical to `simulations/mini-millennium/mini-millennium.a_list`.
- [ ] `make generate` and `make validate-modules` pass for this package under `halos-only` and `sage16`.
- [ ] The package's README states its provenance: which converter commit, command and profile produced its data, and that its `snapshots/` is a local symlink to a converted dataset, never committed data.
- [ ] Existing packages, including `simulations/mini-millennium/`, are unchanged.

### Authorized Surface

- Files allowed to change:
  - `simulations/mini-millennium-horizontal/`
  - `models/halos-only/input/halos-only_mini-millennium-horizontal.yaml`
  - `models/sage16/input/sage16_mini-millennium-horizontal.yaml`
- Functions/classes/components allowed to change: package metadata and run files only.
- Tests allowed or expected to change: package-local fixture tests.

### Explicit Non-Goals

- No reader or driver change; no production conversion committed; no change to any existing package.

### Risk Flags

- Risky surfaces touched: new public simulation package.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Tests to add/update: a package-local test that the package's compiled declarations match a committed v3 fixture's `/schema`.
- Commands to run: `make MODEL=halos-only SIMULATION=mini-millennium-horizontal generate validate-modules`, the same for `sage16`, `make check-docs`, `git diff --check`.
- Lint (differential, via the `lint` skill): required when any Python test is added.
- Manual checks: compare every declaration against a real converted dataset's `/schema`.

### Rollback Path

- Remove the package in a new authorised commit; no other package depends on it.

## Slice 4: Snapshot-qualified progenitor lookup and retained generations

### Intended Change

- Replace the driver's fixed two-generation rotation with a driver-owned pool of retained generations keyed by snapshot number, released by retention horizon, and resolve every progenitor link through its target-snapshot column.

### Acceptance Criteria

- [ ] Under R0-7(a) this slice proceeds as written. Under R0-7(b) it starts only after the owner has amended it from an approved joint chunked-streaming and gap-retention design, and its criteria then apply as amended.
- [ ] Most-massive-progenitor, count and gather resolve `FirstProgenitor` through `FirstProgenitorSnapshot` and each `NextProgenitor` through its own `NextProgenitorSnapshot`, reading the retained generation of that snapshot; N−1 is never assumed when `links_adjacent == 0`.
- [ ] The chain-walk cycle guard is bounded by the total retained population, not by one slab's count.
- [ ] Each generation's horizon is computed at load as the maximum `DescendantSnapshot` over its rows with a descendant, or its own snapshot if none; a generation is released exactly when its horizon has been processed, and never earlier.
- [ ] The driver is the single owner of retained generations: whole generations under R0-6(a) (raw slab, reader-owned arrays, aux, processed buffer, galaxy pool), or the projection R0-6(b) specifies, owning every buffer that projection keeps; the reader holds no retention state; every generation is released on success and on failure, and `close_run` finds no slab loaded.
- [ ] A `links_adjacent == 1` dataset, including every v2 dataset, retains exactly two generations, and v2 output is bitwise identical to the pre-change commit.
- [ ] No synthetic halo, phantom generation or interpolated state is created on any path; empty snapshots are processed as empty.
- [ ] Inherited galaxies keep their progenitor's `SnapNum` until marshalling, so the time interval and dynamic substeps span the real gap.
- [ ] `UniqueGalaxyID` is computed from `ForestIndex`/`HaloRankInForest` only.
- [ ] Output at a snapshot between a halo and its gapped descendant contains exactly what the vertical path emits there; the Slice 6 gate settles this, and this slice's own tests pin it on the worked mixed-gap graph of the design review.

### Authorized Surface

- Files allowed to change:
  - `src/core/horizontal_driver.c`
  - `src/core/galaxy_pool.c`
  - `src/core/galaxy_pool.h`
  - `simulations/micro-uchuu-horizontal/_tests/unit/`
  - `simulations/mini-millennium-horizontal/_tests/`
  - `tests/unit/`
- Functions/classes/components allowed to change: generation acquisition/release, progenitor lookup/count/gather, FoF assembly's progenitor access, retention bookkeeping.
- Tests allowed or expected to change: the design review's worked five-halo mixed-gap graph as a committed fixture; release-order and failure-path lifetime tests.

### Explicit Non-Goals

- No chunked or streamed slab access; no projection of retained generations (unless R0-6 recorded otherwise); no MPI; no physics change; no change to shared inheritance semantics.

### Risk Flags

- Risky surfaces touched: memory ownership, driver lifecycle, scientific inheritance across snapshots.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the worked graph with hand-specified expected inheritance, release points and output rows; a zero-progenitor snapshot; a chain spanning three snapshots.
- Commands to run: horizontal unit tests, `make tests summary` (delegated), the v2 cross-format identity gate (manual, dataset-present), memory leak report, `make check-format`, `make check-docs`, `git diff --check`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: trace every allocation of a retained generation to exactly one release on every path.

### Rollback Path

- Revert in a new authorised commit; v2 behaviour must already be identical, so reverting restores only the gapped capability.

## Slice 5: Retention memory accounting and wide-slab refusal

### Intended Change

- Make retained-generation and wide-slab memory explicit on every path: compute each allocation's size from struct widths before allocating it, and report it, so a slab or retention set that cannot fit never narrows or overflows an index. What happens next is R0-5's recorded option: under (a) it is refused before allocation against a run-file ceiling; under (b) its required size is reported and exhaustion surfaces as an allocator failure.

### Acceptance Criteria

- [ ] The driver computes each generation's resident bytes from actual struct widths before allocating it, and the run profile reports the maximum concurrently retained generations and bytes.
- [ ] Under R0-5(a), a retention or slab that would exceed the configured ceiling aborts before allocation, naming the snapshot, the bytes required and the ceiling. The new key is stored in a `struct MimicConfig` field, and parsed and validated like every other run-file key, with unknown and malformed values rejected at configuration. Its user-facing documentation is Slice 8's job, not this slice's.
- [ ] A synthetic slab above `INT32_MAX` rows is exercised through the reader's and driver's index arithmetic without allocating billions of rows (a virtual or header-only test), and reaches the pre-allocation size computation with its required bytes reported, never an index error or a narrowed value. Under R0-5(a) it then reaches the budget refusal.
- [ ] No document, log or report produced by this slice states or implies that full Uchuu runs with whole slabs resident; under R0-5(a) the refusal message names chunked slab streaming as the missing capability, and under R0-5(b) the reported size does.
- [ ] Measured retention for real mini-Millennium (maximum concurrently retained generations and peak RSS) is recorded in the slice receipt.

### Authorized Surface

- Files allowed to change:
  - `src/core/horizontal_driver.c`
  - `src/core/read_parameter_file.c`
  - `src/include/types.h` (R0-5(a) only: the new `struct MimicConfig` field)
  - `src/util/run_profile.c`
  - `src/util/run_profile.h`
  - `tests/unit/`
  - `tests/data/horizontal_v3/`
- Functions/classes/components allowed to change: retention accounting, pre-allocation checks, run-profile reporting, one run-file key if R0-5(a) was recorded.
- Tests allowed or expected to change: budget refusal, wide-index synthetic cases, key parsing.

### Explicit Non-Goals

- No chunked slab streaming, no slab splitting, no projection, no full-Uchuu run.

### Risk Flags

- Risky surfaces touched: memory accounting; public run-file configuration under R0-5(a) only.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: in every case, pre-allocation size computation and reporting for normal and above-`INT32_MAX` synthetic slabs. Under R0-5(a), also refusal at and just above the ceiling, and a key-rejection test for malformed values. Under R0-5(b), a test that no ceiling key is accepted and that the reported required bytes match the struct-width arithmetic.
- Commands to run: unit tests, `make tests summary` (delegated), a real mini-Millennium horizontal run with the profile captured, `make check-format`, `make check-docs`, `git diff --check`.
- Lint (differential, via the `lint` skill): required.
- Manual checks: confirm every file that reports run memory reports retained generations too.

### Rollback Path

- Revert in a new authorised commit; retention still works, without the ceiling.

## Slice 6: Real gapped mini-Millennium parity gate

### Intended Change

- Prove per-`UniqueGalaxyID` bitwise parity between vertical L-Halo mini-Millennium and horizontal v3 mini-Millennium on the real complete dataset, reusing the existing comparator.

### Acceptance Criteria

- [ ] The horizontal side is a fresh v3 conversion of all eight real `trees_063.*` files with `simulations/mini-millennium/converter_columns.yaml`, recorded by commit, command and `column_mapping_sha256`, and its report shows 1,533,122 halos and 29,291 gapped descendant links.
- [ ] For every output snapshot, the set of `UniqueGalaxyID`s is identical and every output field is bitwise identical, with no tolerance, on each of the four legs: `halos-only`/fixed, `halos-only`/dynamic, `sage16`/fixed and `sage16`/dynamic. A missing leg is a failure.
- [ ] Comparisons use `scripts/compare_cross_format_identity.py` unchanged; if a change is genuinely needed it is a separate approved slice, never a relaxation.
- [ ] Every divergence is reported by snapshot, field and example ID, and treated as a defect to trace to a slice, never a tolerance to add.
- [ ] The v2 micro-Uchuu cross-format identity gate passes on the same commit.
- [ ] Sources, symlinks and baselines are unchanged; workdirs and outputs live outside the repository.

### Authorized Surface

- Files allowed to change:
  - `simulations/mini-millennium-horizontal/_tests/scientific/`
  - `docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md`
- Functions/classes/components allowed to change: the package-local gate harness and its evidence record.
- Tests allowed or expected to change: the package-local scientific gate, registered only when its package is selected.

### Explicit Non-Goals

- No production-code fix hidden in the gate; a defect returns to its owning slice. No baseline regeneration, no tolerance.

### Risk Flags

- Risky surfaces touched: scientific evidence and its interpretation.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the gate harness, following `simulations/micro-uchuu-horizontal/_tests/scientific/test_cross_format_identity.py`. Like that harness, **one invocation runs every leg itself**: it builds the four `{halos-only, sage16} × {mini-millennium (vertical), mini-millennium-horizontal}` pairs in isolated git worktrees and runs each model pair under both `TimestepScheme: fixed` and `TimestepScheme: dynamic`. That makes four parity legs: `halos-only`/fixed, `halos-only`/dynamic, `sage16`/fixed and `sage16`/dynamic. The harness fails, never skips, if any leg does not run, and its log names each leg's verdict.
- Commands to run: `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific` on a machine holding the real data. `MODEL=` there selects only the ambient tier build; the `sage16` legs and both timestep schemes run inside the harness, not from separate commands. Then run the v2 gate, `make MODEL=halos-only SIMULATION=micro-uchuu-horizontal tests-scientific`. Delegate both and capture logs; the receipt quotes all four leg verdicts.
- Lint (differential, via the `lint` skill): required.
- Manual checks: confirm the vertical side read the same eight files the conversion inventory names.

### Rollback Path

- Preserve failed evidence; revert the harness in a new authorised commit if needed.

## Slice 7: Remaining version 3 package routes

### Intended Change

- Add v3 packages for the other converted routes that real data can gate, and record parity against each route's own vertical reader where the data is complete.

### Acceptance Criteria

- [ ] micro-Uchuu v3 from L-Halo binary and from forests-HDF5 each have their own package, with payload declarations matching their own `/schema` (L-Halo `1e10 Msun/h`; forests-HDF5 `Msun/h`). Link-role declarations follow R0-2's recorded option, exactly as Slice 3's does: `long long` under (a); as the amended Slice 1 specifies under (b); under (c) as the amendment R0-2(c) requires specifies (see R0-2). Under R0-3(b) each package also declares `SourceHaloID` and the three target-snapshot columns as catalog properties, with the v3 format table's types; under R0-3(a) it declares none of them.
- [ ] Each micro-Uchuu v3 package passes per-`UniqueGalaxyID` bitwise parity against its **own** vertical package (`micro-uchuu`, `micro-uchuu-hdf5`) under `halos-only`; no cross-source-format identity is claimed.
- [ ] Millennium and mini-Uchuu each get their own v3 package (`simulations/millennium-horizontal/`, `simulations/mini-uchuu-horizontal/`), converted from the locally present leading file range only (files 0–15 today), each with link-role and target-snapshot/`SourceHaloID` declarations following R0-2 and R0-3 on the same terms as the micro-Uchuu packages above, and each passes per-`UniqueGalaxyID` bitwise parity under `halos-only` against its own vertical package run over **the identical file range** — for mini-Uchuu that means overriding the shipped `halos-only_mini-uchuu.yaml` range of 0–3 to match, and recording the range used on both sides.
- [ ] Every record, README and report for those two packages labels them as a sampled subset of named files; no whole-simulation conversion or runtime claim is made. Whole-simulation evidence needs the owner to supply the remaining files (16–511 and 16–127), and without them it remains a named gap of this plan.
- [ ] Full Uchuu gets at most a fixture-level package test; its production run remains explicitly unperformed and, per [Width is not memory](#width-is-not-memory), out of scope.
- [ ] Every package README states which evidence it has: complete real data, a sampled subset, or fixtures only.

### Authorized Surface

- Files allowed to change:
  - `simulations/micro-uchuu-lhalo-horizontal/`
  - `simulations/micro-uchuu-hdf5-horizontal/`
  - `simulations/millennium-horizontal/`
  - `simulations/mini-uchuu-horizontal/`
  - `models/halos-only/input/`
  - `docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-ACCEPTANCE.md`
- Functions/classes/components allowed to change: package metadata, run files, package-local gates.
- Tests allowed or expected to change: package-local fixture and scientific tests.

### Explicit Non-Goals

- No runtime code change; no full-Uchuu run; no production conversion replacing existing data.

### Risk Flags

- Risky surfaces touched: new public packages; scientific evidence.
- Approval needed before implementation: no
- Independent audit required: yes

### Validation Plan

- Tests to add/update: package-local schema-agreement tests and gates.
- Commands to run: per-package `generate`, `validate-modules`, `tests-scientific` on dataset-present machines; `make check-docs`, `git diff --check`.
- Lint (differential, via the `lint` skill): required when Python changes.
- Manual checks: package names and evidence labels agree with R0-10; for the two sampled packages, the vertical and horizontal sides name the same file range.

### Rollback Path

- Remove the packages in a new authorised commit.

## Slice 8: Documentation, skills and format promotion

### Intended Change

- Move durable instructions into the guides, READMEs and skills, promote the v3 specification per R0-11, and update the pathway, stating exactly which routes are runnable and on what evidence.

### Acceptance Criteria

- [ ] `docs/USER-GUIDE.md` and `docs/DEVELOPER-GUIDE.md` describe v3 consumption, the retention model, the memory policy R0-5 recorded (under (a) the ceiling and its run-file key, which Slice 5 added undocumented; under (b) the reported-size behaviour) and the one-package-per-source-format rule as implemented.
- [ ] Runtime support is claimed only for routes whose Slice 6 or Slice 7 gate passed, naming the evidence; full Uchuu is stated as not runnable.
- [ ] Version 3 is normative per R0-11's recorded option: under (a) `HORIZONTAL-HDF5-FORMAT.md` carries it as a normative section; under (b) the v3 file's status line changes from DRAFT to normative and `HORIZONTAL-HDF5-FORMAT.md` cross-links it. Either way, version 2's text, invariants and errata are unchanged.
- [ ] Skills describing the reader, driver and format are updated to the implemented facts.
- [ ] `make check-docs` passes and no prose is hard-wrapped.

### Authorized Surface

- Files allowed to change:
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/dev/HORIZONTAL-HDF5-FORMAT.md`
  - `docs/dev/HORIZONTAL-HDF5-FORMAT-V3-DRAFT.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `scripts/convert/README.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md` (R0-5(a) only: the new run-file key)
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
- Functions/classes/components allowed to change: documentation only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code change; no claim beyond measured evidence.

### Risk Flags

- Risky surfaces touched: normative format contract; agent skill instructions.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`, `make check-format`, `git diff --check`.
- Lint (differential, via the `lint` skill): not required for Markdown-only changes.
- Manual checks: every support claim traced to a recorded gate.

### Rollback Path

- Revert documentation in a new authorised commit.

## Next Chat Prompts

Neither launcher may be used until the owner has recorded every Gate R0 decision above and approved this plan.

### Mode A — checkpointed

```text
Plan: docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Scope: Slice 1 only.

Read the full plan and the Mimic skills it names. Confirm every Gate R0 decision
is recorded in the plan; if any is missing, stop. Stay on the current branch.
Use orchestrator and scoped-implementation; keep implementation and Git local.
Use an independent read-only Reviewer for drift-audit, then code-review.
Stop on approval gates, missing data, or unresolved plan conflicts.
Ask before committing; never amend or bypass hooks. Then use handoff.
```

### Mode B — supervised

```text
Plan: docs/dev/MIMIC-GENERAL-HORIZONTAL-RUNTIME-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: harness <choose> model <choose>
Reviewer: harness <choose> model <choose>

I have recorded every Gate R0 decision and approve this plan. I authorise local
per-slice commits needed by Mode B. Stay on the current branch; no push, amend,
production-data mutation, baseline regeneration or remote operation.

Use project-manager. You are the accountable PM and never write slice code.
Run check-plan with repository context; start only on a clean tree.
Stop at every approval-gated slice for my recorded approval.
Run each slice's prerequisite check first and raise any shortfall before work starts.
Commission independent drift-audit then code-review for every slice.
Never waive the real-data parity gate or treat a fixture pass as runtime support.
Report accepted slices, evidence, stops and residual limitations at the end.
```
