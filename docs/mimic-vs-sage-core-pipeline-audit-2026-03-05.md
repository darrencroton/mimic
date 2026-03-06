# Mimic vs SAGE Core Merger-Tree Pipeline Audit (2026-03-05)

## Scope and Method
- Requested comparison: `sage-code/sage/core_build_model.c` vs `src/core/build_model.c`.
- I first mapped each algorithm end-to-end, then traced each step into adjacent files that define semantics (types, init, merger handling, module execution order, and default pipeline config).
- No code changes were made to source files.

Files reviewed for context:
- `sage-code/sage/core_build_model.c`
- `src/core/build_model.c`
- `sage-code/sage/model_misc.c`
- `src/core/virial.c`
- `sage-code/sage/model_mergers.c`
- `sage-code/sage/model_disk_instability.c`
- `src/core/module_registry.c`
- `src/core/module_registry.h`
- `src/modules/sage_update_merger_time.c`
- `src/modules/sage_calculate_merger_timescale.c`
- `src/modules/sage_merge_galaxies.c`
- `src/modules/sage_disrupt_satellites.c`
- `src/modules/sage_quasar_mode.c`
- `src/modules/sage_collisional_starburst.c`
- `input/millennium.yaml`
- `src/include/generated/property_defs.h`
- `sage-code/sage/types.h`

## Executive Summary
- The tree traversal skeleton in Mimic is still recognizably derived from SAGE.
- The core evolution logic is no longer hardcoded in `build_model.c`; it is delegated to a modular phase pipeline.
- Several differences are expected architecture changes, but there are also high-risk behavioral divergences that can materially change merger outcomes.
- Highest-risk findings are:
- `Type 2` satellite handling diverges strongly from SAGE.
- Central-link semantics diverge (FOF-global central in Mimic vs per-subhalo central links in SAGE internals).
- Merger-triggered downstream physics appears disconnected in the current module-mode execution order.
- Merger event timestamps are not computed with SAGE’s substep midpoint time formula.

## Algorithm Map: SAGE (`core_build_model.c`)
1. Tree traversal and ordering:
- `construct_galaxies()` recursively processes progenitors (`DoneFlag`) and synchronizes FOF-wide readiness (`HaloFlag`) before evolution.
- Once ready, it builds all subhalo galaxy blocks for the FOF and then evolves them together.

2. Progenitor integration:
- `find_most_massive_progenitor()` identifies the occupied progenitor branch to inherit central state.
- `copy_galaxies_from_progenitors()` copies progenitor galaxies, updates host-halo properties, transitions `Type` (0/1/2/3), sets/updates merger times (`MergTime`), and creates a new central galaxy when needed.
- `set_galaxy_centrals()` is called per subhalo block and sets `CentralGal` for that block.

3. Evolution loop:
- `evolve_galaxies()` computes one FOF central and one `infall_recipe()` call per snapshot interval.
- For each substep (`STEPS`):
- `apply_physical_processes()` applies infall/reincorporation/stripping/cooling/star formation.
- `handle_mergers()` updates `MergTime`, applies merger/disruption criteria, and executes mergers or disruptions.

4. Finalization:
- `update_galaxy_properties()` normalizes accumulated rates by `deltaT`, updates satellite baryon summaries, preserves merger lineage metadata (`mergeType`, `mergeIntoID`, `mergeIntoSnapNum`), and copies surviving galaxies to persistent storage.

## Algorithm Map: Mimic (`src/core/build_model.c`)
1. Tree traversal and ordering:
- `build_halo_tree()` preserves SAGE-like recursion with `DoneFlag` and `HaloFlag`, plus a recursion-depth guard.
- It joins all subhalos in an FOF, then sets central references for the whole FOF, then processes evolution.

2. Progenitor integration:
- `find_most_massive_progenitor()` mirrors SAGE logic with `NHalos`.
- `copy_progenitor_halos()` copies prior halos into `FoFWorkspace`, deep-copies `galaxy` payloads, resets snapshot-scoped accumulators, updates Type transitions, and initializes new centrals via `init_halo()`.
- `set_halo_centrals()` assigns one Type 0 central to every object in the FOF (`CentralHalo`, `UniqueCentralGalaxyID`).

