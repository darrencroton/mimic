/**
 * @file    cooling_tables.c
 * @brief   Implementation of metallicity-dependent cooling function tables
 *
 * This file implements the loading and interpolation of cooling function
 * tables for the SAGE cooling model. The tables are based on collisional
 * ionization equilibrium models (Sutherland & Dopita 1993) and cover
 * temperatures from 10^4 K to 10^8.5 K at various metallicities.
 *
 * The implementation follows SAGE's core_cool_func.c but is adapted for
 * Mimic's architecture (using Mimic's memory system, error handling, etc.).
 *
 * References:
 * - Sutherland & Dopita (1993) - Cooling function tables
 * - SAGE core_cool_func.c - Original implementation
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "cooling_tables.h"

#define TABSIZE 91  /* Number of temperature points in each cooling table */

/* Names of the cooling function data files */
static const char *cooling_file_names[] = {
    "stripped_mzero.cie",  /* Primordial composition */
    "stripped_m-30.cie",   /* [Fe/H] = -3.0 */
    "stripped_m-20.cie",   /* [Fe/H] = -2.0 */
    "stripped_m-15.cie",   /* [Fe/H] = -1.5 */
    "stripped_m-10.cie",   /* [Fe/H] = -1.0 */
    "stripped_m-05.cie",   /* [Fe/H] = -0.5 */
    "stripped_m-00.cie",   /* [Fe/H] = 0.0 (solar) */
    "stripped_m+05.cie"    /* [Fe/H] = +0.5 (super-solar) */
};

/* Metallicities in log10(Z) relative to solar, converted to absolute values */
static double metallicities[8] = {
    -5.0,  /* Primordial (effectively -infinity) */
    -3.0,  /* Very metal-poor */
    -2.0,  /* Metal-poor */
    -1.5,  /* Sub-solar */
    -1.0,  /* Sub-solar */
    -0.5,  /* Sub-solar */
    +0.0,  /* Solar */
    +0.5   /* Super-solar */
};

/* Cooling rate tables: [metallicity][temperature] */
static double CoolRate[8][TABSIZE];

/* Flag to track if tables have been initialized */
static int tables_initialized = 0;

/**
 * @brief   Helper function for 1D temperature interpolation within a cooling table
 *
 * @param   tab      Index of the metallicity table (0-7)
 * @param   logTemp  Log10 of temperature in Kelvin
 * @return  Log10 of cooling rate (erg cm^3 s^-1)
 *
 * Performs linear interpolation within a single metallicity cooling table.
 * Temperature range is log(T) = 4.0 to 8.5 with 0.05 dex spacing.
 */
static double get_rate(int tab, double logTemp)
{
    int index;
    double rate1, rate2, rate, logTindex;

    /* Enforce minimum temperature (10^4 K) */
    if (logTemp < 4.0)
        logTemp = 4.0;

    /* Find temperature bin */
    index = (int)((logTemp - 4.0) / 0.05);
    if (index >= 90)
        index = 89;  /* Enforce maximum temperature (10^8.5 K) */

    /* Calculate exact log(T) at the index */
    logTindex = 4.0 + 0.05 * index;

    /* Get cooling rates at bracketing temperature points */
    rate1 = CoolRate[tab][index];
    rate2 = CoolRate[tab][index + 1];

    /* Linear interpolation in log(T) space */
    rate = rate1 + (rate2 - rate1) / 0.05 * (logTemp - logTindex);

    return rate;
}

/**
 * @brief   Initialize and load cooling function tables from data files
 */
int cooling_tables_init(const char *cool_functions_dir)
{
    FILE *fd;
    char filepath[512];
    int i, n;
    float sd_logT, sd_ne, sd_nh, sd_nt, sd_logLnet, sd_logLnorm, sd_logU;
    float sd_logTau, sd_logP12, sd_logRho24, sd_ci, sd_mubar;

    if (tables_initialized) {
        INFO_LOG("Cooling tables already initialized");
        return 0;
    }

    /* Convert metallicities from [Fe/H] to absolute log(Z) by adding log10(Z_sun)
     * where Z_sun = 0.02 (solar metallicity by mass) */
    for (i = 0; i < 8; i++)
        metallicities[i] += log10(0.02);

    /* Load each cooling function table */
    for (i = 0; i < 8; i++) {
        /* Construct full file path */
        snprintf(filepath, sizeof(filepath), "%s/%s", cool_functions_dir,
                 cooling_file_names[i]);

        /* Open cooling table file */
        fd = fopen(filepath, "r");
        if (!fd) {
            ERROR_LOG("Failed to open cooling function file: %s", filepath);
            ERROR_LOG("Please ensure the cooling tables directory is correctly specified");
            ERROR_LOG("Expected location: %s", cool_functions_dir);
            return -1;
        }

        /* Read all 91 temperature points from the table
         * Each line contains 12 columns; we only need column 6 (sd_logLnorm) */
        for (n = 0; n < TABSIZE; n++) {
            int ret = fscanf(fd, " %f %f %f %f %f %f %f %f %f %f %f %f ",
                           &sd_logT, &sd_ne, &sd_nh, &sd_nt, &sd_logLnet,
                           &sd_logLnorm, &sd_logU, &sd_logTau, &sd_logP12,
                           &sd_logRho24, &sd_ci, &sd_mubar);

            if (ret != 12) {
                ERROR_LOG("Failed to read cooling table at line %d in file: %s", n + 1, filepath);
                fclose(fd);
                return -1;
            }

            /* Store the normalized cooling rate (log10(Lambda) in erg cm^3 s^-1) */
            CoolRate[i][n] = sd_logLnorm;
        }

        fclose(fd);
    }

    tables_initialized = 1;
    INFO_LOG("Cooling function tables loaded successfully from %s", cool_functions_dir);
    DEBUG_LOG("  Loaded %d metallicity tables with %d temperature points each", 8, TABSIZE);

    return 0;
}

/**
 * @brief   Get metallicity-dependent cooling rate via 2D interpolation
 */
double get_metaldependent_cooling_rate(double logTemp, double logZ)
{
    int i;
    double rate1, rate2, rate;

    if (!tables_initialized) {
        ERROR_LOG("Cooling tables not initialized! Call cooling_tables_init() first.");
        return 0.0;
    }

    /* Enforce metallicity limits: clamp to table range */
    if (logZ < metallicities[0])
        logZ = metallicities[0];  /* Use primordial if below range */

    if (logZ > metallicities[7])
        logZ = metallicities[7];  /* Use super-solar if above range */

    /* Find metallicity bracket: metallicities[i] <= logZ < metallicities[i+1] */
    i = 0;
    while (i < 7 && logZ > metallicities[i + 1])
        i++;

    /* Get cooling rates at this temperature for bracketing metallicities */
    rate1 = get_rate(i, logTemp);
    rate2 = get_rate(i + 1, logTemp);

    /* Linear interpolation in log(Z) space */
    rate = rate1 + (rate2 - rate1) / (metallicities[i + 1] - metallicities[i]) *
                       (logZ - metallicities[i]);

    /* Convert from log(Lambda) to Lambda */
    return pow(10.0, rate);
}

/**
 * @brief   Free resources allocated for cooling tables
 */
void cooling_tables_cleanup(void)
{
    /* Cooling tables are static arrays, so no dynamic memory to free */
    tables_initialized = 0;
    DEBUG_LOG("Cooling tables cleaned up");
}
