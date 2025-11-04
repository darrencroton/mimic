# Mimic Structural Reorganization Roadmap

**Version:** 1.0
**Date:** 2025-11-04
**Purpose:** Reorganize codebase structure to align with architectural vision

---

## Executive Summary

This document provides a practical plan for reorganizing the Mimic codebase from a flat structure to a hierarchical one that supports the architectural vision described in `docs/mimic-architecture-vision.md`.

**What This Does:**
- Creates hierarchical directory structure (`src/`, `tests/`, `metadata/`, `tools/`, etc.)
- Moves existing files from `code/` to appropriate subdirectories
- Renames files to drop redundant prefixes
- Updates build system for new structure
- Creates placeholders for future development work

**What This Does NOT Do:**
- Implement new features (metadata system, module registry, etc.)
- Change algorithms, data structures, or core functionality
- Write actual tests or module implementations

**Success Criterion:**
After completion, `make clean && make && ./sage input/millennium.par` produces identical results.

---

## Current vs. Target Structure

### Current (Flat)

```
mimic/
├── code/                    [82 files in one directory]
│   ├── main.c
│   ├── core_*.c/h          [5 source files]
│   ├── io_*.c/h            [9 source files]
│   ├── util_*.c/h          [7 source files]
│   ├── model_misc.c/h      [1 source file]
│   ├── types.h, globals.h, constants.h, config.h
│   └── *.o, *.d            [build artifacts]
├── docs/                    [2 files]
├── input/, output/, archive/
├── first_run.sh, beautify.sh
└── Makefile
```

### Target (Hierarchical)

```
mimic/
├── src/                     [Renamed from code/]
│   ├── core/               [Core execution: main.c, init.c, build_model.c, etc.]
│   ├── io/
│   │   ├── tree/          [Tree readers: interface.c, binary.c, hdf5.c]
│   │   └── output/        [Output writers: binary.c, hdf5.c, util.c]
│   ├── util/               [Utilities: memory.c, error.c, version.c, etc.]
│   ├── modules/
│   │   └── halo_properties/  [virial.c, module.c (stub)]
│   ├── generated/          [Future: code from metadata]
│   └── include/            [Public headers: types.h, globals.h, etc.]
├── metadata/               [Future: YAML schemas]
├── tools/                  [Future: code generators]
├── tests/                  [Future: test framework]
│   ├── unit/, integration/, data/, framework/
├── build/                  [Build artifacts: obj/, deps/]
├── docs/
│   ├── architecture/, developer/, user/, api/
├── scripts/                [first_run.sh, beautify.sh]
├── examples/               [Future: usage examples]
├── input/, output/, archive/
└── Makefile
```

---

## Implementation Plan

### Phase 1: Create Directory Structure

```bash
# Source directories
mkdir -p src/core
mkdir -p src/io/tree src/io/output
mkdir -p src/util
mkdir -p src/modules/halo_properties
mkdir -p src/generated
mkdir -p src/include

# Future work placeholders
mkdir -p metadata
mkdir -p tools
mkdir -p tests/unit tests/integration tests/data tests/framework
mkdir -p examples

# Build and documentation
mkdir -p build/obj build/deps
mkdir -p docs/architecture docs/developer docs/user docs/api
mkdir -p scripts
```

**Checkpoint:** Verify directories exist: `tree -L 2 -d src/`

---

### Phase 2: Create Minimal Placeholder READMEs

Create brief READMEs only for directories that are placeholders for future work:

#### metadata/README.md

```markdown
# Metadata Directory

**Purpose:** YAML definitions for properties, parameters, and modules.

**Status:** Placeholder for future metadata-driven development.

**Future contents:**
- `properties.yaml` - Halo/galaxy property definitions
- `parameters.yaml` - Configuration parameters
- `modules.yaml` - Module registry

See architectural vision for details on metadata system design.
```

#### tools/README.md

