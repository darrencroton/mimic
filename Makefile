# =============================================================================
# Mimic Makefile - Semi-Analytic Galaxy Formation Code
# =============================================================================

EXEC = mimic
.DEFAULT_GOAL := all

# -----------------------------------------------------------------------------
# Directory Configuration
# -----------------------------------------------------------------------------
SRC_DIR = src
MODEL_DIR = models
BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj
DEP_DIR = $(BUILD_DIR)/deps

# Targets that work without a model set selected.
MODEL_FREE_TARGETS := clean tidy help check-docs test-clean

# Default model set used when MODEL is not given on the command line.
# Most users work in a single model: leave this as your primary model and run
# plain `make`. Override per-invocation with `make MODEL=<name>`, or change this
# line if your primary model is not sage.
DEFAULT_MODEL := sage
MODEL ?= $(DEFAULT_MODEL)

# Catch the common 'model=' (lowercase) typo — Make variable names are case-sensitive.
# This fires unconditionally because lowercase 'model=' is never correct.
ifdef model
$(error Make variables are case-sensitive. Did you mean: make MODEL=$(model) ?)
endif

MODEL_ROOT = $(MODEL_DIR)/$(MODEL)
export MODEL

# Verify the selected model package exists for any target that builds or
# generates code. MODEL always has a value (DEFAULT_MODEL when unset), but the
# default — or an explicit MODEL=<name> — may name a package that has been
# renamed or removed, so fail loudly rather than silently mis-building.
ifneq ($(filter-out $(MODEL_FREE_TARGETS),$(or $(MAKECMDGOALS),all)),)
  ifeq ($(wildcard $(MODEL_ROOT)/.),)
    $(error Unknown MODEL '$(MODEL)'. Expected a package directory at $(MODEL_ROOT))
  endif
endif

.PHONY: FORCE
FORCE:

# -----------------------------------------------------------------------------
# Source Files Discovery
# -----------------------------------------------------------------------------
# Recursive find excluding templates, archives, generated code, and tests.
SOURCES := $(shell find $(SRC_DIR) -name '*.c' ! -path '*/module_system/template/*' ! -path '*/module_system/generated/*' ! -name 'test_*.c')
SOURCES += $(if $(MODEL),$(shell find $(MODEL_ROOT) -name '*.c' ! -path '*/_tests/*' ! -path '*/archive/*' ! -name 'test_*.c' 2>/dev/null))

# Explicitly add generated registry and framework test modules.
SOURCES += $(SRC_DIR)/module_system/generated/module_init.c
SOURCES += $(SRC_DIR)/module_system/test_fixture/test_fixture.c
SOURCES += $(SRC_DIR)/module_system/test_event_producer/test_event_producer.c
SOURCES += $(SRC_DIR)/module_system/test_event_consumer_alpha/test_event_consumer_alpha.c
SOURCES += $(SRC_DIR)/module_system/test_event_consumer_beta/test_event_consumer_beta.c
SOURCES += $(SRC_DIR)/module_system/test_event_producer_b/test_event_producer_b.c
SOURCES += $(SRC_DIR)/module_system/test_event_consumer_gamma/test_event_consumer_gamma.c

OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES))
DEPS := $(patsubst %.c,$(DEP_DIR)/%.d,$(SOURCES))

# -----------------------------------------------------------------------------
# Compiler Configuration
# -----------------------------------------------------------------------------
CC ?= cc

# Include directories
INCLUDE_DIRS := \
    . \
    $(SRC_DIR) \
    $(SRC_DIR)/include \
    $(SRC_DIR)/core \
    $(SRC_DIR)/io \
    $(SRC_DIR)/util \
    $(SRC_DIR)/module_system \
    $(MODEL_DIR) \
    $(MODEL_ROOT) \
    $(BUILD_DIR)/generated

# Compiler flags
CFLAGS = -g -O2 -Wall -Wextra
CFLAGS += $(addprefix -I,$(INCLUDE_DIRS))
CFLAGS += -DMIMIC_COMPILED_MODEL=\"$(MODEL)\"
CFLAGS += -DMIMIC_COMPILED_MODEL_PATH=\"$(MODEL_ROOT)\"
CFLAGS += -MMD -MP

