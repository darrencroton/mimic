/**
 * @file    sage_add_infall.h
 * @brief   SAGE add infall module interface
 *
 * Adds infalling gas (calculated by sage_infall module) to hot gas reservoir
 * with metallicity tracking. For negative infall (mass loss), removes from
 * ejected reservoir first, then hot gas. Only processes central galaxies.
 *
 * Physics: Transfer InfallingGas → HotGas with metallicity preservation
 *
 * Reference: Croton et al. (2006, 2016), based on SAGE model_infall.c
 */

#ifndef SAGE_ADD_INFALL_H
#define SAGE_ADD_INFALL_H

void sage_add_infall_register(void);

#endif
