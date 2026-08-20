# sage16 Prescription Classification

**Status:** Complete. Settles Open Question 1 of [`MIMIC-COUPLED-RATE-FORMULATION-PLAN.md`](MIMIC-COUPLED-RATE-FORMULATION-PLAN.md).
**Date:** 2026-08-20
**Scope:** Every prescription in the shipped `sage16` package, classified as **rate**, **jump**, **algebraic** or **forcing** by its state-reset semantics. Analysis only — no code changes, and none are implied. The coupled-rate work remains gated on its own measurement spike.

---

## Why this document exists

`MIMIC-COUPLED-RATE-FORMULATION-PLAN.md` targets a hybrid system: coupled continuous flow between reservoirs, explicit jumps at located events.

```text
ẋ = f(x, h(t), θ)        continuous transfer between reservoirs
x⁺ = J(x⁻, h, θ)         discrete jumps at located event times
```

Which prescriptions belong on which line is not obvious from reading the code, because today **every** prescription is written as an increment applied inside a substep loop. Operator splitting makes a rate and a jump look identical at the call site: both are `gal->X += something`. The classification therefore has to be made against the underlying physics, and the brief names the criterion — **state-reset semantics**, not the presence of a threshold.

This document is the brief's "largest prerequisite deliverable and the input to everything else". It is deliberately written to be falsifiable: every classification carries a `path:line` citation to the expression it rests on.

---

## The criterion, and the trap it avoids

A prescription is a **jump** when it *resets state discontinuously* — the post-state is not reachable by shrinking the timestep. A prescription is a **rate** when its effect scales with the interval, so that halving the step halves the transfer and the limit exists.

**A threshold does not make a process discrete.** The brief says this explicitly and `sage16` is full of the temptation. Star formation is zero below a critical cold-gas mass and a smooth rate above it (`sage_calculate_star_formation.c:96`); that is a rate with a crossing to locate, not a jump. Cooling switches between a cold-accretion and a hot-halo regime at `rcool = Rvir` (`sage_calculate_cooling_budget.c:58-63`); that is a piecewise-smooth rate, not a jump. Reincorporation is inactive below `Vcrit`; a crossing, not a jump.

The genuine jumps are the ones where a reservoir is *emptied into another wholesale*, or a discrete label changes: merger, disruption, the disk-instability transfer to the bulge, and the Type transitions the tree imposes.

The four categories as used here:

| Category | Test | Under the target formulation |
|---|---|---|
| **rate** | Effect ∝ interval; limit exists as `dt → 0` | A declared flux with a named source and destination |
| **jump** | Discontinuous state reset at a located instant | Stays an explicit jump `J(x⁻, h, θ)` |
| **algebraic** | Closed-form function of current state/forcing; no reservoir transfer of its own | A constitutive relation evaluated inside `f`, not a separate operator |
| **forcing** | Externally imposed by the tree, not computed from galaxy state | `h(t)`, applied at snapshot boundaries |

---

## Classification

Pipeline order is the shipped `models/sage16/input/sage16_mini-millennium.yaml`.

