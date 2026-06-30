# Mimic Dynamic-Timestep Plan

**Status:** Planned (not yet implemented)
**Author:** Planning pass, 2026-06-30
**Scope:** Add an opt-in `TimestepScheme: dynamic` option that sizes the per-snapshot substep count from the halo dynamical time `t_dyn = Rvir/Vvir`, instead of the fixed `SubSteps` count used today.

**Terminology update, 2026-06-30:** The scheme name was renamed from `dynamical` to `dynamic` before the Slice 1 commit. Future work should use `dynamic` in YAML values, enum names, helper names, tests, docs, and handoff text. The plan filename may still contain `DYNAMICAL` for continuity with the original chat prompt; if any missed scheme-label occurrence says `dynamical`, treat it as stale terminology and update it to `dynamic`. Do not rename the physical phrase "dynamical time" unless the text is referring to the configuration scheme name.

---

## Motivation

Today Mimic divides every snapshot interval `deltaT` into a fixed `SubSteps` equal pieces (`ctx->num_substeps`, default 1; sage16 ships 10). The substep count is the same regardless of redshift or how many dynamical times the interval actually spans.

A more physical choice resolves the physics on the local, redshift-dependent dynamical timescale. In dynamic mode the substep interval becomes `dt = t_dyn / SubSteps` and the per-snapshot count becomes:

```
N = ceil( deltaT * SubSteps / t_dyn ),  clamped to [1, MAX_DYNAMIC_SUBSTEPS]
```

So `SubSteps` is reinterpreted as *resolution per dynamical time*. When `deltaT` is short relative to `t_dyn` (typically low z) `N` collapses to the minimum of 1; when `deltaT` spans many dynamical times (typically high z, where `t_dyn` is short) `N` grows so accumulating physics (star formation, feedback, reincorporation) is integrated over many fractions of a dynamical time.

### Key physical result (de-risks the design)

`Rvir` and `Vvir` are both recomputed from `Mvir` and `ρcrit(z)` at fixed 200×critical overdensity (`src/core/virial.c`). The mass cancels exactly:

```
t_dyn = Rvir / Vvir = sqrt( 3 / (800·π·G·ρcrit(z)) ) = 1 / (10·H(z))      (code units)
```

`t_dyn` depends **only on redshift**, not halo mass. Consequences:

- Every halo at a given snapshot has the same `t_dyn`, so computing `N` from the FoF central is well-justified — there is no meaningful per-object disagreement to resolve, and the existing single shared substep loop is untouched.
- `N` is effectively a per-snapshot quantity. **Alternative considered:** precompute a per-snapshot `N[snap]` array at init (exploiting mass-independence). **Chosen:** compute `N` per-FoF at runtime from the central's actual `Rvir/Vvir`. Rationale — it directly implements "halo dynamical time" as requested, adds no new global state and no init-ordering dependency (smaller surface), and is the cost of two cheap virial calls per FoF central. The mass-independence is *current-code* behaviour (`virial.c` recomputes `Rvir` and ignores catalog `Rvir`), so the per-snapshot optimisation is left as a documented future option rather than relied upon.

### Decisions locked by the user

- `t_dyn = Rvir/Vvir` (crossing-time convention, matches `sage_reincorporation`).
- Rounding: `ceil` (never under-resolve), floored at a minimum of 1.
- Reuse the existing `SubSteps` parameter (no new resolution parameter).
- `MAX_DYNAMIC_SUBSTEPS = 50`, an internal constant — **not** user-exposed; the user will test and adjust.
- **No SAGE parity requirement.** This is a model extension; default `sage16` keeps `TimestepScheme: fixed`. Parity tests/baselines are unaffected because the fixed path is untouched.

### The "fixed-mode unchanged" invariant (made precise)

"Unchanged" means **scientific output values** are byte-for-byte identical for fixed-mode/default runs: galaxy/halo datasets in HDF5 and the binary galaxy output. It does **not** forbid additive run-level provenance — Slice 3 adds `SubSteps`/`TimestepScheme` attributes to the master file's `RunProperties` for *all* runs (Vision principle 6), which changes metadata bytes but no science values. This resolves the apparent tension between the invariant and Slice 3, and keeps provenance universal rather than gating it to dynamic runs.

### Architecture notes confirmed during planning

