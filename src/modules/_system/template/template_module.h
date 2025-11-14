/**
 * @file    template_module.h
 * @brief   Template physics module interface
 *
 * USAGE: This is a template for creating new Mimic physics modules.
 *
 * TO CREATE A NEW MODULE:
 * 1. Copy this directory: cp -r src/modules/_template src/modules/YOUR_MODULE_NAME
 * 2. Rename files: template_module.h → your_module.h, template_module.c → your_module.c
 * 3. Replace all "template_module" with "your_module" (find and replace)
 * 4. Replace all "TEMPLATE_MODULE" with "YOUR_MODULE"
 * 5. Update file documentation below
 * 6. Implement physics in the .c file
 * 7. Add to src/modules/module_init.c for registration
 *
 * DELETE THIS SECTION after customizing.
 *
 * ---
 *
 * [REPLACE WITH YOUR MODULE DESCRIPTION]
 *
 * This module implements [DESCRIBE PHYSICS PROCESS].
 *
 * Physics: [WRITE EQUATION OR BRIEF DESCRIPTION]
 *   Example: ΔColdGas = f_cool * Mvir * Δt
 *
 * Reference: [CITE PAPER OR SAGE SOURCE]
 *   Example: Based on SAGE model_cooling.c (Croton et al. 2016)
 *
 * Dependencies:
 *   - Requires: [LIST REQUIRED PROPERTIES FROM OTHER MODULES]
 *   - Provides: [LIST PROPERTIES THIS MODULE CREATES]
 *
 * Parameters:
 *   - TemplateModule_Parameter1: [DESCRIPTION, DEFAULT VALUE, RANGE]
 *   - TemplateModule_Parameter2: [DESCRIPTION, DEFAULT VALUE, RANGE]
 */

#ifndef TEMPLATE_MODULE_H
#define TEMPLATE_MODULE_H

/**
 * @brief   Register the template module
 *
 * Registers this module with the module registry. This function should be
 * called once during program initialization before module_system_init().
 *
 * Called from: src/modules/module_init.c :: register_all_modules()
 */
void template_module_register(void);

#endif // TEMPLATE_MODULE_H
