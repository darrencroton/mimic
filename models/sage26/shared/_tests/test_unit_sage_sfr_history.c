/**
 * @file test_unit_sage_sfr_history.c
 * @brief Unit tests for the shared SFR history helper (shared/sage_sfr_history.h)
 *
 * Validates the package's representation of SAGE26's six per-substep histories
 * (R/src/core_allvars.h:158-163) as sixty float properties, and the helper's
 * transcriptions of SAGE26's bin map (R/src/core_build_model.c:514-515), its disk and
 * burst sampling operators (R/src/model_starformation_and_feedback.c:1058-1060,
 * R/src/model_mergers.c:755-757) and its divisors (R/src/model_misc.h:49-58). The
 * averaging is checked bit for bit against SAGE26 by sage26_finalize_sfr_histories'
 * oracle test.
 *
 * Test cases:
 *   - test_history_elements_distinct: the sixty (history, bin) pairs name sixty distinct
 *     float members; anything else is NULL
 *   - test_step_bin_map: SAGE26's bin for 7, 10, 11, 13 and 30 substeps, and invalid input
 *   - test_disk_sampling_operators: rate +=, gas samples = (the last substep of a bin wins)
 *   - test_bulge_sampling_operators: rate, gas and metals all +=
 *   - test_rate_rounds_once: the rate is the float element plus the double quotient,
 *     rounded once to float, as SAGE26's float `+=` of a double
 *   - test_sampling_rejects_invalid_bin: an invalid bin or galaxy writes nothing
 */

#include "../../../../tests/framework/test_framework.h"
#include "include/types.h"
#include "shared/sage_sfr_history.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/** @brief Element (history, bin) of a galaxy */
static float element(struct GalaxyData *gal, const enum MimicSageSfrHistory h, const int bin) {
  return *mimic_sage_sfr_history(gal, h, bin);
}

/**
 * @test    test_history_elements_distinct
 * @brief   Sixty distinct float members inside struct GalaxyData; NULL otherwise
 */
int test_history_elements_distinct(void) {
  static struct GalaxyData gal;
  const char *base = (const char *)&gal;
  float *seen[MIMIC_SFR_HISTORY_COUNT * MIMIC_SAGE_SFR_STEPS];
  int n = 0;
  for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
    for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
      float *p = mimic_sage_sfr_history(&gal, (enum MimicSageSfrHistory)h, k);
      TEST_ASSERT(p != NULL, "every (history, bin) has an element");
      TEST_ASSERT((const char *)p >= base &&
                      (const char *)p + sizeof(float) <= base + sizeof(struct GalaxyData),
                  "the element is a member of the galaxy");
      for (int j = 0; j < n; j++) {
        TEST_ASSERT(seen[j] != p, "no two (history, bin) pairs share an element");
      }
      seen[n++] = p;
    }
  }
  TEST_ASSERT(mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_DISK, -1) == NULL, "bin -1 is NULL");
  TEST_ASSERT(mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_DISK, 10) == NULL, "bin 10 is NULL");
  TEST_ASSERT(mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_COUNT, 0) == NULL,
              "an unknown history is NULL");
  TEST_ASSERT(mimic_sage_sfr_history(NULL, MIMIC_SFR_HISTORY_DISK, 0) == NULL,
              "a NULL galaxy is NULL");
  return TEST_PASS;
}

/**
 * @test    test_step_bin_map
 * @brief   (step * STEPS) / N capped at STEPS - 1, for N below, at and above STEPS
 */
int test_step_bin_map(void) {
  const struct {
    int n;
    int bins[30];
  } cases[] = {
      {7, {0, 1, 2, 4, 5, 7, 8}},
      {10, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9}},
      {11, {0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9}},
      {13, {0, 0, 1, 2, 3, 3, 4, 5, 6, 6, 7, 8, 9}},
      {30,
       {0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 9}},
  };
  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    for (int step = 0; step < cases[c].n; step++) {
      TEST_ASSERT(mimic_sage_sfr_step_bin(step, cases[c].n) == cases[c].bins[step],
                  "the bin is SAGE26's (step * STEPS) / effective_steps");
    }
  }
  TEST_ASSERT(mimic_sage_sfr_step_bin(-1, 10) == -1, "a negative substep is invalid");
  TEST_ASSERT(mimic_sage_sfr_step_bin(10, 10) == -1, "a substep past the count is invalid");
  TEST_ASSERT(mimic_sage_sfr_step_bin(0, 0) == -1, "no substeps is invalid");
  return TEST_PASS;
}

/**
 * @test    test_disk_sampling_operators
 * @brief   SfrDisk += stars / dt; SfrDiskColdGas and SfrDiskColdGasMetals assigned
 */
