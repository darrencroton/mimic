# Shin-Uchuu Forest Census — Stage B Record

**Purpose:** The measured record the owner signs off for Stage B: the decided cut table for the whole Shin-Uchuu simulation (every forest cut at its z = 0 FoF groups), what it costs (promotions, progenitor-order changes, affected histories), and how the cut dataset partitions under chunked slab streaming. Written by Slice 9 (revision 5) of [`MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md`](MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md) for the decision gate of the brief, [`MIMIC-SHIN-UCHUU-V3-PLAN.md`](MIMIC-SHIN-UCHUU-V3-PLAN.md) ("The decision gate"). F11 names it as Stage B's standing evidence.

**Status:** Measured 2026-10-09 (run 2b). The cut rule is the owner's decision of 2026-10-09 (plan F6, "The decided table"); this record measures it and does not revisit it. The **two decision slots** at the end are **empty**: they are the owner's. Nothing here is a recommendation. No physics claim is made beyond the measured counts.

**Evidence.** Every number below is quoted from a file named beside it. The files are census summaries in an aggregate directory, logs and scripts under `/Volumes/Internal/results/mimic/shin-uchuu-v3-stage-b/` (abbreviated `$B`), or PM's decision evidence under `.orchestrator/stage-b-decision/` in this repository's working tree (gitignored; abbreviated `$D`). Each `$B/logs/rev5-<step>.log` holds the subcommand's command line, the commit, the uncommitted census paths and the SHA-256 of every census source file, its output, `/usr/bin/time -l` (wall-clock, peak RSS, peak memory footprint), its exit status, and `du -sk` and `df -k` before and after. The scripts the record uses are `$B/scripts/rev5_run_step.sh` (the step wrapper), `rev5_reuse_checks.py`, `rev5_compare_rehearsal.py`, `rev5_extract_record.py`, `rev5_census_facts.py` and `rev5_measure_memory.py`; `$B/logs/rev5-*-extract*.txt` and `rev5-production-facts.txt` print every summary key the record quotes. The aggregate directories are:

- `$P` = `/Volumes/LaCie/data/uchuu/shin-uchuu-census/` (production);
- `$X` = `/Volumes/LaCie/data/uchuu/shin-uchuu-census-run2-retired/` (run 2's retired `graph/` and candidate `cut/` outputs, moved aside, not deleted);
- `$R3` = `/Volumes/LaCie/data/uchuu/micro-uchuu/census-rev5-v3/` and `$R2` = `/Volumes/LaCie/data/uchuu/micro-uchuu/census-rev5-v2/` (rehearsal).

**Code.** The production `occupancy`, `trees` and `partition` aggregates are run 2's, written at commit `5b7edec1` and reused after the four checks below; this slice changes neither their on-disk format nor their meaning (`census/occupancy.py`, `census/partition.py` and the code of `census/trees.py` are unchanged). Every other step ran `convert/mimic-convert/forest_census.py` at this slice's code before it was committed: each `rev5-*` log records `# git 40a448d5` with the census paths modified, and the SHA-256 of every census source file, which equal the committed files' (verified after the commit; [Code identity](#code-identity)). `mimic_venv` (Python 3.14.6, h5py 3.15.1, numpy 2.3.4). Host: the Mac Studio (macOS, 32 cores, 512 GiB RAM); the datasets and aggregates are on the LaCie (an external disk). Every production step ran strictly in sequence, with no other LaCie-bound job alongside.

---

## Memory at a glance

Rows are the most halos one chunk holds resident in any slab; bytes are at **1,100 B per resident halo**, the chunked streaming record's figure, an approximate scale and not an RSS bound. "Job" is the sum over tasks of each task's widest chunk over all slabs (the ranks of one laptop run at once); "process" is one rank's widest chunk.

| Quantity | Rows | At 1,100 B per halo | Source |
|---|---:|---:|---|
| Uncut, unchunked (one task, one chunk): the widest slab | 519,342,987 (snapshot 34) | 571.3 GB | `$P/partition/summary.json` grid (1, 1) `widest` |
| Uncut, chunked at any task and chunk count: the super-forest's peak, which no partition splits | 333,663,215 (snapshot 31) | 367.0 GB | `$P/partition/summary.json` `floor` |
| The decided table's largest piece (its peak): the cut dataset's floor at any chunk count | 3,415,844 (snapshot 31) | 3.76 GB | `$P/cut/summary.json` `pieces.highest_peak[0]` |
| The widest slab spread evenly over G chunks (a floor for any cut): G = 32, 64, 128 | 16,229,469; 8,114,735; 4,057,368 | 17.9; 8.9; 4.5 GB | 519,342,987 / G, rounded up |
| The single-tree floor: the largest tree's peak, which no whole-tree cut goes below | 2,504,273 (snapshot 31) | 2.75 GB | `$P/trees/summary.json` `largest_tree` |
| The decided table's smallest job on the grid ({1, 2, 4, 8} × {1, …, 128}) | 29,113,469 (one task, 128 chunks) | 32.0 GB | `$P/cut/summary.json` `partition.grid` |
| Class 16 GiB less a 4 GiB reserve (12,884,901,888 B usable): smallest fitting job | none on the grid | — | `partition.laptop_classes` |
| Class 32 GiB less a 4 GiB reserve (30,064,771,072 B usable): smallest fitting job | none on the grid | — | `partition.laptop_classes` |
| Class 64 GiB less a 4 GiB reserve (64,424,509,440 B usable): smallest fitting job | 57,143,838 (one task, 16 chunks) | 62.9 GB | `partition.laptop_classes` |

The driver's transient startup weights are 8 B × `n_forests_total`: 1.33 GB for the uncut dataset's 166,547,771 forests and 1.96 GB for the cut dataset's 244,953,607 (`$P/cut/summary.json` `memory.startup_weights`). The table record is 2,954,410,424 B and the table 7,560,101,829 B (`$P/table/summary.json` `aggregates`).

---

## Inputs

| Input | Path | Identity |
|---|---|---|
| Production dataset (version 2) | `/Volumes/LaCie/data/uchuu/shin-uchuu/` | `format_version` 2; `n_forests_total` 166,547,771; 70 slabs; sidecar `ForestID` SHA-256 `f4807578c85b8a8b85ea4706d901aeb3ab3fc17cc64e4d391624203db6336063` (`$P/identity.json`; `$B/logs/rev5-production-reuse-checks.log`) |
| Production `forests.list` | `/Volumes/LaCie/data/uchuu/shin-uchuu-nt-transfer/forests.list` | 7,560,101,829 B, md5 `60dbf14a99ae9f23ca7c334474477a95`, equal to the production conversion report's (`rev5-production-reuse-checks.log`; `$P/table/summary.json` `index_files`) |
| Production `locations.dat` | `/Volumes/LaCie/data/uchuu/shin-uchuu-nt-transfer/locations.dat` | 13,748,317,434 B, md5 `75442d7ae5b1f2465b485e60720e6df7`, equal to run 2's record (the plan's stage map); the report records none |
| Production conversion report | `/Volumes/LaCie/data/uchuu/shin-uchuu/conversion-provenance/shin-uchuu-v2/conversion_report.json` | `source_files` with per-file `parsed_count`, 2,744 files |
| micro-Uchuu version 3 dataset | `/Volumes/LaCie/data/uchuu/micro-uchuu/micro-uchuu-ascii-horizontal-v3` | `format_version` 3, `source_format` `consistent_trees_ascii`; 440,651 forests; sidecar SHA-256 `2b97250084e40fed784ff66b822678d7aa205c97f6914bd1752531ec7371c81b` (`$R3/identity.json`) |
| micro-Uchuu version 2 dataset (retained) | `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii-horizontal` | `format_version` 2, no `source_format`; the same sidecar SHA-256 (`$R2/identity.json`) |
| micro-Uchuu index files | `/Volumes/Internal/data/uchuu/micro-uchuu/micro-uchuu-ascii/{forests.list,locations.dat}` | md5 `189eb35891a07d99cce831f3858af452` and `1d27eda7762fdabd79ab2d326d866838` (`$R3/table/summary.json` `index_files`) |
| micro-Uchuu conservation source | `/Volumes/LaCie/data/uchuu/micro-uchuu/stage-a-v3-workdir/ascii_preparation/manifest.json` | `source_files` with `parsed_count` 22,580,924 for `tree_0_0_0.dat` |

