/**
 * @file    test_unit_sage26_finalize_cooling_heating.c
 * @brief   Unit tests for sage26_finalize_cooling_heating
 *
 * Validates: module lifecycle, the once-per-FoF normalisation of Cooling and
 * Heating by the first galaxy's interval (SAGE26 core_build_model.c:590-603), and
 * the package's boundary handling: merged (Type 3) rows and rows without a galaxy
 * are skipped, and a first galaxy without a positive interval zeroes the energies.
 *
 * Test cases:
 *   - test_module_initialization: module init/cleanup lifecycle through the module system
 *   - test_placement_enforced: init() rejects a substep-phase, pre_timestep, missing or
 *     duplicated post_timestep entry (it must normalise once, after the substeps)
 *   - test_normalises_by_first_galaxy_interval: every live galaxy uses the first row's dT
 *   - test_merged_galaxies_untouched: Type 3 rows keep their energies
 *   - test_first_galaxy_merged_sets_interval: a merged first galaxy still sets deltaT
 *   - test_rows_without_galaxy_skipped: NULL-galaxy rows are skipped, also as the first row
 *   - test_no_positive_interval_zeroes: first galaxy dT <= 0 zeroes live galaxies' energies
 *   - test_diagnostics_untouched: tcool, tff, RcoolToRvir, CoolingRate are not changed
 *   - test_empty_input: NULL halos or ngal 0 is a no-op
 *   - test_mdot_writer_conversion: the output mdot rates are SAGE26's writer expression of the
 *     raw rates (save_gals_hdf5.c:996-997), left to right, for every live galaxy
 *   - test_mdot_on_nonpositive_interval: the conversion also runs where the first galaxy has no
 *     positive interval, and a never-assigned (zero) raw rate is written as 0
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/proto.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"

extern int sage26_finalize_cooling_heating_init(void);
extern int sage26_finalize_cooling_heating_process(struct ModuleContext *ctx, struct Halo *halos,
                                                   int ngal);
extern int sage26_finalize_cooling_heating_cleanup(void);

#define NROWS 4

static struct GalaxyData galaxies[NROWS];
static struct Halo halos[NROWS];
static struct ModuleContext ctx;

/**
 * @brief Append one entry to MimicConfig.post_timestep
 *
 * Allocated as tests/framework/test_phase_config.h allocates the other phases
 * (mymalloc_cat array, strdup name), so module_system_cleanup() frees it.
 */
