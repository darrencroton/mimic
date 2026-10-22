# Mimic Model Builder Prototype

**Status:** Proposal and design brief, not an implementation plan. Written 2026-10-20 for the owner to extend; nothing here is frozen.
**Scope:** A first working version of the model builder of [`MIMIC-MODEL-BUILDER-PLAN.md`](MIMIC-MODEL-BUILDER-PLAN.md) (pathway step 7). Given a brief, it works out what to do, designs a complete campaign, writes its own implementation plans, has them carried out by the existing ai-agent-coder tools, validates the results with a validation layer of its own, asks the human when it must, and runs for days. Importing SAGE26 into Mimic as a new model package (`sage16` → `sage26`) is the exercise it is built and tested against.
**Relationship to the pathway:** It proposes starting step 7 now, as a prototype, in parallel with step 4. The long-term brief stays the requirements anchor, and this document is the starting point for that brief's overdue re-review. [Sequencing Against the Pathway](#sequencing-against-the-pathway) sets out the case and what would change it.

---

## Summary

- **What it is.** A **Campaign Manager (CM)**: a supervising agent seat plus a small deterministic toolkit (`cm.py`) that sits one level above ai-agent-coder's Project Manager (PM). PM supervises a frozen plan's slices. CM supervises a campaign: a graph of nodes, each carried out by one frozen plan run under PM. CM writes those plans itself, under a **charter** the human approves once. Everything specific to Mimic lives in a **domain pack** in this repository, so the CM stays generic.
- **Why this shape.** ai-agent-coder already guarantees that a plan is carried out with a mechanical floor, recorded judgement and independent review. What it explicitly refuses to do is write or repair plans ("not a planner-free autopilot"). The builder is precisely that missing layer. The design repeats PM's proven pattern one level up rather than inventing a new one.
- **Validation is CM's own, not PM's.** PM proves each change is right against its contract. CM proves the campaign advanced: it runs declared instruments with fixed thresholds at each node's final commit, enforces a ratchet so nothing that passed can regress, and checks that each instrument detects an injected error before it is trusted.
- **Recommendation: start now.** Nothing still to come on the pathway is a prerequisite. Step 4 is complementary and should run in parallel. The emulator and the coupled rate formulation both benefit more from the builder than the builder from them. Two unplanned pieces are worth doing early: PM's machine-facing adjustments, which the CM needs, and a **replay benchmark** built from the archived records of past campaigns, which tests the CM's planning against known human-approved answers without running anything.
- **First acceptance target: P0, design only.** Brief → charter → dossier → campaign graph, then stop, scored against a held-out answer key. Cheap, repeatable, no product code.

---

## Goal and Non-Goals

**Goal.** A prototype that turns a brief into a validated model package with bounded human attention:

1. It turns a brief into a charter through a short dialogue, and the human approves the charter.
2. It investigates the sources and the target repository itself and builds an evidence dossier.
3. It designs the whole campaign as a graph of nodes with dependencies, classes and exit validation.
4. It writes a frozen implementation plan for each node just in time, has it reviewed independently, and runs it under PM.
5. It validates every node with its own instruments, diagnoses failures and plans fixes, within budgets.
6. It asks the human structured questions only where the charter requires, keeps working on whatever a question does not block, and runs across days without a long-lived context.
7. It finishes with a validation report, an independent science audit, the human's sign-off and a retrospective.

**Non-goals for the prototype.**

- **No science gates for novel models.** The exercise has a reference implementation to compare against, which is the strongest gate available. Gates for models built from a paper alone (emulator, digitised figures, invariants as acceptance) stay with step 5 and the long-term brief. The instrument contract below is written so they can join later.
- **No calibration.** SAGE26's parameters are already calibrated; the exercise imports them.
- **No parallel execution of PM runs.** Read-only work fans out; code-changing nodes run one at a time.
- **No changes to upstream repositories or remotes** beyond what the charter permits.
- **Not adversary-proof**, in the same sense as ai-agent-coder: it defends against drift, corner-cutting and overconfidence, not a determined fabricator.

