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
 * - the reader's limits (ORACLE_MAX_*), each refused with a message naming it, never
 *   silently truncated; the *_in() variants take the manifest and modules directory
 *   as arguments (the reader's self-test, test_unit_sage26_oracle_reader.c);
 * - oracle_in_*() / oracle_check_*() / oracle_exclude(): read inputs and compare
 *   outputs (f32, f64, i32 and, since c13, i64), each marking the field consumed,
 *   so oracle_report_unconsumed() can prove that every input was applied and every
 *   output asserted or excluded with a stated reason;
 * - oracle_fixture_count_ok() / oracle_all_bit_identical(): the bodies of each
 *   oracle test's two test cases;
 * - oracle_c04_*(): the c04 group's (cooling and radio heating) shared state:
 *   the sixteen galaxy fields its fixtures set and the twelve it records, and its
 *   R0 parameters (oracle README "The c04 Group");
 * - oracle_c05_*(): the c05 group's (star formation, feedback, instability,
 *   histories and outflow) galaxy fields, R0 flags and carried values (oracle
 *   README "The c05 Group").
 * - oracle_c06_*(): the c06 group's merger-family galaxy block, R0 flags, carried
 *   values and branch flags, for the black-hole, wind and burst tests (oracle README
 *   "The c06 Group").
 *
 * Fixtures are found through MIMIC_COMPILED_MODEL_PATH, which is repository
 * relative; the unit runner runs from the repository root. Include this header
 * after the test framework.
 */

#ifndef SAGE26_ORACLE_FIXTURE_H
#define SAGE26_ORACLE_FIXTURE_H

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/constants.h"
#include "include/globals.h"
#include "include/types.h"
#include "shared/sage_sfr_history.h"

/*
 * The reader's limits (oracle README "Reader Limits"). Each is a hard error that names
 * the limit, never a silent truncation: a fixture file with more than ORACLE_MAX_FIELDS
 * field statements, a module with more than ORACLE_MAX_FIXTURES fixture lines (or a
 * larger declared count), a line of ORACLE_MAX_LINE characters or more, and a name or
 * value token of ORACLE_MAX_NAME characters or more are each refused.
 */
#define ORACLE_MAX_FIELDS 2048
#define ORACLE_MAX_NAME 128
#define ORACLE_MAX_FIXTURES 256
#define ORACLE_MAX_PATH 512
#define ORACLE_MAX_LINE 1024
#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define ORACLE_DIR MIMIC_COMPILED_MODEL_PATH "/modules"
#define ORACLE_MANIFEST MIMIC_COMPILED_MODEL_PATH "/modules/_tests/oracle/fixtures.manifest"

/* ORACLE_I64 (c13): a decimal signed 64-bit integer, such as a MostBoundID, which an i32 would
 * narrow and a double would round above 2^53 (oracle README "Fixture File Format") */
enum oracle_type { ORACLE_F32, ORACLE_F64, ORACLE_I32, ORACLE_I64 };

