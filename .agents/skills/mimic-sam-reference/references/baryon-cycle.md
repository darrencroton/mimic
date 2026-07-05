# The sage16 baryon cycle, stage by stage

Companion to the `mimic-sam-reference` SKILL.md section 3. This file walks the sage16 pipeline in execution order and explains the physics of each module in plain words, naming the reservoirs, transport properties, and parameters involved. Module implementations live under `models/sage16/modules/<module_name>/`; each module's `module_info.yaml` declares its property and parameter dependencies, and its `docs.physics` key (when present) points at its physics note. Shared physics helpers and model constants live in `models/sage16/shared/`. Parameter values quoted are the shipped defaults in `models/sage16/input/sage16_mini-millennium.yaml`.

Two structural facts to keep in mind:

- **Substeps.** Each snapshot-to-snapshot interval is divided into `SubSteps` (10 in the shipped run) equal substeps. The `pre_timestep` modules run once per snapshot; the `galaxy_physics` and `satellite_mergers` phases run once per substep. Transport properties (marked `output: false`, `init_repeat: true` in `models/sage16/model_properties.yaml`) carry intermediate results between modules.
- **Calculate/apply split.** Several stages are split into a "calculate" module that writes a transport property and an "apply" module that commits the reservoir moves. This makes each flow inspectable and keeps module responsibilities single-purpose.

## Pre-timestep: setting the stage (once per snapshot)

### sage_reionization

Before the first stars, the universe's gas was cold and neutral. Early stars and quasars flooded it with ultraviolet light ("reionization"), heating the intergalactic gas to ~10^4 K. Heated gas has pressure, and small halos have shallow gravitational wells — so after reionization, low-mass halos can no longer pull in their full share of gas. This module computes a suppression factor (a function of halo mass and redshift) that reduces the effective baryon fraction for small halos. The parity campaign of 2026-06-11 fixed an h-factor bug in this module's H(z) calculation (see the `mimic-failure-archaeology` skill).

### sage_prepare_infall_budget

The universe has a fixed ratio of ordinary matter to total matter: `GlobalBaryonFraction` (0.17, Planck 2018). A halo of mass `Mvir` "should" therefore contain `0.17 × Mvir` of baryons (less any reionization suppression). This module sums all baryons already present across every galaxy in the FoF group — ColdGas + HotGas + EjectedGas + StellarMass + ICS + BlackHoleMass — and stores the shortfall as the snapshot's infall budget (`InfallingGas`, and the diagnostic `HaloBaryonFraction`). The budget can be negative if the halo lost dark mass (e.g. tidal stripping), in which case gas is removed.

### sage_set_disk_scale_radius

Gas falling into a halo shares the halo's spin (angular momentum). As it cools it settles into a rotating disk whose size follows from angular-momentum conservation: `DiskScaleRadius` is set from the halo spin parameter and `Rvir` (Mo, Mao & White 1998). This radius controls star formation (below) — bigger disks spread the same gas thinner.

### sage_initialise_merger_clock

When a satellite's subhalo is stripped below resolution, the galaxy becomes a Type 2 orphan with no halo of its own. This module sets `MergTime` — the dynamical-friction time (Binney & Tremaine 1987): how long the orbiting galaxy takes to spiral into the central, longer for lighter satellites and wider orbits. `MergTime` is in code time units and counts down as snapshots pass; `sage_resolve_mergers_and_disruption` acts when it expires.

## Phase `galaxy_physics` (once per substep, order as listed)

### sage_apply_infall (full-halo)

Deposits the per-substep share of the infall budget into the **central** (Type 0) galaxy's `HotGas` — infalling cosmological gas is shock-heated to the halo's virial temperature on arrival. Only centrals accrete; satellites are inside someone else's halo. Infalling gas is assumed pristine (no metals), which dilutes `MetalsHotGas` metallicity.

### sage_reincorporation (full-halo)