- The substep loop in `execute_module_pipeline` (`src/core/module_registry.c:931`) is a single shared loop over `ctx->num_substeps` for the whole FoF workspace. Only the **count** becomes dynamic; the loop structure is unchanged.
- Module `num_substeps` consumers fall into three groups, all of which adapt correctly to a variable `N` but must be test-locked:
  - **Per-object `dT/num_substeps`** via `mimic_object_substep_dt()` (`models/sage16/shared/time_parity.h`) — e.g. star formation (`sage_calculate_star_formation.c:74-99`).
  - **Direct `/ ctx->num_substeps`** — e.g. infall budget (`sage_apply_infall.c:59-60`) and satellite stripping (`sage_satellite_stripping.c:74-76`). These are *not* routed through the helper, so Slice 2 must add conservation/invariance tests for them specifically.
  - **Substep-fraction** consumers (mergers, merger clock) key off `(substep_number+1)/num_substeps` and `substep_time` (`sage_resolve_mergers_and_disruption.c:166-219`) — adapt automatically.
  - Full-interval rate normalisers (e.g. cooling luminosity `/= halo->dT`) are substep-count invariant by construction.
- `t_dyn` in `Rvir/Vvir` is already in internal time units consistent with `Age[]`/`dT` (evidenced by `sage_reincorporation` using `Rvir/Vvir` as a time directly). Implementation must still guard `Vvir > 0`.

---

## Slice 1: Config plumbing for `TimestepScheme`

### Intended Change
- Add a `TimestepScheme` enum (`fixed`, `dynamic`) to the config types.
- Parse `TimestepScheme` (optional top-level YAML key, default `fixed`), validate the value, and log it.
- Add the `MAX_DYNAMIC_SUBSTEPS` constant.
- Add a `timestep_scheme_name()` string helper (used for logging now and metadata in Slice 3).
- **No change to substep-count behaviour yet** — the scheme is parsed and stored but `ctx->num_substeps` is still computed the fixed way.

### Acceptance Criteria
- Inputs: a run YAML with `TimestepScheme: dynamic`, `TimestepScheme: fixed`, an absent key, and an invalid value.
- Outputs: `MimicConfig.TimestepScheme` set correctly; startup log line reports the active scheme; an invalid value triggers a fatal error with a clear message.
- User-visible behaviour: absent key → `fixed` (unchanged default).
- Behaviour that must not change: any run without `TimestepScheme`, and any run with `TimestepScheme: fixed`, produces identical output to today.

### Authorized Surface
- Files allowed to change:
  - `src/include/types.h` — add `enum TimestepScheme { TIMESTEP_SCHEME_FIXED = 0, TIMESTEP_SCHEME_DYNAMIC = 1 }` (FIXED **must** be 0 so `memset`-zeroed test fixtures default to fixed — see `tests/framework/core_test_fixtures.h`) and an `enum TimestepScheme TimestepScheme;` field in `struct MimicConfig` (adjacent to `SubSteps`, ~line 121).
  - `src/include/constants.h` — add `#define MAX_DYNAMIC_SUBSTEPS 50`.
  - `src/include/proto.h` — declare `const char *timestep_scheme_name(enum TimestepScheme scheme);`.
  - `src/core/read_parameter_file.c` — parse + validate `TimestepScheme` near the `SubSteps` block (~line 180); define `timestep_scheme_name()`; add a log line near the existing `SubSteps` log (~line 1351).
  - `tests/unit/test_parameter_parsing.c` — add cases for the four parse scenarios.
- Functions/classes allowed to change: `read_parameter_file()` (parse block), the config-summary logger, `struct MimicConfig`.
- Tests allowed or expected to change: `tests/unit/test_parameter_parsing.c`.

### Explicit Non-Goals
- No change to `setup_module_context` or substep-count computation (Slice 2).
- No metadata/provenance changes (Slice 3).
- `MAX_DYNAMIC_SUBSTEPS` is not exposed in YAML.

### Risk Flags
- Risky surfaces touched: input-config contract (additive, default-preserving) and a `struct MimicConfig` field add (shared type). Low risk — additive only.
- Approval needed before implementation: no.

