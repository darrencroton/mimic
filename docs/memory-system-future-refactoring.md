# Memory Management System: Future Refactoring Recommendations

**Document Version:** 1.0
**Date:** November 2025
**Status:** Deferred pending Mimic context architecture design
**Author:** Architectural review and planning

---

## Executive Summary

The current memory management system (`util_memory.c`, 673 lines) provides category-based tracking valuable for Mimic's future modular architecture, but implements it using unnecessarily complex block-by-block tables. This document recommends **simplifying the implementation while preserving the category tracking capability**, which will be essential when physics modules are added.

**Key Recommendation:** Defer refactoring until Mimic's context architecture is designed, then integrate memory tracking with per-context scoping as envisioned in the architectural vision document.

---

## Current State Analysis

### What Exists Today

The memory system tracks allocations using three parallel arrays:

```c
// From util_memory.c
static void *(*Table) = NULL;                 // Pointers to allocated blocks
static size_t(*SizeTable) = NULL;             // Sizes of allocated blocks
static MemoryCategory(*CategoryTable) = NULL; // Category of each block
static unsigned long MaxBlocks = DEFAULT_MAX_MEMORY_BLOCKS; // Limit: 1024
static unsigned long Nblocks = 0;             // Current block count
```

**Memory overhead:**
- Table: 1024 × 8 bytes = 8 KB
- SizeTable: 1024 × 8 bytes = 8 KB
- CategoryTable: 1024 × 4 bytes = 4 KB
- **Total: 20 KB + O(N) lookup complexity**

**Categories tracked:**
```c
typedef enum {
  MEM_UNKNOWN = 0,
  MEM_GALAXIES,    // Legacy, will become MEM_HALOS
  MEM_HALOS,       // Dark matter halo data
  MEM_TREES,       // Merger tree structures
  MEM_IO,          // I/O buffers
  MEM_UTILITY,     // Utility arrays
  MEM_MAX_CATEGORY
} MemoryCategory;
```

**Key functions:**
- `mymalloc_cat(size, category)` - Allocate with category
- `mymalloc(size)` - Allocate as MEM_UNKNOWN
- `myrealloc()` - Reallocate (finds block in table)
- `myfree()` - Free (finds block in table, removes from tracking)
- `print_allocated_by_category()` - Reports memory by category

**Current usage:** 54 call sites across 11 files

---

## Why Current Implementation is Problematic

### 1. Block-by-Block Tracking is Overkill

**Problem:** The system maintains a table of every allocation:
```c
// Every mymalloc() call:
Table[Nblocks] = ptr;           // Store pointer
SizeTable[Nblocks] = size;      // Store size
CategoryTable[Nblocks] = cat;   // Store category
Nblocks++;
```

**Why this is excessive:**
- We only need **aggregate** statistics per category
- Block-by-block tracking is for **leak detection**
- Modern tools (Valgrind, AddressSanitizer) do this better
- Adds O(N) lookup overhead in `myfree()` and `myrealloc()`

**Evidence from code:**
```c
// myfree() has to search the table (lines 341-357)
for (i = 0; i < Nblocks; i++) {
  if (Table[i] == ptr) {
    // Found it, now remove from table...
    // Shift all subsequent entries down...
  }
}
```

This is **O(N) deallocation** in 2025!

### 2. Artificial Limits

**Problem:** `MaxBlocks = 1024` is arbitrary
- Large simulations might need more
- Small simulations waste memory
- Requires tuning by users

**From util_memory.c:39:**
```c
static unsigned long MaxBlocks = DEFAULT_MAX_MEMORY_BLOCKS; // 1024
```

If allocation #1025 happens, system must reallocate tracking arrays or fail.

### 3. Complexity Without Benefit

**What we actually need:**
- Total memory usage (TotMem) ✅ Have it
- Peak memory usage (HighMarkMem) ✅ Have it
- Memory by category (CategorySizes[]) ✅ Have it

**What we don't need:**
- List of every pointer ❌ Not used productively
- O(N) lookups in free/realloc ❌ Performance cost
- Arbitrary block limits ❌ Artificial constraint

---

## Why Category Tracking is ESSENTIAL for Mimic

### Connection to Architectural Vision

**From mimic-architecture-vision.md, Principle 6:**
> "Memory should be allocated on a per-forest scope, with guaranteed cleanup after each forest is processed."

