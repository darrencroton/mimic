/**
 * @file    horizontal_driver.c
 * @brief   Horizontal run driver.
 *
 * Deliberately not named "*hdf5.c": Makefile:272 filters that suffix out of a
 * USE-HDF5=no build, and the horizontal dispatch case must compile and
 * link in every build (the reader interface symbols it calls are always
 * present; a non-HDF5 build already rejects tree_type: horizontal_hdf5 at
 * configuration, long before this driver runs). Every call into the HDF5
 * writers is therefore confined to the two output helpers below, which have
 * fail-fast stubs when HDF5 is absent.
 *
 * Where the vertical driver walks one forest's full history depth-first and holds
 * exactly one input generation live, this driver sweeps snapshots in increasing
 * time order and holds a pool of retained generations keyed by snapshot number.
 * Each generation's raw slab, output buffer and galaxy pool stay live until its
 * retention horizon -- the latest snapshot any of its halos names as its
 * descendant's -- has been processed, and are released then. For an adjacent
 * dataset (every version 2 dataset) that is today's two-generation rotation:
 * snapshot N is processed against N-1, which is released once every FoF group at
 * N has deep-copied what it inherits. Across a gap, a generation outlives the
 * snapshots its descendants skip, and no synthetic halo or generation is ever
 * created to bridge them.
 *
 * Each generation's resident bytes are computed from its halo count, the slab
 * row width the reader publishes and the driver's own struct widths before the
 * reader loads its slab, reported, and -- when input.retention_memory_ceiling_mb
 * is set -- refused if the retention pool with it would exceed that ceiling. The
 * run memory profile reports the most generations retained at once and the most
 * bytes resident across them.
 *
 * The physics, inheritance, marshalling and output seams are shared with the
 * vertical driver unchanged, and so are the module-context setup, the halo-evolution
 * dispatch, the FoF subhalo count and the halo init payload — this driver calls
 * process_halo_evolution(), count_fof_subhalos() and make_halo_init_payload()
 * in src/core/halo_evolution.c directly, passing its own workspace.
 *
 * What remains replicated here is only the code that crosses generations:
 * progenitor lookup, count and gather resolve each link through its
 * target-snapshot column into the retained generation it names, and have no
 * vertical-driver equivalent, because the vertical driver holds one generation
 * and finds progenitors inside it. Those three are still line-for-line
 * equivalents of find_most_massive_progenitor(), count_progenitor_galaxies() and
 * gather_progenitor_galaxies() modulo that substitution; the cross-format
 * identity gate rests on them staying that way, so each carries a reference to
 * its vertical-side original. FoF assembly (horizontal_join_progenitor_halos(),
 * horizontal_process_fof_group()) is likewise kept local because it is built on
 * those cross-generation lookups.
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "error.h"
#include "galaxy_id.h"
#include "galaxy_pool.h"
#include "globals.h"
#include "inheritance.h"
#include "memory.h"
#include "output_buffer.h"
#include "progress.h"
#include "proto.h"
#include "run_log.h"
#include "run_profile.h"
#include "horizontal/reader.h"
#include "types.h"

#include "output/util.h"

#ifdef HDF5
#include <hdf5.h>
#include "output/hdf5.h"
#endif

#include "generated/tree_property_accessors.h"

/* The `tree` argument of save_halos_hdf5(), which the shared output counter
 * ignores entirely in horizontal mode: output_increment_halo_counters_checked()
 * never touches the per-tree counters for a horizontal run, because only
 * the vertical reader allocates them. A horizontal run has no trees to number, so
 * this is a placeholder rather than a meaningful index. */
#define HORIZONTAL_OUTPUT_TREE_ID 0

/* Same bound as the vertical driver's MAX_PATH_BUF_SIZE in vertical_driver.c. */
#define HORIZONTAL_PATH_BUF_SIZE (3 * MAX_STRING_LEN + 25)

/* Output paths bye() unlinks if the program exits with a failure while they are
 * still armed. Two slots with independent lifetimes:
 *
 *   slot INFLIGHT - the partition file currently being written. Armed when a
 *     requested output snapshot's file is about to be created and released the
 *     moment that file closes cleanly, so a completed snapshot's output is never
 *     destroyed by a later failure. This is the vertical driver's own per-partition
 *     discipline (vertical_driver.c:311), applied to the horizontal side.
 *   slot MASTER - the run's master file. Armed once at run start and, unlike the
 *     vertical driver's registry, still armed when run_horizontal_driver() returns,
 *     because main.c writes the master afterwards; only a successful
 *     write_master_file() disarms it.
 *
 * An empty slot is skipped by both operations below, so the two lifetimes need
 * no bookkeeping beyond the strings themselves. */
#define HORIZONTAL_OUTPUT_PATH_INFLIGHT 0
#define HORIZONTAL_OUTPUT_PATH_MASTER 1
#define HORIZONTAL_OUTPUT_PATH_SLOTS 2

static char horizontal_output_paths[HORIZONTAL_OUTPUT_PATH_SLOTS][HORIZONTAL_PATH_BUF_SIZE + 1];

/* The fixed extent of the array above, not a running count of armed slots. */
static const int horizontal_output_path_count = HORIZONTAL_OUTPUT_PATH_SLOTS;

void horizontal_driver_clear_output_paths(void) {
  int armed = 0;

  for (int i = 0; i < horizontal_output_path_count; i++) {
    if (horizontal_output_paths[i][0] != '\0') {
      armed++;
    }
    horizontal_output_paths[i][0] = '\0';
  }

  if (armed > 0) {
    VERBOSE_LOG("Disarming snapshot output cleanup for %d remaining path%s", armed,
                armed == 1 ? "" : "s");
  }
}

void horizontal_driver_remove_incomplete_outputs(void) {
  for (int i = 0; i < horizontal_output_path_count; i++) {
    if (horizontal_output_paths[i][0] != '\0') {
      unlink(horizontal_output_paths[i]);
    }
  }
}

#ifdef HDF5
/* The three arming operations below are HDF5-only, as their sole callers are:
 * without HDF5 the driver's output helpers fail fast (see below) and arming the
 * registry would let bye() unlink a completed earlier run's files on the way
 * out. */

/* Arm the master file for cleanup, once, at run start.
 *
 * The path is formatted the way write_master_file() formats it (master_hdf5.c)
 * rather than through a shared helper: adding one would mean editing an
 * output-writer seam this driver only consumes. */
static void horizontal_arm_master_output_path(void) {
  const int written =
      snprintf(horizontal_output_paths[HORIZONTAL_OUTPUT_PATH_MASTER], HORIZONTAL_PATH_BUF_SIZE,
               "%s/%s.hdf5", MimicConfig.OutputDir, MimicConfig.OutputFileBaseName);
  if (written < 0 || written >= HORIZONTAL_PATH_BUF_SIZE) {
    FATAL_ERROR("Master HDF5 output path too long: %s/%s.hdf5", MimicConfig.OutputDir,
                MimicConfig.OutputFileBaseName);
  }

  VERBOSE_LOG("Snapshot master output cleanup armed for '%s'",
              horizontal_output_paths[HORIZONTAL_OUTPUT_PATH_MASTER]);
}

/* Arm the partition file about to be created for output id @p output_id. Armed
 * before H5Fcreate, so a failure that leaves a half-created file behind still
 * has that file registered for removal. */
static void horizontal_arm_partition_output_path(int output_id) {
  output_path_hdf5(horizontal_output_paths[HORIZONTAL_OUTPUT_PATH_INFLIGHT],
                   HORIZONTAL_PATH_BUF_SIZE, output_id);

  VERBOSE_LOG("Snapshot partition output cleanup armed for '%s'",
              horizontal_output_paths[HORIZONTAL_OUTPUT_PATH_INFLIGHT]);
}

/* Release the in-flight partition slot after its file has closed cleanly: that
 * file is now final output and must survive any later failure. */
static void horizontal_clear_partition_output_path(void) {
  horizontal_output_paths[HORIZONTAL_OUTPUT_PATH_INFLIGHT][0] = '\0';
}
#endif /* HDF5 */

/*
 * One retained slab generation: the raw halos of one snapshot (with every
 * reader-owned array its slab carries), where each of them landed in that
 * snapshot's output buffer, the buffer itself, the pool that owns its galaxies,
 * and the snapshot after whose processing all of it may go (its retention
 * horizon, fixed at load).
 */
struct HorizontalGeneration {
  int64_t snapnum;               /* retained snapshot, or SNAPSHOT_SLAB_NO_SNAPSHOT */
  int64_t horizon;               /* release once this snapshot has been processed */
  struct SnapshotSlab slab;      /* reader-owned raw halos and format arrays */
  struct HorizontalHaloAux *aux; /* [slab.nhalos] */
  struct OutputBuffer processed;
  struct GalaxyPool *pool;
};

/*
 * Driver-scoped state.
 *
 * The retention pool is `generations`, indexed by snapshot number: slot k holds
 * snapshot k from its load until its horizon has been processed, and is empty
 * otherwise. `lookup` is the same pool as the progenitor lookup reads it; a slot
 * there is published only once its snapshot's sweep has finished, so a link can
 * never resolve into a generation still being built. The driver is the single
 * owner of every retained generation -- the reader holds no retention state --
 * and `retained_count`/`retained_population` are derived from the pool's own
 * contents at each load and release.
 *
 * Galaxy pools are recycled rather than destroyed at release: `spare_pools`
 * holds the reset pools no retained generation is using, and every pool the run
 * ever created is either there or on a retained generation, so an adjacent run
 * never needs more than the two pools today's rotation used.
 *
 * The workspace and the two scratch buffers are grown monotonically and kept for
 * the whole run (as the vertical driver's equivalents are), then freed before the
 * driver returns. Their capacities are int64_t like every slab index and count
 * they are sized from.
 */