### Validation Plan
- Tests to add/update: `tests/unit/test_parameter_parsing.c` — assert default `fixed`, explicit `fixed`/`dynamic`, and fatal on invalid. Emit `MIMIC_RESULT` markers (see `mimic-tests` skill).
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium generate`
  - `make MODEL=sage16 SIMULATION=mini-millennium`
  - `tests/unit/run_tests.sh test_parameter_parsing`
- Manual checks: run `./mimic models/sage16/input/sage16_mini-millennium.yaml` and confirm the scheme log line shows `fixed`.

### Rollback Path
- Revert the slice commit; the field is additive and unused elsewhere until Slice 2.

---

## Slice 2: Dynamic substep-count computation

### Intended Change
- Add a pure, unit-testable helper `int compute_dynamic_substeps(double time_interval, double t_dyn, int substeps_per_tdyn)` returning `clamp(ceil(time_interval * substeps_per_tdyn / t_dyn), 1, MAX_DYNAMIC_SUBSTEPS)`, with guards: if `substeps_per_tdyn < 1` treat as 1; if `time_interval`, `t_dyn`, or the computed value is not finite/`> 0` (use `isfinite()` before `ceil` and before the `int` cast), return 1.
- **Helper location:** put it in a new `src/core/timestep.c` (+ declaration in `src/include/proto.h`), *not* in `build_model.c`. Reason: the unit-test harness builds `CORE_SRCS` without `build_model.c` (`tests/unit/run_tests.sh:175`), and adding `build_model.c` would duplicate `build_halo_tree()` already stubbed in `tests/unit/test_stubs.c`. A dedicated `timestep.c` is linkable in both the executable and the unit harness.
- In `setup_module_context` (`src/core/build_model.c`), reorder so the dynamic branch is correct: compute `ctx->time_interval` first (currently set *after* `num_substeps` at line 456 vs 466 — must move), then `t_dyn`, then `ctx->num_substeps`, then `ctx->substep_dt = time_interval / num_substeps`. When `MimicConfig.TimestepScheme == TIMESTEP_SCHEME_DYNAMIC`, compute the central's `t_dyn` from `get_virial_radius(halonr)` / `get_virial_velocity(halonr)` and `ctx->num_substeps = compute_dynamic_substeps(ctx->time_interval, Rvir/Vvir, MimicConfig.SubSteps)`. The fixed branch keeps the existing `(SubSteps > 0) ? SubSteps : 1`.
- **Virial source constraint:** use the `halonr`-based virial helpers, which recompute current-snapshot values (`virial.c:99-115`). Do **not** read `FoFWorkspace[centralgal].Rvir/Vvir` — inheritance preserves the progenitor's `Rvir`/`Vvir` when descendant mass decreases (`src/core/inheritance.c:55-61`), so the stored workspace values can be stale.

### Acceptance Criteria
- Inputs: representative `(time_interval, t_dyn, SubSteps)` triples spanning high z (interval ≫ t_dyn), low z (interval < t_dyn), the clamp ceiling, and degenerate `t_dyn <= 0` / `time_interval <= 0`.
- Outputs: `N` matches `clamp(ceil(...), 1, 50)`; degenerate inputs return 1; first-snapshot interval (`time_interval == 0`) returns 1.
- User-visible behaviour: a `TimestepScheme: dynamic` run completes end-to-end and produces more substeps at high z than at low z.
- Behaviour that must not change: `TimestepScheme: fixed` (and the default) yields exactly the prior `num_substeps` and byte-identical output; SHAM and `halos-only` runs are unaffected (no substep-phase integrators).

### Authorized Surface
- Files allowed to change:
  - `src/core/timestep.c` (new) — define `compute_dynamic_substeps()`.
  - `src/include/proto.h` — declare `int compute_dynamic_substeps(double, double, int);`.
  - `src/core/build_model.c` — reorder + branch in `setup_module_context` on `TimestepScheme`; call the helper and the `halonr` virial helpers.
  - `Makefile` — **no edit expected**: the production build auto-discovers `src/**/*.c` via `find` (`Makefile:112` → `OBJECTS` → link), so `timestep.c` is picked up automatically. Just verify the object appears in the build.
  - `tests/unit/run_tests.sh` — add `src/core/timestep.c` to `CORE_SRCS` (~line 175); the unit harness uses an explicit source list (not auto-discovery), so this addition *is* required for the helper to link in tests.
  - `tests/unit/test_dynamic_substeps.c` (new) — table-driven helper tests (register per `mimic-tests` skill).
  - `tests/integration/test_substeps.py` — extend with a dynamic-mode case, or add a sibling `test_dynamic_timestep.py`; do not weaken existing fixed-mode assertions.
  - `tests/framework/harness.py` — add a `timestep_scheme` option to `create_test_param_file()` (it has `substeps` but no scheme; ~line 430-433/528-532), or have the new test mutate the YAML directly.
- Functions/classes allowed to change: `setup_module_context` (`build_model.c`), new `compute_dynamic_substeps`, `create_test_param_file` (additive kwarg).
- Tests allowed or expected to change: new unit test; new/extended dynamic integration test; conservation tests for the direct `num_substeps` consumers (infall, satellite stripping).

### Explicit Non-Goals
- No per-object substep counts; `N` remains one shared value per FoF evolution.
- No change to `mimic_object_substep_dt` / `time_parity.h` (it already consumes `num_substeps`).
- No metadata/docs changes (Slice 3).

### Risk Flags
- Risky surfaces touched: the core stepping count — the behavioural heart of this feature. Gated behind opt-in; fixed path untouched; **no generated files** involved.
- Approval needed before implementation: no (no parity constraint per user), but this is the slice to review most carefully and the one to verify with an actual dynamic run.

### Validation Plan
- Tests to add/update:
  - `tests/unit/test_dynamic_substeps.c` — table-driven assertions over the triples above, including clamp at 50 and degenerate/non-finite guards (`t_dyn<=0`, `time_interval<=0`, `inf`/`nan`). Emit `MIMIC_RESULT` markers.
  - Conservation tests for the **direct** `num_substeps` consumers: infall budget distribution sums correctly across a varied `N` (`sage_apply_infall.c:59-60`) and satellite stripping conserves mass across a varied `N` (`sage_satellite_stripping.c:74-76`).
  - Integration: a dynamic-mode run reaches z=0 without error and uses more substeps at early (high-z) snapshots than late ones.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium`
  - `tests/unit/run_tests.sh test_dynamic_substeps`
  - `python3 tests/integration/test_substeps.py` (delegate full tiers to a subagent per AGENTS.md)
  - A smoke run with a `dynamic` copy of `models/sage16/input/sage16_mini-millennium.yaml`.
