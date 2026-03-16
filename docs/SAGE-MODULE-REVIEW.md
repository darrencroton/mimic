# SAGE Module Organisation Review

**Date:** 2026-03-16
**Scope:** All SAGE physics modules in `src/modules/sage_*/`, `src/modules/_shared/`, and the Millennium pipeline configuration `input/millennium.yaml`
**Question:** What is the most sensible and efficient way to organise and group the SAGE modules in Mimic? Should any be moved, combined, split, or restructured? What are the consequences?
**Baseline:** Original SAGE codebase at `sage-code/sage-model/core_build_model.c`

---

## 1. Executive Summary

**Recommendation tier: targeted presentation reorganisation, high confidence.** Keep Mimic's four-phase runtime architecture and the scientifically meaningful module splits. Remove presentation-only splits and misleading names. The current implementation is largely faithful to SAGE in execution, but not yet the clearest possible presentation of the SAGE physics story to a researcher reading the YAML and module tree alone.

Key decisions:

- **Consolidate the SF/SN triple into one user-facing stage.** `sage_calculate_star_formation`, `sage_calculate_supernova_feedback`, and `sage_update_star_formation_supernova` are one tightly coupled recipe communicating through transient scratch fields with no intervening physics. They should not appear as three independent stages in the YAML (`src/modules/sage_calculate_star_formation/sage_calculate_star_formation.c:96`, `src/modules/sage_calculate_supernova_feedback/sage_calculate_supernova_feedback.c:106`, `src/modules/sage_update_star_formation_supernova/sage_update_star_formation_supernova.c:158`).
- **Keep the cooling chain split and visible.** Cooling, AGN suppression, and cooling application are a real three-step scientific story because `sage_radio_mode_heating` modifies `CoolingGas` between calculation and commitment. This split is scientifically meaningful and should remain (`src/modules/sage_calculate_cooling/README.md:39-42`, `src/modules/sage_radio_mode_heating/sage_radio_mode_heating.c:127-137`).
- **Remove housekeeping from the visible pipeline.** `sage_clear_disk_instability_triggers` zeroing a scratch field is not a SAGE physics stage. It should not appear in the YAML alongside real physics.
- **Rename modules to use physics-stage names.** Several names describe implementation mechanics rather than the science a researcher would recognise.
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
2. **`do_reionization()` is called twice** — once inside `infall_recipe()` for the central (`model_infall.c:43`) and once inside `strip_from_satellite()` for each satellite (`model_infall.c:95`). Mimic's split (`sage_reionization` pre-timestep, satellite stripping consuming stored `HaloBaryonFraction`) requires parity verification — see §10.1.
3. **Cooling is calculate-then-apply**, with AGN radio-mode heating embedded inside `cooling_recipe()` in SAGE. Mimic exposes this as three visible stages — a genuine improvement.
4. **SF, SN feedback, and disk instability are one bundled call** in SAGE. The three Mimic SF/SN modules are a single recipe without any intervening physics between them.
5. **Mergers run after all galaxies complete the SF/cooling substep** — SAGE explicitly separates these passes.
6. **Post-loop normalization is not yet implemented** in Mimic's `post_timestep` phase — see §10.3.

The SAGE physics story a researcher should read from the YAML is:

> baryon supply → cooling → star formation and feedback → morphological change → hierarchy changes

---

## 3. Design Principle

All consolidation and splitting decisions in this review follow one consistent rule:

> **Keep a calculate/apply split only when another visible physics module modifies the intermediate quantity before it is committed.**

If no module intervenes, the split exposes implementation transport fields as first-class physics stages — which creates visual noise in the YAML and implies false swappability. Applied consistently:

| Pipeline chain | Intervening modifier? | Keep split? |
|---|---|---|
| `calculate_cooling` → `radio_mode_heating` → `apply_cooling` | Yes — `radio_mode_heating` modifies `CoolingGas` | **Keep** |
| `calculate_star_formation` → `calculate_supernova_feedback` → `update_star_formation_supernova` | No — all three run consecutively around scratch fields | **Consolidate** |
| `calculate_infall` → `apply_infall` | Phase boundary — budget set once, applied across substeps | **Keep** (phase-lifecycle reason) |