Gas previously ejected from the halo by supernova feedback (`EjectedGas`) is not gone forever — it rains back into `HotGas` on a timescale controlled by `ReIncorporationFactor` (0.15) times the halo dynamical time. Massive halos reincorporate faster (deeper wells). Metals return in proportion (`MetalsEjectedGas` → `MetalsHotGas`). This module was the site of the float-narrowing trap fixed in commit 6cbeafe4 — see the `mimic-failure-archaeology` skill before touching its arithmetic.

### sage_satellite_stripping (by-galaxy)

Satellites plowing through the central's hot atmosphere have their own `HotGas` stripped away (ram pressure and tides) and donated to the central's `HotGas`. This is why satellites redden and die: with no hot reservoir, cooling stops and star formation starves ("strangulation").

### Cooling chain: sage_calculate_cooling_budget → sage_radio_mode_heating → sage_apply_cooling (by-galaxy)

Hot gas at the virial temperature radiates energy (X-rays) at a rate set by the Sutherland & Dopita (1993) cooling function — stored per-galaxy as `CoolingLambda`, a function of temperature and metallicity (metal-rich gas cools faster). Inside the **cooling radius** `Rcool` (where the cooling time is shorter than the age/dynamical time) gas condenses out of `HotGas` into `ColdGas`. Two regimes: small halos have `Rcool > Rvir` and cool rapidly ("cold mode"); massive halos cool slowly from a quasi-static hot atmosphere ("hot mode").

Between calculating and applying the budget, `sage_radio_mode_heating` lets the central supermassive black hole quietly accrete hot gas (mode selected by `AGNrecipe`; 2 = Bondi accretion, the rate at which gas falls onto a point mass through its own atmosphere) and injects jet energy that offsets the cooling budget, scaled by `RadioModeEfficiency` (0.08). This "radio-mode AGN feedback" is the Croton et al. (2006) mechanism that shuts down cooling in massive halos — without it, cluster centrals grow absurdly big and blue. The cumulative energies are logged in the output properties `Cooling` and `Heating` (erg/s, log10-transformed at output), and `Rheat` remembers the radius already heated by past AGN events (heating is cumulative and cooling cannot resume inside it). `sage_apply_cooling` then commits the surviving `CoolingGas` transport amount from `HotGas` to `ColdGas` (metals follow proportionally). Ordering matters and is guarded in code: heating must be computed after the budget and before the apply step.

### Star formation chain: sage_calculate_star_formation → sage_calculate_supernova_feedback → sage_apply_star_formation_supernova (by-galaxy)

Star formation (Kennicutt/Schmidt-style): cold gas above a critical surface density — evaluated over `StarFormingDiskFactor` (3.0) disk scale radii — turns into stars at a rate `SfrEfficiency` (0.05) × (available cold gas) / (disk dynamical time). The result is written to the transport property `NewStellarMass` (per its YAML note: written by sage_calculate_star_formation; read by the next two modules).

Massive stars die within a few Myr as **supernovae**, injecting energy into the surrounding cold gas. `sage_calculate_supernova_feedback` converts that energy budget into two transport amounts: `SupernovaReheatedMass` = `FeedbackReheatingEpsilon` (3.0) × mass of new stars, moved ColdGas → HotGas; and `SupernovaEjectedMass`, the mass blown out of the halo entirely (ColdGas/HotGas → EjectedGas) when the leftover SN energy exceeds what is needed to hold gas at `Vvir` — controlled by `FeedbackEjectionEfficiency` (0.3). Small halos (low `Vvir`) lose gas easily; big halos hold on. This is the mechanism that flattens the faint end of the stellar mass function.

`sage_apply_star_formation_supernova` commits all three flows atomically: stars form (minus `RecycleFraction`, see enrichment below), gas is reheated and ejected, `StarFormationRate` accumulates, and `SupernovaOutflowRate` is recorded.

### Disk instability chain: sage_disk_instability → sage_quasar_mode → sage_starburst_feedback (by-galaxy)

