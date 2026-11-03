#!/bin/sh
# regenerate.sh -- rebuild the SAGE26 oracle and rewrite every fixture output.
#
# Runs build.sh, then, for every fixture fixtures.manifest lists, runs the
# oracle on models/sage26/modules/<module>/_tests/oracle/<name>.in and writes
# <name>.out beside it. Before running anything it checks that the manifest and
# the fixture inputs on disk agree exactly (every listed input exists, every
# input on disk is listed, and each module's stated count matches its listed
# fixtures); after running it checks that no output lacks an input. Any
# disagreement or oracle failure stops the script with a non-zero exit.
#
# Environment: as build.sh (SAGE26_SRC, ORACLE_OUT, ORACLE_ALLOW_ANY_TOOLCHAIN).
# See README.md in this directory.

set -eu

ORACLE_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$ORACLE_DIR/../../../../.." && pwd)
MODULES_DIR=$REPO_ROOT/models/sage26/modules
MANIFEST=$ORACLE_DIR/fixtures.manifest
ORACLE_OUT=${ORACLE_OUT:-$REPO_ROOT/output/oracle}
export ORACLE_OUT

fail() {
  echo "regenerate.sh: error: $*" >&2
  exit 1
}

[ -f "$MANIFEST" ] || fail "missing $MANIFEST"

# 1. Manifest self-consistency: each "module <m> <n>" count equals the number
#    of "fixture <m> <name>" lines, no fixture is listed twice, and every
#    statement is one of the two forms.
awk '
  /^[[:space:]]*(#|$)/ { next }
  $1 == "module" && NF == 3 { count[$2] = $3; order[++nm] = $2; next }
  $1 == "fixture" && NF == 3 {
    key = $2 " " $3
    if (key in seen) { print "duplicate fixture: " key; bad = 1 }
    seen[key] = 1; listed[$2]++; next
  }
  { print "malformed manifest line " NR ": " $0; bad = 1 }
  END {
    for (m in listed) if (!(m in count)) { print "fixtures listed for undeclared module " m; bad = 1 }
    for (i = 1; i <= nm; i++) {
      m = order[i]
      if (listed[m] + 0 != count[m] + 0) {
        print "module " m ": count " count[m] " but " listed[m] + 0 " fixtures listed"; bad = 1
      }
    }
    exit bad
  }
' "$MANIFEST" || fail "fixtures.manifest is inconsistent"

# 2. Manifest against disk: listed inputs exist, inputs on disk are listed.
listed=$(awk '$1 == "fixture" && NF == 3 { print $2 "/_tests/oracle/" $3 ".in" }' "$MANIFEST" | sort)
on_disk=$(cd "$MODULES_DIR" && find . -path '*/_tests/oracle/*.in' | sed 's|^\./||' | sort)
if [ "$listed" != "$on_disk" ]; then
  echo "listed in fixtures.manifest but missing on disk:" >&2
  printf '%s\n' "$listed" | grep -vxF -e "$on_disk" >&2 || true
  echo "on disk but not listed in fixtures.manifest:" >&2
  printf '%s\n' "$on_disk" | grep -vxF -e "$listed" >&2 || true
  fail "fixtures.manifest and the fixture inputs on disk disagree"
fi

# 3. Build, then rewrite every output from its input.
sh "$ORACLE_DIR/build.sh"
ORACLE=$ORACLE_OUT/oracle_c03

count=0
for input in $listed; do
  "$ORACLE" "$MODULES_DIR/$input" "$MODULES_DIR/${input%.in}.out" ||
    fail "oracle failed on $input"
  count=$((count + 1))
done

# 4. No stale outputs: every output has an input.
stale=$(cd "$MODULES_DIR" && find . -path '*/_tests/oracle/*.out' | sed 's|^\./||; s|\.out$|.in|' |
  sort | grep -vxF -e "$on_disk" || true)
[ -z "$stale" ] || fail "outputs without an input: $stale"

echo "regenerate.sh: wrote $count fixture outputs"
