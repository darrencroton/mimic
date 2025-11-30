/**
 * @file    template_module.h
 * @brief   Template physics module interface
 *
 * USAGE: This is a template for creating new Mimic physics modules.
 *
 * TO CREATE A NEW MODULE:
 * 1. Copy this directory: cp -r src/modules/_system/template src/modules/YOUR_MODULE_NAME
 * 2. Rename files: template_module.h → your_module.h, template_module.c → your_module.c
 * 3. Create module_info.yaml with metadata (see template_module_info.yaml)
 * 4. Replace all "template_module" with "your_module" (find and replace)
 * 5. Replace all "TEMPLATE_MODULE" with "YOUR_MODULE"
 * 6. Update file documentation below
 * 7. Implement physics in the .c file
 * 8. Run 'make generate' to auto-generate registration code
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
 * Registers this module with the module registry. Called automatically
 * from generated code during program initialization.
 *
 * Generated: src/modules/_system/generated/module_init.c
 */
void template_module_register(void);

#endif // TEMPLATE_MODULE_H