struct oracle_field {
  char name[ORACLE_MAX_NAME];
  enum oracle_type type;
  uint64_t bits;    /* f32 / f64 bit pattern */
  long long ivalue; /* i32 or i64 value */
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

/** @brief Whitespace as sscanf's %s sees it */
static inline int oracle_is_blank(const char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/**
 * @brief Read one line of a fixture or manifest file
 *
 * A line holds at most ORACLE_MAX_LINE - 1 characters before its newline. A longer
 * line is an error naming ORACLE_MAX_LINE, never split into two statements.
 *
 * @param buffer       ORACLE_MAX_LINE bytes; receives the line
 * @param line_number  1-based number of this line, for the message
 * @return 1 with the line in buffer, 0 at end of file, -1 (message on stderr) when too long
 *         or when reading fails
 */
static inline int oracle_read_line(FILE *file, char *buffer, const char *path,
                                   const int line_number) {
  if (fgets(buffer, ORACLE_MAX_LINE, file) == NULL) {
    if (ferror(file)) {
      fprintf(stderr, "oracle: %s:%d: read error\n", path, line_number);
      return -1;
    }
    return 0;
  }
  const size_t len = strlen(buffer);
  if (len == ORACLE_MAX_LINE - 1 && buffer[len - 1] != '\n') {
    const int next = fgetc(file);
    if (next != EOF && next != '\n') {
      fprintf(stderr, "oracle: %s:%d: line longer than %d characters (ORACLE_MAX_LINE %d)\n", path,
              line_number, ORACLE_MAX_LINE - 1, ORACLE_MAX_LINE);
      return -1;
    }
  }
  return 1;
}

/**
 * @brief Copy the first max whitespace-separated tokens of a trimmed statement
 *
 * Tokens after the first max are ignored, as sscanf ignores them. A copied token of
 * ORACLE_MAX_NAME characters or more is an error naming ORACLE_MAX_NAME, never cut.
 *
 * @param tokens  max buffers of ORACLE_MAX_NAME bytes
 * @return Number of tokens copied (at most max), or -1 (message on stderr)
 */
static inline int oracle_tokens(const char *line, const int max, char tokens[][ORACLE_MAX_NAME],
                                const char *path, const int line_number) {
  int count = 0;
  const char *p = line;
  while (count < max) {
    while (oracle_is_blank(*p)) {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    const char *start = p;
    while (*p != '\0' && !oracle_is_blank(*p)) {
      p++;
    }
    const size_t len = (size_t)(p - start);
    if (len >= ORACLE_MAX_NAME) {
      fprintf(stderr,
              "oracle: %s:%d: token '%.40s...' has %zu characters, more than %d "
              "(ORACLE_MAX_NAME %d)\n",
              path, line_number, start, len, ORACLE_MAX_NAME - 1, ORACLE_MAX_NAME);
      return -1;
    }
    memcpy(tokens[count], start, len);
    tokens[count][len] = '\0';
    count++;
  }
  return count;
}

/**
 * @brief Load one fixture file (format: oracle README "Fixture File Format")
 *
 * Every statement is read; a file beyond any of the reader's limits (more than
 * ORACLE_MAX_FIELDS fields, a line of ORACLE_MAX_LINE characters or more, a token of
 * ORACLE_MAX_NAME or more, an i32 outside the 32-bit range, an i64 outside the signed
 * 64-bit range) is refused with a message naming the limit.
 *
 * @return 0 on success; -1 with a message on stderr otherwise
 */
static inline int oracle_load(const char *path, struct oracle_fixture *fx) {
  memset(fx, 0, sizeof(*fx));
  if (snprintf(fx->path, sizeof(fx->path), "%s", path) >= (int)sizeof(fx->path)) {
    fprintf(stderr, "oracle: path '%.60s...' is longer than %d characters (ORACLE_MAX_PATH %d)\n",
            path, ORACLE_MAX_PATH - 1, ORACLE_MAX_PATH);
    return -1;
  }

  FILE *file = fopen(path, "r");
  if (file == NULL) {
    fprintf(stderr, "oracle: cannot open %s\n", path);
    return -1;
  }

  char buffer[ORACLE_MAX_LINE];
  int statement = 0;
  int line_number = 0;
  int status = 0;
  while (status == 0) {
    const int got = oracle_read_line(file, buffer, path, line_number + 1);
    if (got <= 0) {
      status = got;
      break;
    }
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
      } else if (strlen(line + 8) >= sizeof(fx->routine)) {
        fprintf(stderr,
                "oracle: %s:%d: routine name longer than %d characters (ORACLE_MAX_NAME %d)\n",
                path, line_number, ORACLE_MAX_NAME - 1, ORACLE_MAX_NAME);
        status = -1;
      } else {
        snprintf(fx->routine, sizeof(fx->routine), "%s", line + 8);
      }
      continue;
    }

    char tokens[3][ORACLE_MAX_NAME];
    const int ntokens = oracle_tokens(line, 3, tokens, path, line_number);
    if (ntokens < 0) {
      status = -1;
      continue;
    }
    if (ntokens != 3) {
      fprintf(stderr, "oracle: %s:%d: malformed field statement\n", path, line_number);
      status = -1;
      continue;
    }
    if (fx->num_fields >= ORACLE_MAX_FIELDS) {
      fprintf(stderr, "oracle: %s:%d: more than %d field statements (ORACLE_MAX_FIELDS %d)\n", path,
              line_number, ORACLE_MAX_FIELDS, ORACLE_MAX_FIELDS);
      status = -1;
      continue;
    }
    const char *type = tokens[0];
    const char *name = tokens[1];
    const char *value = tokens[2];
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
      errno = 0;
      field->ivalue = strtoll(value, &end, 10);
      if (end == value || *end != '\0' || errno != 0 || field->ivalue < INT32_MIN ||
          field->ivalue > INT32_MAX) {
        status = -1;
      }
    } else if (strcmp(type, "i64") == 0) {
      /* The full signed 64-bit range: strtoll sets ERANGE outside it (it never wraps), and the
       * whole token must be decimal digits with an optional sign */
      char *end = NULL;
      field->type = ORACLE_I64;
      errno = 0;
      field->ivalue = strtoll(value, &end, 10);
      if (end == value || *end != '\0' || errno != 0) {
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
 * @brief The fixtures a manifest file lists for one module
 *
 * Every line is read. A manifest beyond the reader's limits is refused with a message
 * naming the limit and -1: more than ORACLE_MAX_FIXTURES fixture lines for the module,
 * a declared count above ORACLE_MAX_FIXTURES, a line of ORACLE_MAX_LINE characters or
 * more, or a token of ORACLE_MAX_NAME characters or more. A declared count that is not
 * a whole number is refused too.
 *
 * @param manifest  Path of the manifest file
 * @param module    Owning module name
 * @param names     Output: fixture names, in manifest order
 * @param declared  Output: the count the 'module <module> <count>' line declares (-1 if absent)
 * @return Number of 'fixture <module> <name>' lines read, or -1 if the manifest is unreadable
 *         or beyond a limit
 */
static inline int oracle_manifest_fixtures_in(const char *manifest, const char *module,
                                              char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME],
                                              int *declared) {
  *declared = -1;
  FILE *file = fopen(manifest, "r");
  if (file == NULL) {
    fprintf(stderr, "oracle: cannot open %s\n", manifest);
    return -1;
  }

  char buffer[ORACLE_MAX_LINE];
  int count = 0;
  int line_number = 0;
  int status = 0;
  while (status == 0) {
    const int got = oracle_read_line(file, buffer, manifest, line_number + 1);
    if (got <= 0) {
      status = got;
      break;
    }
    line_number++;
    char *line = oracle_trim(buffer);
    if (line[0] == '\0') {
      continue;
    }
    char tokens[3][ORACLE_MAX_NAME];
    const int ntokens = oracle_tokens(line, 3, tokens, manifest, line_number);
    if (ntokens < 0) {
      status = -1;
      break;
    }
    const char *keyword = tokens[0];
    const char *owner = tokens[1];
    const char *value = tokens[2];
    if (ntokens != 3 || strcmp(owner, module) != 0) {
      continue;
    }
    if (strcmp(keyword, "module") == 0) {
      char *end = NULL;
      errno = 0;
      const long count_declared = strtol(value, &end, 10);
      if (end == value || *end != '\0' || errno != 0 || count_declared < 0) {
        fprintf(stderr, "oracle: %s:%d: module %s declares a bad count '%s'\n", manifest,
                line_number, module, value);
        status = -1;
      } else if (count_declared > ORACLE_MAX_FIXTURES) {
        fprintf(stderr,
                "oracle: %s:%d: module %s declares %ld fixtures, more than %d "
                "(ORACLE_MAX_FIXTURES %d)\n",
                manifest, line_number, module, count_declared, ORACLE_MAX_FIXTURES,
                ORACLE_MAX_FIXTURES);
        status = -1;
      } else {
        *declared = (int)count_declared;
      }
    } else if (strcmp(keyword, "fixture") == 0) {
      if (count >= ORACLE_MAX_FIXTURES) {
        fprintf(stderr,
                "oracle: %s:%d: module %s lists more than %d fixtures (ORACLE_MAX_FIXTURES %d)\n",
                manifest, line_number, module, ORACLE_MAX_FIXTURES, ORACLE_MAX_FIXTURES);
        status = -1;
      } else {
        snprintf(names[count], ORACLE_MAX_NAME, "%s", value);
        count++;
      }
    }
  }
  fclose(file);
  return status == 0 ? count : -1;
}

/** @brief oracle_manifest_fixtures_in() on the oracle's fixtures.manifest */
static inline int oracle_manifest_fixtures(const char *module,
                                           char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME],
                                           int *declared) {
  return oracle_manifest_fixtures_in(ORACLE_MANIFEST, module, names, declared);
}

/**
 * @brief Path of a module's fixture file under a modules directory:
 *        <dir>/<module>/_tests/oracle/<name><ext>
 *
 * Every caller passes an ORACLE_MAX_PATH buffer; a path that does not fit is refused with a
 * message naming ORACLE_MAX_PATH, and the emptied buffer can never open a truncated path.
 *
 * @return 0 on success; -1 (message on stderr, buffer emptied) when the path does not fit
 */
static inline int oracle_fixture_path_in(char *buffer, const size_t size, const char *dir,
                                         const char *module, const char *name, const char *ext) {
  const int written = snprintf(buffer, size, "%s/%s/_tests/oracle/%s%s", dir, module, name, ext);
  if (written < 0 || (size_t)written >= size) {
    fprintf(stderr, "oracle: path of %s/%s%s is longer than %zu characters (ORACLE_MAX_PATH %d)\n",
            module, name, ext, size - 1, ORACLE_MAX_PATH);
    buffer[0] = '\0';
    return -1;
  }
  return 0;
}

/** @brief Path of a module's fixture file: <model>/modules/<module>/_tests/oracle/<name><ext> */
static inline void oracle_fixture_path(char *buffer, const size_t size, const char *module,
                                       const char *name, const char *ext) {
  (void)oracle_fixture_path_in(buffer, size, ORACLE_DIR, module, name, ext);
}

/**
 * @brief How many of the module's listed fixtures have both their .in and .out on disk
 *
 * @param manifest  Path of the manifest file
 * @param dir       Modules directory holding <module>/_tests/oracle/
 * @param listed    Output: fixture lines in the manifest for the module (-1 if unreadable)
 * @param declared  Output: the module's declared count (-1 if absent)
 */
static inline int oracle_count_present_in(const char *manifest, const char *dir, const char *module,
                                          int *listed, int *declared) {
  /* static: ORACLE_MAX_FIXTURES names are 32 KB, kept off the stack (as in oracle_run_all) */
  static char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME];
  *listed = oracle_manifest_fixtures_in(manifest, module, names, declared);
  int found = 0;
  for (int i = 0; i < *listed; i++) {
    int present = 1;
    const char *exts[2] = {".in", ".out"};
    for (int e = 0; e < 2; e++) {
      char path[ORACLE_MAX_PATH];
      if (oracle_fixture_path_in(path, sizeof(path), dir, module, names[i], exts[e]) != 0) {
        present = 0;
        continue;
      }
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

/** @brief oracle_count_present_in() on the oracle's fixtures.manifest and modules directory */
static inline int oracle_count_present(const char *module, int *listed, int *declared) {
  return oracle_count_present_in(ORACLE_MANIFEST, ORACLE_DIR, module, listed, declared);
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
  /* static: ORACLE_MAX_FIXTURES names are 32 KB, kept off the stack (as in oracle_count_present_in)
   */
  static char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME];
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

/** @brief Read an i64 field (a signed 64-bit integer, never narrowed or carried as a double) */
static inline long long oracle_in_i64(struct oracle_fixture *fx, const char *name) {
  const struct oracle_field *field = oracle_take(fx, name, ORACLE_I64);
  return field != NULL ? field->ivalue : 0;
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

/** @brief Compare a 64-bit integer with an i64 oracle output; returns 1 on a mismatch */
static inline int oracle_check_i64(struct oracle_fixture *out, const char *name,
                                   const long long actual) {
  const struct oracle_field *field = oracle_take(out, name, ORACLE_I64);
  if (field == NULL) {
    return 1;
  }
  if (field->ivalue != actual) {
    fprintf(stderr, "oracle: %s: %s expected %lld got %lld\n", out->path, name, field->ivalue,
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
 * MetalsEjectedGas, and its r_heat is the package's Rheat; the other members keep
 * their names (oracle README "Field Names").
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
      /* c04 group */
      {"MetalsColdGas", &galaxy->MetalsColdGas},
      {"r_heat", &galaxy->Rheat},
      {"tcool", &galaxy->tcool},
      {"tff", &galaxy->tff},
      {"RcoolToRvir", &galaxy->RcoolToRvir},
      {"CoolingRate", &galaxy->CoolingRate},
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
 * found. Platform independent. A manifest beyond the reader's limits lists -1 and fails.
 *
 * @param manifest  Path of the manifest file
 * @param dir       Modules directory holding <module>/_tests/oracle/
 */
static inline int oracle_fixture_count_ok_in(const char *manifest, const char *dir,
                                             const char *module) {
  int listed = -1;
  int declared = -1;
  const int found = oracle_count_present_in(manifest, dir, module, &listed, &declared);
  printf("\n  oracle: %s fixtures found %d, listed %d, manifest declares %d ", module, found,
         listed, declared);
  return declared > 0 && listed == declared && found >= declared;
}

/** @brief oracle_fixture_count_ok_in() on the oracle's fixtures.manifest and modules directory */
static inline int oracle_fixture_count_ok(const char *module) {
  return oracle_fixture_count_ok_in(ORACLE_MANIFEST, ORACLE_DIR, module);
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

// ============================================================================
// THE C04 GROUP: COOLING AND RADIO HEATING
// ============================================================================

/** @brief "<prefix>galaxy[<i>].<member>", e.g. the sequence state "intermediate.sequence." */
static inline const char *oracle_prefixed_galaxy_field(const char *prefix, const int index,
                                                       const char *member) {
  static char buffer[ORACLE_MAX_NAME];
  snprintf(buffer, sizeof(buffer), "%sgalaxy[%d].%s", prefix, index, member);
  return buffer;
}

/** @brief The package's double galaxy member for a SAGE26 name (Cooling, Heating), or NULL */
static inline double *oracle_galaxy_double(struct GalaxyData *galaxy, const char *sage26_name) {
  if (strcmp(sage26_name, "Cooling") == 0) {
    return &galaxy->Cooling;
  }
  if (strcmp(sage26_name, "Heating") == 0) {
    return &galaxy->Heating;
  }
  return NULL;
}

/** @brief The ten float fields of the c04 galaxy state (SAGE26 names) */
static inline const char *const *oracle_c04_float_names(size_t *count) {
  static const char *const names[] = {"HotGas",        "MetalsHotGas", "ColdGas", "MetalsColdGas",
                                      "BlackHoleMass", "r_heat",       "tcool",   "tff",
                                      "RcoolToRvir",   "CoolingRate"};
  *count = sizeof(names) / sizeof(names[0]);
  return names;
}

/** @brief The two double fields of the c04 galaxy state (SAGE26 names) */
static inline const char *const *oracle_c04_double_names(size_t *count) {
  static const char *const names[] = {"Cooling", "Heating"};
  *count = sizeof(names) / sizeof(names[0]);
  return names;
}

/**
 * @brief Set halo and galaxy from a c04 fixture's galaxy[index] input
 *
 * Applies the sixteen fields every cooling_recipe_hot, do_AGN_heating and
 * cool_gas_onto_galaxy input sets: Type, and SAGE26's float Mvir, Rvir and Vvir
 * exactly widened to the core's doubles (halo), then the ten float and two double
 * fields of the galaxy state. The caller poisons both structs first.
 */
static inline void oracle_c04_in_galaxy(struct oracle_fixture *in, const int index,
                                        struct Halo *halo, struct GalaxyData *galaxy) {
  halo->galaxy = galaxy;
  halo->Type = oracle_in_i32(in, oracle_galaxy_field(index, "Type"));
  halo->Mvir = oracle_in_f32(in, oracle_galaxy_field(index, "Mvir"));
  halo->Rvir = oracle_in_f32(in, oracle_galaxy_field(index, "Rvir"));
  halo->Vvir = oracle_in_f32(in, oracle_galaxy_field(index, "Vvir"));

  size_t count;
  const char *const *floats = oracle_c04_float_names(&count);
  oracle_in_galaxy_floats(in, index, galaxy, floats, count);
  const char *const *doubles = oracle_c04_double_names(&count);
  for (size_t k = 0; k < count; k++) {
    *oracle_galaxy_double(galaxy, doubles[k]) =
        oracle_in_f64(in, oracle_galaxy_field(index, doubles[k]));
  }
}

/**
 * @brief Compare the named c04 state fields of a galaxy with "<prefix>galaxy[index].*"
 *
 * @param names  SAGE26 names, each a float (oracle_galaxy_float) or double
 *               (oracle_galaxy_double) member
 * @return Number of mismatches
 */
static inline int oracle_c04_check_fields(struct oracle_fixture *out, const char *prefix,
                                          const int index, struct GalaxyData *galaxy,
                                          const char *const *names, const size_t count) {
  int mismatches = 0;
  for (size_t k = 0; k < count; k++) {
    const char *field = oracle_prefixed_galaxy_field(prefix, index, names[k]);
    float *member = oracle_galaxy_float(galaxy, names[k]);
    double *member64 = oracle_galaxy_double(galaxy, names[k]);
    if (member != NULL) {
      mismatches += oracle_check_f32(out, field, *member);
    } else if (member64 != NULL) {
      mismatches += oracle_check_f64(out, field, *member64);
    } else {
      fprintf(stderr, "oracle: %s: no package member for '%s'\n", out->path, names[k]);
      mismatches++;
    }
  }
  return mismatches;
}

/** @brief Compare all twelve c04 state fields with "<prefix>galaxy[index].*" */
static inline int oracle_c04_check_galaxy(struct oracle_fixture *out, const char *prefix,
                                          const int index, struct GalaxyData *galaxy) {
  size_t count;
  const char *const *floats = oracle_c04_float_names(&count);
  int mismatches = oracle_c04_check_fields(out, prefix, index, galaxy, floats, count);
  const char *const *doubles = oracle_c04_double_names(&count);
  mismatches += oracle_c04_check_fields(out, prefix, index, galaxy, doubles, count);
  return mismatches;
}

/**
 * @brief Apply a c04 fixture's R0 parameters (cooling_recipe_hot and do_AGN_heating inputs)
 *
 * Checks the fixed flags (AGNrecipeOn 2, CGMrecipeOn 0), sets the core-owned G,
 * Hubble_h and unit factors in MimicConfig at the fixture's bit patterns, and the
 * radio module's parameters RadioModeEfficiency and AGNrecipe. UnitDensity_in_cgs
 * is read only when the fixture carries it (cooling_recipe_hot).
 *
 * @return Number of problems (a non-R0 flag or a parameter that could not be set)
 */
static inline int oracle_c04_set_params(struct oracle_fixture *in, const char *name) {
  int problems = 0;
  const int agn_recipe = oracle_in_i32(in, "params.AGNrecipeOn");
  if (agn_recipe != 2 || oracle_in_i32(in, "params.CGMrecipeOn") != 0) {
    fprintf(stderr, "oracle: %s: not an R0 fixture (AGNrecipeOn 2, CGMrecipeOn 0)\n", name);
    problems++;
  }
  MimicConfig.G = oracle_in_f64(in, "params.G");
  MimicConfig.Hubble_h = oracle_in_f64(in, "params.Hubble_h");
  MimicConfig.UnitEnergy_in_cgs = oracle_in_f64(in, "params.UnitEnergy_in_cgs");
  MimicConfig.UnitTime_in_s = oracle_in_f64(in, "params.UnitTime_in_s");
  if (oracle_has(in, "params.UnitDensity_in_cgs")) {
    MimicConfig.UnitDensity_in_cgs = oracle_in_f64(in, "params.UnitDensity_in_cgs");
  }
  if (oracle_set_model_param("RadioModeEfficiency",
                             oracle_in_f64(in, "params.RadioModeEfficiency")) != 0 ||
      oracle_set_model_param("AGNrecipe", (double)agn_recipe) != 0) {
    problems++;
  }
  return problems;
}

// ============================================================================
// THE C05 GROUP: STAR FORMATION, FEEDBACK, INSTABILITY, HISTORIES AND OUTFLOW
// ============================================================================

/**
 * @brief The package's float galaxy member for a c05 SAGE26 name, or NULL
 *
 * Extends oracle_galaxy_float() with the c05 fields: SAGE26's OutflowRate is the
 * package's SupernovaOutflowRate (oracle README "Input Fields"); the other names are
 * the package's.
 */
static inline float *oracle_c05_galaxy_float(struct GalaxyData *galaxy, const char *sage26_name) {
  struct member {
    const char *sage26;
    float *address;
  };
  const struct member table[] = {
      {"MetalsStellarMass", &galaxy->MetalsStellarMass},
      {"BulgeMass", &galaxy->BulgeMass},
      {"MetalsBulgeMass", &galaxy->MetalsBulgeMass},
      {"InstabilityBulgeMass", &galaxy->InstabilityBulgeMass},
      {"H1gas", &galaxy->H1gas},
      {"H2gas", &galaxy->H2gas},
      {"OutflowRate", &galaxy->SupernovaOutflowRate},
      {"QuasarModeBHaccretionMass", &galaxy->QuasarModeBHaccretionMass},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(table[i].sage26, sage26_name) == 0) {
      return table[i].address;
    }
  }
  return oracle_galaxy_float(galaxy, sage26_name);
}

/** @brief The fourteen fields every c05 output records for every galaxy (SAGE26 names) */
static inline const char *const *oracle_c05_written_names(size_t *count) {
  static const char *const names[] = {
      "ColdGas",      "MetalsColdGas",   "StellarMass",          "MetalsStellarMass",
      "BulgeMass",    "MetalsBulgeMass", "InstabilityBulgeMass", "HotGas",
      "MetalsHotGas", "EjectedMass",     "MetalsEjectedMass",    "H1gas",
      "H2gas",        "OutflowRate"};
  *count = sizeof(names) / sizeof(names[0]);
  return names;
}

/** @brief The six SAGE26 per-substep histories (ten elements each, R/src/core_allvars.h:158-163) */
static inline const char *const *oracle_c05_history_names(size_t *count) {
  static const char *const names[] = {"SfrDisk",         "SfrBulge",
                                      "SfrDiskColdGas",  "SfrDiskColdGasMetals",
                                      "SfrBulgeColdGas", "SfrBulgeColdGasMetals"};
  *count = sizeof(names) / sizeof(names[0]);
  return names;
}

#define ORACLE_C05_STEPS 10

/**
 * @brief Set a galaxy's named c05 float fields from "galaxy[index].<name>"
 * @return Number of names with no package member
 */
static inline int oracle_c05_in_floats(struct oracle_fixture *in, const int index,
                                       struct GalaxyData *galaxy, const char *const *names,
                                       const size_t count) {
  int problems = 0;
  for (size_t k = 0; k < count; k++) {
    float *member = oracle_c05_galaxy_float(galaxy, names[k]);
    if (member == NULL) {
      fprintf(stderr, "oracle: %s: no package member for '%s'\n", in->path, names[k]);
      problems++;
      continue;
    }
    *member = oracle_in_f32(in, oracle_galaxy_field(index, names[k]));
  }
  return problems;
}

/**
 * @brief Compare a galaxy's named c05 float fields with "<prefix>galaxy[index].<name>"
 * @return Number of mismatches
 */
static inline int oracle_c05_check_floats(struct oracle_fixture *out, const char *prefix,
                                          const int index, struct GalaxyData *galaxy,
                                          const char *const *names, const size_t count) {
  int mismatches = 0;
  for (size_t k = 0; k < count; k++) {
    float *member = oracle_c05_galaxy_float(galaxy, names[k]);
    if (member == NULL) {
      fprintf(stderr, "oracle: %s: no package member for '%s'\n", out->path, names[k]);
      mismatches++;
      continue;
    }
    mismatches +=
        oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, index, names[k]), *member);
  }
  return mismatches;
}

/**
 * @brief Mark the sixty history elements of "<prefix>galaxy[index]" consumed, not asserted
 *
 * The package has no per-substep histories before they are ported; a test that
 * defers them says so at the call.
 */
static inline void oracle_c05_exclude_histories(struct oracle_fixture *fx, const char *prefix,
                                                const int index) {
  size_t count;
  const char *const *names = oracle_c05_history_names(&count);
  for (size_t h = 0; h < count; h++) {
    for (int k = 0; k < ORACLE_C05_STEPS; k++) {
      char member[ORACLE_MAX_NAME];
      snprintf(member, sizeof(member), "%s[%d]", names[h], k);
      oracle_exclude(fx, oracle_prefixed_galaxy_field(prefix, index, member));
    }
  }
}

/**
 * @brief Require one of the fixed c05 R0 flags (oracle README "Flags and Parameters")
 * @return 1 if the fixture's flag differs from the R0 value, else 0
 */
static inline int oracle_c05_require_flag(struct oracle_fixture *in, const char *flag,
                                          const int r0_value) {
  const int value = oracle_in_i32(in, flag);
  if (value != r0_value) {
    fprintf(stderr, "oracle: %s: %s is %d, not R0's %d\n", in->path, flag, value, r0_value);
    return 1;
  }
  return 0;
}

/** @brief Counters of the carried-value assertions a c05 test made, for its report */
struct oracle_c05_carried_tally {
  int asserted;      /**< validated two-sided and compared bit for bit */
  int not_validated; /**< present but not validated two-sided: recorded, not asserted */
  int not_carried;   /**< a SAGE26 local the package does not hold: not asserted */
  int absent;        /**< the path leaves it undefined */
  int binding;       /**< compared with the reservoir its binding clamp or cap set it to */
};

/**
 * @brief Read one carried value's record and assert it where the package holds it
 *
 * Reads "<prefix><name>_absent" and, when present, the value and its _checked,
 * _validated_up, _validated_down and _validated markers (oracle README "Carried
 * Values"). The value is compared bit for bit only where it is validated two-sided
 * and `actual` is not NULL (the package carries it); otherwise it is consumed
 * unasserted and counted in the tally.
 *
 * @param actual  The package's value, or NULL for a SAGE26 local the package does not hold
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c05_check_carried(struct oracle_fixture *out, const char *prefix,
                                           const char *name, const double *actual,
                                           struct oracle_c05_carried_tally *tally) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "%s%s_absent", prefix, name);
  if (oracle_in_i32(out, field) != 0) {
    tally->absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "%s%s_checked", prefix, name);
  if (oracle_in_i32(out, field) != 1) {
    fprintf(stderr, "oracle: %s: %s%s is not marked checked\n", out->path, prefix, name);
    problems++;
  }
  snprintf(field, sizeof(field), "%s%s_validated_up", prefix, name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s%s_validated_down", prefix, name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "%s%s_validated", prefix, name);
  const int validated = oracle_in_i32(out, field);
  if (validated != (up && down)) {
    fprintf(stderr, "oracle: %s: %s%s_validated disagrees with its two directions\n", out->path,
            prefix, name);
    problems++;
  }
  snprintf(field, sizeof(field), "%s%s", prefix, name);
  if (actual == NULL) {
    oracle_exclude(out, field);
    tally->not_carried++;
  } else if (validated) {
    problems += oracle_check_f64(out, field, *actual);
    tally->asserted++;
  } else {
    oracle_exclude(out, field);
    tally->not_validated++;
  }
  return problems;
}

/**
 * @brief Where a clamp or cap binds, assert its recorded value equals the reservoir it binds to
 *
 * Reads the i32 flag; when it is 1, the recorded "<prefix><value_name>" must equal
 * `reservoir`, the package's reservoir at that point (oracle README: a cap's value
 * equals the reservoir before the debit that follows, not the output field). The
 * flag itself is not asserted (strict and non-strict comparisons agree at equality).
 *
 * @return Number of mismatches
 */
static inline int oracle_c05_check_binding(struct oracle_fixture *out, const char *prefix,
                                           const char *flag, const char *value_name,
                                           const float reservoir,
                                           struct oracle_c05_carried_tally *tally) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "%s%s", prefix, flag);
  if (oracle_in_i32(out, field) != 1) {
    return 0;
  }
  snprintf(field, sizeof(field), "%s%s", prefix, value_name);
  tally->binding++;
  return oracle_check_f64(out, field, (double)reservoir);
}

// ============================================================================
// THE C05 HISTORY SUBSTRATE
// ============================================================================

/**
 * @brief The package's element of SAGE26 history `h` (oracle_c05_history_names order) at bin k
 *
 * The package holds each history element as its own float property, reached
 * through shared/sage_sfr_history.h, whose enum MimicSageSfrHistory lists the six
 * histories in the order oracle_c05_history_names() does.
 */
static inline float *oracle_c05_history_element(struct GalaxyData *galaxy, const size_t h,
                                                const int k) {
  if (h >= (size_t)MIMIC_SFR_HISTORY_COUNT) {
    return NULL;
  }
  return mimic_sage_sfr_history(galaxy, (enum MimicSageSfrHistory)h, k);
}

/**
 * @brief Set the sixty history elements of a galaxy from "galaxy[index].<history>[k]"
 * @return Number of elements with no package member
 */
static inline int oracle_c05_in_histories(struct oracle_fixture *in, const int index,
                                          struct GalaxyData *galaxy) {
  int problems = 0;
  size_t count;
  const char *const *names = oracle_c05_history_names(&count);
  for (size_t h = 0; h < count; h++) {
    for (int k = 0; k < ORACLE_C05_STEPS; k++) {
      char member[ORACLE_MAX_NAME];
      snprintf(member, sizeof(member), "%s[%d]", names[h], k);
      float *element = oracle_c05_history_element(galaxy, h, k);
      if (element == NULL) {
        fprintf(stderr, "oracle: %s: no package member for '%s'\n", in->path, member);
        problems++;
        continue;
      }
      *element = oracle_in_f32(in, oracle_galaxy_field(index, member));
    }
  }
  return problems;
}

/**
 * @brief Compare the sixty history elements of a galaxy with "<prefix>galaxy[index].<history>[k]"
 * @return Number of mismatches
 */
static inline int oracle_c05_check_histories(struct oracle_fixture *out, const char *prefix,
                                             const int index, struct GalaxyData *galaxy) {
  int mismatches = 0;
  size_t count;
  const char *const *names = oracle_c05_history_names(&count);
  for (size_t h = 0; h < count; h++) {
    for (int k = 0; k < ORACLE_C05_STEPS; k++) {
      char member[ORACLE_MAX_NAME];
      snprintf(member, sizeof(member), "%s[%d]", names[h], k);
      const float *element = oracle_c05_history_element(galaxy, h, k);
      if (element == NULL) {
        fprintf(stderr, "oracle: %s: no package member for '%s'\n", out->path, member);
        mismatches++;
        continue;
      }
      mismatches +=
          oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, index, member), *element);
    }
  }
  return mismatches;
}

/**
 * @brief Compare the writer's averaging of a galaxy with "<prefix>" + its eight recorded values
 *
 * The oracle records SAGE26's writer locals and results (R/src/io/save_gals_hdf5.c:934-959)
 * as "<prefix>sfr_rate_divisor", "sfr_metallicity_divisor", "SfrDiskZ_bins_skipped",
 * "SfrBulgeZ_bins_skipped", "SfrDisk", "SfrBulge", "SfrDiskZ" and "SfrBulgeZ". The four
 * results are the package's output scalars of the same names.
 *
 * @return Number of mismatches
 */
static inline int oracle_c05_check_writer(struct oracle_fixture *out, const char *prefix,
                                          const struct GalaxyData *galaxy,
                                          const struct MimicSageSfrAverageLocals *locals) {
  int mismatches = 0;
  char field[ORACLE_MAX_NAME];
  const struct {
    const char *name;
    int value;
  } ints[] = {{"sfr_rate_divisor", locals->sfr_rate_divisor},
              {"sfr_metallicity_divisor", locals->sfr_metallicity_divisor},
              {"SfrDiskZ_bins_skipped", locals->disk_bins_skipped},
              {"SfrBulgeZ_bins_skipped", locals->bulge_bins_skipped}};
  for (size_t k = 0; k < sizeof(ints) / sizeof(ints[0]); k++) {
    snprintf(field, sizeof(field), "%s%s", prefix, ints[k].name);
    mismatches += oracle_check_i32(out, field, ints[k].value);
  }
  const struct {
    const char *name;
    float value;
  } floats[] = {{"SfrDisk", galaxy->SfrDisk},
                {"SfrBulge", galaxy->SfrBulge},
                {"SfrDiskZ", galaxy->SfrDiskZ},
                {"SfrBulgeZ", galaxy->SfrBulgeZ}};
  for (size_t k = 0; k < sizeof(floats) / sizeof(floats[0]); k++) {
    snprintf(field, sizeof(field), "%s%s", prefix, floats[k].name);
    mismatches += oracle_check_f32(out, field, floats[k].value);
  }
  return mismatches;
}

// ============================================================================
// THE C06 GROUP: BLACK HOLES, WINDS AND BURSTS
// ============================================================================

/**
 * @brief The package's float galaxy member for a c06 SAGE26 name, or NULL
 *
 * Extends oracle_c05_galaxy_float() with the members the c06 merger-family block adds
 * (oracle README "The c06 Group", "Format Additions"): the merger channel, the three
 * bulge radii, the two merger timestamps and the merger clock. The names are the
 * package's.
 */
static inline float *oracle_c06_galaxy_float(struct GalaxyData *galaxy, const char *sage26_name) {
  struct member {
    const char *sage26;
    float *address;
  };
  const struct member table[] = {
      {"MergerBulgeMass", &galaxy->MergerBulgeMass},
      {"BulgeRadius", &galaxy->BulgeRadius},
      {"MergerBulgeRadius", &galaxy->MergerBulgeRadius},
      {"InstabilityBulgeRadius", &galaxy->InstabilityBulgeRadius},
      {"TimeOfLastMajorMerger", &galaxy->TimeOfLastMajorMerger},
      {"TimeOfLastMinorMerger", &galaxy->TimeOfLastMinorMerger},
      {"MergTime", &galaxy->MergTime},
  };
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(table[i].sage26, sage26_name) == 0) {
      return table[i].address;
    }
  }
  return oracle_c05_galaxy_float(galaxy, sage26_name);
}

