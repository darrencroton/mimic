/**
 * @file    tree/read_ctrees_ascii.c
 * @brief   Consistent-Trees ASCII merger-tree reader.
 *
 * Reads Rockstar/Consistent-Trees ASCII output (`forests.list` + `locations.dat`
 * + `tree_i_j_k.dat`) and presents it to the core as the partition/unit model:
 * one output partition per planned forest-count chunk, one unit per forest. The
 * heavy lifting lives in the vendored, format-independent helpers under
 * tree/ctrees: this file is the Mimic seam that drives them, bridges the
 * reconstructed `struct halo_data` forest into the generated per-simulation
 * `struct RawHalo`, and owns the run-scoped ASCII file descriptors.
 *
 * Consistent-Trees is a FORMAT, not a simulation. The reader is therefore
 * simulation-agnostic and reads its parameters (SimulationDir, particle mass,
 * cosmology) from whatever simulation package is compiled in. A package that
 * uses this reader must declare a RawHalo with the L-Halo field set the bridge
 * below writes (the five merger pointers plus Len, M_Crit200, Pos, Vel, VelDisp,
 * Vmax, Spin, MostBoundID, SnapNum) and ctrees-native units (Msun/h masses,
 * Mpc/h positions) so the generated reference-unit accessors apply the catalog
 * -> reference conversion. See docs/dev/CTREES-UCHUU-VALIDATION.md for the
 * package + validation checklist.
 *
 * Split of responsibilities:
 *   - topology: read_forests/read_locations/assign_forest_ids/sort + fix_flybys/
 *     fix_upid/assign_mergertree_indices reconstruct the L-Halo merger pointers
 *     from the ctrees id/pid/upid/desc_id columns;
 *   - reader conventions (order-dependent on the NATIVE Mvir): spin normalisation
 *     (J / Mvir) and the particle-count estimate (round(Mvir / particle_mass));
 *   - unit conversions (mass * 1e-10 into 1e10 Msun/h, positions in Mpc/h): handled
 *     downstream by the generated reference-unit accessors, NOT here.
 *
 * ASCII does not expose per-forest halo counts before loading, so chunking uses
 * the exact `output.forests_per_file` knob. Per-chunk costs are intentionally
 * uniform: LPT assignment then degrades to deterministic round-robin.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "config.h"
#include "constants.h"
#include "error.h"
#include "globals.h"
#include "memory.h"
#include "types.h"

#include "tree/ctrees/ctrees_compat.h"
#include "tree/ctrees/ctrees_utils.h"
#include "tree/ctrees/forest_utils.h"
#include "tree/ctrees/parse_ctrees.h"
#include "tree/chunk_plan.h"
#include "tree/read_ctrees_ascii.h"
#include "tree/read_ctrees_common.h"
#include "tree/reader.h"

/* Run-scoped metadata plus the one staged chunk. One reader instance per
   process, so a file-static record is sufficient. */
struct ctrees_ascii_partition {
  int64_t totntrees;                        /* run-scoped total tree rows */
  int64_t totnforests;                      /* run-scoped total forest count */
  struct locations_with_forests *locations; /* [totntrees], sorted by forest/file/offset */
  int64_t *ntrees_per_global_forest;        /* [totnforests] number of trees per forest */
  int64_t *start_treenum_per_global_forest; /* [totnforests] first sorted tree row */
  struct ChunkPlan chunk_plan;              /* run-scoped output chunk ranges */
  double *chunk_costs;                      /* [chunk_plan.nchunks], uniform for ASCII */
  int numfiles;                             /* number of distinct tree_i_j_k.dat files */
  int *open_fds;                            /* [numfiles] owned descriptors */
  struct ctrees_column_to_ptr *column_info; /* ctrees column -> struct field mapping */

  int64_t start_forestnum;           /* global first forest in the staged chunk */
  int64_t nforests;                  /* units in this chunk (== global Ntrees) */
  int64_t ntrees_this_chunk;         /* total trees across this chunk's forests */
  int64_t *ntrees_per_forest;        /* [nforests] number of trees in each forest */
  int64_t *start_treenum_per_forest; /* [nforests] first staged tree index */
  off_t *tree_offsets;               /* [ntrees_this_chunk] byte offset of each tree */
  int *tree_fd;                      /* [ntrees_this_chunk] fd for each tree (borrowed) */
};
static struct ctrees_ascii_partition CT;

