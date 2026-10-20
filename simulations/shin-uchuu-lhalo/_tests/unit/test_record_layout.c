/**
 * @file    test_record_layout.c
 * @brief   Pins the compiled struct RawHalo to the L-Halo record the converter writes
 *
 * Validates: the shin-uchuu-lhalo record layout (size and every member offset)
 *
 * The lhalo_binary reader reads each forest's records straight into
 * struct RawHalo, so the compiled struct is the record on disk. The converter
 * (convert/mimic-convert/write_vertical_trees.py) writes that record from its own
 * derivation of the same halo_properties.yaml with C alignment
 * (convert/mimic-convert/vertical/record.py, record digest
 * ab04dcbae52ffd11b1e773fe3c9aafecc22371f4583eeadf2353930e5bdf5e64). The size
 * and offsets below are that derivation's, typed in literally, so a package or
 * generator change that moves the compiled layout away from the converted files
 * fails here rather than misreading them.
 *
 * Test cases:
 *   - test_record_layout: sizeof(struct RawHalo) is 88 and every member sits at
 *     the converter's offset
 *
 * Skips, naming the compiled simulation, unless SIMULATION=shin-uchuu-lhalo.
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../src/include/types.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

#define PACKAGE "shin-uchuu-lhalo"

/* The converter's record: 14 members, 88 bytes, 4 bytes of trailing padding. */
#define RECORD_SIZE 88

struct MemberOffset {
  const char *name;
  size_t compiled;
  size_t converter;
};

/**
 * @test    test_record_layout
 * @brief   The compiled record matches the converter's derivation byte for byte
 *
 * Expected: sizeof(struct RawHalo) == 88 and each offsetof equals the literal
 * Validates: the lhalo_binary reader reads converted files as they were written
 */
int test_record_layout(void) {
  static char reason[MAX_STRING_LEN];
  const char *compiled = MIMIC_COMPILED_SIMULATION;
  if (strcmp(compiled, PACKAGE) != 0) {
    snprintf(reason, sizeof(reason),
             "compiled simulation is %s, not " PACKAGE "; the record pinned here is " PACKAGE "'s",
             compiled);
    return TEST_SKIP_WITH(reason);
  }

  const struct MemberOffset members[] = {
      {"Descendant", offsetof(struct RawHalo, Descendant), 0},
      {"FirstProgenitor", offsetof(struct RawHalo, FirstProgenitor), 4},
      {"NextProgenitor", offsetof(struct RawHalo, NextProgenitor), 8},
      {"FirstHaloInFOFgroup", offsetof(struct RawHalo, FirstHaloInFOFgroup), 12},
      {"NextHaloInFOFgroup", offsetof(struct RawHalo, NextHaloInFOFgroup), 16},
      {"Len", offsetof(struct RawHalo, Len), 20},
      {"M_Crit200", offsetof(struct RawHalo, M_Crit200), 24},
      {"Pos", offsetof(struct RawHalo, Pos), 28},
      {"Vel", offsetof(struct RawHalo, Vel), 40},
      {"Spin", offsetof(struct RawHalo, Spin), 52},
      {"VelDisp", offsetof(struct RawHalo, VelDisp), 64},
      {"Vmax", offsetof(struct RawHalo, Vmax), 68},
      {"MostBoundID", offsetof(struct RawHalo, MostBoundID), 72},
      {"SnapNum", offsetof(struct RawHalo, SnapNum), 80},
  };

  TEST_ASSERT_EQUAL((int)sizeof(struct RawHalo), RECORD_SIZE,
                    "sizeof(struct RawHalo) must equal the converter's 88-byte record");
  for (size_t i = 0; i < sizeof(members) / sizeof(members[0]); i++) {
    if (members[i].compiled != members[i].converter) {
      printf("  %s: compiled offset %zu, converter offset %zu\n", members[i].name,
             members[i].compiled, members[i].converter);
    }
    TEST_ASSERT_EQUAL((int)members[i].compiled, (int)members[i].converter,
                      "a RawHalo member offset differs from the converter's record");
  }

  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: shin-uchuu-lhalo Record Layout\n");
  printf("============================================================\n");
  printf("%s", NC);

  TEST_RUN(test_record_layout);

  TEST_SUMMARY();
  return TEST_RESULT();
}
