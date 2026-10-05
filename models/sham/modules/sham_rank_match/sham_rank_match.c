/**
 * @file    sham_rank_match.c
 * @brief   Calibrated-target subhalo abundance matching by global rank
 *
 * A dual-mode module. In modules.pre_timestep (process_full_halo) it runs once
 * per FoF step before any other module: it retires every carried Type 2 row to
 * Type 3, ratchets the peak proxies ShamVpeak and ShamMpeak of every Type 0/1
 * row up from the catalogue's Vmax and Mvir, and resets every row to a
 * non-member (ShamGhost = 1, StellarMass = 0). In modules.post_snapshot
 * (process_snapshot, horizontal driver only) it ranks the whole snapshot's
 * Type 0/1 rows with ShamVpeak >= ShamMinVpeak by descending ShamVpeak (ties by
 * ascending UniqueGalaxyID) and gives zero-based rank r the stellar mass at
 * which the target's cumulative stellar mass function equals the rank density
 * (r + 0.5) / BoxSize^3 * h^3, masking every rank whose density lies above the
 * target's density at its mass floor. The rank, the audit totals and the
 * failure agreement go through the snapshot collectives (snapshot_collectives.h),
 * so under a distributed horizontal run each task ranks its own rows within the
 * whole snapshot.
 *
 * The target is the Baldry et al. (2012) double Schechter form, published at
 * h_obs and converted once at init to the simulation's h (M ~ h^-2, n ~ h^3);
 * its cumulative density is tabulated by quadrature in ln M and inverted by
 * bisection with log-log interpolation (sham_rank_match.h). There is no scatter.
 *
 * References: Conroy et al. (2006), Vale & Ostriker (2006) for the
 * abundance-matching construction; Reddick et al. (2013) for peak circular
 * velocity as the ranking proxy; Baldry et al. (2012) for the target.
 */

#include <assert.h>
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

#include "module_system/parameter_helpers.h"

#include "sham_rank_match.h"

/** Configured name of this module */
#define SHAM_MODULE_NAME "sham_rank_match"

// ============================================================================
// MODULE STATE
// ============================================================================

/** Parameters loaded and validated by init() */
static struct ShamRankMatchParameters sham_params;

/** Target converted to the simulation's h and tabulated by init(); freed by cleanup() */
static struct ShamRankMatchTarget sham_target;

/** Box side, comoving Mpc/h, validated by init() */
static double sham_box_size = 0.0;

/** Whether init() completed; process() and process_snapshot() refuse to run otherwise */
static bool sham_ready = false;

/**
 * @brief One snapshot population entry in rank scratch
 *
 * Scratch is the only thing ever sorted: the borrowed population keeps its
 * order. index is the entry's position in that population; rank is the
 * candidate's global rank from module_snapshot_rank() (-1 until ranked); mass is
 * the candidate's internal stellar mass when assigned is SHAM_RANK_ASSIGNED.
 */
struct ShamRankRecord {
  long long id;
  int64_t index;
  int64_t rank;
  float vpeak;
  float mass;
  int assigned;
};

// ============================================================================
// TARGET TABLE
// ============================================================================

/**
 * @brief Integrand of n(>M) in t = ln M': exp(-x) [phi1 x^(alpha1+1) + phi2 x^(alpha2+1)]
 *
 * Each term is formed as one exponential of (alpha + 1) ln x - x, so neither
 * the power nor the exponential cutoff overflows or underflows on its own.
 */
static double target_integrand(const struct ShamRankMatchTarget *target, double log_ms,
                               double log_mass) {
  const double log_x = log_mass - log_ms;
  const double x = exp(log_x);
  return target->phi1 * exp((target->alpha1 + 1.0) * log_x - x) +
         target->phi2 * exp((target->alpha2 + 1.0) * log_x - x);
}

void sham_rank_match_free_target(struct ShamRankMatchTarget *target) {
  if (target == NULL) {
    return;
  }
  if (target->log_density != NULL) {
    myfree(target->log_density);
  }
  if (target->density != NULL) {
    myfree(target->density);
  }
  if (target->log_mass != NULL) {
    myfree(target->log_mass);
  }
  memset(target, 0, sizeof(*target));
}

