/**
 * @file    sham_global_rank.c
 * @brief   Deterministic whole-snapshot rank abundance matching (uncalibrated)
 *
 * A snapshot-only module configured once in modules.post_snapshot. At every
 * snapshot it ranks the entire borrowed population (Types 0/1/2 across every
 * FoF group) by peak circular velocity and assigns each ranked galaxy the
 * stellar mass at which an explicit analytic cumulative stellar mass function
 * n(>M) = n0 (M / M0)^(-alpha) reaches the galaxy's rank density
 * (r + 0.5) / BoxSize^3. The target is a framework demonstration: it is not
 * fitted to any observation and claims no realistic stellar masses.
 *
 * The rank helper, sham_global_rank_assign(), is declared in sham_global_rank.h
 * so the unit tests can call it directly; see that header for its contract.
 *
 * References: Conroy et al. (2006), Vale & Ostriker (2006) for the
 * abundance-matching construction; Reddick et al. (2013) for Vpeak as the
 * ranking proxy.
 */

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "error.h"
#include "globals.h"
#include "memory.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

#include "module_system/parameter_helpers.h"

#include "sham_global_rank.h"

/** Configured name of this module and of the legacy prescription it excludes */
#define SHAM_GLOBAL_RANK_MODULE_NAME "sham_global_rank"
#define SHAM_LEGACY_MODULE_NAME "sham_assign_stellar_mass"

/** Domain of ShamGlobalNumberDensity, (Mpc/h)^-3, inclusive */
#define SHAM_GLOBAL_MIN_NUMBER_DENSITY 1.0e-12
#define SHAM_GLOBAL_MAX_NUMBER_DENSITY 1.0e3

/** Domain of ShamGlobalSlope, dimensionless, inclusive */
#define SHAM_GLOBAL_MIN_SLOPE 0.1
#define SHAM_GLOBAL_MAX_SLOPE 10.0

// ============================================================================
// MODULE STATE
// ============================================================================

/** Analytic target loaded and validated by init() */
static struct ShamGlobalRankTarget sham_target;

/** Whether init() completed; process_snapshot() refuses to run otherwise */
static int sham_target_ready = 0;

/**
 * @brief One population entry in sort scratch
 *
 * Scratch is the only thing ever sorted: the borrowed population keeps its
 * order. index is the entry's position in that population. vpeak and mpeak are
 * the validated updated peaks, kept so the write phase stores exactly what was
 * validated; mass is the rank's stellar mass (0 for an ineligible entry).
 */
struct ShamRankRecord {
  long long id;
  int64_t index;
  float vpeak;
  float mpeak;
  float mass;
};

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Fail a parameter that is NaN or infinite
 *
 * The range macros compare only against their bounds, so NaN passes them, and
 * the strict parser accepts "nan" and "inf"; every value is checked here first.
 */
