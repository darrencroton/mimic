/**
 * @file    test_ctrees_support.c
 * @brief   Unit tests for the vendored Consistent-Trees support code.
 *
 * These exercise the format-independent ctrees helpers in isolation so the
 * vendored code is not dead while it is still unwired by the tree driver
 * (Phase 4). Coverage:
 *   - parse_header_ctrees: column-name matching + ascending column sort, with a
 *     requested-but-absent column correctly dropped (parse_ctrees.h).
 *   - read_forests / read_locations / assign_forest_ids /
 *     sort_locations_on_fid_file_offset on a tiny synthetic forests.list +
 *     locations.dat + tree file (ctrees_utils.c).
 *   - fix_flybys / fix_upid / assign_mergertree_indices reconstructing L-Halo
 *     merger pointers for a small hand-built forest (ctrees_utils.c).
 *   - distribute_weighted_forests_over_ntasks + uniform fallback partitioning a
 *     forest list across MPI tasks (forest_utils.c).
 *
 * @date    2026-06-17
 */

#include "../framework/test_framework.h"

#include "error.h"
#include "memory.h"
#include "tree/ctrees/ctrees_compat.h"
#include "tree/ctrees/ctrees_utils.h"
#include "tree/ctrees/forest_utils.h"
#include "tree/ctrees/parse_ctrees.h"

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
 * @test  test_parse_header_column_mapping
 * Maps four requested columns (plus one absent column that must be dropped)
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
 * @test  test_read_forests_and_locations
 * Builds a one-file/one-tree forest index on disk and checks read_forests,
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
 * @test  test_forest_topology_reconstruction
 * Hand-builds a 3-halo forest (a z=0 central with one satellite and one earlier
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

/* Validate that a per-task partition is a contiguous, non-overlapping, complete
   cover of [0, totnforests). */
static int check_partition(int ntasks, const int64_t *starts, const int64_t *counts,
                           int64_t totnforests) {
  int64_t expected_start = 0;
  int64_t sum = 0;
  for (int t = 0; t < ntasks; t++) {
    if (starts[t] != expected_start) {
      return 0;
    }
    if (counts[t] < 0) {
      return 0;
    }
    expected_start += counts[t];
    sum += counts[t];
  }
  return sum == totnforests;
}

/**
 * @test  test_weighted_forest_distribution
 * Checks the weighted and uniform forest distributions partition a forest list
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
 * @test  test_read_single_tree_rows
 * Drives the row-ingestion path: parse_header_ctrees builds a column map, then
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
 * @test  test_find_start_and_end_filenum
 * Maps a contiguous forest range onto the input files that contain it.
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

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Consistent-Trees Support Code\n");
  printf("============================================================\n");
  printf("%s", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_parse_header_column_mapping);
  TEST_RUN(test_read_forests_and_locations);
  TEST_RUN(test_read_single_tree_rows);
  TEST_RUN(test_forest_topology_reconstruction);
  TEST_RUN(test_find_start_and_end_filenum);
  TEST_RUN(test_weighted_forest_distribution);

  TEST_SUMMARY();
  return TEST_RESULT();
}
