# Mimic Shin-Uchuu Version 3 Implementation Plan

**Purpose:** Execute [`MIMIC-SHIN-UCHUU-V3-PLAN.md`](MIMIC-SHIN-UCHUU-V3-PLAN.md), the staged requirements brief: a forest-blocked Consistent-Trees ASCII route under the owner ruling that keeps `format_version 3` (Stage A); the super-forest decision by measurement on the version 2 production dataset (Stage B); a dataset rewriter that migrates version 2 to version 3 and cuts a forest under a declared table (Stage R); the Shin-Uchuu production datasets, uncut then cut, run and compared galaxy by galaxy (Stage C); and the retirement of version 2 (Stage D). The end state is the brief's decision 4: both version 2 datasets replaced by version 3 ones, every piece of version 2 removed, and the Shin-Uchuu dataset cut so that it runs chunked on a 16, 32 or 64 GB laptop.

**Status:** Planning contract, revision 5 (2026-10-09). Stage A (Slices 1 to 6) and Slices 7 and 8 are accepted. Slice 9 was stopped in run 2 for this revision.

Revision 5 records the owner's Stage B decision, taken on the census's production measurements, an independent assessment and its critique. **Every forest of the whole simulation is partitioned into its z = 0 FoF groups:** each tree joins the forest of its terminal halo's z = 0 FoF group. That gives 244,953,607 forests where today there are 166,547,771. The rule has no free parameter and is anchored at the final snapshot, so every halo is treated the same. It replaces the candidate-rule exploration (duration, halo-count and mass thresholds over a co-membership graph) and the laptop-class-driven choice of rule.

Revision 5 amends:
- decisions 6 and 7, F2, F6, F7, and the Stage B execution boundary;
- the execution profiles, the Resource Profile row for Slice 9, and the launcher;
- Slice 9 (rewritten), and Slices 8, 11, 12, 14, 15, 16, 17, 22 and 24;
- procedure steps 1 and 8 to 12.

It adds a [Final-State Inventory](#final-state-inventory) that removes the census machinery the decision makes dead. Every other contract is unchanged.

Revision 4 carries the owner's requests after the panel converged: the stage map below, Claude Opus 5.5 at high effort in place of Fable for Slices 2, 8 and 16 (Fable stays on the rewriter, Slices 11 and 12), and the location of the version 2 reference run output. Revision 2 went back to the same panel for a focused re-review, which found every round-1 item resolved and seven items the amendments themselves opened; revision 3 resolves them: the census labels live for the census directory's lifetime and the cut's candidate evaluation re-reads the forest's slabs with them resident, the rewriter drops its occupancy shortcut and always scans `ForestIndex` itself, `fixtures.py` joins Slice 7's surface, the harness's block-size case moves to Slice 2, the bounded battery's agreement test is restricted to adjacent datasets with the gapped refusal tested separately, the pinned identity gate's mutation source is retargeted, the micro-Uchuu rehearsal compares logical census results through the catalogue id, the surviving converter modules' version 2 prose joins Slice 22, and the Stage R datasets retained into the procedure are named and budgeted. Revision 1 was reviewed by an independent panel (Codex `gpt-6.1-sol` and Claude Fable 5.1, both high effort, read-only); every finding was verified against the repository and this revision resolves them: the production procedure follows the brief's output placement (one run output on `Internal` at a time), the alternate matching modes of the comparator treat identity-valued fields through the matched mapping rather than byte for byte, the cut operation remaps the numeric `Descendant` and verifies the logical target, the version 2 deletions are listed by symbol, the ASCII extras join moves into the adapter slice so no slice breaks the converter floor, the catalogue key is ASCII-only, the sidecar unit ordinal is the dense first-marker rank the parser computes, the census graph is undirected with per-snapshot deduplication and its table invariants are enumerated, the census aggregates are sized and released, the bounded battery has a per-obligation algorithm table with a ceiling in multiples of the widest slab, the rewriter's workdir layout and manifest fields are frozen, the uncut `sage16` run's memory floor is the super-forest's rows, the version 2 branches of the parity gate and the processing-order test are retired, the pinned identity gate's version 2 leg is removed with its reason, the vertical-path preservation stage is kept under a version-neutral gate, and twelve anchors were corrected. The brief's design was critiqued in three panel rounds (2026-10-07) and this plan takes its binding decisions, its ruling, its rewriter contract and its workflows as given; where this plan refines or corrects the brief it says so under [Decisions for the owner](#decisions-for-the-owner) and [Frozen Decisions](#frozen-decisions). Written after five read-only investigations of the code at the planning baseline (the converter's ASCII route, inventory contract, pipeline, manifest, battery and writer; the format document and every stale statement in the guides, skills, changelog and package READMEs; the C reader, driver, partition and identity encoding; the gates, harnesses, fixtures and make targets; the version 2 inventory and the local data), each anchor below re-read at the baseline.

**Planning baseline:** the last commit that changes anything outside this file, [`MIMIC-DEVELOPMENT-PATHWAY.md`](MIMIC-DEVELOPMENT-PATHWAY.md) and the brief's status line. Every `path:line` anchor below was read at that baseline. The hash is deliberately not written here: PM binds a run to the SHA-256 of this file's bytes, so a hash that moves with unrelated commits would force a re-freeze. Recheck drift against the anchors before executing a slice; do not silently rebase a contract onto a changed interface.

