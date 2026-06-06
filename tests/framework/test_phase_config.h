/**
 * @file    test_phase_config.h
 * @brief   Test helpers for building MimicConfig substep phases in C unit tests
 *
 * The runtime no longer has fixed MimicConfig.galaxy_physics/satellite_mergers fields; substep
 * middle phases are an arbitrary ordered set (MimicConfig.substep_phases). These
 * helpers let unit tests construct that pipeline by name without duplicating the
 * allocation boilerplate, mirroring the YAML 'phases:' form.
 *
 * Allocations match module_system_cleanup()'s free strategy (mymalloc_cat arrays
 * + strdup strings), so a test that calls module_system_init()/cleanup() frees
 * them automatically. Tests that never reach cleanup (e.g. negative tests where
 * init fails before the pipeline is built) should call test_free_substep_phases()
 * in teardown.
 *
 * Include AFTER core/module_registry.h and include/types.h.
 */
#ifndef TEST_PHASE_CONFIG_H
#define TEST_PHASE_CONFIG_H

#include <stdlib.h>
#include <string.h>

#include "util/memory.h"
#include "core/module_registry.h"
#include "include/types.h"
#include "include/globals.h" /* extern struct MimicConfig MimicConfig */

/** Per-phase module capacity for tests (over-allocated; freed by count). */
#define TEST_PHASE_MODULE_CAP 16

/**
 * @brief   Append (module_name, mode) to the named substep phase, creating it
 *          if it does not exist yet. Phases appear in first-mention order.
 */
static inline void test_phase_add(const char *phase_name, const char *module_name,
                                  enum ProcessingMode mode) {
  if (MimicConfig.substep_phases == NULL) {
    MimicConfig.substep_phases =
        mymalloc_cat(MAX_SUBSTEP_PHASES * sizeof(struct ModulePhaseConfig), MEM_UTILITY);
    MimicConfig.num_substep_phases = 0;
  }

  struct ModulePhaseConfig *phase = NULL;
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    if (strcmp(MimicConfig.substep_phases[p].name, phase_name) == 0) {
      phase = &MimicConfig.substep_phases[p];
      break;
    }
  }
  if (phase == NULL) {
    phase = &MimicConfig.substep_phases[MimicConfig.num_substep_phases++];
    phase->name = strdup(phase_name);
    phase->modules =
        mymalloc_cat(TEST_PHASE_MODULE_CAP * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    phase->num_modules = 0;
  }

  int i = phase->num_modules++;
  phase->modules[i].module_name = strdup(module_name);
  phase->modules[i].processing_mode = mode;
}

/**
 * @brief   Free substep phases built by test_phase_add(), matching the runtime
 *          cleanup's allocation strategy. Safe to call when none were built.
 */
static inline void test_free_substep_phases(void) {
  if (MimicConfig.substep_phases == NULL) {
    return;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    for (int i = 0; i < phase->num_modules; i++) {
      free(phase->modules[i].module_name);
    }
    myfree(phase->modules);
    free(phase->name);
  }
  myfree(MimicConfig.substep_phases);
  MimicConfig.substep_phases = NULL;
  MimicConfig.num_substep_phases = 0;
}

#endif /* TEST_PHASE_CONFIG_H */
