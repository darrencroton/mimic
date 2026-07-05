---
name: mimic-failure-archaeology
description: The chronicle of Mimic's settled battles — root causes, reverts, dead ends, and precision-policy history — so nobody re-fights them. Load this skill before re-investigating any past decision or "suspicious" old behaviour, when you see a SAGE parity comment or a one-galaxy z=0 count difference, when tempted to widen float fields or "fix" legacy quirks, when hunting why a doc/plan file disappeared from docs/dev, when evaluating an old branch (feature/positive-agn-feedback), or when a task says "has this been tried before?", "why is it like this?", "restore/revert", or "history/archaeology".
---

# Mimic Failure Archaeology

This skill is the project's institutional memory: the settled investigations, their root causes, the evidence that closed them, and their current status. Read it before opening (or re-opening) any investigation that smells like it happened before. All git commands here are read-only — never mutate history while doing archaeology.

## When to use / when NOT to use

Use this skill to check whether a question has already been answered, to find the commit or archived document that answered it, and to cite the precedent instead of re-running the investigation.

Do NOT use it for:
- Diagnosing a live failure with no history angle — see the `mimic-debugging-playbook` skill.
- The measurement method itself (parity recipes, tolerances, conservation checks) — see the `mimic-scientific-method` skill.
- Precision policy as a forward-looking rule for new properties — see the `mimic-properties` skill (this skill holds the history behind the rule).
- Gates and pre-commit process — see the `mimic-change-control` skill.
- Architecture rationale that is still load-bearing design (not a closed incident) — see the `mimic-architecture-contract` skill.

## First actions

1. Search this skill and `references/chronicle.md` for your symptom keyword (property name, module name, "parity", "float", "flyby", ...). If it appears, read the incident before touching code.
2. Search git history before re-investigating (recipes below). The repo has ~1,100+ commits with descriptive messages; most questions are answered in a message or a deleted `docs/dev/` plan.
3. Check `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` — the live index of current plans — to see whether the topic is an active plan rather than a closed battle.
4. Only after (1)–(3) come up empty, open a fresh investigation under the rules in the `mimic-scientific-method` skill.

## Searching history before re-investigating

`docs/dev/` holds active plans only; completed plans are deleted from git and moved to `archive/`, which is a gitignored machine-local symlink that may not exist on your machine. Durable knowledge therefore lives in this skill; git history is how you recover the full documents.

```bash
# Find commits whose message mentions a topic (case-insensitive)
git log --oneline -i --grep='reincorporation'

# Find commits that added/removed a string anywhere in the code (pickaxe)
git log --oneline -S 'MaxDynamicSubsteps' -- src/

# Find when a now-deleted file existed, and every commit that touched it
git log --oneline --follow -- docs/dev/DYNAMIC-TIMESTEP-CONVERGENCE-NOTES.md

# Read a deleted file as it existed just before its deleting commit
git show 9a6f4322^:docs/dev/DYNAMIC-TIMESTEP-CONVERGENCE-NOTES.md

# Read a specific commit's full message and diff
git show 6cbeafe4

# List files a commit deleted (to find which ^:path to recover)
git show --stat --diff-filter=D 9a6f4322
```

Pattern: a commit that "archives" or "removes" planning docs (e.g. `9a6f4322`, `c568e8bb`) is your pointer — `git show <hash>^:<path>` recovers the document even though `archive/` is not in git.

## The ten most valuable settled battles (brief)

Full detail, evidence, and status for every incident is in `references/chronicle.md`. Do not re-open any of these without new numbers that contradict the recorded evidence.

