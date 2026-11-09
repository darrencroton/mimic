/**
 * @file    test_unit_sage26_record_transition_state.c
 * @brief   Unit tests for sage26_record_transition_state and its hand-off to the merger clock
 *
 * The module records each galaxy row's final Type in PreviousType and writes the
 * outputs infallStellarMass and TimeOfInfall by SAGE26's writer rule (0 for a
 * Type 0 row, the held state otherwise).
 * sage26_initialise_merger_clock reads PreviousType at the next snapshot's
 * pre_timestep to make SAGE26's joining writes.
 *
 * Test cases:
 *   - test_placement_*: init() accepts only one post_timestep process_full_halo
 *     entry, last in the phase, and no pre_timestep or substep entry
 *   - test_records_final_type: every galaxy row's Type, Type 3 included, lands in
 *     PreviousType; rows without a galaxy are skipped
 *   - test_output_rule: a Type 0 row writes 0 and keeps its state; Type 1 and 2
 *     rows write their state
 *   - test_consecutive_snapshots: two snapshots through the core's inheritance
 *     service; every transition case (0 -> 1, 1 -> 1, 1 -> 0, non-main 0 -> 2,
 *     1 -> 2, 2 -> 2, a new galaxy) reads the recorded Type and gets SAGE26's
 *     clock and infall writes, and the written values follow the output rule
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"
#include "../../../../tests/framework/test_phase_config.h"

#include "core/galaxy_pool.h"
#include "core/inheritance.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_constants.h"
#include "shared/sage_merger_clock.h"

extern int sage26_record_transition_state_init(void);
extern int sage26_record_transition_state_process(struct ModuleContext *ctx, struct Halo *halos,
                                                  int ngal);
extern int sage26_record_transition_state_cleanup(void);
extern int sage26_initialise_merger_clock_init(void);
extern int sage26_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos,
                                                  int ngal);
extern int sage26_initialise_merger_clock_cleanup(void);

static const char *RECORDER = "sage26_record_transition_state";
static const char *CLOCK = "sage26_initialise_merger_clock";

// ============================================================================
// FIXTURES
// ============================================================================

/** @brief Append one post_timestep entry (the test framework has no helper for this phase) */
static void post_timestep_add(const char *module_name, enum ProcessingMode mode) {
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

static void free_post_timestep(void) {
  if (MimicConfig.post_timestep == NULL) {
    return;
  }
  for (int i = 0; i < MimicConfig.num_post_timestep; i++) {
    free(MimicConfig.post_timestep[i].module_name);
  }
  myfree(MimicConfig.post_timestep);
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;
}

static void free_phases(void) {
  free_post_timestep();
  test_free_pre_timestep();
  test_free_substep_phases();
}

/** @brief The mini-Millennium cosmology and particle mass, as the clock reads them */
static void set_cosmology(void) {
  MimicConfig.PartMass = 0.086;
  MimicConfig.Hubble = 100.0;
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.G = 43.00707785642063;
}

static struct ModuleContext make_context(const int snapshot, const double redshift) {
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.snapshot_number = snapshot;
  ctx.redshift = redshift;
  ctx.num_substeps = 1;
  return ctx;
}

// ============================================================================
// PLACEMENT
// ============================================================================

/**
 * @test    test_placement_accepts_last_post_timestep
 * @brief   One post_timestep process_full_halo entry, last in the phase, initialises
 */
static int test_placement_accepts_last_post_timestep(void) {
  reset_config();
  init_memory_system(0);
  post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);
  post_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);

  TEST_ASSERT(sage26_record_transition_state_init() == 0,
              "init should accept the module as the last post_timestep entry");
  sage26_record_transition_state_cleanup();
  free_phases();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_placement_rejects_not_last
 * @brief   A post_timestep entry after the recorder is rejected
 */
static int test_placement_rejects_not_last(void) {
  reset_config();
  init_memory_system(0);
  post_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);
  post_timestep_add("sage26_finalize_sfr_histories", PROCESSING_MODE_FULL_HALO);

  TEST_ASSERT(sage26_record_transition_state_init() != 0,
              "init should reject a post_timestep module after the recorder");
  free_phases();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_placement_rejects_other_phases
 * @brief   Entries in pre_timestep or a substep phase, or two entries, are rejected
 */
