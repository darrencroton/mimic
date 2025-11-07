# Proof-of-Concept Round 2: Module Interaction and Time Evolution

**Date**: 2025-11-07
**Branch**: feature/minimal-module-poc (continue on this branch)
**Purpose**: Test module interaction, execution ordering, and time evolution before implementing full roadmap
**Time Budget**: 1-2 days maximum

---

## Context

### What We've Already Done (PoC Round 1)

Successfully implemented a minimal module system with one simple module:

**Implementation:**
- ✅ Separate `GalaxyData` structure with physics-agnostic allocation
- ✅ Basic module interface (init/process/cleanup)
- ✅ Simple module registry (manual registration)
- ✅ Module execution pipeline integration
- ✅ Deep copy galaxy data inheritance
- ✅ Binary output extension
- ✅ One module: `stellar_mass` with physics `StellarMass = 0.1 * Mvir`
- ✅ Plotting: stellar mass function (snapshot + evolution)

**Key Files:**
- `src/include/galaxy_types.h` - GalaxyData with StellarMass
- `src/core/module_interface.h` - Module interface definition
- `src/core/module_registry.[ch]` - Registry implementation
- `src/modules/stellar_mass/*` - PoC module
- `output/mimic-plot/figures/stellar_mass_function.py` - SMF snapshot plot
- `output/mimic-plot/figures/smf_evolution.py` - SMF evolution plot

**Current Module Interface (Minimal):**
```c
struct Module {
    const char *name;
    int (*init)(void);
    int (*process_halos)(struct Halo *halos, int ngal);
    int (*cleanup)(void);
};
```

**Lessons Learned:**
- Memory management doesn't scale (needed 64x block limit increase)
- Dual-struct synchronization is painful (7 files touched per property)
- Module interface too minimal for realistic physics

### What We Need to Test (PoC Round 2)

The current implementation has **one simple module** that doesn't interact with anything. We need to test:

1. **Module Interaction**: Module B reads properties written by Module A
2. **Execution Ordering**: Does order matter? (it should for physics)
3. **Time Evolution**: Modules need access to timestep (Δt) and changes (ΔMvir)
4. **Module Context Requirements**: What do modules actually need beyond halo array?
5. **Property Dependencies**: How do modules communicate through properties?
6. **Parameter Access**: How should modules read configuration parameters?

**The roadmap proposes solutions** (ModuleContext, enriched interface, etc.) **but we haven't validated them with real code.**

---

## Goals for PoC Round 2

### Primary Goals

1. **Validate Module Interaction Pattern**
   - Does property-based communication work?
   - Can stellar_mass module read ColdGas property written by cooling module?
   - Are there hidden coupling issues?

2. **Test Execution Ordering**
   - Does cooling → stellar_mass ordering work correctly?
   - What happens if we reverse the order?
   - Can we detect ordering errors?

3. **Identify Module Context Requirements**
   - What simulation state do modules actually need?
   - Do we need timestep (Δt)?
   - Do we need previous halo state (to calculate ΔMvir)?
   - Do we need redshift?

4. **Validate Time Evolution**
   - Can modules track state across timesteps?
   - Does progenitor inheritance work for evolving properties?
   - Are there numerical stability issues?

5. **Create Concrete Test Case**
   - Two interacting modules for testing framework
   - Known physics for validation
   - Example for future module developers

### Secondary Goals

1. Update roadmap_v2.md with any new findings
2. Document additional gaps discovered
3. Refine Phase 1.5 (ModuleContext) requirements
4. Create reference implementation patterns

---

## Implementation Specification

### Module 1: Simple Cooling (`simple_cooling`)

**Physics**: Cool gas from virial reservoir into cold disk

**Prescription:**
```
ΔColdGas = f_baryon * ΔMvir
```

Where:
- `f_baryon = 0.15` (baryon fraction parameter)
- `ΔMvir = Mvir(current) - Mvir(progenitor)` (mass accreted since last timestep)
- `ColdGas` accumulates over time (persists through progenitor inheritance)

