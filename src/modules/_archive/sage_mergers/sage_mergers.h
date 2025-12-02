/**
 * @file    sage_mergers.h
 * @brief   SAGE galaxy merger physics module (header)
 * @author  Mimic Team (ported from SAGE)
 * @date    November 2025
 *
 * This module implements galaxy merger processes from the SAGE model, including:
 * - Dynamical friction timescale calculations for satellite galaxies
 * - Galaxy-galaxy mergers (major and minor)
 * - Merger-induced starbursts
 * - Black hole growth during mergers
 * - Quasar-mode AGN feedback
 * - Morphological transformations (disk to bulge)
 * - Satellite disruption to intracluster stars
 *
 * Physics:
 *   - Merger timescale: T_merge ∝ R_sat² * V_vir / (ln(M_host/M_sat) * G * M_sat)
 *   - Major merger: mass_ratio > ThreshMajorMerger → spheroid transformation
 *   - Minor merger: mass_ratio > 0.1 → disk preservation with bulge growth
 *   - Starburst efficiency: ε_burst = 0.56 * mass_ratio^0.7 (Cox thesis)
 *   - BH growth: ΔM_BH ∝ mass_ratio * ColdGas / (1 + (280/Vvir)²)
 *
 * Reference: SAGE source sage-code/model_mergers.c (Croton et al. 2016)
 *
 * Dependencies:
 *   - Requires: ColdGas, StellarMass, HotGas, MetalsHotGas, MetalsColdGas,
 *               EjectedMass, MetalsEjectedMass, ICS, MetalsICS, BlackHoleMass
 *   - Provides: BulgeMass, MetalsStellarMass, MetalsBulgeMass,
 *               QuasarModeBHaccretionMass, TimeOfLastMajorMerger,
 *               TimeOfLastMinorMerger, OutflowRate, DiskScaleRadius
 *
 * Parameters:
 *   - SageMergers_BlackHoleGrowthRate: BH growth efficiency (default: 0.01)
 *   - SageMergers_QuasarModeEfficiency: Quasar feedback efficiency (default: 0.001)
 *   - SageMergers_ThreshMajorMerger: Major merger mass ratio threshold (default: 0.3)
 *   - SageMergers_RecycleFraction: Stellar recycling fraction (default: 0.43)
 *   - SageMergers_Yield: Metal yield per unit stellar mass (default: 0.03)
 */

#ifndef SAGE_MERGERS_H
#define SAGE_MERGERS_H

/**
 * @brief   Register the sage_mergers module
 *
 * Registers this module with the module registry. This function should be
 * called once during program initialization before module_system_init().
 *
 * Called from: Auto-generated module registration code
 */
void sage_mergers_register(void);

#endif /* SAGE_MERGERS_H */
