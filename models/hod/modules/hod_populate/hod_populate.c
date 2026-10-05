/**
 * @file    hod_populate.c
 * @brief   Threshold-sample halo occupation distribution with a whole-box audit
 *
 * A dual-mode module. In modules.post_timestep (process_full_halo) it runs once
 * per FoF step: it retires every inherited Type 2 row to Type 3, marks every
 * row as scaffold (HODGhost = 1), and on output snapshots draws the FoF's
 * Type 0 host from the five-parameter Zheng et al. (2005) occupation law with
 * the central-gating convention: a present central clears the host's flag and
 * each Poisson satellite is created through module_create_record(), placed at
 * an NFW radius with a Duffy et al. (2008) concentration, an isotropic
 * direction converted from physical to comoving coordinates, a periodic wrap,
 * and a Gaussian velocity offset. In modules.post_snapshot (process_snapshot,
 * horizontal driver only) it audits the realised population against the
 * analytic expectation and writes nothing; its totals, bins and failure
 * agreement go through the snapshot collectives (snapshot_collectives.h), so a
 * distributed horizontal run audits the whole snapshot across tasks.
 *
 * Draws come from hod_random.h, keyed by (HODSeed, snapshot number, host
 * UniqueGalaxyID, draw index), so each host's realisation is independent of
 * row order, FoF order, traversal and partitioning.
 *
 * References: Zheng et al. (2005); Zheng, Coil & Zehavi (2007) equations 2
 * and 5; Duffy et al. (2008) Table 1; Berlind & Weinberg (2002).
 */

#include <float.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "globals.h"
#include "memory.h"
#include "module_interface.h"
#include "module_registry.h"
#include "snapshot_collectives.h"
#include "types.h"
#include "virial.h"

#include "module_system/parameter_helpers.h"

#include "shared/hod_random.h"

#include "hod_populate.h"

/** Configured name of this module */
#define HOD_MODULE_NAME "hod_populate"

/** Width of the audit's occupation bins in log10(M / (Msun/h)) */
#define HOD_AUDIT_BIN_DEX 0.2

/** Below this argument the NFW mass shape is summed as a series, avoiding cancellation */
#define HOD_NFW_SERIES_LIMIT 0.1

// ============================================================================
// MODULE STATE
// ============================================================================

/** Parameters loaded and validated by init() */
static struct HodParameters hod_params;

/** Whether init() completed; process() and process_snapshot() refuse to run otherwise */
static bool hod_ready = false;

/**
 * One host's satellite placements, drawn in full before process() writes
 * anything so a draw that fails leaves the workspace untouched (80 KB, static
 * because one FoF step is processed at a time)
 */
static struct HodSatellite hod_satellites[HOD_MAX_SATELLITES];

// ============================================================================
// OCCUPATION AND PLACEMENT
// ============================================================================

double hod_populate_mean_ncen(const struct HodParameters *p, double mass) {
  if (!(mass > 0.0)) {
    return 0.0;
  }
  return 0.5 * (1.0 + erf((log10(mass) - p->log_mmin) / p->sigma_logm));
}

void hod_populate_cache_powers(struct HodParameters *p) {
  p->mass_m0 = pow(10.0, p->log_m0);
  p->mass_m1 = pow(10.0, p->log_m1);
  p->mass_pivot = pow(10.0, p->conc_log_mpivot);
}

double hod_populate_lambda(const struct HodParameters *p, double mass) {
  if (!(mass > p->mass_m0)) {
    return 0.0;
  }
  return pow((mass - p->mass_m0) / p->mass_m1, p->alpha);
}

double hod_populate_concentration(const struct HodParameters *p, double mass, double redshift) {
  return p->conc_a * pow(mass / p->mass_pivot, p->conc_b) * pow(1.0 + redshift, p->conc_c);
}

/**
 * @brief m(t) / t^2 by its series sum_{n>=2} (-1)^n (n - 1)/n t^(n-2), for 0 <= t < 0.1
 *
 * Each term is at most a tenth of the one before, so the sum stops once the
 * next power of t is below 1e-17 (at most about 17 terms).
 */
static double nfw_scaled_mass_series(double t) {
  double sum = 0.0;
  double power = 1.0;
  for (int n = 2; n < 64 && power >= 1.0e-17; n++) {
    const double term = (double)(n - 1) / (double)n * power;
    sum += (n % 2 == 0) ? term : -term;
    power *= t;
  }
  return sum;
}

/** @brief m(t) = ln(1 + t) - t / (1 + t), from the series below HOD_NFW_SERIES_LIMIT */
static double nfw_mass(double t) {
  if (t < HOD_NFW_SERIES_LIMIT) {
    return t * t * nfw_scaled_mass_series(t);
  }
  return log1p(t) - t / (1.0 + t);
}

double hod_populate_nfw_fraction(double x, double c) {
  if (c < HOD_NFW_SERIES_LIMIT) {
    const double ratio = x / c;
    return ratio * ratio * nfw_scaled_mass_series(x) / nfw_scaled_mass_series(c);
  }
  return nfw_mass(x) / nfw_mass(c);
}

