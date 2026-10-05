/**
 * @file    test_unit_horizontal_identity.c
 * @brief   The horizontal reader's per-halo identity equals the vertical reader's.
 *
 * UniqueGalaxyID is built from two components: the halo's rank within its
 * forest and the forest's run-scoped index. The vertical path takes both from
 * the order the L-Halo reader enumerates the source: the forest index is
 * GlobalForestOffset (the count prefix over earlier files) plus the tree's index
 * in its file, and the rank is the halo's position within that tree
 * (core/build_model.c, make_unique_galaxy_id). The horizontal path reads both
 * from the version 3 dataset's ForestIndex and HaloRankInForest columns
 * (struct SnapshotSlab forest_index / halo_rank_in_forest). The two drivers can
 * only emit the same UniqueGalaxyID for the same halo if these agree halo by
 * halo, which this file checks on every committed converter-produced fixture
 * whose L-Halo source is committed beside it:
 *
 *   ../data/{worked_graph,three_snapshot_chain,adjacent}/  (sources in ../data/source/)
 *   tests/data/horizontal_v3/dataset/                      (source in
 * tests/data/horizontal_v3/source/)
 *
 * The last is the only fixture with more than one forest, so it is the one that
 * can tell a forest index taken from the source order apart from a constant.
 *
 * How the two sides are read:
 *   - Horizontal: the registered horizontal_hdf5 reader, open_run then
 *     load_slab of every snapshot, exactly as the horizontal driver does.
 *   - Vertical: the registered lhalo_binary reader's own hooks enumerate
 *     partitions, count units (the prefix the vertical driver builds
 *     GlobalForestOffset from) and read each file's tree table. Its load_unit
 *     cannot be called here: it freads sizeof(struct RawHalo) per halo, and this
 *     package's RawHalo is the int64-link horizontal layout, not the 104-byte
 *     L-Halo record. So each halo's record is read at the position load_unit
 *     would read it (after the header and tree table, in stored order), through
 *     the L-Halo layout, and the file size is checked against that layout.
 *
 * Halos are joined by payload, never by identity: (snapshot, Len, M_Crit200 bits,
 * Pos[0] bits), which the fixture generators make distinct per halo and this
 * test requires to be unique on the vertical side. Every horizontal row must
 * join exactly one vertical halo, every vertical halo must be joined, and the
 * first identity mismatch fails naming snapshot, row and both values.
 */

#include "../../../../tests/framework/test_framework.h"

#include "../../../../src/include/constants.h"
#include "../../../../src/include/globals.h"
#include "../../../../src/include/proto.h"
#include "../../../../src/include/types.h"
#include "../../../../src/io/horizontal/reader.h"
#include "../../../../src/io/vertical/reader.h"
#include "../../../../src/util/error.h"
#include "../../../../src/util/memory.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

extern struct MimicConfig MimicConfig;

/* mini-Millennium's physical header values (simulations/mini-millennium/simulation_info.yaml),
   which every fixture here was stamped with by the converter. */
#define FIXTURE_BOX_SIZE 62.5
#define FIXTURE_OMEGA_MATTER 0.25
#define FIXTURE_OMEGA_LAMBDA 0.75
#define FIXTURE_HUBBLE_H 0.73
#define FIXTURE_PART_MASS 0.0860657

/* The shipped L-Halo record (convert/mimic-convert/adapters/source_inventory.LHALO_FIELDS):
   byte offsets of the fields the join key reads. */
#define LHALO_RECORD_BYTES 104
#define LHALO_OFFSET_LEN 20
#define LHALO_OFFSET_M_CRIT200 28
#define LHALO_OFFSET_POS0 36
#define LHALO_OFFSET_SNAPNUM 88

/* Upper bound on halos per fixture; the largest committed fixture holds 7. */
#define MAX_FIXTURE_HALOS 64

/** One committed dataset and the L-Halo source it was converted from. */
struct IdentityFixture {
  const char *dataset_dir; /* version 3 snapshot files */
  const char *a_list;      /* snapshot list inside dataset_dir */
  const char *source_dir;  /* L-Halo source directory */
  const char *tree_name;   /* source file stem: <tree_name>.<filenr> */
};

