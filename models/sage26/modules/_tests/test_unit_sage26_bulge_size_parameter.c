/**
 * @file    test_unit_sage26_bulge_size_parameter.c
 * @brief   BulgeSizeOn: a required package parameter, 0 or 3 only, and the mode-0 size helpers
 *
 * Every module that calls a bulge-size helper (sage26_set_disk_scale_radius,
 * sage26_disk_instability, sage26_starburst_feedback, sage26_resolve_mergers_and_disruption)
 * loads BulgeSizeOn at initialisation through the required-parameter path: a missing key fails
 * initialisation, and any value but 0 (SAGE26's R0, no sizes) or 3 (the Tonini sizes) is
 * refused with an error naming the parameter. Modes 1 and 2 of SAGE26 (R/src/model_misc.c:
 * 252-315) are not ported. The mode-0 helpers keep SAGE26's BulgeSizeOn 0 behaviour:
 * get_bulge_radius zeroes the three radii and returns 0 (R/src/model_misc.c:242-247), and
 * update_instability_bulge_radius returns at once (:395), stale radii included.
 *
 * Test cases:
 *   - test_missing_parameter_refused: each module's init fails without BulgeSizeOn
 *   - test_unsupported_values_refused: 1, 2, 4 and -1 are refused, naming the parameter
 *   - test_supported_values_accepted: 0 and 3 initialise every module
 *   - test_mode0_helpers: mode 0 zeroes in get_bulge_radius and returns in the update
 *   - test_mode3_disk_radius_needs_configuration: the disk-radius module refuses mode 3
 *     without the run configuration (Hubble_h), and runs mode 0 without it
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_bulge_size_physics.h"

extern int sage26_set_disk_scale_radius_init(void);
extern int sage26_set_disk_scale_radius_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal);
extern int sage26_set_disk_scale_radius_cleanup(void);
extern int sage26_disk_instability_init(void);
extern int sage26_disk_instability_cleanup(void);
extern int sage26_starburst_feedback_init(void);
extern int sage26_starburst_feedback_cleanup(void);
extern int sage26_resolve_mergers_and_disruption_init(void);
extern int sage26_resolve_mergers_and_disruption_cleanup(void);

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** @brief One module that calls a size helper: its lifecycle and the other parameters it needs */
struct size_module {
  const char *name;
  int (*init)(void);
  int (*cleanup)(void);
};

static const struct size_module MODULES[] = {
    {"sage26_set_disk_scale_radius", sage26_set_disk_scale_radius_init,
     sage26_set_disk_scale_radius_cleanup},
    {"sage26_disk_instability", sage26_disk_instability_init, sage26_disk_instability_cleanup},
    {"sage26_starburst_feedback", sage26_starburst_feedback_init,
     sage26_starburst_feedback_cleanup},
    {"sage26_resolve_mergers_and_disruption", sage26_resolve_mergers_and_disruption_init,
     sage26_resolve_mergers_and_disruption_cleanup},
};

/** @brief Read all content from a FILE* into a static buffer (valid until the next call) */
static const char *read_captured_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/**
 * @brief The package's test parameters with BulgeSizeOn removed, then set to `value` if given
 *
 * Also configures the resolver's required predecessor (sage26_initialise_merger_clock in
 * pre_timestep) and SAGE26's unit system, so every module's other checks pass.
 */
static void setup(const char *value) {
  reset_config();
  set_test_model_parameters();
  int n = 0;
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, "BulgeSizeOn") != 0) {
      MimicConfig.ModelParams[n++] = MimicConfig.ModelParams[i];
    }
  }
  MimicConfig.NumModelParams = n;
  if (value != NULL) {
    snprintf(MimicConfig.ModelParams[n].param_name, MAX_STRING_LEN, "%s", "BulgeSizeOn");
    snprintf(MimicConfig.ModelParams[n].value, MAX_STRING_LEN, "%s", value);
    MimicConfig.NumModelParams = n + 1;
  }
  /* sage26_starburst_feedback's required SFprescription and StarburstColdGasOn, at R1's values */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_STARBURST_MODULE);
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.UnitLength_in_cm = 3.08568e24;
  MimicConfig.UnitMass_in_g = 1.989e43;
  MimicConfig.UnitVelocity_in_cm_per_s = 1.0e5;
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * MimicConfig.UnitLength_in_cm *
                                  MimicConfig.UnitLength_in_cm /
                                  (MimicConfig.UnitTime_in_s * MimicConfig.UnitTime_in_s);
  test_pre_timestep_add("sage26_initialise_merger_clock", PROCESSING_MODE_FULL_HALO);
}

static void teardown(void) { test_free_pre_timestep(); }

