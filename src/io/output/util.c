/**
 * @file    io_save_util.c
 * @brief   Shared utilities for output file writing (binary and HDF5)
 *
 * This file implements common functions used by both binary and HDF5 output
 * writers. By centralizing this logic, we eliminate code duplication and
 * ensure consistent behavior across output formats.
 *
 * Key functions:
 * - count_output_halos_by_snapshot(): Counts output halos by snapshot
 * - prepare_halo_for_output(): Converts internal halo format to output format
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "globals.h"
#include "proto.h"
#include "output/util.h"
#include "module_system/output_helpers.h"
#include "module_system/physical_constants.h"  /* For SEC_PER_MEGAYEAR */

/**
 * @brief Counts processed halos per requested output snapshot.
 *
 * See io_save_util.h for full documentation.
 */
void count_output_halos_by_snapshot(int OutputGalCount[MAXSNAPS]) {
  int i, n;

  for (i = 0; i < MAXSNAPS; i++)
    OutputGalCount[i] = 0;

  /*
   * Count halos for each requested output snapshot.
   *
   * Complexity: O(NumProcessedHalos × NOUT)
   */
  for (n = 0; n < MimicConfig.NOUT; n++) {
    for (i = 0; i < NumProcessedHalos; i++) {
      if (ProcessedHalos[i].SnapNum == MimicConfig.ListOutputSnaps[n]) {
        OutputGalCount[n]++;
      }
    }
  }
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