int hod_populate_nfw_inverse(double u, double c, double *x) {
  if (!isfinite(c) || !(c > 0.0) || !(u >= 0.0 && u <= 1.0)) {
    return -1;
  }
  const double denominator = (c < HOD_NFW_SERIES_LIMIT) ? nfw_scaled_mass_series(c) : nfw_mass(c);
  if (!isfinite(denominator) || !(denominator > 0.0)) {
    return -1;
  }
  double lo = 0.0;
  double hi = c;
  // Each step halves the bracket; it stops shrinking after about 1100 steps at the latest.
  for (int iteration = 0; iteration < 2000; iteration++) {
    // lo + (hi - lo) / 2 stays finite for hi up to DBL_MAX, where lo + hi overflows.
    const double mid = lo + 0.5 * (hi - lo);
    const double residual = hod_populate_nfw_fraction(mid, c) - u;
    if (fabs(residual) <= HOD_NFW_TOLERANCE) {
      *x = mid;
      return 0;
    }
    if (mid <= lo || mid >= hi || !isfinite(residual)) {
      return -1;
    }
    if (residual < 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return -1;
}

double hod_populate_wrap(double coordinate, double box) {
  double wrapped = fmod(coordinate, box);
  if (wrapped < 0.0) {
    wrapped += box;
  }
  // A tiny negative remainder plus box can round to box itself; that point is 0.
  if (wrapped >= box) {
    wrapped = 0.0;
  }
  return wrapped;
}

float hod_populate_store_position(double wrapped, double box) {
  float stored = (float)wrapped;
  while ((double)stored >= box) {
    stored = nextafterf(stored, 0.0f);
  }
  return stored;
}

void hod_populate_draw_occupation(const struct HodParameters *p, uint64_t key, double mass,
                                  struct HodOccupation *out) {
  out->mean_ncen = hod_populate_mean_ncen(p, mass);
  out->lambda = hod_populate_lambda(p, mass);
  out->central = hod_random_uniform(key, HOD_DRAW_CENTRAL) < out->mean_ncen;
  out->num_satellites = 0;
  if (out->central && out->lambda > 0.0) {
    const double u = hod_random_uniform(key, HOD_DRAW_SATELLITE_COUNT);
    out->num_satellites = hod_random_poisson(out->lambda, u, HOD_MAX_SATELLITES);
  }
}

int hod_populate_draw_satellite(uint64_t key, int s, double concentration, double rvir, double vvir,
                                double redshift, struct HodSatellite *out) {
  const uint64_t base =
      HOD_DRAW_FIRST_SATELLITE + (uint64_t)s * (uint64_t)HOD_DRAW_SATELLITE_STRIDE;

  out->u_radius = hod_random_uniform(key, base);
  if (hod_populate_nfw_inverse(out->u_radius, concentration, &out->x) != 0) {
    return -1;
  }
  // The ratio x / c lies in [0, 1]; forming rvir * x first overflows for x near DBL_MAX.
  out->r_phys = rvir * (out->x / concentration);
  out->r_com = out->r_phys * (1.0 + redshift);

  const double cos_theta = 2.0 * hod_random_uniform(key, base + 1) - 1.0;
  const double sin_theta = sqrt(fmax(0.0, 1.0 - cos_theta * cos_theta));
  const double phi = HOD_RANDOM_TWO_PI * hod_random_uniform(key, base + 2);
  out->offset[0] = out->r_com * sin_theta * cos(phi);
  out->offset[1] = out->r_com * sin_theta * sin(phi);
  out->offset[2] = out->r_com * cos_theta;

  const double sigma_1d = vvir / sqrt(2.0);
  for (int j = 0; j < 3; j++) {
    out->velocity[j] = sigma_1d * hod_random_gaussian(key, base + 3 + 2 * (uint64_t)j);
  }
  return 0;
}

// ============================================================================
// CONFIGURATION AND PARAMETERS
// ============================================================================

/**
 * @brief Require the module exactly once in post_timestep as process_full_halo, in no
 *        other FoF phase, and, when post_snapshot is configured, the snapshot audit there
 *
 * FoF phases accept repeated entries (the registry initialises a module once),
 * so entries are counted here. Any further FoF entry would retire and redraw
 * the same FoF step again (per substep in a substep phase), and could exhaust
 * the per-host identity radix with no hint of the cause.
 */
static int check_configuration(void) {
  int elsewhere =
      module_count_phase_entries(HOD_MODULE_NAME, MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO, NULL);
  if (elsewhere > 0) {
    ERROR_LOG(
        "%s is configured in modules.pre_timestep; it must run only in modules.post_timestep, "
        "once per FoF step, or each extra entry retires and redraws the step again",
        HOD_MODULE_NAME);
    return -1;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    elsewhere = module_count_phase_entries(HOD_MODULE_NAME, phase->modules, phase->num_modules,
                                           PROCESSING_MODE_FULL_HALO, NULL);
    if (elsewhere > 0) {
      ERROR_LOG("%s is configured in substep phase '%s'; it must run only in "
                "modules.post_timestep, once per FoF step, or it retires and redraws the step "
                "once per substep",
                HOD_MODULE_NAME, phase->name != NULL ? phase->name : "(unnamed)");
      return -1;
    }
  }

  int full_halo_entries = 0;
  const int entries = module_count_phase_entries(HOD_MODULE_NAME, MimicConfig.post_timestep,
                                                 MimicConfig.num_post_timestep,
                                                 PROCESSING_MODE_FULL_HALO, &full_halo_entries);
  if (entries != 1 || full_halo_entries != 1) {
    ERROR_LOG("%s must be configured exactly once in modules.post_timestep as process_full_halo "
              "(found %d entries, %d as process_full_halo)",
              HOD_MODULE_NAME, entries, full_halo_entries);
    return -1;
  }

  // The registry rejects duplicate and non-snapshot post_snapshot entries before init().
  if (MimicConfig.num_post_snapshot > 0 &&
      !module_configured_in_phase(HOD_MODULE_NAME, MimicConfig.post_snapshot,
                                  MimicConfig.num_post_snapshot, PROCESSING_MODE_SNAPSHOT)) {
    ERROR_LOG("%s: modules.post_snapshot is configured but does not contain %s as "
              "process_snapshot; add it there or remove the phase",
              HOD_MODULE_NAME, HOD_MODULE_NAME);
    return -1;
  }
  return 0;
}

/** Domain a finite double parameter must also satisfy */
enum HodDomain {
  HOD_DOMAIN_ANY,         /**< Any finite value */
  HOD_DOMAIN_POSITIVE,    /**< Strictly positive */
  HOD_DOMAIN_NONNEGATIVE, /**< Zero or positive */
  HOD_DOMAIN_LOG_MASS,    /**< log10 of a mass whose power of ten is a finite positive normal */
};

/** One double parameter's validation: its name, loaded value, domain and meaning for errors */
struct HodDoubleCheck {
  const char *name;
  const double *value;
  enum HodDomain domain;
  const char *meaning; /**< Quoted by the positive and nonnegative errors, else unused */
};

/**
 * @brief Fail a double parameter that is not finite or outside its domain
 *
 * The strict parser accepts "nan" and "inf" and range comparisons pass NaN, so
 * finiteness is checked before the domain.
 */
static int check_double_parameter(const struct HodDoubleCheck *check) {
  const double value = *check->value;
  if (!isfinite(value)) {
    ERROR_LOG("%s = %g is not finite", check->name, value);
    return -1;
  }
  switch (check->domain) {
  case HOD_DOMAIN_ANY:
    break;
  case HOD_DOMAIN_POSITIVE:
    if (!(value > 0.0)) {
      ERROR_LOG("%s = %g must be > 0 (%s)", check->name, value, check->meaning);
      return -1;
    }
    break;
  case HOD_DOMAIN_NONNEGATIVE:
    if (value < 0.0) {
      ERROR_LOG("%s = %g must be >= 0 (%s)", check->name, value, check->meaning);
      return -1;
    }
    break;
  case HOD_DOMAIN_LOG_MASS: {
    const double mass = pow(10.0, value);
    if (!isfinite(mass) || mass < DBL_MIN) {
      ERROR_LOG("%s = %g gives 10^%g = %g Msun/h, which is not a finite positive normal double",
                check->name, value, value, mass);
      return -1;
    }
    break;
  }
  }
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int hod_populate_init(void) {
  hod_ready = false;

  if (check_configuration() != 0) {
    return -1;
  }

  struct HodParameters p;
  memset(&p, 0, sizeof(p));

  // The names stay literal at each load so scripts/lint_parameter_usage.py can match them
  // against module_info.yaml; the table below carries only the validation.
  LOAD_PARAM_DOUBLE("HODLogMmin", p.log_mmin);
  LOAD_PARAM_DOUBLE("HODSigmaLogM", p.sigma_logm);
  LOAD_PARAM_DOUBLE("HODLogM0", p.log_m0);
  LOAD_PARAM_DOUBLE("HODLogM1", p.log_m1);
  LOAD_PARAM_DOUBLE("HODAlpha", p.alpha);
  LOAD_PARAM_INT("HODSeed", p.seed);
  LOAD_PARAM_DOUBLE("HODConcA", p.conc_a);
  LOAD_PARAM_DOUBLE("HODConcLogMpivot", p.conc_log_mpivot);
  LOAD_PARAM_DOUBLE("HODConcB", p.conc_b);
  LOAD_PARAM_DOUBLE("HODConcC", p.conc_c);

  const struct HodDoubleCheck checks[] = {
      {"HODLogMmin", &p.log_mmin, HOD_DOMAIN_ANY, NULL},
      {"HODSigmaLogM", &p.sigma_logm, HOD_DOMAIN_POSITIVE,
       "dex width of the central occupation step"},
      {"HODLogM0", &p.log_m0, HOD_DOMAIN_LOG_MASS, NULL},
      {"HODLogM1", &p.log_m1, HOD_DOMAIN_LOG_MASS, NULL},
      {"HODAlpha", &p.alpha, HOD_DOMAIN_NONNEGATIVE, "satellite occupation slope"},
      {"HODConcA", &p.conc_a, HOD_DOMAIN_POSITIVE, "concentration normalisation"},
      {"HODConcLogMpivot", &p.conc_log_mpivot, HOD_DOMAIN_LOG_MASS, NULL},
      {"HODConcB", &p.conc_b, HOD_DOMAIN_ANY, NULL},
      {"HODConcC", &p.conc_c, HOD_DOMAIN_ANY, NULL},
  };
  for (size_t k = 0; k < sizeof(checks) / sizeof(checks[0]); k++) {
    if (check_double_parameter(&checks[k]) != 0) {
      return -1;
    }
  }
  if (p.seed < 0) {
    ERROR_LOG("HODSeed = %d must be >= 0", p.seed);
    return -1;
  }
  hod_populate_cache_powers(&p);

  p.box_size = MimicConfig.BoxSize;
  const double volume = p.box_size * p.box_size * p.box_size;
  if (!isfinite(p.box_size) || p.box_size <= 0.0 || !isfinite(volume) || volume <= 0.0) {
    ERROR_LOG("%s needs a finite positive BoxSize with a finite positive volume (BoxSize=%g "
              "Mpc/h, volume=%g (Mpc/h)^3)",
              HOD_MODULE_NAME, p.box_size, volume);
    return -1;
  }

  hod_params = p;
  hod_ready = true;
  INFO_LOG("HOD populate initialized: log10 Mmin=%.6g sigma_logM=%.6g log10 M0=%.6g "
           "log10 M1=%.6g alpha=%.6g seed=%d; concentration A=%.6g log10 Mpivot=%.6g B=%.6g "
           "C=%.6g; BoxSize=%.6g Mpc/h; audit %s",
           p.log_mmin, p.sigma_logm, p.log_m0, p.log_m1, p.alpha, p.seed, p.conc_a,
           p.conc_log_mpivot, p.conc_b, p.conc_c, p.box_size,
           MimicConfig.num_post_snapshot > 0 ? "on" : "off (no post_snapshot phase)");
  return 0;
}

/** @brief Whether a drawn placement is finite in every field the module writes from */
static bool satellite_is_finite(const struct HodSatellite *sat) {
  if (!isfinite(sat->r_phys) || !isfinite(sat->r_com)) {
    return false;
  }
  for (int j = 0; j < 3; j++) {
    if (!isfinite(sat->offset[j]) || !isfinite(sat->velocity[j])) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Validate a host and draw it in full before anything is written
 *
 * Fills the occupation and, for each satellite, its placement in
 * hod_satellites[]; any failure (bad proxies, identity radix, concentration,
 * an NFW radius that cannot meet its tolerance, a non-finite placement)
 * returns -1 with nothing written.
 *
 * The placement scales with the host's current virial radius and velocity,
 * from its current Mvir at the draw redshift, not with the host row's own
 * Rvir and Vvir: inheritance keeps those at the largest value the branch ever
 * had. When there are satellites, the values used are returned in *rvir and
 * *vvir for the caller to write onto each created row.
 */
static int prepare_host_draw(const struct ModuleContext *ctx, const struct Halo *host,
                             struct HodOccupation *occupation, double *rvir, double *vvir) {
  const double mass = host->Mvir * HOD_MASS_UNIT_MSUN;
  if (!isfinite(mass) || mass < 0.0) {
    ERROR_LOG("%s: host UniqueGalaxyID %lld has Mvir %.17g; it must be finite and nonnegative",
              HOD_MODULE_NAME, host->UniqueGalaxyID, host->Mvir);
    return -1;
  }
  const double lambda = hod_populate_lambda(&hod_params, mass);
  if (!(lambda < HOD_MAX_SATELLITES)) {
    ERROR_LOG("%s: host UniqueGalaxyID %lld (M=%.6e Msun/h) has satellite mean lambda=%.6g, "
              "not below the per-host identity radix %d",
              HOD_MODULE_NAME, host->UniqueGalaxyID, mass, lambda, HOD_MAX_SATELLITES);
    return -1;
  }

  const uint64_t key = hod_random_key(hod_params.seed, ctx->snapshot_number, host->UniqueGalaxyID);
  hod_populate_draw_occupation(&hod_params, key, mass, occupation);
  if (occupation->num_satellites > HOD_MAX_SATELLITES) {
    ERROR_LOG("%s: host UniqueGalaxyID %lld (lambda=%.6g) drew more than %d satellites, the "
              "per-host identity radix",
              HOD_MODULE_NAME, host->UniqueGalaxyID, occupation->lambda, HOD_MAX_SATELLITES);
    return -1;
  }
  if (occupation->num_satellites == 0) {
    return 0;
  }

  for (int j = 0; j < 3; j++) {
    if (!isfinite(host->Pos[j]) || !isfinite(host->Vel[j])) {
      ERROR_LOG("%s: host UniqueGalaxyID %lld has a non-finite position or velocity",
                HOD_MODULE_NAME, host->UniqueGalaxyID);
      return -1;
    }
  }
  if (!isfinite(ctx->redshift) || ctx->redshift <= -1.0) {
    ERROR_LOG("%s: snapshot %d redshift %.17g is not a finite value above -1", HOD_MODULE_NAME,
              ctx->snapshot_number, ctx->redshift);
    return -1;
  }
  *rvir = virial_radius_for_mass(host->Mvir, ctx->redshift);
  *vvir = virial_velocity_for(host->Mvir, *rvir);
  if (!isfinite(*rvir) || !(*rvir > 0.0) || !isfinite(*vvir) || !(*vvir > 0.0)) {
    ERROR_LOG("%s: host UniqueGalaxyID %lld (Mvir=%.6e, z=%.4f) has virial radius %.17g Mpc/h and "
              "velocity %.17g km/s; both must be finite and positive to place satellites",
              HOD_MODULE_NAME, host->UniqueGalaxyID, host->Mvir, ctx->redshift, *rvir, *vvir);
    return -1;
  }
  const double concentration = hod_populate_concentration(&hod_params, mass, ctx->redshift);
  if (!isfinite(concentration) || !(concentration > 0.0)) {
    ERROR_LOG("%s: host UniqueGalaxyID %lld (M=%.6e Msun/h, z=%.4f) has concentration %.6g; it "
              "must be finite and positive",
              HOD_MODULE_NAME, host->UniqueGalaxyID, mass, ctx->redshift, concentration);
    return -1;
  }
  for (int s = 0; s < occupation->num_satellites; s++) {
    if (hod_populate_draw_satellite(key, s, concentration, *rvir, *vvir, ctx->redshift,
                                    &hod_satellites[s]) != 0) {
      ERROR_LOG("%s: host UniqueGalaxyID %lld satellite %d: the NFW radius for u=%.17g at "
                "concentration c=%.17g cannot meet |m(x)/m(c) - u| <= %g",
                HOD_MODULE_NAME, host->UniqueGalaxyID, s, hod_satellites[s].u_radius, concentration,
                HOD_NFW_TOLERANCE);
      return -1;
    }
    if (!satellite_is_finite(&hod_satellites[s])) {
      ERROR_LOG("%s: host UniqueGalaxyID %lld satellite %d at concentration c=%.17g has a "
                "non-finite radius, offset or velocity (r_phys=%.17g, r_com=%.17g)",
                HOD_MODULE_NAME, host->UniqueGalaxyID, s, concentration, hod_satellites[s].r_phys,
                hod_satellites[s].r_com);
      return -1;
    }
  }
  return 0;
}

int hod_populate_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ctx == NULL || !hod_ready) {
    ERROR_LOG("%s: process called %s", HOD_MODULE_NAME,
              ctx == NULL ? "without a module context" : "before a successful init()");
    return -1;
  }
  if (ngal < 1 || halos == NULL) {
    ERROR_LOG("%s: process needs a FoF workspace with at least one row (ngal=%d, halos=%p)",
              HOD_MODULE_NAME, ngal, (void *)halos);
    return -1;
  }
  const int host_index = ctx->central_index;
  if (host_index < 0 || host_index >= ngal) {
    ERROR_LOG("%s: central_index %d is outside the FoF workspace [0, %d)", HOD_MODULE_NAME,
              host_index, ngal);
    return -1;
  }
  struct Halo *host = &halos[host_index];
  if (host->Type != 0 || host->galaxy == NULL) {
    ERROR_LOG("%s: the FoF host at central_index %d (UniqueGalaxyID %lld) must be a Type 0 row "
              "with a galaxy (Type %d, galaxy %s)",
              HOD_MODULE_NAME, host_index, host->UniqueGalaxyID, host->Type,
              host->galaxy == NULL ? "NULL" : "present");
    return -1;
  }
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL && halos[i].Type != 3) {
      ERROR_LOG("%s: workspace row %d (UniqueGalaxyID %lld, Type %d) has no galaxy",
                HOD_MODULE_NAME, i, halos[i].UniqueGalaxyID, halos[i].Type);
      return -1;
    }
  }

  // Everything that can fail is decided before the first write.
  const bool draw = mimic_is_output_snapshot(ctx->snapshot_number);
  struct HodOccupation occupation = {0};
  double host_rvir = 0.0;
  double host_vvir = 0.0;
  if (draw && prepare_host_draw(ctx, host, &occupation, &host_rvir, &host_vvir) != 0) {
    return -1;
  }

  // Inherited orphans, including last snapshot's satellites, leave the sample; nothing is kept.
  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type == 2) {
      halos[i].Type = 3;
    }
    if (halos[i].galaxy != NULL) {
      halos[i].galaxy->HODGhost = 1;
    }
  }
  if (!draw) {
    return 0;
  }

  if (occupation.central) {
    host->galaxy->HODGhost = 0;
  }
  for (int s = 0; s < occupation.num_satellites; s++) {
    struct Halo *row = NULL;
    if (module_create_record(ctx, host_index, &row) < 0) {
      ERROR_LOG("%s: could not create satellite %d of %d for host UniqueGalaxyID %lld",
                HOD_MODULE_NAME, s, occupation.num_satellites, host->UniqueGalaxyID);
      return -1;
    }
    const struct HodSatellite *satellite = &hod_satellites[s];
    for (int j = 0; j < 3; j++) {
      const double wrapped =
          hod_populate_wrap((double)host->Pos[j] + satellite->offset[j], hod_params.box_size);
      row->Pos[j] = hod_populate_store_position(wrapped, hod_params.box_size);
      row->Vel[j] = (float)((double)host->Vel[j] + satellite->velocity[j]);
    }
    // The row carries the virial values the placement used: the output keeps a Type 2 row's own.
    row->Rvir = host_rvir;
    row->Vvir = host_vvir;
    row->galaxy->HODGhost = 0;
  }
  return 0;
}

