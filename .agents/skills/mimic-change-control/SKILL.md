---
name: mimic-change-control
description: How changes to Mimic are classified, gated, and reviewed before commit. Load this skill before committing anything, when deciding which validation a change needs (generate, check-generated, validate-modules, lint-parameters, test tiers, check-format, check-docs), when touching generated code or baselines, when renaming or refactoring across packages, when asked "is this change done?", or when planning a structural change. Owns the project non-negotiables and the pre-commit checklist.
---

# Mimic Change Control

This skill defines what "done" means for a change in the Mimic repository: how to classify a change, which validation gates each class must pass, the non-negotiable rules (with the incident behind each), and the pre-commit checklist. It is the last skill you consult before a commit and the first one you consult when planning one.

## When to use / when NOT to use

Use this skill when you are about to modify the repo, deciding what validation a finished change needs, preparing a commit, or planning a rename/refactor/structural change.

Do NOT use it for:
- Test mechanics (tiers, markers, registration, tolerances, baseline internals) — see the `mimic-validation-and-qa` skill.
- The evidence method for scientific claims (parity recipes, tolerance design, hypothesis testing) — see the `mimic-scientific-method` skill.
- Diagnosing a failure you hit while validating — see the `mimic-debugging-playbook` skill.
- Writing the actual module/property/plot/simulation change — see `mimic-modules`, `mimic-properties`, `mimic-plots-and-analysis`, `mimic-simulations-and-readers`.
- Doc style and docs/dev lifecycle details — see the `mimic-docs-and-writing` skill.

## First actions

Before editing anything:

1. Classify the change using the table below. A change usually touches more than one class; apply the union of gates.
2. Note the MODEL/SIMULATION pair you will use for the entire change. Defaults are `sage16` + `mini-millennium` (from `DEFAULT_MODEL` / `DEFAULT_SIMULATION` in the `Makefile`), so plain `make` is valid; if you override, use the identical pair on every command.
3. Confirm you are on the intended branch (`git branch --show-current`) — never auto-create a branch, never commit without asking the user.
4. If the change is structural (new driver, new package type, cross-package moves), check `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` first — an active plan may already own the design.
5. If any file you intend to edit lives under a `generated/` directory, stop: edit the metadata or the generator instead (non-negotiable A below).

## 1. Change classification and required gates

Classify by what the change touches. Run gates in the order listed; each column's commands are copy-pasteable with default selectors (add `MODEL=<m> SIMULATION=<s>` uniformly for non-defaults).

| Class | Typical files | Required gates, in order |
|---|---|---|
| Property/parameter metadata | `src/core/core_properties.yaml`, `simulations/<s>/halo_properties.yaml`, `models/<m>/model_properties.yaml`, `models/<m>/parameter_units.yaml` | `make generate` → `make check-generated` → `make` → `make tests-integration` + `make tests-scientific` → format/style/docs sweep |
| Module metadata | `models/<m>/modules/<mod>/module_info.yaml` | `make validate-modules` → `make lint-parameters` → `make generate` → `make check-generated` → `make` → module's own tests + affected tiers |
| Module physics (C) | `models/<m>/modules/**/*.c`, `models/<m>/shared/` | build clean (`make clean && make`) → `make validate-modules` → `make lint-parameters` → all three tiers (unit, integration, scientific) → physics baseline verdict (see non-negotiable F) |
| Core/framework | `src/core/`, `src/io/`, `src/util/`, `src/module_system/` (never `*/generated/`) | `make clean && make` → `make check-generated` → all three tiers → baseline comparisons must pass or be deliberately regenerated with justification |
| Simulation package | `simulations/<s>/` (halo_properties.yaml, simulation_info.yaml, a_list, fixtures) | `make MODEL=<m> SIMULATION=<s> generate` → `make MODEL=<m> SIMULATION=<s> check-generated` → `make MODEL=<m> SIMULATION=<s>` → `make MODEL=<m> SIMULATION=<s> tests` (same selector everywhere) |
| Plotting | `plot/mimic-plot/`, `models/<m>/plots/` | plot tests under `plot/mimic-plot/tests/` → run `mimic-plot` on real output → see the `mimic-plots-and-analysis` skill |
| Docs only | `*.md`, `docs/` | `make check-docs` → no hard-wrapped prose → `make check-format` (harmless, CI runs it anyway) |
| Generated output | any `*/generated/*`, `tests/generated/` | never edited directly — regenerate via `make generate` and commit alongside the metadata change that produced it |
| Baselines | `tests/data/output/baseline/`, `models/sage16/modules/_tests/baseline/` | only via documented regeneration procedures (non-negotiable F), never hand-edited, justification in the commit message |

