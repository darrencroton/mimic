#ifndef CORE_TREE_DRIVER_H
#define CORE_TREE_DRIVER_H

#include <signal.h>

extern volatile sig_atomic_t TreeDriverGotXCPU;

void tree_driver_clear_current_output_paths(void);
void tree_driver_remove_incomplete_outputs(void);
void run_tree_driver(void);
void run_processing_driver(void);

#endif /* CORE_TREE_DRIVER_H */
