# Code Review Report: SAGE Dual Output System (Binary & HDF5)

#### **Executive Summary**

The decision to maintain both binary and HDF5 output formats has been noted. The current implementation successfully provides this dual capability. The HDF5 writer is functionally robust, leveraging key library features like batch writing and external links, which are significant improvements. The primary issue with the current dual-system architecture is **significant code duplication** between the binary and HDF5 save routines. This introduces a maintenance burden and increases the risk of logic divergence over time.

Regarding performance, the reported 3.5x slowdown for HDF5 compared to binary is substantial but not entirely unexpected given HDF5's feature-rich nature. While some overhead is inherent, there is a potential optimization in the data preparation stage that could yield meaningful performance gains, especially in scenarios with many output snapshots.

This report provides a concrete refactoring plan to eliminate code duplication and offers specific, actionable suggestions for investigating and improving HDF5 write performance.

---

### **1. Critical Issue: Code Duplication in Output Routines**

The most critical issue is the near-identical logic for preparing halo output data, which is duplicated across `io_save_binary.c` and `io_save_hdf5.c`.

#### **1.1 The Problem: Duplicated Halo Indexing Logic**

Both `save_halos()` (binary) and `save_halos_hdf5()` perform the exact same three setup steps before writing any data:

1.  **Allocate `OutputGalOrder`:** A workspace array is allocated to map the in-memory halo index to its final output index for a given snapshot.
2.  **Calculate Output Order:** The code iterates through all processed halos for each output snapshot to determine how many halos will be written (`OutputGalCount`) and what their final order will be (`OutputGalOrder`).
3.  **Update Merger Pointers:** The `mergeIntoID` field of each halo is updated using the `OutputGalOrder` map to ensure merger pointers are correct in the output files.

This entire block of logic, approximately 20 lines of code, is copied verbatim in both functions.

**Code Snippet (from `io_save_binary.c:save_halos` and `io_save_hdf5.c:save_halos_hdf5`):**
```c
  // This entire block is duplicated
  int OutputGalCount[MAXSNAPS], *OutputGalOrder;

  OutputGalOrder = (int *)malloc(NumProcessedHalos * sizeof(int));
  // ... error check ...

  for (i = 0; i < MAXSNAPS; i++)
    OutputGalCount[i] = 0;
  for (i = 0; i < NumProcessedHalos; i++)
    OutputGalOrder[i] = -1;

  for (n = 0; n < SageConfig.NOUT; n++) {
    for (i = 0; i < NumProcessedHalos; i++) {
      if (ProcessedHalos[i].SnapNum == ListOutputSnaps[n]) {
        OutputGalOrder[i] = OutputGalCount[n];
        OutputGalCount[n]++;
      }
    }
  }

  for (i = 0; i < NumProcessedHalos; i++)
    if (ProcessedHalos[i].mergeIntoID > -1)
      ProcessedHalos[i].mergeIntoID =
          OutputGalOrder[ProcessedHalos[i].mergeIntoID];
```

*   **Impact:** This duplication is a significant maintenance liability. Any bug fix or change to the halo indexing logic must be manually applied in two separate places, increasing the risk of inconsistency and error.

#### **1.2 The Solution: Refactor into a Shared Utility Function**

This common logic should be extracted into a single, shared function. This function would be responsible for preparing the output mapping for a given tree, independent of the final output format.

**Recommendation:**

1.  **Create a new shared function**, for example in `model_misc.c`, with the following signature:

    ```c
    // In model_misc.c (or a new core_io_utils.c)
    /**
     * @brief Prepares halo output ordering and updates merger pointers for a tree.
     *
     * This function calculates the final output order for all processed halos
     * across all snapshots and updates their mergeIntoID fields accordingly.
     * It is called once per tree before writing to any output format.
     *
     * @param[out] OutputGalCount  An array to be filled with the number of halos
     *                             per output snapshot.
     *
     * @return A dynamically allocated array (OutputGalOrder) containing the
     *         output index for each processed halo. The caller is responsible
     *         for freeing this array.
     */
    int* prepare_output_for_tree(int OutputGalCount[MAXSNAPS]);
    ```

2.  **Implement this function** by moving the duplicated code block into it.

3.  **Refactor `save_halos()` and `save_halos_hdf5()`** to call this new function. The beginning of both functions would be simplified to:

    ```c
    // In both save_halos() and save_halos_hdf5()
    void save_halos... (int filenr, int tree) {
      int OutputGalCount[MAXSNAPS];

      // Call the shared function to handle all the prep work.
      int *OutputGalOrder = prepare_output_for_tree(OutputGalCount);

      // ... proceed with format-specific writing logic ...
      // (The loops for writing will now use the pre-computed OutputGalCount)

      // Free the workspace at the end.
      free(OutputGalOrder);
    }
    ```

*   **Benefit:** This change would completely eliminate the code duplication, centralize the critical indexing logic, and make the entire output system more robust and maintainable.

---

### **2. HDF5 Performance Analysis and Optimization**

