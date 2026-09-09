# Shin-Uchuu Flyby Defect — Addendum to the Conversion Plan

**Status:** Open. This is the remediation plan for a scientific-correctness defect found in the Shin-Uchuu production dataset during P8's science-validation pass.
**Date:** 2026-09-09.
**Precedence.** This addendum's R1–R15 sequence is **the plan of record and supersedes the outcome status of [`SHIN-UCHUU-CONVERSION-PLAN.md`](SHIN-UCHUU-CONVERSION-PLAN.md)'s P1–P9** — that document's Definition of Done, and its records of P6, P7 and P8 as complete, are **historical v1 evidence, not current completion**. What that document still owns, and what you should use it for, is unchanged and load-bearing: the measured source data, the conversion algorithm, the storage and memory envelopes, the flag semantics and footguns, the operational detail of every stage, and its reusable validation snippets. The route this addendum takes is deliberately the same route it describes, so those details apply directly rather than by translation.
**Entry point:** the live root `HANDOFF.md`. Start there, not here.

---

## Table of Contents

1. [Executive Summary](#executive-summary)
2. [The Defect](#the-defect)
3. [Measured Evidence](#measured-evidence)
4. [Blast Radius](#blast-radius)
5. [Why This Was Not Caught Earlier](#why-this-was-not-caught-earlier)
6. [What Survives On Disk, And What Does Not](#what-survives-on-disk-and-what-does-not)
7. [Decisions](#decisions)
8. [The Remediation Sequence](#the-remediation-sequence)
9. [Cost, Memory And Time](#cost-memory-and-time)
10. [Process Improvements For This Round](#process-improvements-for-this-round)
11. [Independent Review](#independent-review)
12. [Definition Of Done](#definition-of-done)

---

## Executive Summary

The Shin-Uchuu production snapshot dataset (`/Volumes/LaCie/data/shin-uchuu/production-snapshot`, 70 files, 2.25 TB, 22,503,649,037 halos) is **structurally wrong at z=0 and correct at every other snapshot**. One file — `snapshot_069.h5` — carries the defect.

The cause is `fix_flybys()`, a semantic convention inherited from `sage-model`'s Consistent-Trees reader and faithfully replicated by our converter. At each forest's maximum snapshot it demotes every FoF central except the single most massive one to a satellite of that survivor. On a coarse catalog this is a documented, tolerated ~10% nuisance. On Shin-Uchuu, whose forests percolate at 8.97 × 10⁵ Msun/h resolution, it collapses **one forest containing 103,362,317 of the 313,317,969 z=0 galaxies — 33.0% of the box — into a single FoF group**, and reduces the entire z=0 central population to exactly one halo per forest.

Consequences: the z=0 Type-0 halo mass function is truncated by ~2 dex; `BaryonFraction`, `HaloOccupation`, `HaloMassFunction` and six other z=0 figures are invalid; and the *physics* of the final timestep is corrupted, because `sage16`'s infall budget subtracts a whole forest's baryons from one halo's `f_b × Mvir` and then strips the survivor's gas to compensate.

**The finding is the validation process working.** P8 exists to catch exactly this, and it did, before any science was published from the run.

The remediation is: **delete `fix_flybys` from Mimic, bump the snapshot format to `format_version = 2`, and re-convert Shin-Uchuu on the Mac Studio by exactly the route that worked the first time.** About eight days end to end, of which roughly six is unattended local conversion; the `sage16` run itself still goes to NT, as it did before, because its ≈639–697 GB peak does not fit this host. Two things must be settled before any code is touched: the volume has to be prepared (the v1 dataset moves to `/Volumes/Scratch` to clear the 7.0 TB cold-start floor), and the owner has to decide whether this re-conversion should also start keeping additional Consistent-Trees source columns — the only moment that can be done without paying for a re-conversion of its own.

---

## The Defect

### Mechanism

`fix_flybys()` — `src/io/tree/ctrees/ctrees_utils.c:318-411`, called from `src/io/tree/read_ctrees_ascii.c:693`:

1. Heap-sorts the forest by `(scale desc, id asc)` and identifies the block of halos at the forest's maximum scale (`:334-345`).
2. If more than one of them has `pid == -1` (i.e. more than one independent FoF group survives to that snapshot), it picks the most massive by `Mvir` (`:370-379`).
3. For every *other* halo in that block it sets `info[i].upid = fof_id` (`:408` — note this line sits **outside** the `if (pid == -1)` guard, so genuine subhalos are re-pointed too), and for those with `pid == -1` it additionally sets `pid = fof_id` and negates `MostBoundID` as a marker (`:395-408`).

`fix_upid()` (`:414-508`) then resolves every satellite chain to the ultimate central and overwrites both `pid` and `upid` (`:501-502`). `assign_mergertree_indices()` (`:511+`) builds `FirstHaloInFOFgroup`/`NextHaloInFOFgroup` from the result.

Net effect: **at each forest's final snapshot the whole forest becomes one FoF group.**

Consistent-Trees forests are rooted at z=0 by construction, so for this dataset *every* forest's maximum snapshot is 69. The defect is therefore confined to a single snapshot — confirmed by measurement, §3.3.

### Which packages are affected

| package | `tree_type` | applies `fix_flybys`? |
|---|---|---|
| `mini-millennium`, `millennium`, `micro-uchuu`, `mini-uchuu` | `lhalo_binary` | no |
| `micro-uchuu-hdf5`, `uchuu` | `consistent_trees_hdf5` | no — reads uchuutools' pre-stored topology |
| **`micro-uchuu-ascii`, `shin-uchuu-ascii`** | `consistent_trees_ascii` | **yes**, at read time |
| **`shin-uchuu`, `micro-uchuu-snapshot`** | `snapshot_hdf5` | **yes**, baked in at conversion by `scripts/convert/fixups.py:262` |

**Four shipped packages are exposed**, not two — two directly and two by inheritance. Only `micro-uchuu-ascii` and `shin-uchuu` have been *measured*, because they are the ones with production output on disk; `shin-uchuu-ascii` and `micro-uchuu-snapshot` carry the same defect and must be treated the same way.

The converter replicates the demotion deliberately: `docs/dev/SNAPSHOT-HDF5-FORMAT.md` → Ordering Contracts item 5 makes the flyby marker part of the frozen `format_version = 1` contract, and defines "reference" as the ASCII reader's semantics. The cross-format identity gate therefore *certifies* the defect rather than catching it.

### Why nothing in Mimic requires it

`src/io/tree/ctrees/ctrees_utils.h:8-17` records the helper's provenance: ported from `sage-model` with minimal edits, to reconstruct L-Halo-tree pointers. `simulations/micro-uchuu-ascii/README.md:37` records the corresponding fact about L-Halo production — one tree per z=0 FoF root. Taken together the intent is clear enough: an LHaloTree "tree" is shaped as a single z=0 FoF group, and the ctrees→LHaloTree conversion had to produce that shape. *(The upstream `sage-model` history has not been inspected, so treat that as the documented intent rather than a proven origin story. It does not change anything below.)* **Mimic's forests are not LHaloTree trees.**

Independently verified in this repository:

- `fix_upid()` performs its own `SCALE_ID_COMPARATOR` heap sort on entry (`ctrees_utils.c:419-429`), so `fix_flybys`'s sort is not load-bearing for it.
- `assign_mergertree_indices()` re-sorts by `(scale desc, upid, pid, id)` (`:524-547`) and opens a fresh FoF chain at every `pid == -1` (`:552-570`). It handles many FoF groups per snapshot — it must, since every non-final snapshot has many.
- **The tree driver** visits every unprocessed halo in a forest (`src/core/tree_driver.c:228-250`), and `build_halo_tree()` processes each halo's actual FoF chain rather than treating the forest as one group (`src/core/build_model.c:109-129`, `:137-179`).
- **The snapshot driver** enumerates every self-central in the slab and processes each FoF chain separately (`src/core/snapshot_driver.c:912-929`).
- **The physics** requires one central per *FoF*, not per forest: `process_halo_evolution()` finds the Type-0 central within the supplied workspace and stamps its ID on the workspace members (`src/core/halo_evolution.c:149-180`), matching `docs/VISION.md:68-79`.
- The `consistent_trees_hdf5` reader delivers forests with up to 2,309 z=0 FoF centrals and both drivers process them without complaint.

So no driver, no core invariant and no physics module requires one FoF per forest. This was tested, not only reasoned about: see §3.4.

---

## Measured Evidence

### 3.1 The controlled three-reader comparison

`micro-uchuu`, `micro-uchuu-hdf5` and `micro-uchuu-ascii` are the same catalog in three formats. Measured from the `sage16` output at snapshot 49 (z=0):

| reader | forests at z=0 | Type-0 centrals | max centrals in one forest | negated `MostBoundID` |
|---|---:|---:|---:|---:|
| `lhalo_binary` | 440,651 | 496,374 | 2,309 | 0 |
| `consistent_trees_hdf5` | 440,651 | 496,374 | 2,309 | 0 |
| `consistent_trees_ascii` | 440,651 | **440,651** | **1** | 55,331 |

> The 55,331 count is **galaxy records** in the `sage16` output. `simulations/micro-uchuu-ascii/README.md:35` quotes ≈55,362 for the same phenomenon counted as **halos**; the 31-halo difference is halos that carry no galaxy record. Both are right about their own denominator. Say which one you mean when freezing a regression expectation.

### 3.2 Shin-Uchuu at snapshot 69, over all 313,317,969 galaxy records

- Exactly **one** Type-0 central per forest: 166,547,771 forests, 166,547,771 centrals, **zero** forests with more than one.
- 77,957,287 galaxies (24.9%) are flyby-demoted.
- The percolation super-forest holds **103,362,317 galaxies (33.0% of the z=0 population)** as one FoF group. Its central: `Mvir` = 1.2288 × 10¹⁵ Msun/h, `Len` = 1,369,899,600. Summed `Mvir` of its 50,435,897 flyby members is **80.6×** the central's own mass. Its computed baryon fraction is **10.9**.
- Group baryon fraction by central mass (0.25 dex bins, ≥3 centrals; `Sb` = summed group baryons, `Mc` = central `Mvir`, `SMfly` = summed `Mvir` of flyby members):

```
 logM(Msun)   Ncen   Sb/Mc   Sb/(Mc+SMfly)   <Ngrp>   <Nfly>  <SMfly/Mc>
   11.00     15015  0.1322      0.0924         168.6     95.6    0.6457
   12.00       957  0.1965      0.1029        1933.8    992.0    1.0931
   12.50       149  0.2455      0.0985        7452.1   3756.8    1.6066
   13.00        12  0.2854      0.0971       24782.0  11815.6    1.9272
```

The `BaryonFraction.png` curve reaches 0.35 against a cosmic value of 0.17.

- **The z=0 central mass function is truncated ~2 dex.** Counts per 0.25 dex (galaxy records at snapshot 69):

```
 logM(Msun)   current Type-0   corrected (Type-0 + flyby)   all halos
   12.00           957                   6726                 7624
   12.50           149                   2439                 2686
   13.00            12                    847                  922
   13.50             0                    254                  266
   14.00             0                     65                   68
   14.50             0                     13                   13
   15.00             0                      1                    1
   15.25             1                      1                    1
```

Every halo above 10^13.25 Msun except the single super-central is currently misclassified as a satellite. The corrected central population is **244,953,607 halos** (166,547,771 surviving + 78,405,836 demoted), 47% more than the dataset currently exposes.

### 3.3 The defect is confined to snapshot 69

Read directly from the production dataset (`/halos/MostBoundID`, full-array scan):

```
snap000: N=    1860842  negated MostBoundID=        0   |MostBoundID| strictly ascending = True
snap034: N=  519342987  negated MostBoundID=        0   |MostBoundID| strictly ascending = True
snap052: N=  409112176  negated MostBoundID=        0   |MostBoundID| strictly ascending = True
snap068: N=  319035008  negated MostBoundID=        0   |MostBoundID| strictly ascending = True
snap069: N=  315004242  negated MostBoundID= 78405836   |MostBoundID| strictly ascending = True
```

This matches `conversion_report.json`, whose per-snapshot `flyby_demotions` are non-zero only at snapshot 69 and total 78,405,836. The `sage16` output agrees independently: at snapshot 52, forest 1 alone carries **119,969,570 Type-0 centrals** and 14,576,846 forests carry more than one — i.e. uncollapsed.

### 3.4 The A/B experiment: removing `fix_flybys` reproduces the HDF5 reader exactly

The `fix_flybys` call at `read_ctrees_ascii.c:693` was temporarily guarded behind an environment variable, `MODEL=sage16 SIMULATION=micro-uchuu-ascii` rebuilt, and the package run end to end (1 m 53 s wall, 0.735 GB peak RSS) into `output/_exp-noflyby/`. The source patch was reverted immediately afterwards; the output directory is retained as evidence.

At snapshot 49, against `output/sage16-micro-uchuu-hdf5/`:

- galaxy count **557,519 vs 557,519** — identical
- Type counts **`[496374, 61145]` vs `[496374, 61145]`** — identical
- `MostBoundID` sets identical; zero negated `MostBoundID`

Residual per-field value differences remain, but they are a **pre-existing reader-precision difference, unrelated to flybys and unchanged by removing it**. At snapshot 16 — well before the max snapshot, where `fix_flybys` cannot act — the no-flyby run and the with-flyby run disagree with the HDF5 reader by *exactly* the same amount (508,089 field values at relative difference > 10⁻³, per-field counts identical). The driver is a ~2.2 × 10⁻⁸ relative difference in `Mvir` (float32-ULP scale; `Len` is bit-identical across readers) which `sage16` amplifies chaotically: 8.3% of galaxies differ by > 10⁻³ in `StellarMass`, 0.43% by > 10%. Removing `fix_flybys` moves the snapshot-49 disagreement from 2,449,690 such values to 1,680,425 — towards that same floor.

> **A pre-existing documentation error, found here.** `simulations/micro-uchuu-ascii/README.md` states "All snapshots before snap49 are byte-identical across the three formats." That is false for the `sage16` galaxy output, by the measurement above. It may have been true of the `RawHalo` halo data, or it may never have been checked at the galaxy level. Correct the claim; do not widen the flyby fix to chase it. **This float32-ULP reader divergence is a separate, still-open matter** — it is not caused by `fix_flybys`, is not fixed by removing it, and is not in this addendum's scope. Record it and decide separately.

---

## Blast Radius

### 4.1 Figures (z=0 only)

Nine `sage16` figures select on `Type == 0` or on group membership, and are wrong at z=0 in the current output:

| figure | how it breaks |
|---|---|
| `baryon_fraction.py` | group baryons over a whole forest ÷ one central's `Mvir` — the presenting symptom |
| `halo_mass_function.py` (`:70`) | Type-0 selection ⇒ ~2 dex truncation |
| `hmf_evolution.py` | same, at its z=0 point only |
| `halo_occupation.py` | ⟨N_sat⟩ = 4 × 10⁴ in a 10^13.3 halo |
| `quiescent_fraction.py` | uses `CentralMvir`, which for a demoted halo is the super-central's mass |
| `baryonic_tully_fisher.py`, `gas_fraction.py`, `metallicity.py`, `mass_reservoir_scatter.py` | Type-0 restriction ⇒ mass-biased sample loss, plus the §4.2 physics perturbation |

Evolution figures that sum over all galaxies without selecting on `Type` are structurally unaffected, but their z=0 point still carries the §4.2 physics perturbation.

### 4.2 The physics, not only the figures

`models/sage16/modules/sage_prepare_infall_budget/sage_prepare_infall_budget.c:110-112` computes

```c
infallingMass = central->HaloBaryonFraction * halos[central_idx].Mvir
              - (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected + tot_BHMass + tot_ICS);
```

with the sum running over every galaxy in the FoF workspace (`:68-94`). Foreign halos in the workspace drive this strongly negative, and `sage_apply_infall.c:60-91` then drains the central's `EjectedGas` to zero and eats its `HotGas` (clamped at 0).

Measured on `micro-uchuu-ascii` vs `micro-uchuu-hdf5`, restricted to the 440,651 halos that are Type 0 in **both** runs and matched by `|MostBoundID|` — ratio ascii/hdf5 of summed central reservoirs:

```
logM    HotGas  Ejected  Stellar  ColdGas    ICS
13.0     0.792    0.548    0.998    0.972   1.127
13.5     0.707    0.448    1.006    0.978   1.239
14.0     0.595    0.292    0.999    0.991   1.244
14.5     0.586    0.000    1.000    1.001   1.949
```

Surviving centrals lose up to 41% of their hot gas and all of their ejected gas. `StellarMass` and `ColdGas` are untouched because the demotion lasts one timestep, but `ICS` rises as the enlarged satellite population surrenders it to the central (`sage_prepare_infall_budget.c:99-107`).

The gas stripping is not the only physical consequence. An independent review found five more, all following from the same false workspace:

- **Cross-system mass and metal transfer.** Demoted independent centrals surrender their `EjectedGas` and `ICS` outright to an unrelated survivor (`sage_prepare_infall_budget.c:84-107`), and `sage_satellite_stripping.c:54-108` can transfer their `HotGas` and metals there too. Baryons physically belonging to one halo are booked to another.
- **Type-transition state corruption.** A formerly independent central becomes Type 1 and has `infallMvir`, `infallVvir` and `infallVmax` stamped from its previous state (`src/core/inheritance.c:64-74`) — infall quantities recorded for an infall that never happened.
- **Merger and disruption exposure.** False satellites are given merger clocks (`sage_initialise_merger_clock.c:97-133`) and can then be disrupted into, or merged with, the synthetic group's central (`sage_resolve_mergers_and_disruption.c:151-183`, `:191-228`).
- **Host-dependent physics follows the false central.** Reincorporation, supernova ejection potential and metal destination all key off it (`sage_reincorporation.c:48-76`, `sage_calculate_supernova_feedback.c:95-124`, `sage_apply_metal_enrichment.c:88-113`).
- **Demoted centrals skip Type-0 disk-radius recomputation** (`sage_set_disk_scale_radius.c:91-100`).

The scope is one timestep at each forest's final snapshot. That is small in integrated terms and total in terms of the snapshot anyone actually analyses. **It also means the existing `sage16` output cannot be salvaged by fixing the input**: the physics has already run on the false groups, so Mimic must be re-run regardless of how the input is corrected.

### 4.3 Gates, tests and documentation that assert the current behaviour

Everything below encodes the flyby convention and must move with the fix. **This list is the authorised surface for R1–R4** — it was assembled by the Developer and completed by an independent read-only review; treat an omission as a defect, not as licence to skip.

**Production implementation**

- `src/io/tree/ctrees/ctrees_utils.c`, `ctrees_utils.h` — `fix_flybys()` itself and its declaration/provenance comment.
- `src/io/tree/read_ctrees_ascii.c` — the call site (`:692-695`) and the pipeline comment (`:24-25`).
- `scripts/convert/` — `fixups.py` (`fix_flybys_snapshot`, its module docstring, `MostBoundID` negation, `verify_mostboundid_invariant`), `convert_ctrees.py`, `crosscheck.py`, `report.py`, `subset.py`, `hdf5_writer.py`.

**Input-format version and contract**

- `docs/dev/SNAPSHOT-HDF5-FORMAT.md` — Ordering Contracts item 5, Format Invariant 3's sign clause, the "reference semantics" definition (`:124`), the `MostBoundID` dataset description.
- `src/io/snapshot/read_snapshot_hdf5.c` (`SNAPSHOT_HDF5_FORMAT_VERSION` at `:62` and its four error-message uses), `src/io/snapshot/reader.h`, `simulations/micro-uchuu-snapshot/halo_properties.yaml`.

**Tests and fixtures** — note that several *assert* the defect and will fail correctly until updated:

- `scripts/convert/tests/test_fixups.py:249-282` (requires demotion and negation), `test_crosscheck.py:248-268` (requires the negative marker), plus `fixtures.py`, `test_links.py`, `test_hdf5_writer.py`, `test_subset.py`, `test_validate.py`.
- `simulations/micro-uchuu-snapshot/_tests/input/create_snapshot_fixture.py:583-615` (requires a negative id), `check_fixture_conformance.py`, `_tests/unit/test_unit_snapshot_reader_open.c`, `test_unit_snapshot_reader_realdata.c`, `_tests/data/fixture_manifest.json`, and the committed `_tests/data/snapshot_00{0..5}.h5`.
- `tests/unit/test_ctrees_support.c:220-245`, `tests/unit/tools/dump_ctrees_topology.c`. **Note the coverage gap**: the C helper test exercises only the single-FoF no-op path. There is no regression asserting that *multiple* independent FoF groups at a forest's final snapshot survive as independent. Add one — see R3.

**Documentation and status**

- `scripts/convert/README.md`, `docs/DEVELOPER-GUIDE.md`, `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`, `docs/dev/SHIN-UCHUU-CONVERSION-PLAN.md`.
- `simulations/{micro-uchuu,micro-uchuu-ascii,micro-uchuu-hdf5,micro-uchuu-snapshot,shin-uchuu,shin-uchuu-ascii}/README.md`. (`shin-uchuu-ascii`'s carries no flyby claim today — include it for completeness, since it is one of the four exposed packages.)
- Completed historical plans under `archive/dev-plans/` may keep their original v1 decisions **only if clearly marked as superseded**.

**Skills** — three of these actively instruct the reader not to fix this:

- `.agents/skills/mimic-simulations-and-readers/SKILL.md:56` — "do not 'fix' the difference away". **Wrong as of this work, and dangerous to the next agent.**
- `.agents/skills/mimic-debugging-playbook/SKILL.md:162`, `.agents/skills/mimic-failure-archaeology/SKILL.md:68` and `references/chronicle.md` — labelled "accepted".

  **All three were corrected on 2026-09-09**, with this document, so no skill still tells an agent to leave the defect alone. Each is one lean statement of the current position pointing here — not a changelog. **Revisit them at R7** once the code has actually changed, to drop the "scheduled for removal" framing and state the finished behaviour.
- `.agents/skills/mimic-docs-and-writing/SKILL.md` — flyby reference.

**The gates themselves**

- The **cross-format identity gate** (`docs/DEVELOPER-GUIDE.md:1072-1078`) compares ASCII against snapshot-HDF5 *converted from that same ASCII reference*, and the converter's topology cross-check includes the signed `MostBoundID` and the five rewritten link fields (`scripts/convert/README.md:516-522`). **Both prove parity with the reference, not scientific correctness** — that is precisely why neither caught this. Removing `fix_flybys` from both sides keeps them meaningful, but the `micro-uchuu-snapshot` fixture dataset must be regenerated first or the gate compares v1 data against a v2 reader.
- The producer battery checks index ranges and structural chain consistency, never whether the FoF grouping matches the source catalogue (`SNAPSHOT-HDF5-FORMAT.md:103-112`, `:132-136`). §10 item 9 adds the check that would have caught this.

---

## Why This Was Not Caught Earlier

Not a process failure — a correctly-scoped process meeting a regime change.

1. **The behaviour was known, characterised and accepted at micro-Uchuu scale**, where it costs ~10–25% of Type-0 halos per mass bin and the summed effect on `BaryonFraction` is a ~12% overshoot. It was documented in `simulations/micro-uchuu-ascii/README.md` as an accepted reader divergence.
2. **The scale dependence was not anticipated.** Nothing in the record connects "forests percolate harder at finer particle mass" to "the flyby fix's blast radius grows with resolution". The percolation super-forest was known — `SNAPSHOT-HDF5-FORMAT.md` even sizes `HaloRankInForest` as int64 because of it — but it was treated purely as a *memory and indexing* problem, never as a *topology semantics* problem.
3. **The converter's acceptance gate could not have caught it.** It certifies bit-exact reproduction of the reference reader's semantics. Reproducing a defective convention exactly is a pass.
4. **P8 is exactly the step that catches this, and it did.** The defect was found on the first science-validation figure examined after the run completed.

The generalisable lesson: **a gate that certifies "matches the reference implementation" cannot certify "is scientifically correct".** Both are needed, and the second one is the science-check pass.

---

## What Survives On Disk, And What Does Not

Physical inventory taken 2026-09-09.

### Present

| Artefact | Path | Size |
|---|---|---|
| Production snapshot dataset (70 + `forests.h5`) | `/Volumes/LaCie/data/shin-uchuu/production-snapshot` | 2.2520 TB (70 files = 2.2507 TB = 100.01 B/halo; `forests.h5` = 1.33 GB) |
| Same dataset, copied to NT before P6 | `/fred/oz214/dcroton/shin-uchuu/snapshot-trees/` | 2.25 TB |
| `sage16` production output (10 snapshots) | `/Volumes/LaCie/data/shin-uchuu/output/sage16-shin-uchuu` | 543.4 GB |
| Converter workdir + full `manifest.json` | `/Volumes/LaCie/convert/shin-uchuu` | 8.7 GB |
| `forest_index_table.npy`, `forest_max_snap.npy` | same | 1.3 + 2.7 GB |
| 5,488 per-source `.npy` sidecars (`roots_src_*`, `forest_max_src_*`) | `…/convert/shin-uchuu/scratch` | 4.8 GB |
| Production `forests.list` | `/Volumes/LaCie/staging/shin-uchuu/forests.list` | 7.56 GB |
| Frozen batch plan + inventory | `/Volumes/LaCie/staging/shin-uchuu/plan/` | 164 KB |
| All stage logs + `state.json` + the one-off driver | `/Volumes/LaCie/staging/shin-uchuu/{logs,state.json,run_p1_p2.py}` | 2.5 MB |
| Rehearsal ASCII subset (2,744 files, full name coverage) | `/Volumes/LaCie/data/shin-uchuu/subset-ascii` | 197 GiB |
| Rehearsal snapshot subset | `/Volumes/LaCie/data/shin-uchuu/subset-snapshot` | 38 GiB |
| **The 11.61 TB Consistent-Trees source, on NT** | `/fred/oz214/simulations/uchuu/shinuchuu/mergertrees` | 11.61 TB; 2,744 `tree_*.dat` plus `forests.list`, `locations.dat` and `locations_no_extra_columns.dat` — 2,748 directory entries. Verified intact 2026-09-09 |

### Absent

- **The 11.61 TB ASCII source, locally.** Consumed batch-by-batch during the original conversion. Only `tree_0_0_5.dat` and `tree_0_0_6.dat` (6.9 GB) plus two aborted rsync temp files survive in `staging/…/incoming`. The production `locations.dat` was never transferred.
- **Every Phase-3-and-later intermediate**, deleted by `--consume-intermediates`: `snap_NNN_sorted.bin` ×70 (the pre-fix-up state), `snap_NNN.idx` ×70, `snap_NNN_fixed.bin` ×70, `snap_NNN_links.bin` ×70, `snap_NNN_pending_fp.bin` ×69, and 192,078 worker-scratch files. The manifest records their md5, row counts and dtype tags — enough to *verify* a regenerated copy, not to *recover* one.
- `/Volumes/Scratch` (2.7 TiB = 2.97 TB) was never used and holds nothing. **It is the destination for the v1 dataset at R1** — 2.2520 TB fits with ≈0.7 TB to spare, and moving it is what clears LaCie's 7.0 TB cold-start floor.
- `/Volumes/Work Backup` (2.6 TiB used) is root-owned and could not be enumerated. **Check it before concluding the ASCII source is irrecoverable locally** — this is cheap and would not change the recommendation, but it should be ruled out on the record.

### Consequence

There is **no per-snapshot re-entry point**. The converter's own resume rules (`scripts/convert/README.md:284-320`) confirm it: with every snapshot at status `linked` and every fixed input consumed, `sort`, `fixups` and `links` all skip; only `write` and `report` are re-runnable, and they have nothing new to write. Re-converting even one snapshot means re-reading the ASCII source, and because snapshots are interleaved across every tree file, that means all 2,744 of them.

---

## Decisions

### D1 — Delete `fix_flybys` from Mimic entirely

**Decision: delete, do not make optional.**

For deleting: nothing in Mimic requires it (§2, verified by experiment in §3.4); it produces a scientifically wrong FoF topology at every scale, merely tolerably so at coarse resolution; two of the four reader paths already do not apply it and produce the correct answer; keeping it as a switch means a permanent configuration axis whose "on" setting is *known to be wrong*, plus a doubled validation surface for the converter, the identity gate and every affected package.

Against deleting: it is `sage-model` parity, and parity with the ancestor code has been a deliberate project value (`// SAGE parity:` markers are load-bearing elsewhere). Anyone reproducing a published `sage-model` ctrees result would need it. And it is the only thing that guarantees a single FoF root per forest at z=0, which some future tree-format exporter might want.

A third argument against a switch, from the independent review: a runtime option would make **the same input format mean different things depending on configuration** — a permanent catalogue-semantics axis affecting topology, identity, Types and physics simultaneously. That is at odds with `docs/VISION.md:68-79` (one coherent processing model, deterministic given a halo's own data) and `:94-104` (format-agnostic I/O whose outputs carry enough metadata to interpret a run).

The counter to all three arguments for keeping it: SAGE parity is a means to correctness, not an end. Where parity and correctness conflict — as they do here, demonstrably — correctness wins, and this repository has already made that call once, in `sage_satellite_stripping.c:85-93`, where stock SAGE's metal-destroying clamp was deliberately *not* reproduced ("That is a genuine conservation bug; Mimic fixes it"). Reproducing a published ctrees `sage-model` run is a historical-comparison exercise that would need a pinned old checkout anyway. And nothing exports LHaloTree from Mimic.

Legacy reproduction, if it is ever wanted, belongs in explicitly-legacy v1 data plus a pinned old checkout — not in a live scientific configuration.

**Blast radius: §4.3.** The format contract change is the significant one — see D3.

**Independently reviewed.** Codex `gpt-5.6-sol` (high effort, read-only) reached the same position — "Delete it from active Mimic and converter semantics. Do not add a runtime switch." Artefacts: `.orchestrator/runs/flyby-diagnosis-20260909-181215-96232/`.

### D2 — Keep `micro-uchuu-ascii`

**Decision: keep the package; it is not the problem.**

The package is not defective — the *reader semantics* were, and once `fix_flybys` is gone `micro-uchuu-ascii` produces the same halo population and Type classification as the other two readers (§3.4). Dropping it would remove: the only live exercise of the `consistent_trees_ascii` reader; the reference semantics source that the whole converter and its cross-check are defined against (`SNAPSHOT-HDF5-FORMAT.md:124`); and one leg of the three-format validation triplet that made this diagnosis possible in two minutes rather than two days. `MIMIC-DEVELOPMENT-PATHWAY.md` → Standing Constraints already says to keep it, and that constraint was right.

What *should* change: the package README's characterisation of the flyby divergence (it becomes historical), and its incorrect "byte-identical before snap49" claim (§3.4 callout).

The user's underlying point — that for *science* one micro-Uchuu is enough — is correct and unaffected: `micro-uchuu-ascii` earns its place as test infrastructure, not as a science package.

**The minimum that must exist either way** (from the independent review, and it is a stronger requirement than what exists today):

1. the `consistent_trees_ascii` reader and a tiny ASCII fixture;
2. **a fixture forest containing at least two independent FoF groups at its maximum snapshot**;
3. an assertion that both remain self-central, each retaining its own subhalos;
4. the converter's topology comparison against that corrected reader;
5. the dataset-present full micro-Uchuu identity gate as the high-volume check.

Item 2 and item 3 do not exist today — `tests/unit/test_ctrees_support.c:220-245` only exercises the single-FoF no-op path. That gap is why a unit test could never have caught this, and closing it is part of R3.

### D3 — Bump the snapshot format to `format_version = 2`

Removing the flyby demotion changes which files on disk conform. By `SNAPSHOT-HDF5-FORMAT.md`'s own rule (line 168) that is not an erratum, it is a version bump. Concretely:

- Ordering Contracts item 5 (flyby convention) is **deleted**; `MostBoundID` becomes strictly positive and Format Invariant 3's "whose sign may have been flipped by the flyby convention" clause goes with it.
- The "reference semantics" definition (line 124) must name `fix_upid()` + `assign_mergertree_indices()` and explicitly exclude `fix_flybys()`.
- `src/io/snapshot/read_snapshot_hdf5.c` must accept 2 and reject 1, so the **existing 2.25 TB dataset fails fast** rather than being silently re-used. This is a feature: it makes the bad dataset unusable by construction.
- The `micro-uchuu-snapshot` fixture dataset and its committed test fixtures must be regenerated at version 2 before the identity gate is meaningful.

**Decided by the owner 2026-09-09: reject v1 outright.** No legacy-read path. The reader accepts `format_version = 2` and refuses 1, naming the file and the version it found. That makes the defective dataset unusable by construction, which is the point — there is no v1 data worth keeping, and a legacy path would be a permanent compatibility surface maintained for data we intend to retire.

### D4 — Re-convert rather than repair in place

Both routes were assessed. See §8 for the chosen sequence and §9 for the cost of each.

The in-place repair is *technically* narrower than it first appears, and the reason is worth recording. The emitted slab is ordered by ascending `|MostBoundID|` (Format Invariant 3, `SNAPSHOT-HDF5-FORMAT.md:103-110`; `scripts/convert/links.py:37-42`), verified on all five sampled snapshot files (§3.3) — so clearing the sign does not move a row. And `HaloRankInForest` is **identity metadata, not a slab index**: `src/core/snapshot_driver.c:363-385` states that explicitly, and `Descendant`/`FirstProgenitor`/`NextProgenitor` are the slab indices (`SNAPSHOT-HDF5-FORMAT.md:70-101`). So repairing `snapshot_069.h5` would preserve row order and leave every descendant and progenitor index untouched. Only four datasets in one file would change: `MostBoundID` (un-negate), `FirstHaloInFOFgroup`, `NextHaloInFOFgroup`, and `HaloRankInForest` (a permutation *within* each forest's z=0 block, so `max_halo_rank_in_forest` and `ForestIndex` are unchanged). `UniqueGalaxyID` is a **runtime** value the reader composes from `ForestIndex` and the rank (`SNAPSHOT-HDF5-FORMAT.md:126-130`) — it is not a field to patch, and it changes for galaxies created at snapshot 69 as a consequence, not as an extra task.

**But it is blocked on information that no longer exists locally.** Rebuilding the z=0 FoF chains needs the original ctrees `pid`/`upid` for the 315,004,242 snapshot-69 halos. The emitted format does not store `pid`/`upid`; `fix_flybys` overwrote `upid` for every max-scale halo and `fix_upid` then overwrote `pid`; and the pre-fix-up intermediates are deleted. Reconstructing the association from snapshot 68's FoF structure plus progenitor links is a heuristic that misassigns newly-formed halos and any halo that changed FoF between snapshots — not acceptable for a dataset whose entire purpose is validation.

That leaves "extract the authoritative `(id, pid, upid)` for each forest's final-snapshot rows from the ASCII source on NT and ship a sidecar back". The independent review judged this **sound**, and specified what the sidecar must carry (forest id / dense `ForestIndex`; the forest's maximum snapshot or enough scale information to determine it; **every** halo at that snapshot, satellites included; and exact `id`, `pid`, `upid`) and the repair steps: join by `(ForestIndex, id)` → restore `pid`/`upid` → apply `fix_upid` semantics only → rebuild the FoF chains in reference `(upid, pid, id)` order (`scripts/convert/links.py:216-280`) → make every `MostBoundID` positive → recompute `HaloRankInForest` within each forest's final-snapshot block → stamp v2 and re-run the battery and the corrected gates. It also stressed producing **validated v2 replacement files** rather than overwriting the only 2 TB copy.

**It is still rejected, and the reasons are cost-shaped rather than correctness-shaped.** It reads the full 11.61 TB on NT anyway; it needs two pieces of novel, unvalidated tooling (the extractor and the slab surgeon), each of which would itself need a micro-Uchuu proof of byte-exact preservation of every non-FoF field before it could be trusted; and it yields a dataset that was *patched* rather than *produced by the certified pipeline*. Set against a certified pipeline that runs on NT for a comparable elapsed cost, on a dataset whose entire purpose is to be defensible, the shortcut does not pay.

**Recorded so it is not re-proposed — but with the conditions under which it would become the right answer.** If a future defect is confined to derived link fields *and* the pre-fix-up intermediates still exist, a partial re-run from `fixups` would be both cheaper and fully certified. **Note that retaining the bytes is necessary but not currently sufficient**: `fixups` refuses a snapshot whose manifest status is `linked` with nothing recorded as consumed (`scripts/convert/fixups.py:584-590`), so a real sorted-stage checkpoint/rollback would have to be built as well. See §10 item 1. The sidecar surgery is the fallback for the case where none of that holds.

### D5 — Run the conversion on the Mac Studio, exactly as before

**Decided by the owner 2026-09-09.** An NT-side conversion was proposed and rejected. The reasoning is operational, not technical: NT's Slurm queue can be unstable, it is shared with up to hundreds of users, and the owner has no control over it. A multi-day unattended job on a contended queue is a worse bet than a slower job on a machine that is entirely ours. **The Mac Studio is slower but rock solid, and the path is already proven end to end.**

So the conversion follows the original P1–P2 route: transfer the 11.61 TB source from NT in batches, scatter and release each batch, then the six-stage tail — on `/Volumes/LaCie`, with `--consume-intermediates`, exactly as `SHIN-UCHUU-CONVERSION-PLAN.md` → "The Production Execution Sequence" specifies.

**Three consequences follow, and all three are binding.**

1. **`--consume-intermediates` is not optional here.** The measured storage envelope is **6.89 TB against a 7.0 TB policy ceiling** with deletion on, and **≈8.65 TB with it off, before the staged source batch** — which does not fit the volume at all (`scripts/convert/README.md` → "Storage envelope and the production memory term"). The retention improvement that would have made a future fix-up-stage defect a ~51-hour re-run instead of a full re-conversion is **not affordable on this host**. §10 records what *is* affordable instead.
2. **P6 will be attempted here first, with NT as the fallback — see D7.** P5's ≈639–697 GB *projection* is what sent it to NT; the run then measured far lower, and the owner wants the local attempt. The margin is real but thin, and it is coupled to D6. D7 owns that analysis.
3. **Local storage must be prepared first**, because LaCie currently holds the v1 dataset and does not have the 7.0 TB cold-start headroom. See R1.

### D6 — The source column set: an owner decision, taken before any code changes

**Open. This must be settled before R3, and it is the reason R2 exists as a separate step.**

The converter extracts roughly 18–19 of the Consistent-Trees source format's 61 columns and discards the rest (`RECORD_DTYPE` in `scripts/convert/ctrees_parser.py`; the full 61-column table with what survives into Mimic's output is in the owner's companion note, *Shin-Uchuu Conversion — Format & Roadmap Addendum* §3). A full re-conversion is the **only** time additional columns can be added without paying for a full re-conversion, so if any are wanted, now is the moment — and the `format_version = 2` bump this work already requires absorbs the format change for free.

**There is no blocking problem with adding columns. There are four consequences that must be priced in, and one sequencing rule.**

1. **Storage.** Each additional `float32` column costs **≈0.09 TB on the emitted dataset** (4 B × 22.5 × 10⁹) and roughly the same again on each coexisting intermediate that carries the record (`worker scratch` 108 B/halo, `sorted` 108, `fixed` 120). Four extra `float32` columns is therefore ≈0.36 TB on the output and ≈0.36 TB on the intermediates peak. Against an envelope with **0.11 TB of headroom** that is not free — but the binding term is the *staged source batch*, which is an operator choice: **re-cut the batch plan from 4 batches to 5 or 6** and the headroom returns. Cost: a little more wall clock in the transfer/release loop, nothing else. **The envelope must be re-derived as part of this decision, not after it.**
2. **Memory at P6.** `struct RawHalo` widens, and the snapshot driver holds two slab generations. On the largest slab (519,342,987 halos) each `float32` column adds ≈8 B/halo across both generations ⇒ **≈+8.3 GB per column**, so four columns is ≈+33 GB on a ≈697 GB projection. Comfortable on the ≈1.03 TB NT node, but **re-project at R10 rather than assuming it**.
3. **Cross-format parity.** The ctrees ASCII reader is the *reference semantics* the converter is validated against, and the cross-format identity gate compares the two. A column present in the snapshot format but not produced by `read_ctrees_ascii.c` cannot be compared by either gate. Worse, `micro-uchuu-hdf5` reads uchuutools' pre-stored columns, which are a fixed set — a column absent there breaks the three-way triplet for that field. **Decide per column**: either it is obtainable in all three formats, or it is explicitly declared snapshot+ASCII-only and excluded from the gate by name. Do not leave this implicit.
4. **The property system wants a reason.** Each column needs a `halo_properties.yaml` entry in **both** `simulations/shin-uchuu/` and `simulations/shin-uchuu-ascii/` (and the micro-Uchuu packages if the gate is to cover it), carrying `units` and `h_convention`, plus either a `provides_core_role` or a named consumer. A field nothing reads is generated struct members, output bytes and validation surface for nothing — and `docs/STYLE-GUIDE.md` discourages speculative fields. **Add a column only with a stated intended use.**

**Sequencing rule.** The column decision touches the same files as the `fix_flybys` removal and the same format bump (`ctrees_parser.py`, `fixups.py`, `hdf5_writer.py`, `read_ctrees_ascii.c`, `SNAPSHOT-HDF5-FORMAT.md`, the two shin-uchuu `halo_properties.yaml`, the micro-uchuu-snapshot fixtures, the producer battery and the cross-check). Taking it **first** means one code change, one acceptance gate, one commit. Taking it later means doing all of that twice. Hence R2 before R3.

**One thing deliberately *not* done here.** `MIMIC-CONVERTER-GENERALISATION-PLAN.md` observes that `RECORD_DTYPE` is a hardcoded struct rather than declared per-simulation metadata, and this is the moment that struct gets touched. **Do not take the generalisation on now.** Adding a declarative column-mapping mechanism under time pressure, immediately before a six-day unattended conversion, is the wrong risk. Add the chosen columns to the existing struct and leave the generalisation as the separate, unscheduled decision it already is.

**Candidate columns, for the conversation only — the owner chooses.** From the 61-column table, the ones with a plausible semi-analytic use and no exotic cost: `Rvir` (11, measured virial radius rather than Mimic's derived one), `rs` (12) or `Rs_Klypin` (37) for concentration, `scale_of_last_MM` (15, last major merger — directly consumable by merger-driven physics), `M200c`/`M200b` (39/40, alternative mass definitions), `Xoff` (43) and `T/|U|` (56) as relaxedness indicators, `b_to_a`/`c_to_a` (46/47) for shape, and `rvmax` (60). Availability in the uchuutools HDF5 path must be checked per column before any of them is committed to.


### D8 — Keep four sequential transfer batches; the real time lever is elsewhere

**Question raised by the owner 2026-09-09:** three transfer rounds instead of four, using `/Volumes/Scratch`'s spare 3 TB, to speed things up.

**Three batches does fit.** The storage envelope is `peak = 2.47 TB workdir at scatter + staged batch S`, and it was derived to admit `S ≤ 4.4 TB` — explicitly "the 11.61 TB source needs at least three batches". At three, `S` = 3.87 TB and the peak is **6.35 TB** against LaCie's ≈7.09 TB free after R1; at four, `S` = 2.90 TB and the peak is **5.38 TB**.

**But it buys almost nothing.** Total transfer bytes are unchanged, so the transfer time is unchanged. The saving is one fewer `release` (5.19 h / 4 = **1.30 h** each) plus one fewer Phase-0 `forests.list` parse (≈8 min wall). That is **≈1.5 h out of 138 h — about 1%** — in exchange for cutting the headroom from 1.71 TB to 0.74 TB. **Decision: keep four.** The owner's own rule applies: a stable workflow at four is worth more than 1%.

**And `/Volumes/Scratch` was genuinely never used last round** — `df` reports 1.1 MiB consumed, and no log, `state.json` entry or handoff line references it. It was designated "overflow only" and the whole conversion ran on LaCie alone, exactly as the consumptive-deletion envelope intended. Nothing was staged there and subsequently deleted.

**The lever that would actually save real time is overlap, and it wants *more* batches, not fewer.** Transfers totalled 25.79 h and scatters 30.26 h, run strictly sequentially — `transfer → scatter → release → delete → next`. Overlapped, the transfers hide entirely behind the scatters: **≈26 h off 138 h, ≈19%**. The conversion plan already names this as a knowingly-accepted cost, and Scratch is what would make it clean: at four batches each staged batch is 2.90 TB, which **fits Scratch's 2.97 TB**, so batch *N*+1 could arrive on Scratch while batch *N* is scattered off LaCie — a double buffer across two physical volumes, with no head contention.

**Two things stand in the way, and the second is the real one.**

1. Scratch is earmarked for the v1 dataset at R1. That constraint is softer than it looks: **v1 already exists complete on NT** — verified 2026-09-09, `/fred/oz214/dcroton/shin-uchuu/snapshot-trees/`, 71 files totalling 2.2520 TB, byte-for-byte the same total as the local copy, and P6 ran off it. The local copy is a *second* backup, not the only one.
2. Overlap requires the one-off driver to run transfer and scatter concurrently. `/Volumes/LaCie/staging/shin-uchuu/run_p1_p2.py` is gitignored local state that already had **three restart-safety bugs found and fixed mid-run** last time. Adding untested concurrency to it, for a multi-day unattended job, is precisely the class of risk the owner chose the Mac Studio over NT to avoid.

**Not taken this round.** If the ≈26 h is wanted later, the honest way to get it is a small, separately tested change to the driver with its own dry run — not a change made in passing while setting up a six-day conversion.

### D7 — P6 on the Mac Studio: attempt locally first, with a defined abort and the NT fallback intact

**Decided by the owner 2026-09-09**, on the observation that the real peak came in far below P5's projection. It is the right call to try, and the margin is genuinely thin, so this section fixes the numbers, the abort criterion and the one interaction that could invalidate it.

**The numbers.** Physical memory is **549.8 GB decimal** (512 GiB, `sysctl hw.memsize`). What is left for Mimic depends on how hard the box is quiesced, and the owner will free it fully:

| Box state | Committed outside Mimic | Available to Mimic | Margin against a 507.4 GB peak |
|---|---:|---:|---|
| "Lean", as measured 2026-08-26 | ≈37.7 GB (31.0 anonymous + 6.7 wired) | ≈512 GB | **≈4.7 GB (99.1%)** |
| Fully quiesced (the plan) | wired plus a small OS residue | ≈530–540 GB | **≈25–35 GB (94–96%)** |

The second row is an estimate, not a measurement — **measure it, do not assume it** (see below). Note that file-backed pages, however large, are evictable cache and do not count.

| Peak figure | Value |
|---|---:|
| Slurm cgroup peak RSS, job `16267967` | 472.3 GB |
| **Mimic's own `print_run_memory_profile()` peak process RSS** | **507.384 GB** |

**Plan against 507.4, not 472.3.** The conversion plan records both and deliberately does not reconcile them. Mimic's own `getrusage` sampling of its own process is the directly comparable quantity for "does this fit on this box"; Slurm's cgroup accounting is a different measurement and choosing it here would be choosing the more convenient number. On that basis the margin is **≈5% on a fully quiesced box, and ≈1% if anything substantial is left running**. Quiescing is therefore not housekeeping here, it is a precondition.

**Two things attack that margin.**

1. **D6 — and this is the important coupling.** Each additional `float32` source column widens `struct RawHalo` by 4 B/halo across **two live slab generations**, which on the largest slab (519,342,987 halos) is **≈+8.3 GB**. Against a quiesced box's ≈25–35 GB that is a quarter to a third of the margin *per column*; against the lean-box figure a single column exceeds it outright. **The two decisions are not independent.** Decide D6 first, re-project, then choose the host: a couple of columns is probably still survivable locally on a quiesced box, more than that is not, and any column at all rules out attempting it on a box that is merely tidy rather than empty.
2. **macOS degrades by swapping, not by failing.** An overrun does not abort cleanly — it pages, turning a 17-hour run into a multi-day crawl. And snapshot-ordered runs have **no `--skip` resume** (C10), so there is nothing to pick up from.

**There is a lever that would make this comfortable, and its calculus has changed.** The compact previous-slab projection was deferred by owner decision on 2026-08-26 because it saved 28.4 GB against a 35.4 GB overshoot — it did not close the gap *as the gap was then projected*. Against the **measured** peak it saves 80 B/halo × 519,342,987 = **≈41.5 GB**, taking 507.4 → **≈466 GB, a ≈46 GB margin (91%)**. That deferral was correct on its evidence and is worth revisiting on this evidence — but it is unimplemented work, so it is noted here, not assumed.

**How to run the attempt.**

- Re-project first from the v2 report's own largest-slab count (R10), including any D6 widening. Do not carry 507.4 forward as a given.
- **Fully quiesce the box, then measure** — `vm_stat`, `memory_pressure`, `sysctl vm.swapusage` — and record the real committed figure immediately before launch. Neither the ≈37.7 GB lean-box measurement nor the ≈530–540 GB quiesced estimate above is a substitute for the number on the day.
- **Abort criterion**: watch `sysctl vm.swapusage` and `vm_stat` pageouts through the first hour. Any sustained pageout growth means abort and move to NT — do not let it grind.
- Keep the NT route ready. It costs only R11's 2.25 TB copy, which the local attempt otherwise skips.

**What the local attempt buys if it holds**: R11 (2.25 TB copy to NT, ≈5.7 h) and the ≈543 GB output transfer back both disappear — roughly **9–10 hours** and a good deal of operational faff, plus the output lands where the plotting already runs.

---

## The Remediation Sequence

Execute in order. **R1 and R2 come before any code is touched** — R1 because the volume has to be ready and that takes hours of copying, R2 because the column decision changes the same files everything else changes. R3–R7 are code and validation on the Mac Studio. R8–R10 are the local conversion. R11–R13 move to NT for the `sage16` run, exactly as the original sequence did. R14–R15 are closeout.

| # | Step | Blocks the rest? |
|---|---|---|
| **R1** | **Prepare local resources.** LaCie must have **≥ 7.0 TB free** before the first batch (the cold-start preflight floor; `SHIN-UCHUU-CONVERSION-PLAN.md` → P2 Step 1). Today it has ≈4.84 TB. **Move the v1 production dataset (2.2520 TB) to `/Volumes/Scratch`** — 2.97 TB, verified empty and unused for the whole first conversion — which takes LaCie to ≈7.09 TB free and keeps v1 intact as the hedge. **Moving it breaks `simulations/shin-uchuu/snapshots`, which points at the old location — re-point it at the Scratch copy in the same step**, so the symlink never dangles and nothing later reads a half-built v2 directory by accident. R10 re-points it again, at v2. Then sweep the rest: the old converter workdir (`/Volumes/LaCie/convert/shin-uchuu`, 8.7 GB — **keep**, it holds the manifest and the two `.npy` tables), `staging/shin-uchuu/incoming` (two orphaned `tree_*.dat` plus two aborted rsync temps, ≈10 GB — **delete**, they are re-transferred anyway), and confirm the retained rehearsal subsets survive (`subset-ascii` 197 GiB, `subset-snapshot` 38 GiB — **both must survive**; `simulations/shin-uchuu-ascii/snapshots` points at the first). **Use `/bin/rm`** — the shell alias moves to Trash and does not free space. Record `df -k` before and after. | **YES — nothing can start until the volume is ready** |
| **R2** | **Settle the source column set (D6), then update the scripts for it.** An owner conversation, not a code task: which of the 61 Consistent-Trees columns, if any, should the converter start keeping? Decide *per column* whether it is obtainable in all three formats or is snapshot+ASCII-only; give each one a stated use; then re-derive the storage envelope for the widened record — and **only if it no longer clears the 7.0 TB ceiling**, re-cut the batch plan (4 → 5 or 6 batches; the binding term is the staged source batch, which is an operator choice). Only then edit `ctrees_parser.py`'s `RECORD_DTYPE`, `fixups.py`'s `FIXED_RECORD_DTYPE`, `hdf5_writer.py`, `read_ctrees_ascii.c`'s bridge, and the `halo_properties.yaml` of every affected package. **"No new columns" is a legitimate and cheap answer** — it is the zero-risk branch, and R3 onward is unchanged either way. | **YES — R3 changes the same files** |
| **R3** | **Remove `fix_flybys` from the C reader and the converter.** Delete `fix_flybys()` from `src/io/tree/ctrees/ctrees_utils.{c,h}` and its call site (`read_ctrees_ascii.c:692-695`); delete `fix_flybys_snapshot()` from `scripts/convert/fixups.py` and its call from `apply_fixups_snapshot()`; remove the `MostBoundID` negation and `verify_mostboundid_invariant`'s sign handling. Nothing else in the fix-up order changes: `fix_upid` still runs, still first-and-only. **Two contracts die with it and their replacements must be named, not left implicit**: (a) the topology cross-check's `flyby-signs` check is one of the seven always-run checks that, with `topology-chains`, make up the "8/8" (`scripts/convert/crosscheck.py:1419-1451`) — decide whether it is deleted (making the gate 7/7) or replaced by a positive-`MostBoundID` assertion that keeps it at 8/8, and freeze that choice here; (b) `flyby_demotions` is a required field of the report schema (`scripts/convert/report.py:107-142`) — decide whether it is removed, or retained as a required zero-valued field that would loudly fail if the demotion ever came back. **The Developer's recommendation is (a) replace, keeping 8/8, and (b) retain as required-zero** — both preserve a live assertion rather than deleting the evidence, but the owner should confirm. | YES |
| **R4** | **Bump the format contract to `format_version = 2`** per D3: `docs/dev/SNAPSHOT-HDF5-FORMAT.md`, the producer stamp `FORMAT_VERSION` in `scripts/convert/hdf5_writer.py:56` (written at `:233`), and the consumer constant `SNAPSHOT_HDF5_FORMAT_VERSION` in `src/io/snapshot/read_snapshot_hdf5.c:62`. **Grep every use, not just the definition** — it appears in the object-set and header-attribute error messages (`:277`, `:313`, `:438`), the generated-property macro (`:767-770`), the actual version rejection (`:1056-1059`) and the `links_adjacent` rejection (`:1061-1064`). **v1 is rejected outright, no legacy path.** Verify the moved v1 dataset is now refused with a message naming the file and the version found. If R2 added columns, the new `Halo Datasets` rows land in the same bump. | YES |
| **R5** | **Re-establish the converter acceptance gate on micro-Uchuu, and close the coverage gap that let this through.** Re-run `scatter → sort → fixups → links → write → report` on `micro-uchuu-ascii` (measured 119.65 / 15.35 / 13.37 / 38.69 / 10.09 / 6.36 s), regenerate `micro-uchuu-snapshot` (both the full machine-local dataset and the committed small fixtures) at v2, then run the **producer battery (15/15)** and the **topology cross-check** against the tree-ordered reader — 8/8 or 7/7 depending on R3's `flyby-signs` decision, and whichever it is, say so in the record. These are minutes of compute. **The cross-format identity gate is not part of this step** — it is hours-scale and runs at R7. **Add the missing regression**: a fixture forest with at least two independent FoF groups at its maximum snapshot, asserting both survive as self-central with their own subhalos, in both the C helper test (`tests/unit/test_ctrees_support.c`) and the converter suite. Without it, nothing prevents this returning. | YES |
| **R6** | **Verify the corrected `micro-uchuu-ascii` science — the cheap end-to-end proof, ~2 minutes of runtime.** Re-run `sage16` on `micro-uchuu-ascii` and check it **numerically**, not by eye. Predeclared criteria, all computed from the z=0 (snap 49) galaxy output: (i) **zero** records with `MostBoundID < 0`; (ii) Type-0 count **496,374** and total count **557,519**, exactly matching `micro-uchuu-hdf5`; (iii) `max` centrals in any one forest > 1 (it is 2,309 for the other two readers); (iv) the mean group baryon fraction `Σ(baryons)/Mvir_central` over Type-0 galaxies, in 0.25 dex bins from 10^12.0 to 10^14.5 Msun with ≥ 3 centrals per bin, satisfies two bounds: within **1 × 10⁻³ absolute of `BaryonFrac` = 0.17** in every bin above 10^13.0 Msun — this is the physics gate, and the `micro-uchuu-hdf5` reference achieves 0.1695–0.1700 there — and within **2 × 10⁻³ absolute of the `micro-uchuu-hdf5` value** bin by bin across the whole range. The looser second bound is deliberate: the two readers carry a pre-existing float32-ULP `Mvir` divergence that `sage16` amplifies chaotically (§3.4), so exact agreement is not expected and demanding it would fail the gate for the wrong reason. If a bin misses by more than 2 × 10⁻³, check whether the miss is flyby-shaped (systematic, mass-increasing) or noise-shaped before concluding anything. Plots may accompany this; they are not the gate. | YES |
| **R7** | **Full validation, the identity gate, change control, commit.** `make generate`, `check-generated`, `validate-modules`, `check-format`, `check-docs`; full unit, integration and scientific tiers (delegate the long tiers to a subagent per the project convention). Then the **cross-format identity gate** — note this is *not* the minutes-scale converter battery: it is a manual, dataset-present operation over both full machine-local micro-Uchuu datasets, four builds and nine full runs, and it takes **hours** (`docs/DEVELOPER-GUIDE.md` → "The cross-format identity gate"). Budget it its own window. Then update every document and skill in §4.3, take independent review, and commit. | YES |
| **R8** | **P1 — transfer the 11.61 TB source from NT, batched.** `rsync -aL --files-from=<batch list>`, **no `--checksum`**, 5 attempts with backoff; fetch the 7.56 GB production `forests.list` first (it is a hard prerequisite of the first `scatter`). Re-cut the batch plan if R2 widened the record. Interleaved with R9 batch by batch, exactly as before: transfer → scatter → release → delete → next. | YES |
| **R9** | **P2 — production conversion on `/Volumes/LaCie`.** `--workdir /Volumes/LaCie/convert/shin-uchuu-v2`, **a fresh workdir** (the manifest binds to its inputs and refuses to resume across a dtype change — and R2 may have changed the dtype). `scatter --batch --pool-size 8` per batch then `release`, then `finalize` → `sort` → `fixups` → `links` → `write --output-dir /Volumes/LaCie/data/shin-uchuu/production-snapshot-v2` → `report --multiplier 20000000000`. **`--consume-intermediates` on all three of `fixups`, `links` and `write`** — mandatory here, see D5. **Wrap every stage in `/usr/bin/time -l`** and record peak RSS; the original run measured none of them. | YES |
| **R10** | **Post-conversion checks — P3, P3b, P4, P5.** Confirm `n_forests_total`/`max_halo_rank_in_forest` against the new report and re-confirm the 2 × 10¹⁰ identity multiplier (it should be unchanged, but confirm rather than assume); re-point `simulations/shin-uchuu/snapshots` at the v2 dataset and verify per-snapshot counts against the report; full `Spin` scan; re-project P6's memory peak from the new per-snapshot slab counts, **including any widening R2 introduced**. **New checks this round: assert zero negated `MostBoundID` in every snapshot file, and that the z=0 FoF-central count is 244,953,607 rather than 166,547,771.** | YES |
| **R11** | **P9 — copy the v2 dataset to NT.** `rsync -aL` to `/fred/oz214/dcroton/shin-uchuu/snapshot-trees-v2/`, ≈2.25 TB (more if R2 added columns) at the measured ≈110 MB/s ⇒ ≈5.7 h. Leave the v1 copy at `snapshot-trees/` in place; R14 retires it. Re-point the NT checkout's `simulations/shin-uchuu/snapshots` at the v2 directory. | YES |
| **R12** | **P6 — the `sage16` production run on NT.** Pull the NT checkout to the R7 commit. **Two things must change before submitting, and neither is optional.** (a) `ops/nt/sage16_shin-uchuu.sbatch:77` unconditionally does `ln -sfn /fred/oz214/dcroton/shin-uchuu/snapshot-trees simulations/shin-uchuu/snapshots` — it would silently point the run back at the **v1** dataset, which the v2 reader then rejects at `read_snapshot_hdf5.c:1056-1059`. Point it at `snapshot-trees-v2`, and add an assertion on the resolved target plus a `format_version == 2` check alongside the existing 71-file count check. (b) `models/sage16/input/sage16_shin-uchuu.yaml:11` writes to `output/sage16-shin-uchuu`, and HDF5 output truncates on create (`src/io/output/hdf5.c:88-91`) — so a rerun **destroys the v1 galaxy output**. Archive the NT v1 output first and set `output_directory: output/sage16-shin-uchuu-v2`. Then `sbatch`; it rebuilds from source every run. Expect ≈17 h and ≈472–507 GB peak RSS against R10's projection; a large deviation is a signal worth stopping for. | YES |
| **R13** | **P7 + P8 — range scan and science checks.** `deltaMvir` scan against `[-20000, 20000]` after asserting the job exited 0 and the snapshot set is complete. Then the full plot suite with the shin-uchuu profile, plus the three-epoch HMF/GSMF check at snapshots 69/52/43 in separate output directories. **The acceptance criteria are numerical, computed from `model_069.hdf5`, not read off a plot** — plots may accompany them: (i) **zero** galaxy records with `MostBoundID < 0`; (ii) Type-0 galaxy count consistent with the input FoF-central count of **244,953,607** (they are different quantities — some halos carry no galaxy — so state the expected relationship rather than asserting equality, as the v1 run's 166,547,771 Type-0 galaxies against 166,547,771 input centrals and 78,405,836 demotions established); (iii) mean group baryon fraction within **2 × 10⁻³ absolute** of `BaryonFrac` = 0.17 in every 0.25 dex bin above 10^13.0 Msun holding ≥ 3 centrals; (iv) the Type-0 halo mass function non-empty in every 0.25 dex bin up to at least 10^14.5 Msun, against the corrected expectation of 65 / 41 / 13 / 5 / 1 / 1 centrals in the 14.00–15.25 bins (§3.2), tolerance ±10% per bin on counts above 10. Transfer the ≈543 GB output back to the Mac Studio — **into a fresh destination** (`/Volumes/LaCie/data/shin-uchuu/output/sage16-shin-uchuu-v2/`), leaving the v1 output directory intact until R14. | YES |
| **R14** | **Retire v1 — only after R13's acceptance criteria have passed, and only with capacity proved first.** Four v1 artefacts exist. The Scratch dataset copy (2.2520 TB) **is already its archive** — R1 put it there, so nothing moves and the only action is to record it as retired. The other three need destinations named: the NT `snapshot-trees/` copy (2.25 TB), the NT `output/sage16-shin-uchuu/` galaxy output (≈543 GB), and the local `…/output/sage16-shin-uchuu/` copy (543.4 GB). For each of those three: name the archive destination, `df` it before moving, move rather than copy-then-delete where the destination is the same volume, and verify file count and total bytes at the destination **before** the source is touched. Per the project convention these are archived, never deleted — and `/bin/rm` is required locally if anything is removed, because the shell alias moves to Trash without freeing space. **Do not retire the v2 converter workdir or its manifest** — they are the provenance record for the dataset that replaced v1. | No |
| **R15** | **Close out.** NT cleanup is limited to `/fred/oz214/dcroton/shin-uchuu/working/` and any superseded job logs — **not** `snapshot-trees-v2/`, not the v2 output, and not `/fred/oz214/simulations/uchuu/shinuchuu/` (read-only source, not ours). Mac Studio cleanup is the v2 staging directory only. Then confirm this document's Definition of Done — **not** the conversion plan's, whose items are v1 evidence — and proceed to F1 (Uchuu-family particle mass) and F2 (merge `feature/ctrees-snapshot-reader` → `main`). | No |

**One decision point mid-sequence, worth flagging now.** Once `sort` completes at R9, `snap_NNN_sorted.bin` — the pre-fix-up state, 108 B/halo ≈ 2.43 TB — exists briefly before `fixups` consumes it. Retaining a copy is what would turn any *future* fix-up-stage defect into a ~51-hour re-run instead of another full re-conversion. It does not fit on LaCie (D5), but it **would** fit on Scratch (2.97 TB) *if v1 has been released from there by then*. By R9 the fix will have been proven end to end on micro-Uchuu (R6), so v1's insurance value is much lower than it is today. **Not decided here** — raise it with the owner when `sort` starts.

## Cost, Memory And Time

All measured figures come from the original production run: `/Volumes/LaCie/staging/shin-uchuu/logs/driver.log`, `conversion_report.json`, `links.log`.

### Measured, original run (Mac Studio, M3 Ultra, 32 cores, 512 GB)

| Stage | Wall clock | Notes |
|---|---:|---|
| Transfers (4 batches) | 25.79 h | ≈110 MB/s per batch, `rsync -aL`, no `--checksum` |
| `scatter` (4 batches, `--pool-size 8`) | 30.26 h | ≈106.6 MB/s aggregate — faster than the 71.9 MB/s pooled projection, unexplained |
| `release` (4 batches) | 5.19 h | |
| `finalize` | 12.64 h | overran its "30–75 min" estimate by more than 10× |
| `sort` | 9.09 h | |
| `fixups` | 9.85 h | |
| `links` | 28.48 h | 1.787 TB transient spill, 360.06 GB identity stores, 783 sorted runs, 1 merge pass, default 2 GiB budget |
| `write` | 7.39 h | |
| `report` | 5.26 h | full 15-check battery over 2.25 TB |
| **Total P1+P2** | **≈138.3 h (5.76 days)** | 2026-08-29T04:17Z → 2026-09-03T22:36Z |

Free space on `/Volumes/LaCie` through the run: 7.705 TB at cold-start preflight, down to a minimum of **4.163 TB post-`links`**, back to 5.422 TB post-`write`. Between batches it was observed as low as ~5.2 TB with no problem — which is why the 7.0 TB floor is a cold-start check only and the between-batch floor is 1.0 TB.

### Projected, this round (same machine, same route)

The original run *is* the projection. Expect the same figures, plus whatever R2's column decision adds.

| Phase | Projection | Basis |
|---|---:|---|
| R1 local preparation | **4–8 h** | copying 2.2520 TB from LaCie to Scratch; measure, do not assume |
| R2–R7 code, gates, review | **1–2 sessions** | R5's converter gate is ~3.5 min of compute, R7 ~2 min; the time is in R6's tiers and the doc/skill sweep |
| R8 + R9 conversion | **≈138.3 h (5.76 days)** | measured; more if the batch plan is re-cut to 5–6 batches for a widened record |
| R10 checks | ~2 h | `Spin` scan ≈14 min plus the re-point verification |
| R11 copy to NT | ≈5.7 h | 2.25 TB at ≈110 MB/s |
| R12 `sage16` on NT | 17.1 h | measured, job `16267967` |
| R13 scans, plots, transfer back | ~4 h | measured |
| **Total elapsed** | **≈8 days**, of which ≈6 days is unattended local conversion | |

**Peak RSS was never measured at production scale** for any stage of the original run. The only observations are two ad-hoc spot checks of `links` (4.8 GB at start, 30.8 GB at snapshot 31/70) against an unverified ≈225–235 GB projection. §10 item 2 closes that gap.

### The alternatives that were considered and rejected

| route | elapsed | new code | risk | provenance |
|---|---|---|---|---|
| **Re-convert on the Mac Studio (chosen)** | ≈8 days | none beyond R3/R4, plus R2 if columns are added | low — proven end to end on this exact machine | certified pipeline |
| Re-convert on NT | ≈6 days, saving the 25.8 h transfer and all the batching | none | **rejected by the owner (D5)**: the Slurm queue is shared with up to hundreds of users, can be unstable, and is outside our control. A multi-day unattended job there is a worse bet than a slower one here. The converter has also never run on Linux | certified pipeline |
| In-place surgical repair | ~1–2 days compute, but 2–3 days of new tooling plus its own validation | two novel tools | needs the ctrees `pid`/`upid`, which no longer exists locally | patched, not produced — **rejected, D4** |

---

## Process Improvements For This Round

Changes to how the conversion is *run*, not to what it produces. The first entry is the one that matters most, and on this host it is the one we cannot have.

1. **Retaining the pre-fix-up state is the lesson, and it is not affordable on LaCie.** Keeping `snap_NNN_sorted.bin` (108 B/halo ≈ 2.43 TB) would have turned this entire remediation into a ~51-hour `fixups → links → write → report` re-run (9.85 + 28.48 + 7.39 + 5.26 h measured) instead of an eight-day round trip. But the measured envelope is **6.89 TB against a 7.0 TB ceiling with `--consume-intermediates` on, and ≈8.65 TB with it off** — the volume cannot hold the retained state. **What is affordable** is the mid-sequence decision point recorded at the end of §8: once `sort` completes, and with the fix already proven end to end on micro-Uchuu at R6, v1 can be released from Scratch and `sorted` archived there in its place. **Record the choice either way**, so the next person knows it was weighed rather than forgotten.
**Operational — do these, they need no code change and belong to R9.**

2. **Wrap every stage in `/usr/bin/time -l`** and record peak RSS and peak workdir footprint. The original run measured neither at production scale; the `links` ≈225–235 GB projection is still unverified, and the next conversion plan deserves real numbers.
3. **Use a fresh workdir** (`/Volumes/LaCie/convert/shin-uchuu-v2`). The manifest binds to its input identities and refuses to resume across a dtype change; if R2 widens the record it *must* be fresh, and even if it does not, mixing a v1 manifest into a v2 run is a needless hazard. Keep the old workdir — it holds the manifest and the two `.npy` tables.
4. **Pass `--multiplier 20000000000` explicitly to `report`.** Three `--multiplier` defaults are 10⁹, all of which fail the production rank bound and make `report` exit 1; `type=int` means `2e10` is rejected by argparse.
5. **Re-read the one-off driver before trusting it.** `/Volumes/LaCie/staging/shin-uchuu/run_p1_p2.py` had three restart-safety bugs found and fixed mid-run — an unconditional cold-start preflight that failed every restart, no memory of completed batches (which would have re-downloaded a batch and then hit a guaranteed `release` failure), and orphaned `rsync` children surviving a driver kill on macOS. The fixes are in that file, but it is gitignored local state, not tested code. Always `ps aux | grep -iE "rsync|convert_ctrees"` after stopping it.
6. **Do not reintroduce the `du -sh` unit error.** macOS `du -sh` reports TiB. The dataset is **2.2520 TB decimal = 100.01 B/halo**, not "2.0 TB / 89 B/halo". Already corrected in the conversion plan.
**Deferred code work — explicitly NOT in this remediation.** Each is a real improvement and each would need its own frozen requirements, authorized files, tests and acceptance threshold. Taking any of them on immediately before a six-day unattended conversion is the wrong risk, and none of them appears in this document's authorized surface (§4.3) or Definition of Done. Record them; schedule them separately.

7. **Per-snapshot timestamps** in `sort`, `fixups`, `links` and `write` logging. Every one of these stages overran its extrapolation — `finalize` by more than 10× — and nobody can say which snapshots were slow.
8. **A science-level assertion in the conversion report**, not just a reference-parity one: FoF centrals per snapshot, plus a warning if any single forest holds more than a threshold fraction of a snapshot's halos. That one number — 33.0% — would have flagged this on day one. **Needs a threshold chosen on evidence**, which is exactly why it is not being bolted on now.
9. **Caching the Phase-0 `forests.list` parse.** 464 s per parse, uncached, redone on every `scatter`/`finalize` invocation and independently by each pool worker. It looks attractive — a healthy `scatter` appears hung for eight minutes before throughput appears — but it saves roughly **four CPU-hours against a six-day run** while adding cache-invalidation risk to the one file whose identity the manifest binds itself to. **Assessed as over-engineering for this round.** Revisit only if a future conversion is dominated by scatter startup.

---

## Independent Review

The diagnosis and all three decisions were put to **Codex `gpt-5.6-sol`** (high effort, read-only) before this document was finalised. Verdict: **CONFIRMED WITH CORRECTIONS**. It independently verified every structural claim in §2 and §6 against the source, and agreed with D1 (delete, no switch), D2 (keep `micro-uchuu-ascii`), D3 (`format_version = 2` is required, not an erratum) and D4 (exact repair from the emitted HDF5 alone is impossible).

Its corrections and additions are folded into the text above: the package exposure is four packages rather than two; the `upid` assignment is at `ctrees_utils.c:408`, not `:409`; five further physics consequences (§4.2); the complete file-level blast radius including the committed `micro-uchuu-snapshot` fixtures (§4.3); the missing multi-FoF regression test (R3); the minimum retention set for `micro-uchuu-ascii` (D2); the v1 compatibility policy question (D3); and the sharper statement of why the in-place repair does not need index rewriting (D4).

Two things it could not verify, stated plainly: **every large-data measurement in §3 is Developer-measured and was not independently re-derived** (Codex had no access to the multi-hundred-GB outputs or the external volumes), and the upstream `sage-model` rationale for `fix_flybys` was not inspected. Neither affects any decision here.

**A second round then reviewed these documents as execution instructions and returned NOT READY with 15 findings.** Several were real defects: the NT batch script would have silently repointed the run at the v1 dataset; a rerun would have truncated the v1 galaxy output; the claim that retaining `snap_NNN_sorted.bin` enables a partial re-run is false as the code stands; the cross-format identity gate was mis-scoped as minutes rather than hours; the scientific acceptance gates were prose rather than numbers; and removing the flyby convention deletes one of the eight topology cross-checks with nothing named to replace it. All 15 are addressed above, along with four the Developer found on a subsequent read.

> **A third, verifying round is still owed and has not run.** It was launched and hit a usage limit; it also flagged that the documents were being edited while it read them, which is a Developer process error worth not repeating — freeze the tree, launch the review, wait. **Run an independent verification pass over these documents before session C commits anything.** Artefacts, including the brief, both completed reports and the incomplete third: `.orchestrator/runs/flyby-diagnosis-20260909-181215-96232/`.

---

## Definition Of Done

0. **Prepared.** v1 archived on `/Volumes/Scratch`, LaCie ≥ 7.0 TB free, `df -k` recorded before and after (R1). The source column set is decided and recorded, whichever way it goes (R2, D6). The `flyby-signs` replacement and the fate of the report's `flyby_demotions` field are both decided and recorded (R3).
1. `fix_flybys` is absent from `src/`, `scripts/convert/` and their tests; no document or skill describes it as live behaviour.
2. `format_version = 2` is stamped by the producer, **required** by the consumer with no legacy-read path, and specified in `SNAPSHOT-HDF5-FORMAT.md`; the archived v1 dataset is rejected with a message naming the file and the version found.
3. The converter acceptance gate is green on micro-Uchuu: producer battery 15/15, topology cross-check at its post-R3 count (8/8 or 7/7 as decided), **and the new multi-FoF regression passes** — a fixture forest with two independent FoF groups at its maximum snapshot, both surviving as self-central with their own subhalos, asserted in both the C helper test and the converter suite.
4. The **cross-format identity gate** is green, run as the hours-scale dataset-present operation it is, over both full machine-local micro-Uchuu datasets regenerated at v2 — not merely over the committed small fixtures.
5. `micro-uchuu-ascii` `sage16` output meets R6's four numerical criteria: zero negated `MostBoundID`, Type-0 count 496,374 and total 557,519 exactly matching `micro-uchuu-hdf5`, more than one central in at least one forest, and the binned group baryon fraction within 1 × 10⁻³ of both the hdf5 reference and 0.17 above 10^13.0 Msun.
6. Full unit, integration and scientific tiers pass; `check-generated`, `validate-modules`, `check-format`, `check-docs` all clean.
7. The v2 Shin-Uchuu dataset exists, its report validates 15/15, it carries **zero** negated `MostBoundID` in any of the 70 files, and its z=0 FoF-central count is **244,953,607**, not 166,547,771. Per-stage peak RSS is recorded for all six tail stages.
8. P6 completes on the v2 dataset — submitted against `snapshot-trees-v2` with the symlink target asserted and a fresh output directory, so no v1 output was overwritten. P7's `deltaMvir` scan runs **after** P6 exits 0 and accepts against `[-20000, 20000]`.
9. P8's figures — full suite plus the three-epoch HMF/GSMF check — are produced, and R13's four numerical criteria pass, `BaryonFraction` and the Type-0 HMF included.
10. All four v1 artefacts (Scratch dataset, NT dataset, NT galaxy output, local galaxy output) are archived with destinations named, capacity proved and counts verified before any source was touched. The v2 workdir and manifest are retained.
11. This addendum, the conversion plan and the development pathway all record the outcome, and the conversion plan's own Definition of Done is explicitly marked v1-historical.
