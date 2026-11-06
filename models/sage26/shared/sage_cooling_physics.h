/**
 * @file sage_cooling_physics.h
 * @brief SAGE26's cooling, radio-mode AGN heating and cooling transfer, shared by the cooling
 * modules
 *
 * C = R/src/model_cooling_heating.c and H = R/src/model_halo_properties.c of the staged SAGE26
 * source (R). One implementation of each piece of SAGE26's cooling arithmetic, used by the
 * classic three-module hot path (sage26_calculate_cooling_budget -> sage26_radio_mode_heating ->
 * sage26_apply_cooling, CGMrecipeOn 0) and written so that the CGMrecipeOn 1 cooling transaction
 * can call the same code. Every helper takes CGMrecipeOn and the run quantities as arguments; the
 * classic callers pass CGMrecipeOn 0, the R0-R3 behaviour.
 *
 * The classic pieces, moved here from the three modules with every expression unchanged:
 *
 * - mimic_sage_hot_cooling_budget(): cooling_recipe_hot up to the AGN step (C:139-292): the
 *   diagnostics tcool and tff, the cooling function, x, rcool and the float RcoolToRvir, the
 *   cooling mass of the selected branch (CGMrecipeOn 0: the rapid or static branch, C:190-201;
 *   CGMrecipeOn 1: the cold streams below), and its cap to HotGas and floor at zero.
 * - mimic_sage_agn_accretion_compute(): agn_accretion_compute (C:526-576): the rate of the
 *   selected recipe (2, Bondi, ported exactly; 1, empirical, and 3, cold cloud, the earlier
 *   package's), the Eddington cap, the cap of the accreted mass to the reservoir, AGNcoeff and the
 *   cap of the heating to the suppressed cooling mass.
 * - mimic_sage_do_agn_heating_hot(): do_AGN_heating (C:587-627): the heating-radius suppression,
 *   the accretion from the hot pair, the radius ratchet and the Heating energy.
 * - mimic_sage_cooling_energy() and mimic_sage_cooling_rate(): the recipes' tail (C:301-313).
 * - mimic_sage_cool_gas_onto_galaxy(): cool_gas_onto_galaxy (C:689-707), the classic transfer.
 *
 * The CGMrecipeOn 1 pieces (specification c11 item 2), not yet called by production code:
 *
 * - mimic_sage_interpolate_clustering_mass(): interpolate_clustering_mass (H:98-132), the
 *   Press-Schechter clustering mass log10 M_*(z) in log10 Msun (not Msun/h) from SAGE26's literal
 *   31-entry tables, Uchuu for fabs(Omega - 0.3089) < 0.01 and Millennium otherwise.
 * - mimic_sage_cold_stream_cooling(): the hot regime's cold streams (C:205-285), reading the
 *   redshift of the galaxy's STORED snapshot, ZZ[SnapNum] (C:205), through the package's
 *   bounds-checked mimic_sage_fire_stored_redshift (campaign decisions D35 and D37).
 * - mimic_sage_cgm_cooling_budget(): bulk CGM cooling up to the AGN step (C:319-370).
 * - mimic_sage_do_agn_heating_hot() with CGMrecipeOn 1: the 0.99 * rcool saturation (C:590-591)
 *   and the cap of Rheat to Rvir outside the positive-reservoir block (C:623-624).
 * - mimic_sage_do_agn_heating_cgm(): the static do_AGN_heating_cgm (C:635-680): the same
 *   suppression, accretion from the CGM pair, the Rvir cap inside the positive-reservoir block and
 *   the re-cap of the returned mass to the post-accretion CGMgas.
 * - mimic_sage_cooling_recipe_hot() and mimic_sage_cooling_recipe_cgm(): the two recipes as SAGE26
 *   composes them (C:135-315, C:317-391), each with its AGN call and its tail.
 *
 * Precision. SAGE26 holds the reservoirs and Mvir, Rvir, Vvir as floats and every local as a
 * double (R/src/core_allvars.h:106-221); the callers' core doubles are read as those floats.
 * Float-only divisions stay float before widening: Rvir / Vvir (C:142), the metallicity divisions
 * before log10 (C:154, C:326), HotGas / Rvir (C:200, C:279) and the CGM free-fall Rvir * Rvir
 * (C:352).
 *
 * Rounding points. Each expression is one C expression with SAGE26's operand order and types. The
 * fused-instruction table (oracle README, "The c11 Group") read from SAGE26's objects: the
 * clustering interpolation (H:131, fmadd of the first endpoint product with the separately
 * rounded second, model_halo_properties.o 0x3ac) is an explicit fma(); the Cooling and Heating
 * accumulations (C:302, C:377, C:619, C:667), the AGN metal debits (C:609, C:656) and the
 * transfer's metal credit and debit (C:697, C:699) fuse in SAGE26's object and stay one
 * contractible expression here, as they were in the classic modules; the stream and CGM-time
 * arithmetic fuses nothing. SAGE26's object computes both pow(10.0, x) of the streams (C:222,
 * C:242) as __exp10(x); mimic_sage_exp10 writes that form.
 */

