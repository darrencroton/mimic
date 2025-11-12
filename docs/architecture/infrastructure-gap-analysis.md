# Infrastructure Gap Analysis: Automatic Module Discovery

**Date**: 2025-11-12
**Triggered By**: Implementation of sage_infall module revealed architectural violations
**Status**: ✅ **RESOLVED** - Implemented 2025-11-12
**Resolution**: Phase 4.2.5 completed - automatic module discovery system fully operational

---

## Executive Summary

The implementation of the `sage_infall` module has revealed a **critical gap** between our architectural vision and current implementation. We are violating our own core principles:

**Violated Principles**:
1. **Metadata-Driven Architecture** (Principle 3): Module registration is hardcoded, not metadata-driven
2. **Single Source of Truth** (Principle 4): Module information duplicated across 3+ files
3. **Type Safety and Validation** (Principle 8): No build-time verification of module consistency

**Current Pain Points** (discovered during sage_infall implementation):
1. Added `#include "sage_infall/sage_infall.h"` to `src/modules/module_init.c`
2. Added `sage_infall_register()` call to `register_all_modules()`
3. Added `sage_infall.c` to `tests/unit/run_tests.sh` MODULE_SRCS
4. Will need to update module documentation manually
5. **All of these are manual, error-prone, and violate our architecture**

**Recommendation**: **Implement automatic module discovery NOW**, before implementing sage_cooling. This was planned for Phase 5, but we've hit the pain point in Phase 4, so we should address it immediately.

---

## Current State: Manual Synchronization Hell

### What We Currently Do (Wrong)

When adding a new module, developers must manually update **4-5 locations**:

#### 1. Module Implementation (`src/modules/sage_infall/`)
✅ **Good**: This is the single source of truth for the module itself
- `sage_infall.c` - Implementation
- `sage_infall.h` - Header

#### 2. Module Registration (`src/modules/module_init.c`)
❌ **Manual**: Must add include and registration call
```c
#include "sage_infall/sage_infall.h"  // Manual addition

void register_all_modules(void) {
  sage_infall_register();   // Manual addition
  simple_cooling_register();
  simple_sfr_register();
}
```

#### 3. Test Build System (`tests/unit/run_tests.sh`)
❌ **Manual**: Must add to MODULE_SRCS variable
```bash
MODULE_SRCS="... ${SRC_DIR}/modules/sage_infall/sage_infall.c ..."
```

#### 4. Documentation (`docs/user/module-configuration.md`)
❌ **Manual**: Must document new module's parameters

#### 5. Makefile (`Makefile` - currently automatic via wildcard)
✅ **Automatic**: `MODULES = $(wildcard src/modules/*/)`
⚠️ **Partial**: Compiles automatically but doesn't handle metadata

### The Problem

This creates **4 opportunities for error** every time we add a module:
- **Forgot include?** Compilation error
- **Forgot registration?** Module silently unavailable
- **Forgot test script?** Unit tests fail
- **Forgot documentation?** Users confused

**This is exactly the kind of manual synchronization our metadata-driven architecture was designed to eliminate.**

---

## Vision State: Metadata-Driven Module Discovery

### What We Should Do (Right)

When adding a new module, developers should only need to:

1. **Create module directory** with implementation
2. **Create module metadata file** describing the module
3. **Run `make`** - everything else automatic

That's it. No manual synchronization.

### How It Should Work

#### Step 1: Module Metadata File

Each module contains a `module_info.yaml`:

