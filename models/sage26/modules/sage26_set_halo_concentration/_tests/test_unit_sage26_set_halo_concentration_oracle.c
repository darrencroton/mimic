/**
 * @file    test_unit_sage26_set_halo_concentration_oracle.c
 * @brief   Bit-exact comparison of shared/sage_halo_concentration.h with SAGE26's
 *          concentration_from_vmax_vvir and get_halo_concentration, and of the module's
 *          assignment with SAGE26's progenitor joining
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_set_halo_concentration
 * (models/sage26/modules/sage26_set_halo_concentration/_tests/oracle/; the inv_* and ghc_*
 * written by oracle_c12, the tr_join_* by oracle_c06): the inv_* and ghc_* through the shared
 * helpers the module calls, directly, and the tr_join_* through the joining harness:
 *
 * - inv_* fixtures (routine concentration_from_vmax_vvir): each case's double Vmax and Vvir into
 *   mimic_sage_concentration_from_vmax_vvir;
 * - ghc_* fixtures (routine get_halo_concentration): the fixture's ConcentrationOn and each
 *   galaxy's Type (its Type at the call) and four float velocities into
 *   mimic_sage_get_halo_concentration.
 *
 * Per case it asserts the double return, the float store SAGE26's joining makes of it (where the
 * fixture records one: ConcentrationOn > 0), the guard and ratio flags, the step count, the
 * selected pair and every recorded local (ratio_sq, the final c_lo and c_hi, the selected vmax and
 * vvir) bit for bit, which the helpers report; checks each local's markers (present or absent as in
 * the port, checked, of the recorded check kind, `_validated` the conjunction of its two
 * directions); asserts each bracket end recorded at its initial bound equal to that bound; and
 * prints its tallies. The redshift input is consumed unused: neither mode reads it, in SAGE26 or
 * the package. The transcription's tie observations (`ties`, `first_tie_step`) are consumed
 * unasserted: they are not SAGE26 or package state.
 *
 * - tr_join_* fixtures (routine join_progenitors, written by oracle_c06 with ConcentrationOn 3:
 *   SAGE26's joining with its own get_halo_concentration at R/src/core_build_model.c:290-295): the
 *   joining harness (modules/_tests/sage26_oracle_join.h) joins the fixture's progenitor
 *   galaxies with the core's inheritance service (inherit_descendant_halos) and runs
 *   the shipped pre_timestep order, sage26_initialise_merger_clock, this module and
 *   sage26_set_disk_scale_radius, each initialised from the fixture (ConcentrationOn 3, BulgeSizeOn
 *   3 and R5's cosmology). Every retained galaxy's recorded members are asserted bit for bit:
 *   Concentration; the merger-family block's reservoirs, radii and channels; the histories;
 *   MergTime; Type, SnapNum, HaloNr, CentralGal and Len; the core's halo values and infall record
 *   (SAGE26's floats against the core's doubles narrowed); infallStellarMass and TimeOfInfall (the
 *   private state properties that hold SAGE26's members); Cooling and Heating. Excluded by name,
 *   with the merger-family group's reasons (the resolver's and the clock's oracle tests): on a
 *   row off SAGE26's main-branch reset (a new orphan, or a former Type 2 that B:240 passes over),
 *   QuasarModeBHaccretionMass, OutflowRate, Cooling and Heating (SAGE26 keeps them, B:273-276 run
 *   on the main branch only; the core's init_repeat resets them on every inherited row); on a new
 *   orphan, Len (SAGE26 keeps it, B:261; the core zeros it); and the infall halo values of a Type
 *   1 with a clock above 999 that loses its subhalo (none here). mergeType is checked to be 0 (no
 *   package member); mergeIntoID and dT are SAGE26 bookkeeping with no package member at joining
 *   (B:237, :287, :304). A join_progenitors fixture carries no pass timing, so the test gives
 *   the harness's full-state path a zero halo age and progenitor ages (add_zero_timing): SAGE26's
 *   joining and the package's joining modules read no time (B:237 sets dT to -1, and the core's dT
 *   is the excluded bookkeeping). Each path has a wiring assertion of its own: a new Type 0 with
 *   PreviousType 0 holds the inversion of its current float pair, one with PreviousType 1 that of
 *   its infall pair (and differs from the current pair's), and every other row its input value;
 *   the oracle's concentration_assigned marks exactly the assigned rows. On a subhalo join the
 *   harness's stand-in FoF central (row 0, not a fixture galaxy) is neither recorded nor compared.
 *
 * Outside the oracle environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/galaxy_pool.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_join.h"
#include "shared/sage_halo_concentration.h"

extern int sage26_set_halo_concentration_init(void);
extern int sage26_set_halo_concentration_cleanup(void);
extern int sage26_initialise_merger_clock_init(void);
extern int sage26_initialise_merger_clock_cleanup(void);
extern int sage26_set_disk_scale_radius_init(void);
extern int sage26_set_disk_scale_radius_cleanup(void);

static const char *MODULE = "sage26_set_halo_concentration";

#define MAX_CASES 24
#define CHECK_CONTINUATION 1
#define CHECK_TAIL 2

/** Tallies of the recorded locals over one test run */
struct c12_tally {
  int cases;
  int continuation;      /* present and continuation-checked */
  int tail;              /* present and tail-checked */
  int validated;         /* validated two-sided, a float store changing in a direction */
  int narrowing_limited; /* validated two-sided through the double return only */
  int not_validated;     /* checked, not validated two-sided */
  int binding;           /* a bracket end at its initial bound, asserted equal to it */
  int absent;            /* not formed on the case's path */
};

