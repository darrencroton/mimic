/**
 * @file    timestep.c
 * @brief   Timestep helper functions.
 */

#include "constants.h"
#include "proto.h"

#include <math.h>

int compute_dynamic_substeps(double time_interval, double t_dyn, int substeps_per_tdyn,
                             int min_dynamic_substeps, int max_dynamic_substeps) {
  int ceiling = (max_dynamic_substeps > 0) ? max_dynamic_substeps : DEFAULT_MAX_DYNAMIC_SUBSTEPS;
  int floor_count = (min_dynamic_substeps > 0) ? min_dynamic_substeps : 1;
  /* Parse validation keeps floor <= ceiling; clamp here so direct callers get a true bound */
  if (floor_count > ceiling) {
    floor_count = ceiling;
  }

  /* A degenerate interval or dynamical time has no request: return the floor, not 1 */
  if (!isfinite(time_interval) || !isfinite(t_dyn) || time_interval <= 0.0 || t_dyn <= 0.0) {
    return floor_count;
  }

  int resolution = (substeps_per_tdyn > 0) ? substeps_per_tdyn : 1;
  double requested = ceil(time_interval * (double)resolution / t_dyn);
  if (!isfinite(requested) || requested <= 0.0) {
    return floor_count;
  }
  if (requested > (double)ceiling) {
    return ceiling;
  }
  if (requested < (double)floor_count) {
    return floor_count;
  }

  return (int)requested;
}
