/**
 * @file    sage26_oracle_fixture.h
 * @brief   Reader and bit-exact comparator for the SAGE26 function-level oracle fixtures
 *
 * The SAGE26 oracle (models/sage26/modules/_tests/oracle/, whose README.md defines
 * the fixture format) records, for one call of a SAGE26 routine, its inputs
 * (<name>.in) and everything it computed (<name>.out), as exact IEEE-754 bit
 * patterns. A module's oracle unit test builds Mimic state from each input, runs
 * the module's process() entry, and compares the result with the output field by
 * field, bit for bit. This header holds the machinery those tests share:
 *
 * - oracle_environment_mismatch(): NULL in the oracle environment (macOS arm64,
 *   Apple clang 21), otherwise the reason a bit-exact comparison must SKIP;
 * - oracle_manifest_fixtures(): the fixture names fixtures.manifest lists for a
 *   module, and the count it declares;
 * - oracle_load(): parses one fixture file;
 * - oracle_in_*() / oracle_check_*() / oracle_exclude(): read inputs and compare
 *   outputs, each marking the field consumed, so oracle_report_unconsumed() can
 *   prove that every input was applied and every output asserted or excluded
 *   with a stated reason;
 * - oracle_fixture_count_ok() / oracle_all_bit_identical(): the bodies of each
 *   oracle test's two test cases.
 *
 * Fixtures are found through MIMIC_COMPILED_MODEL_PATH, which is repository
 * relative; the unit runner runs from the repository root. Include this header
 * after the test framework.
 */

#ifndef SAGE26_ORACLE_FIXTURE_H
#define SAGE26_ORACLE_FIXTURE_H

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/constants.h"
#include "include/globals.h"
#include "include/types.h"

#define ORACLE_MAX_FIELDS 512
#define ORACLE_MAX_NAME 128
#define ORACLE_MAX_FIXTURES 64
#define ORACLE_MAX_PATH 512
#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define ORACLE_DIR MIMIC_COMPILED_MODEL_PATH "/modules"
#define ORACLE_MANIFEST MIMIC_COMPILED_MODEL_PATH "/modules/_tests/oracle/fixtures.manifest"

enum oracle_type { ORACLE_F32, ORACLE_F64, ORACLE_I32 };

struct oracle_field {
  char name[ORACLE_MAX_NAME];
  enum oracle_type type;
  uint64_t bits;    /* f32 / f64 bit pattern */
  long long ivalue; /* i32 value */
  int consumed;
};

struct oracle_fixture {
  char path[ORACLE_MAX_PATH];
  char routine[ORACLE_MAX_NAME];
  int num_fields;
  struct oracle_field fields[ORACLE_MAX_FIELDS];
  int errors; /* missing or mistyped fields requested by the test */
};

// ============================================================================
// ENVIRONMENT
// ============================================================================

/**
 * @brief Why a bit-exact comparison is not meaningful here, or NULL in the oracle environment
 *
 * The fixtures are bit-exact only for the oracle's compiler, contraction behaviour
 * and libm: macOS arm64 with Apple clang 21 (the oracle README's "Oracle
 * Environment"). Elsewhere a test must report SKIP with this reason.
 */
static inline const char *oracle_environment_mismatch(void) {
#if defined(__APPLE__) && defined(__aarch64__) && defined(__apple_build_version__)
#if __clang_major__ == 21
  return NULL;
#else
  return "bit-exact SAGE26 oracle comparison needs Apple clang 21 (oracle environment: "
         "macOS arm64, Apple clang 21); this compiler is another version";
#endif
#else
  return "bit-exact SAGE26 oracle comparison needs the oracle environment (macOS arm64, Apple "
         "clang 21, macOS libm); this platform or compiler differs";
#endif
}

// ============================================================================
// BIT PATTERNS
// ============================================================================

static inline float oracle_bits_to_f32(const uint64_t bits) {
  const uint32_t b32 = (uint32_t)bits;
  float value;
  memcpy(&value, &b32, sizeof(value));
  return value;
}