static void test_post_timestep_add(const char *module_name, enum ProcessingMode mode) {
  if (MimicConfig.post_timestep == NULL) {
    MimicConfig.post_timestep =
        mymalloc_cat(TEST_PHASE_MODULE_CAP * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.num_post_timestep = 0;
  }
  const int i = MimicConfig.num_post_timestep++;
  MimicConfig.post_timestep[i].module_name = strdup(module_name);
  MimicConfig.post_timestep[i].processing_mode = mode;
  MimicConfig.post_timestep[i].resolved = NULL;
}

/** @brief Fresh configuration with the memory system and modules registered */
static void fresh_config(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
}

/** @brief NROWS rows: Type 0 central then Type 1 satellites, each with its own dT and energies */
static void setup_rows(void) {
  memset(galaxies, 0, sizeof(galaxies));
  memset(halos, 0, sizeof(halos));
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = 10;
  for (int i = 0; i < NROWS; i++) {
    halos[i].galaxy = &galaxies[i];
    halos[i].Type = (i == 0) ? 0 : 1;
    halos[i].SnapNum = 40 + i;
    halos[i].dT = 0.25 + 0.25 * i; /* differs per row: only row 0's may be used */
    galaxies[i].Cooling = 3.0 * (i + 1);
    galaxies[i].Heating = 1.5 * (i + 1);
  }
}

/**
 * @test    test_module_initialization
 * @brief   The module initialises and cleans up through the module system in post_timestep
 */
int test_module_initialization(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  test_post_timestep_add("sage26_finalize_cooling_heating", PROCESSING_MODE_FULL_HALO);

  const int result = module_system_init();
  TEST_ASSERT(result == 0, "module system initialization should succeed");

  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_placement_enforced
 * @brief   The module must run exactly once in post_timestep as process_full_halo
 *
 * In a substep phase it would multiply by 1/dT once per substep; in pre_timestep
 * before the energies accumulate; twice in post_timestep it would normalise twice.
 */
int test_placement_enforced(void) {
  /* In a substep phase (as well as post_timestep) */
  fresh_config();
  test_post_timestep_add("sage26_finalize_cooling_heating", PROCESSING_MODE_FULL_HALO);
  test_phase_add("galaxy_physics", "sage26_finalize_cooling_heating", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a substep-phase entry is rejected");
  module_system_cleanup();
  check_memory_leaks();

  /* In pre_timestep (as well as post_timestep) */
  fresh_config();
  test_post_timestep_add("sage26_finalize_cooling_heating", PROCESSING_MODE_FULL_HALO);
  test_pre_timestep_add("sage26_finalize_cooling_heating", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a pre_timestep entry is rejected");
  module_system_cleanup();
  check_memory_leaks();

  /* Twice in post_timestep */
  fresh_config();
  test_post_timestep_add("sage26_finalize_cooling_heating", PROCESSING_MODE_FULL_HALO);
  test_post_timestep_add("sage26_finalize_cooling_heating", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(module_system_init() != 0, "a second post_timestep entry is rejected");
  module_system_cleanup();
  check_memory_leaks();

  /* Not configured at all: a direct init() finds no post_timestep entry */
  reset_config();
  TEST_ASSERT(sage26_finalize_cooling_heating_init() != 0,
              "init fails without a post_timestep entry");
  return TEST_PASS;
}

/**
 * @test    test_normalises_by_first_galaxy_interval
 * @brief   Cooling and Heating of every live galaxy are multiplied by 1.0 / (first row's dT)
 */
int test_normalises_by_first_galaxy_interval(void) {
  setup_rows();
  const double inv_deltaT = 1.0 / halos[0].dT;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  for (int i = 0; i < NROWS; i++) {
    double expected_cooling = 3.0 * (i + 1);
    double expected_heating = 1.5 * (i + 1);
    expected_cooling *= inv_deltaT;
    expected_heating *= inv_deltaT;
    TEST_ASSERT(galaxies[i].Cooling == expected_cooling,
                "Cooling must be the sum times 1.0/deltaT of the first galaxy, exactly");
    TEST_ASSERT(galaxies[i].Heating == expected_heating,
                "Heating must be the sum times 1.0/deltaT of the first galaxy, exactly");
  }
  return TEST_PASS;
}

/**
 * @test    test_merged_galaxies_untouched
 * @brief   A row merged or disrupted this snapshot (Type 3) keeps its energies
 */
int test_merged_galaxies_untouched(void) {
  setup_rows();
  halos[2].Type = 3;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  TEST_ASSERT(galaxies[2].Cooling == 9.0, "merged galaxy's Cooling must be unchanged");
  TEST_ASSERT(galaxies[2].Heating == 4.5, "merged galaxy's Heating must be unchanged");
  TEST_ASSERT(galaxies[1].Cooling == 6.0 * (1.0 / halos[0].dT), "live galaxy is normalised");
  return TEST_PASS;
}

/**
 * @test    test_first_galaxy_merged_sets_interval
 * @brief   SAGE26 takes deltaT from galaxy zero even when galaxy zero has merged
 */
int test_first_galaxy_merged_sets_interval(void) {
  setup_rows();
  halos[0].Type = 3;
  const double inv_deltaT = 1.0 / halos[0].dT;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  TEST_ASSERT(galaxies[0].Cooling == 3.0, "merged first galaxy keeps its Cooling");
  TEST_ASSERT(galaxies[1].Cooling == 6.0 * inv_deltaT,
              "a live galaxy is normalised by the merged first galaxy's interval");
  TEST_ASSERT(galaxies[3].Heating == 6.0 * inv_deltaT,
              "a live galaxy is normalised by the merged first galaxy's interval");
  return TEST_PASS;
}

/**
 * @test    test_rows_without_galaxy_skipped
 * @brief   Rows without a galaxy are skipped; the first row with a galaxy sets the interval
 */
int test_rows_without_galaxy_skipped(void) {
  setup_rows();
  halos[0].galaxy = NULL; /* the core can keep a row whose galaxy it discarded */
  halos[2].galaxy = NULL;
  const double inv_deltaT = 1.0 / halos[1].dT;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed with NULL-galaxy rows");
  TEST_ASSERT(galaxies[1].Cooling == 6.0 * inv_deltaT,
              "the first row with a galaxy sets the interval");
  TEST_ASSERT(galaxies[3].Cooling == 12.0 * inv_deltaT,
              "every live galaxy uses the first galaxy row's interval");
  TEST_ASSERT(galaxies[0].Cooling == 3.0 && galaxies[2].Cooling == 9.0,
              "galaxies detached from their rows are not touched");
  return TEST_PASS;
}

/**
 * @test    test_no_positive_interval_zeroes
 * @brief   Where the first galaxy has no positive interval (dT = -1 at birth), the
 *          live galaxies' energies are set to 0; merged rows are left alone
 */
int test_no_positive_interval_zeroes(void) {
  setup_rows();
  halos[0].dT = -1.0;
  halos[3].Type = 3;

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(galaxies[i].Cooling == 0.0 && galaxies[i].Heating == 0.0,
                "live galaxies' energies are zeroed without a positive interval");
  }
  TEST_ASSERT(galaxies[3].Cooling == 12.0, "merged galaxy is left alone");
  return TEST_PASS;
}

/**
 * @test    test_diagnostics_untouched
 * @brief   The cooling diagnostics are not normalised
 */
int test_diagnostics_untouched(void) {
  setup_rows();
  for (int i = 0; i < NROWS; i++) {
    galaxies[i].tcool = -11.0f;
    galaxies[i].tff = -22.0f;
    galaxies[i].RcoolToRvir = -33.0f;
    galaxies[i].CoolingRate = -44.0f;
  }

  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  for (int i = 0; i < NROWS; i++) {
    TEST_ASSERT(galaxies[i].tcool == -11.0f && galaxies[i].tff == -22.0f &&
                    galaxies[i].RcoolToRvir == -33.0f && galaxies[i].CoolingRate == -44.0f,
                "diagnostics must be unchanged");
  }
  return TEST_PASS;
}

/**
 * @test    test_empty_input
 * @brief   NULL halos or ngal 0 is a no-op that succeeds
 */
int test_empty_input(void) {
  setup_rows();
  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, NULL, 0) == 0, "NULL halos is a no-op");
  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, 0) == 0, "ngal 0 is a no-op");
  TEST_ASSERT(galaxies[0].Cooling == 3.0, "nothing is changed");
  return TEST_PASS;
}

