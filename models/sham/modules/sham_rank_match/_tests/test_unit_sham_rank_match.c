/**
 * @file    test_unit_sham_rank_match.c
 * @brief   Unit tests for the sham_rank_match module
 *
 * Validates:
 * - the target converted to the simulation's h (h = 0.6774 and 0.73) and its
 *   tabulated n(>M) against reference values, and the inversion against the
 *   independent double-precision reference of
 *   _tests/sham_rank_match_reference.py (the table between the
 *   SHAM_DECIMAL_REFERENCE markers, which test_integration_sham_rank_match.py
 *   checks against it): log10 M* within 1e-4 and the mask/assign outcome exactly
 * - init(): the placement guards (exactly once in pre_timestep as
 *   process_full_halo, in no other FoF phase, and in post_snapshot as
 *   process_snapshot), every parameter's domain including the strings the
 *   strict parser accepts and the range checks would pass, the box and Hubble
 *   checks, and the redshift window over the output snapshots
 * - process() (pre_timestep): Type 2 retirement and Type 1 retention, the peak
 *   ratchet and its float storage bound (Mvir = FLT_MAX stored exactly, 1e39
 *   fails with the ID and value), the non-member reset, no other write, no
 *   allocation, and validation before any write
 * - process_snapshot() (post_snapshot): the hand-solvable tie order, the rank
 *   masses through the helper chain, permutation and repeat bitwise identity,
 *   the cross-FoF global rank, masking from the floor down, peak persistence
 *   through a masked snapshot, entry validation with no partial assignment,
 *   empty and all-ineligible populations, silence on non-output snapshots, the
 *   audit line, no net allocation across snapshots, and a rank whose stellar
 *   mass overflows the float StellarMass storage failing the snapshot with
 *   nothing written
 * - the registered dispatch of both callbacks (execute_module_pipeline() over a
 *   hand-built FoF workspace, then execute_post_snapshot()), with the fixture's
 *   rank 0 mass checked against the reference
 *
 * Every case ends with a leak check.
 */

#include "framework/test_framework.h"
#include "core/fof_workspace.h"
#include "core/galaxy_pool.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "framework/test_phase_config.h"
#include "include/globals.h"
#include "include/types.h"
#include "io/vertical/reader.h"
#include "util/error.h"
#include "util/memory.h"

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shared SHAM test fixture boilerplate (counters, config reset, shipped parameters) */
#include "modules/_tests/sham_test_fixtures.h"

/* Module under test: lifecycle and the helpers declared in its header */
#include "modules/sham_rank_match/sham_rank_match.h"
extern int sham_rank_match_init(void);
extern int sham_rank_match_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);
extern int sham_rank_match_process_snapshot(const struct SnapshotContext *ctx,
                                            const struct Halo *halos, int64_t count);
extern int sham_rank_match_cleanup(void);

/** Largest synthetic population used by any test */
#define MAX_POPULATION 512

/** Agreement required between the module's log10 M* and the reference, dex */
#define LOG_MASS_TOLERANCE 1.0e-4

/* ============================================================================
 * FIXTURES
 * ============================================================================ */

static struct Halo halos[MAX_POPULATION];
static struct GalaxyData galaxies[MAX_POPULATION];
static struct Halo halos_before[MAX_POPULATION];
static struct GalaxyData galaxies_before[MAX_POPULATION];

static FILE *log_file = NULL;
static char log_text[1 << 16];

/** @brief Route every log line at INFO and above into a file */
static void capture_log(void) {
  log_file = tmpfile();
  initialize_error_handling(LOG_LEVEL_INFO, log_file);
}

/** @brief Stop capturing and return what was logged */
static const char *captured_log(void) {
  log_text[0] = '\0';
  if (log_file != NULL) {
    fflush(log_file);
    rewind(log_file);
    const size_t n = fread(log_text, 1, sizeof(log_text) - 1, log_file);
    log_text[n] = '\0';
    fclose(log_file);
    log_file = NULL;
  }
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  return log_text;
}

