/**
 * @file sage_cooling_physics.h
 * @brief SAGE26's cooling, radio-mode AGN heating and cooling transfer, shared by the cooling
 * modules
 *
 * Implements SAGE26's cooling_recipe_hot and cooling_recipe_cgm (Carr et al. 2023 bulk CGM
 * cooling), the hot regime's cold streams (Dekel & Birnboim 2006), interpolate_clustering_mass,
 * agn_accretion_compute (Bondi accretion, AGNrecipe 2), do_AGN_heating, do_AGN_heating_cgm and
 * cool_gas_onto_galaxy. Used by the CGMrecipeOn 0 path (sage26_calculate_cooling_budget ->
 * sage26_radio_mode_heating -> sage26_apply_cooling) and by the CGMrecipeOn 1 cooling transaction
 * (sage26_regime_aware_cooling); helpers serving both paths take CGMrecipeOn as an argument.
 *
 * The cooling helpers call get_metaldependent_cooling_rate from sage26_calculate_cooling_budget's
 * cooling_tables.c, so the tables must be loaded (sage26_calculate_cooling_budget_init) before they
 * run: unloaded tables return 0, which reads as lambda <= 0. That include is the one coupling of
 * this shared file to a module.
 *
 * No helper writes the galaxy's mdot_cool or mdot_stream: the float rates are returned in struct
 * MimicSageColdStreamLocals, and sage26_regime_aware_cooling stores them in mdot_cool_raw and
 * mdot_stream_raw, from which sage26_finalize_cooling_heating writes the output fields.
 *
 * SAGE parity: Mvir, Rvir, Vvir and the reservoirs are read as SAGE26's floats, and float-only
 * operations (Rvir / Vvir, the metallicities, HotGas / Rvir, Rvir * Rvir) stay float.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_COOLING_PHYSICS_H
#define MIMIC_SHARED_SAGE_COOLING_PHYSICS_H

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "constants.h"
#include "error.h"
#include "types.h"
#include "module_system/physical_constants.h"
#include "shared/metallicity.h"
#include "shared/sage_agn_recipe.h"
#include "shared/sage_constants.h"
#include "shared/sage_exp10.h"
#include "shared/sage_fire_feedback_physics.h"
#include "modules/sage26_calculate_cooling_budget/cooling_tables.h"

/* Mean molecular weight of a fully ionised gas */
#define MIMIC_SAGE_MU_IONISED 0.59
/* Dekel & Birnboim (2006) stream width factor f and logistic width in dex */
#define MIMIC_SAGE_DB06_STREAM_MASS_FACTOR 3.0
#define MIMIC_SAGE_STREAM_THRESHOLD_WIDTH_DEX 0.15
/* The fraction of rcool at which a CGMrecipeOn 1 heating radius saturates */
#define MIMIC_SAGE_RHEAT_SATURATION 0.99

/* Which cooling-mass branch mimic_sage_hot_cooling_budget took */
#define MIMIC_SAGE_HOT_BRANCH_NONE -1   /* an early return: no cooling mass formed */
#define MIMIC_SAGE_HOT_BRANCH_STATIC 0  /* CGMrecipeOn 0, rcool <= Rvir */
#define MIMIC_SAGE_HOT_BRANCH_RAPID 1   /* CGMrecipeOn 0, rcool > Rvir */
#define MIMIC_SAGE_HOT_BRANCH_STREAMS 2 /* CGMrecipeOn 1, the cold streams */

/* How a reservoir cap of the cooling mass ended */
#define MIMIC_SAGE_CAP_NOT_REACHED -1
#define MIMIC_SAGE_CAP_NONE 0
#define MIMIC_SAGE_CAP_RESERVOIR 1 /* capped to the reservoir */
#define MIMIC_SAGE_CAP_FLOOR 2     /* floored at zero */

// ============================================================================
// THE CLUSTERING-MASS TABLE
// ============================================================================

/** @brief interpolate_clustering_mass's locals, for tests; NULL when not wanted */
struct MimicSageClusteringLocals {
  int uchuu; /**< 1 when the Uchuu table was selected */
  double zz; /**< the redshift after the clamps to [0, 15] */
  int iz;    /**< the lower grid index after its clamps to [0, 29] */
  double fz; /**< the fractional position */
};

/**
 * @brief SAGE26's interpolate_clustering_mass: log10 M_*(z) in log10 Msun
 *
 * The Press-Schechter clustering mass in log10 Msun (not Msun/h), linearly interpolated in a
 * 31-entry table: Uchuu (Planck15) for fabs(Omega - 0.3089) < 0.01, Millennium (WMAP1) otherwise.
 * Both tables are SAGE26's literal arrays, never recomputed.
 *
 * SAGE parity: the interpolation is the fma(table[iz], 1 - fz, table[iz + 1] * fz) SAGE26
 * computes.
 *
 * @param z       Redshift
 * @param omega   The run's Omega (selects the table)
 * @param locals  Optional locals (may be NULL)
 * @return log10 of the clustering mass in Msun
 */
