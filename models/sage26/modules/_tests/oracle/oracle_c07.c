/**
 * @file    oracle_c07.c
 * @brief   SAGE26 function-level oracle for the bulge-size routines and the central's
 *          bulge-radius synchronisation
 *
 * Reads one fixture input file, initialises the SAGE26 struct GALAXY, struct halo_data and
 * struct params fields its reader for the selected routine consumes (a superset of the fields
 * the routine reads), calls the routine from the SAGE26 objects this executable is linked
 * against, and writes a fixture output file holding the routine's return value, every field
 * it writes or reads on the galaxy it touches and its locals, which the package's size helpers
 * report for tests.
 *
 * Usage: oracle_c07 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one; R/ is the staged SAGE26 source root,
 * X = R/src/model_misc.c, B = R/src/core_build_model.c):
 *   get_bulge_radius                X:239-377, SAGE26's own public routine, called directly
 *   update_instability_bulge_radius X:385-424, SAGE26's own public routine, called directly
 *   central_disk_and_bulge_radius   a SOURCE-REVIEWED TRANSCRIPTION of the FoF central's disk
 *                                   radius and bulge-radius synchronisation during progenitor
 *                                   joining, B:297-298, calling SAGE26's own get_disk_radius and
 *                                   get_bulge_radius (B is static driver code, not in the link)
 *
 * Routine-internal locals. Every local of the two public routines is obtained from a
 * TRANSCRIPTION of the routine below (each statement cited to its line) and is CHECKED: the
 * transcription, composed with SAGE26's own routines, must reproduce SAGE26's own end state (and
 * return value) bit for bit, or the oracle refuses the fixture. Neither routine passes a local
 * to a SAGE26 function (update_instability_bulge_radius passes only the galaxy index to
 * get_bulge_radius, X:423), so every local is tail-checked: the composition runs the transcribed
 * rest of the routine from that value. Each value is then replaced by its upward and its
 * downward neighbouring double and the same composition rerun (two-sided validation).
 *
 * The central's synchronisation is static driver code, so no SAGE26 code anchors
 * it: its fixtures are named tr_* and a mutation sweep covers it (README.md,
 * "The c07 Group", "Driver Transcription").
 *
 * Every transcription restates SAGE26's statements with the same expression shapes, operand types
 * and operand order and is compiled with the same flags, so floating-point contraction (clang's
 * default -ffp-contract=on) treats them as it treats SAGE26's; the "Fused Multiply-Adds" table
 * under README.md's "The c07 Group" lists the sites, checked in this file's disassembly.
 * File-static SAGE26 constants are restated with their values and source lines.
 *
 * The file format is defined in README.md ("Fixture File Format"). The parsing and output helpers
 * are copies of oracle_c06.c's.
 *
 * This is SAGE26 reference tooling: it is compiled and linked only by build.sh, never by
 * Mimic's Makefile, and never against Mimic code.
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_allvars.h"
#include "macros.h"
#include "model_misc.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 1024
#define MAX_GALAXIES 1
#define MAX_HALOS 1
#define NAME_LEN 96
#define LINE_LEN 512

/* SAGE26 file-static constants, restated (values and lines of the staged source) */
static const double SHEN03_SLOPE_HIGH = 0.56;      /* X:29 */
static const double SHEN03_INTERCEPT_HIGH = -5.54; /* X:30 */
static const double TONINI16_DISK_FRAC = 0.2;      /* X:55 */

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
static struct halo_data halos[MAX_HALOS];
static struct params run_params;

// ============================================================================
// ERROR HANDLING AND NAMES
// ============================================================================

