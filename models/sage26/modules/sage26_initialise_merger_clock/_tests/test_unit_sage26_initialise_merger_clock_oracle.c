/**
 * @file    test_unit_sage26_initialise_merger_clock_oracle.c
 * @brief   Bit-exact comparison of the merger clock and joining writes with SAGE26
 *
 * Runs every SAGE26 oracle fixture fixtures.manifest lists for
 * sage26_initialise_merger_clock (its _tests/oracle/ directory), by routine:
 *
 * - estimate_merging_time (R/src/model_mergers.c:89-121): the shared
 *   mimic_sage_estimate_merging_time() on two workspace rows built from the
 *   fixture's halo catalogue, asserting return.mergtime and the flags bit for bit,
 *   and each carried intermediate whose _validated marker is 1 (the others are
 *   checked by the oracle, not validated, and their markers are consumed);
 *   absent intermediates must be absent in the package's trace too.
 * - join_progenitors, the source-reviewed transcription of progenitor joining
 *   (R/src/core_build_model.c:240-341): the core's own inheritance service
 *   (inherit_descendant_halos, src/core/inheritance.c) joins the fixture's
 *   progenitor galaxies into the descendant halo, then the module's process()
 *   makes the package's joining writes (modules/_tests/sage26_oracle_join.h). Each
 *   retained galaxy is asserted on the package's fields (MergTime; infallStellarMass
 *   and TimeOfInfall through the private state properties that hold SAGE26's
 *   members; and the sixty history elements, which the module zeroes for a galaxy
 *   that stays a Type 0 or 1 and leaves for a new orphan, B:252, :278-282, :322-337)
 *   and, as evidence of the composition, on the core's (Type, SnapNum, HaloNr, Len,
 *   Mvir, deltaMvir, Rvir, Vvir, Vmax, infallMvir, infallVvir, infallVmax, each
 *   SAGE26 float compared with the core's double narrowed to float) and on
 *   StellarMass and ColdGas, which the clock reads.
 * - join_and_pass (tr_jp_probe_already_merged_trailing, owned here by PM decision
 *   PM-D3): the joining as above with every galaxy's full state and the pass's
 *   timing, then sage26_set_disk_scale_radius and the package's merger pass
 *   (sage26_resolve_mergers_and_disruption) at the fixture's substep, asserting
 *   the joined state (intermediate.join.galaxy[*]) and the end state (galaxy[*]).
 *   The resolver's oracle test asserts the other join_and_pass fixtures in full.
 *
 * Fields excluded from the joining assertions, by reason (each consumed by name):
 * - core-boundary: an orphan's Len (the core zeros it, SAGE26 keeps it,
 *   src/core/inheritance.c:86) and the infall halo values of a Type 1 with a clock
 *   above 999 that loses its subhalo (SAGE26 writes them, B:327-333; the core
 *   writes them only for a previous Type 0, src/core/inheritance.c:88-92);
 * - other owners: the Cooling, Heating, QuasarModeBHaccretionMass and OutflowRate
 *   resets (the core's init_repeat on every inherited row; SAGE26 resets them only
 *   on the main branch, B:273-276, so a new orphan keeps them there: recorded in the
 *   resolver's validation as a residual), the central's disk and bulge radii
 *   (sage26_set_disk_scale_radius), and the reservoirs joining copies unchanged;
 * - SAGE26 bookkeeping without a package counterpart: mergeType, mergeIntoID,
 *   CentralGal, dT, and the driver locals intermediate.join.progenitor[*] (their
 *   effects are the asserted fields).
 *
 * The fixtures' float halo masses enter as the core holds them: the catalogue
 * float widened, or Len * PartMass (core_virial_mass below restates the core's
 * get_virial_mass, src/core/virial.c:47-54, whose input view has no setter for the
 * catalogue mass). Outside the oracle environment (macOS arm64, Apple clang 21)
 * the bit-exact test reports SKIP.
 *
 * Test cases:
 *   - test_oracle_fixture_count: every fixture the manifest lists is present
 *   - test_oracle_bit_identity: every fixture reproduces the oracle bit for bit
 */

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/galaxy_pool.h"
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
#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_join.h"
#include "shared/sage_constants.h"
#include "shared/sage_merger_clock.h"

