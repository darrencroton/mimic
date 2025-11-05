# Mimic Architectural Transformation Roadmap

**Document Status**: Strategic Planning
**Created**: 2025-11-05
**Updated**: 2025-11-05 (clarified scope)
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

**All available modules compiled into executable** + **Runtime selection via parameter file**
- Developer writes new cooling module → recompilation needed (add new code)
- User switches between cooling models → NO recompilation (just edit parameter file)
- `EnabledModules cooling,star_formation` in .par file selects which modules execute
- Memory allocated for enabled modules only

This is simpler than full dynamic loading while providing scientific flexibility.

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
  const char *name;              // "cooling", "star_formation", etc.
  const char *version;           // "1.0.0"

  // Lifecycle
  int (*init)(void);             // Called at startup
  int (*cleanup)(void);          // Called at shutdown

  // Execution - THE KEY ABSTRACTION
  int (*process_galaxy)(int halonr, struct Halo *halos, int ngal);

  // Dependencies
  const char **requires;         // NULL-terminated list
  int enabled;                   // Runtime flag from config
};
```

### Registration Pattern

**Auto-registration** (compile-time):
```c
// In src/modules/cooling/cooling_module.c
static struct Module cooling_module = {
  .name = "cooling",
  .version = "1.0.0",
  .init = cooling_init,
  .cleanup = cooling_cleanup,
  .process_galaxy = cooling_process,
  .requires = (const char*[]){"star_formation", NULL}  // Example dependency
};

// Auto-register when compiled in
__attribute__((constructor))
static void register_cooling(void) {
  register_module(&cooling_module);
}
```

All modules in `src/modules/` auto-register this way. Core discovers them automatically.

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
  // Execute each enabled module in dependency order
  for (int i = 0; i < num_modules; i++) {
    if (!modules[i]->enabled) continue;

    int result = modules[i]->process_galaxy(halonr, halos, ngal);
    if (result != 0) return result;  // Propagate errors
  }
  return 0;
}
```

### Design Decisions

**1. Auto-registration vs Explicit Table**
- **Choice**: Auto-registration with `__attribute__((constructor))`
- **Rationale**: Modules self-register, no manual list to maintain
- **Trade-off**: Registration order undefined (use dependency resolution)

**2. Single-phase vs Multi-phase**
- **Choice**: Single-phase initially (one execution point per halo)
- **Rationale**: Simpler, sufficient for most physics
- **Extension**: Can add phases later if needed (pre/post evolution, etc.)

**3. Dependency Resolution**
- **Choice**: Simple topological sort
- **Rationale**: Ensures correct execution order (e.g., cooling before star formation)
- **Implementation**: Standard graph algorithm, ~100 lines

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

## Layer 3: Runtime Module Selection

### The Goal

**Select which galaxy physics modules to run via parameter file.**

**Current**: N/A (no modules yet)
**After**: `EnabledModules cooling,star_formation` in .par file

### Configuration Format

**Extend existing .par format** (minimal user disruption):

```
%------------------------------------------
%----- GALAXY PHYSICS MODULES -------------
%------------------------------------------

EnabledModules    cooling,star_formation,feedback

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

### Module Selection Flow

```
1. Startup: All compiled modules auto-register
   → cooling, star_formation, feedback, mergers all register

2. Read parameter file
   → EnabledModules = cooling,star_formation

3. Enable configured modules, disable others
   → cooling.enabled = true
   → star_formation.enabled = true
   → feedback.enabled = false (compiled but not run)

4. Resolve dependencies
   → star_formation requires cooling
   → Execution order: cooling → star_formation

5. Initialize enabled modules
   → cooling.init()
   → star_formation.init()

6. Main loop: Execute enabled modules only
   → cooling.process_galaxy()
   → star_formation.process_galaxy()

7. Cleanup: Shutdown enabled modules
   → star_formation.cleanup()
   → cooling.cleanup()
```

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

**1. Compile-time vs Runtime Loading**
- **Choice**: All modules compiled, runtime enable/disable
- **Rationale**: Simpler than dlopen(), sufficient for science use case
- **Trade-off**: Slightly larger executable (acceptable)

**2. .par vs JSON vs YAML**
- **Choice**: Extend .par format initially
- **Rationale**: Minimal change, users understand it
- **Extension**: Can migrate to JSON when complexity increases

**3. Automatic vs Manual Dependencies**
- **Choice**: Automatic dependency resolution
- **Rationale**: User just lists what they want, system figures out order
- **Example**: Request "star_formation" → auto-enables "cooling"

---

## Implementation Phases

### Phase 1: Module System (2-3 weeks)

**Goal**: Infrastructure for galaxy physics modules (none exist yet, just the system)

**Deliverables**:
- Module registry and registration
- Module lifecycle management (init/cleanup)
- Module execution pipeline
- Dependency resolution

**Changes**:
- Move `src/modules/halo_properties/` → `src/core/halo_properties/`
- Create `src/core/module_registry.c/h`
- Create `src/core/module_pipeline.c/h`
- Add pipeline execution to `build_model.c`
- Add module init/cleanup to `main.c`

**Validation**:
- Core executes with zero modules → bit-identical output to current
- Module system ready for first galaxy module

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

### Phase 3: Runtime Selection (1-2 weeks)

**Goal**: Enable/disable modules via parameter file

**Deliverables**:
- Extended .par format with `EnabledModules`
- Module enable/disable logic
- Dependency resolution
- Conditional memory allocation

**Changes**:
- Extend parameter parser for `EnabledModules`
- Add dependency resolution to module registry
- Update init to respect enabled flags
- Update execution pipeline to skip disabled modules

**Validation**:
- Enabling all modules → bit-identical output
- Disabling all modules → bit-identical to Phase 1
- Invalid module name → clear error message
- Dependency auto-resolution works

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
- [ ] Module pipeline executes in dependency order
- [ ] Output identical with zero modules enabled
- [ ] Infrastructure ready for galaxy modules

### After Phase 2 (Property System)
- [ ] All 24 halo properties defined in YAML
- [ ] C code auto-generated from YAML
- [ ] Output system uses property metadata
- [ ] Adding property requires only YAML edit

### After Phase 3 (Runtime Selection)
- [ ] Modules enabled/disabled via .par file
- [ ] Dependency resolution works automatically
- [ ] Clear error messages for invalid config
- [ ] Memory allocated only for enabled modules

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
| **Module compilation** | All compiled, runtime selection | Simpler than dynamic loading, scientifically flexible |
| **Module registration** | Auto-registration | Self-contained modules, no manual lists |
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

## Conclusion

This transformation **prepares Mimic for galaxy physics** while preserving excellent halo infrastructure:

**What Stays**: Halo tracking, tree processing, memory management, I/O (all solid)
**What's Added**: Module system, property metadata, runtime configuration
**What It Enables**: Rapid galaxy physics development without core changes

**Timeline**: 6-9 weeks for complete infrastructure + proof of concept
**Risk**: LOW (additive changes, bit-identical validation, phased approach)
**Benefit**: Galaxy modules in hours (not days), properties in minutes (not hours)

**Recommendation**: Proceed with Phase 1 (Module System). The infrastructure is well-designed, minimal, and focused on enabling scientific productivity.
