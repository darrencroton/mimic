/**
 * @file    test_unit_sage26_apply_star_formation_supernova_oracle.c
 * @brief   Bit-exact comparison of the shared SF and feedback commits with SAGE26's own routines
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_apply_star_formation_supernova (its _tests/oracle/ directory): SAGE26's own
 * update_from_star_formation (R/src/model_starformation_and_feedback.c:1119-1136)
 * and update_from_feedback (:1146-1251), each called on its own with the cold gas's
 * get_metallicity, as a burst calls them (R/src/model_mergers.c:759-760, :788-791).
 *
 * These fixtures have no star-formation chain input, so the test calls the package's
 * shared commits directly (shared/sage_sf_feedback_physics.h): the one
 * implementation that sage26_apply_star_formation_supernova uses for disk star
 * formation and mimic_apply_collisional_starburst for both burst channels. It
 * asserts bit for bit:
 *
 * - the cold-gas metallicity the package forms (mimic_get_metallicity), against
 *   intermediate.metallicity;
 * - every recorded field of every galaxy (the fourteen fields of the oracle README,
 *   OutflowRate being the package's SupernovaOutflowRate), changed or not;
 * - update_from_feedback's locals (the clamped reheated mass, the capped ejected
 *   mass, the hot metallicity and the metal transfer before and after its cap),
 *   which the kernel reports, wherever the oracle validates them two-sided; and,
 *   where a clamp or cap binds, that its recorded value equals the reservoir it
 *   binds to (the cold gas at the call; the central's hot gas and hot metals after
 *   the reheating, before the ejection debit).
 *
 * The branch flags are read to drive those equalities and are not asserted. Outside
 * the oracle environment (macOS arm64, Apple clang 21) the bit-exact test reports
 * SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "shared/metallicity.h"
#include "shared/sage_sf_feedback_physics.h"

static const char *MODULE = "sage26_apply_star_formation_supernova";

#define MAX_GALAXIES 4

static struct oracle_c05_carried_tally tally;

/**
 * @brief Set galaxies[0 .. ngal-1] from the fixture: Type and the fourteen written fields
 * @return Number of problems
 */
static int read_galaxies(struct oracle_fixture *in, const int ngal,
                         struct GalaxyData galaxies[MAX_GALAXIES]) {
  int problems = 0;
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < ngal; i++) {
    oracle_poison(&galaxies[i], sizeof(galaxies[i]));
    /* Type is set for the Mimic side; neither SAGE26 routine reads it (oracle README) */
    (void)oracle_in_i32(in, oracle_galaxy_field(i, "Type"));
    problems += oracle_c05_in_floats(in, i, &galaxies[i], written, count);
  }
  return problems;
}

/** @brief Compare the fourteen written fields of every galaxy */
static int check_galaxies(struct oracle_fixture *out, const int ngal,
                          struct GalaxyData galaxies[MAX_GALAXIES]) {
  int problems = 0;
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < ngal; i++) {
    problems += oracle_c05_check_floats(out, "", i, &galaxies[i], written, count);
  }
  return problems;
}

/** @brief One update_from_star_formation fixture */
static int check_update_from_star_formation(struct oracle_fixture *in, struct oracle_fixture *out) {
  int problems = oracle_c05_require_flag(in, "params.SFprescription", 0);
  const double recycle_fraction = oracle_in_f64(in, "params.RecycleFraction");
  const double stars = oracle_in_f64(in, "call.stars");

  struct GalaxyData galaxies[MAX_GALAXIES];
  problems += read_galaxies(in, 1, galaxies);

  /* The metallicity a burst passes: get_metallicity of the cold gas (model_mergers.c:759) */
  const double metallicity = mimic_get_metallicity(galaxies[0].ColdGas, galaxies[0].MetalsColdGas);
  problems += oracle_check_f64(out, "intermediate.metallicity", metallicity);

  mimic_sage_update_from_star_formation(&galaxies[0], stars, metallicity, recycle_fraction);

  problems += check_galaxies(out, 1, galaxies);
  return problems;
}

