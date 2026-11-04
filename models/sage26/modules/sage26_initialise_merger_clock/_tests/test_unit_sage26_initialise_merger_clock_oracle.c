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
 *   makes the package's joining writes. Each retained galaxy is asserted on the
 *   package's fields (MergTime, and infallStellarMass and TimeOfInfall through the
 *   private state properties that hold SAGE26's members) and, as evidence of the
 *   composition, on the core's (Type, SnapNum, HaloNr, Len, Mvir, deltaMvir, Rvir,
 *   Vvir, Vmax, infallMvir, infallVvir, infallVmax, each SAGE26 float compared
 *   with the core's double narrowed to float) and on StellarMass and ColdGas,
 *   which the clock reads.
 * - join_and_pass (tr_jp_probe_already_merged_trailing, owned here by PM decision
 *   PM-D3): the joining as above, then the package's merger pass
 *   (sage26_resolve_mergers_and_disruption) at the fixture's substep, asserting
 *   the joined state (intermediate.join.galaxy[*]) and the end state (galaxy[*]).
 *
 * Fields excluded from the joining assertions, by reason (each consumed by name):
 * - core-boundary: an orphan's Len (the core zeros it, SAGE26 keeps it,
 *   src/core/inheritance.c:86) and the infall halo values of a Type 1 with a clock
 *   above 999 that loses its subhalo (SAGE26 writes them, B:327-333; the core
 *   writes them only for a previous Type 0, src/core/inheritance.c:88-92);
 * - other owners: the history, Cooling, Heating, QuasarModeBHaccretionMass and
 *   OutflowRate resets (the core's init_repeat; the orphan exception is Slice 5's),
 *   the central's disk and bulge radii (sage26_set_disk_scale_radius), and the
 *   reservoirs joining copies unchanged;
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
#include "core/inheritance.h"
#include "core/module_interface.h"
#include "core/module_registry.h"
#include "core/virial.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"
#include "util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage_test_fixtures.h"
#include "modules/_tests/sage26_oracle_fixture.h"
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

#define MAX_JOIN_GALAXIES 8
#define WORKSPACE_ROWS (MAX_JOIN_GALAXIES + 1)

// ============================================================================
// FIXTURE HELPERS
// ============================================================================

/** @brief Name of a catalogue entry, "halo[<h>].<member>" */
static const char *halo_field(const int h, const char *member) {
  static char buffer[ORACLE_MAX_NAME];
  snprintf(buffer, sizeof(buffer), "halo[%d].%s", h, member);
  return buffer;
}

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

/**
 * @brief The core's get_virial_mass (src/core/virial.c:47-54) on the fixture catalogue
 *
 * The catalogue mass for a FoF's first halo when it is not negative (the tree's float
 * widened, as mimic_tree_get_HaloMass returns it), Len * PartMass otherwise; SAGE26's
 * rule (R/src/model_halo_properties.c:31-37).
 */
static double core_virial_mass(struct oracle_fixture *in, const int h) {
  const int fof = oracle_in_i32(in, halo_field(h, "FirstHaloInFOFgroup"));
  const double halo_mass = oracle_in_f32(in, halo_field(h, "Mvir"));
  const int len = oracle_in_i32(in, halo_field(h, "Len"));
  if (h == fof && halo_mass >= 0.0) {
    return halo_mass;
  }
  return len * MimicConfig.PartMass;
}