/**
 * @brief   Shared Consistent-Trees -> L-Halo value conventions (both readers).
 *
 * Operates on the NATIVE Mvir (Msun/h): spin normalisation (J / Mvir) and the
 * particle-count estimate (round(Mvir * 1e-10 / particle_mass)). Mass and
 * position unit scaling is deliberately left to the generated reference-unit
 * accessors. Declared in read_ctrees_common.h; the HDF5 reader calls this too.
 */
void apply_ctrees_value_conventions(struct halo_data *halos, const int64_t nhalos) {
  for (int64_t i = 0; i < nhalos; i++) {
    const float mvir_native = halos[i].Mvir;

    /* Spin normalisation: J / Mvir on the un-scaled mass. */
    if (mvir_native != 0.0f) {
      const double inv_mvir = 1.0 / (double)mvir_native;
      for (int k = 0; k < 3; k++) {
        halos[i].Spin[k] = (float)((double)halos[i].Spin[k] * inv_mvir);
      }
    }

    /* Approximate particle count from native Mvir and the simulation particle
       mass (PartMass is 1e10 Msun/h; Mvir is Msun/h). */
    if (MimicConfig.PartMass > 0.0) {
      const double len_particles = (double)mvir_native * 1e-10 / MimicConfig.PartMass;
      if (!isfinite(len_particles) || len_particles < 0.0 || len_particles > (double)INT_MAX) {
        FATAL_ERROR("Consistent-Trees: halo %" PRId64 " has invalid derived particle count %.17g "
                    "(Mvir=%g, PartMass=%g)",
                    i, len_particles, (double)mvir_native, MimicConfig.PartMass);
      }
      halos[i].Len = (int)round(len_particles);
    } else {
      halos[i].Len = 0;
    }
  }
}

static int validate_ctrees_snapshot_range(const struct halo_data *halos, const int64_t nhalos,
                                          const char *context) {
  for (int64_t i = 0; i < nhalos; i++) {
    if (halos[i].SnapNum < 0 || halos[i].SnapNum > MimicConfig.LastSnapshotNr) {
      fprintf(stderr,
              "Error: %s halo %" PRId64 " has SnapNum=%d outside configured range [0, %d]\n",
              context, i, halos[i].SnapNum, MimicConfig.LastSnapshotNr);
      return EXIT_FAILURE;
    }
  }
  return EXIT_SUCCESS;
}

/**
 * @brief   Apply the full Consistent-Trees ASCII -> L-Halo conventions.
 *
 * The shared value conventions (spin, Len) plus the ASCII-only steps: carry the
 * ctrees halo id through as MostBoundID and seed the pre-topology link sentinels
 * (the HDF5 reader reads both id and merger pointers straight from file, so it
 * does not need these).
 */
void convert_ctrees_to_lht(struct halo_data *halos, const struct additional_info *info,
                           const int64_t nhalos) {
  apply_ctrees_value_conventions(halos, nhalos);
  for (int64_t i = 0; i < nhalos; i++) {
    /* Carry the Rockstar/ctrees halo id through as the most-bound id. */
    halos[i].MostBoundID = info[i].id;

    /* Pre-topology sentinels; assign_mergertree_indices fills the real links
       (it relies on FirstProgenitor/NextProgenitor/NextHaloInFOFgroup starting
       at -1 for halos it never touches). */
    halos[i].Descendant = -1;
    halos[i].FirstProgenitor = -1;
    halos[i].NextProgenitor = -1;
    halos[i].FirstHaloInFOFgroup = -1;
    halos[i].NextHaloInFOFgroup = -1;
  }
}

/* Copy one reconstructed halo_data record into the generated RawHalo layout. The
   ctrees-local halo_data carries fields by name; RawHalo field order is set by
   the compiled simulation package, so the bridge is explicit per field. The
   target field names are the reader's contract on that package (see file header). */
void bridge_halo_data_to_rawhalo(struct RawHalo *out, const struct halo_data *in) {
  out->Descendant = in->Descendant;
  out->FirstProgenitor = in->FirstProgenitor;
  out->NextProgenitor = in->NextProgenitor;
  out->FirstHaloInFOFgroup = in->FirstHaloInFOFgroup;
  out->NextHaloInFOFgroup = in->NextHaloInFOFgroup;
  out->Len = in->Len;
  out->M_Crit200 = in->Mvir; /* native Msun/h; accessor converts to 1e10 Msun/h */
  for (int k = 0; k < 3; k++) {
    out->Pos[k] = in->Pos[k];
    out->Vel[k] = in->Vel[k];
    out->Spin[k] = in->Spin[k];
  }
  out->VelDisp = in->VelDisp;
  out->Vmax = in->Vmax;
  out->MostBoundID = in->MostBoundID;
  out->SnapNum = in->SnapNum;
}

