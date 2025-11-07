# Mimic Modular Architecture Implementation Roadmap v2

**Document Status**: Implementation Plan (Post-PoC Revision)
**Created**: 2025-11-07
**Version**: 2.0 (Revised after minimal module system PoC)
**Purpose**: Complete implementation guide for transforming Mimic to support modular galaxy physics with metadata-driven properties

---

## Executive Summary

### Overview

Mimic currently has **excellent halo-tracking infrastructure** (~21k LOC) with solid memory management, tree processing, and I/O. This roadmap defines the transformation to **prepare Mimic to accept galaxy physics modules** (cooling, star formation, feedback, etc.) without disrupting the working halo infrastructure.

### Why This Roadmap Exists

**v1 Roadmap** (original): Provided high-level architectural vision and implementation phases based on analysis and planning.

**Minimal Module System PoC** (completed): Implemented one simple physics module (StellarMass = 0.1 * Mvir) with supporting infrastructure to validate concepts and gain hands-on experience.

**v2 Roadmap** (this document): Revised implementation plan incorporating critical insights from the PoC that were not anticipated in planning. **This document is the single source of truth for implementation going forward.**

### Critical PoC Discoveries

The PoC successfully validated core architectural concepts but revealed **critical gaps** requiring revised priorities:

✅ **What Worked:**
- Physics-agnostic separation is feasible and clean
- Module interface pattern (init/process/cleanup) is sound
- Deep copy inheritance works correctly
- Output and plotting integration is straightforward

⚠️ **Critical Issues Discovered:**
- **Memory management doesn't scale**: Required 64x increase in block limit for ONE property
- **Dual-struct synchronization is painful**: 7 files touched per property, unsustainable at scale
- **Module interface too minimal**: Realistic physics needs simulation context and infrastructure access
- **Testing framework is prerequisite**: Cannot validate complex modules without automated testing
- **Many architectural decisions deferred**: Module communication, error handling, parameter access

### Revised Timeline

**Original Estimate**: 6-9 weeks for complete transformation

**Revised Estimate**: 10-16 weeks for solid foundation

**Why Longer:** PoC revealed critical missing pieces:
- Memory management refactor (NEW: Phase 0)
- Module interface enrichment (NEW: Phase 1.5)
- Testing framework (NEW: Phase 5)
- Property metadata system is more critical than anticipated

**Why Worth It:** Solid foundation vs technical debt that compounds rapidly

---

## Vision Principles

All implementation work must align with these 8 core principles from `vision.md`:

### 1. Physics-Agnostic Core Infrastructure
Core systems have zero knowledge of specific physics. Physics modules interact only through well-defined interfaces.

### 2. Runtime Modularity
Physics module combinations configurable at runtime without recompilation.

### 3. Metadata-Driven Architecture
System structure defined by metadata (YAML), not hardcoded implementations.

### 4. Single Source of Truth
Galaxy data has one authoritative representation with consistent access patterns.

### 5. Unified Processing Model
One consistent, well-understood method for processing merger trees.

### 6. Memory Efficiency and Safety
Memory usage bounded, predictable, and safe with automatic leak detection.

### 7. Format-Agnostic I/O
Support multiple input/output formats through unified interfaces.

### 8. Type Safety and Validation
Data access is type-safe with automatic validation.

**Current Compliance Status:**
```
Principle 1: Physics-Agnostic Core          ⚠️ → ✅  (Halo core good, need galaxy module system)
Principle 2: Runtime Modularity             ❌ → ✅  (Need module selection via config)
Principle 3: Metadata-Driven Architecture   ❌ → ✅  (Need property metadata system)
Principle 4: Single Source of Truth         ❌ → ✅  (Need unified property representation)
Principle 5: Unified Processing Model       ✅ → ✅  (Already good, maintain)
Principle 6: Memory Efficiency              ⚠️ → ✅  (Good but needs scalability refactor)
Principle 7: Format-Agnostic I/O            ✅ → ✅  (Already good, extend)
Principle 8: Type Safety and Validation     ⚠️ → ✅  (Need generated accessors)
```

**Transformation Focus**: Principles 1-4 and 6 (module system + property system + memory refactor)

---

## Current State Assessment

### What Works (Preserve These)

| Component | Quality | Action |
|-----------|---------|--------|
| Three-tier data model (RawHalo → Halo → HaloOutput) | ✅ Excellent | Keep pattern, extend for galaxy data |
| Memory management (scoped allocation, leak detection) | ✅ Excellent | Refactor for scalability, keep philosophy |
| Tree processing (recursive traversal, inheritance) | ✅ Solid | Core stays intact, add module hooks |
| I/O abstraction (binary & HDF5) | ✅ Good | Extend for new properties |
| Error handling & logging | ✅ Professional | Unchanged |

### PoC Implementation Status

**Completed Work** (feature/minimal-module-poc branch):
- ✅ Separate GalaxyData structure with physics-agnostic allocation
- ✅ Basic module interface (init/process/cleanup)
- ✅ Simple module registry (manual registration)
- ✅ Module execution pipeline integration
- ✅ Deep copy galaxy data inheritance
- ✅ Binary output extension for StellarMass
- ✅ Stellar mass function plotting (snapshot + evolution)
- ✅ Zero memory leaks, successful execution on Millennium

**Key Files Created/Modified:**
- New: `src/include/galaxy_types.h`, `src/core/module_interface.h`, `src/core/module_registry.[ch]`
- New: `src/modules/stellar_mass/*` (proof-of-concept module)
- New: `output/mimic-plot/figures/stellar_mass_function.py`, `smf_evolution.py`
- Modified: `types.h`, `virial.c`, `build_model.c`, `main.c`, `interface.c`, `binary.c`

**What's Still Missing:**
- ❌ Runtime parameter control (module always runs)
- ❌ Auto-registration (manual call in main.c)
- ❌ Module ordering control
- ❌ Build-time module selection
- ❌ Scalable memory management
- ❌ Property metadata system
- ❌ Enriched module interface
- ❌ Testing framework

---

## Architecture Overview

### Transformation Layers

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

### Key Design Principles

1. **Purely Additive Transformation**: Existing halo code continues unchanged
2. **Physics-Agnostic Memory**: GalaxyData allocated separately from Halo
3. **Metadata-Driven Properties**: YAML → Generated C code → No synchronization
4. **Two-Level Module Control**: Build-time selection + runtime enable/disable
5. **User-Controlled Ordering**: Module execution order from parameter file

---

## Implementation Phases (Revised)

### Phase 0: Memory Management Refactor (1 week) **← NEW, CRITICAL**

