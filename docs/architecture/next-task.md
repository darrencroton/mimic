# Phase 4.3 Next Tasks: Module Integration & Validation

**Current Phase**: Phase 4.3 - Module Integration & Validation
**Status**: All 6 SAGE modules merged to main (November 18, 2025)
**Goal**: Systematically integrate and validate merged modules into a working pipeline

---

## Current State

### ✅ Completed (Phase 4.2)
- All 6 SAGE module branches merged to main
- All modules compile successfully (with 3 expected warnings in sage_mergers)
- Module registration system working (8 modules total)
- Property conflicts resolved during merge
- **Code quality improvements** (November 18, 2025):
  - safe_div() utility integration across all 4 new modules (18 divisions updated)
  - Compiler warnings resolved (unused parameters fixed, expected warnings documented)
  - All unit tests updated to current framework API (14/14 passing)

### ⏸️ In Progress (Phase 4.3)
- Module integration and validation
- Core infrastructure decisions
- Pipeline testing strategy

---

## Critical Decisions Needed

Before proceeding with systematic integration, several architectural and implementation decisions must be made:

### Decision 1: Module Enabling Strategy

**Question**: How should we systematically enable and validate modules?

**Options Under Consideration**:
1. Start minimal (infall + cooling only), add one module at a time
2. Enable all at once, debug issues as they arise
3. Enable by priority groups (foundation → core → advanced)

**Factors**:
- Need to validate each module's integration independently
- Want to catch property flow issues early
- Performance testing requires full pipeline
- SAGE comparison requires all modules

**To Decide**: Preferred approach and validation criteria for each step

---

### Decision 2: Merger Triggering Architecture

**Question**: Where and how should merger detection/triggering be implemented?

**Context**:
- sage_mergers module is complete but untestable
- Core doesn't detect when satellites should merge with centrals
- SAGE uses dynamical friction timescale to trigger mergers

**Options Under Consideration**:
1. New core module (`src/core/merger_detection.c`)
2. Integrated into tree processing (`src/core/build_model.c`)
3. Part of halo processing pipeline
4. Module-driven (merger module signals when to trigger)

**Key Questions**:
- Who owns merger detection logic?
- How are mergers marked/queued?
- When in processing cycle do mergers execute?
- How to handle multiple mergers in one timestep?

**To Decide**: Architecture, implementation location, API design

---

### Decision 3: Shared Function Architecture

**Question**: How to handle functions needed by multiple modules?

**Context**:
- sage_disk_instability needs functions from sage_mergers:
  - `collisional_starburst_recipe()`
  - `grow_black_hole()`
- Want to avoid code duplication
- Need to maintain module isolation

**Options Under Consideration**:
1. Extract to `src/modules/shared/` as utilities
2. Keep in owning module, expose via function pointers
3. Keep in owning module, call via module-to-module API
4. Duplicate code in each module (simple but not DRY)

**Factors**:
- Physics-agnostic core principle
- Module independence
- Code maintainability
- Testing complexity

**To Decide**: Architecture pattern for shared physics functions

---

### Decision 4: Module Pipeline Enhancement

**Question**: How to enhance module calling pipeline for better diagnostics and control?

**Context**:
- User wants to enhance pipeline execution in core
- Need better visibility into module execution
- Want validation hooks between modules
- Performance profiling needed

**Potential Enhancements**:
- Pre/post execution hooks
- Property validation between modules
- Execution timing/profiling
- Conditional module execution
- Module dependency verification

**To Decide**: Which enhancements to implement, priority order

---

### Decision 5: Property Organization & Verification

**Question**: How to organize and verify galaxy property definitions?

**Context**:
- 23 galaxy properties defined
- Some properties created by one module, modified by others
- Need clear ownership semantics
- Want to verify no duplicates/conflicts

**Tasks Needed**:
- Review all property `created_by` values
- Verify module `provides` vs `requires` in metadata
- Check for missing properties
- Organize properties logically in YAML
- Document property lifecycle

**To Decide**: Organization scheme, verification process

---

## Immediate Next Steps (This Week)