**From Principle 1 (Physics-Agnostic Core):**
> "Physics modules interact with the core only through well-defined interfaces."

### Future Physics Modules Will Need Tracking

When Mimic adds physics modules, you'll need answers like:
- "Which module is consuming the most memory?"
- "Is cooling module leaking memory across forests?"
- "How much memory overhead do I add by enabling star formation?"

**Future categories will include:**
```c
typedef enum {
  MEM_UNKNOWN = 0,
  MEM_CORE_HALOS,        // Core halo tracking
  MEM_CORE_TREES,        // Core tree structures
  MEM_CORE_IO,           // Core I/O operations

  // Physics module categories
  MEM_MODULE_COOLING,    // Cooling module data
  MEM_MODULE_SF,         // Star formation module data
  MEM_MODULE_FEEDBACK,   // Feedback module data
  MEM_MODULE_MERGERS,    // Merger module data
  MEM_MODULE_DISK,       // Disk instability module data
  MEM_MODULE_CUSTOM,     // User-defined modules

  MEM_MAX_CATEGORY
} MemoryCategory;
```

### Production Diagnostics

**Why not just use Valgrind/AddressSanitizer?**

Development tools like Valgrind and AddressSanitizer are **not suitable for production**:
- 2-20x slowdown (unacceptable for HPC runs)
- Not available on all HPC systems
- Require recompilation with special flags
- Output is developer-focused, not user-friendly

**Production needs:**
```
=== Memory Usage by Category ===
Core Halos:           1.2 GB
Core Trees:           0.8 GB
Cooling Module:       2.1 GB  ← Aha! This is the problem
Star Formation:       0.3 GB
Feedback Module:      0.5 GB
-----------------------------------
Total:                4.9 GB
Peak:                 5.2 GB
```

This is **scientific insight**, not just debugging.

### Per-Context Memory Scoping

**Vision for Mimic:**
```c
struct MimicContext {
    // Configuration
    const struct SageConfig *config;

    // State
    struct TreeProcessingState state;

    // Data arrays
    struct {
        struct Halo *processed_halos;
        struct RawHalo *input_tree_halos;
        // ... allocated per context
    } data;

    // Memory tracking PER CONTEXT
    struct {
        size_t total_allocated;
        size_t peak_allocated;
        size_t category_sizes[MEM_MAX_CATEGORY];
    } memory;
};
```

With per-context tracking:
- Memory is scoped to forest processing
- Cleanup is guaranteed after each forest
- Multiple contexts can run concurrently (threading)
- Each context reports its own memory profile

**This requires custom allocator with category tracking!**

---

## Recommended Future Refactoring

### Overview

**Keep:**
- ✅ Category-based tracking (essential for modules)
- ✅ High water mark tracking (HighMarkMem)
- ✅ Aggregate statistics (TotMem, CategorySizes[])
- ✅ `mymalloc_cat(size, category)` interface
- ✅ `print_allocated_by_category()` for diagnostics

**Remove:**
- ❌ Block-by-block tracking (Table, SizeTable, CategoryTable)
- ❌ O(N) lookup in myfree/myrealloc
- ❌ MaxBlocks limit
- ❌ Complex table management code

**Add:**
- ✅ Per-allocation header (stores size + category)
- ✅ Integration with MimicContext (future)

### Technical Approach

#### Option A: Header-Based Tracking (Recommended)

Store metadata in a header before each allocation:

```c
// Header structure (16 bytes)
struct AllocationHeader {
    size_t size;              // 8 bytes - allocation size
    MemoryCategory category;  // 4 bytes - memory category
    uint32_t magic;          // 4 bytes - integrity check (0xDEADBEEF)
};

void *mymalloc_cat(size_t size, MemoryCategory category) {
    // Allocate size + header
    size_t total_size = size + sizeof(struct AllocationHeader);
    struct AllocationHeader *header = malloc(total_size);

    if (!header) {
        FATAL_ERROR("Memory allocation failed (%zu bytes)", size);
    }

    // Fill header
    header->size = size;
    header->category = category;
    header->magic = 0xDEADBEEF;

    // Update aggregate statistics
    TotMem += size;
    CategorySizes[category] += size;
    if (TotMem > HighMarkMem) {
        HighMarkMem = TotMem;
    }

    // Return pointer after header
    return (void *)(header + 1);
}

void myfree(void *ptr) {
    if (!ptr) return;

    // Get header
    struct AllocationHeader *header =
        ((struct AllocationHeader *)ptr) - 1;

    // Validate magic number
    if (header->magic != 0xDEADBEEF) {
        FATAL_ERROR("Memory corruption detected (invalid magic number)");
    }

    // Update statistics
    TotMem -= header->size;
    CategorySizes[header->category] -= header->size;

    // Free including header
    free(header);
}
```