#ifndef MIMIC_SHARED_SAGE_COOLING_PHYSICS_H
#define MIMIC_SHARED_SAGE_COOLING_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "types.h"
#include "module_system/physical_constants.h"
#include "shared/metallicity.h"
#include "shared/sage_constants.h"
#include "shared/sage_fire_feedback_physics.h"
#include "modules/sage26_calculate_cooling_budget/cooling_tables.h"

/* Mean molecular weight of a fully ionised gas (C:52) */
#define MIMIC_SAGE_MU_IONISED 0.59
/* Dekel & Birnboim (2006) stream width factor f and logistic width in dex (C:104-105) */
#define MIMIC_SAGE_DB06_STREAM_MASS_FACTOR 3.0
#define MIMIC_SAGE_STREAM_THRESHOLD_WIDTH_DEX 0.15
/* The fraction of rcool at which a CGMrecipeOn 1 heating radius saturates (C:591, C:639) */
#define MIMIC_SAGE_RHEAT_SATURATION 0.99

/* Which cooling-mass branch mimic_sage_hot_cooling_budget took */
#define MIMIC_SAGE_HOT_BRANCH_NONE -1   /* an early return: no cooling mass formed */
#define MIMIC_SAGE_HOT_BRANCH_STATIC 0  /* CGMrecipeOn 0, rcool <= Rvir (C:198-201) */
#define MIMIC_SAGE_HOT_BRANCH_RAPID 1   /* CGMrecipeOn 0, rcool > Rvir (C:193-197) */
#define MIMIC_SAGE_HOT_BRANCH_STREAMS 2 /* CGMrecipeOn 1, the cold streams (C:202-286) */

/* How a reservoir cap of the cooling mass ended (C:288-292, C:366-370) */
#define MIMIC_SAGE_CAP_NOT_REACHED -1
#define MIMIC_SAGE_CAP_NONE 0
#define MIMIC_SAGE_CAP_RESERVOIR 1 /* capped to the reservoir */
#define MIMIC_SAGE_CAP_FLOOR 2     /* floored at zero */

// ============================================================================
// SMALL HELPERS
// ============================================================================

/**
 * @brief 10^x in the form SAGE26's object computes pow(10.0, x)
 *
 * Apple clang compiles SAGE26's pow(10.0, x) at C:222 and C:242 to a call of __exp10 (the
 * relocations of model_cooling_heating.o, oracle README "The c11 Group"); macOS's __exp10 and pow
 * need not agree in the last bit, so the package calls __exp10 there. Elsewhere it writes pow.
 */
static inline double mimic_sage_exp10(const double x) {
#if defined(__APPLE__)
  return __exp10(x);
#else
  return pow(10.0, x);
#endif
}

// ============================================================================
// THE CLUSTERING-MASS TABLE (H:98-132)
// ============================================================================

/** @brief interpolate_clustering_mass's locals, for tests; NULL when not wanted */
struct MimicSageClusteringLocals {
  int uchuu; /**< 1 when the Uchuu table was selected (H:118) */
  double zz; /**< the redshift after the clamps to [0, 15] (H:122-124) */
  int iz;    /**< the lower grid index after its clamps to [0, 29] (H:126-128) */
  double fz; /**< the fractional position (H:130) */
};

/**
 * @brief SAGE26's interpolate_clustering_mass: log10 M_*(z) in log10 Msun (H:98-132)
 *
 * Both 31-entry tables are SAGE26's literal arrays, never recomputed. The interpolation
 * table[iz] * (1 - fz) + table[iz + 1] * fz is fused in SAGE26's object as
 * fma(table[iz], 1 - fz, table[iz + 1] * fz) (model_halo_properties.o 0x3a8-0x3ac: fmul of the
 * second product, then fmadd), written here as that explicit fma().
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
  /* Millennium / WMAP1 (H:104-108) */
  static const double mstar_mill[31] = {
      12.8964, 12.2501, 11.5685, 10.9046, 10.2717, 9.6699, 9.0969, 8.5486, 8.0214, 7.5126, 7.0206,
      6.5435,  6.0794,  5.6266,  5.1842,  4.7514,  4.3276, 3.9122, 3.5045, 3.1039, 2.7098, 2.3217,
      1.9393,  1.5626,  1.1912,  1.0000,  1.0000,  1.0000, 1.0000, 1.0000, 1.0000};
  /* Uchuu / Planck15 (H:111-115) */
  static const double mstar_uchuu[31] = {
      12.8028, 12.0672, 11.3101, 10.5795, 9.8831, 9.2194, 8.5834, 7.9701, 7.3764, 6.8002, 6.2389,
      5.6901,  5.1521,  4.6241,  4.1054,  3.5950, 3.0915, 2.5943, 2.1027, 1.6166, 1.1359, 1.0000,
      1.0000,  1.0000,  1.0000,  1.0000,  1.0000, 1.0000, 1.0000, 1.0000, 1.0000};

  const double *table = mstar_mill;
  int uchuu = 0;
  if (fabs(omega - 0.3089) < 0.01) { /* H:118 */
    table = mstar_uchuu;
    uchuu = 1;
  }

  double zz = z; /* H:122-124 */
  if (zz < mstar_table_z[0])
    zz = mstar_table_z[0];
  if (zz > mstar_table_z[31 - 1])
    zz = mstar_table_z[31 - 1];

  int iz = (int)(zz / 0.5); /* H:126-128 */
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
  /* H:131, fused as SAGE26's object computes it */
  return fma(table[iz], 1.0 - fz, table[iz + 1] * fz);
}

