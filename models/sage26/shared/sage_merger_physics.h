/**
 * @file    sage_merger_physics.h
 * @brief   SAGE26's merger transaction pieces on live galaxies (R0 and R1 paths)
 *
 * The pieces of SAGE26's merger transaction that run outside the black-hole growth and
 * the burst (R = the staged SAGE26 source, M = R/src/model_mergers.c):
 *
 * - mimic_sage_merger_mass_ratio(): the mass ratio from the float StellarMass + ColdGas
 *   sums (M:235-249);
 * - mimic_sage_merger_target_disk_dominated(): the target's pre-transfer morphology flag
 *   (M:253-254), which selects the burst's bulge channel;
 * - mimic_sage_merger_burst_channel(): that selection (M:266-282), used by the burst's
 *   merger-event consumer (sage26_starburst_feedback);
 * - mimic_sage_add_galaxies_together(): add_galaxies_together (M:456-543), with its satellite
 *   disk-star radius update (M:516-522, sage_bulge_size_physics.h);
 * - mimic_sage_make_bulge_from_burst(): make_bulge_from_burst (M:553-570);
 * - mimic_sage_merger_remnant_radius(): the static calculate_merger_remnant_radius
 *   (M:135-212), the remnant radius BulgeSizeOn 3 assigns (M:297-330);
 * - mimic_sage_reset_bulge_radii(): get_bulge_radius on R0 (BulgeSizeOn 0,
 *   R/src/model_misc.c:239-247), which zeroes the three bulge radii;
 * - mimic_sage_disrupt_satellite_to_ics(): disrupt_satellite_to_ICS (M:845-902).
 *
 * sage26_resolve_mergers_and_disruption composes the first seven into
 * deal_with_galaxy_merger and calls the last from its pass. Each expression keeps
 * SAGE26's operand order and types: the struct GALAXY members are float, so a sum of two
 * members rounds in float before any widening, and a compound assignment of a double into
 * a float member rounds once into the float. The multiply-adds on these paths that
 * SAGE26's objects fuse are written as explicit fma(): the disruption's ICS_sum_mt +=
 * StellarMass * time (M:871, the oracle README's c06 table) and the remnant radius's three
 * (M:158, :171, :204, its c07 table); no other helper here contains a multiply-add (the
 * table lists none for add_galaxies_together and make_bulge_from_burst; the radius update's
 * own fused site is in sage_bulge_size_physics.h).
 */

#ifndef MIMIC_SHARED_SAGE_MERGER_PHYSICS_H
#define MIMIC_SHARED_SAGE_MERGER_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "types.h"
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_sfr_history.h"

/* SAGE26's exponential-disk half-mass factor and Covington et al. (2011) radiative-loss
 * coefficient, file-static constants of R/src/model_mergers.c (:37, :41) */
#define MIMIC_SAGE_DISK_HALF_MASS_FRAC 1.68
#define MIMIC_SAGE_COVINGTON11_C_RAD 2.75

/**
 * @brief The locals of one merger remnant-radius calculation, for tests that compare them
 *
 * Each double is SAGE26's local of the same name in calculate_merger_remnant_radius
 * (R/src/model_mergers.c:135-212), NAN where the call does not form it; each flag is -1 where
 * the call does not evaluate it. R1_progenitor and R2_progenitor are R1 and R2 as :153-174 form
 * them, before the zero-radius fallbacks (:177-178); R1 and R2 are the values after them.
 */
