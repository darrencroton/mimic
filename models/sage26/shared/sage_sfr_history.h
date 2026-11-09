/**
 * @file sage_sfr_history.h
 * @brief SAGE26's per-substep star-formation histories and their output averaging
 *
 * SAGE26 keeps six float arrays of STEPS = 10 bins in struct GALAXY
 * (R/src/core_allvars.h:158-163, R = the staged SAGE26 source; STEPS at
 * R/src/macros.h:17): the disk and bulge star-formation rates SfrDisk and SfrBulge
 * (1e10 Msun/h per code time), and the cold gas and its metals sampled with each
 * (SfrDiskColdGas, SfrDiskColdGasMetals, SfrBulgeColdGas, SfrBulgeColdGasMetals,
 * 1e10 Msun/h), plus the int SubstepsUsed (:164). The property generator emits
 * scalars only, so the package holds each element as its own float property,
 * <history>Bin<k> (models/sage26/model_properties.yaml, output: false), and this
 * header is the one way modules reach them:
 *
 * - mimic_sage_sfr_history(): the element (history, bin) of a galaxy;
 * - mimic_sage_sfr_step_bin(): the bin of a substep, (step * STEPS) / N capped at
 *   STEPS - 1 (R/src/core_build_model.c:514-515, :579-580);
 * - mimic_sage_sample_disk_sfr() and mimic_sage_sample_bulge_sfr(): SAGE26's disk
 *   (R/src/model_starformation_and_feedback.c:1058-1060) and burst
 *   (R/src/model_mergers.c:755-757) sampling, with SAGE26's operators and float
 *   stores;
 * - mimic_sage_reset_sfr_histories(): SAGE26's reset of every bin at the
 *   first-occupied Type 0/1 update (R/src/core_build_model.c:278-282), which the
 *   package applies at joining instead of an init_repeat reset of every row;
 * - mimic_sage_average_sfr_histories(): the writer's averaging of the six
 *   histories into SfrDisk, SfrBulge, SfrDiskZ and SfrBulgeZ
 *   (R/src/io/save_gals_hdf5.c:934-959), with its divisors
 *   (R/src/model_misc.h:49-58).
 *
 * None of these expressions contains a multiply-add: each sample adds a quotient
 * or a float, and each averaging term ends in a division, so floating-point
 * contraction cannot fuse them and they are written left to right as SAGE26 does.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_SFR_HISTORY_H
#define MIMIC_SHARED_SAGE_SFR_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#include "types.h"
#include "module_system/physical_constants.h"

/* SAGE26's STEPS, the number of history bins (R/src/macros.h:17) */
#define MIMIC_SAGE_SFR_STEPS 10

/* SAGE26's SEC_PER_YEAR and SOLAR_MASS (R/src/macros.h:38, :24), as the writer's
 * rate conversion uses them (R/src/io/save_gals_hdf5.c:944-945): the same literals, 3.155e7 and
 * 1.989e33, as physical_constants.h's, which these names alias */
#define MIMIC_SAGE_SEC_PER_YEAR SEC_PER_YEAR
#define MIMIC_SAGE_SOLAR_MASS SOLAR_MASS

/** @brief SAGE26's six per-substep histories (R/src/core_allvars.h:158-163) */
enum MimicSageSfrHistory {
  MIMIC_SFR_HISTORY_DISK = 0,              /* SfrDisk[STEPS] */
  MIMIC_SFR_HISTORY_BULGE,                 /* SfrBulge[STEPS] */
  MIMIC_SFR_HISTORY_DISK_COLD_GAS,         /* SfrDiskColdGas[STEPS] */
  MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS,  /* SfrDiskColdGasMetals[STEPS] */
  MIMIC_SFR_HISTORY_BULGE_COLD_GAS,        /* SfrBulgeColdGas[STEPS] */
  MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, /* SfrBulgeColdGasMetals[STEPS] */
  MIMIC_SFR_HISTORY_COUNT
};