static struct c12_tally tally;

/** Tallies of the joining fixtures' rows by the module's path */
struct c12_join_tally {
  int fixtures;
  int rows;
  int current;          /* new Type 0, PreviousType 0: the current pair */
  int infall;           /* new Type 0, PreviousType 1: the infall pair */
  int kept;             /* every other row: the input value */
  int orphaned;         /* rows joining made an orphan */
  int reset_exclusions; /* members excluded on rows off SAGE26's main-branch reset */
};

static struct c12_join_tally join_tally;

/**
 * @brief Check one recorded local: its markers, and its value bit for bit
 * @return Number of mismatches and marker errors
 */
static int check_local(struct oracle_fixture *out, const char *prefix, const char *name,
                       const int kind, const int absent_in_port, const double value) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "%s.%s_absent", prefix, name);
  const int absent = oracle_in_i32(out, field);
  if (absent != absent_in_port) {
    fprintf(stderr, "oracle: %s: %s.%s is %s in the oracle but %s in the port\n", out->path, prefix,
            name, absent ? "absent" : "present", absent_in_port ? "absent" : "present");
    return 1;
  }
  if (absent) {
    tally.absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "%s.%s_checked", prefix, name);
  if (oracle_in_i32(out, field) != 1) {
    fprintf(stderr, "oracle: %s: %s.%s is not marked checked\n", out->path, prefix, name);
    problems++;
  }
  snprintf(field, sizeof(field), "%s.%s_check_kind", prefix, name);
  if (oracle_in_i32(out, field) != kind) {
    fprintf(stderr, "oracle: %s: %s.%s is not of check kind %d\n", out->path, prefix, name, kind);
    problems++;
  }
  if (kind == CHECK_CONTINUATION) {
    tally.continuation++;
  } else {
    tally.tail++;
  }
  snprintf(field, sizeof(field), "%s.%s_validated_up", prefix, name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_validated_down", prefix, name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_validated", prefix, name);
  const int validated = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_store_changes_up", prefix, name);
  const int store_up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_store_changes_down", prefix, name);
  const int store_down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.%s_narrowing_limited", prefix, name);
  const int narrowing = oracle_in_i32(out, field);
  if (validated != (up && down) || narrowing != (validated && !store_up && !store_down) ||
      (store_up && !up) || (store_down && !down)) {
    fprintf(stderr, "oracle: %s: %s.%s's validation markers disagree with each other\n", out->path,
            prefix, name);
    problems++;
  }
  if (narrowing) {
    tally.narrowing_limited++;
  } else if (validated) {
    tally.validated++;
  } else {
    tally.not_validated++;
  }
  snprintf(field, sizeof(field), "%s.%s", prefix, name);
  problems += oracle_check_f64(out, field, value);
  return problems;
}