struct MimicSageRemnantLocals {
  double M1;                /**< g1's float StellarMass + ColdGas, widened (:138) */
  double M2;                /**< g2's float StellarMass + ColdGas, widened (:139) */
  double M_tot;             /**< M1 + M2 (:140) */
  double R1_disk_half;      /**< 1.68 * g1's DiskScaleRadius (:149) */
  double R1_bulge_half;     /**< g1's float BulgeRadius, widened (:150) */
  double M1_disk;           /**< g1's float ColdGas + (StellarMass - BulgeMass), widened (:156) */
  double M1_bulge;          /**< g1's float BulgeMass, widened (:157) */
  double R1_progenitor;     /**< the fused average of :158, or 0.0 (:160) */
  double R2_disk_half;      /**< 1.68 * g2's DiskScaleRadius (:164) */
  double R2_bulge_half;     /**< g2's float BulgeRadius, widened (:165) */
  double M2_disk;           /**< g2's float ColdGas + (StellarMass - BulgeMass), widened (:169) */
  double M2_bulge;          /**< g2's float BulgeMass, widened (:170) */
  double R2_progenitor;     /**< the fused average of :171, or 0.0 (:173) */
  double R1;                /**< R1 after the fallbacks (:177) */
  double R2;                /**< R2 after the fallbacks (:178) */
  double E_init;            /**< M1 * M1 / R1 + M2 * M2 / R2 (:185) */
  double E_orb;             /**< M1 * M2 / (R1 + R2) (:189) */
  double f_gas;             /**< the float g1.ColdGas + g2.ColdGas, widened, over M_tot (:194) */
  double E_rad;             /**< 2.75 * E_init * f_gas (:195) */
  double E_final;           /**< E_init + E_orb + E_rad (:199) */
  double new_merger_radius; /**< the value returned (:142, :179, :204, :209-211) */
  int total_nonpositive;    /**< M_tot <= 0.0: returns 0 (:142) */
  int g1_mass_positive;     /**< g1's float StellarMass + ColdGas > 0 (:153) */
  int g2_mass_positive;     /**< g2's float StellarMass + ColdGas > 0 (:168) */
  int r1_fallback;          /**< R1 <= 0.0: R1 takes R2 (:177) */
  int r2_fallback;          /**< R2 <= 0.0: R2 takes R1 (:178) */
  int both_radii_zero;      /**< R1 <= 0.0 after both fallbacks: returns 0 (:179) */
  int energy_nonpositive;   /**< E_final <= 0.0: the mass-weighted fallback (:202-205) */
};

/** @brief Mark every value of a remnant locals record as not formed */
static inline void mimic_sage_remnant_locals_reset(struct MimicSageRemnantLocals *locals) {
  if (locals == NULL) {
    return;
  }
  double *const values[] = {&locals->M1,
                            &locals->M2,
                            &locals->M_tot,
                            &locals->R1_disk_half,
                            &locals->R1_bulge_half,
                            &locals->M1_disk,
                            &locals->M1_bulge,
                            &locals->R1_progenitor,
                            &locals->R2_disk_half,
                            &locals->R2_bulge_half,
                            &locals->M2_disk,
                            &locals->M2_bulge,
                            &locals->R2_progenitor,
                            &locals->R1,
                            &locals->R2,
                            &locals->E_init,
                            &locals->E_orb,
                            &locals->f_gas,
                            &locals->E_rad,
                            &locals->E_final,
                            &locals->new_merger_radius};
  for (size_t k = 0; k < sizeof(values) / sizeof(values[0]); k++) {
    *values[k] = NAN;
  }
  locals->total_nonpositive = -1;
  locals->g1_mass_positive = -1;
  locals->g2_mass_positive = -1;
  locals->r1_fallback = -1;
  locals->r2_fallback = -1;
  locals->both_radii_zero = -1;
  locals->energy_nonpositive = -1;
}

/**
 * @brief The values and branch flags of one merger's own arithmetic, for tests
 *
 * A double is NAN and a flag -1 where the path does not form it.
 */
struct MimicSageMergerLocals {
  double mi;                /**< smaller float baryon sum, widened (M:235-241) */
  double ma;                /**< larger float baryon sum, widened (M:235-241) */
  double mass_ratio;        /**< M:243-249 */
  int mass_ratio_path;      /**< 0 mi / ma, 1 the ma <= 0 < mi branch, 2 both zero */
  double central_disk_mass; /**< the target's float disk mass before the addition (M:253) */
  int is_disk_dominated;    /**< M:254 */
  int major_merger;         /**< mass_ratio > ThreshMajorMerger (M:268, :299) */
  double old_disk_radius;   /**< the target's float DiskScaleRadius before the addition (M:257) */
  /** BulgeSizeOn 3 only: the remnant radius's locals (M:297); all absent in mode 0, where the
   *  radius is not formed (get_bulge_radius zeroes every radius after it) */
  struct MimicSageRemnantLocals remnant;
};

