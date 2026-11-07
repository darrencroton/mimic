/**
 * @file    sage26_oracle_cooling.h
 * @brief   Oracle-test helpers for the CGMrecipeOn 1 cooling fixtures (oracle_c11)
 *
 * Adds, beside sage26_oracle_fixture.h (which stays as it is), what the oracle tests of
 * sage26_calculate_cooling_budget and sage26_radio_mode_heating need for the CGM cooling fixtures
 * (oracle README, "The c11 Group"). Each CGM cooling fixture is a direct call of a public SAGE26
 * routine, and the test calls the package's shared helpers (shared/sage_cooling_physics.h)
 * directly, since no production module calls them yet:
 *
 * - interpolate_clustering_mass  -> mimic_sage_interpolate_clustering_mass
 * - cooling_recipe_hot_streams   -> mimic_sage_hot_cooling_budget (CGMrecipeOn 1), then
 *                                   mimic_sage_do_agn_heating_hot and the tail; and the composed
 *                                   mimic_sage_cooling_recipe_hot on a fresh copy
 * - cooling_recipe_cgm           -> mimic_sage_cgm_cooling_budget, mimic_sage_do_agn_heating_cgm
 *                                   and the tail; and mimic_sage_cooling_recipe_cgm on a copy
 * - do_AGN_heating_cgm_on        -> mimic_sage_do_agn_heating_hot (CGMrecipeOn 1)
 *
 * Every recorded field, return value, branch flag the helpers expose and local is asserted bit
 * for bit; each local's check and validation markers are consumed and tallied; a binding cap's
 * value is asserted equal to the field it was capped to. The oracle's flags for caps inside
 * agn_accretion_compute and the f_stream clamps, which the helpers do not report, are consumed
 * unasserted (oracle_exclude); their effect is in the recorded state.
 *
 * Test-only: included by module oracle tests, never by package code.
 */

#ifndef SAGE26_ORACLE_COOLING_H
#define SAGE26_ORACLE_COOLING_H

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "modules/_tests/sage26_oracle_fixture.h"
#include "shared/sage_cooling_physics.h"
#include "shared/time_parity.h"

/** @brief Tallies of the CGM cooling locals an oracle test compared, for its report */
struct oracle_c11_tally {
  int fixtures;     /**< CGM cooling fixtures checked */
  int locals;       /**< locals compared bit for bit */
  int continuation; /**< of which continuation-checked */
  int tail;         /**< of which tail-checked */
  int validated;    /**< of which validated two-sided */
  int binding;      /**< binding-cap values asserted equal to their field */
  int flags;        /**< branch flags asserted */
};

static struct oracle_c11_tally oracle_c11_counts;

static inline int oracle_c11_is_routine(const char *routine) {
  return strcmp(routine, "interpolate_clustering_mass") == 0 ||
         strcmp(routine, "cooling_recipe_hot_streams") == 0 ||
         strcmp(routine, "cooling_recipe_cgm") == 0 ||
         strcmp(routine, "do_AGN_heating_cgm_on") == 0;
}

/** @brief Compare one local, consume its markers and tally it; absent in the oracle -> skipped */
static inline int oracle_c11_check_local(struct oracle_fixture *out, const char *name,
                                         const double actual) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s", name);
  if (!oracle_has(out, field)) {
    return 0;
  }
  int problems = oracle_check_f64(out, field, actual);
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  problems += oracle_check_i32(out, field, 0);
  snprintf(field, sizeof(field), "intermediate.%s_checked", name);
  problems += oracle_check_i32(out, field, 1);
  snprintf(field, sizeof(field), "intermediate.%s_check_kind", name);
  const int kind = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", name);
  (void)oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", name);
  (void)oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, field);
  oracle_c11_counts.locals++;
  if (kind == 1) {
    oracle_c11_counts.continuation++;
  } else {
    oracle_c11_counts.tail++;
  }
  if (validated == 1) {
    oracle_c11_counts.validated++;
  }
  return problems;
}

static inline int oracle_c11_check_flag(struct oracle_fixture *out, const char *name,
                                        const int actual) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s", name);
  oracle_c11_counts.flags++;
  return oracle_check_i32(out, field, actual);
}

