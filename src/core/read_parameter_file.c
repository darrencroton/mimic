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

#ifndef MIMIC_COMPILED_MODEL
#error "MIMIC_COMPILED_MODEL must be set at compile time via -DMIMIC_COMPILED_MODEL=<name>. Use make MODEL=<name>."
#endif

#ifndef MIMIC_COMPILED_MODEL_PATH
#error "MIMIC_COMPILED_MODEL_PATH must be set at compile time via -DMIMIC_COMPILED_MODEL_PATH=<path>. Use make MODEL=<name>."
#endif

#ifndef MIMIC_COMPILED_SIMULATION
#error "MIMIC_COMPILED_SIMULATION must be set at compile time via -DMIMIC_COMPILED_SIMULATION=<name>. Use make SIMULATION=<name>."
#endif

/* Helper functions for DOM navigation */
static yaml_node_t *get_mapping_value(yaml_document_t *doc, yaml_node_t *mapping, const char *key);
static const char *get_scalar_value(yaml_node_t *node);
static int get_int_value(yaml_node_t *node);
static double get_double_value(yaml_node_t *node);
static void parse_output_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_input_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_model_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_simulation_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_plotting_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_modules_section(yaml_document_t *doc, yaml_node_t *section);
static void validate_and_postprocess(void);
static void parse_simulation_config_file(const char *fname);
static void resolve_config_path(const char *path, const char *param_file,
                                char *resolved, size_t resolved_size);
static int file_exists_readable(const char *path);

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

  yaml_node_t *section;
  yaml_node_t *node;
  const char *str;

  /*
   * Load order: simulation config file first (provides defaults), then all
   * sections from the run file (may override those defaults). This means any
   * field present in both files takes the value from the run file.
   *
   * The simulation config path lives inside the run file's simulation section,
   * so we extract just that key before anything else, load the sim config, then
   * parse the full run file on top.
   */
  section = get_mapping_value(&document, root, "simulation");
  if (section) {
    node = get_mapping_value(&document, section, "config");
    if (node && (str = get_scalar_value(node))) {
      resolve_config_path(str, fname, MimicConfig.SimulationConfigPath,
                          sizeof(MimicConfig.SimulationConfigPath));
      parse_simulation_config_file(MimicConfig.SimulationConfigPath);
    }
  }

  section = get_mapping_value(&document, root, "output");
  if (section) parse_output_section(&document, section);

  section = get_mapping_value(&document, root, "input");
  if (section) parse_input_section(&document, section);

  section = get_mapping_value(&document, root, "model");
  if (section) parse_model_section(&document, section);

  section = get_mapping_value(&document, root, "simulation");
  if (section) parse_simulation_section(&document, section);

  section = get_mapping_value(&document, root, "plotting");
  if (section) parse_plotting_section(&document, section);

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
 * @brief   Check whether a file can be opened for reading.
 */
static int file_exists_readable(const char *path) {
  FILE *fh = fopen(path, "r");
  if (!fh) {
    return 0;
  }
  fclose(fh);
  return 1;
}

/**
 * @brief   Resolve a run-file path with param-file-relative fallback.
 *
 * Absolute paths are used as-is. Relative paths are first tried exactly as
 * provided (normally repository-root-relative because runs are launched from the
 * repo root), then relative to the parameter file's parent directory. The
 * returned path is stored for metadata copying and later diagnostics.
 */
static void resolve_config_path(const char *path, const char *param_file,
                                char *resolved, size_t resolved_size) {
  const char *last_slash;
  int written;
  char param_dir[MAX_STRING_LEN];
  char candidate[MAX_STRING_LEN];

  if (path == NULL || path[0] == '\0') {
    resolved[0] = '\0';
    return;
  }

  if (path[0] == '/' || file_exists_readable(path)) {
    written = snprintf(resolved, resolved_size, "%s", path);
    if (written < 0 || (size_t)written >= resolved_size) {
      FATAL_ERROR("Simulation config path too long: %s", path);
    }
    return;
  }

  last_slash = strrchr(param_file, '/');
  if (last_slash == NULL) {
    written = snprintf(resolved, resolved_size, "%s", path);
    if (written < 0 || (size_t)written >= resolved_size) {
      FATAL_ERROR("Simulation config path too long: %s", path);
    }
    return;
  }

  written = snprintf(param_dir, sizeof(param_dir), "%.*s",
                     (int)(last_slash - param_file), param_file);
  if (written < 0 || (size_t)written >= sizeof(param_dir)) {
    FATAL_ERROR("Parameter file path too long while resolving '%s'", path);
  }

  written = snprintf(candidate, sizeof(candidate), "%s/%s", param_dir, path);
  if (written < 0 || (size_t)written >= sizeof(candidate)) {
    FATAL_ERROR("Resolved simulation config path too long: %s/%s", param_dir,
                path);
  }

  written = snprintf(resolved, resolved_size, "%s", candidate);
  if (written < 0 || (size_t)written >= resolved_size) {
    FATAL_ERROR("Resolved simulation config path too long: %s", candidate);
  }
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
 * @brief   Parse model package metadata.
 */
static void parse_model_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  const char *str;

  DEBUG_LOG("Parsing model section");

  node = get_mapping_value(doc, section, "name");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.ModelName, str, MAX_STRING_LEN - 1);
  }

  node = get_mapping_value(doc, section, "path");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.ModelPath, str, MAX_STRING_LEN - 1);
  }

  node = get_mapping_value(doc, section, "properties");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.ModelPropertiesPath, str, MAX_STRING_LEN - 1);
  }
}

