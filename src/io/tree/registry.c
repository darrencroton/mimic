/**
 * @file    tree/registry.c
 * @brief   Static registry of merger-tree readers.
 *
 * The single source of truth for which input formats this build supports.
 * Adding a format means implementing a `struct TreeReader` in its own file and
 * appending one row here; the core read path (tree/interface.c) never changes.
 */

#include <stddef.h>
#include <strings.h> /* strcasecmp */

#include "tree/reader.h"

/* Format readers, each defined in its implementation file. */
extern const struct TreeReader LHaloBinaryReader;
extern const struct TreeReader CTreesAsciiReader;
#ifdef HDF5
extern const struct TreeReader LHaloHDF5Reader;
#endif

static const struct TreeReader *const reader_table[] = {
    &LHaloBinaryReader,
    &CTreesAsciiReader,
#ifdef HDF5
    &LHaloHDF5Reader,
#endif
};

const struct TreeReader *tree_reader_lookup(const char *name) {
  if (name == NULL)
    return NULL;
  /* Case-insensitive to match the tree_type parsing this replaced and the
     output_format parser in read_parameter_file.c. */
  for (size_t i = 0; i < sizeof(reader_table) / sizeof(reader_table[0]); i++) {
    if (strcasecmp(reader_table[i]->name, name) == 0)
      return reader_table[i];
  }
  return NULL;
}
