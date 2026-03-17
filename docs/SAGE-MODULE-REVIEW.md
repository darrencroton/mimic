# SAGE Module Organisation Review

**Date:** 2026-03-16 (updated 2026-03-17)
**Scope:** All SAGE physics modules in `src/modules/sage_*/`, `src/modules/_shared/`, and the Millennium pipeline configuration `input/millennium.yaml`
**Question:** What is the most sensible and efficient way to organise and group the SAGE modules in Mimic? Should any be moved, combined, split, or restructured? What are the consequences?
**Baseline:** Original SAGE codebase at `sage-code/sage-model/core_build_model.c`

---

## 1. Executive Summary

**Recommendation tier: targeted presentation reorganisation, high confidence.** Keep Mimic's four-phase runtime architecture and the scientifically meaningful module splits. Remove presentation-only splits and misleading names. The current implementation is largely faithful to SAGE in execution, but not yet the clearest possible presentation of the SAGE physics story to a researcher reading the YAML and module tree alone.

Key decisions:

- **Keep SF and SN as separately swappable prescriptions; rename the apply step.** `sage_calculate_star_formation` and `sage_calculate_supernova_feedback` represent two independently swappable physics models and must remain as separate YAML entries. `sage_update_star_formation_supernova` is an infrastructure apply step — not a physics prescription — and should be renamed `sage_apply_stellar_feedback` to make its role clear. Either SF or SN may be removed from the YAML independently; the absent module's transport fields remain at their init value of 0.0, and the apply step commits zeros correctly.
- **Keep the cooling chain split and visible.** Cooling, AGN suppression, and cooling application are a real three-step scientific story because `sage_radio_mode_heating` modifies `CoolingGas` between calculation and commitment. This split is scientifically meaningful and should remain (`src/modules/sage_calculate_cooling/README.md:39-42`, `src/modules/sage_radio_mode_heating/sage_radio_mode_heating.c:127-137`).
- **Remove housekeeping from the visible pipeline.** `sage_clear_disk_instability_triggers` zeroing a scratch field is not a SAGE physics stage. It should not appear in the YAML alongside real physics.
- **Rename modules to use physics-stage names.** Several names describe implementation mechanics rather than the science a researcher would recognise.
- **Add a dependency enforcement framework.** Module inter-dependencies and ordering constraints are currently unenforced. Promote `phase_has_module()` from a module-local static to a public framework API (`module_registry.h`) and add explicit dependency checks to each module's `init()`. This prevents silent physics loss from misconfigured pipelines (see §7).
- **Tighten `_shared/`.** `sage_merger_ops.h` has a single consumer, violating the directory's stated policy. `merger_physics.h` mixes two unrelated concerns and should be split. `sage_events.h` is a contract schema, not a physics utility, and should be reclassified.
- **Fix 8 specific metadata drift bugs** identified in `module_info.yaml` files — these are correctness issues, not style preferences.

---

## 2. Background: SAGE Physics Baseline

Mimic exposes SAGE through a four-phase runtime module system: `pre_timestep`, `phase_1`, `phase_2`, and `post_timestep`. `pre_timestep` runs once before substeps, `phase_1` and `phase_2` run inside the substep loop, and `post_timestep` runs once after the loop (`src/core/build_model.c:540-550`, `src/core/build_model.c:581-599`).

### Pre-loop setup (once per FOF halo, before substeps)

| Step | SAGE function | Source | Description |
|------|---------------|--------|-------------|
| S1 | `get_virial_mass/radius/velocity()` | `model_misc.c:128,151,138` | Refresh virial properties if halo grew |
| S2 | `get_disk_radius()` | `model_misc.c:90` | Compute disk scale radius from halo spin |
| S3 | `estimate_merging_time()` | `model_mergers.c:14` | Assign merger timescale for new Type 1 satellites |
| S4 | `init_galaxy()` | `model_misc.c:11` | Initialise fresh central if no progenitors |
| S5 | `infall_recipe()` | `model_infall.c:13` | FOF baryon budget; calls `do_reionization()` for central |

### Per-substep loop (repeated `STEPS=10` times)

**Central-only pass:**

| Step | SAGE function | Source | Description |
|------|---------------|--------|-------------|
| S6 | `add_infall_to_hot()` | `model_infall.c:178` | Distribute `infallingGas/STEPS` to hot reservoir |
| S7 | `reincorporate_gas()` | `model_reincorporation.c:12` | Return ejected gas to hot gas in massive halos |
| S8 | `strip_from_satellite()` | `model_infall.c:91` | Strip hot gas from Type 1 satellites; calls `do_reionization()` |

**Per-galaxy pass (all active galaxies):**

| Step | SAGE function | Source | Description |
|------|---------------|--------|-------------|
| S9 | `cooling_recipe()` | `model_cooling_heating.c:14` | Compute cooling; calls `do_AGN_heating()` internally |
| S10 | `cool_gas_onto_galaxy()` | `model_cooling_heating.c:164` | Apply computed cooling: hot → cold gas |
| S11 | `starformation_and_feedback()` | `model_starformation_and_feedback.c:13` | SF + SN feedback + disk instability + metals |
| S11a | ↳ `update_from_star_formation()` | `:80` | Form stars; metallicity recalculated before feedback |
| S11b | ↳ `update_from_feedback()` | `:86` | SN reheating and ejection |
| S11c | ↳ `check_disk_instability()` | `model_disk_instability.c:13` | Disk stability → BH growth → quasar wind → burst SF |
| S11d | ↳ `add_metals_to_galaxy()` | `:93` | Metal enrichment |