Every class additionally passes through the pre-commit checklist (section 3). Long-running tiers (unit and integration, up to ~3 min each with large output) should be delegated to a subagent that captures a log and returns a pass/fail summary; scientific is fast (~30 s). Capture pattern:

```bash
mkdir -p archive/test-logs   # archive/ is a gitignored local dir; create it if absent
make tests-unit > archive/test-logs/tests-unit.log 2>&1; rc=$?
echo "exit_code=$rc"
```

Treat any non-zero exit code as failure regardless of what the log text looks like. `make tests` runs everything (clean + build + check-docs + validate-modules + all tiers); append the `summary` goal to any test target to filter output down to `MIMIC_RESULT:` FAIL/SKIP/WARN/ERROR lines.

## 2. Non-negotiables

Each rule below exists because of a real incident or a deliberate architectural decision. Do not relitigate them; if you believe one is wrong, raise it with the project owner instead of quietly violating it.

### A. Never hand-edit files under `*/generated/`

Generated C, headers, and schemas are outputs of `make generate`, driven by YAML metadata (metadata-first architecture — the YAML is the single source of truth, the C is a projection). Hand edits are overwritten on the next regeneration and, until then, put code and metadata silently out of sync. `make check-generated` catches drift deterministically: every generated file embeds a `Source MD5:` header hash of its source metadata, and `scripts/check_generated.py` recomputes and compares it. STYLE-GUIDE: "Generated files are outputs, not editing targets." Fix path: edit the metadata (or the generator in `scripts/`), run `make generate`, commit both together.

### B. Same MODEL/SIMULATION selectors across generate, validate, tests, build, and run

Mimic compiles exactly one model package against one simulation package. Generation, module validation, test-input generation, the build, and the test harness all key off the same two selectors; mixing them (e.g. `make MODEL=sham generate` then plain `make`) produces silently inconsistent generated code and binaries. Two guards exist because this bit people: the Makefile fails loudly on an unknown package (`Unknown MODEL`/`Unknown SIMULATION`) and on lowercase `model=`/`simulation=` typos, and at runtime `src/core/read_parameter_file.c` refuses to run when the run YAML's `model.name` does not match the compile-time `MIMIC_COMPILED_MODEL`. Baseline-comparison tests skip (with a stated reason) when the selected pair differs from the committed-baseline pair — a SKIP there means you tested less than you think, not that you passed.

### C. Failing tests are real problems — never weaken them to pass

STYLE-GUIDE, verbatim: "Do not weaken errors into warnings merely to get a test or run to pass" and "Never simplify failing tests to make them pass." Incident basis: the precision-policy history showed that lenient float comparisons had masked a real inheritance comparison bug for some time (see the `mimic-failure-archaeology` skill for the full account), and commit `6cbeafe4` found a silently re-narrowed local float in `sage_reincorporation.c` only because the FULL suite ran with strict tolerances. A weakened test is worse than no test: it certifies broken behaviour. Legitimate escape hatches are `TEST_SKIP_WITH("reason")` (C) / `TestSkipped` (Python) for genuinely unavailable configurations — never for inconvenient failures.

### D. Respect package ownership boundaries