**Physical Interpretation:**
- Halo accretes dark matter (ΔMvir)
- 15% of accreted mass is baryons
- Baryons cool into cold gas disk
- No gas removal (no star formation in this module)

**Property Created:**
- `ColdGas` (float, units: 10^10 Msun/h)

**Implementation Notes:**
- Need to know: Mvir(current), Mvir(progenitor) → requires tracking or calculating ΔMvir
- First timestep (no progenitor): assume all current Mvir is newly accreted
- Must handle Type 0 (central) galaxies only initially

### Module 2: Simple Star Formation (`stellar_mass` - MODIFIED)

**Physics**: Form stars from cold gas using Kennicutt-Schmidt-like law

**Prescription:**
```
ΔStellarMass = ε_SF * ColdGas * (Vvir / Rvir) * Δt
```

Where:
- `ε_SF = 0.02` (star formation efficiency parameter)
- `ColdGas` (from cooling module)
- `Vvir / Rvir` is dynamical time^(-1) (units: 1/time)
- `Δt` is timestep (time between snapshots)
- `ColdGas` is depleted by star formation

**Physical Interpretation:**
- Star formation rate proportional to gas mass and dynamical time
- Efficiency of 2%
- Cold gas is consumed (removed from ColdGas reservoir)
- Stars accumulate in StellarMass

**Properties Modified:**
- `ColdGas` (float, units: 10^10 Msun/h) - DEPLETED
- `StellarMass` (float, units: 10^10 Msun/h) - GROWN

**Implementation Notes:**
- Need to know: ColdGas (from cooling module), Vvir, Rvir, Δt
- Vvir and Rvir should be available in Halo struct (check virial.c)
- Δt needs to come from somewhere (snapshot time difference)
- Update: `ColdGas -= ΔStellarMass` and `StellarMass += ΔStellarMass`
- Must run AFTER cooling module (order matters!)

### Execution Order

**CRITICAL**: Modules must execute in this order:
1. `simple_cooling` (writes ColdGas)
2. `stellar_mass` (reads ColdGas, writes StellarMass)

Reversing the order should either:
- Produce wrong results (stellar_mass sees stale ColdGas)
- Produce error (stellar_mass sees uninitialized ColdGas = 0)

This tests whether ordering matters and can be detected.

---

## Key Questions to Answer

### Q1: Module Context - What's Actually Needed?

**Current**: Modules receive only `struct Halo *halos, int ngal`

**For this PoC, modules need:**
- Current snapshot redshift/time
- Previous snapshot time (to calculate Δt)
- Previous halo state (to calculate ΔMvir)

**Questions:**
1. Where does this information come from?
2. Should we pass it explicitly in a context structure?
3. Or should modules access it through globals?
4. What's the cleanest pattern?

**Proposed (for testing):**
```c
struct ModuleContext {
    double time;           // Current snapshot time
    double time_prev;      // Previous snapshot time
    double redshift;       // Current redshift
    // More as needed
};

// Updated interface
int (*process_halos)(struct ModuleContext *ctx, struct Halo *halos, int ngal);
```

**Action**: Implement minimal ModuleContext with just what we need, document what's missing.

### Q2: Timestep (Δt) - How to Calculate? (NOTE: might already be calculated as deltaT - CHECK)

**Options:**
1. Pass snapshot times in ModuleContext, modules calculate Δt
2. Pre-calculate Δt in core, pass in ModuleContext
3. Store timestep in Halo struct (accessible to all modules)

**Questions:**
- Which approach is cleanest?
- Where is time information available in current code? (check metadata structure)
- Is Δt the same for all halos in a snapshot? (yes - snapshot-level property)

**Action**: Explore current code to find time/redshift information, implement cleanest approach.

### Q3: ΔMvir - How to Calculate? (NOTE: might already be calculated as deltaMvir - CHECK)

