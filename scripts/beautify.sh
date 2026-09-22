#!/usr/bin/env bash

# beautify.sh - Format C and Python code in the Mimic codebase

# Display help information
show_help() {
    echo "Usage: ./beautify.sh [options]"
    echo ""
    echo "Format C and Python code in the Mimic codebase using industry-standard tools."
    echo ""
    echo "Options:"
    echo "  --help             Display this help message and exit"
    echo "  --c-only           Only format C code (using clang-format)"
    echo "  --py-only          Only format Python code (using black and isort)"
    echo ""
    echo "Requirements (all pinned in requirements.txt and installed into mimic_venv by"
    echo "./scripts/first_run.sh; the venv copy is preferred over any tool on PATH):"
    echo "  - clang-format     For C code formatting"
    echo "  - black            For Python code formatting"
    echo "  - isort            For Python import sorting"
    echo ""
    echo "Exits non-zero if a requested formatter is missing or reports errors."
    echo ""
    exit 0
}

# Process arguments
FORMAT_C=true
FORMAT_PY=true

for arg in "$@"; do
    case $arg in
        --help)
            show_help
            ;;
        --c-only)
            FORMAT_C=true
            FORMAT_PY=false
            ;;
        --py-only)
            FORMAT_C=false
            FORMAT_PY=true
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Check for required tools
check_tool() {
  if ! command -v "$1" &> /dev/null; then
    echo -e "${RED}Error: $1 is not installed or not in PATH${NC}"
    echo "To install: $2"
    return 1
  fi
  return 0
}

# Don't exit on error as we want to try all formatting stages
set +e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"

# Prefer venv-installed formatters: a PATH copy may be an unpinned version whose
# output differs from the one CI enforces.
if [ -f "${ROOT_DIR}/mimic_venv/bin/clang-format" ]; then
    CLANG_FORMAT="${ROOT_DIR}/mimic_venv/bin/clang-format"
else
    CLANG_FORMAT="clang-format"
fi

if [ -x "${ROOT_DIR}/mimic_venv/bin/black" ]; then
    BLACK="${ROOT_DIR}/mimic_venv/bin/black"
else
    BLACK="black"
fi

if [ -x "${ROOT_DIR}/mimic_venv/bin/isort" ]; then
    ISORT="${ROOT_DIR}/mimic_venv/bin/isort"
else
    ISORT="isort"
fi

# A skipped formatter is a failure, not a pass: this is the pre-commit format step.
TOOLS_MISSING=false

# Shared ANSI colour codes (RED/GREEN/YELLOW/BLUE/NC)
# shellcheck source=scripts/lib/colors.sh
. "${ROOT_DIR}/scripts/lib/colors.sh"

BLACK_ERRORS="$(mktemp "${TMPDIR:-/tmp}/mimic_black_errors.XXXXXX")"
ISORT_ERRORS="$(mktemp "${TMPDIR:-/tmp}/mimic_isort_errors.XXXXXX")"
trap 'rm -f "${BLACK_ERRORS}" "${ISORT_ERRORS}"' EXIT

# Print banner
echo -e "${YELLOW}=== Mimic Code Beautifier ===${NC}"

# Format C code
if $FORMAT_C; then
    echo -n "Formatting C code... "
    if check_tool "${CLANG_FORMAT}" "pip install 'clang-format>=20,<21'"; then
        # -exec ... + not `| xargs`: xargs splits paths containing spaces.
        if (cd "${ROOT_DIR}" && find . \( -path ./build -o -path ./mimic_venv -o -path ./sage-code \
                -o -name "generated" \) -prune \
                -o \( -name "*.c" -o -name "*.h" \) \
                -exec "${CLANG_FORMAT}" -i {} +) > /dev/null 2>&1; then
            echo -e "${GREEN}✓${NC}"
        else
            echo -e "${RED}✗${NC}"
            echo -e "${RED}Error formatting C code. See details below:${NC}"
            (cd "${ROOT_DIR}" && find . \( -path ./build -o -path ./mimic_venv -o -path ./sage-code \
                -o -name "generated" \) -prune \
                -o \( -name "*.c" -o -name "*.h" \) \
                -exec "${CLANG_FORMAT}" -i {} +)
        fi
    else
        echo -e "${RED}✗ (tool not found)${NC}"
    fi
fi

# Format Python code
if $FORMAT_PY; then
    # Format with Black
    echo -n "Formatting Python code with Black... "
    if check_tool "${BLACK}" "mimic_venv/bin/pip install -r requirements.txt"; then
        if "${BLACK}" --quiet "${ROOT_DIR}" 2> "${BLACK_ERRORS}"; then
            echo -e "${GREEN}✓${NC}"
        else
            echo -e "${RED}✗${NC}"
            echo -e "${RED}Black encountered errors:${NC}"
            cat "${BLACK_ERRORS}"
        fi
    else
        echo -e "${RED}✗ (tool not found)${NC}"
        TOOLS_MISSING=true
    fi

    # Sort imports with isort
    echo -n "Sorting Python imports with isort... "
    if check_tool "${ISORT}" "mimic_venv/bin/pip install -r requirements.txt"; then
        if "${ISORT}" --profile black --quiet "${ROOT_DIR}" 2> "${ISORT_ERRORS}"; then
            echo -e "${GREEN}✓${NC}"
        else
            echo -e "${RED}✗${NC}"
            echo -e "${RED}isort encountered errors:${NC}"
            cat "${ISORT_ERRORS}"
        fi
    else
        echo -e "${RED}✗ (tool not found)${NC}"
        TOOLS_MISSING=true
    fi
fi

# Check if any errors occurred
if $FORMAT_PY && { [ -s "${BLACK_ERRORS}" ] || [ -s "${ISORT_ERRORS}" ]; }; then
    echo -e "${YELLOW}Some Python files could not be formatted. See errors above.${NC}"
    echo "Tip: For Python 2 files, consider converting to Python 3 with '2to3 -w filename.py'"
    echo "     or manually adding parentheses to print statements."
fi

if $FORMAT_C && ! check_tool "${CLANG_FORMAT}" "" > /dev/null 2>&1; then
    TOOLS_MISSING=true
fi

echo -e "${YELLOW}=== Formatting Complete ===${NC}"

if $TOOLS_MISSING; then
    echo -e "${RED}Formatting INCOMPLETE: a required formatter was not found.${NC}"
    echo "Install the pinned tools: ./scripts/first_run.sh, or"
    echo "  mimic_venv/bin/pip install -r requirements.txt"
    exit 1
fi

if [ -s "${BLACK_ERRORS}" ] || [ -s "${ISORT_ERRORS}" ]; then
    echo -e "${RED}Formatting INCOMPLETE: a formatter reported errors (see above).${NC}"
    exit 1
fi

echo -e "${GREEN}Formatting completed successfully.${NC}"
