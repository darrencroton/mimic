/**
 * @file    sage_merger_physics.h
 * @brief   SAGE26's merger transaction pieces on live galaxies (R0 path)
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
 * - mimic_sage_add_galaxies_together(): add_galaxies_together (M:456-543);
 * - mimic_sage_make_bulge_from_burst(): make_bulge_from_burst (M:553-570);
 * - mimic_sage_reset_bulge_radii(): get_bulge_radius on R0 (BulgeSizeOn 0,
 *   R/src/model_misc.c:239-247), which zeroes the three bulge radii;
 * - mimic_sage_disrupt_satellite_to_ics(): disrupt_satellite_to_ICS (M:845-902).
 *
 * sage26_resolve_mergers_and_disruption composes the first six into
 * deal_with_galaxy_merger and calls the last from its pass. Each expression keeps
 * SAGE26's operand order and types: the struct GALAXY members are float, so a sum of two
 * members rounds in float before any widening, and a compound assignment of a double into
 * a float member rounds once into the float. The one multiply-add on these paths that
 * SAGE26's objects fuse, the disruption's ICS_sum_mt += StellarMass * time (M:871, the
 * oracle README's c06 table), is written as an explicit fma(); no other helper here
 * contains a multiply-add (the table lists none for add_galaxies_together and
 * make_bulge_from_burst).
 */

#ifndef MIMIC_SHARED_SAGE_MERGER_PHYSICS_H
#define MIMIC_SHARED_SAGE_MERGER_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "types.h"
#include "shared/sage_sfr_history.h"

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
 * @brief SAGE26's add_galaxies_together on the R0 path (R/src/model_mergers.c:456-543)
 *
 * Adds the satellite's reservoirs to the target: cold, stellar, hot and ejected gas with
 * their metals; the ICS, booking a positive satellite ICS and its deposit-time
 * accumulator as accreted and clearing the satellite's ICS history; the black hole; the
 * CGM gas; the satellite's stars and stellar metals into the bulge, its two bulge
 * channels into the target's, and its remaining disk stars into the instability channel
 * if the target's post-addition disk fraction exceeds 0.5, else the merger channel; and
 * each bin of the satellite's disk and bulge histories into the target's bulge
 * histories. The satellite's other reservoirs are left as they are, as in SAGE26.
 *
 * Not run on R0: update_instability_bulge_radius returns at once with BulgeSizeOn 0
 * (R/src/model_misc.c:395), and the full star-formation history needs SaveFullSFH
 * (M:536-541).
 *
 * @param target     SAGE26's t
 * @param satellite  SAGE26's p
 * @param locals     Optional: receives the satellite-disk values and flags
 */
static inline void mimic_sage_add_galaxies_together(struct GalaxyData *target,
                                                    struct GalaxyData *satellite,
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
   * fraction AFTER the addition (M:509-528) */
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
 * @brief get_bulge_radius on R0: BulgeSizeOn 0 zeroes the three bulge radii
 *        (R/src/model_misc.c:241-247)
 *
 * SAGE26's merger transaction calls it on the target after the burst and again on every
 * branch of the major/minor split (R/src/model_mergers.c:293, :308, :325, :330), so the
 * remnant radius it computes in between (:297) never reaches the state on R0.
 */
static inline void mimic_sage_reset_bulge_radii(struct GalaxyData *gal) {
  gal->BulgeRadius = 0.0f;
  gal->MergerBulgeRadius = 0.0f;
  gal->InstabilityBulgeRadius = 0.0f;
}

#endif /* MIMIC_SHARED_SAGE_MERGER_PHYSICS_H */
