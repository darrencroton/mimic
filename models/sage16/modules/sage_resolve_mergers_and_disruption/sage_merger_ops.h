/**
 * @file    sage_merger_ops.h
 * @brief   Live-target baryon transfer helpers for SAGE merger/disruption parity
 *
 * These helpers operate on already-resolved live target galaxies. They do not
 * clear the source galaxy; the caller owns source Type changes and event routing.
 */

#ifndef MIMIC_SHARED_SAGE_MERGER_OPS_H
#define MIMIC_SHARED_SAGE_MERGER_OPS_H

#include "types.h"

/**
 * @brief Calculate SAGE's baryonic merger mass-ratio convention.
 *
 * SAGE mass-ratio convention: mi/ma, with fallback to 1.0 when both are zero.
 * This must use the target's live pre-transfer state for immediate-order parity.
 */
static inline double mimic_sage_calculate_merger_mass_ratio(const struct GalaxyData *satellite,
                                                            const struct GalaxyData *target) {
  const double sat_mass = (double)satellite->StellarMass + (double)satellite->ColdGas;
  const double target_mass = (double)target->StellarMass + (double)target->ColdGas;
  const double smaller = (sat_mass < target_mass) ? sat_mass : target_mass;
  const double larger = (sat_mass < target_mass) ? target_mass : sat_mass;

  return (larger > 0.0) ? (smaller / larger) : 1.0;
}

/**
 * @brief Transfer all merger baryons into the live target before event consumers run.
 */
static inline void mimic_sage_merge_transfer(struct GalaxyData *target,
                                             const struct GalaxyData *satellite) {
  target->ColdGas += satellite->ColdGas;
  target->MetalsColdGas += satellite->MetalsColdGas;

  target->StellarMass += satellite->StellarMass;
  target->MetalsStellarMass += satellite->MetalsStellarMass;

  target->HotGas += satellite->HotGas;
  target->MetalsHotGas += satellite->MetalsHotGas;

  target->EjectedGas += satellite->EjectedGas;
  target->MetalsEjectedGas += satellite->MetalsEjectedGas;

  target->ICS += satellite->ICS;
  target->MetalsICS += satellite->MetalsICS;

  target->BlackHoleMass += satellite->BlackHoleMass;

  target->BulgeMass += satellite->StellarMass;
  target->MetalsBulgeMass += satellite->MetalsStellarMass;
}

/**
 * @brief Apply SAGE disruption transfer: heat gas, preserve ICS, and move stars to ICS.
 */
static inline void mimic_sage_disruption_transfer(struct GalaxyData *target,
                                                  const struct GalaxyData *satellite) {
  target->HotGas += satellite->ColdGas + satellite->HotGas;
  target->MetalsHotGas += satellite->MetalsColdGas + satellite->MetalsHotGas;

  target->EjectedGas += satellite->EjectedGas;
  target->MetalsEjectedGas += satellite->MetalsEjectedGas;

  target->ICS += satellite->ICS;
  target->MetalsICS += satellite->MetalsICS;

  target->ICS += satellite->StellarMass;
  target->MetalsICS += satellite->MetalsStellarMass;
}

#endif /* MIMIC_SHARED_SAGE_MERGER_OPS_H */
