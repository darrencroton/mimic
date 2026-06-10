#ifndef CONFIG_H
#define CONFIG_H

#include "types.h"
#include "error.h"

/* HDF5 configuration */
#ifdef HDF5
#include <hdf5.h>
#define MODELNAME "MIMIC"
#endif

/* Global configuration structure - the single source of truth for run
 * parameters; access fields as MimicConfig.<field> */
extern struct MimicConfig MimicConfig;

#endif /* #ifndef CONFIG_H */
