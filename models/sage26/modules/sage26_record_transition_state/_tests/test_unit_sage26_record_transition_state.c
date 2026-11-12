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
 *   - test_records_final_type: every galaxy row's Type, Type 3 included, lands in
 *     PreviousType; rows without a galaxy are skipped; a Type 0 row writes 0 and keeps its
 *     state, every other row writes its state
 *   - test_consecutive_snapshots: two snapshots through the core's inheritance service;
 *     every transition case (0 -> 1, 1 -> 1, 1 -> 0, non-main 0 -> 2, 1 -> 2, 2 -> 2, a new
 *     galaxy) reads the PreviousType the recorder wrote and leaves the infall state SAGE26's
 *     joining leaves, and the written values follow the output rule
 *
 * The module's placement (once, last in post_timestep) is refused, with diagnostics, by
 * test_unit_sage_dependency_contracts.c.
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
#include "virial.h"

#include "modules/_tests/sage_test_fixtures.h"
#include "shared/sage_constants.h"

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

static void free_phases(void) {
  test_free_post_timestep();
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
// PROCESS
// ============================================================================

/**
 * @test    test_records_final_type
 * @brief   Every galaxy row's Type lands in PreviousType and the outputs follow SAGE26's writer
 *          rule; rows without a galaxy are skipped
 *
 * A Type 0 row writes infallStellarMass and TimeOfInfall as 0 and keeps its state; every other
 * row (a merged Type 3 included) writes its state.
 */
static int test_records_final_type(void) {
  struct GalaxyData galaxies[4];
  struct Halo halos[5];
  memset(halos, 0, sizeof(halos));
  for (int i = 0; i < 4; i++) {
    init_galaxy_defaults(&galaxies[i]);
    halos[i].Type = i; /* 0, 1, 2 and a merged 3 */
    halos[i].galaxy = &galaxies[i];
    galaxies[i].infallStellarMassState = 0.25f * (float)(i + 1);
    galaxies[i].TimeOfInfallState = (float)(30 + i);
  }
  halos[4].Type = 1;
  halos[4].galaxy = NULL;

  struct ModuleContext ctx = make_context(40, 1.0);
  TEST_ASSERT(sage26_record_transition_state_process(&ctx, halos, 5) == 0, "process succeeds");
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT(galaxies[i].PreviousType == i, "PreviousType should be the row's final Type");
    TEST_ASSERT(galaxies[i].infallStellarMassState == 0.25f * (float)(i + 1) &&
                    galaxies[i].TimeOfInfallState == (float)(30 + i),
                "the held infall state is never changed");
  }
  TEST_ASSERT(galaxies[0].infallStellarMass == 0.0f && galaxies[0].TimeOfInfall == 0.0f,
              "a Type 0 row writes infallStellarMass and TimeOfInfall 0");
  for (int i = 1; i < 4; i++) {
    TEST_ASSERT(galaxies[i].infallStellarMass == galaxies[i].infallStellarMassState,
                "a row of another Type writes its infallStellarMass state");
    TEST_ASSERT(galaxies[i].TimeOfInfall == galaxies[i].TimeOfInfallState,
                "a row of another Type writes its TimeOfInfall state");
  }
  TEST_ASSERT(sage26_record_transition_state_process(&ctx, NULL, 0) == 0,
              "an empty FoF is accepted");
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
 * @brief   Every transition case reads the PreviousType the recorder wrote and leaves the infall
 *          state SAGE26's joining leaves
 *
 * Snapshot 40 holds, with the recorder run at its end:
 *   A Type 0 central, never a satellite         -> snapshot 41: main branch of subhalo 21 (0 -> 1)
 *   B Type 1, infall (0.7, 33)                  -> main branch of subhalo 22 (1 -> 1)
 *   C Type 1, infall (0.5, 35)                  -> main branch of FoF halo 20 (1 -> 0)
 *   D Type 0, earlier infall (0.2, 30)          -> non-main progenitor of halo 20 (0 -> 2)
 *   E Type 1, infall (0.3, 36)                  -> non-main progenitor of halo 20 (1 -> 2)
 *   F Type 2, infall (0.1, 31)                  -> progenitor of halo 20 (2 -> 2)
 * and snapshot 41 adds a new FoF, halo 30, with no progenitor galaxy. The merger clock's own
 * writes are asserted by its unit tests; here it is the consumer that shows the recorded Type
 * reached it.
 */
static int test_consecutive_snapshots(void) {
  reset_config();
  init_memory_system(0);
  set_cosmology();
  test_pre_timestep_add(CLOCK, PROCESSING_MODE_FULL_HALO);
  test_post_timestep_add(RECORDER, PROCESSING_MODE_FULL_HALO);
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

  // The clock reads PreviousType: only 0 -> 1 and non-main 0 -> 2 record an infall
  TEST_ASSERT(a->galaxy->TimeOfInfallState == 41.0f && a->galaxy->infallStellarMassState == 2.0f,
              "0 -> 1: the infall is recorded at the snapshot with the stellar mass");
  TEST_ASSERT(d->galaxy->infallStellarMassState == 0.9f && d->galaxy->TimeOfInfallState == 30.0f,
              "0 -> 2: infallStellarMass recorded, the earlier TimeOfInfall kept");
  TEST_ASSERT(b->galaxy->infallStellarMassState == 0.7f && b->galaxy->TimeOfInfallState == 33.0f,
              "1 -> 1: the infall state is kept");
  TEST_ASSERT(c->galaxy->infallStellarMassState == 0.5f && c->galaxy->TimeOfInfallState == 35.0f,
              "1 -> 0: the infall state is kept");
  TEST_ASSERT(e->galaxy->infallStellarMassState == 0.3f && e->galaxy->TimeOfInfallState == 36.0f,
              "1 -> 2: the infall state is kept");
  TEST_ASSERT(f->galaxy->infallStellarMassState == 0.1f && f->galaxy->TimeOfInfallState == 31.0f,
              "2 -> 2: the infall state is untouched");

  // The end of snapshot 41: the written values and the Types recorded for snapshot 42
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

  // A new FoF: the core creates a Type 0 with no previous Type; the clock leaves it be
  struct Halo ws_new[1];
  memset(ws_new, 0, sizeof(ws_new));
  const struct InheritanceDescendant halo30 = make_descendant(30, 300, 25.0, 1, z41);
  const int64_t end_new = inherit_descendant_halos(pool, ws_new, 0, 1, &halo30, NULL, 0);
  TEST_ASSERT(end_new == 1 && ws_new[0].Type == 0 && ws_new[0].galaxy->PreviousType == -1,
              "a new galaxy is a Type 0 without a previous Type");
  TEST_ASSERT(sage26_initialise_merger_clock_process(&ctx41, ws_new, 1) == 0, "clock runs");
  TEST_ASSERT(ws_new[0].galaxy->infallStellarMassState == -1.0f &&
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

  TEST_RUN(test_records_final_type);
  TEST_RUN(test_consecutive_snapshots);

  TEST_SUMMARY();
  return TEST_RESULT();
}
