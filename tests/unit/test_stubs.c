/**
 * @file    test_stubs.c
 * @brief   Stub implementations for unit tests
 *
 * Provides minimal implementations of functions from main.c that are
 * needed by unit tests but can't be linked from main.c (which has main()).
 *
 * @author  Mimic Testing Team
 * @date    2025-11-08
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/include/types.h"
#include "../../src/include/globals.h"

/**
 * @brief   Test version of myexit - just call exit()
 *
 * This is a simplified version for tests. The real myexit() in main.c
 * includes MPI cleanup and other teardown logic.
 */
void myexit(int signum) {
    printf("Test exiting with code %d\n", signum);
    exit(signum);
}

/**
 * @brief   Set up all required model parameters with test values
 *
 * Since all model parameters are REQUIRED (no defaults), unit tests must
 * provide all parameters. This helper sets them all to standard test
 * values based on SAGE defaults.
 *
 * Usage in tests:
 *   reset_config();
 *   set_test_model_parameters();
 *   // Module-specific test setup...
 *   module_system_init();
 *
 * @note    Called after reset_config() to populate MimicConfig.ModelParams[]
 */
void set_test_model_parameters(void) {
    int idx = 0;

    /* Cosmological Parameters */
    strcpy(MimicConfig.ModelParams[idx].param_name, "GlobalBaryonFraction");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.17");

    /* Cooling & AGN Feedback */
    strcpy(MimicConfig.ModelParams[idx].param_name, "RadioModeEfficiency");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.01");

    strcpy(MimicConfig.ModelParams[idx].param_name, "AGNrecipe");
    strcpy(MimicConfig.ModelParams[idx++].value, "1");

    /* Star Formation */
    strcpy(MimicConfig.ModelParams[idx].param_name, "SFprescription");
    strcpy(MimicConfig.ModelParams[idx++].value, "0");

    strcpy(MimicConfig.ModelParams[idx].param_name, "SfrEfficiency");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.02");

    /* Stellar Feedback */
    strcpy(MimicConfig.ModelParams[idx].param_name, "SupernovaRecipeOn");
    strcpy(MimicConfig.ModelParams[idx++].value, "1");

    strcpy(MimicConfig.ModelParams[idx].param_name, "FeedbackReheatingEpsilon");
    strcpy(MimicConfig.ModelParams[idx++].value, "3.0");

    strcpy(MimicConfig.ModelParams[idx].param_name, "FeedbackEjectionEfficiency");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.3");

    strcpy(MimicConfig.ModelParams[idx].param_name, "EnergySNcode");
    strcpy(MimicConfig.ModelParams[idx++].value, "1.0");

    strcpy(MimicConfig.ModelParams[idx].param_name, "EtaSNcode");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.5");

    /* Stellar Evolution */
    strcpy(MimicConfig.ModelParams[idx].param_name, "RecycleFraction");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.43");

    strcpy(MimicConfig.ModelParams[idx].param_name, "Yield");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.03");

    strcpy(MimicConfig.ModelParams[idx].param_name, "FracZleaveDisk");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.3");

    /* Reincorporation */
    strcpy(MimicConfig.ModelParams[idx].param_name, "ReIncorporationFactor");
    strcpy(MimicConfig.ModelParams[idx++].value, "1.0");

    /* Mergers */
    strcpy(MimicConfig.ModelParams[idx].param_name, "BlackHoleGrowthRate");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.01");

    strcpy(MimicConfig.ModelParams[idx].param_name, "QuasarModeEfficiency");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.001");

    strcpy(MimicConfig.ModelParams[idx].param_name, "ThreshMajorMerger");
    strcpy(MimicConfig.ModelParams[idx++].value, "0.3");

    /* Disk Instability */
    strcpy(MimicConfig.ModelParams[idx].param_name, "DiskInstabilityOn");
    strcpy(MimicConfig.ModelParams[idx++].value, "1");

    strcpy(MimicConfig.ModelParams[idx].param_name, "DiskRadiusFactor");
    strcpy(MimicConfig.ModelParams[idx++].value, "3.0");

    /* Set count (should be 20) */
    MimicConfig.NumModelParams = idx;
}
