/**
 * @file    test_unit_sage26_quasar_mode_oracle.c
 * @brief   Bit-exact comparison of the black-hole growth and quasar wind with SAGE26's
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for sage26_quasar_mode (its
 * _tests/oracle/ directory) through the shared functions the module calls
 * (shared/sage_agn_physics.h), directly, with the fixture's inputs standing in for the
 * module's trigger (the merger event's ratio or the instability's UnstableDiskGasFraction):
 *
 * - routine grow_black_hole: SAGE26's grow_black_hole (R/src/model_mergers.c:346-378),
 *   which calls quasar_mode_wind (:374), on galaxy 0 with call.mass_ratio, against
 *   mimic_apply_black_hole_growth;
 * - routine quasar_mode_wind: SAGE26's quasar_mode_wind (:390-441) on its own, with
 *   call.BHaccrete, against mimic_apply_quasar_mode_wind.
 *
 * Asserted bit for bit: the thirty floats of galaxy 0's recorded block (oracle README
 * "The c06 Group"), every other member of the galaxy unchanged, every branch flag the fixture
 * records, the presence of every carried value, and each carried value where the oracle validates
 * it two-sided or a clamp or floor binds it (BHaccrete where the ColdGas cap binds). Checked values
 * that are not validated are recorded, not asserted (the rule of the oracle README's
 * "The c05 Group": the float stores absorb a one-ulp change of most doubles). Outside the oracle
 * environment (macOS arm64, Apple clang 21) the bit-exact test reports SKIP.
 *
 * SFprescription 1 (the fixtures, R2's flags with BulgeSizeOn 3): the growth applies the
 * accretion's hydrogen cap (:366-372), and the test also asserts its record (the guard, the two cap
 * flags and the float bound max_h_bh, bit for bit), sage26_oracle_hydrogen.h. BulgeSizeOn and
 * Hubble_h, which neither routine reads, are read and not applied.
 *
 * CGMrecipeOn 1 (the qw_cgm_* and bh_cgm_* fixtures, R4's flags; oracle README "The c10 Group, Part
 * 3"): the fixture's CGMrecipeOn is passed to both helpers and the galaxy's Regime is set from the
 * fixture; the wind's second test then selects the WIND HOST's pair, its CGM pair for Regime 0
 * (M:406-428). The test also asserts the CGMrecipeOn 1 record: the selection
 * (intermediate.wind_reservoir_cgm) against the wind's reservoir_cgm AND against the host's input
 * Regime, the CGM test (wind_ejects_cgm; the merger-family record's wind_ejects_hot is -1 where the
 * CGM pair is selected and hot_gas_energy absent), cgm_gas_energy bit for bit, and the host's pair
 * its Regime does not select unchanged against the input. FIREmodeOn (R4's 1, which neither routine
 * reads) is read and not applied.
 *
 * Inputs read but not applied, each named: the burst parameters (FeedbackReheatingEpsilon,
 * FeedbackEjectionEfficiency, EtaSNcode, EnergySNcode, RecycleFraction, Yield,
 * FracZleaveDisk, ThreshMajorMerger) and G, which no black-hole or wind statement reads;
 * the sixty history elements, which neither routine reads or writes (they are applied to
 * the galaxy and must stay unchanged).
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_interface.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_cgm.h"
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_hydrogen.h"
#include "shared/sage_agn_physics.h"

static const char *MODULE = "sage26_quasar_mode";

/* Carried-value check kinds (oracle README "Carried Values, Check Kinds") */
#define KIND_CONTINUATION 1
#define KIND_TAIL 2

static struct oracle_c06_carried_tally tally;
static struct oracle_c08_local_tally h_tally;
/* The CGMrecipeOn 1 fixtures' assertions, and the fixtures whose wind selected each pair */
static struct oracle_c10_site_tally cgm_tally;
static int cgm_wind_cgm_pair = 0, cgm_wind_hot_pair = 0;

/**
 * @brief Read the fixture's flags and parameters into the run configuration
 * @return Number of problems
 */