static inline int oracle_c11_flag(struct oracle_fixture *out, const char *name) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s", name);
  return oracle_in_i32(out, field);
}

static inline void oracle_c11_exclude_flag(struct oracle_fixture *out, const char *name) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s", name);
  oracle_exclude(out, field);
}

/** @brief A binding cap: the value must equal the field it was capped to, bit for bit */
static inline int oracle_c11_check_binding(const char *fixture, const char *what,
                                           const double value, const double field) {
  oracle_c11_counts.binding++;
  if (oracle_f64_to_bits(value) != oracle_f64_to_bits(field)) {
    fprintf(stderr, "oracle: %s: binding cap %s = %.17g differs from its field %.17g\n", fixture,
            what, value, field);
    return 1;
  }
  return 0;
}

/** @brief The CGM cooling fixture's units into MimicConfig, and its AGN configuration */
static inline struct MimicSageAgnConfig oracle_c11_set_params(struct oracle_fixture *in) {
  struct MimicSageAgnConfig agn;
  agn.recipe = oracle_in_i32(in, "params.AGNrecipeOn");
  (void)oracle_in_i32(in, "params.CGMrecipeOn"); /* always 1: the helpers are called with 1 */
  MimicConfig.G = oracle_in_f64(in, "params.G");
  agn.efficiency = oracle_in_f64(in, "params.RadioModeEfficiency");
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");
  MimicConfig.UnitEnergy_in_cgs = oracle_in_f64(in, "params.UnitEnergy_in_cgs");
  MimicConfig.UnitTime_in_s = oracle_in_f64(in, "params.UnitTime_in_s");
  MimicConfig.UnitDensity_in_cgs = oracle_in_f64(in, "params.UnitDensity_in_cgs");
  return agn;
}

/**
 * @brief Set halo and galaxy from a CGM cooling fixture's galaxy[0] input (SAGE26's floats widened)
 */
static inline void oracle_c11_in_galaxy(struct oracle_fixture *in, struct Halo *halo,
                                        struct GalaxyData *galaxy) {
  halo->galaxy = galaxy;
  halo->Type = oracle_in_i32(in, "galaxy[0].Type");
  halo->SnapNum = oracle_in_i32(in, "galaxy[0].SnapNum");
  halo->Mvir = oracle_in_f32(in, "galaxy[0].Mvir");
  halo->Rvir = oracle_in_f32(in, "galaxy[0].Rvir");
  halo->Vvir = oracle_in_f32(in, "galaxy[0].Vvir");
  galaxy->HotGas = oracle_in_f32(in, "galaxy[0].HotGas");
  galaxy->MetalsHotGas = oracle_in_f32(in, "galaxy[0].MetalsHotGas");
  galaxy->CGMgas = oracle_in_f32(in, "galaxy[0].CGMgas");
  galaxy->MetalsCGMgas = oracle_in_f32(in, "galaxy[0].MetalsCGMgas");
  galaxy->ColdGas = oracle_in_f32(in, "galaxy[0].ColdGas");
  galaxy->MetalsColdGas = oracle_in_f32(in, "galaxy[0].MetalsColdGas");
  galaxy->BlackHoleMass = oracle_in_f32(in, "galaxy[0].BlackHoleMass");
  galaxy->Rheat = oracle_in_f32(in, "galaxy[0].r_heat");
  galaxy->Cooling = oracle_in_f64(in, "galaxy[0].Cooling");
  galaxy->Heating = oracle_in_f64(in, "galaxy[0].Heating");
  galaxy->tcool = oracle_in_f32(in, "galaxy[0].tcool");
  galaxy->tff = oracle_in_f32(in, "galaxy[0].tff");
  galaxy->RcoolToRvir = oracle_in_f32(in, "galaxy[0].RcoolToRvir");
  galaxy->CoolingRate = oracle_in_f32(in, "galaxy[0].CoolingRate");
  galaxy->mdot_cool = oracle_in_f32(in, "galaxy[0].mdot_cool");
  galaxy->mdot_stream = oracle_in_f32(in, "galaxy[0].mdot_stream");
}