static inline double
mimic_sage_interpolate_clustering_mass(const double z, const double omega,
                                       struct MimicSageClusteringLocals *locals) {
  static const double mstar_table_z[31] = {
      0.0, 0.5, 1.0, 1.5, 2.0,  2.5,  3.0,  3.5,  4.0,  4.5,  5.0,  5.5,  6.0,  6.5,  7.0, 7.5,
      8.0, 8.5, 9.0, 9.5, 10.0, 10.5, 11.0, 11.5, 12.0, 12.5, 13.0, 13.5, 14.0, 14.5, 15.0};
  /* Millennium / WMAP1 */
  static const double mstar_mill[31] = {
      12.8964, 12.2501, 11.5685, 10.9046, 10.2717, 9.6699, 9.0969, 8.5486, 8.0214, 7.5126, 7.0206,
      6.5435,  6.0794,  5.6266,  5.1842,  4.7514,  4.3276, 3.9122, 3.5045, 3.1039, 2.7098, 2.3217,
      1.9393,  1.5626,  1.1912,  1.0000,  1.0000,  1.0000, 1.0000, 1.0000, 1.0000};
  /* Uchuu / Planck15 */
  static const double mstar_uchuu[31] = {
      12.8028, 12.0672, 11.3101, 10.5795, 9.8831, 9.2194, 8.5834, 7.9701, 7.3764, 6.8002, 6.2389,
      5.6901,  5.1521,  4.6241,  4.1054,  3.5950, 3.0915, 2.5943, 2.1027, 1.6166, 1.1359, 1.0000,
      1.0000,  1.0000,  1.0000,  1.0000,  1.0000, 1.0000, 1.0000, 1.0000, 1.0000};

  const double *table = mstar_mill;
  int uchuu = 0;
  if (fabs(omega - 0.3089) < 0.01) {
    table = mstar_uchuu;
    uchuu = 1;
  }

  double zz = z;
  if (zz < mstar_table_z[0])
    zz = mstar_table_z[0];
  if (zz > mstar_table_z[31 - 1])
    zz = mstar_table_z[31 - 1];

  int iz = (int)(zz / 0.5);
  if (iz < 0)
    iz = 0;
  if (iz >= 31 - 1)
    iz = 31 - 2;

  const double fz = (zz - mstar_table_z[iz]) / (mstar_table_z[iz + 1] - mstar_table_z[iz]);

  if (locals != NULL) {
    locals->uchuu = uchuu;
    locals->zz = zz;
    locals->iz = iz;
    locals->fz = fz;
  }
  return fma(table[iz], 1.0 - fz, table[iz + 1] * fz);
}

// ============================================================================
// THE HOT RECIPE UP TO THE AGN STEP
// ============================================================================

/** @brief The cold-stream locals of one CGMrecipeOn 1 hot call */
struct MimicSageColdStreamLocals {
  double z;                   /**< ZZ[stored SnapNum] */
  double M_shock;             /**< MShockMsun * Hubble_h / 1e10 */
  double mass_ratio;          /**< Mvir / M_shock */
  double log_mstar;           /**< interpolate_clustering_mass(z) */
  double Mstar;               /**< 10^log_mstar * Hubble_h / 1e10 */
  double fMstar;              /**< 3 * Mstar */
  double ratio;               /**< (fMstar / Mvir)^(2/3) * mass_ratio^(4/3) */
  int ratio_nonpositive;      /**< 1 when ratio <= 0 */
  double exponent;            /**< log10(ratio) / 0.15 after its clamps; NaN if unused */
  int exponent_capped_high;   /**< 1 when the 300 clamp bound */
  int exponent_capped_low;    /**< 1 when the -300 clamp bound */
  double f_stream_criterion;  /**< f_stream before the cutoff */
  int cutoff;                 /**< 1 when fMstar >= M_shock && mass_ratio > 1 */
  double f_stream;            /**< after the cutoff and the clamps */
  double cold_stream_cooling; /**< f_stream * HotGas / tcool * dt */
  double hot_halo_cooling;    /**< (1 - f_stream) * float(HotGas / Rvir) * ... */
  float mdot_cool;            /**< float(hot_halo_cooling / dt), before the cap and AGN */
  float mdot_stream;          /**< float(cold_stream_cooling / dt) */
};

/**
 * @brief SAGE26's hot-regime cold streams: the cooling mass before the cap
 *
 * Dekel & Birnboim (2006) cold streams: the stream fraction is a logistic function of the halo mass
 * relative to the shock mass and the clustering mass, switched off above the shock mass once
 * f M_* reaches it. Reads the redshift of the galaxy's stored snapshot, ZZ[stored_snapnum], never
 * the current snapshot's; a stored SnapNum outside the run's redshift table is refused with an
 * error naming it, where SAGE26 reads out of bounds. HotGas, Rvir and Mvir are SAGE26's floats.
 * The float mdot rates are returned in s->mdot_cool and s->mdot_stream; no galaxy field is written.
 *
 * @param params          Run configuration (ZZ, Snaplistlen, Omega, Hubble_h)
 * @param stored_snapnum  The galaxy's stored SnapNum
 * @param mshock_msun     MShockMsun, the shock mass in physical Msun
 * @param mvir            SAGE26's float Mvir
 * @param hot_gas         HotGas
 * @param rvir            SAGE26's float Rvir
 * @param tcool           The hot recipe's tcool, the float Rvir / Vvir widened
 * @param rcool           The uncapped cooling radius
 * @param dt              Substep interval
 * @param s               Output: the stream locals
 * @param cooling_out     Output: cold_stream_cooling + hot_halo_cooling
 * @return 0, or -1 when the stored SnapNum has no redshift
 */