The Stage A conversion's `conversion_report.json` (version 3) carries no per-file counts. The conservation check therefore uses the conversion's own per-file parsed counts, which live in its ASCII preparation manifest (PM ruling DD13); both rehearsal censuses use it.

---

## Definitions

**Effective descendant trees.** A tree is a component of the descendant graph rooted at a terminal halo, one whose `Descendant` is −1. Its root id is that halo's `MostBoundID`, and its **root ordinal** is the position of that id in the sorted array of all terminal ids. `trees` labels every halo with its tree's root ordinal in a backward pass from the last slab. Descendant links are never rewritten, so a tree is an indivisible history. **Equality of these trees with Consistent-Trees' physical `#tree` blocks is not established** (F7): the root correspondence and the conservation check are necessary for it, not sufficient, and this record never assumes it.

**Root correspondence.** The census root set is compared with the index files' tree roots: roots missing from `forests.list`, listed roots with no terminal halo, roots outside the last slab, and roots whose census forest differs from the one `forests.list` gives. `forest_mismatch_halos` counts halos whose `ForestIndex` is not their tree's forest.

**Conservation.** Per source file, the census sums the tree totals through `locations.dat` (tree root → file) and compares each sum with the file's `parsed_count`.

**z = 0 FoF groups.** At the dataset's final snapshot (Shin-Uchuu: snapshot 69, scale factor 0.99998; micro-Uchuu: snapshot 49, 0.99951), every halo's `FirstHaloInFOFgroup` names its group's central, the row that names itself. The groups are the effective (post-fix-up) ones the dataset records. A group is identified by the **tree of its central**. `table` validates every reference before it builds anything: in range, the named row its own central (self-central), and that row in the member's original forest.

**The decided rule** (F6, revision 5). Every tree joins the forest of its terminal halo's z = 0 FoF group, for every forest of the simulation. The rule's scope is catalogues in which every tree reaches the final snapshot, as Consistent-Trees guarantees; `table` and `cut` refuse a census holding a tree that ends earlier, naming the count, before any output. Then each tree has exactly one halo in the final slab, and its piece is its terminal halo's central's tree. A forest whose trees end in one group is unchanged. A forest ending in several (a **split forest**) is cut into exactly its groups, and `table` asserts for every split forest that its piece count equals its z = 0 centrals.

**Equivalence.** The decided table is exactly the partition that cuts every co-membership ended before the final snapshot and keeps every one present at it: a pair of trees co-membered at the final snapshot is in one z = 0 group, so the kept pairs join exactly the trees of each group. The test suite computes that partition independently from the raw slab columns and compares it with the table. It follows that no halo is promoted at the final snapshot (`cut` checks this in every run), every z = 0 FoF group of the input is preserved, and no tree is split. "Ended" is an operational definition fixed by the dataset's final snapshot, not a claim that an encounter was an artificial join or a physical flyby.

