/**
 * @file sage_h2_physics.h
 * @brief SAGE26's Blitz & Rosolowsky (2006) molecular gas with radial integration (SFprescription
 * 1)
 *
 * Ports the molecular-gas routines SAGE26's SFprescription 1 runs (R/src/model_h2_chemistry.c,
 * R = the staged SAGE26 source), and the configuration checks of the parameters that select them:
 *
 * - mimic_sage_br06_stellar_scale_height(): the static calculate_stellar_scale_height_BR06
 *   (:48-60), BR06 eq. 9, log h* = -0.23 + 0.8 log R*. The source comment (:51) gives the slope as
 *   -0.8; the code, mirrored here, uses +0.8.
 * - mimic_sage_br06_molecular_fraction_from_hstar(): the static
 *   calculate_molecular_fraction_BR06_from_hstar (:68-101): the stellar-dominated midplane
 *   pressure of BR06 eq. 5 with the stellar surface density floored at 0.1 Msun/pc^2 (also for a
 *   gas-only disk), R_mol = (P / P0)^alpha with BR06 eq. 13's P0 = 4.3e4 K cm^-3 and alpha = 0.92,
 *   and f_mol = R_mol / (1 + R_mol).
 * - mimic_sage_br06_molecular_fraction(): the public calculate_molecular_fraction_BR06 (:117-125).
 * - mimic_sage_h2_radial_integration(): the public calculate_molecular_fraction_radial_integration
 *   (:134-290) as sfr_br06 calls it (R/src/model_starformation_and_feedback.c:222): prescription 1
 *   and a NULL SFR-output pointer, so the K13 depletion-time branch (:268-272, :279-285) never runs
 *   and H2DepletionTime_Gyr is left as it was. The H2 mass of an exponential gas disk is the sum,
 *   in increasing radius, of N_BINS annuli out to H2RadialRMaxFactor * r_s, each its BR06 molecular
 *   fraction times 0.74 of the annulus gas; bins below 1e-3 Msun/pc^2 are skipped and the
 *   truncated disk is not renormalised.
 *
 * Precision. SAGE26 freezes these fits in single precision (:10-12, :138), and every boundary is
 * mirrored: h, rs_pc, the total masses, the central surface densities, R_MAX, dr, every annulus
 * quantity and the accumulator are floats; the unit macros (R/src/macros.h:45-47) multiply by the
 * double 1e6 or 1e10 and divide by the float h widened, left to right, then narrow; the disk
 * stellar mass subtracts BulgeMass in float first; the central surface densities divide by the
 * double 2.0 * M_PI * rs_pc * rs_pc; the annulus exponential is expf and the annulus mass the float
 * product 2.0f * (float)M_PI * r * sigma * 0.74f * f_mol * dr, left to right; the conversion back
 * multiplies the float total by the float h in float, divides by the double 1e10 and narrows. In
 * the molecular fraction the pressure is formed in double and narrowed, P0, alpha and v_g are float
 * constants (each the double literal narrowed, as SAGE26 writes them), the ratio is a float
 * division, R_mol is double pow of the widened floats narrowed to float, and f_mol is double.
 * SAGE26 calls pow, never powf, for R_mol. The 0.74 of the annulus is the float 0.74f, distinct
 * from the double HYDROGEN_MASS_FRAC of the callers' caps.
 *
 * Rounding points. SAGE26's object fuses exactly one multiply-add on these paths (oracle README,
 * "The c08 Group", Fused Multiply-Adds), the height's -0.23 + 0.8 * log10(R*) (:56), in the public
 * routine (model_h2_chemistry.o offset 0x50) and in its inlined copy in the radial integration
 * (0x35c); it is written as an explicit fma(). SAGE26's object evaluates the height's pow(10.0,
 * log_h_star) (:57) by a call to __exp10 (clang's library-call simplification of pow with base 10);
 * the package writes pow(10.0, ...), whose float result equals __exp10's for every float argument
 * the height can receive (mimic_sage_br06_stellar_scale_height). Every other expression is one C
 * expression with SAGE26's operand order and types, none of which clang contracts.
 *
 * @note Used by sage26_calculate_star_formation for its BR06 branch (SFprescription 1), and by
 *       the oracle tests of that module, which call the public routines directly.
 */

