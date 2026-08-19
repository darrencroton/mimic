/**
 * @file    run_profile.c
 * @brief   Run-scope memory profile: peak RSS and the driver's sizing terms
 *
 * See run_profile.h for what C and G are and why peak RSS is the primary
 * number rather than a reconstructed sum.
 */

#include <inttypes.h>
#include <sys/resource.h>
#include <sys/time.h>

#include "error.h"
#include "run_profile.h"

/* Bytes per gigabyte, decimal. The memory projection these terms feed is
 * written in GB = 1e9 B, not GiB. */
#define RUN_PROFILE_BYTES_PER_GB 1.0e9

/* Run-level maxima. A byte size of 0 means the term was never noted.
 *
 * The two byte sizes are overwritten by each note rather than paired with the
 * maximum they belong to, which is safe because one binary compiles exactly one
 * model and one simulation package: `struct Halo` and `struct GalaxyData` each
 * have a single generated definition for the whole run, so every caller passes
 * the same value. If that ever stops holding, pair each size with its own term. */
static int64_t OutputBufferPopulation = 0;
static int64_t OutputBufferCapacity = 0;
static size_t OutputRecordBytes = 0;
static int64_t GalaxyPoolHighWater = 0;
static int64_t GalaxyPoolSlots = 0;
static int GalaxyPoolChunks = 0;
static size_t GalaxyBytes = 0;

void run_profile_note_output_buffer(int64_t count, int64_t capacity, size_t record_bytes) {
  if (count > OutputBufferPopulation)
    OutputBufferPopulation = count;
  if (capacity > OutputBufferCapacity)
    OutputBufferCapacity = capacity;
  OutputRecordBytes = record_bytes;
}

void run_profile_note_galaxy_pool(int64_t galaxies_high_water, int64_t slots_allocated,
                                  int chunk_count, size_t galaxy_bytes) {
  /* Each term keeps its own maximum, which is the per-generation upper bound the
   * projection wants -- not a sum -- however many pools a driver owns. Note the
   * snapshot driver's two pools alternate on snapshot parity, so they do NOT
   * both see the largest slab: only the pool whose parity matches it does. The
   * maximum across pools is therefore a conservative bound on any one
   * generation, which is what the projection multiplies. */
  if (galaxies_high_water > GalaxyPoolHighWater)
    GalaxyPoolHighWater = galaxies_high_water;
  if (slots_allocated > GalaxyPoolSlots)
    GalaxyPoolSlots = slots_allocated;
  if (chunk_count > GalaxyPoolChunks)
    GalaxyPoolChunks = chunk_count;
  GalaxyBytes = galaxy_bytes;
}

int64_t run_profile_peak_rss_bytes(void) {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return 0;

#ifdef __APPLE__
  /* Darwin reports ru_maxrss in bytes. */
  return (int64_t)usage.ru_maxrss;
#else
  /* Linux and the other BSDs report kilobytes. */
  return (int64_t)usage.ru_maxrss * 1024;
#endif
}

void print_run_memory_profile(void) {
  const int64_t rss = run_profile_peak_rss_bytes();

  /* INFO is the right semantic level -- this is a run milestone, not diagnostic
   * commentary -- but `--quiet` raises the threshold to WARNING and would
   * discard the whole block. That failure is silent and expensive: a production
   * run is exactly the case `--quiet` is documented for, and a run whose peak
   * went unrecorded cannot be re-measured without repeating it. Lower the
   * threshold for the duration of the report and restore it, so emission does
   * not depend on how the operator invoked the run. */
  const LogLevel saved_level = get_log_level();
  if (saved_level > LOG_LEVEL_INFO)
    set_log_level(LOG_LEVEL_INFO);

  INFO_LOG("Run memory profile (GB = 1e9 B):");

  if (rss > 0) {
    INFO_LOG("  Peak process RSS: %.3f GB", (double)rss / RUN_PROFILE_BYTES_PER_GB);
  } else {
    INFO_LOG("  Peak process RSS: unavailable on this platform");
  }

  if (OutputRecordBytes > 0) {
    INFO_LOG("  Output buffer capacity C: %" PRId64 " records at %zu B = %.3f GB",
             OutputBufferCapacity, OutputRecordBytes,
             (double)OutputBufferCapacity * (double)OutputRecordBytes / RUN_PROFILE_BYTES_PER_GB);
    INFO_LOG("  Output population P: %" PRId64 " records", OutputBufferPopulation);
  } else {
    INFO_LOG("  Output buffer capacity C and population P: unmeasured (no output buffer was "
             "sized)");
  }

  if (GalaxyBytes > 0) {
    INFO_LOG("  Galaxy pool high-water G: %" PRId64 " galaxies at %zu B = %.3f GB",
             GalaxyPoolHighWater, GalaxyBytes,
             (double)GalaxyPoolHighWater * (double)GalaxyBytes / RUN_PROFILE_BYTES_PER_GB);
    /* Slots allocated is what the pool actually holds resident; the difference
     * from G is chunk slack, which the projection treats as small but should
     * not have to assume. The subtraction cannot go negative even though the two
     * maxima may come from different pools: within any one pool slots >= its own
     * high-water, so max(slots) >= max(high-water). */
    const double slack =
        (GalaxyPoolSlots > 0)
            ? 100.0 * (double)(GalaxyPoolSlots - GalaxyPoolHighWater) / (double)GalaxyPoolSlots
            : 0.0;
    INFO_LOG("  Galaxy pool allocated: %" PRId64
             " slots in %d chunks = %.3f GB (%.1f%% chunk slack)",
             GalaxyPoolSlots, GalaxyPoolChunks,
             (double)GalaxyPoolSlots * (double)GalaxyBytes / RUN_PROFILE_BYTES_PER_GB, slack);
  } else {
    INFO_LOG("  Galaxy pool high-water G: unmeasured (no galaxy pool was reported)");
  }

  set_log_level(saved_level);
}