static void add_post_timestep(const char *module_name, enum ProcessingMode mode) {
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

/** @brief Shipped parameters (one optionally overridden) and the module in both of its phases */
static void configure_run(const char *override_name, const char *override_value) {
  set_sham_test_parameters(override_name, override_value);
  ensure_modules_registered();
  MimicConfig.ProcessingOrder = INPUT_PROCESSING_ORDER_HORIZONTAL;
  test_pre_timestep_add("sham_rank_match", PROCESSING_MODE_FULL_HALO);
  test_post_snapshot_add("sham_rank_match", PROCESSING_MODE_SNAPSHOT);
}

/** @brief Release the module and the phase configuration; safe to repeat */
static void release_run(void) {
  if (log_file != NULL) {
    (void)captured_log();
  }
  sham_rank_match_cleanup();
  module_system_cleanup(); /* frees the phase configuration */
}

/** @brief init() directly over configure_run(); the module stays initialised until release */
static int init_run(const char *override_name, const char *override_value) {
  configure_run(override_name, override_value);
  return sham_rank_match_init();
}

/** @brief init() for one parameter override, then release; returns init's result */
static int init_with(const char *name, const char *value) {
  const int rc = init_run(name, value);
  release_run();
  return rc;
}

/** Build entry i with a galaxy; everything not set here stays zeroed, including the peaks */
static void set_entry(int64_t i, int type, long long id, float vmax, double mvir) {
  memset(&halos[i], 0, sizeof(halos[i]));
  memset(&galaxies[i], 0, sizeof(galaxies[i]));
  halos[i].Type = type;
  halos[i].UniqueGalaxyID = id;
  halos[i].Vmax = vmax;
  halos[i].Mvir = mvir;
  halos[i].galaxy = &galaxies[i];
}

/** Snapshot entry i as the pre_timestep callback leaves it: peaks set, a non-member */
static void set_ranked_entry(int64_t i, int type, long long id, float vpeak, int fof) {
  set_entry(i, type, id, vpeak, 1.0);
  halos[i].CentralHalo = fof; /* FoF label only; the module never reads it */
  galaxies[i].ShamVpeak = vpeak;
  galaxies[i].ShamMpeak = 1.0f;
  galaxies[i].ShamGhost = 1;
}

static void save_population(int64_t count) {
  memcpy(halos_before, halos, (size_t)count * sizeof(halos[0]));
  memcpy(galaxies_before, galaxies, (size_t)count * sizeof(galaxies[0]));
}

/** Whether halos and galaxies are byte-identical to the last save_population() */
static bool population_unchanged(int64_t count) {
  return memcmp(halos_before, halos, (size_t)count * sizeof(halos[0])) == 0 &&
         memcmp(galaxies_before, galaxies, (size_t)count * sizeof(galaxies[0])) == 0;
}

static uint32_t float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static int64_t find_id(int64_t count, long long id) {
  for (int64_t i = 0; i < count; i++) {
    if (halos[i].UniqueGalaxyID == id) {
      return i;
    }
  }
  return -1;
}

/**
 * The internal StellarMass the module must store at zero-based rank r, formed
 * through the same public helpers in the same order; 0 for a masked rank.
 */
static float expected_rank_mass(int64_t rank) {
  const struct ShamRankMatchTarget *target = sham_rank_match_active_target();
  const double density =
      sham_rank_match_rank_density(rank, MimicConfig.BoxSize, target->hubble_sim);
  double mass = 0.0;
  if (sham_rank_match_mass_at_density(target, density, &mass) != SHAM_RANK_ASSIGNED) {
    return 0.0f;
  }
  return (float)(mass * target->hubble_sim / SHAM_MASS_UNIT_MSUN);
}

/** log10 of a stored StellarMass in physical Msun at the fixture's h */
static double log10_physical_mass(float stellar_mass) {
  return log10((double)stellar_mass * SHAM_MASS_UNIT_MSUN / SHAM_TEST_HUBBLE);
}

/** Run process_snapshot() at snapshot 5, an output snapshot of the fixture, on entries [0, count)
 */
static int rank_population(int64_t count) {
  const struct SnapshotContext ctx = {5, 0.0, 0.0, &MimicConfig};
  return sham_rank_match_process_snapshot(&ctx, count > 0 ? halos : NULL, count);
}

/* ============================================================================
 * TARGET AND REFERENCE
 * ============================================================================ */

/** Whether got agrees with a quoted value to five significant figures (relative 1e-5) */
static bool five_figures(double got, double quoted) { return fabs(got / quoted - 1.0) <= 1.0e-5; }

/**
 * @test   test_converted_target_values
 * @brief  The h-converted parameters, the table's shape and n(>M) at five masses
 */
int test_converted_target_values(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "the shipped parameters initialise at h = 0.6774");
  const struct ShamRankMatchTarget *t = sham_rank_match_active_target();
  TEST_ASSERT(t != NULL, "init publishes its target");
  TEST_ASSERT(five_figures(t->log_mstar, 10.68851), "log10 Ms_sim = 10.68851 at h = 0.6774");
  TEST_ASSERT(five_figures(t->phi1, 3.58870e-3), "phi1_sim = 3.58870e-3 at h = 0.6774");
  TEST_ASSERT(five_figures(t->phi2, 7.15927e-4), "phi2_sim = 7.15927e-4 at h = 0.6774");
  TEST_ASSERT(t->alpha1 == -0.35 && t->alpha2 == -1.47, "the slopes are not rescaled");

  static const double log_masses[] = {8.0, 9.0, 10.0, 11.0, 11.5};
  static const double reference[] = {3.031993e-2, 1.162303e-2, 4.370059e-3, 3.398640e-4,
                                     2.793662e-6};
  for (int k = 0; k < 5; k++) {
    const double n = sham_rank_match_cumulative_density(t, pow(10.0, log_masses[k]));
    if (!(fabs(n / reference[k] - 1.0) <= 1.0e-4)) {
      fprintf(stderr, "n(>10^%g) = %.9e, reference %.9e\n", log_masses[k], n, reference[k]);
      TEST_ASSERT(0, "n(>M) must match the reference to a relative 1e-4");
    }
  }
  TEST_ASSERT(fabs(t->floor_density / 3.031993e-2 - 1.0) <= 1.0e-4,
              "the floor density is n(>10^8)");

  /* Table shape: at least 4096 log-spaced nodes from 10^(floor - 1) to 120 Ms, n strictly
     decreasing to exactly 0 at the top. */
  TEST_ASSERT(t->num_points >= 4096, "the table has at least 4096 nodes");
  TEST_ASSERT(fabs(t->log_mass[0] - 7.0 * log(10.0)) <= 1e-12, "the table starts at 10^7");
  TEST_ASSERT(fabs(t->log_mass[t->num_points - 1] - (t->log_mstar * log(10.0) + log(120.0))) <=
                  1e-12,
              "the table ends at 120 Ms");
  TEST_ASSERT(t->density[t->num_points - 1] == 0.0, "n(>120 Ms) is exactly 0");
  bool decreasing = true;
  for (int k = 0; k + 1 < t->num_points; k++) {
    decreasing = decreasing && t->density[k] > t->density[k + 1];
  }
  TEST_ASSERT(decreasing, "n(>M) decreases strictly along the table");
  TEST_ASSERT(isnan(sham_rank_match_cumulative_density(t, 1.0e6)), "below the table is NaN");
  TEST_ASSERT(sham_rank_match_cumulative_density(t, 1.0e14) == 0.0, "above the table is 0");
  double mass = 0.0;
  TEST_ASSERT(sham_rank_match_inverse(t, 2.0 * t->density[0], &mass) != 0,
              "a density above the table is not extrapolated");
  TEST_ASSERT(sham_rank_match_inverse(t, 0.5 * t->density[t->num_points - 2], &mass) != 0,
              "a density below the last positive node is not extrapolated");
  TEST_ASSERT(sham_rank_match_inverse(t, NAN, &mass) != 0, "a NaN density fails");
  release_run();

  configure_run(NULL, NULL);
  MimicConfig.Hubble_h = 0.73;
  TEST_ASSERT_EQUAL(sham_rank_match_init(), 0, "the shipped parameters initialise at h = 0.73");
  t = sham_rank_match_active_target();
  TEST_ASSERT(five_figures(t->log_mstar, 10.62355), "log10 Ms_sim = 10.62355 at h = 0.73");
  TEST_ASSERT(five_figures(t->phi1, 4.49127e-3), "phi1_sim = 4.49127e-3 at h = 0.73");
  TEST_ASSERT(five_figures(t->phi2, 8.95987e-4), "phi2_sim = 8.95987e-4 at h = 0.73");
  release_run();
  TEST_ASSERT(sham_rank_match_active_target() == NULL, "cleanup withdraws the target");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * One rank-density case and its reference, generated by
 * _tests/sham_rank_match_reference.py (which test_integration_sham_rank_match.py
 * checks against this table): the simulation's h, the rank density in physical
 * Mpc^-3, whether it is assigned (1) or masked (0), and the reference log10 M*
 * in physical Msun (0.0 when masked).
 */
struct ReferenceCase {
  const char *name;
  double hubble;
  double density;
  int assigned;
  double log_mass;
};

/* SHAM_DECIMAL_REFERENCE_BEGIN */
static const struct ReferenceCase reference_cases[] = {
    {"fixture_rank0", 0.6774, 1.554195e-7, 1, 11.65462093},
    {"fixture_rank5", 0.6774, 1.709615e-6, 1, 11.53029151},
    {"mid_1e-4", 0.6774, 1.0e-4, 1, 11.18501602},
    {"mid_1e-3", 0.6774, 1.0e-3, 1, 10.74894360},
    {"mid_1e-2", 0.6774, 1.0e-2, 1, 9.16409725},
    {"floor_inside", 0.6774, 3.0316e-2, 1, 8.00012946},
    {"floor_outside", 0.6774, 3.0323e-2, 0, 0.0},
    {"tail_deep", 0.6774, 1.0e-12, 1, 12.00921460},
    {"tail_extreme", 0.6774, 1.0e-30, 1, 12.48089371},
    {"hubble_073", 0.73, 1.0e-4, 1, 11.14743937},
    {"hubble_073_outside", 0.73, 5.0e-2, 0, 0.0},
};
/* SHAM_DECIMAL_REFERENCE_END */

/**
 * @test   test_reference_table
 * @brief  Every reference case: the outcome exactly, log10 M* within 1e-4 when assigned
 */
int test_reference_table(void) {
  const size_t num_cases = sizeof(reference_cases) / sizeof(reference_cases[0]);
  TEST_ASSERT(num_cases >= 8, "the reference table has at least eight cases");
  for (size_t k = 0; k < num_cases; k++) {
    const struct ReferenceCase *c = &reference_cases[k];
    configure_run(NULL, NULL);
    MimicConfig.Hubble_h = c->hubble;
    TEST_ASSERT_EQUAL(sham_rank_match_init(), 0, "every reference case initialises");
    double mass = 0.0;
    const int outcome =
        sham_rank_match_mass_at_density(sham_rank_match_active_target(), c->density, &mass);
    const int expected = c->assigned ? SHAM_RANK_ASSIGNED : SHAM_RANK_MASKED;
    if (outcome != expected) {
      fprintf(stderr, "case %s: outcome %d, reference %d\n", c->name, outcome, expected);
      TEST_ASSERT(0, "the mask/assign outcome must match the reference exactly");
    }
    if (c->assigned && !(fabs(log10(mass) - c->log_mass) <= LOG_MASS_TOLERANCE)) {
      fprintf(stderr, "case %s: log10 M* = %.8f, reference %.8f\n", c->name, log10(mass),
              c->log_mass);
      TEST_ASSERT(0, "log10 M* must be within 1e-4 of the reference");
    }
    release_run();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * INIT
 * ============================================================================ */

/**
 * @test   test_init_configuration
 * @brief  Exactly once in pre_timestep as process_full_halo, no other FoF phase, and post_snapshot
 */
int test_init_configuration(void) {
  configure_run(NULL, NULL);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "pre_timestep plus post_snapshot is valid");
  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "cleanup succeeds");

  /* Missing from pre_timestep. */
  set_sham_test_parameters(NULL, NULL);
  test_post_snapshot_add("sham_rank_match", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(sham_rank_match_init() != 0, "init fails without a pre_timestep entry");
  release_run();

  /* Missing from post_snapshot. */
  set_sham_test_parameters(NULL, NULL);
  test_pre_timestep_add("sham_rank_match", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sham_rank_match_init() != 0, "init fails without a post_snapshot entry");
  release_run();

  /* Twice in pre_timestep. */
  configure_run(NULL, NULL);
  test_pre_timestep_add("sham_rank_match", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sham_rank_match_init() != 0, "two pre_timestep entries are rejected");
  release_run();

  /* pre_timestep with the wrong mode. */
  set_sham_test_parameters(NULL, NULL);
  test_pre_timestep_add("sham_rank_match", PROCESSING_MODE_BY_GALAXY);
  test_post_snapshot_add("sham_rank_match", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(sham_rank_match_init() != 0, "pre_timestep as process_by_galaxy is rejected");
  release_run();

  /* Also in another FoF phase. */
  configure_run(NULL, NULL);
  add_post_timestep("sham_rank_match", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sham_rank_match_init() != 0, "a post_timestep entry as well is rejected");
  release_run();

  configure_run(NULL, NULL);
  test_phase_add("galaxy_physics", "sham_rank_match", PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sham_rank_match_init() != 0, "a substep-phase entry as well is rejected");
  release_run();

  /* The registry rejects a second post_snapshot entry before init() runs. */
  configure_run(NULL, NULL);
  test_post_snapshot_add("sham_rank_match", PROCESSING_MODE_SNAPSHOT);
  TEST_ASSERT(module_system_init() != 0, "two post_snapshot entries are rejected at startup");
  module_system_cleanup();

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_init_parameter_domain
 * @brief  Non-finite, malformed, missing and out-of-range values fail; boundary values pass
 */
int test_init_parameter_domain(void) {
  TEST_ASSERT_EQUAL(init_with(NULL, NULL), 0, "the shipped parameters initialise");

  static const char *const malformed[] = {"nan",   "inf", "infinity", "-inf",
                                          "1e400", "1,0", "abc",      ""};
  for (int k = 0; k < SHAM_NUM_PARAMETERS; k++) {
    for (size_t m = 0; m < sizeof(malformed) / sizeof(malformed[0]); m++) {
      if (init_with(sham_parameter_names[k], malformed[m]) == 0) {
        fprintf(stderr, "%s='%s' was accepted\n", sham_parameter_names[k], malformed[m]);
        TEST_ASSERT(0, "every non-finite or malformed double must fail init");
      }
    }
    TEST_ASSERT(init_with(sham_parameter_names[k], NULL) != 0, "every parameter is required");
  }

  TEST_ASSERT(init_with("ShamTargetPhi1", "0") != 0, "phi1 = 0 is rejected");
  TEST_ASSERT(init_with("ShamTargetPhi1", "-3.96e-3") != 0, "phi1 < 0 is rejected");
  TEST_ASSERT(init_with("ShamTargetPhi2", "0") != 0, "phi2 = 0 is rejected");
  TEST_ASSERT(init_with("ShamTargetPhi2", "-1e-4") != 0, "phi2 < 0 is rejected");
  TEST_ASSERT(init_with("ShamTargetHubble", "0") != 0, "h_obs = 0 is rejected");
  TEST_ASSERT(init_with("ShamTargetHubble", "2") != 0, "h_obs = 2 is rejected");
  TEST_ASSERT(init_with("ShamTargetHubble", "-0.7") != 0, "h_obs < 0 is rejected");
  TEST_ASSERT(init_with("ShamTargetHubble", "1.99") == 0, "h_obs = 1.99 passes");
  TEST_ASSERT(init_with("ShamTargetHubble", "0.01") == 0, "h_obs = 0.01 passes");
  TEST_ASSERT(init_with("ShamMinVpeak", "0") != 0, "ShamMinVpeak = 0 is rejected");
  TEST_ASSERT(init_with("ShamMinVpeak", "-80") != 0, "ShamMinVpeak < 0 is rejected");
  TEST_ASSERT(init_with("ShamMinVpeak", "1e-3") == 0, "a small positive ShamMinVpeak passes");
  TEST_ASSERT(init_with("ShamTargetLogMassFloor", "12.8") != 0,
              "a floor at or above the table top (120 Ms = 10^12.77) is rejected");
  TEST_ASSERT(init_with("ShamTargetLogMassFloor", "12.7") == 0, "a floor just below it passes");
  TEST_ASSERT(init_with("ShamTargetAlpha2", "-400") != 0,
              "slopes whose table overflows are rejected");

  static const double boxes[] = {0.0, -100.0, NAN, INFINITY, 1.0e110};
  for (size_t k = 0; k < sizeof(boxes) / sizeof(boxes[0]); k++) {
    configure_run(NULL, NULL);
    MimicConfig.BoxSize = boxes[k];
    TEST_ASSERT(sham_rank_match_init() != 0,
                "a non-positive, non-finite or density-underflowing box fails");
    release_run();
  }
  static const double hubbles[] = {0.0, -0.6774, NAN};
  for (size_t k = 0; k < sizeof(hubbles) / sizeof(hubbles[0]); k++) {
    configure_run(NULL, NULL);
    MimicConfig.Hubble_h = hubbles[k];
    TEST_ASSERT(sham_rank_match_init() != 0, "a non-positive or NaN simulation h fails");
    release_run();
  }
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_init_redshift_window
 * @brief  init() fails if any output snapshot's redshift exceeds ShamTargetRedshiftMax
 */
int test_init_redshift_window(void) {
  configure_run(NULL, NULL);
  MimicConfig.NOUT = 2;
  MimicConfig.ListOutputSnaps[0] = 5;
  MimicConfig.ListOutputSnaps[1] = 3;
  MimicConfig.ZZ[5] = 0.0005;
  MimicConfig.ZZ[3] = 0.1943;
  TEST_ASSERT_EQUAL(sham_rank_match_init(), 0, "every output snapshot inside z <= 0.2 passes");
  release_run();

  configure_run(NULL, NULL);
  MimicConfig.NOUT = 2;
  MimicConfig.ListOutputSnaps[0] = 5;
  MimicConfig.ListOutputSnaps[1] = 3;
  MimicConfig.ZZ[5] = 0.0005;
  MimicConfig.ZZ[3] = 0.2001;
  capture_log();
  TEST_ASSERT(sham_rank_match_init() != 0, "an output snapshot at z = 0.2001 fails");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "output snapshot 3 has z = 0.2001") != NULL,
              "the error names the snapshot and its redshift");
  release_run();

  configure_run("ShamTargetRedshiftMax", "0.1943");
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 3;
  MimicConfig.ZZ[3] = 0.1943;
  TEST_ASSERT_EQUAL(sham_rank_match_init(), 0, "a redshift exactly at the window passes");
  release_run();

  configure_run(NULL, NULL);
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 2;
  MimicConfig.ZZ[2] = NAN;
  TEST_ASSERT(sham_rank_match_init() != 0, "a NaN output redshift fails");
  release_run();

  /* A non-output snapshot outside the window is allowed: only output snapshots are assigned. */
  configure_run(NULL, NULL);
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 5;
  MimicConfig.ZZ[5] = 0.0005;
  MimicConfig.ZZ[0] = 5.0;
  TEST_ASSERT_EQUAL(sham_rank_match_init(), 0, "a high-redshift non-output snapshot is allowed");
  release_run();

  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * PROCESS (PRE_TIMESTEP)
 * ============================================================================ */

/**
 * @test   test_process_retires_type2_and_updates_peaks
 * @brief  Type 2 -> 3, Types 0/1 ratchet their peaks, every row becomes a non-member, no other
 * write
 */
int test_process_retires_type2_and_updates_peaks(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");

  set_entry(0, 0, 11, 120.0f, 30.0); /* Vpeak stays at its peak, Mpeak rises */
  galaxies[0].ShamVpeak = 150.0f;
  galaxies[0].ShamMpeak = 20.0f;
  set_entry(1, 1, 12, 180.0f, 10.0); /* Vpeak rises, Mpeak stays */
  galaxies[1].ShamVpeak = 170.0f;
  galaxies[1].ShamMpeak = 40.0f;
  set_entry(2, 2, 13, 900.0f, 500.0); /* carried orphan: retired, its proxies unused */
  galaxies[2].ShamVpeak = 175.0f;
  galaxies[2].ShamMpeak = 25.0f;
  set_entry(3, 3, 14, 0.0f, 0.0); /* already retired, no galaxy */
  halos[3].galaxy = NULL;
  for (int i = 0; i < 3; i++) {
    galaxies[i].StellarMass = 5.0f;
    galaxies[i].ShamGhost = 0;
  }
  save_population(4);

  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.snapshot_number = 4;
  const size_t utility_before = memory_category_bytes(MEM_UTILITY);
  TEST_ASSERT_EQUAL(sham_rank_match_process(&ctx, halos, 4), 0, "the FoF step succeeds");
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == utility_before, "process() never allocates");

  TEST_ASSERT_EQUAL(halos[2].Type, 3, "the Type 2 row is retired to Type 3");
  TEST_ASSERT_EQUAL(halos[1].Type, 1, "the Type 1 row is retained");
  TEST_ASSERT_EQUAL(halos[0].Type, 0, "the Type 0 row is retained");
  TEST_ASSERT(galaxies[0].ShamVpeak == 150.0f && galaxies[0].ShamMpeak == 30.0f,
              "Type 0: Vpeak keeps its peak, Mpeak rises to Mvir");
  TEST_ASSERT(galaxies[1].ShamVpeak == 180.0f && galaxies[1].ShamMpeak == 40.0f,
              "Type 1: Vpeak rises to Vmax, Mpeak keeps its peak");
  TEST_ASSERT(galaxies[2].ShamVpeak == 175.0f && galaxies[2].ShamMpeak == 25.0f,
              "the retired orphan's peaks are not updated");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(galaxies[i].ShamGhost == 1 && galaxies[i].StellarMass == 0.0f,
                "every row with a galaxy is reset to a non-member with no stellar mass");
  }

  /* Nothing else changed: restore the four expected writes and compare bytes. */
  halos[2].Type = 2;
  for (int i = 0; i < 3; i++) {
    galaxies[i].ShamVpeak = galaxies_before[i].ShamVpeak;
    galaxies[i].ShamMpeak = galaxies_before[i].ShamMpeak;
    galaxies[i].ShamGhost = galaxies_before[i].ShamGhost;
    galaxies[i].StellarMass = galaxies_before[i].StellarMass;
  }
  TEST_ASSERT(population_unchanged(4), "process() writes nothing but Type and its four fields");

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_process_peak_float_bound
 * @brief  Mvir = FLT_MAX is stored exactly; Mvir = 1e39 fails the step, naming ID and value
 */
int test_process_peak_float_bound(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  set_entry(0, 0, 21, 200.0f, (double)FLT_MAX);
  TEST_ASSERT_EQUAL(sham_rank_match_process(&ctx, halos, 1), 0, "Mvir = FLT_MAX is storable");
  TEST_ASSERT(galaxies[0].ShamMpeak == FLT_MAX, "Mvir = FLT_MAX is stored exactly");

  set_entry(0, 2, 22, 100.0f, 1.0);
  set_entry(1, 1, 23, 200.0f, 1.0e39);
  galaxies[0].ShamGhost = 0;
  save_population(2);
  capture_log();
  TEST_ASSERT(sham_rank_match_process(&ctx, halos, 2) != 0, "Mvir = 1e39 fails the FoF step");
  const char *log = captured_log();
  /* 1e39 prints as its exact double, 9.9999999999999994e+38, at %.17g. */
  TEST_ASSERT(strstr(log, "UniqueGalaxyID 23 peak mass 9.9999999999999994e+38") != NULL,
              "the error names the UniqueGalaxyID and the value");
  TEST_ASSERT(population_unchanged(2), "a failed step writes nothing, not even the retirement");

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_process_input_validation
 * @brief  Bad calls and malformed rows fail before any write
 */
int test_process_input_validation(void) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  set_entry(0, 0, 31, 200.0f, 1.0);
  TEST_ASSERT(sham_rank_match_process(&ctx, halos, 1) != 0, "process fails before init");

  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");
  TEST_ASSERT(sham_rank_match_process(NULL, halos, 1) != 0, "a NULL context fails");
  TEST_ASSERT(sham_rank_match_process(&ctx, halos, 0) != 0, "ngal = 0 fails");
  TEST_ASSERT(sham_rank_match_process(&ctx, NULL, 1) != 0, "a NULL workspace fails");

  enum { NO_GALAXY, TYPE_FIVE, VMAX_NAN, MVIR_NEGATIVE, VPEAK_INF, MPEAK_NEGATIVE, NUM_CASES };
  for (int c = 0; c < NUM_CASES; c++) {
    set_entry(0, 2, 32, 100.0f, 1.0);
    set_entry(1, 0, 33, 200.0f, 5.0);
    set_entry(2, 1, 34, 150.0f, 2.0);
    switch (c) {
    case NO_GALAXY:
      halos[2].galaxy = NULL;
      break;
    case TYPE_FIVE:
      halos[1].Type = 5;
      break;
    case VMAX_NAN:
      halos[2].Vmax = NAN;
      break;
    case MVIR_NEGATIVE:
      halos[1].Mvir = -1.0;
      break;
    case VPEAK_INF:
      galaxies[1].ShamVpeak = INFINITY;
      break;
    case MPEAK_NEGATIVE:
      galaxies[2].ShamMpeak = -2.0f;
      break;
    default:
      break;
    }
    save_population(3);
    if (sham_rank_match_process(&ctx, halos, 3) == 0) {
      fprintf(stderr, "malformed FoF case %d was accepted\n", c);
      TEST_ASSERT(0, "every malformed FoF step must fail");
    }
    TEST_ASSERT(population_unchanged(3), "a failed FoF step writes nothing");
  }

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * PROCESS_SNAPSHOT (POST_SNAPSHOT)
 * ============================================================================ */

/**
 * @test   test_tie_order_is_hand_solvable
 * @brief  Two equal ShamVpeak: the lower UniqueGalaxyID takes the lower rank
 *
 * Population: ID 80 (Vpeak 300), ID 42 and ID 7 (both 200), ID 9 (Vpeak 79,
 * below the 80 km/s floor). Ranks: 80 -> 0, 7 -> 1, 42 -> 2; ID 9 is not a
 * candidate and keeps the reset values.
 */
int test_tie_order_is_hand_solvable(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");
  set_ranked_entry(0, 0, 42, 200.0f, 0);
  set_ranked_entry(1, 1, 80, 300.0f, 1);
  set_ranked_entry(2, 0, 7, 200.0f, 2);
  set_ranked_entry(3, 0, 9, 79.0f, 3);

  capture_log();
  TEST_ASSERT_EQUAL(rank_population(4), 0, "the snapshot succeeds");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "SHAM audit z=0.0000 candidates=3 assigned=3 masked=0") != NULL,
              "one audit line with the candidate, assigned and masked counts");

  TEST_ASSERT(float_bits(galaxies[1].StellarMass) == float_bits(expected_rank_mass(0)),
              "ID 80 takes rank 0");
  TEST_ASSERT(float_bits(galaxies[2].StellarMass) == float_bits(expected_rank_mass(1)),
              "ID 7 wins the tie and takes rank 1");
  TEST_ASSERT(float_bits(galaxies[0].StellarMass) == float_bits(expected_rank_mass(2)),
              "ID 42 loses the tie and takes rank 2");
  TEST_ASSERT(galaxies[2].StellarMass > galaxies[0].StellarMass,
              "the tie is broken by ascending UniqueGalaxyID");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(galaxies[i].ShamGhost == 0, "assigned candidates become sample members");
  }
  TEST_ASSERT(galaxies[3].ShamGhost == 1 && galaxies[3].StellarMass == 0.0f,
              "an entry below ShamMinVpeak is not a candidate and keeps its reset values");

  /* Independent of the helper chain: rank 0 at h = 0.6774 in a 100 Mpc/h box is the
     reference's fixture_rank0 case. */
  TEST_ASSERT(fabs(log10_physical_mass(galaxies[1].StellarMass) - reference_cases[0].log_mass) <=
                  LOG_MASS_TOLERANCE,
              "rank 0 matches the reference fixture_rank0 mass");

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * Population used by the invariance tests: four FoF labels, Types 0/1, exact
 * Vpeak ties inside and across FoFs, and entries below the 80 km/s floor.
 */
static int64_t build_mixed_population(void) {
  int64_t n = 0;
  for (int k = 0; k < 48; k++) {
    const int type = (k % 3 == 1) ? 1 : 0;
    const long long id = 1000LL + 37LL * ((k * 11) % 48); /* unique, not sorted by k */
    const float vpeak = (k % 7 == 0) ? 60.0f : (float)(100 + (k % 9) * 10); /* many ties */
    set_ranked_entry(n, type, id, vpeak, k % 4);
    n++;
  }
  return n;
}

/** Fisher-Yates shuffle of halos/galaxies [0, count) by a fixed LCG; repoints every galaxy */
static void shuffle_population(int64_t count, uint64_t seed) {
  static struct Halo halos_old[MAX_POPULATION];
  static struct GalaxyData galaxies_old[MAX_POPULATION];
  int64_t order[MAX_POPULATION];
  for (int64_t i = 0; i < count; i++) {
    order[i] = i;
  }
  for (int64_t i = count - 1; i > 0; i--) {
    seed = seed * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    const int64_t j = (int64_t)((seed >> 33) % (uint64_t)(i + 1));
    const int64_t swap = order[i];
    order[i] = order[j];
    order[j] = swap;
  }
  memcpy(halos_old, halos, (size_t)count * sizeof(halos[0]));
  memcpy(galaxies_old, galaxies, (size_t)count * sizeof(galaxies[0]));
  for (int64_t i = 0; i < count; i++) {
    halos[i] = halos_old[order[i]];
    galaxies[i] = galaxies_old[order[i]];
    halos[i].galaxy = &galaxies[i];
  }
}

/** Per-ID StellarMass bits of the mixed population (ascending ID) after one ranking */
static int mixed_population_bits(uint64_t seed, int64_t *n_out, uint32_t *bits) {
  const int64_t n = build_mixed_population();
  if (seed != 0) {
    shuffle_population(n, seed);
  }
  if (rank_population(n) != 0) {
    return -1;
  }
  int64_t written = 0;
  for (long long id = 0; written < n; id++) {
    const int64_t i = find_id(n, id);
    if (i >= 0) {
      bits[written++] = float_bits(galaxies[i].StellarMass);
    }
  }
  *n_out = n;
  return 0;
}

/** Brute-force rank of entry i among the candidates of [0, count) */
static int64_t brute_force_rank(int64_t count, int64_t i) {
  int64_t rank = 0;
  for (int64_t j = 0; j < count; j++) {
    if (j == i || galaxies[j].ShamVpeak < 80.0f) {
      continue;
    }
    if (galaxies[j].ShamVpeak > galaxies[i].ShamVpeak ||
        (galaxies[j].ShamVpeak == galaxies[i].ShamVpeak &&
         halos[j].UniqueGalaxyID < halos[i].UniqueGalaxyID)) {
      rank++;
    }
  }
  return rank;
}

/**
 * @test   test_permutation_and_repeat_identity
 * @brief  Repeats and permutations give identical per-ID bits, each the brute-force rank's mass
 */
int test_permutation_and_repeat_identity(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");
  static uint32_t reference_bits[MAX_POPULATION];
  static uint32_t bits[MAX_POPULATION];
  int64_t reference_n = 0;
  int64_t n = 0;

  TEST_ASSERT_EQUAL(mixed_population_bits(0, &reference_n, reference_bits), 0, "reference run");
  int64_t candidates = 0;
  for (int64_t i = 0; i < reference_n; i++) {
    if (galaxies[i].ShamVpeak < 80.0f) {
      TEST_ASSERT(galaxies[i].StellarMass == 0.0f && galaxies[i].ShamGhost == 1,
                  "a non-candidate stays a non-member");
      continue;
    }
    candidates++;
    TEST_ASSERT(float_bits(galaxies[i].StellarMass) ==
                    float_bits(expected_rank_mass(brute_force_rank(reference_n, i))),
                "every candidate receives the mass of its brute-force rank");
  }
  TEST_ASSERT(candidates > 0 && candidates < reference_n, "the population mixes both kinds");

  TEST_ASSERT_EQUAL(mixed_population_bits(0, &n, bits), 0, "repeat run");
  TEST_ASSERT(n == reference_n && memcmp(bits, reference_bits, (size_t)n * sizeof(bits[0])) == 0,
              "a repeat gives identical bits");
  static const uint64_t seeds[] = {1, 2, 3, 0x5eed, 0xfeedbeef, 77777};
  for (size_t s = 0; s < sizeof(seeds) / sizeof(seeds[0]); s++) {
    TEST_ASSERT_EQUAL(mixed_population_bits(seeds[s], &n, bits), 0, "permuted run");
    TEST_ASSERT(memcmp(bits, reference_bits, (size_t)n * sizeof(bits[0])) == 0,
                "a permutation preserves every per-ID result bitwise");
  }

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_rank_is_global_across_fofs
 * @brief  A higher-proxy galaxy in another FoF shifts every rank of the first FoF down by one
 */
int test_rank_is_global_across_fofs(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");

  set_ranked_entry(0, 0, 11, 150.0f, 0); /* FoF A */
  set_ranked_entry(1, 1, 12, 120.0f, 0); /* FoF A */
  TEST_ASSERT_EQUAL(rank_population(2), 0, "FoF A alone");
  const uint32_t alone_11 = float_bits(galaxies[0].StellarMass);
  const uint32_t alone_12 = float_bits(galaxies[1].StellarMass);
  TEST_ASSERT(alone_11 == float_bits(expected_rank_mass(0)), "ID 11 is rank 0 on its own");

  set_ranked_entry(0, 0, 11, 150.0f, 0);
  set_ranked_entry(1, 1, 12, 120.0f, 0);
  set_ranked_entry(2, 0, 99, 300.0f, 1); /* FoF B, higher proxy */
  TEST_ASSERT_EQUAL(rank_population(3), 0, "with FoF B");
  TEST_ASSERT(float_bits(galaxies[2].StellarMass) == alone_11, "the FoF B galaxy takes rank 0");
  TEST_ASSERT(float_bits(galaxies[0].StellarMass) == alone_12,
              "ID 11 moves to rank 1 and receives what rank 1 received before");
  TEST_ASSERT(float_bits(galaxies[1].StellarMass) == float_bits(expected_rank_mass(2)),
              "ID 12 moves down to rank 2");

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_masking_from_the_floor_down
 * @brief  In a 10 Mpc/h box the ranks whose density exceeds n(>10^8) are masked, all of them
 *
 * With h = 0.6774 the rank density is (r + 0.5) (0.06774)^3 = (r + 0.5) 3.1084e-4 Mpc^-3,
 * which first exceeds n(>10^8) = 3.031993e-2 at r + 0.5 > 97.54, so ranks 0-97 are
 * assigned and ranks 98-119 of a 120-candidate population are masked.
 */
int test_masking_from_the_floor_down(void) {
  configure_run(NULL, NULL);
  MimicConfig.BoxSize = 10.0;
  TEST_ASSERT_EQUAL(sham_rank_match_init(), 0, "init succeeds in a 10 Mpc/h box");

  const int64_t n = 130;
  for (int64_t i = 0; i < n; i++) {
    /* IDs descend with Vpeak, so rank r is entry r; the last ten are below the floor. */
    const float vpeak = (i < 120) ? (float)(1000 - 5 * i) : 50.0f;
    set_ranked_entry(i, (int)(i % 2), 5000 + i, vpeak, (int)(i % 7));
    galaxies[i].StellarMass = 3.0f; /* stale: a masked candidate must be zeroed */
    galaxies[i].ShamGhost = 0;      /* stale: a masked candidate must be flagged */
  }
  capture_log();
  TEST_ASSERT_EQUAL(rank_population(n), 0, "the snapshot succeeds");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "SHAM audit z=0.0000 candidates=120 assigned=98 masked=22") != NULL,
              "the audit counts 120 candidates, 98 assigned and 22 masked");

  for (int64_t r = 0; r < 120; r++) {
    if (r < 98) {
      TEST_ASSERT(galaxies[r].ShamGhost == 0 && galaxies[r].StellarMass > 0.0f,
                  "ranks 0-97 are assigned");
      TEST_ASSERT(float_bits(galaxies[r].StellarMass) == float_bits(expected_rank_mass(r)),
                  "each assigned rank receives its rank mass");
    } else {
      TEST_ASSERT(galaxies[r].ShamGhost == 1 && galaxies[r].StellarMass == 0.0f,
                  "ranks 98-119 are masked: ShamGhost 1 and no stellar mass");
    }
  }
  TEST_ASSERT(log10_physical_mass(galaxies[97].StellarMass) >= 8.0,
              "the last assigned rank lies at or above the 10^8 Msun floor");
  for (int64_t i = 120; i < n; i++) {
    TEST_ASSERT(galaxies[i].ShamGhost == 0 && galaxies[i].StellarMass == 3.0f,
                "non-candidates are not written by the snapshot callback");
  }

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_peak_persists_through_masked_snapshot
 * @brief  A masked candidate keeps its peak; when the population thins it is ranked on that peak
 */
int test_peak_persists_through_masked_snapshot(void) {
  configure_run(NULL, NULL);
  MimicConfig.BoxSize = 10.0;
  TEST_ASSERT_EQUAL(sham_rank_match_init(), 0, "init succeeds in a 10 Mpc/h box");
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));

  /* Step 1: 110 rows; the tracked row (ID 1, Vmax 90) ranks last of 110, beyond rank 97. */
  const int64_t n = 110;
  for (int64_t i = 0; i < n - 1; i++) {
    set_entry(i, 0, 100 + i, (float)(500 - i), 1.0);
  }
  set_entry(n - 1, 1, 1, 90.0f, 7.0);
  TEST_ASSERT_EQUAL(sham_rank_match_process(&ctx, halos, (int)n), 0, "the FoF step succeeds");
  TEST_ASSERT_EQUAL(rank_population(n), 0, "the snapshot succeeds");
  TEST_ASSERT(galaxies[n - 1].ShamGhost == 1 && galaxies[n - 1].StellarMass == 0.0f,
              "the tracked row is masked");
  TEST_ASSERT(galaxies[n - 1].ShamVpeak == 90.0f && galaxies[n - 1].ShamMpeak == 7.0f,
              "masking leaves its peaks alone");

  /* Step 2: only the tracked row remains, with lower proxies. */
  struct GalaxyData tracked = galaxies[n - 1];
  set_entry(0, 1, 1, 70.0f, 2.0);
  galaxies[0] = tracked;
  TEST_ASSERT_EQUAL(sham_rank_match_process(&ctx, halos, 1), 0, "the next FoF step succeeds");
  TEST_ASSERT(galaxies[0].ShamVpeak == 90.0f && galaxies[0].ShamMpeak == 7.0f,
              "the peaks persist above the lower proxies");
  TEST_ASSERT_EQUAL(rank_population(1), 0, "the next snapshot succeeds");
  TEST_ASSERT(galaxies[0].ShamGhost == 0 &&
                  float_bits(galaxies[0].StellarMass) == float_bits(expected_rank_mass(0)),
              "ranked on its persisted 90 km/s peak (Vmax 70 is below the floor), it takes rank 0");

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_snapshot_validation_without_writes
 * @brief  Every contract violation fails the snapshot with no partial assignment
 */
int test_snapshot_validation_without_writes(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");
  enum {
    CASE_TYPE_2,
    CASE_TYPE_3,
    CASE_NULL_GALAXY,
    CASE_ZERO_ID,
    CASE_NEGATIVE_ID,
    CASE_DUPLICATE_CANDIDATES,
    CASE_DUPLICATE_NON_CANDIDATES,
    CASE_VPEAK_NAN,
    CASE_VPEAK_NEGATIVE,
    CASE_MPEAK_INF,
    NUM_CASES
  };
  for (int c = 0; c < NUM_CASES; c++) {
    set_ranked_entry(0, 0, 10, 300.0f, 0);
    set_ranked_entry(1, 1, 20, 50.0f, 0);
    set_ranked_entry(2, 0, 30, 200.0f, 1);
    set_ranked_entry(3, 1, 40, 40.0f, 1);
    switch (c) {
    case CASE_TYPE_2:
      halos[2].Type = 2;
      break;
    case CASE_TYPE_3:
      halos[1].Type = 3;
      break;
    case CASE_NULL_GALAXY:
      halos[3].galaxy = NULL;
      break;
    case CASE_ZERO_ID:
      halos[2].UniqueGalaxyID = 0;
      break;
    case CASE_NEGATIVE_ID:
      halos[1].UniqueGalaxyID = -5;
      break;
    case CASE_DUPLICATE_CANDIDATES:
      halos[2].UniqueGalaxyID = 10;
      break;
    case CASE_DUPLICATE_NON_CANDIDATES:
      halos[3].UniqueGalaxyID = 20;
      break;
    case CASE_VPEAK_NAN:
      galaxies[0].ShamVpeak = NAN;
      break;
    case CASE_VPEAK_NEGATIVE:
      galaxies[3].ShamVpeak = -1.0f;
      break;
    case CASE_MPEAK_INF:
      galaxies[2].ShamMpeak = INFINITY;
      break;
    default:
      break;
    }
    save_population(4);
    const size_t utility_before = memory_category_bytes(MEM_UTILITY);
    if (rank_population(4) == 0) {
      fprintf(stderr, "malformed snapshot case %d was accepted\n", c);
      TEST_ASSERT(0, "every malformed snapshot must fail");
    }
    if (!population_unchanged(4)) {
      fprintf(stderr, "malformed snapshot case %d wrote to the population\n", c);
      TEST_ASSERT(0, "a failed snapshot assigns nothing");
    }
    TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == utility_before,
                "a failed snapshot releases its scratch");
  }

  /* The error names the offending ID and value. */
  set_ranked_entry(0, 0, 10, 300.0f, 0);
  set_ranked_entry(1, 2, 77, 150.0f, 0);
  capture_log();
  TEST_ASSERT(rank_population(2) != 0, "a Type 2 entry fails the snapshot");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "UniqueGalaxyID 77) has Type 2") != NULL,
              "the error names the UniqueGalaxyID and the Type");

  /* Control and NULL-population checks. */
  set_ranked_entry(0, 0, 10, 300.0f, 0);
  set_ranked_entry(1, 1, 20, 50.0f, 0);
  TEST_ASSERT_EQUAL(rank_population(2), 0, "the unmodified population is valid");
  const struct SnapshotContext ctx = {5, 0.0, 0.0, &MimicConfig};
  TEST_ASSERT(sham_rank_match_process_snapshot(&ctx, NULL, 3) != 0,
              "a NULL population with a positive count fails");
  TEST_ASSERT(sham_rank_match_process_snapshot(&ctx, halos, -1) != 0, "a negative count fails");
  TEST_ASSERT(sham_rank_match_process_snapshot(NULL, halos, 2) != 0, "a NULL context fails");

  release_run();
  const struct SnapshotContext after = {5, 0.0, 0.0, &MimicConfig};
  TEST_ASSERT(sham_rank_match_process_snapshot(&after, halos, 2) != 0,
              "process_snapshot fails after cleanup");
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_rank_mass_float_overflow
 * @brief  A scale that puts every rank mass above FLT_MAX initialises, then fails the snapshot
 *
 * The target table depends on M / Ms only, so ShamTargetLogMstar = 60 initialises. The top
 * rank's mass is then about 1e60 Msun, far above the float StellarMass limit, and the snapshot
 * callback must fail with the rank named and write nothing.
 */