/**
 * @brief The thirty float reservoirs, channels, radii and timestamps of the c06 galaxy block
 *
 * Every merger-family fixture sets them for every galaxy, and every recorded galaxy
 * block holds them (oracle README "Format Additions", SAGE26 names).
 */
static inline const char *const *oracle_c06_block_names(size_t *count) {
  static const char *const names[] = {"ColdGas",
                                      "MetalsColdGas",
                                      "StellarMass",
                                      "MetalsStellarMass",
                                      "BulgeMass",
                                      "MetalsBulgeMass",
                                      "MergerBulgeMass",
                                      "InstabilityBulgeMass",
                                      "HotGas",
                                      "MetalsHotGas",
                                      "EjectedMass",
                                      "MetalsEjectedMass",
                                      "BlackHoleMass",
                                      "QuasarModeBHaccretionMass",
                                      "ICS",
                                      "MetalsICS",
                                      "ICS_disrupt",
                                      "ICS_accrete",
                                      "ICS_sum_mt",
                                      "CGMgas",
                                      "MetalsCGMgas",
                                      "H1gas",
                                      "H2gas",
                                      "OutflowRate",
                                      "DiskScaleRadius",
                                      "BulgeRadius",
                                      "MergerBulgeRadius",
                                      "InstabilityBulgeRadius",
                                      "TimeOfLastMajorMerger",
                                      "TimeOfLastMinorMerger"};
  *count = sizeof(names) / sizeof(names[0]);
  return names;
}

