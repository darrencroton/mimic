/**
 * @file    snapshot/registry.c
 * @brief   Static registry of snapshot-ordered input readers.
 *
 * The single source of truth for which snapshot-ordered input formats this
 * build supports, mirroring tree/registry.c. Adding a format means implementing
 * a `struct SnapshotReader` in its own file and appending one row here.
 *
 * This file is deliberately NOT named *hdf5.c: the Makefile drops sources
 * matching that pattern from USE-HDF5=no builds, and snapshot_reader_lookup()
 * is called from the configuration path in every build. In a non-HDF5 build the
 * table is elided entirely (a zero-length array is not valid ISO C) and the
 * lookup returns NULL for every name.
 */

#include <stddef.h>
#include <strings.h> /* strcasecmp */

#include "snapshot/reader.h"

#ifdef HDF5
/* Format readers, each defined in its implementation file. */
extern const struct SnapshotReader SnapshotHDF5Reader;

static const struct SnapshotReader *const snapshot_reader_table[] = {
    &SnapshotHDF5Reader,
};

#define SNAPSHOT_READER_TABLE_COUNT                                                                \
  (sizeof(snapshot_reader_table) / sizeof(snapshot_reader_table[0]))
#endif

const struct SnapshotReader *snapshot_reader_lookup(const char *name) {
  if (name == NULL)
    return NULL;

#ifdef HDF5
  /* Case-insensitive, matching tree_reader_lookup() and the output_format
     parser in read_parameter_file.c. */
  for (size_t i = 0; i < SNAPSHOT_READER_TABLE_COUNT; i++) {
    if (strcasecmp(snapshot_reader_table[i]->name, name) == 0)
      return snapshot_reader_table[i];
  }
#endif

  return NULL;
}

size_t snapshot_reader_count(void) {
#ifdef HDF5
  return SNAPSHOT_READER_TABLE_COUNT;
#else
  return 0;
#endif
}

const struct SnapshotReader *snapshot_reader_at(size_t index) {
#ifdef HDF5
  if (index < SNAPSHOT_READER_TABLE_COUNT)
    return snapshot_reader_table[index];
#else
  (void)index;
#endif
  return NULL;
}