A self-gravitating disk that grows too massive relative to its halo support becomes unstable (Mo, Mao & White 1998 criterion): the excess material buckles into the central **bulge**. Unstable cold gas (`UnstableDiskGasFraction`) partly feeds the black hole — `sage_quasar_mode` grows `BlackHoleMass` via the `QuasarModeBHaccretionMass` transport (efficiency `BlackHoleGrowthRate`, 0.015) and its luminous accretion drives "quasar-mode" feedback that can blow cold (and even hot) gas out, scaled by `QuasarModeEfficiency` (0.005) — and partly ignites a **starburst** handled by `sage_starburst_feedback`. These two modules also run in per-event mode during mergers (below); here they run by-galaxy for instability-driven events.

### sage_apply_metal_enrichment (by-galaxy)

Stars fuse hydrogen into heavier elements ("metals" = everything heavier than helium, to an astronomer). Under the instantaneous-recycling approximation, each unit of new stellar mass immediately returns `RecycleFraction` (0.43) of itself as gas (massive stars die fast) and produces `Yield` (0.025) units of fresh metals, deposited into `MetalsColdGas` (a fraction `FracZleaveDisk`, 0.0 here, may bypass the disk straight to the hot phase). Metallicity `Z = MetalsX / X` for each reservoir X; solar metallicity `Z_sun` comes from `src/module_system/physical_constants.h`. This module exists as a separate stage because legacy SAGE applies disk-SF yields *after* the instability chain — reordering it broke bit parity, which is why it was split out during the 2026-06-11 parity campaign (module `sage_apply_metal_enrichment` was created then; see the `mimic-failure-archaeology` skill).

## Phase `satellite_mergers` (once per substep, after all galaxy physics)

### sage_resolve_mergers_and_disruption (full-halo; emits the `merger` event)

Walks the FoF group's satellites. For each orphan whose `MergTime` has expired: compare satellite and central baryonic masses. Mass ratio above `ThresholdMajorMerger` (0.3) → **major merger**: both disks are destroyed, all stars → `BulgeMass`, `TimeOfLastMajorMerger` stamps. Below it → **minor merger**: satellite stars join the central's bulge, gas joins the disk, `TimeOfLastMinorMerger` stamps. Separately, satellites whose halo has been stripped too far relative to their baryons (`ThresholdSatDisruption`, 1.0, an Mvir-to-baryonic-mass ratio test) are **disrupted**: their stars are scattered into the central's `ICS` (intracluster stars — the diffuse glow between cluster galaxies) rather than merging. Consumed galaxies become Type 3 and are never output.

For each merger it emits a `merger` event (`module_emit_event`, contract in `src/module_system/generated/event_contracts.h` — currently the only event producer in the codebase). The subscribed per-event consumers `sage_quasar_mode` and `sage_starburst_feedback` then run immediately for that merger: the collision funnels cold gas onto the black hole (Kauffmann & Haehnelt 2000 — this is where most `BlackHoleMass` is built) and triggers a merger-driven starburst with its own supernova feedback.

## Accounting invariants worth checking

These follow from the design and make good conservation checks (see the `mimic-scientific-method` skill for tolerance design):

- For a central galaxy, ColdGas + HotGas + EjectedGas + StellarMass + ICS + BlackHoleMass ≈ (suppressed baryon fraction) × Mvir, by construction of the infall budget.
- Every reservoir and every metal mirror must be ≥ 0 at output; `MetalsX ≤ X` always (metallicity ≤ 1).
- Metallicity of any reservoir should sit between 0 and a few × Z_sun (~0.02); MetalsColdGas/ColdGas ~ 0.5–2 Z_sun for Milky-Way-like galaxies is healthy.
- Reservoirs in sage16 are deliberately `float`, not `double` — a SAGE-parity decision (precision policy commit bf0993fa). Do not widen them; see the `mimic-properties` and `mimic-failure-archaeology` skills.
