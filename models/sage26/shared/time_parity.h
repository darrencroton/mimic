/**
 * @file time_parity.h
 * @brief Per-object substep timestep and midpoint time helpers for SAGE parity
 *
 * Provides substep dt and midpoint time calculation with strict boundary handling
 * matching SAGE's initial-snapshot sentinel and multi-substep loop. Used by
 * modules that accumulate rates or apply physics at substep midpoints.
 */

#ifndef MIMIC_SHARED_TIME_PARITY_H
#define MIMIC_SHARED_TIME_PARITY_H

#include <stddef.h>

#include "module_interface.h"
#include "types.h"

/**
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

static inline const char *mimic_object_time_status_str(enum MimicObjectTimeStatus status) {
  switch (status) {
  case MIMIC_OBJECT_TIME_OK:
    return "OK";
  case MIMIC_OBJECT_TIME_SKIP_INITIAL:
    return "SKIP_INITIAL";
  default:
    return "INVALID";
  }
}

/**
 * @brief Derive per-object substep dt = halo->dT / ctx->num_substeps
 *
 * Strict boundary handling: SnapNum < 0 with dT <= 0 is the initial boundary sentinel
 * (returns SKIP_INITIAL); non-boundary dT <= 0 is an invalid state (returns INVALID).
 *
 * @param halo    Halo with dT and SnapNum fields
 * @param ctx     Module context with num_substeps
 * @param dt_out  Output: substep dt (0.0 on non-OK status); must not be NULL
 * @return Status indicating OK, SKIP_INITIAL, or INVALID
 */
static inline enum MimicObjectTimeStatus
mimic_object_substep_dt(const struct Halo *halo, const struct ModuleContext *ctx, double *dt_out) {
  if (dt_out != NULL) {
    *dt_out = 0.0;
  }

  if (halo == NULL || ctx == NULL || dt_out == NULL || ctx->num_substeps <= 0) {
    return MIMIC_OBJECT_TIME_INVALID;
  }

  if (halo->dT > 0.0) {
    *dt_out = (double)halo->dT / (double)ctx->num_substeps;
    return MIMIC_OBJECT_TIME_OK;
  }

  if (halo->SnapNum < 0 && halo->dT <= 0.0) {
    return MIMIC_OBJECT_TIME_SKIP_INITIAL;
  }

  return MIMIC_OBJECT_TIME_INVALID;
}

/**
 * @brief Derive SAGE-equivalent substep midpoint time
 *
 * Computes: Age[obj] - (substep + 0.5) * (dT_obj / num_substeps),
 * where Age[obj] = ctx->time + halo->dT.
 *
 * @param halo      Halo with dT and SnapNum fields
 * @param ctx       Module context with substep_number, num_substeps, and time
 * @param time_out  Output: midpoint time (0.0 on non-OK status); must not be NULL
 * @return Status indicating OK, SKIP_INITIAL, or INVALID
 */
static inline enum MimicObjectTimeStatus mimic_object_substep_time(const struct Halo *halo,
                                                                   const struct ModuleContext *ctx,
                                                                   double *time_out) {
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

  *time_out = (ctx->time + (double)halo->dT) - ((double)ctx->substep_number + 0.5) * dt;
  return MIMIC_OBJECT_TIME_OK;
}

#endif /* MIMIC_SHARED_TIME_PARITY_H */
