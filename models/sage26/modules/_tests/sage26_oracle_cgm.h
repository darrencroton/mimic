/**
 * @file    sage26_oracle_cgm.h
 * @brief   Oracle-test helpers for the CGMrecipeOn 1 merger-family fixtures (oracle_c06)
 *
 * Adds, beside sage26_oracle_fixture.h, sage26_oracle_hydrogen.h and sage26_oracle_fire.h (which
 * stay as they are), what the oracle tests of the merger-family routines with R4's flags need
 * (oracle README "The c10 Group, Part 3", merger-family part):
 *
 * - oracle_c10_require_merger_flags(): the merger family's flags with CGMrecipeOn read (0, or 1
 *   where the routine admits it) and FIREmodeOn read where R4's 1 is admitted;
 * - oracle_c10_read_regimes() and oracle_c10_check_regimes(): every galaxy's Regime, set from a
 *   CGMrecipeOn 1 fixture and asserted unchanged against its record;
 * - oracle_c10_check_site(): one routing site's selection, the oracle's flag against the
 *   package's AND the package's against the routing galaxy's input Regime;
 * - oracle_c10_check_value(): one CGMrecipeOn 1 value (tail-checked), its markers and its value bit
 *   for bit against the package's local;
 * - oracle_c10_observed(): one observation of SAGE26's nested routines, counted.
 *
 * Test-only: included by module oracle tests, never by package code.
 */

#ifndef SAGE26_ORACLE_CGM_H
#define SAGE26_ORACLE_CGM_H

#include <math.h>
#include <stdio.h>

#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_h2.h"
#include "modules/_tests/sage26_oracle_sizes.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_fire_feedback_physics.h"

/** @brief Counters of the CGMrecipeOn 1 assertions an oracle test made, for its report */
struct oracle_c10_site_tally {
  int fixtures;          /**< CGMrecipeOn 1 fixtures */
  int sites;             /**< routing sites asserted (oracle flag, package local, input Regime) */
  int sites_cgm;         /**< of which selected the CGM pair */
  int unselected;        /**< pairs asserted unchanged (or changed only by a named addition) */
  int values;            /**< CGMrecipeOn 1 values compared bit for bit */
  int values_validated;  /**< of which validated two-sided by the oracle */
  int observed_expected; /**< observations of the expected galaxy's Regime that are 1 */
  int observed_other;    /**< observations of the other galaxy's Regime that are 0 */
};

/**
 * @brief The merger family's flags of a CGMrecipeOn 1 fixture (oracle_c08_require_merger_flags with
 *        the CGM and FIRE flags read)
 *
 * SFprescription 0 or 1 (with the H2 configuration for 1) and BulgeSizeOn 0 or 3 as
 * oracle_c08_read_sf_config and oracle_c07_read_sizes read them; CGMrecipeOn 0, or 0 or 1
 * where cgm_allowed (refused otherwise); FIREmodeOn left to oracle_c09_read_fire_config for a
 * *_fire routine (fire_routine), otherwise 0, or 0 or 1 for a CGMrecipeOn 1 fixture (R4's 1,
 * which grow_black_hole, quasar_mode_wind and disrupt_satellite_to_ICS do not read; returned in
 * *fire_mode_on); every other flag R0-R4's.
 *
 * @return Number of problems
 */
static inline int oracle_c10_require_merger_flags(struct oracle_fixture *in,
                                                  struct MimicSageBulgeSize *sizes,
                                                  int *sf_prescription, const int fire_routine,
                                                  const int cgm_allowed, int *cgm_recipe_on,
                                                  int *fire_mode_on) {
  struct oracle_c08_sf_config cfg;
  int problems = oracle_c08_read_sf_config(in, 0, &cfg);
  *sf_prescription = cfg.sf_prescription;
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  *cgm_recipe_on = oracle_in_i32(in, "params.CGMrecipeOn");
  if (*cgm_recipe_on != MIMIC_SAGE_CGM_RECIPE_OFF &&
      !(cgm_allowed && *cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON)) {
    fprintf(stderr, "oracle: %s: params.CGMrecipeOn %d is not admitted for this routine\n",
            in->path, *cgm_recipe_on);
    problems++;
  }
  *fire_mode_on = MIMIC_SAGE_FIRE_MODE_OFF;
  if (!fire_routine) {
    *fire_mode_on = oracle_in_i32(in, "params.FIREmodeOn");
    const int admitted =
        *fire_mode_on == MIMIC_SAGE_FIRE_MODE_OFF ||
        (*cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON && *fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON);
    if (!admitted) {
      fprintf(stderr, "oracle: %s: params.FIREmodeOn %d is not admitted for this routine\n",
              in->path, *fire_mode_on);
      problems++;
    }
  }
  problems += oracle_c07_read_sizes(in, sizes);
  problems += oracle_c05_require_flag(in, "params.SaveFullSFH", 0);
  problems += oracle_c05_require_flag(in, "params.AGNrecipeOn", 2);
  problems += oracle_c05_require_flag(in, "params.StarburstColdGasOn", 1);
  problems += oracle_c05_require_flag(in, "params.DiskInstabilityOn", 1);
  return problems;
}