/**
 * @brief The values and branch flags of one add_galaxies_together call, for tests
 *
 * A double is NAN and a flag -1 where the path does not form it.
 */
struct MimicSageAddLocals {
  double satellite_disk_mass;   /**< float StellarMass - BulgeMass of the satellite (M:509) */
  double disk_mass_post;        /**< the target's float disk mass after the addition (M:512) */
  double disk_fraction_post;    /**< M:513-514 */
  int satellite_disk_positive;  /**< satellite_disk_mass > 0.0 (M:511) */
  int disk_fraction_above_half; /**< disk_fraction_post > 0.5 (M:516) */
  int satellite_ics_positive;   /**< satellite ICS > 0.0 (M:479) */
};

static inline void mimic_sage_merger_locals_reset(struct MimicSageMergerLocals *locals) {
  if (locals == NULL) {
    return;
  }
  locals->mi = NAN;
  locals->ma = NAN;
  locals->mass_ratio = NAN;
  locals->mass_ratio_path = -1;
  locals->central_disk_mass = NAN;
  locals->is_disk_dominated = -1;
  locals->major_merger = -1;
  locals->old_disk_radius = NAN;
  mimic_sage_remnant_locals_reset(&locals->remnant);
}

static inline void mimic_sage_add_locals_reset(struct MimicSageAddLocals *locals) {
  if (locals == NULL) {
    return;
  }
  locals->satellite_disk_mass = NAN;
  locals->disk_mass_post = NAN;
  locals->disk_fraction_post = NAN;
  locals->satellite_disk_positive = -1;
  locals->disk_fraction_above_half = -1;
  locals->satellite_ics_positive = -1;
}

/**
 * @brief SAGE26's merger mass ratio (R/src/model_mergers.c:235-249)
 *
 *   if(p.StellarMass + p.ColdGas < t.StellarMass + t.ColdGas) { mi = p..; ma = t..; }
 *   else { mi = t..; ma = p..; }
 *   if(ma > 0) ratio = mi / ma; else if(mi > 0) ratio = 1.0; else ratio = 0.0;
 *
 * The sums are float (two float members), compared in float and only then widened into
 * the double locals mi and ma. Both sums zero gives 0, not 1.
 *
 * @param satellite  The merging satellite (SAGE26's p), before the addition
 * @param target     The merger target (merger_centralgal), before the addition
 * @param locals     Optional: receives mi, ma, the ratio and its path
 * @return The mass ratio
 */
static inline double mimic_sage_merger_mass_ratio(const struct GalaxyData *satellite,
                                                  const struct GalaxyData *target,
                                                  struct MimicSageMergerLocals *locals) {
  double mi, ma, mass_ratio;
  int path;

  // SAGE parity: float sums, compared in float, widened on assignment (M:235-241)
  if (satellite->StellarMass + satellite->ColdGas < target->StellarMass + target->ColdGas) {
    mi = satellite->StellarMass + satellite->ColdGas;
    ma = target->StellarMass + target->ColdGas;
  } else {
    mi = target->StellarMass + target->ColdGas;
    ma = satellite->StellarMass + satellite->ColdGas;
  }

  // SAGE parity: both sums zero gives a ratio of 0 (M:243-249)
  if (ma > 0) {
    mass_ratio = mi / ma;
    path = 0;
  } else if (mi > 0) {
    mass_ratio = 1.0;
    path = 1;
  } else {
    mass_ratio = 0.0;
    path = 2;
  }

  if (locals != NULL) {
    locals->mi = mi;
    locals->ma = ma;
    locals->mass_ratio = mass_ratio;
    locals->mass_ratio_path = path;
  }
  return mass_ratio;
}

/**
 * @brief The target's morphology before the satellite is added (R/src/model_mergers.c:253-254)
 *
 *   double central_disk_mass = t.StellarMass - t.BulgeMass;
 *   int is_disk_dominated = (central_disk_mass > 0.5 * t.StellarMass);
 *
 * Read once, before add_galaxies_together; the burst's channel uses this value, never a
 * re-read after the transfer.
 *
 * @return 1 if the target is disk-dominated, else 0
 */
