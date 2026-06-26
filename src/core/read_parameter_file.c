/**
 * @file    read_parameter_file.c
 * @brief   YAML parameter file parser using libyaml DOM API
 *
 * Reads YAML configuration files using libyaml's document API (DOM-style).
 * This provides simple tree navigation for configuration parsing.
 *
 * Structure: YAML file -> Document tree -> Navigate sections -> Extract values
 */

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp */
#include <yaml.h>

#include "config.h"
#include "constants.h"
#include "error.h"
#include "globals.h"
#include "memory.h"          /* For mymalloc_cat, myfree */
#include "module_registry.h" /* For PhaseModuleConfig and LoopMode */
#include "proto.h"
#include "tree/forest_distribution.h" /* forest_distribution_scheme_from_string */
#include "tree/reader.h"              /* tree_reader_lookup, struct TreeReader */
#include "types.h"
#include "generated/unit_registry.h" /* mimic_unit_label_cgs / mimic_unit_label_h_convention */

#ifndef MIMIC_COMPILED_MODEL
#error                                                                                             \
    "MIMIC_COMPILED_MODEL must be set at compile time via -DMIMIC_COMPILED_MODEL=<name>. Use make MODEL=<name>."
#endif

#ifndef MIMIC_COMPILED_MODEL_PATH
#error                                                                                             \
    "MIMIC_COMPILED_MODEL_PATH must be set at compile time via -DMIMIC_COMPILED_MODEL_PATH=<path>. Use make MODEL=<name>."
#endif

#ifndef MIMIC_COMPILED_SIMULATION
#error                                                                                             \
    "MIMIC_COMPILED_SIMULATION must be set at compile time via -DMIMIC_COMPILED_SIMULATION=<name>. Use make SIMULATION=<name>."
#endif

/* Helper functions for DOM navigation */
static yaml_node_t *get_mapping_value(yaml_document_t *doc, yaml_node_t *mapping, const char *key);
static const char *get_scalar_value(yaml_node_t *node);
static int is_yaml_null(const yaml_node_t *node);
static void reject_unknown_keys(yaml_document_t *doc, yaml_node_t *mapping,
                                const char *section_name, const char *const *valid_keys,
                                size_t num_valid_keys);
static int get_strict_int_value(yaml_node_t *node, const char *field_name);
static int64_t get_strict_int64_value(yaml_node_t *node, const char *field_name);
static double get_strict_double_value(yaml_node_t *node, const char *field_name);
static double get_unit_scalar_value(yaml_document_t *doc, yaml_node_t *node, const char *field_name,
                                    const char *reference_label);
static void parse_output_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_input_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_model_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_simulation_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_plotting_section(yaml_document_t *doc, yaml_node_t *section);
static void parse_modules_section(yaml_document_t *doc, yaml_node_t *section);
static void validate_and_postprocess(void);
static void parse_simulation_config_file(const char *fname);
static void validate_output_snapshots(void);
static enum InputProcessingOrder parse_processing_order(const char *value);
static void resolve_config_path(const char *path, const char *param_file, char *resolved,
                                size_t resolved_size);
static int file_exists_readable(const char *path);
static void set_model_package_paths(void);
static void set_simulation_package_paths(void);
static void set_default_simulation_config_path(const char *param_file, yaml_document_t *doc,
                                               yaml_node_t *simulation_section);

/**
 * @brief   Bundled handles for one loaded YAML document.
 *
 * yaml_file_open() fills all four fields (fatal on any failure), and
 * yaml_file_close() releases them. The root node is guaranteed to be a
 * mapping.
 */
struct YamlFile {
  FILE *fh;
  yaml_parser_t parser;
  yaml_document_t document;
  yaml_node_t *root;
};

static void yaml_file_open(struct YamlFile *yf, const char *fname, const char *what) {
  yf->fh = fopen(fname, "r");
  if (!yf->fh) {
    FATAL_ERROR("Cannot open %s '%s'", what, fname);
  }

  if (!yaml_parser_initialize(&yf->parser)) {
    FATAL_ERROR("Failed to initialize YAML parser for '%s'", fname);
  }

  yaml_parser_set_input_file(&yf->parser, yf->fh);

  if (!yaml_parser_load(&yf->parser, &yf->document)) {
    FATAL_ERROR("YAML parse error in %s '%s' at line %zu: %s", what, fname,
                yf->parser.problem_mark.line + 1, yf->parser.problem);
  }

  yf->root = yaml_document_get_root_node(&yf->document);
  if (!yf->root || yf->root->type != YAML_MAPPING_NODE) {
    FATAL_ERROR("%s '%s': YAML root must be a mapping", what, fname);
  }
}

static void yaml_file_close(struct YamlFile *yf) {
  yaml_document_delete(&yf->document);
  yaml_parser_delete(&yf->parser);
  fclose(yf->fh);
}

/**
 * @brief   Read and parse YAML parameter file
 *
 * @param   fname   Path to YAML parameter file
 *
 * Uses libyaml's document API to load the entire YAML file into a DOM tree,
 * then navigates the tree to extract configuration values.
 */
