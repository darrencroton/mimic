# Mimic Architectural Transformation Roadmap

**Document Status**: Strategic Planning - Design Landscape
**Created**: 2025-11-05
**Purpose**: Define the transformation strategy from current state to vision architecture

---

## Executive Summary

This document presents the strategic landscape for transforming Mimic from its current halo-tracking implementation into the physics-agnostic, metadata-driven framework envisioned in `vision.md`. Rather than prescriptive implementation steps, we present **the design space, key architectural choices, and how the pieces must fit together** to minimize technical debt while maximizing learning flexibility.

**Current Reality**: ~21k LOC, excellent infrastructure (memory, I/O, tree processing), but tightly coupled to halo physics with hardcoded properties.

**Transformation Goal**: Physics-agnostic core with runtime-configurable modules and metadata-driven properties.

**Strategic Approach**: Four architectural layers that must work together, each with multiple viable implementation paths.

---

## 1. Current State Assessment

### 1.1 Architectural Strengths (Preserve These)

| Component | Status | Quality | Action |
|-----------|--------|---------|--------|
| **Three-tier data model** | RawHalo → Halo → HaloOutput | ✅ Excellent | Preserve pattern |
| **Memory management** | Categorized tracking, scoped allocation | ✅ Excellent | Keep unchanged |
| **Tree processing** | Recursive traversal, proper inheritance | ✅ Solid | Core algorithm intact |
| **I/O abstraction** | Format-agnostic interface pattern | ✅ Good | Extend, don't replace |
| **Error handling** | Comprehensive logging and validation | ✅ Professional | Keep as-is |
| **Code quality** | Well-documented, consistent style | ✅ High | Maintain standards |

**Assessment**: The infrastructure is production-quality. Transform carefully to avoid breaking what works.

### 1.2 Critical Architecture Gaps

| Gap | Vision Principle Violated | Consequence | Urgency |
|-----|---------------------------|-------------|---------|
| **No module system** | #1 (physics-agnostic), #2 (runtime modularity) | Cannot add galaxy physics without massive core changes | 🔴 CRITICAL |
| **Hardcoded properties** | #3 (metadata-driven), #4 (single source of truth) | Every property addition touches 5+ files | 🔴 CRITICAL |
| **Core ↔ physics coupling** | #1 (physics-agnostic) | `build_model.c` directly calls `get_virial_mass()` | 🔴 CRITICAL |
| **Manual synchronization** | #4 (single source of truth) | Halo ↔ HaloOutput must be kept in sync by hand | 🟡 HIGH |
| **No runtime configuration** | #2 (runtime modularity) | Changing physics requires recompilation | 🟡 MEDIUM |

**Interpretation**: Three critical areas must be addressed: **module system**, **property abstraction**, and **core-physics decoupling**. These are interdependent.

---

## 2. Vision Principles - Current Compliance

```
Legend: ✅ Compliant  ⚠️ Partial  ❌ Non-compliant

Principle 1: Physics-Agnostic Core          ❌  Core calls physics functions directly
Principle 2: Runtime Modularity             ❌  Everything compiled in, no runtime config
Principle 3: Metadata-Driven Architecture   ❌  Properties hardcoded in C structs
Principle 4: Single Source of Truth         ⚠️  Multiple property representations exist
Principle 5: Unified Processing Model       ✅  One tree traversal algorithm works well
Principle 6: Memory Efficiency              ✅  Scoped allocation, leak detection excellent
Principle 7: Format-Agnostic I/O            ✅  Binary and HDF5 via unified interface
Principle 8: Type Safety and Validation     ⚠️  Manual struct access, limited validation
```

**Gap Analysis**: Infrastructure principles (5-7) are satisfied. The architecture principles (1-4) require transformation. Priority: **1 → 3 → 2** (module system, then property system, then runtime config).

---

## 3. Transformation Architecture - Four Interconnected Layers

The transformation requires building four architectural layers that must interoperate cleanly:

```
┌─────────────────────────────────────────────────────────────────┐
│                     LAYER 4: Runtime Configuration               │
│  Enables/disables modules at runtime, resolves dependencies      │
│  Design Choice: .par extension vs JSON vs YAML                   │
└─────────────────────────────────────────────────────────────────┘
                                 ↓
┌─────────────────────────────────────────────────────────────────┐
│                     LAYER 3: Metadata System                     │
│  Properties defined in metadata, code generated at build time    │
│  Design Choice: YAML vs JSON, Python vs tool-specific generator  │
└─────────────────────────────────────────────────────────────────┘
                                 ↓
┌─────────────────────────────────────────────────────────────────┐
│                     LAYER 2: Property Abstraction                │
│  Unified property access, single source of truth                 │
│  Design Choice: Macros vs inline functions vs vtable             │
└─────────────────────────────────────────────────────────────────┘
                                 ↓
┌─────────────────────────────────────────────────────────────────┐
│                     LAYER 1: Module System                       │
│  Physics decoupled from core, module lifecycle management        │
│  Design Choice: Static vs dynamic loading, callback architecture │
└─────────────────────────────────────────────────────────────────┘
                                 ↓
┌─────────────────────────────────────────────────────────────────┐
│                     EXISTING CORE (Unchanged)                    │
│  Memory management, tree processing, I/O - these stay solid      │
└─────────────────────────────────────────────────────────────────┘
```

**Critical Integration Points**:
- Layer 1 ↔ Layer 2: Modules must use property abstraction for all data access
- Layer 2 ↔ Layer 3: Property accessors must be generatable from metadata
- Layer 3 ↔ Layer 4: Metadata must inform what's configurable at runtime
- All layers must preserve the existing core's quality and performance

---

## 4. Layer 1: Module System Architecture

### 4.1 The Problem

**Current**: `build_model.c` has explicit physics calls:
```c
init_halo(ngal, halonr);                    // Direct call
FoFWorkspace[ngal].Mvir = get_virial_mass(halonr);  // Direct call
```

**Consequence**: Adding galaxy physics means editing core files, creating spaghetti dependencies.

### 4.2 Design Space - Module Registration

**Option A: Static Registration with Auto-Discovery**
- Modules self-register using compiler attributes (`__attribute__((constructor))`)
- Core discovers modules automatically at startup
- Zero configuration overhead
- **Pros**: Simple, automatic, no manual lists
- **Cons**: Registration order undefined (needs dependency system)
- **Best for**: Initial implementation, enables rapid module addition

**Option B: Explicit Module Table**
- Central registry file lists all modules
- Modules registered by adding to table
- Clear, obvious, explicit
- **Pros**: Guaranteed order, easy debugging, visible structure
- **Cons**: Manual maintenance, violates open-closed principle
- **Best for**: If auto-registration proves problematic

**Option C: Dynamic Loading (dlopen/dlsym)**
- Modules compiled as shared libraries
- Loaded at runtime from .so files
- True plugin architecture
- **Pros**: Can distribute modules separately, hot-swapping potential
- **Cons**: Platform-specific, complex debugging, unnecessary for science use case
- **Best for**: DEFER - not needed for initial vision compliance

**Recommendation**: Start with **Option A** (auto-registration). It's the sweet spot: automatic like Option C but simple like Option B. Provides flexibility to evolve.

### 4.3 Design Space - Module Interface

**The Core Question**: What does a module need to do?

**Minimal Interface** (start here):
```
Module {
  - name, version
  - init() callback
  - cleanup() callback
  - process_halo() callback  ← THE KEY ABSTRACTION
  - dependencies list
}
```

**Execution Model**:
```
For each halo group:
  join_progenitors()     [CORE - unchanged]
  ↓
  execute_modules()      [NEW - calls each module]
    → module_1.process_halo()
    → module_2.process_halo()
    → ...
  ↓
  update_properties()    [CORE - unchanged]
```

**Key Insight**: The `process_halo()` callback receives a halo array and modifies it. This is the abstraction boundary between core and physics.

**Design Alternatives**:

1. **Single-phase execution** (simple)
   - All modules execute in one pass
   - Order determined by dependency resolution
   - Sufficient for many physics models

2. **Multi-phase execution** (extensible)
   - Modules can register for multiple phases (pre-evolve, evolve, post-evolve)
   - Allows complex interaction patterns
   - Needed for some advanced physics (e.g., iterative cooling+SF)
   - More complex to implement