# Linker configuration
LDFLAGS =
LIBS = -lm

# -----------------------------------------------------------------------------
# Required Library Detection - YAML
# -----------------------------------------------------------------------------
# YAML library is required for parameter file parsing
# Detection order: 1) pkg-config, 2) homebrew, 3) common paths, 4) error
YAML_FOUND := no

# Try pkg-config first (works on most Linux and properly configured macOS)
ifeq ($(shell pkg-config --exists yaml-0.1 2>/dev/null && echo yes),yes)
    CFLAGS += $(shell pkg-config --cflags yaml-0.1)
    LDFLAGS += $(shell pkg-config --libs-only-L yaml-0.1)
    LIBS += $(shell pkg-config --libs-only-l yaml-0.1)
    YAML_FOUND := yes
else
    # Try Homebrew (macOS) - use brew --prefix to get version-independent path
    BREW_YAML := $(shell command -v brew >/dev/null 2>&1 && brew --prefix libyaml 2>/dev/null)
    ifneq ($(BREW_YAML),)
        CFLAGS += -I$(BREW_YAML)/include
        LDFLAGS += -L$(BREW_YAML)/lib
        LIBS += -lyaml
        YAML_FOUND := yes
    else
        # Try common system paths (Linux distributions)
        ifneq ($(wildcard /usr/include/yaml.h),)
            LIBS += -lyaml
            YAML_FOUND := yes
        else ifneq ($(wildcard /usr/local/include/yaml.h),)
            CFLAGS += -I/usr/local/include
            LDFLAGS += -L/usr/local/lib
            LIBS += -lyaml
            YAML_FOUND := yes
        endif
    endif
endif

# Validate YAML library was found
ifneq ($(YAML_FOUND),yes)
    $(error libyaml not found! Install with: \
        Ubuntu/Debian: sudo apt-get install libyaml-dev | \
        macOS: brew install libyaml | \
        Fedora/RHEL: sudo dnf install libyaml-devel)
endif

# -----------------------------------------------------------------------------
# Optional Library Detection - HDF5
# -----------------------------------------------------------------------------
# Default: enable HDF5 unless explicitly opted out
ifndef USE-HDF5
	USE-HDF5 := yes
endif

ifeq ($(USE-HDF5),yes)
    CFLAGS += -DHDF5
    HDF5_FOUND := no

    # Try pkg-config first (works on most Linux and properly configured macOS)
    ifeq ($(shell pkg-config --exists hdf5 2>/dev/null && echo yes),yes)
        CFLAGS += $(shell pkg-config --cflags hdf5)
        LDFLAGS += $(shell pkg-config --libs-only-L hdf5)
        LIBS += -lhdf5_hl $(shell pkg-config --libs-only-l hdf5)
        HDF5_FOUND := yes
    else
        # Try Homebrew (macOS) - use brew --prefix to get version-independent path
        BREW_HDF5 := $(shell command -v brew >/dev/null 2>&1 && brew --prefix hdf5 2>/dev/null)
        ifneq ($(BREW_HDF5),)
            CFLAGS += -I$(BREW_HDF5)/include
            LDFLAGS += -L$(BREW_HDF5)/lib
            LIBS += -lhdf5_hl -lhdf5
            HDF5_FOUND := yes
        else
            # Try common system paths (Linux distributions)
            ifneq ($(wildcard /usr/include/hdf5.h),)
                LIBS += -lhdf5_hl -lhdf5
                HDF5_FOUND := yes
            else ifneq ($(wildcard /usr/include/hdf5/serial/hdf5.h),)
                # Ubuntu/Debian specific path
                CFLAGS += -I/usr/include/hdf5/serial
                LDFLAGS += -L/usr/lib/x86_64-linux-gnu/hdf5/serial
                LIBS += -lhdf5_hl -lhdf5
                HDF5_FOUND := yes
            else ifneq ($(wildcard /usr/local/include/hdf5.h),)
                CFLAGS += -I/usr/local/include
                LDFLAGS += -L/usr/local/lib
                LIBS += -lhdf5_hl -lhdf5
                HDF5_FOUND := yes
            endif
        endif
    endif

    # Validate HDF5 library was found
	ifneq ($(HDF5_FOUND),yes)
		$(error HDF5 not found! Install with: \
			Ubuntu/Debian: sudo apt-get install libhdf5-dev | \
			macOS: brew install hdf5 | \
			Fedora/RHEL: sudo dnf install hdf5-devel | \
			Or build without HDF5: make MODEL=$(MODEL) USE-HDF5=no)
	endif