struct HorizontalDriverState {
  const struct HorizontalReader *reader;
  int run_open; /* the reader's run is open and must be closed */
  int64_t snapshot_count;
  int64_t slab_row_bytes; /* per-row width the reader published at open */

  struct HorizontalGeneration *generations;    /* [snapshot_count] */
  struct HorizontalRetainedGeneration *lookup; /* [snapshot_count] */
  int64_t retained_count;                      /* generations currently retained */
  int64_t retained_population;                 /* halos across them */
  int64_t max_retained_count;                  /* peak concurrently retained */
  int64_t max_retained_bytes;                  /* peak resident across the pool */
  int warned_sweep_over_ceiling;               /* in-sweep growth passed the ceiling */

  struct GalaxyPool **spare_pools; /* [spare_capacity] */
  int64_t spare_count;
  int64_t spare_capacity;

  struct Halo *workspace;
  int64_t workspace_capacity;

  struct InheritanceProgenitorGalaxy *progenitor_scratch;
  int64_t progenitor_capacity;

  struct OutputBufferSegment *segments;
  int64_t segment_capacity;
};

/* ------------------------------------------------------------------------- */
/* Scratch growth                                                             */
/* ------------------------------------------------------------------------- */

/* Mirrors ensure_fof_workspace_capacity() in build_model.c: same growth
 * factor, same minimum increment, same ceiling, same fatal. */
static void horizontal_ensure_workspace_capacity(struct HorizontalDriverState *state,
                                                 int64_t required) {
  while (required > state->workspace_capacity) {
    const int64_t old_size = state->workspace_capacity;
    int64_t new_size = (int64_t)(state->workspace_capacity * HALO_ARRAY_GROWTH_FACTOR);

    if (new_size - state->workspace_capacity < MIN_HALO_ARRAY_GROWTH)
      new_size = state->workspace_capacity + MIN_HALO_ARRAY_GROWTH;

    if (new_size > MAX_HALO_ARRAY_SIZE)
      new_size = MAX_HALO_ARRAY_SIZE;

    if (new_size <= state->workspace_capacity) {
      FATAL_ERROR("Snapshot FoF workspace requires %" PRId64
                  " halos but maximum allowed size is %d",
                  required, MAX_HALO_ARRAY_SIZE);
    }

    INFO_LOG("Growing snapshot halo workspace from %" PRId64 " to %" PRId64 " elements",
             state->workspace_capacity, new_size);

    state->workspace_capacity = new_size;
    state->workspace =
        myrealloc_cat(state->workspace, (size_t)new_size * sizeof(struct Halo), MEM_HALOS);
    memset(&state->workspace[old_size], 0, (size_t)(new_size - old_size) * sizeof(struct Halo));
  }
}

static struct InheritanceProgenitorGalaxy *
horizontal_ensure_progenitor_scratch(struct HorizontalDriverState *state, int64_t required) {
  if (required > state->progenitor_capacity) {
    state->progenitor_scratch =
        myrealloc_cat(state->progenitor_scratch,
                      (size_t)required * sizeof(struct InheritanceProgenitorGalaxy), MEM_HALOS);
    state->progenitor_capacity = required;
  }
  return state->progenitor_scratch;
}

static struct OutputBufferSegment *
horizontal_ensure_segment_scratch(struct HorizontalDriverState *state, int64_t required) {
  if (required > state->segment_capacity) {
    state->segments = myrealloc_cat(
        state->segments, (size_t)required * sizeof(struct OutputBufferSegment), MEM_HALOS);
    state->segment_capacity = required;
  }
  return state->segments;
}

/* ------------------------------------------------------------------------- */
/* Progenitor lookup and gather (the parity-critical replications)            */
/* ------------------------------------------------------------------------- */

/*
 * One step of a progenitor chain: the retained generation a link resolved into
 * and the row it names there. `halonr` is -1 once the chain has ended, and
 * `generation` is then NULL.
 */
struct HorizontalProgenitorCursor {
  const struct HorizontalRetainedGeneration *generation;
  int64_t halonr;
};

/*
 * Resolve one progenitor link into its retained generation.
 *
 * `target_snap` is the snapshot the link names: its target-snapshot column for
 * version 3, or the version 2 implicit scope. load_slab has already validated
 * every version 3 link against the file its column names, and every version 2
 * link against N-1 or its own slab, so each check below guards the driver's own
 * retention rather than the input: a target that is not retained here would mean
 * a generation was released before its horizon.
 */
static const struct HorizontalRetainedGeneration *
horizontal_resolve_progenitor(const struct HorizontalGatherContext *lookup, int64_t target_snap,
                              int64_t prog, int64_t halonr, const char *link) {
  if (target_snap < 0 || target_snap >= lookup->snapnum) {
    FATAL_ERROR("%s link of snapshot %" PRId64 " halo %" PRId64 " names snapshot %" PRId64
                ", which is not an earlier snapshot of this run",
                link, lookup->snapnum, halonr, target_snap);
  }

  const struct HorizontalRetainedGeneration *generation = &lookup->generations[target_snap];
  if (generation->snapnum != target_snap) {
    FATAL_ERROR("%s link of snapshot %" PRId64 " halo %" PRId64 " names snapshot %" PRId64
                ", whose generation is not retained: either the input's progenitor chain names "
                "a generation its own DescendantSnapshot did not keep alive, or a generation "
                "was released before its retention horizon",
                link, lookup->snapnum, halonr, target_snap);
  }
  if (prog >= generation->view.count) {
    FATAL_ERROR("%s link of snapshot %" PRId64 " halo %" PRId64 " names row %" PRId64
                " of snapshot %" PRId64 ", which holds %" PRId64 " halos",
                link, lookup->snapnum, halonr, prog, target_snap, generation->view.count);
  }

  return generation;
}

/* The head of halonr's progenitor chain: FirstProgenitor, resolved through the
 * descendant slab's FirstProgenitorSnapshot (version 3) or into N-1 (version 2). */
static struct HorizontalProgenitorCursor
horizontal_first_progenitor(struct HaloInputView view, const struct HorizontalGatherContext *lookup,
                            int64_t halonr) {
  struct HorizontalProgenitorCursor cursor = {NULL, mimic_tree_get_FirstProgenitor(view, halonr)};

  if (cursor.halonr >= 0) {
    const int64_t target_snap = (lookup->first_progenitor_snapshot != NULL)
                                    ? lookup->first_progenitor_snapshot[halonr]
                                    : lookup->snapnum - 1;
    cursor.generation = horizontal_resolve_progenitor(lookup, target_snap, cursor.halonr, halonr,
                                                      "FirstProgenitor");
  }

  return cursor;
}

/* Advance a chain by one NextProgenitor link, resolved through the current
 * entry's own NextProgenitorSnapshot (version 3) or inside its own slab
 * (version 2). A sibling may sit in a different snapshot from its predecessor,
 * earlier or later, because the constraint is relative to the shared descendant,
 * not to the owner. */
static void horizontal_next_progenitor(const struct HorizontalGatherContext *lookup,
                                       struct HorizontalProgenitorCursor *cursor, int64_t halonr) {
  const struct HorizontalRetainedGeneration *owner = cursor->generation;
  const int64_t next = mimic_tree_get_NextProgenitor(owner->view, cursor->halonr);

  if (next < 0) {
    cursor->generation = NULL;
    cursor->halonr = -1;
    return;
  }

  const int64_t target_snap = (owner->next_progenitor_snapshot != NULL)
                                  ? owner->next_progenitor_snapshot[cursor->halonr]
                                  : owner->snapnum;
  cursor->generation =
      horizontal_resolve_progenitor(lookup, target_snap, next, halonr, "NextProgenitor");
  cursor->halonr = next;
}

static struct HorizontalProgenitorRef
horizontal_progenitor_ref(struct HorizontalProgenitorCursor cursor) {
  struct HorizontalProgenitorRef ref = {-1, -1};
  if (cursor.halonr >= 0) {
    ref.snapnum = cursor.generation->snapnum;
    ref.halonr = cursor.halonr;
  }
  return ref;
}

/* The chain walk's cycle guard. Every retained halo can be visited at most once
 * by one chain, so more steps than the retained population means the input's
 * NextProgenitor links form a cycle, which would otherwise loop forever. The
 * bound is the whole retained population because a chain may cross every
 * retained generation. */
static void horizontal_check_chain_steps(const struct HorizontalGatherContext *lookup,
                                         int64_t steps, int64_t halonr) {
  if (steps > lookup->retained_population) {
    FATAL_ERROR("Progenitor chain of snapshot %" PRId64 " halo %" PRId64
                " visits more than the %" PRId64
                " halos of every retained generation; the input's NextProgenitor links "
                "contain a cycle",
                lookup->snapnum, halonr, lookup->retained_population);
  }
}

/*
 * Horizontal-side find_most_massive_progenitor() in build_model.c.
 *
 * The chain is followed exactly as stored: FirstProgenitor into the generation
 * its target snapshot names, then each NextProgenitor into its own. Occupancy
 * and Len are read from whichever retained generation each entry lives in,
 * never through `view`.
 *
 * Selection is the vertical-side rule unchanged: an occupied FirstProgenitor pins
 * the answer (lenoccmax = -1 disables further replacement), and otherwise the
 * chain is scanned in order and replaced only on a strict Len increase. The
 * answer is a generation and row rather than a row alone, so the gather below
 * recognises the main branch even where slab indices of different generations
 * coincide.
 */
