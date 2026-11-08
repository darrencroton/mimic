#!/bin/sh
# apply.sh - apply the core-alignment patch series to a scratch worktree.
#
# Run from the root of a clean git worktree on a throwaway branch or a detached HEAD:
#
#     sh path/to/core-alignment/apply.sh
#
# The script refuses (exit 2) when the worktree is not clean, or when its
# branch is `integration/sage26` or `main` (a detached HEAD is allowed). Before
# applying anything it reads `series` and `series.sha256` (next to this script)
# and refuses, naming the line or entry, unless every non-blank line of `series`
# is a bare `NN-<slug>.patch`, every non-blank line of `series.sha256` is
# `<64 lower-case hex>  NN-<slug>.patch`, the two files name the same patches in
# the same order, and each patch file's SHA-256 equals its `series.sha256` line.
# It then applies each patch, in order, with `git apply --index`. If any patch
# fails, the worktree is restored to its starting commit and clean state with
# `git reset --hard HEAD` (safe because it was clean at the start) and the
# script exits 2 naming the patch. Otherwise, provided the number applied equals
# the number of series entries, it prints each applied patch's SHA-256 and exits
# 0. An empty `series` with an empty `series.sha256` applies nothing, says so,
# and exits 0. The script commits nothing.
#
# Both files are read by redirection from the files themselves, never through a
# here-document: a here-document whose temporary file cannot be created would
# read as an empty series.
#
# Exit status: 0 applied (or nothing to apply); 2 refused or failed.

set -u

# --- helpers ---------------------------------------------------------------

NL='
'
CR=$(printf '\r')

die() {
  echo "apply.sh: $*" >&2
  exit 2
}

# Print the SHA-256 of the file named by $1. Fails (status 1, no output) unless
# the hash tool succeeds and yields exactly 64 hexadecimal characters.
sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    hash_out=$(sha256sum "$1") || return 1
  else
    hash_out=$(shasum -a 256 "$1") || return 1
  fi
  digest=${hash_out%% *}
  case "$digest" in
    *[!0-9a-fA-F]*) return 1 ;;
  esac
  [ "${#digest}" -eq 64 ] || return 1
  printf '%s\n' "$digest"
}

# Restore the worktree to its starting commit, then exit 2 naming the patch $1.
# If the restoring reset itself fails, say so: the worktree may hold staged changes.
fail_patch() {
  if git reset --hard HEAD >/dev/null 2>&1; then
    die "patch failed: $1; worktree restored to its starting commit"
  fi
  die "patch failed: $1; restoring the worktree FAILED (git reset --hard HEAD), it may hold staged changes"
}

# Status 0 when $1 is blank: empty, or spaces and tabs only. A carriage return is not blank.
is_blank() {
  case "$1" in
    *[!" 	"]*) return 1 ;;
  esac
  return 0
}

# Status 0 when $1 is a bare NN-<slug>.patch: NN two digits, <slug> one or more
# lower-case letters, digits and hyphens. Explicit character lists, not ranges,
# so the test does not depend on the locale's collation.
is_patch_name() {
  case "$1" in
    [0123456789][0123456789]-?*.patch) ;;
    *) return 1 ;;
  esac
  slug=${1#??-}
  slug=${slug%.patch}
  case "$slug" in
    '' | *[!abcdefghijklmnopqrstuvwxyz0123456789-]*) return 1 ;;
  esac
  return 0
}

# Print line $1 for a message: a line holding a carriage return is printed without it and
# marked, so the message stays on one visible line.
show_line() {
  case "$1" in
    *"$CR"*) printf '%s [contains a carriage return]' "$(printf '%s' "$1" | tr -d '\r')" ;;
    *) printf '%s' "$1" ;;
  esac
}

# --- preconditions ---------------------------------------------------------

[ "$#" -eq 0 ] || die "takes no arguments"

if ! command -v sha256sum >/dev/null 2>&1 && ! command -v shasum >/dev/null 2>&1; then
  die "neither sha256sum nor shasum is available"
fi

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd -P) || die "cannot locate the script directory"
SERIES="$SCRIPT_DIR/series"
DIGESTS="$SCRIPT_DIR/series.sha256"
[ -f "$SERIES" ] || die "series file not found: $SERIES"
[ -f "$DIGESTS" ] || die "digest file not found: $DIGESTS"

TOPLEVEL=$(git rev-parse --show-toplevel 2>/dev/null) || die "not inside a git worktree"
[ "$TOPLEVEL" = "$(pwd -P)" ] || die "run from the root of the worktree ($TOPLEVEL)"

