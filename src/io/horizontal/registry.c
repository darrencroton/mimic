/**
 * @file    horizontal/registry.c
 * @brief   Static registry of horizontal input readers.
 *
 * The single source of truth for which horizontal input formats this
 * build supports, mirroring vertical/registry.c. Adding a format means implementing
 * a `struct HorizontalReader` in its own file and appending one row here.
 *
 * This file is deliberately NOT named *hdf5.c: the Makefile drops sources
 * matching that pattern from USE-HDF5=no builds, and horizontal_reader_lookup()
 * is called from the configuration path in every build. In a non-HDF5 build the
 * table is elided entirely (a zero-length array is not valid ISO C) and the
 * lookup returns NULL for every name.
 */

#include <stddef.h>
#include <strings.h> /* strcasecmp */

#include "horizontal/reader.h"

#ifdef HDF5
/* Format readers, each defined in its implementation file. */
extern const struct HorizontalReader HorizontalHDF5Reader;

static const struct HorizontalReader *const horizontal_reader_table[] = {
    &HorizontalHDF5Reader,
};

#define HORIZONTAL_READER_TABLE_COUNT                                                              \
  (sizeof(horizontal_reader_table) / sizeof(horizontal_reader_table[0]))
#endif

const struct HorizontalReader *horizontal_reader_lookup(const char *name) {
  if (name == NULL)
    return NULL;

#ifdef HDF5
  /* Case-insensitive, matching vertical_reader_lookup() and the output_format
     parser in read_parameter_file.c. */
  for (size_t i = 0; i < HORIZONTAL_READER_TABLE_COUNT; i++) {
    if (strcasecmp(horizontal_reader_table[i]->name, name) == 0)
      return horizontal_reader_table[i];
  }
#endif

  return NULL;
}

size_t horizontal_reader_count(void) {
#ifdef HDF5
  return HORIZONTAL_READER_TABLE_COUNT;
#else
  return 0;
#endif
}

const struct HorizontalReader *horizontal_reader_at(size_t index) {
#ifdef HDF5
  if (index < HORIZONTAL_READER_TABLE_COUNT)
    return horizontal_reader_table[index];
#else
  (void)index;
#endif
  return NULL;
}