/**
 * @brief   Parse simulation section
 *
 * Parses simulation properties including cosmology and units subsections.
 */
static void parse_simulation_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node, *cosmology, *units;
  const char *str;

  DEBUG_LOG("Parsing simulation section");

  node = get_mapping_value(doc, section, "name");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.SimulationName, str, MAX_STRING_LEN - 1);
  }

  node = get_mapping_value(doc, section, "path");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.SimulationPath, str, MAX_STRING_LEN - 1);
  }

  node = get_mapping_value(doc, section, "halo_properties");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.SimulationHaloPropertiesPath, str, MAX_STRING_LEN - 1);
  }

  node = get_mapping_value(doc, section, "config");
  if (node && (str = get_scalar_value(node))) {
    if (strlen(MimicConfig.SimulationConfigPath) == 0) {
      strncpy(MimicConfig.SimulationConfigPath, str, MAX_STRING_LEN - 1);
      MimicConfig.SimulationConfigPath[MAX_STRING_LEN - 1] = '\0';
    }
    /* Simulation config is pre-loaded before run-file sections so that run-file
     * values override simulation defaults. No second load here. */
  }

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
 * @brief   Parse plotting package metadata.
 */
static void parse_plotting_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  const char *str;

  DEBUG_LOG("Parsing plotting section");

  node = get_mapping_value(doc, section, "profile");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.PlottingProfilePath, str, MAX_STRING_LEN - 1);
  }
}

/**
 * @brief   Load simulation-owned input and physical metadata from YAML.
 */
static void parse_simulation_config_file(const char *fname) {
  FILE *fh;
  yaml_parser_t parser;
  yaml_document_t document;

  INFO_LOG("Reading simulation config file: %s", fname);

  fh = fopen(fname, "r");
  if (!fh) {
    ERROR_LOG("Cannot open simulation config file '%s'", fname);
    FATAL_ERROR("Failed to open simulation config file");
  }

  if (!yaml_parser_initialize(&parser)) {
    fclose(fh);
    FATAL_ERROR("Failed to initialize YAML parser");
  }

  yaml_parser_set_input_file(&parser, fh);
  if (!yaml_parser_load(&parser, &document)) {
    ERROR_LOG("YAML parse error in simulation config at line %zu: %s",
              parser.problem_mark.line + 1, parser.problem);
    yaml_parser_delete(&parser);
    fclose(fh);
    FATAL_ERROR("Failed to parse simulation config file");
  }

  yaml_node_t *root = yaml_document_get_root_node(&document);
  if (!root || root->type != YAML_MAPPING_NODE) {
    ERROR_LOG("Simulation config root must be a mapping");
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);
    fclose(fh);
    FATAL_ERROR("Invalid simulation config structure");
  }

  yaml_node_t *section = get_mapping_value(&document, root, "input");
  if (section) parse_input_section(&document, section);

  section = get_mapping_value(&document, root, "simulation");
  if (section) parse_simulation_section(&document, section);

  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  fclose(fh);
}

/**
 * @brief   Parse a single module phase configuration
 *
 * Parses YAML like:
 *   phase_name:
 *     - module_a: process_full_halo
 *     - module_b: process_by_galaxy
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
/**
 * @brief   Reject substep phase names that collide with reserved keys.
 *
 * @return  1 if the name is reserved, 0 otherwise
 */
