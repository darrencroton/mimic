/**
 * @file    snapshot_driver.c
 * @brief   Snapshot-ordered run driver.
 *
 * Deliberately not named "*hdf5.c": Makefile:272 filters that suffix out of a
 * USE-HDF5=no build, and the snapshot-ordered dispatch case must compile and
 * link in every build (the reader interface symbols it calls are always
 * present; a non-HDF5 build already rejects tree_type: snapshot_hdf5 at
 * configuration, long before this driver runs). Every call into the HDF5
 * writers is therefore confined to the three output helpers below, which have
 * fail-fast stubs when HDF5 is absent.
 *
 * Where the tree driver walks one forest's full history depth-first and holds
 * exactly one input generation live, this driver sweeps snapshots in increasing
 * time order and holds exactly two: snapshot N is processed against the
 * retained snapshot N-1, whose raw slab, output buffer and galaxy pool are
 * released as soon as every FoF group at N has deep-copied what it inherits.
 *
 * The physics, inheritance, marshalling and output seams are shared with the
 * tree driver unchanged, and so are the module-context setup, the halo-evolution
 * dispatch, the FoF subhalo count and the halo init payload — this driver calls
 * process_halo_evolution(), count_fof_subhalos() and make_halo_init_payload()
 * in src/core/halo_evolution.c directly, passing its own workspace.
 *
 * What remains replicated here is only the code that crosses the two-generation
 * boundary: progenitor lookup, count and gather read the previous generation
 * through `prev` and have no tree-driver equivalent, because the tree driver
 * holds one generation and finds progenitors inside it. Those three are still
 * line-for-line equivalents of find_most_massive_progenitor(),
 * count_progenitor_galaxies() and gather_progenitor_galaxies() modulo that
 * substitution; the cross-format identity gate rests on them staying that way,
 * so each carries a reference to its tree-side original. FoF assembly
 * (snapshot_join_progenitor_halos(), snapshot_process_fof_group()) is likewise
 * kept local because it is built on those two-generation lookups.
 */

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "error.h"
#include "galaxy_id.h"
#include "galaxy_pool.h"
#include "globals.h"
#include "inheritance.h"
#include "memory.h"
#include "numeric.h"
#include "output_buffer.h"
#include "progress.h"
#include "proto.h"
#include "run_log.h"
#include "snapshot/reader.h"
#include "types.h"

#include "output/util.h"

#ifdef HDF5
#include <hdf5.h>
#include "output/hdf5.h"
#endif

#include "generated/tree_property_accessors.h"

/* Snapshot-ordered runs write exactly one output partition (see
 * get_output_partition_source() in tree_driver.c, which publishes count 1 and
 * output id 0 for this processing order). */
#define SNAPSHOT_OUTPUT_ID 0

/* The `tree` argument of save_halos_hdf5(), which the shared output counter
 * ignores entirely in snapshot mode: output_increment_halo_counters_checked()
 * never touches the per-tree counters for a snapshot-ordered run, because only
 * the tree reader allocates them. A snapshot run has no trees to number, so
 * this is a placeholder rather than a meaningful index. */
#define SNAPSHOT_OUTPUT_TREE_ID 0

/* Same bound as the tree driver's MAX_PATH_BUF_SIZE in tree_driver.c. */
#define SNAPSHOT_PATH_BUF_SIZE (3 * MAX_STRING_LEN + 25)

/* Output paths this run has created or will create: the partition file and the
 * master file, in that order. Registered together when the partition file is
 * created and unlinked by bye() if the program exits with a failure while they
 * are still registered. Unlike the tree driver's registry (which is cleared as
 * each partition completes), this one stays armed after run_snapshot_driver()
 * returns, because main.c writes the master afterwards; only a successful
 * write_master_file() disarms it. */
static char snapshot_output_paths[2][SNAPSHOT_PATH_BUF_SIZE + 1];
static int snapshot_output_path_count = 0;