---

## 4. Current State Assessment

### What already works well

- The **core phase model** is clean and matches SAGE's structural division: setup before substeps, baryonic physics within substeps, merger/disruption resolution at end of substep (`src/core/build_model.c:540-550`, `src/core/build_model.c:581-599`).
- The **YAML groupings** already tell most of the story. Inline comment groups (Cooling, Star Formation, etc.) make the flow visible without opening any source file (`input/millennium.yaml:41-67`).
- The **cooling calculate → modify → apply chain** is a genuine improvement over SAGE's embedded `do_AGN_heating()`. The visible AGN slot makes model replacement trivial (`src/modules/sage_calculate_cooling/README.md:39-42`).
- The **dual-use modules** are scientifically correct. `sage_quasar_mode` and `sage_collisional_starburst` spanning disk-instability and merger channels maps directly to SAGE physics, where `check_disk_instability()` and `collisional_starburst_recipe()` serve both trigger types.
- The **infall split** faithfully mirrors SAGE's loop structure: budget computed once pre-substep, applied incrementally within substeps.

### What is unclear or misleading

- **The SF/SN triple** presents one SAGE recipe as three independent stages, exposing `NewStellarMass`, `SupernovaReheatedMass`, and `SupernovaEjectedMass` as first-class model properties when they are inter-module transport fields.
- **`sage_clear_disk_instability_triggers`** zeroing a scratch field is visible alongside real physics stages — a researcher has to know this is housekeeping, not a scientific step.
- **Several names describe implementation mechanics** rather than the science. `sage_handle_mergers_immediate` is architecturally precise but obscures the user-facing step ("resolve mergers and disruption"). `sage_calculate_infall` is not a pure calculator — it also consolidates satellite reservoirs (`src/modules/sage_calculate_infall/sage_calculate_infall.c:62-121`).
- **`sage_calculate_merger_timescale` understates its scope.** It also resets `MergTime` to the sentinel for Type 0 promotions and forces `MergTime=0` for unresolved Type 2 orphans (`src/modules/sage_calculate_merger_timescale/sage_calculate_merger_timescale.c:56-88`). This is merger state management, not just timescale calculation.
- **`phase_2` is not self-explanatory from the YAML alone.** A researcher cannot infer that `process_per_event` consumers run when events are emitted, not at a later fixed position (`input/millennium.yaml:63-67`, `src/core/module_registry.c:393-452`).
- **`sage_collisional_starburst` has hidden cross-phase coupling.** At init time it queries whether `sage_disk_instability` is in phase_1 and whether `sage_quasar_mode` is in phase_2, and silently changes its own behaviour based on the answer (`src/modules/sage_collisional_starburst/sage_collisional_starburst.c:135-140`). Removing `sage_quasar_mode` from phase_2 silently removes quasar wind physics from the post-merger disk instability path.

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
| `sage_calculate_star_formation` + `sage_calculate_supernova_feedback` + `sage_update_star_formation_supernova` | `sage_star_formation_feedback` | One SAGE recipe; see §6 for consolidation rationale |
| `sage_collisional_starburst` | `sage_starburst_feedback` | Under-describes scope: handles disk-instability-triggered, merger-triggered, and post-minor-merger bursts (`sage_collisional_starburst.c:8-19`) |
| `sage_calculate_merger_timescale` | `sage_initialise_merger_clock` | Communicates lifecycle purpose; the module also handles Type 0 reset and Type 2 force-merge (`sage_calculate_merger_timescale.c:56-88`) |
| `sage_handle_mergers_immediate` | `sage_resolve_mergers_and_disruption` | This is the science stage a researcher expects to see in the flow (`sage_handle_mergers_immediate.c:5-10`) |
| `sage_clear_disk_instability_triggers` | *(remove from pipeline — see §6)* | Not a physics stage |

---

## 6. Recommendations: Consolidation and Splitting

