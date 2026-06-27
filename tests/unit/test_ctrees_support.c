/**
 * @file    test_ctrees_support.c
 * @brief   Unit tests for the Consistent-Trees support code and ASCII reader.
 *
 * These exercise the format-independent ctrees helpers and the reader-owned
 * value conventions in isolation (no simulation package or end-to-end run is
 * required). Coverage:
 *   - parse_header_ctrees: column-name matching + ascending column sort, with a
 *     requested-but-absent column correctly dropped (parse_ctrees.h).
 *   - read_forests / read_locations / assign_forest_ids /
 *     sort_locations_on_fid_file_offset on a tiny synthetic forests.list +
 *     locations.dat + tree file (ctrees_utils.c).
 *   - fix_flybys / fix_upid / assign_mergertree_indices reconstructing L-Halo
 *     merger pointers for a small hand-built forest (ctrees_utils.c).
 *   - forest distribution across MPI tasks, including the surplus-task and
 *     weighted-no-negative edges fixed when wiring the reader (forest_utils.c).
 *   - the ASCII reader's Consistent-Trees -> L-Halo conventions and the
 *     halo_data -> RawHalo bridge (read_ctrees_ascii.c).
 *   - the value conventions shared by both readers (spin/Len without touching the
 *     file-supplied id/pointers), used by the HDF5 reader (read_ctrees_common.h).
 *   - HDF5-specific validation is covered separately by test_ctrees_hdf5_reader
 *     when HDF5 development headers/libraries are available.
 *
 * @date    2026-06-18
 */

#include "../framework/test_framework.h"

#include "error.h"
#include "globals.h"
#include "memory.h"
#include "tree/ctrees/ctrees_compat.h"
#include "tree/ctrees/ctrees_utils.h"
#include "tree/ctrees/forest_utils.h"
#include "tree/ctrees/parse_ctrees.h"
#include "tree/read_ctrees_ascii.h"

#include <limits.h>
#include <math.h>

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0, failed = 0;

/* Return the array index whose info[].id matches `id`, or -1. After topology
   reconstruction forest[] and info[] are co-sorted, so this maps a stable halo
   identity back onto its post-sort slot. */
static int64_t index_of_id(const struct additional_info *info, int64_t n, int64_t id) {
  for (int64_t i = 0; i < n; i++) {
    if (info[i].id == id) {
      return i;
    }
  }
  return -1;
}

static int write_text_file(const char *path, const char *contents) {
  FILE *fp = fopen(path, "w");
  if (fp == NULL) {
    return -1;
  }
  fputs(contents, fp);
  fclose(fp);
  return 0;
}

/**
 * @test    test_parse_header_column_mapping
 * @brief   Maps four requested columns (plus one absent column that must be dropped)
 * onto a synthetic Consistent-Trees header and checks the resulting column
 * indices are exactly the matched positions in ascending order.
 */