struct HorizontalProgenitorRef horizontal_find_most_massive_progenitor(
    struct HaloInputView view, const struct HorizontalGatherContext *lookup, int64_t halonr) {
  struct HorizontalProgenitorCursor prog;
  struct HorizontalProgenitorRef first_occupied;
  int lenoccmax;
  int64_t steps = 0;

  lenoccmax = 0;
  prog = horizontal_first_progenitor(view, lookup, halonr);
  first_occupied = horizontal_progenitor_ref(prog);

  if (prog.halonr >= 0)
    if (prog.generation->aux[prog.halonr].NHalos > 0)
      lenoccmax = -1;

  while (prog.halonr >= 0) {
    /* First traversal of this chain in the FoF sweep, so it carries its own
     * cycle guard. */
    horizontal_check_chain_steps(lookup, ++steps, halonr);
    if (lenoccmax != -1 && mimic_tree_get_Len(prog.generation->view, prog.halonr) > lenoccmax &&
        prog.generation->aux[prog.halonr].NHalos > 0) {
      lenoccmax = mimic_tree_get_Len(prog.generation->view, prog.halonr);
      first_occupied = horizontal_progenitor_ref(prog);
    }
    horizontal_next_progenitor(lookup, &prog, halonr);
  }

  return first_occupied;
}

/* Horizontal-side count_progenitor_galaxies() in build_model.c. */
int64_t horizontal_count_progenitor_galaxies(struct HaloInputView view,
                                             const struct HorizontalGatherContext *lookup,
                                             int64_t halonr) {
  int64_t count = 0;
  int64_t steps = 0;
  struct HorizontalProgenitorCursor prog = horizontal_first_progenitor(view, lookup, halonr);

  while (prog.halonr >= 0) {
    horizontal_check_chain_steps(lookup, ++steps, halonr);
    count += prog.generation->aux[prog.halonr].NHalos;
    horizontal_next_progenitor(lookup, &prog, halonr);
  }

  return count;
}

/*
 * Horizontal-side gather_progenitor_galaxies() in build_model.c.
 *
 * Visit order is load-bearing for cross-format identity: each progenitor chain
 * entry in chain order, then that halo's own output range in order, from the
 * output buffer of the generation that entry lives in. source_time comes from
 * the stored SnapNum of the source galaxy, exactly as the vertical side takes it,
 * so a galaxy inherited across a gap carries its own snapshot's age rather than
 * N-1's.
 *
 * No cycle guard here: horizontal_count_progenitor_galaxies() walked this exact
 * chain, over the same immutable generations, immediately before.
 */
void horizontal_gather_progenitor_galaxies(struct HaloInputView view,
                                           const struct HorizontalGatherContext *lookup,
                                           int64_t halonr,
                                           struct HorizontalProgenitorRef first_occupied,
                                           struct InheritanceProgenitorGalaxy *progenitors) {
  int64_t index = 0;
  struct HorizontalProgenitorCursor prog = horizontal_first_progenitor(view, lookup, halonr);

  while (prog.halonr >= 0) {
    const struct HorizontalRetainedGeneration *generation = prog.generation;
    const int is_main_branch =
        (generation->snapnum == first_occupied.snapnum && prog.halonr == first_occupied.halonr);

    for (int64_t i = 0; i < generation->aux[prog.halonr].NHalos; i++) {
      const struct Halo *source =
          &generation->processed[generation->aux[prog.halonr].FirstHalo + i];
      progenitors[index].source = source;
      progenitors[index].source_time = Age[source->SnapNum];
      progenitors[index].is_main_branch = is_main_branch;
      index++;
    }

    horizontal_next_progenitor(lookup, &prog, halonr);
  }
}

/* ------------------------------------------------------------------------- */
/* Identity                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Horizontal-side make_unique_galaxy_id() in build_model.c.
 *
 * The two components are carried by the format in reference vertical-driver order
 * (HORIZONTAL-HDF5-FORMAT.md "Galaxy Identity Encoding") and live on the slab, not
 * on struct RawHalo, so they are read from the reader's own arrays. struct
 * Halo.HaloNr keeps the slab index, which is what the output-conversion virial
 * recomputation indexes the slab view with; HaloRankInForest never goes there.
 */
static int64_t horizontal_make_unique_galaxy_id(const struct SnapshotSlab *slab, int64_t halonr) {
  const int64_t multiplier = MimicConfig.UniqueGalaxyIDMultiplier;
  const int64_t rank_in_forest = slab->halo_rank_in_forest[halonr];
  const int64_t forestnr_global = slab->forest_index[halonr];

  if (!mimic_unique_galaxy_id_components_valid(multiplier, rank_in_forest, forestnr_global)) {
    FATAL_ERROR("UniqueGalaxyID components out of range at snapshot %" PRId64 " halo %" PRId64 ": "
                "HaloRankInForest=%" PRId64 ", ForestIndex=%" PRId64 " (limits: rank < %" PRId64
                ", forest index < %" PRId64 ")",
                slab->snapnum, halonr, rank_in_forest, forestnr_global, multiplier,
                mimic_unique_galaxy_id_max_forests(multiplier));
  }

  return mimic_encode_unique_galaxy_id(multiplier, rank_in_forest, forestnr_global);
}

/* ------------------------------------------------------------------------- */
/* FoF assembly and physics                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Horizontal-side join_progenitor_halos() in build_model.c.
 *
 * Every descendant field is derived exactly as the vertical side derives it; the
 * only substitutions are the retained-generation lookup (which crosses slabs,
 * and across a gap crosses several) and the identity encoding (which reads the
 * format's carried components).
 */
static int64_t horizontal_join_progenitor_halos(struct HorizontalDriverState *state,
                                                struct HorizontalGeneration *cur,
                                                const struct HorizontalGatherContext *lookup,
                                                int64_t halonr, int64_t ngalstart) {
  const struct HaloInputView view = {cur->slab.halos, cur->slab.nhalos};
  struct InheritanceDescendant descendant;
  struct InheritanceProgenitorGalaxy *progenitors = NULL;
  struct HorizontalProgenitorRef first_occupied;
  int current_snap;
  int64_t required;

  first_occupied = horizontal_find_most_massive_progenitor(view, lookup, halonr);

  const int64_t nprogenitors = horizontal_count_progenitor_galaxies(view, lookup, halonr);

  required = ngalstart + nprogenitors;
  if (nprogenitors == 0 && halonr == mimic_tree_get_FirstHaloInFOFgroup(view, halonr)) {
    required++;
  }
  horizontal_ensure_workspace_capacity(state, required);

  if (nprogenitors > 0) {
    progenitors = horizontal_ensure_progenitor_scratch(state, nprogenitors);
    horizontal_gather_progenitor_galaxies(view, lookup, halonr, first_occupied, progenitors);
  }

  current_snap = mimic_tree_get_SnapNum(view, halonr);
  descendant.halo_nr = halonr;
  descendant.current_snap = current_snap;
  descendant.current_time = Age[current_snap];
  descendant.new_halo_dt = (current_snap > 0) ? Age[current_snap - 1] - Age[current_snap] : -1.0;
  descendant.virial_mass = get_virial_mass(view, halonr);
  descendant.virial_radius = get_virial_radius(view, halonr);
  descendant.virial_velocity = get_virial_velocity(view, halonr);
  descendant.is_fof_central = (halonr == mimic_tree_get_FirstHaloInFOFgroup(view, halonr));
  descendant.unique_galaxy_id = horizontal_make_unique_galaxy_id(&cur->slab, halonr);
  descendant.halo_payload = make_halo_init_payload(view, halonr);

  return inherit_descendant_halos(cur->pool, state->workspace, ngalstart, state->workspace_capacity,
                                  &descendant, progenitors, nprogenitors);
}

/*
 * Process one FoF group of snapshot N: build its workspace subhalo slice by
 * subhalo slice, evolve it, and marshal it into this generation's output
 * buffer.
 *
 * This is the body of build_halo_tree()'s FoF block in build_model.c
 * with the recursion removed: a snapshot slab needs none, because every
 * progenitor lives in an earlier snapshot, which was swept before this one and
 * is still retained until its horizon.
 *
 * @return  Number of subhalos in the group (its members are now accounted for).
 */
static int64_t horizontal_process_fof_group(struct HorizontalDriverState *state,
                                            struct HorizontalGeneration *cur,
                                            const struct HorizontalGatherContext *lookup,
                                            int64_t central) {
  const struct HaloInputView view = {cur->slab.halos, cur->slab.nhalos};
  const int64_t nsegments = count_fof_subhalos(view, central);
  struct OutputBufferSegment *segments = horizontal_ensure_segment_scratch(state, nsegments);
  int64_t segment_index = 0;
  int64_t fofhalo = central;
  int64_t ngal = 0;

  /* Cycle safety: count_fof_subhalos() above already walked this exact chain
   * over the same immutable slab with a bounded-iteration guard, so this second
   * traversal cannot loop. */
  while (fofhalo >= 0) {
    const int64_t workspace_start = ngal;
    const int64_t source_halo = fofhalo;

    ngal = horizontal_join_progenitor_halos(state, cur, lookup, fofhalo, ngal);

    /* Stamp the FoF-central catalog virial mass onto every member of this
     * subhalo slice before physics runs, exactly as the tree FoF block in build_model.c. */
    const double central_mvir =
        get_virial_mass(view, mimic_tree_get_FirstHaloInFOFgroup(view, source_halo));
    for (int64_t p = workspace_start; p < ngal; p++) {
      state->workspace[p].CentralMvir = central_mvir;
    }

    segments[segment_index].source_id = source_halo;
    segments[segment_index].snapshot_number = mimic_tree_get_SnapNum(view, source_halo);
    segments[segment_index].workspace_start = workspace_start;
    segments[segment_index].workspace_count = ngal - workspace_start;
    segments[segment_index].output_first = -1;
    segments[segment_index].output_count = 0;
    segment_index++;

    fofhalo = mimic_tree_get_NextHaloInFOFgroup(view, fofhalo);
  }

  process_halo_evolution(view, state->workspace, central, ngal);

  marshal_workspace_to_output_buffer(state->workspace, &cur->processed, segments, segment_index);

  for (int64_t i = 0; i < segment_index; i++) {
    cur->aux[segments[i].source_id].FirstHalo = segments[i].output_first;
    cur->aux[segments[i].source_id].NHalos = segments[i].output_count;
  }

  return segment_index;
}

