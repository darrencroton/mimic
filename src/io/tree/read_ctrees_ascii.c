/**
 * @file    tree/read_ctrees_ascii.c
 * @brief   Consistent-Trees ASCII merger-tree reader.
 *
 * Reads Rockstar/Consistent-Trees ASCII output (`forests.list` + `locations.dat`
 * + `tree_i_j_k.dat`) and presents it to the core as the partition/unit model:
 * one partition per MPI task, one unit per forest. The heavy lifting lives in
 * the vendored, format-independent helpers under tree/ctrees: this file is the
 * Mimic seam that drives them, bridges the reconstructed `struct halo_data`
 * forest into the generated per-simulation `struct RawHalo`, and owns the
 * per-partition file descriptors.
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
 * The forests are split across MPI tasks uniformly by forest count. ASCII does
 * not expose per-forest halo counts before loading, so the weighted distribution
 * (used by the HDF5 reader) is not available here.
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
#include "tree/read_ctrees_ascii.h"
#include "tree/read_ctrees_common.h"
#include "tree/reader.h"

/* The one open partition (this task's forest chunk). One reader instance per
   process, so a file-static record is sufficient and mirrors the L-Halo readers'
   single open file handle. */
struct ctrees_ascii_partition {
  int64_t nforests;                         /* units in this partition (== global Ntrees) */
  int64_t ntrees_this_task;                 /* total trees across this task's forests */
  int64_t *ntrees_per_forest;               /* [nforests] number of trees in each forest */
  int64_t *start_treenum_per_forest;        /* [nforests] first tree index of each forest */
  off_t *tree_offsets;                      /* [ntrees_this_task] byte offset of each tree */
  int *tree_fd;                             /* [ntrees_this_task] fd for each tree (borrowed) */
  int numfiles;                             /* number of distinct tree_i_j_k.dat files */
  int *open_fds;                            /* [numfiles] owned descriptors, closed on close */
  struct ctrees_column_to_ptr *column_info; /* ctrees column -> struct field mapping */
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

/**
 * @brief   Open this task's partition: split forests and stage their file offsets.
 *
 * @param   output_id   The MPI task id (the per-task output partition id).
 *
 * Reads the forest/location index files, assigns forest ids, sorts the trees by
 * (forest, file, offset), splits the forests uniformly across tasks, and records
 * the (fd, offset) of every tree this task must read. Sets the global Ntrees to
 * this task's forest count so the core driver iterates forests as units.
 */
static void open_partition_ctrees_ascii(int output_id) {
  (void)output_id; /* equals ThisTask; the split below reads ThisTask/NTask */
  const int thistask = ThisTask;
  const int ntasks = (NTask > 0) ? NTask : 1; /* serial builds leave NTask == 0 */

  /* The unique-galaxy-id task term is FILENR_MUL_FAC*ThisTask; guard the int64
     overflow before any work. */
  if ((long long)thistask > CTREES_MAX_TASK_ID) {
    FATAL_ERROR("MPI task id %d exceeds the unique-galaxy-id task limit of %lld", thistask,
                (long long)CTREES_MAX_TASK_ID);
  }

  /* One descriptor per tree file: raise the soft open-file limit to the hard
     limit (a documented scaling constraint of the ctrees ASCII layout). */
  struct rlimit rlp;
  if (getrlimit(RLIMIT_NOFILE, &rlp) == 0) {
    rlp.rlim_cur = rlp.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rlp);
  }

  char locations_file[2 * MAX_STRING_LEN + 1], forests_file[2 * MAX_STRING_LEN + 1];
  snprintf(locations_file, sizeof(locations_file), "%s/locations.dat", MimicConfig.SimulationDir);
  snprintf(forests_file, sizeof(forests_file), "%s/forests.list", MimicConfig.SimulationDir);