The 3.5x performance difference between binary and HDF5 is significant. While some overhead is inherent to HDF5's advanced features, we can investigate potential bottlenecks in the current implementation.

#### **2.1 Acknowledging Inherent HDF5 Overhead**

It is important to recognize that HDF5 will likely never be as fast as a raw binary `fwrite`. The library performs additional work:
*   **Metadata Management:** Updating table metadata, attributes, and internal data structures.
*   **Data Marshalling:** Packing the C struct data into the HDF5 compound datatype format.
*   **Chunking:** Managing the on-disk layout of data chunks.

The current implementation already employs the two most critical HDF5 performance best practices: **batch writing** (`H5TBappend_records`) and **keeping the file handle open**, so the largest performance gains have already been realized. However, further improvements may be possible.

#### **2.2 Potential Optimization: Data Gathering for Batch Writes**

*   **Observation:** Inside `save_halos_hdf5()`, the code iterates through all `NumProcessedHalos` for *each output snapshot* to collect the halos for that snapshot into a `halo_batch` buffer.

    ```c
    // In save_halos_hdf5()
    for (n = 0; n < SageConfig.NOUT; n++) {
      // ... allocate halo_batch ...
      for (i = 0; i < NumProcessedHalos; i++) { // This loop runs for every snapshot
        if (ProcessedHalos[i].SnapNum == ListOutputSnaps[n]) {
          // Copy halo to batch
        }
      }
      // ... write batch ...
    }
    ```
    The complexity of this data gathering phase is **O(NumProcessedHalos * NOUT)**. For a large tree and many output snapshots, this CPU-bound preparation step can become a significant bottleneck before any I/O even occurs.

*   **Recommendation:**
    Consider sorting the `ProcessedHalos` array by `SnapNum` once after `build_halo_tree()` is complete but before `save_halos()` is called.
    1.  **Sort:** Use `qsort` to sort the `ProcessedHalos` array based on the `SnapNum` field. The complexity would be **O(NumProcessedHalos * log(NumProcessedHalos))**.
    2.  **Iterate:** The `save_halos_hdf5()` function could then iterate through the sorted array *once*. It would identify contiguous blocks of halos belonging to each snapshot and write them as batches. This would reduce the data gathering complexity to **O(NumProcessedHalos)**.

*   **Trade-off:**
    *   If `NOUT` (number of output snapshots) is small (e.g., < 10), the current O(N * M) approach may be faster than an O(N log N) sort.
    *   If `NOUT` is large (e.g., > 20), the sort will almost certainly be faster.
    *   **Actionable Suggestion:** Profile the `save_halos_hdf5` function. If a significant amount of time is spent in the data gathering loops (and not the `H5TBappend_records` call itself), implementing this sorting strategy could provide a substantial performance boost.

#### **2.3 Tunable Parameter: HDF5 Chunk Size**

*   **Observation:** The chunk size for the HDF5 table is hardcoded to `1000` records in `prep_hdf5_file()`.
    ```c
    hsize_t chunk_size = 1000;
    ```
    HDF5 write performance is highly sensitive to chunk size. The ideal size depends on the underlying filesystem and access patterns. The current size of `1000 * sizeof(struct HaloOutput)` (~140 KB) is a very reasonable default and falls within the recommended 10 KB - 1 MB range.

*   **Recommendation:** While the current value is good, this is the most important "knob" for tuning HDF5 I/O performance. Consider making the `chunk_size` a configurable parameter (perhaps an optional one in the `.par` file) for advanced users who need to optimize I/O for a specific HPC system. For now, add a comment to the code explaining its importance.

---

### **3. Other Findings**

*   **(Minor Optimization) Inefficient Metadata Gathering for Master File:** As noted previously, `write_master_file()` re-opens each HDF5 file to read the `TotHalosPerSnap` attribute. This information is already available in the global `TotHalosPerSnap` array in memory. Using the in-memory array would avoid unnecessary file I/O during the finalization step.

*   **(Positive) Strong Foundational Implementation:** The use of a master file with external links, correct HDF5 datatype mapping, and consistent error handling are all hallmarks of a well-designed system. These features should be preserved.

---

### **Conclusion and Prioritized Actions**

Given the decision to maintain a dual-output system, the following actions are recommended to improve its quality:

1.  **High Priority - Eliminate Code Duplication:** Refactor the halo indexing and merger pointer logic into a single shared function as described in section 1.2. This is the most important step to improve the long-term maintainability and robustness of the codebase.

2.  **Medium Priority - Profile and Optimize HDF5 Data Gathering:** Profile the `save_halos_hdf5` function. If data preparation is a bottleneck, implement the sorting strategy described in section 2.2. This is the most likely area for a meaningful performance improvement beyond the inherent library overhead.

3.  **Low Priority - Refine Minor Details:**
    *   Modify `write_master_file()` to use in-memory halo counts.
    *   Add a comment to `prep_hdf5_file()` about the importance of `chunk_size` for performance tuning.

By addressing the code duplication, you will create a much cleaner and more professional dual-output architecture. By investigating the data gathering performance, you may be able to significantly narrow the performance gap between the binary and HDF5 writers.