```markdown
# Tools Directory

**Purpose:** Code generation scripts and build utilities.

**Status:** Placeholder for future development.

**Future contents:**
- `generate_properties.py` - Generate structs/accessors from metadata
- `generate_parameters.py` - Generate config from metadata
- `validate_metadata.py` - YAML validation

See architectural vision for details on code generation system.
```

#### src/generated/README.md

```markdown
# Generated Code Directory

**Purpose:** Auto-generated C code from YAML metadata.

**Status:** Placeholder. DO NOT manually edit files here.

**Future contents:**
- `properties.{h,c}` - Generated from metadata/properties.yaml
- `parameters.{h,c}` - Generated from metadata/parameters.yaml

Files here will be regenerated during build from metadata sources.
```

#### tests/README.md

```markdown
# Test Infrastructure

**Purpose:** Unit tests, integration tests, and test data.

**Status:** Placeholder for future test implementation.

**Structure:**
- `unit/` - Unit tests for individual functions
- `integration/` - End-to-end workflow tests
- `data/` - Test input data and reference outputs
- `framework/` - Test utilities and helpers

Test framework selection and implementation to be decided in future sprint.
```

#### examples/README.md

```markdown
# Examples Directory

**Purpose:** Usage examples and tutorials.

**Status:** Placeholder for future examples.

Examples will be added as features are implemented and stabilized.
```

#### src/modules/README.md

```markdown
# Physics Modules

**Purpose:** Runtime-loadable physics modules.

**Current:** halo_properties/ (virial calculations, moved from model_misc.c)

**Future:** Module system will be designed in future sprint. See architectural vision for details.
```

**No READMEs needed for:** `src/core/`, `src/io/`, `src/util/`, `src/include/` - names are self-explanatory.

---

### Phase 3: File Migration

#### Step 3.1: File Mapping

| Current | New Location | Notes |
|---------|--------------|-------|
| **Core Files** |
| `code/main.c` | `src/core/main.c` | |
| `code/core_init.c` | `src/core/init.c` | Drop "core_" prefix |
| `code/core_build_model.c` | `src/core/build_model.c` | Drop "core_" prefix |
| `code/core_read_parameter_file.c` | `src/core/read_parameter_file.c` | Drop "core_" prefix |
| `code/core_allvars.c` | `src/core/allvars.c` | Drop "core_" prefix |
| (+ corresponding .h files) | `src/core/*.h` | Same naming |
| **I/O Files** |
| `code/io_tree.c` | `src/io/tree/interface.c` | More descriptive |
| `code/io_tree_binary.c` | `src/io/tree/binary.c` | Drop "io_tree_" prefix |
| `code/io_tree_hdf5.c` | `src/io/tree/hdf5.c` | Drop "io_tree_" prefix |
| `code/io_save_binary.c` | `src/io/output/binary.c` | Drop "io_save_" prefix |
| `code/io_save_hdf5.c` | `src/io/output/hdf5.c` | Drop "io_save_" prefix |
| `code/io_save_util.c` | `src/io/output/util.c` | Drop "io_save_" prefix |
| `code/io_util.c` | `src/io/util.c` | Drop "io_" prefix |
| (+ corresponding .h files) | Same pattern | |
| **Utility Files** |
| `code/util_*.c/h` | `src/util/*.c/h` | Drop "util_" prefix |
| **Module Files** |
| `code/model_misc.c` | `src/modules/halo_properties/virial.c` | More specific name |
| `code/model_misc.h` | `src/modules/halo_properties/virial.h` | |
| **Public Headers** |
| `code/types.h` | `src/include/types.h` | |
| `code/globals.h` | `src/include/globals.h` | |
| `code/constants.h` | `src/include/constants.h` | |
| `code/config.h` | `src/include/config.h` | |
| `code/core_proto.h` | `src/include/proto.h` | Drop "core_" prefix |
| `code/core_allvars.h` | `src/include/allvars.h` | Drop "core_" prefix |
| **Documentation** |
| `code/doc_standards.md` | `docs/developer/coding-standards.md` | |