static const struct IdentityFixture FIXTURES[] = {
    {"simulations/mini-millennium-horizontal/_tests/data/worked_graph", "worked_graph.a_list",
     "simulations/mini-millennium-horizontal/_tests/data/source", "trees_worked_graph"},
    {"simulations/mini-millennium-horizontal/_tests/data/three_snapshot_chain",
     "three_snapshot_chain.a_list", "simulations/mini-millennium-horizontal/_tests/data/source",
     "trees_three_snapshot_chain"},
    {"simulations/mini-millennium-horizontal/_tests/data/adjacent", "adjacent.a_list",
     "simulations/mini-millennium-horizontal/_tests/data/source", "trees_adjacent"},
    {"tests/data/horizontal_v3/dataset", "fixture.a_list", "tests/data/horizontal_v3/source",
     "trees_fixture"},
};

/** The payload a halo is joined by: never an identity component. */
struct JoinKey {
  int snapnum;
  int len;
  uint32_t m_crit200_bits;
  uint32_t pos0_bits;
};

/** One halo as the vertical reader enumerates it. */
struct VerticalHalo {
  struct JoinKey key;
  int64_t forest_index; /* GlobalForestOffset + unit */
  int64_t rank;         /* position within the unit */
  int joined;           /* horizontal rows joined to this halo */
};

static struct VerticalHalo vertical[MAX_FIXTURE_HALOS];
static int64_t n_vertical = 0;

/* Failure text for TEST_ASSERT, which takes a message but not a format.
   check_fixture() writes the description of each check here before making it,
   so on a failed check it names the failure. */
static char message[512];

/* Inside check_fixture(): stop at the first failed check. */
#define REQUIRE(cond)                                                                              \
  do {                                                                                             \
    if (!(cond)) {                                                                                 \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)

static uint32_t float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static int keys_equal(const struct JoinKey *a, const struct JoinKey *b) {
  return a->snapnum == b->snapnum && a->len == b->len && a->m_crit200_bits == b->m_crit200_bits &&
         a->pos0_bits == b->pos0_bits;
}

static int32_t read_i32(const unsigned char *record, size_t offset) {
  int32_t value;
  memcpy(&value, record + offset, sizeof(value));
  return value;
}

static float read_f32(const unsigned char *record, size_t offset) {
  float value;
  memcpy(&value, record + offset, sizeof(value));
  return value;
}

/**
 * @brief   Enumerate the fixture's L-Halo source as the vertical reader does.
 * @return  0 on success, -1 on any inconsistency (a message is printed).
 */
static int enumerate_vertical(const struct IdentityFixture *fixture) {
  const struct VerticalReader *reader = vertical_reader_lookup("lhalo_binary");
  if (reader == NULL) {
    fprintf(stderr, "  lhalo_binary is not registered\n");
    return -1;
  }

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", fixture->source_dir);
  snprintf(MimicConfig.TreeName, sizeof(MimicConfig.TreeName), "%s", fixture->tree_name);
  MimicConfig.FirstFile = 0;
  MimicConfig.LastFile = 0;
  MimicConfig.vertical_reader = reader;

  n_vertical = 0;
  int64_t forest_offset = 0;
  const int npartitions = reader->num_partitions();
  for (int partition = 0; partition < npartitions; partition++) {
    const int output_id = reader->partition_output_id(partition);
    if (!reader->partition_exists(partition)) {
      fprintf(stderr, "  source file %d of %s is missing\n", output_id, fixture->tree_name);
      return -1;
    }
    const int64_t units = reader->count_partition_units(partition);
    GlobalForestOffset = forest_offset;
    reader->open_partition(output_id);
    if ((int64_t)Ntrees != units) {
      fprintf(stderr, "  file %d opened with %d trees but counted %" PRId64 "\n", output_id, Ntrees,
              units);
      return -1;
    }

    char path[3 * MAX_STRING_LEN];
    snprintf(path, sizeof(path), "%s/%s.%d", fixture->source_dir, fixture->tree_name, output_id);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
      fprintf(stderr, "  cannot open %s\n", path);
      return -1;
    }
    /* load_unit reads each tree's records back to back after Ntrees, totNHalos
       and the per-tree counts. */
    const long table_bytes = (long)(2 + Ntrees) * (long)sizeof(int32_t);
    int64_t file_halos = 0;
    int rc = 0;
    for (int unit = 0; unit < Ntrees && rc == 0; unit++) {
      for (int k = 0; k < InputTreeNHalos[unit]; k++) {
        const int64_t record_index = (int64_t)InputTreeFirstHalo[unit] + k;
        unsigned char record[LHALO_RECORD_BYTES];
        if (n_vertical >= MAX_FIXTURE_HALOS ||
            fseek(fp, table_bytes + (long)record_index * LHALO_RECORD_BYTES, SEEK_SET) != 0 ||
            fread(record, sizeof(record), 1, fp) != 1) {
          fprintf(stderr, "  cannot read record %" PRId64 " of %s\n", record_index, path);
          rc = -1;
          break;
        }
        struct VerticalHalo *halo = &vertical[n_vertical++];
        halo->key.snapnum = read_i32(record, LHALO_OFFSET_SNAPNUM);
        halo->key.len = read_i32(record, LHALO_OFFSET_LEN);
        halo->key.m_crit200_bits = float_bits(read_f32(record, LHALO_OFFSET_M_CRIT200));
        halo->key.pos0_bits = float_bits(read_f32(record, LHALO_OFFSET_POS0));
        halo->forest_index = GlobalForestOffset + unit;
        halo->rank = k;
        halo->joined = 0;
        file_halos++;
      }
    }
    fclose(fp);

    /* The file must be exactly header + table + records in the L-Halo layout,
       or the offsets above would be reading some other layout. */
    struct stat st;
    if (rc == 0 && (stat(path, &st) != 0 ||
                    (int64_t)st.st_size != table_bytes + file_halos * LHALO_RECORD_BYTES)) {
      fprintf(stderr, "  %s is not %" PRId64 " L-Halo records after its tree table\n", path,
              file_halos);
      rc = -1;
    }

    myfree(InputTreeFirstHalo);
    InputTreeFirstHalo = NULL;
    myfree(InputTreeNHalos);
    InputTreeNHalos = NULL;
    reader->close_partition();
    if (rc != 0) {
      return -1;
    }
    forest_offset += units;
  }

  for (int64_t i = 0; i < n_vertical; i++) {
    for (int64_t j = i + 1; j < n_vertical; j++) {
      if (keys_equal(&vertical[i].key, &vertical[j].key)) {
        fprintf(stderr, "  vertical halos %" PRId64 " and %" PRId64 " share a join key\n", i, j);
        return -1;
      }
    }
  }
  return 0;
}

