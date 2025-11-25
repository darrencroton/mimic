#include <stdio.h>
#include <time.h>

#include "git_version.h"
#include "run_log.h"

int MimicLogUseColor = 0;

void log_run_header(const char *param_file) {
  const char *bold = MimicLogUseColor ? "\x1b[1m" : "";
  const char *reset = MimicLogUseColor ? "\x1b[0m" : "";

  /* Optional per-line colours for the ASCII MIMIC banner */
  const char *c1 = MimicLogUseColor ? "\x1b[31m" : "";   // red
  const char *c2 = MimicLogUseColor ? "\x1b[33m" : "";   // yellow
  const char *c3 = MimicLogUseColor ? "\x1b[32m" : "";   // green
  const char *c4 = MimicLogUseColor ? "\x1b[36m" : "";   // cyan
  const char *c5 = MimicLogUseColor ? "\x1b[35m" : "";   // magenta

  time_t now;
  time(&now);
  char time_str[32];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(stdout, "%s%s    __  ___   ____  __  ___   ____   ______%s\n", bold, c1,
      reset);
    fprintf(stdout, "%s   /  |/  /  /  _/ /  |/  /  /  _/  / ____/%s\n", c2, reset);
    fprintf(stdout, "%s  / /|_/ /   / /  / /|_/ /   / /   / /     %s\n", c3, reset);
    fprintf(stdout, "%s / /  / /  _/ /  / /  / /  _/ /   / /___   %s\n", c4, reset);
    fprintf(stdout, "%s/_/  /_/  /___/ /_/  /_/  /___/   \\____/   %s%s\n\n", c5,
      bold, reset);

  fprintf(stdout, "%sMIMIC Galaxy Evolution Framework%s\n", bold, reset);
  fprintf(stdout, "%sCommit%s  : %s (%s)\n", bold, reset, GIT_COMMIT,
          GIT_BRANCH);
  fprintf(stdout, "Started : %s\n", time_str);
  if (param_file != NULL) {
    fprintf(stdout, "Config  : %s\n", param_file);
  }
}

void log_phase_banner(MimicPhase phase) {
  const char *bold_cyan = MimicLogUseColor ? "\x1b[1;36m" : "";
  const char *reset = MimicLogUseColor ? "\x1b[0m" : "";

  const char *name = "UNKNOWN";
  const char *label = "";
  switch (phase) {
  case PHASE_STARTUP:
    name = "PHASE 0";
    label = "STARTUP";
    break;
  case PHASE_CONFIG:
    name = "PHASE 1";
    label = "CONFIGURATION & ENVIRONMENT";
    break;
  case PHASE_MODULE_PIPELINE:
    name = "PHASE 2";
    label = "MODULE PIPELINE";
    break;
  case PHASE_TREE_PROCESSING:
    name = "PHASE 3";
    label = "TREE PROCESSING";
    break;
  case PHASE_OUTPUT:
    name = "PHASE 4";
    label = "OUTPUT & METADATA";
    break;
  case PHASE_SHUTDOWN:
    name = "PHASE 5";
    label = "SHUTDOWN";
    break;
  default:
    break;
  }

  fprintf(stdout, "\n");
  fprintf(stdout,
          "===============================================================\n");
  fprintf(stdout, "%s[%s] %s%s\n", bold_cyan, name, label, reset);
  fprintf(stdout,
          "===============================================================\n");
}
