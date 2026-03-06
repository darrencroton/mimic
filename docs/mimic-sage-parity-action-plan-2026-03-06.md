# Mimic-SAGE Parity Action Plan

**Date**: 2026-03-06
**Amended**: 2026-03-06 (incorporated critical amendments C1-C3, F1-F2)
**Purpose**: Comprehensive implementation guide to recover Mimic-SAGE parity
**Status**: Ready for implementation

---

## Document Structure

Each issue is documented with:
- **ID**: Unique identifier for tracking
- **Severity**: Critical / High / Medium / Low
- **Location**: File paths and line numbers
- **Cause**: Root cause of the divergence
- **Effect**: Scientific/behavioral impact
- **Solutions**: Ordered best → acceptable, with vision principle alignment
- **Recommended**: Highlighted solution

---

## Issue Summary Table

| ID | Severity | Summary | Recommended Solution | Effort | Status (2026-03-06) |
|----|----------|---------|---------------------|--------|----------------------|
| P1 | Critical | Substep time base and merger timestamp consumer mismatch | Fix base + midpoint consumer usage | Low | Done |
| P2 | Critical | Merger physics disconnected (BH/starburst missed) | Shared helpers inline (P2-A) | Low-Med | Done |
| P3 | Critical | Trigger clearing suppresses disk instability starburst | Split channels + by-galaxy clear (P3-A) | Low-Med | Done |
| P4 | High | Type 2 eligibility bypasses baryonic protection | Remove Type==2 condition | Trivial | Done |
| P5 | High | Type 0→2 transitions lack immediate merge | Add MergTime=0.0 | Low | Pending |
| P6 | High | Central-link semantics differ (FOF vs per-subhalo) | Restore per-subhalo central semantics (P6-B) | High | Pending |
| P7 | Low | Virial mass condition edge case | Change > to >= | Trivial | Skipped |
| P8 | Low | Documentation execution-order mismatch | Fix header comment | Trivial | Done |
| P9 | Low | Merger lineage fields not in output | Add to metadata if needed | Low | Skipped |

---

## Critical Issues

### P1: Substep Time Formula Uses Wrong Base Reference

**Severity**: Critical
**Discovered by**: Independent assessment revalidation

**Location**:
- `src/core/build_model.c:534` (`update_context_for_substep`)
- `src/core/build_model.c:497` (`setup_module_context`)
- `src/modules/sage_merge_galaxies.c:106,113` (`TimeOfLast*Merger` uses `ctx->time`)

**Cause**:
The substep time calculation uses current snapshot age as the base instead of progenitor snapshot age, and merger timestamp consumers currently write `ctx->time` instead of midpoint time.

Current (incorrect):
```c
ctx->time = Age[snap];  // Current snapshot
ctx->substep_time = ctx->time - (step + 0.5) * ctx->substep_dt;

central->TimeOfLastMajorMerger = ctx->time;
central->TimeOfLastMinorMerger = ctx->time;
```

SAGE (correct):
```c
time = Age[Gal[p].SnapNum] - (step + 0.5) * (deltaT / STEPS);
// Gal[p].SnapNum is progenitor snapshot
```

**Effect**:
- ALL substep-dependent physics operates in completely wrong time range
- Numerical example with Age[prev]=10 Gyr, Age[current]=8 Gyr, STEPS=10:
  - SAGE: interpolates 9.9 → 8.1 Gyr (correct: near progenitor → near current)
  - Mimic: interpolates 7.9 → 6.1 Gyr (wrong: beyond current snapshot!)
- Affects cooling rates, star formation, merger timing, all time-dependent calculations
- Merger timestamp fields (`TimeOfLastMajorMerger`, `TimeOfLastMinorMerger`) remain snapshot-anchored unless consumer writes are corrected

**Solutions**:

#### Solution P1-A: Fix base reference in context setup ⭐ RECOMMENDED

Change `update_context_for_substep` to use progenitor age as base, then update merger timestamp consumers to use midpoint time:

