/**
 * @file    run_log.h
 * @brief   Run-phase banners, startup header, and ANSI colour helpers
 */

#ifndef RUN_LOG_H
#define RUN_LOG_H

#include "error.h"

/**
 * @brief Simple phase identifiers for grouping runtime output
 */
typedef enum {
  PHASE_CONFIG = 0,
  PHASE_MODULE_PIPELINE,
  PHASE_TREE_PROCESSING,
  PHASE_OUTPUT,
  PHASE_SHUTDOWN
} MimicPhase;

/**
 * @brief Global flag controlling ANSI color usage in run banners
 *
 * This is configured by the main program based on TTY detection or
 * command-line flags.
 */
extern int MimicLogUseColor;

/**
 * @brief ANSI colour escapes for run output, gated on MimicLogUseColor.
 *
 * Each returns its escape sequence when colour is enabled and "" when it is not,
 * so any output can wrap text unconditionally, e.g.
 *   INFO_LOG("%sdone%s", mimic_color_green(), mimic_color_reset());
 * Add further colours here following the same pattern as they are needed.
 */
const char *mimic_color_green(void);
const char *mimic_color_reset(void);

/**
 * @brief Print a compact run header at startup
 *
 * @param param_file Path to the main parameter file
 * @param log_level  Current log level (for quiet mode check)
 */
void log_run_header(const char *param_file, LogLevel log_level);

/**
 * @brief Print a simple banner for the current execution phase
 *
 * @param phase Current phase identifier
 */
void log_phase_banner(MimicPhase phase);

#endif /* RUN_LOG_H */