else
    # If HDF5 is not enabled, exclude HDF5-specific source files
    SOURCES := $(filter-out %hdf5.c,$(SOURCES))
    OBJECTS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(SOURCES))
    DEPS := $(patsubst %.c,$(DEP_DIR)/%.d,$(SOURCES))
endif

# -----------------------------------------------------------------------------
# Optional Feature - MPI Support
# -----------------------------------------------------------------------------
ifdef USE-MPI
    # Check that mpicc is available
    ifeq ($(shell command -v mpicc >/dev/null 2>&1 && echo yes),yes)
        CC = mpicc
        CFLAGS += -DMPI
    else
        $(error MPI requested but mpicc not found! Install with: \
            Ubuntu/Debian: sudo apt-get install libopenmpi-dev | \
            macOS: brew install open-mpi | \
            Fedora/RHEL: sudo dnf install openmpi-devel | \
            Or specify compiler: make MODEL=$(MODEL) USE-MPI=yes CC=your-mpi-wrapper)
    endif
endif

# -----------------------------------------------------------------------------
# Python Configuration (for tests and code generation)
# -----------------------------------------------------------------------------
PYTHON := $(shell if [ -f mimic_venv/bin/python3 ]; then echo mimic_venv/bin/python3; else echo python3; fi)

# -----------------------------------------------------------------------------
# Git Version Tracking
# -----------------------------------------------------------------------------
GIT_VERSION_H = $(BUILD_DIR)/generated/git_version.h

# -----------------------------------------------------------------------------
# Build Targets
# -----------------------------------------------------------------------------
.PHONY: all clean tidy help info generate generate-modules check-generated check-docs tests test-unit test-integration test-scientific test-clean validate-modules lint-parameters validate-build

all: generate validate-build $(EXEC)

# Pre-build validation - runs on every make
validate-build:
	@echo "Running pre-build validation..."
	@$(MAKE) MODEL=$(MODEL) --no-print-directory lint-parameters
	@echo "Pre-build validation passed"

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

$(OBJ_DIR)/%.o: %.c $(GIT_VERSION_H) Makefile
	@mkdir -p $(dir $@) $(dir $(DEP_DIR)/$*.d)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -MF $(DEP_DIR)/$*.d -c $< -o $@

-include $(DEPS)

# -----------------------------------------------------------------------------
# Property metadata auto-generation
# -----------------------------------------------------------------------------

# YAML metadata inputs for property generation
PROP_YAML := src/core/core_properties.yaml \
             $(wildcard $(MODEL_ROOT)/model_properties.yaml) \
             $(wildcard simulations/*/halo_properties.yaml)

# Generated headers and include fragments required by the C build
GEN_DIR := $(SRC_DIR)/include/generated
GENERATED_HEADERS := \
    $(GEN_DIR)/property_defs.h \
    $(GEN_DIR)/init_halo_properties.inc \
    $(GEN_DIR)/init_galaxy_properties.inc \
    $(GEN_DIR)/copy_to_output.inc \
    $(GEN_DIR)/hdf5_field_count.inc \
    $(GEN_DIR)/hdf5_field_definitions.inc \
    $(GEN_DIR)/hdf5_field_metadata.inc \
    $(GEN_DIR)/output_schema_writer.inc

PROP_STAMP := $(BUILD_DIR)/generated/property_generation.stamp

# Run the smart property generator once per make invocation so MODEL switches
# cannot reuse a stale generated schema.
$(PROP_STAMP): $(PROP_YAML) scripts/generate_properties.py FORCE
	@echo "Generating property code from metadata..."
	@python3 scripts/generate_properties.py
	@mkdir -p $(BUILD_DIR)/generated
	@touch $@

# Generated headers depend on property YAML - kept for explicit dependency tracking
$(GENERATED_HEADERS): $(PROP_STAMP)
	@true

# Ensure object compilation waits for generated property and module registration outputs
$(OBJECTS): | $(GENERATED_HEADERS) $(MODULE_INIT_C)

# -----------------------------------------------------------------------------
# Module metadata auto-generation
# -----------------------------------------------------------------------------

# YAML metadata inputs for module generation
MODULE_YAML := $(wildcard $(MODEL_ROOT)/module_info.yaml) \
               $(wildcard $(MODEL_ROOT)/shared/module_info.yaml) \
               $(wildcard $(MODEL_ROOT)/modules/*/module_info.yaml) \
               $(wildcard $(SRC_DIR)/module_system/test_*/module_info.yaml)

