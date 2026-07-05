---
name: mimic-modules
description: Creating, modifying, validating, testing, and documenting Mimic runtime physics modules. Load when a task involves model package module files, a module_info.yaml, module lifecycle functions such as name_init/process/cleanup, processing modes (process_full_halo, process_per_event, process_by_galaxy), module phases (pre_timestep, modules.phases, post_timestep), events (module_emit_event, events.emits/consumes), module parameters and LOAD_PARAM macros, transport properties between modules, converting a standalone .c prototype into a directory module, "add a new physics module", "change module X", "module not registered", or module-local tests under _tests/.
---

# Mimic Modules

Runtime physics modules are the unit of scientific extension in Mimic. The core never names them; it discovers them from `models/<MODEL>/modules/`, registers them through generated code, and dispatches them according to the run YAML. This skill is the complete workflow for writing and changing them correctly.

## When to use / when NOT to use

Use for: creating a module, modifying module C code or `module_info.yaml`, wiring events, loading parameters, module tests, pipeline ordering questions.

Do NOT use for:
- Adding or changing galaxy/halo properties (the fields modules read/write) — see the `mimic-properties` skill.
- Test-system mechanics (tiers, markers, registration internals, baselines) — see the `mimic-validation-and-qa` skill.
- What the physics *means* scientifically — see the `mimic-sam-reference` skill.
- Whether a behavior change is allowed and what gates it — see the `mimic-change-control` skill.
- Core dispatch internals and invariants — see the `mimic-architecture-contract` skill.

## First actions

Before touching any module:

1. Read the module's `README.md` and `module_info.yaml`, then its `_tests/` — they are the local contract.
2. `grep -n "SAGE parity" <module>.c` — parity-marked code is deliberately quirky; never "fix" it in sage16 (fork a new model package instead; see `mimic-failure-archaeology`).
3. Find where the module sits in the pipeline: open the shipped run YAML (`models/sage16/input/sage16_mini-millennium.yaml`) and locate it in `pre_timestep` / `phases` / `post_timestep`. Note which transport properties it reads or writes (its `dependencies.properties` plus the README).
4. `make validate-modules` before and after any metadata edit; `make lint-parameters` after any parameter change.
5. Use one MODEL/SIMULATION pair for everything. Defaults (`sage16` + `mini-millennium`) mean plain `make` is fine.

## 1. Standalone vs directory modules

**Standalone module** (prototype): a single `my_prototype.c` placed directly under `models/<model>/modules/`. The registry generator synthesizes minimal metadata from the filename (`scripts/generate_module_registry.py`, `create_standalone_module_metadata`): module name = filename stem, all three processing modes assumed supported, no dependencies/parameters/tests/docs/events. The C file must still implement the three lifecycle symbols. A directory module and a standalone module must not share a name (generation fails).

**Directory module** (production): `models/<model>/modules/<name>/` containing `<name>.c`, `module_info.yaml`, `README.md`, optional helper files, and `_tests/`.

Convert a standalone to a directory module as soon as ANY of these matter: a real mode constraint, declared property/parameter dependencies, events, tests, extra source files, `compilation_requires`, or documentation. Conversion checklist: `mkdir models/<model>/modules/<name>/`, move the `.c` in, write `module_info.yaml` (below), write `README.md`, add `_tests/`, then `make validate-modules && make generate && make`.

## 2. The lifecycle contract

Every module implements exactly three functions whose prefix matches `module.name` (and, for directory modules, the directory name):

```c
int <name>_init(void);      /* once at startup: load+validate params, alloc tables    */
int <name>_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
int <name>_cleanup(void);   /* at shutdown: free module-owned memory                  */
```

Return-code semantics: `init` non-zero aborts startup (before any tree is processed); `process` non-zero exits the run with failure — log an `ERROR_LOG()` with the physics reason first, because the core only knows the module and substep; `cleanup` non-zero is logged and cleanup continues for other modules.

## 3. module_info.yaml schema