void read_parameter_file(const char *fname) {
  struct YamlFile yf;

  VERBOSE_LOG("Reading YAML parameter file: %s", fname);

  yaml_file_open(&yf, fname, "parameter file");
  yaml_document_t *document = &yf.document;
  yaml_node_t *root = yf.root;

  yaml_node_t *section;
  yaml_node_t *node;

  /*
   * Load order: simulation config file first (provides defaults), then all
   * sections from the run file (may override those defaults). This means any
   * field present in both files takes the value from the run file.
   *
   * The default simulation config is derived from simulation.name. A run file
   * may still point simulation.config at a smaller fixture or alternate input
   * range while keeping the compiled simulation property package fixed.
   */
  section = get_mapping_value(document, root, "simulation");
  if (section) {
    set_default_simulation_config_path(fname, document, section);
    if (strlen(MimicConfig.SimulationConfigPath) > 0) {
      parse_simulation_config_file(MimicConfig.SimulationConfigPath);
    }
  }

  section = get_mapping_value(document, root, "output");
  if (section)
    parse_output_section(document, section);

  section = get_mapping_value(document, root, "input");
  if (section)
    parse_input_section(document, section);

  section = get_mapping_value(document, root, "model");
  if (section)
    parse_model_section(document, section);

  section = get_mapping_value(document, root, "simulation");
  if (section)
    parse_simulation_section(document, section);

  section = get_mapping_value(document, root, "plotting");
  if (section)
    parse_plotting_section(document, section);

  /* Parse SubSteps (top-level parameter) */
  node = get_mapping_value(document, root, "SubSteps");
  if (node) {
    MimicConfig.SubSteps = get_strict_int_value(node, "SubSteps");
    DEBUG_LOG("SubSteps = %d", MimicConfig.SubSteps);
  } else {
    MimicConfig.SubSteps = 1; /* Default: no sub-stepping */
  }

  section = get_mapping_value(document, root, "modules");
  if (section)
    parse_modules_section(document, section);

  yaml_file_close(&yf);

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
  for (pair = mapping->data.mapping.pairs.start; pair < mapping->data.mapping.pairs.top; pair++) {
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
 * @brief   Return 1 if node represents a YAML null value.
 *
 * Covers absent keys (NULL pointer), bare keys with no value (empty scalar),
 * and the explicit YAML null representations "null" and "~". All are
 * semantically equivalent to "no value" and should be treated identically.
 */
static int is_yaml_null(const yaml_node_t *node) {
  if (!node)
    return 1;
  if (node->type != YAML_SCALAR_NODE)
    return 0;
  const char *val = (const char *)node->data.scalar.value;
  return !val || val[0] == '\0' || strcmp(val, "null") == 0 || strcmp(val, "~") == 0;
}

/**
 * @brief   Reject unknown keys in sections whose schema is fixed.
 */
static void reject_unknown_keys(yaml_document_t *doc, yaml_node_t *mapping,
                                const char *section_name, const char *const *valid_keys,
                                size_t num_valid_keys) {
  if (!mapping || mapping->type != YAML_MAPPING_NODE) {
    return;
  }

  for (yaml_node_pair_t *pair = mapping->data.mapping.pairs.start;
       pair < mapping->data.mapping.pairs.top; pair++) {
    yaml_node_t *key_node = yaml_document_get_node(doc, pair->key);
    const char *key = get_scalar_value(key_node);
    int known = 0;

    if (key == NULL) {
      continue;
    }

    for (size_t i = 0; i < num_valid_keys; i++) {
      if (strcmp(key, valid_keys[i]) == 0) {
        known = 1;
        break;
      }
    }

    if (!known) {
      FATAL_ERROR("Unknown key '%s.%s'", section_name, key);
    }
  }
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

static enum InputProcessingOrder parse_processing_order(const char *value) {
  if (strcasecmp(value, "tree_ordered") == 0) {
    return INPUT_PROCESSING_ORDER_TREE;
  }
  if (strcasecmp(value, "snapshot_ordered") == 0) {
    return INPUT_PROCESSING_ORDER_SNAPSHOT;
  }

  FATAL_ERROR("Unknown input.processing_order '%s'. Valid values are tree_ordered, "
              "snapshot_ordered.",
              value);
  return INPUT_PROCESSING_ORDER_TREE; /* unreachable */
}

/**
 * @brief   Resolve a run-file path with param-file-relative fallback.
 *
 * Absolute paths are used as-is. Relative paths are first tried exactly as
 * provided (normally repository-root-relative because runs are launched from
 * the repo root), then relative to the parameter file's parent directory. The
 * returned path is stored for metadata copying and later diagnostics.
 */
static void snprintf_path(char *dst, size_t size, const char *what, const char *fmt, ...) {
  va_list ap;
  int written;

  va_start(ap, fmt);
  written = vsnprintf(dst, size, fmt, ap);
  va_end(ap);

  if (written < 0 || (size_t)written >= size) {
    FATAL_ERROR("Path too long while resolving '%s'", what);
  }
}

static void resolve_config_path(const char *path, const char *param_file, char *resolved,
                                size_t resolved_size) {
  const char *last_slash;
  char param_dir[MAX_STRING_LEN];

  if (path == NULL || path[0] == '\0') {
    resolved[0] = '\0';
    return;
  }

  /* Absolute paths and paths that already resolve are used as-is. */
  if (path[0] == '/' || file_exists_readable(path)) {
    snprintf_path(resolved, resolved_size, path, "%s", path);
    return;
  }

  /* Fall back to a path relative to the parameter file's directory. */
  last_slash = strrchr(param_file, '/');
  if (last_slash == NULL) {
    snprintf_path(resolved, resolved_size, path, "%s", path);
    return;
  }

  snprintf_path(param_dir, sizeof(param_dir), param_file, "%.*s", (int)(last_slash - param_file),
                param_file);
  snprintf_path(resolved, resolved_size, path, "%s/%s", param_dir, path);
}

/**
 * @brief   Derive model-owned package paths from the selected model name.
 */
static void set_model_package_paths(void) {
  if (strlen(MimicConfig.ModelName) == 0) {
    return;
  }

  snprintf_path(MimicConfig.ModelPath, sizeof(MimicConfig.ModelPath), "model.path", "models/%s",
                MimicConfig.ModelName);
  snprintf_path(MimicConfig.ModelPropertiesPath, sizeof(MimicConfig.ModelPropertiesPath),
                "model.model_properties", "%s/model_properties.yaml", MimicConfig.ModelPath);
}

/**
 * @brief   Derive simulation-owned package paths from the selected simulation.
 */
static void set_simulation_package_paths(void) {
  if (strlen(MimicConfig.SimulationName) == 0) {
    return;
  }

  snprintf_path(MimicConfig.SimulationPath, sizeof(MimicConfig.SimulationPath), "simulation.path",
                "simulations/%s", MimicConfig.SimulationName);
  snprintf_path(MimicConfig.SimulationHaloPropertiesPath,
                sizeof(MimicConfig.SimulationHaloPropertiesPath), "simulation.halo_properties",
                "%s/halo_properties.yaml", MimicConfig.SimulationPath);
}

/**
 * @brief   Resolve the simulation config path before loading simulation defaults.
 */
static void set_default_simulation_config_path(const char *param_file, yaml_document_t *doc,
                                               yaml_node_t *simulation_section) {
  yaml_node_t *node;
  const char *str;
  char default_path[MAX_STRING_LEN];

  node = get_mapping_value(doc, simulation_section, "name");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.SimulationName, str, MAX_STRING_LEN - 1);
    set_simulation_package_paths();
  }

  node = get_mapping_value(doc, simulation_section, "config");
  if (node && (str = get_scalar_value(node))) {
    resolve_config_path(str, param_file, MimicConfig.SimulationConfigPath,
                        sizeof(MimicConfig.SimulationConfigPath));
    return;
  }

  if (strlen(MimicConfig.SimulationName) == 0) {
    return;
  }

  snprintf_path(default_path, sizeof(default_path), "simulation.config", "%s/simulation_info.yaml",
                MimicConfig.SimulationPath);
  resolve_config_path(default_path, param_file, MimicConfig.SimulationConfigPath,
                      sizeof(MimicConfig.SimulationConfigPath));
}

/**
 * @brief   Get scalar value as integer, rejecting malformed input.
 */
static int get_strict_int_value(yaml_node_t *node, const char *field_name) {
  const char *str = get_scalar_value(node);
  char *endptr;
  long value;

  if (str == NULL) {
    FATAL_ERROR("%s must be an integer scalar", field_name);
  }

  errno = 0;
  value = strtol(str, &endptr, 10);
  if (str == endptr || errno != 0 || value < INT_MIN || value > INT_MAX) {
    FATAL_ERROR("%s must be a valid integer", field_name);
  }

  while (isspace((unsigned char)*endptr)) {
    endptr++;
  }
  if (*endptr != '\0') {
    FATAL_ERROR("%s must be a valid integer", field_name);
  }

  return (int)value;
}

/**
 * @brief   Get scalar value as int64_t, rejecting malformed input.
 */
static int64_t get_strict_int64_value(yaml_node_t *node, const char *field_name) {
  const char *str = get_scalar_value(node);
  char *endptr;
  long long value;

  if (str == NULL) {
    FATAL_ERROR("%s must be an integer scalar", field_name);
  }

  errno = 0;
  value = strtoll(str, &endptr, 10);
  if (str == endptr || errno != 0) {
    FATAL_ERROR("%s must be a valid 64-bit integer", field_name);
  }

  while (isspace((unsigned char)*endptr)) {
    endptr++;
  }
  if (*endptr != '\0') {
    FATAL_ERROR("%s must be a valid 64-bit integer", field_name);
  }

  return (int64_t)value;
}

/**
 * @brief   Get scalar value as double, rejecting malformed input.
 */
static double get_strict_double_value(yaml_node_t *node, const char *field_name) {
  const char *str = get_scalar_value(node);
  char *endptr;
  double value;

  if (str == NULL) {
    FATAL_ERROR("%s must be a numeric scalar", field_name);
  }

  errno = 0;
  value = strtod(str, &endptr);
  if (str == endptr || errno != 0 || !isfinite(value)) {
    FATAL_ERROR("%s must be a valid finite number", field_name);
  }

  while (isspace((unsigned char)*endptr)) {
    endptr++;
  }
  if (*endptr != '\0') {
    FATAL_ERROR("%s must be a valid finite number", field_name);
  }

  return value;
}

static double unit_label_cgs(const char *label, const char *field_name) {
  const double cgs = mimic_unit_label_cgs(label);
  if (cgs < 0.0)
    FATAL_ERROR("%s has unsupported units '%s'", field_name, label);
  return cgs;
}

static const char *unit_label_h_convention(const char *label, const char *field_name) {
  const char *h_convention = mimic_unit_label_h_convention(label);
  if (h_convention == NULL)
    FATAL_ERROR("%s has unsupported units '%s'", field_name, label);
  return h_convention;
}

static double convert_unit_scalar(double value, const char *units, const char *h_convention,
                                  const char *reference_label, const char *field_name) {
  const double source_cgs = unit_label_cgs(units, field_name);
  const double target_cgs = unit_label_cgs(reference_label, field_name);
  const char *target_h_convention = unit_label_h_convention(reference_label, field_name);
  double factor = source_cgs / target_cgs;

  if (strcmp(h_convention, target_h_convention) != 0) {
    if (strcmp(h_convention, "none") == 0 || strcmp(target_h_convention, "none") == 0) {
      FATAL_ERROR("%s cannot convert between h-independent and h-dependent conventions",
                  field_name);
    }

    if (strcmp(target_h_convention, "carried") == 0)
      factor *= MimicConfig.Hubble_h;
    else
      factor /= MimicConfig.Hubble_h;
  }

  return value * factor;
}

static double get_unit_scalar_value(yaml_document_t *doc, yaml_node_t *node, const char *field_name,
                                    const char *reference_label) {
  yaml_node_t *value_node, *units_node, *h_node;
  const char *units, *h_convention;
  static const char *const scalar_keys[] = {"value", "units", "h_convention"};

  if (node->type != YAML_MAPPING_NODE) {
    return get_strict_double_value(node, field_name);
  }

  reject_unknown_keys(doc, node, field_name, scalar_keys,
                      sizeof(scalar_keys) / sizeof(scalar_keys[0]));
  value_node = get_mapping_value(doc, node, "value");
  units_node = get_mapping_value(doc, node, "units");
  h_node = get_mapping_value(doc, node, "h_convention");

  if (value_node == NULL || units_node == NULL) {
    FATAL_ERROR("%s must provide value and units", field_name);
  }

  units = get_scalar_value(units_node);
  if (units == NULL) {
    FATAL_ERROR("%s units must be a scalar string", field_name);
  }

  h_convention = h_node ? get_scalar_value(h_node) : unit_label_h_convention(units, field_name);
  if (h_convention == NULL) {
    FATAL_ERROR("%s units and h_convention must be scalar strings", field_name);
  }
  if (strcmp(h_convention, "carried") != 0 && strcmp(h_convention, "none") != 0 &&
      strcmp(h_convention, "free") != 0) {
    FATAL_ERROR("%s has invalid h_convention '%s'", field_name, h_convention);
  }

  return convert_unit_scalar(get_strict_double_value(value_node, field_name), units, h_convention,
                             reference_label, field_name);
}

/**
 * @brief   Parse output section
 */
static void parse_output_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  const char *str;
  static const char *const valid_keys[] = {"output_filename",     "output_directory",
                                           "output_format",       "snapshot_list",
                                           "target_file_size_mb", "forests_per_file"};

  DEBUG_LOG("Parsing output section");
  reject_unknown_keys(doc, section, "output", valid_keys,
                      sizeof(valid_keys) / sizeof(valid_keys[0]));

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

  node = get_mapping_value(doc, section, "target_file_size_mb");
  if (node) {
    int64_t mb = get_strict_int64_value(node, "output.target_file_size_mb");
    if (mb <= 0) {
      FATAL_ERROR("output.target_file_size_mb must be positive");
    }
    MimicConfig.TargetFileSize = mb * 1024LL * 1024LL;
    DEBUG_LOG("TargetFileSize = %" PRId64 " (from %" PRId64 " MB)", MimicConfig.TargetFileSize, mb);
  }

  node = get_mapping_value(doc, section, "forests_per_file");
  if (node) {
    MimicConfig.ForestsPerFile = get_strict_int64_value(node, "output.forests_per_file");
    if (MimicConfig.ForestsPerFile < 0) {
      FATAL_ERROR("output.forests_per_file must be non-negative");
    }
    DEBUG_LOG("ForestsPerFile = %" PRId64, MimicConfig.ForestsPerFile);
  }

  /* Parse snapshot list array */
  node = get_mapping_value(doc, section, "snapshot_list");
  if (node) {
    if (node->type != YAML_SEQUENCE_NODE) {
      FATAL_ERROR("output.snapshot_list must be a sequence");
    }
    yaml_node_item_t *item;
    int idx = 0;
    for (item = node->data.sequence.items.start; item < node->data.sequence.items.top; item++) {
      yaml_node_t *value_node = yaml_document_get_node(doc, *item);
      if (value_node) {
        if (idx >= ABSOLUTEMAXSNAPS) {
          FATAL_ERROR("output.snapshot_list has more than %d entries", ABSOLUTEMAXSNAPS);
        }
        int snap = get_strict_int_value(value_node, "output.snapshot_list[]");
        MimicConfig.ListOutputSnaps[idx] = snap;
        DEBUG_LOG("Snapshot[%d] = %d", idx, snap);
        idx++;
      }
    }
    MimicConfig.NOUT = idx;
    DEBUG_LOG("NumOutputs inferred from snapshot_list = %d", MimicConfig.NOUT);
  }
}

