/**
 * @file sage_metal_yield_physics.h
 * @brief SAGE26's disk star-formation metal yield (instantaneous recycling, SNII only)
 *
 * SAGE26 writes the same disk yield twice in R/src/model_starformation_and_feedback.c (R = the
 * staged SAGE26 source, S = that file): at the end of starformation_and_feedback(), after the
 * disk instability, with the stars compute_sn_feedback renormalised (S:1092-1109), and at the
 * end of the feedback-free-burst path starformation_ffb(), with the stars compute_sn_feedback_ffb
 * renormalised (S:1353-1362):
 *
 *   if ColdGas > 1.0e-8:
 *     FracZleaveDiskVal = FracZleaveDisk * exp(-1.0 * Mvir_central / KD11_METAL_HALO_MASS)
 *     MetalsColdGas            += Yield * (1.0 - FracZleaveDiskVal) * stars
 *     selected metals (central) += Yield * FracZleaveDiskVal * stars
 *   else:
 *     selected metals (central) += Yield * stars
 *
 * The escaping metals go through SAGE26's add_metals_to_hot_reservoir (R/src/model_misc.h:67-74,
 * shared/sage_cgm_routing.h) to the FoF central's selected metals: MetalsCGMgas with CGMrecipeOn 1
 * and the CENTRAL's Regime 0, MetalsHotGas otherwise. This header holds the package's one
 * implementation, moved unchanged from sage26_apply_metal_enrichment so that
 * sage26_ffb_star_formation calls the same code. The burst's yield, which also tests the merger
 * ratio (R/src/model_mergers.c:814-831), stays in shared/sage_starburst_physics.h.
 *
 * Rounding points. SAGE26's reference object fuses the retained metals into one multiply-add on
 * both paths: Yield * (1.0 - FracZleaveDiskVal) left to right, then fused with stars and the
 * widened MetalsColdGas (S:1097; S:1356, model_starformation_and_feedback.o offset 0x1fa0). The
 * package writes it as an explicit fma(). The escaping metals Yield * FracZleaveDiskVal * stars and
 * Yield * stars are products only, formed left to right in their own statements and added to the
 * selected float field as completed doubles (no fusion into the addition), as the object has them.
 */

#ifndef MIMIC_SHARED_SAGE_METAL_YIELD_PHYSICS_H
#define MIMIC_SHARED_SAGE_METAL_YIELD_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "types.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_constants.h"

/**
 * @brief The locals of one disk yield, for tests that compare them with the oracle
 *
 * A double is NAN, and a flag -1, where its branch does not form it.
 */
struct MimicSageDiskYieldLocals {
  int coldgas_above_threshold;  /**< ColdGas > 1.0e-8 (S:1093, S:1353) */
  double frac_z_leave_disk_val; /**< FracZleaveDiskVal (S:1094, S:1354-1355) */
  double metals_to_hot;         /**< metals_leaving_disk (S:1100, S:1358) or all_metals (S:1106,
                                     S:1361), the completed double added to the selected metals */
  int reservoir_cgm;            /**< 1 if the central's CGM metals were selected */
};

/**
 * @brief SAGE26's disk yield (S:1092-1109, S:1353-1362) for `stars` formed by `gal`
 *
 * @param gal                The forming galaxy (SAGE26's p), whose ColdGas is tested and whose
 *                           MetalsColdGas receives the retained metals
 * @param central_gal        Its FoF central (SAGE26's centralgal), whose selected metals receive
 *                           the escaping metals and whose Regime selects them
 * @param central_mvir       The FoF central's Mvir, as SAGE26's float (S:1094, S:1355)
 * @param stars              The stellar mass the yield is formed from (double)
 * @param yield              Yield
 * @param frac_z_leave_disk  FracZleaveDisk
 * @param cgm_recipe_on      CGMrecipeOn (0 or 1)
 * @param locals             Optional: receives the call's locals (NULL in production)
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
    /* SAGE parity: the Krumholz & Dekel (2011) ejection scale uses the FoF central's float Mvir
     * (S:1094, S:1355) */
    const double FracZleaveDiskVal =
        frac_z_leave_disk * exp(-1.0 * central_mvir / SAGE_METAL_EJECTION_MVIR_SCALE);

    /* SAGE parity: metals that stay in the disk, Yield * (1.0 - FracZleaveDiskVal) left to
     * right, then one fused multiply-add with stars in SAGE26's build (S:1097, S:1356) */
    const double yield_retained = yield * (1.0 - FracZleaveDiskVal);
    gal->MetalsColdGas = (float)fma(yield_retained, stars, (double)gal->MetalsColdGas);

    /* SAGE parity: the metals that leave the disk, formed in their own statement, go to the FoF
     * CENTRAL's selected metals by its Regime (S:1100-1102, S:1358-1359,
     * R/src/model_misc.h:67-74) */
    const double metals_leaving_disk = yield * FracZleaveDiskVal * stars;
    mimic_sage_add_metals_to_hot_reservoir(central_gal, cgm_recipe_on, metals_leaving_disk);
    l.frac_z_leave_disk_val = FracZleaveDiskVal;
    l.metals_to_hot = metals_leaving_disk;
  } else {
    // All metals leave the disk when ColdGas is very low, to the same pair (S:1105-1108,
    // S:1360-1362)
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