# Generated module registration files
MODULE_INIT_C := $(SRC_DIR)/module_system/generated/module_init.c
MODULE_SOURCES_MK := tests/generated/module_sources.mk
# Module validation script
MODULE_VALIDATOR := scripts/validate_modules.py

# Ensure module_init.o waits for generated module registration code
$(OBJ_DIR)/src/module_system/generated/module_init.o: $(MODULE_INIT_C)

# Rule to (re)generate module registration code whenever YAML or generator changes
$(MODULE_INIT_C): $(MODULE_YAML) scripts/generate_module_registry.py FORCE
	@echo ""
	@echo "Generating module registration code from metadata (auto)..."
	@python3 scripts/generate_module_registry.py
	@echo "Generated files for $(words $(MODULE_YAML)) module(s)"
	@echo ""
	@echo "Validating parameter usage..."
	@python3 scripts/lint_parameter_usage.py

# -----------------------------------------------------------------------------
# Housekeeping Targets
# -----------------------------------------------------------------------------

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
	@echo "  make MODEL=sage info - Show build configuration and library detection"
	@echo "  make clean        - Remove all build artifacts"
	@echo "  make tidy         - Remove build directory only"
	@echo "  make MODEL=sage generate - Generate all code from metadata (properties + modules)"
	@echo "  make MODEL=sage check-generated - Verify generated code is up-to-date (CI)"
	@echo "  make check-docs   - Validate documentation links and anchors"
	@echo ""
	@echo "Module targets:"
	@echo "  make MODEL=sage generate-modules   - Generate module registration code only"
	@echo "  make MODEL=sage validate-modules   - Validate module metadata"
	@echo "  make MODEL=sage lint-parameters    - Verify parameter usage matches declarations"
	@echo ""
	@echo "Test targets:"
	@echo "  make MODEL=sage tests             - Run all tests (unit + integration + scientific)"
	@echo "  make MODEL=sage test-unit         - Run unit tests only"
	@echo "  make MODEL=sage test-integration  - Run integration tests only"
	@echo "  make MODEL=sage test-scientific   - Run scientific tests only"
	@echo "  make test-clean                   - Clean test artifacts"
	@echo "  make MODEL=sage generate-test-registry - Auto-discover module tests"
	@echo "  make MODEL=sage validate-test-registry - Validate test declarations"
	@echo ""
	@echo "Options:"
	@echo "  make MODEL=sage  - Build against a single model set (default: sage)"
	@echo "  make MODEL=sham   - Build the SHAM model set instead"
	@echo "  make MODEL=sage USE-HDF5=no  - Disable HDF5 support (binary-only build)"
	@echo "  make MODEL=sage USE-MPI=yes  - Enable MPI support"
	@echo "  make MODEL=sage -j4          - Parallel build (4 jobs, adjust as needed)"
	@echo ""
	@echo "Tips:"
	@echo "  - Use 'make MODEL=sage info' to see detected libraries and configuration"
	@echo "  - Parallel builds significantly speed up compilation: make MODEL=sage -j$$(nproc)"
	@echo ""
	@echo "Notes:"
	@echo "  Code is auto-regenerated when YAML metadata changes:"
	@echo ""
	@echo "  Property metadata (models/<name>/model_properties.yaml):"
	@echo "    - src/include/generated/property_defs.h"
	@echo "    - src/include/generated/init_*_properties.inc"
	@echo "    - src/include/generated/copy_to_output.inc"
	@echo "    - src/include/generated/hdf5_field_*.inc"
	@echo "    - src/include/generated/output_schema_writer.inc"
	@echo ""
	@echo "  Module metadata (models/<name>/modules/*/module_info.yaml):"
	@echo "    - src/module_system/generated/module_init.c"
	@echo "    - tests/generated/module_sources.mk"