/**
 * @brief Check an inversion's flags, step count, locals and bound markers under `prefix`
 * @return Number of mismatches and marker errors
 */
static int check_inversion(struct oracle_fixture *out, const char *prefix,
                           const struct MimicSageConcentrationLocals *l) {
  char field[ORACLE_MAX_NAME];
  int problems = 0;
  snprintf(field, sizeof(field), "%s.guard_returned", prefix);
  problems += oracle_check_i32(out, field, l->guard_returned);
  snprintf(field, sizeof(field), "%s.below_one", prefix);
  problems += oracle_check_i32(out, field, l->below_one);
  snprintf(field, sizeof(field), "%s.iterations", prefix);
  problems += oracle_check_i32(out, field, l->iterations);
  problems += check_local(out, prefix, "ratio_sq", CHECK_TAIL, l->ratio_sq_absent, l->ratio_sq);
  problems += check_local(out, prefix, "c_lo", CHECK_TAIL, l->bracket_absent, l->c_lo);
  problems += check_local(out, prefix, "c_hi", CHECK_TAIL, l->bracket_absent, l->c_hi);

  /* A bracket end that never moved: the bound binds, and the port's value must equal it */
  snprintf(field, sizeof(field), "%s.c_lo_at_initial_bound", prefix);
  const int lo_bound = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s.c_hi_at_initial_bound", prefix);
  const int hi_bound = oracle_in_i32(out, field);
  if (l->bracket_absent) {
    problems += (lo_bound != -1) + (hi_bound != -1);
  } else {
    if (lo_bound != (l->c_lo == MIMIC_SAGE_CONCENTRATION_C_LO) ||
        hi_bound != (l->c_hi == MIMIC_SAGE_CONCENTRATION_C_HI)) {
      fprintf(stderr, "oracle: %s: %s's bound markers disagree with the port's bracket\n",
              out->path, prefix);
      problems++;
    }
    if (lo_bound == 1) {
      tally.binding++;
      problems += oracle_f64_to_bits(l->c_lo) != oracle_f64_to_bits(MIMIC_SAGE_CONCENTRATION_C_LO);
    }
    if (hi_bound == 1) {
      tally.binding++;
      problems += oracle_f64_to_bits(l->c_hi) != oracle_f64_to_bits(MIMIC_SAGE_CONCENTRATION_C_HI);
    }
  }
  /* The transcription's tie observations: not SAGE26 or package state */
  snprintf(field, sizeof(field), "%s.ties", prefix);
  oracle_exclude(out, field);
  snprintf(field, sizeof(field), "%s.first_tie_step", prefix);
  oracle_exclude(out, field);
  return problems;
}

