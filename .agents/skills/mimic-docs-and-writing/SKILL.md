---
name: mimic-docs-and-writing
description: Maintaining Mimic's documentation of record and house writing style. Load when a task involves editing README.md, docs/VISION.md, docs/USER-GUIDE.md, docs/DEVELOPER-GUIDE.md, docs/STYLE-GUIDE.md, tests/README.md, plot/mimic-plot/README.md, any model/simulation/module README, files under docs/dev/ (plans), make check-docs failures or broken links/anchors, Markdown formatting rules (hard-wrapping, line length), writing commit-message or report prose about Mimic, or deciding what may be claimed about Mimic externally (papers, release notes) versus what must stay labeled open/candidate.
---

# Mimic Docs and Writing

Mimic's documentation is a designed system with strict role separation, a narrative doctrine that survived a same-day revert, and a validator (`make check-docs`). This skill is how to write inside that system without degrading it.

## When to use / when NOT to use

Use for: editing any doc of record, README conventions, docs/dev lifecycle, Markdown rules, link validation, external claims.

Do NOT use for:
- Skill files' own maintenance duty — the pre-commit "skill sweep" lives in `mimic-change-control`.
- The evidence behind a scientific claim — see the `mimic-scientific-method` skill (write only what was measured).
- Code comments and C/Python style — `docs/STYLE-GUIDE.md` directly (comments explain *why*, one line; `SAGE parity:` markers).

## First actions

1. Identify the owning document before writing a word (table below) — content in the wrong document is the main failure mode.
2. Read the surrounding sections and match their voice; each document has one audience.
3. After ANY doc edit: `make check-docs` (validates every internal link and anchor repo-wide, and rejects leftover `PONDER` review markers; skips `.git`, `.claude`, `archive`, `build`, `mimic_venv`, `sage-code`, test outputs).

## 1. The documents of record and their roles

| Document | Role | Audience question it answers |
|---|---|---|
| `README.md` | Problem-first front door; shortest path to a first result | "Should I use Mimic?" |
| `docs/VISION.md` | Architectural principles and boundaries; changes only when implemented behavior justifies it | "Why is it designed this way?" |
| `docs/USER-GUIDE.md` | Workflow-oriented: generate/configure/analyse catalogues; troubleshooting | "How do I use it successfully?" |
| `docs/DEVELOPER-GUIDE.md` | Extension workflows, APIs, metadata, testing; the Reference section is the one place for reference-manual prose | "How do I modify it?" |
| `docs/STYLE-GUIDE.md` | Naming, comments, metadata style, test style, review conventions | "What should contributions look like?" |
| `tests/README.md`, `plot/mimic-plot/README.md` | Quick references for their subsystems, deferring depth to the guides | — |
| `models/<m>/README.md` | That package's science scope, pipeline, parameters, plots, references, citations | — |
| `simulations/<s>/README.md` | Data provenance, units, snapshot lists, fixtures, maintenance obligations | — |
| `models/<m>/modules/<mod>/README.md` | Short local contract: what/mode/pipeline position/properties/parameters/events/notes/references (the ~29-line `sage_resolve_mergers_and_disruption` README is the house model) | — |

Keep content in its owner and cross-link generously rather than repeating. When code, metadata, and docs disagree: fix the source of truth first — metadata or code, then generated artifacts, then documentation.

## 2. The narrative doctrine (learned the hard way)

**Mimic the framework is the focus — never one model.** A full documentation overhaul was reverted the same day it landed (2026-06-11) because it leaned model-centric; the accepted redo presents model packages as interchangeable and self-documenting. Operational rules:

- sage16 is "the current build default and worked example" — no pedestal phrases ("the main package", "the package to use for science"). Model prominence IS appropriate inside that model's own README.
- Write model references pattern-first: `models/<model>/README.md` is each package's canonical doc; "see `models/` for what's shipped". New packages must never require guide rewrites.
- Modularity means models AND simulations: wherever docs show `make MODEL=<name>`, include `SIMULATION=<name>`; running one model across different boxes/resolutions is a first-class workflow, not a footnote.
- Citation guidance defers to each package's README (the guides never hardcode one model's papers as "the" citation).