**Context**: The PoC revealed that the current block-based memory allocator does not scale with the module architecture. With one property, we needed a 64x increase in the block limit (1024 → 65536). With 50 properties (typical SAM), we would need 500,000+ blocks, which is not sustainable.

**Root Cause**: The allocator was designed for a world with:
- Small number of large allocations
- Known allocation patterns
- Fixed number of data structures

The module system introduces:
- Large number of small allocations (one per property per halo)
- Dynamic allocation patterns
- Unbounded number of property structures

**Goal**: Refactor memory management to scale with separate property allocations

**Deliverables**:
- Replace block-based tracking with category-based tracking
- Implement memory pools for galaxy data (MEM_GALAXIES category)
- Maintain categorized leak detection (critical for debugging)
- Validate with large trees (10,000+ halos)
- Ensure <5% overhead vs monolithic allocation

**Key Decision Points**:

**Decision 1: Memory Pool Strategy**

Top 2 Options:
1. **Category-Based Pools** (RECOMMENDED)
   - Allocate large pools per category (MEM_GALAXIES, MEM_HALOS, etc.)
   - Sub-allocate from pools using simple bump allocator
   - ✅ Scales well, maintains categorization for leak detection
   - ✅ Low overhead, cache-friendly sequential allocation
   - ❌ Requires refactoring memory system (~40 hours)

2. **Hybrid: Core Tracked + Galaxy Pool**
   - Keep block-based tracking for Halo structs
   - Separate untracked pool for GalaxyData
   - ✅ Minimal changes to existing allocator
   - ✅ Isolates galaxy data memory management
   - ❌ Two memory systems to maintain
   - ❌ Loses categorized tracking for galaxy data

**Decision 2: Granularity of Leak Detection**

Top 2 Options:
1. **Category-Level Tracking** (RECOMMENDED)
   - Track total bytes allocated per category
   - Report leaks by category, not individual blocks
   - ✅ Scalable to any number of allocations
   - ✅ Still catches memory leaks effectively
   - ❌ Less granular than current system

2. **Full Block Tracking with Dynamic Array**
   - Keep per-block tracking, use dynamic array instead of fixed size
   - ✅ Maintains current granularity
   - ❌ Higher memory overhead
   - ❌ More complex implementation

**Implementation Notes**:
- This is a **prerequisite for Phase 2** - cannot add 50+ properties with current allocator
- Must maintain zero-leak guarantee (critical for long runs)
- Benchmark against current system: <5% performance degradation acceptable
- Existing memory categories: MEM_HALOS, MEM_TREES, MEM_IO, MEM_UTILITY, MEM_GALAXIES

**Validation**:
- [ ] No memory leaks with 10,000 halo tree
- [ ] Performance <5% slower than current allocator
- [ ] Category-based leak reporting works correctly
- [ ] All existing tests pass

**Files to Modify**:
- `src/util/memory.c/h` - Core allocator implementation
- `src/util/error.c` - Memory reporting functions
- All call sites using `mymalloc_cat()` - verify category assignments

---

### Phase 1.5: Module Interface Enrichment (1 week) **← NEW**

**Context**: The PoC module interface is minimal (just halo array + count). Realistic physics modules need much more:
- Simulation context (redshift, time, dt)
- Cosmological parameters (Omega_m, Hubble_h, etc.)
- Core infrastructure (integration routines, random numbers)
- Module-specific persistent data (cooling tables, lookup grids)

**Current Interface** (Too Minimal):
```c
struct Module {
    const char *name;
    int (*init)(void);
    int (*process_halos)(struct Halo *halos, int ngal);
    int (*cleanup)(void);
};
```

**Goal**: Enrich interface to support realistic physics while maintaining simplicity

**Deliverables**:
- Define `ModuleContext` structure
- Update module interface to pass context
- Provide simulation state (redshift, time, dt)
- Provide access to MimicConfig parameters
- Provide core infrastructure functions
- Add module-specific data storage
- Document interface thoroughly
- Update stellar_mass module to use enriched interface

**Key Decision Points**:

**Decision 1: Module Context Contents**

Top 2 Options:
1. **Moderate Context** (RECOMMENDED)
   ```c
   struct ModuleContext {
       // Simulation state
       double redshift;
       double time;
       double dt;

       // Cosmological parameters (read-only pointer)
       const struct MimicConfig *params;

       // Core infrastructure
       double (*integrate)(integrand_func f, double x0, double x1, void *data);
       double (*random_uniform)(void);

       // Module-specific data (module owns allocation)
       void *module_data;      // Persistent data (cooling tables, etc.)
       void *workspace;        // Temporary scratch space
   };
   ```
   - ✅ Provides essential context without overwhelming complexity
   - ✅ Modules can access params, infrastructure, store their own data
   - ✅ Extensible (can add more infrastructure functions later)
   - ❌ More complex than minimal interface

2. **Minimal Context**
   ```c
   struct ModuleContext {
       double redshift;
       const struct MimicConfig *params;
       void *module_data;
   };
   ```
   - ✅ Simpler, less to understand
   - ❌ Modules must implement their own infrastructure
   - ❌ Code duplication across modules
   - ❌ Will need to add more later anyway

**Decision 2: Module Data Ownership**

Top 2 Options:
1. **Module Owns, Context Stores** (RECOMMENDED)
   - Module allocates data in `init()`
   - Module stores pointer in `ctx->module_data`
   - Module frees data in `cleanup()`
   - ✅ Clear ownership model
   - ✅ Module controls memory management
   - ❌ Module must handle allocation failures

2. **Core Allocates, Module Declares**
   - Module declares data size/type in registration
   - Core allocates during module initialization
   - Core frees during cleanup
   - ✅ Simpler module implementation
   - ❌ Less flexible
   - ❌ Core needs to know about module data types

**Implementation Notes**:
- Keep interface **minimal but sufficient** - easier to add than remove
- Document every field in ModuleContext thoroughly
- Provide example showing how to use each feature
- Update stellar_mass module as reference implementation

**Validation**:
- [ ] stellar_mass module works with enriched interface
- [ ] Module can access redshift, params, infrastructure
- [ ] Module can store persistent data
- [ ] Interface documentation complete
- [ ] Zero memory leaks with module data

**Files to Modify**:
- `src/core/module_interface.h` - Add ModuleContext structure
- `src/core/module_registry.c` - Pass context to modules
- `src/core/build_model.c` - Populate context before module execution
- `src/modules/stellar_mass/*` - Update to use enriched interface
- `docs/developer/module-interface.md` - Document interface (NEW)

---

### Phase 2: Property Metadata System (3-4 weeks) **← HIGHEST PRIORITY**

