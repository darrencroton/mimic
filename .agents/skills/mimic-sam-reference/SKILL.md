---
name: mimic-sam-reference
description: Domain theory pack for Mimic - semi-analytic galaxy modelling (SAM) concepts tied to this codebase. Load when a task mentions galaxy formation physics, merger trees, halos, subhalos, FoF groups, galaxy Types 0/1/2/3, baryon reservoirs (HotGas, ColdGas, EjectedGas, StellarMass, ICS), cooling, star formation, supernova or AGN feedback, black holes, metallicity, reionization, infall, reincorporation, mergers, orphans, dynamical friction, cosmology (Omega_m, h, redshift, scale factor), units like 1e10 Msun/h or Mpc/h, abundance matching / SHAM, or when interpreting what a physics plot or property value MEANS scientifically. Assume the reader has zero astrophysics background.
---

# Mimic SAM Reference — the physics behind the code

This skill explains what Mimic actually computes, for someone with no galaxy-formation background. Every concept is tied to a real Mimic property, module, file, or data product. It is a reference, not a runbook: load it to understand *meaning*, then route to the sibling skill that owns the *mechanics*.

## When to use / when NOT to use

Use this skill when you need to understand what a property, module, parameter, or plot means physically; when judging whether an output value is credible; or when a task uses astrophysics vocabulary you do not know.

Do NOT use it for:
- Plot registry/profile/figure-contract mechanics → see the `mimic-plots-and-analysis` skill (interpretation of what plots *show* is here, in `references/diagnostics-interpretation.md`).
- Property YAML schema, generated code, precision policy mechanics → see the `mimic-properties` skill.
- Writing or modifying physics modules → see the `mimic-modules` skill.
- Tree readers, simulation packages, catalog fields → see the `mimic-simulations-and-readers` skill.
- Proving a scientific claim with numbers and tolerances → see the `mimic-scientific-method` skill.
- Why past physics decisions were made (parity quirks, precision history) → see the `mimic-failure-archaeology` skill.

## First actions when handed a physics task

1. Read the relevant module README/docs: `models/<model>/modules/<module>/` (check `module_info.yaml` `docs.physics` key) and `models/<model>/README.md` for the science scope and citations.
2. Read the model-local shared helpers under `models/<model>/shared/` — sage16 keeps its scientific constants and common physics functions there.
3. Read the run YAML pipeline (`models/sage16/input/sage16_mini-millennium.yaml`) to see the exact module order and parameter values in force.
4. Look up every property you will touch in `models/<model>/model_properties.yaml`, `src/core/core_properties.yaml`, or `simulations/<sim>/halo_properties.yaml` — the `description`, `units`, `range`, and `notes` fields are the authoritative one-line physics definitions.
5. Before claiming any scientific result, apply numbers-before-claims (see the `mimic-scientific-method` skill).

## 1. The big picture

A **semi-analytic model (SAM)** of galaxy formation works like this:

1. An **N-body simulation** (run elsewhere, not by Mimic) evolves only dark matter — collisionless particles under gravity — in a periodic box from the early universe to today, saving **snapshots** at fixed times.
2. A halo finder groups particles into **halos** (gravitationally bound clumps of dark matter) at each snapshot, and a tree builder links each halo to its **progenitors** (the halos at the previous snapshot that merged to form it). The result is a **merger tree** per final halo. These trees are Mimic's *input*.
3. The SAM walks each tree from earliest to latest snapshot and applies simplified, calibrated equations for the **baryonic** physics (ordinary matter: gas, stars, black holes) that the dark-matter-only simulation could not follow: gas falls in, cools, forms stars, explodes, gets ejected, comes back, merges.
4. The output is a **mock galaxy catalogue**: for every halo at every requested snapshot, a set of galaxy properties (stellar mass, gas mass, star formation rate, ...).

Why SAMs exist: a full hydrodynamic simulation of the same volume takes months on a supercomputer; a SAM re-runs the baryonic physics over precomputed trees in minutes on a laptop. That makes SAMs the tool of choice for exploring parameter space and large volumes.

