/**
 * @file    read_parameter_file.c
 * @brief   YAML parameter file parser using libyaml DOM API
 *
 * Reads YAML configuration files using libyaml's document API (DOM-style).
 * This provides simple tree navigation for configuration parsing.
 *
 * Structure: YAML file -> Document tree -> Navigate sections -> Extract values
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <yaml.h>

#include "config.h"
#include "constants.h"
#include "proto.h"
#include "globals.h"
#include "types.h"
#include "error.h"

/* Helper functions for DOM navigation */
static yaml_node_t *get_mapping_value(yaml_document_t *doc, yaml_node_t *mapping, const char *key);
static const char *get_scalar_value(yaml_node_t *node);
static int get_int_value(yaml_node_t *node);
static double get_double_value(yaml_node_t *node);
static void parse_output_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_input_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_simulation_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_units_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_modules_section(yaml_document_t *doc, yaml_node_t *section);
static void validate_and_postprocess(void);

/**
 * @brief   Read and parse YAML parameter file
 *
 * @param   fname   Path to YAML parameter file
 *
 * Uses libyaml's document API to load the entire YAML file into a DOM tree,
 * then navigates the tree to extract configuration values.
 */
void read_parameter_file(const char *fname) {
  FILE *fh;
  yaml_parser_t parser;
  yaml_document_t document;

  INFO_LOG("Reading YAML parameter file: %s", fname);

  /* Open file */
  fh = fopen(fname, "r");
  if (!fh) {
    ERROR_LOG("Cannot open parameter file '%s'", fname);
    FATAL_ERROR("Failed to open parameter file");
  }

  /* Initialize parser */
  if (!yaml_parser_initialize(&parser)) {
    fclose(fh);
    FATAL_ERROR("Failed to initialize YAML parser");
  }

  yaml_parser_set_input_file(&parser, fh);

  /* Load document (builds DOM tree) */
  if (!yaml_parser_load(&parser, &document)) {
    ERROR_LOG("YAML parse error at line %zu: %s",
              parser.problem_mark.line + 1, parser.problem);
    yaml_parser_delete(&parser);
    fclose(fh);
    FATAL_ERROR("Failed to parse YAML file");
  }

  /* Get root node */
  yaml_node_t *root = yaml_document_get_root_node(&document);
  if (!root || root->type != YAML_MAPPING_NODE) {
    ERROR_LOG("YAML root must be a mapping");
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    fclose(fh);
    FATAL_ERROR("Invalid YAML structure");
  }

  /* Parse each top-level section */
  yaml_node_t *section;

  section = get_mapping_value(&document, root, "output");
  if (section) parse_output_section(&document, section);

  section = get_mapping_value(&document, root, "input");
  if (section) parse_input_section(&document, section);

  section = get_mapping_value(&document, root, "simulation");
  if (section) parse_simulation_section(&document, section);

  section = get_mapping_value(&document, root, "units");
  if (section) parse_units_section(&document, section);

  section = get_mapping_value(&document, root, "modules");
  if (section) parse_modules_section(&document, section);

  /* Cleanup */
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  fclose(fh);

  /* Validate and post-process */
  validate_and_postprocess();

  INFO_LOG("Parameter file '%s' read successfully", fname);
}

/**
 * @brief   Get value node for a key in a mapping
 *
 * @param   doc      YAML document
 * @param   mapping  Mapping node to search
 * @param   key      Key to find
 * @return  Value node, or NULL if not found
 */
static yaml_node_t *get_mapping_value(yaml_document_t *doc, yaml_node_t *mapping, const char *key) {
  if (!mapping || mapping->type != YAML_MAPPING_NODE) {
    return NULL;
  }

  yaml_node_pair_t *pair;
  for (pair = mapping->data.mapping.pairs.start;
       pair < mapping->data.mapping.pairs.top; pair++) {
    yaml_node_t *key_node = yaml_document_get_node(doc, pair->key);
    if (key_node && key_node->type == YAML_SCALAR_NODE) {
      if (strcmp((char *)key_node->data.scalar.value, key) == 0) {
        return yaml_document_get_node(doc, pair->value);
      }
    }
  }
  return NULL;
}

/**
 * @brief   Get scalar value as string
 */
static const char *get_scalar_value(yaml_node_t *node) {
  if (!node || node->type != YAML_SCALAR_NODE) {
    return NULL;
  }
  return (const char *)node->data.scalar.value;
}

/**
 * @brief   Get scalar value as integer
 */