```c
static void update_context_for_substep(struct ModuleContext *ctx, int step) {
    ctx->substep_number = step;
    // Use progenitor age as base, subtract to move toward current
    double progenitor_age = ctx->time + ctx->time_interval;
    ctx->substep_time = progenitor_age - (step + 0.5) * ctx->substep_dt;
}

// In sage_merge_galaxies.c (major/minor merger timing)
central->TimeOfLastMajorMerger = ctx->substep_time;
central->TimeOfLastMinorMerger = ctx->substep_time;
```

**Vision Alignment**:
- Vision 1 (Physics-Agnostic Core): ✅ Core provides correct time context
- Vision 4 (Single Source of Truth): ✅ Time computed once, used everywhere
- Vision 8 (Type Safety): ✅ No interface changes needed

**Effort**: Low (2-3 lines changed)

#### Solution P1-B: Add explicit progenitor_time field to context

Add `ctx->progenitor_time = Age[prev_snap]` in setup, reference explicitly.

**Vision Alignment**: Same as P1-A, slightly more explicit
**Effort**: Low

---

### P2: Merger Physics Disconnected (BH Growth/Starburst Missed)

**Severity**: Critical
**Discovered by**: Original audit Finding 1

**Location**:
- `src/modules/sage_update_merger_time.c:131-134` (sets IsMerging on satellite)
- `src/modules/sage_merge_galaxies.c:117` (marks satellite Type 3)
- `src/core/module_registry.c:356-359` (skips Type 3 in by-galaxy loop)
- `src/modules/sage_quasar_mode.c:162` (checks gal->IsMerging)
- `src/modules/sage_collisional_starburst.c:122` (checks gal->IsMerging)
- `input/millennium.yaml:62-70` (phase_2 module order)

**Cause**:
1. `sage_update_merger_time` sets `IsMerging=1` and `MergerMassRatio` on the **satellite**
2. `sage_merge_galaxies` transfers mass and marks satellite as **Type 3**
3. `execute_phase` skips Type 3 galaxies in by-galaxy loop
4. `sage_quasar_mode` and `sage_collisional_starburst` never see the merger trigger

In SAGE, BH growth and starburst are called inline within `deal_with_galaxy_merger()` on the **central**.

**Effect**:
- Merger-driven BH accretion is completely missed
- Merger-driven starbursts are completely missed
- Systematic underestimation of BH masses
- Missing burst-driven stellar mass and bulge growth
- Missing metal production from merger starbursts

**Solutions**:

> **CRITICAL DESIGN CONSTRAINT (Amendment C2)**: A central can receive multiple mergers in one substep. Any solution must handle this correctly to achieve SAGE parity, where each merger triggers BH growth and starburst sequentially.

#### Solution P2-A: Parity bridge via shared helpers ⭐ RECOMMENDED

Call shared helper functions from `sage_merge_galaxies` during each merge, immediately after mass transfer. This matches SAGE's inline execution model exactly.

```c
// In sage_merge_galaxies_process, for EACH merging satellite:
if (satellite->MergerMassRatio > 0.0) {
    // Call shared helpers immediately - handles multiple mergers correctly
    merger_grow_black_hole(central_halo, satellite->MergerMassRatio, ctx);
    merger_collisional_starburst(central_halo, satellite->MergerMassRatio, ctx);
}
// Then mark satellite Type 3
halos[i].Type = 3;
```

Extract helpers to `src/modules/_shared/merger_physics.h`:
```c
void merger_grow_black_hole(struct Halo *central, double mass_ratio,
                            struct ModuleContext *ctx);
void merger_collisional_starburst(struct Halo *central, double mass_ratio,
                                   struct ModuleContext *ctx);
```

**Why this is now recommended**:
- Handles multiple mergers per central per substep correctly (each processed sequentially)
- Matches SAGE causal coupling exactly
- Avoids Type-3/by-galaxy skip issue entirely
- No complex event queue or aggregation semantics needed

**Vision Alignment**:
- Vision 1: ⚠️ Physics called from merge module, but via shared helpers
- Vision 2: ⚠️ BH/starburst not independently configurable for mergers (matches SAGE)
- Vision 4: ✅ Single source of truth via shared helper extraction

