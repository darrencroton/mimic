#ifndef IO_TREE_FOREST_DISTRIBUTION_H
#define IO_TREE_FOREST_DISTRIBUTION_H

/**
 * @file    tree/forest_distribution.h
 * @brief   Forest load-balancing schemes for forest-oriented tree readers.
 *
 * These options are part of Mimic's reader layer, not of the core driver and not
 * of the vendored Consistent-Trees compatibility seam. Today the
 * Consistent-Trees HDF5 reader uses the weighted schemes; uniform splitting is
 * also used by the ASCII reader.
 */

#include <strings.h>

enum ForestDistributionScheme {
  uniform_in_forests = 0,      /* every forest has equal cost */
  linear_in_nhalos = 1,        /* cost = nhalos */
  quadratic_in_nhalos = 2,     /* cost = nhalos^2 */
  exponent_in_nhalos = 3,      /* cost = nhalos^exponent */
  generic_power_in_nhalos = 4, /* cost = pow(nhalos, exponent) */
  num_forest_weight_types
};

static inline int forest_distribution_scheme_from_string(const char *s) {
  if (strcasecmp(s, "uniform") == 0)
    return uniform_in_forests;
  if (strcasecmp(s, "linear") == 0)
    return linear_in_nhalos;
  if (strcasecmp(s, "quadratic") == 0)
    return quadratic_in_nhalos;
  if (strcasecmp(s, "exponent") == 0)
    return exponent_in_nhalos;
  if (strcasecmp(s, "generic_power") == 0)
    return generic_power_in_nhalos;
  return -1;
}

#endif /* IO_TREE_FOREST_DISTRIBUTION_H */