static int get_int_value(yaml_node_t *node) {
  const char *str = get_scalar_value(node);
  return str ? atoi(str) : 0;
}

/**
 * @brief   Get scalar value as double
 */
static double get_double_value(yaml_node_t *node) {
  const char *str = get_scalar_value(node);
  return str ? atof(str) : 0.0;
}

/**
 * @brief   Parse output section
 */
static void parse_output_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  const char *str;

  DEBUG_LOG("Parsing output section");

  node = get_mapping_value(doc, section, "file_base_name");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.OutputFileBaseName, str, MAX_STRING_LEN - 1);
    DEBUG_LOG("OutputFileBaseName = %s", str);
  }

  node = get_mapping_value(doc, section, "directory");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.OutputDir, str, MAX_STRING_LEN - 1);
    DEBUG_LOG("OutputDir = %s", str);
  }

  node = get_mapping_value(doc, section, "snapshot_count");
  if (node) {
    MimicConfig.NOUT = get_int_value(node);
    DEBUG_LOG("NumOutputs = %d", MimicConfig.NOUT);
  }

  node = get_mapping_value(doc, section, "format");
  if (node && (str = get_scalar_value(node))) {
    if (strcasecmp(str, "binary") == 0) {
      MimicConfig.OutputFormat = output_binary;
    } else if (strcasecmp(str, "hdf5") == 0) {
#ifndef HDF5
      ERROR_LOG("OutputFormat 'hdf5' requires HDF5 support");
      FATAL_ERROR("Recompile with 'make USE-HDF5=yes'");
#else
      MimicConfig.OutputFormat = output_hdf5;
#endif
    }
    DEBUG_LOG("OutputFormat = %s", str);
  }

  /* Parse snapshot list array */
  node = get_mapping_value(doc, section, "snapshot_list");
  if (node && node->type == YAML_SEQUENCE_NODE) {
    yaml_node_item_t *item;
    int idx = 0;
    for (item = node->data.sequence.items.start;
         item < node->data.sequence.items.top && idx < ABSOLUTEMAXSNAPS; item++) {
      yaml_node_t *value_node = yaml_document_get_node(doc, *item);
      if (value_node) {
        int snap = get_int_value(value_node);
        MimicConfig.ListOutputSnaps[idx] = snap;
        ListOutputSnaps[idx] = snap;
        DEBUG_LOG("Snapshot[%d] = %d", idx, snap);
        idx++;
      }
    }
  }
}

/**
 * @brief   Parse input section
 */
static void parse_input_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  const char *str;

  DEBUG_LOG("Parsing input section");

  node = get_mapping_value(doc, section, "first_file");
  if (node) {
    MimicConfig.FirstFile = get_int_value(node);
    DEBUG_LOG("FirstFile = %d", MimicConfig.FirstFile);
  }

  node = get_mapping_value(doc, section, "last_file");
  if (node) {
    MimicConfig.LastFile = get_int_value(node);
    DEBUG_LOG("LastFile = %d", MimicConfig.LastFile);
  }

  node = get_mapping_value(doc, section, "tree_name");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.TreeName, str, MAX_STRING_LEN - 1);
    DEBUG_LOG("TreeName = %s", str);
  }

  node = get_mapping_value(doc, section, "tree_type");
  if (node && (str = get_scalar_value(node))) {
    if (strcasecmp(str, "lhalo_binary") == 0) {
      MimicConfig.TreeType = lhalo_binary;
    } else if (strcasecmp(str, "genesis_lhalo_hdf5") == 0) {
#ifndef HDF5
      ERROR_LOG("TreeType '%s' requires HDF5 support", str);
      FATAL_ERROR("Recompile with 'make USE-HDF5=yes'");
#else
      MimicConfig.TreeType = genesis_lhalo_hdf5;
      strncpy(MimicConfig.TreeExtension, ".hdf5", MAX_STRING_LEN - 1);
#endif
    }
    DEBUG_LOG("TreeType = %s", str);
  }

  node = get_mapping_value(doc, section, "simulation_dir");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.SimulationDir, str, MAX_STRING_LEN - 1);
    DEBUG_LOG("SimulationDir = %s", str);
  }

  node = get_mapping_value(doc, section, "snapshot_list_file");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.FileWithSnapList, str, MAX_STRING_LEN - 1);
    DEBUG_LOG("FileWithSnapList = %s", str);
  }

  node = get_mapping_value(doc, section, "last_snapshot");
  if (node) {
    MimicConfig.LastSnapshotNr = get_int_value(node);
    DEBUG_LOG("LastSnapshotNr = %d", MimicConfig.LastSnapshotNr);
  }

  node = get_mapping_value(doc, section, "max_tree_depth");
  if (node) {
    MimicConfig.MaxTreeDepth = get_int_value(node);
    DEBUG_LOG("MaxTreeDepth = %d", MimicConfig.MaxTreeDepth);
  }
}