int test_disk_sampling_operators(void) {
  static struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  gal.ColdGas = 2.0f;
  gal.MetalsColdGas = 0.04f;
  TEST_ASSERT(mimic_sage_sample_disk_sfr(&gal, 6, 0.5, 0.25) == 0, "sampling succeeds");
  gal.ColdGas = 1.5f;
  gal.MetalsColdGas = 0.03f;
  TEST_ASSERT(mimic_sage_sample_disk_sfr(&gal, 6, 0.25, 0.25) == 0, "sampling succeeds");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_DISK, 6) == 3.0f, "the rates add: 2 + 1");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 6) == 1.5f,
              "the gas sample is the last substep's");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, 6) == 0.03f,
              "the metals sample is the last substep's");
  for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
    for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
      const int disk = h == MIMIC_SFR_HISTORY_DISK || h == MIMIC_SFR_HISTORY_DISK_COLD_GAS ||
                       h == MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS;
      if (!(disk && k == 6)) {
        TEST_ASSERT(element(&gal, (enum MimicSageSfrHistory)h, k) == 0.0f,
                    "no other element is touched");
      }
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_bulge_sampling_operators
 * @brief   SfrBulge, SfrBulgeColdGas and SfrBulgeColdGasMetals all accumulate
 */
int test_bulge_sampling_operators(void) {
  static struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  gal.ColdGas = 2.0f;
  gal.MetalsColdGas = 0.5f;
  TEST_ASSERT(mimic_sage_sample_bulge_sfr(&gal, 0, 1.0, 0.5) == 0, "sampling succeeds");
  gal.ColdGas = 1.0f;
  gal.MetalsColdGas = 0.25f;
  TEST_ASSERT(mimic_sage_sample_bulge_sfr(&gal, 0, 0.5, 0.5) == 0, "sampling succeeds");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_BULGE, 0) == 3.0f, "the rates add: 2 + 1");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, 0) == 3.0f, "the gas adds: 2 + 1");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, 0) == 0.75f,
              "the metals add: 0.5 + 0.25");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_DISK, 0) == 0.0f &&
                  element(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 0) == 0.0f,
              "the disk history is not touched");
  return TEST_PASS;
}

/**
 * @test    test_rate_rounds_once
 * @brief   float += double rounds the double sum once, not the quotient first
 *
 * With the element at 1 and the quotient q = 2^-24 + 2^-50 (just above half an ulp of
 * 1 in float), SAGE26's `SfrDisk[step] += stars / dt` forms 1 + q exactly in double
 * and rounds it up to 1 + 2^-23. Rounding the quotient to float first would give
 * exactly 2^-24, and 1 + 2^-24 is a tie that rounds to even, 1.
 */
int test_rate_rounds_once(void) {
  static struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  const double q = ldexp(1.0, -24) + ldexp(1.0, -50);
  *mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_DISK, 2) = 1.0f;
  *mimic_sage_sfr_history(&gal, MIMIC_SFR_HISTORY_BULGE, 2) = 1.0f;
  TEST_ASSERT(mimic_sage_sample_disk_sfr(&gal, 2, q, 1.0) == 0, "sampling succeeds");
  TEST_ASSERT(mimic_sage_sample_bulge_sfr(&gal, 2, q, 1.0) == 0, "sampling succeeds");
  const float once = (float)(1.0 + q);
  const float twice = 1.0f + (float)q;
  TEST_ASSERT(once != twice, "the case separates the two roundings");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_DISK, 2) == once, "disk rate rounds once");
  TEST_ASSERT(element(&gal, MIMIC_SFR_HISTORY_BULGE, 2) == once, "bulge rate rounds once");
  return TEST_PASS;
}

/**
 * @test    test_sampling_rejects_invalid_bin
 * @brief   An invalid bin or a NULL galaxy writes nothing and reports -1
 */
int test_sampling_rejects_invalid_bin(void) {
  static struct GalaxyData gal;
  memset(&gal, 0, sizeof(gal));
  gal.ColdGas = 1.0f;
  TEST_ASSERT(mimic_sage_sample_disk_sfr(&gal, -1, 1.0, 1.0) == -1, "bin -1 is rejected");
  TEST_ASSERT(mimic_sage_sample_bulge_sfr(&gal, 10, 1.0, 1.0) == -1, "bin 10 is rejected");
  TEST_ASSERT(mimic_sage_sample_disk_sfr(NULL, 0, 1.0, 1.0) == -1, "a NULL galaxy is rejected");
  for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
    for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
      TEST_ASSERT(element(&gal, (enum MimicSageSfrHistory)h, k) == 0.0f, "nothing is written");
    }
  }
  return TEST_PASS;
}

int main(void) {
  printf("============================================================\n");
  printf("UNIT TEST: SAGE26 SFR history helper\n");
  printf("============================================================\n");

  TEST_RUN(test_history_elements_distinct);
  TEST_RUN(test_step_bin_map);
  TEST_RUN(test_disk_sampling_operators);
  TEST_RUN(test_bulge_sampling_operators);
  TEST_RUN(test_rate_rounds_once);
  TEST_RUN(test_sampling_rejects_invalid_bin);

  TEST_SUMMARY();

  return TEST_RESULT();
}
