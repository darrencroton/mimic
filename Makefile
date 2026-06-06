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

# Targets that work without selected model/simulation packages.
MODEL_FREE_TARGETS := clean tidy help check-docs check-format test-clean

# Default model package used when MODEL is not given on the command line.
# Most users work with one model/simulation pair: leave these defaults as your
# primary pair and run plain `make`. Override per-invocation with
# `make MODEL=<name> SIMULATION=<name>`, or change these lines if your primary
# packages are not sage/Millennium.
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

# Default simulation package used when SIMULATION is not given on the command
# line. Mimic compiles one model package against one simulation/catalog property
# package at a time. Leave this as your primary simulation and run plain `make`;
# override per-invocation with `make SIMULATION=<name>` (or the `SIM=<name>`
# shorthand), or change this line if your primary simulation is not millennium.
DEFAULT_SIMULATION := millennium

# Catch the common 'simulation='/'sim=' (lowercase) typos — Make variable names
# are case-sensitive, so these would otherwise be silently ignored.
ifdef simulation
$(error Make variables are case-sensitive. Did you mean: make SIMULATION=$(simulation) ?)
endif
ifdef sim
$(error Make variables are case-sensitive. Did you mean: make SIM=$(sim) ?)
endif

# Accept SIM as a shorthand for SIMULATION. An explicit SIMULATION=<name> on the
# command line takes precedence; SIM only fills in when SIMULATION is unset.
ifdef SIM
  SIMULATION ?= $(SIM)
endif
SIMULATION ?= $(DEFAULT_SIMULATION)

SIMULATION_ROOT = simulations/$(SIMULATION)
export SIMULATION

# Verify the selected simulation package exists for any target that builds or
# generates code, mirroring the MODEL guard above.
ifneq ($(filter-out $(MODEL_FREE_TARGETS),$(or $(MAKECMDGOALS),all)),)
  ifeq ($(wildcard $(SIMULATION_ROOT)/.),)
    $(error Unknown SIMULATION '$(SIMULATION)'. Expected a package directory at $(SIMULATION_ROOT))
  endif
endif

.PHONY: FORCE
FORCE:

# -----------------------------------------------------------------------------
# Test build toggle
# -----------------------------------------------------------------------------
# Production builds (the default) carry no framework test scaffolding. Test
# builds (TEST_BUILD=yes) additionally compile the framework test fixture/event
# modules under src/module_system/test_* and merge their test-only property
# metadata (TestDummyProperty, from src/module_system/test_fixture/
# test_properties.yaml) into the generated schema. The generation scripts read
# MIMIC_TEST_BUILD via scripts/discovery.py. The test targets below build with
# TEST_BUILD=yes; tests/unit/run_tests.sh sets MIMIC_TEST_BUILD directly.
TEST_BUILD ?= no

# Test builds use a separate object tree (build/test) so they never share
# compiled objects with a production build. This matters because the two modes
# generate different code (the test build adds the fixture modules and
# TestDummyProperty): a shared object tree could otherwise link a stale
# production module registry into a freshly generated test binary. The
# executable name is intentionally left as $(EXEC) (mimic) so every test
# harness, including model-local module tests, finds it without special casing.
ifeq ($(TEST_BUILD),yes)
  export MIMIC_TEST_BUILD := 1
  BUILD_DIR := build/test
endif

# -----------------------------------------------------------------------------
# Source Files Discovery
# -----------------------------------------------------------------------------
# Recursive find excluding templates, archives, generated code, and tests.
SOURCES := $(shell find $(SRC_DIR) -name '*.c' ! -path '*/module_system/template/*' ! -path '*/module_system/generated/*' ! -name 'test_*.c')
SOURCES += $(if $(MODEL),$(shell find $(MODEL_ROOT) -name '*.c' ! -path '*/_tests/*' ! -path '*/archive/*' ! -name 'test_*.c' 2>/dev/null))