### Consolidate: SF/SN triple → `sage_star_formation_feedback`

**Recommendation: Yes, consolidate.** The three modules form a single instantaneous SF/SN recipe. The fields `NewStellarMass`, `SupernovaReheatedMass`, and `SupernovaEjectedMass` are written, potentially renormalized, consumed, and zeroed in one continuous pass with no other module between them. No module in the current pipeline inserts itself between any of these three steps.

Evidence of tight coupling:
- `sage_calculate_star_formation` writes `NewStellarMass` (`sage_calculate_star_formation.c:96`)
- `sage_calculate_supernova_feedback` reads and may renormalize it before writing the SN fields (`sage_calculate_supernova_feedback.c:76-106`)
- `sage_update_star_formation_supernova` consumes all three and zeroes them (`sage_update_star_formation_supernova.c:70, 158`)

This is a stronger coupling than the cooling chain, where `sage_radio_mode_heating` genuinely intervenes with a different scientific process (AGN feedback) that a researcher would want to see or replace independently.

**SAGE parity:** Preserve the internal call order exactly inside the consolidated module: compute SF → metallicity refresh → compute SN → apply recycling/enrichment/transfers. This matches SAGE's `starformation_and_feedback()` internal sequence (`model_starformation_and_feedback.c:79-94`).

**Consequence:** Reduces YAML phase_1 length by 2 entries. Unit tests become coarser but more realistic. Three temporary properties can be reclassified as module-private scratch rather than global model properties (see §10.4).

### Keep split: cooling chain

**Recommendation: No consolidation.** `sage_radio_mode_heating` is a real physics module with three selectable AGN accretion models that a researcher may want to swap, suppress, or compare. The visible slot between cooling calculation and application is a genuine architectural improvement over SAGE's `do_AGN_heating()` being embedded inside `cooling_recipe()`.

**Rename the endpoints** to `sage_calculate_cooling_budget` and `sage_apply_cooling` to make the calculate-modify-apply pattern explicit.

### Remove from pipeline: `sage_clear_disk_instability_triggers`

**Recommendation: Remove this module from the visible YAML pipeline.** It does nothing except zero `UnstableDiskGasFraction` after the downstream consumers have read it. A researcher should not encounter a "clear trigger" step when reading the SAGE physics flow.

**Alternative:** Either clear `UnstableDiskGasFraction` at the end of `sage_disk_instability`'s own process function (after its consumers have run in the same phase), or handle it as a framework-managed scratch field lifecycle. If the explicit clear is retained for defensive correctness reasons, it should be documented as infrastructure, not listed alongside real physics modules.

**SAGE parity:** No change. The clear still occurs after both downstream consumers, just without surfacing as a YAML entry (`src/modules/sage_clear_disk_instability_triggers/sage_clear_disk_instability_triggers.c:29`).

### Keep split: cooling infall across phase boundary

**Recommendation: No consolidation.** `sage_prepare_infall_budget` runs once in `pre_timestep` and `sage_apply_infall` distributes across substeps in `phase_1`. The `InfallingGas` transport field bridges a genuine phase-lifecycle boundary. Combining would lose the substep distribution and the ordering against `EjectedMass` for negative infall (`sage_add_infall.c:60-89`).

### Keep split: merger clock initialisation and resolution

**Recommendation: No consolidation.** One initialises `MergTime` before substeps; the other consumes it live during `phase_2` substeps. The split is phase-driven and scientifically valid: setup before the substep loop, resolution within it. Rename both as described in §5.

### Address: `sage_collisional_starburst` hidden coupling

**Issue:** At init time, `sage_collisional_starburst` calls `phase_has_module()` to check for `sage_disk_instability` in phase_1 and `sage_quasar_mode` in phase_2, silently changing its own behaviour based on what other modules are configured (`sage_collisional_starburst.c:135-140`). A researcher who removes `sage_quasar_mode` from phase_2 while keeping it in phase_1 would silently lose quasar wind physics from the post-merger disk instability path — with no warning.