**Cooling module needs**: `ΔMvir = Mvir(current) - Mvir(progenitor)`

**Current implementation**: Halos inherit from progenitors in `copy_progenitor_halos()`, but we don't track previous Mvir.

**Options:**
1. Store previous Mvir in GalaxyData (e.g., `MvirPrev`)
2. Calculate on-the-fly (current Mvir - progenitor's current Mvir)
3. Modules track their own state

**Questions:**
- Is Mvir from progenitor still available when module runs?
- Should ΔMvir be calculated once and stored, or calculated per-module?
- For first timestep (no progenitor), how to handle?

**Action**: Determine cleanest pattern for accessing previous state.

### Q4: Property Dependencies - How Explicit?

**Stellar mass module depends on cooling**: Needs ColdGas property.

**Questions:**
- Should dependencies be declared explicitly? (e.g., stellar_mass declares "requires: cooling")
- Or is property-based implicit dependency sufficient?
- What happens if stellar_mass runs but ColdGas = 0? (cooling didn't run or ran after)
- Should we validate property existence before reading?

**Action**: Test both scenarios (correct order and wrong order), see what breaks.

### Q5: Property Initialization - Who's Responsible?

**ColdGas property:**
- Created by cooling module
- Read/modified by stellar_mass module

**Questions:**
- Should ColdGas be initialized to 0 in `init_halo()` (core)?
- Or should cooling module initialize it on first access?
- What if stellar_mass runs first and ColdGas is uninitialized?

**Current Pattern** (from stellar_mass PoC):
- Properties initialized to 0 in `init_halo()` (virial.c)

**Action**: Verify this pattern works for multi-module interaction.

### Q6: Module Parameters - How to Access?

**Both modules have parameters:**
- `Cooling_BaryonFraction = 0.15`
- `StarFormation_Efficiency = 0.02`

**Questions:**
- How should modules read these from parameter file?
- Should we extend .par format now or hardcode for PoC?
- Is prefixed parameter naming sufficient? (`Cooling_BaryonFraction`)

**Action**: Either hardcode or implement minimal parameter reading. Document requirements for Phase 3.

### Q7: Numerical Stability - Any Issues?

**Concerns:**
- Star formation could consume more ColdGas than available
- Negative gas masses
- Mass conservation

**Questions:**
- Do we need to clamp `ΔStellarMass <= ColdGas`?
- Should modules check for negative values?
- Who enforces conservation laws?

**Action**: Implement safely (clamp values), document validation needs.

---

## Implementation Tasks

### Task 1: Add ColdGas Property

**Files to Modify:**
1. `src/include/galaxy_types.h`
   ```c
   struct GalaxyData {
       float StellarMass;
       float ColdGas;      // NEW
   };
   ```

2. `src/include/types.h`
   ```c
   struct HaloOutput {
       // ... existing fields ...
       float StellarMass;
       float ColdGas;      // NEW
   };
   ```

3. `src/core/halo_properties/virial.c` (init_halo)
   - Initialize ColdGas to 0.0

4. `src/io/output/binary.c` (prepare_halo_for_output)
   - Copy ColdGas from galaxy to output

5. `output/mimic-plot/mimic-plot.py`
   - Add ColdGas to binary reader dtype

6. `output/mimic-plot/hdf5_reader.py` (if using HDF5)
   - Add ColdGas to HDF5 reader dtype

### Task 2: Create Simple Cooling Module

**Create Directory:**
```bash
mkdir -p src/modules/simple_cooling
```

**Files to Create:**

1. `src/modules/simple_cooling/simple_cooling.h`
   ```c
   #ifndef SIMPLE_COOLING_H
   #define SIMPLE_COOLING_H

   void simple_cooling_register(void);

   #endif
   ```

2. `src/modules/simple_cooling/simple_cooling.c`
   ```c
   #include "simple_cooling.h"
   #include "module_interface.h"
   #include "types.h"
   #include "error.h"
   #include <stdio.h>

   static const float BARYON_FRACTION = 0.15;

   static int simple_cooling_init(void) {
       INFO_LOG("Simple cooling module initialized (f_baryon = %.2f)", BARYON_FRACTION);
       return 0;
   }

   static int simple_cooling_process(struct Halo *halos, int ngal) {
       // TODO: Implement cooling physics
       // ΔColdGas = BARYON_FRACTION * ΔMvir

       for (int i = 0; i < ngal; i++) {
           // Only process central galaxies
           if (halos[i].Type != 0) continue;

           // Calculate ΔMvir (need to figure out how)
           // For now: Question to answer in PoC

           // Update ColdGas
           // halos[i].galaxy->ColdGas += BARYON_FRACTION * delta_mvir;
       }

       return 0;
   }

   static int simple_cooling_cleanup(void) {
       INFO_LOG("Simple cooling module cleaned up");
       return 0;
   }

   static struct Module simple_cooling_module = {
       .name = "simple_cooling",
       .init = simple_cooling_init,
       .process_halos = simple_cooling_process,
       .cleanup = simple_cooling_cleanup
   };

   void simple_cooling_register(void) {
       register_module(&simple_cooling_module);
   }
   ```

3. `src/modules/simple_cooling/README.md` - Document module

**Key Implementation Questions:**
- How to get ΔMvir? (see Q3 above)
- Do we need ModuleContext for time info?

### Task 3: Modify Stellar Mass Module

**File to Modify:** `src/modules/stellar_mass/stellar_mass.c`

**Changes:**

1. Update physics prescription from `StellarMass = 0.1 * Mvir` to:
   ```c
   ΔStellarMass = ε_SF * ColdGas * (Vvir / Rvir) * Δt
   ```

2. Deplete ColdGas:
   ```c
   ColdGas -= ΔStellarMass
   ```

3. Add efficiency parameter:
   ```c
   static const float SF_EFFICIENCY = 0.02;
   ```

4. Get Vvir, Rvir from Halo struct (check what's available)

5. Get Δt (see Q2 above)

**Implementation Questions:**
- Are Vvir and Rvir in Halo struct? (check virial.c, types.h)
- If not, can we calculate from Mvir? (Vvir ~ sqrt(G*Mvir/Rvir), Rvir from virial relations)
- How to get Δt?

### Task 4: Update Module Registration

**File to Modify:** `src/core/main.c`

```c
// Add cooling module registration
#include "../modules/simple_cooling/simple_cooling.h"

// In main():
simple_cooling_register();  // BEFORE stellar_mass
stellar_mass_register();
module_system_init();
```

**Test ordering**: What if we register stellar_mass first? Should still work (registration order doesn't matter), but execution order should be cooling → stellar_mass.

### Task 5: Implement Module Execution Ordering

**File to Modify:** `src/core/module_registry.c`

**Current**: Modules execute in registration order (undefined).

**Needed**: Explicit ordering control.

**For PoC**: Hardcode order as cooling → stellar_mass.

**Options:**
1. Hardcoded array of module names in desired order
2. Simple integer priority (cooling.priority = 1, stellar_mass.priority = 2)
3. User-specified order (deferred to Phase 3)

**Recommendation**: Hardcode for PoC, document need for Phase 3.

### Task 6: Add Plotting for ColdGas

**Goal**: Visualize cold gas mass function (like stellar mass function).

**Files to Create:**

1. `output/mimic-plot/figures/cold_gas_function.py`
   - Copy from `stellar_mass_function.py`
   - Change StellarMass → ColdGas
   - Update labels, titles

2. `output/mimic-plot/figures/cgf_evolution.py`
   - Copy from `smf_evolution.py`
   - Change StellarMass → ColdGas
   - Update labels, titles

**File to Modify:**

3. `output/mimic-plot/figures/__init__.py`
   - Register new plots
   - Add `get_cold_gas_label()` helper

**Validation**: Visually inspect plots to see if physics makes sense.

### Task 7: Test and Validate

**Tests to Run:**

1. **Correct Order** (cooling → stellar_mass)
   ```bash
   make clean && make
   ./mimic input/millennium.par
   ```
   - Check output has ColdGas and StellarMass
   - Check no memory leaks
   - Generate plots

2. **Wrong Order** (stellar_mass → cooling)
   - Swap registration order in main.c
   - Recompile and run
   - Observe what breaks (stellar_mass sees ColdGas=0?)

3. **Single Module** (cooling only)
   - Comment out stellar_mass registration
   - Run, verify ColdGas grows but StellarMass stays 0

4. **Mass Conservation**
   - Check: ColdGas + StellarMass ≈ expected from baryon fraction
   - Plot: ColdGas vs redshift, StellarMass vs redshift

**Expected Results:**
- ColdGas accumulates over time (higher at z=0)
- StellarMass grows as ColdGas is converted
- Gas depletion visible in plots
- Ordering matters (wrong order gives wrong results)

---

## Success Criteria

### Must Achieve

1. **Two Modules Working**
   - ✅ simple_cooling compiles and runs
   - ✅ stellar_mass (modified) compiles and runs
   - ✅ Both modules execute in correct order

2. **Property Interaction**
   - ✅ stellar_mass successfully reads ColdGas written by cooling
   - ✅ Gas depletion works (ColdGas decreases when stars form)
   - ✅ Properties persist across timesteps (inheritance works)

3. **Time Evolution**
   - ✅ Modules have access to Δt (timestep)
   - ✅ Can calculate ΔMvir (or determine it's not needed)
   - ✅ Properties evolve correctly over time

4. **Validation**
   - ✅ No memory leaks
   - ✅ Output files contain ColdGas and StellarMass
   - ✅ Plots show reasonable evolution

5. **Questions Answered**
   - ✅ Documented what ModuleContext needs (minimum viable)
   - ✅ Determined how to handle timesteps
   - ✅ Tested execution ordering importance
   - ✅ Validated property-based communication

### Nice to Have

1. Wrong order detection (stellar_mass warns if ColdGas=0)
2. Parameter reading for baryon fraction and SF efficiency
3. Mass conservation validation
4. Comparison plots (ColdGas vs StellarMass evolution)

---

## Deliverables

### Code
1. New module: `src/modules/simple_cooling/*`
2. Modified module: `src/modules/stellar_mass/*`
3. Updated core: `src/core/main.c` (registration)
4. New property: ColdGas in all relevant files
5. New plots: cold_gas_function.py, cgf_evolution.py

### Documentation
1. Update `docs/architecture/poc-lessons-learned.md` with Round 2 findings
2. Create `docs/architecture/poc-round2-results.md` with:
   - What worked
   - What didn't work
   - Answers to the 7 key questions
   - Recommendations for roadmap adjustments
   - New gaps discovered

### Validation
1. Successful execution on Millennium simulation
2. Memory leak report (should be zero)
3. Output plots showing ColdGas and StellarMass evolution
4. Comparison of correct vs wrong module ordering

---

## Implementation Notes

### Finding Information in Current Code

**Time/Redshift Information:**
- Check `metadata` structure passed to modules (might have redshift)
- Look in `src/core/build_model.c` for snapshot processing
- Check `struct RawHalo` or `struct Halo` for time info
- Examine snapshot list loading

**Virial Properties:**
- `src/core/halo_properties/virial.c` calculates halo properties
- Check what's stored in `struct Halo` (types.h)
- Look for Vvir, Rvir, or virial radius/velocity

**Progenitor Information:**
- `copy_progenitor_halos()` in build_model.c
- See what's available from progenitor
- Determine if we can access progenitor Mvir

### Placeholder Implementation Strategy

**If information not available**, use placeholders and document:

```c
// TODO: Need Δt from ModuleContext
double delta_t = 1.0;  // PLACEHOLDER: assume 1 Gyr for now

// TODO: Need to calculate ΔMvir
double delta_mvir = halos[i].Mvir * 0.1;  // PLACEHOLDER: assume 10% growth
```

**Document each placeholder** - this tells us what's missing and validates need for ModuleContext.

### Validation Approach

**Visual Validation:**
- Cold gas mass function should look reasonable (compare shape to stellar mass function)
- Evolution: ColdGas higher at high-z (more gas), StellarMass higher at low-z (more stars)
- Depletion: ColdGas + StellarMass roughly constant (stars come from gas)

**Quantitative Validation:**
- Check total mass: ColdGas + StellarMass ≈ 0.15 * total Mvir accreted
- Star formation efficiency: StellarMass / (ColdGas + StellarMass) ≈ function of time

---

## Expected Timeline

**Day 1:**
- Add ColdGas property to all required files (2 hours)
- Create simple_cooling module skeleton (1 hour)
- Implement cooling physics (2 hours)
- Modify stellar_mass module (2 hours)
- Debug compilation issues (1 hour)

**Day 2:**
- Test execution and fix bugs (2 hours)
- Create plotting scripts (2 hours)
- Run on Millennium and validate (2 hours)
- Document findings (2 hours)

**Total**: 16 hours (2 days)

**Time-box**: If not complete in 2 days, document blockers and stop. The goal is quick validation, not perfection.

---

## What to Document

### During Implementation

Keep notes on:
1. **Blockers encountered**: What information wasn't available?
2. **Design decisions**: How did you solve problems? Why?
3. **Workarounds used**: Placeholders, assumptions, hacks
4. **Surprises**: Anything unexpected that worked or didn't work

### In Results Document

Answer these questions:
1. **Module Context**: What's the minimum viable ModuleContext?
2. **Time Evolution**: How should Δt and ΔMvir be provided?
3. **Execution Order**: Does ordering matter? Can we detect wrong order?
4. **Property Communication**: Does property-based interaction work smoothly?
5. **Missing Pieces**: What's needed that we don't have?
6. **Roadmap Impact**: Should roadmap_v2.md be updated? How?

---

## Starting Point for AI Coder

**Branch**: feature/minimal-module-poc (existing PoC Round 1 work)

**Entry Point**: `src/core/main.c` (where modules are registered)

**Key Files to Understand:**
- `src/include/galaxy_types.h` - GalaxyData structure
- `src/core/module_interface.h` - Module interface
- `src/modules/stellar_mass/stellar_mass.c` - Example module
- `src/core/build_model.c` - Where modules are executed

**Build and Test:**
```bash
make clean && make
./mimic input/millennium.par
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.par
```

**Validation:**
```bash
# Check for memory leaks (should be zero)
grep "Memory leaks" output/mimic.log

# Check plots generated
ls output/results/millennium/plots/
```

---

## Questions for AI Coder to Answer

As you implement, explicitly answer these in your documentation:

1. **Where did you find time/redshift information?** What's the best way to access it?

2. **How did you calculate ΔMvir?** Was progenitor Mvir accessible?

3. **How did you get Δt?** Was it calculated or passed?

4. **What did you add to ModuleContext?** What's the minimal viable context?

5. **Did execution ordering matter?** What happened with wrong order?

6. **Were there any subtle bugs** in property interaction?

7. **What's missing** that would make this easier?

8. **How should roadmap_v2.md be updated** based on what you learned?

---

## Final Notes

**Philosophy**: This is quick empirical validation, not production code. Use shortcuts and placeholders where needed, but document everything. The goal is to **discover what we don't know**, not to build perfect code.

**Success Metric**: Not "does it work perfectly?" but "did we learn what we needed to learn?"

**Output**: A clear set of findings that inform Phase 0-6 implementation, reducing risk of building the wrong thing.

Good luck! Document everything you discover.
