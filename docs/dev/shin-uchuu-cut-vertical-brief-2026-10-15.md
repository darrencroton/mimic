# Brief: a cut, vertical Shin-Uchuu for Mimic (updated Fri 16 Oct 2026, 18:10 AEDT; first written Thu 15 Oct 17:00)

A planning brief for a fresh session. It is a follow-on to the Shin-Uchuu v3 plan (`MIMIC-SHIN-UCHUU-V3-IMPLEMENTATION-PLAN.md` (archived to `archive/dev-plans/`), branch `feature/shin-uchuu-v3`), not part of it. Figures marked *estimate* are reasoned, not measured; everything else comes from the v3 production procedure's record (`docs/dev/MIMIC-SHIN-UCHUU-V3-ACCEPTANCE.md`, Stage C section) and its logs. The same intent is summarised in the repo's `HANDOFF.md` ("FOLLOW-ON INTENT") so the sessions finishing v3 know about it.

## The question and the owner's direction

Produce a **cut** version of the Shin-Uchuu merger trees in a **vertical** (per-forest) format, using the same cut as v3 (the decided table, which severs essentially all flyby links), and run `sage16` on it with Mimic's vertical driver.

Owner direction (15 Oct):
- Do it **after** the v3 plan is complete, verified and merged, as its own plan.
- Do the conversion **here on the Mac Studio** (512 GB RAM, 32 cores) if the disk space can be managed.
- **NT is available as a holding pen** for data, so that nothing puts the v3 work at risk.
- **Be conservative with deletions** until the direction of this plan is settled: nothing that might be useful is deleted without a verified NT copy.

## Short answers

1. **Possible: yes.** The cut is a topology change (which halos form a forest and how they link), independent of layout. The validated cut dataset already exists in horizontal v3 form.
2. **Hardware: the Mac Studio, not a 16 GiB laptop.** A vertical run holds one whole forest at a time; the largest cut forest has 126,380,326 halos over all snapshots, so the floor is *estimate* 60–150 GB, well inside 512 GB. The horizontal cut run peaked at only 6.17 GB because it works one snapshot at a time (the largest forest peaks at 3,415,844 rows at snapshot 31). MPI does not help the largest forest, which also sets the wall time. The cut still cuts the vertical floor to about 38% of the uncut forest's (333,663,215 halos).
3. **Effort: far less than v3.** v3 built the format, rewriter, census, cut decision and bounded battery; all of that is done. This is *estimate* one or two slices (a converter or reader plus tests), a production conversion (*estimate* 10–20 h, I/O bound: one full read of the cut dataset took about 8.75 h), a `sage16` vertical run and a comparison. Days, not weeks.
4. **Retained data helps a lot**, and all of it is being kept, now with NT copies (see the tables below).

## Two routes to weigh (the disk budget makes this a real choice)