**Benefits:**
- O(1) deallocation (no table lookup)
- No arbitrary limits
- Minimal overhead (16 bytes per allocation)
- Built-in corruption detection
- Easy integration with contexts

**Trade-offs:**
- 16 bytes per allocation overhead
- For 10,000 allocations: 160 KB overhead
- Compare to current: 20 KB for 1024 allocations, then must grow
- Net: Simpler, more scalable, still minimal overhead

#### Option B: Hash Table Tracking

Use hash table instead of linear array:

```c
// Hash table entry
struct MemoryBlock {
    void *ptr;
    size_t size;
    MemoryCategory category;
    struct MemoryBlock *next;  // For collision chaining
};

// Hash table
#define HASH_TABLE_SIZE 1024
static struct MemoryBlock *HashTable[HASH_TABLE_SIZE];

// Hash function
static inline size_t hash_ptr(void *ptr) {
    return ((size_t)ptr >> 4) % HASH_TABLE_SIZE;
}
```

**Benefits:**
- O(1) average case for lookup
- No per-allocation overhead
- Can still use leak detection tools

**Trade-offs:**
- More complex than headers
- Hash table collisions
- Still need external table (memory overhead)
- Harder to integrate with per-context tracking

**Verdict:** Option A (headers) is simpler and better for Mimic's goals.

---

## Integration with Mimic Context Architecture

### Current Call Pattern
```c
// Today:
struct Halo *halos = mymalloc_cat(n * sizeof(struct Halo), MEM_HALOS);
// ... use halos ...
myfree(halos);
```

### Future Context-Based Pattern

When Mimic contexts are implemented:

```c
// Future:
struct MimicContext *ctx = mimic_context_create(&config);

// Allocate within context
struct Halo *halos = mimic_context_alloc(ctx,
    n * sizeof(struct Halo),
    MEM_HALOS);

// Context tracks its own memory
printf("Context memory: %zu bytes\n", ctx->memory.total_allocated);

// Automatic cleanup on context destruction
mimic_context_destroy(ctx);  // Frees all allocations
```

**Implementation:**
```c
void *mimic_context_alloc(struct MimicContext *ctx,
                          size_t size,
                          MemoryCategory category) {
    // Use header-based tracking
    void *ptr = mymalloc_cat(size, category);

    // Update context-specific tracking
    ctx->memory.total_allocated += size;
    ctx->memory.category_sizes[category] += size;

    if (ctx->memory.total_allocated > ctx->memory.peak_allocated) {
        ctx->memory.peak_allocated = ctx->memory.total_allocated;
    }

    return ptr;
}
```

This provides:
- Per-context memory profiles
- Guaranteed cleanup (destroy context = free all)
- Module-level attribution (which module allocated what)
- Thread-safe when each thread has its own context

---

## Implementation Timeline

### Phase 1: Design Mimic Context Architecture (8-12 weeks)
**Do this first, before refactoring memory system!**

1. Design MimicContext structure
2. Design physics module interface
3. Determine memory ownership model
4. Prototype context lifecycle

### Phase 2: Refactor Memory System (3-4 weeks)
**Only after context design is complete**

1. Implement header-based tracking (1 week)
   - Add AllocationHeader struct
   - Rewrite mymalloc/myfree/myrealloc
   - Remove Table/SizeTable/CategoryTable
   - Keep aggregate statistics

2. Update all call sites (1 week)
   - 54 call sites across 11 files
   - Most are already using mymalloc_cat() ✅
   - Some use mymalloc() (MEM_UNKNOWN) ← fix these

3. Add context integration (1 week)
   - Implement mimic_context_alloc()
   - Add per-context tracking
   - Test context isolation

4. Testing and validation (1 week)
   - Memory leak testing
   - Corruption detection testing
   - Performance comparison
   - Production run validation