static inline double oracle_bits_to_f64(const uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static inline uint32_t oracle_f32_to_bits(const float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static inline uint64_t oracle_f64_to_bits(const double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

// ============================================================================
// PARSING
// ============================================================================

/** @brief Strip a '#' comment and surrounding whitespace in place; returns the trimmed start */
static inline char *oracle_trim(char *line) {
  char *hash = strchr(line, '#');
  if (hash != NULL) {
    *hash = '\0';
  }
  while (*line == ' ' || *line == '\t') {
    line++;
  }
  size_t len = strlen(line);
  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t' || line[len - 1] == '\n' ||
                     line[len - 1] == '\r')) {
    line[--len] = '\0';
  }
  return line;
}

/** @brief Parse an exact hex bit pattern of 8 (f32) or 16 (f64) digits */
static inline int oracle_parse_bits(const char *text, const size_t digits, uint64_t *bits) {
  if (text[0] != '0' || text[1] != 'x' || strlen(text) != digits + 2) {
    return -1;
  }
  uint64_t value = 0;
  for (size_t i = 2; i < digits + 2; i++) {
    const char c = text[i];
    int nibble;
    if (c >= '0' && c <= '9') {
      nibble = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      nibble = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      nibble = c - 'A' + 10;
    } else {
      return -1;
    }
    value = (value << 4) | (uint64_t)nibble;
  }
  *bits = value;
  return 0;
}

/**
 * @brief Load one fixture file (format: oracle README "Fixture File Format")
 * @return 0 on success; -1 with a message on stderr otherwise
 */
static inline int oracle_load(const char *path, struct oracle_fixture *fx) {
  memset(fx, 0, sizeof(*fx));
  snprintf(fx->path, sizeof(fx->path), "%s", path);

  FILE *file = fopen(path, "r");
  if (file == NULL) {
    fprintf(stderr, "oracle: cannot open %s\n", path);
    return -1;
  }

  char buffer[1024];
  int statement = 0;
  int line_number = 0;
  int status = 0;
  while (status == 0 && fgets(buffer, sizeof(buffer), file) != NULL) {
    line_number++;
    char *line = oracle_trim(buffer);
    if (line[0] == '\0') {
      continue;
    }
    statement++;
    if (statement == 1) {
      if (strcmp(line, ORACLE_FORMAT_LINE) != 0) {
        fprintf(stderr, "oracle: %s:%d: expected '%s'\n", path, line_number, ORACLE_FORMAT_LINE);
        status = -1;
      }
      continue;
    }
    if (statement == 2) {
      if (strncmp(line, "routine ", 8) != 0) {
        fprintf(stderr, "oracle: %s:%d: expected 'routine <name>'\n", path, line_number);
        status = -1;
      } else {
        snprintf(fx->routine, sizeof(fx->routine), "%s", line + 8);
      }
      continue;
    }

    char type[8], name[ORACLE_MAX_NAME], value[64];
    if (sscanf(line, "%7s %127s %63s", type, name, value) != 3 ||
        fx->num_fields >= ORACLE_MAX_FIELDS) {
      fprintf(stderr, "oracle: %s:%d: malformed field statement\n", path, line_number);
      status = -1;
      continue;
    }
    for (int i = 0; i < fx->num_fields; i++) {
      if (strcmp(fx->fields[i].name, name) == 0) {
        fprintf(stderr, "oracle: %s:%d: duplicate field '%s'\n", path, line_number, name);
        status = -1;
      }
    }
    struct oracle_field *field = &fx->fields[fx->num_fields];
    snprintf(field->name, sizeof(field->name), "%s", name);
    if (strcmp(type, "f32") == 0) {
      field->type = ORACLE_F32;
      status = (status == 0) ? oracle_parse_bits(value, 8, &field->bits) : status;
    } else if (strcmp(type, "f64") == 0) {
      field->type = ORACLE_F64;
      status = (status == 0) ? oracle_parse_bits(value, 16, &field->bits) : status;
    } else if (strcmp(type, "i32") == 0) {
      char *end = NULL;
      field->type = ORACLE_I32;
      field->ivalue = strtoll(value, &end, 10);
      if (end == value || *end != '\0') {
        status = -1;
      }
    } else {
      status = -1;
    }
    if (status != 0) {
      fprintf(stderr, "oracle: %s:%d: bad field '%s %s %s'\n", path, line_number, type, name,
              value);
    }
    fx->num_fields++;
  }
  fclose(file);

  if (status == 0 && statement < 2) {
    fprintf(stderr, "oracle: %s: missing format or routine line\n", path);
    status = -1;
  }
  return status;
}

/**
 * @brief The fixtures fixtures.manifest lists for one module
 *
 * @param module    Owning module name
 * @param names     Output: fixture names, in manifest order
 * @param declared  Output: the count the 'module <module> <count>' line declares (-1 if absent)
 * @return Number of 'fixture <module> <name>' lines read, or -1 if the manifest is unreadable
 */
static inline int oracle_manifest_fixtures(const char *module,
                                           char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME],
                                           int *declared) {
  *declared = -1;
  FILE *file = fopen(ORACLE_MANIFEST, "r");
  if (file == NULL) {
    fprintf(stderr, "oracle: cannot open %s\n", ORACLE_MANIFEST);
    return -1;
  }

  char buffer[512];
  int count = 0;
  while (fgets(buffer, sizeof(buffer), file) != NULL) {
    char *line = oracle_trim(buffer);
    char keyword[16], owner[ORACLE_MAX_NAME], value[ORACLE_MAX_NAME];
    if (line[0] == '\0' || sscanf(line, "%15s %127s %127s", keyword, owner, value) != 3 ||
        strcmp(owner, module) != 0) {
      continue;
    }
    if (strcmp(keyword, "module") == 0) {
      *declared = atoi(value);
    } else if (strcmp(keyword, "fixture") == 0 && count < ORACLE_MAX_FIXTURES) {
      snprintf(names[count], ORACLE_MAX_NAME, "%s", value);
      count++;
    }
  }
  fclose(file);
  return count;
}