**Effort**: Low-Medium (extract helpers, call from merge module)

#### Solution P2-B: Event-Contract Pipeline with Queue Semantics

Design explicit producer/consumer/clear with bounded event queue for multiple mergers:

1. **Producer** (`sage_merge_galaxies`): Append merger event to central's queue
   ```c
   // Bounded queue (e.g., max 8 mergers per substep)
   if (central->NumPendingMergers < MAX_PENDING_MERGERS) {
       central->MergerEventQueue[central->NumPendingMergers].MassRatio = satellite->MergerMassRatio;
       central->NumPendingMergers++;
   }
   ```

2. **Consumers** (`sage_quasar_mode`, `sage_collisional_starburst`): Process all queued events
   ```c
   for (int m = 0; m < gal->NumPendingMergers; m++) {
       double mass_ratio = gal->MergerEventQueue[m].MassRatio;
       // Process merger physics
   }
   ```

3. **Clear stage**: Clear queue after all consumers

**Vision Alignment**:
- Vision 1: ✅ Core unchanged
- Vision 2: ✅ Consumers remain independent modules
- Vision 3: ⚠️ Requires array/struct in metadata (more complex)
- Vision 5: ✅ Deterministic flow with explicit queue

**Effort**: Medium-High (queue data structure, metadata changes, all consumers updated)
**Note**: More architecturally pure but higher complexity; prefer P2-A for parity

#### Solution P2-C: Forward flags to central (NOT RECOMMENDED)

Single scalar flag forwarding cannot handle multiple mergers per substep correctly.

**Vision Alignment**: N/A - does not meet requirements
**Note**: Rejected per Amendment C2

---

### P3: Trigger Clearing Suppresses Disk Instability Starburst

**Severity**: Critical
**Discovered by**: Independent assessment revalidation

**Location**:
- `src/modules/sage_quasar_mode.c:170-173` (clears triggers)
- `src/modules/sage_collisional_starburst.c:127-130` (clears triggers)
- `input/millennium.yaml:57-60` (phase_1 order: disk_instability → quasar → starburst)

**Cause**:
In phase_1, modules execute in order for each galaxy:
1. `sage_disk_instability` sets `UnstableDiskGasFraction = X`
2. `sage_quasar_mode` reads X, processes BH growth, **clears to 0.0**
3. `sage_collisional_starburst` reads **0.0** - trigger already cleared!

Both modules clear the same triggers after processing:
```c
gal->UnstableDiskGasFraction = 0.0;
gal->IsMerging = 0;
gal->MergerMassRatio = 0.0;
```

**Effect**:
- Disk instability starburst is completely missed
- BH growth from disk instability works (runs first)
- Systematic underestimation of starburst-driven star formation
- Missing bulge growth from disk instability

**Solutions**:

> **CRITICAL DESIGN CONSTRAINT (Amendment C1)**: `execute_phase()` runs ALL `process_full_halo` modules FIRST, then ALL `process_by_galaxy` modules. A `process_full_halo` clear module listed "at the end" of the YAML will actually run BEFORE the by-galaxy consumers. Clear modules MUST use `process_by_galaxy` mode to run after other by-galaxy modules.

> **CRITICAL DESIGN CONSTRAINT (Amendment C3)**: Triggers must be managed across the full lifecycle: phase_1 (disk instability) → phase_2 (mergers) → next substep. Either split triggers by channel OR clear at end of every phase where consumers run.

#### Solution P3-A: Split triggers by channel with phase-specific clears ⭐ RECOMMENDED

Separate disk instability triggers from merger triggers to prevent cross-contamination:

**Trigger Channels**:
- **Disk Instability Channel**: `UnstableDiskGasFraction` (produced in phase_1, consumed in phase_1)
- **Merger Channel**: `IsMerging`, `MergerMassRatio` (produced in phase_2, consumed in phase_2)

**Implementation**:

1. Remove ALL trigger clearing from `sage_quasar_mode.c` and `sage_collisional_starburst.c`