---

## Testing Strategy

### Before Refactoring

**Baseline measurements:**
```bash
# Run with current system
./sage input/millennium.par

# Record:
# - Total memory usage
# - Peak memory usage
# - Memory by category
# - Execution time
```

### After Refactoring

**Validation tests:**

1. **Correctness:** Output files identical (byte-for-byte)
2. **Memory:** Total/peak within 5% of baseline
3. **Performance:** Runtime within 5% of baseline
4. **Leak detection:**
   ```bash
   valgrind --leak-check=full ./sage input/millennium.par
   ```
5. **Category tracking:** Reports match expectations

### Stress Testing

**Large simulation:**
```bash
# Test with many allocations
./sage input/large_simulation.par

# Should handle 100,000+ allocations gracefully
# No artificial MaxBlocks limit!
```

**Multi-forest testing:**
```bash
# Verify per-context cleanup
# Each forest should start with TotMem = 0
```

---

## Migration Guide for Developers

### Current Code Patterns

**Pattern 1: Simple allocation**
```c
// Current (keep this)
int *array = mymalloc(n * sizeof(int));
```

**Better:**
```c
// Specify category for better tracking
int *array = mymalloc_cat(n * sizeof(int), MEM_UTILITY);
```

**Pattern 2: Halo allocation**
```c
// Current
ProcessedHalos = mymalloc(MaxProcessedHalos * sizeof(struct Halo));
```

**Better (add category):**
```c
ProcessedHalos = mymalloc_cat(
    MaxProcessedHalos * sizeof(struct Halo),
    MEM_HALOS
);
```

**Pattern 3: Reallocation**
```c
// Current (works with both systems)
FoFWorkspace = myrealloc(FoFWorkspace, new_size * sizeof(struct Halo));
```

**Better (add category in future):**
```c
FoFWorkspace = myrealloc_cat(
    FoFWorkspace,
    new_size * sizeof(struct Halo),
    MEM_HALOS
);
```

### Future: Physics Module Pattern

When adding a physics module:

```c
// Module initialization
void cooling_module_init(struct MimicContext *ctx) {
    // Allocate module-specific data with proper category
    ctx->module_data[MODULE_COOLING] = mimic_context_alloc(
        ctx,
        sizeof(struct CoolingData),
        MEM_MODULE_COOLING  // New category!
    );
}

// Module cleanup is automatic when context is destroyed
```

---

## Specific Code Locations

### Files to Modify

**Core memory system (3 files):**
- `code/util_memory.c` (673 lines) - Main implementation
- `code/util_memory.h` (46 lines) - Public interface
- `code/core_proto.h` (lines with mymalloc declarations)

**Call sites to review (11 files):**
- `code/core_build_model.c` - Halo array management
- `code/core_init.c` - Initialization allocations
- `code/core_read_parameter_file.c` - Parameter arrays
- `code/io_tree.c` - Tree data structures
- `code/io_tree_binary.c` - Binary tree loading
- `code/io_tree_hdf5.c` - HDF5 tree loading
- `code/io_save_hdf5.c` - HDF5 output
- `code/main.c` - Top-level allocations
- `code/util_memory.c` - Internal allocations

### Current Category Usage

**Scan results:** Only 8 uses of category tracking today!
```bash
$ grep -r "MEM_HALOS\|MEM_TREES\|MEM_IO" code/ | wc -l
8
```

**This means:** Most allocations use `mymalloc()` (MEM_UNKNOWN)

**Future task:** Review and categorize the 46 remaining uncategorized calls.

---

## Performance Considerations

### Current System

**Allocation:** O(1) - add to end of array
**Deallocation:** O(N) - linear search + shift array
**Reallocation:** O(N) - linear search + potential realloc of tracking arrays

With 1000 allocations:
- 1000 allocations: 1000 × O(1) = O(1000)
- 1000 deallocations: 1000 × O(N) = O(1000 × 500 avg) = O(500,000)
- **Total: O(500,000) operations**

### Header-Based System

**Allocation:** O(1) - malloc + fill header
**Deallocation:** O(1) - read header + free
**Reallocation:** O(1) - read header + realloc

With 1000 allocations:
- 1000 allocations: O(1000)
- 1000 deallocations: O(1000)
- **Total: O(2000) operations**

