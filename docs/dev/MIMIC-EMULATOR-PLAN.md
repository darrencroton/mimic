# Mimic Emulator Plan

**Status:** Requirements brief. Not scheduled. Records a direction and two decisions so they are not re-litigated; the design is deliberately left open until the work is due.

**Date:** 2026-08-20

---

## Goal

Give Mimic an emulator-based instrument that answers **"is this model package scientifically well-posed, and which data constrains which physics?"** — by building a cheap statistical approximation of a configured Mimic pipeline and using it to rule out regions of parameter space rather than to fit a point.

The deliverable is a diagnosis, not a calibration. The method is Bayes linear emulation with history matching, as implemented in [PRISM](https://github.com/1313e/PRISM) (van der Velden et al. 2019, ApJS 242, 22; [arXiv:1901.08725](https://arxiv.org/abs/1901.08725)).

---

## Motivation

Mimic can already prove a model package is **correctly built**: `check-generated`, `validate-modules`, `lint-parameters`, the test tiers, and the cross-format identity gate all pass or fail mechanically. Nothing in that stack can say a package is **scientifically well-posed** — that its parameters are identifiable, that its degrees of freedom are supported by the data, or that the observables chosen to constrain it actually do.

That gap is the reason [`MIMIC-MODEL-BUILDER-PLAN.md`](MIMIC-MODEL-BUILDER-PLAN.md) still lists *"a science-gate prototype exists and has been validated on at least one trusted model or module"* as an unmet precondition, and why its Science Gates section says the layer *"does not yet exist as a complete mechanism"*.

**The evidence that this method closes that gap is direct, and it is on a sibling model.** [arXiv:2011.14530](https://arxiv.org/abs/2011.14530) (van der Velden, Duffy, Croton & Mutch 2021, ApJS 253, 50) applied PRISM to Meraxes. Almost none of its findings are calibration results; they are structural diagnoses that no optimiser or MCMC run would have produced:

- M16 Meraxes *"has too many free parameters"* — the supernova feedback parameters could not be constrained at all.
- The published hand-calibrated values *"are likely to be biased as the parameters do not converge, which is probably caused by the large correlations between the free parameters."*
- The Q19 variant's **reduced** degrees of freedom *"greatly improves the convergence rate"* — the method validated a model *simplification*.
- *"The SMF and the LF/CMR data constrain the Meraxes model differently."* Q19 had constrained on LF/CMR alone, so *"the differing, stronger constraining power of the SMF data was not noted."*
- Best-fit values sitting at prior boundaries *"can imply that aspects of the Meraxes model itself are incomplete or incorrect."*

Cost: roughly 7,000 Meraxes evaluations, *"multiple orders of magnitude less than the average amount used by an MCMC approach."* One emulator reduced plausible parameter space by a factor of ~4,000 from 3,056 evaluations. Nine parameters were varied; the rest were held at defaults.

The paper's own conclusion is that such a framework belongs *"as a core component in the development of scientific models"*, and that the diagnosis is *"a task that a full Bayesian analysis cannot perform quickly."*

---

## Why This Matters Specifically To Mimic

Three of these consequences are sharper here than they were for Meraxes, because they follow from Mimic's architecture rather than from any one model.

- **Structural comparison needs a currency, and best-fit χ² is not one.** `docs/VISION.md` §2 exists so that physics combinations can be swapped at runtime. But comparing pipeline A against pipeline B is only science if both are on equal footing, and a goodness-of-fit score rewards whichever has more freedom. **Non-implausible volume fraction is a better currency than a fit score** — it penalises a model for needing a fine-tuned corner of its parameter space to work. It is a conditional comparison and not a ranking: the fraction is measured against declared prior ranges, so it compares two structures only when their prior measures are explicitly comparable, and a wholly unconstrained parameter barely moves it. Making that condition precise is Open Question 8.
- **Over-parameterisation is a standing risk of a framework that makes adding modules cheap.** Every module brings parameters; nothing currently checks whether the constraint set can support them. The M16 result is the characteristic failure mode of exactly this design, and it compounds as `models/` grows.
- **Observable selection becomes a decision, not an accident.** As constraints accumulate, knowing which are load-bearing and which are redundant is durable knowledge that outlives any single model package.

---

## What The Method Is

Sample the parameter space, evaluate the model at those points, fit a cheap emulator with an honest variance, and cut away everything the emulator can rule out. Repeat on what survives.

```text
I²(x) = (E_adj[f(x)] − z)² / (Var_adj[f(x)] + Var_md + Var_obs)
```

Implausibility per output; cut at I > 3; a parameter set is rejected on its nth-largest implausibility rather than its worst, so one bad output cannot veto alone. `Var_md` is the **model discrepancy** — a declared statement of how wrong the model is allowed to be — and it is the term that makes the cut principled rather than hand-tuned.

Two properties matter more than the speed:

- **No likelihood is required.** SAM residuals against an observed mass function are dominated by systematics and model discrepancy, not by Poisson noise. MCMC forces the invention of a likelihood and then trusts it; history matching asks only for a discrepancy budget, which is a more honest object and an auditable one.
- **Multiple constraints combine without a weighted-sum objective.** Each observable carries its own implausibility instead of a term in a summed score, so the *"how do I weight the SMF against the BHBM"* problem — which [`MIMIC-MODEL-BUILDER-PLAN.md`](MIMIC-MODEL-BUILDER-PLAN.md) records under *Calibration trade-offs* — does not arise in that form. It does not vanish: a per-observable discrepancy budget still sets how restrictive each constraint is, and the nth-maximum rule is itself a declared choice. **The weighting reappears as the budget**, which is why the budget is the crux below rather than a detail.

An empty non-implausible region is a **result**, not a failure: it says no parameter choice reconciles this model structure with this data.

---

## Two Decisions, Recorded So They Are Not Re-Litigated

### 1. Port the method, not PRISM

**PRISM itself is not adopted as a dependency, and not ported wholesale.** Assessed 2026-08-20 against the upstream repository at `1313e/PRISM`:

- The package is **24,000 lines**, of which **8,100 are a PyQt5 GUI** and **4,900 are tests for that architecture**. The irreducible method — the Bayes linear adjustment, the implausibility cut, the active-parameter regression — is a few hundred lines built on `PolynomialFeatures`, `LinearRegression` and a feature selector, all of which are still maintained upstream.
- It is unmaintained: last release v1.3.2 (2021-02-01), last commit 2021-06.
- The final release **cannot be installed** on a current stack (`pyqt5==5.12.*` has no distribution for modern Python); `pip install prism` silently backtracks to **1.1.3 (2019-07)**, which then fails to import against present-day `e13tools`. Reviving it means pinning a 2020-era Python, numpy, h5py and PyQt5 alongside four unmaintained helper packages.

Adopting 24,000 lines of unmaintained foreign code, with its own HDF5 state format and MPI layer, to obtain ~500 lines of algorithm is a poor trade anywhere; it is a worse one in a repository whose discipline is generated metadata, validated contracts and provenance. **What PRISM supplies is its design decisions** — the implausibility formulation and its multi-output cut, an emulator cheap enough to refit every iteration, explicit model discrepancy as a first-class user-supplied term, iterative refocusing, and projection figures as the primary output. Those are the deliverable of the reading, and they are recorded here.

### 2. This is not a calibrator, and does not compete with one

[SAGEswarm](https://github.com/sage-home/SAGEswarm) is live, in-house, SAGE-family calibration software: particle swarm optimisation over nine constraint types, multi-simulation support including miniMillennium and miniUchuu, SLURM integration, actively developed. **It should be the calibrator.** This brief must not grow into a second one.

The two answer different questions, and the distinction is structural rather than a matter of quality:

| Instrument | Question answered | Output |
|---|---|---|
| PSO / SAGEswarm | *What are the best-fit parameters for this fixed model?* | A point, and a fit |
| History matching | *Is this model structure well-posed, and which data constrains which physics?* | A surviving region, or none |

An optimiser returns its best point unconditionally, including when that point is meaningless — so none of the five Meraxes findings above are reachable from one. Note also that PSO particle spread is not a statistical uncertainty: in SAGEswarm's `src/pso_uncertainty.py` the reported errors are the standard deviation and 16/84 percentiles **of the particle positions**, with the fitness scores not entering the statistic, so they contract as the swarm converges. That is a description of optimiser dynamics, and it is fine for what it is; it is not a statement about what the data constrains.

**The intended relationship is sequential:** history matching to establish that a model is identifiable and to bound the region worth searching, then SAGEswarm or an MCMC inside that region for the point calibration and its uncertainties. Reuse before rebuild applies to its constraint layer — the observables and their error budgets are the expensive, scientific, slow-to-validate part, and nine of them already exist there against simulations Mimic also ships.

---

## Relationship to the Vision

Governed by `docs/VISION.md`. This work is a **consumer** of Mimic's contracts, not a modifier of them — it adds no core code, no processing mode, and no module ABI surface.

| Principle | Effect |
|---|---|
| 1. Physics-agnostic core | **Untouched.** The instrument sits outside the executable and drives it through its existing run-file interface |
| 2. Runtime modularity | **Served, not changed.** Nothing in the core moves; the instrument supplies from outside the evidence that makes a runtime pipeline comparison a scientific comparison rather than a software one |
| 3. Metadata as structural truth | **Extended, outside the core.** Constraints, their errors, and the model-discrepancy budget become declared metadata rather than values embedded in figure code |
| 4. One coherent processing model | **Untouched, and depended upon.** Its determinism requirement — stochastic modules seed from stable per-halo or per-FoF keys, never a traversal-order RNG stream — is a precondition here: an emulator means nothing if one parameter set can produce two outputs |
| 5. Bounded memory | **Untouched.** Each evaluation is an ordinary run in its own process |
| 6. Format-agnostic I/O and provenance | **Consumed, not changed.** Reads HDF5 output and `metadata/output_schema.json` through the existing readers. Mimic supplies per-run provenance; campaign-level provenance — the sampling design, the constraint and discrepancy versions, the emulator and its train/holdout split — is not in the run files and is Open Question 6 |
| 7. Validation and fast failure | **Extended into science.** Adds a check for whether a package is *well-posed*, beside the existing checks for whether it is correctly built |

---

## Relationship to Other Plans

- **[`MIMIC-MODEL-BUILDER-PLAN.md`](MIMIC-MODEL-BUILDER-PLAN.md) — the primary consumer; this is a candidate for its missing science gate.** That brief's unmet precondition is a validated science-gate prototype, and its Science Gate Open Problems name *tolerance selection*, *novel model validation* and *calibration trade-offs*. History matching speaks to all three: the implausibility cut is a principled tolerance once a discrepancy budget is declared; a non-implausible region is an acceptance criterion that needs no trusted baseline, which is what a *novel* model requires; and multiple constraints combine without a weighted-sum objective, though the budget in the first clause is where their relative weight actually lives. It does **not** solve that brief's *injected-error validation* problem — the emulator gate needs its own mutation evidence, exactly as every other gate here does.
- **[`MIMIC-COUPLED-RATE-FORMULATION-PLAN.md`](MIMIC-COUPLED-RATE-FORMULATION-PLAN.md) — the second consumer, and the earlier one.** That brief's deliverable is *"a new model package, a recalibration, and a quantitative comparison against the control"*; its Open Question 7 is *"recalibration methodology, and which observables define acceptance"*; its Gate item 5 requires the new package's differences from `sage16` to be *"quantified and attributed"*. A point calibration cannot compare two structures on equal footing, because each package is judged at one hand-tuned operating point. This instrument supplies the identifiability diagnosis and, subject to Open Question 8, the structural comparison; the recalibration itself remains an optimiser's job, per Decision 2. It does **not** by itself discharge Gate item 5, whose *attribution* half — which physics change produced which output difference — is a direct control-versus-package comparison that no emulator performs. The contribution is still enough to put a plausible call on this brief **before** the model builder.
- **[`MIMIC-EMBEDDED-ENGINE-PLAN.md`](MIMIC-EMBEDDED-ENGINE-PLAN.md) — explicitly not required.** A physics-only in-process API would be the natural way to drive thousands of evaluations, and it is **not needed here**: a full `sage16` run on mini-Millennium costs ~3 s wall clock (measured 2026-08-19 on macOS/arm64, eight output snapshots, HDF5), against campaign sizes of order 10²–10³ evaluations that parallelise across cores with no shared state. Process spawn is noise at that ratio. This brief must not be used to argue for scheduling that one.
- **[`OPTIMISATION-SPECTRUM.md`](OPTIMISATION-SPECTRUM.md) — upstream (pathway step 4), and it owes this brief a guarantee rather than a speedup.** Campaign cost is **not** the connection: as the entry above records, campaigns parallelise across cores with no shared state, so a cheaper single run buys little here and thread-per-forest (its item 16) buys a core-saturated campaign nothing at all. The real dependency is determinism. That item introduces threads into the run, and Principle 4's guarantee — one parameter set, one output — is a precondition of this instrument, not a nicety. Bit-identical output under threading is therefore an acceptance criterion of that work, and if it is not met, this brief is blocked rather than merely slower.
- **[`MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md`](MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md) — orthogonal, and a future client.** Snapshot-global modules add parameters like any others; they raise no new question for this instrument beyond evaluation cost.
- **Shin-Uchuu pathway — unrelated, and deliberately so.** Campaigns run on the smallest box that resolves the constraints; production volumes are for validation, never for calibration. This brief does not compete for the snapshot pathway's seams and does not belong on its critical path.

---

## What Mimic Already Provides

The reusable surface is larger than it looks, which is part of why the effort is bounded:

- **Parameters are already declared and already external.** The `modules.parameters:` block of a run file is the design vector; `models/sage16/input/sage16_mini-millennium.yaml` exposes 15, of which 14 are continuous and `AGNrecipe` is a discrete structural switch. Sweeping means writing YAML, not patching code.
- **Summary statistics are already factored out of plotting.** `plot/mimic-plot/output_utils.py:259` provides `calculate_mass_function()` with standardised binning and normalisation, and `hdf5_reader.py` plus `output_schema.py` give programmatic access to galaxy output.
- **Observational constraints already exist inline, in two forms that are not interchangeable.** Some figure modules embed digitised point tables with errors: `models/sage16/plots/figures/stellar_mass_function.py:210` computes upper and lower bounds separately, which is the shape a discrepancy-aware implausibility wants. Others embed analytic fitted relations carrying no error at all — `baryonic_mass_function.py:90` (Bell 2003), `black_hole_bulge_relation.py:103` (Häring & Rix 2004), `metallicity.py:149` (Tremonti 2003). **Only the first kind can supply an observational variance; the second needs one invented for it.** Both need lifting into a declared constraint set, and sorting them into those two classes is the first task, not a detail of it.

---

## Costs and Risks

- **The model-discrepancy budget is the crux, and it is a research problem rather than an engineering one.** The Meraxes analysis used a flat `Var(md) = 10⁻⁴`, defensible there partly because mock data was involved. Establishing a defensible budget for SAGE-family observables at z = 0 is genuine scientific work. **If it is fudged, the implausibility cuts are as arbitrary as a hand-tuned tolerance and the whole instrument is theatre.** This is the single risk that engineering quality cannot mitigate.
- **The value is conditional on Mimic hosting several genuinely distinct model structures.** If `models/` converges on one production pipeline, a single calibration campaign would serve and this is over-provisioned. That condition is a bet on Mimic's own stated direction; it should be a conscious one, and it is worth re-testing when the work comes due.
- **This is an instrument, not infrastructure.** It runs when a model package is created or restructured — not on every run, not in CI. Low duty cycle argues for keeping it small and standalone, and against it ever acquiring a framework, a GUI, or a state format. PRISM's own history is the cautionary case.
- **A wrong emulator is worse than no emulator**, because it rules out regions with false confidence. Emulator variance must be validated against held-out evaluations before any cut is trusted — see First Work below.
- **Constraint provenance must not fork.** If constraints are lifted from the figure modules, the figures and the campaign must not drift into disagreeing about what Baldry et al. says. One source, consumed by both.

---

## First Work: Measure Before Building

**Nothing should be built until the load-bearing assumption is measured: that a cheap emulator can predict Mimic's response well enough, and knows honestly when it cannot.**

A zero-repository-change spike, outside the product tree:

1. Latin-hypercube sample ~5 `sage16` parameters spanning star formation and supernova feedback, holding the rest at defaults, on mini-Millennium with a reduced snapshot list and a subset of tree files.
2. Evaluate a few hundred points — hours on one machine, embarrassingly parallel across cores.
3. Fit the emulator on part of the design and test it against the rest. The pass criterion is **calibrated variance, not accuracy**: held-out residuals must fall within the emulator's own claimed uncertainty at the expected rate. An emulator that is inaccurate but honest is usable; one that is accurate on average but overconfident is not.
4. Report the fraction of parameter space a single iteration removes at a defensible discrepancy budget, and the sensitivity of that fraction to the budget.

If held-out variance is not calibrated, or if one iteration removes almost nothing at any defensible budget, the method does not transfer to this model and this brief should be closed rather than promoted. It can run alongside any other work.

---

## Open Questions

To be settled before or during an implementation plan, not by it:

1. **The model-discrepancy budget.** Flat per observable, mass-dependent, or derived from the spread across published determinations of the same relation. The most important open question here.
2. **The constraint set, and its ownership.** Which observables define acceptance; where they live so that figures and campaigns cannot disagree; and how much of SAGEswarm's existing set can be shared rather than duplicated.
3. **Emulator family.** Polynomial regression with active-parameter selection (PRISM's choice, cheap and interpretable) versus a direct Gaussian process. Decide from the spike, not in advance.
4. **Output transformation.** The Meraxes work mapped log number densities through `arctan` to bound the dynamic range; whether that is needed here is measurable.
5. **Structural variation.** `AGNrecipe` and module-pipeline choices are discrete and must not enter the design vector. Whether to emulate each structure separately and compare surviving volumes, or to treat structure selection as outside the instrument entirely.
6. **Where campaign state lives.** `MIMIC-MODEL-BUILDER-PLAN.md` requires that orchestration artifacts not pollute the product repository; the same constraint applies here and the same answer should serve both.
7. **Whether the gate is ever mechanical.** Auto-clearing a package as well-posed requires injected-error validation of the gate itself, on the model builder's own terms. Until that exists, the output is review-class evidence.
8. **What makes two structures comparable.** A surviving-volume comparison needs an explicit statement of the prior measure each structure is judged against, and a rule for structures that do not share a parameter set. Without one the metric is a within-structure diagnostic only.

---

## Gate (when activated)

1. The spike in [First Work](#first-work-measure-before-building) is complete and published, and its calibrated-variance criterion is met.
2. A declared constraint set with an explicit, defended model-discrepancy budget exists, shared with the plotting figures rather than forked from them.
3. The instrument recovers a **known** degeneracy: a `sage16` variant carrying a deliberately injected redundant parameter is reported as unidentifiable. The shipped package is then diagnosed and the result reported **whatever it turns out to be** — if `sage16` proves over-parameterised that is a finding, exactly as it was for M16 Meraxes, not a failure of the instrument.
4. A structure-versus-structure comparison is demonstrated on two real pipelines, reported as surviving parameter-space fraction against stated and comparable prior ranges.
5. Mimic itself is unchanged — no core edit, no new processing mode, no module ABI surface — and all existing gates stay green.