**Pieces** (F6 naming). A piece's size is its total halos over all snapshots, ties broken by the smallest tree root id over the piece's trees (which need not be its central's tree). The largest piece of a split forest keeps the forest's id; every other piece takes a fresh id above `fresh_id_floor`, the sidecar's largest forest id, which the passed index check makes the index files' largest too. Fresh ids are assigned in descending size with the same tie rule, so every original forest keeps its `ForestIndex` and the fresh pieces enumerate after them. A **forest selection** (`--forest-index`) splits only the named forests; every other forest keeps its identity assignment, and the outputs (`table-restricted/`, `cut-restricted/`) say so.

**Promotions** (per slab, then summed):

- **promoted halos** are members whose FoF central lies in another piece; each becomes a central;
- **groups losing members** are FoF groups with at least one promoted member;
- **(group, piece) remnants** (`groups_central_leaves_members_stay`, PM ruling DD8) are a piece's members of one group whose central lies in another piece. Each member becomes its own central: no host is invented. `remnants_with_several_members` counts the remnants of two or more members, which stay together in one piece but no longer share a group.

**Progenitor-order changes** (F4). A descendant's progenitors are encountered in ascending (upid, pid, id), with upid the FoF central's `MostBoundID` and pid −1 for a central, else the central's id; a promoted halo becomes (id, −1, id). The chain is the reference incremental-insertion loop on `M_Crit200` (the native float32 Consistent-Trees `Mvir`). Only descendants with a promoted progenitor can change. For each one with two or more progenitors, the chain before the cut is also rebuilt from the dataset's stored `NextProgenitor` links and compared with the recomputed one (`stored_chain_check.mismatches`, which must be 0 for the prediction to stand). The key reads the post-fix-up hosts, which is **exact for Consistent-Trees-derived datasets only**; both datasets here are.

**Affected-history bracket** (PM ruling DD9, F7). The **seeds** are the promoted halos, the centrals of groups that lose members, and the descendants whose progenitor chain changes. **`dependent_halos`** counts the seeds and every halo on a seed's descendant path, propagated slab by slab. **`upper_bound_halos`** counts every halo of a piece holding a seed: a piece without one keeps its topology and its relative order. Both are input-topology counts, not a prediction of how many galaxies differ.

**Re-labelled halos.** Halos given a new forest id (those of fresh pieces); halos whose `HaloRankInForest` is recomputed (every halo of a split forest); and halos whose `SourceHaloID` prefix moves (every halo of every forest from the first split one in `ForestIndex` order).

**Partition with the pieces installed.** The exact simulation of `horizontal_partition_cut` (`src/core/horizontal_partition.c`): minimax contiguous packing of the widest slab's per-forest rows into `ntask` task ranges, then `nchunk` chunks within each task, with the pieces installed (each original forest keeps its `ForestIndex`; the fresh pieces follow in ascending fresh id). The grid is (ntask, nchunk) ∈ {1, 2, 4, 8} × {1, 2, 4, 8, 16, 32, 64, 128}. Each point records the **process** figure (the widest range over all slabs: one rank's working set, the per-rank `retention_memory_ceiling_mb` guidance) and the **job** figure (the sum over tasks of each task's widest chunk over all slabs; the ranks of one `mpirun` job run at once and do not synchronise during the sweep, so this is the conservative bound).

**Laptop rows.** One row per class (16, 32 and 64 GiB): the smallest grid point (fewest ranges, then fewest tasks) whose **job** bytes at 1,100 B per halo fit the class **less a usable reserve of 4 GiB**. The reserve is an argument of `cut` (`--reserve-gib`), chosen for this presentation only and not a recommendation; every grid point's job bytes are in `$P/cut/summary.json`, so any other budget can be read off them.

---

## The evidence for the decision

The owner decided the rule on 2026-10-09 from run 2's measurements and PM's informal estimates; they are recorded here as that evidence, labelled as what they are. The code that produced run 2's graph and preview is retired by this slice (its outputs are kept in `$X`); the decided table's own figures below supersede the estimates.

**Run 2's co-membership graph of the super-forest** (`$X/graph/summary.json`, moved aside from `$P/graph/`; `$B/logs/production-graph.log`). Over the super-forest's 104,845,278 trees it found 177,551,953 distinct tree pairs from 951,063,057 (slab, pair) entries, 1,085,744,968 cross-tree members over all slabs, and 54,039,445 members in as many pairs at snapshot 69. Its complete graph had 21,527 components, the largest holding 12,642,063,653 halos (104,788,727 trees).

**Run 2's rule preview** (`$B/logs/production-rule-preview.log`, `$B/scripts/preview_rules.py`; run 2's union-find over the graph's merged pairs; super-forest only; component totals only, **no memory figure**). Duration thresholds d = 2, 5, 10, 20 left a largest component of 8,938,717,241, 683,058,267, 359,917,233 and 204,555,077 halos; every previewed rule left one of at least 1.7 × 10⁸ halos.

**PM's informal estimates** (read-only scripts on run 2's aggregates; **informal, not census outputs**):

