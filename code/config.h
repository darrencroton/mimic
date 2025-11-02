#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"
#include "util_error.h"

/* HDF5 configuration */
#ifdef HDF5
#include <hdf5.h>
#define MODELNAME "SAGE"
#endif

/* Global configuration structure - replaces individual globals */
extern struct SageConfig SageConfig;

#endif /* #ifndef CONFIG_H */
