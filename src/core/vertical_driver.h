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
 * (src/core/vertical_driver.c). Between run_vertical_driver()'s startup scan and
 * its first unit, and after a run that processed no unit, `unit` is -1 while
 * rows_per_unit and fits carry the run's evaluation; before any vertical run it
 * is {-1, 0, true}. */
struct RecordIdentitySpace vertical_driver_record_identity_space(void);

#endif /* CORE_VERTICAL_DRIVER_H */
