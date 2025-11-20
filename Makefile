# Mimic Makefile
EXEC = mimic

# Directories
SRC_DIR = src
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
DEP_DIR = $(BUILD_DIR)/deps

# Source files (recursive find, excluding system template, archive, and test files)
# Note: Includes _system/generated/ and _system/test_fixture/ but excludes _system/template/
SOURCES := $(shell find $(SRC_DIR) -name '*.c' ! -path '*/modules/_system/template/*' ! -path '*/modules/_archive/*' ! -name 'test_*.c')
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
CFLAGS += -I$(BUILD_DIR)/generated

# Dependency generation
CFLAGS += -MMD -MP

# Libraries
LDFLAGS =
LIBS = -lm

# YAML library (required for parameter file parsing)
YAML_PREFIX ?= /opt/homebrew/Cellar/libyaml/0.2.5
CFLAGS += -I$(YAML_PREFIX)/include
LDFLAGS += -L$(YAML_PREFIX)/lib
LIBS += -lyaml

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
GIT_VERSION_H = $(BUILD_DIR)/generated/git_version.h

# Build targets
.PHONY: all clean tidy help generate check-generated tests test-unit test-integration test-scientific test-clean generate-modules validate-modules check-modules

all: $(EXEC)

$(GIT_VERSION_H): .git/HEAD .git/index
	@echo "Generating git version..."
	@mkdir -p $(BUILD_DIR)/generated
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
PROP_YAML := src/core/halo_properties.yaml \
             src/modules/galaxy_properties.yaml

# Generated headers and include fragments required by the C build
GEN_DIR := $(SRC_DIR)/include/generated
GENERATED_HEADERS := \
    $(GEN_DIR)/property_defs.h \
    $(GEN_DIR)/init_halo_properties.inc \
    $(GEN_DIR)/init_galaxy_properties.inc \
    $(GEN_DIR)/copy_to_output.inc \
    $(GEN_DIR)/hdf5_field_count.inc \
    $(GEN_DIR)/hdf5_field_definitions.inc

# Sentinel file to track that generated code has been verified
GEN_VERIFIED := $(BUILD_DIR)/.generated_verified

# Verify generated code is up-to-date before building
# This catches cases where git pull changes files but timestamps don't reflect dependencies
$(GEN_VERIFIED): $(PROP_YAML) $(MODULE_YAML) scripts/generate_properties.py scripts/generate_module_registry.py scripts/check_generated.py
	@echo "Verifying generated code is up-to-date..."
	@if ! python3 scripts/check_generated.py > /dev/null 2>&1; then \
		echo "Generated code out of date - auto-regenerating..."; \
		python3 scripts/generate_properties.py; \
		python3 scripts/generate_module_registry.py; \
	fi
	@mkdir -p $(BUILD_DIR)
	@touch $@

# Ensure all object files wait for verification (which ensures generated headers exist and are current)
$(OBJECTS): $(GEN_VERIFIED)

# Fallback rule to (re)generate property code when explicitly needed
# This is kept for direct make dependencies and manual regeneration
$(GENERATED_HEADERS): $(PROP_YAML) scripts/generate_properties.py
	@echo "Generating property code from metadata..."
	@python3 scripts/generate_properties.py

# -----------------------------------------------------------------------------
# Module metadata auto-generation
# -----------------------------------------------------------------------------

# YAML metadata inputs for module generation
MODULE_YAML := $(wildcard $(SRC_DIR)/modules/*/module_info.yaml)

# Generated module registration files
MODULE_INIT_C := $(SRC_DIR)/modules/_system/generated/module_init.c
MODULE_SOURCES_MK := tests/generated/module_sources.mk
MODULE_REFERENCE_MD := docs/generated/module-reference.md

# Module validation script
MODULE_VALIDATOR := scripts/validate_modules.py

# Ensure module_init.o waits for generated module registration code
$(OBJ_DIR)/modules/_system/generated/module_init.o: $(MODULE_INIT_C)

# Rule to (re)generate module registration code whenever YAML or generator changes
$(MODULE_INIT_C): $(MODULE_YAML) scripts/generate_module_registry.py
	@echo "Generating module registration code from metadata (auto)..."
	@python3 scripts/generate_module_registry.py
	@echo "Done. Generated files for $(words $(MODULE_YAML)) module(s)"

clean: test-clean
	@echo "Cleaning..."
	rm -rf $(BUILD_DIR) $(EXEC)
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
	@echo "  Property metadata (metadata/*.yaml):"
	@echo "    - src/include/generated/property_defs.h"
	@echo "    - src/include/generated/init_*_properties.inc"
	@echo "    - src/include/generated/copy_to_output.inc"
	@echo "    - src/include/generated/hdf5_field_*.inc"
	@echo "    - output/mimic-plot/generated/dtype.py"
	@echo ""
	@echo "  Module metadata (src/modules/*/module_info.yaml):"
	@echo "    - src/modules/_system/generated/module_init.c"
	@echo "    - tests/generated/module_sources.mk"
	@echo "    - docs/generated/module-reference.md"

# Code generation from metadata (smart - only regenerates what changed)
generate:
	@python3 scripts/generate_properties.py
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
tests: validate-modules test-unit test-integration test-scientific
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
	@$(MAKE) generate-test-registry > /dev/null 2>&1
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
	@for test in $$(grep -v '^#' build/generated/integration_tests.txt | grep -v '^$$'); do \
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
	@for test in $$(grep -v '^#' build/generated/scientific_tests.txt | grep -v '^$$'); do \
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