void snapshot_driver_clear_output_paths(void) {
  if (snapshot_output_path_count > 0) {
    VERBOSE_LOG("Disarming snapshot output cleanup for %d registered path%s",
                snapshot_output_path_count, snapshot_output_path_count == 1 ? "" : "s");
  }
  for (int i = 0; i < snapshot_output_path_count; i++) {
    snapshot_output_paths[i][0] = '\0';
  }
  snapshot_output_path_count = 0;
}

void snapshot_driver_remove_incomplete_outputs(void) {
  for (int i = 0; i < snapshot_output_path_count; i++) {
    if (snapshot_output_paths[i][0] != '\0') {
      unlink(snapshot_output_paths[i]);
    }
  }
}

#ifdef HDF5
/* Register the partition and master paths as this run's incomplete output.
 *
 * Only the HDF5 build has an output path to arm: without HDF5 the driver's
 * output helpers fail fast (see below) and arming the registry would let bye()
 * unlink a completed earlier run's files on the way out.
 *
 * The master path is formatted the way write_master_file() formats it
 * (master_hdf5.c) rather than through a shared helper: adding one would mean
 * editing an output-writer seam this driver only consumes. */
static void snapshot_register_output_paths(void) {
  snapshot_driver_clear_output_paths();

  output_path_hdf5(snapshot_output_paths[0], SNAPSHOT_PATH_BUF_SIZE, SNAPSHOT_OUTPUT_ID);

  const int written = snprintf(snapshot_output_paths[1], SNAPSHOT_PATH_BUF_SIZE, "%s/%s.hdf5",
                               MimicConfig.OutputDir, MimicConfig.OutputFileBaseName);
  if (written < 0 || written >= SNAPSHOT_PATH_BUF_SIZE) {
    FATAL_ERROR("Master HDF5 output path too long: %s/%s.hdf5", MimicConfig.OutputDir,
                MimicConfig.OutputFileBaseName);
  }

  snapshot_output_path_count = 2;
  VERBOSE_LOG("Snapshot output cleanup armed for '%s' and '%s'", snapshot_output_paths[0],
              snapshot_output_paths[1]);
}
#endif /* HDF5 */

/*
 * One live slab generation: the raw halos of one snapshot, where each of them
 * landed in that snapshot's output buffer, the buffer itself, and the pool that
 * owns its galaxies. Two of these ping-pong by snapshot parity.
 */
struct SnapshotGeneration {
  int64_t snapnum;             /* loaded snapshot, or SNAPSHOT_SLAB_NO_SNAPSHOT */
  struct SnapshotSlab slab;    /* reader-owned raw halos */
  struct SnapshotHaloAux *aux; /* [slab.nhalos] */
  struct OutputBuffer processed;
  struct GalaxyPool *pool;
};

/*
 * Driver-scoped state. The workspace and the two scratch buffers are grown
 * monotonically and kept for the whole run (as the tree driver's equivalents
 * are), then freed before the driver returns.
 */
struct SnapshotDriverState {
  const struct SnapshotReader *reader;
  struct SnapshotGeneration gen[2];

  struct Halo *workspace;
  int workspace_capacity;

  struct InheritanceProgenitorGalaxy *progenitor_scratch;
  int progenitor_capacity;

  struct OutputBufferSegment *segments;
  int segment_capacity;
};

/* ------------------------------------------------------------------------- */
/* Scratch growth                                                             */
/* ------------------------------------------------------------------------- */

/* Mirrors ensure_fof_workspace_capacity() in build_model.c: same growth
 * factor, same minimum increment, same ceiling, same fatal. */
static void snapshot_ensure_workspace_capacity(struct SnapshotDriverState *state, int required) {
  while (required > state->workspace_capacity) {
    const int old_size = state->workspace_capacity;
    int new_size = (int)(state->workspace_capacity * HALO_ARRAY_GROWTH_FACTOR);

    if (new_size - state->workspace_capacity < MIN_HALO_ARRAY_GROWTH)
      new_size = state->workspace_capacity + MIN_HALO_ARRAY_GROWTH;

    if (new_size > MAX_HALO_ARRAY_SIZE)
      new_size = MAX_HALO_ARRAY_SIZE;

    if (new_size <= state->workspace_capacity) {
      FATAL_ERROR("Snapshot FoF workspace requires %d halos but maximum allowed size is %d",
                  required, MAX_HALO_ARRAY_SIZE);
    }

    INFO_LOG("Growing snapshot halo workspace from %d to %d elements", state->workspace_capacity,
             new_size);

    state->workspace_capacity = new_size;
    state->workspace =
        myrealloc_cat(state->workspace, (size_t)new_size * sizeof(struct Halo), MEM_HALOS);
    memset(&state->workspace[old_size], 0, (size_t)(new_size - old_size) * sizeof(struct Halo));
  }
}

