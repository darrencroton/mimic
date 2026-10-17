# Mimic Shin-Uchuu Cut-Vertical Implementation Plan

**Purpose:** Execute the owner's brief [`shin-uchuu-cut-vertical-brief-2026-10-15.md`](shin-uchuu-cut-vertical-brief-2026-10-15.md): produce the cut Shin-Uchuu merger trees (the same decided cut as the production horizontal dataset) in a vertical, per-forest format that Mimic's existing vertical reader consumes, run `sage16` on it with the vertical driver on the Mac Studio, and prove that run identical, galaxy for galaxy, to the recorded cut horizontal `sage16` run. A secondary aim, the owner's direction of 2026-10-17 and kept in view throughout, is that the same files are the natural input of SAGE26 (`https://github.com/sage-home/SAGE26`), whose native tree format is the one chosen here; what SAGE26 still needs is written up as a brief for that repository, not built in this plan.

**Status:** Planning contract, revision 1 (2026-10-17). Not yet executed. Nine Mode B slices in three runs, with the owner-operated production procedure between runs 2 and 3.

**Planning baseline:** `8ca7549a` on `main` (v1.5 plus the panel closeout), clean tree. Every `path:line` anchor below was read at that baseline. PM binds a run to the SHA-256 of this file's bytes; recheck drift against the anchors before executing a slice, and never silently rebase a contract onto a changed interface.

**Owner:** [Development pathway, next focus](MIMIC-DEVELOPMENT-PATHWAY.md). Principles: [VISION](../VISION.md). Standard: [STYLE-GUIDE](../STYLE-GUIDE.md). Input contract: [`HORIZONTAL-HDF5-FORMAT.md`](../../convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md) (version 3). Evidence of record for the input dataset: [`MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md`](MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md) (Stage C) and [`MIMIC-SHIN-UCHUU-FOREST-CENSUS.md`](MIMIC-SHIN-UCHUU-FOREST-CENSUS.md).

---

## Decisions for the owner

Each row is a decision this plan takes as frozen unless the owner changes it before run 1. The recommendation is the plan's text below; an alternative chosen here changes the slices named.

| # | Decision | Recommendation | Alternatives and their cost |
|---|---|---|---|
| 1 | **Target vertical format.** The brief leaned to Consistent-Trees HDF5 (Route A) with Route B (a reader over the slabs) to weigh. | **L-Halo binary (`lhalo_binary`).** Every candidate needs the converter to write all five links as forest-local indices: the `consistent_trees_hdf5` reader does no topology reconstruction either (`src/io/vertical/read_ctrees_hdf5.c:9-10`), so the mapping work is identical. L-Halo binary is then the exact carrier: a headerless file of `int` counts followed by the package's `struct RawHalo` records (`src/io/vertical/binary.c:77-131`, `:205-217`), one allocation and one read per forest, values passed through untouched (the ctrees-HDF5 reader re-derives `Spin` and `Len` on read, `src/io/vertical/read_ctrees_ascii.c:99-125`, which risks bit-parity), free choice of file count, resume per file with `--skip`, and it is the format SAGE26 reads natively. Route B (a vertical reader over the forest-blocked slabs) is rejected: it is new C in the I/O layer, 71 interleaved seek-bound streams per forest on a spinning RAID (14 ms per random 32 KB read measured), and it leaves SAGE26 with nothing. | Consistent-Trees HDF5: Slices 1 to 4 change their output format; 19 datasets of exactly 8 bytes per forest (`read_ctrees_hdf5.c:80-144`), `ForestInfo` re-read per chunk, `Spin`/`Len` re-derived on read; SAGE26 would need an identity-multiplier patch and opens 21 datasets per forest. `lhalo_hdf5`: one group per forest, about 3.4 billion datasets; not viable. |
| 2 | **The record.** Mimic's bitwise identity with the horizontal run needs the horizontal dataset's own field types and units (`M_Crit200` as float32 **Msun/h**, `Spin` as stored, `Len` as stored); SAGE26's 104-byte record expects masses as float32 in 1e10 Msun/h (clone `src/core_simulation.h:17-46`). One file cannot satisfy both bit for bit. | **The Mimic-native record:** the fourteen fields of `simulations/shin-uchuu-ascii/halo_properties.yaml:33-146` in that order with `int` links, native units, 88 bytes with the compiler's padding (`MostBoundID` at offset 72, trailing pad to 88). Mimic is the primary objective and the acceptance is bitwise. SAGE26 then needs a small reader for this record (a declared field table plus a mass scale), written up in Slice 8 for the SAGE26 repository. | The SAGE 19-field layout in SAGE units: SAGE26 reads it unchanged, but `Mvir` becomes float32(M × 1e-10), a relative change up to 6e-8 that `sage16` amplifies chaotically, so the acceptance would no longer be bitwise and would need a tolerance the project does not grant. A second dataset in that layout later is a converter option, not a plan change; the LaCie cannot hold both beside the input (about 2.0 and 2.3 TB against 4.35 TB free less the output). |
| 3 | **Package name.** | **`simulations/shin-uchuu-lhalo/`**, naming the format like `micro-uchuu-hdf5` and `micro-uchuu-ascii`; `shin-uchuu` is the horizontal production package and `shin-uchuu-ascii` the vertical subset package. | `shin-uchuu-cut-vertical` or `shin-uchuu-vertical` name the layout rather than the format, against the sibling convention. |
| 4 | **File cut.** | **Files are contiguous `ForestIndex` ranges in file-number order; a new file starts when adding the next forest would exceed 100,000,000 halos or 1,000,000 forests, and a forest larger than the halo cap occupies its file alone** (file 0 holds the 126,380,326-halo super-forest alone). Both caps are converter options with these defaults; the hard refusals are a file above 2,147,483,647 halos or forests (`int` header counts, `binary.c:62-65,101-102`, and the `int` running sum at `:196-201`). Expected about 260 to 320 files. The forest cap keeps SAGE26's own galaxy-index scheme valid (`FileNr_Mulfac` 1e15 over `ForestNr_Mulfac` 1e9, clone `src/core_save.c:228-310`); the halo cap balances Mimic's round-robin file striding under MPI (`src/core/vertical_driver.c:455`). | Fewer, larger files (up to 2^31 halos, at least 11) halve the file count but unbalance MPI and break SAGE26's indices. |
| 5 | **The production run and its comparison.** | **One `sage16` run with the reference run's output snapshots [69, 52, 43, 40, 20, 10, 5, 2, 1, 0], `SubSteps: 10` and the committed `sage16` pipeline, compared with the recorded cut horizontal run in the comparator's default mode (pairing by `UniqueGalaxyID`, every field byte for byte).** The committed run files keep the package convention [69, 48, 44, 37, 32, 30, 27, 24]; the procedure's run file is a scratch variant with the reference's list, as the v3 procedure did. The default mode measured 297 GB and 7,395 s at this scale (`MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md:591`). | A `halos-only` production leg as well: cheap to run (hours) but it needs a horizontal `halos-only` production run to compare with (none exists) and another 1 TB of output; `halos-only` identity is proved at subset scale by the gate instead. |
| 6 | **Serial or MPI for the production run.** | **Serial first, decided at the procedure from the subset-measured rate.** The whole set is about 9 to 16 h serial at the measured 0.46 to 0.75 million halos per second, in line with the 12.3 h horizontal run; the super-forest alone is minutes. Under MPI each rank's peak is the largest forest it holds (166 to 205 GB for file 0, allow 250 GB on macOS) and the five largest pieces sum to about 574 GB if they ever coincide, so a multi-rank run needs the rank count chosen against 512 GB. | `mpirun` at a rank count the ledger allows, if the measured serial projection exceeds 24 h; output identity does not depend on the task count (`UniqueGalaxyID` from the file prefix sums, `src/core/vertical_driver.c:250-271`). |
| 7 | **Where the data goes.** | **The vertical dataset and the vertical run output on the LaCie** (4.35 TB free: about 1.98 TB dataset plus 0.54 TB output, after about 0.6 TB of rehearsal scratch, leaves about 1.2 TB); the cut horizontal `sage16` reference stays on Internal (454 GB free, which cannot host a second output). Nothing is deleted; the Stage R subsets and the subset ASCII have no NT copy and are not touched. | Writing the dataset to the `Scratch` volume (2.2 TB free on USB 5 Gb/s) avoids read-write contention on the LaCie during the conversion but needs a 2 TB move afterwards. |
| 8 | **Core is not changed.** The vertical driver seeds its output buffer at five times the forest's halos and zero-fills it (`src/io/vertical/interface.c:163-179`, `MAXHALOFAC` 5 at `src/include/constants.h:25`), 116 GB of the super-forest's about 166 to 205 GB. | **No core change in this plan.** The host has 512 GB; the figure is measured in the procedure and recorded. Sizing the seed from a measured output ratio (as the horizontal driver does) would cut the super-forest to about 70 GB and is recorded as a candidate for the performance step, not taken here. | Changing the seed now widens the plan into `src/core/` and every vertical package's memory profile. |
| 9 | **The SAGE26 deliverable.** | **A brief, `docs/dev/SAGE26-SHIN-UCHUU-LHALO-READER-BRIEF.md` (Slice 8), for implementation in the SAGE26 repository afterwards:** the record, the reader to add, the identity join between SAGE26's `GalaxyIndex` and Mimic's `UniqueGalaxyID`, SAGE26's own memory and index limits at this scale, and the comparison that is scientifically meaningful (SAGE26 against `sage16` is not an identity). | Implementing the SAGE26 reader inside this plan: out of scope (another repository). |

---

## Outcome and Limits

**Delivered when the plan closes:**

- `convert/mimic-convert/write_vertical_trees.py` with its package `convert/mimic-convert/vertical/`: a resumable, bounded-memory converter from a forest-blocked version 3 horizontal dataset to L-Halo binary files in the record a named simulation package declares, with a file plan, a conversion manifest, write-time verification and a self-test suite under `make tests-converter`.
- `simulations/shin-uchuu-lhalo/`: the vertical package for the cut dataset, with run files for `halos-only` and `sage16`, a committed test-sized fixture the generic tiers run on, a record-layout test, and a subset-scale cross-format identity gate against `shin-uchuu` over the Stage R cut subset.
- The production vertical dataset on the LaCie, its manifest, and the `sage16` vertical run proved identical to the cut horizontal run in every field over the reference's ten output snapshots, with measured wall time and peak RSS for the conversion, the super-forest alone and the whole run; recorded in a new standing record, `docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-ACCEPTANCE.md`.
- The guides, converter manual, skills, package README, changelog and pathway describing the new tool and package as live, user-facing and release-ready; the SAGE26 brief.

**Limits, stated so they are not overclaimed:** identity is proved for `sage16` at production scale and for `halos-only` and `sage16` at subset scale; no `hod` or `sham` claim is made (the vertical created-record identity space does not fit int64 for 244,953,607 forests, a log-only verdict, `src/core/vertical_driver.c:201-225`); the SAGE26 side is a written brief, not a run; measured figures are single runs on one macOS host.

---

## Frozen Facts and Evidence

The numbers this plan designs against, each with its source. A slice that finds one wrong stops rather than silently rebases.

