# Mimic Model Builder Plan

**Status:** Long-term planning brief, not an implementation plan.
**Date:** 2026-06-05
**Context:** This work is downstream of Mimic v1.0 and a working science-gate prototype. The current brief is conservative about waiting for the dual-driver migration; after v1.0 is tagged, re-review whether the model builder should wait for the snapshot driver or proceed from the stable v1.0 module, metadata, and validation seams. **That re-review is now due (2026-08-12):** both triggers have fired — v1.0 is tagged and the snapshot driver landed with its identity gate green — so the brief should be refreshed against the tagged baseline rather than left waiting.

---

## Purpose

This document preserves the essential requirements for a future system that helps build new Mimic model packages from scientific papers. It intentionally avoids tactical tooling choices. Those choices will be made when the work starts, based on the then-current model ecosystem, orchestration tools, hardware, cost constraints, and team workflow.

The model builder should not drive current Mimic architecture except where it reinforces already-valid constraints: stable module interfaces, generated metadata as structural truth, deterministic stochastic physics, strong validation gates, and traceable scientific evidence.

---

## Status And Preconditions

The model builder is not ready to implement. It becomes actionable only after:

- Mimic v1.0 is tagged and the v1.0 baseline is refreshed.
- ~~The dual-driver architecture is implemented and the snapshot driver passes cross-format identity.~~ **Met 2026-08-12.**
- A science-gate prototype exists and has been validated on at least one trusted model or module.
- The team has reviewed which orchestration and model-serving tools are appropriate at that time.

Until those conditions hold, this document is a requirements brief and design checklist.

Before promoting this brief to an implementation RFC, refresh it against the tagged v1.0 baseline and any pre-v1.0 chunked-output changes. Do not revise tactical tooling or workflow details now based on work that has not landed.

---

## Essential Idea

The builder should turn a paper plus a goal statement into a traceable, tested Mimic model package through a gate-driven workflow:

1. Extract equations, parameters, units, calibration targets, validation figures, assumptions, and ambiguities from the paper.
2. Produce a model specification and implementation plan for human approval before writing production code.
3. Implement one physical process at a time in isolated workspaces.
4. Run code gates, generated-code gates, metadata validation, tests, and science gates repeatedly until failures are resolved or a real scientific judgement call is reached.
5. Produce a final scientific quality report linking every implemented claim to paper evidence and validation results.

The core principle is that "done" must be defined by evidence and gates, not by an agent or developer claiming the implementation looks plausible.

---

## Non-Negotiable Requirements

### Traceability

Every formula, parameter, unit conversion, redshift dependence, calibration target, and implementation decision must be traceable to a paper location, reference implementation, or explicit user decision. Ambiguities must be preserved rather than silently resolved.

### Stable Mimic Contracts

The builder must work with Mimic's existing model/package boundary, module metadata, generated property system, and module ABI. It must not require changing ordinary module call signatures or hand-editing generated code.

### Isolation

Campaign state, ledgers, intermediate drafts, worker logs, and failed attempts should not pollute the Mimic product repository. The future implementation should use a separate orchestration environment or an equivalently isolated workspace model. Mimic should receive a clean branch or patch series with source, metadata, tests, plots, and reports.

### Human Approval Before Production Code

The first hard gate is a human-approved model specification. The builder may parse, search, draft plans, and identify ambiguities before approval, but it should not make production Mimic changes until the plan is accepted.

### Deterministic Stochastic Physics

Any stochastic module must use stable per-halo or per-FoF seeds. Traversal-order RNG streams are incompatible with dual-driver cross-format identity and must not be introduced.

### Honest Calibration

Calibration is a scientific workflow, not a purely mechanical pass/fail test. The builder may search within paper-stated priors and report best-effort fits, but target weighting and unresolved trade-offs are scientific decisions that must be escalated.

### Gate Trustworthiness

A science gate may auto-clear a relation only if the chosen tolerance has been validated to catch realistic injected errors for that relation. Otherwise the relation remains review-class evidence, not a mechanical pass.

### Protected Institutional Memory

If the builder compounds reusable model knowledge across builds, promotion into that shared store must be gated harder than ordinary task advancement. A subtly wrong model that passes the quantitative gates could otherwise be promoted and poison every future build. Even in an otherwise autonomous run, promoting a recipe into shared memory should require an explicit human sign-off.

---

## Gate Stack

### Code Gates

