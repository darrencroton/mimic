/**
 * @file    test_input_view.c
 * @brief   Unit tests for the explicit halo input view (struct HaloInputView)
 *
 * Validates: accessors, virial helpers, the generated payload populator, and
 * output conversion all read the raw halos they are *handed*, not a global.
 *
 * A bitwise run comparison cannot see this: on the tree path the view always
 * points at the same array the global does, so a helper that quietly kept
 * reading the global would produce identical output. These tests therefore
 * build **two** RawHalo arrays with deliberately different values at the same
 * indices, drive each layer through one view while the unrelated
 * InputTreeHalos global points at the other array, and assert that reads
 * through view A never return view B's values (and the reverse).
 *
 * Catalog field names used here (M_Crit200, FirstHaloInFOFgroup, Len, SnapNum,
 * Vmax) are core-role fields declared by every simulation package this suite
 * builds against, matching the convention in test_virial_properties.c.
 */

#include "../framework/test_framework.h"
#include "../../src/include/types.h"
#include "../../src/include/proto.h"
#include "../../src/include/generated/tree_property_accessors.h"
#include "../../src/io/output/util.h"
#include "../../src/util/memory.h"
#include "../../src/util/error.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* The tree driver's input storage. These tests deliberately point it at the
 * *wrong* array so that any layer still reading it fails loudly. */
extern struct RawHalo *InputTreeHalos;
extern double *Age_base; /* For cleanup of init() allocation */

/* Shared core-test fixtures (config reset, registration, generated run file path) */
#include "../framework/core_test_fixtures.h"

#define VIEW_TEST_NHALOS 2

/*
 * "These are different values" for a package-agnostic test. A fixed absolute
 * epsilon will not do: the catalog-to-reference unit conversion differs per
 * simulation package (mini-millennium stores mass already in 1e10 Msun/h,
 * micro-uchuu stores native Msun/h and converts by 1e-10), so the same seed
 * lands eighteen orders of magnitude apart between packages. The seeds below
 * differ by whole factors, so a relative test discriminates in every package.
 */
static int values_are_distinct(double a, double b) {
  return fabs(a - b) > 1.0e-9 * fmax(fabs(a), fabs(b));
}

/*
 * Mirror of the tree driver's make_halo_init_payload() (build_model.c). The
 * generated populator is an .inc included into a function body that declares
 * `payload` and has `view` in scope, so reproducing that shape here is the only
 * way to exercise the generated emission directly.
 */
static struct HaloInitPayload populate_payload_through_view(struct HaloInputView view, int halonr) {
  struct HaloInitPayload payload;

#include "../../src/include/generated/populate_halo_payload.inc"

  return payload;
}

/* Seed one fixture array. Every halo is its own FoF central with a positive
 * catalog mass, so get_virial_mass() takes the catalog path and the two arrays
 * stay distinguishable through every derived quantity. */
static void seed_fixture_array(struct RawHalo *halos, double catalog_mass, int len, int snapnum,
                               float vmax) {
  memset(halos, 0, VIEW_TEST_NHALOS * sizeof(struct RawHalo));

  for (int i = 0; i < VIEW_TEST_NHALOS; i++) {
    halos[i].FirstHaloInFOFgroup = i;
    halos[i].SnapNum = snapnum;
    halos[i].Len = len * (i + 1);
    halos[i].M_Crit200 = catalog_mass * (i + 1);
    halos[i].Vmax = vmax + (float)i;
  }
}

/**
 * @test    test_view_accessor_and_virial_reads_are_independent
 * @brief   Interleaved reads through two views never cross over
 *
 * Expected: every accessor and virial helper returns the handed view's values
 * Validates: mimic_tree_get_*(), get_virial_mass/radius/velocity()
 */
