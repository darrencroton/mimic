/**
 * @file    test_unit_sage_keyed_draw.c
 * @brief   Unit tests of the package's keyed uniform draw (shared/sage_keyed_draw.h)
 *
 * The keyed draw replaces SAGE26's libc draw in the CGM regime classifier. Its key contract is
 * stated in shared/sage_keyed_draw.h; the eight reference vectors were computed with an
 * independent Python implementation of that contract. The test uses integer arithmetic and exact
 * double conversions only.
 *
 * Test cases:
 *   - test_reference_vectors: the eight vectors, `bits` and the deviate, bit for bit (they vary
 *     the UniqueGalaxyID, snapshot and ordinal, with an extreme and a negative ID); for each
 *     ordinal-0 vector the classifier's wrapper (CGM purpose, ordinal 0) gives the same deviate;
 *     a different purpose salt changes the bits; the deviate's mapping at the extreme bit
 *     patterns (2^-53 and 1 - 2^-53) is exact and the low 12 bits do not enter it
 */

#include "../../../../tests/framework/test_framework.h"
#include "shared/sage_keyed_draw.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, bit patterns) */
#include "modules/_tests/sage_test_fixtures.h"

/** One reference vector of the keyed draw */
struct draw_vector {
  int64_t galaxy_id;
  int snapshot;
  uint64_t ordinal;
  uint64_t bits;  /* the expected keyed bits */
  double deviate; /* the expected deviate, as a hexadecimal double */
};

static const struct draw_vector VECTORS[] = {
    {0, 0, 0, UINT64_C(0x600f3d65cab0d697), 0x1.803cf5972ac36p-2},
    {1, 63, 0, UINT64_C(0x0916b00c4a9b61fe), 0x1.22d60189536d0p-5},
    {INT64_C(1000000000123), 37, 0, UINT64_C(0x060f98fcec722e1f), 0x1.83e63f3b1c8a0p-6},
    {-5, 8, 0, UINT64_C(0x9c1fad9e65e48c21), 0x1.383f5b3ccbc91p-1},
    {INT64_C(1000000000123), 38, 0, UINT64_C(0x64309bb56590e1f3), 0x1.90c26ed59643ap-2},
    {INT64_C(1000000000124), 37, 0, UINT64_C(0x5f00daf2f53b6350), 0x1.7c036bcbd4edap-2},
    {INT64_C(1000000000123), 37, 1, UINT64_C(0x54c26ced2ceb1f4a), 0x1.5309b3b4b3ac6p-2},
    {INT64_C(9223372036854775807), 63, 0, UINT64_C(0x94aaf0f9d5a99164), 0x1.2955e1f3ab533p-1},
};

#define NUM_VECTORS ((int)(sizeof(VECTORS) / sizeof(VECTORS[0])))

/**
 * @test    test_reference_vectors
 * @brief   The eight reference vectors bit for bit, the wrapper, the purpose salt and the
 *          deviate mapping's extremes
 */
int test_reference_vectors(void) {
  int mismatches = 0;
  for (int i = 0; i < NUM_VECTORS; i++) {
    const struct draw_vector *v = &VECTORS[i];
    const uint64_t bits =
        mimic_sage_keyed_bits(v->galaxy_id, v->snapshot, MIMIC_SAGE_DRAW_PURPOSE_CGM, v->ordinal);
    const double u = mimic_sage_keyed_uniform(v->galaxy_id, v->snapshot,
                                              MIMIC_SAGE_DRAW_PURPOSE_CGM, v->ordinal);
    int ok = bits == v->bits && double_bits(u) == double_bits(v->deviate);
    if (v->ordinal == MIMIC_SAGE_DRAW_ORDINAL_CGM) {
      ok = ok && double_bits(mimic_sage_cgm_regime_deviate(v->galaxy_id, v->snapshot)) ==
                     double_bits(v->deviate);
    }
    /* The purpose salt is part of the key */
    ok = ok && mimic_sage_keyed_bits(v->galaxy_id, v->snapshot,
                                     MIMIC_SAGE_DRAW_PURPOSE_CGM ^ UINT64_C(1), v->ordinal) != bits;
    printf("\n  (%lld, %d, %llu) -> bits 0x%016llx deviate %a (expected 0x%016llx %a) %s",
           (long long)v->galaxy_id, v->snapshot, (unsigned long long)v->ordinal,
           (unsigned long long)bits, u, (unsigned long long)v->bits, v->deviate,
           ok ? "ok" : "MISMATCH");
    mismatches += !ok;
  }
  TEST_ASSERT(mismatches == 0,
              "every reference vector must be reproduced bit for bit, by the CGM wrapper too, and "
              "must change with the purpose salt");
  TEST_ASSERT(MIMIC_SAGE_DRAW_ORDINAL_CGM == 0, "the classifier's ordinal must be 0");

  /* The extremes of the mapping: all-zero bits give 2^-53, all-one bits 1 - 2^-53 */
  const double lowest = mimic_sage_keyed_uniform_from_bits(UINT64_C(0));
  const double highest = mimic_sage_keyed_uniform_from_bits(UINT64_MAX);
  printf("\n  extremes: %a, %a", lowest, highest);
  TEST_ASSERT(double_bits(lowest) == double_bits(0x1.0p-53), "all-zero bits must give 2^-53");
  TEST_ASSERT(double_bits(highest) == double_bits(1.0 - 0x1.0p-53),
              "all-one bits must give 1 - 2^-53");

  /* The low 12 bits are discarded; the next bit up is the lowest one that enters */
  TEST_ASSERT(double_bits(mimic_sage_keyed_uniform_from_bits(UINT64_C(0xfff))) ==
                  double_bits(lowest),
              "the low 12 bits must not enter the deviate");
  TEST_ASSERT(double_bits(mimic_sage_keyed_uniform_from_bits(UINT64_C(0x1000))) ==
                  double_bits(1.5 * 0x1.0p-52),
              "bit 12 must be the lowest bit of the deviate");
  return TEST_PASS;
}

int main(void) {
  printf("============================================================\n");
  printf("UNIT TEST: Keyed uniform draw (sage_keyed_draw.h, D1)\n");
  printf("============================================================\n");

  TEST_RUN(test_reference_vectors);

  TEST_SUMMARY();

  return TEST_RESULT();
}
