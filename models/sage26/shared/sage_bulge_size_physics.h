/**
 * @file sage_bulge_size_physics.h
 * @brief SAGE26's bulge sizes: get_bulge_radius and update_instability_bulge_radius
 *
 * Ports the two size routines of SAGE26's R/src/model_misc.c (R = the staged SAGE26 source) for
 * the two bulge-size modes the package supports, BulgeSizeOn 0 (R0) and 3 (R1 onward, Tonini et
 * al. 2016's separate merger and instability bulges):
 *
 * - mimic_sage_get_bulge_radius(): get_bulge_radius (:239-377). Mode 0 zeroes the three radii
 *   and returns 0 (:242-247). Mode 3 (:317-369) widens the two float channel masses into double
 *   locals and sums them; a non-positive sum zeroes the three radii and returns 0; otherwise a
 *   component radius is zeroed only when its channel mass is exactly zero, a positive channel
 *   mass with a non-positive radius takes a fallback (Shen et al. 2003 eq. 33 for the merger
 *   channel; 0.2 * DiskScaleRadius, or Shen when the disk radius is not positive, for the
 *   instability channel), and the channel-mass-weighted average is stored in BulgeRadius and
 *   returned.
 * - mimic_sage_update_instability_bulge_radius(): update_instability_bulge_radius (:385-424),
 *   Tonini et al. 2016 eq. 15, called after the instability channel mass has already been
 *   increased by delta_mass and rounded into its float field. Mode 0 returns at once (:395),
 *   as does a non-positive delta_mass; otherwise the radius is the mass-weighted average of the
 *   old radius and 0.2 times the disk radius saved by the caller (or that contribution alone
 *   for the first transfer), stored in InstabilityBulgeRadius, and get_bulge_radius follows.
 *
 * Arithmetic follows SAGE26's expression shapes and types: every local is a double (the float
 * channel masses, radii and DiskScaleRadius widened exactly), the fallback radii stay double
 * locals for the weighted average even though they are also stored in the narrower float
 * fields (never reloaded from them), M_old is recovered from the POST-store float channel mass,
 * and the kpc conversions that cancel algebraically are kept. SAGE26's compiled object fuses
 * four multiply-adds on these paths (oracle README, c07 fused-instruction table), each written
 * below as an explicit fma(): the two Shen fallbacks 0.56 * log10(M) - 5.54 (:345, :359), the
 * weighted-average numerator M_merger * R_merger + (M_instability * R_instability rounded)
 * (:366), and the incremental numerator R_old_kpc * M_old + (R_new_contribution_kpc *
 * delta_mass rounded) (:415). Every other expression rounds as written.
 *
 * @note Used by sage26_set_disk_scale_radius (the FoF central's synchronisation after its disk
 *       radius, R/src/core_build_model.c:297-298), by the disk-instability transfer
 *       (sage_disk_instability_physics.h, R/src/model_disk_instability.c:83), by the satellite
 *       disk-star transfer of add_galaxies_together (sage_merger_physics.h,
 *       R/src/model_mergers.c:516-522), by the burst's instability channel
 *       (sage_starburst_physics.h, R/src/model_mergers.c:778-784) and, with BulgeSizeOn 3, by
 *       the merger transaction's synchronisation and remnant assignment
 *       (sage26_resolve_mergers_and_disruption, R/src/model_mergers.c:293-330).
 */

#ifndef MIMIC_SHARED_SAGE_BULGE_SIZE_PHYSICS_H
#define MIMIC_SHARED_SAGE_BULGE_SIZE_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "error.h"
#include "types.h"

/** @brief BulgeSizeOn 0: no bulge sizes, every radius zeroed (SAGE26's R0) */
#define MIMIC_SAGE_BULGE_SIZE_OFF 0
/** @brief BulgeSizeOn 3: Tonini et al. 2016 merger and instability bulges (R1 onward) */
#define MIMIC_SAGE_BULGE_SIZE_TONINI 3

/* SAGE26's Shen et al. (2003) eq. 33 coefficients and Tonini et al. (2016) eq. 15 disk
 * fraction, file-static constants of R/src/model_misc.c (:29-30, :55) */