static inline int mimic_sage_cold_stream_cooling(
    const struct MimicConfig *params, const int stored_snapnum, const double mshock_msun,
    const float mvir, const float hot_gas, const float rvir, const double tcool, const double rcool,
    const double dt, struct MimicSageColdStreamLocals *s, double *cooling_out) {
  double z;
  if (mimic_sage_fire_stored_redshift(params, stored_snapnum, &z) != 0) {
    ERROR_LOG("Cold streams: SAGE26 reads ZZ[SnapNum] of the galaxy's stored snapshot; "
              "stored SnapNum = %d has no redshift",
              stored_snapnum);
    return -1;
  }
  s->z = z;

  /* The shock mass in code units, 1e10 Msun/h */
  const double M_shock = mshock_msun * params->Hubble_h / 1.0e10;
  const double mass_ratio = mvir / M_shock;

  /* The clustering mass in code units and the stream criterion's mass ratio */
  const double log_mstar = mimic_sage_interpolate_clustering_mass(z, params->Omega, NULL);
  const double Mstar = mimic_sage_exp10(log_mstar) * params->Hubble_h / 1.0e10;
  const double fMstar = MIMIC_SAGE_DB06_STREAM_MASS_FACTOR * Mstar;
  const double ratio = pow(fMstar / mvir, 2.0 / 3.0) * pow(mass_ratio, 4.0 / 3.0);

  s->M_shock = M_shock;
  s->mass_ratio = mass_ratio;
  s->log_mstar = log_mstar;
  s->Mstar = Mstar;
  s->fMstar = fMstar;
  s->ratio = ratio;
  s->exponent = NAN;
  s->exponent_capped_high = 0;
  s->exponent_capped_low = 0;

  double f_stream;
  if (ratio <= 0.0) {
    s->ratio_nonpositive = 1;
    f_stream = 1.0;
  } else {
    s->ratio_nonpositive = 0;
    double exponent = log10(ratio) / MIMIC_SAGE_STREAM_THRESHOLD_WIDTH_DEX;
    // SAGE parity: the exponent is clamped above 300 and then below -300 before 10^exponent
    if (exponent > 300.0) {
      exponent = 300.0;
      s->exponent_capped_high = 1;
    }
    if (exponent < -300.0) {
      exponent = -300.0;
      s->exponent_capped_low = 1;
    }
    s->exponent = exponent;
    f_stream = 1.0 / (1.0 + mimic_sage_exp10(exponent));
  }
  s->f_stream_criterion = f_stream;

  // SAGE parity: streams are switched off for a halo above the shock mass once f M_* reaches it,
  // a step at z_crit SAGE26 keeps
  s->cutoff = 0;
  if (fMstar >= M_shock && mass_ratio > 1.0) {
    f_stream = 0.0;
    s->cutoff = 1;
  }

  if (f_stream > 1.0)
    f_stream = 1.0;
  if (f_stream < 0.0)
    f_stream = 0.0;
  s->f_stream = f_stream;

  /* HotGas / Rvir is a float division */
  const double cold_stream_cooling = f_stream * hot_gas / tcool * dt;
  const double hot_halo_cooling =
      (1.0 - f_stream) * (hot_gas / rvir) * (rcool / (2.0 * tcool)) * dt;

  // SAGE parity: the mdot rates are stored as floats before the reservoir cap and the AGN
  // suppression, so they are not the applied rates
  s->mdot_cool = (float)(hot_halo_cooling / dt);
  s->mdot_stream = (float)(cold_stream_cooling / dt);
  s->cold_stream_cooling = cold_stream_cooling;
  s->hot_halo_cooling = hot_halo_cooling;

  *cooling_out = cold_stream_cooling + hot_halo_cooling;
  return 0;
}

/** @brief SAGE26's locals of cooling_recipe_hot up to the AGN step */
struct MimicSageHotCoolingLocals {
  int path;              /**< SAGE_COOLING_PATH_* */
  double tcool;          /**< float Rvir / Vvir widened; 0 on the outer guard */
  double lambda;         /**< the cooling function; NaN when not formed */
  double x;              /**< PROTONMASS * BOLTZMANN * temp / lambda in code units */
  double rcool;          /**< the uncapped cooling radius */
  int branch;            /**< MIMIC_SAGE_HOT_BRANCH_* */
  double cooling_precap; /**< the branch's cooling mass before the cap; 0 when not formed */
  int cap;               /**< MIMIC_SAGE_CAP_* */
  struct MimicSageColdStreamLocals stream; /**< valid only for MIMIC_SAGE_HOT_BRANCH_STREAMS */
};

/**
 * @brief SAGE26's cooling_recipe_hot up to the AGN step: the post-cap cooling mass
 *
 * Writes tcool, tff and RcoolToRvir as SAGE26 does, including its early returns, which leave
 * RcoolToRvir untouched. Mvir, Rvir and Vvir are read as SAGE26's floats. With CGMrecipeOn 0 the
 * cooling mass is the rapid or static branch of an isothermal hot halo; with 1 it is the cold
 * streams, which read the galaxy's stored SnapNum (halo->SnapNum) and can refuse it. The streams'
 * float mdot rates are returned in l->stream, never written to the galaxy. On the -1 return
 * l->path is SAGE_COOLING_PATH_NOT_EVALUATED and the stream locals are zero (the refusal is the
 * streams' first step).
 *
 * @param halo           Halo whose galaxy cools (galaxy not NULL)
 * @param params         Run configuration
 * @param dt             Substep interval, SAGE26's deltaT / effective_steps
 * @param cgm_recipe_on  CGMrecipeOn (0 or 1)
 * @param mshock_msun    MShockMsun (read only with CGMrecipeOn 1)
 * @param l              Output: SAGE26's locals and the return path
 * @param cooling_out    Output: the post-cap, pre-suppression cooling mass (0 on early returns)
 * @return 0, or -1 when the streams refuse the stored SnapNum
 */