**End-of-substep merger/disruption pass:**

| Step | SAGE function | Source | Description |
|------|---------------|--------|-------------|
| S12a | `disrupt_satellite_to_ICS()` | `model_mergers.c:297` | Satellites satisfying disruption criterion → ICS |
| S12b | `deal_with_galaxy_merger()` | `model_mergers.c:50` | Merger coalescence: BH growth, quasar wind, burst SF, bulge |

**Post-loop normalization (after all substeps):**

| Step | SAGE function | Source | Description |
|------|---------------|--------|-------------|
| S13 | normalization | `core_build_model.c:410,422,427` | Normalize `Cooling`, `Heating`, `OutflowRate`; accumulate `TotalSatelliteBaryons` |

### Key structural observations

1. **Infall is split across the loop boundary.** `infall_recipe()` runs once; `add_infall_to_hot()` distributes across substeps. This is intentional and must be preserved.
2. **`do_reionization()` is called twice** — once inside `infall_recipe()` for the central (`model_infall.c:43`) and once inside `strip_from_satellite()` for each satellite (`model_infall.c:95`). Mimic's split (`sage_reionization` pre-timestep, satellite stripping consuming stored `HaloBaryonFraction`) requires parity verification — see §11.1.
3. **Cooling is calculate-then-apply**, with AGN radio-mode heating embedded inside `cooling_recipe()` in SAGE. Mimic exposes this as three visible stages — a genuine improvement.
4. **SF and SN feedback are one bundled call in SAGE.** Mimic deliberately exposes these as two separately swappable prescriptions plus one infrastructure apply step, which is an extension of SAGE rather than a departure from it.
5. **Mergers run after all galaxies complete the SF/cooling substep** — SAGE explicitly separates these passes.
6. **Post-loop normalization is not yet implemented** in Mimic's `post_timestep` phase — see §11.3.

The SAGE physics story a researcher should read from the YAML is:

> baryon supply → cooling → star formation and feedback → morphological change → hierarchy changes

---

## 3. Design Principle

All consolidation and splitting decisions in this review follow two consistent criteria. A split is justified when **either** applies:

> 1. **Intervention criterion:** Another visible physics module modifies the intermediate quantity before it is committed.
> 2. **Swappability criterion:** The step represents a distinct scientific prescription a researcher might want to independently replace.

If neither criterion applies, the split exposes implementation transport fields as first-class physics stages — creating visual noise in the YAML and implying false independence. Applied consistently:

| Pipeline chain | Intervention? | Independently swappable? | Keep split? |
|---|---|---|---|
| `calculate_cooling_budget` → `radio_mode_heating` → `apply_cooling` | Yes — `radio_mode_heating` modifies `CoolingGas` | Yes | **Keep** |
| `star_formation` → `supernova_feedback` | No | **Yes — distinct physics prescriptions** | **Keep** |
| SF/SN calculate steps → `apply_stellar_feedback` | No | No (apply is infrastructure) | Apply kept but named as infrastructure |
| `prepare_infall_budget` → `apply_infall` | No | No (phase boundary) | **Keep** (phase-lifecycle reason) |

---

## 4. Current State Assessment

### What already works well

- The **core phase model** is clean and matches SAGE's structural division: setup before substeps, baryonic physics within substeps, merger/disruption resolution at end of substep (`src/core/build_model.c:540-550`, `src/core/build_model.c:581-599`).
- The **YAML groupings** already tell most of the story. Inline comment groups (Cooling, Star Formation, etc.) make the flow visible without opening any source file (`input/millennium.yaml:41-67`).
- The **cooling calculate → modify → apply chain** is a genuine improvement over SAGE's embedded `do_AGN_heating()`. The visible AGN slot makes model replacement trivial (`src/modules/sage_calculate_cooling/README.md:39-42`).
- The **SF/SN module split** correctly maps to Mimic's swappability goal. Keeping SF and SN as separate modules allows a researcher to swap the SF rate law, swap the SN feedback model, or disable either independently.
- The **dual-use modules** are scientifically correct. `sage_quasar_mode` and `sage_collisional_starburst` spanning disk-instability and merger channels maps directly to SAGE physics, where `check_disk_instability()` and `collisional_starburst_recipe()` serve both trigger types.
- The **infall split** faithfully mirrors SAGE's loop structure: budget computed once pre-substep, applied incrementally within substeps.

### What is unclear or misleading

