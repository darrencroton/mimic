/**
 * @file sage_starburst_physics.h
 * @brief SAGE26's collisional starburst transaction (collisional_starburst_recipe)
 *
 * Ports SAGE26's collisional_starburst_recipe (R/src/model_mergers.c:584-831, R = the
 * staged SAGE26 source) on the R0 to R3 paths: StarburstColdGasOn 1, SFprescription 0 or 1,
 * SupernovaRecipeOn 1, FIREmodeOn 0 or 1, SaveFullSFH 0, CGMrecipeOn 0 or 1, and BulgeSizeOn 0
 * or 3. With CGMrecipeOn 1 the burst's feedback commit and its escaping yield go to the FoF
 * CENTRAL's CGM pair when the central's Regime is 0 and to its hot pair otherwise, and the
 * recheck's quasar wind selects the burst HOST's pair by the host's own Regime
 * (R/src/model_misc.h:67-85, R/src/model_starformation_and_feedback.c:1175-1223,
 * R/src/model_mergers.c:406-428). With SFprescription 1 the star-formation and feedback commits
 * apply their hydrogen caps (sage_sf_feedback_physics.h), the burst refreshes the hydrogen after
 * its feedback (step 7b below) and the recheck's black-hole growth applies its accretion cap
 * (sage_agn_physics.h). One shared function owns SAGE26's whole sequence, for both burst channels:
 *
 *  1. the bursting fraction eburst: the trigger itself for mode 1 (a disk instability) and
 *     0.56 * ratio^0.7 for mode 0 (a merger), as SAGE26's code has them (:600-604; its
 *     header comment at :577 has the two modes reversed);
 *  2. the burst gas: the host's total ColdGas (StarburstColdGasOn 1, :680-683), floored at
 *     zero (:683), times eburst, the stars floored at zero (:685-688);
 *  3. the reheated mass: FeedbackReheatingEpsilon * stars with FIREmodeOn 0 (:711-712); with
 *     FIREmodeOn 1 the FIRE loading capped_eta_reheat(FeedbackReheatingEpsilon * fire_scaling,
 *     Vvir) times the stars (:706-710), the scaling (:690-703) from the burst HOST's (the
 *     target's) own float Vvir and the redshift of its stored snapshot, ZZ[SnapNum], and NO
 *     MassLoading write (sage_fire_feedback_physics.h); both scaled down together when they
 *     exceed the burst gas, with no positive-sum clause (:720-725);
 *  4. the ejected mass from the burst HOST's own float Vvir (:727-749): with FIREmodeOn 0 the
 *     fixed-epsilon formula with that float squared in float; with FIREmodeOn 1 the energy-based
 *     ejection (E_FB - E_lift) / (0.5 * V^2) from the renormalised stars and reheated mass, the
 *     velocity widened to double (:729-734); zero for a non-positive Vvir, then floored at zero;
 *  5. the bulge history sampling: SfrBulge += stars / dt and SfrBulgeColdGas(Metals) +=
 *     the host's ColdGas (metals), accumulated, before any gas is consumed (:754-757);
 *  6. update_from_star_formation with the cold-gas metallicity (:759-760), then the locked
 *     stars recycled_stars = (1 - RecycleFraction) * stars into BulgeMass, MetalsBulgeMass
 *     and the channel the caller selects, MergerBulgeMass or InstabilityBulgeMass, the
 *     instability channel followed by the incremental instability-bulge radius from the
 *     caller's saved disk radius (:770-784, sage_bulge_size_physics.h);
 *  7. update_from_feedback with the recomputed cold metallicity, reheated gas and ejection
 *     going to the FoF central's selected pair, chosen by the central's Regime (:787-791);
 *  7b. for a prescription that tracks H2 (sf_prescription_tracks_h2, :798), the hydrogen
 *     refresh: H2gas capped at the double ColdGas * 0.74 (:799-800), then H1gas overwritten with
 *     the atomic remainder ColdGas * 0.74 - H2gas whatever it held (:801-802), a negative result
 *     floored at zero (:803). It is not a pair of independent caps, and it applies no ionisation
 *     correction: an H1gas the disk star formation reduced for ionisation is restored to the full
 *     atomic remainder, until the next disk SF recomputes it (:794-797);
 *  8. for a merger burst (mode 0) whose ratio is strictly below ThreshMajorMerger, the
 *     post-minor-merger check_disk_instability on the host, whose gas branch grows the
 *     black hole and bursts again in mode 1 (:806-811, R/src/model_disk_instability.c:47-108);
 *  9. only then this burst's metal yield: retained in the host's cold gas (less the
 *     fraction leaving the disk) when the host keeps more than 1e-8 of cold gas and the
 *     ratio is strictly below ThreshMajorMerger, otherwise all of it escapes; the escaping
 *     metals go to the FoF central's selected pair, its CGM metals with CGMrecipeOn 1 and the
 *     central's Regime 0, its hot metals otherwise (:813-830, R/src/model_misc.h:67-74).
 *
 * A burst always runs to its end: there is no early return on a non-positive trigger, zero cold gas
 * or zero stars, so a zero-star burst still samples the bulge history and runs its feedback and
 * yield with zero mass, as SAGE26 does. Only calling-contract NULL checks return early (no R0 call
 * reaches them), and, with FIREmodeOn 1, the refusal of a stored SnapNum outside the run's redshift
 * table, which SAGE26 would read out of bounds (:694): that burst writes nothing and returns -1.
 *
 * The star-formation and feedback commits are the package's one implementation of
 * SAGE26's update_from_star_formation and update_from_feedback
 * (sage_sf_feedback_physics.h), the functions SAGE26's own burst calls.
 *
 * Rounding points. SAGE26's object fuses two multiply-adds in this routine on R0 and R1 (the oracle
 * README's "The c06 Group", Fused Multiply-Adds; the radius update's own are in
 * sage_bulge_size_physics.h): MetalsBulgeMass += metallicity * recycled_stars (:773) and
 * MetalsColdGas += Yield * (1.0 - FracZleaveDiskVal) * stars (:819). Both are written as explicit
 * fma() calls; every other expression is one C expression with SAGE26's operand order and types.
 * With SFprescription 1 the refresh's H1gas = ColdGas * 0.74 - H2gas (:802) is a third, one fused
 * multiply-subtract (fnmsub) in SAGE26's object (the oracle README's "The c08 Group", The Merger
 * Family's Hydrogen Sites), written as fma(ColdGas, 0.74, -H2gas); the refresh's cap compares the
 * widened float H2gas with the unfused double product and stores that product narrowed (:799-800).
 * BulgeMass += recycled_stars (:772) adds the rounded double, unfused. The host's Vvir and the
 * central's Mvir are read as the floats SAGE26 holds (R/src/core_allvars.h:129-131). The FIRE
 * budget (:690-752) fuses no multiply-add in SAGE26's object (the oracle README's "The c09 Group"):
 * each of its expressions is one C expression with SAGE26's operand order and
 * types, in sage_fire_feedback_physics.h.
 *
 * The package's StarFormationRate diagnostic (stars / rate_dt) has no SAGE26 counterpart;
 * it is written beside the commit and changes no SAGE26 field.
 */

