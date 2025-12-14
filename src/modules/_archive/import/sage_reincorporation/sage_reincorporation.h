/**
 * @file    sage_reincorporation.h
 * @brief   SAGE gas reincorporation module interface
 *
 * Implements return of ejected gas (from supernova feedback) back to hot halo reservoir.
 * More massive halos (Vvir > Vcrit) recapture ejected gas more efficiently.
 *
 * Physics: dM_reinc/dt = (Vvir/Vcrit - 1) × M_ejected / t_dyn
 *          Vcrit = 445.48 km/s × ReIncorporationFactor
 *
 * Mass flow: EjectedMass → HotGas (with metals)
 *
 * Reference: Croton et al. (2016), Guo et al. (2011), based on SAGE model_reincorporation.c
 */

#ifndef SAGE_REINCORPORATION_H
#define SAGE_REINCORPORATION_H

/**
 * @brief   Register the sage_reincorporation module with the module registry
 */
void sage_reincorporation_register(void);

#endif /* SAGE_REINCORPORATION_H */