What Mimic adds over legacy SAMs (e.g. original SAGE): a **physics-agnostic core** (tree traversal, memory, config, validation, output live in `src/` and know nothing about galaxies), **runtime-configurable module pipelines** (the physics is an ordered list of modules in the run YAML, not hard-coded), and separate **model and simulation package selector axes** (`models/<model>/` owns physics, `simulations/<sim>/` owns the input catalog). Supported combinations require a matching run file, consistent properties, and validation; do not claim an untested Cartesian-product guarantee. See the `mimic-architecture-contract` skill for the design contract.

## 2. Halo and galaxy taxonomy; tree vocabulary

- **FoF group**: a friends-of-friends group — the halo finder's top-level object, a cluster of dark matter that may contain several distinct bound clumps. Mimic processes one FoF group's members together in a `FoFWorkspace`.
- **Subhalo**: a bound clump inside a FoF group. The largest is the "central" subhalo; the rest are satellites that fell in earlier.
- **Galaxy `Type`** (property in `src/core/core_properties.yaml`, output range 0–2):
  - **Type 0** — central galaxy of the FoF group's main subhalo. The only galaxy that receives fresh cosmological gas infall.
  - **Type 1** — satellite galaxy in a still-resolved subhalo. Keeps its own halo properties but no longer accretes; its hot gas can be stripped.
  - **Type 2** — **orphan**: the subhalo dropped below the simulation's resolution (particles were tidally stripped until the finder lost it), but the galaxy inside should still exist. Mimic keeps it alive on a merger clock. `make_orphan` in `src/core/inheritance.c` zeroes `Mvir`/`Len` but preserves `Rvir`/`Vvir`.
  - **Type 3** — consumed: the galaxy merged into another this snapshot. Internal only; never output.
- **The five tree links** (core roles in `core_properties.yaml` `required_inputs`): `Descendant` (which halo this becomes next snapshot), `FirstProgenitor` (most massive parent at the previous snapshot), `NextProgenitor` (sibling chain of additional parents), `FirstHaloInFOFgroup` (the FoF central), `NextHaloInFOFgroup` (sibling chain within the FoF group).
- **Snapshots and `a_list`**: each simulation package ships `<name>.a_list`, one scale factor `a` per line, earliest to latest; line count defines the snapshot count. Convert to **redshift** with `z = 1/a − 1` (a=1 is today, z=0; a=0.5 is z=1, when the universe was half its current size).
- **Halo structural properties** (one-liners; YAML descriptions are authoritative):
  - `Mvir` — virial mass: total mass inside the radius where mean density is 200× the critical density (units 1e10 Msun/h).
  - `Rvir` — virial radius: that radius (Mpc/h).
  - `Vvir` — virial velocity: circular orbit speed at `Rvir`, `Vvir = sqrt(G·Mvir/Rvir)` (km/s).
  - `Vmax` — the maximum of the circular-velocity curve; more robust than `Mvir` for stripped subhalos.
- **SAGE quirk**: sage16 updates `Rvir`/`Vvir` only when `Mvir` grows, mirroring legacy SAGE — halos never "shrink" structurally. Marked with `// SAGE parity:` comments; never "fix" in sage16 (see the `mimic-failure-archaeology` skill).
- **Infall properties**: when a central is demoted to satellite (Type 0→1), Mimic records `infallMvir`, `infallVvir`, `infallVmax` — a frozen picture of the halo at its peak, before stripping erodes it. Done at inheritance time in `src/core/inheritance.c`.
- **Merger clock / dynamical friction**: a satellite orbiting inside a bigger halo is dragged inward by **dynamical friction** (its gravity raises a wake behind it that pulls it back). sage16 estimates the time-to-merge when a subhalo is lost (`sage_initialise_merger_clock` sets `MergTime`, in code time units) and merges the orphan into the central when the clock expires (`sage_resolve_mergers_and_disruption`).

## 3. The baryon cycle (sage16 pipeline)

The universe's ordinary matter cycles between named **reservoirs** attached to each galaxy. In words:

```
cosmic gas --infall--> HotGas --cooling--> ColdGas --star formation--> StellarMass
                 ^                   ^         |                            |
                 |                   |     SN feedback              recycling (RecycleFraction)
        reincorporation         reheated       |
                 |                   |         v
             EjectedGas <--ejected-- (energetic outflow)
Metals mirror every flow (MetalsHotGas, MetalsColdGas, ...); BlackHoleMass grows in
mergers/instabilities; disrupted satellites' stars -> ICS (intracluster stars).
```

Reservoir properties, all in `models/sage16/model_properties.yaml` with these descriptions: `HotGas` (hot gas mass in halo atmosphere), `ColdGas` (cold gas mass available for star formation), `EjectedGas` (mass of gas ejected from galaxy by feedback), `StellarMass` (total stellar mass), `BulgeMass` (stellar mass in bulge component — the central spheroid, built by mergers and instabilities, vs the rotating disk), `ICS` (intracluster stellar mass — stars unbound from disrupted satellites), `BlackHoleMass` (central supermassive black hole mass), plus the metal mirrors `MetalsStellarMass`, `MetalsBulgeMass`, `MetalsColdGas`, `MetalsHotGas`, `MetalsEjectedGas`, `MetalsICS`. Transport scratch properties (`output: false`, reset per snapshot) carry flows between modules within a substep: `InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, `QuasarModeBHaccretionMass`.

Pipeline order (verified against `models/sage16/input/sage16_mini-millennium.yaml`; `SubSteps: 10` divides each snapshot interval into 10 substeps):

| Stage | Module(s) | Physics in plain words | Key parameters |
|---|---|---|---|
| Reionization (pre_timestep) | `sage_reionization` | Early-universe UV radiation heats gas so small halos can't hold their share of baryons; suppresses infall in low-mass halos | `GlobalBaryonFraction` (0.17) |
| Infall budget (pre_timestep) | `sage_prepare_infall_budget` | Computes how much fresh gas the FoF halo should have: baryon fraction × Mvir minus baryons already present | `GlobalBaryonFraction` |
| Disk size (pre_timestep) | `sage_set_disk_scale_radius` | Sets `DiskScaleRadius` from halo spin — spinning gas settles into a rotating disk | — |
| Merger clock (pre_timestep) | `sage_initialise_merger_clock` | Starts `MergTime` (dynamical-friction countdown) for newly orphaned satellites | — |
| Infall + return | `sage_apply_infall`, `sage_reincorporation` | Adds the infall budget to the central's `HotGas`; slowly returns `EjectedGas` to `HotGas` | `ReIncorporationFactor` (0.15) |
| Stripping | `sage_satellite_stripping` | Removes hot gas from satellites into the central's hot atmosphere | — |
| Cooling + AGN heating | `sage_calculate_cooling_budget`, `sage_radio_mode_heating`, `sage_apply_cooling` | Hot gas radiates energy (rate set by the Sutherland & Dopita cooling function `CoolingLambda`) and condenses to `ColdGas`; the black hole's "radio mode" jets offset cooling in massive halos | `AGNrecipe` (2 = Bondi), `RadioModeEfficiency` (0.08) |
| Star formation + SN feedback | `sage_calculate_star_formation`, `sage_calculate_supernova_feedback`, `sage_apply_star_formation_supernova` | Cold gas above a critical surface density forms stars; supernovae reheat some cold gas back to hot and eject some from the halo entirely | `SfrEfficiency` (0.05), `StarFormingDiskFactor` (3.0), `FeedbackReheatingEpsilon` (3.0), `FeedbackEjectionEfficiency` (0.3) |
| Disk instability | `sage_disk_instability`, `sage_quasar_mode`, `sage_starburst_feedback` | Too-massive disks buckle: gas/stars move to the bulge, feeding the black hole (quasar mode) and triggering a starburst | `BlackHoleGrowthRate` (0.015), `QuasarModeEfficiency` (0.005) |
| Metal enrichment | `sage_apply_metal_enrichment` | Newly formed stars return metals (elements heavier than helium) to the gas; must run after the disk-instability chain (SAGE ordering) | `Yield` (0.025), `RecycleFraction` (0.43), `FracZleaveDisk` (0.0) |
| Mergers/disruption (`satellite_mergers` phase) | `sage_resolve_mergers_and_disruption` (full-halo, emits `merger` event) → `sage_quasar_mode`, `sage_starburst_feedback` (per-event consumers) | Expired merger clocks merge satellites into centrals; major mergers (mass ratio > `ThresholdMajorMerger`) destroy disks into bulges; fragile satellites are disrupted into `ICS` | `ThresholdMajorMerger` (0.3), `ThresholdSatDisruption` (1.0) |

Full physics narrative with equations-in-words per module: `references/baryon-cycle.md`.

## 4. Units and h, for non-astronomers

- **h** is the dimensionless Hubble parameter: the universe's current expansion rate is `H0 = 100·h km/s/Mpc`. Historically uncertain, so simulators bake h into their units so results can be rescaled; h is 0.73 (Millennium) or 0.6774 (Uchuu) here.
- Mimic's **fixed internal reference basis** (`reference_units:` in `src/core/core_properties.yaml`): mass `1e10 Msun/h`, length `Mpc/h`, velocity `km/s`, time derived as length/velocity. All reader input is converted to this basis at the boundary (generated `unit_registry.h`); Msun = solar mass ≈ 1.989e33 g.
- Each property's `h_convention` is `carried` (value contains an h factor you must divide out; e.g. masses, lengths), `free` (no h dependence), or `none` (h not applicable; e.g. velocities). See the `mimic-properties` skill for mechanics.
- **Worked conversions** (h = 0.73): a mass of `10` code units = 10 × 1e10 Msun/h = 1e11 Msun/h = 1e11 / 0.73 ≈ **1.37e11 Msun**. A radius of `0.2` Mpc/h = 0.2 / 0.73 ≈ **0.27 Mpc**.
- **Code time unit** = (Mpc/h)/(km/s) ≈ 3.086e19 s / h ≈ 978 Gyr/h — huge, so times in code units are small fractions. Note `dT` (time since progenitor snapshot) is stored in **Myr/h** (its YAML says so), while `MergTime` is in raw code time units. `StarFormationRate` is converted to **Msun/yr** at output (`output_convert` in its YAML entry).

Magnitude sanity table (code units unless noted; derived from `range:` fields in the property YAMLs and the particle masses — use to spot garbage values):

| Property | Dwarf galaxy halo | Milky-Way-like | Galaxy cluster | YAML range cap |
|---|---|---|---|---|
| `Mvir` (1e10 Msun/h) | ~1–10 | ~100 (1e12 Msun/h) | ~1e4–1e5 | 1e6 (1e16 Msun/h) |
| `StellarMass` (1e10 Msun/h) | ~0.01–0.1 | ~4–6 | BCG ~50–100 | — |
| `StarFormationRate` (Msun/yr, output) | ~0.01–0.1 | ~1–3 | starburst ~100s | 1e4 |
| `Rvir` (Mpc/h) | ~0.05 | ~0.2 | ~1–2 | 10 |
| `Vvir`/`Vmax` (km/s) | ~30–80 | ~150–250 | ~1000–2000 | 5000 |

Stellar mass should always be well below `GlobalBaryonFraction × Mvir` (≈ 0.17·Mvir); total baryons (ColdGas+HotGas+EjectedGas+StellarMass+ICS+BlackHoleMass) should equal it for centrals to within the model's accounting.

## 5. Shipped simulations

Verified from `simulations/<name>/simulation_info.yaml` (2026-07-04):

| Package | Box (Mpc/h) | Particle mass (1e10 Msun/h) | Ωm / ΩΛ / h | Notes |
|---|---|---|---|---|
| mini-millennium | 62.5 | 0.0860657 | 0.25 / 0.75 / 0.73 | Default; only trees `first_run.sh` downloads |
| millennium | 500.0 | 0.0860657 | 0.25 / 0.75 / 0.73 | Metadata only; symlink your own trees |
| micro-uchuu / -hdf5 / -ascii | 100.0 | 0.0325 | 0.3089 / 0.6911 / 0.6774 | Same data in L-Halo binary / CTrees-HDF5 / CTrees-ASCII |
| mini-uchuu | 400.0 | 0.0325 | 0.3089 / 0.6911 / 0.6774 | |
| uchuu | 2000.0 | 0.0325 | 0.3089 / 0.6911 / 0.6774 | Full Uchuu, CTrees-HDF5 |

The Uchuu family uses the Planck-2015 cosmology. **Rule of thumb (community convention, not a repo-enforced fact)**: a halo needs ≳20 particles to be minimally credible, so trust nothing below ~20 × particle_mass (≈1.7 code units for Millennium, ≈0.65 for Uchuu); statistical properties (mass functions) need more like 100+ particles. Running the *same* physics on Millennium and Uchuu boxes is a first-class robustness workflow: different cosmology, resolution, volume, and tree format expose resolution artefacts, cosmology hard-coding bugs, and reader bugs that a single simulation hides.

## 6. SHAM in one paragraph

**Subhalo abundance matching (SHAM)** is the simplest way to paint galaxies onto halos: rank all (sub)halos in a volume by a mass/velocity proxy, rank observed galaxies by stellar mass, and match the two ranked lists so the N-th biggest halo hosts the N-th biggest galaxy — a *global* operation over a whole snapshot. Mimic's `models/sham` package is **not** that: because Mimic processes one FoF workspace at a time, `sham_assign_stellar_mass` (the package's single module, run as `process_full_halo` in `post_timestep` with `SubSteps: 1`) is a *deterministic local proxy* — it tracks peak halo properties (`ShamMpeak`, `ShamVpeak`) along each branch and applies an analytic Moster et al. (2013)-style stellar-to-halo mass relation with optional deterministic scatter (`ShamUseScatter`, `ShamScatterDex`). Its README says explicitly it is not for precision science; it exists to exercise the model-package architecture. See `models/sham/README.md`.

## 7. Literature anchors (as the repo cites them)

From `models/sage16/README.md`: sage16 is a modular port of **SAGE** as calibrated in **Croton et al. (2016)** "Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results", with the physics lineage of **Croton et al. (2006)** "The many lives of active galactic nuclei" (the AGN radio-mode paper). Supporting citations: White & Frenk (1991) (the founding SAM framework), Sutherland & Dopita (1993) (cooling functions), Kauffmann & Haehnelt (2000) (BH growth in mergers), Somerville et al. (2001), Mo, Mao & White (1998) (disk sizes from spin), Binney & Tremaine (1987) (dynamical friction). From `models/sham/README.md`: Conroy et al. (2006) and Vale & Ostriker (2006) (abundance matching), Reddick et al. (2013) (scatter/proxy choice), Moster et al. (2013) (the double-power-law relation the defaults use).

## References

- `references/baryon-cycle.md` — the full physics narrative per pipeline stage, with the transport-property handoffs.
- `references/diagnostics-interpretation.md` — what each sage16 figure shows, what healthy output looks like, and which parameter moves which feature.

## Provenance and maintenance

All facts verified against the live repo on 2026-07-04. Re-verify before relying:

- Pipeline order and parameter values: `cat models/sage16/input/sage16_mini-millennium.yaml`
- Reservoir property definitions: `grep -n -E 'name:|description:|units:' models/sage16/model_properties.yaml`
- Core halo properties and reference units: `sed -n '1,220p' src/core/core_properties.yaml`
- Cosmology/box/particle mass: `grep -sA2 -E 'omega|hubble_h|box_size:|particle_mass:' simulations/*/simulation_info.yaml`
- Figure inventory: `grep -A25 'SNAPSHOT_PLOTS' models/sage16/plots/figures/__init__.py`
- Citations: `models/sage16/README.md` and `models/sham/README.md`
- SAGE parity quirks: `grep -rn 'SAGE parity' src/ models/sage16/ | wc -l`