/**
 * @brief   Parse simulation-owned output defaults.
 *
 * Simulation metadata may provide catalogue-scale chunking defaults, but output
 * destinations, formats, and snapshot selections remain run-file concerns.
 */
static void parse_simulation_output_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  static const char *const valid_keys[] = {"target_file_size_mb", "forests_per_file"};

  DEBUG_LOG("Parsing simulation output defaults");
  reject_unknown_keys(doc, section, "simulation output defaults", valid_keys,
                      sizeof(valid_keys) / sizeof(valid_keys[0]));

  node = get_mapping_value(doc, section, "target_file_size_mb");
  if (node) {
    int64_t mb = get_strict_int64_value(node, "output.target_file_size_mb");
    if (mb <= 0) {
      FATAL_ERROR("output.target_file_size_mb must be positive");
    }
    MimicConfig.TargetFileSize = mb * 1024LL * 1024LL;
    DEBUG_LOG("Simulation TargetFileSize default = %" PRId64 " (from %" PRId64 " MB)",
              MimicConfig.TargetFileSize, mb);
  }

  node = get_mapping_value(doc, section, "forests_per_file");
  if (node) {
    MimicConfig.ForestsPerFile = get_strict_int64_value(node, "output.forests_per_file");
    if (MimicConfig.ForestsPerFile < 0) {
      FATAL_ERROR("output.forests_per_file must be non-negative");
    }
    DEBUG_LOG("Simulation ForestsPerFile default = %" PRId64, MimicConfig.ForestsPerFile);
  }
}