## 3. Mechanical rules

- **Never hard-wrap Markdown prose.** Full paragraphs are single long lines; renderers soft-wrap. Applies to ALL `.md` files including skills. (Manual wrapping renders poorly and makes diffs unreadable.)
- Code blocks and YAML/shell examples inside Markdown follow the 100-character guideline.
- Start major documents with a one-line purpose statement; end guides with the shared "Documentation Directory" section (copy an existing one).
- Do not duplicate generated or exhaustive structural lists (property tables, module inventories) in prose — link to the metadata or generated output instead; small, stable, deliberately illustrative excerpts are fine (STYLE-GUIDE rule).
- Anchors: `make check-docs` verifies `#section-anchors` against actual headings — renaming a heading breaks inbound links repo-wide; grep before renaming.

## 4. The docs/dev lifecycle

`docs/dev/` holds ACTIVE architecture plans only, indexed by `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` (what is active, in what order, which document owns which details). Completed plans move to the gitignored, machine-local `archive/` — recover deleted ones with `git show <hash>^:<path>` (recipes in `mimic-failure-archaeology`). Rules: docs/dev/ material is ephemeral working matter — **never cross-reference it from permanent files** (guides, READMEs, code comments, skills) as if stable; when a plan lands, its durable instructions migrate into the guides/READMEs/skills before the plan is archived; plans own implementation scope and acceptance criteria while VISION owns principles.

## 5. External claims (papers, release notes, README statements)

The evidence bar applies to prose: every claim states what was measured, and unmeasured aspirations stay labeled open/candidate (`mimic-scientific-method` owns the bar). What the repo's record currently supports:

- **Physics-agnostic core with runtime-configurable modules** — supported: the core names no physics module; the empty pipeline and `halos-only` package run; infrastructure tests use the model-neutral fixture.
- **sage16 reproduces published SAGE** — supported, with the precise phrasing: near-bit-parity against Croton et al. (2016) SAGE on mini-Millennium, ≥98% of matched galaxies bit-identical per property at z=0, residuals at float-ULP level plus ~0.1% chaotic threshold flips (chronicle: `mimic-failure-archaeology`). Do not round this up to "identical".
- **Model and simulation interchangeability** — supported to the extent shipped: three model packages and seven simulation packages run through one framework; cross-format consistency is validated on the micro-Uchuu triplet with one documented, explained divergence (fix_flybys at the final snapshot).
- **Reproducible output provenance** — supported: every run self-records pipeline, parameters, event contracts, versions, and schema (HDF5 `RunProperties`; run-local `metadata/`).
- Anything about snapshot-ordered processing, distributed operation, embedded engines, or assisted model building is **planned/open** (status lives in `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md`) — label it so.

When writing citations, follow each package README's list (sage16: Croton et al. 2016, ApJS 222, 22 and Croton et al. 2006).

## Provenance and maintenance

Verified against the live repo 2026-07-04 (doc roles cross-checked against the documents themselves; narrative doctrine against the recorded revert/redo history; validator behavior against `scripts/check_docs.py`). Re-verify:

```bash
make check-docs                                            # validator alive and repo clean
grep -n "PONDER\|SKIP_DIRS" scripts/check_docs.py | head -5
ls docs/ docs/dev/                                          # document set + active plans
head -5 docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md               # pathway index still the entry point
wc -l models/sage16/modules/sage_resolve_mergers_and_disruption/README.md   # house-model README
git log --oneline -n1 8d0f39c6 432e4ca7                     # the revert/redo doctrine anchors
```

Document roles and the narrative doctrine are owner-set and durable; the external-claims list must be re-derived from the repo's evidence whenever capabilities land (a claim is only as current as its measurement).