- Manual checks: per-snapshot `num_substeps` is logged only at `DEBUG_LOG` level (`module_registry.c:846-847,882-883`), so confirm variation with `--debug` (not `--verbose`), or assert it through the unit test / a `test_fixture`-based integration check (`src/module_system/test_fixture/test_fixture.c:105-109`, parsed via `tests/framework/harness.py`). Confirm a fixed-mode run's **science output** is byte-identical to a pre-change build (diff the binary galaxy output / HDF5 halo datasets, per the invariant above — metadata attributes excluded).

### Rollback Path
- Revert the slice commit; Slice 1's parsed field becomes inert again (parsed but unused), which is safe.

---

## Slice 3: Provenance, docs, and sweeps

### Intended Change
- Record the timestep configuration in run output for self-describing provenance (Vision principle 6): in `store_run_properties()` (the **master**-file writer, `metadata_hdf5.c:477`), add `{"SubSteps", INT, &MimicConfig.SubSteps}` to the hand-maintained config table (~line 485-499; INT entries are supported) and write `TimestepScheme` as a string attribute via `timestep_scheme_name()` (copy into a `MAX_STRING_LEN` buffer using the existing string helper, `metadata_hdf5.c:24-30`). Scope: master `RunProperties` only — the per-file writer `write_perfile_metadata()` uses the `ModelParams` table, not this config table, so leave it untouched.
- Update `docs/USER-GUIDE.md` timestep section to document the two schemes and the reinterpretation of `SubSteps` in dynamic mode.
- Fix stale developer docs/comments that still describe substeps as fixed-count or show `ctx->substep_dt` for integration — point them at the per-object `mimic_object_substep_dt()` pattern and note `num_substeps` may be scheme-derived:
  - `src/core/module_interface.h` — the process() doc comment (~line 335) **and** the `num_substeps`/`substep_dt` field descriptions (~line 173-204).
  - `src/include/types.h` — the lifecycle comment `... x SubSteps ...` and "`SubSteps`: number per snapshot interval" (~line 108-121).
  - `src/core/build_model.c` — the "SubSteps parameter controls time sub-stepping" comment (~line 489-490).
  - `docs/DEVELOPER-GUIDE.md` — the `ctx->substep_dt` example (~line 64-77) and the "num_substeps is configured" text (~line 1269-1277).
- Skill sweep: update `mimic-modules` / `mimic-debug` (and any plotting/test skill) if they describe substep behaviour.

### Acceptance Criteria
- Inputs: a completed HDF5 run in each scheme.
- Outputs: HDF5 metadata exposes `SubSteps` and `TimestepScheme`; docs describe both schemes; the doc comment is corrected.
- User-visible behaviour: an output file is self-describing about its timestep scheme.
- Behaviour that must not change: numerical output values; only metadata/attributes are added.