/** @brief The redshift of a catalogue halo's snapshot, params.ZZ[<SnapNum>] */
static double halo_redshift(struct oracle_fixture *in, const int h) {
  char name[ORACLE_MAX_NAME];
  snprintf(name, sizeof(name), "params.ZZ[%d]", oracle_in_i32(in, halo_field(h, "SnapNum")));
  return oracle_in_f64(in, name);
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
  satellite->Len = oracle_in_i32(&in, halo_field(sat_halo, "Len"));
  satellite->Mvir = core_virial_mass(&in, sat_halo);
  satellite->galaxy = &sat_gal;
  mother->HaloNr = mother_halo;
  mother->Len = oracle_in_i32(&in, halo_field(mother_halo, "Len"));
  mother->Mvir = core_virial_mass(&in, mother_halo);
  mother->galaxy = NULL;

  struct MimicSageMergerClockTrace trace;
  const double mergtime = mimic_sage_estimate_merging_time(
      satellite, mother, halo_redshift(&in, mother_halo),
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

/** @brief The joined workspace and what each retained row came from */
struct JoinResult {
  struct Halo workspace[WORKSPACE_ROWS];
  struct GalaxyData standin_galaxy;
  int start;                             /* first row the descendant's galaxies occupy */
  int end;                               /* one past the last */
  int source_type[MAX_JOIN_GALAXIES];    /* each retained row's progenitor Type */
  float source_clock[MAX_JOIN_GALAXIES]; /* each retained row's progenitor MergTime */
};

/**
 * @brief Build the progenitor rows, join them with the core's inheritance service, run the module
 *
 * A progenitor whose mergeType is not 0 is a galaxy that has merged: the package
 * holds it as Type 3, and the core drops it (SAGE26 marks it Type 3 and skips it,
 * B:242-245). Each progenitor's PreviousType is its Type, as
 * sage26_record_transition_state wrote it at the end of its snapshot.
 *
 * @return Number of setup problems (0 on success)
 */
static int join(struct oracle_fixture *in, struct GalaxyPool *pool, struct JoinResult *result,
                struct ModuleContext *ctx) {
  const int ngal = oracle_in_i32(in, "call.ngal");
  const int halonr = oracle_in_i32(in, "call.halonr");
  const int first_occupied = oracle_in_i32(in, "call.first_occupied");
  if (ngal < 0 || ngal > MAX_JOIN_GALAXIES) {
    fprintf(stderr, "oracle: %s: %d galaxies exceed the test's %d\n", in->path, ngal,
            MAX_JOIN_GALAXIES);
    return 1;
  }

  static struct Halo sources[MAX_JOIN_GALAXIES];
  static struct GalaxyData source_galaxies[MAX_JOIN_GALAXIES];
  struct InheritanceProgenitorGalaxy progenitors[MAX_JOIN_GALAXIES];
  int source_type_all[MAX_JOIN_GALAXIES];
  float source_clock_all[MAX_JOIN_GALAXIES];

  for (int k = 0; k < ngal; k++) {
    struct Halo *h = &sources[k];
    struct GalaxyData *g = &source_galaxies[k];
    memset(h, 0, sizeof(*h));
    init_galaxy_defaults(g);
    h->galaxy = g;

    int type = oracle_in_i32(in, oracle_galaxy_field(k, "Type"));
    const int merge_type = oracle_in_i32(in, oracle_galaxy_field(k, "mergeType"));
    if (merge_type != 0 && (type == 0 || type == 1)) {
      type = 3;
    }
    h->Type = type;
    g->PreviousType = type;
    h->SnapNum = oracle_in_i32(in, oracle_galaxy_field(k, "SnapNum"));
    h->Len = oracle_in_i32(in, oracle_galaxy_field(k, "Len"));
    h->Mvir = oracle_in_f32(in, oracle_galaxy_field(k, "Mvir"));
    h->deltaMvir = oracle_in_f32(in, oracle_galaxy_field(k, "deltaMvir"));
    h->Rvir = oracle_in_f32(in, oracle_galaxy_field(k, "Rvir"));
    h->Vvir = oracle_in_f32(in, oracle_galaxy_field(k, "Vvir"));
    h->Vmax = oracle_in_f32(in, oracle_galaxy_field(k, "Vmax"));
    h->infallMvir = oracle_in_f32(in, oracle_galaxy_field(k, "infallMvir"));
    h->infallVvir = oracle_in_f32(in, oracle_galaxy_field(k, "infallVvir"));
    h->infallVmax = oracle_in_f32(in, oracle_galaxy_field(k, "infallVmax"));
    g->MergTime = oracle_in_f32(in, oracle_galaxy_field(k, "MergTime"));
    g->StellarMass = oracle_in_f32(in, oracle_galaxy_field(k, "StellarMass"));
    g->ColdGas = oracle_in_f32(in, oracle_galaxy_field(k, "ColdGas"));
    g->infallStellarMassState = oracle_in_f32(in, oracle_galaxy_field(k, "infallStellarMass"));
    g->TimeOfInfallState = oracle_in_f32(in, oracle_galaxy_field(k, "TimeOfInfall"));
    source_type_all[k] = type;
    source_clock_all[k] = g->MergTime;

    char prog_name[ORACLE_MAX_NAME];
    snprintf(prog_name, sizeof(prog_name), "call.galaxy[%d].prog", k);
    const int prog = oracle_in_i32(in, prog_name);
    h->HaloNr = prog;
    progenitors[k].source = h;
    progenitors[k].source_time = 0.0;
    progenitors[k].is_main_branch = (prog == first_occupied);
  }

  // The descendant as the core's driver describes it: the virial values of the
  // halo's current catalogue entry (get_virial_mass/radius/velocity)
  struct InheritanceDescendant descendant;
  memset(&descendant, 0, sizeof(descendant));
  const int fof = oracle_in_i32(in, halo_field(halonr, "FirstHaloInFOFgroup"));
  const int snap = oracle_in_i32(in, halo_field(halonr, "SnapNum"));
  const double redshift = halo_redshift(in, halonr);
  descendant.halo_nr = halonr;
  descendant.current_snap = snap;
  descendant.virial_mass = core_virial_mass(in, halonr);
  descendant.virial_radius = virial_radius_for_mass(descendant.virial_mass, redshift);
  descendant.virial_velocity =
      virial_velocity_for(descendant.virial_mass, descendant.virial_radius);
  descendant.is_fof_central = (halonr == fof);
  descendant.halo_payload.Len = oracle_in_i32(in, halo_field(halonr, "Len"));
  descendant.halo_payload.Vmax = oracle_in_f32(in, halo_field(halonr, "Vmax"));

  // A subhalo's FoF central row, which the FoF workspace always holds (the mother halo
  // of the clock, B:317): a stand-in Type 0 at row 0
  result->start = descendant.is_fof_central ? 0 : 1;
  memset(result->workspace, 0, sizeof(result->workspace));
  if (!descendant.is_fof_central) {
    struct Halo *central = &result->workspace[0];
    init_galaxy_defaults(&result->standin_galaxy);
    result->standin_galaxy.PreviousType = 0;
    central->Type = 0;
    central->HaloNr = fof;
    central->Len = oracle_in_i32(in, halo_field(fof, "Len"));
    central->Mvir = core_virial_mass(in, fof);
    central->CentralHalo = 0;
    central->galaxy = &result->standin_galaxy;
  }

  const int64_t end = inherit_descendant_halos(pool, result->workspace, result->start,
                                               WORKSPACE_ROWS, &descendant, progenitors, ngal);
  result->end = (int)end;

  int row = 0;
  for (int k = 0; k < ngal; k++) {
    if (source_type_all[k] == 3) {
      continue;
    }
    result->source_type[row] = source_type_all[k];
    result->source_clock[row] = source_clock_all[k];
    row++;
  }
  if (row != result->end - result->start) {
    fprintf(stderr, "oracle: %s: %d retained progenitors but %d joined rows\n", in->path, row,
            result->end - result->start);
    return 1;
  }

  memset(ctx, 0, sizeof(*ctx));
  ctx->params = &MimicConfig;
  ctx->redshift = redshift;
  ctx->snapshot_number = snap;
  ctx->num_substeps = 1;
  ctx->central_galaxy = &result->workspace[0];
  if (sage26_initialise_merger_clock_process(ctx, result->workspace, result->end) != 0) {
    fprintf(stderr, "oracle: %s: process() failed\n", in->path);
    return 1;
  }
  return 0;
}

/**
 * @brief Assert one joined galaxy row against the fields "<prefix>galaxy[<j>].*"
 */
static int check_joined_galaxy(struct oracle_fixture *out, const char *prefix, const int j,
                               const struct Halo *row, const int source_type,
                               const float source_clock) {
  const struct GalaxyData *g = row->galaxy;
  const int newly_orphaned = (row->Type == 2 && (source_type == 0 || source_type == 1));
  int problems = 0;

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
  static struct JoinResult result;
  struct ModuleContext ctx;
  problems += join(&in, pool, &result, &ctx);

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
  if (oracle_set_model_param("ThresholdMajorMerger",
                             oracle_in_f64(&in, "params.ThreshMajorMerger")) != 0 ||
      oracle_set_model_param("ThresholdSatDisruption",
                             oracle_in_f64(&in, "params.ThresholdSatDisruption")) != 0) {
    problems++;
  }
  if (problems == 0 && sage26_resolve_mergers_and_disruption_init() != 0) {
    fprintf(stderr, "oracle: %s: resolver init failed\n", name);
    problems++;
  }

  struct GalaxyPool *pool = galaxy_pool_create(16);
  static struct JoinResult result;
  struct ModuleContext ctx;
  if (problems == 0) {
    problems += join(&in, pool, &result, &ctx);
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