**Context**: The PoC demonstrated that manual property synchronization is unsustainable. Adding ONE property (StellarMass) required touching 7 files. With 50 properties (typical SAM), this becomes:
- 350 file edits
- 750-1000 lines of boilerplate
- 4-8 hours of error-prone work
- High risk of synchronization bugs (type mismatches, field order errors)

The roadmap originally positioned this as a "nice to have" optimization. **The PoC proves it's absolutely critical** before adding more modules.

**Goal**: Define all properties once in metadata, auto-generate C code

**What Changes**:

**BEFORE** (Manual synchronization):
- Define in `struct GalaxyData` (galaxy_types.h)
- Define in `struct HaloOutput` (types.h)
- Initialize in `init_halo()` (virial.c)
- Copy in `copy_progenitor_halos()` (build_model.c)
- Copy to output in `prepare_halo_for_output()` (binary.c)
- Update binary reader (mimic-plot.py)
- Update HDF5 reader (hdf5_reader.py)

**AFTER** (Metadata-driven):
- Edit properties.yaml
- Run `make generate` (or automatic)
- Done

**Deliverables**:
- Property metadata format specification (YAML)
- Python code generator script
- Generated C structures (Halo, GalaxyData, HaloOutput)
- Generated property accessors (macros)
- Generated property metadata table
- Property-driven output system (binary + HDF5)
- Migrate existing StellarMass to metadata
- Documentation for adding properties

**Key Decision Points**:

**Decision 1: Metadata Format**

Top 2 Options:
1. **YAML** (RECOMMENDED)
   ```yaml
   halo_properties:
     - name: Mvir
       type: float
       units: "1e10 Msun/h"
       description: "Virial mass"
       output: true

   galaxy_properties:
     - name: StellarMass
       type: float
       units: "1e10 Msun/h"
       description: "Total stellar mass"
       output: true
       created_by: stellar_mass
   ```
   - ✅ Human-readable, supports comments
   - ✅ Python has excellent libraries (PyYAML)
   - ✅ Standard for config files
   - ❌ Whitespace-sensitive

2. **JSON**
   ```json
   {
     "galaxy_properties": [
       {
         "name": "StellarMass",
         "type": "float",
         "units": "1e10 Msun/h"
       }
     ]
   }
   ```
   - ✅ Widely supported, strict syntax
   - ✅ Easy to parse
   - ❌ No comments
   - ❌ More verbose

**Decision 2: Generated Code Placement**

Top 2 Options:
1. **Commit Generated Code to Git** (RECOMMENDED)
   - Generated files checked into repository
   - CI verifies they're up-to-date with metadata
   - Build doesn't require Python
   - ✅ Builds work without Python installed
   - ✅ Diffs show changes to generated code (reviewable)
   - ✅ Clear what changed in code reviews
   - ❌ Extra files in repository

2. **Generate During Build**
   - Generated files not in git
   - Makefile triggers generation when metadata changes
   - ✅ No extra files in repo
   - ❌ Requires Python to build
   - ❌ Slower builds
   - ❌ Can't see generated code in diffs

**Property Metadata Specification**:

Each property must define:
- `name`: Property identifier (used in C code)
- `type`: Data type (float, int, vec3_float, etc.)
- `units`: Physical units (for documentation)
- `description`: Human-readable description
- `output`: Whether to write to output files
- `created_by`: (galaxy props only) Which module creates it

**Generated Files**:
```
src/include/generated/
├── property_defs.h         # struct Halo, struct GalaxyData definitions
├── property_accessors.h    # GET/SET macros for type-safe access
└── property_metadata.c     # Metadata table for output system
```

**Code Generation Strategy**:
```makefile
# Makefile targets
generate:
    python3 scripts/generate_properties.py

check-generated:
    @python3 scripts/check_generated.py || \
        (echo "Generated files out of date! Run: make generate"; exit 1)

# Dependency
$(OBJECTS): src/include/generated/property_defs.h
```

**Implementation Notes**:
- Start by moving 24 existing halo properties to YAML
- Validate bit-identical output before adding StellarMass
- Then migrate StellarMass from hardcoded to metadata
- Generate property accessor macros: `GET_HALO_MVIR(h)`, `SET_GALAXY_STELLARMASS(g, val)`
- Update output system to use property metadata table (eliminates manual HDF5 field lists)

**Validation**:
- [ ] All 24 halo properties defined in YAML
- [ ] Generated structs match current layout exactly
- [ ] Bit-identical output with generated code
- [ ] StellarMass migrated to metadata system
- [ ] Property-driven HDF5 output works
- [ ] Adding test property works end-to-end
- [ ] Documentation for adding properties complete

**Files to Create**:
- `metadata/properties/halo_properties.yaml` - Halo property definitions
- `metadata/properties/galaxy_properties.yaml` - Galaxy property definitions
- `scripts/generate_properties.py` - Code generator
- `scripts/check_generated.py` - CI validation script
- `src/include/generated/property_defs.h` - Generated (committed to git)
- `src/include/generated/property_accessors.h` - Generated (committed to git)
- `src/include/generated/property_metadata.c` - Generated (committed to git)
- `docs/developer/adding-properties.md` - Documentation

**Files to Modify**:
- `src/io/output/binary.c` - Use property metadata
- `src/io/output/hdf5.c` - Use property metadata (replaces ~150 lines)
- `output/mimic-plot/mimic-plot.py` - Generate dtype from metadata
- `Makefile` - Add generate and check-generated targets

---

### Phase 3: Runtime Module Configuration (1-2 weeks)

**Context**: Currently modules are hardcoded (manual registration in main.c, always run). We need runtime control via parameter files so users can:
- Enable/disable modules without recompilation
- Configure module-specific parameters
- Control module execution order

**Goal**: Enable/disable and configure modules via parameter file

**Deliverables**:
- Extend parameter parser to read module configuration
- Support module-specific parameters (namespaced)
- Module enable/disable flags
- User-controlled module ordering
- Parameter validation
- Update stellar_mass to read efficiency from config

**Parameter File Format** (Extend .par):
```
%------------------------------------------
%----- GALAXY PHYSICS MODULES -------------
%------------------------------------------

# Order matters! Execution order = order listed below
EnabledModules    cooling_SD93,star_formation_KS98,feedback_simple

# Module-specific parameters (prefixed with module name)
StellarMass_Efficiency     0.1
StellarMass_MinMass       1e8

Cooling_Model             Sutherland_Dopita_1993
Cooling_ReionRedshift     6.0

StarFormation_Efficiency   0.02
StarFormation_MinGasMass  1e9
```

**Key Decision Points**:

**Decision 1: Parameter Namespace**