// ============================================================================
// SNAPSHOT AUDIT
// ============================================================================

/** One Type 0 host in the audit's ID-sorted lookup table */
struct HodAuditHost {
  long long id;
  int bin; /**< Occupation bin, or -1 for a host with no positive mass */
};

static int compare_audit_hosts(const void *a, const void *b) {
  const long long id_a = ((const struct HodAuditHost *)a)->id;
  const long long id_b = ((const struct HodAuditHost *)b)->id;
  return (id_a > id_b) - (id_a < id_b);
}

/** Totals of the audit's first pass over the population */
struct HodAuditTotals {
  int64_t hosts;        /**< Type 0 rows */
  int64_t realised_all; /**< Sample members (HODGhost == 0) */
  int64_t realised_sat; /**< Sample members that are Type 2 satellites */
  double expected_all;  /**< Sum over Type 0 rows of <Ncen> (1 + lambda) */
  double expected_sat;  /**< Sum over Type 0 rows of <Ncen> lambda */
  double log_min;       /**< Smallest log10(M) over hosts with a positive mass; HUGE_VAL if none */
  double log_max;       /**< Largest log10(M) over hosts with a positive mass; -HUGE_VAL if none */
};

/** Per-bin sums of the audit: hosts, expected and realised galaxies */
struct HodAuditBins {
  int count;
  double log_lo;     /**< Lower edge of bin 0, a multiple of HOD_AUDIT_BIN_DEX */
  int64_t *hosts;    /**< [count]; owns the counts block that `realised` continues */
  double *expected;  /**< [count]; its own allocation */
  int64_t *realised; /**< [count]; the second half of the `hosts` block */
};