/* Build the ctrees column -> struct-field mapping by parsing the header line of
   the first tree file. Pins the supported Consistent-Trees column set. */
static void setup_column_info(void) {
  char column_names[][PARSE_CTREES_MAX_COLNAME_LEN] = {
      "scale", "id",   "desc_scale", "desc_id", "pid",      "upid",     "mvir",
      "vrms",  "vmax", "x",          "y",       "z",        "vx",       "vy",
      "vz",    "Jx",   "Jy",         "Jz",      "snap_num", "snap_idx", /* older=snap_num,
                                                                           newer=snap_idx */
      "M200b", "M200c"};
  enum parse_numeric_types dest_field_types[] = {F64, I64, F64, I64, I64, I64, F32, F32,
                                                 F32, F32, F32, F32, F32, F32, F32, F32,
                                                 F32, F32, I32, I32, F32, F32};
  int64_t base_ptr_idx[] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  size_t dest_offset_to_element[] = {
      offsetof(struct additional_info, scale), offsetof(struct additional_info, id),
      offsetof(struct additional_info, desc_scale), offsetof(struct additional_info, descid),
      offsetof(struct additional_info, pid), offsetof(struct additional_info, upid),
      offsetof(struct halo_data, Mvir), offsetof(struct halo_data, VelDisp),
      offsetof(struct halo_data, Vmax), offsetof(struct halo_data, Pos[0]),
      offsetof(struct halo_data, Pos[1]), offsetof(struct halo_data, Pos[2]),
      offsetof(struct halo_data, Vel[0]), offsetof(struct halo_data, Vel[1]),
      offsetof(struct halo_data, Vel[2]), offsetof(struct halo_data, Spin[0]),
      offsetof(struct halo_data, Spin[1]), offsetof(struct halo_data, Spin[2]),
      /* only one of snap_num/snap_idx is present */
      offsetof(struct halo_data, SnapNum), offsetof(struct halo_data, SnapNum),
      offsetof(struct halo_data, M_Mean200), offsetof(struct halo_data, M_TopHat)};

  const int nwanted = (int)(sizeof(column_names) / sizeof(column_names[0]));

  char header_path[2 * MAX_STRING_LEN + 1];
  snprintf(header_path, sizeof(header_path), "%s/%s", MimicConfig.SimulationDir,
           MimicConfig.TreeName);

  CT.column_info = mymalloc_cat(sizeof(*CT.column_info), MEM_IO);
  const int status =
      parse_header_ctrees(column_names, dest_field_types, base_ptr_idx, dest_offset_to_element,
                          nwanted, header_path, CT.column_info);
  if (status != EXIT_SUCCESS) {
    FATAL_ERROR("Failed to parse Consistent-Trees column header from '%s' (status %d)", header_path,
                status);
  }
}

static int prepare_run_ctrees_ascii_state(void);
static int stage_range_ctrees_ascii(int64_t start_forestnum, int64_t nforests);

static void raise_open_file_limit_ctrees_ascii(void) {
  /* One descriptor per tree file: raise the soft open-file limit to the hard
     limit (a documented scaling constraint of the ctrees ASCII layout). */
  struct rlimit rlp;
  if (getrlimit(RLIMIT_NOFILE, &rlp) == 0) {
    rlp.rlim_cur = rlp.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rlp);
  }
}

static void free_unowned_files_fd_ctrees_ascii(struct filenames_and_fd *files_fd) {
  if (files_fd == NULL) {
    return;
  }
  if (files_fd->fd != NULL) {
    for (uint32_t i = 0; i < files_fd->nallocated; i++) {
      if (files_fd->fd[i] >= 0) {
        close(files_fd->fd[i]);
      }
    }
    myfree(files_fd->fd);
    files_fd->fd = NULL;
  }
  if (files_fd->numtrees_per_file != NULL) {
    myfree(files_fd->numtrees_per_file);
    files_fd->numtrees_per_file = NULL;
  }
  files_fd->numfiles = 0;
  files_fd->nallocated = 0;
}