/** @brief Compare one concentration_from_vmax_vvir fixture; returns mismatches and errors */
static int check_inversion_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "concentration_from_vmax_vvir", &in, &out) != 0) {
    return 1;
  }
  int problems = 0;
  const int ncase = oracle_in_i32(&in, "call.ncase");
  if (ncase < 1 || ncase > MAX_CASES) {
    fprintf(stderr, "oracle: %s: call.ncase %d outside 1..%d\n", name, ncase, MAX_CASES);
    return 1;
  }
  for (int k = 0; k < ncase; k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "case[%d].Vmax", k);
    const double vmax = oracle_in_f64(&in, field);
    snprintf(field, sizeof(field), "case[%d].Vvir", k);
    const double vvir = oracle_in_f64(&in, field);

    struct MimicSageConcentrationLocals l;
    const double concentration = mimic_sage_concentration_from_vmax_vvir(vmax, vvir, &l);
    tally.cases++;

    snprintf(field, sizeof(field), "return.case[%d]", k);
    problems += oracle_check_f64(&out, field, concentration);
    snprintf(field, sizeof(field), "output.case[%d].Concentration", k);
    problems += oracle_check_f32(&out, field, (float)concentration);
    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    problems += check_inversion(&out, prefix, &l);
  }
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Compare one get_halo_concentration fixture; returns mismatches and errors */
static int check_dispatch_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "get_halo_concentration", &in, &out) != 0) {
    return 1;
  }
  int problems = 0;
  const int concentration_on = oracle_in_i32(&in, "params.ConcentrationOn");
  if (mimic_sage_validate_concentration_on(MODULE, concentration_on) != 0) {
    return 1;
  }
  (void)oracle_in_f64(&in, "call.z"); /* read by neither mode, in SAGE26 or the package */
  const int ncase = oracle_in_i32(&in, "call.ncase");
  if (ncase < 1 || ncase > MAX_CASES) {
    fprintf(stderr, "oracle: %s: call.ncase %d outside 1..%d\n", name, ncase, MAX_CASES);
    return 1;
  }
  /* SAGE26's joining stores the result only under ConcentrationOn > 0 (B:291) */
  const int store_made = concentration_on > 0;
  problems += oracle_check_i32(&out, "intermediate.store_made", store_made);

  for (int k = 0; k < ncase; k++) {
    const int type = oracle_in_i32(&in, oracle_galaxy_field(k, "Type"));
    const float vmax = oracle_in_f32(&in, oracle_galaxy_field(k, "Vmax"));
    const float vvir = oracle_in_f32(&in, oracle_galaxy_field(k, "Vvir"));
    const float infall_vmax = oracle_in_f32(&in, oracle_galaxy_field(k, "infallVmax"));
    const float infall_vvir = oracle_in_f32(&in, oracle_galaxy_field(k, "infallVvir"));

    struct MimicSageHaloConcentrationLocals l;
    const double concentration = mimic_sage_get_halo_concentration(
        concentration_on, type, vmax, vvir, infall_vmax, infall_vvir, &l);
    tally.cases++;

    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "return.case[%d]", k);
    problems += oracle_check_f64(&out, field, concentration);
    if (store_made) {
      snprintf(field, sizeof(field), "output.case[%d].Concentration", k);
      problems += oracle_check_f32(&out, field, (float)concentration);
    }
    char prefix[ORACLE_MAX_NAME];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    snprintf(field, sizeof(field), "%s.selected_infall", prefix);
    problems += oracle_check_i32(&out, field, l.selected_infall);
    problems += check_local(&out, prefix, "vmax", CHECK_CONTINUATION, l.pair_absent, l.pair.vmax);
    problems += check_local(&out, prefix, "vvir", CHECK_CONTINUATION, l.pair_absent, l.pair.vvir);
    problems += check_inversion(&out, prefix, &l.inversion);
  }
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

// ============================================================================
// PROGENITOR JOINING (THE CORE'S INHERITANCE, THEN THE PRE_TIMESTEP MODULES)
// ============================================================================

/** @brief Whether name starts with prefix */
static int starts_with(const char *name, const char *prefix) {
  return strncmp(name, prefix, strlen(prefix)) == 0;
}

/** @brief Consume every field whose name starts with prefix (a deliberate, named exclusion) */
static void consume_prefix(struct oracle_fixture *fx, const char *prefix) {
  for (int i = 0; i < fx->num_fields; i++) {
    if (starts_with(fx->fields[i].name, prefix)) {
      fx->fields[i].consumed = 1;
    }
  }
}