/** @brief Allocate zeroed bins: one counts block (hosts, then realised) and the expected sums */
static void audit_bins_allocate(struct HodAuditBins *bins, int count, double log_lo) {
  const size_t entries = (size_t)count;
  bins->count = count;
  bins->log_lo = log_lo;
  bins->hosts = mymalloc_cat(2 * entries * sizeof(int64_t), MEM_UTILITY);
  memset(bins->hosts, 0, 2 * entries * sizeof(int64_t));
  bins->realised = bins->hosts + entries;
  bins->expected = mymalloc_cat(entries * sizeof(double), MEM_UTILITY);
  memset(bins->expected, 0, entries * sizeof(double));
}

/** @brief Release the bins' allocations; safe on bins that were never allocated */
static void audit_bins_release(struct HodAuditBins *bins) {
  if (bins->hosts != NULL) {
    myfree(bins->hosts);
    myfree(bins->expected);
  }
  *bins = (struct HodAuditBins){0};
}

static int audit_bin(const struct HodAuditBins *bins, double mass) {
  if (!(mass > 0.0)) {
    return -1;
  }
  int bin = (int)floor((log10(mass) - bins->log_lo) / HOD_AUDIT_BIN_DEX);
  if (bin < 0) {
    bin = 0;
  }
  if (bin >= bins->count) {
    bin = bins->count - 1;
  }
  return bin;
}