/**
 * @brief Set every galaxy's Regime from a CGMrecipeOn 1 fixture (0 or 1: SAGE26's classifier
 *        assigns only those, R/src/model_regimes.c:77-95)
 * @return Number of problems
 */
static inline int oracle_c10_read_regimes(struct oracle_fixture *in, const int ngal,
                                          struct GalaxyData *galaxies) {
  int problems = 0;
  for (int i = 0; i < ngal; i++) {
    galaxies[i].Regime = oracle_in_i32(in, oracle_galaxy_field(i, "Regime"));
    if (galaxies[i].Regime != 0 && galaxies[i].Regime != 1) {
      fprintf(stderr, "oracle: %s: galaxy[%d].Regime %d is neither 0 nor 1\n", in->path, i,
              galaxies[i].Regime);
      problems++;
    }
  }
  return problems;
}

/**
 * @brief Each recorded galaxy's Regime against the oracle's (no routine writes it)
 * @return Number of mismatches
 */
static inline int oracle_c10_check_regimes(struct oracle_fixture *out, const int ngal,
                                           const struct GalaxyData *galaxies) {
  int problems = 0;
  for (int i = 0; i < ngal; i++) {
    const char *field = oracle_galaxy_field(i, "Regime");
    if (oracle_has(out, field)) {
      problems += oracle_check_i32(out, field, galaxies[i].Regime);
    }
  }
  return problems;
}

/**
 * @brief One routing site's selection: the oracle's flag against the package's local, and the
 *        package's against the routing galaxy's input Regime (1 exactly when that is 0)
 *
 * @param flag             The oracle's flag name (without "intermediate.")
 * @param actual           The package's selection (1 CGM pair, 0 hot pair, -1 not run)
 * @param expected_regime  The input Regime of the galaxy that must route this site, or -1 where
 *                         the site does not run (the package's selection must then be -1)
 * @return Number of mismatches
 */
static inline int oracle_c10_check_site(struct oracle_fixture *out, const char *flag,
                                        const int actual, const int expected_regime,
                                        struct oracle_c10_site_tally *tally) {
  int problems = oracle_c06_check_flag(out, flag, actual);
  const int expected = expected_regime < 0 ? -1 : expected_regime == 0;
  if (actual != expected) {
    fprintf(stderr, "oracle: %s: %s routed by the wrong galaxy's Regime (%d, expected %d)\n",
            out->path, flag, actual, expected);
    problems++;
  }
  if (actual >= 0) {
    tally->sites++;
    tally->sites_cgm += actual;
  }
  return problems;
}

/**
 * @brief One CGMrecipeOn 1 value: absent exactly where the package's local is NaN; otherwise its
 *        markers (checked, tail-checked, the two-sided conjunction) and the value bit for bit
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c10_check_value(struct oracle_fixture *out, const char *name,
                                         const double actual, struct oracle_c10_site_tally *tally) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  const int absent = oracle_in_i32(out, field);
  if (absent != (isnan(actual) ? 1 : 0)) {
    fprintf(stderr, "oracle: %s: %s is %s in the oracle but %s in the port\n", out->path, name,
            absent ? "absent" : "present", isnan(actual) ? "absent" : "present");
    return 1;
  }
  if (absent) {
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "intermediate.%s_checked", name);
  problems += oracle_in_i32(out, field) != 1;
  snprintf(field, sizeof(field), "intermediate.%s_check_kind", name);
  problems += oracle_in_i32(out, field) != ORACLE_C10_TAIL;
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, field);
  if (validated != (up && down)) {
    fprintf(stderr, "oracle: %s: %s_validated disagrees with its two directions\n", out->path,
            name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s", name);
  problems += oracle_check_f64(out, field, actual);
  tally->values++;
  tally->values_validated += validated;
  return problems;
}

/**
 * @brief One observation of SAGE26's nested routines (an i32 -1, 0 or 1), counted: 1 where the
 *        flipped galaxy is the one that must route the call (expected), 0 where it is not
 * @return The observation
 */
static inline int oracle_c10_observed(struct oracle_fixture *out, const char *name,
                                      const int flips_expected_galaxy,
                                      struct oracle_c10_site_tally *tally) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s", name);
  const int observed = oracle_in_i32(out, field);
  if (flips_expected_galaxy && observed == 1) {
    tally->observed_expected++;
  } else if (!flips_expected_galaxy && observed == 0) {
    tally->observed_other++;
  }
  return observed;
}

/** @brief Print one test run's CGMrecipeOn 1 tallies */
static inline void oracle_c10_print_site_tally(const struct oracle_c10_site_tally *t) {
  printf("\n  oracle: c10 CGMrecipeOn 1 fixtures %d: routing sites asserted against the oracle and "
         "the expected galaxy's Regime %d (CGM pair %d, hot pair %d); unselected pairs asserted "
         "%d; c10 values compared bit for bit %d (validated two-sided %d); SAGE26's nested "
         "routines observed routing by the expected galaxy %d, insensitive to the other's %d ",
         t->fixtures, t->sites, t->sites_cgm, t->sites - t->sites_cgm, t->unselected, t->values,
         t->values_validated, t->observed_expected, t->observed_other);
}

#endif /* SAGE26_ORACLE_CGM_H */