#### Step 3.2: Execute File Moves

Use `git mv` to preserve history:

```bash
# Core files
git mv code/main.c src/core/main.c
git mv code/core_init.c src/core/init.c
git mv code/core_init.h src/core/init.h
git mv code/core_build_model.c src/core/build_model.c
git mv code/core_build_model.h src/core/build_model.h
git mv code/core_read_parameter_file.c src/core/read_parameter_file.c
git mv code/core_read_parameter_file.h src/core/read_parameter_file.h
git mv code/core_allvars.c src/core/allvars.c

# I/O tree files
git mv code/io_tree.c src/io/tree/interface.c
git mv code/io_tree.h src/io/tree/interface.h
git mv code/io_tree_binary.c src/io/tree/binary.c
git mv code/io_tree_binary.h src/io/tree/binary.h
git mv code/io_tree_hdf5.c src/io/tree/hdf5.c
git mv code/io_tree_hdf5.h src/io/tree/hdf5.h

# I/O output files
git mv code/io_save_binary.c src/io/output/binary.c
git mv code/io_save_binary.h src/io/output/binary.h
git mv code/io_save_hdf5.c src/io/output/hdf5.c
git mv code/io_save_hdf5.h src/io/output/hdf5.h
git mv code/io_save_util.c src/io/output/util.c
git mv code/io_save_util.h src/io/output/util.h

# I/O util
git mv code/io_util.c src/io/util.c
git mv code/io_util.h src/io/util.h

# Utilities
git mv code/util_memory.c src/util/memory.c
git mv code/util_memory.h src/util/memory.h
git mv code/util_error.c src/util/error.c
git mv code/util_error.h src/util/error.h
git mv code/util_version.c src/util/version.c
git mv code/util_version.h src/util/version.h
git mv code/util_parameters.c src/util/parameters.c
git mv code/util_parameters.h src/util/parameters.h
git mv code/util_numeric.c src/util/numeric.c
git mv code/util_numeric.h src/util/numeric.h
git mv code/util_integration.c src/util/integration.c
git mv code/util_integration.h src/util/integration.h

# Module
git mv code/model_misc.c src/modules/halo_properties/virial.c
git mv code/model_misc.h src/modules/halo_properties/virial.h

# Public headers
git mv code/types.h src/include/types.h
git mv code/globals.h src/include/globals.h
git mv code/constants.h src/include/constants.h
git mv code/config.h src/include/config.h
git mv code/core_proto.h src/include/proto.h
git mv code/core_allvars.h src/include/allvars.h

# Documentation
git mv code/doc_standards.md docs/developer/coding-standards.md
```

**Checkpoint:** After each section, verify:
```bash
git status  # Should show renames, not deletions/additions
ls -la src/core/  # Verify files present
```

#### Step 3.3: Update Include Paths

After file moves, update `#include` statements throughout codebase.

**Automated replacements:**

```bash
# Update include paths (macOS sed syntax)
find src/ -type f \( -name "*.c" -o -name "*.h" \) -exec sed -i '' \
    -e 's|"core_init\.h"|"init.h"|g' \
    -e 's|"core_build_model\.h"|"build_model.h"|g' \
    -e 's|"core_read_parameter_file\.h"|"read_parameter_file.h"|g' \
    -e 's|"io_tree\.h"|"tree/interface.h"|g' \
    -e 's|"io_tree_binary\.h"|"tree/binary.h"|g' \
    -e 's|"io_tree_hdf5\.h"|"tree/hdf5.h"|g' \
    -e 's|"io_save_binary\.h"|"output/binary.h"|g' \
    -e 's|"io_save_hdf5\.h"|"output/hdf5.h"|g' \
    -e 's|"io_save_util\.h"|"output/util.h"|g' \
    -e 's|"io_util\.h"|"util.h"|g' \
    -e 's|"util_memory\.h"|"memory.h"|g' \
    -e 's|"util_error\.h"|"error.h"|g' \
    -e 's|"util_version\.h"|"version.h"|g' \
    -e 's|"util_parameters\.h"|"parameters.h"|g' \
    -e 's|"util_numeric\.h"|"numeric.h"|g' \
    -e 's|"util_integration\.h"|"integration.h"|g' \
    -e 's|"model_misc\.h"|"virial.h"|g' \
    -e 's|"core_proto\.h"|"proto.h"|g' \
    {} \;
```