**This is a SAGE parity requirement.** In original SAGE, `deal_with_galaxy_merger()` calls `check_disk_instability()` directly for minor mergers (`model_mergers.c:277`), which in turn calls `grow_black_hole()` and `quasar_mode_wind()` inline. Mimic cannot replicate this inline call within the event-based architecture without the module knowing what consumers exist downstream.

**Recommendation:** Accept the current implementation for SAGE parity. Add an explicit `WARNING`-level log message in `sage_collisional_starburst_init()` when `sage_disk_instability` is present in phase_1 but `sage_quasar_mode` is absent from phase_2, so the silent physics loss becomes visible. Document the design decision in the module's README.

---

## 7. Recommendations: The `_shared/` Directory

### Overall assessment

`_shared/` is conceptually correct but currently under-governed. Its own README defines it as a home for reusable shared calculations used by multiple modules and explicitly says not to create a shared utility when only one module uses it (`src/modules/_shared/README.md:11-16`, `src/modules/_shared/README.md:45-53`). The live contents only partly follow that rule.

### File-by-file decisions

| File | Recommendation | Justification |
|---|---|---|
| `metallicity.h` | **Keep** | Textbook shared utility: small, stateless, header-only, used by 7 modules (`sage_add_infall`, `sage_calculate_cooling`, `sage_add_cooling`, `sage_radio_mode_heating`, `sage_reincorporation`, `sage_satellite_stripping`, `sage_update_star_formation_supernova`) |
| `time_parity.h` | **Keep** | High-value shared runtime/SAGE-parity support used by 6 modules across cooling, SF, mergers, and starbursts |
| `central_link.h` | **Keep** | Shared by two merger modules; encodes SAGE-specific target-resolution rules used by both |
| `sage_disk_instability_physics.h` | **Keep while two consumers exist** | Used by `sage_disk_instability` and `sage_collisional_starburst`; justified by the post-merger follow-up. If the follow-up is ever removed, move to `sage_disk_instability/` as a private header |
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

## 8. Recommended YAML After Changes

After the recommended renames and consolidation, the pipeline should read:

```yaml
modules:
  pre_timestep:
    # Setup and snapshot budgets
    - sage_reionization:           process_full_halo
    - sage_prepare_infall_budget:  process_full_halo
    - sage_set_disk_scale_radius:  process_full_halo
    - sage_initialise_merger_clock: process_full_halo

  phase_1:
    # Baryon supply
    - sage_apply_infall:           process_full_halo
    - sage_reincorporation:        process_full_halo
    - sage_satellite_stripping:    process_full_halo

    # Cooling and AGN suppression
    - sage_calculate_cooling_budget: process_by_galaxy
    - sage_radio_mode_heating:     process_by_galaxy
    - sage_apply_cooling:          process_by_galaxy

    # Star formation and supernova feedback
    - sage_star_formation_feedback: process_by_galaxy

    # Disk instability
    - sage_disk_instability:       process_by_galaxy
    - sage_quasar_mode:            process_by_galaxy
    - sage_starburst_feedback:     process_by_galaxy

  phase_2:
    # Mergers and disruption — runs after all galaxy physics for the current substep
    # sage_resolve_mergers_and_disruption emits SAGE_EVENT_MERGER for downstream consumers
    - sage_resolve_mergers_and_disruption: process_full_halo
    - sage_quasar_mode:            process_per_event
    - sage_starburst_feedback:     process_per_event

  post_timestep:
    - sage_finalise_outputs:       process_full_halo   # normalize Cooling, Heating, OutflowRate
```

The physics of SAGE is visible in the YAML without opening any source file.

---

## 9. Consequences and Trade-offs

### If Mimic consolidates the SF/SN triple

**Benefits:**
- The YAML presents one SAGE physics stage rather than three implementation steps.
- Three transport properties (`NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`) can become module-private scratch, reducing the global property surface.
- Fewer metadata declarations, fewer documentation surfaces to keep in sync.

**Costs:**
- Some fine-grained swappability is lost. Replacing only the SF rate model (keeping the SN model) becomes more intrusive: it requires editing a larger module rather than swapping a small one.
- Unit tests become coarser — one test exercises SF + SN together.

