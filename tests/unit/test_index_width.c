/**
 * @file    test_index_width.c
 * @brief   Unit tests pinning the int64 halo-index contract of the input/driver seam
 *
 * Every halo index a driver computes -- from a link accessor, a slab loop or a
 * workspace offset -- is int64_t, so a horizontal slab above int32 can never be
 * narrowed silently. Nothing downstream can catch a regression here: every
 * dataset this suite runs fits in int32, so an accessor or field quietly put
 * back to `int` would produce bit-identical output. These tests therefore pin
 * the types themselves, at compile time where C allows it and on values at the
 * int32 boundary where it does not.
 *
 * Values above 2^31 cannot be stored in a committed package's `int` links, so
 * the accessor round trip above int32 is proven by the generator's
 * generate-and-compile test in tests/integration/test_unit_contract_generation.py
 * against `long long` link storage. The catalog names written here (Descendant,
 * FirstProgenitor, ...) are the ones every simulation package binds to the core
 * link roles, the convention tests/unit/test_input_view.c already relies on.
 */

#include "../framework/test_framework.h"
#include "../../src/core/inheritance.h"
#include "../../src/core/output_buffer.h"
#include "../../src/include/proto.h"
#include "../../src/include/types.h"
#include "../../src/include/generated/tree_property_accessors.h"
#include "../../src/util/error.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* 1 when `expr` has type int64_t exactly. _Generic does not evaluate its
 * controlling expression, so a function designator here is never called. */
#define IS_INT64(expr) _Generic((expr), int64_t: 1, default: 0)
#define IS_INT(expr) _Generic((expr), int: 1, default: 0)

/* 1 when function `fn` has exactly the pointer type `type`. */
#define HAS_SIGNATURE(fn, type) _Generic(&(fn), type: 1, default: 0)

/* A field wide enough for an int64 halo index and signed like one. Probes width and sign
 * rather than using _Generic because struct Halo.HaloNr is `long long`, which is not
 * int64_t on Linux/glibc (where int64_t is `long`). */
#define IS_SIGNED_64(lvalue) (sizeof(lvalue) == sizeof(int64_t) && (lvalue = -1, lvalue < 0))

/**
 * @test    test_link_accessors_take_and_return_int64
 * @brief   Every generated tree-link accessor is int64_t in and out
 */
int test_link_accessors_take_and_return_int64(void) {
  const struct HaloInputView view = {NULL, 0};

  TEST_ASSERT(IS_INT64(mimic_tree_get_Descendant(view, 0)), "Descendant accessor returns int64_t");
  TEST_ASSERT(IS_INT64(mimic_tree_get_FirstProgenitor(view, 0)),
              "FirstProgenitor accessor returns int64_t");
  TEST_ASSERT(IS_INT64(mimic_tree_get_NextProgenitor(view, 0)),
              "NextProgenitor accessor returns int64_t");
  TEST_ASSERT(IS_INT64(mimic_tree_get_FirstHaloInFOFgroup(view, 0)),
              "FirstHaloInFOFgroup accessor returns int64_t");
  TEST_ASSERT(IS_INT64(mimic_tree_get_NextHaloInFOFgroup(view, 0)),
              "NextHaloInFOFgroup accessor returns int64_t");

  /* Index and count roles keep int storage and return type; only their halo
   * index parameter widens. */
  TEST_ASSERT(IS_INT(mimic_tree_get_SnapNum(view, 0)), "SnapNum accessor still returns int");
  TEST_ASSERT(IS_INT(mimic_tree_get_Len(view, 0)), "Len accessor still returns int");

  typedef int64_t (*link_accessor)(struct HaloInputView, int64_t);
  typedef int (*int_accessor)(struct HaloInputView, int64_t);
  typedef double (*mass_accessor)(struct HaloInputView, int64_t);
  TEST_ASSERT(HAS_SIGNATURE(mimic_tree_get_Descendant, link_accessor),
              "Descendant accessor takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(mimic_tree_get_FirstProgenitor, link_accessor),
              "FirstProgenitor accessor takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(mimic_tree_get_NextProgenitor, link_accessor),
              "NextProgenitor accessor takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(mimic_tree_get_FirstHaloInFOFgroup, link_accessor),
              "FirstHaloInFOFgroup accessor takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(mimic_tree_get_NextHaloInFOFgroup, link_accessor),
              "NextHaloInFOFgroup accessor takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(mimic_tree_get_SnapNum, int_accessor),
              "SnapNum accessor takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(mimic_tree_get_Len, int_accessor),
              "Len accessor takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(mimic_tree_get_HaloMass, mass_accessor),
              "HaloMass accessor takes an int64_t halo index");

  return TEST_PASS;
}

