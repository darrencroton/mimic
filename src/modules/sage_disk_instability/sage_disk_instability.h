/**
 * @file    sage_disk_instability.h
 * @brief   SAGE disk instability module interface
 *
 * This module implements disk instability detection and mass redistribution
 * from the SAGE model. It handles:
 * - Disk stability criterion (Mo, Mao & White 1998)
 * - Direct stellar mass transfer from disk to bulge
 * - Metallicity preservation during mass transfers
 * - Disk scale radius calculation
 *
 * Physics:
 *   Mcrit = Vmax^2 * (3 * DiskScaleRadius) / G
 *   If (disk_mass > Mcrit): transfer excess mass to bulge
 *
 * Reference:
 *   Based on SAGE model_disk_instability.c (Croton et al. 2016)
 *   Stability criterion: Mo, Mao & White (1998)
 *
 * Dependencies:
 *   - Requires: ColdGas, MetalsColdGas, StellarMass, Vmax
 *   - Provides: BulgeMass, MetalsBulgeMass, MetalsStellarMass, DiskScaleRadius
 *
 * Parameters:
 *   - SageDiskInstability_DiskInstabilityOn: Enable physics (default: 1)
 *   - SageDiskInstability_DiskRadiusFactor: Effective radius factor (default: 3.0)
 *
 * Implementation Status (v1.0.0):
 *   This is a PARTIAL IMPLEMENTATION providing core stability physics.
 *   Starburst triggering and AGN growth deferred pending sage_mergers module.
 *   See module_info.yaml and README.md for complete implementation roadmap.
 */

#ifndef SAGE_DISK_INSTABILITY_H
#define SAGE_DISK_INSTABILITY_H

/**
 * @brief   Register the sage_disk_instability module
 *
 * Registers this module with the module registry. This function should be
 * called once during program initialization before module_system_init().
 *
 * Called from: src/modules/_system/generated/module_registration.c
 */
void sage_disk_instability_register(void);

#endif // SAGE_DISK_INSTABILITY_H