```yaml
# src/modules/sage_infall/module_info.yaml
module:
  name: sage_infall
  display_name: "SAGE Infall"
  description: "Cosmological gas infall and satellite stripping from SAGE model"
  version: "1.0.0"
  author: "Mimic Team (ported from SAGE)"

  # Source files (relative to module directory)
  sources:
    - sage_infall.c

  headers:
    - sage_infall.h

  # Registration function name (must match implementation)
  register_function: sage_infall_register

  # Module dependencies (execution order)
  dependencies:
    requires: []  # No dependencies
    provides:
      - HotGas
      - MetalsHotGas
      - EjectedMass
      - MetalsEjectedMass
      - ICS
      - MetalsICS
      - TotalSatelliteBaryons

  # Parameters with defaults
  parameters:
    - name: BaryonFrac
      type: double
      default: 0.17
      range: [0.0, 1.0]
      description: "Cosmic baryon fraction (Omega_b / Omega_m)"

    - name: ReionizationOn
      type: int
      default: 1
      range: [0, 1]
      description: "Enable reionization suppression (0=off, 1=on)"

    - name: Reionization_z0
      type: double
      default: 8.0
      range: [0.0, 20.0]
      description: "Redshift when UV background turns on"

    - name: Reionization_zr
      type: double
      default: 7.0
      range: [0.0, 20.0]
      description: "Redshift of full reionization"

    - name: StrippingSteps
      type: int
      default: 10
      range: [1, 100]
      description: "Number of substeps for satellite stripping"

  # Testing requirements
  tests:
    unit: test_sage_infall.c
    integration: test_sage_infall.py
    scientific: test_sage_infall_validation.py

  # Documentation
  docs:
    physics: docs/physics/sage-infall.md
    user_guide_section: "SAGE Infall Module"

  # References
  references:
    - "Gnedin (2000) - Reionization model"
    - "Kravtsov et al. (2004) - Filtering mass formulas"
    - "Croton et al. (2016) - SAGE model description"
```

#### Step 2: Auto-Generation Script

**Script**: `scripts/generate_module_registry.py`

**Scans**: All `src/modules/*/module_info.yaml` files

**Generates**:

1. **`src/modules/module_init.c`** - Auto-generated registration code:
```c
/* AUTO-GENERATED FILE - DO NOT EDIT MANUALLY */
/* Generated from module metadata by scripts/generate_module_registry.py */
/* Source: src/modules/*/module_info.yaml */

#include "module_registry.h"

/* Auto-generated module includes */
#include "sage_infall/sage_infall.h"
#include "simple_cooling/simple_cooling.h"
#include "simple_sfr/simple_sfr.h"

/**
 * @brief Register all available physics modules
 *
 * Modules registered: 3
 * - sage_infall (SAGE Infall)
 * - simple_cooling (Simple Cooling)
 * - simple_sfr (Simple Star Formation)
 */
void register_all_modules(void) {
  /* Register in dependency order */
  sage_infall_register();      /* Provides: HotGas, MetalsHotGas, ... */
  simple_cooling_register();   /* Provides: ColdGas */
  simple_sfr_register();       /* Requires: ColdGas */
}
```

2. **`tests/unit/module_sources.mk`** - Auto-generated makefile fragment:
```makefile
# AUTO-GENERATED FILE - DO NOT EDIT MANUALLY
# Generated from module metadata by scripts/generate_module_registry.py

MODULE_SRCS = \
    $(SRC_DIR)/core/module_registry.c \
    $(SRC_DIR)/modules/sage_infall/sage_infall.c \
    $(SRC_DIR)/modules/simple_cooling/simple_cooling.c \
    $(SRC_DIR)/modules/simple_sfr/simple_sfr.c \
    $(SRC_DIR)/modules/module_init.c
```

3. **`docs/user/module-reference.md`** - Auto-generated documentation:
```markdown
# Module Reference

Auto-generated from module metadata.

## Available Modules

### sage_infall - SAGE Infall

Cosmological gas infall and satellite stripping from SAGE model.

**Provides**: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS

**Parameters**:
- `SageInfall_BaryonFrac` (double, default: 0.17, range: [0.0, 1.0])
  Cosmic baryon fraction (Omega_b / Omega_m)

- `SageInfall_ReionizationOn` (int, default: 1, range: [0, 1])
  Enable reionization suppression (0=off, 1=on)

[etc...]

**References**:
- Gnedin (2000) - Reionization model
- Kravtsov et al. (2004) - Filtering mass formulas

---

### simple_cooling - Simple Cooling
[etc...]
```

