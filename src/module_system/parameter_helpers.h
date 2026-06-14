/**
 * @file    parameter_helpers.h
 * @brief   Helper macros for module parameter loading and validation
 *
 * Provides convenient macros for common parameter patterns:
 * - Loading parameters with automatic error handling
 * - Validating parameter ranges with optional physics context
 * - Combined load-and-validate for simple cases
 *
 * These macros reduce boilerplate while maintaining clarity and flexibility.
 *
 * Vision Principles:
 * - DRY: Eliminates repetitive error-handling code
 * - KISS: Simple macros for common patterns
 * - Type Safety: Compile-time type checking via function calls
 *
 * Usage:
 *   #include "module_system/parameter_helpers.h"
 *
 * Author: Mimic Development Team
 * Date: 2025-12-02
 */

#ifndef PARAMETER_HELPERS_H
#define PARAMETER_HELPERS_H

#include "module_registry.h"
#include "error.h"

/* ==============================================================================
 * PARAMETER LOADING MACROS
 * ============================================================================== */

/**
 * @brief Load a double parameter (with automatic error handling)
 *
 * Calls model_get_double() and returns -1 on failure.
 * Use when you need to validate the parameter separately.
 *
 * Example:
 *   LOAD_PARAM_DOUBLE("BaryonFrac", baryon_frac);
 *   if (baryon_frac <= 0.0 || baryon_frac > 1.0) {
 *     ERROR_LOG("BaryonFrac must be physical (got %.4f)", baryon_frac);
 *     return -1;
 *   }
 */
