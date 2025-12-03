/**
 * @file    sage_cooling.h
 * @brief   SAGE cooling and AGN heating module interface
 *
 * Implements gas cooling from hot halos to cold disks with AGN feedback. Two cooling
 * regimes: cold accretion (rcool > Rvir) and hot halo cooling (rcool < Rvir). AGN
 * radio-mode feedback can suppress cooling. Black hole growth via empirical, Bondi-Hoyle,
 * or cold cloud accretion modes.
 *
 * Physics: Lambda(T, Z) from Sutherland & Dopita (1993) cooling tables
 *
 * Reference: White & Frenk (1991), Croton et al. (2006, 2016)
 */

#ifndef SAGE_COOLING_H
#define SAGE_COOLING_H

/**
 * @brief   Register the sage_cooling module with the module registry
 */
void sage_cooling_register(void);

#endif // SAGE_COOLING_H
