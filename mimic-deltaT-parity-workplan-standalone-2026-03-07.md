# Standalone Workplan: Restore Exact SAGE `deltaT` Semantics in Mimic

Date: 2026-03-07
Repo: `/Users/dcroton/Local/git-repos/mimic`
Scope: Ensure Mimic uses the same per-galaxy/per-satellite time definition as SAGE in physics and merger/disruption logic.

## 1) Problem Statement (Parity Contract)

In SAGE, `deltaT` is computed **inside the loop over each galaxy/satellite**, not globally from the central.

Reference snippet (SAGE model):
- File: `sage-code/sage-model/core_build_model.c`
- Function: `evolve_galaxies(...)`
- Physics loop: lines 338-363
- Merger/disruption loop: lines 376-400

```c
const double deltaT = run_params->Age[galaxies[p].SnapNum] - halo_age;
...
reincorporate_gas(..., deltaT / STEPS, ...);
cooling_recipe(..., deltaT / STEPS, ...);
starformation_and_feedback(..., time, deltaT / STEPS, ...);
...
galaxies[p].MergTime -= deltaT / STEPS;
...
deal_with_galaxy_merger(..., time, deltaT / STEPS, ...);
```

Therefore parity rule is:
- For any object `p`: `deltaT_p = Age[snap_of_p] - Age[current_halo_snap]`
- Per-substep dt for `p`: `deltaT_p / STEPS`

## 2) Current Mimic State (What It Does Today)

### 2.1 Per-object `dT` already exists and is correct at copy/init

- File: `src/core/build_model.c`
- Function: `copy_progenitor_halos(...)`
- Lines: 243-249

```c
int current_snap = InputTreeHalos[halonr].SnapNum;
int progenitor_snap = FoFWorkspace[ngal].SnapNum;
FoFWorkspace[ngal].dT = Age[progenitor_snap] - Age[current_snap];
```

Also for newly created halos:
- File: `src/core/virial.c`
- Function: `init_halo(...)`
- Line: 59 (`FoFWorkspace[p].dT = Age[current_snap - 1] - Age[current_snap];`)

### 2.2 `ModuleContext` currently computes one central-derived global substep dt

- File: `src/core/build_model.c`
- Function: `setup_module_context(...)`
- Lines: 507-518

```c
int prev_snap = FoFWorkspace[centralgal].SnapNum;
ctx->time_interval = Age[prev_snap] - Age[snap];
ctx->substep_dt = ctx->time_interval / ctx->num_substeps;
```

That is central/global, not per-object.

## 3) Exact Mismatch Locations to Fix

These modules currently use `ctx->substep_dt` where parity expects object-local `dT/num_substeps`.

1. `src/modules/sage_calculate_cooling/sage_calculate_cooling.c`
- Function: `sage_calculate_cooling_process(...)`
- Line: 115
```c
double coolingGas = cooling_recipe(halo, ctx, ctx->substep_dt, &rcool, &lambda);
```

2. `src/modules/sage_calculate_star_formation.c`
- Function: `sage_calculate_star_formation_process(...)`
- Line: 61
```c
const double dt = ctx->substep_dt;
```

3. `src/modules/sage_radio_mode_heating.c`
- Function: `sage_radio_mode_heating_process(...)`
- Line: 240
```c
do_AGN_heating(halo, ctx, ctx->substep_dt);
```

4. `src/modules/sage_update_merger_time.c`
- Function: `sage_update_merger_time_process(...)`
- Line: 77
```c
const double dt = ctx->substep_dt;
```

5. `src/modules/sage_reincorporation.c`
- Function: `sage_reincorporation_process(...)`
- Line: 75
```c
double reincorporated = ... * ctx->substep_dt;
```

6. `src/modules/sage_merge_galaxies.c`
- Function: `sage_merge_galaxies_process(...)`
- Lines: 135, 142
```c
central->TimeOfLastMinorMerger = ctx->substep_time;
central->TimeOfLastMajorMerger = ctx->substep_time;
```
These timestamps are currently central/global-time based.

Note: Some modules already use `halo->dT` correctly for rates (good baseline):
- `src/modules/sage_add_cooling.c` line 45
- `src/modules/sage_update_star_formation_supernova.c` lines 98, 138

## 4) KISS Professional Fix (Do This)

### 4.1 Introduce one shared helper for object-local dt/time

Create helper in a shared header (recommended):
- New file: `src/modules/_shared/time_parity.h`

