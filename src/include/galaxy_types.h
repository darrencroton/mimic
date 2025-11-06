/**
 * @file galaxy_types.h
 * @brief Galaxy-specific data structures (physics-agnostic allocation)
 *
 * This file defines galaxy properties that are separate from halo properties.
 * Galaxy data is allocated independently to maintain physics-agnostic core.
 *
 * Vision Principle 1 (Physics-Agnostic Core): Core manages memory allocation,
 * but has zero knowledge of galaxy physics. Modules populate these properties.
 *
 * Vision Principle 4 (Single Source of Truth): GalaxyData is the authoritative
 * source for all galaxy properties.
 */

#ifndef GALAXY_TYPES_H
#define GALAXY_TYPES_H

/**
 * @brief Galaxy baryonic properties
 *
 * This structure contains all galaxy-specific properties computed by physics
 * modules. It is allocated separately from struct Halo to maintain clear
 * separation between dark matter halo tracking (core) and baryonic physics
 * (modules).
 *
 * Memory lifecycle:
 * - Allocated in init_halo() when new halos are created
 * - Copied in copy_progenitor_halos() when halos inherit from progenitors
 * - Freed in free_halos_and_tree() after output is written
 *
 * Current properties:
 * - StellarMass: Total stellar mass (set by stellar_mass module)
 */
struct GalaxyData {
    float StellarMass; /**< Total stellar mass in 10^10 Msun/h */
};

#endif // GALAXY_TYPES_H