static inline int mimic_sage_hot_cooling_budget(struct Halo *halo, const struct MimicConfig *params,
                                                const double dt, const int cgm_recipe_on,
                                                const double mshock_msun,
                                                struct MimicSageHotCoolingLocals *l,
                                                double *cooling_out) {
  struct GalaxyData *galaxy = halo->galaxy;
  const float rvir = (float)halo->Rvir;
  const float vvir = (float)halo->Vvir;
  double coolingGas;

  l->tcool = 0.0;
  l->lambda = NAN;
  l->x = 0.0;
  l->rcool = 0.0;
  l->branch = MIMIC_SAGE_HOT_BRANCH_NONE;
  l->cooling_precap = 0.0;
  l->cap = MIMIC_SAGE_CAP_NOT_REACHED;
  memset(&l->stream, 0, sizeof(l->stream));
  *cooling_out = 0.0;

  galaxy->tcool = 0.0f;

  if (!(galaxy->HotGas > 0.0 && vvir > 0.0)) {
    l->path = SAGE_COOLING_PATH_OUTER_GUARD;
    return 0;
  }

  // SAGE parity: tcool is the float Rvir / Vvir stored in a double, used by every cooling branch
  const double tcool = rvir / vvir;
  const double tff = M_SQRT2 * tcool;
  l->tcool = tcool;

  // SAGE parity: tcool and tff are written with SAGE26's conversion, a mass-rate
  // factor applied to a time, so neither is a time in Gyr
  galaxy->tcool =
      (float)((tcool * 1.0e10 / params->Hubble_h * SAGE_SEC_PER_GIGAYEAR / params->UnitTime_in_s));
  galaxy->tff =
      (float)((tff * 1.0e10 / params->Hubble_h * SAGE_SEC_PER_GIGAYEAR / params->UnitTime_in_s));

  const double temp = SAGE_TVIR_K_PER_SQKMS * vvir * vvir; // T_vir in Kelvin

  // The float MetalsHotGas / HotGas is a float division, as in SAGE26
  double logZ = -10.0;
  if (galaxy->MetalsHotGas > 0) {
    logZ = log10(galaxy->MetalsHotGas / galaxy->HotGas);
  }

  const double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);
  l->lambda = lambda;

  // SAGE parity: the lambda <= 0 and rho_rcool <= 0 early returns skip the rest of the routine,
  // so RcoolToRvir, Cooling and CoolingRate keep their previous values (with CGMrecipeOn 1 the
  // regime-aware wrapper overwrites CoolingRate)
  if (lambda <= 0.0) {
    l->path = SAGE_COOLING_PATH_LAMBDA;
    return 0;
  }

  double x = PROTONMASS * BOLTZMANN * temp / lambda;                  // sec g/cm^3
  x /= (params->UnitDensity_in_cgs * params->UnitTime_in_s);          // internal units
  const double rho_rcool = x / tcool * (1.5 * MIMIC_SAGE_MU_IONISED); // 3/2 mu, ionised gas
  l->x = x;

  if (rho_rcool <= 0.0) {
    l->path = SAGE_COOLING_PATH_RHO_RCOOL;
    return 0;
  }

  // An isothermal density profile for the hot gas
  const double rho0 = galaxy->HotGas / (4 * M_PI * rvir);
  const double rcool = sqrt(rho0 / rho_rcool);
  l->rcool = rcool;

  galaxy->RcoolToRvir = rcool / rvir;

  if (cgm_recipe_on == 0) {
    // SAGE parity: rcool is uncapped, and the rapid and static branches are discontinuous by a
    // factor 2 at rcool == Rvir, as SAGE26 keeps them
    if (rcool > rvir) {
      coolingGas = galaxy->HotGas / tcool * dt;
      l->branch = MIMIC_SAGE_HOT_BRANCH_RAPID;
    } else {
      // HotGas / Rvir is a float division, as in SAGE26
      coolingGas = (galaxy->HotGas / rvir) * (rcool / (2.0 * tcool)) * dt;
      l->branch = MIMIC_SAGE_HOT_BRANCH_STATIC;
    }
  } else {
    l->path = SAGE_COOLING_PATH_NOT_EVALUATED; /* the value a stored-SnapNum refusal leaves */
    if (mimic_sage_cold_stream_cooling(params, halo->SnapNum, mshock_msun, (float)halo->Mvir,
                                       galaxy->HotGas, rvir, tcool, rcool, dt, &l->stream,
                                       &coolingGas) != 0) {
      return -1;
    }
    l->branch = MIMIC_SAGE_HOT_BRANCH_STREAMS;
  }
  l->cooling_precap = coolingGas;

  if (coolingGas > galaxy->HotGas) {
    coolingGas = galaxy->HotGas;
    l->cap = MIMIC_SAGE_CAP_RESERVOIR;
  } else {
    if (coolingGas < 0.0) {
      coolingGas = 0.0;
      l->cap = MIMIC_SAGE_CAP_FLOOR;
    } else {
      l->cap = MIMIC_SAGE_CAP_NONE;
    }
  }

  l->path = SAGE_COOLING_PATH_TAIL;
  *cooling_out = coolingGas;
  return 0;
}

// ============================================================================
// BULK CGM COOLING UP TO THE AGN STEP
// ============================================================================

/** @brief SAGE26's locals of cooling_recipe_cgm up to the AGN step */
struct MimicSageCgmCoolingLocals {
  int entered;           /**< CGMgas > 0 && Vvir > 0 */
  int lambda_positive;   /**< lambda > 0; -1 when not entered */
  double lambda;         /**< the cooling function; NaN when not formed */
  double x;              /**< as the hot recipe's; NaN when not formed */
  double rho0;           /**< CGMgas / (4 pi r0^3 I_M); NaN when not formed */
  double rho_eff;        /**< rho0 * (I_cool / I_M); NaN when not formed */
  double tcool;          /**< (x / rho_eff) * (1.5 * 0.59); NaN when not formed */
  double g_accel;        /**< G * Mvir / float(Rvir * Rvir); NaN when not formed */
  double tff;            /**< sqrt(2 Rvir / g_accel); NaN when not formed */
  double rcool;          /**< Rvir; NaN when not formed */
  double cooling_precap; /**< (CGMgas / (tcool + tff)) * dt; NaN when not formed */
  int cap;               /**< MIMIC_SAGE_CAP_* */
};

/**
 * @brief SAGE26's cooling_recipe_cgm up to the AGN step: the post-cap CGM cooling mass
 *
 * Carr et al. (2023) bulk cooling of the CGM pair: logZ = -10 unless MetalsCGMgas > 0, the cooling
 * function, the lambda > 0 guard, a power-law profile with alpha 1.4 and r0 = 0.1 Rvir, the volume
 * integrals, rho0, rho_eff, tcool and tff, each written to its float diagnostic with its own
 * conversion, rcool = Rvir and the cap to CGMgas and floor at zero. No positive Mvir or Rvir guard
 * is added where SAGE26 has none, and RcoolToRvir is not written. A positive CGMgas with a failed
 * Vvir or lambda guard writes nothing.
 *
 * @param halo    Halo whose galaxy cools (galaxy not NULL)
 * @param params  Run configuration (G, UnitDensity_in_cgs, UnitTime_in_s)
 * @param dt      Substep interval
 * @param l       Output: SAGE26's locals
 * @return The post-cap, pre-suppression CGM cooling mass (0 when not formed)
 */
