# Mimic Coupled Rate Formulation Plan

**Purpose:** State what is left of the coupled rate formulation, which is putting real models on the rate mode, and hold what is already known and decided about that work until its campaign briefs are written.

**Status:** Rewritten 2026-11-23 as the standing plan for the rate-equation forks; pathway step 7. The first half of the work, the rate mode in Mimic's core, is finished: built, reviewed and closed on 2026-11-14 and released in v1.7. The second half, the forks of `sage16` and then `sage26`, is not started and has no brief. **This document is a placeholder for those briefs, not one of them:** the `sage16` fork's brief is written when the campaign `calibration` has reported, from the inputs listed at the end. The requirements brief that governed the core work (revised 2026-11-12, with its formulation, contracts, fixture and gate) is archived as `archive/dev-plans/MIMIC-COUPLED-RATE-FORMULATION-PLAN-2026-11-23-stage-1-brief.md` and is in git at `973efd43`; its implementation plan, acceptance record and post-implementation review have been in `archive/dev-plans/` since 2026-11-14.

**Order of work:** [the pathway](MIMIC-DEVELOPMENT-PATHWAY.md#the-sequence-from-here-owner-2026-11-17), revised by the owner on 2026-11-23. The calibration instrument first; then `sage16` forked onto the rate mode; the time step from 1 to 200 substeps, for the published model once the instrument exists and for the fork once that exists; then `sage26`'s fork, after the owner's import of SAGE26 v1.0.

---

## Why

Mimic evolves galaxies by walking a configured list of modules, each reading the galaxy's state and writing its effect back before the next runs. The scientific result therefore depends on the order of execution. For a prescription that is a rate, that dependence is a splitting artefact, and it shrinks with the time step. Two facts make it concrete:

- **The shipped `sage16` is not converged.** At its ten fixed substeps, total z = 0 stellar mass on mini-Millennium was 66% above its value at 1,000 fixed substeps; the mass function differed by 0.08 dex RMS below the knee and by 0.4 to 0.8 dex above it; the fixed and dynamic schemes converged to one answer, roughly first order in the step (measured 2026-11-10 with no commit or machine recorded, so it is owed a reproducible re-measurement).
- **Calibrated parameters absorb the residual.** `sage16`'s parameters were tuned against that pipeline, so some part of every calibrated value compensates for numerics. Order dependence itself has not been measured for a shipped model.

The question the forks answer, and the Mimic paper reports (owner, 2026-11-23): when the same physics is integrated as one coupled system, with its accuracy set and measured and its result independent of module order, how far must the parameters move for the model to fit the same observations?

---

## What Exists: the Rate Mode

The mode as built is specified by the Doxygen of `src/core/rate_integrator.h`, `src/core/rate_assembly.h` and `src/core/module_registry.h`, explained in the Developer Guide's [Rate Callback Contract](../DEVELOPER-GUIDE.md#rate-callback-contract), stated as principle in [`docs/VISION.md`](../VISION.md), and exercised by `make tests-rate`. In short:

- **Modules declare transfers, not updates.** A `process_rates` module declares conservative transfers between reservoirs in `module_info.yaml` and returns their rate coefficients from a pure callback; the core assembles every rate module of the phase into one system and integrates it across each substep with error control.
- **A transfer out of a reservoir is a specific rate,** a non-negative coefficient times the donor's content, so a reservoir cannot go negative and nothing is clamped. A constant drain, `min(demand, available / dt)` and any law that empties a reservoir in finite time cannot be declared.
- **A law is a function of the state, the forcing, the parameters and the time,** with no side effects and no view of the solver's step or a substep index.
- **One rate phase at most; discrete phases keep their modes and their order.** Jumps (mergers, disruption, instabilities) run at segment boundaries. A substep is a segment, so `SubSteps` now sets the cadence of jumps.
- **The integration domain is decided from declared metadata:** each galaxy alone, or the whole FoF group as soon as a transfer crosses to the central or a law reads it.
- **Two schemes:** an explicit positivity-preserving scheme, the default, and a linearly implicit one, first order and experimental.
- **A conservation ledger** closes every conserved total over each rate segment, where a failure names its cause and commits nothing, and checks over the discrete phases that a total changes only where a module declares it.
- **What is proven:** on a model-neutral fixture only. Permuting the rate modules gives bit-identical output on one binary, across both drivers, MPI task counts and chunk counts. No model package uses the mode, and no production cost is measured.

---

## What Is Decided

1. **The published packages are not converted.** `sage16` and `sage26` stay as published; a published model's numerics are part of what was published. Each fork is a new package beside its parent (`sage16rate` first), and the parent is its reference.
2. **The forks are not released as new published models.** They develop the tools and the workflow, and the Mimic paper reports what they show (owner, 2026-11-23, amending the decision of 2026-11-12). `sage16` and `sage26` are equal for results: the work is developed on `sage16`, the final results are run for both models and both forks, and what the paper includes is decided from them.
3. **Each fork is calibrated with the calibration instrument, against the same observations as its parent,** and the two are compared calibration against calibration (owner, 2026-11-23). The fork's baseline is the `sage16` calibration at the shipped ten substeps that the campaign `calibration` makes. Reproducing the parent's own population results, the target of 2026-11-12, is a second comparison the brief may keep. Galaxy-by-galaxy parity is not a goal.
4. **A refusal is a legitimate outcome.** Whether a converged, order-independent `sage16` can fit the same observations, and how far its parameters must move, is not known. "It cannot, for these statistics" is a result.
5. **The rate mode is refined where a fork needs it** (owner, 2026-11-23). The known candidates are in the backlog's [Rate mode](MIMIC-BACKLOG.md#rate-mode) section; a fork may also ask the core for a further kind of state.
6. **Modularity is protected.** The calculate, apply and enrich stages of today's splitting recipe disappear under the rate mode, and that must never mean a model-specific monolith: independently selectable physical processes stay separate runtime modules.
7. **The fork's build is not done in advance.** Writing `sage16`'s rate laws, or fixing what the fork will meet, ahead of its campaign removes what the campaign is there to find. This document records what is known, not solutions.
8. **Standing decisions on the formulation,** taken with the core work and not reopened without new evidence: it is implemented in C in the existing framework (a differentiable backend was assessed and rejected for this purpose, and the side-effect-free law contract forecloses nothing); the core gains no forward-sensitivity service and no runtime-control service; the existing module ABI is frozen and the rate contract is additive; bit-identity is claimed for one binary, never across compilers or platforms; a shared budget that must be allocated, such as infall across a group, stays explicit and model-declared, and the framework gains no arbiter; every existing package and identity gate stays green with the mode unused.

---

## What a Fork Will Meet

From [`SAGE16-PRESCRIPTION-CLASSIFICATION.md`](SAGE16-PRESCRIPTION-CLASSIFICATION.md) and [`SAGE26-PRESCRIPTION-CLASSIFICATION.md`](SAGE26-PRESCRIPTION-CLASSIFICATION.md), and from building the mode. Each is a question for the campaign's build.

- **The split is natural for `sage16`.** Of its eighteen prescriptions ten are rates, four are jumps, three are algebraic and one is forcing; the jumps are few, fire from located guards and are not entangled with a rate. A cascade of jumps ends by construction, with one pass of the discrete phases per boundary, and the question is real: `sage16`'s disk instability does not by itself restore its own guard, and `sage26` has a cascade two jumps deep.
- **The transport scratch properties become declared transfers.** Eight `sage16` properties are fluxes in transit between split operators, with their producer and consumer already recorded in metadata; under the rate mode they stop being galaxy state.
- **Every cap is a finite-step artefact that must become a law satisfying the contract.** The classifications list the load-bearing ones (the joint star-formation and feedback cap, the cooling, ejection, stripping and reincorporation caps, the negative-state repairs in infall), not every one: `sage_apply_infall.c` alone holds several, and the build must find them all. This is the largest single item and it is physics work, a re-derivation and not a port.
- **Two rates are not functions of the state alone:** the radio-mode heating radius accumulates, and supernova feedback consumes the star-formation budget of an earlier module. Both must become state functions or declared couplings before order-independence can hold.
- **Satellites couple to their central** (stripping, reheated gas, metals), so for `sage16` the integration domain is the whole FoF group in the common case, and the cost follows.
- **Beyond what the mode supports today:** observer state that depends on the substep count, a redshift held from a row's previous snapshot, rates whose time constant is the snapshot interval, stored relations re-clamped after each debit, a ratcheting heating radius with an order dependence inside one module, and a jump fired by a label inside a budget module.
- **Discrete modules beside a rate phase leave rounding residues below zero** (`sage16` does, in `MetalsHotGas`), which the mode refuses at segment start. Whether the modules clamp their own residues or the core clamps below a threshold and records it is reserved to the owner in the fork's brief (backlog, *Rate mode*).
- **`sage26` adds** the two-regime CGM with its keyed random draw, FIRE feedback, feedback-free bursts, molecular hydrogen, concentration, intracluster stars and star-formation histories. Its classification was written against the import of 2026-11-08 and is checked again against SAGE26 v1.0 once the owner has imported it.

---

## Costs and Risks

None of these is measured on a real model; the fork measures them.

- **Run time rises.** The module pipeline is about 83% of a default `sage16` run on mini-Millennium. The explicit scheme costs four evaluations of the rate laws per step and first tries each segment as one step, so with ten segments each costs at least four evaluations where the split pipeline makes one pass: if an evaluation costs what a module pass costs today, about three and a half times the whole run at the least, and more wherever a segment is cut back through rejections or needs several steps. Against operator splitting of equal accuracy, which the 66% defect puts at hundreds of substeps, the coupled scheme should be several times cheaper.
- **Calibration multiplies that cost.** A calibration is hundreds to thousands of runs, so a fork is likely to be calibrated on a part of a box and confirmed on the whole, and what the instrument costs per run is one of the things the campaign `calibration` reports.
- **A group steps at the pace of its stiffest member.** With the whole FoF group as one component, a cluster's thousands of satellites share one step sequence, and the memory of a whole-group component is not bounded (backlog, *Rate mode*).
- **Precision is the persistent memory term.** A reservoir in the rate mode must be a `double`, which widens every galaxy in the pool: the cut Shin-Uchuu's largest forest peaks at 70.96 GB for `sage16`, of which the galaxy pool is 22.9 GB.
- **Jump cadence caps the accuracy.** Inside a segment the error is set by the tolerance; across segments the timing of jumps is first order in the segment count, and no tolerance removes that; locating events inside a segment would, and is a later increment to the core that no list yet schedules. So the time-step study has a different meaning for a fork: `SubSteps` is the number of segments, the curve is expected to be much flatter than the published model's, and any stated convergence says which of the two it measures.
- **A stiff fork may need the implicit scheme,** which is first order today; a second-order scheme on the same solve is the known remedy (backlog, *Rate mode*).
- **A fork's parameters will move.** That movement is the result, not a defect.

---

## To Settle When the `sage16rate` Brief Is Written

The brief is written after `calibration` reports, as a campaign brief in the form the earlier campaigns used (the toolkit's `campaign-mode.md` and dossier template, with `campaigns/calibration/brief.md` on the Mac Studio as the nearest model). It must settle, or reserve to the owner by name:

1. **The standard of a fit.** Which observations, held back and fitted, and to what tolerance. The architecture document delegates the definition to the Campaign Manager, recorded before the runs it governs and never loosened, with the owner's own answer written first and kept outside every repository.
2. **Whether the comparison with the published model's own results is kept** beside the calibration against observations, and if so its constraint table (the parent's captured statistics, pinned by digest) and its tolerance (on the same trees there is no sample variance, so the floor is the roughness of counting galaxies in bins).
3. **How the parent's parameters correspond to the fork's.** Re-derived laws add, remove and redefine parameters, since a cap becomes a law with its own constant, so "how far the parameters must move" needs a stated correspondence, and the shifts are to be attributed to the changes that caused them. What a comparison of two calibrations reports (the regions the observations allow as well as the best points) is settled here too, from what the instrument provides.
4. **How the core is refined inside the campaign:** which rate-mode changes it may make itself under the identity gates, and which come to the owner. The backlog's open items are its starting list.
5. **The residue decision** (above), reserved to the owner.
6. **What "converged" and "order-independent" are shown by:** the tolerance and segment-count scans, and the permutations of the rate modules, on which data.
7. **Cost gates:** what a calibration of the fork may spend, on which part of which box, before the owner is asked.
8. **What the instrument lacks for a fork,** as `calibration`'s findings report it.
9. **The hold-out discipline:** what the campaign may not read (this directory among it), so that its build and its judgement are its own.

`sage26`'s fork gets its own brief after `sage16`'s has reported and SAGE26 v1.0 is in Mimic, from the same list and from what the first fork taught.

---

## Inputs

- [`SAGE16-PRESCRIPTION-CLASSIFICATION.md`](SAGE16-PRESCRIPTION-CLASSIFICATION.md) and [`SAGE26-PRESCRIPTION-CLASSIFICATION.md`](SAGE26-PRESCRIPTION-CLASSIFICATION.md): every prescription of both models classified, with the caps, the impure rates and the cross-galaxy couplings cited by file and line. They cite the archived brief for the formulation.
- [`MIMIC-BACKLOG.md`](MIMIC-BACKLOG.md#rate-mode): the rate mode's open follow-ups, each with what would accept it.
- [`AGENTIC-SCIENCE-ARCHITECTURE.md`](AGENTIC-SCIENCE-ARCHITECTURE.md#the-road): stage 4b and stage 5, what 4b is known to need, and the lessons on designing a campaign that tests judgement.
- [`MIMIC-DEVELOPMENT-PATHWAY.md`](MIMIC-DEVELOPMENT-PATHWAY.md#the-sequence-from-here-owner-2026-11-17): the order, and the state of each step.
- The campaign `calibration`'s findings, when it has reported (`campaigns/calibration/findings.md` on the Mac Studio): the instrument, its limits, and `sage16`'s calibration at the shipped time step.
- The archived brief (`archive/dev-plans/MIMIC-COUPLED-RATE-FORMULATION-PLAN-2026-11-23-stage-1-brief.md`): the formulation in full, the state kinds, the ledger, the integrator, the tolerance and step policy, the proving fixture and the prior art, for whoever needs the reasoning behind the mode as built.