int test_rank_mass_float_overflow(void) {
  TEST_ASSERT_EQUAL(init_run("ShamTargetLogMstar", "60"), 0,
                    "init succeeds at ShamTargetLogMstar = 60: the table is scale invariant");
  for (int64_t i = 0; i < 3; i++) {
    set_ranked_entry(i, (int)(i % 2), 900 + i, (float)(300 - 10 * i), 0);
    galaxies[i].StellarMass = 3.0f; /* stale: a failed snapshot must leave it alone */
    galaxies[i].ShamGhost = 0;
  }
  save_population(3);
  const size_t utility_before = memory_category_bytes(MEM_UTILITY);
  capture_log();
  TEST_ASSERT(rank_population(3) != 0, "a rank mass above FLT_MAX fails the snapshot");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "ERROR") != NULL && strstr(log, "rank 0 (UniqueGalaxyID 900") != NULL &&
                  strstr(log, "outside the float StellarMass range") != NULL,
              "the error names the rank, the UniqueGalaxyID and the float limit");
  TEST_ASSERT(population_unchanged(3), "a failed snapshot writes no StellarMass or ShamGhost");
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == utility_before,
              "a failed snapshot releases its scratch");

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_empty_and_all_ineligible_populations
 * @brief  Both succeed, assign nothing, audit zero candidates and leave no net allocation
 */