**Assessment:** The swappability cost is real but speculative. No current research use case requires independent SF/SN model swapping without touching the other. The readability gain for researchers is concrete and immediate.

### If Mimic stays at current granularity

**Benefits:**
- Maximum swappability for SF-rate and SN-model experiments.
- Small, focused unit tests.
- Very explicit dataflow for developers who understand the transport-field contracts.

**Costs:**
- The YAML overstates the number of meaningful SAGE physics stages.
- A researcher must understand Mimic-specific transport mechanics to reconstruct the flow.
- Metadata drift risk increases — every helper boundary has its own `module_info.yaml` and test surface.

### Recommended trade-off

Keep splits only where there is a real intervening physics step (cooling chain) or a phase-lifecycle reason (infall across the substep boundary). Consolidate presentation-only decomposition (SF/SN triple). Remove visible housekeeping (disk instability trigger clear).

---

## 10. Open Questions and Architectural Gaps

### 10.1 `sage_reionization` satellite parity

In original SAGE, `do_reionization()` is called in two places: inside `infall_recipe()` for the central (`model_infall.c:43`) and inside `strip_from_satellite()` for each Type 1 satellite (`model_infall.c:95`). In Mimic, `sage_reionization` runs in `pre_timestep` and computes `HaloBaryonFraction` only for the Type 0 central (`sage_reionization.c:117,126`). `sage_satellite_stripping` then reads that stored value (`sage_satellite_stripping.c:79`).

**Open question:** Does `do_reionization()` in SAGE use FOF-level properties (same for all members of the group) or galaxy-local properties? If FOF-level, the Mimic split is parity-safe. If satellite-local, the stored central value is incorrect for stripping, and reionization logic should either run `process_by_galaxy` or move back into the consuming modules.

**Risk:** Scientific — output galaxy properties could diverge from SAGE under certain reionization-affected conditions. Verify against SAGE output before relying on the current split.

### 10.2 `sage_collisional_starburst` pipeline coupling contract

When `sage_quasar_mode` is absent from phase_2 but `sage_disk_instability` is present in phase_1, the post-merger disk instability path silently skips quasar wind physics (`sage_collisional_starburst.c:138`). The unit tests codify this behaviour (`test_unit_sage_collisional_starburst.c:929, 972`), but it is not visible from the YAML. Add a `WARNING`-level log at init time when this combination is detected.

### 10.3 `post_timestep` not yet implemented

SAGE normalizes `Cooling`, `Heating`, and `OutflowRate` after the substep loop and accumulates `TotalSatelliteBaryons` (`core_build_model.c:410,422,427`). Mimic's `post_timestep` phase is currently empty. These are output-field correctness issues, not galaxy-physics correctness issues, but output fields will disagree with SAGE until `sage_finalise_outputs` (or equivalent) is implemented.

### 10.4 Transient transport fields as global model properties