3. **Event-driven execution** (most flexible)
   - Core emits events (halo_created, halo_merged, etc.)
   - Modules subscribe to events
   - Maximum flexibility
   - Significant complexity overhead

**Recommendation**: Start with **single-phase** (simplest), design for easy evolution to multi-phase. Event-driven is overkill for scientific computing.

### 4.4 Integration with Core

**Before** (core knows physics):
```c
void process_halo_evolution(...) {
  // Physics directly in core
  init_halo(...);
  calculate_virial_properties(...);
  update_halo_properties(...);
}
```

**After** (core agnostic):
```c
void process_halo_evolution(...) {
  // Execute module pipeline
  execute_module_pipeline(halonr, FoFWorkspace, ngal);

  // Core bookkeeping only
  update_halo_properties(...);
}
```

**Validation Requirement**: Must produce bit-identical output with halo_properties as a module vs. direct calls.

---

## 5. Layer 2: Property Abstraction Architecture

### 5.1 The Problem

**Current**: Direct struct access everywhere:
```c
FoFWorkspace[p].Mvir = 1.5;
if (FoFWorkspace[p].Type == 0) { ... }
```

**Consequences**:
- Cannot track property modifications
- Cannot validate property access
- Cannot generate property access code
- Tightly couples code to struct layout

### 5.2 Design Space - Access Abstraction

**Option A: Macro-Based Accessors** (zero-cost abstraction)
```c
GET_HALO_PROP(halo, Mvir)        // Expands to: halo.Mvir
SET_HALO_PROP(halo, Mvir, 1.5)   // Expands to: halo.Mvir = 1.5
```
- **Pros**: Zero runtime cost, compile-time evaluation, easy migration
- **Cons**: Harder debugging, limited compile-time validation
- **Performance**: Perfect (macros are free)
- **Best for**: Initial abstraction layer, performance-critical code

**Option B: Inline Function Accessors** (typed abstraction)
```c
float get_halo_mvir(struct Halo *h) { return h->Mvir; }
void set_halo_mvir(struct Halo *h, float v) { h->Mvir = v; }
```
- **Pros**: Type safety, easier debugging, can add validation
- **Cons**: Requires optimization flags, more verbose code generation
- **Performance**: Excellent (inlined by compiler with -O2)
- **Best for**: After macro validation, when type safety is prioritized

**Option C: Virtual Property Table** (runtime flexibility)
```c
get_property(halo, "Mvir")       // Runtime lookup
set_property(halo, "Mvir", 1.5)  // Runtime assignment
```
- **Pros**: Maximum flexibility, runtime property addition
- **Cons**: **Performance killer** (hashtable lookup per access)
- **Performance**: POOR (10-100x slower)
- **Best for**: Tools and analysis code, NOT for core simulation loop

**Recommendation**: Start with **Option A** (macros), transition to **Option B** (inline functions) once validated. Option C is unacceptable for HPC simulation code.

### 5.3 Property Metadata Structure

**The Core Question**: What information do we need about each property?

**Minimal Metadata**:
```
Property {
  - name: "Mvir"
  - type: float
  - offset: offsetof(struct Halo, Mvir)  [for vtable if needed]
  - size: sizeof(float)
}
```

**Extended Metadata** (for full vision):
```
Property {
  - name, type, offset, size  [as above]
  - units: "10^10 Msun/h"
  - description: "Virial mass"
  - output: true/false  [write to output files?]
  - valid_range: [min, max]  [for validation]
  - category: "halo_properties" | "galaxy_properties"
  - created_by: "halo_properties_module"  [provenance]
}
```

**Design Decision**: Start with minimal, extend as needs become clear. Premature metadata adds complexity without benefit.

### 5.4 Property-Driven Output

**Current Problem**: HDF5 output has 150+ lines of manual property mapping:
```c
HDF5_dst_offsets[0] = HOFFSET(struct HaloOutput, Mvir);
HDF5_dst_sizes[0] = sizeof(galout.Mvir);
HDF5_field_names[0] = "Mvir";
HDF5_field_types[0] = H5T_NATIVE_FLOAT;
// ... repeated 24 times!
```

**Solution**: Generate from metadata:
```c
for each property in metadata:
  if property.output:
    add_to_hdf5_table(property)
```