static inline double mimic_sage_cgm_cooling_budget(struct Halo *halo,
                                                   const struct MimicConfig *params,
                                                   const double dt,
                                                   struct MimicSageCgmCoolingLocals *l) {
  struct GalaxyData *galaxy = halo->galaxy;
  const float mvir = (float)halo->Mvir;
  const float rvir = (float)halo->Rvir;
  const float vvir = (float)halo->Vvir;
  double coolingGas = 0.0;

  l->entered = 0;
  l->lambda_positive = -1;
  l->lambda = NAN;
  l->x = NAN;
  l->rho0 = NAN;
  l->rho_eff = NAN;
  l->tcool = NAN;
  l->g_accel = NAN;
  l->tff = NAN;
  l->rcool = NAN;
  l->cooling_precap = NAN;
  l->cap = MIMIC_SAGE_CAP_NOT_REACHED;

  if (galaxy->CGMgas > 0.0 && vvir > 0.0) {
    l->entered = 1;
    const double temp = SAGE_TVIR_K_PER_SQKMS * vvir * vvir; // T_vir in Kelvin

    // The float MetalsCGMgas / CGMgas is a float division, as in SAGE26
    double logZ = -10.0;
    if (galaxy->MetalsCGMgas > 0) {
      logZ = log10(galaxy->MetalsCGMgas / galaxy->CGMgas);
    }

    const double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);
    l->lambda = lambda;
    l->lambda_positive = (lambda > 0.0) ? 1 : 0;

    if (lambda > 0.0) {
      double x = PROTONMASS * BOLTZMANN * temp / lambda;
      x /= (params->UnitDensity_in_cgs * params->UnitTime_in_s);

      // Carr et al. (2023) profile constants: alpha, r0 and Rvir / r0
      const double alpha = 1.4;
      const double r0 = 0.1 * rvir;
      const double ratio = 10.0;
      const double I_M = (pow(ratio, 3.0 - alpha) - 1.0) / (3.0 - alpha);
      const double I_cool = (pow(ratio, 3.0 - 2.0 * alpha) - 1.0) / (3.0 - 2.0 * alpha);

      const double rho0 = galaxy->CGMgas / (4.0 * M_PI * r0 * r0 * r0 * I_M);
      const double rho_eff = rho0 * (I_cool / I_M);
      const double tcool = (x / rho_eff) * (1.5 * MIMIC_SAGE_MU_IONISED);

      // Rvir * Rvir is a float product
      const double g_accel = params->G * mvir / (rvir * rvir);
      const double tff = sqrt(2.0 * rvir / g_accel);

      // SAGE parity: the CGM diagnostics are tcool and tff in Gyr, a different conversion from
      // the hot recipe's
      galaxy->tcool = (float)(tcool * params->UnitTime_in_s / SAGE_SEC_PER_GIGAYEAR);
      galaxy->tff = (float)(tff * params->UnitTime_in_s / SAGE_SEC_PER_GIGAYEAR);

      // SAGE parity: rcool is pinned to Rvir for the AGN call, and RcoolToRvir is not written
      const double rcool = rvir;

      coolingGas = (galaxy->CGMgas / (tcool + tff)) * dt;
      l->cooling_precap = coolingGas;

      if (coolingGas > galaxy->CGMgas) {
        coolingGas = galaxy->CGMgas;
        l->cap = MIMIC_SAGE_CAP_RESERVOIR;
      } else if (coolingGas < 0.0) {
        coolingGas = 0.0;
        l->cap = MIMIC_SAGE_CAP_FLOOR;
      } else {
        l->cap = MIMIC_SAGE_CAP_NONE;
      }

      l->x = x;
      l->rho0 = rho0;
      l->rho_eff = rho_eff;
      l->tcool = tcool;
      l->g_accel = g_accel;
      l->tff = tff;
      l->rcool = rcool;
    }
  }
  return coolingGas;
}

// ============================================================================
// RADIO-MODE AGN ACCRETION
// ============================================================================

/**
 * @brief Bondi-Hoyle accretion recipe (Bondi 1952), AGNrecipe 2
 *
 * SAGE26's Bondi branch of agn_accretion_compute, with x carried from the cooling budget rather
 * than recomputed from the cooling function.
 */
static inline double mimic_sage_agn_rate_bondi(const double black_hole_mass, const double x,
                                               const double efficiency,
                                               const struct MimicConfig *run_params) {
  return (2.5 * M_PI * run_params->G) * (0.375 * 0.6 * x) * black_hole_mass * efficiency;
}

/**
 * @brief SAGE26's agn_accretion_compute: accreted mass and heating mass
 *
 * The Bondi accretion rate, the Eddington cap, the reservoir cap, the heating
 * coefficient and the cap of heating to the suppressed cooling mass, in SAGE26's
 * order and with its Vvir > 0 and AGNcoeff > 0 guards. Reads no field it writes. The heating
 * coefficient uses SAGE26's float Vvir. Only the Bondi branch is implemented: its readers refuse
 * every AGNrecipe but 0 and 2 (shared/sage_agn_recipe.h), and none calls this with 0.
 *
 * @param halo            Halo whose galaxy accretes (galaxy not NULL)
 * @param agn_recipe      AGNrecipe (only 2 reaches here and forms the Bondi rate; no rate is
 *                        formed for any other value, where SAGE26 forms the empirical rate for
 *                        anything but 2 and 3)
 * @param efficiency      RadioModeEfficiency
 * @param dt              Substep interval
 * @param x               SAGE26's x of the calling recipe
 * @param coolingGas      The suppressed cooling mass
 * @param reservoir_mass  The selected reservoir: HotGas (hot call) or CGMgas (CGM call)
 * @param params          Run configuration
 * @param accreted_out    Output: the accreted mass
 * @param heating_out     Output: the heating mass
 */
