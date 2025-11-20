/**
 * @file    sage_infall.h
 * @brief   SAGE infall module interface
 *
 * This module implements gas infall and stripping processes from the SAGE model.
 * It handles:
 * - Cosmological gas infall onto halos
 * - Reionization suppression of gas accretion onto low-mass halos
 * - Stripping of hot gas from satellite galaxies
 * - Redistribution of baryons between galaxies
 *
 * Physics:
 *   infallingMass = f_reion * BaryonFrac * Mvir - (total baryon content)
 *   HotGas += infallingMass
 *
 * Reference:
 *   Based on SAGE model_infall.c (Croton et al. 2016)
 *   Reionization model: Gnedin (2000), Kravtsov et al. (2004)
 *
 * Dependencies:
 *   - Requires: Mvir, deltaMvir (from halo tracking)
 *   - Provides: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS
 *
 * Parameters:
 *   See module_info.yaml for complete parameter list (5 parameters)
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