2. Add clear module using `process_by_galaxy` (runs AFTER other by-galaxy modules):
   ```c
   // sage_clear_disk_instability_triggers.c
   int sage_clear_disk_instability_triggers_process(struct ModuleContext *ctx,
                                                     struct Halo *halos, int ngal) {
       if (ngal != 1) return -1;  // process_by_galaxy mode
       struct GalaxyData *gal = halos[0].galaxy;
       if (gal != NULL) {
           gal->UnstableDiskGasFraction = 0.0;
       }
       return 0;
   }
   ```

3. Update `millennium.yaml`:
   ```yaml
   phase_1:
     # Disk Instability Physics
     - sage_disk_instability: process_by_galaxy
     - sage_quasar_mode: process_by_galaxy
     - sage_collisional_starburst: process_by_galaxy
     - sage_clear_disk_instability_triggers: process_by_galaxy  # LAST by-galaxy

   phase_2:
     # Merger Physics (if using P2-A shared helpers, no separate clear needed)
     - sage_update_merger_time: process_full_halo
     - sage_merge_galaxies: process_full_halo  # Handles BH/starburst inline
     - sage_disrupt_satellites: process_full_halo
     # Merger triggers cleared implicitly by P2-A approach (processed inline)
   ```

**Why split channels**:
- Disk instability triggers don't leak into phase_2
- Merger triggers (if using event queue approach) don't leak into next substep
- Each channel has clear ownership and lifecycle
- `process_by_galaxy` clear module runs in correct position (after consumers)

**Vision Alignment**:
- Vision 1: ✅ Core unchanged
- Vision 2: ✅ Clear stage is configurable module
- Vision 3: ✅ Trigger fields clearly scoped by channel
- Vision 5: ✅ Deterministic processing contract per channel

**Effort**: Low-Medium (new simple module, remove inline clears, update YAML)

#### Solution P3-B: Unified clear at end of each phase

Single clear module that runs at end of both phase_1 and phase_2, clearing ALL triggers.

```yaml
phase_1:
  - sage_disk_instability: process_by_galaxy
  - sage_quasar_mode: process_by_galaxy
  - sage_collisional_starburst: process_by_galaxy
  - sage_clear_all_triggers: process_by_galaxy  # Clear all

phase_2:
  - sage_update_merger_time: process_full_halo
  - sage_merge_galaxies: process_full_halo
  - sage_disrupt_satellites: process_full_halo
  - sage_quasar_mode: process_by_galaxy
  - sage_collisional_starburst: process_by_galaxy
  - sage_clear_all_triggers: process_by_galaxy  # Clear all again
```

**Vision Alignment**:
- Vision 2: ⚠️ Same module in multiple phases
- Vision 5: ✅ Deterministic but requires careful phase configuration

**Effort**: Low
**Note**: Works but less clean than channel separation; prefer P3-A

#### Solution P3-C: Move clearing to last consumer only (NOT RECOMMENDED)

**Note**: Rejected per Amendment C1 - creates implicit ordering dependency and is fragile if configuration changes.

---

## High Priority Issues

### P4: Type 2 Eligibility Bypasses Baryonic Protection

**Severity**: High
**Discovered by**: Original audit Finding 2

**Location**:
- `src/modules/sage_update_merger_time.c:114`

**Cause**:
Mimic adds unconditional Type 2 eligibility:
```c
const int eligible = (galaxyBaryons == 0.0) || (halos[i].Type == 2) ||  // <-- This
                    (galaxyBaryons > 0.0 && (currentMvir / galaxyBaryons <= THRESHOLD));
```

SAGE only checks mass ratio:
```c
if (is_zero(galaxyBaryons) ||
    (is_greater(galaxyBaryons, 0.0) &&
     is_less_or_equal(currentMvir / galaxyBaryons, threshold)))
```

**Effect**:
- Type 2 orphans with high baryonic mass lose protection
- `currentMvir` is interpolated from `previousMvir` over substeps
- Early substeps should protect massive orphans; Mimic doesn't
- Faster Type 2 mergers/disruptions
- Elevated ICS contribution
- Changed central mass assembly history

**Solutions**:

#### Solution P4-A: Remove Type 2 condition ⭐ RECOMMENDED

