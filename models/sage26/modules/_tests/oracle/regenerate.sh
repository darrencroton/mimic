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
# Fixtures of the c04 modules (C04_MODULES below) run through oracle_c04;
# every other fixture runs through oracle_c03, except that a fixture whose
# routine line names a c05 routine (C05_ROUTINES below) runs through oracle_c05,
# whichever module owns it (sage26_finalize_cooling_heating holds both c04 and
# c05 fixtures; c09's starformation_and_feedback_fire is a c05 routine), a fixture whose routine line names a c06 routine
# (C06_ROUTINES below) runs through oracle_c06, one whose routine line names
# a c07 routine (C07_ROUTINES below) runs through oracle_c07, one whose
# routine line names a c08 routine (C08_ROUTINES below) runs through oracle_c08,
# one whose routine line names a c10 routine (C10_ROUTINES below) runs
# through oracle_c10, and one whose routine line names a c11 routine
# (C11_ROUTINES below) runs through oracle_c11, although the c11 fixtures are
# owned by the c04 modules sage26_calculate_cooling_budget, sage26_radio_mode_heating
# and sage26_finalize_cooling_heating (the writer conversion) as well as by
# sage26_regime_aware_cooling (the regime-aware wrapper). A fixture whose routine line
# names a c12 routine (C12_ROUTINES below) runs through oracle_c12, and one whose routine line
# names a c13 routine (C13_ROUTINES below) through oracle_c13.
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
# The c04 group (cooling and radio heating) has its own oracle main.
ORACLE_C04=$ORACLE_OUT/oracle_c04
C04_MODULES="sage26_calculate_cooling_budget sage26_radio_mode_heating sage26_apply_cooling
sage26_finalize_cooling_heating"
# The c05 group (star formation, feedback, instability, histories, outflow) has
# its own oracle main and is selected by routine, not by module.
ORACLE_C05=$ORACLE_OUT/oracle_c05
C05_ROUTINES="starformation_and_feedback starformation_and_feedback_fire
starformation_and_feedback_sequence update_from_star_formation update_from_feedback check_disk_instability step_bin_map
sfr_history_average normalise_outflow_rate check_disk_instability_fire"
# The c06 group (merger clocks, mergers, black holes, bursts, disruption and the
# driver transcriptions) has its own oracle main and is selected by routine.
ORACLE_C06=$ORACLE_OUT/oracle_c06
C06_ROUTINES="estimate_merging_time grow_black_hole quasar_mode_wind
collisional_starburst_recipe add_galaxies_together make_bulge_from_burst
deal_with_galaxy_merger disrupt_satellite_to_ICS merger_pass join_progenitors join_and_pass
collisional_starburst_recipe_fire deal_with_galaxy_merger_fire merger_pass_fire"
# The c07 group (the bulge-size routines and the central's synchronisation
# transcription) has its own oracle main and is selected by routine.
ORACLE_C07=$ORACLE_OUT/oracle_c07
C07_ROUTINES="get_bulge_radius update_instability_bulge_radius central_disk_and_bulge_radius"
# The c08 group (the BR06 molecular fraction and its radial integration, called
# directly) has its own oracle main and is selected by routine.
ORACLE_C08=$ORACLE_OUT/oracle_c08
C08_ROUTINES="calculate_molecular_fraction_BR06 calculate_molecular_fraction_radial_integration"
# The c10 group (the CGM regime classifier, called directly with the fixture's rand()
# queue) has its own oracle main and is selected by routine.
ORACLE_C10=$ORACLE_OUT/oracle_c10
C10_ROUTINES="determine_and_store_regime"
# The c11 group (the clustering-mass table, the CGMrecipeOn 1 hot recipe, bulk CGM cooling, the
# CGM-on radio AGN and the regime-aware wrapper, each called directly, and the writer's mdot
# conversion, transcribed) has its own oracle main and is selected by routine.
ORACLE_C11=$ORACLE_OUT/oracle_c11
C11_ROUTINES="interpolate_clustering_mass cooling_recipe_hot_streams cooling_recipe_cgm
do_AGN_heating_cgm_on cooling_recipe_regime_aware writer_mdot_conversion"
# The c12 group (the halo concentration: the Vmax/Vvir inversion and get_halo_concentration's
# ConcentrationOn 0 and 3 dispatch, each called directly) has its own oracle main and is
# selected by routine.
ORACLE_C12=$ORACLE_OUT/oracle_c12
C12_ROUTINES="concentration_from_vmax_vvir get_halo_concentration"
# The c13 group (the feedback-free-burst classifier: the threshold, the fraction and
# determine_and_store_ffb_regime with the fixture's rand() queue, each called directly) has its
# own oracle main and is selected by routine.
ORACLE_C13=$ORACLE_OUT/oracle_c13
C13_ROUTINES="calculate_ffb_threshold_mass calculate_ffb_fraction determine_and_store_ffb_regime"

