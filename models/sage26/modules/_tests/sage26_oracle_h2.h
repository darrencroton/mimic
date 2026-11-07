/**
 * @file    sage26_oracle_h2.h
 * @brief   Oracle-test helpers for the molecular gas and hydrogen caps (SFprescription 0 and 1)
 *
 * Adds, beside the shared fixture reader sage26_oracle_fixture.h (which stays as it is), what the
 * oracle tests of the molecular-gas and hydrogen-cap fixtures need (oracle README "The c08 Group"):
 *
 * - oracle_c08_read_sf_config(): a fixture's params.SFprescription (0 or 1) and, for 1, the H2
 *   configuration the oracle requires with it (H2RadialIntegrationOn 1, H2RadialNBins 25,
 *   H2RadialRMaxFactor 5.0 and, where asked, StarburstColdGasOn 1);
 * - oracle_c08_set_sf_parameters(): those values as the package's required module parameters,
 *   the H2 parameters at their R2 values when the fixture is SFprescription 0 (which carries
 *   none of them, as the run files carry R2's values with SFprescription 0);
 * - oracle_c08_check_local(): one molecular-gas and hydrogen-cap local or carried value, its
 *   markers checked (those of the oracle README's "The c06 Group", with check kind 2 and the
 *   narrowing limitation) and its value compared bit for bit as the float or double the oracle
 *   records, tallied by validation status.
 *
 * Test-only: included by module oracle tests, never by package code.
 */

#ifndef SAGE26_ORACLE_H2_H
#define SAGE26_ORACLE_H2_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage26_oracle_fixture.h"
#include "shared/sage_h2_physics.h"

/** @brief The SF prescription and H2 configuration of one fixture */
struct oracle_c08_sf_config {
  int sf_prescription;          /**< 0 (R0, R1) or 1 (R2) */
  int h2_radial_integration_on; /**< 1 (read for SFprescription 1) */
  int h2_radial_nbins;          /**< 25 (read for SFprescription 1) */
  double h2_radial_rmax_factor; /**< 5.0 (read for SFprescription 1) */
};

/**
 * @brief Read a fixture's SFprescription and, for 1, its H2 configuration
 *
 * SFprescription must be 0 or 1. For 1 the fixture must carry H2RadialIntegrationOn 1,
 * H2RadialNBins 25 and H2RadialRMaxFactor 5.0 and, when read_starburst is set,
 * StarburstColdGasOn 1 (a fixture whose reader pins that flag elsewhere passes 0). For 0 the
 * H2 fields are SAGE26's defaults, the values the package's run files carry.
 *
 * @return Number of problems
 */
static inline int oracle_c08_read_sf_config(struct oracle_fixture *in, const int read_starburst,
                                            struct oracle_c08_sf_config *cfg) {
  int problems = 0;
  cfg->sf_prescription = oracle_in_i32(in, "params.SFprescription");
  cfg->h2_radial_integration_on = MIMIC_SAGE_H2_RADIAL_INTEGRATION_ON;
  cfg->h2_radial_nbins = MIMIC_SAGE_H2_RADIAL_NBINS;
  cfg->h2_radial_rmax_factor = MIMIC_SAGE_H2_RADIAL_RMAX_FACTOR;
  if (cfg->sf_prescription != MIMIC_SAGE_SF_PRESCRIPTION_CROTON &&
      cfg->sf_prescription != MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    fprintf(stderr, "oracle: %s: params.SFprescription %d is neither 0 nor 1\n", in->path,
            cfg->sf_prescription);
    return 1;
  }
  if (cfg->sf_prescription != MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    return 0;
  }
  cfg->h2_radial_integration_on = oracle_in_i32(in, "params.H2RadialIntegrationOn");
  cfg->h2_radial_nbins = oracle_in_i32(in, "params.H2RadialNBins");
  cfg->h2_radial_rmax_factor = oracle_in_f64(in, "params.H2RadialRMaxFactor");
  if (cfg->h2_radial_integration_on != MIMIC_SAGE_H2_RADIAL_INTEGRATION_ON ||
      cfg->h2_radial_nbins != MIMIC_SAGE_H2_RADIAL_NBINS ||
      oracle_f64_to_bits(cfg->h2_radial_rmax_factor) !=
          oracle_f64_to_bits(MIMIC_SAGE_H2_RADIAL_RMAX_FACTOR)) {
    fprintf(stderr, "oracle: %s: the H2 configuration is not the supported 1, 25, 5.0\n", in->path);
    problems++;
  }
  if (read_starburst) {
    problems +=
        oracle_c05_require_flag(in, "params.StarburstColdGasOn", MIMIC_SAGE_STARBURST_COLD_GAS_ON);
  }
  return problems;
}