Enforced by `scripts/validate_modules.py` (exit codes: 1 schema, 2 missing file, 3 dependency, 4 naming) and consumed by `scripts/generate_module_registry.py`. Required: `name` (C identifier, `lowercase_with_underscores`) and `supported_processing_modes`. Everything else optional:

| Key | Meaning |
|---|---|
| `description` | One-sentence module contract |
| `display_name`, `version` (semver), `author` | Cosmetic/bookkeeping |
| `supported_processing_modes` | Non-empty subset of `process_full_halo`, `process_per_event`, `process_by_galaxy` |
| `additional_files` | Helper sources ONLY. **The primary `<name>.c` is implicit — never list it.** Only `.c` entries are compiled; `.h` entries are documentary |
| `compilation_requires` | Subset of `HDF5`, `MPI`, `GSL` (exact spelling) — module only builds when the feature is enabled |
| `dependencies.properties` / `dependencies.parameters` | Names the module uses. Validation aids checked against property metadata and `modules.parameters` — they do NOT order the pipeline; YAML phase order does |
| `events.emits` | List of `{name, description}` — producers only (`process_full_halo`) |
| `events.consumes` | List of `{producer, event}` — consumers only (`process_per_event`) |
| `tests.unit` / `tests.integration` / `tests.scientific` | Path(s) relative to the module dir, e.g. `_tests/test_unit_<name>.c` |
| `docs.physics` | Usually `README.md` |
| `is_utility` | True for non-runtime collections (e.g. `models/sage16/shared/`) — compiles no runtime module |

Real example — `models/sage16/modules/sage_apply_cooling/module_info.yaml` declares `process_by_galaxy`, nine property dependencies, `parameters: []`, both test paths, and `docs.physics: README.md`. Use it as the template for new production metadata.

## 4. Processing modes and the ordering law

| YAML mode | Module receives | Use for |
|---|---|---|
| `process_full_halo` | Whole FoF workspace, `ngal >= 1` | Cross-galaxy physics (infall budgets, merger detection); the ONLY mode that may emit events |
| `process_per_event` | One event target, `ngal == 1`, `ctx->active_event != NULL` | Physics triggered by a producer's event |
| `process_by_galaxy` | One galaxy, `ngal == 1` | Local per-galaxy physics and time integration |

Choose the narrowest mode that gives enough context. Validate the expectation at the top of `process()` (e.g. `if (ngal != 1) { ERROR_LOG(...); return -1; }`) and skip `halos[i].galaxy == NULL` and `Type == 3` entries.

**The ordering law** (per phase, enforced by the core — see `execute_phase` in `src/core/module_registry.c`): all `process_full_halo` modules run first in YAML order; events they emit are dispatched immediately to subscribed `process_per_event` consumers (consumer YAML order); then all `process_by_galaxy` modules run galaxy-major (for each galaxy, every by-galaxy module in YAML order). Mode grouping beats YAML line position: a by-galaxy module listed above a full-halo module still runs after it.

Phases: `pre_timestep` (once per snapshot interval) → each named phase under `modules.phases:` (once per substep, in YAML order; max 32 phases) → `post_timestep` (once). Adding a phase is a pure run-YAML change.

**Known shipped exception**: `sage_satellite_stripping` runs `process_by_galaxy` yet mutates the FoF central through `ctx->central_galaxy` — a deliberate SAGE-parity placement so stripping interleaves per-satellite with cooling (the `// SAGE parity:` comments in its `.c` explain it). Do not generalize from it without the same justification.

## 5. Events end to end

Events connect one `process_full_halo` producer to `process_per_event` consumers **in the same phase**. Generated constants live in `src/module_system/generated/event_contracts.h`: producer IDs as `MODULE_ID_<UPPER_NAME>` and per-producer enums named `<PascalName>EventId` with members `<UPPER_NAME>_EVENT_<UPPER_EVENT>` (IDs start at 1; 0 is reserved). Current shipped contract: `sage_resolve_mergers_and_disruption` emits `merger` (payload convention documented in its `events.emits` description: `value0` = baryonic mass ratio, `value1` = source substep dt), consumed by `sage_quasar_mode` and `sage_starburst_feedback`.

Producer code:

```c
#include "module_system/generated/event_contracts.h"
if (module_emit_event(ctx, SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER,
                      source_idx, target_idx, mass_ratio, source_dt) != 0) {
  ERROR_LOG("Failed to emit merger event");
  return -1;
}
```

`module_emit_event(ctx, event_id, source_index, target_index, value0, value1)` (declared in `src/core/module_interface.h`) validates that the emitting module declared the event and that indices are in bounds. Consumers read `ctx->active_event->value0/value1/source_index/target_index`; subscription routing guarantees only subscribed events arrive. Startup validation fails fast on: a per-event module with no `events.consumes`, a consumed producer/event that is not declared, or a producer not configured as `process_full_halo` in the same phase. Resolved contracts are recorded in HDF5 output under `RunProperties/EventContracts`.

## 6. Parameters

Parameters come from `modules.parameters` in the run YAML. There are **no defaults**: a missing required parameter fails the module's `init()` before any tree is processed — this is deliberate (reproducibility over convenience). Load and validate in `init()` with the macros from `src/module_system/parameter_helpers.h`, storing into module-private statics:

`LOAD_PARAM_DOUBLE`, `LOAD_PARAM_DOUBLE_INTERNAL`, `LOAD_PARAM_INT`, `LOAD_PARAM_STRING`, `VALIDATE_RANGE_EXCLUSIVE`, `VALIDATE_RANGE_INCLUSIVE`, `VALIDATE_OPTION`, `LOAD_AND_VALIDATE_RANGE_EXCLUSIVE`, `LOAD_AND_VALIDATE_RANGE_INCLUSIVE`, `LOAD_AND_VALIDATE_RANGE_INCLUSIVE_INTERNAL`, `LOAD_AND_VALIDATE_OPTION`.

The `*_INTERNAL` variants convert a dimensional parameter from the units declared in `models/<model>/parameter_units.yaml` into the internal reference basis on load (see `mimic-properties`; sham uses this, sage16 declares none). Declare every parameter in `dependencies.parameters`; `make lint-parameters` fails (exit 1) on used-but-undeclared and warns (exit 2) on declared-but-unused. Validate physical ranges locally — only the module knows its constraints.

## 7. Ordering guards and the transport-property pattern

Metadata dependencies do not enforce order, so modules that require a specific pipeline arrangement enforce it themselves at `init()` using the registry queries in `src/core/module_registry.h`:

- `module_precedes_in_substep_phase(first, first_mode, second, second_mode)` — e.g. `sage_apply_cooling` refuses to start unless `sage_calculate_cooling_budget` precedes it in the same phase.
- `modules_in_same_substep_phase(a, mode_a, b, mode_b)` — e.g. `sage_quasar_mode` requires its event producer in the same phase.

Copy this pattern whenever your module consumes another module's output within a substep.

**Transport properties** are the inter-module data bus: galaxy properties declared with `output: false` + `init_repeat: true` (see `mimic-properties` for the schema). A "calculate" module writes the budget; an "apply" module commits and usually zeroes it. Shipped pairs: `sage_calculate_cooling_budget` → `CoolingGas` → `sage_apply_cooling`; `sage_calculate_star_formation` / `sage_calculate_supernova_feedback` → `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass` → `sage_apply_star_formation_supernova` (with `NewStellarMass` finally consumed and zeroed by `sage_apply_metal_enrichment`, which must come after the disk-instability chain — a parity ordering enforced at init). Disabling one half of a pair silently breaks the other: check the module's README and `dependencies.properties` before removing anything from a pipeline.

## 8. Worked examples (read these files, in this order)