Top 2 Options:
1. **Prefixed Parameters** (RECOMMENDED for Phase 3)
   ```
   StellarMass_Efficiency  0.1
   Cooling_Model           SD93
   ```
   - ✅ Backward compatible with .par format
   - ✅ Uses existing parser
   - ✅ Clear module ownership (prefix)
   - ❌ Pollutes global namespace
   - ❌ Verbose

2. **Nested Sections** (Better for Future)
   ```
   [StellarMass]
   Efficiency = 0.1

   [Cooling]
   Model = SD93
   ```
   - ✅ Clean namespace
   - ✅ Better organization
   - ❌ Requires parser changes (non-trivial)
   - ❌ Breaking change for existing .par files

**Recommendation**: Use prefixed parameters for Phase 3 (minimal changes). Consider nested sections when migrating to JSON (Phase 4+).

**Decision 2: Module Execution Order**

Top 2 Options:
1. **User-Specified Order** (RECOMMENDED)
   ```
   EnabledModules = cooling,star_formation,feedback
   ```
   Execution order = order in parameter file
   - ✅ Simple, explicit, user controls physics sequence
   - ✅ No dependency graph or topological sort needed (~100 lines saved)
   - ✅ Clear errors: "Module X failed at position Y"
   - ✅ Users can experiment with different orderings
   - ❌ User must understand physics dependencies

2. **Automatic Dependency Resolution**
   ```c
   struct Module {
       const char *dependencies[];  // List of required modules
   };
   ```
   Core builds dependency graph, executes in correct order
   - ✅ Prevents incorrect ordering
   - ✅ Automatic ordering
   - ❌ Complex: dependency graph, topological sort, cycle detection
   - ❌ Hidden module execution order
   - ❌ Harder to debug

**Recommendation**: User-specified ordering. Scientists understand their physics. Can add dependency validation later if needed.

**Implementation Notes**:
- Parse `EnabledModules` parameter
- Build ordered execution array
- Skip non-enabled modules in pipeline
- Validate module names (error if unknown module specified)
- Pass module-specific parameters to modules during init()

**Module Parameter Access**:
```c
// In stellar_mass.c
int stellar_mass_init(struct ModuleContext *ctx) {
    // Read module-specific parameters
    float efficiency = 0.1;  // default
    read_module_parameter(ctx->params, "StellarMass_Efficiency", &efficiency);

    // Store in module data
    struct StellarMassData *data = malloc(sizeof(*data));
    data->efficiency = efficiency;
    ctx->module_data = data;

    return 0;
}
```

**Validation**:
- [ ] Can enable/disable modules via EnabledModules
- [ ] Module execution order matches parameter file order
- [ ] Module-specific parameters work
- [ ] Invalid module name produces clear error
- [ ] stellar_mass reads efficiency from config
- [ ] Bit-identical output when same modules enabled

**Files to Modify**:
- `src/core/read_parameter_file.c` - Parse EnabledModules and module params
- `src/core/module_registry.c` - Build ordered execution pipeline
- `src/core/module_interface.h` - Add parameter access functions
- `src/modules/stellar_mass/stellar_mass.c` - Read parameters
- `docs/user/configuration.md` - Document module configuration

---

### Phase 4: Build-Time Module Selection (1 week)

**Context**: Currently all modules are always compiled. For deployment flexibility and performance (especially on HPC), we want:
- Build only the modules you need (smaller executables)
- Predefined profiles for published models
- But still allow runtime enable/disable of compiled modules

This gives **two-level control**:
1. **Build-time**: Which modules to compile
2. **Runtime**: Which compiled modules to execute

**Goal**: Compile-time module selection with predefined profiles

**Deliverables**:
- Makefile MODULES variable for custom selection
- Makefile PROFILE variable for predefined sets
- Build-time module table generation
- Module profiles (croton2006, henriques2015, all)
- Documentation for custom builds

**Makefile Design**:
```makefile
# Default module set
MODULES ?= cooling_SD93 star_formation_KS98 feedback_simple

# User can override
# make MODULES="cooling_G12 star_formation_KMT09"

# Or use profiles
PROFILE ?= default

ifeq ($(PROFILE),croton2006)
    MODULES = cooling_SD93 star_formation_KS98 feedback_croton06 mergers_guo11
else ifeq ($(PROFILE),henriques2015)
    MODULES = cooling_G12 star_formation_KMT09 AGN_bondi feedback_H15
else ifeq ($(PROFILE),all)
    MODULES = $(notdir $(wildcard src/modules/*))
endif

# Compile only specified modules
MODULE_SRCS = $(foreach mod,$(MODULES),src/modules/$(mod)/$(mod).c)
```

**Module Table Generation**:
```python
# scripts/generate_module_table.py
# Scans src/modules/*/module_info.yaml
# Generates src/modules/module_table_generated.c

# Generated code:
#include "cooling_SD93/cooling_SD93.h"
#include "star_formation_KS98/star_formation_KS98.h"

struct Module* mimic_all_modules[] = {
    &cooling_SD93_module,
    &star_formation_KS98_module,
    NULL
};
```

**Key Decision Points**:

**Decision 1: Module Discovery**

Top 2 Options:
1. **Build-Time Generation** (RECOMMENDED)
   - Script scans src/modules/ at build time
   - Generates module_table_generated.c
   - Static linking, all modules in one executable
   - ✅ Automatic discovery
   - ✅ No manual maintenance
   - ✅ Simple deployment (one binary)
   - ❌ Requires Python at build time

2. **Dynamic Loading**
   - Runtime dlopen() of shared libraries
   - True plugin architecture
   - ✅ Can add modules without recompiling core
   - ❌ Complex, platform-specific
   - ❌ Deployment complexity
   - ❌ Security risks

**Decision 2: Module Metadata**

Top 2 Options:
1. **module_info.yaml per Module** (RECOMMENDED)
   ```yaml
   # src/modules/stellar_mass/module_info.yaml
   name: stellar_mass
   version: 1.0.0
   description: "Simple stellar mass prescription"
   parameters:
     - StellarMass_Efficiency
   ```
   - ✅ Modules self-describe
   - ✅ Can validate at build time
   - ✅ Documentation generation

2. **Hardcoded in Module**
   ```c
   struct Module stellar_mass_module = {
       .name = "stellar_mass",
       .version = "1.0.0"
   };
   ```
   - ✅ Simpler (no YAML)
   - ❌ Can't inspect without compiling
   - ❌ Harder to generate documentation

**Implementation Notes**:
- Each module gets a `module_info.yaml` describing metadata
- Build-time script generates registration table
- Predefined profiles for reproducibility (published models)
- Generated files committed to git (like property_defs.h)