- `$D/evidence/flyby_estimate.log` and `flyby_sweep.log`: cutting every pair ended before snapshot 69 kept 54,039,445 of 177,551,953 pairs and gave 50,805,833 super-forest pieces, the largest by total 126,380,326, 71,150,152 and 55,080,340 halos. The largest pieces' peaks were sampled over snapshots 24 to 40 and the top five pieces only: 3,415,844 rows at snapshot 31 for the first. Thresholded variants ("ended and shorter than d snapshots", d = 2 to 20) left largest pieces peaking at 245,244,630 to 6,766,164 rows over the same slabs.
- `$D/evidence/z0_groups.py` (a count on snapshot 69; its output is quoted in `$D/decision-brief.md` and the plan's F6 section, not logged): 244,953,607 z = 0 groups in all, 50,805,833 in the super-forest, and 8,053,561 other forests ending in more than one group, holding 27,600,004 extra groups and 4,519,034,547 halos.

The census reproduces each of PM's counts independently in [The decided table](#the-decided-table).

---

## Rehearsal: micro-Uchuu, version 3 and version 2

**Runs.** Every subcommand ran on the version 3 dataset into `$R3` and on the retained version 2 dataset into `$R2`, with identical arguments (`$B/scripts/rev5_rehearsal_census.sh`, `rev5_rehearsal_table_cut.sh`): `occupancy`; `trees` with the micro-Uchuu index files and the preparation manifest; `partition` over {1, 2, 4, 8} × {1, 2, 4, 8, 16, 32}; `table` for the whole simulation; `cut` with `--reserve-gib 4`; and `table` and `cut` restricted to the largest forest (`--forest-index 237997`). Each exited 0 in at most 2.2 s and 0.49 GB peak RSS (`$B/logs/rev5-rehearsal-v3-<step>.log`, `rev5-rehearsal-v2-<step>.log`). A first round (logs `*-attempt1*`) ran before a one-string fix to `census/table.py`'s summary text and gave the same figures; the round quoted here ran at the code production ran.

**Agreement through the catalogue id** (`$B/scripts/rev5_compare_rehearsal.py`, `$B/logs/rev5-rehearsal-compare.log`, exit 0, `VERDICT: agree`). The two directories hold the same file set:

- **209 `.npy` arrays are identical** in dtype, shape and value (per-slab forest pairs, per-forest totals and peaks, tree roots and per-tree arrays, per-slab tree pairs);
- **both tables are byte-identical**: the whole-simulation table (md5 `75ebf1572d513a020c0425bf1bd84338`) and the restricted one (md5 `1759395bc7110082e0fe20d5db72864e`);
- **all 50 label slabs agree** compared through the catalogue id (each halo's `MostBoundID` with its root's `MostBoundID`, sorted by `MostBoundID`, since the labels follow each dataset's row order);
- the ten JSON summaries and records agree key by key except where the layouts must differ: the identity record (`format_version` 3 against 2, `source_format`), the dataset directory and the file paths, the bytes read (version 2 slabs are not grouped by forest and its links are 4 B), the measured peak RSS, and the sizes of the files that embed the identity record.

Every logical result therefore agrees, including both tables, every promotion and chain count and the partition grid; the figures below are quoted from `$R3` (`$B/logs/rev5-rehearsal-v3-extract.txt`, `rev5-rehearsal-v3-extract-restricted.txt`) and hold for `$R2`.

**Census.** 22,580,924 halos over 50 slabs; the widest slab is snapshot 27 (621,360 halos). The largest forest, `ForestIndex` 237997 (`ForestID` 28435178), holds 350,075 halos and peaks at 11,775 at snapshot 17, which is the uncut partition's floor (`$R3/partition/summary.json`). There are 561,266 effective trees; the largest, root 28453397, holds 55,081 halos and peaks at 2,108 at snapshot 16. **Root correspondence passes**: 561,266 terminal roots against 561,266 index roots, 0 in each of the four mismatch counts, and **0 trees end before snapshot 49**. **Conservation passes** against the Stage A conversion's per-file parsed counts: `tree_0_0_0.dat` sums to 22,580,924 census halos against `parsed_count` 22,580,924, 0 files disagreeing, 0 unattributed halos (`$R3/trees/summary.json`).

**The decided table for the whole micro-Uchuu simulation** (`$R3/table/`, `$R3/cut/summary.json`). Snapshot 49 (scale factor 0.99951) holds 496,374 FoF groups over the 440,651 forests: 426,232 forests end in one group and are unchanged, and 14,419 split into 70,142 pieces (55,723 fresh, above `ForestID` 28,706,645), so the cut dataset has 496,374 forests. The table has 561,266 rows (10,102,809 B, md5 `75ebf1572d513a020c0425bf1bd84338`), its record 1,979,440 B. The largest piece by halos holds 80,644 (`ForestIndex` 239245's kept piece); the highest peak is 3,015 rows at snapshot 16 (`ForestIndex` 237997's kept piece, 79,124 halos). Its cost:

| | Whole simulation | Restricted to `ForestIndex` 237997 |
|---|---:|---:|
| Split forests; pieces (fresh) | 14,419; 70,142 (55,723) | 1; 2,309 (2,308) |
| Promoted halos (at the final snapshot) | 216,922 (0) | 11,699 (0) |
| Groups losing members | 102,910 | 2,484 |
| (group, piece) remnants; with ≥ 2 members | 198,716; 10,118 | 9,352; 806 |
| Progenitor chains changed (first progenitor changed) | 129 (0) | 19 (0) |
| Stored-chain check: descendants; mismatches | 3,437; 0 | 271; 0 |
| Halos with a new forest id; rank recomputed; `SourceHaloID` shifted | 3,338,472; 10,080,682; 22,580,924 | 270,951; 350,075; 10,080,520 |
| `sage16`/`halos-only`: seeds; `dependent_halos`; `upper_bound_halos` | 319,852; 1,028,661; 10,067,526 | 14,185; 39,519; 350,017 |
| Table md5 | `75ebf1572d513a020c0425bf1bd84338` | `1759395bc7110082e0fe20d5db72864e` |

The restricted table splits only `ForestIndex` 237997 into its 2,309 z = 0 groups; every other forest keeps its id, and its record and summaries say "restricted to the selected forests" (`$R3/table-restricted/`, `$R3/cut-restricted/`). These are Slice 15's two tables for its gate G3c. At micro-Uchuu scale every laptop class holds the unchunked job (621,360 rows, 0.68 GB at (1, 1)), so the rehearsal checks the method, not a class.

---

## Production: the version 2 Shin-Uchuu dataset

**Runs** (`$B/scripts/rev5_production.sh`; one log per step in `$B/logs/`). Run 2's `occupancy`, `trees` (with the production index files and conversion report) and `partition` (grid {1, 2, 4, 8} × {1, 2, 4, 8, 16, 32}) aggregates were reused after the checks below. Run 2's retired outputs were then moved aside, and `table` and `cut --reserve-gib 4` ran one after the other (`rev5-production-table.log`, `rev5-production-cut.log`), each exiting 0. A first `table` attempt was stopped by hand during its index load, before it wrote anything, to correct one summary string (`rev5-production-table-attempt1-stopped.log`). Both steps were then run a second time with `MallocLargeCache=0` (`rev5-production-table-nolargecache.log`, `rev5-production-cut-nolargecache.log`, `$B/scripts/rev5_production_nolargecache.sh`); see [Resources](#resources).

### Reuse of run 2's aggregates

`$B/scripts/rev5_reuse_checks.py` (`$B/logs/rev5-production-reuse-checks.log`, exit 0, `VERDICT: pass`) checked, read-only, before anything was moved or written:

1. **The summaries are complete**: `occupancy`, `trees` and `partition` each hold a `summary.json` with its keys; `trees` records root correspondence `pass`, 0 roots outside the last snapshot, conservation `pass`, `forest_mismatch_halos` 0, and 70 label slabs.
2. **The dataset identity matches**: the dataset's identity record equals `$P/identity.json` and every summary's `dataset` entry (`format_version` 2, 166,547,771 forests, 70 slabs, sidecar SHA-256 `f4807578…6336063`).
3. **The index files' md5 match**: `forests.list` `60dbf14a99ae9f23ca7c334474477a95` equals the production report's and run 2's; `locations.dat` `75442d7ae5b1f2465b485e60720e6df7` equals run 2's (the report records none).
4. **The source is unchanged since it was aggregated**: the latest-modified of the 71 dataset files is `forests.h5` at 2026-09-15T21:55:52Z, before run 2's census began (2026-10-08T13:39:39Z, `production-occupancy.log`) and before its earliest occupancy aggregate (13:39:44Z).

The aggregates were written at `5b7edec1`; this slice changes neither their format nor their meaning.

**Run 2's retired outputs moved aside** (`$B/logs/rev5-production-move-aside.log`): `$P/graph/` (25,636,764 KiB, 74 files) and `$P/cut/` (9,829,248 KiB, 12 files) were renamed to `$X/graph/` and `$X/cut/` on the same volume at 2026-10-09T01:32:54Z, before any new output was written; the file counts and sizes are equal before and after, and the volume's used space is unchanged. They remain charged to the LaCie's storage ledger until the owner deletes them (procedure step 12).

### The brief's measured table, reproduced

Every row of the brief's "Measured on 2026-10-07" table that the census measures, from run 2's summaries (`$B/logs/rev5-production-extract.txt`; `$P/occupancy/summary.json` unless named):

| Quantity | Brief | Census | Census key |
|---|---:|---:|---|
| Halos, all 70 slabs | 22,503,649,037 | 22,503,649,037 | `total_halos` |
| Widest slab | snapshot 34, 519,342,987 | snapshot 34, 519,342,987 | `widest_slab` |
| Super-forest (`ForestIndex` 0, `ForestID` 26551468179), total | 12,646,607,901 | 12,646,607,901 (56.20% of all halos); `max_halo_rank_in_forest` 12,646,607,900 | `largest_forest` |
| Super-forest peak | 333,663,215 at snapshot 31 | 333,663,215 at snapshot 31 (65.71% of its 507,798,774) | `largest_forest`; `per_snapshot[31]` |
| Super-forest trees | 104,845,278 | 104,845,278 | `$P/trees/summary.json` `largest_forest.trees` |
| Second-largest forest's maximum occupancy | 220,983 (`ForestIndex` 16386454) | 220,983 at snapshot 31, `ForestIndex` 16386454 (`ForestID` 26582413341), 8,312,566 in total | `second_largest_forest` and `second_highest_peak_forest`: the same forest |
| Forests whose occupancy ever exceeds 100,000 / 250,000 / 1,000,000 | 11 / 1 / 1 | 11 / 1 / 1 | `forests_exceeding` |
| Snapshot 34: forests present, top-10 share, top-100 share | 68,294,028; 62.10%; 62.86% | 68,294,028; 322,493,271 halos = 62.10%; 326,479,638 = 62.86% | `widest_slab` |

**Every figure reproduces to the number; there is no discrepancy.** The brief's "second-largest forest" is unambiguous here: the forest second by total is also second by peak. The super-forest holds 33.28% of snapshot 69 (104,845,278 of 315,004,242) and 91.00% of snapshot 0 (1,693,311 of 1,860,842); the twelve highest forest peaks, eleven above 100,000, are in `$B/logs/rev5-production-facts.txt`.

### Root correspondence and conservation

**Root correspondence passes** (`$P/trees/summary.json` `root_correspondence`): 315,004,242 terminal roots against 315,004,242 `forests.list` roots, with **0** roots not in `forests.list`, 0 listed roots without a terminal halo, **0 roots outside snapshot 69**, and 0 roots whose forest differs; `forest_mismatch_halos` is 0. `table` checked again that no tree ends before the final snapshot (0 of 315,004,242) and that the index files describe the census (`$P/table/summary.json` `index_check`: 0 roots in another forest). **Conservation passes** (`conservation`): the 2,744 files of `locations.dat` match the report's 2,744; 0 disagree and 0 halos are unattributed; the per-file sums total 22,503,649,037 against the report's 22,503,649,037 parsed rows. Both checks are necessary for the effective trees to be the catalogue's `#tree` blocks, **not sufficient**; nothing below depends on more than the root ids the table is keyed by.

### The largest individual tree

Root 26728264386 (root ordinal 170,541,159, its root at snapshot 69) in the super-forest holds **88,241,165 halos** and peaks at **2,504,273 rows at snapshot 31** (`$P/trees/summary.json` `largest_tree`). No whole-tree cut goes below 2,504,273 rows in a slab: 2.75 GB at 1,100 B per halo.

### The uncut partition

The driver's weights are snapshot 34's per-forest rows, the super-forest holding 321,253,424 of them (`$P/partition/summary.json` `weights`). The floor is **333,663,215 rows at snapshot 31**, the super-forest's peak. At (1, 1) the widest range is the whole of snapshot 34, 519,342,987 rows; at every other point of run 2's grid the widest range is the floor (`$P/partition/summary.json` `grid`). The uncut dataset therefore reaches no laptop class at any task or chunk count: 367 GB at 1,100 B per halo.

### The decided table

`$P/table/` (`$P/table/summary.json`, `record.json`, `forests.list`; `$B/logs/rev5-production-table.log`). Snapshot 69 (scale factor 0.99998) holds **244,953,607 FoF groups** among its 315,004,242 halos. Every central reference was validated, and for every split forest the piece count equals its z = 0 centrals:

- **158,494,209 forests end in one group** and are unchanged;
- **8,053,562 forests are split**, into 86,459,398 pieces holding 17,165,642,448 halos; 78,405,836 pieces are fresh, with ids from 26,877,727,958 (above the catalogue's largest, 26,877,727,957);
- the **super-forest** ends in **50,805,833 groups**; the **other 8,053,561 split forests** hold **27,600,004 extra groups** and **4,519,034,547 halos**; the next most divided forest is the second-largest (`ForestIndex` 16386454, 32,410 groups);
- the cut dataset has **244,953,607 forests**, one per z = 0 group.

The total of 244,953,607 groups, the super-forest's 50,805,833, and the other split forests' 8,053,561, 27,600,004 and 4,519,034,547 reproduce PM's informal counts exactly, computed independently by the census (`$P/table/summary.json` `groups`). The table has 315,004,242 rows (7,560,101,829 B, md5 **`6e9f2fb03b26a36ce51c5392e7bcf164`**); its record lists the 86,459,398 pieces of the split forests (2,954,410,424 B, md5 `432b157965cebf9420993e62d074377d`, `$B/logs/rev5-production-run1-summaries/md5.txt`), and the assignment's SHA-256 is `9cc47fb6a4dc087f9c2de98cdb5d258a76062e611055cb361dc96cbb33766227`, the same in `$P/cut/summary.json`. The table's invariants were checked on write: 315,004,242 trees, 244,953,607 pieces, 8,053,562 cut forests, 78,405,836 fresh pieces.

**Its largest pieces** (`pieces.largest`, `pieces.highest_peak`; all in the super-forest, all peaking at snapshot 31):

| Piece id | Halos | Trees | Peak rows (snapshot 31) | At 1,100 B per halo |
|---|---:|---:|---:|---:|
| 26551468179 (keeps the super-forest's id) | 126,380,326 | 558,306 | **3,415,844** | 3.76 GB |
| 26877727958 | 71,150,152 | 348,722 | 1,851,104 | 2.04 GB |
| 26877727959 | 55,080,340 | 327,333 | 1,452,036 | 1.60 GB |
| 26877727960 | 51,040,732 | 314,791 | 1,341,795 | 1.48 GB |
| 26877727961 | 49,139,058 | 267,012 | 1,323,859 | 1.46 GB |

These are the piece totals and peaks PM's estimate sampled over snapshots 24 to 40; over every slab and every piece, no piece peaks higher than 3,415,844 rows. 81,442,826 pieces are single trees. The second-largest forest's kept piece holds 752,524 halos and peaks at 19,929.

### The decided table's cost

From one pass over the split forests' rows of every slab (`$P/cut/summary.json`; per-slab rows in `$B/logs/rev5-production-extract.txt`):

| Measure | Production |
|---|---:|
| Promoted halos, all slabs | **481,296,108** |
| Promoted at the final snapshot (checked) | **0** |
| Most promoted in one slab | 25,198,213 (snapshot 63) |
| Groups losing members | 155,039,609 |
| (group, piece) remnants; with ≥ 2 members | 425,265,272; 16,383,078 |
| Progenitor chains changed; first progenitor changed | **254,554**; 1,891 |
| Stored-chain check: descendants checked; mismatches | 3,410,566; **0** |
| Halos with a new forest id | 14,749,355,436 |
| Halos whose `HaloRankInForest` is recomputed | 17,165,642,448 |
| Halos whose `SourceHaloID` prefix moves | 22,503,649,037 (every halo: the super-forest is `ForestIndex` 0) |
| `sage16`/`halos-only` seeds | 636,367,687 |
| `dependent_halos` | **2,445,611,151** |
| `upper_bound_halos` | **17,163,320,870** |

The stored-chain check's 0 mismatches mean the recomputed chains reproduce the dataset's `NextProgenitor` links for every affected descendant, so the progenitor-order prediction stands (exact for this Consistent-Trees-derived dataset). The promotions grow with time, from 19 at snapshot 1 to their maximum at snapshot 63, and fall to 0 at snapshot 69; the pass read 17,165,642,448 split-forest rows, every halo of the split forests.

### Chunked memory with the pieces installed

The partition with the pieces installed, weighted as the driver weighs it by the widest slab (snapshot 34), for every grid point (`$P/cut/summary.json` `partition.grid`; `$B/logs/rev5-production-facts.txt`). Rows are the widest chunk; GB at 1,100 B per halo:

| ntask | nchunk | Process: widest rows (snapshot) | Process GB | Job rows | Job GB |
|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 519,342,987 (34) | 571.3 | 519,342,987 | 571.3 |
| 1 | 16 | 57,143,838 (67) | 62.9 | 57,143,838 | 62.9 |
| 1 | 32 | 41,209,248 (67) | 45.3 | 41,209,248 | 45.3 |
| 1 | 64 | 33,188,253 (67) | 36.5 | 33,188,253 | 36.5 |
| 1 | 128 | 29,113,469 (67) | 32.0 | 29,113,469 | 32.0 |
| 2 | 128 | 27,042,619 (67) | 29.7 | 31,262,440 | 34.4 |
| 4 | 128 | 26,019,137 (67) | 28.6 | 33,699,603 | 37.1 |
| 8 | 128 | 25,508,104 (67) | 28.1 | 35,082,325 | 38.6 |

The smallest job figure on the grid is **29,113,469 rows, 32.0 GB, at one task and 128 chunks**. Per class at the 4 GiB reserve (`partition.laptop_classes`):

| Class | Usable (class less 4 GiB) | Smallest fitting grid point | Job |
|---|---:|---|---:|
| 16 GiB | 12,884,901,888 B | **none** | — |
| 32 GiB | 30,064,771,072 B | **none** | — |
| 64 GiB | 64,424,509,440 B | ntask 1, nchunk 16 | 57,143,838 rows, 62.86 GB |

**Why the job figure stays far above the largest piece's 3,415,844 rows.** At every point with 16 or more chunks the widest chunk is the last range of the last task, at snapshot 67. The driver packs the forests by their rows in the widest slab, snapshot 34, and F6 enumerates the fresh pieces after every original forest in descending size. At one task and 128 chunks the last chunk is `ForestIndex` 215,847,007 to 244,953,606: the 29,106,600 smallest fresh pieces. They hold few rows at snapshot 34, so the packing gives them a single chunk: no chunk holds more than 4,085,261 rows there (`widest_rows_per_snapshot[34]`). But every one of them reaches z = 0, and the same last chunk (range 127) holds 29,113,469 rows at snapshot 67, the point's `widest`. This is a measured property of the decided table under the driver's present partition and F6's present enumeration. On this grid a 64 GiB class is reached at a 4 GiB reserve; a 32 GiB class is reached only with a reserve below 2,334,922,468 B (34,359,738,368 B less the 32,024,815,900 B job at (1, 128)); a 16 GiB class is not reached.

### Resources

Wall-clock and peak RSS from `/usr/bin/time -l` in each step's log; the run 2 steps are reused, not rerun.

| Step | Wall-clock | Peak RSS (default allocator) | Peak RSS with `MallocLargeCache=0` | Written (sum of file sizes) |
|---|---:|---:|---:|---|
| `occupancy` (run 2, `production-occupancy.log`) | 1,006 s | 32.7 GB | — | `occupancy/` 64,619,992,405 B |
| `trees` (run 2, `production-trees.log`) | 2,768 s | 41.7 GB | — | `trees/` 190,327,307,815 B |
| `partition` (run 2, `production-partition.log`) | 421 s | 27.4 GB | — | `partition/` 1,363,597 B |
| `table` (`rev5-production-table.log`; rerun `-nolargecache`) | 1,527 s; 1,089 s | 66.7 GB | **57.3 GB** | `table/` 10,514,524,769 B (table 7,560,101,829; record 2,954,410,424) |
| `cut` (`rev5-production-cut.log`; rerun `-nolargecache`) | 5,839 s; 5,075 s | 109.4 GB | **33.7 GB** | `cut/` 191,193 B |

(Sizes from `$B/logs/rev5-production-facts.txt`.) **The default-allocator RSS includes freed memory.** macOS's allocator keeps freed large blocks resident in its large-allocation cache, and `ru_maxrss` counts them. Run in isolation on production (`$B/scripts/rev5_measure_cut_aggregates.py`), the cut's aggregates phase reached 59.7 GB of RSS with the cache (`$B/logs/rev5-production-cut-aggregates-memory.log`) and 30.25 GB without it (`rev5-production-cut-aggregates-memory-nolargecache.log`), with 16.6 GB of live arrays after the table's construction. With the cache disabled, the phases' peaks are (summaries' `memory`): `table` 17.8 GB after loading the index files, 32.1 GB through the pieces and their peaks, 57.3 GB while the table is checked and written; `cut` 27.1 GB through its pieces, 30.3 GB through the aggregates, 33.7 GB through the pass. These follow the modules' stated formulas. The table's check (about 96 B per catalogue tree, 30.2 GB) sits on top of the index files' roots and ids, the table's ids and the trees' totals (about 32 B per tree, 10.1 GB) and the pieces, census roots and per-forest arrays still held (about 15 GB). The pass's widest slab costs 12 B per row (6.2 GB at snapshot 34) beside the 16.6 GB held. **The rerun is deterministic**: its `forests.list` and `record.json` have the md5s of the first run, and both summaries are identical except the measured memory (and the cut summary's own size), so the outputs now in `$P` are the rerun's.

**Disk and footprint.** `$P` holds 259,242,356 KiB at rest (`du -sk`) and `$X` 35,466,012 KiB, together **0.30 TB** (301.8 GB). The LaCie footprint peaked during the rerun's rewrite of the table at 260,336,064 KiB in `$P` with `$X` unchanged (0.303 TB together) and 2,789,593,412 KiB used on the volume (`$B/logs/rev5-production-footprint-nolargecache.log`, sampled every 2 min); 5,024,478,164 KiB (5.14 TB) were free afterwards.

**The procedure's free-space precondition, rechecked.** Procedure step 1 budgets "the census aggregates (the measured figure from Slice 9; run 2 measured about 0.29 TB in all, retired outputs included)". The measured figure is **0.30 TB** at rest, retired outputs included (0.303 TB at the table-rewrite peak); the ledger entry is 0.01 TB above run 2's. The decided table and its record (10.5 GB) are kept with the conversion provenance (the Final-State Inventory); the rest is deleted by the owner after the last step that reads it.

---

## Predicted differences per model (decision 7)

These are predictions from input topology, per the plan's decision 7; none is a measured galaxy difference. The galaxy-by-galaxy differences of the cut dataset are measured later (Slice 15 at subset scale, procedure step 10 in production).

- **Unchanged forests.** The 158,494,209 forests ending in one z = 0 group keep their id, their `ForestIndex`, their topology and payload, and their `UniqueGalaxyID`. Decision 7 limits the byte-identity claim to `sage16` and `halos-only`: every galaxy of such a forest, measured against the uncut dataset. For SHAM, the global ranking below can reach these forests too.
- **`sage16` and `halos-only` inside the split forests.** A promoted halo becomes a central, so an earlier satellite phase is removed. That changes what a galaxy inherits along its history: infall, ejected-gas and ICS consolidation, stripping, reincorporation, the virial-mass source, progenitor order and inheritance, mergers, disruption and orphans. The affected histories are bracketed by `dependent_halos` (2,445,611,151: the seeds and every halo on a seed's descendant path) and `upper_bound_halos` (17,163,320,870: every halo of a piece holding a seed). The logical descendant relation and every z = 0 central/member relation of the input are preserved, but the z = 0 galaxy population and Types are measured, not claimed.
- **HOD and SHAM**, as predicted mechanisms rather than counts (`$P/cut/summary.json` `predicted_effects.hod_sham`):
  - *identity recomputation*: `ForestIndex`, `HaloRankInForest` and `UniqueGalaxyID` are recomputed for every halo of a split forest (17,165,642,448 halos), and HOD's draws and SHAM's tie-breaks key on them;
  - *host and centrality changes*: a promoted halo (481,296,108 over all slabs, none at z = 0) becomes a central, so HOD populates it as a host and SHAM ranks it as a central, and its former group loses it;
  - *SHAM's global ranking*: abundance matching ranks every halo of a snapshot together, so a changed centrality or tie order can move tied assignments outside the split forests.

  These are distinct from the measured subset differences of Slice 15. Neither model chunks, and no production run of either on the cut dataset is planned (decision 7).

---

## Code identity

Every `rev5-*` step log records `# git 40a448d5` (plan revision 5, this slice's starting commit) with the census paths modified or new, and the SHA-256 of each census source file. Every rehearsal step (the round quoted) and every production `table` and `cut` run (both rounds) recorded the same digests:

| File (under `convert/mimic-convert/`) | SHA-256 |
|---|---|
| `forest_census.py` | `f1fcb6679202f802d5a617c5829687cd5a7cad139c773f651d03d968080d8e78` |
| `census/aggregate.py` | `a865385db5b48e14539f50f388ce95a116427bd6c58fd0d54e89fa62fb90fb99` |
| `census/cut.py` | `3751412ab207982ae3273a59c9bd5c9b6b07ae77600dbbaab9cc703227484076` |
| `census/cut_table.py` | `24a9d2e13ef1a9c163e6ee51a9e241b8c6fad4475081bcf6e4d0c1c9a2deac43` |
| `census/occupancy.py` | `4e87b54257d5ba98d663375fd9c358293538bc1bb78192ae2cdf254902acea08` |
| `census/partition.py` | `95cd52696e0657cfd7079cef75a893ed0c08a1bf1b0df39008efca278235c686` |
| `census/table.py` | `59bbc28d5d86c06bc44948ede9d19df082d9963180e5d0ec67072c5f152e3b76` |
| `census/trees.py` | `06a0f36f6d0c22c6097c2d1d6e5432298bcde3458e829a828dcf6fa8237591d8` |

The same files, byte for byte, are kept in `$B/logs/rev5-code-as-run/`, and they are the files Slice 9 committed: the digests of the committed blobs equal these. `horizontal_dataset.py` and `source_index.py`, which this slice does not change, recorded `79f6dd02…` and `2bb1617d…`. The per-unit memory figures in `census/table.py`, `census/cut.py` and `census/cut_table.py` were calibrated with `tracemalloc` on the micro-Uchuu census (`$B/scripts/rev5_measure_memory.py`, `$B/logs/rev5-memory-calibration-v3.txt`).

---

## The owner's decision

The two slots below are the owner's. They are **empty** until the owner fills them; the owner then records them in the plan's decision 6.

### Decision slot 1: the laptop class and its usable budget

*(empty: the owner's to fill)*

### Decision slot 2: the stop condition of the brief's decision gate

*(empty: the owner's to fill)*