1. `src/module_system/template/` — `template_module.c` + `template_module_info.yaml`: the fill-in-the-blank skeleton for a new module. Keep the test paths as `_tests/...`, matching live modules and `docs/DEVELOPER-GUIDE.md`.
2. `models/sage16/modules/sage_apply_cooling/` — minimal production module: by-galaxy, no parameters, one ordering guard, uses the model-local shared helper `shared/metallicity.h`, guards `ngal == 1` and `halo->dT > 0`.
3. `models/sage16/modules/sage_calculate_cooling_budget/` — module with data tables: `additional_files: [cooling_tables.c, cooling_tables.h]`, loads its `CoolFunctions/` table directory in `init()` via the compile-time `MIMIC_COMPILED_MODEL_PATH`, frees in `cleanup()`, reads per-object substep time through `shared/time_parity.h` (`mimic_object_substep_dt`).
4. `models/sage16/modules/sage_resolve_mergers_and_disruption/` — event producer; its 29-line `README.md` is the house model for module documentation (what/mode/position/properties/parameters/events/notes/references).

## 9. Module code rules

- Logging: `DEBUG_LOG`/`VERBOSE_LOG`/`INFO_LOG`/`WARNING_LOG`/`ERROR_LOG`/`FATAL_ERROR` from `src/util/error.h` — never `printf`.
- Constants: universal physics from `src/module_system/physical_constants.h` (G, c, Z_SUN, ENERGY_SN...); model-specific science constants in `models/<model>/shared/` (e.g. `sage_constants.h`), never inline magic numbers.
- Memory: module-owned allocations via `mymalloc_cat(size, MEM_UTILITY)` and freed in `cleanup()` — the leak checker reports what you forget.
- Treat all `ctx` fields as read-only (`ctx->substep_dt`, `ctx->central_galaxy`, `ctx->params`, ...).
- Style: 2-space indent, 100-column, warning-clean under `-Wall -Wextra -Wshadow -Wformat-security -Wundef`; comments explain *why*, one line; mark intentional SAGE mirroring with `// SAGE parity:`.
- Model-local shared helpers (`models/<model>/shared/*.h`) are the home for calculations several modules need; they are model-private, not framework API.

## 10. Checklists

**Add a module** (default selectors shown; substitute your pair uniformly):

```bash
mkdir -p models/sage16/modules/my_module/_tests
# 1. Write my_module.c from src/module_system/template/template_module.c
# 2. Write module_info.yaml (section 3) and README.md (section 8.4 pattern)
# 3. Add tests from tests/framework templates; register them in module_info.yaml
make validate-modules && make lint-parameters
make generate && make
# 4. Add the module + parameters to a run YAML (modules.phases + modules.parameters)
./mimic --debug models/sage16/input/sage16_mini-millennium.yaml > debug.log 2>&1
rc=$?; tail -n 80 debug.log; echo "run_exit_code=$rc"
tests/unit/run_tests.sh test_unit_my_module
# Delegate this full-suite run unless you are the test subagent; unit/integration are long.
mkdir -p archive/test-logs && make tests summary > archive/test-logs/tests.log 2>&1
rc=$?; echo "exit_code=$rc"
```

**Modify an existing module**: read README + module_info + tests first → check for `SAGE parity:` markers (behavior changes to parity code route through `mimic-change-control` and need the evidence bar of `mimic-scientific-method`) → make the change → `make validate-modules && make lint-parameters && make generate && make` → module tests → full relevant tiers → update README/metadata if the contract changed.

## Provenance and maintenance

Verified against the live repo 2026-07-04. Re-verify drift-prone specifics:

```bash
grep -n "^#define LOAD\|^#define VALIDATE" src/module_system/parameter_helpers.h   # macro set
grep -n "module_precedes_in_substep_phase\|modules_in_same_substep_phase" src/core/module_registry.h
sed -n '1,45p' src/module_system/generated/event_contracts.h                      # ID naming pattern
grep -n "VALID_PROCESSING_MODES\|VALID_COMPILATION_FEATURES" scripts/generate_module_registry.py scripts/validate_modules.py
grep -n "standalone" scripts/generate_module_registry.py | head -5                # prototype semantics
ls src/module_system/template/                                                    # template files
cat models/sage16/modules/sage_apply_cooling/module_info.yaml                     # reference metadata
grep -rn "MAX_SUBSTEP_PHASES" src/ | head -3                                      # phase cap (32)
```

The schema tables (sections 3–6) drift only when the generator/validator scripts change; the ordering law and lifecycle contract are architectural and durable (see `mimic-architecture-contract`).
