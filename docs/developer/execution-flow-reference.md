# Mimic Execution Flow Reference

**Complete Function Call Trace from Entry to Exit**

This document provides a detailed traversal of every function in the Mimic codebase, documenting the execution flow from program startup through output writing and exit. Use this as a reference for understanding code structure and execution order.

**⚠️ IMPORTANT**: This document contains specific line number references (e.g., `src/core/main.c:149`) that are accurate as of the document creation date but may drift as code evolves. Treat line numbers as approximate guidance rather than exact locations. Use grep/search to find current locations of referenced functions.

---

## Table of Contents

1. [Program Startup & Initialization](#1-program-startup--initialization)
2. [Main Processing Loop](#2-main-processing-loop)
3. [Tree Loading](#3-tree-loading)
4. [Halo Processing](#4-halo-processing)
5. [Output Writing](#5-output-writing)
6. [Post-Processing & Cleanup](#6-post-processing--cleanup)
7. [Utility Functions](#7-utility-functions)

---

## 1. Program Startup & Initialization

### 1.1 Entry Point: `main()`
**Location:** `src/core/main.c:149`
**Purpose:** Program entry point, coordinates overall execution
**Condition:** Always called (entry point)

```
main()
├─ [BRANCH: MPI Compilation]
│  ├─ MPI_Init()                    // Initialize MPI environment
│  ├─ MPI_Comm_rank()               // Get processor task ID
│  ├─ MPI_Comm_size()               // Get total processors
│  └─ MPI_Get_processor_name()      // Get node name
│
├─ [Command-line Processing]
│  ├─ Parse --help, --verbose, --quiet, --skip flags
│  ├─ [IF --help]:
│  │  ├─ initialize_error_handling()
│  │  ├─ INFO_LOG()
│  │  └─ exit(0)
│  └─ Set verbosity and skip flags
│
├─ [Signal Handling Setup]
│  ├─ atexit(bye)                   // Register cleanup handler
│  └─ sigaction() [2x]              // SIGXCPU handler
│
├─ initialize_error_handling()      // src/util/error.c:77
│  ├─ set_log_level()
│  ├─ set_log_output()
│  └─ INFO_LOG()
│
├─ init_memory_system(0)            // src/util/memory.c:68
│  ├─ Allocate Table, SizeTable, CategoryTable arrays
│  ├─ Initialize memory statistics
│  └─ INFO_LOG()
│
├─ DEBUG_LOG()
├─ INFO_LOG()
│
└─ [Continue to Parameter Reading...]
```

---

### 1.2 Parameter File Reading: `read_parameter_file()`
**Location:** `src/core/read_parameter_file.c:64`
**Purpose:** Parse and validate parameter file
**Condition:** Always called during initialization

```
read_parameter_file(paramfilename)
├─ get_parameter_table()            // Get parameter definitions
├─ get_parameter_table_size()       // Get parameter count
├─ mymalloc()                       // Allocate param_read tracking
├─ INFO_LOG()
│
├─ [First Pass: Read Parameters]
│  ├─ fopen()                       // Open parameter file
│  ├─ [LOOP: Each line]
│  │  ├─ fgets()
│  │  ├─ sscanf()
│  │  ├─ is_parameter_valid()
│  │  └─ atof() / atoi() / strcpy()
│  └─ fclose()
│
├─ [Validation]
│  ├─ ERROR_LOG()                   // Report missing required params
│  ├─ strcat()                      // Add trailing slash to OutputDir
│  └─ [IF errors]: FATAL_ERROR()
│
├─ [BRANCH: Snapshot List Reading]
│  ├─ [IF SnapshotListFile specified]:
│  │  ├─ fopen()                    // Reopen parameter file
│  │  ├─ [LOOP]: fscanf()           // Read snapshot indices
│  │  └─ fclose()
│
├─ myfree()                         // Free param_read
├─ [IF errors]: FATAL_ERROR()
└─ INFO_LOG()
```

---

### 1.3 Framework Initialization: `init()`
**Location:** `src/core/init.c:50`
**Purpose:** Initialize units, cosmology, and snapshot data
**Condition:** Always called after parameter reading

```
init()
├─ mymalloc()                       // Allocate Age array
│
├─ set_units()                      // src/core/init.c:89
│  ├─ Calculate derived units (time, density, pressure, energy)
│  ├─ Convert physical constants to code units
│  └─ Compute critical density
│
├─ srand()                          // Seed RNG with fixed seed
│
├─ read_snap_list()                 // src/core/init.c:160
│  ├─ snprintf()                    // Build snapshot list filename
│  ├─ fopen()
│  ├─ [LOOP]: fscanf()              // Read scale factors
│  ├─ fclose()
│  └─ INFO_LOG()
│
├─ [LOOP: Each snapshot]
│  ├─ time_to_present()             // src/core/init.c:205
│  │  ├─ integration_workspace_alloc()
│  │  ├─ integration_qag()
│  │  │  └─ [Calls integrand_time_to_present() repeatedly]
│  │  └─ integration_workspace_free()
│  │
│  ├─ Calculate redshift from scale factor
│  └─ Calculate lookback time
│
└─ [Return to main]
```

---

### 1.4 HDF5 Initialization (Conditional)
**Location:** `src/core/main.c:267-272`
**Purpose:** Set up HDF5 output structures
**Condition:** IF compiled with USE_HDF5=yes AND HDF5 format selected

```
[BRANCH: HDF5 Format]
└─ calc_hdf5_props()                // src/io/output/hdf5.c:54
   ├─ mymalloc() [4x]               // Allocate property arrays
   │  ├─ HDF5_dst_offsets
   │  ├─ HDF5_dst_sizes
   │  ├─ HDF5_field_names
   │  └─ HDF5_field_types
   │
   ├─ H5Tarray_create() [3x]        // Create array datatypes for:
   │  ├─ Position[3]
   │  ├─ Velocity[3]
   │  └─ Spin[3]
   │
   └─ [Set up 24 halo property fields]
      └─ Field offsets, sizes, names, types
```

---

## 2. Main Processing Loop

### 2.1 File Loop: Processing Multiple Tree Files
**Location:** `src/core/main.c:275-377`
**Purpose:** Process each tree file from FirstFile to LastFile
**Condition:** Always executed

```
[LOOP: FileNum = FirstFile to LastFile]
│
├─ [BRANCH: MPI Mode]
│  └─ [IF MPI]: Skip if (FileNum - FirstFile) % NTasks != ThisTask
│
├─ [File Validation]
│  ├─ snprintf()                    // Build tree filename
│  ├─ fopen() / fclose()            // Check if tree file exists
│  ├─ [IF missing]:
│  │  ├─ INFO_LOG()
│  │  └─ continue                   // Skip to next file
│  │
│  ├─ snprintf()                    // Build output filename
│  ├─ stat()                        // Check if output exists
│  └─ [IF exists AND skip flag]:
│     ├─ INFO_LOG()
│     └─ continue                   // Skip to next file
│
├─ [Continue to Tree Table Loading...]
```

---

### 2.2 Tree Table Loading: Format Branching
**Location:** `src/io/tree/interface.c:68`
**Purpose:** Load tree metadata and structure
**Condition:** Once per file

```
load_tree_table()
│
├─ [BRANCH: Input Format]
│  │
│  ├─ [FORMAT: lhalo_binary]
│  │  └─ load_tree_table_binary()      // src/io/tree/binary.c:67
│  │     ├─ snprintf()                  // Build filename
│  │     ├─ fopen()                     // Open binary file
│  │     ├─ set_file_endianness()       // Set byte order
│  │     ├─ fread()                     // Read Ntrees
│  │     ├─ fread()                     // Read totNHalos
│  │     ├─ mymalloc()                  // InputTreeNHalos array
│  │     ├─ mymalloc()                  // InputTreeFirstHalo array
│  │     ├─ fread()                     // Read halo counts per tree
│  │     └─ Calculate starting indices
│  │
│  └─ [FORMAT: genesis_lhalo_hdf5] (if USE_HDF5=yes)
│     └─ load_tree_table_hdf5()         // Similar HDF5 operations
│        ├─ H5Fopen()
│        ├─ H5Dopen() / H5Dread()       // Read datasets
│        └─ Populate tree table arrays
│
├─ [Common: Allocate Output Counters]
│  └─ [LOOP: Each output snapshot]
│     └─ mymalloc()                     // InputHalosPerSnap arrays
│
└─ [BRANCH: Output Format Preparation]
   │
   ├─ [FORMAT: HDF5]
   │  └─ prep_hdf5_file()               // src/io/output/hdf5.c:229
   │     ├─ snprintf()                  // Build HDF5 filename
   │     ├─ H5Fcreate()                 // Create file
   │     ├─ [LOOP: Each output snapshot]
   │     │  ├─ H5Gcreate()              // Create snapshot group
   │     │  ├─ H5TBmake_table()         // Create empty halo table
   │     │  └─ H5Gclose()
   │     ├─ H5Fclose()
   │     └─ H5Fopen()                   // Reopen and keep open
   │
   └─ [FORMAT: Binary]
      └─ [LOOP: Each output snapshot]
         ├─ snprintf()                  // Build output filename
         ├─ fopen()                     // Create empty file
         └─ fclose()
```

---

## 3. Tree Loading

### 3.1 Individual Tree Loading: Format Branching
**Location:** `src/io/tree/interface.c:220`
**Purpose:** Load single merger tree into memory
**Condition:** Once per tree in file

```
load_tree(treenr)
│
├─ [BRANCH: Input Format]
│  │
│  ├─ [FORMAT: lhalo_binary]
│  │  └─ load_tree_binary()             // src/io/tree/binary.c:137
│  │     ├─ mymalloc()                  // Allocate InputTreeHalos
│  │     ├─ fread()                     // Read all halos in tree
│  │     └─ [Data now in InputTreeHalos array]
│  │
│  └─ [FORMAT: genesis_lhalo_hdf5]
│     └─ load_tree_hdf5()
│        ├─ H5Dread()                   // Read halo dataset
│        └─ Populate InputTreeHalos
│
├─ [Common: Calculate Array Sizes]
│  ├─ MaxProcessedHalos = 2 * TreeNHalos
│  └─ MaxFoFWorkspace = 4 * TreeNHalos
│
├─ [Common: Allocate Working Arrays]
│  ├─ mymalloc()                        // HaloAux array
│  ├─ mymalloc()                        // ProcessedHalos array
│  └─ mymalloc()                        // FoFWorkspace array
│
└─ [Initialize HaloAux flags]
   └─ [LOOP: Each halo]
      └─ Set DoneFlag = 0
```

---

## 4. Halo Processing

### 4.1 Tree Building: Recursive Traversal
**Location:** `src/core/build_model.c:53`
**Purpose:** Build halo tracking structures via depth-first traversal
**Condition:** Called for each unprocessed halo in tree

```
build_halo_tree(halonr, tree)
│
├─ Set HaloAux[halonr].DoneFlag = 1
│
├─ [Phase 1: Process Progenitors Recursively]
│  ├─ Get FirstProgenitor
│  └─ [WHILE progenitor exists]
│     ├─ [IF not done]:
│     │  └─ build_halo_tree(prog, tree)  // RECURSIVE CALL
│     └─ Get NextProgenitor
│
├─ [Phase 2: Process FOF Group Progenitors]
│  ├─ Get FirstHaloInFOFgroup
│  ├─ [IF not flagged]:
│  │  ├─ Set HaloFlag = 1
│  │  └─ [LOOP: Each halo in FOF group]
│  │     ├─ Get FirstProgenitor
│  │     └─ [WHILE progenitor exists]
│  │        ├─ [IF not done]:
│  │        │  └─ build_halo_tree(prog, tree)  // RECURSIVE
│  │        └─ Get NextProgenitor
│  │
│  └─ [Ensures all progenitors processed before current halo]
│
└─ [Phase 3: Join and Evolve Halos]
   ├─ Get FirstHaloInFOFgroup
   └─ [IF HaloFlag == 1]:
      ├─ Set ngal = 0
      ├─ Set HaloFlag = 2
      │
      ├─ [LOOP: Each halo in FOF group]
      │  └─ ngal = join_progenitor_halos()  // See Section 4.2
      │
      └─ process_halo_evolution()             // See Section 4.3
```

**Recursion Depth:** Can reach ~1000 levels for deep merger trees

---

### 4.2 Joining Progenitor Halos
**Location:** `src/core/build_model.c:362`
**Purpose:** Merge progenitor halos into FOF group
**Condition:** Called for each halo in FOF group

```
join_progenitor_halos(halonr, ngal)
│
├─ find_most_massive_progenitor()       // src/core/build_model.c:119
│  ├─ [LOOP: All progenitors]
│  │  └─ Track most massive with halos
│  └─ Return first_occupied index
│
├─ copy_progenitor_halos()              // src/core/build_model.c:172
│  │
│  ├─ [LOOP: All progenitors]
│  │  ├─ Get progenitor index
│  │  │
│  │  └─ [LOOP: Each halo in progenitor]
│  │     │
│  │     ├─ [Check/Grow FoFWorkspace]
│  │     │  ├─ [IF at capacity]:
│  │     │  │  └─ myrealloc()          // Double workspace size
│  │     │  └─ Copy ProcessedHalos → FoFWorkspace
│  │     │
│  │     ├─ [IF from most massive progenitor]:
│  │     │  └─ [Update Virial Properties]
│  │     │     ├─ get_virial_mass()    // src/core/halo_properties/virial.c:91
│  │     │     ├─ get_virial_radius()  // src/core/halo_properties/virial.c:146
│  │     │     │  └─ Calculate from critical density
│  │     │     └─ get_virial_velocity() // src/core/halo_properties/virial.c:115
│  │     │        └─ get_virial_radius() // Nested call
│  │     │
│  │     ├─ [Handle Type Transitions]
│  │     │  ├─ Central → Satellite
│  │     │  ├─ Satellite → Orphan
│  │     │  └─ Update infall properties
│  │     │
│  │     └─ Increment ngal
│  │
│  └─ [IF no progenitors with halos]:
│     └─ init_halo()                    // src/core/halo_properties/virial.c:36
│        ├─ Initialize with default properties
│        ├─ Copy position/velocity from InputTreeHalos
│        ├─ get_virial_velocity()
│        ├─ get_virial_mass()
│        ├─ get_virial_radius()
│        ├─ Set initial merger time = 999.9
│        └─ Increment ngal
│
├─ set_halo_centrals()                  // src/core/build_model.c:328
│  ├─ Find central halo (Type 0 or 1)
│  └─ Set CentralHalo pointer for all halos
│
└─ Return updated ngal
```

---

### 4.3 Processing Halo Evolution
**Location:** `src/core/build_model.c:455`
**Purpose:** Track halo evolution and mergers
**Condition:** Called after joining progenitors

```
process_halo_evolution(ngal)
│
├─ Identify central halo in FoFWorkspace
│
└─ update_halo_properties()             // src/core/build_model.c:385
   │
   ├─ [LOOP: All halos in FoFWorkspace]
   │  │
   │  ├─ [New Halo Branch]
   │  │  ├─ Set HaloAux pointers
   │  │  └─ Update counters
   │  │
   │  ├─ [Merged Halo Branch]
   │  │  ├─ Find in previous snapshot
   │  │  ├─ Update MergerTime
   │  │  ├─ Update MergedIntoIndex
   │  │  └─ Mark as merged (Type 3)
   │  │
   │  └─ [Continuing Halo Branch]
   │     ├─ Copy to ProcessedHalos array
   │     ├─ Increment NumProcessedHalos
   │     └─ Update HaloAux.NHalos
   │
   └─ [All halos now in ProcessedHalos array ready for output]
```

---

## 5. Output Writing

### 5.1 Output Preparation: Common Path
**Location:** `src/io/output/util.c:26`
**Purpose:** Prepare halos for output writing
**Condition:** Called before save_halos() or save_halos_hdf5()

```
prepare_output_for_tree()
│
├─ mymalloc_cat()                       // Allocate OutputGalOrder
│
├─ [Initialize Output Counters]
│  └─ [LOOP: Each snapshot]
│     └─ OutputGalCount[snap] = 0
│
├─ [Build Output Ordering Map]
│  └─ [LOOP: NumProcessedHalos]
│     ├─ Get snapshot number
│     ├─ Store in OutputGalOrder[counter]
│     └─ Increment OutputGalCount[snap]
│
├─ [Update Merger Pointers]
│  └─ [LOOP: All halos]
│     └─ Convert MergedIntoIndex to output index
│
└─ Return OutputGalOrder array
```

---

### 5.2 Output Writing: Format Branching
**Purpose:** Write processed halos to disk
**Condition:** Called after each tree is processed

```
[BRANCH: Output Format Selection]
│
├─ [FORMAT: HDF5]
│  └─ save_halos_hdf5()                 // src/io/output/hdf5.c:776
│     │
│     ├─ prepare_output_for_tree()
│     │
│     └─ [LOOP: Each output snapshot]
│        │
│        ├─ [IF no halos]: skip
│        │
│        ├─ mymalloc_cat()              // Allocate halo_batch
│        │
│        ├─ [LOOP: ProcessedHalos]
│        │  ├─ [IF belongs to this snapshot]:
│        │  │  └─ prepare_halo_for_output()  // src/io/output/binary.c:173
│        │  │     ├─ Copy properties to HaloOutput
│        │  │     ├─ Create unique HaloIndex:
│        │  │     │  └─ (FileNum * 1e12) + (TreeID * 1e6) + halo_nr
│        │  │     └─ Convert to physical units
│        │  └─ Increment batch counter
│        │
│        ├─ write_hdf5_halo_batch()     // src/io/output/hdf5.c:316
│        │  ├─ H5Gopen()                // Open snapshot group
│        │  ├─ H5TBappend_records()     // Write all halos at once
│        │  └─ H5Gclose()
│        │
│        └─ myfree()                    // Free halo_batch
│
└─ [FORMAT: Binary]
   └─ save_halos()                      // src/io/output/binary.c:67
      │
      ├─ prepare_output_for_tree()
      │
      └─ [LOOP: Each output snapshot]
         │
         ├─ [IF file not open]:
         │  ├─ fopen()                  // Binary write mode ("wb")
         │  ├─ setvbuf()                // Disable buffering
         │  ├─ malloc()                 // Allocate header buffer
         │  ├─ fwrite()                 // Write placeholder header
         │  ├─ fflush()
         │  └─ free()
         │
         └─ [LOOP: ProcessedHalos]
            └─ [IF belongs to this snapshot]:
               ├─ prepare_halo_for_output()
               ├─ fwrite()              // Write HaloOutput struct
               └─ Increment counters
```

---

### 5.3 File Finalization: Format Branching
**Location:** `src/core/main.c:352-375`
**Purpose:** Finalize output files after all trees processed
**Condition:** After file processing loop completes

```
[BRANCH: Output Format Finalization]
│
├─ [FORMAT: HDF5]
│  ├─ [LOOP: Each output snapshot]
│  │  └─ write_hdf5_attrs()            // src/io/output/hdf5.c:411
│  │     ├─ H5Gopen()                  // Open snapshot group
│  │     ├─ H5Dopen()                  // Open Galaxies dataset
│  │     │
│  │     ├─ [Write Ntrees Attribute]
│  │     │  ├─ H5Screate_simple()
│  │     │  ├─ H5Acreate()
│  │     │  ├─ H5Awrite()
│  │     │  ├─ H5Aclose()
│  │     │  └─ H5Sclose()
│  │     │
│  │     ├─ [Write TotHalosPerSnap Attribute]
│  │     │  └─ [Same H5A* sequence]
│  │     │
│  │     ├─ H5Dclose()
│  │     │
│  │     ├─ [Create TreeHalosPerSnap Dataset]
│  │     │  ├─ H5Screate_simple()
│  │     │  ├─ H5Dcreate()
│  │     │  └─ H5Dclose()
│  │     │
│  │     └─ H5Gclose()
│  │
│  ├─ H5Fclose()                       // Close file
│  └─ Set HDF5_current_file_id = -1
│
└─ [FORMAT: Binary]
   └─ finalize_halo_file()             // src/io/output/binary.c:274
      └─ [LOOP: Each output snapshot]
         ├─ fflush()                   // Flush buffer
         ├─ fseek()                    // Seek to beginning
         ├─ fwrite()                   // Write Ntrees
         ├─ fwrite()                   // Write TotHalosPerSnap
         ├─ fwrite()                   // Write InputHalosPerSnap[]
         ├─ fflush()                   // Final flush
         └─ fclose()                   // Close file
```

---

## 6. Post-Processing & Cleanup

### 6.1 Master File Creation (HDF5 Only)
**Location:** `src/io/output/hdf5.c:608`
**Purpose:** Create unified access file linking all output files
**Condition:** IF HDF5 format AND all files processed

```
[BRANCH: HDF5 Format Only]
└─ write_master_file()                 // src/io/output/hdf5.c:608
   │
   ├─ H5Fcreate()                      // Create master file
   │
   ├─ [LOOP: Each output snapshot]
   │  │
   │  ├─ H5Gcreate()                   // Create snapshot group
   │  │
   │  ├─ [Write Redshift Attribute]
   │  │  ├─ H5Screate_simple()
   │  │  ├─ H5Acreate()
   │  │  ├─ H5Awrite()
   │  │  ├─ H5Aclose()
   │  │  └─ H5Sclose()
   │  │
   │  ├─ H5Gclose()
   │  │
   │  └─ [LOOP: Each file number]
   │     │
   │     ├─ H5Gcreate()                // Create file group
   │     │
   │     ├─ [Create External Links]
   │     │  ├─ H5Lcreate_external()    // Link to Galaxies dataset
   │     │  └─ H5Lcreate_external()    // Link to TreeHalosPerSnap
   │     │
   │     ├─ [Read TotHalosPerSnap from target]
   │     │  ├─ H5Fopen()
   │     │  ├─ H5Dopen()
   │     │  ├─ H5Aopen()
   │     │  ├─ H5Aread()
   │     │  └─ H5Aclose() / H5Dclose() / H5Fclose()
   │     │
   │     ├─ [Write File Attributes]
   │     │  ├─ H5Screate_simple()
   │     │  ├─ H5Gopen()
   │     │  ├─ H5Acreate()
   │     │  ├─ H5Awrite()
   │     │  ├─ H5Aclose()
   │     │  ├─ H5Gclose()
   │     │  └─ H5Sclose()
   │     │
   │     └─ H5Gclose()
   │
   ├─ store_run_properties()           // Write simulation parameters
   │  ├─ get_parameter_table()
   │  ├─ H5Gcreate()                   // Create RunProperties group
   │  ├─ H5Screate_simple()
   │  ├─ H5Tcopy() / H5Tset_size()
   │  │
   │  ├─ [LOOP: Each parameter]
   │  │  ├─ H5Acreate()
   │  │  ├─ H5Awrite()
   │  │  └─ H5Aclose()
   │  │
   │  ├─ [Write Extra Properties]
   │  │  ├─ NCores
   │  │  ├─ RunEndTime
   │  │  └─ InputSimulation
   │  │
   │  └─ H5Sclose() / H5Gclose()
   │
   └─ H5Fclose()
```

---

### 6.2 Memory Cleanup
**Location:** `src/core/main.c:389-399`
**Purpose:** Free allocated memory and check for leaks
**Condition:** Always at end of execution

```
Memory Cleanup Sequence
│
├─ Age--                               // Restore original pointer
├─ myfree()                            // Free Age array
│
├─ check_memory_leaks()                // Verify all freed
│  └─ [Reports any remaining allocations]
│
└─ cleanup_memory_system()             // Free tracking arrays
```

---

### 6.3 Metadata File Creation
**Location:** `src/core/main.c:402-428`
**Purpose:** Copy parameter files and create version info
**Condition:** Always at end of successful run

```
Metadata Creation Sequence
│
├─ [Copy Configuration Files]
│  ├─ snprintf()                       // Build metadata dir path
│  ├─ mkdir()                          // Create directory
│  ├─ copy_file()                      // Copy parameter file
│  ├─ copy_file()                      // Copy snapshot list
│  └─ INFO_LOG()
│
└─ create_version_metadata()           // src/util/version.c:355
   │
   ├─ get_current_datetime()           // Get timestamp
   │
   ├─ get_git_commit_hash()            // Get git info
   │  └─ execute_command()             // src/util/version.c
   │     ├─ popen()                    // Run: git rev-parse HEAD
   │     ├─ fgets()
   │     └─ pclose()
   │
   ├─ get_git_branch_name()
   │  └─ execute_command()             // Run: git branch --show-current
   │
   ├─ get_github_commit_url()          // Build GitHub URL
   │
   ├─ get_compiler_info()              // Get compiler version
   │  └─ execute_command()             // Run: gcc --version
   │
   ├─ get_system_info()                // Get OS info
   │  ├─ uname()                       // Get system info
   │  │
   │  ├─ [BRANCH: Platform]
   │  │  ├─ [Darwin]:
   │  │  │  └─ execute_command()      // sw_vers
   │  │  └─ [Linux]:
   │  │     └─ fopen() / fgets() / fclose()  // /etc/os-release
   │
   ├─ get_username()
   │  └─ getpwuid()                    // Get user info
   │
   ├─ calculate_file_md5_checksum()
   │  └─ execute_command()             // md5 or md5sum
   │
   ├─ [Create JSON File]
   │  ├─ mkdir()                       // Ensure metadata dir exists
   │  ├─ fopen()                       // Create version_info.json
   │  ├─ fprintf() [many calls]        // Write JSON structure
   │  ├─ fclose()
   │  └─ INFO_LOG()
   │
   └─ [Return to main]
```

---

### 6.4 Program Exit
**Location:** `src/core/main.c:432-434`
**Purpose:** Clean exit from main
**Condition:** Always

```
Program Exit Sequence
│
├─ Set exitfail = 0                    // Mark success
├─ return 0                            // Exit main()
│
└─ [Automatically Triggered]
   └─ bye()                            // src/core/main.c:97
      │                                // (registered via atexit)
      ├─ [BRANCH: MPI Mode]
      │  ├─ MPI_Finalize()
      │  └─ free()                     // Free ThisNode
      │
      └─ [BRANCH: Exit Status]
         └─ [IF exitfail != 0]:
            ├─ unlink()                // Remove temp output
            │
            └─ [IF SIGXCPU received]:
               └─ printf()             // Print termination message
```

---

## 7. Utility Functions

### 7.1 Memory Management (Called Throughout)
**Location:** `src/util/memory.c`
**Purpose:** Track and manage all memory allocations

```
Memory Management Functions
│
├─ mymalloc(size, category)            // line 154
│  ├─ Check allocation limit
│  ├─ malloc()                         // Standard C allocation
│  ├─ Update tracking tables
│  ├─ Update statistics by category
│  └─ Return pointer
│
├─ mymalloc_cat(size)                  // Categorized allocation
│  └─ Calls mymalloc() with inferred category
│
├─ myrealloc(ptr, new_size)            // line 216
│  ├─ Find in tracking table
│  ├─ realloc()                        // Standard C reallocation
│  ├─ Update tracking tables
│  └─ Return new pointer
│
├─ myfree(ptr)                         // line 284
│  ├─ Find in tracking table
│  ├─ free()                           // Standard C free
│  ├─ Update statistics
│  └─ Remove from tracking
│
├─ print_allocated()                   // Print total memory usage
└─ print_allocated_by_category()       // Print per-category usage
```

**Memory Categories:**
- `MEM_HALOS`: Halo data structures
- `MEM_TREES`: Merger tree data
- `MEM_IO`: I/O working data
- `MEM_UTILITY`: Utility arrays
- `MEM_GALAXIES`: Legacy (deprecated)

---

### 7.2 Error Handling & Logging (Called Throughout)
**Location:** `src/util/error.c`
**Purpose:** Comprehensive logging and error handling

```
Logging Functions
│
├─ log_message(level, file, line, fmt, ...)  // line 153
│  ├─ Check log level filter
│  ├─ [Get timestamp]
│  │  └─ gettimeofday()
│  ├─ fprintf()                        // Write to log output
│  └─ [IF FATAL]:
│     └─ exit(1)
│
├─ DEBUG_LOG(fmt, ...)                 // Macro → log_message()
├─ INFO_LOG(fmt, ...)                  // Macro → log_message()
├─ WARNING_LOG(fmt, ...)               // Macro → log_message()
├─ ERROR_LOG(fmt, ...)                 // Macro → log_message()
└─ FATAL_ERROR(fmt, ...)               // Macro → log_message() + exit
```

**Log Levels:**
- `DEBUG`: Verbose debug information
- `INFO`: Normal informational messages
- `WARNING`: Warning messages
- `ERROR`: Non-fatal errors
- `FATAL`: Fatal errors (program exits)

---

### 7.3 I/O Wrappers (Called Throughout)
**Location:** `src/util/io.c` and inline in output files
**Purpose:** Endianness handling and error checking

```
I/O Wrapper Functions
│
├─ myfread(ptr, size, nmemb, stream, endian)
│  ├─ fread()                          // Standard C read
│  ├─ [IF endian mismatch]:
│  │  └─ swap_bytes_if_needed()       // Byte swapping
│  └─ [IF error]: FATAL_ERROR()
│
├─ myfwrite(ptr, size, nmemb, stream, endian)
│  ├─ malloc()                         // Temp buffer
│  ├─ memcpy()                         // Copy data
│  ├─ [IF endian mismatch]:
│  │  └─ swap_bytes_if_needed()
│  ├─ fwrite()                         // Standard C write
│  ├─ free()                           // Free temp buffer
│  └─ [IF error]: FATAL_ERROR()
│
└─ myfseek(stream, offset, whence)
   ├─ fseek()                          // Standard C seek
   └─ [IF error]: FATAL_ERROR()
```

---

### 7.4 Numerical Utilities (Called in Physics Modules)
**Location:** `src/util/numeric.c`
**Purpose:** Safe numerical operations

```
Numerical Functions
│
├─ is_greater(a, b, epsilon)           // Safe float comparison
│  └─ Returns (a - b) > epsilon
│
├─ is_equal(a, b, epsilon)             // Safe float equality
│  └─ Returns fabs(a - b) < epsilon
│
└─ is_finite(x)                        // Check for NaN/Inf
   └─ Returns isfinite(x)
```

---

### 7.5 Integration Routines (Called in Cosmology)
**Location:** `src/util/integration.c`
**Purpose:** Numerical integration for cosmology calculations

```
Integration Functions
│
├─ integration_workspace_alloc(n)      // Allocate workspace
│  └─ malloc()
│
├─ integration_qag(f, a, b, epsabs, epsrel, workspace, result)
│  ├─ [Adaptive quadrature algorithm]
│  ├─ Calls integrand function f repeatedly
│  └─ Returns integrated value
│
└─ integration_workspace_free(workspace)
   └─ free()
```

**Used for:**
- Time-to-present calculations
- Age of universe at each snapshot
- Lookback times

---

## Execution Statistics

### Typical Function Call Counts (for Millennium-scale simulation)

| Function Type | Calls per Run | Notes |
|--------------|---------------|-------|
| `build_halo_tree()` | Millions | Recursive, once per halo |
| `join_progenitor_halos()` | Millions | Once per FOF group |
| `prepare_halo_for_output()` | Millions | Once per output halo |
| `mymalloc()`/`myfree()` | Thousands | Per tree |
| `fwrite()` | Millions | Once per output halo |
| Log functions | Thousands | Throughout execution |

### Memory High Watermark

Typically occurs during tree processing when:
- `ProcessedHalos` array is largest
- Multiple large trees being processed
- FoF workspace has grown dynamically

### Performance Bottlenecks

1. **File I/O**: Reading/writing GB of data
2. **Tree Traversal**: Deep recursion in merger trees
3. **Halo Copying**: Memory copies during progenitor joins
4. **Dynamic Growth**: FoFWorkspace reallocation

---

## Key Control Flow Branches

### 1. Compilation Branches
- **USE_HDF5**: Enables HDF5 I/O paths
- **USE_MPI**: Enables parallel processing

### 2. Runtime Branches
- **Input Format**: `lhalo_binary` vs `genesis_lhalo_hdf5`
- **Output Format**: `binary` vs `hdf5`
- **MPI Mode**: Single vs multi-processor
- **Command Flags**: `--verbose`, `--quiet`, `--skip`

### 3. Processing Branches
- **Halo Type**: Central, Satellite, Orphan, Merged
- **Progenitor Status**: First occupied, merged, new
- **File Status**: Exists, missing, already processed

---

## Call Graph Summary

```
main()
 ├─ Initialization
 │   ├─ initialize_error_handling()
 │   ├─ init_memory_system()
 │   ├─ read_parameter_file()
 │   ├─ init()
 │   │   ├─ set_units()
 │   │   ├─ read_snap_list()
 │   │   └─ time_to_present() [loop]
 │   └─ calc_hdf5_props() [if HDF5]
 │
 ├─ File Loop [FirstFile..LastFile]
 │   ├─ load_tree_table()
 │   │   ├─ load_tree_table_binary() OR load_tree_table_hdf5()
 │   │   └─ prep_hdf5_file() OR create binary files
 │   │
 │   ├─ Tree Loop [0..Ntrees-1]
 │   │   ├─ load_tree()
 │   │   ├─ build_halo_tree() [recursive, depth-first]
 │   │   │   ├─ join_progenitor_halos()
 │   │   │   │   ├─ find_most_massive_progenitor()
 │   │   │   │   ├─ copy_progenitor_halos()
 │   │   │   │   │   ├─ init_halo() [if new]
 │   │   │   │   │   └─ get_virial_*() [3 functions]
 │   │   │   │   └─ set_halo_centrals()
 │   │   │   └─ process_halo_evolution()
 │   │   │       └─ update_halo_properties()
 │   │   ├─ save_halos() OR save_halos_hdf5()
 │   │   │   ├─ prepare_output_for_tree()
 │   │   │   ├─ prepare_halo_for_output() [loop]
 │   │   │   └─ write functions
 │   │   └─ free_halos_and_tree()
 │   │
 │   ├─ finalize_halo_file() OR write_hdf5_attrs()
 │   └─ free_tree_table()
 │
 ├─ Post-Processing
 │   ├─ write_master_file() [if HDF5]
 │   ├─ free_hdf5_ids() [if HDF5]
 │   ├─ Memory cleanup
 │   ├─ Copy metadata files
 │   └─ create_version_metadata()
 │
 └─ Exit
     └─ bye() [via atexit]
```

---

## Document Version

**Created:** 2025-11-06
**Codebase Version:** Based on commit hash at HEAD
**Purpose:** Developer reference for understanding execution flow

---

## Usage Notes

**For debugging:**
- Trace through this document matching actual execution
- Add breakpoints at key function boundaries
- Check recursion depth in `build_halo_tree()`

**For optimization:**
- Focus on bottleneck sections (I/O, tree traversal)
- Consider batch operations where noted
- Profile memory usage at high watermark points

**For extending:**
- Add new physics modules in Phase 4 (Halo Processing)
- Add new output formats by branching in Phase 5
- Add new tree formats by branching in Phase 3