/**
 * @brief Build one merger-family galaxy and its halo from "galaxy[index].*"
 *
 * Poisons both structs (as the oracle poisons its state) and then sets, from the
 * fixture: the halo's Type, SnapNum, Len, Mvir, deltaMvir, Rvir, Vvir and Vmax (SAGE26's
 * float members, given to the package's double halo fields exactly widened), the
 * galaxy's MergTime, the thirty block floats and the sixty history elements. SAGE26's
 * driver bookkeeping mergeType, mergeIntoID and CentralGal has no package galaxy member
 * and is read by no black-hole, wind or burst routine: it is consumed unapplied.
 *
 * @return Number of problems (names with no package member)
 */
static inline int oracle_c06_in_galaxy(struct oracle_fixture *in, const int index,
                                       struct Halo *halo, struct GalaxyData *galaxy) {
  int problems = 0;
  oracle_poison(halo, sizeof(*halo));
  oracle_poison(galaxy, sizeof(*galaxy));
  halo->galaxy = galaxy;
  halo->HaloNr = index;
  halo->Type = oracle_in_i32(in, oracle_galaxy_field(index, "Type"));
  halo->SnapNum = oracle_in_i32(in, oracle_galaxy_field(index, "SnapNum"));
  halo->Len = oracle_in_i32(in, oracle_galaxy_field(index, "Len"));
  halo->Mvir = oracle_in_f32(in, oracle_galaxy_field(index, "Mvir"));
  halo->deltaMvir = oracle_in_f32(in, oracle_galaxy_field(index, "deltaMvir"));
  halo->Rvir = oracle_in_f32(in, oracle_galaxy_field(index, "Rvir"));
  halo->Vvir = oracle_in_f32(in, oracle_galaxy_field(index, "Vvir"));
  halo->Vmax = oracle_in_f32(in, oracle_galaxy_field(index, "Vmax"));
  (void)oracle_in_i32(in, oracle_galaxy_field(index, "mergeType"));
  (void)oracle_in_i32(in, oracle_galaxy_field(index, "mergeIntoID"));
  (void)oracle_in_i32(in, oracle_galaxy_field(index, "CentralGal"));

  const char *const clock[] = {"MergTime"};
  size_t count;
  const char *const *block = oracle_c06_block_names(&count);
  for (int pass = 0; pass < 2; pass++) {
    const char *const *names = pass == 0 ? clock : block;
    const size_t n = pass == 0 ? 1 : count;
    for (size_t k = 0; k < n; k++) {
      float *member = oracle_c06_galaxy_float(galaxy, names[k]);
      if (member == NULL) {
        fprintf(stderr, "oracle: %s: no package member for '%s'\n", in->path, names[k]);
        problems++;
        continue;
      }
      *member = oracle_in_f32(in, oracle_galaxy_field(index, names[k]));
    }
  }
  problems += oracle_c05_in_histories(in, index, galaxy);
  return problems;
}