#define MIMIC_SAGE_SHEN03_SLOPE_HIGH 0.56
#define MIMIC_SAGE_SHEN03_INTERCEPT_HIGH -5.54
#define MIMIC_SAGE_TONINI16_DISK_FRAC 0.2

/**
 * @brief The bulge-size configuration a size routine reads
 *
 * SAGE26 reads run_params->BulgeSizeOn and, in mode 3, run_params->Hubble_h
 * (R/src/model_misc.c:242, :249, :395, :398). A module fills it from its BulgeSizeOn
 * parameter and the core's Hubble_h.
 */
struct MimicSageBulgeSize {
  int bulge_size_on; /**< MIMIC_SAGE_BULGE_SIZE_OFF or MIMIC_SAGE_BULGE_SIZE_TONINI */
  double hubble_h;   /**< little h, read only in mode 3 */
};

/**
 * @brief Refuse a BulgeSizeOn the package does not support
 *
 * The package ports exactly SAGE26's modes 0 and 3; modes 1 and 2 (Shen-only sizes,
 * R/src/model_misc.c:252-315) are not ported. Each module that calls a size routine loads the
 * required parameter BulgeSizeOn at initialisation and passes it here.
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded BulgeSizeOn
 * @return 0 for 0 or 3; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_bulge_size_on(const char *module_name, const int value) {
  if (value != MIMIC_SAGE_BULGE_SIZE_OFF && value != MIMIC_SAGE_BULGE_SIZE_TONINI) {
    ERROR_LOG("%s: BulgeSizeOn = %d is not supported: the package ports only 0 (no bulge sizes, "
              "SAGE26's R0) and 3 (Tonini et al. 2016 merger and instability bulges)",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief The locals of one get_bulge_radius call, for tests that compare them
 *
 * Each double is SAGE26's local of the same name (R/src/model_misc.c:318-366), NAN where the
 * call does not form it; each flag is -1 where the call does not evaluate it.
 */
struct MimicSageBulgeRadiusLocals {
  double M_merger;           /**< the float MergerBulgeMass widened (:318) */
  double M_instability;      /**< the float InstabilityBulgeMass widened (:319) */
  double M_total;            /**< M_merger + M_instability (:320) */
  double M_merger_sun;       /**< M_merger * 1e10 / h, merger fallback (:344) */
  double log_R_merger_kpc;   /**< 0.56 * log10(M_merger_sun) - 5.54, fused (:345) */
  double R_merger;           /**< the merger radius the average uses (:342, :346) */
  double R_disc;             /**< the float DiskScaleRadius widened, instability fallback (:353) */
  double M_inst_sun;         /**< M_instability * 1e10 / h, Shen instability fallback (:358) */
  double log_R_inst_kpc;     /**< 0.56 * log10(M_inst_sun) - 5.54, fused (:359) */
  double R_instability;      /**< the instability radius the average uses (:351, :355, :360) */
  double R_bulge;            /**< the weighted average, fused numerator (:366) */
  int total_nonpositive;     /**< M_total <= 0.0 (:322) */
  int merger_mass_zero;      /**< M_merger == 0.0: MergerBulgeRadius zeroed (:332) */
  int instability_mass_zero; /**< M_instability == 0.0: InstabilityBulgeRadius zeroed (:335) */
  int merger_fallback;       /**< M_merger > 0 && R_merger <= 0: the Shen fallback ran (:343) */
  int instability_fallback;  /**< 0 none, 1 the disk fallback (:354-355), 2 Shen (:356-360) */
};

/**
 * @brief The locals of one update_instability_bulge_radius call, for tests that compare them
 *
 * Each double is SAGE26's local of the same name (R/src/model_misc.c:398-420), NAN where the
 * call does not form it; each flag is -1 where the call does not evaluate it.
 */