**The input (the cut version 3 dataset, `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-c/cut/dataset`, the `shin-uchuu` package's `snapshots`).** 71 files, 3,156,784,461,544 B, 140.28 B per halo; 22,503,649,037 halos in 70 snapshot slabs; 244,953,607 forests, dense `ForestIndex` in [0, 244,953,607); `links_adjacent 1`, 0 gapped links, 0 `Len == 0` halos; widest slab 34 with 519,342,987 halos; the largest forest is `ForestIndex` 0 (`ForestID` 26,551,468,179) with 126,380,326 halos over 558,306 trees, peaking at 3,415,844 rows at snapshot 31; `max_halo_rank_in_forest` 126,380,325; identity multiplier 2 × 10¹⁰ (`simulations/shin-uchuu/README.md` "How the production dataset was made"; `MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md` Stage C; the dataset's headers). Forest sizes: 1,254 forests hold at least 1 million halos, 70 at least 10 million, one at least 100 million; the median forest holds 36 halos and 97.15% of forests have one halo at z = 0 (derived in planning from slab 69's `ForestIndex` and `SourceHaloID`, 17 s). The five largest pieces hold 126,380,326; 71,150,152; 55,080,340; 51,040,732 and 49,139,058 halos (`MIMIC-SHIN-UCHUU-FOREST-CENSUS.md` "Its largest pieces").

**Per-slab layout.** Twenty uncompressed datasets chunked at 65,536 rows, each column one contiguous run of chunks: the five int64 links, three int32 target-snapshot columns, int64 `SourceHaloID`, `ForestIndex`, `HaloRankInForest`, `MostBoundID`, int32 `SnapNum` and `Len`, float32 `M_Crit200`, `VelDisp`, `Vmax`, and [N,3] float32 `Pos`, `Vel`, `Spin` (`HORIZONTAL-HDF5-FORMAT.md:314-372`). Rows within a slab ascend in `SourceHaloID`, the halo's 1-based position in (`ForestIndex`, `HaloRankInForest`) order, so every slab is grouped by forest in ascending `ForestIndex` (`:419`, `:443`). Links are global rows into the slab the companion target-snapshot column names, −1 for none; `FirstHaloInFOFgroup` is never −1 and FoF links stay in the owner's slab (`:318-334`, `:397-409`).

**The within-forest order is the vertical reader's order.** `HaloRankInForest` is "the halo's index in reference vertical-driver order" (`HORIZONTAL-HDF5-FORMAT.md:97`, `:131`): descending snapshot first, then FoF-group runs with the central first, then the reference progenitor-chain insertion order (`convert/mimic-convert/rewriter/__init__.py:34-35`). Verified in planning on seven forests of the Stage R cut subset, including its largest (1,250,835 halos in all 70 slabs): concatenating a forest's slab blocks from snapshot 69 down to 0 gives ranks exactly 0..n−1, and all 9.87 million non-null links of the five kinds resolve to rows inside the same forest's block of the target slab. The subset gate already proves that the vertical ASCII reader's `halonr` equals this rank: its `UniqueGalaxyID`s matched the horizontal run's over the 406,668,896-halo uncut subset's 92,413,007 `halos-only` and 60,016,178 `sage16` galaxies at eight output snapshots (`MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md:300-316`). The production cut dataset's every tree reaches the final snapshot (the census's `table` and `cut` refuse a census holding one that ends earlier); a fixture or another dataset need not, and nothing in this plan relies on it.

**Identity.** Vertical: `UniqueGalaxyID = halonr + multiplier × (GlobalForestOffset + unit + 1)` with `halonr` the halo's index in the loaded forest array and the offset the prefix sum of tree counts over present files in file-number order (`src/core/build_model.c:259-275`, `src/include/galaxy_id.h:35-56`, `src/core/vertical_driver.c:250-271`). Horizontal: the same encoder over `ForestIndex` and `HaloRankInForest` (`src/core/horizontal_driver.c:652-666`). So the converter reproduces the horizontal ids exactly by writing forests in ascending `ForestIndex` with no file gaps and each forest's records in rank order. At 2 × 10¹⁰ the largest id is about 4.9 × 10¹⁸, under `INT64_MAX`, and the forest ceiling is 461,168,600 (`galaxy_id.h:46-50`, `vertical_driver.c:266-270`).

**The L-Halo binary reader.** File `<simulation_dir>/<tree_name>.<filenr>` for `filenr` in `first_file..last_file` (`binary.c:88-89`; `file_extension ""` at `:231`); `int ntrees`, `int totNHalos`, `int counts[ntrees]`, then every forest's `struct RawHalo` records back to back (`:101-125`, `:205-217`); `totNHalos` is logged, not checked; a missing file is skipped and consumes no forest ids (`vertical_driver.c:254-257`), so the production run must present every file. `struct RawHalo` is generated from the package's `halo_properties.yaml` in declaration order with C types `int`, `float`, `float[3]`, `long long` (`scripts/generate_properties.py:58-100`, `:1101-1120`); its on-disk layout is the compiler's, padding included.

**The vertical driver at the super-forest.** Index types are `int` within a unit (ceiling 2,147,483,647, 17 times the super-forest); `build_halo_tree` recursion depth is bounded by the snapshot count, about 70, not by chain length (`src/core/build_model.c:92-175`, guard at `:103-107`, `MaxTreeDepth` 500). Modelled resident memory for 126,380,326 halos: output buffer seed 116.3 GB, FoF seed 11.6 GB, `RawHalo` 11.1 GB, `HaloAux` 3.0 GB, galaxy pool about 23.6 GB, **about 166 GB**, or **about 205 GB** by the one measured vertical `sage16` run on real data (13.514 GB peak for an 8,312,566-halo largest forest, 1,626 B per halo, the 2026-08-26 rehearsal); allow up to about 250 GB on macOS for the allocator cache (`MallocLargeCache=0` for measurement). The brief's 60 to 150 GB estimate was low. Throughput: 461,000 halos per second vertical on mini-Millennium, 507,000 wall (755,000 CPU) on the horizontal cut production run; so about 9 to 16 h serial for the whole set and 4 to 20 minutes for the super-forest, assuming cost per halo does not grow with forest size, which has not been measured above 8.3 million halos.

**Machine.** Mac Studio, Apple M3 Ultra, 32 cores (24 performance), 512 GiB RAM, 1 GiB swap (a job that outgrows RAM fails rather than pages), macOS 27.0.1. LaCie "2big RAID" on USB 10 Gb/s, rotational: 4.35 TB free of 8.0; sequential reads 380 to 450 MB/s; 60 interleaved streams at 32 MB reads 348 MB/s, at 8 MB 174, at 1 MB 114; a random 32 KB read 14 ms; the previous LaCie-to-LaCie rewrite ran at about 115 MB/s combined. Internal (Apple SSD) 454 GB free. `Scratch` 2.2 TB free on USB 5 Gb/s. Python 3.14.6 with h5py 3.15.1, HDF5 1.14.6, numpy 2.3.4; `h5ls`, `h5dump`, `h5repack`, `mpicc`, `mpirun` present; `make info` shows MPI disabled by default. The llama router (`com.dcroton.llama-router`, `KeepAlive`, can load up to 331 GB) runs; only the owner can stop it (`sudo launchctl bootout system/com.dcroton.llama-router`; restart with `sudo launchctl bootstrap system /Library/LaunchDaemons/com.dcroton.llama-router.plist`). NT (`nt.swin.edu.au:/fred/oz214/dcroton/shin-uchuu/`) is reachable non-interactively and holds the cut dataset, its provenance and the cut `sage16` output; transfers are the owner's.

**The reference.** The cut horizontal `sage16` run, `/Volumes/Internal/results/mimic/sage16-shin-uchuu-v3-cut/` (17 files, 543 GB; NT copy verified), made by the frozen binary at `b4903283` with the version 2 run's output snapshots [69, 52, 43, 40, 20, 10, 5, 2, 1, 0], one task and 128 chunks: 12.33 h, 6,170,116,096 B peak RSS, 313,753,033 galaxies at z = 0 of which 244,953,607 centrals (`MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md:690-696`; the z = 0 science table in `simulations/shin-uchuu/README.md`). Its `metadata/` records its build, modules, parameters and timestep scheme; the procedure reconciles the vertical run's configuration against it, as the v3 procedure did.

**The test bed.** The Stage R cut subset, `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-r/cut/dataset`: a version 3 cut dataset in the production layout, 57.4 GB, 406,668,896 halos in 7,206,195 forests (from 6,011,205), largest forest 1,250,835 halos (`ForestIndex` 4,310,475), widest slab 44 with 9,006,294 halos, 5,339,202 promotions, identity multiplier 2 × 10¹⁰; its horizontal `sage16` run under the committed `sage16_shin-uchuu.yaml` took 559.66 s at 9.712 GB serial and wrote 60,033,537 galaxies over eight output snapshots, `halos-only` 92,429,377 (`MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md:349-358`). The subset ASCII behind `shin-uchuu-ascii` is the **uncut** topology and is not this plan's reference for the cut.

**The comparator and the halo-level instrument.** `scripts/compare_cross_format_identity.py` default mode: no duplicate ids, same schema and snapshot set, identical id sets, every field byte for byte; exit 0 identical, 1 differ, 2 input error, 3 comparator failure; about 57 to 70 B per row plus both runs' snapshot records. `convert/mimic-convert/tests/run_generalisation_acceptance.py` with `tests/unit/tools/dump_ctrees_topology.c --source-payload` dumps any vertical reader's view of a package (identity as (forest, rank), links as target ranks with target snapshots, payload as exact integers and binary32 bit patterns; it "works with every vertical reader") and `compare --dataset <v3 dataset> --source-format lhalo_binary --dump <dump>` proves a version 3 dataset says what that vertical reading says, halo by halo, link by link and bit by bit, in bounded memory (`run_generalisation_acceptance.py:1-80`, `:2703-2706`; `dump_ctrees_topology.c:1-40`; built by `tests/unit/tools/build_topology_dump.sh` for a `MODEL`/`SIMULATION` pair). `compare` requires the dump's reader and the dataset header's `source_format` to be one and the same format (`:1443-1447`, `:1456-1459`), so a version 3 dataset of the Consistent-Trees ASCII route compared with an `lhalo_binary` dump of its own vertical conversion needs an explicit option, which Slice 1 adds; the comparison itself keys on (forest, rank), which the dump's file-prefix forest number and `halonr` supply. The dump carries every one of the fourteen record fields (the five links with their target snapshots, `SnapNum`, `Len`, `MostBoundID` and the twelve float components, `dump_ctrees_topology.c:148-158`; `run_generalisation_acceptance.py:185-197`), so `compare-extras` and its packed L-Halo record derivation (`:1641-1653`, used only there) play no part in this plan.

**SAGE26** (clone `e1174dda`, 2026-10-08, read in planning; nothing built or run). `TreeType` values `lhalo_binary`, `lhalo_hdf5`, `consistent_trees_ascii`, `consistent_trees_hdf5`, `genesis_hdf5`, `gadget4_hdf5` (`src/core_allvars.h:33-47`). Its `lhalo_binary` reader expects exactly Mimic's header (`src/io/read_tree_lhalo_binary.c:146-152`, `:270-296`) and the 104-byte SAGE record (`src/core_simulation.h:17-46`: five `int` links, `int Len`, `float M_Mean200, Mvir, M_TopHat, Pos[3], Vel[3], VelDisp, Vmax, Spin[3]`, `long long MostBoundID`, `int SnapNum, FileNr, SubhaloIndex`, `float SubHalfMass`; masses 1e10 Msun/h). `GalaxyIndex = GalaxyNr + forest_in_file × 1e9 + file × 1e15` (`src/core_save.c:228-310`), so SAGE26 ids never equal Mimic's; SAGE26's output carries `SAGETreeIndex` (the forest within its file, so Mimic's forest number is the file plan's prefix plus it) and `SAGEHaloIndex` (the galaxy's current host halo's within-forest index, overwritten as the galaxy is inherited, so a correspondence between host halos rather than the persistent birth-halo component of Mimic's `UniqueGalaxyID`); a galaxy-identity join needs a birth or lineage mapping stated on both sides. SAGE26 allocates two `GALAXY` arrays sized to the forest's halo count (`src/sage.c:331-336`), about 2 KB per galaxy by struct reading, so the super-forest is a SAGE26 memory question of its own. Its forest distribution gives each rank a contiguous global forest range weighted by halo count by default. Its `lhalo_hdf5` layout is Illustris-style and incompatible with Mimic's; its `consistent_trees_hdf5` reader shares Mimic's lineage but requires 21 eight-byte datasets and `TotNforests`.