# Compare the full ref, so a tag or other ref of the same name cannot hide the branch.
# git symbolic-ref -q exits 1 only for a detached HEAD (allowed); any other failure is an error.
REF=$(git symbolic-ref -q HEAD)
ref_status=$?
case "$ref_status" in
  0)
    case "$REF" in
      refs/heads/integration/sage26 | refs/heads/main)
        die "refusing to run on branch '${REF#refs/heads/}'"
        ;;
    esac
    ;;
  1) ;;
  *) die "cannot determine the branch: git symbolic-ref -q HEAD exited $ref_status" ;;
esac

# Independent of the user's status.showUntrackedFiles and submodule settings; docs/dev
# is excluded (the campaign's access rule).
STATUS=$(git status --porcelain --untracked-files=all --ignore-submodules=none -- . ':(exclude)docs/dev') ||
  die "git status failed"
[ -z "$STATUS" ] || die "worktree is not clean; commit or discard changes first"

# --- read and check series and series.sha256 -------------------------------

# Read each file once, checked: an unreadable file is an error, not an empty one.
[ -r "$SERIES" ] || die "series file is not readable: $SERIES"
[ -r "$DIGESTS" ] || die "digest file is not readable: $DIGESTS"

# series: one bare patch name per non-blank line.
SERIES_LIST=""
n_series=0
lineno=0
while IFS= read -r line || [ -n "$line" ]; do
  lineno=$((lineno + 1))
  is_blank "$line" && continue
  is_patch_name "$line" ||
    die "series line $lineno is not a bare NN-<slug>.patch: '$(show_line "$line")'"
  SERIES_LIST="$SERIES_LIST$line$NL"
  n_series=$((n_series + 1))
done <"$SERIES" || die "cannot read the series file: $SERIES"

# series.sha256: `<64 lower-case hex>  <bare patch name>` per non-blank line.
DIGEST_LIST=""
n_digests=0
lineno=0
while IFS= read -r line || [ -n "$line" ]; do
  lineno=$((lineno + 1))
  is_blank "$line" && continue
  want=${line%%" "*}
  name=${line#"$want  "}
  case "$want" in
    *[!0123456789abcdef]*) want="" ;;
  esac
  if [ "${#want}" -ne 64 ] || [ "$line" != "$want  $name" ] || ! is_patch_name "$name"; then
    die "series.sha256 line $lineno is not '<64 lower-case hex>  NN-<slug>.patch': '$(show_line "$line")'"
  fi
  DIGEST_LIST="$DIGEST_LIST$line$NL"
  n_digests=$((n_digests + 1))
done <"$DIGESTS" || die "cannot read the digest file: $DIGESTS"

[ "$n_series" -eq "$n_digests" ] ||
  die "series names $n_series patch(es) but series.sha256 has $n_digests line(s); nothing applied"

# Same patches, same order, and each patch file's SHA-256 equal to its line, before
# anything is applied. Names and digest lines hold no glob or IFS characters other
# than the two spaces, which splitting on newlines alone keeps.
set -f
OLD_IFS=$IFS
IFS=$NL
i=0
for name in $SERIES_LIST; do
  i=$((i + 1))
  j=0
  entry=""
  for d in $DIGEST_LIST; do
    j=$((j + 1))
    if [ "$j" -eq "$i" ]; then
      entry=$d
      break
    fi
  done
  want=${entry%%" "*}
  [ "${entry#"$want  "}" = "$name" ] ||
    die "series entry $i ($name) does not match series.sha256 line $i ($entry); nothing applied"
  patch="$SCRIPT_DIR/$name"
  [ -f "$patch" ] || die "series entry $i ($name): file not found; nothing applied"
  got=$(sha256_of "$patch") || die "series entry $i ($name): could not compute its SHA-256; nothing applied"
  [ "$got" = "$want" ] ||
    die "series entry $i ($name): SHA-256 $got does not equal series.sha256's $want; nothing applied"
done
IFS=$OLD_IFS
set +f

# --- apply the series ------------------------------------------------------

count=0
report=""
set -f
IFS=$NL
for name in $SERIES_LIST; do
  patch="$SCRIPT_DIR/$name"
  git apply --index "$patch" </dev/null || fail_patch "$name"
  digest=$(sha256_of "$patch") || fail_patch "$name (could not compute its SHA-256)"
  count=$((count + 1))
  report="$report$digest  $name
"
done
IFS=$OLD_IFS
set +f

# Every series entry must have been applied; anything else is refused and restored.
if [ "$count" -ne "$n_series" ]; then
  if git reset --hard HEAD >/dev/null 2>&1; then
    die "applied $count of $n_series series entries; worktree restored to its starting commit"
  fi
  die "applied $count of $n_series series entries; restoring the worktree FAILED (git reset --hard HEAD)"
fi

if [ "$count" -eq 0 ]; then
  echo "apply.sh: series is empty; nothing applied"
  exit 0
fi

echo "apply.sh: applied $count patch(es), staged in the index, nothing committed:"
printf '%s' "$report"
exit 0