/* ------------------------------------------------------------------------- */
/* Output (the only HDF5-dependent code in this driver)                       */
/* ------------------------------------------------------------------------- */

/*
 * Return the vertical driver's output-buffer globals to their unowned state.
 *
 * This driver owns one output buffer per retained generation and lends one to
 * the shared writer for the duration of a single save call (see
 * horizontal_write_output). Outside that window the globals must point at
 * nothing: the generation they were lent from
 * is freed at its release, so leaving them set would leave a dangling pointer
 * live for the rest of the run for any shared code that reads them.
 */
static void horizontal_clear_output_globals(void) {
  ProcessedHalos = NULL;
  NumProcessedHalos = 0;
  MaxProcessedHalos = 0;
}

#ifdef HDF5

/*
 * Prepare the run's output without creating any file yet.
 *
 * A partition file appears only when its own snapshot finishes, so
 * there is nothing to open here: this zeroes the per-snapshot output counters
 * the writers accumulate into and arms the master file for cleanup, and the rest
 * of the lifecycle belongs to horizontal_write_output() below.
 */
static void horizontal_open_output(void) {
  horizontal_arm_master_output_path();

  for (int n = 0; n < MimicConfig.NOUT; n++) {
    TotHalosPerSnap[n] = 0;
  }
}

/*
 * Write one requested output snapshot to its own partition file, start to
 * finish, and close it before returning.
 *
 * @param   cur            The generation holding this snapshot's processed halos.
 * @param   output_index   Index of this snapshot in MimicConfig.ListOutputSnaps,
 *                         which is also its partition index.
 * @param   selection      That partition's selection — this one snapshot.
 *
 * The file is created, filled, stamped and closed inside this call, so a
 * finished snapshot's output is final the moment this returns and the driver
 * never holds a second writable output file open. Its cleanup registration is
 * armed before the file is created and released after it closes cleanly, so a
 * later failure removes only whatever was still in flight.
 *
 * save_halos_hdf5() reads the driver's output buffer through the ProcessedHalos
 * globals and converts each record through the supplied view, so the globals
 * are pointed at this generation and the view is this snapshot's slab — which
 * is exactly why the raw slab must still be live here (output conversion
 * recomputes Rvir/Vvir from it). The loan lasts exactly as long as the save
 * call: this generation's buffer is freed when its release comes, so the
 * globals are cleared again on the way out rather than left pointing into freed
 * memory.
 */
static void horizontal_write_output(struct HorizontalGeneration *cur, int output_index,
                                    struct OutputSnapshotSelection selection) {
  const struct HaloInputView view = {cur->slab.halos, cur->slab.nhalos};
  const int output_id = MimicConfig.ListOutputSnaps[output_index];

  /* This partition's output id, as the vertical driver sets it per partition
   * (vertical_driver.c:203). */
  FileNum = output_id;

  horizontal_arm_partition_output_path(output_id);
  prepare_output_files(output_id, selection);

  ProcessedHalos = cur->processed.halos;
  NumProcessedHalos = cur->processed.count;
  MaxProcessedHalos = cur->processed.capacity;

  save_halos_hdf5(output_id, HORIZONTAL_OUTPUT_TREE_ID, view, selection);

  horizontal_clear_output_globals();

  flush_hdf5_buffers(output_id, selection);

  /* This partition holds exactly one Snap%03d group, so it is stamped for its
   * own snapshot index alone; the other requested snapshots' groups do not
   * exist in this file. */
  write_hdf5_attrs(output_index, output_id);

  if (HDF5_current_file_id >= 0) {
    DEBUG_LOG("Closing HDF5 file (ID %lld) for horizontal partition %d",
              (long long)HDF5_current_file_id, output_id);
    /* A deferred write error (a full filesystem, a failed flush of a chunk
     * still in the library cache) surfaces here and nowhere else, so an ignored
     * status would let a truncated partition exit successfully. */
    const herr_t close_status = H5Fclose(HDF5_current_file_id);
    HDF5_current_file_id = -1;
    if (close_status < 0) {
      FATAL_ERROR("Failed to close the HDF5 output partition %d ('%s'); its galaxy data may be "
                  "truncated or unflushed (check free space and file permissions)",
                  output_id, horizontal_output_paths[HORIZONTAL_OUTPUT_PATH_INFLIGHT]);
    }
  }

  horizontal_clear_partition_output_path();

  /* VERBOSE_LOG, not DEBUG_LOG: this driver enables the vertical driver's debug
   * rate limiting for the physics phase, which caps each DEBUG_LOG site at
   * DEBUG_LOG_MAX_CALLS. These lifecycle lines are bounded by the snapshot
   * count, not by halo count, and are the operator's (and the integration
   * suite's) evidence of the retention schedule, so they must not be capped. */
  VERBOSE_LOG("Wrote snapshot %" PRId64 " output (%" PRId64 " galax%s) to partition %d",
              cur->snapnum, cur->processed.count, cur->processed.count == 1 ? "y" : "ies",
              output_id);
}

#else /* !HDF5 */

/*
 * Unreachable in practice: horizontal_hdf5 is the only registered horizontal
 * reader, and a non-HDF5 build registers none, so a horizontal
 * configuration is rejected at startup long before the driver runs. These stubs
 * exist so this translation unit links in a USE-HDF5=no build without any HDF5
 * writer symbol, and fail loudly rather than silently producing nothing if that
 * ever stops being true.
 */
#define HORIZONTAL_NO_HDF5_MESSAGE                                                                 \
  "Horizontal runs require an HDF5-enabled build; rebuild with USE-HDF5=yes"

static void horizontal_open_output(void) { FATAL_ERROR(HORIZONTAL_NO_HDF5_MESSAGE); }

static void horizontal_write_output(struct HorizontalGeneration *cur, int output_index,
                                    struct OutputSnapshotSelection selection) {
  (void)cur;
  (void)output_index;
  (void)selection;
  FATAL_ERROR(HORIZONTAL_NO_HDF5_MESSAGE);
}

#endif /* HDF5 */

/*
 * Index of `snapnum` in MimicConfig.ListOutputSnaps, or -1 if this snapshot was
 * not requested for output.
 *
 * The index is also the output partition this snapshot's galaxies belong to, so
 * a hit names both the requested-snapshot slot the writers stamp and the file
 * they write. output.snapshot_list may be unsorted, so this is a scan rather
 * than a search.
 */
static int horizontal_output_snapshot_index(int64_t snapnum) {
  for (int n = 0; n < MimicConfig.NOUT; n++) {
    if ((int64_t)MimicConfig.ListOutputSnaps[n] == snapnum) {
      return n;
    }
  }
  return -1;
}

/* ------------------------------------------------------------------------- */
/* Generation lifecycle and retention                                         */
/* ------------------------------------------------------------------------- */

/*
 * The retention horizon of a loaded slab (Consumer design review, "Retained
 * gap-state ownership"): the latest snapshot any of its halos names as its
 * descendant's, or the slab's own snapshot when none of its halos has a
 * descendant -- an empty snapshot included. Nothing after that snapshot can link
 * back into this one, because every progenitor link runs from a descendant to
 * its own progenitors and every sibling shares the descendant, so the generation
 * is dead once that snapshot has been processed.
 *
 * The rule is forward-only and exact, computed from the slab already loaded: a
 * version 3 slab names each descendant's snapshot in its DescendantSnapshot
 * column, and a version 2 slab (no column) implicitly names N+1 for every halo
 * with a descendant.
 */
int64_t horizontal_generation_horizon(const struct SnapshotSlab *slab) {
  const struct HaloInputView view = {slab->halos, slab->nhalos};
  int64_t horizon = slab->snapnum;

  for (int64_t i = 0; i < slab->nhalos; i++) {
    if (mimic_tree_get_Descendant(view, i) < 0) {
      continue;
    }
    const int64_t target =
        (slab->descendant_snapshot != NULL) ? slab->descendant_snapshot[i] : slab->snapnum + 1;
    if (target > horizon) {
      horizon = target;
    }
  }

  return horizon;
}

/* Generations currently retained, counted from the pool's own slabs rather than
 * from the bookkeeping counter: a retention bug (a skipped, early or doubled
 * release) then shows up in the lifecycle log instead of being masked by a
 * counter that is updated in the same place as the bug. */
static int64_t horizontal_count_live_slabs(const struct HorizontalDriverState *state) {
  int64_t live = 0;
  for (int64_t k = 0; k < state->snapshot_count; k++) {
    live += !snapshot_slab_is_empty(&state->generations[k].slab);
  }
  return live;
}