/** @brief Every recorded field of galaxy[0], bit for bit */
static inline int oracle_c11_check_state(struct oracle_fixture *out, const struct GalaxyData *g) {
  int p = 0;
  p += oracle_check_f32(out, "galaxy[0].HotGas", g->HotGas);
  p += oracle_check_f32(out, "galaxy[0].MetalsHotGas", g->MetalsHotGas);
  p += oracle_check_f32(out, "galaxy[0].CGMgas", g->CGMgas);
  p += oracle_check_f32(out, "galaxy[0].MetalsCGMgas", g->MetalsCGMgas);
  p += oracle_check_f32(out, "galaxy[0].ColdGas", g->ColdGas);
  p += oracle_check_f32(out, "galaxy[0].MetalsColdGas", g->MetalsColdGas);
  p += oracle_check_f32(out, "galaxy[0].BlackHoleMass", g->BlackHoleMass);
  p += oracle_check_f32(out, "galaxy[0].r_heat", g->Rheat);
  p += oracle_check_f32(out, "galaxy[0].tcool", g->tcool);
  p += oracle_check_f32(out, "galaxy[0].tff", g->tff);
  p += oracle_check_f32(out, "galaxy[0].RcoolToRvir", g->RcoolToRvir);
  p += oracle_check_f32(out, "galaxy[0].CoolingRate", g->CoolingRate);
  p += oracle_check_f32(out, "galaxy[0].mdot_cool", g->mdot_cool);
  p += oracle_check_f32(out, "galaxy[0].mdot_stream", g->mdot_stream);
  p += oracle_check_f64(out, "galaxy[0].Cooling", g->Cooling);
  p += oracle_check_f64(out, "galaxy[0].Heating", g->Heating);
  return p;
}

/** @brief Two galaxies' recorded fields bit for bit (the step-by-step and composed helpers) */
static inline int oracle_c11_same_state(const struct GalaxyData *a, const struct GalaxyData *b) {
  const float fa[] = {a->HotGas,        a->MetalsHotGas,  a->CGMgas,    a->MetalsCGMgas, a->ColdGas,
                      a->MetalsColdGas, a->BlackHoleMass, a->Rheat,     a->tcool,        a->tff,
                      a->RcoolToRvir,   a->CoolingRate,   a->mdot_cool, a->mdot_stream};
  const float fb[] = {b->HotGas,        b->MetalsHotGas,  b->CGMgas,    b->MetalsCGMgas, b->ColdGas,
                      b->MetalsColdGas, b->BlackHoleMass, b->Rheat,     b->tcool,        b->tff,
                      b->RcoolToRvir,   b->CoolingRate,   b->mdot_cool, b->mdot_stream};
  return memcmp(fa, fb, sizeof(fa)) == 0 &&
         oracle_f64_to_bits(a->Cooling) == oracle_f64_to_bits(b->Cooling) &&
         oracle_f64_to_bits(a->Heating) == oracle_f64_to_bits(b->Heating);
}

/** @brief The substep dt from the oracle's interval, checked against intermediate.dt */
static inline int oracle_c11_dt(struct oracle_fixture *in, struct oracle_fixture *out,
                                struct Halo *halo, double *dt) {
  (void)oracle_in_f64(in, "call.Age_galaxy_snap");
  (void)oracle_in_f64(in, "call.halo_age");
  halo->dT = oracle_in_f64(out, "intermediate.deltaT");
  struct ModuleContext ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.params = &MimicConfig;
  ctx.num_substeps = oracle_in_i32(in, "call.effective_steps");
  ctx.central_galaxy = halo;
  int problems = 0;
  if (mimic_object_substep_dt(halo, &ctx, dt) != MIMIC_OBJECT_TIME_OK) {
    fprintf(stderr, "oracle: substep dt is not available\n");
    problems++;
  }
  return problems + oracle_check_f64(out, "intermediate.dt", *dt);
}