#ifndef MIMIC_SHARED_SAGE_H2_PHYSICS_H
#define MIMIC_SHARED_SAGE_H2_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "error.h"
#include "types.h"

/** @brief SFprescription 0: Croton et al. (2006) critical-mass law (R0 and R1) */
#define MIMIC_SAGE_SF_PRESCRIPTION_CROTON 0
/** @brief SFprescription 1: BR06 molecular gas with radial integration (R2 onward) */
#define MIMIC_SAGE_SF_PRESCRIPTION_BR06 1

/** @brief The supported H2RadialIntegrationOn, H2RadialNBins and H2RadialRMaxFactor (SAGE26's
 *         defaults, R/src/core_read_parameter_file.c:99-101) */
#define MIMIC_SAGE_H2_RADIAL_INTEGRATION_ON 1
#define MIMIC_SAGE_H2_RADIAL_NBINS 25
#define MIMIC_SAGE_H2_RADIAL_RMAX_FACTOR 5.0

/** @brief The supported StarburstColdGasOn: every non-FFB burst uses the cold gas (SAGE26's
 *         default, R/src/core_read_parameter_file.c:109) */
#define MIMIC_SAGE_STARBURST_COLD_GAS_ON 1

/**
 * @brief SAGE26's sf_prescription_tracks_h2 (R/src/model_misc.h:93-96)
 *
 * Prescriptions 0 (Croton) and 2 (Somerville cold gas) never set H2gas or H1gas; every other one
 * tracks them, and the commits' hydrogen caps run under this test
 * (R/src/model_starformation_and_feedback.c:1131, :1169). The package supports 0 and 1.
 */
static inline int mimic_sage_sf_prescription_tracks_h2(const int sf_prescription) {
  return sf_prescription != 0 && sf_prescription != 2;
}

/**
 * @brief Refuse an SFprescription the package does not support
 *
 * The package ports exactly SAGE26's prescriptions 0 (R0 and R1) and 1 with radial integration
 * (R2 onward); 2-7 are not ported. Each module whose code reaches a branch or cap the
 * prescription selects loads the required parameter SFprescription and passes it here.
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded SFprescription
 * @return 0 for 0 or 1; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_sf_prescription(const char *module_name, const int value) {
  if (value != MIMIC_SAGE_SF_PRESCRIPTION_CROTON && value != MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    ERROR_LOG("%s: SFprescription = %d is not supported: the package ports only 0 (Croton et al. "
              "2006) and 1 (Blitz & Rosolowsky 2006 with radial integration)",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief Refuse H2 integration parameters the package does not support
 *
 * The package ports the radial integration only as SAGE26's defaults configure it: radial
 * integration on (the single-slab branch, R/src/model_starformation_and_feedback.c:225-236, and
 * its H2DiskAreaOption are not ported), 25 bins out to 5 disk scale radii.
 *
 * @return 0 when all three are supported; -1 (with an error naming each refused parameter)
 */
static inline int mimic_sage_validate_h2_radial_parameters(const char *module_name,
                                                           const int integration_on,
                                                           const int n_bins,
                                                           const double r_max_factor) {
  int status = 0;
  if (integration_on != MIMIC_SAGE_H2_RADIAL_INTEGRATION_ON) {
    ERROR_LOG("%s: H2RadialIntegrationOn = %d is not supported: the package ports only 1 (radial "
              "integration; the single-slab BR06 branch is not ported)",
              module_name, integration_on);
    status = -1;
  }
  if (n_bins != MIMIC_SAGE_H2_RADIAL_NBINS) {
    ERROR_LOG("%s: H2RadialNBins = %d is not supported: the package ports only %d", module_name,
              n_bins, MIMIC_SAGE_H2_RADIAL_NBINS);
    status = -1;
  }
  if (!(r_max_factor == MIMIC_SAGE_H2_RADIAL_RMAX_FACTOR)) {
    ERROR_LOG("%s: H2RadialRMaxFactor = %.17g is not supported: the package ports only %.1f",
              module_name, r_max_factor, MIMIC_SAGE_H2_RADIAL_RMAX_FACTOR);
    status = -1;
  }
  return status;
}

