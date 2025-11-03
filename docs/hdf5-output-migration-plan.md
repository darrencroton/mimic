# SAGE HDF5 Output Migration Plan

**Status:** Planning Document
**Date:** 2025-11-03
**Objective:** Replace binary output format with HDF5 output format while retaining binary/HDF5 tree input capability

---

## Executive Summary

This plan outlines the complete migration from binary output to HDF5 output format. The migration will:
- **Remove** all binary output writing code (`io_save_binary.c`)
- **Activate** HDF5 output as the sole output format
- **Make HDF5 a required dependency** (not optional)
- **Remove all `#ifdef HDF5` conditionals** for cleaner code
- **Remove `USE-HDF5` Makefile flag** (HDF5 always enabled)
- **Update** sage-plot tools to read HDF5 output files
- **Retain** ability to read both binary and HDF5 merger tree input files
- **Improve** output file organization and metadata storage

### Key Architectural Decision

**HDF5 becomes a required dependency, not an optional feature.** This simplifies the codebase by:
- Eliminating preprocessor conditionals (`#ifdef HDF5`)
- Removing build configuration complexity
- Ensuring consistent behavior across all builds
- Aligning with modern scientific computing practices (HDF5 is ubiquitous)

---

## Current State Assessment

### Output (Writing)
- **Active:** Binary format via `io_save_binary.c`
  - Files: `model_z{redshift}_{filenr}` (e.g., `model_z0.000_0`)
  - Structure: Header (ntrees, ntotgals, gals_per_tree[]) + struct array
  - Called from: `main.c:370` via `save_halos()`

- **Inactive:** HDF5 format via `io_save_hdf5.c`
  - Functions exist: `calc_hdf5_props()`, `prep_hdf5_file()`, `write_hdf5_halo()`, `write_hdf5_attrs()`, `write_master_file()`, `free_hdf5_ids()`
  - **Not integrated** into main execution flow
  - **Never called** from main.c

### Input (Reading - No Changes Planned)
- **Active:** Both binary and HDF5 tree readers
  - `io_tree_binary.c`: Reads LHalo binary format trees
  - `io_tree_hdf5.c`: Reads Genesis HDF5 format trees
  - Selection: Via `TreeType` parameter in `.par` file

### Plotting Tools
- **Current:** Read binary output format
  - Location: `output/sage-plot/sage-plot.py`
  - Method: `np.fromfile()` for binary struct reading
  - Format: Hardcoded binary layout matching C structs

---

## Migration Architecture

### Phase 0: Simplify HDF5 Integration (Critical First Step)

**Objective:** Remove optional HDF5 flag and make HDF5 a required dependency.

#### 0.1 Remove Preprocessor Conditionals

**Current problematic pattern:**
```c
#ifdef HDF5
    // HDF5 code here
#endif
```

**Files requiring cleanup:**

1. **`code/config.h`** (lines 8-11):
   ```c
   // REMOVE the #ifdef wrapper
   #include <hdf5.h>
   #define MODELNAME "SAGE"
   ```

2. **`code/globals.h`** (lines 65-74):
   ```c
   // REMOVE the #ifdef wrapper - always declare these
   extern char *core_output_file;
   extern size_t HDF5_dst_size;
   extern size_t *HDF5_dst_offsets;
   extern size_t *HDF5_dst_sizes;
   extern const char **HDF5_field_names;
   extern hid_t *HDF5_field_types;
   extern int HDF5_n_props;

   // REMOVE this (no longer needed):
   // extern int HDF5Output;
   ```

3. **`code/io_tree.c`** (lines 38-40, 73, 147, 189):
   ```c
   // REMOVE #ifdef wrappers, always include:
   #include "io_tree_hdf5.h"

   // Always compile HDF5 tree loading code
   ```

4. **`code/io_tree_hdf5.h`** (line 4):
   ```c
   // REMOVE #ifdef HDF5 wrapper at top of file
   ```

5. **`code/core_allvars.c`** (line 39):
   ```c
   // REMOVE #ifdef wrapper around HDF5 variable declarations
   ```

6. **`code/core_read_parameter_file.c`** (line 261):
   ```c
   // REMOVE #ifndef HDF5 error check
   // TreeType validation should always support both formats
   ```