- **`sage_update_star_formation_supernova`** uses an implementation name that gives no indication of its architectural role. Alongside `sage_calculate_star_formation` and `sage_calculate_supernova_feedback` in the YAML, a researcher cannot distinguish it from a third physics prescription. It is an infrastructure apply step and should be named accordingly.
- **No dependency or ordering enforcement exists.** If `sage_apply_stellar_feedback` is accidentally placed before SF and SN in the YAML, it silently applies stale values from the previous substep. If the event producer `sage_resolve_mergers_and_disruption` is removed while its event consumers remain, those consumers are silently never called. The framework currently validates only module name registration and processing mode support.
- **`sage_clear_disk_instability_triggers`** zeroing a scratch field is visible alongside real physics stages — a researcher has to know this is housekeeping, not a scientific step.
- **Several names describe implementation mechanics** rather than the science. `sage_handle_mergers_immediate` is architecturally precise but obscures the user-facing step ("resolve mergers and disruption"). `sage_calculate_infall` is not a pure calculator — it also consolidates satellite reservoirs (`src/modules/sage_calculate_infall/sage_calculate_infall.c:62-121`).
- **`sage_calculate_merger_timescale` understates its scope.** It also resets `MergTime` to the sentinel for Type 0 promotions and forces `MergTime=0` for unresolved Type 2 orphans (`src/modules/sage_calculate_merger_timescale/sage_calculate_merger_timescale.c:56-88`). This is merger state management, not just timescale calculation.
- **`phase_2` is not self-explanatory from the YAML alone.** A researcher cannot infer that `process_per_event` consumers run when events are emitted, not at a later fixed position (`input/millennium.yaml:63-67`, `src/core/module_registry.c:393-452`).
- **`sage_collisional_starburst` has hidden cross-phase coupling.** At init time it queries whether `sage_disk_instability` is in phase_1 and whether `sage_quasar_mode` is in phase_2, and silently changes its own behaviour based on the answer (`src/modules/sage_collisional_starburst/sage_collisional_starburst.c:135-140`). Removing `sage_quasar_mode` from phase_2 silently removes quasar wind physics from the post-merger disk instability path. The `phase_has_module()` function used here is a module-local static and should be replaced with the framework API described in §7.

---

## 5. Recommended Module Names

Use **physics-stage names** for every user-facing SAGE module. Reserve `calculate_`, `apply_`, `handle_`, and `clear_` prefixes for modules that are genuinely internal or helper-like.

| Current name | Recommended name | Rationale |
|---|---|---|
| `sage_calculate_infall` | `sage_prepare_infall_budget` | Consolidates satellite reservoirs AND computes the budget — not a pure calculator (`sage_calculate_infall.c:62-121`) |
| `sage_add_infall` | `sage_apply_infall` | This is the commit step: distributes the budget to the hot reservoir over substeps |
| `sage_update_disk_radius` | `sage_set_disk_scale_radius` | "Set" describes the user-facing purpose; avoids ambiguity about which galaxy types are processed |
| `sage_calculate_cooling` | `sage_calculate_cooling_budget` | Makes the role explicit; pairs naturally with the apply naming |
| `sage_add_cooling` | `sage_apply_cooling` | This is the commit step: `CoolingGas` → `ColdGas` (`sage_add_cooling.c:23-48`) |
| `sage_calculate_star_formation` | `sage_star_formation` | Physics-stage name; this is a swappable SF rate prescription |
| `sage_calculate_supernova_feedback` | `sage_supernova_feedback` | Physics-stage name; this is a swappable SN feedback prescription |
| `sage_update_star_formation_supernova` | `sage_apply_stellar_feedback` | Infrastructure apply step: commits SF and SN scratch fields to galaxy reservoirs; not a swappable prescription |
| `sage_collisional_starburst` | `sage_starburst_feedback` | Under-describes scope: handles disk-instability-triggered, merger-triggered, and post-minor-merger bursts (`sage_collisional_starburst.c:8-19`) |
| `sage_calculate_merger_timescale` | `sage_initialise_merger_clock` | Communicates lifecycle purpose; the module also handles Type 0 reset and Type 2 force-merge (`sage_calculate_merger_timescale.c:56-88`) |
| `sage_handle_mergers_immediate` | `sage_resolve_mergers_and_disruption` | This is the science stage a researcher expects to see in the flow (`sage_handle_mergers_immediate.c:5-10`) |
| `sage_clear_disk_instability_triggers` | *(remove from pipeline — see §6)* | Not a physics stage |

---

## 6. Recommendations: Consolidation and Splitting

### Keep split: SF and SN as independently swappable prescriptions

**Recommendation: Keep `sage_star_formation` and `sage_supernova_feedback` as separate YAML entries.** These modules represent two distinct scientific prescriptions — the SF rate law and the SN feedback model — that a researcher may independently want to swap, tune, or disable. Mimic's central value proposition is YAML-level physics configurability; consolidating them would remove exactly that capability for the two most likely candidates for model comparison.

**Either module may be removed from the YAML independently:**
- Remove `sage_star_formation`: `NewStellarMass` retains its init value of 0.0. `sage_supernova_feedback` reads zero new stellar mass and produces zero feedback. `sage_apply_stellar_feedback` commits zeros. The galaxy evolves with no SF or SN — correct and well-defined.
- Remove `sage_supernova_feedback`: `NewStellarMass` is written by SF as normal. SN scratch fields retain 0.0. `sage_apply_stellar_feedback` commits SF results without SN feedback — stars form, but no reheating or ejection occurs. Correct and well-defined.
- Remove both: Apply commits all zeros. Correct.

