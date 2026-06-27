#ifndef CORE_TREE_DRIVER_H
#define CORE_TREE_DRIVER_H

/**
 * @file    tree_driver.h
 * @brief   Tree-ordered partition driver entry points
 *
 * Owns the per-partition output path lifecycle, XCPU signal state, and the
 * dispatch from run_processing_driver() to run_tree_driver().
 */

#include <signal.h>

extern volatile sig_atomic_t TreeDriverGotXCPU;

void tree_driver_clear_current_output_paths(void);
void tree_driver_remove_incomplete_outputs(void);
void run_tree_driver(void);
void run_processing_driver(void);

#endif /* CORE_TREE_DRIVER_H */