extern int sage26_initialise_merger_clock_init(void);
extern int sage26_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos,
                                                  int ngal);
extern int sage26_initialise_merger_clock_cleanup(void);
extern int sage26_resolve_mergers_and_disruption_init(void);
extern int sage26_resolve_mergers_and_disruption_process(struct ModuleContext *ctx,
                                                         struct Halo *halos, int ngal);
extern int sage26_resolve_mergers_and_disruption_cleanup(void);

static const char *MODULE = "sage26_initialise_merger_clock";

// ============================================================================
// FIXTURE HELPERS
// ============================================================================

/** @brief "<prefix>galaxy[<j>].<member>" */
static const char *prefixed(const char *prefix, const int j, const char *member) {
  return oracle_prefixed_galaxy_field(prefix, j, member);
}

/** @brief Whether name starts with prefix */
static int starts_with(const char *name, const char *prefix) {
  return strncmp(name, prefix, strlen(prefix)) == 0;
}

/**
 * @brief Consume every field whose name starts with prefix (a deliberate, named exclusion)
 *
 * @return Number of fields consumed
 */
static int consume_prefix(struct oracle_fixture *fx, const char *prefix) {
  int n = 0;
  for (int i = 0; i < fx->num_fields; i++) {
    if (!fx->fields[i].consumed && starts_with(fx->fields[i].name, prefix)) {
      fx->fields[i].consumed = 1;
      n++;
    }
  }
  return n;
}

/** @brief Set the cosmology and MergerTimeFactor from the fixture and re-run the module init */
static int set_params(struct oracle_fixture *in) {
  MimicConfig.PartMass = oracle_in_f64(in, "params.PartMass");
  MimicConfig.Hubble = oracle_in_f64(in, "params.Hubble");
  MimicConfig.Omega = oracle_in_f64(in, "params.Omega");
  MimicConfig.OmegaLambda = oracle_in_f64(in, "params.OmegaLambda");
  MimicConfig.G = oracle_in_f64(in, "params.G");
  if (oracle_set_model_param("MergerTimeFactor", oracle_in_f64(in, "params.MergerTimeFactor")) !=
      0) {
    return 1;
  }
  if (sage26_initialise_merger_clock_init() != 0) {
    fprintf(stderr, "oracle: %s: module init failed\n", in->path);
    return 1;
  }
  return 0;
}

// ============================================================================
// ESTIMATE_MERGING_TIME
// ============================================================================

/** @brief Assert one carried double intermediate: absence always, the value where validated */
static int check_intermediate(struct oracle_fixture *out, const char *name, const int absent,
                              const double value) {
  char key[ORACLE_MAX_NAME];
  snprintf(key, sizeof(key), "intermediate.%s_absent", name);
  int problems = oracle_check_i32(out, key, absent);
  if (absent) {
    return problems;
  }
  snprintf(key, sizeof(key), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, key);
  snprintf(key, sizeof(key), "intermediate.%s", name);
  if (validated == 1) {
    problems += oracle_check_f64(out, key, value);
  } else {
    // Checked by the oracle through SAGE26's continuation, not two-sided validated:
    // asserted through the end state only (the node's rule for carried values)
    oracle_exclude(out, key);
  }
  return problems;
}