```c
const int eligible = (galaxyBaryons == 0.0) ||
                    (galaxyBaryons > 0.0 && (currentMvir / galaxyBaryons <= THRESHOLD_SAT_DISRUPTION));
```

**Vision Alignment**:
- Vision 1: ✅ Physics in module
- Vision 4: ✅ Matches SAGE single source of truth

**Effort**: Trivial (delete 1 condition)

---

### P5: Type 0→2 Transitions Lack Immediate Merge

**Severity**: High
**Discovered by**: Original audit Finding 5

**Location**:
- `src/core/build_model.c:319-331` (copy_progenitor_halos, Type 2 branch)

**Cause**:
SAGE forces immediate merge for direct Type 0→2 transitions:
```c
if (is_greater(Gal[ngal].MergTime, 999.0) || Gal[ngal].Type == 0) {
    Gal[ngal].MergTime = 0.0;  // Force immediate merge
}
```

Mimic doesn't set MergTime for this case:
```c
if (FoFWorkspace[ngal].Type == 0) {
    FoFWorkspace[ngal].infallMvir = previousMvir;
    // ... but no MergTime = 0.0
}
FoFWorkspace[ngal].Type = 2;
```

**Effect**:
- Former centrals that skip Type 1 get calculated dynamical friction timescale
- SAGE assumes instant merger for halos that lose subhalo completely
- Different Type 2 survival times

**Solutions**:

#### Solution P5-A: Add MergTime = 0.0 for Type 0→2 ⭐ RECOMMENDED

```c
if (FoFWorkspace[ngal].Type == 0) {
    FoFWorkspace[ngal].infallMvir = previousMvir;
    FoFWorkspace[ngal].infallVvir = previousVvir;
    FoFWorkspace[ngal].infallVmax = previousVmax;
    // Force immediate merge like SAGE
    if (FoFWorkspace[ngal].galaxy != NULL) {
        FoFWorkspace[ngal].galaxy->MergTime = 0.0;
    }
}
FoFWorkspace[ngal].Type = 2;
```

**Vision Alignment**:
- Vision 1: ⚠️ This is physics logic in core - acceptable as it's type transition handling
- Vision 5: ✅ Matches SAGE unified processing model

**Effort**: Low (add 3 lines)

---

## High Priority Structural Issue

### P6: Central-Link Semantics Differ (FOF vs Per-Subhalo)

**Severity**: High (strict SAGE parity required)
**Discovered by**: Original audit Finding 3

**Location**:
- `src/core/build_model.c:371-403` (`set_halo_centrals`)
- `src/core/build_model.c:109` (call site - once for entire FOF)
- SAGE: `sage-code/sage/core_build_model.c:349-364`, `393` (per-subhalo)

**Cause**:
SAGE sets central per subhalo, finding Type 0 OR Type 1:
```c
for (i = ngalstart; i < ngal; i++) {
    if (Gal[i].Type == 0 || Gal[i].Type == 1) {
        centralgal = i;
    }
}
// Called per-subhalo in join_galaxies_of_progenitors
```

Mimic sets one FOF-wide Type 0 central:
```c
for (i = ngalstart; i < ngal; i++) {
    if (FoFWorkspace[i].Type == 0) {  // Only Type 0
        centralgal = i;
        break;
    }
}
// Called once for entire FOF
```

**Effect**:
- SAGE: Type 2 orphans from satellite subhalos merge into their subhalo's Type 1 central first
- Mimic: Type 2 orphans merge directly into FOF Type 0 central
- Changes mass assembly hierarchy
- Intermediate accumulation in satellite centrals lost

**Solutions**:

#### Solution P6-A: Document as intentional Mimic simplification (NON-PARITY FALLBACK ONLY)

Add to `docs/DEVELOPER-GUIDE.md`:
> Mimic uses FOF-global central assignment. All satellites and orphans reference the FOF's Type 0 central directly. This differs from SAGE's per-subhalo central assignment where Type 2 orphans first merge into their subhalo's Type 1 central.

