/**
 * @file    sage_satellite_stripping.h
 * @brief   SAGE satellite stripping module interface
 *
 * Implements environmental gas removal from satellites via ram pressure and tidal
 * stripping. Satellites lose hot gas when their baryon content exceeds expectations
 * based on local baryon fraction (set by sage_reionization). Stripped gas transfers
 * to central galaxy's hot reservoir with metallicity preserved.
 *
 * Physics: strippedGas = -(HaloBaryonFraction × Mvir - total_baryons) / STEPS
 *
 * This module requires sage_reionization to run first to set HaloBaryonFraction.
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2006, 2016)
 */

#ifndef SAGE_SATELLITE_STRIPPING_H
#define SAGE_SATELLITE_STRIPPING_H

#include "types.h"

/**
 * @brief   Register the sage_satellite_stripping module with the module registry
 */
void sage_satellite_stripping_register(void);

#endif /* SAGE_SATELLITE_STRIPPING_H */