/** @brief The AGN locals and flags of one hot or CGM AGN call */
static inline int oracle_c11_check_agn(const char *fixture, struct oracle_fixture *out,
                                       const struct MimicSageAgnLocals *al,
                                       const double reservoir_before) {
  int p = 0;
  p += oracle_c11_check_flag(out, "agn_suppressed", al->suppressed);
  p += oracle_c11_check_flag(out, "agn_saturated", al->saturated);
  p += oracle_c11_check_flag(out, "agn_reservoir_positive", al->reservoir_positive);
  p += oracle_c11_check_local(out, "CoolingGas_suppressed", al->coolingGas_suppressed);
  if (al->reservoir_positive) {
    p += oracle_c11_check_flag(out, "ratchet_raised", al->ratchet_raised);
    p += oracle_c11_check_local(out, "AGNaccreted", al->accreted);
    p += oracle_c11_check_local(out, "AGNheating", al->heating);
    /* C:560 binds and the heating cap (C:568-571) does not rewrite it afterwards */
    if (oracle_c11_flag(out, "accretion_capped") == 1 &&
        oracle_c11_flag(out, "heating_capped") == 0) {
      p += oracle_c11_check_binding(fixture, "AGNaccreted", al->accreted, reservoir_before);
    }
    if (oracle_c11_flag(out, "heating_capped") == 1) {
      p += oracle_c11_check_binding(fixture, "AGNheating", al->heating, al->coolingGas_suppressed);
    }
    oracle_c11_exclude_flag(out, "eddington_capped"); /* inside agn_accretion_compute */
  } else {
    p += oracle_c11_check_flag(out, "ratchet_raised", al->ratchet_raised);
    oracle_c11_exclude_flag(out, "eddington_capped");
  }
  p += oracle_c11_check_flag(out, "rheat_capped", al->rheat_capped);
  return p;
}

/** @brief One interpolate_clustering_mass fixture */
static inline int oracle_c11_check_clustering(struct oracle_fixture *in,
                                              struct oracle_fixture *out) {
  const double omega = oracle_in_f64(in, "params.Omega");
  const double z = oracle_in_f64(in, "call.z");
  struct MimicSageClusteringLocals cl;
  const double log_mstar = mimic_sage_interpolate_clustering_mass(z, omega, &cl);
  int p = oracle_check_f64(out, "return.log_mstar", log_mstar);
  p += oracle_c11_check_flag(out, "uchuu", cl.uchuu);
  p += oracle_c11_check_flag(out, "iz", cl.iz);
  p += oracle_c11_check_flag(out, "z_clamped_low", (z < 0.0) ? 1 : 0);
  p += oracle_c11_check_flag(out, "z_clamped_high", (z > 15.0) ? 1 : 0);
  p += oracle_c11_check_flag(out, "iz_clamped", ((int)(cl.zz / 0.5) != cl.iz) ? 1 : 0);
  p += oracle_c11_check_local(out, "zz", cl.zz);
  p += oracle_c11_check_local(out, "fz", cl.fz);
  /* the unfused form is evidence for the fused-instruction table: the package must not equal it
   * where SAGE26's fused value differs */
  const double unfused = oracle_in_f64(out, "intermediate.unfused_log_mstar");
  const int differs = oracle_in_i32(out, "intermediate.fused_differs_from_unfused");
  if (differs && oracle_f64_to_bits(unfused) == oracle_f64_to_bits(log_mstar)) {
    fprintf(stderr, "oracle: the package computes the unfused interpolation\n");
    p++;
  }
  return p;
}

