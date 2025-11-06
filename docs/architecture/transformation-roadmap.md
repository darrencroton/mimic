# Mimic Architectural Transformation Roadmap

**Document Status**: Strategic Planning
**Created**: 2025-11-05
**Updated**: 2025-11-05 (simplified to user-controlled module ordering)
**Purpose**: Transform Mimic to support modular galaxy physics with metadata-driven properties

---

## Executive Summary

Mimic currently has **excellent halo-tracking infrastructure** (~21k LOC) with solid memory management, tree processing, and I/O. The transformation goal is to **prepare Mimic to accept galaxy physics modules** (cooling, star formation, feedback, etc.) without disrupting the working halo infrastructure.

### Scope Clarification

**What is CORE (not modular)**:
- All halo physics: virial calculations, halo tracking, FoF processing
- Current code in `src/modules/halo_properties/` → moving to `src/core/`
- Memory management, tree processing, I/O systems
- This code stays as-is (it's solid infrastructure)

**What is MODULAR (the transformation target)**:
- Future galaxy/baryonic physics: cooling, star formation, feedback, mergers, etc.
- These don't exist yet - we're building the infrastructure to support them
- Property system for galaxy properties: StellarMass, ColdGas, SFR, etc.

### Compilation Model

**Three-level module control** for optimal flexibility and performance:

**1. Write New Module** (C code development)
- Add new galaxy physics module source code
- Requires: Writing C code in `src/modules/`
- Triggers: Recompilation

**2. Select Modules to Compile** (Build-time)
- Choose which modules to compile into executable
- Method: `make MODULES="cooling_SD93 star_formation_KS98"` or predefined profiles
- Triggers: Recompilation
- Result: Smaller executable with only needed code

**3. Enable/Disable Compiled Modules** (Runtime)
- Choose which compiled modules to execute
- Method: `EnabledModules cooling_SD93,star_formation_KS98` in .par file
- Triggers: NO recompilation (just edit parameter file)
- Result: Memory allocated only for enabled modules

**Example workflow**:
```bash
# Build with specific module set (compile-time selection)
make MODULES="cooling_SD93 cooling_G12 star_formation_KS98"

# Run with subset (runtime selection)
EnabledModules cooling_SD93,star_formation_KS98  # Fast cooling
EnabledModules cooling_G12,star_formation_KS98   # Slow cooling (no recompile!)

# Add new module version (requires recompile)
make MODULES="cooling_SD93 cooling_G12 cooling_new star_formation_KS98"
```

**Module Profiles** (predefined sets for common models):
```bash
make PROFILE=croton2006    # Croton+2006 modules
make PROFILE=henriques2015 # Henriques+2015 modules
make PROFILE=all           # All available modules (exploration mode)
```

This approach balances performance (small executables) with flexibility (runtime switching).

---

## Current State Assessment

### Strengths (Preserve These)

| Component | Quality | Action |
|-----------|---------|--------|
| Three-tier data model (RawHalo → Halo → HaloOutput) | ✅ Excellent | Keep pattern |
| Memory management (scoped allocation, leak detection) | ✅ Excellent | Unchanged |
| Tree processing (recursive traversal, inheritance) | ✅ Solid | Core stays intact |
| I/O abstraction (binary & HDF5) | ✅ Good | Extend for new properties |
| Error handling & logging | ✅ Professional | Unchanged |

**Assessment**: Infrastructure is production-quality. Transform by adding, not replacing.

### Critical Gaps

| Gap | Consequence | Priority |
|-----|-------------|----------|
| **No module system** | Can't add galaxy physics without editing core | 🔴 CRITICAL |
| **Hardcoded properties** | Every property touches 5+ files (types.h, output, HDF5, etc.) | 🔴 CRITICAL |
| **No property abstraction** | Direct struct access everywhere, hard to extend | 🟡 HIGH |
| **Manual synchronization** | Halo ↔ HaloOutput kept in sync by hand | 🟡 HIGH |

---

## Pre-Transformation Optimization (COMPLETED)

**Status**: ✅ Complete (15 hours, November 2025)
**Goal**: Strengthen foundation before transformation phases begin

### Completed Improvements

| Optimization | Impact | Benefit for Transformation |
|-------------|--------|----------------------------|
| **Memory Categorization** | All 15 allocations categorized (MEM_TREES, MEM_HALOS, MEM_IO, MEM_UTILITY) | Essential for debugging Phase 4 galaxy modules - clear subsystem breakdown |
| **Single Source of Truth** | ~60 references updated to use MimicConfig.* directly | Establishes Principle 4 foundation - eliminates 50+ lines of sync code |
| **Dead Code Removal** | 12 unused functions removed (~250 lines) | Reduces maintenance burden before transformation |
| **Function Simplification** | prepare_halo_for_output() reduced from 83 to ~60 lines | Better understanding of output requirements for Phase 2 generator |
| **Const Correctness** | Added const to 3 key functions (prepare_halo_for_output, is_parameter_valid, read_parameter_file) | Improves type safety foundation for generated accessors |
| **Data Structure Lifecycles** | Comprehensive documentation of InputTreeHalos → FoFWorkspace → ProcessedHalos flow | Critical for Phase 1-2 as galaxy properties are added |

### Total Impact

- **~410 lines** removed/simplified
- **Zero functional changes** - all optimizations are purely architectural
- **Builds verified** with both `make` and `make USE-HDF5=yes`
- **Single source of truth** established for all configuration parameters
- **Memory tracking** ready for Phase 4 galaxy module allocations

### Key Architectural Improvements

1. **SYNC_CONFIG Macros** (Temporary scaffolding for transition):
   - Formalizes synchronization pattern between MimicConfig and legacy globals
   - Makes dependencies explicit and searchable
   - Will be removed in Phase 3 (post-transformation)

2. **Three-Tier Data Flow Documentation** (See globals.h):
   ```
   InputTreeHalos (RawHalo*) → IMMUTABLE INPUT from merger trees
   FoFWorkspace (Halo*)      → TEMPORARY processing workspace
   ProcessedHalos (Halo*)    → PERMANENT storage for current tree
   HaloAux (HaloAuxData*)    → PROCESSING METADATA
   ```
   - Allocation pattern documented in load_tree()
   - Deallocation pattern documented in free_halos_and_tree()
   - Critical for understanding where galaxy properties will be stored

3. **Vision Principle Alignment**:
   - ✅ Principle 4 (Single Source of Truth): Config accessed via MimicConfig.* throughout
   - ✅ Principle 6 (Memory Efficiency): Categorized tracking enables precise leak detection
   - ✅ Principle 8 (Type Safety): Const correctness improved for read-only parameters

---

## Vision Principles - Current Compliance

```
Current → Target

Principle 1: Physics-Agnostic Core          ⚠️ → ✅  (Halo core is fine, need galaxy module system)
Principle 2: Runtime Modularity             ❌ → ✅  (Need module selection via config)
Principle 3: Metadata-Driven Architecture   ❌ → ✅  (Need property metadata system)
Principle 4: Single Source of Truth         ❌ → ✅  (Need unified property representation)
Principle 5: Unified Processing Model       ✅ → ✅  (Already good, maintain)
Principle 6: Memory Efficiency              ✅ → ✅  (Already excellent, maintain)
Principle 7: Format-Agnostic I/O            ✅ → ✅  (Already good, extend)
Principle 8: Type Safety and Validation     ⚠️ → ✅  (Need generated accessors)
```

**Transformation Focus**: Principles 1-4 (module system + property system). Principles 5-7 are already satisfied.

---

## Transformation Architecture

The transformation adds **three new layers** on top of existing infrastructure:

```
┌─────────────────────────────────────────────────────────┐
│         LAYER 3: Runtime Module Selection               │
│  Parameter file selects which compiled modules to run   │
│  EnabledModules = cooling,star_formation                 │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│         LAYER 2: Metadata-Driven Properties             │
│  Properties defined in YAML, C code auto-generated      │
│  Single source of truth for halo + galaxy properties    │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│         LAYER 1: Galaxy Physics Module System           │
│  Registration, lifecycle, execution pipeline            │
│  Modules in src/modules/ auto-register at compile time  │
└─────────────────────────────────────────────────────────┘
                          ↓
┌─────────────────────────────────────────────────────────┐
│         EXISTING: Halo Physics Core (Unchanged)         │
│  Tree processing, memory, I/O, halo virial physics      │
│  Current code - solid infrastructure we preserve        │
└─────────────────────────────────────────────────────────┘
```

**Key Design Decision**: Purely additive transformation. Existing halo code continues unchanged.

---

## Layer 1: Galaxy Physics Module System

### The Goal

**Enable adding galaxy physics without touching core code.**

**Current**: No galaxy physics (cooling, SF, feedback) - can't add them cleanly
**After**: Module system ready to accept galaxy physics modules

### What Changes

**New Files** (create these):
- `src/core/module_registry.c/h` - Module registration and lifecycle
- `src/core/module_pipeline.c/h` - Module execution coordination
- `src/include/module_interface.h` - Standard module interface

**Modified Files**:
- `src/core/build_model.c` - Add module pipeline execution point
- `src/core/main.c` - Initialize/cleanup module system

**Moved Files**:
- `src/modules/halo_properties/` → `src/core/halo_properties/` (it's core, not a module)

### Module Interface (Minimal)

```c
struct Module {
  const char *name;              // "cooling_SD93", "star_formation_KS98", etc.
  const char *version;           // "1.0.0"

  // Lifecycle
  int (*init)(void);             // Called at startup
  int (*cleanup)(void);          // Called at shutdown

  // Execution - THE KEY ABSTRACTION
  int (*process_galaxy)(int halonr, struct Halo *halos, int ngal);

  // Runtime control
  int enabled;                   // Runtime flag from config
};
```

**Key simplification**: No dependency tracking. User controls execution order via parameter file.

### Registration Pattern

**Auto-registration** (compile-time):
```c
// In src/modules/cooling_SD93/cooling_SD93.c
static struct Module cooling_SD93_module = {
  .name = "cooling_SD93",
  .version = "1.0.0",
  .init = cooling_SD93_init,
  .cleanup = cooling_SD93_cleanup,
  .process_galaxy = cooling_SD93_process,
  .enabled = 0  // Disabled by default, enabled from config
};

// Auto-register when compiled in
__attribute__((constructor))
static void register_cooling_SD93(void) {
  register_module(&cooling_SD93_module);
}
```

All modules in `src/modules/` auto-register this way. Core discovers them automatically.

**Order-independent registration**: Modules register in any order at compile-time. Execution order determined by user at runtime.

### Execution Integration

**Core execution loop** (src/core/build_model.c):

```c
void process_halo_evolution(int halonr, int ngal) {
  // EXISTING: Core halo processing (unchanged)
  // ... join_progenitors, halo tracking, etc. ...

  // NEW: Execute galaxy physics modules (if any enabled)
  execute_module_pipeline(halonr, FoFWorkspace, ngal);

  // EXISTING: Core bookkeeping (unchanged)
  update_halo_properties(ngal);
}
```

**Module pipeline** (new):
```c
int execute_module_pipeline(int halonr, struct Halo *halos, int ngal) {
  // Execute each enabled module in user-specified order
  // Order determined by EnabledModules in parameter file
  for (int i = 0; i < num_enabled_modules; i++) {
    struct Module *module = enabled_modules[i];

    DEBUG_LOG("Executing module: %s", module->name);
    int result = module->process_galaxy(halonr, halos, ngal);
    if (result != 0) {
      ERROR_LOG("Module %s failed at position %d", module->name, i);
      return result;
    }
  }
  return 0;
}
```

**Order matters**: Execution order = order in parameter file. User controls physics sequence.

### Design Decisions

**1. Auto-registration vs Explicit Table**
- **Choice**: Auto-registration with `__attribute__((constructor))`
- **Rationale**: Modules self-register, no manual list to maintain
- **Trade-off**: Registration order undefined (but execution order from config)

**2. User-Controlled Ordering vs Automatic Dependency Resolution**
- **Choice**: User specifies execution order in parameter file
- **Rationale**:
  - **Simpler**: No dependency graph, topological sort, or cycle detection (~100+ lines saved)
  - **User control**: Scientists know their physics dependencies
  - **Explicit**: Order visible in config file, not hidden in code
  - **Flexible**: Users can experiment with different orderings
  - **Clearer errors**: "Module X failed at position Y" vs dependency errors
- **Trade-off**: User must understand physics ordering (but they should anyway!)
- **Implementation**: Parse parameter file, enable modules in specified order

**3. Single-phase vs Multi-phase**
- **Choice**: Single-phase initially (one execution point per halo)
- **Rationale**: Simpler, sufficient for most physics
- **Extension**: Can add phases later if needed (pre/post evolution, etc.)

---

## Layer 2: Metadata-Driven Properties

### The Goal

**Define all properties (halo + galaxy) once in metadata, generate C code automatically.**

**Current**: 24 halo properties hardcoded in 5+ files (types.h, output/binary.c, output/hdf5.c, etc.)
**After**: All properties defined in YAML, C structs/accessors auto-generated

### Property Metadata

**File**: `metadata/properties/properties.yaml`

```yaml
# Mimic Property Metadata - Single Source of Truth

halo_properties:
  - name: SnapNum
    type: int
    units: "-"
    description: "Snapshot number"
    output: true

  - name: Type
    type: int
    units: "-"
    description: "Halo type (0=central, 1=satellite, 2=orphan)"
    output: true

  - name: Mvir
    type: float
    units: "1e10 Msun/h"
    description: "Virial mass"
    output: true

  - name: Pos
    type: vec3_float
    units: "Mpc/h"
    description: "Position vector"
    output: true

  # ... all 24 halo properties ...

galaxy_properties:
  - name: StellarMass
    type: float
    units: "1e10 Msun/h"
    description: "Total stellar mass"
    output: true
    created_by: star_formation

  - name: ColdGas
    type: float
    units: "1e10 Msun/h"
    description: "Cold gas mass"
    output: true
    created_by: cooling

  # ... galaxy properties added as modules are developed ...
```

### Code Generation

**Build-time generation**:
```bash
# In Makefile
generate:
    python3 scripts/generate_properties.py

# Generates:
src/include/generated/property_defs.h      # struct definitions
src/include/generated/property_accessors.h # GET/SET macros
src/include/generated/property_metadata.c  # metadata table
```

**Generated struct** (property_defs.h):
```c
// Generated from metadata/properties/properties.yaml
// DO NOT EDIT - regenerate with: make generate

struct Halo {
  // Halo properties
  int SnapNum;
  int Type;
  float Mvir;
  float Pos[3];
  // ... all properties ...

  // Galaxy properties (initially empty)
  float StellarMass;
  float ColdGas;
  // ... added as modules define them ...
};
```

**Generated accessors** (property_accessors.h):
```c
// Zero-overhead macros for type-safe access
#define GET_HALO_SNAPNUM(h)       ((h).SnapNum)
#define SET_HALO_SNAPNUM(h, val)  ((h).SnapNum = (val))

#define GET_HALO_MVIR(h)          ((h).Mvir)
#define SET_HALO_MVIR(h, val)     ((h).Mvir = (val))

// ... one per property ...
```

**Generated metadata** (property_metadata.c):
```c
// Property metadata table for output system
const PropertyMetadata property_table[] = {
  {"SnapNum", PROP_INT, offsetof(struct Halo, SnapNum),
   sizeof(int), "-", "Snapshot number", true},
  {"Mvir", PROP_FLOAT, offsetof(struct Halo, Mvir),
   sizeof(float), "1e10 Msun/h", "Virial mass", true},
  // ... all properties ...
  {NULL, 0, 0, 0, NULL, NULL, false}  // sentinel
};
```

### Property-Driven Output

**Before** (manual, 150+ lines):
```c
void calc_hdf5_props(void) {
  HDF5_dst_offsets[0] = HOFFSET(struct HaloOutput, SnapNum);
  HDF5_field_names[0] = "SnapNum";
  HDF5_field_types[0] = H5T_NATIVE_INT;
  // ... repeated 24 times by hand ...
}
```

**After** (automatic, ~10 lines):
```c
void calc_hdf5_props(void) {
  const PropertyMetadata *props = get_property_table();

  for (int i = 0; props[i].name != NULL; i++) {
    if (!props[i].output) continue;

    HDF5_dst_offsets[i] = props[i].offset;
    HDF5_field_names[i] = props[i].name;
    HDF5_field_types[i] = convert_type(props[i].type);
  }
}
```

### Design Decisions

**1. YAML vs JSON vs C DSL**
- **Choice**: YAML for metadata
- **Rationale**: Human-readable, supports comments, standard for config
- **Trade-off**: Requires PyYAML (already have Python for plotting)

**2. Macros vs Inline Functions**
- **Choice**: Macros initially (zero overhead)
- **Rationale**: Critical for HPC performance (10M+ property accesses)
- **Extension**: Can add inline functions later if needed

**3. Unified vs Separate**
- **Choice**: Unified system for halo + galaxy properties
- **Rationale**: Single source of truth, consistent output handling
- **Benefit**: Adding galaxy property = edit YAML only

**4. Generated Files in Git**
- **Choice**: Commit generated files to repository
- **Rationale**: Builds work without Python, diffs show changes
- **Trade-off**: Extra files in repo (acceptable)

---

## Layer 3: Build-Time + Runtime Module Selection

### The Goal

**Two-level module control for performance and flexibility.**

**Current**: N/A (no modules yet)
**After**:
- Build-time: Select modules to compile (`make MODULES="..."`)
- Runtime: Enable/disable compiled modules (`EnabledModules ...` in .par file)

### Build-Time Module Selection

**Makefile approach** (simple, flexible):

```makefile
# Default module set
MODULES ?= cooling_SD93 star_formation_KS98 feedback_simple

# User can override:
# make MODULES="cooling_G12 star_formation_KMT09 AGN_bondi"

# Compile only specified modules
MODULE_SRCS = $(foreach mod,$(MODULES),src/modules/$(mod)/$(mod).c)
```

**Predefined profiles** for common models:

```makefile
# In Makefile
PROFILE ?= default

ifeq ($(PROFILE),croton2006)
    MODULES = cooling_SD93 star_formation_KS98 feedback_croton06 mergers_guo11
else ifeq ($(PROFILE),henriques2015)
    MODULES = cooling_G12 star_formation_KMT09 AGN_bondi feedback_H15
else ifeq ($(PROFILE),all)
    MODULES = $(wildcard src/modules/*/*)  # All available
else
    MODULES = cooling_SD93 star_formation_KS98  # Default
endif
```

**User workflow**:
```bash
# Use predefined profile
make PROFILE=croton2006
./mimic millennium.par

# Custom module selection
make MODULES="cooling_SD93 cooling_G12 star_formation_KS98"
./mimic millennium.par

# Exploration mode (compile everything)
make PROFILE=all
./mimic millennium.par
```

**Benefits**:
- **Small executables**: Only needed code compiled (~2-5 MB vs 10-20 MB with all modules)
- **Fast loading**: Important on HPC network filesystems
- **Clear dependencies**: Makefile shows exactly what's compiled
- **Reproducible**: Can document exact module set used for published results

**Performance impact**:
- Compiled modules have zero runtime overhead when disabled
- Each disabled module: one `if (!enabled) continue;` check (~1 CPU cycle)
- With 5-10 compiled modules: <0.01% overhead

### Runtime Module Configuration

**Extend existing .par format** (minimal user disruption):

```
%------------------------------------------
%----- GALAXY PHYSICS MODULES -------------
%------------------------------------------

# Order matters! Execution order = order listed below
# Typical physics sequence: cooling → star formation → feedback
EnabledModules    cooling_SD93,star_formation_KS98,feedback_simple

# Module-specific parameters
Cooling_Model           sutherland_dopita_1993
StarFormation_Efficiency  0.02
Feedback_Epsilon         0.01
```

**Why .par extension (not JSON/YAML)?**
- Users already understand .par format
- Leverages existing parameter parsing
- Minimal disruption
- Can migrate to JSON later if needed for complex module config

**Critical**: Order in `EnabledModules` determines execution order. User controls physics sequence.

### Module Selection Flow

```
1. Startup: All compiled modules auto-register
   → cooling_SD93, cooling_G12, star_formation_KS98, feedback_simple all register
   → Registration order doesn't matter

2. Read parameter file
   → EnabledModules = cooling_SD93,star_formation_KS98,feedback_simple
   → Parse module names in order

3. Build execution pipeline in user-specified order
   → enabled_modules[0] = cooling_SD93
   → enabled_modules[1] = star_formation_KS98
   → enabled_modules[2] = feedback_simple
   → Execution order = order in parameter file

4. Initialize enabled modules (in order)
   → cooling_SD93.init()
   → star_formation_KS98.init()
   → feedback_simple.init()

5. Main loop: Execute enabled modules (in order)
   → cooling_SD93.process_galaxy()
   → star_formation_KS98.process_galaxy()
   → feedback_simple.process_galaxy()

6. Cleanup: Shutdown enabled modules (reverse order)
   → feedback_simple.cleanup()
   → star_formation_KS98.cleanup()
   → cooling_SD93.cleanup()
```

**Key principle**: User specifies order → system executes in that order. No hidden logic.

### Memory Management

**Memory allocated only for enabled modules**:

```c
int module_init_all(void) {
  for (int i = 0; i < num_modules; i++) {
    if (!modules[i]->enabled) continue;

    // Module allocates its own working memory
    if (modules[i]->init() != 0) {
      ERROR_LOG("Module %s initialization failed", modules[i]->name);
      return -1;
    }

    INFO_LOG("Module %s initialized", modules[i]->name);
  }
  return 0;
}
```

Disabled modules: no memory allocated, no execution overhead.

### Design Decisions

**1. Build-Time Selection + Runtime Enable/Disable**
- **Choice**: Two-level control (compile selected modules, runtime enable/disable)
- **Rationale**:
  - Small executables (performance on HPC)
  - Runtime flexibility (no recompile to switch between compiled modules)
  - Simpler than dynamic loading (no dlopen/platform issues)
- **Trade-off**: Must recompile to add new module set (acceptable)

**2. Module Profiles vs Manual Selection**
- **Choice**: Support both (`PROFILE=croton2006` and `MODULES="..."`)
- **Rationale**:
  - Profiles for reproducibility (published models)
  - Manual for flexibility (custom combinations)
  - `PROFILE=all` for exploration
- **Implementation**: Makefile conditionals (~20 lines)

**3. .par vs JSON vs YAML**
- **Choice**: Extend .par format initially
- **Rationale**: Minimal change, users understand it
- **Extension**: Can migrate to JSON when complexity increases

**4. User-Specified Order vs Order-Independent**
- **Choice**: Order in parameter file determines execution order
- **Rationale**: User controls physics, sees order clearly, can experiment
- **Example**: `EnabledModules cooling,star_formation` executes cooling first
- **Documentation**: Explain typical ordering requirements in module docs

---

## Implementation Phases

### Phase 1: Module System (2-3 weeks)

**Goal**: Infrastructure for galaxy physics modules (none exist yet, just the system)

**Deliverables**:
- Module registry and auto-registration
- Module lifecycle management (init/cleanup)
- Module execution pipeline (user-ordered)
- Parameter file parsing for module order

**Changes**:
- Move `src/modules/halo_properties/` → `src/core/halo_properties/`
- Create `src/core/module_registry.c/h` - registration and lookup
- Create `src/core/module_pipeline.c/h` - ordered execution
- Add pipeline execution to `build_model.c`
- Add module init/cleanup to `main.c`
- Extend parameter parser to read `EnabledModules` with ordering

**Validation**:
- Core executes with zero modules → bit-identical output to current
- Module system ready for first galaxy module
- Order preservation: modules execute in config file order

**Code simplification**: ~100 lines saved by removing dependency resolution logic.

### Phase 2: Property System (2-3 weeks)

**Goal**: Unified metadata-driven properties for halo + galaxy

**Deliverables**:
- YAML property definitions (start with 24 halo properties)
- Python code generator
- Generated C structs, accessors, metadata
- Property-driven output system

**Changes**:
- Create `metadata/properties/properties.yaml`
- Create `scripts/generate_properties.py`
- Generate `src/include/generated/*.h`
- Update output system to use property metadata
- Refactor code to use generated accessors

**Validation**:
- Generated code produces bit-identical output
- Adding test property works end-to-end
- Output system automatically includes new properties

### Phase 3: Build-Time + Runtime Selection (1-2 weeks)

**Goal**: Two-level module control (compile selection + runtime enable/disable)

**Deliverables**:
- Makefile build-time module selection (`MODULES` variable)
- Predefined profiles (`PROFILE` variable for common models)
- Extended .par format with `EnabledModules` (order-preserving)
- Module enable/disable logic
- Ordered execution pipeline
- Conditional memory allocation

**Changes**:
- Update Makefile to support `MODULES` and `PROFILE` variables
- Add profile definitions for common models (croton2006, henriques2015, etc.)
- Extend parameter parser to read `EnabledModules` with order preservation
- Build ordered execution array from config
- Update init to respect enabled flags and order
- Update execution pipeline to run modules in specified order

**Validation**:
- Build with specific modules → only those compiled
- Build with PROFILE → correct module set
- Runtime enable/disable → bit-identical when same modules enabled
- Invalid module name → clear error message
- Module order preserved: execution matches config file order

### Phase 4: First Galaxy Module (1-2 weeks)

**Goal**: Proof of concept - implement simple cooling module

**Deliverables**:
- Example galaxy physics module
- Galaxy properties added to metadata
- Documentation for module developers

**Creates**:
- `src/modules/cooling/cooling_module.c`
- Properties: `ColdGas`, `CoolingRate` (in YAML)
- `docs/developer/adding-modules.md`

**Validation**:
- Cooling module compiles and registers
- Can enable/disable via parameter file
- Galaxy properties appear in output
- Scientific validation of cooling prescription

**Total Timeline**: 6-9 weeks for complete transformation + proof of concept

---

## Integration & Validation Strategy

### Critical Requirement

**Bit-identical output after each phase** (until galaxy physics is added):

```bash
# Baseline
./mimic input/millennium.par
md5sum output/results/millennium/model_z0.000_0 > baseline.md5

# After Phase 1 (module system)
make clean && make
./mimic input/millennium.par  # with zero modules
diff <(md5sum output/results/millennium/model_z0.000_0) baseline.md5
# MUST be identical

# After Phase 2 (property system)
make generate && make clean && make
./mimic input/millennium.par
diff <(md5sum output/results/millennium/model_z0.000_0) baseline.md5
# MUST be identical

# After Phase 3 (runtime selection)
./mimic input/millennium.par  # EnabledModules = (empty)
diff <(md5sum output/results/millennium/model_z0.000_0) baseline.md5
# MUST be identical
```

**If output changes**: Bug in transformation, must fix before proceeding.

### Rollback Strategy

Each phase on feature branch:
- `feature/module-system`
- `feature/property-system`
- `feature/runtime-selection`

Merge only after validation passes. Can revert independently.

---

## Success Metrics

### After Phase 1 (Module System)
- [ ] Module registry can register 0, 1, or N modules
- [ ] Module pipeline executes in user-specified order
- [ ] Output identical with zero modules enabled
- [ ] Infrastructure ready for galaxy modules
- [ ] Order preservation: config file order = execution order

### After Phase 2 (Property System)
- [ ] All 24 halo properties defined in YAML
- [ ] C code auto-generated from YAML
- [ ] Output system uses property metadata
- [ ] Adding property requires only YAML edit

### After Phase 3 (Build-Time + Runtime Selection)
- [ ] Build-time module selection via `MODULES` variable works
- [ ] Predefined profiles (`PROFILE=croton2006`) work correctly
- [ ] Runtime enable/disable via .par file works
- [ ] Module execution order matches config file order
- [ ] Clear error messages for invalid config (unknown module, etc.)
- [ ] Memory allocated only for enabled modules
- [ ] Can switch between compiled modules without recompilation
- [ ] Can change module ordering without recompilation

### After Phase 4 (First Galaxy Module)
- [ ] Cooling module implemented and working
- [ ] Galaxy properties in output files
- [ ] Developer documentation complete
- [ ] Scientific validation passed

### Vision Compliance
- Physics-Agnostic Core: ✅ (Halo core unchanged, galaxy modules separate)
- Runtime Modularity: ✅ (Select modules via config, no recompile to switch)
- Metadata-Driven: ✅ (Properties in YAML, code generated)
- Single Source of Truth: ✅ (One property definition in metadata)

---

## Key Design Decisions Summary

| Decision Point | Choice | Rationale |
|----------------|--------|-----------|
| **Halo code** | Keep in core (not modular) | Solid infrastructure, no need to change |
| **Module compilation** | Build-time selection + runtime enable/disable | Small executables (HPC performance), runtime flexibility |
| **Module selection** | Makefile MODULES + predefined PROFILES | Custom combinations + reproducible models |
| **Module registration** | Auto-registration | Self-contained modules, no manual lists |
| **Module execution order** | User-controlled via config file | User knows physics dependencies, simpler code (~100 lines saved) |
| **Property metadata** | YAML + Python generator | Standard, readable, good tooling |
| **Property accessors** | Macros (zero-overhead) | HPC performance critical |
| **Property scope** | Unified halo + galaxy | Single source of truth |
| **Config format** | Extend .par initially | Minimal user disruption |
| **Validation** | Bit-identical output | Proves correctness |

---

## Future Extensions (Post-Transformation)

Once the infrastructure is complete:

### Additional Galaxy Modules
- Star formation (multiple prescriptions)
- Stellar & AGN feedback
- Galaxy mergers and morphology
- Disk instability
- Chemical enrichment
- Dust physics

### Advanced Features
- Property provenance tracking (which module created/modified)
- Property validation (runtime range checking)
- Module parameter exploration (grid search)
- Multi-phase module execution (if needed)
- JSON configuration (when complexity requires)

### Performance
- Module parallelization (OpenMP for independent modules)
- Property lazy evaluation (compute only when needed)
- Output optimization (write only used properties)

---

## Deferred Optimizations (Post-Transformation Priority)

These optimizations were identified during pre-transformation analysis but deferred because:
1. They touch areas that will be significantly changed by the transformation
2. Small effort now would be replaced by larger transformation work
3. Better to complete transformation first, then optimize the new architecture

### Deferred Items

| Optimization | Effort | Priority | When to Address |
|--------------|--------|----------|-----------------|
| **Runtime State Consolidation** | 40+ hours | HIGH | After Phase 4 when module system stable |
| **Complete Global Variable Elimination (Phase 3)** | 1 hour | MEDIUM | After Phase 4 |
| **Naming Convention Standardization** | 4-6 hours | LOW | After transformation complete |
| **System Info Platform Split** | 1 hour | LOW | Pure refactoring, anytime |

### 1. Runtime State Consolidation (HIGH Priority)

**Current State**: Runtime state scattered across 10+ global variables:
```c
extern int Ntrees, FileNum, TreeID, NumProcessedHalos;
extern int MaxProcessedHalos, MaxFoFWorkspace, HaloCounter;
extern int TotHalos, Snaplistlen;
// ... more ...
```

**Target State**: Consolidated MimicRuntimeState struct:
```c
struct MimicRuntimeState {
  // File-level state
  int file_num;
  int num_trees;

  // Tree-level state
  int tree_id;
  int halo_counter;

  // Processing state
  int num_processed_halos;
  int max_processed_halos;
  int max_fof_workspace;

  // Snapshot state
  int snaplistlen;
  int tot_halos;
};
```

**Benefits**:
- Makes function dependencies explicit (pass state as parameter)
- Enables better testing (create isolated state contexts)
- Prepares for potential multi-threading (thread-local state)
- Clarifies what state is per-file vs per-tree vs global

**Why Deferred**: Phase 4 will add significant new state for galaxy modules. Better to consolidate after we understand the full state requirements.

**Estimated Effort**: 40+ hours (touches ~100+ call sites)

### 2. Complete Global Variable Elimination (MEDIUM Priority)

**Current State**: SYNC_CONFIG macros formalize temporary synchronization:
```c
SYNC_CONFIG_INT(MAXSNAPS);  // MAXSNAPS = MimicConfig.MAXSNAPS
```

**Target State**: Remove standalone globals entirely:
- Delete global variable declarations (globals.h)
- Remove all SYNC_CONFIG macro calls
- Remove temporary macros from config.h

**Why Deferred**:
- Phase 1 is complete (all code uses MimicConfig.*)
- Phase 2 deferred until after transformation
- Synchronization code uses explicit macros (easy to find and remove)
- No rush - macros are clear scaffolding that won't cause problems

**Estimated Effort**: 1 hour (straightforward cleanup)

### 3. Naming Convention Standardization (LOW Priority)

**Current State**: Mixed naming conventions:
- Some camelCase: `InputTreeHalos`, `NumProcessedHalos`
- Some underscore: `get_virial_mass`, `load_tree_table`
- Some PascalCase: `RawHalo`, `MimicConfig`

**Target State**: Follow documented standards in coding-standards.md:
- Functions: `snake_case` (get_virial_mass)
- Structs: `PascalCase` (RawHalo)
- Variables: `snake_case` (num_processed_halos)
- Constants/Macros: `UPPER_SNAKE` (TREE_MUL_FAC)

**Why Deferred**:
- Pure refactoring with no functional benefit
- Better to standardize after transformation when code is stable
- Risk of merge conflicts if done during active transformation

**Estimated Effort**: 4-6 hours (systematic search-and-replace)

### 4. System Info Platform Split (LOW Priority)

**Current State**: get_system_info() has platform-specific #ifdef blocks

**Target State**: Split into platform-specific functions:
- `get_system_info_darwin()`
- `get_system_info_linux()`
- etc.

**Why Deferred**: Pure refactoring, no urgency

**Estimated Effort**: 1 hour

---

## Conclusion

This transformation **prepares Mimic for galaxy physics** while preserving excellent halo infrastructure:

**What Stays**: Halo tracking, tree processing, memory management, I/O (all solid)
**What's Added**: Module system, property metadata, build-time + runtime configuration
**What It Enables**: Rapid galaxy physics development without core changes

**Timeline**: 6-9 weeks for complete infrastructure + proof of concept
**Risk**: LOW (additive changes, bit-identical validation, phased approach)
**Benefit**:
- Galaxy modules in hours (not days)
- Properties in minutes (not hours)
- Small executables optimized for HPC (build-time selection)
- Runtime flexibility to switch between compiled modules (no recompilation)

**Recommendation**: Proceed with Phase 1 (Module System). The infrastructure is well-designed, minimal, and focused on enabling scientific productivity.