static inline int mimic_sage_merger_target_disk_dominated(const struct GalaxyData *target,
                                                          struct MimicSageMergerLocals *locals) {
  const double central_disk_mass = target->StellarMass - target->BulgeMass;
  const int is_disk_dominated = (central_disk_mass > 0.5 * target->StellarMass);
  if (locals != NULL) {
    locals->central_disk_mass = central_disk_mass;
    locals->is_disk_dominated = is_disk_dominated;
  }
  return is_disk_dominated;
}

/**
 * @brief The bulge channel of a merger burst (R/src/model_mergers.c:266-282)
 *
 * A major merger (ratio strictly above the threshold) credits the merger channel; a
 * minor one the instability channel onto a disk-dominated target and the merger
 * channel onto a spheroid-dominated one.
 *
 * @return 1 for MergerBulgeMass, 0 for InstabilityBulgeMass
 */
static inline int mimic_sage_merger_burst_channel(const double mass_ratio,
                                                  const double threshold_major_merger,
                                                  const int is_disk_dominated) {
  if (mass_ratio > threshold_major_merger) {
    return 1;
  }
  return is_disk_dominated ? 0 : 1;
}

/**
 * @brief SAGE26's add_galaxies_together on the R0 and R1 paths (R/src/model_mergers.c:456-543)
 *
 * Adds the satellite's reservoirs to the target: cold, stellar, hot and ejected gas with
 * their metals; the ICS, booking a positive satellite ICS and its deposit-time
 * accumulator as accreted and clearing the satellite's ICS history; the black hole; the
 * CGM gas; the satellite's stars and stellar metals into the bulge, its two bulge
 * channels into the target's, and its remaining disk stars into the instability channel
 * if the target's post-addition disk fraction exceeds 0.5, the instability channel then
 * updating the target's instability-bulge radius from its own disk radius (a no-op in
 * BulgeSizeOn 0, R/src/model_misc.c:395), else the merger channel; and each bin of the
 * satellite's disk and bulge histories into the target's bulge histories. The satellite's
 * other reservoirs are left as they are, as in SAGE26, and its component radii are not
 * averaged into the target's.
 *
 * Not run: the full star-formation history needs SaveFullSFH (M:536-541).
 *
 * @param target     SAGE26's t
 * @param satellite  SAGE26's p
 * @param sizes      The bulge-size configuration (BulgeSizeOn, Hubble_h)
 * @param locals     Optional: receives the satellite-disk values and flags
 */
