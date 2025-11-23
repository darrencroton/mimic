# Simple HDF5 Performance Optimizations for Mimic

**Date**: 2025-11-23
**Context**: HDF5 output is ~7× slower than binary (25s vs 3.5s), but provides flexibility for testing/analysis
**Philosophy**: Keep it simple, clean, and transparent - no convoluted code

---

## Current Performance Profile

**Baseline (commit 287fb0c)**:
- Binary: 3.5s
- HDF5: ~19s (incomplete - missing TreeHalosPerSnap data)

**Current (HEAD)**:
- Binary: 3.7s (+0.2s common overhead)
- HDF5: 25s (+6.0s, primarily from H5Dwrite operations)

**HDF5-Specific Overhead**: ~5.7s from 64 H5Dwrite() calls (~89ms each)

---

## Optimization Philosophy

**Key Principle**: Binary is the production performance path. HDF5 is for:
- Testing and validation
- Interactive analysis
- Small-to-medium datasets
- When flexibility > raw speed

**Goal**: Reduce HDF5 overhead with **simple, transparent optimizations** that don't compromise code clarity.

---

## Recommended Optimizations (Ranked by Simplicity)

### 1. **File Access Properties** (5-15% speedup, ~0.5-1.5s)
**Complexity**: LOW (5-10 lines of code)
**Transparency**: HIGH (well-documented HDF5 standard features)

**Current Code** (line 133):
```c
file_id = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
```

**Optimized Code**:
```c
// Create file access property list with optimized settings
hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);

// Set alignment for better I/O (align on 4KB boundaries)
H5Pset_alignment(fapl, 4096, 4096);  // threshold, alignment

// Increase metadata cache size (default is often too small)
H5Pset_cache(fapl, 0, 521, 8 * 1024 * 1024, 0.75);
// mdc_nelmts=0 (unused), rdcc_nslots=521 (prime number),
// rdcc_nbytes=8MB (raw data chunk cache), rdcc_w0=0.75 (preemption policy)

// Create file with optimized properties
file_id = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);

// Clean up property list
H5Pclose(fapl);
```

**Why It Helps**:
- **Alignment**: Ensures data writes align with filesystem block boundaries (reduces syscalls)
- **Metadata cache**: Reduces repeated metadata reads during writes
- **Chunk cache**: Improves performance for chunked datasets (already used for table storage)

**Estimated Impact**: 5-15% of HDF5 write time (~1-3s → ~0.5-1.5s gain)

---

### 2. **Dataset Transfer Properties for H5Dwrite** (10-20% speedup, ~0.6-1.1s)
**Complexity**: LOW (5-10 lines of code)
**Transparency**: HIGH (standard HDF5 buffering)

**Current Code** (line 361):
```c
status = H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                  InputHalosPerSnap[n]);
```

**Optimized Code**:
```c
// Create dataset transfer property list (once, reuse for all writes)
static hid_t dxpl = H5P_DEFAULT;
if (dxpl == H5P_DEFAULT) {
  dxpl = H5Pcreate(H5P_DATASET_XFER);

  // Set type conversion and background buffer sizes (1MB each)
  size_t buf_size = 1024 * 1024;
  H5Pset_buffer(dxpl, buf_size, NULL, NULL);
}

// Use optimized transfer properties
status = H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, dxpl,
                  InputHalosPerSnap[n]);
```

**Why It Helps**:
- Default buffer size is often small (1KB)
- Larger buffers reduce system call overhead
- Type conversion buffers improve performance for data marshaling

**Estimated Impact**: 10-20% of H5Dwrite time (~5.7s → ~0.6-1.1s gain)

**Note**: Property list should be created once and reused (not per-write)

---

### 3. **Disable HDF5 Metadata Tracking** (2-5% speedup, ~0.1-0.3s)
**Complexity**: VERY LOW (1 line)
**Transparency**: HIGH (simple flag)

