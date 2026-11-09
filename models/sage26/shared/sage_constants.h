/**
 * @file sage_constants.h
 * @brief Shared SAGE model constants used across multiple physics modules
 *
 * Only literals that appear in more than one file belong here — single-site
 * constants stay at their point of use. Values must not be changed without
 * regenerating and verifying the byte-identical physics baseline.
 */

#ifndef MIMIC_SHARED_SAGE_CONSTANTS_H
#define MIMIC_SHARED_SAGE_CONSTANTS_H

/*
 * MergTime sentinel protocol (galaxy property MergTime, init 999.9 in
 * models/sage26/model_properties.yaml):
 * - SAGE_MERGTIME_UNSET (999.9f): no merger clock assigned; SAGE26's creation
 *   value, written for every FoF central at progenitor joining by
 *   sage26_initialise_merger_clock.
 * - Values above SAGE_MERGTIME_UNSET_THRESHOLD (999.0) mean "unset": joining
 *   estimates a clock for a Type 1 in this state and starts a new orphan's
 *   clock at 0, and sage26_resolve_mergers_and_disruption treats it as a
 *   wiring error.
 * - estimate_merging_time caps computed dynamical-friction times at or above
 *   999 to SAGE_MERGTIME_CEILING (998.0) so they stay below the unset threshold.
 * - SAGE_MERGTIME_IMMEDIATE (-1.0): estimate_merging_time's result for a
 *   same-halo call or a failed mass, Coulomb-logarithm or particle-count guard;
 *   the satellite merges at its first eligible substep.
 */
#define SAGE_MERGTIME_UNSET 999.9f
#define SAGE_MERGTIME_UNSET_THRESHOLD 999.0
#define SAGE_MERGTIME_CEILING 998.0
#define SAGE_MERGTIME_IMMEDIATE -1.0

/* SAGE parity: the disk-SF yield and collisional_starburst_recipe (which also requires
 * the trigger ratio below ThreshMajorMerger) both gate the retained metals on this
 * cold-gas floor (1e10 Msun/h code units). */
#define SAGE_COLD_GAS_YIELD_THRESHOLD 1.0e-8

/* Krumholz & Dekel (2011) metal-ejection halo-mass scale, in 1e10 Msun/h:
 * FracZleaveDisk is attenuated by exp(-central Mvir / this scale). SAGE26's
 * KD11_METAL_HALO_MASS, used by the disk-SF yield and the starburst yield. */
#define SAGE_METAL_EJECTION_MVIR_SCALE 30.0

/* SAGE26's HYDROGEN_MASS_FRAC, the primordial hydrogen mass fraction, as the double 0.74.
 * Used by the H I diagnostic, sfr_br06's H2 cap and the star-formation commits' hydrogen caps.
 * SAGE parity: the radial H2 annulus (shared/sage_h2_physics.h) uses the float 0.74f instead. */
#define SAGE_HYDROGEN_MASS_FRAC 0.74

/* Virial temperature coefficient: T_vir[K] = 35.9 * (Vvir[km/s])^2,
 * i.e. T = mu * m_p * Vvir^2 / (2 k_B) with mu = 0.59 for ionized gas. */
#define SAGE_TVIR_K_PER_SQKMS 35.9

/* SAGE parity: SAGE26's seconds per gigayear, the literal 3.155e16 rather than
 * SEC_PER_YEAR * 1e9. Used by the cooling diagnostics' conversion. */
#define SAGE_SEC_PER_GIGAYEAR 3.155e16

/* Hot-cooling return path, carried from sage26_calculate_cooling_budget to
 * sage26_apply_cooling in the CoolingReturnPath transport property. The values
 * 0 to 3 name the exits of SAGE26's cooling_recipe_hot. */
#define SAGE_COOLING_PATH_NOT_EVALUATED -1 /* no positive interval: cooling not run */
#define SAGE_COOLING_PATH_TAIL 0           /* the normal tail */
#define SAGE_COOLING_PATH_OUTER_GUARD 1    /* HotGas > 0 && Vvir > 0 failed */
#define SAGE_COOLING_PATH_LAMBDA 2         /* lambda <= 0 early return */
#define SAGE_COOLING_PATH_RHO_RCOOL 3      /* rho_rcool <= 0 early return */

#endif /* MIMIC_SHARED_SAGE_CONSTANTS_H */