# Each oracle run writes to a fresh temporary file under ORACLE_OUT, which
# replaces the committed output only if the oracle exited 0 AND the file is a
# complete fixture output. The exit status alone is not enough: SAGE26's
# ABORT(0) (R/src/macros.h), which read_cooling_functions() uses on a missing or
# malformed cooling table, exits with status 0 and writes nothing, and would
# otherwise leave the old output in place while this script reports it written.
TMP_OUT=$(mktemp -d "$ORACLE_OUT/regenerate.XXXXXX")
trap 'rm -rf "${TMP_OUT:?}"' EXIT

# A complete output: the oracle's header, format and routine lines, at least
# one field, every later line a typed field (f32, f64, i32 or, since c13, the
# decimal signed 64-bit i64), and a final newline.
complete_output() {
  [ -s "$1" ] && [ "$(tail -c 1 "$1" | od -An -c | tr -d ' ')" = '\n' ] &&
    awk '
      NR == 1 { ok = ($0 ~ /^# SAGE26 oracle output for /); next }
      NR == 2 { ok = ok && ($0 == "format sage26-oracle-fixture 1"); next }
      NR == 3 { ok = ok && ($0 ~ /^routine [A-Za-z_0-9]+$/); next }
      { ok = ok && ($0 ~ /^(f32|f64|i32|i64) [^ ]+ [^ ]+/); fields++ }
      END { exit !(ok && fields > 0) }
    ' "$1"
}

count=0
for input in $listed; do
  oracle=$ORACLE
  for module in $C04_MODULES; do
    case $input in "$module"/*) oracle=$ORACLE_C04 ;; esac
  done
  routine=$(awk '$1 == "routine" { print $2; exit }' "$MODULES_DIR/$input")
  for name in $C05_ROUTINES; do
    if [ "$routine" = "$name" ]; then
      oracle=$ORACLE_C05
    fi
  done
  for name in $C06_ROUTINES; do
    if [ "$routine" = "$name" ]; then
      oracle=$ORACLE_C06
    fi
  done
  for name in $C07_ROUTINES; do
    if [ "$routine" = "$name" ]; then
      oracle=$ORACLE_C07
    fi
  done
  for name in $C08_ROUTINES; do
    if [ "$routine" = "$name" ]; then
      oracle=$ORACLE_C08
    fi
  done
  for name in $C10_ROUTINES; do
    if [ "$routine" = "$name" ]; then
      oracle=$ORACLE_C10
    fi
  done
  for name in $C11_ROUTINES; do
    if [ "$routine" = "$name" ]; then
      oracle=$ORACLE_C11
    fi
  done
  for name in $C12_ROUTINES; do
    if [ "$routine" = "$name" ]; then
      oracle=$ORACLE_C12
    fi
  done
  for name in $C13_ROUTINES; do
    if [ "$routine" = "$name" ]; then
      oracle=$ORACLE_C13
    fi
  done
  tmp=$TMP_OUT/output
  rm -f "${tmp:?}"
  "$oracle" "$MODULES_DIR/$input" "$tmp" ||
    fail "oracle failed on $input"
  complete_output "$tmp" ||
    fail "oracle exited 0 but wrote no complete output for $input (old output left untouched)"
  mv -f "$tmp" "$MODULES_DIR/${input%.in}.out"
  count=$((count + 1))
done

# 4. No stale outputs: every output has an input.
stale=$(cd "$MODULES_DIR" && find . -path '*/_tests/oracle/*.out' | sed 's|^\./||; s|\.out$|.in|' |
  sort | grep -vxF -e "$on_disk" || true)
[ -z "$stale" ] || fail "outputs without an input: $stale"

echo "regenerate.sh: wrote $count fixture outputs"