static inline void mimic_sage_add_galaxies_together(struct GalaxyData *target,
                                                    struct GalaxyData *satellite,
                                                    const struct MimicSageBulgeSize *sizes,
                                                    struct MimicSageAddLocals *locals) {
  mimic_sage_add_locals_reset(locals);

  target->ColdGas += satellite->ColdGas;
  target->MetalsColdGas += satellite->MetalsColdGas;

  target->StellarMass += satellite->StellarMass;
  target->MetalsStellarMass += satellite->MetalsStellarMass;

  target->HotGas += satellite->HotGas;
  target->MetalsHotGas += satellite->MetalsHotGas;

  target->EjectedGas += satellite->EjectedGas;
  target->MetalsEjectedGas += satellite->MetalsEjectedGas;

  // SAGE parity: a positive satellite ICS enters as accreted, with its accumulator (M:479-484)
  const int satellite_ics_positive = satellite->ICS > 0.0;
  if (satellite_ics_positive) {
    target->ICS_accrete += satellite->ICS;
    target->ICS_sum_mt += satellite->ICS_sum_mt;
  }

  target->ICS += satellite->ICS;
  target->MetalsICS += satellite->MetalsICS;

  // SAGE parity: the satellite's ICS history is cleared, nothing else of it (M:492)
  satellite->ICS_disrupt = satellite->ICS_accrete = satellite->ICS_sum_mt = 0.0f;

  target->BlackHoleMass += satellite->BlackHoleMass;

  // SAGE parity: the CGM gas is transferred as SAGE26's code does on every path (M:496-497)
  target->CGMgas += satellite->CGMgas;
  target->MetalsCGMgas += satellite->MetalsCGMgas;

  // The satellite's stars go to the bulge, its two channels to the target's (M:500-505)
  target->BulgeMass += satellite->StellarMass;
  target->MetalsBulgeMass += satellite->MetalsStellarMass;

  target->InstabilityBulgeMass += satellite->InstabilityBulgeMass;
  target->MergerBulgeMass += satellite->MergerBulgeMass;

  /* SAGE parity: the satellite's float disk mass is classified by the target's disk
   * fraction AFTER the addition (M:509-528). Only the instability branch updates a radius:
   * the target's, from the target's own disk radius, after the channel store (M:516-522) */
  const double satellite_disk_mass = satellite->StellarMass - satellite->BulgeMass;
  const int satellite_disk_positive = satellite_disk_mass > 0.0;
  double disk_mass = NAN;
  double disk_fraction = NAN;
  int disk_fraction_above_half = -1;

  if (satellite_disk_positive) {
    disk_mass = target->StellarMass - target->BulgeMass;
    disk_fraction = (target->StellarMass > 0.0) ? disk_mass / target->StellarMass : 0.0;
    disk_fraction_above_half = disk_fraction > 0.5;

    if (disk_fraction_above_half) {
      target->InstabilityBulgeMass += satellite_disk_mass;
      const double old_disk_radius = target->DiskScaleRadius;
      mimic_sage_update_instability_bulge_radius(target, satellite_disk_mass, old_disk_radius,
                                                 sizes, NULL);
    } else {
      target->MergerBulgeMass += satellite_disk_mass;
    }
  }

  // SAGE parity: every bin of both satellite histories into the target's bulge (M:529-533)
  for (int bin = 0; bin < MIMIC_SAGE_SFR_STEPS; bin++) {
    *mimic_sage_sfr_history(target, MIMIC_SFR_HISTORY_BULGE, bin) +=
        *mimic_sage_sfr_history(satellite, MIMIC_SFR_HISTORY_DISK, bin) +
        *mimic_sage_sfr_history(satellite, MIMIC_SFR_HISTORY_BULGE, bin);
    *mimic_sage_sfr_history(target, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, bin) +=
        *mimic_sage_sfr_history(satellite, MIMIC_SFR_HISTORY_DISK_COLD_GAS, bin) +
        *mimic_sage_sfr_history(satellite, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, bin);
    *mimic_sage_sfr_history(target, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, bin) +=
        *mimic_sage_sfr_history(satellite, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, bin) +
        *mimic_sage_sfr_history(satellite, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, bin);
  }

  if (locals != NULL) {
    locals->satellite_disk_mass = satellite_disk_mass;
    locals->disk_mass_post = disk_mass;
    locals->disk_fraction_post = disk_fraction;
    locals->satellite_disk_positive = satellite_disk_positive;
    locals->disk_fraction_above_half = disk_fraction_above_half;
    locals->satellite_ics_positive = satellite_ics_positive;
  }
}

/**
 * @brief SAGE26's make_bulge_from_burst (R/src/model_mergers.c:553-570)
 *
 * After a major merger the whole stellar mass is a merger-built bulge: BulgeMass and
 * MergerBulgeMass take the final StellarMass, InstabilityBulgeMass is zeroed, the bulge
 * metals take the stellar metals, and each bin of the disk histories is added into the
 * bulge histories and then zeroed.
 */
static inline void mimic_sage_make_bulge_from_burst(struct GalaxyData *gal) {
  gal->BulgeMass = gal->StellarMass;
  gal->MergerBulgeMass = gal->StellarMass;
  gal->InstabilityBulgeMass = 0.0f;
  gal->MetalsBulgeMass = gal->MetalsStellarMass;

  for (int bin = 0; bin < MIMIC_SAGE_SFR_STEPS; bin++) {
    *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE, bin) +=
        *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK, bin);
    *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, bin) +=
        *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, bin);
    *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, bin) +=
        *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, bin);
    *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK, bin) = 0.0f;
    *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, bin) = 0.0f;
    *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, bin) = 0.0f;
  }
}

/**
 * @brief The values and branch flags of one disrupt_satellite_to_ICS call, for tests
 */
