/**
 * @file    sage_satellite_stripping.h
 * @brief   SAGE satellite stripping module interface
 *
 * Implements environmental gas stripping from satellites via ram pressure and tidal
 * effects. Satellites lose hot gas when baryon content exceeds HaloBaryonFraction
 * × Mvir. Stripped gas transfers to central's hot reservoir with metallicity preserved.
 *
 * Physics: strippedGas = -(HaloBaryonFraction × Mvir - total_baryons) / SubSteps
 *
 * Mass flow: Satellite HotGas → Central HotGas (with metals)
 *
 * Requires sage_reionization to set HaloBaryonFraction.
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