/** @brief Path of a module's fixture file: <model>/modules/<module>/_tests/oracle/<name><ext> */
static inline void oracle_fixture_path(char *buffer, const size_t size, const char *module,
                                       const char *name, const char *ext) {
  snprintf(buffer, size, "%s/%s/_tests/oracle/%s%s", ORACLE_DIR, module, name, ext);
}

/**
 * @brief How many of the module's listed fixtures have both their .in and .out on disk
 *
 * @param listed    Output: fixture lines in the manifest for the module (-1 if unreadable)
 * @param declared  Output: the module's declared count (-1 if absent)
 */
static inline int oracle_count_present(const char *module, int *listed, int *declared) {
  char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME];
  *listed = oracle_manifest_fixtures(module, names, declared);
  int found = 0;
  for (int i = 0; i < *listed; i++) {
    int present = 1;
    const char *exts[2] = {".in", ".out"};
    for (int e = 0; e < 2; e++) {
      char path[ORACLE_MAX_PATH];
      oracle_fixture_path(path, sizeof(path), module, names[i], exts[e]);
      FILE *file = fopen(path, "r");
      if (file == NULL) {
        fprintf(stderr, "oracle: %s is missing\n", path);
        present = 0;
      } else {
        fclose(file);
      }
    }
    found += present;
  }
  return found;
}

/**
 * @brief Run a per-fixture check over every fixture the manifest lists for a module
 *
 * @param check     Returns the number of mismatches and errors for one fixture name
 * @param listed    Output: fixture lines in the manifest for the module
 * @param declared  Output: the module's declared count
 * @return Number of fixtures that reproduced the oracle bit for bit
 */
static inline int oracle_run_all(const char *module, int (*check)(const char *name), int *listed,
                                 int *declared) {
  char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME];
  *listed = oracle_manifest_fixtures(module, names, declared);
  int identical = 0;
  for (int i = 0; i < *listed; i++) {
    const int problems = check(names[i]);
    if (problems == 0) {
      identical++;
      printf("\n  oracle: %s/%s bit-identical", module, names[i]);
    } else {
      printf("\n  oracle: %s/%s DIFFERS (%d problems; details on stderr)", module, names[i],
             problems);
    }
  }
  printf("\n  oracle: %s bit-identical %d of %d listed fixtures (manifest declares %d) ", module,
         identical, *listed, *declared);
  return identical;
}

/**
 * @brief Load a fixture's .in and .out and check both name the expected routine
 * @return 0 on success, 1 otherwise
 */
