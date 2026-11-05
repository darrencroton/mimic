/**
 * @file    oracle_c05.c
 * @brief   SAGE26 function-level oracle for the c05 star-formation, feedback and instability
 *          routines, and the driver and writer arithmetic around them
 *
 * Reads one fixture input file, initialises the SAGE26 struct GALAXY and struct params
 * fields its reader for the selected routine consumes (a superset of the fields the routine
 * reads), calls the routine from the SAGE26 objects this executable is linked against, and
 * writes a fixture output file holding the routine's return value, every field it writes
 * and every intermediate value the Mimic package carries between modules.
 *
 * Usage: oracle_c05 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one; R/ is the staged SAGE26 source
 * root, S = R/src/model_starformation_and_feedback.c, D = R/src/model_disk_instability.c,
 * B = R/src/core_build_model.c, W = R/src/io/save_gals_hdf5.c):
 *   starformation_and_feedback           S:995-1110 with R0's flags, called as B:513-516
 *                                        calls it (step bin, dt = deltaT / effective_steps);
 *                                        SFprescription 0 (sfr_croton06) or 1 (sfr_br06 with
 *                                        radial integration, R2's flags, c08)
 *   starformation_and_feedback_sequence  the same call over every substep of one galaxy
 *                                        (B:470-516), then SubstepsUsed (B:466-467) and the
 *                                        writer's averaging (W:934-959)
 *   update_from_star_formation           S:1119-1136, called as bursts call it
 *                                        (R/src/model_mergers.c:759-760); with SFprescription 1
 *                                        its hydrogen cap (S:1131-1135) is recorded (c08)
 *   update_from_feedback                 S:1146-1251, called as bursts call it
 *                                        (R/src/model_mergers.c:790-791); with SFprescription 1
 *                                        its hydrogen cap (S:1169-1173) is recorded (c08)
 *   check_disk_instability               D:47-110
 *   step_bin_map                         a TRANSCRIPTION of B:513-516 (bin and dt)
 *   sfr_history_average                  a TRANSCRIPTION of W:934-959, with SAGE26's own
 *                                        sfr_rate_divisor and sfr_metallicity_divisor
 *                                        (R/src/model_misc.h:49-58)
 *   normalise_outflow_rate               a TRANSCRIPTION of B:590-604 (Cooling, Heating and
 *                                        OutflowRate) and of the writer's OutflowRate
 *                                        conversion (W:995)
 *
 * sfr_croton06, sfr_br06 and compute_sn_feedback are static in S, and evolve_galaxies and the
 * writer are not in this link, so every value they form that the package carries is a labelled
 * TRANSCRIPTION below. sfr_br06's radial integration is SAGE26's own public routine
 * (R/src/model_h2_chemistry.c:134-290; oracle_c08.c holds its transcription). The transcriptions of
 * S are checked: compose_wrapper() recomposes starformation_and_feedback in SAGE26's statement
 * order from the transcribed blocks and SAGE26's own public routines, and the oracle refuses a
 * fixture unless that recomposition reproduces SAGE26's own end state bit for bit, once with
 * SAGE26's own update_from_feedback and check_disk_instability and once with their transcriptions.
 * Each carried value is then replaced by its upward and its downward neighbour (two-sided
 * validation) and the recomposition rerun. The instability transcription is checked the same way,
 * composed with SAGE26's own grow_black_hole and collisional_starburst_recipe where the disk is
 * gas-unstable. README.md documents every output.
 *
 * Every transcription restates SAGE26's statements with the same expression shapes and
 * operand order and is compiled with the same flags, so floating-point contraction (clang's
 * default -ffp-contract=on) treats them as it treats SAGE26's; the composition checks above
 * confirm it on every fixture. File-static SAGE26 constants are restated with their values
 * and source lines.
 *
 * The file format is defined in README.md ("Fixture File Format"). The parsing and output
 * helpers are copies of oracle_c04.c's, which stays unchanged.
 *
 * This is SAGE26 reference tooling: it is compiled and linked only by build.sh, never by
 * Mimic's Makefile, and never against Mimic code.
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_allvars.h"
#include "model_disk_instability.h"
#include "model_h2_chemistry.h"
#include "model_mergers.h"
#include "model_misc.h"
#include "model_starformation_and_feedback.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 1024
#define MAX_GALAXIES 4
#define NAME_LEN 96
#define LINE_LEN 512

/* The halonr argument SAGE26 passes on: no c05 routine reads it on R0 (it reaches
 * check_disk_instability only through collisional_starburst_recipe's mode-0 path, and
 * the instability calls it in mode 1, D:106) */
#define ORACLE_HALONR 0

/* SAGE26 file-static constants, restated (values and lines of the staged source) */
static const double SF_DISK_RADIUS_FRAC = 3.0;       /* S:60 */
static const double KAUFFMANN96_SF_THRESHOLD = 0.19; /* S:66 */
static const double KD11_METAL_HALO_MASS = 30.0;     /* S:84 */
static const double SIGMA_HI_CRIT = 0.5;             /* S:97 */
static const double GAS_DISK_RADIUS_FACTOR = 1.0;    /* S:106 */
static const double TOOMRE_DISK_FACTOR = 3.0;        /* D:35 */

/* The H2 configuration the oracle supports with SFprescription 1: SAGE26's defaults
 * H2RadialNBins 25 and H2RadialRMaxFactor 5.0 (R/src/core_read_parameter_file.c:100-101) */
#define SUPPORTED_H2_NBINS 25
#define SUPPORTED_H2_RMAX_FACTOR 5.0

enum field_type { FIELD_F32, FIELD_F64, FIELD_I32 };

struct input_field {
  char name[NAME_LEN];
  enum field_type type;
  uint64_t bits;  /* IEEE-754 bit pattern (f32, f64) */
  int64_t ivalue; /* decimal value (i32) */
  int line;
  int used;
};

static struct input_field input_fields[MAX_FIELDS];
static int num_input_fields = 0;
static char routine_name[NAME_LEN] = "";
static const char *input_path = NULL;
static FILE *output_file = NULL;
static const char *output_path = NULL;

static struct GALAXY galaxies[MAX_GALAXIES];
static struct params run_params;

// ============================================================================
// ERROR HANDLING AND NAMES
// ============================================================================

static void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void fatal(const char *fmt, ...) {
  va_list args;
  fprintf(stderr, "oracle_c05: %s: ", input_path != NULL ? input_path : "(no input)");
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fputc('\n', stderr);
  if (output_file != NULL) {
    /* Never leave a partial fixture output behind */
    fclose(output_file);
    output_file = NULL;
    remove(output_path);
  }
  exit(EXIT_FAILURE);
}