/**
 * @brief Validate the population and total the expectation and the sample
 *
 * Pass 1 of the audit: every row must carry a galaxy and a binary ghost flag,
 * a Type 0 row must have a finite nonnegative mass, and a sample member must be
 * a Type 0 central or a Type 2 satellite.
 */
static int audit_scan(const struct Halo *halos, int64_t count, struct HodAuditTotals *totals) {
  *totals = (struct HodAuditTotals){.log_min = HUGE_VAL, .log_max = -HUGE_VAL};
  for (int64_t i = 0; i < count; i++) {
    const struct Halo *h = &halos[i];
    if (h->galaxy == NULL) {
      ERROR_LOG("%s audit: UniqueGalaxyID %lld (entry %lld) has no galaxy; a snapshot population "
                "row always carries one",
                HOD_MODULE_NAME, h->UniqueGalaxyID, (long long)i);
      return -1;
    }
    const int ghost = h->galaxy->HODGhost;
    if (ghost != 0 && ghost != 1) {
      ERROR_LOG("%s audit: UniqueGalaxyID %lld has HODGhost %d outside [0, 1]", HOD_MODULE_NAME,
                h->UniqueGalaxyID, ghost);
      return -1;
    }
    if (h->Type == 0) {
      const double mass = h->Mvir * HOD_MASS_UNIT_MSUN;
      if (!isfinite(mass) || mass < 0.0) {
        ERROR_LOG("%s audit: host UniqueGalaxyID %lld has Mvir %.17g; it must be finite and "
                  "nonnegative",
                  HOD_MODULE_NAME, h->UniqueGalaxyID, h->Mvir);
        return -1;
      }
      const double ncen = hod_populate_mean_ncen(&hod_params, mass);
      const double lambda = hod_populate_lambda(&hod_params, mass);
      totals->hosts++;
      totals->expected_all += ncen + ncen * lambda;
      totals->expected_sat += ncen * lambda;
      if (mass > 0.0) {
        totals->log_min = fmin(totals->log_min, log10(mass));
        totals->log_max = fmax(totals->log_max, log10(mass));
      }
    }
    if (ghost == 0) {
      if (h->Type == 2) {
        totals->realised_sat++;
      } else if (h->Type != 0) {
        ERROR_LOG("%s audit: UniqueGalaxyID %lld is a sample member of Type %d; only Type 0 "
                  "centrals and Type 2 created satellites can be",
                  HOD_MODULE_NAME, h->UniqueGalaxyID, h->Type);
        return -1;
      }
      totals->realised_all++;
    }
  }
  return 0;
}