static int setup_parameters(struct oracle_fixture *in, double *growth_rate, double *efficiency,
                            int *sf_prescription, int *cgm_recipe_on) {
  struct MimicSageBulgeSize sizes; /* read, not applied: neither routine reads the sizes */
  /* CGMrecipeOn 0, or 1 (R4); FIREmodeOn 0, or R4's 1 with CGMrecipeOn 1, read and not applied
   * (neither routine reads it) */
  int fire_mode_on = 0;
  int problems = oracle_c10_require_merger_flags(in, &sizes, sf_prescription, 0, 1, cgm_recipe_on,
                                                 &fire_mode_on);
  reset_config();
  MimicConfig.UnitVelocity_in_cm_per_s = oracle_in_f64(in, "params.UnitVelocity_in_cm_per_s");
  *growth_rate = oracle_in_f64(in, "params.BlackHoleGrowthRate");
  *efficiency = oracle_in_f64(in, "params.QuasarModeEfficiency");

  /* Read by the burst, not by grow_black_hole or quasar_mode_wind */
  const char *const unread[] = {"FeedbackReheatingEpsilon",
                                "FeedbackEjectionEfficiency",
                                "EtaSNcode",
                                "EnergySNcode",
                                "RecycleFraction",
                                "Yield",
                                "FracZleaveDisk",
                                "ThreshMajorMerger",
                                "G"};
  for (size_t k = 0; k < sizeof(unread) / sizeof(unread[0]); k++) {
    char field[ORACLE_MAX_NAME];
    snprintf(field, sizeof(field), "params.%s", unread[k]);
    (void)oracle_in_f64(in, field);
  }
  return problems;
}

/**
 * @brief Assert the wind's flags and carried values; returns the number of problems
 *
 * The merger-family record's wind_ejects_hot is the second test on the hot pair; with CGMrecipeOn 1
 * and the CGM pair selected it is not evaluated (-1) and the CGMrecipeOn 1 record holds the
 * CGM test (check_wind_cgm).
 */
static int check_wind(struct oracle_fixture *out, const struct MimicSageQuasarWindLocals *wind) {
  int problems = 0;
  problems += oracle_c06_check_flag(out, "wind_ejects_cold", wind->ejects_cold);
  problems += oracle_c06_check_flag(out, "wind_ejects_hot",
                                    wind->reservoir_cgm == 1 ? -1 : wind->ejects_selected);
  problems +=
      oracle_c06_check_carried(out, "quasar_energy", KIND_TAIL, wind->quasar_energy, 0, &tally);
  problems +=
      oracle_c06_check_carried(out, "cold_gas_energy", KIND_TAIL, wind->cold_gas_energy, 0, &tally);
  problems +=
      oracle_c06_check_carried(out, "hot_gas_energy", KIND_TAIL, wind->hot_gas_energy, 0, &tally);
  return problems;
}

/**
 * @brief The CGMrecipeOn 1 record of a wind (direct or the growth's): the selection against the
 *        package's and the wind host's input Regime, the CGM test, cgm_gas_energy, and the host's
 *        unselected pair unchanged against the input
 * @return Number of mismatches
 */
static int check_wind_cgm(const char *name, struct oracle_fixture *out,
                          const struct MimicSageQuasarWindLocals *wind, const int wind_ran,
                          const struct GalaxyData *before, struct GalaxyData *galaxy) {
  int problems = oracle_check_i32(out, "intermediate.cgm_recipe_on", MIMIC_SAGE_CGM_RECIPE_ON);
  /* The wind host is the galaxy itself (M:374, M:406-407) */
  problems += oracle_c10_check_site(out, "wind_reservoir_cgm", wind->reservoir_cgm,
                                    wind_ran ? before->Regime : -1, &cgm_tally);
  problems += oracle_c06_check_flag(out, "wind_ejects_cgm",
                                    wind->reservoir_cgm == 1 ? wind->ejects_selected : -1);
  problems += oracle_c10_check_value(out, "cgm_gas_energy", wind->cgm_gas_energy, &cgm_tally);
  problems += oracle_c10_check_regimes(out, 1, galaxy);
  if (wind_ran) {
    /* The pair the host's Regime does not select is never written (M:406-428) */
    problems += oracle_c10_check_unselected(name, 0, before, galaxy,
                                            oracle_c10_pair(before->Regime != 0), 2);
    cgm_tally.unselected++;
    cgm_wind_cgm_pair += wind->reservoir_cgm == 1;
    cgm_wind_hot_pair += wind->reservoir_cgm == 0;
  }
  cgm_tally.fixtures++;
  return problems;
}