  int64_t *treeids = NULL, *forestids = NULL;
  const int64_t totntrees = read_forests(forests_file, &forestids, &treeids);
  if (totntrees < 0) {
    FATAL_ERROR("Failed to read Consistent-Trees forests file '%s' (status %" PRId64 ")",
                forests_file, totntrees);
  }

  struct locations_with_forests *locations = mymalloc_cat(totntrees * sizeof(*locations), MEM_IO);
  memset(locations, 0, totntrees * sizeof(*locations));

  struct filenames_and_fd files_fd;
  const int64_t nread = read_locations(locations_file, totntrees, locations, &files_fd);
  if (nread != totntrees) {
    FATAL_ERROR("Consistent-Trees locations '%s' lists %" PRId64 " trees but forests '%s' lists "
                "%" PRId64,
                locations_file, nread, forests_file, totntrees);
  }

  if (assign_forest_ids(totntrees, locations, forestids, treeids) != EXIT_SUCCESS) {
    FATAL_ERROR("Failed to assign Consistent-Trees forest ids");
  }
  myfree(treeids);
  myfree(forestids);

  /* Group trees by (forestid, fileid, offset). */
  sort_locations_on_fid_file_offset(totntrees, locations);

  int64_t totnforests = 0, prev_forestid = -1;
  for (int64_t i = 0; i < totntrees; i++) {
    if (locations[i].forestid != prev_forestid) {
      totnforests++;
      prev_forestid = locations[i].forestid;
    }
  }
  if (totnforests >= INT_MAX) {
    FATAL_ERROR("Consistent-Trees forest count %" PRId64 " cannot be indexed by a 32-bit int",
                totnforests);
  }

  /* Uniform forest-count split across tasks. */
  int64_t nforests_this_task = 0, start_forestnum = 0;
  if (distribute_forests_over_ntasks(totnforests, ntasks, thistask, &nforests_this_task,
                                     &start_forestnum) != EXIT_SUCCESS) {
    FATAL_ERROR("Failed to distribute %" PRId64 " Consistent-Trees forests across %d tasks",
                totnforests, ntasks);
  }
  if (nforests_this_task >= CTREES_MAX_FORESTS_PER_TASK) {
    FATAL_ERROR("Task %d was assigned %" PRId64 " forests, at or above the unique-galaxy-id limit "
                "of %lld; run with more MPI tasks",
                thistask, nforests_this_task, (long long)CTREES_MAX_FORESTS_PER_TASK);
  }
  const int64_t end_forestnum = start_forestnum + nforests_this_task;

  /* Locate this task's contiguous tree range [start_treenum, start+ntrees). */
  int64_t ntrees_this_task = 0, start_treenum = (nforests_this_task > 0) ? -1 : 0, iforest = -1;
  prev_forestid = -1;
  for (int64_t i = 0; i < totntrees; i++) {
    if (locations[i].forestid != prev_forestid) {
      iforest++;
      prev_forestid = locations[i].forestid;
    }
    if (iforest < start_forestnum)
      continue;
    if (iforest == start_forestnum && start_treenum < 0)
      start_treenum = i;
    if (iforest >= end_forestnum)
      break;
    ntrees_this_task++;
  }

  /* Stage the per-partition record (Ntrees == this task's forest count). */
  Ntrees = (int)nforests_this_task;
  CT.nforests = nforests_this_task;
  CT.ntrees_this_task = ntrees_this_task;
  CT.ntrees_per_forest = mymalloc_cat(nforests_this_task * sizeof(*CT.ntrees_per_forest), MEM_IO);
  CT.start_treenum_per_forest =
      mymalloc_cat(nforests_this_task * sizeof(*CT.start_treenum_per_forest), MEM_IO);
  CT.tree_offsets = mymalloc_cat(ntrees_this_task * sizeof(*CT.tree_offsets), MEM_IO);
  CT.tree_fd = mymalloc_cat(ntrees_this_task * sizeof(*CT.tree_fd), MEM_IO);