static struct InheritanceProgenitorGalaxy *
snapshot_ensure_progenitor_scratch(struct SnapshotDriverState *state, int required) {
  if (required > state->progenitor_capacity) {
    state->progenitor_scratch =
        myrealloc_cat(state->progenitor_scratch,
                      (size_t)required * sizeof(struct InheritanceProgenitorGalaxy), MEM_HALOS);
    state->progenitor_capacity = required;
  }
  return state->progenitor_scratch;
}

static struct OutputBufferSegment *
snapshot_ensure_segment_scratch(struct SnapshotDriverState *state, int required) {
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
 * Snapshot-side find_most_massive_progenitor() in build_model.c.
 *
 * The chain crosses generations exactly once: FirstProgenitor is an index into
 * the previous slab (SNAPSHOT-HDF5-FORMAT.md "Link Scope"), and every
 * NextProgenitor step stays inside that same previous slab. Occupancy and Len
 * are therefore read through `prev`, never through `view`.
 *
 * Selection is the tree-side rule unchanged: an occupied FirstProgenitor pins
 * the answer (lenoccmax = -1 disables further replacement), and otherwise the
 * chain is scanned in order and replaced only on a strict Len increase.
 */
int snapshot_find_most_massive_progenitor(struct HaloInputView view,
                                          const struct SnapshotGatherContext *prev, int halonr) {
  int prog, first_occupied, lenoccmax;
  int64_t steps = 0;

  lenoccmax = 0;
  first_occupied = mimic_tree_get_FirstProgenitor(view, halonr);
  prog = mimic_tree_get_FirstProgenitor(view, halonr);

  if (prog >= 0)
    if (prev->aux[prog].NHalos > 0)
      lenoccmax = -1;

  while (prog >= 0) {
    /* First traversal of this chain in the FoF sweep, so it carries its own
     * cycle guard: the chain stays inside the previous slab and can visit each
     * of that slab's halos at most once. */
    if (++steps > prev->view.count) {
      FATAL_ERROR("Progenitor chain of halo %d visits more than the %" PRId64
                  " halos of the previous snapshot; the input's NextProgenitor links "
                  "contain a cycle",
                  halonr, prev->view.count);
    }
    if (lenoccmax != -1 && mimic_tree_get_Len(prev->view, prog) > lenoccmax &&
        prev->aux[prog].NHalos > 0) {
      lenoccmax = mimic_tree_get_Len(prev->view, prog);
      first_occupied = prog;
    }
    prog = mimic_tree_get_NextProgenitor(prev->view, prog);
  }

  return first_occupied;
}

/* Snapshot-side count_progenitor_galaxies() in build_model.c. */
int64_t snapshot_count_progenitor_galaxies(struct HaloInputView view,
                                           const struct SnapshotGatherContext *prev, int halonr) {
  int64_t count = 0;
  int64_t steps = 0;
  int prog = mimic_tree_get_FirstProgenitor(view, halonr);

  while (prog >= 0) {
    /* The chain stays inside the previous slab, so it can visit each of that
     * slab's halos at most once; more steps than that means the input's
     * progenitor links form a cycle, which would otherwise loop forever. */
    if (++steps > prev->view.count) {
      FATAL_ERROR("Progenitor chain of halo %d visits more than the %" PRId64
                  " halos of the previous snapshot; the input's NextProgenitor links "
                  "contain a cycle",
                  halonr, prev->view.count);
    }
    count += prev->aux[prog].NHalos;
    prog = mimic_tree_get_NextProgenitor(prev->view, prog);
  }

  return count;
}

/*
 * Snapshot-side gather_progenitor_galaxies() in build_model.c.
 *
 * Visit order is load-bearing for cross-format identity: each progenitor chain
 * entry in chain order, then that halo's own output range in order. source_time
 * comes from the stored SnapNum of the source galaxy, exactly as the tree side
 * takes it, so a galaxy that skipped a snapshot carries its own age rather than
 * the previous slab's.
 */
void snapshot_gather_progenitor_galaxies(struct HaloInputView view,
                                         const struct SnapshotGatherContext *prev, int halonr,
                                         int first_occupied,
                                         struct InheritanceProgenitorGalaxy *progenitors) {
  int64_t index = 0;
  int prog = mimic_tree_get_FirstProgenitor(view, halonr);

  while (prog >= 0) {
    for (int64_t i = 0; i < prev->aux[prog].NHalos; i++) {
      const struct Halo *source = &prev->processed[prev->aux[prog].FirstHalo + i];
      progenitors[index].source = source;
      progenitors[index].source_time = Age[source->SnapNum];
      progenitors[index].is_main_branch = (prog == first_occupied);
      index++;
    }

    prog = mimic_tree_get_NextProgenitor(prev->view, prog);
  }
}

/* ------------------------------------------------------------------------- */
/* Identity                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Snapshot-side make_unique_galaxy_id() in build_model.c.
 *
 * The two components are carried by the format in reference tree-driver order
 * (SNAPSHOT-HDF5-FORMAT.md "Galaxy Identity Encoding") and live on the slab, not
 * on struct RawHalo, so they are read from the reader's own arrays. struct
 * Halo.HaloNr keeps the slab index, which is what the output-conversion virial
 * recomputation indexes the slab view with; HaloRankInForest never goes there.
 */
static int64_t snapshot_make_unique_galaxy_id(const struct SnapshotSlab *slab, int halonr) {
  const int64_t multiplier = MimicConfig.UniqueGalaxyIDMultiplier;
  const int64_t rank_in_forest = slab->halo_rank_in_forest[halonr];
  const int64_t forestnr_global = slab->forest_index[halonr];

  if (!mimic_unique_galaxy_id_components_valid(multiplier, rank_in_forest, forestnr_global)) {
    FATAL_ERROR("UniqueGalaxyID components out of range at snapshot %" PRId64 " halo %d: "
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
 * Snapshot-side join_progenitor_halos() in build_model.c.
 *
 * Every descendant field is derived exactly as the tree side derives it; the
 * only substitutions are the previous-generation lookup (which crosses slabs)
 * and the identity encoding (which reads the format's carried components).
 */
static int snapshot_join_progenitor_halos(struct SnapshotDriverState *state,
                                          struct SnapshotGeneration *cur,
                                          const struct SnapshotGatherContext *prev, int halonr,
                                          int ngalstart) {
  const struct HaloInputView view = {cur->slab.halos, cur->slab.nhalos};
  struct InheritanceDescendant descendant;
  struct InheritanceProgenitorGalaxy *progenitors = NULL;
  int current_snap, first_occupied, required;

  first_occupied = snapshot_find_most_massive_progenitor(view, prev, halonr);

  const int nprogenitors = narrow_int64_to_int_checked(
      snapshot_count_progenitor_galaxies(view, prev, halonr), "snapshot progenitor galaxy count");

  required = ngalstart + nprogenitors;
  if (nprogenitors == 0 && halonr == mimic_tree_get_FirstHaloInFOFgroup(view, halonr)) {
    required++;
  }
  snapshot_ensure_workspace_capacity(state, required);

  if (nprogenitors > 0) {
    progenitors = snapshot_ensure_progenitor_scratch(state, nprogenitors);
    snapshot_gather_progenitor_galaxies(view, prev, halonr, first_occupied, progenitors);
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
  descendant.unique_galaxy_id = snapshot_make_unique_galaxy_id(&cur->slab, halonr);
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
 * progenitor was already processed when snapshot N-1 was swept.
 *
 * @return  Number of subhalos in the group (its members are now accounted for).
 */
static int snapshot_process_fof_group(struct SnapshotDriverState *state,
                                      struct SnapshotGeneration *cur,
                                      const struct SnapshotGatherContext *prev, int central) {
  const struct HaloInputView view = {cur->slab.halos, cur->slab.nhalos};
  const int nsegments = count_fof_subhalos(view, central);
  struct OutputBufferSegment *segments = snapshot_ensure_segment_scratch(state, nsegments);
  int segment_index = 0;
  int fofhalo = central;
  int ngal = 0;

  /* Cycle safety: count_fof_subhalos() above already walked this exact chain
   * over the same immutable slab with a bounded-iteration guard, so this second
   * traversal cannot loop. */
  while (fofhalo >= 0) {
    const int workspace_start = ngal;
    const int source_halo = fofhalo;

    ngal = snapshot_join_progenitor_halos(state, cur, prev, fofhalo, ngal);

    /* Stamp the FoF-central catalog virial mass onto every member of this
     * subhalo slice before physics runs, exactly as the tree FoF block in build_model.c. */
    const double central_mvir =
        get_virial_mass(view, mimic_tree_get_FirstHaloInFOFgroup(view, source_halo));
    for (int p = workspace_start; p < ngal; p++) {
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

  for (int i = 0; i < segment_index; i++) {
    cur->aux[segments[i].source_id].FirstHalo = segments[i].output_first;
    cur->aux[segments[i].source_id].NHalos = segments[i].output_count;
  }

  return segment_index;
}

/* ------------------------------------------------------------------------- */
/* Output (the only HDF5-dependent code in this driver)                       */
/* ------------------------------------------------------------------------- */

/*
 * Return the tree driver's output-buffer globals to their unowned state.
 *
 * This driver owns two output buffers and lends one to the shared writer for
 * the duration of a single save call (see snapshot_write_output). Outside that
 * window the globals must point at nothing: the generation they were lent from
 * is freed at its rotation, so leaving them set would leave a dangling pointer
 * live for the rest of the run for any shared code that reads them.
 */
static void snapshot_clear_output_globals(void) {
  ProcessedHalos = NULL;
  NumProcessedHalos = 0;
  MaxProcessedHalos = 0;
}

#ifdef HDF5

/* Create the single output partition and arm its cleanup registration. */
static void snapshot_open_output(void) {
  snapshot_register_output_paths();

  for (int n = 0; n < MimicConfig.NOUT; n++) {
    TotHalosPerSnap[n] = 0;
  }

  prepare_output_files(SNAPSHOT_OUTPUT_ID);
}

/*
 * Buffer this snapshot's processed halos into the shared HDF5 write buffers.
 *
 * save_halos_hdf5() reads the driver's output buffer through the ProcessedHalos
 * globals and converts each record through the supplied view, so the globals
 * are pointed at this generation and the view is this snapshot's slab — which
 * is exactly why the raw slab must still be live here (output conversion
 * recomputes Rvir/Vvir from it).
 *
 * The loan lasts exactly as long as the save call: this generation's buffer is
 * freed when the rotation releases it, which for a sparse output list can be
 * several snapshots before the next save, so the globals are cleared again on
 * the way out rather than left pointing into freed memory.
 */
static void snapshot_write_output(struct SnapshotGeneration *cur) {
  const struct HaloInputView view = {cur->slab.halos, cur->slab.nhalos};

  ProcessedHalos = cur->processed.halos;
  NumProcessedHalos = cur->processed.count;
  MaxProcessedHalos = cur->processed.capacity;

  save_halos_hdf5(SNAPSHOT_OUTPUT_ID, SNAPSHOT_OUTPUT_TREE_ID, view);

  snapshot_clear_output_globals();

  /* VERBOSE_LOG, not DEBUG_LOG: this driver enables the tree driver's debug
   * rate limiting for the physics phase, which caps each DEBUG_LOG site at
   * DEBUG_LOG_MAX_CALLS. These lifecycle lines are bounded by the snapshot
   * count, not by halo count, and are the operator's (and the integration
   * suite's) evidence of the rotation, so they must not be capped. */
  VERBOSE_LOG("Buffered snapshot %" PRId64 " output (%" PRId64 " galax%s)", cur->snapnum,
              cur->processed.count, cur->processed.count == 1 ? "y" : "ies");
}

/* Flush, stamp per-snapshot counts and metadata, and close the partition. */
static void snapshot_finalize_output(void) {
  flush_hdf5_buffers(SNAPSHOT_OUTPUT_ID);

  for (int n = 0; n < MimicConfig.NOUT; n++) {
    write_hdf5_attrs(n, SNAPSHOT_OUTPUT_ID);
  }

  if (HDF5_current_file_id >= 0) {
    DEBUG_LOG("Closing HDF5 file (ID %lld) for the snapshot-ordered partition",
              (long long)HDF5_current_file_id);
    /* A deferred write error (a full filesystem, a failed flush of a chunk
     * still in the library cache) surfaces here and nowhere else, so an ignored
     * status would let a truncated partition exit successfully. */
    const herr_t close_status = H5Fclose(HDF5_current_file_id);
    HDF5_current_file_id = -1;
    if (close_status < 0) {
      FATAL_ERROR("Failed to close the HDF5 output partition %d ('%s'); its galaxy data may be "
                  "truncated or unflushed (check free space and file permissions)",
                  SNAPSHOT_OUTPUT_ID, snapshot_output_paths[0]);
    }
  }
}

#else /* !HDF5 */

/*
 * Unreachable in practice: snapshot_hdf5 is the only registered snapshot
 * reader, and a non-HDF5 build registers none, so a snapshot-ordered
 * configuration is rejected at startup long before the driver runs. These stubs
 * exist so this translation unit links in a USE-HDF5=no build without any HDF5
 * writer symbol, and fail loudly rather than silently producing nothing if that
 * ever stops being true.
 */
#define SNAPSHOT_NO_HDF5_MESSAGE                                                                   \
  "Snapshot-ordered runs require an HDF5-enabled build; rebuild with USE-HDF5=yes"

static void snapshot_open_output(void) { FATAL_ERROR(SNAPSHOT_NO_HDF5_MESSAGE); }

static void snapshot_write_output(struct SnapshotGeneration *cur) {
  (void)cur;
  FATAL_ERROR(SNAPSHOT_NO_HDF5_MESSAGE);
}

static void snapshot_finalize_output(void) { FATAL_ERROR(SNAPSHOT_NO_HDF5_MESSAGE); }

#endif /* HDF5 */

static int snapshot_is_output_snapshot(int64_t snapnum) {
  for (int n = 0; n < MimicConfig.NOUT; n++) {
    if ((int64_t)MimicConfig.ListOutputSnaps[n] == snapnum) {
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* Generation lifecycle                                                       */
/* ------------------------------------------------------------------------- */

/* Load snapshot `snapnum` into `gen` and allocate its per-halo aux array and
 * output buffer. */
static void snapshot_acquire_generation(struct SnapshotDriverState *state,
                                        struct SnapshotGeneration *gen, int64_t snapnum) {
  snapshot_reader_load_slab(state->reader, snapnum, &gen->slab);
  gen->snapnum = snapnum;

  /* Halo indices are int throughout the halo structures (struct Halo.HaloNr and
   * the generated accessors), and the format's own int32 topology bound already
   * forbids a larger slab (SNAPSHOT-HDF5-FORMAT.md invariant 2). Check it here
   * rather than trusting the producer. */
  if (gen->slab.nhalos > (int64_t)INT_MAX) {
    FATAL_ERROR("Snapshot %" PRId64 " holds %" PRId64 " halos, above the %d the driver can index",
                snapnum, gen->slab.nhalos, INT_MAX);
  }

  const int64_t nhalos = gen->slab.nhalos;

  gen->aux =
      mymalloc_cat(sizeof(struct SnapshotHaloAux) * (size_t)(nhalos > 0 ? nhalos : 1), MEM_HALOS);
  for (int64_t i = 0; i < nhalos; i++) {
    gen->aux[i].FirstHalo = -1;
    gen->aux[i].NHalos = 0;
  }

  /* The output count of a snapshot is its halo count plus the orphans carried
   * forward from earlier snapshots, so start one halo per slab entry (plus the
   * shared minimum growth increment) and let the marshaller's own growth take
   * it from there. The tree driver's MAXHALOFAC over-allocation is not copied:
   * at slab scale a five-fold reservation is hundreds of megabytes. */
  gen->processed.count = 0;
  gen->processed.capacity = nhalos + MIN_HALO_ARRAY_GROWTH;
  gen->processed.halos =
      mymalloc_cat((size_t)gen->processed.capacity * sizeof(struct Halo), MEM_HALOS);
  memset(gen->processed.halos, 0, (size_t)gen->processed.capacity * sizeof(struct Halo));
}

/* Release a generation's raw slab, aux array, output buffer and galaxy slots.
 * The pool is reset rather than destroyed: it is the pool the generation two
 * snapshots later will allocate from. */
static void snapshot_release_generation(struct SnapshotDriverState *state,
                                        struct SnapshotGeneration *gen) {
  const int64_t released = gen->snapnum;

  snapshot_reader_release_slab(state->reader, &gen->slab);

  /* Belt and braces: snapshot_write_output() already returns the globals to
   * their unowned state, so this only ever matters if a future edit stops doing
   * that. Clearing before the free keeps "the globals point at a live buffer or
   * at nothing" true at every point in the rotation. */
  snapshot_clear_output_globals();

  myfree(gen->processed.halos);
  gen->processed.halos = NULL;
  gen->processed.count = 0;
  gen->processed.capacity = 0;

  myfree(gen->aux);
  gen->aux = NULL;

  galaxy_pool_reset(gen->pool);
  gen->snapnum = SNAPSHOT_SLAB_NO_SNAPSHOT;

  VERBOSE_LOG("Released snapshot %" PRId64 " (raw slab and processed generation)", released);
}

/* ------------------------------------------------------------------------- */
/* Driver                                                                     */
/* ------------------------------------------------------------------------- */

/**
 * @brief   Run a snapshot-ordered configuration end to end.
 *
 * Opens the configured dataset, creates the single output partition, then walks
 * every snapshot in increasing time order holding at most two slab generations
 * live at once. For snapshot N: load slab N (N-1 still live), process every FoF
 * group against N-1, release generation N-1, then write snapshot N's output if
 * it was requested. After the final snapshot the last generation is released,
 * the partition is finalized and the dataset closed; main.c writes the master
 * file afterwards and only then disarms this driver's output cleanup.
 */
void run_snapshot_driver(void) {
  struct SnapshotDriverState state;
  struct SnapshotRunInfo info;
  ProgressBar bar;

  memset(&state, 0, sizeof(state));
  state.reader = MimicConfig.snapshot_reader;
  for (int slot = 0; slot < 2; slot++) {
    state.gen[slot].snapnum = SNAPSHOT_SLAB_NO_SNAPSHOT;
    state.gen[slot].slab = snapshot_slab_empty();
  }

  snapshot_reader_open_run(state.reader, &info);
  INFO_LOG("Opened snapshot-ordered run '%s': %" PRId64 " snapshot%s, format_version %" PRId32
           ", %" PRId64 " forest%s, max halo rank in forest %" PRId64,
           state.reader->name, info.snapshot_count, info.snapshot_count == 1 ? "" : "s",
           info.format_version, info.n_forests_total, info.n_forests_total == 1 ? "" : "s",
           info.max_halo_rank_in_forest);

  log_phase_banner(PHASE_TREE_PROCESSING);
  enable_debug_log_rate_limiting();

  /* The tree driver's per-partition globals have no meaning here; the writers
   * that still read them are guarded on the processing order (Slice 8), and
   * FileNum names the one partition this run produces. */
  FileNum = SNAPSHOT_OUTPUT_ID;
  TreeID = 0;
  GlobalForestOffset = 0;

  snapshot_open_output();

  state.workspace_capacity = INITIAL_FOF_HALOS;
  state.workspace = mymalloc_cat((size_t)state.workspace_capacity * sizeof(struct Halo), MEM_HALOS);
  memset(state.workspace, 0, (size_t)state.workspace_capacity * sizeof(struct Halo));

  for (int slot = 0; slot < 2; slot++) {
    state.gen[slot].pool = galaxy_pool_create(0);
  }

  INFO_LOG("Processing %" PRId64 " snapshot%s → 1 output file", info.snapshot_count,
           info.snapshot_count == 1 ? "" : "s");
  progress_bar_init(&bar, info.snapshot_count, "");

  for (int64_t snapnum = 0; snapnum < info.snapshot_count; snapnum++) {
    struct SnapshotGeneration *cur = &state.gen[snapnum % 2];
    struct SnapshotGeneration *previous = (snapnum > 0) ? &state.gen[(snapnum - 1) % 2] : NULL;
    struct SnapshotGatherContext prev;

    progress_bar_update(&bar, snapnum);

    snapshot_acquire_generation(&state, cur, snapnum);

    /* Derived from the slabs' own state, not the loop counter: a rotation bug
       (wrong slot, a skipped or early release) would then show up here rather
       than being masked by an assertion that could never be false. */
    const int live_slabs = (!snapshot_slab_is_empty(&state.gen[0].slab)) +
                           (!snapshot_slab_is_empty(&state.gen[1].slab));
    VERBOSE_LOG("Loaded snapshot %" PRId64 " (%" PRId64 " halos); %d slab%s live", snapnum,
                cur->slab.nhalos, live_slabs, live_slabs == 1 ? "" : "s");

    if (previous != NULL) {
      prev.view.halos = previous->slab.halos;
      prev.view.count = previous->slab.nhalos;
      prev.aux = previous->aux;
      prev.processed = previous->processed.halos;
    } else {
      prev.view.halos = NULL;
      prev.view.count = 0;
      prev.aux = NULL;
      prev.processed = NULL;
    }

    /* Walk FoF groups in slab order, processing each group when its central is
     * first met. Every halo names a central whose own FirstHaloInFOFgroup is
     * itself (SNAPSHOT-HDF5-FORMAT.md invariant 6), so this visits every group
     * exactly once; the member tally below proves it visited every halo. */
    const struct HaloInputView view = {cur->slab.halos, cur->slab.nhalos};
    const int nhalos = (int)cur->slab.nhalos;
    int64_t members_processed = 0;

    for (int halonr = 0; halonr < nhalos; halonr++) {
      if (mimic_tree_get_FirstHaloInFOFgroup(view, halonr) == halonr) {
        members_processed += snapshot_process_fof_group(&state, cur, &prev, halonr);
      }
    }

    if (members_processed != cur->slab.nhalos) {
      FATAL_ERROR("Snapshot %" PRId64 " FoF chains cover %" PRId64 " of %" PRId64
                  " halos; the slab's FoF links are inconsistent",
                  snapnum, members_processed, cur->slab.nhalos);
    }

    /* Rotation: every FoF group at N has now deep-copied whatever it inherits,
     * so generation N-1 (raw slab, aux, output buffer and galaxies) is dead.
     * Snapshot N's own output is written afterwards, from the still-live slab N. */
    if (previous != NULL) {
      snapshot_release_generation(&state, previous);
    }

    if (snapshot_is_output_snapshot(snapnum)) {
      snapshot_write_output(cur);
    }
  }

  progress_bar_finish(&bar);

  if (info.snapshot_count > 0) {
    snapshot_release_generation(&state, &state.gen[(info.snapshot_count - 1) % 2]);
  }

  snapshot_finalize_output();
  snapshot_reader_close_run(state.reader);

  for (int slot = 0; slot < 2; slot++) {
    galaxy_pool_destroy(state.gen[slot].pool);
    state.gen[slot].pool = NULL;
  }

  if (state.segments != NULL) {
    myfree(state.segments);
  }
  if (state.progenitor_scratch != NULL) {
    myfree(state.progenitor_scratch);
  }
  myfree(state.workspace);

  /* Already cleared after each output call and at each release; repeated here
   * so the driver cannot return with them set under any path. */
  snapshot_clear_output_globals();

  disable_debug_log_rate_limiting();
}
