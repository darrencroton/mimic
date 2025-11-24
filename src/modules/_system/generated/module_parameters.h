/* AUTO-GENERATED FILE - DO NOT EDIT MANUALLY */
/* Generated from module metadata by scripts/generate_module_registry.py */
/* Source: src/modules/[MODULE]/module_info.yaml */
/*
 * This file provides compile-time parameter metadata for automatic
 * validation. Ranges are defined once in module_info.yaml (single
 * source of truth) and generated into this lookup table.
 *
 * To regenerate:
 *   make generate
 *
 * Generated: 2025-11-24 21:19:05
 * Source MD5: 321af7d0aca96d6a05f4f33f000bb739
 */

#ifndef MODULE_PARAMETERS_H
#define MODULE_PARAMETERS_H

/**
 * @brief Parameter metadata for automatic validation and defaults
 *
 * Each entry represents a parameter from module_info.yaml with its
 * default value and validation range. Used by module_get_double/int to
 * automatically provide defaults and validate parameter values.
 *
 * Vision Principle 4 (Single Source of Truth): Defaults and ranges
 * defined once in module_info.yaml, not hardcoded in C.
 */
struct ModuleParameterMetadata {
    const char *module_name;  /* Module name (e.g., 'sage_infall') */
    const char *param_name;   /* Parameter name (e.g., 'BaryonFrac') */
    const char *type;         /* Type: 'double', 'int', 'string' */
    double default_value;     /* Default value from metadata */
    double range_min;         /* Minimum valid value */
    double range_max;         /* Maximum valid value */
    int has_min;              /* 1 if min constraint exists, 0 otherwise */
    int has_max;              /* 1 if max constraint exists, 0 otherwise */
};

