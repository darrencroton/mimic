/**
 * @file    sage26_oracle_sizes.h
 * @brief   Oracle-test helpers for the c07 bulge sizes (BulgeSizeOn 0 and 3)
 *
 * Adds, beside the shared fixture reader sage26_oracle_fixture.h (which stays as it is), what
 * the oracle tests of the c07 fixtures need (oracle README "The c07 Group"):
 *
 * - oracle_c07_read_sizes(): a fixture's params.BulgeSizeOn (0 or 3) and, in mode 3,
 *   params.Hubble_h, into the package's struct MimicSageBulgeSize;
 * - oracle_c07_require_merger_flags(): c06's merger-family flags with BulgeSizeOn read rather
 *   than pinned to 0 (oracle_c06_require_r0_flags pins it);
 * - the six-member size block of the c07 routines (the two channel masses, the disk radius and
 *   the three radii): oracle_c07_in_size_block() and oracle_c07_check_size_block();
 * - oracle_c07_check_local(): one local of get_bulge_radius or update_instability_bulge_radius,
 *   its markers checked and its value compared bit for bit (tallied by validation status).
 *
 * Test-only: included by module oracle tests, never by package code.
 */

#ifndef SAGE26_ORACLE_SIZES_H
#define SAGE26_ORACLE_SIZES_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage26_oracle_fixture.h"
#include "shared/sage_bulge_size_physics.h"

/** @brief The size block's SAGE26 member names, as the c07 oracle records them */
static const char *const ORACLE_C07_SIZE_NAMES[] = {"MergerBulgeMass",   "InstabilityBulgeMass",
                                                    "DiskScaleRadius",   "BulgeRadius",
                                                    "MergerBulgeRadius", "InstabilityBulgeRadius"};
#define ORACLE_C07_SIZE_COUNT 6

/** @brief The package member holding a size-block name (the names are the package's) */
static inline float *oracle_c07_size_member(struct GalaxyData *galaxy, const int k) {
  float *const members[ORACLE_C07_SIZE_COUNT] = {
      &galaxy->MergerBulgeMass, &galaxy->InstabilityBulgeMass, &galaxy->DiskScaleRadius,
      &galaxy->BulgeRadius,     &galaxy->MergerBulgeRadius,    &galaxy->InstabilityBulgeRadius};
  return members[k];
}

/**
 * @brief A fixture's bulge-size configuration
 *
 * Reads params.BulgeSizeOn, which must be 0 or 3 (the oracle refuses any other value), and
 * params.Hubble_h when it is 3 (a mode-0 fixture carries none).
 *
 * @return Number of problems
 */
static inline int oracle_c07_read_sizes(struct oracle_fixture *in,
                                        struct MimicSageBulgeSize *sizes) {
  sizes->bulge_size_on = oracle_in_i32(in, "params.BulgeSizeOn");
  sizes->hubble_h = 0.0;
  if (sizes->bulge_size_on == MIMIC_SAGE_BULGE_SIZE_TONINI) {
    sizes->hubble_h = oracle_in_f64(in, "params.Hubble_h");
  } else if (sizes->bulge_size_on != MIMIC_SAGE_BULGE_SIZE_OFF) {
    fprintf(stderr, "oracle: %s: params.BulgeSizeOn %d is neither 0 nor 3\n", in->path,
            sizes->bulge_size_on);
    return 1;
  }
  return 0;
}

/**
 * @brief c06's merger-family flags with BulgeSizeOn read (0 or 3) rather than pinned to 0
 *
 * The other flags are R0's (and R1's: R1.par differs from R0.par only in BulgeSizeOn, :109).
 *
 * @return Number of problems
 */
static inline int oracle_c07_require_merger_flags(struct oracle_fixture *in,
                                                  struct MimicSageBulgeSize *sizes) {
  int problems = 0;
  problems += oracle_c05_require_flag(in, "params.SFprescription", 0);
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  problems += oracle_c05_require_flag(in, "params.CGMrecipeOn", 0);
  problems += oracle_c05_require_flag(in, "params.FIREmodeOn", 0);
  problems += oracle_c07_read_sizes(in, sizes);
  problems += oracle_c05_require_flag(in, "params.SaveFullSFH", 0);
  problems += oracle_c05_require_flag(in, "params.AGNrecipeOn", 2);
  problems += oracle_c05_require_flag(in, "params.StarburstColdGasOn", 1);
  problems += oracle_c05_require_flag(in, "params.DiskInstabilityOn", 1);
  return problems;
}

/** @brief Set the size block of galaxy[index] from the fixture */
static inline void oracle_c07_in_size_block(struct oracle_fixture *in, const int index,
                                            struct GalaxyData *galaxy) {
  for (int k = 0; k < ORACLE_C07_SIZE_COUNT; k++) {
    *oracle_c07_size_member(galaxy, k) =
        oracle_in_f32(in, oracle_galaxy_field(index, ORACLE_C07_SIZE_NAMES[k]));
  }
}

/**
 * @brief Compare the size block of galaxy[index] with the oracle bit for bit
 * @return Number of mismatches
 */
static inline int oracle_c07_check_size_block(struct oracle_fixture *out, const int index,
                                              struct GalaxyData *galaxy) {
  int mismatches = 0;
  for (int k = 0; k < ORACLE_C07_SIZE_COUNT; k++) {
    mismatches += oracle_check_f32(out, oracle_galaxy_field(index, ORACLE_C07_SIZE_NAMES[k]),
                                   *oracle_c07_size_member(galaxy, k));
  }
  return mismatches;
}

/** @brief Counters of the c07 local assertions a test made, for its report */
struct oracle_c07_local_tally {
  int validated;         /**< validated two-sided */
  int narrowing_limited; /**< one float narrowing is the only path: not validated, by name */
  int not_validated;     /**< neither (a one-ulp change is absorbed elsewhere) */
  int absent;            /**< the path does not form it (and the port agrees) */
};

/**
 * @brief Check one c07 local and compare it bit for bit
 *
 * Requires the port's value to be NaN exactly when the oracle marks it absent. When present,
 * requires _checked 1, _check_kind 2 (every c07 local is tail-checked), _validated equal to
 * the conjunction of _validated_up and _validated_down, and _narrowing_limited equal to
 * `narrowing_limited` (1 for the locals of update_instability_bulge_radius, whose only path to
 * the state is one float store). The value is compared bit for bit whatever its validation
 * status: the port forms every local with SAGE26's expression, so a validated, a
 * narrowing-limited and an unvalidated local are all asserted; the tally records which.
 *
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c07_check_local(struct oracle_fixture *out, const char *name,
                                         const double actual, const int narrowing_limited,
                                         struct oracle_c07_local_tally *tally) {
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
  problems += oracle_check_f64(out, field, actual);
  if (validated) {
    tally->validated++;
  } else if (narrowing_limited) {
    tally->narrowing_limited++;
  } else {
    tally->not_validated++;
  }
  return problems;
}

#endif /* SAGE26_ORACLE_SIZES_H */