| # | Prescription | Phase / mode | Class | Evidence and reasoning |
|---|---|---|---|---|
| 1 | `sage_reionization` | pre_timestep, full_halo | **algebraic** | Sets `HaloBaryonFraction = f_baryon × f_reion(Mvir, z)`. No reservoir transfer; a coefficient consumed by the infall budget. Pure function of forcing and parameters |
| 2 | `sage_prepare_infall_budget` | pre_timestep, full_halo | **algebraic** (budget) | Computes `InfallingGas` as expected baryons minus those present. A once-per-snapshot closed-form quantity, not a transfer. Its output becomes the infall **forcing** for the interval |
| 3 | `sage_set_disk_scale_radius` | pre_timestep, full_halo | **algebraic** | `DiskScaleRadius = (λ/√2)·Rvir` from `Spin` and `Rvir`. A geometric constitutive relation on tree forcing |
| 4 | `sage_initialise_merger_clock` | pre_timestep, full_halo | **forcing** + **jump** | Sets `MergTime` from dynamical friction — the *guard variable* whose crossing fires the merger jump. Its Type-transition resets (`MergTime` cleared on 1/2→0 promotion, forced to 0 for Type 2) are genuine discrete resets driven by the tree |
| 5 | `sage_apply_infall` | galaxy_physics, full_halo | **rate** (constant) | Transfers `InfallingGas / num_substeps` into `HotGas` — explicitly the interval-averaged rate of item 2. Negative infall drains `EjectedGas` then `HotGas`. Source-limited only in the negative branch |
| 6 | `sage_reincorporation` | galaxy_physics, full_halo | **rate** | `reincorporated = (Vvir/Vcrit − 1)·EjectedGas/(Rvir/Vvir)·dt` (`sage_reincorporation.c:99`). Textbook source-limited rate: ∝ `EjectedGas`, vanishes as it empties. `Vvir > Vcrit` is a crossing |
| 7 | `sage_satellite_stripping` | galaxy_physics, by_galaxy | **rate**, cross-galaxy | Moves satellite `HotGas` into the **central's** `HotGas` (`sage_satellite_stripping.c:107`). A transfer between two galaxies' reservoirs — see the integration-domain finding below |
| 8 | `sage_calculate_cooling_budget` | galaxy_physics, by_galaxy | **rate** (flux only) | `coolingGas = HotGas/(Rvir/Vvir)·dt` or `(HotGas/Rvir)·(rcool/2tcool)·dt` (`:60`, `:63`). Both ∝ `HotGas`·`dt`, so source-limited. Computes the flux; applies nothing |
| 9 | `sage_radio_mode_heating` | galaxy_physics, by_galaxy | **rate** + **stateful coupling** | `BlackHoleMass += AGNrate·dt`, `HotGas -= AGNrate·dt` — a rate with named endpoints. But it also *reduces the cooling flux* and accumulates `Rheat` (`:113`, `:186`), which is the non-purity the brief flags |
| 10 | `sage_apply_cooling` | galaxy_physics, by_galaxy | **rate** (application) | Applies item 8's flux, `HotGas → ColdGas`. Split from its own rate law purely by the calculate/apply pattern |
| 11 | `sage_calculate_star_formation` | galaxy_physics, by_galaxy | **rate** | `strdot = SfrEfficiency·(ColdGas − ColdCrit)/tdyn` (`:96`), zero below the threshold. The brief's own worked example of a rate with a crossing |
| 12 | `sage_calculate_supernova_feedback` | galaxy_physics, by_galaxy | **rate**, slaved | `reheated = FeedbackReheatingEpsilon · stars` (`:103`) — proportional to the SF flux, not independently to state. Ejection likewise ∝ `stars`. This is a *rate slaved to another rate*, and the joint cap at `:108` is the adjudication discussed below |
| 13 | `sage_apply_star_formation_supernova` | galaxy_physics, by_galaxy | **rate**, cross-galaxy | Applies `ColdGas → StellarMass`, `ColdGas → central HotGas` (`:151`), `central HotGas → central EjectedGas`. Instantaneous recycling `(1−R)` is an algebraic partition of the SF flux |
| 14 | `sage_disk_instability` | galaxy_physics, by_galaxy | **jump** | Efstathiou criterion; when `diskmass > mcrit` the unstable excess is transferred to the bulge instantaneously (`sage_disk_instability_physics.h:57`, `:80`). The post-state is a projection onto the stability boundary and is *not* reachable by shrinking `dt` — a true state reset |
| 15 | `sage_quasar_mode` | galaxy_physics by_galaxy **and** per_event | **jump** | An instantaneous wind driven by a black-hole accretion increment. In per-event mode it consumes the merger event; in by-galaxy mode it follows the instability jump. Discrete in both |
| 16 | `sage_starburst_feedback` | galaxy_physics by_galaxy **and** per_event | **jump** | Collisional starburst at a merger, plus its own quasar wind. Fires at a located event |
| 17 | `sage_apply_metal_enrichment` | galaxy_physics, by_galaxy | **rate**, slaved | `MetalsColdGas += Yield·(1−f)·stars` — proportional to the SF flux. Consumes and clears `NewStellarMass` (`:98`), which is the operator-split marker described below |
| 18 | `sage_resolve_mergers_and_disruption` | satellite_mergers, full_halo | **jump** with a **rate** guard | `MergTime` decrements continuously (a clock, `dMergTime/dt = −1`); its crossing of zero fires either a merger or a disruption, which transfers reservoirs wholesale and sets `Type = 3` (`:99`). The canonical hybrid: continuous guard, located crossing, discrete jump |

**Totals, by primary class: 10 rate, 4 jump, 3 algebraic, 1 forcing** (18). Four prescriptions carry a second character — item 4 is forcing that also imposes jumps, item 9 is a rate that also mutates coupling state, item 12 is a rate slaved to another rate, and item 18 is a jump gated by a continuous guard. Those are discussed below.

---

## Five cross-cutting findings

These matter more than the table, because they are what the re-derivation actually has to deal with.

### 1. The operator-split fluxes are already named, and would cease to be state