static void clear_staged_range_ctrees_ascii(void) {
  if (CT.tree_fd != NULL) {
    myfree(CT.tree_fd);
    CT.tree_fd = NULL;
  }
  if (CT.tree_offsets != NULL) {
    myfree(CT.tree_offsets);
    CT.tree_offsets = NULL;
  }
  if (CT.start_treenum_per_forest != NULL) {
    myfree(CT.start_treenum_per_forest);
    CT.start_treenum_per_forest = NULL;
  }
  if (CT.ntrees_per_forest != NULL) {
    myfree(CT.ntrees_per_forest);
    CT.ntrees_per_forest = NULL;
  }
  CT.start_forestnum = 0;
  CT.nforests = 0;
  CT.ntrees_this_chunk = 0;
}

static void clear_staged_tree_globals_ctrees_ascii(void) {
  if (InputTreeFirstHalo != NULL) {
    myfree(InputTreeFirstHalo);
    InputTreeFirstHalo = NULL;
  }
  if (InputTreeNHalos != NULL) {
    myfree(InputTreeNHalos);
    InputTreeNHalos = NULL;
  }
}

static void teardown_run_ctrees_ascii_state(void) {
  clear_staged_range_ctrees_ascii();

  if (CT.open_fds != NULL) {
    for (int i = 0; i < CT.numfiles; i++) {
      if (CT.open_fds[i] >= 0) {
        close(CT.open_fds[i]);
      }
    }
    myfree(CT.open_fds);
    CT.open_fds = NULL;
  }
  if (CT.column_info != NULL) {
    myfree(CT.column_info);
    CT.column_info = NULL;
  }
  if (CT.chunk_costs != NULL) {
    myfree(CT.chunk_costs);
    CT.chunk_costs = NULL;
  }
  chunk_plan_free(&CT.chunk_plan);
  if (CT.start_treenum_per_global_forest != NULL) {
    myfree(CT.start_treenum_per_global_forest);
    CT.start_treenum_per_global_forest = NULL;
  }
  if (CT.ntrees_per_global_forest != NULL) {
    myfree(CT.ntrees_per_global_forest);
    CT.ntrees_per_global_forest = NULL;
  }
  if (CT.locations != NULL) {
    myfree(CT.locations);
    CT.locations = NULL;
  }

  memset(&CT, 0, sizeof(CT));
}

static int build_chunk_plan_ctrees_ascii(void) {
  enum { CTREES_ASCII_CHUNK_PLAN_BATCH = 4096 };

  if (MimicConfig.ForestsPerFile <= 0) {
    fprintf(stderr,
            "Error: consistent_trees_ascii requires output.forests_per_file > 0 because ASCII "
            "catalogues cannot derive chunk sizes from output.target_file_size_mb\n");
    return EXIT_FAILURE;
  }
  if (CT.totnforests <= 0) {
    fprintf(stderr, "Error: Consistent-Trees ASCII chunk planner needs at least one forest\n");
    return EXIT_FAILURE;
  }

  struct ChunkPlanBuilder builder;
  if (chunk_plan_builder_init(&builder, 1.0, MimicConfig.ForestsPerFile) != 0) {
    fprintf(stderr,
            "Error: failed to initialise Consistent-Trees ASCII chunk planner "
            "(forests_per_file=%" PRId64 ")\n",
            MimicConfig.ForestsPerFile);
    return EXIT_FAILURE;
  }

  double unit_sizes[CTREES_ASCII_CHUNK_PLAN_BATCH];
  for (int i = 0; i < CTREES_ASCII_CHUNK_PLAN_BATCH; i++) {
    unit_sizes[i] = 1.0;
  }

  int64_t remaining = CT.totnforests;
  while (remaining > 0) {
    const int64_t batch =
        remaining < CTREES_ASCII_CHUNK_PLAN_BATCH ? remaining : CTREES_ASCII_CHUNK_PLAN_BATCH;
    if (chunk_plan_builder_add_file(&builder, batch, unit_sizes) != 0) {
      chunk_plan_free(&builder.plan);
      fprintf(stderr,
              "Error: failed to feed Consistent-Trees ASCII chunk planner "
              "(forests_per_file=%" PRId64 ")\n",
              MimicConfig.ForestsPerFile);
      return EXIT_FAILURE;
    }
    remaining -= batch;
  }

  if (chunk_plan_builder_finish(&builder, &CT.chunk_plan) != 0) {
    chunk_plan_free(&builder.plan);
    fprintf(stderr,
            "Error: failed to build Consistent-Trees ASCII chunk plan "
            "(forests_per_file=%" PRId64 ")\n",
            MimicConfig.ForestsPerFile);
    return EXIT_FAILURE;
  }
  if (CT.chunk_plan.nchunks >= INT_MAX) {
    chunk_plan_free(&CT.chunk_plan);
    fprintf(stderr,
            "Error: Consistent-Trees ASCII chunk count %" PRId64
            " cannot be represented by the reader interface\n",
            CT.chunk_plan.nchunks);
    return EXIT_FAILURE;
  }

  CT.chunk_costs = mymalloc_cat((size_t)CT.chunk_plan.nchunks * sizeof(*CT.chunk_costs), MEM_IO);
  for (int64_t chunk = 0; chunk < CT.chunk_plan.nchunks; chunk++) {
    CT.chunk_costs[chunk] = 1.0;
  }

  DEBUG_LOG("Consistent-Trees ASCII chunk plan: nchunks=%" PRId64 ", forests_per_file=%" PRId64,
            CT.chunk_plan.nchunks, MimicConfig.ForestsPerFile);
  for (int64_t chunk = 0; chunk < CT.chunk_plan.nchunks; chunk++) {
    const struct ChunkPlanRange *range = &CT.chunk_plan.chunks[chunk];
    DEBUG_LOG("  chunk %" PRId64 ": forests [%" PRId64 ", %" PRId64 "), cost=%g", chunk,
              range->start_forest, range->start_forest + range->nforests, CT.chunk_costs[chunk]);
  }

  return EXIT_SUCCESS;
}