**Manual review needed:** Some cross-module includes may need path adjustments. The Makefile's `-I` flags will handle most cases.

**Checkpoint:**
```bash
make clean
make  # Should compile successfully
```

---

### Phase 4: Update Build System

#### Step 4.1: New Makefile

Replace Makefile with reorganized version:

```makefile
# Mimic Makefile
EXEC = sage

# Directories
SRC_DIR = src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
DEP_DIR = $(BUILD_DIR)/deps

# Source files (recursive find)
SOURCES := $(shell find $(SRC_DIR) -name '*.c')
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))
DEPS := $(patsubst $(SRC_DIR)/%.c,$(DEP_DIR)/%.d,$(SOURCES))

# Compiler
CC ?= cc
CFLAGS = -g -O2 -Wall -Wextra
CFLAGS += -I$(SRC_DIR)/include
CFLAGS += -I$(SRC_DIR)/core
CFLAGS += -I$(SRC_DIR)/io
CFLAGS += -I$(SRC_DIR)/util
CFLAGS += -I$(SRC_DIR)/modules

# Dependency generation
CFLAGS += -MMD -MP

# Libraries
LDFLAGS =
LIBS = -lm

# Optional: HDF5 support
ifdef USE-HDF5
    CFLAGS += -DHDF5
    HDF5_PREFIX ?= /opt/homebrew
    CFLAGS += -I$(HDF5_PREFIX)/include
    LDFLAGS += -L$(HDF5_PREFIX)/lib
    LIBS += -lhdf5_hl -lhdf5
endif

# Optional: MPI support
ifdef USE-MPI
    CC = mpicc
    CFLAGS += -DMPI
endif

# Git version tracking
GIT_VERSION_H = $(SRC_DIR)/include/git_version.h

$(GIT_VERSION_H): .git/HEAD .git/index
	@echo "Generating git version..."
	@echo "#ifndef GIT_VERSION_H" > $@
	@echo "#define GIT_VERSION_H" >> $@
	@echo "#define GIT_COMMIT \"$$(git rev-parse HEAD 2>/dev/null || echo 'unknown')\"" >> $@
	@echo "#define GIT_BRANCH \"$$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')\"" >> $@
	@echo "#define GIT_DATE \"$$(git log -1 --format=%cd --date=short 2>/dev/null || echo 'unknown')\"" >> $@
	@echo "#define BUILD_DATE \"$$(date '+%Y-%m-%d')\"" >> $@
	@echo "#endif" >> $@

# Build targets
.PHONY: all clean tidy help

all: $(EXEC)

$(EXEC): $(OBJECTS)
	@echo "Linking $@..."
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(GIT_VERSION_H)
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*.d)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -MF $(DEP_DIR)/$*.d -c $< -o $@

-include $(DEPS)

clean:
	@echo "Cleaning..."
	rm -rf $(BUILD_DIR) $(EXEC) $(GIT_VERSION_H)
	@echo "Clean complete"

tidy:
	@echo "Removing build artifacts..."
	rm -rf $(BUILD_DIR)

help:
	@echo "Mimic Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build executable"
	@echo "  make clean        - Remove all build artifacts"
	@echo "  make tidy         - Remove build directory only"
	@echo ""
	@echo "Options:"
	@echo "  make USE-HDF5=yes - Enable HDF5 support"
	@echo "  make USE-MPI=yes  - Enable MPI support"
```

#### Step 4.2: Update .gitignore

```bash
cat >> .gitignore << 'EOF'

# Build artifacts
build/
*.o
*.d

# Executables
sage

# IDE files
.vscode/
.idea/
*.swp
*.swo
*~

# Python
__pycache__/
*.pyc

# macOS
.DS_Store
EOF
```