**User Workflow**:
```bash
# Use predefined profile
make PROFILE=croton2006
./mimic millennium.par

# Custom module set
make MODULES="cooling_SD93 cooling_G12 star_formation_KS98"
./mimic millennium.par

# Compile everything (exploration mode)
make PROFILE=all
./mimic millennium.par

# Runtime: enable subset of compiled modules
# In millennium.par: EnabledModules = cooling_SD93,star_formation_KS98
./mimic millennium.par  # No recompile needed!
```

**Validation**:
- [ ] Can build with MODULES variable
- [ ] Can build with PROFILE variable
- [ ] Only specified modules compiled
- [ ] Generated module table works
- [ ] Can switch between compiled modules at runtime
- [ ] Profile documentation complete

**Files to Create**:
- `scripts/generate_module_table.py` - Module table generator
- `src/modules/stellar_mass/module_info.yaml` - Module metadata
- `src/modules/module_table_generated.c` - Generated (committed to git)
- `docs/developer/module-profiles.md` - Profile documentation

**Files to Modify**:
- `Makefile` - Add MODULES and PROFILE support
- `src/core/module_registry.c` - Use generated module table

---

### Phase 5: Testing Framework (1-2 weeks) **← NEW, CRITICAL**

**Context**: The PoC had no automated testing - validation was by visual inspection of plots and memory leak detection. This is inadequate for:
- Complex modules with subtle bugs
- Regression prevention
- Scientific validation
- Confidence in refactoring

**Goal**: Establish comprehensive testing before adding more modules

**Deliverables**:
- Unit test framework for module system
- Integration tests for full pipeline
- Scientific validation framework
- Conservation law checks
- Regression test suite
- CI integration

**Test Structure**:
```
tests/
├── unit/
│   ├── test_module_registry.c      # Module add/remove/lookup
│   ├── test_property_system.c      # Property accessors, metadata
│   ├── test_memory.c               # Memory pools, leak detection
│   └── test_parameter_parsing.c    # Module config parsing
├── integration/
│   ├── test_module_pipeline.c      # Multi-module execution
│   ├── test_full_run.c             # End-to-end: load→process→output
│   └── test_error_handling.c       # Error propagation
├── scientific/
│   ├── test_conservation.c         # Mass/energy conservation
│   ├── test_published_results.c    # Compare to Croton+2006, etc.
│   └── test_bit_identical.c        # Validate no regressions
└── data/
    ├── small_tree.dat              # Minimal test tree
    └── expected_output/            # Known-good outputs
```

**Key Decision Points**:

**Decision 1: Test Framework**

Top 2 Options:
1. **Python Integration Tests + Minimal C Unit Tests** (RECOMMENDED)
   - Python for integration/scientific tests (use existing plotting infrastructure)
   - Minimal C framework for unit tests (avoid dependencies)
   - ✅ Leverages existing Python ecosystem
   - ✅ Scientific tests easier in Python
   - ✅ No new dependencies for core
   - ❌ Two test systems to maintain

2. **C Test Framework (cmocka)**
   - Use cmocka for all tests
   - ✅ Unified testing approach
   - ✅ Professional framework
   - ❌ New dependency
   - ❌ Scientific tests harder in C

**Decision 2: Bit-Identical Validation**

Top 2 Options:
1. **MD5 Checksums of Output** (RECOMMENDED)
   ```bash
   ./mimic test.par
   md5sum output/* > checksums.txt
   # Store in tests/data/expected_checksums.txt
   ```
   - ✅ Fast, simple
   - ✅ Catches any output changes
   - ❌ Breaks on floating-point changes

2. **Statistical Comparison**
   - Compare mass functions, correlation functions
   - Allow small numerical differences
   - ✅ More robust to harmless changes
   - ❌ Can miss subtle bugs
   - ❌ More complex implementation

**Recommendation**: Use both - MD5 for regression, statistical for validation.

**Test Levels**:

**Level 1: Unit Tests** (Fast, run on every build)
- Module registration/lookup
- Property accessors
- Memory allocation/deallocation
- Parameter parsing

**Level 2: Integration Tests** (Medium, run on PR)
- Full module pipeline execution
- Error handling
- Multi-module interaction

**Level 3: Scientific Validation** (Slow, run on major changes)
- Conservation laws (mass, energy)
- Comparison with published results
- Bit-identical output validation

**Level 4: Performance** (Manual, before releases)
- Memory usage tracking
- Execution time benchmarks
- Scaling tests

**Implementation Notes**:
- Start with minimal C unit test framework (no external dependencies)
- Use Python for integration and scientific tests
- Store test data in git (small trees only)
- CI runs Level 1 + 2 on every commit
- Developer runs Level 3 before major merges

**Validation**:
- [ ] Unit tests for module system pass
- [ ] Integration test for stellar_mass module passes
- [ ] Bit-identical validation works
- [ ] CI integrated with GitHub Actions
- [ ] Documentation for writing tests complete

**Files to Create**:
- `tests/unit/test_module_registry.c` - Module system tests
- `tests/integration/test_stellar_mass.py` - End-to-end test
- `tests/scientific/test_conservation.py` - Conservation checks
- `tests/data/small_tree.dat` - Test data
- `.github/workflows/ci.yml` - CI configuration
- `docs/developer/testing.md` - Testing guide

---

### Phase 6: First Realistic Module (2-3 weeks)

**Context**: stellar_mass is a proof-of-concept (one line of physics). We need a realistic module to validate:
- Enriched module interface under real workload
- Module data management (persistent tables)
- Integration routines
- Scientific validation process
- Documentation for module developers

**Goal**: Implement cooling module as real-world test case

**Deliverables**:
- Cooling module with realistic physics
- Cooling rate tables (Sutherland & Dopita 1993)
- Redshift-dependent UV background
- Metallicity dependence
- Full module lifecycle (init/process/cleanup)
- Scientific validation against published results
- Module developer guide based on experience

**Cooling Module Scope**:

**Physics**:
- Sutherland & Dopita (1993) cooling function
- Redshift-dependent UV background (Haardt & Madau 2012)
- Metallicity dependence (Wiersma et al. 2009)
- Cooling rate: Λ(T, Z, z)

**Data Structures**:
```c
struct CoolingTables {
    double *temperature_grid;     // 1000 points
    double *cooling_rate_grid;    // 1000 points
    size_t n_temp;
    // Loaded once in init(), shared across all halos
};

struct CoolingData {
    float ColdGas;               // In GalaxyData (via property metadata)
    float CoolingRate;           // In GalaxyData (via property metadata)
};
```