/** @brief Append an f64 field to a loaded fixture; returns 1 when the reader's table is full */
static int add_f64_field(struct oracle_fixture *fx, const char *name, const double value) {
  if (fx->num_fields >= ORACLE_MAX_FIELDS) {
    fprintf(stderr, "oracle: %s: no room for '%s'\n", fx->path, name);
    return 1;
  }
  struct oracle_field *field = &fx->fields[fx->num_fields++];
  memset(field, 0, sizeof(*field));
  snprintf(field->name, sizeof(field->name), "%s", name);
  field->type = ORACLE_F64;
  field->bits = oracle_f64_to_bits(value);
  return 0;
}

/**
 * @brief The harness's full-state timing for a join_progenitors fixture: call.halo_age and each
 *        progenitor's call.Age[<SnapNum>], all 0
 *
 * SAGE26's joining reads no time (B:237 sets dT to -1) and neither do the package's joining
 * modules; the timing only sets the core's dT, which is excluded as bookkeeping.
 */
static int add_zero_timing(struct oracle_fixture *in) {
  int problems = add_f64_field(in, "call.halo_age", 0.0);
  const int ngal = oracle_in_i32(in, "call.ngal");
  for (int k = 0; k < ngal && k < ORACLE_JOIN_MAX_GALAXIES; k++) {
    char name[ORACLE_MAX_NAME];
    snprintf(name, sizeof(name), "call.Age[%d]",
             oracle_in_i32(in, oracle_galaxy_field(k, "SnapNum")));
    if (!oracle_has(in, name)) {
      problems += add_f64_field(in, name, 0.0);
    }
  }
  return problems;
}

/**
 * @brief The run configuration the joining fixtures need: R5's cosmology and parameters, and the
 *        three pre_timestep modules initialised (ConcentrationOn and BulgeSizeOn the fixture's)
 */
static int init_join_modules(struct oracle_fixture *in) {
  MimicConfig.PartMass = oracle_in_f64(in, "params.PartMass");
  MimicConfig.Hubble = oracle_in_f64(in, "params.Hubble");
  MimicConfig.Omega = oracle_in_f64(in, "params.Omega");
  MimicConfig.OmegaLambda = oracle_in_f64(in, "params.OmegaLambda");
  MimicConfig.G = oracle_in_f64(in, "params.G");
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");
  int problems = 0;
  if (oracle_set_model_param("MergerTimeFactor", oracle_in_f64(in, "params.MergerTimeFactor")) !=
          0 ||
      oracle_set_model_param("BulgeSizeOn", (double)oracle_in_i32(in, "params.BulgeSizeOn")) != 0 ||
      oracle_set_model_param("ConcentrationOn",
                             (double)oracle_in_i32(in, "params.ConcentrationOn")) != 0) {
    problems++;
  }
  if (problems == 0 &&
      (sage26_initialise_merger_clock_init() != 0 || sage26_set_halo_concentration_init() != 0 ||
       sage26_set_disk_scale_radius_init() != 0)) {
    fprintf(stderr, "oracle: %s: a joining module failed to initialise\n", in->path);
    problems++;
  }
  return problems;
}

static void cleanup_join_modules(void) {
  sage26_set_disk_scale_radius_cleanup();
  sage26_set_halo_concentration_cleanup();
  sage26_initialise_merger_clock_cleanup();
}

/**
 * @brief Assert one joined row against "galaxy[<j>].*": every recorded member, the named
 *        exclusions of the file comment apart
 */
