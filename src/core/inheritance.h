#ifndef CORE_INHERITANCE_H
#define CORE_INHERITANCE_H

#include "types.h"

struct GalaxyPool; /* opaque; defined in galaxy_pool.c */

struct InheritanceDescendant {
  /* Driver-supplied identity, time, and descendant halo properties. halo_nr is
   * an index into the driver's input view, so it is as wide as struct
   * Halo.HaloNr. */
  int64_t halo_nr;
  int current_snap;
  double current_time;
  double new_halo_dt;
  double virial_mass;
  double virial_radius;
  double virial_velocity;
  int is_fof_central;
  long long unique_galaxy_id;
  struct HaloInitPayload halo_payload;
};

struct InheritanceProgenitorGalaxy {
  /* Source points to driver-owned processed state; inheritance deep-copies it. */
  const struct Halo *source;
  double source_time;
  int is_main_branch;
};

/*
 * Build the FoF workspace rows [start, return value) for one descendant
 * subhalo. The caller owns workspace capacity, progenitor lookup, and the
 * galaxy pool `pool` that every inherited or newly created galaxy is
 * allocated from.
 *
 * Precondition: `pool` must not be NULL; every caller owns a live pool before
 * calling this function, so unlike `free_unit_halos()`, NULL is not a legal
 * "no galaxies allocated" signal here.
 *
 * Precondition: `capacity` must be large enough for every halo this call can
 * produce, i.e. capacity >= start + nprogenitors (+1 when a new central object
 * is created because no progenitor galaxy survives). The caller (the driver
 * gather step) is responsible for pre-sizing the workspace; this function only
 * asserts the bound and never grows `workspace`. Bounds are enforced with
 * assert(), so callers must satisfy the precondition rather than rely on
 * runtime growth here.
 *
 * Workspace offsets and counts are int64_t, like the halo indices the drivers
 * compute them from; the one int consumer, struct Halo.CentralHalo, is written
 * through a checked narrowing.
 */
int64_t inherit_descendant_halos(struct GalaxyPool *pool, struct Halo *workspace, int64_t start,
                                 int64_t capacity, const struct InheritanceDescendant *descendant,
                                 const struct InheritanceProgenitorGalaxy *progenitors,
                                 int64_t nprogenitors);

/*
 * Turn a Type 0 or 1 row into a Type 2 orphan of the halo it occupies: Mvir and
 * Len become zero, deltaMvir becomes -Mvir, Rvir and Vvir are kept, and a Type 0
 * row records its Mvir, Vvir and Vmax as the infall values (a Type 1 row keeps
 * the infall values it already has). Every other field, including the galaxy
 * pointer, is left unchanged.
 *
 * Used by inheritance for a non-main-branch progenitor, and by
 * module_create_record() (module_registry.c) to initialise a created record
 * from a struct copy of its host.
 */
void make_orphan(struct Halo *halo);

#endif /* CORE_INHERITANCE_H */
