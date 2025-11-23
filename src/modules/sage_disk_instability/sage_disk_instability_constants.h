/**
 * @file    sage_disk_instability_constants.h
 * @brief   Physics constants for SAGE disk instability module
 *
 * This header defines all physics constants used in the SAGE disk instability
 * module. These constants were previously hardcoded and have been extracted
 * for clarity and maintainability.
 *
 * Constants are organized by physics process:
 * - Disk Structure (empirical disk size scaling)
 * - Numerical Tolerances (sanity check thresholds)
 *
 * References:
 *   - Mo, Mao & White (1998) - Disk stability criterion
 *   - Efstathiou et al. (1982) - Disk formation in halos
 *   - SAGE: sage-code/model_disk_instability.c
 *
 * Created: 2025-11-23 (Issue 1.3.5 - Hardcoded Magic Numbers)
 */

#ifndef SAGE_DISK_INSTABILITY_CONSTANTS_H
#define SAGE_DISK_INSTABILITY_CONSTANTS_H

// ============================================================================
// DISK STRUCTURE PHYSICS
// ============================================================================

/**
 * @brief   Empirical disk fraction for scale radius calculation
 *
 * Empirical calibration factor for disk scale radius as a fraction of
 * virial radius. This is a simplified model used when full spin-dependent
 * calculation is not available.
 *
 * Physics:
 *   R_disk = DISK_FRACTION * R_vir
 *
 * Typical observed values:
 *   R_vir ~ 0.1-1 Mpc/h → R_disk ~ 3-30 kpc/h = 0.003-0.03 Mpc/h
 *
 * This approximation will be replaced with the full Mo, Mao & White (1998)
 * spin-dependent formula when halo spin parameters are available:
 *   R_disk = (λ / √2) * (j_d / m_d) * R_vir
 *
 * Units: dimensionless (fraction of R_vir)
 * Value: 0.03
 * Purpose: Empirical calibration
 * Reference: Calibrated to match typical disk sizes
 */
#define DISK_FRACTION 0.03

// ============================================================================
// NUMERICAL TOLERANCES
// ============================================================================

/**
 * @brief   Tolerance factor for mass conservation sanity checks
 *
 * Small multiplicative tolerance used in sanity checks to account for
 * floating-point rounding errors. Ensures that physically impossible
 * conditions (e.g., bulge mass exceeding total stellar mass) are detected
 * and corrected.
 *
 * Physics constraint being checked:
 *   BulgeMass ≤ StellarMass
 *   MetalsBulgeMass ≤ MetalsStellarMass
 *
 * The factor of 1.0001 allows for 0.01% numerical error before triggering
 * a warning and correction.
 *
 * Units: dimensionless
 * Value: 1.0001
 * Purpose: Numerical tolerance for floating-point comparisons
 */
#define MASS_TOLERANCE_FACTOR 1.0001

#endif  /* SAGE_DISK_INSTABILITY_CONSTANTS_H */