#ifndef MIMIC_SHARED_SAGE_STARBURST_PHYSICS_H
#define MIMIC_SHARED_SAGE_STARBURST_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "constants.h"
#include "module_interface.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/sage_agn_physics.h"
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_constants.h"
#include "shared/sage_disk_instability_physics.h"
#include "shared/sage_fire_feedback_physics.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/sage_sfr_history.h"

/* SAGE26's STARBURST_FRAC_COEFF and STARBURST_MASS_POWER, the merger burst's
 * eburst = 0.56 * mass_ratio^0.7 (R/src/model_mergers.c:50-51, :603) */
#define SAGE_STARBURST_FRAC_COEFF 0.56
#define SAGE_STARBURST_MASS_POWER 0.7

/**
 * @brief Parameter bundle for the collisional starburst and its post-merger recheck
 *
 * Five members drive the post-minor-merger instability recheck (step 8 above): SAGE26 runs
 * it when DiskInstabilityOn is set, and grows the black hole in its gas branch when
 * AGNrecipeOn is positive (R/src/model_mergers.c:807, R/src/model_disk_instability.c:102).
 * The last, bulge_size, is SAGE26's BulgeSizeOn and Hubble_h, which the burst's and the
 * recheck's radius updates read (R/src/model_misc.c:395, :398). sf_prescription is SAGE26's
 * SFprescription, which the commits' hydrogen caps, the hydrogen refresh and the recheck's
 * black-hole accretion cap test. fire_mode_on is SAGE26's FIREmodeOn and redshift_config the run's
 * redshift table (ZZ and Snaplistlen), from which a FIREmodeOn 1 burst reads the redshift of its
 * host's stored snapshot (R/src/model_mergers.c:692-694); every burst, the recheck's included,
 * receives both through this bundle. cgm_recipe_on is SAGE26's CGMrecipeOn, which the burst's
 * feedback commit and yield (with the FoF central's Regime) and the recheck's quasar wind (with the
 * host's Regime) read; every burst, the recheck's included, receives it through this bundle. A
 * zero-initialised bundle is BulgeSizeOn 0, SFprescription 0, FIREmodeOn 0 and CGMrecipeOn 0.
 */
