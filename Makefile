# Mimic Makefile
EXEC = mimic

# Directories
SRC_DIR = src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
DEP_DIR = $(BUILD_DIR)/deps

# Source files (recursive find, excluding template, archive, and test files)
SOURCES := $(shell find $(SRC_DIR) -name '*.c' ! -path '*/modules/_template/*' ! -path '*/modules/_archive/*' ! -name 'test_*.c')
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

# Python for tests (use virtual environment if available)
PYTHON := $(shell if [ -f mimic_venv/bin/python3 ]; then echo mimic_venv/bin/python3; else echo python3; fi)

# Git version tracking
GIT_VERSION_H = $(SRC_DIR)/include/git_version.h

# Build targets
.PHONY: all clean tidy help generate check-generated tests test-unit test-integration test-scientific test-clean generate-modules validate-modules check-modules

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

# -----------------------------------------------------------------------------
# Property metadata auto-generation
# -----------------------------------------------------------------------------

# YAML metadata inputs for property generation
PROP_YAML := metadata/properties/halo_properties.yaml \
             metadata/properties/galaxy_properties.yaml

# Generated headers and include fragments required by the C build
GEN_DIR := $(SRC_DIR)/include/generated
GENERATED_HEADERS := \
    $(GEN_DIR)/property_defs.h \
    $(GEN_DIR)/init_halo_properties.inc \
    $(GEN_DIR)/init_galaxy_properties.inc \
    $(GEN_DIR)/copy_to_output.inc \
    $(GEN_DIR)/hdf5_field_count.inc \
    $(GEN_DIR)/hdf5_field_definitions.inc

# Ensure all object files wait for generated headers; this lets `make` rebuild
# them automatically when YAML changes without requiring a manual `make generate`.
$(OBJECTS): $(GENERATED_HEADERS)

# Rule to (re)generate property code whenever YAML or generator changes
$(GENERATED_HEADERS): $(PROP_YAML) scripts/generate_properties.py
	@echo "Generating property code from metadata (auto)..."
	@python3 scripts/generate_properties.py
	@echo "Done. Generated files are in $(GEN_DIR)/ and output/mimic-plot/"

# -----------------------------------------------------------------------------
# Module metadata auto-generation
# -----------------------------------------------------------------------------

# YAML metadata inputs for module generation
MODULE_YAML := $(wildcard $(SRC_DIR)/modules/*/module_info.yaml)

# Generated module registration files
MODULE_INIT_C := $(SRC_DIR)/modules/module_init.c
MODULE_SOURCES_MK := tests/unit/module_sources.mk
MODULE_REFERENCE_MD := docs/user/module-reference.md

# Module validation script
MODULE_VALIDATOR := scripts/validate_modules.py

# Ensure module_init.o waits for generated module registration code
$(OBJ_DIR)/modules/module_init.o: $(MODULE_INIT_C)

# Rule to (re)generate module registration code whenever YAML or generator changes
$(MODULE_INIT_C): $(MODULE_YAML) scripts/generate_module_registry.py
	@echo "Generating module registration code from metadata (auto)..."
	@python3 scripts/generate_module_registry.py
	@echo "Done. Generated files for $(words $(MODULE_YAML)) module(s)"

clean: test-clean
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
	@echo "  make generate     - Generate all code from metadata (properties + modules)"
	@echo "  make check-generated - Verify generated code is up-to-date (CI)"
	@echo ""
	@echo "Module targets:"
	@echo "  make generate-modules  - Generate module registration code"
	@echo "  make validate-modules  - Validate module metadata"
	@echo ""
	@echo "Test targets:"
	@echo "  make tests        - Run all tests (unit + integration + scientific)"
	@echo "  make test-unit    - Run unit tests only"
	@echo "  make test-integration - Run integration tests only"
	@echo "  make test-scientific  - Run scientific tests only"
	@echo "  make test-clean   - Clean test artifacts"
	@echo "  make generate-test-registry - Auto-discover module tests"
	@echo "  make validate-test-registry - Validate test declarations"
	@echo ""
	@echo "Options:"
	@echo "  make USE-HDF5=yes - Enable HDF5 support"
	@echo "  make USE-MPI=yes  - Enable MPI support"
	@echo ""
	@echo "Notes:"
	@echo "  Code is auto-regenerated when YAML metadata changes:"
	@echo ""
	@echo "  Property metadata (metadata/properties/*.yaml):"
	@echo "    - src/include/generated/property_defs.h"
	@echo "    - src/include/generated/init_*_properties.inc"
	@echo "    - src/include/generated/copy_to_output.inc"
	@echo "    - src/include/generated/hdf5_field_*.inc"
	@echo "    - output/mimic-plot/generated_dtype.py"
	@echo ""
	@echo "  Module metadata (src/modules/*/module_info.yaml):"
	@echo "    - src/modules/module_init.c"
	@echo "    - tests/unit/module_sources.mk"
	@echo "    - docs/user/module-reference.md"

