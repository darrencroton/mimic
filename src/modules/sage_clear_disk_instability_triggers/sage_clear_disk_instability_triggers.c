/**
 * @file    sage_clear_disk_instability_triggers.c
 * @brief   Clear disk-instability trigger channel after phase_1 consumers
 *
 * This module clears only the disk-instability trigger field used by
 * sage_quasar_mode and sage_starburst_feedback in phase_1.
 */

#include "error.h"
#include "module_interface.h"
#include "types.h"

int sage_clear_disk_instability_triggers_init(void) { return 0; }

int sage_clear_disk_instability_triggers_process(struct ModuleContext *ctx,
                                                 struct Halo *halos, int ngal) {
  (void)ctx;

  if (ngal != 1) {
    ERROR_LOG("sage_clear_disk_instability_triggers expects ngal=1, got %d",
              ngal);
    return -1;
  }

  if (halos == NULL || halos[0].galaxy == NULL) {
    return 0;
  }

  halos[0].galaxy->UnstableDiskGasFraction = 0.0;
  return 0;
}

int sage_clear_disk_instability_triggers_cleanup(void) { return 0; }
