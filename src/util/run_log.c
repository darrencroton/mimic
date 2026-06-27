/**
 * @file    run_log.c
 * @brief   Implementation of run-phase banners and the startup header
 */

#include <stdio.h>
#include <time.h>

#include "error.h"
#include "git_version.h"
#include "run_log.h"

int MimicLogUseColor = 0;

const char *mimic_color_green(void) { return MimicLogUseColor ? "\x1b[92m" : ""; }
const char *mimic_color_reset(void) { return MimicLogUseColor ? "\x1b[0m" : ""; }

void log_run_header(const char *param_file, LogLevel log_level) {
  /* Check if we're in quiet mode */
  int is_quiet = (log_level >= LOG_LEVEL_WARNING);

  const char *bold = MimicLogUseColor ? "\x1b[1m" : "";
  const char *reset = MimicLogUseColor ? "\x1b[0m" : "";

  /* Optional per-line colours for the ASCII Mimic banner */
  const char *c1 = MimicLogUseColor ? "\x1b[95m" : ""; // bright magenta
  const char *c2 = MimicLogUseColor ? "\x1b[94m" : ""; // bright blue
  const char *c3 = MimicLogUseColor ? "\x1b[96m" : ""; // bright cyan
  const char *c4 = MimicLogUseColor ? "\x1b[92m" : ""; // bright green
  const char *c5 = MimicLogUseColor ? "\x1b[93m" : ""; // bright yellow

  time_t now;
  time(&now);
  char time_str[32];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

  /* Skip ASCII art in quiet mode, but keep informational lines */
  if (!is_quiet) {
    fprintf(stdout, "%s%s    __  ___  ____  __  ___  ____  ______%s\n", bold, c1, reset);
    fprintf(stdout, "%s   /  |/  / /  _/ /  |/  / /  _/ / ____/%s\n", c2, reset);
    fprintf(stdout, "%s  / /|_/ /  / /  / /|_/ /  / /  / /     %s\n", c3, reset);
    fprintf(stdout, "%s / /  / / _/ /  / /  / / _/ /  / /___   %s\n", c4, reset);
    fprintf(stdout, "%s/_/  /_/ /___/ /_/  /_/ /___/  \\____/   %s%s\n\n", c5, bold, reset);
  }

  fprintf(stdout, "%sMimic Galaxy Evolution Framework%s\n", bold, reset);
  fprintf(stdout, "Commit  : %s (%s)\n", GIT_COMMIT, GIT_BRANCH);
  fprintf(stdout, "Started : %s\n", time_str);
  if (param_file != NULL) {
    fprintf(stdout, "Config  : %s\n", param_file);
  }
}

void log_phase_banner(MimicPhase phase) {
  LogLevel current_level = get_log_level();
  int is_quiet = (current_level >= LOG_LEVEL_WARNING);

  if (is_quiet) {
    if (phase == PHASE_TREE_PROCESSING) {
      fprintf(stdout, "\nMimic is running ...\n");
    } else if (phase == PHASE_SHUTDOWN) {
      fprintf(stdout, "Mimic has completed (check the config file for details)\n");
    }
    return;
  }

  /* Plain INFO mode: minimal output with spacing only */
  if (!get_verbose_format()) {
    if (phase == PHASE_TREE_PROCESSING) {
      fprintf(stdout, "\n");
    } else if (phase == PHASE_OUTPUT) {
      fprintf(stdout, "\n");
    }
    return;
  }

  /* Verbose/debug mode: full section banners */
  const char *bold_cyan = MimicLogUseColor ? "\x1b[1;36m" : "";
  const char *reset = MimicLogUseColor ? "\x1b[0m" : "";

  const char *label = "";
  switch (phase) {
  case PHASE_CONFIG:
    label = "CONFIGURATION";
    break;
  case PHASE_MODULE_PIPELINE:
    label = "MODULE INITIALIZATION";
    break;
  case PHASE_TREE_PROCESSING:
    label = "TREE PROCESSING";
    break;
  case PHASE_OUTPUT:
    label = "OUTPUT";
    break;
  case PHASE_SHUTDOWN:
    label = "SHUTDOWN";
    break;
  default:
    label = "UNKNOWN";
    break;
  }

  fprintf(stdout, "\n");
  fprintf(stdout, "===============================================================\n");
  fprintf(stdout, "%s%s%s\n", bold_cyan, label, reset);
  fprintf(stdout, "===============================================================\n");
}