### Step 1: Parameter File Cleanup
- Remove all modules from `input/millennium.par` except sage_infall + sage_cooling
- Verify minimal pipeline runs successfully
- Document baseline behavior

### Step 2: Architecture Discussions
- Resolve Decision 1 (module enabling strategy)
- Resolve Decision 2 (merger triggering architecture)
- Resolve Decision 3 (shared function architecture)
- Document decisions in roadmap

### Step 3: Create Integration Task List
- Based on decided strategy, create detailed task breakdown
- Identify validation tests needed for each integration step
- Estimate timeline
- Update this document with concrete tasks

---

## Open Questions (For Discussion)

### Module Integration
1. What validation tests are needed when adding each module?
2. How to test module ordering dependencies?
3. What constitutes "passing" for each integration step?
4. How to handle modules that can't be fully tested yet (mergers)?

### Core Infrastructure
5. Where should merger detection logic live?
6. What data structures are needed for merger queue/marking?
7. How to call merger module when satellite is flagged for merger?
8. Should merger triggering be in its own module or core infrastructure?

### Shared Functions
9. Is code duplication acceptable for 1-2 shared functions?
10. If extracting to shared utilities, what's the API?
11. How do modules discover/call shared functions?
12. What's the testing strategy for shared utilities?

### Property System
13. Are all `created_by` values correct?
14. Which modules modify vs create each property?
15. Missing any properties for full SAGE physics?
16. How to document property lifecycle clearly?

### Testing Strategy
17. What cross-module validation tests are needed?
18. How to test mass/metal/energy conservation across modules?
19. When to compare against SAGE reference?
20. What statistical tests for SAGE comparison?

---

## Timeline (To Be Determined)

**Depends on**:
- Architectural decision complexity
- Merger triggering implementation time
- Number of integration issues discovered
- Validation test requirements

**Rough Estimates** (subject to change):
- Architecture decisions: 1 week
- Merger triggering implementation: 2-4 weeks (if complex)
- Module-by-module integration: 1-2 weeks
- Full pipeline validation: 1-2 weeks
- SAGE comparison: 2-3 weeks

**Total**: 7-12 weeks (highly uncertain)

---

## Success Criteria (Phase 4.3 Complete)

Phase 4.3 is complete when:

- [ ] All architectural decisions documented
- [ ] Merger triggering implemented and tested
- [ ] All 6 SAGE modules integrated into pipeline
- [ ] Module execution order validated
- [ ] Property flow verified across all modules
- [ ] Mass/metal conservation verified
- [ ] Performance acceptable on Millennium data
- [ ] Full pipeline tests passing
- [ ] Ready for SAGE scientific comparison (Phase 4.4)

---

## Notes

- This document is forward-looking and should be updated as decisions are made
- Completed work moves to roadmap_v4.md
- Open questions should be resolved through discussion
- Task list will become more concrete once strategy is decided

**Philosophy**: Take time to make good architectural decisions. Rushing into integration without resolving these questions will lead to rework.

---

---

## Recent Progress (November 18, 2025)

### Code Quality & Testing Improvements

**safe_div() utility integration**:
- Merged remote main branch bringing safe_div() utility function
- Applied safe_div() to all 4 new modules:
  - sage_starformation_feedback: 4 divisions updated
  - sage_reincorporation: 2 divisions updated
  - sage_mergers: 7 divisions updated, removed duplicate safe_div()
  - sage_disk_instability: 5 divisions updated
- Total: 18 division operations now use centralized safe division

**Compiler warnings addressed**:
- Fixed 2 unused parameter warnings (ctx) with proper (void)ctx; idiom
- Documented 3 expected warnings in sage_mergers (functions blocked by Phase 4.3)
- Build now clean except for documented expected warnings

**Unit test framework updates**:
- Rewrote test_unit_sage_disk_instability.c to match current API
- Fixed 20 compilation errors from outdated testing patterns
- All 14 unit tests now passing (all 8 modules)
- Test coverage: 6 infrastructure + 8 module tests = 14 total

**Impact**: All merged modules now meet professional code quality standards. Ready for integration testing phase.

---

**Last Updated**: 2025-11-18 (after code quality improvements)
**Next Review**: After architectural decisions are made
