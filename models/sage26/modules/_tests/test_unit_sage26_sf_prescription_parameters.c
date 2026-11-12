/**
 * @file    test_unit_sage26_sf_prescription_parameters.c
 * @brief   What the supported SFprescription values select: whether the hydrogen caps track H2
 *
 * The package supports SAGE26's SFprescription 0 (Croton et al. 2006) and 1 (Blitz & Rosolowsky
 * 2006 with radial integration). The modules whose commits cap the cold-gas hydrogen split (the
 * star-formation, burst and quasar paths) apply the H2 cap only where the prescription tracks H2;
 * that guard is sage26's sf_prescription_tracks_h2. The refusal of every other value, of the H2
 * integration parameters and of StarburstColdGasOn, and the acceptance of the supported ones, are
 * in test_unit_sage26_parameter_refusals.c.
 *
 * Test cases:
 *   - test_tracks_h2: prescription 0 does not track H2 and prescription 1 does
 */

#include "../../../../tests/framework/test_framework.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_h2_physics.h"

/**
 * @test    test_tracks_h2
 * @brief   The caps' guard is SAGE26's sf_prescription_tracks_h2: false for the Croton
 *          prescription 0, true for the Blitz and Rosolowsky prescription 1
 */
int test_tracks_h2(void) {
  TEST_ASSERT(!mimic_sage_sf_prescription_tracks_h2(0), "prescription 0 does not track H2");
  TEST_ASSERT(mimic_sage_sf_prescription_tracks_h2(1), "prescription 1 tracks H2");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: SAGE26 SF prescriptions and the H2 caps\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);

  TEST_RUN(test_tracks_h2);

  TEST_SUMMARY();
  return TEST_RESULT();
}
