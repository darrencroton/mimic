/*
 * Output Helper Functions
 *
 * Purpose: Shared helper functions for property output logic
 * Location: src/modules/shared/output_helpers.h
 *
 * These functions provide reusable output logic patterns that would otherwise
 * require complex conditional expressions in property metadata. They maintain
 * core-physics separation by:
 * - Living in src/modules/shared/ (physics layer, not core)
 * - Being referenced by name in property metadata (no hardcoded physics in core)
 * - Being auto-generated into output code via metadata
 *
 * Usage in property metadata:
 *   output_source: recalculate
 *   output_function: function_name_from_this_file
 *   output_function_arg: "arguments"
 *
 * Architecture Notes:
 * - These functions access galaxy properties (allowed in modules/)
 * - Auto-generated code in copy_to_output.inc calls these functions
 * - Core remains physics-agnostic; all domain logic lives here
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
static inline float output_infall_property_or_zero(const struct Halo *g, float value)
{
    // Satellites: output actual infall property value
    // Centrals: output 0.0 (no infall event)
    return (g->Type != 0) ? value : 0.0f;
}

/*
 * Additional Output Helpers
 *
 * Add new helper functions here as needed. Common patterns:
 * - Type-dependent properties (central vs satellite)
 * - Conditional calculations (merger-triggered properties)
 * - Property combinations (ratios, derived quantities)
 *
 * Guidelines:
 * 1. Keep functions simple and focused (single responsibility)
 * 2. Use inline for performance (these are called per-halo)
 * 3. Document the pattern and which properties use it
 * 4. Make functions testable (pure functions where possible)
 * 5. Use const pointers where appropriate
 */

/**
 * @brief Output Rvir: recalculate current for Type 0/1, preserve for Type 2
 *
 * Type 0/1: Output current virial radius (maintains virial relation)
 * Type 2:   Output preserved virial radius (from when orphan had subhalo)
 *
 * Used by: Rvir property
 */
static inline float output_rvir_conditional(const struct Halo *g)
{
    // Type 2 orphans: return preserved value (no current halo to calculate from)
    // Type 0/1: recalculate current value (stored value is "maximum ever")
    return (g->Type == 2) ? g->Rvir : (float)get_virial_radius(g->HaloNr);
}

/**
 * @brief Output Vvir: recalculate current for Type 0/1, preserve for Type 2
 *
 * Type 0/1: Output current virial velocity (maintains virial relation)
 * Type 2:   Output preserved virial velocity (from when orphan had subhalo)
 *
 * Used by: Vvir property
 */
static inline float output_vvir_conditional(const struct Halo *g)
{
    // Type 2 orphans: return preserved value (no current halo to calculate from)
    // Type 0/1: recalculate current value (stored value is "maximum ever")
    return (g->Type == 2) ? g->Vvir : (float)get_virial_velocity(g->HaloNr);
}

/**
 * @brief Output VelDisp: copy from tree for Type 0/1, preserve for Type 2
 *
 * Type 0/1: Output current velocity dispersion from tree
 * Type 2:   Output preserved velocity dispersion (from when orphan had subhalo)
 *
 * Used by: VelDisp property
 */
static inline float output_veldisp_conditional(const struct Halo *g)
{
    // Type 2 orphans: return preserved value (no current halo in tree)
    // Type 0/1: get current value from InputTreeHalos
    return (g->Type == 2) ? g->VelDisp : InputTreeHalos[g->HaloNr].VelDisp;
}

#endif /* OUTPUT_HELPERS_H */
