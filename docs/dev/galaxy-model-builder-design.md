# A Self-Driving System for Building Galaxy Formation Models in Mimic — Design Document

**Author:** Claude Opus 4.8
**Date:** 2026-05-30
**Status:** **Aspirational design proposal — NOT yet the source of truth.** This document describes a longer-term vision that intentionally runs ahead of the codebase. It will be reviewed and revised against the actual state of Mimic *after* v1.0 is tagged and the dual-driver work (`MIMIC-DUAL-DRIVER-ARCHITECTURE.md` /`-CHANGE-MAP.md`) is implemented and working correctly. Until that revision, treat specific commands, phase durations, and "exists today" claims as **intent to validate**, not verified fact. §0 records what is locked, what is open, and what the future updater must decide.
**Context:** Read `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` first. This proposal is intentionally downstream of the v1.0 baseline and the completed dual-driver migration.

---

## 0. Status, scope, and guidance for the future updater

This section is written *to the person (or agent) who finalises this design*. It exists because the rest of the document is deliberately ambitious and must be pinned to reality before any of it is built.

### 0.1 What is confirmed against the codebase (safe to rely on)

- **The code gates are real and strong.** `make MODEL=<name> check-generated`, `validate-modules`, and the three test tiers (`test-unit`/`test-integration`/`test-scientific`) all exist as Makefile targets. The YAML→C generation contract is genuine and uncheatable. The 22-plot system exists (18 snapshot, 4 evolution).
- **The baseline mechanism is real and will become the v1.0 trusted reference.** `tests/data/output/baseline/` (binary + HDF5) is the existing Mimic regression-baseline mechanism. The intended path is to finish v1.0 optimisation work, tag v1.0, then refresh or extend the SAGE baseline so it supports byte-identical regression coverage for the relevant core and baryonic output. That v1.0 baseline is the reference-parity foundation this document depends on. Do not read the current pre-v1 baseline as the final science-gate reference without that refresh.

### 0.2 What does NOT yet exist and must be built (do not describe as present)

- **There is no `make science-gate` target and no figure-parity machinery.** The scientific test tier (`tests/scientific/test_scientific.py`) is *sanity/invariant* checks (`test_numerical_validity`, `test_zero_values`, `test_physical_ranges`, `test_unit_consistency`) — **not** relation-level comparison against digitised paper figures. Wherever this document says "reuse the existing SAGE parity machinery" for *figure* parity, read it as **build it**.
- **The hard distinction the future updater must hold onto:** byte-identity to the v1.0 SAGE baseline validates the *existing reimplementation* and powers **regression** and **gate-validation** (does the gate catch a known bug?). It says nothing about a *novel* model built from a new paper, which by construction will **not** be byte-identical to the SAGE baseline. A new model's correctness therefore rests on (a) physical/conservation invariants, (b) figure-parity against the paper within tolerance, and (c) non-regression of the trusted model where shared — and (b) is the weak link (§0.3).

### 0.3 Open problems the finalised version must resolve (lock these in)

1. **Figure-parity tolerance is unsolved and may be unsolvable per-relation.** Published relations disagree by 0.2–0.5 dex from sample variance, IMF, and binning; digitised curves carry their own extraction error. A tolerance loose enough to pass a correct reimplementation can be loose enough to pass a subtly wrong one. **Rule to lock in:** a relation may be an *auto-cleared mechanical* gate only if a realistic injected error is demonstrably caught at the chosen tolerance (the §10.1.4 test, run *per relation*). Any relation that fails that demonstration is a **Review-class** gate (frontier/human judgement), not auto-clear. Do not let figure-parity sit in the "Mechanical / None unless broken" row by default.
2. **Calibration is not autonomous, and the doc must stop implying it is.** The honest end state for Stage 4 on a *novel* coupled model is: the system produces a **best-effort calibration within the paper's stated priors, with named deviations, then defers to the user**, who has the expertise to judge, finish, or re-guide. Multi-relation weighting is a scientific decision and an escalation trigger, never auto-invented. The "overnight convergence" framing applies to *reproducing an already-trusted module* (the Phase-2 pilot), not to first-time calibration. Capture this uncertainty explicitly when finalising — it is a feature (correct humility), not a gap.
3. **Determinism / RNG contract.** This design anticipates stochastic physics with "deterministic seeds" (§10.3). That must mean **per-halo / per-FoF seeding from a stable key**, never a global RNG stream — otherwise it breaks the cross-format identity invariant the dual-driver work depends on (`MIMIC-DUAL-DRIVER-ARCHITECTURE.md` §7.1). Lock this constraint in.
4. **Named substep phase contract.** If `MIMIC-NAMED-SUBSTEP-PHASES.md` has landed by the time this design is finalised, the builder should map paper processes onto physically named middle phases under the fixed `pre_timestep`/`post_timestep` lifecycle. Do not keep teaching the builder that Mimic has exactly two generic middle buckets called `phase_1` and `phase_2`.
5. **Effort/scope honesty.** Any "weekend-buildable MVP" framing applies only to a narrow Phase-2 single-process pilot *given* Phase 0 (fleet) and Phase 1 (gate) already done, *given* the v1.0 SAGE baseline exists, and *given* the dual-driver work is complete enough that this document has been revised against the real code. The full system is weeks or longer, and establishing the figure-parity gate is itself non-trivial. State the dependency chain; do not let the headline collapse it.
6. **Unsubstantiated enabling claims to verify before trusting.** The MLX "~30–40% faster than llama.cpp" figure and the `robmost/sage_tree_converter` pattern are cited as fact; treat both as assumptions to validate, consistent with this doc's own "benchmark before trusting" discipline (§7.3).

### 0.4 When to review `VISION.md`

`VISION.md` is the guiding source of truth and is **not** edited now. This design *extends* the vision's scope (framework → substrate for assisted, gate-driven model-building) rather than violating any principle. The vision should be reviewed for a scope amendment **only when this document is itself finalised** (post v1.0, post dual-driver, post a working Phase-1 science gate) — i.e. after the dual-driver vision review (`MIMIC-DUAL-DRIVER-ARCHITECTURE.md` §6.1), not before. Do not amend the vision on the strength of an aspirational design.

---

## 1. Executive Summary

We want a system that takes a scientific paper describing a SAGE-scale galaxy formation model and helps turn it into a tested, scientifically-audited set of Mimic physics modules through a disciplined, gate-driven workflow. The long-term aim is that it can run for hours to a day, self-correct through ordinary engineering failures, report in periodically, accept mid-run steering, and stop only when both **code quality** and the available **scientific validation gates** clear a high bar — escalating to a human for scientific judgement calls and for any validation frontier the gates cannot honestly settle.

The design rests on one core insight and three structural commitments.