/**
 * @test    test_link_accessors_preserve_int32_boundary_values
 * @brief   int-stored links widen exactly: INT32_MAX stays positive, -1 stays -1
 */
int test_link_accessors_preserve_int32_boundary_values(void) {
  struct RawHalo halos[2];
  memset(halos, 0, sizeof(halos));

  halos[1].Descendant = INT32_MAX;
  halos[1].FirstProgenitor = -1;
  halos[1].NextProgenitor = INT32_MIN;
  halos[1].FirstHaloInFOFgroup = INT32_MAX - 1;
  halos[1].NextHaloInFOFgroup = -1;

  const struct HaloInputView view = {halos, 2};
  const int64_t index = 1;

  TEST_ASSERT(mimic_tree_get_Descendant(view, index) == (int64_t)INT32_MAX,
              "INT32_MAX link widens without sign change");
  TEST_ASSERT(mimic_tree_get_FirstProgenitor(view, index) == -1,
              "the -1 no-link sentinel sign-extends to int64 -1");
  TEST_ASSERT(mimic_tree_get_NextProgenitor(view, index) == (int64_t)INT32_MIN,
              "INT32_MIN link widens without sign change");
  TEST_ASSERT(mimic_tree_get_FirstHaloInFOFgroup(view, index) == (int64_t)INT32_MAX - 1,
              "FoF link widens exactly");
  TEST_ASSERT(mimic_tree_get_NextHaloInFOFgroup(view, index) == -1,
              "the FoF chain terminator sign-extends to int64 -1");

  return TEST_PASS;
}

/**
 * @test    test_seam_signatures_are_int64
 * @brief   Every shared driver, inheritance, output-buffer and virial index is int64_t
 *
 * Checked through the function types, so none of these functions is called or
 * needs to be linked (build_model.c is deliberately not in the unit harness).
 */
int test_seam_signatures_are_int64(void) {
  TEST_ASSERT(HAS_SIGNATURE(build_halo_tree, void (*)(int64_t, int, int)),
              "build_halo_tree takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(join_progenitor_halos,
                            int64_t (*)(struct HaloInputView, int64_t, int64_t, int)),
              "join_progenitor_halos takes and returns int64_t indices");
  TEST_ASSERT(
      HAS_SIGNATURE(find_most_massive_progenitor, int64_t (*)(struct HaloInputView, int64_t)),
      "find_most_massive_progenitor takes and returns int64_t");
  TEST_ASSERT(HAS_SIGNATURE(count_fof_subhalos, int64_t (*)(struct HaloInputView, int64_t)),
              "count_fof_subhalos takes and returns int64_t");
  TEST_ASSERT(HAS_SIGNATURE(make_halo_init_payload,
                            struct HaloInitPayload (*)(struct HaloInputView, int64_t)),
              "make_halo_init_payload takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(process_halo_evolution,
                            void (*)(struct HaloInputView, struct Halo *, int64_t, int64_t)),
              "process_halo_evolution takes int64_t halo index and count");
  TEST_ASSERT(HAS_SIGNATURE(get_virial_mass, double (*)(struct HaloInputView, int64_t)),
              "get_virial_mass takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(get_virial_radius, double (*)(struct HaloInputView, int64_t)),
              "get_virial_radius takes an int64_t halo index");
  TEST_ASSERT(HAS_SIGNATURE(get_virial_velocity, double (*)(struct HaloInputView, int64_t)),
              "get_virial_velocity takes an int64_t halo index");
  TEST_ASSERT(
      HAS_SIGNATURE(horizontal_find_most_massive_progenitor,
                    struct HorizontalProgenitorRef (*)(
                        struct HaloInputView, const struct HorizontalGatherContext *, int64_t)),
      "horizontal_find_most_massive_progenitor takes an int64_t index and names its "
      "answer by generation and row");
  TEST_ASSERT(sizeof(((struct HorizontalProgenitorRef *)0)->snapnum) == sizeof(int64_t) &&
                  sizeof(((struct HorizontalProgenitorRef *)0)->halonr) == sizeof(int64_t),
              "HorizontalProgenitorRef carries int64_t snapshot and row");
  TEST_ASSERT(HAS_SIGNATURE(horizontal_count_progenitor_galaxies,
                            int64_t (*)(struct HaloInputView,
                                        const struct HorizontalGatherContext *, int64_t)),
              "horizontal_count_progenitor_galaxies takes and returns int64_t");
  TEST_ASSERT(
      HAS_SIGNATURE(horizontal_gather_progenitor_galaxies,
                    void (*)(struct HaloInputView, const struct HorizontalGatherContext *, int64_t,
                             struct HorizontalProgenitorRef, struct InheritanceProgenitorGalaxy *)),
      "horizontal_gather_progenitor_galaxies takes int64_t indices");
  TEST_ASSERT(HAS_SIGNATURE(inherit_descendant_halos,
                            int64_t (*)(struct GalaxyPool *, struct Halo *, int64_t, int64_t,
                                        const struct InheritanceDescendant *,
                                        const struct InheritanceProgenitorGalaxy *, int64_t)),
              "inherit_descendant_halos takes and returns int64_t workspace offsets");
  TEST_ASSERT(HAS_SIGNATURE(marshal_workspace_to_output_buffer,
                            void (*)(struct Halo *, struct OutputBuffer *,
                                     struct OutputBufferSegment *, int64_t)),
              "marshal_workspace_to_output_buffer takes an int64_t segment count");

  return TEST_PASS;
}