---

## The Core Idea: One More Level of the Same Pattern

| | Frozen contract | Unit of work | Executor | Authority that may not be exercised by the supervisor |
|---|---|---|---|---|
| **PM** (exists) | Implementation plan | Slice | Developer session | Amending the plan; approving a human-gated slice |
| **CM** (new) | Campaign charter | Campaign node, carried out as a frozen plan | A PM run | Amending the charter; approving a human-reserved plan class or a PM approval gate |

CM does what PM is forbidden to do: it investigates, designs, writes plans and revises them after a PM stop. It never touches the charter. Every other property of PM carries over unchanged:

- A small mechanical floor that cannot be waived, checked by `cm.py`.
- Recorded, accountable judgement above it: every node decision cites the evidence examined.
- Independent review, commissioned by the judge, never by the implementer.
- A fresh agent session per unit of work, so context never accumulates.
- Durable signed state, so a crash or a pause loses nothing.

---

## Architecture

```text
                                    Human
     brief ─┐    approves charter   answers questions   signs off at the end
            ▼            ▲                  ▲                    ▲
┌──────── Campaign Manager: a fresh agent session per "tick" ───────────────┐
│  cm.py toolkit: state · event log · campaign graph · question queue ·      │
│                 campaign floor                                             │
│                                                                            │
│  recon ─► dossier ─► graph ─► node spec ─► plan ─► plan review ─► PM run   │
│    ▲                   ▲                                            │      │
│    └──── diagnose ◄── fail ◄── validation layer (instruments) ◄─────┘      │
└────────────────────────────────────────────────────────────────────────────┘
     │ read-only delegates (orchestrator)          │ PM seat → Developers, reviewers
     ▼                                             ▼
  reference source and outputs                  target worktree on the
  at a pinned commit                            campaign integration branch
```

### Seats

| Seat | Role | Suggested model |
|---|---|---|
| **Campaign Manager** | Designs the campaign, writes plans, decides on the record, routes questions | Strongest available |
| **Recon and diagnose delegates** | Read-only investigation returning structured evidence with `file:line` references | Cheaper, run in parallel |
| **Plan reviewer** | Adversarial review of each plan against the charter, the dossier and the repository | A different model or harness from the CM |
| **PM** | Carries out one frozen plan, exactly as today | Strong |
| **Developers and code reviewers** | As today | Per plan |
| **Science auditor** | Independent review of the instrument evidence at milestones and at acceptance | Strong, independent of the CM |

### Toolkit

`cm.py` mirrors `pm.py`: it owns state, the event log, the graph, the question queue, session launch and the campaign floor; the CM agent owns everything that needs reading and judgement. It should reuse PM's library (`skills/project-manager/scripts/pm_lib/`: `sessions.py` for tmux launch and readiness, `profiles.py` for harness differences, `state.py` for HMAC-signed state, `ledger.py`) rather than duplicate it.