static int prepare_run_ctrees_ascii_state(void) {
  struct filenames_and_fd files_fd = {0};
  int64_t *treeids = NULL, *forestids = NULL;

  if (MimicConfig.ForestsPerFile <= 0) {
    fprintf(stderr,
            "Error: consistent_trees_ascii requires output.forests_per_file > 0 because ASCII "
            "catalogues cannot derive chunk sizes from output.target_file_size_mb\n");
    return EXIT_FAILURE;
  }

  raise_open_file_limit_ctrees_ascii();

  char locations_file[2 * MAX_STRING_LEN + 1], forests_file[2 * MAX_STRING_LEN + 1];
  snprintf(locations_file, sizeof(locations_file), "%s/locations.dat", MimicConfig.SimulationDir);
  snprintf(forests_file, sizeof(forests_file), "%s/forests.list", MimicConfig.SimulationDir);

  const int64_t totntrees = read_forests(forests_file, &forestids, &treeids);
  if (totntrees < 0) {
    fprintf(stderr,
            "Error: failed to read Consistent-Trees forests file '%s' (status %" PRId64 ")\n",
            forests_file, totntrees);
    return EXIT_FAILURE;
  }
  CT.totntrees = totntrees;

  CT.locations = mymalloc_cat(totntrees * sizeof(*CT.locations), MEM_IO);
  memset(CT.locations, 0, totntrees * sizeof(*CT.locations));

  const int64_t nread = read_locations(locations_file, totntrees, CT.locations, &files_fd);
  if (nread != totntrees) {
    fprintf(stderr,
            "Error: Consistent-Trees locations '%s' lists %" PRId64
            " trees but forests '%s' lists %" PRId64 "\n",
            locations_file, nread, forests_file, totntrees);
    myfree(treeids);
    myfree(forestids);
    free_unowned_files_fd_ctrees_ascii(&files_fd);
    return EXIT_FAILURE;
  }

  if (assign_forest_ids(totntrees, CT.locations, forestids, treeids) != EXIT_SUCCESS) {
    fprintf(stderr, "Error: failed to assign Consistent-Trees forest ids\n");
    myfree(treeids);
    myfree(forestids);
    free_unowned_files_fd_ctrees_ascii(&files_fd);
    return EXIT_FAILURE;
  }
  myfree(treeids);
  myfree(forestids);

  /* Group trees by (forestid, fileid, offset). */
  sort_locations_on_fid_file_offset(totntrees, CT.locations);

  int64_t totnforests = 0, prev_forestid = -1;
  for (int64_t i = 0; i < totntrees; i++) {
    if (CT.locations[i].forestid != prev_forestid) {
      totnforests++;
      prev_forestid = CT.locations[i].forestid;
    }
  }
  CT.totnforests = totnforests;
  if (!mimic_unique_galaxy_id_total_forests_valid(totnforests)) {
    fprintf(stderr,
            "Error: Consistent-Trees total forest count %" PRId64
            " exceeds the UniqueGalaxyID encoding limit of %" PRId64 "\n",
            totnforests, mimic_unique_galaxy_id_max_forests());
    free_unowned_files_fd_ctrees_ascii(&files_fd);
    return EXIT_FAILURE;
  }

  CT.ntrees_per_global_forest =
      mymalloc_cat(totnforests * sizeof(*CT.ntrees_per_global_forest), MEM_IO);
  CT.start_treenum_per_global_forest =
      mymalloc_cat(totnforests * sizeof(*CT.start_treenum_per_global_forest), MEM_IO);
  int64_t iforest = -1;
  prev_forestid = -1;
  for (int64_t i = 0; i < totntrees; i++) {
    if (CT.locations[i].forestid != prev_forestid) {
      iforest++;
      prev_forestid = CT.locations[i].forestid;
      CT.start_treenum_per_global_forest[iforest] = i;
      CT.ntrees_per_global_forest[iforest] = 1;
    } else {
      CT.ntrees_per_global_forest[iforest]++;
    }
  }
  if (iforest != totnforests - 1) {
    fprintf(stderr,
            "Error: Consistent-Trees forest bookkeeping mismatch: recovered %" PRId64
            " forests, expected %" PRId64 "\n",
            iforest + 1, totnforests);
    free_unowned_files_fd_ctrees_ascii(&files_fd);
    return EXIT_FAILURE;
  }

  /* Take ownership of the open descriptors for the run. */
  CT.numfiles = files_fd.numfiles;
  CT.open_fds = files_fd.fd;
  myfree(files_fd.numtrees_per_file);

  setup_column_info();
  if (build_chunk_plan_ctrees_ascii() != EXIT_SUCCESS) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

static int stage_range_ctrees_ascii(const int64_t start_forestnum, const int64_t nforests) {
  clear_staged_range_ctrees_ascii();

  if (CT.locations == NULL || CT.ntrees_per_global_forest == NULL) {
    fprintf(stderr, "Error: Consistent-Trees ASCII reader was not prepared before staging\n");
    return EXIT_FAILURE;
  }
  if (nforests < 0 || start_forestnum < 0 || start_forestnum > CT.totnforests - nforests) {
    fprintf(stderr,
            "Error: requested ASCII forest range [%" PRId64 ", %" PRId64 ") outside [0, %" PRId64
            ")\n",
            start_forestnum, start_forestnum + nforests, CT.totnforests);
    return EXIT_FAILURE;
  }
  if (nforests > INT_MAX) {
    fprintf(stderr,
            "Error: Consistent-Trees ASCII chunk has %" PRId64
            " forests, exceeding the 32-bit per-partition limit\n",
            nforests);
    return EXIT_FAILURE;
  }

  CT.start_forestnum = start_forestnum;
  GlobalForestOffset = CT.start_forestnum;
  CT.nforests = nforests;
  Ntrees = (int)nforests;

  int64_t ntrees_this_chunk = 0;
  for (int64_t i = 0; i < nforests; i++) {
    ntrees_this_chunk += CT.ntrees_per_global_forest[start_forestnum + i];
  }
  CT.ntrees_this_chunk = ntrees_this_chunk;

  const int64_t forest_alloc = nforests > 0 ? nforests : 1;
  const int64_t tree_alloc = ntrees_this_chunk > 0 ? ntrees_this_chunk : 1;
  CT.ntrees_per_forest = mymalloc_cat(forest_alloc * sizeof(*CT.ntrees_per_forest), MEM_IO);
  CT.start_treenum_per_forest =
      mymalloc_cat(forest_alloc * sizeof(*CT.start_treenum_per_forest), MEM_IO);
  CT.tree_offsets = mymalloc_cat(tree_alloc * sizeof(*CT.tree_offsets), MEM_IO);
  CT.tree_fd = mymalloc_cat(tree_alloc * sizeof(*CT.tree_fd), MEM_IO);

  InputTreeNHalos = mymalloc_cat(forest_alloc * sizeof(int), MEM_TREES);
  InputTreeFirstHalo = mymalloc_cat(forest_alloc * sizeof(int), MEM_TREES);
  for (int64_t i = 0; i < nforests; i++) {
    InputTreeNHalos[i] = 0;    /* filled per forest in load_unit */
    InputTreeFirstHalo[i] = 0; /* each forest loads into a fresh InputTreeHalos */
  }

  int64_t staged_tree = 0;
  for (int64_t local_forest = 0; local_forest < nforests; local_forest++) {
    const int64_t global_forest = start_forestnum + local_forest;
    const int64_t ntrees = CT.ntrees_per_global_forest[global_forest];
    const int64_t start_treenum = CT.start_treenum_per_global_forest[global_forest];
    CT.ntrees_per_forest[local_forest] = ntrees;
    CT.start_treenum_per_forest[local_forest] = staged_tree;

    for (int64_t i = 0; i < ntrees; i++) {
      const struct locations_with_forests *location = &CT.locations[start_treenum + i];
      if (location->fileid < 0 || location->fileid >= CT.numfiles) {
        fprintf(stderr,
                "Error: Consistent-Trees ASCII tree row %" PRId64
                " has file id %d outside [0, %d)\n",
                start_treenum + i, location->fileid, CT.numfiles);
        clear_staged_tree_globals_ctrees_ascii();
        return EXIT_FAILURE;
      }
      CT.tree_fd[staged_tree] = CT.open_fds[location->fileid];
      CT.tree_offsets[staged_tree] = (off_t)location->offset;
      staged_tree++;
    }
  }

  if (staged_tree != ntrees_this_chunk) {
    fprintf(stderr,
            "Error: Consistent-Trees ASCII staged %" PRId64 " trees but expected %" PRId64 "\n",
            staged_tree, ntrees_this_chunk);
    clear_staged_tree_globals_ctrees_ascii();
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

static void prepare_run_ctrees_ascii(void) {
  memset(&CT, 0, sizeof(CT));
  if (prepare_run_ctrees_ascii_state() != EXIT_SUCCESS) {
    teardown_run_ctrees_ascii_state();
    FATAL_ERROR("Failed to prepare the Consistent-Trees ASCII reader");
  }
}

static int num_partitions_ctrees_ascii(void) { return (int)CT.chunk_plan.nchunks; }

static int partition_output_id_ctrees_ascii(int partition) { return partition; }

static int partition_exists_ctrees_ascii(int partition) {
  return partition >= 0 && (int64_t)partition < CT.chunk_plan.nchunks;
}

static int64_t count_partition_units_ctrees_ascii(int partition) {
  if (!partition_exists_ctrees_ascii(partition)) {
    return -1;
  }
  return CT.chunk_plan.chunks[partition].nforests;
}

static int64_t global_forest_offset_ctrees_ascii(int partition) {
  if (!partition_exists_ctrees_ascii(partition)) {
    FATAL_ERROR("Consistent-Trees ASCII: chunk id %d is outside [0, %" PRId64 ")", partition,
                CT.chunk_plan.nchunks);
  }
  return CT.chunk_plan.chunks[partition].start_forest;
}

static double partition_cost_ctrees_ascii(int partition) {
  if (!partition_exists_ctrees_ascii(partition)) {
    FATAL_ERROR("Consistent-Trees ASCII: chunk id %d is outside [0, %" PRId64 ")", partition,
                CT.chunk_plan.nchunks);
  }
  return CT.chunk_costs[partition];
}

/**
 * @brief   Open one chunk partition and stage its forest range.
 * @param   output_id   The global chunk id (also the output file id).
 */
static void open_partition_ctrees_ascii(int output_id) {
  if (!partition_exists_ctrees_ascii(output_id)) {
    FATAL_ERROR("Consistent-Trees ASCII: chunk id %d is outside [0, %" PRId64 ")", output_id,
                CT.chunk_plan.nchunks);
  }

  const struct ChunkPlanRange *range = &CT.chunk_plan.chunks[output_id];
  if (stage_range_ctrees_ascii(range->start_forest, range->nforests) != EXIT_SUCCESS) {
    clear_staged_range_ctrees_ascii();
    FATAL_ERROR("Failed to set up Consistent-Trees ASCII chunk %d", output_id);
  }
}

/**
 * @brief   Load one forest (unit) into InputTreeHalos as RawHalo records.
 *
 * Reads every tree of the forest into a ctrees-local halo_data array, applies the
 * reader conventions, reconstructs the merger pointers, then bridges the result
 * into the generated RawHalo layout and records the forest's halo count.
 */
static void load_unit_ctrees_ascii(int unit) {
  const int64_t ntrees = CT.ntrees_per_forest[unit];
  const int64_t start_treenum = CT.start_treenum_per_forest[unit];

  const int64_t default_nhalos_per_tree = 1000;
  int64_t nhalos_allocated = default_nhalos_per_tree * (ntrees > 0 ? ntrees : 1);

  struct halo_data *halos = mymalloc_cat(nhalos_allocated * sizeof(*halos), MEM_TREES);
  struct additional_info *info = mymalloc_cat(nhalos_allocated * sizeof(*info), MEM_TREES);

  struct base_ptr_info base_info;
  base_info.num_base_ptrs = 2;
  base_info.base_ptrs[0] = (void **)&halos;
  base_info.base_element_size[0] = sizeof(struct halo_data);
  base_info.base_ptrs[1] = (void **)&info;
  base_info.base_element_size[1] = sizeof(struct additional_info);
  base_info.N = 0;
  base_info.nallocated = nhalos_allocated;

  for (int64_t i = 0; i < ntrees; i++) {
    const int64_t treenum = i + start_treenum;
    const int64_t prev_N = base_info.N;
    if (read_single_tree_ctrees(CT.tree_fd[treenum], CT.tree_offsets[treenum], CT.column_info,
                                &base_info) != EXIT_SUCCESS) {
      FATAL_ERROR("Failed to read Consistent-Trees tree %" PRId64 " of forest %d", i, unit);
    }
    /* read_single_tree_ctrees may realloc halos/info; re-read the current
       pointers before applying conventions to the just-read rows. */
    const int64_t nhalos = base_info.N - prev_N;
    convert_ctrees_to_lht(&halos[prev_N], &info[prev_N], nhalos);
  }

  const int64_t totnhalos = base_info.N;
  if (totnhalos >= INT_MAX) {
    FATAL_ERROR("Consistent-Trees forest %d has %" PRId64 " halos, which cannot be indexed by an "
                "int",
                unit, totnhalos);
  }
  if (totnhalos >= TREE_MUL_FAC) {
    FATAL_ERROR("Consistent-Trees forest %d has %" PRId64
                " halos, at or above the unique-galaxy-id "
                "limit of %lld",
                unit, totnhalos, (long long)TREE_MUL_FAC);
  }

  /* Reconstruct the L-Halo merger pointers across the whole forest. */
  if (fix_flybys(totnhalos, halos, info, 0) != EXIT_SUCCESS) {
    FATAL_ERROR("fix_flybys failed for Consistent-Trees forest %d", unit);
  }
  const int max_snapnum = fix_upid(totnhalos, halos, info, 0);
  if (max_snapnum < 0) {
    FATAL_ERROR("fix_upid failed for Consistent-Trees forest %d", unit);
  }
  if (assign_mergertree_indices(totnhalos, halos, info, max_snapnum) != EXIT_SUCCESS) {
    FATAL_ERROR("assign_mergertree_indices failed for Consistent-Trees forest %d", unit);
  }
  if (validate_ctrees_snapshot_range(halos, totnhalos, "Consistent-Trees ASCII") != EXIT_SUCCESS) {
    FATAL_ERROR("Consistent-Trees forest %d has a snapshot outside the configured snapshot list",
                unit);
  }
  myfree(info);

  InputTreeNHalos[unit] = (int)totnhalos;
  InputTreeHalos =
      mymalloc_cat(sizeof(struct RawHalo) * (totnhalos > 0 ? totnhalos : 1), MEM_TREES);
  for (int64_t i = 0; i < totnhalos; i++) {
    bridge_halo_data_to_rawhalo(&InputTreeHalos[i], &halos[i]);
  }
  myfree(halos);
}

/** @brief Close this chunk partition: free staged scaffolding. */
static void close_partition_ctrees_ascii(void) { clear_staged_range_ctrees_ascii(); }

static void teardown_run_ctrees_ascii(void) { teardown_run_ctrees_ascii_state(); }

/* Consistent-Trees ASCII: forest-organised Rockstar output. One output
   partition per planned forest-count chunk, one unit per forest; topology is
   reconstructed from id/pid/upid. */
const struct TreeReader CTreesAsciiReader = {
    .name = "consistent_trees_ascii",
    .file_extension = "",
    .partition_model = PARTITION_ENUMERATED,
    .processing_order = INPUT_PROCESSING_ORDER_TREE,
    .prepare_run = prepare_run_ctrees_ascii,
    .teardown_run = teardown_run_ctrees_ascii,
    .num_partitions = num_partitions_ctrees_ascii,
    .partition_output_id = partition_output_id_ctrees_ascii,
    .partition_exists = partition_exists_ctrees_ascii,
    .format_partition_path = NULL,
    .count_partition_units = count_partition_units_ctrees_ascii,
    .global_forest_offset = global_forest_offset_ctrees_ascii,
    .partition_cost = partition_cost_ctrees_ascii,
    .open_partition = open_partition_ctrees_ascii,
    .load_unit = load_unit_ctrees_ascii,
    .close_partition = close_partition_ctrees_ascii,
};
