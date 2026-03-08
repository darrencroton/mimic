#ifndef MIMIC_SHARED_TIME_PARITY_H
#define MIMIC_SHARED_TIME_PARITY_H

#include "module_interface.h"
#include "types.h"

/*
 * Object-local timestep status for SAGE parity operations.
 * - OK: valid positive per-object timestep.
 * - SKIP_INITIAL: first-snapshot boundary object (SnapNum < 0, dT <= 0), no-op.
 * - INVALID: inconsistent state that should fail fast.
 */
enum MimicObjectTimeStatus {
  MIMIC_OBJECT_TIME_OK = 0,
  MIMIC_OBJECT_TIME_SKIP_INITIAL = 1,
  MIMIC_OBJECT_TIME_INVALID = -1
};

static inline const char *mimic_object_time_status_str(
    enum MimicObjectTimeStatus status) {
  switch (status) {
  case MIMIC_OBJECT_TIME_OK:
    return "OK";
  case MIMIC_OBJECT_TIME_SKIP_INITIAL:
    return "SKIP_INITIAL";
  default:
    return "INVALID";
  }
}

/*
 * Derive per-object substep dt = halo->dT / ctx->num_substeps with strict
 * boundary handling:
 * - SnapNum < 0 and dT <= 0: initial boundary sentinel (skip/no-op).
 * - Non-boundary dT <= 0: invalid state.
 */
static inline enum MimicObjectTimeStatus mimic_object_substep_dt(
    const struct Halo *halo, const struct ModuleContext *ctx, double *dt_out) {
  if (dt_out != NULL) {
    *dt_out = 0.0;
  }

  if (halo == NULL || ctx == NULL || dt_out == NULL || ctx->num_substeps <= 0) {
    return MIMIC_OBJECT_TIME_INVALID;
  }

  if (halo->dT > 0.0f) {
    *dt_out = (double)halo->dT / (double)ctx->num_substeps;
    return MIMIC_OBJECT_TIME_OK;
  }

  if (halo->SnapNum < 0 && halo->dT <= 0.0f) {
    return MIMIC_OBJECT_TIME_SKIP_INITIAL;
  }

  return MIMIC_OBJECT_TIME_INVALID;
}

/*
 * Derive SAGE-equivalent per-object substep midpoint time:
 *   Age[obj] - (substep + 0.5) * (dT_obj / num_substeps)
 * with Age[obj] = ctx->time + halo->dT.
 */
static inline enum MimicObjectTimeStatus mimic_object_substep_time(
    const struct Halo *halo, const struct ModuleContext *ctx, double *time_out) {
  double dt = 0.0;
  enum MimicObjectTimeStatus dt_status;

  if (time_out != NULL) {
    *time_out = 0.0;
  }

  if (ctx == NULL || time_out == NULL || ctx->substep_number < 0 ||
      ctx->substep_number >= ctx->num_substeps) {
    return MIMIC_OBJECT_TIME_INVALID;
  }

  dt_status = mimic_object_substep_dt(halo, ctx, &dt);
  if (dt_status != MIMIC_OBJECT_TIME_OK) {
    return dt_status;
  }

  *time_out = (ctx->time + (double)halo->dT) -
              ((double)ctx->substep_number + 0.5) * dt;
  return MIMIC_OBJECT_TIME_OK;
}

#endif /* MIMIC_SHARED_TIME_PARITY_H */