# Show build configuration and detected libraries
info:
	@echo "Mimic Build Configuration"
	@echo "========================="
	@echo ""
	@echo "Compiler: $(CC)"
	@echo "Model set: $(MODEL) ($(MODEL_ROOT))"
	@echo "Build flags: $(CFLAGS)"
	@echo ""
	@echo "Library Detection:"
	@echo "------------------"
	@echo "YAML library: $(if $(filter yes,$(YAML_FOUND)),✓ Found,✗ Not found)"
ifeq ($(USE-HDF5),yes)
	@echo "HDF5 support: $(if $(filter yes,$(HDF5_FOUND)),✓ Enabled and found,✗ Enabled but not found)"
else
	@echo "HDF5 support: ✗ Disabled (set USE-HDF5=yes to enable)"
endif
ifdef USE-MPI
	@echo "MPI support: ✓ Enabled (using $(CC))"
else
	@if command -v mpicc >/dev/null 2>&1; then \
		echo "MPI support: ✗ Disabled (mpicc available, use USE-MPI=yes to enable)"; \
	else \
		echo "MPI support: ✗ Disabled (mpicc not installed)"; \
	fi
endif
	@echo ""
	@echo "Detection methods used:"
	@if pkg-config --exists yaml-0.1 2>/dev/null; then \
		echo "  YAML: pkg-config ($(shell pkg-config --modversion yaml-0.1 2>/dev/null))"; \
	elif command -v brew >/dev/null 2>&1 && brew --prefix libyaml >/dev/null 2>&1; then \
		echo "  YAML: Homebrew at $(shell brew --prefix libyaml)"; \
	else \
		echo "  YAML: System paths"; \
	fi
ifeq ($(USE-HDF5),yes)
	@if pkg-config --exists hdf5 2>/dev/null; then \
		echo "  HDF5: pkg-config ($(shell pkg-config --modversion hdf5 2>/dev/null))"; \
	elif command -v brew >/dev/null 2>&1 && brew --prefix hdf5 >/dev/null 2>&1; then \
		echo "  HDF5: Homebrew at $(shell brew --prefix hdf5)"; \
	else \
		echo "  HDF5: System paths"; \
	fi
endif
	@echo ""
	@echo "Module count: $(words $(MODULE_YAML))"
	@echo "Source files: $(words $(SOURCES))"
	@echo "Object files: $(words $(OBJECTS))"
	@echo ""

# -----------------------------------------------------------------------------
# Code Generation & Validation Targets
# -----------------------------------------------------------------------------

# Code generation from metadata (smart - only regenerates what changed)
generate:
	@python3 scripts/generate_properties.py
	@python3 scripts/generate_module_registry.py

generate-modules:
	@python3 scripts/generate_module_registry.py

validate-modules:
	@echo "Validating module metadata..."
	@python3 scripts/validate_modules.py

lint-parameters:
	@echo "Linting parameter usage..."
	@echo ""
	@python3 scripts/lint_parameter_usage.py

check-generated:
	@python3 scripts/check_generated.py

check-docs:
	@python3 scripts/check_docs.py

# Test registry generation (auto-discovers module tests)
generate-test-registry:
	@python3 scripts/generate_test_registry.py

validate-test-registry:
	@python3 scripts/validate_module_tests.py

# -----------------------------------------------------------------------------
# Test Targets
# -----------------------------------------------------------------------------