**Route A: convert to an existing vertical format (first recommendation).** Mimic's vertical `tree_type`s are LHaloTree binary/HDF5 and consistent-trees ASCII/HDF5; consistent-trees HDF5 (as the `micro-uchuu-hdf5` package uses) is the likely target, but choose on evidence.
- Each snapshot slab of the cut dataset is sorted by `ForestIndex` (forest-blocked; the battery checks this), so one **streaming k-way pass over the 71 slabs in `ForestIndex` order** emits each forest whole, in sequence. No global sort and no scratch space.
- Links must be re-expressed as the vertical format's within-tree indices (descendant, first/next progenitor, FoF first/next). That mapping is the converter's real work and needs adversarial tests (orphans, gapped descendants, promoted halos, the super-forest's kept piece).
- A new simulation package (e.g. `simulations/shin-uchuu-cut-vertical/`; name to decide) points at the result.
- **Cost: a second ~3 TB dataset on the LaCie** (see the disk plan).

**Route B: no copy at all; a vertical reader that reads forests straight from the forest-blocked v3 slabs.** Because every slab is forest-blocked, a reader can keep one cursor per slab and assemble forest k from the next block of each slab, in `ForestIndex` order. This removes the 3 TB copy and the conversion run, which makes the Mac Studio disk plan easy. Costs: C reader work in Mimic's I/O layer (and the reader/driver pairing rules), and 71 interleaved read streams on a spinning disk (needs large per-slab buffers; performance unmeasured). Worth a short feasibility check in the fresh session before committing to Route A.

**Not recommended for production: applying the cut table to the original consistent-trees ASCII.** The full production ASCII is not local, and this redoes identity bookkeeping the rewriter already did and verified. It is, however, a good **independent cross-check on the subset** (see "Test bed").

## Disk plan for the Mac Studio (Route A)

| | LaCie (8.0 TB) | Internal |
|---|---|---|
| Free now (Thu 15 Oct, during step 12c) | 0.88 TB | 0.49 TB |
| Freed when step 12c's NT copies verify (uncut dataset 3.16, census 0.27, run 2 retired 0.04) | +3.47 TB → **about 4.35 TB free** | — |
| Kept after v3 (not deleted) | cut dataset 3.16 TB + workdir 0.03 + table 0.01; Stage R subsets 0.16; subset ASCII 0.21; index files 0.02 | cut `sage16` output 0.54 TB |
| Cut-vertical needs | vertical dataset *estimate* about 3.2 TB (2.5–4 by format); subset development about 0.1 TB; vertical `sage16` output about 0.54 TB (same snapshot list) | — |
| Headroom | *estimate* about 0.5 TB | |

- It fits, but tightly. The vertical `sage16` output goes to the LaCie, because Internal cannot hold a second 0.54 TB output beside the horizontal reference, and the comparison needs both locally.
- **If the vertical format turns out larger**, use NT as the holding pen, in this order, each a verified copy before any local deletion: (1) the Stage R subsets and subset ASCII (0.37 TB), once the converter's subset tests are done; (2) if still short, switch to Route B rather than squeeze the LaCie. Never delete the cut horizontal dataset: it is production and the input.
- Route B needs none of this.

## Acceptance idea

- `sage16` vertical on the cut vertical data should reproduce the **cut horizontal `sage16` run galaxy for galaxy** (Stage R's subset gate already showed vertical and horizontal paths agree on the subset). Compare with `scripts/compare_cross_format_identity.py` (fixed in Stage C to fit production scale: about 380 GB peak at this size).
- Converter or reader checks: forest count 244,953,607; per-forest halo totals equal the cut dataset's; the largest forest 126,380,326 halos (ForestID 26,551,468,179); every link resolves inside its forest.
- Record measured peak RSS and wall time for the vertical run.

## First questions for the fresh session

1. **Memory per halo of vertical `sage16` on a large forest**, measured on the subset (or a synthetic large forest), extrapolated to 126.4 M halos.
2. **Can the chosen vertical reader hold a 126 M-halo forest?** Check index types, every count and offset, `MAX_HALO_ARRAY_SIZE`-style limits, and any per-forest allocation that assumes small forests.
3. **Route A or B?** A short feasibility check of Route B (read throughput of 71 interleaved slab cursors on the LaCie; size of the reader change). For Route A: which format, its fields vs. `simulations/shin-uchuu/halo_properties.yaml`, file layout, and its size per halo (this sets the disk plan).
4. **Run time of the largest forest** under the vertical driver (serial; dominates).
5. **Machine memory for the heavy steps:** the vertical `sage16` run (*estimate* 60–150 GB) and above all the galaxy-by-galaxy comparison (about 380 GB measured at this scale in v3 step 10) need the llama server stopped (`sudo launchctl bootout system/com.dcroton.llama-router`; restart with `sudo launchctl bootstrap system /Library/LaunchDaemons/com.dcroton.llama-router.plist`). Development on the subset does not.

## Where everything is (after v3 step 12 and the extra NT copies)

NT root: `nt.swin.edu.au:/fred/oz214/dcroton/shin-uchuu/`. Every NT copy is checked on the login node (`md5sum -c` / `sha256sum -c`); the check outputs are under `verify/`. NT quota: about 21.9 TB free before step 12c, *estimate* about 14.7 TB after all copies below.

| Item | Size | Local | NT copy | For cut-vertical |
|---|---|---|---|---|
| **Cut v3 dataset (production)**, 71 files | 3.16 TB | LaCie `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-c/cut/dataset` (the `shin-uchuu` package symlink points here) | `snapshot-trees-v3-cut/` (step 12e; sha256 of all 71 files against the cut manifest; `snapshot_016.h5` hit one transient NT read error and was re-verified, including a cold read that bypassed the cache) | **The input** |
| Cut conversion workdir: `manifest.json`, `conversion_report.{json,txt}`, `rewrite_summary.json`, `rewrite_state.json`, `tree_inventory.npy` (18 GB), `cut_pieces.npy` (4.8 GB) | about 23 GB | LaCie `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-c/cut/` | `snapshot-trees-v3-cut/conversion-provenance/` (step 12e, md5) | Forest sizes, provenance, battery report |
| Cut table: `forests.list` 7.6 GB, `record.json` 3.0 GB, `summary.json` (md5-pinned at step 8) | 10.6 GB | LaCie `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-c/cut-table/` | `snapshot-trees-v3-cut/conversion-provenance/cut-table/` (step 12e) and `shin-uchuu-census/census/table/` (step 12c) | Provenance; the ASCII cross-check route |
| **Cut `sage16` output** (horizontal, 17 files) | 0.54 TB | Internal `/Volumes/Internal/results/mimic/sage16-shin-uchuu-v3-cut/` | `sage16-shin-uchuu-v3-cut/` (step 12e, md5 17/17) | **The identity reference** |
| Procedure folder: frozen `sage16` binary (b4903283, SHA-256 `0219e514…`), every step log, scripts, checksum lists, comparator reports | 2.6 GB | Internal `/Volumes/Internal/results/mimic/shin-uchuu-v3-stage-c/procedure/` | `shin-uchuu-stage-c-procedure/` (copied 16 Oct, md5 2,184/2,184) | Reproducibility |
| Stage R subsets: `cut` (54 GB, the subset already cut), `cut-table` (232 MB), `migrated` (54 GB), `v2-subset` (39 GB), `ref-manifest` | 0.16 TB | LaCie `/Volumes/LaCie/data/uchuu/shin-uchuu-stage-r/` | none (copy first if the space is needed) | **Test bed: the converter's development input** |
| Subset ASCII (2,744 tree files, 406,668,896 halos, 6,011,205 forests) | 0.21 TB | LaCie `/Volumes/LaCie/data/uchuu/shin-uchuu-subset-ascii/` | none (copy first if the space is needed) | **Test bed: independent cut-vertical route** |
| Index files (`forests.list`, `locations.dat`) | 20 GB | LaCie `/Volumes/LaCie/data/uchuu/shin-uchuu-nt-transfer/` (untouched) | the originals came from NT | Only for the ASCII route |
| Uncut v3 dataset + provenance | 3.16 TB | deleted after its NT check (step 12c) | `snapshot-trees-v3-uncut/` (+ `conversion-provenance/`) | No |
| Uncut `sage16` output | 0.54 TB | deleted (verified first) | `sage16-shin-uchuu-v3-uncut/` (md5 17/17) | No |
| v2 dataset (+ provenance, also kept on the LaCie at `shin-uchuu/conversion-provenance/`) | 2.25 TB | provenance only | `snapshot-trees-v2/` (md5 71/71) | No |
| v2 `sage16` output | 0.54 TB | deleted (verified first) | `sage16-shin-uchuu-v2/` (md5 184/184) | No |
| Census aggregates (occupancy, partition, trees, table) and run 2's retired outputs | 0.28 TB | deleted after their NT check (step 12c) | `shin-uchuu-census/census/` and `shin-uchuu-census/run2-retired/` | Planning only |
| Original full consistent-trees ASCII | — | not local | upstream source (location not checked here) | Only for the ASCII route |
| Acceptance and census records | small | git, `docs/dev/` | — | The documentation of every number here |

The extra NT copies (step 12e, owner-authorized 15 Oct) are **done and verified** (Fri 16 Oct), and the procedure folder was copied last. Note: every NT copy has group `cas`, not `oz214`, so none counts against the oz214 quota (about 5.25 TB on disk, Lustre-compressed); a `chgrp` is the owner's call.

## Things the v3 sessions must not undo (also in `HANDOFF.md`)

Runs 5 (Slice 18) and 6 (Stage D) should raise with the owner any decision that would remove or change: the vertical readers under `src/io/vertical/`; the `shin-uchuu-ascii` and `micro-uchuu-hdf5` packages; `scripts/compare_cross_format_identity.py`; the v3 bounded reader (`horizontal_dataset.py`) and the format's forest-blocking invariant; `tests/tools/transform_ascii_fixture.py` and `convert_trees.py ingest --cut-table`; or any data above. None of these is slated for removal in the plan's Final-State Inventory (Stage D removes the version 2 horizontal reader, converter, validator, crosscheck and fixtures); this is a guard, not a known conflict. At plan close, the owner's answer on "the fate of the Stage R subset data" is: **keep** (or NT first).

## Context pointers

- Cut figures: 244,953,607 forests (one per z = 0 FoF group); 481,296,108 promotions; 254,554 progenitor-order changes; largest forest 126,380,326 halos, peak 3,415,844 rows at snapshot 31; 8,053,562 forests split from the uncut set.
- Cut `sage16` (horizontal, 1 task, 128 chunks): 12.3 h, peak RSS 6,170,116,096 B; 313,753,033 galaxies at z = 0 (244,953,607 centrals).
- Uncut `sage16` (horizontal, 1 task, 2 chunks): 360.6 GB peak; 2,058,518,068 galaxies bitwise identical to version 2.
- Live state and paths: `HANDOFF.md` (repo root, gitignored); v3 timeline: `shin-uchuu-v3-timeline-2026-10-12.md` in this folder.
