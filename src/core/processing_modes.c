/**
 * @file    processing_modes.c
 * @brief   The single C table of module processing modes and its lookups
 *
 * Each processing mode names its run-file configuration string and the
 * callback family it dispatches to. Every C lookup (parsing a mode name,
 * naming a mode, finding a mode's family) goes through this table, and
 * scripts/module_modes.py mirrors it for metadata generation and validation.
 *
 * The file depends on nothing but the module headers and <string.h> (no
 * logging, no allocation, no globals), so any translation unit that needs to
 * map mode names, including the run-file parser and the topology-dump harness
 * that links it without the module system, can link it on its own.
 */

#include <string.h>

#include "module_interface.h"
#include "module_registry.h"

/**
 * @brief   One processing mode: its configuration name and callback family
 */
struct ProcessingModeDescriptor {
  enum ProcessingMode mode;
  const char *name;
  enum ModuleCallbackFamily family;
};

/**
 * @brief   The single C table of processing modes
 *
 * A mode absent from the table fails closed everywhere. Adding a mode or a
 * family needs an explicit entry here and in scripts/module_modes.py; the
 * static assertion below makes an enumerator without an entry a compile error.
 */
static const struct ProcessingModeDescriptor processing_mode_descriptors[] = {
    {PROCESSING_MODE_FULL_HALO, "process_full_halo", MODULE_CALLBACK_FAMILY_FOF},
    {PROCESSING_MODE_PER_EVENT, "process_per_event", MODULE_CALLBACK_FAMILY_FOF},
    {PROCESSING_MODE_BY_GALAXY, "process_by_galaxy", MODULE_CALLBACK_FAMILY_FOF},
    {PROCESSING_MODE_SNAPSHOT, "process_snapshot", MODULE_CALLBACK_FAMILY_SNAPSHOT},
};

#define NUM_PROCESSING_MODE_DESCRIPTORS                                                            \
  ((int)(sizeof(processing_mode_descriptors) / sizeof(processing_mode_descriptors[0])))

_Static_assert(NUM_PROCESSING_MODE_DESCRIPTORS == PROCESSING_MODE_COUNT,
               "processing_mode_descriptors[] needs exactly one entry per enum ProcessingMode "
               "value; add the new mode here and to scripts/module_modes.py");

/**
 * @brief   Find the descriptor for a processing mode
 *
 * @param   mode  Processing mode
 * @return  The descriptor, or NULL for a value outside the table
 */
static const struct ProcessingModeDescriptor *find_mode_descriptor(enum ProcessingMode mode) {
  for (int i = 0; i < NUM_PROCESSING_MODE_DESCRIPTORS; i++) {
    if (processing_mode_descriptors[i].mode == mode) {
      return &processing_mode_descriptors[i];
    }
  }
  return NULL;
}

const char *processing_mode_to_string(enum ProcessingMode mode) {
  const struct ProcessingModeDescriptor *descriptor = find_mode_descriptor(mode);
  return (descriptor != NULL) ? descriptor->name : "unknown";
}

int processing_mode_from_string(const char *name, enum ProcessingMode *out_mode) {
  if (name == NULL || out_mode == NULL) {
    return -1;
  }
  for (int i = 0; i < NUM_PROCESSING_MODE_DESCRIPTORS; i++) {
    if (strcmp(processing_mode_descriptors[i].name, name) == 0) {
      *out_mode = processing_mode_descriptors[i].mode;
      return 0;
    }
  }
  return -1;
}

int processing_mode_family(enum ProcessingMode mode, enum ModuleCallbackFamily *out_family) {
  const struct ProcessingModeDescriptor *descriptor = find_mode_descriptor(mode);
  if (descriptor == NULL || out_family == NULL) {
    return -1;
  }
  *out_family = descriptor->family;
  return 0;
}
