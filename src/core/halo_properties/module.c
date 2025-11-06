/**
 * @file    module.c
 * @brief   Module implementation stubs for halo properties
 * @author  Mimic Development Team
 *
 * This file contains stub implementations for the module system interface.
 * These functions are not yet called by the main program but are placeholders
 * for the future module registry system.
 */

#include "module.h"
#include "../util/error.h"

/**
 * @brief   Initialize the halo properties module
 * @return  0 on success, non-zero on error
 *
 * This is a stub function for future module system implementation.
 * Currently not called by the main program.
 */
int halo_properties_init(void) {
  INFO_LOG("Halo properties module initialized (stub)");
  return 0;
}

/**
 * @brief   Clean up the halo properties module
 * @return  0 on success, non-zero on error
 *
 * This is a stub function for future module system implementation.
 * Currently not called by the main program.
 */
int halo_properties_cleanup(void) {
  return 0;
}