static int check_joined_row(struct oracle_fixture *out, const int j, const struct Halo *row,
                            const int source_type, const float source_clock, const int offset) {
  struct GalaxyData *g = row->galaxy;
  const int newly_orphaned = (row->Type == 2 && (source_type == 0 || source_type == 1));
  /* A row off SAGE26's main-branch reset (B:252, :273-276): a new orphan, or a former Type 2
   * that B:240 passes over. SAGE26 keeps its QuasarModeBHaccretionMass, OutflowRate, Cooling and
   * Heating; the core's init_repeat resets them on every inherited row */
  const int not_reset = newly_orphaned || source_type == 2;
  int problems = 0;

  size_t count;
  const char *const *names = oracle_c06_block_names(&count);
  for (size_t k = 0; k < count; k++) {
    const char *field = oracle_galaxy_field(j, names[k]);
    if (not_reset && (strcmp(names[k], "QuasarModeBHaccretionMass") == 0 ||
                      strcmp(names[k], "OutflowRate") == 0)) {
      oracle_exclude(out, field);
      join_tally.reset_exclusions++;
      continue;
    }
    problems += oracle_check_f32(out, field, *oracle_c06_galaxy_float(g, names[k]));
  }
  problems += oracle_c05_check_histories(out, "", j, g);
  problems += oracle_check_f32(out, oracle_galaxy_field(j, "MergTime"), g->MergTime);
  problems += oracle_check_f32(out, oracle_galaxy_field(j, "Concentration"), g->Concentration);

  problems += oracle_check_i32(out, oracle_galaxy_field(j, "mergeType"), 0);
  problems += oracle_check_i32(out, oracle_galaxy_field(j, "Type"), row->Type);
  problems += oracle_check_i32(out, oracle_galaxy_field(j, "SnapNum"), row->SnapNum);
  problems += oracle_check_i32(out, oracle_galaxy_field(j, "HaloNr"), (int)row->HaloNr);
  problems +=
      oracle_check_i32(out, oracle_galaxy_field(j, "CentralGal"), row->CentralHalo - offset);
  oracle_exclude(out, oracle_galaxy_field(j, "mergeIntoID")); /* B:287, :304: bookkeeping */
  oracle_exclude(out, oracle_galaxy_field(j, "dT"));          /* B:237: bookkeeping */
  if (newly_orphaned) {
    oracle_exclude(out, oracle_galaxy_field(j, "Len")); /* the core zeros it, SAGE26 keeps it */
  } else {
    problems += oracle_check_i32(out, oracle_galaxy_field(j, "Len"), row->Len);
  }

  const char *const halo_names[] = {"Mvir", "deltaMvir", "Rvir", "Vvir", "Vmax"};
  const double halo_values[] = {row->Mvir, row->deltaMvir, row->Rvir, row->Vvir, row->Vmax};
  for (size_t k = 0; k < sizeof(halo_names) / sizeof(halo_names[0]); k++) {
    problems += oracle_check_f32(out, oracle_galaxy_field(j, halo_names[k]), (float)halo_values[k]);
  }
  const char *const infall_names[] = {"infallMvir", "infallVvir", "infallVmax"};
  const double infall_values[] = {row->infallMvir, row->infallVvir, row->infallVmax};
  for (size_t k = 0; k < sizeof(infall_names) / sizeof(infall_names[0]); k++) {
    if (newly_orphaned && source_type == 1 && source_clock > 999.0f) {
      /* Core boundary (B:327-333; the core records them only for a previous Type 0) */
      oracle_exclude(out, oracle_galaxy_field(j, infall_names[k]));
    } else {
      problems +=
          oracle_check_f32(out, oracle_galaxy_field(j, infall_names[k]), (float)infall_values[k]);
    }
  }
  problems +=
      oracle_check_f32(out, oracle_galaxy_field(j, "infallStellarMass"), g->infallStellarMassState);
  problems += oracle_check_f32(out, oracle_galaxy_field(j, "TimeOfInfall"), g->TimeOfInfallState);

  const char *const doubles[] = {"Cooling", "Heating"};
  for (size_t k = 0; k < sizeof(doubles) / sizeof(doubles[0]); k++) {
    if (not_reset) {
      oracle_exclude(out, oracle_galaxy_field(j, doubles[k]));
      join_tally.reset_exclusions++;
    } else {
      problems += oracle_check_f64(out, oracle_galaxy_field(j, doubles[k]),
                                   *oracle_galaxy_double(g, doubles[k]));
    }
  }
  join_tally.orphaned += newly_orphaned;
  return problems;
}