**`sage_apply_stellar_feedback` is infrastructure, not physics.** It contains no swappable model. Its role is to commit the SF and SN scratch fields to galaxy reservoirs at the end of the calculation step — analogous to `sage_apply_cooling` for the cooling chain. It should always be present whenever either prescription is configured, and its README and `module_info.yaml` must document this role explicitly. The dependency framework (§7) enforces the ordering constraint at init time.

**Dependency between SF and SN:** `sage_supernova_feedback` reads `NewStellarMass` written by `sage_star_formation`. If both are configured, SF must precede SN in the YAML. This ordering constraint is enforced at init time via the framework API (§7).

**SAGE parity:** Preserve the internal call order in `sage_apply_stellar_feedback`: apply SF results → metallicity refresh → apply SN transfers. This matches SAGE's `starformation_and_feedback()` internal sequence (`model_starformation_and_feedback.c:79-94`).

**Transport fields:** `NewStellarMass` remains an inter-module transport field — the contract between the SF and SN prescriptions. `SupernovaReheatedMass` and `SupernovaEjectedMass` are inter-module transport fields between SN and apply. All three remain in the global property schema and should be documented as transient transport fields (see §11.4).

### Keep split: cooling chain

**Recommendation: No consolidation.** `sage_radio_mode_heating` is a real physics module with three selectable AGN accretion models that a researcher may want to swap, suppress, or compare. The visible slot between cooling calculation and application is a genuine architectural improvement over SAGE's `do_AGN_heating()` being embedded inside `cooling_recipe()`.

**Rename the endpoints** to `sage_calculate_cooling_budget` and `sage_apply_cooling` to make the calculate-modify-apply pattern explicit.

### Remove from pipeline: `sage_clear_disk_instability_triggers`

**Recommendation: Remove this module from the visible YAML pipeline.** It does nothing except zero `UnstableDiskGasFraction` after the downstream consumers have read it. A researcher should not encounter a "clear trigger" step when reading the SAGE physics flow.

**Alternative:** Either clear `UnstableDiskGasFraction` at the end of `sage_disk_instability`'s own process function (after its consumers have run in the same phase), or handle it as a framework-managed scratch field lifecycle. If the explicit clear is retained for defensive correctness reasons, it should be documented as infrastructure, not listed alongside real physics modules.

**SAGE parity:** No change. The clear still occurs after both downstream consumers, just without surfacing as a YAML entry (`src/modules/sage_clear_disk_instability_triggers/sage_clear_disk_instability_triggers.c:29`).

### Keep split: infall across phase boundary

**Recommendation: No consolidation.** `sage_prepare_infall_budget` runs once in `pre_timestep` and `sage_apply_infall` distributes across substeps in `phase_1`. The `InfallingGas` transport field bridges a genuine phase-lifecycle boundary. Combining would lose the substep distribution and the ordering against `EjectedMass` for negative infall (`sage_add_infall.c:60-89`).

### Keep split: merger clock initialisation and resolution

**Recommendation: No consolidation.** One initialises `MergTime` before substeps; the other consumes it live during `phase_2` substeps. The split is phase-driven and scientifically valid: setup before the substep loop, resolution within it. Rename both as described in §5.

### Address: `sage_collisional_starburst` hidden coupling

**Issue:** At init time, `sage_collisional_starburst` calls a module-local static `phase_has_module()` to check for `sage_disk_instability` in phase_1 and `sage_quasar_mode` in phase_2, silently changing its own behaviour (`sage_collisional_starburst.c:135-140`). A researcher who removes `sage_quasar_mode` from phase_2 while keeping it in phase_1 would silently lose quasar wind physics from the post-merger disk instability path — with no warning.

**This is a SAGE parity requirement.** In original SAGE, `deal_with_galaxy_merger()` calls `check_disk_instability()` directly for minor mergers (`model_mergers.c:277`), which in turn calls `grow_black_hole()` and `quasar_mode_wind()` inline. Mimic cannot replicate this inline call within the event-based architecture without the module knowing what consumers exist downstream.

**Recommendation:** Replace the module-local `phase_has_module()` static with the public framework API (`module_configured_in_phase()` from §7). Add an explicit `WARNING`-level log message in `sage_starburst_feedback_init()` when `sage_disk_instability` is present in phase_1 but `sage_quasar_mode` is absent from phase_2, so the silent physics loss becomes visible. Document the design decision in the module's README.

---

## 7. Dependency Enforcement Framework

### Current state

`phase_has_module()` is a **module-local static function** defined only inside `sage_collisional_starburst.c:57-76`. It works by directly querying the global `MimicConfig` struct's phase arrays (`pre_timestep`, `phase_1`, `phase_2`, `post_timestep` with their `num_*` counts). Any module `init()` can perform the same query — there is nothing architecturally special about this function; it was written ad hoc for one use case and never promoted to a shared utility.

The framework currently validates only two things during `module_system_init()`: module names must be registered, and configured processing modes must be declared as supported by the module. No inter-module dependency or ordering checks exist. This means:

- A badly ordered YAML (e.g., `sage_apply_stellar_feedback` placed before `sage_star_formation`) silently applies stale values from the previous substep.
- Event consumers configured without an event producer (`sage_quasar_mode: process_per_event` with no `sage_resolve_mergers_and_disruption` in the same phase) are silently never called.
- Cross-phase dependencies (e.g., `sage_apply_infall` requires `sage_prepare_infall_budget` in `pre_timestep`) are never checked.

### Proposed public API

Add to `module_registry.h` and implement in `module_registry.c`:

```c
/**
 * @brief Check if a module is configured in a given phase with a specific mode.
 *
 * Intended for use in module init() functions to enforce dependency contracts.
 * All four phases are accessible via MimicConfig: pre_timestep, phase_1,
 * phase_2, post_timestep.
 *
 * @param name         Module name to search for
 * @param phase        Phase config array (e.g., MimicConfig.phase_1)
 * @param num_modules  Number of entries in the phase array
 * @param mode         Processing mode to match
 * @return true if module is found with the given mode, false otherwise
 */
bool module_configured_in_phase(const char *name,
                                 const struct PhaseModuleConfig *phase,
                                 int num_modules,
                                 enum ProcessingMode mode);

/**
 * @brief Check if a module is configured in any phase with any mode.
 */
bool module_configured_anywhere(const char *name);

/**
 * @brief Check if 'first' appears before 'second' in a phase array.
 *
 * Returns false if either module is absent from the phase.
 * Used to enforce ordering constraints between dependent modules.
 */
bool module_precedes_in_phase(const char *first, const char *second,
                               const struct PhaseModuleConfig *phase,
                               int num_modules);
```

Remove the static `phase_has_module()` from `sage_collisional_starburst.c` and replace its calls with `module_configured_in_phase()`. All modules use the public API.

### How modules enforce dependencies

Each module's `init()` function calls these utilities and either:
- Returns non-zero on a hard constraint violation — `module_system_init()` propagates the failure and the program exits before any processing begins, with a clear `ERROR_LOG` message naming the missing or misordered module.
- Calls `WARNING_LOG()` on a soft advisory — the program continues, the researcher is informed.

### All dependency constraints to enforce

| Module | Constraint | Severity | Notes |
|---|---|---|---|
| `sage_supernova_feedback` | `sage_star_formation` precedes it in same phase (if both configured) | ERROR | SN reads `NewStellarMass` written by SF; wrong order applies stale values |
| `sage_apply_stellar_feedback` | Any SF/SN module precedes it in same phase (ordering) | ERROR | Apply must commit freshly computed values, not previous-substep residuals |
| `sage_apply_stellar_feedback` | Neither `sage_star_formation` nor `sage_supernova_feedback` present | WARNING | All fields will be zero; likely a configuration mistake |
| `sage_apply_cooling` | `sage_calculate_cooling_budget` present and preceding in same phase | ERROR | `CoolingGas` would be 0 without it; ordering matters |
| `sage_apply_infall` | `sage_prepare_infall_budget` present in `pre_timestep` | ERROR | `InfallingGas` is 0 without it; cross-phase check against `MimicConfig.pre_timestep` |
| `sage_resolve_mergers_and_disruption` | `sage_initialise_merger_clock` present in `pre_timestep` | WARNING | `MergTime` values from tree load may be stale; not always wrong, but suspicious |
| `sage_quasar_mode` (process_per_event) | `sage_resolve_mergers_and_disruption` present in same phase | ERROR | Event consumers are silently never called without a producer |
| `sage_starburst_feedback` (process_per_event) | `sage_resolve_mergers_and_disruption` present in same phase | ERROR | Same |
| `sage_starburst_feedback` (process_by_galaxy) | `sage_disk_instability` precedes it in same phase | WARNING | Disk instability channel silently inactive without trigger writer |
| `sage_starburst_feedback` | `sage_disk_instability` in phase_1 but `sage_quasar_mode` absent from phase_2 | WARNING | Post-merger disk instability quasar wind silently skipped (SAGE parity loss) |

### Testing dependency enforcement

Each constraint should have a dedicated test. The natural home is `tests/unit/test_module_configuration.c` (already exists) or the relevant module's `_tests/` directory. Each test:

1. Sets up a `MimicConfig` with the bad configuration (the existing pattern in `test_unit_sage_collisional_starburst.c:251-256` provides the template for mock phase config).
2. Calls the module's `init()` function.
3. For ERROR constraints: asserts the return value is non-zero.
4. For WARNING constraints: asserts the return value is zero and verifies the warning was logged (if the logging framework supports capture).

---

## 8. Recommendations: The `_shared/` Directory

### Overall assessment

`_shared/` is conceptually correct but currently under-governed. Its own README defines it as a home for reusable shared calculations used by multiple modules and explicitly says not to create a shared utility when only one module uses it (`src/modules/_shared/README.md:11-16`, `src/modules/_shared/README.md:45-53`). The live contents only partly follow that rule.

### File-by-file decisions