static int test_placement_rejects_other_phases(void) {
  reset_config();
  init_memory_system(0);
  test_pre_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);
  post_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sage26_record_transition_state_init() != 0,
              "init should reject a pre_timestep entry");
  free_phases();

  test_phase_add("galaxy_physics", RECORDER, PROCESSING_MODE_FULL_HALO);
  post_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sage26_record_transition_state_init() != 0,
              "init should reject a substep-phase entry");
  free_phases();

  post_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);
  post_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sage26_record_transition_state_init() != 0,
              "init should reject two post_timestep entries");
  free_phases();

  TEST_ASSERT(sage26_record_transition_state_init() != 0,
              "init should reject a configuration without the post_timestep entry");
  free_phases();
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// PROCESS
// ============================================================================

/**
 * @test    test_records_final_type
 * @brief   Every galaxy row's Type lands in PreviousType; rows without a galaxy are skipped
 */
static int test_records_final_type(void) {
  struct GalaxyData galaxies[4];
  struct Halo halos[5];
  memset(halos, 0, sizeof(halos));
  for (int i = 0; i < 4; i++) {
    init_galaxy_defaults(&galaxies[i]);
    halos[i].Type = i; /* 0, 1, 2 and a merged 3 */
    halos[i].galaxy = &galaxies[i];
  }
  halos[4].Type = 1;
  halos[4].galaxy = NULL;

  struct ModuleContext ctx = make_context(40, 1.0);
  TEST_ASSERT(sage26_record_transition_state_process(&ctx, halos, 5) == 0, "process succeeds");
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT(galaxies[i].PreviousType == i, "PreviousType should be the row's final Type");
  }
  TEST_ASSERT(sage26_record_transition_state_process(&ctx, NULL, 0) == 0,
              "an empty FoF is accepted");
  return TEST_PASS;
}

/**
 * @test    test_output_rule
 * @brief   Type 0 writes 0 and keeps its state; Type 1 and 2 write their state
 */
static int test_output_rule(void) {
  struct GalaxyData galaxies[3];
  struct Halo halos[3];
  memset(halos, 0, sizeof(halos));
  for (int i = 0; i < 3; i++) {
    init_galaxy_defaults(&galaxies[i]);
    halos[i].Type = i;
    halos[i].galaxy = &galaxies[i];
    galaxies[i].infallStellarMassState = 0.25f * (float)(i + 1);
    galaxies[i].TimeOfInfallState = (float)(30 + i);
  }

  struct ModuleContext ctx = make_context(40, 1.0);
  TEST_ASSERT(sage26_record_transition_state_process(&ctx, halos, 3) == 0, "process succeeds");

  TEST_ASSERT(galaxies[0].infallStellarMass == 0.0f, "a Type 0 row writes infallStellarMass 0");
  TEST_ASSERT(galaxies[0].TimeOfInfall == 0.0f, "a Type 0 row writes TimeOfInfall 0");
  TEST_ASSERT(galaxies[0].infallStellarMassState == 0.25f,
              "a Type 0 row keeps its infallStellarMass state");
  TEST_ASSERT(galaxies[0].TimeOfInfallState == 30.0f, "a Type 0 row keeps its TimeOfInfall state");
  printf("\n  Type 0 row: written infallStellarMass %g TimeOfInfall %g; state %g %g ",
         (double)galaxies[0].infallStellarMass, (double)galaxies[0].TimeOfInfall,
         (double)galaxies[0].infallStellarMassState, (double)galaxies[0].TimeOfInfallState);
  for (int i = 1; i < 3; i++) {
    TEST_ASSERT(galaxies[i].infallStellarMass == galaxies[i].infallStellarMassState,
                "a satellite row writes its infallStellarMass state");
    TEST_ASSERT(galaxies[i].TimeOfInfall == galaxies[i].TimeOfInfallState,
                "a satellite row writes its TimeOfInfall state");
  }
  return TEST_PASS;
}

// ============================================================================
// CONSECUTIVE SNAPSHOTS THROUGH THE CORE'S INHERITANCE
// ============================================================================

