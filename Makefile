# Mimic Makefile
EXEC = sage

# Directories
SRC_DIR = src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
DEP_DIR = $(BUILD_DIR)/deps

# Source files (recursive find)
SOURCES := $(shell find $(SRC_DIR) -name '*.c')
OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))
DEPS := $(patsubst $(SRC_DIR)/%.c,$(DEP_DIR)/%.d,$(SOURCES))

# Compiler
CC ?= cc
CFLAGS = -g -O2 -Wall -Wextra
CFLAGS += -I$(SRC_DIR)/include
CFLAGS += -I$(SRC_DIR)/core
CFLAGS += -I$(SRC_DIR)/io
CFLAGS += -I$(SRC_DIR)/util
CFLAGS += -I$(SRC_DIR)/modules

# Dependency generation
CFLAGS += -MMD -MP

# Libraries
LDFLAGS =
LIBS = -lm

# Optional: HDF5 support
ifdef USE-HDF5
    CFLAGS += -DHDF5
    HDF5_PREFIX ?= /opt/homebrew
    CFLAGS += -I$(HDF5_PREFIX)/include
    LDFLAGS += -L$(HDF5_PREFIX)/lib
    LIBS += -lhdf5_hl -lhdf5
else
    # If HDF5 is not enabled, exclude HDF5-specific source files
    SOURCES := $(filter-out %hdf5.c,$(SOURCES))
    OBJECTS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))
    DEPS := $(patsubst $(SRC_DIR)/%.c,$(DEP_DIR)/%.d,$(SOURCES))
endif

# Optional: MPI support
ifdef USE-MPI
    CC = mpicc
    CFLAGS += -DMPI
endif

# Git version tracking
GIT_VERSION_H = $(SRC_DIR)/include/git_version.h

# Build targets
.PHONY: all clean tidy help

all: $(EXEC)

$(GIT_VERSION_H): .git/HEAD .git/index
	@echo "Generating git version..."
	@echo "#ifndef GIT_VERSION_H" > $@
	@echo "#define GIT_VERSION_H" >> $@
	@echo "#define GIT_COMMIT \"$$(git rev-parse HEAD 2>/dev/null || echo 'unknown')\"" >> $@
	@echo "#define GIT_BRANCH \"$$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')\"" >> $@
	@echo "#define GIT_DATE \"$$(git log -1 --format=%cd --date=short 2>/dev/null || echo 'unknown')\"" >> $@
	@echo "#define BUILD_DATE \"$$(date '+%Y-%m-%d')\"" >> $@
	@echo "#endif" >> $@

$(EXEC): $(OBJECTS)
	@echo "Linking $@..."
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	@echo "Build complete"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c $(GIT_VERSION_H)
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*.d)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -MF $(DEP_DIR)/$*.d -c $< -o $@

-include $(DEPS)

clean:
	@echo "Cleaning..."
	rm -rf $(BUILD_DIR) $(EXEC) $(GIT_VERSION_H)
	@echo "Clean complete"

tidy:
	@echo "Removing build artifacts..."
	rm -rf $(BUILD_DIR)

help:
	@echo "Mimic Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build executable"
	@echo "  make clean        - Remove all build artifacts"
	@echo "  make tidy         - Remove build directory only"
	@echo ""
	@echo "Options:"
	@echo "  make USE-HDF5=yes - Enable HDF5 support"
	@echo "  make USE-MPI=yes  - Enable MPI support"