/**
 * @brief   Parse input section
 */
static void parse_input_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  const char *str;
  static const char *const valid_keys[] = {"first_file",
                                           "last_file",
                                           "tree_name",
                                           "tree_type",
                                           "processing_order",
                                           "simulation_dir",
                                           "snapshot_list_file",
                                           "max_tree_depth",
                                           "forest_distribution_scheme",
                                           "exponent_forest_dist_scheme"};

  DEBUG_LOG("Parsing input section");
  reject_unknown_keys(doc, section, "input", valid_keys,
                      sizeof(valid_keys) / sizeof(valid_keys[0]));

  node = get_mapping_value(doc, section, "first_file");
  if (node) {
    MimicConfig.FirstFile = get_strict_int_value(node, "input.first_file");
    DEBUG_LOG("FirstFile = %d", MimicConfig.FirstFile);
  }

  node = get_mapping_value(doc, section, "last_file");
  if (node) {
    MimicConfig.LastFile = get_strict_int_value(node, "input.last_file");
    DEBUG_LOG("LastFile = %d", MimicConfig.LastFile);
  }

  node = get_mapping_value(doc, section, "tree_name");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.TreeName, str, MAX_STRING_LEN - 1);
    DEBUG_LOG("TreeName = %s", str);
  }

  node = get_mapping_value(doc, section, "tree_type");
  if (node && (str = get_scalar_value(node))) {
    const struct TreeReader *reader = tree_reader_lookup(str);
    if (reader == NULL) {
      FATAL_ERROR("Unknown tree_type '%s'. Valid types are registered in "
                  "src/io/tree/registry.c; HDF5-based types also require an "
                  "HDF5-enabled build (do not pass USE-HDF5=no).",
                  str);
    }
    MimicConfig.reader = reader;
    strncpy(MimicConfig.TreeExtension, reader->file_extension, MAX_STRING_LEN - 1);
    MimicConfig.TreeExtension[MAX_STRING_LEN - 1] = '\0';
    DEBUG_LOG("tree_type = %s", str);
  }

  node = get_mapping_value(doc, section, "processing_order");
  if (node && (str = get_scalar_value(node))) {
    MimicConfig.ProcessingOrder = parse_processing_order(str);
    DEBUG_LOG("processing_order = %s",
              input_processing_order_name((enum InputProcessingOrder)MimicConfig.ProcessingOrder));
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

  node = get_mapping_value(doc, section, "max_tree_depth");
  if (node) {
    MimicConfig.MaxTreeDepth = get_strict_int_value(node, "input.max_tree_depth");
    DEBUG_LOG("MaxTreeDepth = %d", MimicConfig.MaxTreeDepth);
  }

  /* Consistent-Trees forest -> MPI-task load balancing (ignored by other
     readers). The ASCII reader splits forests uniformly regardless; the HDF5
     reader can weight by per-forest halo count. */
  node = get_mapping_value(doc, section, "forest_distribution_scheme");
  if (node && (str = get_scalar_value(node))) {
    const int scheme = forest_distribution_scheme_from_string(str);
    if (scheme < 0) {
      FATAL_ERROR("Unknown forest_distribution_scheme '%s'. Valid values are uniform, linear, "
                  "quadratic, exponent, generic_power.",
                  str);
    }
    MimicConfig.ForestDistributionScheme = scheme;
    DEBUG_LOG("ForestDistributionScheme = %s (%d)", str, scheme);
  }

  node = get_mapping_value(doc, section, "exponent_forest_dist_scheme");
  if (node) {
    MimicConfig.Exponent_Forest_Dist_Scheme =
        get_strict_double_value(node, "input.exponent_forest_dist_scheme");
    DEBUG_LOG("Exponent_Forest_Dist_Scheme = %g", MimicConfig.Exponent_Forest_Dist_Scheme);
  }
}

