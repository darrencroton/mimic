/**
 * @file    sage_add_infall.h
 * @brief   SAGE add infall module interface
 *
 * Distributes infalling gas to hot gas reservoir with metallicity tracking.
 * For negative infall (mass loss), removes from ejected reservoir first, then hot gas.
 *
 * Physics: Transfer InfallingGas / num_substeps → HotGas per substep
 *
 * Reference: Croton et al. (2006, 2016), based on SAGE model_infall.c
 */

#ifndef SAGE_ADD_INFALL_H
#define SAGE_ADD_INFALL_H

void sage_add_infall_register(void);

#endif