**Vision Alignment**:
- Vision 5: ✅ Unified (simpler) processing model
- Parity: ❌ Does not match SAGE

**Effort**: Trivial (documentation only)

#### Solution P6-B: Restore per-subhalo central assignment ⭐ RECOMMENDED (MANDATORY FOR THIS PARITY PLAN)

1. Move `set_halo_centrals` call into `join_progenitor_halos` (per-subhalo)
2. Modify to accept Type 0 OR Type 1 as central
3. Update modules to respect `CentralHalo` for Type 2 merge targets

**Vision Alignment**:
- Vision 5: ⚠️ More complex, but matches SAGE
- Parity: ✅ Matches SAGE

**Effort**: High (structural change, module updates)

---

## Low Priority Issues

### P7: Virial Mass Condition Edge Case

**Severity**: Low
**Discovered by**: Original audit Finding 7

**Location**:
- `src/core/virial.c:96-97`

**Cause**:
```c
// Mimic:
if (InputTreeHalos[halonr].Mvir > 0.0)  // Excludes exactly 0.0

// SAGE:
if (Halo[halonr].Mvir >= 0.0)  // Includes exactly 0.0
```

**Effect**:
- For halos with exactly Mvir == 0.0 (rare edge case):
  - SAGE returns 0.0
  - Mimic returns Len * PartMass

**Solutions**:

#### Solution P7-A: Change > to >= ⭐ RECOMMENDED

```c
if (halonr == InputTreeHalos[halonr].FirstHaloInFOFgroup &&
    InputTreeHalos[halonr].Mvir >= 0.0)
```

**Vision Alignment**: No vision impact
**Effort**: Trivial (1 character)

---

### P8: Documentation Execution-Order Mismatch

**Severity**: Low
**Discovered by**: Original audit Finding 8

**Location**:
- `src/core/module_registry.h:83-89`

**Cause**:
Header claims by-galaxy first, implementation does full-halo first:
```c
// Header (wrong):
* 1. All PROCESSING_MODE_BY_GALAXY modules execute in galaxy-major order
* 2. All PROCESSING_MODE_FULL_HALO modules execute with full halo array

// Implementation (correct):
/* PASS 1: PROCESSING_MODE_FULL_HALO modules */
/* PASS 2: PROCESSING_MODE_BY_GALAXY modules */
```

**Solutions**:

#### Solution P8-A: Fix header documentation ⭐ RECOMMENDED

```c
* Execution order within phase:
* 1. All PROCESSING_MODE_FULL_HALO modules execute with full halo array
* 2. All PROCESSING_MODE_BY_GALAXY modules execute in galaxy-major order
```

**Effort**: Trivial

---

### P9: Merger Lineage Fields Not in Output

**Severity**: Low
**Discovered by**: Original audit Finding 6

**Location**:
- `src/modules/model_properties.yaml` (fields not defined)

**Cause**:
SAGE outputs `mergeType`, `mergeIntoID`, `mergeIntoSnapNum` for lineage reconstruction. Mimic's metadata-driven system doesn't define these.

**Solutions**:

#### Solution P9-A: Add fields to metadata if needed

```yaml
# In src/modules/model_properties.yaml
- name: MergerType
  type: int
  scope: snapshot
  description: "Type of merger event (0=none, 1=minor, 2=major, 4=disruption)"

- name: MergeIntoID
  type: long_long
  scope: snapshot
  description: "UniqueGalaxyID of merger target"

- name: MergeIntoSnapNum
  type: int
  scope: snapshot
  description: "Snapshot number when merger occurred"
```

**Vision Alignment**:
- Vision 3: ✅ Metadata-driven
- Vision 7: ✅ Format-agnostic output

**Effort**: Low (metadata addition, run `make generate`)

---

## Implementation Sequence

> **IMPORTANT (Amendment C2)**: Finalize trigger contract and merge event semantics BEFORE coding module changes. The recommended approach (P2-A shared helpers + P3-A channel separation) simplifies implementation by handling merger physics inline.

### Phase 0: Design Decisions (Must finalize first)