#define MIMIC_SAGE_SFR_HISTORY_BINS(member)                                                        \
  switch (bin) {                                                                                   \
  case 0:                                                                                          \
    return &gal->member##Bin0;                                                                     \
  case 1:                                                                                          \
    return &gal->member##Bin1;                                                                     \
  case 2:                                                                                          \
    return &gal->member##Bin2;                                                                     \
  case 3:                                                                                          \
    return &gal->member##Bin3;                                                                     \
  case 4:                                                                                          \
    return &gal->member##Bin4;                                                                     \
  case 5:                                                                                          \
    return &gal->member##Bin5;                                                                     \
  case 6:                                                                                          \
    return &gal->member##Bin6;                                                                     \
  case 7:                                                                                          \
    return &gal->member##Bin7;                                                                     \
  case 8:                                                                                          \
    return &gal->member##Bin8;                                                                     \
  case 9:                                                                                          \
    return &gal->member##Bin9;                                                                     \
  default:                                                                                         \
    return NULL;                                                                                   \
  }

/**
 * @brief The element (history, bin) of a galaxy, SAGE26's <history>[bin]
 *
 * @return The float property holding it, or NULL for a NULL galaxy, an unknown
 *         history or a bin outside [0, MIMIC_SAGE_SFR_STEPS)
 */
static inline float *mimic_sage_sfr_history(struct GalaxyData *gal,
                                            const enum MimicSageSfrHistory history, const int bin) {
  if (gal == NULL) {
    return NULL;
  }
  switch (history) {
  case MIMIC_SFR_HISTORY_DISK:
    MIMIC_SAGE_SFR_HISTORY_BINS(SfrDisk)
  case MIMIC_SFR_HISTORY_BULGE:
    MIMIC_SAGE_SFR_HISTORY_BINS(SfrBulge)
  case MIMIC_SFR_HISTORY_DISK_COLD_GAS:
    MIMIC_SAGE_SFR_HISTORY_BINS(SfrDiskColdGas)
  case MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS:
    MIMIC_SAGE_SFR_HISTORY_BINS(SfrDiskColdGasMetals)
  case MIMIC_SFR_HISTORY_BULGE_COLD_GAS:
    MIMIC_SAGE_SFR_HISTORY_BINS(SfrBulgeColdGas)
  case MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS:
    MIMIC_SAGE_SFR_HISTORY_BINS(SfrBulgeColdGasMetals)
  default:
    return NULL;
  }
}

#undef MIMIC_SAGE_SFR_HISTORY_BINS

/**
 * @brief The history bin of substep step of num_substeps (R/src/core_build_model.c:514-515)
 *
 *   int step_bin = (step * STEPS) / effective_steps;
 *   if(step_bin >= STEPS) step_bin = STEPS - 1;
 *
 * num_substeps is the package's substep count for the halo, which stands for
 * SAGE26's effective_steps. With more substeps than bins several substeps share a
 * bin; with fewer, some bins are never sampled.
 *
 * @return The bin, or -1 when step is outside [0, num_substeps) or num_substeps < 1
 */
static inline int mimic_sage_sfr_step_bin(const int step, const int num_substeps) {
  if (num_substeps < 1 || step < 0 || step >= num_substeps) {
    return -1;
  }
  /* The product in 64 bits cannot overflow for any int step; the quotient is below STEPS */
  int step_bin = (int)(((int64_t)step * MIMIC_SAGE_SFR_STEPS) / num_substeps);
  if (step_bin >= MIMIC_SAGE_SFR_STEPS) {
    step_bin = MIMIC_SAGE_SFR_STEPS - 1;
  }
  return step_bin;
}

/**
 * @brief SAGE26's disk history sampling (R/src/model_starformation_and_feedback.c:1058-1060)
 *
 *   galaxies[p].SfrDisk[step] += stars / dt;
 *   galaxies[p].SfrDiskColdGas[step] = galaxies[p].ColdGas;
 *   galaxies[p].SfrDiskColdGasMetals[step] = galaxies[p].MetalsColdGas;
 *
 * stars is the renormalised disk star-formation mass and the cold gas is the
 * reservoir before this substep's star formation commits anything. The rate is
 * the float element plus the double quotient, rounded to float; the gas samples
 * are assignments, so the last substep mapped to a bin wins.
 *
 * @return 0, or -1 for a NULL galaxy or an invalid bin (nothing is written)
 */
static inline int mimic_sage_sample_disk_sfr(struct GalaxyData *gal, const int bin,
                                             const double stars, const double dt) {
  float *rate = mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK, bin);
  float *gas = mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, bin);
  float *metals = mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, bin);
  if (rate == NULL || gas == NULL || metals == NULL) {
    return -1;
  }
  *rate += stars / dt;
  *gas = gal->ColdGas;
  *metals = gal->MetalsColdGas;
  return 0;
}