/** @brief Point MimicConfig at a committed dataset, as a run of this package would. */
static void configure_horizontal(const struct IdentityFixture *fixture) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s",
           fixture->dataset_dir);
  snprintf(MimicConfig.FileWithSnapList, sizeof(MimicConfig.FileWithSnapList), "%s/%s",
           fixture->dataset_dir, fixture->a_list);
  read_snap_list();
  MimicConfig.MAXSNAPS = MimicConfig.Snaplistlen;
  MimicConfig.UniqueGalaxyIDMultiplier = (int64_t)TREE_MUL_FAC;
  MimicConfig.BoxSize = FIXTURE_BOX_SIZE;
  MimicConfig.Omega = FIXTURE_OMEGA_MATTER;
  MimicConfig.OmegaLambda = FIXTURE_OMEGA_LAMBDA;
  MimicConfig.Hubble_h = FIXTURE_HUBBLE_H;
  MimicConfig.PartMass = FIXTURE_PART_MASS;
}

/** @brief Index of the vertical halo with `key`, or -1. Keys are unique. */
static int64_t find_vertical(const struct JoinKey *key) {
  for (int64_t i = 0; i < n_vertical; i++) {
    if (keys_equal(&vertical[i].key, key)) {
      return i;
    }
  }
  return -1;
}

/**
 * @brief   Compare every halo of every snapshot of one fixture.
 * @return  0 when every check holds; -1 at the first that does not, with
 *          `message` describing it.
 */