**The core insight — Mimic can supply unusually strong verification signals for long-running assisted model construction.** Most autonomous coding systems fail because "the agent thinks it's done" is unverifiable, so they drift into confident nonsense over long runs. Mimic is unusual: model-selected YAML-driven code generation (`make MODEL=<name> check-generated`), module metadata validation (`make MODEL=<name> validate-modules`), three test tiers (`make MODEL=<name> test-unit/integration/scientific`), a v1.0 SAGE parity baseline to be established before this work becomes active, and a 22-plot validation system are the raw material for a much stronger gate stack than a general software project can offer. The job of the multi-agent layer is to **drive those gates relentlessly**, plus add the one gate Mimic lacks today: an automated **science gate** that asks *do the output relations match the paper's figures and a trusted reference model, within tolerance?* If that gate is trustworthy for a relation, automation can clear that relation; if it is not, no amount of orchestration will make the conclusion safe. **Everything in this design is downstream of v1.0, the dual-driver work, and building and validating the science gate first.**

**Three structural commitments:**

1. **A dedicated builder repo, with Mimic as a pinned submodule.** Orchestration discipline lives in a new `mimic-model-builder` repo modelled on the [`robmost/sage_tree_converter`](https://github.com/robmost/sage_tree_converter) philosophy: a single `AGENTS.md` "constitution" defining a fixed, non-skippable staged path; per-stage skills the agent must read before acting; filesystem artifact handoff (state on disk, not in chat); strict per-stage write boundaries; and a **Model Knowledge Database** that compounds into reusable physics across builds. Mimic stays pristine — cloned at Stage 0 at a pinned commit, edited only through isolated git worktrees, and receiving a clean patch series at the end.

2. **A tiered model fleet with dual-path orchestration.** The workforce is local models on the M3 Ultra (free, private, high-volume: paper parsing, equation extraction, codebase search, first-draft C and plot code, log triage, first-pass review). The judgement is frontier-cloud: **Opus 4.8** as lead architect and final scientific arbiter; **Codex/GPT** as an *independent* adversarial reviewer and second implementer (genuine cross-model disagreement, the antidote to plausible-but-wrong physics). We start with a `tmux-cli`-driven Claude Code MVP and graduate to a durable **Claude Agent SDK** control loop for day-long runs.

3. **Dual acceptance, with inverted gates.** Nothing advances until it compiles, `check-generated` and `validate-modules` are clean, the relevant tests pass, *and* the science gate passes (figure parity + conservation invariants + reference parity + a vision cross-check). Quantitative gates are **auto-cleared by the orchestrator**; the human is invoked only at **G1 (spec approval, before any production code)** and at **explicit escalations**. Every implemented equation traces to a line in the paper; every paper claim traces to a source location and a test.

**What this could deliver after the prerequisites are real:** a narrow pilot that proves the gates on a model or module we already trust, growing into a durable system that runs unattended for substantial engineering work, reports status, and hands back a traceable scientific quality report. For novel coupled models, the realistic outcome is not guaranteed autonomous convergence; it is best-effort implementation and calibration inside explicit priors, with named deviations and human judgement where the paper or the gates underdetermine the answer.

---

## 2. Motivation and Context

### 2.1 The problem

Implementing a published galaxy formation model in Mimic is currently expert, hands-on work: read the paper, extract every equation and parameter with its units and redshift dependence, map each physical process onto Mimic's module/phase architecture, define properties in YAML, write and regenerate code, write tests at three tiers, build validation plots, calibrate the coupled model against observed relations, and reconcile every deviation from the paper. It takes days to weeks of focused effort, and the failure modes are subtle and dangerous: a plausible-but-wrong equation, a silent unit mismatch (we have already been burned by exactly this — the sev-5 quasar-wind unit bug in `sage_agn_physics.h`), a broken generated-code contract, or a test that only proves the code compiles rather than that the physics is right.

These are the tasks an agentic system is well-suited to *if and only if* its sense of "done" is grounded in something more reliable than its own confidence.

### 2.2 Why Mimic is a good fit for autonomy

Mimic was engineered around machine-checkable contracts, and that is precisely what a long-horizon agent loop needs:

- **YAML-driven code generation** (`src/core/core_properties.yaml`, `models/<MODEL>/model_properties.yaml` → C structs, init/output logic, output schema writers). `make MODEL=<name> check-generated` proves the generated code matches the selected model-set metadata — a hard contract an agent cannot fake.
- **Module metadata validation** (`make MODEL=<name> validate-modules`) checks dependencies, properties, and file consistency.
- **Three test tiers** — unit (C, fast), integration (Python, medium), scientific (Python, slow) — plus plotting unit/integration tests.
- **A reference baseline** — the v1.0 `tests/data/output/baseline/` output should become the genuinely trusted SAGE reference to diff against (regression) and to validate the science gate on (does it catch an injected bug?) before trusting it on anything new. *Caveat (§0.2): this validates the existing reimplementation and powers regression/gate-validation; a novel model from a new paper will not be byte-identical, so its correctness rests on invariants + figure-parity + non-regression, not on this baseline.*
- **A 22-plot validation system** that regenerates standard relations (stellar mass function, Tully–Fisher, mass–metallicity, cosmic SFRD, …) — the raw material for figure-parity scoring. *Note: the plots exist; the numeric figure-parity-against-paper gate that consumes them does **not** yet exist and must be built (§0.2).*

The architecture is also naturally decomposable: physics runs as runtime-configurable modules through a lifecycle with fixed optional `pre_timestep`/`post_timestep` work and, if the named-phase proposal has landed, user-named substep phases in between. Within each phase, full-halo/event work precedes galaxy-local work. One physical process maps cleanly to one module worked in one isolated worktree.

### 2.3 What "autonomous" means here — and what it does not

Autonomy here means **the loop runs without you**, not **the loop is unaccountable**. The system keeps working through ordinary failures (compile errors, failing tests, low-quality local-model drafts, branch conflicts) without asking. It stops and asks only for genuine scientific judgement. Explicit non-goals: a free-for-all swarm where any agent edits `main`; "autonomous" meaning untraceable; maximising agent count; or replacing frontier review with local models for final scientific correctness.

---

## 3. Goal and Requirements

**Functional**

- **F1 — Intake → spec → approval.** Ingest a paper (PDF/URL/arXiv) plus a one-line goal contract; produce a spec + plan + evidence ledger for human approval *before* writing production code.
- **F2 — Decomposition.** Decompose the model into Mimic modules: phase placement, processing mode, properties (YAML), parameters, dependencies, shared helpers, plotting extensions, and migration/archival if replacing active behaviour.
- **F3 — Incremental implementation.** Implement one physical process at a time, in isolation.
- **F4 — Full test coverage.** Generate tests at all three tiers plus new/extended validation plots.
- **F5 — Long autonomous run with steering.** Run unattended for hours-to-a-day, report periodically, resume from the last completed artifact after interruption, and accept mid-run steering without a restart.
- **F6 — Self-diagnosis.** On failure, diagnose → hypothesise → patch → re-gate; escalate when genuinely stuck.
- **F7 — Principled stop.** Stop only when acceptance criteria (code *and* science) are met, or a scientific judgement call requires a human.

**Quality**

- **Q1 — Traceability.** Every formula ↔ a paper location; every paper claim ↔ a source; every merge ↔ one accountable lead.
- **Q2 — Verifiability.** "Done" is defined by passing gates, never by an agent's say-so.
- **Q3 — Scientific correctness over plausibility.** The hard failures are plausible-but-wrong equations, silent unit mismatches, broken generated-code contracts, and tests that only prove syntax. The system must defend against each explicitly.
- **Q4 — Cost and privacy.** Bulk work runs locally and free; frontier spend is capped, itemised, and reserved for reasoning and final acceptance.

**Repository policy (inherited, non-negotiable)**

- Never edit `main` directly; branch first; never commit without explicit human approval; never use `--no-verify`.
- Archive, do not delete (old modules → `archive/src-modules/_archive/`).
- Capture long-running test output under `archive/test-logs/`; check exit codes explicitly and treat any non-zero as failure.
- Generated code changes only through generation commands, never by hand-editing `src/include/generated/`.

---

## 4. User Stories

These seven stories are the contract; every design decision below exists to make them true. (§14 maps the design back to each.)

- **US-1 — Paper to plan (gate before code).** *I drop a PDF and one line ("implement the Henriques+15 model in Mimic"). Within ~an hour I get a spec + evidence ledger — every equation mapped to a paper location, ambiguities flagged — plus a module architecture, to approve before any production code is written.*
- **US-2 — Overnight autonomous build.** *I approve and hit go. The system implements one process at a time in isolated worktrees, driving the full gate stack on each, and Slacks me a digest every few hours: what passed, what's blocked, current spend. I close my laptop.*
- **US-3 — Self-correction without me.** *The cooling module compiles and unit-tests pass, but the stellar mass function is 0.4 dex high. The system detects the science-gate failure, forms hypotheses (calibration vs. unit error vs. missing term), tests them, fixes it, and pings me only if it burns N cycles or hits a genuine scientific judgement call.*
- **US-4 — Morning review.** *I wake to a report: a table of modules × (code gates, science gates), thumbnails of generated-vs-paper figures, blockers with the system's best diagnosis, and reviewable diffs per worktree. I approve merges or redirect.*
- **US-5 — Mid-run steering.** *I interject: "use the Croton+06 cooling prescription, not the paper's, and cap AGN spend." The system folds the instruction into the plan and the cost cap without restarting.*
- **US-6 — Cost and privacy control.** *Bulk parsing/drafting/review runs on my Mac for free and never leaves the machine; cloud frontier spend is capped, itemised, and reserved for architecture, hard debugging, and final acceptance. I see a running tally.*
- **US-7 — Traceable handoff.** *At the end I get a scientific quality report: agreement per relation, deviations and suspected causes, unresolved ambiguities, model limitations — every claim linked to a paper location and a test.*

---

## 5. The Central Idea: Dual Acceptance and the Science Reward Signal

The single design decision that makes everything else work is defining **"done"** as the conjunction of two automatically-evaluated gate sets, per task. This is the reward signal that lets the loop run overnight without drifting.

### 5.1 Code gates (already in Mimic)

1. **Compile clean** — `make MODEL=<name>` succeeds, including the relevant variants (`USE-MPI=yes`, `USE-HDF5=no`) where the change touches them.
2. **Generated-code contract** — `make MODEL=<name> check-generated`: generated C/Python matches the YAML. Catches broken property contracts.
3. **Module metadata** — `make MODEL=<name> validate-modules`: dependencies, properties, files consistent.
4. **Tests** — `make MODEL=<name> test-unit` → `test-integration` → `test-scientific`, plus plotting unit/integration tests. Long output captured to `archive/test-logs/`; non-zero exit = failure.

### 5.2 Science gates (the new part — the heart of the system)

5. **Conservation & sanity invariants.** Mass / metal / baryon budgets conserved; no negative masses; monotonicity where physics demands it; values within physical ranges. Encoded as scientific tests with unit-explicit assertions.
6. **Figure parity vs the paper.** The plotting system regenerates the relevant relation (SMF, Tully–Fisher, mass–metallicity, SFRD, …); the result is compared numerically against the **digitised paper figure** within a stated tolerance (e.g. ≤ X dex over the calibrated range). Pass/fail is a number — *but the tolerance is the whole ballgame, and for some relations a tolerance that passes a correct model may also pass a subtly wrong one. This gate is auto-clearable only where a realistic injected error is provably caught at the chosen tolerance; otherwise it is Review-class. See §0.3.1 — this must be resolved per-relation when the design is finalised.*
7. **Reference-model parity.** Diff against the trusted v1.0 SAGE baseline to flag regressions and unphysical drift relative to a model we already trust. *This half becomes reusable once the v1.0 baseline is established (§0.1); for a novel model it checks non-regression where physics is shared, not equivalence (§0.2).*
8. **Vision cross-check.** A frontier vision model views the generated plot beside the paper figure and reports agreement/deviations in words — a qualitative backstop to the numeric test and a good escalation trigger.

### 5.3 The loop this enables

```
for each task in the DAG:
    implement (local draft → frontier review)
    run code gates           ── fail → diagnose → hypothesise → patch → retry (≤ N cycles)
    run science gates        ── fail → diagnose → hypothesise → patch → retry (≤ N cycles)
    if all green:            ── integrate (lead merges into integration branch)
    if N cycles exhausted,   ── escalate (Codex/Zen review → Opus lead → human)
       or conservation
       violation it can't
       resolve, or explicit
       scientific-ambiguity
       flag:
```

*Advance a task iff its code gates AND science gates pass; otherwise diagnose and retry; escalate after N failed cycles.* That is the whole control philosophy. It is trivial to reason about, and it is why the run can be left alone.

**Precondition (non-negotiable):** auto-clearing a gate is only as safe as the gate is trustworthy. An over-loose tolerance turns "autonomous" into "confidently wrong, fast." Therefore the science gate is **built and validated first**, on a model we already trust (the v1.0 SAGE baseline), before any relation is allowed to auto-clear (§12, Phase 1).

---

## 6. System Architecture

### 6.1 Two repos: builder (orchestration) + Mimic (product)

The orchestration discipline lives in a **new `mimic-model-builder` repo**, separate from Mimic. This separation is the single most important structural choice, and both prior analyses converged on it independently:

- **Mimic stays the product repo.** A failed or half-complete campaign never pollutes Mimic with orchestration scaffolding, transient prompts, worker transcripts, or partial ledgers. The deliverable back to Mimic is a clean branch/patch series plus docs, tests, plots, and the final report.
- **The builder repo is the reproducible operating system for model-building campaigns.** It versions independently of Mimic and carries the stage protocol, skills, gates, ledgers, worker profiles, reference templates, run archives, and the Model Knowledge Database.
- **Mimic is a pinned submodule.** Cloned at Stage 0 at an explicit commit SHA; pinning makes a build reproducible. Per-task git worktrees are created off that checkout so workers build in parallel without colliding or touching `main`.

This is the [`sage_tree_converter`](https://github.com/robmost/sage_tree_converter) philosophy — *put the workflow contract in the repo, not the agent's memory* — adapted from one-shot format conversion to coupled, expensive-to-judge physics building.

### 6.2 Repository skeleton

```
mimic-model-builder/                 # the constitution + path + memory
├── AGENTS.md                        # fixed staged path + gate definitions (the "constitution")
├── .ai/skills/                      # one SKILL.md per step — read-before-act
│   ├── paper-intake/        evidence-extraction/   model-spec/    module-architecture/
│   ├── skeleton-scaffold/   process-implement/
│   ├── unit-gate/                   # compile, check-generated, validate-modules, unit tests
│   ├── science-gate/                # figure-parity + conservation + reference parity + vision
│   ├── calibration/         auditor/                # auditor = cross-model adversarial review
│   ├── final-report/        kdb-register/
├── reference/                       # versioned contracts the agent reads
│   ├── mimic-module-contract.md     # how Mimic modules/phases/properties work
│   ├── mimic-test-gates.md          # exact gate commands (refreshed from Mimic AGENTS.md)
│   ├── science-gate-style.md        # tolerances, plot styles, parity targets
│   ├── digitised-figures/           # WebPlotDigitizer reference curves per paper
│   └── *-schema.md                  # evidence / design-spec / task / acceptance schemas
├── runs/<model-name>/               # per-run durable state (resumable, inspectable)
│   ├── 00-intake.md  01-evidence-ledger.md  02-design-spec.md
│   ├── 03-implementation-plan.md  04-validation-plan.md
│   ├── 05-run-log.md  06-final-scientific-report.md
│   ├── tasks.yaml  acceptance.yaml  worker-events.jsonl  steering.md
│   ├── patches/  plots/
├── model-database/                  # the Model KDB — compounding institutional memory
│   ├── prescriptions/               # learned modules: cooling / SF / feedback / AGN / metals …
│   ├── papers/                      # paper → module mappings, with tolerances
│   ├── parameter-priors/            validation-targets/
├── audits/                          # archived run bundles (morning-review + provenance)
├── orchestrator/                    # durable control loop (tmux MVP → Claude Agent SDK)
├── workspace/                       # gitignored
│   ├── mimic/                       # PINNED Mimic submodule/checkout — cloned at Stage 0
│   └── worktrees/<task-id>/         # per-task Mimic worktrees
└── container/  Makefile             # MLX/env reproducibility; gate shortcuts, lint, format
```

Key safety properties carried over from the pattern: **per-stage write boundaries** (Stages 1–4 may write only `runs/` and `workspace/worktrees/...`; only integration writes the Mimic integration branch), and **state on disk** so any run survives context compaction, crashes, and agent restarts.

### 6.3 The fixed staged path

The orchestrator must follow this path and may not skip stages, regardless of what the user supplies up front. Each stage has a skill the agent reads in full before acting.

| Stage | Name | What happens | Gate | Human? |
|---|---|---|---|---|
| **0** | Bootstrap | Clone/pin `workspace/mimic/`; record remote URL + branch + SHA; verify env (MLX gateway up, all gates runnable); run baseline Mimic gates on the clean checkout; refuse if Mimic has uncommitted changes | env + baseline green | no |
| **1** | Understand | paper-intake → evidence-extraction → model-spec → module-architecture | evidence complete + reviewed | **G1: approve spec before code** |
| **2** | Scaffold | properties YAML, module registration, parameter stubs, README placeholders | unit-gate (`make MODEL=<name> generate`/`check-generated`/`validate-modules`/`test-unit`) | no |
| **3** | Implement (fan-out) | for each process, in its own worktree: implement → unit-gate → *isolated* science-gate → independent review → integrate | per-process code + science gates | no (escalation only) |
| **4** | Couple & calibrate | assemble all processes; run the **full** science-gate on the coupled model; calibrate against published targets | full code + science gates | no (escalation only) |
| **5** | Audit & report | cross-model adversarial auditor + final scientific quality report | high automated bar (G-final) | no (escalation only) |
| **6** | Register | promote validated recipe into the Model KDB; archive audit bundle; notify | KDB-write gate | **async morning-after sign-off** |

Two adaptations the physics problem forces, relative to the linear conversion pattern:

- **Stage 3 is a fan-out DAG, not a line.** A SAGE-scale model is many coupled processes (cooling ↔ SF ↔ feedback ↔ AGN ↔ metals). Stage 3 iterates over a process list, one worktree per process; **Stage 4 is a distinct coupled-calibration stage** because relations like the SMF are only meaningful end-to-end and calibration is global (see §10).
- **KDB writes are protected against poisoning.** A subtly-wrong model that passes numeric gates could be auto-registered and poison every future build. So KDB promotion (Stage 6) is gated *harder* than stage advancement: an async morning-after human sign-off of the audit bundle promotes a recipe from `audits/` into `model-database/`. Full overnight autonomy, but institutional memory stays clean.

### 6.4 The gate model: three classes, inverted

The published `sage_tree_converter` pauses for a human at *every* gate — correct for a one-shot conversion a human can eyeball in seconds, fatal for an overnight physics build. We keep the structure and **reclassify the gates**:

| Gate class | Owner | Examples | Human involvement |
|---|---|---|---|
| **Mechanical** | Orchestrator/runner | `make MODEL=<name>`, `make MODEL=<name> generate`, `check-generated`, `validate-modules`, test exit codes, conservation invariants, reference parity, *and figure-parity only for relations that pass the per-relation injected-error test (§0.3.1)* | None unless infrastructure is broken |
| **Review** | Frontier models | design review, patch review, paper-to-code traceability, plot review, cross-model consensus, *figure-parity for any relation whose tolerance cannot separate correct from subtly-wrong (§0.3.1)* | None by default; summarised in the periodic digest |
| **Scientific decision** | Human (or delegated expert) | ambiguous formula the paper doesn't determine, calibration target weighting, unsupported approximation, replacing active model behaviour | **Stop and ask** |

The human is a *fallback*, not a per-stage toll: routine interruptions are **G1 (spec approval)** and **explicit escalations** only. This inversion is sound *only because* the mechanical science gate is trustworthy — which is why it is built first.

### 6.5 How the pieces connect

```
                         ┌──────────────────────────────────────────────┐
                         │   Durable Orchestrator (tmux MVP → Agent SDK) │
                         │   • owns task DAG + ledgers (on disk)         │
   human ── G1 / steering│   • runs gates, decides dispatch, escalates   │── Slack / push
   (steering.md, Slack)  │   • enforces write boundaries & cost caps     │   digests
                         └───────────────┬──────────────────────────────┘
                                         │ dispatches workers (each in a worktree)
        ┌────────────────────────────────┼───────────────────────────────────┐
        ▼                                ▼                                    ▼
  Local fleet (M3 Ultra, MLX)     Frontier lead (Opus 4.8)         Independent reviewer (Codex/GPT)
  • paper RAG / equation extract  • architecture & decomposition   • adversarial review
  • draft C modules + plot code   • hard debugging                 • second implementation
  • log triage, first review      • final scientific acceptance    • cross-model consensus (via Zen MCP)
  • local VLM first-pass judge        │                                    │
        │                             │                                    │
        └──────────────┬──────────────┴────────────────────────────────────┘
                       ▼
                Code gates + Science gates  (the only path to "done")
                       ▼
                Integration branch  ──►  clean patch series + report  ──►  Mimic
                       ▼
                Model KDB (async morning-after sign-off)
```

LiteLLM/OpenRouter sits in front of every model (local + cloud) as one OpenAI-compatible proxy with routing, fallback, and hard spend caps. Zen MCP gives the lead on-demand `consensus`/`codereview`/`debug`/`precommit` across models.

---

## 7. The Model Fleet

Use **roles**, not hard-coded model names; pin model IDs only in config and refresh them after benchmarking (model availability changes fast).

### 7.1 Role map

| Role | Tier | Class | Responsibility |
|---|---|---|---|
| **Lead orchestrator / architect / final arbiter** | Frontier cloud | **Opus 4.8** | Decomposition, cross-file judgement, hard debugging, final scientific acceptance, merges |
| **Independent reviewer / second implementer** | Frontier cloud | **Codex / GPT** | Adversarial architecture & patch review, alternative implementations for hard modules, final acceptance review |
| **Local coder** | Local | Large MoE coder (Qwen3-Coder-class, DeepSeek-V-class, GLM, Kimi K2) @ 4–8 bit | Draft C modules, plot Python, test scaffolding, narrow scoped edits |
| **Local researcher** | Local | Long-context model + small embed model | Paper RAG, equation/parameter/unit extraction, evidence-ledger drafting, codebase search |
| **Local log analyst** | Local | Fast model | Test-failure summaries, repeated log classification |
| **Local plot judge** | Local | Vision-capable model | First-pass "does this look like the paper?", escalate close calls |
| **Scientific reviewer** | Frontier + source | Opus/Codex with evidence | Equations, units, redshift dependence, calibration logic |

### 7.2 Local serving on the M3 Ultra (512 GB)

512 GB unified memory is a rare capability: frontier-class open weights, in MoE form, at long context, several instances concurrently. Treat the Mac as a **private, free worker farm** and reserve cloud calls for the few hard decisions.

- **Engine: MLX-first.** On Apple Silicon, MLX (MLX-LM, or LM Studio's MLX engine, or `vllm-mlx`) is ~30–40% faster than llama.cpp Metal, with the biggest gap on time-to-first-token — which dominates agentic loops. Keep llama.cpp as a GGUF-only fallback. *(This corrects the earlier llama.cpp/Ollama default; the KV-cache and serving guidance from those notes still applies.)*
- **Mandatory env settings** (carried over, they protect the KV cache and ports): `CLAUDE_CODE_ATTRIBUTION_HEADER=0`, `CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1`.
- **Gateway: LiteLLM** (OpenAI-compatible) and/or an Anthropic-compatible endpoint, so the *same* worker can be a Claude Code instance (`ANTHROPIC_BASE_URL`, subshell-isolated for concurrency) or an SDK worker (OpenAI base URL).
- **Serve specialised endpoints**, not one universal model: `local-coder`, `local-long`, `local-fast`, `local-vision`, `local-judge`. The 512 GB hosts a big coder *and* a RAG model *and* a VLM simultaneously.

### 7.3 Benchmark local models before trusting them

Local tool-calling reliability varies a lot by model and quantisation. Before assigning real work, score each candidate on Mimic-specific tasks: extract equations from a known paper section into the evidence schema; explain a Mimic module phase from source; add a trivial property and regenerate code (`make MODEL=<name> generate` clean); diagnose a seeded unit-test failure; compare a generated plot to a target. Promote a model to a role only once it passes its role's benchmark.

---

## 8. Tooling — Evaluation and Roles

Consolidated verdict on every tool considered across both analyses. The recommended stack is marked **★**.

| Tool | What it is | Verdict | Role in this design |
|---|---|---|---|
| **★ tmux-cli** | Programmatic control of interactive CLIs in tmux panes (launch, send keys, capture output, wait-for-idle, exit codes as JSON) | Adopt now | Dispatch/observation layer for the MVP; fallback in the durable system. Brittle as the *sole* day-long state manager. |
| **★ Claude Agent SDK** | Durable orchestrator in TS/Python inside the Anthropic ecosystem (native tool use, subagents, hooks, MCP, prompt caching) | Adopt for the durable loop | Primary orchestrator for day-long runs; the natural fit with an Opus lead. |
| **★ Zen MCP / pal-mcp** | MCP server letting Claude Code *consult* other models with `consensus`/`codereview`/`debug`/`analyze`/`precommit` | Adopt | The cheapest way to add a real multi-model QC brain. The review/consensus layer; bolt on immediately. |
| **★ MLX serving stack** | MLX-LM / LM Studio MLX / `vllm-mlx` | Adopt as primary local engine | The correct engine for Apple Silicon. |
| **★ LiteLLM + OpenRouter** | One OpenAI-compatible proxy over all models, with routing, fallback, **hard spend caps** | Adopt | Cost control (Q4/US-6); decouples worker code from any provider. |
| **★ git worktrees** | Native parallel isolation — one checkout per process off a shared branch | Adopt | Parallel module work without `main` conflicts. The harness already supports `isolation: "worktree"`. |
| **★ WebPlotDigitizer (+ vision model)** | Digitise paper figures into reference curves | Adopt | Gives the science gate quantitative ground truth, not just vibes. |
| **★ Alt-provider Claude Code** | Point Claude Code at local/cheap models via `ANTHROPIC_BASE_URL` + subshell env | Adopt for workers | Worker Claude Code instances on local/cheap models. Use for secondary workers, not the lead. |
| **★ Local servers (llama.cpp / LM Studio / Ollama)** | OpenAI/Anthropic-compatible local serving | Adopt mechanism, prefer MLX | llama.cpp = GGUF fallback; LM Studio = MLX engine + management; serving guidance (context slots, chat templates, first-request latency) is operationally important. |
| **Claude Code native primitives** | subagents, hooks, model routing, `Workflow`/`Task`, `schedule`/`Cron`, push/Slack MCP | Use what's already there | Hooks for logging/gates/notifications without bloating context; built-ins may be *enough* for the first loop before any custom build. |
| **Langroid** | Python multi-agent framework | Alternative control plane | Competes with the Agent SDK; pick one (we pick the SDK). The CC plugin is low-cost to add. → §15 |
| **Hermes Agent (Nous)** | Self-hostable autonomous agent: persistent memory, scheduling, sandboxed subagents, multi-channel reporting | Optional host | Best fit for the *host + reporting* role, not the physics brain. Borrow for hosting/reporting if desired. → §15 |
| **OpenHands / SWE-agent** | Software-agent platforms / SDKs | Swappable worker bodies | Capable autonomous SWE agents; adopt-not-build option. Keep as workers, not the core. → §15 |
| **Aider** | Repo-map pair-programmer, many providers | Swappable local worker | Strong local-model worker on narrow, explicit-scope patches. |
| **pi.dev** | Minimal terminal harness, mid-session model switch, RPC/SDK, steering | Not in core stack | Redundant given Claude Code + Codex; its "no sub-agents" stance is the opposite of what we need. RPC/steering bits are interesting. → §15 |

---

## 9. Operating Model — Artifacts, Isolation, Reporting, Escalation

### 9.1 Ledgers (machine-readable, on disk, resumable)

Three committed ledgers per run give traceability (Q1) and crash recovery.

**Task ledger** (`tasks.yaml`) — the DAG of processes with status × gate results. Minimal entry:

```yaml
- id: cooling-001
  title: Implement gas cooling process
  status: pending            # pending | in_progress | gated | blocked | done
  owner: local-coder-1
  worktree: workspace/worktrees/cooling-001
  inputs:
    - runs/henriques15/01-evidence-ledger.md
    - workspace/mimic/docs/DEVELOPER-GUIDE.md
  expected_outputs:
    - workspace/mimic/models/sage/modules/cooling/
  file_scope:                # the lease — workers write only here
    - workspace/mimic/models/sage/modules/cooling/**
  acceptance:
    code_gates:    [compile, check-generated, validate-modules, test-unit]
    science_gates: [conservation, figure-parity:SMF, reference-parity]
  logs:
    - archive/test-logs/henriques15/cooling-001.log
```

**Evidence ledger** (`01-evidence-ledger.md`) — paper claim → source location (page/section/equation) → implemented symbol (file + function), with units, redshift dependence, parameter defaults, and allowed ranges. Plus an explicit list of ambiguities.

**Acceptance ledger** (`acceptance.yaml`) — per-relation quantitative targets and tolerances, and pass/fail. This is what makes "scientifically validated" mean something specific.

### 9.2 Worker contract

Each worker call **states**: task ID, objective, explicit input files, explicit allowed output files (the lease), the exact gate(s) it must pass, the worktree it owns, the required evidence format, command/test expectations, and "do not commit." Each worker **returns data, not prose**: files changed, summary, evidence links, tests run + exit codes, remaining risks, and the patch/branch name.

### 9.3 Isolation rules

One task → one worktree. One writer per file at a time; workers read broadly, write narrowly (within the lease). Only the **lead** merges, into the integration branch, and only after all gates are green. Reviewers do not edit during review. Generated files change only through generation commands. Never edit `main`.

### 9.4 Reporting

A periodic digest (every N hours or per process-completion) to Slack MCP / push notification / Hermes channel, plus a human-readable `05-run-log.md`. Format:

```
Time:                 Current stage:
Completed since last: Active workers:
Tests run / failures: Science-gate status (per relation):
Scientific risks:     Spend tally (cumulative, vs cap):
Next actions:         Human input needed:
```

### 9.5 Escalation ladder

`local worker → Zen/Codex independent review → Opus lead → you`, triggered by N failed gate cycles, a conservation-law violation it can't resolve, or an explicit scientific-judgement flag. **Escalate (stop and ask) only when:** the paper is too ambiguous to choose between scientifically meaningful alternatives; calibration needs subjective target weighting; a required dataset/reference is unavailable; a test reveals a real scientific contradiction (not a coding bug); a change would archive/replace major existing behaviour; or runtime cost exceeds the agreed budget. **Do not stop** merely for a compile error, a failed unit test, a plot-script tweak, a low-quality local-model draft, or a branch conflict — those are normal work items.

### 9.6 Steering channel (US-5, first-class from day one)

A watched `runs/<model-name>/steering.md` (and/or the durable orchestrator polling a Slack channel). Between sub-steps the orchestrator reads it and folds new instructions into the plan, the acceptance ledger, and the cost cap **without restarting**. This is the one gap the prior pattern left open; here it is a core component, not an afterthought.

---

## 10. The Science Gate and Coupled Calibration (in detail)

This section gets first-class treatment because it is both the system's center of gravity *and* the part neither prior analysis fully resolved.

### 10.1 Building the gate (Phase 1 work, before any autonomous clearing)

For a chosen reference model already in Mimic:

1. **Digitise 2–3 key figures** with WebPlotDigitizer into reference curves under `reference/digitised-figures/`, with explicit x/y units and the calibrated range.
2. **Encode conservation invariants** as scientific tests: mass/metal/baryon budgets, non-negativity, monotonicity where physics requires it — all unit-explicit (the defence against the class of bug that bit us in `sage_agn_physics.h`).
3. **Wire a `make science-gate`** target that regenerates the relevant plots, computes a numeric parity score against the digitised curves (e.g. max |Δ| in dex over the calibrated range), diffs against the SAGE reference baseline, and emits structured pass/fail plus the vision cross-check.
4. **Validate the gate on the known-good model:** confirm it *passes* the v1.0 model we trust and *fails* when we deliberately perturb a parameter or inject the historical unit bug. A gate that can't catch a known bug is not trustworthy, and until it is, autonomous clearing for that relation stays off. Tolerances live in `reference/` under version control and are reviewed like code.

### 10.2 Per-process vs coupled evaluation

In Stage 3, each process is gated in **isolation** on what is meaningful alone (conservation invariants, sanity ranges, unit checks, and any single-process diagnostic). Whole-model relations (SMF, Tully–Fisher, mass–metallicity, SFRD) are only meaningful for the **assembled** model, so they are evaluated in Stage 4 on the coupled run.

### 10.3 Calibration is multi-objective optimisation, not a single gate

This is the hard part a naive design under-weights. A SAGE-scale model has several free parameters (SF efficiency, feedback loading, reincorporation timescale, AGN radio-mode efficiency, …) that jointly determine several relations at once, with trade-offs (tightening the SMF knee can loosen the mass–metallicity relation). The system must therefore:

- **Treat Stage 4 as a calibration loop, not a pass/fail check.** Start from the paper's published parameter defaults (captured in the evidence ledger), run the coupled model on a small test volume, score all target relations from the acceptance ledger, and adjust parameters within their paper-stated priors to minimise a weighted multi-relation objective.
- **Make the weighting explicit and human-owned.** How to trade SMF agreement against SFRD agreement is a *scientific decision*, not something to auto-invent — it is exactly a Stage-4 escalation trigger. Default to the paper's stated calibration targets and weights; if the paper doesn't specify, stop and ask (US-5/escalation).
- **Use deterministic seeds** for stochastic physics so calibration is reproducible and parity scores aren't chasing noise.
- **Run cheaply, validate fully.** Calibrate on a small test volume / subset of trees; only run the representative Millennium configuration for the final acceptance pass, to keep frontier and compute cost bounded.

Calibration convergence (or a documented "best achievable within priors, with named deviations") is itself part of G-final.

**Honest framing for the finalised version (§0.3.2).** For a *novel* coupled model, do not expect autonomous convergence. The realistic, correct end state is a **best-effort calibration within the paper's priors, with named deviations, handed to the user** — who has the expertise to judge whether it is acceptable, finish the calibration, or re-guide the system. This deferral is not a shortcoming of the design; it is the appropriate division of labour between machine (exhaustive gating, bookkeeping, first-pass optimisation) and scientist (judgement on trade-offs the paper underdetermines). The "overnight, converges because done is machine-checkable" language elsewhere in this document describes *reproducing an already-trusted model* (the Phase-2 pilot), where the byte-exact SAGE reference makes "done" genuinely exact; it does **not** describe first-time calibration of a new model.

---

## 11. End-to-End Workflow (per run)

A concrete walk-through of the staged path, with each stage's workers, outputs, and gate.

**Stage 0 — Bootstrap.** Clone Mimic into `workspace/mimic/`; record remote/branch/SHA in `00-intake.md`; run baseline gates on the clean checkout; refuse if there are uncommitted changes. Create `runs/<model-name>/` and the initial `tasks.yaml`/`acceptance.yaml`. *Inputs:* paper PDF/URL, target model name, target simulation (e.g. Millennium), reproduce-figures-exactly vs equivalent-diagnostics, any reference implementation, cost budget.

**Stage 1 — Understand.** *Workers:* local long-context readers + frontier scientific reviewer. Extract equations, parameter defaults, units, algorithm order, calibration targets, required state variables, validation figures, and ambiguities into `01-evidence-ledger.md`. Map processes to Mimic phases, using physically named substep phases if that contract has landed; define module boundaries, shared helper APIs, new `model_properties.yaml` properties, input-YAML parameters, generated-code updates, tests, plots, and any archive/migration. Produce `02-design-spec.md`, `03-implementation-plan.md`, `04-validation-plan.md`. **Gate G1: human approves the spec before any production code** (the one mandatory human gate, US-1). Frontier reviewer checks source traceability first.

**Stage 2 — Scaffold.** Add module directories + metadata, README placeholders, parameter schema, property definitions, generated-code updates, basic compile tests. *Gate:* `make MODEL=<name> generate` → `check-generated` → `validate-modules` → `test-unit`. Auto.

**Stage 3 — Implement (fan-out loop).** For each process (initialisation, cooling, star formation, stellar feedback, reincorporation, metal enrichment, black-hole growth, AGN feedback, mergers, disk instabilities, environment, luminosities as needed), in its own worktree, the mini-cycle: evidence reviewed → interface/state defined → implementation patch (local draft → frontier review) → unit tests → integration check → isolated science-gate (conservation/sanity) → independent review → lead merges. *Gate:* per-process code + science gates; evidence ledger updated with implemented locations; no out-of-scope changes. Auto, with escalation.

**Stage 4 — Couple & calibrate.** Assemble all processes; run the full model on small test data then a representative Millennium config; generate diagnostics; run the **full** science-gate; calibrate against published targets (§10.3). *Gate:* all three test tiers + plot generation + full figure parity + reference parity + final scientific-reviewer approval. Auto, with escalation on calibration trade-offs.

**Stage 5 — Audit & report.** Cross-model adversarial auditor (Codex/Zen disagreeing with the implementation) + `06-final-scientific-report.md`: agreement per relation, deviations and suspected causes, unresolved ambiguities, model limitations, every claim linked to a paper location and a test. *Gate:* G-final, a high automated bar; lead + independent reviewer agree.

**Stage 6 — Register.** Promote the validated recipe into the Model KDB; archive the audit bundle; notify. *Gate:* KDB-write gate + **async morning-after human sign-off** before promotion from `audits/` into `model-database/`.

---

## 12. Phased Implementation Roadmap

Deliberately ordered so the science gate exists before any autonomous clearing. The timings below are provisional placeholders for the future updater, not commitments; they must be revised after v1.0 and the dual-driver work.

**Phase 0 — Foundations (½ day).** Stand up the MLX gateway + LiteLLM with a hard spend cap; wire `ANTHROPIC_BASE_URL` worker functions (subshell-isolated) for at least one big local coder; set the KV-cache env vars; install Zen MCP into Claude Code; verify a local-backed Claude Code instance can edit a file and run `make`. Benchmark candidate local models on the five Mimic-specific tasks (§7.3).

**Phase 1 — Gates first, agents second (timing to be re-estimated). *The gate before the loop.*** Build the **science gate** on a model already in Mimic: digitise reference figures, encode conservation invariants, write `make science-gate`, and *validate it* relation by relation — it must pass the trusted model and fail an injected bug/perturbation where it claims mechanical authority. No autonomous clearing until this is trustworthy. A narrow first pass may use 2-3 figures, but production use requires per-relation tolerance validation.

**Phase 2 — MVP orchestration (timing to be re-estimated).** Opus-in-Claude-Code as lead, driving a `tmux-cli` worker pool (local-backed Claude Code, Codex), running the gates, writing the ledgers into the builder repo. Use built-in `Workflow`/`Task` for fan-out and `schedule`/Slack MCP for reporting. Pilot on a *single, well-understood process* — reproduce one existing module from its paper, end-to-end: intake → spec → worktree → implement (local) → review (Codex/Zen) → gates → report. Tune prompts, ledgers, escalation thresholds, and steering here. This is the first place to test US-1 through US-4 on a narrow run; it does not prove the full system.

**Phase 3 — Durable orchestrator (1–2 weeks).** Port the proven prompts/gates into a thin **Claude Agent SDK** control loop: persisted state, worktree-per-process parallelism, the steering channel, and Hermes (or built-in `schedule` + Slack MCP) for day-long running and reporting. Point it at the real SAGE-scale paper. *This is the end state — design #1 below.*

**Phase 4 — Hardening.** Cost dashboards; a regression suite of past papers (re-run known models, confirm the KDB makes the Nth model cheaper/more deterministic); escalation-threshold tuning; a "completeness critic" pass that asks *what's unverified / unread / unphysical* before declaring done; refresh of the cached gate contract against Mimic's `AGENTS.md` via the controller self-check.

---

## 13. Risks and Guardrails

| Risk | Mitigation |
|---|---|
| **Plausible-but-wrong physics** | Cross-model consensus (Zen/Codex disagreeing with the local draft); mandatory paper-location citation per equation; numeric science gate. |
| **Silent unit mismatches** (we've been burned) | Unit-explicit scientific tests + conservation invariants + reference-parity diff in the gate; the gate must demonstrably catch the historical bug. |
| **Tests that only prove syntax** | Science gates are about *output relations*, not compilation; the acceptance ledger requires a quantitative target per relation. |
| **Generated-code contract drift** | `make MODEL=<name> check-generated` and `validate-modules` are blocking gates every cycle; never hand-edit `src/include/generated/`. |
| **Runaway spend** | Bulk work local; LiteLLM/OpenRouter hard cap; frontier reserved for lead/acceptance; live tally in every report. |
| **Long-run state loss** | Durable on-disk ledgers + resumable orchestrator; never keep day-scale state only in a chat session. |
| **Over-automation of judgement** | G1 plan-approval + explicit escalation rules; "autonomous" = unattended, not unaccountable. |
| **Calibration trade-offs auto-invented** | Multi-relation weighting is a scientific decision → Stage-4 escalation; default to the paper's stated targets/weights. |
| **KDB poisoning** | Gate KDB *writes* harder than stage advancement: async morning-after human sign-off before a recipe is promoted into `model-database/`. |
| **Untrustworthy science gate** | Phase 1 validates the gate on a known-good model before any autonomy; tolerances versioned in `reference/` and reviewed. |
| **Two-repo / nested-git friction** | `workspace/` gitignored; commit only Mimic URL/branch/SHA + patch refs; every worker prompt states controller root, Mimic root, and worktree root; controller self-check refuses to run if Mimic's `AGENTS.md` diverges from the cached gate contract; final patches must apply cleanly to a fresh Mimic checkout before a run closes. |

---

## 14. How the Design Satisfies Each User Story

| Story | Where it's satisfied | Strength |
|---|---|---|
| **US-1** paper→plan, approve before code | Stage 1 + **G1**, the one retained mandatory human gate; evidence ledger gives the traceability table | Strong target |
| **US-2** overnight autonomous build | Gate inversion (§6.4) + fan-out Stage 3 + durable orchestrator + Slack digests | Strong for engineering work once gates exist; provisional for novel science |
| **US-3** self-correction without me | The diagnose→hypothesise→patch→re-gate loop (§5.3); "never skip a validation failure — N cycles then flag" | Strong for ordinary failures; scientific ambiguity still escalates |
| **US-4** morning review | Per-run audit bundle + science-gate table + figure thumbnails + per-worktree diffs (§9.4, §11 Stage 5) | Strong target |
| **US-5** mid-run steering | First-class `steering.md` watched channel + orchestrator polling (§9.6) | Strong target |
| **US-6** cost/privacy control | Local-first fleet + LiteLLM/OpenRouter hard caps + per-skill tier declarations + live tally (§7, §9.4) | Strong target, pending tool validation |
| **US-7** traceable handoff | Evidence ledger + acceptance ledger + final scientific report + archived audit + KDB recipe (§9.1, §11) | Strong target |

All seven are design targets. They should not be marked achieved until the post-dual-driver revision proves the gate stack, orchestration, and reporting against working code.

---

## 15. Alternative Design Choices

These are real options the team may prefer; recording them sharpens the recommendation rather than hiding the trade-offs.

**A1 — Langroid as the durable control plane (instead of the Claude Agent SDK).** A mature Python multi-agent framework with typed tools, structured messages, explicit task routing, and MCP. *For:* model-agnostic, durable, a natural home for typed ledgers and acceptance gates. *Against:* competes directly with the Agent SDK and adds infrastructure to build/debug before solving the science problem; the SDK is a more natural fit with an Opus lead and keeps us in one ecosystem. *Verdict:* viable Phase-3 alternative; pick one control plane, and we pick the SDK. The Langroid CC plugin (a pattern library) is low-cost to add regardless.

**A2 — Hermes (or OpenHands) as the long-run platform (instead of a hand-built loop).** Adopt a platform that owns scheduling, sandboxed subagents, persistent memory, and multi-channel reporting; supply only the Mimic workflow and gates. *For:* day-long running, isolation, and Slack/Telegram reporting come for free — least harness code. *Against:* you inherit *their* control-flow and abstractions, get a looser grip on science-gate logic, and add trust surface (persistent memory can also preserve stale assumptions; audit before granting production-write authority). *Verdict:* best borrowed for the **host + reporting** role inside the recommended design — keep our gates and lead model. Don't make it the brain or the first trusted merge authority.

**A3 — Single-repo, in-Mimic pilot (instead of a separate builder repo).** Run the whole thing inside Mimic, with ledgers under `docs/models/<model-name>/` and logs under `archive/test-logs/`. *For:* lowest setup cost, immediately adoptable, no two-repo bookkeeping. *Against:* mixes orchestration artifacts with product code; less reusable across campaigns; long unattended runs are harder to audit cleanly; a failed campaign litters Mimic. *Verdict:* fine as the very first pilot in Phase 2 if standing up a second repo is a blocker, but the separate builder repo is the target — extract early.

**A4 — Zen-MCP-centric "quality brain" as the whole system.** One Claude Code driver consults Gemini/Codex/local models via Zen MCP at every decision. *For:* the strongest *correctness* layer for the least effort; cross-model consensus is the direct antidote to plausible-but-wrong physics. *Against:* one model "does the work," so it's weak as a *standalone autonomy* engine and isn't a day-long runner by itself. *Verdict:* don't run it alone — **fold it into the recommended design as the review/consensus layer** (already done, §6/§8). Highest value-per-effort addition.

**A5 — pi.dev as a single scriptable multi-provider harness (instead of tmux-driving CC/Codex).** *For:* RPC/SDK mode + mid-session model switch + steering in one tool. *Against:* redundant given Claude Code + Codex already cover the harness role, and its deliberate "no sub-agents" stance is the opposite of what fan-out needs. *Verdict:* revisit only if a single scriptable multi-provider harness becomes desirable; not in the recommended stack.

**A6 — llama.cpp/Ollama as the primary local engine (instead of MLX).** *For:* the widest model/format coverage (GGUF), and the documented serving/KV-cache guidance is excellent. *Against:* ~30–40% slower than MLX on Apple Silicon, with the gap concentrated in time-to-first-token, which dominates agentic latency. *Verdict:* MLX primary, llama.cpp as the GGUF-only fallback.

---

## 16. Bottom Line

After v1.0 and the dual-driver migration, build the **science gate first** and prove it on a model we already trust. Then give the system a **spine**: a `mimic-model-builder` repo in the `sage_tree_converter` mould — an `AGENTS.md` constitution, per-step skills, filesystem artifact handoff, per-stage write boundaries, a compounding Model KDB, and Mimic as a pinned submodule — but with gates classified carefully so only validated mechanical checks are auto-cleared. Wrap it in the smallest orchestration that drives it: Opus + `tmux-cli` to start, then a durable control loop if the pilot proves the value. Staff it with local models for bulk work, independent model review for adversarial checking, and git worktrees plus on-disk ledgers for safe, parallel, traceable, resumable work. Treat coupled calibration as the multi-objective optimisation it really is, with weighting kept in human hands.

That stack is plausible only if the gates earn trust relation by relation. For already-trusted reproductions, "done" can become machine-checkable. For novel coupled models, the honest target is a traceable best-effort implementation and calibration report with explicit unresolved scientific decisions.