// ============================================================================
// THE HOT RECIPE UP TO THE AGN STEP (C:139-292)
// ============================================================================

/** @brief The cold-stream locals of one CGMrecipeOn 1 hot call (C:205-285) */
struct MimicSageColdStreamLocals {
  double z;                  /**< ZZ[stored SnapNum] (C:205) */
  double M_shock;            /**< MShockMsun * Hubble_h / 1e10 (C:210) */
  double mass_ratio;         /**< Mvir / M_shock (C:211) */
  double log_mstar;          /**< interpolate_clustering_mass(z) (C:222) */
  double Mstar;              /**< 10^log_mstar * Hubble_h / 1e10 (C:222-223) */
  double fMstar;             /**< 3 * Mstar (C:224) */
  double ratio;              /**< (fMstar / Mvir)^(2/3) * mass_ratio^(4/3) (C:225-226) */
  int ratio_nonpositive;     /**< 1 when ratio <= 0 (C:229) */
  double exponent;           /**< log10(ratio) / 0.15 after its clamps (C:239-241); NaN if unused */
  int exponent_capped_high;  /**< 1 when the 300 clamp bound (C:240) */
  int exponent_capped_low;   /**< 1 when the -300 clamp bound (C:241) */
  double f_stream_criterion; /**< f_stream before the cutoff (C:230 or C:242) */
  int cutoff;                /**< 1 when fMstar >= M_shock && mass_ratio > 1 (C:263) */
  double f_stream;           /**< after the cutoff and the clamps (C:263-271) */
  double cold_stream_cooling; /**< f_stream * HotGas / tcool * dt (C:277) */
  double hot_halo_cooling;    /**< (1 - f_stream) * float(HotGas / Rvir) * ... (C:279-280) */
  float mdot_cool;            /**< float(hot_halo_cooling / dt), before the cap and AGN (C:282) */
  float mdot_stream;          /**< float(cold_stream_cooling / dt) (C:283) */
};

/**
 * @brief SAGE26's hot-regime cold streams (C:205-285): the cooling mass before the cap
 *
 * Reads the redshift of the galaxy's stored snapshot, ZZ[stored_snapnum] (C:205), never the
 * current snapshot's; a stored SnapNum outside the run's redshift table is refused with an error
 * naming it (campaign decision D35), where SAGE26 reads out of bounds. Every local is a double;
 * HotGas, Rvir and Mvir are SAGE26's floats.
 *
 * @param params          Run configuration (ZZ, Snaplistlen, Omega, Hubble_h)
 * @param stored_snapnum  The galaxy's stored SnapNum
 * @param mshock_msun     MShockMsun, the shock mass in physical Msun
 * @param mvir            SAGE26's float Mvir
 * @param hot_gas         HotGas
 * @param rvir            SAGE26's float Rvir
 * @param tcool           The hot recipe's tcool, the float Rvir / Vvir widened (C:142)
 * @param rcool           The uncapped cooling radius (C:173)
 * @param dt              Substep interval
 * @param s               Output: the stream locals
 * @param cooling_out     Output: cold_stream_cooling + hot_halo_cooling (C:285)
 * @return 0, or -1 when the stored SnapNum has no redshift (D35)
 */
