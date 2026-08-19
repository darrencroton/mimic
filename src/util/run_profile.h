#ifndef UTIL_RUN_PROFILE_H
#define UTIL_RUN_PROFILE_H

/**
 * @file    run_profile.h
 * @brief   Run-scope memory profile: peak RSS and the driver's sizing terms
 *
 * The snapshot driver's memory footprint is projected per live generation as a
 * fixed per-halo cost plus terms that are NOT fixed by the slab count. This unit
 * measures three of them, because none can be derived from the input:
 *
 *   C - the output buffer's realised capacity, in records. The buffer is only
 *       seeded from the halo count and grows geometrically whenever the output
 *       population exceeds it, so it overshoots what is actually filled.
 *   P - the largest output population any one live buffer reached, in records:
 *       the galaxies emitted plus the orphans carried forward. This is a separate
 *       measurement from C, and it is what MAX_HALO_ARRAY_SIZE must be checked
 *       against -- that ceiling is enforced on the live buffer during growth, so
 *       checking it against a slab count or against C misstates the headroom. P
 *       is scoped to whatever buffer the driver keeps live: one snapshot for the
 *       snapshot-ordered driver, one tree for the tree-ordered driver, which
 *       resets the buffer per tree.
 *
 *       The separate per-snapshot counter guard is NOT bounded by P under the
 *       tree-ordered driver: TotHalosPerSnap accumulates over every tree in an
 *       output partition (src/io/output/util.c), so a per-tree P says nothing
 *       about its headroom. Under the snapshot-ordered driver the two scopes
 *       coincide and P does bound it.
 *   G - the galaxy pool's allocation high-water, in galaxies. The pool serves
 *       every inherited progenitor galaxy and every newly initialised halo,
 *       including Type 3 galaxies that are never emitted, so the output count
 *       does not bound it.
 *
 * None of the three is reported anywhere else, and a projection that assumes
 * C = G = slab count is a case rather than an invariant. This unit collects them
 * as run-level maxima and reports them once at run end, next to peak process
 * RSS.
 *
 * Peak RSS is the primary number and the others are what it is checked
 * against: the tracked allocator's own high-water cannot see a realloc that
 * has to move its block (the old and new allocations coexist for the duration
 * of the copy), nor untracked and allocator-overhead residency. RSS subsumes
 * all of it.
 *
 * Sizes are reported in GB = 1e9 B, matching how the memory projection is
 * written. Note that the tracked allocator's own reports use MB = 1024^2, so
 * do not compare the two without converting.
 *
 * A term never noted is reported as unmeasured rather than as zero: a run that
 * marshalled nothing has no realised capacity, which is not the same claim as
 * a capacity of zero.
 *
 * Every term is collected from each contributing buffer and pool and reported as
 * a single run-level maximum; per-contributor values are not printed. The
 * maximum is the per-generation upper bound the projection multiplies, which is
 * what it needs, and it is conservative because no single generation can exceed
 * it.
 *
 * MPI: RSS is per process. The report is emitted by rank 0 only, so under a
 * multi-rank tree-ordered run it describes that rank and not the node total; a
 * node peak would need a max-reduce across ranks. Snapshot-ordered runs are
 * serial by configuration, so rank 0 is the whole run wherever these terms
 * matter most.
 *
 * Not thread-safe, consistent with the rest of the utility layer: MPI
 * rank-per-process isolation makes the file-static state safe under the
 * current model.
 */

#include <stddef.h>
#include <stdint.h>

/* Record an output buffer's realised population and capacity. Called wherever a
 * capacity is established or grown and wherever marshalling completes; keeps the
 * run-level maximum of each independently. `record_bytes` is the caller's sizeof
 * for one buffered record. */
void run_profile_note_output_buffer(int64_t count, int64_t capacity, size_t record_bytes);

/* Record a galaxy pool's statistics, normally just before the pool is
 * destroyed. Keeps the run-level maximum of each term, so a driver holding
 * more than one pool reports the largest rather than a sum -- the projection
 * multiplies a per-generation term by the number of live generations. */
void run_profile_note_galaxy_pool(int64_t galaxies_high_water, int64_t slots_allocated,
                                  int chunk_count, size_t galaxy_bytes);

/* Peak resident set size of this process in bytes, or 0 if the platform did
 * not report it. */
int64_t run_profile_peak_rss_bytes(void);

/* Log the profile as one block. Call once, at run end, after every pool has
 * been noted and while peak RSS is still the run's peak. */
void print_run_memory_profile(void);

#endif /* UTIL_RUN_PROFILE_H */