`InfallingGas`, `CoolingGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, `Rcool`, `CoolingLambda`, and `UnstableDiskGasFraction` appear in the global property schema as ordinary model properties, but their main role is inter-module communication (`src/modules/model_properties.yaml:29-49`, `:121-130`, `:246-317`, `:397-406`). If the SF/SN triple is consolidated, three of these become true module-private scratch. All of them should be documented explicitly as transient inter-module transport fields in the property metadata.

---

## 11. Metadata Drift Bugs

These should be fixed regardless of any larger reorganisation. All are correctness problems in the documentation layer.

1. **`sage_disk_instability/module_info.yaml` declares `Vvir`** but the actual physics helper uses `Vmax` in the stability criterion (`src/modules/sage_disk_instability/module_info.yaml:7-18`, `src/modules/_shared/sage_disk_instability_physics.h:27-29`).
2. **`sage_calculate_cooling/module_info.yaml` under-declares inputs and outputs.** The module reads `Vvir` and `Rvir` and writes `CoolingLambda`, but those are missing from the metadata (`src/modules/sage_calculate_cooling/module_info.yaml:11-17`, `src/modules/sage_calculate_cooling/sage_calculate_cooling.c:39-57, 129-136`).
3. **`sage_quasar_mode/module_info.yaml` omits `Vvir`** and `QuasarModeBHaccretionMass` from its property declarations (`src/modules/sage_quasar_mode/module_info.yaml:7-19`, `src/modules/_shared/merger_physics.h:44-51, 82-91`).
4. **Merger modules omit `CentralHalo`.** Both `sage_initialise_merger_clock` and `sage_resolve_mergers_and_disruption` depend on the central-link logic that resolves `CentralHalo` (`src/modules/sage_calculate_merger_timescale/module_info.yaml:7-18`, `src/modules/_shared/central_link.h:23-49`).
5. **`sage_update_disk_radius` documentation is internally inconsistent.** `module_info.yaml` says Type 0/1; the source header says Type 1/2; the implementation skips `Type >= 2`, so live behaviour is Type 0/1 only (`src/modules/sage_update_disk_radius/module_info.yaml:2-14`, `src/modules/sage_update_disk_radius/sage_update_disk_radius.c:5-8, 90-92`).
6. **`docs/USER-GUIDE.md` shows a misleading phase_1 order.** The SAGE example visually places `sage_satellite_stripping` after cooling and SF, but runtime always executes `process_full_halo` modules before `process_by_galaxy` modules (`docs/USER-GUIDE.md:171-193`, `src/core/module_registry.c:535-597`).
7. **`_shared/module_info.yaml` contains a stale path.** It points readers to `src/modules/shared/README.md`, but the live path is `_shared/README.md` (`src/modules/_shared/module_info.yaml:20`).
8. **Transient transport fields are not documented as transport fields** in `model_properties.yaml` — see §10.4.

---

## 12. Recommended Action Plan

### Must-do

1. **Consolidate the SF/SN triple** into `sage_star_formation_feedback`. If the code remains split internally for test or implementation reasons, the YAML and documentation should present it as one stage.
2. **Rename the modules** listed in §5. The highest-priority renames are those that actively mislead: `sage_calculate_infall` (not a pure calculator), `sage_handle_mergers_immediate` (obscures the physics step), and `sage_collisional_starburst` (understates scope).
3. **Remove `sage_clear_disk_instability_triggers` from the visible YAML pipeline.** Handle the scratch field clear as infrastructure or inside the existing module lifecycle.
4. **Move `sage_merger_ops.h` out of `_shared/`** into `sage_resolve_mergers_and_disruption/` as a private header. Single consumer violates the directory's stated policy.
5. **Split `merger_physics.h`** into `sage_agn_physics.h` and `sage_starburst_physics.h`.
6. **Fix all 8 metadata drift bugs** in §11. These are correctness errors, not style preferences.
7. **Add a `WARNING`-level init log** in `sage_collisional_starburst` when `sage_disk_instability` is present in phase_1 but `sage_quasar_mode` is absent from phase_2.

### Nice-to-have

1. **Implement `sage_finalise_outputs`** in `post_timestep` to close the SAGE output-normalization gap.
2. **Verify `sage_reionization` satellite parity** against SAGE scientific output. If a gap is confirmed, restructure the module or move reionization logic into the consuming modules.
3. **Reclassify transient transport fields** in `model_properties.yaml` with explicit documentation that they are inter-module scratch, not persistent output properties.
4. **Add a SAGE pipeline index to `src/modules/README.md`** — a one-page table mapping every live SAGE module to one physics stage in visible order.
5. **Consider subdirectory structure or naming convention** in `_shared/` to distinguish pure utilities from shared physics kernels from contract headers (see §7).

---

*This report was produced by orchestrated analysis: Worker 1 traced original SAGE physics execution order from `sage-code/sage-model/`; Worker 2 audited all Mimic modules and `_shared/` contents; Worker 3 performed an independent review of the draft findings. Source citations are available in the accompanying worker artifacts in `.ai-orchestrator/runs/`.*
