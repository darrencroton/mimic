/**
 * @file    sage_agn_recipe.h
 * @brief   The AGNrecipe values the sage26 package supports, and their validator
 *
 * AGNrecipe is the package's name for SAGE26's AGNrecipeOn. The package supports two of its
 * values: 0, which turns the radio mode off and stops black-hole growth in mergers and disk
 * instabilities, and 2, SAGE26's Bondi-Hoyle radio-mode accretion. SAGE26's 1 (the empirical
 * rate) and 3 (cold cloud accretion) are not supported; they are refused at initialisation with
 * every other value.
 *
 * Every module that reads AGNrecipe (sage26_radio_mode_heating, sage26_regime_aware_cooling,
 * sage26_quasar_mode, and sage26_starburst_feedback when its post-merger recheck grows the black
 * hole) loads the required parameter at initialisation and passes it here, so the four agree.
 */

#ifndef MIMIC_SHARED_SAGE_AGN_RECIPE_H
#define MIMIC_SHARED_SAGE_AGN_RECIPE_H

#include "error.h"

/** @brief AGNrecipe 0: no radio-mode heating and no black-hole growth */
#define MIMIC_SAGE_AGN_RECIPE_OFF 0
/** @brief AGNrecipe 2: SAGE26's Bondi-Hoyle radio-mode accretion */
#define MIMIC_SAGE_AGN_RECIPE_BONDI 2

/**
 * @brief Refuse an AGNrecipe the package does not support
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded AGNrecipe
 * @return 0 for 0 or 2; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_agn_recipe(const char *module_name, const int value) {
  if (value != MIMIC_SAGE_AGN_RECIPE_OFF && value != MIMIC_SAGE_AGN_RECIPE_BONDI) {
    ERROR_LOG("%s: AGNrecipe = %d is not supported: the package accepts only 0 (off) and 2 "
              "(SAGE26's Bondi-Hoyle accretion); SAGE26's 1 (empirical) and 3 (cold cloud) are "
              "refused",
              module_name, value);
    return -1;
  }
  return 0;
}

#endif /* MIMIC_SHARED_SAGE_AGN_RECIPE_H */