# Explicitly add the generated module registry (always compiled; it registers
# the framework test modules only when generated for a test build).
SOURCES += $(SRC_DIR)/module_system/generated/module_init.c

# Framework test fixture/event modules — test builds only. In production these
# are excluded from the executable and their registrations are absent from the
# generated module_init.c, so the two stay consistent.
ifeq ($(TEST_BUILD),yes)
SOURCES += $(SRC_DIR)/module_system/test_fixture/test_fixture.c
SOURCES += $(SRC_DIR)/module_system/test_event_producer/test_event_producer.c
SOURCES += $(SRC_DIR)/module_system/test_event_consumer_alpha/test_event_consumer_alpha.c
SOURCES += $(SRC_DIR)/module_system/test_event_consumer_beta/test_event_consumer_beta.c
SOURCES += $(SRC_DIR)/module_system/test_event_producer_b/test_event_producer_b.c
SOURCES += $(SRC_DIR)/module_system/test_event_consumer_gamma/test_event_consumer_gamma.c
endif

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
CFLAGS = -g -O2 -Wall -Wextra -Wshadow -Wformat-security -Wundef
CFLAGS += $(addprefix -I,$(INCLUDE_DIRS))
CFLAGS += -DMIMIC_COMPILED_MODEL=\"$(MODEL)\"
CFLAGS += -DMIMIC_COMPILED_MODEL_PATH=\"$(MODEL_ROOT)\"
CFLAGS += -DMIMIC_COMPILED_SIMULATION=\"$(SIMULATION)\"
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
.PHONY: all clean tidy help info generate generate-modules generate-test-inputs check-generated check-docs check-format tests tests-unit tests-integration tests-scientific test-clean validate-modules lint-parameters validate-build

all: generate validate-build $(EXEC)

# Pre-build validation - runs on every make
validate-build:
	@echo "Running pre-build validation..."
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) --no-print-directory lint-parameters
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

# Records the build mode (TEST_BUILD value) of the last link. Production and
# test builds use separate object trees but share the $(EXEC) (mimic) path, so
# without this a production build after a test build (or vice versa) might see
# its own objects as older than the existing binary and skip relinking, leaving
# a mismatched executable. The marker is shared (always under build/) and only
# rewritten when the mode changes, forcing a relink exactly on a mode switch.
EXEC_MODE_MARKER := build/.last_exec_mode
$(EXEC_MODE_MARKER): FORCE
	@mkdir -p build
	@printf '%s' '$(TEST_BUILD)' > $@.tmp
	@if cmp -s $@.tmp $@ 2>/dev/null; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(EXEC): $(OBJECTS) $(EXEC_MODE_MARKER)
	@echo "Linking $@..."
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS)
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
             $(wildcard $(SIMULATION_ROOT)/halo_properties.yaml)

# Test builds merge fixture-owned test-only properties (TestDummyProperty) so
# the stamp re-fires if that file changes; production builds omit it entirely.
ifeq ($(TEST_BUILD),yes)
PROP_YAML += $(SRC_DIR)/module_system/test_fixture/test_properties.yaml
endif

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
	rm -rf build $(EXEC)
	@echo "Clean complete"

tidy:
	@echo "Removing build artifacts..."
	rm -rf $(BUILD_DIR)