/**
 * @brief SAGE26's burst history sampling (R/src/model_mergers.c:755-757)
 *
 *   galaxies[merger_centralgal].SfrBulge[step] += stars / dt;
 *   galaxies[merger_centralgal].SfrBulgeColdGas[step] += galaxies[merger_centralgal].ColdGas;
 *   galaxies[merger_centralgal].SfrBulgeColdGasMetals[step] +=
 *       galaxies[merger_centralgal].MetalsColdGas;
 *
 * gal is the bursting galaxy (SAGE26's merger_centralgal), stars the burst mass
 * after its feedback renormalisation and dt the substep interval. Unlike the disk
 * sampling, the gas samples accumulate (+=, in float), so several bursts in one
 * bin add their cold gas.
 *
 * @return 0, or -1 for a NULL galaxy or an invalid bin (nothing is written)
 */
static inline int mimic_sage_sample_bulge_sfr(struct GalaxyData *gal, const int bin,
                                              const double stars, const double dt) {
  float *rate = mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE, bin);
  float *gas = mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, bin);
  float *metals = mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, bin);
  if (rate == NULL || gas == NULL || metals == NULL) {
    return -1;
  }
  *rate += stars / dt;
  *gas += gal->ColdGas;
  *metals += gal->MetalsColdGas;
  return 0;
}

/**
 * @brief SAGE26's history reset at the first-occupied update (R/src/core_build_model.c:278-282)
 *
 *   for(int step = 0; step < STEPS; step++) {
 *       galaxies[ngal].SfrDisk[step] = galaxies[ngal].SfrBulge[step] = 0.0;
 *       galaxies[ngal].SfrDiskColdGas[step] = galaxies[ngal].SfrDiskColdGasMetals[step] = 0.0;
 *       galaxies[ngal].SfrBulgeColdGas[step] = galaxies[ngal].SfrBulgeColdGasMetals[step] = 0.0;
 *   }
 *
 * Zeroes every bin of the six histories. SAGE26 runs it only for a galaxy that was a
 * Type 0 or 1 and joins on the main branch (it stays a Type 0 or 1); a galaxy that
 * becomes or stays an orphan keeps its histories. sage26_initialise_merger_clock calls
 * it at joining for exactly those rows.
 *
 * @return 0, or -1 for a NULL galaxy (nothing is written)
 */
static inline int mimic_sage_reset_sfr_histories(struct GalaxyData *gal) {
  if (gal == NULL) {
    return -1;
  }
  for (int history = 0; history < MIMIC_SFR_HISTORY_COUNT; history++) {
    for (int bin = 0; bin < MIMIC_SAGE_SFR_STEPS; bin++) {
      *mimic_sage_sfr_history(gal, (enum MimicSageSfrHistory)history, bin) = 0.0f;
    }
  }
  return 0;
}

/** @brief SAGE26's sfr_rate_divisor (R/src/model_misc.h:49-52) */
static inline int mimic_sage_sfr_rate_divisor(const int substeps_used) {
  return (substeps_used > 0) ? substeps_used : MIMIC_SAGE_SFR_STEPS;
}

/** @brief SAGE26's sfr_metallicity_divisor (R/src/model_misc.h:54-58) */
static inline int mimic_sage_sfr_metallicity_divisor(const int substeps_used) {
  const int n = mimic_sage_sfr_rate_divisor(substeps_used);
  return (n < MIMIC_SAGE_SFR_STEPS) ? n : MIMIC_SAGE_SFR_STEPS;
}

/** @brief The writer's locals, reported for tests (may be ignored) */
struct MimicSageSfrAverageLocals {
  int sfr_rate_divisor;        /* sfr_norm */
  int sfr_metallicity_divisor; /* met_norm */
  int disk_bins_skipped;       /* bins whose SfrDiskColdGas is not positive */
  int bulge_bins_skipped;      /* bins whose SfrBulgeColdGas is not positive */
};

