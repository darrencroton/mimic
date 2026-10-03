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
 * analytic expectation and writes nothing.
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
#include "types.h"

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

double hod_populate_lambda(const struct HodParameters *p, double mass) {
  const double m0 = pow(10.0, p->log_m0);
  if (!(mass > m0)) {
    return 0.0;
  }
  return pow((mass - m0) / pow(10.0, p->log_m1), p->alpha);
}

double hod_populate_concentration(const struct HodParameters *p, double mass, double redshift) {
  return p->conc_a * pow(mass / pow(10.0, p->conc_log_mpivot), p->conc_b) *
         pow(1.0 + redshift, p->conc_c);
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
    const double mid = 0.5 * (lo + hi);
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

bool hod_populate_is_output_snapshot(int snapshot) {
  if (MimicConfig.NOUT == 0) {
    return true;
  }
  for (int n = 0; n < MimicConfig.NOUT; n++) {
    if (MimicConfig.ListOutputSnaps[n] == snapshot) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// CONFIGURATION AND PARAMETERS
// ============================================================================

/** Entries naming this module in one FoF phase */
static int count_fof_entries(const struct PhaseModuleConfig *phase, int num_modules) {
  int entries = 0;
  for (int i = 0; i < num_modules; i++) {
    if (phase[i].module_name != NULL && strcmp(phase[i].module_name, HOD_MODULE_NAME) == 0) {
      entries++;
    }
  }
  return entries;
}

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
  int elsewhere = count_fof_entries(MimicConfig.pre_timestep, MimicConfig.num_pre_timestep);
  if (elsewhere > 0) {
    ERROR_LOG(
        "%s is configured in modules.pre_timestep; it must run only in modules.post_timestep, "
        "once per FoF step, or each extra entry retires and redraws the step again",
        HOD_MODULE_NAME);
    return -1;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    elsewhere = count_fof_entries(phase->modules, phase->num_modules);
    if (elsewhere > 0) {
      ERROR_LOG("%s is configured in substep phase '%s'; it must run only in "
                "modules.post_timestep, once per FoF step, or it retires and redraws the step "
                "once per substep",
                HOD_MODULE_NAME, phase->name != NULL ? phase->name : "(unnamed)");
      return -1;
    }
  }

  int entries = 0;
  int full_halo_entries = 0;
  for (int i = 0; i < MimicConfig.num_post_timestep; i++) {
    const struct PhaseModuleConfig *entry = &MimicConfig.post_timestep[i];
    if (entry->module_name != NULL && strcmp(entry->module_name, HOD_MODULE_NAME) == 0) {
      entries++;
      full_halo_entries += (entry->processing_mode == PROCESSING_MODE_FULL_HALO);
    }
  }
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

/**
 * @brief Fail a parameter that is NaN or infinite
 *
 * The strict parser accepts "nan" and "inf" and range comparisons pass NaN, so
 * every value is checked here before its range check.
 */
static int check_finite(const char *name, double value) {
  if (!isfinite(value)) {
    ERROR_LOG("%s = %g is not finite", name, value);
    return -1;
  }
  return 0;
}

/** Fail a log10 mass whose power of ten is not a finite positive normal double */
static int check_log_mass(const char *name, double log_mass) {
  const double mass = pow(10.0, log_mass);
  if (!isfinite(mass) || mass < DBL_MIN) {
    ERROR_LOG("%s = %g gives 10^%g = %g Msun/h, which is not a finite positive normal double", name,
              log_mass, log_mass, mass);
    return -1;
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

  LOAD_PARAM_DOUBLE("HODLogMmin", p.log_mmin);
  if (check_finite("HODLogMmin", p.log_mmin) != 0) {
    return -1;
  }

  LOAD_PARAM_DOUBLE("HODSigmaLogM", p.sigma_logm);
  if (check_finite("HODSigmaLogM", p.sigma_logm) != 0) {
    return -1;
  }
  if (!(p.sigma_logm > 0.0)) {
    ERROR_LOG("HODSigmaLogM = %g must be > 0 (dex width of the central occupation step)",
              p.sigma_logm);
    return -1;
  }

  LOAD_PARAM_DOUBLE("HODLogM0", p.log_m0);
  if (check_finite("HODLogM0", p.log_m0) != 0 || check_log_mass("HODLogM0", p.log_m0) != 0) {
    return -1;
  }

  LOAD_PARAM_DOUBLE("HODLogM1", p.log_m1);
  if (check_finite("HODLogM1", p.log_m1) != 0 || check_log_mass("HODLogM1", p.log_m1) != 0) {
    return -1;
  }

  LOAD_PARAM_DOUBLE("HODAlpha", p.alpha);
  if (check_finite("HODAlpha", p.alpha) != 0) {
    return -1;
  }
  if (p.alpha < 0.0) {
    ERROR_LOG("HODAlpha = %g must be >= 0 (satellite occupation slope)", p.alpha);
    return -1;
  }

  LOAD_PARAM_INT("HODSeed", p.seed);
  if (p.seed < 0) {
    ERROR_LOG("HODSeed = %d must be >= 0", p.seed);
    return -1;
  }

  LOAD_PARAM_DOUBLE("HODConcA", p.conc_a);
  if (check_finite("HODConcA", p.conc_a) != 0) {
    return -1;
  }
  if (!(p.conc_a > 0.0)) {
    ERROR_LOG("HODConcA = %g must be > 0 (concentration normalisation)", p.conc_a);
    return -1;
  }

  LOAD_PARAM_DOUBLE("HODConcLogMpivot", p.conc_log_mpivot);
  if (check_finite("HODConcLogMpivot", p.conc_log_mpivot) != 0 ||
      check_log_mass("HODConcLogMpivot", p.conc_log_mpivot) != 0) {
    return -1;
  }

  LOAD_PARAM_DOUBLE("HODConcB", p.conc_b);
  if (check_finite("HODConcB", p.conc_b) != 0) {
    return -1;
  }

  LOAD_PARAM_DOUBLE("HODConcC", p.conc_c);
  if (check_finite("HODConcC", p.conc_c) != 0) {
    return -1;
  }

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
 */
static int prepare_host_draw(const struct ModuleContext *ctx, const struct Halo *host,
                             struct HodOccupation *occupation) {
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

  if (!isfinite(host->Rvir) || host->Rvir < 0.0 || !isfinite(host->Vvir) || host->Vvir < 0.0) {
    ERROR_LOG("%s: host UniqueGalaxyID %lld has Rvir=%.17g Vvir=%.17g; both must be finite and "
              "nonnegative to place satellites",
              HOD_MODULE_NAME, host->UniqueGalaxyID, host->Rvir, host->Vvir);
    return -1;
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
  const double concentration = hod_populate_concentration(&hod_params, mass, ctx->redshift);
  if (!isfinite(concentration) || !(concentration > 0.0)) {
    ERROR_LOG("%s: host UniqueGalaxyID %lld (M=%.6e Msun/h, z=%.4f) has concentration %.6g; it "
              "must be finite and positive",
              HOD_MODULE_NAME, host->UniqueGalaxyID, mass, ctx->redshift, concentration);
    return -1;
  }
  for (int s = 0; s < occupation->num_satellites; s++) {
    if (hod_populate_draw_satellite(key, s, concentration, host->Rvir, host->Vvir, ctx->redshift,
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
  const bool draw = hod_populate_is_output_snapshot(ctx->snapshot_number);
  struct HodOccupation occupation = {0};
  if (draw && prepare_host_draw(ctx, host, &occupation) != 0) {
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

/** Per-bin sums of the audit: hosts, expected and realised galaxies */
struct HodAuditBins {
  int count;
  double log_lo; /**< Lower edge of bin 0, a multiple of HOD_AUDIT_BIN_DEX */
  int64_t *hosts;
  double *expected;
  int64_t *realised;
};

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
 * @brief Attribute every sample galaxy to its host's occupation bin
 *
 * A central counts in its own bin; a satellite in the bin of the Type 0 row
 * whose UniqueGalaxyID is its UniqueCentralGalaxyID, found in an ID-sorted
 * scratch table. A sample satellite with no such host breaks the module's own
 * contract and fails the audit.
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

  // The caller's first pass has already checked that every entry has a galaxy.
  for (int64_t i = 0; i < count; i++) {
    if (halos[i].galaxy->HODGhost != 0) {
      continue;
    }
    int bin = -1;
    if (halos[i].Type == 0) {
      const struct HodAuditHost key = {halos[i].UniqueGalaxyID, 0};
      const struct HodAuditHost *found =
          bsearch(&key, table, (size_t)n, sizeof(*table), compare_audit_hosts);
      bin = (found != NULL) ? found->bin : -1;
    } else {
      const struct HodAuditHost key = {halos[i].UniqueCentralGalaxyID, 0};
      const struct HodAuditHost *found =
          bsearch(&key, table, (size_t)n, sizeof(*table), compare_audit_hosts);
      bin = (found != NULL) ? found->bin : -1;
    }
    if (bin < 0) {
      ERROR_LOG("%s audit: sample row UniqueGalaxyID %lld (Type %d) has no Type 0 host with a "
                "positive mass under UniqueCentralGalaxyID %lld in this snapshot",
                HOD_MODULE_NAME, halos[i].UniqueGalaxyID, halos[i].Type,
                halos[i].UniqueCentralGalaxyID);
      myfree(table);
      return -1;
    }
    bins->realised[bin]++;
  }
  myfree(table);
  return 0;
}

int hod_populate_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                                  int64_t count) {
  if (ctx == NULL || !hod_ready) {
    ERROR_LOG("%s: process_snapshot called %s", HOD_MODULE_NAME,
              ctx == NULL ? "without a snapshot context" : "before a successful init()");
    return -1;
  }
  if (!hod_populate_is_output_snapshot(ctx->snapshot_number)) {
    return 0;
  }
  if (count < 0 || (count > 0 && halos == NULL)) {
    ERROR_LOG("%s audit: invalid population (halos=%p, count=%lld)", HOD_MODULE_NAME,
              (const void *)halos, (long long)count);
    return -1;
  }

  // Pass 1: validate, total the expectation over Type 0 hosts and count the sample.
  int64_t hosts = 0;
  int64_t realised_all = 0;
  int64_t realised_sat = 0;
  double expected_all = 0.0;
  double expected_sat = 0.0;
  double log_min = HUGE_VAL;
  double log_max = -HUGE_VAL;
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
      hosts++;
      expected_all += ncen + ncen * lambda;
      expected_sat += ncen * lambda;
      if (mass > 0.0) {
        log_min = fmin(log_min, log10(mass));
        log_max = fmax(log_max, log10(mass));
      }
    }
    if (ghost == 0) {
      if (h->Type == 2) {
        realised_sat++;
      } else if (h->Type != 0) {
        ERROR_LOG("%s audit: UniqueGalaxyID %lld is a sample member of Type %d; only Type 0 "
                  "centrals and Type 2 created satellites can be",
                  HOD_MODULE_NAME, h->UniqueGalaxyID, h->Type);
        return -1;
      }
      realised_all++;
    }
  }

  // Pass 2: <N(M)> realised versus expected in HOD_AUDIT_BIN_DEX bins over the host mass range.
  struct HodAuditBins bins = {0, 0.0, NULL, NULL, NULL};
  if (log_max >= log_min) {
    bins.log_lo = floor(log_min / HOD_AUDIT_BIN_DEX) * HOD_AUDIT_BIN_DEX;
    bins.count = (int)floor((log_max - bins.log_lo) / HOD_AUDIT_BIN_DEX) + 1;
    bins.hosts = mymalloc_cat((size_t)bins.count * sizeof(int64_t), MEM_UTILITY);
    bins.expected = mymalloc_cat((size_t)bins.count * sizeof(double), MEM_UTILITY);
    bins.realised = mymalloc_cat((size_t)bins.count * sizeof(int64_t), MEM_UTILITY);
    for (int b = 0; b < bins.count; b++) {
      bins.hosts[b] = 0;
      bins.expected[b] = 0.0;
      bins.realised[b] = 0;
    }
  }
  if (realised_all > 0 && bins.count == 0) {
    ERROR_LOG("%s audit: %lld sample rows but no Type 0 host with a positive mass", HOD_MODULE_NAME,
              (long long)realised_all);
    return -1;
  }
  if (bins.count > 0 && fill_audit_bins(halos, count, hosts, &bins) != 0) {
    myfree(bins.realised);
    myfree(bins.expected);
    myfree(bins.hosts);
    return -1;
  }

  const double volume = hod_params.box_size * hod_params.box_size * hod_params.box_size;
  const double fsat_expected = (expected_all > 0.0) ? expected_sat / expected_all : 0.0;
  const double fsat_realised =
      (realised_all > 0) ? (double)realised_sat / (double)realised_all : 0.0;
  INFO_LOG("HOD audit z=%.4f hosts=%lld n_gal expected=%.6e realised=%.6e f_sat expected=%.6f "
           "realised=%.6f",
           ctx->redshift, (long long)hosts, expected_all / volume, (double)realised_all / volume,
           fsat_expected, fsat_realised);
  for (int b = 0; b < bins.count; b++) {
    if (bins.hosts[b] == 0) {
      continue;
    }
    const double lo = bins.log_lo + b * HOD_AUDIT_BIN_DEX;
    VERBOSE_LOG("HOD audit bin z=%.4f log10M=[%.2f, %.2f) hosts=%lld <N> expected=%.6f "
                "realised=%.6f",
                ctx->redshift, lo, lo + HOD_AUDIT_BIN_DEX, (long long)bins.hosts[b],
                bins.expected[b] / (double)bins.hosts[b],
                (double)bins.realised[b] / (double)bins.hosts[b]);
  }

  if (bins.count > 0) {
    myfree(bins.realised);
    myfree(bins.expected);
    myfree(bins.hosts);
  }
  return 0;
}

int hod_populate_cleanup(void) {
  hod_ready = false;
  VERBOSE_LOG("HOD populate cleaned up");
  return 0;
}