/**
 * @brief Attribute every sample galaxy to its host's occupation bin
 *
 * Pass 2 of the audit. Every row, a central and a satellite alike, is attributed
 * through its UniqueCentralGalaxyID (core sets it to the Type 0 row's own
 * UniqueGalaxyID for the central), looked up in an ID-sorted scratch table of
 * the Type 0 rows. A sample row with no such host breaks the module's own
 * contract and fails the audit. The table holds @p hosts entries; meeting more
 * Type 0 rows than that fails the audit rather than writing past it.
 */
static int fill_audit_bins(const struct Halo *halos, int64_t count, int64_t hosts,
                           struct HodAuditBins *bins) {
  struct HodAuditHost *table =
      mymalloc_cat((size_t)hosts * sizeof(struct HodAuditHost), MEM_UTILITY);
  int64_t n = 0;
  for (int64_t i = 0; i < count; i++) {
    if (halos[i].Type != 0) {
      continue;
    }
    if (n >= hosts) {
      ERROR_LOG("%s audit: more Type 0 rows than the %lld the host table was sized for",
                HOD_MODULE_NAME, (long long)hosts);
      myfree(table);
      return -1;
    }
    const double mass = halos[i].Mvir * HOD_MASS_UNIT_MSUN;
    const int bin = audit_bin(bins, mass);
    table[n].id = halos[i].UniqueGalaxyID;
    table[n].bin = bin;
    n++;
    if (bin >= 0) {
      const double ncen = hod_populate_mean_ncen(&hod_params, mass);
      bins->hosts[bin]++;
      bins->expected[bin] += ncen * (1.0 + hod_populate_lambda(&hod_params, mass));
    }
  }
  qsort(table, (size_t)n, sizeof(*table), compare_audit_hosts);

  int status = 0;
  // The caller's first pass has already checked that every entry has a galaxy.
  for (int64_t i = 0; i < count && status == 0; i++) {
    if (halos[i].galaxy->HODGhost != 0) {
      continue;
    }
    const struct HodAuditHost key = {halos[i].UniqueCentralGalaxyID, 0};
    const struct HodAuditHost *found =
        bsearch(&key, table, (size_t)n, sizeof(*table), compare_audit_hosts);
    if (found == NULL || found->bin < 0) {
      ERROR_LOG("%s audit: sample row UniqueGalaxyID %lld (Type %d) has no Type 0 host with a "
                "positive mass under UniqueCentralGalaxyID %lld in this snapshot",
                HOD_MODULE_NAME, halos[i].UniqueGalaxyID, halos[i].Type,
                halos[i].UniqueCentralGalaxyID);
      status = -1;
    } else {
      bins->realised[found->bin]++;
    }
  }
  myfree(table);
  return status;
}

