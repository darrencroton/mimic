/**
 * @file    sage_disk_instability.h
 * @brief   SAGE disk instability module interface
 *
 * Implements disk stability criterion (Mo, Mao & White 1998) and direct stellar
 * mass transfer from unstable disks to bulge. Metallicity preserved during transfers.
 *
 * Physics: Mcrit = Vmax^2 × (3 × DiskScaleRadius) / G
 *
 * Note: Partial implementation - starburst triggering and BH growth deferred pending
 * sage_mergers module.
 *
 * Reference: Mo, Mao & White (1998), Croton et al. (2016), based on SAGE model_disk_instability.c
 */

#ifndef SAGE_DISK_INSTABILITY_H
#define SAGE_DISK_INSTABILITY_H

/**
 * @brief   Register the sage_disk_instability module with the module registry
 */
void sage_disk_instability_register(void);

#endif // SAGE_DISK_INSTABILITY_H