struct MimicSageDisruptLocals {
  double total_gas;           /**< float ColdGas + HotGas + CGMgas, widened (M:848) */
  double total_metals_gas;    /**< the same sum of their metals (M:849) */
  int disrupt_stars_positive; /**< satellite StellarMass > 0.0 (M:869) */
  int disrupt_ics_positive;   /**< satellite ICS > 0.0 (M:881) */
};

/**
 * @brief SAGE26's disrupt_satellite_to_ICS on the R0 path (R/src/model_mergers.c:845-902)
 *
 * The satellite's cold, hot and CGM gas, and their metals, each summed in float (three
 * float members) and only then widened into the double locals, go to the target's hot
 * gas (add_gas_to_hot_reservoir, R/src/model_misc.h:76-85, takes the HotGas branch with
 * CGMrecipeOn 0); its ejected gas and existing ICS are transferred, and its stars and
 * stellar metals added to the target's ICS. Positive stars are booked as disrupted with
 * StellarMass * time into ICS_sum_mt (fused, M:871); the black hole is transferred; a
 * positive existing ICS is booked as accreted with its deposit-time accumulator. Then
 * exactly SAGE26's fields of the satellite are zeroed (M:891-900): the two bulge
 * channels, the radii, H1gas, H2gas, QuasarModeBHaccretionMass, the outflow rate, the
 * timestamps and the histories are not. The caller marks the satellite merged (SAGE26's
 * mergeType 4, M:902).
 *
 * @param target     SAGE26's centralgal (the pass's merger_centralgal)
 * @param satellite  SAGE26's gal
 * @param time       The event time, a lookback time in code units (B:549)
 * @param locals     Optional: receives the gas sums and the two flags
 */
static inline void mimic_sage_disrupt_satellite_to_ics(struct GalaxyData *target,
                                                       struct GalaxyData *satellite,
                                                       const double time,
                                                       struct MimicSageDisruptLocals *locals) {
  // SAGE parity: float sums of three float members, widened on assignment (M:848-849)
  const double total_gas = satellite->ColdGas + satellite->HotGas + satellite->CGMgas;
  const double total_metals_gas =
      satellite->MetalsColdGas + satellite->MetalsHotGas + satellite->MetalsCGMgas;

  // SAGE parity: add_gas_to_hot_reservoir's HotGas branch on R0 (M:851, misc.h:76-85)
  target->HotGas += total_gas;
  target->MetalsHotGas += total_metals_gas;

  target->EjectedGas += satellite->EjectedGas;
  target->MetalsEjectedGas += satellite->MetalsEjectedGas;

  target->ICS += satellite->ICS;
  target->MetalsICS += satellite->MetalsICS;

  target->ICS += satellite->StellarMass;
  target->MetalsICS += satellite->MetalsStellarMass;

  // SAGE parity: disrupted stars are deposited at the event time; M:871 is one fused
  // multiply-add in SAGE26's objects (oracle README c06 table), written as fma()
  const int stars_positive = satellite->StellarMass > 0.0;
  if (stars_positive) {
    target->ICS_disrupt += satellite->StellarMass;
    target->ICS_sum_mt =
        (float)fma((double)satellite->StellarMass, time, (double)target->ICS_sum_mt);
  }

  // SAGE parity: the black hole is transferred, not lost (M:875)
  target->BlackHoleMass += satellite->BlackHoleMass;

  // SAGE parity: a positive existing ICS is accreted with its accumulator (M:881-887)
  const int ics_positive = satellite->ICS > 0.0;
  if (ics_positive) {
    target->ICS_accrete += satellite->ICS;
    target->ICS_sum_mt += satellite->ICS_sum_mt;
  }

  // SAGE parity: exactly SAGE26's cleared fields (M:891-900), no others
  satellite->ColdGas = satellite->MetalsColdGas = 0.0f;
  satellite->HotGas = satellite->MetalsHotGas = 0.0f;
  satellite->CGMgas = satellite->MetalsCGMgas = 0.0f;
  satellite->EjectedGas = satellite->MetalsEjectedGas = 0.0f;
  satellite->ICS = satellite->MetalsICS = 0.0f;
  satellite->ICS_disrupt = satellite->ICS_accrete = 0.0f;
  satellite->ICS_sum_mt = 0.0f;
  satellite->StellarMass = satellite->MetalsStellarMass = 0.0f;
  satellite->BulgeMass = satellite->MetalsBulgeMass = 0.0f;
  satellite->BlackHoleMass = 0.0f;

  if (locals != NULL) {
    locals->total_gas = total_gas;
    locals->total_metals_gas = total_metals_gas;
    locals->disrupt_stars_positive = stars_positive;
    locals->disrupt_ics_positive = ics_positive;
  }
}