/**
 * @brief Refuse a StarburstColdGasOn the package does not support
 *
 * StarburstColdGasOn 0 bursts fresh H2 (R/src/model_mergers.c:606-679), which is not ported; the
 * package bursts the cold gas, SAGE26's default 1.
 *
 * @return 0 for 1; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_starburst_cold_gas_on(const char *module_name,
                                                            const int value) {
  if (value != MIMIC_SAGE_STARBURST_COLD_GAS_ON) {
    ERROR_LOG("%s: StarburstColdGasOn = %d is not supported: the package ports only 1 (bursts "
              "use the cold gas; the H2 burst fuel of 0 is not ported)",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief The locals of one BR06 molecular-fraction call, for tests that compare them
 *
 * Each value is SAGE26's local of the same name (R/src/model_h2_chemistry.c:52-100), NAN where
 * the call does not form it; each flag is -1 where the call does not evaluate it.
 */
struct MimicSageBr06Locals {
  float log_h_star;             /**< -0.23 + 0.8 * log10(R*), fused, narrowed (:56) */
  float h_star_pc;              /**< pow(10, log_h_star) narrowed (:57) */
  float effective_sigma_stars;  /**< the stellar surface density after its floor (:73-76) */
  float pressure;               /**< the midplane pressure, narrowed (:80) */
  float pressure_ratio;         /**< pressure / P0 in float (:93) */
  float R_mol;                  /**< pow(ratio, alpha) narrowed (:94) */
  double f_mol;                 /**< R_mol / (1.0 + R_mol) in double (:98) */
  int scale_length_nonpositive; /**< the public routine's disk_scale_length_pc <= 0.0 (:120) */
  int pressure_branch;          /**< gas > 0.0 && h_star > 0.0 (:72) */
  int stellar_floor_binds;      /**< stellar surface density < 0.1 (:74) */
  int pressure_nonpositive;     /**< pressure <= 0.0 returns 0 (:83) */
};

/** @brief Mark every value of a BR06 locals record as not formed */
static inline void mimic_sage_br06_locals_reset(struct MimicSageBr06Locals *locals) {
  if (locals == NULL) {
    return;
  }
  locals->log_h_star = NAN;
  locals->h_star_pc = NAN;
  locals->effective_sigma_stars = NAN;
  locals->pressure = NAN;
  locals->pressure_ratio = NAN;
  locals->R_mol = NAN;
  locals->f_mol = NAN;
  locals->scale_length_nonpositive = -1;
  locals->pressure_branch = -1;
  locals->stellar_floor_binds = -1;
  locals->pressure_nonpositive = -1;
}

/**
 * @brief SAGE26's calculate_stellar_scale_height_BR06 (R/src/model_h2_chemistry.c:48-60)
 *
 * @param disk_scale_length_pc  R* in pc (float)
 * @param log_h_star_out        Optional: receives log_h_star (left unchanged where not formed)
 * @return h* in pc (float); 0 for a non-positive scale length (:52-54)
 */
static inline float mimic_sage_br06_stellar_scale_height(const float disk_scale_length_pc,
                                                         float *log_h_star_out) {
  /* The guard compares the float with the double 0.0 (:52); neither public routine reaches it
   * with a non-positive length, as each returns first (:120, :141) */
  if (disk_scale_length_pc <= 0.0) {
    return 0.0f;
  }
  /* SAGE parity: +0.8, as the code has it; the comment above it (:51) states -0.8 log R*. SAGE26's
   * object fuses -0.23 + 0.8 * log10(R*) into one multiply-add (:56, model_h2_chemistry.o 0x50 and
   * the radial copy 0x35c) and narrows it to float. */
  const float log_h_star = (float)fma(0.8, log10((double)disk_scale_length_pc), -0.23);
  /* SAGE parity: double pow(10.0, ...) of the widened float, narrowed (:57). The reference object
   * calls __exp10 here (clang's simplification of pow with base 10), as Mimic's -O2 build does and
   * its -O0 unit-test build does not; the two differ in the double for some inputs but, checked
   * for every float from -37 to 31 (the range a positive float R* gives), never after this
   * narrowing */
  const float h_star_pc = (float)pow(10.0, (double)log_h_star);
  if (log_h_star_out != NULL) {
    *log_h_star_out = log_h_star;
  }
  return h_star_pc;
}