3. Evolution loop (modular):
- `process_halo_evolution()` builds `ModuleContext`, then runs:
- `pre_timestep` once,
- `phase_1` + `phase_2` for each substep (`SubSteps` from config),
- `post_timestep` once.
- `execute_phase()` executes all `process_full_halo` modules first, then all `process_by_galaxy` modules (galaxy-major), regardless of list interleaving.

4. Finalization:
- `update_halo_properties()` copies non-`Type 3` objects into `ProcessedHalos`; merged entries are dropped (and galaxy memory freed).

## Side-by-Side Mapping
- SAGE `construct_galaxies()` ↔ Mimic `build_halo_tree()`: same traversal intent, Mimic adds recursion depth guard.
- SAGE `join_galaxies_of_progenitors()` ↔ Mimic `join_progenitor_halos()`: same role, but central assignment timing changed.
- SAGE `evolve_galaxies()` ↔ Mimic `process_halo_evolution()`: hardcoded physics loop replaced by phase/module execution.
- SAGE `update_galaxy_properties()` ↔ Mimic `update_halo_properties()`: Mimic has much thinner finalization; merger lineage bookkeeping moved/removed.

## Findings and Expected Effects

### Finding 1 (High): Merger-triggered post-merger physics path is not equivalent to SAGE
Evidence:
- SAGE executes BH growth and collisional starburst directly inside merger handling (`model_mergers.c` lines 128-139).
- Mimic phase config places merger detection/transfer in full-halo modules, then quasar/starburst in by-galaxy modules (`input/millennium.yaml` lines 64-70).
- `execute_phase()` runs all full-halo modules before by-galaxy modules (`src/core/module_registry.c` lines 327-356), and by-galaxy pass skips `Type 3` (`src/core/module_registry.c` lines 358-360).
- `sage_merge_galaxies` marks merging satellites as `Type 3` (`src/modules/sage_merge_galaxies.c` line 117).
- Quasar/starburst modules depend on trigger flags (`src/modules/sage_quasar_mode.c` lines 152-168; `src/modules/sage_collisional_starburst.c` lines 115-125), but those are on satellites that have just become `Type 3`.
- `sage_quasar_mode` also clears triggers (`src/modules/sage_quasar_mode.c` lines 170-173), which can suppress downstream consumers.

Expected effect:
- Merger-triggered black-hole growth and merger-triggered starburst channels are likely under-applied or skipped relative to SAGE.
- This will bias BH mass growth, burst-driven star formation, and metal/feedback pathways.

### Finding 2 (High): `Type 2` eligibility differs from SAGE merger/disruption criterion
Evidence:
- Mimic marks all `Type 2` satellites eligible regardless of mass-ratio criterion (`src/modules/sage_update_merger_time.c` lines 113-115).
- SAGE only uses zero-baryon or ratio-threshold criteria (`sage-code/sage/core_build_model.c` lines 516-519).

Expected effect:
- Shorter effective Type 2 survival times in Mimic.
- Earlier disruption/merging of orphans, potentially elevating ICS and changing central growth histories.

### Finding 3 (High): Central-reference semantics changed (per-subhalo vs FOF-global)
Evidence:
- SAGE sets central references per subhalo block (`set_galaxy_centrals`, `core_build_model.c` lines 349-364), then Type 2 merger targets can be subhalo centrals (`core_build_model.c` lines 521-526).
- Mimic sets one FOF-wide Type 0 central for all entries (`set_halo_centrals`, `src/core/build_model.c` lines 371-402) and does this once per FOF (`src/core/build_model.c` lines 105-111).

Expected effect:
- Type 2 objects that would merge into a satellite-subhalo central in SAGE are redirected to FOF-central-centric behavior in Mimic.
- This changes where mass/feedback is deposited and can alter merger tree baryon flow topology.

