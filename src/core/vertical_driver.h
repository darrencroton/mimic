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

extern volatile sig_atomic_t VerticalDriverGotXCPU;

void vertical_driver_clear_current_output_paths(void);
void vertical_driver_remove_incomplete_outputs(void);
void run_vertical_driver(void);
void run_processing_driver(void);

#endif /* CORE_VERTICAL_DRIVER_H */
