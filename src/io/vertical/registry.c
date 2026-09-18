/**
 * @file    vertical/registry.c
 * @brief   Static registry of merger-tree readers.
 *
 * The single source of truth for which input formats this build supports.
 * Adding a format means implementing a `struct VerticalReader` in its own file and
 * appending one row here; the core read path (vertical/interface.c) never changes.
 */

#include <stddef.h>
#include <strings.h> /* strcasecmp */

#include "vertical/reader.h"

/* Format readers, each defined in its implementation file. */
extern const struct VerticalReader LHaloBinaryReader;
extern const struct VerticalReader CTreesAsciiReader;
#ifdef HDF5
extern const struct VerticalReader LHaloHDF5Reader;
extern const struct VerticalReader CTreesHDF5Reader;
#endif

static const struct VerticalReader *const reader_table[] = {
    &LHaloBinaryReader,
    &CTreesAsciiReader,
#ifdef HDF5
    &LHaloHDF5Reader,
    &CTreesHDF5Reader,
#endif
};

const struct VerticalReader *vertical_reader_lookup(const char *name) {
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