/**
 * @brief SAGE26's merger remnant radius by energy conservation
 *        (calculate_merger_remnant_radius, R/src/model_mergers.c:135-212)
 *
 * Ports the routine's code, not its introductory comment's formula (:129-133). Each
 * progenitor's mass is its float StellarMass + ColdGas, widened; its radius is the
 * disk-and-bulge mass-weighted half-mass radius (1.68 times DiskScaleRadius for the disk, the
 * stored BulgeRadius for the bulge) when that float sum is positive, else 0; a non-positive
 * radius takes the other progenitor's, in that order, and two non-positive radii give 0. The
 * self-binding, orbital and radiative energies (Covington et al. 2011) are doubles; a
 * non-positive total energy falls back to the mass-weighted average of the two radii,
 * otherwise the remnant radius is M_tot^2 / E_final.
 *
 * Every expression keeps SAGE26's operand order and types: the progenitor sums and
 * M_disk = ColdGas + (StellarMass - BulgeMass) are float before widening, and so is the gas
 * sum of f_gas. SAGE26's object fuses three multiply-adds here (oracle README, c07
 * fused-instruction table), each written as an explicit fma() with the other product rounded
 * first: M_disk * R_disk_half onto the rounded M_bulge * R_bulge_half for each progenitor
 * (:158, :171), and M1 * R1 onto the rounded M2 * R2 in the fallback (:204).
 *
 * In deal_with_galaxy_merger g1 is the target after the addition, the black hole and the
 * burst (its BulgeRadius just synchronised), and g2 the satellite, still populated (:293-297).
 *
 * @param g1      SAGE26's g1 (the merger target)
 * @param g2      SAGE26's g2 (the satellite)
 * @param locals  Optional (NULL in production): receives SAGE26's locals
 * @return The remnant radius (code length units), 0.0 on the two early returns
 */