/**
 * @brief   Parse simulation section
 */
static void parse_simulation_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node, *cosmology;

  DEBUG_LOG("Parsing simulation section");

  /* Parse cosmology subsection */
  cosmology = get_mapping_value(doc, section, "cosmology");
  if (cosmology) {
    node = get_mapping_value(doc, cosmology, "omega_matter");
    if (node) {
      MimicConfig.Omega = get_double_value(node);
      DEBUG_LOG("Omega = %g", MimicConfig.Omega);
    }

    node = get_mapping_value(doc, cosmology, "omega_lambda");
    if (node) {
      MimicConfig.OmegaLambda = get_double_value(node);
      DEBUG_LOG("OmegaLambda = %g", MimicConfig.OmegaLambda);
    }

    node = get_mapping_value(doc, cosmology, "hubble_h");
    if (node) {
      MimicConfig.Hubble_h = get_double_value(node);
      DEBUG_LOG("Hubble_h = %g", MimicConfig.Hubble_h);
    }
  }

  node = get_mapping_value(doc, section, "box_size");
  if (node) {
    MimicConfig.BoxSize = get_double_value(node);
    DEBUG_LOG("BoxSize = %g", MimicConfig.BoxSize);
  }

  node = get_mapping_value(doc, section, "particle_mass");
  if (node) {
    MimicConfig.PartMass = get_double_value(node);
    DEBUG_LOG("PartMass = %g", MimicConfig.PartMass);
  }
}

/**
 * @brief   Parse units section
 */
static void parse_units_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;

  DEBUG_LOG("Parsing units section");

  node = get_mapping_value(doc, section, "length_in_cm");
  if (node) {
    MimicConfig.UnitLength_in_cm = get_double_value(node);
    DEBUG_LOG("UnitLength_in_cm = %g", MimicConfig.UnitLength_in_cm);
  }

  node = get_mapping_value(doc, section, "mass_in_g");
  if (node) {
    MimicConfig.UnitMass_in_g = get_double_value(node);
    DEBUG_LOG("UnitMass_in_g = %g", MimicConfig.UnitMass_in_g);
  }

  node = get_mapping_value(doc, section, "velocity_in_cm_per_s");
  if (node) {
    MimicConfig.UnitVelocity_in_cm_per_s = get_double_value(node);
    DEBUG_LOG("UnitVelocity_in_cm_per_s = %g", MimicConfig.UnitVelocity_in_cm_per_s);
  }
}

/**
 * @brief   Parse modules section
 */
static void parse_modules_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node, *params_node;

  DEBUG_LOG("Parsing modules section");

  /* Parse enabled modules array */
  node = get_mapping_value(doc, section, "enabled");
  if (node && node->type == YAML_SEQUENCE_NODE) {
    yaml_node_item_t *item;
    int idx = 0;
    for (item = node->data.sequence.items.start;
         item < node->data.sequence.items.top && idx < 32; item++) {
      yaml_node_t *value_node = yaml_document_get_node(doc, *item);
      const char *module_name = get_scalar_value(value_node);
      if (module_name) {
        strncpy(MimicConfig.EnabledModules[idx], module_name, MAX_STRING_LEN - 1);
        DEBUG_LOG("EnabledModule[%d] = %s", idx, module_name);
        idx++;
      }
    }
    MimicConfig.NumEnabledModules = idx;
  }

  /* Parse module parameters */
  params_node = get_mapping_value(doc, section, "parameters");
  if (params_node && params_node->type == YAML_MAPPING_NODE) {
    yaml_node_pair_t *module_pair;

    /* Iterate over each module in parameters */
    for (module_pair = params_node->data.mapping.pairs.start;
         module_pair < params_node->data.mapping.pairs.top; module_pair++) {

      yaml_node_t *module_key = yaml_document_get_node(doc, module_pair->key);
      yaml_node_t *module_params = yaml_document_get_node(doc, module_pair->value);

      const char *module_name = get_scalar_value(module_key);
      if (!module_name || !module_params || module_params->type != YAML_MAPPING_NODE) {
        continue;
      }

      /* Iterate over parameters for this module */
      yaml_node_pair_t *param_pair;
      for (param_pair = module_params->data.mapping.pairs.start;
           param_pair < module_params->data.mapping.pairs.top; param_pair++) {

        yaml_node_t *param_key = yaml_document_get_node(doc, param_pair->key);
        yaml_node_t *param_value = yaml_document_get_node(doc, param_pair->value);

        const char *param_name = get_scalar_value(param_key);
        const char *param_val = get_scalar_value(param_value);

        if (param_name && param_val && MimicConfig.NumModuleParams < 256) {
          int idx = MimicConfig.NumModuleParams;
          strncpy(MimicConfig.ModuleParams[idx].module_name, module_name, MAX_STRING_LEN - 1);
          strncpy(MimicConfig.ModuleParams[idx].param_name, param_name, MAX_STRING_LEN - 1);
          strncpy(MimicConfig.ModuleParams[idx].value, param_val, MAX_STRING_LEN - 1);
          DEBUG_LOG("%s_%s = %s (module parameter)", module_name, param_name, param_val);
          MimicConfig.NumModuleParams++;
        }
      }
    }
  }
}