struct MimicSageInstabilityRadiusLocals {
  double M_old;                  /**< post-store float mass widened, minus delta_mass (:399) */
  double R_old;                  /**< the float InstabilityBulgeRadius widened (:400) */
  double R_disc_kpc;             /**< old_disk_radius * 1e3 / h (:404) */
  double R_new_contribution_kpc; /**< 0.2 * R_disc_kpc (:407) */
  double R_new_contribution;     /**< R_new_contribution_kpc * 1e-3 * h (:408) */
  double R_old_kpc;              /**< R_old * 1e3 / h, incremental branch (:413) */
  double M_new;                  /**< the post-store float mass widened (:414) */
  double R_new_kpc;              /**< the eq. 15 average in kpc, fused numerator (:415) */
  double R_new;                  /**< the radius stored, as a double (:416, :419) */
  int returned_early;            /**< 1 mode is not 3 (:395), 2 delta_mass <= 0 (:396), 0 ran */
  int incremental;               /**< M_old > 0 && R_old > 0 (:411) */
};

/** @brief Mark every value of a get_bulge_radius locals record as not formed */
static inline void mimic_sage_bulge_radius_locals_reset(struct MimicSageBulgeRadiusLocals *locals) {
  if (locals == NULL) {
    return;
  }
  locals->M_merger = NAN;
  locals->M_instability = NAN;
  locals->M_total = NAN;
  locals->M_merger_sun = NAN;
  locals->log_R_merger_kpc = NAN;
  locals->R_merger = NAN;
  locals->R_disc = NAN;
  locals->M_inst_sun = NAN;
  locals->log_R_inst_kpc = NAN;
  locals->R_instability = NAN;
  locals->R_bulge = NAN;
  locals->total_nonpositive = -1;
  locals->merger_mass_zero = -1;
  locals->instability_mass_zero = -1;
  locals->merger_fallback = -1;
  locals->instability_fallback = -1;
}

/** @brief Mark every value of an update_instability_bulge_radius locals record as not formed */
static inline void
mimic_sage_instability_radius_locals_reset(struct MimicSageInstabilityRadiusLocals *locals) {
  if (locals == NULL) {
    return;
  }
  locals->M_old = NAN;
  locals->R_old = NAN;
  locals->R_disc_kpc = NAN;
  locals->R_new_contribution_kpc = NAN;
  locals->R_new_contribution = NAN;
  locals->R_old_kpc = NAN;
  locals->M_new = NAN;
  locals->R_new_kpc = NAN;
  locals->R_new = NAN;
  locals->returned_early = -1;
  locals->incremental = -1;
}

/**
 * @brief SAGE26's get_bulge_radius for BulgeSizeOn 0 and 3 (R/src/model_misc.c:239-377)
 *
 * @param gal     The galaxy; its three bulge radii are written
 * @param sizes   The bulge-size configuration (mode and, for mode 3, Hubble_h)
 * @param locals  Optional (NULL in production): receives SAGE26's locals
 * @return The bulge radius as a double, before its float store (0.0 where the radii are zeroed)
 */
