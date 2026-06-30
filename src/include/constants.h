#ifndef MIMIC_CONSTANTS_H
#define MIMIC_CONSTANTS_H

/**
 * @file    constants.h
 * @brief   Core infrastructure constants for Mimic framework
 *
 * This file contains constants for Mimic's core infrastructure including
 * numerical stability thresholds, array sizes, memory management parameters,
 * and data type identifiers.
 *
 * For physical constants (G, c, M_sun, etc.), see:
 *   src/module_system/physical_constants.h
 */

/* Floating-point comparison epsilon values */
#define EPSILON_SMALL 1.0e-10 /* For near-zero comparisons */
#define EPSILON_MEDIUM 1.0e-6 /* For general equality comparisons */

/* Numerical constants for the simulation */
#define NDIM 3
/* Initial ProcessedHalos allocation = MAXHALOFAC * InputTreeNHalos. This is a
 * starting estimate only; the buffer grows dynamically via myrealloc_cat when
 * orphan halos cause the actual output count to exceed this multiple. */
#define MAXHALOFAC 5
#define ABSOLUTEMAXSNAPS 1000   /* The largest number of snapshots for any simulation */
#define MAX_STRING_LEN 1024     /* Max length of a string containing a name */
#define MAX_MODEL_PARAMS 256    /* Max modules.parameters entries in the input file */
#define MAX_DYNAMIC_SUBSTEPS 50 /* Internal cap for dynamic timestep substeps */

/* Cosmological constants */
#define INITIAL_REDSHIFT 1000.0 /* Recombination era (CMB formation) */

/* Progress reporting */
#define TREE_PROGRESS_INTERVAL 10000 /* Log progress every N trees */

/* UniqueGalaxyID encoding constants */
#define TREE_MUL_FAC (1000000000LL) /* Global forest multiplier: 10^9 */

/* Memory allocation parameters */
#define HALO_ARRAY_GROWTH_FACTOR 1.5    /* Factor to grow arrays by (1.5 = 50% growth) */
#define MIN_HALO_ARRAY_GROWTH 1000      /* Minimum growth increment regardless of factor */
#define MAX_HALO_ARRAY_SIZE 1000000000  /* Upper limit to prevent excessive allocation */
#define INITIAL_FOF_HALOS 1000          /* Initial size for FOF halo arrays */
#define MEMORY_REPORT_THRESHOLD_MB 10.0 /* Report if > 10 MB allocated */

/* Data type IDs */
#define DOUBLE 1
#define STRING 2
#define INT 3

#endif /* #ifndef MIMIC_CONSTANTS_H */