static inline int oracle_load_pair(const char *module, const char *name, const char *routine,
                                   struct oracle_fixture *in, struct oracle_fixture *out) {
  char path[ORACLE_MAX_PATH];
  oracle_fixture_path(path, sizeof(path), module, name, ".in");
  if (oracle_load(path, in) != 0) {
    return 1;
  }
  oracle_fixture_path(path, sizeof(path), module, name, ".out");
  if (oracle_load(path, out) != 0) {
    return 1;
  }
  if (strcmp(in->routine, routine) != 0 || strcmp(out->routine, routine) != 0) {
    fprintf(stderr, "oracle: %s/%s: routine is not %s\n", module, name, routine);
    return 1;
  }
  return 0;
}

// ============================================================================
// FIELD ACCESS
// ============================================================================

/** @brief Find a field by name and type, marking it consumed; records an error if absent */
static inline const struct oracle_field *oracle_take(struct oracle_fixture *fx, const char *name,
                                                     const enum oracle_type type) {
  for (int i = 0; i < fx->num_fields; i++) {
    if (strcmp(fx->fields[i].name, name) == 0) {
      if (fx->fields[i].type != type) {
        fprintf(stderr, "oracle: %s: field '%s' has the wrong type\n", fx->path, name);
        fx->errors++;
        return NULL;
      }
      fx->fields[i].consumed = 1;
      return &fx->fields[i];
    }
  }
  fprintf(stderr, "oracle: %s: missing field '%s'\n", fx->path, name);
  fx->errors++;
  return NULL;
}