### Finding 4 (Medium-High): Merger-event time stamping differs from SAGE midpoint formulation
Evidence:
- SAGE merger event time uses substep midpoint between progenitor and current snapshot (`sage-code/sage/core_build_model.c` lines 542-543).
- Mimic context sets `ctx->time = Age[current_snap]` (`src/core/build_model.c` line 497), and `sage_merge_galaxies` writes merger times from `ctx->time` (`src/modules/sage_merge_galaxies.c` lines 106, 113).

Expected effect:
- `TimeOfLastMajorMerger` and `TimeOfLastMinorMerger` can be systematically shifted versus SAGE and lose substep-midpoint timing fidelity.

### Finding 5 (Medium): Type 0→Type 2 transition MergTime handling differs
Evidence:
- SAGE explicitly forces immediate merge path in this transition case (`MergTime = 0.0`) when unset or Type 0→2 (`sage-code/sage/core_build_model.c` lines 305-308).
- Mimic Type 2 branch does not force `MergTime = 0.0` (`src/core/build_model.c` lines 319-331); instead merger times can be calculated in module flow if sentinel persists (`src/modules/sage_calculate_merger_timescale.c` lines 74-79).

Expected effect:
- Different transient behavior for recently orphaned former centrals; interaction with Finding 2 can further skew merge/disrupt timing.

### Finding 6 (Medium): Output-side merger lineage instrumentation is no longer SAGE-equivalent
Evidence:
- SAGE output structures and finalization preserve merge metadata (`mergeType`, `mergeIntoID`, `mergeIntoSnapNum`) (`sage-code/sage/types.h` lines 105-108; `sage-code/sage/core_build_model.c` lines 630-634).
- Mimic output struct omits these fields (`src/include/generated/property_defs.h` lines 89-135), and `update_halo_properties` simply filters out `Type 3` (`src/core/build_model.c` lines 463-480).

Expected effect:
- Harder/impossible to do one-to-one lineage-level comparison against classic SAGE merge records from outputs alone.

### Finding 7 (Low-Medium): Virial-mass fallback condition differs at exactly `Mvir == 0`
Evidence:
- SAGE: use catalog `Mvir` for centrals when `Mvir >= 0.0` (`sage-code/sage/model_misc.c` lines 214-215).
- Mimic: uses catalog `Mvir` only when `Mvir > 0.0`, else falls back to `Len * PartMass` (`src/core/virial.c` lines 96-101).

Expected effect:
- For centrals with exact zero catalog `Mvir`, Mimic may assign larger masses than SAGE, affecting infall/cooling/merger-related calculations.
- Magnitude depends on how often this tree edge case occurs.

### Finding 8 (Low): Public execution-order documentation does not match implementation
Evidence:
- Header claims by-galaxy modules execute before full-halo modules (`src/core/module_registry.h` lines 83-89).
- Implementation does full-halo first (`src/core/module_registry.c` lines 327-356).

Expected effect:
- Misleading integration expectations and easier pipeline misconfiguration.

## What Still Looks Faithful to SAGE
- DFS progenitor-first traversal and FOF synchronization (`DoneFlag`/`HaloFlag`) remain aligned.
- Occupied-progenitor branch selection logic is structurally preserved.
- Core Type transitions (0/1/2/3 states) are still central to copy/join flow.
- Time-substep interpolation for `currentMvir` is mathematically consistent with SAGE form.

## Suggested Next Validation Work (for future chat)
1. Build a tiny deterministic tree fixture (2-3 FOFs, explicit Type1/Type2 transitions) and log per-substep decisions in both codes.
2. Compare event-level counts and timing: mergers, disruptions, Type2 lifetimes, ICS growth, BH growth.
3. Trace one known merger case to verify whether merger-triggered quasar/starburst occurs in Mimic under current phase/mode execution.
4. If strict SAGE parity is the goal, prioritize Findings 1-4 before tuning lower-level physics constants.

## Notes on Confidence
- High confidence on structural differences directly evidenced by line-level behavior.
- Medium confidence where impact size depends on data distribution (for example `Mvir == 0` occurrence frequency).
- If you want, I can run a concrete parity trace in the next chat using one small tree and produce per-step state tables.
