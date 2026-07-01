/**
 * @file    timestep.c
 * @brief   Timestep helper functions.
 */

#include "constants.h"
#include "proto.h"

#include <math.h>

int compute_dynamic_substeps(double time_interval, double t_dyn, int substeps_per_tdyn,
                             int max_dynamic_substeps) {
  if (!isfinite(time_interval) || !isfinite(t_dyn) || time_interval <= 0.0 || t_dyn <= 0.0) {
    return 1;
  }

  int ceiling = (max_dynamic_substeps > 0) ? max_dynamic_substeps : DEFAULT_MAX_DYNAMIC_SUBSTEPS;
  int resolution = (substeps_per_tdyn > 0) ? substeps_per_tdyn : 1;
  double requested = ceil(time_interval * (double)resolution / t_dyn);
  if (!isfinite(requested) || requested <= 0.0) {
    return 1;
  }
  if (requested > (double)ceiling) {
    return ceiling;
  }

  return (int)requested;
}
