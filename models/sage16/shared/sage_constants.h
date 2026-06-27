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
 * models/sage16/model_properties.yaml):
 * - SAGE_MERGTIME_UNSET (999.9): no merger clock assigned yet; written at
 *   property init and when a satellite is promoted back to a Type 0 central.
 * - Values above SAGE_MERGTIME_UNSET_THRESHOLD (999.0) mean "unset";
 *   sage_initialise_merger_clock assigns a clock only in this state, and
 *   sage_resolve_mergers_and_disruption treats it as a wiring error.
 * - Computed dynamical-friction times at or above the threshold are capped
 *   to SAGE_MERGTIME_CEILING (998.0) so they remain distinguishable from
 *   the unset state.
 * - SAGE_MERGTIME_IMMEDIATE (-1.0): forced immediate merger (satellite below
 *   the SAGE particle-count floor, or an unresolved Type 2 orphan).
 */
#define SAGE_MERGTIME_UNSET 999.9f
#define SAGE_MERGTIME_UNSET_THRESHOLD 999.0
#define SAGE_MERGTIME_CEILING 998.0
#define SAGE_MERGTIME_IMMEDIATE -1.0

/* SAGE parity: collisional_starburst_recipe and the disk-SF yield both gate
 * metal production on this cold-gas floor (1e10 Msun/h code units). */
#define SAGE_COLD_GAS_YIELD_THRESHOLD 1.0e-8

/* Krumholz & Dekel (2011) metal-ejection halo-mass scale, in 1e10 Msun/h:
 * FracZleaveDisk is attenuated by exp(-central Mvir / this scale). */
#define SAGE_METAL_EJECTION_MVIR_SCALE 30.0

/* Virial temperature coefficient: T_vir[K] = 35.9 * (Vvir[km/s])^2,
 * i.e. T = mu * m_p * Vvir^2 / (2 k_B) with mu = 0.59 for ionized gas. */
#define SAGE_TVIR_K_PER_SQKMS 35.9

#endif /* MIMIC_SHARED_SAGE_CONSTANTS_H */