static inline void mimic_sage_agn_accretion_compute(const struct Halo *halo, const int agn_recipe,
                                                    const double efficiency, const double dt,
                                                    const double x, const double coolingGas,
                                                    const double reservoir_mass,
                                                    const struct MimicConfig *params,
                                                    double *accreted_out, double *heating_out) {
  const double black_hole_mass = halo->galaxy->BlackHoleMass;
  const double vvir = (float)halo->Vvir;

  double AGNrate = 0.0;
  if (agn_recipe == MIMIC_SAGE_AGN_RECIPE_BONDI) {
    AGNrate = mimic_sage_agn_rate_bondi(black_hole_mass, x, efficiency, params);
  }

  // Eddington-limited accretion: L_edd = 1.3e38 erg/s per Msun, radiative
  // efficiency 0.1, c^2 = 9e10 (km/s)^2
  const double EDDrate = (1.3e38 * black_hole_mass * 1e10 / params->Hubble_h) /
                         (params->UnitEnergy_in_cgs / params->UnitTime_in_s) / (0.1 * 9.0e10);
  if (AGNrate > EDDrate)
    AGNrate = EDDrate;

  double AGNaccreted = AGNrate * dt;
  if (AGNaccreted > reservoir_mass)
    AGNaccreted = reservoir_mass;

  // Heating coefficient 1.34e5 km/s = sqrt(2 eta c^2)
  double AGNheating = 0.0;
  if (vvir > 0.0) {
    const double AGNcoeff = (1.34e5 / vvir) * (1.34e5 / vvir);
    AGNheating = AGNcoeff * AGNaccreted;
    // Limit heating to the available cooling mass
    if (AGNheating > coolingGas && AGNcoeff > 0.0) {
      AGNaccreted = coolingGas / AGNcoeff;
      AGNheating = coolingGas;
    }
  }

  *accreted_out = AGNaccreted;
  *heating_out = AGNheating;
}

// ============================================================================
// RADIO-MODE AGN HEATING
// ============================================================================

/** @brief The locals of one do_AGN_heating or do_AGN_heating_cgm call, for tests */
struct MimicSageAgnLocals {
  int suppressed; /**< 1 when the radius suppression formula ran, 0 when zeroed */
  int saturated;  /**< 1 when the CGMrecipeOn 1 saturation zeroed it (Rheat >= 0.99 rcool) */
  double coolingGas_suppressed; /**< after the suppression */
  int reservoir_positive;       /**< the selected reservoir was positive */
  double accreted;              /**< agn_accretion_compute's AGNaccreted; 0 when not formed */
  double heating;               /**< its AGNheating; 0 when not formed */
  int ratchet_raised;           /**< 1 when the ratchet raised Rheat */
  int rheat_capped;             /**< 1 when Rheat was capped to Rvir */
  int recapped;                 /**< CGM call: 1 when the returned mass was re-capped */
};

/** @brief Whether SAGE26's heating-radius suppression applies */
static inline int mimic_sage_agn_suppression_applies(const float rheat, const double rcool,
                                                     const int cgm_recipe_on, int *saturated) {
  // SAGE parity: the cooling mass is suppressed only by the heating radius from earlier events;
  // Rheat >= rcool suppresses it completely, and with CGMrecipeOn 1 so does Rheat >= 0.99 * rcool
  if (rheat < rcool && !(cgm_recipe_on == 1 && rheat >= MIMIC_SAGE_RHEAT_SATURATION * rcool)) {
    *saturated = 0;
    return 1;
  }
  *saturated = (rheat < rcool) ? 1 : 0;
  return 0;
}

/**
 * @brief SAGE26's do_AGN_heating: suppress the cooling mass, grow the BH from the hot pair
 *
 * The heating-radius suppression, the accretion from the hot pair, the radius ratchet and the
 * Heating energy. Vvir and Rvir are read as SAGE26's floats.
 *
 * @param halo           Halo whose galaxy is heated (galaxy not NULL)
 * @param params         Run configuration
 * @param agn_recipe     AGNrecipe
 * @param efficiency     RadioModeEfficiency
 * @param cgm_recipe_on  CGMrecipeOn (0 or 1)
 * @param dt             Substep interval, SAGE26's deltaT / effective_steps
 * @param coolingGas     Post-cap cooling mass of the hot recipe (positive)
 * @param x              SAGE26's x of the hot recipe
 * @param rcool          SAGE26's rcool of the hot recipe
 * @param l              Optional locals (may be NULL)
 * @return The radius-suppressed cooling mass
 */
static inline double
mimic_sage_do_agn_heating_hot(struct Halo *halo, const struct MimicConfig *params,
                              const int agn_recipe, const double efficiency,
                              const int cgm_recipe_on, const double dt, double coolingGas,
                              const double x, const double rcool, struct MimicSageAgnLocals *l) {
  struct GalaxyData *galaxy = halo->galaxy;
  const float vvir = (float)halo->Vvir;
  const float rvir = (float)halo->Rvir;
  struct MimicSageAgnLocals local;
  if (l == NULL)
    l = &local;

  l->accreted = 0.0;
  l->heating = 0.0;
  l->ratchet_raised = 0;
  l->rheat_capped = 0;
  l->recapped = 0;

  l->suppressed =
      mimic_sage_agn_suppression_applies(galaxy->Rheat, rcool, cgm_recipe_on, &l->saturated);
  if (l->suppressed) {
    coolingGas = (1.0 - galaxy->Rheat / rcool) * coolingGas;
  } else {
    coolingGas = 0.0;
  }
  l->coolingGas_suppressed = coolingGas;

  l->reservoir_positive = (galaxy->HotGas > 0.0) ? 1 : 0;
  if (galaxy->HotGas > 0.0) {
    double AGNaccreted, AGNheating;
    mimic_sage_agn_accretion_compute(halo, agn_recipe, efficiency, dt, x, coolingGas,
                                     galaxy->HotGas, params, &AGNaccreted, &AGNheating);
    l->accreted = AGNaccreted;
    l->heating = AGNheating;

    // Metallicity of the hot gas before the accretion is drawn from it
    const double metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
    galaxy->BlackHoleMass += AGNaccreted;
    galaxy->HotGas -= AGNaccreted;
    galaxy->MetalsHotGas -= metallicity * AGNaccreted;

    // The heating radius ratchet: it only grows
    if (galaxy->Rheat < rcool && coolingGas > 0.0) {
      const double rheat_new = (AGNheating / coolingGas) * rcool;
      if (rheat_new > galaxy->Rheat) {
        galaxy->Rheat = rheat_new;
        l->ratchet_raised = 1;
      }
    }

    // Heating energy, accumulated undivided; sage26_finalize_cooling_heating
    // normalises it once per snapshot
    if (AGNheating > 0.0)
      galaxy->Heating += 0.5 * AGNheating * vvir * vvir;
  }

  // SAGE parity: with CGMrecipeOn 1 the hot call caps Rheat to Rvir outside its positive-reservoir
  // block, so a galaxy without hot gas is capped too
  if (cgm_recipe_on > 0 && galaxy->Rheat > rvir) {
    galaxy->Rheat = rvir;
    l->rheat_capped = 1;
  }

  // SAGE parity: the AGN heating is not subtracted from the cooling mass a second
  // time; the returned mass is the radius-suppressed one
  return coolingGas;
}