int test_parse_header_column_mapping(void) {
  init_memory_system(0);

  char dir_template[] = "/tmp/mimic_ctrees_hdr_XXXXXX";
  char *dir = mkdtemp(dir_template);
  TEST_ASSERT(dir != NULL, "mkdtemp should create a temp directory");

  char header_path[512];
  snprintf(header_path, sizeof(header_path), "%s/tree_header.dat", dir);
  /* positions: scale=0 id=1 desc_scale=2 desc_id=3 pid=4 upid=5 Mvir=6 x=7 y=8 z=9 */
  TEST_ASSERT(write_text_file(header_path, "#scale id desc_scale desc_id pid upid Mvir x y z\n"
                                           "1.0 1 -1 -1 -1 -1 100.0 0 0 0\n") == 0,
              "should write synthetic header file");

  const int64_t nfields = 5;
  char wanted[5][PARSE_CTREES_MAX_COLNAME_LEN];
  memset(wanted, 0, sizeof(wanted));
  strcpy(wanted[0], "Mvir");         /* position 6 */
  strcpy(wanted[1], "scale");        /* position 0 */
  strcpy(wanted[2], "x");            /* position 7 */
  strcpy(wanted[3], "id");           /* position 1 */
  strcpy(wanted[4], "not_a_column"); /* absent -> dropped */

  enum parse_numeric_types field_types[5] = {F32, F64, F32, I64, I64};
  int64_t base_ptr_idx[5] = {0, 0, 0, 0, 0};
  size_t dest_offset[5] = {0, 0, 0, 0, 0};

  struct ctrees_column_to_ptr column_info;
  memset(&column_info, 0, sizeof(column_info));

  int rc = parse_header_ctrees(wanted, field_types, base_ptr_idx, dest_offset, nfields, header_path,
                               &column_info);
  TEST_ASSERT(rc == EXIT_SUCCESS, "parse_header_ctrees should succeed");
  TEST_ASSERT(column_info.ncols == 4, "exactly four of the five requested columns should match");

  /* matched positions, ascending: scale(0), id(1), Mvir(6), x(7) */
  TEST_ASSERT(column_info.column_number[0] == 0, "first mapped column should be scale at 0");
  TEST_ASSERT(column_info.column_number[1] == 1, "second mapped column should be id at 1");
  TEST_ASSERT(column_info.column_number[2] == 6, "third mapped column should be Mvir at 6");
  TEST_ASSERT(column_info.column_number[3] == 7, "fourth mapped column should be x at 7");

  unlink(header_path);
  rmdir(dir);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_read_forests_and_locations
 * @brief   Builds a one-file/one-tree forest index on disk and checks read_forests,
 * read_locations and assign_forest_ids agree on the forest id and tree layout.
 */
int test_read_forests_and_locations(void) {
  init_memory_system(0);

  char dir_template[] = "/tmp/mimic_ctrees_idx_XXXXXX";
  char *dir = mkdtemp(dir_template);
  TEST_ASSERT(dir != NULL, "mkdtemp should create a temp directory");

  char forests_path[512], locations_path[512], tree_path[512], locations_line[640];
  snprintf(forests_path, sizeof(forests_path), "%s/forests.list", dir);
  snprintf(locations_path, sizeof(locations_path), "%s/locations.dat", dir);
  snprintf(tree_path, sizeof(tree_path), "%s/tree_0_0_0.dat", dir);

  TEST_ASSERT(write_text_file(forests_path, "#TreeRootID ForestID\n10 100\n") == 0,
              "should write forests.list");
  /* read_locations parses: treeid fileid offset filename */
  snprintf(locations_line, sizeof(locations_line),
           "#TreeRootID FileID Offset Filename\n10 0 0 tree_0_0_0.dat\n");
  TEST_ASSERT(write_text_file(locations_path, locations_line) == 0, "should write locations.dat");
  TEST_ASSERT(write_text_file(tree_path, "#tree 10\n") == 0, "should write a tree data file");

  int64_t *forests = NULL;
  int64_t *tree_roots = NULL;
  int64_t ntrees = read_forests(forests_path, &forests, &tree_roots);
  TEST_ASSERT(ntrees == 1, "read_forests should report one tree");
  TEST_ASSERT(tree_roots[0] == 10, "tree root id should be 10");
  TEST_ASSERT(forests[0] == 100, "forest id should be 100");

  struct locations_with_forests *locations = mymalloc_cat(ntrees * sizeof(*locations), MEM_IO);
  struct filenames_and_fd files_fd;
  memset(&files_fd, 0, sizeof(files_fd));

  int64_t nloc = read_locations(locations_path, ntrees, locations, &files_fd);
  TEST_ASSERT(nloc == 1, "read_locations should report one tree");
  TEST_ASSERT(files_fd.numfiles == 1, "exactly one tree file should be opened");
  TEST_ASSERT(locations[0].treeid == 10, "location tree id should be 10");
  TEST_ASSERT(locations[0].fileid == 0, "location file id should be 0");
  TEST_ASSERT(locations[0].offset == 0, "location offset should be 0");

  int rc = assign_forest_ids(ntrees, locations, forests, tree_roots);
  TEST_ASSERT(rc == EXIT_SUCCESS, "assign_forest_ids should succeed");
  TEST_ASSERT(locations[0].forestid == 100, "assigned forest id should be 100");

  sort_locations_on_fid_file_offset(ntrees, locations);
  TEST_ASSERT(locations[0].forestid == 100, "sort should preserve the single forest id");

  /* read_locations opened the tree file; close the OS fd before freeing. */
  if (files_fd.fd[0] >= 0) {
    close(files_fd.fd[0]);
  }
  myfree(files_fd.fd);
  myfree(files_fd.numtrees_per_file);
  myfree(locations);
  myfree(forests);
  myfree(tree_roots);

  unlink(forests_path);
  unlink(locations_path);
  unlink(tree_path);
  rmdir(dir);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_forest_topology_reconstruction
 * @brief   Hand-builds a 3-halo forest (a z=0 central with one satellite and one earlier
 * progenitor of the central) and checks fix_upid + assign_mergertree_indices
 * reconstruct the FOF grouping and Descendant/FirstProgenitor pointers.
 */
int test_forest_topology_reconstruction(void) {
  init_memory_system(0);

  const int64_t n = 3;
  struct halo_data forest[3];
  struct additional_info info[3];
  memset(forest, 0, sizeof(forest));
  memset(info, 0, sizeof(info));

  /* sentinel-initialise the merger pointers as the reader would before topology */
  for (int64_t i = 0; i < n; i++) {
    forest[i].Descendant = -1;
    forest[i].FirstProgenitor = -1;
    forest[i].NextProgenitor = -1;
    forest[i].FirstHaloInFOFgroup = -1;
    forest[i].NextHaloInFOFgroup = -1;
  }

  /* root: z=0 central (snap 1, scale 1.0) */
  forest[0].SnapNum = 1;
  forest[0].Mvir = 200.0f;
  forest[0].MostBoundID = 1000;
  info[0] = (struct additional_info){
      .id = 2, .pid = -1, .upid = -1, .descid = -1, .desc_scale = -1.0, .scale = 1.0};

  /* sat: z=0 satellite of root (snap 1, scale 1.0) */
  forest[1].SnapNum = 1;
  forest[1].Mvir = 50.0f;
  forest[1].MostBoundID = 1001;
  info[1] = (struct additional_info){
      .id = 3, .pid = 2, .upid = 2, .descid = -1, .desc_scale = -1.0, .scale = 1.0};

  /* prog: progenitor of root (snap 0, scale 0.5) */
  forest[2].SnapNum = 0;
  forest[2].Mvir = 100.0f;
  forest[2].MostBoundID = 1002;
  info[2] = (struct additional_info){
      .id = 1, .pid = -1, .upid = -1, .descid = 2, .desc_scale = 1.0, .scale = 0.5};

  int fb = fix_flybys(n, forest, info, 0);
  TEST_ASSERT(fb == EXIT_SUCCESS, "fix_flybys should succeed (single FOF at max scale)");

  int max_snapnum = fix_upid(n, forest, info, 0);
  TEST_ASSERT(max_snapnum == 1, "fix_upid should return the max snapshot number (1)");

  int rc = assign_mergertree_indices(n, forest, info, max_snapnum);
  TEST_ASSERT(rc == EXIT_SUCCESS, "assign_mergertree_indices should succeed");

  int64_t i_root = index_of_id(info, n, 2);
  int64_t i_sat = index_of_id(info, n, 3);
  int64_t i_prog = index_of_id(info, n, 1);
  TEST_ASSERT(i_root >= 0 && i_sat >= 0 && i_prog >= 0, "all three halos should be present");

  /* FOF grouping: root is its own central, sat hangs off root, prog is its own central */
  TEST_ASSERT(forest[i_root].FirstHaloInFOFgroup == i_root, "root should be its own FOF central");
  TEST_ASSERT(forest[i_sat].FirstHaloInFOFgroup == i_root, "sat should point at the root central");
  TEST_ASSERT(forest[i_root].NextHaloInFOFgroup == i_sat, "root should link to the satellite");
  TEST_ASSERT(forest[i_prog].FirstHaloInFOFgroup == i_prog, "prog should be its own FOF central");

  /* Merger pointers: prog descends into root; root's first progenitor is prog. */
  TEST_ASSERT(forest[i_root].Descendant == -1, "the z=0 central has no descendant");
  TEST_ASSERT(forest[i_sat].Descendant == -1, "the satellite has no descendant in this forest");
  TEST_ASSERT(forest[i_prog].Descendant == i_root, "prog should descend into root");
  TEST_ASSERT(forest[i_root].FirstProgenitor == i_prog, "root's first progenitor should be prog");

  check_memory_leaks();
  return TEST_PASS;
}

/* Validate that N forest ranges form a contiguous, non-overlapping, complete
   cover of [0, totnforests). */
static int check_partition(int ntasks, const int64_t *starts, const int64_t *counts,
                           int64_t totnforests) {
  int64_t expected_start = 0;
  int64_t sum = 0;
  for (int t = 0; t < ntasks; t++) {
    if (starts[t] < 0 || starts[t] > totnforests) {
      return 0;
    }
    if (starts[t] != expected_start) {
      return 0;
    }
    if (counts[t] < 0) {
      return 0;
    }
    if (counts[t] > totnforests - starts[t]) {
      return 0;
    }
    expected_start += counts[t];
    sum += counts[t];
  }
  return sum == totnforests;
}

/**
 * @test    test_weighted_forest_distribution
 * @brief   Checks the weighted and uniform forest distributions partition a forest list
 * across MPI tasks into a contiguous, complete, non-overlapping cover.
 */
int test_weighted_forest_distribution(void) {
  init_memory_system(0);

  const int64_t totnforests = 6;
  const int64_t nhalos_per_forest[6] = {10, 10, 10, 10, 10, 10};
  const int ntasks = 3;

  int64_t starts[3], counts[3];
  for (int t = 0; t < ntasks; t++) {
    int rc = distribute_weighted_forests_over_ntasks(
        totnforests, nhalos_per_forest, linear_in_nhalos, 1.0, ntasks, t, &counts[t], &starts[t]);
    TEST_ASSERT(rc == EXIT_SUCCESS, "weighted distribution should succeed");
  }
  TEST_ASSERT(check_partition(ntasks, starts, counts, totnforests),
              "weighted partition should be a contiguous complete cover");

  /* Uniform scheme falls back to the equal-split path. */
  int64_t ustarts[3], ucounts[3];
  for (int t = 0; t < ntasks; t++) {
    int rc =
        distribute_weighted_forests_over_ntasks(totnforests, nhalos_per_forest, uniform_in_forests,
                                                0.0, ntasks, t, &ucounts[t], &ustarts[t]);
    TEST_ASSERT(rc == EXIT_SUCCESS, "uniform distribution should succeed");
  }
  TEST_ASSERT(check_partition(ntasks, ustarts, ucounts, totnforests),
              "uniform partition should be a contiguous complete cover");
  TEST_ASSERT(ucounts[0] == 2 && ucounts[1] == 2 && ucounts[2] == 2,
              "uniform split of 6 forests over 3 tasks should be 2 each");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_read_single_tree_rows
 * @brief   Drives the row-ingestion path: parse_header_ctrees builds a column map, then
 * read_single_tree_ctrees (via parse_line_ctrees) reads the data rows of a
 * synthetic tree file into structure-of-arrays destinations.
 */
int test_read_single_tree_rows(void) {
  init_memory_system(0);

  char dir_template[] = "/tmp/mimic_ctrees_rows_XXXXXX";
  char *dir = mkdtemp(dir_template);
  TEST_ASSERT(dir != NULL, "mkdtemp should create a temp directory");

  char tree_path[512];
  snprintf(tree_path, sizeof(tree_path), "%s/tree_0_0_0.dat", dir);
  const char *header = "#scale id desc_scale desc_id pid upid Mvir x y z\n";
  const char *rows = "1.0 1 -1 -1 -1 -1 100.0 0.5 1.5 2.5\n"
                     "0.5 2 1.0 1 -1 -1 50.0 0.6 1.6 2.6\n";
  char contents[1024];
  snprintf(contents, sizeof(contents), "%s%s", header, rows);
  TEST_ASSERT(write_text_file(tree_path, contents) == 0, "should write synthetic tree file");

  /* Request scale (F64), id (I64) and Mvir (F32); each lands in its own array. */
  const int64_t nfields = 3;
  char wanted[3][PARSE_CTREES_MAX_COLNAME_LEN];
  memset(wanted, 0, sizeof(wanted));
  strcpy(wanted[0], "scale"); /* base ptr 0, column 0 */
  strcpy(wanted[1], "id");    /* base ptr 1, column 1 */
  strcpy(wanted[2], "Mvir");  /* base ptr 2, column 6 */
  enum parse_numeric_types field_types[3] = {F64, I64, F32};
  int64_t base_ptr_idx[3] = {0, 1, 2};
  size_t dest_offset[3] = {0, 0, 0};

  struct ctrees_column_to_ptr column_info;
  memset(&column_info, 0, sizeof(column_info));
  int rc = parse_header_ctrees(wanted, field_types, base_ptr_idx, dest_offset, nfields, tree_path,
                               &column_info);
  TEST_ASSERT(rc == EXIT_SUCCESS, "parse_header_ctrees should succeed");
  TEST_ASSERT(column_info.ncols == 3, "all three requested columns should match");

  /* Pre-allocate one element per destination array (the reader seeds capacity
     before reading; parse_line_ctrees grows from there). */
  double *scale_arr = mymalloc_cat(sizeof(double), MEM_TREES);
  int64_t *id_arr = mymalloc_cat(sizeof(int64_t), MEM_TREES);
  float *mvir_arr = mymalloc_cat(sizeof(float), MEM_TREES);

  struct base_ptr_info base;
  memset(&base, 0, sizeof(base));
  base.num_base_ptrs = 3;
  base.base_ptrs[0] = (void **)&scale_arr;
  base.base_ptrs[1] = (void **)&id_arr;
  base.base_ptrs[2] = (void **)&mvir_arr;
  base.base_element_size[0] = sizeof(double);
  base.base_element_size[1] = sizeof(int64_t);
  base.base_element_size[2] = sizeof(float);
  base.N = 0;
  base.nallocated = 1;

  int fd = open(tree_path, O_RDONLY);
  TEST_ASSERT(fd >= 0, "should open the tree file");
  /* Data rows begin immediately after the header line. */
  off_t data_offset = (off_t)strlen(header);
  rc = read_single_tree_ctrees(fd, data_offset, &column_info, &base);
  close(fd);
  TEST_ASSERT(rc == EXIT_SUCCESS, "read_single_tree_ctrees should succeed");
  TEST_ASSERT(base.N == 2, "two data rows should be parsed");

  TEST_ASSERT(scale_arr[0] == 1.0 && scale_arr[1] == 0.5, "scale column should round-trip");
  TEST_ASSERT(id_arr[0] == 1 && id_arr[1] == 2, "id column should round-trip");
  TEST_ASSERT(mvir_arr[0] == 100.0f && mvir_arr[1] == 50.0f, "Mvir column should round-trip");

  myfree(scale_arr);
  myfree(id_arr);
  myfree(mvir_arr);
  unlink(tree_path);
  rmdir(dir);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_parse_rejects_malformed_numeric_tokens
 * @brief   Verifies malformed ASCII numbers fail at parse time instead of silently
 * becoming zero, clamped values, or truncated integers.
 */
int test_parse_rejects_malformed_numeric_tokens(void) {
  init_memory_system(0);

  double *scale_arr = mymalloc_cat(sizeof(double), MEM_TREES);
  int64_t *id_arr = mymalloc_cat(sizeof(int64_t), MEM_TREES);
  int32_t *snap_arr = mymalloc_cat(sizeof(int32_t), MEM_TREES);

  struct base_ptr_info base;
  memset(&base, 0, sizeof(base));
  base.num_base_ptrs = 3;
  base.base_ptrs[0] = (void **)&scale_arr;
  base.base_ptrs[1] = (void **)&id_arr;
  base.base_ptrs[2] = (void **)&snap_arr;
  base.base_element_size[0] = sizeof(double);
  base.base_element_size[1] = sizeof(int64_t);
  base.base_element_size[2] = sizeof(int32_t);
  base.nallocated = 1;

  struct ctrees_column_to_ptr column_info;
  memset(&column_info, 0, sizeof(column_info));
  column_info.ncols = 3;
  column_info.column_number[0] = 0;
  column_info.field_types[0] = F64;
  column_info.base_ptr_idx[0] = 0;
  column_info.column_number[1] = 1;
  column_info.field_types[1] = I64;
  column_info.base_ptr_idx[1] = 1;
  column_info.column_number[2] = 2;
  column_info.field_types[2] = I32;
  column_info.base_ptr_idx[2] = 2;

  base.N = 0;
  TEST_ASSERT(parse_line_ctrees("abc 1 2", &column_info, &base) != EXIT_SUCCESS,
              "non-numeric float token must fail");
  TEST_ASSERT(base.N == 0, "failed parse must not advance row count");

  base.N = 0;
  TEST_ASSERT(parse_line_ctrees("1.0 2.5 3", &column_info, &base) != EXIT_SUCCESS,
              "fractional integer token must fail");
  TEST_ASSERT(base.N == 0, "failed integer parse must not advance row count");

  base.N = 0;
  TEST_ASSERT(parse_line_ctrees("1.0 2 2147483648", &column_info, &base) != EXIT_SUCCESS,
              "int32 overflow must fail");
  TEST_ASSERT(base.N == 0, "failed overflow parse must not advance row count");

  base.N = 0;
  TEST_ASSERT(parse_line_ctrees("1.0 2 3", &column_info, &base) == EXIT_SUCCESS,
              "valid strict numeric row should pass");
  TEST_ASSERT(base.N == 1, "successful parse should advance row count");

  myfree(scale_arr);
  myfree(id_arr);
  myfree(snap_arr);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_find_start_and_end_filenum
 * @brief   Maps a contiguous forest range onto the input files that contain it.
 */
int test_find_start_and_end_filenum(void) {
  init_memory_system(0);

  /* file0: forests {0,1}, file1: {2,3,4}, file2: {5,6}; total 7. */
  const int64_t totnforests_per_file[3] = {2, 3, 2};
  const int64_t totnforests = 7;
  const int firstfile = 0, lastfile = 2;
  const int64_t start_forestnum = 1, end_forestnum = 5; /* process forests [1,5) */

  int64_t num_per_file[3] = {0, 0, 0};
  int64_t start_per_file[3] = {0, 0, 0};
  int start_file = -1, end_file = -1;

  int rc = find_start_and_end_filenum(start_forestnum, end_forestnum, totnforests_per_file,
                                      totnforests, firstfile, lastfile, /*ThisTask=*/0,
                                      /*NTasks=*/1, num_per_file, start_per_file, &start_file,
                                      &end_file);
  TEST_ASSERT(rc == EXIT_SUCCESS, "find_start_and_end_filenum should succeed");
  TEST_ASSERT(start_file == 0, "range should start in file 0");
  TEST_ASSERT(end_file == 1, "range should end in file 1");
  TEST_ASSERT(start_per_file[0] == 1, "file 0 processing should start at its second forest");
  TEST_ASSERT(num_per_file[0] == 1, "file 0 should contribute one forest");
  TEST_ASSERT(num_per_file[1] == 3, "file 1 should contribute three forests");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_distribute_forests_surplus_tasks
 * @brief   When there are more tasks than forests the uniform splitter must still produce
 * a contiguous complete cover, handing the surplus high-rank tasks empty ranges
 * (never a negative count), and must reject an out-of-range task id. Exercises
 * the ThisTask >= NTasks guard fix.
 */
int test_distribute_forests_surplus_tasks(void) {
  init_memory_system(0);

  const int64_t totnforests = 2;
  const int ntasks = 4;
  int64_t starts[4], counts[4];
  for (int t = 0; t < ntasks; t++) {
    int rc = distribute_forests_over_ntasks(totnforests, ntasks, t, &counts[t], &starts[t]);
    TEST_ASSERT(rc == EXIT_SUCCESS, "uniform distribution should succeed for every valid task");
  }
  TEST_ASSERT(check_partition(ntasks, starts, counts, totnforests),
              "surplus-task partition should be a contiguous complete cover");
  TEST_ASSERT(counts[0] == 1 && counts[1] == 1 && counts[2] == 0 && counts[3] == 0,
              "two forests over four tasks should give {1,1,0,0}");
  TEST_ASSERT(starts[0] == 0 && starts[1] == 1 && starts[2] == 2 && starts[3] == 2,
              "surplus-task starts should be global contiguous offsets");

  /* ThisTask == NTasks is out of the [0, NTasks) range and must be rejected. */
  int64_t bad_count = -99, bad_start = -99;
  int rc = distribute_forests_over_ntasks(totnforests, ntasks, ntasks, &bad_count, &bad_start);
  TEST_ASSERT(rc == EXIT_FAILURE, "task id equal to NTasks must be rejected");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_weighted_distribution_no_negative
 * @brief   With the cost concentrated in one forest and more tasks than the weighting can
 * fill, the trailing task(s) must receive an empty (zero) range rather than the
 * sentinel -1 count. Exercises the weighted-path -1 clamp.
 */
int test_weighted_distribution_no_negative(void) {
  init_memory_system(0);

  const int64_t totnforests = 2;
  const int64_t nhalos_per_forest[2] = {1000, 1}; /* cost concentrated in forest 0 */
  const int ntasks = 3;

  int64_t starts[3], counts[3];
  for (int t = 0; t < ntasks; t++) {
    int rc =
        distribute_weighted_forests_over_ntasks(totnforests, nhalos_per_forest, quadratic_in_nhalos,
                                                2.0, ntasks, t, &counts[t], &starts[t]);
    TEST_ASSERT(rc == EXIT_SUCCESS, "weighted distribution should succeed for every valid task");
    TEST_ASSERT(counts[t] >= 0, "no task may receive a negative forest count");
  }
  TEST_ASSERT(check_partition(ntasks, starts, counts, totnforests),
              "weighted surplus partition should be a contiguous complete cover");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_forest_cost_quadratic_uses_double_before_multiply
 * @brief   Quadratic cost promotes to double before multiply to avoid int64 overflow
 */
int test_forest_cost_quadratic_uses_double_before_multiply(void) {
  const int64_t nhalos = 4000000000LL;
  const double cost = compute_forest_cost_from_nhalos(quadratic_in_nhalos, nhalos, 0.0);

  TEST_ASSERT(isfinite(cost), "quadratic forest cost should stay finite for large nhalos");
  TEST_ASSERT(cost == (double)nhalos * (double)nhalos,
              "quadratic forest cost should multiply in double precision");
  TEST_ASSERT(cost > (double)LLONG_MAX, "test should exercise the int64 overflow regime");

  return TEST_PASS;
}

/**
 * @test    test_convert_ctrees_conventions
 * @brief   Pins the reader-owned Consistent-Trees -> L-Halo conventions: spin normalised
 * by the native Mvir, Len derived from native Mvir and the particle mass, the id
 * carried into MostBoundID, and the merger pointers sentinel-initialised.
 */
int test_convert_ctrees_conventions(void) {
  init_memory_system(0);

  MimicConfig.PartMass = 0.1; /* 1e10 Msun/h per particle -> clean Len arithmetic */

  struct halo_data h;
  memset(&h, 0, sizeof(h));
  h.Mvir = 1.0e12f; /* native Msun/h */
  h.Spin[0] = 1.0e12f;
  h.Spin[1] = 5.0e11f;
  h.Spin[2] = 0.0f;

  struct additional_info info;
  memset(&info, 0, sizeof(info));
  info.id = 42;

  convert_ctrees_to_lht(&h, &info, 1);

  TEST_ASSERT(fabsf(h.Spin[0] - 1.0f) <= 1.0e-4f, "Spin_x should be normalised to J/Mvir = 1.0");
  TEST_ASSERT(fabsf(h.Spin[1] - 0.5f) <= 1.0e-4f, "Spin_y should be normalised to J/Mvir = 0.5");
  TEST_ASSERT(fabsf(h.Spin[2] - 0.0f) <= 1.0e-4f, "Spin_z should be normalised to 0.0");

  /* Len = round(Mvir * 1e-10 / PartMass) = round(1e12 * 1e-10 / 0.1) = 1000. */
  TEST_ASSERT(h.Len >= 999 && h.Len <= 1001, "Len should be ~1000 particles");
  TEST_ASSERT(h.MostBoundID == 42, "MostBoundID should carry the ctrees id");
  TEST_ASSERT(h.Descendant == -1 && h.FirstProgenitor == -1 && h.NextProgenitor == -1,
              "merger pointers should be sentinel-initialised");
  TEST_ASSERT(h.FirstHaloInFOFgroup == -1 && h.NextHaloInFOFgroup == -1,
              "FOF pointers should be sentinel-initialised");

  /* Mvir itself is left NATIVE (scaling is the accessor's job, not the reader's). */
  TEST_ASSERT(fabsf(h.Mvir - 1.0e12f) <= 1.0e6f,
              "Mvir must remain native (un-scaled) in the reader");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_apply_value_conventions_shared
 * @brief   Pins the conventions shared by both ctrees readers (read_ctrees_common.h):
 * spin normalised by the native Mvir and Len from native Mvir / particle mass.
 * Unlike convert_ctrees_to_lht (ASCII), it must NOT touch MostBoundID or the
 * merger pointers — the HDF5 reader supplies those from the file, so this helper
 * has to leave them untouched.
 */
int test_apply_value_conventions_shared(void) {
  init_memory_system(0);

  MimicConfig.PartMass = 0.1; /* 1e10 Msun/h per particle -> clean Len arithmetic */

  struct halo_data h;
  memset(&h, 0, sizeof(h));
  h.Mvir = 1.0e12f; /* native Msun/h */
  h.Spin[0] = 1.0e12f;
  h.Spin[1] = 5.0e11f;
  h.Spin[2] = 0.0f;
  /* Fields the HDF5 path fills from file; the shared helper must leave them. */
  h.MostBoundID = 555;
  h.Descendant = 9;
  h.FirstProgenitor = 8;
  h.NextHaloInFOFgroup = 7;

  apply_ctrees_value_conventions(&h, 1);

  TEST_ASSERT(fabsf(h.Spin[0] - 1.0f) <= 1.0e-4f, "Spin_x should be normalised to J/Mvir = 1.0");
  TEST_ASSERT(fabsf(h.Spin[1] - 0.5f) <= 1.0e-4f, "Spin_y should be normalised to J/Mvir = 0.5");
  TEST_ASSERT(fabsf(h.Spin[2] - 0.0f) <= 1.0e-4f, "Spin_z should be normalised to 0.0");
  TEST_ASSERT(h.Len >= 999 && h.Len <= 1001, "Len should be ~1000 particles");

  /* The shared helper must not overwrite the file-supplied id or pointers. */
  TEST_ASSERT(h.MostBoundID == 555, "shared conventions must not touch MostBoundID");
  TEST_ASSERT(h.Descendant == 9 && h.FirstProgenitor == 8 && h.NextHaloInFOFgroup == 7,
              "shared conventions must not touch the merger pointers");

  /* Mvir itself is left NATIVE (scaling is the accessor's job, not the reader's). */
  TEST_ASSERT(fabsf(h.Mvir - 1.0e12f) <= 1.0e6f,
              "Mvir must remain native (un-scaled) in the reader");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_bridge_to_rawhalo
 * @brief   Checks the halo_data -> RawHalo bridge copies every contracted field, including
 * the native Mvir into the HaloMass-providing M_Crit200 slot.
 */
int test_bridge_to_rawhalo(void) {
  init_memory_system(0);

  struct halo_data h;
  memset(&h, 0, sizeof(h));
  h.Descendant = 3;
  h.FirstProgenitor = 4;
  h.NextProgenitor = 5;
  h.FirstHaloInFOFgroup = 6;
  h.NextHaloInFOFgroup = 7;
  h.Len = 1234;
  h.Mvir = 8.5e11f;
  h.Pos[0] = 1.0f;
  h.Pos[1] = 2.0f;
  h.Pos[2] = 3.0f;
  h.Vel[0] = -10.0f;
  h.Vel[1] = 20.0f;
  h.Vel[2] = -30.0f;
  h.Spin[0] = 0.01f;
  h.Spin[1] = 0.02f;
  h.Spin[2] = 0.03f;
  h.VelDisp = 111.0f;
  h.Vmax = 222.0f;
  h.MostBoundID = 99887766;
  h.SnapNum = 17;

  struct RawHalo out;
  memset(&out, 0, sizeof(out));
  bridge_halo_data_to_rawhalo(&out, &h);

  TEST_ASSERT(out.Descendant == 3 && out.FirstProgenitor == 4 && out.NextProgenitor == 5,
              "merger pointers should bridge through");
  TEST_ASSERT(out.FirstHaloInFOFgroup == 6 && out.NextHaloInFOFgroup == 7,
              "FOF pointers should bridge through");
  TEST_ASSERT(out.Len == 1234, "Len should bridge through");
  TEST_ASSERT(out.M_Crit200 == 8.5e11f, "native Mvir should land in the M_Crit200 (HaloMass) slot");
  TEST_ASSERT(out.Pos[0] == 1.0f && out.Pos[1] == 2.0f && out.Pos[2] == 3.0f,
              "position should bridge through");
  TEST_ASSERT(out.Vel[0] == -10.0f && out.Vel[1] == 20.0f && out.Vel[2] == -30.0f,
              "velocity should bridge through");
  TEST_ASSERT(out.Spin[0] == 0.01f && out.Spin[1] == 0.02f && out.Spin[2] == 0.03f,
              "spin should bridge through");
  TEST_ASSERT(out.VelDisp == 111.0f && out.Vmax == 222.0f, "VelDisp/Vmax should bridge through");
  TEST_ASSERT(out.MostBoundID == 99887766, "MostBoundID should bridge through");
  TEST_ASSERT(out.SnapNum == 17, "SnapNum should bridge through");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_sort_locations_offset_tie
 * @brief   Sorts locations whose (forestid, fileid, offset) keys are equal in pairs. The
 * comparator fix returns 0 on an offset tie (a valid strict-weak ordering), so
 * the sort must produce a valid permutation (every treeid present exactly once)
 * with the forests correctly grouped and ordered.
 *
 * Note: two of the deliberate ctrees-helper fixes are NOT unit-tested here and
 * are instead validated end-to-end (see docs/dev/CTREES-UCHUU-VALIDATION.md):
 *   - read_locations' file-array realloc boundary (fileid >= nallocated) needs a
 *     perfect-cube file count above 2000 (13^3 = 2197 files) to trigger;
 *   - the open()-returns-fd-0 acceptance (`>= 0`) is effectively unreachable in
 *     read_locations because its own fopen() of locations.dat claims fd 0 first
 *     whenever stdin is closed, so the tree open() never receives fd 0.
 */
int test_sort_locations_offset_tie(void) {
  init_memory_system(0);

  const int64_t n = 4;
  struct locations_with_forests loc[4];
  memset(loc, 0, sizeof(loc));
  /* Two forests, each with two trees tied on (fileid, offset). */
  loc[0] = (struct locations_with_forests){.forestid = 5, .treeid = 100, .fileid = 0, .offset = 0};
  loc[1] = (struct locations_with_forests){.forestid = 5, .treeid = 101, .fileid = 0, .offset = 0};
  loc[2] = (struct locations_with_forests){.forestid = 2, .treeid = 102, .fileid = 1, .offset = 8};
  loc[3] = (struct locations_with_forests){.forestid = 2, .treeid = 103, .fileid = 1, .offset = 8};

  sort_locations_on_fid_file_offset(n, loc);

  /* Forest 2 sorts before forest 5; forests stay grouped. */
  TEST_ASSERT(loc[0].forestid == 2 && loc[1].forestid == 2, "forest 2 should sort first");
  TEST_ASSERT(loc[2].forestid == 5 && loc[3].forestid == 5, "forest 5 should sort second");

  /* No element lost or duplicated by the equal-key path. */
  int seen[4] = {0, 0, 0, 0};
  for (int64_t i = 0; i < n; i++) {
    const int t = (int)loc[i].treeid - 100;
    TEST_ASSERT(t >= 0 && t < 4, "treeids should be preserved by the sort");
    seen[t]++;
  }
  TEST_ASSERT(seen[0] == 1 && seen[1] == 1 && seen[2] == 1 && seen[3] == 1,
              "every treeid should appear exactly once after the tie-key sort");

  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Consistent-Trees Support Code\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_parse_header_column_mapping);
  TEST_RUN(test_read_forests_and_locations);
  TEST_RUN(test_read_single_tree_rows);
  TEST_RUN(test_parse_rejects_malformed_numeric_tokens);
  TEST_RUN(test_forest_topology_reconstruction);
  TEST_RUN(test_find_start_and_end_filenum);
  TEST_RUN(test_weighted_forest_distribution);
  TEST_RUN(test_distribute_forests_surplus_tasks);
  TEST_RUN(test_weighted_distribution_no_negative);
  TEST_RUN(test_forest_cost_quadratic_uses_double_before_multiply);
  TEST_RUN(test_convert_ctrees_conventions);
  TEST_RUN(test_apply_value_conventions_shared);
  TEST_RUN(test_bridge_to_rawhalo);
  TEST_RUN(test_sort_locations_offset_tie);

  TEST_SUMMARY();
  return TEST_RESULT();
}