struct MimicStarburstParams {
  double feedback_reheating_epsilon;
  double feedback_ejection_efficiency;
  double recycle_fraction;
  double yield;
  double frac_z_leave_disk;
  double threshold_major_merger;
  double energy_sn_code;
  double eta_sn_code;
  int disk_instability_on;              /**< SAGE26's DiskInstabilityOn: the recheck runs */
  double star_forming_disk_factor;      /**< SAGE26's TOOMRE_DISK_FACTOR (3.0) for the recheck */
  int agn_recipe_on;                    /**< SAGE26's AGNrecipeOn > 0: the recheck grows the BH */
  double black_hole_growth_rate;        /**< BlackHoleGrowthRate, for the recheck's BH growth */
  double quasar_mode_efficiency;        /**< QuasarModeEfficiency, for the recheck's wind */
  struct MimicSageBulgeSize bulge_size; /**< BulgeSizeOn and Hubble_h, for the radius updates */
  int sf_prescription; /**< SFprescription (0 or 1): the commits' and BH H caps, the refresh */
  int fire_mode_on;    /**< FIREmodeOn (0 or 1): the burst's FIRE budget (:690-752) */
  const struct MimicConfig *redshift_config; /**< ZZ and Snaplistlen, FIREmodeOn 1's stored-snapshot
                                                  redshift (:694); unread for FIREmodeOn 0 */
  int cgm_recipe_on; /**< CGMrecipeOn (0 or 1): the feedback's, the yield's and the recheck wind's
                          pair selection (R/src/model_misc.h:69, :78; R/src/model_mergers.c:406) */
};

/**
 * @brief The FIRE budget one burst received (R/src/model_mergers.c:690-752), for tests
 *
 * fire_mode is the bundle's FIREmodeOn (-1 where the burst did not run). With FIREmodeOn 1,
 * snapnum and vvir are the host's stored SnapNum and its float Vvir widened to double, the inputs
 * the scaling read (:694-695), and l holds the shared helpers' locals; otherwise snapnum is -1,
 * vvir NaN and l not formed (NaN and -1).
 */
struct MimicSageBurstFireRecord {
  int fire_mode;                /**< FIREmodeOn of the burst, -1 where it did not run */
  int snapnum;                  /**< the host's stored SnapNum read for ZZ (:694), -1 if unread */
  double vvir;                  /**< the host's float Vvir widened (:695), NaN if unread */
  struct MimicSageFireLocals l; /**< the scaling, loading, coupling and energies */
};

/** @brief Mark a burst's FIRE record as not formed */
static inline void mimic_sage_burst_fire_record_reset(struct MimicSageBurstFireRecord *r) {
  r->fire_mode = -1;
  r->snapnum = -1;
  r->vvir = NAN;
  mimic_sage_fire_locals_init(&r->l);
}

/**
 * @brief The locals of the burst's hydrogen refresh (R/src/model_mergers.c:798-803)
 *
 * tracks_h2 is the site's guard, sf_prescription_tracks_h2(SFprescription); where it is false
 * the doubles are NaN and the flags -1.
 */
struct MimicSageBurstHydrogenRefreshLocals {
  int tracks_h2;     /**< sf_prescription_tracks_h2(SFprescription) (:798) */
  double h2_budget;  /**< the double ColdGas * HYDROGEN_MASS_FRAC (:799-800) */
  double h1_refresh; /**< the fused double ColdGas * HYDROGEN_MASS_FRAC - H2gas (:801-802) */
  int h2_cap_binds;  /**< 1 if H2gas > h2_budget: H2gas set to h2_budget narrowed (:799-800) */
  int h1_floored;    /**< 1 if the stored H1gas < 0.0: set to 0 (:803) */
};

/**
 * @brief The locals of one collisional_starburst_recipe call, for tests that compare them
 *
 * Each value is SAGE26's double local of the same meaning. A value the call does not form
 * is left as NaN, a flag it does not evaluate as -1.
 */
