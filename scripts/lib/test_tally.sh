#!/bin/sh
# scripts/lib/test_tally.sh — Per-case outcome counting and live-or-summary test running.
#
# Source this file (after scripts/lib/colors.sh) from the Makefile test recipes and
# tests/unit/run_tests.sh; do not execute it. Plain POSIX sh, because the Makefile
# recipes run under /bin/sh.
#
# Every tier counts the same way, from the MIMIC_RESULT: markers (tests/framework/markers.py)
# rather than from test files, so a tier's counts line reports test cases:
#
#   passed   PASS and WARN markers (a WARN is a pass, as in tests/framework/runner.py)
#   failed   FAIL and ERROR markers, plus one for every run that exited non-zero without
#            emitting either (a crash, compile failure or import error never reads as zero)
#   skipped  SKIP markers: the case applies here but could not run
#   n/a      NA markers: the case does not apply to the selected MODEL/SIMULATION pair
#
# After sourcing, the following are available:
#   tally_log LOG STATUS     — add LOG's markers (and the exit STATUS of the run) to the tally
#   run_tallied CMD [ARG...] — run CMD, show its output per TEST_SUMMARY, tally it, and
#                              return its exit status; TALLY_FAIL_NAMES then holds the names
#                              of the FAIL and ERROR markers it emitted
#   print_tally_line LABEL   — print "<Label> Test Summary: passed=N failed=N skipped=N n/a=N"
#                              (LABEL in any case)
#   TALLY_PASSED, TALLY_FAILED, TALLY_SKIPPED, TALLY_NA, TALLY_WARNED, TALLY_COMPILE_ERRORS
#                            — the running counts; the unit runner adds its own compile errors

# Read by the sourcing scripts, so their uses are not visible here.
# shellcheck disable=SC2034
TALLY_PASSED=0
TALLY_FAILED=0
TALLY_SKIPPED=0
TALLY_NA=0
TALLY_WARNED=0
TALLY_COMPILE_ERRORS=0
TALLY_FAIL_NAMES=""

# tally_log LOG STATUS: add one run's markers to the tally.
tally_log() {
    # The awk fields are: PASS, WARN, FAIL+ERROR, SKIP, NA (all numeric, so the
    # unquoted expansion into positional parameters is safe).
    _tally_rc=$2
    # shellcheck disable=SC2046
    set -- $(awk '/^MIMIC_RESULT: / { n[$2]++ }
        END { printf "%d %d %d %d %d", n["PASS"], n["WARN"], n["FAIL"] + n["ERROR"],
              n["SKIP"], n["NA"] }' "$1")
    TALLY_PASSED=$((TALLY_PASSED + $1 + $2))
    TALLY_WARNED=$((TALLY_WARNED + $2))
    TALLY_FAILED=$((TALLY_FAILED + $3))
    TALLY_SKIPPED=$((TALLY_SKIPPED + $4))
    TALLY_NA=$((TALLY_NA + $5))
    if [ "$_tally_rc" -ne 0 ] && [ "$3" -eq 0 ]; then
        TALLY_FAILED=$((TALLY_FAILED + 1))
    fi
}

# run_tallied CMD [ARG...]: run one test command and tally its markers.
#
# Summary mode (TEST_SUMMARY=1) shows only the FAIL/SKIP/WARN/ERROR markers, or the whole
# log when a failed run emitted no markers at all (a crash or import error). Full mode
# streams the output live through tee; the exit status travels through a file because
# /bin/sh has no pipefail, and MIMIC_FORCE_COLOR tells a Python test (tests/framework/runner.py)
# that the pipe ends at a terminal, so it keeps its colour. Returns the command's exit status.
run_tallied() {
    _tally_log=$(mktemp)
    if [ "${TEST_SUMMARY:-0}" = "1" ]; then
        "$@" > "$_tally_log" 2>&1
        _tally_status=$?
        if grep -q "^MIMIC_RESULT:" "$_tally_log"; then
            grep "^MIMIC_RESULT: \(FAIL\|SKIP\|WARN\|ERROR\)" "$_tally_log" || true
        elif [ "$_tally_status" -ne 0 ]; then
            cat "$_tally_log"
        fi
    else
        _tally_status_file=$(mktemp)
        _tally_color=""
        if [ -t 1 ] && [ -z "${NO_COLOR+x}" ]; then
            _tally_color=1
        fi
        # Unbuffered so a Python test's output keeps streaming through the pipe.
        (PYTHONUNBUFFERED=1 MIMIC_FORCE_COLOR=$_tally_color "$@" 2>&1
            echo $? > "$_tally_status_file") | tee "$_tally_log"
        # An interrupted run never writes its status: count it as a failure.
        _tally_status=$(cat "$_tally_status_file" 2>/dev/null)
        _tally_status=${_tally_status:-1}
        rm -f "$_tally_status_file"
    fi
    TALLY_FAIL_NAMES=$(awk '/^MIMIC_RESULT: (FAIL|ERROR) / { print $3 }' "$_tally_log")
    tally_log "$_tally_log" "$_tally_status"
    rm -f "$_tally_log"
    return "$_tally_status"
}

# print_tally_line LABEL: the one-line counts summary shown by every tier. Needs the
# colour variables from colors.sh; skipped and warned are coloured only when non-zero.
print_tally_line() {
    _tally_label=$(printf '%s' "$1" |
        awk '{ print toupper(substr($0, 1, 1)) tolower(substr($0, 2)) }')
    printf "%s Test Summary: passed=${GREEN}%s${NC}" "$_tally_label" "$TALLY_PASSED"
    if [ "$TALLY_FAILED" -gt 0 ]; then
        printf " failed=${RED}%s${NC}" "$TALLY_FAILED"
    else
        printf " failed=%s" "$TALLY_FAILED"
    fi
    if [ "$TALLY_SKIPPED" -gt 0 ]; then
        printf " skipped=${YELLOW}%s${NC}" "$TALLY_SKIPPED"
    else
        printf " skipped=%s" "$TALLY_SKIPPED"
    fi
    printf " n/a=%s" "$TALLY_NA"
    if [ "$TALLY_WARNED" -gt 0 ]; then
        printf " warned=${YELLOW}%s${NC}" "$TALLY_WARNED"
    fi
    if [ "$TALLY_COMPILE_ERRORS" -gt 0 ]; then
        printf " compile_errors=${YELLOW}%s${NC}" "$TALLY_COMPILE_ERRORS"
    fi
    printf "\n"
}