static void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void fatal(const char *fmt, ...) {
  va_list args;
  fprintf(stderr, "oracle_c07: %s: ", input_path != NULL ? input_path : "(no input)");
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

static int same_f64(const double a, const double b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static int same_f32(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

// ============================================================================
// GALAXY FIELDS: THE BULGE-SIZE BLOCK
// ============================================================================

/** @brief A float member of SAGE26's struct GALAXY */
struct member {
  const char *name;
  float *(*access)(struct GALAXY *g);
};

#define SCALAR_ACCESSOR(m)                                                                         \
  static float *access_##m(struct GALAXY *g) { return &g->m; }

SCALAR_ACCESSOR(MergerBulgeMass)
SCALAR_ACCESSOR(InstabilityBulgeMass)
SCALAR_ACCESSOR(DiskScaleRadius)
SCALAR_ACCESSOR(BulgeRadius)
SCALAR_ACCESSOR(MergerBulgeRadius)
SCALAR_ACCESSOR(InstabilityBulgeRadius)

#define MEMBER(m) {#m, access_##m}
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** Every member the two size routines read or write (X:318-368, :399-422): the two channel
 *  masses, the disk radius (the instability fallback, X:353) and the three bulge radii */
static const struct member SIZE_MEMBERS[] = {
    MEMBER(MergerBulgeMass), MEMBER(InstabilityBulgeMass), MEMBER(DiskScaleRadius),
    MEMBER(BulgeRadius),     MEMBER(MergerBulgeRadius),    MEMBER(InstabilityBulgeRadius)};

static const char *galaxy_field(const int index, const char *member) {
  return field_name("galaxy[%d].%s", index, member);
}

static void read_size_block(const int i) {
  for (size_t m = 0; m < COUNT(SIZE_MEMBERS); m++) {
    *SIZE_MEMBERS[m].access(&galaxies[i]) = in_f32(galaxy_field(i, SIZE_MEMBERS[m].name));
  }
}

static void write_size_block(const int i, const struct GALAXY *g) {
  for (size_t m = 0; m < COUNT(SIZE_MEMBERS); m++) {
    out_f32(galaxy_field(i, SIZE_MEMBERS[m].name), *SIZE_MEMBERS[m].access((struct GALAXY *)g));
  }
}

/** @brief Whether the size blocks of two galaxies hold the same bit patterns */
static int same_size_block(const struct GALAXY *a, const struct GALAXY *b) {
  for (size_t m = 0; m < COUNT(SIZE_MEMBERS); m++) {
    if (!same_f32(*SIZE_MEMBERS[m].access((struct GALAXY *)a),
                  *SIZE_MEMBERS[m].access((struct GALAXY *)b))) {
      return 0;
    }
  }
  return 1;
}

// ============================================================================
// STATE INITIALISATION
// ============================================================================

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(halos, 0xff, sizeof(halos));
  memset(&run_params, 0xff, sizeof(run_params));
}

/**
 * @brief BulgeSizeOn as the fixture sets it, and Hubble_h when the sizes are on
 *
 * As oracle_c06.c's reader: BulgeSizeOn is 0 (R0) or 3 (R1 onward, the Tonini sizes);
 * modes 1 and 2 are not ported and any other value is refused, naming the parameter. Mode 3
 * reads Hubble_h (X:249, :398), so a mode-3 fixture must carry params.Hubble_h; a mode-0 fixture
 * does not, and one that carries it is refused as an unread field (neither routine reads it in
 * mode 0, X:242-247, :395).
 */
static void read_bulge_size_config(void) {
  run_params.BulgeSizeOn = in_i32("params.BulgeSizeOn");
  if (run_params.BulgeSizeOn != 0 && run_params.BulgeSizeOn != 3) {
    fatal("params.BulgeSizeOn = %d, but the oracle supports only 0 (R0) and 3 (R1 onward)",
          run_params.BulgeSizeOn);
  }
  if (run_params.BulgeSizeOn == 3) {
    run_params.Hubble_h = in_f64("params.Hubble_h");
  }
}

/** @brief The galaxy index of the call: the oracle holds one galaxy, index 0 */
static int read_call_p(void) {
  const int p = in_i32("call.p");
  if (p != 0) {
    fatal("call.p = %d, but the oracle holds one galaxy (index 0)", p);
  }
  return p;
}

// ============================================================================
// CARRIED LOCALS
// ============================================================================

/** The SAGE26 routine a local belongs to */
enum stage {
  STAGE_GET_BULGE_RADIUS,   /* X:239-377 */
  STAGE_UPDATE_INSTABILITY, /* X:385-424 */
};

enum im_id {
  IM_M_MERGER,
  IM_M_INSTABILITY,
  IM_M_TOTAL,
  IM_M_MERGER_SUN,
  IM_LOG_R_MERGER_KPC,
  IM_R_MERGER,
  IM_R_DISC,
  IM_M_INST_SUN,
  IM_LOG_R_INST_KPC,
  IM_R_INSTABILITY,
  IM_R_BULGE,
  IM_M_OLD,
  IM_R_OLD,
  IM_R_DISC_KPC,
  IM_R_NEW_CONTRIBUTION_KPC,
  IM_R_NEW_CONTRIBUTION,
  IM_R_OLD_KPC,
  IM_M_NEW,
  IM_R_NEW_KPC,
  IM_R_NEW,
  IM_COUNT
};

/* Every local is tail-checked (file comment); the check kind is written as 2 (README) */
#define CHECK_TAIL 2

static const struct {
  const char *name;
  enum stage stage;
} IM_INFO[IM_COUNT] = {
    [IM_M_MERGER] = {"M_merger", STAGE_GET_BULGE_RADIUS},
    [IM_M_INSTABILITY] = {"M_instability", STAGE_GET_BULGE_RADIUS},
    [IM_M_TOTAL] = {"M_total", STAGE_GET_BULGE_RADIUS},
    [IM_M_MERGER_SUN] = {"M_merger_sun", STAGE_GET_BULGE_RADIUS},
    [IM_LOG_R_MERGER_KPC] = {"log_R_merger_kpc", STAGE_GET_BULGE_RADIUS},
    [IM_R_MERGER] = {"R_merger", STAGE_GET_BULGE_RADIUS},
    [IM_R_DISC] = {"R_disc", STAGE_GET_BULGE_RADIUS},
    [IM_M_INST_SUN] = {"M_inst_sun", STAGE_GET_BULGE_RADIUS},
    [IM_LOG_R_INST_KPC] = {"log_R_inst_kpc", STAGE_GET_BULGE_RADIUS},
    [IM_R_INSTABILITY] = {"R_instability", STAGE_GET_BULGE_RADIUS},
    [IM_R_BULGE] = {"R_bulge", STAGE_GET_BULGE_RADIUS},
    [IM_M_OLD] = {"M_old", STAGE_UPDATE_INSTABILITY},
    [IM_R_OLD] = {"R_old", STAGE_UPDATE_INSTABILITY},
    [IM_R_DISC_KPC] = {"R_disc_kpc", STAGE_UPDATE_INSTABILITY},
    [IM_R_NEW_CONTRIBUTION_KPC] = {"R_new_contribution_kpc", STAGE_UPDATE_INSTABILITY},
    [IM_R_NEW_CONTRIBUTION] = {"R_new_contribution", STAGE_UPDATE_INSTABILITY},
    [IM_R_OLD_KPC] = {"R_old_kpc", STAGE_UPDATE_INSTABILITY},
    [IM_M_NEW] = {"M_new", STAGE_UPDATE_INSTABILITY},
    [IM_R_NEW_KPC] = {"R_new_kpc", STAGE_UPDATE_INSTABILITY},
    [IM_R_NEW] = {"R_new", STAGE_UPDATE_INSTABILITY},
};

/** Branch flags: -1 not evaluated on this path, otherwise the branch taken */
enum flag_id {
  FL_TOTAL_NONPOSITIVE,
  FL_MERGER_MASS_ZERO,
  FL_INSTABILITY_MASS_ZERO,
  FL_MERGER_FALLBACK,
  FL_INSTABILITY_FALLBACK,
  FL_RETURNED_EARLY,
  FL_INCREMENTAL,
  FL_COUNT
};

static const char *const FLAG_NAMES[FL_COUNT] = {
    [FL_TOTAL_NONPOSITIVE] = "total_nonpositive",
    [FL_MERGER_MASS_ZERO] = "merger_mass_zero",
    [FL_INSTABILITY_MASS_ZERO] = "instability_mass_zero",
    [FL_MERGER_FALLBACK] = "merger_fallback",
    [FL_INSTABILITY_FALLBACK] = "instability_fallback",
    [FL_RETURNED_EARLY] = "returned_early",
    [FL_INCREMENTAL] = "incremental",
};

/** The locals and branch flags of one composition */
struct locals {
  double value[IM_COUNT];
  int present[IM_COUNT];
  int flag[FL_COUNT];
};

static void init_locals(struct locals *l) {
  memset(l, 0, sizeof(*l));
  for (int f = 0; f < FL_COUNT; f++) {
    l->flag[f] = -1;
  }
}

/** A replacement of one local by a neighbour */
struct perturbation {
  int im; /* enum im_id to perturb, or -1 */
  int up; /* 1: toward +inf; 0: toward -inf */
};

static const struct perturbation NO_PERTURBATION = {-1, 0};

struct context {
  struct locals *l;
  const struct perturbation *pt;
  int transcribe_get_bulge_radius; /* the nested get_bulge_radius (X:423) transcribed */
};

/** @brief Record a local, then return it, or its neighbour when it is perturbed */
static double tap(struct context *ctx, const enum im_id id, const double v) {
  ctx->l->value[id] = v;
  ctx->l->present[id] = 1;
  if (ctx->pt->im == (int)id) {
    return nextafter(v, ctx->pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

// ============================================================================
// TRANSCRIPTIONS OF SAGE26 ROUTINES (checked against SAGE26's own end state)
// ============================================================================

/**
 * @brief TRANSCRIPTION of get_bulge_radius, X:239-377, for BulgeSizeOn 0 and 3
 *
 * Modes 1 and 2 (X:252-315) are not ported, and the reader refuses them. The locals are
 * recorded as the routine forms them; R_merger and R_instability are the radii the weighted
 * average uses (X:366), after their fallbacks.
 */
static double transcribe_get_bulge_radius(struct context *ctx, const int p) {
  struct locals *l = ctx->l;

  /* X:242-247 */
  if (run_params.BulgeSizeOn == 0) {
    galaxies[p].BulgeRadius = 0.0;
    galaxies[p].MergerBulgeRadius = 0.0;
    galaxies[p].InstabilityBulgeRadius = 0.0;
    return 0.0;
  }

  /* X:249 */
  const double h = run_params.Hubble_h;

  /* X:252-315: modes 1 and 2 are refused by the reader */
  if (run_params.BulgeSizeOn != 3) {
    fatal("transcribe_get_bulge_radius covers BulgeSizeOn 0 and 3 only");
  }

  /* X:318-320 */
  double M_merger = galaxies[p].MergerBulgeMass;
  M_merger = tap(ctx, IM_M_MERGER, M_merger);
  double M_instability = galaxies[p].InstabilityBulgeMass;
  M_instability = tap(ctx, IM_M_INSTABILITY, M_instability);
  double M_total = M_merger + M_instability;
  M_total = tap(ctx, IM_M_TOTAL, M_total);

  /* X:322-327 */
  l->flag[FL_TOTAL_NONPOSITIVE] = 0;
  if (M_total <= 0.0) {
    l->flag[FL_TOTAL_NONPOSITIVE] = 1;
    galaxies[p].BulgeRadius = 0.0;
    galaxies[p].MergerBulgeRadius = 0.0;
    galaxies[p].InstabilityBulgeRadius = 0.0;
    return 0.0;
  }

  /* X:332-337 */
  l->flag[FL_MERGER_MASS_ZERO] = 0;
  if (M_merger == 0.0) {
    l->flag[FL_MERGER_MASS_ZERO] = 1;
    galaxies[p].MergerBulgeRadius = 0.0;
  }
  l->flag[FL_INSTABILITY_MASS_ZERO] = 0;
  if (M_instability == 0.0) {
    l->flag[FL_INSTABILITY_MASS_ZERO] = 1;
    galaxies[p].InstabilityBulgeRadius = 0.0;
  }

  /* X:342-348 */
  double R_merger = galaxies[p].MergerBulgeRadius;
  l->flag[FL_MERGER_FALLBACK] = 0;
  if (M_merger > 0.0 && R_merger <= 0.0) {
    l->flag[FL_MERGER_FALLBACK] = 1;
    double M_merger_sun = CODE_MASS_TO_MSUN(M_merger, h);
    M_merger_sun = tap(ctx, IM_M_MERGER_SUN, M_merger_sun);
    double log_R_kpc = SHEN03_SLOPE_HIGH * log10(M_merger_sun) + SHEN03_INTERCEPT_HIGH;
    log_R_kpc = tap(ctx, IM_LOG_R_MERGER_KPC, log_R_kpc);
    R_merger = pow(10.0, log_R_kpc) * 1.0e-3 * h;
    galaxies[p].MergerBulgeRadius = R_merger;
  }
  R_merger = tap(ctx, IM_R_MERGER, R_merger);

  /* X:351-363 */
  double R_instability = galaxies[p].InstabilityBulgeRadius;
  l->flag[FL_INSTABILITY_FALLBACK] = 0;
  if (M_instability > 0.0 && R_instability <= 0.0) {
    double R_disc = galaxies[p].DiskScaleRadius;
    R_disc = tap(ctx, IM_R_DISC, R_disc);
    if (R_disc > 0.0) {
      l->flag[FL_INSTABILITY_FALLBACK] = 1;
      R_instability = TONINI16_DISK_FRAC * R_disc;
    } else {
      l->flag[FL_INSTABILITY_FALLBACK] = 2;
      double M_inst_sun = CODE_MASS_TO_MSUN(M_instability, h);
      M_inst_sun = tap(ctx, IM_M_INST_SUN, M_inst_sun);
      double log_R_kpc = SHEN03_SLOPE_HIGH * log10(M_inst_sun) + SHEN03_INTERCEPT_HIGH;
      log_R_kpc = tap(ctx, IM_LOG_R_INST_KPC, log_R_kpc);
      R_instability = pow(10.0, log_R_kpc) * 1.0e-3 * h;
    }
    galaxies[p].InstabilityBulgeRadius = R_instability;
  }
  R_instability = tap(ctx, IM_R_INSTABILITY, R_instability);

  /* X:366-369 */
  double R_bulge = (M_merger * R_merger + M_instability * R_instability) / M_total;
  R_bulge = tap(ctx, IM_R_BULGE, R_bulge);

  galaxies[p].BulgeRadius = R_bulge;
  return R_bulge;
}

/** @brief SAGE26's own or the transcribed get_bulge_radius, per the composition */
static double call_get_bulge_radius(struct context *ctx, const int p) {
  if (ctx->transcribe_get_bulge_radius) {
    return transcribe_get_bulge_radius(ctx, p);
  }
  return get_bulge_radius(p, galaxies, &run_params);
}

/**
 * @brief TRANSCRIPTION of update_instability_bulge_radius, X:385-424, for BulgeSizeOn 0 and 3
 *
 * Its closing get_bulge_radius (X:423) is SAGE26's own or the transcription above, per the
 * composition.
 */
static void transcribe_update_instability_bulge_radius(struct context *ctx, const int p,
                                                       const double delta_mass,
                                                       const double old_disk_radius) {
  struct locals *l = ctx->l;

  /* X:395-396 */
  if (run_params.BulgeSizeOn != 3) {
    l->flag[FL_RETURNED_EARLY] = 1;
    return;
  }
  if (delta_mass <= 0.0) {
    l->flag[FL_RETURNED_EARLY] = 2;
    return;
  }
  l->flag[FL_RETURNED_EARLY] = 0;

  /* X:398-400 */
  const double h = run_params.Hubble_h;
  double M_old = galaxies[p].InstabilityBulgeMass - delta_mass;
  M_old = tap(ctx, IM_M_OLD, M_old);
  double R_old = galaxies[p].InstabilityBulgeRadius;
  R_old = tap(ctx, IM_R_OLD, R_old);

  /* X:404-408 */
  double R_disc_kpc = old_disk_radius * 1.0e3 / h;
  R_disc_kpc = tap(ctx, IM_R_DISC_KPC, R_disc_kpc);
  double R_new_contribution_kpc = TONINI16_DISK_FRAC * R_disc_kpc;
  R_new_contribution_kpc = tap(ctx, IM_R_NEW_CONTRIBUTION_KPC, R_new_contribution_kpc);
  double R_new_contribution = R_new_contribution_kpc * 1.0e-3 * h;
  R_new_contribution = tap(ctx, IM_R_NEW_CONTRIBUTION, R_new_contribution);

  /* X:410-420 */
  double R_new;
  l->flag[FL_INCREMENTAL] = 0;
  if (M_old > 0.0 && R_old > 0.0) {
    l->flag[FL_INCREMENTAL] = 1;
    double R_old_kpc = R_old * 1.0e3 / h;
    R_old_kpc = tap(ctx, IM_R_OLD_KPC, R_old_kpc);
    double M_new = galaxies[p].InstabilityBulgeMass;
    M_new = tap(ctx, IM_M_NEW, M_new);
    double R_new_kpc = (R_old_kpc * M_old + R_new_contribution_kpc * delta_mass) / M_new;
    R_new_kpc = tap(ctx, IM_R_NEW_KPC, R_new_kpc);
    R_new = R_new_kpc * 1.0e-3 * h;
  } else {
    R_new = R_new_contribution;
  }
  R_new = tap(ctx, IM_R_NEW, R_new);

  /* X:422-423 */
  galaxies[p].InstabilityBulgeRadius = R_new;
  (void)call_get_bulge_radius(ctx, p);
}

// ============================================================================
// CHECKED COMPOSITIONS
// ============================================================================

enum checked_routine { CR_GET_BULGE_RADIUS, CR_UPDATE_INSTABILITY };

struct call_args {
  enum checked_routine routine;
  int p;
  double delta_mass;
  double old_disk_radius;
};

/** @brief SAGE26's own routine on the current galaxy; returns its double result (or 0) */
static double run_sage26(const struct call_args *a) {
  if (a->routine == CR_GET_BULGE_RADIUS) {
    return get_bulge_radius(a->p, galaxies, &run_params);
  }
  update_instability_bulge_radius(a->p, a->delta_mass, a->old_disk_radius, galaxies, &run_params);
  return 0.0;
}

/**
 * @brief The routine's transcription on a copy of the input, its nested get_bulge_radius
 *        SAGE26's own or transcribed; returns its double result (or 0)
 */
static double compose(const struct GALAXY *input, const struct call_args *a,
                      const int transcribe_nested, const struct perturbation *pt, struct locals *l,
                      struct GALAXY *result) {
  memcpy(galaxies, input, sizeof(galaxies));
  init_locals(l);
  struct context ctx = {l, pt, transcribe_nested};
  double value = 0.0;
  if (a->routine == CR_GET_BULGE_RADIUS) {
    value = transcribe_get_bulge_radius(&ctx, a->p);
  } else {
    transcribe_update_instability_bulge_radius(&ctx, a->p, a->delta_mass, a->old_disk_radius);
  }
  memcpy(result, galaxies, sizeof(galaxies));
  return value;
}

struct sensitivity {
  int up;
  int down;
};

struct checked_record {
  struct GALAXY end[MAX_GALAXIES];
  double end_value;
  struct locals locals;
  struct sensitivity sens[IM_COUNT];
};

static int composition_matches(const struct GALAXY *result, const double value,
                               const struct checked_record *rec) {
  return same_size_block(&result[0], &rec->end[0]) && same_f64(value, rec->end_value);
}

/**
 * @brief Call SAGE26's routine, check every composition used, validate every local
 *
 * The oracle refuses the fixture unless the fully transcribed composition and the composition with
 * SAGE26's own nested get_bulge_radius each reproduce SAGE26's end state in every recorded field
 * and its return value bit for bit. The recorded locals come from the fully transcribed
 * composition. A local of the selected routine is validated in the composition that transcribes
 * that routine and keeps SAGE26's own nested get_bulge_radius (the rule of README.md's
 * "The c06 Group": the value's routine transcribed, every other one SAGE26's own).
 */
static void run_checked(const struct GALAXY *input, const struct call_args *a,
                        struct checked_record *rec) {
  memcpy(galaxies, input, sizeof(galaxies));
  rec->end_value = run_sage26(a);
  memcpy(rec->end, galaxies, sizeof(galaxies));

  struct GALAXY result[MAX_GALAXIES];
  double value = compose(input, a, 1, &NO_PERTURBATION, &rec->locals, result);
  if (!composition_matches(result, value, rec)) {
    fatal("transcription check failed: the fully transcribed composition does not reproduce "
          "SAGE26's %s bit for bit",
          routine_name);
  }
  struct locals own;
  value = compose(input, a, 0, &NO_PERTURBATION, &own, result);
  if (!composition_matches(result, value, rec)) {
    fatal("transcription check failed: the composition with SAGE26's own get_bulge_radius does "
          "not reproduce SAGE26's %s bit for bit",
          routine_name);
  }

  const enum stage stage =
      a->routine == CR_GET_BULGE_RADIUS ? STAGE_GET_BULGE_RADIUS : STAGE_UPDATE_INSTABILITY;
  for (int id = 0; id < IM_COUNT; id++) {
    rec->sens[id].up = 0;
    rec->sens[id].down = 0;
    if (IM_INFO[id].stage != stage || !own.present[id]) {
      continue;
    }
    for (int up = 0; up <= 1; up++) {
      const struct perturbation pt = {id, up};
      struct locals lp;
      value = compose(input, a, 0, &pt, &lp, result);
      const int changed = !composition_matches(result, value, rec);
      if (up) {
        rec->sens[id].up = changed;
      } else {
        rec->sens[id].down = changed;
      }
    }
  }
}

/**
 * @brief Whether a local's only observable path is one double-to-float narrowing
 *
 * Every local of update_instability_bulge_radius reaches the state only through R_new's float
 * store into InstabilityBulgeRadius (X:422); the closing get_bulge_radius reads that float, and
 * its double return is discarded (X:423). A single narrowing cannot make both neighbouring
 * doubles of a value differ from the stored float, so these locals cannot be validated
 * two-sided (README). The locals of the directly called get_bulge_radius reach its double
 * return, which the fixture records.
 */
static int narrowing_limited(const int id) { return IM_INFO[id].stage == STAGE_UPDATE_INSTABILITY; }

/** @brief Write one local: absent marker, value, check, validation and narrowing markers */
static void out_carried(const struct checked_record *rec, const int id) {
  const char *name = IM_INFO[id].name;
  const int present = rec->locals.present[id];
  out_i32(field_name("intermediate.%s_absent", name), present ? 0 : 1);
  if (!present) {
    return;
  }
  out_f64(field_name("intermediate.%s", name), rec->locals.value[id]);
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), CHECK_TAIL);
  out_i32(field_name("intermediate.%s_validated_up", name), rec->sens[id].up);
  out_i32(field_name("intermediate.%s_validated_down", name), rec->sens[id].down);
  out_i32(field_name("intermediate.%s_validated", name), rec->sens[id].up && rec->sens[id].down);
  out_i32(field_name("intermediate.%s_narrowing_limited", name), narrowing_limited(id));
}

static void out_flags(const struct locals *l, const int first, const int last) {
  for (int f = first; f <= last; f++) {
    out_i32(field_name("intermediate.%s", FLAG_NAMES[f]), l->flag[f]);
  }
}

// ============================================================================
// ROUTINES
// ============================================================================

/** @brief get_bulge_radius (X:239-377), SAGE26's own, its locals from the checked transcription */
static void run_get_bulge_radius(void) {
  read_bulge_size_config();
  if (in_i32("call.ngal") != 1) {
    fatal("call.ngal must be 1");
  }
  const int p = read_call_p();
  read_size_block(p);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));
  const struct call_args a = {CR_GET_BULGE_RADIUS, p, 0.0, 0.0};
  static struct checked_record rec;
  run_checked(input, &a, &rec);

  out_f64("return.R_bulge", rec.end_value);
  write_size_block(p, &rec.end[p]);
  out_flags(&rec.locals, FL_TOTAL_NONPOSITIVE, FL_INSTABILITY_FALLBACK);
  for (int id = IM_M_MERGER; id <= IM_R_BULGE; id++) {
    out_carried(&rec, id);
  }
}

/**
 * @brief update_instability_bulge_radius (X:385-424), SAGE26's own, its locals from the
 *        checked transcription
 *
 * The input is the state SAGE26's callers hand it: InstabilityBulgeMass ALREADY holds the
 * transfer (the callers store it first, R/src/model_disk_instability.c:78-83,
 * R/src/model_mergers.c:518-522, :779-784).
 */
static void run_update_instability_bulge_radius(void) {
  read_bulge_size_config();
  if (in_i32("call.ngal") != 1) {
    fatal("call.ngal must be 1");
  }
  const int p = read_call_p();
  const double delta_mass = in_f64("call.delta_mass");
  const double old_disk_radius = in_f64("call.old_disk_radius");
  read_size_block(p);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));
  const struct call_args a = {CR_UPDATE_INSTABILITY, p, delta_mass, old_disk_radius};
  static struct checked_record rec;
  run_checked(input, &a, &rec);

  write_size_block(p, &rec.end[p]);
  out_flags(&rec.locals, FL_RETURNED_EARLY, FL_INCREMENTAL);
  for (int id = IM_M_OLD; id <= IM_R_NEW; id++) {
    out_carried(&rec, id);
  }
}