The builder should drive Mimic's existing hard checks:

```bash
make MODEL=<name> SIMULATION=<name>
make MODEL=<name> SIMULATION=<name> check-generated
make MODEL=<name> SIMULATION=<name> validate-modules
make MODEL=<name> SIMULATION=<name> tests-unit
make MODEL=<name> SIMULATION=<name> tests-integration
make MODEL=<name> SIMULATION=<name> tests-scientific
```

Long-running output must be captured under `archive/test-logs/`, exit codes must be checked explicitly, and failing tests must be treated as real problems.

### Science Gates

The builder needs a science-gate layer that does not yet exist as a complete mechanism. It should include:

- Physical invariants such as non-negativity, mass/metal/baryon conservation where applicable, physical ranges, and unit consistency.
- Figure or relation parity against digitised paper targets where tolerances can be defended.
- Regression checks against trusted baseline output where physics is shared.
- A structured report of deviations, uncertainties, and failed or review-class relations.

Unit consistency deserves first-class weight: a silent unit mismatch has already produced a severe defect in Mimic (the quasar-wind unit error in `sage_agn_physics.h`), so unit-explicit assertions are part of the invariant set, not an optional extra.

Reference parity to a trusted Mimic baseline validates regression and shared behaviour. It does not prove correctness for a novel model whose intended physics differs from the trusted baseline.

---

## Workflow Shape

The future implementation should preserve this staged shape:

| Stage | Purpose | Gate |
|---|---|---|
| Intake | Capture paper, goal, scope, target model package, target simulation, available references, budget, and constraints | Inputs complete |
| Evidence | Extract equations, units, parameters, assumptions, calibration targets, validation figures, and ambiguities | Evidence ledger reviewed |
| Specification | Map paper processes to Mimic modules, properties, parameters, phases, tests, plots, and validation targets | Human approval before production code |
| Scaffold | Create package/module structure, metadata, property definitions, and basic tests | Compile, generation, validation, unit tests |
| Implement | Implement one process at a time in isolation | Per-process code gates and relevant science gates |
| Couple and calibrate | Assemble the model and evaluate coupled relations | Full code gates, science gates, and explicit calibration report |
| Audit and report | Perform independent review and produce a scientific quality report | Remaining risks and decisions documented |

The staged shape is more important than any particular future orchestration tool.

---

## Science Gate Open Problems

These must be solved before the builder can safely auto-clear scientific acceptance:

- **Tolerance selection:** Published relations can differ because of sample variance, assumptions, binning, IMF choices, and digitisation error. A loose tolerance may pass subtly wrong physics.
- **Injected-error validation:** Each auto-cleared relation needs a demonstration that a realistic injected bug or parameter perturbation fails the gate.
- **Novel model validation:** A novel model will not be byte-identical to a trusted baseline. Its acceptance must rest on physical invariants, paper-figure parity where defensible, shared-physics non-regression, and explicit scientific review.
- **Calibration trade-offs:** Multi-relation weighting is not automatically determined by code. If the paper does not define the weighting, the builder must ask.
- **Data availability:** Missing figures, unavailable observational data, ambiguous paper text, or unavailable reference implementations are explicit blockers or review items, not reasons to invent hidden assumptions.

---

## Future Design Choices To Make

When the work becomes active, the team will need to choose:

- The orchestration environment and persistence model.
- The isolation model for Mimic workspaces and integration branches.
- The model/tooling stack for paper extraction, coding, review, plotting, and reporting.
- The reporting and steering channels for long-running campaigns.
- The schema for evidence ledgers, task ledgers, acceptance ledgers, and final reports.
- The storage and promotion policy for reusable model knowledge.
- The cost, privacy, and review policies for any external services.

These are important choices, but they are intentionally not specified here.

---

## Definition Of Fit For Purpose

The model-builder plan is ready to become an implementation RFC only when it can answer:

- What exact science gates exist and which relations can they auto-clear?
- Which relations are review-class because tolerances cannot safely separate correct from subtly wrong?
- How does the builder preserve paper-to-code traceability?
- How does it prevent generated-code drift?
- How does it avoid contaminating the Mimic product repo with orchestration artifacts?
- How does it escalate calibration ambiguity and scientific trade-offs?
- How does it preserve dual-driver determinism for stochastic physics?

Until those answers are grounded in working code, the correct target is a traceable best-effort implementation and calibration report, not autonomous scientific certainty.