/** @brief Log the audit summary line and, with --verbose, one line per occupied bin */
static void log_audit(const struct SnapshotContext *ctx, const struct HodAuditTotals *totals,
                      const struct HodAuditBins *bins) {
  const double volume = hod_params.box_size * hod_params.box_size * hod_params.box_size;
  const double fsat_expected =
      (totals->expected_all > 0.0) ? totals->expected_sat / totals->expected_all : 0.0;
  const double fsat_realised = (totals->realised_all > 0)
                                   ? (double)totals->realised_sat / (double)totals->realised_all
                                   : 0.0;
  INFO_LOG("HOD audit z=%.4f hosts=%lld n_gal expected=%.6e realised=%.6e f_sat expected=%.6f "
           "realised=%.6f",
           ctx->redshift, (long long)totals->hosts, totals->expected_all / volume,
           (double)totals->realised_all / volume, fsat_expected, fsat_realised);
  for (int b = 0; b < bins->count; b++) {
    if (bins->hosts[b] == 0) {
      continue;
    }
    const double lo = bins->log_lo + b * HOD_AUDIT_BIN_DEX;
    VERBOSE_LOG("HOD audit bin z=%.4f log10M=[%.2f, %.2f) hosts=%lld <N> expected=%.6f "
                "realised=%.6f",
                ctx->redshift, lo, lo + HOD_AUDIT_BIN_DEX, (long long)bins->hosts[b],
                bins->expected[b] / (double)bins->hosts[b],
                (double)bins->realised[b] / (double)bins->hosts[b]);
  }
}