int test_view_accessor_and_virial_reads_are_independent(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  read_parameter_file(test_binary_param_file());
  init(); /* Initializes cosmology and ZZ[] array */

  struct RawHalo *halos_a = mymalloc_cat(VIEW_TEST_NHALOS * sizeof(struct RawHalo), MEM_HALOS);
  struct RawHalo *halos_b = mymalloc_cat(VIEW_TEST_NHALOS * sizeof(struct RawHalo), MEM_HALOS);
  const int snap_a = MimicConfig.LastSnapshotNr;
  const int snap_b = (MimicConfig.LastSnapshotNr > 0) ? MimicConfig.LastSnapshotNr - 1 : 0;

  seed_fixture_array(halos_a, 100.0, 10, snap_a, 150.0f);
  seed_fixture_array(halos_b, 800.0, 70, snap_b, 450.0f);

  const struct HaloInputView view_a = {halos_a, VIEW_TEST_NHALOS};
  const struct HaloInputView view_b = {halos_b, VIEW_TEST_NHALOS};

  /* ===== EXECUTE & VALIDATE ===== */
  for (int i = 0; i < VIEW_TEST_NHALOS; i++) {
    /* Interleave A and B at the same index: a cached view, a stale pointer, or
     * a global read would show up as a crossed-over value here. */
    TEST_ASSERT(mimic_tree_get_Len(view_a, i) == halos_a[i].Len,
                "Len through view A should return view A's value");
    TEST_ASSERT(mimic_tree_get_Len(view_b, i) == halos_b[i].Len,
                "Len through view B should return view B's value");
    TEST_ASSERT(mimic_tree_get_Len(view_a, i) != mimic_tree_get_Len(view_b, i),
                "Fixtures must differ at this index — adjust the seeds if this fires");

    TEST_ASSERT(mimic_tree_get_SnapNum(view_a, i) == snap_a,
                "SnapNum through view A should return view A's snapshot");
    TEST_ASSERT(mimic_tree_get_SnapNum(view_b, i) == snap_b,
                "SnapNum through view B should return view B's snapshot");

    const double mass_a = get_virial_mass(view_a, i);
    const double mass_b = get_virial_mass(view_b, i);

    TEST_ASSERT_DOUBLE_EQUAL(mass_a, mimic_tree_get_HaloMass(view_a, i), 1e-12,
                             "Virial mass through view A should be view A's catalog mass");
    TEST_ASSERT_DOUBLE_EQUAL(mass_b, mimic_tree_get_HaloMass(view_b, i), 1e-12,
                             "Virial mass through view B should be view B's catalog mass");
    TEST_ASSERT(values_are_distinct(mass_a, mass_b), "Virial masses of the two views must differ");

    const double rvir_a = get_virial_radius(view_a, i);
    const double rvir_b = get_virial_radius(view_b, i);
    const double vvir_a = get_virial_velocity(view_a, i);
    const double vvir_b = get_virial_velocity(view_b, i);

    TEST_ASSERT(isfinite(rvir_a) && isfinite(rvir_b), "Virial radii should be finite");
    TEST_ASSERT(isfinite(vvir_a) && isfinite(vvir_b), "Virial velocities should be finite");
    TEST_ASSERT(rvir_a > 0.0 && rvir_b > 0.0, "Virial radii should be positive");
    TEST_ASSERT(values_are_distinct(rvir_a, rvir_b), "Virial radii of the two views must differ");
    TEST_ASSERT(values_are_distinct(vvir_a, vvir_b),
                "Virial velocities of the two views must differ");

    /* Re-read A after B: proves the B reads left nothing behind. */
    TEST_ASSERT_DOUBLE_EQUAL(get_virial_radius(view_a, i), rvir_a, 0.0,
                             "Re-reading view A after view B must be unchanged");

    printf("  halo %d: view A Mvir=%g Rvir=%g | view B Mvir=%g Rvir=%g\n", i, mass_a, rvir_a,
           mass_b, rvir_b);
  }

  /* ===== CLEANUP ===== */
  myfree(halos_b);
  myfree(halos_a);
  myfree(Age_base); /* Free Age array allocated by init() */
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_output_conversion_reads_view_not_global
 * @brief   prepare_halo_for_output() ignores InputTreeHalos entirely
 *
 * Expected: recomputed Rvir/Vvir match the handed view while the global points
 *           at the other array
 * Validates: prepare_halo_for_output(), output_rvir_conditional(),
 *            output_vvir_conditional()
 */
int test_output_conversion_reads_view_not_global(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  read_parameter_file(test_binary_param_file());
  init();

  struct RawHalo *halos_a = mymalloc_cat(VIEW_TEST_NHALOS * sizeof(struct RawHalo), MEM_HALOS);
  struct RawHalo *halos_b = mymalloc_cat(VIEW_TEST_NHALOS * sizeof(struct RawHalo), MEM_HALOS);

  seed_fixture_array(halos_a, 100.0, 10, MimicConfig.LastSnapshotNr, 150.0f);
  seed_fixture_array(halos_b, 800.0, 70, MimicConfig.LastSnapshotNr, 450.0f);

  const struct HaloInputView view_a = {halos_a, VIEW_TEST_NHALOS};
  const struct HaloInputView view_b = {halos_b, VIEW_TEST_NHALOS};

  /* Type 0 so the conditional helpers recompute from the input rather than
   * preserving the stored orphan values. */
  struct GalaxyData galaxy;
  struct Halo halo;
  memset(&galaxy, 0, sizeof(galaxy));
  memset(&halo, 0, sizeof(halo));
  halo.Type = 0;
  halo.HaloNr = 1;
  halo.dT = -1.0; /* sentinel: no unit conversion applied */
  halo.galaxy = &galaxy;

  /* ===== EXECUTE ===== */
  /* Point the unrelated global at the *other* array in each direction. */
  InputTreeHalos = halos_b;
  struct HaloOutput output_a;
  memset(&output_a, 0, sizeof(output_a));
  prepare_halo_for_output(view_a, &halo, &output_a);

  InputTreeHalos = halos_a;
  struct HaloOutput output_b;
  memset(&output_b, 0, sizeof(output_b));
  prepare_halo_for_output(view_b, &halo, &output_b);

  InputTreeHalos = NULL;

  /* ===== VALIDATE ===== */
  TEST_ASSERT_DOUBLE_EQUAL(output_a.Rvir, get_virial_radius(view_a, halo.HaloNr), 0.0,
                           "Output Rvir through view A must come from view A");
  TEST_ASSERT_DOUBLE_EQUAL(output_a.Vvir, get_virial_velocity(view_a, halo.HaloNr), 0.0,
                           "Output Vvir through view A must come from view A");
  TEST_ASSERT_DOUBLE_EQUAL(output_b.Rvir, get_virial_radius(view_b, halo.HaloNr), 0.0,
                           "Output Rvir through view B must come from view B");
  TEST_ASSERT_DOUBLE_EQUAL(output_b.Vvir, get_virial_velocity(view_b, halo.HaloNr), 0.0,
                           "Output Vvir through view B must come from view B");

  TEST_ASSERT(values_are_distinct(output_a.Rvir, output_b.Rvir),
              "The two views must produce different output Rvir");
  TEST_ASSERT(values_are_distinct(output_a.Vvir, output_b.Vvir),
              "The two views must produce different output Vvir");

  printf("  output conversion: view A Rvir=%g Vvir=%g | view B Rvir=%g Vvir=%g\n", output_a.Rvir,
         output_a.Vvir, output_b.Rvir, output_b.Vvir);

  /* ===== CLEANUP ===== */
  myfree(halos_b);
  myfree(halos_a);
  myfree(Age_base);
  check_memory_leaks();

  return TEST_PASS;
}

/**
 * @test    test_payload_populator_reads_view_not_global
 * @brief   The generated payload populator fills from the view it is handed
 *
 * Expected: payload fields match the handed view while the global points at the
 *           other array
 * Validates: generated populate_halo_payload.inc
 */
int test_payload_populator_reads_view_not_global(void) {
  /* ===== SETUP ===== */
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  read_parameter_file(test_binary_param_file());
  init();

  struct RawHalo *halos_a = mymalloc_cat(VIEW_TEST_NHALOS * sizeof(struct RawHalo), MEM_HALOS);
  struct RawHalo *halos_b = mymalloc_cat(VIEW_TEST_NHALOS * sizeof(struct RawHalo), MEM_HALOS);
  const int snap_a = MimicConfig.LastSnapshotNr;
  const int snap_b = (MimicConfig.LastSnapshotNr > 0) ? MimicConfig.LastSnapshotNr - 1 : 0;

  seed_fixture_array(halos_a, 100.0, 10, snap_a, 150.0f);
  seed_fixture_array(halos_b, 800.0, 70, snap_b, 450.0f);

  const struct HaloInputView view_a = {halos_a, VIEW_TEST_NHALOS};
  const struct HaloInputView view_b = {halos_b, VIEW_TEST_NHALOS};
  const int halonr = 1;

  /* ===== EXECUTE ===== */
  InputTreeHalos = halos_b; /* global points at the wrong array for payload A */
  const struct HaloInitPayload payload_a = populate_payload_through_view(view_a, halonr);

  InputTreeHalos = halos_a; /* and at the wrong array for payload B */
  const struct HaloInitPayload payload_b = populate_payload_through_view(view_b, halonr);

  InputTreeHalos = NULL;

  /* ===== VALIDATE ===== */
  TEST_ASSERT(payload_a.Len == halos_a[halonr].Len, "Payload A Len must come from view A");
  TEST_ASSERT(payload_b.Len == halos_b[halonr].Len, "Payload B Len must come from view B");
  TEST_ASSERT(payload_a.Len != payload_b.Len, "Payload Len must differ between the two views");

  TEST_ASSERT(payload_a.SnapNum == snap_a, "Payload A SnapNum must come from view A");
  TEST_ASSERT(payload_b.SnapNum == snap_b, "Payload B SnapNum must come from view B");

  TEST_ASSERT_DOUBLE_EQUAL(payload_a.Mvir, get_virial_mass(view_a, halonr), 0.0,
                           "Payload A Mvir must be view A's virial mass");
  TEST_ASSERT_DOUBLE_EQUAL(payload_b.Mvir, get_virial_mass(view_b, halonr), 0.0,
                           "Payload B Mvir must be view B's virial mass");
  TEST_ASSERT(values_are_distinct(payload_a.Mvir, payload_b.Mvir),
              "Payload Mvir must differ between the two views");

  TEST_ASSERT_DOUBLE_EQUAL(payload_a.Rvir, get_virial_radius(view_a, halonr), 0.0,
                           "Payload A Rvir must be view A's virial radius");
  TEST_ASSERT_DOUBLE_EQUAL(payload_b.Vvir, get_virial_velocity(view_b, halonr), 0.0,
                           "Payload B Vvir must be view B's virial velocity");

  TEST_ASSERT(!values_are_distinct(payload_a.Vmax, mimic_tree_get_Vmax(view_a, halonr)),
              "Payload A Vmax must come from view A");
  TEST_ASSERT(!values_are_distinct(payload_b.Vmax, mimic_tree_get_Vmax(view_b, halonr)),
              "Payload B Vmax must come from view B");
  TEST_ASSERT(values_are_distinct(payload_a.Vmax, payload_b.Vmax),
              "Payload Vmax must differ between the two views");

  printf("  payload: view A Len=%d Mvir=%g | view B Len=%d Mvir=%g\n", payload_a.Len,
         payload_a.Mvir, payload_b.Len, payload_b.Mvir);

  /* ===== CLEANUP ===== */
  myfree(halos_b);
  myfree(halos_a);
  myfree(Age_base);
  check_memory_leaks();

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Explicit Halo Input View\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_view_accessor_and_virial_reads_are_independent);
  TEST_RUN(test_output_conversion_reads_view_not_global);
  TEST_RUN(test_payload_populator_reads_view_not_global);

  TEST_SUMMARY();
  return TEST_RESULT();
}