/** @brief One previous-snapshot galaxy row */
static void make_source(struct Halo *halo, struct GalaxyData *galaxy, const int type,
                        const float mergtime, const float stellar_mass, const float infall_sm,
                        const float infall_time) {
  memset(halo, 0, sizeof(*halo));
  init_galaxy_defaults(galaxy);
  halo->galaxy = galaxy;
  halo->Type = type;
  halo->SnapNum = 40;
  halo->Len = 200;
  halo->Mvir = 5.0;
  halo->Rvir = 0.1;
  halo->Vvir = 120.0;
  halo->Vmax = 130.0f;
  galaxy->MergTime = mergtime;
  galaxy->StellarMass = stellar_mass;
  galaxy->ColdGas = 0.5f * stellar_mass;
  galaxy->infallStellarMassState = infall_sm;
  galaxy->TimeOfInfallState = infall_time;
}

/** @brief A snapshot-41 descendant with the core driver's virial values */
static struct InheritanceDescendant make_descendant(const long long halo_nr, const int len,
                                                    const double mvir, const int is_fof_central,
                                                    const double redshift) {
  struct InheritanceDescendant d;
  memset(&d, 0, sizeof(d));
  d.halo_nr = halo_nr;
  d.current_snap = 41;
  d.virial_mass = mvir;
  d.virial_radius = virial_radius_for_mass(mvir, redshift);
  d.virial_velocity = virial_velocity_for(mvir, d.virial_radius);
  d.is_fof_central = is_fof_central;
  d.halo_payload.Len = len;
  d.halo_payload.Vmax = 150.0f;
  d.halo_payload.SnapNum = 41;
  d.halo_payload.Mvir = mvir;
  d.halo_payload.Rvir = d.virial_radius;
  d.halo_payload.Vvir = d.virial_velocity;
  return d;
}

/**
 * @test    test_consecutive_snapshots
 * @brief   Every transition case reads the recorded Type and gets SAGE26's joining writes
 *
 * Snapshot 40 holds, with the recorder run at its end:
 *   A Type 0 central, never a satellite         -> snapshot 41: main branch of subhalo 21 (0 -> 1)
 *   B Type 1, clock 3.5, infall (0.7, 33)       -> main branch of subhalo 22 (1 -> 1)
 *   C Type 1, clock 2.0, infall (0.5, 35)       -> main branch of FoF halo 20 (1 -> 0)
 *   D Type 0, earlier infall (0.2, 30)          -> non-main progenitor of halo 20 (0 -> 2)
 *   E Type 1, clock 4.0, infall (0.3, 36)       -> non-main progenitor of halo 20 (1 -> 2)
 *   F Type 2, clock 0.5, infall (0.1, 31)       -> progenitor of halo 20 (2 -> 2)
 * and snapshot 41 adds a new FoF, halo 30, with no progenitor galaxy.
 */