/**
 * @brief   Parse model package metadata.
 */
static void parse_model_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  const char *str;
  static const char *const valid_keys[] = {"name"};

  DEBUG_LOG("Parsing model section");
  reject_unknown_keys(doc, section, "model", valid_keys,
                      sizeof(valid_keys) / sizeof(valid_keys[0]));

  node = get_mapping_value(doc, section, "name");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.ModelName, str, MAX_STRING_LEN - 1);
    set_model_package_paths();
  }
}

/**
 * @brief   Parse simulation section
 *
 * Parses simulation properties including cosmology and units subsections.
 */
static void parse_simulation_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node, *cosmology;
  const char *str;
  static const char *const valid_keys[] = {"name", "config", "cosmology", "box_size",
                                           "particle_mass"};
  static const char *const cosmology_keys[] = {"omega_matter", "omega_lambda", "hubble_h"};

  DEBUG_LOG("Parsing simulation section");
  reject_unknown_keys(doc, section, "simulation", valid_keys,
                      sizeof(valid_keys) / sizeof(valid_keys[0]));

  node = get_mapping_value(doc, section, "name");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.SimulationName, str, MAX_STRING_LEN - 1);
    set_simulation_package_paths();
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
    reject_unknown_keys(doc, cosmology, "simulation.cosmology", cosmology_keys,
                        sizeof(cosmology_keys) / sizeof(cosmology_keys[0]));

    node = get_mapping_value(doc, cosmology, "omega_matter");
    if (node) {
      MimicConfig.Omega = get_strict_double_value(node, "simulation.cosmology.omega_matter");
      DEBUG_LOG("Omega = %g", MimicConfig.Omega);
    }

    node = get_mapping_value(doc, cosmology, "omega_lambda");
    if (node) {
      MimicConfig.OmegaLambda = get_strict_double_value(node, "simulation.cosmology.omega_lambda");
      DEBUG_LOG("OmegaLambda = %g", MimicConfig.OmegaLambda);
    }

    node = get_mapping_value(doc, cosmology, "hubble_h");
    if (node) {
      MimicConfig.Hubble_h = get_strict_double_value(node, "simulation.cosmology.hubble_h");
      DEBUG_LOG("Hubble_h = %g", MimicConfig.Hubble_h);
    }
  }

  node = get_mapping_value(doc, section, "box_size");
  if (node) {
    MimicConfig.BoxSize = get_unit_scalar_value(doc, node, "simulation.box_size", "Mpc/h");
    DEBUG_LOG("BoxSize = %g", MimicConfig.BoxSize);
  }

  node = get_mapping_value(doc, section, "particle_mass");
  if (node) {
    MimicConfig.PartMass =
        get_unit_scalar_value(doc, node, "simulation.particle_mass", "1e10 Msun/h");
    DEBUG_LOG("PartMass = %g", MimicConfig.PartMass);
  }
}