static int check_fixture(const struct IdentityFixture *fixture) {
  snprintf(message, sizeof(message), "%s: the L-Halo source should enumerate cleanly",
           fixture->tree_name);
  REQUIRE(enumerate_vertical(fixture) == 0);
  snprintf(message, sizeof(message), "%s: the source should hold at least one halo",
           fixture->tree_name);
  REQUIRE(n_vertical > 0);

  int64_t vertical_forests = 0;
  int64_t vertical_max_rank = -1;
  for (int64_t i = 0; i < n_vertical; i++) {
    if (vertical[i].forest_index + 1 > vertical_forests) {
      vertical_forests = vertical[i].forest_index + 1;
    }
    if (vertical[i].rank > vertical_max_rank) {
      vertical_max_rank = vertical[i].rank;
    }
  }

  const struct HorizontalReader *reader = horizontal_reader_lookup("horizontal_hdf5");
  struct HorizontalRunInfo info;
  configure_horizontal(fixture);
  const struct HorizontalOpenOptions options = {.validate_columns = 1};
  horizontal_reader_open_run(reader, &options, &info);

  snprintf(message, sizeof(message), "%s: n_forests_total horizontal=%" PRId64 " vertical=%" PRId64,
           fixture->dataset_dir, info.n_forests_total, vertical_forests);
  REQUIRE(info.n_forests_total == vertical_forests);
  snprintf(message, sizeof(message),
           "%s: max_halo_rank_in_forest horizontal=%" PRId64 " vertical=%" PRId64,
           fixture->dataset_dir, info.max_halo_rank_in_forest, vertical_max_rank);
  REQUIRE(info.max_halo_rank_in_forest == vertical_max_rank);

  int64_t rows_seen = 0;
  for (int64_t snap = 0; snap < info.snapshot_count; snap++) {
    struct SnapshotSlab slab = snapshot_slab_empty();
    horizontal_reader_load_slab(reader, snap, 0, horizontal_reader_halo_count(reader, snap), &slab);
    for (int64_t row = 0; row < slab.nhalos; row++) {
      const struct RawHalo *halo = &slab.halos[row];
      const struct JoinKey key = {(int)snap, halo->Len, float_bits(halo->M_Crit200),
                                  float_bits(halo->Pos[0])};
      const int64_t match = find_vertical(&key);
      snprintf(message, sizeof(message),
               "%s snapshot %" PRId64 " row %" PRId64
               ": no vertical halo has this row's (snapshot, Len, M_Crit200, Pos[0])",
               fixture->dataset_dir, snap, row);
      REQUIRE(match >= 0);
      struct VerticalHalo *expected = &vertical[match];
      snprintf(message, sizeof(message),
               "%s snapshot %" PRId64 " row %" PRId64 ": the vertical halo is joined twice",
               fixture->dataset_dir, snap, row);
      REQUIRE(expected->joined == 0);
      expected->joined = 1;

      snprintf(message, sizeof(message),
               "%s snapshot %" PRId64 " row %" PRId64 ": forest_index horizontal=%" PRId64
               " vertical=%" PRId64,
               fixture->dataset_dir, snap, row, slab.forest_index[row], expected->forest_index);
      REQUIRE(slab.forest_index[row] == expected->forest_index);
      snprintf(message, sizeof(message),
               "%s snapshot %" PRId64 " row %" PRId64 ": halo_rank_in_forest horizontal=%" PRId64
               " vertical=%" PRId64,
               fixture->dataset_dir, snap, row, slab.halo_rank_in_forest[row], expected->rank);
      REQUIRE(slab.halo_rank_in_forest[row] == expected->rank);
      rows_seen++;
    }
    horizontal_reader_release_slab(reader, &slab);
  }
  horizontal_reader_close_run(reader);

  snprintf(message, sizeof(message),
           "%s: %" PRId64 " horizontal rows joined, the source holds %" PRId64 " halos",
           fixture->dataset_dir, rows_seen, n_vertical);
  REQUIRE(rows_seen == n_vertical);
  return 0;
}

/**
 * @test  test_horizontal_identity_matches_vertical
 * Every halo of every snapshot of every committed fixture carries the forest
 * index and halo rank the vertical reader assigns the same halo of the same
 * source, and the run-scoped identity bounds equal the source's.
 */
int test_horizontal_identity_matches_vertical(void) {
  if (horizontal_reader_lookup("horizontal_hdf5") == NULL) {
    return TEST_SKIP_WITH("horizontal_hdf5 needs an HDF5 build");
  }
  for (size_t i = 0; i < sizeof(FIXTURES) / sizeof(FIXTURES[0]); i++) {
    TEST_ASSERT(check_fixture(&FIXTURES[i]) == 0, message);
  }
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Horizontal/Vertical Halo Identity Equivalence\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_horizontal_identity_matches_vertical);

  check_memory_leaks();

  TEST_SUMMARY();
  return TEST_RESULT();
}
