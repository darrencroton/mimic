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
#include "memory.h"             /* For mymalloc_cat, myfree */
#include "module_registry.h"    /* For PhaseModuleConfig and LoopMode */

/* Helper functions for DOM navigation */
static yaml_node_t *get_mapping_value(yaml_document_t *doc, yaml_node_t *mapping, const char *key);
static const char *get_scalar_value(yaml_node_t *node);
static int get_int_value(yaml_node_t *node);
static double get_double_value(yaml_node_t *node);
static void parse_output_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_input_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_simulation_section(yaml_document_t *doc, yaml_node_t *section);
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
  yaml_node_t *node;

  section = get_mapping_value(&document, root, "output");
  if (section) parse_output_section(&document, section);

  section = get_mapping_value(&document, root, "input");
  if (section) parse_input_section(&document, section);

  section = get_mapping_value(&document, root, "simulation");
  if (section) parse_simulation_section(&document, section);

  /* Parse SubSteps (top-level parameter) */
  node = get_mapping_value(&document, root, "SubSteps");
  if (node) {
    MimicConfig.SubSteps = get_int_value(node);
    DEBUG_LOG("SubSteps = %d", MimicConfig.SubSteps);
  } else {
    MimicConfig.SubSteps = 1; /* Default: no sub-stepping */
  }

  section = get_mapping_value(&document, root, "modules");
  if (section) parse_modules_section(&document, section);

  /* Cleanup */
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  fclose(fh);

  /* Validate and post-process */
  validate_and_postprocess();

  VERBOSE_LOG("Parameter file '%s' read successfully", fname);
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

  node = get_mapping_value(doc, section, "output_filename");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.OutputFileBaseName, str, MAX_STRING_LEN - 1);
    DEBUG_LOG("OutputFileBaseName = %s", str);
  }

  node = get_mapping_value(doc, section, "output_directory");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.OutputDir, str, MAX_STRING_LEN - 1);
    DEBUG_LOG("OutputDir = %s", str);
  }

  node = get_mapping_value(doc, section, "snapshot_count");
  if (node) {
    MimicConfig.NOUT = get_int_value(node);
    DEBUG_LOG("NumOutputs = %d", MimicConfig.NOUT);
  }

  node = get_mapping_value(doc, section, "output_format");
  if (node && (str = get_scalar_value(node))) {
    if (strcasecmp(str, "binary") == 0) {
      MimicConfig.OutputFormat = output_binary;
    } else if (strcasecmp(str, "hdf5") == 0) {
#ifndef HDF5
      ERROR_LOG("OutputFormat 'hdf5' requires HDF5 support");
  FATAL_ERROR("Recompile with HDF5 enabled (default) or remove USE-HDF5=no");
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
  FATAL_ERROR("Recompile with HDF5 enabled (default) or remove USE-HDF5=no");
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
 *
 * Parses simulation properties including cosmology and units subsections.
 */
static void parse_simulation_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node, *cosmology, *units;

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

  /* Parse units subsection */
  units = get_mapping_value(doc, section, "units");
  if (units) {
    DEBUG_LOG("Parsing simulation.units subsection");

    node = get_mapping_value(doc, units, "length_in_cm");
    if (node) {
      MimicConfig.UnitLength_in_cm = get_double_value(node);
      DEBUG_LOG("UnitLength_in_cm = %g", MimicConfig.UnitLength_in_cm);
    }

    node = get_mapping_value(doc, units, "mass_in_g");
    if (node) {
      MimicConfig.UnitMass_in_g = get_double_value(node);
      DEBUG_LOG("UnitMass_in_g = %g", MimicConfig.UnitMass_in_g);
    }

    node = get_mapping_value(doc, units, "velocity_in_cm_per_s");
    if (node) {
      MimicConfig.UnitVelocity_in_cm_per_s = get_double_value(node);
      DEBUG_LOG("UnitVelocity_in_cm_per_s = %g", MimicConfig.UnitVelocity_in_cm_per_s);
    }
  }
}

/**
 * @brief   Parse a single module phase configuration
 *
 * Parses YAML like:
 *   phase_name:
 *     - module_a: once
 *     - module_b: all
 *
 * @param   doc         YAML document
 * @param   phase_node  Node for this phase (sequence of module:loop pairs)
 * @param   config      Output: array of PhaseModuleConfig
 * @param   num_modules Output: number of modules in phase
 * @param   phase_name  Phase name for error messages
 * @return  0 on success, -1 on error
 */
static int parse_phase_config(yaml_document_t *doc, yaml_node_t *phase_node,
                              struct PhaseModuleConfig **config,
                              int *num_modules, const char *phase_name) {
  if (!phase_node) {
    *config = NULL;
    *num_modules = 0;
    return 0; /* Empty phase is valid */
  }

  /* Handle scalar nodes (happens when phase has only comments in YAML) */
  if (phase_node->type == YAML_SCALAR_NODE) {
    const char *value = (const char *)phase_node->data.scalar.value;
    if (!value || strlen(value) == 0) {
      /* Empty scalar - treat as empty phase (common when all modules commented out) */
      DEBUG_LOG("Phase '%s' is empty (all modules commented out)", phase_name);
      *config = NULL;
      *num_modules = 0;
      return 0;
    }
    /* Non-empty scalar is an error */
    ERROR_LOG("Phase '%s' must be a sequence (found scalar: '%s')", phase_name, value);
    return -1;
  }

  if (phase_node->type != YAML_SEQUENCE_NODE) {
    ERROR_LOG("Phase '%s' must be a sequence", phase_name);
    return -1;
  }

  /* Count modules */
  *num_modules = 0;
  for (yaml_node_item_t *item = phase_node->data.sequence.items.start;
       item < phase_node->data.sequence.items.top; item++) {
    (*num_modules)++;
  }

  if (*num_modules == 0) {
    *config = NULL;
    return 0; /* Empty phase is valid */
  }

  /* Allocate config array */
  *config = mymalloc_cat(*num_modules * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
  if (!*config) {
    ERROR_LOG("Failed to allocate memory for phase '%s'", phase_name);
    return -1;
  }

  /* Parse each module entry */
  int idx = 0;
  for (yaml_node_item_t *item = phase_node->data.sequence.items.start;
       item < phase_node->data.sequence.items.top; item++) {
    yaml_node_t *module_node = yaml_document_get_node(doc, *item);

    /* Each item should be a mapping with one entry: "module_name: processing_mode" */
    if (module_node->type != YAML_MAPPING_NODE) {
      ERROR_LOG("Phase '%s': module entry must be 'name: mode'", phase_name);
      myfree(*config);
      *config = NULL;
      return -1;
    }

    /* Get the single key-value pair */
    yaml_node_pair_t *pair = module_node->data.mapping.pairs.start;
    if (pair >= module_node->data.mapping.pairs.top) {
      ERROR_LOG("Phase '%s': empty module entry", phase_name);
      myfree(*config);
      *config = NULL;
      return -1;
    }

    yaml_node_t *key = yaml_document_get_node(doc, pair->key);
    yaml_node_t *value = yaml_document_get_node(doc, pair->value);

    const char *module_name = get_scalar_value(key);
    const char *processing_mode_str = get_scalar_value(value);

    if (!module_name || !processing_mode_str) {
      ERROR_LOG("Phase '%s': invalid module entry format", phase_name);
      myfree(*config);
      *config = NULL;
      return -1;
    }

    /* Parse processing mode */
    enum ProcessingMode processing_mode;
    if (strcmp(processing_mode_str, "process_full_halo") == 0) {
      processing_mode = PROCESSING_MODE_FULL_HALO;
    } else if (strcmp(processing_mode_str, "process_per_event") == 0) {
      processing_mode = PROCESSING_MODE_PER_EVENT;
    } else if (strcmp(processing_mode_str, "process_by_galaxy") == 0) {
      processing_mode = PROCESSING_MODE_BY_GALAXY;
    } else {
      ERROR_LOG("Phase '%s': invalid processing mode '%s' (must be "
                "'process_full_halo', 'process_per_event', or "
                "'process_by_galaxy')",
                phase_name, processing_mode_str);
      myfree(*config);
      *config = NULL;
      return -1;
    }

    /* Store in config */
    (*config)[idx].module_name = strdup(module_name);
    (*config)[idx].processing_mode = processing_mode;

    DEBUG_LOG("Phase '%s': %s (processing_mode=%s)", phase_name, module_name,
              processing_mode_str);
    idx++;
  }

  return 0;
}

/**
 * @brief   Parse multi-phase modules section
 *
 * Parses module configuration with multi-phase pipeline structure.
 * Model parameters are ALL physics parameters required by modules.
 * They must be explicitly specified - NO defaults are used.
 *
 * Vision Principle 2 (Runtime Modularity): Pipeline structure configured at runtime.
 * Vision Principle 4 (Single Source of Truth): Input file defines complete model.
 */
static void parse_modules_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node, *parameters;

  DEBUG_LOG("Parsing multi-phase modules section");

  /* Initialize phase configurations to NULL/0 */
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;
  MimicConfig.phase_1 = NULL;
  MimicConfig.num_phase_1 = 0;
  MimicConfig.phase_2 = NULL;
  MimicConfig.num_phase_2 = 0;
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;

  /* Parse each phase */
  node = get_mapping_value(doc, section, "pre_timestep");
  if (parse_phase_config(doc, node, &MimicConfig.pre_timestep,
                         &MimicConfig.num_pre_timestep, "pre_timestep") != 0) {
    FATAL_ERROR("Failed to parse pre_timestep phase");
  }

  node = get_mapping_value(doc, section, "phase_1");
  if (parse_phase_config(doc, node, &MimicConfig.phase_1,
                         &MimicConfig.num_phase_1, "phase_1") != 0) {
    FATAL_ERROR("Failed to parse phase_1");
  }

  node = get_mapping_value(doc, section, "phase_2");
  if (parse_phase_config(doc, node, &MimicConfig.phase_2,
                         &MimicConfig.num_phase_2, "phase_2") != 0) {
    FATAL_ERROR("Failed to parse phase_2");
  }

  node = get_mapping_value(doc, section, "post_timestep");
  if (parse_phase_config(doc, node, &MimicConfig.post_timestep,
                         &MimicConfig.num_post_timestep, "post_timestep") != 0) {
    FATAL_ERROR("Failed to parse post_timestep phase");
  }

  INFO_LOG("Multi-phase pipeline configured:");
  INFO_LOG("  pre_timestep: %d module(s)", MimicConfig.num_pre_timestep);
  INFO_LOG("  phase_1: %d module(s)", MimicConfig.num_phase_1);
  INFO_LOG("  phase_2: %d module(s)", MimicConfig.num_phase_2);
  INFO_LOG("  post_timestep: %d module(s)", MimicConfig.num_post_timestep);

  /* Parse parameters subsection */
  parameters = get_mapping_value(doc, section, "parameters");
  if (parameters) {
    DEBUG_LOG("Parsing modules.parameters subsection");

    /* Parameters are a simple mapping: param_name -> value */
    if (parameters->type != YAML_MAPPING_NODE) {
      ERROR_LOG("modules.parameters section must be a mapping");
      return;
    }

    yaml_node_pair_t *pair;
    int idx = 0;

    /* Iterate over each parameter in the mapping */
    for (pair = parameters->data.mapping.pairs.start;
         pair < parameters->data.mapping.pairs.top && idx < 256; pair++) {

      yaml_node_t *key_node = yaml_document_get_node(doc, pair->key);
      yaml_node_t *value_node = yaml_document_get_node(doc, pair->value);

      const char *param_name = get_scalar_value(key_node);
      const char *param_value = get_scalar_value(value_node);

      if (param_name && param_value) {
        /* Store in ModelParams array */
        strncpy(MimicConfig.ModelParams[idx].param_name, param_name,
                MAX_STRING_LEN - 1);
        strncpy(MimicConfig.ModelParams[idx].value, param_value,
                MAX_STRING_LEN - 1);
        DEBUG_LOG("Module parameter: %s = %s", param_name, param_value);
        idx++;
      }
    }

    MimicConfig.NumModelParams = idx;
    VERBOSE_LOG("Validated %d module parameters", idx);
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
    ERROR_LOG("Required parameter 'output.output_filename' missing");
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

  if (MimicConfig.NOUT != -1 &&
      (MimicConfig.NOUT <= 0 || MimicConfig.NOUT > ABSOLUTEMAXSNAPS)) {
    ERROR_LOG("NumOutputs = %d outside valid range (1, %d] or sentinel -1",
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

  /* When NOUT == -1, select all snapshots and ignore provided list */
  if (MimicConfig.NOUT == -1) {
    MimicConfig.NOUT = MimicConfig.MAXSNAPS;
    for (int i = 0; i < MimicConfig.NOUT && i < ABSOLUTEMAXSNAPS; i++) {
      MimicConfig.ListOutputSnaps[i] = i;
      ListOutputSnaps[i] = i;
    }
    INFO_LOG("All %d snapshots selected for output (NOUT=-1)", MimicConfig.NOUT);
  }

  /* Synchronize NOUT */
  SYNC_CONFIG_INT(NOUT);

  /* Log summary */
  int total_modules = MimicConfig.num_pre_timestep + MimicConfig.num_phase_1 +
                      MimicConfig.num_phase_2 + MimicConfig.num_post_timestep;
  INFO_LOG("Configuration: %d output snapshots, %d module instances across %d phases",
           MimicConfig.NOUT, total_modules, 4);
  INFO_LOG("SubSteps: %d", MimicConfig.SubSteps);
}
