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
 *   the reheating, before the ejection debit);
 * - with SFprescription 1 (the uff_r2_*, ufsf_r2_* and uff_cgm_* fixtures, R2's and R4's flags),
 *   each commit's hydrogen cap (S:1131-1135, S:1169-1173), which the kernel applies for that
 *   prescription: its guard and two binding flags, and its bound max_h, compared bit for bit, and,
 *   where a cap binds, the capped field equal to that bound;
 * - with CGMrecipeOn 1 (the uff_cgm_* fixtures, R4's flags; oracle README "The c10 Group, Part 3"),
 *   the fixture's CGMrecipeOn passed to the commit, every galaxy's Regime and CGM pair read and
 *   asserted with the other fields; the commit's selection (feedback_reservoir_cgm) asserted
 *   against the kernel's (locals.reservoir_cgm) AND against the FoF central's input Regime (1
 *   exactly when it is 0: S:1175-1176 route by the central, never the forming galaxy); the
 *   central's unselected pair, and both pairs of every other galaxy, asserted unchanged against the
 *   input; the update_from_feedback locals mapped from SAGE26's names (metallicity_hot,
 *   metals_hot_to_eject and metals_hot_to_eject_capped hold the selected pair's metallicityCGM or
 *   metallicityHot and transfers) to the kernel's metallicity_selected, metals_to_eject and
 *   metals_to_eject_capped, and the binding caps compared with the selected pair at the
 *   cap (selected_gas_at_cap, selected_metals_at_cap).
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
#include "modules/_tests/sage26_oracle_h2.h"
#include "shared/metallicity.h"
#include "shared/sage_sf_feedback_physics.h"

static const char *MODULE = "sage26_apply_star_formation_supernova";

#define MAX_GALAXIES 4

static struct oracle_c05_carried_tally tally;
static struct oracle_c08_local_tally h2_tally;
/* CGMrecipeOn 1 fixtures, their selections asserted (each against the oracle and the central's
 * Regime), how many selected the CGM pair, and the unselected pairs asserted unchanged */
static int cgm_fixtures = 0;
static int cgm_selections_cgm = 0;
static int cgm_unselected_pairs = 0;

/**
 * @brief Compare one commit's hydrogen-cap record (SFprescription 1 fixtures only)
 *
 * The guard and the two binding flags, the bound max_h bit for bit, and, where a cap binds, the
 * capped field of the end state equal to the bound.
 *
 * @param site   "sf" (update_from_star_formation) or "fb" (update_from_feedback)
 * @return Number of mismatches
 */
static int check_hydrogen_caps(struct oracle_fixture *out, const char *site,
                               const struct MimicSageHydrogenCapLocals *caps,
                               const struct GalaxyData *gal) {
  int problems = 0;
  char flag[ORACLE_MAX_NAME];
  snprintf(flag, sizeof(flag), "hydrogen_caps_%s_tracked", site);
  problems += oracle_c06_check_flag(out, flag, caps->tracks_h2);
  snprintf(flag, sizeof(flag), "h2_cap_%s_binds", site);
  problems += oracle_c06_check_flag(out, flag, caps->h2_cap_binds);
  snprintf(flag, sizeof(flag), "h1_cap_%s_binds", site);
  problems += oracle_c06_check_flag(out, flag, caps->h1_cap_binds);
  char name[ORACLE_MAX_NAME];
  snprintf(name, sizeof(name), "max_h_%s", site);
  problems += oracle_c08_check_local(out, name, caps->max_h, 1, 0, &h2_tally);
  /* A binding cap sets its field to the bound (strict >: a field equal to it stays) */
  if (caps->h2_cap_binds == 1) {
    h2_tally.binding++;
    problems += oracle_f32_to_bits(gal->H2gas) != oracle_f32_to_bits(caps->max_h);
  }
  if (caps->h1_cap_binds == 1) {
    h2_tally.binding++;
    problems += oracle_f32_to_bits(gal->H1gas) != oracle_f32_to_bits(caps->max_h);
  }
  return problems;
}

/** @brief The CGM pair a CGMrecipeOn 1 fixture adds to every galaxy (SAGE26 names) */
static const char *const CGM_PAIR[] = {"CGMgas", "MetalsCGMgas"};

/**
 * @brief Set galaxies[0 .. ngal-1] from the fixture: Type and the fourteen written fields, and with
 *        CGMrecipeOn 1 Regime and the CGM pair
 * @return Number of problems
 */