**Module Implementation**:
```c
int cooling_init(struct ModuleContext *ctx) {
    // Load cooling tables
    struct CoolingTables *tables = load_cooling_tables("data/cooling_SD93.dat");
    ctx->module_data = tables;
    return 0;
}

int cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    struct CoolingTables *tables = ctx->module_data;

    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type != 0) continue;  // Centrals only

        // Get virial temperature
        double Tvir = get_virial_temperature(halos[i].Mvir, ctx->redshift);

        // Interpolate cooling rate
        double Lambda = interpolate_cooling_rate(tables, Tvir, /* Z= */ 0.02);

        // Calculate cooled gas
        double cooling_time = /* ... physics ... */;
        double cooled_mass = /* ... physics ... */;

        // Update galaxy properties
        halos[i].galaxy->ColdGas += cooled_mass;
        halos[i].galaxy->CoolingRate = Lambda;
    }
    return 0;
}

int cooling_cleanup(struct ModuleContext *ctx) {
    free_cooling_tables(ctx->module_data);
    return 0;
}
```

**Key Decision Points**:

**Decision 1: Module Data vs Galaxy Properties**

What goes where?

Top 2 Options:
1. **Persistent Data in Module, Properties in GalaxyData** (RECOMMENDED)
   - Cooling tables → `ctx->module_data` (shared, read-only)
   - ColdGas, CoolingRate → `GalaxyData` (per-galaxy, output)
   - ✅ Clear separation
   - ✅ Memory efficient (tables shared)
   - ✅ Properties automatically output

2. **Everything in GalaxyData**
   - ColdGas, CoolingRate, pointer to tables → all in GalaxyData
   - ✅ All module data in one place
   - ❌ Wastes memory (table pointer per galaxy)
   - ❌ Table pointer doesn't make sense in output

**Decision 2: Scientific Validation Strategy**

How to validate cooling physics?

Top 2 Options:
1. **Multi-Level Validation** (RECOMMENDED)
   - Unit test: Cooling rate at specific T, Z matches tables
   - Integration test: Run on small tree, check output
   - Scientific test: Compare mass functions to published results
   - Conservation test: Verify mass conservation
   - ✅ Comprehensive
   - ❌ More work

2. **Visual Validation Only**
   - Plot cooling rates, gas fractions
   - Visual comparison to published figures
   - ✅ Quick
   - ❌ Not automated
   - ❌ Can miss subtle bugs

**Implementation Notes**:
- Add ColdGas and CoolingRate to galaxy_properties.yaml
- Load cooling tables from data/ directory
- Use core integration routines (from ModuleContext)
- Implement proper error handling (table load failures, interpolation bounds)
- Document every physics choice (which papers, which equations)

**Validation**:
- [ ] Cooling module compiles and registers
- [ ] Can enable/disable via parameter file
- [ ] Cooling tables load correctly
- [ ] Cooling rates match published values
- [ ] Gas mass conserved
- [ ] ColdGas, CoolingRate appear in output
- [ ] Scientific validation passes
- [ ] Module developer guide written based on experience

**Files to Create**:
- `src/modules/cooling_SD93/cooling_SD93.c` - Module implementation
- `src/modules/cooling_SD93/cooling_tables.c` - Table loading/interpolation
- `src/modules/cooling_SD93/module_info.yaml` - Module metadata
- `data/cooling_SD93.dat` - Cooling rate tables
- `tests/scientific/test_cooling_validation.py` - Scientific tests
- `docs/developer/module-developer-guide.md` - Developer guide
- `docs/physics/cooling-module.md` - Physics documentation

**Files to Modify**:
- `metadata/properties/galaxy_properties.yaml` - Add ColdGas, CoolingRate

---

## Success Metrics

Each phase has specific success criteria:

### Phase 0: Memory Management
- [ ] No memory leaks with 10,000 halo tree
- [ ] Performance overhead <5% vs current system
- [ ] Category-based leak reporting works
- [ ] All existing tests pass
- [ ] Memory usage predictable and bounded

### Phase 1.5: Module Interface
- [ ] ModuleContext structure defined and documented
- [ ] stellar_mass module works with enriched interface
- [ ] Module can access redshift, params, infrastructure
- [ ] Module data ownership clear
- [ ] Zero memory leaks with module data

### Phase 2: Property Metadata
- [ ] All 24 halo properties defined in YAML
- [ ] Generated code produces bit-identical output
- [ ] StellarMass migrated to metadata
- [ ] Adding test property works end-to-end
- [ ] Output system automatically includes new properties
- [ ] No manual synchronization required

### Phase 3: Runtime Configuration
- [ ] Can enable/disable modules via parameter file
- [ ] Module execution order matches config
- [ ] Module-specific parameters work
- [ ] Invalid config produces clear error
- [ ] Bit-identical when same modules enabled

### Phase 4: Build-Time Selection
- [ ] Build with MODULES variable works
- [ ] Predefined profiles work
- [ ] Only specified modules compiled
- [ ] Can switch between compiled modules at runtime
- [ ] Documentation complete

### Phase 5: Testing Framework
- [ ] Unit tests pass
- [ ] Integration tests pass
- [ ] Bit-identical validation works
- [ ] CI integrated
- [ ] Test coverage >80%

### Phase 6: Realistic Module
- [ ] Cooling module passes scientific validation
- [ ] Conservation laws satisfied
- [ ] Module developer guide complete
- [ ] Can be used as template for future modules

### Overall Vision Compliance

After all phases:
- ✅ Physics-Agnostic Core (Principle 1)
- ✅ Runtime Modularity (Principle 2)
- ✅ Metadata-Driven Architecture (Principle 3)
- ✅ Single Source of Truth (Principle 4)
- ✅ Unified Processing Model (Principle 5)
- ✅ Memory Efficiency (Principle 6)
- ✅ Format-Agnostic I/O (Principle 7)
- ✅ Type Safety and Validation (Principle 8)

---

## Risk Management

### High Priority Risks

**Risk 1: Memory Management Refactor Breaks Something**
- **Probability**: Medium
- **Impact**: High (could break core functionality)
- **Mitigation**:
  - Comprehensive testing before/after
  - Bit-identical validation on large trees
  - Gradual migration (category by category)
  - Feature branch with easy rollback
- **Contingency**: Revert to current allocator, increase block limit as temporary fix

**Risk 2: Property Metadata System Adds Complexity**
- **Probability**: Low
- **Impact**: Medium (learning curve for developers)
- **Mitigation**:
  - Excellent documentation
  - Simple examples
  - Error messages guide to metadata
  - Gradual migration (halo props → galaxy props)
- **Contingency**: Keep manual synchronization as fallback option

**Risk 3: Performance Degradation from Separate Allocations**
- **Probability**: Medium
- **Impact**: High (could make code unusable)
- **Mitigation**:
  - Benchmark at every phase
  - Profile hotspots
  - Memory pool optimization
  - Cache-friendly data layout
