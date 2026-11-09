/**
 * @file sage_bulge_size_physics.h
 * @brief SAGE26's bulge sizes: get_bulge_radius and update_instability_bulge_radius
 *
 * Implements SAGE26's two bulge-size routines, get_bulge_radius and
 * update_instability_bulge_radius, for the two bulge-size modes the package supports:
 * BulgeSizeOn 0 (no bulge sizes) and 3 (Tonini et al. 2016's separate merger and instability
 * bulges).
 *
 * - mimic_sage_get_bulge_radius(): mode 0 zeroes the three radii and returns 0. Mode 3 sums the
 *   two channel masses; a non-positive sum zeroes the three radii and returns 0; otherwise a
 *   component radius is zeroed only when its channel mass is exactly zero, a positive channel
 *   mass with a non-positive radius takes a fallback (Shen et al. 2003 eq. 33 for the merger
 *   channel; 0.2 * DiskScaleRadius, or Shen when the disk radius is not positive, for the
 *   instability channel), and the channel-mass-weighted average is stored in BulgeRadius and
 *   returned.
 * - mimic_sage_update_instability_bulge_radius(): Tonini et al. 2016 eq. 15, called after the
 *   instability channel mass has already been increased by delta_mass. Mode 0 returns at once,
 *   as does a non-positive delta_mass; otherwise the radius is the mass-weighted average of the
 *   old radius and 0.2 times the disk radius saved by the caller (or that contribution alone
 *   for the first transfer), stored in InstabilityBulgeRadius, and get_bulge_radius follows.
 *
 * Every local is a double; the fallback radii stay double locals for the weighted average even
 * though they are also stored in the float fields, and M_old is recovered from the post-store
 * float channel mass, as SAGE26 does.
 *
 * @note Used by sage26_set_disk_scale_radius (the FoF central's synchronisation after its disk
 *       radius), by the disk-instability transfer (sage_disk_instability_physics.h), by the
 *       satellite disk-star transfer of add_galaxies_together (sage_merger_physics.h), by the
 *       burst's instability channel (sage_starburst_physics.h) and, with BulgeSizeOn 3, by the
 *       merger transaction's synchronisation and remnant assignment
 *       (sage26_resolve_mergers_and_disruption).
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_BULGE_SIZE_PHYSICS_H
#define MIMIC_SHARED_SAGE_BULGE_SIZE_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "error.h"
#include "types.h"

/** @brief BulgeSizeOn 0: no bulge sizes, every radius zeroed */
#define MIMIC_SAGE_BULGE_SIZE_OFF 0
/** @brief BulgeSizeOn 3: Tonini et al. 2016 merger and instability bulges */
#define MIMIC_SAGE_BULGE_SIZE_TONINI 3

/* SAGE26's Shen et al. (2003) eq. 33 coefficients and Tonini et al. (2016) eq. 15 disk
 * fraction */
#define MIMIC_SAGE_SHEN03_SLOPE_HIGH 0.56
#define MIMIC_SAGE_SHEN03_INTERCEPT_HIGH -5.54
#define MIMIC_SAGE_TONINI16_DISK_FRAC 0.2

/**
 * @brief The bulge-size configuration a size routine reads
 *
 * SAGE26 reads run_params->BulgeSizeOn and, in mode 3, run_params->Hubble_h. A module fills it
 * from its BulgeSizeOn parameter and the core's Hubble_h.
 */
struct MimicSageBulgeSize {
  int bulge_size_on; /**< MIMIC_SAGE_BULGE_SIZE_OFF or MIMIC_SAGE_BULGE_SIZE_TONINI */
  double hubble_h;   /**< little h, read only in mode 3 */
};

/**
 * @brief Refuse a BulgeSizeOn the package does not support
 *
 * The package supports exactly SAGE26's modes 0 and 3; modes 1 and 2 (Shen-only sizes) are not
 * supported. Each module that calls a size routine loads the
 * required parameter BulgeSizeOn at initialisation and passes it here.
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded BulgeSizeOn
 * @return 0 for 0 or 3; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_bulge_size_on(const char *module_name, const int value) {
  if (value != MIMIC_SAGE_BULGE_SIZE_OFF && value != MIMIC_SAGE_BULGE_SIZE_TONINI) {
    ERROR_LOG("%s: BulgeSizeOn = %d is not supported: the package supports only 0 (no bulge "
              "sizes) and 3 (Tonini et al. 2016 merger and instability bulges)",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief The locals of one get_bulge_radius call, for tests that compare them
 *
 * Each double is SAGE26's local of the same name, NAN where the
 * call does not form it; each flag is -1 where the call does not evaluate it.
 */