static int read_galaxies(struct oracle_fixture *in, const int ngal, const int cgm_recipe_on,
                         struct GalaxyData galaxies[MAX_GALAXIES]) {
  int problems = 0;
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < ngal; i++) {
    oracle_poison(&galaxies[i], sizeof(galaxies[i]));
    /* Type is set for the Mimic side; neither SAGE26 routine reads it (oracle README) */
    (void)oracle_in_i32(in, oracle_galaxy_field(i, "Type"));
    problems += oracle_c05_in_floats(in, i, &galaxies[i], written, count);
    if (cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
      galaxies[i].Regime = oracle_in_i32(in, oracle_galaxy_field(i, "Regime"));
      problems += oracle_c05_in_floats(in, i, &galaxies[i], CGM_PAIR, 2);
    }
  }
  return problems;
}

/** @brief Compare the fourteen written fields of every galaxy, and with CGMrecipeOn 1 its
 *         Regime and CGM pair */
static int check_galaxies(struct oracle_fixture *out, const int ngal, const int cgm_recipe_on,
                          struct GalaxyData galaxies[MAX_GALAXIES]) {
  int problems = 0;
  size_t count;
  const char *const *written = oracle_c05_written_names(&count);
  for (int i = 0; i < ngal; i++) {
    problems += oracle_c05_check_floats(out, "", i, &galaxies[i], written, count);
    if (cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
      problems += oracle_check_i32(out, oracle_galaxy_field(i, "Regime"), galaxies[i].Regime);
      problems += oracle_c05_check_floats(out, "", i, &galaxies[i], CGM_PAIR, 2);
    }
  }
  return problems;
}

/**
 * @brief The assertions of one CGMrecipeOn 1 update_from_feedback fixture
 *
 * The selection against the oracle's and the kernel's, each against the FoF central's input
 * Regime (the site's wiring: S:1175-1176 route by the central); the central's unselected pair and
 * both pairs of every other galaxy (the commit touches only the forming galaxy's cold gas and the
 * central's selected and ejected pairs) unchanged against the input.
 *
 * @return Number of mismatches
 */
static int check_cgm_routing(struct oracle_fixture *out, const int ngal, const int centralgal,
                             const struct GalaxyData input[MAX_GALAXIES],
                             struct GalaxyData galaxies[MAX_GALAXIES],
                             const struct MimicSageFeedbackLocals *locals) {
  int problems = oracle_check_i32(out, "intermediate.cgm_recipe_on", MIMIC_SAGE_CGM_RECIPE_ON);
  const int expected = input[centralgal].Regime == 0;
  problems += oracle_c06_check_flag(out, "feedback_reservoir_cgm", locals->reservoir_cgm);
  if (locals->reservoir_cgm != expected) {
    fprintf(stderr, "oracle: %s: the commit routed by a Regime other than the FoF central's\n",
            out->path);
    problems++;
  }
  cgm_selections_cgm += expected;
  for (int i = 0; i < ngal; i++) {
    if (i == centralgal) {
      problems += oracle_c10_check_unselected(out->path, i, &input[i], &galaxies[i],
                                              oracle_c10_pair(!expected), 2);
      cgm_unselected_pairs++;
    } else {
      problems +=
          oracle_c10_check_unselected(out->path, i, &input[i], &galaxies[i], oracle_c10_pair(0), 2);
      problems +=
          oracle_c10_check_unselected(out->path, i, &input[i], &galaxies[i], oracle_c10_pair(1), 2);
      cgm_unselected_pairs += 2;
    }
  }
  cgm_fixtures++;
  return problems;
}

/** @brief One update_from_star_formation fixture */
static int check_update_from_star_formation(struct oracle_fixture *in, struct oracle_fixture *out) {
  struct oracle_c08_sf_config cfg;
  int problems = oracle_c08_read_sf_config(in, 1, &cfg);
  const double recycle_fraction = oracle_in_f64(in, "params.RecycleFraction");
  const double stars = oracle_in_f64(in, "call.stars");

  struct GalaxyData galaxies[MAX_GALAXIES];
  problems += read_galaxies(in, 1, MIMIC_SAGE_CGM_RECIPE_OFF, galaxies);

  /* The metallicity a burst passes: get_metallicity of the cold gas (model_mergers.c:759) */
  const double metallicity = mimic_get_metallicity(galaxies[0].ColdGas, galaxies[0].MetalsColdGas);
  problems += oracle_check_f64(out, "intermediate.metallicity", metallicity);

  struct MimicSageHydrogenCapLocals caps;
  mimic_sage_update_from_star_formation(&galaxies[0], stars, metallicity, recycle_fraction,
                                        cfg.sf_prescription, &caps);

  problems += check_galaxies(out, 1, MIMIC_SAGE_CGM_RECIPE_OFF, galaxies);
  if (cfg.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    problems += check_hydrogen_caps(out, "sf", &caps, &galaxies[0]);
  }
  return problems;
}