/** @brief The hot recipe's locals (budget and streams) */
static inline int oracle_c11_check_hot_locals(const char *fixture, struct oracle_fixture *out,
                                              const struct MimicSageHotCoolingLocals *l,
                                              const double postcap, const double hot_gas_in) {
  int p = oracle_c11_check_flag(out, "return_path", l->path);
  if (l->path == SAGE_COOLING_PATH_OUTER_GUARD) {
    return p;
  }
  p += oracle_c11_check_local(out, "tcool", l->tcool);
  p += oracle_c11_check_local(out, "lambda", l->lambda);
  p += oracle_c11_check_local(out, "x", l->x);
  if (l->path != SAGE_COOLING_PATH_TAIL) {
    return p;
  }
  const struct MimicSageColdStreamLocals *s = &l->stream;
  p += oracle_c11_check_local(out, "rcool", l->rcool);
  p += oracle_c11_check_local(out, "z", s->z);
  p += oracle_c11_check_local(out, "M_shock", s->M_shock);
  p += oracle_c11_check_local(out, "mass_ratio", s->mass_ratio);
  p += oracle_c11_check_local(out, "log_mstar", s->log_mstar);
  p += oracle_c11_check_local(out, "Mstar", s->Mstar);
  p += oracle_c11_check_local(out, "fMstar", s->fMstar);
  p += oracle_c11_check_local(out, "ratio", s->ratio);
  p += oracle_c11_check_flag(out, "ratio_nonpositive", s->ratio_nonpositive);
  if (!s->ratio_nonpositive) {
    p += oracle_c11_check_local(out, "exponent", s->exponent);
    p += oracle_c11_check_flag(out, "exponent_capped_high", s->exponent_capped_high);
    p += oracle_c11_check_flag(out, "exponent_capped_low", s->exponent_capped_low);
  } else {
    p += oracle_c11_check_flag(out, "exponent_capped_high", -1);
    p += oracle_c11_check_flag(out, "exponent_capped_low", -1);
  }
  p += oracle_c11_check_local(out, "f_stream_criterion", s->f_stream_criterion);
  p += oracle_c11_check_flag(out, "stream_cutoff", s->cutoff);
  oracle_c11_exclude_flag(out, "f_stream_capped_high");
  oracle_c11_exclude_flag(out, "f_stream_floored");
  p += oracle_c11_check_local(out, "f_stream", s->f_stream);
  p += oracle_c11_check_local(out, "cold_stream_cooling", s->cold_stream_cooling);
  p += oracle_c11_check_local(out, "hot_halo_cooling", s->hot_halo_cooling);
  p += oracle_c11_check_local(out, "CoolingGas_precap", l->cooling_precap);
  p += oracle_c11_check_flag(out, "reservoir_cap", l->cap);
  p += oracle_c11_check_local(out, "CoolingGas_postcap", postcap);
  if (l->cap == MIMIC_SAGE_CAP_RESERVOIR) {
    p += oracle_c11_check_binding(fixture, "CoolingGas_postcap", postcap, hot_gas_in);
  }
  return p;
}