static int phase_name_is_reserved(const char *name) {
  static const char *reserved[] = {"pre_timestep", "post_timestep", "parameters",
                                   "phases"};
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
    if (strcmp(name, reserved[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

/**
 * @brief   Append one parsed middle phase to MimicConfig.substep_phases.
 *
 * Validates the phase name (non-empty, not reserved, unique) before parsing the
 * module sequence. Fatal on any violation so misconfiguration fails at startup.
 */
static void add_substep_phase(yaml_document_t *doc, const char *name,
                              yaml_node_t *phase_node) {
  if (name == NULL || name[0] == '\0') {
    FATAL_ERROR("Substep phase name must be a non-empty string");
  }
  if (strlen(name) >= MAX_STRING_LEN) {
    FATAL_ERROR("Substep phase name '%s' is too long (max %d characters)", name,
                MAX_STRING_LEN - 1);
  }
  if (phase_name_is_reserved(name)) {
    FATAL_ERROR("Substep phase name '%s' is reserved", name);
  }
  for (int i = 0; i < MimicConfig.num_substep_phases; i++) {
    if (strcmp(MimicConfig.substep_phases[i].name, name) == 0) {
      FATAL_ERROR("Duplicate substep phase name '%s'", name);
    }
  }
  if (MimicConfig.num_substep_phases >= MAX_SUBSTEP_PHASES) {
    FATAL_ERROR("Too many substep phases (max %d)", MAX_SUBSTEP_PHASES);
  }

  struct ModulePhaseConfig *phase =
      &MimicConfig.substep_phases[MimicConfig.num_substep_phases];
  phase->name = strdup(name);
  if (phase->name == NULL) {
    FATAL_ERROR("Failed to allocate substep phase name '%s'", name);
  }
  if (parse_phase_config(doc, phase_node, &phase->modules, &phase->num_modules,
                         name) != 0) {
    FATAL_ERROR("Failed to parse substep phase '%s'", name);
  }
  MimicConfig.num_substep_phases++;
}

static void parse_modules_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node, *parameters;

  DEBUG_LOG("Parsing multi-phase modules section");

  /* Initialize phase configurations to NULL/0 */
  MimicConfig.pre_timestep = NULL;
  MimicConfig.num_pre_timestep = 0;
  MimicConfig.substep_phases = NULL;
  MimicConfig.num_substep_phases = 0;
  MimicConfig.post_timestep = NULL;
  MimicConfig.num_post_timestep = 0;

  /* Fixed lifecycle phases */
  node = get_mapping_value(doc, section, "pre_timestep");
  if (parse_phase_config(doc, node, &MimicConfig.pre_timestep,
                         &MimicConfig.num_pre_timestep, "pre_timestep") != 0) {
    FATAL_ERROR("Failed to parse pre_timestep phase");
  }

  node = get_mapping_value(doc, section, "post_timestep");
  if (parse_phase_config(doc, node, &MimicConfig.post_timestep,
                         &MimicConfig.num_post_timestep, "post_timestep") != 0) {
    FATAL_ERROR("Failed to parse post_timestep phase");
  }

  /* Reject any unrecognised key under modules: so stale or mistyped pipelines
   * (e.g. the removed phase_1/phase_2/enabled forms) fail loudly at startup
   * rather than silently dropping physics. */
  for (yaml_node_pair_t *pair = section->data.mapping.pairs.start;
       pair < section->data.mapping.pairs.top; pair++) {
    yaml_node_t *key = yaml_document_get_node(doc, pair->key);
    const char *key_name = get_scalar_value(key);
    if (key_name == NULL) {
      continue;
    }
    if (strcmp(key_name, "pre_timestep") != 0 &&
        strcmp(key_name, "post_timestep") != 0 &&
        strcmp(key_name, "phases") != 0 &&
        strcmp(key_name, "parameters") != 0) {
      FATAL_ERROR("Unknown key 'modules.%s'; supported keys are pre_timestep, "
                  "phases, post_timestep, parameters",
                  key_name);
    }
  }

  /* Middle phases: an ordered mapping of user-named phases under 'phases:'.
   * Absent 'phases:' simply means no per-substep middle phases. */
  yaml_node_t *phases_node = get_mapping_value(doc, section, "phases");

  MimicConfig.substep_phases =
      mymalloc_cat(MAX_SUBSTEP_PHASES * sizeof(struct ModulePhaseConfig),
                   MEM_UTILITY);
  if (MimicConfig.substep_phases == NULL) {
    FATAL_ERROR("Failed to allocate substep phase array");
  }

  if (phases_node != NULL) {
    if (phases_node->type != YAML_MAPPING_NODE) {
      FATAL_ERROR("modules.phases must be a mapping of phase_name -> module list");
    }
    /* libyaml preserves mapping order, so phases run in declared order. */
    for (yaml_node_pair_t *pair = phases_node->data.mapping.pairs.start;
         pair < phases_node->data.mapping.pairs.top; pair++) {
      yaml_node_t *key = yaml_document_get_node(doc, pair->key);
      yaml_node_t *value = yaml_document_get_node(doc, pair->value);
      add_substep_phase(doc, get_scalar_value(key), value);
    }
  }

  /* Release the unused array in physics-free / no-middle-phase runs so cleanup
   * (which short-circuits when no modules are configured) cannot leak it. */
  if (MimicConfig.num_substep_phases == 0) {
    myfree(MimicConfig.substep_phases);
    MimicConfig.substep_phases = NULL;
  }

  INFO_LOG("Multi-phase pipeline configured:");
  INFO_LOG("  pre_timestep: %d module(s)", MimicConfig.num_pre_timestep);
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    INFO_LOG("  %s: %d module(s)", MimicConfig.substep_phases[p].name,
             MimicConfig.substep_phases[p].num_modules);
  }
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
  if (strlen(MimicConfig.ModelName) == 0) {
    ERROR_LOG("Required parameter 'model.name' missing");
    errors++;
  }
  if (strlen(MimicConfig.ModelPath) == 0) {
    ERROR_LOG("Required parameter 'model.path' missing");
    errors++;
  }
  if (strlen(MimicConfig.ModelPropertiesPath) == 0) {
    ERROR_LOG("Required parameter 'model.properties' missing");
    errors++;
  }
  if (strlen(MimicConfig.ModelName) > 0 &&
      strcmp(MimicConfig.ModelName, MIMIC_COMPILED_MODEL) != 0) {
    ERROR_LOG("Run file selects model.name='%s' but this executable was built "
              "with MODEL=%s",
              MimicConfig.ModelName, MIMIC_COMPILED_MODEL);
    errors++;
  }
  if (strlen(MimicConfig.ModelPath) > 0 &&
      strcmp(MimicConfig.ModelPath, MIMIC_COMPILED_MODEL_PATH) != 0) {
    ERROR_LOG("Run file selects model.path='%s' but this executable was built "
              "with MODEL=%s (%s)",
              MimicConfig.ModelPath, MIMIC_COMPILED_MODEL,
              MIMIC_COMPILED_MODEL_PATH);
    errors++;
  }
  if (strlen(MimicConfig.SimulationName) == 0) {
    ERROR_LOG("Required parameter 'simulation.name' missing");
    errors++;
  }
  /* The compiled property schema is generated from one simulation's
   * halo_properties.yaml (selected with make SIMULATION=<name>). The run file's
   * simulation.name is a free-form label and may legitimately differ (for
   * example a test fixture reusing the millennium catalog as 'test_millennium').
   * What must match is the property package: the parent directory of the
   * declared simulation.halo_properties path must equal MIMIC_COMPILED_SIMULATION,
   * otherwise the run would be interpreted with a schema it was not built for. */
  if (strlen(MimicConfig.SimulationHaloPropertiesPath) > 0) {
    const char *path = MimicConfig.SimulationHaloPropertiesPath;
    const char *file_slash = strrchr(path, '/');
    const char *pkg = NULL;
    size_t pkg_len = 0;
    if (file_slash != NULL && file_slash != path) {
      const char *dir_start = file_slash - 1;
      while (dir_start > path && *dir_start != '/') {
        dir_start--;
      }
      if (*dir_start == '/') {
        dir_start++;
      }
      pkg = dir_start;
      pkg_len = (size_t)(file_slash - dir_start);
    }
    const char *compiled = MIMIC_COMPILED_SIMULATION;
    if (pkg == NULL || pkg_len != strlen(compiled) ||
        strncmp(pkg, compiled, pkg_len) != 0) {
      ERROR_LOG("Run file's simulation.halo_properties='%s' belongs to a "
                "different simulation package than this executable, which was "
                "built with SIMULATION=%s (expected path under simulations/%s/)",
                path, compiled, compiled);
      errors++;
    }
  }
  if (strlen(MimicConfig.SimulationPath) == 0) {
    ERROR_LOG("Required parameter 'simulation.path' missing");
    errors++;
  }
  if (strlen(MimicConfig.SimulationConfigPath) == 0) {
    ERROR_LOG("Required parameter 'simulation.config' missing");
    errors++;
  }
  if (strlen(MimicConfig.SimulationHaloPropertiesPath) == 0) {
    ERROR_LOG("Required parameter 'simulation.halo_properties' missing");
    errors++;
  }
  if (strlen(MimicConfig.PlottingProfilePath) > 0 &&
      MimicConfig.PlottingProfilePath[0] == '/') {
    ERROR_LOG("plotting.profile must be package-relative, not absolute");
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
  int total_modules = MimicConfig.num_pre_timestep + MimicConfig.num_post_timestep;
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    total_modules += MimicConfig.substep_phases[p].num_modules;
  }
  int total_phases = MimicConfig.num_substep_phases + 2; /* pre + post */
  INFO_LOG("Configuration: %d output snapshots, %d module instances across %d phases",
           MimicConfig.NOUT, total_modules, total_phases);
  INFO_LOG("SubSteps: %d", MimicConfig.SubSteps);
}
