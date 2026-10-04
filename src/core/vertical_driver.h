#ifndef CORE_VERTICAL_DRIVER_H
#define CORE_VERTICAL_DRIVER_H

/**
 * @file    vertical_driver.h
 * @brief   Vertical partition driver entry points
 *
 * Owns the per-partition output path lifecycle, XCPU signal state, and the
 * dispatch from run_processing_driver() to run_vertical_driver().
 */

#include <signal.h>

#include "types.h" /* struct RecordIdentitySpace */

extern volatile sig_atomic_t VerticalDriverGotXCPU;

void vertical_driver_clear_current_output_paths(void);
void vertical_driver_remove_incomplete_outputs(void);
void run_vertical_driver(void);
void run_processing_driver(void);

/* The vertical driver's currently published identity space
 * (src/core/vertical_driver.c), a read-only observation seam for tests: the
 * driver itself hands each unit's space to its FoF workspace. Before any
 * vertical run it is {.unit = -1, .rows_per_unit = 0, .fits = true, .units = 0,
 * .driver = "vertical"}. run_vertical_driver()'s startup scan sets
 * rows_per_unit, fits and units for the run, leaving unit at -1 until the first
 * unit is loaded; after a run, unit keeps the last global forest number
 * published, or -1 when the run processed no unit. */
struct RecordIdentitySpace vertical_driver_record_identity_space(void);

#endif /* CORE_VERTICAL_DRIVER_H */
