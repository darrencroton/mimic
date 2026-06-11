/**
 * @file    cooling_tables.c
 * @brief   Metallicity-dependent cooling function tables
 *
 * Loads and interpolates Sutherland & Dopita (1993) cooling tables covering
 * temperatures 10^4 to 10^8.5 K at metallicities from primordial to super-solar.
 *
 * Reference: Sutherland & Dopita (1993), based on SAGE core_cool_func.c
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "cooling_tables.h"
#include "numeric.h"
#include "module_system/physical_constants.h"

#define TABSIZE 91
#define LOG_TEMP_MIN 4.0
#define LOG_TEMP_MAX 8.5
#define LOG_TEMP_STEP 0.05
#define TEMP_INDEX_MAX (TABSIZE - 1)

// Cooling function data files
static const char *cooling_file_names[] = {
    "stripped_mzero.cie", "stripped_m-30.cie", "stripped_m-20.cie", "stripped_m-15.cie",
    "stripped_m-10.cie",  "stripped_m-05.cie", "stripped_m-00.cie", "stripped_m+05.cie"};

// Metallicities [Fe/H] relative to solar
static const double metallicities_feh[8] = {-5.0, // primordial -> effectively -infinity
                                            -3.0, -2.0, -1.5, -1.0, -0.5,
                                            +0.0, // solar
                                            +0.5};

// Absolute log(Z) values, filled at init by adding log10(Z_sun). Kept separate
// from the [Fe/H] source so init/cleanup/init cycles cannot re-shift the table.
static double metallicities[8];

static double CoolRate[8][TABSIZE];
static int tables_initialized = 0;

/**
 * @brief Linear interpolation in temperature for a single metallicity table
 */
static double get_rate(int tab, double logTemp) {
  const double inv_dlogT = 1.0 / LOG_TEMP_STEP;

  if (logTemp < LOG_TEMP_MIN)
    logTemp = LOG_TEMP_MIN;

  int index = (int)((logTemp - LOG_TEMP_MIN) * inv_dlogT);
  if (index >= TEMP_INDEX_MAX)
    index = TEMP_INDEX_MAX - 1;

  const double logTindex = LOG_TEMP_MIN + LOG_TEMP_STEP * index;
  const double rate1 = CoolRate[tab][index];
  const double rate2 = CoolRate[tab][index + 1];

  const double rate = rate1 + (rate2 - rate1) * inv_dlogT * (logTemp - logTindex);

  return rate;
}

/**
 * @brief Initialize and load cooling function tables from data files
 */
int cooling_tables_init(const char *cool_functions_dir) {
  char filepath[512];

  if (tables_initialized) {
    INFO_LOG("Cooling tables already initialized");
    return 0;
  }

  // Convert metallicities from [Fe/H] to absolute log(Z) by adding log10(Z_sun), Z_sun=0.02
  const double log10_zsun = log10(0.02);
  for (int i = 0; i < 8; i++)
    metallicities[i] = metallicities_feh[i] + log10_zsun;

  // Load each cooling function table
  for (int i = 0; i < 8; i++) {
    snprintf(filepath, sizeof(filepath), "%s/%s", cool_functions_dir, cooling_file_names[i]);

    FILE *fd = fopen(filepath, "r");
    if (!fd) {
      ERROR_LOG("Failed to open cooling function file: %s", filepath);
      ERROR_LOG("Please ensure the cooling tables directory is correctly specified");
      ERROR_LOG("Expected location: %s", cool_functions_dir);
      return -1;
    }

    // Read all 91 temperature points - only need column 6 (sd_logLnorm)
    for (int n = 0; n < TABSIZE; n++) {
      float sd_logLnorm;
      const int nitems = fscanf(fd, " %*f %*f %*f %*f %*f %f%*[^\n]", &sd_logLnorm);

      if (nitems != 1) {
        ERROR_LOG("Failed to read cooling table at line %d in file: %s", n + 1, filepath);
        fclose(fd);
        return -1;
      }

      CoolRate[i][n] = sd_logLnorm;
    }

    fclose(fd);
  }

  tables_initialized = 1;
  VERBOSE_LOG("Cooling function tables loaded successfully from %s", cool_functions_dir);

  return 0;
}

/**
 * @brief Get metallicity-dependent cooling rate via 2D interpolation
 */
double get_metaldependent_cooling_rate(double logTemp, double logZ) {
  if (!tables_initialized) {
    ERROR_LOG("Cooling tables not initialized! Call cooling_tables_init() first.");
    return 0.0;
  }

  // Clamp metallicity to table range
  if (logZ < metallicities[0])
    logZ = metallicities[0];

  if (logZ > metallicities[7])
    logZ = metallicities[7];

  // Find metallicity bracket: metallicities[i] <= logZ < metallicities[i+1]
  int i = 0;
  while (logZ > metallicities[i + 1])
    i++;

  // Get cooling rates at this temperature for bracketing metallicities
  const double rate1 = get_rate(i, logTemp);
  const double rate2 = get_rate(i + 1, logTemp);
  const double rate = rate1 + (rate2 - rate1) / (metallicities[i + 1] - metallicities[i]) *
                                  (logZ - metallicities[i]);

  return pow(10.0, rate);
}

/**
 * @brief Free cooling table resources
 */
void cooling_tables_cleanup(void) {
  tables_initialized = 0;
  DEBUG_LOG("Cooling tables cleaned up");
}
