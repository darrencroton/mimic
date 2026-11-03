#!/bin/sh
# apply.sh - apply the core-alignment patch series to a scratch worktree.
#
# Run from the root of a clean git worktree on a throwaway branch:
#
#     sh path/to/core-alignment/apply.sh
#
# The script refuses (exit 2) when the worktree is not clean, or when its
# branch is `integration/sage26` or `main`. It then applies each patch named in
# `series` (next to this script), in order, with `git apply --index`. If any
# patch fails, the worktree is restored to its starting commit and clean state
# with `git reset --hard HEAD` (safe because it was clean at the start) and the
# script exits 2 naming the patch. Otherwise it prints each applied patch's
# SHA-256 and exits 0. An empty `series` applies nothing, says so, and exits 0.
# The script commits nothing.
#
# Exit status: 0 applied (or nothing to apply); 2 refused or failed.

set -u

# --- helpers ---------------------------------------------------------------

die() {
  echo "apply.sh: $*" >&2
  exit 2
}

# Print the SHA-256 of the file named by $1.
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

# Restore the worktree to its starting commit, then exit 2 naming the patch $1.
fail_patch() {
  git reset --hard HEAD >/dev/null 2>&1 || echo "apply.sh: git reset --hard HEAD failed" >&2
  die "patch failed: $1; worktree restored to its starting commit"
}

# --- preconditions ---------------------------------------------------------

[ "$#" -eq 0 ] || die "takes no arguments"

if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
  die "neither sha256sum nor shasum is available"
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd -P) || die "cannot locate the script directory"
SERIES="$SCRIPT_DIR/series"
[ -f "$SERIES" ] || die "series file not found: $SERIES"

TOPLEVEL=$(git rev-parse --show-toplevel 2>/dev/null) || die "not inside a git worktree"
[ "$TOPLEVEL" = "$(pwd -P)" ] || die "run from the root of the worktree ($TOPLEVEL)"

BRANCH=$(git symbolic-ref --short -q HEAD || true)
case "$BRANCH" in
  integration/sage26 | main)
    die "refusing to run on branch '$BRANCH'"
    ;;
esac

STATUS=$(git status --porcelain) || die "git status failed"
[ -z "$STATUS" ] || die "worktree is not clean; commit or discard changes first"

# --- apply the series ------------------------------------------------------

count=0
report=""
while IFS= read -r name || [ -n "$name" ]; do
  # Blank and whitespace-only lines name no patch.
  case "$name" in
    *[![:space:]]*) ;;
    *) continue ;;
  esac
  # A patch is a file next to this script; never a path.
  case "$name" in
    */*) fail_patch "$name (a series entry must be a bare file name)" ;;
  esac
  patch="$SCRIPT_DIR/$name"
  [ -f "$patch" ] || fail_patch "$name (file not found)"
  git apply --index "$patch" </dev/null || fail_patch "$name"
  count=$((count + 1))
  report="$report$(sha256_of "$patch")  $name
"
done <"$SERIES"

if [ "$count" -eq 0 ]; then
  echo "apply.sh: series is empty; nothing applied"
  exit 0
fi

echo "apply.sh: applied $count patch(es), staged in the index, nothing committed:"
printf '%s' "$report"
exit 0