static int check_estimate(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "estimate_merging_time", &in, &out) != 0) {
    return 1;
  }
  int problems = set_params(&in);

  const int sat_halo = oracle_in_i32(&in, "call.sat_halo");
  const int mother_halo = oracle_in_i32(&in, "call.mother_halo");
  const int ngal = oracle_in_i32(&in, "call.ngal");

  struct GalaxyData sat_gal;
  struct Halo rows[2];
  oracle_poison(&sat_gal, sizeof(sat_gal));
  oracle_poison(rows, sizeof(rows));
  sat_gal.StellarMass = oracle_in_f32(&in, oracle_galaxy_field(ngal, "StellarMass"));
  sat_gal.ColdGas = oracle_in_f32(&in, oracle_galaxy_field(ngal, "ColdGas"));

  struct Halo *satellite = &rows[0];
  struct Halo *mother = &rows[1];
  satellite->HaloNr = sat_halo;
  satellite->Len = oracle_in_i32(&in, oracle_join_halo_field(sat_halo, "Len"));
  satellite->Mvir = oracle_join_core_virial_mass(&in, sat_halo);
  satellite->galaxy = &sat_gal;
  mother->HaloNr = mother_halo;
  mother->Len = oracle_in_i32(&in, oracle_join_halo_field(mother_halo, "Len"));
  mother->Mvir = oracle_join_core_virial_mass(&in, mother_halo);
  mother->galaxy = NULL;

  struct MimicSageMergerClockTrace trace;
  const double mergtime = mimic_sage_estimate_merging_time(
      satellite, mother, oracle_join_halo_redshift(&in, mother_halo),
      oracle_in_f64(&in, "params.MergerTimeFactor"), MimicConfig.G, &trace);

  problems += oracle_check_f64(&out, "return.mergtime", mergtime);
  problems += oracle_check_i32(&out, "intermediate.clock_same_halo", trace.same_halo);
  problems += oracle_check_i32(&out, "intermediate.clock_guard_passed", trace.guard_passed);
  problems += oracle_check_i32(&out, "intermediate.clock_capped", trace.capped);
  problems +=
      check_intermediate(&out, "sat_virial_mass", trace.values_absent, trace.sat_virial_mass);
  problems += check_intermediate(&out, "coulomb", trace.values_absent, trace.coulomb);
  problems += check_intermediate(&out, "SatelliteMass", trace.values_absent, trace.satellite_mass);
  problems +=
      check_intermediate(&out, "SatelliteRadius", trace.values_absent, trace.satellite_radius);
  problems += check_intermediate(&out, "mother_virial_velocity",
                                 trace.mother_virial_velocity_absent, trace.mother_virial_velocity);
  problems +=
      check_intermediate(&out, "mergtime_pre_cap", trace.values_absent, trace.mergtime_pre_cap);

  // The oracle's check and validation markers of each carried value (not package state)
  for (int i = 0; i < out.num_fields; i++) {
    const char *field = out.fields[i].name;
    if (strstr(field, "_checked") != NULL || strstr(field, "_check_kind") != NULL ||
        strstr(field, "_validated_up") != NULL || strstr(field, "_validated_down") != NULL) {
      out.fields[i].consumed = 1;
    }
  }
  // Inputs estimate_merging_time does not read: the galaxy's Type and SnapNum (its
  // error message only), the other catalogue entries and redshifts
  consume_prefix(&in, "galaxy[");
  consume_prefix(&in, "halo[");
  consume_prefix(&in, "params.ZZ[");
  consume_prefix(&in, "call.nhalos");

  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

// ============================================================================
// PROGENITOR JOINING (THE CORE'S INHERITANCE, THEN THE MODULE)
// ============================================================================

/**
 * @brief Assert one joined galaxy row against the fields "<prefix>galaxy[<j>].*"
 */
