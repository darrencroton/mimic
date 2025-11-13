# Generated Files Audit and Reorganization Report

**Date**: 2025-11-13
**Author**: Automated Audit
**Status**: Recommendations for Review

---

## Executive Summary

This report audits all generated files in the Mimic codebase and provides recommendations to consolidate them into appropriate `generated/` subdirectories. The primary findings:

1. **`src/generated/` is unused** and should be archived
2. Most generated files are already well-organized in **`src/include/generated/`**
3. Some generated files need relocation to new `generated/` subdirectories
4. Documentation references need updating for consistency

**Total Generated Files**: 15 files across 7 generation sources

---

## Table of Contents

1. [Current State: All Generated Files](#current-state-all-generated-files)
2. [Generation Scripts](#generation-scripts)
3. [Metadata Sources](#metadata-sources)
4. [Recommendations](#recommendations)
5. [Implementation Plan](#implementation-plan)
6. [Documentation Updates Required](#documentation-updates-required)

---

## Current State: All Generated Files

### 1. Property Generation (8 files)

**Source Metadata:**
- `metadata/properties/halo_properties.yaml` (24 properties)
- `metadata/properties/galaxy_properties.yaml` (11 properties)

**Generator:**
- `scripts/generate_properties.py` (729 lines)

**Generated Files:**

| File | Current Location | Status | Purpose |
|------|-----------------|--------|---------|
| `property_defs.h` | `src/include/generated/` | ✅ Good | C struct definitions (Halo, GalaxyData, HaloOutput) |
| `init_halo_properties.inc` | `src/include/generated/` | ✅ Good | Halo property initialization code |
| `init_galaxy_properties.inc` | `src/include/generated/` | ✅ Good | Galaxy property initialization code |
| `copy_to_output.inc` | `src/include/generated/` | ✅ Good | Output copy logic for binary writer |
| `hdf5_field_count.inc` | `src/include/generated/` | ✅ Good | HDF5 field count macro |
| `hdf5_field_definitions.inc` | `src/include/generated/` | ✅ Good | HDF5 field definitions |
| `generated_dtype.py` | `output/mimic-plot/` | ⚠️ Move | NumPy dtype definitions for plotting |
| `property_ranges.json` | `tests/generated/` | ✅ Good | Property validation ranges |

**Triggered By:**
- `make` (automatic when YAML changes via dependency in Makefile lines 104-110)
- `make generate` (manual)

**Included By:**
- `src/include/types.h:5` → includes `property_defs.h`
- `src/core/halo_properties/virial.c:44,54` → includes init files
- `src/io/output/binary.c:205` → includes `copy_to_output.inc`
- `src/io/output/hdf5.c:70,80` → includes HDF5 files

---

### 2. Module Generation (3 files)

**Source Metadata:**
- `src/modules/sage_infall/module_info.yaml`
- `src/modules/simple_sfr/module_info.yaml`
- `src/modules/test_fixture/module_info.yaml`

**Generator:**
- `scripts/generate_module_registry.py` (613 lines)

**Generated Files:**

| File | Current Location | Status | Purpose |
|------|-----------------|--------|---------|
| `module_init.c` | `src/modules/` | ⚠️ Move | Module registration and initialization code |
| `module_sources.mk` | `tests/unit/` | ⚠️ Move | Module source paths for unit test builds |
| `module-reference.md` | `docs/user/` | ⚠️ Move | User documentation for all modules |

**Triggered By:**
- `make` (automatic when module_info.yaml changes via Makefile lines 128-134)
- `make generate` (manual)
- `make generate-modules` (manual)

**Git Status:**
- ✅ All three files in `.gitignore` (lines 19, 20, 21)

---

### 3. Build System Generation (1 file)

**Generator:**
- Makefile lines 62-70 (inline shell commands)

**Generated Files:**

| File | Current Location | Status | Purpose |
|------|-----------------|--------|---------|
| `git_version.h` | `src/include/` | ⚠️ Move | Git commit/branch/date tracking |

**Triggered By:**
- `make` (automatic when `.git/HEAD` or `.git/index` changes)

**Git Status:**
- ✅ In `.gitignore` (line 18)

**Included By:**
- Required by all object files via Makefile line 77

---

### 4. Test Registry Generation (3 files)

**Source Metadata:**
- Same as module generation (module_info.yaml files)

**Generator:**
- `scripts/generate_test_registry.py` (source file exists)

**Generated Files:**

| File | Current Location | Status | Purpose |
|------|-----------------|--------|---------|
| `unit_tests.txt` | `build/generated_test_lists/` | ❌ Created | List of module unit test paths |
| `integration_tests.txt` | `build/generated_test_lists/` | ❌ Created | List of module integration test paths |
| `scientific_tests.txt` | `build/generated_test_lists/` | ❌ Created | List of module scientific test paths |

**Triggered By:**
- `make generate-test-registry` (manual)
- `make test-integration` (automatic before tests, line 237)
- `make test-scientific` (automatic before tests, line 262)

**Git Status:**
- ✅ Entire `build/` directory in `.gitignore` (line 13)

**Used By:**
- Makefile lines 248-252 (integration tests loop)
- Makefile lines 269-273 (scientific tests loop)

**Current Status:**
- Directory does not exist (created on first test run)

---

### 5. Unused Directory

**File:**
- `src/generated/README.md`

**Content:**
```markdown
# Generated Code Directory

**Status:** Not used.

**Note:** Generated code is located in `src/include/generated/` directory.
```

**Recommendation:**
- ❌ **Archive to `ignore/archive/src/generated/`**

---

## Generation Scripts

All generation scripts in `scripts/` directory:

| Script | Lines | Purpose | Output Files |
|--------|-------|---------|--------------|
| `generate_properties.py` | 729 | Property code generation | 8 files (C headers, includes, Python dtype, test metadata) |
| `generate_module_registry.py` | 613 | Module registration | 3 files (C code, test config, docs) |
| `check_generated.py` | 252 | Verify generated files are current | None (validation only) |
| `generate_test_registry.py` | ~200 | Auto-discover module tests | 3 files (test lists) |

**Total**: 1,594 lines of generation code

---

## Metadata Sources

### Property Metadata (YAML)

| File | Properties | Purpose |
|------|-----------|---------|
| `metadata/properties/halo_properties.yaml` | 24 | Core halo tracking properties (physics-agnostic) |
| `metadata/properties/galaxy_properties.yaml` | 11 | Galaxy properties (module-specific) |

**Note:** These source files remain untouched (per user requirement).

### Module Metadata (YAML)

| File | Module | Type |
|------|--------|------|
| `src/modules/sage_infall/module_info.yaml` | sage_infall | Physics module |
| `src/modules/simple_sfr/module_info.yaml` | simple_sfr | Physics module |
| `src/modules/test_fixture/module_info.yaml` | test_fixture | Infrastructure/testing module |

---

## Recommendations

### Priority 1: Archive Unused Directory

**Action:** Archive `src/generated/` directory

**Reason:**
- Explicitly marked as "not used" in its own README
- No code references `src/generated` anywhere in codebase (verified via grep)
- Creates confusion about where generated files belong

**Implementation:**
```bash
mkdir -p ignore/archive/src
mv src/generated ignore/archive/src/
```

---

### Priority 2: Create `generated/` Subdirectories

Create consistent `generated/` subdirectories for better organization:

#### 2.1 Module Generated Files → `src/modules/generated/`

**Current State:**
- `module_init.c` lives in `src/modules/` (mixed with source code)

**Recommendation:**
- Create `src/modules/generated/`
- Move `module_init.c` to `src/modules/generated/module_init.c`

**Benefits:**
- Separates generated code from module source code
- Clear signal that file is auto-generated
- Consistent with `src/include/generated/` pattern

**Changes Required:**
- Update `scripts/generate_module_registry.py` line 54:
  ```python
  # OLD: MODULE_INIT_C = REPO_ROOT / 'src' / 'modules' / 'module_init.c'
  # NEW:
  MODULE_INIT_C = REPO_ROOT / 'src' / 'modules' / 'generated' / 'module_init.c'
  ```
- Update Makefile line 120:
  ```makefile
  # OLD: MODULE_INIT_C := $(SRC_DIR)/modules/module_init.c
  # NEW:
  MODULE_INIT_C := $(SRC_DIR)/modules/generated/module_init.c
  ```
- Update `.gitignore` line 19:
  ```
  # OLD: src/modules/module_init.c
  # NEW:
  src/modules/generated/module_init.c
  ```

#### 2.2 Build Generated Files → `build/generated/`

**Current State:**
- `git_version.h` lives in `src/include/` (mixed with source headers)
- Test lists live in `build/generated_test_lists/` (inconsistent naming)

**Recommendation:**
- Move `git_version.h` to `build/generated/git_version.h`
- Rename `build/generated_test_lists/` to `build/generated/`

**Benefits:**
- Build-time artifacts separate from source code
- All build artifacts under `build/`
- Entire `build/` directory already gitignored

**Changes Required:**

**For git_version.h:**
- Update Makefile line 55:
  ```makefile
  # OLD: GIT_VERSION_H = $(SRC_DIR)/include/git_version.h
  # NEW:
  GIT_VERSION_H = $(BUILD_DIR)/generated/git_version.h
  ```
- Update Makefile line 18 (add include path):
  ```makefile
  CFLAGS += -I$(BUILD_DIR)/generated
  ```
- Update `.gitignore` (remove line 18 - no longer needed, covered by `build/`)

**For test lists:**
- Update `scripts/generate_test_registry.py` line 37:
  ```python
  # OLD: output_dir = repo_root / "build" / "generated_test_lists"
  # NEW:
  output_dir = repo_root / "build" / "generated"
  ```
- Update Makefile lines 248, 269:
  ```makefile
  # OLD: build/generated_test_lists/integration_tests.txt
  # NEW: build/generated/integration_tests.txt
  ```

#### 2.3 Test Generated Files → Keep `tests/generated/`

**Current State:**
- `tests/generated/property_ranges.json` ✅

**Recommendation:**
- ✅ Keep as-is (already well-organized)
- Add `module_sources.mk` here as well

**Changes Required:**
- Update `scripts/generate_module_registry.py` line 55:
  ```python
  # OLD: MODULE_SOURCES_MK = REPO_ROOT / 'tests' / 'unit' / 'module_sources.mk'
  # NEW:
  MODULE_SOURCES_MK = REPO_ROOT / 'tests' / 'generated' / 'module_sources.mk'
  ```
- Update `.gitignore` line 20:
  ```
  # OLD: tests/unit/module_sources.mk
  # NEW:
  tests/generated/module_sources.mk
  ```
- Update `tests/unit/` Makefile or build scripts that include this file

#### 2.4 Documentation Generated Files → `docs/generated/`

**Current State:**
- `docs/user/module-reference.md` (generated) mixed with hand-written docs

**Recommendation:**
- Create `docs/generated/`
- Move to `docs/generated/module-reference.md`
- Add symlink or reference from `docs/user/` if needed

**Changes Required:**
- Update `scripts/generate_module_registry.py` line 56:
  ```python
  # OLD: MODULE_REFERENCE_MD = REPO_ROOT / 'docs' / 'user' / 'module-reference.md'
  # NEW:
  MODULE_REFERENCE_MD = REPO_ROOT / 'docs' / 'generated' / 'module-reference.md'
  ```
- Update `.gitignore` line 21:
  ```
  # OLD: docs/user/module-reference.md
  # NEW:
  docs/generated/module-reference.md
  ```
- Update any links in `docs/README.md` or other docs

#### 2.5 Plotting Generated Files → `output/mimic-plot/generated/`

**Current State:**
- `output/mimic-plot/generated_dtype.py` (single file in root of plotting directory)

**Recommendation:**
- Create `output/mimic-plot/generated/`
- Move to `output/mimic-plot/generated/dtype.py`

**Benefits:**
- Future-proof for additional generated plotting code
- Clear separation from plotting source code
- Consistent naming pattern

**Changes Required:**
- Update `scripts/generate_properties.py` line 109:
  ```python
  # OLD: PLOT_DIR = REPO_ROOT / 'output' / 'mimic-plot'
  # NEW:
  PLOT_GENERATED_DIR = REPO_ROOT / 'output' / 'mimic-plot' / 'generated'
  ```
- Update output path in generation function (find and update in script)
- Update import statements in `output/mimic-plot/mimic-plot.py`:
  ```python
  # OLD: from generated_dtype import get_halo_dtype
  # NEW:
  from generated.dtype import get_halo_dtype
  ```

---

## Implementation Plan

### Phase 1: Archive Unused Directory ✅

**Files Changed:** 0
**Risk:** None
**Time:** 1 minute

```bash
mkdir -p ignore/archive/src
mv src/generated ignore/archive/src/
```

### Phase 2: Module Generated Code 🔄

**Files Changed:** 3 (script, Makefile, .gitignore)
**Risk:** Low (only one generated file)
**Time:** 5 minutes

1. Update `scripts/generate_module_registry.py`
2. Update Makefile
3. Update `.gitignore`
4. Run `make generate-modules` to create file in new location
5. Verify build works

### Phase 3: Build Generated Code 🔄

**Files Changed:** 4 (Makefile, .gitignore, test script)
**Risk:** Medium (affects all compilation)
**Time:** 10 minutes

1. Update Makefile (git_version.h path and include dir)
2. Update `.gitignore`
3. Update `scripts/generate_test_registry.py`
4. Run `make clean && make` to test
5. Run `make tests` to verify test discovery

### Phase 4: Test and Documentation Generated Code 🔄

**Files Changed:** 4 (script, .gitignore, test includes)
**Risk:** Low
**Time:** 10 minutes

1. Update `scripts/generate_module_registry.py` (both output paths)
2. Update `.gitignore`
3. Find and update any `#include` statements for `module_sources.mk`
4. Update documentation links
5. Test with `make generate && make tests`

### Phase 5: Plotting Generated Code 🔄

**Files Changed:** 3 (script, plotting code)
**Risk:** Low
**Time:** 10 minutes

1. Update `scripts/generate_properties.py`
2. Update imports in `output/mimic-plot/mimic-plot.py`
3. Run `make generate`
4. Test plotting system

### Phase 6: Documentation Updates 📝

**Files Changed:** ~10 (various docs)
**Risk:** None
**Time:** 20 minutes

See [Documentation Updates Required](#documentation-updates-required) section.

---

## Documentation Updates Required

### Files Needing Updates

#### 1. `docs/README.md`
- **Line 376**: Update directory structure to show `generated/` subdirs
- **Lines 175-180**: Update generated file paths in help text

#### 2. `metadata/README.md`
- **Lines 18-24**: Update all generated file paths
- **Line 24**: Update Python dtype path

#### 3. `docs/architecture/property-metadata-schema.md`
- **Line 55**: Update path (currently incorrect: `properties.h` vs actual `property_defs.h`)
- **Line 56**: Update path (references non-existent `property_accessors.h`)

#### 4. `docs/developer/module-developer-guide.md`
- **Line 443**: Update reference to galaxy properties header location

#### 5. `docs/developer/testing.md`
- Add section documenting `tests/generated/` and `build/generated/` structure
- Update test discovery documentation with new paths

#### 6. `Makefile` help text
- **Lines 176-185**: Update all generated file paths in help output

#### 7. `CLAUDE.md`
- Update directory structure section
- Update build commands section with new paths

#### 8. `tests/README.md`
- Update test registry paths

### New Documentation Files Needed

#### `src/modules/generated/README.md`
```markdown
# Module Generated Code

**Auto-generated by:** `scripts/generate_module_registry.py`

**Files:**
- `module_init.c` - Module registration and initialization

**Triggered by:** Module metadata changes (`*/module_info.yaml`)

**DO NOT EDIT** - Changes will be overwritten. Edit module_info.yaml instead.
```

#### `build/generated/README.md`
```markdown
# Build-Time Generated Files

**Auto-generated during build process**

**Files:**
- `git_version.h` - Git commit, branch, and build date
- `integration_tests.txt` - Module integration test paths
- `scientific_tests.txt` - Module scientific test paths
- `unit_tests.txt` - Module unit test paths

**DO NOT EDIT** - Regenerated on every build.
```

#### `docs/generated/README.md`
```markdown
# Generated Documentation

**Auto-generated from metadata**

**Files:**
- `module-reference.md` - Module configuration reference (from module_info.yaml)

**DO NOT EDIT** - Changes will be overwritten. Edit source metadata instead.
```

#### `output/mimic-plot/generated/README.md`
```markdown
# Plotting Generated Code

**Auto-generated by:** `scripts/generate_properties.py`

**Files:**
- `dtype.py` - NumPy dtype definitions for reading Mimic binary output

**Triggered by:** Property metadata changes (metadata/properties/*.yaml)

**DO NOT EDIT** - Changes will be overwritten. Edit property YAML instead.
```

---

## Summary of Changes

### Directory Structure (After Implementation)

```
mimic/
├── src/
│   ├── include/
│   │   └── generated/          ✅ KEEP - Property C headers (6 files)
│   └── modules/
│       └── generated/          ✨ NEW - Module registration (1 file)
├── build/
│   └── generated/              ✨ NEW - Build-time files (4 files)
├── tests/
│   └── generated/              ✅ EXPAND - Test metadata (2 files)
├── docs/
│   └── generated/              ✨ NEW - Generated docs (1 file)
├── output/
│   └── mimic-plot/
│       └── generated/          ✨ NEW - Plotting dtypes (1 file)
└── ignore/
    └── archive/
        └── src/
            └── generated/      📦 ARCHIVED - Unused directory
```

### Files by New Location

| Directory | Files | Purpose |
|-----------|-------|---------|
| `src/include/generated/` | 6 | Property C code (struct defs, init, output, HDF5) |
| `src/modules/generated/` | 1 | Module registration C code |
| `build/generated/` | 4 | Build artifacts (git version, test lists) |
| `tests/generated/` | 2 | Test metadata (property ranges, module sources) |
| `docs/generated/` | 1 | Generated documentation (module reference) |
| `output/mimic-plot/generated/` | 1 | Plotting support (NumPy dtypes) |

**Total:** 15 generated files across 6 directories

---

## Questions for Review

Before implementation, please confirm:

1. ✅ **Archive `src/generated/`?** - Marked as unused, no references found
2. ✅ **Move `module_init.c` to `src/modules/generated/`?** - Better separation
3. ✅ **Move `git_version.h` to `build/generated/`?** - Build artifact, not source
4. ✅ **Consolidate test lists under `build/generated/`?** - Simpler structure
5. ✅ **Move `module_sources.mk` to `tests/generated/`?** - Consistent with test metadata
6. ✅ **Move `module-reference.md` to `docs/generated/`?** - Clear it's generated
7. ✅ **Create `output/mimic-plot/generated/`?** - Future-proof for more generated plotting code

---

## Risk Assessment

| Change | Risk Level | Mitigation |
|--------|-----------|------------|
| Archive `src/generated/` | **None** | No code references, just README |
| Move module_init.c | **Low** | Single file, clear dependency in Makefile |
| Move git_version.h | **Medium** | All files include it, test thoroughly |
| Move test registry | **Low** | Only used by Makefile test targets |
| Move module_sources.mk | **Low** | Only included by test build system |
| Move module-reference.md | **Low** | Documentation only, update links |
| Move plotting dtype | **Low** | Single import to update |

**Overall Risk:** Low-Medium
**Recommended Approach:** Implement in phases, test after each phase

---

## Conclusion

The current generated file organization is mostly good (property files in `src/include/generated/`), but several files are scattered in inappropriate locations:

1. `src/generated/` - Unused, should be archived
2. `module_init.c` - Mixed with source, should be in `generated/` subdir
3. `git_version.h` - Build artifact in source tree, should be under `build/`
4. Generated docs and test configs - Should be in clear `generated/` subdirs

Implementing these recommendations will create a **consistent, clear structure** where:
- ✅ All generated files live in `generated/` subdirectories
- ✅ Generated files are clearly separated from hand-written code
- ✅ Each category has its own namespace (src, build, tests, docs, output)
- ✅ Future generated code has obvious homes

**Next Step:** Review recommendations and approve implementation plan phases.