/** @brief n(>M) at ln M from the table; NAN below the table, 0 at or above its top */
static double cumulative_at_log_mass(const struct ShamRankMatchTarget *target, double log_mass) {
  const int last = target->num_points - 1;
  if (!(log_mass >= target->log_mass[0])) {
    return NAN;
  }
  if (log_mass >= target->log_mass[last]) {
    return 0.0;
  }
  int lo = 0;
  int hi = last;
  while (hi - lo > 1) {
    const int mid = lo + (hi - lo) / 2;
    if (target->log_mass[mid] <= log_mass) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double fraction =
      (log_mass - target->log_mass[lo]) / (target->log_mass[hi] - target->log_mass[lo]);
  if (hi == last) {
    // The top node's density is 0, so the last interval is linear in ln M.
    return target->density[lo] * (1.0 - fraction);
  }
  return exp(target->log_density[lo] +
             fraction * (target->log_density[hi] - target->log_density[lo]));
}

int sham_rank_match_build_target(const struct ShamRankMatchParameters *params, double hubble_sim,
                                 struct ShamRankMatchTarget *target) {
  memset(target, 0, sizeof(*target));
  target->hubble_sim = hubble_sim;
  target->log_mstar = params->log_mstar + 2.0 * log10(params->hubble_obs / hubble_sim);
  const double h_ratio = hubble_sim / params->hubble_obs;
  target->phi1 = params->phi1 * h_ratio * h_ratio * h_ratio;
  target->alpha1 = params->alpha1;
  target->phi2 = params->phi2 * h_ratio * h_ratio * h_ratio;
  target->alpha2 = params->alpha2;
  target->log_mass_floor = params->log_mass_floor;

  const double ln10 = log(10.0);
  const double log_ms = target->log_mstar * ln10;
  const double log_start = (params->log_mass_floor - 1.0) * ln10;
  const double log_end = log_ms + log(SHAM_TARGET_MAX_MASS_RATIO);
  const double log_floor = params->log_mass_floor * ln10;
  if (!isfinite(target->log_mstar) || !isfinite(target->phi1) || !(target->phi1 > 0.0) ||
      !isfinite(target->phi2) || !(target->phi2 > 0.0) || !isfinite(log_end) ||
      !(log_floor < log_end)) {
    ERROR_LOG("%s: the converted target is unusable: log10 Ms_sim=%.10g phi1_sim=%.10g "
              "phi2_sim=%.10g Mpc^-3 (h_sim=%.10g); the mass floor 10^%.10g Msun must lie below "
              "the table top %g Ms_sim = 10^%.10g Msun",
              SHAM_MODULE_NAME, target->log_mstar, target->phi1, target->phi2, hubble_sim,
              params->log_mass_floor, SHAM_TARGET_MAX_MASS_RATIO, log_end / ln10);
    return -1;
  }

  const int n = SHAM_TARGET_TABLE_POINTS;
  target->num_points = n;
  target->log_mass = mymalloc_cat((size_t)n * sizeof(double), MEM_UTILITY);
  target->density = mymalloc_cat((size_t)n * sizeof(double), MEM_UTILITY);
  target->log_density = mymalloc_cat((size_t)n * sizeof(double), MEM_UTILITY);

  const double step = (log_end - log_start) / (double)(n - 1);
  for (int k = 0; k < n - 1; k++) {
    target->log_mass[k] = log_start + (double)k * step;
  }
  target->log_mass[n - 1] = log_end;

  // Simpson's rule on each interval's two halves, summed from the top node down.
  target->density[n - 1] = 0.0;
  target->log_density[n - 1] = -HUGE_VAL;
  for (int k = n - 2; k >= 0; k--) {
    const double a = target->log_mass[k];
    const double b = target->log_mass[k + 1];
    const double segment = (b - a) / 6.0 *
                           (target_integrand(target, log_ms, a) +
                            4.0 * target_integrand(target, log_ms, 0.5 * (a + b)) +
                            target_integrand(target, log_ms, b));
    target->density[k] = target->density[k + 1] + segment;
    if (!isfinite(target->density[k]) || !(target->density[k] > target->density[k + 1])) {
      ERROR_LOG("%s: the tabulated n(>M) is not finite and strictly decreasing at M = 10^%.10g "
                "Msun (n = %.10g, next %.10g Mpc^-3); check the target's slopes and normalisations",
                SHAM_MODULE_NAME, a / ln10, target->density[k], target->density[k + 1]);
      sham_rank_match_free_target(target);
      return -1;
    }
    target->log_density[k] = log(target->density[k]);
  }

  target->floor_density = cumulative_at_log_mass(target, log_floor);
  if (!isfinite(target->floor_density) || !(target->floor_density > 0.0)) {
    ERROR_LOG("%s: the target's floor density n(>10^%.10g Msun) = %.10g Mpc^-3 is not finite and "
              "positive",
              SHAM_MODULE_NAME, params->log_mass_floor, target->floor_density);
    sham_rank_match_free_target(target);
    return -1;
  }
  return 0;
}

double sham_rank_match_cumulative_density(const struct ShamRankMatchTarget *target, double mass) {
  if (!isfinite(mass) || !(mass > 0.0)) {
    return NAN;
  }
  return cumulative_at_log_mass(target, log(mass));
}

int sham_rank_match_inverse(const struct ShamRankMatchTarget *target, double density,
                            double *mass) {
  const int last_positive = target->num_points - 2;
  if (!isfinite(density) || !(density <= target->density[0]) ||
      !(density >= target->density[last_positive])) {
    return -1;
  }
  // Bisection over the nodes: density[lo] >= density >= density[hi], hi = lo + 1.
  int lo = 0;
  int hi = last_positive;
  while (hi - lo > 1) {
    const int mid = lo + (hi - lo) / 2;
    if (target->density[mid] >= density) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double fraction = (log(density) - target->log_density[lo]) /
                          (target->log_density[hi] - target->log_density[lo]);
  *mass = exp(target->log_mass[lo] + fraction * (target->log_mass[hi] - target->log_mass[lo]));
  return 0;
}

int sham_rank_match_mass_at_density(const struct ShamRankMatchTarget *target, double density,
                                    double *mass) {
  if (density > target->floor_density) {
    return SHAM_RANK_MASKED;
  }
  double value = 0.0;
  if (sham_rank_match_inverse(target, density, &value) != 0) {
    return -1;
  }
  *mass = value;
  return SHAM_RANK_ASSIGNED;
}

double sham_rank_match_rank_density(int64_t rank, double box_size, double hubble_sim) {
  const double ratio = hubble_sim / box_size;
  return ((double)rank + 0.5) * (ratio * ratio * ratio);
}

const struct ShamRankMatchTarget *sham_rank_match_active_target(void) {
  return sham_ready ? &sham_target : NULL;
}

// ============================================================================
// CONFIGURATION AND PARAMETERS
// ============================================================================

/**
 * @brief Require the module exactly once in pre_timestep as process_full_halo, in no
 *        other FoF phase, and in post_snapshot as process_snapshot
 *
 * The retirement of carried Type 2 rows must precede every other module, and
 * the rank needs every FoF group processed, hence the two placements. FoF
 * phases accept repeated entries (the registry initialises a module once), so
 * entries are counted here; the registry rejects duplicate and non-snapshot
 * post_snapshot entries before init().
 */
static int check_configuration(void) {
  int full_halo_entries = 0;
  const int entries = module_count_phase_entries(SHAM_MODULE_NAME, MimicConfig.pre_timestep,
                                                 MimicConfig.num_pre_timestep,
                                                 PROCESSING_MODE_FULL_HALO, &full_halo_entries);
  if (entries != 1 || full_halo_entries != 1) {
    ERROR_LOG("%s must be configured exactly once in modules.pre_timestep as process_full_halo "
              "(found %d entries, %d as process_full_halo)",
              SHAM_MODULE_NAME, entries, full_halo_entries);
    return -1;
  }
  if (module_count_phase_entries(SHAM_MODULE_NAME, MimicConfig.post_timestep,
                                 MimicConfig.num_post_timestep, PROCESSING_MODE_FULL_HALO,
                                 NULL) > 0) {
    ERROR_LOG("%s is configured in modules.post_timestep; its FoF step runs only in "
              "modules.pre_timestep, before every other module",
              SHAM_MODULE_NAME);
    return -1;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    if (module_count_phase_entries(SHAM_MODULE_NAME, phase->modules, phase->num_modules,
                                   PROCESSING_MODE_FULL_HALO, NULL) > 0) {
      ERROR_LOG("%s is configured in substep phase '%s'; its FoF step runs only in "
                "modules.pre_timestep, before every other module",
                SHAM_MODULE_NAME, phase->name != NULL ? phase->name : "(unnamed)");
      return -1;
    }
  }
  if (!module_configured_in_phase(SHAM_MODULE_NAME, MimicConfig.post_snapshot,
                                  MimicConfig.num_post_snapshot, PROCESSING_MODE_SNAPSHOT)) {
    ERROR_LOG("%s must be configured in modules.post_snapshot as process_snapshot: the rank "
              "match needs the whole snapshot (horizontal driver only)",
              SHAM_MODULE_NAME);
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
    ERROR_LOG("%s: %s = %g is not finite", SHAM_MODULE_NAME, name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief Fail if any output snapshot lies above ShamTargetRedshiftMax
 *
 * The target is cross-sectional: it is only applied where it was observed.
 * NOUT == 0 (no list resolved) means every snapshot of the package's list.
 */
static int check_redshift_window(double redshift_max) {
  const int count = (MimicConfig.NOUT == 0) ? MimicConfig.MAXSNAPS : MimicConfig.NOUT;
  for (int n = 0; n < count; n++) {
    const int snapshot = (MimicConfig.NOUT == 0) ? n : MimicConfig.ListOutputSnaps[n];
    if (snapshot < 0 || snapshot >= ABSOLUTEMAXSNAPS) {
      ERROR_LOG("%s: output snapshot %d is outside [0, %d)", SHAM_MODULE_NAME, snapshot,
                ABSOLUTEMAXSNAPS);
      return -1;
    }
    const double redshift = MimicConfig.ZZ[snapshot];
    if (!(redshift <= redshift_max)) {
      ERROR_LOG("%s: output snapshot %d has z = %.6g above ShamTargetRedshiftMax = %.6g; the "
                "target is cross-sectional, so remove the snapshot from output.snapshot_list or "
                "raise the window deliberately",
                SHAM_MODULE_NAME, snapshot, redshift, redshift_max);
      return -1;
    }
  }
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sham_rank_match_init(void) {
  sham_ready = false;
  sham_rank_match_free_target(&sham_target);

  if (check_configuration() != 0) {
    return -1;
  }

  // Loaded by name so the parameter lint sees every key; every value is then checked finite
  // before any range check.
  struct ShamRankMatchParameters p;
  LOAD_PARAM_DOUBLE("ShamTargetLogMstar", p.log_mstar);
  LOAD_PARAM_DOUBLE("ShamTargetPhi1", p.phi1);
  LOAD_PARAM_DOUBLE("ShamTargetAlpha1", p.alpha1);
  LOAD_PARAM_DOUBLE("ShamTargetPhi2", p.phi2);
  LOAD_PARAM_DOUBLE("ShamTargetAlpha2", p.alpha2);
  LOAD_PARAM_DOUBLE("ShamTargetHubble", p.hubble_obs);
  LOAD_PARAM_DOUBLE("ShamTargetLogMassFloor", p.log_mass_floor);
  LOAD_PARAM_DOUBLE("ShamTargetRedshiftMax", p.redshift_max);
  LOAD_PARAM_DOUBLE("ShamMinVpeak", p.min_vpeak);
  const struct {
    const char *name;
    double value;
  } finite_checks[] = {
      {"ShamTargetLogMstar", p.log_mstar},
      {"ShamTargetPhi1", p.phi1},
      {"ShamTargetAlpha1", p.alpha1},
      {"ShamTargetPhi2", p.phi2},
      {"ShamTargetAlpha2", p.alpha2},
      {"ShamTargetHubble", p.hubble_obs},
      {"ShamTargetLogMassFloor", p.log_mass_floor},
      {"ShamTargetRedshiftMax", p.redshift_max},
      {"ShamMinVpeak", p.min_vpeak},
  };
  for (size_t k = 0; k < sizeof(finite_checks) / sizeof(finite_checks[0]); k++) {
    if (check_finite(finite_checks[k].name, finite_checks[k].value) != 0) {
      return -1;
    }
  }
  if (!(p.phi1 > 0.0) || !(p.phi2 > 0.0)) {
    ERROR_LOG("%s: ShamTargetPhi1 = %g and ShamTargetPhi2 = %g must both be > 0 (Mpc^-3)",
              SHAM_MODULE_NAME, p.phi1, p.phi2);
    return -1;
  }
  if (!(p.hubble_obs > 0.0 && p.hubble_obs < 2.0)) {
    ERROR_LOG("ShamTargetHubble = %g must lie in (0, 2) (the h of the published fit)",
              p.hubble_obs);
    return -1;
  }
  if (!(p.min_vpeak > 0.0)) {
    ERROR_LOG("ShamMinVpeak = %g must be > 0 (km/s completeness floor on ShamVpeak)", p.min_vpeak);
    return -1;
  }

  const double box_size = MimicConfig.BoxSize;
  const double hubble_sim = MimicConfig.Hubble_h;
  if (!isfinite(box_size) || !(box_size > 0.0)) {
    ERROR_LOG("%s needs a finite positive BoxSize (BoxSize=%g Mpc/h)", SHAM_MODULE_NAME, box_size);
    return -1;
  }
  if (!isfinite(hubble_sim) || !(hubble_sim > 0.0)) {
    ERROR_LOG("%s needs a finite positive simulation Hubble_h (Hubble_h=%g)", SHAM_MODULE_NAME,
              hubble_sim);
    return -1;
  }
  const double unit_density = sham_rank_match_rank_density(0, box_size, hubble_sim) * 2.0;
  if (!isfinite(unit_density) || unit_density < DBL_MIN) {
    ERROR_LOG("%s: (Hubble_h / BoxSize)^3 = %g Mpc^-3 is not a finite positive normal double "
              "(BoxSize=%g Mpc/h, Hubble_h=%g)",
              SHAM_MODULE_NAME, unit_density, box_size, hubble_sim);
    return -1;
  }

  if (check_redshift_window(p.redshift_max) != 0) {
    return -1;
  }

  if (sham_rank_match_build_target(&p, hubble_sim, &sham_target) != 0) {
    return -1;
  }

  sham_params = p;
  sham_box_size = box_size;
  sham_ready = true;
  INFO_LOG("SHAM rank match initialized: target at h_sim=%.6g: log10 Ms=%.6f phi1=%.6e "
           "alpha1=%.4g phi2=%.6e alpha2=%.4g Mpc^-3 (published at h=%.4g); mass floor 10^%.4g "
           "Msun with n(>floor)=%.6e Mpc^-3; ShamMinVpeak=%.6g km/s; z <= %.4g; BoxSize=%.6g Mpc/h",
           hubble_sim, sham_target.log_mstar, sham_target.phi1, sham_target.alpha1,
           sham_target.phi2, sham_target.alpha2, p.hubble_obs, p.log_mass_floor,
           sham_target.floor_density, p.min_vpeak, p.redshift_max, box_size);
  return 0;
}

/**
 * @brief Validate one Type 0/1 row's peak update without writing it
 *
 * Mvir is double and ShamMpeak float, so the updated mass peak is bounded by
 * FLT_MAX before it may be stored; Vmax is already a float.
 */
static int updated_peaks(const struct Halo *halo, float *vpeak, float *mpeak) {
  const struct GalaxyData *gal = halo->galaxy;
  const float previous_vpeak = gal->ShamVpeak;
  const float previous_mpeak = gal->ShamMpeak;
  if (!isfinite(previous_vpeak) || previous_vpeak < 0.0f || !isfinite(previous_mpeak) ||
      previous_mpeak < 0.0f) {
    ERROR_LOG("%s: UniqueGalaxyID %lld has malformed peaks ShamVpeak=%g ShamMpeak=%g; both must "
              "be finite and nonnegative",
              SHAM_MODULE_NAME, halo->UniqueGalaxyID, (double)previous_vpeak,
              (double)previous_mpeak);
    return -1;
  }
  if (!isfinite(halo->Vmax) || halo->Vmax < 0.0f || !isfinite(halo->Mvir) || halo->Mvir < 0.0) {
    ERROR_LOG("%s: UniqueGalaxyID %lld has malformed proxies Vmax=%g Mvir=%.17g; both must be "
              "finite and nonnegative",
              SHAM_MODULE_NAME, halo->UniqueGalaxyID, (double)halo->Vmax, halo->Mvir);
    return -1;
  }
  const double mpeak_double = (halo->Mvir > (double)previous_mpeak) ? halo->Mvir : previous_mpeak;
  if (mpeak_double > (double)FLT_MAX) {
    ERROR_LOG("%s: UniqueGalaxyID %lld peak mass %.17g exceeds the float ShamMpeak storage "
              "limit %.9g",
              SHAM_MODULE_NAME, halo->UniqueGalaxyID, mpeak_double, (double)FLT_MAX);
    return -1;
  }
  *vpeak = (halo->Vmax > previous_vpeak) ? halo->Vmax : previous_vpeak;
  *mpeak = (float)mpeak_double;
  return 0;
}

int sham_rank_match_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (ctx == NULL || !sham_ready) {
    ERROR_LOG("%s: process called %s", SHAM_MODULE_NAME,
              ctx == NULL ? "without a module context" : "before a successful init()");
    return -1;
  }
  if (ngal < 1 || halos == NULL) {
    ERROR_LOG("%s: process needs a FoF workspace with at least one row (ngal=%d, halos=%p)",
              SHAM_MODULE_NAME, ngal, (void *)halos);
    return -1;
  }

  // Validate every row before the first write, so a failure leaves the FoF step untouched.
  for (int i = 0; i < ngal; i++) {
    const struct Halo *h = &halos[i];
    if (h->Type < 0 || h->Type > 3) {
      ERROR_LOG("%s: workspace row %d (UniqueGalaxyID %lld) has Type %d outside [0, 3]",
                SHAM_MODULE_NAME, i, h->UniqueGalaxyID, h->Type);
      return -1;
    }
    if (h->Type <= 2 && h->galaxy == NULL) {
      ERROR_LOG("%s: workspace row %d (UniqueGalaxyID %lld, Type %d) has no galaxy",
                SHAM_MODULE_NAME, i, h->UniqueGalaxyID, h->Type);
      return -1;
    }
    float vpeak;
    float mpeak;
    if (h->Type <= 1 && updated_peaks(h, &vpeak, &mpeak) != 0) {
      ERROR_LOG("%s: FoF step at snapshot %d failed; nothing was written", SHAM_MODULE_NAME,
                ctx->snapshot_number);
      return -1;
    }
  }

  for (int i = 0; i < ngal; i++) {
    struct Halo *h = &halos[i];
    // Carried orphans leave the population: the candidates are resolved rows only.
    if (h->Type == 2) {
      h->Type = 3;
    }
    if (h->Type <= 1) {
      float vpeak;
      float mpeak;
      // The validation pass ran this same pure function on these unchanged rows and succeeded.
      const int status = updated_peaks(h, &vpeak, &mpeak);
      assert(status == 0);
      (void)status;
      h->galaxy->ShamVpeak = vpeak;
      h->galaxy->ShamMpeak = mpeak;
    }
    if (h->galaxy != NULL) {
      h->galaxy->ShamGhost = 1;
      h->galaxy->StellarMass = 0.0f;
    }
  }
  return 0;
}

static int compare_by_id(const void *a, const void *b) {
  const long long id_a = ((const struct ShamRankRecord *)a)->id;
  const long long id_b = ((const struct ShamRankRecord *)b)->id;
  return (id_a > id_b) - (id_a < id_b);
}

/** Local order of ranked candidates: ascending global rank (ranks are unique) */
static int compare_by_global_rank(const void *a, const void *b) {
  const int64_t rank_a = ((const struct ShamRankRecord *)a)->rank;
  const int64_t rank_b = ((const struct ShamRankRecord *)b)->rank;
  return (rank_a > rank_b) - (rank_a < rank_b);
}

/** @brief Validate one snapshot entry: the contract the pre_timestep callback establishes */
static int validate_entry(const struct Halo *h, int64_t index) {
  if (h->galaxy == NULL) {
    ERROR_LOG("%s: entry %lld (UniqueGalaxyID %lld) has no galaxy", SHAM_MODULE_NAME,
              (long long)index, h->UniqueGalaxyID);
    return -1;
  }
  if (h->Type != 0 && h->Type != 1) {
    ERROR_LOG("%s: entry %lld (UniqueGalaxyID %lld) has Type %d; the pre_timestep callback "
              "retires every Type 2 row before marshal, so only Types 0/1 can reach the rank "
              "(did the pre_timestep entry run?)",
              SHAM_MODULE_NAME, (long long)index, h->UniqueGalaxyID, h->Type);
    return -1;
  }
  if (h->UniqueGalaxyID <= 0) {
    ERROR_LOG("%s: entry %lld has non-positive UniqueGalaxyID %lld", SHAM_MODULE_NAME,
              (long long)index, h->UniqueGalaxyID);
    return -1;
  }
  const float vpeak = h->galaxy->ShamVpeak;
  const float mpeak = h->galaxy->ShamMpeak;
  if (!isfinite(vpeak) || vpeak < 0.0f || !isfinite(mpeak) || mpeak < 0.0f) {
    ERROR_LOG("%s: UniqueGalaxyID %lld has malformed peaks ShamVpeak=%g ShamMpeak=%g; both must "
              "be finite and nonnegative",
              SHAM_MODULE_NAME, h->UniqueGalaxyID, (double)vpeak, (double)mpeak);
    return -1;
  }
  return 0;
}

/**
 * @brief Validate the local population and compact its candidates into scratch
 *
 * The duplicate-id check covers this task's whole population, as the rank's
 * cross-task id uniqueness is guaranteed by disjoint forests. On success
 * records[0, *num_candidates) hold the candidates (ShamVpeak >= ShamMinVpeak).
 */
static int collect_candidates(const struct Halo *halos, int64_t count,
                              struct ShamRankRecord *records, int64_t *num_candidates) {
  for (int64_t i = 0; i < count; i++) {
    if (validate_entry(&halos[i], i) != 0) {
      return -1;
    }
    records[i].id = halos[i].UniqueGalaxyID;
    records[i].index = i;
    records[i].rank = -1;
    records[i].vpeak = halos[i].galaxy->ShamVpeak;
    records[i].mass = 0.0f;
    records[i].assigned = SHAM_RANK_MASKED;
  }

  qsort(records, (size_t)count, sizeof(*records), compare_by_id);
  for (int64_t i = 1; i < count; i++) {
    if (records[i].id == records[i - 1].id) {
      ERROR_LOG("%s: UniqueGalaxyID %lld appears more than once (entries %lld and %lld)",
                SHAM_MODULE_NAME, records[i].id, (long long)records[i - 1].index,
                (long long)records[i].index);
      return -1;
    }
  }

  int64_t candidates = 0;
  for (int64_t i = 0; i < count; i++) {
    if ((double)records[i].vpeak >= sham_params.min_vpeak) {
      records[candidates++] = records[i];
    }
  }
  *num_candidates = candidates;
  return 0;
}

/**
 * @brief Give each candidate its global rank through module_snapshot_rank()
 *
 * Keys are {(double)ShamVpeak, UniqueGalaxyID}: the float-to-double widening
 * is exact, so the collective's order (descending value, ascending id) is the
 * module's rank order. Every task calls the collective, with no keys when it
 * holds no candidates. On success the candidates are sorted by rank.
 */
static int rank_candidates(const struct SnapshotContext *ctx, struct ShamRankRecord *records,
                           int64_t candidates) {
  struct SnapshotRankKey *keys = NULL;
  int64_t *ranks = NULL;
  if (candidates > 0) {
    keys = mymalloc_cat((size_t)candidates * sizeof(*keys), MEM_UTILITY);
    ranks = mymalloc_cat((size_t)candidates * sizeof(*ranks), MEM_UTILITY);
    for (int64_t k = 0; k < candidates; k++) {
      keys[k].value = (double)records[k].vpeak;
      keys[k].id = records[k].id;
    }
  }
  const int status = module_snapshot_rank(ctx, keys, candidates, ranks);
  if (status == 0) {
    for (int64_t k = 0; k < candidates; k++) {
      records[k].rank = ranks[k];
    }
    qsort(records, (size_t)candidates, sizeof(*records), compare_by_global_rank);
  } else {
    ERROR_LOG("%s: the global rank of %lld local candidates failed", SHAM_MODULE_NAME,
              (long long)candidates);
  }
  if (keys != NULL) {
    myfree(ranks);
    myfree(keys);
  }
  return status;
}

/**
 * @brief Price the rank-ordered candidates into scratch, writing nothing
 *
 * On success the leading *num_assigned candidates are assigned and the rest
 * masked. Rank densities rise with the global rank, so the first masked
 * candidate masks every later one on this task.
 */
static int price_candidates(struct ShamRankRecord *records, int64_t candidates,
                            int64_t *num_assigned) {
  int64_t assigned = 0;
  for (int64_t k = 0; k < candidates; k++) {
    const int64_t rank = records[k].rank;
    const double density =
        sham_rank_match_rank_density(rank, sham_box_size, sham_target.hubble_sim);
    double mass_phys = 0.0;
    const int outcome = sham_rank_match_mass_at_density(&sham_target, density, &mass_phys);
    if (outcome == SHAM_RANK_MASKED) {
      break;
    }
    const double mass_internal = mass_phys * sham_target.hubble_sim / SHAM_MASS_UNIT_MSUN;
    if (outcome != SHAM_RANK_ASSIGNED || !isfinite(mass_internal) || !(mass_internal > 0.0)) {
      ERROR_LOG("%s: rank %lld (UniqueGalaxyID %lld, ShamVpeak %g) at density %.10g Mpc^-3 has "
                "no representable stellar mass in the target table (n(>M) spans [%.10g, %.10g])",
                SHAM_MODULE_NAME, (long long)rank, records[k].id, (double)records[k].vpeak, density,
                sham_target.density[sham_target.num_points - 2], sham_target.density[0]);
      return -1;
    }
    // Bound the double before narrowing: an out-of-range conversion to float is undefined.
    const float mass = (mass_internal <= (double)FLT_MAX) ? (float)mass_internal : 0.0f;
    if (!(mass > 0.0f)) {
      ERROR_LOG("%s: rank %lld (UniqueGalaxyID %lld, ShamVpeak %g) has stellar mass %.10g "
                "(1e10 Msun/h) outside the float StellarMass range (0, %.9g]",
                SHAM_MODULE_NAME, (long long)rank, records[k].id, (double)records[k].vpeak,
                mass_internal, (double)FLT_MAX);
      return -1;
    }
    records[k].mass = mass;
    records[k].assigned = SHAM_RANK_ASSIGNED;
    assigned++;
  }
  *num_assigned = assigned;
  return 0;
}

/**
 * @brief Validate, rank and price this task's candidates into scratch, writing nothing
 *
 * Reaches module_snapshot_rank() on every task whatever its local state: a task
 * whose population failed validation ranks no keys and reports the failure.
 * On success records[0, *num_candidates) hold the candidates in rank order with
 * their outcomes; *num_assigned of them, the leading ones, are assigned.
 */
static int rank_into_scratch(const struct SnapshotContext *ctx, const struct Halo *halos,
                             int64_t count, struct ShamRankRecord *records, int64_t *num_candidates,
                             int64_t *num_assigned) {
  int failed = 0;
  int64_t candidates = 0;
  if (count > 0 && collect_candidates(halos, count, records, &candidates) != 0) {
    failed = 1;
    candidates = 0;
  }
  if (rank_candidates(ctx, records, candidates) != 0) {
    failed = 1;
  }
  int64_t assigned = 0;
  if (!failed && price_candidates(records, candidates, &assigned) != 0) {
    failed = 1;
  }
  *num_candidates = candidates;
  *num_assigned = assigned;
  return failed ? -1 : 0;
}

/**
 * Collectives, in the order every task reaches them on an output snapshot:
 * module_snapshot_rank() (in rank_into_scratch()), module_snapshot_sum_i64()
 * on the audit counts, module_snapshot_any() on the failure flag. None depends
 * on a local count; a task writes only after every task has agreed success.
 */
int sham_rank_match_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                                     int64_t count) {
  if (ctx == NULL || !sham_ready) {
    ERROR_LOG("%s: process_snapshot called %s", SHAM_MODULE_NAME,
              ctx == NULL ? "without a snapshot context" : "before a successful init()");
    return -1;
  }
  if (!mimic_is_output_snapshot(ctx->snapshot_number)) {
    return 0;
  }

  int failed = 0;
  if (count < 0 || (count > 0 && halos == NULL)) {
    ERROR_LOG("%s: invalid population (halos=%p, count=%lld)", SHAM_MODULE_NAME,
              (const void *)halos, (long long)count);
    failed = 1;
  } else if ((uint64_t)count > SIZE_MAX / sizeof(struct ShamRankRecord)) {
    ERROR_LOG("%s: population of %lld entries exceeds the addressable scratch size",
              SHAM_MODULE_NAME, (long long)count);
    failed = 1;
  }
  // A task whose population is unusable still takes part, with nothing to rank.
  const int64_t local_count = failed ? 0 : count;

  struct ShamRankRecord *records = NULL;
  if (local_count > 0) {
    records = mymalloc_cat((size_t)local_count * sizeof(struct ShamRankRecord), MEM_UTILITY);
  }
  int64_t candidates = 0;
  int64_t assigned = 0;
  if (rank_into_scratch(ctx, halos, local_count, records, &candidates, &assigned) != 0) {
    failed = 1;
  }

  int64_t audit[2] = {candidates, assigned};
  if (module_snapshot_sum_i64(ctx, audit, 2) != 0) {
    failed = 1;
  }

  // No task writes unless every task succeeded.
  if (module_snapshot_any(ctx, failed) != 0) {
    if (records != NULL) {
      myfree(records);
    }
    ERROR_LOG("%s: snapshot %d (z=%.4f, %lld entries) failed; no stellar mass is assigned for it",
              SHAM_MODULE_NAME, ctx->snapshot_number, ctx->redshift, (long long)count);
    return -1;
  }

  for (int64_t k = 0; k < candidates; k++) {
    struct GalaxyData *gal = halos[records[k].index].galaxy;
    if (records[k].assigned == SHAM_RANK_ASSIGNED) {
      gal->StellarMass = records[k].mass;
      gal->ShamGhost = 0;
    } else {
      gal->StellarMass = 0.0f;
      gal->ShamGhost = 1;
    }
  }
  if (records != NULL) {
    myfree(records);
  }

  if (module_snapshot_is_root_task()) {
    INFO_LOG("SHAM audit z=%.4f candidates=%lld assigned=%lld masked=%lld", ctx->redshift,
             (long long)audit[0], (long long)audit[1], (long long)(audit[0] - audit[1]));
  }
  return 0;
}

int sham_rank_match_cleanup(void) {
  sham_ready = false;
  sham_rank_match_free_target(&sham_target);
  VERBOSE_LOG("SHAM rank match cleaned up");
  return 0;
}
