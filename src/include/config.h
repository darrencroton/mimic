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

/*
 * Temporary macros for configuration synchronization during transition.
 *
 * These macros formalize the synchronization pattern between MimicConfig
 * and legacy global variables. They make the synchronization explicit and
 * searchable during the transition to "single source of truth" architecture.
 *
 * IMPORTANT: These are temporary scaffolding and will be removed in Phase 3
 * (post-transformation). DO NOT use these macros in new code - access
 * MimicConfig.* directly instead.
 *
 * Usage:
 *   SYNC_CONFIG_INT(MAXSNAPS);     // MAXSNAPS = MimicConfig.MAXSNAPS
 *   SYNC_CONFIG_DOUBLE(Hubble);    // Hubble = MimicConfig.Hubble
 */
#define SYNC_CONFIG_INT(field) field = MimicConfig.field
#define SYNC_CONFIG_DOUBLE(field) field = MimicConfig.field

#endif /* #ifndef CONFIG_H */