/** @brief One update_from_feedback fixture */
static int check_update_from_feedback(const char *name, struct oracle_fixture *in,
                                      struct oracle_fixture *out) {
  int problems = oracle_c05_require_flag(in, "params.SFprescription", 0);
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  problems += oracle_c05_require_flag(in, "params.CGMrecipeOn", 0);

  const int ngal = oracle_in_i32(in, "call.ngal");
  const int p = oracle_in_i32(in, "call.p");
  const int centralgal = oracle_in_i32(in, "call.centralgal");
  if (ngal < 1 || ngal > MAX_GALAXIES || p < 0 || p >= ngal || centralgal < 0 ||
      centralgal >= ngal) {
    fprintf(stderr, "oracle: %s: call.ngal, call.p or call.centralgal out of range\n", name);
    return problems + 1;
  }
  const double reheated_mass = oracle_in_f64(in, "call.reheated_mass");
  const double ejected_mass = oracle_in_f64(in, "call.ejected_mass");

  struct GalaxyData galaxies[MAX_GALAXIES];
  problems += read_galaxies(in, ngal, galaxies);
  const float cold_gas_at_call = galaxies[p].ColdGas;

  /* The metallicity a burst passes: get_metallicity of the cold gas (model_mergers.c:788) */
  const double metallicity = mimic_get_metallicity(galaxies[p].ColdGas, galaxies[p].MetalsColdGas);
  problems += oracle_check_f64(out, "intermediate.metallicity", metallicity);

  struct MimicSageFeedbackLocals locals;
  mimic_sage_update_from_feedback(&galaxies[p], &galaxies[centralgal], reheated_mass, ejected_mass,
                                  metallicity, &locals);

  problems += check_galaxies(out, ngal, galaxies);

  /* Binding clamps and caps: the recorded value equals the reservoir at that point */
  problems += oracle_c05_check_binding(out, "intermediate.", "reheat_clamp_fb_binds",
                                       "reheated_mass_fb_clamped", cold_gas_at_call, &tally);
  problems += oracle_c05_check_binding(out, "intermediate.", "ejected_cap_binds",
                                       "ejected_mass_capped", locals.hot_gas_at_cap, &tally);
  problems +=
      oracle_c05_check_binding(out, "intermediate.", "metals_cap_binds",
                               "metals_hot_to_eject_capped", locals.metals_hot_gas_at_cap, &tally);

  /* The routine's locals, which the kernel reports: asserted where validated */
  problems += oracle_c05_check_carried(out, "intermediate.", "reheated_mass_fb_clamped",
                                       &locals.reheated_mass_clamped, &tally);
  problems += oracle_c05_check_carried(out, "intermediate.", "ejected_mass_capped",
                                       &locals.ejected_mass_capped, &tally);
  problems += oracle_c05_check_carried(out, "intermediate.", "metallicity_hot",
                                       &locals.metallicity_hot, &tally);
  problems += oracle_c05_check_carried(out, "intermediate.", "metals_hot_to_eject",
                                       &locals.metals_hot_to_eject, &tally);
  problems += oracle_c05_check_carried(out, "intermediate.", "metals_hot_to_eject_capped",
                                       &locals.metals_hot_to_eject_capped, &tally);
  return problems;
}

/** @brief Compare one fixture; dispatches on its routine line */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  const int feedback = strcmp(in.routine, "update_from_feedback") == 0;
  if (oracle_load_pair(MODULE, name,
                       feedback ? "update_from_feedback" : "update_from_star_formation", &in,
                       &out) != 0) {
    return 1;
  }

  int problems = feedback ? check_update_from_feedback(name, &in, &out)
                          : check_update_from_star_formation(&in, &out);
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/**
 * @test    test_oracle_fixture_count
 * @brief   Every fixture fixtures.manifest lists for the module is present (platform independent)
 */
int test_oracle_fixture_count(void) {
  TEST_ASSERT(oracle_fixture_count_ok(MODULE),
              "fixtures.manifest must list its declared count and every listed fixture must exist");
  return TEST_PASS;
}

/**
 * @test    test_oracle_bit_identity
 * @brief   Every fixture reproduces the SAGE26 oracle bit for bit (SKIP outside its environment)
 */
int test_oracle_bit_identity(void) {
  const char *mismatch = oracle_environment_mismatch();
  if (mismatch != NULL) {
    return TEST_SKIP_WITH(mismatch);
  }
  memset(&tally, 0, sizeof(tally));
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: carried locals asserted %d (validated two-sided), not validated %d, "
         "absent %d; binding clamps and caps asserted equal to their reservoir %d ",
         tally.asserted, tally.not_validated, tally.absent, tally.binding);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_apply_star_formation_supernova SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