/**
 * @brief   Parse plotting package metadata.
 */
static void parse_plotting_section(yaml_document_t *doc, yaml_node_t *section) {
  yaml_node_t *node;
  const char *str;
  static const char *const valid_keys[] = {"profile"};

  DEBUG_LOG("Parsing plotting section");
  reject_unknown_keys(doc, section, "plotting", valid_keys,
                      sizeof(valid_keys) / sizeof(valid_keys[0]));

  node = get_mapping_value(doc, section, "profile");
  if (node && (str = get_scalar_value(node))) {
    strncpy(MimicConfig.PlottingProfilePath, str, MAX_STRING_LEN - 1);
  }
}

/**
 * @brief   Load simulation-owned input and physical metadata from YAML.
 */
static void parse_simulation_config_file(const char *fname) {
  struct YamlFile yf;

  VERBOSE_LOG("Reading simulation config file: %s", fname);

  yaml_file_open(&yf, fname, "simulation config file");
  yaml_document_t *document = &yf.document;

  yaml_node_t *section = get_mapping_value(document, yf.root, "input");
  if (section)
    parse_input_section(document, section);

  section = get_mapping_value(document, yf.root, "output");
  if (section)
    parse_simulation_output_section(document, section);

  section = get_mapping_value(document, yf.root, "simulation");
  if (section)
    parse_simulation_section(document, section);

  yaml_file_close(&yf);
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
                              struct PhaseModuleConfig **config, int *num_modules,
                              const char *phase_name) {
  if (!phase_node) {
    *config = NULL;
    *num_modules = 0;
    return 0; /* Empty phase is valid */
  }

  /* Handle scalar nodes: bare keys with no value, explicit "null" / "~", or
   * all modules commented out all produce a scalar node that means empty. */
  if (phase_node->type == YAML_SCALAR_NODE) {
    if (is_yaml_null(phase_node)) {
      DEBUG_LOG("Phase '%s' is empty (null or all modules commented out)", phase_name);
      *config = NULL;
      *num_modules = 0;
      return 0;
    }
    ERROR_LOG("Phase '%s' must be a sequence (found scalar: '%s')", phase_name,
              (const char *)phase_node->data.scalar.value);
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

    /* Each item should be a mapping with one entry: "module_name:
     * processing_mode" */
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

    /* Store in config (resolved is populated later by module_system_init) */
    (*config)[idx].module_name = strdup(module_name);
    (*config)[idx].processing_mode = processing_mode;
    (*config)[idx].resolved = NULL;

    DEBUG_LOG("Phase '%s': %s (processing_mode=%s)", phase_name, module_name, processing_mode_str);
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
 * Vision Principle 2 (Runtime Modularity): Pipeline structure configured at
 * runtime. Vision Principle 4 (Single Source of Truth): Input file defines
 * complete model.
 */
/**
 * @brief   Reject substep phase names that collide with reserved keys.
 *
 * @return  1 if the name is reserved, 0 otherwise
 */
static int phase_name_is_reserved(const char *name) {
  static const char *reserved[] = {"pre_timestep", "post_timestep", "parameters", "phases"};
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
static void add_substep_phase(yaml_document_t *doc, const char *name, yaml_node_t *phase_node) {
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

  struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[MimicConfig.num_substep_phases];
  phase->name = strdup(name);
  if (phase->name == NULL) {
    FATAL_ERROR("Failed to allocate substep phase name '%s'", name);
  }
  if (parse_phase_config(doc, phase_node, &phase->modules, &phase->num_modules, name) != 0) {
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
  if (parse_phase_config(doc, node, &MimicConfig.pre_timestep, &MimicConfig.num_pre_timestep,
                         "pre_timestep") != 0) {
    FATAL_ERROR("Failed to parse pre_timestep phase");
  }

  node = get_mapping_value(doc, section, "post_timestep");
  if (parse_phase_config(doc, node, &MimicConfig.post_timestep, &MimicConfig.num_post_timestep,
                         "post_timestep") != 0) {
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
    if (strcmp(key_name, "pre_timestep") != 0 && strcmp(key_name, "post_timestep") != 0 &&
        strcmp(key_name, "phases") != 0 && strcmp(key_name, "parameters") != 0) {
      FATAL_ERROR("Unknown key 'modules.%s'; supported keys are pre_timestep, "
                  "phases, post_timestep, parameters",
                  key_name);
    }
  }

  /* Middle phases: an ordered mapping of user-named phases under 'phases:'.
   * Absent 'phases:' simply means no per-substep middle phases. */
  yaml_node_t *phases_node = get_mapping_value(doc, section, "phases");

  MimicConfig.substep_phases =
      mymalloc_cat(MAX_SUBSTEP_PHASES * sizeof(struct ModulePhaseConfig), MEM_UTILITY);
  if (MimicConfig.substep_phases == NULL) {
    FATAL_ERROR("Failed to allocate substep phase array");
  }

  if (phases_node != NULL && !is_yaml_null(phases_node)) {
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

  VERBOSE_LOG("Multi-phase pipeline configured:");
  VERBOSE_LOG("  pre_timestep: %d module(s)", MimicConfig.num_pre_timestep);
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    VERBOSE_LOG("  %s: %d module(s)", MimicConfig.substep_phases[p].name,
                MimicConfig.substep_phases[p].num_modules);
  }
  VERBOSE_LOG("  post_timestep: %d module(s)", MimicConfig.num_post_timestep);

  /* Parse parameters subsection */
  parameters = get_mapping_value(doc, section, "parameters");
  if (parameters) {
    DEBUG_LOG("Parsing modules.parameters subsection");

    /* Parameters are a simple mapping: param_name -> value */
    if (parameters->type != YAML_MAPPING_NODE) {
      FATAL_ERROR("modules.parameters section must be a mapping");
    }

    yaml_node_pair_t *pair;
    int idx = 0;

    /* Iterate over each parameter in the mapping */
    for (pair = parameters->data.mapping.pairs.start; pair < parameters->data.mapping.pairs.top;
         pair++) {

      yaml_node_t *key_node = yaml_document_get_node(doc, pair->key);
      yaml_node_t *value_node = yaml_document_get_node(doc, pair->value);

      const char *param_name = get_scalar_value(key_node);
      const char *param_value = get_scalar_value(value_node);

      if (param_name && param_value) {
        if (idx >= MAX_MODEL_PARAMS) {
          FATAL_ERROR("modules.parameters has more than %d entries", MAX_MODEL_PARAMS);
        }
        /* Store in ModelParams array */
        strncpy(MimicConfig.ModelParams[idx].param_name, param_name, MAX_STRING_LEN - 1);
        strncpy(MimicConfig.ModelParams[idx].value, param_value, MAX_STRING_LEN - 1);
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
    ERROR_LOG("Required parameter 'output.output_directory' missing");
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
  if (strlen(MimicConfig.ModelName) > 0 &&
      strcmp(MimicConfig.ModelName, MIMIC_COMPILED_MODEL) != 0) {
    ERROR_LOG("Run file selects model.name='%s' but this executable was built "
              "with MODEL=%s",
              MimicConfig.ModelName, MIMIC_COMPILED_MODEL);
    errors++;
  }
  if (strlen(MimicConfig.ModelPath) > 0 &&
      strcmp(MimicConfig.ModelPath, MIMIC_COMPILED_MODEL_PATH) != 0) {
    ERROR_LOG("Derived model.path='%s' but this executable was built with MODEL=%s (%s)",
              MimicConfig.ModelPath, MIMIC_COMPILED_MODEL, MIMIC_COMPILED_MODEL_PATH);
    errors++;
  }
  if (strlen(MimicConfig.SimulationName) == 0) {
    ERROR_LOG("Required parameter 'simulation.name' missing");
    errors++;
  }
  if (strlen(MimicConfig.SimulationName) > 0 &&
      strcmp(MimicConfig.SimulationName, MIMIC_COMPILED_SIMULATION) != 0) {
    ERROR_LOG("Run file selects simulation.name='%s' but this executable was built "
              "with SIMULATION=%s",
              MimicConfig.SimulationName, MIMIC_COMPILED_SIMULATION);
    errors++;
  }
  if (strlen(MimicConfig.SimulationPath) == 0) {
    ERROR_LOG("Failed to derive simulation.path from simulation.name");
    errors++;
  }
  if (strlen(MimicConfig.SimulationConfigPath) == 0) {
    ERROR_LOG("Required parameter 'simulation.config' missing");
    errors++;
  }
  if (strlen(MimicConfig.SimulationHaloPropertiesPath) == 0) {
    ERROR_LOG("Failed to derive simulation.halo_properties from simulation.name");
    errors++;
  }
  if (strlen(MimicConfig.PlottingProfilePath) > 0 && MimicConfig.PlottingProfilePath[0] == '/') {
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
  if (MimicConfig.reader == NULL) {
    ERROR_LOG("Required parameter 'input.tree_type' missing or unrecognised");
    errors++;
  } else if (MimicConfig.ProcessingOrder == INPUT_PROCESSING_ORDER_SNAPSHOT) {
    ERROR_LOG("The snapshot-ordered driver is not implemented yet");
    errors++;
  } else if (MimicConfig.reader->processing_order !=
             (enum InputProcessingOrder)MimicConfig.ProcessingOrder) {
    ERROR_LOG("Reader '%s' is compatible with processing_order '%s', but "
              "input.processing_order is '%s'",
              MimicConfig.reader->name,
              input_processing_order_name(MimicConfig.reader->processing_order),
              input_processing_order_name((enum InputProcessingOrder)MimicConfig.ProcessingOrder));
    errors++;
  }
  if (strlen(MimicConfig.FileWithSnapList) == 0) {
    ERROR_LOG("Required parameter 'input.snapshot_list_file' missing");
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

  if (errors > 0) {
    FATAL_ERROR("Parameter validation failed");
  }

  /* Post-process parameters */

  read_snap_list();
  validate_output_snapshots();

  /* Log summary */
  int total_modules = MimicConfig.num_pre_timestep + MimicConfig.num_post_timestep;
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    total_modules += MimicConfig.substep_phases[p].num_modules;
  }
  int total_phases = MimicConfig.num_substep_phases + 2; /* pre + post */
  VERBOSE_LOG("Configuration: %d output snapshots, %d module instances across %d "
              "phases",
              MimicConfig.NOUT, total_modules, total_phases);
  VERBOSE_LOG("SubSteps: %d", MimicConfig.SubSteps);
}

/**
 * @brief   Expand and validate output snapshot selection.
 */
static void validate_output_snapshots(void) {
  if (MimicConfig.NOUT == 0) {
    MimicConfig.NOUT = MimicConfig.MAXSNAPS;
    for (int i = 0; i < MimicConfig.NOUT; i++) {
      MimicConfig.ListOutputSnaps[i] = i;
    }
    VERBOSE_LOG("All %d snapshots selected for output (empty snapshot_list)", MimicConfig.NOUT);
    return;
  }

  if (MimicConfig.NOUT < 0 || MimicConfig.NOUT > ABSOLUTEMAXSNAPS) {
    FATAL_ERROR("Output snapshot count %d outside valid range [0, %d]", MimicConfig.NOUT,
                ABSOLUTEMAXSNAPS);
  }

  for (int i = 0; i < MimicConfig.NOUT; i++) {
    int snap = MimicConfig.ListOutputSnaps[i];
    if (snap < 0 || snap > MimicConfig.LastSnapshotNr) {
      FATAL_ERROR("output.snapshot_list[%d] = %d outside valid range [0, %d]", i, snap,
                  MimicConfig.LastSnapshotNr);
    }
    for (int j = 0; j < i; j++) {
      if (MimicConfig.ListOutputSnaps[j] == snap) {
        FATAL_ERROR("output.snapshot_list contains duplicate snapshot %d", snap);
      }
    }
  }
}
