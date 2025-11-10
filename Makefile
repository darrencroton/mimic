# Mimic Makefile
EXEC = mimic

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
.PHONY: all clean tidy help generate check-generated test test-unit test-integration test-scientific test-clean

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
	@echo "  make generate     - Generate property code from metadata"
	@echo "  make check-generated - Verify generated code is up-to-date (CI)"
	@echo ""
	@echo "Test targets:"
	@echo "  make test         - Run all tests (unit + integration + scientific)"
	@echo "  make test-unit    - Run unit tests only"
	@echo "  make test-integration - Run integration tests only"
	@echo "  make test-scientific  - Run scientific tests only"
	@echo "  make test-clean   - Clean test artifacts"
	@echo ""
	@echo "Options:"
	@echo "  make USE-HDF5=yes - Enable HDF5 support"
	@echo "  make USE-MPI=yes  - Enable MPI support"

# Property metadata code generation
generate:
	@echo "Generating property code from metadata..."
	@python3 scripts/generate_properties.py
	@echo "Done. Generated files are in src/include/generated/"

check-generated:
	@python3 scripts/check_generated.py

# Test targets
test: test-unit test-integration test-scientific
	@echo ""
	@echo ""
	@echo "============================================================"
	@echo "ALL TESTS COMPLETED"
	@echo "============================================================"
	@echo ""

test-unit:
	@echo ""
	@echo "============================================================"
	@echo "RUNNING UNIT TESTS"
	@echo "============================================================"
	@cd tests/unit && ./run_tests.sh

test-integration:
	@echo ""
	@echo ""
	@echo "============================================================"
	@echo "RUNNING INTEGRATION TESTS"
	@echo "============================================================"
	@echo "Building mimic with HDF5 support for integration tests..."
	@$(MAKE) clean > /dev/null 2>&1
	@$(MAKE) USE-HDF5=yes
	@echo ""
	-@cd tests/integration && python test_full_pipeline.py
	-@cd tests/integration && python test_output_formats.py

test-scientific:
	@echo ""
	@echo ""
	@echo "============================================================"
	@echo "RUNNING SCIENTIFIC VALIDATION TESTS"
	@echo "============================================================"
	@echo "Building mimic with HDF5 support for scientific tests..."
	@$(MAKE) clean > /dev/null 2>&1
	@$(MAKE) USE-HDF5=yes
	@echo ""
	-@cd tests/scientific && python test_scientific.py

test-clean:
	@echo "Cleaning test artifacts..."
	@rm -rf tests/unit/build
	@rm -f tests/unit/*.test
	@rm -rf tests/data/output/binary
	@rm -rf tests/data/output/hdf5
	@rm -f tests/data/test_*.par
	@rm -rf tests/**/__pycache__
	@rm -f tests/**/*.pyc
	@echo "Test artifacts cleaned"