**Benefit**: Adding property requires metadata update only, output adapts automatically. Reduces ~100 lines of repetitive code.

---

## 6. Layer 3: Metadata-Driven Architecture

### 6.1 The Vision

**Single Source of Truth**: Properties defined once, in metadata format, code generated automatically.

```
                    ┌─────────────────┐
                    │  properties.yaml│  ← SINGLE SOURCE OF TRUTH
                    └────────┬────────┘
                             │
                    [Code Generator]
                             │
              ┌──────────────┼──────────────┐
              ↓              ↓              ↓
      ┌──────────────┐ ┌──────────┐ ┌──────────┐
      │  structs.h   │ │accessor.h│ │metadata.c│
      │              │ │          │ │          │
      │struct Halo { │ │GET_HALO..│ │prop_meta │
      │  int Type;   │ │SET_HALO..│ │  table[] │
      │  float Mvir; │ │          │ │          │
      │  ...         │ │          │ │          │
      └──────────────┘ └──────────┘ └──────────┘
              │              │              │
              └──────────────┴──────────────┘
                             │
                    [Compilation & Build]
                             │
                         ┌───┴───┐
                         │ mimic │
                         └───────┘
```

### 6.2 Design Space - Metadata Format

**Option A: YAML**
```yaml
properties:
  - name: Mvir
    type: float
    units: "10^10 Msun/h"
    description: "Virial mass"
    output: true
```
- **Pros**: Human-readable, standard, supports comments, version-controllable
- **Cons**: Requires PyYAML or libyaml
- **Best for**: Complex metadata with documentation

**Option B: JSON**
```json
{
  "properties": [
    {
      "name": "Mvir",
      "type": "float",
      "units": "10^10 Msun/h",
      "output": true
    }
  ]
}
```
- **Pros**: More universal, Python stdlib (no deps), stricter parsing
- **Cons**: Less readable, no comments, verbose
- **Best for**: If YAML proves problematic

**Option C: Embedded C DSL**
```c
DEFINE_PROPERTY(float, Mvir, "10^10 Msun/h", "Virial mass")
DEFINE_PROPERTY(int, Type, "-", "Halo type")
```
- **Pros**: No external files, stays in C
- **Cons**: Hard to parse, C preprocessor not designed for this, no tooling
- **Best for**: NOT RECOMMENDED (too clever, unmaintainable)

**Recommendation**: **YAML** for metadata. It's the standard for configuration in modern scientific software. Python script for generation (already a dependency for plotting).

### 6.3 Design Space - Code Generation

**The Question**: What generates the C code?

**Option A: Python Script**
- Read YAML with PyYAML
- Generate C code with string templates or Jinja2
- Integrate with build system (Makefile or CMake)
- **Pros**: Standard, flexible, easy to debug, can generate docs too
- **Cons**: Python dependency (but already exists)
- **Best for**: Recommended approach

**Option B: Custom C Tool**
- Parse metadata, generate code in C
- **Pros**: No dependencies beyond C compiler
- **Cons**: Writing a parser is non-trivial, harder to maintain
- **Best for**: If Python is truly unavailable

**Option C: CMake Generation**
- Use CMake's string manipulation to generate code
- **Pros**: Build system native
- **Cons**: CMake string manipulation is painful, hard to debug
- **Best for**: NOT RECOMMENDED (wrong tool for the job)

**Recommendation**: **Python script** (Option A). It's the right tool for this job. ~500 lines of clear Python vs. complex CMake or custom C parser.

### 6.4 Build Integration

**The Flow**:
```
1. Developer edits properties.yaml
2. Build system detects change
3. Runs generate_properties.py
4. Produces .h/.c files in src/include/generated/
5. Compilation includes generated headers
6. Success
```

**Key Design Decision**: Generated files should be **committed to git** for these reasons:
- Builds work without Python installed
- Diffs show what changed (code review visibility)
- Fallback if generation fails
- No surprise changes in CI

**Alternative**: Regenerate every build (not recommended - slows builds unnecessarily)

---

## 7. Layer 4: Runtime Configuration Architecture

### 7.1 The Goal

**Vision**: Change physics without recompilation.

**Before**:
```bash
# To change physics:
1. Edit source code
2. Recompile (1-2 minutes)
3. Re-run
```