STYLE-GUIDE "Repository Boundaries": `src/core/` owns execution and dispatch; `src/io/` owns readers/writers; `src/module_system/` owns framework helpers and fixtures; `models/<model>/` owns model physics, parameters, model tests, and plot figures; `simulations/<simulation>/` owns catalog properties, metadata, and fixtures; `plot/mimic-plot/` owns the plotting engine; `tests/` owns cross-package tests. No model-specific physics in the core; no simulation package depending on a model package. Mixing modules from different model families means creating a new model package with reconciled properties, parameters, units, tests, and plots — not runtime mixing. This boundary is what keeps the framework physics-agnostic (docs/VISION.md); every violation is a future extraction job.

### E. Numbers before claims

Never claim parity, correctness, or improvement from eyeballing plots or "it looks right". Every scientific claim needs a measured comparison — per-galaxy/per-property where relevant — with stated tolerances, before the claim appears in a commit message, doc, or report. This is the project's evidence bar, confirmed directly by the project owner (2026-07-03). The `mimic-scientific-method` skill owns the method (comparison recipes, tolerance design, refutation); change control's part is the gate: a commit whose message makes a scientific claim without a measured number behind it is not done.

### F. Baselines are recorded run outputs — regenerate via documented procedures only

Baselines are byte-committed outputs of a known-good run, so hand-editing one is fabricating evidence. Two baselines, two procedures:
- Physics-free core baseline (`tests/data/output/baseline/`): regenerate the HDF5 side with `./scripts/regenerate_baseline.sh`, which validates the run is physics-free before copying. The script's own header says it best: "Never regenerate to fix a failing test — investigate the failure first!"
- sage16 full-physics baseline (`models/sage16/modules/_tests/baseline/physics-binary/`): the exact refresh procedure lives in the docstring of `models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py` — run the test's own YAML, copy the output and its `output_schema.json`, and document why in the commit message.

Regeneration is legitimate only after a deliberate, validated change (e.g. commit `a646e7b4` regenerated all baselines after the parity campaign changed every baryonic property, and said so). Baseline regeneration and the code change that necessitates it belong in the same commit with the justification spelled out.

### G. sage16 preserves SAGE quirks deliberately — corrected physics means a new model package

The 2026-06-11 parity campaign brought sage16 to near-bit-parity with original SAGE (≥98% bit-identical per property), and that parity baseline is the package's validation anchor. Dozens of `SAGE parity:` comments, concentrated in `models/sage16/`, mark code that intentionally mirrors legacy SAGE behaviour, including behaviour you may recognise as physically wrong. Do NOT "fix" them in sage16 — any fix silently invalidates the parity baseline. Per `models/sage16/README.md`: create a new model family by copying the needed modules into a new `models/<model>/` package and reconciling properties, units, parameters, tests, and plots there. If a change to sage16 is genuinely intended to diverge from SAGE, it must be deliberate, measured (non-negotiable E), and accompanied by a physics-baseline refresh (non-negotiable F).

### H. Git discipline (from AGENTS.md — restated, not replaced)

- Run `./scripts/beautify.sh` before every commit (full formatter, both languages, regardless of what you touched — CI checks both).
- Never use `--no-verify` or bypass hooks.
- Always ask the user before committing; never commit without explicit approval.
- Never amend a previous commit — always create a new one.
- Commit to the current branch; do not auto-create branches unless told to.
- Never delete files — archive to `archive/` in the project root (gitignored; create it if needed).
- Commit messages must be meaningful and list every changed file with reasons, grouped logically.

## 3. Pre-commit checklist (from AGENTS.md, in substance)

Complete all three steps for every commit and report the outcome:

1. **Format** — run `./scripts/beautify.sh` (clang-format for C, black + isort for Python, both at 100 columns). Run it in full even for single-language changes; `make check-format` in CI covers both.
2. **Style sweep** — re-read your diff against `docs/STYLE-GUIDE.md`. Fix sub-par local style in the files you touched even where it predates your change, but do not expand into whole-repo cleanup. State that the sweep was done.
3. **Skill sweep** — if the change touched modules, tests, properties, simulations, plots, or core architecture, review the relevant `.agents/skills/mimic-*` skill files and update anything now stale or missing. State that the sweep was done, or flag a skill needing a larger update.