#### 0.2 Update Makefile

**Current (complex):**
```makefile
# Enable HDF5
USE-HDF5 = yes

ifdef USE-HDF5
    HDF5INCL := -I/opt/homebrew/include
    HDF5LIB := -L/opt/homebrew/lib -lhdf5_hl -lhdf5
    CFLAGS += $(HDF5INCL) -DHDF5
    LIBS += $(HDF5LIB)
else
    OBJS := $(filter-out $(SRC_DIR)/io_%hdf5.o, $(OBJS))
endif
```

**Target (simple):**
```makefile
# HDF5 is required (no optional flag)
# Adjust paths for your system:
# - macOS (Homebrew): /opt/homebrew/{include,lib}
# - Linux (apt): /usr/{include,lib}
# - Custom: specify your HDF5 installation paths

HDF5_INCL := -I/opt/homebrew/include
HDF5_LIB := -L/opt/homebrew/lib -lhdf5_hl -lhdf5

CFLAGS += $(HDF5_INCL)
LIBS += $(HDF5_LIB)

# Note: All HDF5 code is always compiled (no conditional exclusion)
```

**Changes:**
- Remove `USE-HDF5` flag completely
- Remove `ifdef/endif` conditional
- Remove `-DHDF5` from CFLAGS (no longer needed without #ifdef)
- Remove `filter-out` logic (always compile HDF5 files)
- Simplify comments to reflect required status

#### 0.3 Benefits of This Approach

1. **Cleaner Code:**
   - No preprocessor conditionals to maintain
   - Easier to read and understand
   - Reduces potential for configuration-dependent bugs

2. **Simpler Build:**
   - One build configuration
   - No optional features to track
   - Faster compilation (no conditional evaluation)

3. **Better Testing:**
   - All code paths always compiled
   - Consistent behavior across systems
   - No hidden bugs in conditional branches

4. **Professional Standard:**
   - Modern scientific software expects HDF5
   - Aligns with community practices
   - Easier for new developers

---

### Phase 1: Core Output System Integration

#### 1.1 Remove Binary Output System
**Files to remove:**
- `code/io_save_binary.c`
- `code/io_save_binary.h`

**References to clean up:**
- Remove from Makefile source lists (automatic via pattern matching)
- Remove `#include "io_save_binary.h"` from `main.c`
- Remove `save_halos()` calls (will be replaced)
- Remove file handle arrays: `save_fd[]`
- Remove finalization: `finalize_halo_file()`

#### 1.2 Integrate HDF5 Output
**Modify `main.c`:**

**Note:** No `#ifdef` wrappers needed - HDF5 is always compiled.

1. **Add includes** (top of file):
   ```c
   #include "io_save_hdf5.h"
   // Remove: #include "io_save_binary.h"
   ```

2. **Add initialization** (after parameter reading, before file loop):
   ```c
   /* Initialize HDF5 output table structure */
   calc_hdf5_props();
   ```

3. **Add per-file initialization** (start of file loop):
   ```c
   /* Create HDF5 output file for this file number */
   char hdf5_filename[MAX_STRING_LEN];
   snprintf(hdf5_filename, MAX_STRING_LEN, "%s/%s_%03d.hdf5",
            SageConfig.OutputDir, SageConfig.FileNameGalaxies, filenr);
   prep_hdf5_file(hdf5_filename);
   ```

4. **Replace save call** (per tree, replace `save_halos(filenr, treenr)`):
   ```c
   /* Save processed halos to HDF5 file */
   save_halos_hdf5(filenr, treenr);
   ```

5. **Add per-file finalization** (end of file loop, replace `finalize_halo_file()`):
   ```c
   /* Write metadata attributes for each output snapshot */
   for (int n = 0; n < SageConfig.NOUT; n++) {
       write_hdf5_attrs(n, filenr);
   }
   ```

6. **Add final cleanup** (after all files processed, before memory cleanup):
   ```c
   /* Create master HDF5 file with links to all output files */
   write_master_file();

   /* Free HDF5 resource arrays */
   free_hdf5_ids();
   ```

#### 1.3 Create HDF5 save_halos() Function
**New function in `io_save_hdf5.c`:**

```c
void save_halos_hdf5(int filenr, int tree) {
    // Similar structure to binary save_halos() but writes to HDF5

    // 1. Build output order and update mergeIntoID
    int OutputGalCount[MAXSNAPS], *OutputGalOrder;
    OutputGalOrder = malloc(NumProcessedHalos * sizeof(int));

    for (int n = 0; n < SageConfig.NOUT; n++) {
        for (int i = 0; i < NumProcessedHalos; i++) {
            if (ProcessedHalos[i].SnapNum == ListOutputSnaps[n]) {
                OutputGalOrder[i] = OutputGalCount[n]++;
            }
        }
    }

    // Update mergeIntoID pointers
    for (int i = 0; i < NumProcessedHalos; i++) {
        if (ProcessedHalos[i].mergeIntoID > -1) {
            ProcessedHalos[i].mergeIntoID = OutputGalOrder[ProcessedHalos[i].mergeIntoID];
        }
    }

    // 2. Write halos to HDF5 file
    for (int n = 0; n < SageConfig.NOUT; n++) {
        for (int i = 0; i < NumProcessedHalos; i++) {
            if (ProcessedHalos[i].SnapNum == ListOutputSnaps[n]) {
                struct HaloOutput halo_out;
                // Copy ProcessedHalos[i] to halo_out
                // ...
                write_hdf5_halo(&halo_out, n, filenr);
            }
        }
    }

    free(OutputGalOrder);
}
```

**Add to `io_save_hdf5.h`:**
```c
extern void save_halos_hdf5(int filenr, int tree);
```

#### 1.4 Clean Up Obsolete Code
**From `globals.h` and `core_allvars.c`:**
- ~~Remove: `HDF5Output`~~ (already removed in Phase 0)
- Remove: Binary-specific file descriptors: `save_fd[]`
- Remove: Any remaining binary output variables

**From `main.c`:**
- Remove: `finalize_halo_file()` function and calls (binary-specific)

---

### Phase 2: Plotting System Update

#### 2.1 Create HDF5 Reader Module
**New file: `output/sage-plot/hdf5_reader.py`**

```python
"""
HDF5 reader for SAGE halo output files.
"""
import h5py
import numpy as np
import os

def get_hdf5_dtype():
    """Return the NumPy dtype matching SAGE HDF5 HaloOutput structure."""
    return np.dtype([
        ("SnapNum", np.int32),
        ("Type", np.int32),
        ("HaloIndex", np.int64),
        ("CentralHaloIndex", np.int64),
        ("SAGEHaloIndex", np.int32),
        ("SAGETreeIndex", np.int32),
        ("SimulationHaloIndex", np.int64),
        ("MergeStatus", np.int32),
        ("mergeIntoID", np.int32),
        ("mergeIntoSnapNum", np.int32),
        ("dT", np.float32),
        ("Pos", (np.float32, 3)),
        ("Vel", (np.float32, 3)),
        ("Spin", (np.float32, 3)),
        ("Len", np.int32),
        ("Mvir", np.float32),
        ("CentralMvir", np.float32),
        ("Rvir", np.float32),
        ("Vvir", np.float32),
        ("Vmax", np.float32),
        ("VelDisp", np.float32),
        ("infallMvir", np.float32),
        ("infallVvir", np.float32),
        ("infallVmax", np.float32),
    ])

def read_hdf5_snapshot(filename, snapshot_num):
    """
    Read halos from a specific snapshot in an HDF5 file.

    Args:
        filename: Path to HDF5 file
        snapshot_num: Snapshot number to read

    Returns:
        NumPy recarray of halo data
    """
    with h5py.File(filename, 'r') as f:
        group_name = f"Snap{snapshot_num:03d}"

        if group_name not in f:
            return None

        dataset = f[group_name]['Galaxies']

        # Read as structured array
        data = dataset[:]

        # Convert to recarray for attribute access
        return np.rec.array(data)

def count_halos_in_file(filename, snapshot_num):
    """
    Count total halos in a specific snapshot of an HDF5 file.

    Args:
        filename: Path to HDF5 file
        snapshot_num: Snapshot number

    Returns:
        Number of halos, or 0 if snapshot doesn't exist
    """
    try:
        with h5py.File(filename, 'r') as f:
            group_name = f"Snap{snapshot_num:03d}"
            if group_name not in f:
                return 0
            dataset = f[group_name]['Galaxies']
            return len(dataset)
    except:
        return 0

def read_hdf5_metadata(filename):
    """
    Read metadata from HDF5 file attributes.

    Args:
        filename: Path to HDF5 file

    Returns:
        Dictionary of metadata
    """
    metadata = {}
    try:
        with h5py.File(filename, 'r') as f:
            # Read from master file RunProperties if it exists
            if 'RunProperties' in f:
                props = f['RunProperties']
                for key in props.attrs:
                    metadata[key] = props.attrs[key]
    except:
        pass
    return metadata
```

#### 2.2 Update Main Plotting Script
**Modify `output/sage-plot/sage-plot.py`:**

1. **Add import:**
   ```python
   from hdf5_reader import read_hdf5_snapshot, count_halos_in_file, get_hdf5_dtype
   ```

2. **Update file pattern detection:**
   ```python
   def detect_output_format(model_path, first_file):
       """Detect if output is binary or HDF5."""
       # Check for HDF5 format first (new default)
       hdf5_file = f"{model_path}_{first_file:03d}.hdf5"
       if os.path.isfile(hdf5_file):
           return 'hdf5'

       # Check for binary format (legacy)
       binary_file = f"{model_path}_{first_file}"
       if os.path.isfile(binary_file):
           return 'binary'

       return None
   ```

3. **Create format-agnostic reader:**
   ```python
   def read_data_auto(model_path, first_file, last_file, params=None):
       """
       Automatically detect format and read data.
       """
       fmt = detect_output_format(model_path, first_file)

       if fmt == 'hdf5':
           return read_data_hdf5(model_path, first_file, last_file, params)
       elif fmt == 'binary':
           print("Warning: Reading legacy binary format. "
                 "Consider regenerating with HDF5 output.")
           return read_data_binary(model_path, first_file, last_file, params)
       else:
           raise FileNotFoundError(f"No output files found for {model_path}")
   ```

4. **Rename current `read_data()` to `read_data_binary()`:**
   - Keep existing binary reader for backward compatibility
   - Add deprecation warning

5. **Create new `read_data_hdf5()`:**
   ```python
   def read_data_hdf5(model_path, first_file, last_file, params=None):
       """
       Read halo data from HDF5 output files.
       """
       print(f"Reading HDF5 halo data from {model_path}")

       if not params:
           print("Error: Parameter dictionary is required.")
           sys.exit(1)

       if validate_required_params(params, ["Hubble_h", "BoxSize"], "reading"):
           sys.exit(1)

       # Get snapshot list from parameter file
       snapshot_list = params.get("OutputSnapshots", [])
       if not snapshot_list:
           print("Error: No output snapshots defined.")
           sys.exit(1)

       # We'll read all snapshots and concatenate
       all_halos = []
       good_files = 0

       for fnr in range(first_file, last_file + 1):
           fname = f"{model_path}_{fnr:03d}.hdf5"

           if not os.path.isfile(fname):
               continue

           # Read all snapshots from this file
           for snap in snapshot_list:
               halos = read_hdf5_snapshot(fname, snap)
               if halos is not None and len(halos) > 0:
                   all_halos.append(halos)

           good_files += 1

       if not all_halos:
           print("Error: No halos found in HDF5 files.")
           sys.exit(1)

       # Concatenate all halos
       galaxies = np.concatenate(all_halos)

       # Calculate volume
       box_size = params["BoxSize"]
       total_files = last_file - first_file + 1
       volume = (box_size ** 3) * (good_files / total_files)

       # Metadata
       metadata = {
           'total_files': good_files,
           'total_halos': len(galaxies),
           'format': 'hdf5'
       }

       return galaxies, volume, metadata
   ```

6. **Update main execution:**
   ```python
   # Replace read_data() calls with read_data_auto()
   galaxies, volume, metadata = read_data_auto(model_path, first_file, last_file, params)
   ```

#### 2.3 Add HDF5 to Requirements
**Update `requirements.txt`:**
```
numpy>=1.20.0
matplotlib>=3.3.0
h5py>=3.0.0
tqdm>=4.50.0
```

---

### Phase 3: Documentation Updates

#### 3.1 Update CLAUDE.md
**Changes needed:**

1. **Update Build Commands section:**
   ```markdown
   ## Build Commands

   ```bash
   # Basic compilation (HDF5 required)
   make

   # With MPI support for parallel processing
   make USE-MPI=yes

   # Clean build artifacts
   make clean
   ```

   Note: HDF5 libraries are required. Installation:
   - macOS: `brew install hdf5`
   - Ubuntu/Debian: `sudo apt-get install libhdf5-dev`
   - CentOS/RHEL: `sudo yum install hdf5-devel`
   ```

2. **Update I/O System section:**
   ```markdown
   ### I/O System
   - **io_tree.c**: Master tree loading interface
   - **io_tree_binary.c**: Binary format tree reader (LHalo format)
   - **io_tree_hdf5.c**: HDF5 format tree reader (Genesis format)
   - **io_save_hdf5.c**: HDF5 output format writer (halo properties, 24 fields)
   ```

3. **Update Code Architecture section:**
   - Remove: References to `USE-HDF5` flag
   - Remove: References to optional HDF5 compilation
   - Remove: `io_save_binary.c` from documentation

#### 3.2 Update README.md
**Changes needed:**

1. **Requirements section:**
   ```markdown
   ### Requirements

   - C compiler (gcc or compatible)
   - GNU Make
   - **HDF5 libraries** (required) - version 1.10 or later
   - Python 3.x with h5py (for plotting)
   - (Optional) clang-format for code formatting
   - (Optional) black and isort for Python code formatting

   #### Installing HDF5

   **macOS (Homebrew):**
   ```bash
   brew install hdf5
   ```

   **Ubuntu/Debian:**
   ```bash
   sudo apt-get install libhdf5-dev
   ```

   **CentOS/RHEL:**
   ```bash
   sudo yum install hdf5-devel
   ```

   **From source:**
   Download from [HDF5 Downloads](https://www.hdfgroup.org/downloads/hdf5/)
   ```

2. **Quick Start section:**
   - Update file examples: `model_z0.000_0` → `model_000.hdf5`
   - Mention HDF5 format explicitly

3. **Add Output Format section (new):**
   ```markdown
   ## Output Format

   SAGE outputs halo catalogs in HDF5 format:
   - **Format:** HDF5 (Hierarchical Data Format 5)
   - **Files:** `{model_name}_{filenr:03d}.hdf5`
   - **Structure:**
     - `/Snap{snapnum:03d}/Galaxies` - Halo data tables
     - `/Snap{snapnum:03d}/TreeHalosPerSnap` - Halos per tree
     - Attributes: Ntrees, TotHalosPerSnap, etc.
   - **Master File:** `{model_name}.hdf5` - Links to all output files

   ### Advantages of HDF5 Format
   - Self-describing with metadata
   - Platform-independent
   - Efficient compression
   - Partial reading support
   - Standard analysis tools (HDFView, h5py)
   ```

#### 3.3 Update Parameter File Comments
**Modify `input/millennium.par`:**
- Remove any binary format references
- Add HDF5 format notes
- Update expected output file names in comments

#### 3.4 Create Migration Guide
**New file: `docs/binary-to-hdf5-migration.md`**

```markdown
# Migration Guide: Binary to HDF5 Output

## For Users Upgrading from Binary Output

### What Changed
- Output format: Binary → HDF5
- File naming: `model_z0.000_0` → `model_000.hdf5`
- File organization: Flat files → Snapshot-grouped structure

### What to Do
1. **Regenerate output:** Run SAGE with new version
2. **Update analysis scripts:** Use h5py to read HDF5 files
3. **sage-plot:** Automatically detects format (or use new version)

### Reading HDF5 in Python
```python
import h5py

# Open file
with h5py.File('model_000.hdf5', 'r') as f:
    # List snapshots
    print(list(f.keys()))  # ['Snap063', 'Snap037', ...]

    # Read snapshot data
    halos = f['Snap063']['Galaxies'][:]

    # Access attributes
    ntrees = f['Snap063']['Galaxies'].attrs['Ntrees']
```

### Reading HDF5 in C
```c
// Use HDF5 C library (link with -lhdf5)
hid_t file_id = H5Fopen("model_000.hdf5", H5F_ACC_RDONLY, H5P_DEFAULT);
hid_t dataset_id = H5Dopen(file_id, "/Snap063/Galaxies", H5P_DEFAULT);
// Read data...
```
```

---

### Phase 4: Testing Strategy

#### 4.1 Unit Tests
**Test components individually:**

1. **HDF5 Writer:**
   - Test: `prep_hdf5_file()` creates valid HDF5 structure
   - Test: `write_hdf5_halo()` writes correct data
   - Test: `write_hdf5_attrs()` stores metadata correctly
   - Test: `write_master_file()` creates valid links

2. **HDF5 Reader (Python):**
   - Test: `read_hdf5_snapshot()` reads all fields correctly
   - Test: Handles missing snapshots gracefully
   - Test: Metadata reading works

#### 4.2 Integration Tests
**Test complete workflow:**

1. **Basic Run:**
   ```bash
   make clean && make
   ./sage input/millennium.par
   # Verify: HDF5 files created
   # Verify: Correct number of halos
   # Verify: Master file exists
   ```

2. **Multi-file Run:**
   - Test with FirstFile=0, LastFile=7
   - Verify all 8 files created
   - Verify master file links work

3. **Plot Generation:**
   ```bash
   python output/sage-plot/sage-plot.py --param-file=input/millennium.par
   # Verify: Plots generated successfully
   # Verify: No errors reading HDF5
   ```

#### 4.3 Validation Tests
**Compare output with binary version:**

1. **Data Integrity:**
   - Run old version (binary output)
   - Run new version (HDF5 output)
   - Compare: Number of halos per snapshot
   - Compare: Key properties (mass, position, etc.)
   - Tolerance: Exact match expected

2. **Plot Comparison:**
   - Generate plots from binary output
   - Generate plots from HDF5 output
   - Visual comparison: Should be identical

#### 4.4 Performance Tests
**Measure I/O performance:**

1. **Write Performance:**
   - Time: Binary write vs HDF5 write
   - Memory: Peak memory usage
   - Expected: HDF5 may be slightly slower but manageable

2. **Read Performance (Python):**
   - Time: Binary read vs HDF5 read
   - Expected: HDF5 may be faster due to selective reading

---

### Phase 5: Implementation Sequence

#### Step-by-Step Execution Order

**Step 0: Simplify HDF5 Integration (Critical First!)**
- [ ] Remove all `#ifdef HDF5` conditionals from code files
- [ ] Update config.h to always include hdf5.h
- [ ] Update globals.h to always declare HDF5 variables
- [ ] Remove HDF5Output variable (obsolete)
- [ ] Update Makefile: remove USE-HDF5 flag and conditionals
- [ ] Update Makefile: always link HDF5 libraries
- [ ] Test compilation with simplified Makefile
- [ ] Verify both tree readers still work

**Step 1: Prepare HDF5 Output (Review Existing Code)**
- [ ] Review all HDF5 output code in `io_save_hdf5.c`
- [ ] Verify error handling is complete (already done)
- [ ] Check that all 24 fields are written correctly
- [ ] Verify no remaining #ifdef HDF5 in output code

**Step 2: Create Python HDF5 Reader**
- [ ] Create `output/sage-plot/hdf5_reader.py`
- [ ] Write unit tests for reader
- [ ] Verify dtype matches C struct exactly
- [ ] Test with manually created HDF5 file

**Step 3: Update sage-plot (Backward Compatible)**
- [ ] Add HDF5 reader to sage-plot.py
- [ ] Implement format auto-detection
- [ ] Keep binary reader functional
- [ ] Add deprecation warnings for binary
- [ ] Test with both formats

**Step 4: Integrate HDF5 Output into SAGE**
- [ ] Create `save_halos_hdf5()` function
- [ ] Add initialization in main.c
- [ ] Add per-file setup in main.c
- [ ] Replace binary save call with HDF5
- [ ] Add per-file finalization
- [ ] Add final cleanup
- [ ] Compile and test

**Step 5: Remove Binary Output**
- [ ] Remove `io_save_binary.c` and `.h`
- [ ] Remove includes from main.c
- [ ] Remove unused variables
- [ ] Clean up Makefile (automatic)
- [ ] Compile and verify

**Step 6: Update Documentation**
- [ ] Update CLAUDE.md
- [ ] Update README.md
- [ ] Update parameter file comments
- [ ] Create migration guide
- [ ] Update requirements.txt

**Step 7: Testing**
- [ ] Run unit tests
- [ ] Run integration tests
- [ ] Run validation tests
- [ ] Run performance tests
- [ ] Fix any issues found

**Step 8: Final Cleanup**
- [ ] Remove deprecated binary reader from sage-plot
- [ ] Final documentation review
- [ ] Create release notes
- [ ] Tag version

---

### Phase 6: Risk Assessment & Mitigation

#### Risks

1. **Data Loss Risk**
   - **Issue:** Users lose access to old binary output
   - **Mitigation:** Keep binary reader in sage-plot for 1-2 versions
   - **Mitigation:** Provide clear migration guide
   - **Mitigation:** Recommend users keep both versions

2. **Performance Risk**
   - **Issue:** HDF5 I/O slower than binary
   - **Mitigation:** Benchmark before migration
   - **Mitigation:** Optimize HDF5 chunk sizes
   - **Mitigation:** Use HDF5 compression if beneficial

3. **Compatibility Risk**
   - **Issue:** HDF5 library not available on all systems
   - **Mitigation:** Document installation for major platforms
   - **Mitigation:** Test on Linux, macOS, Windows (WSL)
   - **Mitigation:** Provide troubleshooting guide

4. **Bug Risk**
   - **Issue:** HDF5 output code has undetected bugs
   - **Mitigation:** Extensive testing before removal of binary
   - **Mitigation:** Validation against binary output
   - **Mitigation:** Beta testing period

#### Rollback Plan

If critical issues are found:
1. **Keep binary output code in git history**
   - Can revert specific commits
   - Can cherry-pick fixes

2. **Maintain release branches**
   - Last binary-output version tagged
   - Users can downgrade if needed

3. **Provide binary-to-HDF5 converter**
   - Python script to convert old binary to new HDF5
   - Allows users to update archived data

---

## Timeline Estimate

| Phase | Duration | Dependencies |
|-------|----------|--------------|
| Phase 0: Simplify HDF5 | 0.5 days | None |
| Phase 1: Core Integration | 2-3 days | Phase 0 complete |
| Phase 2: Plotting Update | 2-3 days | Python HDF5 reader |
| Phase 3: Documentation | 1 day | Phases 1-2 complete |
| Phase 4: Testing | 2-3 days | All phases complete |
| Phase 5: Cleanup | 1 day | Testing passed |
| **Total** | **8.5-11.5 days** | |

**Note:** Phase 0 must be completed first. It simplifies all subsequent phases by removing conditional compilation complexity.

---

## Success Criteria

- [ ] SAGE compiles and runs with HDF5 output only
- [ ] HDF5 files contain all expected data fields
- [ ] Master file correctly links to individual files
- [ ] sage-plot reads HDF5 files and generates correct plots
- [ ] All plots match previous binary-based plots
- [ ] Documentation is complete and accurate
- [ ] No binary output code remains in repository
- [ ] Migration guide is clear and tested

---

## Future Enhancements (Post-Migration)

After successful migration, consider:

1. **HDF5 Compression**
   - Enable gzip compression for smaller files
   - Test impact on read/write performance

2. **Chunked Writing**
   - Optimize chunk sizes for access patterns
   - Test with large simulation outputs

3. **Parallel HDF5**
   - Enable parallel HDF5 for MPI runs
   - Requires parallel HDF5 library build

4. **Extended Metadata**
   - Store more run parameters in HDF5 attributes
   - Store git version, compile flags, etc.

5. **Analysis Tools**
   - Provide example analysis scripts
   - Create HDFView configuration files
   - Add Jupyter notebook examples

---

## Appendix A: File Structure Comparison

### Binary Format (Current)
```
output/results/millennium/
├── model_z0.998_0          # Snapshot 63, file 0
├── model_z0.998_1          # Snapshot 63, file 1
├── ...
├── model_z0.000_0          # Snapshot 0, file 0
└── model_z0.000_7          # Snapshot 0, file 7
```

### HDF5 Format (Target)
```
output/results/millennium/
├── model_000.hdf5          # File 0, all snapshots
│   ├── /Snap063/Galaxies
│   ├── /Snap037/Galaxies
│   └── ...
├── model_001.hdf5          # File 1, all snapshots
├── ...
├── model_007.hdf5          # File 7, all snapshots
└── model.hdf5              # Master file with links
    ├── /Snap063/File000 → model_000.hdf5:/Snap063/Galaxies
    ├── /Snap063/File001 → model_001.hdf5:/Snap063/Galaxies
    └── ...
```

---

## Appendix B: Code Dependencies

### Files Modified
- `code/main.c` - Integration of HDF5 calls
- `code/io_save_hdf5.c` - Add `save_halos_hdf5()`
- `code/io_save_hdf5.h` - Add function declaration
- `output/sage-plot/sage-plot.py` - Add HDF5 reader
- `output/sage-plot/hdf5_reader.py` - New file
- `requirements.txt` - Add h5py

### Files Removed
- `code/io_save_binary.c`
- `code/io_save_binary.h`

### Files Unchanged
- `code/io_tree_binary.c` - Keep for tree reading
- `code/io_tree_hdf5.c` - Keep for tree reading
- `code/io_tree.c` - Keep as dispatcher

---

## Appendix C: Backward Compatibility

### Supporting Old Binary Output (Temporary)

During transition, sage-plot will support both formats:

```python
# Auto-detect format
format = detect_output_format(model_path, first_file)

if format == 'binary':
    warnings.warn("Reading legacy binary format. "
                  "Please regenerate with HDF5 output.",
                  DeprecationWarning)
    data = read_data_binary(...)
elif format == 'hdf5':
    data = read_data_hdf5(...)
```

**Deprecation Timeline:**
- Version N: Both formats supported, binary deprecated
- Version N+1: Both formats supported, binary warns prominently
- Version N+2: Binary support removed

---

## Summary: Clean Architecture Principles

This migration plan follows professional software engineering principles:

### 1. **Simplicity First**
- Remove optional compilation flags → Single build path
- Remove preprocessor conditionals → Cleaner, more readable code
- Standardize on HDF5 → Industry-standard format

### 2. **Explicit Dependencies**
- HDF5 is required (not hidden behind #ifdef)
- Clear installation instructions for all platforms
- No "optional feature" confusion

### 3. **Maintainability**
- Less code to maintain (no conditional branches)
- Easier for new developers to understand
- Consistent behavior across all builds

### 4. **Testing Benefits**
- All code paths always compiled
- No platform-specific bugs from conditional compilation
- Single test configuration

### 5. **Professional Standards**
- Aligns with modern scientific computing practices
- HDF5 is ubiquitous in the field
- Self-documenting output format

### Critical Success Factor

**Phase 0 must be completed first.** Removing the optional HDF5 flag and all `#ifdef` conditionals simplifies the entire migration. This is not just a technical detail—it's a fundamental architectural improvement that makes all subsequent work cleaner and more reliable.

---

## Appendix D: Preprocessor Conditionals Audit

**Complete list of `#ifdef HDF5` removals:**

| File | Line | Action |
|------|------|--------|
| `code/config.h` | 8 | Remove #ifdef wrapper around hdf5.h include |
| `code/globals.h` | 65 | Remove #ifdef wrapper around HDF5 globals |
| `code/io_tree.c` | 38 | Remove #ifdef around io_tree_hdf5.h include |
| `code/io_tree.c` | 73 | Remove #ifdef around HDF5 case in switch |
| `code/io_tree.c` | 147 | Remove #ifdef around load_tree_table_hdf5() |
| `code/io_tree.c` | 189 | Remove #ifdef around free_tree_table_hdf5() |
| `code/io_tree_hdf5.h` | 4 | Remove #ifdef wrapper at file level |
| `code/core_allvars.c` | 39 | Remove #ifdef around HDF5 variable definitions |
| `code/core_read_parameter_file.c` | 261 | Remove #ifndef error for genesis_lhalo_hdf5 |

**After cleanup:** Zero preprocessor conditionals for HDF5. All HDF5 code always compiled.

---

*End of Migration Plan*