/**
 * @brief The wiring of one row's path: the current pair, the infall pair, or kept
 *
 * A full-state comparison cannot tell which pair produced a value when both pairs give it, so
 * each path is asserted on its own here: the assigned value is the inversion of the selected
 * float pair (the shared helper, which the module calls), the infall path's value differs from
 * the current pair's, and a kept row holds its progenitor's input value. The oracle's
 * concentration_assigned marks exactly the assigned rows.
 */
static int check_path(struct oracle_fixture *in, struct oracle_fixture *out, const int j,
                      const int k, const struct Halo *row) {
  const struct GalaxyData *g = row->galaxy;
  const float current =
      (float)mimic_sage_concentration_from_vmax_vvir((float)row->Vmax, (float)row->Vvir, NULL);
  const float infall = (float)mimic_sage_concentration_from_vmax_vvir((float)row->infallVmax,
                                                                      (float)row->infallVvir, NULL);
  /* The progenitor's input value, read again (the harness read it once already) */
  const float input = oracle_in_f32(in, oracle_galaxy_field(k, "Concentration"));
  const uint32_t got = oracle_f32_to_bits(g->Concentration);
  int assigned = 0;
  int problems = 0;
  if (row->Type == 0 && g->PreviousType == 0) {
    assigned = 1;
    join_tally.current++;
    if (got != oracle_f32_to_bits(current)) {
      fprintf(stderr, "oracle: %s: row %d (previous Type 0) does not hold its current pair's\n",
              out->path, j);
      problems++;
    }
  } else if (row->Type == 0 && g->PreviousType == 1) {
    assigned = 1;
    join_tally.infall++;
    if (got != oracle_f32_to_bits(infall) || got == oracle_f32_to_bits(current)) {
      fprintf(stderr,
              "oracle: %s: row %d (previous Type 1) must hold its infall pair's value, distinct "
              "from its current pair's\n",
              out->path, j);
      problems++;
    }
  } else {
    join_tally.kept++;
    if (got != oracle_f32_to_bits(input)) {
      fprintf(stderr, "oracle: %s: row %d (kept) changed its Concentration\n", out->path, j);
      problems++;
    }
  }

  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.join.progenitor[%d].concentration_assigned_absent",
           k);
  const int absent = oracle_in_i32(out, field);
  int marked = 0;
  if (!absent) {
    snprintf(field, sizeof(field), "intermediate.join.progenitor[%d].concentration_assigned", k);
    marked = oracle_in_i32(out, field);
  }
  if (marked != assigned) {
    fprintf(stderr, "oracle: %s: progenitor %d's concentration_assigned %d, the port's path %d\n",
            out->path, k, marked, assigned);
    problems++;
  }
  return problems;
}