| File | Recommendation | Justification |
|---|---|---|
| `metallicity.h` | **Keep** | Textbook shared utility: small, stateless, header-only, used by 7 modules (`sage_apply_infall`, `sage_calculate_cooling_budget`, `sage_apply_cooling`, `sage_radio_mode_heating`, `sage_reincorporation`, `sage_satellite_stripping`, `sage_apply_stellar_feedback`) |
| `time_parity.h` | **Keep** | High-value shared runtime/SAGE-parity support used by 6 modules across cooling, SF, mergers, and starbursts |
| `central_link.h` | **Keep** | Shared by two merger modules; encodes SAGE-specific target-resolution rules used by both |
| `sage_disk_instability_physics.h` | **Keep while two consumers exist** | Used by `sage_disk_instability` and `sage_starburst_feedback`; justified by the post-merger follow-up. If the follow-up is ever removed, move to `sage_disk_instability/` as a private header |
| `merger_physics.h` | **Split into two files** | Currently mixes BH growth/quasar-wind helpers (`mimic_apply_black_hole_growth`, `mimic_apply_quasar_mode_wind`) with collisional starburst plus SN/recycling/enrichment logic (`mimic_apply_collisional_starburst`). These are two distinct subsystems. Split into `sage_agn_physics.h` and `sage_starburst_physics.h` |
| `sage_events.h` | **Reclassify as a contract header** | This is an event payload schema linking one producer to multiple consumers — not a physics utility. Rename to `sage_merger_event_contract.h` or move to a `contracts/` subdirectory to signal its role |
| `sage_merger_ops.h` | **Move out of `_shared/`** | Single live consumer (`sage_handle_mergers_immediate`). The `_shared/README.md` explicitly says single-module logic should stay local (`_shared/README.md:50-53`, `sage_handle_mergers_immediate.c:17`). Move to `sage_resolve_mergers_and_disruption/` as a module-private header |

### What a well-organised `_shared/` should look like

The cleanest structure separates three concepts currently mixed together:

```
src/modules/_shared/
  # Pure reusable utilities (stateless, widely used)
  metallicity.h
  time_parity.h

  # Shared SAGE physics kernels (algorithms used by 2+ modules)
  sage_agn_physics.h           (renamed from merger_physics.h, BH/quasar half)
  sage_starburst_physics.h     (renamed from merger_physics.h, burst/SN half)
  sage_disk_instability_physics.h

  # Shared contracts (event schemas, targeting protocols)
  sage_merger_event_contract.h (renamed from sage_events.h)
  central_link.h
```

If subdirectories are not desired, the same result is achieved by file naming alone and updating `_shared/README.md` to document the distinction explicitly.

---

## 9. Recommended YAML After Changes

After the recommended renames and restructuring, the pipeline should read:

```yaml
modules:
  pre_timestep:
    # Setup and snapshot budgets
    - sage_reionization:              process_full_halo
    - sage_prepare_infall_budget:     process_full_halo
    - sage_set_disk_scale_radius:     process_full_halo
    - sage_initialise_merger_clock:   process_full_halo

  phase_1:
    # Baryon supply
    - sage_apply_infall:              process_full_halo
    - sage_reincorporation:           process_full_halo
    - sage_satellite_stripping:       process_full_halo

    # Cooling and AGN suppression
    - sage_calculate_cooling_budget:  process_by_galaxy
    - sage_radio_mode_heating:        process_by_galaxy
    - sage_apply_cooling:             process_by_galaxy

    # Star formation and supernova feedback
    # sage_star_formation and sage_supernova_feedback are each independently optional.
    # Removing either leaves its transport fields at 0.0; sage_apply_stellar_feedback
    # commits zeros, which is correct. sage_apply_stellar_feedback is required
    # whenever either prescription is active; see §7 for enforced ordering constraints.
    - sage_star_formation:            process_by_galaxy   # prescription: SF rate law
    - sage_supernova_feedback:        process_by_galaxy   # prescription: SN feedback model (optional)
    - sage_apply_stellar_feedback:    process_by_galaxy   # infrastructure: commit SF/SN results

    # Disk instability
    - sage_disk_instability:          process_by_galaxy
    - sage_quasar_mode:               process_by_galaxy
    - sage_starburst_feedback:        process_by_galaxy

  phase_2:
    # Mergers and disruption — runs after all galaxy physics for the current substep
    # sage_resolve_mergers_and_disruption emits SAGE_EVENT_MERGER for downstream consumers
    - sage_resolve_mergers_and_disruption: process_full_halo
    - sage_quasar_mode:               process_per_event
    - sage_starburst_feedback:        process_per_event

  post_timestep:
    - sage_finalise_outputs:          process_full_halo   # normalize Cooling, Heating, OutflowRate
```

The physics of SAGE is visible in the YAML without opening any source file.

---

## 10. Consequences and Trade-offs

### Three SF/SN modules with dependency enforcement (recommended)

**Benefits:**
- SF and SN prescriptions are independently swappable via YAML — the tool's core design goal is preserved.
- Removing either prescription passes zeros through cleanly; the pipeline remains valid.
- Unit tests remain fine-grained: SF and SN can be tested in isolation, and `sage_apply_stellar_feedback` can be tested with controlled input values for each field.
- The pattern is structurally consistent with the cooling chain: two calculate steps (SF, SN) feed one apply step, just as `calculate_cooling_budget` and `radio_mode_heating` feed `apply_cooling`.
- The dependency framework (§7) makes the ordering constraint explicit and enforced, removing the silent-breakage risk.