| # | Battle | One-line verdict |
|---|---|---|
| 1 | sage16 parity campaign (2026-06-11) | Six root causes found (dominant: SAGE's literal `1.414` for √2); result ≥98% bit-identical per property; a one-galaxy z=0 count difference vs original SAGE is EXPECTED, not a bug |
| 2 | Precision policy (`bf0993fa`) | Core/simulation properties are double; float rounding masked a real bug (orphan Rvir/Vvir could freeze); sage16 keeps floats ONLY for parity |
| 3 | Stale float locals (`6cbeafe4`) | Widening a struct field is incomplete until every local `float` copy is chased; found only by the FULL test suite |
| 4 | ctrees catalog precision (`4a97d3d0`) | Investigated and settled: DON'T widen — ASCII source has ~7 sig figs; `ctrees_compat.h` is deliberately float. Cite this before reopening |
| 5 | Docs overhaul revert (`8d0f39c6` → redo `432e4ca7`) | Framework-first framing is doctrine; models are interchangeable packages, not the product |
| 6 | Rename staleness (sage→sage16, `3c40e2b5` et al.) | Renames must grep docs, scripts, harness defaults, AND model-name guards in tests — the physics baseline test silently skipped for a while |
| 7 | Dynamic timestep campaign (shipped 2026-07-01) | Closed and shipped (`469b7adc`, `b942bf3c`, `58f1d3c2`); MaxDynamicSubsteps 50→200 came from measurement; notes recoverable via `git show 9a6f4322^:...` |
| 8 | Stripping metal conservation | Documented as a known issue (`25f54878`), then fixed (`3f1e124b`) — check status before "rediscovering" it |
| 9 | fix_flybys z=0 divergence (`b727fd36`) | ASCII ctrees reader collapses flyby FoF groups at z=0; other readers don't; documented and ACCEPTED, not a bug to fix |
| 10 | Deliberate retirements ≠ failures | Split-pass merger path (`53fb1904`), per-task partition (`8be9309f`), parameter registry (`74c56f4a`) were removed on purpose — do not "restore" them |

Also settled, from the upstream SAGE era: an abandoned Len-based infall recipe (`3a2d1f30`), a reverted infall recipe (`b0088922`), and an ejected-gas wind-back experiment (`10ac96eb`). These are prior art — check them before proposing similar physics changes.

## Deliberate-quirk markers: `// SAGE parity:` comments

sage16 contains dozens of `// SAGE parity:` comments marking code that intentionally mirrors legacy SAGE behaviour, including its bugs. Never "fix" one in sage16 — fork a new model package for corrected physics (see the `mimic-modules` skill). Count and browse them:

```bash
rg -n 'SAGE parity' models/sage16/ | wc -l
rg -n 'SAGE parity' models/sage16/modules/ src/core/
```

A handful more live in `src/core/build_model.c` (core-side parity behaviour) and the convention itself is defined in `docs/STYLE-GUIDE.md` and `AGENTS.md`.

Representative quirks: the literal `1.414` where √2 belongs, asymmetric gas/metal stripping, the frozen Type-1 disk radius, and the speed of light used in cm/s in a cgs-mixed expression.

## Stalled branches and marker branches

- `feature/positive-agn-feedback` — STALLED. One commit ahead, roughly 246 behind `main`, and it predates the model/simulation package split, so its paths no longer exist. Never merge it; if the idea is wanted, re-port the physics into a module under the current layout. Verify staleness: `git rev-list --left-right --count main...feature/positive-agn-feedback`.
- `pre-chunk-baseline` / `post-chunk-baseline` — ancestor markers pinning the states before/after the chunked-output change, kept for comparison. Not development branches.

## Provenance and maintenance

Status snapshot in this skill and the chronicle dated 2026-07-03; `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` is the live index of current plans — trust it over any plan status stated here. Re-verify before relying:

- Commit hashes and messages: `git show -s --format='%h %ad %s' --date=short <hash>`
- Tags/era boundaries: `git tag` (as of 2026-07-03: `v0.1-beta`, `v0.5`, `v0.9-pre-release`, `v1.0`)
- Branch staleness: `git rev-list --left-right --count main...feature/positive-agn-feedback`
- Parity-quirk marker count: `rg -n 'SAGE parity' models/sage16/ | wc -l`
- Recoverability of archived plans: `git show 9a6f4322^:docs/dev/DYNAMIC-TIMESTEP-CONVERGENCE-NOTES.md | head`
- `archive/` (gitignored, machine-local symlink) may hold full reports (e.g. `archive/SAGE16-PARITY-REPORT.md`) — treat it as corroboration; the durable record is here and in git history.