struct MimicSageBulgeRadiusLocals {
  double M_merger;           /**< the float MergerBulgeMass widened */
  double M_instability;      /**< the float InstabilityBulgeMass widened */
  double M_total;            /**< M_merger + M_instability */
  double M_merger_sun;       /**< M_merger * 1e10 / h, merger fallback */
  double log_R_merger_kpc;   /**< 0.56 * log10(M_merger_sun) - 5.54 */
  double R_merger;           /**< the merger radius the average uses */
  double R_disc;             /**< the float DiskScaleRadius widened, instability fallback */
  double M_inst_sun;         /**< M_instability * 1e10 / h, Shen instability fallback */
  double log_R_inst_kpc;     /**< 0.56 * log10(M_inst_sun) - 5.54 */
  double R_instability;      /**< the instability radius the average uses */
  double R_bulge;            /**< the weighted average */
  int total_nonpositive;     /**< M_total <= 0.0 */
  int merger_mass_zero;      /**< M_merger == 0.0: MergerBulgeRadius zeroed */
  int instability_mass_zero; /**< M_instability == 0.0: InstabilityBulgeRadius zeroed */
  int merger_fallback;       /**< M_merger > 0 && R_merger <= 0: the Shen fallback ran */
  int instability_fallback;  /**< 0 none, 1 the disk fallback, 2 Shen */
};

/**
 * @brief The locals of one update_instability_bulge_radius call, for tests that compare them
 *
 * Each double is SAGE26's local of the same name, NAN where the
 * call does not form it; each flag is -1 where the call does not evaluate it.
 */
struct MimicSageInstabilityRadiusLocals {
  double M_old;                  /**< post-store float mass widened, minus delta_mass */
  double R_old;                  /**< the float InstabilityBulgeRadius widened */
  double R_disc_kpc;             /**< old_disk_radius * 1e3 / h */
  double R_new_contribution_kpc; /**< 0.2 * R_disc_kpc */
  double R_new_contribution;     /**< R_new_contribution_kpc * 1e-3 * h */
  double R_old_kpc;              /**< R_old * 1e3 / h, incremental branch */
  double M_new;                  /**< the post-store float mass widened */
  double R_new_kpc;              /**< the eq. 15 average in kpc */
  double R_new;                  /**< the radius stored, as a double */
  int returned_early;            /**< 1 mode is not 3, 2 delta_mass <= 0, 0 ran */
  int incremental;               /**< M_old > 0 && R_old > 0 */
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
 * @brief SAGE26's get_bulge_radius for BulgeSizeOn 0 and 3
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

  /* SAGE parity: BulgeSizeOn 0 zeroes the three radii and returns 0; the package accepts only 0
   * and 3 (mimic_sage_validate_bulge_size_on), so every other mode here is 0 */
  if (sizes->bulge_size_on != MIMIC_SAGE_BULGE_SIZE_TONINI) {
    gal->BulgeRadius = 0.0f;
    gal->MergerBulgeRadius = 0.0f;
    gal->InstabilityBulgeRadius = 0.0f;
    return 0.0;
  }

  const double h = sizes->hubble_h;

  /* SAGE parity: the two float channel masses are widened separately and summed in double; the
   * combined size is weighted by the channels, not BulgeMass */
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

  /* SAGE parity: a component radius is zeroed only when its channel mass is EXACTLY zero, so a
   * stale radius beside a negative channel mass survives */
  const int merger_mass_zero = M_merger == 0.0;
  const int instability_mass_zero = M_instability == 0.0;
  if (merger_mass_zero) {
    gal->MergerBulgeRadius = 0.0f;
  }
  if (instability_mass_zero) {
    gal->InstabilityBulgeRadius = 0.0f;
  }

  /* SAGE parity: a positive merger mass with a non-positive radius takes the Shen et al. 2003
   * eq. 33 fallback, read after the zeroing above; it stays a double local for the average.
   * The fma reproduces SAGE26's build */
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
   * DiskScaleRadius when that is positive, else the same Shen fallback with the instability mass;
   * the result stays a double local for the average */
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

  /* The channel-mass-weighted average of the double radii (Tonini et al. 2016 eq. 25).
   * SAGE parity: the numerator is one fma, as SAGE26's build evaluates it */
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

  /* SAGE parity: mode 0 returns at once, leaving every radius as it was, stale ones included;
   * so does a non-positive transfer. Neither synchronises */
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
   * double transfer, not from the mass before the store */
  const double M_old = gal->InstabilityBulgeMass - delta_mass;
  const double R_old = gal->InstabilityBulgeRadius;

  /* SAGE parity: the kpc round trip is kept although it cancels algebraically */
  const double R_disc_kpc = old_disk_radius * 1.0e3 / h;
  const double R_new_contribution_kpc = MIMIC_SAGE_TONINI16_DISK_FRAC * R_disc_kpc;
  const double R_new_contribution = R_new_contribution_kpc * 1.0e-3 * h;

  double R_new;
  const int incremental = M_old > 0.0 && R_old > 0.0;
  if (incremental) {
    /* Tonini et al. 2016 eq. 15, in kpc. SAGE parity: the numerator is one fma, as SAGE26's
     * build evaluates it */
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
    // First transfer: 0.2 times the saved disk radius
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

  // SAGE26 synchronises the combined radius at once; its return is discarded
  (void)mimic_sage_get_bulge_radius(gal, sizes, NULL);
}

#endif /* MIMIC_SHARED_SAGE_BULGE_SIZE_PHYSICS_H */
