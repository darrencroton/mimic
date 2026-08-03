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
    echo "Requirements:"
    echo "  - clang-format     For C code formatting (install with 'brew install clang-format')"
    echo "  - black            For Python code formatting (install with 'pip install black')"
    echo "  - isort            For Python import sorting (install with 'pip install isort')"
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

# Prefer venv-installed clang-format (version-pinned via requirements.txt)
if [ -f "${ROOT_DIR}/mimic_venv/bin/clang-format" ]; then
    CLANG_FORMAT="${ROOT_DIR}/mimic_venv/bin/clang-format"
else
    CLANG_FORMAT="clang-format"
fi

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
    if check_tool black "pip install black"; then
        if black --quiet "${ROOT_DIR}" 2> "${BLACK_ERRORS}"; then
            echo -e "${GREEN}✓${NC}"
        else
            echo -e "${RED}✗${NC}"
            echo -e "${RED}Black encountered errors:${NC}"
            cat "${BLACK_ERRORS}"
        fi
    else
        echo -e "${RED}✗ (tool not found)${NC}"
    fi

    # Sort imports with isort
    echo -n "Sorting Python imports with isort... "
    if check_tool isort "pip install isort"; then
        if isort --profile black --quiet "${ROOT_DIR}" 2> "${ISORT_ERRORS}"; then
            echo -e "${GREEN}✓${NC}"
        else
            echo -e "${RED}✗${NC}"
            echo -e "${RED}isort encountered errors:${NC}"
            cat "${ISORT_ERRORS}"
        fi
    else
        echo -e "${RED}✗ (tool not found)${NC}"
    fi
fi

# Check if any errors occurred
if $FORMAT_PY; then
    if command -v black &> /dev/null && command -v isort &> /dev/null; then
        if [ -s "${BLACK_ERRORS}" ] || [ -s "${ISORT_ERRORS}" ]; then
            echo -e "${YELLOW}Some Python files could not be formatted. See errors above.${NC}"
            echo "Tip: For Python 2 files, consider converting to Python 3 with '2to3 -w filename.py'"
            echo "     or manually adding parentheses to print statements."
        fi
    elif [ "$FORMAT_C" = true ] && [ "$FORMAT_PY" = true ]; then
        echo -e "${YELLOW}Note: Python formatting tools were not available.${NC}"
        echo "To install: pip install black isort"
    fi
fi

if $FORMAT_C && ! command -v clang-format &> /dev/null && [ "$FORMAT_PY" = true ]; then
    echo -e "${YELLOW}Note: C formatting tool (clang-format) was not available.${NC}"
    echo "To install: brew install clang-format"
fi

if { $FORMAT_C && command -v clang-format &> /dev/null; } || \
   { $FORMAT_PY && command -v black &> /dev/null && command -v isort &> /dev/null && [ ! -s "${BLACK_ERRORS}" ] && [ ! -s "${ISORT_ERRORS}" ]; }; then
    echo -e "${GREEN}Formatting completed successfully for all available tools!${NC}"
fi

echo -e "${YELLOW}=== Formatting Complete ===${NC}"