/**
 * @brief Compare a galaxy's thirty block floats (and, if asked, its histories) with the oracle
 * @return Number of mismatches
 */
static inline int oracle_c06_check_galaxy(struct oracle_fixture *out, const char *prefix,
                                          const int index, struct GalaxyData *galaxy,
                                          const int with_histories) {
  int mismatches = 0;
  size_t count;
  const char *const *names = oracle_c06_block_names(&count);
  for (size_t k = 0; k < count; k++) {
    float *member = oracle_c06_galaxy_float(galaxy, names[k]);
    if (member == NULL) {
      fprintf(stderr, "oracle: %s: no package member for '%s'\n", out->path, names[k]);
      mismatches++;
      continue;
    }
    mismatches +=
        oracle_check_f32(out, oracle_prefixed_galaxy_field(prefix, index, names[k]), *member);
  }
  if (with_histories) {
    mismatches += oracle_c05_check_histories(out, prefix, index, galaxy);
  }
  return mismatches;
}

/**
 * @brief Require every galaxy member outside the c06 block and histories to be unchanged
 *
 * Copies the block floats and the histories from `after` into a copy of `before` and
 * compares the whole struct, so a write to any other member (a diagnostic, a transport
 * property) is reported.
 *
 * @return 1 if another member changed, else 0
 */