# Property metadata code generation
generate:
	@echo "Generating code from metadata..."
	@python3 scripts/generate_properties.py
	@python3 scripts/generate_module_registry.py
	@echo "Done. Generated files in src/include/generated/ and src/modules/"

# Module-specific generation targets
generate-modules:
	@echo "Generating module registration code..."
	@python3 scripts/generate_module_registry.py

validate-modules:
	@echo "Validating module metadata..."
	@python3 scripts/validate_modules.py

check-generated:
	@python3 scripts/check_generated.py

# Test registry generation (auto-discovers module tests)
generate-test-registry:
	@python3 scripts/generate_test_registry.py

validate-test-registry:
	@python3 scripts/validate_module_tests.py

# Test targets
tests: test-unit test-integration test-scientific
	@echo ""
	@echo ""
	@echo "============================================================"
	@echo "ALL TESTS COMPLETED"
	@echo "============================================================"
	@echo ""

test-unit:
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING UNIT TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@cd tests/unit && ./run_tests.sh

test-integration:
	@echo ""
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING INTEGRATION TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@echo "Building mimic with HDF5 support for integration tests..."
	@$(MAKE) clean > /dev/null 2>&1
	@$(MAKE) generate-test-registry > /dev/null 2>&1
	@$(MAKE) USE-HDF5=yes
	@echo ""
	@echo "Running core integration tests..."
	-@$(PYTHON) tests/integration/test_full_pipeline.py
	@echo ""
	-@$(PYTHON) tests/integration/test_output_formats.py
	@echo ""
	-@$(PYTHON) tests/integration/test_module_pipeline.py
	@echo ""
	@echo "Running module integration tests from registry..."
	@for test in $$(grep -v '^#' build/generated_test_lists/integration_tests.txt | grep -v '^$$'); do \
		echo ""; \
		echo "\033[0;34mRunning: $$test\033[0m"; \
		$(PYTHON) $$test || exit 1; \
	done

test-scientific:
	@echo ""
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING SCIENTIFIC VALIDATION TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@echo "Building mimic with HDF5 support for scientific tests..."
	@$(MAKE) clean > /dev/null 2>&1
	@$(MAKE) generate-test-registry > /dev/null 2>&1
	@$(MAKE) USE-HDF5=yes
	@echo ""
	@echo "Running core scientific tests..."
	-@$(PYTHON) tests/scientific/test_scientific.py
	@echo ""
	@echo "Running module scientific tests from registry..."
	@for test in $$(grep -v '^#' build/generated_test_lists/scientific_tests.txt | grep -v '^$$'); do \
		echo ""; \
		echo "\033[0;34mRunning: $$test\033[0m"; \
		$(PYTHON) $$test || exit 1; \
	done

test-clean:
	@echo "Cleaning test artifacts..."
	@rm -rf tests/unit/build
	@rm -rf tests/data/output/binary/*
	@rm -rf tests/data/output/hdf5/*
	@mkdir -p tests/data/output/binary
	@mkdir -p tests/data/output/hdf5
	@rm -rf tests/**/__pycache__
	@rm -f tests/**/*.pyc
	@echo "Test artifacts cleaned"