Eight properties in `models/sage16/model_properties.yaml` are declared with `output: false` and a note of the form "Transport scratch buffer written by X; read by Y": `InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, `Rcool`, the cooling-`Lambda` diagnostic, and the unstable-gas fraction.

**These are not galaxy state. They are fluxes in transit between split operators**, and two of them are consumed-and-cleared exactly as a temporary would be (`sage_apply_star_formation_supernova.c:115-116`, `sage_apply_metal_enrichment.c:98`). Under the target formulation they are *declared transfers* and stop being stored on the galaxy at all.

This is a substantial and encouraging result for the brief's **Open Question 2** (metadata mechanism): the package already records producer→consumer endpoints for every one of them, in machine-readable YAML. Deriving reservoirs and roles from declared transfer endpoints, rather than adding a new declaration layer, looks feasible on this evidence — the endpoints exist and are already maintained.

### 2. Every `min(requested, available)` is a finite-step artifact, and they are enumerable

The brief predicts these dissolve under a source-limited continuous formulation. They can now be counted rather than assumed. The load-bearing ones:

- **Joint SF/feedback cap** — `sage_calculate_supernova_feedback.c:107-110`: when `stars + reheated > ColdGas`, *both* are scaled by `fac = ColdGas/(stars + reheated)`. This is the sharpest case in the package and is precisely the "whichever runs first wins" adjudication the brief describes.
- **Cooling cap** — `sage_calculate_cooling_budget.c:66-70`: `coolingGas` capped at `HotGas`, then floored at 0.
- **Ejection cap** — `sage_apply_star_formation_supernova.c:159-160`: ejected mass capped at the central's `HotGas`.
- **Stripping caps** — `sage_satellite_stripping.c:98-101`: gas and metals each capped at the satellite's reservoir.
- **Reincorporation cap** — capped at `EjectedGas`.
- **Negative-state repair** — `sage_apply_infall.c:68-92` and `sage_prepare_infall_budget.c:89-93` clamp reservoirs to `0.0f` after a subtraction.

The last item is worth separating: **those are clamps, and the brief forbids them** ("a negative committed state is an invariant violation, not a value to repair… Never clamp"). Under the target formulation they become step-rejection conditions. That is a genuine behavioural difference from `sage16`, not a refactor, and it belongs in the recalibration discussion rather than being glossed as parity.

### 3. Two rates are not pure functions of state — as the brief anticipated

The brief names both, and both are confirmed:

- **`Rheat` accumulates.** `sage_radio_mode_heating` reads `Rheat` (`:113`) and writes it back (`:186`), and the AGN heating it applies reduces the cooling flux computed upstream. Cooling is therefore a function of heating history, not of current state alone.
- **Feedback consumes the SF budget.** `sage_calculate_supernova_feedback` reads `NewStellarMass` produced by an earlier module (`:103`) rather than recomputing a rate from state.

Both must become explicit state functions or declared algebraic couplings before order-independence (brief gate 3) can hold. Neither is fatal: `Rheat` is already a real stored property, and the SF-slaved terms are naturally expressed as fluxes proportional to the SF flux within one coupled system.

### 4. Three prescriptions couple galaxies, and they set the integration domain

The brief's integration domain is "each connected component of the declared transfer graph". The components are not hypothetical here:

- `sage_satellite_stripping.c:107` — satellite `HotGas` → central `HotGas`
- `sage_apply_star_formation_supernova.c:151` — satellite `ColdGas` → **central** `HotGas`, then central `HotGas` → central `EjectedGas`
- `sage_apply_metal_enrichment` — a metal fraction routed to the central's `MetalsHotGas`

So under `sage16` the transfer graph's connected component is **the whole FoF group whenever any satellite is forming stars or retains hot gas** — which is the common case, not the exception. By-galaxy dispatch does *not* survive unchanged for this package. That is a materially more expensive starting point than "galaxies remain independently solvable where physics does not couple" might suggest, and it should be priced into the brief's cost estimate before Option A is chosen over Option B.

### 5. The jumps are well-separated and few

Five of eighteen are jumps, and they cluster cleanly: mergers and disruption, the disk instability, and the two merger-driven feedback consumers. All fire from located guards (`MergTime` crossing zero; the Efstathiou criterion). None is entangled with a rate in a way that would force a rate into jump form or vice versa.

This is the most favourable structural finding in the document: the hybrid split the brief proposes is a natural fit for `sage16` rather than an imposition, and the continuous subsystem is a large connected majority with a small, well-defined discrete boundary.

---

## What this settles, and what it does not

**Settled.** Open Question 1 is answered for every shipped `sage16` prescription, with evidence. The rate-shaped subset that a coupled package would have to reproduce (brief gate 2) is items 5–13 and 17 — ten prescriptions, of which two carry the non-purity of finding 3.

**Not settled, and not settleable here.**

- Whether this classification alone argues for the work. It does not — it is an input to designing the measurement spike, not a substitute for it. Note the surrounding decision, though, recorded 2026-08-20 in `MIMIC-DEVELOPMENT-PATHWAY.md` → "The Ordered Road": the coupled formulation is treated as **certain rather than contingent**, and the spike sets priority, solver family and the attribution baseline rather than permission.
- The clamp-to-zero sites (finding 2) are a behavioural difference, so a coupled package cannot be validated against `sage16` by bitwise parity. Its acceptance must be observational (brief Open Question 7).
- Finding 4 raises the expected solver cost. The ordering question it bore on is now settled — `MIMIC-DEVELOPMENT-PATHWAY.md` → "The Ordered Road" places the snapshot-global modules before the coupled rate formulation — so this finding bears on step 6's solver family and cost estimate rather than on a sequencing choice.

---

## Provenance

Classified by reading the shipped module sources at `bdff389b` on `feature/ctrees-snapshot-reader`. Every classification cites the expression it rests on; the pipeline order is the shipped `sage16`/`mini-millennium` run file. No code, tests, or configuration were changed, and no run was performed — this is a desk analysis of source, as the queue entry specified.