static inline int mimic_sage_cold_stream_cooling(
    const struct MimicConfig *params, const int stored_snapnum, const double mshock_msun,
    const float mvir, const float hot_gas, const float rvir, const double tcool, const double rcool,
    const double dt, struct MimicSageColdStreamLocals *s, double *cooling_out) {
  double z;
  if (mimic_sage_fire_stored_redshift(params, stored_snapnum, &z) != 0) {
    ERROR_LOG("Cold streams: SAGE26 reads ZZ[SnapNum] of the galaxy's stored snapshot "
              "(R/src/model_cooling_heating.c:205); stored SnapNum = %d has no redshift",
              stored_snapnum);
    return -1;
  }
  s->z = z;

  /* C:210-211: MSUN_TO_CODE_MASS(x, h) is ((x) * (h) / 1.0e10) (R/src/macros.h:47) */
  const double M_shock = mshock_msun * params->Hubble_h / 1.0e10;
  const double mass_ratio = mvir / M_shock;

  /* C:222-226 */
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

  /* C:228-243 */
  double f_stream;
  if (ratio <= 0.0) {
    s->ratio_nonpositive = 1;
    f_stream = 1.0;
  } else {
    s->ratio_nonpositive = 0;
    double exponent = log10(ratio) / MIMIC_SAGE_STREAM_THRESHOLD_WIDTH_DEX;
    // SAGE parity: the exponent is clamped above 300 and then below -300 before 10^exponent
    // (R/src/model_cooling_heating.c:240-241)
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
  // a step at z_crit SAGE26 keeps (R/src/model_cooling_heating.c:245-265)
  s->cutoff = 0;
  if (fMstar >= M_shock && mass_ratio > 1.0) {
    f_stream = 0.0;
    s->cutoff = 1;
  }

  /* C:270-271 */
  if (f_stream > 1.0)
    f_stream = 1.0;
  if (f_stream < 0.0)
    f_stream = 0.0;
  s->f_stream = f_stream;

  /* C:274-285: HotGas / Rvir is a float division */
  const double cold_stream_cooling = f_stream * hot_gas / tcool * dt;
  const double hot_halo_cooling =
      (1.0 - f_stream) * (hot_gas / rvir) * (rcool / (2.0 * tcool)) * dt;

  // SAGE parity: the mdot rates are stored as floats before the reservoir cap and the AGN
  // suppression, so they are not the applied rates (R/src/model_cooling_heating.c:282-283)
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
  double tcool;          /**< float Rvir / Vvir widened (C:142); 0 on the outer guard */
  double lambda;         /**< the cooling function (C:157); NaN when not formed */
  double x;              /**< PROTONMASS * BOLTZMANN * temp / lambda in code units (C:163-164) */
  double rcool;          /**< the uncapped cooling radius (C:173) */
  int branch;            /**< MIMIC_SAGE_HOT_BRANCH_* */
  double cooling_precap; /**< the branch's cooling mass before the cap; 0 when not formed */
  int cap;               /**< MIMIC_SAGE_CAP_* (C:288-292) */
  struct MimicSageColdStreamLocals stream; /**< valid only for MIMIC_SAGE_HOT_BRANCH_STREAMS */
};

/**
 * @brief SAGE26's cooling_recipe_hot up to the AGN step: the post-cap cooling mass (C:139-292)
 *
 * Writes tcool, tff and RcoolToRvir as SAGE26 does, including its early returns, which leave
 * RcoolToRvir untouched. Mvir, Rvir and Vvir are SAGE26 floats (R/src/core_allvars.h:106-221);
 * the core's doubles are read as those floats. With CGMrecipeOn 0 the cooling mass is the rapid
 * or static branch (C:190-201); with 1 it is the cold streams (C:202-286), which read the
 * galaxy's stored SnapNum (halo->SnapNum) and can refuse it (D35).
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
  *cooling_out = 0.0;

  galaxy->tcool = 0.0f;

  if (!(galaxy->HotGas > 0.0 && vvir > 0.0)) {
    l->path = SAGE_COOLING_PATH_OUTER_GUARD;
    return 0;
  }

  // SAGE parity: SAGE26 divides its float Rvir by its float Vvir in float and
  // stores the result in a double (R/src/model_cooling_heating.c:142); every
  // cooling branch then uses that tcool (:190-200, :277-280).
  const double tcool = rvir / vvir;
  const double tff = M_SQRT2 * tcool;
  l->tcool = tcool;

  // SAGE parity: tcool and tff are written with SAGE26's conversion, a mass-rate
  // factor applied to a time, so neither is a time in Gyr
  // (R/src/model_cooling_heating.c:142-148).
  galaxy->tcool =
      (float)((tcool * 1.0e10 / params->Hubble_h * SAGE_SEC_PER_GIGAYEAR / params->UnitTime_in_s));
  galaxy->tff =
      (float)((tff * 1.0e10 / params->Hubble_h * SAGE_SEC_PER_GIGAYEAR / params->UnitTime_in_s));

  const double temp = SAGE_TVIR_K_PER_SQKMS * vvir * vvir; // T_vir in Kelvin

  // The float MetalsHotGas / HotGas is a float division, as in SAGE26 (:152-155)
  double logZ = -10.0;
  if (galaxy->MetalsHotGas > 0) {
    logZ = log10(galaxy->MetalsHotGas / galaxy->HotGas);
  }

  const double lambda = get_metaldependent_cooling_rate(log10(temp), logZ);
  l->lambda = lambda;

  // SAGE parity: the lambda <= 0 and rho_rcool <= 0 early returns come after
  // tcool and tff are written and skip the rest of the routine, so RcoolToRvir,
  // Cooling and CoolingRate keep their previous values
  // (R/src/model_cooling_heating.c:157-169, :175, :302, :310). With CGMrecipeOn 1 the
  // early returns still occur, but the regime-aware wrapper overwrites CoolingRate at its
  // tail (:498-502).
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
    // SAGE parity: with CGMrecipeOn 0 rcool is left uncapped, and the rapid branch cools the
    // whole corona in one tcool while the static branch cools at half that rate at rcool == Rvir:
    // the branches are discontinuous by a factor 2, as SAGE26 keeps them
    // (R/src/model_cooling_heating.c:177-201). CGMrecipeOn 1 replaces the choice by the cold
    // streams (:202-286).
    if (rcool > rvir) {
      coolingGas = galaxy->HotGas / tcool * dt;
      l->branch = MIMIC_SAGE_HOT_BRANCH_RAPID;
    } else {
      // HotGas / Rvir is a float division, as in SAGE26 (:200)
      coolingGas = (galaxy->HotGas / rvir) * (rcool / (2.0 * tcool)) * dt;
      l->branch = MIMIC_SAGE_HOT_BRANCH_STATIC;
    }
  } else {
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
// BULK CGM COOLING UP TO THE AGN STEP (C:319-370)
// ============================================================================

/** @brief SAGE26's locals of cooling_recipe_cgm up to the AGN step */
struct MimicSageCgmCoolingLocals {
  int entered;           /**< CGMgas > 0 && Vvir > 0 (C:321) */
  int lambda_positive;   /**< lambda > 0 (C:331); -1 when not entered */
  double lambda;         /**< the cooling function (C:329); NaN when not formed */
  double x;              /**< as the hot recipe's (C:332-333); NaN when not formed */
  double rho0;           /**< CGMgas / (4 pi r0^3 I_M) (C:345); NaN when not formed */
  double rho_eff;        /**< rho0 * (I_cool / I_M) (C:346); NaN when not formed */
  double tcool;          /**< (x / rho_eff) * (1.5 * 0.59) (C:349); NaN when not formed */
  double g_accel;        /**< G * Mvir / float(Rvir * Rvir) (C:352); NaN when not formed */
  double tff;            /**< sqrt(2 Rvir / g_accel) (C:352-353); NaN when not formed */
  double rcool;          /**< Rvir (C:360); NaN when not formed */
  double cooling_precap; /**< (CGMgas / (tcool + tff)) * dt (C:364); NaN when not formed */
  int cap;               /**< MIMIC_SAGE_CAP_* (C:366-370) */
};

/**
 * @brief SAGE26's cooling_recipe_cgm up to the AGN step: the post-cap CGM cooling mass
 *
 * Carr et al. (2023) bulk cooling of the CGM pair (C:319-370): the double locals, logZ = -10
 * unless MetalsCGMgas > 0, the cooling function, the lambda > 0 guard, alpha 1.4, r0 = 0.1 Rvir,
 * Rvir / r0 = 10, the volume integrals, rho0, rho_eff, tcool and tff, each written to its float
 * diagnostic with its own conversion (C:356-357), rcool = Rvir and the cap to CGMgas and floor at
 * zero. No positive Mvir or Rvir guard is added where SAGE26 has none, and RcoolToRvir is not
 * written (C:361 is commented out in SAGE26). A positive CGMgas with a failed Vvir or lambda
 * guard writes nothing.
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

    // The float MetalsCGMgas / CGMgas is a float division, as in SAGE26 (:324-327)
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

      // Carr et al. (2023) profile constants (C:336-342); SAGE26's object folds the integrals
      const double alpha = 1.4;
      const double r0 = 0.1 * rvir;
      const double ratio = 10.0;
      const double I_M = (pow(ratio, 3.0 - alpha) - 1.0) / (3.0 - alpha);
      const double I_cool = (pow(ratio, 3.0 - 2.0 * alpha) - 1.0) / (3.0 - 2.0 * alpha);

      const double rho0 = galaxy->CGMgas / (4.0 * M_PI * r0 * r0 * r0 * I_M); /* C:345 */
      const double rho_eff = rho0 * (I_cool / I_M);                           /* C:346 */
      const double tcool = (x / rho_eff) * (1.5 * MIMIC_SAGE_MU_IONISED);     /* C:349 */

      // C:352-353: Rvir * Rvir is a float product
      const double g_accel = params->G * mvir / (rvir * rvir);
      const double tff = sqrt(2.0 * rvir / g_accel);

      // SAGE parity: the CGM diagnostics are tcool and tff in Gyr, a different conversion from
      // the hot recipe's (R/src/model_cooling_heating.c:145-148, :356-357)
      galaxy->tcool = (float)(tcool * params->UnitTime_in_s / SAGE_SEC_PER_GIGAYEAR);
      galaxy->tff = (float)(tff * params->UnitTime_in_s / SAGE_SEC_PER_GIGAYEAR);

      // SAGE parity: rcool is pinned to Rvir for the AGN call, and RcoolToRvir is not written
      // (R/src/model_cooling_heating.c:359-361)
      const double rcool = rvir;

      coolingGas = (galaxy->CGMgas / (tcool + tff)) * dt; /* C:364 */
      l->cooling_precap = coolingGas;

      if (coolingGas > galaxy->CGMgas) { /* C:366-370 */
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
// RADIO-MODE AGN ACCRETION (C:526-576)
// ============================================================================

/**
 * @brief AGN Mode 1: Empirical accretion recipe (the earlier package's, not SAGE26's form)
 *
 * Scales with black hole mass, virial velocity, and hot gas fraction.
 * Normalized to typical massive galaxy: M_BH = 10^8 M_sun, V_vir = 200 km/s, f_hot = 0.1
 */
static inline double mimic_sage_agn_rate_empirical(const double black_hole_mass, const double mvir,
                                                   const double vvir, const double hot_gas,
                                                   const double efficiency,
                                                   const struct MimicConfig *run_params) {
  const double unit_conv =
      run_params->UnitMass_in_g / run_params->UnitTime_in_s * SEC_PER_YEAR / SOLAR_MASS;

  double AGNrate;
  if (mvir > 0.0) {
    AGNrate = efficiency / unit_conv * (black_hole_mass / 0.01) * // Normalized to M_BH = 10^8 M_sun
              (vvir / 200.0) * (vvir / 200.0) * (vvir / 200.0) *  // Normalized to V_vir = 200 km/s
              ((hot_gas / mvir) / 0.1);                           // Normalized to f_hot = 0.1
  } else {
    AGNrate = efficiency / unit_conv * (black_hole_mass / 0.01) * (vvir / 200.0) * (vvir / 200.0) *
              (vvir / 200.0);
  }

  return AGNrate;
}

/**
 * @brief AGN Mode 2: Bondi-Hoyle accretion recipe (Bondi 1952)
 *
 * SAGE26's Bondi branch of agn_accretion_compute (R/src/model_cooling_heating.c:538-540),
 * with x carried from the cooling budget rather than recomputed from the cooling function.
 */
static inline double mimic_sage_agn_rate_bondi(const double black_hole_mass, const double x,
                                               const double efficiency,
                                               const struct MimicConfig *run_params) {
  return (2.5 * M_PI * run_params->G) * (0.375 * 0.6 * x) * black_hole_mass * efficiency;
}

/**
 * @brief AGN Mode 3: Cold cloud accretion recipe (the earlier package's, not SAGE26's form)
 *
 * Triggered when BH mass exceeds threshold: M_BH > 10^-4 * M_vir * (R_cool/R_vir)^3
 * Accretion rate = 0.01% of cooling rate when triggered.
 */
static inline double mimic_sage_agn_rate_cold_cloud(const double black_hole_mass, const double mvir,
                                                    const double rcool, const double rvir,
                                                    const double coolingGas, const double dt) {
  if (black_hole_mass > 0.0001 * mvir * (rcool / rvir) * (rcool / rvir) * (rcool / rvir)) {
    return 0.0001 * coolingGas / dt;
  } else {
    return 0.0;
  }
}

/**
 * @brief SAGE26's agn_accretion_compute: accreted mass and heating mass
 *
 * Accretion rate by recipe, the Eddington cap, the reservoir cap, the heating
 * coefficient and the cap of heating to the suppressed cooling mass, in SAGE26's
 * order and with its Vvir > 0 and AGNcoeff > 0 guards
 * (R/src/model_cooling_heating.c:526-575). Reads no field it writes. The heating
 * coefficient uses SAGE26's float Vvir; the empirical and cold-cloud rates, which
 * are not ported, keep the core's double halo values they always used.
 *
 * @param halo            Halo whose galaxy accretes (galaxy not NULL)
 * @param agn_recipe      AGNrecipe (1, 2 or 3; any other positive value is the empirical rate)
 * @param efficiency      RadioModeEfficiency
 * @param dt              Substep interval
 * @param x               SAGE26's x of the calling recipe
 * @param coolingGas      The suppressed cooling mass
 * @param rcool           SAGE26's rcool of the calling recipe
 * @param reservoir_mass  The selected reservoir: HotGas (hot call) or CGMgas (CGM call)
 * @param params          Run configuration
 * @param accreted_out    Output: the accreted mass
 * @param heating_out     Output: the heating mass
 */
static inline void mimic_sage_agn_accretion_compute(const struct Halo *halo, const int agn_recipe,
                                                    const double efficiency, const double dt,
                                                    const double x, const double coolingGas,
                                                    const double rcool, const double reservoir_mass,
                                                    const struct MimicConfig *params,
                                                    double *accreted_out, double *heating_out) {
  const double black_hole_mass = halo->galaxy->BlackHoleMass;
  const double vvir = (float)halo->Vvir;

  double AGNrate;
  if (agn_recipe == 2) {
    AGNrate = mimic_sage_agn_rate_bondi(black_hole_mass, x, efficiency, params);
  } else if (agn_recipe == 3) {
    AGNrate = mimic_sage_agn_rate_cold_cloud(black_hole_mass, halo->Mvir, rcool, halo->Rvir,
                                             coolingGas, dt);
  } else {
    AGNrate = mimic_sage_agn_rate_empirical(black_hole_mass, halo->Mvir, halo->Vvir, reservoir_mass,
                                            efficiency, params);
  }

  // Eddington-limited accretion: L_edd = 1.3e38 erg/s per Msun, radiative
  // efficiency 0.1, c^2 = 9e10 (km/s)^2 (R/src/model_cooling_heating.c:65-67, :553-557)
  const double EDDrate = (1.3e38 * black_hole_mass * 1e10 / params->Hubble_h) /
                         (params->UnitEnergy_in_cgs / params->UnitTime_in_s) / (0.1 * 9.0e10);
  if (AGNrate > EDDrate)
    AGNrate = EDDrate;

  double AGNaccreted = AGNrate * dt;
  if (AGNaccreted > reservoir_mass)
    AGNaccreted = reservoir_mass;

  // Heating coefficient 1.34e5 km/s = sqrt(2 eta c^2) (:72, :562-572)
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
// RADIO-MODE AGN HEATING (C:587-680)
// ============================================================================

/** @brief The locals of one do_AGN_heating or do_AGN_heating_cgm call, for tests */
struct MimicSageAgnLocals {
  int suppressed; /**< 1 when the radius suppression formula ran, 0 when zeroed (C:590-595) */
  int saturated;  /**< 1 when the CGMrecipeOn 1 saturation zeroed it (Rheat >= 0.99 rcool) */
  double coolingGas_suppressed; /**< after the suppression */
  int reservoir_positive;       /**< the selected reservoir was positive (C:599, C:648) */
  double accreted;              /**< agn_accretion_compute's AGNaccreted; 0 when not formed */
  double heating;               /**< its AGNheating; 0 when not formed */
  int ratchet_raised;           /**< 1 when the ratchet raised Rheat (C:612-616, C:658-662) */
  int rheat_capped;             /**< 1 when Rheat was capped to Rvir (C:623-624, C:663-664) */
  int recapped;                 /**< CGM call: 1 when the returned mass was re-capped (C:675-677) */
};

/** @brief Whether SAGE26's heating-radius suppression applies (C:590-591, C:638-639) */
static inline int mimic_sage_agn_suppression_applies(const float rheat, const double rcool,
                                                     const int cgm_recipe_on, int *saturated) {
  // SAGE parity: the cooling mass is suppressed only by the heating radius from earlier events;
  // Rheat >= rcool suppresses it completely, and with CGMrecipeOn 1 so does
  // Rheat >= 0.99 * rcool (R/src/model_cooling_heating.c:590-595, :638-643)
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
 * Reproduces R/src/model_cooling_heating.c:587-627. Vvir and Rvir are SAGE26 floats
 * (R/src/core_allvars.h:106-221); the core's doubles are read as those floats.
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
    mimic_sage_agn_accretion_compute(halo, agn_recipe, efficiency, dt, x, coolingGas, rcool,
                                     galaxy->HotGas, params, &AGNaccreted, &AGNheating);
    l->accreted = AGNaccreted;
    l->heating = AGNheating;

    // Metallicity of the hot gas before the accretion is drawn from it (:605-608)
    const double metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
    galaxy->BlackHoleMass += AGNaccreted;
    galaxy->HotGas -= AGNaccreted;
    galaxy->MetalsHotGas -= metallicity * AGNaccreted;

    // The heating radius ratchet: it only grows (:612-616)
    if (galaxy->Rheat < rcool && coolingGas > 0.0) {
      const double rheat_new = (AGNheating / coolingGas) * rcool;
      if (rheat_new > galaxy->Rheat) {
        galaxy->Rheat = rheat_new;
        l->ratchet_raised = 1;
      }
    }

    // Heating energy, accumulated undivided; sage26_finalize_cooling_heating
    // normalises it once per snapshot (:618-620, R/src/core_build_model.c:590-603)
    if (AGNheating > 0.0)
      galaxy->Heating += 0.5 * AGNheating * vvir * vvir;
  }

  // SAGE parity: with CGMrecipeOn 1 the hot call caps Rheat to Rvir outside its positive-reservoir
  // block, so a galaxy without hot gas is capped too (R/src/model_cooling_heating.c:623-624)
  if (cgm_recipe_on > 0 && galaxy->Rheat > rvir) {
    galaxy->Rheat = rvir;
    l->rheat_capped = 1;
  }

  // SAGE parity: the AGN heating is not subtracted from the cooling mass a second
  // time; only the earlier heating radius suppresses it, and the returned mass is
  // the radius-suppressed one (R/src/model_cooling_heating.c:590-626).
  return coolingGas;
}

/**
 * @brief SAGE26's static do_AGN_heating_cgm: suppress, grow the BH from the CGM pair, re-cap
 *
 * Reproduces R/src/model_cooling_heating.c:635-680: the suppression as the hot call's, the
 * accretion from CGMgas and its metals, the ratchet, the cap of Rheat to Rvir inside the
 * positive-reservoir block (C:663-664), the Heating energy and the re-cap of the returned mass to
 * the post-accretion CGMgas (C:675-677).
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
    mimic_sage_agn_accretion_compute(halo, agn_recipe, efficiency, dt, x, coolingGas, rcool,
                                     galaxy->CGMgas, params, &AGNaccreted, &AGNheating);
    l->accreted = AGNaccreted;
    l->heating = AGNheating;

    /* C:653-656: the CGM pair's metallicity before the accretion is drawn from it */
    const double metallicity = mimic_get_metallicity(galaxy->CGMgas, galaxy->MetalsCGMgas);
    galaxy->BlackHoleMass += AGNaccreted;
    galaxy->CGMgas -= AGNaccreted;
    galaxy->MetalsCGMgas -= metallicity * AGNaccreted;

    /* C:658-662 */
    if (galaxy->Rheat < rcool && coolingGas > 0.0) {
      const double rheat_new = (AGNheating / coolingGas) * rcool;
      if (rheat_new > galaxy->Rheat) {
        galaxy->Rheat = rheat_new;
        l->ratchet_raised = 1;
      }
    }
    // SAGE parity: the CGM call caps Rheat to Rvir inside its positive-reservoir block, with no
    // CGMrecipeOn test (R/src/model_cooling_heating.c:663-664)
    if (galaxy->Rheat > rvir) {
      galaxy->Rheat = rvir;
      l->rheat_capped = 1;
    }

    /* C:666-667 */
    if (AGNheating > 0.0)
      galaxy->Heating += 0.5 * AGNheating * vvir * vvir;

    // SAGE parity: the returned mass is re-capped to the CGM left after the accretion, and the
    // heating is not subtracted from it (R/src/model_cooling_heating.c:669-677)
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
 * @brief Cooling energy, accumulated undivided (C:301-303, C:376-378)
 *
 * sage26_finalize_cooling_heating normalises it once per snapshot
 * (R/src/core_build_model.c:590-603). Vvir is SAGE26's float.
 */
static inline void mimic_sage_cooling_energy(struct GalaxyData *galaxy, const float vvir,
                                             const double coolingGas) {
  if (coolingGas > 0.0) {
    galaxy->Cooling += 0.5 * coolingGas * vvir * vvir;
  }
}

/**
 * @brief CoolingRate from a cooled mass (C:310-313, C:382-385, C:499-502)
 *
 * SAGE parity: (mass / dt) * 1.0e10 / Hubble_h * SEC_PER_GIGAYEAR / UnitTime_in_s narrowed to
 * float, the true Msun/Gyr rate over little h; 0 for a non-positive dt
 * (R/src/model_cooling_heating.c:310-313).
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
 * Reproduces R/src/model_cooling_heating.c:689-705: below the hot reservoir the
 * mass moves with the hot gas's metallicity; at or above it the whole hot
 * reservoir, metals included, moves. The classic (CGMrecipeOn 0) transfer.
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
 * @brief SAGE26's whole cooling_recipe_hot (C:135-315), with its AGN call and its tail
 *
 * The budget, do_AGN_heating when AGNrecipe > 0 and the post-cap mass is positive (C:297-299),
 * the Cooling energy, and CoolingRate on the paths that reach the tail (the normal tail and the
 * outer guard; the two early returns leave it, C:159-169).
 *
 * @param agn_locals  Optional AGN locals (may be NULL); suppressed is -1 when AGN did not run
 * @param cooling_out Output: the mass SAGE26 returns
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
  if (agn_locals != NULL)
    agn_locals->suppressed = -1;
  if (mimic_sage_hot_cooling_budget(halo, params, dt, cgm_recipe_on, mshock_msun, l, &coolingGas) !=
      0) {
    return -1;
  }
  *cooling_out = coolingGas;
  if (l->path != SAGE_COOLING_PATH_TAIL && l->path != SAGE_COOLING_PATH_OUTER_GUARD) {
    return 0; /* C:159-169: the early returns skip the tail */
  }
  if (l->path == SAGE_COOLING_PATH_TAIL) {
    if (agn->recipe > 0 && coolingGas > 0.0) { /* C:297-299 */
      coolingGas =
          mimic_sage_do_agn_heating_hot(halo, params, agn->recipe, agn->efficiency, cgm_recipe_on,
                                        dt, coolingGas, l->x, l->rcool, agn_locals);
    }
    mimic_sage_cooling_energy(halo->galaxy, (float)halo->Vvir, coolingGas); /* C:301-303 */
  }
  halo->galaxy->CoolingRate = mimic_sage_cooling_rate(coolingGas, dt, params); /* C:310-313 */
  *cooling_out = coolingGas;
  return 0;
}

/**
 * @brief SAGE26's whole cooling_recipe_cgm (C:317-391), with its AGN call and its tail
 *
 * The budget, do_AGN_heating_cgm when AGNrecipe > 0 and the post-cap mass is positive
 * (C:372-374), the Cooling energy (C:376-378) and CoolingRate, always written (C:382-385).
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
    if (agn->recipe > 0 && coolingGas > 0.0) { /* C:372-374 */
      coolingGas =
          mimic_sage_do_agn_heating_cgm(halo, params, agn->recipe, agn->efficiency, cgm_recipe_on,
                                        dt, coolingGas, l->x, l->rcool, agn_locals);
    }
    mimic_sage_cooling_energy(halo->galaxy, (float)halo->Vvir, coolingGas); /* C:376-378 */
  }
  halo->galaxy->CoolingRate = mimic_sage_cooling_rate(coolingGas, dt, params); /* C:382-385 */
  return coolingGas;
}

#endif /* MIMIC_SHARED_SAGE_COOLING_PHYSICS_H */