**Owner:** [Development pathway, named follow-up](MIMIC-DEVELOPMENT-PATHWAY.md#named-follow-up-version-3-conversions-of-shin-uchuu-and-full-uchuu). Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md). Format contract: [`HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md).

---

## Stage Map

One row per run. "Going in" is what must be true before the run starts; "Your action" is what the owner does. Every PM run is a fresh Mode B session with the launcher at the end of this plan, the PM seat on Claude Opus 5.5 at high effort, the Reviewer on Claude Fable 5.1 at high effort, the Developer per the profiles table, and `--attest` for every earlier slice; the approvals named are recorded with PM's `approve` before the slice starts.

| Run | Stage | Slices | Purpose | Going in | Your action |
|---|---|---|---|---|---|
| 1 | A | 1 to 6 | The ruling into the format document and the ASCII route made forest-blocked, gated on micro-Uchuu; `micro-uchuu-ascii-horizontal` moved to version 3; guides and skills corrected | Decision 1 confirmed; this plan committed and `check-plan` clean; `make`, `make USE-MPI=yes`, `mimic_venv`, the micro-Uchuu ASCII and version 2 datasets resolve; `tests-converter`, `tests-horizontal-v3` and `tests-distributed` pass at the baseline; LaCie directories chosen and the version 2 micro-Uchuu dataset's retention path recorded | Start the run-1 launcher; approve 1, 2, 3, 5, 6 when asked; Slice 5 runs real data for a few hours. Expect at the end: the version 3 micro-Uchuu dataset installed, the acceptance record started |
| 2 | B | 7 to 9 | Measure the super-forest: effective trees, the co-membership graph, candidate cut rules with their predicted physics cost and chunk maxima. **Run 2 (2026-10-08/09):** Slices 7 and 8 accepted; Slice 9 stopped for revision 5 after the owner chose the z = 0 FoF-group rule | Run 1 accepted; the LaCie holds only `data/`; the production `forests.list` (7.56 GB, md5 `60dbf14a99ae9f23ca7c334474477a95`) and `locations.dat` (13.75 GB, md5 `75442d7ae5b1f2465b485e60720e6df7`) in `/Volumes/LaCie/data/uchuu/shin-uchuu-nt-transfer/` | Done |
| 2b | B | 9 (revision 5) | The z = 0 FoF-group table for the whole simulation, its measured cost (promotions, progenitor-order changes, affected histories) and its chunked memory; the obsolete exploration machinery removed; the census record you sign off | Revision 5 committed; run 2's aggregates on the LaCie | New PM run; approve 9. The production `cut` accounting reads the dataset for a few hours (measured in run 2: occupancy 17 min, trees 46 min, graph 43 min). Then read `MIMIC-SHIN-UCHUU-FOREST-CENSUS.md`, fill its two decision slots (the laptop class with its usable budget, and the stop condition), and record them in decision 6 here, then commit |
| 3 | R | 10 to 15 | The rewriter (migrate, cut, no-op), the route's declared cut table, the comparator's lineage key, and every gate at subset scale against the Stage A route and the C reference reader | Run 2b accepted, and your laptop class and stop condition recorded in this plan; about 0.6 TB of LaCie space for the subset conversions and workdirs | New PM run; approve 11 to 15; Slice 15 runs 12 to 24 h of machine time with 100 GB kept free. Expect: the Stage R record with the measured spill, RSS and sizes, and three retained subset datasets |
| 4 | C | 16 | The bounded battery, proven to agree with the external-sort battery on the subset | Run 3 accepted | New PM run; approve 16; a 2 to 4 h agreement run |
| (owner) | C | the production procedure | The uncut dataset by (M), run and compared galaxy by galaxy with the version 2 run; the cut dataset by (C), run, compared, science-checked; the laptop proxy | Slice 16 accepted; the free space the recomputed storage ledger requires on the LaCie (procedure step 1); the version 2 run output at `/Volumes/Internal/results/mimic/sage16-shin-uchuu` with its `metadata/`; decisions 9 to 11 answered | Not PM: a Mode A assistant session walking the twelve steps in order, each step's evidence into the acceptance record as it happens. Two whole-machine moments: step 2 if the reference has to be re-run, and step 5 (the uncut `sage16` run). Your own transfers to NT at steps 6 and 12 |
| 5 | C | 17 | Provenance, the record and the pathway after production | The procedure complete and its record accepted | New PM run; approve 17 |
| 6 | D | 18 to 24 | Retire version 2: utilities extracted, the version 3 ASCII fixture and every consumer retargeted, the negative battery ported, the reader path, converter, crosscheck, fixtures, make target and CI step deleted, the format document's version 2 text frozen as history | Run 5 accepted; the version 2 production dataset deleted from the LaCie after its NT copy was verified | New PM run; approve 19, 21, 22, 23, 24. Expect about 18 thousand lines gone; then archive this plan and the brief on merge |

---

## Decisions for the owner

Each row is a decision this plan needs. The plan is written on the recommendation; a different answer revises the named slices before the affected run starts. The brief's open questions 1 to 6 are rows 1, 6, 7, 8, 9 and 10.

| # | Decision | Recommendation and reason | Plan assumes | Slices affected |
|---|---|---|---|---|
| 1 | **Confirm the ruling** (brief open question 1): stay in version 3 by an explicit errata carve-out, with the dataset produced by the rewriter from version 2. | **Confirm.** Both panelists and the brief converge on it; the technically exact alternative (`format_version 4` with a read of the 11.61 TB source) keeps duplicate reader, converter, test and documentation paths against the vision's single source of truth, and re-conversion from ASCII buys the same bytes for six to seven days and scale engineering the route lacks. | Confirmed | 1 to 6 |
| 2 | **One plan, six Mode B runs (and run 2b, revision 5's restart of Slice 9).** The brief's five stages are one document, executed as six PM runs (one per stage, and Stage C split around the owner-operated production procedure) with a revision point at each boundary, because Stage B ends in an owner decision and Stage C is production work; earlier slices are attested at the next `init`. | **Recommended** over one plan per stage: every slice's contract is written now against the code as it is, the owner reads one document, and the revision points are where the plan legitimately changes (the chosen cut rule, the measured subset figures). | Six runs | all |
| 3 | **`crosscheck.py` (1,704 lines) and `test_crosscheck.py` (1,913 lines)** read datasets through the version 2 layout only (`convert/mimic-convert/crosscheck.py:119-126`). Port to version 3, or retire with version 2? | **Retire at Stage D.** Its role, the reference-topology proof against the C dump, is held for version 3 by `run_generalisation_acceptance.py compare`, which the Stage A route joins; porting would keep two tools for one proof. This raises the brief's 12 to 14 thousand lines to about 18 thousand, three quarters still tests and prose. | Retired | 22, 24 |
| 4 | **Where the new tools live.** The census and the rewriter are new Python under `convert/mimic-convert/` (`forest_census.py` with a `census/` package; `rewrite_trees.py` with a `rewriter/` package; two shared modules, `horizontal_dataset.py` for bounded column reads of version 2 and 3 slabs and `source_index.py` for `forests.list`/`locations.dat`), tested under `make tests-converter`, sharing the writer, schema, manifest and battery modules. | **Recommended.** The cut table is a converter input and the rewriter is a producer of the format the converter owns; `scripts/` holds analysis of Mimic output, not producers. | As stated | 7, 8, 11, 12 |
| 5 | **The committed version 2 fixtures stay where they are until Stage D** (a refinement of the brief's recommendation to move them to a legacy location under `tests/data/` for the A to D window). | **Recommended.** Moving them costs about ten path edits that Stage D deletes anyway; leaving them in place costs nothing, keeps `check-horizontal-fixture` and the version 2 refusal check unchanged, and makes Stage D one coherent swap to a version 3 fixture converted from a committed synthetic ASCII source. | In place until D | 5, 19, 23 |
| 6 | **Laptop class and usable budget** (brief open question 2). Revision 5: the cut rule is decided (F6), so the class no longer chooses the rule. It sets only the chunked configuration and the acceptance budget. | Decide on the revised Stage B census. Record:<br>• the class (16, 32 or 64 GiB, as the census judges it; estimates are quoted in decimal GB);<br>• the **usable concurrent-job budget** (the class less a stated reserve for the operating system and applications);<br>• the task and chunk counts that meet it. Ranks on one laptop run concurrently, so the job's figure is the sum over tasks of each task's widest chunk (Slice 8).<br>The chunk count is bounded below by the widest slab: 519,342,987 rows ÷ G × 1.1 KB, about 17.9 GB at G = 32. A 16 GiB class therefore needs G ≥ 64 whatever the cut. The census-model fit, the Mac proxy (procedure step 11) and a laptop run are distinct acceptances. | 32 GB, provisional | 9, procedure 11 |
| 7 | **Scope of the scientific claim on the cut dataset** (brief open question 3). | **Limit the byte-identity claim to `sage16` and `halos-only`**, measured against the uncut dataset: every galaxy of a forest the cut leaves unchanged (a forest ending in one z = 0 FoF group) is byte-identical. Inside the split forests the differences come from the removed satellite phases of earlier encounters. They are measured with the cut-difference report (procedure step 10) and attributed to the census record's promotions and affected histories. The census record's counts are input-topology counts, not a prediction of how many galaxies differ.<br><br>**Structural invariant claimed:** the input's z = 0 FoF groups are preserved exactly (every z = 0 central/member relation by `MostBoundID`). The z = 0 *galaxy* population and Types of `sage16` are **measured, not claimed**: earlier satellite phases change gas transfers, stripping, mergers, disruption and orphans.<br><br>For HOD and SHAM the cut recomputes the identities their draws and tie-breaks key on in every split forest. SHAM's global ranking can also move tied assignments outside them. Their change is **measured and recorded, not claimed away**, at subset scale in Slice 15. Neither model chunks, so no production run of either on the cut dataset is planned. The uncut dataset keeps all four models byte-identical. | As stated | 9, 15, 17, procedure 10 |
| 8 | **The reference C reader learns the severance rule?** (brief open question 4) | **No.** The independent leg uses transformed ASCII fixtures read by the unmodified reader; the reader stays reference-pure. | No | 13 |
| 9 | **Is the version 2 production run's output (0.51 TB) the reference** for the galaxy-by-galaxy comparison? (brief open question 5) | Check its recorded build, modules, parameters, timestep scheme, output fields and snapshot coverage against the new run file first; re-run the version 2 dataset once, locally, only if they differ. | Check first | Procedure step 5 |
| 10 | **Park the uncut dataset on NT, or keep it locally** in scenario (a)? (brief open question 6) | Keep it until the cut dataset's science checks are accepted, then park it slab by slab with checksums if the facility agrees; nothing in the plan depends on the answer. | Decide at settle | Procedure step 12 |
| 11 | **Storage scenario.** | Plan against **(a)**, one LaCie, the conservative case; the validator scale slice is built regardless, since a 4.4 TB battery spill is unreasonable in either scenario. | (a) | 16, procedure |
| 12 | **Developer models.** | Claude Fable 5.1 at high effort for the rewriter's two slices (migration and cut), whose output is production data at a scale the subset gates never reach; Claude Opus 5.5 at high effort for every other correctness-critical slice (each is gated by an independent reference and reviewed at its final commit, so the Fable premium there buys review rounds, not a safety property) and for real-data stages; Claude Sonnet 5.5 for documentation, package preparation and deletions. High effort, not higher: the contracts are written, and the previous two plans ran Opus at high without a steer attributable to effort. Reviewer: Claude Fable 5.1, high. | Per the profiles table | all |

---

## Frozen Decisions

| # | Decision | Binding statement |
|---|---|---|
| F1 | **The ruling.** | `format_version` stays 3. For `consistent_trees_ascii` the canonical unit is the whole forest, units are ordered by ascending `ForestIndex` (the dense enumeration of the catalogue's forest ids, `convert/mimic-convert/scatter.py:171-184`, the same enumeration the vertical reader builds at `src/io/vertical/read_ctrees_ascii.c:460-483`), the row ordinal is `HaloRankInForest`, and `SourceHaloID = 1 + Σ_{g < ForestIndex} n_g + HaloRankInForest` with `n_g` the forest's halo count over all snapshots, aborting on int64 overflow. Every slab is therefore forest-blocked and the ASCII route joins the position check the two prelinked routes already pass (`convert/mimic-convert/validate_v3.py:1333-1346`). Unchanged: `ForestIndex`, `HaloRankInForest`, `UniqueGalaxyID`, every payload and link value and chain order, the header attribute set, the object set, every invariant, and the sidecar, whose `ForestID` is the source forest id and whose ordinals are, for a forest held in one file, that file's ordinal and the forest's unit ordinal in it (its rank among the file's forests ordered by the file position of their first `#tree` marker, `convert/mimic-convert/ctrees_parser.py:593-603`), or −1/−1 for a forest spanning files. It is recorded in the format's errata as an explicit second owner carve-out, a conformance change and not a correction (the preamble at `HORIZONTAL-HDF5-FORMAT.md:191` admits exactly one carve-out today, which preserved conformance): a version 3 ASCII dataset written before the ruling is recognisable because its `SourceHaloID` is not its (`ForestIndex`, `HaloRankInForest`) position, fails the battery and must be reconverted; an old ASCII workdir does not resume. The route refuses sampling; an ASCII subset is a separate catalogue with its own enumeration. The ruling does not lean on the 2026-09-29 precedent and does not extend to any other route. The verbatim text is Slice 1's acceptance criteria. |
| F2 | **Identity is pinned.** | The identity multiplier is `simulation.unique_galaxy_id_multiplier` (`src/core/read_parameter_file.c:1055-1066`; not a header attribute). It is **2 × 10¹⁰ for every Shin-Uchuu dataset** (uncut, cut, and the subset), already declared by `simulations/shin-uchuu/simulation_info.yaml:27` and `simulations/shin-uchuu-ascii/simulation_info.yaml:30`, and 10⁹ for micro-Uchuu; the decided cut (F6) yields 244,953,607 forests, one per z = 0 FoF group. Even the whole-simulation worst case of one forest per tree, 315,004,242 forests, is inside the 461,168,600 the multiplier admits (`INT64_MAX / multiplier − 1`, `src/include/galaxy_id.h:37-40`). Revision 4's 271,392,048 was a super-forest-only figure. An uncut migration changes no `ForestIndex`, `HaloRankInForest` or `UniqueGalaxyID`, so HOD (`models/hod/modules/hod_populate/hod_populate.c:455`) and SHAM (`models/sham/modules/sham_rank_match/sham_rank_match.c:658-659`), which key on the id, stay byte-identical as well as `sage16` and `halos-only`, which never read it. |
| F3 | **Scientific identity and its acceptance matrix.** | Two datasets are scientifically identical for a model under a named run configuration when the galaxy set is the same and every output field is byte-identical for matched galaxies (`scripts/compare_cross_format_identity.py`, no tolerance). Tree galaxies match by `UniqueGalaxyID` where the forest enumeration, ranks and multiplier are unchanged, else by (`MostBoundID`, `SnapNum`); created records (HOD satellites) match by birth lineage, (host `MostBoundID`, `SnapNum`, creation ordinal decoded from the negative id, `src/include/galaxy_id.h:61-72`), which Slice 14 teaches the comparator. In those alternate modes the matching key itself (`UniqueGalaxyID`, which the brief allows to differ) is not a compared field, and the one other identity-valued output field, `UniqueCentralGalaxyID` (`src/core/core_properties.yaml:90`), is compared through the matched mapping (the left record's central must map to the right record's central); every other field is compared byte for byte, and the default mode is unchanged. The matrix: `halos-only` and `sage16` vertical against horizontal, fixed and dynamic schemes, serial; horizontal serial against chunked and against `mpirun`; `sham` and `hod` horizontally only, serial against `mpirun` (collective), and version 2 against version 3 where both exist. No shipped module sums floats across a snapshot, so no last-bit allowance is used. |
| F4 | **The rank is derivable from version 2 columns.** | `HaloRankInForest` is the dense position of a halo in the lexsort of (`ForestIndex` asc, `SnapNum` desc, upid asc, pid asc, `MostBoundID` asc) with upid the FoF central's `MostBoundID` (the `FirstHaloInFOFgroup` row's) and pid −1 for a central else the central's id (`convert/mimic-convert/fixups.py:386-426`, `rank_sort.py:13-24`; 0 mismatches over 22,580,924 micro-Uchuu halos). The progenitor chain order is the reference incremental-insertion loop over the encounter order (upid, pid, id) within a slab (`links.py:191-195`, `:370-380`). Both are reproduced in the rewriter, in int64 throughout. |
| F5 | **The rewriter.** | `convert/mimic-convert/rewrite_trees.py` with a `rewriter/` package: one tool reading a horizontal dataset slab by slab and writing a version 3 dataset under a forest table, for Consistent-Trees-derived datasets with `links_adjacent` 1 only (anything else refused; the gapped case is out of scope). Pass 0: headers, sidecar, the index files (`forests.list`, `locations.dat`), the forest tables, and per-forest halo totals from its own scan of every slab's `ForestIndex` (about 180 GB read, under an hour; no shortcut from the census, so nothing has to be bound to it). Pass 1, in descending snapshot order with a per-forest running count: per slab the new row permutation and (`ForestIndex`, `HaloRankInForest`, `SourceHaloID`) per row, to a remap store of which only slabs N−1, N and N+1 are resident. Pass 2, trailing by one slab: payload rewritten in the new order, every link remapped through the target slab's permutation (`Descendant` at N+1, `FirstProgenitor` at N−1, the rest at N) with each `*Snapshot` companion −1 iff its link is −1, `/schema` carried through for a version 3 input and built from the package's profile for a version 2 input, the header attributes, the sidecar, a per-slab verification, and a manifest of the shape the battery binds to (`validate_v3.py:508-571`, `:1506-1539`, `:675-714`) with per-artefact SHA-256, the pinned a_list, an inventory whose totals and per-snapshot counts come from the input or the cut table and that declares no physical units, and the embedded schema. Three operations: **(M)** version 2 to version 3; **(C)** version 3 to version 3 under a cut table; **(N)** version 3 to version 3 under the identity table, which must reproduce its input exactly in every dataset value, dtype and metadata item. Scratch and output locations are explicit arguments; nothing defaults to the repository. The output directory is a converter-shaped workdir: `<output>/manifest.json`, `<output>/dataset/` (registered as the manifest's `write` stage directory, holding the snapshot files and `forests.h5` and nothing else, since the battery rejects unregistered entries, `validate_v3.py:521-530`), the per-tree inventory and the conversion report beside the manifest; the manifest's `configuration` carries the keys the report reads (`record_dtypes` with `ingest` and `transposed` itemsizes, `ingest_max_rows`, `transpose_budget_bytes`, `report.py:442-456`) with the rewriter's own values, and its `ingest` and `transpose` stage results carry `snapshot_counts`, `total_halos`, `n_links`, `n_gapped_descendants` 0, `max_descendant_span` 1 and `links_adjacent` 1. |
| F6 | **The cut.** | A cut is a declared transformed source: the original index files, the original forest table, a **cut forest table** (a `forests.list`-shaped file, tree root to forest id, satisfying these invariants, each refused when violated: every tree root of the index files appears exactly once and no other root appears; a piece holds trees of exactly one original forest; an uncut forest keeps its id; of a cut forest's pieces exactly one, the largest by total halos over all snapshots with ties broken by the smallest tree root id, keeps the original id and every other receives a fresh id above the catalogue's maximum, unique across the table, assigned in descending piece size with the same tie rule, so that every untouched forest keeps its `ForestIndex`) and one deterministic transformation: hosts are resolved under the original partition (the post-fix-up central the dataset records), the cut table is installed, and every halo whose resolved central lies in another forest of the table becomes a central (`FirstHaloInFOFgroup` itself; upid = id, pid = −1 in the rank key); no host relation is invented. Tree membership is the terminal-root label (the label of the descendant, else the halo's own `MostBoundID`), computed in pass 1's descending order. The logical descendant relation never changes: the numeric `Descendant` index is remapped through the target slab's permutation like every link, and the per-slab verification checks that each halo's descendant carries the same `MostBoundID` as before; `DescendantSnapshot` is unchanged. For every forest the table changes: FoF chains, `HaloRankInForest` and progenitor chain order are recomputed under F4; for every other forest payload and topology are reproduced exactly and `UniqueGalaxyID` is unchanged. `SourceHaloID` is recomputed for every row (its prefix moves), `n_forests_total` and `max_halo_rank_in_forest` describe the new census, and a piece spanning files carries −1/−1. The ASCII route's fix-up stage applies the same rule behind a declared cut table; without one it refuses a cross-forest host as today. The table's md5 and the transformation go into the conversion report, the manifest and the package README's provenance section. Revision 5 fixes the table: see [The decided table](#the-decided-table-revision-5). |
| F7 | **The census.** | Units are **effective descendant trees** (terminal-root components of the descendant graph; equality with Consistent-Trees' physical `#tree` blocks is not established and is not claimed; the root set is checked against the `forests.list` tree roots and the per-file row counts through `locations.dat` against the production report, a necessary and not sufficient check). Edges are undirected tree pairs (the smaller tree ordinal first) joined by a FoF co-membership between halos of different trees in the effective (post-fix-up) graph, each pair counted once per snapshot for its duration and accumulating the halos involved and their mass, aggregated in bounded per-slab edge lists merged by key (no dense tree × snapshot matrix). The components of the complete effective graph are themselves a census output: a forest that the effective relations already leave in several components is separable without severing anything, and the record says so rather than assuming connectivity. Candidate rules are duration and weight thresholds on those edges, each yielding components, a cut table under F6's naming, the severed relations and promotions per snapshot, the progenitor-order changes, the re-labelled halos, the predicted identity-dependent effects per model, and a simulation of `horizontal_partition_cut` (`src/core/horizontal_partition.c:106-182`: minimax contiguous packing by binary search over capacity, weights = the widest slab's per-forest row counts, lowest-numbered widest slab on ties, chunks packed within each task's range) for candidate task and chunk counts, giving the widest chunk in every slab. The raw `pid`/`upid` graph is not available without the source and is recorded as optional. Revision 5 supersedes the candidate rules: see [The census after revision 5](#the-census-after-revision-5). |
| F8 | **The reference reader stays pure.** | The C reference reader (`src/io/vertical/ctrees/ctrees_utils.c`) learns nothing; the independent leg feeds it transformed ASCII fixtures whose host columns are canonicalised to the original resolved centrals and then severed per F6, through the vertical driver. |
| F9 | **Fixtures.** | The committed version 2 fixtures (`simulations/micro-uchuu-ascii-horizontal/_tests/data/`, `.../data/generic/`, `simulations/shin-uchuu/_tests/data/`) stay in place and keep their consumers until Stage D, when a version 3 fixture converted by the Stage A route from a committed synthetic ASCII source replaces them on the pattern of `simulations/mini-millennium-horizontal/_tests/data/source/`. |
| F10 | **Placement of data and scratch.** | Datasets, conversion workdirs, census aggregates and rewriter scratch go on the LaCie through explicit arguments. Gate scratch and the subset-scale run outputs go where `output/` resolves (`/Volumes/Internal/results/mimic`; `tests/framework/parity_gate.py:484-487`), which is the owner's rule for `Internal`; the brief's "override the gate harness's default" is therefore not needed and is dropped. Production run outputs (0.55 TB each) follow the brief's ledger exactly: `Internal` holds one at a time, so a new production output is written to the LaCie, compared, and moved to `Internal` only after the output it replaces has left for NT. The identity references for serial-versus-distributed comparisons live under `archive/distributed-references/`. |
| F11 | **Records.** | Standing evidence: `docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md` (one section per stage, every number quoted elsewhere traced to it) and `docs/dev/MIMIC-SHIN-UCHUU-FOREST-CENSUS.md` (Stage B's record and the owner's decision). Nothing outside `docs/dev/` cites either; durable facts migrate to the guides, the format document, the package READMEs and the skills in Slices 6, 17 and 24. |


### The decided table (revision 5)

Owner, 2026-10-09. Every tree joins the forest of its terminal halo's z = 0 FoF group, for every forest of the simulation.
- **Construction:** the group is identified by the z = 0 central's tree, and the table is built from the dataset's final snapshot (its scale factor recorded). The rule's scope is catalogues in which every tree reaches the final snapshot, as Consistent-Trees guarantees. A catalogue containing a tree whose terminal halo lies before the final snapshot is refused, naming the count. The three real catalogues have none (Shin-Uchuu production 0 of 315,004,242 trees; micro-Uchuu versions 2 and 3 0 of 561,266).
- **Measured on production:** 244,953,607 z = 0 FoF groups.
  - The super-forest holds 50,805,833 of them.
  - 8,053,561 other forests end in more than one group, together holding 27,600,004 extra groups and 4,519,034,547 halos.
  - The remaining forests end in exactly one group and are unchanged.
- **Equivalence:** this is exactly the partition that cuts every co-membership ended before the final snapshot and keeps every one present at it. The super-forest's every-ended piece count equals its z = 0 group count, 104,845,278 trees less 54,039,445 z = 0 cross-tree links.
- **Guarantees:**
  - no halo is promoted at the final snapshot;
  - every z = 0 FoF group of the input is preserved;
  - no tree is split.
- **Naming:** unchanged. Each forest's largest piece keeps its id; fresh ids sit above the catalogue maximum, assigned in descending size with the tie rule.
- **"Ended" is an operational definition** fixed by the final snapshot of the dataset. It is not a claim that an encounter was an artificial join or a physical flyby. The cut uses the final snapshot to decide which groups a halo belonged to earlier, so it is a dataset definition, not a causal physics prescription.

### The census after revision 5

The candidate-rule exploration in F7 was measured in run 2 and superseded by the owner's decision (F6, the decided table). The co-membership graph, the threshold rules, union-find components, the multi-rule grid and the rule preview are retired by the [Final-State Inventory](#final-state-inventory).

What the census keeps:
- the bounded readers, occupancy, effective-tree labelling with the correspondence and conservation checks, and the partition simulation;
- the cut-table format with its invariants and record;
- for the decided table, over every split forest:
  - the promotions per slab;
  - the progenitor-order changes under F4, with the stored-chain check;
  - the affected-history bracket:
    - `dependent_halos`: the seeds, plus every halo on a seed's descendant path. The seeds are the promoted halos, the centrals of groups that lose members, and the descendants whose progenitor chain changes.
    - `upper_bound_halos`: every halo of a piece that holds a seed;
  - the re-labelled halos;
  - the partition with the pieces installed, judged on job memory.

Run 2's measured exploration (the rule preview and PM's informal estimates) is recorded in the census record as the evidence for the decision, labelled as informal where it is.

---

## Outcome and Limits

After this plan the Consistent-Trees ASCII route of `convert_trees.py` emits forest-blocked version 3 datasets whose `SourceHaloID` is the (`ForestIndex`, `HaloRankInForest`) position, under a recorded owner ruling; `micro-uchuu-ascii-horizontal` and `shin-uchuu` are version 3 packages with version 3 datasets; a rewriter migrates, cuts and self-tests horizontal datasets without reading a source; every Shin-Uchuu forest is partitioned into its z = 0 FoF groups under the owner's rule (F6), and the cut dataset is validated by a bounded battery and run under `sage16` with chunked slab streaming at a calibrated laptop budget; and no version 2 code, fixture, test or prose remains.

**What it does not do.** It does not claim HOD or SHAM identity on the cut dataset (decision 7). It does not convert full Uchuu, write a gapped-link rewriter, add sub-forest decomposition to the driver, or re-read the 11.61 TB source (all recorded out of scope by the brief). Laptop acceptance is a Mac-measured proxy unless a laptop of the class runs the sweep. The subset gates exercise the whole-simulation partition at subset scale. They exclude the production super-forest, so its cut is covered only by the production battery and the science checks of Stage C.

---

## Final-State Inventory

*Revision 5. Owner, 2026-10-09: "if any of the choices made here simplify the overall final state of anything we've added or changed in mimic, we should make sure that is cleaned up as well before the full plan has concluded."*

The rows below cover what this plan adds, and the earlier work whose final state revision 5 changes. Everything else the plan touches reaches its final state through its own slice's contract and Stage D's retirement slices. Nothing is deferred past the plan. Slice 24 confirms every row and greps for leftovers (its final-state check). Removed code leaves the tree; history keeps it.

| Artefact | Final state | Owning slice |
|---|---|---|
| `horizontal_dataset.py` (bounded reader, identity record) | keep; version 2 branch removed | 22 |
| `source_index.py` (index files, forest table) | keep; `FileID`-to-converter-ordinal mapping and corrected memory figures added | 11 |
| `census/aggregate.py`, `census/occupancy.py`, `census/trees.py`, `census/partition.py` (including `slab_prefix`) | keep | 7, 9 |
| `census/graph.py`: co-membership edge lists, external merge, components | **remove**; every helper the surviving census still imports moves to the module that uses it | 9 |
| `census/rules.py`: threshold rules, union-find | **remove** | 9 |
| `census/cut.py`: rule lists, rule names, the multi-rule pass, rule-keyed outputs | **simplify** to the decided table. The per-slab accounting, piece installation, partition rows and laptop rows survive | 9 |
| `census/cut_table.py` (F6 naming, invariants, streamed record) | keep. The rewriter's table reader reuses it rather than restating the invariants | 9, 12 |
| `forest_census.py` CLI | **simplify**: no `graph` subcommand, no rule options; the decided-table builder added | 9 |
| `tests/test_forest_census.py`; `tests/fixtures.py` correspondence forests | retired cases go with their code; the rest keep | 9 |
| `rewrite_trees.py` and `rewriter/`: `migrate` (M) | **remove** with version 2 | 22 |
| `rewrite_trees.py` and `rewriter/`: `noop` (N) and `cut` (C) | keep. (C) keeps the general declared table (F6), because the table is the provenance artefact and the adversarial fixtures need arbitrary tables | 11, 12 |
| `convert_trees.py ingest --cut-table` (route) and `tests/tools/transform_ascii_fixture.py` | keep: the independent gate of (C), and a documented route for converting a catalogue directly into its cut form | 13 |
| Comparator lineage and catalogue modes; the cut-difference report | keep | 14 |
| Parity-gate manifest-proved inventory and dataset override | keep; version 2 branches removed | 19 |
| `validate_bounded.py` and the external-sort battery | **to decide inside the plan, not after it.** Slice 16 records the agreement. The revision before run 6 freezes the disposition in Slice 22's placeholder criterion, and run 6 does not start without it. Revision 4's "post-run review" wording is withdrawn | 16, 22 |
| Census documentation: the converter manual's section | keep, describing the tool as it finally is | 9 |
| Census documentation: skills and guides | keep, describing the tool as it finally is | 17, 24 |
| Any text describing candidate rules, the co-membership graph or thresholds as live features | **remove** | 9, 24 |
| The records under `docs/dev/` (the acceptance record, the census record) | standing evidence after the plan closes (F11). This plan and the brief (`MIMIC-SHIN-UCHUU-V3-PLAN.md`) are archived on merge | 24, then the owner on merge |
| LaCie census aggregates; run 2's retired graph and candidate-cut outputs (moved aside in Slice 9) | The accepted cut tables and records are kept with the conversion provenance. Every other aggregate is deleted by the owner, the `trees` labels and tree pairs only after the last step that reads them | procedure step 12 |

---

## Repository Evidence

Anchors every slice relies on, read at the baseline.

- **Identity assignment.** `convert/mimic-convert/adapters/ctrees_ascii.py:790-820` (`_source_ids`, the only place the ASCII `SourceHaloID` is computed, from physical (file, unit, row) coordinates stamped at scatter time); `:473-539` (`iter_forests`, the sidecar records, −1/−1 when a forest has more than one populated unit); `:670-754` (`_build_inventory`); `:595` (slab order is the argsort of `SourceHaloID`, so the new order follows from the new ids). `ForestIndex` and `HaloRankInForest` exist only after `links` (`links.py:1138-1139`). Per-forest totals are available before any write from the units sidecars and `forest_index_table.npy` (`scatter.py:1366-1371`).
- **Inventory contract.** `adapters/base.py:196-304` (`SourceInventory`: strictly ascending (file, unit), non-negative ordinals, `base_id`, `coordinate` inversion); `:409-439` (`CanonicalBatch` demands positive strictly increasing ids and non-negative coordinates). `lhalo_binary.py:518-560`, `:639` and `ctrees_hdf5.py:963-1006`, `:1340` use the inventory unchanged.
- **Manifest and resume.** `pipeline.py:379-415` (`_ascii_parameters`, inside the configuration digest), `:631-643` and `conversion_manifest.py:961-984` (`require_inventory`, compared on every resume), `:394-453` (`inventory_record`). Nothing today distinguishes an old ASCII workdir.
- **Battery.** `validate_v3.py:119` (`_V3_UNIT_FORESTS`), `:1300`, `:1333-1349` (position check), `:1445-1465` (ASCII sidecar branch), `:1487-1502` (per-file inventory check, which −1 ordinals break), `:1398-1419` (sampling branches), `:508-571`, `:1506-1539`, `:675-714` (manifest binding), `:1630-1631`, `:1733-1736` (SKIP without a manifest), `:718-787`, `:1658` (external sorters and spill), `:839-849` (per-file ascending `SourceHaloID`).
- **Stale statements.** `report.py:333-338`; `convert_trees.py:346-356`; `column_schema.py:505-515`; `README.md:94`, `:134`; `docs/USER-GUIDE.md:543`, `:563`; `docs/DEVELOPER-GUIDE.md:1227` (and `:1206`, `:1240`, `:1248`); `CHANGELOG.md:12`, `:20`; `.agents/skills/mimic-run-and-operate/SKILL.md:129`, `mimic-simulations-and-readers/SKILL.md:61`, `:104`, `mimic-debugging-playbook/SKILL.md:77`, `mimic-docs-and-writing/SKILL.md:73`, `mimic-validation-and-qa/SKILL.md:141`, `mimic-config-and-flags/SKILL.md:108`, `:113` and `references/all-config-keys.md:51`, `:55`; `simulations/micro-uchuu-ascii-horizontal/README.md:62`; `simulations/shin-uchuu/README.md:31`, `:42`.
- **Acceptance harness.** `convert/mimic-convert/tests/run_generalisation_acceptance.py:222` (`UNIT_FOREST_FORMATS`), `:836-846`, `:1215-1220`, `:1282-1290` (the C-dump identity check skips ASCII), `:1881` (the ASCII extractor's physical id), `:2010-2050` (`compare-extras` joins on `SourceHaloID`). `tests/tools/unit_ordinals.py` never reads `SourceHaloID` and is unchanged.
- **Gates.** `tests/framework/parity_gate.py:1095-1124` (the sidecar check infers the inventory from `SourceFileOrdinal` and fails on any −1); `simulations/micro-uchuu-ascii-horizontal/_tests/scientific/test_cross_format_identity.py:70-89` (version 2 pins), `:489` (`VersionTwoGate` with Stage 8); `scripts/compare_cross_format_identity.py:333-347` (created rows excluded by default), `:490-495` (`--compare-created`); `tests/manual/test_distributed_identity.py:110-120` (chunked legs), `:152-155`, `:586-607` (version 2 refusal).
- **C side.** `src/io/horizontal/read_horizontal_hdf5.c:2787-2789` loads `SourceHaloID`; nothing under `src/` or `models/` reads `slab->source_halo_id`. Forest blocking is enforced only by the driver under distribution or chunking (`src/core/horizontal_driver.c:2399-2412`, `:984-993`, `:1073-1080`; `src/core/horizontal_partition.c:208-246`). `:1553-1577` rejects an `int` link declaration against version 3. Version-2-only code is about 520 lines of the reader plus the driver's refusal and fallbacks (`:1073-1080`, `:1430-1445`); `horizontal_h5_dataset_spec_by_name` (`:1806`) must be repointed at the version 3 fixed table before the version 2 table goes.
- **Packages.** `simulations/micro-uchuu-ascii-horizontal/halo_properties.yaml` and `simulations/shin-uchuu/halo_properties.yaml` differ from a version 3 ctrees package only in the five link types (`int` to `long long`); neither ships `converter_columns.yaml`, and `simulations/shin-uchuu-ascii/` ships none either, while `simulations/micro-uchuu-ascii/converter_columns.yaml` is the shipped default profile with package comments.
- **Version 2 inventory.** `convert_ctrees.py` (300 lines, version 2 only); `hdf5_writer.py` (about 460 of 578 lines version 2 only; `CHUNK_1D`, `HEADER_ATTRS`, `_log`, `load_header_metadata`, `snapshot_h5_name` are imported by `hdf5_writer_v3.py:60-66`, `validate_v3.py:59`, `convert_trees.py:223`); `validate.py` (about 1,560 of 1,767; `DEFAULT_MULTIPLIER`, `DEFAULT_V3_BUDGET_BYTES`, `RUN_SCOPED_ATTRS`, `V3_FORMAT_VERSION`, `Outcome`, `_examples`, `_filter_failures`, `battery_failed`, `check_header_bounds` are imported by `validate_v3.py:68-78`, `report.py:34-39`, `convert_trees.py:760`); `report.py:107-296` (`build_report`, `render_text`, `write_report`, `run_report`; the version 3 constants from `:298` stay); `scatter.py:1499-1582` (`run_release`, `run_finalize`); `crosscheck.py`; the default `mimic-topology-dump v1` mode of `tests/unit/tools/dump_ctrees_topology.c` (`:25-34`, `:119-130`) exists only to feed crosscheck. The stage modules `scatter.py`, `sort_index.py`, `fixups.py`, `links.py`, `ctrees_parser.py`, `rank_sort.py` are shared with the version 3 ASCII route (`adapters/ctrees_ascii.py:82-98`, `:319-333`) and stay. Tests: `test_validate.py:1-2180`, `test_hdf5_writer.py:79-918`, `test_cli.py:685-733`, `test_scatter.py:1823-1920`, `test_crosscheck.py`; `test_links.py`, `test_fixups.py`, `test_sort_index.py`, `test_ascii_adapter.py`, `test_conversion_manifest.py` and `mock_reference.py` use the version 2 writer or CLI as setup. C tests: `simulations/micro-uchuu-ascii-horizontal/_tests/unit/` (2,079 + 409 + 159 lines); the 18 cases and the 36-row corrupt-input and 8-row corrupt-link tables of `test_unit_horizontal_reader_open.c` against the 15 cases and 85 rows of `tests/unit/test_horizontal_v3_reader.c`.
- **Local data.** Version 2 Shin-Uchuu at `/Volumes/LaCie/data/uchuu/shin-uchuu/` (71 files, 2,251,984,563,372 bytes; provenance under `conversion-provenance/shin-uchuu-v2/`: `conversion_report.{json,txt}`, `manifest.json`, `forest_index_table.npy`, `forest_max_snap.npy`); the subset ASCII at `/Volumes/LaCie/data/uchuu/shin-uchuu-subset-ascii/` (2,744 tree files, `forests.list`, `locations.dat`, 210,418,634,152 bytes; 406,668,896 halos, 6,011,205 forests, 8,000,198 trees); micro-Uchuu ASCII at `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii/` (one file, `forests.list`, `locations.dat`); the version 2 micro-Uchuu dataset beside it; the version 2 production `sage16` run output at `/Volumes/Internal/results/mimic/sage16-shin-uchuu`. The production `forests.list` (7.56 GB) and `locations.dat` are not local. LaCie 3.8 TiB free, Internal 962 GiB free at the baseline. `mimic_venv`: Python 3.14, h5py 3.15, numpy 2.3; no pytest (converter tests run under `unittest`).

---

## Execution Structure, Implementation Profiles and Run Preparation

**Six runs, one plan (and run 2b).** PM executes the slices in plan order, one fresh Developer per slice, no batching. The stage boundaries are run boundaries: run 1 is Stage A (Slices 1 to 6); run 2 is Stage B (Slices 7 to 9). Revision 5 (owner, 2026-10-09) is an exceptional amendment beyond the boundary revision originally foreseen here. Run 2 accepted Slices 7 and 8 and stopped Slice 9; the owner decided the cut rule (F6) and amended Slice 9, F7, decisions 6 and 7, and the slices and procedure steps the [Final-State Inventory](#final-state-inventory) names. Run 2b runs the amended Slice 9, attesting Slices 1 to 8. The three performance commits run 2 left above Slice 8's commit (`ade3b7f0`, `003051df`, `72f7695f`) were drift-audited in run 2. Their surviving helpers are reviewed with the amended Slice 9. After run 2b the owner records the laptop class and stop condition (decision 6). Run 3 is Stage R (Slices 10 to 15); run 4 is Slice 16, after which the owner runs the production procedure; run 5 is Slice 17; run 6 is Stage D (Slices 18 to 24). Each later `init` attests the earlier slices (`--attest`). A stage's slices are independently gateable and nothing in a later stage is required by an earlier one.

| Slice | Stage | Recommended Developer | Effort | Reason |
|---|---|---|---|---|
| 1 | A | Claude Sonnet 5.5 | high | The ruling's verbatim text into the format document; prose only, text supplied |
| 2 | A | Claude Opus 5.5 | high | The adapter's canonical unit, identity, inventory, manifest binding and resume refusal, with the harness's ASCII extras join moved to the catalogue id so the converter floor holds: the identity convention every later gate compares against |
| 3 | A | Claude Opus 5.5 | high | The battery's tuple split, the ASCII position check, sampling refusal, and the stale converter statements |
| 4 | A | Claude Opus 5.5 | medium | The acceptance harness: ASCII joins the C-dump identity check |
| 5 | A | Claude Opus 5.5 | high | The micro-Uchuu conversion, the package moved to version 3 in place, its gate, the distributed, chunked and SHAM legs, and the acceptance record |
| 6 | A | Claude Sonnet 5.5 | high | Guides, skills, changelog, tests README, pathway |
| 7 | B | Claude Opus 5.5 | high | Bounded slab column reader, index-file reader, occupancy, effective trees, the partition simulation with its C oracle |
| 8 | B | Claude Opus 5.5 | high | The co-membership graph, candidate rules, deterministic cut table, predicted effects: the measurement the owner decides on |
| 9 | B | Claude Opus 5.5 | high | Revision 5: the z = 0 FoF-group table and its census accounting for the whole simulation, the removal of the exploration machinery, the micro-Uchuu rehearsal, the production census and the record |
| 10 | R | Claude Sonnet 5.5 | medium | `shin-uchuu` and `shin-uchuu-ascii` package preparation for version 3 |
| 11 | R | Claude Fable 5.1 | high | The rewriter's passes, (M) and (N), manifest, sidecar and verification |
| 12 | R | Claude Fable 5.1 | high | The (C) operation under F6 with its adversarial fixtures |
| 13 | R | Claude Opus 5.5 | high | The ASCII route's fix-up stage behind a declared cut table; the independent reference leg's transformed fixtures |
| 14 | R | Claude Opus 5.5 | high | The comparator's lineage key; the parity gate's manifest-proved inventory; the version 3 ASCII gate for the subset |
| 15 | R | Claude Opus 5.5 | high | The subset conversions and gates G3, G3b, G3c, G4 with the HOD leg, determinism and resume, and the record |
| 16 | C | Claude Opus 5.5 | high | The bounded battery: every obligation with a stated storage ceiling, verdict agreement before trust |
| 17 | C | Claude Sonnet 5.5 | high | After the production procedure: the package README's provenance, the acceptance record, pathway and changelog |
| 18 | D | Claude Opus 5.5 | medium | Utility extraction out of the version 2 modules |
| 19 | D | Claude Opus 5.5 | high | The version 3 ASCII fixture from a committed synthetic source; every consumer retargeted |
| 20 | D | Claude Opus 5.5 | medium | The version 2 reader's negative battery ported to the version 3 reader test |
| 21 | D | Claude Opus 5.5 | high | Deletion of the version 2 reader path |
| 22 | D | Claude Opus 5.5 | high | Deletion of the version 2 converter, validator, crosscheck and their tests; the rewriter's version 2 input |
| 23 | D | Claude Sonnet 5.5 | high | Deletion of the version 2 fixtures, generators, make target and CI step |
| 24 | D | Claude Sonnet 5.5 | high | The format document's version 2 text frozen as history; guides, skills, changelog, pathway closeout |

Pass the table's model and effort explicitly to `start-slice` and record the resolved model versions. Reviewer: Claude Fable 5.1 (`claude-fable-5-1`), high effort, fresh sessions for drift audit and code review. From run 2, by the owner's standing instruction, the drift audit runs on Claude Sonnet 5.5 at high effort, and Codex `gpt-6.1-sol` at high effort joins as a second code reviewer. An unavailable model is a setup blocker, never permission to substitute silently.

**Approvals.** Slices marked `Approval needed before implementation: yes` need a recorded human approval (`approve`) before they start; this document grants none. Slices 4, 7, 10, 18 and 20 run unattended.

**Before run 1,** in order:

1. Commit this plan, the pathway update and the brief's status note. PM's `init` binds the run to this file's bytes and refuses a dirty tree outside `.pm/`.
2. `python3 ~/.claude/skills/project-manager/scripts/pm.py check-plan --plan docs/dev/MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md --repo .` and resolve every warning.
3. Confirm `make`, `make USE-MPI=yes` (Open MPI `mpicc` and `mpirun`), `mimic_venv` with h5py and numpy, HDF5, and that `simulations/micro-uchuu-ascii/snapshots` and `simulations/micro-uchuu-ascii-horizontal/snapshots` resolve.
4. Confirm `make tests-converter`, `make tests-horizontal-v3` and `MPIRUN="mpirun --oversubscribe" make tests-distributed` pass at the baseline; record the distributed gate's live check count.
5. Choose the LaCie directories for the Stage A workdir and the version 3 micro-Uchuu dataset (about 3 GB), and record where the version 2 micro-Uchuu dataset is retained for Slice 15's gate G3.

**Before run 2:** the owner's 0.72 TB of unrelated files have left the LaCie (brief, "The two workflows"); the production `forests.list` (7.56 GB) and `locations.dat` (about 13 GB) have been transferred by the owner to the LaCie beside the version 2 dataset's provenance directory, with their md5 (the production report records the `forests.list` md5 `60dbf14a99ae9f23ca7c334474477a95`). **Before run 2b:** revision 5 committed. **Before run 3:** run 2b accepted; the laptop class, its usable budget, the task and chunk configuration and the stop condition recorded in decision 6 (the rule and its scope are fixed in F6). **Before run 4:** nothing beyond run 3's acceptance. **Before run 5:** the production procedure is complete and its record accepted. **Before run 6:** run 5 accepted; Slice 22's battery placeholder replaced by the decided text.

## Resource Profile

What each slice and procedure step demands of the Mac Studio (512 GB RAM, 32 cores, the LaCie over its external bus), so the owner can keep the right resources free. Durations are rough estimates from the recorded figures: the version 2 production conversion's 6 days and its link stage's 235 GB RSS, the corrected version 2 `sage16` run's 9 h 35 m at 513 GB peak RSS, the measured 71.9 MB/s pooled ASCII scatter rate, the chunked streaming record's about 1.1 KB per resident halo, and the converter manual's storage-per-stage figures; the external bus is assumed to sustain about 200 to 400 MB/s, which the first production step measures. "Keep free" names the memory the owner should not have other applications holding while the step runs; everything not listed is a PM Developer session (compiling, `make tests-converter` at about 5 min, the unit and integration tiers at about 3 min each) with a few GB of RSS and light I/O.

| Slice or step | I/O | Memory (peak RSS) | CPU | Rough duration | Keep free |
|---|---|---|---|---|---|
| 1 to 4 | light | a few GB | light | under an hour of machine time each | no |
| 5 | moderate: the micro-Uchuu ASCII file (12 GB) read several times; about 3 GB written | under 20 GB (the ASCII inventory and the gate's runs) | moderate during the conversion and the eight gate runs | 3 to 6 h of machine time, mostly waiting on runs | no |
| 6 | none | a few GB | light | under an hour | no |
| 7, 8 | light (fixtures) | a few GB | light | under an hour each | no |
| 9 | **heavy**: the production census reads the LaCie for the decided table's accounting pass over every split forest (about 17 bn halos, three to five columns), reusing run 2's occupancy, trees and partition aggregates. Measured in run 2 at `5b7edec1`: occupancy 1,006 s at 32.7 GB peak RSS; trees 2,768 s at 41.7 GB; graph 2,581 s at 35.1 GB; partition 421 s at 27.4 GB. The 12-rule candidate `cut` reached 174 GB RSS before its fixes and projected about 2 h for its pass after them. The aggregate directory measured about 291 GB in all (labels 90 GB, per-slab tree pairs 89 GB, occupancy pairs 62 GB, graph 25 GB); the graph and the candidate `cut` outputs are retired | 174 GB was measured for the retired 12-rule pass. The decided-table pass over every forest has its phase bounds stated and its RSS measured in Slice 9; the historical figure is not its ceiling. The host has 512 GB. | moderate, single process | a few hours | no, but do not run another LaCie-bound job alongside |
| 10 | none | a few GB | light | under an hour | no |
| 11 to 14 | light (fixtures) | a few GB | light | under an hour each | no |
| 15 | **heavy**: two subset conversions through the route (the reference, and the one under a cut table) each read 210 GB of ASCII and spill about 0.5 TB, plus the version 2 subset conversion; the census on the subset reference; three subset rewrites each read and write about 60 GB; the gates run `sage16` and `halos-only` on 406 million halos vertically and horizontally eight times, plus the SHAM, HOD, chunked and `mpirun` legs | 40 to 80 GB (the ASCII inventory at about 5 GB, the rank stage, the battery's spill buffers; a subset horizontal run holds its widest slab at about 1.1 KB per halo) | high during the conversion (pooled scatter) and the runs | 12 to 24 h of machine time, across several sessions | yes, 100 GB, during the conversion and the vertical `sage16` legs |
| 16 | moderate: both battery modes over the two subset datasets (about 120 GB read twice) | bounded mode: resident columns of the widest subset slab plus the bitsets, a few GB; the external-sort mode as measured in Slice 15 | moderate | 2 to 4 h | no |
| Procedure 2 (only if the reference is re-run) | heavy: the version 2 dataset (2.25 TB) read once; 0.55 TB written to `Internal` | **513 GB** (the uncut, unchunked version 2 run; the whole machine) | one core for 10 h | about 10 h | **yes, everything** |
| Procedure 3 (M) | **very heavy**: 2.25 TB read, 3.25 TB written, 3.25 TB re-read to verify, plus the 0.05 TB remap store | 30 to 60 GB (one 519 million-row column at a time, the permutation, the remap window) | moderate (one `lexsort` per slab) | 12 to 20 h | yes, 100 GB |
| Procedure 4 and 9 (bounded battery) | heavy: 3.25 TB read about twice | about 40 GB by the stated ceiling | moderate | 6 to 10 h each | no |
| Procedure 5 (`sage16`, uncut, chunked) | heavy: 3.25 TB read once per sweep (each chunk reads its own row range of every slab) plus `G` file opens per slab; 0.55 TB written | about 1.1 KB per halo of the widest chunk, and the super-forest's 333,663,215 indivisible rows make that at least about 370 GB whatever `G` is; `G` trims only the remainder | one core | 10 to 14 h | **yes, everything**: the measured version 2 run needed 513 GB unchunked and the chunked uncut run cannot go below the super-forest's floor |
| Procedure 8 (C) | as step 3 | as step 3 plus the label arrays (about 10 GB) | as step 3 | 12 to 20 h | yes, 100 GB |
| Procedure 10 (`sage16`, cut, chunked) | as step 5 | the chosen class's budget plus the output buffers, well under step 5's | one core | 10 to 14 h | yes, the chosen budget plus 50 GB |
| Procedure 11 (laptop proxy) | as step 5 | the class's budget (16, 32 or 64 GB) by construction | one core | 10 to 14 h | yes, the budget plus 50 GB, with `MallocLargeCache=0` |
| Procedure 6 and 12 (transfers to NT) | heavy on the LaCie and the network: 0.5 to 3.25 TB per transfer at the measured 110 MB/s download rate, upload unmeasured | none | none | 1.5 h per 0.55 TB; about 8 h for the uncut dataset if parked | no |
| 18 to 24 | light | a few GB | light | under two hours each; Slice 19 runs several test tiers and the distributed gate | no |

The whole-machine moments are procedure step 2, if the version 2 reference has to be re-run, and step 5, the uncut `sage16` run, whose floor is the super-forest; the cut runs (steps 10 and 11) are the first that fit a small budget, which is the point of the cut. Nothing in Stages A, B, D or the code slices of R and C needs more than about 100 GB free.

---

Run on a feature branch the owner names at `init`; no implementation session edits this plan, dependency manifests or licences, regenerates a physics baseline, deletes source data, or pushes. Every slice uses one `MODEL`/`SIMULATION` pair per command sequence and restores the default pair's generated code before its final default-tier run. Format check: run the full unmodified `./scripts/beautify.sh`, then `make check-format`.

---

## Slice 1: The ruling in the format document

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. No prerequisites beyond the planning baseline.
- Record the owner ruling (F1) in `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md` as an explicit second carve-out: the errata preamble, an errata row, the V3 Source Identity table row, the assignment paragraph, the inversion and sampling consequences, the sidecar ordinal definition, and the non-normative note. No other file changes; the code that implements it is Slices 2 to 4.

### Acceptance Criteria

- [ ] The Status line (`HORIZONTAL-HDF5-FORMAT.md:5`) gains one sentence after its last: "One further owner carve-out, a conformance change to one route's identity convention made on 2026-10-07, is recorded under Errata with the files it affects."
- [ ] The Errata preamble (`:191`) gains, after its last sentence, exactly: "A second carve-out, ruled by the owner on 2026-10-07: a change to one route's identity convention that leaves every value a consumer reads unchanged, made while no dataset of that route was shipped, committed or claimed in V3 Runtime Support (linked to that section), is recorded here without a bump. The row states the files it makes non-conforming and how to recognise them."
- [ ] A new errata row, dated 2026-10-07, section "V3 Source Identity, V3 Ordering Contracts, V3 Forest Sidecar" (each linked to its section as the neighbouring rows link theirs), with exactly this text: "**Owner ruling, not a correction; `format_version` stays 3.** For `consistent_trees_ascii`, `SourceHaloID` previously prefix-summed per-file units (the part of one forest that one file carries, numbered by the forest's first `#tree` marker in that file) in ascending (file ordinal, unit ordinal) with rows in file order, and inverted to a physical source row. It is now the halo's 1-based position in (`ForestIndex`, `HaloRankInForest`) order, as for the other two routes: the unit is the whole forest, units are in ascending `ForestIndex`, the row ordinal is `HaloRankInForest`, so every slab is grouped by forest. Unchanged: `ForestIndex`, `HaloRankInForest`, `UniqueGalaxyID`; every payload and link value and chain order; the header attribute set; the object set; every invariant; and the sidecar, whose `ForestID` is the source forest id and whose `SourceFileOrdinal`/`SourceUnitOrdinal` give, for a forest held in one file, that file's ordinal and the forest's unit ordinal in it (its rank among the file's forests ordered by their first `#tree` marker), or −1/−1 for a forest spanning files. **This changes which files conform.** A version 3 ASCII dataset written before this date (by `convert_trees.py` up to and including release v1.2) is recognisable from its data alone, because its `SourceHaloID` is not its (`ForestIndex`, `HaloRankInForest`) position; it fails the producer battery and must be reconverted, although Mimic's run-time physics reads none of the changed values and its output is identical. A conversion workdir of this route started before the ruling does not resume under it. The route does not sample: an ASCII subset is a separate catalogue with its own enumeration. No bump was taken because (1) the physical inversion's only consumers were the converter's own acceptance tools, which now check this route through `ForestIndex`, `HaloRankInForest` and the catalogue id; (2) no dataset of this route was shipped, committed or claimed under V3 Runtime Support; (3) a bump would keep duplicate reader, converter, test and documentation paths for one route's identity convention, against the vision's single source of truth; and (4) the only production source of this route is being retired, so the owner relinquishes the physical-row inversion for it. A declared cut forest table (a `forests.list` with the same tree roots and new forest ids) is a transformed source for this route: hosts are resolved under the original partition, the table is installed, and every halo whose resolved central lies in another forest of the table becomes a central; without a table the route refuses a cross-forest host. The ruling does not rest on the 2026-09-29 carve-out, which preserved conformance, and does not extend to any other route."
- [ ] The identity table row (`:447`) becomes: `| consistent_trees_ascii | dense forest-id enumeration (ascending source forest id) | post-fix-up reference ordering, also the canonical row ordinal that assigns SourceHaloID | original source forest id, or the declared cut table's id |` with the column cells backticked as the neighbouring rows are.
- [ ] The assignment paragraph (`:433`) becomes: "`SourceHaloID` is assigned by **prefix-summing halo counts over the adapter's declared total order of units**, starting at 1 so every value is positive. Assignment aborts on int64 overflow rather than wrapping. For `lhalo_binary` and `consistent_trees_hdf5` the units are an L-Halo tree or a forests-HDF5 `ForestInfo` row, in ascending source file ordinal and then ascending within-file unit ordinal, and the row ordinal is the source's within-unit row. For `consistent_trees_ascii` (from 2026-10-07, see Errata, linked to that section) the unit is the whole forest, units are in ascending `ForestIndex`, and the row ordinal is `HaloRankInForest`. For every route, therefore, `SourceHaloID` is the halo's 1-based position in (`ForestIndex`, `HaloRankInForest`) order, and every slab is grouped by forest."
- [ ] The second consequence (`:438`) becomes: "**It inverts exactly to its unit and row.** For `lhalo_binary` and `consistent_trees_hdf5` that is `(source_file_ordinal, unit_ordinal, row_ordinal)`, a physical source row, which is what independent comparison tooling uses. For `consistent_trees_ascii` it is (`ForestIndex`, `HaloRankInForest`), not a physical row; this route's physical provenance is the sidecar and the conversion manifest." The third consequence (`:439`) gains the final sentence: "The `consistent_trees_ascii` route does not sample; an ASCII subset is a separate catalogue with its own enumeration."
- [ ] The sidecar paragraph (`:471`) states that for `consistent_trees_ascii` the ordinals of a forest held entirely in one file are the file's ordinal and the forest's unit ordinal in it, the rank among that file's forests ordered by the file position of their first `#tree` marker (so with markers F, F, G the unit ordinal of G is 1), −1/−1 for a forest spanning files, and that a piece of a declared cut table whose trees come from several files is a spanning forest.
- [ ] The non-normative note (`:429`) says that datasets from all three routes have the property by construction from 2026-10-07 (the ASCII route by the ruling), that version 2 datasets do not, and keeps its last two sentences; the sentence saying the ASCII route does not have it is removed.
- [ ] No other normative sentence changes; `make check-docs` passes.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`
- Functions/classes/components allowed to change: prose only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code, no changelog, no guide or skill edit, no V3 Runtime Support row (Slice 6 adds it), no version 2 text change, no change to any other route's row.

### Risk Flags

- Risky surfaces touched: the normative format contract.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the errata row, read alone, says what changed, which files no longer conform, how to recognise them, and why no bump was taken; the preamble's two carve-outs are distinguishable; no sentence outside the listed anchors moved.

### Rollback Path

- Revert the slice commit; the document returns to the physical-row convention.

---

## Slice 2: The adapter's canonical unit and identity

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 1.
- Make the ASCII adapter declare the whole forest as its canonical unit in ascending `ForestIndex` and assign `SourceHaloID` under F1, keeping the physical (file, first marker) coordinates only for the sidecar; bind the identity scheme into the manifest so an old ASCII workdir is refused; adjust the inventory contract so that a route may declare a canonical unit distinct from its physical coordinates; move the acceptance harness's ASCII extras join to the catalogue id, because its extractor assigns physical ids and `test_ascii_extras` runs the real adapter against it, so the converter floor would otherwise break between this slice and Slice 4; and update the tests to the new literal identities.

### Acceptance Criteria

- [ ] `CTreesAsciiAdapter.inventory()` returns a `SourceInventory` whose units are the forests in ascending `ForestIndex` (one unit per forest, `n_halos` the forest's total over all snapshots, taken from the units sidecars and `forest_index_table.npy` before any halo row is read), so that `inventory.base_id` and `inventory.coordinate` invert a `SourceHaloID` to (`ForestIndex`, `HaloRankInForest`) under the existing prefix-sum arithmetic; the memory-budget check of `_build_inventory` is stated and enforced against the forest count.
- [ ] `_source_ids` computes `SourceHaloID = base[ForestIndex] + HaloRankInForest`, with `HaloRankInForest` and `ForestIndex` taken from the links record (`links.py:1138-1139`) rather than the scatter-time coordinates, range-checking both against the inventory; a rank at or above the forest's count, or a forest outside the table, is a `ConverterError` naming the snapshot, halo id, forest and rank.
- [ ] `iter_batches` still sorts each snapshot by `SourceHaloID`; the module docstring's statements that ids are per-file prefix sums and do not ascend across batches are rewritten to the new convention; `CanonicalBatch.coordinates` for this route carries (`0`, `ForestIndex`, `HaloRankInForest`) so the base contract's non-negativity holds (a deliberate shim, named as such in the docstring, to avoid widening the three-field contract the two prelinked routes rely on), and the base-class docstrings (`adapters/base.py:15-16`, `:200-215`, `:278`) say that inversion is to the route's declared canonical unit, physical for the two prelinked routes and (forest, rank) for ASCII.
- [ ] `iter_forests` is unchanged in what it emits (sidecar records with the forest's file and first-marker ordinals, −1/−1 when more than one populated unit); the physical units sidecars are retained for it.
- [ ] `_ascii_parameters` (`pipeline.py:379-415`) records `identity_scheme: forest-rank`, inside the configuration digest, so that `initialize` refuses an old ASCII workdir with the existing configuration-mismatch error; `conversion_manifest.inventory_record` for this route records one unit per forest (its `units_sha256` therefore differs from an old workdir's) and declares no physical units; `require_inventory` is unchanged.
- [ ] `column_schema.SOURCE_IDENTITY_CONVENTIONS["consistent_trees_ascii"]` gains `source_halo_id: "1-based position in (ForestIndex, HaloRankInForest) order"` and keeps its other entries.
- [ ] `lhalo_binary` and `consistent_trees_hdf5` are unchanged in every emitted value (their existing tests pass unmodified).
- [ ] In `convert/mimic-convert/tests/run_generalisation_acceptance.py`, `iter_ascii_source` (`:1840-1885`) no longer assigns a physical prefix id; for `consistent_trees_ascii` it yields the catalogue id (`MostBoundID`) and the row's `SnapNum`, and `compare_extras` (`:2010-2050`) sorts and joins both sides on (`SnapNum`, `MostBoundID`) **for this route only**, keeping its duplicate and coverage findings under that key; `lhalo_binary` and `consistent_trees_hdf5` keep the `SourceHaloID` join, because L-Halo sources carry duplicate `MostBoundID` values (`adapters/lhalo_binary.py:19-21`). `test_ascii_extras` (`test_generalisation_acceptance.py:1288`) passes under the new join; `test_ascii_slices_assign_the_same_source_halo_ids_at_any_block_size` (`:1687`), which asserts the extractor's physical ids, is rewritten to assert the catalogue key is identical at any block size; a new case joins extras for an ASCII fixture with a forest spanning two files.
- [ ] Tests: `test_ascii_adapter.py` `TestCanonicalBridge` literal expectations are the new ids (every forest's rows contiguous in rank order, ids 1..17 in (`ForestIndex`, rank) order for the existing fixture), `test_inventory_is_the_unit_prefix_order` becomes the per-forest inventory, `test_forest_records` is unchanged, and a new case proves an old workdir (a manifest recorded without `identity_scheme`) is refused on resume; `test_adapter_contract.py` gains the canonical-unit statement for ASCII; `test_pipeline.py` and `test_conversion_manifest.py` cases that assert the ASCII inventory record are updated. No test is weakened.
- [ ] Required evidence: `make tests-converter` via a subagent, all cases passing; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/adapters/ctrees_ascii.py`
  - `convert/mimic-convert/adapters/base.py`
  - `convert/mimic-convert/pipeline.py`
  - `convert/mimic-convert/conversion_manifest.py`
  - `convert/mimic-convert/column_schema.py`
  - `convert/mimic-convert/tests/test_ascii_adapter.py`
  - `convert/mimic-convert/tests/test_adapter_contract.py`
  - `convert/mimic-convert/tests/test_pipeline.py`
  - `convert/mimic-convert/tests/test_conversion_manifest.py`
  - `convert/mimic-convert/tests/fixtures.py`
  - `convert/mimic-convert/tests/run_generalisation_acceptance.py`
  - `convert/mimic-convert/tests/test_generalisation_acceptance.py`
- Functions/classes/components allowed to change: `CTreesAsciiAdapter` (`inventory`, `iter_forests` only in comments, `iter_batches` docstring, `_build_inventory`, `_source_ids`, `_batch`), the module docstring and constants; `SourceInventory` and `CanonicalBatch` docstrings and the base module docstring; `_ascii_parameters`; `inventory_record`; `SOURCE_IDENTITY_CONVENTIONS`; `iter_ascii_source`, `source_extra_blocks` and `compare_extras`; the named tests.
- Tests allowed or expected to change: the seven test files listed.

### Explicit Non-Goals

- No change to the battery, the harness's `compare` leg (Slice 4), the writer, the report, the CLI text, scatter, sort, fixups, links or rank_sort; no change to the other two adapters' behaviour; no documentation beyond docstrings.

### Risk Flags

- Risky surfaces touched: the converter's identity assignment and the manifest's configuration digest.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent, pass/fail summary); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the ids of the existing adapter fixture, listed by hand against (`ForestIndex`, rank), are the new literal expectations; a resumed old workdir fails before any file is touched.

### Rollback Path

- Revert the slice commit; the adapter returns to physical ids and the format document (Slice 1) is ahead of the code until the revert is itself reverted.

---

## Slice 3: The producer battery and the converter's stale statements

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 2.
- Split the battery's gating tuple so the ASCII route joins the (`ForestIndex`, rank) position check for complete datasets while the per-file sidecar inventory check stays with the two prelinked routes; refuse a sampled ASCII inventory; and correct the report and CLI statements that the ASCII route is not forest-blocked.

### Acceptance Criteria

- [ ] `validate_v3.py` replaces `_V3_UNIT_FORESTS` with two tuples: one naming all three formats, gating the identity-position check (`:1333`), and one naming the two prelinked formats, gating the per-file sidecar inventory check (`:1487`); the ASCII sidecar branch (`:1445-1465`) is kept.
- [ ] For `consistent_trees_ascii`, an inventory with `selected_halos != total_halos` is a FAIL naming the route and the two counts ("the ASCII route does not sample"), so the position check always applies to this route (the CLI always supplies a manifest, `convert_trees.py:611-616`). No separate per-slab `ForestIndex` non-decreasing check is added: for a complete dataset the position check implies forest blocking, and its message already names the first offending (forest, rank, id).
- [ ] `report.py:333-338` and `convert_trees.py:346-356` state that output from all three routes is forest-blocked and chunked sweeps apply; `README.md:94` and `:134` are corrected (the `compare-extras` sentence now says it joins on the catalogue id for ASCII and on `SourceHaloID` for the prelinked routes, and that `compare` checks `SourceHaloID` for every route).
- [ ] Tests: `test_validate.py` gains ASCII-format cases for the position check passing, the position check failing on a physically ordered dataset, and the sampled-inventory refusal; `test_report.py` and `test_cli.py` expectations of the limitation text are updated.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/validate_v3.py`
  - `convert/mimic-convert/report.py`
  - `convert/mimic-convert/convert_trees.py`
  - `convert/mimic-convert/README.md`
  - `convert/mimic-convert/tests/test_validate.py`
  - `convert/mimic-convert/tests/test_report.py`
  - `convert/mimic-convert/tests/test_cli.py`
- Functions/classes/components allowed to change: the gating tuples and their uses, `_v3_identity`, `_v3_source_keys`, `_v3_sidecar_content`; `_v3_limitations`; the width-line text; the two README sentences; the named tests.
- Tests allowed or expected to change: the three test files listed.

### Explicit Non-Goals

- No bounded-memory rewrite of the battery (Slice 16); no change to the manifest-binding checks; no harness change (Slice 4); no guide or skill edit (Slice 6).

### Risk Flags

- Risky surfaces touched: the producer battery's verdicts.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the committed version 3 fixtures of the five evidenced packages still pass `convert_trees.py validate` unchanged (run on `tests/data/horizontal_v3/dataset` and `simulations/mini-millennium-horizontal/_tests/data/worked_graph`).

### Rollback Path

- Revert the slice commit.

---

## Slice 4: The acceptance harness

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: medium. Requires Slice 2.
- The C-dump identity check of `run_generalisation_acceptance.py compare` gains the ASCII route (the extras join moved to Slice 2).

### Acceptance Criteria

- [ ] `UNIT_FOREST_FORMATS` (`:222`) names all three formats, so `reference_blocks` assigns the expected `SourceHaloID` from the (`ForestIndex`, rank)-sorted dump for ASCII too, the `source_halo_id` finding is declared applicable for ASCII, and `_compare_window` emits it.
- [ ] Docstrings (`:23-46`, `:95-98`, `:841`) describe the new convention.
- [ ] `test_generalisation_acceptance.py`: `test_ascii_identity_with_the_shipped_zero_extras_profile` (`:1402`) expects the `source_halo_id` finding applicable and passing.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/tests/run_generalisation_acceptance.py`
  - `convert/mimic-convert/tests/test_generalisation_acceptance.py`
- Functions/classes/components allowed to change: `UNIT_FOREST_FORMATS` and its uses, `reference_blocks`, `_compare_window`, the finding declarations, docstrings; the named tests.
- Tests allowed or expected to change: `test_generalisation_acceptance.py`.

### Explicit Non-Goals

- No change to `unit_ordinals.py`, the dump tool, the L-Halo or forests-HDF5 extractors, or the make targets.

### Risk Flags

- Risky surfaces touched: none beyond the manual harness.
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: none beyond the tests; Slice 5 runs the harness on real data.

### Rollback Path

- Revert the slice commit.

---

## Slice 5: micro-Uchuu on the new route, the package moved to version 3, the gate

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slices 1 to 4. Run on this host with the local data.
- Convert `micro-uchuu-ascii` through the route, prove it against the C dump and the independent extraction, install it as `micro-uchuu-ascii-horizontal`'s dataset, move the package to version 3 in place (declarations, README, gate), add its schema-conformance test, and gate it: the parity gate under `halos-only` and `sage16` with both schemes, a serial-versus-`mpirun -np 4` leg and a serial chunked leg for both models, and a SHAM version 2 against version 3 leg; start the acceptance record.

### Acceptance Criteria

- [ ] The conversion (`inspect`, `ingest`, `transpose`, `write`, `validate`, `report`) of `simulations/micro-uchuu-ascii/snapshots` with the package's profile runs in a LaCie workdir; `validate` passes, including the position check; `report` records forest blocking; `run_generalisation_acceptance.py compare` against the C dump and `compare-extras` pass with no finding; the dataset is installed at the directory `simulations/micro-uchuu-ascii-horizontal/snapshots` resolves to, with the version 2 dataset retained at a recorded path for Slice 15.
- [ ] `simulations/micro-uchuu-ascii-horizontal/halo_properties.yaml` declares the five link roles `long long` with its header comment naming version 3; `simulation_info.yaml` is unchanged in values; `README.md` is rewritten to the version 3 packages' shape (provenance: converter commit, profile digest, counts, `links_adjacent`, the battery result; the regeneration commands are the `convert_trees.py` sequence; the version 2 refusal paragraph at `:62` becomes the statement that this dataset is forest-blocked and distributable and chunkable; the committed fixtures paragraph says the committed fixtures are version 2 until Stage D) with no sentence claiming more than was measured.
- [ ] `_tests/integration/test_schema_conformance.py` exists on the `micro-uchuu-hdf5-horizontal` template and passes against the real dataset.
- [ ] `_tests/scientific/test_cross_format_identity.py` carries version 3 pins (`format_version 3`, `source_format consistent_trees_ascii`, the profile's `column_mapping_sha256`, 22,580,924 halos, 440,651 forests, `links_adjacent 1`, file range (0, 0)) and runs `halos-only` and `sage16` under fixed and dynamic schemes; `VersionTwoGate` (`:489`) is renamed to a version-neutral `AsciiGate` that keeps its multi-partition check and its Stage 8, the vertical-path preservation stage against the pinned `BASELINE_COMMIT` (the brief requires the version 3 gate to inherit it; it is the only check of the vertical ASCII reader against a pinned commit).
- [ ] The parity gate passes all four legs and Stage 8 with the comparator's PASSED line recorded for each.
- [ ] Distributed and chunked legs: `sage16` and `halos-only` serial references on the new dataset (into `archive/distributed-references/<model>-ascii/` with commit hash and `make info`), compared per `UniqueGalaxyID` with `mpirun -np 4` (`USE-MPI=yes`) and with serial `input.forest_chunks: 4` runs through `scripts/compare_cross_format_identity.py`; all identical; the driver's startup forest-blocking check passes (its log line recorded).
- [ ] SHAM leg: the shipped `models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml` resolves its input through the fixture's `test_simulation.yaml`, so two real-data run files are used: the committed `-realdata.yaml` variant pointing at the package's dataset (version 3 after installation) and a scratch run file under `output/` pointing at the retained version 2 dataset, its path recorded; the two outputs compared per `UniqueGalaxyID` are identical, which is the evidence that the ruling keeps a module keyed on the id unchanged. HOD's leg waits for Slice 14's lineage comparator.
- [ ] `docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md` is created with a Stage A section: converter commit, dataset counts and digests, battery verdict, harness verdicts, the four parity legs, the distributed, chunked and SHAM legs with wall-clock, and the retained version 2 dataset path.
- [ ] `simulations/micro-uchuu-ascii-horizontal/_tests/unit/test_unit_horizontal_reader_realdata.c` pins `format_version 3` and `source_format consistent_trees_ascii` against the package's real dataset (it is the shared reader's real-data smoke test and stays after retirement); the two fixture-based C tests beside it are untouched. `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-unit tests-integration` passes (the generic tiers still run on the version 2 fixture, F9).

### Authorized Surface

- Files allowed to change:
  - `simulations/micro-uchuu-ascii-horizontal/halo_properties.yaml`
  - `simulations/micro-uchuu-ascii-horizontal/README.md`
  - `simulations/micro-uchuu-ascii-horizontal/_tests/scientific/test_cross_format_identity.py`
  - `simulations/micro-uchuu-ascii-horizontal/_tests/integration/test_schema_conformance.py` (new file)
  - `simulations/micro-uchuu-ascii-horizontal/_tests/unit/test_unit_horizontal_reader_realdata.c`
  - `models/sham/input/sham_micro-uchuu-ascii-horizontal-realdata.yaml` (new file)
  - `docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md` (new file)
- Functions/classes/components allowed to change: the package declarations and prose; the gate script's `PACKAGE` and `main`; the new test and run file; the record.
- Tests allowed or expected to change: the gate script; the new schema-conformance test; the real-data C test's pins.

### Explicit Non-Goals

- No change to `parity_gate.py` (Slice 14), the comparator, the converter, the committed fixtures, the generic test configuration, or any guide or skill (Slice 6); no HOD leg; no deletion of the version 2 dataset.

### Risk Flags

- Risky surfaces touched: a shipped package's declarations and dataset; real-data runs on this host.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: the gate script; the schema-conformance test; the real-data C test's pins.
- Commands to run: the converter sequence; `run_generalisation_acceptance.py build-dump`, `dump`, `compare`, `compare-extras`; `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-scientific` and the same with `MODEL=sage16`; the distributed and chunked runs and comparisons; the SHAM runs and comparison; `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-unit tests-integration`; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: every number in the record traces to a log named in it; the README's provenance section states the converter commit the dataset was written at.

### Rollback Path

- Revert the slice commit and restore the package's `snapshots` symlink to the retained version 2 dataset.

---

## Slice 6: Guides, skills, changelog and pathway for Stage A

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. Requires Slice 5.
- Correct every statement that the ASCII route is not forest-blocked or cannot be chunked or distributed, record the ruling and the new package evidence where they permanently live, and register the stage in the pathway.

### Acceptance Criteria

- [ ] `docs/USER-GUIDE.md:543` and `:563`: forest-blocked version 3 datasets are those from any of the three routes; a version 2 dataset is refused.
- [ ] `docs/DEVELOPER-GUIDE.md:1227` (and the wording at `:1206`, `:1240`, `:1248` where it names the ASCII route): the three routes have the property by construction; version 2 does not.
- [ ] Skills: `mimic-run-and-operate/SKILL.md:129`, `mimic-simulations-and-readers/SKILL.md:61` and `:104`, `mimic-debugging-playbook/SKILL.md:77` (the fix becomes "reconvert with the current converter" and names the ruling date), `mimic-docs-and-writing/SKILL.md:73`, `mimic-validation-and-qa/SKILL.md:141`, `mimic-config-and-flags/SKILL.md:108` and `:113`, `mimic-config-and-flags/references/all-config-keys.md:51` and `:55`; each a sentence, no restating of the guides.
- [ ] `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md` V3 Runtime Support gains the `micro-uchuu-ascii-horizontal` row from the acceptance record (`consistent_trees_ascii`, complete real data, adjacent, `halos-only` and `sage16`, fixed and dynamic, the galaxy counts and field counts the record states) and the "no other route" bullets are reworded so that `sage16` is now gated on two packages; one errata row dated for the documentation addition.
- [ ] `CHANGELOG.md` Unreleased: one entry for the ruling and the route (what changed, which files no longer conform, the package move, the new gate legs), in the style of the existing entries; `:12` and `:20` lose "as the converter emits it today" and "the Consistent-Trees ASCII route ... cannot be chunked" in favour of "version 2 datasets".
- [ ] `tests/README.md:64-72`: the package's gate is version 3 and its committed fixtures remain version 2 until retirement.
- [ ] `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: the current-focus box and the named follow-up record Stage A complete with the record's numbers; the Plan Inventory lists this plan (frozen, in execution), the brief (promoted), and the acceptance record (standing evidence); no other section changes.
- [ ] `make check-docs` passes (it checks links and `PONDER` markers only), and a manual `git grep -n 'docs/dev/' -- ':!docs/dev' ':!CHANGELOG.md'` finds no citation of a `docs/dev/` document outside `docs/dev/`.

### Authorized Surface

- Files allowed to change:
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-debugging-playbook/SKILL.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`
  - `CHANGELOG.md`
  - `tests/README.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
- Functions/classes/components allowed to change: prose only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code; no change to the ruling's text (Slice 1); no claim about Shin-Uchuu beyond the brief's measured census; no archiving.

### Risk Flags

- Risky surfaces touched: the format document's runtime-support table.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: every number traces to the acceptance record; a grep for "not forest-blocked" and "cannot be chunked" over the repository (excluding `archive/`, `.orchestrator/` and the brief) finds only version 2 statements.

### Rollback Path

- Revert the slice commit; documentation only.

---

## Slice 7: Slab reader, index files, occupancy, effective trees, partition simulation

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. No prerequisite beyond Stage A (reads version 2 and version 3 datasets alike).
- The census's first half: a bounded column reader for horizontal datasets, a reader for the ASCII index files, per-forest occupancy per slab and over all slabs, terminal-root labelling into effective descendant trees with their occupancy, and an exact simulation of the driver's two-level partition.

### Acceptance Criteria

- [ ] `convert/mimic-convert/horizontal_dataset.py`: opens a version 2 or 3 dataset directory, exposes the header attributes, the snapshot list, and bounded block reads of one named `/halos` column of one slab (row budget an argument, default 2²² rows), and a **dataset identity** record (format version, `source_format` when present, `n_forests_total`, per-snapshot `n_halos`, the sidecar's `ForestID` digest) that the census subcommands bind their aggregate directory to, so that `trees`, `graph` and `cut` refuse a directory produced from a different dataset.
- [ ] `convert/mimic-convert/source_index.py`: parses `forests.list` (tree root to forest id) and `locations.dat` (tree root to file id and offset) into sorted int64 arrays; derives per forest the set of files, the count of trees, and for a forest held in one file its file ordinal and unit ordinal, the dense rank among that file's forests ordered by their first `#tree` marker (`ctrees_parser.py:593-603`; tested with repeated and non-contiguous markers such as F, F, G, F), else −1/−1; rejects a tree root present in one file and not the other.
- [ ] `forest_census.py occupancy`: per slab, the (`ForestIndex`, count) pairs of the forests present, written as sorted int64 and int32 arrays into an aggregate directory (12 B per present forest per slab; about 25 GB at Shin-Uchuu scale, stated with its formula); over all slabs, per-forest totals (int64, length `n_forests_total`), the widest slab (lowest-numbered on ties, as `src/core/horizontal_driver.c:915-929`), each forest's maximum occupancy and the slab it occurs in; a summary JSON reproducing the brief's measured table rows (total halos, widest slab, the largest forest's total, peak occupancy and slab, the second-largest forest's maximum, the forests exceeding 100,000, 250,000 and 1,000,000 in any slab, and the top-10 and top-100 shares of the widest slab).
- [ ] `forest_census.py trees`: a backward pass from the last slab labelling every halo with its terminal root (`MostBoundID` of the halo whose `Descendant` is −1), two slabs resident; the roots are enumerated first (a sorted int64 array, one per terminal halo) and labels are written per slab as **int32 root ordinals** (4 B per halo, about 90 GB at Shin-Uchuu scale, stated in the docstring with its formula) and live for the aggregate directory's lifetime, because `graph` and `cut` both read them (the directory is deleted by hand when the census is done); the root set is compared with the index files' tree roots (any root not in `forests.list`, and any root whose slab is not the last, reported with counts) and the correspondence verdict is recorded, since the cut table and the rewriter's inventory depend on it; per tree: its forest, its total, its per-slab occupancy as sparse (tree, count) arrays per slab, and the largest tree's total and peak occupancy; the conservation check (per-file row sums through `locations.dat` against a report's per-file parsed counts when a `conversion_report.json` is given).
- [ ] `census/partition.py`: `partition_cut(weights, ntask, nchunk)` reproducing `horizontal_partition_cut_forests` and `horizontal_partition_cut` (`src/core/horizontal_partition.c:31-47`, `:106-182`) exactly, including the capacity binary search, the greedy pass, zero-weight handling and idle trailing ranges; `forest_census.py partition` applies it with the widest slab's per-forest counts as weights for a grid of (ntask, nchunk) and reports, for every slab, every range's row count and the widest range, plus the heaviest forest as the floor.
- [ ] Tests (`convert/mimic-convert/tests/test_forest_census.py`): the partition against the C unit test's literal cases (`tests/unit/test_horizontal_partition.c:340` weights `{1, 5, 1}`, `:358` weights `{3, 0, 7, 1}`) and the `forest_blocks` fixture's recorded chunkings for weights `[2, 1, 7, 2, 2, 3]` (`tests/manual/test_distributed_identity.py:111-113`), no further brute-force oracle (the C test has its own); occupancy, labelling and the index reader on small synthetic version 3 datasets written with the converter's own fixtures, including new **correspondence-valid** fixtures whose `#tree` root id equals the terminal halo's id (the existing adapter fixtures use root 101 for terminal halo 1010, `tests/fixtures.py:251-255`, and are kept for the adapter), plus a correspondence-mismatch case that is reported, not repaired; the identity record on a version 2 fixture (`simulations/micro-uchuu-ascii-horizontal/_tests/data/generic`).
- [ ] Memory and disk: no array of length `n_halos` of any slab beyond the column block being read and the two label arrays; no dense tree × snapshot or forest × snapshot matrix; every aggregate's size formula in its docstring and in the summary JSON, so Slice 9 can record the measured total and the procedure can budget it.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/horizontal_dataset.py` (new file)
  - `convert/mimic-convert/source_index.py` (new file)
  - `convert/mimic-convert/forest_census.py` (new file)
  - `convert/mimic-convert/census/`
  - `convert/mimic-convert/tests/test_forest_census.py` (new file)
  - `convert/mimic-convert/tests/fixtures.py`
- Functions/classes/components allowed to change: new code only; in `fixtures.py` only added correspondence-valid forests.
- Tests allowed or expected to change: the new test file; `fixtures.py` additions.

### Explicit Non-Goals

- No graph, rules or cut table (Slice 8); no production run (Slice 9); no change to the converter, the driver or any existing test; no README entry yet (Slice 9 documents the tool in the converter manual).

### Risk Flags

- Risky surfaces touched: none (new, unwired modules).
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: `test_forest_census.py`.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `forest_census.py occupancy` on the committed `generic` fixture prints the counts the fixture manifest states.

### Rollback Path

- Revert the slice commit; new files only.

---

## Slice 8: The co-membership graph, candidate rules and the cut table

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 7.
- Revision 5: accepted in run 2 at `5b7edec1`. Its co-membership graph, threshold rules and candidate-rule machinery are retired by the revised Slice 9 under the owner's decided table (F6). The cut-table format, its invariants and record, and the per-slab accounting survive.
- The census's second half: the effective FoF co-membership graph between trees, components under candidate rules, a deterministic cut table under F6, and for each candidate the severed relations, promotions, progenitor-order changes, re-labelled halos, the predicted identity-dependent effects per model and the partition simulation's chunk maxima over every slab.

### Acceptance Criteria

- [ ] `forest_census.py graph`: per slab, for every FoF group whose members carry more than one tree label, the undirected pair (min, max) of the central's tree and each member's other tree, deduplicated within the slab so a pair counts one snapshot however many members join it, carrying the slab, the member count and the members' `Mvir` sum; per-slab edge lists sorted by pair and merged across slabs by an external merge into per-pair records (distinct-snapshot count, halo count, mass sum, first and last slab), the per-slab lists retained beside them (they give the per-snapshot promotions); restricted to the trees of the forests named (default: the forest with the largest total); the components of the complete graph (every pair kept) are reported as a census result, never assumed to be one; bounded as in Slice 7, with the per-slab list and merged-record size formulas stated.
- [ ] `census/rules.py`: a candidate rule keeps an edge iff its distinct-snapshot count ≥ d, its halo count ≥ h and its mass sum ≥ m (any of the three may be unbounded); components by union-find over kept edges; the rule is named by its thresholds in every output.
- [ ] `census/cut_table.py`: from a forest's components, a cut forest table in `forests.list` shape under F6's naming and invariants (piece size is the total halo count over all snapshots, ties by the smallest tree root id), written only when the root correspondence verdict of `trees` passed; the table's invariants are checked on write and on read (a malformed table, a root missing or duplicated, a piece mixing forests, or a fresh id at or below the catalogue's maximum is refused); for every exploratory rule only the compact per-tree component assignment and its summary are kept, and a complete table is materialised for the rehearsal tables and the rule the owner chooses; the table is accompanied by a JSON record carrying the input dataset identity, the index files' md5, the rule, the pieces (id, tree count, halo total, peak occupancy and slab) and the table's own md5.
- [ ] `forest_census.py cut`: for every candidate rule, the table and record above plus, from one further pass over the named forest's rows of every slab with the labels resident (the fourth read of those columns, bounded like the others): the severed co-memberships (edges dropped, halos promoted to central per slab, and the groups whose central leaves while members stay); the descendants whose progenitor chain order changes under F4 after severance (computed per slab from the kept and dropped relations with `Mvir`); the halos re-labelled (every halo of a piece that does not keep the id, plus every halo of the forest, since ranks are recomputed); the predicted model effects stated per decision 7 (`sage16`/`halos-only`: the promoted halos and their dependants; HOD/SHAM: every halo of the forest); and the partition simulation over every slab for the candidate (ntask, nchunk) grid with the piece sizes installed, giving the widest range per slab and the implied memory at a stated bytes-per-resident-halo figure (an argument, default 1,100 B from the chunked streaming record), so that each laptop class's feasibility is a row.
- [ ] Tests: synthetic graphs with known components under each threshold kind, including a pair whose host switches direction between snapshots (counted once per snapshot); the naming and tie rule on a hand-built case; every table invariant's refusal; severance counts and a progenitor-order change on a five-halo fixture; the table round-trips through `source_index.py`.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/forest_census.py`
  - `convert/mimic-convert/census/`
  - `convert/mimic-convert/tests/test_forest_census.py`
- Functions/classes/components allowed to change: the new subcommands and modules; Slice 7's modules only where the new subcommands need an exported helper.
- Tests allowed or expected to change: `test_forest_census.py`.

### Explicit Non-Goals

- No minimum-cut algorithm (threshold rules only; a min-cut over 10⁸ nodes is not bounded and is recorded as a possible later rule); no raw `pid`/`upid` graph; no rewriter; no production run.

### Risk Flags

- Risky surfaces touched: none (new modules); the output is what the owner decides on, hence the audit.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: a rule with every threshold at its minimum keeps every edge; its components are reported as the complete effective graph's, and a forest with one component yields the identity table.

### Rollback Path

- Revert the slice commit.

---

## Slice 9: The z = 0 FoF-group table, the production census and the record (revision 5)

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slices 7 and 8 and the three performance commits run 2 left above Slice 8's commit (`ade3b7f0`, `003051df`, `72f7695f`). Run on this host.
- Build the owner's decided cut table (F6) for the whole simulation and measure on the micro-Uchuu rehearsal and on production:
  - its cost (promotions, progenitor-order changes, affected histories);
  - its chunked memory.
- Remove the census machinery the decision makes dead (the [Final-State Inventory](#final-state-inventory)).
- Write the census record the owner signs off.
- Revision 4's Slice 9 contract (candidate rules over the co-membership graph) is superseded. Run 2's work on it is archived at `archive/stage-b-run2-slice9-wip/`, and its valid measurements are reused.

### Acceptance Criteria

- [ ] **Table builder.** `forest_census.py table` builds the decided table from the `trees` aggregates and the final slab's `FirstHaloInFOFgroup`.
  - Each tree's piece is its terminal halo's z = 0 FoF group, identified by the z = 0 central's tree.
  - Pieces are named under F6 with the existing `cut_table` naming.
  - A catalogue containing a tree that ends before the final snapshot is refused before any output, naming the count (F6).
  - For every split forest, the piece count equals its z = 0 centrals (asserted). The final slab's central references are validated: in range, self-central, and in the member's original forest.
  - A forest selection (the rehearsal's restricted table) splits only the named forests. Every other forest keeps its identity assignment, and the record states the selection; a restricted table is never labelled whole-simulation.
  - The complete table is written in `forests.list` shape with its streamed JSON record: the dataset identity, the index files' md5, the rule stated as "the z = 0 FoF groups of every forest", the pieces and the table's md5.
  - It is written only when the `trees` root correspondence passed and the supplied index files pass the census's index check. The invariants are checked on write and on read.
- [ ] **Accounting.** The census evaluates exactly the decided table over every forest the table gives more than one piece. A forest selection remains available for the rehearsal's second table. It measures:
  - the promotions per slab, and that **no promotion occurs at the final snapshot**;
  - the groups losing members, and the (group, piece) remnants (the members of one group that lie in a piece other than their central's piece), with those of two or more members separately;
  - the progenitor-order changes under F4, with the stored-chain check. Its mismatches must be 0 for the prediction to stand;
  - the re-labelled halos, and the affected-history bracket (`dependent_halos` and `upper_bound_halos`, F7);
  - the partition with the pieces installed, for (ntask, nchunk) in {1, 2, 4, 8} × {1, 2, 4, 8, 16, 32, 64, 128}. Each point records the per-process figure and the job figure (the sum over tasks of each task's widest chunk over all slabs);
  - one row per class (16, 32 and 64 GiB) at a stated usable reserve and bytes per resident halo, both arguments (the bytes per halo defaulting to 1,100).

  Every figure covers every slab and every piece, not a selection. Memory and I/O are stated per phase, including the retained-row discovery that replaces the graph, and measured into the summary, as in Slices 7 and 8.

  The memory table also states two quantities: the driver's transient startup weights (8 B × `n_forests_total`) and the table record's size.

  The graph's edge-list cross-check of promotions retires with the graph. Its replacements are the per-forest piece-count assertion, the zero-promotion assertion at the final snapshot, and the test's independent all-ended computation.

  The HOD and SHAM effects are stated as predicted mechanisms: identity recomputation, host and centrality changes, and SHAM's global ranking. They are distinct from Slice 15's measured subset differences.
- [ ] **Retirement** (the Final-State Inventory's Slice 9 rows).
  - Removed, together with their tests and CLI subcommands:
    - the threshold rules and union-find (`census/rules.py`);
    - the co-membership graph (`census/graph.py`'s per-slab edge lists, external merge and components);
    - the candidate-rule machinery of `cut` (rule lists, `--rule`, rule names, the multi-rule pass, rule-keyed output directories).
  - Every helper the surviving census still imports moves to the module that uses it.
  - `cut` requires only `occupancy`, `trees` and `partition`. Its tree index comes from the `trees` aggregates, not `graph/forest_trees.npy`.
  - No import, docstring, test, CLI help or manual text refers to the retired code.
- [ ] **Tests.**
  - The decided table on synthetic version 3 and version 2 datasets:
    - a forest ending in one z = 0 group is unchanged;
    - a forest ending in several splits into exactly its groups;
    - F6 naming and the tie rule, including a group whose central's tree is not its smallest-root tree;
    - a catalogue with a tree ending before the final snapshot is refused. This uses Slice 7's early-ending forest, moved out of the valid correspondence set into a separately named negative fixture;
    - a promotion before the final snapshot and none at it;
    - a progenitor-order change;
    - the table round-trips through `source_index.py`.
  - On fixtures, the decided table equals the partition that cuts every co-membership ended before the final snapshot, computed independently inside the test.
  - The helpers run 2's performance commits added that survive (`in_sorted_window`, `slab_prefix`, the once-sorted fresh-piece order, or their successors) are covered.
- [ ] **Rehearsal.**
  - The census on the version 3 micro-Uchuu dataset and on the retained version 2 one agree in every logical result, compared through the catalogue id.
  - Two materialised tables for Slice 15's G3c, each with its accounting:
    - the decided table for the whole micro-Uchuu simulation;
    - the decided table restricted to micro-Uchuu's largest forest.
  - Conservation passes against the Stage A conversion's per-file parsed counts in `/Volumes/LaCie/data/uchuu/micro-uchuu/stage-a-v3-workdir/ascii_preparation/manifest.json`. A version 3 `conversion_report.json` carries none.
- [ ] **Production.**
  - The decided table for the whole Shin-Uchuu simulation is materialised with its record, accounting and partition rows.
  - Run 2's `occupancy`, `trees` and `partition` aggregates in `/Volumes/LaCie/data/uchuu/shin-uchuu-census/` are reused after the following checks pass:
    - their summaries are complete;
    - the dataset identity matches;
    - the index files' md5 match the production report's and run 2's record;
    - the dataset files' modification times predate run 2's census, which shows the source is unchanged since it was aggregated.
  - Run 2's retired `graph/` and candidate `cut/` outputs are moved aside, not deleted, to `/Volumes/LaCie/data/uchuu/shin-uchuu-census-run2-retired/` before the new outputs are written. The same volume, so they stay charged to the storage ledger.
  - Recorded: every aggregate's size, the peak LaCie footprint, the wall-clock and peak RSS of every subcommand. The production procedure's free-space precondition is rechecked against them.
- [ ] **The brief's measured table** is reproduced to the number, as run 2 measured it, or each discrepancy is stated:
  - 22,503,649,037 halos;
  - snapshot 34 widest at 519,342,987;
  - the super-forest: 12,646,607,901 total, 333,663,215 peak at snapshot 31, 104,845,278 trees;
  - the second-largest forest's 220,983 (`ForestIndex` 16386454);
  - the 11 / 1 / 1 counts;
  - the 68,294,028 / 62.10% / 62.86% figures.
- [ ] **`docs/dev/MIMIC-SHIN-UCHUU-FOREST-CENSUS.md` is created.** It may start from run 2's archived draft. It contains:
  - the definitions: effective trees, z = 0 FoF groups, the decided rule and its equivalence to cutting every co-membership ended before the final snapshot;
  - the root correspondence and the conservation check;
  - the evidence for the decision: run 2's rule preview and PM's informal estimates, labelled as such;
  - **a memory table, near the top:** the uncut unchunked and chunked floors, the decided table's largest piece, the per-class job rows, and the single-tree floor (2,504,273 rows);
  - the decided table's measured cost;
  - the largest individual tree;
  - the predicted differences per model per decision 7;
  - **two clearly marked, empty decision slots** for the owner: the laptop class with its usable budget, and the stop condition of the brief's decision gate.

  Nothing in it is a recommendation.
- [ ] `convert/mimic-convert/README.md` gains a short section naming `forest_census.py`, its subcommands as they now are, its inputs, and its memory and disk bounds. The acceptance record gains a Stage B section pointing at the census record, naming run 2's stop and revision 5.

### Authorized Surface

- Files allowed to change:
  - `docs/dev/MIMIC-SHIN-UCHUU-FOREST-CENSUS.md` (new file)
  - `docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md`
  - `convert/mimic-convert/README.md`
  - `convert/mimic-convert/forest_census.py`
  - `convert/mimic-convert/census/`
  - `convert/mimic-convert/tests/test_forest_census.py`
  - `convert/mimic-convert/tests/fixtures.py`
- Functions/classes/components allowed to change:
  - the census package and its CLI, including the removal of the retired code and the move of its surviving helpers;
  - in `tests/fixtures.py` only moving Slice 7's early-ending forest out of the valid correspondence set into a named negative fixture;
  - the two records;
  - the manual.
- Tests allowed or expected to change: `test_forest_census.py`. Retired cases go with the code they test; new cases are added. No surviving case is weakened.

### Explicit Non-Goals

- No rewriter (Slices 11 and 12).
- No change to `horizontal_dataset.py`, `source_index.py` or the converter's routes.
- No change to the decided rule or its scope.
- No claim about physics beyond the measured counts.
- No deletion of data: retired aggregates are moved aside.

### Risk Flags

- Risky surfaces touched:
  - a multi-hour read of the production dataset on this host (read-only);
  - the removal of accepted code;
  - the table the production cut is made from.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run:
  - the census subcommands on both micro-Uchuu datasets and on production;
  - `make tests-converter` (subagent);
  - `make check-docs`;
  - `./scripts/beautify.sh`;
  - `make check-format`.
- Lint (differential, via the `lint` skill): required.
- Manual checks:
  - the record's every number traces to an aggregate file or log it names;
  - the decision slots are empty;
  - a grep for the retired modules, subcommands and options outside history finds nothing.

### Rollback Path

- Revert the slice commits. Restore the moved-aside aggregates by hand.

---

## Slice 10: The Shin-Uchuu packages prepared for version 3

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: medium. No prerequisite beyond Stage A.
- Prepare `simulations/shin-uchuu` to load a version 3 dataset and `simulations/shin-uchuu-ascii` to be converted by the version 3 route.

### Acceptance Criteria

- [ ] `simulations/shin-uchuu/halo_properties.yaml`: the five link roles are `long long`, the header comment names version 3, every other declaration unchanged (the version 2 production dataset still loads, since the version 2 path accepts either link width, `src/io/horizontal/read_horizontal_hdf5.c:1913-1935`).
- [ ] `simulations/shin-uchuu-ascii/converter_columns.yaml` exists: the shipped default ASCII profile's selection with this package's comments (the route line and the statement that `halo_properties.yaml` declares exactly the payload it fills).
- [ ] `simulations/shin-uchuu/_tests/integration/test_schema_conformance.py` exists on the `micro-uchuu-hdf5-horizontal` template; its real-dataset half skips with a stated reason while the resolved dataset is version 2.
- [ ] `simulations/shin-uchuu/README.md`: one paragraph under "Maintenance notes" states that the declarations are version 3 while the dataset is still version 2, until the production rewrite; nothing else changes.
- [ ] `make MODEL=halos-only SIMULATION=shin-uchuu generate check-generated` and `make MODEL=halos-only SIMULATION=shin-uchuu tests-integration` pass (the committed version 2 fixture); the default pair's generated code is restored.

### Authorized Surface

- Files allowed to change:
  - `simulations/shin-uchuu/halo_properties.yaml`
  - `simulations/shin-uchuu/README.md`
  - `simulations/shin-uchuu/_tests/integration/test_schema_conformance.py` (new file)
  - `simulations/shin-uchuu-ascii/converter_columns.yaml` (new file)
- Functions/classes/components allowed to change: declarations and prose; the new test and profile.
- Tests allowed or expected to change: the new test.

### Explicit Non-Goals

- No dataset change, no rewrite, no provenance rewrite (Slice 17), no generated file edited by hand.

### Risk Flags

- Risky surfaces touched: a shipped package's declarations (generated code regenerates from them).
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: the new test.
- Commands to run: `make MODEL=halos-only SIMULATION=shin-uchuu generate check-generated tests-integration`; `make generate` for the default pair; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `convert_trees.py inspect --source-format consistent_trees_ascii` with the new profile on the subset resolves without error.

### Rollback Path

- Revert the slice commit and regenerate.

---

## Slice 11: The rewriter: passes, migration (M) and the no-op (N)

### Intended Change

- Recommended Developer: Claude Fable 5.1; effort: high. Requires Slices 7 and 10.
- The rewriter under F5 with its (M) and (N) operations, proven at fixture scale against the Stage A route.

### Acceptance Criteria

- [ ] `convert/mimic-convert/rewrite_trees.py` with subcommands `migrate` (version 2 in, version 3 out), `noop` (version 3 in, version 3 out under the identity table) and the common options: `--input`, `--output`, `--scratch` (all explicit; none defaults into the repository), `--simulation-info`, `--a-list`, `--forests-list`, `--locations`, `--column-map` (the vertical ASCII package's profile; required for `migrate`, refused for `noop`, which carries `/schema` through), `--multiplier`, `--resume`, and (revision 5, `migrate` only, for multi-file catalogues) `--conversion-manifest`.
- [ ] Refusals before any write: an input that is not version 2 or 3; `links_adjacent` 0; a version 3 input whose `source_format` is not `consistent_trees_ascii`; missing or inconsistent index files; an a_list that differs from the input's scale factors; a multiplier that cannot encode the output's bounds.
- [ ] Pass 0 as F5 states, including the per-forest totals from its own `ForestIndex` scan and, from `source_index.py`, each forest's sidecar ordinals and the per-tree inventory (root, file, marker ordinal, forest, and the tree's row count once pass 1 labels it), written in the output workdir beside the manifest (never inside `dataset/`) as `tree_inventory.npy` and checked against a given conversion report's per-file counts. The converter's tree-file order, which the sidecar ordinals need (revision 5), comes from the given version 2 conversion manifest's ordered `provenance.source_files` (`--conversion-manifest`). That option is required when `locations.dat` names more than one tree file. For a one-file catalogue the order is that file, and a manifest naming another file is refused. A missing or ambiguous order is refused, and its digest is bound into the resume state; the terminal-root correspondence with the index files is a prerequisite verdict: a mismatch is refused, not repaired.
- [ ] Pass 1 as F5 states, with the rank key of F4 computed per slab from `ForestIndex`, `FirstHaloInFOFgroup` and `MostBoundID`; the remap store holds per slab the old-to-new permutation and the three identity columns, retains only N−1, N and N+1, and records its peak size.
- [ ] Pass 2 as F5 states; each slab is written to a temporary name and renamed on success; per-slab verification re-reads the written file and checks every payload column equals the input's under the permutation, every link's target row carries the expected `MostBoundID` (through the target slab's permutation), the companion `*Snapshot` columns, `SnapNum`, and that `ForestIndex` is non-decreasing and ranks are dense per forest; the header attributes, `/schema` and the sidecar are written through `hdf5_writer_v3`'s helpers (chunk shape, dataset order, header, sidecar) and a new column-source entry point beside `write_v3_snapshot_file` (which takes record blocks, `hdf5_writer_v3.py:215-222`, while pass 2 produces permuted columns); every object the rewriter creates is written with `track_times=False`, so a repeated run of the same operation is byte-identical.
- [ ] The workdir layout and manifest are F5's; `convert_trees.py validate --workdir <output>` and `report --workdir <output>` run through their existing entry points (`cmd_validate`, `cmd_report`) with the binding and conservation checks applied, not skipped, which a test asserts from the battery's outcome names; checksums per artefact; restart state per slab, bound to the pinned input artefacts (path, size, mtime and inode as `conversion_manifest.pin_dependency` records them, `conversion_manifest.py:929-947`) and to the operation, multiplier, profile digest, a_list digest and cut-table md5, so `--resume` refuses a changed input or configuration (tested with a same-size content change), continues after the last verified slab, and on a completed run re-verifies and exits.
- [ ] (N) reproduces its input in every dataset value, dtype, header attribute, `/schema` attribute and sidecar value; a byte comparison of files is additionally reported, and is expected equal when the input was itself written by the rewriter (same layout, no timestamps) and not otherwise.
- [ ] **Revision 5 (run 2's routed findings R8 and R9).**
  - `source_index.forest_table().file_ordinal` is the `locations.dat` `FileID`, not the sidecar's `SourceFileOrdinal`. The converter numbers tree files in the order it is given them, and on Shin-Uchuu a lexically sorted file list differs from `FileID` order. So the rewriter's sidecar ordinals map `FileID` through the conversion's recorded tree-file order and refuse an ambiguous mapping, tested with sparse and reordered `FileID`s.
  - `source_index.py`'s docstring states `forest_table()`'s returned size, 40 B × forests + 8 B × (file, forest) pairs, and its measured peak, including the forests ≈ trees case.
- [ ] Tests (`convert/mimic-convert/tests/test_rewriter.py`): on Slice 7's correspondence-valid fixture forests (`tests/fixtures.py`), the version 2 route's output through (M) equals the Stage A route's output of the same source in every `/halos` column, the header, `/schema` and the sidecar (the fixture-scale G3); (N) is the identity; resume after an injected failure equals an uninterrupted run; each refusal; the per-tree inventory against the fixture's known trees.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/rewrite_trees.py` (new file)
  - `convert/mimic-convert/rewriter/`
  - `convert/mimic-convert/hdf5_writer_v3.py`
  - `convert/mimic-convert/source_index.py`
  - `convert/mimic-convert/horizontal_dataset.py`
  - `convert/mimic-convert/tests/test_rewriter.py` (new file)
  - `convert/mimic-convert/tests/fixtures.py`
- Functions/classes/components allowed to change: new code; in `hdf5_writer_v3.py` only an added streaming entry point beside `write_v3_snapshot_file` with no change to existing functions; in the two shared modules only exported helpers the rewriter needs, plus `source_index.py`'s revision 5 docstring corrections.
- Tests allowed or expected to change: the new test file; `fixtures.py` additions.

### Explicit Non-Goals

- No (C) operation (Slice 12); no gapped links; no change to the battery, the manifest module's contract, the converter's stages or the C code; no production run.

### Risk Flags

- Risky surfaces touched: a new producer of the normative format.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: `test_rewriter.py`.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `migrate` of `simulations/micro-uchuu-ascii-horizontal/_tests/data/generic` (version 2, 3 forests) validates under `convert_trees.py validate` with every check applied.

### Rollback Path

- Revert the slice commit; new files only, plus the writer's added entry point.

---

## Slice 12: The cut operation (C)

### Intended Change

- Recommended Developer: Claude Fable 5.1; effort: high. Requires Slice 11.
- The rewriter's `cut` subcommand under F6, with the adversarial fixtures.

### Acceptance Criteria

- [ ] `rewrite_trees.py cut --cut-table <forests.list-shaped file>` on a version 3 input: pass 1 labels every halo with its terminal root (the descendant's label from slab N+1, else its own `MostBoundID`), installs the table (a root absent from the table, or a table root absent from the index files, is refused), resolves each halo's central under the original partition and promotes every halo whose central's forest differs from its own (F6), and recomputes for every forest the table changes: FoF chains in the reference encounter order, `HaloRankInForest` under F4, and progenitor chain order under F4's insertion loop with `Mvir`; the numeric `Descendant` index is remapped through the next slab's permutation like every other link, `DescendantSnapshot` is copied, and the per-slab verification checks that every halo's descendant carries the `MostBoundID` it carried in the input (the logical relation is unchanged).
- [ ] Untouched forests reproduce their input exactly in payload, in topology (every link's target carries the same `MostBoundID`, chain order unchanged) and in `UniqueGalaxyID` (asserted by the per-slab verification); `SourceHaloID` is recomputed for every row; `n_forests_total`, `max_halo_rank_in_forest` and the sidecar describe the new census, with a piece whose trees span files carrying −1/−1 and a single-file piece its file and first-marker ordinal.
- [ ] The manifest and conversion report record the table's md5, the rule text from its JSON record when given, the promotions and progenitor-order changes per snapshot (the census's definitions), and the pieces. The table is bound to its input by its md5 and the index files' md5, not by the record's dataset identity: (M) changes the identity record while preserving the forest enumeration, and the record's version 2 lineage is carried into the manifest; `convert_trees.py validate` passes on the output.
- [ ] A table that changes nothing produces exactly (N)'s output; every F6 table invariant is checked on read and each violation refused with a message naming the root or id.
- [ ] **Revision 5 (run 2's routed finding R10).**
  - Fresh pieces take `ForestIndex` `n_forests_total + k`, in ascending fresh id after every original forest. That is the dense enumeration of ascending forest id, which the census's partition simulation assumes.
  - The table reader enforces that fresh ids are contiguous from the catalogue maximum + 1. That is a stricter reading of F6's "assigned in descending piece size", added to `census/cut_table.py`'s invariants, which the rewriter reuses.
- [ ] Adversarial fixtures (`tests/fixtures.py` additions, each read vertically by the reference path in Slice 13's leg): a severed satellite; a group whose central leaves while its members stay together; a piece that initially holds no central; a tie in progenitor order (equal `Mvir`) whose resolution follows the insertion loop; a forest cut into three pieces where the largest keeps the id. Revision 5 adds, removing none: a forest split into its z = 0 FoF groups by the decided table (F6), with a promotion before the final snapshot and none at it, so the output's final-snapshot FoF groups equal the input's.
- [ ] Tests (`test_rewriter.py`): each fixture's expected chains, ranks, ids and sidecar rows stated literally; the identity table ≡ (N); every table refusal; a descendant whose row moves between the input and the output still resolves to the same halo.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/rewrite_trees.py`
  - `convert/mimic-convert/rewriter/`
  - `convert/mimic-convert/tests/test_rewriter.py`
  - `convert/mimic-convert/tests/fixtures.py`
  - `convert/mimic-convert/census/cut_table.py`
  - `convert/mimic-convert/tests/test_forest_census.py`
- Functions/classes/components allowed to change: the new subcommand and modules; Slice 11's modules where the cut needs a hook; in `census/cut_table.py` only the contiguity invariant (revision 5) and its test.
- Tests allowed or expected to change: `test_rewriter.py`, `fixtures.py`, and in `test_forest_census.py` only the contiguity invariant's case.

### Explicit Non-Goals

- No change to `links.py` or `fixups.py` (Slice 13 owns the route's cut-table support); no C change; no production run.

### Risk Flags

- Risky surfaces touched: the operation that produces the production cut dataset.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the severed-satellite fixture's output, dumped with h5py, shows the satellite as its own FoF group's first halo with the same payload and an unchanged descendant.

### Rollback Path

- Revert the slice commit.

---

## Slice 13: The route's cut table and the independent reference leg's fixtures

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slices 2 and 12.
- Teach the ASCII route's fix-up stage the same severance rule behind a declared cut table, so that (C) can be gated against a real conversion; and build the tool that transforms small ASCII fixtures for the reference reader (F8).

### Acceptance Criteria

- [ ] `convert_trees.py ingest --cut-table <file>` for `consistent_trees_ascii` only: `scatter` keeps the original `forests.list` as the partition under which `fixups.fix_upid_snapshot` resolves hosts; after resolution every halo is re-labelled to the cut table's forest through its `tree_root_id` (carried in the scratch record, `ctrees_parser.py:79`, `:913`), the forest table (`forest_index_table.npy`) and the per-file units sidecars (`units_src_N.npy`: unit membership, counts and first-marker ordinals, which `iter_forests` checks against the table, `adapters/ctrees_ascii.py:519-527`) are recomputed under the transformed source, the original partition's metadata being retained only until resolution finishes; every halo whose resolved central's forest differs from its own becomes a central (upid = id, pid = −1 in the post-fix-up record); and `links` proceeds unchanged on the result, so ranks, chains and identities follow F4 under the new partition; the manifest records the table's md5 under the configuration digest; without `--cut-table` behaviour is byte-identical to today (existing tests unchanged).
- [ ] `convert/mimic-convert/tests/tools/transform_ascii_fixture.py`: given small ASCII tree files, `forests.list` and a cut table, writes transformed tree files in which every halo's `upid` and `pid` are first canonicalised to the resolved centrals under the original partition and then severed under F6, a promoted halo written in the raw Consistent-Trees convention (`pid` −1 and `upid` −1, which is what the reference reader expects of a central, `fixups.py:386-387` being the post-fix-up form), with the cut table as the new `forests.list` and a regenerated `locations.dat`, so that the unmodified C reference reader (`src/io/vertical/ctrees/ctrees_utils.c:354-449`) reconstructs exactly the cut topology; it refuses an input whose hosts it cannot resolve.
- [ ] Tests: `test_fixups.py` or `test_ascii_adapter.py` gain the cut-table cases on Slice 12's adversarial fixtures, asserting that the route under the table and (C) on the route's uncut output agree in every `/halos` column, header and sidecar (the fixture-scale G3c); `test_transform_ascii_fixture.py` asserts the transformed files parse under `ctrees_parser.py` with the expected hosts.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/convert_trees.py`
  - `convert/mimic-convert/pipeline.py`
  - `convert/mimic-convert/adapters/ctrees_ascii.py`
  - `convert/mimic-convert/scatter.py`
  - `convert/mimic-convert/fixups.py`
  - `convert/mimic-convert/conversion_manifest.py`
  - `convert/mimic-convert/tests/tools/transform_ascii_fixture.py` (new file)
  - `convert/mimic-convert/tests/test_transform_ascii_fixture.py` (new file)
  - `convert/mimic-convert/tests/test_fixups.py`
  - `convert/mimic-convert/tests/test_ascii_adapter.py`
  - `convert/mimic-convert/tests/test_cli.py`
- Functions/classes/components allowed to change: the `ingest` option and its plumbing; `prepare_workdir`; the forest-table and units-sidecar recomputation in `scatter`; a severance step after `fix_upid_snapshot`; `_ascii_parameters`; the new tool and tests.
- Tests allowed or expected to change: the listed test files.

### Explicit Non-Goals

- No change to the C reader; no change to `links.py`; no other route accepts a table; no production run.

### Risk Flags

- Risky surfaces touched: the converter's fix-up stage behind an option that is off by default.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the micro-Uchuu conversion of Slice 5 re-run without `--cut-table` yields byte-identical `/halos` columns (spot-checked on three snapshots).

### Rollback Path

- Revert the slice commit.

---

## Slice 14: The comparator's lineage key and the gate's manifest-proved inventory

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 5.
- Extend the identity comparator so created records are compared by birth lineage and tree rows may be matched by the catalogue id; make the parity gate's sidecar stage prove the inventory from the conversion manifest; let a gate name a dataset directory other than the package's; and add the subset's version 3 gate script.

### Acceptance Criteria

- [ ] `scripts/compare_cross_format_identity.py --match-created-by-lineage`: created rows (negative ids) are keyed by (`MostBoundID`, `SnapNum`, ordinal), the ordinal decoded as `(-id - 1) mod 1024` (`MAX_CREATED_RECORDS_PER_HOST` in `src/include/galaxy_id.h:61-72`, copied as a named constant with a comment naming the header); duplicate keys in either run, unmatched keys and field differences are failures with the same reporting as tree rows; `--match-by-catalogue-id` keys tree rows (positive ids only) by (`MostBoundID`, `SnapNum`) for datasets whose enumeration differs, with duplicate keys a failure; in either alternate mode `UniqueGalaxyID` is the key and not a compared field, `UniqueCentralGalaxyID` is compared through the matched mapping (the left record's central maps to the right record's central, else a failure naming both), and every other field is compared byte for byte; the default behaviour is unchanged; the PASSED line reports the mode and the created rows compared.
- [ ] `tests/scientific/test_compare_cross_format_identity.py` covers both flags on synthesised HDF5 with created rows: equal science with different labels passes; a lineage mismatch, a duplicate catalogue key and a broken central mapping each fail.
- [ ] `tests/framework/parity_gate.py`: `GatePackage` gains an optional `manifest` path; when present, the sidecar stage proves the inventory from the manifest's source dependencies (their names must equal the vertical side's file list) and checks that every single-file ordinal lies in that set and that −1/−1 rows are a subset of the forests the index files place in more than one file (`source_index.py`); when absent, the existing inference runs; every existing gate passes unchanged.
- [ ] `GatePackage` gains an optional `horizontal_dataset` (directory, snapshot list) override; when set, the horizontal run file may additionally differ from the vertical one in `input.simulation_dir` and `input.snapshot_list_file` (`AUTHORIZED_KEY_CHANGES` extended under that condition only) and `assert_dataset_present` resolves the override.
- [ ] `simulations/shin-uchuu/_tests/scientific/test_cross_format_identity.py` exists: vertical `shin-uchuu-ascii` against a horizontal dataset named by the override (the migrated subset), with version 3 pins to be filled by Slice 15 (counts 406,668,896 halos and 6,011,205 forests, `consistent_trees_ascii`, `links_adjacent 1`, the profile digest), `halos-only` and `sage16`, both schemes, free-space requirement 150 GiB; it skips with a stated reason when the override is not given.
- [ ] **Revision 5: the cut-difference report.** `scripts/compare_cross_format_identity.py` gains `--multiplier`, both datasets' sidecars and the cut table's record.
  - Each tree record's `ForestIndex` is decoded from its `UniqueGalaxyID`. It is then mapped through the sidecar's `ForestID`, and through the record for a cut piece, to its original forest. Created records follow their host, and an unmappable record is an input error.
  - Records of the split original forests are matched by the catalogue key and **reported, not failed**: identical, differing (per field) and unmatched counts, per forest and in total.
  - Records of every other forest keep the strict default semantics, where any difference or unmatched record fails. A separate option reports every forest without failing, for Slice 15's SHAM leg only; input errors still fail.
  - The default behaviour without the option is unchanged.
  - `tests/scientific/test_compare_cross_format_identity.py` covers it: a difference inside a split forest is reported and passes; the same difference outside one fails; an unmatched record in a fresh piece is classified to its original forest.
- [ ] `tests/integration/test_parity_gate_helpers.py` covers the extended key rule.
- [ ] Required evidence: `make tests-scientific` and `make tests-integration` via a subagent (default pair); `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `scripts/compare_cross_format_identity.py`
  - `tests/scientific/test_compare_cross_format_identity.py`
  - `tests/framework/parity_gate.py`
  - `tests/integration/test_parity_gate_helpers.py`
  - `simulations/shin-uchuu/_tests/scientific/test_cross_format_identity.py` (new file)
- Functions/classes/components allowed to change: `compared_rows`, the matching and reporting functions, argument parsing; `GatePackage`, `stage_dataset_provenance`, `assert_horizontal_run_file_diff`, `assert_dataset_present`; the new gate script.
- Tests allowed or expected to change: the two test files and the new gate script.

### Explicit Non-Goals

- No change to any existing gate script's pins; no change to `test_distributed_identity.py`; no run of the subset gate (Slice 15).

### Risk Flags

- Risky surfaces touched: the comparator every identity claim rests on.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-scientific`, `make tests-integration` (subagent); `make MODEL=halos-only SIMULATION=mini-millennium-horizontal tests-scientific` (the fixture gate still passes); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the HOD run pair on micro-Uchuu (the retained version 2 dataset against Slice 5's version 3 dataset, through real-data run files on the pattern of Slice 5's SHAM leg, since the shipped `models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml` resolves its input through the fixture's `test_simulation.yaml`) compares identical under `--match-created-by-lineage`, recorded for Slice 15.

### Rollback Path

- Revert the slice commit.

---

## Slice 15: The subset conversions and the Stage R gates

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slices 10 to 14. Run on this host; LaCie space for about 0.5 TB of transient workdir plus two subset datasets (about 60 GB each) and the version 2 subset (about 40 GB).
- Produce the Stage A conversion of the local subset (the reference) and a regenerated version 2 subset, and run every Stage R gate: micro-Uchuu G3 and the HOD leg, subset G3, G3b, G3c with the independent reference leg, G4 with SHAM and HOD, the cut subset serial against chunked against `mpirun`, determinism and resume; measure what Stage C needs; record it.

### Acceptance Criteria

- [ ] micro-Uchuu G3: (M) of the retained version 2 micro-Uchuu dataset equals Slice 5's dataset in every `/halos` column, the header, `/schema` and the sidecar (a column-by-column comparison script in the record's log); `validate` passes with binding checks applied; HOD version 2 against version 3 identical under `--match-created-by-lineage`.
- [ ] Subset reference: `convert_trees.py` over `simulations/shin-uchuu-ascii/snapshots` (2,744 files in inventory order, the Slice 10 profile, `--multiplier 20000000000`, a stated `--memory-budget-mb`) in a LaCie workdir; `validate` and `report` pass; the transient workdir's peak size, the battery's spill and peak RSS and the wall-clock are recorded (these size Stage C).
- [ ] Version 2 subset: `convert_ctrees.py` over the same files, `validate.py` passing.
- [ ] G3: (M) of the version 2 subset ≡ the reference in every `/halos` column, header, `/schema` and sidecar, which proves the sidecar ordinals derived from `locations.dat` on a dataset with forests spanning files; the per-tree inventory's conservation check passes against the reference's report.
- [ ] G3b: (N) of the reference is the identity.
- [ ] G3c, first on micro-Uchuu where a second conversion is cheap: Slice 9's two rehearsal tables; (C) of Slice 5's dataset under each ≡ the route's conversion of `micro-uchuu-ascii` under `--cut-table` with the same table, in every `/halos` column, header and sidecar; then on the subset, where pieces span files and the sidecar's −1/−1 is exercised under a cut: the census (revision 5's subcommands) run on the subset reference to produce the decided table for the whole subset (F6), with every subset forest that ends in more than one z = 0 group split, which must be a non-trivial table, (C) of the reference under it, and a second subset conversion through the route under `--cut-table` with the same table (about 0.5 TB transient, hours; named in the commands and the resource profile), compared as above; the independent reference leg: Slice 12's adversarial fixtures transformed by Slice 13's tool, run vertically under `halos-only` through the unmodified reader against the horizontal driver on (C)'s output, per-`UniqueGalaxyID` identical.
- [ ] G4: the subset gate (Slice 14) against the migrated subset passes all four legs; SHAM and HOD on the version 2 subset against the migrated subset, serial, identical (HOD by lineage); SHAM and HOD serial against `mpirun -np 4` on the migrated subset, identical.
- [ ] The cut subset (one table): `sage16` and `halos-only` serial against `input.forest_chunks: 8` and against `mpirun -np 4`, identical; the driver's partition log lines recorded.
- [ ] Revision 5: SHAM and HOD serial on the cut subset against the migrated subset, through the cut-difference report (Slice 14), by catalogue key and lineage, reported not failed. The record gives the per-forest and total differing counts, and SHAM's differences outside the split forests separately. This is decision 7's HOD and SHAM measurement, at subset scale.
- [ ] Determinism: a second (M) run yields byte-equal files where the writer's layout is deterministic, else value-equal; resume: an (M) interrupted after a recorded slab and resumed equals the uninterrupted output. Revision 5 adds the same pair for (C) on micro-Uchuu under Slice 9's whole-simulation rehearsal table:
  - a second run is byte-equal, or value-equal as above;
  - a run interrupted and resumed equals the uninterrupted one;
  - a resume with a changed table is refused.
- [ ] The acceptance record gains the Stage R section with every verdict, size, RSS and wall-clock, and the rewriter's measured bytes per halo of remap store and temporary.
- [ ] `convert/mimic-convert/README.md` gains the rewriter's section (operations, inputs, refusals, manifest, memory and storage as measured).

### Authorized Surface

- Files allowed to change:
  - `docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md`
  - `convert/mimic-convert/README.md`
  - `simulations/shin-uchuu/_tests/scientific/test_cross_format_identity.py`
  - `simulations/shin-uchuu-ascii/README.md`
  - `convert/mimic-convert/rewrite_trees.py`
  - `convert/mimic-convert/rewriter/`
  - `convert/mimic-convert/tests/test_rewriter.py`
- Functions/classes/components allowed to change: the record, the manuals, the gate's pins; the rewriter only for defects the subset exposes, each with a test.
- Tests allowed or expected to change: the gate's pins; `test_rewriter.py` for a defect fix.

### Explicit Non-Goals

- No production or source data touched or deleted; no change to the census outputs or the owner's table; no claim about the super-forest.

### Risk Flags

- Risky surfaces touched: multi-hour real-data runs on this host; LaCie space.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: the conversions (the subset reference, the version 2 subset, and the subset under `--cut-table`), the census on the subset, the rewrites, comparisons, gates and runs named above, every `validate` and `report` with `--multiplier 20000000000`, an explicit `--memory-budget-mb` and `--spill-dir` on the LaCie; `make tests-converter` if the rewriter changed; `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: every number in the record traces to a named log; the record names the three subset datasets retained after this slice, each with the workdir and manifest that describe it, since `validate` and `report` load the manifest (`convert_trees.py:603`): the version 2 subset and the migrated subset for Stage D, and the cut subset for Slice 16's agreement run, about 0.16 TB together; and it confirms that the reference conversion, the cut-route conversion, the rewriter's scratch and every other workdir were deleted once the record was written, so the procedure's precondition can budget the rest.

### Rollback Path

- Revert the slice commit; the three retained subset datasets and their workdirs stay on the LaCie until Stage D says otherwise.

---

## Slice 16: The bounded producer battery

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 3; uses Slice 15's subset datasets and measurements.
- Give `validate_v3.py` a bounded mode in which every obligation of the battery has a replacement with a stated storage ceiling, and prove its verdicts agree with the external-sort battery's before it is trusted on production.

### Acceptance Criteria

- [ ] `convert_trees.py validate --bounded` (and `report --bounded`), for adjacent (`links_adjacent` 1) datasets only, refusing a gapped dataset by name, runs the battery with bounded implementations whose ceiling is a stated function of the widest slab, `n_forests_total` and `n_halos`, printed at start and enforced against `--memory-budget-mb`: resident per-slab int64 columns are allowed (read in bounded blocks, never one read above `INT32_MAX` rows), whole-dataset sorts and spills are not. The slice freezes an obligation-to-algorithm table in `validate_bounded.py`'s docstring and the manual, one row per obligation: manifest binding and source conservation (unchanged); every per-row invariant (per block, unchanged); progenitor closure, chain membership and FoF integrity (per snapshot pair N and N−1 with their link, `*Snapshot`, `ForestIndex` and `MostBoundID` columns resident, cycle detection by a visited bitset per chain pass, state carried across block boundaries explicitly); `SourceHaloID` uniqueness and the [1, total] density as two independently indexed one-bit-per-halo bitsets (`total_halos / 8` bytes each, about 2.8 GB at Shin-Uchuu scale); the (`ForestIndex`, rank) position check and rank density from the per-forest totals (int64 per forest) and the per-slab block order; identity bounds; `/schema` agreement. At Shin-Uchuu's numbers the printed ceiling is about eight int64 columns of the widest slab plus the bitsets and the per-forest array, roughly 40 GB.
- [ ] Every finding the external-sort battery can report has a counterpart in the bounded mode with the same name, so the two verdict sets are comparable item by item.
- [ ] Agreement: a test runs both modes on every adjacent (`links_adjacent` 1) converter fixture dataset and adversarial variant `test_validate.py` already builds, on Slice 12's cut fixtures, and on new adjacent adversaries for the bounded mode (a chain crossing a block boundary, a long progenitor chain, a cycle), and asserts identical verdict sets; the gapped fixtures stay with the general battery, where they pass today (`test_validate.py:2703-2717`), and the bounded mode's refusal of one is a separate case; on this host the two modes agree on the migrated subset and the cut subset Slice 15 retained, with the bounded mode's peak RSS and spill (none expected beyond the bitsets) recorded in the acceptance record.
- [ ] `validate_v3.py`'s external-sort implementation is untouched and remains the default; the README's "Restart, cleanup and memory" states the two modes, when to use the bounded one, and the command template for a Shin-Uchuu dataset (`--bounded --multiplier 20000000000 --memory-budget-mb <N> --spill-dir <LaCie path>`); whether one implementation retires is decided in Slice 22's contract at the Stage D revision, not after the plan (revision 5; see the Final-State Inventory).
- [ ] Required evidence: `make tests-converter` via a subagent; the subset agreement run; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/validate_v3.py`
  - `convert/mimic-convert/validate_bounded.py` (new file)
  - `convert/mimic-convert/convert_trees.py`
  - `convert/mimic-convert/report.py`
  - `convert/mimic-convert/README.md`
  - `convert/mimic-convert/tests/test_validate.py`
  - `convert/mimic-convert/tests/test_cli.py`
  - `docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md`
- Functions/classes/components allowed to change: a dispatch in `run_battery_v3` and in `cmd_validate`/`cmd_report` (`convert_trees.py:611-616`, `:640-643`); the new module; the two options; the manual; the tests; the record's Stage C preamble.
- Tests allowed or expected to change: `test_validate.py`, `test_cli.py`.

### Explicit Non-Goals

- No change to any verdict's meaning; no removal of the external-sort battery (Slice 22's contract decides whether one implementation goes; revision 5); no change to the rewriter.

### Risk Flags

- Risky surfaces touched: the battery the production datasets are accepted on.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); both modes on the two subset datasets with `/usr/bin/time -l`; `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the printed ceiling for Shin-Uchuu's numbers matches the stated formula and is below 64 GB.

### Rollback Path

- Revert the slice commit.

---

## The production procedure (owner-operated, between Slices 16 and 17)

Not a PM slice: the owner runs it with an assistant session in Mode A, step by step, on the Mac Studio, under workflow (a) of the brief (one LaCie; `Internal` holds one run output at a time; NT is the archive of finished files by the owner's transfers). Every step's evidence goes into the acceptance record's Stage C section as it happens; a step that fails stops the procedure. Decimal TB; the output budget per dataset is 3.25 TB.

1. **Preconditions.** Stage R accepted; the LaCie holds the version 2 dataset (2.25), the subset ASCII (0.21), the index files (0.02), the three subset datasets Slice 15 retains with their workdirs (about 0.16) and the census aggregates (the measured figure from Slice 9; run 2 measured about 0.29 TB in all, retired outputs included). Revision 5: before production, the free-space figure and every step's peak and headroom below are recomputed from a measured ledger of everything retained, same-volume moves included, and the free space the recomputed ledger requires is available (revision 4's 4.85 TB, superseded; it derived from the brief's 7.71 usable less those, which leaves the cut step's 0.66 TB headroom intact only if the other Stage R datasets and workdirs were deleted as Slice 15 requires; at the baseline 4.2 TB was free with the owner's 0.72 TB of unrelated files and the 0.51 TB version 2 run output still on it, so about 1.2 TB moves off first); the version 2 run output is at `/Volumes/Internal/results/mimic/sage16-shin-uchuu` and its `metadata/` is readable (the empty `sage16-shin-uchuu-ascii` beside it is not needed: no vertical run of the full box exists or is planned, and the vertical comparison is the subset gate of Slice 15).
2. **The reference run's configuration** (decision 9): compare the version 2 run's recorded build, modules, parameters, timestep scheme, output fields and snapshot coverage with `models/sage16/input/sage16_shin-uchuu.yaml` at the current commit; if any differs, re-run `sage16` on the version 2 dataset now (about 10 h; 513 GB peak RSS was measured) into `Internal`, and record which was done.
3. **The uncut dataset by (M):** `rewrite_trees.py migrate` from the version 2 dataset with the index files, the Slice 10 profile, `--multiplier 20000000000`, scratch and output on the LaCie; expected peak 6.05 TB resident, the remap store (about 0.05) released at the end; the per-tree inventory's conservation check against the production report passes.
4. **Battery:** `convert_trees.py validate --workdir <uncut> --bounded --multiplier 20000000000 --memory-budget-mb <N> --spill-dir <LaCie path>` and `report` with the same options on the uncut dataset; pass required.
5. **`sage16` on the uncut dataset:** single task, `input.forest_chunks` chosen from Slice 9's uncut partition figures (the super-forest's 333,663,215 rows are indivisible, so the widest chunk and the memory floor, about 370 GB at 1.1 KB per halo, are set by it whatever the chunk count; chunking only trims the rest), output to the LaCie (`Internal` still holds the reference); the galaxy-by-galaxy comparison against the reference run with `scripts/compare_cross_format_identity.py` (tree rows by `UniqueGalaxyID`); identical required, which isolates the format change from the cut; the science checks (the baryon fraction, the z = 0 Type 0 count, the halo mass function) recorded alongside the version 2 values.
6. **Settle the version 2 artefacts:** the version 2 run output from `Internal` to NT (owner transfer, checksummed), then the uncut output from the LaCie to `Internal`; the version 2 dataset is deleted from the LaCie only after its NT copy is verified against the production report's checksums (owner's step; the brief's second copy already exists at `/fred/oz214/dcroton/shin-uchuu/snapshot-trees-v2`).
7. **`shin-uchuu` moves to version 3:** the package's `snapshots` symlink points at the uncut dataset; Slice 10's schema-conformance test now runs its real-dataset half and passes.
8. **The cut dataset by (C):** `rewrite_trees.py cut` from the uncut dataset under the decided table (F6; Slice 9's production table and record, pinned by md5, built from the version 2 census and applied to the validated migrated dataset, with both provenance links recorded); peak and headroom as recomputed in step 1 (revision 4 estimated 7.05 TB resident with 0.66 TB headroom); the manifest records the table's md5.
9. **Battery on the cut dataset:** as step 4; the report's forest census (`n_forests_total`, the largest forest) matches the census record's prediction:
   - `n_forests_total` 244,953,607;
   - the largest forest, as predicted;
   - every z = 0 FoF group of the input preserved, with every final-snapshot central/member relation by `MostBoundID` identical between the uncut and cut datasets;
   - no promotion at the final snapshot;
   - the cut's conversion report gives promotions and progenitor-order changes per snapshot equal to the census record's. A block read of the final slab's central relations by `MostBoundID` in both datasets confirms the preserved z = 0 groups. Any mismatch stops the procedure.
10. **`sage16` on the cut dataset:** output to the LaCie (`Internal` holds the uncut output); the galaxy-by-galaxy comparison against step 5's output (decision 7: tree rows by (`MostBoundID`, `SnapNum`) since the split forests' ids changed): every galaxy of a forest the cut leaves unchanged is identical.
    - Inside the split forests, the cut-difference report (Slice 14) lists identical, differing and unmatched galaxies per forest and per field.
    - Those differences are attributed to the census record's promotions and affected-history bracket. The record's counts are input-topology counts, not a predicted number of differing galaxies.
    - The science checks are compared with step 5 and the version 2 values, and every difference is attributed to the cut.
    - The z = 0 `sage16` galaxy population and Types are reported as measured.
11. **Laptop acceptance (Mac-measured proxy):** on the cut dataset, `sage16` with `MallocLargeCache=0` at the task and chunk counts the chosen class implies (from the census record's partition rows, judged on the job figure; a 16 GiB class needs at least 64 chunks), recording peak RSS (for several tasks, each rank's peak and their sum), the build configuration and the class's usable budget (decision 6); the predicate is peak RSS under the budget with the sweep completing; labelled a proxy until a laptop of the class runs it, which is then the final acceptance.
12. **Settle:** the uncut output from `Internal` to NT, then the cut output from the LaCie to `Internal` (or the reverse, as the owner prefers); decision 10 on parking the uncut dataset; the package's `snapshots` symlink points at the dataset the owner designates as production (the cut one, under the end state). Then the Final-State Inventory's data rows are settled:
    - the accepted cut tables and their records are kept, checksummed, with the conversion provenance;
    - the census aggregates and run 2's moved-aside outputs are deleted by the owner;
    - the acceptance record notes the disposition of every directory.

## Slice 17: Provenance, record and pathway after the production procedure

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. Requires the procedure's record.
- Write the production datasets' provenance where it permanently lives and close Stage C.

### Acceptance Criteria

- [ ] `simulations/shin-uchuu/README.md`: title and contract sentences name version 3; "How the production dataset was made" is rewritten: the version 2 conversion as history in one paragraph with its report's totals and the `forests.list` md5, then the migration (rewriter commit, inputs, index-file md5s, the uncut dataset's counts, battery verdict, the `sage16` identity against the version 2 run), then the cut (the table's md5, the rule stated as "every forest partitioned into its z = 0 FoF groups" with its equivalence and the structural invariant of decision 7, the pieces, the promotions, the battery verdict, the `sage16` differences and the science checks), then the laptop proxy measurement with its label; the version 2 refusal sentence and the "neither distributed nor chunked" sentence are replaced by the measured chunked figures; the multiplier paragraph is kept; the maintenance note of Slice 10 is removed.
- [ ] `simulations/shin-uchuu-ascii/README.md`: the cross-format sibling paragraph names the version 3 dataset and the subset gate.
- [ ] The format document's V3 Runtime Support table gains the `shin-uchuu` row from the record (`consistent_trees_ascii`, complete real data by migration, `sage16` gated by the production comparison and the subset gate's four legs) and the full-Uchuu bullet is unchanged; one errata row dated for the documentation addition.
- [ ] `docs/USER-GUIDE.md` and `docs/DEVELOPER-GUIDE.md`: the Shin-Uchuu sentences (the super-forest floor, "neither distributed nor chunked") state what now exists, with the measured chunked figures; `.agents/skills/mimic-docs-and-writing/SKILL.md:73-75` moves Shin-Uchuu to supported-with-limits naming the cut and the proxy label; `mimic-simulations-and-readers/SKILL.md` and `mimic-run-and-operate/SKILL.md` name the rewriter and the census in a sentence each.
- [ ] `CHANGELOG.md` Unreleased: the rewriter, the census, the bounded battery, the production datasets and their evidence, in the existing style.
- [ ] The acceptance record's Stage C section is complete; the pathway's current-focus box, named follow-up and Completed Work record Stage C with the record's numbers; `make check-docs` passes.

### Authorized Surface

- Files allowed to change:
  - `simulations/shin-uchuu/README.md`
  - `simulations/shin-uchuu-ascii/README.md`
  - `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
- Functions/classes/components allowed to change: prose only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code; no claim beyond the record; no HOD or SHAM identity claim on the cut dataset; no archiving.

### Risk Flags

- Risky surfaces touched: the format document's runtime-support table; external claims.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: every number traces to the record; the proxy label is present wherever the laptop class is named.

### Rollback Path

- Revert the slice commit; documentation only.

---

## Slice 18: Utility extraction out of the version 2 modules

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: medium. Requires Stage C accepted.
- Move every name that surviving version 3 code imports from `hdf5_writer.py` and `validate.py` into the version 3 modules, so the version 2 modules can be deleted without touching behaviour.

### Acceptance Criteria

- [ ] `CHUNK_1D`, `HEADER_ATTRS`, `_log`, `load_header_metadata` and `snapshot_h5_name` are defined in `hdf5_writer_v3.py`; `hdf5_writer_v3.py:60-66`, `validate_v3.py:59` and `convert_trees.py:223` import them from there; `hdf5_writer.py` imports them back from `hdf5_writer_v3.py` so its own behaviour is unchanged until Slice 22.
- [ ] `DEFAULT_MULTIPLIER`, `DEFAULT_V3_BUDGET_BYTES`, `RUN_SCOPED_ATTRS`, `V3_FORMAT_VERSION`, `Outcome`, `_examples`, `_filter_failures`, `battery_failed` and `check_header_bounds` are defined in `validate_v3.py`; `validate_v3.py:68-78`, `report.py:34-39` and `convert_trees.py:760` import them from there; `validate.py` imports them back; the `validate` CLI (`validate.py:1714`) and the version dispatch (`:1659-1712`) are untouched.
- [ ] No import cycle (`python -c "import validate_v3, hdf5_writer_v3, report, convert_trees"` from the converter directory succeeds); every converter test passes unchanged.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/hdf5_writer_v3.py`
  - `convert/mimic-convert/hdf5_writer.py`
  - `convert/mimic-convert/validate_v3.py`
  - `convert/mimic-convert/validate.py`
  - `convert/mimic-convert/report.py`
  - `convert/mimic-convert/convert_trees.py`
- Functions/classes/components allowed to change: definitions moved verbatim and the import lines.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No deletion, no behaviour change, no renaming of the moved names.

### Risk Flags

- Risky surfaces touched: none beyond import topology.
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `git diff --stat` shows moves and import lines only.

### Rollback Path

- Revert the slice commit.

---

## Slice 19: The version 3 ASCII fixture and every consumer retargeted

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 18.
- Replace the committed version 2 fixtures' role with a version 3 fixture converted by the route from a committed synthetic ASCII source, and retarget every test, battery and run file that names the version 2 fixture, so that nothing but the version 2 code itself still depends on version 2.

### Acceptance Criteria

- [ ] `simulations/micro-uchuu-ascii-horizontal/_tests/data/source/` holds a generator (`generate_source.py`) writing the synthetic Consistent-Trees ASCII tree, `forests.list` and `locations.dat` that `create_snapshot_fixture.py` synthesises today (the same three forests, six snapshots and halos, so every value-level expectation survives), and the written files, committed; `_tests/data/regenerate.sh` converts it with `convert_trees.py` (ingest, transpose, write, report) under the package's `simulation_info.yaml` and the `micro-uchuu-ascii` profile into `_tests/data/worked_ascii/`, committed, on the `mini-millennium-horizontal` pattern; `simulations/shin-uchuu/_tests/data/regenerate.sh` does the same into `simulations/shin-uchuu/_tests/data/worked_ascii/` with that package's scale factors.
- [ ] Both packages' `_tests/input/test_simulation.yaml` point at `worked_ascii/`; `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-unit tests-integration` and the same for `shin-uchuu` pass on it. The two fixture-based C tests of `micro-uchuu-ascii-horizontal` (`test_unit_horizontal_reader_open.c`, `test_unit_horizontal_driver_gather.c`) read the top-level `_tests/data/` version 2 files directly, which stay until Slice 23, so they are untouched here and deleted in Slice 21.
- [ ] `models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml`, `models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml`, `models/sham/modules/sham_rank_match/_tests/test_integration_sham_rank_match.py`, `models/hod/modules/hod_populate/_tests/test_integration_hod_populate.py`, `tests/manual/run_snapshot_global_battery.py` (the `halos-only-v2`, `sham` and `hod` groups become the version 3 ASCII fixture; the group name loses "v2"), `tests/integration/test_snapshot_phase.py` (`HORIZONTAL_INPUT`) run on `worked_ascii/`; any assertion that pinned a row-position-derived value (a created-record id) is updated to the version 3 fixture's value with the reason recorded in the test; `make tests-snapshot-global` and the two model integration tests pass.
- [ ] `tests/manual/test_snapshot_disabled_identity.py`: the version 2 leg is removed, with its reason in the module docstring: the gate runs the pinned pre-feature reference commit on each commit's own fixture configuration (`required_paths`, `:211-219`) and compares metadata allowing only path-prefix differences, so a leg on a fixture the reference commit never carried cannot be compared; the vertical and version 3 gapped legs remain (a horizontal disabled-mode identity on adjacent input is covered by the package's parity gate); the mutation source for `test_comparator_rejects_mutations` (`:928-929`, today the `sage16` version 2 fixed leg) becomes the `sage16` version 3 gapped fixed leg, every mutation and control case retained; `make tests-snapshot-global-identity` passes with the two remaining fixtures.
- [ ] `tests/framework/parity_gate.py` loses its version 2 branches (`:1019`, `:1083`, `:1095-1109`: the gap census and the manifest-proved inventory now apply to every gate) and `tests/integration/test_processing_order.py` loses its version 2 branch (`:117-129`, the docstrings at `:340-376` and `:852-856`); every gate and the integration tier still pass.
- [ ] `tests/manual/test_distributed_identity.py`: the version 2 refusal leg (`:152-155`, `:586-607`) is removed (the reader's rejection of version 2 as unsupported is Slice 21's unit case); the live check count in the record is updated.
- [ ] Required evidence: the tiers above via subagents; `make tests-distributed`; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `simulations/micro-uchuu-ascii-horizontal/_tests/data/source/`
  - `simulations/micro-uchuu-ascii-horizontal/_tests/data/worked_ascii/`
  - `simulations/micro-uchuu-ascii-horizontal/_tests/data/regenerate.sh` (new file)
  - `simulations/micro-uchuu-ascii-horizontal/_tests/input/test_simulation.yaml`
  - `simulations/shin-uchuu/_tests/data/regenerate.sh`
  - `simulations/shin-uchuu/_tests/data/worked_ascii/`
  - `simulations/shin-uchuu/_tests/input/test_simulation.yaml`
  - `models/sham/input/sham_micro-uchuu-ascii-horizontal.yaml`
  - `models/hod/input/hod_micro-uchuu-ascii-horizontal.yaml`
  - `models/sham/modules/sham_rank_match/_tests/test_integration_sham_rank_match.py`
  - `models/hod/modules/hod_populate/_tests/test_integration_hod_populate.py`
  - `tests/manual/run_snapshot_global_battery.py`
  - `tests/integration/test_snapshot_phase.py`
  - `tests/manual/test_snapshot_disabled_identity.py`
  - `tests/manual/test_distributed_identity.py`
  - `tests/framework/parity_gate.py`
  - `tests/integration/test_processing_order.py`
  - `docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md`
- Functions/classes/components allowed to change: the generator, the two regeneration scripts, the fixture directories, the test configurations, the fixture paths and the pinned values in the named tests, the battery groups, the removed legs and branches.
- Tests allowed or expected to change: the named tests.

### Explicit Non-Goals

- No deletion of the version 2 fixtures, generators, checker or `check-horizontal-fixture` (Slice 23); no C code change (Slice 21); no physics change; no weakening of any case.

### Risk Flags

- Risky surfaces touched: test entry points of four models; committed fixtures.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-unit tests-integration`, the same for `shin-uchuu`, `make MODEL=sham SIMULATION=micro-uchuu-ascii-horizontal tests-integration`, the same for `hod`, `make tests-snapshot-global`, `make tests-snapshot-global-identity`, `MPIRUN="mpirun --oversubscribe" make tests-distributed` (subagents); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the `worked_ascii` fixture's `report` output names forest blocking and the position check passing.

### Rollback Path

- Revert the slice commit.

---

## Slice 20: The version 2 reader's negative battery ported to the version 3 reader test

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: medium. Requires Slice 19.
- Port every case of `simulations/micro-uchuu-ascii-horizontal/_tests/unit/test_unit_horizontal_reader_open.c` that has no like-named analogue in `tests/unit/test_horizontal_v3_reader.c`, so that deleting the version 2 tests loses no coverage of shared reader behaviour.

### Acceptance Criteria

- [ ] `tests/unit/test_horizontal_v3_reader.c` gains, on the version 3 fixtures it already uses: the generic missing and wrong-dtype rows for each of the twelve core header attributes; NaN and infinity in compared physical attributes; the particle-mass naive-comparison trap; a physical mismatch in the last snapshot only; the structural-defect-before-bulk-read case; vector second-dimension truncation; `max_halo_rank_in_forest` below the measured maximum; `test_physical_value_tolerance_boundary`; `test_registry_lookup` and `test_registries_are_disjoint`; `test_slab_lifecycle`; `test_two_generation_rotation_holds_two_slabs_live`; `test_identity_bounds_predicate` and `test_identity_bounds_division_boundary`, each a `MIMIC_RESULT:` marker, each named for the behaviour.
- [ ] `test_unit_horizontal_driver_gather.c`'s coverage of the implicit N−1 link scope is either shown to be covered by `test_horizontal_retention_budget.c` and `test_unit_horizontal_retention` (cited by case name in the slice's validation note) or ported.
- [ ] `make tests-horizontal-v3` passes with the new cases; the version 2 tests are untouched.

### Authorized Surface

- Files allowed to change:
  - `tests/unit/test_horizontal_v3_reader.c`
- Functions/classes/components allowed to change: new cases and table rows; registration.
- Tests allowed or expected to change: `tests/unit/test_horizontal_v3_reader.c`.

### Explicit Non-Goals

- No reader change; no deletion; no fixture change.

### Risk Flags

- Risky surfaces touched: none.
- Approval needed before implementation: no
- Independent audit required: no

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-horizontal-v3`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: a table mapping each version 2 case or row to its version 3 case name is in the slice's validation note.

### Rollback Path

- Revert the slice commit.

---

## Slice 21: Deletion of the version 2 reader path

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slices 19 and 20.
- Remove version 2 from the C reader and driver so the reader accepts exactly version 3.

### Acceptance Criteria

- [ ] `src/io/horizontal/read_horizontal_hdf5.c`: `horizontal_h5_dataset_spec_by_name` (`:1806`) looks up `HORIZONTAL_H5_V3_FIXED_DATASETS`; then the version 2 constants and tables (`:98`, `:181-200`), `horizontal_h5_reject_unknown_attr`, `horizontal_h5_validate_object_set`, `horizontal_h5_read_header`, `horizontal_h5_validate_halo_datasets`, `horizontal_h5_validate_read_list_against_format`, the version 2 link spec, `horizontal_h5_link_limit`, `horizontal_h5_validate_links`, and every version 2 branch of `open_run_horizontal_hdf5`, `horizontal_h5_slab_row_bytes` and `load_slab` are deleted; the open-time message says the reader supports only version 3 and keeps the version 1 hint; `peek_format_version`'s default for a missing attribute becomes a rejection.
- [ ] `src/core/horizontal_driver.c:1073-1080` (the version 2 refusal) and the `descendant_snapshot == NULL` fallbacks (`:1430-1445` and the sites at `:476`, `:495`, `:1833`, `:2357`) are deleted, with the companion columns required; `src/include/types.h:284-316` and `src/io/horizontal/reader.h:55-65`, `:135` lose "NULL for version 2".
- [ ] `tests/unit/test_horizontal_v3_reader.c`: the expected messages at `:1358-1359` become "supports only version 3" and the mixed-version case expects the version 2 file rejected outright; `test_int_link_package_rejects_v3` (`:1035-1050`) is deleted together with its NA allowlist entry in `make tests-horizontal-v3` (`Makefile:884-889`), because after Slices 5 and 10 no shipped package declares `int` links, and the reader branch at `read_horizontal_hdf5.c:1553-1577` stays as the fast-failure guard for a future package with a one-line comment saying so; `tests/unit/test_horizontal_retention_budget.c`'s fake reader labels drop "version 2"; the two fixture-based package-local C tests `simulations/micro-uchuu-ascii-horizontal/_tests/unit/test_unit_horizontal_reader_open.c` and `test_unit_horizontal_driver_gather.c` are deleted, and `tests/unit/run_tests.sh:279` loses the `test_unit_horizontal_reader_open` entry (`realdata`'s entry stays; `driver_gather` has none); the `read_horizontal_hdf5.c:2313` comment no longer points at the deleted test.
- [ ] Clean build under `-Wall -Wextra -Wshadow -Wformat-security -Wundef`; `make tests-horizontal-v3`, `make tests-unit`, `make tests-integration`, `make tests-scientific` and `MPIRUN="mpirun --oversubscribe" make tests-distributed` pass; the deleted line count is reported as descriptive evidence against the inventory's about 520.

### Authorized Surface

- Files allowed to change:
  - `src/io/horizontal/read_horizontal_hdf5.c`
  - `src/io/horizontal/reader.h`
  - `src/include/types.h`
  - `src/core/horizontal_driver.c`
  - `Makefile`
  - `tests/unit/test_horizontal_v3_reader.c`
  - `tests/unit/test_horizontal_retention_budget.c`
  - `tests/unit/run_tests.sh`
  - `simulations/micro-uchuu-ascii-horizontal/_tests/unit/test_unit_horizontal_reader_open.c`
  - `simulations/micro-uchuu-ascii-horizontal/_tests/unit/test_unit_horizontal_driver_gather.c`
- Functions/classes/components allowed to change: the named functions, tables, branches, comments and test entries; deletions of the two test files.
- Tests allowed or expected to change: as listed.

### Explicit Non-Goals

- No change to the version 3 read path's behaviour or messages beyond the version check; no Python change; no documentation (Slice 24).

### Risk Flags

- Risky surfaces touched: the reader and driver core.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make clean && make`; `make tests-horizontal-v3`; `make tests-unit`, `make tests-integration`, `make tests-scientific` (subagents); `MPIRUN="mpirun --oversubscribe" make tests-distributed`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: opening the retained version 2 micro-Uchuu dataset fails with the new message; the version 3 micro-Uchuu dataset opens.

### Rollback Path

- Revert the slice commit.

---

## Slice 22: Deletion of the version 2 converter, validator and crosscheck

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 21.
- Remove the version 2 producer and its batteries, the rewriter's version 2 input path, and rework the tests that used the version 2 writer as a harness.

### Acceptance Criteria

- [ ] Deleted: `convert/mimic-convert/convert_ctrees.py`; `crosscheck.py` and `tests/test_crosscheck.py` (decision 3); `hdf5_writer.py` and `validate.py` (their surviving names moved by Slice 18; the `validate` CLI's version 3 dispatch moves into `validate_v3.py`'s `main` and `convert_trees.py validate` is the documented entry point); in `report.py` the version 2 functions `build_report`, `render_text`, `write_report` and `run_report` (`:107-296`; the version 3 constants from `:298` and everything after stay); `scatter.py:1499-1582` (`run_release`, `run_finalize`); the default `mimic-topology-dump v1` mode of `tests/unit/tools/dump_ctrees_topology.c` (`:25-34`, `:119-130`), which exists only to feed crosscheck, leaving `--source-payload` as the tool's one mode, with `Makefile:935`'s comment retargeted; `tests/data/legacy_manifest_v2/`; the version 2 halves of `tests/test_validate.py` (`:1-2180`) and `tests/test_hdf5_writer.py` (`:79-918` except the shared header-metadata and multiplier cases, which move to the version 3 classes); `tests/test_cli.py:685-733` and the `--help` assertion at `:214`; `tests/test_scatter.py:1823-1920`; the `migrate` subcommand and `horizontal_dataset.py`'s version 2 branch in the rewriter and census, with their tests.
- [ ] Reworked, not weakened: `test_links.py`, `test_fixups.py`, `test_sort_index.py`, `test_ascii_adapter.py`, `test_conversion_manifest.py` and `mock_reference.py` set up their datasets through the version 3 route or the adapter fixtures instead of `run_write` or `convert_ctrees.main`; a coverage map in the slice's validation note pairs every removed case with the surviving case that holds its obligation, or states that the obligation itself retired with the interface (release, finalize, the consumptive writer); names describe current behaviour.
- [ ] The surviving converter modules lose their live version 2 statements and paths: `hdf5_writer_v3.py:12-13`, `validate_v3.py:7-9`, `convert_trees.py:40-41`, `:115`, `:833`; `conversion_manifest.py:19-25` and its legacy-manifest classification (`LEGACY_MANIFEST_VERSION`, `MANIFEST_LEGACY`, `classify_manifest`'s hand-back, `:90-102`, `:473-480`, and `pipeline.py:715`'s refusal), which only `tests/data/legacy_manifest_v2/` exercised; `scatter.Manifest`'s own version (the ASCII preparation state the version 3 route shares) and `column_schema.py:1323`'s profile-grammar comment are not version 2 statements and stay. `convert_trees.py --help` and the converter manual no longer mention `convert_ctrees.py`, batch mode, release, finalize or `--consume-intermediates`; the manual's "Legacy ASCII-to-v2" section (`README.md:146-378`) and "Reference-topology proof" (`:415-450`) are deleted and the module map lists only what exists.
- [ ] **Battery disposition** (revision 5 placeholder; the revision before run 6 replaces this bullet with the decided text and the surface it needs): either the external-sort implementation is removed with its dispatch, options, tests and live documentation, and the manual names the bounded mode as the battery, once that revision extends the bounded mode to gapped input or declares gapped input unsupported by the battery, with the reason recorded; or both stay, with their separate supported purposes recorded in the manual.
- [ ] `make tests-converter` passes; the deleted line count is reported in the slice's validation note as descriptive evidence against the inventory (about 2,600 source, 5,000 tests, with crosscheck).

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/convert_ctrees.py`
  - `convert/mimic-convert/crosscheck.py`
  - `convert/mimic-convert/hdf5_writer.py`
  - `convert/mimic-convert/validate.py`
  - `convert/mimic-convert/validate_v3.py`
  - `convert/mimic-convert/hdf5_writer_v3.py`
  - `convert/mimic-convert/conversion_manifest.py`
  - `convert/mimic-convert/pipeline.py`
  - `convert/mimic-convert/report.py`
  - `convert/mimic-convert/scatter.py`
  - `convert/mimic-convert/convert_trees.py`
  - `convert/mimic-convert/rewrite_trees.py`
  - `convert/mimic-convert/rewriter/`
  - `convert/mimic-convert/horizontal_dataset.py`
  - `convert/mimic-convert/forest_census.py`
  - `convert/mimic-convert/census/`
  - `convert/mimic-convert/README.md`
  - `convert/mimic-convert/tests/`
  - `tests/unit/tools/dump_ctrees_topology.c`
  - `Makefile`
- Functions/classes/components allowed to change: deletions as listed; the `validate` entry point move; the dump tool's default mode; one Makefile comment; the version 2 statements and the legacy-manifest path in the surviving modules; the test harness rework; the manual's sections.
- Tests allowed or expected to change: the converter tests as listed.

### Explicit Non-Goals

- No change to the version 3 ASCII route's stage modules beyond removing the two version 2 CLI functions from `scatter.py`; no fixture deletion outside `tests/data/legacy_manifest_v2/` (Slice 23); no guide, skill or changelog edit (Slice 24).

### Risk Flags

- Risky surfaces touched: the converter's public CLI; a large deletion.
- Approval needed before implementation: yes
- Independent audit required: yes

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `make dump-ctrees-topology-tool`; `git grep -n "convert_ctrees\|crosscheck\|run_release\|run_finalize\|consume-intermediates" -- convert/ tests/ Makefile` finds nothing, and `git grep -n -i "version 2\|format_version 2\|to-v2\|legacy" -- convert/ ':!convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md'` finds only the profile-grammar comment (the guides, skills, READMEs and the format document are Slice 24's); `./scripts/beautify.sh`; `make check-format`; `make check-docs`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `convert_trees.py validate --workdir` on the `worked_ascii` fixture's workdir still passes through the moved entry point.

### Rollback Path

- Revert the slice commit.

---

## Slice 23: Deletion of the version 2 fixtures, generators, make target and CI step

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. Requires Slice 22.
- Remove the committed version 2 fixtures and everything that only generated or checked them.

### Acceptance Criteria

- [ ] Deleted: `simulations/micro-uchuu-ascii-horizontal/_tests/data/snapshot_00[0-5].h5`, `forests.h5`, `micro-uchuu-fixture.a_list`, `fixture_manifest.json` and the whole `_tests/data/generic/`; `simulations/shin-uchuu/_tests/data/`'s version 2 files and manifest; `_tests/input/create_snapshot_fixture.py` and `check_fixture_conformance.py`; the `check-horizontal-fixture` target (`Makefile:821-826`), its call in `tests` (`:758`), its help line (`:511`) and the CI step (`.github/workflows/ci.yml:57-59`).
- [ ] Nothing under `simulations/`, `tests/`, `models/`, `scripts/` or `.github/` references a deleted path (`scripts/fuzz_pipeline.py` names run files by their unchanged names and needs no edit).
- [ ] `make tests` (with `summary`) passes under the default pair; `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests` passes.

### Authorized Surface

- Files allowed to change:
  - `simulations/micro-uchuu-ascii-horizontal/_tests/data/`
  - `simulations/micro-uchuu-ascii-horizontal/_tests/input/create_snapshot_fixture.py`
  - `simulations/micro-uchuu-ascii-horizontal/_tests/input/check_fixture_conformance.py`
  - `simulations/shin-uchuu/_tests/data/`
  - `Makefile`
  - `.github/workflows/ci.yml`
- Functions/classes/components allowed to change: deletions; the make target and CI step removal.
- Tests allowed or expected to change: none beyond deletions.

### Explicit Non-Goals

- No code change elsewhere; no documentation (Slice 24); the `worked_ascii/` fixtures and `source/` of Slice 19 are untouched.

### Risk Flags

- Risky surfaces touched: the Makefile and CI configuration.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make tests summary` (subagent); `make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests summary` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `git grep -n "fixture_manifest\|check_fixture_conformance\|create_snapshot_fixture\|check-horizontal-fixture" -- ':!CHANGELOG.md' ':!docs/dev'` returns nothing.

### Rollback Path

- Revert the slice commit.

---

## Slice 24: Version 2 frozen as history; guides, skills, changelog and pathway closeout

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. Requires Slice 23.
- Freeze the format document's version 2 text as history, remove every live statement about version 2 from the guides, skills, READMEs and tests README, write the changelog entry, and close the plan in the pathway.

### Acceptance Criteria

- [ ] `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`: the Status line says version 3 is the only supported version and version 2 is retained as history; the version 2 body (`:33-166`) is kept under a heading "Version 2 (historical, unsupported since <date>)" with one introductory sentence and no normative wording change; "Version 3 is also normative" and the reader-accepts-both sentences (`:9`, `:493`, `:495`) say the reader accepts version 3 only; the version 2 comparisons inside the version 3 sections (the "What Version 3 Changes" table and the sentences at `:206`, `:208`, `:212`, `:225`, `:229`, `:231`, `:243`, `:260`, `:286`, `:389`, `:399`, `:406`, `:416`, `:423`, `:427`, `:429`, `:453`, `:469`, `:506`, `:537`) are kept where they explain a version 3 rule and reworded in the past tense where they describe reader behaviour; one errata row records the retirement; the version ratchet's text is unchanged.
- [ ] `docs/USER-GUIDE.md` and `docs/DEVELOPER-GUIDE.md`: every version 2 sentence (the inventory's line lists) is removed or made historical; `docs/DEVELOPER-GUIDE.md:1064`'s vertical statement is untouched.
- [ ] Skills: the inventory's line lists in `mimic-run-and-operate`, `mimic-simulations-and-readers`, `mimic-validation-and-qa`, `mimic-debugging-playbook`, `mimic-architecture-contract`, `mimic-docs-and-writing`, `mimic-build-and-env`, `mimic-config-and-flags` (and `references/all-config-keys.md`) lose their live version 2 statements; `mimic-failure-archaeology`'s chronicle entries stay as history and gain one entry for the retirement.
- [ ] `simulations/micro-uchuu-ascii-horizontal/README.md`, `simulations/micro-uchuu-hdf5-horizontal/README.md:78`, `simulations/micro-uchuu-horizontal/README.md:79`, `simulations/micro-uchuu/README.md:34`, `models/hod/README.md`, `models/sham/README.md`, `tests/README.md`, `AGENTS.md:75` and `plot/mimic-plot/README.md` lose their version 2 references; `tests/README.md:64-72` describes the version 3 ASCII fixture and the retired target.
- [ ] `CHANGELOG.md` Unreleased: the retirement entry (what was removed, the reader's new rejection, the fixture swap, the line count), in the existing style; historical entries untouched.
- [ ] `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: the named follow-up and Completed Work record the plan complete with the record's numbers; the inventory marks this plan and the brief for archiving on merge and keeps the two records as standing evidence; the full-Uchuu follow-up is restated as the remaining item.
- [ ] **Revision 5: final-state check** (owner, 2026-10-09: no stale or dead code or docs may remain that are unrelated to what Mimic is after this plan).
  - Every row of the [Final-State Inventory](#final-state-inventory) is confirmed done, with the commit that did it, in the slice's validation note.
  - A grep for every retired module, subcommand, option and term the inventory names finds nothing outside history, `CHANGELOG.md`'s historical entries and the failure-archaeology chronicle. That covers code, tests, docs, skills, READMEs and run-file comments.
  - Every surviving tool this plan added is described in the documentation of record.
- [ ] `make check-docs` passes; `git grep -n -i "version 2\|format_version 2\|version-2\|to-v2" -- docs/ .agents/ simulations/ convert/ tests/README.md README.md AGENTS.md ':!docs/dev' ':!CHANGELOG.md'` finds only the format document's historical section and the chronicle (the converter's internal scratch-layout tags such as `ctrees-scratch-v2`, `ctrees_parser.py:105`, are not version 2 statements and are left alone); and the manual `docs/dev/` citation grep of Slice 6 finds nothing.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md`
  - `docs/USER-GUIDE.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `.agents/skills/mimic-run-and-operate/SKILL.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-debugging-playbook/SKILL.md`
  - `.agents/skills/mimic-architecture-contract/SKILL.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
  - `.agents/skills/mimic-build-and-env/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/SKILL.md`
  - `.agents/skills/mimic-config-and-flags/references/all-config-keys.md`
  - `.agents/skills/mimic-failure-archaeology/SKILL.md`
  - `.agents/skills/mimic-failure-archaeology/references/chronicle.md`
  - `simulations/micro-uchuu-ascii-horizontal/README.md`
  - `simulations/micro-uchuu-hdf5-horizontal/README.md`
  - `simulations/micro-uchuu-horizontal/README.md`
  - `simulations/micro-uchuu/README.md`
  - `models/hod/README.md`
  - `models/sham/README.md`
  - `tests/README.md`
  - `AGENTS.md`
  - `plot/mimic-plot/README.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
  - `docs/dev/MIMIC-SHIN-UCHUU-V3-PLAN.md`
- Functions/classes/components allowed to change: prose only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code; no archiving (the owner archives on merge); no change to the ruling's text; no new claim.

### Risk Flags

- Risky surfaces touched: the format document; `AGENTS.md`.
- Approval needed before implementation: yes
- Independent audit required: no

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`; the grep above; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the format document's historical section is clearly labelled and reachable from the table of contents; no document outside `docs/dev/` cites a `docs/dev/` document.

### Rollback Path

- Revert the slice commit; documentation only.

---

## Next Chat Prompts

### Mode A — Checkpointed alternative

```text
Plan file: docs/dev/MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md
Slices this session: Slice 1

Read the full plan. Stop before coding if any receipt is incomplete or baseline drift
changes its meaning. Work on the current branch; do not create a branch without my instruction.
Use orchestrator as the controlling skill. Act as the Developer: implementation, validation,
Git and commits stay local. Use the slice's recommended Developer model/effort and a fresh
read-only Claude Fable 5.1 high-effort Reviewer for each independent review. Do not substitute
or self-audit.

For the selected slice: restate the frozen contract; stop for my approval if its Risk Flags say
approval is needed; apply scoped-implementation; apply drift-audit through the Reviewer and
report the authorization result before any quality review; on a passing gate apply code-review
through the Reviewer (run differential code-health first where the change is structural); fix
findings and re-run the relevant gate; ask me before committing; commit with the commit skill;
then use the handoff skill to record state and the next slice.

Confirm the plan, selected slice, branch and model/effort before beginning.
```

### Mode B — Supervised execution (run 1, Stage A)

```text
Plan file: docs/dev/MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md
Repo: /Users/dcroton/Local/git-repos/mimic
Developer: harness claude model sonnet effort high initially
Reviewer: harness claude model claude-fable-5-1 effort high

Use project-manager. You are the accountable PM and never write slice code.
Read the complete frozen plan; run check-plan with repo context. Require a clean committed
planning baseline and resolve any drift against the plan's anchors first. Create the feature
branch I name at init; never run on main. This run is Stage A: Slices 1 to 6 only; stop the run
after Slice 6 is decided (Stages B, R, C and D are later runs that attest the earlier slices).
Record human approval for each flagged slice (1, 2, 3, 5, 6) before starting it; neither this
launcher nor the plan grants those approvals.
Ask for commit authorization before launch unless I have already explicitly granted it; no
session pushes. Keep PM_RUN_TOKEN private to the PM seat. Preflight make, make USE-MPI=yes
(Open MPI), mimic_venv with h5py and numpy, HDF5, the committed fixtures, and that
simulations/micro-uchuu-ascii/snapshots and simulations/micro-uchuu-ascii-horizontal/snapshots
resolve, without changing source data.

For each slice in order:
1. Launch a fresh Developer, explicitly passing that receipt's model and effort to
   start-slice (Sonnet high for 1 and 6; Opus high for 2, 3 and 5; Opus medium for 4).
2. Wait with one long observe --wait rather than repeated checks; Slice 5 runs real data and
   may need several hours, so wait accordingly.
3. Check the mechanical floor, then the frozen authorization contract and the actual evidence;
   run lint yourself; rerun the slice's validation commands where risk or doubt warrants.
4. For elevated slices commission fresh independent drift-audit; read and judge it and report
   authorization before commissioning fresh code-review. Both must cover the exact final
   commit. Record reviewer and developer judgments.
5. A contract defect stops the run; do not amend this plan. Never waive a gate leg or accept a
   serial-identity regression.

Confirm the plan, branch, Developer/Reviewer models and effort, approvals and first slice.
After all six slices are decided, read the PM report and report its verified total elapsed
time, accepted commits and evidence, audit provenance, plan defects, stops and residual
limitations.
```

### Mode B: Supervised execution (run 2b, the revised Slice 9)

```text
Plan file: docs/dev/MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md (revision 5)
Repo: /Users/dcroton/Local/git-repos/mimic

Start by reading the full HANDOFF.md closely.

Harness/models:
- Developer: claude, Opus 5.5 at high effort (the profiles table, Slice 9)
- Drift audit: claude, Sonnet 5.5 at high effort
- Code reviewer 1: claude, Fable 5.1 at high effort
- Code reviewer 2: codex, gpt-6.1-sol at high effort
Run the drift audit first, then the two code reviews in parallel with staggered starts.
Complete all reviews before steering.

Use project-manager. You are the accountable PM and never write slice code.
Read the complete frozen plan, then run check-plan with repo context.
Require a clean committed baseline with revision 5 committed on feature/shin-uchuu-v3.
Init on that branch with --branch feature/shin-uchuu-v3 and
--attest "Slice 1,Slice 2,Slice 3,Slice 4,Slice 5,Slice 6,Slice 7,Slice 8".
Run only Slice 9 (revision 5), then stop.
Record human approval for Slice 9 before starting it.
Ask for commit authorization before launch. No session pushes.
Keep PM_RUN_TOKEN private to the PM seat.

The panel must also review the helpers run 2's three performance commits added that survive.
They fall outside this run's diff range: in_sorted_window, slab_prefix, and the once-sorted
fresh-piece order.

For Slice 9, independently trace every census-record number to its aggregate or log.
At the end, update HANDOFF.md and add the run-3 (Stage R) launcher.
```

Later runs use the same launcher with the run's slice range, its approvals (run 2: 8, 9; run 2b: 9; run 3: 11, 12, 13, 14, 15; run 4: 16; run 5: 17; run 6: 19, 21, 22, 23, 24), its models from the profiles table, and `--attest` for every earlier slice.
