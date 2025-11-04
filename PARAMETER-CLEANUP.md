# Parameter Cleanup - 2025-11-04

## Summary
Removed unnecessary function parameters that were identified during the compiler warning cleanup. This improves code clarity and eliminates API bloat from legacy code.

## Motivation
After fixing compiler warnings with `(void)param;` statements, a critical code review was performed to determine if unused parameters should be kept or removed. Since MIMIC is a new codebase (halo-only tracking, no galaxy physics), parameters that were only needed for the full galaxy evolution model were removed.

## Parameters Removed

### 1. Core Build Model Functions (3 parameters removed)

**`update_halo_properties()`** - Removed 2 parameters:
- **centralgal** (int): Legacy parameter from galaxy physics model
  - Was used to identify the central galaxy for physical processes
  - Not needed in halo-only tracking mode
  - File: `src/core/build_model.c:388`

- **deltaT** (double): Legacy parameter for physics timestep
  - Was used for time-dependent physical processes (cooling, star formation, etc.)
  - Not needed when no physics is being calculated
  - File: `src/core/build_model.c:388`

**Before:**
```c
void update_halo_properties(int ngal, int centralgal, double deltaT)
```

**After:**
```c
void update_halo_properties(int ngal)
```

**`process_halo_evolution()`** - Removed 1 parameter:
- **tree** (int): Tree index parameter
  - Never used in function body
  - No clear purpose for this parameter
  - File: `src/core/build_model.c:455`

**Before:**
```c
void process_halo_evolution(int halonr, int ngal, int tree)
```

**After:**
```c
void process_halo_evolution(int halonr, int ngal)
```

### 2. Tree Loading Functions (3 parameters removed)

**`load_tree_binary()`** - Removed 1 parameter:
- **filenr** (int32_t): File number
  - File already opened by `load_tree_table_binary()`
  - File handle stored in static variable `load_fd`
  - No purpose for this parameter
  - File: `src/io/tree/binary.c:137`

**Before:**
```c
void load_tree_binary(int32_t filenr, int32_t treenr)
```

**After:**
```c
void load_tree_binary(int32_t treenr)
```

**`load_tree_hdf5()`** - Removed 1 parameter:
- **filenr** (int32_t): File number
  - File already opened by `load_tree_table_hdf5()`
  - File handle stored in static variable `hdf5_file`
  - No purpose for this parameter
  - File: `src/io/tree/hdf5.c:191`

**Before:**
```c
void load_tree_hdf5(int32_t filenr, int32_t treenr)
```

**After:**
```c
void load_tree_hdf5(int32_t treenr)
```

**`load_tree()`** - Removed 1 parameter (wrapper function):
- **filenr** (int): File number
  - Only passed through to format-specific functions
  - No longer needed after removing from `load_tree_binary()` and `load_tree_hdf5()`
  - File: `src/io/tree/interface.c:220`

**Before:**
```c
void load_tree(int filenr, int treenr, enum Valid_TreeTypes TreeType)
```

**After:**
```c
void load_tree(int treenr, enum Valid_TreeTypes TreeType)
```

## Parameters Kept (Intentionally Unused)

These parameters remain unused but are required for API compatibility:

### 1. Integration Function Parameters
**`integrand_time_to_present()`** - Parameter: `void *param`
- Required by integration function pointer signature
- Standard numerical integration API requires this parameter
- File: `src/core/init.c`

**`integration_qag()`** - Parameters: `limit`, `key`, `workspace`
- Required by standard GSL-compatible numerical integration API
- Maintains compatibility with external integration libraries
- File: `src/util/integration.c:121`

## Design Issues Identified (Not Fixed)

**`read_dataset()` in HDF5 module** - Parameter: `my_hdf5_file`
- Function receives file handle as parameter but uses global `hdf5_file` instead
- Design inconsistency but left unchanged to avoid scope creep
- Should be fixed in future refactoring
- File: `src/io/tree/hdf5.c`

## Files Modified

### Function Definitions:
- `src/core/build_model.c` - Updated 2 function signatures and call sites
- `src/io/tree/binary.c` - Updated 1 function signature
- `src/io/tree/hdf5.c` - Updated 1 function signature
- `src/io/tree/interface.c` - Updated 1 wrapper function signature

### Function Declarations:
- `src/include/proto.h` - Updated 3 function declarations
- `src/io/tree/binary.h` - Updated 1 function declaration
- `src/io/tree/hdf5.h` - Updated 1 function declaration
- `src/io/tree/interface.h` - Updated 1 function declaration

### Call Sites:
- `src/core/build_model.c` - Updated calls to `update_halo_properties()` and `process_halo_evolution()`
- `src/core/main.c` - Updated call to `load_tree()`

## Verification

```bash
# Standard build (no HDF5)
make clean && make
# Result: ✓ No warnings (was 1 unused parameter warning before)

# HDF5 build
make clean && make USE-HDF5=yes
# Result: ✓ No warnings

# Functional test
./sage input/millennium.par
# Result: ✓ Runs successfully (exit 0)
```

## Impact

- **Code Clarity**: Function signatures now accurately reflect what parameters are actually used
- **API Cleanliness**: Removed 6 unnecessary parameters from public API
- **Maintainability**: Less confusing for future developers (no "why is this parameter here?" questions)
- **No Functional Changes**: All changes are signature-only, no behavior changes
- **Backward Compatibility**: Not applicable (MIMIC is new codebase, not yet released)

## Professional Coding Standards Applied

1. **Intent-Based Design**: Parameters reflect actual function needs, not legacy requirements
2. **API Minimalism**: Functions only receive data they actually use
3. **Clear Separation**: Legacy physics parameters removed from halo-only tracking code
4. **Documentation**: Updated all function headers to match new signatures
5. **Zero Technical Debt**: Removed dead parameters rather than marking them as unused