4. **`scripts/validate_modules.py`** - Validation script:
- Verifies all referenced files exist
- Checks for circular dependencies
- Validates parameter naming conventions
- Ensures register functions exist

#### Step 3: Build System Integration

**Update `Makefile`**:

```makefile
# Auto-generate module registration code before compilation
.PHONY: generate-modules
generate-modules:
	@echo "Generating module registration code..."
	@python3 scripts/generate_module_registry.py

# Make compilation depend on module generation
build/obj/modules/module_init.o: src/modules/module_init.c generate-modules
	$(CC) $(CFLAGS) -c $< -o $@

# Regenerate if any module metadata changes
src/modules/module_init.c: $(wildcard src/modules/*/module_info.yaml)
	@$(MAKE) generate-modules

# Validation target
.PHONY: validate-modules
validate-modules:
	@echo "Validating module metadata..."
	@python3 scripts/validate_modules.py
```

**Update `tests/unit/run_tests.sh`**:

```bash
# Source auto-generated module list
if [ -f "tests/unit/module_sources.mk" ]; then
    # Extract MODULE_SRCS from makefile fragment
    MODULE_SRCS=$(grep -E '^\s+\$' tests/unit/module_sources.mk | \
                  sed 's/\$(SRC_DIR)/'"${SRC_DIR}"'/g' | \
                  tr '\n' ' ')
else
    echo "ERROR: Module sources not generated. Run 'make generate-modules'"
    exit 2
fi
```

---

## Implementation Plan

### Phase 5-Early: Automatic Module Discovery (1 week)

This should be implemented **before sage_cooling**, because every new module will benefit.

#### Week 1: Infrastructure Implementation

**Day 1-2: Design & Schema**
- [ ] Finalize `module_info.yaml` schema
- [ ] Write schema documentation
- [ ] Create example module_info.yaml files for existing modules

**Day 3-4: Generation Script**
- [ ] Implement `scripts/generate_module_registry.py`
  - YAML parsing
  - Dependency resolution (topological sort)
  - Code generation (module_init.c)
  - Makefile fragment generation
  - Documentation generation
- [ ] Implement `scripts/validate_modules.py`
  - File existence checks
  - Circular dependency detection
  - Parameter naming validation
  - Register function verification

**Day 5: Integration**
- [ ] Update Makefile to call generation scripts
- [ ] Update test build system
- [ ] Add CI checks for validation

**Day 6: Migration**
- [ ] Create module_info.yaml for sage_infall
- [ ] Create module_info.yaml for simple_cooling
- [ ] Create module_info.yaml for simple_sfr
- [ ] Regenerate all auto-generated files
- [ ] Verify tests still pass

**Day 7: Documentation**
- [ ] Document module_info.yaml schema
- [ ] Update module developer guide
- [ ] Update roadmap (Phase 5 partially complete)
- [ ] Add to next-task.md

### Benefits

**Immediate Benefits** (after 1 week):
1. ✅ No manual synchronization when adding modules
2. ✅ Build-time validation of module consistency
3. ✅ Auto-generated documentation stays in sync
4. ✅ Reduced developer cognitive load
5. ✅ Fewer opportunities for error

**Long-Term Benefits**:
1. ✅ Enables build-time module selection (Phase 5 goal)
2. ✅ Can generate IDE configuration files
3. ✅ Can generate module dependency graphs
4. ✅ Foundation for plugin system
5. ✅ Enables automatic test generation

---

## Comparison: Manual vs Automatic

### Adding a New Module: Manual (Current)

**Time**: 30-60 minutes
**Error Opportunities**: 4-5

1. ⏱️ 5 min: Create module files
2. ⏱️ 5 min: Add include to module_init.c
3. ⏱️ 2 min: Add registration call
4. ⏱️ 5 min: Update test script
5. ⏱️ 10 min: Compile and debug registration errors
6. ⏱️ 10 min: Document parameters manually
7. ⏱️ 5 min: Update module list documentation

**Risk**: Forget any step → errors or silent failures

### Adding a New Module: Automatic (Proposed)