/* ------------------------------------------------------------------------- */
/* Retention memory accounting                                                */
/* ------------------------------------------------------------------------- */

/* Bytes per gigabyte, decimal, as the run memory profile reports them. */
#define HORIZONTAL_BYTES_PER_GB 1.0e9

/*
 * What one generation holds resident, in bytes, computed before any of it is
 * allocated: the slab term from the row width the reader published, the aux,
 * output and pool terms from struct widths.
 *
 * Every term is int64_t and formed by checked arithmetic, so a slab of any row
 * count -- including one above INT32_MAX -- is sized exactly or refused, never
 * narrowed or wrapped. The galaxy pool is a term only when the generation must
 * create one: its first chunk and headers are then fixed before the sweep
 * (galaxy_pool_initial_resident_bytes()). A reused spare pool is already
 * resident and already counted by horizontal_retained_resident_bytes(). What
 * no footprint can include is how far the sweep then grows the output buffer
 * and the galaxy pool.
 */
struct HorizontalGenerationFootprint {
  int64_t slab_bytes;      /* struct RawHalo rows plus every reader-owned array */
  int64_t aux_bytes;       /* struct HorizontalHaloAux rows */
  int64_t output_capacity; /* output buffer seed, in struct Halo records */
  int64_t output_bytes;    /* that seed, in bytes */
  int64_t pool_bytes;      /* a new galaxy pool, or 0 when a spare is reused */
  int64_t total_bytes;
};

/* `count` rows of `width` bytes, or 0 when the product does not fit in int64_t. */
static int horizontal_checked_bytes(int64_t count, size_t width, int64_t *bytes) {
  if (count < 0 || (width != 0 && (uint64_t)count > (uint64_t)INT64_MAX / width)) {
    return 0;
  }
  *bytes = count * (int64_t)width;
  return 1;
}

/* a + b for non-negative a and b, or 0 when the sum does not fit in int64_t. */
static int horizontal_checked_sum(int64_t a, int64_t b, int64_t *sum) {
  if (a < 0 || b < 0 || b > INT64_MAX - a) {
    return 0;
  }
  *sum = a + b;
  return 1;
}

/* The output buffer's seed capacity for a slab of `nhalos`: the slab plus
 * proportional headroom (see horizontal_acquire_generation()), clamped at
 * MAX_HALO_ARRAY_SIZE unless the slab is already past it. 0 on int64_t overflow,
 * which only a slab within MIN_HALO_ARRAY_GROWTH of INT64_MAX rows can reach. */
static int horizontal_output_seed_capacity(int64_t nhalos, int64_t *capacity) {
  if (nhalos > MAX_HALO_ARRAY_SIZE) {
    return horizontal_checked_sum(nhalos, MIN_HALO_ARRAY_GROWTH, capacity);
  }

  int64_t seed_headroom = (int64_t)((double)nhalos * HORIZONTAL_OUTPUT_SEED_HEADROOM);
  if (seed_headroom < MIN_HALO_ARRAY_GROWTH) {
    seed_headroom = MIN_HALO_ARRAY_GROWTH;
  }
  int64_t seed_capacity = nhalos + seed_headroom;
  if (seed_capacity > MAX_HALO_ARRAY_SIZE) {
    seed_capacity = MAX_HALO_ARRAY_SIZE;
  }
  *capacity = seed_capacity;
  return 1;
}

/* Size a generation of `nhalos` rows before anything is allocated for it. The
 * aux array always holds at least one row, as horizontal_acquire_generation()
 * allocates it; `new_pool` says whether horizontal_take_pool() will have to
 * create a galaxy pool (created with the same hint, 0) rather than reuse a
 * spare. Returns 0 when any term overflows int64_t. */
static int horizontal_generation_footprint(int64_t nhalos, int64_t slab_row_bytes, int new_pool,
                                           struct HorizontalGenerationFootprint *fp) {
  memset(fp, 0, sizeof(*fp));

  int64_t slab_and_aux = 0;
  int64_t without_pool = 0;
  return horizontal_checked_bytes(nhalos, (size_t)slab_row_bytes, &fp->slab_bytes) &&
         horizontal_checked_bytes(nhalos > 0 ? nhalos : 1, sizeof(struct HorizontalHaloAux),
                                  &fp->aux_bytes) &&
         horizontal_output_seed_capacity(nhalos, &fp->output_capacity) &&
         horizontal_checked_bytes(fp->output_capacity, sizeof(struct Halo), &fp->output_bytes) &&
         (!new_pool || galaxy_pool_initial_resident_bytes(0, &fp->pool_bytes)) &&
         horizontal_checked_sum(fp->slab_bytes, fp->aux_bytes, &slab_and_aux) &&
         horizontal_checked_sum(slab_and_aux, fp->output_bytes, &without_pool) &&
         horizontal_checked_sum(without_pool, fp->pool_bytes, &fp->total_bytes);
}

/* A galaxy pool's resident bytes: its header, its chunks' headers and slots. */
static int64_t horizontal_pool_resident_bytes(const struct GalaxyPool *pool) {
  struct GalaxyPoolStats stats;
  galaxy_pool_stats(pool, &stats);
  return stats.resident_bytes;
}

/*
 * Bytes resident across the retention pool right now: every retained
 * generation's slab and reader-owned arrays, aux array, output buffer at its
 * current (possibly marshaller-grown) capacity and galaxy pool, plus every reset
 * spare pool, whose chunks stay allocated until teardown. Each term is memory
 * this process already holds, so the plain sums below cannot overflow int64_t.
 */
static int64_t horizontal_retained_resident_bytes(const struct HorizontalDriverState *state) {
  int64_t resident = 0;

  for (int64_t k = 0; k < state->snapshot_count; k++) {
    const struct HorizontalGeneration *gen = &state->generations[k];
    if (gen->snapnum == SNAPSHOT_SLAB_NO_SNAPSHOT) {
      continue;
    }
    const int64_t nhalos = gen->slab.nhalos;
    resident += nhalos * state->slab_row_bytes;
    resident += (nhalos > 0 ? nhalos : 1) * (int64_t)sizeof(struct HorizontalHaloAux);
    resident += gen->processed.capacity * (int64_t)sizeof(struct Halo);
    if (gen->pool != NULL) {
      resident += horizontal_pool_resident_bytes(gen->pool);
    }
  }
  for (int64_t k = 0; k < state->spare_count; k++) {
    resident += horizontal_pool_resident_bytes(state->spare_pools[k]);
  }

  return resident;
}

/*
 * Apply the width policy for a slab of `nhalos` rows at snapshot `snapnum`,
 * before anything is allocated for it.
 *
 * The reader and driver index a slab with int64_t, but the output path does not:
 * it caps a snapshot's record count at INT_MAX (output_increment_halo_counters_checked in
 * src/io/output/util.c), the marshaller cannot grow an output buffer past MAX_HALO_ARRAY_SIZE,
 * and a FoF group's galaxy count is narrowed to int. Refusal is reserved for where failure
 * is certain: a requested output snapshot above INT_MAX. Above the marshaller
 * cap the failure is only likely (the sweep may emit fewer records than it has
 * rows), so it is a warning. Both need chunked slab streaming to lift.
 */
static void horizontal_require_slab_emittable(int64_t snapnum, int64_t nhalos) {
  if (nhalos > INT_MAX && horizontal_output_snapshot_index(snapnum) >= 0) {
    FATAL_ERROR("Snapshot %" PRId64 " holds %" PRId64 " halos and is a requested output snapshot, "
                "but the output path caps a snapshot's record count at INT_MAX "
                "(output_increment_halo_counters_checked). Refused before allocation: "
                "emitting a slab this wide needs chunked slab streaming, a capability Mimic "
                "does not implement",
                snapnum, nhalos);
  }

  if (nhalos > MAX_HALO_ARRAY_SIZE) {
    WARNING_LOG("Snapshot %" PRId64 " holds %" PRId64 " halos, above MAX_HALO_ARRAY_SIZE (%d). "
                "The output marshaller cannot grow a buffer past that bound and a FoF workspace "
                "is counted in int, so the sweep is likely to abort after the slab is loaded. "
                "Running a slab this wide needs chunked slab streaming, a capability Mimic does "
                "not implement",
                snapnum, nhalos, (int)MAX_HALO_ARRAY_SIZE);
  }
}

/*
 * Size snapshot `snapnum`'s generation from its halo count, the reader's slab
 * row width and struct widths, report it, and refuse it -- before the reader
 * allocates its slab or this driver allocates anything for it -- when it cannot
 * be held: when its size overflows int64_t, when it is a requested output
 * snapshot too wide for the output path's int counts (see
 * horizontal_require_slab_emittable(), which also warns above the output
 * marshaller's cap), or when input.retention_memory_ceiling_mb is set and the
 * retention pool with this generation added would exceed it. A retention set
 * exactly at the ceiling is accepted.
 *
 * The figure checked is what the pool holds resident now plus this generation's
 * footprint, which includes a new galaxy pool whenever no spare one will be
 * reused. The only allocation for a generation not refused here is in-sweep
 * growth of its output buffer and galaxy pool, which happens after this check;
 * it is measured, and reported in the run memory profile.
 */
