/**
 * @file    sage_positive_agn_feedback.c
 * @brief   SAGE positive AGN feedback (high-redshift AGN-triggered star formation)
 *
 * Implements the "positive" AGN-feedback phase described by
 *
 *   Silk, Begelman, Norman, Nusser & Wyse (2024),
 *   "Which came first: supermassive black holes or galaxies? Insights from JWST",
 *   arXiv:2401.02482.
 *
 * The central claim of that paper (Abstract, quoted verbatim) is:
 *
 *   "Insights from JWST observations suggest that AGN feedback evolved from a
 *    short-lived, high redshift phase in which radiatively cooled turbulence
 *    and/or momentum-conserving outflows stimulated vigorous early star
 *    formation ('positive' feedback), to late, energy-conserving outflows that
 *    depleted halo gas reservoirs and quenched star formation. The transition
 *    between these two regimes occurred at z~6, independently of galaxy mass,
 *    for simple assumptions about the outflows and star formation process."
 *
 * So the *same* AGN that quenches galaxies at late times (already modelled in
 * Mimic by sage_radio_mode_heating and sage_quasar_mode) is, at high redshift,
 * a net *driver* of star formation: its momentum-conserving outflow shocks the
 * dense, high-column ISM, that shocked gas cools radiatively, and the resulting
 * cold dense phase forms stars vigorously.
 *
 * --------------------------------------------------------------------------
 * Which regime are we in? (the column-density criterion)
 * --------------------------------------------------------------------------
 * The paper decides "positive vs negative" by comparing the gas column the
 * outflow has to plough through against the column above which the shocked gas
 * can cool (momentum-conserving), rather than stay hot and blow out
 * (energy-conserving).
 *
 *   - Gas column of the host, §3.3, quoted verbatim:
 *       "Empirical data on interstellar medium column densities in massive and
 *        AGN host galaxies suggest that the gas column evolves with redshift
 *        approximately as (1+z)^3.3, and is some 100 times larger than local
 *        values at z=4-6. The estimated normalization is
 *           N_H = 10^21 (1+z)^3.3 cm^-2."
 *
 *   - Cooling / transition column, §5.1, quoted verbatim:
 *       "The minimum obscuration along the line of sight for transitioning from
 *        momentum-driven to energy-driven outflows is
 *           N_cool ~ 10^23 cm^-2 (v_s/3000 km s^-1)^2,
 *        where v_s is the feedback outflow velocity."
 *
 * When N_H > N_cool the shocked gas is optically thick enough to cool: the
 * outflow stays momentum-conserving and feedback is POSITIVE. When N_H < N_cool
 * the flow goes energy-conserving and feedback turns NEGATIVE (handled
 * elsewhere). Because N_H(z) carries no galaxy-mass dependence and the
 * threshold is set by the *outflow* (feedback) physics rather than the halo,
 * the crossover redshift is essentially mass-independent — the paper's headline
 * "z~6, independently of galaxy mass".
 *
 * We expose the transition column N_cool as a single tunable parameter
 * (PositiveFeedbackColumnThreshold). The default 6e23 cm^-2 sits within §5.1's
 * ~10^23-10^24 cm^-2 range (the multi-layer cooled column the paper invokes) and
 * is chosen so the crossover N_H(z) = N_cool lands at the paper's z~6:
 *
 *     10^21 (1+z)^3.3 = 6x10^23   =>   (1+z)^3.3 = 600   =>   z ~ 6.
 *
 * We turn the criterion into a smooth, dimensionless "positivity" weight
 *
 *     f_pos(z) = N_H(z) / (N_H(z) + N_cool)   in [0,1],
 *
 * which -> 1 at high z (deep in the positive regime), -> 0 at low z, and equals
 * 1/2 exactly at the crossover. This avoids a hard on/off switch at the
 * snapshot grid while preserving the paper's transition.
 *
 * --------------------------------------------------------------------------
 * How much extra star formation? (the triggered SF rate)
 * --------------------------------------------------------------------------
 * Silk et al. (2024) is an analytic, order-of-magnitude paper: it argues *that*
 * the momentum-conserving outflow "stimulated vigorous early star formation"
 * but does not hand down a closed-form SFR-boost law for a semi-analytic model.
 * We therefore make a deliberate, clearly-flagged modelling choice consistent
 * with the paper's mechanism: while in the positive regime the AGN outflow
 * compresses the cold disc and forms stars on the disc dynamical time, scaled
 * by an efficiency and by f_pos:
 *
 *     SFR_trig = PositiveFeedbackEfficiency * f_pos * ColdGas / t_dyn,
 *     t_dyn    = DiskScaleRadius / Vvir.
 *
 * This mirrors Mimic's quiescent Kennicutt-Schmidt law
 * (sage_calculate_star_formation: strdot = eff * (ColdGas - crit) / t_dyn) so
 * the triggered term is a like-for-like *enhancement* of the disc SF, switched
 * on only where an AGN exists (BlackHoleMass > 0) and only at the redshifts the
 * paper identifies as positive (f_pos -> 1). The shock velocity v_s is taken to
 * be the virial speed, matching §2's "shocks at close to the virial speed,
 * v_s ~ 600 (M10/r150)^1/2 km s^-1" — which is just Vvir for a virialised halo.
 *
 * Implementation note: the triggered stars are ADDED to the NewStellarMass
 * transport field and committed by sage_apply_star_formation_supernova together
 * with the quiescent stars, so recycling, metal enrichment and StarFormationRate
 * bookkeeping all happen once, in one place (no duplicated reservoir logic).
 * Because we run AFTER sage_calculate_supernova_feedback, the triggered stars
 * are a net high-z boost and are not themselves immediately SN-reheated.
 *
 * Reference: Silk et al. (2024), arXiv:2401.02482 (positive AGN feedback).
 *            Croton et al. (2006, 2016) for the host SAGE SF/feedback model.
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"

// ============================================================================
// PAPER CONSTANTS (Silk et al. 2024) — fixed, quoted in comments above
// ============================================================================

/* §3.3: N_H = 10^21 (1+z)^3.3 cm^-2 — host ISM column vs redshift. */
static const double NH_NORMALISATION_CM2 = 1.0e21; /* N_H at z=0 (cm^-2) */
static const double NH_REDSHIFT_EXPONENT = 3.3;    /* (1+z) power law      */

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/* Strength of the AGN-triggered star formation enhancement (our modelling
 * choice for the paper's "vigorous early star formation"). */