struct MimicSageStarburstLocals {
  double eburst;            /**< bursting fraction (:600-604) */
  double gas_for_starburst; /**< burst gas after its floor (:680-683) */
  double stars_pre_renorm;  /**< eburst * gas, after the floor (:685-688) */
  double
      reheated_mass_pre_renorm;  /**< FeedbackReheatingEpsilon * stars (:712), or with
                                      FIREmodeOn 1 the capped loading times the stars (:707-710) */
  double renorm_fac;             /**< gas / (stars + reheated), where it applies (:722) */
  double stars;                  /**< stars after the renormalisation (:721-725) */
  double reheated_mass;          /**< reheated mass after the renormalisation */
  double ejected_mass_pre_floor; /**< the ejection formula before its floor (:731-743); not
                                      formed where the host's Vvir is not positive */
  double ejected_mass;           /**< the ejection after its floor (:746-748) */
  double metallicity_sf;         /**< cold metallicity of the star formation (:759) */
  double recycled_stars;         /**< (1 - RecycleFraction) * stars (:770) */
  double metallicity_fb;         /**< cold metallicity of the feedback (:788) */
  double frac_z_leave_disk_val;  /**< FracZleaveDisk * exp(-Mvir / 30), minor yield (:816) */
  double yield_metals_to_hot;    /**< metals_leaving_disk or all_metals (:822, :828), added to the
                                      central's selected metals */
  int gas_floored;               /**< 1 if the burst gas was negative (:683) */
  int stars_floored;             /**< 1 if the stars were negative (:686) */
  int renorm_active;             /**< 1 if stars + reheated > gas (:721) */
  int ejection_vvir_nonpositive; /**< 1 if the host's Vvir > 0.0 failed (:728) */
  int ejected_floored;           /**< 1 if the ejection was negative (:746) */
  int instability_recheck_ran;   /**< 1 if the post-minor-merger recheck ran (:807-808) */
  int yield_minor_branch;        /**< 1 if the yield took the retained branch (:814) */
  int yield_reservoir_cgm;       /**< 1 if the escaping yield went to the central's CGM metals */
  struct MimicSageHydrogenCapLocals sf_caps; /**< the burst's update_from_star_formation cap */
  struct MimicSageFeedbackLocals feedback;   /**< the burst's update_from_feedback locals */
  struct MimicSageBurstHydrogenRefreshLocals hydrogen_refresh; /**< the refresh (:798-803) */
  struct MimicSageHydrogenCapLocals recheck_bh_caps; /**< the hydrogen cap of the recheck's
                                                          black-hole growth (:366-372 through
                                                          R/src/model_disk_instability.c:102-104);
                                                          all -1/NaN where that growth did not
                                                          run, tracks_h2 0 for SFprescription 0 */
  struct MimicSageBurstFireRecord fire;              /**< this burst's FIRE budget (:690-752) */
  struct MimicSageBurstFireRecord recheck_fire;      /**< the FIRE budget of the recheck's own burst
                                                          (R/src/model_disk_instability.c:106-108);
                                                          not formed where that burst did not run */
  /* The routing of the recheck's own sites, for tests only: a later site can re-establish the end
   * state a whole-state comparison sees, so each records its own selection */
  struct MimicSageQuasarWindLocals recheck_wind; /**< the recheck's black-hole wind, selected by
                                                      the host's Regime (:406); not formed (NaN,
                                                      -1) where that growth did not run */
  int recheck_feedback_reservoir_cgm; /**< the recheck burst's feedback selection (the central's
                                           Regime), -1 where that burst did not run */
  int recheck_yield_reservoir_cgm;    /**< the recheck burst's yield selection, -1 likewise */
};

/** @brief Mark every value of a burst locals record as not formed */
static inline void mimic_sage_starburst_locals_reset(struct MimicSageStarburstLocals *locals) {
  if (locals == NULL) {
    return;
  }
  locals->eburst = NAN;
  locals->gas_for_starburst = NAN;
  locals->stars_pre_renorm = NAN;
  locals->reheated_mass_pre_renorm = NAN;
  locals->renorm_fac = NAN;
  locals->stars = NAN;
  locals->reheated_mass = NAN;
  locals->ejected_mass_pre_floor = NAN;
  locals->ejected_mass = NAN;
  locals->metallicity_sf = NAN;
  locals->recycled_stars = NAN;
  locals->metallicity_fb = NAN;
  locals->frac_z_leave_disk_val = NAN;
  locals->yield_metals_to_hot = NAN;
  locals->gas_floored = -1;
  locals->stars_floored = -1;
  locals->renorm_active = -1;
  locals->ejection_vvir_nonpositive = -1;
  locals->ejected_floored = -1;
  locals->instability_recheck_ran = -1;
  locals->yield_minor_branch = -1;
  locals->sf_caps.tracks_h2 = -1;
  locals->sf_caps.max_h = NAN;
  locals->sf_caps.h2_cap_binds = -1;
  locals->sf_caps.h1_cap_binds = -1;
  locals->hydrogen_refresh.tracks_h2 = -1;
  locals->hydrogen_refresh.h2_budget = NAN;
  locals->hydrogen_refresh.h1_refresh = NAN;
  locals->hydrogen_refresh.h2_cap_binds = -1;
  locals->hydrogen_refresh.h1_floored = -1;
  locals->recheck_bh_caps.tracks_h2 = -1;
  locals->recheck_bh_caps.max_h = NAN;
  locals->recheck_bh_caps.h2_cap_binds = -1;
  locals->recheck_bh_caps.h1_cap_binds = -1;
  mimic_sage_burst_fire_record_reset(&locals->fire);
  mimic_sage_burst_fire_record_reset(&locals->recheck_fire);
  locals->feedback.reservoir_cgm = -1;
  locals->yield_reservoir_cgm = -1;
  mimic_sage_quasar_wind_locals_reset(&locals->recheck_wind);
  locals->recheck_feedback_reservoir_cgm = -1;
  locals->recheck_yield_reservoir_cgm = -1;
}