static inline int oracle_c06_check_rest_unchanged(const char *path, const int index,
                                                  const struct GalaxyData *before,
                                                  struct GalaxyData *after) {
  struct GalaxyData expected = *before;
  size_t count;
  const char *const *names = oracle_c06_block_names(&count);
  for (size_t k = 0; k < count; k++) {
    *oracle_c06_galaxy_float(&expected, names[k]) = *oracle_c06_galaxy_float(after, names[k]);
  }
  for (size_t h = 0; h < (size_t)MIMIC_SFR_HISTORY_COUNT; h++) {
    for (int k = 0; k < ORACLE_C05_STEPS; k++) {
      *oracle_c05_history_element(&expected, h, k) = *oracle_c05_history_element(after, h, k);
    }
  }
  if (memcmp(&expected, after, sizeof(expected)) != 0) {
    fprintf(stderr, "oracle: %s: galaxy[%d] changed a member outside the c06 block\n", path, index);
    return 1;
  }
  return 0;
}

/** @brief Counters of the c06 carried-value assertions a test made, for its report */
struct oracle_c06_carried_tally {
  int asserted;      /**< validated two-sided and compared bit for bit */
  int binding;       /**< a binding clamp or floor: compared with the value it was set to */
  int not_validated; /**< present, checked, not validated two-sided: recorded, not asserted */
  int absent;        /**< the path leaves it undefined (and the port agrees) */
};