/**
 * @brief SAGE26's calculate_molecular_fraction_BR06_from_hstar (R/src/model_h2_chemistry.c:68-101)
 *
 * @param gas_surface_density_msun_pc2      Sigma_gas, Msun/pc^2 (float)
 * @param stellar_surface_density_msun_pc2  Sigma_star, Msun/pc^2 (float)
 * @param h_star_pc                         The stellar scale height, pc (float)
 * @param locals                            Optional (NULL in production): receives the locals
 * @return The molecular fraction f_mol in [0, 1] (float)
 */
static inline float mimic_sage_br06_molecular_fraction_from_hstar(
    const float gas_surface_density_msun_pc2, const float stellar_surface_density_msun_pc2,
    const float h_star_pc, struct MimicSageBr06Locals *locals) {
  float pressure = 0.0f;
  const int pressure_branch = gas_surface_density_msun_pc2 > 0.0 && h_star_pc > 0.0;
  if (locals != NULL) {
    locals->pressure_branch = pressure_branch;
  }
  if (pressure_branch) {
    float effective_sigma_stars = stellar_surface_density_msun_pc2;
    /* SAGE parity: the floor applies to any density below 0.1, zero and negative included, so a
     * gas-only disk still has a pressure (:73-76); 0.1 is the double literal, stored in float */
    const int stellar_floor_binds = stellar_surface_density_msun_pc2 < 0.1;
    if (stellar_floor_binds) {
      effective_sigma_stars = (float)0.1;
    }
    const float v_g = (float)8.0; /* km/s, BR06's gas velocity dispersion (:77) */
    /* BR06 eq. 5, left to right in double, narrowed (:80) */
    pressure = (float)(272.0 * gas_surface_density_msun_pc2 * sqrt(effective_sigma_stars) * v_g /
                       sqrt(h_star_pc));
    if (locals != NULL) {
      locals->stellar_floor_binds = stellar_floor_binds;
      locals->effective_sigma_stars = effective_sigma_stars;
    }
  }
  if (locals != NULL) {
    locals->pressure = pressure;
  }

  const int pressure_nonpositive = pressure <= 0.0;
  if (locals != NULL) {
    locals->pressure_nonpositive = pressure_nonpositive;
  }
  if (pressure_nonpositive) {
    return 0.0f;
  }

  /* SAGE parity: BR06 eq. 13's constants are float (:89-90), each the double literal narrowed */
  const float P0 = (float)4.3e4;
  const float alpha = (float)0.92;

  /* BR06 eq. 11 (:93-94): a float ratio, then double pow of the widened floats, narrowed */
  const float pressure_ratio = pressure / P0;
  const float R_mol = (float)pow((double)pressure_ratio, (double)alpha);

  /* The molecular-to-atomic ratio as a fraction, in double (:98), narrowed by the return */
  const double f_mol = R_mol / (1.0 + R_mol);

  if (locals != NULL) {
    locals->pressure_ratio = pressure_ratio;
    locals->R_mol = R_mol;
    locals->f_mol = f_mol;
  }
  return (float)f_mol;
}

/**
 * @brief SAGE26's calculate_molecular_fraction_BR06 (R/src/model_h2_chemistry.c:117-125)
 *
 * @param gas_surface_density_msun_pc2      Sigma_gas, Msun/pc^2 (float)
 * @param stellar_surface_density_msun_pc2  Sigma_star, Msun/pc^2 (float)
 * @param disk_scale_length_pc              r_s, pc (float); sets the stellar scale height
 * @param locals                            Optional (NULL in production): receives the locals
 * @return The molecular fraction (float); 0 for a non-positive scale length (:120-122)
 */