/**
 * @brief SAGE26's static do_AGN_heating_cgm: suppress, grow the BH from the CGM pair, re-cap
 *
 * The suppression as the hot call's, the accretion from CGMgas and its metals, the ratchet, the
 * cap of Rheat to Rvir inside the positive-reservoir block, the Heating energy and the re-cap of
 * the returned mass to the post-accretion CGMgas.
 *
 * @param halo           Halo whose galaxy is heated (galaxy not NULL)
 * @param params         Run configuration
 * @param agn_recipe     AGNrecipe
 * @param efficiency     RadioModeEfficiency
 * @param cgm_recipe_on  CGMrecipeOn (1 on every SAGE26 path that calls it)
 * @param dt             Substep interval
 * @param coolingGas     Post-cap CGM cooling mass (positive)
 * @param x              SAGE26's x of the CGM recipe
 * @param rcool          The CGM recipe's rcool (Rvir)
 * @param l              Optional locals (may be NULL)
 * @return The suppressed, re-capped cooling mass
 */
static inline double
mimic_sage_do_agn_heating_cgm(struct Halo *halo, const struct MimicConfig *params,
                              const int agn_recipe, const double efficiency,
                              const int cgm_recipe_on, const double dt, double coolingGas,
                              const double x, const double rcool, struct MimicSageAgnLocals *l) {
  struct GalaxyData *galaxy = halo->galaxy;
  const float vvir = (float)halo->Vvir;
  const float rvir = (float)halo->Rvir;
  struct MimicSageAgnLocals local;
  if (l == NULL)
    l = &local;

  l->accreted = 0.0;
  l->heating = 0.0;
  l->ratchet_raised = 0;
  l->rheat_capped = 0;
  l->recapped = 0;

  l->suppressed =
      mimic_sage_agn_suppression_applies(galaxy->Rheat, rcool, cgm_recipe_on, &l->saturated);
  if (l->suppressed) {
    coolingGas = (1.0 - galaxy->Rheat / rcool) * coolingGas;
  } else {
    coolingGas = 0.0;
  }
  l->coolingGas_suppressed = coolingGas;

  l->reservoir_positive = (galaxy->CGMgas > 0.0) ? 1 : 0;
  if (galaxy->CGMgas > 0.0) {
    double AGNaccreted, AGNheating;
    mimic_sage_agn_accretion_compute(halo, agn_recipe, efficiency, dt, x, coolingGas,
                                     galaxy->CGMgas, params, &AGNaccreted, &AGNheating);
    l->accreted = AGNaccreted;
    l->heating = AGNheating;

    /* The CGM pair's metallicity before the accretion is drawn from it */
    const double metallicity = mimic_get_metallicity(galaxy->CGMgas, galaxy->MetalsCGMgas);
    galaxy->BlackHoleMass += AGNaccreted;
    galaxy->CGMgas -= AGNaccreted;
    galaxy->MetalsCGMgas -= metallicity * AGNaccreted;

    /* The heating radius ratchet: it only grows */
    if (galaxy->Rheat < rcool && coolingGas > 0.0) {
      const double rheat_new = (AGNheating / coolingGas) * rcool;
      if (rheat_new > galaxy->Rheat) {
        galaxy->Rheat = rheat_new;
        l->ratchet_raised = 1;
      }
    }
    // SAGE parity: the CGM call caps Rheat to Rvir inside its positive-reservoir block, with no
    // CGMrecipeOn test
    if (galaxy->Rheat > rvir) {
      galaxy->Rheat = rvir;
      l->rheat_capped = 1;
    }

    /* Heating energy, accumulated undivided */
    if (AGNheating > 0.0)
      galaxy->Heating += 0.5 * AGNheating * vvir * vvir;

    // SAGE parity: the returned mass is re-capped to the CGM left after the accretion, and the
    // heating is not subtracted from it
    if (coolingGas > galaxy->CGMgas) {
      coolingGas = galaxy->CGMgas;
      l->recapped = 1;
    }
  }
  return coolingGas;
}

// ============================================================================
// THE RECIPES' TAIL AND THE CLASSIC TRANSFER
// ============================================================================

/**
 * @brief Cooling energy, accumulated undivided
 *
 * sage26_finalize_cooling_heating normalises it once per snapshot. Vvir is SAGE26's float.
 */
static inline void mimic_sage_cooling_energy(struct GalaxyData *galaxy, const float vvir,
                                             const double coolingGas) {
  if (coolingGas > 0.0) {
    galaxy->Cooling += 0.5 * coolingGas * vvir * vvir;
  }
}

/**
 * @brief CoolingRate from a cooled mass
 *
 * SAGE parity: (mass / dt) * 1.0e10 / Hubble_h * SEC_PER_GIGAYEAR / UnitTime_in_s narrowed to
 * float, the true Msun/Gyr rate over little h; 0 for a non-positive dt.
 */