**Checkpoint:**
```bash
make clean
make
./sage input/millennium.par
# Verify output identical to baseline
```

---

### Phase 5: Minor Code Refactoring

#### Fix Misplaced Functions

**Issue:** `copy_file()` function is in `main.c` but is a utility function.

**Solution:** Create `src/util/io.{c,h}` and move it there.

**src/util/io.h:**
```c
#ifndef UTIL_IO_H
#define UTIL_IO_H

int copy_file(const char *source, const char *dest);

#endif
```

**src/util/io.c:**
```c
#include "io.h"
#include "error.h"
// Move copy_file() implementation from main.c here
```

**Update src/core/main.c:**
- Remove `copy_file()` function
- Add `#include "../util/io.h"`

#### Create Module Stub

**src/modules/halo_properties/module.h:**
```c
#ifndef MODULE_HALO_PROPERTIES_H
#define MODULE_HALO_PROPERTIES_H

// Stub for future module system
int halo_properties_init(void);
int halo_properties_cleanup(void);

#endif
```

**src/modules/halo_properties/module.c:**
```c
#include "module.h"
#include "../../util/error.h"

// Stubs for future module system implementation
int halo_properties_init(void) {
    INFO_LOG("Halo properties module initialized (stub)");
    return 0;
}

int halo_properties_cleanup(void) {
    return 0;
}
```

**Note:** These stubs are NOT called yet - they're placeholders for future module system.

**Checkpoint:**
```bash
make clean
make
./sage input/millennium.par
diff -r output/sage/ baseline_output/
```

---

### Phase 6: Documentation Organization

#### Move Documents

```bash
# Move architecture docs
git mv docs/mimic-architecture-vision.md docs/architecture/vision.md

# Update root README.md
```

**Update root README.md to add:**
```markdown
## Documentation

- **Architecture**: See [docs/architecture/](docs/architecture/)
- **Developer Guide**: See [docs/developer/](docs/developer/)
- **User Guide**: See [docs/user/](docs/user/)
```

**Create docs/developer/getting-started.md:**
```markdown
# Getting Started

## Building
```bash
make clean && make
```

## Running
```bash
./sage input/millennium.par
```

See [coding-standards.md](coding-standards.md) for code style requirements.
```

---

### Phase 7: Scripts Migration

```bash
# Move scripts
git mv first_run.sh scripts/first_run.sh
git mv beautify.sh scripts/beautify.sh

# Update script paths if they reference code/
sed -i '' 's|code/|src/|g' scripts/first_run.sh
sed -i '' 's|code/|src/|g' scripts/beautify.sh
```

**Checkpoint:** Verify scripts still work:
```bash
./scripts/beautify.sh
./scripts/first_run.sh
```

---

## Naming Conventions

### Directories
- Use lowercase: `src/`, `tests/`, `docs/`
- Use underscores for multi-word: `halo_properties/`

### Files
- Single word: `init.c`, `main.c`
- Multi-word: `build_model.c`, `read_parameter_file.c`
- Headers match source: `init.h` for `init.c`
- **Drop redundant prefixes** when in subdirectories:
  - `src/core/init.c` NOT `src/core/core_init.c`
  - `src/io/util.c` NOT `src/io/io_util.c`

### Functions
- Action: `verb_object()` - `load_tree()`, `init_memory()`
- Getters: `get_property()` - `get_virial_mass()`
- Setters: `set_property()` - `set_halo_position()`
- Boolean: `is_condition()`, `has_condition()`

### Structs & Constants
- Structs: `struct PascalCase` - `struct Halo`, `struct SageConfig`
- Enums: `enum PascalCase` - `enum Valid_TreeTypes`
- Constants/Macros: `UPPER_CASE` - `MAX_PATH_LENGTH`, `DEBUG_LOG()`

---

## Testing Checkpoints

### After Each Phase