help:
	@echo "Mimic Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make              - Build executable"
	@echo "  make info         - Show build configuration and library detection"
	@echo "  make clean        - Remove all build artifacts"
	@echo "  make tidy         - Remove build directory only"
	@echo "  make generate     - Generate all code from metadata"
	@echo "  make check-generated - Verify generated code is up-to-date"
	@echo "  make check-docs   - Validate documentation links and anchors"
	@echo "  make check-format - Check C and Python code formatting (no-modify)"
	@echo ""
	@echo "Module targets:"
	@echo "  make generate-modules   - Generate module registration code only"
	@echo "  make validate-modules   - Validate module metadata"
	@echo "  make lint-parameters    - Verify parameter usage matches declarations"
	@echo ""
	@echo "Test targets:"
	@echo "  make tests             - Run all tests"
	@echo "  make tests-unit         - Run unit tests only"
	@echo "  make tests-integration  - Run integration tests only"
	@echo "  make tests-scientific   - Run scientific tests only"
	@echo "  make test-clean                   - Clean test artifacts"
	@echo "  make generate-test-registry - Discover selected tests"
	@echo "  make validate-test-registry - Validate test declarations"
	@echo ""
	@echo "Options:"
	@echo "  Defaults: MODEL=sage SIMULATION=millennium"
	@echo "  make MODEL=sham SIMULATION=millennium  - Build SHAM against Millennium"
	@echo "  make SIM=millennium                    - Shorthand for SIMULATION=<name>"
	@echo "  make USE-HDF5=no                       - Disable HDF5 support"
	@echo "  make USE-MPI=yes                       - Enable MPI support"
	@echo "  make -j4                               - Parallel build"
	@echo ""
	@echo "Tips:"
	@echo "  - Use 'make info' to see detected libraries and configuration"
	@echo "  - Parallel builds significantly speed up compilation: make -j$$(nproc)"
	@echo ""
	@echo "Notes:"
	@echo "  Code is auto-regenerated when YAML metadata changes:"
	@echo ""
	@echo "  Property metadata (simulations/<simulation>/halo_properties.yaml and models/<model>/model_properties.yaml):"
	@echo "    - src/include/generated/property_defs.h"
	@echo "    - src/include/generated/init_*_properties.inc"
	@echo "    - src/include/generated/copy_to_output.inc"
	@echo "    - src/include/generated/hdf5_field_*.inc"
	@echo "    - src/include/generated/output_schema_writer.inc"
	@echo ""
	@echo "  Module metadata (models/<model>/modules/*/module_info.yaml):"
	@echo "    - src/module_system/generated/module_init.c"
	@echo "    - tests/generated/module_sources.mk"

# Show build configuration and detected libraries
info:
	@echo "Mimic Build Configuration"
	@echo "========================="
	@echo ""
	@echo "Compiler: $(CC)"
	@echo "Model set: $(MODEL) ($(MODEL_ROOT))"
	@echo "Simulation: $(SIMULATION) ($(SIMULATION_ROOT))"
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
	@python3 scripts/generate_test_inputs.py

generate-modules:
	@python3 scripts/generate_module_registry.py

generate-test-inputs:
	@python3 scripts/generate_test_inputs.py

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

check-format:
	@echo "Checking C formatting..."
	@find . \( -path ./build -o -path ./mimic_venv -o -path ./sage-code -o -name "generated" \) -prune \
	    -o \( -name "*.c" -o -name "*.h" \) -print \
	    | xargs clang-format --dry-run --Werror
	@echo "Checking Python formatting..."
	@$(PYTHON) -m black --check .
	@$(PYTHON) -m isort --check-only .
	@echo "Format checks passed"

# Test registry generation (auto-discovers core, selected-simulation, and selected-model tests)
generate-test-registry:
	@python3 scripts/generate_test_registry.py
	@python3 scripts/generate_test_inputs.py

validate-test-registry:
	@python3 scripts/validate_module_tests.py

# -----------------------------------------------------------------------------
# Test Targets
# -----------------------------------------------------------------------------

define RUN_PYTHON_TEST_REGISTRY
	@FAILED=0; \
	FAILED_TESTS=""; \
	echo "Running $(1) tests from registry..."; \
	for test in $$(grep -v '^#' build/generated/$(2)_tests.txt | grep -v '^$$'); do \
		echo ""; \
		echo "\033[0;34mRunning: $$test\033[0m"; \
		if ! $(PYTHON) $$test; then \
			FAILED=1; \
			FAILED_TESTS="$$FAILED_TESTS $$test"; \
		fi; \
	done; \
	$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) generate >/dev/null 2>&1 || true; \
	echo ""; \
	if [ $$FAILED -eq 1 ]; then \
		mkdir -p build; \
		for test in $$FAILED_TESTS; do \
			failure="$(2): $$test"; \
			grep -qxF "$$failure" build/.test_failures 2>/dev/null || echo "$$failure" >> build/.test_failures; \
		done; \
		echo "\033[0;31m=== TLDR: $(3) TESTS FAILED ===\033[0m"; \
		echo "\033[0;31mFailed tests:\033[0m"; \
		for test in $$FAILED_TESTS; do \
			echo "  - $$test"; \
		done; \
		echo ""; \
		exit 1; \
	else \
		echo "\033[0;32m=== TLDR: ALL $(3) TESTS PASSED ===\033[0m"; \
		echo ""; \
	fi