/** @brief One cooling_recipe_hot_streams fixture */
static inline int oracle_c11_check_hot(const char *fixture, struct oracle_fixture *in,
                                       struct oracle_fixture *out) {
  reset_config();
  const struct MimicSageAgnConfig agn = oracle_c11_set_params(in);
  MimicConfig.Omega = oracle_in_f64(in, "params.Omega");
  const double mshock = oracle_in_f64(in, "params.MShockMsun");
  MimicConfig.Snaplistlen = oracle_in_i32(in, "params.Snaplistlen");
  for (int k = 0; k < ABSOLUTEMAXSNAPS; k++) {
    MimicConfig.ZZ[k] = NAN; /* a read of another snapshot surfaces as NaN */
  }
  const int current = oracle_in_i32(in, "call.current_SnapNum");
  const double zz_stored = oracle_in_f64(in, "params.ZZ_stored");
  const double zz_current = oracle_in_f64(in, "params.ZZ_current");

  struct GalaxyData galaxy, composed_galaxy;
  struct Halo halo, composed_halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  oracle_c11_in_galaxy(in, &halo, &galaxy);
  MimicConfig.ZZ[halo.SnapNum] = zz_stored;
  MimicConfig.ZZ[current] = zz_current;
  double dt = 0.0;
  int p = oracle_c11_dt(in, out, &halo, &dt);
  composed_galaxy = galaxy;
  composed_halo = halo;
  composed_halo.galaxy = &composed_galaxy;
  const double hot_gas_in = galaxy.HotGas;

  /* Step by step: the budget, the AGN, the tail */
  struct MimicSageHotCoolingLocals l;
  double coolingGas = 0.0;
  if (mimic_sage_hot_cooling_budget(&halo, &MimicConfig, dt, 1, mshock, &l, &coolingGas) != 0) {
    fprintf(stderr, "oracle: %s: the hot budget refused the fixture\n", fixture);
    return p + 1;
  }
  if (l.branch == MIMIC_SAGE_HOT_BRANCH_STREAMS) {
    /* The helper writes no mdot field (the cooling transaction stores the raw rates). The harness
     * places the helper's returned float stores where SAGE26 writes them (C:282-283), so the oracle
     * compares those returned values with SAGE26's galaxy fields. */
    galaxy.mdot_cool = l.stream.mdot_cool;
    galaxy.mdot_stream = l.stream.mdot_stream;
  }
  p += oracle_c11_check_hot_locals(fixture, out, &l, coolingGas, hot_gas_in);
  struct MimicSageAgnLocals al;
  int agn_entered = -1;
  if (l.path == SAGE_COOLING_PATH_TAIL) {
    agn_entered = (agn.recipe > 0 && coolingGas > 0.0) ? 1 : 0;
    if (agn_entered) {
      coolingGas = mimic_sage_do_agn_heating_hot(&halo, &MimicConfig, agn.recipe, agn.efficiency, 1,
                                                 dt, coolingGas, l.x, l.rcool, &al);
      p += oracle_c11_check_agn(fixture, out, &al, hot_gas_in);
    }
    mimic_sage_cooling_energy(&galaxy, (float)halo.Vvir, coolingGas);
  }
  if (l.path == SAGE_COOLING_PATH_TAIL || l.path == SAGE_COOLING_PATH_OUTER_GUARD) {
    galaxy.CoolingRate = mimic_sage_cooling_rate(coolingGas, dt, &MimicConfig);
  }
  p += oracle_c11_check_flag(out, "agn_entered", agn_entered);
  if (agn_entered != 1) {
    const char *const unevaluated[] = {
        "agn_suppressed",   "agn_saturated",  "agn_reservoir_positive", "eddington_capped",
        "accretion_capped", "heating_capped", "ratchet_raised",         "rheat_capped"};
    for (size_t k = 0; k < sizeof(unevaluated) / sizeof(unevaluated[0]); k++) {
      p += oracle_c11_check_flag(out, unevaluated[k], -1);
    }
  } else {
    oracle_c11_exclude_flag(out, "accretion_capped");
    oracle_c11_exclude_flag(out, "heating_capped");
  }
  if (l.path == SAGE_COOLING_PATH_OUTER_GUARD) {
    const char *const unevaluated[] = {
        "ratio_nonpositive",    "exponent_capped_high", "exponent_capped_low", "stream_cutoff",
        "f_stream_capped_high", "f_stream_floored",     "reservoir_cap"};
    for (size_t k = 0; k < sizeof(unevaluated) / sizeof(unevaluated[0]); k++) {
      p += oracle_c11_check_flag(out, unevaluated[k], -1);
    }
  }
  p += oracle_check_f64(out, "return.coolingGas", coolingGas);
  p += oracle_c11_check_state(out, &galaxy);

  /* The composed recipe on a fresh copy must reach the same state and return */
  struct MimicSageHotCoolingLocals cl;
  double composed = 0.0;
  if (mimic_sage_cooling_recipe_hot(&composed_halo, &MimicConfig, &agn, 1, mshock, dt, &cl, NULL,
                                    &composed) != 0) {
    fprintf(stderr, "oracle: %s: the composed hot recipe refused the fixture\n", fixture);
    return p + 1;
  }
  if (cl.branch == MIMIC_SAGE_HOT_BRANCH_STREAMS) {
    /* As above: the composed recipe writes no mdot field; the harness places its returned float
     * stores where SAGE26 writes them (C:282-283) for the comparison. */
    composed_galaxy.mdot_cool = cl.stream.mdot_cool;
    composed_galaxy.mdot_stream = cl.stream.mdot_stream;
  }
  if (oracle_f64_to_bits(composed) != oracle_f64_to_bits(coolingGas) ||
      !oracle_c11_same_state(&composed_galaxy, &galaxy)) {
    fprintf(stderr, "oracle: %s: mimic_sage_cooling_recipe_hot differs from its steps\n", fixture);
    p++;
  }
  return p;
}