| Order | Decision | Options | Recommendation | Status (2026-03-06) |
|-------|----------|---------|----------------|----------------------|
| 0.1 | Merger physics approach | P2-A (inline helpers) vs P2-B (event queue) | P2-A for SAGE parity | Accepted and implemented (shared helpers called inline from `sage_merge_galaxies`) |
| 0.2 | Trigger lifecycle | P3-A (split channels) vs P3-B (unified clear) | P3-A for clarity | Accepted and applied for phase_1 via by-galaxy clear module |
| 0.3 | Central-link policy | P6-A (document fallback) vs P6-B (restore SAGE behavior) | P6-B (mandatory for strict parity) | Recommendation accepted; implementation deferred (P6 skipped for now) |

### Phase 1: Critical Correctness (Must complete first)

| Order | Issue | Action | Files | Status (2026-03-06) |
|-------|-------|--------|-------|----------------------|
| 1.1 | P1 | Fix substep time formula | `build_model.c` | Done |
| 1.2 | P3 | Remove inline clears, add `process_by_galaxy` clear module | `sage_quasar_mode.c`, `sage_collisional_starburst.c`, new module, `millennium.yaml` | Done |
| 1.3 | P2 | Extract shared helpers, call from `sage_merge_galaxies` | `sage_merge_galaxies.c`, new `_shared/merger_physics.h` | Done |
| 1.4 | P1 | Update merger timestamp consumers to midpoint time (`ctx->substep_time`) | `sage_merge_galaxies.c` | Done |

### Phase 2: High Priority Fixes

| Order | Issue | Action | Files | Status (2026-03-06) |
|-------|-------|--------|-------|----------------------|
| 2.1 | P4 | Remove Type 2 eligibility bypass | `sage_update_merger_time.c` | Done |
| 2.2 | P5 | Add MergTime=0.0 for Type 0→2 | `build_model.c` | Pending |
| 2.3 | P6 | Restore per-subhalo central assignment for strict parity | `build_model.c`, modules using `CentralHalo` | Pending |

### Phase 3: Medium/Low Priority

| Order | Issue | Action | Files | Status (2026-03-06) |
|-------|-------|--------|-------|----------------------|
| 3.1 | P7 | Fix virial mass condition | `virial.c` | Skipped |
| 3.2 | P8 | Fix documentation | `module_registry.h` | Done |
| 3.3 | P9 | Add lineage fields if needed | `model_properties.yaml` | Skipped |

### Phase 4: Validation

1. Run existing test suite (`make tests`)
2. Create parity test fixture with known merger tree
3. Compare event counts: mergers, disruptions, BH accretion, starbursts
4. Compare output distributions: BH mass, stellar mass, ICS

---

## Vision Principle Compliance Summary

| Principle | P1 | P2 | P3 | P4 | P5 | P6 | P7 | P8 | P9 |
|-----------|----|----|----|----|----|----|----|----|----|
| 1. Physics-Agnostic Core | ✅ | ✅ | ✅ | ✅ | ⚠️ | ✅ | ✅ | N/A | ✅ |
| 2. Runtime Modularity | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | N/A | ✅ |
| 3. Metadata-Driven | N/A | ✅ | N/A | N/A | N/A | N/A | N/A | N/A | ✅ |
| 4. Single Source of Truth | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | N/A | ✅ |
| 5. Unified Processing | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️ | ✅ | N/A | ✅ |

⚠️ = Minor relaxation acceptable for SAGE parity

---

## Quick Reference: Files to Modify

```
src/core/build_model.c          # P1, P5, P6
src/core/virial.c               # P7
src/core/module_registry.h      # P8
src/modules/sage_update_merger_time.c      # P4
src/modules/sage_merge_galaxies.c          # P1, P2 (midpoint timestamps + helper calls)
src/modules/sage_quasar_mode.c             # P3 (remove inline clears)
src/modules/sage_collisional_starburst.c   # P3 (remove inline clears)
src/modules/model_properties.yaml          # P9 (if lineage fields needed)
input/millennium.yaml                      # P3 (add clear module)

NEW FILES:
src/modules/_shared/merger_physics.h       # P2 (shared BH growth + starburst helpers)
src/modules/_shared/merger_physics.c       # P2 (implementation)
src/modules/sage_clear_disk_instability_triggers.c  # P3 (process_by_galaxy clear)
```