### Authorized Surface
- Files allowed to change:
  - `src/io/output/metadata_hdf5.c` — in `store_run_properties()`, add the `SubSteps` INT row to the config table and write the `TimestepScheme` string attribute via the helper. Leave `write_perfile_metadata()` untouched.
  - `docs/USER-GUIDE.md` — timestep/substep section (~lines 161, 205).
  - `docs/DEVELOPER-GUIDE.md` — correct the `ctx->substep_dt` example and `num_substeps` text (~line 64-77, 1269-1277).
  - `src/core/module_interface.h` — correct the substep_dt process() comment (~line 335) and the `num_substeps`/`substep_dt` field descriptions (~line 173-204). Comment-only.
  - `src/include/types.h` — correct the lifecycle / `SubSteps` comments (~line 108-121). Comment-only (re-touch; field added in Slice 1).
  - `src/core/build_model.c` — correct the "SubSteps controls sub-stepping" comment (~line 489-490). Comment-only (re-touch; logic edited in Slice 2).
  - `.agents/skills/mimic-*` — sweep and update stale substep descriptions.
  - `tests/integration/` + `tests/framework/data_loader.py` — extend the HDF5 metadata round-trip test (existing tests validate layout/baseline but not specific `RunProperties` attributes, `tests/integration/test_output_formats.py:311-360`); the new check must read back `TimestepScheme` and `SubSteps` and assert they match the run config.
- Functions/classes allowed to change: `store_run_properties` (additive table row + one attribute write).
- Tests allowed or expected to change: metadata round-trip integration test (additive); `data_loader.py` reader if needed to surface the attributes.

### Explicit Non-Goals
- No changes to the binary `output_schema.json` generator or any `*/generated/` file.
- No new model-parameter plumbing — `TimestepScheme`/`SubSteps` are core params, not `modules.parameters`.

### Risk Flags
- Risky surfaces touched: HDF5 output metadata (I/O surface). The parameter table is hand-maintained and explicitly generic by design — low risk, no generated code.
- Approval needed before implementation: no.

### Validation Plan
- Tests to add/update: metadata round-trip integration test reads back `TimestepScheme` and `SubSteps`.
- Commands to run:
  - `make MODEL=sage16 SIMULATION=mini-millennium`
  - `make check-docs`
  - `./mimic models/sage16/input/sage16_mini-millennium.yaml` then `h5dump`/inspect the metadata group.
- Manual checks: confirm both attributes appear and match the run config in fixed and dynamic modes.

### Rollback Path
- Revert the slice commit; attributes and doc edits are additive.

---

## Cross-slice notes

- **Selectors:** use `MODEL=sage16 SIMULATION=mini-millennium` consistently for generate, build, tests, and runs (per AGENTS.md).
- **Repo state:** source tree was clean on `main` at planning time; the only untracked file is this plan document. Create a feature branch before implementing.
- **Pre-commit (per AGENTS.md):** run `./scripts/beautify.sh`, do the style sweep against `docs/STYLE-GUIDE.md`, and do the skill sweep (folded into Slice 3). Ask before committing.
- **Full-tier tests:** delegate unit/integration runs to a subagent that returns a pass/fail summary (AGENTS.md), acting on the report in the main context.

---

## Next Chat Prompt

**Mode A — Assisted run** (recommended: Slice 2 is the behavioural core and the user wants to test/adjust between slices).

```md
Plan file: docs/dev/MIMIC-DYNAMICAL-TIMESTEP-PLAN.md
Slices this session: Slice 1

Read the full plan file first. If a selected slice receipt is incomplete or the plan state is unclear, stop and tell me before coding.

Work on the current feature branch for this plan; if none exists, create one and tell me the name.

Use ai-orchestrator as the controlling skill. Keep the implementation local; delegate per that skill's guidance when independence or context economy helps — primarily hostile drift-audit, independent code-review, and long-running tests.

For each selected slice, in plan order:
1. Restate the frozen contract (authorized surface + non-goals) from the plan.
2. If the slice's Risk Flags mark approval-needed, stop and get my approval before coding.
3. Apply scoped-implementation against the slice contract.
4. Apply drift-audit. Report the authorization gate result before any quality review.
5. If the gate passes, apply code-review. If it fails, fix the drift and re-audit.
6. Surface drift and review findings to me, fix them, then re-run the relevant gate. If consecutive reviews return only minor findings and have clearly converged, record residuals in the slice summary and proceed.
7. Ask me before committing. On my approval, commit that slice with the commit skill.

After the selected slice(s) are committed, use handoff to record state and the next slice to resume from. Do not continue past the selected slice(s).

Confirm before starting: plan file read, selected slice(s), branch, and the first slice.
```

(Slices are well-isolated, opt-in, and touch no generated/schema/migration surfaces, so Mode B autonomous is also viable if you'd rather run all three slices unattended — swap in the Mode B launcher from the implementation-plan skill, scope "all remaining slices".)
