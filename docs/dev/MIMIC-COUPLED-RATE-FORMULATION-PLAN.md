# Mimic Coupled Rate Formulation Plan

**Purpose:** State what the coupled rate formulation is, what Mimic's core must gain to run it, and what must be true before that work is accepted, in enough detail to derive a frozen implementation plan from.

**Status:** Requirements brief; pathway step 7 and the next work on the road. Revised 2026-11-12 from the brief of 2026-08-18, against the code as it stands at v1.6 and against [`AGENTIC-SCIENCE-ARCHITECTURE.md`](AGENTIC-SCIENCE-ARCHITECTURE.md), which splits the work in two. This brief governs the first part. Revised again on 2026-11-12 after the implementation plan was derived from it: the owner's decisions are taken, and the plan is [`MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md`](MIMIC-COUPLED-RATE-IMPLEMENTATION-PLAN.md). The earlier text is in git history (`56ed8484` and before).

**Scope:** The rate mode in Mimic's core: a new processing mode, the assembly of declared transfers, the integrator, the conservation ledger, their metadata and their tests. It is built from an ordinary implementation plan under the Project Manager and proven on a model-neutral fixture. Putting a real model on the mode is campaign work, described under [The Campaigns That Follow](#the-campaigns-that-follow), and no model package changes in this part.

---

## Goal

Replace sequential operator-split module execution, for the models that choose it, with a coupled formulation in which physics modules **declare conservative transfers** between named reservoirs and a physics-agnostic integrator advances the assembled system with error control. Physics that resets state stays discrete and explicitly ordered.

The objective is mathematical consistency: a model whose output does not depend on the order its rate terms are listed in, whose conservation is checked by the framework, and whose numerical accuracy is set and measured.

---

## Motivation

Mimic evolves galaxies by walking a configured list of modules. `execute_module_pipeline()` (`src/core/module_registry.c`) runs the `pre_timestep` phase once, then the named phases under `modules.phases` once per substep, then `post_timestep` once. Each module reads galaxy state, computes its effect and writes it back before the next module runs.

The consequence is that **the scientific result depends on the order of execution**. Run cooling before star formation and star formation sees gas that has already arrived; reverse them and it does not. Both orderings are defensible and they give different galaxies. Within a phase the order is also set by dispatch: full-halo modules run first and by-galaxy modules follow galaxy by galaxy, so wherever a satellite writes into its central the result depends on workspace row order too.

For a prescription that is a rate, this is a first-order splitting artefact that shrinks with the time step. What it converges to has never been specified, because the pipeline mixes true rates with staged budgets, hard caps and immediately dispatched events.

Two facts make this concrete:

- **The shipped `sage16` is not converged.** At its ten fixed substeps, total z = 0 stellar mass on mini-Millennium is 66% above its value at 1,000 fixed substeps. The mass function differs by 0.08 dex RMS below the knee and by 0.4 to 0.8 dex above it. The fixed and dynamic schemes converge to one answer, roughly first order in the step (measured 2026-11-10).
- **Calibrated parameters absorb the residual.** `sage16`'s parameters were tuned against that pipeline, so some part of every calibrated value compensates for numerics. Order dependence itself has not been measured.

---

## What This Brief Governs

| Part | Work | How it runs | Governed by |
|---|---|---|---|
| **Stage 1** | The rate mode in the core, proven on a fixture, then a release | A frozen implementation plan under the Project Manager | This brief |
| **Stage 4a** | The numerics of the published models as shipped: time-step convergence and order dependence | A science campaign | The architecture document |
| **Stage 4b** | A fork of `sage16` on the rate mode (`sage16rate`), recalibrated to reproduce the published results if that is possible | A science campaign | The architecture document, with the requirements here as its inputs |

Three things follow from the split, each a change from the 2026-08-18 brief:

- **The core is built before any measurement on real physics.** The earlier brief required the defect to be sized first. Its size is now partly known (above), the owner has set the priority, and the measurement is campaign work.
- **The solver is chosen without a stiffness measurement.** Writing `sage16`'s rate laws to measure their stiffness would do part of the campaign's build. [The integrator](#the-integrator) says how the choice is kept open and how the campaign then measures stiffness at no extra cost.
- **Acceptance of this part rests on the fixture.** No production physics is needed to pass [the gate](#gate).

---

## Target Formulation

### A hybrid system

The target is a hybrid system: coupled continuous flow between events, explicit jumps at events.

```text
dx/dt = f(x, h(t), θ)     continuous transfer between reservoirs
x⁺    = J(x⁻, h, θ)       discrete jumps
```

Mergers, disruption and instantaneous restructuring are jumps and stay jumps. A threshold alone does not make a process discrete: a prescription that is zero below a critical density and a smooth rate above it is a rate. Each prescription is classified by its state-reset semantics, and the two published models are already classified ([`SAGE16-PRESCRIPTION-CLASSIFICATION.md`](SAGE16-PRESCRIPTION-CLASSIFICATION.md), [`SAGE26-PRESCRIPTION-CLASSIFICATION.md`](SAGE26-PRESCRIPTION-CLASSIFICATION.md)).

### Modules declare transfers, not updates

Today a module applies its effect to galaxy state. Under this formulation a rate module declares a transfer, out of one named reservoir and into another, and does not touch state. The integrator decides how declared transfers are realised in time.

- **Every transfer has two ends.** Mass cannot go missing inside a closed set of reservoirs through a coding error, because one computed amount is subtracted from the donor and added to the recipient. What enters or leaves the system (infall, metals made in stars) is declared as a source or a sink and is visible as one.
- **Declaration order stops mattering.** Rate terms evaluated at a common state sum. Within a rate phase they are an unordered set.

The second property holds only if every rate is a pure function of the declared state, the forcing and the parameters. Some current prescriptions are not: cooling is modified by accumulated heating, and feedback consumes the star-formation budget an earlier module computed. Those dependencies become state functions or declared couplings.

### The rate-law contract

"Source-limited" needs a precise form, because it is what makes negative reservoirs impossible without clamping.

- **A transfer out of a reservoir is declared as a specific rate,** whether its recipient is another reservoir or outside the system (a sink). The module returns a non-negative coefficient `k(x, h, θ)` in units of inverse time, and the core forms the flux as `k` times the donor's content. A flux that scales with its donor vanishes as the donor empties.
- **Only a source from outside the system is declared as an absolute rate.** An absolute sink is refused.
- **Fluxes are non-negative.** Flow in the reverse direction is a second transfer.
- **A transfer between two reservoirs stays within one conserved total.** Movement between totals is a sink in one and a slaved source in the other, so each total's ledger can close.
- **A law is continuous in the state and receives only the state, the forcing, the parameters and the time.** It never sees the solver's step or a substep index. The snapshot interval is legitimate forcing.
- **A law has no side effects.** The integrator evaluates it repeatedly, out of chronological order, and at trial states it later rejects.

This form does more than limit the source:

- **The positivity bound is computable.** A forward Euler step keeps every reservoir non-negative when the step times the reservoir's total specific outflow is at most one. Because the outflow depends on the state, it changes between the stages of a step, so the core bounds the step before it takes it and verifies the bound at each stage; a stage that violates it is rejected.
- **Tracers come free.** A companion reservoir (the metals in a gas reservoir) moves at the same `k` as its parent, so its flux is `k` times its own content and no division by a possibly empty parent is needed.
- **An ill-posed law fails loudly.** A coefficient that grows without bound as its donor empties forces the step towards zero and trips the failure rule below, which is the intended outcome.

What the form excludes is deliberate: a constant drain (it cannot be declared, since an absolute sink is refused), `min(demand, available / dt)`, and any law that empties a reservoir in finite time. Each of today's caps becomes a law that satisfies the contract, which is physics work for the campaign and not for the core.

### Negative state is a violation, not a value to repair

A committed negative reservoir is an invariant violation. The integrator reduces the step, and if it cannot recover it fails. **It never clamps.** This is `docs/VISION.md` principle 7 applied to the solver.

The rule needs a termination condition to be implementable:

- **Negative means strictly below zero at any stage.** It is checked on every computed stage and on the committed state, before any law sees it.
- **Retries and steps per segment are bounded** by declared budgets.
- **A failure names its cause:** the component, the reservoir, the law and the time, with enough state written out to reproduce it. One halo in ten billion must not end a production run without a diagnosis.
- **Nothing is committed from a failed segment.**

Genuine shared budgets that must be allocated, such as infall across a group, stay explicit and model-declared. The framework gains no arbiter.

### Phases, segments and time

The existing phase structure carries the hybrid system with one new rule.

- **A phase is either a rate phase or a discrete phase.** A rate phase holds only rate modules and is integrated as one system. A discrete phase holds modules in the existing modes and runs in order, as today. A phase that mixes the two is refused at startup.
- **At most one rate phase is configured.** Two would be split against each other in order, which is the artefact this work removes. The limit is lifted only on a demonstrated need. A rate mode is also refused in `pre_timestep`, `post_timestep` and `post_snapshot`.
- **A substep is a segment.** The integrator advances the rate phase across one segment, steps freely inside it and lands exactly on its end. Discrete phases run at segment boundaries. `SubSteps` and `TimestepScheme` keep their meaning and now set the cadence of jumps, per FoF group under the dynamic scheme.
- **Convergence is two separate claims.** Inside a segment the error is controlled by the tolerance. Across segments the timing of jumps is first order in the segment count, and no tolerance removes that. Any stated convergence rate says which one it measures.
- **Halo properties arrive as discontinuities at snapshot boundaries.** Integration stops and restarts there. Whether forcing is also interpolated within an interval is a scientific choice for a model, not for the core.

**Rows in one FoF group can have different elapsed times, and each advances on its own clock** (the owner's decision 3). Each row carries its own `dT`, the published `sage16` evolves each object over its own `dT / num_substeps`, and mixed `dT` within a group is tested today (`models/sage16/modules/_tests/test_unit_mixed_dt_parity.c` and `models/sage26/modules/_tests/test_unit_mixed_dt_parity.c`). In the rate mode the rows of a component share one normalised segment time, running from the segment's start to its end, and each row covers its own elapsed time across it. Each transfer's amount is set on its donor's clock and applied to both ends, so a transfer between rows of different elapsed times still conserves. This is the continuous limit of what `sage16` does today.

### State kinds

"A source and a destination" does not cover everything a real model declares. The published models need these kinds. Property metadata declares reservoirs, tracer companions and observers; external sources, sinks and slaved sources are ends of declared transfers, not properties:

| Kind | Meaning | In stage 1 |
|---|---|---|
| Reservoir | A conserved, non-negative quantity that transfers move | Yes |
| Tracer companion | A quantity carried with a parent reservoir at the parent's specific rates (metals in gas) | Yes |
| External source or sink | The end of a transfer outside the system: a source at an absolute rate, a sink at a specific rate out of a reservoir. Its amount is integrated as solver state for each segment so that the ledger can close | Yes |
| Observer | The time integral of a declared flux, read out and never fed back (a star-formation rate, a history bin) | Yes |
| Slaved source | An external source into a different conserved total, proportional to a named transfer's flux (metals yielded by star formation). One level deep: its master is a transfer out of a reservoir, never a source. It makes its recipient's total depend on its master's, and a dependency cycle among totals is refused | Yes |
| Auxiliary state | Non-conserved state that evolves but is not a mass (a merger clock, an accumulated heating radius) | No: it stays in discrete phases |
| Label | A discrete value that changes only at a jump or a snapshot boundary (a galaxy type, a keyed random regime draw) | Read as forcing |

### Integration domain

A transfer's two ends are either on the same record or on a galaxy and its FoF central. Those are the only cross-record transfers the two classifications show. The domain is therefore decided at startup from declared metadata: each galaxy alone when no configured transfer crosses records and no configured module declares that its laws read the central's state, and the whole FoF group otherwise. A law that has not declared it is given no view of the central, so a law in the per-galaxy domain cannot depend on another galaxy.

For `sage16` it is the whole group in the common case, because satellites return gas and metals to their central. By-galaxy independence does not survive for that model, and the cost follows ([Costs and Risks](#costs-and-risks)). A general transfer graph with component discovery waits until a model declares an endpoint that needs it.

### The ledger

Conservation is checked, not assumed. A Runge–Kutta scheme written in increment form, with one computed amount per transfer per stage combination applied to both of its ends, preserves each conserved total to rounding, not exactly, and jumps and open systems are outside that argument altogether. The ledger is three things:

- **External sources and sinks are integrated** by the same scheme as the state, so their totals over a segment are known.
- **Closure is asserted per component per segment:** the change in each conserved total equals the net external flow, within a tolerance that scales with the machine precision, the number of steps, the number of terms on each reservoir, and the size of the total and of the external throughput.
- **Every discrete phase's net change to each conserved total is recorded,** and is either zero or declared by the module that caused it. The core attributes a change to the dispatch unit it measures around: one full-halo module's call with the event consumers it triggers, or a phase's by-galaxy pass, whose modules run galaxy by galaxy and cannot be separated without a sum per call.

A tracer above its parent beyond a rounding tolerance is a failure, and the bound is checked. Both schemes keep a tracer within its parent when every inflow to the tracer is matched by a parent inflow at least as large. Once a tracer has sources of its own, such as a slaved yield, the exact solution can pass the bound, so it is then a property of the model and not of the integrator: the failure means either an implementation error or a model whose tracer sources imply a concentration above one.

### The integrator

No external library is used. The build links only the maths library, libyaml, HDF5 and MPI, and an ODE package would raise that floor while suiting short, cold-started, per-group integrations poorly.

The integrator sits behind one internal interface: it is given a component's state, the means to evaluate the coefficients, a segment and the tolerances, and it returns the advanced state, a status and its step statistics.

- **First, an explicit strong-stability-preserving Runge–Kutta pair with an embedded error estimate:** the four-stage third-order scheme with a second-order embedded estimate, in place of the three-stage scheme, because it allows twice the positivity-limited step for one more evaluation and so costs a third less where positivity limits the step. Every stage of such a scheme is a convex combination of forward Euler steps. With the rate-law contract its step is bounded before it is taken and the bound is verified at each stage, every computed stage is checked non-negative before a law sees it, and a step that fails either check is rejected. With a margin below one on the stage-wise bound, reducing the step terminates whenever the coefficients are continuous at the step's start, and the budgets terminate it otherwise. It needs no Jacobian.
- **Second, behind the same interface, a linearly implicit first-order scheme with coefficients frozen at the start of the step.** Its matrix is a column-diagonally-dominant M-matrix by construction; solved by elimination without subtraction and without pivoting, it is non-negative at any step size and conserves to rounding. It is the fallback for a stiff component. It also removes the loud failure above, because it will integrate an ill-posed law without complaint, so it is never the default.

Unconditionally positive linear methods are limited to first order, which is why no higher-order linear implicit method is proposed. Nonlinear Patankar-type schemes reach higher order, but they weight fluxes by ratios of stage values, which breaks the tracer property and needs a convention at an empty reservoir, so none is proposed for stage 1.

The stiffness of the real models is not measured. A timescale estimate puts the product of the fastest specific rate and a snapshot interval between one and a hundred, which an explicit scheme handles; that is an estimate and not a measurement. So the core records what a measurement would have given: per run, the distribution of the fastest specific rate times the segment length, and of steps per segment. Campaign 4b reads the stiffness of the fork from its own output.

### Tolerance and step policy

The plan fixes the formula. The brief fixes its properties:

- **Component-wise weights** with an absolute and a relative part, the absolute part non-zero so that an empty reservoir has a scale. Both tolerances are dimensionless, and the absolute part is scaled by the galaxy's own total, so the norm does not depend on the units of mass.
- **A galaxy-local scale.** A dwarf in a cluster is not ignored because the cluster is large.
- **A maximum norm,** so that a galaxy's accuracy does not depend on the size of its group, and the norm does not depend on the order its terms are evaluated in.
- **The tolerance is a local error target and is stated as one.** Accuracy is measured by rerunning at a tighter tolerance, not read off the setting.
- **The step sequence is a pure function** of the state at the start of the segment, the forcing, the parameters, the segment and the tolerances. The integrator starts cold in every segment, at the cost of one extra law evaluation to choose the first step, and carries nothing between segments or between groups.

### Events and jumps

In stage 1 jumps fire at segment boundaries, in discrete phases, exactly as events fire today: from full-halo producers to per-event consumers in the same phase, dispatched immediately. Locating an event at its guard crossing is a later increment, and four requirements now keep it from needing a redesign:

- **The integrator advances one segment and returns, and the driver owns segmentation.** An early return at a located time is then an addition.
- **State is gathered from the records at the start of a segment and written back at its end.** A change in the number of galaxies (a merger, or records created by `module_create_record()`) is then free, because nothing is cached across callbacks.
- **Jumps see committed state only,** and the integrator restarts cold after any jump.
- **A cascade terminates by construction:** one configured pass of the discrete phases per boundary. `sage16`'s disk instability shows the question is real, since its reset does not by itself restore its own guard, and `sage26` has a cascade two jumps deep.

---

## How It Meets the Core As Built

A great deal has landed since this brief was first written. Each item below is a fact about the code and the requirement it places on the rate mode.

**The mode machinery is a closed table, not an open set.** `src/core/processing_modes.c` holds the single C table of modes, with a static assertion that every enumerator has an entry, and `scripts/module_modes.py` holds its Python counterpart, which is an input to the generation hash. FoF phases refuse a mode of any other family, and `struct PhasePlan` holds three fixed dispatch lists. `execute_phase()` makes two passes, full-halo then by-galaxy, and per-event consumers run nested inside emission. A rate mode is therefore explicit work in each of those places. The builder of the phase plans does not check that its lists cover every entry of a phase, so a mode with no list would be dropped from dispatch silently; the rate work must close that gap.

**A rate module needs its own callback family.** `process(ctx, halos, ngal)` mutates state and is frozen. A rate law must not mutate, so it gets a third family beside `MODULE_CALLBACK_FAMILY_FOF` and `MODULE_CALLBACK_FAMILY_SNAPSHOT`, with a callback whose signature takes read-only state. While a rate callback runs, `module_emit_event()` and `module_create_record()` must refuse. `module_create_record()` already refuses any callback kind but full-halo, so it refuses a rate callback without change. `module_emit_event()` does not consult the callback kind, and when no phase dispatch is active it returns success and drops the event, so it needs an explicit refusal. `const` on the callback's row argument is shallow in C: a galaxy can still be written through the row's pointer. The law is therefore given a scratch copy holding the trial state, and the purity probe, not the signature, detects a law that writes.

**The core cannot address a reservoir by index today.** Properties are generated as struct fields, and no table of galaxy properties is emitted. Reservoirs, their kinds and their conserved totals are therefore declared in property metadata, and the generator emits the table the assembler and the ledger read. Transfers are declared in `module_info.yaml` by reservoir name, so a transfer to an undeclared reservoir fails at startup.

**Rate-mode reservoirs are stored as `double`.** Adaptive error control against single-precision state is not meaningful, and a round trip through `float` each segment would break the ledger at one part in ten million. A reservoir declared `float` in a rate phase is refused. Property types are declared per package and new models default to `double`, so a fork can be `double` while the published `sage16`, which declares 21 `float` properties, stays as it is.

**Records are created only from full-halo callbacks and join the workspace when the callback returns.** The state vector is assembled from the workspace at the start of each segment and never held across a callback.

**Both drivers call one function.** `process_halo_evolution()` (`src/core/halo_evolution.c`) serves the vertical and horizontal drivers, and neither MPI decomposition nor chunking ever splits a FoF group. An integrator that reads no mutable run-level state and nothing driver-dependent (it reads the read-only configuration and the module parameters) inherits every identity gate.

**Determinism has a rule already.** `docs/VISION.md` principle 4 requires identical output whatever driver or traversal order processed the data. For the rate mode that means the order of the state vector, the order in which inflows to one reservoir are summed, and the order in which the error norm is evaluated are all canonical: independent of the run file's order, of registration order and of the driver. The canonical key cannot be a created record's `UniqueGalaxyID`, which differs between drivers. Both drivers build each group's rows through one inheritance function, in FoF-chain then progenitor-chain order, so the order is the same by construction under the horizontal format's chain-order contract. No test compares it across drivers, and a row carries no driver-independent identity that covers created records. The plan therefore uses the workspace row index as the canonical key and adds a cross-driver test of in-group row order.

**The snapshot scope is separate.** `docs/VISION.md` describes snapshot-wide rate callbacks as prospective. The rate mode here is FoF-scoped and is not that. A `post_snapshot` module that writes a reservoir does so outside the ledger, and the documentation says so.

**Output provenance must cover the new settings.** HDF5 output records the enabled modules, the parameters and the event contracts. The integrator, its tolerances and budgets, and the declared transfers are recorded the same way. The reference-identity gate requires a run's attribute names to match the reference commit's exactly, so the new attributes, datasets and rows are written only when a rate phase is configured, and every existing gate sees identical objects. The same gate also compares `metadata/output_schema.json` byte for byte, and that file's `source_md5` covers the property generator's own bytes as well as the property files, so any edit to the generator changes it in every run's output. The reservoir table is emitted by the generator, so the gates' shared comparison treats that hash as build provenance, as a standing rule (the owner's decision 4).

**Infrastructure is tested on fixtures.** The model-neutral fixtures live under `src/module_system/` (`test_fixture`, `test_snapshot_fixture`, the event producers and consumers), are compiled only in test builds, and take their properties from `test_fixture/test_properties.yaml`. `test_snapshot_fixture` is the precedent for adding a mode. The rate fixture follows the same pattern, with `double` reservoir properties.

---

## Relationship to the Vision

| Principle | Effect |
|---|---|
| 1. Physics-agnostic core | **Strengthened.** The integrator, the transfer assembler and the ledger are pure numerics and know nothing about galaxies |
| 2. Runtime modularity | **Refined in one sentence.** See below |
| 3. Metadata as structural truth | **Extended.** Reservoirs, their kinds, their conserved totals and each transfer's ends become declared metadata, and the reservoir table is generated |
| 4. One coherent processing model | **Amended.** The principle names three FoF dispatch modes and one additive snapshot scope, and says a further mode needs its own explicit contract. The rate mode is a fourth dispatch mode inside the same traversal, and its contract is written into the principle |
| 5. Bounded memory | **Preserved, with a bound to meet.** Solver memory is linear in the component's state and its transfers, from one reused, tracked allocation |
| 6. Format-agnostic I/O and provenance | **Extended.** The integrator, its tolerances and the declared transfers join the recorded provenance |
| 7. Validation and fast failure | **Strengthened.** A mixed phase, an undeclared reservoir, a `float` reservoir, an absolute sink, a slaved source whose master is not a transfer out of a reservoir and a negative coefficient fail at startup or at first use, and non-conservation becomes a detected violation |

**The refinement to principle 2.** It states that scientific ordering remains explicit in configuration. Under this work jumps keep explicit configured ordering, because they do not commute, and rate terms within a rate phase become unordered, because ordering them was an artefact of sequential execution. `docs/VISION.md` changes when the implemented behaviour justifies it, so the edits to principles 2, 4 and 6 are part of the implementation plan's last slice and not made ahead of it.

**Modularity is protected.** Several current modules are stages of a splitting recipe (calculate, then apply, then enrich), and under this formulation those stages disappear. That must never mean a model-specific monolith. Independently selectable physical processes stay separate runtime modules, and the core assembles whatever compatible set a run file configures, including the empty set.

---

## Constraints Carried Forward

- **The existing module ABI is frozen.** The rate contract is additive. `process(ctx, halos, ngal)` and `process_snapshot()` are neither changed nor removed, every existing package runs unmodified, and the three other briefs that assume a preserved ABI (the emulator, the model builder and the embedded engine) stay valid.
- **Published models are housed as published.** `sage16` and `sage26` are not converted in place. A published model's numerics are part of what was published, and reproducing it requires reproducing its splitting. The frozen modes are how any externally published model is imported and run faithfully. The rate mode is for forks and for models derived afresh.
- **The event system is reused, not replaced.** Discrete phases keep the existing modes, `module_emit_event()` and the producer and consumer validation.
- **Cross-format identity stays green** for every existing package on both drivers. This work adds a mode and must not perturb any model that does not use it.
- **Bit-identity is claimed for one binary.** Compilers contract multiply-adds differently and the build sets no flag to stop them, so no claim crosses compilers or platforms. The plan does not add one: conservation is asserted to a rounding tolerance, not bitwise.

---

## The Proving Fixture

Two levels: integrator tests on hand-built components with no trees, and fixture modules run through real dispatch on a tree fixture. Each problem has a known solution or invariant.

| Problem | What it demonstrates | Accepted when |
|---|---|---|
| A linear chain, A to B to C | Order of convergence; error tracks tolerance | Measured in the asymptotic window, the fixed-step order is within 0.2 of the scheme's over at least three halvings, and error falls monotonically with tolerance |
| A closed three-cycle | A conserved total to rounding | Drift stays within the ledger's tolerance over a long integration |
| Three sinks drawing on one source | Source limitation; order-independence | The three receive in the ratio of their rates; nothing goes negative when the rate times the interval is large; permuting the modules is bit-identical. Two sinks cannot show a missing canonical order, because two terms commute in IEEE arithmetic |
| An external source, a reservoir and a sink | The ledger | The integrated source and sink match their closed forms, and closure holds within the ledger's tolerance |
| A stiff reversible pair with a slow drain | Step control; the choice of scheme | The explicit scheme is correct with a step count that tracks the positivity prediction; from a start on the slow manifold, the implicit scheme's step count does not depend on the fast rate |
| A bimolecular reaction | Nonlinearity; a second invariant | The closed form is met; the stoichiometric invariant A − B, which is linear, holds within the ledger's tolerance under the explicit scheme and drifts at first order under the implicit scheme |
| A tracer with dilution and a slaved yield | Companions and cross-module dependence | The tracer stays between zero and its parent, the closed-form concentration is met, and an empty donor produces no undefined value |
| A star: satellites feeding one central, at several sizes | Coupled groups; determinism; cost | Isolated groups are bit-identical; results are identical across drivers, task counts and chunk counts; time grows linearly with the number of galaxies |
| A threshold law | Behaviour at a kink | On a family whose crossing falls at a step's midpoint, the fixed-step order is near second; adaptively, the error stays proportional to the tolerance with a bounded number of rejected steps |
| Two intervals with different rates, and a jump that removes a galaxy | Restart, a change of dimension, the jump ledger | The piecewise closed form is met |
| A coefficient that diverges as its donor empties; an undefined or negative coefficient; an exhausted step budget | Loud failure | A non-zero exit that names the law, with nothing committed. A constant drain is not a case: it cannot be declared, because an absolute sink is refused |

Record creation in the middle of an interval, located events, cascades and a classic stiff benchmark can wait for a later increment.

One probe belongs with the fixture because the order-independence gate cannot see it: **a purity check, switched on in the run file,** that evaluates the laws twice in reversed order, requires the coefficients to be equal as bit patterns, and checks that the state the laws were given was not written. Canonical sorting makes the permutation gate pass by construction, so it does not show that a law is pure.

---

## Gate

Stage 1 is accepted when all of these hold.

1. Every fixture problem above meets its criterion.
2. Permuting the rate modules in a run file gives bit-identical output on one binary, and the same holds across the vertical and horizontal drivers, MPI task counts and chunk counts.
3. The ledger closes on every fixture problem, and a planted non-conserving transfer is detected.
4. Every failure case exits non-zero, names its cause and commits nothing.
5. Every existing package and gate is green with the mode unused: `make tests` for each shipped package, `make check-generated` for every package (generator edits reach all of them), `make tests-reference-identity` against a commit that precedes the work, `make tests-distributed`, `make tests-snapshot-global`, `make tests-horizontal-v3`, the converter and bench suites, and the strict baselines of the packages the byte gate does not cover.
6. The cost of the mode on the fixture is measured and reported: time against group size, and peak memory. No production figure is claimed.
7. The documents of record describe the mode: `docs/VISION.md` principles 2, 4 and 6, the Developer Guide's module and property sections, and the skills that own modules, properties, the architecture contract and configuration keys.

---

## Decisions Taken by the Owner (2026-11-12)

1. **The implicit scheme ships in stage 1,** as recommended. A run-file key with one legal value is a mechanism for a need not yet shown, the stiff fixture proves nothing about the interface without a second scheme, and campaign 4b cannot cheaply reopen the core if the fork turns out stiff. The explicit scheme is the default.
2. **The names.** The mode's configuration string is `process_rates`, the callback `<module>_rates`, the run-file section for the integrator and its tolerances `modules.rate_integration`, the property metadata key `reservoir:` and the `module_info.yaml` key `rates:`. They are user-facing and are recorded in output.
3. **Rows with different elapsed times advance on their own clock** in a shared normalised segment time, with each transfer's amount set on the donor's clock ([Phases, segments and time](#phases-segments-and-time)).
4. **The property generator is edited, and the identity gates treat the schema's generator hash as build provenance.** The generator hashes its own bytes into every run's `output_schema.json`, which the reference-identity gate compared byte for byte, so any edit to the generator failed the gate whatever it changed. The fix is a standing rule in the gates' shared comparison, not an allowance for this work: two schemas may differ in that hash alone, and only when each side's value is the digest its own build generated. This was chosen over keeping the new generation out of the generator, as the robust long-term fix.

## Decisions for the Implementation Plan

The plan settles each of these in its Design section and its slices, each with an acceptance criterion:

1. The metadata keys that declare a reservoir, its kind, its parent and its conserved total, and the shape of the generated table.
2. How a module declares a transfer, a source, an observer and a slaved source in `module_info.yaml`, and what the validator checks.
3. The rate callback's signature and its context.
4. The tolerance formula and its defaults, the step and retry budgets, and the diagnostic a failure writes.
5. The canonical ordering key, proved independent of the driver.
6. The mechanics of the rule for rows with different elapsed times in one component (the rule itself is the owner's decision 3).
7. The memory category, owner and free point of the solver's workspace.
8. The linear solve for the implicit scheme, which must exploit the satellite-to-central structure and never form a dense matrix.
9. How the new output attributes and the generator edit pass the reference-identity gate.

---

## The Campaigns That Follow

These are not part of the implementation plan. They are recorded so that the plan builds what they will need and nothing more.

- **Stage 4a measures the published models as shipped:** the noise floor, time-step refinement and the legal module orderings, for `sage16` and `sage26`. It needs no new core. This is what the earlier brief called First Work, with two changes: the throwaway monolithic integrator is no longer needed as a reference, because stage 4b compares against the rate mode itself, and the sampled Jacobian spectra are replaced by the stiffness telemetry the core records.
- **Stage 4b forks `sage16` as `sage16rate`:** the classified prescriptions rewritten to satisfy the rate-law contract, convergence and order-independence shown on the fork, and its parameters sought so that it reproduces the published population results, if that is possible. Galaxy-by-galaxy parity is not a goal. The earlier brief said the new package would not reproduce `sage16` and should not; the target is now the opposite, with a refusal as a legitimate result.
- **The forks are not new models for publication.** They exist to develop the tools and the workflow, and for the owner's scientific curiosity.

What the classifications say the forks will meet, beyond what stage 1 supports: observer state that depends on the substep count, a redshift held from a row's previous snapshot, rates whose time constant is the snapshot interval, stored relations re-clamped after each debit, a ratcheting heating radius with an order dependence inside one module, and a jump fired by a label inside a budget module. Each is a question for the campaign's build, and some may ask the core for a further state kind.

The classification documents were written to open this work and are not exhaustive on one point: the caps and clamps they list are the load-bearing ones, not every one. `sage_apply_infall.c` alone holds several, and the fork's build must find them all.

---

## Relationship to Other Plans

- **[`AGENTIC-SCIENCE-ARCHITECTURE.md`](AGENTIC-SCIENCE-ARCHITECTURE.md)** sets the order: this brief's part is its stage 1, and the campaigns are its stage 4.
- **[`OPTIMISATION-SPECTRUM.md`](OPTIMISATION-SPECTRUM.md).** Its Tier A work has landed. Two of its options stay deferred behind this work: the structure-of-arrays generator (item 19) would transpose the transport-scratch properties a rate-mode model no longer needs, and batched dispatch (item 17) would be a further extension of the mode machinery. The init-time dispatch plans Tier A added are the structure the rate pass extends.
- **[`MIMIC-EMULATOR-PLAN.md`](MIMIC-EMULATOR-PLAN.md) and the bench.** The bench (`bench/mimic-bench/`) is the instrument campaign 4b uses to compare the fork with its reference. It rules parameter regions out and does not locate a best point, so the campaign needs a calibration method beside it. A declared transfer is also a better design vector than an imperative `process()` body.
- **[`MIMIC-EMBEDDED-ENGINE-PLAN.md`](MIMIC-EMBEDDED-ENGINE-PLAN.md).** Sequence it after this. A side-effect-free rate law is re-entrant by construction, where the imperative ABI is blocked by `init(void)` and `cleanup(void)` reading globals.
- **[`MIMIC-MODEL-BUILDER-PLAN.md`](MIMIC-MODEL-BUILDER-PLAN.md).** A declared transfer (a donor, a recipient, a rate law, units) maps onto "extract equations, parameters and units from a paper" far more directly than an imperative body does, and is more machine-checkable.
- **Delivered work this builds on.** The snapshot-global modules added the callback-family and mode-descriptor machinery this mode extends. Distributed operation and chunking shard by whole forest and never split a FoF group, so no component is cut. The dual-driver work gave both drivers one evolution entry point.

---

## Costs and Risks

- **Run time rises, and by how much is not measured.** The module pipeline is about 83% of a default `sage16` run on mini-Millennium. The four-stage scheme costs four evaluations of the rate laws per step, plus one per segment to choose the first step, so with ten segments kept, each costs at least five evaluations where the split pipeline makes one pass, before any step is rejected. If an evaluation costs what a module pass costs today, that is about four times the whole run at the least, and more wherever a segment needs several steps: a transient reservoir costs about three steps per e-fold at a relative tolerance of 1e-3 and about six at 1e-4, while a quasi-steady one costs a step count set by its fastest rate. None of this is measured. Against operator splitting of equal accuracy, which the 66% defect puts at hundreds of substeps, the coupled scheme should be several times cheaper. Stage 1 measures the fixture only.
- **A group steps at the pace of its stiffest member.** With the whole FoF group as one component, a cluster's thousands of satellites share one step sequence. Schemes that step members at different rates exist and are a later optimisation, not part of stage 1.
- **Work and memory must be linear in the component.** A rich group has thousands of galaxies and tens of reservoirs each. A dense matrix at that size is tens of gigabytes and its factorisation is cubic, so no dense matrix is ever formed and no Jacobian is built by differences per step.
- **The persistent memory term is precision, not solver stages.** The solver's workspace is a few copies of one group's state from a reused allocation. Storing a fork's reservoirs as `double` widens every galaxy in the pool, which matters at the scale of the largest measured run: the cut Shin-Uchuu's largest forest peaks at 70.96 GB for `sage16`, of which the galaxy pool is 22.9 GB.
- **The conversion is a re-derivation, not a port.** Rewriting hard caps as laws that satisfy the contract is the largest single item and is physics work. It belongs to campaign 4b.
- **Jump cadence caps the accuracy.** With jumps at segment boundaries, the hybrid solution keeps a first-order error in the segment count. Located events remove it and are a later increment.
- **A fork's parameters will move.** That movement is a result of campaign 4b, not a defect.

---

## Language and Backend

**Implement in C, in the existing framework.** The formulation is language-independent, and its goal, mathematical consistency, is not what automatic differentiation provides.

A JAX or other differentiable backend was assessed and rejected for this purpose. Reverse-mode differentiation retains evolution history and needs checkpointing or adjoint methods to respect bounded memory. Ragged merger trees whose topology changes suit array compilers poorly. The dependency floor rises from a C compiler to a pinned Python stack. The MPI model does not carry over. And maintaining physics in two languages needs a permanent parity harness between them. The side-effect-free contract this work introduces is the prerequisite for any future differentiable backend, so the choice forecloses nothing.

---

## Prior Art and What Was Not Adopted

The target formulation coincides with two established formalisms, and the implementation plan should cite their results where they apply.

- **Compartmental and production–destruction systems.** Declared conservative transfers between reservoirs are a compartmental system. Two standard results are used above: a Runge–Kutta scheme written in increment form preserves linear invariants to rounding when each transfer's amount is applied to both ends, and the non-negative orthant is invariant when each reservoir's outflow is bounded by a multiple of its content. The specific-rate form is also the structure the positivity-preserving schemes of that literature are built on.
- **Hybrid dynamical systems.** Continuous flow with guard crossings and resets is a hybrid automaton. That field names the failure the earlier brief did not: an unbounded cascade of jumps in finite time. Stage 1 avoids it by construction, with one pass of the discrete phases per boundary, and a termination rule is owed when events are located.

Two ideas were assessed and set aside, and the reasons stand:

- **A forward-sensitivity service in the core.** Integrating parameter sensitivities beside the state would give the local response of outputs to parameters. A core service needs a demonstrated need, and controlled ablation (disable a term, rerun, difference) needs no service.
- **A runtime-control service in the core** that adjusts parameters during a run or steers output toward a target. Rejected for the core outright: it puts scientific policy inside physics-agnostic infrastructure. A module may implement whatever self-regulating prescription its physics calls for; the framework does not own that policy.

---

## How This Revision Was Produced

The brief of 2026-08-18 was checked on 2026-11-12 by two independent reviews: one of every statement against the code and the other documents, and one of the numerical design. Their main citations were checked against the files. The stiffness estimate, the run-time estimate and the claim about rich groups are reviewer estimates and are marked as unmeasured where they appear. Nothing here was tested by building or running Mimic.

It was revised again on 2026-11-12, alongside the derivation of the implementation plan, from eight read-only investigations of the code at v1.6 and a numerical design memo whose formulas and fixture criteria were checked in a reference implementation outside the repository. The owner's decisions were recorded, the numerical statements the memo found wrong or loose were corrected, and the facts about the core were rechecked against the files. Nothing was tested by building or running Mimic.