static inline double mimic_sage_get_bulge_radius(struct GalaxyData *gal,
                                                 const struct MimicSageBulgeSize *sizes,
                                                 struct MimicSageBulgeRadiusLocals *locals) {
  mimic_sage_bulge_radius_locals_reset(locals);

  /* SAGE parity: BulgeSizeOn 0 zeroes the three radii and returns 0 (R/src/model_misc.c:242-247);
   * the package accepts only 0 and 3 (mimic_sage_validate_bulge_size_on), so every other mode
   * here is 0 */
  if (sizes->bulge_size_on != MIMIC_SAGE_BULGE_SIZE_TONINI) {
    gal->BulgeRadius = 0.0f;
    gal->MergerBulgeRadius = 0.0f;
    gal->InstabilityBulgeRadius = 0.0f;
    return 0.0;
  }

  const double h = sizes->hubble_h;

  /* SAGE parity: the two float channel masses are widened separately and summed in double
   * (R/src/model_misc.c:318-320); the combined size is weighted by the channels, not BulgeMass */
  const double M_merger = gal->MergerBulgeMass;
  const double M_instability = gal->InstabilityBulgeMass;
  const double M_total = M_merger + M_instability;
  const int total_nonpositive = M_total <= 0.0;
  if (locals != NULL) {
    locals->M_merger = M_merger;
    locals->M_instability = M_instability;
    locals->M_total = M_total;
    locals->total_nonpositive = total_nonpositive;
  }

  if (total_nonpositive) {
    gal->BulgeRadius = 0.0f;
    gal->MergerBulgeRadius = 0.0f;
    gal->InstabilityBulgeRadius = 0.0f;
    return 0.0;
  }

  /* SAGE parity: a component radius is zeroed only when its channel mass is EXACTLY zero
   * (R/src/model_misc.c:332-337), so a stale radius beside a negative channel mass survives */
  const int merger_mass_zero = M_merger == 0.0;
  const int instability_mass_zero = M_instability == 0.0;
  if (merger_mass_zero) {
    gal->MergerBulgeRadius = 0.0f;
  }
  if (instability_mass_zero) {
    gal->InstabilityBulgeRadius = 0.0f;
  }

  /* SAGE parity: a positive merger mass with a non-positive radius takes the Shen et al. 2003
   * eq. 33 fallback (R/src/model_misc.c:342-348). The radius is read after the zeroing above;
   * the fallback stays a double local for the average below, and only its float store is
   * written. SAGE26's object fuses :345, written as fma() */
  double R_merger = gal->MergerBulgeRadius;
  const int merger_fallback = M_merger > 0.0 && R_merger <= 0.0;
  if (merger_fallback) {
    const double M_merger_sun = M_merger * 1.0e10 / h;
    const double log_R_kpc =
        fma(MIMIC_SAGE_SHEN03_SLOPE_HIGH, log10(M_merger_sun), MIMIC_SAGE_SHEN03_INTERCEPT_HIGH);
    R_merger = pow(10.0, log_R_kpc) * 1.0e-3 * h;
    gal->MergerBulgeRadius = (float)R_merger;
    if (locals != NULL) {
      locals->M_merger_sun = M_merger_sun;
      locals->log_R_merger_kpc = log_R_kpc;
    }
  }

  /* SAGE parity: a positive instability mass with a non-positive radius takes 0.2 times the
   * widened float DiskScaleRadius when that is positive, else the same Shen fallback with the
   * instability mass (R/src/model_misc.c:351-363); the result stays a double local for the
   * average. SAGE26's object fuses :359, written as fma() */
  double R_instability = gal->InstabilityBulgeRadius;
  int instability_fallback = 0;
  if (M_instability > 0.0 && R_instability <= 0.0) {
    const double R_disc = gal->DiskScaleRadius;
    if (locals != NULL) {
      locals->R_disc = R_disc;
    }
    if (R_disc > 0.0) {
      R_instability = MIMIC_SAGE_TONINI16_DISK_FRAC * R_disc;
      instability_fallback = 1;
    } else {
      const double M_inst_sun = M_instability * 1.0e10 / h;
      const double log_R_kpc =
          fma(MIMIC_SAGE_SHEN03_SLOPE_HIGH, log10(M_inst_sun), MIMIC_SAGE_SHEN03_INTERCEPT_HIGH);
      R_instability = pow(10.0, log_R_kpc) * 1.0e-3 * h;
      instability_fallback = 2;
      if (locals != NULL) {
        locals->M_inst_sun = M_inst_sun;
        locals->log_R_inst_kpc = log_R_kpc;
      }
    }
    gal->InstabilityBulgeRadius = (float)R_instability;
  }

  /* SAGE parity: the channel-mass-weighted average of the double radii (Tonini et al. 2016
   * eq. 25, R/src/model_misc.c:366); SAGE26's object rounds M_instability * R_instability and
   * fuses M_merger * R_merger onto it, written as fma() */
  const double R_bulge = fma(M_merger, R_merger, M_instability * R_instability) / M_total;
  gal->BulgeRadius = (float)R_bulge;

  if (locals != NULL) {
    locals->R_merger = R_merger;
    locals->R_instability = R_instability;
    locals->R_bulge = R_bulge;
    locals->merger_mass_zero = merger_mass_zero;
    locals->instability_mass_zero = instability_mass_zero;
    locals->merger_fallback = merger_fallback;
    locals->instability_fallback = instability_fallback;
  }
  return R_bulge;
}