/** @brief Whether a field of that name exists (does not consume it) */
static inline int oracle_has(const struct oracle_fixture *fx, const char *name) {
  for (int i = 0; i < fx->num_fields; i++) {
    if (strcmp(fx->fields[i].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static inline float oracle_in_f32(struct oracle_fixture *fx, const char *name) {
  const struct oracle_field *field = oracle_take(fx, name, ORACLE_F32);
  return field != NULL ? oracle_bits_to_f32(field->bits) : 0.0f;
}

static inline double oracle_in_f64(struct oracle_fixture *fx, const char *name) {
  const struct oracle_field *field = oracle_take(fx, name, ORACLE_F64);
  return field != NULL ? oracle_bits_to_f64(field->bits) : 0.0;
}

static inline int oracle_in_i32(struct oracle_fixture *fx, const char *name) {
  const struct oracle_field *field = oracle_take(fx, name, ORACLE_I32);
  return field != NULL ? (int)field->ivalue : 0;
}

/** @brief "galaxy[<i>].<member>" */
static inline const char *oracle_galaxy_field(const int index, const char *member) {
  static char buffer[ORACLE_MAX_NAME];
  snprintf(buffer, sizeof(buffer), "galaxy[%d].%s", index, member);
  return buffer;
}

// ============================================================================
// COMPARISON
// ============================================================================

/** @brief Compare a float with the oracle output bit for bit; returns 1 on a mismatch */
static inline int oracle_check_f32(struct oracle_fixture *out, const char *name,
                                   const float actual) {
  const struct oracle_field *field = oracle_take(out, name, ORACLE_F32);
  if (field == NULL) {
    return 1;
  }
  const uint32_t expected = (uint32_t)field->bits;
  const uint32_t got = oracle_f32_to_bits(actual);
  if (expected != got) {
    fprintf(stderr, "oracle: %s: %s expected 0x%08" PRIx32 " (%.9g) got 0x%08" PRIx32 " (%.9g)\n",
            out->path, name, expected, (double)oracle_bits_to_f32(expected), got, (double)actual);
    return 1;
  }
  return 0;
}

/** @brief Compare a double with the oracle output bit for bit; returns 1 on a mismatch */
static inline int oracle_check_f64(struct oracle_fixture *out, const char *name,
                                   const double actual) {
  const struct oracle_field *field = oracle_take(out, name, ORACLE_F64);
  if (field == NULL) {
    return 1;
  }
  const uint64_t got = oracle_f64_to_bits(actual);
  if (field->bits != got) {
    fprintf(stderr,
            "oracle: %s: %s expected 0x%016" PRIx64 " (%.17g) got 0x%016" PRIx64 " (%.17g)\n",
            out->path, name, field->bits, oracle_bits_to_f64(field->bits), got, actual);
    return 1;
  }
  return 0;
}

/** @brief Compare an integer with the oracle output; returns 1 on a mismatch */
static inline int oracle_check_i32(struct oracle_fixture *out, const char *name, const int actual) {
  const struct oracle_field *field = oracle_take(out, name, ORACLE_I32);
  if (field == NULL) {
    return 1;
  }
  if (field->ivalue != actual) {
    fprintf(stderr, "oracle: %s: %s expected %lld got %d\n", out->path, name, field->ivalue,
            actual);
    return 1;
  }
  return 0;
}

/**
 * @brief Mark an output field deliberately not asserted
 *
 * Only for a value that is not package state (for example a SAGE26 local the
 * package never forms); the test states the reason at the call.
 */
static inline void oracle_exclude(struct oracle_fixture *out, const char *name) {
  for (int i = 0; i < out->num_fields; i++) {
    if (strcmp(out->fields[i].name, name) == 0) {
      out->fields[i].consumed = 1;
      return;
    }
  }
  fprintf(stderr, "oracle: %s: excluded field '%s' is absent\n", out->path, name);
  out->errors++;
}

/** @brief Report every field nobody consumed; returns their number plus any access errors */
static inline int oracle_report_unconsumed(const struct oracle_fixture *fx) {
  int count = fx->errors;
  for (int i = 0; i < fx->num_fields; i++) {
    if (!fx->fields[i].consumed) {
      fprintf(stderr, "oracle: %s: field '%s' was not applied or asserted\n", fx->path,
              fx->fields[i].name);
      count++;
    }
  }
  return count;
}

// ============================================================================
// MIMIC-SIDE SETUP
// ============================================================================

/**
 * @brief Set (or add) a model parameter in MimicConfig to exactly this double
 *
 * "%.17g" round-trips a double through strtod exactly, so the module reads the
 * fixture's bit pattern.
 *
 * @return 0 on success; -1 (with a message on stderr) when adding it would exceed
 *         MAX_MODEL_PARAMS, which the caller must count as a failure
 */
static inline int oracle_set_model_param(const char *name, const double value) {
  int index = -1;
  for (int i = 0; i < MimicConfig.NumModelParams; i++) {
    if (strcmp(MimicConfig.ModelParams[i].param_name, name) == 0) {
      index = i;
    }
  }
  if (index == -1) {
    if (MimicConfig.NumModelParams < 0 || MimicConfig.NumModelParams >= MAX_MODEL_PARAMS) {
      fprintf(stderr,
              "oracle: cannot add model parameter '%s': %d already set (MAX_MODEL_PARAMS %d)\n",
              name, MimicConfig.NumModelParams, MAX_MODEL_PARAMS);
      return -1;
    }
    index = MimicConfig.NumModelParams++;
    snprintf(MimicConfig.ModelParams[index].param_name,
             sizeof(MimicConfig.ModelParams[index].param_name), "%s", name);
  }
  snprintf(MimicConfig.ModelParams[index].value, sizeof(MimicConfig.ModelParams[index].value),
           "%.17g", value);
  return 0;
}

/**
 * @brief The package's float galaxy reservoir for a SAGE26 struct GALAXY member name
 *
 * SAGE26's EjectedMass and MetalsEjectedMass are the package's EjectedGas and
 * MetalsEjectedGas; the other reservoirs keep their names (oracle README "Field Names").
 *
 * @return Pointer to the member, or NULL for a name this table does not map
 */
static inline float *oracle_galaxy_float(struct GalaxyData *galaxy, const char *sage26_name) {
  struct member {
    const char *sage26;
    float *address;
  };
  const struct member table[] = {
      {"StellarMass", &galaxy->StellarMass},
      {"BlackHoleMass", &galaxy->BlackHoleMass},
      {"ColdGas", &galaxy->ColdGas},
      {"HotGas", &galaxy->HotGas},
      {"MetalsHotGas", &galaxy->MetalsHotGas},
      {"EjectedMass", &galaxy->EjectedGas},
      {"MetalsEjectedMass", &galaxy->MetalsEjectedGas},
      {"ICS", &galaxy->ICS},
      {"MetalsICS", &galaxy->MetalsICS},
      {"CGMgas", &galaxy->CGMgas},
      {"MetalsCGMgas", &galaxy->MetalsCGMgas},
      {"ICS_disrupt", &galaxy->ICS_disrupt},
      {"ICS_accrete", &galaxy->ICS_accrete},
      {"ICS_sum_mt", &galaxy->ICS_sum_mt},
      {"DiskScaleRadius", &galaxy->DiskScaleRadius},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(table[i].sage26, sage26_name) == 0) {
      return table[i].address;
    }
  }
  return NULL;
}

/** @brief Set each named float reservoir of galaxy[index] from the fixture input */
static inline void oracle_in_galaxy_floats(struct oracle_fixture *in, const int index,
                                           struct GalaxyData *galaxy, const char *const *names,
                                           const size_t count) {
  for (size_t k = 0; k < count; k++) {
    float *member = oracle_galaxy_float(galaxy, names[k]);
    if (member == NULL) {
      fprintf(stderr, "oracle: %s: no package member for '%s'\n", in->path, names[k]);
      in->errors++;
      continue;
    }
    *member = oracle_in_f32(in, oracle_galaxy_field(index, names[k]));
  }
}

/** @brief Compare each named float reservoir of galaxy[index] with the oracle output */
static inline int oracle_check_galaxy_floats(struct oracle_fixture *out, const int index,
                                             struct GalaxyData *galaxy, const char *const *names,
                                             const size_t count) {
  int mismatches = 0;
  for (size_t k = 0; k < count; k++) {
    float *member = oracle_galaxy_float(galaxy, names[k]);
    if (member == NULL) {
      fprintf(stderr, "oracle: %s: no package member for '%s'\n", out->path, names[k]);
      mismatches++;
      continue;
    }
    mismatches += oracle_check_f32(out, oracle_galaxy_field(index, names[k]), *member);
  }
  return mismatches;
}

/**
 * @brief Check each named float reservoir of galaxy[index] is bit-identical to its value before
 *
 * For fixtures whose SAGE26 call site does not call the routine: the module must
 * visibly leave the state untouched, independently of the oracle's recorded outputs.
 *
 * @return Number of reservoirs that changed (each reported on stderr)
 */
static inline int oracle_check_galaxy_unchanged(const struct oracle_fixture *fx, const int index,
                                                const struct GalaxyData *before,
                                                struct GalaxyData *after, const char *const *names,
                                                const size_t count) {
  int changed = 0;
  struct GalaxyData copy = *before;
  for (size_t k = 0; k < count; k++) {
    const float *old_member = oracle_galaxy_float(&copy, names[k]);
    const float *new_member = oracle_galaxy_float(after, names[k]);
    if (old_member == NULL || new_member == NULL) {
      fprintf(stderr, "oracle: %s: no package member for '%s'\n", fx->path, names[k]);
      changed++;
      continue;
    }
    if (oracle_f32_to_bits(*old_member) != oracle_f32_to_bits(*new_member)) {
      fprintf(stderr, "oracle: %s: galaxy[%d].%s changed although the routine was not called\n",
              fx->path, index, names[k]);
      changed++;
    }
  }
  return changed;
}

/** @brief Fill a struct with all bits set, as the oracle poisons its state (NaN / -1) */
static inline void oracle_poison(void *object, const size_t size) { memset(object, 0xff, size); }

// ============================================================================
// THE TWO STANDARD TEST CASES
// ============================================================================

/**
 * @brief Body of test_oracle_fixture_count: are all the module's listed fixtures present?
 *
 * True when the manifest declares a positive count for the module, lists that many
 * fixtures, and each has its .in and .out on disk; false (fail) when fewer are
 * found. Platform independent.
 */
static inline int oracle_fixture_count_ok(const char *module) {
  int listed = -1;
  int declared = -1;
  const int found = oracle_count_present(module, &listed, &declared);
  printf("\n  oracle: %s fixtures found %d, listed %d, manifest declares %d ", module, found,
         listed, declared);
  return declared > 0 && listed == declared && found >= declared;
}

/**
 * @brief Body of test_oracle_bit_identity: does every listed fixture reproduce the oracle?
 *
 * Runs CHECK (returning the number of mismatches for one fixture name) on every
 * fixture the manifest lists for the module; true only when the manifest lists its
 * declared count and all of them are bit-identical. The caller reports SKIP first
 * when oracle_environment_mismatch() is not NULL.
 */
static inline int oracle_all_bit_identical(const char *module, int (*check)(const char *name)) {
  int listed = -1;
  int declared = -1;
  const int identical = oracle_run_all(module, check, &listed, &declared);
  return declared > 0 && listed == declared && identical == declared;
}

#endif /* SAGE26_ORACLE_FIXTURE_H */