static int check_joined_galaxy(struct oracle_fixture *out, const char *prefix, const int j,
                               const struct Halo *row, const int source_type,
                               const float source_clock) {
  struct GalaxyData *g = row->galaxy;
  const int newly_orphaned = (row->Type == 2 && (source_type == 0 || source_type == 1));
  int problems = 0;

  // The package's history reset: zeroed for a row that stays a Type 0 or 1, kept otherwise
  problems += oracle_c05_check_histories(out, prefix, j, g);

  // The package's fields
  problems += oracle_check_f32(out, prefixed(prefix, j, "MergTime"), g->MergTime);
  problems +=
      oracle_check_f32(out, prefixed(prefix, j, "infallStellarMass"), g->infallStellarMassState);
  problems += oracle_check_f32(out, prefixed(prefix, j, "TimeOfInfall"), g->TimeOfInfallState);
  problems += oracle_check_f32(out, prefixed(prefix, j, "StellarMass"), g->StellarMass);
  problems += oracle_check_f32(out, prefixed(prefix, j, "ColdGas"), g->ColdGas);

  // The core's fields, SAGE26's floats against the core's values narrowed to float
  problems += oracle_check_i32(out, prefixed(prefix, j, "Type"), row->Type);
  problems += oracle_check_i32(out, prefixed(prefix, j, "SnapNum"), row->SnapNum);
  problems += oracle_check_i32(out, prefixed(prefix, j, "HaloNr"), (int)row->HaloNr);
  problems += oracle_check_f32(out, prefixed(prefix, j, "Mvir"), (float)row->Mvir);
  problems += oracle_check_f32(out, prefixed(prefix, j, "deltaMvir"), (float)row->deltaMvir);
  problems += oracle_check_f32(out, prefixed(prefix, j, "Rvir"), (float)row->Rvir);
  problems += oracle_check_f32(out, prefixed(prefix, j, "Vvir"), (float)row->Vvir);
  problems += oracle_check_f32(out, prefixed(prefix, j, "Vmax"), row->Vmax);
  if (newly_orphaned) {
    // Core boundary: the core zeros an orphan's Len, SAGE26 keeps it
    oracle_exclude(out, prefixed(prefix, j, "Len"));
  } else {
    problems += oracle_check_i32(out, prefixed(prefix, j, "Len"), row->Len);
  }
  if (newly_orphaned && source_type == 1 && source_clock > 999.0f) {
    // Core boundary: SAGE26 records the infall halo values of a Type 1 with an unset
    // clock that loses its subhalo (B:327-333); the core records them only for a Type 0
    oracle_exclude(out, prefixed(prefix, j, "infallMvir"));
    oracle_exclude(out, prefixed(prefix, j, "infallVvir"));
    oracle_exclude(out, prefixed(prefix, j, "infallVmax"));
  } else {
    problems += oracle_check_f32(out, prefixed(prefix, j, "infallMvir"), (float)row->infallMvir);
    problems += oracle_check_f32(out, prefixed(prefix, j, "infallVvir"), (float)row->infallVvir);
    problems += oracle_check_f32(out, prefixed(prefix, j, "infallVmax"), (float)row->infallVmax);
  }

  // Everything else this galaxy's record holds: other owners or SAGE26 bookkeeping
  // (the file comment lists them)
  char rest[ORACLE_MAX_NAME];
  snprintf(rest, sizeof(rest), "%sgalaxy[%d].", prefix, j);
  consume_prefix(out, rest);
  return problems;
}

/** @brief Inputs the composition does not read (the file comment gives the reasons) */
static void consume_join_inputs(struct oracle_fixture *in) {
  consume_prefix(in, "galaxy[");
  consume_prefix(in, "halo[");
  consume_prefix(in, "params.");
  consume_prefix(in, "call.nhalos");
}

static int check_join(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "join_progenitors", &in, &out) != 0) {
    return 1;
  }
  int problems = set_params(&in);

  struct GalaxyPool *pool = galaxy_pool_create(16);
  static struct oracle_join_result result;
  struct ModuleContext ctx;
  problems += oracle_join(&in, pool, &result, &ctx, 0);

  if (problems == 0) {
    const int retained = result.end - result.start;
    problems += oracle_check_i32(&out, "return.ngal", retained);
    for (int j = 0; j < retained; j++) {
      problems += check_joined_galaxy(&out, "", j, &result.workspace[result.start + j],
                                      result.source_type[j], result.source_clock[j]);
    }
  }
  // SAGE26's driver locals (their effects are the asserted fields)
  consume_prefix(&out, "intermediate.join.progenitor[");
  consume_join_inputs(&in);

  galaxy_pool_destroy(pool);
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/**
 * @brief join_and_pass: joining, then the package's merger pass at the fixture's substep
 */