Add functions:
1. `mimic_object_substep_dt(const struct Halo *halo, const struct ModuleContext *ctx)`
- returns `halo->dT / ctx->num_substeps` when valid, else `0.0`.
2. `mimic_object_substep_time(const struct Halo *halo, const struct ModuleContext *ctx)`
- returns SAGE-equivalent midpoint time for that object:
- `Age[halo->SnapNum] - (ctx->substep_number + 0.5) * (halo->dT / ctx->num_substeps)`

Implementation notes:
- Guard for null pointers, non-positive `num_substeps`, sentinel `dT <= 0.0`.
- Keep `ctx->substep_dt` for true FOF-global logic only.

### 4.2 Replace module-local dt usage with object-local dt

Patch these files/functions:
1. `sage_calculate_cooling_process` -> use `dt_obj = mimic_object_substep_dt(halo, ctx)`
2. `sage_calculate_star_formation_process` -> use object-local dt
3. `sage_radio_mode_heating_process` -> pass object-local dt to `do_AGN_heating`
4. `sage_update_merger_time_process` -> use per-satellite dt inside loop, not one global `dt`
5. `sage_reincorporation_process` -> for central object, use `mimic_object_substep_dt(&halos[0], ctx)`

### 4.3 Fix merger event timestamps to object-local time

Patch `sage_merge_galaxies_process`:
- Replace `ctx->substep_time` with per-satellite time:
- `event_time = mimic_object_substep_time(&halos[i], ctx)`
- Use `event_time` for `TimeOfLastMinorMerger` / `TimeOfLastMajorMerger`.

This aligns with SAGE where merge time uses the satellite’s `deltaT` in-loop.

## 5) File/Function Patch Checklist

- [ ] `src/modules/_shared/time_parity.h` (new)
- [ ] `src/modules/sage_calculate_cooling/sage_calculate_cooling.c` (`sage_calculate_cooling_process`)
- [ ] `src/modules/sage_calculate_star_formation.c` (`sage_calculate_star_formation_process`)
- [ ] `src/modules/sage_radio_mode_heating.c` (`sage_radio_mode_heating_process`)
- [ ] `src/modules/sage_update_merger_time.c` (`sage_update_merger_time_process`)
- [ ] `src/modules/sage_reincorporation.c` (`sage_reincorporation_process`)
- [ ] `src/modules/sage_merge_galaxies.c` (`sage_merge_galaxies_process`)

Optional docs touch:
- [ ] `src/core/module_interface.h` comments: clarify `ctx->substep_dt` is global convenience, while SAGE-parity modules should prefer object-local dt.

## 6) Validation Plan

### 6.1 Unit tests (minimum)

Run module unit tests related to modified files:
- `make test-unit`

Recommended targeted checks:
- Merger time decrement behavior uses per-satellite dt.
- Cooling/star-formation results differ when two halos in one FOF have different `dT`.
- Major/minor merger timestamps derive from satellite-local time.

### 6.2 Integration/parity tests

1. Build deterministic fixtures with mixed `dT` in same FOF workspace.
2. Compare pre/post patch behavior for:
- `MergTime` decrement
- merge/disrupt eligibility timing
- cooling/star formation mass updates
- `TimeOfLastMajorMerger`/`TimeOfLastMinorMerger`
3. Compare against SAGE trace for same fixture.

### 6.3 Acceptance criteria

- All patched modules use object-local dt where SAGE does.
- No regressions in existing unit/integration tests.
- Mixed-`dT` fixture shows corrected parity behavior.

## 7) Risks and Mitigations

Risk 1: Some modules intentionally rely on global FOF dt.
- Mitigation: only swap dt in modules that map directly to SAGE in-loop per-galaxy/per-satellite operations.

Risk 2: Timestamp semantics could shift historical outputs.
- Mitigation: document expected one-time parity correction and verify against SAGE fixture outputs.

Risk 3: Inconsistent helper adoption.
- Mitigation: enforce one helper include and grep check for `ctx->substep_dt` in SAGE modules after patch.

## 8) Fresh-Chat Kickoff Prompt (Ready to Paste)

"Implement the standalone plan in `/Users/dcroton/Local/git-repos/mimic/obsidian-inbox/mimic-deltaT-parity-workplan-standalone-2026-03-07.md`. Use SAGE parity rule that per-galaxy/per-satellite `deltaT` is computed from that object’s `SnapNum` inside loops. Patch listed modules to use object-local dt (`halo->dT / num_substeps`) and object-local merger timestamp. Run relevant tests and report diffs with file/line refs."

## 9) Status

This document is planning-only. No source code changes included here.