static inline double mimic_sage_merger_remnant_radius(const struct GalaxyData *g1,
                                                      const struct GalaxyData *g2,
                                                      struct MimicSageRemnantLocals *locals) {
  mimic_sage_remnant_locals_reset(locals);

  // SAGE parity: float sums of two float members, widened on assignment (M:138-140)
  const double M1 = g1->StellarMass + g1->ColdGas;
  const double M2 = g2->StellarMass + g2->ColdGas;
  const double M_tot = M1 + M2;
  const int total_nonpositive = M_tot <= 0.0;
  if (locals != NULL) {
    locals->M1 = M1;
    locals->M2 = M2;
    locals->M_tot = M_tot;
    locals->total_nonpositive = total_nonpositive;
  }
  if (total_nonpositive) {
    if (locals != NULL) {
      locals->new_merger_radius = 0.0;
    }
    return 0.0;
  }

  /* SAGE parity: each progenitor's radius is formed only when its FLOAT stellar-plus-cold sum
   * is positive (M:153, :168), with M_disk evaluated in float before widening (M:156, :169).
   * SAGE26's object fuses the disk product onto the rounded bulge product (M:158, :171),
   * written as fma() */
  const double R1_disk_half = MIMIC_SAGE_DISK_HALF_MASS_FRAC * g1->DiskScaleRadius;
  const double R1_bulge_half = g1->BulgeRadius;
  double R1 = 0.0;
  const int g1_mass_positive = g1->StellarMass + g1->ColdGas > 0;
  if (g1_mass_positive) {
    const double M1_disk = g1->ColdGas + (g1->StellarMass - g1->BulgeMass);
    const double M1_bulge = g1->BulgeMass;
    R1 = fma(M1_disk, R1_disk_half, M1_bulge * R1_bulge_half) / M1;
    if (locals != NULL) {
      locals->M1_disk = M1_disk;
      locals->M1_bulge = M1_bulge;
    }
  }

  const double R2_disk_half = MIMIC_SAGE_DISK_HALF_MASS_FRAC * g2->DiskScaleRadius;
  const double R2_bulge_half = g2->BulgeRadius;
  double R2 = 0.0;
  const int g2_mass_positive = g2->StellarMass + g2->ColdGas > 0;
  if (g2_mass_positive) {
    const double M2_disk = g2->ColdGas + (g2->StellarMass - g2->BulgeMass);
    const double M2_bulge = g2->BulgeMass;
    R2 = fma(M2_disk, R2_disk_half, M2_bulge * R2_bulge_half) / M2;
    if (locals != NULL) {
      locals->M2_disk = M2_disk;
      locals->M2_bulge = M2_bulge;
    }
  }
  if (locals != NULL) {
    locals->R1_disk_half = R1_disk_half;
    locals->R1_bulge_half = R1_bulge_half;
    locals->R1_progenitor = R1;
    locals->R2_disk_half = R2_disk_half;
    locals->R2_bulge_half = R2_bulge_half;
    locals->R2_progenitor = R2;
    locals->g1_mass_positive = g1_mass_positive;
    locals->g2_mass_positive = g2_mass_positive;
  }

  // SAGE parity: the zero-radius fallbacks in SAGE26's order, R1 first (M:177-179)
  const int r1_fallback = R1 <= 0.0;
  if (r1_fallback) {
    R1 = R2;
  }
  const int r2_fallback = R2 <= 0.0;
  if (r2_fallback) {
    R2 = R1;
  }
  const int both_radii_zero = R1 <= 0.0;
  if (locals != NULL) {
    locals->R1 = R1;
    locals->R2 = R2;
    locals->r1_fallback = r1_fallback;
    locals->r2_fallback = r2_fallback;
    locals->both_radii_zero = both_radii_zero;
  }
  if (both_radii_zero) {
    if (locals != NULL) {
      locals->new_merger_radius = 0.0;
    }
    return 0.0;
  }

  /* SAGE parity: the energies in double, in SAGE26's statement order (M:185-199); f_gas
   * divides the float gas sum, widened, by M_tot (M:194) */
  const double E_init = (M1 * M1) / R1 + (M2 * M2) / R2;
  const double E_orb = (M1 * M2) / (R1 + R2);
  const double f_gas = (g1->ColdGas + g2->ColdGas) / M_tot;
  const double E_rad = MIMIC_SAGE_COVINGTON11_C_RAD * E_init * f_gas;
  const double E_final = E_init + E_orb + E_rad;

  /* SAGE parity: a non-positive total energy falls back to the mass-weighted radius (M:202-205);
   * SAGE26's object fuses M1 * R1 onto the rounded M2 * R2 (M:204), written as fma() */
  const int energy_nonpositive = E_final <= 0.0;
  const double new_merger_radius =
      energy_nonpositive ? fma(M1, R1, M2 * R2) / M_tot : (M_tot * M_tot) / E_final;

  if (locals != NULL) {
    locals->E_init = E_init;
    locals->E_orb = E_orb;
    locals->f_gas = f_gas;
    locals->E_rad = E_rad;
    locals->E_final = E_final;
    locals->energy_nonpositive = energy_nonpositive;
    locals->new_merger_radius = new_merger_radius;
  }
  return new_merger_radius;
}

/**
 * @brief get_bulge_radius on R0: BulgeSizeOn 0 zeroes the three bulge radii
 *        (R/src/model_misc.c:241-247)
 *
 * SAGE26's merger transaction calls it on the target after the burst and again on every
 * branch of the major/minor split (R/src/model_mergers.c:293, :308, :325, :330), so the
 * remnant radius it computes in between (:297) never reaches the state on R0. With BulgeSizeOn
 * 3 the transaction calls mimic_sage_get_bulge_radius() instead (sage_bulge_size_physics.h).
 */
static inline void mimic_sage_reset_bulge_radii(struct GalaxyData *gal) {
  gal->BulgeRadius = 0.0f;
  gal->MergerBulgeRadius = 0.0f;
  gal->InstabilityBulgeRadius = 0.0f;
}

#endif /* MIMIC_SHARED_SAGE_MERGER_PHYSICS_H */