/**
 * @test    test_index_fields_are_signed_int64
 * @brief   Struct fields that hold a halo index or an index-derived offset are int64
 */
int test_index_fields_are_signed_int64(void) {
  struct Halo halo;
  struct HaloAuxData aux;
  struct HorizontalHaloAux horizontal_aux;
  struct OutputBufferSegment segment;
  struct InheritanceDescendant descendant;

  TEST_ASSERT(IS_SIGNED_64(halo.HaloNr), "struct Halo.HaloNr is a signed 64-bit index");
  TEST_ASSERT(IS_SIGNED_64(aux.FirstHalo), "struct HaloAuxData.FirstHalo is int64");
  TEST_ASSERT(IS_SIGNED_64(aux.NHalos), "struct HaloAuxData.NHalos is int64");
  TEST_ASSERT(IS_SIGNED_64(horizontal_aux.FirstHalo),
              "struct HorizontalHaloAux.FirstHalo is int64");
  TEST_ASSERT(IS_SIGNED_64(horizontal_aux.NHalos), "struct HorizontalHaloAux.NHalos is int64");
  TEST_ASSERT(IS_SIGNED_64(segment.source_id), "OutputBufferSegment.source_id is int64");
  TEST_ASSERT(IS_SIGNED_64(segment.workspace_start),
              "OutputBufferSegment.workspace_start is int64");
  TEST_ASSERT(IS_SIGNED_64(segment.workspace_count),
              "OutputBufferSegment.workspace_count is int64");
  TEST_ASSERT(IS_SIGNED_64(segment.output_first), "OutputBufferSegment.output_first is int64");
  TEST_ASSERT(IS_SIGNED_64(segment.output_count), "OutputBufferSegment.output_count is int64");
  TEST_ASSERT(IS_SIGNED_64(descendant.halo_nr), "InheritanceDescendant.halo_nr is int64");

  /* The whole int64 range survives a store, not only the sign bit. */
  halo.HaloNr = INT64_MAX;
  TEST_ASSERT(halo.HaloNr == INT64_MAX, "struct Halo.HaloNr holds INT64_MAX exactly");

  return TEST_PASS;
}

/**
 * @test    test_fof_count_is_int64
 * @brief   count_fof_subhalos() walks a FoF chain and reports its length as int64_t
 */
int test_fof_count_is_int64(void) {
  struct RawHalo halos[3];
  memset(halos, 0, sizeof(halos));

  for (int i = 0; i < 3; i++) {
    halos[i].FirstHaloInFOFgroup = 0;
    halos[i].NextHaloInFOFgroup = (i < 2) ? i + 1 : -1;
  }

  const struct HaloInputView view = {halos, 3};
  const int64_t count = count_fof_subhalos(view, 0);

  TEST_ASSERT(IS_INT64(count_fof_subhalos(view, 0)), "count_fof_subhalos returns int64_t");
  TEST_ASSERT_EQUAL(count, 3, "the three-member FoF chain is counted exactly");

  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Halo Index Width (int64 input/driver seam)\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_link_accessors_take_and_return_int64);
  TEST_RUN(test_link_accessors_preserve_int32_boundary_values);
  TEST_RUN(test_seam_signatures_are_int64);
  TEST_RUN(test_index_fields_are_signed_int64);
  TEST_RUN(test_fof_count_is_int64);

  TEST_SUMMARY();
  return TEST_RESULT();
}