/**
 * @brief SOURCE-REVIEWED TRANSCRIPTION of the FoF central's disk radius and bulge-radius
 *        synchronisation during progenitor joining, B:297-298
 *
 *   galaxies[ngal].DiskScaleRadius = get_disk_radius(halonr, ngal, halos, galaxies);   B:297
 *   get_bulge_radius(ngal, galaxies, run_params);                                      B:298
 *
 * The double result of get_disk_radius is stored into the float DiskScaleRadius, which
 * get_bulge_radius then reads (its instability fallback, X:353); get_bulge_radius's return is
 * discarded. The rest of the central branch (B:285-295, B:300: mergeType, mergeIntoID, the clock,
 * the concentration with ConcentrationOn 0, Type) is not transcribed here; oracle_c06.c's
 * join_progenitors transcribes it. get_disk_radius and get_bulge_radius are SAGE26's own, or
 * get_bulge_radius the checked transcription above.
 *
 * @return get_disk_radius's double result
 */
static double transcribe_central_disk_and_bulge_radius(const int halonr, const int ngal,
                                                       const int transcribe_nested) {
  /* B:297 */
  const double r_disk = get_disk_radius(halonr, ngal, halos, galaxies);
  galaxies[ngal].DiskScaleRadius = r_disk;

  /* B:298 */
  struct locals l;
  init_locals(&l);
  struct context ctx = {&l, &NO_PERTURBATION, transcribe_nested};
  (void)call_get_bulge_radius(&ctx, ngal);
  return r_disk;
}