/** @brief One cooling_recipe_cgm fixture */
static inline int oracle_c11_check_cgm(const char *fixture, struct oracle_fixture *in,
                                       struct oracle_fixture *out) {
  reset_config();
  const struct MimicSageAgnConfig agn = oracle_c11_set_params(in);
  struct GalaxyData galaxy, composed_galaxy;
  struct Halo halo, composed_halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  oracle_c11_in_galaxy(in, &halo, &galaxy);
  double dt = 0.0;
  int p = oracle_c11_dt(in, out, &halo, &dt);
  composed_galaxy = galaxy;
  composed_halo = halo;
  composed_halo.galaxy = &composed_galaxy;
  const double cgm_in = galaxy.CGMgas;

  struct MimicSageCgmCoolingLocals l;
  double coolingGas = mimic_sage_cgm_cooling_budget(&halo, &MimicConfig, dt, &l);
  p += oracle_c11_check_flag(out, "cgm_entered", l.entered);
  p += oracle_c11_check_flag(out, "lambda_positive", l.lambda_positive);
  int agn_entered = -1;
  int recapped = -1;
  if (l.entered) {
    p += oracle_c11_check_local(out, "lambda", l.lambda);
  }
  if (l.lambda_positive == 1) {
    p += oracle_c11_check_local(out, "x", l.x);
    p += oracle_c11_check_local(out, "rho0", l.rho0);
    p += oracle_c11_check_local(out, "rho_eff", l.rho_eff);
    p += oracle_c11_check_local(out, "tcool", l.tcool);
    p += oracle_c11_check_local(out, "g_accel", l.g_accel);
    p += oracle_c11_check_local(out, "tff", l.tff);
    p += oracle_c11_check_local(out, "rcool", l.rcool);
    p += oracle_c11_check_local(out, "CoolingGas_precap", l.cooling_precap);
    p += oracle_c11_check_flag(out, "reservoir_cap", l.cap);
    p += oracle_c11_check_local(out, "CoolingGas_postcap", coolingGas);
    if (l.cap == MIMIC_SAGE_CAP_RESERVOIR) {
      p += oracle_c11_check_binding(fixture, "CoolingGas_postcap", coolingGas, cgm_in);
    }
    agn_entered = (agn.recipe > 0 && coolingGas > 0.0) ? 1 : 0;
    if (agn_entered) {
      struct MimicSageAgnLocals al;
      coolingGas = mimic_sage_do_agn_heating_cgm(&halo, &MimicConfig, agn.recipe, agn.efficiency, 1,
                                                 dt, coolingGas, l.x, l.rcool, &al);
      p += oracle_c11_check_agn(fixture, out, &al, cgm_in);
      recapped = al.reservoir_positive ? al.recapped : 0;
      if (al.recapped) {
        p += oracle_c11_check_binding(fixture, "returned coolingGas", coolingGas, galaxy.CGMgas);
      }
      oracle_c11_exclude_flag(out, "accretion_capped");
      oracle_c11_exclude_flag(out, "heating_capped");
    }
    mimic_sage_cooling_energy(&galaxy, (float)halo.Vvir, coolingGas);
  } else {
    p += oracle_c11_check_flag(out, "reservoir_cap", -1);
  }
  galaxy.CoolingRate = mimic_sage_cooling_rate(coolingGas, dt, &MimicConfig);
  p += oracle_c11_check_flag(out, "agn_entered", agn_entered);
  p += oracle_c11_check_flag(out, "recapped", recapped);
  if (agn_entered != 1) {
    const char *const unevaluated[] = {
        "agn_suppressed",   "agn_saturated",  "agn_reservoir_positive", "eddington_capped",
        "accretion_capped", "heating_capped", "ratchet_raised",         "rheat_capped"};
    for (size_t k = 0; k < sizeof(unevaluated) / sizeof(unevaluated[0]); k++) {
      p += oracle_c11_check_flag(out, unevaluated[k], -1);
    }
  }
  p += oracle_check_f64(out, "return.coolingGas", coolingGas);
  p += oracle_c11_check_state(out, &galaxy);

  struct MimicSageCgmCoolingLocals cl;
  const double composed =
      mimic_sage_cooling_recipe_cgm(&composed_halo, &MimicConfig, &agn, 1, dt, &cl, NULL);
  if (oracle_f64_to_bits(composed) != oracle_f64_to_bits(coolingGas) ||
      !oracle_c11_same_state(&composed_galaxy, &galaxy)) {
    fprintf(stderr, "oracle: %s: mimic_sage_cooling_recipe_cgm differs from its steps\n", fixture);
    p++;
  }
  return p;
}

