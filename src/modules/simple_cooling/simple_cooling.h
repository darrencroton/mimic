/**
 * @file simple_cooling.h
 * @brief Simple cooling module interface
 *
 * This module implements a simple cooling prescription that converts
 * accreted halo mass into cold gas based on the baryon fraction.
 *
 * Physics: ΔColdGas = f_baryon * ΔMvir
 *
 * This is part of PoC Round 2 to test module interaction and time evolution.
 */

#ifndef SIMPLE_COOLING_H
#define SIMPLE_COOLING_H

/**
 * @brief Register the simple cooling module
 *
 * Registers this module with the module registry. Should be called once
 * during program initialization before module_system_init().
 */
void simple_cooling_register(void);

#endif // SIMPLE_COOLING_H
