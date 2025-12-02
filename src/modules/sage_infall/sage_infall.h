/**
 * @file    sage_infall.h
 * @brief   SAGE infall module interface
 *
 * This module implements cosmological gas infall from the SAGE model.
 * It handles:
 * - Cosmological gas infall onto central galaxies
 * - Redistribution of ejected gas and ICS from satellites to central
 *
 * Physics:
 *   infallingMass = HaloBaryonFraction * Mvir - (total baryon content)
 *   HotGas += infallingMass
 *
 * Reference:
 *   Based on SAGE model_infall.c (Croton et al. 2016)
 *
 * Dependencies:
 *   - Requires: HaloBaryonFraction (from sage_reionization), Mvir (from halo tracking)
 *   - Provides: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, InfallingGas
 *
 * Parameters:
 *   No module parameters (uses HaloBaryonFraction property)
 */

#ifndef SAGE_INFALL_H
#define SAGE_INFALL_H

/**
 * @brief   Register the sage_infall module
 *
 * Registers this module with the module registry. This function should be
 * called once during program initialization before module_system_init().
 *
 * Called from: src/modules/module_init.c :: register_all_modules()
 */
void sage_infall_register(void);

#endif // SAGE_INFALL_H
