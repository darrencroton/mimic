/**
 * @file    template_module.h
 * @brief   Template physics module interface
 *
 * USAGE: Copy to src/modules/YOUR_MODULE_NAME/, rename files, replace
 * "template_module" with "your_module", then customize.
 *
 * Steps:
 * 1. Copy: cp -r src/modules/_system/template src/modules/your_module_name
 * 2. Rename: template_module.h → your_module.h, template_module.c → your_module.c
 * 3. Replace: "template_module" → "your_module", "TEMPLATE_MODULE" → "YOUR_MODULE"
 * 4. Update this header with your module description
 * 5. Create module_info.yaml (use template_module_info.yaml as guide)
 * 6. Implement physics in .c file
 * 7. Run 'make generate' to auto-register
 *
 * ---
 * DELETE ABOVE SECTION AFTER CUSTOMIZING
 * ---
 *
 * [YOUR MODULE NAME]
 *
 * [Brief description of physics process - 1-2 sentences]
 *
 * Physics: [Key equation or principle]
 *
 * Reference: [Citation and/or SAGE source file]
 */

#ifndef TEMPLATE_MODULE_H
#define TEMPLATE_MODULE_H

/**
 * @brief   Register the template module with the module registry
 */
void template_module_register(void);

#endif // TEMPLATE_MODULE_H
