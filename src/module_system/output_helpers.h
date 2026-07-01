/**
 * @file    output_helpers.h
 * @brief   Helper functions for metadata-driven property output conversion
 *
 * These functions are referenced by name in property metadata (`output_function`)
 * and called from auto-generated output code. They keep domain logic out of the
 * generated core, preserving the physics-agnostic boundary.
 *
 * Usage in property metadata:
 *   output_source: recalculate
 *   output_function: function_name_from_this_file
 *   output_function_arg: "arguments"
 *
 * Location: src/module_system/output_helpers.h
 * Framework infrastructure — do not modify unless adding universal output helpers.
 */

#ifndef OUTPUT_HELPERS_H
#define OUTPUT_HELPERS_H

/* Forward declaration - avoid circular includes */
struct Halo;

/*
 * Infall Property Helpers
 *
 * Pattern: Output infall property for satellites, 0.0 for centrals
 * Used by: infallMvir, infallVvir, infallVmax
 *
 * Rationale:
 * - Infall properties only meaningful for satellites (Type != 0)
 * - Central galaxies (Type == 0) have no infall event
 * - Output 0.0 for centrals (distinguishable from -1.0 "unset" sentinel)
 */

/**
 * Output infall property value for satellites, 0.0 for centrals
 *
 * @param g Halo pointer (must not be NULL)
 * @param value The infall property value from the halo
 * @return value if satellite (Type != 0), 0.0 if central (Type == 0)
 *
 * Example usage in metadata:
 *   - name: infallMvir
 *     output_source: recalculate
 *     output_function: output_infall_property_or_zero
 *     output_function_arg: "g, g->infallMvir"
 */
static inline double output_infall_property_or_zero(const struct Halo *g, double value) {
  return (g->Type != 0) ? value : 0.0;
}

/**
 * @brief Output Rvir: recalculate current for Type 0/1, preserve for Type 2
 *
 * Type 0/1: Output current virial radius (maintains virial relation)
 * Type 2:   Output preserved virial radius (from when orphan had subhalo)
 *
 * Used by: Rvir property
 */
static inline double output_rvir_conditional(const struct Halo *g) {
  /* Type 2 orphans: preserve value from when the orphan had a subhalo;
   * Type 0/1: recalculate current (stored value is "maximum ever") */
  return (g->Type == 2) ? g->Rvir : get_virial_radius(g->HaloNr);
}

/**
 * @brief Output Vvir: recalculate current for Type 0/1, preserve for Type 2
 *
 * Type 0/1: Output current virial velocity (maintains virial relation)
 * Type 2:   Output preserved virial velocity (from when orphan had subhalo)
 *
 * Used by: Vvir property
 */
static inline double output_vvir_conditional(const struct Halo *g) {
  /* Type 2 orphans: preserve value from when the orphan had a subhalo;
   * Type 0/1: recalculate current (stored value is "maximum ever") */
  return (g->Type == 2) ? g->Vvir : get_virial_velocity(g->HaloNr);
}

#endif /* OUTPUT_HELPERS_H */
