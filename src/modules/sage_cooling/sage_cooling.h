/**
 * @file    sage_cooling.h
 * @brief   SAGE cooling and AGN heating module interface
 *
 * This module implements gas cooling from hot halos to cold disks with
 * AGN feedback from the SAGE model. It handles:
 * - Gas cooling from hot halo based on cooling radius and metallicity
 * - Two cooling regimes: cold accretion (rcool > Rvir) and hot halo cooling
 * - AGN feedback suppression of cooling via radio-mode heating
 * - Black hole accretion and growth (empirical, Bondi-Hoyle, or cold cloud modes)
 * - Tracking of heating radius and energy budgets
 *
 * Physics:
 *   - Virial temperature: T_vir = 35.9 * Vvir^2 (K, with Vvir in km/s)
 *   - Cooling rate: Lambda(T, Z) from Sutherland & Dopita (1993) tables
 *   - Cooling radius: rcool = sqrt(rho0 / rho_rcool)
 *   - Cold accretion: coolingGas = HotGas * (Vvir/Rvir) * dt (if rcool > Rvir)
 *   - Hot halo: coolingGas = HotGas/Rvir * rcool/(2*tcool) * dt (if rcool < Rvir)
 *   - AGN heating: Suppress cooling based on black hole accretion
 *
 * References:
 *   - Based on SAGE model_cooling_heating.c (Croton et al. 2016)
 *   - White & Frenk (1991) - Cooling model framework
 *   - Croton et al. (2006) - AGN feedback implementation
 *   - Sutherland & Dopita (1993) - Cooling function tables
 *
 * Dependencies:
 *   - Requires: Mvir, Rvir, Vvir, HotGas, MetalsHotGas (from sage_infall)
 *   - Provides: ColdGas, MetalsColdGas, BlackHoleMass, Cooling, Heating, r_heat
 *
 * Parameters:
 *   - SageCooling_RadioModeEfficiency: AGN feedback efficiency (default: 0.01)
 *   - SageCooling_AGNrecipeOn: AGN mode (0=off, 1=empirical, 2=Bondi, 3=cold cloud)
 */

#ifndef SAGE_COOLING_H
#define SAGE_COOLING_H

/**
 * @brief   Register the sage_cooling module
 *
 * Registers this module with the module registry. This function should be
 * called once during program initialization before module_system_init().
 *
 * Called from: src/modules/module_init.c :: register_all_modules()
 */
void sage_cooling_register(void);

#endif // SAGE_COOLING_H