static int test_consecutive_snapshots(void) {
  reset_config();
  init_memory_system(0);
  set_cosmology();
  test_pre_timestep_add(CLOCK, PROCESSING_MODE_FULL_HALO);
  post_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);
  TEST_ASSERT(sage26_initialise_merger_clock_init() == 0, "the clock initialises");
  TEST_ASSERT(sage26_record_transition_state_init() == 0, "the recorder initialises");

  // ----- snapshot 40: the progenitors, then the recorder at the end of the snapshot -----
  enum { A, B, C, D, E, F, NSRC };
  struct Halo src[NSRC];
  struct GalaxyData src_gal[NSRC];
  make_source(&src[A], &src_gal[A], 0, SAGE_MERGTIME_UNSET, 2.0f, -1.0f, -1.0f);
  make_source(&src[B], &src_gal[B], 1, 3.5f, 0.8f, 0.7f, 33.0f);
  make_source(&src[C], &src_gal[C], 1, 2.0f, 0.6f, 0.5f, 35.0f);
  make_source(&src[D], &src_gal[D], 0, SAGE_MERGTIME_UNSET, 0.9f, 0.2f, 30.0f);
  make_source(&src[E], &src_gal[E], 1, 4.0f, 0.4f, 0.3f, 36.0f);
  make_source(&src[F], &src_gal[F], 2, 0.5f, 0.15f, 0.1f, 31.0f);
  struct ModuleContext ctx40 = make_context(40, 1.0778745836458805);
  TEST_ASSERT(sage26_record_transition_state_process(&ctx40, src, NSRC) == 0,
              "the recorder runs at the end of snapshot 40");

  // ----- snapshot 41: the core joins the progenitors into FoF 20 -----
  const double z41 = 0.98870811532120606;
  struct GalaxyPool *pool = galaxy_pool_create(16);
  struct Halo ws[8];
  memset(ws, 0, sizeof(ws));

  const struct InheritanceDescendant halo20 = make_descendant(20, 1200, 100.0, 1, z41);
  const struct InheritanceProgenitorGalaxy prog20[] = {
      {&src[C], 0.0, 1}, {&src[D], 0.0, 0}, {&src[E], 0.0, 0}, {&src[F], 0.0, 0}};
  int64_t end = inherit_descendant_halos(pool, ws, 0, 8, &halo20, prog20, 4);
  const struct InheritanceDescendant halo21 = make_descendant(21, 30, 30 * 0.086, 0, z41);
  const struct InheritanceProgenitorGalaxy prog21[] = {{&src[A], 0.0, 1}};
  end = inherit_descendant_halos(pool, ws, end, 8, &halo21, prog21, 1);
  const struct InheritanceDescendant halo22 = make_descendant(22, 50, 50 * 0.086, 0, z41);
  const struct InheritanceProgenitorGalaxy prog22[] = {{&src[B], 0.0, 1}};
  end = inherit_descendant_halos(pool, ws, end, 8, &halo22, prog22, 1);
  TEST_ASSERT(end == 6, "FoF 20 holds the six joined galaxies");

  struct Halo *c = &ws[0], *d = &ws[1], *e = &ws[2], *f = &ws[3], *a = &ws[4], *b = &ws[5];
  TEST_ASSERT(c->Type == 0 && d->Type == 2 && e->Type == 2 && f->Type == 2 && a->Type == 1 &&
                  b->Type == 1,
              "the core assigns the new Types");
  TEST_ASSERT(a->galaxy->PreviousType == 0 && b->galaxy->PreviousType == 1 &&
                  c->galaxy->PreviousType == 1 && d->galaxy->PreviousType == 0 &&
                  e->galaxy->PreviousType == 1 && f->galaxy->PreviousType == 2,
              "each galaxy carries the Type the recorder wrote at snapshot 40");

  struct ModuleContext ctx41 = make_context(41, z41);
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx41, ws, (int)end) == 0,
              "the clock runs at snapshot 41's pre_timestep");

  // 0 -> 1: infall recorded at snapshot 41, clock estimated against the FoF central
  const double expected_a = mimic_sage_estimate_merging_time(a, c, z41, 2.0, MimicConfig.G, NULL);
  TEST_ASSERT(a->galaxy->TimeOfInfallState == 41.0f, "0 -> 1: TimeOfInfall is the snapshot");
  TEST_ASSERT(a->galaxy->infallStellarMassState == 2.0f, "0 -> 1: infallStellarMass recorded");
  TEST_ASSERT(a->galaxy->MergTime == (float)expected_a && expected_a > 0.0 && expected_a < 999.0,
              "0 -> 1: the clock is estimated");
  // 1 -> 1: clock and infall kept
  TEST_ASSERT(b->galaxy->MergTime == 3.5f, "1 -> 1: the clock is kept");
  TEST_ASSERT(b->galaxy->infallStellarMassState == 0.7f && b->galaxy->TimeOfInfallState == 33.0f,
              "1 -> 1: the infall state is kept");
  // 1 -> 0: promotion resets the clock, keeps the state
  TEST_ASSERT(c->galaxy->MergTime == SAGE_MERGTIME_UNSET, "1 -> 0: the clock is reset to 999.9");
  TEST_ASSERT(c->galaxy->infallStellarMassState == 0.5f && c->galaxy->TimeOfInfallState == 35.0f,
              "1 -> 0: the infall state is kept");
  // non-main 0 -> 2: clock 0, infallStellarMass recorded, earlier TimeOfInfall kept
  TEST_ASSERT(d->galaxy->MergTime == 0.0f, "0 -> 2: the clock starts at 0");
  TEST_ASSERT(d->galaxy->infallStellarMassState == 0.9f, "0 -> 2: infallStellarMass recorded");
  TEST_ASSERT(d->galaxy->TimeOfInfallState == 30.0f, "0 -> 2: the earlier TimeOfInfall is kept");
  // 1 -> 2: clock and infall kept
  TEST_ASSERT(e->galaxy->MergTime == 4.0f, "1 -> 2: the clock is kept");
  TEST_ASSERT(e->galaxy->infallStellarMassState == 0.3f && e->galaxy->TimeOfInfallState == 36.0f,
              "1 -> 2: the infall state is kept");
  // 2 -> 2: untouched
  TEST_ASSERT(f->galaxy->MergTime == 0.5f && f->galaxy->infallStellarMassState == 0.1f &&
                  f->galaxy->TimeOfInfallState == 31.0f,
              "2 -> 2: untouched");

  // The end of snapshot 41: the written values
  TEST_ASSERT(sage26_record_transition_state_process(&ctx41, ws, (int)end) == 0,
              "the recorder runs at the end of snapshot 41");
  TEST_ASSERT(c->galaxy->infallStellarMass == 0.0f && c->galaxy->TimeOfInfall == 0.0f,
              "the promoted central is written with 0, 0");
  TEST_ASSERT(c->galaxy->infallStellarMassState == 0.5f && c->galaxy->TimeOfInfallState == 35.0f,
              "the promoted central keeps its state");
  TEST_ASSERT(a->galaxy->TimeOfInfall == 41.0f && a->galaxy->infallStellarMass == 2.0f,
              "the new satellite is written with its infall values");
  TEST_ASSERT(d->galaxy->TimeOfInfall == 30.0f && d->galaxy->infallStellarMass == 0.9f,
              "the new orphan is written with the earlier TimeOfInfall");
  TEST_ASSERT(c->galaxy->PreviousType == 0 && a->galaxy->PreviousType == 1 &&
                  d->galaxy->PreviousType == 2,
              "the snapshot-41 Types are recorded for snapshot 42");
  printf("\n  1 -> 0 central: written (%g, %g), state (%g, %g); 0 -> 1: TimeOfInfall %g, "
         "MergTime %.6g; 0 -> 2: written TimeOfInfall %g ",
         (double)c->galaxy->infallStellarMass, (double)c->galaxy->TimeOfInfall,
         (double)c->galaxy->infallStellarMassState, (double)c->galaxy->TimeOfInfallState,
         (double)a->galaxy->TimeOfInfall, (double)a->galaxy->MergTime,
         (double)d->galaxy->TimeOfInfall);

  // A new FoF: the core creates a Type 0 with no previous Type; the clock leaves it be
  struct Halo ws_new[1];
  memset(ws_new, 0, sizeof(ws_new));
  const struct InheritanceDescendant halo30 = make_descendant(30, 300, 25.0, 1, z41);
  const int64_t end_new = inherit_descendant_halos(pool, ws_new, 0, 1, &halo30, NULL, 0);
  TEST_ASSERT(end_new == 1 && ws_new[0].Type == 0 && ws_new[0].galaxy->PreviousType == -1,
              "a new galaxy is a Type 0 without a previous Type");
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx41, ws_new, 1) == 0, "clock runs");
  TEST_ASSERT(ws_new[0].galaxy->MergTime == SAGE_MERGTIME_UNSET &&
                  ws_new[0].galaxy->infallStellarMassState == -1.0f &&
                  ws_new[0].galaxy->TimeOfInfallState == -1.0f,
              "a new galaxy keeps SAGE26's creation values");

  galaxy_pool_destroy(pool);
  sage26_initialise_merger_clock_cleanup();
  sage26_record_transition_state_cleanup();
  free_phases();
  check_memory_leaks();
  return TEST_PASS;
}

// ============================================================================
// MAIN TEST RUNNER
// ============================================================================

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_record_transition_state\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_placement_accepts_last_post_timestep);
  TEST_RUN(test_placement_rejects_not_last);
  TEST_RUN(test_placement_rejects_other_phases);
  TEST_RUN(test_records_final_type);
  TEST_RUN(test_output_rule);
  TEST_RUN(test_consecutive_snapshots);

  TEST_SUMMARY();
  return TEST_RESULT();
}