**Time**: 5-10 minutes
**Error Opportunities**: 1 (module_info.yaml)

1. ⏱️ 5 min: Create module files
2. ⏱️ 3 min: Create module_info.yaml
3. ⏱️ 1 min: Run `make`
4. ⏱️ 1 min: Everything auto-generated and validated

**Risk**: Only need to get module_info.yaml right (schema-validated!)

**Time Saved**: 20-50 minutes per module × 6 modules = **2-5 hours saved in Phase 4**

---

## Alignment with Architectural Vision

This implementation directly addresses **4 of our 8 core principles**:

### ✅ Principle 3: Metadata-Driven Architecture
**Before**: Module registration is hardcoded
**After**: Module registration driven by `module_info.yaml` metadata

### ✅ Principle 4: Single Source of Truth
**Before**: Module info duplicated across 4+ files
**After**: `module_info.yaml` is the single source, everything else generated

### ✅ Principle 5: Unified Processing Model
**Before**: Manual dependency ordering, error-prone
**After**: Automatic dependency resolution from metadata

### ✅ Principle 8: Type Safety and Validation
**Before**: No validation until runtime/test time
**After**: Build-time validation of module consistency

---

## Decision: Implement Now or Later?

### Option A: Implement Now (Recommended)

**Pros**:
- Saves time on remaining 5 modules (sage_cooling, reincorporation, etc.)
- Validates architecture before investing more in modules
- Prevents accumulating technical debt
- sage_cooling benefits immediately

**Cons**:
- 1 week delay before sage_cooling
- Additional complexity to learn

**Total Time**: +1 week now, -2 to 5 hours later = **Net time savings**

### Option B: Defer to Phase 5

**Pros**:
- Continue momentum on physics implementation
- Simpler near-term workflow

**Cons**:
- Manual synchronization for 5 more modules
- Risk of inconsistency accumulation
- Harder to refactor later with more modules
- Violates architectural principles for longer

**Total Time**: No delay now, +30-60 min per module × 5 = **Net time loss**

---

## Recommendation

**Implement automatic module discovery NOW**, before sage_cooling.

**Rationale**:
1. **Architectural Integrity**: We've violated our own principles - fix it immediately
2. **Time Efficiency**: 1 week investment saves 2-5 hours over remaining modules
3. **Quality**: Reduces error opportunities by 75% (4-5 → 1)
4. **Momentum**: Better to fix architecture early than accumulate debt
5. **Validation**: Proves our metadata-driven vision actually works

**Suggested Timeline**:
- Week 1: Implement module discovery infrastructure
- Week 2: Begin sage_cooling with clean workflow
- Weeks 3-4: Complete sage_cooling
- Future modules: Benefit from clean, automatic workflow

---

## Implementation Checklist

When implementing, ensure:

- [ ] YAML schema is well-documented
- [ ] Schema includes versioning for future evolution
- [ ] Generation script is idempotent (can run multiple times safely)
- [ ] Generated files have clear "AUTO-GENERATED" warnings
- [ ] Validation script catches all common errors
- [ ] Makefile correctly handles dependencies
- [ ] CI validates modules on every commit
- [ ] Developer guide updated with new workflow
- [ ] Migration path documented for existing modules
- [ ] All existing modules migrated successfully
- [ ] Tests pass after migration

---

## Conclusion

The implementation of sage_infall has revealed a critical gap: **we are not living our metadata-driven architectural vision**. We have the opportunity to fix this now, before it becomes entrenched technical debt.

**The gap is clear**: Manual synchronization across multiple files violates principles 3, 4, 5, and 8.

**The solution is clear**: Automatic module discovery from metadata, as originally envisioned for Phase 5.

**The timing is right**: Do it now, before sage_cooling, to validate the approach and benefit immediately.

This is not scope creep - this is **architectural integrity**. We designed Mimic to be metadata-driven. Now we need to actually be metadata-driven.

---

**Next Steps**: Get approval to implement automatic module discovery, then proceed with clean workflow for sage_cooling and beyond.