/** @brief Compare one fixture; returns the number of mismatches and errors */
static int check_fixture(const char *name) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), MODULE, name, ".in");
  if (oracle_load(path, &in) != 0) {
    return 1;
  }
  char routine[ORACLE_MAX_NAME];
  snprintf(routine, sizeof(routine), "%s", in.routine);
  const int is_growth = strcmp(routine, "grow_black_hole") == 0;
  if (!is_growth && strcmp(routine, "quasar_mode_wind") != 0) {
    fprintf(stderr, "oracle: %s: routine %s is not a black-hole or wind routine\n", name, routine);
    return 1;
  }
  if (oracle_load_pair(MODULE, name, routine, &in, &out) != 0) {
    return 1;
  }

  double growth_rate = 0.0;
  double efficiency = 0.0;
  int sf_prescription = 0;
  int cgm_recipe_on = 0;
  int problems = setup_parameters(&in, &growth_rate, &efficiency, &sf_prescription, &cgm_recipe_on);

  static struct GalaxyData galaxy, before;
  static struct Halo halo;
  problems += oracle_c06_in_galaxy(&in, 0, &halo, &galaxy);
  /* The wind host's Regime, which its CGMrecipeOn 1 wind reads (M:407) */
  if (cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
    problems += oracle_c10_read_regimes(&in, 1, &galaxy);
  }
  if (problems != 0) {
    return problems;
  }
  before = galaxy;

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.central_galaxy = &halo;

  if (is_growth) {
    const double mass_ratio = oracle_in_f64(&in, "call.mass_ratio");
    struct MimicSageBlackHoleLocals bh;
    const double accreted = mimic_apply_black_hole_growth(
        &halo, mass_ratio, growth_rate, efficiency, &ctx, sf_prescription, cgm_recipe_on, &bh);
    if (bh.coldgas_positive == 1 &&
        oracle_f64_to_bits(accreted) != oracle_f64_to_bits(bh.bh_accrete)) {
      fprintf(stderr, "oracle: %s: the returned accretion differs from the local\n", name);
      problems++;
    }
    problems += oracle_c06_check_flag(&out, "bh_coldgas_positive", bh.coldgas_positive);
    problems += oracle_c06_check_flag(&out, "bh_accretion_cap_binds", bh.accretion_cap_binds);
    problems += oracle_c06_check_flag(&out, "bh_metals_floor_binds", bh.metals_floor_binds);
    problems += oracle_c06_check_carried(&out, "BHaccrete_pre_cap", KIND_CONTINUATION,
                                         bh.bh_accrete_pre_cap, 0, &tally);
    /* Where the cap binds, BHaccrete is the ColdGas it was capped to (:355-357) */
    problems += oracle_c06_check_carried(&out, "BHaccrete", KIND_CONTINUATION, bh.bh_accrete,
                                         bh.accretion_cap_binds == 1, &tally);
    problems +=
        oracle_c06_check_carried(&out, "bh_metallicity", KIND_TAIL, bh.bh_metallicity, 0, &tally);
    problems += check_wind(&out, &bh.wind);
    problems += oracle_c08_check_bh_hydrogen(&out, sf_prescription, &bh.hydrogen_caps, &h_tally);
    if (cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
      problems += check_wind_cgm(name, &out, &bh.wind, bh.coldgas_positive == 1, &before, &galaxy);
    }
  } else {
    const double bh_accrete = oracle_in_f64(&in, "call.BHaccrete");
    struct MimicSageQuasarWindLocals wind;
    mimic_apply_quasar_mode_wind(&halo, bh_accrete, efficiency, &ctx, cgm_recipe_on, &wind);
    problems += check_wind(&out, &wind);
    if (cgm_recipe_on == MIMIC_SAGE_CGM_RECIPE_ON) {
      problems += check_wind_cgm(name, &out, &wind, 1, &before, &galaxy);
    }
  }

  problems += oracle_c06_check_galaxy(&out, "", 0, &galaxy, 0);
  problems += oracle_c06_check_rest_unchanged(name, 0, &before, &galaxy);

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
  memset(&h_tally, 0, sizeof(h_tally));
  memset(&cgm_tally, 0, sizeof(cgm_tally));
  cgm_wind_cgm_pair = 0;
  cgm_wind_hot_pair = 0;
  const int identical = oracle_all_bit_identical(MODULE, check_fixture);
  printf("\n  oracle: carried values asserted %d (validated two-sided), binding %d, "
         "not validated %d, absent %d; c08 values validated %d, narrowing-limited %d, "
         "not validated %d, absent %d ",
         tally.asserted, tally.binding, tally.not_validated, tally.absent, h_tally.validated,
         h_tally.narrowing_limited, h_tally.not_validated, h_tally.absent);
  oracle_c10_print_site_tally(&cgm_tally);
  printf("\n  oracle: c10 winds selecting the host's CGM pair %d, its hot pair %d ",
         cgm_wind_cgm_pair, cgm_wind_hot_pair);
  TEST_ASSERT(identical, "every fixture must reproduce the oracle bit for bit");
  TEST_ASSERT(cgm_wind_cgm_pair >= 1 && cgm_wind_hot_pair >= 1,
              "CGMrecipeOn 1 winds selecting each of the host's pairs must be asserted");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_quasar_mode SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  TEST_SUMMARY();
  return TEST_RESULT();
}