static void horizontal_require_generation_fits(const struct HorizontalDriverState *state,
                                               int64_t snapnum, int64_t nhalos,
                                               struct HorizontalGenerationFootprint *fp) {
  if (nhalos < 0) {
    FATAL_ERROR("Reader '%s' reports %" PRId64 " halos for snapshot %" PRId64
                "; a halo count cannot be negative",
                state->reader->name, nhalos, snapnum);
  }
  horizontal_require_slab_emittable(snapnum, nhalos);

  const int64_t resident = horizontal_retained_resident_bytes(state);
  const int new_pool = (state->spare_count == 0);
  int64_t required = 0;
  if (!horizontal_generation_footprint(nhalos, state->slab_row_bytes, new_pool, fp) ||
      !horizontal_checked_sum(resident, fp->total_bytes, &required)) {
    FATAL_ERROR("Snapshot %" PRId64 " holds %" PRId64 " halos, too many for its generation's "
                "resident size to be counted in 64-bit bytes. Refused before allocation: "
                "whole-slab retention cannot hold a slab this wide, which needs chunked slab "
                "streaming, a capability Mimic does not implement",
                snapnum, nhalos);
  }

  const int64_t ceiling = MimicConfig.RetentionMemoryCeiling;
  VERBOSE_LOG("Snapshot %" PRId64 " generation needs %" PRId64 " B for %" PRId64
              " halos (slab and reader-owned arrays %" PRId64 " B, aux %" PRId64
              " B, output buffer seed %" PRId64 " records = %" PRId64 " B, galaxy pool %" PRId64
              " B%s); retention pool holds %" PRId64 " B, %" PRId64 " B with it; ceiling %" PRId64
              " B%s",
              snapnum, fp->total_bytes, nhalos, fp->slab_bytes, fp->aux_bytes, fp->output_capacity,
              fp->output_bytes, fp->pool_bytes, new_pool ? " new" : ", a resident spare reused",
              resident, required, ceiling, ceiling > 0 ? "" : " (none set)");

  if (ceiling > 0 && required > ceiling) {
    FATAL_ERROR("Snapshot %" PRId64 " needs %" PRId64 " B (%.3f GB) resident for its %" PRId64
                " halos, which would bring the retention pool to %" PRId64 " B (%.3f GB), above "
                "the input.retention_memory_ceiling_mb ceiling of %" PRId64 " B (%.3f GB). "
                "Refused before allocation: whole-slab retention cannot hold this snapshot "
                "within the ceiling, and holding it in less memory needs chunked slab "
                "streaming, a capability Mimic does not implement",
                snapnum, fp->total_bytes, (double)fp->total_bytes / HORIZONTAL_BYTES_PER_GB, nhalos,
                required, (double)required / HORIZONTAL_BYTES_PER_GB, ceiling,
                (double)ceiling / HORIZONTAL_BYTES_PER_GB);
  }
}

/* Allocate the retention pool for a run of `snapshot_count` snapshots, every
 * slot empty. The spare-pool stack is sized for the worst case up front -- a
 * run can never hold more galaxy pools than snapshots -- so releasing a
 * generation never allocates, which is what lets the failure path release
 * without risking a second abort. */
static void horizontal_allocate_retention(struct HorizontalDriverState *state,
                                          int64_t snapshot_count) {
  const size_t slots = (size_t)(snapshot_count > 0 ? snapshot_count : 1);

  state->generations = mymalloc_cat(slots * sizeof(struct HorizontalGeneration), MEM_HALOS);
  state->lookup = mymalloc_cat(slots * sizeof(struct HorizontalRetainedGeneration), MEM_HALOS);
  state->spare_pools = mymalloc_cat(slots * sizeof(struct GalaxyPool *), MEM_HALOS);
  state->spare_capacity = (int64_t)slots;
  state->spare_count = 0;

  for (size_t k = 0; k < slots; k++) {
    memset(&state->generations[k], 0, sizeof(state->generations[k]));
    state->generations[k].snapnum = SNAPSHOT_SLAB_NO_SNAPSHOT;
    state->generations[k].horizon = SNAPSHOT_SLAB_NO_SNAPSHOT;
    state->generations[k].slab = snapshot_slab_empty();

    memset(&state->lookup[k], 0, sizeof(state->lookup[k]));
    state->lookup[k].snapnum = SNAPSHOT_SLAB_NO_SNAPSHOT;
  }

  /* Published last, so a failure above leaves no slot count for teardown to walk
   * over a half-initialised array. */
  state->snapshot_count = snapshot_count;
}

/* A reset galaxy pool for a new generation: a spare one when a released
 * generation left one behind, else a new one. */
static struct GalaxyPool *horizontal_take_pool(struct HorizontalDriverState *state) {
  if (state->spare_count > 0) {
    return state->spare_pools[--state->spare_count];
  }
  return galaxy_pool_create(0);
}

/* Reset a released generation's pool and keep it for the next generation. */
static void horizontal_return_pool(struct HorizontalDriverState *state, struct GalaxyPool *pool) {
  galaxy_pool_reset(pool);
  /* Cannot overflow: every pool was taken for a retained generation, and at most
   * snapshot_count of those exist at once. */
  state->spare_pools[state->spare_count++] = pool;
}

/*
 * Load snapshot `snapnum` into its retention slot, compute its horizon, and
 * allocate its per-halo aux array, output buffer and galaxy pool.
 *
 * The slot counts as retained from the moment the reader hands the slab over,
 * so a failure anywhere after that point is released by the failure path, and
 * every buffer is recorded on the generation as soon as it exists.
 *
 * The output buffer is seeded with proportional headroom rather than a flat
 * increment: the output-to-slab ratio sits just under 1.0 and rises with scale,
 * so a flat increment leaves a large run a fraction of a percent from a growth
 * that reallocs the whole buffer. The vertical driver's MAXHALOFAC over-allocation
 * is not copied -- at slab scale a five-fold reservation is hundreds of
 * megabytes -- and the seed adds no proportional headroom to a
 * slab already past MAX_HALO_ARRAY_SIZE, only the minimum growth increment.
 */
static struct HorizontalGeneration *
horizontal_acquire_generation(struct HorizontalDriverState *state, int64_t snapnum,
                              int32_t links_adjacent) {
  struct HorizontalGeneration *gen = &state->generations[snapnum];

  if (gen->snapnum != SNAPSHOT_SLAB_NO_SNAPSHOT) {
    FATAL_ERROR("Snapshot %" PRId64 " is already retained; a generation is loaded exactly once",
                snapnum);
  }

  /* Sized, reported and (against a configured ceiling) refused from the halo
   * count alone, before the reader allocates the slab. The only refusals by width
   * are those horizontal_require_generation_fits() makes: an output snapshot
   * above INT_MAX rows, and the ceiling. Every index this driver computes from a
   * slab (struct Halo.HaloNr, the generated accessors and link values, the aux
   * ranges, the FoF and progenitor walks, and the workspace and scratch sizes) is
   * int64_t, and so is every byte count above. The int32 bound of a version 2
   * slab is the reader's, enforced at open (HORIZONTAL-HDF5-FORMAT.md invariant 2). */
  const int64_t nhalos = horizontal_reader_halo_count(state->reader, snapnum);
  struct HorizontalGenerationFootprint footprint;
  horizontal_require_generation_fits(state, snapnum, nhalos, &footprint);

  horizontal_reader_load_slab(state->reader, snapnum, &gen->slab);

  /* Counted as retained together with the slot's snapnum, and by the slab's own
   * row count, so any failure from here on -- the count check below included --
   * is released by the failure path with the bookkeeping balanced: release
   * decrements both counters by exactly what was added here. */
  gen->snapnum = snapnum;
  state->retained_count++;
  state->retained_population += gen->slab.nhalos;
  if (state->retained_count > state->max_retained_count) {
    state->max_retained_count = state->retained_count;
  }

  if (gen->slab.nhalos != nhalos) {
    FATAL_ERROR("Reader '%s' loaded %" PRId64 " halos for snapshot %" PRId64
                " after reporting %" PRId64 "; the generation was sized for the reported count",
                state->reader->name, gen->slab.nhalos, snapnum, nhalos);
  }

  /* A gapped dataset can only be walked through its target-snapshot columns;
   * without them every link would silently fall back to the adjacent version 2
   * scope. */
  if (!links_adjacent && nhalos > 0 &&
      (gen->slab.descendant_snapshot == NULL || gen->slab.first_progenitor_snapshot == NULL ||
       gen->slab.next_progenitor_snapshot == NULL)) {
    FATAL_ERROR("Snapshot %" PRId64 " belongs to a dataset with links_adjacent = 0 but its slab "
                "carries no target-snapshot columns",
                snapnum);
  }

  gen->horizon = horizontal_generation_horizon(&gen->slab);
  if (gen->horizon >= state->snapshot_count) {
    FATAL_ERROR("Snapshot %" PRId64 " names descendant snapshot %" PRId64
                ", beyond the run's last snapshot %" PRId64,
                snapnum, gen->horizon, state->snapshot_count - 1);
  }

  gen->pool = horizontal_take_pool(state);

  gen->aux =
      mymalloc_cat(sizeof(struct HorizontalHaloAux) * (size_t)(nhalos > 0 ? nhalos : 1), MEM_HALOS);
  for (int64_t i = 0; i < nhalos; i++) {
    gen->aux[i].FirstHalo = -1;
    gen->aux[i].NHalos = 0;
  }

  /* Output count is the slab plus the orphans carried forward from earlier
   * snapshots; seed for that and let the marshaller grow if it is exceeded.
   * Seeding policy and its rationale are on the function comment above; the seed
   * is the one the footprint was sized with (horizontal_output_seed_capacity()). */
  gen->processed.count = 0;
  gen->processed.capacity = footprint.output_capacity;
  gen->processed.halos =
      mymalloc_cat((size_t)gen->processed.capacity * sizeof(struct Halo), MEM_HALOS);
  memset(gen->processed.halos, 0, (size_t)gen->processed.capacity * sizeof(struct Halo));
  /* The memset makes the full seed capacity resident immediately, so it counts
   * towards the run memory profile whether or not the marshaller grows it. */
  run_profile_note_output_buffer(gen->processed.count, gen->processed.capacity,
                                 sizeof(struct Halo));

  return gen;
}