/** @brief Initialise one module, capturing its log; returns the init status */
static int init_captured(const struct size_module *m, const char **log) {
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = m->init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/**
 * @test    test_missing_parameter_refused
 * @brief   Each module's init fails without BulgeSizeOn, naming the parameter (no default)
 */
int test_missing_parameter_refused(void) {
  for (size_t k = 0; k < COUNT(MODULES); k++) {
    setup(NULL);
    const char *log = "";
    const int status = init_captured(&MODULES[k], &log);
    printf("\n  %s without BulgeSizeOn: init %d", MODULES[k].name, status);
    TEST_ASSERT(status != 0, "init must fail when BulgeSizeOn is missing");
    TEST_ASSERT(strstr(log, "BulgeSizeOn") != NULL, "the error must name BulgeSizeOn");
    MODULES[k].cleanup();
    teardown();
  }
  return TEST_PASS;
}

/**
 * @test    test_unsupported_values_refused
 * @brief   BulgeSizeOn 1, 2, 4 and -1 are refused by every module, naming the parameter
 */
int test_unsupported_values_refused(void) {
  const char *const values[] = {"1", "2", "4", "-1"};
  for (size_t k = 0; k < COUNT(MODULES); k++) {
    for (size_t v = 0; v < COUNT(values); v++) {
      setup(values[v]);
      const char *log = "";
      const int status = init_captured(&MODULES[k], &log);
      printf("\n  %s with BulgeSizeOn %s: init %d", MODULES[k].name, values[v], status);
      TEST_ASSERT(status != 0, "init must refuse an unsupported BulgeSizeOn");
      TEST_ASSERT(strstr(log, "BulgeSizeOn") != NULL, "the error must name BulgeSizeOn");
      MODULES[k].cleanup();
      teardown();
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_supported_values_accepted
 * @brief   BulgeSizeOn 0 and 3 initialise every module
 */
int test_supported_values_accepted(void) {
  const char *const values[] = {"0", "3"};
  for (size_t k = 0; k < COUNT(MODULES); k++) {
    for (size_t v = 0; v < COUNT(values); v++) {
      setup(values[v]);
      const char *log = "";
      const int status = init_captured(&MODULES[k], &log);
      printf("\n  %s with BulgeSizeOn %s: init %d", MODULES[k].name, values[v], status);
      TEST_ASSERT(status == 0, "init must accept BulgeSizeOn 0 and 3");
      MODULES[k].cleanup();
      teardown();
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_mode0_helpers
 * @brief   BulgeSizeOn 0: get_bulge_radius zeroes and returns 0, the update returns at once
 */
int test_mode0_helpers(void) {
  const struct MimicSageBulgeSize r0 = {MIMIC_SAGE_BULGE_SIZE_OFF, 0.73};
  struct GalaxyData g;
  memset(&g, 0, sizeof(g));
  g.MergerBulgeMass = 0.5f;
  g.InstabilityBulgeMass = 0.25f;
  g.DiskScaleRadius = 0.004f;
  g.BulgeRadius = 0.0005f;
  g.MergerBulgeRadius = 0.0007f;
  g.InstabilityBulgeRadius = 0.0003f;

  struct GalaxyData updated = g;
  struct MimicSageInstabilityRadiusLocals ul;
  mimic_sage_update_instability_bulge_radius(&updated, 0.0625, 0.004, &r0, &ul);
  TEST_ASSERT(memcmp(&updated, &g, sizeof(g)) == 0,
              "mode 0: the update must leave every member, the stale radii included, as it was");
  TEST_ASSERT(ul.returned_early == 1 && isnan(ul.R_new), "mode 0: the update returns at once");

  struct MimicSageBulgeRadiusLocals bl;
  const double r = mimic_sage_get_bulge_radius(&g, &r0, &bl);
  TEST_ASSERT(r == 0.0 && g.BulgeRadius == 0.0f && g.MergerBulgeRadius == 0.0f &&
                  g.InstabilityBulgeRadius == 0.0f,
              "mode 0: get_bulge_radius zeroes the three radii and returns 0");
  TEST_ASSERT(g.MergerBulgeMass == 0.5f && g.InstabilityBulgeMass == 0.25f,
              "mode 0: get_bulge_radius leaves the channel masses");
  TEST_ASSERT(isnan(bl.M_total) && bl.total_nonpositive == -1,
              "mode 0: get_bulge_radius forms no local");
  return TEST_PASS;
}

/**
 * @test    test_mode3_disk_radius_needs_configuration
 * @brief   The disk-radius module refuses BulgeSizeOn 3 without the run configuration
 *          (Hubble_h, R/src/model_misc.c:249) and runs BulgeSizeOn 0 without it
 */
int test_mode3_disk_radius_needs_configuration(void) {
  const char *const values[] = {"3", "0"};
  for (size_t v = 0; v < COUNT(values); v++) {
    setup(values[v]);
    TEST_ASSERT(sage26_set_disk_scale_radius_init() == 0, "init must accept 0 and 3");
    struct GalaxyData g;
    memset(&g, 0, sizeof(g));
    struct Halo halo;
    memset(&halo, 0, sizeof(halo));
    halo.galaxy = &g;
    halo.Type = 0;
    halo.Vvir = 200.0;
    halo.Rvir = 0.2;
    halo.Spin[0] = 1.0f;
    struct ModuleContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.params = NULL;
    const int status = sage26_set_disk_scale_radius_process(&ctx, &halo, 1);
    printf("\n  BulgeSizeOn %s without the run configuration: process %d", values[v], status);
    if (v == 0) {
      TEST_ASSERT(status != 0, "mode 3 must refuse a context without the run configuration");
    } else {
      TEST_ASSERT(status == 0, "mode 0 reads no Hubble_h and must run without it");
    }
    sage26_set_disk_scale_radius_cleanup();
    teardown();
  }
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: SAGE26 BulgeSizeOn parameter and mode-0 size helpers\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);

  TEST_RUN(test_missing_parameter_refused);
  TEST_RUN(test_unsupported_values_refused);
  TEST_RUN(test_supported_values_accepted);
  TEST_RUN(test_mode0_helpers);
  TEST_RUN(test_mode3_disk_radius_needs_configuration);

  check_memory_leaks();
  TEST_SUMMARY();
  return TEST_RESULT();
}