/**
 * @brief Read one c06 carried value's record and assert it where the oracle allows
 *
 * Reads "intermediate.<name>_absent" and requires the port's value to be NaN exactly when
 * the oracle marks it absent. When present, reads _checked (must be 1), _check_kind (must
 * be `kind`: 1 continuation, 2 tail), _validated_up, _validated_down and _validated (must
 * be their conjunction), and compares the value bit for bit when it is validated
 * two-sided or when `binds` is set (a clamp or floor bound it, so the recorded value is
 * the reservoir or zero it was set to); otherwise it is consumed unasserted.
 *
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c06_check_carried(struct oracle_fixture *out, const char *name,
                                           const int kind, const double actual, const int binds,
                                           struct oracle_c06_carried_tally *tally) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  const int absent = oracle_in_i32(out, field);
  if (absent != (isnan(actual) ? 1 : 0)) {
    fprintf(stderr, "oracle: %s: %s is %s in the oracle but %s in the port\n", out->path, name,
            absent ? "absent" : "present", isnan(actual) ? "absent" : "present");
    return 1;
  }
  if (absent) {
    tally->absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "intermediate.%s_checked", name);
  if (oracle_in_i32(out, field) != 1) {
    fprintf(stderr, "oracle: %s: %s is not marked checked\n", out->path, name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s_check_kind", name);
  if (oracle_in_i32(out, field) != kind) {
    fprintf(stderr, "oracle: %s: %s is not of check kind %d\n", out->path, name, kind);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, field);
  if (validated != (up && down)) {
    fprintf(stderr, "oracle: %s: %s_validated disagrees with its two directions\n", out->path,
            name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s", name);
  if (validated) {
    problems += oracle_check_f64(out, field, actual);
    tally->asserted++;
  } else if (binds) {
    problems += oracle_check_f64(out, field, actual);
    tally->binding++;
  } else {
    oracle_exclude(out, field);
    tally->not_validated++;
  }
  return problems;
}

/**
 * @brief Compare one c06 branch flag (i32, -1 where the path does not evaluate it)
 * @return 1 on a mismatch
 */
