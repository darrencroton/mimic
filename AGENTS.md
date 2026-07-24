# AGENTS.md — Mimic

Guidance for AI coding assistants (Claude Code, Codex CLI, etc.) working in Mimic. Global behavioural rules (code quality, git workflow, file organisation) live in the global AGENTS.md and are not repeated here. `CLAUDE.md` symlinks to this file.

**Mimic is a physics-agnostic core with runtime-configurable physics modules.** The core owns execution, memory, I/O, metadata, and validation; physics modules own astrophysical prescriptions and are combined at runtime through YAML run files. See `docs/VISION.md` for the design principles.

Depth lives in the project-local **skill library** (`.agents/skills/mimic-*`) and the guides under `docs/`. This file is the always-loaded map: it orients you and routes you to the skill that owns each task. **Load the narrowest skill set for the task before acting**, and always use `mimic-change-control` before deciding a change is done or committing.

---

## Skill Routing

| Task | Load these skills |
|---|---|
| Build, first-run setup, venv, compiler/library detection | `mimic-build-and-env`; add `mimic-debugging-playbook` if something fails |
| Broken build, rejected run file, failing run/test/plot, memory report | `mimic-debugging-playbook`; add the relevant domain skill once the failure is localized |
| Core architecture, data flow, reader/driver boundary, ownership changes | `mimic-architecture-contract` plus `mimic-change-control` |
| Runtime module work under `models/<model>/modules/` | `mimic-modules`, `mimic-validation-and-qa`, `mimic-change-control`; add `mimic-properties` for new fields and `mimic-scientific-method` for physics claims |
| Property YAML, generated structs/schemas, units, precision | `mimic-properties`, then `mimic-validation-and-qa` and `mimic-change-control` |
| Run YAML keys, Make variables, CLI flags, unknown-key errors | `mimic-config-and-flags`; add `mimic-run-and-operate` for execution |
| Running Mimic and reading binary/HDF5 output | `mimic-run-and-operate`; add `mimic-diagnostics-and-tooling` for inspection |
| Simulation package or tree reader work | `mimic-simulations-and-readers`; add `mimic-properties` for `halo_properties.yaml` |
| Plot generation, figure/profile edits, skipped plots | `mimic-plots-and-analysis`; add `mimic-run-and-operate` if output is missing |
| Test selection, test writing, baselines, marker interpretation | `mimic-validation-and-qa`; use subagents for long unit/integration suite runs |
| Scientific output differences, parity/correctness claims, tolerances | `mimic-scientific-method`; add `mimic-sam-reference` for domain meaning |
| Historical quirks, reverted ideas, SAGE parity comments | `mimic-failure-archaeology` |
| Documentation, README, docs/dev plans, external claims | `mimic-docs-and-writing`; add `mimic-scientific-method` before measured science claims |
| Benchmarking, HDF5/binary inspection, validator exit codes, leak scans | `mimic-diagnostics-and-tooling` |

---

## Command Cheat-Sheet

The commands below cover the common path. Full flag/target/variable catalogs live in `mimic-build-and-env` and `mimic-config-and-flags`.

```bash
./scripts/first_run.sh        # Fresh-clone setup: dirs, data, mimic_venv/ (see mimic-build-and-env)

make                          # Build defaults (MODEL=sage16, SIMULATION=mini-millennium)
make info                     # Show library detection, compiler, enabled features
make generate                 # Regenerate code from property/module YAML (smart; run after YAML edits)
make -j$(sysctl -n hw.ncpu)   # Parallel build on macOS (Linux: -j$(nproc))
make clean                    # Remove build artifacts

./mimic models/sage16/input/sage16_mini-millennium.yaml   # Run (see mimic-run-and-operate for flags)

make tests                    # Full suite: validate-modules, then unit/integration/scientific
make tests summary            # Same, showing only warnings/failures/skips/outcomes
```

**MODEL/SIMULATION invariant (footgun):** Mimic builds one model set and one simulation package at a time. Use the **same** `MODEL=` and `SIMULATION=` for `generate`, `validate-modules`, tests, and `make` — mixing selectors produces silently inconsistent output. Missing packages fail loudly (`Unknown MODEL` / `Unknown SIMULATION`). Defaults are `sage16` / `mini-millennium`.

**Testing note:** Unit and integration tiers take up to ~3 min and produce large output — **delegate them to a subagent** that captures logs and returns a pass/fail summary; act on that in the main context. Scientific tests are fast (~30 s). Never run multiple test suites in parallel. Details, markers, and baselines: `mimic-validation-and-qa`.

---

