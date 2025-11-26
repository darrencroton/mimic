/**
 * @file    sage_satellite_stripping.h
 * @brief   Environmental gas stripping from satellite galaxies (SAGE model)
 *
 * This module handles the environmental stripping of hot gas from satellite
 * galaxies as they move through the hot halo of the central galaxy. Part of
 * the SAGE (Semi-Analytic Galaxy Evolution) model implementation in Mimic.
 *
 * PHYSICS
 * =======
 * Satellite galaxies lose hot gas through environmental processes (ram pressure
 * stripping, tidal stripping) as they orbit within their host halo. The
 * stripped gas is transferred to the central galaxy's hot gas reservoir.
 *
 * The amount of stripping depends on:
 * 1. Satellite's current baryon content vs cosmic baryon fraction
 * 2. Reionization suppression (Gnedin 2000)
 * 3. Available hot gas in satellite
 *
 * VISION PRINCIPLES
 * =================
 * 2. Runtime Modularity: Can be enabled/disabled independently from sage_infall
 * 3. Metadata-Driven: Full module configuration in module_info.yaml
 * 4. Single Source of Truth: Uses shared reionization.h utility
 * 6. Memory Efficiency: Processes in-place, no additional allocations
 *
 * References:
 *   - Croton et al. (2006) - SAGE galaxy evolution model
 *   - Gnedin (2000) - Reionization suppression model
 *   - Kravtsov et al. (2004) - Fitting formulas (Appendix B)
 */

#ifndef SAGE_SATELLITE_STRIPPING_H
#define SAGE_SATELLITE_STRIPPING_H

#include "types.h"

/**
 * @brief   Register sage_satellite_stripping module with Mimic core
 *
 * Called during module system initialization. Provides module metadata
 * and lifecycle function pointers to the core.
 */
void sage_satellite_stripping_register(void);

#endif /* SAGE_SATELLITE_STRIPPING_H */
