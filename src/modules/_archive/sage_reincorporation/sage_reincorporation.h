/**
 * @file    sage_reincorporation.h
 * @brief   SAGE gas reincorporation module interface
 *
 * This module implements gas reincorporation from the SAGE model, handling
 * the return of ejected gas (from supernova feedback) back to the hot halo
 * gas reservoir.
 *
 * Physics Overview:
 * -----------------
 * Reincorporation occurs when the halo's virial velocity exceeds a critical
 * velocity related to the characteristic velocity of supernova-driven winds
 * (~445 km/s). More massive halos can recapture their ejected gas more
 * efficiently.
 *
 * Rate equation:
 *   dM_reinc/dt = (Vvir/Vcrit - 1) * M_ejected / t_dyn
 *
 * where:
 *   - Vcrit = 445.48 km/s * ReIncorporationFactor (tunable parameter)
 *   - t_dyn = Rvir / Vvir (halo dynamical time)
 *
 * Mass Flow:
 *   EjectedMass      → HotGas
 *   MetalsEjectedMass → MetalsHotGas
 *
 * Module Dependencies:
 * --------------------
 * Requires (from sage_infall):
 *   - EjectedMass, MetalsEjectedMass (ejected reservoir)
 *   - HotGas, MetalsHotGas (hot gas reservoir)
 *
 * Provides:
 *   - (No new properties; modifies existing reservoirs)
 *
 * Configuration Parameters:
 * -------------------------
 * SageReincorporation_ReIncorporationFactor:
 *   Tunable parameter multiplying critical velocity
 *   Default: 1.0
 *   Range: [0.0, 10.0]
 *   Effect: Lower values → more reincorporation in lower-mass halos
 *
 * References:
 * -----------
 * - Croton et al. (2016) - SAGE model description
 * - Guo et al. (2011) - Reincorporation timescale discussion
 * - SAGE source: sage-code/model_reincorporation.c
 *
 * Vision Principles:
 * ------------------
 * - Physics-Agnostic Core: Interacts only through module interface
 * - Runtime Modularity: Configurable via parameter file
 * - Single Source of Truth: Updates GalaxyData properties only
 */

#ifndef SAGE_REINCORPORATION_H
#define SAGE_REINCORPORATION_H

/**
 * @brief   Register the SAGE reincorporation module
 *
 * Called during module initialization to register this module with the
 * module system. Automatically invoked by the generated module registry.
 */
void sage_reincorporation_register(void);

#endif /* SAGE_REINCORPORATION_H */