static inline float mimic_sage_cooling_rate(const double coolingGas, const double dt,
                                            const struct MimicConfig *params) {
  return (dt > 0.0) ? (float)((coolingGas / dt) * 1.0e10 / params->Hubble_h *
                              SAGE_SEC_PER_GIGAYEAR / params->UnitTime_in_s)
                    : 0.0f;
}

/**
 * @brief SAGE26's cool_gas_onto_galaxy: move the cooled mass from hot to cold gas
 *
 * Below the hot reservoir the mass moves with the hot gas's metallicity; at or above it the whole
 * hot reservoir, metals included, moves. The CGMrecipeOn 0 transfer.
 */
static inline void mimic_sage_cool_gas_onto_galaxy(struct GalaxyData *galaxy,
                                                   const double coolingGas) {
  if (coolingGas > 0.0) {
    if (coolingGas < galaxy->HotGas) {
      const double metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
      galaxy->ColdGas += coolingGas;
      galaxy->MetalsColdGas += metallicity * coolingGas;
      galaxy->HotGas -= coolingGas;
      galaxy->MetalsHotGas -= metallicity * coolingGas;
    } else {
      galaxy->ColdGas += galaxy->HotGas;
      galaxy->MetalsColdGas += galaxy->MetalsHotGas;
      galaxy->HotGas = 0.0;
      galaxy->MetalsHotGas = 0.0;
    }
  }
}

// ============================================================================
// THE TWO RECIPES AS SAGE26 COMPOSES THEM
// ============================================================================

/** @brief The AGN configuration both recipes pass to their AGN call */
struct MimicSageAgnConfig {
  int recipe;        /**< AGNrecipe (0 off) */
  double efficiency; /**< RadioModeEfficiency */
};

/**
 * @brief SAGE26's cooling_recipe_hot, with its AGN call and its tail, except the mdot stores
 *
 * The budget, do_AGN_heating when AGNrecipe > 0 and the post-cap mass is positive, the Cooling
 * energy, and CoolingRate on the paths that reach the tail (the normal tail and the outer guard;
 * the two early returns leave it). The one exception to SAGE26's routine: its galaxy writes of
 * mdot_cool and mdot_stream are not made; their float values are returned in l->stream for the
 * caller.
 *
 * On the -1 return *cooling_out is 0 and the galaxy holds the budget's partial writes made before
 * the refusal (tcool, tff and RcoolToRvir; mimic_sage_hot_cooling_budget); the caller fails the
 * run, so no later step reads them.
 *
 * @param agn_locals  Optional AGN locals (may be NULL); suppressed is -1 when AGN did not run
 * @param cooling_out Output: the mass SAGE26 returns (0 on the -1 return)
 * @return 0, or -1 when the streams refuse the stored SnapNum
 */
static inline int mimic_sage_cooling_recipe_hot(struct Halo *halo, const struct MimicConfig *params,
                                                const struct MimicSageAgnConfig *agn,
                                                const int cgm_recipe_on, const double mshock_msun,
                                                const double dt,
                                                struct MimicSageHotCoolingLocals *l,
                                                struct MimicSageAgnLocals *agn_locals,
                                                double *cooling_out) {
  double coolingGas = 0.0;
  *cooling_out = 0.0;
  if (agn_locals != NULL)
    agn_locals->suppressed = -1;
  if (mimic_sage_hot_cooling_budget(halo, params, dt, cgm_recipe_on, mshock_msun, l, &coolingGas) !=
      0) {
    return -1;
  }
  *cooling_out = coolingGas;
  if (l->path != SAGE_COOLING_PATH_TAIL && l->path != SAGE_COOLING_PATH_OUTER_GUARD) {
    return 0; /* the early returns skip the tail */
  }
  if (l->path == SAGE_COOLING_PATH_TAIL) {
    if (agn->recipe > 0 && coolingGas > 0.0) {
      coolingGas =
          mimic_sage_do_agn_heating_hot(halo, params, agn->recipe, agn->efficiency, cgm_recipe_on,
                                        dt, coolingGas, l->x, l->rcool, agn_locals);
    }
    mimic_sage_cooling_energy(halo->galaxy, (float)halo->Vvir, coolingGas);
  }
  halo->galaxy->CoolingRate = mimic_sage_cooling_rate(coolingGas, dt, params);
  *cooling_out = coolingGas;
  return 0;
}

/**
 * @brief SAGE26's cooling_recipe_cgm, with its AGN call and its tail
 *
 * The budget, do_AGN_heating_cgm when AGNrecipe > 0 and the post-cap mass is positive, the Cooling
 * energy and CoolingRate, always written.
 *
 * @param agn_locals  Optional AGN locals (may be NULL); suppressed is -1 when AGN did not run
 * @return The mass SAGE26 returns
 */
static inline double mimic_sage_cooling_recipe_cgm(struct Halo *halo,
                                                   const struct MimicConfig *params,
                                                   const struct MimicSageAgnConfig *agn,
                                                   const int cgm_recipe_on, const double dt,
                                                   struct MimicSageCgmCoolingLocals *l,
                                                   struct MimicSageAgnLocals *agn_locals) {
  if (agn_locals != NULL)
    agn_locals->suppressed = -1;
  double coolingGas = mimic_sage_cgm_cooling_budget(halo, params, dt, l);
  if (l->lambda_positive == 1) {
    if (agn->recipe > 0 && coolingGas > 0.0) {
      coolingGas =
          mimic_sage_do_agn_heating_cgm(halo, params, agn->recipe, agn->efficiency, cgm_recipe_on,
                                        dt, coolingGas, l->x, l->rcool, agn_locals);
    }
    mimic_sage_cooling_energy(halo->galaxy, (float)halo->Vvir, coolingGas);
  }
  halo->galaxy->CoolingRate = mimic_sage_cooling_rate(coolingGas, dt, params);
  return coolingGas;
}

#endif /* MIMIC_SHARED_SAGE_COOLING_PHYSICS_H */