static double POSITIVE_FEEDBACK_EFFICIENCY;

/* §5.1 transition column N_cool (cm^-2): momentum->energy (positive->negative)
 * crossover. Paper scale ~10^24 cm^-2 places the transition near z~6. */
static double POSITIVE_FEEDBACK_COLUMN_THRESHOLD;

// ============================================================================
// PHYSICS HELPERS
// ============================================================================

/**
 * @brief Positive-feedback weight f_pos(z) in [0,1] from the column criterion.
 *
 * Silk et al. (2024) §3.3 host column vs §5.1 transition column:
 *   N_H(z) = 10^21 (1+z)^3.3 cm^-2     (rises steeply towards high z)
 *   f_pos  = N_H / (N_H + N_cool)      (=1/2 at the N_H=N_cool crossover)
 *
 * High z -> f_pos~1 (positive, momentum-conserving, SF-boosting);
 * low  z -> f_pos~0 (negative regime, handled by radio/quasar modes).
 */
static double positive_feedback_weight(const double redshift, const double n_cool)
{
    if (n_cool <= 0.0) {
        return 0.0;
    }

    const double one_plus_z = 1.0 + redshift;
    if (one_plus_z <= 0.0) {
        return 0.0;
    }

    const double n_h = NH_NORMALISATION_CM2 * pow(one_plus_z, NH_REDSHIFT_EXPONENT);
    return n_h / (n_h + n_cool);
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_positive_agn_feedback_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("PositiveFeedbackEfficiency",
                                      POSITIVE_FEEDBACK_EFFICIENCY, 0.0, 10.0,
                                      "AGN-triggered star formation efficiency");

    /* Column threshold spans many orders of magnitude (cm^-2); validate loosely
     * but reject non-physical (<=0) values. Paper scale ~10^24 cm^-2. */
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("PositiveFeedbackColumnThreshold",
                                      POSITIVE_FEEDBACK_COLUMN_THRESHOLD,
                                      0.0, 1.0e30,
                                      "positive->negative transition column N_cool (cm^-2)");

    /* This module only *produces* triggered stars into NewStellarMass; the
     * commit (recycling, metals, SFR accounting) is done by the apply step.
     * Without it the triggered SF is silently discarded each substep. */
    if (!module_configured_anywhere("sage_apply_star_formation_supernova")) {
        ERROR_LOG("sage_positive_agn_feedback requires "
                  "sage_apply_star_formation_supernova in the pipeline — without it "
                  "the AGN-triggered NewStellarMass is computed but never committed "
                  "to galaxy reservoirs (silent output loss)");
        return -1;
    }

    /* Ordering: must run AFTER the quiescent SF prescription (so it adds to,
     * rather than is overwritten by, NewStellarMass) and BEFORE the apply step
     * (so its contribution is committed this substep). */
    if (module_configured_anywhere("sage_calculate_star_formation") &&
        !module_precedes_in_phase("sage_calculate_star_formation",
                                  "sage_positive_agn_feedback",
                                  MimicConfig.phase_1, MimicConfig.num_phase_1)) {
        ERROR_LOG("sage_positive_agn_feedback must follow sage_calculate_star_formation "
                  "in phase_1 — otherwise the quiescent SF step overwrites the "
                  "triggered contribution in NewStellarMass");
        return -1;
    }
    if (!module_precedes_in_phase("sage_positive_agn_feedback",
                                  "sage_apply_star_formation_supernova",
                                  MimicConfig.phase_1, MimicConfig.num_phase_1)) {
        ERROR_LOG("sage_positive_agn_feedback must precede "
                  "sage_apply_star_formation_supernova in phase_1 — otherwise the "
                  "triggered stars are added after the commit and lost");
        return -1;
    }

    INFO_LOG("SAGE positive AGN feedback module initialized (Silk et al. 2024)");
    VERBOSE_LOG("  PositiveFeedbackEfficiency      = %.4f", POSITIVE_FEEDBACK_EFFICIENCY);
    VERBOSE_LOG("  PositiveFeedbackColumnThreshold = %.3e cm^-2", POSITIVE_FEEDBACK_COLUMN_THRESHOLD);
    return 0;
}