endef

tests:
	@echo "Cleaning and building once for all tests..."
	@$(MAKE) clean > /dev/null 2>&1
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) generate-test-registry > /dev/null 2>&1
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) USE-HDF5=yes
	@mkdir -p build
	@rm -f build/.test_failures
	@echo ""
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) check-docs || { grep -qx docs build/.test_failures 2>/dev/null || echo "docs" >> build/.test_failures; true; }
	@echo ""
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) validate-modules || { grep -qx validate-modules build/.test_failures 2>/dev/null || echo "validate-modules" >> build/.test_failures; true; }
	@echo ""
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) tests-unit || { grep -q '^unit:' build/.test_failures 2>/dev/null || grep -qx unit build/.test_failures 2>/dev/null || echo "unit" >> build/.test_failures; true; }
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) tests-integration || { grep -q '^integration:' build/.test_failures 2>/dev/null || grep -qx integration build/.test_failures 2>/dev/null || echo "integration" >> build/.test_failures; true; }
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) tests-scientific || { grep -q '^scientific:' build/.test_failures 2>/dev/null || grep -qx scientific build/.test_failures 2>/dev/null || echo "scientific" >> build/.test_failures; true; }
	@echo ""
	@echo ""
	@if [ -f build/.test_failures ]; then \
		echo "\033[0;31m############################################################\033[0m"; \
		echo "\033[0;31m=== TLDR: FAILED TESTS/SUITES ===\033[0m"; \
		while IFS= read -r failure; do \
			echo "  - $$failure"; \
		done < build/.test_failures; \
		echo "\033[0;31m############################################################\033[0m"; \
	else \
		echo "\033[0;32m############################################################\033[0m"; \
		echo "\033[0;32m=== TLDR: ALL UNIT, INTEGRATION, SCIENTIFIC TESTS PASSED ===\033[0m"; \
		echo "\033[0;32m############################################################\033[0m"; \
	fi
	@echo ""
	@if [ -f build/.test_failures ]; then \
		rm -f build/.test_failures; \
		exit 1; \
	fi

tests-unit:
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING UNIT TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@python3 scripts/generate_test_registry.py --strict
	@python3 scripts/generate_test_inputs.py
	@cd tests/unit && MIMIC_RECORD_TEST_FAILURES=1 ./run_tests.sh

tests-integration:
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) TEST_BUILD=yes generate validate-build $(EXEC)
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING INTEGRATION TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@python3 scripts/generate_test_registry.py --strict
	@python3 scripts/generate_test_inputs.py
	@echo ""
	$(call RUN_PYTHON_TEST_REGISTRY,integration,integration,INTEGRATION)

tests-scientific:
	@$(MAKE) MODEL=$(MODEL) SIMULATION=$(SIMULATION) TEST_BUILD=yes generate validate-build $(EXEC)
	@echo ""
	@echo "\033[0;34m============================================================\033[0m"
	@echo "\033[0;34mRUNNING SCIENTIFIC VALIDATION TESTS\033[0m"
	@echo "\033[0;34m============================================================\033[0m"
	@python3 scripts/generate_test_registry.py --strict
	@python3 scripts/generate_test_inputs.py
	@echo ""
	$(call RUN_PYTHON_TEST_REGISTRY,scientific,scientific,SCIENTIFIC)

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