---

## Acceptance Criteria

- [ ] All substep time calculations use correct base reference (P1)
- [ ] Merger timestamps (`TimeOfLastMajorMerger`, `TimeOfLastMinorMerger`) use corrected substep midpoint timing (P1)
- [x] Merger-triggered BH growth occurs for every merger event (P2)
- [x] Merger-triggered starburst occurs for every merger event (P2)
- [x] Multiple mergers per central per substep handled correctly (P2, Amendment C2)
- [x] Disk instability starburst occurs for every unstable disk (P3)
- [x] Clear modules use `process_by_galaxy` and run after consumers (P3, Amendment C1)
- [x] Triggers don't leak across phases (P3, Amendment C3)
- [ ] Type 2 orphans with high baryons protected in early substeps (P4)
- [ ] Type 0→2 transitions trigger immediate merge (P5)
- [ ] Per-subhalo central semantics restored and validated against SAGE behavior (P6-B)
- [ ] No test regressions
- [ ] Parity metrics within acceptable tolerance

---

## Amendment Log

### Amendment 2026-03-06: Critical Corrections (C1-C3)

**Source**: `obsidian-inbox/mimic-sage-parity-action-plan-critical-amendments-2026-03-06.md`

#### C1: Clear Stage Placement Corrected

**Problem**: Original plan specified `process_full_halo` for clear module, but `execute_phase()` runs ALL full-halo modules BEFORE by-galaxy modules regardless of YAML order.

**Fix**: Clear modules must use `process_by_galaxy` mode to run after consumer modules.

**Impact**: P3 solutions updated to use `process_by_galaxy` clear modules.

#### C2: Multi-Merger Semantics Added

**Problem**: Original P2-A event-contract used single scalar payload which would overwrite prior events when multiple satellites merge into same central in one substep.

**Fix**: Changed P2-A recommendation to shared helpers approach (inline execution matching SAGE). Event queue alternative documented as P2-B with explicit queue semantics.

**Impact**: P2 recommendation changed from event-contract to parity bridge (shared helpers). This is now the fastest path to strict SAGE parity.

#### C3: Trigger Lifecycle Across Phases Clarified

**Problem**: Original plan focused on phase_1 ordering but didn't specify full lifecycle: phase_1 → phase_2 → next substep.

**Fix**: P3-A now recommends split trigger channels (disk instability vs merger) with phase-specific clears. P3-B offers unified clear at end of every phase.

**Impact**: Cleaner trigger ownership, no cross-phase contamination.

### Design Decisions Resulting from Amendments

1. **P2 Merger Physics**: Use shared helpers called inline from `sage_merge_galaxies` (matches SAGE exactly)
2. **P3 Trigger Clearing**: Use `process_by_galaxy` clear module, placed last among by-galaxy modules in each phase
3. **Trigger Channels**: Separate disk instability triggers from merger triggers for clean lifecycle management

### Amendment 2026-03-06: Final Parity Locks (F1-F2)

**Source**: `obsidian-inbox/mimic-sage-parity-action-plan-final-amendments-2026-03-06.md`

#### F1: Merger Timestamp Consumer Alignment Added

**Problem**: Fixing `ctx->substep_time` base alone is insufficient if merger timestamp consumers continue writing `ctx->time`.

**Fix**: Added explicit implementation step and acceptance criterion requiring `TimeOfLastMajorMerger` and `TimeOfLastMinorMerger` to use midpoint timing (`ctx->substep_time`).

**Impact**: Merger-event timing parity is now explicitly testable and no longer implicit.

#### F2: P6 Parity Policy Locked

**Problem**: P6 was previously framed as \"document or restore\", which is ambiguous for strict parity execution.

**Fix**: Locked P6 recommendation to restore per-subhalo central semantics (P6-B) for this parity plan.

**Impact**: Plan scope now matches strict Mimic-SAGE parity target and removes policy ambiguity.