int test_empty_and_all_ineligible_populations(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");
  const size_t utility_before = memory_category_bytes(MEM_UTILITY);

  capture_log();
  TEST_ASSERT_EQUAL(rank_population(0), 0, "an empty population succeeds");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "SHAM audit z=0.0000 candidates=0 assigned=0 masked=0") != NULL,
              "an empty population still audits");

  for (int64_t i = 0; i < 5; i++) {
    set_ranked_entry(i, (int)(i % 2), 60 + i, 79.9f, 0);
  }
  save_population(5);
  capture_log();
  TEST_ASSERT_EQUAL(rank_population(5), 0, "an all-ineligible population succeeds");
  log = captured_log();
  TEST_ASSERT(strstr(log, "SHAM audit z=0.0000 candidates=0 assigned=0 masked=0") != NULL,
              "it audits zero candidates");
  TEST_ASSERT(population_unchanged(5), "and writes nothing");
  TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == utility_before,
              "neither leaves a net MEM_UTILITY allocation");

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_non_output_snapshot_is_silent
 * @brief  A snapshot outside output.snapshot_list returns 0 without ranking, writing or logging
 */
int test_non_output_snapshot_is_silent(void) {
  configure_run(NULL, NULL);
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 5;
  MimicConfig.ZZ[5] = 0.0005;
  TEST_ASSERT_EQUAL(sham_rank_match_init(), 0, "init succeeds");

  set_ranked_entry(0, 0, 10, 300.0f, 0);
  set_ranked_entry(1, 2, 20, 200.0f, 0); /* would fail validation if it were ranked */
  save_population(2);
  const struct SnapshotContext ctx = {4, 0.02, 0.0, &MimicConfig};
  capture_log();
  TEST_ASSERT_EQUAL(sham_rank_match_process_snapshot(&ctx, halos, 2), 0,
                    "a non-output snapshot returns 0");
  const char *log = captured_log();
  TEST_ASSERT(strstr(log, "SHAM audit") == NULL, "and logs nothing");
  TEST_ASSERT(population_unchanged(2), "and writes nothing");

  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test   test_no_allocation_growth_across_snapshots
 * @brief  Repeated snapshots leave net MEM_UTILITY bytes exactly where they started
 */
int test_no_allocation_growth_across_snapshots(void) {
  TEST_ASSERT_EQUAL(init_run(NULL, NULL), 0, "init succeeds");
  const size_t utility_before = memory_category_bytes(MEM_UTILITY);
  for (int snap = 0; snap < 64; snap++) {
    const int64_t n = (snap % 4 == 0) ? 0 : 1 + (snap * 97) % (MAX_POPULATION - 1);
    for (int64_t i = 0; i < n; i++) {
      set_ranked_entry(i, (int)(i % 2), 1 + i, (float)(1 + (i * 31) % 400), 0);
    }
    TEST_ASSERT_EQUAL(rank_population(n), 0, "every snapshot succeeds");
    TEST_ASSERT(memory_category_bytes(MEM_UTILITY) == utility_before,
                "no MEM_UTILITY allocation may be retained or accumulate across snapshots");
  }
  release_run();
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * REGISTERED DISPATCH
 * ============================================================================ */

/**
 * @test   test_registered_dispatch
 * @brief  The generated registration binds both callbacks; core dispatches each in its phase
 *
 * One FoF step through execute_module_pipeline() over a hand-built workspace
 * (a Type 0 host, a Type 1 subhalo and a carried Type 2 orphan), then the
 * snapshot callback through execute_post_snapshot() over the surviving rows.
 */
int test_registered_dispatch(void) {
  configure_run(NULL, NULL);
  TEST_ASSERT_EQUAL(module_system_init(), 0, "module_system_init succeeds");

  struct GalaxyPool *pool = galaxy_pool_create(8);
  struct FoFWorkspace workspace;
  memset(&workspace, 0, sizeof(workspace));
  workspace.halos = mymalloc_cat(3 * sizeof(struct Halo), MEM_HALOS);
  memset(workspace.halos, 0, 3 * sizeof(struct Halo));
  workspace.count = 3;
  workspace.capacity = 3;
  workspace.base_count = 3;
  workspace.pool = pool;
  workspace.identity =
      (struct RecordIdentitySpace){.unit = 5, .rows_per_unit = 10, .fits = true, .units = 6};
  static const int types[3] = {0, 1, 2};
  static const float vmax[3] = {250.0f, 120.0f, 400.0f};
  for (int i = 0; i < 3; i++) {
    struct Halo *h = &workspace.halos[i];
    h->Type = types[i];
    h->UniqueGalaxyID = 100 + i;
    h->Vmax = vmax[i];
    h->Mvir = 10.0 + i;
    h->galaxy = galaxy_pool_alloc(pool);
    init_galaxy_defaults(h->galaxy);
  }
  struct ModuleContext context;
  memset(&context, 0, sizeof(context));
  context.snapshot_number = 5;
  context.num_substeps = 1;
  context.central_index = 0;
  context.central_galaxy = &workspace.halos[0];
  context.params = &MimicConfig;

  execute_module_pipeline(&context, &workspace);
  TEST_ASSERT_EQUAL(workspace.halos[2].Type, 3, "the dispatched FoF step retires the orphan");
  TEST_ASSERT(workspace.halos[0].galaxy->ShamVpeak == 250.0f &&
                  workspace.halos[1].galaxy->ShamVpeak == 120.0f,
              "the dispatched FoF step records the peaks");

  /* The snapshot population is the two surviving rows (marshal drops Type 3). */
  struct SnapshotContext ctx = {5, 0.0005, 0.0, &MimicConfig};
  execute_post_snapshot(&ctx, workspace.halos, 2); /* fatal on a non-zero return */
  TEST_ASSERT(workspace.halos[0].galaxy->ShamGhost == 0 &&
                  workspace.halos[1].galaxy->ShamGhost == 0,
              "the dispatched snapshot callback assigns both candidates");
  TEST_ASSERT(float_bits(workspace.halos[0].galaxy->StellarMass) ==
                  float_bits(expected_rank_mass(0)),
              "the host takes rank 0");
  TEST_ASSERT(fabs(log10_physical_mass(workspace.halos[0].galaxy->StellarMass) -
                   reference_cases[0].log_mass) <= LOG_MASS_TOLERANCE,
              "the fixture's rank 0 mass matches the reference");
  TEST_ASSERT(float_bits(workspace.halos[1].galaxy->StellarMass) ==
                  float_bits(expected_rank_mass(1)),
              "the subhalo takes rank 1");
  execute_post_snapshot(&ctx, NULL, 0);

  TEST_ASSERT_EQUAL(module_system_cleanup(), 0, "module_system_cleanup succeeds");
  fof_workspace_destroy(&workspace);
  galaxy_pool_destroy(pool);
  check_memory_leaks();
  return TEST_PASS;
}

/* ============================================================================
 * MAIN
 * ============================================================================ */

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sham_rank_match (target, inversion, peaks, rank match)\n");
  printf("============================================================\n");
  printf("%s", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_converted_target_values);
  TEST_RUN(test_reference_table);
  TEST_RUN(test_init_configuration);
  TEST_RUN(test_init_parameter_domain);
  TEST_RUN(test_init_redshift_window);
  TEST_RUN(test_process_retires_type2_and_updates_peaks);
  TEST_RUN(test_process_peak_float_bound);
  TEST_RUN(test_process_input_validation);
  TEST_RUN(test_tie_order_is_hand_solvable);
  TEST_RUN(test_permutation_and_repeat_identity);
  TEST_RUN(test_rank_is_global_across_fofs);
  TEST_RUN(test_masking_from_the_floor_down);
  TEST_RUN(test_peak_persists_through_masked_snapshot);
  TEST_RUN(test_snapshot_validation_without_writes);
  TEST_RUN(test_rank_mass_float_overflow);
  TEST_RUN(test_empty_and_all_ineligible_populations);
  TEST_RUN(test_non_output_snapshot_is_silent);
  TEST_RUN(test_no_allocation_growth_across_snapshots);
  TEST_RUN(test_registered_dispatch);

  TEST_SUMMARY();
  return TEST_RESULT();
}
