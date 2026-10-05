#ifndef CORE_TASK_LAYOUT_H
#define CORE_TASK_LAYOUT_H

/**
 * @file    core/task_layout.h
 * @brief   The run's task count and this process's task id, as both drivers see them.
 *
 * NTask and ThisTask keep their non-MPI defaults (NTask == 0, ThisTask == 0)
 * outside an MPI build, so every reader of them goes through these two helpers
 * instead: a run always has at least one task, and a process's id is never
 * negative.
 */

#include "globals.h"

/** Number of tasks this run divides its work over; 1 for a serial run. */
static inline int effective_task_count(void) { return NTask > 0 ? NTask : 1; }

/** This process's task id in [0, effective_task_count()); 0 for a serial run. */
static inline int current_task_id(void) { return ThisTask >= 0 ? ThisTask : 0; }

#endif /* #ifndef CORE_TASK_LAYOUT_H */