## Architecture Orientation

Full architecture, data flow, and invariants: `mimic-architecture-contract` and `docs/DEVELOPER-GUIDE.md`. Consult `docs/dev/` before structural changes.

```
src/
├── core/            Execution (main, init, build_model, params); core_properties.yaml
├── io/tree/         Tree readers (binary, HDF5)   io/output/  Output writers
├── util/            Memory, error, numeric, version, I/O
├── module_system/   Framework infrastructure (do not modify); physical_constants.h, generated/, template/
└── include/         Headers; generated/ holds auto-generated property + validation code

models/<model>/      Self-contained package: input/ run YAMLs, model_properties.yaml, modules/,
                     shared/ helpers, plots/  (sage16, sham, halos-only)
simulations/<sim>/   Catalog halo_properties.yaml, tree fixtures, snapshot lists
                     (mini-millennium, millennium, {micro,mini}-uchuu[-hdf5], uchuu)
build/generated/     Build-time generated files (git_version.h, test lists, module registry)
tests/               Unit, integration, scientific tests
plot/mimic-plot/     Plotting system (registry is model-local)
```

**Load-bearing boundaries** (details in the architecture skill):
- **Property system** — properties defined in YAML (`core_properties.yaml`, `simulations/<sim>/halo_properties.yaml`, `models/<model>/model_properties.yaml`) generate C structs, init/output logic, HDF5 metadata, and run-local output schemas via `make generate`. Never hand-edit `*/generated/`.
- **Model/Simulation boundary** — one model set + one simulation package per build. Run files declare `model.name` and `simulation.name`; paths derive from those. Mixing module families means creating a new model package and reconciling properties, parameters, units, tests, and plots.
- **Reader/driver boundary** — `input.tree_type` selects the on-disk reader; `input.processing_order` selects the driver (`tree_ordered` default; `snapshot_ordered` recognized but fails fast until implemented).
- **Module system** — modules run through optional `pre_timestep`/`post_timestep` phases plus ordered user-named substep phases under `modules.phases:`, in modes `PROCESSING_MODE_FULL_HALO`, `PROCESSING_MODE_PER_EVENT`, `PROCESSING_MODE_BY_GALAXY`. Lifecycle: `init()` → `process()` → `cleanup()`. Standalone `.c` modules are fine for prototypes; convert to a directory module with `module_info.yaml` once tests/metadata/events/dependencies matter.

---

## Code Style & Committing

Full rules: `docs/STYLE-GUIDE.md` (human readability) and the formatter (mechanical layout). Change classification and the pre-commit gate: `mimic-change-control`.

- **C** — 2-space indent, LLVM base, 100-char limit (`.clang-format`). Compile clean under `-Wall -Wextra -Wshadow -Wformat-security -Wundef`; write for both macOS Clang and Linux GCC/mpicc. Never `printf` in module code — use the logging macros in `src/util/error.h`. Universal constants come from `physical_constants.h`; model-specific constants from model-local shared headers. `// SAGE parity:` marks intentional legacy behaviour — check the baseline before "fixing".
- **Python** — black + isort (line-length 100, profile black); runs under 3.9+.
- **Markdown** — do not hard-wrap prose; the 100-char guideline applies to code blocks only.
- **Never hand-edit `*/generated/`** — regenerated by `make generate`.
- Check without modifying: `make check-format` (what CI runs).

**Pre-commit checklist** (owned by `mimic-change-control` — load it for the full gate): 1) run `./scripts/beautify.sh`; 2) re-read the diff against `docs/STYLE-GUIDE.md`; 3) for changes touching modules/tests/properties/simulations/plots/architecture, sweep the relevant `mimic-*` skills for staleness. Report the outcome of all three.

---

## Documentation Map

- `docs/VISION.md` — architectural principles and design boundaries
- `docs/USER-GUIDE.md` — installation, run configuration, output, plotting, troubleshooting
- `docs/DEVELOPER-GUIDE.md` — architecture, modules, simulations, properties, tests, generated metadata
- `docs/STYLE-GUIDE.md` — naming, comments, metadata, tests, review conventions
- `docs/dev/SNAPSHOT-HDF5-FORMAT.md` — frozen snapshot-ordered HDF5 input contract (`format_version` ratchet)
- `docs/dev/` — architecture planning documents; read before structural changes
- `plot/mimic-plot/README.md` — plotting manual · `tests/README.md` — test-suite quick reference
- `models/<model>/README.md` — package science scope, pipeline, parameters, plots, references
- `simulations/<sim>/README.md` — data, units, snapshot lists, maintenance notes