/**
 * @brief SAGE26's collisional_starburst_recipe on the R0 to R3 paths
 *        (R/src/model_mergers.c:584-831)
 *
 * @param mass_ratio            The trigger: the merger mass ratio (mode 0) or the unstable gas
 *                              fraction (mode 1), SAGE26's mass_ratio
 * @param host_halo             Halo of the bursting galaxy (SAGE26's merger_centralgal); its
 *                              float Vvir sets the ejection (and with FIREmodeOn 1 the scaling and
 *                              the loading cap, with its stored SnapNum's redshift), its Vmax the
 *                              recheck
 * @param central_halo          Halo of the FoF central (SAGE26's centralgal), which receives the
 *                              reheated and ejected gas and the escaping metals; its float Mvir
 *                              sets the fraction of metals leaving the disk; may be host_halo
 * @param mode                  1 = disk instability, 0 = merger (R/src/model_mergers.c:600)
 * @param burst_to_merger_bulge 1 credits the locked stars to MergerBulgeMass, 0 to
 *                              InstabilityBulgeMass (:775-782)
 * @param old_disk_radius       The disk radius the caller saved, SAGE26's old_disk_radius
 *                              argument (:585-586): the instability channel's radius update
 *                              uses it (:783-784)
 * @param rate_dt               Interval for the package's StarFormationRate diagnostic (0 skips it)
 * @param sfr_bin               History bin of the substep (mimic_sage_sfr_step_bin), SAGE26's step
 * @param sfr_dt                The substep interval, SAGE26's dt, the bulge history's denominator;
 *                              the callers validate it (SAGE26 asserts dt > 0, :590-591), and a
 *                              non-positive one samples nothing
 * @param ctx                   Module context: G for the recheck, UnitVelocity_in_cm_per_s for
 *                              its wind
 * @param p                     Parameter bundle (struct MimicStarburstParams)
 * @param locals                Optional (NULL in production): receives this burst's locals,
 *                              with the hydrogen cap of the recheck's black-hole growth and the
 *                              FIRE record of the recheck's own burst
 * @return 0; -1 when a FIREmodeOn 1 burst (this one or the recheck's) refuses its host's stored
 *         SnapNum, with an error logged; this burst then writes nothing (the recheck's
 *         refusal comes after this burst's commits)
 */
