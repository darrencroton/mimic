/**
 * @file    test_unit_sage26_determine_ffb_regime.c
 * @brief   Platform-independent behavioural tests of sage26_determine_ffb_regime
 *
 * The oracle test (test_unit_sage26_determine_ffb_regime_oracle.c) skips off macOS arm64, so
 * these tests are the classifier's guard on every other platform. Each runs the module's
 * process(), through its test seam (sage26_determine_ffb_regime.h) where a test must place the
 * compared deviate exactly or see which rows were classified. Every expectation is derived
 * independently of the module: literal inputs and the halo_uniform deviates of the c13 plan's
 * frozen vectors (an independent transcription's, which SAGE26's own values from the oracle
 * confirm), or SAGE26's own expressions written here with their operand order
 * (R/src/model_regimes.c:309-312 for the threshold, :332-339 for the fraction, :236 for the
 * comparison), never by calling the module or its shared helpers. R/ is the staged SAGE26
 * source root.
 *
 * Software quality:
 * - test_module_lifecycle: registration, module_system_init with the module in pre_timestep,
 *   cleanup, no leaks
 * - test_enhanced_sf_missing_refused, test_enhanced_sf_values: the required
 *   EnhancedStarFormationOn (campaign decision D33): missing fails init naming it; 2, 3 and -1
 *   are refused naming it; 0 and 1 are accepted
 * - test_placement_refused: a substep phase, post_timestep, twice in pre_timestep,
 *   process_by_galaxy, before sage26_prepare_infall_budget and before sage26_determine_regime are
 *   each refused, naming the module or the predecessor; after both, and alone, are accepted
 * - test_requires_transaction (c13 Slice 3): at EnhancedStarFormationOn 1 init refuses, naming
 *   sage26_ffb_star_formation, a configuration without the transaction in a substep phase as
 *   process_by_galaxy (absent, or there as process_full_halo); at 0 it accepts both
 *
 * Every test that initialises the module configures sage26_ffb_star_formation by galaxy in a
 * substep phase, as the cross-module guard requires at EnhancedStarFormationOn 1.
 *
 * Behaviour:
 * - test_off_writes_nothing: EnhancedStarFormationOn 0 writes nothing and classifies no row
 * - test_types_classified_type3_kept: Types 0, 1 and 2 are classified (no Type restriction); a
 *   Type 3 row and a galaxy-less row keep a seeded FFBRegime; an input FFBRegime is overwritten
 * - test_cgm_regime_ignored: rows identical but for their CGM Regime (-1, 0, 1) get the same
 *   FFBRegime from the production draw
 * - test_reads_narrowed_mvir: the fraction is SAGE26's of the float Mvir, not the core's double
 * - test_reads_current_redshift: the fraction uses ctx->redshift, not the redshift of the row's
 *   stored snapshot
 * - test_draw_keys: the production draw is keyed on MostBoundID and ctx->snapshot_number, not on
 *   UniqueGalaxyID or the row's stored SnapNum
 * - test_seam_tie: a deviate equal to the fraction gives 0, one ulp below gives 1 (the strict
 *   comparison, R/src/model_regimes.c:236)
 * - test_initial_boundary_row: a row in the initial-boundary state (stored SnapNum -1, dT 0, D37)
 *   is classified without error
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
#include <stdlib.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/sage26_determine_ffb_regime/sage26_determine_ffb_regime.h"

extern int sage26_determine_ffb_regime_init(void);
extern int sage26_determine_ffb_regime_process(struct ModuleContext *ctx, struct Halo *halos,
                                               int ngal);
extern int sage26_determine_ffb_regime_cleanup(void);

#define MAX_ROWS 8

/* R6's Hubble_h (R6.par:40) */
#define TEST_HUBBLE_H 0.73

/* halo_uniform deviates (R/src/model_regimes.c:151-159) as f64 bit patterns: the c13 plan's
 * frozen vectors, and (4896973, 11) from the same independent transcription */
#define DEVIATE_ID0_SNAP0 0x3fec4415072f63baULL      /* (0, 0): ~0.8833 */
#define DEVIATE_ID1_SNAP0 0x3fe22145bd91204cULL      /* (1, 0): ~0.5666 */
#define DEVIATE_4896973_SNAP12 0x3fdc862dc2b4faddULL /* ~0.4457 */
#define DEVIATE_4896973_SNAP11 0x3fe93c46ec358b7eULL /* ~0.7886 */

// ============================================================================
// FIXTURES
// ============================================================================