**Expected speedup:** ~250x for deallocation-heavy workloads

**Memory overhead:**
- Current: 20 KB (fixed for 1024 blocks)
- Header: 16 bytes × N allocations
- For 10,000 allocations: 160 KB (but no linear search!)

**Net result:** Simpler code + better performance + better scalability

---

## Risk Assessment

### Low Risk Changes
- ✅ Add category to existing mymalloc() calls (54 sites)
- ✅ Remove Table/SizeTable/CategoryTable arrays
- ✅ Switch from O(N) to O(1) lookups

### Medium Risk Changes
- ⚠️ Rewrite myfree/myrealloc with header access
- ⚠️ Add per-allocation headers (memory layout changes)
- ⚠️ Integration with future context system

### High Risk Changes
- ❌ None if done after context architecture is designed

### Mitigation Strategies

**For medium risks:**
1. **Extensive testing:** Valgrind leak checks on all test cases
2. **Gradual migration:** Keep old system, add #ifdef NEW_MEMORY
3. **Validation:** Compare old vs new output byte-for-byte
4. **Monitoring:** Track memory usage in production runs

**Testing checklist:**
- [ ] All existing tests pass
- [ ] No memory leaks (Valgrind clean)
- [ ] No memory corruption (magic number checks)
- [ ] Category tracking accurate
- [ ] Peak memory within 5% of baseline
- [ ] Performance within 5% of baseline

---

## Relationship to Other Systems

### I/O System (io_util.c)

**Current:** Memory system allocates I/O buffers with MEM_IO category
**Future:** After I/O simplification (removing custom buffering), fewer MEM_IO allocations
**Impact:** Reduces memory system load, validation is simpler

### Tree Processing (core_build_model.c)

**Current:** Large dynamic arrays (FoFWorkspace, ProcessedHalos)
**Future:** These become context-owned, per-forest allocations
**Impact:** Memory scoping aligns with forest boundaries

### Physics Modules (future)

**Current:** N/A
**Future:** Each module has its own category (MEM_MODULE_*)
**Impact:** Can profile which modules consume most memory

---

## Conclusion

**DO NOT refactor the memory system yet.** The current system, while complex, provides essential category tracking that will be crucial when physics modules are added. The block-by-block tracking IS overkill and should be simplified, but wait until after:

1. I/O simplification is complete (reduces moving parts)
2. MimicContext architecture is designed (defines memory ownership)
3. Physics module interface is prototyped (defines category requirements)

**When ready to refactor:**
- Use header-based tracking (Option A)
- Keep category tracking and aggregate statistics
- Remove block-by-block tables and O(N) lookups
- Integrate with per-context memory accounting
- Expected effort: 3-4 weeks

**Benefits of waiting:**
- Refactor once, correctly
- Align with context architecture from day one
- Avoid rework when modules are added
- Reduce risk by having I/O system stable first

**The category tracking system is not technical debt - it's architectural foresight for Mimic's modular future.**

---

## References

- **Architectural Vision:** `mimic-architecture-vision.md`
- **Current Implementation:** `code/util_memory.c` (673 lines)
- **Public Interface:** `code/util_memory.h` (46 lines)
- **Call Sites:** 54 locations across 11 files

## Appendix: Example Output

**Current system output:**
```
=== Memory Usage ===
Total allocated: 2.4 GB
Peak allocated: 2.8 GB

By category:
  Unknown:    1.2 GB  ← Should be categorized!
  Halos:      0.8 GB
  Trees:      0.4 GB
  I/O:        0.0 GB
  Utility:    0.0 GB
```

**Future system output:**
```
=== Memory Usage by Module ===
Core Systems:
  Halos:              0.8 GB
  Trees:              0.4 GB
  I/O:                0.1 GB
  Utility:            0.2 GB
  Subtotal:           1.5 GB

Physics Modules:
  Cooling:            0.6 GB
  Star Formation:     0.2 GB
  AGN Feedback:       0.3 GB
  Disk Instability:   0.1 GB
  Subtotal:           1.2 GB

Total:                2.7 GB
Peak:                 3.1 GB

Per-Forest Statistics:
  Forest 1:  2.1 GB peak
  Forest 2:  2.8 GB peak  ← This one!
  Forest 3:  1.9 GB peak
```

This is the diagnostic power you want for Mimic's future.
