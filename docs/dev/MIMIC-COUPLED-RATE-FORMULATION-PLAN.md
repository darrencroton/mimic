# Mimic Coupled Rate Formulation Plan

**Status:** Requirements brief. Not scheduled. Complete enough to derive a frozen implementation plan from, once two prerequisites exist: the measurement in [First Work](#first-work-measure-before-building), and the per-prescription hybrid classification (Open Question 1).
**Note, updated 2026-08-20:** the classification prerequisite is discharged ([`SAGE16-PRESCRIPTION-CLASSIFICATION.md`](SAGE16-PRESCRIPTION-CLASSIFICATION.md)), and the ordering question this brief once fed is settled — `MIMIC-DEVELOPMENT-PATHWAY.md` → "The Ordered Road" schedules this work as step 5, after the snapshot-global modules, and treats it as certain rather than contingent. **No core change starts ahead of the measurement spike**, which stays fixture-scale and independent of the Shin-Uchuu sequence; what it sets is priority, solver family and the attribution baseline, not permission.
**Date:** 2026-08-18

---

## Goal

Replace sequential operator-split module execution with a coupled formulation in which physics modules **declare conservative transfers** between named reservoirs, and a physics-agnostic integrator advances the assembled system with error control. Physics that genuinely resets state stays discrete and explicitly ordered.

The objective is mathematical consistency: a model whose output does not depend on the order its modules are listed in, whose conservation is structural rather than audited, and whose numerical accuracy is set and measured rather than inherited.

---

## Motivation

Mimic evolves galaxies by walking a configured list of modules, `SubSteps` times per snapshot interval (ten in the shipped `sage16` configuration). Each module reads galaxy state, computes its effect, and writes it back before the next module runs — see `execute_module_pipeline()` in `src/core/module_registry.c` and the `process(ctx, halos, ngal)` contract in `src/core/module_interface.h`.

The consequence is that **the scientific result depends on the order of the list**. Run cooling before star formation and star formation sees gas that has already arrived; reverse them and it does not. Both orderings are equally defensible and they produce different galaxies. The same holds for every competing pair in the pipeline, compounded over every substep, snapshot, and tree.

For any prescription that is genuinely a rate, this order dependence is a first-order splitting artefact: a property of our bookkeeping rather than of the physics, and one that shrinks as the timestep shrinks. But what it converges *to* has never been specified. The pipeline mixes true rates with staged budgets, hard caps, and immediately dispatched events, and no document states which mathematical object that mixture approximates. Specifying it is the substance of this work.

Two consequences follow that are worth stating plainly:

- **Calibrated parameters absorb the residual.** `sage16`'s parameters were tuned against a pipeline carrying an unquantified splitting error. Some fraction of every calibrated value is compensating for numerics.
- **We cannot presently answer a reviewer who asks how converged the model is**, because there is no convergence rate to report and no error target that was set.

---

## Target Formulation

### Hybrid system, not a pure ODE

The target is a hybrid system: coupled continuous flow between events, explicit jumps at events.

```
ẋ = f(x, h(t), θ)        continuous transfer between reservoirs
x⁺ = J(x⁻, h, θ)         discrete jumps at located event times
```

Mergers, disruption, and instantaneous restructuring are jumps and stay jumps; forcing them into rate form would be less physical, not more. A threshold alone does **not** make a process discrete — a prescription that is zero below a critical density and a smooth rate above it is still a rate with a crossing to locate. Each existing prescription must be classified by its state-reset semantics, not by the presence of a threshold. That classification is a prerequisite deliverable (see [Open Questions](#open-questions)).

### Modules declare transfers, not updates

Today a module *applies* its effect to galaxy state. Under this formulation it *declares* a flux — a mass rate, out of one named reservoir and into another — without touching state at all. The integrator decides how declared fluxes are realised in time.

Two properties follow, and they are the whole point:

- **Conservation becomes structural.** Because every flux names both a source and a destination, mass cannot go missing inside a closed set of reservoirs through a coding error. What genuinely enters or leaves — infall from outside the halo, metals produced in stars — must be declared as a source or a sink and is therefore *visible* as one, instead of hiding inside an unbalanced update. Hand-written rate equations conserve when their author is careful; declared transfers conserve because nothing else is expressible.
- **Declaration order stops mattering.** Fluxes evaluated at a common state simply sum. Within a phase, rate terms become an unordered set, and a whole class of silent configuration error becomes inexpressible in a run file.

The second property holds **only if every rate is a pure function of the declared state, forcing, and parameters**. Some current prescriptions are not: cooling is modified by accumulated heating (`Rheat`), and feedback consumes the star-formation budget computed by an earlier module. Those dependencies must become explicit state functions or declared algebraic couplings. This is part of the re-derivation, not an afterthought.

### Depletion is handled by rate laws, not adjudication

When two processes want the last of the cold gas, today whichever runs first wins and each caps its own demand with `min(requested, available)`. This is the sharpest form of the order problem.

It dissolves in a continuous formulation **provided each rate is source-limited**: a flux that scales with its source vanishes as that source empties, so competing draws throttle each other automatically. This is a constraint on how rate laws are written, plus a positivity-preserving integrator — deliberately **not** a new arbitration mechanism inside the framework. An arbiter that rations competing demands would import finite-step thinking into a continuous system, introduce a physics policy into physics-agnostic infrastructure, and add nonsmooth behaviour for the solver to fight.

A negative committed state is therefore an invariant violation, not a value to repair: reduce the step, and if the invariant cannot be recovered, fail. **Never clamp.** This is the existing fast-failure principle (`docs/VISION.md` §7) applied to the solver.

Genuine shared finite budgets that must be allocated — infall distribution across a group is the candidate — stay explicit and model-declared rather than becoming a general framework mechanism.

### Integration domain

The integration domain is **each connected component of the declared transfer graph** within the processing workspace, not the whole workspace by default.

Where physics genuinely couples galaxies — satellite feedback depositing into its central's hot halo — that component spans those galaxies and solver cost follows. Where it does not, galaxies remain independently solvable and by-galaxy dispatch survives unchanged. We neither pay group-scale solver cost for galaxy-local physics, nor fake independence where the physics couples.

---

## Relationship to the Vision

This work is governed by `docs/VISION.md`. Assessed against each principle:

| Principle | Effect |
|---|---|
| 1. Physics-agnostic core | **Strengthened.** The integrator, the transfer assembler, and event location are pure numerics — they know nothing about galaxies, exactly like the allocator and the HDF5 writer |
| 2. Runtime modularity | **Strengthened, with one deliberate refinement — see below** |
| 3. Metadata as structural truth | **Extended.** Reservoirs, their conservation relationships, and transfer endpoints become declared metadata |
| 4. One coherent processing model | **Preserved.** Rate terms are a dispatch mode within the existing model, not a new traversal |
| 5. Bounded memory | **Preserved.** Integrator stages cost a fixed small multiple of the state vector — see the memory note in [Costs and Risks](#costs-and-risks) |
| 6. Format-agnostic I/O and provenance | **Untouched** |
| 7. Validation and fast failure | **Strengthened.** Non-conservation and negative reservoirs become detectable invariant violations rather than silently absorbed |

### The one deliberate refinement

`docs/VISION.md` §2 states that *scientific ordering remains explicit in configuration*. This work refines that:

- **Jumps retain explicit configured ordering**, because they genuinely do not commute.
- **Continuous rates become unordered**, because ordering them was never physics in the first place — it was an artefact of sequential execution.

This is a deliberate, documented change to a vision statement, not an oversight, and it should be reflected in `docs/VISION.md` when this work is promoted from brief to plan.

### Modularity is protected, explicitly

Several current modules are not physical processes but stages of a splitting recipe — calculate, then apply, then enrich. Those stages disappear, and the module count drops. **This must never mean a model-specific monolith.** Independently selectable physical processes remain separate runtime modules; the core must still assemble whatever compatible set a run file configures, including the empty set. Collapse means fewer bookkeeping steps, never fewer scientific choices.

---

## Constraints Carried Forward

- **The existing FoF-scoped module ABI stays frozen.** The rate-term contract is **additive**: a new processing mode alongside `PROCESSING_MODE_FULL_HALO` / `BY_GALAXY` / `PER_EVENT`, declared in module metadata. `process(ctx, halos, ngal)` is not changed and not removed. This is what allows existing model packages — including the control — to keep running unmodified, and it is what keeps this work compatible with the three briefs that assume a frozen ABI (see [Relationship to Other Plans](#relationship-to-other-plans)).
- **The new physics lives in a new model package.** `sage16` is not converted in place; it becomes the experimental control.
- **Faithful housing of published models is a first-class goal, not a concession.** `sage16` is a published model and Mimic's stated purpose is to house it as written. A published model's numerics are part of what was published: reproducing it *requires* reproducing its operator splitting, order dependence included. The frozen mode is therefore not legacy baggage to be retired — it is the mode in which any externally published SAGE-like model can be imported and run faithfully, without modification and without silently improving it. The new mode is for models we are free to derive properly: an improved successor, and anything built from scratch afterwards. Housing both is the flexibility Mimic exists to provide.
- **The existing event system is reused, not replaced.** Jump modules keep `PROCESSING_MODE_PER_EVENT` dispatch and `module_emit_event()` (`src/core/module_interface.h`) as they work today. This work adds a rate-term mode beside them; it does not redesign event contracts, subscription routing, or the producer/consumer validation already in place.
- **Rate laws must be side-effect free.** An adaptive integrator evaluates them repeatedly, out of chronological order, and at trial states it will subsequently reject. No counters, no emitted events, no writes to galaxy state. Jump modules keep a separate, frankly mutating contract.
- **Working state is `double`.** Adaptive error control against `float` reservoirs is not meaningful. This breaks SAGE storage parity, which is acceptable in a new package and is the settled precedent for non-parity packages.
- **Determinism.** Assembled transfers must reduce in a canonical order so results are reproducible independent of module registration order.
- **Cross-format identity must stay green** for existing packages on both drivers. This work adds a mode; it must not perturb the tree/snapshot identity gate for models that do not use it.

---

## Relationship to Other Plans

**Three of the five remaining requirements briefs assume the physics-module ABI is frozen.** The additive-contract constraint above is what makes that assumption survive this work, and it is the single most important interface decision in this document.

- **`MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md` — shares machinery; design jointly or sequence this first.** That brief adds a snapshot-scoped processing mode; this one adds a rate-term processing mode. Both extend the same dispatch and metadata machinery, and both were written assuming the other did not exist. Extending it twice independently is waste, and worse, risks two incompatible notions of "a mode". Its own constraint — *"a snapshot-global contract is additive … never a change to `process(ctx, halos, ngal)`"* — is satisfied by this brief's additive constraint, so there is no conflict of substance, only of sequencing economy. Note also that snapshot-global operations (rank-order SHAM, environment measures) are **population operations, not transfers**; they do not join the coupled system and are genuinely orthogonal in physics terms.
- **`MIMIC-DISTRIBUTED-SNAPSHOT-PLAN.md` — downstream, but carries a real constraint.** Domain decomposition must not cut a connected component of the transfer graph, or the coupled solve is split across ranks. That is far cheaper to know while designing decomposition than to retrofit afterwards. This brief should be settled before that one is promoted.
- **`MIMIC-EMBEDDED-ENGINE-PLAN.md` — strongly synergistic; sequence after.** That brief records that reentrancy is blocked by `init(void)`/`cleanup(void)` reading globals, and that threading state through `ModuleContext` covers only the `process()` path. A side-effect-free transfer declaration is trivially reentrant by construction. Building the embedded engine against the imperative ABI first would be effort spent on the contract this work supersedes for new models.
- **`MIMIC-MODEL-BUILDER-PLAN.md` — strongly synergistic; sequence after.** A declared transfer (source reservoir, destination reservoir, rate law, units) maps far more directly onto "extract equations, parameters, units from a paper" than an imperative `process()` body does, and is far more machine-checkable. That brief states it should not drive architecture except where it reinforces already-valid constraints; this work reinforces exactly the ones it names.
- **[`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md) — supplies part of this brief's acceptance evidence; sequence its spike alongside, not after.** Gate item 5 requires the new package's differences from `sage16` to be quantified and attributed, and Open Question 7 asks which observables define acceptance. That brief is the instrument for both, and it is independent of this one: it adds no processing mode and touches none of the dispatch or metadata machinery, so the two do not contend. Note one interaction in the other direction — a declared transfer (source reservoir, destination, rate law, units) is a far better emulator design vector than an imperative `process()` body, so this work makes that one easier rather than harder.
- **Snapshot pathway (`MIMIC-DUAL-DRIVER-PLAN.md`, `SHIN-UCHUU-CONVERSION-PLAN.md`) — prerequisite, and already delivered the seam this work needs.** Phase 5 extracted `process_halo_evolution()` into `src/core/halo_evolution.c` parameterised by `struct HaloInputView`, giving both drivers a single evolution entry point; it also made the galaxy pool an instanced object and the output seam driver-neutral. This work must be built on that foundation, not on the pre-Phase-5 tree-driver-shaped one.

---

## Costs and Risks

- **This is a re-derivation, not a port.** Rewriting hard caps as source-limited rate laws is the largest single item and is physics work, not engineering. Making every rate a pure function of state is part of it.
- **Parity is not the goal, and parameters will move.** The new model will not reproduce `sage16` and should not. The deliverable is a new model package, a recalibration, and a quantitative comparison against the control.
- **Solver memory is a new term in a budget under active pressure.** An RK-class integrator holds several copies of the state vector per component. `POST-PHASE-5-WORK.md` §2.2 is actively tracking the Shin-Uchuu memory projection against an 85%-of-RAM fallback trigger, with the galaxy-pool high-water still unmeasured. This work must be costed against the rehearsal's measured numbers, not against the pre-rehearsal projection.
- **Stiffness is a live unknown.** Cooling, dynamical, and feedback timescales span orders of magnitude. An explicit integrator may crawl or fail in the massive-halo regime; an implicit method needs a Jacobian, which for a component of dimension N costs order-N extra evaluations by finite differences.
- **Scale interaction with snapshot-global work.** A coupled component confined to a FoF group is a small object. If a future snapshot-global module introduces transfers across a co-resident population, the component could in principle span the box. The transfer-graph framing handles this correctly in principle, but the scale is qualitatively different and is not in scope here.
- **Event location is deferrable but not free.** A first implementation may fire events at step boundaries as today; locating them at their guard crossings is a second increment.

---

## Language and Backend

**Implement in C, in the existing framework.** The formulation is language-independent, and the goal — mathematical consistency — is not what automatic differentiation provides.

A JAX or AD-based backend was assessed and rejected for this purpose. In brief: reverse-mode differentiation retains evolution history and needs checkpointing or adjoint methods to respect bounded memory; ragged, topology-changing merger trees suit array compilers poorly; the dependency floor rises from a C compiler to a pinned Python and toolchain stack; the MPI model does not carry over; and maintaining physics in two languages requires a permanent cross-language parity harness. Mature stiff ODE solvers for C are long established and sufficient.

The one argument that could reopen this is cost: if measured component dimension N proves large and stiffness forces an implicit solver, exact Jacobians become valuable. That is a decision to revisit **after** the transfer contract is defined and N is measured — not before. Note also that the side-effect-free declarative contract this work introduces is the prerequisite for any future differentiable backend, so this choice forecloses nothing.

---

## First Work: Measure Before Building

**No core change should be made until the size of the defect is known.**

Build a throwaway model package containing a single monolithic integrator module — legal today under the existing ABI, using `process_full_halo` with `SubSteps: 1`, with physics terms as model-local helpers and zero core changes. Then measure the current pipeline against a conservative coupled reference for the rate-shaped subset:

- several module orderings, to size the order dependence directly;
- timestep refinement, to size the splitting residual;
- Strang splitting, as the cheap comparator;

stratified by halo mass and redshift, and reported against familiar `sage16` statistics.

Symmetric splitting is a **comparator, not an alternative**: it reduces error for two operators without removing permutation dependence among many, and it does nothing about the undefined hybrid semantics. The measurement sets priority and sizes the defect. It does not decide whether the defect is real.

**Nor does it decide whether the work happens. Recorded 2026-08-20, so the spike is never read as a veto.** Sequential operator splitting has no defined answer to adjudicate: permuting the module list in a run file changes the result, and that ordering carries no physics. A small residual measured for `sage16` at z = 0 constrains this package at this epoch — it says nothing about a stiffer successor, nor about massive halos where cooling and dynamical timescales separate. The declarative benefits are independent of the residual's size in any case: machine-checkable conservation, a side-effect-free process path, and transfers that map onto published equations. What the measurement sets is **priority, solver family, and the baseline the eventual comparison is attributed against** — not permission.

This spike touches only new files under `models/`, so it is safe to run alongside any other in-flight work.

---

## Open Questions

To be settled before or during the implementation plan, not by it:

1. ~~**Per-prescription hybrid classification.**~~ — **SETTLED 2026-08-20**, in [`SAGE16-PRESCRIPTION-CLASSIFICATION.md`](SAGE16-PRESCRIPTION-CLASSIFICATION.md). All 18 shipped `sage16` prescriptions classified by state-reset semantics with `path:line` evidence: 10 rate, 4 jump, 3 algebraic, 1 forcing. Three results feed the questions below — the eight operator-split fluxes are already declared transport buffers with producer→consumer notes (bears on Question 2), the six `min(requested, available)` sites and two clamp-to-zero sites are enumerated (the clamps are a real behavioural difference, so acceptance must be observational — Question 7), and the transfer graph connects the whole FoF group in the common case (bears on cost and on Question 9).
2. **Metadata mechanism.** Whether reservoirs and their roles are declared explicitly in property YAML, or derived from module transfer endpoints with validation for undeclared writes. Derivation is preferable if it can be made complete and fast-failing.
3. **Solver family.** Explicit adaptive versus semi-implicit or Rosenbrock, decided from measured stiffness rather than assumed.
4. **Tolerance policy.** Component-wise absolute and relative tolerances, acceptance norms, and the precision at which state is committed to output.
5. **Event semantics.** Step-boundary firing (first increment) versus located guard crossings; simultaneous-event priority; cascading jumps; restart rules.
6. **Tree forcing.** Halo properties arrive as discontinuities at snapshot boundaries. Integration should terminate and restart there rather than letting an adaptive solver fight the jump; whether forcing is additionally interpolated within an interval is a separate scientific choice.
7. **Recalibration methodology**, and which observables define acceptance for the new package. **A candidate instrument is on record: [`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md) (2026-08-20).** Its relevance here is the comparison, not the calibration: Gate item 5 asks for this package's differences from the control to be *quantified and attributed*, and a best-fit score cannot compare two structures on equal footing because it rewards whichever has more freedom. Surviving parameter-space fraction compares them fairly, provided each structure's prior measure is stated and comparable; attributing a particular output difference to a particular physics change remains a direct control-versus-package comparison that no emulator performs. Point calibration itself remains an optimiser's job.
8. **Package naming.** A working name such as `sage16-coupled` is self-documenting while the package is explicitly the coupled counterpart of the control. Once recalibrated it is a scientifically distinct model with its own parameters and results, and carrying the `sage16` name would misstate its provenance — so plan to rename at that point. Cheap to defer: it is a directory name, and nothing cites it yet.
9. **Joint design with `MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`** on the shared dispatch-mode and metadata machinery.

---

## Gate (when activated)

1. The measurement in [First Work](#first-work-measure-before-building) is complete and published, and its results have set the solver family, the error target, and the baseline that gate item 5's attribution is measured against.
2. A new model package reproduces the classified rate-shaped subset as declared transfers, with a machine-checked conservation ledger and a demonstrated convergence rate under step refinement.
3. Order-independence is demonstrated: permuting rate terms in the run file produces bit-identical output.
4. All existing packages, both drivers, and the cross-format identity gate stay green with the new mode unused.
5. The new package is recalibrated, and its differences from the control are quantified and attributed.