static inline float mimic_sage_br06_molecular_fraction(const float gas_surface_density_msun_pc2,
                                                       const float stellar_surface_density_msun_pc2,
                                                       const float disk_scale_length_pc,
                                                       struct MimicSageBr06Locals *locals) {
  mimic_sage_br06_locals_reset(locals);
  const int scale_length_nonpositive = disk_scale_length_pc <= 0.0;
  if (locals != NULL) {
    locals->scale_length_nonpositive = scale_length_nonpositive;
  }
  if (scale_length_nonpositive) {
    return 0.0f;
  }
  const float h_star_pc = mimic_sage_br06_stellar_scale_height(
      disk_scale_length_pc, locals != NULL ? &locals->log_h_star : NULL);
  if (locals != NULL) {
    locals->h_star_pc = h_star_pc;
  }
  return mimic_sage_br06_molecular_fraction_from_hstar(
      gas_surface_density_msun_pc2, stellar_surface_density_msun_pc2, h_star_pc, locals);
}

/** @brief One annulus of the radial integration, for tests that compare it */
struct MimicSageH2RadialBin {
  float r;                 /**< the bin centre, (i + 0.5f) * dr (:212) */
  float exp_factor;        /**< expf(-r / rs_pc) (:215) */
  float sigma_gas_r;       /**< sigma_gas_0 * exp_factor (:216) */
  float sigma_star_r;      /**< sigma_star_0 * exp_factor (:217) */
  int skipped;             /**< sigma_gas_r < 1e-3f: the bin adds nothing (:220) */
  float f_mol_r;           /**< the bin's BR06 molecular fraction (:227); NAN when skipped */
  int stellar_floor_binds; /**< the bin's stellar floor (:74); -1 when skipped */
  float dM_H2;             /**< the annulus H2 mass (:263); NAN when skipped */
  float M_H2_total;        /**< the accumulator after the bin (:265); NAN when skipped */
};

/**
 * @brief The locals of one radial-integration call, for tests that compare them
 *
 * Each value is SAGE26's local of the same name (R/src/model_h2_chemistry.c:138-276), NAN where
 * the call does not form it; each flag is -1 where the call does not evaluate it.
 */
struct MimicSageH2RadialLocals {
  float h;                 /**< Hubble_h narrowed (:138) */
  float rs_pc;             /**< CODE_LENGTH_TO_PC(DiskScaleRadius, h) narrowed (:139) */
  int returned_early;      /**< rs_pc <= 0.0 || ColdGas <= 0.0 (:141) */
  float M_gas_total;       /**< CODE_MASS_TO_MSUN(ColdGas, h) narrowed (:146) */
  float M_disk_star;       /**< CODE_MASS_TO_MSUN(StellarMass - BulgeMass, h) narrowed (:147) */
  float sigma_gas_0;       /**< the central gas surface density (:150) */
  int disk_stars_positive; /**< M_disk_star > 0.0 (:151) */
  float sigma_star_0;      /**< the central stellar surface density, 0 without disk stars */
  float R_MAX;             /**< (float)H2RadialRMaxFactor * rs_pc (:155) */
  float dr;                /**< R_MAX / N_BINS in float (:156) */
  float log_h_star;        /**< the height's log, once for every bin (:177-178, :56) */
  float h_star_br06;       /**< the stellar scale height (:177-178) */
  int bins_recorded; /**< bins written to bin[] (N_BINS, at most MIMIC_SAGE_H2_RADIAL_NBINS) */
  struct MimicSageH2RadialBin bin[MIMIC_SAGE_H2_RADIAL_NBINS];
  float M_H2_total;    /**< the accumulator after the last bin (:208-273) */
  float H2_code_units; /**< MSUN_TO_CODE_MASS(M_H2_total, h) narrowed (:276) */
};