**Code Addition** (in prep_hdf5_file, before H5Fcreate):
```c
// Disable metadata tracking for better write performance
// Metadata tracking is primarily for debugging/optimization analysis
H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);
```

**Why It Helps**:
- Uses latest HDF5 format optimizations
- Reduces metadata overhead
- Files remain readable by recent HDF5 libraries (>= 1.10)

**Trade-off**: Files may not be readable by very old HDF5 versions (<1.10)
**Verdict**: Acceptable - HDF5 1.10+ is widely available (released 2016)

**Estimated Impact**: 2-5% overall (~0.1-0.3s gain)

---

### 4. **OPTIONAL: Compression** (Variable, can be slower or faster)
**Complexity**: LOW (3-5 lines)
**Transparency**: MEDIUM (adds dependency on deflate)
**Recommendation**: TEST ONLY - may not help

**When Compression Helps**:
- Large datasets (>100MB)
- I/O bound systems (slow disk, NFS)
- Repetitive data patterns

**When Compression Hurts**:
- CPU bound systems
- Small datasets (<10MB)
- Already-compressed filesystems

**Simple Implementation** (for TreeHalosPerSnap dataset):
```c
// Create dataset creation property list with compression
hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
H5Pset_deflate(dcpl, 1);  // Compression level 1 (fast)
H5Pset_chunk(dcpl, 1, &chunk_dims);  // Chunking required for compression

// Create dataset with compression
dataset_id = H5Dcreate(group_id, "TreeHalosPerSnap", H5T_NATIVE_INT,
                       dataspace_id, H5P_DEFAULT, dcpl, H5P_DEFAULT);

H5Pclose(dcpl);
```

**Estimated Impact**: Highly variable (-20% to +30% depending on system)
**Recommendation**: **Skip this** - adds complexity for uncertain benefit

---

## Implementation Strategy

### Phase 1: File Access Properties (Immediate, Low Risk)
**Files**: `src/io/output/hdf5.c` (prep_hdf5_file function)
**Lines to add**: ~8-10
**Expected gain**: ~0.5-1.5s

```c
void prep_hdf5_file(char *fname) {
  hsize_t chunk_size = 1000;
  int *fill_data = NULL;
  hid_t file_id, snap_group_id, fapl;
  char target_group[100];
  hid_t status;
  int i_snap;

  // Create file access property list with optimized settings
  fapl = H5Pcreate(H5P_FILE_ACCESS);
  H5Pset_alignment(fapl, 4096, 4096);              // 4KB alignment
  H5Pset_cache(fapl, 0, 521, 8 * 1024 * 1024, 0.75);  // 8MB cache
  H5Pset_libver_bounds(fapl, H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);

  DEBUG_LOG("Creating new HDF5 file '%s'", fname);
  file_id = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
  H5Pclose(fapl);

  // ... rest of function unchanged ...
}
```

### Phase 2: Dataset Transfer Properties (Immediate, Low Risk)
**Files**: `src/io/output/hdf5.c` (write_hdf5_attrs function)
**Lines to add**: ~10-12
**Expected gain**: ~0.6-1.1s

```c
void write_hdf5_attrs(int n, int filenr) {
  // Static property list - created once, reused
  static hid_t dxpl = H5P_DEFAULT;

  // ... existing variable declarations ...

  // Initialize transfer properties once
  if (dxpl == H5P_DEFAULT) {
    dxpl = H5Pcreate(H5P_DATASET_XFER);
    H5Pset_buffer(dxpl, 1024 * 1024, NULL, NULL);  // 1MB buffer
    INFO_LOG("Initialized HDF5 dataset transfer properties with 1MB buffer");
  }

  // ... existing code up to H5Dwrite ...

  // Write with optimized transfer properties
  status = H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, dxpl,
                    InputHalosPerSnap[n]);

  // ... rest of function unchanged ...
}
```