Indicative commands: `init` (from an approved charter), `next` (a bounded situation summary for a tick), `graph` (revise, with a recorded reason), `plan` (register a plan version and its review), `approve` (human only), `run` (launch a PM seat for a node), `validate` (run a node's instruments), `ask` and `answer` (answer is human only), `notes`, `drive` (the supervisor loop), `status --report`.

### The campaign directory

Outside every product repository, under its own git history:

- `brief.md` and `charter.md` (the charter frozen by its digest)
- `dossier/`: source and target inventories, the mapping between them, gaps, the dependency graph of features, the risk register and the decisions log, every claim carrying `file:line` evidence
- `graph.yaml` and its revision history
- `plans/<node>/`: plan versions, plan reviews and approvals
- `runs/<node>/`: pointers to PM run directories and their structured outcomes
- `validation/`: the validation ledger (`commit, instrument version, configuration, verdict`) and reports
- `questions/`, `events.jsonl`, `state/` (signed), daily digests, the final report and the retrospective

---

## Lifecycle

### Intake: brief to charter

The human writes a short brief: the goal, the pinned sources, the target, what "done" means in words, constraints and a budget. The CM asks clarifying questions, then drafts a charter:

| Charter field | Content |
|---|---|
| Goal and definition of done | Validation targets with thresholds, not prose |
| Pinned references | Source commits, datasets, reference configurations |
| Node classes | For each class: an **envelope** (the paths its plans may touch) and an approval policy |
| Question policy | What must be asked, what may be decided and recorded, what is never asked |
| Budgets | Wall-clock time, spend, PM attempts, diagnose-and-fix cycles per node |
| Seats | Which model fills which seat |
| External actions | What, if anything, may be pushed or changed outside the campaign worktree |
| Exclusions | Paths the campaign may not read (used for hold-out evaluation) |

The human approves the charter once. Any later change to it is the human's.

### Recon: the dossier

The CM fans out read-only delegates through the `orchestrator` skill and assembles the dossier:

- a source inventory: processes, state, parameters and output, each with `file:line`;
- a target inventory: existing modules, properties, parameters, tests, plots and the framework's capabilities, read from the code, the guides and the `mimic-*` skills;
- the mapping between them, each process marked same, modified or new;
- gaps: everything the target cannot yet express, classified by the node class it would need;
- a dependency graph of the features to import;
- feasibility: can the reference code be built here, is the data present, what is missing from the environment;
- a risk register and a first list of questions.

The CM is expected to find the campaign's real obstacles at this stage, unprompted. That is what P0 measures.

### Campaign design: the node graph

A small, fixed vocabulary:

| Node type | Purpose | Execution |
|---|---|---|
| `recon` | Add to the dossier | Read-only delegates |
| `decide` | A question for the human; blocks its dependants | Question queue |
| `instrument` | Build a validation instrument | Frozen plan under PM; human-approved class; must pass a self-test |
| `implement` | Change the product | Node spec → frozen plan → PM run |
| `diagnose` | Localise a validation failure; produce hypotheses with evidence | Read-only delegates |
| `accept` | Final validation against the definition of done | Instruments, science audit, human sign-off |

Each node records its objective, entry conditions, exit validation (instruments and thresholds), class and dependencies. The skeleton is drawn up front; a node's plan is written only when the nodes it depends on have landed, so it is written against the real state of the repository. The graph is living: the CM may revise it within the charter on the record, but a revision that adds a human-class node or touches the definition of done goes to the human.

### The execution loop

For each ready node, one at a time:

1. **Plan.** Write a self-contained plan with the `implementation-plan` skill: the evidence it needs is copied in, source paths are absolute, the validation plan names the instruments.
2. **Check and review.** `pm.py check-plan`, then an independent plan review, then approval by the node's class.
3. **Run.** Launch a PM seat on the node's branch, `campaign/<id>/<node>`, cut from the integration branch.
4. **On a PM stop**, classify the cause: a plan defect becomes plan v2 and a new PM run (PM never sees a mid-run plan edit); a Developer failure changes the seat or re-slices; an environment failure is retried or asked about.
5. **On PM acceptance**, run the node's instruments in a clean worktree at the node's final commit.
6. **Pass:** merge into the integration branch, re-run the ratchet, update the graph.
7. **Fail:** a `diagnose` node, then a fix plan, then validation again, within the charter's cycle budget; when the budget is spent, a question to the human that carries the diagnostic dossier.

Step 7 is how a parity campaign's root causes get found without the owner doing the bisection.

### Acceptance and retrospective

The final `accept` node runs every exit validation in the definition of done, commissions the independent science audit, and asks the human to sign off. The retrospective records where human attention went, the questions asked and whether each was necessary, PM stop causes, plan revisions per node, diagnose cycles, spend and wall time. Lessons proposed for promotion into skills or plan templates need the human's sign-off, as the long-term brief requires for institutional memory.

---

## The Validation Layer

### Three levels, each owned separately

| Level | Owner | Question | Evidence |
|---|---|---|---|
| Slice | PM (exists) | Is the change right against its contract? | Floor, tests, lint, drift audit, code review |
| Node | CM (new) | Did the campaign advance? | Instruments at fixed thresholds, plus the ratchet |
| Campaign | CM, science auditor and human | Is the model right? | Definition of done, independent audit, sign-off |

### The instrument contract

The CM knows nothing about Mimic; it knows only this contract, which the domain pack fills in:

```yaml
id: parity
version: <commit of the instrument's code>
run: <command> --config {config} --target {worktree} --reference {cache}
verdict: JSON {pass, metrics, residuals}
self_test: {perturb: <a declared, realistic injected error>, expect: fail}
cost: cheap | expensive          # cheap ones re-run on every merge
```

Four rules give the layer its meaning:

1. **Trust before use.** An instrument version counts only after its self-test has failed on the declared perturbation. This is the long-term brief's injected-error requirement made mechanical.
2. **Independence.** Instruments are built in `instrument` nodes, approved by the human, and their paths are excluded from every `implement` envelope. The code under test cannot edit its judge, and PM's existing floor fact 5 enforces that once plan surfaces are drawn from the envelopes.
3. **The CM measures.** Instruments run in a clean worktree at the node's final commit, never inside the Developer's session; a Developer's report of success is a pointer to evidence, never the evidence.
4. **The ratchet.** A check that passed must keep passing. Cheap checks re-run on every merge, expensive ones at milestones, and thresholds can only be tightened from the charter's values.

The contract is deliberately general. Comparison against a reference implementation is the strongest instrument and the one the exercise uses; conservation invariants, digitised figure parity, review-class evidence and, later, the emulator all fit the same shape.

---

## Working Across Days

- **No long-lived CM context.** `cm.py drive` runs under tmux or launchd and wakes a fresh CM session (a tick) when a PM run ends, an instrument finishes, the human answers a question, or an hourly heartbeat fires.
- **Bounded ticks.** `cm.py next` prints the charter digest, ready and running nodes, open questions, budget state, recent events and the CM's curated notes. The CM decides one thing, acts, records, and exits. It is the same device that makes long PM plans tractable for Developers.
- **Durable state.** A crash, reboot or usage-limit pause means the next tick resumes from signed state.
- **Serial execution, parallel investigation.** Mimic forbids parallel test suites and ties builds to a working tree, so PM runs go one at a time. Recon and diagnosis fan out freely. A node blocked on a question never stops independent branches of the graph.

---

## Humans in the Loop

- **`cm.py ask`** records a structured question: the nodes blocked, context and evidence, the options and what each leads to, a recommendation, and what continues meanwhile.
- **`cm.py answer`** is a human command, like `pm.py approve`. The CM never answers its own questions. Answers become recorded decisions that later plans cite as authority.
- **Three classes**, set by the charter: *must ask* (scientific judgement, core changes, upstream changes, installing software, overspend, external side effects); *decide and record* (naming, ordering; shown in the daily digest, and overturning one reopens the affected nodes); *never ask* (anything mechanical).
- **Delivery** through a notification hook (push or email) plus a daily digest; immediate only when a question blocks the critical path.

---

## Authority and the Campaign Floor

**The CM may:** revise the graph within the charter; write and revise plans; commission recon, plan reviews, diagnoses and science audits; launch PM runs and choose seats; record minor decisions; merge validated nodes into the campaign integration branch.

**The CM may never:** change the charter; approve a human-reserved plan class or one of PM's approval gates; loosen a threshold or accept a node whose validation failed; let an `implement` node touch an instrument's paths; push to `main` or any remote beyond the charter; act on an upstream repository (it may draft a patch and ask).

**The campaign floor**, checked mechanically by `cm.py`:

1. The charter digest is unchanged.
2. Every PM run was preceded by a passing `check-plan`, a recorded plan review and an approval, all against that exact plan's digest.
3. Every plan's authorized surface lies inside its class envelope, and no `implement` surface overlaps an instrument path.
4. A node is marked validated only on verdicts recorded at its integrated commit, by instrument versions that passed their self-test.
5. No threshold is below the charter's.
6. The integration branch's ancestry is intact.
7. No budget is exceeded.

---

## Where It Lives

| Piece | Home | Reason |
|---|---|---|
| CM skill and `cm.py` | `ai-agent-coder/skills/campaign-manager/` | Generic; reuses `pm_lib`; becomes Rung 3 of that repository's autonomy ladder |
| `plan-review`, `recon`, `diagnose` skills | `ai-agent-coder/skills/` | Generic, and usable on their own like every other skill there |
| Domain profile | `.agents/builder/` in this repository | Agent-facing, beside the skills: build and test commands, the `MODEL=`/`SIMULATION=` invariant, never-parallel suites, the node-class envelopes, the `docs/dev/` reference rule |
| Instruments | Ordinary repository tooling: model-local under `models/<model>/tools/`, cross-package under `scripts/` | They are product tooling in their own right, like the existing comparators |
| Campaign directory | Outside both repositories | The long-term brief's isolation requirement |

---

## What Is Missing and What Needs Adjusting

| Where | Change |
|---|---|
| **ai-agent-coder: new** | `campaign-manager` skill and `cm.py`. |
| | `plan-review` skill: adversarial review of a plan against the charter, the dossier and the repository, before it is frozen. Today a plan defect is caught only when PM stops on it mid-run. |
| | `recon` skill and a dossier schema: inventories, mapping, gaps, dependencies, all with `file:line` evidence. |
| | `diagnose` skill: localising a validation failure to a property, object, configuration or commit, with hypotheses ranked by evidence. |
| | A campaign ledger extending PM's model ledger with campaign-level measures. |
| **ai-agent-coder: PM** | **Launch without a human present.** The Mode B launcher's "confirm with me once" and "standing authorisation for Developers to commit" would come from a delegation file the CM writes; the PM seat exits when its run ends. |
| | **A machine-readable outcome**, for example `status --json` or a final `outcome.json`: per-slice decisions, stop causes, and plan defects with recommended amendments (today these are prose in the run report and the PM's closing message). |
| | **A completion signal** the CM's supervisor can wait on. |
| | **No change** to `pm.py approve`: it stays human-only, and the CM routes PM approval gates to its question queue. |
| **ai-agent-coder: `implementation-plan`** | A "plan from node spec" mode: self-contained plans, instrument commands in the validation plan, and a library of plan templates per domain (for example "port a module"). |
| **ai-agent-coder: VISION** | A deliberate revision adding Rung 3. The non-goal "not a planner-free autopilot" becomes: plans are written by an accountable planner seat under a human-approved charter and reviewed independently; humans keep the charter and the reserved plan classes. |
| **mimic: domain pack** | The domain profile. Instrument contracts for the existing gates (`make` targets, `check-generated`, `validate-modules`, the test tiers). The exercise's own instruments are built by the campaign itself as its first `instrument` nodes, which tests that mechanism too. |
| **mimic: docs/dev** | The long-term brief's re-review, using this document as its starting point. |
| **Environment** | A machine (the Mac, preferably inside a container or VM, as PM's README advises for unattended runs) with tmux, the harness CLIs, a notification channel and a spend ceiling. Anything the exercise's reference build needs is for the CM to discover and ask about: installing software is a must-ask question. |

---

## The Exercise: sage16 to sage26

### Why it is the right first exercise

- **A reference implementation exists.** Mimic's output can be compared with SAGE26's on the same trees, galaxy by galaxy, which removes most of the long-term brief's open science-gate problems for this exercise.
- **It is an evolution, not a blank sheet.** SAGE26 descends from the SAGE that `sage16` already reproduces, so the exercise tests whether the CM recognises what to reuse and what to build.
- **The parameters are already calibrated**, so calibration is out of scope.
- **It is real work the owner wants**, so its output is not throwaway.

### The brief the CM would receive

A draft for the owner to finalise. It must contain only what a human would naturally write:

> Import SAGE26 (`sage-home/SAGE26` at `11b1ae8`) into Mimic as a new model package, `models/sage26`. SAGE26 is a substantial evolution of the SAGE model that Mimic already carries as `sage16`: much may be reusable, some will be new. Done means the package reproduces SAGE26's own output for SAGE26's default configuration on mini-Millennium, to the standard `sage16` met against SAGE, under both Mimic drivers, with Mimic's gates green and the package documented and plotted like `sage16`. Constraints: `sage16` output must not change; core changes need my approval; no changes to SAGE26 or to any remote without my approval. Budget and seats: to be set in the charter.

### Hold-out discipline

The prototype is only tested if it finds the exercise's obstacles itself. So:

- **This document names none of them.** The owner holds an answer key, written from a hand analysis on 2026-10-20, **outside every repository** the CM can read. It must never be committed here, in ai-agent-coder or in SAGE26.
- **The charter excludes the builder's own design documents** (this file and its successors) from the CM's read scope for the test runs, and the run transcripts are checked for reads of excluded paths.
- **Do not pre-solve campaign content.** Gaps, residuals and upstream issues the exercise would hit are left for the CM to find, ask about or plan, even where fixing one early would be easy. Each pre-solved item removes test signal.

---

## Building and Testing the Prototype

### Build order

1. **Plan the CM with the tools it will use.** Write an implementation plan for `cm.py` and the CM skill with `implementation-plan`, and run it under PM in ai-agent-coder.
2. **Test the state machine offline.** Develop `cm.py` against a stub PM (scripted outcomes: accept, stop for a plan defect, stop for the environment) and stub instruments (pass, fail, self-test failure), so almost all of its behaviour is tested without spending agent time.
3. **The PM adjustments**: launch without a human present, structured outcome, completion signal.
4. **The `plan-review`, `recon` and `diagnose` skills.**
5. **The minimal mimic domain profile.**
6. **P0.**

### Stages

| Stage | Scope | What it proves | Go or no-go |
|---|---|---|---|
| **P0** | Design only: brief → intake questions → charter → dossier → graph, then stop. Repeatable, no product code | Recon recall against the answer key; whether the must-ask questions were asked and unnecessary ones were not; graph quality on the owner's review | No-go if recall of the answer key's must-find items is low or the graph needs rewriting; revise the CM and repeat |
| **P0r** | Replay: P0 on archived past campaigns (see below) | Planning quality against known human-approved decompositions | As P0 |
| **P1** | One `implement` node end to end, the first one the CM's graph schedules | The full loop: plan, review, approval, PM run, validation, merge, ratchet | No-go on any floor violation or unexplained acceptance |
| **P2** | The first `instrument` nodes and the first science gate, including at least one real or injected `diagnose` cycle | Instrument self-tests, independence, the diagnose-and-fix loop | No-go if a broken instrument is trusted or a failure is accepted |
| **P3** | The full campaign over several days | Persistence, the question protocol, budgets, the final audit and sign-off | Prototype accepted |

### The replay benchmark

`archive/` holds the records of every finished campaign on this repository: briefs, implementation plans, reviews, acceptances and handoffs (the dual driver, the converter generalisation, HOD and SHAM, chunked streaming, distribution, the Shin-Uchuu work). Each is a human-approved answer to "given this brief, what campaign?". Giving the CM a past campaign's brief, against a worktree at that campaign's starting commit, and comparing its charter, graph and first plans with what was actually approved, tests planning quality with no execution cost. HOD and SHAM is the nearest analogue to the exercise: new model packages with new tests, plots and READMEs. The corpus is machine-local and gitignored, so this is work for the owner's machine.

### Fault injection

Each must produce the right behaviour, at least once, before P3:

- a seeded plan defect → the plan is revised, not the run forced through;
- a broken instrument → its self-test catches it and it is never trusted;
- an unanswered must-ask question → only its dependants block;
- a charter edit mid-campaign → the floor stops the campaign;
- a weak Developer seat → its budget is exhausted and the node escalates;
- an `implement` plan whose surface reaches an instrument path → refused before approval.

### Measures

Human minutes per node; questions asked and the share that were necessary; PM stop causes; plan revisions per node; diagnose cycles; the trajectory of instrument verdicts; spend and wall time. All inputs are pinned, so a campaign can be replayed to compare one version of the builder with the next.

---

## Sequencing Against the Pathway

The question is whether any work still to come should be done before the prototype starts. The test applied to each item: what would it give the prototype if done first, and what does waiting for it cost?

| Pathway item | What doing it first would give the prototype | Cost of waiting for it | Verdict |
|---|---|---|---|
| **4 · Performance** (frozen plan, ten Mode B slices) | A reusable bit-identity gate against any ancestor commit (`make tests-reference-identity REFERENCE_COMMIT=<hash>`, its Slice 1), which becomes the campaign's "existing packages unchanged" ratchet for free; a measured PM run on this repository to calibrate campaign budgets; core changes landed before any campaign core node could conflict with them | None: it runs unattended under PM while the CM is built in ai-agent-coder | **Run now, in parallel.** P0 can start at any time; P1 onwards starts from step 4's merged result |
| **4.3 · Thread-per-forest** | Nothing: a mini-Millennium run takes about 3 s, and the campaign's cost is agent time | Weeks to months | Do not wait |
| **5 · Emulator** | A well-posedness instrument for novel models. Irrelevant to a reference port, which has a stronger gate | Its spike, then a research-grade model-discrepancy budget; months to its gate | **After.** The import strengthens the emulator's own case: its brief says its value depends on Mimic hosting several distinct model structures, and its gate item 4 needs two real pipelines to compare. Its spike needs no repository change and can run alongside anything |
| **6 · Coupled rate formulation** | `sage26` could be ported straight into the target formulation, avoiding a second migration | Months, plus its measurement spike | **After.** The builder turns migrating each package into a campaign, which is a good second exercise (no reference implementation, so it exercises the non-parity side of the validation layer). Recon can classify the imported prescriptions as rate, jump, algebraic or forcing as a by-product, extending [`SAGE16-PRESCRIPTION-CLASSIFICATION.md`](SAGE16-PRESCRIPTION-CLASSIFICATION.md). The comparison instrument built for the exercise also serves step 6's attribution gate |
| **7 · Model builder** | This document | — | Pulled forward as a prototype |
| **8 · Embedded engine** | Nothing | — | Unrelated |
| **Open candidates** (full Uchuu, laptop acceptance, the `halos-only`-gated routes, provenance for version 3 inputs) | Nothing | — | Independent |

**The long-term brief's preconditions** are better met now than its text says. v1.0 is tagged and the dual driver landed. The orchestration tools now exist (ai-agent-coder). The remaining one, a science-gate prototype validated on a trusted model, is exactly what the exercise builds for the reference-port class: comparison against a reference implementation, with injected-error self-tests. The novel-model class stays open, with the emulator as its candidate.

### Unplanned work worth doing first

1. **The PM adjustments.** The CM cannot drive PM unattended without them, and they help any unattended PM run. Small.
2. **The replay corpus.** Gather the archived briefs, plans and acceptances into a form the CM can be pointed at, each with its starting commit. Cheap, and it multiplies the evaluation power of P0.
3. **Cost measurement on the step 4 run.** Record agent time and spend per slice from PM's ledger and the harness, so the charter's budgets are grounded rather than guessed.
4. **The environment.** Decide the isolation (container or VM), the notification channel and the spend ceiling. These are owner decisions the CM cannot make.

### Not worth doing first

Anything that pre-solves the exercise: fixing gaps, residuals or upstream issues the import would hit, building its instruments by hand, or preparing the reference build. Each removes something the prototype is meant to be tested on.

### Recommended order

| Track | Where | Work |
|---|---|---|
| **A** | mimic, unattended | Run step 4 under PM now; record its costs |
| **B** | ai-agent-coder | Build the CM: the plan, `cm.py` against stubs, the PM adjustments, the three skills; then the mimic domain profile |
| **C** | The owner's machine | The replay corpus, the answer key kept safe, the environment decisions |
| **Then** | — | P0 and P0r; P1 once step 4 has merged; P2; P3 |

---

## Risks

- **Planning quality is the ceiling.** PM's own vision says plan quality bounds everything above it, and the CM now writes the plans. Mitigation: independent plan review, the replay benchmark, human approval of the charter, the first graph and every human-class plan.
- **Gaming the gate.** A CM optimising for passing verdicts. Mitigation: instrument independence, self-tests, the ratchet, the science auditor and the human's sign-off; thresholds that only tighten.
- **CM seat quality and cost.** The CM seat is trusted in the way the PM seat is, and a weak model there weakens the whole judgement layer. Multi-day campaigns with an hourly heartbeat need a spend ceiling grounded in measured costs.
- **Evaluation contamination.** A builder that has read the answer is not tested. Mitigation: the hold-out discipline above.
- **Scope creep in the prototype itself.** ai-agent-coder's principle of minimising the whole system applies: no mechanism for a hypothetical need, and `cm.py` stays small by reusing `pm_lib`.
- **Owner attention across three tracks.** Track A is unattended by design; track C is short; track B is where the attention goes.

---

## Decisions for the Owner

| # | Decision | Recommendation |
|---|---|---|
| 1 | Start now, or after another pathway step | Start now, on the three tracks above |
| 2 | Where the CM lives | ai-agent-coder as Rung 3, with a deliberate VISION revision; the domain pack here |
| 3 | How much plan approval is delegated in v0 | The human approves the charter, the first graph, and every `instrument` and `core` plan; `model`-class plans are approved after an independent plan review |
| 4 | Serial or parallel nodes | Serial PM runs; parallel read-only work |
| 5 | Upstream repositories | The CM may draft a patch and ask; it never applies one |
| 6 | Whether the prototype becomes a numbered pathway step | The owner's call; this document proposes it without reordering the road |
| 7 | Seats and a spend ceiling for P3 | Set after step 4's measured costs are known |

---

## Open Questions

1. **Schemas.** The charter, dossier, graph, node spec, question and outcome formats. Start minimal and let P0 show what is missing.
2. **How CM judgements are recorded.** Probably as PM's are: a reasoned assessment per decision with the evidence examined, signed into state.
3. **Tick cadence and wake-up sources.** Event-driven with an hourly heartbeat is the starting assumption.
4. **How self-contained a plan must be.** How much dossier evidence a plan copies in versus references, given that Developers read the plan and the repository but not the campaign directory.
5. **Plan templates.** Which recurring node shapes deserve a template, and who approves a template's promotion.
6. **When a graph revision needs the human.** The rule above (new human-class nodes, any change to the definition of done) may prove too loose or too tight.
7. **The science auditor's brief.** What it reads, what it may conclude, and how its findings re-enter the graph.
8. **The second exercise.** The coupled-rate migration of a package, or a model built from a paper with no reference implementation, and which instruments each would need.