static inline int oracle_c06_check_flag(struct oracle_fixture *out, const char *flag,
                                        const int actual) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s", flag);
  return oracle_check_i32(out, field, actual);
}

/**
 * @brief Require the R0 flags every c06 merger-family fixture carries (oracle README
 *        "The c06 Group", "Flags and Parameters")
 * @return Number of flags that differ from R0's values
 */
static inline int oracle_c06_require_r0_flags(struct oracle_fixture *in) {
  int problems = 0;
  problems += oracle_c05_require_flag(in, "params.SFprescription", 0);
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  problems += oracle_c05_require_flag(in, "params.CGMrecipeOn", 0);
  problems += oracle_c05_require_flag(in, "params.FIREmodeOn", 0);
  problems += oracle_c05_require_flag(in, "params.BulgeSizeOn", 0);
  problems += oracle_c05_require_flag(in, "params.SaveFullSFH", 0);
  problems += oracle_c05_require_flag(in, "params.AGNrecipeOn", 2);
  problems += oracle_c05_require_flag(in, "params.StarburstColdGasOn", 1);
  problems += oracle_c05_require_flag(in, "params.DiskInstabilityOn", 1);
  return problems;
}

// ============================================================================
// THE C10 GROUP: CGMrecipeOn 1 ROUTING OF THE C03 ROUTINES
// ============================================================================

/** @brief The check kinds the oracle records (oracle README, "The c06 Group") */
#define ORACLE_C10_CONTINUATION 1
#define ORACLE_C10_TAIL 2

/** @brief Tallies of the c10 locals one fixture or test run recorded */
struct oracle_c10_tally {
  int continuation; /* present and continuation-checked */
  int tail;         /* present and tail-checked */
  int validated;    /* validated two-sided */
  int binding;      /* recorded where a cap binds, asserted equal to the capped reservoir */
  int absent;       /* not evaluated on the fixture's path */
};

/**
 * @brief Check one c10 local of a CGMrecipeOn 1 c03 fixture (oracle README, "The c10 Group")
 *
 * Reads `intermediate.<name>_absent`; for a present local requires `_checked` 1 and `_check_kind`
 * `kind`, and `_validated` equal to `_validated_up && _validated_down`. The locals are SAGE26's,
 * which the package does not carry between modules, so the value is consumed unasserted, except
 * where `binds` is set: then the cap bound it, and it must equal `reservoir`, the package's input
 * reservoir it was capped to. `value_consumed` marks a value the caller has already consumed.
 *
 * @return Number of marker errors and mismatches
 */
static inline int oracle_c10_check_local(struct oracle_fixture *out, const char *name,
                                         const int kind, const int is_f32, const int value_consumed,
                                         const int binds, const float reservoir,
                                         struct oracle_c10_tally *tally) {
  char field[ORACLE_MAX_NAME];
  snprintf(field, sizeof(field), "intermediate.%s_absent", name);
  if (oracle_in_i32(out, field) != 0) {
    tally->absent++;
    return 0;
  }
  int problems = 0;
  snprintf(field, sizeof(field), "intermediate.%s_checked", name);
  if (oracle_in_i32(out, field) != 1) {
    fprintf(stderr, "oracle: %s: %s is not marked checked\n", out->path, name);
    problems++;
  }
  snprintf(field, sizeof(field), "intermediate.%s_check_kind", name);
  if (oracle_in_i32(out, field) != kind) {
    fprintf(stderr, "oracle: %s: %s is not of check kind %d\n", out->path, name, kind);
    problems++;
  }
  if (kind == ORACLE_C10_CONTINUATION) {
    tally->continuation++;
  } else {
    tally->tail++;
  }
  snprintf(field, sizeof(field), "intermediate.%s_validated_up", name);
  const int up = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated_down", name);
  const int down = oracle_in_i32(out, field);
  snprintf(field, sizeof(field), "intermediate.%s_validated", name);
  const int validated = oracle_in_i32(out, field);
  if (validated != (up && down)) {
    fprintf(stderr, "oracle: %s: %s_validated disagrees with its two directions\n", out->path,
            name);
    problems++;
  }
  if (validated) {
    tally->validated++;
  }
  snprintf(field, sizeof(field), "intermediate.%s", name);
  if (binds) {
    tally->binding++;
    problems += is_f32 ? oracle_check_f32(out, field, reservoir)
                       : oracle_check_f64(out, field, (double)reservoir);
  } else if (!value_consumed) {
    oracle_exclude(out, field); /* a SAGE26 local, not package state */
  }
  return problems;
}

/**
 * @brief Check that each named float reservoir of galaxy[index] kept its input bits
 *
 * For a reservoir the routing did not select (CGMrecipeOn 1): the module must leave it untouched.
 *
 * @return Number of reservoirs that changed (each reported on stderr)
 */
static inline int oracle_c10_check_unselected(const char *path, const int index,
                                              const struct GalaxyData *before,
                                              struct GalaxyData *after, const char *const *names,
                                              const size_t count) {
  int changed = 0;
  struct GalaxyData copy = *before;
  for (size_t k = 0; k < count; k++) {
    const float *old_member = oracle_galaxy_float(&copy, names[k]);
    const float *new_member = oracle_galaxy_float(after, names[k]);
    if (old_member == NULL || new_member == NULL) {
      fprintf(stderr, "oracle: %s: no package member for '%s'\n", path, names[k]);
      changed++;
      continue;
    }
    if (oracle_f32_to_bits(*old_member) != oracle_f32_to_bits(*new_member)) {
      fprintf(stderr, "oracle: %s: unselected galaxy[%d].%s changed\n", path, index, names[k]);
      changed++;
    }
  }
  return changed;
}

/** @brief The CGM pair (cgm 1) or the hot pair (cgm 0), by SAGE26 name; two names */
static inline const char *const *oracle_c10_pair(const int cgm) {
  static const char *const hot[] = {"HotGas", "MetalsHotGas"};
  static const char *const cgm_pair[] = {"CGMgas", "MetalsCGMgas"};
  return cgm ? cgm_pair : hot;
}

/** @brief Print the c10 local tallies of one test run (quoted in the validation record) */
static inline void oracle_c10_print_tally(const char *module, const struct oracle_c10_tally *t) {
  printf(
      "\n  oracle c10 locals (%s): continuation-checked %d, tail-checked %d, validated two-sided "
      "%d, binding-cap values %d, absent %d ",
      module, t->continuation, t->tail, t->validated, t->binding, t->absent);
}

#endif /* SAGE26_ORACLE_FIXTURE_H */
