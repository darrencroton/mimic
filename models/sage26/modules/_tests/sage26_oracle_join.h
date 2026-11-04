/**
 * @file    sage26_oracle_join.h
 * @brief   Progenitor joining for the SAGE26 oracle tests: the core's inheritance, then the
 *          package's joining modules
 *
 * SAGE26 joins a halo's progenitor galaxies in join_galaxies_of_progenitors
 * (R/src/core_build_model.c:210-372, B below; R = the staged SAGE26 source). In Mimic the
 * core joins them (inherit_descendant_halos, src/core/inheritance.c) and the package makes
 * SAGE26's remaining joining writes in pre_timestep, in the shipped run files' order:
 * sage26_initialise_merger_clock (the history reset, the clock and the infall state), then
 * sage26_set_disk_scale_radius (the FoF central's disk radius). oracle_join() composes them
 * on a fixture's progenitor galaxies (oracle README "The c06 Group", join_progenitors and
 * join_and_pass), so an oracle test can assert the joined state and run the merger pass on
 * it.
 *
 * Used by the oracle tests of sage26_initialise_merger_clock (join_progenitors and its
 * join_and_pass probe) and sage26_resolve_mergers_and_disruption (join_and_pass).
 */

#ifndef SAGE26_ORACLE_JOIN_H
#define SAGE26_ORACLE_JOIN_H

#include "core/galaxy_pool.h"
#include "core/inheritance.h"
#include "core/module_interface.h"
#include "core/virial.h"
#include "include/globals.h"
#include "include/types.h"

#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage26_oracle_fixture.h"

extern int sage26_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos,
                                                  int ngal);
extern int sage26_set_disk_scale_radius_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal);

#define ORACLE_JOIN_MAX_GALAXIES 8
#define ORACLE_JOIN_WORKSPACE_ROWS (ORACLE_JOIN_MAX_GALAXIES + 1)

/** @brief The joined workspace and what each retained row came from */
struct oracle_join_result {
  struct Halo workspace[ORACLE_JOIN_WORKSPACE_ROWS];
  struct GalaxyData standin_galaxy;
  int start;                                    /* first row the descendant's galaxies occupy */
  int end;                                      /* one past the last */
  int source_type[ORACLE_JOIN_MAX_GALAXIES];    /* each retained row's progenitor Type */
  float source_clock[ORACLE_JOIN_MAX_GALAXIES]; /* each retained row's progenitor MergTime */
};

/** @brief Name of a catalogue entry, "halo[<h>].<member>" */
static inline const char *oracle_join_halo_field(const int h, const char *member) {
  static char buffer[ORACLE_MAX_NAME];
  snprintf(buffer, sizeof(buffer), "halo[%d].%s", h, member);
  return buffer;
}

/**
 * @brief The core's get_virial_mass (src/core/virial.c:47-54) on the fixture catalogue
 *
 * The catalogue mass for a FoF's first halo when it is not negative (the tree's float
 * widened, as mimic_tree_get_HaloMass returns it), Len * PartMass otherwise; SAGE26's
 * rule (R/src/model_halo_properties.c:31-37). The core's input view has no setter for the
 * catalogue mass, so the rule is restated here.
 */
static inline double oracle_join_core_virial_mass(struct oracle_fixture *in, const int h) {
  const int fof = oracle_in_i32(in, oracle_join_halo_field(h, "FirstHaloInFOFgroup"));
  const double halo_mass = oracle_in_f32(in, oracle_join_halo_field(h, "Mvir"));
  const int len = oracle_in_i32(in, oracle_join_halo_field(h, "Len"));
  if (h == fof && halo_mass >= 0.0) {
    return halo_mass;
  }
  return len * MimicConfig.PartMass;
}

/** @brief The redshift of a catalogue halo's snapshot, params.ZZ[<SnapNum>] */
static inline double oracle_join_halo_redshift(struct oracle_fixture *in, const int h) {
  char name[ORACLE_MAX_NAME];
  snprintf(name, sizeof(name), "params.ZZ[%d]",
           oracle_in_i32(in, oracle_join_halo_field(h, "SnapNum")));
  return oracle_in_f64(in, name);
}

/**
 * @brief Build the progenitor rows, join them with the core's inheritance service, and run
 *        the package's joining modules
 *
 * A progenitor whose mergeType is not 0 is a galaxy that has merged: the package holds it
 * as Type 3, and the core drops it (SAGE26 marks it Type 3 and skips it, B:242-245). Each
 * progenitor's PreviousType is its Type, as sage26_record_transition_state wrote it at the
 * end of its snapshot. Each progenitor galaxy starts from the package's creation defaults
 * and takes the fixture's clock, infall record, histories and, with full_state, every
 * float of the c06 galaxy block; its halo takes the fixture's halo values.
 *
 * With full_state the rows also take the pass's timing: each progenitor's source time is
 * call.Age[<its SnapNum>] and the descendant's call.halo_age, so the core's dT is SAGE26's
 * deltaT = Age[SnapNum] - halo_age (B:529), and ctx->time is call.halo_age. Without it
 * both times are 0 (the joining reads neither).
 *
 * @return Number of setup problems (0 on success)
 */
