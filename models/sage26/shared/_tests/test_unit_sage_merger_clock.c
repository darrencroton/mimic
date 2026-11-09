/**
 * @file    test_unit_sage_merger_clock.c
 * @brief   Unit tests for shared/sage_merger_clock.h (SAGE26's estimate_merging_time)
 *
 * Checks of each branch and guard of SAGE26's estimate_merging_time on hand-checkable inputs.
 *
 * Test cases:
 *   - test_same_halo: equal halo numbers return -1 with nothing formed
 *   - test_formula: the clock is MergerTimeFactor * 1.17 R^2 V / (ln(1 + Nm/Ns) G M)
 *   - test_particle_floor: satellite Len 9 returns -1, Len 10 a clock
 *   - test_invalid_mass: a non-positive satellite mass returns -1
 *   - test_zero_satellite_len: Len 0 forms an infinite Coulomb logarithm and the
 *     particle floor returns -1 (no extra guard)
 *   - test_zero_coulomb: a zero mother Len gives log1p(0) = 0 and -1
 *   - test_cap: a clock at or above 999 is capped to 998
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_merger_clock.h"

#define REDSHIFT 0.5

static void set_cosmology(void) {
  reset_config();
  MimicConfig.Hubble = 100.0;
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.G = 43.00707785642063;
}

static void make_pair(struct Halo *sat, struct GalaxyData *gal, struct Halo *mother,
                      const int sat_len, const double sat_mvir, const int mother_len,
                      const double mother_mvir) {
  memset(sat, 0, sizeof(*sat));
  memset(mother, 0, sizeof(*mother));
  memset(gal, 0, sizeof(*gal));
  sat->HaloNr = 2;
  sat->Len = sat_len;
  sat->Mvir = sat_mvir;
  sat->galaxy = gal;
  gal->StellarMass = 0.5f;
  gal->ColdGas = 0.25f;
  mother->HaloNr = 1;
  mother->Len = mother_len;
  mother->Mvir = mother_mvir;
}

static int test_same_halo(void) {
  set_cosmology();
  struct Halo sat, mother;
  struct GalaxyData gal;
  struct MimicSageMergerClockTrace trace;
  make_pair(&sat, &gal, &mother, 100, 8.6, 1000, 100.0);
  mother.HaloNr = sat.HaloNr;
  const double t = mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 2.0, 43.0, &trace);
  TEST_ASSERT(t == -1.0, "the same halo returns -1");
  TEST_ASSERT(trace.same_halo == 1 && trace.values_absent == 1 && trace.guard_passed == -1,
              "nothing is formed after the same-halo return");
  return TEST_PASS;
}

static int test_formula(void) {
  set_cosmology();
  struct Halo sat, mother;
  struct GalaxyData gal;
  struct MimicSageMergerClockTrace trace;
  make_pair(&sat, &gal, &mother, 100, 8.6, 1000, 100.0);
  const double t =
      mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 2.0, MimicConfig.G, &trace);
  const double r = virial_radius_for_mass(100.0, REDSHIFT);
  const double v = sqrt(MimicConfig.G * 100.0 / r);
  const double m = 8.6 + 0.5 + 0.25;
  const double expected = 2.0 * 1.17 * r * r * v / (log1p(10.0) * MimicConfig.G * m);
  TEST_ASSERT(fabs(t / expected - 1.0) < 1e-12, "the clock is SAGE26's formula");
  TEST_ASSERT(trace.guard_passed == 1 && trace.capped == 0 &&
                  trace.mother_virial_velocity_absent == 0,
              "the guard passes and the clock is not capped");
  TEST_ASSERT(trace.satellite_mass == m && trace.coulomb == log1p(10.0),
              "the trace carries SAGE26's locals");
  const double t4 =
      mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 4.0, MimicConfig.G, NULL);
  TEST_ASSERT(fabs(t4 / t - 2.0) < 1e-12, "the clock scales with MergerTimeFactor");
  return TEST_PASS;
}

static int test_particle_floor(void) {
  set_cosmology();
  struct Halo sat, mother;
  struct GalaxyData gal;
  make_pair(&sat, &gal, &mother, 9, 9 * 0.086, 1000, 100.0);
  TEST_ASSERT(mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 2.0, MimicConfig.G, NULL) ==
                  -1.0,
              "a satellite of 9 particles returns -1");
  sat.Len = 10;
  TEST_ASSERT(mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 2.0, MimicConfig.G, NULL) >
                  0.0,
              "a satellite of 10 particles gets a clock (>= 10)");
  return TEST_PASS;
}

static int test_invalid_mass(void) {
  set_cosmology();
  struct Halo sat, mother;
  struct GalaxyData gal;
  struct MimicSageMergerClockTrace trace;
  make_pair(&sat, &gal, &mother, 100, 0.0, 1000, 100.0);
  gal.StellarMass = 0.0f;
  gal.ColdGas = 0.0f;
  TEST_ASSERT(
      mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 2.0, MimicConfig.G, &trace) == -1.0,
      "zero satellite mass returns -1");
  TEST_ASSERT(trace.guard_passed == 0 && trace.mother_virial_velocity_absent == 1 &&
                  trace.mergtime_pre_cap == -1.0 && trace.capped == 0,
              "the guard fails before the mother's velocity is formed");
  return TEST_PASS;
}

static int test_zero_satellite_len(void) {
  set_cosmology();
  struct Halo sat, mother;
  struct GalaxyData gal;
  struct MimicSageMergerClockTrace trace;
  make_pair(&sat, &gal, &mother, 0, 1.0, 1000, 100.0);
  TEST_ASSERT(
      mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 2.0, MimicConfig.G, &trace) == -1.0,
      "a satellite of 0 particles returns -1");
  TEST_ASSERT(isinf(trace.coulomb) && trace.coulomb > 0.0,
              "the Coulomb logarithm is formed first, +inf, without a trap");
  return TEST_PASS;
}

static int test_zero_coulomb(void) {
  set_cosmology();
  struct Halo sat, mother;
  struct GalaxyData gal;
  make_pair(&sat, &gal, &mother, 100, 8.6, 0, 100.0);
  TEST_ASSERT(mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 2.0, MimicConfig.G, NULL) ==
                  -1.0,
              "a zero Coulomb logarithm returns -1");
  return TEST_PASS;
}

static int test_cap(void) {
  set_cosmology();
  struct Halo sat, mother;
  struct GalaxyData gal;
  struct MimicSageMergerClockTrace trace;
  /* A tiny satellite in a massive halo: the clock is far above 999 */
  make_pair(&sat, &gal, &mother, 10, 1e-6, 11, 1.0e5);
  gal.StellarMass = 0.0f;
  gal.ColdGas = 0.0f;
  const double t =
      mimic_sage_estimate_merging_time(&sat, &mother, REDSHIFT, 2.0, MimicConfig.G, &trace);
  TEST_ASSERT(trace.mergtime_pre_cap >= 999.0, "the uncapped clock is at least 999");
  TEST_ASSERT(t == 998.0 && trace.capped == 1, "the clock is capped to 998");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: shared/sage_merger_clock.h\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_same_halo);
  TEST_RUN(test_formula);
  TEST_RUN(test_particle_floor);
  TEST_RUN(test_invalid_mass);
  TEST_RUN(test_zero_satellite_len);
  TEST_RUN(test_zero_coulomb);
  TEST_RUN(test_cap);

  TEST_SUMMARY();
  return TEST_RESULT();
}