static inline int mimic_apply_collisional_starburst(
    const double mass_ratio, struct Halo *host_halo, struct Halo *central_halo, const int mode,
    const int burst_to_merger_bulge, const double old_disk_radius, const double rate_dt,
    const int sfr_bin, const double sfr_dt, const struct ModuleContext *ctx,
    const struct MimicStarburstParams *p, struct MimicSageStarburstLocals *locals) {
  mimic_sage_starburst_locals_reset(locals);
  /* Calling-contract guards only: SAGE26 is always called with both galaxies and its run
   * parameters; no R0 call reaches this return */
  if (host_halo == NULL || host_halo->galaxy == NULL || central_halo == NULL ||
      central_halo->galaxy == NULL || ctx == NULL || p == NULL) {
    return 0;
  }

  struct GalaxyData *gal = host_halo->galaxy;
  struct GalaxyData *central_gal = central_halo->galaxy;

  /* SAGE parity: mode 1 bursts the instability's fraction itself and mode 0 the merger
   * scaling, as SAGE26's code has them (R/src/model_mergers.c:600-604); its comment at
   * :577 states the reverse */
  double eburst;
  if (mode == 1) {
    eburst = mass_ratio;
  } else {
    eburst = SAGE_STARBURST_FRAC_COEFF * pow(mass_ratio, SAGE_STARBURST_MASS_POWER);
  }

  /* SAGE parity: StarburstColdGasOn 1 bursts the host's total ColdGas, floored at zero
   * before the efficiency is applied (R/src/model_mergers.c:680-683) */
  double gas_for_starburst = gal->ColdGas;
  const int gas_floored = gas_for_starburst < 0.0;
  if (gas_floored) {
    gas_for_starburst = 0.0;
  }

  double stars = eburst * gas_for_starburst;
  const int stars_floored = stars < 0.0;
  if (stars_floored) {
    stars = 0.0;
  }

  /* SAGE parity: the burst's FIRE scaling (FIREmodeOn 1, SupernovaRecipeOn 1) reads the burst
   * HOST's (the target's, merger_centralgal's) own float Vvir and the redshift of ITS stored
   * snapshot, ZZ[SnapNum], never the satellite's or the FoF central's, nor the current halo
   * snapshot's (R/src/model_mergers.c:690-703); ZZ is read before the velocity test, as SAGE26
   * orders it, and an out-of-range SnapNum is refused (D35). FIREmodeOn 0 leaves it 0, unread. */
  const float host_vvir = (float)host_halo->Vvir;
  struct MimicSageBurstFireRecord fire_record;
  mimic_sage_burst_fire_record_reset(&fire_record);
  fire_record.fire_mode = p->fire_mode_on;
  double fire_scaling = 0.0;
  if (p->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    double z_fire = 0.0;
    if (mimic_sage_fire_stored_redshift(p->redshift_config, host_halo->SnapNum, &z_fire) != 0) {
      ERROR_LOG("collisional starburst: no FIRE scaling for the burst host, HaloNr %lld",
                (long long)host_halo->HaloNr);
      return -1;
    }
    fire_record.snapnum = host_halo->SnapNum;
    fire_record.vvir = host_vvir;
    fire_scaling = mimic_sage_fire_scaling(host_vvir, z_fire, &fire_record.l);
  }

  /* SN feedback of the burst (SupernovaRecipeOn 1): with FIREmodeOn 1 the capped FIRE loading,
   * from the unfloored float Vvir widened, times the stars (:706-710); SAGE parity: unlike the
   * disk path, the burst does NOT record the loading in MassLoading (:707-710). FIREmodeOn 0:
   * FeedbackReheatingEpsilon * stars (:711-712). */
  double reheated_mass;
  if (p->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
    reheated_mass =
        mimic_sage_capped_eta_reheat(p->feedback_reheating_epsilon * fire_scaling, host_vvir,
                                     p->eta_sn_code, p->energy_sn_code, &fire_record.l) *
        stars;
  } else {
    reheated_mass = p->feedback_reheating_epsilon * stars;
  }
  const double stars_pre_renorm = stars;
  const double reheated_mass_pre_renorm = reheated_mass;

  /* Cannot use more gas than is available for the burst (:720-725); SAGE parity: the burst's
   * guard has no positive-sum clause, unlike the disk path's
   * (R/src/model_starformation_and_feedback.c:775) */
  double renorm_fac = NAN;
  const int renorm_active = (stars + reheated_mass) > gas_for_starburst;
  if (renorm_active) {
    renorm_fac = gas_for_starburst / (stars + reheated_mass);
    stars *= renorm_fac;
    reheated_mass *= renorm_fac;
  }

  /* SAGE parity: the ejection uses the burst HOST's own Vvir, not the FoF central's
   * (R/src/model_mergers.c:727-743); the gas it ejects still leaves the central's selected
   * pair, chosen by the central's Regime (update_from_feedback below). A non-positive Vvir ejects
   * nothing in either mode (:728). */
  double ejected_mass;
  const int ejection_vvir_nonpositive = !(host_vvir > 0.0);
  if (!ejection_vvir_nonpositive) {
    if (p->fire_mode_on == MIMIC_SAGE_FIRE_MODE_ON) {
      /* FIREmodeOn 1: the energy-based ejection from the renormalised stars and reheated mass,
       * the float Vvir widened to double and squared in double (:729-734) */
      const double coupling = mimic_sage_sn_energy_coupling(
          fire_scaling, p->feedback_ejection_efficiency, &fire_record.l);
      ejected_mass =
          mimic_sage_fire_energy_ejection(coupling, stars, reheated_mass, host_vvir, p->eta_sn_code,
                                          p->energy_sn_code, &fire_record.l);
    } else {
      /* SAGE parity: with FIREmodeOn 0 only, the float Vvir is squared in float, as SAGE26's
       * float member makes it (:738-743) */
      ejected_mass = (p->feedback_ejection_efficiency * (p->eta_sn_code * p->energy_sn_code) /
                          (host_vvir * host_vvir) -
                      p->feedback_reheating_epsilon) *
                     stars;
    }
  } else {
    ejected_mass = 0.0;
  }
  /* The formula's value, which the else branch does not form */
  const double ejected_mass_pre_floor = ejection_vvir_nonpositive ? NAN : ejected_mass;
  const int ejected_floored = ejected_mass < 0.0;
  if (ejected_floored) {
    ejected_mass = 0.0;
  }

  /* SAGE parity: the bulge history is sampled once the burst mass is final and before
   * update_from_star_formation, its cold gas and metals accumulated rather than assigned,
   * for every burst, zero stars included (R/src/model_mergers.c:754-757) */
  if (sfr_dt > 0.0) {
    (void)mimic_sage_sample_bulge_sfr(gal, sfr_bin, stars, sfr_dt);
  }

  const double metallicity_sf = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  mimic_sage_update_from_star_formation(gal, stars, metallicity_sf, p->recycle_fraction,
                                        p->sf_prescription,
                                        locals != NULL ? &locals->sf_caps : NULL);

  /* SAGE parity: the locked stars are one rounded double, recycled_stars, added to the
   * bulge and to the selected channel (R/src/model_mergers.c:770-782); SAGE26's object
   * fuses the bulge-metal update into one multiply-add (:773). (1 - RecycleFraction) is
   * formed from the int literal 1, which converts exactly to 1.0. The instability channel is
   * followed at once, before the feedback, by the incremental radius from the caller's saved
   * disk radius (:778-784), which returns at once in BulgeSizeOn 0 (R/src/model_misc.c:395);
   * the merger channel writes no radius here (:775-777). */
  const double recycled_stars = (1.0 - p->recycle_fraction) * stars;
  gal->BulgeMass += recycled_stars;
  gal->MetalsBulgeMass = (float)fma(metallicity_sf, recycled_stars, (double)gal->MetalsBulgeMass);
  if (burst_to_merger_bulge) {
    gal->MergerBulgeMass += recycled_stars;
  } else {
    gal->InstabilityBulgeMass += recycled_stars;
    mimic_sage_update_instability_bulge_radius(gal, recycled_stars, old_disk_radius, &p->bulge_size,
                                               NULL);
  }

  if (rate_dt > 0.0) {
    gal->StarFormationRate += stars / rate_dt;
  }

  // Recompute the cold metallicity and commit the feedback (:787-791)
  const double metallicity_fb = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  mimic_sage_update_from_feedback(gal, central_gal, reheated_mass, ejected_mass, metallicity_fb,
                                  p->sf_prescription, p->cgm_recipe_on,
                                  locals != NULL ? &locals->feedback : NULL);

  /* SAGE parity: after the feedback and before the instability recheck, a prescription that
   * tracks H2 refreshes the hydrogen (R/src/model_mergers.c:798-803): H2gas is capped at the
   * double ColdGas * 0.74 (the widened float compared with the unfused product, which is
   * narrowed on a binding store, :799-800); H1gas is then OVERWRITTEN with the atomic remainder,
   * fused in SAGE26's object (:801-802, fnmsub), so a stale H1gas is replaced and an ionised
   * fraction the disk star formation removed is restored (no ionisation correction, :794-797);
   * a negative stored H1gas is floored at zero (:803). Every burst runs it, a zero-star burst
   * included. SFprescription 0 (R0, R1) does not track H2 and skips it. */
  struct MimicSageBurstHydrogenRefreshLocals hydrogen_refresh = {0, NAN, NAN, -1, -1};
  hydrogen_refresh.tracks_h2 = mimic_sage_sf_prescription_tracks_h2(p->sf_prescription);
  if (hydrogen_refresh.tracks_h2) {
    const double h2_budget = gal->ColdGas * SAGE_HYDROGEN_MASS_FRAC;
    hydrogen_refresh.h2_budget = h2_budget;
    hydrogen_refresh.h2_cap_binds = gal->H2gas > h2_budget;
    if (hydrogen_refresh.h2_cap_binds) {
      gal->H2gas = (float)h2_budget;
    }
    const double h1_refresh =
        fma((double)gal->ColdGas, SAGE_HYDROGEN_MASS_FRAC, -(double)gal->H2gas);
    hydrogen_refresh.h1_refresh = h1_refresh;
    gal->H1gas = (float)h1_refresh;
    hydrogen_refresh.h1_floored = gal->H1gas < 0.0;
    if (hydrogen_refresh.h1_floored) {
      gal->H1gas = 0.0;
    }
  }

  /* SAGE parity: the post-minor-merger disk-instability check runs inside the burst, BEFORE
   * this burst's metal yield, only for a merger burst (mode 0) whose ratio is strictly below
   * ThreshMajorMerger (R/src/model_mergers.c:806-811): a ratio equal to the threshold is a
   * minor merger (:299) with no recheck. Its gas branch grows the black hole (with its wind and,
   * for SFprescription 1, its hydrogen cap) and bursts the unstable fraction in mode 1 into the
   * instability channel (R/src/model_disk_instability.c:90-108), that burst refreshing the
   * hydrogen in turn. The recheck saves and passes its own disk radius to its stellar
   * transfer's radius update (:68-83), and its burst passes the host's current DiskScaleRadius
   * (:106-107). The recheck's burst takes the same bundle, so with FIREmodeOn 1 its FIRE budget
   * reads the same host's float Vvir and stored SnapNum (R/src/model_disk_instability.c:106).
   * The recheck has no FFBRegime test: SAGE26 skips the instability only for an FFB row's disk
   * call (R/src/model_starformation_and_feedback.c:1004-1008, :1365-1367), and its merger routines
   * do not read FFBRegime, so an FFB target is rechecked as any other. */
  const int instability_recheck_ran =
      p->disk_instability_on && mode == 0 && mass_ratio < p->threshold_major_merger;
  /* The recheck growth's hydrogen-cap locals, for tests only (no effect on the state): the cap
   * is end-state invariant (the recheck's burst re-caps and refreshes the hydrogen after it), so
   * its locals are the only evidence that the growth received SFprescription */
  struct MimicSageHydrogenCapLocals recheck_bh_caps = {-1, NAN, -1, -1};
  /* The recheck burst's FIRE record, for tests only (no effect on the state) */
  struct MimicSageBurstFireRecord recheck_fire;
  mimic_sage_burst_fire_record_reset(&recheck_fire);
  /* The recheck's wind and its burst's feedback and yield selections, for tests only */
  struct MimicSageQuasarWindLocals recheck_wind;
  mimic_sage_quasar_wind_locals_reset(&recheck_wind);
  int recheck_feedback_reservoir_cgm = -1;
  int recheck_yield_reservoir_cgm = -1;
  if (instability_recheck_ran) {
    struct MimicSageInstabilityLocals recheck;
    (void)mimic_sage_check_disk_instability_with_sizes(host_halo, ctx, p->star_forming_disk_factor,
                                                       &p->bulge_size, &recheck);
    if (!isnan(recheck.unstable_gas_fraction)) {
      if (p->agn_recipe_on) {
        struct MimicSageBlackHoleLocals recheck_bh;
        /* SAGE parity: the recheck's wind host is the burst host, whose own regime selects the
         * second test's pair (R/src/model_disk_instability.c:102-104, R/src/model_mergers.c:406) */
        (void)mimic_apply_black_hole_growth(host_halo, recheck.unstable_gas_fraction,
                                            p->black_hole_growth_rate, p->quasar_mode_efficiency,
                                            ctx, p->sf_prescription, p->cgm_recipe_on,
                                            locals != NULL ? &recheck_bh : NULL);
        if (locals != NULL) {
          recheck_bh_caps = recheck_bh.hydrogen_caps;
          recheck_wind = recheck_bh.wind;
        }
      }
      struct MimicSageStarburstLocals recheck_burst;
      if (mimic_apply_collisional_starburst(recheck.unstable_gas_fraction, host_halo, central_halo,
                                            1, 0, (double)gal->DiskScaleRadius, rate_dt, sfr_bin,
                                            sfr_dt, ctx, p,
                                            locals != NULL ? &recheck_burst : NULL) != 0) {
        return -1;
      }
      if (locals != NULL) {
        recheck_fire = recheck_burst.fire;
        recheck_feedback_reservoir_cgm = recheck_burst.feedback.reservoir_cgm;
        recheck_yield_reservoir_cgm = recheck_burst.yield_reservoir_cgm;
      }
    }
  }

  /* SAGE parity: the burst's yield (instantaneous recycling, SNII only): a minor burst
   * whose host keeps more than 1e-8 of cold gas retains (1 - FracZleaveDiskVal) of it in
   * the cold gas and the rest escapes; otherwise all of it escapes (R/src/model_mergers.c:813-830).
   * The condition is strict, so a ratio equal to the threshold makes all the yield escape. The
   * escaping metals, a completed double, go through add_metals_to_hot_reservoir to the FoF
   * CENTRAL's selected metals: its CGM metals with CGMrecipeOn 1 and the central's Regime 0, its
   * hot metals otherwise (:824, :830, R/src/model_misc.h:67-74). SAGE26's object fuses the
   * retained update (:819). */
  double frac_z_leave_disk_val = NAN;
  double yield_metals_to_hot;
  const int yield_minor_branch =
      gal->ColdGas > SAGE_COLD_GAS_YIELD_THRESHOLD && mass_ratio < p->threshold_major_merger;
  if (yield_minor_branch) {
    const float central_mvir = (float)central_halo->Mvir;
    frac_z_leave_disk_val =
        p->frac_z_leave_disk * exp(-1.0 * central_mvir / SAGE_METAL_EJECTION_MVIR_SCALE);
    gal->MetalsColdGas =
        (float)fma(p->yield * (1.0 - frac_z_leave_disk_val), stars, (double)gal->MetalsColdGas);
    yield_metals_to_hot = p->yield * frac_z_leave_disk_val * stars;
  } else {
    yield_metals_to_hot = p->yield * stars;
  }
  const int yield_reservoir_cgm =
      mimic_sage_cgm_reservoir_selected(p->cgm_recipe_on, central_gal->Regime);
  mimic_sage_add_metals_to_hot_reservoir(central_gal, p->cgm_recipe_on, yield_metals_to_hot);

  if (locals != NULL) {
    locals->eburst = eburst;
    locals->gas_for_starburst = gas_for_starburst;
    locals->stars_pre_renorm = stars_pre_renorm;
    locals->reheated_mass_pre_renorm = reheated_mass_pre_renorm;
    locals->renorm_fac = renorm_fac;
    locals->stars = stars;
    locals->reheated_mass = reheated_mass;
    locals->ejected_mass_pre_floor = ejected_mass_pre_floor;
    locals->ejected_mass = ejected_mass;
    locals->metallicity_sf = metallicity_sf;
    locals->recycled_stars = recycled_stars;
    locals->metallicity_fb = metallicity_fb;
    locals->frac_z_leave_disk_val = frac_z_leave_disk_val;
    locals->yield_metals_to_hot = yield_metals_to_hot;
    locals->gas_floored = gas_floored;
    locals->stars_floored = stars_floored;
    locals->renorm_active = renorm_active;
    locals->ejection_vvir_nonpositive = ejection_vvir_nonpositive;
    locals->ejected_floored = ejected_floored;
    locals->instability_recheck_ran = instability_recheck_ran;
    locals->yield_minor_branch = yield_minor_branch;
    locals->yield_reservoir_cgm = yield_reservoir_cgm;
    locals->hydrogen_refresh = hydrogen_refresh;
    locals->recheck_bh_caps = recheck_bh_caps;
    locals->fire = fire_record;
    locals->recheck_fire = recheck_fire;
    locals->recheck_wind = recheck_wind;
    locals->recheck_feedback_reservoir_cgm = recheck_feedback_reservoir_cgm;
    locals->recheck_yield_reservoir_cgm = recheck_yield_reservoir_cgm;
  }
  return 0;
}

#endif /* MIMIC_SHARED_SAGE_STARBURST_PHYSICS_H */