static inline int oracle_join(struct oracle_fixture *in, struct GalaxyPool *pool,
                              struct oracle_join_result *result, struct ModuleContext *ctx,
                              const int full_state) {
  const int ngal = oracle_in_i32(in, "call.ngal");
  const int halonr = oracle_in_i32(in, "call.halonr");
  const int first_occupied = oracle_in_i32(in, "call.first_occupied");
  if (ngal < 0 || ngal > ORACLE_JOIN_MAX_GALAXIES) {
    fprintf(stderr, "oracle: %s: %d galaxies exceed the test's %d\n", in->path, ngal,
            ORACLE_JOIN_MAX_GALAXIES);
    return 1;
  }
  const double halo_age = full_state ? oracle_in_f64(in, "call.halo_age") : 0.0;

  static struct Halo sources[ORACLE_JOIN_MAX_GALAXIES];
  static struct GalaxyData source_galaxies[ORACLE_JOIN_MAX_GALAXIES];
  struct InheritanceProgenitorGalaxy progenitors[ORACLE_JOIN_MAX_GALAXIES];
  int source_type_all[ORACLE_JOIN_MAX_GALAXIES];
  float source_clock_all[ORACLE_JOIN_MAX_GALAXIES];
  int problems = 0;

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
    problems += oracle_c05_in_histories(in, k, g);
    if (full_state) {
      size_t count;
      const char *const *names = oracle_c06_block_names(&count);
      for (size_t n = 0; n < count; n++) {
        float *member = oracle_c06_galaxy_float(g, names[n]);
        if (member == NULL) {
          fprintf(stderr, "oracle: %s: no package member for '%s'\n", in->path, names[n]);
          problems++;
          continue;
        }
        *member = oracle_in_f32(in, oracle_galaxy_field(k, names[n]));
      }
      g->Cooling = oracle_in_f64(in, oracle_galaxy_field(k, "Cooling"));
      g->Heating = oracle_in_f64(in, oracle_galaxy_field(k, "Heating"));
    }
    source_type_all[k] = type;
    source_clock_all[k] = g->MergTime;

    char prog_name[ORACLE_MAX_NAME];
    snprintf(prog_name, sizeof(prog_name), "call.galaxy[%d].prog", k);
    const int prog = oracle_in_i32(in, prog_name);
    h->HaloNr = prog;
    progenitors[k].source = h;
    progenitors[k].source_time = 0.0;
    if (full_state) {
      char age_name[ORACLE_MAX_NAME];
      snprintf(age_name, sizeof(age_name), "call.Age[%d]", h->SnapNum);
      progenitors[k].source_time = oracle_in_f64(in, age_name);
    }
    progenitors[k].is_main_branch = (prog == first_occupied);
  }
  if (problems != 0) {
    return problems;
  }

  // The descendant as the core's driver describes it: the virial values of the
  // halo's current catalogue entry (get_virial_mass/radius/velocity)
  struct InheritanceDescendant descendant;
  memset(&descendant, 0, sizeof(descendant));
  const int fof = oracle_in_i32(in, oracle_join_halo_field(halonr, "FirstHaloInFOFgroup"));
  const int snap = oracle_in_i32(in, oracle_join_halo_field(halonr, "SnapNum"));
  const double redshift = oracle_join_halo_redshift(in, halonr);
  descendant.halo_nr = halonr;
  descendant.current_snap = snap;
  descendant.current_time = halo_age;
  descendant.virial_mass = oracle_join_core_virial_mass(in, halonr);
  descendant.virial_radius = virial_radius_for_mass(descendant.virial_mass, redshift);
  descendant.virial_velocity =
      virial_velocity_for(descendant.virial_mass, descendant.virial_radius);
  descendant.is_fof_central = (halonr == fof);
  descendant.halo_payload.Len = oracle_in_i32(in, oracle_join_halo_field(halonr, "Len"));
  descendant.halo_payload.Vmax = oracle_in_f32(in, oracle_join_halo_field(halonr, "Vmax"));
  for (int j = 0; j < 3; j++) {
    char spin[ORACLE_MAX_NAME];
    snprintf(spin, sizeof(spin), "Spin[%d]", j);
    if (oracle_has(in, oracle_join_halo_field(halonr, spin))) {
      descendant.halo_payload.Spin[j] = oracle_in_f32(in, oracle_join_halo_field(halonr, spin));
    }
  }

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
    central->Len = oracle_in_i32(in, oracle_join_halo_field(fof, "Len"));
    central->Mvir = oracle_join_core_virial_mass(in, fof);
    central->CentralHalo = 0;
    central->galaxy = &result->standin_galaxy;
  }

  const int64_t end =
      inherit_descendant_halos(pool, result->workspace, result->start, ORACLE_JOIN_WORKSPACE_ROWS,
                               &descendant, progenitors, ngal);
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

  // The package's joining writes, in the shipped run files' pre_timestep order
  memset(ctx, 0, sizeof(*ctx));
  ctx->params = &MimicConfig;
  ctx->redshift = redshift;
  ctx->time = halo_age;
  ctx->snapshot_number = snap;
  ctx->num_substeps = 1;
  ctx->central_galaxy = &result->workspace[0];
  if (sage26_initialise_merger_clock_process(ctx, result->workspace, result->end) != 0) {
    fprintf(stderr, "oracle: %s: the clock module's process() failed\n", in->path);
    return 1;
  }
  if (full_state &&
      sage26_set_disk_scale_radius_process(ctx, result->workspace, result->end) != 0) {
    fprintf(stderr, "oracle: %s: the disk-radius module's process() failed\n", in->path);
    return 1;
  }
  return 0;
}

#endif /* SAGE26_ORACLE_JOIN_H */
