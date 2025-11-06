/**
 * @file    module.h
 * @brief   Module interface for halo properties calculations
 * @author  Mimic Development Team
 *
 * This is a stub for the future module system. Module initialization
 * and cleanup functions will be called by the module registry when
 * the system is implemented.
 */

#ifndef CORE_HALO_PROPERTIES_H
#define CORE_HALO_PROPERTIES_H

/**
 * @brief   Initialize the halo properties module
 * @return  0 on success, non-zero on error
 */
int halo_properties_init(void);

/**
 * @brief   Clean up the halo properties module
 * @return  0 on success, non-zero on error
 */
int halo_properties_cleanup(void);

#endif  // CORE_HALO_PROPERTIES_H