/**
 * Collectives, in the order every task reaches them on an output snapshot:
 * module_snapshot_min_max_f64() on the host log-mass extent (before the bin
 * layout is fixed), module_snapshot_sum_i64() on hosts[] and realised[] and then
 * on the scalar counts, module_snapshot_sum_f64() on expected[] and then on the
 * scalar sums, and module_snapshot_any() on the failure flag. A local failure
 * sets the flag instead of returning, so no call depends on a local count; the
 * bin layout depends only on the reduced extent, so every task passes the same
 * `n`.
 */
int hod_populate_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                                  int64_t count) {
  if (ctx == NULL || !hod_ready) {
    ERROR_LOG("%s: process_snapshot called %s", HOD_MODULE_NAME,
              ctx == NULL ? "without a snapshot context" : "before a successful init()");
    return -1;
  }
  if (!mimic_is_output_snapshot(ctx->snapshot_number)) {
    return 0;
  }

  int failed = 0;
  int collective_failed = 0;
  if (count < 0 || (count > 0 && halos == NULL)) {
    ERROR_LOG("%s audit: invalid population (halos=%p, count=%lld)", HOD_MODULE_NAME,
              (const void *)halos, (long long)count);
    failed = 1;
  }
  // A task whose population is unusable still takes part, with nothing to audit.
  const int64_t local_count = failed ? 0 : count;

  struct HodAuditTotals totals;
  if (audit_scan(halos, local_count, &totals) != 0) {
    failed = 1;
  }

  // The extent is seeded with +/-HUGE_VAL and folded with fmin/fmax, so it is NaN-free.
  if (module_snapshot_min_max_f64(ctx, &totals.log_min, &totals.log_max, 1) != 0) {
    collective_failed = 1;
  }

  // <N(M)> realised versus expected in HOD_AUDIT_BIN_DEX bins over the host mass range.
  struct HodAuditBins bins = {0};
  if (!collective_failed && totals.log_max >= totals.log_min) {
    const double log_lo = floor(totals.log_min / HOD_AUDIT_BIN_DEX) * HOD_AUDIT_BIN_DEX;
    audit_bins_allocate(&bins, (int)floor((totals.log_max - log_lo) / HOD_AUDIT_BIN_DEX) + 1,
                        log_lo);
  }
  // After a failed extent collective the layout is empty on every task; it is reported below.
  if (!failed && !collective_failed) {
    if (totals.realised_all > 0 && bins.count == 0) {
      ERROR_LOG("%s audit: %lld sample rows but no Type 0 host with a positive mass",
                HOD_MODULE_NAME, (long long)totals.realised_all);
      failed = 1;
    } else if (bins.count > 0 && fill_audit_bins(halos, local_count, totals.hosts, &bins) != 0) {
      failed = 1;
    }
  }

  // hosts[] and realised[] are one contiguous block of 2 * bins.count counts.
  int64_t counts[3] = {totals.hosts, totals.realised_all, totals.realised_sat};
  double sums[2] = {totals.expected_all, totals.expected_sat};
  // Separate statements, not ||: every call is made even after one has failed.
  if (module_snapshot_sum_i64(ctx, bins.hosts, 2 * bins.count) != 0) {
    collective_failed = 1;
  }
  if (module_snapshot_sum_i64(ctx, counts, 3) != 0) {
    collective_failed = 1;
  }
  if (module_snapshot_sum_f64(ctx, bins.expected, bins.count) != 0) {
    collective_failed = 1;
  }
  if (module_snapshot_sum_f64(ctx, sums, 2) != 0) {
    collective_failed = 1;
  }

  const int any_failed = module_snapshot_any(ctx, failed || collective_failed);
  if (any_failed == 0) {
    totals.hosts = counts[0];
    totals.realised_all = counts[1];
    totals.realised_sat = counts[2];
    totals.expected_all = sums[0];
    totals.expected_sat = sums[1];
    if (module_snapshot_is_root_task()) {
      log_audit(ctx, &totals, &bins);
    }
  } else if (!failed) {
    ERROR_LOG("%s audit: snapshot %d (z=%.4f) failed on another task or in a snapshot collective",
              HOD_MODULE_NAME, ctx->snapshot_number, ctx->redshift);
  }
  audit_bins_release(&bins);
  return any_failed == 0 ? 0 : -1;
}

int hod_populate_cleanup(void) {
  hod_ready = false;
  VERBOSE_LOG("HOD populate cleaned up");
  return 0;
}