/* Make a swept generation visible to later snapshots' progenitor lookup. Done
 * only after the sweep, because marshalling may move its output buffer while the
 * sweep is running and nothing may link into a snapshot still being built. */
static void horizontal_publish_generation(struct HorizontalDriverState *state,
                                          const struct HorizontalGeneration *gen) {
  struct HorizontalRetainedGeneration *entry = &state->lookup[gen->snapnum];

  entry->snapnum = gen->snapnum;
  entry->view.halos = gen->slab.halos;
  entry->view.count = gen->slab.nhalos;
  entry->aux = gen->aux;
  entry->processed = gen->processed.halos;
  entry->next_progenitor_snapshot = gen->slab.next_progenitor_snapshot;
}

/*
 * Release a generation's raw slab (with every reader-owned array on it), aux
 * array, output buffer and galaxy slots. The pool is reset and kept as a spare
 * for a later generation rather than destroyed.
 *
 * The generation is detached from the pool before anything is freed, so a
 * failure part-way through this release is never retried by the failure path,
 * which would otherwise free the same buffers twice.
 */
static void horizontal_release_generation(struct HorizontalDriverState *state,
                                          struct HorizontalGeneration *gen) {
  const int64_t released = gen->snapnum;
  const int64_t horizon = gen->horizon;

  gen->snapnum = SNAPSHOT_SLAB_NO_SNAPSHOT;
  gen->horizon = SNAPSHOT_SLAB_NO_SNAPSHOT;
  state->lookup[released].snapnum = SNAPSHOT_SLAB_NO_SNAPSHOT;
  state->retained_count--;
  state->retained_population -= gen->slab.nhalos;

  horizontal_reader_release_slab(state->reader, &gen->slab);

  /* Belt and braces: horizontal_write_output() already returns the globals to
   * their unowned state, so this only ever matters if a future edit stops doing
   * that. Clearing before the free keeps "the globals point at a live buffer or
   * at nothing" true at every point in the retention schedule. */
  horizontal_clear_output_globals();

  myfree(gen->processed.halos);
  gen->processed.halos = NULL;
  gen->processed.count = 0;
  gen->processed.capacity = 0;

  myfree(gen->aux);
  gen->aux = NULL;

  if (gen->pool != NULL) {
    horizontal_return_pool(state, gen->pool);
    gen->pool = NULL;
  }

  VERBOSE_LOG("Released snapshot %" PRId64 " (raw slab and processed generation; horizon %" PRId64
              ")",
              released, horizon);
}

/* Release every earlier generation whose horizon is `snapnum`, now that
 * snapshot `snapnum`'s sweep has deep-copied everything it inherits. Ascending
 * snapshot order, so the lifecycle log is deterministic. */
static void horizontal_release_expired_generations(struct HorizontalDriverState *state,
                                                   int64_t snapnum) {
  for (int64_t k = 0; k < snapnum; k++) {
    struct HorizontalGeneration *gen = &state->generations[k];
    if (gen->snapnum != SNAPSHOT_SLAB_NO_SNAPSHOT && gen->horizon <= snapnum) {
      horizontal_release_generation(state, gen);
    }
  }
}

/*
 * Release everything the driver owns: every generation still retained, the
 * reader's run, every galaxy pool, the retention arrays and the scratch buffers.
 *
 * The success path reaches it with no generation retained, and harvests each
 * pool's cost into the run profile first. The failure path reaches it from
 * horizontal_failure_cleanup() with whatever was live at the abort, in any
 * partially built state: every pointer below is NULL until its buffer exists.
 * The reader's run is closed only after every slab is released, so its own
 * "no slab still loaded" check holds on both paths.
 */
static void horizontal_teardown(struct HorizontalDriverState *state, int record_profile) {
  for (int64_t k = 0; k < state->snapshot_count; k++) {
    if (state->generations[k].snapnum != SNAPSHOT_SLAB_NO_SNAPSHOT) {
      horizontal_release_generation(state, &state->generations[k]);
    }
  }

  if (state->run_open) {
    state->run_open = 0;
    horizontal_reader_close_run(state->reader);
    VERBOSE_LOG("Closed horizontal run '%s' with no slab loaded", state->reader->name);
  }

  while (state->spare_count > 0) {
    struct GalaxyPool *pool = state->spare_pools[--state->spare_count];
    if (record_profile) {
      /* The profile keeps maxima rather than sums, so harvesting every pool
       * reports a conservative bound on any one generation -- which is the term
       * the memory projection multiplies by the number of live generations. */
      struct GalaxyPoolStats pool_stats;
      galaxy_pool_stats(pool, &pool_stats);
      run_profile_note_galaxy_pool(pool_stats.galaxies_high_water, pool_stats.slots_allocated,
                                   pool_stats.chunk_count, sizeof(struct GalaxyData));
    }
    galaxy_pool_destroy(pool);
  }

  myfree(state->spare_pools);
  state->spare_pools = NULL;
  state->spare_capacity = 0;
  myfree(state->lookup);
  state->lookup = NULL;
  myfree(state->generations);
  state->generations = NULL;
  state->snapshot_count = 0;

  myfree(state->segments);
  state->segments = NULL;
  myfree(state->progenitor_scratch);
  state->progenitor_scratch = NULL;
  myfree(state->workspace);
  state->workspace = NULL;

  /* Already cleared after each output call and at each release; repeated here
   * so the driver cannot return with them set under any path. */
  horizontal_clear_output_globals();
}

/*
 * Failure-path release of retained generations.
 *
 * FATAL_ERROR leaves through exit(), so no code after the failing call runs and
 * main.c's bye() does not know this driver's buffers. This exit handler does:
 * while run_horizontal_driver() is active it points at the driver's state (which
 * stays alive during exit(), because exit() does not unwind the caller's frame)
 * and releases every retained generation and closes the reader's run. It is
 * registered after bye(), so it runs before it.
 *
 * The state is detached before anything is released, so an abort raised while
 * releasing cannot re-enter this handler and free the same buffers twice.
 */
static struct HorizontalDriverState *horizontal_failure_state = NULL;

static void horizontal_failure_cleanup(void) {
  struct HorizontalDriverState *state = horizontal_failure_state;

  if (state == NULL) {
    return;
  }
  horizontal_failure_state = NULL;

  VERBOSE_LOG("Horizontal driver exiting early: releasing %" PRId64 " retained generation%s",
              state->retained_count, state->retained_count == 1 ? "" : "s");
  horizontal_teardown(state, 0);
}

static void horizontal_arm_failure_cleanup(struct HorizontalDriverState *state) {
  static int registered = 0;

  if (!registered) {
    if (atexit(horizontal_failure_cleanup) != 0) {
      FATAL_ERROR("Could not register the horizontal driver's failure cleanup");
    }
    registered = 1;
  }
  horizontal_failure_state = state;
}

static void horizontal_disarm_failure_cleanup(void) { horizontal_failure_state = NULL; }

/* ------------------------------------------------------------------------- */
/* Driver                                                                     */
/* ------------------------------------------------------------------------- */

/*
 * Prove the output directory is writable before the run starts.
 *
 * main.c:393 proves the directory can be *created*, which is not the same
 * property: a pre-existing directory the run cannot write to passes that check.
 * The driver used to catch it immediately, because it created its output file up
 * front; now the first partition file appears only when its snapshot completes,
 * which for a z=0-only request is the end of a multi-week run. So the property
 * is restored explicitly, and deliberately ahead of the dataset open, which
 * validates every input file first and is not instant at production scale.
 *
 * Every status is checked, including both unlinks: a probe that cannot be
 * removed means something is wrong with the directory even though the write
 * succeeded, and in the failure branch the removal is reported alongside — never
 * instead of — whatever actually failed.
 *
 * The probe is created by mkstemp() rather than by name. That is what makes it
 * collision-safe in the sense that matters: mkstemp() creates with
 * O_CREAT | O_EXCL and mode 0600 and keeps trying fresh names, so it can neither
 * follow a symlink planted at the probe path (which would truncate the link's
 * target) nor adopt and truncate a stale probe left by an earlier run. A
 * pid-derived name is unique only among live processes and is neither of those
 * things.
 */