// ============================================================================
// THE WRITTEN MDOT RATES
// ============================================================================

/** @brief A float from its IEEE-754 bit pattern */
static float float_from_bits(const uint32_t bits) {
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/** @brief A double the compiler cannot constant-fold (the expectation is evaluated at run time) */
static double opaque_double(const double value) {
  volatile double v = value;
  return v;
}

/*
 * A unit mass and two raw rates for which SAGE26's left-to-right conversion and the same
 * conversion regrouped as one constant factor round to different floats (found by an
 * exhaustive scratch search): UnitMass_in_g = 1.989e43 * (1 + 25e-6), UnitTime_in_s the
 * mini-Millennium 3.08568e24 / 1e5.
 */
#define T_UNIT_MASS 0x1.c8a9d5f481618p+143
#define T_UNIT_TIME 0x1.ac39601d1945fp+64
#define T_RAW_COOL_BITS 0x3fa9c5eau   /* 1.32635236 */
#define T_RAW_STREAM_BITS 0x3ff90024u /* 1.94531679 */

/** @brief SAGE26's writer expression (R/src/io/save_gals_hdf5.c:996-997), inline */
static float writer_value(const float raw) {
  return (float)(raw * opaque_double(T_UNIT_MASS) / opaque_double(T_UNIT_TIME) * 3.155e7 /
                 1.989e33);
}

/** @brief The same conversion regrouped as one constant factor (the form it must not take) */
static float grouped_value(const float raw) {
  return (float)(raw *
                 (opaque_double(T_UNIT_MASS) / opaque_double(T_UNIT_TIME) * 3.155e7 / 1.989e33));
}

/** @brief Rows with distinguishable raw rates and stale output values */
static void setup_mdot_rows(void) {
  setup_rows();
  MimicConfig.UnitMass_in_g = T_UNIT_MASS;
  MimicConfig.UnitTime_in_s = T_UNIT_TIME;
  for (int i = 0; i < NROWS; i++) {
    galaxies[i].mdot_cool = -77.0f;
    galaxies[i].mdot_stream = -88.0f;
  }
  galaxies[0].mdot_cool_raw = float_from_bits(T_RAW_COOL_BITS);
  galaxies[0].mdot_stream_raw = float_from_bits(T_RAW_STREAM_BITS);
  galaxies[1].mdot_cool_raw = 0.0f; /* never assigned: SAGE26's creation value */
  galaxies[1].mdot_stream_raw = 0.0f;
  galaxies[2].mdot_cool_raw = 3.0f;
  galaxies[2].mdot_stream_raw = 0.25f;
  galaxies[3].mdot_cool_raw = 7.0f;
  galaxies[3].mdot_stream_raw = 9.0f;
  halos[3].Type = 3; /* merged this snapshot: not finalised */
}

/**
 * @test    test_mdot_writer_conversion
 * @brief   mdot_cool and mdot_stream are SAGE26's writer conversion of the raw float rates
 *
 * Expected: every live galaxy's outputs equal raw * UnitMass_in_g / UnitTime_in_s *
 * SEC_PER_YEAR / SOLAR_MASS evaluated left to right and narrowed (SAGE26's writer, with
 * 3.155e7 and 1.989e33, R/src/macros.h:24, :38); for row 0 that differs from the regrouped
 * constant factor; a zero raw rate is written 0; the merged row keeps its stale outputs.
 */
int test_mdot_writer_conversion(void) {
  setup_mdot_rows();
  TEST_ASSERT(writer_value(float_from_bits(T_RAW_COOL_BITS)) !=
                      grouped_value(float_from_bits(T_RAW_COOL_BITS)) &&
                  writer_value(float_from_bits(T_RAW_STREAM_BITS)) !=
                      grouped_value(float_from_bits(T_RAW_STREAM_BITS)),
              "precondition: the regrouped conversion rounds differently on row 0");
  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(galaxies[i].mdot_cool == writer_value(galaxies[i].mdot_cool_raw) &&
                    galaxies[i].mdot_stream == writer_value(galaxies[i].mdot_stream_raw),
                "a live galaxy's outputs are the writer conversion of its raw rates");
  }
  TEST_ASSERT(galaxies[1].mdot_cool == 0.0f && galaxies[1].mdot_stream == 0.0f,
              "a never-assigned rate is written 0");
  TEST_ASSERT(galaxies[3].mdot_cool == -77.0f && galaxies[3].mdot_stream == -88.0f,
              "a merged galaxy is not finalised");
  return TEST_PASS;
}