int sage_positive_agn_feedback_process(struct ModuleContext *ctx,
                                       struct Halo *halos, int ngal)
{
    double dt = 0.0;
    enum MimicObjectTimeStatus dt_status;

    if (ngal != 1) {
        ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    if (halo->galaxy == NULL) {
        return 0;
    }
    struct GalaxyData *gal = halo->galaxy;

    /* Positive feedback is AGN-driven: no black hole means no AGN outflow to
     * shock-compress the ISM, so there is nothing to trigger. We also need cold
     * gas to convert and a finite disc to set the dynamical time / virial speed. */
    if (gal->BlackHoleMass <= 0.0 || gal->ColdGas <= EPSILON_SMALL ||
        gal->DiskScaleRadius <= 0.0 || halo->Vvir <= 0.0) {
        return 0;
    }

    /* Are we in the positive (momentum-conserving) regime for this galaxy? */
    const double f_pos = positive_feedback_weight((double)ctx->redshift,
                                                  POSITIVE_FEEDBACK_COLUMN_THRESHOLD);
    if (f_pos <= 0.0) {
        return 0;
    }

    dt_status = mimic_object_substep_dt(halo, ctx, &dt);
    if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
        return 0;
    }
    if (dt_status != MIMIC_OBJECT_TIME_OK) {
        ERROR_LOG("Invalid positive-feedback dt for halo %d (SnapNum=%d, dT=%.3e, "
                  "num_substeps=%d, status=%s)",
                  halo->HaloNr, halo->SnapNum, halo->dT,
                  (ctx != NULL) ? ctx->num_substeps : -1,
                  mimic_object_time_status_str(dt_status));
        return -1;
    }

    /* Disc dynamical time, with v_s ~ virial speed (Silk et al. 2024 §2:
     * "shocks at close to the virial speed, v_s ~ 600 (M10/r150)^1/2 km s^-1").
     * Same t_dyn = reff/Vvir form as the quiescent SF law. */
    const double t_dyn = gal->DiskScaleRadius / halo->Vvir;
    if (t_dyn <= 0.0) {
        return 0;
    }

    /* Triggered SF rate: an f_pos-weighted enhancement of the disc SF law,
     * representing the AGN momentum-conserving outflow compressing the cold
     * disc into "vigorous early star formation" (Silk et al. 2024, Abstract). */
    const double strdot_trig = POSITIVE_FEEDBACK_EFFICIENCY * f_pos * gal->ColdGas / t_dyn;

    double stars_trig = strdot_trig * dt;
    if (stars_trig <= 0.0) {
        return 0;
    }
    /* Never form more stars than the available cold gas in this substep. */
    if (stars_trig > gal->ColdGas) {
        stars_trig = gal->ColdGas;
    }

    /* Hand the triggered stars to the shared commit path (apply step), and keep
     * a cumulative diagnostic of how much stellar mass positive feedback made. */
    gal->NewStellarMass += stars_trig;
    gal->AGNTriggeredStellarMass += (float)stars_trig;

    DEBUG_LOG("Type=%d z=%.2f: positive AGN feedback f_pos=%.3f, triggered SF=%.3e "
              "(ColdGas=%.3e, M_BH=%.3e)",
              halo->Type, ctx->redshift, f_pos, stars_trig, gal->ColdGas,
              gal->BlackHoleMass);

    return 0;
}

int sage_positive_agn_feedback_cleanup(void)
{
    VERBOSE_LOG("SAGE positive AGN feedback module cleaned up");
    return 0;
}