tests:
	@echo "Cleaning and building once for all tests..."
	@$(MAKE) clean > /dev/null 2>&1
	@$(MAKE) MODEL=$(MODEL) generate-test-registry > /dev/null 2>&1
	@$(MAKE) MODEL=$(MODEL) USE-HDF5=yes
	@mkdir -p build
	@rm -f build/.test_failures
	@echo ""
	@$(MAKE) MODEL=$(MODEL) check-docs || echo "docs" >> build/.test_failures || true
	@echo ""
	@$(MAKE) MODEL=$(MODEL) validate-modules || echo "validate-modules" >> build/.test_failures || true
	@echo ""
	@$(MAKE) MODEL=$(MODEL) test-unit || echo "unit" >> build/.test_failures || true
	@$(MAKE) MODEL=$(MODEL) test-integration || true
	@$(MAKE) MODEL=$(MODEL) test-scientific || true
	@echo ""
	@echo ""
	@if [ -f build/.test_failures ]; then \
		echo "\033[0;31m############################################################\033[0m"; \
		echo "\033[0;31mFAILED TEST SUITES: $$(cat build/.test_failures | tr '\n' ' ')\033[0m"; \
		echo "\033[0;31m############################################################\033[0m"; \
	else \
		echo "\033[0;32m############################################################\033[0m"; \
		echo "\033[0;32mALL UNIT, INTEGRATION, SCIENTIFIC TESTS PASSED ✓\033[0m"; \
		echo "\033[0;32m############################################################\033[0m"; \
	fi
	@echo ""
	@if [ -f build/.test_failures ]; then \
		rm -f build/.test_failures; \
		exit 1; \
	fi

test-unit:
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING UNIT TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@python3 scripts/generate_test_registry.py --strict
	@cd tests/unit && ./run_tests.sh

test-integration: generate validate-build $(EXEC)
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING INTEGRATION TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@python3 scripts/generate_test_registry.py --strict
	@echo ""
	@FAILED=0; \
	echo "Running core integration tests..."; \
	$(PYTHON) tests/integration/test_output_formats.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_tree_preservation.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_unique_galaxy_id.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_satellite_spatial_distribution.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_full_pipeline.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_module_pipeline.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_phase_execution.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_substeps.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_processing_modes.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_galaxy_major_loop.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_event_routing.py || FAILED=1; \
	echo ""; \
	$(PYTHON) tests/integration/test_event_schema_validation.py || FAILED=1; \
	echo ""; \
	echo "Running module integration tests from registry..."; \
	for test in $$(grep -v '^#' build/generated/integration_tests.txt | grep -v '^$$'); do \
		echo "\033[0;34mRunning: $$test\033[0m"; \
		$(PYTHON) $$test || FAILED=1; \
	done; \
	if [ $$FAILED -eq 1 ]; then \
		mkdir -p build; \
		echo "integration" >> build/.test_failures; \
		echo "\033[0;31m=== INTEGRATION TESTS FAILED ===\033[0m"; \
		echo ""; \
		exit 1; \
	else \
		echo "\033[0;32m=== ALL INTEGRATION TESTS PASSED ===\033[0m"; \
		echo ""; \
	fi

test-scientific: generate validate-build $(EXEC)
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING SCIENTIFIC VALIDATION TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@python3 scripts/generate_test_registry.py --strict
	@echo ""
	@FAILED=0; \
	echo "Running core scientific tests..."; \
	$(PYTHON) tests/scientific/test_scientific.py || FAILED=1; \
	echo ""; \
	echo "Running module scientific tests from registry..."; \
	for test in $$(grep -v '^#' build/generated/scientific_tests.txt | grep -v '^$$'); do \
		echo "\033[0;34mRunning: $$test\033[0m"; \
		$(PYTHON) $$test || FAILED=1; \
	done; \
	echo ""; \
	if [ $$FAILED -eq 1 ]; then \
		mkdir -p build; \
		echo "scientific" >> build/.test_failures; \
		echo "\033[0;31m=== SCIENTIFIC TESTS FAILED ===\033[0m"; \
		echo ""; \
		exit 1; \
	else \
		echo "\033[0;32m=== ALL SCIENTIFIC TESTS PASSED ===\033[0m"; \
		echo ""; \
	fi

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