/**
 * @brief   Validate required parameters and post-process
 */
static void validate_and_postprocess(void) {
  int errors = 0;

  /* Check required parameters */
  if (strlen(MimicConfig.OutputDir) == 0) {
    ERROR_LOG("Required parameter 'output.directory' missing");
    errors++;
  }
  if (strlen(MimicConfig.OutputFileBaseName) == 0) {
    ERROR_LOG("Required parameter 'output.file_base_name' missing");
    errors++;
  }
  if (strlen(MimicConfig.SimulationDir) == 0) {
    ERROR_LOG("Required parameter 'input.simulation_dir' missing");
    errors++;
  }
  if (strlen(MimicConfig.TreeName) == 0) {
    ERROR_LOG("Required parameter 'input.tree_name' missing");
    errors++;
  }
  if (strlen(MimicConfig.FileWithSnapList) == 0) {
    ERROR_LOG("Required parameter 'input.snapshot_list_file' missing");
    errors++;
  }
  if (MimicConfig.LastSnapshotNr == 0) {
    ERROR_LOG("Required parameter 'input.last_snapshot' missing or zero");
    errors++;
  }
  if (MimicConfig.BoxSize == 0.0) {
    ERROR_LOG("Required parameter 'simulation.box_size' missing or zero");
    errors++;
  }
  if (MimicConfig.Hubble_h == 0.0) {
    ERROR_LOG("Required parameter 'simulation.cosmology.hubble_h' missing or zero");
    errors++;
  }

  /* Validate ranges */
  if (MimicConfig.LastSnapshotNr < 0 || MimicConfig.LastSnapshotNr >= ABSOLUTEMAXSNAPS) {
    ERROR_LOG("LastSnapshotNr = %d outside valid range [0, %d)",
              MimicConfig.LastSnapshotNr, ABSOLUTEMAXSNAPS);
    errors++;
  }

  if (MimicConfig.NOUT <= 0 || MimicConfig.NOUT > ABSOLUTEMAXSNAPS) {
    ERROR_LOG("NumOutputs = %d outside valid range (1, %d]",
              MimicConfig.NOUT, ABSOLUTEMAXSNAPS);
    errors++;
  }

  if (errors > 0) {
    FATAL_ERROR("Parameter validation failed");
  }

  /* Post-process parameters */

  /* Add trailing slash to OutputDir */
  int len = strlen(MimicConfig.OutputDir);
  if (len > 0 && MimicConfig.OutputDir[len - 1] != '/') {
    strcat(MimicConfig.OutputDir, "/");
  }

  /* Set MAXSNAPS */
  MimicConfig.MAXSNAPS = MimicConfig.LastSnapshotNr + 1;
  SYNC_CONFIG_INT(MAXSNAPS);

  /* Synchronize NOUT */
  SYNC_CONFIG_INT(NOUT);

  /* Log summary */
  INFO_LOG("Configuration: %d output snapshots, %d enabled modules",
           MimicConfig.NOUT, MimicConfig.NumEnabledModules);

  if (MimicConfig.NumEnabledModules > 0) {
    char module_list[MAX_STRING_LEN * 4] = {0};
    for (int i = 0; i < MimicConfig.NumEnabledModules; i++) {
      if (i > 0) strcat(module_list, ", ");
      strcat(module_list, MimicConfig.EnabledModules[i]);
    }
    INFO_LOG("Enabled modules: %s", module_list);
  }
}