**After**:
```bash
# To change physics:
1. Edit config file: EnabledModules = cooling,star_formation
2. Re-run (0 seconds recompile)
```

### 7.2 Design Space - Configuration Format

**Option A: Extend Existing .par Format** (minimal disruption)
```
%------------------------------------------
%----- PHYSICS MODULES -------------------
%------------------------------------------
EnabledModules    halo_properties,cooling

# Module-specific parameters
Cooling_Model     sutherland_dopita_1993
```
- **Pros**: Minimal change, users understand format, leverages existing parser
- **Cons**: .par format is flat, limited structure for complex config
- **Best for**: Initial implementation (Phase 4)

**Option B: JSON Configuration** (structured)
```json
{
  "simulation": { "box_size": 62.5, ... },
  "modules": {
    "enabled": ["halo_properties", "cooling"],
    "cooling": { "model": "sutherland_dopita_1993" }
  }
}
```
- **Pros**: Hierarchical, standard, validation via JSON Schema
- **Cons**: Breaking change for users, requires JSON-C library
- **Best for**: When galaxy physics adds complex parameters (Phase 5+)

**Option C: YAML Configuration** (consistency with metadata)
```yaml
simulation:
  box_size: 62.5
modules:
  enabled: [halo_properties, cooling]
  cooling:
    model: sutherland_dopita_1993
```
- **Pros**: Consistent with property metadata, most readable
- **Cons**: Requires libyaml in C runtime, more complex than JSON
- **Best for**: If YAML metadata proves highly successful

**Recommendation**: Start with **Option A** (extend .par) for minimal disruption. Migrate to **Option B** (JSON) when galaxy physics requires complex configuration.

### 7.3 Module Discovery & Selection

**The Pattern**:
```
1. All modules register at compile-time (always compiled in)
2. Configuration file lists enabled modules
3. At startup:
   - Read config
   - Enable configured modules
   - Disable others (compiled but not executed)
   - Resolve dependencies
   - Initialize enabled modules only
```