/** @brief Mark every value of a radial locals record as not formed */
static inline void mimic_sage_h2_radial_locals_reset(struct MimicSageH2RadialLocals *locals) {
  if (locals == NULL) {
    return;
  }
  locals->h = NAN;
  locals->rs_pc = NAN;
  locals->returned_early = -1;
  locals->M_gas_total = NAN;
  locals->M_disk_star = NAN;
  locals->sigma_gas_0 = NAN;
  locals->disk_stars_positive = -1;
  locals->sigma_star_0 = NAN;
  locals->R_MAX = NAN;
  locals->dr = NAN;
  locals->log_h_star = NAN;
  locals->h_star_br06 = NAN;
  locals->bins_recorded = 0;
  for (int i = 0; i < MIMIC_SAGE_H2_RADIAL_NBINS; i++) {
    struct MimicSageH2RadialBin *b = &locals->bin[i];
    b->r = NAN;
    b->exp_factor = NAN;
    b->sigma_gas_r = NAN;
    b->sigma_star_r = NAN;
    b->skipped = -1;
    b->f_mol_r = NAN;
    b->stellar_floor_binds = -1;
    b->dM_H2 = NAN;
    b->M_H2_total = NAN;
  }
  locals->M_H2_total = NAN;
  locals->H2_code_units = NAN;
}

/**
 * @brief SAGE26's calculate_molecular_fraction_radial_integration as sfr_br06 calls it
 *        (R/src/model_h2_chemistry.c:134-290; R/src/model_starformation_and_feedback.c:222)
 *
 * SFprescription 1 (BR06) and a NULL SFR-output pointer: the GD14, KD12, KMT09 and K13
 * branches and the K13 SFR and depletion time (:161-205, :229-259, :267-272, :279-285) do not
 * run, and H2DepletionTime_Gyr is not written. Writes H2gas, except on the early return.
 *
 * @param gal           The galaxy: reads DiskScaleRadius, ColdGas, StellarMass and BulgeMass;
 *                      writes H2gas
 * @param hubble_h      Hubble_h (double, narrowed to float as SAGE26 does, :138)
 * @param n_bins        H2RadialNBins (25)
 * @param r_max_factor  H2RadialRMaxFactor (5.0)
 * @param locals        Optional (NULL in production): receives the locals
 * @return The H2 mass in 1e10 Msun/h (float); 0, with H2gas NOT written, for rs_pc <= 0 or
 *         ColdGas <= 0 (:141-143)
 */