---

## Design

### The converter: `write_vertical_trees.py`

One streaming pass per output file, parallel across files. Nothing is sorted and no scratch is written.

- **`plan`** (Slice 1): every forest's halo total from one bounded pass over every slab's `ForestIndex` and `SourceHaloID` (16 B per halo, about 360 GB at production, tens of minutes): within a slab a forest's first row carries `SourceHaloID = 1 + Σ_{g<F} n_g + rank`, so the minimum over slabs of each forest's first-row `SourceHaloID` is `1 + Σ_{g<F} n_g`, and consecutive minima give `n_F` (the last forest's from the headers' halo total); this needs no assumption about which snapshot a forest reaches. The plan refuses, naming the first offending value, a slab whose `ForestIndex` is not non-decreasing, a forest in [0, n_forests_total) present in no slab, minima that do not increase with `ForestIndex`, or totals that do not sum to the headers' halo total. It then cuts files under Decision 4 and writes `file_plan.json`: per file the forest range, forest count and halo count, plus the caps, the dataset identity and the record digest. Every forest's total is re-counted during assembly and must agree.
- **`convert`** (Slices 2 and 3): each worker takes files; for a file it finds the first row of its first forest in every slab by binary search on `ForestIndex` (about 28 seeks per slab, once per file), then streams every slab's columns forward in windows of `--window-rows` rows (default 1,048,576: 8 MB per int64 column read, at which the LaCie sustains about 175 MB/s over interleaved streams, and 140 MB per slab held, about 10 GB per worker over 70 slabs; 4,194,304 reaches about 350 MB/s at 41 GB per worker, a trade the rehearsal measures) and emits forests in ascending `ForestIndex` as soon as every slab's window has passed them. Assembly is vectorised over all the forests a set of windows completes (the record position of every row is its forest's file offset plus its `HaloRankInForest`, and every link is mapped by one gather), so the per-forest Python cost is a bounded number of calls, which matters for 244,953,607 forests of median 36 halos. A forest is assembled by concatenating its slab blocks from the highest snapshot down; the concatenation must give `HaloRankInForest` exactly 0..n−1 (refusal otherwise, naming the forest), and the forest's total must equal the plan's. Each link is mapped from (target snapshot from the companion column, global row) to the forest-local rank `HaloRankInForest[target slab][row]`, which the assembler has in memory because every link resolves inside the forest's own blocks (a link whose target row lies outside the forest's block of the target slab is a refusal naming the halo); −1 stays −1; and every mapped link is verified by `MostBoundID` equality between the source link's target row and the record it now points at. The record is packed with the package-derived dtype and appended to the file after its header (`ntrees`, `totNHalos`, `counts[]` from the plan). Memory per worker is stated per phase: 70 windows × `--window-rows` × 140 B, plus the forest being assembled (the super-forest's columns about 18 GB plus its rank maps), plus the output buffer; the CLI prints the bound for its options before it starts.
- **Verification and manifest** (Slice 3): a file is written to a temporary name, re-read whole and checked (header counts against the plan, size equals header plus records, the stream's SHA-256 equals the SHA-256 computed while writing), then renamed; the manifest records the input dataset identity (`forest_id_sha256`, headers, `column_mapping_sha256`), the package's `halo_properties.yaml` md5, the record layout (names, offsets, sizes, total) and its digest, the file plan md5, the caps, the converter commit, per-file forest and halo counts, SHA-256 and wall time, and the totals; only the parent process writes it. A rerun resumes by skipping files whose manifest entry and on-disk SHA-256 agree; a file of a planned name that the manifest does not record as complete (a rename that a kill separated from its manifest entry) and any `.partial` are moved aside to a `stale` name, never deleted, and the file is rebuilt. Exit 2 for every refusal before or during a pass, 1 for a verification failure.

### The package: `simulations/shin-uchuu-lhalo/`

`simulation_info.yaml`: `tree_type: lhalo_binary`, `tree_name: shin-uchuu-cut`, `first_file 0`, `last_file` the plan's last file number, `unique_galaxy_id_multiplier: 20000000000`, the Shin-Uchuu cosmology, box and particle mass of the sibling packages; no `forests_per_file` or distribution scheme (per-file reader). `halo_properties.yaml`: the `shin-uchuu-ascii` list in order, L-Halo style (the on-disk record), with `Len`, `Spin` and `M_Crit200` described as stored values; the compiled record is 88 bytes. The committed fixture is the `shin-uchuu` package's `worked_ascii` version 3 fixture converted by the new converter (`_tests/data/worked_ascii_lhalo/`, `regenerate.sh`), declared by `_tests/input/test_simulation.yaml` so the generic tiers run on it. Run files `halos-only_shin-uchuu-lhalo.yaml` and `sage16_shin-uchuu-lhalo.yaml` mirror the `shin-uchuu` ones.

### The gate

`simulations/shin-uchuu-lhalo/_tests/scientific/test_cross_format_identity.py`, a `GatePackage` over `tests/framework/parity_gate.py` with `vertical="shin-uchuu-lhalo"`, `horizontal="shin-uchuu"`, both sides pointed at the Stage R cut subset from outside (the package's `snapshots` will hold production): `MIMIC_SHIN_UCHUU_CUT_SUBSET_LHALO` names the converted subset directory and `MIMIC_SHIN_UCHUU_CUT_SUBSET_DATASET` the subset's horizontal dataset; unset, the gate skips with its reason after a data-free pin check, as the `shin-uchuu` gate does (`simulations/shin-uchuu/_tests/scientific/test_cross_format_identity.py`). The harness gains three things in Slice 5, each the smallest change the reverse direction needs: the vertical-side counterpart of `horizontal_dataset` (a scratch variant of the committed vertical run file adding `input.simulation_dir`, `input.first_file` and `input.last_file`, with the run-file stage diffing the horizontal file against the committed vertical file and checking the vertical variant separately as "committed plus exactly those three keys"); an expected header `source_format` pinned separately from the vertical package's `tree_type` (here `consistent_trees_ascii` against `lhalo_binary`), since today one field serves both (`tests/framework/parity_gate.py:180-181`, `:1130`, `:1255`); and a reverse-direction inventory proof, because the dataset's `forests.h5` ordinals name the original ASCII tree files, not the L-Halo files (`:1309-1338`): with `vertical_dataset` set, the dataset stage reads the converted directory's `manifest.json` and requires its input identity (`forest_id_sha256`, `column_mapping_sha256`, forest and halo totals) to equal the horizontal dataset's and its file list to equal the files the vertical side reads. Four legs: `halos-only` and `sage16`, fixed and dynamic.

### The production procedure

Owner-operated in Mode A between runs 2 and 3, following the v3 procedure's discipline (step wrapper, ledger, binaries copied out of the tree, every long step detached under `caffeinate`, one LaCie-bound job at a time); its record is the acceptance record's "Production" section. Steps are at [The production procedure](#the-production-procedure-owner-operated-between-runs-2-and-3).

---

## Repository Evidence

Anchors read at the planning baseline; the sections above cite them in place. The ones a slice must re-read before it starts:

- `src/io/vertical/binary.c:62-65`, `:77-131`, `:196-217`, `:231` (the reader's file layout, header types, the `int` running sum, the empty extension).
- `src/io/vertical/interface.c:71-73`, `:85-97`, `:143-197` (file range, missing files, the per-unit allocations).
- `src/core/vertical_driver.c:201-225`, `:250-271`, `:324-326`, `:375-436`, `:455` (identity verdict, offsets, unit loop, `--skip`, MPI stride).
- `src/core/build_model.c:92-175`, `:259-275`; `src/include/galaxy_id.h:35-56`, `:88-99`; `src/core/horizontal_driver.c:652-666`.
- `scripts/generate_properties.py:58-100`, `:1101-1120`; `scripts/generate_test_registry.py:67-71`, `:132-134`; `scripts/discovery.py:46-65`.
- `convert/mimic-convert/HORIZONTAL-HDF5-FORMAT.md:97`, `:131`, `:314-372`, `:397-409`, `:419`, `:443`, `:461-467`, `:471-483`.
- `convert/mimic-convert/horizontal_dataset.py:99-190`, `:272-312` (open-time checks, `iter_columns`, `read_rows`); `convert/mimic-convert/rewriter/__init__.py:31-35`; `convert/mimic-convert/tests/fixtures.py` (the version 3 fixture builders); `convert/mimic-convert/tests/test_cli.py:197` (an L-Halo fixture writer).
- `convert/mimic-convert/tests/run_generalisation_acceptance.py:1-80`, `:234`, `:1641-1690`, `:2473-2606`, `:2703-2722`; `tests/unit/tools/dump_ctrees_topology.c:1-40`; `tests/unit/tools/build_topology_dump.sh`.
- `tests/framework/parity_gate.py:137`, `:167-227`, `:321-368`, `:524-549`, `:1106-1110`, `:1124-1160`; `tests/integration/test_parity_gate_helpers.py`.
- `simulations/shin-uchuu-ascii/halo_properties.yaml:33-146`, `simulations/shin-uchuu-ascii/simulation_info.yaml`, `simulations/shin-uchuu/_tests/` (fixture, `regenerate.sh`, `test_simulation.yaml`, the gate), `simulations/mini-millennium/halo_properties.yaml:1-40`, `simulations/mini-millennium/_tests/unit/test_tree_loading.c`, `simulations/micro-uchuu-hdf5/_tests/integration/test_reader_smoke.py`.
- `models/sage16/input/sage16_shin-uchuu.yaml`, `models/halos-only/input/halos-only_shin-uchuu.yaml`.

---

## Execution Structure, Implementation Profiles and Run Preparation

**Three Mode B runs and one Mode A procedure.** Run 1 is Slices 1 to 5 (the converter, the package, the gate); run 2 is Slices 6 to 8 (the subset rehearsal, the documentation, the SAGE26 brief); the owner then runs the production procedure in Mode A; run 3 is Slice 9 (the closeout). Each later `init` attests the earlier slices (`--attest`). Every slice is independently gateable; PM executes them in order with no batching. The branch is `feature/shin-uchuu-cut-vertical`, created at `init`.

| Slice | Run | Recommended Developer | Effort | Difficulty | Reason |
|---|---|---|---|---|---|
| 1 | 1 | Claude Opus 5.5 | high | moderate | The record derived from a package's YAML with C alignment, the harness's `compare` option for the reverse direction, and the file plan from the per-forest totals |
| 2 | 1 | Claude Fable 5.1 | high | hard | The forest assembler: the k-way pass over forest-blocked slabs, rank proof, link remapping and `MostBoundID` verification, with adversarial fixtures |
| 3 | 1 | Claude Opus 5.5 | high | hard | The CLI, file-parallel workers, write-verify-rename, the manifest and its interruption recovery, exit codes and memory statements: concurrency and persistent state |
| 4 | 1 | Claude Opus 5.5 | high | moderate | The package, its fixture, its record-layout and reader tests, run files |
| 5 | 1 | Claude Opus 5.5 | high | hard | The harness's three reverse-direction changes (vertical dataset variant and diff rule, the header format pin, the manifest-proved inventory) and the package's gate |
| 6 | 2 | Claude Opus 5.5 | high | hard | The subset rehearsal on real data: halo-level proof, the four-leg gate, the measured rates and the production projections |
| 7 | 2 | Claude Sonnet 5.5 | high | easy | Guides, converter manual, skills, package README, changelog, pathway |
| 8 | 2 | Claude Opus 5.5 | medium | moderate | The SAGE26 reader brief |
| 9 | 3 | Claude Sonnet 5.5 | high | easy | After the procedure: the record, provenance, pathway, changelog |

Pass the table's model and effort explicitly to `start-slice` and record the resolved model versions. The review panel is the owner's standing one: drift audit Claude Sonnet 5.5 at high effort; code reviewers Claude Fable 5.1 and Codex `gpt-6.1-sol`, both at high effort, in parallel with staggered starts, every review complete before any steer. An unavailable model is a setup blocker, never permission to substitute silently.

**Approvals.** Slices marked `Approval needed before implementation: yes` need a recorded human approval (`approve`) before they start; this document grants none. Slices 1, 5, 7 and 8 run unattended.

**Before run 1,** in order:

1. Commit this plan, the pathway update and the brief's status note (PM's `init` binds the run to this file's bytes and refuses a dirty tree outside `.pm/`).
2. `python3 ~/.claude/skills/project-manager/scripts/pm.py check-plan --plan docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-IMPLEMENTATION-PLAN.md --repo .` and resolve every warning.
3. Confirm `make`, `mimic_venv` with h5py and numpy, HDF5, and that `simulations/shin-uchuu/snapshots` resolves to the cut dataset (its real-data schema test half then runs rather than skips).
4. Confirm `make tests-converter` (1550 cases at the baseline), `make tests-horizontal-v3` and the default pair's tiers pass at the baseline.
5. Record LaCie and Internal free space and the llama router's state.

**Before run 2:** run 1 accepted; about 600 GB of LaCie space chosen for Slice 6's scratch (three subset conversions at about 36 GB each, the dump of 406 million halos at about 60 to 80 GB of text, the comparator's spill of about 170 GB plus its request streams, the gate's worktrees and runs at about 100 GB, and two production files converted cold), with the comparator's `--spill-dir` on it (the Stage R cut subset is read only). **Before the procedure:** run 2 accepted. **Before run 3:** the procedure complete, its record and step 7's one-line commit on the branch.

### User-facing documentation (binding on every slice)

Owner, 2026-10-16, carried from the v3 plan: **every document outside `docs/dev/` is user-facing.** READMEs, the guides, the skills, the format document, `CHANGELOG.md`, `AGENTS.md`, run-file and YAML comments, and code comments and docstrings say what the code and data are and how to use them, never how the work was done. None carries log-type content: procedure step numbers, wall times or per-step RSS of production runs; NT, transfers or machine-local paths; deviations, review panels or narrative of the work; this plan's identifiers. Measured properties of a tool (a memory bound at a stated dataset size) are documentation, not log content. Provenance stays where a dataset's identity needs it (what it was made from and the commit that made it), phrased as history. Nothing outside `docs/dev/` references anything inside it. PM checks every added line outside `docs/dev/` against this convention before accepting a slice, and tells every Developer and reviewer.

## Resource Profile

| Slice or step | I/O | Memory (peak RSS) | CPU | Rough duration | Keep free |
|---|---|---|---|---|---|
| 1 to 5 | light (fixtures); the production file plan is Slice 6's, a 360 GB read of two columns | a few GB | light; the converter suite about 6 min, the tiers about 3 min each | under two hours of machine time each | no |
| 6 | **moderate to heavy**: the 57 GB cut subset read once per conversion run (three runs to measure workers; the subset fits the page cache, so the cold rate comes from two production files converted into scratch), the dump of 406 million halos (about 60 to 80 GB of text) read by the comparator with about 170 GB of spill on the LaCie, the gate's eight runs and four builds, one serial and one `-np 4` vertical `sage16` run for timing; about 600 GB of scratch in all | the converter at about 10 GB per worker at the default window on the subset (largest forest 1.25 million halos), so 8 workers about 80 GB, and one worker on a production file up to about 40 GB; the comparator under its `--budget-mb`; the vertical runs under about 15 GB (the subset's largest forest is 1.25 million halos); the gate measured 12.4 GB on the uncut subset | high during the conversion (workers) and the runs | 8 to 16 h of machine time across one or two sessions | no, but no other LaCie-bound job alongside |
| 7, 8 | none | a few GB | light | under two hours each | no |
| Procedure: file 0 alone | the super-forest's blocks of every slab (about 18 GB read), 11 GB written | the converter: one worker holding the super-forest's columns and maps, about 40 GB bound stated by the tool; the vertical runs on file 0: **166 to 205 GB modelled, allow 250 GB** | one worker, then one core | under an hour to convert; minutes to run | **yes, 300 GB** for the runs; the llama router stopped |
| Procedure: the whole conversion | **heavy**: 3.16 TB read in 70 interleaved streams per worker, about 1.98 TB written, then every file re-read once to verify; LaCie to LaCie | per worker the stated bound (default window about 10 GB of slab windows plus the forest in flight, up to about 40 GB for the super-forest); 8 workers about 80 to 120 GB, more if several large forests are in flight or a larger window is chosen | 8 workers | 8 to 20 h (the previous LaCie-to-LaCie rewrite ran 15 h at about 115 MB/s combined; parallel workers are expected to be CPU-bound rather than disk-bound) | yes, 200 GB |
| Procedure: `sage16` vertical, serial | 1.98 TB read once, 0.54 TB written | about 166 to 250 GB at the super-forest, far less elsewhere | one core | 9 to 16 h projected, decided from the subset-measured rate | **yes, 300 GB**; the llama router stopped |
| Procedure: the comparison | both outputs read once (0.54 TB each; the reference on Internal) | **about 300 GB** (297 GB measured for the default mode at this scale) | one core | about 2 h (7,395 s measured) | **yes, everything**; the llama router stopped |
| Procedure: transfers to NT | the dataset (1.98 TB) and the output (0.54 TB) at about 105 MB/s | none | none | about 5.5 h and 1.5 h | no |
| 9 | none | a few GB | light | under two hours | no |

The whole-machine moments are the production `sage16` run and the comparison; the file 0 runs need 300 GB free. The llama router is stopped for procedure steps 3, 6 and 8. Nothing in Slices 1 to 5, 7, 8 or 9 needs more than a few GB.

Run on the feature branch the owner names at `init`; no implementation session edits this plan, dependency manifests or licences, regenerates a physics baseline, deletes source data, or pushes. Every slice uses one `MODEL`/`SIMULATION` pair per command sequence and restores the default pair's generated code before its final default-tier run. Format check: run the full unmodified `./scripts/beautify.sh`, then `make check-format`. Any file under `archive/` is gitignored and never cited as the source of a fact outside `docs/dev/`.

---

## Slice 1: The vertical record and the file plan

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires nothing.
- The package `convert/mimic-convert/vertical/` begins with two pure modules: `record.py`, which derives a package's L-Halo record from its ordered `halo_properties.yaml` exactly as the C compiler lays out `struct RawHalo`, and `plan.py`, which derives every forest's halo total from a version 3 dataset's slabs and cuts the output files under Decision 4. The generalisation acceptance harness's `compare` gains the one option the reverse direction needs: comparing a dataset of one route with an `lhalo_binary` dump of its own vertical conversion; the harness stays independent of the converter (it imports nothing from it).

### Acceptance Criteria

- [ ] `vertical/record.py` exposes a function that, given a `halo_properties.yaml` path (or its parsed `halo_properties` list), returns a numpy structured dtype built with C alignment from the declared `type` of every entry in declaration order using the generator's own type set (`int` → `<i4`, `float` → `<f4`, `long long` → `<i8`, `double` → `<f8`, `vec3_float` → `(<f4, (3,))`, `vec3_int` → `(<i4, (3,))`), and raises `ConverterError` naming the entry for any other type; the dtype's `itemsize` includes trailing padding so that an array of records has the compiler's stride.
- [ ] For `simulations/shin-uchuu-ascii/halo_properties.yaml` (the field list Slice 4's package adopts) the derived dtype has `itemsize` 88 with `MostBoundID` at offset 72 and `SnapNum` at offset 80; for `simulations/mini-millennium/halo_properties.yaml` it has `itemsize` 104 with no padding; both are asserted literally in `tests/test_vertical_record.py`.
- [ ] `record.py` also exposes the record's layout as a stable, JSON-serialisable description (field names, offsets, sizes, item size) and a SHA-256 digest of that description, which Slice 3's manifest records.
- [ ] `run_generalisation_acceptance.py compare` gains `--dataset-source-format <format>` (default: the value of `--source-format`): `compare_dataset` checks the dataset header's `source_format` against it instead of against `--source-format`, while the dump's reader is still checked against `--source-format`; `build_parser` documents it as the option for a dataset of one route compared with a vertical conversion of it read by another reader; one new test in `tests/test_generalisation_acceptance.py` compares a synthetic ASCII-route dataset with an `lhalo_binary`-labelled dump of its rows and passes, and the same without the option fails with the existing message; the harness still imports nothing from the converter.
- [ ] `vertical/plan.py` exposes `forest_totals(dataset)` over a `HorizontalDataset`: one bounded pass (`iter_columns`) over every slab's `ForestIndex` and `SourceHaloID`, keeping per forest the minimum over slabs of its first-row `SourceHaloID` (8 B per forest), from which each forest's total is the difference of consecutive minima and the last forest's the headers' halo total less its minimum plus one; it refuses, naming the first offending value, a slab whose `ForestIndex` is not non-decreasing, a forest in [0, `n_forests_total`) present in no slab, minima that are not strictly increasing with `ForestIndex`, a first minimum other than 1, or totals that do not sum to the dataset's halo total.
- [ ] `plan.py` exposes `cut_files(totals, max_halos_per_file, max_forests_per_file)` returning contiguous forest ranges in ascending `ForestIndex` such that a new file starts when adding the next forest would exceed either cap, a forest alone in a file may exceed the halo cap, no file holds more than the cap in forests, and the ranges tile `[0, n_forests)`; it refuses a cap that would put more than 2,147,483,647 halos or forests in a file, and a single forest above 2,147,483,647 halos.
- [ ] `plan.py` writes and reads `file_plan.json`: the dataset identity (`forest_id_sha256`, `n_forests_total`, halo total, `column_mapping_sha256`), the caps, the record digest (the binding; the package's `halo_properties.yaml` path and md5 are recorded for information only), and per file its number, first and last forest, forest count and halo count; reading refuses a plan whose dataset identity or record digest does not match the open dataset and the derived record.
- [ ] Tests in `tests/test_vertical_plan.py` on the synthetic version 3 fixtures of `fixtures.py`, including one whose forest ends before the final snapshot (as the `worked_ascii` fixture's forest 30 does): literal totals and literal file cuts for small caps (including a forest larger than the halo cap alone in its file, and the forest cap binding before the halo cap); every refusal above, each from a fixture mutated in one way.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/vertical/` (new package: `__init__.py`, `record.py`, `plan.py`)
  - `convert/mimic-convert/tests/test_vertical_record.py` (new)
  - `convert/mimic-convert/tests/test_vertical_plan.py` (new)
  - `convert/mimic-convert/tests/run_generalisation_acceptance.py`
  - `convert/mimic-convert/tests/test_generalisation_acceptance.py`
- Functions/classes/components allowed to change: the new modules; in the harness only `compare_dataset`, `cmd_compare`, `build_parser` and the new test's fixtures.
- Tests allowed or expected to change: the two new files and the one new harness test.

### Explicit Non-Goals

- No assembly of forests, no L-Halo record-file writing, no CLI of the converter (Slices 2 and 3); no change to `horizontal_dataset.py`, the writer, the rewriter or any package; no change to the harness's extractors or `compare-extras`; no C change.

### Risk Flags

- Risky surfaces touched: the acceptance harness's `compare` contract (shared evidence tooling).
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: moderate

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the derived dtype for `shin-uchuu-ascii` printed with `numpy.dtype.descr` and offsets shows the 4-byte trailing pad; `plan` run on `simulations/shin-uchuu/_tests/data/worked_ascii` gives totals 7, 4 and 2 halos for forests 10, 20 and 30 (the fixture's three forests, the last ending at snapshot 2).

### Rollback Path

- Revert the slice commit.

---

## Slice 2: The forest assembler

### Intended Change

- Recommended Developer: Claude Fable 5.1; effort: high. Requires Slice 1.
- `vertical/assemble.py`: the bounded k-way pass over a forest-blocked version 3 dataset that yields each forest of a `ForestIndex` range as one L-Halo record array in rank order with forest-local links, every link verified, with the refusals that make a malformed dataset fail by name rather than produce a wrong file.

### Acceptance Criteria

- [ ] `assemble.py` exposes an iterator over the forests of a given `ForestIndex` range `[F_lo, F_hi)` of a `HorizontalDataset` whose `links_adjacent` need not be 1 (the companion target-snapshot columns are read and honoured): it finds the first row of `F_lo` in every slab by binary search on `ForestIndex` through `read_rows`, then streams every slab's twenty columns forward in windows of `window_rows` rows (a constructor argument, default 1,048,576), emitting forest F once every slab's window extends past F; a slab in which F is absent contributes an empty block. Assembly is vectorised over every forest the current windows complete (record positions from the forests' running offsets plus `HaloRankInForest`, links by one gather per link kind), yielding per-forest views; the per-forest Python work is a bounded number of calls, which `tests/test_vertical_assemble.py` checks by counting calls on a fixture of a thousand small forests.
- [ ] A forest's record array is the concatenation of its slab blocks from the highest snapshot down to the lowest, in each block's row order; the assembler asserts that the concatenated `HaloRankInForest` equals 0..n−1 exactly and that `ForestIndex` equals F on every row, refusing otherwise with the forest and the first offending rank.
- [ ] Every link of the five kinds is mapped from (target snapshot, global row) to the target's `HaloRankInForest`, which the assembler reads from the forest's own block of the target slab; a non-null link whose target row lies outside that block, or whose companion snapshot names a slab in which the forest has no block, is a refusal naming the forest, the halo's rank and the link; −1 maps to −1 and `FirstHaloInFOFgroup` is never −1.
- [ ] Every mapped non-null link is verified: the `MostBoundID` of the target row in the source slab equals the `MostBoundID` of the record the mapped rank names; a mismatch is a refusal naming both ids.
- [ ] The payload fields of the record are copied verbatim from the source columns (no unit, type or convention change: `M_Crit200` stays float32 Msun/h, `Spin` and `Len` as stored); the record dtype is the one `record.py` derives for the named package; a source payload column (every field but the five links, which are mapped from int64 rows to the record's link type) whose dtype does not equal the record field's base dtype is a refusal before any row is read, and a mapped rank that does not fit the record's link type is a refusal naming the forest.
- [ ] The memory bound per assembler instance is a stated function of `window_rows`, the number of slabs and the largest forest in the range (its total halos from the plan), exposed as a function the CLI prints; the assembler holds no per-forest state beyond the forest in flight and the windows.
- [ ] Tests in `tests/test_vertical_assemble.py` on synthetic version 3 fixtures built with `fixtures.py`: a worked forest whose expected rank order and expected local links are typed literally and compared; a forest absent from some slabs; a forest split across two windows (`window_rows` smaller than a block); a promoted halo (a FoF group of one whose `FirstHaloInFOFgroup` is itself and `NextHaloInFOFgroup` −1) and its former group; a progenitor chain of three and its `NextProgenitor` order; the last forest of the dataset; a range starting mid-dataset; and the refusals, each from a one-way mutation (a link pointing into another forest's block, a rank gap, a wrong companion snapshot, a column of the wrong dtype) plus the `MostBoundID` verification exercised by a patched mapping between assembly and verification (a mapped rank pointed at the wrong record), since swapping ids in a valid source dataset changes nothing the invariants see.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/vertical/` (new module `assemble.py`; `__init__.py` exports)
  - `convert/mimic-convert/tests/test_vertical_assemble.py` (new)
  - `convert/mimic-convert/tests/fixtures.py`
- Functions/classes/components allowed to change: the new module; in `fixtures.py` only additive builders the new tests need.
- Tests allowed or expected to change: the new file; additive fixtures.

### Explicit Non-Goals

- No file writing, manifest, workers or CLI (Slice 3); no change to `horizontal_dataset.py` (its `read_rows` and `iter_columns` are sufficient); no C change; no production data.

### Risk Flags

- Risky surfaces touched: the operation that produces the production vertical dataset.
- Approval needed before implementation: yes
- Independent audit required: yes
- Difficulty: hard

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the assembler run over the `shin-uchuu` `worked_ascii` fixture prints each forest's ranks and local links; the Developer checks one forest by hand against `h5dump` of the fixture.

### Rollback Path

- Revert the slice commit.

---

## Slice 3: The converter CLI, workers, verification and manifest

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slices 1 and 2.
- `convert/mimic-convert/write_vertical_trees.py` with `vertical/writer.py`: the `plan` and `convert` subcommands over Slices 1 and 2, file-parallel workers, write-verify-rename per file, the conversion manifest, resume, exit codes and the printed memory bound.

### Acceptance Criteria

- [ ] `write_vertical_trees.py plan --input <v3 dataset> --halo-properties <package yaml> --output <dir> [--max-halos-per-file N] [--max-forests-per-file M]` writes `<dir>/file_plan.json` (Slice 1) and prints the file count, the largest file's halos and forests, and the dataset totals; defaults 100,000,000 and 1,000,000.
- [ ] `write_vertical_trees.py convert --input <v3 dataset> --halo-properties <package yaml> --output <dir> --tree-name <name> [--workers W] [--window-rows R] [--only-files a,b-c]` writes `<dir>/<name>.<k>` for every planned file k (or the selected ones) with the L-Halo header (`int32` `ntrees`, `int32` `totNHalos`, `int32` counts in forest order from the plan) followed by the forests' records in ascending `ForestIndex`, each in rank order; a file is written to `<name>.<k>.partial`, re-read whole, checked (header against the plan, size equals header plus `totNHalos` records, the re-read stream's SHA-256 equals the SHA-256 accumulated while writing) and renamed only then; `--workers` processes files concurrently, each worker one file at a time, with the largest files first.
- [ ] `convert` refuses (exit 2), before any row is read, a missing or mismatching `file_plan.json`, a `tree_name` with a path separator, a `--workers` or `--window-rows` below 1, and a dataset that is not version 3 (the `HorizontalDataset` open-time checks); it prints the per-worker memory bound from the assembler's function for the chosen options and the plan's largest forest before starting.
- [ ] `<dir>/manifest.json` records: the converter commit and command line, the input dataset identity (`forest_id_sha256`, `n_forests_total`, halo total, `column_mapping_sha256`, `links_adjacent`), the package's `halo_properties.yaml` path and md5, the record layout and digest, the plan's md5 and caps, `tree_name`, and per completed file its number, forest range, forest and halo counts, byte size, SHA-256, wall time and worker; only the parent process writes it, atomically, after every completed file. A rerun of `convert` with the same inputs skips files whose manifest entry's SHA-256 equals the on-disk file's and redoes the rest; a file of a planned name without a complete manifest entry (a kill between the rename and the manifest write) and any `.partial` are moved aside to `<name>.<k>.stale-<timestamp>` and `.partial.stale-<timestamp>`, never deleted, logged with their sizes, and the file is rebuilt.
- [ ] A verification failure in any file (exit 1) names the file and the check, leaves no renamed file for it, and does not stop other workers' completed files from being recorded.
- [ ] `--help` for both subcommands states the memory bound's terms, the exit codes (0, 1 verification, 2 refusal) and that a forest larger than the halo cap occupies its file alone.
- [ ] Tests in `tests/test_write_vertical_trees.py`: the round trip on a synthetic dataset (plan, convert, then an independent re-read of every file with the Slice 1 dtype reproduces every forest's records and the literal expected links of Slice 2's worked forest; the headers' counts equal the plan's); resume (a run interrupted after one file, by a monkeypatched failure, then rerun, completes with the first file's manifest entry unchanged); interruption between a file's rename and its manifest entry (the orphan is moved aside and the file rebuilt, byte-identical); each refusal and the verification failure (a file corrupted between write and verify by a patched re-read); `--only-files`; two workers producing the same bytes as one.
- [ ] Required evidence: `make tests-converter` via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/write_vertical_trees.py` (new)
  - `convert/mimic-convert/vertical/` (new module `writer.py`; `__init__.py` exports)
  - `convert/mimic-convert/tests/test_write_vertical_trees.py` (new)
  - `convert/mimic-convert/tests/fixtures.py`
- Functions/classes/components allowed to change: the new CLI and module; in `fixtures.py` only additive builders.
- Tests allowed or expected to change: the new file; additive fixtures.

### Explicit Non-Goals

- No change to `convert_trees.py`, `rewrite_trees.py`, `forest_census.py` or the format document; no package, run file or C change; no production data; no documentation beyond docstrings and `--help` (Slice 7).

### Risk Flags

- Risky surfaces touched: a new public CLI in the converter; the operation that produces the production vertical dataset.
- Approval needed before implementation: yes
- Independent audit required: yes
- Difficulty: hard

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make tests-converter` (subagent); `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `plan` and `convert` run on `simulations/shin-uchuu/_tests/data/worked_ascii` into a scratch directory; `xxd` of the file's first 32 bytes shows the three-forest header.

### Rollback Path

- Revert the slice commit.

---

## Slice 4: The `shin-uchuu-lhalo` package, its fixture and its tests

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 3.
- The vertical simulation package for the cut dataset, with the committed test-sized fixture the generic tiers run on, a record-layout test that pins the compiled `struct RawHalo` against the converter's derivation, a reader test through the real `lhalo_binary` path, and the two run files.

### Acceptance Criteria

- [ ] `simulations/shin-uchuu-lhalo/simulation_info.yaml` declares `tree_type: lhalo_binary`, `tree_name: shin-uchuu-cut`, `first_file: 0`, `last_file: 0` with a comment that `last_file` is the last file of the dataset `snapshots/` points at, `simulation_dir: simulations/shin-uchuu-lhalo/snapshots`, `snapshot_list_file: simulations/shin-uchuu-lhalo/shin-uchuu.a_list`, `unique_galaxy_id_multiplier: 20000000000`, and the cosmology, box size and particle mass of `simulations/shin-uchuu-ascii/simulation_info.yaml` exactly; `shin-uchuu.a_list` is a copy of the sibling's.
- [ ] `halo_properties.yaml` lists the fourteen entries of `simulations/shin-uchuu-ascii/halo_properties.yaml` in the same order with the same types, units, `h_convention`, roles and ranges, with its header and descriptions saying this is the on-disk L-Halo record of the cut dataset in the horizontal dataset's own units and that `Len`, `Spin` and `M_Crit200` are stored values; `make MODEL=halos-only SIMULATION=shin-uchuu-lhalo generate` then `check-generated` pass and the generated `struct RawHalo` has fourteen members.
- [ ] `_tests/data/regenerate.sh` converts the `shin-uchuu` package's committed `worked_ascii` fixture with `write_vertical_trees.py` (plan and convert, caps small enough to produce at least two files) into `_tests/data/worked_ascii_lhalo/` beside a copy of the fixture's a_list, and fails if the installed data files are not exactly the converted set; the data files and `file_plan.json` are committed, the conversion's `manifest.json` is not (it carries the commit, command line and wall times, which would make the fixture irreproducible and is log-type content); `_tests/input/test_simulation.yaml` points the generic tiers at it with the fixture's file range and a_list.
- [ ] `_tests/unit/test_record_layout.c` asserts, through `sizeof` and `offsetof`, the record's size 88 and the offsets of every member against literal values typed into the test from the Slice 1 derivation (named as their source), so that a package or generator change that moves the layout fails here; it skips, naming the compiled simulation, under any other.
- [ ] `_tests/unit/test_tree_loading.c` (modelled on mini-Millennium's) opens the fixture's partitions through `open_partition`/`load_unit`, checks every file's tree count and halo counts against the fixture's `file_plan.json` values typed literally, every link in [−1, n) and `FirstHaloInFOFgroup` never −1, `SnapNum` non-increasing along ranks within a forest, and no leak.
- [ ] `_tests/integration/test_fixture_halo_identity.py` proves the fixture halo by halo: it builds the dump tool for the compiled model and `shin-uchuu-lhalo` into its own build directory (`tests/unit/tools/build_topology_dump.sh` with `TOPOLOGY_DUMP_BUILD_DIR`, the compiled model so the ambient generated code is untouched), dumps the fixture with `--source-payload` through a scratch run file naming it, and runs `run_generalisation_acceptance.py compare --dataset simulations/shin-uchuu/_tests/data/worked_ascii --source-format lhalo_binary --dataset-source-format consistent_trees_ascii --dump <dump>`, requiring PASS; it skips with its reason when the compiled simulation is not `shin-uchuu-lhalo` or h5py is absent, and fails on any comparator check.
- [ ] `models/halos-only/input/halos-only_shin-uchuu-lhalo.yaml` and `models/sage16/input/sage16_shin-uchuu-lhalo.yaml` differ from the `shin-uchuu` run files only in their header comment, `simulation.name` and `output_directory` (asserted by `diff` in the slice's evidence).
- [ ] `make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests summary` and `make MODEL=sage16 SIMULATION=shin-uchuu-lhalo tests summary` run with no FAIL, every SKIP naming its reason; the default pair's generated code is restored and its tiers pass.
- [ ] `README.md` for the package: purpose, the record (user-facing, including that masses are float32 Msun/h and that this is not the 104-byte SAGE record), how the data is obtained (the converter command), the fixture, the tests, the identity multiplier invariant with the siblings, and the created-record limit (at the production dataset's forest count a module that creates records cannot run on this package under the vertical driver); provenance of the production dataset is left for Slice 9.
- [ ] Required evidence: the two package tier runs above via a subagent; `make check-docs`; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `simulations/shin-uchuu-lhalo/` (new package)
  - `models/halos-only/input/halos-only_shin-uchuu-lhalo.yaml` (new)
  - `models/sage16/input/sage16_shin-uchuu-lhalo.yaml` (new)
  - `tests/unit/run_tests.sh`
- Functions/classes/components allowed to change: the new package; in `run_tests.sh` only what registering the package's C tests requires, if anything.
- Tests allowed or expected to change: the new package tests.

### Explicit Non-Goals

- No change to the readers, the generator, `scripts/discovery.py`'s test lists, the converter or the gate (Slice 5); no production data; no guide or skill text (Slice 7).

### Risk Flags

- Risky surfaces touched: generated code for a new package; the test registry.
- Approval needed before implementation: yes
- Independent audit required: yes
- Difficulty: moderate

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `make MODEL=halos-only SIMULATION=shin-uchuu-lhalo generate check-generated`; the two `tests summary` runs (subagent); `make generate` for the default pair and `make tests-unit` (subagent); `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `./mimic models/halos-only/input/halos-only_shin-uchuu-lhalo.yaml` with `input.simulation_dir` pointed at the fixture runs to completion and its log shows the identity-space verdict line.

### Rollback Path

- Revert the slice commit; `make generate` for the default pair.

---

## Slice 5: The harness's vertical-dataset option and the package's gate

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slice 4.
- `tests/framework/parity_gate.py` gains the three changes the reverse direction needs (a vertical-side dataset variant with its run-file diff rule, a header `source_format` pinned apart from the vertical `tree_type`, and a manifest-proved inventory for a vertical dataset converted from the horizontal one), and `shin-uchuu-lhalo` gains its subset-scale cross-format identity gate against `shin-uchuu` over the Stage R cut subset. Every existing gate keeps its behaviour unchanged.

### Acceptance Criteria

- [ ] `GatePackage` gains an optional `vertical_dataset: tuple[Path, int, int] | None` (a dataset directory and the file range it holds); when set, every vertical leg runs a scratch variant of the committed vertical run file that adds exactly `input.simulation_dir`, `input.first_file` and `input.last_file` (a new `vertical_dataset_variant` beside `dataset_variant`, refusing a run file that already has an `input` section, and verified as `dataset_variant` is); `effective_vertical_run_file` returns it instead of `range_override_variant`; the inventory stage lists that directory's files; the recorded `SimulationDir` and file range of each vertical run are checked against it; `file_range` must equal the tuple's range (an assertion otherwise); and the existing `override_vertical_range` consistency rule in `read_vertical_inventory` still holds (True here, because the committed vertical run files declare no range and the package's `simulation_info.yaml` declares 0 to 0).
- [ ] When `vertical_dataset` is set, and only then, the run-file stage and the gate's data-free pin check diff each horizontal run file against the **committed** vertical run file (so `assert_horizontal_run_file_diff` sees no vertical-only key) and verify the vertical variant separately as the committed file plus exactly the three keys; every other gate keeps diffing against its effective vertical file (Millennium's and mini-Uchuu's range overrides); `stage_run_files`, `stage_preconditions` (its `assert_dataset_present` call covers the vertical dataset too) and the leg checks that read `SimulationDir` are the functions this touches.
- [ ] `GatePackage` gains `header_source_format: str | None` (default `None`, meaning `source_format`): the dataset stage checks the horizontal header's `source_format` against it, while `read_vertical_inventory` keeps checking the vertical package's `tree_type` against `source_format`.
- [ ] With `vertical_dataset` set, the dataset stage proves the inventory from the converted directory's `manifest.json` instead of `forests.h5`'s ordinals: its input identity (`forest_id_sha256`, `column_mapping_sha256`, `n_forests_total`, halo total) must equal the horizontal dataset's and its complete file list must equal the files the vertical side reads; `manifest` (the ASCII conversion manifest) and `vertical_dataset` together are refused as contradictory pins.
- [ ] `tests/integration/test_parity_gate_helpers.py` gains cases for the new variant (exactly the three keys added; refusal of a run file with an input section), for the diff-against-committed rule, for the header pin, and for the manifest-proved inventory (a matching and a mismatching synthetic manifest), in the style of `test_a_named_dataset_is_used_by_every_stage`; every existing case passes unchanged.
- [ ] `simulations/shin-uchuu-lhalo/_tests/scientific/test_cross_format_identity.py` is a plain `ParityGate` with `vertical="shin-uchuu-lhalo"`, `horizontal="shin-uchuu"`, `source_format="lhalo_binary"`, `header_source_format="consistent_trees_ascii"`, `format_version=3`, `links_adjacent=1`, the cut subset's census (406,668,896 halos, 7,206,195 forests, 0 gapped descendants, span 1), the subset dataset's `column_mapping_sha256`, `models=("halos-only", "sage16")`, both schemes, `require_halos_only_before_sage16=True`, `vertical_dataset` from `MIMIC_SHIN_UCHUU_CUT_SUBSET_LHALO` with the file range read from that directory's `file_plan.json`, and `horizontal_dataset` from `MIMIC_SHIN_UCHUU_CUT_SUBSET_DATASET`; with either variable unset the tier runs a data-free check that the pins name the subset and skips the comparison with its reason; with both set everything else fails rather than skips.
- [ ] The gate's docstring states what it covers (the cut at subset scale, both drivers, four legs) and what it does not (the production super-forest).
- [ ] Required evidence: `mimic_venv/bin/python tests/integration/test_parity_gate_helpers.py` passes; `make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests-scientific` with the variables unset shows the gate's skip with its reason; `make tests-integration` for the default pair via a subagent; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `tests/framework/parity_gate.py`
  - `tests/integration/test_parity_gate_helpers.py`
  - `simulations/shin-uchuu-lhalo/_tests/scientific/test_cross_format_identity.py` (new)
- Functions/classes/components allowed to change: `GatePackage`, `ParityGate.effective_vertical_run_file`, `read_vertical_inventory`, `stage_preconditions`, the dataset stage, `stage_run_files`, the leg checks that read `SimulationDir` and the file range, and the new helper; the new gate file.
- Tests allowed or expected to change: as listed.

### Explicit Non-Goals

- No run of the gate on data (Slice 6); no change to any other package's gate, to `assert_horizontal_run_file_diff`'s rule for existing gates, or to the comparator; no production data.

### Risk Flags

- Risky surfaces touched: the shared parity-gate harness every version 3 package's gate uses.
- Approval needed before implementation: no
- Independent audit required: yes
- Difficulty: hard

### Validation Plan

- Tests to add/update: as listed.
- Commands to run: `mimic_venv/bin/python tests/integration/test_parity_gate_helpers.py`; `make tests-integration` (subagent); the package's scientific tier with the variables unset; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the existing gates' pins still parse (`mimic_venv/bin/python -c` importing each package gate module).

### Rollback Path

- Revert the slice commit.

---

## Slice 6: The subset rehearsal

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: high. Requires Slices 1 to 5. Real data.
- The whole route exercised at subset scale on the Stage R cut subset: the conversion measured (warm on the subset, cold on two production files), the halo-level proof through Mimic's own L-Halo reader, the four-leg gate, the vertical `sage16` run timed serially and under MPI, the production file plan computed, and all of it recorded in the new acceptance record with the production projections the procedure needs.

### Acceptance Criteria

- [ ] `write_vertical_trees.py plan` and `convert` on `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-r/cut/dataset` with the `shin-uchuu-lhalo` package's `halo_properties.yaml` and `--tree-name shin-uchuu-cut` (the package's `tree_name`, which the gate's inventory and the run files derive file names from), caps chosen to give at least eight files (recorded), at 1, 4 and 8 workers into a LaCie directory the record names; each run's wall time, peak RSS (`MallocLargeCache=0 /usr/bin/time -l`), forests per second, read and write rates and the printed memory bound are recorded, and the three outputs are byte-identical (SHA-256 per file); these runs read a 57 GB subset the page cache holds, which the record says.
- [ ] A cold measurement at production scale: `plan` on the production cut dataset with the default caps (its `file_plan.json` kept beside the record's evidence; the record states the file count, the largest file (file 0, 126,380,326 halos, 1 forest), the last file and the distribution of file sizes), then `convert --only-files` for two mid-plan files into scratch at the worker count chosen from the subset runs, timed and rated as above; the record projects the whole conversion from this rate and states the owner's stop rule for the procedure (a projection above the hours the owner sets stops before the procedure).
- [ ] Halo-level proof: `run_generalisation_acceptance.py build-dump` for `halos-only × shin-uchuu-lhalo` (its own build directory), `dump --source-payload` over the converted subset (through a scratch run file naming it), then `compare --dataset <the cut subset> --source-format lhalo_binary --dataset-source-format consistent_trees_ascii --dump <dump>` with `--spill-dir` on the LaCie reports PASS with every check named; the record states the wall time, the peak RSS and the spill volume.
- [ ] The gate: `MIMIC_SHIN_UCHUU_CUT_SUBSET_LHALO=<converted subset> MIMIC_SHIN_UCHUU_CUT_SUBSET_DATASET=<cut subset dataset> make MODEL=halos-only SIMULATION=shin-uchuu-lhalo tests-scientific` passes all four legs with each leg's galaxy count, wall time and peak RSS recorded (the fixed `sage16` leg is expected at 60,033,537 galaxies and the fixed `halos-only` leg at 92,429,377, the cut subset's recorded horizontal figures; the dynamic legs' counts are recorded, not predicted); the gate's leg timings are recorded as context, and the timing the projections use is the kept serial run below.
- [ ] Vertical `sage16` on the converted subset, serial, through a scratch variant of the committed run file naming the converted subset (the gate removes its own worktrees and runs on exit, so this run is the kept reference), with wall time, peak RSS and halos per second; and the same under `mpirun -np 4` on a `USE-MPI=yes` build, with its per-rank peaks and wall time, compared identical to the serial run with the comparator (default mode).
- [ ] `docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-ACCEPTANCE.md` is created with a "Rehearsal" section: method (step wrapper, logs, code identity by commit and md5 of the converter modules), every measurement above with its log path, the scratch ledger (conversions, dump, spill, worktrees and runs, the two production files), and the production projections: conversion wall time at the cold measured rate and worker scaling, the super-forest's conversion bound from the assembler's function, the vertical run's wall time at the measured halos per second, and the disk ledger (dataset at 88 B per halo plus headers, output at the reference's output-snapshot list, free space before and after).
- [ ] Required evidence: every log named in the record; `make check-docs`.

### Authorized Surface

- Files allowed to change:
  - `docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-ACCEPTANCE.md` (new)
- Functions/classes/components allowed to change: none in code. A defect found in the converter, the package or the gate stops this slice for the owner, who routes the fix to its owning slice and its review before this slice resumes; nothing under this surface fixes code.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No conversion of the production dataset beyond the two-file cold measurement into scratch, and no production run; no change to code, run files or user-facing documents; no deletion or move of any dataset (the Stage R subsets have no NT copy).

### Risk Flags

- Risky surfaces touched: real data on the LaCie (read only), about 600 GB of scratch written.
- Approval needed before implementation: yes
- Independent audit required: yes
- Difficulty: hard

### Validation Plan

- Tests to add/update: none.
- Commands to run: as the criteria list, each under `MallocLargeCache=0 /usr/bin/time -l` through a step wrapper that logs the commit, the command and both volumes' free space.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: an independent read-only trace (a Reviewer) of every number in the record to its log.

### Rollback Path

- Revert the record; move the scratch outputs aside under the owner's trash rule (logged with sizes, nothing deleted by the session); nothing else changes.

---

## Slice 7: Guides, converter manual, skills, package README, changelog and pathway

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. Requires Slice 6.
- The documentation of record describes the converter, the package and the gate as live, user-facing and release-ready, under the binding convention above.

### Acceptance Criteria

- [ ] `convert/mimic-convert/README.md`: a section for `write_vertical_trees.py` (purpose, the two subcommands with their options, the record derived from the named package, the file-plan rule, the memory bound's terms, resume, verification, exit codes, and the halo-level check through `run_generalisation_acceptance.py`), the module map rows for `write_vertical_trees.py` and `vertical/`, and the tests section if the suite's description changes.
- [ ] `docs/DEVELOPER-GUIDE.md`: the converter section gains the reverse direction (horizontal version 3 to L-Halo binary), the simulation package list and the cross-format identity gate paragraph name `shin-uchuu-lhalo` and its gate, and the harness's `vertical_dataset` option is documented beside `horizontal_dataset`; `docs/USER-GUIDE.md` names the package where the shipped packages are listed.
- [ ] `.agents/skills/mimic-simulations-and-readers/SKILL.md`: the package table gains `shin-uchuu-lhalo`, the Shin-Uchuu paragraph describes the three packages and their roles, and the converter-profile paragraph names the vertical direction; `.agents/skills/mimic-validation-and-qa/SKILL.md` and `mimic-diagnostics-and-tooling/SKILL.md` name the new gate and the harness option where they list gates and instruments; `mimic-docs-and-writing`'s external-claims list says what is claimed for `shin-uchuu-lhalo` once Slice 9 records it (this slice adds the subset-scale claim only).
- [ ] `simulations/shin-uchuu-lhalo/README.md` is completed for the subset-scale evidence (the gate, the halo-level check) with no production provenance yet; `simulations/shin-uchuu/README.md` and `simulations/shin-uchuu-ascii/README.md` cross-reference the new package in their related-packages sections.
- [ ] `CHANGELOG.md` Unreleased gains entries for the converter, the package and the harness option, stating evidence at subset scale only.
- [ ] `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: the next-focus box and the plan inventory row reflect runs 1 and 2 as complete and the procedure as next.
- [ ] Every added line outside `docs/dev/` follows the user-facing convention; `make check-docs` passes.
- [ ] Required evidence: `make check-docs`; `./scripts/beautify.sh`; `make check-format`.

### Authorized Surface

- Files allowed to change:
  - `convert/mimic-convert/README.md`
  - `docs/DEVELOPER-GUIDE.md`
  - `docs/USER-GUIDE.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `.agents/skills/mimic-validation-and-qa/SKILL.md`
  - `.agents/skills/mimic-diagnostics-and-tooling/SKILL.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
  - `simulations/shin-uchuu-lhalo/README.md`
  - `simulations/shin-uchuu/README.md`
  - `simulations/shin-uchuu-ascii/README.md`
  - `CHANGELOG.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
- Functions/classes/components allowed to change: prose only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code change; no production claim; no edit to the format document (the vertical format is L-Halo binary, which the format document does not own).

### Risk Flags

- Risky surfaces touched: none.
- Approval needed before implementation: no
- Independent audit required: no
- Difficulty: easy

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: `rg -n 'docs/dev' --glob '!docs/dev/**'` shows no new reference.

### Rollback Path

- Revert the slice commit.

---

## Slice 8: The SAGE26 reader brief

### Intended Change

- Recommended Developer: Claude Opus 5.5; effort: medium. Requires Slice 6 (the record is final). Reads the SAGE26 repository at a pinned commit (a fresh clone into scratch, read only; nothing built or run).
- `docs/dev/SAGE26-SHIN-UCHUU-LHALO-READER-BRIEF.md`: everything the SAGE26 repository needs to read the cut vertical dataset and to compare its galaxies with Mimic's, written for whoever implements it there.

### Acceptance Criteria

- [ ] The brief pins the SAGE26 commit it read and cites `path:line` in that clone for every claim about SAGE26.
- [ ] It specifies the dataset as SAGE26 must see it: file naming and range, the header, the 88-byte record with every field's offset, type and unit (masses float32 Msun/h, `Spin` already per unit mass, `Len` stored), the forest order and the within-forest rank order, the link semantics (forest-local, −1 none, `FirstHaloInFOFgroup` self for a central), the file cut rule and caps, and the identity multiplier; it states what SAGE26's `lhalo_binary` reader would do with the file unchanged (read the 104-byte record and misinterpret everything) so that nobody tries.
- [ ] It specifies the reader to add: a `TreeType` value, the record table, the mass scale to SAGE26's 1e10 Msun/h, the derivation of the fields SAGE26's `halo_data` has and the dataset lacks (`M_Mean200`, `M_TopHat`, `FileNr`, `SubhaloIndex`, `SubHalfMass`) with the value each should take and why, the forest distribution setting, and the parameter-file keys.
- [ ] It states SAGE26's own limits at this scale with their sources: the galaxy-index scheme and the caps the file plan keeps it inside, the per-forest `GALAXY` allocation and its super-forest memory consequence (an estimate, labelled), int32 forest-local indices.
- [ ] It states how SAGE26's output relates to Mimic's identity: SAGE26's `GalaxyIndex` never equals `UniqueGalaxyID`; `SAGETreeIndex` is the forest within its file, so Mimic's forest number is the file plan's prefix for that file plus it; `SAGEHaloIndex` is the galaxy's current host halo's within-forest index, overwritten as the galaxy is inherited, so (file, `SAGETreeIndex`, `SAGEHaloIndex`) gives a correspondence between host halos at an output snapshot and not the persistent birth-halo component of Mimic's id; a galaxy-identity join needs a birth or lineage mapping stated for both codes, with `MostBoundID` as data; and what a comparison of SAGE26 against `sage16` can and cannot claim (`sage16` is near-bit-parity with SAGE 2016, not with SAGE26; the comparison is scientific, not an identity).
- [ ] It names the validation SAGE26 should run first: the subset conversion of Slice 6, whose halo totals and forest counts are recorded, before the production dataset.
- [ ] `make check-docs` passes.

### Authorized Surface

- Files allowed to change:
  - `docs/dev/SAGE26-SHIN-UCHUU-LHALO-READER-BRIEF.md` (new)
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
- Functions/classes/components allowed to change: prose; in the pathway only the inventory row for the brief.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code in either repository; no run of SAGE26; no claim about SAGE26 behaviour beyond what its source states.

### Risk Flags

- Risky surfaces touched: none.
- Approval needed before implementation: no
- Independent audit required: no
- Difficulty: moderate

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: a Reviewer checks three cited SAGE26 anchors against the clone.

### Rollback Path

- Revert the slice commit.

---

## The production procedure (owner-operated, between runs 2 and 3)

Not a PM slice: the owner runs it with an assistant session in Mode A, step by step, on the Mac Studio, under the v3 procedure's discipline (log root with logs, scripts, runs, bin, a ledger and a trash log; every step under `MallocLargeCache=0 /usr/bin/time -l` through the step wrapper; every long step detached under `caffeinate -i`; binaries built once at the procedure's commit on a clean tree and copied out with their SHA-256; one LaCie-bound job at a time; the record written between steps). Its evidence goes into the acceptance record's "Production" section as it happens; a step whose predicate fails stops the procedure. The session edits only the acceptance record, `HANDOFF.md` (gitignored) and the gitignored `snapshots` symlink and the package's `last_file` value in `simulation_info.yaml` (step 7, a one-line committed change the record names).

1. **Preconditions and ledger.** Run 2 accepted; `make tests-converter` and the package tiers pass at the procedure's commit; the cut dataset, the reference output and its `metadata/` resolve; the production `file_plan.json` of Slice 6 is reproduced by `plan` on the production dataset (identical md5); the ledger measures everything retained on both volumes and recomputes every step's peak and headroom from the rehearsal's measured coefficients (bytes per halo written, read rate per worker, RSS per worker); the free space the ledger requires is available or the procedure stops for the owner. The llama router's state is recorded.
2. **Reconciliation.** Compare the reference run's `metadata/` (build, modules, parameters, `SubSteps`, output fields, output snapshots) with `models/sage16/input/sage16_shin-uchuu-lhalo.yaml` at the procedure's commit, and the code: `git diff --stat b4903283 <procedure commit> -- src models/sage16` may touch only the horizontal reader and driver, files this plan adds, and changes whose effect on the vertical `sage16` path the session shows to be none (the v1.5 closeout's), with the build flags matching the reference's (`cc -O2`, no MPI unless step 6 chooses it, recorded); any other difference stops for the owner's decision, who distinguishes the expected build and package metadata differences from scientific configuration drift. The procedure's run file is the committed `sage16_shin-uchuu-lhalo.yaml` with an `input` section naming the dataset directory and the plan's full file range, `output.snapshot_list` set to the reference's ten snapshots and the output directory on the LaCie; step 3's variants, one per model from each model's committed run file, name file 0 alone.
3. **File 0 alone.** The llama router is stopped and 300 GB confirmed free. `convert --only-files 0 --tree-name shin-uchuu-cut` into the dataset directory (the super-forest, 11.1 GB); then `halos-only` and `sage16` vertical runs on file 0 alone through their step-3 variants, recording wall time, peak RSS and the output population: the super-forest's measured cost, against the 166 to 250 GB projection. A peak above the ledger's allowance, or a wall time above the owner's stop rule, stops for the owner before the whole conversion.
4. **The conversion.** `convert` with the same options for every remaining file at the rehearsal-chosen worker count; record per-file and total wall time, the peak RSS of the worker set, read and write rates and the LaCie's free space at the peak; the manifest's totals must equal the dataset's (244,953,607 forests, 22,503,649,037 halos) and every file's SHA-256 is recorded. A verification failure stops the procedure.
5. **The dataset is the run's.** No separate check of the dataset precedes the run: the per-file write-verify of step 4, the reader's own startup scan logged by step 6 (the forest total, 244,953,607, and the largest unit, 126,380,326, which the record quotes from that log) and the identity of step 8 are the proof; the dump harness has no forest subset and the whole-dataset dump is out of reach at this scale.
6. **The vertical `sage16` run.** The llama router stopped. Serial unless the ledger's projection exceeds 24 h, in which case `mpirun` at the rank count the ledger allows (the owner's call, recorded, with the MPI build's flags recorded against step 2), with `--skip` available for a restart; record wall time, peak RSS (per rank if several), the output population per output snapshot and the partition count.
7. **The package goes to production.** `simulations/shin-uchuu-lhalo/snapshots` points at the dataset; `simulation_info.yaml`'s `last_file` becomes the plan's last file (committed on the branch, with the record naming the commit, before run 3's `init`); the package's unit tests and the real-dataset half of any test that reads `snapshots` run and pass.
8. **The comparison.** The llama router stopped. `scripts/compare_cross_format_identity.py` in its default mode, the reference as left and the vertical run as right, redirected to a file; required: exit 0, identical `UniqueGalaxyID` sets and every field byte-identical at every one of the ten output snapshots, with the galaxy count per snapshot recorded (313,753,033 at z = 0). Any difference stops the procedure and is a defect for the owner, not a tolerance.
9. **Science checks (optional).** Identity in step 8 implies the z = 0 table of `simulations/shin-uchuu/README.md`; if the owner wants it, the table is recomputed from the vertical output by the same script the v3 procedure used and the record states the values as measured.
10. **Settle.** The owner's transfers: the vertical dataset (with `file_plan.json` and `manifest.json`) and the vertical output to NT, checksummed; the record notes every directory's disposition; nothing local is deleted in this plan (the owner decides later, with NT copies verified, what stays).

---

## Slice 9: Provenance, record and pathway after the procedure

### Intended Change

- Recommended Developer: Claude Sonnet 5.5; effort: high. Requires the procedure's record.
- The documentation of record carries the production evidence: the package README's provenance, the acceptance record's closing, the changelog's claims and the pathway's closeout.

### Acceptance Criteria

- [ ] `simulations/shin-uchuu-lhalo/README.md` gains "How the production dataset was made": the input dataset's identity, the converter commit, the caps and file count, the record, the totals, and the identity result (the `sage16` vertical run identical to the cut horizontal run in every field over ten output snapshots, with the galaxy counts), phrased as history and user-facing; the memory the largest forest needs under the vertical driver is stated as a property of the package (the measured peak, as a requirement to run it), and the run's wall time stays in the acceptance record.
- [ ] `docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-ACCEPTANCE.md`'s "Production" section is complete and every number in it traces to a named log; a Reviewer trace is recorded.
- [ ] `CHANGELOG.md` Unreleased states the production claim exactly (identity for `sage16` at production scale; `halos-only` and `sage16` at subset scale; no `hod` or `sham` claim; the created-record limit); `.agents/skills/mimic-docs-and-writing/SKILL.md`'s external-claims list says the same; `.agents/skills/mimic-simulations-and-readers/SKILL.md`'s Shin-Uchuu paragraph says the cut box runs vertical through this package.
- [ ] `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`: the next-focus box records the plan complete with the measured outcomes, the inventory marks this plan for archiving on merge and the acceptance record as standing evidence, and the candidate for the performance step (sizing the vertical output-buffer seed from a measured ratio) is recorded under residuals with the super-forest figure.
- [ ] `make check-docs` passes; every added line outside `docs/dev/` follows the user-facing convention.

### Authorized Surface

- Files allowed to change:
  - `simulations/shin-uchuu-lhalo/README.md`
  - `docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-ACCEPTANCE.md`
  - `CHANGELOG.md`
  - `.agents/skills/mimic-docs-and-writing/SKILL.md`
  - `.agents/skills/mimic-simulations-and-readers/SKILL.md`
  - `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`
- Functions/classes/components allowed to change: prose only.
- Tests allowed or expected to change: none.

### Explicit Non-Goals

- No code change; no new claim beyond the record's measurements.

### Risk Flags

- Risky surfaces touched: none.
- Approval needed before implementation: yes
- Independent audit required: no
- Difficulty: easy

### Validation Plan

- Tests to add/update: none.
- Commands to run: `make check-docs`; `./scripts/beautify.sh`; `make check-format`.
- Lint (differential, via the `lint` skill): required unless the slice changes no linted file.
- Manual checks: the Reviewer trace of the record.

### Rollback Path

- Revert the slice commit.

---

## Final-State Inventory

**Added:** `convert/mimic-convert/write_vertical_trees.py`, `convert/mimic-convert/vertical/`, their four test modules and additive fixtures; `simulations/shin-uchuu-lhalo/` with its fixture, tests and gate; two run files; the parity harness's `vertical_dataset` option with its diff rule, `header_source_format` pin and manifest-proved inventory; `docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-ACCEPTANCE.md` (standing evidence); `docs/dev/SAGE26-SHIN-UCHUU-LHALO-READER-BRIEF.md` (a brief for another repository; archived when that work is done or dropped).

**Changed:** `run_generalisation_acceptance.py`'s `compare` gains `--dataset-source-format`; the guides, manual, skills, changelog, the two sibling package READMEs, the pathway; `simulations/shin-uchuu-lhalo/simulation_info.yaml`'s `last_file` at step 7.

**Unchanged by design:** every reader under `src/io/`, the vertical driver and its seed, the generator, the format document, `convert_trees.py`, `rewrite_trees.py`, `forest_census.py`, the comparator, every other package and gate.

**Data:** the cut horizontal dataset is read only and never moved; the vertical dataset and the vertical output live on the LaCie with NT copies after step 10; the Stage R subsets, the subset ASCII and the converted subset stay; nothing is deleted in this plan.

**On merge:** this plan is archived to `archive/dev-plans/`; the acceptance record stays in `docs/dev/`.

---

## Next Chat Prompts

### Mode A: Assisted run (checkpointed alternative)

```text
Plan file: docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-IMPLEMENTATION-PLAN.md
Slices this session: Slice 1

Read the full plan. Stop before coding if any receipt is incomplete or baseline drift
changes its meaning. Work on feature/shin-uchuu-cut-vertical; create it from main if it
does not exist and tell me. Use orchestrator as the controlling skill. Act as the
Developer: implementation, validation, Git and commits stay local. Use the slice's
recommended Developer model and effort and fresh read-only Reviewers (drift audit Claude
Sonnet 5.5 high; code review Claude Fable 5.1 high and Codex gpt-6.1-sol high, in
parallel, staggered). Do not substitute or self-audit.

For the selected slice: restate the frozen contract; stop for my approval if its Risk
Flags say approval is needed; apply scoped-implementation; apply drift-audit through a
Reviewer and report the authorization result before any quality review; on a passing
gate apply code-review through the Reviewers; fix findings as one batch and re-run the
relevant gate; ask me before committing; commit with the commit skill; then use the
handoff skill to record state and the next slice.

Confirm the plan, selected slice, branch and model/effort before beginning.
```

### Mode B: Supervised execution (run 1, Slices 1 to 5)

```text
Plan file: docs/dev/MIMIC-SHIN-UCHUU-CUT-VERTICAL-IMPLEMENTATION-PLAN.md (revision 1)
Repo: /Users/dcroton/Local/git-repos/mimic

If it exists, start by reading the full HANDOFF.md closely. If none exists, create one.

Harness/models:
- Developer: claude, at the model and effort the plan's profiles table gives the slice,
  passed explicitly to start-slice: Slice 1 Opus 5.5 high; Slice 2 Fable 5.1 high;
  Slice 3 Opus 5.5 high; Slice 4 Opus 5.5 high; Slice 5 Opus 5.5 high
- Drift audit: claude sonnet 5.5 on high effort
- Code reviewer 1: claude fable 5.1 on high effort
- Code reviewer 2: codex gpt-6.1-sol on high effort (always pass high; the codex config
  defaults to low)
Employ all listed reviewers as a review panel: run the drift audit first, then the code
reviews in parallel, starting about 90 s apart, with one identical adjudications file.
Complete all reviews before steering. Keep follow-up reviews tight and focused; be
sensitive to findings that introduce bloat, overtesting or low value. All work must be
vision aligned (docs/VISION.md) and style guide aligned (docs/STYLE-GUIDE.md), and every
line added outside docs/dev/ must follow the plan's user-facing documentation convention.

Use project-manager. You are the accountable PM and never write slice code. Read the
complete frozen plan; run check-plan with repo context. Require a clean committed
planning baseline on main and resolve any drift against the plan's anchors first. Init
with --branch feature/shin-uchuu-cut-vertical (create it); never run on main. This run is
Slices 1 to 5; stop the run after Slice 5 is decided. Record human approval for Slices
2, 3 and 4 before each starts; neither this launcher nor the plan grants it. Ask for
commit authorization before launch unless I have already explicitly granted it; no
session pushes. Keep PM_RUN_TOKEN private to the PM seat.

Preflight, without changing source data: make; mimic_venv with h5py and numpy; HDF5;
make tests-converter (1550 cases at the baseline), make tests-horizontal-v3 and the
default pair's tiers through a subagent; simulations/shin-uchuu/snapshots resolves to
the cut dataset; record LaCie and Internal free space.

For each slice in order:
1. Launch a fresh Developer with the receipt's model and effort. Give it the plan's
   Frozen Facts section as notes, the trash rule (never rm; trash, and log each entry
   with its size), and MallocLargeCache=0 for any measurement.
2. Wait with one long observe --wait rather than repeated checks.
3. Check the mechanical floor, then the frozen contract and the actual evidence; run
   lint yourself; rerun the slice's validation commands where risk or doubt warrants
   (make tests-converter through a subagent at minimum for Slices 1 to 3).
4. For elevated slices commission the panel as above, both reviews fresh at the exact
   final commit. After every review, check that HEAD is still on the branch. Record
   reviewer and developer judgments.
5. A contract defect stops the run unless the plan itself resolves it; do not amend
   this plan. Never waive a gate leg.

At the end, update HANDOFF.md so it is the sole entry point, and add the run-2 launcher
(Slices 6 to 8: Slice 6 Opus 5.5 high, Slice 7 Sonnet 5.5 high, Slice 8 Opus 5.5 medium;
Slice 6 needs approval and about 600 GB of LaCie scratch). Confirm the plan, branch,
models and effort, PM_RUN_TOKEN, approvals and the first slice. Warn me of the expected
resource load before each slice. After the run, report its verified total elapsed time,
accepted commits and evidence, audit provenance, plan defects, stops and residual
limitations.
```
