#include <assert.h>
#include <string.h>

#include "inheritance.h"
#include "galaxy_pool.h"
#include "error.h"

static void copy_progenitor_galaxy(struct Halo *target, const struct Halo *source) {
  *target = *source;

  if (source->galaxy != NULL) {
    target->galaxy = galaxy_pool_alloc();
    memcpy(target->galaxy, source->galaxy, sizeof(struct GalaxyData));
    reset_galaxy_snapshot_accumulators(target->galaxy);
  } else {
    target->galaxy = NULL;
  }
}

/* The pool owns galaxy memory and reclaims it on reset, so discarding an
 * inherited galaxy is just clearing the halo's pointer. */
static void discard_inherited_galaxy(struct Halo *halo) { halo->galaxy = NULL; }

static void apply_descendant_properties(struct Halo *halo,
                                        const struct InheritanceDescendant *descendant) {
  int j;
  double previous_mvir = halo->Mvir;
  double previous_vvir = halo->Vvir;
  double previous_vmax = halo->Vmax;

  halo->MostBoundID = descendant->halo_payload.MostBoundID;

  for (j = 0; j < 3; j++) {
    halo->Pos[j] = descendant->halo_payload.Pos[j];
    halo->Vel[j] = descendant->halo_payload.Vel[j];
    halo->Spin[j] = descendant->halo_payload.Spin[j];
  }

  halo->Len = descendant->halo_payload.Len;
  halo->Vmax = descendant->halo_payload.Vmax;
  halo->VelDisp = descendant->halo_payload.VelDisp;
  halo->deltaMvir = descendant->virial_mass - halo->Mvir;

  if (descendant->virial_mass > halo->Mvir) {
    halo->Rvir = descendant->virial_radius;
    halo->Vvir = descendant->virial_velocity;
  }
  halo->Mvir = descendant->virial_mass;

  if (descendant->is_fof_central) {
    halo->Type = 0;
  } else {
    if (halo->Type == 0) {
      halo->infallMvir = previous_mvir;
      halo->infallVvir = previous_vvir;
      halo->infallVmax = previous_vmax;
    }

    halo->Type = 1;
  }
}

static void make_orphan(struct Halo *halo) {
  double previous_mvir = halo->Mvir;
  double previous_vvir = halo->Vvir;
  double previous_vmax = halo->Vmax;

  halo->deltaMvir = -1.0 * halo->Mvir;
  halo->Mvir = 0.0;
  halo->Len = 0;

  if (halo->Type == 0) {
    halo->infallMvir = previous_mvir;
    halo->infallVvir = previous_vvir;
    halo->infallVmax = previous_vmax;
  }

  halo->Type = 2;
}

static void init_new_halo(struct Halo *halo, const struct InheritanceDescendant *descendant) {
  init_halo_from_payload(halo, &descendant->halo_payload);

  halo->HaloNr = descendant->halo_nr;
  halo->SnapNum = descendant->current_snap - 1;
  halo->dT = descendant->new_halo_dt;
  halo->UniqueGalaxyID = descendant->unique_galaxy_id;
  halo->galaxy = galaxy_pool_alloc();
  init_galaxy_defaults(halo->galaxy);
}

static void set_local_centrals(struct Halo *workspace, int start, int end) {
  int i, centralgal, ncentrals;

  if (end <= start) {
    return;
  }

  centralgal = -1;
  ncentrals = 0;
  for (i = start; i < end; i++) {
    if (workspace[i].Type == 0 || workspace[i].Type == 1) {
      ncentrals++;
      if (ncentrals > 1) {
        ERROR_LOG("FATAL: Multiple Type 0/1 centrals found in subhalo slice "
                  "(range %d-%d, first=%d, second=%d)",
                  start, end, centralgal, i);
        assert(ncentrals == 1);
      }
      centralgal = i;
    }
  }

  if (centralgal == -1) {
    ERROR_LOG("FATAL: No Type 0/1 central found in subhalo slice (range %d-%d)", start, end);
    for (i = start; i < end; i++) {
      ERROR_LOG("  Galaxy %d: Type=%d, HaloNr=%d", i, workspace[i].Type, workspace[i].HaloNr);
    }
    assert(centralgal != -1);
  }

  for (i = start; i < end; i++) {
    workspace[i].CentralHalo = centralgal;
  }
}

int inherit_descendant_halos(struct Halo *workspace, int start, int capacity,
                             const struct InheritanceDescendant *descendant,
                             const struct InheritanceProgenitorGalaxy *progenitors,
                             int nprogenitors) {
  int end = start;

  assert(workspace != NULL);
  assert(descendant != NULL);
  assert(start >= 0);
  assert(capacity >= start);
  assert(nprogenitors >= 0);
  assert(nprogenitors == 0 || progenitors != NULL);

  for (int i = 0; i < nprogenitors; i++) {
    assert(end < capacity);
    assert(progenitors[i].source != NULL);

    copy_progenitor_galaxy(&workspace[end], progenitors[i].source);
    workspace[end].HaloNr = descendant->halo_nr;
    workspace[end].dT = progenitors[i].source_time - descendant->current_time;

    if (workspace[end].Type == 3) {
      discard_inherited_galaxy(&workspace[end]);
      continue;
    }

    if (workspace[end].Type == 0 || workspace[end].Type == 1) {
      if (progenitors[i].is_main_branch) {
        apply_descendant_properties(&workspace[end], descendant);
      } else {
        make_orphan(&workspace[end]);
      }
    }

    end++;
  }

  if (end == start && descendant->is_fof_central) {
    assert(end < capacity);
    init_new_halo(&workspace[end], descendant);
    end++;
  }

  if (end > start) {
    set_local_centrals(workspace, start, end);
  }

  return end;
}