/**
 * @brief The writer's averaging of the six histories (R/src/io/save_gals_hdf5.c:934-959)
 *
 *   float tmp_SfrDisk = 0.0; ... (four float accumulators)
 *   const int sfr_norm = sfr_rate_divisor(g->SubstepsUsed);
 *   const int met_norm = sfr_metallicity_divisor(g->SubstepsUsed);
 *   for(int step = 0; step < STEPS; step++) {
 *       tmp_SfrDisk += g->SfrDisk[step] * UnitMass_in_g / UnitTime_in_s
 *                      * SEC_PER_YEAR / SOLAR_MASS / sfr_norm;           (and SfrBulge)
 *       if(g->SfrDiskColdGas[step] > 0.0)
 *           tmp_SfrDiskZ += g->SfrDiskColdGasMetals[step] / g->SfrDiskColdGas[step] / met_norm;
 *       if(g->SfrBulgeColdGas[step] > 0.0)   (the same for SfrBulgeZ)
 *   }
 *
 * Writes the four results into the galaxy's output scalars SfrDisk and SfrBulge
 * (Msun/yr) and SfrDiskZ and SfrBulgeZ (dimensionless), which are written to the
 * output as they are. Each rate term is a double expression evaluated left to
 * right from the float element and added to the float accumulator in double, then
 * rounded to float; each metallicity term is float arithmetic throughout, the int
 * divisor converted to float. A bin whose gas sample is not positive (zero, -0 or
 * negative) adds no metallicity term; its rate term still counts.
 *
 * @param gal             Galaxy whose SubstepsUsed and histories are read
 * @param unit_mass_in_g  SAGE26's UnitMass_in_g
 * @param unit_time_in_s  SAGE26's UnitTime_in_s (UnitLength_in_cm / UnitVelocity_in_cm_per_s)
 * @param locals          Optional: receives the divisors and skipped-bin counts
 */
static inline void mimic_sage_average_sfr_histories(struct GalaxyData *gal,
                                                    const double unit_mass_in_g,
                                                    const double unit_time_in_s,
                                                    struct MimicSageSfrAverageLocals *locals) {
  if (gal == NULL) {
    return;
  }
  float tmp_SfrDisk = 0.0;
  float tmp_SfrBulge = 0.0;
  float tmp_SfrDiskZ = 0.0;
  float tmp_SfrBulgeZ = 0.0;
  int disk_bins_skipped = 0;
  int bulge_bins_skipped = 0;

  const int sfr_norm = mimic_sage_sfr_rate_divisor(gal->SubstepsUsed);
  const int met_norm = mimic_sage_sfr_metallicity_divisor(gal->SubstepsUsed);
  for (int step = 0; step < MIMIC_SAGE_SFR_STEPS; step++) {
    const float sfr_disk = *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK, step);
    const float sfr_bulge = *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE, step);
    const float disk_gas = *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, step);
    const float disk_metals =
        *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, step);
    const float bulge_gas = *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS, step);
    const float bulge_metals =
        *mimic_sage_sfr_history(gal, MIMIC_SFR_HISTORY_BULGE_COLD_GAS_METALS, step);

    tmp_SfrDisk += sfr_disk * unit_mass_in_g / unit_time_in_s * MIMIC_SAGE_SEC_PER_YEAR /
                   MIMIC_SAGE_SOLAR_MASS / sfr_norm;
    tmp_SfrBulge += sfr_bulge * unit_mass_in_g / unit_time_in_s * MIMIC_SAGE_SEC_PER_YEAR /
                    MIMIC_SAGE_SOLAR_MASS / sfr_norm;

    if (disk_gas > 0.0) {
      tmp_SfrDiskZ += disk_metals / disk_gas / met_norm;
    } else {
      disk_bins_skipped++;
    }

    if (bulge_gas > 0.0) {
      tmp_SfrBulgeZ += bulge_metals / bulge_gas / met_norm;
    } else {
      bulge_bins_skipped++;
    }
  }

  gal->SfrDisk = tmp_SfrDisk;
  gal->SfrBulge = tmp_SfrBulge;
  gal->SfrDiskZ = tmp_SfrDiskZ;
  gal->SfrBulgeZ = tmp_SfrBulgeZ;

  if (locals != NULL) {
    locals->sfr_rate_divisor = sfr_norm;
    locals->sfr_metallicity_divisor = met_norm;
    locals->disk_bins_skipped = disk_bins_skipped;
    locals->bulge_bins_skipped = bulge_bins_skipped;
  }
}

#endif /* MIMIC_SHARED_SAGE_SFR_HISTORY_H */