static int check_join_and_pass(const char *name) {
  static struct oracle_fixture in, out;
  if (oracle_load_pair(MODULE, name, "join_and_pass", &in, &out) != 0) {
    return 1;
  }
  int problems = set_params(&in);
  /* BulgeSizeOn: the resolver's required parameter, at the fixtures' R0 value 0 */
  if (oracle_set_model_param("ThresholdMajorMerger",
                             oracle_in_f64(&in, "params.ThreshMajorMerger")) != 0 ||
      oracle_set_model_param("ThresholdSatDisruption",
                             oracle_in_f64(&in, "params.ThresholdSatDisruption")) != 0 ||
      oracle_set_model_param("BulgeSizeOn", 0.0) != 0 ||
      /* CGMrecipeOn: the resolver's required parameter (D33), at R3's value 0 */
      oracle_set_model_param("CGMrecipeOn", 0.0) != 0) {
    problems++;
  }
  if (problems == 0 && sage26_resolve_mergers_and_disruption_init() != 0) {
    fprintf(stderr, "oracle: %s: resolver init failed\n", name);
    problems++;
  }

  struct GalaxyPool *pool = galaxy_pool_create(16);
  static struct oracle_join_result result;
  struct ModuleContext ctx;
  if (problems == 0) {
    // With the pass's timing (dT = Age[SnapNum] - halo_age, ctx->time = halo_age)
    problems += oracle_join(&in, pool, &result, &ctx, 1);
  }

  if (problems == 0) {
    const int retained = result.end - result.start;
    problems += oracle_check_i32(&out, "intermediate.join.ngal", retained);
    for (int j = 0; j < retained; j++) {
      problems +=
          check_joined_galaxy(&out, "intermediate.join.", j, &result.workspace[result.start + j],
                              result.source_type[j], result.source_clock[j]);
    }

    // The pass at SAGE26's substep: centralgal is the joined central (B:393-400)
    ctx.substep_number = oracle_in_i32(&in, "call.step");
    ctx.num_substeps = oracle_in_i32(&in, "call.effective_steps");
    if (sage26_resolve_mergers_and_disruption_process(&ctx, result.workspace, result.end) != 0) {
      fprintf(stderr, "oracle: %s: the merger pass failed\n", name);
      problems++;
    }
    for (int j = 0; j < retained; j++) {
      problems += check_joined_galaxy(&out, "", j, &result.workspace[result.start + j],
                                      result.source_type[j], result.source_clock[j]);
    }
  }
  sage26_resolve_mergers_and_disruption_cleanup();

  // SAGE26's driver locals: the joining's per-progenitor record and the pass's
  // per-galaxy record (their effects are the asserted fields)
  consume_prefix(&out, "intermediate.join.progenitor[");
  consume_prefix(&out, "intermediate.pass.");
  // The pass's timing inputs the package forms from its own context
  consume_prefix(&in, "call.");
  consume_join_inputs(&in);

  galaxy_pool_destroy(pool);
  problems += oracle_report_unconsumed(&in);
  problems += oracle_report_unconsumed(&out);
  return problems;
}

/** @brief Dispatch one fixture by its routine line */
static int check_fixture(const char *name) {
  if (strncmp(name, "emt_", 4) == 0) {
    return check_estimate(name);
  }
  if (strncmp(name, "tr_join_", 8) == 0) {
    return check_join(name);
  }
  if (strncmp(name, "tr_jp_", 6) == 0) {
    return check_join_and_pass(name);
  }
  fprintf(stderr, "oracle: %s: no check for this fixture's routine\n", name);
  return 1;
}

// ============================================================================
// TESTS
// ============================================================================

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
  TEST_ASSERT(oracle_all_bit_identical(MODULE, check_fixture),
              "every fixture must reproduce the oracle bit for bit");
  return TEST_PASS;
}

/** @brief Append the transition recorder to post_timestep, so the clock's init sees it */
static void add_recorder_to_post_timestep(void) {
  MimicConfig.post_timestep =
      mymalloc_cat(TEST_PHASE_MODULE_CAP * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  MimicConfig.post_timestep[0].module_name = strdup("sage26_record_transition_state");
  MimicConfig.post_timestep[0].processing_mode = PROCESSING_MODE_FULL_HALO;
  MimicConfig.post_timestep[0].resolved = NULL;
  MimicConfig.num_post_timestep = 1;
}

static void free_post_timestep(void) {
  for (int i = 0; i < MimicConfig.num_post_timestep; i++) {
    free(MimicConfig.post_timestep[i].module_name);
  }
  myfree(MimicConfig.post_timestep);
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_initialise_merger_clock SAGE26 oracle (bit-exact)\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  reset_config();
  init_memory_system(0);
  test_pre_timestep_add(MODULE, PROCESSING_MODE_FULL_HALO);
  add_recorder_to_post_timestep();

  TEST_RUN(test_oracle_fixture_count);
  TEST_RUN(test_oracle_bit_identity);

  sage26_initialise_merger_clock_cleanup();
  free_post_timestep();
  test_free_pre_timestep();
  check_memory_leaks();

  TEST_SUMMARY();
  return TEST_RESULT();
}