/* Parameter metadata lookup table */
static const struct ModuleParameterMetadata MODULE_PARAMETER_METADATA[] = {
    {"sage_cooling", "AGNrecipeOn", "int", 1.0, 0.0, 3.0, 1, 1},  /* sage_cooling_AGNrecipeOn = 1.0 */
    {"sage_cooling", "CoolFunctionsDir", "string", 0.0, 0.0, 0.0, 0, 0},  /* sage_cooling_CoolFunctionsDir = 0.0 */
    {"sage_cooling", "RadioModeEfficiency", "double", 0.01, 0.0, 1.0, 1, 1},  /* sage_cooling_RadioModeEfficiency = 0.01 */
    {"sage_disk_instability", "DiskInstabilityOn", "int", 1.0, 0.0, 1.0, 1, 1},  /* sage_disk_instability_DiskInstabilityOn = 1.0 */
    {"sage_disk_instability", "DiskRadiusFactor", "double", 3.0, 1.0, 10.0, 1, 1},  /* sage_disk_instability_DiskRadiusFactor = 3.0 */
    {"sage_infall", "BaryonFrac", "double", 0.17, 0.0, 1.0, 1, 1},  /* sage_infall_BaryonFrac = 0.17 */
    {"sage_infall", "ReionizationOn", "int", 1.0, 0.0, 1.0, 1, 1},  /* sage_infall_ReionizationOn = 1.0 */
    {"sage_infall", "Reionization_z0", "double", 8.0, 0.0, 20.0, 1, 1},  /* sage_infall_Reionization_z0 = 8.0 */
    {"sage_infall", "Reionization_zr", "double", 7.0, 0.0, 20.0, 1, 1},  /* sage_infall_Reionization_zr = 7.0 */
    {"sage_mergers", "AGNrecipeOn", "int", 1.0, 0.0, 1.0, 1, 1},  /* sage_mergers_AGNrecipeOn = 1.0 */
    {"sage_mergers", "BlackHoleGrowthRate", "double", 0.01, 0.0, 1.0, 1, 1},  /* sage_mergers_BlackHoleGrowthRate = 0.01 */
    {"sage_mergers", "DiskInstabilityOn", "int", 0.0, 0.0, 1.0, 1, 1},  /* sage_mergers_DiskInstabilityOn = 0.0 */
    {"sage_mergers", "FeedbackEjectionEfficiency", "double", 0.3, 0.0, 10.0, 1, 1},  /* sage_mergers_FeedbackEjectionEfficiency = 0.3 */
    {"sage_mergers", "FeedbackReheatingEpsilon", "double", 3.0, 0.0, 10.0, 1, 1},  /* sage_mergers_FeedbackReheatingEpsilon = 3.0 */
    {"sage_mergers", "FracZleaveDisk", "double", 0.3, 0.0, 1.0, 1, 1},  /* sage_mergers_FracZleaveDisk = 0.3 */
    {"sage_mergers", "QuasarModeEfficiency", "double", 0.001, 0.0, 1.0, 1, 1},  /* sage_mergers_QuasarModeEfficiency = 0.001 */
    {"sage_mergers", "RecycleFraction", "double", 0.43, 0.0, 1.0, 1, 1},  /* sage_mergers_RecycleFraction = 0.43 */
    {"sage_mergers", "SupernovaRecipeOn", "int", 1.0, 0.0, 1.0, 1, 1},  /* sage_mergers_SupernovaRecipeOn = 1.0 */
    {"sage_mergers", "ThreshMajorMerger", "double", 0.3, 0.0, 1.0, 1, 1},  /* sage_mergers_ThreshMajorMerger = 0.3 */
    {"sage_mergers", "Yield", "double", 0.03, 0.0, 0.1, 1, 1},  /* sage_mergers_Yield = 0.03 */
    {"sage_reincorporation", "ReIncorporationFactor", "double", 1.0, 0.0, 10.0, 1, 1},  /* sage_reincorporation_ReIncorporationFactor = 1.0 */
    {"sage_starformation_feedback", "DiskInstabilityOn", "int", 0.0, 0.0, 1.0, 1, 1},  /* sage_starformation_feedback_DiskInstabilityOn = 0.0 */
    {"sage_starformation_feedback", "EnergySNcode", "double", 1.0, 0.0, 100.0, 1, 1},  /* sage_starformation_feedback_EnergySNcode = 1.0 */
    {"sage_starformation_feedback", "EtaSNcode", "double", 0.5, 0.0, 10.0, 1, 1},  /* sage_starformation_feedback_EtaSNcode = 0.5 */
    {"sage_starformation_feedback", "FeedbackEjectionEfficiency", "double", 0.3, 0.0, 100.0, 1, 1},  /* sage_starformation_feedback_FeedbackEjectionEfficiency = 0.3 */
    {"sage_starformation_feedback", "FeedbackReheatingEpsilon", "double", 3.0, 0.0, 100.0, 1, 1},  /* sage_starformation_feedback_FeedbackReheatingEpsilon = 3.0 */
    {"sage_starformation_feedback", "FracZleaveDisk", "double", 0.3, 0.0, 1.0, 1, 1},  /* sage_starformation_feedback_FracZleaveDisk = 0.3 */
    {"sage_starformation_feedback", "RecycleFraction", "double", 0.43, 0.0, 1.0, 1, 1},  /* sage_starformation_feedback_RecycleFraction = 0.43 */
    {"sage_starformation_feedback", "SFprescription", "int", 0.0, 0.0, 0.0, 1, 1},  /* sage_starformation_feedback_SFprescription = 0.0 */
    {"sage_starformation_feedback", "SfrEfficiency", "double", 0.02, 0.0, 1.0, 1, 1},  /* sage_starformation_feedback_SfrEfficiency = 0.02 */
    {"sage_starformation_feedback", "SupernovaRecipeOn", "int", 1.0, 0.0, 1.0, 1, 1},  /* sage_starformation_feedback_SupernovaRecipeOn = 1.0 */
    {"sage_starformation_feedback", "Yield", "double", 0.03, 0.0, 1.0, 1, 1},  /* sage_starformation_feedback_Yield = 0.03 */
    {"test_fixture", "DummyParameter", "double", 1.0, 0.0, 10.0, 1, 1},  /* test_fixture_DummyParameter = 1.0 */
    {"test_fixture", "EnableLogging", "int", 0.0, 0.0, 1.0, 1, 1},  /* test_fixture_EnableLogging = 0.0 */
};

#define NUM_MODULE_PARAMETERS 34  /* Total parameters with metadata */

#endif /* MODULE_PARAMETERS_H */
