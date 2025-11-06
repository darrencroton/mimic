/**
 * @file    parameter_table.c
 * @brief   Defines the parameter table for the Mimic framework
 *
 * This file contains the definition of the parameter table, which centralizes
 * all model parameters in a structured format. Each parameter has a name,
 * description, type, storage address, required flag, and valid range values.
 *
 * The parameter system provides a table-driven approach to parameter handling,
 * which allows for consistent parameter reading, validation, and documentation.
 * The core_read_parameter_file.c module uses this table to read parameters
 * from configuration files.
 *
 * Key functions:
 * - get_parameter_table(): Returns the parameter table
 * - get_parameter_table_size(): Returns the number of parameters
 * - is_parameter_valid(): Validates parameter values against their allowed
 * ranges
 * - get_parameter_type_string(): Converts parameter type enum to string
 */

#include "parameters.h"
#include "config.h"
#include "constants.h"
#include "globals.h"
#include "types.h"
#include <stdio.h>
#include <string.h>

static ParameterDefinition ParameterTable[] = {
    // Format: {name, description, type, address, required, min_value,
    // max_value}

    // File information parameters
    {"OutputFileBaseName", "Base name of output files", STRING,
     &MimicConfig.OutputFileBaseName, 1, 0.0, 0.0},
    {"OutputDir", "Directory for output files", STRING, &MimicConfig.OutputDir,
     1, 0.0, 0.0},
    {"TreeName", "Base name of merger tree files", STRING, &MimicConfig.TreeName,
     1, 0.0, 0.0},
    {"TreeType",
     "Type of merger tree files (lhalo_binary or genesis_lhalo_hdf5)", STRING,
     NULL, 1, 0.0, 0.0}, // Special handling needed
    {"OutputFormat", "Output file format (binary or hdf5)", STRING, NULL, 1,
     0.0, 0.0}, // Special handling needed
    {"SimulationDir", "Directory containing simulation data", STRING,
     &MimicConfig.SimulationDir, 1, 0.0, 0.0},
    {"FileWithSnapList", "File containing snapshot list", STRING,
     &MimicConfig.FileWithSnapList, 1, 0.0, 0.0},

    // Simulation parameters
    {"LastSnapshotNr", "Last snapshot number", INT, &MimicConfig.LastSnapshotNr,
     1, 0.0, ABSOLUTEMAXSNAPS - 1},
    {"FirstFile", "First file to process", INT, &MimicConfig.FirstFile, 1, 0.0,
     0.0},
    {"LastFile", "Last file to process", INT, &MimicConfig.LastFile, 1, 0.0,
     0.0},

    // Output parameters
    {"NumOutputs", "Number of outputs (-1 for all snapshots)", INT,
     &MimicConfig.NOUT, 1, -1.0, ABSOLUTEMAXSNAPS},

    // Cosmology parameters
    {"Omega", "Matter density parameter", DOUBLE, &MimicConfig.Omega, 1, 0.0,
     1.0},
    {"OmegaLambda", "Dark energy density parameter", DOUBLE,
     &MimicConfig.OmegaLambda, 1, 0.0, 1.0},
    {"Hubble_h", "Hubble parameter (H0/100)", DOUBLE, &MimicConfig.Hubble_h, 1,
     0.0, 0.0},
    {"PartMass", "Particle mass in simulation", DOUBLE, &MimicConfig.PartMass, 1,
     0.0, 0.0},

    // Unit parameters
    {"UnitVelocity_in_cm_per_s", "Velocity unit in cm/s", DOUBLE,
     &MimicConfig.UnitVelocity_in_cm_per_s, 1, 0.0, 0.0},
    {"UnitLength_in_cm", "Length unit in cm", DOUBLE,
     &MimicConfig.UnitLength_in_cm, 1, 0.0, 0.0},
    {"UnitMass_in_g", "Mass unit in g", DOUBLE, &MimicConfig.UnitMass_in_g, 1,
     0.0, 0.0},

};

static int NParameters = sizeof(ParameterTable) / sizeof(ParameterDefinition);

/**
 * @brief   Returns the parameter table
 *
 * @return  Pointer to the parameter definition table
 *
 * This function returns a pointer to the parameter definition table,
 * which contains information about all model parameters. The table
 * includes parameter names, descriptions, types, storage addresses,
 * required flags, and valid range values.
 *
 * The parameter table is used by the parameter reading code to validate
 * and store parameters from configuration files.
 */
ParameterDefinition *get_parameter_table(void) { return ParameterTable; }

/**
 * @brief   Returns the number of parameters in the table
 *
 * @return  Number of parameters in the parameter table
 *
 * This function returns the count of parameters defined in the
 * parameter table. It's used by the parameter reading code to
 * iterate through all parameters.
 */
int get_parameter_table_size(void) { return NParameters; }

/**
 * @brief   Validates a parameter value against its allowed range
 *
 * @param   param   Pointer to parameter definition
 * @param   value   Pointer to parameter value to validate
 * @return  1 if valid, 0 if invalid
 *
 * This function checks if a parameter value is within the allowed range
 * defined in the parameter table. For numeric types (INT, DOUBLE), it
 * checks min and max values if specified. For STRING type, all values
 * are considered valid.
 *
 * The function is used during parameter reading to ensure that parameter
 * values are within acceptable ranges.
 *
 * @note min_value or max_value of 0.0 means "unbounded". This works correctly
 *       for all current parameters which are positive, but would not work for
 *       parameters that can be negative.
 */
int is_parameter_valid(const ParameterDefinition *param, const void *value) {
  // Strings are always valid (for now)
  if (param->type == STRING)
    return 1;

  // Convert to double for unified numeric comparison
  double val = (param->type == INT) ? (double)(*((const int *)value))
                                    : *((const double *)value);

  // Validate against bounds (0.0 means "no bound set")
  if (param->min_value > 0.0 && val < param->min_value)
    return 0;
  if (param->max_value > 0.0 && val > param->max_value)
    return 0;

  return 1; // Valid
}

/**
 * @brief   Returns string representation of a parameter type
 *
 * @param   type   Parameter type enum value
 * @return  String representation of the parameter type
 *
 * This function converts a parameter type enum value (INT, DOUBLE, STRING)
 * to its string representation. It's used for error messages and debugging
 * to provide human-readable type information.
 *
 * The returned string is statically allocated and should not be freed.
 */
const char *get_parameter_type_string(int type) {
  switch (type) {
  case INT:
    return "INT";
  case DOUBLE:
    return "DOUBLE";
  case STRING:
    return "STRING";
  default:
    return "UNKNOWN";
  }
}