/** @brief Format a field name into a static buffer (valid until the next call) */
static const char *field_name(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static const char *field_name(const char *fmt, ...) {
  static char buffer[NAME_LEN];
  va_list args;
  va_start(args, fmt);
  const int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  if (written < 0 || written >= (int)sizeof(buffer)) {
    fatal("field name too long (format '%s')", fmt);
  }
  return buffer;
}

// ============================================================================
// INPUT PARSING
// ============================================================================

/**
 * @brief Parse an exact-width hexadecimal bit pattern such as 0x3ff0000000000000
 *
 * The text must be "0x" followed by exactly hex_digits characters from [0-9a-fA-F]
 * and nothing else; the digits are decoded here rather than by strtoull, which
 * would accept a second "0x", a sign or leading whitespace and decode other bits.
 */
static uint64_t parse_bits(const char *text, const int hex_digits, const int line) {
  if (text[0] != '0' || text[1] != 'x' || (int)strlen(text + 2) != hex_digits) {
    fatal("line %d: '%s' is not 0x followed by exactly %d hex digits", line, text, hex_digits);
  }
  uint64_t value = 0;
  for (int i = 0; i < hex_digits; i++) {
    const char c = text[2 + i];
    uint64_t digit;
    if (c >= '0' && c <= '9') {
      digit = (uint64_t)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = (uint64_t)(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = (uint64_t)(c - 'A' + 10);
    } else {
      fatal("line %d: '%s' is not 0x followed by exactly %d hex digits", line, text, hex_digits);
    }
    value = (value << 4) | digit;
  }
  return value;
}

static int64_t parse_i32(const char *text, const int line) {
  char *end = NULL;
  errno = 0;
  const long long value = strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
    fatal("line %d: '%s' is not a decimal 32-bit integer", line, text);
  }
  return (int64_t)value;
}

static void add_field(const char *type, const char *name, const char *value, const int line) {
  if (num_input_fields >= MAX_FIELDS) {
    fatal("line %d: more than %d fields", line, MAX_FIELDS);
  }
  if (strlen(name) >= NAME_LEN) {
    fatal("line %d: field name too long", line);
  }
  for (int i = 0; i < num_input_fields; i++) {
    if (strcmp(input_fields[i].name, name) == 0) {
      fatal("line %d: duplicate field '%s' (first on line %d)", line, name, input_fields[i].line);
    }
  }

  struct input_field *field = &input_fields[num_input_fields];
  strcpy(field->name, name);
  field->line = line;
  field->used = 0;
  field->bits = 0;
  field->ivalue = 0;
  if (strcmp(type, "f32") == 0) {
    field->type = FIELD_F32;
    field->bits = parse_bits(value, 8, line);
  } else if (strcmp(type, "f64") == 0) {
    field->type = FIELD_F64;
    field->bits = parse_bits(value, 16, line);
  } else if (strcmp(type, "i32") == 0) {
    field->type = FIELD_I32;
    field->ivalue = parse_i32(value, line);
  } else {
    fatal("line %d: unknown field type '%s'", line, type);
  }
  num_input_fields++;
}

static void read_input(const char *path) {
  FILE *file = fopen(path, "r");
  if (file == NULL) {
    fatal("cannot open input: %s", strerror(errno));
  }

  char line[LINE_LEN];
  int line_number = 0;
  int seen_format = 0;
  while (fgets(line, sizeof(line), file) != NULL) {
    line_number++;
    if (strchr(line, '\n') == NULL && !feof(file)) {
      fatal("line %d: longer than %d characters", line_number, LINE_LEN - 2);
    }
    char *comment = strchr(line, '#');
    if (comment != NULL) {
      *comment = '\0';
    }

    char word[3][NAME_LEN];
    char extra[2];
    const int count = sscanf(line, "%95s %95s %95s %1s", word[0], word[1], word[2], extra);
    if (count <= 0) {
      continue; /* blank or comment-only line */
    }

    if (!seen_format) {
      if (count != 3 || strcmp(word[0], "format") != 0 ||
          strcmp(word[1], "sage26-oracle-fixture") != 0 || strcmp(word[2], "1") != 0) {
        fatal("line %d: first statement must be '%s'", line_number, ORACLE_FORMAT_LINE);
      }
      seen_format = 1;
    } else if (strcmp(word[0], "routine") == 0) {
      if (count != 2 || routine_name[0] != '\0') {
        fatal("line %d: expected exactly one 'routine <name>' statement", line_number);
      }
      strcpy(routine_name, word[1]);
    } else if (count == 3) {
      add_field(word[0], word[1], word[2], line_number);
    } else {
      fatal("line %d: expected '<type> <name> <value>'", line_number);
    }
  }
  if (ferror(file)) {
    fatal("read error");
  }
  fclose(file);

  if (!seen_format || routine_name[0] == '\0') {
    fatal("missing format or routine statement");
  }
}

static struct input_field *take_field(const char *name, const enum field_type type) {
  for (int i = 0; i < num_input_fields; i++) {
    if (strcmp(input_fields[i].name, name) == 0) {
      if (input_fields[i].type != type) {
        fatal("line %d: field '%s' has the wrong type", input_fields[i].line, name);
      }
      if (input_fields[i].used) {
        fatal("field '%s' read twice (oracle bug)", name);
      }
      input_fields[i].used = 1;
      return &input_fields[i];
    }
  }
  fatal("missing input field '%s'", name);
}

static float in_f32(const char *name) {
  const uint32_t bits = (uint32_t)take_field(name, FIELD_F32)->bits;
  float value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static double in_f64(const char *name) {
  const uint64_t bits = take_field(name, FIELD_F64)->bits;
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

static int in_i32(const char *name) { return (int)take_field(name, FIELD_I32)->ivalue; }

/** @brief Every input field must have been consumed by the routine's reader */
static void check_all_fields_used(void) {
  for (int i = 0; i < num_input_fields; i++) {
    if (!input_fields[i].used) {
      fatal("line %d: field '%s' is not read by routine %s", input_fields[i].line,
            input_fields[i].name, routine_name);
    }
  }
}

// ============================================================================
// OUTPUT AND BIT COMPARISON
// ============================================================================

static void out_f32(const char *name, const float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  fprintf(output_file, "f32 %s 0x%08" PRIx32 "  # %.9g\n", name, bits, (double)value);
}

static void out_f64(const char *name, const double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  fprintf(output_file, "f64 %s 0x%016" PRIx64 "  # %.17g\n", name, bits, value);
}

static void out_i32(const char *name, const int value) {
  fprintf(output_file, "i32 %s %d\n", name, value);
}

static int same_f32(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static int same_f64(const double a, const double b) { return memcmp(&a, &b, sizeof(a)) == 0; }

// ============================================================================
// GALAXY FIELDS
// ============================================================================

/** @brief A float member of SAGE26's struct GALAXY (a scalar, or the first of STEPS elements) */
struct member {
  const char *name;
  float *(*access)(struct GALAXY *g); /* the member's address (its first element) */
};

/* One typed accessor per member, so no pointer arithmetic on the struct is needed */
#define SCALAR_ACCESSOR(m)                                                                         \
  static float *access_##m(struct GALAXY *g) { return &g->m; }
#define ARRAY_ACCESSOR(m)                                                                          \
  static float *access_##m(struct GALAXY *g) { return g->m; }

SCALAR_ACCESSOR(Mvir)
SCALAR_ACCESSOR(Vvir)
SCALAR_ACCESSOR(Vmax)
SCALAR_ACCESSOR(DiskScaleRadius)
SCALAR_ACCESSOR(ColdGas)
SCALAR_ACCESSOR(MetalsColdGas)
SCALAR_ACCESSOR(StellarMass)
SCALAR_ACCESSOR(MetalsStellarMass)
SCALAR_ACCESSOR(BulgeMass)
SCALAR_ACCESSOR(MetalsBulgeMass)
SCALAR_ACCESSOR(InstabilityBulgeMass)
SCALAR_ACCESSOR(HotGas)
SCALAR_ACCESSOR(MetalsHotGas)
SCALAR_ACCESSOR(EjectedMass)
SCALAR_ACCESSOR(MetalsEjectedMass)
SCALAR_ACCESSOR(H1gas)
SCALAR_ACCESSOR(H2gas)
SCALAR_ACCESSOR(OutflowRate)
SCALAR_ACCESSOR(BlackHoleMass)
SCALAR_ACCESSOR(QuasarModeBHaccretionMass)
ARRAY_ACCESSOR(SfrDisk)
ARRAY_ACCESSOR(SfrBulge)
ARRAY_ACCESSOR(SfrDiskColdGas)
ARRAY_ACCESSOR(SfrDiskColdGasMetals)
ARRAY_ACCESSOR(SfrBulgeColdGas)
ARRAY_ACCESSOR(SfrBulgeColdGasMetals)
SCALAR_ACCESSOR(MergerBulgeMass)
SCALAR_ACCESSOR(BulgeRadius)
SCALAR_ACCESSOR(MergerBulgeRadius)
SCALAR_ACCESSOR(InstabilityBulgeRadius)
SCALAR_ACCESSOR(H2DepletionTime_Gyr)

#define MEMBER(m) {#m, access_##m}
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** The per-galaxy halo inputs the routines read and never write (float in SAGE26) */
static const struct member HALO_MEMBERS[] = {MEMBER(Mvir), MEMBER(Vvir), MEMBER(Vmax),
                                             MEMBER(DiskScaleRadius)};

/** The reservoirs and diagnostics the c05 routines write */
static const struct member STATE_MEMBERS[] = {MEMBER(ColdGas),
                                              MEMBER(MetalsColdGas),
                                              MEMBER(StellarMass),
                                              MEMBER(MetalsStellarMass),
                                              MEMBER(BulgeMass),
                                              MEMBER(MetalsBulgeMass),
                                              MEMBER(InstabilityBulgeMass),
                                              MEMBER(HotGas),
                                              MEMBER(MetalsHotGas),
                                              MEMBER(EjectedMass),
                                              MEMBER(MetalsEjectedMass),
                                              MEMBER(H1gas),
                                              MEMBER(H2gas),
                                              MEMBER(OutflowRate)};

/** The black-hole fields an instability's c06 continuation writes (grow_black_hole) */
static const struct member BH_MEMBERS[] = {MEMBER(BlackHoleMass),
                                           MEMBER(QuasarModeBHaccretionMass)};

/** The six per-substep histories, STEPS float elements each (R/src/core_allvars.h:158-163) */
static const struct member HISTORY_MEMBERS[] = {
    MEMBER(SfrDisk),         MEMBER(SfrBulge),
    MEMBER(SfrDiskColdGas),  MEMBER(SfrDiskColdGasMetals),
    MEMBER(SfrBulgeColdGas), MEMBER(SfrBulgeColdGasMetals)};

/**
 * The bulge-size members BulgeSizeOn 3 adds (c07): the merger channel and the three radii,
 * which get_bulge_radius reads and writes in mode 3 (R/src/model_misc.c:317-369) after an
 * instability transfer (update_instability_bulge_radius, R/src/model_misc.c:385-424, D:83).
 * Read, recorded and compared, after STATE_MEMBERS, for every galaxy of a routine whose flags
 * read_r0_flags read with BulgeSizeOn 3, and never in mode 0, so every mode-0 fixture reads
 * and records exactly what it did before c07.
 */
static const struct member SIZE_MEMBERS[] = {MEMBER(MergerBulgeMass), MEMBER(BulgeRadius),
                                             MEMBER(MergerBulgeRadius),
                                             MEMBER(InstabilityBulgeRadius)};

/** Set by read_r0_flags: BulgeSizeOn is 3, so SIZE_MEMBERS are read and recorded */
static int sizes_recorded = 0;

/**
 * The member SFprescription 1 adds to a starformation_and_feedback fixture (c08): the K13
 * depletion time, which sfr_br06's radial integration writes only with an SFR-output pointer
 * (R/src/model_h2_chemistry.c:279-285) and sfr_br06 passes NULL (S:222), so it must come out as
 * it went in. Read, recorded and compared after SIZE_MEMBERS for every galaxy of such a fixture
 * (h2_block_recorded), never for SFprescription 0, so every earlier fixture reads and records
 * exactly what it did before.
 */
static const struct member H2_MEMBERS[] = {MEMBER(H2DepletionTime_Gyr)};

/** Set by run_starformation_and_feedback for SFprescription 1: H2_MEMBERS are recorded */
static int h2_block_recorded = 0;

/** Which groups of members a routine reads and records */
enum block {
  BLOCK_STATE = 1,     /* Type and STATE_MEMBERS (always) */
  BLOCK_HALO = 2,      /* HALO_MEMBERS */
  BLOCK_BH = 4,        /* BH_MEMBERS */
  BLOCK_HISTORIES = 8, /* HISTORY_MEMBERS, all STEPS elements */
  BLOCK_FULL = BLOCK_STATE | BLOCK_HALO | BLOCK_BH | BLOCK_HISTORIES
};

static float *member_ptr(struct GALAXY *g, const struct member *m) { return m->access(g); }

/** @brief Read one element of a member; the accessor does not write through the galaxy */
static float member_value(const struct GALAXY *g, const struct member *m, const int element) {
  return m->access((struct GALAXY *)g)[element];
}

static const char *galaxy_field(const char *prefix, const int index, const char *member) {
  return field_name("%sgalaxy[%d].%s", prefix, index, member);
}

static const char *history_field(const char *prefix, const int index, const char *member,
                                 const int element) {
  return field_name("%sgalaxy[%d].%s[%d]", prefix, index, member, element);
}

static void read_galaxy(const int i, const int blocks) {
  struct GALAXY *g = &galaxies[i];
  g->Type = in_i32(galaxy_field("", i, "Type"));
  if (blocks & BLOCK_HALO) {
    for (size_t m = 0; m < COUNT(HALO_MEMBERS); m++) {
      *member_ptr(g, &HALO_MEMBERS[m]) = in_f32(galaxy_field("", i, HALO_MEMBERS[m].name));
    }
  }
  for (size_t m = 0; m < COUNT(STATE_MEMBERS); m++) {
    *member_ptr(g, &STATE_MEMBERS[m]) = in_f32(galaxy_field("", i, STATE_MEMBERS[m].name));
  }
  if (sizes_recorded) {
    for (size_t m = 0; m < COUNT(SIZE_MEMBERS); m++) {
      *member_ptr(g, &SIZE_MEMBERS[m]) = in_f32(galaxy_field("", i, SIZE_MEMBERS[m].name));
    }
  }
  if (h2_block_recorded) {
    for (size_t m = 0; m < COUNT(H2_MEMBERS); m++) {
      *member_ptr(g, &H2_MEMBERS[m]) = in_f32(galaxy_field("", i, H2_MEMBERS[m].name));
    }
  }
  if (blocks & BLOCK_BH) {
    for (size_t m = 0; m < COUNT(BH_MEMBERS); m++) {
      *member_ptr(g, &BH_MEMBERS[m]) = in_f32(galaxy_field("", i, BH_MEMBERS[m].name));
    }
  }
  if (blocks & BLOCK_HISTORIES) {
    for (size_t m = 0; m < COUNT(HISTORY_MEMBERS); m++) {
      for (int k = 0; k < STEPS; k++) {
        member_ptr(g, &HISTORY_MEMBERS[m])[k] =
            in_f32(history_field("", i, HISTORY_MEMBERS[m].name, k));
      }
    }
  }
}

/** @brief Write every recorded (written) field of galaxy i under <prefix>galaxy[i]. */
static void write_galaxy(const char *prefix, const int i, const struct GALAXY *g,
                         const int blocks) {
  for (size_t m = 0; m < COUNT(STATE_MEMBERS); m++) {
    out_f32(galaxy_field(prefix, i, STATE_MEMBERS[m].name), member_value(g, &STATE_MEMBERS[m], 0));
  }
  if (sizes_recorded) {
    for (size_t m = 0; m < COUNT(SIZE_MEMBERS); m++) {
      out_f32(galaxy_field(prefix, i, SIZE_MEMBERS[m].name), member_value(g, &SIZE_MEMBERS[m], 0));
    }
  }
  if (h2_block_recorded) {
    for (size_t m = 0; m < COUNT(H2_MEMBERS); m++) {
      out_f32(galaxy_field(prefix, i, H2_MEMBERS[m].name), member_value(g, &H2_MEMBERS[m], 0));
    }
  }
  if (blocks & BLOCK_BH) {
    for (size_t m = 0; m < COUNT(BH_MEMBERS); m++) {
      out_f32(galaxy_field(prefix, i, BH_MEMBERS[m].name), member_value(g, &BH_MEMBERS[m], 0));
    }
  }
  if (blocks & BLOCK_HISTORIES) {
    for (size_t m = 0; m < COUNT(HISTORY_MEMBERS); m++) {
      for (int k = 0; k < STEPS; k++) {
        out_f32(history_field(prefix, i, HISTORY_MEMBERS[m].name, k),
                member_value(g, &HISTORY_MEMBERS[m], k));
      }
    }
  }
}

/** @brief Whether every recorded field of two galaxy arrays is the same bit pattern */
static int same_galaxies(const struct GALAXY *a, const struct GALAXY *b, const int ngal,
                         const int blocks) {
  for (int i = 0; i < ngal; i++) {
    for (size_t m = 0; m < COUNT(STATE_MEMBERS); m++) {
      if (!same_f32(member_value(&a[i], &STATE_MEMBERS[m], 0),
                    member_value(&b[i], &STATE_MEMBERS[m], 0))) {
        return 0;
      }
    }
    if (sizes_recorded) {
      for (size_t m = 0; m < COUNT(SIZE_MEMBERS); m++) {
        if (!same_f32(member_value(&a[i], &SIZE_MEMBERS[m], 0),
                      member_value(&b[i], &SIZE_MEMBERS[m], 0))) {
          return 0;
        }
      }
    }
    if (h2_block_recorded) {
      for (size_t m = 0; m < COUNT(H2_MEMBERS); m++) {
        if (!same_f32(member_value(&a[i], &H2_MEMBERS[m], 0),
                      member_value(&b[i], &H2_MEMBERS[m], 0))) {
          return 0;
        }
      }
    }
    if (blocks & BLOCK_BH) {
      for (size_t m = 0; m < COUNT(BH_MEMBERS); m++) {
        if (!same_f32(member_value(&a[i], &BH_MEMBERS[m], 0),
                      member_value(&b[i], &BH_MEMBERS[m], 0))) {
          return 0;
        }
      }
    }
    if (blocks & BLOCK_HISTORIES) {
      for (size_t m = 0; m < COUNT(HISTORY_MEMBERS); m++) {
        for (int k = 0; k < STEPS; k++) {
          if (!same_f32(member_value(&a[i], &HISTORY_MEMBERS[m], k),
                        member_value(&b[i], &HISTORY_MEMBERS[m], k))) {
            return 0;
          }
        }
      }
    }
  }
  return 1;
}

// ============================================================================
// STATE INITIALISATION: FLAGS, PARAMETERS AND CALL ARGUMENTS
// ============================================================================

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(&run_params, 0xff, sizeof(run_params));
}

/** @brief Read an R0 flag the oracle requires at a fixed value */
static int32_t require_flag(const char *name, const int value) {
  const int read = in_i32(field_name("params.%s", name));
  if (read != value) {
    fatal("params.%s = %d, but the oracle requires R0's %d", name, read, value);
  }
  return read;
}

/**
 * @brief BulgeSizeOn as the fixture sets it: 0 (R0) or 3 (R1 onward, the Tonini sizes)
 *
 * Modes 1 and 2 are not ported (the campaign supports exactly 0 and 3); any value other
 * than 0 or 3 is refused, naming the parameter.
 */
static int32_t read_bulge_size_on(void) {
  const int read = in_i32("params.BulgeSizeOn");
  if (read != 0 && read != 3) {
    fatal("params.BulgeSizeOn = %d, but the oracle supports only 0 (R0) and 3 (R1 onward)", read);
  }
  return read;
}

/**
 * @brief SFprescription as the fixture sets it: 0 (R0, R1) or 1 (R2: BR06 with radial
 *        integration), and, for 1, the H2 configuration it is supported with (c08)
 *
 * SFprescription 1 requires, explicitly and at exactly these values, H2RadialIntegrationOn 1,
 * H2RadialNBins 25 and H2RadialRMaxFactor 5.0 (SAGE26's defaults,
 * R/src/core_read_parameter_file.c:99-101; R2.par sets none of them) and, when read_starburst is
 * set, StarburstColdGasOn 1 (:109); read_r0_flags pins that flag itself for every
 * configuration. Any other value is refused, naming the parameter. H2DiskAreaOption stays
 * poisoned: only the single-slab branch (S:225-236) and the StarburstColdGasOn 0 burst fuel
 * (R/src/model_mergers.c:625-629) read it, neither of them supported, so the poisoned state never
 * supplies an H2 parameter. SFprescription 0 reads nothing more, so every earlier fixture is
 * read unchanged.
 */
static void read_sf_prescription(const int read_starburst) {
  run_params.SFprescription = in_i32("params.SFprescription");
  if (run_params.SFprescription != 0 && run_params.SFprescription != 1) {
    fatal("params.SFprescription = %d, but the oracle supports only 0 (R0, R1) and 1 (R2: BR06 "
          "with radial integration)",
          run_params.SFprescription);
  }
  if (run_params.SFprescription != 1) {
    return;
  }
  run_params.H2RadialIntegrationOn = in_i32("params.H2RadialIntegrationOn");
  if (run_params.H2RadialIntegrationOn != 1) {
    fatal("params.H2RadialIntegrationOn = %d, but the oracle supports only 1 (radial "
          "integration; the single-slab branch, S:225-236, is not ported)",
          run_params.H2RadialIntegrationOn);
  }
  run_params.H2RadialNBins = in_i32("params.H2RadialNBins");
  if (run_params.H2RadialNBins != SUPPORTED_H2_NBINS) {
    fatal("params.H2RadialNBins = %d, but the oracle supports only %d", run_params.H2RadialNBins,
          SUPPORTED_H2_NBINS);
  }
  run_params.H2RadialRMaxFactor = in_f64("params.H2RadialRMaxFactor");
  if (!same_f64(run_params.H2RadialRMaxFactor, SUPPORTED_H2_RMAX_FACTOR)) {
    fatal("params.H2RadialRMaxFactor = %.17g, but the oracle supports only %.1f",
          run_params.H2RadialRMaxFactor, SUPPORTED_H2_RMAX_FACTOR);
  }
  if (read_starburst) {
    run_params.StarburstColdGasOn = in_i32("params.StarburstColdGasOn");
    if (run_params.StarburstColdGasOn != 1) {
      fatal("params.StarburstColdGasOn = %d, but the oracle supports only 1 (cold-gas bursts)",
            run_params.StarburstColdGasOn);
    }
  }
}

/**
 * @brief The R0 flags (and DiskInstabilityOn) of every routine that can reach the
 *        instability: starformation_and_feedback and check_disk_instability
 *
 * The transcriptions below implement the R0 paths only: SFprescription 0, SN recipe 1,
 * no CGM, no FIRE, no FFB, no Karpov, no full SFH, the Bondi AGN recipe, and cold-gas
 * starbursts (StarburstColdGasOn defaults to 1, R/src/core_read_parameter_file.c:109).
 * BulgeSizeOn is read, not pinned: 0 (R0) or 3 (R1 onward, read_bulge_size_on); Hubble_h,
 * which mode 3's size fallbacks read (R/src/model_misc.c:249,398), is read for every
 * configuration by read_r0_params. With BulgeSizeOn 3 every galaxy also reads and records
 * SIZE_MEMBERS (sizes_recorded). DiskInstabilityOn may be 0 (a labelled non-R0 value)
 * where allow_no_instability is set. SFprescription is 0 (R0, R1), or 1 (R2, c08, through
 * read_sf_prescription) where allow_br06 is set: starformation_and_feedback only.
 */
static void read_r0_flags(const int allow_no_instability, const int allow_br06) {
  if (allow_br06) {
    read_sf_prescription(0);
  } else {
    run_params.SFprescription = require_flag("SFprescription", 0);
  }
  run_params.SupernovaRecipeOn = require_flag("SupernovaRecipeOn", 1);
  run_params.CGMrecipeOn = require_flag("CGMrecipeOn", 0);
  run_params.FIREmodeOn = require_flag("FIREmodeOn", 0);
  run_params.EnhancedStarFormationOn = require_flag("EnhancedStarFormationOn", 0);
  run_params.BulgeSizeOn = read_bulge_size_on();
  sizes_recorded = run_params.BulgeSizeOn == 3;
  run_params.KarpovModeOn = require_flag("KarpovModeOn", 0);
  run_params.SaveFullSFH = require_flag("SaveFullSFH", 0);
  run_params.AGNrecipeOn = require_flag("AGNrecipeOn", 2);
  run_params.StarburstColdGasOn = require_flag("StarburstColdGasOn", 1);
  run_params.DiskInstabilityOn = in_i32("params.DiskInstabilityOn");
  if (!(run_params.DiskInstabilityOn == 1 ||
        (allow_no_instability && run_params.DiskInstabilityOn == 0))) {
    fatal("params.DiskInstabilityOn = %d is not allowed for routine %s",
          run_params.DiskInstabilityOn, routine_name);
  }
}

/**
 * @brief R0's parameter values and the code constants SAGE26 derives at initialisation
 *
 * The SF, feedback, recycling and yield values (R/input/millennium.par:66-89), the SN
 * code constants EtaSNcode and EnergySNcode (R/src/core_init.c:102-103), G and the
 * velocity unit (core_init.c:96, the run file), and the parameters the instability's c06
 * continuation reads (grow_black_hole, quasar_mode_wind, collisional_starburst_recipe).
 */
static void read_r0_params(void) {
  run_params.SfrEfficiency = in_f64("params.SfrEfficiency");
  run_params.FeedbackReheatingEpsilon = in_f64("params.FeedbackReheatingEpsilon");
  run_params.FeedbackEjectionEfficiency = in_f64("params.FeedbackEjectionEfficiency");
  run_params.EtaSNcode = in_f64("params.EtaSNcode");
  run_params.EnergySNcode = in_f64("params.EnergySNcode");
  run_params.RecycleFraction = in_f64("params.RecycleFraction");
  run_params.Yield = in_f64("params.Yield");
  run_params.FracZleaveDisk = in_f64("params.FracZleaveDisk");
  run_params.ThreshMajorMerger = in_f64("params.ThreshMajorMerger");
  run_params.BlackHoleGrowthRate = in_f64("params.BlackHoleGrowthRate");
  run_params.QuasarModeEfficiency = in_f64("params.QuasarModeEfficiency");
  run_params.Hubble_h = in_f64("params.Hubble_h");
  run_params.G = in_f64("params.G");
  run_params.UnitVelocity_in_cm_per_s = in_f64("params.UnitVelocity_in_cm_per_s");
}

/** @brief The FoF layout of one call: ngal galaxies, the forming galaxy p and its central */
struct layout {
  int ngal;
  int p;
  int centralgal;
};

static struct layout read_layout(void) {
  struct layout l;
  l.ngal = in_i32("call.ngal");
  l.p = in_i32("call.p");
  l.centralgal = in_i32("call.centralgal");
  if (l.ngal < 1 || l.ngal > MAX_GALAXIES || l.p < 0 || l.p >= l.ngal || l.centralgal < 0 ||
      l.centralgal >= l.ngal) {
    fatal("call.ngal = %d, call.p = %d, call.centralgal = %d are inconsistent", l.ngal, l.p,
          l.centralgal);
  }
  return l;
}

/**
 * @brief TRANSCRIPTION of the step bin, R/src/core_build_model.c:513-515
 *
 *   int step_bin = (step * STEPS) / effective_steps;
 *   if(step_bin >= STEPS) step_bin = STEPS - 1;
 */
static int transcribe_step_bin(const int step, const int effective_steps) {
  int step_bin = (step * STEPS) / effective_steps;
  if (step_bin >= STEPS) {
    step_bin = STEPS - 1;
  }
  return step_bin;
}

/** @brief TRANSCRIPTION of the substep length SAGE26 passes as dt (B:516) */
static double transcribe_dt(const double deltaT, const int effective_steps) {
  return deltaT / effective_steps;
}

static int read_effective_steps(void) {
  const int effective_steps = in_i32("call.effective_steps");
  if (effective_steps < 1 || effective_steps > MAX_STEPS) {
    fatal("call.effective_steps = %d is outside [1, %d]", effective_steps, MAX_STEPS);
  }
  return effective_steps;
}

/**
 * @brief Read call.deltaT and call.dt and check dt against the transcription of B:516
 *
 * dt is a core-owned input set explicitly in the fixture; the oracle refuses a fixture
 * whose dt is not deltaT / effective_steps bit for bit, or is not positive (SAGE26's
 * collisional_starburst_recipe asserts dt > 0, R/src/model_mergers.c:590).
 */
static double read_dt(const int effective_steps) {
  const double deltaT = in_f64("call.deltaT");
  const double dt = in_f64("call.dt");
  if (!same_f64(dt, transcribe_dt(deltaT, effective_steps))) {
    fatal("call.dt = %.17g is not call.deltaT / call.effective_steps = %.17g", dt,
          transcribe_dt(deltaT, effective_steps));
  }
  if (!(dt > 0.0)) {
    fatal("call.dt = %.17g must be positive", dt);
  }
  return dt;
}

// ============================================================================
// CARRIED INTERMEDIATES
// ============================================================================

/** Where a carried value lives: which routine must be transcribed to perturb it */
enum stage {
  STAGE_WRAPPER,     /* formed in starformation_and_feedback or its static helpers */
  STAGE_FEEDBACK,    /* a local of update_from_feedback */
  STAGE_INSTABILITY, /* a local of check_disk_instability */
  STAGE_YIELD,       /* a local of the yield block, after the instability */
  STAGE_SF_COMMIT    /* a local of update_from_star_formation (c08) */
};

enum im_id {
  IM_TDYN,
  IM_COLD_CRIT,
  IM_STRDOT,
  IM_ATOMICH,
  IM_F_ION,
  IM_STARS_PRE_RENORM,
  IM_REHEATED_PRE_RENORM,
  IM_RENORM_FAC,
  IM_STARS,
  IM_REHEATED,
  IM_EJECTED_PRE_FLOOR,
  IM_EJECTED,
  IM_METALLICITY_SF,
  IM_METALLICITY_FB,
  IM_REHEATED_SF_CLAMPED,
  IM_REHEATED_FB_CLAMPED,
  IM_EJECTED_CAPPED,
  IM_METALLICITY_HOT,
  IM_METALS_TO_EJECT,
  IM_METALS_TO_EJECT_CAPPED,
  IM_DISKMASS,
  IM_MCRIT,
  IM_UNSTABLE_GAS,
  IM_UNSTABLE_STARS,
  IM_INSTABILITY_METALLICITY,
  IM_UNSTABLE_GAS_FRACTION,
  IM_FRAC_Z_LEAVE,
  IM_YIELD_TO_HOT,
  IM_COUNT
};

/**
 * Float fields of the instability's post-transfer state, perturbed with nextafterf: the three
 * masses (every gas-unstable fixture) and, with BulgeSizeOn 3, the three radii the radius
 * update writes (PT_R0_COUNT is the number perturbed in mode 0)
 */
enum post_transfer_id {
  PT_BULGE,
  PT_METALS_BULGE,
  PT_INSTABILITY_BULGE,
  PT_R0_COUNT,
  PT_BULGE_RADIUS = PT_R0_COUNT,
  PT_MERGER_BULGE_RADIUS,
  PT_INSTABILITY_BULGE_RADIUS,
  PT_COUNT
};

static const struct {
  const char *name;
  enum stage stage;
} IM_INFO[IM_COUNT] = {
    [IM_TDYN] = {"tdyn", STAGE_WRAPPER},
    [IM_COLD_CRIT] = {"cold_crit", STAGE_WRAPPER},
    [IM_STRDOT] = {"strdot", STAGE_WRAPPER},
    [IM_ATOMICH] = {"atomicH", STAGE_WRAPPER},
    [IM_F_ION] = {"f_ion", STAGE_WRAPPER},
    [IM_STARS_PRE_RENORM] = {"stars_pre_renorm", STAGE_WRAPPER},
    [IM_REHEATED_PRE_RENORM] = {"reheated_mass_pre_renorm", STAGE_WRAPPER},
    [IM_RENORM_FAC] = {"renorm_fac", STAGE_WRAPPER},
    [IM_STARS] = {"stars", STAGE_WRAPPER},
    [IM_REHEATED] = {"reheated_mass", STAGE_WRAPPER},
    [IM_EJECTED_PRE_FLOOR] = {"ejected_mass_pre_floor", STAGE_WRAPPER},
    [IM_EJECTED] = {"ejected_mass", STAGE_WRAPPER},
    [IM_METALLICITY_SF] = {"metallicity_sf", STAGE_WRAPPER},
    [IM_METALLICITY_FB] = {"metallicity_fb", STAGE_WRAPPER},
    [IM_REHEATED_SF_CLAMPED] = {"reheated_mass_sf_clamped", STAGE_WRAPPER},
    [IM_REHEATED_FB_CLAMPED] = {"reheated_mass_fb_clamped", STAGE_FEEDBACK},
    [IM_EJECTED_CAPPED] = {"ejected_mass_capped", STAGE_FEEDBACK},
    [IM_METALLICITY_HOT] = {"metallicity_hot", STAGE_FEEDBACK},
    [IM_METALS_TO_EJECT] = {"metals_hot_to_eject", STAGE_FEEDBACK},
    [IM_METALS_TO_EJECT_CAPPED] = {"metals_hot_to_eject_capped", STAGE_FEEDBACK},
    [IM_DISKMASS] = {"diskmass", STAGE_INSTABILITY},
    [IM_MCRIT] = {"Mcrit", STAGE_INSTABILITY},
    [IM_UNSTABLE_GAS] = {"unstable_gas", STAGE_INSTABILITY},
    [IM_UNSTABLE_STARS] = {"unstable_stars", STAGE_INSTABILITY},
    [IM_INSTABILITY_METALLICITY] = {"instability_metallicity", STAGE_INSTABILITY},
    [IM_UNSTABLE_GAS_FRACTION] = {"unstable_gas_fraction", STAGE_INSTABILITY},
    [IM_FRAC_Z_LEAVE] = {"FracZleaveDiskVal", STAGE_YIELD},
    [IM_YIELD_TO_HOT] = {"yield_metals_to_hot", STAGE_YIELD},
};

/** Branch flags: -1 not evaluated on this path, otherwise 0 or 1 (f_ion_path 0-4) */
enum flag_id {
  FL_SF_VVIR_NONPOSITIVE,
  FL_SF_ABOVE_CRITICAL,
  FL_STARS_FLOORED,
  FL_ATOMICH_FLOORED,
  FL_F_ION_PATH,
  FL_RENORM_ACTIVE,
  FL_EJECTION_VVIR_NONPOSITIVE,
  FL_EJECTED_FLOORED,
  FL_REHEAT_CLAMP_SF_BINDS,
  FL_REHEAT_CLAMP_FB_BINDS,
  FL_EJECTED_CAP_BINDS,
  FL_METALS_CAP_BINDS,
  FL_DISK_POSITIVE,
  FL_MCRIT_CAPPED,
  FL_STARS_TRANSFERRED,
  FL_GAS_BRANCH,
  FL_YIELD_ABOVE_THRESHOLD,
  FL_COUNT
};

static const char *const FLAG_NAMES[FL_COUNT] = {
    [FL_SF_VVIR_NONPOSITIVE] = "sf_vvir_nonpositive",
    [FL_SF_ABOVE_CRITICAL] = "sf_above_critical",
    [FL_STARS_FLOORED] = "stars_floored",
    [FL_ATOMICH_FLOORED] = "atomicH_floored",
    [FL_F_ION_PATH] = "f_ion_path",
    [FL_RENORM_ACTIVE] = "renorm_active",
    [FL_EJECTION_VVIR_NONPOSITIVE] = "ejection_vvir_nonpositive",
    [FL_EJECTED_FLOORED] = "ejected_floored",
    [FL_REHEAT_CLAMP_SF_BINDS] = "reheat_clamp_sf_binds",
    [FL_REHEAT_CLAMP_FB_BINDS] = "reheat_clamp_fb_binds",
    [FL_EJECTED_CAP_BINDS] = "ejected_cap_binds",
    [FL_METALS_CAP_BINDS] = "metals_cap_binds",
    [FL_DISK_POSITIVE] = "instability_disk_positive",
    [FL_MCRIT_CAPPED] = "instability_mcrit_capped",
    [FL_STARS_TRANSFERRED] = "instability_stars_transferred",
    [FL_GAS_BRANCH] = "instability_gas_branch",
    [FL_YIELD_ABOVE_THRESHOLD] = "yield_coldgas_above_threshold",
};

/** Values of intermediate.f_ion_path (ionized_gas_fraction, S:135-152) */
enum f_ion_path {
  F_ION_INVALID_INPUT = 0, /* a non-positive input: 0.0 (S:138-140) */
  F_ION_BELOW_CRIT = 1,    /* sigma0 <= sigma_hi_crit: 1.0 (S:144-146) */
  F_ION_GENERAL = 2,       /* the general expression, inside [0, 1] (S:147-148) */
  F_ION_CLAMPED_LOW = 3,   /* the general expression clamped up to 0 (S:149) */
  F_ION_CLAMPED_HIGH = 4   /* the general expression clamped down to 1 (S:150) */
};

/**
 * The values SFprescription 1 adds (c08), recorded only for SFprescription 1 fixtures: sfr_br06's
 * (S:193-252), the H I the SF step hands on, and the two commits' hydrogen caps (S:1131-1135,
 * S:1169-1173). Each is tail-checked (its routine passes it to no SAGE26 function) and recorded
 * with c06's markers and the narrowing limitation.
 */
enum h2_id {
  IMH_RS_PC,     /* sfr_br06's float rs_pc (S:215) */
  IMH_H2_RADIAL, /* H2gas after SAGE26's own radial integration, before the cap (S:222) */
  IMH_H2_BUDGET, /* the double ColdGas * HYDROGEN_MASS_FRAC of the cap (S:239-240) */
  IMH_H2_SF,     /* H2gas after the cap (S:239-242), the H2 the SF step hands on */
  IMH_H1_SF,     /* H1gas after the H I block (S:1045), the H I the SF step hands on */
  IMH_MAX_H_SF,  /* update_from_star_formation's float max_h (S:1132) */
  IMH_MAX_H_FB,  /* update_from_feedback's float max_h_fb (S:1170) */
  IMH_COUNT
};

static const struct {
  const char *name;
  enum stage stage;
  int is_f32;            /* the local is a float (a float field after its store) */
  int narrowing_limited; /* 1: its only path to the state is one double-to-float narrowing */
} IMH_INFO[IMH_COUNT] = {
    [IMH_RS_PC] = {"rs_pc", STAGE_WRAPPER, 1, 0},
    [IMH_H2_RADIAL] = {"H2gas_radial", STAGE_WRAPPER, 1, 0},
    /* compared with H2gas and, when the cap binds, narrowed into it (S:239-240) */
    [IMH_H2_BUDGET] = {"h2_budget", STAGE_WRAPPER, 0, 1},
    [IMH_H2_SF] = {"H2gas_sf", STAGE_WRAPPER, 1, 0},
    [IMH_H1_SF] = {"H1gas_sf", STAGE_WRAPPER, 1, 0},
    [IMH_MAX_H_SF] = {"max_h_sf", STAGE_SF_COMMIT, 1, 0},
    [IMH_MAX_H_FB] = {"max_h_fb", STAGE_FEEDBACK, 1, 0},
};

/** The branch flags SFprescription 1 adds (c08): -1 not evaluated on this path, else 0 or 1 */
enum h2_flag_id {
  FLH_RS_NONPOSITIVE,        /* rs_pc <= 0.0 (S:216) */
  FLH_RADIAL_RETURNED_EARLY, /* the radial integration's rs_pc <= 0 || ColdGas <= 0 (H:141) */
  FLH_H2_CAP_BINDS,          /* H2gas > ColdGas * HYDROGEN_MASS_FRAC (S:239) */
  FLH_SF_FROM_H2,            /* H2gas > 0.0 && tdyn > 0.0 (S:244) */
  FLH_CAPS_SF_TRACKED,       /* sf_prescription_tracks_h2 at S:1131 */
  FLH_H2_CAP_SF_BINDS,       /* H2gas > max_h (S:1133) */
  FLH_H1_CAP_SF_BINDS,       /* H1gas > max_h (S:1134) */
  FLH_CAPS_FB_TRACKED,       /* sf_prescription_tracks_h2 at S:1169 */
  FLH_H2_CAP_FB_BINDS,       /* H2gas > max_h_fb (S:1171) */
  FLH_H1_CAP_FB_BINDS,       /* H1gas > max_h_fb (S:1172) */
  FLH_COUNT
};

static const char *const H2_FLAG_NAMES[FLH_COUNT] = {
    [FLH_RS_NONPOSITIVE] = "br06_rs_nonpositive",
    [FLH_RADIAL_RETURNED_EARLY] = "br06_radial_returned_early",
    [FLH_H2_CAP_BINDS] = "br06_h2_cap_binds",
    [FLH_SF_FROM_H2] = "br06_sf_from_h2",
    [FLH_CAPS_SF_TRACKED] = "hydrogen_caps_sf_tracked",
    [FLH_H2_CAP_SF_BINDS] = "h2_cap_sf_binds",
    [FLH_H1_CAP_SF_BINDS] = "h1_cap_sf_binds",
    [FLH_CAPS_FB_TRACKED] = "hydrogen_caps_fb_tracked",
    [FLH_H2_CAP_FB_BINDS] = "h2_cap_fb_binds",
    [FLH_H1_CAP_FB_BINDS] = "h1_cap_fb_binds",
};

/** The carried values and branch flags of one composition */
struct locals {
  double value[IM_COUNT];
  int present[IM_COUNT];
  int flag[FL_COUNT];
  double h2_value[IMH_COUNT]; /* a float local widened exactly */
  int h2_present[IMH_COUNT];
  int h2_flag[FLH_COUNT];
  int out_of_domain; /* a SAGE26 assertion would abort on this path (perturbed runs only) */
};

static void init_locals(struct locals *l) {
  memset(l, 0, sizeof(*l));
  for (int f = 0; f < FL_COUNT; f++) {
    l->flag[f] = -1;
  }
  for (int f = 0; f < FLH_COUNT; f++) {
    l->h2_flag[f] = -1;
  }
}

/** A replacement of one carried value by its upward or downward neighbour */
struct perturbation {
  int im;   /* enum im_id to perturb, or -1 */
  int post; /* enum post_transfer_id to perturb, or -1 */
  int up;   /* 1: toward +inf; 0: toward -inf */
  int h2;   /* enum h2_id to perturb (c08), or -1 */
};

static const struct perturbation NO_PERTURBATION = {-1, -1, 0, -1};

/** @brief Record a carried value, then return it, or its neighbour when it is perturbed */
static double tap(struct locals *l, const struct perturbation *pt, const enum im_id id,
                  const double v) {
  l->value[id] = v;
  l->present[id] = 1;
  if (pt->im == (int)id) {
    return nextafter(v, pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

/** @brief Record a float c08 value, then return it, or its float neighbour when perturbed */
static float tap_h2_f32(struct locals *l, const struct perturbation *pt, const enum h2_id id,
                        const float v) {
  l->h2_value[id] = v;
  l->h2_present[id] = 1;
  if (pt->h2 == (int)id) {
    return nextafterf(v, pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

/** @brief The double counterpart of tap_h2_f32() */
static double tap_h2_f64(struct locals *l, const struct perturbation *pt, const enum h2_id id,
                         const double v) {
  l->h2_value[id] = v;
  l->h2_present[id] = 1;
  if (pt->h2 == (int)id) {
    return nextafter(v, pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

/** @brief The float counterpart of tap() for the instability's post-transfer fields */
static float tap_post(const struct perturbation *pt, const enum post_transfer_id id,
                      const float v) {
  if (pt->post == (int)id) {
    return nextafterf(v, pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

// ============================================================================
// TRANSCRIPTIONS OF SAGE26 SOURCE (model_starformation_and_feedback.c)
// ============================================================================

/**
 * @brief TRANSCRIPTION of atomic_disk_radius, S:130-133 (static in SAGE26)
 */
static double transcribe_atomic_disk_radius(const double rs_code) {
  return GAS_DISK_RADIUS_FACTOR * rs_code;
}

/**
 * @brief TRANSCRIPTION of ionized_gas_fraction, S:135-152 (static in SAGE26)
 *
 * Statement for statement, with the path taken recorded in intermediate.f_ion_path.
 * CODE_MASS_TO_MSUN and CODE_LENGTH_TO_PC are SAGE26's own macros (R/src/macros.h:45-46).
 */
static double transcribe_ionized_gas_fraction(const double coldgas_code, const double rs_code,
                                              const double h, const double sigma_hi_crit,
                                              struct locals *l) {
  if (coldgas_code <= 0.0 || rs_code <= 0.0 || sigma_hi_crit <= 0.0 || h <= 0.0) {
    l->flag[FL_F_ION_PATH] = F_ION_INVALID_INPUT;
    return 0.0;
  }
  const double mgas = CODE_MASS_TO_MSUN(coldgas_code, h);    /* Msun */
  const double rs_pc = CODE_LENGTH_TO_PC(rs_code, h);        /* pc */
  const double sigma0 = mgas / (2.0 * M_PI * rs_pc * rs_pc); /* Msun/pc^2 */
  if (sigma0 <= sigma_hi_crit) {
    l->flag[FL_F_ION_PATH] = F_ION_BELOW_CRIT;
    return 1.0;
  }
  const double x = log(sigma0 / sigma_hi_crit);
  double f_ion = (1.0 + x) * sigma_hi_crit / sigma0;
  l->flag[FL_F_ION_PATH] = F_ION_GENERAL;
  if (f_ion < 0.0) {
    f_ion = 0.0;
    l->flag[FL_F_ION_PATH] = F_ION_CLAMPED_LOW;
  }
  if (f_ion > 1.0) {
    f_ion = 1.0;
    l->flag[FL_F_ION_PATH] = F_ION_CLAMPED_HIGH;
  }
  return f_ion;
}

/**
 * @brief TRANSCRIPTION of sfr_br06, S:193-252 (static in SAGE26), with radial integration
 *
 * SFprescription 1 with H2RadialIntegrationOn 1 (read_sf_prescription requires it), so the
 * single-slab branch (S:225-236) does not run. The radial integration it calls (S:222) is
 * SAGE26's own public calculate_molecular_fraction_radial_integration with its NULL SFR-output
 * pointer; it writes H2gas except on its early return (R/src/model_h2_chemistry.c:141-143,
 * recorded in br06_radial_returned_early). The cap's product ColdGas * HYDROGEN_MASS_FRAC
 * (S:239-240) is formed once and used for the comparison and the store, as the two identical
 * expressions of the source evaluate to the same double. The clamp counter (S:241) is a
 * diagnostic and is not transcribed.
 */
static double transcribe_sfr_br06(const int p, struct locals *l, const struct perturbation *pt) {
  /* S:195-198 */
  double reff, tdyn, strdot;
  strdot = 0.0;
  tdyn = 0.0;

  /* S:206 */
  reff = SF_DISK_RADIUS_FRAC * galaxies[p].DiskScaleRadius;

  /* S:208-210 */
  l->flag[FL_SF_VVIR_NONPOSITIVE] = 0;
  if (galaxies[p].Vvir <= 0.0) {
    l->flag[FL_SF_VVIR_NONPOSITIVE] = 1;
    galaxies[p].H2gas = 0.0;
    strdot = 0.0;
  } else {
    /* S:212 */
    tdyn = reff / galaxies[p].Vvir;
    tdyn = tap(l, pt, IM_TDYN, tdyn);
    /* S:214-215 */
    const float h = run_params.Hubble_h;
    float rs_pc = CODE_LENGTH_TO_PC(galaxies[p].DiskScaleRadius, h);
    rs_pc = tap_h2_f32(l, pt, IMH_RS_PC, rs_pc);
    /* S:216-218 */
    l->h2_flag[FLH_RS_NONPOSITIVE] = 0;
    if (rs_pc <= 0.0) {
      l->h2_flag[FLH_RS_NONPOSITIVE] = 1;
      galaxies[p].H2gas = 0.0;
      strdot = 0.0;
    } else {
      /* S:221-223, with R/src/model_h2_chemistry.c:138-143 restated for the flag */
      const float h_radial = run_params.Hubble_h;
      const float rs_pc_radial = CODE_LENGTH_TO_PC(galaxies[p].DiskScaleRadius, h_radial);
      l->h2_flag[FLH_RADIAL_RETURNED_EARLY] = rs_pc_radial <= 0.0 || galaxies[p].ColdGas <= 0.0;
      calculate_molecular_fraction_radial_integration(p, galaxies, &run_params, NULL);
      galaxies[p].H2gas = tap_h2_f32(l, pt, IMH_H2_RADIAL, galaxies[p].H2gas);

      /* S:239-242 */
      double h2_budget = galaxies[p].ColdGas * HYDROGEN_MASS_FRAC;
      h2_budget = tap_h2_f64(l, pt, IMH_H2_BUDGET, h2_budget);
      l->h2_flag[FLH_H2_CAP_BINDS] = 0;
      if (galaxies[p].H2gas > h2_budget) {
        galaxies[p].H2gas = h2_budget;
        l->h2_flag[FLH_H2_CAP_BINDS] = 1;
      }
      galaxies[p].H2gas = tap_h2_f32(l, pt, IMH_H2_SF, galaxies[p].H2gas);

      /* S:244-248 */
      l->h2_flag[FLH_SF_FROM_H2] = 0;
      if (galaxies[p].H2gas > 0.0 && tdyn > 0.0) {
        l->h2_flag[FLH_SF_FROM_H2] = 1;
        strdot = run_params.SfrEfficiency * galaxies[p].H2gas / tdyn;
      } else {
        strdot = 0.0;
      }
    }
  }

  /* S:252 */
  return strdot;
}

/**
 * @brief TRANSCRIPTION of update_from_star_formation, S:1119-1136 (c08)
 *
 * The four reservoir stores (S:1123-1126), each one C expression with SAGE26's operand order,
 * which clang fuses as SAGE26's object does (README "The c08 Group"), then, for a prescription
 * that tracks H2, the hydrogen cap (S:1131-1135): the float max_h of the remaining cold gas
 * and the independent strict caps of H2gas and H1gas.
 */
static void transcribe_update_from_star_formation(const int p, const double stars,
                                                  const double metallicity, struct locals *l,
                                                  const struct perturbation *pt) {
  /* S:1121 */
  const double RecycleFraction = run_params.RecycleFraction;
  /* S:1123-1126 */
  galaxies[p].ColdGas -= (1 - RecycleFraction) * stars;
  galaxies[p].MetalsColdGas -= metallicity * (1 - RecycleFraction) * stars;
  galaxies[p].StellarMass += (1 - RecycleFraction) * stars;
  galaxies[p].MetalsStellarMass += metallicity * (1 - RecycleFraction) * stars;

  /* S:1131-1135 */
  l->h2_flag[FLH_CAPS_SF_TRACKED] = sf_prescription_tracks_h2(run_params.SFprescription);
  if (sf_prescription_tracks_h2(run_params.SFprescription)) {
    float max_h = (galaxies[p].ColdGas > 0.0f) ? galaxies[p].ColdGas * HYDROGEN_MASS_FRAC : 0.0f;
    max_h = tap_h2_f32(l, pt, IMH_MAX_H_SF, max_h);
    l->h2_flag[FLH_H2_CAP_SF_BINDS] = galaxies[p].H2gas > max_h;
    if (galaxies[p].H2gas > max_h)
      galaxies[p].H2gas = max_h;
    l->h2_flag[FLH_H1_CAP_SF_BINDS] = galaxies[p].H1gas > max_h;
    if (galaxies[p].H1gas > max_h)
      galaxies[p].H1gas = max_h;
  }
}

/**
 * @brief TRANSCRIPTION of update_from_feedback, S:1146-1250, on R0's path and R2's
 *
 * SupernovaRecipeOn 1, CGMrecipeOn 0 ("Original SAGE behavior", S:1224-1246) and
 * SFprescription 0 or 1: for 1 (sf_prescription_tracks_h2), the hydrogen cap of S:1169-1173
 * after the cold-gas and metal debits and before the hot-gas credit (c08). Its XASSERTs
 * (S:1158-1163) are checked: a violation marks the composition out of domain, as SAGE26 would
 * abort there.
 */
static void transcribe_update_from_feedback(const int p, const int centralgal, double reheated_mass,
                                            double ejected_mass, const double metallicity,
                                            struct locals *l, const struct perturbation *pt) {
  l->flag[FL_REHEAT_CLAMP_FB_BINDS] = 0;
  if (reheated_mass > galaxies[p].ColdGas) {
    reheated_mass = galaxies[p].ColdGas;
    l->flag[FL_REHEAT_CLAMP_FB_BINDS] = 1;
  }
  reheated_mass = tap(l, pt, IM_REHEATED_FB_CLAMPED, reheated_mass);
  if (!(reheated_mass >= 0.0) || !(reheated_mass <= galaxies[p].ColdGas)) {
    l->out_of_domain = 1;
    return;
  }

  /* S:1153-1156 (above), S:1167-1168 */
  galaxies[p].ColdGas -= reheated_mass;
  galaxies[p].MetalsColdGas -= metallicity * reheated_mass;
  /* S:1169-1173 */
  l->h2_flag[FLH_CAPS_FB_TRACKED] = sf_prescription_tracks_h2(run_params.SFprescription);
  if (sf_prescription_tracks_h2(run_params.SFprescription)) {
    float max_h_fb = (galaxies[p].ColdGas > 0.0f) ? galaxies[p].ColdGas * HYDROGEN_MASS_FRAC : 0.0f;
    max_h_fb = tap_h2_f32(l, pt, IMH_MAX_H_FB, max_h_fb);
    l->h2_flag[FLH_H2_CAP_FB_BINDS] = galaxies[p].H2gas > max_h_fb;
    if (galaxies[p].H2gas > max_h_fb)
      galaxies[p].H2gas = max_h_fb;
    l->h2_flag[FLH_H1_CAP_FB_BINDS] = galaxies[p].H1gas > max_h_fb;
    if (galaxies[p].H1gas > max_h_fb)
      galaxies[p].H1gas = max_h_fb;
  }

  /* S:1228-1229 */
  galaxies[centralgal].HotGas += reheated_mass;
  galaxies[centralgal].MetalsHotGas += metallicity * reheated_mass;

  /* S:1232-1234 */
  l->flag[FL_EJECTED_CAP_BINDS] = 0;
  if (ejected_mass > galaxies[centralgal].HotGas) {
    ejected_mass = galaxies[centralgal].HotGas;
    l->flag[FL_EJECTED_CAP_BINDS] = 1;
  }
  ejected_mass = tap(l, pt, IM_EJECTED_CAPPED, ejected_mass);

  /* S:1235, S:1237-1240 */
  double metallicityHot =
      get_metallicity(galaxies[centralgal].HotGas, galaxies[centralgal].MetalsHotGas);
  metallicityHot = tap(l, pt, IM_METALLICITY_HOT, metallicityHot);

  double metalsHot_to_eject = metallicityHot * ejected_mass;
  metalsHot_to_eject = tap(l, pt, IM_METALS_TO_EJECT, metalsHot_to_eject);
  l->flag[FL_METALS_CAP_BINDS] = 0;
  if (metalsHot_to_eject > galaxies[centralgal].MetalsHotGas) {
    metalsHot_to_eject = galaxies[centralgal].MetalsHotGas;
    l->flag[FL_METALS_CAP_BINDS] = 1;
  }
  metalsHot_to_eject = tap(l, pt, IM_METALS_TO_EJECT_CAPPED, metalsHot_to_eject);

  /* S:1243-1246 */
  galaxies[centralgal].HotGas -= ejected_mass;
  galaxies[centralgal].MetalsHotGas -= metalsHot_to_eject;
  galaxies[centralgal].EjectedMass += ejected_mass;
  galaxies[centralgal].MetalsEjectedMass += metalsHot_to_eject;

  /* S:1249 */
  galaxies[p].OutflowRate += reheated_mass;
}

/**
 * @brief SAGE26's own update_from_feedback, after checking its assertions (S:1158-1163)
 *
 * A perturbed run can hand it a negative reheated mass, on which SAGE26's XASSERT would
 * abort this process. That composition is marked out of domain instead.
 */
static int feedback_in_domain(const int p, const double reheated_mass) {
  double clamped = reheated_mass;
  if (clamped > galaxies[p].ColdGas) {
    clamped = galaxies[p].ColdGas;
  }
  return clamped >= 0.0 && clamped <= galaxies[p].ColdGas;
}

static void call_update_from_feedback(const int p, const int centralgal, const double reheated_mass,
                                      const double ejected_mass, const double metallicity,
                                      struct locals *l) {
  if (!feedback_in_domain(p, reheated_mass)) {
    l->out_of_domain = 1;
    return;
  }
  update_from_feedback(p, centralgal, reheated_mass, ejected_mass, metallicity, galaxies,
                       &run_params);
}

// ============================================================================
// TRANSCRIPTION OF SAGE26 SOURCE (model_disk_instability.c)
// ============================================================================

/** @brief What a transcribed instability did, and its state before the c06 continuation */
struct instability_run {
  int reached_gas_branch;
  struct GALAXY post_transfer[MAX_GALAXIES]; /* the state after D:54-83 (and D:101) */
};

/**
 * @brief TRANSCRIPTION of check_disk_instability's stellar transfer and gas fraction,
 *        D:54-101, followed by SAGE26's own continuation D:102-108
 *
 * Statement for statement: the float disk-mass sums (D:54), Mcrit from the float Vmax
 * product (D:57-60), the double fractions (D:63-66), the stellar transfer into the float
 * BulgeMass, InstabilityBulgeMass and MetalsBulgeMass with SAGE26's own get_metallicity of
 * the float disk differences (D:73-80), and SAGE26's own update_instability_bulge_radius,
 * which returns at once on R0 (BulgeSizeOn 0, R/src/model_misc.c:395) and with BulgeSizeOn 3
 * updates the instability radius and synchronises the three radii (R/src/model_misc.c:385-424). The
 * VERBOSE diagnostic of D:91-99 prints and sets run_params->interrupted only; it is not
 * transcribed. If the gas branch (D:90) is reached, the unstable gas fraction is formed
 * (D:101) and, when continue_to_c06 is set, SAGE26's own grow_black_hole and
 * collisional_starburst_recipe run with exactly the arguments of D:102-108.
 */
static void transcribe_check_disk_instability(const int p, const int centralgal, const double time,
                                              const double dt, const int step, const int ngal,
                                              const int continue_to_c06, struct locals *l,
                                              const struct perturbation *pt,
                                              struct instability_run *run) {
  run->reached_gas_branch = 0;
  l->flag[FL_DISK_POSITIVE] = 0;

  /* D:54 */
  double diskmass = galaxies[p].ColdGas + (galaxies[p].StellarMass - galaxies[p].BulgeMass);
  diskmass = tap(l, pt, IM_DISKMASS, diskmass);
  if (diskmass > 0.0) {
    l->flag[FL_DISK_POSITIVE] = 1;

    /* D:57-60 */
    double Mcrit = galaxies[p].Vmax * galaxies[p].Vmax *
                   (TOOMRE_DISK_FACTOR * galaxies[p].DiskScaleRadius) / run_params.G;
    l->flag[FL_MCRIT_CAPPED] = 0;
    if (Mcrit > diskmass) {
      Mcrit = diskmass;
      l->flag[FL_MCRIT_CAPPED] = 1;
    }
    Mcrit = tap(l, pt, IM_MCRIT, Mcrit);

    /* D:63-66 */
    const double gas_fraction = galaxies[p].ColdGas / diskmass;
    double unstable_gas = gas_fraction * (diskmass - Mcrit);
    unstable_gas = tap(l, pt, IM_UNSTABLE_GAS, unstable_gas);
    const double star_fraction = 1.0 - gas_fraction;
    double unstable_stars = star_fraction * (diskmass - Mcrit);
    unstable_stars = tap(l, pt, IM_UNSTABLE_STARS, unstable_stars);

    /* D:70 */
    const double old_disk_radius = galaxies[p].DiskScaleRadius;

    /* D:73-84 */
    l->flag[FL_STARS_TRANSFERRED] = 0;
    if (unstable_stars > 0.0) {
      l->flag[FL_STARS_TRANSFERRED] = 1;
      double metallicity =
          get_metallicity(galaxies[p].StellarMass - galaxies[p].BulgeMass,
                          galaxies[p].MetalsStellarMass - galaxies[p].MetalsBulgeMass);
      metallicity = tap(l, pt, IM_INSTABILITY_METALLICITY, metallicity);

      galaxies[p].BulgeMass += unstable_stars;
      galaxies[p].InstabilityBulgeMass += unstable_stars;
      galaxies[p].MetalsBulgeMass += metallicity * unstable_stars;

      update_instability_bulge_radius(p, unstable_stars, old_disk_radius, galaxies, &run_params);
    }

    /* The post-transfer state, perturbable for the two-sided validation */
    galaxies[p].BulgeMass = tap_post(pt, PT_BULGE, galaxies[p].BulgeMass);
    galaxies[p].MetalsBulgeMass = tap_post(pt, PT_METALS_BULGE, galaxies[p].MetalsBulgeMass);
    galaxies[p].InstabilityBulgeMass =
        tap_post(pt, PT_INSTABILITY_BULGE, galaxies[p].InstabilityBulgeMass);
    /* BulgeSizeOn 3 only: no perturbation names these ids in mode 0 */
    galaxies[p].BulgeRadius = tap_post(pt, PT_BULGE_RADIUS, galaxies[p].BulgeRadius);
    galaxies[p].MergerBulgeRadius =
        tap_post(pt, PT_MERGER_BULGE_RADIUS, galaxies[p].MergerBulgeRadius);
    galaxies[p].InstabilityBulgeRadius =
        tap_post(pt, PT_INSTABILITY_BULGE_RADIUS, galaxies[p].InstabilityBulgeRadius);

    /* D:90 */
    l->flag[FL_GAS_BRANCH] = 0;
    if (unstable_gas > 0.0 && galaxies[p].ColdGas > 0.0) {
      l->flag[FL_GAS_BRANCH] = 1;
      run->reached_gas_branch = 1;

      /* D:101 */
      double unstable_gas_fraction = unstable_gas / galaxies[p].ColdGas;
      unstable_gas_fraction = tap(l, pt, IM_UNSTABLE_GAS_FRACTION, unstable_gas_fraction);
      memcpy(run->post_transfer, galaxies, (size_t)ngal * sizeof(galaxies[0]));
      if (!continue_to_c06) {
        return;
      }

      /* D:102-108: SAGE26's own continuation (c06's routines), not a transcription */
      if (run_params.AGNrecipeOn > 0) {
        grow_black_hole(p, unstable_gas_fraction, galaxies, &run_params);
      }
      collisional_starburst_recipe(unstable_gas_fraction, p, centralgal, time, dt, ORACLE_HALONR, 1,
                                   step, 0, galaxies[p].DiskScaleRadius, galaxies, &run_params);
      return;
    }
  }
  memcpy(run->post_transfer, galaxies, (size_t)ngal * sizeof(galaxies[0]));
}

// ============================================================================
// THE WRAPPER COMPOSITION (starformation_and_feedback, S:995-1110)
// ============================================================================

/** The arguments of one starformation_and_feedback call (B:516) */
struct wrapper_args {
  struct layout layout;
  double time;
  double dt;
  int step_bin;
};

/** Which of SAGE26's public routines a composition replaces by its transcription */
struct composition {
  int transcribe_feedback;
  int transcribe_instability;
  int transcribe_sf_commit; /* update_from_star_formation (c08) */
};

/**
 * @brief Recompose starformation_and_feedback in SAGE26's statement order
 *
 * On a copy of the input, from the transcribed blocks of S (each labelled with its lines)
 * and SAGE26's own get_metallicity, update_from_star_formation, update_from_feedback and
 * check_disk_instability (or, per comp, their transcriptions above):
 *   1. sfr_croton06 (S:160-184), the SFprescription 0 case of S:1016, or sfr_br06 (S:193-252),
 *      the SFprescription 1 case of S:1017, with SAGE26's own radial integration (c08);
 *   2. the H I block (S:1035-1046) with ionized_gas_fraction (S:135-152);
 *   3. stars = strdot * dt and its floor (S:1048-1051);
 *   4. compute_sn_feedback's non-FIRE, non-Karpov path (S:732-799, S:876-889);
 *   5. the disk history sampling (S:1058-1060) at the step bin;
 *   6. update_from_star_formation with SAGE26's get_metallicity (S:1063-1064);
 *   7. the metallicity and the reheating clamp (S:1076-1082), then update_from_feedback
 *      (S:1085);
 *   8. check_disk_instability when DiskInstabilityOn (S:1088-1090);
 *   9. the yield (S:1093-1108).
 * SaveFullSFH is 0 on R0, so S:1068-1073 does not run; the FFB branch (S:1004-1010) is off
 * (EnhancedStarFormationOn 0). Returns the end state in result.
 */
static void compose_wrapper(const struct GALAXY *input, const struct wrapper_args *a,
                            const struct composition *comp, const struct perturbation *pt,
                            struct locals *l, struct GALAXY *result,
                            struct instability_run *instability) {
  const int p = a->layout.p;
  const int centralgal = a->layout.centralgal;
  const int ngal = a->layout.ngal;
  memcpy(galaxies, input, (size_t)ngal * sizeof(galaxies[0]));
  init_locals(l);
  instability->reached_gas_branch = 0;

  /* 1. S:1015-1017: TRANSCRIPTION of sfr_br06 (SFprescription 1) or of sfr_croton06,
   * S:162-183 (SFprescription 0) */
  double strdot = 0.0;
  if (run_params.SFprescription == 1) {
    strdot = transcribe_sfr_br06(p, l, pt);
  } else {
    double reff, tdyn;
    tdyn = 0.0;
    reff = SF_DISK_RADIUS_FRAC * galaxies[p].DiskScaleRadius;
    l->flag[FL_SF_VVIR_NONPOSITIVE] = 0;
    if (galaxies[p].Vvir <= 0.0) {
      l->flag[FL_SF_VVIR_NONPOSITIVE] = 1;
      strdot = 0.0;
    } else {
      tdyn = reff / galaxies[p].Vvir;
      tdyn = tap(l, pt, IM_TDYN, tdyn);
      double cold_crit = KAUFFMANN96_SF_THRESHOLD * galaxies[p].Vvir * reff;
      cold_crit = tap(l, pt, IM_COLD_CRIT, cold_crit);
      l->flag[FL_SF_ABOVE_CRITICAL] = 0;
      if (galaxies[p].ColdGas > cold_crit && tdyn > 0.0) {
        l->flag[FL_SF_ABOVE_CRITICAL] = 1;
        strdot = run_params.SfrEfficiency * (galaxies[p].ColdGas - cold_crit) / tdyn;
      } else {
        strdot = 0.0;
      }
    }
  }
  strdot = tap(l, pt, IM_STRDOT, strdot);

  /* 2. TRANSCRIPTION of the H I block, S:1035-1046 */
  {
    double atomicH = galaxies[p].ColdGas * HYDROGEN_MASS_FRAC - galaxies[p].H2gas;
    l->flag[FL_ATOMICH_FLOORED] = 0;
    if (atomicH < 0.0) {
      atomicH = 0.0;
      l->flag[FL_ATOMICH_FLOORED] = 1;
    }
    atomicH = tap(l, pt, IM_ATOMICH, atomicH);
    double f_ion = transcribe_ionized_gas_fraction(
        galaxies[p].ColdGas, transcribe_atomic_disk_radius(galaxies[p].DiskScaleRadius),
        run_params.Hubble_h, SIGMA_HI_CRIT, l);
    f_ion = tap(l, pt, IM_F_ION, f_ion);
    atomicH *= (1.0 - f_ion);
    galaxies[p].H1gas = atomicH;
    if (run_params.SFprescription == 1) {
      galaxies[p].H1gas = tap_h2_f32(l, pt, IMH_H1_SF, galaxies[p].H1gas);
    }
  }

  /* 3. TRANSCRIPTION of S:1048-1051 */
  double stars = strdot * a->dt;
  l->flag[FL_STARS_FLOORED] = 0;
  if (stars < 0.0) {
    stars = 0.0;
    l->flag[FL_STARS_FLOORED] = 1;
  }
  stars = tap(l, pt, IM_STARS_PRE_RENORM, stars);

  /* 4. TRANSCRIPTION of compute_sn_feedback, S:735-736, S:757-799, S:876-889 */
  double ejected_mass;
  double reheated_mass = 0.0;
  reheated_mass = run_params.FeedbackReheatingEpsilon * stars;
  reheated_mass = tap(l, pt, IM_REHEATED_PRE_RENORM, reheated_mass);
  /* S:771-772: XASSERT(reheated_mass >= 0.0) */
  if (!(reheated_mass >= 0.0)) {
    l->out_of_domain = 1;
    return;
  }
  l->flag[FL_RENORM_ACTIVE] = 0;
  if ((stars + reheated_mass) > galaxies[p].ColdGas && (stars + reheated_mass) > 0.0) {
    double fac = galaxies[p].ColdGas / (stars + reheated_mass);
    fac = tap(l, pt, IM_RENORM_FAC, fac);
    stars *= fac;
    reheated_mass *= fac;
    l->flag[FL_RENORM_ACTIVE] = 1;
  }
  stars = tap(l, pt, IM_STARS, stars);
  reheated_mass = tap(l, pt, IM_REHEATED, reheated_mass);
  l->flag[FL_EJECTION_VVIR_NONPOSITIVE] = 0;
  if (galaxies[p].Vvir > 0.0) {
    ejected_mass =
        (run_params.FeedbackEjectionEfficiency * (run_params.EtaSNcode * run_params.EnergySNcode) /
             (galaxies[p].Vvir * galaxies[p].Vvir) -
         run_params.FeedbackReheatingEpsilon) *
        stars;
    ejected_mass = tap(l, pt, IM_EJECTED_PRE_FLOOR, ejected_mass);
  } else {
    ejected_mass = 0.0;
    l->flag[FL_EJECTION_VVIR_NONPOSITIVE] = 1;
  }
  l->flag[FL_EJECTED_FLOORED] = 0;
  if (ejected_mass < 0.0) {
    ejected_mass = 0.0;
    l->flag[FL_EJECTED_FLOORED] = 1;
  }
  ejected_mass = tap(l, pt, IM_EJECTED, ejected_mass);

  /* 5. TRANSCRIPTION of the disk history sampling, S:1058-1060 */
  galaxies[p].SfrDisk[a->step_bin] += stars / a->dt;
  galaxies[p].SfrDiskColdGas[a->step_bin] = galaxies[p].ColdGas;
  galaxies[p].SfrDiskColdGasMetals[a->step_bin] = galaxies[p].MetalsColdGas;

  /* 6. S:1063-1064: SAGE26's own get_metallicity and update_from_star_formation (or its
   * transcription, per comp) */
  double metallicity = get_metallicity(galaxies[p].ColdGas, galaxies[p].MetalsColdGas);
  metallicity = tap(l, pt, IM_METALLICITY_SF, metallicity);
  if (comp->transcribe_sf_commit) {
    transcribe_update_from_star_formation(p, stars, metallicity, l, pt);
  } else {
    update_from_star_formation(p, stars, metallicity, galaxies, &run_params);
  }

  /* 7. S:1076: SAGE26's own get_metallicity; TRANSCRIPTION of the clamp, S:1079-1082 */
  metallicity = get_metallicity(galaxies[p].ColdGas, galaxies[p].MetalsColdGas);
  metallicity = tap(l, pt, IM_METALLICITY_FB, metallicity);
  l->flag[FL_REHEAT_CLAMP_SF_BINDS] = 0;
  if (reheated_mass > galaxies[p].ColdGas) {
    reheated_mass = galaxies[p].ColdGas;
    l->flag[FL_REHEAT_CLAMP_SF_BINDS] = 1;
  }
  reheated_mass = tap(l, pt, IM_REHEATED_SF_CLAMPED, reheated_mass);

  /* S:1085 */
  if (comp->transcribe_feedback) {
    transcribe_update_from_feedback(p, centralgal, reheated_mass, ejected_mass, metallicity, l, pt);
  } else {
    call_update_from_feedback(p, centralgal, reheated_mass, ejected_mass, metallicity, l);
  }
  if (l->out_of_domain) {
    return;
  }

  /* 8. S:1088-1090 */
  if (run_params.DiskInstabilityOn) {
    if (comp->transcribe_instability) {
      transcribe_check_disk_instability(p, centralgal, a->time, a->dt, a->step_bin, ngal, 1, l, pt,
                                        instability);
    } else {
      check_disk_instability(p, centralgal, ORACLE_HALONR, a->time, a->dt, a->step_bin, galaxies,
                             &run_params);
    }
  }

  /* 9. TRANSCRIPTION of the yield, S:1093-1109 */
  l->flag[FL_YIELD_ABOVE_THRESHOLD] = 0;
  if (galaxies[p].ColdGas > 1.0e-8) {
    l->flag[FL_YIELD_ABOVE_THRESHOLD] = 1;
    double FracZleaveDiskVal =
        run_params.FracZleaveDisk * exp(-1.0 * galaxies[centralgal].Mvir / KD11_METAL_HALO_MASS);
    FracZleaveDiskVal = tap(l, pt, IM_FRAC_Z_LEAVE, FracZleaveDiskVal);

    galaxies[p].MetalsColdGas += run_params.Yield * (1.0 - FracZleaveDiskVal) * stars;

    double metals_leaving_disk = run_params.Yield * FracZleaveDiskVal * stars;
    metals_leaving_disk = tap(l, pt, IM_YIELD_TO_HOT, metals_leaving_disk);

    add_metals_to_hot_reservoir(&galaxies[centralgal], &run_params, metals_leaving_disk);
  } else {
    double all_metals = run_params.Yield * stars;
    all_metals = tap(l, pt, IM_YIELD_TO_HOT, all_metals);

    add_metals_to_hot_reservoir(&galaxies[centralgal], &run_params, all_metals);
  }

  memcpy(result, galaxies, (size_t)ngal * sizeof(galaxies[0]));
}

/** @brief Whether a replacement of one value by a neighbour changes the end state */
struct sensitivity {
  int up;
  int down;
};

/**
 * @brief Two-sided validation of one carried value of the wrapper
 *
 * Reruns the composition with the value replaced by its upward and then its downward
 * neighbour. SAGE26's own routines are used except for the one the value is a local of
 * (update_from_feedback or check_disk_instability), which is replaced by its checked
 * transcription. A direction counts only if the composition stays in SAGE26's domain
 * (no assertion would abort) and some recorded float field of the end state changes.
 */
static struct sensitivity validate_wrapper_value(const struct GALAXY *input,
                                                 const struct wrapper_args *a,
                                                 const struct GALAXY *expected, const int id) {
  struct composition comp = {0, 0, 0};
  comp.transcribe_feedback = IM_INFO[id].stage == STAGE_FEEDBACK;
  comp.transcribe_instability = IM_INFO[id].stage == STAGE_INSTABILITY;
  struct sensitivity s = {0, 0};
  for (int up = 0; up <= 1; up++) {
    struct perturbation pt = {id, -1, up, -1};
    struct locals l;
    struct GALAXY result[MAX_GALAXIES];
    struct instability_run inst;
    /* compose_wrapper fills result on every in-domain path; out of domain it is not read */
    compose_wrapper(input, a, &comp, &pt, &l, result, &inst);
    const int changed =
        !l.out_of_domain && !same_galaxies(result, expected, a->layout.ngal, BLOCK_FULL);
    if (up) {
      s.up = changed;
    } else {
      s.down = changed;
    }
  }
  return s;
}

/**
 * @brief Two-sided validation of one c08 value of the wrapper (SFprescription 1)
 *
 * As validate_wrapper_value, with the routine the value is a local of (update_from_star_formation
 * or update_from_feedback) replaced by its checked transcription.
 */
static struct sensitivity validate_wrapper_h2_value(const struct GALAXY *input,
                                                    const struct wrapper_args *a,
                                                    const struct GALAXY *expected, const int id) {
  struct composition comp = {0, 0, 0};
  comp.transcribe_feedback = IMH_INFO[id].stage == STAGE_FEEDBACK;
  comp.transcribe_sf_commit = IMH_INFO[id].stage == STAGE_SF_COMMIT;
  struct sensitivity s = {0, 0};
  for (int up = 0; up <= 1; up++) {
    struct perturbation pt = {-1, -1, up, id};
    struct locals l;
    struct GALAXY result[MAX_GALAXIES];
    struct instability_run inst;
    compose_wrapper(input, a, &comp, &pt, &l, result, &inst);
    const int changed =
        !l.out_of_domain && !same_galaxies(result, expected, a->layout.ngal, BLOCK_FULL);
    if (up) {
      s.up = changed;
    } else {
      s.down = changed;
    }
  }
  return s;
}

/** @brief One checked, validated wrapper call: its end state and carried values */
struct wrapper_record {
  struct GALAXY end[MAX_GALAXIES];
  struct locals locals;
  struct sensitivity sens[IM_COUNT];
  struct sensitivity h2_sens[IMH_COUNT];
};

/**
 * @brief Call SAGE26's starformation_and_feedback, check the recomposition, validate
 *
 * The oracle refuses the fixture unless (a) the composition with every transcription,
 * and (b) the composition with SAGE26's own update_from_star_formation, update_from_feedback
 * and check_disk_instability, each reproduce SAGE26's end state in every recorded field bit
 * for bit, and unless the instability (when on) stays off its gas branch: a gas-unstable
 * disk reaches grow_black_hole and collisional_starburst_recipe (c06), which the
 * check_disk_instability fixtures cover instead.
 */
static void run_checked_wrapper(const struct GALAXY *input, const struct wrapper_args *a,
                                struct wrapper_record *rec) {
  const int ngal = a->layout.ngal;

  /* SAGE26's own call (B:516) */
  memcpy(galaxies, input, (size_t)ngal * sizeof(galaxies[0]));
  starformation_and_feedback(a->layout.p, a->layout.centralgal, a->time, a->dt, ORACLE_HALONR,
                             a->step_bin, galaxies, &run_params);
  memcpy(rec->end, galaxies, (size_t)ngal * sizeof(galaxies[0]));

  /* (a) every block transcribed: the recorded carried values come from this run */
  const struct composition all_transcribed = {1, 1, 1};
  struct GALAXY result[MAX_GALAXIES];
  struct instability_run inst;
  compose_wrapper(input, a, &all_transcribed, &NO_PERTURBATION, &rec->locals, result, &inst);
  if (rec->locals.out_of_domain) {
    fatal("the recomposition left SAGE26's domain without a perturbation (oracle bug)");
  }
  if (inst.reached_gas_branch) {
    fatal("the disk is gas-unstable after feedback: check_disk_instability would reach "
          "grow_black_hole and collisional_starburst_recipe (c06); use DiskInstabilityOn 0 or "
          "a check_disk_instability fixture");
  }
  if (!same_galaxies(result, rec->end, ngal, BLOCK_FULL)) {
    fatal("transcription check failed: the fully transcribed recomposition does not reproduce "
          "starformation_and_feedback bit for bit");
  }

  /* (b) SAGE26's own update_from_star_formation, update_from_feedback and
   * check_disk_instability */
  const struct composition own_routines = {0, 0, 0};
  struct locals own_locals;
  compose_wrapper(input, a, &own_routines, &NO_PERTURBATION, &own_locals, result, &inst);
  if (own_locals.out_of_domain || !same_galaxies(result, rec->end, ngal, BLOCK_FULL)) {
    fatal("transcription check failed: the recomposition with SAGE26's own "
          "update_from_star_formation, update_from_feedback and check_disk_instability does not "
          "reproduce starformation_and_feedback bit for bit");
  }

  for (int id = 0; id < IM_COUNT; id++) {
    rec->sens[id].up = 0;
    rec->sens[id].down = 0;
    if (rec->locals.present[id]) {
      rec->sens[id] = validate_wrapper_value(input, a, rec->end, id);
    }
  }
  for (int id = 0; id < IMH_COUNT; id++) {
    rec->h2_sens[id].up = 0;
    rec->h2_sens[id].down = 0;
    if (rec->locals.h2_present[id]) {
      rec->h2_sens[id] = validate_wrapper_h2_value(input, a, rec->end, id);
    }
  }
}

/** @brief Write one carried value: absent marker, value, checked and validated markers */
static void out_carried(const char *prefix, const char *name, const int present, const double value,
                        const struct sensitivity *s) {
  out_i32(field_name("intermediate.%s%s_absent", prefix, name), present ? 0 : 1);
  if (!present) {
    return;
  }
  out_f64(field_name("intermediate.%s%s", prefix, name), value);
  out_i32(field_name("intermediate.%s%s_checked", prefix, name), 1);
  out_i32(field_name("intermediate.%s%s_validated_up", prefix, name), s->up);
  out_i32(field_name("intermediate.%s%s_validated_down", prefix, name), s->down);
  out_i32(field_name("intermediate.%s%s_validated", prefix, name), s->up && s->down);
}

static void out_flags(const char *prefix, const struct locals *l, const int first, const int last) {
  for (int f = first; f <= last; f++) {
    out_i32(field_name("intermediate.%s%s", prefix, FLAG_NAMES[f]), l->flag[f]);
  }
}

/** @brief The carried values of starformation_and_feedback, in SAGE26's statement order */
static const int WRAPPER_VALUES[] = {IM_TDYN,
                                     IM_COLD_CRIT,
                                     IM_STRDOT,
                                     IM_ATOMICH,
                                     IM_F_ION,
                                     IM_STARS_PRE_RENORM,
                                     IM_REHEATED_PRE_RENORM,
                                     IM_RENORM_FAC,
                                     IM_STARS,
                                     IM_REHEATED,
                                     IM_EJECTED_PRE_FLOOR,
                                     IM_EJECTED,
                                     IM_METALLICITY_SF,
                                     IM_METALLICITY_FB,
                                     IM_REHEATED_SF_CLAMPED,
                                     IM_REHEATED_FB_CLAMPED,
                                     IM_EJECTED_CAPPED,
                                     IM_METALLICITY_HOT,
                                     IM_METALS_TO_EJECT,
                                     IM_METALS_TO_EJECT_CAPPED,
                                     IM_DISKMASS,
                                     IM_MCRIT,
                                     IM_UNSTABLE_GAS,
                                     IM_UNSTABLE_STARS,
                                     IM_INSTABILITY_METALLICITY,
                                     IM_UNSTABLE_GAS_FRACTION,
                                     IM_FRAC_Z_LEAVE,
                                     IM_YIELD_TO_HOT};

/**
 * @brief Write one c08 value: c06's markers (tail-checked, check kind 2) and the narrowing
 *        limitation, the value as the float or double it is
 */
static void out_carried_h2(const struct locals *l, const int id, const struct sensitivity *s) {
  const char *name = IMH_INFO[id].name;
  out_i32(field_name("intermediate.%s_absent", name), l->h2_present[id] ? 0 : 1);
  if (!l->h2_present[id]) {
    return;
  }
  if (IMH_INFO[id].is_f32) {
    out_f32(field_name("intermediate.%s", name), (float)l->h2_value[id]);
  } else {
    out_f64(field_name("intermediate.%s", name), l->h2_value[id]);
  }
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), 2);
  out_i32(field_name("intermediate.%s_validated_up", name), s->up);
  out_i32(field_name("intermediate.%s_validated_down", name), s->down);
  out_i32(field_name("intermediate.%s_validated", name), s->up && s->down);
  out_i32(field_name("intermediate.%s_narrowing_limited", name), IMH_INFO[id].narrowing_limited);
}

static void out_h2_flags(const struct locals *l, const int first, const int last) {
  for (int f = first; f <= last; f++) {
    out_i32(field_name("intermediate.%s", H2_FLAG_NAMES[f]), l->h2_flag[f]);
  }
}

static void write_wrapper_record(const char *prefix, const struct wrapper_record *rec) {
  out_flags(prefix, &rec->locals, 0, FL_COUNT - 1);
  for (size_t k = 0; k < COUNT(WRAPPER_VALUES); k++) {
    const int id = WRAPPER_VALUES[k];
    out_carried(prefix, IM_INFO[id].name, rec->locals.present[id], rec->locals.value[id],
                &rec->sens[id]);
  }
  /* SFprescription 1 (c08): its flags and values, after every earlier record */
  if (run_params.SFprescription == 1) {
    out_h2_flags(&rec->locals, 0, FLH_COUNT - 1);
    for (int id = 0; id < IMH_COUNT; id++) {
      out_carried_h2(&rec->locals, id, &rec->h2_sens[id]);
    }
  }
}

// ============================================================================
// ROUTINES
// ============================================================================

/** @brief Every galaxy of the call, with the full block (halo, state, BH and histories) */
static void read_wrapper_galaxies(const struct layout *layout) {
  for (int i = 0; i < layout->ngal; i++) {
    read_galaxy(i, BLOCK_FULL);
  }
}

static void run_starformation_and_feedback(void) {
  read_r0_flags(1, 1);
  read_r0_params();
  h2_block_recorded = run_params.SFprescription == 1;
  struct wrapper_args a;
  a.layout = read_layout();
  const int effective_steps = read_effective_steps();
  const int step = in_i32("call.step");
  if (step < 0 || step >= effective_steps) {
    fatal("call.step = %d is outside [0, call.effective_steps)", step);
  }
  a.dt = read_dt(effective_steps);
  a.time = in_f64("call.time");
  a.step_bin = transcribe_step_bin(step, effective_steps);
  read_wrapper_galaxies(&a.layout);
  check_all_fields_used();

  /* run_checked_wrapper reuses the galaxies array, so it works from a copy of the input */
  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, (size_t)a.layout.ngal * sizeof(input[0]));
  static struct wrapper_record rec;
  run_checked_wrapper(input, &a, &rec);

  out_i32("intermediate.step_bin", a.step_bin);
  for (int i = 0; i < a.layout.ngal; i++) {
    write_galaxy("", i, &rec.end[i], BLOCK_FULL);
  }
  write_wrapper_record("", &rec);
}

/**
 * @brief TRANSCRIPTION of the writer's averaging of the histories, W:934-959
 *
 * The float accumulators, the left-to-right unit conversion with SAGE26's SEC_PER_YEAR
 * and SOLAR_MASS (R/src/macros.h:24, :37), and the integer divisors from SAGE26's own
 * sfr_rate_divisor and sfr_metallicity_divisor (R/src/model_misc.h:49-58).
 */
struct writer_sfr {
  float SfrDisk;
  float SfrBulge;
  float SfrDiskZ;
  float SfrBulgeZ;
  int sfr_norm;
  int met_norm;
  int disk_bins_skipped;
  int bulge_bins_skipped;
};

static struct writer_sfr transcribe_writer_sfr(const struct GALAXY *g, const struct params *rp) {
  struct writer_sfr w;
  w.disk_bins_skipped = 0;
  w.bulge_bins_skipped = 0;

  float tmp_SfrDisk = 0.0;
  float tmp_SfrBulge = 0.0;
  float tmp_SfrDiskZ = 0.0;
  float tmp_SfrBulgeZ = 0.0;

  const int sfr_norm = sfr_rate_divisor(g->SubstepsUsed);
  const int met_norm = sfr_metallicity_divisor(g->SubstepsUsed);
  for (int step = 0; step < STEPS; step++) {
    tmp_SfrDisk += g->SfrDisk[step] * rp->UnitMass_in_g / rp->UnitTime_in_s * SEC_PER_YEAR /
                   SOLAR_MASS / sfr_norm;
    tmp_SfrBulge += g->SfrBulge[step] * rp->UnitMass_in_g / rp->UnitTime_in_s * SEC_PER_YEAR /
                    SOLAR_MASS / sfr_norm;

    if (g->SfrDiskColdGas[step] > 0.0) {
      tmp_SfrDiskZ += g->SfrDiskColdGasMetals[step] / g->SfrDiskColdGas[step] / met_norm;
    } else {
      w.disk_bins_skipped++;
    }

    if (g->SfrBulgeColdGas[step] > 0.0) {
      tmp_SfrBulgeZ += g->SfrBulgeColdGasMetals[step] / g->SfrBulgeColdGas[step] / met_norm;
    } else {
      w.bulge_bins_skipped++;
    }
  }

  w.SfrDisk = tmp_SfrDisk;
  w.SfrBulge = tmp_SfrBulge;
  w.SfrDiskZ = tmp_SfrDiskZ;
  w.SfrBulgeZ = tmp_SfrBulgeZ;
  w.sfr_norm = sfr_norm;
  w.met_norm = met_norm;
  return w;
}

static void write_writer_sfr(const char *prefix, const struct writer_sfr *w) {
  out_i32(field_name("%ssfr_rate_divisor", prefix), w->sfr_norm);
  out_i32(field_name("%ssfr_metallicity_divisor", prefix), w->met_norm);
  out_i32(field_name("%sSfrDiskZ_bins_skipped", prefix), w->disk_bins_skipped);
  out_i32(field_name("%sSfrBulgeZ_bins_skipped", prefix), w->bulge_bins_skipped);
  out_f32(field_name("%sSfrDisk", prefix), w->SfrDisk);
  out_f32(field_name("%sSfrBulge", prefix), w->SfrBulge);
  out_f32(field_name("%sSfrDiskZ", prefix), w->SfrDiskZ);
  out_f32(field_name("%sSfrBulgeZ", prefix), w->SfrBulgeZ);
}

static void read_writer_units(void) {
  run_params.UnitMass_in_g = in_f64("params.UnitMass_in_g");
  run_params.UnitTime_in_s = in_f64("params.UnitTime_in_s");
}

/**
 * @brief starformation_and_feedback over every substep of one galaxy (B:470-516)
 *
 * Runs SAGE26's own call for step = 0 .. effective_steps - 1 on the evolving state, with
 * the transcribed step bin and dt, each substep checked and validated as a single call.
 * Before the loop, the TRANSCRIPTION of B:466-467 sets SubstepsUsed = effective_steps on
 * every galaxy. After it, the writer's averaging (W:934-959) of each galaxy.
 */
static void run_starformation_and_feedback_sequence(void) {
  read_r0_flags(1, 0);
  read_r0_params();
  read_writer_units();
  struct wrapper_args a;
  a.layout = read_layout();
  const int effective_steps = read_effective_steps();
  a.dt = read_dt(effective_steps);
  double time[MAX_STEPS];
  for (int k = 0; k < effective_steps; k++) {
    time[k] = in_f64(field_name("call.substep[%d].time", k));
  }
  read_wrapper_galaxies(&a.layout);
  check_all_fields_used();

  struct GALAXY state[MAX_GALAXIES];
  memcpy(state, galaxies, (size_t)a.layout.ngal * sizeof(state[0]));

  /* TRANSCRIPTION of B:466-467 */
  for (int p = 0; p < a.layout.ngal; p++) {
    state[p].SubstepsUsed = effective_steps;
  }

  static const int SEQUENCE_VALUES[] = {IM_STARS, IM_REHEATED, IM_EJECTED};
  static struct wrapper_record rec;
  for (int step = 0; step < effective_steps; step++) {
    a.step_bin = transcribe_step_bin(step, effective_steps);
    a.time = time[step];
    run_checked_wrapper(state, &a, &rec);
    memcpy(state, rec.end, (size_t)a.layout.ngal * sizeof(state[0]));

    char prefix[NAME_LEN];
    snprintf(prefix, sizeof(prefix), "substep[%d].", step);
    out_i32(field_name("intermediate.%sstep_bin", prefix), a.step_bin);
    for (size_t k = 0; k < COUNT(SEQUENCE_VALUES); k++) {
      const int id = SEQUENCE_VALUES[k];
      out_carried(prefix, IM_INFO[id].name, rec.locals.present[id], rec.locals.value[id],
                  &rec.sens[id]);
    }
  }

  for (int i = 0; i < a.layout.ngal; i++) {
    out_i32(galaxy_field("", i, "SubstepsUsed"), state[i].SubstepsUsed);
    write_galaxy("", i, &state[i], BLOCK_FULL);
  }
  for (int i = 0; i < a.layout.ngal; i++) {
    const struct writer_sfr w = transcribe_writer_sfr(&state[i], &run_params);
    char prefix[NAME_LEN];
    snprintf(prefix, sizeof(prefix), "intermediate.writer.galaxy[%d].", i);
    write_writer_sfr(prefix, &w);
  }
}

/**
 * @brief update_from_star_formation as a burst calls it (R/src/model_mergers.c:759-760)
 *
 * With SFprescription 1 (c08) the TRANSCRIPTION transcribe_update_from_star_formation must also
 * reproduce SAGE26's end state bit for bit; it gives the hydrogen cap's flags and max_h
 * (S:1131-1135), validated two-sided through that transcription's remainder.
 */
static void run_update_from_star_formation(void) {
  read_sf_prescription(1);
  run_params.RecycleFraction = in_f64("params.RecycleFraction");
  const double stars = in_f64("call.stars");
  read_galaxy(0, BLOCK_STATE);
  check_all_fields_used();

  struct GALAXY input = galaxies[0];
  const double metallicity = get_metallicity(galaxies[0].ColdGas, galaxies[0].MetalsColdGas);
  update_from_star_formation(0, stars, metallicity, galaxies, &run_params);
  const struct GALAXY end = galaxies[0];

  out_f64("intermediate.metallicity", metallicity);
  write_galaxy("", 0, &end, BLOCK_STATE);
  if (run_params.SFprescription != 1) {
    return;
  }

  struct locals l;
  init_locals(&l);
  galaxies[0] = input;
  transcribe_update_from_star_formation(0, stars, metallicity, &l, &NO_PERTURBATION);
  if (!same_galaxies(galaxies, &end, 1, BLOCK_STATE)) {
    fatal("transcription check failed: transcribe_update_from_star_formation does not reproduce "
          "update_from_star_formation bit for bit");
  }
  struct sensitivity sens = {0, 0};
  for (int up = 0; up <= 1; up++) {
    const struct perturbation pt = {-1, -1, up, IMH_MAX_H_SF};
    struct locals lp;
    init_locals(&lp);
    galaxies[0] = input;
    transcribe_update_from_star_formation(0, stars, metallicity, &lp, &pt);
    const int changed = !same_galaxies(galaxies, &end, 1, BLOCK_STATE);
    if (up) {
      sens.up = changed;
    } else {
      sens.down = changed;
    }
  }
  out_h2_flags(&l, FLH_CAPS_SF_TRACKED, FLH_H1_CAP_SF_BINDS);
  out_carried_h2(&l, IMH_MAX_H_SF, &sens);
}

/**
 * @brief update_from_feedback as a burst calls it (R/src/model_mergers.c:790-791)
 *
 * The metallicity is SAGE26's own get_metallicity of the forming galaxy's cold gas, as at
 * R/src/model_mergers.c:788. The routine's locals (the clamped reheated mass, the capped
 * ejected mass, the hot metallicity and the metals ejected) come from the TRANSCRIPTION
 * transcribe_update_from_feedback, which must reproduce SAGE26's end state bit for bit,
 * and are validated two-sided through that transcription's remainder. With SFprescription 1
 * (c08) the transcription includes the hydrogen cap (S:1169-1173), whose flags and max_h_fb are
 * recorded after every earlier field.
 */
static void run_update_from_feedback(void) {
  read_sf_prescription(1);
  run_params.SupernovaRecipeOn = require_flag("SupernovaRecipeOn", 1);
  run_params.CGMrecipeOn = require_flag("CGMrecipeOn", 0);
  const struct layout layout = read_layout();
  const double reheated_mass = in_f64("call.reheated_mass");
  const double ejected_mass = in_f64("call.ejected_mass");
  for (int i = 0; i < layout.ngal; i++) {
    read_galaxy(i, BLOCK_STATE);
  }
  check_all_fields_used();
  const int p = layout.p;
  const int c = layout.centralgal;
  const int ngal = layout.ngal;

  /* SAGE26's assertions (S:1158-1163) must hold, or it would abort */
  if (!feedback_in_domain(p, reheated_mass)) {
    fatal("call.reheated_mass fails SAGE26's assertions in update_from_feedback (S:1158-1163)");
  }

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, (size_t)ngal * sizeof(input[0]));

  const double metallicity = get_metallicity(input[p].ColdGas, input[p].MetalsColdGas);
  memcpy(galaxies, input, (size_t)ngal * sizeof(galaxies[0]));
  update_from_feedback(p, c, reheated_mass, ejected_mass, metallicity, galaxies, &run_params);
  struct GALAXY end[MAX_GALAXIES];
  memcpy(end, galaxies, (size_t)ngal * sizeof(end[0]));

  struct locals l;
  init_locals(&l);
  memcpy(galaxies, input, (size_t)ngal * sizeof(galaxies[0]));
  transcribe_update_from_feedback(p, c, reheated_mass, ejected_mass, metallicity, &l,
                                  &NO_PERTURBATION);
  if (l.out_of_domain || !same_galaxies(galaxies, end, ngal, BLOCK_STATE)) {
    fatal("transcription check failed: transcribe_update_from_feedback does not reproduce "
          "update_from_feedback bit for bit");
  }

  static const int FEEDBACK_VALUES[] = {IM_REHEATED_FB_CLAMPED, IM_EJECTED_CAPPED,
                                        IM_METALLICITY_HOT, IM_METALS_TO_EJECT,
                                        IM_METALS_TO_EJECT_CAPPED};
  struct sensitivity sens[IM_COUNT];
  memset(sens, 0, sizeof(sens));
  for (size_t k = 0; k < COUNT(FEEDBACK_VALUES); k++) {
    const int id = FEEDBACK_VALUES[k];
    for (int up = 0; up <= 1; up++) {
      struct perturbation pt = {id, -1, up, -1};
      struct locals lp;
      init_locals(&lp);
      memcpy(galaxies, input, (size_t)ngal * sizeof(galaxies[0]));
      transcribe_update_from_feedback(p, c, reheated_mass, ejected_mass, metallicity, &lp, &pt);
      const int changed = !lp.out_of_domain && !same_galaxies(galaxies, end, ngal, BLOCK_STATE);
      if (up) {
        sens[id].up = changed;
      } else {
        sens[id].down = changed;
      }
    }
  }

  out_f64("intermediate.metallicity", metallicity);
  for (int i = 0; i < ngal; i++) {
    write_galaxy("", i, &end[i], BLOCK_STATE);
  }
  out_flags("", &l, FL_REHEAT_CLAMP_FB_BINDS, FL_METALS_CAP_BINDS);
  for (size_t k = 0; k < COUNT(FEEDBACK_VALUES); k++) {
    const int id = FEEDBACK_VALUES[k];
    out_carried("", IM_INFO[id].name, l.present[id], l.value[id], &sens[id]);
  }
  if (run_params.SFprescription != 1) {
    return;
  }

  /* SFprescription 1 (c08): the hydrogen cap's flags and max_h_fb, validated two-sided */
  struct sensitivity h2_sens = {0, 0};
  if (l.h2_present[IMH_MAX_H_FB]) {
    for (int up = 0; up <= 1; up++) {
      const struct perturbation pt = {-1, -1, up, IMH_MAX_H_FB};
      struct locals lp;
      init_locals(&lp);
      memcpy(galaxies, input, (size_t)ngal * sizeof(galaxies[0]));
      transcribe_update_from_feedback(p, c, reheated_mass, ejected_mass, metallicity, &lp, &pt);
      const int changed = !lp.out_of_domain && !same_galaxies(galaxies, end, ngal, BLOCK_STATE);
      if (up) {
        h2_sens.up = changed;
      } else {
        h2_sens.down = changed;
      }
    }
  }
  out_h2_flags(&l, FLH_CAPS_FB_TRACKED, FLH_H1_CAP_FB_BINDS);
  out_carried_h2(&l, IMH_MAX_H_FB, &h2_sens);
}

/** @brief The arguments of one check_disk_instability call */
struct instability_call {
  int p;
  int centralgal;
  int ngal;
  int step;
  double time;
  double dt;
};

/**
 * @brief Two-sided validation of one instability value: rerun the transcription (composed
 *        with SAGE26's c06 continuation where the gas branch is reached) with the double value
 *        im, or the post-transfer float field post, replaced by each neighbour in turn
 */
static struct sensitivity instability_sensitivity(const struct instability_call *call,
                                                  const struct GALAXY *input,
                                                  const struct GALAXY *expected, const int im,
                                                  const int post) {
  struct sensitivity s = {0, 0};
  for (int up = 0; up <= 1; up++) {
    const struct perturbation pt = {im, post, up, -1};
    struct locals lp;
    init_locals(&lp);
    static struct instability_run rp;
    memcpy(galaxies, input, (size_t)call->ngal * sizeof(galaxies[0]));
    transcribe_check_disk_instability(call->p, call->centralgal, call->time, call->dt, call->step,
                                      call->ngal, 1, &lp, &pt, &rp);
    const int changed = !same_galaxies(galaxies, expected, call->ngal, BLOCK_FULL);
    if (up) {
      s.up = changed;
    } else {
      s.down = changed;
    }
  }
  return s;
}

/**
 * @brief check_disk_instability (D:47-110), with its transcription checked
 *
 * SAGE26's own routine gives the end state. The TRANSCRIPTION of D:54-101, composed with
 * SAGE26's own grow_black_hole and collisional_starburst_recipe where the gas branch is
 * reached, must reproduce that end state bit for bit, or the oracle refuses the fixture.
 * On a stable disk or one with no cold gas (no gas branch) the recorded state is SAGE26's
 * end state. On a gas-unstable disk the recorded state is the transcription's post-transfer
 * state (before the c06 continuation) and SAGE26's full end state is recorded separately,
 * as c06's, under intermediate.c06.
 */
static void run_check_disk_instability(void) {
  read_r0_flags(0, 0);
  read_r0_params();
  const struct layout layout = read_layout();
  const double time = in_f64("call.time");
  const double dt = in_f64("call.dt");
  const int step = in_i32("call.step_bin");
  for (int i = 0; i < layout.ngal; i++) {
    read_galaxy(i, BLOCK_FULL);
  }
  check_all_fields_used();
  /* collisional_starburst_recipe's assertions (R/src/model_mergers.c:589-592) */
  if (!(dt > 0.0) || step < 0 || step >= STEPS) {
    fatal("call.dt must be positive and call.step_bin in [0, %d)", STEPS);
  }
  const int p = layout.p;
  const int c = layout.centralgal;
  const int ngal = layout.ngal;

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, (size_t)ngal * sizeof(input[0]));

  check_disk_instability(p, c, ORACLE_HALONR, time, dt, step, galaxies, &run_params);
  struct GALAXY end[MAX_GALAXIES];
  memcpy(end, galaxies, (size_t)ngal * sizeof(end[0]));

  struct locals l;
  init_locals(&l);
  static struct instability_run run;
  memcpy(galaxies, input, (size_t)ngal * sizeof(galaxies[0]));
  transcribe_check_disk_instability(p, c, time, dt, step, ngal, 1, &l, &NO_PERTURBATION, &run);
  if (!same_galaxies(galaxies, end, ngal, BLOCK_FULL)) {
    fatal("transcription check failed: the transcribed instability%s does not reproduce "
          "check_disk_instability bit for bit",
          run.reached_gas_branch ? ", composed with grow_black_hole and "
                                   "collisional_starburst_recipe,"
                                 : "");
  }
  struct GALAXY post_transfer[MAX_GALAXIES];
  memcpy(post_transfer, run.post_transfer, (size_t)ngal * sizeof(post_transfer[0]));
  const int gas = run.reached_gas_branch;

  static const int INSTABILITY_VALUES[] = {IM_DISKMASS,
                                           IM_MCRIT,
                                           IM_UNSTABLE_GAS,
                                           IM_UNSTABLE_STARS,
                                           IM_INSTABILITY_METALLICITY,
                                           IM_UNSTABLE_GAS_FRACTION};
  struct sensitivity sens[IM_COUNT];
  struct sensitivity post_sens[PT_COUNT];
  memset(sens, 0, sizeof(sens));
  memset(post_sens, 0, sizeof(post_sens));
  const struct instability_call call = {p, c, ngal, step, time, dt};
  for (size_t k = 0; k < COUNT(INSTABILITY_VALUES); k++) {
    const int id = INSTABILITY_VALUES[k];
    if (l.present[id]) {
      sens[id] = instability_sensitivity(&call, input, end, id, -1);
    }
  }
  /* The post-transfer fields perturbed: the three masses, and the three radii in mode 3 */
  const int post_count = sizes_recorded ? PT_COUNT : PT_R0_COUNT;
  if (gas) {
    for (int k = 0; k < post_count; k++) {
      post_sens[k] = instability_sensitivity(&call, input, end, -1, k);
    }
  }

  out_i32("intermediate.state_is_post_transfer", gas);
  for (int i = 0; i < ngal; i++) {
    write_galaxy("", i, gas ? &post_transfer[i] : &end[i], BLOCK_FULL);
  }
  out_flags("", &l, FL_DISK_POSITIVE, FL_GAS_BRANCH);
  for (size_t k = 0; k < COUNT(INSTABILITY_VALUES); k++) {
    const int id = INSTABILITY_VALUES[k];
    out_carried("", IM_INFO[id].name, l.present[id], l.value[id], &sens[id]);
  }
  static const char *const POST_NAMES[PT_COUNT] = {
      "BulgeMass",   "MetalsBulgeMass",   "InstabilityBulgeMass",
      "BulgeRadius", "MergerBulgeRadius", "InstabilityBulgeRadius"};
  out_i32("intermediate.post_transfer_absent", gas ? 0 : 1);
  if (gas) {
    for (int k = 0; k < post_count; k++) {
      out_i32(field_name("intermediate.post_transfer.%s_checked", POST_NAMES[k]), 1);
      out_i32(field_name("intermediate.post_transfer.%s_validated_up", POST_NAMES[k]),
              post_sens[k].up);
      out_i32(field_name("intermediate.post_transfer.%s_validated_down", POST_NAMES[k]),
              post_sens[k].down);
      out_i32(field_name("intermediate.post_transfer.%s_validated", POST_NAMES[k]),
              post_sens[k].up && post_sens[k].down);
    }
    for (int i = 0; i < ngal; i++) {
      write_galaxy("intermediate.c06.", i, &end[i], BLOCK_FULL);
    }
  }
}

/** @brief TRANSCRIPTION of the step bin and dt for every substep of a snapshot (B:513-516) */
static void run_step_bin_map(void) {
  const int effective_steps = read_effective_steps();
  const double deltaT = in_f64("call.deltaT");
  check_all_fields_used();
  out_f64("intermediate.dt", transcribe_dt(deltaT, effective_steps));
  for (int step = 0; step < effective_steps; step++) {
    out_i32(field_name("intermediate.step[%d].step_bin", step),
            transcribe_step_bin(step, effective_steps));
  }
}

/** @brief The writer's averaging (W:934-959) of one galaxy's histories */
static void run_sfr_history_average(void) {
  read_writer_units();
  struct GALAXY *g = &galaxies[0];
  g->SubstepsUsed = in_i32("galaxy[0].SubstepsUsed");
  for (size_t m = 0; m < COUNT(HISTORY_MEMBERS); m++) {
    for (int k = 0; k < STEPS; k++) {
      member_ptr(g, &HISTORY_MEMBERS[m])[k] =
          in_f32(history_field("", 0, HISTORY_MEMBERS[m].name, k));
    }
  }
  check_all_fields_used();
  const struct writer_sfr w = transcribe_writer_sfr(g, &run_params);
  write_writer_sfr("intermediate.writer.", &w);
}

/**
 * @brief TRANSCRIPTION of the end-of-halo normalisation, B:590-604, and of the writer's
 *        OutflowRate conversion, W:995
 *
 * evolve_galaxies is static in core_build_model.c and the writer is not in this link, so
 * these lines are restated here:
 *
 *   const double deltaT = run_params->Age[galaxies[0].SnapNum] - halo_age;
 *   const double inv_deltaT = 1.0/deltaT;
 *   for each galaxy p: if mergeType > 0 skip;
 *       Cooling *= inv_deltaT; Heating *= inv_deltaT; OutflowRate *= inv_deltaT;
 *
 * and, for every galaxy written (merged ones included), the float output
 *   OutflowRate * UnitMass_in_g / UnitTime_in_s * SEC_PER_YEAR / SOLAR_MASS.
 * call.Age[<snap>] supplies Age for every SnapNum a galaxy carries. As in oracle_c04.c, a
 * galaxy with mergeType > 0 must be Type 3 (the package's convention, not SAGE26's).
 */
static void run_normalise_outflow_rate(void) {
  read_writer_units();
  const int ngal = in_i32("call.ngal");
  if (ngal < 1 || ngal > MAX_GALAXIES) {
    fatal("call.ngal = %d is outside [1, %d]", ngal, MAX_GALAXIES);
  }
  const double halo_age = in_f64("call.halo_age");
  double age[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    struct GALAXY *g = &galaxies[i];
    g->Type = in_i32(galaxy_field("", i, "Type"));
    g->SnapNum = in_i32(galaxy_field("", i, "SnapNum"));
    g->mergeType = in_i32(galaxy_field("", i, "mergeType"));
    g->Cooling = in_f64(galaxy_field("", i, "Cooling"));
    g->Heating = in_f64(galaxy_field("", i, "Heating"));
    g->OutflowRate = in_f32(galaxy_field("", i, "OutflowRate"));
    if (g->mergeType < 0 || (g->mergeType > 0) != (g->Type == 3)) {
      fatal("galaxy[%d]: mergeType %d with Type %d (the package marks a merged galaxy Type 3)", i,
            g->mergeType, g->Type);
    }
    /* Each distinct SnapNum's Age is one input field, read at its first use */
    int seen = -1;
    for (int j = 0; j < i; j++) {
      if (galaxies[j].SnapNum == g->SnapNum) {
        seen = j;
      }
    }
    age[i] = seen >= 0 ? age[seen] : in_f64(field_name("call.Age[%d]", g->SnapNum));
  }
  check_all_fields_used();

  /* B:591-592: galaxy zero's interval sets the normalisation of every galaxy */
  const double deltaT = age[0] - halo_age;
  if (!(deltaT > 0.0)) {
    fatal("galaxy zero's deltaT = %.17g must be positive (it is on R0)", deltaT);
  }
  const double inv_deltaT = 1.0 / deltaT;

  /* B:594-604 */
  int normalised[MAX_GALAXIES];
  for (int p = 0; p < ngal; p++) {
    normalised[p] = 0;
    if (galaxies[p].mergeType > 0) {
      continue;
    }
    galaxies[p].Cooling *= inv_deltaT;
    galaxies[p].Heating *= inv_deltaT;
    galaxies[p].OutflowRate *= inv_deltaT;
    normalised[p] = 1;
  }

  out_f64("intermediate.deltaT", deltaT);
  out_f64("intermediate.inv_deltaT", inv_deltaT);
  for (int i = 0; i < ngal; i++) {
    const struct GALAXY *g = &galaxies[i];
    /* W:995: the float output of the writer */
    const float outflow_output = g->OutflowRate * run_params.UnitMass_in_g /
                                 run_params.UnitTime_in_s * SEC_PER_YEAR / SOLAR_MASS;
    out_f64(galaxy_field("intermediate.", i, "deltaT"), age[i] - halo_age);
    out_i32(galaxy_field("intermediate.", i, "normalised"), normalised[i]);
    out_f64(galaxy_field("", i, "Cooling"), g->Cooling);
    out_f64(galaxy_field("", i, "Heating"), g->Heating);
    out_f32(galaxy_field("", i, "OutflowRate"), g->OutflowRate);
    out_f32(galaxy_field("intermediate.writer.", i, "OutflowRate"), outflow_output);
  }
}

// ============================================================================
// MAIN
// ============================================================================

struct routine_entry {
  const char *name;
  void (*run)(void);
};

static const struct routine_entry routines[] = {
    {"starformation_and_feedback", run_starformation_and_feedback},
    {"starformation_and_feedback_sequence", run_starformation_and_feedback_sequence},
    {"update_from_star_formation", run_update_from_star_formation},
    {"update_from_feedback", run_update_from_feedback},
    {"check_disk_instability", run_check_disk_instability},
    {"step_bin_map", run_step_bin_map},
    {"sfr_history_average", run_sfr_history_average},
    {"normalise_outflow_rate", run_normalise_outflow_rate},
};

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <fixture.in> <fixture.out>\n", argv[0]);
    return EXIT_FAILURE;
  }
  input_path = argv[1];
  read_input(input_path);

  const struct routine_entry *entry = NULL;
  for (size_t i = 0; i < sizeof(routines) / sizeof(routines[0]); i++) {
    if (strcmp(routines[i].name, routine_name) == 0) {
      entry = &routines[i];
    }
  }
  if (entry == NULL) {
    fatal("unknown routine '%s'", routine_name);
  }

  output_path = argv[2];
  output_file = fopen(output_path, "w");
  if (output_file == NULL) {
    fatal("cannot open output %s: %s", output_path, strerror(errno));
  }
  fprintf(output_file,
          "# SAGE26 oracle output for %s, written by oracle_c05 (regenerate.sh); do not edit.\n",
          base_name(input_path));
  fprintf(output_file, "%s\nroutine %s\n", ORACLE_FORMAT_LINE, routine_name);

  poison_state();
  entry->run();

  const int close_status = fclose(output_file);
  output_file = NULL;
  if (close_status != 0) {
    remove(output_path);
    fatal("cannot write output %s", output_path);
  }
  return EXIT_SUCCESS;
}