- **Contingency**: Revert to embedded properties if >10% overhead

**Risk 4: Scientific Accuracy Compromised by Module Boundaries**
- **Probability**: Low
- **Impact**: Critical (wrong science)
- **Mitigation**:
  - Conservation law checks
  - Comparison with published results
  - Scientific code review
  - Bit-identical validation where possible
- **Contingency**: Redesign module interface if artifacts detected

**Risk 5: Adoption Failure (Too Complex for Users/Developers)**
- **Probability**: Medium
- **Impact**: High (system unused)
- **Mitigation**:
  - Extensive documentation
  - Simple examples
  - Clear error messages
  - User support during transition
- **Contingency**: Simplify interface, improve docs, provide templates

### Medium Priority Risks

**Risk 6: Build System Complexity Increases**
- **Mitigation**: Comprehensive build documentation, test on multiple platforms
- **Contingency**: Simplify build if problems arise

**Risk 7: Module Ordering Bugs**
- **Mitigation**: Clear documentation of ordering requirements, validation
- **Contingency**: Add dependency checking if user errors are common

**Risk 8: Parameter File Format Limitations**
- **Mitigation**: Design with future JSON migration in mind
- **Contingency**: Migrate to JSON sooner if .par becomes limiting

---

## Timeline and Effort

### Phase Estimates

| Phase | Duration | Effort (hrs) | Priority |
|-------|----------|--------------|----------|
| Phase 0: Memory Refactor | 1 week | 40 | CRITICAL |
| Phase 1.5: Interface Enrichment | 1 week | 30 | HIGH |
| Phase 2: Property Metadata | 3-4 weeks | 100 | CRITICAL |
| Phase 3: Runtime Config | 1-2 weeks | 50 | MEDIUM |
| Phase 4: Build Selection | 1 week | 30 | MEDIUM |
| Phase 5: Testing Framework | 1-2 weeks | 50 | HIGH |
| Phase 6: Realistic Module | 2-3 weeks | 70 | HIGH |
| **Total** | **10-16 weeks** | **370 hrs** | |

### Critical Path

The **critical path** items that must be completed before other work:

1. **Phase 0** (Memory) → Blocks Phase 2
2. **Phase 2** (Metadata) → Blocks all future property additions
3. **Phase 5** (Testing) → Needed before Phase 6 and all future modules

### Parallelization Opportunities

After Phase 0 completes:
- Phase 1.5 (Interface) and Phase 2 (Metadata) can proceed in parallel
- Phase 3 (Runtime Config) depends on Phase 1.5
- Phase 4 (Build Selection) can happen anytime
- Phase 5 (Testing) should happen before Phase 6

### Milestones

**Milestone 1** (Week 4): Phase 0 + 1.5 Complete
- Memory management scales
- Module interface enriched
- Ready for property system

**Milestone 2** (Week 8): Phase 2 Complete
- Property metadata system working
- Zero-synchronization property additions
- Foundation for scaling to 50+ properties

**Milestone 3** (Week 12): Phases 3-5 Complete
- Runtime module configuration
- Build-time selection
- Testing framework
- Ready for production modules

**Milestone 4** (Week 16): Phase 6 Complete
- Realistic cooling module validated
- Module developer guide complete
- Infrastructure proven under real workload

---

## Open Questions and Decisions

### Decisions Required Before Starting

**AD1: Memory Pool Implementation Details**
- **Question**: Exact pool allocation strategy (fixed blocks, growth factor, etc.)?
- **Impact**: Performance, memory usage
- **Deadline**: Before Phase 0
- **Owner**: Core team

**AD2: ModuleContext Final Contents**
- **Question**: Which infrastructure functions to expose?
- **Impact**: Module developer experience
- **Deadline**: Before Phase 1.5
- **Owner**: Module developers + core team

**AD3: Property Metadata Schema**
- **Question**: Final YAML schema with all required fields?
- **Impact**: Property system flexibility
- **Deadline**: Before Phase 2
- **Owner**: Core team

### Decisions Can Be Deferred

**AD4: Error Handling Policy**
- **Question**: Abort vs continue on module failure?
- **Current**: Abort (safe default)
- **Deadline**: Can decide in Phase 5 based on testing experience
- **Options**: Compile flag to control behavior

**AD5: Primary Output Format**
- **Question**: Binary vs HDF5 as primary?
- **Current**: Support both
- **Deadline**: Can decide after Phase 2 when property system is in place
- **Recommendation**: Prioritize HDF5 for new features (self-describing)

**AD6: Parameter File Migration**
- **Question**: When to migrate from .par to JSON?
- **Current**: Extend .par
- **Deadline**: Can decide in Phase 3 based on complexity
- **Trigger**: If module config becomes too complex for .par

**AD7: Module Communication**
- **Question**: How should modules share data beyond properties?
- **Current**: Properties only (simple)
- **Deadline**: Can decide in Phase 6 based on cooling module experience
- **Options**: Add specialized APIs in v2 if needed

### Questions to Explore During Implementation

**Q1: Performance Impact of Separate Allocation**
- Benchmark early in Phase 0
- Profile memory access patterns
- Measure cache performance

**Q2: User Experience with Module Configuration**
- Get user feedback during Phase 3
- Iterate on parameter format
- Document common use cases

**Q3: Scientific Validation Requirements**
- Define in Phase 5
- Learn from cooling module (Phase 6)
- Establish standards for future modules

**Q4: Documentation Effectiveness**
- Get developer feedback throughout
- Iterate on examples
- Measure time-to-first-module

---

## Post-Transformation Roadmap

After completing Phases 0-6, the following work becomes possible:

### Additional Physics Modules

With infrastructure in place, adding new modules should be rapid:
- Star formation (multiple prescriptions)
- Stellar feedback
- AGN feedback
- Galaxy mergers
- Disk instability
- Chemical enrichment
- Dust physics

**Estimated effort per module**: 1-3 weeks depending on complexity

### Advanced Features

Once basic module system is proven:
- Property provenance tracking (which module created/modified property)
- Property validation (runtime range checking)
- Module parameter exploration (grid search)
- Multi-phase module execution (if needed)
- Performance optimization (module parallelization)

### Configuration Evolution

As needs grow:
- Migrate to JSON configuration (when .par becomes limiting)
- Add schema validation
- Support module-specific config files
- Configuration GUI for complex setups

### Scientific Workflows

With flexible module system:
- Reproduce published models (Croton+2006, Henriques+2015, etc.)
- Compare physics prescriptions
- Parameter exploration
- Scientific uncertainty quantification

---

## Integration and Validation Strategy

### Per-Phase Validation