/** @brief Compare one ConcentrationOn 3 join_progenitors fixture; returns mismatches and errors */
static int check_join_fixture(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "join_progenitors", &in, &out) != 0) {
    return 1;
  }
  if (oracle_in_i32(&in, "params.ConcentrationOn") != MIMIC_SAGE_CONCENTRATION_VMAX_VVIR) {
    fprintf(stderr, "oracle: %s: the module's joining fixtures run ConcentrationOn 3\n", name);
    return 1;
  }
  int problems = init_join_modules(&in);
  problems += add_zero_timing(&in);

  struct GalaxyPool *pool = galaxy_pool_create(16);
  static struct oracle_join_result result;
  struct ModuleContext ctx;
  if (problems == 0) {
    problems += oracle_join(&in, pool, &result, &ctx, 1);
  }
  if (problems == 0) {
    const int start = result.start;
    const int retained = result.end - start;
    problems += oracle_check_i32(&out, "return.ngal", retained);
    const int nprog = oracle_in_i32(&in, "call.ngal");
    int j = 0;
    for (int k = 0; k < nprog; k++) {
      /* The core drops an already-merged progenitor (Type 3), as SAGE26's B:244 does */
      if (oracle_in_i32(&in, oracle_galaxy_field(k, "mergeType")) != 0) {
        continue;
      }
      const struct Halo *row = &result.workspace[start + j];
      problems +=
          check_joined_row(&out, j, row, result.source_type[j], result.source_clock[j], start);
      problems += check_path(&in, &out, j, k, row);
      j++;
    }
    join_tally.rows += retained;
    join_tally.fixtures++;
  }
  cleanup_join_modules();

  /* SAGE26's driver locals (their effects are the asserted fields; concentration_assigned is
   * asserted by check_path) and the inputs the composition reads through the core and modules */
  consume_prefix(&out, "intermediate.join.progenitor[");
  consume_prefix(&in, "galaxy[");
  consume_prefix(&in, "halo[");
  consume_prefix(&in, "params.");
  consume_prefix(&in, "call.");

  galaxy_pool_destroy(pool);
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Compare one fixture by its routine (the name's prefix) */
static int check_fixture(const char *name) {
  if (strncmp(name, "inv_", 4) == 0) {
    return check_inversion_fixture(name);
  }
  if (strncmp(name, "ghc_", 4) == 0) {
    return check_dispatch_fixture(name);
  }
  if (strncmp(name, "tr_join_", 8) == 0) {
    return check_join_fixture(name);
  }
  fprintf(stderr, "oracle: %s: a fixture name must start with inv_, ghc_ or tr_join_\n", name);
  return 1;
}

/**
 * @test    test_oracle_fixture_count
 * @brief   Every fixture fixtures.manifest lists for the module is present (platform independent)
 */
int test_oracle_fixture_count(void) {
  TEST_ASSERT(oracle_fixture_count_ok(MODULE),
              "fixtures.manifest must list its declared count and every listed fixture must exist");
  return TEST_PASS;
}

/**
 * @test    test_oracle_bit_identity
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit (SKIP outside its environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&tally, 0, sizeof(tally));
  memset(&join_tally, 0, sizeof(join_tally));
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle c12 (%s): %d cases; locals continuation-checked %d, tail-checked %d; "
         "validated two-sided %d, narrowing-limited %d, not validated %d; binding bracket bounds "
         "%d; absent %d ",
         MODULE, tally.cases, tally.continuation, tally.tail, tally.validated,
         tally.narrowing_limited, tally.not_validated, tally.binding, tally.absent);
  printf("\n  oracle c12 joining: %d fixtures, %d rows: current pair %d, infall pair %d, kept %d "
         "(newly orphaned %d; members excluded off the main-branch reset %d) ",
         join_tally.fixtures, join_tally.rows, join_tally.current, join_tally.infall,
         join_tally.kept, join_tally.orphaned, join_tally.reset_exclusions);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(join_tally.current >= 1 && join_tally.infall >= 1 && join_tally.kept >= 1,
              "each joining path (current pair, infall pair, kept) must be asserted");
  return TEST_PASS;
}

/** @brief Append the transition recorder to post_timestep, so the module's init at 3 sees it */
static void add_recorder_to_post_timestep(void) {
  MimicConfig.post_timestep =
      mymalloc_cat(TEST_PHASE_MODULE_CAP * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.post_timestep[0].module_name = strdup("sage26_record_transition_state");
  MimicConfig.post_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.post_timestep[0].resolved = NULL;
  MimicConfig.num_post_timestep = 1;
}

static void free_post_timestep(void) {
  for (int i = 0; i < MimicConfig.num_post_timestep; i++) {
    free(MimicConfig.post_timestep[i].module_name);
  }
  myfree(MimicConfig.post_timestep);
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_set_halo_concentration SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  reset_config();
  init_memory_system(0);

  // The shipped run files' joining placement: the clock, this module and the disk radius in
  // pre_timestep, and the recorder of PreviousType in post_timestep (required at
  // ConcentrationOn 3)
  test_pre_timestep_add("sage26_initialise_merger_clock", PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add(MODULE, PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add("sage26_set_disk_scale_radius", PROCESSING_MODE_FULL_HALO);
  add_recorder_to_post_timestep();

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  free_post_timestep();
  test_free_pre_timestep();
  check_memory_leaks();

  TEST_SUMMARY();
  return TEST_RESULT();
}