/** @brief One do_AGN_heating_cgm_on fixture */
static inline int oracle_c11_check_agn_alone(const char *fixture, struct oracle_fixture *in,
                                             struct oracle_fixture *out) {
  reset_config();
  const struct MimicSageAgnConfig agn = oracle_c11_set_params(in);
  const double cooling = oracle_in_f64(in, "call.coolingGas");
  const double x = oracle_in_f64(in, "call.x");
  const double rcool = oracle_in_f64(in, "call.rcool");
  struct GalaxyData galaxy;
  struct Halo halo;
  oracle_poison(&galaxy, sizeof(galaxy));
  oracle_poison(&halo, sizeof(halo));
  oracle_c11_in_galaxy(in, &halo, &galaxy);
  double dt = 0.0;
  int p = oracle_c11_dt(in, out, &halo, &dt);
  const double hot_in = galaxy.HotGas;
  struct MimicSageAgnLocals al;
  const double ret = mimic_sage_do_agn_heating_hot(&halo, &MimicConfig, agn.recipe, agn.efficiency,
                                                   1, dt, cooling, x, rcool, &al);
  p += oracle_c11_check_agn(fixture, out, &al, hot_in);
  if (al.reservoir_positive) {
    oracle_c11_exclude_flag(out, "accretion_capped");
    oracle_c11_exclude_flag(out, "heating_capped");
  } else {
    p += oracle_c11_check_flag(out, "accretion_capped", -1);
    p += oracle_c11_check_flag(out, "heating_capped", -1);
  }
  p += oracle_check_f64(out, "return.coolingGas", ret);
  p += oracle_c11_check_state(out, &galaxy);
  return p;
}

/**
 * @brief Check one CGM cooling fixture of a module, if its routine is a CGM cooling routine
 *
 * @param handled  Set to 1 when the fixture was a CGM cooling fixture (and checked), 0 otherwise
 * @return The number of mismatches and errors
 */
static inline int oracle_c11_check_fixture(const char *module, const char *name, int *handled) {
  static struct oracle_fixture in, out;
  char path[ORACLE_MAX_PATH];
  *handled = 0;
  oracle_fixture_path(path, sizeof(path), module, name, ".in");
  if (oracle_load(path, &in) != 0) {
    *handled = 1;
    return 1;
  }
  if (!oracle_c11_is_routine(in.routine)) {
    return 0;
  }
  *handled = 1;
  oracle_fixture_path(path, sizeof(path), module, name, ".out");
  if (oracle_load(path, &out) != 0) {
    return 1;
  }
  if (strcmp(in.routine, out.routine) != 0) {
    fprintf(stderr, "oracle: %s/%s: .in and .out name different routines\n", module, name);
    return 1;
  }
  int p = 0;
  if (strcmp(in.routine, "interpolate_clustering_mass") == 0) {
    p += oracle_c11_check_clustering(&in, &out);
  } else if (strcmp(in.routine, "cooling_recipe_hot_streams") == 0) {
    p += oracle_c11_check_hot(name, &in, &out);
  } else if (strcmp(in.routine, "cooling_recipe_cgm") == 0) {
    p += oracle_c11_check_cgm(name, &in, &out);
  } else {
    p += oracle_c11_check_agn_alone(name, &in, &out);
  }
  oracle_c11_counts.fixtures++;
  p += oracle_report_unconsumed(&in);
  p += oracle_report_unconsumed(&out);
  if (p != 0) {
    fprintf(stderr, "oracle: %s/%s: %d c11 mismatches\n", module, name, p);
  }
  return p;
}

/** @brief Print the run's CGM cooling tallies (MIMIC_RESULT-adjacent evidence) */
static inline void oracle_c11_report(const char *module) {
  printf("c11 oracle tally (%s): fixtures %d, locals %d (continuation-checked %d, tail-checked "
         "%d), validated two-sided %d, binding caps %d, flags %d\n",
         module, oracle_c11_counts.fixtures, oracle_c11_counts.locals,
         oracle_c11_counts.continuation, oracle_c11_counts.tail, oracle_c11_counts.validated,
         oracle_c11_counts.binding, oracle_c11_counts.flags);
}

#endif /* SAGE26_ORACLE_COOLING_H */
