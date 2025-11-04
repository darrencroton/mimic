#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"
#include "error.h"

/* HDF5 configuration */
#ifdef HDF5
#include <hdf5.h>
#define MODELNAME "MIMIC"
#endif

/* Global configuration structure - replaces individual globals */
extern struct MimicConfig MimicConfig;

#endif /* #ifndef CONFIG_H */
