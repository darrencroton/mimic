/**
 * @file    sage_infall.h
 * @brief   SAGE infall module interface
 *
 * Calculates cosmological gas infall for central galaxies and consolidates
 * satellite ejected gas and ICS to centrals. Modified by reionization
 * suppression (HaloBaryonFraction set by sage_reionization module).
 *
 * Physics: InfallingGas = HaloBaryonFraction × Mvir - total_baryons
 *
 * Reference: Croton et al. (2006, 2016)
 */

#ifndef SAGE_INFALL_H
#define SAGE_INFALL_H

void sage_infall_register(void);

#endif
