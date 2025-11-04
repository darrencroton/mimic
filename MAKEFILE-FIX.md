# Makefile Fix - 2025-11-04

## Issue
After the structural reorganization, running `make` without arguments would only generate the git version header and stop, not building the executable.

## Root Cause
The `$(GIT_VERSION_H)` target was defined before the `all` target in the Makefile. In GNU Make, the first non-special, non-pattern target becomes the default target. This caused Make to treat `git_version.h` as the goal instead of building `sage`.

Additionally, HDF5-specific source files were being compiled even when `USE-HDF5` was not set, causing compilation errors.

## Fix Applied
1. **Moved `all` target declaration to the top** of the targets section (before the git_version.h rule)
2. **Added conditional filtering** to exclude HDF5 source files when `USE-HDF5` is not defined:
   ```makefile
   else
       # If HDF5 is not enabled, exclude HDF5-specific source files
       SOURCES := $(filter-out %hdf5.c,$(SOURCES))
       OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))
       DEPS := $(patsubst $(SRC_DIR)/%.c,$(DEP_DIR)/%.d,$(SOURCES))
   endif
   ```

## Verification
✅ `make clean && make` - Builds successfully without HDF5
✅ `make clean && make USE-HDF5=yes` - Builds successfully with HDF5
✅ `./sage input/millennium.par` - Runs and exits with code 0

Both build configurations now work correctly.