**Why Not Dynamic Loading?** (for now)
- Adds platform-specific complexity (dlopen, Windows DLL issues)
- Debugging becomes harder
- Not needed for scientific use case (users aren't distributing module binaries)
- Can be added later if proven necessary

**Static Compilation + Runtime Enable/Disable** is the sweet spot: simple implementation, no recompilation needed, all modules available.

### 7.4 Dependency Resolution

**The Challenge**: Modules have dependencies (e.g., cooling needs halo_properties).

**Simple Solution** (topological sort):
```
1. Modules declare dependencies in their metadata
2. At init time, build dependency graph
3. Topological sort to determine execution order
4. Detect cycles, fail if found
5. Execute modules in resolved order
```

**Example**:
```
Config says: EnabledModules = cooling,star_formation
cooling depends on: halo_properties
star_formation depends on: cooling

Dependency resolution:
  → Automatically enable halo_properties (transitive dependency)
  → Execution order: halo_properties → cooling → star_formation
```

**Design Decision**: Simple is sufficient. More complex dependency systems (semantic versioning, optional dependencies) can be added later if needed. Start with basic topological sort.

---

## 8. Integration Strategy - How The Layers Fit Together

### 8.1 The Critical Integration Points

```
┌──────────────────────────────────────────────────────────────┐
│                    Runtime Configuration                      │
│  Reads: EnabledModules                                       │
│  Outputs: Module enable/disable flags                        │
└────────────────────┬─────────────────────────────────────────┘
                     │ which modules to run?
                     ↓
┌──────────────────────────────────────────────────────────────┐
│                      Module System                            │
│  Executes: execute_module_pipeline()                         │
│  Calls: module->process_halo(halos)                          │
└────────────────────┬─────────────────────────────────────────┘
                     │ how to access halo data?
                     ↓
┌──────────────────────────────────────────────────────────────┐
│                   Property Abstraction                        │
│  Provides: GET_HALO_PROP(halo, Mvir)                         │
│  Uses: Generated accessors from metadata                     │
└────────────────────┬─────────────────────────────────────────┘
                     │ where do accessors come from?
                     ↓
┌──────────────────────────────────────────────────────────────┐
│                    Metadata System                            │
│  Defines: properties.yaml                                    │
│  Generates: struct definitions, accessors, metadata table    │
└──────────────────────────────────────────────────────────────┘
```

### 8.2 Data Flow Through Layers

**Compile Time**:
1. Developer edits `properties.yaml`
2. Build system runs `generate_properties.py`
3. Generated: `property_defs.h`, `property_accessors.h`, `property_metadata.c`
4. Modules include generated headers
5. Core includes generated headers
6. Compilation produces `mimic` executable with all modules compiled in

**Runtime**:
1. Read parameter file (including `EnabledModules`)
2. Auto-register all compiled modules
3. Enable only configured modules
4. Resolve dependencies, order modules
5. Initialize enabled modules (call `init()`)
6. **Main loop**:
   - Load tree → RawHalo[]
   - For each halo:
     - `join_progenitors()` (core logic)
     - `execute_module_pipeline()`:
       - For each enabled module:
         - Call `module->process_halo(halos)`
         - Module uses `GET_HALO_PROP/SET_HALO_PROP` macros
         - Macros expand to direct struct access (zero overhead)
     - `update_properties()` (core logic)
   - `save_halos()` (uses property metadata for output)
7. Cleanup enabled modules (call `cleanup()`)

### 8.3 Validation Strategy

**The Non-Negotiable Requirement**: Output must be bit-identical until galaxy physics is added.

**Testing Approach**:
```bash
# Baseline (before any transformation)
./mimic input/millennium.par
md5sum output/results/millennium/model_z0.000_0 > baseline.md5

# After adding Layer 1 (module system)
[rebuild]
./mimic input/millennium.par
md5sum output/results/millennium/model_z0.000_0 > layer1.md5
diff baseline.md5 layer1.md5  # MUST be identical

# After adding Layer 2 (property abstraction)
[rebuild]
[repeat test]  # MUST be identical

# After adding Layer 3 (metadata)
[rebuild]
[repeat test]  # MUST be identical

# After adding Layer 4 (runtime config)
[rebuild with EnabledModules=halo_properties]
[repeat test]  # MUST be identical
```

**If output changes**: Transformation has a bug, must be fixed before proceeding.

---

## 9. Phased Implementation Strategy

### 9.1 The Four Phases

**Phase 1: Module System Foundation** (2-3 weeks)
- **Goal**: Decouple core from physics
- **Deliverable**: Core executes module pipeline instead of direct calls
- **Validation**: Bit-identical output with halo_properties as module
- **Blocks**: All future galaxy physics development

**Phase 2: Property Abstraction** (2-3 weeks)
- **Goal**: Abstract property access, prepare for metadata
- **Deliverable**: All property access via GET/SET macros
- **Validation**: Bit-identical output using macros
- **Blocks**: Metadata-driven architecture

**Phase 3: Metadata-Driven Properties** (3-4 weeks)
- **Goal**: Properties defined in YAML, code generated
- **Deliverable**: Property structs and accessors generated at build time
- **Validation**: Bit-identical output from generated code
- **Blocks**: Rapid property addition, reduced maintenance

**Phase 4: Runtime Configuration** (2 weeks)
- **Goal**: Module selection via config file
- **Deliverable**: Change physics without recompilation
- **Validation**: Same output when enabling same modules
- **Blocks**: Scientific flexibility for different physics combinations

**Total Duration**: 9-12 weeks for vision compliance

### 9.2 Parallelization Opportunities

**Can Be Developed In Parallel**:
- Property system design (Phase 2) can begin during Phase 1 implementation
- Metadata format design (Phase 3) can begin during Phase 2 implementation
- Configuration format design (Phase 4) can begin early

**Must Be Sequential**:
- Cannot implement Phase 3 without Phase 2 abstraction in place
- Cannot implement Phase 4 without Phase 1 module system

**With 2 Developers**:
- Developer A: Module system (Phase 1) → Runtime config (Phase 4)
- Developer B: Property abstraction (Phase 2) → Metadata system (Phase 3)
- Estimated: 6-8 weeks with parallel work

### 9.3 Rollback Strategy

**Each phase must be independently reversible**:
- Develop on feature branches
- Merge only after validation passes
- Keep old code until new code is proven
- Can revert to previous phase if problems arise

**Example Rollback Scenarios**:
- Phase 1 causes performance regression → revert, optimize, retry
- Phase 3 code generation has bugs → fall back to hand-written code
- Phase 4 config system is too complex → continue with compile-time selection

---

## 10. Risk Assessment and Mitigation

### 10.1 Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Performance regression** | Medium | High | Benchmark every phase, use zero-overhead abstractions (macros/inline) |
| **Output inconsistency** | Low | Critical | Automated MD5 validation, bit-identical requirement |
| **Complexity explosion** | Medium | High | Start simple, add features only when proven necessary |
| **Code generation bugs** | Medium | Medium | Keep hand-written code until generated is validated, extensive testing |
| **Module interaction bugs** | Low | Medium | Explicit dependency declarations, clear module boundaries |

### 10.2 Strategic Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Over-engineering** | Medium | Medium | Implement minimal viable version first, extend based on experience |
| **Under-engineering** | Low | High | Design for extensibility (but don't implement until needed) |
| **Premature optimization** | Low | Medium | Measure performance, optimize only proven bottlenecks |
| **Technical debt accumulation** | Low | High | Code reviews, maintain documentation, refactor before adding |

**Key Insight**: The biggest risk is **building more than needed**. Start minimal, learn from usage, extend based on real requirements.

---

## 11. Success Metrics

### 11.1 Technical Success Criteria

**After Phase 1** (Module System):
- [ ] Core has zero direct physics calls
- [ ] Module pipeline executes 0, 1, or N modules successfully
- [ ] Output is bit-identical to pre-transformation
- [ ] Can add new module without touching core code

**After Phase 2** (Property Abstraction):
- [ ] All property access via abstraction layer
- [ ] Property metadata covers all properties
- [ ] Output generation uses metadata (not hardcoded)
- [ ] Output is bit-identical to pre-transformation

**After Phase 3** (Metadata-Driven):
- [ ] Properties defined in YAML
- [ ] Code generation runs automatically in build
- [ ] Adding property requires only YAML edit
- [ ] Output is bit-identical to pre-transformation

**After Phase 4** (Runtime Config):
- [ ] Modules enabled/disabled via config file
- [ ] No recompilation needed to change physics
- [ ] Dependency resolution works correctly
- [ ] Output is bit-identical when same modules enabled

### 11.2 Architecture Quality Metrics

**Vision Compliance** (8 Principles):
- Physics-Agnostic Core: ❌ → ✅
- Runtime Modularity: ❌ → ✅
- Metadata-Driven: ❌ → ✅
- Single Source of Truth: ⚠️ → ✅
- Unified Processing: ✅ → ✅ (maintained)
- Memory Efficiency: ✅ → ✅ (maintained)
- Format-Agnostic I/O: ✅ → ✅ (maintained)
- Type Safety: ⚠️ → ✅

**Code Quality**:
- Maintenance burden: Reduced ~30% (single source of truth)
- Lines of code: +1.5k (~7% increase for +4 architectural capabilities)
- Time to add module: 2 days → 2 hours
- Time to add property: 1 day → 15 minutes
- Time to change physics: 2 minutes (recompile) → 30 seconds (config edit)

---

## 12. Post-Vision Extensions (Phase 5+)

**Once Phases 1-4 are complete, the architecture enables**:

### 12.1 Galaxy Physics Modules
With the module system in place, adding galaxy physics becomes straightforward:
- **Cooling modules**: Gas cooling prescriptions (multiple models selectable)
- **Star formation**: Schmidt law, Kennicutt relations
- **Feedback**: Stellar feedback, AGN feedback
- **Mergers**: Major/minor mergers, tidal stripping
- **And many more...**

Each module is self-contained, registers itself, declares dependencies, and executes via the pipeline.

### 12.2 Advanced Property Features
- **Computed properties**: Properties calculated from others (e.g., specific SFR)
- **Property provenance**: Track which module last modified each property
- **Property validation**: Runtime range checking based on metadata
- **Property units**: Automatic unit conversion in output

### 12.3 Performance Optimization
- **Module parallelization**: Execute independent modules in parallel (OpenMP)
- **Property lazy evaluation**: Compute properties only when accessed
- **Output compression**: Property metadata enables smart compression

### 12.4 Scientific Flexibility
- **Physics model comparison**: A/B testing of different physics prescriptions
- **Parameter exploration**: Grid search over module parameter space
- **Module versioning**: Track which module versions produced results

---

## 13. Recommendations and Conclusions

### 13.1 Implementation Recommendations

**1. Start with Phase 1 (Module System) Immediately** ⭐⭐⭐
- This is the critical path blocker for all galaxy physics
- Relatively low risk (core algorithms unchanged)
- High value (enables everything else)
- **Action**: Create feature branch this week

**2. Use Conservative Implementation Choices**
- Static registration (not dynamic loading) - simpler, sufficient
- Macros (not functions) initially - zero overhead, validates pattern
- Extend .par (not JSON) initially - minimal user disruption
- **Rationale**: Start simple, prove patterns, extend when needed

**3. Maintain Bit-Identical Output Requirement**
- Every phase must produce identical output
- Automated validation in CI/CD
- This is non-negotiable proof of correctness
- **Rationale**: Proves transformation doesn't break science

**4. Build Minimal Viable Versions First**
- Single-phase module execution (not multi-phase or event-driven)
- Basic property metadata (not full provenance tracking)
- Simple dependency resolution (not semantic versioning)
- **Rationale**: Learn from usage, extend based on real needs, avoid over-engineering

**5. Design For Evolution, Not Perfection**
- Abstractions can be enhanced later
- Don't implement features before they're needed
- Performance optimization only when measured
- **Rationale**: Premature optimization is the root of evil; premature feature addition is close behind

### 13.2 Critical Success Factors

**Technical**:
- Zero-overhead abstractions (maintain HPC performance)
- Automated validation (bit-identical output requirement)
- Clean abstraction boundaries (module ↔ property ↔ metadata)

**Process**:
- Phased rollout with validation between phases
- Feature branch development with integration testing
- Code review for architectural consistency
- Documentation as you build

**Strategic**:
- Start minimal, extend based on experience
- Prioritize unblocking galaxy physics development
- Maintain existing code quality and performance
- Minimize technical debt through single source of truth

### 13.3 The Path Forward

**Months 1-2**: Phase 1 (Module System)
- Highest priority, unblocks everything
- Proves the abstraction pattern works
- Low risk, high value

**Months 2-3**: Phase 2 (Property Abstraction)
- Prepares for metadata-driven architecture
- Validates zero-overhead pattern

**Months 3-4**: Phase 3 (Metadata System)
- Highest maintenance burden reduction
- Single source of truth achieved
- Enables rapid property addition

**Month 5**: Phase 4 (Runtime Configuration)
- Completes vision compliance
- Enables scientific flexibility
- Final piece of the puzzle

**Month 6+**: Galaxy Physics Development
- Add cooling, star formation, feedback modules
- Use the architecture we've built
- Rapid development cycle (hours to add module, not days)

### 13.4 Final Assessment

**Current State**: Excellent infrastructure, but architecturally constrained for galaxy physics.

**Transformation Feasibility**: **HIGH**
- Clear transformation path with four phases
- Each phase independently validated
- Conservative implementation choices minimize risk
- Existing code quality makes transformation safer

**Expected Outcome**: A physics-agnostic framework that:
- ✅ Enables rapid galaxy physics development
- ✅ Maintains performance (HPC-grade code)
- ✅ Reduces maintenance burden by ~30%
- ✅ Provides scientific flexibility (runtime configuration)
- ✅ Complies with all 8 vision principles

**Risk Level**: **LOW-MEDIUM**
- Phased approach with validation reduces risk
- Performance maintained via zero-overhead design
- Rollback options at every phase
- Core algorithms untouched (tree processing, memory, I/O remain excellent)

**Recommendation**: **PROCEED** with confidence. The transformation is well-conceived, technically sound, and achieves the vision while minimizing technical debt. The architecture will be robust, flexible, and maintainable.

The key is to **start minimal, validate aggressively, and extend based on real usage**. This avoids over-engineering while building a solid foundation for Mimic's scientific future.

---

**Document End**

This roadmap presents the strategic landscape for Mimic's architectural transformation. The specifics of implementation are left to the development team, but the design space, integration points, and success criteria are clearly defined. The path forward prioritizes minimal technical debt while maximizing flexibility for learning and evolution.