/**
 * @brief Set the package's required SF-prescription parameters from a fixture's configuration
 *
 * SFprescription and the three H2 integration parameters, which sage26_calculate_star_formation
 * reads (sage26_apply_star_formation_supernova reads SFprescription alone).
 *
 * @return Number of problems
 */
static inline int oracle_c08_set_sf_parameters(const struct oracle_c08_sf_config *cfg) {
  int problems = 0;
  problems += oracle_set_model_param("SFprescription", (double)cfg->sf_prescription) != 0;
  problems +=
      oracle_set_model_param("H2RadialIntegrationOn", (double)cfg->h2_radial_integration_on) != 0;
  problems += oracle_set_model_param("H2RadialNBins", (double)cfg->h2_radial_nbins) != 0;
  problems += oracle_set_model_param("H2RadialRMaxFactor", cfg->h2_radial_rmax_factor) != 0;
  return problems;
}

/**
 * @brief Counters of the molecular-gas and hydrogen-cap local
 *        assertions a test made, for its report
 */
struct oracle_c08_local_tally {
  int validated;         /**< validated two-sided */
  int narrowing_limited; /**< one float narrowing is the only path: not validated, by name */
  int not_validated;     /**< neither (a one-ulp change is absorbed elsewhere) */
  int absent;            /**< the path does not form it (and the port agrees) */
  int binding;           /**< a binding cap's value asserted equal to the reservoir it set */
};

/**
 * @brief Check one molecular-gas and hydrogen-cap local or carried value and compare it bit for bit
 *
 * Requires the port's value to be NaN exactly when the oracle marks it absent. When present,
 * requires _checked 1, _check_kind 2 (every molecular-gas and hydrogen-cap value is tail-checked),
 * _validated equal to the conjunction of _validated_up and _validated_down, and _narrowing_limited
 * equal to `narrowing_limited`. The value is compared bit for bit, as the f32 or f64 the oracle
 * records (`is_f32`), whatever its validation status: the port forms each with SAGE26's expression;
 * the tally records which status it has.
 *
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c08_check_local(struct oracle_fixture *out, const char *name,
                                         const double actual, const int is_f32,
                                         const int narrowing_limited,
                                         struct oracle_c08_local_tally *tally) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  const int absent = oracle_in_i32(out, field);
  if (absent != (isnan(actual) ? 1 : 0)) {
    fprintf(stderr, "oracle: %s: %s is %s in the oracle but %s in the port\n", out->path, name,
            absent ? "absent" : "present", isnan(actual) ? "absent" : "present");
    return 1;
  }
  if (absent) {
    tally->absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "intermediate.%s_checked", name);
  problems += oracle_check_i32(out, field, 1);
  snprintf(field, sizeof(field), "intermediate.%s_check_kind", name);
  problems += oracle_check_i32(out, field, 2);
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
  snprintf(field, sizeof(field), "intermediate.%s_narrowing_limited", name);
  problems += oracle_check_i32(out, field, narrowing_limited);
  if (narrowing_limited && validated) {
    fprintf(stderr, "oracle: %s: %s is narrowing-limited but validated two-sided\n", out->path,
            name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s", name);
  problems +=
      is_f32 ? oracle_check_f32(out, field, (float)actual) : oracle_check_f64(out, field, actual);
  if (validated) {
    tally->validated++;
  } else if (narrowing_limited) {
    tally->narrowing_limited++;
  } else {
    tally->not_validated++;
  }
  return problems;
}

#endif /* SAGE26_ORACLE_H2_H */