static void horizontal_probe_output_directory(void) {
  /* mkstemp() rewrites the six X's in place, so after a successful call this
   * holds the real path of the file that was created. */
  char probe_path[HORIZONTAL_PATH_BUF_SIZE + 1];

  const int written = snprintf(probe_path, sizeof(probe_path), "%s/.mimic_write_probe_XXXXXX",
                               MimicConfig.OutputDir);
  if (written < 0 || written >= (int)sizeof(probe_path)) {
    FATAL_ERROR("Output-directory write-probe path too long under '%s'", MimicConfig.OutputDir);
  }

  const int probe_fd = mkstemp(probe_path);
  if (probe_fd < 0) {
    /* The template is left unspecified by a failed mkstemp(), so the directory
     * is what gets named here rather than a path that may be half-substituted. */
    FATAL_ERROR("Output directory '%s' is not writable: could not create a probe file there: %s",
                MimicConfig.OutputDir, strerror(errno));
  }

  /* Both statuses are taken before either is judged, so the descriptor is closed
   * exactly once whichever of them failed: a deferred write error can surface at
   * the close, and bailing out at the write would leak the descriptor. */
  const char probe_byte = '\0';
  const int write_failed = (write(probe_fd, &probe_byte, 1) != 1);
  const int write_errno = errno;
  const int close_failed = (close(probe_fd) != 0);
  const int close_errno = errno;

  if (write_failed || close_failed) {
    const int unlink_failed = (unlink(probe_path) != 0);
    FATAL_ERROR("Output directory '%s' is not writable: could not %s the probe file '%s': %s "
                "(check free space)%s",
                MimicConfig.OutputDir, write_failed ? "write to" : "close", probe_path,
                strerror(write_failed ? write_errno : close_errno),
                unlink_failed ? "; the probe file could not be removed either" : "");
  }

  if (unlink(probe_path) != 0) {
    FATAL_ERROR(
        "Output directory '%s' is writable but its probe file '%s' could not be removed: %s",
        MimicConfig.OutputDir, probe_path, strerror(errno));
  }

  VERBOSE_LOG("Output directory '%s' is writable", MimicConfig.OutputDir);
}

/**
 * @brief   Run a horizontal configuration end to end.
 *
 * Proves the output directory writable, opens the configured dataset, then walks
 * every snapshot in increasing time order. For snapshot N: load slab N into the
 * retention pool, process every FoF group against the retained generations its
 * progenitor links name, release every earlier generation whose horizon is N,
 * then — if snapshot N was requested for output — write it to its own partition
 * file and close that file, and finally release generation N itself if nothing
 * after N links back into it. After the final snapshot no generation remains,
 * and the dataset is closed; main.c writes the master file afterwards and only
 * then disarms this driver's remaining output cleanup.
 *
 * An adjacent dataset (links_adjacent = 1, every version 2 dataset) never
 * retains more than two generations, the bound of the two-generation rotation
 * this pool generalises; a gapped one retains as many as its horizons demand.
 */
void run_horizontal_driver(void) {
  struct HorizontalDriverState state;
  struct HorizontalRunInfo info;
  ProgressBar bar;

  memset(&state, 0, sizeof(state));
  state.reader = MimicConfig.horizontal_reader;
  horizontal_arm_failure_cleanup(&state);

  horizontal_probe_output_directory();

  horizontal_reader_open_run(state.reader, &info);
  state.run_open = 1;
  if (info.slab_row_bytes < (int64_t)sizeof(struct RawHalo)) {
    FATAL_ERROR("Reader '%s' published a slab row width of %" PRId64
                " B, below the %zu B of struct RawHalo alone; every horizontal reader must "
                "publish the bytes load_slab allocates per halo",
                state.reader->name, info.slab_row_bytes, sizeof(struct RawHalo));
  }
  INFO_LOG("Opened horizontal run '%s': %" PRId64 " snapshot%s, format_version %" PRId32
           ", links_adjacent %" PRId32 ", %" PRId64 " forest%s, max halo rank in forest %" PRId64,
           state.reader->name, info.snapshot_count, info.snapshot_count == 1 ? "" : "s",
           info.format_version, info.links_adjacent, info.n_forests_total,
           info.n_forests_total == 1 ? "" : "s", info.max_halo_rank_in_forest);

  log_phase_banner(PHASE_TREE_PROCESSING);
  enable_debug_log_rate_limiting();

  /* The vertical driver's per-partition globals have no meaning here; the writers
   * that still read them are guarded on the processing order. FileNum
   * is set per partition instead, by horizontal_write_output(). */
  TreeID = 0;
  GlobalForestOffset = 0;

  /* One partition per requested output snapshot, resolved through the shared
   * seam so this driver never derives a partition's snapshot range itself. */
  const struct OutputPartitionSource output_source = get_output_partition_source();
  const int npartitions = output_source.num_partitions();

  horizontal_open_output();

  state.slab_row_bytes = info.slab_row_bytes;
  horizontal_allocate_retention(&state, info.snapshot_count);

  state.workspace_capacity = INITIAL_FOF_HALOS;
  state.workspace = mymalloc_cat((size_t)state.workspace_capacity * sizeof(struct Halo), MEM_HALOS);
  memset(state.workspace, 0, (size_t)state.workspace_capacity * sizeof(struct Halo));

  INFO_LOG("Processing %" PRId64 " snapshot%s → %d output file%s", info.snapshot_count,
           info.snapshot_count == 1 ? "" : "s", npartitions, npartitions == 1 ? "" : "s");
  progress_bar_init(&bar, info.snapshot_count, "");

  for (int64_t snapnum = 0; snapnum < info.snapshot_count; snapnum++) {
    progress_bar_update(&bar, snapnum);

    struct HorizontalGeneration *cur =
        horizontal_acquire_generation(&state, snapnum, info.links_adjacent);

    const int64_t live_slabs = horizontal_count_live_slabs(&state);
    VERBOSE_LOG("Loaded snapshot %" PRId64 " (%" PRId64 " halos); %" PRId64 " slab%s live", snapnum,
                cur->slab.nhalos, live_slabs, live_slabs == 1 ? "" : "s");
    VERBOSE_LOG("Snapshot %" PRId64 " retention horizon is snapshot %" PRId64, snapnum,
                cur->horizon);

    struct HorizontalGatherContext lookup;
    lookup.snapnum = snapnum;
    lookup.first_progenitor_snapshot = cur->slab.first_progenitor_snapshot;
    lookup.generations = state.lookup;
    lookup.retained_population = state.retained_population;

    /* Walk FoF groups in slab order, processing each group when its central is
     * first met. Every halo names a central whose own FirstHaloInFOFgroup is
     * itself (HORIZONTAL-HDF5-FORMAT.md invariant 6), so this visits every group
     * exactly once; the member tally below proves it visited every halo. An
     * empty snapshot has no group and is processed as empty. */
    const struct HaloInputView view = {cur->slab.halos, cur->slab.nhalos};
    const int64_t nhalos = cur->slab.nhalos;
    int64_t members_processed = 0;

    for (int64_t halonr = 0; halonr < nhalos; halonr++) {
      if (mimic_tree_get_FirstHaloInFOFgroup(view, halonr) == halonr) {
        members_processed += horizontal_process_fof_group(&state, cur, &lookup, halonr);
      }
    }

    if (members_processed != cur->slab.nhalos) {
      FATAL_ERROR("Snapshot %" PRId64 " FoF chains cover %" PRId64 " of %" PRId64
                  " halos; the slab's FoF links are inconsistent",
                  snapnum, members_processed, cur->slab.nhalos);
    }

    /* The retention pool is at its largest for this snapshot now: the sweep has
     * grown the output buffers and pools, and nothing has been released yet. */
    const int64_t resident = horizontal_retained_resident_bytes(&state);
    if (resident > state.max_retained_bytes) {
      state.max_retained_bytes = resident;
    }
    run_profile_note_retention(state.retained_count, resident);

    /* The ceiling is enforced before each generation is allocated, from what is
     * resident then plus the new generation's footprint (slab, aux, seeded output
     * buffer and, when none is reused, a new galaxy pool). What remains is
     * in-sweep growth of the output buffer and galaxy pool only -- the marshaller
     * enlarging the buffer past its seed, the pool adding a chunk -- which the
     * output-buffer and galaxy-pool services allocate mid-sweep, so it cannot be
     * refused before allocation and is reported instead. Once per run: later
     * overshoots are covered by the profile's peak. */
    const int64_t ceiling = MimicConfig.RetentionMemoryCeiling;
    if (ceiling > 0 && resident > ceiling && !state.warned_sweep_over_ceiling) {
      state.warned_sweep_over_ceiling = 1;
      WARNING_LOG("The retention pool grew to %" PRId64 " B during snapshot %" PRId64
                  "'s sweep, above the input.retention_memory_ceiling_mb ceiling of %" PRId64
                  " B. Only in-sweep growth of the output buffer and galaxy pool can pass the "
                  "ceiling of the retention pool's admitted payload: it is allocated mid-sweep, so "
                  "it is measured here rather than "
                  "refused before allocation",
                  resident, snapnum, ceiling);
    }

    horizontal_publish_generation(&state, cur);

    /* Every FoF group at N has now deep-copied whatever it inherits, so every
     * earlier generation whose horizon is N (raw slab, aux, output buffer and
     * galaxies) is dead. Snapshot N's own output is written afterwards, from the
     * still-live slab N. */
    horizontal_release_expired_generations(&state, snapnum);

    const int output_index = horizontal_output_snapshot_index(snapnum);
    if (output_index >= 0) {
      horizontal_write_output(cur, output_index, output_source.partition_snapshots(output_index));
    }

    /* A generation nothing later links into is dead once its own output is
     * written. */
    if (cur->horizon <= snapnum) {
      horizontal_release_generation(&state, cur);
    }
  }

  progress_bar_finish(&bar);

  /* Every horizon lies inside the run (checked at load), so the sweep released
   * every generation at its horizon; one left over is a retention bug. */
  if (state.retained_count != 0) {
    FATAL_ERROR("%" PRId64 " generation%s still retained after the final snapshot",
                state.retained_count, state.retained_count == 1 ? " is" : "s are");
  }
  VERBOSE_LOG("Retained at most %" PRId64 " generation%s concurrently, holding at most %" PRId64
              " B resident",
              state.max_retained_count, state.max_retained_count == 1 ? "" : "s",
              state.max_retained_bytes);

  horizontal_disarm_failure_cleanup();
  horizontal_teardown(&state, 1);

  disable_debug_log_rate_limiting();
}
