/**
 * @file test_unit_metallicity.c
 * @brief Unit tests for the shared metallicity calculation utility
 *
 * Validates mimic_get_metallicity (shared/metallicity.h), the safe Z = M_metals / M_gas used by
 * several physics modules. SAGE parity (model_misc.c get_metallicity): the arithmetic is double,
 * the result is 0 unless both gas > 0 and metals > 0, and it is capped at 1.
 *
 * Test cases:
 *   - test_metallicity_table: one table of (gas, metals, expected Z) rows covering ordinary
 *     ratios (solar and multiples, large and small masses, no epsilon cutoff on tiny gas), the
 *     cap at 1 (metals above and exactly equal to gas), and every guard (zero or negative gas,
 *     zero or negative metals, both zero, both negative). Ratios are compared exactly: the
 *     expected value is the double quotient written out from the row's own inputs.
 */

#include "../../../../tests/framework/test_framework.h"
#include "shared/metallicity.h"
#include <stdio.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/**
 * @test    test_metallicity_table
 * @brief   Z = metals / gas for gas > 0 and metals > 0, capped at 1; otherwise exactly 0
 *
 * A row's name is the failure message, so a failing row identifies itself.
 */
int test_metallicity_table(void) {
  static const struct {
    const char *name;
    double gas;
    double metals;
    double expected;
  } rows[] = {
      /* Ordinary ratios */
      {"2 in 100 is solar, Z = 0.02", 100.0, 2.0, 2.0 / 100.0},
      {"0.2 in 100 is a tenth of solar", 100.0, 0.2, 0.2 / 100.0},
      {"10 in 100 is five times solar", 100.0, 10.0, 10.0 / 100.0},
      {"6 in 100 is three times solar", 100.0, 6.0, 6.0 / 100.0},
      {"small but valid gas mass", 0.01, 0.0001, 0.0001 / 0.01},
      {"very large masses", 1e10, 1e8, 1e8 / 1e10},
      {"very small masses", 1e-8, 1e-10, 1e-10 / 1e-8},
      {"tiny gas above 1e-10 computes a ratio", 1.1e-10, 1e-12, 1e-12 / 1.1e-10},
      {"tiny gas below 1e-10 has no epsilon cutoff", 0.9e-10, 1e-12, 1e-12 / 0.9e-10},
      {"very small metals give a small Z, not 0", 100.0, 1e-12, 1e-12 / 100.0},
      {"a binary fraction is exact", 4.0, 2.0, 0.5},
      {"a non-terminating ratio is the rounded quotient", 3.0, 1.0, 1.0 / 3.0},
      /* The cap at 1 */
      {"metals above gas cap at 1", 10.0, 20.0, 1.0},
      {"metals exactly equal to gas give 1", 20.0, 20.0, 1.0},
      {"tiny gas with larger metals caps at 1", 1e-12, 1.0, 1.0},
      /* Guards: gas <= 0 or metals <= 0 returns exactly 0 */
      {"zero gas", 0.0, 5.0, 0.0},
      {"negative gas", -1.0, 1.0, 0.0},
      {"zero metals (primordial gas)", 100.0, 0.0, 0.0},
      {"negative metals never give a negative Z", 100.0, -1.0, 0.0},
      {"zero gas and zero metals", 0.0, 0.0, 0.0},
      {"negative gas and negative metals do not divide to +Z", -1.0, -1.0, 0.0},
  };

  for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
    const double Z = mimic_get_metallicity(rows[i].gas, rows[i].metals);
    TEST_ASSERT(Z == rows[i].expected, rows[i].name);
  }

  return TEST_PASS;
}

int main(void) {
  printf("============================================================\n");
  printf("UNIT TEST: Metallicity Calculations\n");
  printf("============================================================\n");

  TEST_RUN(test_metallicity_table);

  TEST_SUMMARY();

  return TEST_RESULT();
}