/**
 * @test    test_mdot_on_nonpositive_interval
 * @brief   The conversion runs on the branch where the first galaxy has no positive interval
 *
 * Expected: with the first row's dT -1 (the energies zeroed) every live galaxy's outputs are
 * still the writer conversion of its raw rates, the zero raw rate is written 0 over the stale
 * output, and the merged row keeps its stale outputs.
 */
int test_mdot_on_nonpositive_interval(void) {
  setup_mdot_rows();
  halos[0].dT = -1.0;
  TEST_ASSERT(sage26_finalize_cooling_heating_process(&ctx, halos, NROWS) == 0,
              "process should succeed");
  TEST_ASSERT(galaxies[0].Cooling == 0.0, "precondition: the non-positive-interval branch ran");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(galaxies[i].mdot_cool == writer_value(galaxies[i].mdot_cool_raw) &&
                    galaxies[i].mdot_stream == writer_value(galaxies[i].mdot_stream_raw),
                "a live galaxy's outputs are written on this branch too");
  }
  TEST_ASSERT(galaxies[1].mdot_cool == 0.0f && galaxies[1].mdot_stream == 0.0f,
              "a never-assigned rate is written 0 over the stale output");
  TEST_ASSERT(galaxies[3].mdot_cool == -77.0f && galaxies[3].mdot_stream == -88.0f,
              "a merged galaxy is not finalised");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_finalize_cooling_heating Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_module_initialization);
  TEST_RUN(test_placement_enforced);
  TEST_RUN(test_normalises_by_first_galaxy_interval);
  TEST_RUN(test_merged_galaxies_untouched);
  TEST_RUN(test_first_galaxy_merged_sets_interval);
  TEST_RUN(test_rows_without_galaxy_skipped);
  TEST_RUN(test_no_positive_interval_zeroes);
  TEST_RUN(test_diagnostics_untouched);
  TEST_RUN(test_empty_input);
  TEST_RUN(test_mdot_writer_conversion);
  TEST_RUN(test_mdot_on_nonpositive_interval);

  TEST_SUMMARY();
  return TEST_RESULT();
}
