/**
 * @file sage_metal_yield_physics.h
 * @brief SAGE26's disk star-formation metal yield (instantaneous recycling, SNII only)
 *
 * SAGE26 writes the same disk yield twice: at the end of starformation_and_feedback(), after the
 * disk instability, with the stars compute_sn_feedback renormalised, and at the end of the
 * feedback-free-burst path starformation_ffb(), with the stars compute_sn_feedback_ffb
 * renormalised:
 *
 *   if ColdGas > 1.0e-8:
 *     FracZleaveDiskVal = FracZleaveDisk * exp(-1.0 * Mvir_central / KD11_METAL_HALO_MASS)
 *     MetalsColdGas            += Yield * (1.0 - FracZleaveDiskVal) * stars
 *     selected metals (central) += Yield * FracZleaveDiskVal * stars
 *   else:
 *     selected metals (central) += Yield * stars
 *
 * The escaping metals go through SAGE26's add_metals_to_hot_reservoir (shared/sage_cgm_routing.h)
 * to the FoF central's selected metals: MetalsCGMgas with CGMrecipeOn 1 and the CENTRAL's Regime
 * 0, MetalsHotGas otherwise. sage26_apply_metal_enrichment and sage26_ffb_star_formation both
 * call this one implementation. The burst's yield, which also tests the merger ratio, is in
 * shared/sage_starburst_physics.h.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_METAL_YIELD_PHYSICS_H
#define MIMIC_SHARED_SAGE_METAL_YIELD_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "types.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_constants.h"

/**
 * @brief The locals of one disk yield, for tests that inspect the intermediate values
 *
 * A double is NAN, and a flag -1, where its branch does not form it.
 */
struct MimicSageDiskYieldLocals {
  int coldgas_above_threshold;  /**< ColdGas > 1.0e-8 */
  double frac_z_leave_disk_val; /**< FracZleaveDiskVal */
  double metals_to_hot;         /**< metals_leaving_disk or all_metals, the completed double
                                     added to the selected metals */
  int reservoir_cgm;            /**< 1 if the central's CGM metals were selected */
};

/**
 * @brief SAGE26's disk yield (starformation_and_feedback, starformation_ffb) for `stars` formed
 *        by `gal`
 *
 * @param gal                The forming galaxy (SAGE26's p), whose ColdGas is tested and whose
 *                           MetalsColdGas receives the retained metals
 * @param central_gal        Its FoF central (SAGE26's centralgal), whose selected metals receive
 *                           the escaping metals and whose Regime selects them
 * @param central_mvir       The FoF central's Mvir, as SAGE26's float
 * @param stars              The stellar mass the yield is formed from (double)
 * @param yield              Yield
 * @param frac_z_leave_disk  FracZleaveDisk
 * @param cgm_recipe_on      CGMrecipeOn (0 or 1)
 * @param locals             Optional, may be NULL: receives the call's locals
 */
static inline void mimic_sage_apply_disk_yield(struct GalaxyData *gal,
                                               struct GalaxyData *central_gal,
                                               const float central_mvir, const double stars,
                                               const double yield, const double frac_z_leave_disk,
                                               const int cgm_recipe_on,
                                               struct MimicSageDiskYieldLocals *locals) {
  struct MimicSageDiskYieldLocals l = {-1, NAN, NAN, -1};
  l.coldgas_above_threshold = gal->ColdGas > SAGE_COLD_GAS_YIELD_THRESHOLD;
  if (l.coldgas_above_threshold) {
    // SAGE parity: the Krumholz & Dekel (2011) ejection scale uses the FoF central's float Mvir
    const double FracZleaveDiskVal =
        frac_z_leave_disk * exp(-1.0 * central_mvir / SAGE_METAL_EJECTION_MVIR_SCALE);

    // SAGE parity: the retained metals are one multiply-add, as SAGE26's build computes them
    const double yield_retained = yield * (1.0 - FracZleaveDiskVal);
    gal->MetalsColdGas = (float)fma(yield_retained, stars, (double)gal->MetalsColdGas);

    // The metals that leave the disk go to the FoF CENTRAL's selected metals by its Regime
    const double metals_leaving_disk = yield * FracZleaveDiskVal * stars;
    mimic_sage_add_metals_to_hot_reservoir(central_gal, cgm_recipe_on, metals_leaving_disk);
    l.frac_z_leave_disk_val = FracZleaveDiskVal;
    l.metals_to_hot = metals_leaving_disk;
  } else {
    // All metals leave the disk when ColdGas is very low, to the same reservoir
    const double all_metals = yield * stars;
    mimic_sage_add_metals_to_hot_reservoir(central_gal, cgm_recipe_on, all_metals);
    l.metals_to_hot = all_metals;
  }
  l.reservoir_cgm = mimic_sage_cgm_reservoir_selected(cgm_recipe_on, central_gal->Regime);
  if (locals != NULL) {
    *locals = l;
  }
}

#endif /* MIMIC_SHARED_SAGE_METAL_YIELD_PHYSICS_H */