#define LOAD_PARAM_DOUBLE(name, var)                                                               \
  do {                                                                                             \
    if (model_get_double(name, &var) != 0) {                                                       \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)

#define LOAD_PARAM_DOUBLE_INTERNAL(name, var)                                                      \
  do {                                                                                             \
    if (model_get_double_internal(name, &var) != 0) {                                              \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)

/**
 * @brief Load an int parameter (with automatic error handling)
 *
 * Example:
 *   LOAD_PARAM_INT("ReionizationOn", reionization_on);
 */
#define LOAD_PARAM_INT(name, var)                                                                  \
  do {                                                                                             \
    if (model_get_int(name, &var) != 0) {                                                          \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)

/**
 * @brief Load a string parameter (with automatic error handling)
 *
 * Example:
 *   char cool_dir[MAX_STRING_LEN];
 *   LOAD_PARAM_STRING("CoolFunctionsDir", cool_dir, MAX_STRING_LEN);
 */
#define LOAD_PARAM_STRING(name, var, max_len)                                                      \
  do {                                                                                             \
    if (model_get_string(name, var, max_len) != 0) {                                               \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)

/* ==============================================================================
 * VALIDATION MACROS
 * ============================================================================== */

/**
 * @brief Validate parameter is in exclusive range (min, max]
 *
 * Range: min < value <= max
 *
 * Example:
 *   VALIDATE_RANGE_EXCLUSIVE("BaryonFrac", baryon_frac, 0.0, 1.0, NULL);
 *   VALIDATE_RANGE_EXCLUSIVE("BaryonFrac", baryon_frac, 0.0, 1.0,
 *                             "cosmic baryon fraction must be physical");
 */
#define VALIDATE_RANGE_EXCLUSIVE(param, value, min, max, context)                                  \
  do {                                                                                             \
    if ((value) <= (min) || (value) > (max)) {                                                     \
      if (context != NULL && *context != '\0') {                                                   \
        ERROR_LOG("%s = %.4g out of valid range (%.4g, %.4g] - %s", param, (double)(value),        \
                  (double)(min), (double)(max), context);                                          \
      } else {                                                                                     \
        ERROR_LOG("%s = %.4g out of valid range (%.4g, %.4g]", param, (double)(value),             \
                  (double)(min), (double)(max));                                                   \
      }                                                                                            \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)

/**
 * @brief Validate parameter is in inclusive range [min, max]
 *
 * Range: min <= value <= max
 *
 * Example:
 *   VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", efficiency, 0.0, 1.0, NULL);
 */
#define VALIDATE_RANGE_INCLUSIVE(param, value, min, max, context)                                  \
  do {                                                                                             \
    if ((value) < (min) || (value) > (max)) {                                                      \
      if (context != NULL && *context != '\0') {                                                   \
        ERROR_LOG("%s = %.4g out of valid range [%.4g, %.4g] - %s", param, (double)(value),        \
                  (double)(min), (double)(max), context);                                          \
      } else {                                                                                     \
        ERROR_LOG("%s = %.4g out of valid range [%.4g, %.4g]", param, (double)(value),             \
                  (double)(min), (double)(max));                                                   \
      }                                                                                            \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)

/**
 * @brief Validate integer option is in valid range [0, max_value]
 *
 * For mode selectors and discrete options.
 *
 * Example:
 *   VALIDATE_OPTION("AGNrecipe", agn_recipe, 3, "0=off, 1=radio, 2=quasar, 3=both");
 */
#define VALIDATE_OPTION(param, value, max_value, context)                                          \
  do {                                                                                             \
    if ((value) < 0 || (value) > (max_value)) {                                                    \
      if (context != NULL && *context != '\0') {                                                   \
        ERROR_LOG("%s = %d out of valid range [0, %d] - %s", param, value, max_value, context);    \
      } else {                                                                                     \
        ERROR_LOG("%s = %d out of valid range [0, %d]", param, value, max_value);                  \
      }                                                                                            \
      return -1;                                                                                   \
    }                                                                                              \
  } while (0)

/* ==============================================================================
 * COMBINED LOAD-AND-VALIDATE MACROS
 * ============================================================================== */

/**
 * @brief Load and validate double parameter in one call
 *
 * For parameters with simple range validation, combines loading and validation.
 * If you need complex validation, use LOAD_PARAM_DOUBLE + custom validation.
 *
 * Range: min < value <= max (exclusive lower bound)
 *
 * Example:
 *   LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BaryonFrac", baryon_frac, 0.0, 1.0,
 *                                     "cosmic baryon fraction");
 */
#define LOAD_AND_VALIDATE_RANGE_EXCLUSIVE(param, var, min, max, context)                           \
  do {                                                                                             \
    LOAD_PARAM_DOUBLE(param, var);                                                                 \
    VALIDATE_RANGE_EXCLUSIVE(param, var, min, max, context);                                       \
  } while (0)

/**
 * @brief Load and validate double parameter in one call (inclusive range)
 *
 * Range: min <= value <= max (inclusive bounds)
 *
 * Example:
 *   LOAD_AND_VALIDATE_RANGE_INCLUSIVE("RadioModeEfficiency", efficiency, 0.0, 1.0, NULL);
 */
#define LOAD_AND_VALIDATE_RANGE_INCLUSIVE(param, var, min, max, context)                           \
  do {                                                                                             \
    LOAD_PARAM_DOUBLE(param, var);                                                                 \
    VALIDATE_RANGE_INCLUSIVE(param, var, min, max, context);                                       \
  } while (0)

#define LOAD_AND_VALIDATE_RANGE_INCLUSIVE_INTERNAL(param, var, min, max, context)                  \
  do {                                                                                             \
    LOAD_PARAM_DOUBLE_INTERNAL(param, var);                                                        \
    VALIDATE_RANGE_INCLUSIVE(param, var, min, max, context);                                       \
  } while (0)

/**
 * @brief Load and validate integer option in one call
 *
 * For mode selectors: validates value is in [0, max_value]
 *
 * Example:
 *   LOAD_AND_VALIDATE_OPTION("AGNrecipe", agn_recipe, 3,
 *                            "0=off, 1=radio, 2=quasar, 3=both");
 */
#define LOAD_AND_VALIDATE_OPTION(param, var, max_value, context)                                   \
  do {                                                                                             \
    LOAD_PARAM_INT(param, var);                                                                    \
    VALIDATE_OPTION(param, var, max_value, context);                                               \
  } while (0)

#endif /* PARAMETER_HELPERS_H */
