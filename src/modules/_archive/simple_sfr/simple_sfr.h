/**
 * @file    simple_sfr.h
 * @brief   Simple star formation rate module interface
 *
 * This module implements simple star formation from cold gas using a
 * Kennicutt-Schmidt-like prescription.
 *
 * Physics: ΔStellarMass = ε_SF * ColdGas * (Vvir/Rvir) * Δt
 *
 * This is a placeholder module for testing the module system infrastructure.
 * A realistic star formation module will be implemented in a later phase.
 */

#ifndef SIMPLE_SFR_H
#define SIMPLE_SFR_H

/**
 * @brief   Register the simple star formation rate module
 *
 * Registers this module with the module registry. Should be called once
 * during program initialization before module_system_init().
 */
void simple_sfr_register(void);

#endif // SIMPLE_SFR_H