Every phase must pass validation before merging:

**Critical Requirement**: Bit-identical output after each infrastructure phase (until new physics is added)

```bash
# Baseline (before transformation)
./mimic input/millennium.par
md5sum output/results/millennium/model_z0.000_0 > baseline.md5

# After Phase 0 (memory refactor)
make clean && make
./mimic input/millennium.par
diff <(md5sum output/results/millennium/model_z0.000_0) baseline.md5
# MUST be identical

# After Phase 2 (property system)
make generate && make clean && make
./mimic input/millennium.par
diff <(md5sum output/results/millennium/model_z0.000_0) baseline.md5
# MUST be identical
```

**If output changes**: Bug in transformation, must fix before proceeding.

### Feature Branch Strategy

Each phase on separate feature branch:
- `feature/phase-0-memory-refactor`
- `feature/phase-1.5-module-interface`
- `feature/phase-2-property-metadata`
- `feature/phase-3-runtime-config`
- `feature/phase-4-build-selection`
- `feature/phase-5-testing`
- `feature/phase-6-cooling-module`

Merge to main only after:
- [ ] All phase deliverables complete
- [ ] Validation passes
- [ ] Documentation updated
- [ ] Code review approved

### Rollback Plan

Can revert phases independently if needed:
- Each phase is self-contained
- Clear commit messages
- Documentation of what changed
- Easy to identify and revert

---

## Conclusion

This roadmap provides a **complete, standalone guide** for transforming Mimic to support modular galaxy physics. It incorporates critical insights from the minimal module system PoC that were not evident during planning.

### Key Insights from PoC

1. **Memory management refactor is critical path** (Phase 0 - NEW)
2. **Property metadata system is not optional** (Phase 2 - elevated priority)
3. **Module interface needs enrichment** (Phase 1.5 - NEW)
4. **Testing framework is prerequisite** (Phase 5 - elevated importance)
5. **Many architectural decisions remain** (documented in this roadmap)

### Why This Approach Will Succeed

✅ **Validated Core Concepts**: PoC proved architecture is sound
✅ **Realistic Timeline**: Based on actual implementation experience, not estimates
✅ **Clear Priorities**: Critical path items identified and prioritized
✅ **Risk Management**: Risks identified with mitigations
✅ **Comprehensive Testing**: Testing built in from Phase 5 onward
✅ **Incremental Delivery**: Each phase delivers value independently
✅ **Clear Success Metrics**: Objective validation criteria per phase

### Next Steps

1. **Review this roadmap** with team/stakeholders
2. **Make pending architectural decisions** (AD1-AD7)
3. **Begin Phase 0** (Memory Management Refactor)
4. **Track progress** against milestones
5. **Iterate** based on lessons learned during implementation

### Success Metrics

**Technical Success**:
- All 8 vision principles satisfied
- Infrastructure supports 50+ galaxy properties
- Modules can be developed independently
- Zero memory leaks under all conditions
- <5% performance overhead from infrastructure

**Scientific Success**:
- Can reproduce published models
- Conservation laws satisfied
- Scientific validation passes
- Enables new research through flexibility

**Developer Success**:
- New module in <1 week for experienced developer
- New property in <1 minute
- Clear documentation and examples
- Positive developer experience

**Timeline**: 10-16 weeks for solid foundation, then rapid module development

---

## Appendix: Quick Reference

### File Organization After Transformation

```
mimic/
├── src/
│   ├── core/
│   │   ├── module_interface.h         # Module API definition
│   │   ├── module_registry.[ch]       # Registration and lifecycle
│   │   └── halo_properties/           # Core halo physics (unchanged)
│   ├── modules/
│   │   ├── stellar_mass/              # PoC module
│   │   ├── cooling_SD93/              # Realistic module (Phase 6)
│   │   └── module_table_generated.c   # Build-time generated
│   ├── include/
│   │   ├── galaxy_types.h             # GalaxyData structure
│   │   └── generated/
│   │       ├── property_defs.h        # Generated structs
│   │       ├── property_accessors.h   # Generated macros
│   │       └── property_metadata.c    # Generated metadata
│   └── util/
│       └── memory.[ch]                # Refactored allocator
├── metadata/
│   └── properties/
│       ├── halo_properties.yaml       # Halo property definitions
│       └── galaxy_properties.yaml     # Galaxy property definitions
├── scripts/
│   ├── generate_properties.py         # Property code generator
│   ├── generate_module_table.py       # Module table generator
│   └── check_generated.py             # CI validation
├── tests/
│   ├── unit/                          # C unit tests
│   ├── integration/                   # Python integration tests
│   └── scientific/                    # Scientific validation
└── docs/
    ├── architecture/
    │   ├── vision.md                  # Architectural principles
    │   ├── roadmap_v2.md             # This document
    │   └── poc-lessons-learned.md    # PoC insights
    ├── developer/
    │   ├── module-developer-guide.md  # How to write modules
    │   ├── adding-properties.md       # How to add properties
    │   └── testing.md                 # Testing guide
    └── user/
        └── configuration.md           # Module configuration
```

### Key Commands

```bash
# Generate code from metadata
make generate

# Build with specific modules
make MODULES="cooling_SD93 star_formation_KS98"

# Build with profile
make PROFILE=croton2006

# Run tests
make test

# Check generated code is up-to-date
make check-generated

# Run with module configuration (millennium.par)
# EnabledModules = cooling,star_formation
./mimic input/millennium.par
```

### Property Addition Workflow (After Phase 2)

```bash
# 1. Add to metadata
vim metadata/properties/galaxy_properties.yaml
# Add:
#   - name: NewProperty
#     type: float
#     ...

# 2. Generate code
make generate

# 3. Implement in module
vim src/modules/my_module/my_module.c
# Use: halos[i].galaxy->NewProperty = value;

# 4. Build and test
make
./mimic test.par

# Done! No manual synchronization needed.
```

### Module Development Workflow (After Phase 6)

```bash
# 1. Create module directory
mkdir src/modules/my_module

# 2. Create module_info.yaml
cat > src/modules/my_module/module_info.yaml << EOF
name: my_module
version: 1.0.0
description: "My physics module"
EOF

# 3. Implement module (use cooling_SD93 as template)
vim src/modules/my_module/my_module.c

# 4. Add properties to metadata if needed
vim metadata/properties/galaxy_properties.yaml

# 5. Generate and build
make generate
make MODULES="my_module"

# 6. Test
./mimic test.par

# 7. Scientific validation
python tests/scientific/test_my_module.py
```

---

**Document Version**: 2.0
**Last Updated**: 2025-11-07
**Status**: Ready for Implementation
**Next Review**: After Phase 0 completion