**Costs:**
- Three YAML entries for what is conceptually one physics bundle. The YAML comments and the `sage_apply_stellar_feedback` name mitigate this, but the apply step is still visible.
- Three module directories, three `module_info.yaml` files, three README surfaces to keep in sync.
- Without the dependency enforcement framework, a user who deletes `sage_apply_stellar_feedback` from the YAML gets silent wrong output. The dependency framework (§7) is therefore a **required companion** to this approach, not an optional improvement.

**Assessment:** The three-module structure is the right design for a swappable physics platform. The apply-step visibility problem is a documentation and enforcement problem, not an architectural one. It is solved by the dependency framework and clear naming.

### If Mimic consolidated the SF/SN triple (not recommended)

**Benefits:**
- One YAML entry for one conceptual SAGE physics step.
- Three transport properties could become module-private scratch, reducing the global property surface.
- Fewer metadata declarations, fewer documentation surfaces.

**Costs:**
- YAML-level swappability is gone. To run SF without SN, a researcher must edit C source code or add a conditional parameter — not remove a YAML entry.
- "Swap the SF rate law" becomes "fork the entire SF+SN+apply module and modify one function inside it."
- Inconsistent with the rest of Mimic's architecture — every other physics prescription (cooling, AGN, reincorporation, reionization) is a separate module.

**Assessment:** Consolidation trades the tool's primary feature for visual tidiness. This is the wrong trade-off for Mimic.

---

## 11. Open Questions and Architectural Gaps

### 11.1 `sage_reionization` satellite parity

In original SAGE, `do_reionization()` is called in two places: inside `infall_recipe()` for the central (`model_infall.c:43`) and inside `strip_from_satellite()` for each Type 1 satellite (`model_infall.c:95`). In Mimic, `sage_reionization` runs in `pre_timestep` and computes `HaloBaryonFraction` only for the Type 0 central (`sage_reionization.c:117,126`). `sage_satellite_stripping` then reads that stored value (`sage_satellite_stripping.c:79`).

**Open question:** Does `do_reionization()` in SAGE use FOF-level properties (same for all members of the group) or galaxy-local properties? If FOF-level, the Mimic split is parity-safe. If satellite-local, the stored central value is incorrect for stripping, and reionization logic should either run `process_by_galaxy` or move back into the consuming modules.

**Risk:** Scientific — output galaxy properties could diverge from SAGE under certain reionization-affected conditions. Verify against SAGE output before relying on the current split.

### 11.2 `sage_starburst_feedback` pipeline coupling contract

When `sage_quasar_mode` is absent from phase_2 but `sage_disk_instability` is present in phase_1, the post-merger disk instability path silently skips quasar wind physics (`sage_collisional_starburst.c:138`). The unit tests codify this behaviour (`test_unit_sage_collisional_starburst.c:929, 972`), but it is not visible from the YAML.

This is addressed by the dependency framework (§7): the module-local `phase_has_module()` static is replaced with `module_configured_in_phase()` from the public API, and the dangerous combination triggers an explicit `WARNING_LOG` at init time. The unit tests for this combination should be updated to assert the warning is emitted.

### 11.3 `post_timestep` not yet implemented

SAGE normalizes `Cooling`, `Heating`, and `OutflowRate` after the substep loop and accumulates `TotalSatelliteBaryons` (`core_build_model.c:410,422,427`). Mimic's `post_timestep` phase is currently empty. These are output-field correctness issues, not galaxy-physics correctness issues, but output fields will disagree with SAGE until `sage_finalise_outputs` (or equivalent) is implemented.

### 11.4 Transient transport fields as global model properties

`InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, `Rcool`, `CoolingLambda`, and `UnstableDiskGasFraction` appear in the global property schema as ordinary model properties, but their main role is inter-module communication (`src/modules/model_properties.yaml:29-49`, `:121-130`, `:246-317`, `:397-406`). With SF and SN kept as separate modules, all of these remain in the global schema. All should be documented explicitly as transient inter-module transport fields in the property metadata — for example with a `role: transport` annotation and a description of the producer and consumer — so it is clear they are not persistent output properties.

---

## 12. Metadata Drift Bugs

These should be fixed regardless of any larger reorganisation. All are correctness problems in the documentation layer.

1. **`sage_disk_instability/module_info.yaml` declares `Vvir`** but the actual physics helper uses `Vmax` in the stability criterion (`src/modules/sage_disk_instability/module_info.yaml:7-18`, `src/modules/_shared/sage_disk_instability_physics.h:27-29`).
2. **`sage_calculate_cooling/module_info.yaml` under-declares inputs and outputs.** The module reads `Vvir` and `Rvir` and writes `CoolingLambda`, but those are missing from the metadata (`src/modules/sage_calculate_cooling/module_info.yaml:11-17`, `src/modules/sage_calculate_cooling/sage_calculate_cooling.c:39-57, 129-136`).
3. **`sage_quasar_mode/module_info.yaml` omits `Vvir`** and `QuasarModeBHaccretionMass` from its property declarations (`src/modules/sage_quasar_mode/module_info.yaml:7-19`, `src/modules/_shared/merger_physics.h:44-51, 82-91`).
4. **Merger modules omit `CentralHalo`.** Both `sage_calculate_merger_timescale` and `sage_handle_mergers_immediate` depend on the central-link logic that resolves `CentralHalo` (`src/modules/sage_calculate_merger_timescale/module_info.yaml:7-18`, `src/modules/_shared/central_link.h:23-49`).
5. **`sage_update_disk_radius` documentation is internally inconsistent.** `module_info.yaml` says Type 0/1; the source header says Type 1/2; the implementation skips `Type >= 2`, so live behaviour is Type 0/1 only (`src/modules/sage_update_disk_radius/module_info.yaml:2-14`, `src/modules/sage_update_disk_radius/sage_update_disk_radius.c:5-8, 90-92`).
6. **`docs/USER-GUIDE.md` shows a misleading phase_1 order.** The SAGE example visually places `sage_satellite_stripping` after cooling and SF, but runtime always executes `process_full_halo` modules before `process_by_galaxy` modules (`docs/USER-GUIDE.md:171-193`, `src/core/module_registry.c:535-597`).
7. **`_shared/module_info.yaml` contains a stale path.** It points readers to `src/modules/shared/README.md`, but the live path is `_shared/README.md` (`src/modules/_shared/module_info.yaml:20`).
8. **Transient transport fields are not documented as transport fields** in `model_properties.yaml` — see §11.4.

---

## 13. Recommended Action Plan

### Must-do

**Renaming and restructuring:**

1. **Rename the modules** listed in §5. The highest-priority renames are those that actively mislead: `sage_calculate_infall` (not a pure calculator), `sage_handle_mergers_immediate` (obscures the physics step), `sage_collisional_starburst` (understates scope), and `sage_update_star_formation_supernova` (implementation name, not architectural role).
2. **Rename `sage_update_star_formation_supernova` → `sage_apply_stellar_feedback`** and update its `module_info.yaml` and README to document explicitly that it is an infrastructure apply step, not a swappable physics prescription. State clearly that it must always follow SF and/or SN modules in the pipeline.
3. **Remove `sage_clear_disk_instability_triggers` from the visible YAML pipeline.** Handle the scratch field clear inside the existing module lifecycle or as infrastructure.

**Dependency enforcement framework (§7):**

4. **Implement the public API** in `module_registry.h` / `module_registry.c`: `module_configured_in_phase()`, `module_configured_anywhere()`, `module_precedes_in_phase()`. These are thin wrappers over `MimicConfig` phase array scans.
5. **Add dependency checks to each module's `init()`** per the constraint table in §7. Each check either returns non-zero (hard ERROR) or emits a `WARNING_LOG` (soft advisory), matching the severity column.
6. **Replace `sage_collisional_starburst`'s module-local `phase_has_module()` static** with calls to the public API. Update the init logic to use `WARNING_LOG` for the detected dangerous combination.
7. **Write dependency enforcement tests** for each constraint in the §7 table, in `tests/unit/test_module_configuration.c`. Follow the mock-config pattern in `test_unit_sage_collisional_starburst.c:251-256`.

**`_shared/` cleanup:**

8. **Move `sage_merger_ops.h` out of `_shared/`** into `sage_resolve_mergers_and_disruption/` as a private header. Single consumer violates the directory's stated policy.
9. **Split `merger_physics.h`** into `sage_agn_physics.h` (BH growth and quasar-wind helpers) and `sage_starburst_physics.h` (starburst and SN/recycling/enrichment). Update all consumers.

**Metadata correctness:**

10. **Fix all 8 metadata drift bugs** in §12. These are correctness errors, not style preferences.

**Documentation:**

11. **Update `docs/USER-GUIDE.md`:** correct the phase_1 order bug (§12 bug #6); update the pipeline walkthrough to show the new module names; document the SF/SN optional/required pattern and `sage_apply_stellar_feedback` infrastructure role; add a note explaining `process_per_event` execution semantics.
12. **Update each renamed module's README** to reflect its new name, role, and any dependency contracts it enforces at init time.
13. **Update `input/millennium.yaml`** to use new module names and add inline comments as shown in §9.

### Nice-to-have

1. **Implement `sage_finalise_outputs`** in `post_timestep` to close the SAGE output-normalization gap (§11.3).
2. **Verify `sage_reionization` satellite parity** against SAGE scientific output (§11.1). If a gap is confirmed, restructure the module or move reionization logic into the consuming modules.
3. **Reclassify transient transport fields** in `model_properties.yaml` with explicit `role: transport` documentation identifying the producer and consumer module for each field (§11.4).
4. **Add a SAGE pipeline index to `src/modules/README.md`** — a one-page table mapping every live SAGE module to one physics stage in visible order.
5. **Consider subdirectory structure or naming convention** in `_shared/` to distinguish pure utilities from shared physics kernels from contract headers (see §8).

---

*This report was produced by orchestrated analysis: Worker 1 traced original SAGE physics execution order from `sage-code/sage-model/`; Worker 2 audited all Mimic modules and `_shared/` contents; Worker 3 performed an independent review of the draft findings. Updated 2026-03-17 to revise the SF/SN consolidation recommendation (keep as separate swappable prescriptions), add the dependency enforcement framework (§7), and update the action plan accordingly. Source citations are available in the accompanying worker artifacts in `.ai-orchestrator/runs/`.*