### Cleanup on Exit
**Files**: `src/core/main.c` or cleanup function
**Lines to add**: ~3-5

```c
// In cleanup/exit section
extern hid_t hdf5_dxpl;  // If made global
if (hdf5_dxpl != H5P_DEFAULT) {
  H5Pclose(hdf5_dxpl);
}
```

---

## Combined Expected Performance

**Current**: 25.0s HDF5 output
**After Phase 1**: ~24.0s (4% improvement)
**After Phase 2**: ~23.0s (8% total improvement, ~2s faster)

**Realistic Target**: ~23s for HDF5 (vs 3.7s binary, 6.2× slower instead of 6.8×)

**Is This Enough?**
Given that:
1. Binary is 6× faster and remains the production choice
2. HDF5 is now writing correct/complete data (vs buggy "fast" version)
3. These are simple, clean optimizations with no code complexity

**Answer**: Yes - 2s improvement with <25 lines of clean, transparent code is worthwhile.

---

## What NOT to Do

### ❌ Don't Add Complex Batching Logic
**Why**: Adds code complexity, makes debugging harder
**Verdict**: Not worth the maintenance burden

### ❌ Don't Use Async I/O or Threading
**Why**: Major architectural change, error-prone
**Verdict**: Violates "simple and clean" principle

### ❌ Don't Implement Custom Compression
**Why**: Reinventing the wheel, HDF5's compression is sufficient
**Verdict**: Unnecessary complexity

### ❌ Don't Add HDF5 SWMR (Single-Writer-Multiple-Reader)
**Why**: Not needed for current use case
**Verdict**: Feature creep

---

## Testing & Validation

### Performance Testing
```bash
cd scripts

# Baseline (current)
MAKE_FLAGS="USE-HDF5=yes" ./benchmark_mimic.sh
# Expected: ~25s

# After optimizations
MAKE_FLAGS="USE-HDF5=yes" ./benchmark_mimic.sh
# Target: ~23s (2s improvement)
```

### Correctness Testing
```bash
# Ensure all tests pass
make tests

# Verify HDF5 output is readable
h5dump output/results/millennium/model_000.hdf5 | head -50

# Compare with baseline output (should be identical except for timestamps)
```

### HDF5 File Validation
```bash
# Check file integrity
h5ls -r output/results/millennium/model_000.hdf5

# Verify TreeHalosPerSnap dataset is present and correct
h5dump -d "/Snap063/TreeHalosPerSnap" output/results/millennium/model_000.hdf5
```

---

## Alternative: Document the Trade-off

If even 2s isn't worth the code changes, simply **document the performance trade-off**:

**Option**: Add to documentation (e.g., `docs/user/output-formats.md`):

```markdown
### Performance Comparison

Output format performance (Millennium benchmark):
- **Binary**: ~3.7s (production performance)
- **HDF5**: ~25s (6.8× slower, flexible format)

**Recommendation**:
- Use HDF5 for testing, validation, and interactive analysis
- Use Binary for production runs with large datasets

The HDF5 overhead comes primarily from:
1. Metadata management (~2s)
2. Dataset write operations (~5.7s)
3. Type safety and error checking (~0.6s)

This overhead is inherent to HDF5's self-describing format and
ensures data portability, type safety, and ease of use.
```

---

## Recommendation

**Implement Phase 1 + Phase 2** for ~2s improvement (~8% faster HDF5):
- **Effort**: 2-3 hours (coding + testing)
- **Code added**: ~25 lines (clean, well-documented)
- **Risk**: Low (standard HDF5 features, widely used)
- **Benefit**: Measurable performance improvement with minimal complexity

**Rationale**:
- Aligns with Mimic's philosophy (clean, optimized code)
- Uses standard HDF5 features (transparent, well-documented)
- Meaningful improvement without over-engineering
- Good engineering practice (proper property list usage)

**Final Verdict**: Worth doing. The optimizations are simple, clean, and demonstrate proper HDF5 usage.
