/**
 * @file    output/util.c
 * @brief   Shared utilities for output file writing (binary and HDF5)
 *
 * This file implements common functions used by both binary and HDF5 output
 * writers. By centralizing this logic, we eliminate code duplication and
 * ensure consistent behavior across output formats.
 *
 * Key functions:
 * - prepare_halo_for_output(): Converts internal halo format to output format
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "globals.h"
#include "proto.h"
#include "output/util.h"
#include "output/binary.h"
#ifdef HDF5
#include "output/hdf5.h"
#endif
#include "module_system/output_helpers.h"
#include "module_system/physical_constants.h" /* For SEC_PER_MEGAYEAR */

void output_path_binary(char *buf, size_t size, int filenr, int snap_index) {
  int written =
      snprintf(buf, size, "%s/%s_z%1.3f_%d", MimicConfig.OutputDir, MimicConfig.OutputFileBaseName,
               MimicConfig.ZZ[MimicConfig.ListOutputSnaps[snap_index]], filenr);
  if (written < 0 || (size_t)written >= size) {
    FATAL_ERROR("Binary output path too long (filenr %d, snapshot index %d)", filenr, snap_index);
  }
}

void output_path_hdf5(char *buf, size_t size, int filenr) {
  int written = snprintf(buf, size, "%s/%s_%03d.hdf5", MimicConfig.OutputDir,
                         MimicConfig.OutputFileBaseName, filenr);
  if (written < 0 || (size_t)written >= size) {
    FATAL_ERROR("HDF5 output path too long (filenr %d)", filenr);
  }
}

void prepare_output_files(int filenr) {
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    open_hdf5_output_file(filenr);
    return;
  }
#endif
  create_binary_output_files(filenr);
}

/**
 * @brief   Converts internal halo structure to output format
 *
 * @param   g         Pointer to the internal halo tracking structure
 * @param   o         Pointer to the output halo structure to be filled
 *
 * This function transforms the internal halo representation (struct Halo)
 * to the output format (struct HaloOutput). All properties are copied
 * automatically by the generated code.
 *
 * All properties (including UniqueGalaxyID, UniqueCentralGalaxyID, and MostBoundID) are
 * stored in struct Halo and copied automatically.
 *
 * This function is format-agnostic and used by both binary and HDF5 output
 * writers, ensuring consistent halo conversion across all output formats.
 */
void prepare_halo_for_output(const struct Halo *g, struct HaloOutput *o) {
/* AUTO-GENERATED: Copy all properties from struct Halo to struct HaloOutput */
/* Includes automatic unit conversion for dT (seconds → Myr) with sentinel preservation */
#include "../../include/generated/copy_to_output.inc"
}