static inline float mimic_sage_h2_radial_integration(struct GalaxyData *gal, const double hubble_h,
                                                     const int n_bins, const double r_max_factor,
                                                     struct MimicSageH2RadialLocals *locals) {
  mimic_sage_h2_radial_locals_reset(locals);

  /* SAGE parity: h is the float Hubble_h (:138), rs_pc CODE_LENGTH_TO_PC's double
   * DiskScaleRadius * 1e6 / h narrowed to float (:139, R/src/macros.h:46) */
  const float h = (float)hubble_h;
  const float rs_pc = (float)(gal->DiskScaleRadius * 1.0e6 / h);
  if (locals != NULL) {
    locals->h = h;
    locals->rs_pc = rs_pc;
  }

  /* SAGE parity: the early return leaves H2gas as it was (:141-143); sfr_br06's cap that follows
   * is a separate step (R/src/model_starformation_and_feedback.c:239-242) */
  const int returned_early = rs_pc <= 0.0 || gal->ColdGas <= 0.0;
  if (locals != NULL) {
    locals->returned_early = returned_early;
  }
  if (returned_early) {
    return 0.0f;
  }

  /* Total masses in Msun, CODE_MASS_TO_MSUN left to right and narrowed; the disk stars subtract
   * the bulge in float first (:146-147, R/src/macros.h:45) */
  const float M_gas_total = (float)(gal->ColdGas * 1.0e10 / h);
  const float M_disk_star = (float)((gal->StellarMass - gal->BulgeMass) * 1.0e10 / h);

  /* Central surface densities of the exponential profiles, with the double denominator, narrowed;
   * no disk stars give zero stellar density (:150-151) */
  const float sigma_gas_0 = (float)(M_gas_total / (2.0 * M_PI * rs_pc * rs_pc));
  const int disk_stars_positive = M_disk_star > 0.0;
  const float sigma_star_0 =
      (float)(disk_stars_positive ? M_disk_star / (2.0 * M_PI * rs_pc * rs_pc) : 0.0);

  /* The radial grid (:154-156): the factor narrowed to float times rs_pc, divided by the bin
   * count converted to float */
  const int N_BINS = n_bins;
  const float R_MAX = (float)r_max_factor * rs_pc;
  const float dr = R_MAX / N_BINS;

  /* The stellar scale height depends only on rs_pc: once, before the loop (:177-178) */
  float log_h_star = NAN;
  const float h_star_br06 = mimic_sage_br06_stellar_scale_height(rs_pc, &log_h_star);

  if (locals != NULL) {
    locals->M_gas_total = M_gas_total;
    locals->M_disk_star = M_disk_star;
    locals->sigma_gas_0 = sigma_gas_0;
    locals->disk_stars_positive = disk_stars_positive;
    locals->sigma_star_0 = sigma_star_0;
    locals->R_MAX = R_MAX;
    locals->dr = dr;
    locals->log_h_star = log_h_star;
    locals->h_star_br06 = h_star_br06;
  }

  /* SAGE parity: a float accumulator, in increasing-bin order (:208-265); the truncated disk is
   * not renormalised for the gas beyond R_MAX */
  float M_H2_total = 0.0f;
  for (int i = 0; i < N_BINS; i++) {
    struct MimicSageH2RadialBin *b =
        (locals != NULL && i < MIMIC_SAGE_H2_RADIAL_NBINS) ? &locals->bin[i] : NULL;

    /* Bin centre and the exponential profiles, all float (:212-217) */
    const float r = (i + 0.5f) * dr;
    const float exp_factor = expf(-r / rs_pc);
    const float sigma_gas_r = sigma_gas_0 * exp_factor;
    const float sigma_star_r = sigma_star_0 * exp_factor;
    const int skipped = sigma_gas_r < 1e-3f;
    if (b != NULL) {
      b->r = r;
      b->exp_factor = exp_factor;
      b->sigma_gas_r = sigma_gas_r;
      b->sigma_star_r = sigma_star_r;
      b->skipped = skipped;
    }

    /* Bins with negligible gas are skipped (:220) */
    if (skipped) {
      continue;
    }

    struct MimicSageBr06Locals bin_locals;
    mimic_sage_br06_locals_reset(&bin_locals);
    const float f_mol_r = mimic_sage_br06_molecular_fraction_from_hstar(
        sigma_gas_r, sigma_star_r, h_star_br06, b != NULL ? &bin_locals : NULL);

    /* SAGE parity: the annulus mass is entirely float, left to right, with the float 0.74f
     * (:263), not the double HYDROGEN_MASS_FRAC */
    const float dM_H2 = 2.0f * (float)M_PI * r * sigma_gas_r * 0.74f * f_mol_r * dr;
    M_H2_total += dM_H2;
    if (b != NULL) {
      b->f_mol_r = f_mol_r;
      b->stellar_floor_binds = bin_locals.stellar_floor_binds;
      b->dM_H2 = dM_H2;
      b->M_H2_total = M_H2_total;
    }
  }

  /* SAGE parity: MSUN_TO_CODE_MASS (R/src/macros.h:47): M_H2_total * h is a float product, then
   * divided by the double 1e10 and narrowed (:276) */
  const float H2_code_units = (float)(M_H2_total * h / 1.0e10);

  if (locals != NULL) {
    locals->bins_recorded =
        N_BINS < MIMIC_SAGE_H2_RADIAL_NBINS ? N_BINS : MIMIC_SAGE_H2_RADIAL_NBINS;
    locals->M_H2_total = M_H2_total;
    locals->H2_code_units = H2_code_units;
  }

  /* Store and return (:288-289) */
  gal->H2gas = H2_code_units;
  return H2_code_units;
}

#endif /* MIMIC_SHARED_SAGE_H2_PHYSICS_H */