static char module_name[] = "sage26_determine_ffb_regime";
static char budget_name[] = "sage26_prepare_infall_budget";
static char cgm_name[] = "sage26_determine_regime";
static char phase_name[] = "galaxy_physics";
static struct PhaseModuleConfig pre_timestep_entries[3];
static struct PhaseModuleConfig post_timestep_entries[1];
static char transaction_name[] = "sage26_ffb_star_formation";
static struct PhaseModuleConfig substep_entries[1];
static struct ModulePhaseConfig substep_phase;

/** @brief A double from its IEEE-754 bit pattern */
static double from_bits(const uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/** @brief Bit pattern of a double, for exact comparisons */
static uint64_t bits_of(const double v) {
  uint64_t b;
  memcpy(&b, &v, sizeof(b));
  return b;
}

/** @brief Read all content from a FILE* into a static buffer (valid until the next call) */
static const char *read_captured_log(FILE *fp) {
  static char buf[8192];
  rewind(fp);
  const size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
  buf[n] = '\0';
  return buf;
}

/** @brief Set pre_timestep to the given entries, each process_full_halo */
static void set_pre_timestep(char *const *names, const int count) {
  for (int i = 0; i < count; i++) {
    pre_timestep_entries[i].module_name = names[i];
    pre_timestep_entries[i].processing_mode = PROCESSING_MODE_FULL_HALO;
    pre_timestep_entries[i].resolved = NULL;
  }
  MimicConfig.pre_timestep = count > 0 ? pre_timestep_entries : NULL;
  MimicConfig.num_pre_timestep = count;
}

/**
 * @brief Configure the module alone in pre_timestep with an EnhancedStarFormationOn text (NULL:
 *        missing), then let `arrange` change the placement, and initialise, capturing the log
 * @return The init status
 */
static int init_captured(const char *value, void (*arrange)(void), const char **log) {
  reset_config();
  MimicConfig.Hubble_h = TEST_HUBBLE_H;
  char *alone[1] = {module_name};
  set_pre_timestep(alone, 1);
  /* The FFB transaction by galaxy in a substep phase (the guard of EnhancedStarFormationOn 1) */
  substep_entries[0].module_name = transaction_name;
  substep_entries[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
  substep_entries[0].resolved = NULL;
  substep_phase.name = phase_name;
  substep_phase.modules = substep_entries;
  substep_phase.num_modules = 1;
  MimicConfig.substep_phases = &substep_phase;
  MimicConfig.num_substep_phases = 1;
  if (value != NULL) {
    sage26_test_append_model_parameter("EnhancedStarFormationOn", value);
  }
  if (arrange != NULL) {
    arrange();
  }
  FILE *capture = tmpfile();
  FILE *old_out = capture != NULL ? set_log_output(capture) : NULL;
  const int status = sage26_determine_ffb_regime_init();
  if (capture != NULL) {
    set_log_output(old_out);
    *log = read_captured_log(capture);
    fclose(capture);
  } else {
    *log = "";
  }
  return status;
}

/** @brief Initialise with EnhancedStarFormationOn `value`, the module alone in pre_timestep */
static int init_ok(const char *value) {
  const char *log = "";
  return init_captured(value, NULL, &log);
}

/** What the seam saw, and the deviates it hands out per row (NAN: keep SAGE26's draw) */
struct recording_seam {
  double deviate_for_row[MAX_ROWS];
  int observed;
  int observed_row[MAX_ROWS];
  struct Sage26FfbRowTrace trace[MAX_ROWS];
};

static double recording_deviate(void *user, const struct Sage26FfbRowTrace *row) {
  const struct recording_seam *s = user;
  if (row->row < 0 || row->row >= MAX_ROWS || isnan(s->deviate_for_row[row->row])) {
    return row->draw;
  }
  return s->deviate_for_row[row->row];
}

static void recording_observe(void *user, const struct Sage26FfbRowTrace *row) {
  struct recording_seam *s = user;
  if (row->row >= 0 && row->row < MAX_ROWS) {
    s->trace[row->row] = *row;
    s->observed_row[row->row] = 1;
  }
  s->observed++;
}

/** @brief A seam that keeps every production draw and only observes */
static void observe_only(struct recording_seam *s) {
  memset(s, 0, sizeof(*s));
  for (int i = 0; i < MAX_ROWS; i++) {
    s->deviate_for_row[i] = NAN;
  }
}

/** @brief A row with a galaxy: Type, core Mvir, MostBoundID, UniqueGalaxyID and FFBRegime */
static void make_row(struct Halo *halo, struct GalaxyData *gal, const int type, const double mvir,
                     const long long most_bound_id, const int ffb_regime) {
  memset(halo, 0, sizeof(*halo));
  memset(gal, 0, sizeof(*gal));
  halo->galaxy = gal;
  halo->Type = type;
  halo->Mvir = mvir;
  halo->MostBoundID = most_bound_id;
  halo->UniqueGalaxyID = 1000000000123LL;
  halo->SnapNum = 11;
  halo->dT = 100.0;
  gal->FFBRegime = ffb_regime;
  gal->Regime = 1;
}

/** @brief Run process() at (snapshot, redshift) with the seam installed (or NULL for none) */
static int run_process(struct Halo *halos, const int ngal, struct recording_seam *s,
                       const int snapshot, const double redshift) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.snapshot_number = snapshot;
  ctx.redshift = redshift;
  ctx.num_substeps = 1;
  ctx.central_galaxy = &halos[0];
  if (s != NULL) {
    const struct Sage26FfbSeam seam = {recording_deviate, recording_observe, s};
    sage26_determine_ffb_regime_set_test_seam(&seam);
  }
  const int status = sage26_determine_ffb_regime_process(&ctx, halos, ngal);
  sage26_determine_ffb_regime_set_test_seam(NULL);
  return status;
}

/**
 * @brief SAGE26's FFB fraction of a float Mvir, written as SAGE26 writes it
 *
 * calculate_ffb_threshold_mass (R/src/model_regimes.c:308-312) with the log term fused as
 * SAGE26's object computes it (model_regimes.o 0x908: fmadd of log10(z_norm), -6.2 and
 * log10(h) + 0.8), then calculate_ffb_fraction (:330-339) on the widened float. The inputs pass
 * through volatile locals and the helper is never inlined, so the compiler cannot fold log10,
 * exp or pow at compile time with a different rounding than the runtime libm.
 */
#if defined(__GNUC__) || defined(__clang__)
#define SAGE26_FFB_TEST_NOINLINE __attribute__((noinline))
#else
#define SAGE26_FFB_TEST_NOINLINE
#endif
static SAGE26_FFB_TEST_NOINLINE double sage26_ffb_fraction(const float mvir_in, const double z_in,
                                                           const double hubble_h_in) {
  volatile float mvir_runtime = mvir_in;
  volatile double z_runtime = z_in;
  volatile double hubble_h_runtime = hubble_h_in;
  const double Mvir = mvir_runtime;
  const double z = z_runtime;
  const double h = hubble_h_runtime;

  const double z_norm = (1.0 + z) / 10.0;
  const double log_Mvir_ffb_code = fma(-6.2, log10(z_norm), 0.8 + log10(h));
  const double Mvir_ffb = pow(10.0, log_Mvir_ffb_code);
  if (Mvir <= 0.0 || Mvir_ffb <= 0.0) {
    return 0.0;
  }
  const double delta_log_M = 0.15;
  const double x = log10(Mvir / Mvir_ffb) / delta_log_M;
  return 1.0 / (1.0 + exp(-x));
}

/** @brief The float Mvir at (log10 offset) dex from the threshold at z, h 0.73 */
static SAGE26_FFB_TEST_NOINLINE float mvir_at_offset(const double z, const double dex) {
  volatile double z_runtime = z;
  const double z_norm = (1.0 + z_runtime) / 10.0;
  const double threshold = pow(10.0, fma(-6.2, log10(z_norm), 0.8 + log10(TEST_HUBBLE_H)));
  return (float)(threshold * pow(10.0, dex));
}

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

/**
 * @test    test_module_lifecycle
 * @brief   The module registers, initialises through module_system_init in pre_timestep with
 *          EnhancedStarFormationOn 0 (SAGE26's R0-R5 value), cleans up, no leaks
 */
int test_module_lifecycle(void) {
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();
  MimicConfig.Hubble_h = TEST_HUBBLE_H;
  test_pre_timestep_add("sage26_determine_ffb_regime", PROCESSING_MODE_FULL_HALO);
  set_test_model_parameters();
  sage26_test_append_model_parameter("EnhancedStarFormationOn", "0");

  const int result = module_system_init();
  TEST_ASSERT(result == 0, "module system initialisation must succeed");

  module_system_cleanup();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_enhanced_sf_missing_refused
 * @brief   Init fails without EnhancedStarFormationOn, naming it: required, no default (D33)
 */
int test_enhanced_sf_missing_refused(void) {
  const char *log = "";
  const int status = init_captured(NULL, NULL, &log);
  printf("\n  without EnhancedStarFormationOn: init %d", status);
  TEST_ASSERT(status != 0, "init must fail when EnhancedStarFormationOn is missing");
  TEST_ASSERT(strstr(log, "EnhancedStarFormationOn") != NULL,
              "the error must name EnhancedStarFormationOn");
  return TEST_PASS;
}

/**
 * @test    test_enhanced_sf_values
 * @brief   EnhancedStarFormationOn 2 (BK25, not ported), 3 and -1 are refused naming the
 *          parameter; 0 and 1 initialise
 */
int test_enhanced_sf_values(void) {
  const char *refused[] = {"2", "3", "-1"};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    const char *log = "";
    const int status = init_captured(refused[k], NULL, &log);
    printf("\n  EnhancedStarFormationOn %s: init %d", refused[k], status);
    TEST_ASSERT(status != 0, "init must refuse an unsupported EnhancedStarFormationOn");
    TEST_ASSERT(strstr(log, "EnhancedStarFormationOn") != NULL,
                "the error must name EnhancedStarFormationOn");
  }
  TEST_ASSERT(init_ok("0") == 0, "init must accept EnhancedStarFormationOn 0");
  TEST_ASSERT(init_ok("1") == 0, "init must accept EnhancedStarFormationOn 1");
  return TEST_PASS;
}

/* The placements, each arranged after init_captured() puts the module alone in pre_timestep */
static void arrange_substep_only(void) {
  MimicConfig.num_pre_timestep = 0;
  substep_entries[0].module_name = module_name;
  substep_entries[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  substep_entries[0].resolved = NULL;
  substep_phase.name = phase_name;
  substep_phase.modules = substep_entries;
  substep_phase.num_modules = 1;
  MimicConfig.substep_phases = &substep_phase;
  MimicConfig.num_substep_phases = 1;
}
static void arrange_post_timestep_only(void) {
  MimicConfig.num_pre_timestep = 0;
  post_timestep_entries[0] = pre_timestep_entries[0];
  MimicConfig.post_timestep = post_timestep_entries;
  MimicConfig.num_post_timestep = 1;
}
static void arrange_twice(void) {
  char *names[2] = {module_name, module_name};
  set_pre_timestep(names, 2);
}
static void arrange_by_galaxy(void) {
  pre_timestep_entries[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
}
static void arrange_before_budget(void) {
  char *names[3] = {cgm_name, module_name, budget_name};
  set_pre_timestep(names, 3);
}
static void arrange_before_cgm_classifier(void) {
  char *names[2] = {module_name, cgm_name};
  set_pre_timestep(names, 2);
}
static void arrange_after_both(void) {
  char *names[3] = {cgm_name, budget_name, module_name};
  set_pre_timestep(names, 3);
}
static void arrange_after_budget(void) {
  char *names[2] = {budget_name, module_name};
  set_pre_timestep(names, 2);
}

/**
 * @test    test_placement_refused
 * @brief   Only one pre_timestep process_full_halo entry, after sage26_determine_regime and
 *          sage26_prepare_infall_budget where configured, is accepted; each refusal names what
 *          it refuses
 */
int test_placement_refused(void) {
  const struct {
    const char *label;
    void (*arrange)(void);
    const char *named;
  } refused[] = {
      {"a substep phase only", arrange_substep_only, "substep phase"},
      {"post_timestep only", arrange_post_timestep_only, "post_timestep"},
      {"twice in pre_timestep", arrange_twice, "exactly once"},
      {"process_by_galaxy in pre_timestep", arrange_by_galaxy, "process_full_halo"},
      {"before sage26_prepare_infall_budget", arrange_before_budget, budget_name},
      {"before sage26_determine_regime", arrange_before_cgm_classifier, cgm_name},
  };
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    for (int on = 0; on <= 1; on++) {
      const char *log = "";
      const int status = init_captured(on ? "1" : "0", refused[k].arrange, &log);
      printf("\n  %s (EnhancedStarFormationOn %d): init %d", refused[k].label, on, status);
      TEST_ASSERT(status != 0, "init must refuse the misplacement");
      TEST_ASSERT(strstr(log, module_name) != NULL, "the error must name the module");
      TEST_ASSERT(strstr(log, refused[k].named) != NULL,
                  "the error must name what it refuses (the phase, count, mode or predecessor)");
    }
  }
  const char *log = "";
  TEST_ASSERT(init_captured("1", arrange_after_both, &log) == 0,
              "init must accept the module after sage26_determine_regime and the budget");
  TEST_ASSERT(init_captured("1", arrange_after_budget, &log) == 0,
              "init must accept the module after the budget");
  TEST_ASSERT(init_captured("1", NULL, &log) == 0, "init must accept the module alone");
  return TEST_PASS;
}

static void arrange_no_transaction(void) { MimicConfig.num_substep_phases = 0; }
static void arrange_transaction_full_halo(void) {
  substep_entries[0].processing_mode = PROCESSING_MODE_FULL_HALO;
}

/**
 * @test    test_requires_transaction
 * @brief   At EnhancedStarFormationOn 1 init refuses, naming sage26_ffb_star_formation, a
 *          configuration without the transaction by galaxy in a substep phase: an FFBRegime 1 row
 *          bypasses the ordinary star-formation chain (R/src/model_starformation_and_feedback.c
 *          :1004-1008) and would form no stars; at 0 nothing is classified and both are accepted
 *          (c13 Slice 3)
 */
int test_requires_transaction(void) {
  const struct {
    const char *label;
    void (*arrange)(void);
  } refused[] = {{"no transaction", arrange_no_transaction},
                 {"the transaction as process_full_halo", arrange_transaction_full_halo}};
  for (size_t k = 0; k < sizeof(refused) / sizeof(refused[0]); k++) {
    const char *log = "";
    int status = init_captured("1", refused[k].arrange, &log);
    printf("\n  %s (EnhancedStarFormationOn 1): init %d", refused[k].label, status);
    TEST_ASSERT(status != 0, "init must refuse a configuration without the FFB transaction");
    TEST_ASSERT(strstr(log, transaction_name) != NULL,
                "the error must name sage26_ffb_star_formation");
    status = init_captured("0", refused[k].arrange, &log);
    printf("; at 0: init %d", status);
    TEST_ASSERT(status == 0, "at EnhancedStarFormationOn 0 the guard does not apply");
  }
  const char *log = "";
  TEST_ASSERT(init_captured("1", NULL, &log) == 0,
              "init accepts the transaction by galaxy in a substep phase");
  return TEST_PASS;
}

// ============================================================================
// BEHAVIOURAL TESTS
// ============================================================================

/**
 * @test    test_off_writes_nothing
 * @brief   EnhancedStarFormationOn 0 writes nothing and classifies no row: SAGE26 skips the call
 *          (R/src/core_build_model.c:425)
 */
int test_off_writes_nothing(void) {
  TEST_ASSERT(init_ok("0") == 0, "init must succeed");
  const double z = 7.0;
  struct Halo halos[4];
  struct GalaxyData gals[4];
  make_row(&halos[0], &gals[0], 0, (double)mvir_at_offset(z, 1.0), 0, 0);
  make_row(&halos[1], &gals[1], 1, (double)mvir_at_offset(z, 1.0), 1, 1);
  make_row(&halos[2], &gals[2], 2, 0.0, 2, 1);
  make_row(&halos[3], &gals[3], 0, INFINITY, 3, 0);
  struct Halo halos_before[4];
  struct GalaxyData gals_before[4];
  memcpy(halos_before, halos, sizeof(halos));
  memcpy(gals_before, gals, sizeof(gals));

  struct recording_seam s;
  observe_only(&s);
  const int status = run_process(halos, 4, &s, 12, z);
  printf("\n  EnhancedStarFormationOn 0: status %d, rows classified %d", status, s.observed);
  TEST_ASSERT(status == 0, "process must succeed");
  TEST_ASSERT(s.observed == 0, "EnhancedStarFormationOn 0 must classify no row");
  TEST_ASSERT(memcmp(gals, gals_before, sizeof(gals)) == 0,
              "EnhancedStarFormationOn 0 must write nothing");
  TEST_ASSERT(memcmp(halos, halos_before, sizeof(halos)) == 0,
              "EnhancedStarFormationOn 0 must leave the rows unchanged");
  return TEST_PASS;
}

/**
 * @test    test_types_classified_type3_kept
 * @brief   Types 0, 1 and 2 are each classified, with no Type restriction (R/src/model_regimes.c
 *          :212-240); Type 3 (SAGE26's mergeType > 0, :213) and galaxy-less rows keep a seeded
 *          FFBRegime; an input FFBRegime is overwritten
 */
int test_types_classified_type3_kept(void) {
  TEST_ASSERT(init_ok("1") == 0, "init must succeed");
  const double z = 7.0;
  const float m = mvir_at_offset(z, 0.0);
  const double f = sage26_ffb_fraction(m, z, TEST_HUBBLE_H);
  struct Halo halos[6];
  struct GalaxyData gals[6];
  make_row(&halos[0], &gals[0], 0, (double)m, 10, 0); /* deviate below f -> 1 */
  make_row(&halos[1], &gals[1], 1, (double)m, 11, 0); /* satellite, deviate below f -> 1 */
  make_row(&halos[2], &gals[2], 2, (double)m, 12, 0); /* Type 2 with a mass, below f -> 1 */
  make_row(&halos[3], &gals[3], 2, 0.0, 13, 1);       /* orphan, Mvir 0: f 0 -> 0 */
  make_row(&halos[4], &gals[4], 3, (double)m, 14, 7); /* merged: skipped, keeps 7 */
  make_row(&halos[5], &gals[5], 0, (double)m, 15, 5); /* no galaxy: skipped */
  halos[5].galaxy = NULL;

  struct recording_seam s;
  observe_only(&s);
  for (int i = 0; i < 5; i++) {
    s.deviate_for_row[i] = 0.0; /* below every positive fraction */
  }
  TEST_ASSERT(run_process(halos, 6, &s, 12, z) == 0, "process must succeed");
  printf("\n  f %.17g; FFBRegime %d %d %d %d %d %d; rows classified %d", f, gals[0].FFBRegime,
         gals[1].FFBRegime, gals[2].FFBRegime, gals[3].FFBRegime, gals[4].FFBRegime,
         gals[5].FFBRegime, s.observed);
  TEST_ASSERT(f > 0.0, "the chosen mass must have a positive fraction");
  TEST_ASSERT(gals[0].FFBRegime == 1 && gals[1].FFBRegime == 1 && gals[2].FFBRegime == 1,
              "Types 0, 1 and 2 with a positive fraction and a lower deviate must be FFB");
  TEST_ASSERT(gals[3].FFBRegime == 0, "an orphan with Mvir 0 must be overwritten with 0");
  TEST_ASSERT(gals[4].FFBRegime == 7 && gals[5].FFBRegime == 5,
              "the Type 3 and galaxy-less rows must keep their FFBRegime");
  TEST_ASSERT(s.observed == 4 && !s.observed_row[4] && !s.observed_row[5],
              "exactly the four rows that are not Type 3 and have a galaxy must be classified");
  return TEST_PASS;
}

/**
 * @test    test_cgm_regime_ignored
 * @brief   Rows that differ only in their CGM Regime (-1, 0 and 1) get the same FFBRegime from the
 *          production draw (R/src/model_regimes.c:215-217: no regime restriction)
 */
int test_cgm_regime_ignored(void) {
  TEST_ASSERT(init_ok("1") == 0, "init must succeed");
  const double z = 7.0;
  /* At snapshot 12 MostBoundID 4896973 draws ~0.4457; a mass 0.05 dex above the threshold has a
   * fraction ~0.68 above it, and one 0.05 dex below ~0.32 below it */
  const double draw = from_bits(DEVIATE_4896973_SNAP12);
  const float m_hi = mvir_at_offset(z, 0.05);
  const float m_lo = mvir_at_offset(z, -0.05);
  const double f_hi = sage26_ffb_fraction(m_hi, z, TEST_HUBBLE_H);
  const double f_lo = sage26_ffb_fraction(m_lo, z, TEST_HUBBLE_H);
  TEST_ASSERT(draw < f_hi && !(draw < f_lo), "the two masses must straddle the draw");
  const float masses[2] = {m_hi, m_lo};
  const int expected[2] = {1, 0};
  for (int k = 0; k < 2; k++) {
    struct Halo halos[3];
    struct GalaxyData gals[3];
    const int regimes[3] = {-1, 0, 1};
    for (int i = 0; i < 3; i++) {
      make_row(&halos[i], &gals[i], 0, (double)masses[k], 4896973, 1 - expected[k]);
      gals[i].Regime = regimes[i];
    }
    TEST_ASSERT(run_process(halos, 3, NULL, 12, z) == 0, "process must succeed");
    printf("\n  f %.6g against the draw %.6g: FFBRegime %d %d %d for Regime -1, 0, 1",
           k == 0 ? f_hi : f_lo, draw, gals[0].FFBRegime, gals[1].FFBRegime, gals[2].FFBRegime);
    for (int i = 0; i < 3; i++) {
      TEST_ASSERT(gals[i].FFBRegime == expected[k],
                  "every CGM Regime must give the FFBRegime of the draw against the fraction");
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_reads_narrowed_mvir
 * @brief   The fraction is of the float Mvir SAGE26 holds (R/src/model_regimes.c:229), not the
 *          core's double: with the deviate placed exactly at the float's fraction the row ties
 *          (0), where the double's larger fraction would give 1
 */
int test_reads_narrowed_mvir(void) {
  TEST_ASSERT(init_ok("1") == 0, "init must succeed");
  const double z = 7.0;
  const float m = mvir_at_offset(z, 0.0);
  /* A double a third of a float step above the float: it narrows back to m */
  const double mvir_double = (double)m + (double)(nextafterf(m, INFINITY) - m) / 3.0;
  const double f_float = sage26_ffb_fraction(m, z, TEST_HUBBLE_H);
  TEST_ASSERT((float)mvir_double == m && mvir_double > (double)m,
              "the double must lie above the float it narrows to");

  struct Halo halos[1];
  struct GalaxyData gals[1];
  make_row(&halos[0], &gals[0], 0, mvir_double, 21, 1);
  struct recording_seam s;
  observe_only(&s);
  s.deviate_for_row[0] = f_float;
  TEST_ASSERT(run_process(halos, 1, &s, 12, z) == 0, "process must succeed");
  printf("\n  float fraction %a, module fraction %a, FFBRegime %d", f_float, s.trace[0].f_ffb,
         gals[0].FFBRegime);
  TEST_ASSERT(bits_of(s.trace[0].mvir) == bits_of((double)m),
              "the classifier must read the widened float Mvir");
  TEST_ASSERT(bits_of(s.trace[0].f_ffb) == bits_of(f_float),
              "the fraction must be SAGE26's of the float Mvir");
  TEST_ASSERT(gals[0].FFBRegime == 0, "a deviate equal to the float's fraction must give 0");
  return TEST_PASS;
}

/**
 * @test    test_reads_current_redshift
 * @brief   The fraction is at ctx->redshift (SAGE26's Zcurr, R/src/core_build_model.c:408), not
 *          at the redshift of the row's stored snapshot: with the deviate at the current
 *          redshift's fraction the row ties (0), where the stored snapshot's higher redshift
 *          (a lower threshold, a larger fraction) would give 1
 */
int test_reads_current_redshift(void) {
  TEST_ASSERT(init_ok("1") == 0, "init must succeed");
  const double z_current = 7.0;
  const double z_stored = 8.0;
  MimicConfig.ZZ[11] = z_stored;
  MimicConfig.ZZ[12] = z_current;
  const float m = mvir_at_offset(z_current, 0.0);
  const double f_current = sage26_ffb_fraction(m, z_current, TEST_HUBBLE_H);
  const double f_stored = sage26_ffb_fraction(m, z_stored, TEST_HUBBLE_H);
  TEST_ASSERT(f_stored > f_current, "the stored redshift must give a larger fraction");

  struct Halo halos[1];
  struct GalaxyData gals[1];
  make_row(&halos[0], &gals[0], 0, (double)m, 31, 1);
  halos[0].SnapNum = 11;
  struct recording_seam s;
  observe_only(&s);
  s.deviate_for_row[0] = f_current;
  TEST_ASSERT(run_process(halos, 1, &s, 12, z_current) == 0, "process must succeed");
  printf("\n  fraction at z %g: %.17g (at the stored z %g: %.17g); FFBRegime %d", z_current,
         f_current, z_stored, f_stored, gals[0].FFBRegime);
  TEST_ASSERT(bits_of(s.trace[0].redshift) == bits_of(z_current) &&
                  bits_of(s.trace[0].f_ffb) == bits_of(f_current),
              "the fraction must be at the current redshift");
  TEST_ASSERT(gals[0].FFBRegime == 0, "a deviate equal to the current fraction must give 0");
  return TEST_PASS;
}

/**
 * @test    test_draw_keys
 * @brief   The production draw is halo_uniform(MostBoundID, ctx->snapshot_number)
 *          (R/src/model_regimes.c:225, :151-159), not keyed on UniqueGalaxyID or the row's stored
 *          SnapNum: each pair of keys is placed either side of the row's fraction
 */
int test_draw_keys(void) {
  TEST_ASSERT(init_ok("1") == 0, "init must succeed");

  /* MostBoundID against UniqueGalaxyID: at snapshot 0, MostBoundID 0 draws ~0.8833 and
   * UniqueGalaxyID 1 would draw ~0.5666; a fraction ~0.68 (0.05 dex above the threshold) lies
   * between them, so the row must be 0 */
  const double z = 7.0;
  const float m = mvir_at_offset(z, 0.05);
  const double f = sage26_ffb_fraction(m, z, TEST_HUBBLE_H);
  const double draw_mbid = from_bits(DEVIATE_ID0_SNAP0);
  const double draw_ugid = from_bits(DEVIATE_ID1_SNAP0);
  TEST_ASSERT(draw_ugid < f && !(draw_mbid < f), "the fraction must separate the two keys");
  struct Halo halos[1];
  struct GalaxyData gals[1];
  make_row(&halos[0], &gals[0], 0, (double)m, 0, 1);
  halos[0].UniqueGalaxyID = 1;
  halos[0].SnapNum = 0;
  struct recording_seam s;
  observe_only(&s);
  TEST_ASSERT(run_process(halos, 1, &s, 0, z) == 0, "process must succeed");
  printf("\n  MostBoundID 0 / UniqueGalaxyID 1 at snapshot 0: draw %a, FFBRegime %d",
         s.trace[0].draw, gals[0].FFBRegime);
  TEST_ASSERT(bits_of(s.trace[0].draw) == DEVIATE_ID0_SNAP0,
              "the draw must be halo_uniform of MostBoundID 0 at snapshot 0");
  TEST_ASSERT(gals[0].FFBRegime == 0, "MostBoundID's draw above the fraction must give 0");

  /* The current snapshot against the stored SnapNum: MostBoundID 4896973 draws ~0.4457 at
   * snapshot 12 and would draw ~0.7886 at the stored 11; the same fraction ~0.68 lies between */
  const double draw_current = from_bits(DEVIATE_4896973_SNAP12);
  const double draw_stored = from_bits(DEVIATE_4896973_SNAP11);
  TEST_ASSERT(draw_current < f && !(draw_stored < f), "the fraction must separate the snapshots");
  make_row(&halos[0], &gals[0], 0, (double)m, 4896973, 0);
  halos[0].SnapNum = 11;
  halos[0].UniqueGalaxyID = 4896973;
  observe_only(&s);
  TEST_ASSERT(run_process(halos, 1, &s, 12, z) == 0, "process must succeed");
  printf("\n  MostBoundID 4896973 at snapshot 12 (stored 11): draw %a, FFBRegime %d",
         s.trace[0].draw, gals[0].FFBRegime);
  TEST_ASSERT(bits_of(s.trace[0].draw) == DEVIATE_4896973_SNAP12,
              "the draw must be halo_uniform at the current snapshot");
  TEST_ASSERT(gals[0].FFBRegime == 1, "the current snapshot's draw below the fraction must give 1");
  return TEST_PASS;
}

/**
 * @test    test_seam_tie
 * @brief   Through the seam, a deviate equal to the fraction gives 0 and one ulp below gives 1:
 *          SAGE26's comparison is strict (R/src/model_regimes.c:236). The tie is not constructible
 *          through SAGE26's hash.
 */
int test_seam_tie(void) {
  const double z = 7.0;
  const float m = mvir_at_offset(z, 0.02);
  const double f = sage26_ffb_fraction(m, z, TEST_HUBBLE_H);
  const double deviates[3] = {nextafter(f, 0.0), f, nextafter(f, 1.0)};
  const int expected[3] = {1, 0, 0};
  for (int d = 0; d < 3; d++) {
    TEST_ASSERT(init_ok("1") == 0, "init must succeed");
    struct Halo halos[1];
    struct GalaxyData gals[1];
    make_row(&halos[0], &gals[0], 0, (double)m, 41, 1 - expected[d]);
    struct recording_seam s;
    observe_only(&s);
    s.deviate_for_row[0] = deviates[d];
    TEST_ASSERT(run_process(halos, 1, &s, 12, z) == 0, "process must succeed");
    printf("\n  fraction %a, deviate %a -> FFBRegime %d", f, deviates[d], gals[0].FFBRegime);
    TEST_ASSERT(bits_of(s.trace[0].deviate) == bits_of(deviates[d]),
                "the seam's deviate must be the one compared");
    TEST_ASSERT(gals[0].FFBRegime == expected[d],
                "FFBRegime must be 1 exactly when the deviate is below the fraction");
  }
  return TEST_PASS;
}

/**
 * @test    test_initial_boundary_row
 * @brief   A row in the initial-boundary state (stored SnapNum -1, dT 0; campaign decision D37) is
 *          classified like any other: the classifier reads no snapshot-indexed table
 */
int test_initial_boundary_row(void) {
  TEST_ASSERT(init_ok("1") == 0, "init must succeed");
  const double z = 7.0;
  struct Halo halos[1];
  struct GalaxyData gals[1];
  make_row(&halos[0], &gals[0], 0, INFINITY, 51, 0); /* fraction exactly 1 */
  halos[0].SnapNum = -1;
  halos[0].dT = 0.0;
  struct recording_seam s;
  observe_only(&s);
  s.deviate_for_row[0] = 0.5;
  const int status = run_process(halos, 1, &s, 0, z);
  printf("\n  SnapNum -1, dT 0: status %d, FFBRegime %d", status, gals[0].FFBRegime);
  TEST_ASSERT(status == 0, "the initial-boundary row must be classified without error");
  TEST_ASSERT(s.observed == 1 && s.trace[0].f_ffb == 1.0 && gals[0].FFBRegime == 1,
              "the row must be classified (an infinite Mvir has fraction exactly 1)");
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_determine_ffb_regime Module\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("\n%s--- Software Quality Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_module_lifecycle);
  TEST_RUN(test_enhanced_sf_missing_refused);
  TEST_RUN(test_enhanced_sf_values);
  TEST_RUN(test_placement_refused);
  TEST_RUN(test_requires_transaction);

  printf("\n%s--- Classifier Tests ---%s\n", BLUE, NC);
  TEST_RUN(test_off_writes_nothing);
  TEST_RUN(test_types_classified_type3_kept);
  TEST_RUN(test_cgm_regime_ignored);
  TEST_RUN(test_reads_narrowed_mvir);
  TEST_RUN(test_reads_current_redshift);
  TEST_RUN(test_draw_keys);
  TEST_RUN(test_seam_tie);
  TEST_RUN(test_initial_boundary_row);

  TEST_SUMMARY();
  return TEST_RESULT();
}