/** @brief One update_from_feedback fixture */
static int check_update_from_feedback(const char *name, struct oracle_fixture *in,
                                      struct oracle_fixture *out) {
  struct oracle_c08_sf_config cfg;
  int problems = oracle_c08_read_sf_config(in, 1, &cfg);
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  /* CGMrecipeOn: 0 for every fixture but the uff_cgm_* ones, 1 for those (R4) */
  const int cgm_recipe_on = oracle_in_i32(in, "params.CGMrecipeOn");
  if (cgm_recipe_on != MIMIC_SAGE_CGM_RECIPE_OFF && cgm_recipe_on != MIMIC_SAGE_CGM_RECIPE_ON) {
    fprintf(stderr, "oracle: %s: params.CGMrecipeOn must be 0 or 1\n", name);
    return problems + 1;
  }

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
  problems += read_galaxies(in, ngal, cgm_recipe_on, galaxies);
  const float cold_gas_at_call = galaxies[p].ColdGas;
  struct GalaxyData input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* The metallicity a burst passes: get_metallicity of the cold gas (model_mergers.c:788) */
  const double metallicity = mimic_get_metallicity(galaxies[p].ColdGas, galaxies[p].MetalsColdGas);
  problems += oracle_check_f64(out, "intermediate.metallicity", metallicity);

  struct MimicSageFeedbackLocals locals;
  mimic_sage_update_from_feedback(&galaxies[p], &galaxies[centralgal], reheated_mass, ejected_mass,
                                  metallicity, cfg.sf_prescription, cgm_recipe_on, &locals);

  problems += check_galaxies(out, ngal, cgm_recipe_on, galaxies);

  /* Binding clamps and caps: the recorded value equals the reservoir at that point (with
   * CGMrecipeOn 1 the selected pair's) */
  problems += oracle_c05_check_binding(out, "intermediate.", "reheat_clamp_fb_binds",
                                       "reheated_mass_fb_clamped", cold_gas_at_call, &tally);
  problems += oracle_c05_check_binding(out, "intermediate.", "ejected_cap_binds",
                                       "ejected_mass_capped", locals.selected_gas_at_cap, &tally);
  problems +=
      oracle_c05_check_binding(out, "intermediate.", "metals_cap_binds",
                               "metals_hot_to_eject_capped", locals.selected_metals_at_cap, &tally);

  /* The routine's locals, which the kernel reports: asserted where validated (SAGE26's names, the
   * selected pair's values with CGMrecipeOn 1) */
  problems += oracle_c05_check_carried(out, "intermediate.", "reheated_mass_fb_clamped",
                                       &locals.reheated_mass_clamped, &tally);
  problems += oracle_c05_check_carried(out, "intermediate.", "ejected_mass_capped",
                                       &locals.ejected_mass_capped, &tally);
  problems += oracle_c05_check_carried(out, "intermediate.", "metallicity_hot",
                                       &locals.metallicity_selected, &tally);
  problems += oracle_c05_check_carried(out, "intermediate.", "metals_hot_to_eject",
                                       &locals.metals_to_eject, &tally);
  problems += oracle_c05_check_carried(out, "intermediate.", "metals_hot_to_eject_capped",
                                       &locals.metals_to_eject_capped, &tally);
  if (cfg.sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    problems += check_hydrogen_caps(out, "fb", &locals.hydrogen_caps, &galaxies[p]);
  }
  if (cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
    problems += check_cgm_routing(out, ngal, centralgal, input, galaxies, &locals);
  }
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
  memset(&h2_tally, 0, sizeof(h2_tally));
  cgm_fixtures = 0;
  cgm_selections_cgm = 0;
  cgm_unselected_pairs = 0;
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: carried locals asserted %d (validated two-sided), not validated %d, "
         "absent %d; binding clamps and caps asserted equal to their reservoir %d ",
         tally.asserted, tally.not_validated, tally.absent, tally.binding);
  printf("\n  oracle: hydrogen-cap bounds compared %d (validated two-sided %d, not validated %d, "
         "absent %d); binding hydrogen caps asserted equal to their bound %d ",
         h2_tally.validated + h2_tally.not_validated + h2_tally.narrowing_limited,
         h2_tally.validated, h2_tally.not_validated, h2_tally.absent, h2_tally.binding);
  printf("\n  oracle: c10 CGMrecipeOn 1 fixtures %d: the commit's selection asserted against the "
         "oracle and the FoF central's Regime on each (CGM pair %d, hot pair %d); unselected "
         "pairs asserted unchanged %d ",
         cgm_fixtures, cgm_selections_cgm, cgm_fixtures - cgm_selections_cgm, cgm_unselected_pairs);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(cgm_selections_cgm >= 1 && cgm_fixtures - cgm_selections_cgm >= 1,
              "CGMrecipeOn 1 fixtures selecting each pair must be asserted");
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