static int check_finite_parameter(const char *name, double value) {
  if (!isfinite(value)) {
    ERROR_LOG("%s = %g is not finite", name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief Validate one entry before anything is written
 *
 * Checks the framework contract (Type 0/1/2, non-NULL galaxy, positive ID) and
 * every proxy the update consumes, and returns the updated peaks: Type 0/1
 * ratchet up from Vmax/Mvir, Type 2 keeps its inherited peaks. Mvir is double
 * and ShamMpeak float, so the updated mass peak is bounded by FLT_MAX before
 * it may be stored.
 */
static int updated_peaks(const struct Halo *halo, int64_t index, float *vpeak, float *mpeak) {
  if (halo->galaxy == NULL) {
    ERROR_LOG("sham_global_rank: entry %lld (UniqueGalaxyID %lld) has no galaxy", (long long)index,
              halo->UniqueGalaxyID);
    return -1;
  }
  if (halo->Type < 0 || halo->Type > 2) {
    ERROR_LOG("sham_global_rank: entry %lld (UniqueGalaxyID %lld) has Type %d; a snapshot "
              "population holds only Types 0/1/2",
              (long long)index, halo->UniqueGalaxyID, halo->Type);
    return -1;
  }
  if (halo->UniqueGalaxyID <= 0) {
    ERROR_LOG("sham_global_rank: entry %lld has non-positive UniqueGalaxyID %lld", (long long)index,
              halo->UniqueGalaxyID);
    return -1;
  }

  const struct GalaxyData *gal = halo->galaxy;
  const float previous_vpeak = gal->ShamVpeak;
  const float previous_mpeak = gal->ShamMpeak;
  if (!isfinite(previous_vpeak) || previous_vpeak < 0.0f || !isfinite(previous_mpeak) ||
      previous_mpeak < 0.0f) {
    ERROR_LOG("sham_global_rank: UniqueGalaxyID %lld has malformed peaks ShamVpeak=%g "
              "ShamMpeak=%g; both must be finite and nonnegative",
              halo->UniqueGalaxyID, (double)previous_vpeak, (double)previous_mpeak);
    return -1;
  }

  if (halo->Type == 2) {
    *vpeak = previous_vpeak;
    *mpeak = previous_mpeak;
    return 0;
  }

  if (!isfinite(halo->Vmax) || halo->Vmax < 0.0f || !isfinite(halo->Mvir) || halo->Mvir < 0.0) {
    ERROR_LOG("sham_global_rank: UniqueGalaxyID %lld has malformed proxies Vmax=%g Mvir=%.17g; "
              "both must be finite and nonnegative",
              halo->UniqueGalaxyID, (double)halo->Vmax, halo->Mvir);
    return -1;
  }
  const double mpeak_double = (halo->Mvir > (double)previous_mpeak) ? halo->Mvir : previous_mpeak;
  if (mpeak_double > (double)FLT_MAX) {
    ERROR_LOG("sham_global_rank: UniqueGalaxyID %lld peak mass %.17g exceeds the float "
              "ShamMpeak storage limit %.9g",
              halo->UniqueGalaxyID, mpeak_double, (double)FLT_MAX);
    return -1;
  }
  *vpeak = (halo->Vmax > previous_vpeak) ? halo->Vmax : previous_vpeak;
  *mpeak = (float)mpeak_double;
  if (!isfinite(*vpeak) || !isfinite(*mpeak)) {
    ERROR_LOG("sham_global_rank: UniqueGalaxyID %lld updated peaks ShamVpeak=%g ShamMpeak=%g "
              "are not finite floats",
              halo->UniqueGalaxyID, (double)*vpeak, (double)*mpeak);
    return -1;
  }
  return 0;
}

static int compare_by_id(const void *a, const void *b) {
  const long long id_a = ((const struct ShamRankRecord *)a)->id;
  const long long id_b = ((const struct ShamRankRecord *)b)->id;
  return (id_a > id_b) - (id_a < id_b);
}

/** Rank order: descending ShamVpeak, then ascending UniqueGalaxyID for exact ties */
static int compare_by_rank(const void *a, const void *b) {
  const struct ShamRankRecord *rec_a = a;
  const struct ShamRankRecord *rec_b = b;
  if (rec_a->vpeak > rec_b->vpeak) {
    return -1;
  }
  if (rec_a->vpeak < rec_b->vpeak) {
    return 1;
  }
  return (rec_a->id > rec_b->id) - (rec_a->id < rec_b->id);
}

/**
 * @brief ln M_r for zero-based rank r, entirely in logarithms
 *
 * Forms the bracket ln(r + 0.5) - 3 ln BoxSize - ln n0, divides it by alpha and
 * subtracts the result from ln M0. The rank density, the volume and their
 * ratio to n0 are never formed: they overflow or underflow for admitted
 * parameters whose final mass is representable. The steps are separate
 * statements for readability only: the build sets no -ffp-contract, so GCC's
 * default (-ffp-contract=fast) may still fuse across statements. Results are
 * deterministic within one build; bits may differ across platforms by rounding,
 * within the two-ULP bound the oracle tests assert.
 */
static double log_mass_at_rank(const struct ShamGlobalRankTarget *target, int64_t rank) {
  const double log_rank = log((double)rank + 0.5);
  const double three_log_box = 3.0 * log(target->box_size);
  const double bracket = log_rank - three_log_box - log(target->number_density);
  const double scaled = bracket / target->slope;
  return log(target->mass_scale) - scaled;
}

/**
 * @brief Float stellar mass for one rank, or -1 if it is out of range
 *
 * The order is fixed: the upper bound is decided on the computed logarithm
 * before exponentiation (exp(log(100000.0)) rounds above 100000), then the
 * float mass must be finite, nonzero and at most 100000. A positive float
 * subnormal is accepted as is. Nothing is clipped.
 */
static int mass_at_rank(const struct ShamGlobalRankTarget *target, int64_t rank, long long id,
                        float *mass) {
  const double log_mass = log_mass_at_rank(target, rank);
  const double log_upper = log(SHAM_GLOBAL_MAX_STELLAR_MASS);
  if (!(log_mass <= log_upper)) {
    ERROR_LOG("sham_global_rank: rank %lld (UniqueGalaxyID %lld) has ln M = %.17g above "
              "ln(%g) = %.17g (M0=%.17g n0=%.17g alpha=%.17g BoxSize=%.17g)",
              (long long)rank, id, log_mass, SHAM_GLOBAL_MAX_STELLAR_MASS, log_upper,
              target->mass_scale, target->number_density, target->slope, target->box_size);
    return -1;
  }
  const float stored = (float)exp(log_mass);
  if (!isfinite(stored) || stored == 0.0f || stored > (float)SHAM_GLOBAL_MAX_STELLAR_MASS) {
    ERROR_LOG("sham_global_rank: rank %lld (UniqueGalaxyID %lld) has ln M = %.17g, whose float "
              "mass %g is not in (0, %g] (M0=%.17g n0=%.17g alpha=%.17g BoxSize=%.17g)",
              (long long)rank, id, log_mass, (double)stored, SHAM_GLOBAL_MAX_STELLAR_MASS,
              target->mass_scale, target->number_density, target->slope, target->box_size);
    return -1;
  }
  *mass = stored;
  return 0;
}

static void reset_stellar_fields(struct GalaxyData *gal) {
  gal->StellarMass = 0.0f;
  gal->ShamStellarMassNoScatter = 0.0f;
  gal->ShamScatterDex = 0.0f;
  gal->BulgeMass = 0.0f;
  gal->MetalsStellarMass = 0.0f;
  gal->MetalsBulgeMass = 0.0f;
  gal->StarFormationRate = 0.0f;
}

/**
 * @brief Validate, rank and price every entry into scratch, writing nothing
 *
 * On success records[0, count) hold every entry in rank order: the eligible
 * entries (updated peak above zero) come first, records[0, *num_ranked), with
 * their masses; ineligible entries follow with mass 0. Every record carries its
 * validated updated peaks. Uniqueness covers every entry, eligible or not.
 */
static int rank_into_scratch(const struct ShamGlobalRankTarget *target, const struct Halo *halos,
                             int64_t count, struct ShamRankRecord *records, int64_t *num_ranked) {
  for (int64_t i = 0; i < count; i++) {
    float vpeak;
    float mpeak;
    if (updated_peaks(&halos[i], i, &vpeak, &mpeak) != 0) {
      return -1;
    }
    records[i].id = halos[i].UniqueGalaxyID;
    records[i].index = i;
    records[i].vpeak = vpeak;
    records[i].mpeak = mpeak;
    records[i].mass = 0.0f;
  }

  qsort(records, (size_t)count, sizeof(*records), compare_by_id);
  for (int64_t i = 1; i < count; i++) {
    if (records[i].id == records[i - 1].id) {
      ERROR_LOG("sham_global_rank: UniqueGalaxyID %lld appears more than once (entries %lld and "
                "%lld)",
                records[i].id, (long long)records[i - 1].index, (long long)records[i].index);
      return -1;
    }
  }

  // Peaks are nonnegative, so descending ShamVpeak puts every eligible entry first.
  qsort(records, (size_t)count, sizeof(*records), compare_by_rank);
  int64_t eligible = 0;
  while (eligible < count && records[eligible].vpeak > 0.0f) {
    eligible++;
  }
  for (int64_t rank = 0; rank < eligible; rank++) {
    if (mass_at_rank(target, rank, records[rank].id, &records[rank].mass) != 0) {
      return -1;
    }
  }
  *num_ranked = eligible;
  return 0;
}

int sham_global_rank_assign(const struct ShamGlobalRankTarget *target, const struct Halo *halos,
                            int64_t count) {
  if (target == NULL || count < 0 || (count > 0 && halos == NULL)) {
    ERROR_LOG("sham_global_rank: invalid call (target=%p, halos=%p, count=%lld)",
              (const void *)target, (const void *)halos, (long long)count);
    return -1;
  }
  if (count == 0) {
    return 0;
  }
  if ((uint64_t)count > SIZE_MAX / sizeof(struct ShamRankRecord)) {
    ERROR_LOG("sham_global_rank: population of %lld entries exceeds the addressable scratch size",
              (long long)count);
    return -1;
  }

  struct ShamRankRecord *records =
      mymalloc_cat((size_t)count * sizeof(struct ShamRankRecord), MEM_UTILITY);
  int64_t num_ranked = 0;
  if (rank_into_scratch(target, halos, count, records, &num_ranked) != 0) {
    myfree(records);
    return -1;
  }

  // Validation passed for every entry: only now write. Every entry, eligible or not, takes
  // its validated peaks and has its stellar fields reset; ranked entries then take their mass.
  for (int64_t k = 0; k < count; k++) {
    struct GalaxyData *gal = halos[records[k].index].galaxy;
    gal->ShamVpeak = records[k].vpeak;
    gal->ShamMpeak = records[k].mpeak;
    reset_stellar_fields(gal);
    if (k < num_ranked) {
      gal->StellarMass = records[k].mass;
      gal->ShamStellarMassNoScatter = records[k].mass;
    }
  }

  myfree(records);
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sham_global_rank_init(void) {
  sham_target_ready = 0;

  // Duplicate entries are rejected before init(), so presence is all that is checked here.
  if (!module_configured_in_phase(SHAM_GLOBAL_RANK_MODULE_NAME, MimicConfig.post_snapshot,
                                  MimicConfig.num_post_snapshot, PROCESSING_MODE_SNAPSHOT)) {
    ERROR_LOG("%s must be configured in modules.post_snapshot as process_snapshot",
              SHAM_GLOBAL_RANK_MODULE_NAME);
    return -1;
  }
  if (module_configured_anywhere(SHAM_LEGACY_MODULE_NAME)) {
    ERROR_LOG("%s and %s are independent stellar-mass prescriptions and would overwrite each "
              "other; configure only one",
              SHAM_GLOBAL_RANK_MODULE_NAME, SHAM_LEGACY_MODULE_NAME);
    return -1;
  }

  struct ShamGlobalRankTarget target;

  // Converted to internal mass units (parameter_units.yaml) before its range check.
  LOAD_PARAM_DOUBLE_INTERNAL("ShamGlobalMassScale", target.mass_scale);
  if (check_finite_parameter("ShamGlobalMassScale", target.mass_scale) != 0) {
    return -1;
  }
  VALIDATE_RANGE_EXCLUSIVE("ShamGlobalMassScale", target.mass_scale, 0.0,
                           SHAM_GLOBAL_MAX_STELLAR_MASS, "M0 in internal units of 1e10 Msun/h");

  // Fixed units (Mpc/h)^-3, not converted.
  LOAD_PARAM_DOUBLE("ShamGlobalNumberDensity", target.number_density);
  if (check_finite_parameter("ShamGlobalNumberDensity", target.number_density) != 0) {
    return -1;
  }
  VALIDATE_RANGE_INCLUSIVE("ShamGlobalNumberDensity", target.number_density,
                           SHAM_GLOBAL_MIN_NUMBER_DENSITY, SHAM_GLOBAL_MAX_NUMBER_DENSITY,
                           "n0 in (Mpc/h)^-3");

  // Dimensionless.
  LOAD_PARAM_DOUBLE("ShamGlobalSlope", target.slope);
  if (check_finite_parameter("ShamGlobalSlope", target.slope) != 0) {
    return -1;
  }
  VALIDATE_RANGE_INCLUSIVE("ShamGlobalSlope", target.slope, SHAM_GLOBAL_MIN_SLOPE,
                           SHAM_GLOBAL_MAX_SLOPE, "dimensionless slope alpha");

  // The cube is formed only for this domain check; rank evaluation stays in logarithms.
  target.box_size = MimicConfig.BoxSize;
  const double volume = target.box_size * target.box_size * target.box_size;
  if (!isfinite(target.box_size) || target.box_size <= 0.0 || !isfinite(volume) || volume <= 0.0) {
    ERROR_LOG("%s needs a finite positive BoxSize with a finite positive volume (BoxSize=%g "
              "Mpc/h, volume=%g (Mpc/h)^3)",
              SHAM_GLOBAL_RANK_MODULE_NAME, target.box_size, volume);
    return -1;
  }

  sham_target = target;
  sham_target_ready = 1;
  INFO_LOG("SHAM global rank initialized (uncalibrated target): M0=%.10g [1e10 Msun/h] "
           "n0=%.10g [(Mpc/h)^-3] alpha=%.10g BoxSize=%.10g [Mpc/h]",
           sham_target.mass_scale, sham_target.number_density, sham_target.slope,
           sham_target.box_size);
  return 0;
}

int sham_global_rank_process_snapshot(const struct SnapshotContext *ctx, const struct Halo *halos,
                                      int64_t count) {
  if (ctx == NULL || !sham_target_ready) {
    ERROR_LOG("sham_global_rank: process_snapshot called %s",
              ctx == NULL ? "without a snapshot context" : "before a successful init()");
    return -1;
  }
  if (sham_global_rank_assign(&sham_target, halos, count) != 0) {
    ERROR_LOG("sham_global_rank: snapshot %d (z=%.4f, %lld entries) failed; no stellar mass is "
              "accepted for it",
              ctx->snapshot_number, ctx->redshift, (long long)count);
    return -1;
  }
  return 0;
}

int sham_global_rank_cleanup(void) {
  sham_target_ready = 0;
  VERBOSE_LOG("SHAM global rank cleaned up");
  return 0;
}
