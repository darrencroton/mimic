#ifndef MIMIC_CONSTANTS_H
#define MIMIC_CONSTANTS_H

/**
 * @file    mimic_constants.h
 * @brief   Core infrastructure constants for Mimic framework
 *
 * This file contains constants for Mimic's core infrastructure including
 * numerical stability thresholds, array sizes, memory management parameters,
 * and data type identifiers.
 *
 * For physical constants (G, c, M_sun, etc.), see:
 *   src/modules/_system/physical_constants.h
 */

/* Floating-point comparison epsilon values */
#define EPSILON_SMALL 1.0e-10 /* For near-zero comparisons */
#define EPSILON_MEDIUM 1.0e-6 /* For general equality comparisons */
#define EPSILON_LARGE 1.0e-4  /* For physics model thresholds */

/* Numerical constants for the simulation */
#define NDIM 3
#define MAXHALOFAC 5
#define ALLOCPARAMETER 10.0
#define MAX_NODE_NAME_LEN 50
#define ABSOLUTEMAXSNAPS                                                       \
  1000              /* The largest number of snapshots for any simulation */
#define MAXTAGS 300 /* Max number of parameters */
#define MAX_STRING_LEN 1024 /* Max length of a string containing a name */

/* Cosmological constants */
#define INITIAL_REDSHIFT 1000.0 /* Recombination era (CMB formation) */

/* Progress reporting */
#define TREE_PROGRESS_INTERVAL 10000 /* Log progress every N trees */

/* Memory allocation parameters */
#define HALO_ARRAY_GROWTH_FACTOR                                               \
  1.5 /* Factor to grow arrays by (1.5 = 50% growth) */
#define MIN_HALO_ARRAY_GROWTH                                                  \
  1000 /* Minimum growth increment regardless of factor */
#define MAX_HALO_ARRAY_SIZE                                                    \
  1000000000                   /* Upper limit to prevent excessive allocation */
#define INITIAL_FOF_HALOS 1000 /* Initial size for FOF halo arrays */
#define MEMORY_REPORT_THRESHOLD_MB 10.0 /* Report if > 10 MB allocated */

/* Data type IDs */
#define DOUBLE 1
#define STRING 2
#define INT 3

#endif /* #ifndef MIMIC_CONSTANTS_H */
