/**
 * @file time_parity.h
 * @brief Per-object substep timestep helper for SAGE parity
 *
 * Provides the substep dt with strict boundary handling matching SAGE's
 * initial-snapshot sentinel and multi-substep loop. Used by modules that
 * accumulate rates or apply physics per substep. The merger pass forms SAGE26's
 * fused event time itself (sage26_resolve_mergers_and_disruption,
 * R/src/core_build_model.c:549).
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

#endif /* MIMIC_SHARED_TIME_PARITY_H */
