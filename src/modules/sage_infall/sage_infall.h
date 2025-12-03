/**
 * @file    sage_infall.h
 * @brief   SAGE infall module interface
 *
 * Implements cosmological gas infall onto central galaxies from the SAGE model.
 * Central galaxies accrete baryonic gas proportional to halo growth, modified by
 * reionization suppression (set by sage_reionization module).
 *
 * Physics: InfallingGas = HaloBaryonFraction × Mvir - total_baryons
 *
 * Reference: Croton et al. (2006, 2016)
 */

#ifndef SAGE_INFALL_H
#define SAGE_INFALL_H

/**
 * @brief   Register the sage_infall module with the module registry
 */
void sage_infall_register(void);

#endif // SAGE_INFALL_H