  InputTreeNHalos = mymalloc_cat(nforests_this_task * sizeof(int), MEM_TREES);
  InputTreeFirstHalo = mymalloc_cat(nforests_this_task * sizeof(int), MEM_TREES);
  for (int64_t i = 0; i < nforests_this_task; i++) {
    InputTreeNHalos[i] = 0;    /* filled per forest in load_unit */
    InputTreeFirstHalo[i] = 0; /* each forest loads into a fresh InputTreeHalos */
  }

  /* Map each tree to its forest, fd and byte offset. */
  if (nforests_this_task > 0) {
    iforest = -1;
    prev_forestid = -1;
    int first_tree = 0;
    const int64_t end_treenum = start_treenum + ntrees_this_task;
    for (int64_t i = start_treenum; i < end_treenum; i++) {
      if (locations[i].forestid != prev_forestid) {
        iforest++;
        prev_forestid = locations[i].forestid;
        first_tree = 1;
      }
      const int64_t treeindex = i - start_treenum;
      if (first_tree == 1) {
        CT.ntrees_per_forest[iforest] = 1;
        CT.start_treenum_per_forest[iforest] = treeindex;
        first_tree = 0;
      } else {
        CT.ntrees_per_forest[iforest]++;
      }
      const int64_t fileid = locations[i].fileid;
      CT.tree_fd[treeindex] = files_fd.fd[fileid];
      CT.tree_offsets[treeindex] = (off_t)locations[i].offset;
    }
    if (iforest != nforests_this_task - 1) {
      FATAL_ERROR("Consistent-Trees forest bookkeeping mismatch: recovered %" PRId64
                  " forests, expected %" PRId64,
                  iforest + 1, nforests_this_task);
    }
  }

  /* Take ownership of the open descriptors; the fd-array itself is no longer
     needed (CT.tree_fd already holds the per-tree descriptors). */
  CT.numfiles = files_fd.numfiles;
  CT.open_fds = mymalloc_cat(files_fd.numfiles * sizeof(int), MEM_IO);
  for (int i = 0; i < files_fd.numfiles; i++) {
    CT.open_fds[i] = files_fd.fd[i];
  }
  myfree(files_fd.numtrees_per_file);
  myfree(files_fd.fd);
  myfree(locations);

  /* Parse the column header once per partition (only when there is data). */
  if (nforests_this_task > 0) {
    setup_column_info();
  } else {
    CT.column_info = NULL;
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

/** @brief Close this task's partition: close descriptors, free scaffolding. */
static void close_partition_ctrees_ascii(void) {
  for (int i = 0; i < CT.numfiles; i++) {
    if (CT.open_fds[i] >= 0) {
      close(CT.open_fds[i]);
    }
  }
  myfree(CT.open_fds);
  myfree(CT.column_info);
  myfree(CT.tree_fd);
  myfree(CT.tree_offsets);
  myfree(CT.start_treenum_per_forest);
  myfree(CT.ntrees_per_forest);
  memset(&CT, 0, sizeof(CT));
}

/* Consistent-Trees ASCII: forest-organised Rockstar output. One partition per
   MPI task, one unit per forest; topology reconstructed from id/pid/upid. See
   tree/registry.c. num_partitions/partition_output_id are unused for
   PARTITION_PER_TASK readers (the driver derives the output id from ThisTask). */
const struct TreeReader CTreesAsciiReader = {
    .name = "consistent_trees_ascii",
    .file_extension = "",
    .partition_model = PARTITION_PER_TASK,
    .processing_order = INPUT_PROCESSING_ORDER_TREE,
    .num_partitions = NULL,
    .partition_output_id = NULL,
    .format_partition_path = NULL,
    .count_partition_trees = NULL,
    .open_partition = open_partition_ctrees_ascii,
    .load_unit = load_unit_ctrees_ascii,
    .close_partition = close_partition_ctrees_ascii,
};