## 4. The rename/refactor trap

History: the `models/sage → models/sage16` rename (commit `3c40e2b5`) left stale references all over the repo. Commit `332153c7` later fixed the docs fallout — ~62 stale `models/sage/` references across guides, READMEs, and scripts, including a broken run-file path in `first_run.sh` that made fresh-clone setup fail. Worse, commit `a646e7b4` discovered that the sage16 physics baseline test had been **silently skipping since the rename** because its model-name guard and paths still said "sage", and test-harness model defaults were hardcoded to "sage" too. The fix derived harness defaults from the Makefile's `DEFAULT_MODEL` (see `tests/framework/harness.py` → `scripts/discovery.py:makefile_default`) so there is now one source of truth.

Rule — after ANY rename or path move:

```bash
rg -n '<old-name>' --hidden -g '!.git' .          # docs, scripts, YAML, harness defaults, CI
make tests summary                                 # then read every SKIP line and its reason
```

A test that skips because a guard still references the old name reports green while testing nothing. Grep is not optional, and neither is reading the SKIP reasons.

## 5. Structural changes: the docs/dev plan convention

`docs/dev/` holds active architecture plans only; completed plans are archived to the gitignored `archive/` (so they may not exist on other machines — durable knowledge must migrate to the permanent docs and skills before archiving). `docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md` is the index: what is active, in what order, and which document owns which details. Before any structural change (new drivers, new package types, cross-package data-flow changes), read the pathway index — do not duplicate or contradict an active plan, and do not treat plan contents as settled instruction (plans evolve; the index tells you their current status). `docs/dev/` documents are ephemeral working material: never cross-reference them from permanent files (docs of record, code comments, skills) as if they were stable.

## 6. What CI runs (`.github/workflows/ci.yml`)

Single job on `ubuntu-latest`, triggered on push/PR to `main` and `develop`, with default selectors. Steps in order — this is exactly what gates you remotely:

1. `make check-format`
2. `make clean && make`
3. `make check-generated`
4. `make check-docs`
5. `make validate-modules`
6. `make tests-unit`
7. `make tests-integration`
8. `make tests-scientific` with `MIMIC_BASELINE_RTOL=1e-3` — the physics baseline was generated on macOS and Linux libm/compiler reproduces it only to ~7e-4 relative, so CI relaxes the gate above that measured noise floor; the strict 1e-6 default still applies locally, and diffs in (1e-6, 1e-3] surface as warnings.

Notes: `make` runs `lint-parameters` automatically as pre-build validation, so CI's build step covers it. Memory-leak gating happens inside the test suites themselves (C unit tests call `check_memory_leaks()` in-process; integration tests assert against each run's captured output). If local validation followed section 1, CI holds no surprises except the Linux-vs-macOS tolerance band.

## Provenance and maintenance

Verified against the live repo on 2026-07-03. Re-verify before relying on volatile specifics:

```bash
grep -n 'DEFAULT_MODEL\|DEFAULT_SIMULATION' Makefile             # current default selectors
grep -n 'name:' .github/workflows/ci.yml | head -20              # CI step list still as documented
rg -n 'SAGE parity' models/sage16/ src/core/ | wc -l             # parity-comment count
rg -n 'weaken errors into warnings|Never simplify failing' docs/STYLE-GUIDE.md
rg -n 'Repository Boundaries' docs/STYLE-GUIDE.md
git log --oneline -n1 3c40e2b5 a646e7b4 332153c7 6cbeafe4        # incident commits still resolvable
sed -n '1,30p' scripts/regenerate_baseline.sh                    # HDF5 baseline procedure unchanged
sed -n '1,30p' models/sage16/modules/_tests/test_scientific_sage_physics_baseline.py  # physics refresh
ls docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md                          # plan index still present
make help | grep -E 'generate|check-|validate|lint|tests'         # target names still current
```

The Makefile target set, CI step order, tolerance values (1e-6 local / 1e-3 CI), and default selectors are the most drift-prone facts here; everything in section 2 is historical or doctrinal and durable.