/**
 * @brief SAGE26's update_instability_bulge_radius for BulgeSizeOn 0 and 3
 *        (R/src/model_misc.c:385-424)
 *
 * The caller has already added delta_mass to InstabilityBulgeMass (a float store); M_old is
 * recovered from that post-store mass, as SAGE26 does.
 *
 * @param gal              The galaxy; InstabilityBulgeRadius and then (get_bulge_radius) its
 *                         three radii are written
 * @param delta_mass       The mass just credited to the instability channel (double)
 * @param old_disk_radius  The disk radius the caller saved before its transfer (double)
 * @param sizes            The bulge-size configuration
 * @param locals           Optional (NULL in production): receives SAGE26's locals
 */
static inline void mimic_sage_update_instability_bulge_radius(
    struct GalaxyData *gal, const double delta_mass, const double old_disk_radius,
    const struct MimicSageBulgeSize *sizes, struct MimicSageInstabilityRadiusLocals *locals) {
  mimic_sage_instability_radius_locals_reset(locals);

  /* SAGE parity: mode 0 returns at once, leaving every radius as it was, stale ones included
   * (R/src/model_misc.c:395); so does a non-positive transfer (:396). Neither synchronises */
  if (sizes->bulge_size_on != MIMIC_SAGE_BULGE_SIZE_TONINI) {
    if (locals != NULL) {
      locals->returned_early = 1;
    }
    return;
  }
  if (delta_mass <= 0.0) {
    if (locals != NULL) {
      locals->returned_early = 2;
    }
    return;
  }

  const double h = sizes->hubble_h;

  /* SAGE parity: M_old is recovered from the POST-store float channel mass widened, minus the
   * double transfer (R/src/model_misc.c:399), not from the mass before the store */
  const double M_old = gal->InstabilityBulgeMass - delta_mass;
  const double R_old = gal->InstabilityBulgeRadius;

  /* SAGE parity: the kpc round trip is kept although it cancels algebraically
   * (R/src/model_misc.c:404-408) */
  const double R_disc_kpc = old_disk_radius * 1.0e3 / h;
  const double R_new_contribution_kpc = MIMIC_SAGE_TONINI16_DISK_FRAC * R_disc_kpc;
  const double R_new_contribution = R_new_contribution_kpc * 1.0e-3 * h;

  double R_new;
  const int incremental = M_old > 0.0 && R_old > 0.0;
  if (incremental) {
    /* SAGE parity: eq. 15 in kpc (R/src/model_misc.c:413-416); SAGE26's object rounds
     * R_new_contribution_kpc * delta_mass and fuses R_old_kpc * M_old onto it (:415), written
     * as fma() */
    const double R_old_kpc = R_old * 1.0e3 / h;
    const double M_new = gal->InstabilityBulgeMass;
    const double R_new_kpc = fma(R_old_kpc, M_old, R_new_contribution_kpc * delta_mass) / M_new;
    R_new = R_new_kpc * 1.0e-3 * h;
    if (locals != NULL) {
      locals->R_old_kpc = R_old_kpc;
      locals->M_new = M_new;
      locals->R_new_kpc = R_new_kpc;
    }
  } else {
    // First transfer: 0.2 times the saved disk radius (:417-420)
    R_new = R_new_contribution;
  }

  gal->InstabilityBulgeRadius = (float)R_new;

  if (locals != NULL) {
    locals->M_old = M_old;
    locals->R_old = R_old;
    locals->R_disc_kpc = R_disc_kpc;
    locals->R_new_contribution_kpc = R_new_contribution_kpc;
    locals->R_new_contribution = R_new_contribution;
    locals->R_new = R_new;
    locals->returned_early = 0;
    locals->incremental = incremental;
  }

  // SAGE26 synchronises the combined radius at once (:423); its return is discarded
  (void)mimic_sage_get_bulge_radius(gal, sizes, NULL);
}

#endif /* MIMIC_SHARED_SAGE_BULGE_SIZE_PHYSICS_H */
