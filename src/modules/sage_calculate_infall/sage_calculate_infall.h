/**
 * @file    sage_calculate_infall.h
 * @brief   SAGE calculate infall module interface
 *
 * Calculates cosmological gas infall for central galaxies and consolidates
 * satellite ejected gas and ICS to centrals. Modified by reionization
 * suppression (HaloBaryonFraction set by sage_reionization module).
 *
 * Physics: InfallingGas = HaloBaryonFraction × Mvir - total_baryons
 *
 * Reference: Croton et al. (2006, 2016)
 */

#ifndef SAGE_CALCULATE_INFALL_H
#define SAGE_CALCULATE_INFALL_H

void sage_calculate_infall_register(void);

#endif