/**
 * @brief The central's synchronisation (B:297-298), a source-reviewed transcription composed
 *        with SAGE26's own get_disk_radius and get_bulge_radius
 *
 * Reads the galaxy's float Vvir and Rvir and the halo's float Spin (get_disk_radius,
 * X:203-229), its Type (the Mimic side: the package synchronises the FoF Type 0 only, which
 * the oracle requires) and the size block; the input DiskScaleRadius is the stale value the
 * store overwrites. The composition with SAGE26's own get_bulge_radius gives the recorded
 * state; the one with the transcribed get_bulge_radius must reproduce it bit for bit. Neither
 * shows anything about B:297-298 itself, which the mutation sweep covers. No local is
 * carried: B:297-298 form none.
 */
static void run_central_disk_and_bulge_radius(void) {
  read_bulge_size_config();
  if (in_i32("call.ngal") != 1) {
    fatal("call.ngal must be 1");
  }
  const int p = read_call_p();
  struct GALAXY *g = &galaxies[p];
  g->Type = in_i32(galaxy_field(p, "Type"));
  if (g->Type != 0) {
    fatal("galaxy[0].Type = %d, but the synchronisation is the FoF central's (Type 0)", g->Type);
  }
  g->Vvir = in_f32(galaxy_field(p, "Vvir"));
  g->Rvir = in_f32(galaxy_field(p, "Rvir"));
  for (int j = 0; j < 3; j++) {
    halos[0].Spin[j] = in_f32(field_name("halo.Spin[%d]", j));
  }
  read_size_block(p);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  const double r_disk = transcribe_central_disk_and_bulge_radius(0, p, 0);
  struct GALAXY end[MAX_GALAXIES];
  memcpy(end, galaxies, sizeof(end));

  memcpy(galaxies, input, sizeof(galaxies));
  const double r_disk_transcribed = transcribe_central_disk_and_bulge_radius(0, p, 1);
  if (!same_size_block(&galaxies[p], &end[p]) || !same_f64(r_disk, r_disk_transcribed)) {
    fatal("transcription check failed: the synchronisation with the transcribed "
          "get_bulge_radius does not reproduce the one with SAGE26's own bit for bit");
  }

  out_f64("return.r_disk", r_disk);
  write_size_block(p, &end[p]);
}

// ============================================================================
// MAIN
// ============================================================================

struct routine_entry {
  const char *name;
  void (*run)(void);
};

static const struct routine_entry routines[] = {
    {"get_bulge_radius", run_get_bulge_radius},
    {"update_instability_bulge_radius", run_update_instability_bulge_radius},
    {"central_disk_and_bulge_radius", run_central_disk_and_bulge_radius},
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
          "# SAGE26 oracle output for %s, written by oracle_c07 (regenerate.sh); do not edit.\n",
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