```bash
# Always run this after changes:
make clean
make
./sage input/millennium.par

# Compare to baseline (saved before reorganization):
diff -r output/sage/ baseline_output/

# Check for memory leaks:
valgrind --leak-check=full ./sage input/millennium.par
```

### Verify Directory Structure

```bash
# Check key directories exist:
test -d src/core && echo "✓ src/core"
test -d src/io/tree && echo "✓ src/io/tree"
test -d src/modules/halo_properties && echo "✓ src/modules/halo_properties"
test -d metadata && echo "✓ metadata"
test -d tests && echo "✓ tests"
test -d build && echo "✓ build"

# Verify code/ is empty:
test ! -f code/*.c && echo "✓ code/ clean"
```

### Test Build Variations

```bash
# Standard build
make clean && make

# With HDF5
make clean && make USE-HDF5=yes

# With MPI (if available)
make clean && make USE-MPI=yes
```

---

## Common Issues

### Include path errors
**Symptom:** `error: 'core_init.h' file not found`
**Fix:** Check file was moved correctly, update `#include` to new name

### Git not tracking renames
**Symptom:** Git shows deletions + additions instead of renames
**Fix:** Use `git mv` explicitly, not manual move

### Build artifacts in wrong place
**Symptom:** `.o` files still in `code/`
**Fix:** `rm -rf code/*.o code/*.d build/` then `make clean && make`

---

## Final Checklist

### Before Starting
- [ ] Save baseline: `cp -r output/sage/ baseline_output/`
- [ ] Git working tree clean
- [ ] Current build works

### Directory Structure
- [ ] All directories created
- [ ] Placeholder READMEs in: metadata/, tools/, src/generated/, tests/, examples/, src/modules/
- [ ] .gitignore updated

### File Migration
- [ ] Core files moved to src/core/
- [ ] I/O files moved to src/io/{tree,output}/
- [ ] Util files moved to src/util/
- [ ] Module files moved to src/modules/halo_properties/
- [ ] Headers moved to src/include/
- [ ] All moves used `git mv`
- [ ] Include paths updated

### Build System
- [ ] Makefile updated for src/ structure
- [ ] Build artifacts go to build/
- [ ] Clean target works
- [ ] git_version.h generates correctly

### Code Changes
- [ ] copy_file() moved to src/util/io.c
- [ ] Module stubs created (but not called)
- [ ] No broken references

### Documentation
- [ ] Architecture docs in docs/architecture/
- [ ] Developer docs in docs/developer/
- [ ] coding-standards.md moved from code/
- [ ] Root README.md updated

### Scripts
- [ ] Scripts moved to scripts/
- [ ] Script paths updated
- [ ] Scripts still work

### Testing
- [ ] Build works: `make clean && make`
- [ ] Execution works: `./sage input/millennium.par`
- [ ] Output identical to baseline
- [ ] No memory leaks
- [ ] Git history preserved: `git log --follow src/core/init.c`

---

## Success Criteria

**Functional:**
- Build completes without errors
- Sage runs and produces identical output
- All build variations work (standard, HDF5, MPI)
- Git history preserved for all moved files

**Structural:**
- Hierarchical organization: `src/{core,io,util,modules,include}/`
- Placeholder directories ready: `metadata/`, `tools/`, `tests/`, `examples/`
- Build artifacts isolated in `build/`
- Naming conventions followed
- Documentation organized

**Readiness:**
- Clear homes established for future work
- Naming patterns established
- No anticipation of future implementation details

---

## Conclusion

This reorganization creates a clean, hierarchical structure that supports the architectural vision without implementing complex features. The codebase maintains identical functionality while providing proper organizational homes for future development work.

**Next steps** (future sprints):
- Design metadata system
- Design module system
- Implement test framework
- Implement features from architectural vision

Each future task will have its proper place in this reorganized structure.

---

*For questions, refer to:*
- `docs/architecture/vision.md` - Architectural principles
- `docs/developer/coding-standards.md` - Code style guide
