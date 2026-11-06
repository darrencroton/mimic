/**
 * @file    oracle_c12.c
 * @brief   SAGE26 function-level oracle for the c12 halo concentration
 *          (concentration_from_vmax_vvir and get_halo_concentration, ConcentrationOn 0 and 3)
 *
 * Reads one fixture input file, calls SAGE26's public concentration_from_vmax_vvir or
 * get_halo_concentration (R/src/model_halo_properties.c:288-319 and :327-362, declared at
 * R/src/model_halo_properties.h:25-27; R/ is the staged SAGE26 source root) from the SAGE26
 * objects this executable is linked against, once per case of the fixture, and writes a fixture
 * output file holding each call's double return, the float store SAGE26's joining makes of it
 * (Concentration = (float)get_halo_concentration(...), R/src/core_build_model.c:292-294), and the
 * routine's carried locals.
 *
 * Usage: oracle_c12 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one):
 *   concentration_from_vmax_vvir   R/src/model_halo_properties.c:288-319, called directly with
 *                                  each case's double Vmax and Vvir
 *   get_halo_concentration         R/src/model_halo_properties.c:327-362, called directly with
 *                                  params.ConcentrationOn 0 or 3, the fixture's redshift and
 *                                  each case's galaxy (its Type at the call and its four float
 *                                  velocities) at its own index of the galaxy array
 *
 * The locals (ratio_sq, the final bracket c_lo and c_hi, and the mode-3 selected pair) cannot be
 * read from SAGE26's call, so they come from SOURCE-REVIEWED TRANSCRIPTIONS below, each
 * statement cited to its line. The oracle refuses a fixture unless, for every case:
 *
 * - the full transcription reproduces SAGE26's return value bit for bit;
 * - each recorded local reproduces it from where it is formed: ratio_sq through the transcribed
 *   bisection, c_lo and c_hi through the transcribed return statement (TAIL-checked, marker 2),
 *   and the selected pair through SAGE26's own concentration_from_vmax_vvir
 *   (CONTINUATION-checked, marker 1);
 * - SAGE26's call changed no byte of the galaxy array (get_halo_concentration reads it as const).
 *
 * Two-sided validation (campaign decision D25) replaces each recorded local, in its composition,
 * by its upward and, separately, its downward neighbouring double, and records whether each
 * replacement changes the double return or the float store; `_validated` only if both do. It
 * also records whether each replacement changes the float store alone: a value validated
 * two-sided whose neighbours both leave the float store unchanged is narrowing-limited (only the
 * double return, which SAGE26's joining narrows at once, shows it).
 *
 * The transcriptions restate SAGE26's statements with the same expression shapes, operand order
 * and types and are compiled with the same flags. None has a multiply feeding an add or subtract,
 * so contraction fuses none, here or in SAGE26's object (README.md, "The c12 Group").
 *
 * The file format is defined in README.md ("Fixture File Format"). The parsing and output
 * helpers are copies of oracle_c10.c's, which stays unchanged.
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
#include "macros.h"
#include "model_halo_properties.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 1024
#define MAX_GALAXIES 24
#define NAME_LEN 96
#define LINE_LEN 512

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
  fprintf(stderr, "oracle_c12: %s: ", input_path != NULL ? input_path : "(no input)");
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

static void out_f64(const char *name, const double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  fprintf(output_file, "f64 %s 0x%016" PRIx64 "  # %.17g\n", name, bits, value);
}

static void out_i32(const char *name, const int value) {
  fprintf(output_file, "i32 %s %d\n", name, value);
}

static int same_f64(const double a, const double b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static void out_f32(const char *name, const float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  fprintf(output_file, "f32 %s 0x%08" PRIx32 "  # %.9g\n", name, bits, (double)value);
}

static int same_f32(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

// ============================================================================
// SOURCE-REVIEWED TRANSCRIPTION OF SAGE26 SOURCE (model_halo_properties.c)
// ============================================================================

/** The check kinds of a recorded local (README.md, "The c06 Group") */
#define CHECK_CONTINUATION 1
#define CHECK_TAIL 2

/** One concentration_from_vmax_vvir call's record from the transcription */
struct inversion_trace {
  int guard_returned;  /* :294 returned 0 */
  int below_one;       /* :306 returned 0; -1 when not evaluated */
  int iterations;      /* bisection steps taken (:308) */
  int ratio_sq_absent; /* 1 when :296 was not reached */
  double ratio_sq;     /* :296 */
  int bracket_absent;  /* 1 when the bisection did not run */
  double c_lo, c_hi;   /* the bracket after the last step (:313, :315) */
  int ties;            /* steps whose f_mid equalled ratio_sq, converged repeats included (an
                          observation, not SAGE26 state) */
  int first_tie_step;  /* the first such step, -1 for none */
};

static void reset_inversion_trace(struct inversion_trace *t) {
  t->guard_returned = 0;
  t->below_one = -1;
  t->iterations = 0;
  t->ratio_sq_absent = 1;
  t->ratio_sq = NAN;
  t->bracket_absent = 1;
  t->c_lo = NAN;
  t->c_hi = NAN;
  t->ties = 0;
  t->first_tie_step = -1;
}

/** @brief TRANSCRIPTION of the return statement, R/src/model_halo_properties.c:318 */
static double tr_return(const double c_lo, const double c_hi) {
  /* :318 */
  return 0.5 * (c_lo + c_hi);
}

/**
 * @brief TRANSCRIPTION of concentration_from_vmax_vvir from ratio_sq on,
 *        R/src/model_halo_properties.c:301-318
 */
static double tr_from_ratio_sq(const double ratio_sq, struct inversion_trace *t) {
  /* :301 */
  const double A = 0.21621;

  /* :303 */
  double c_lo = 1.0, c_hi = 200.0;

  /* :306 */
  if (ratio_sq < 1.0) {
    t->below_one = 1;
    return 0.0;
  }
  t->below_one = 0;

  /* :308 */
  for (int iter = 0; iter < 60; iter++) {
    /* :309 */
    const double c_mid = 0.5 * (c_lo + c_hi);
    /* :310 */
    const double mu = log(1.0 + c_mid) - c_mid / (1.0 + c_mid);
    /* :311 */
    const double f_mid = A * c_mid / mu;
    if (f_mid == ratio_sq) {
      if (t->ties++ == 0) {
        t->first_tie_step = iter;
      }
    }
    /* :312-316 */
    if (f_mid < ratio_sq) {
      c_lo = c_mid;
    } else {
      c_hi = c_mid;
    }
    t->iterations++;
  }
  t->bracket_absent = 0;
  t->c_lo = c_lo;
  t->c_hi = c_hi;

  /* :318 */
  return tr_return(c_lo, c_hi);
}

/** @brief TRANSCRIPTION of concentration_from_vmax_vvir, R/src/model_halo_properties.c:288-319 */
static double tr_concentration_from_vmax_vvir(const double Vmax, const double Vvir,
                                              struct inversion_trace *t) {
  reset_inversion_trace(t);

  /* :294 */
  if (Vvir <= 0.0 || Vmax <= 0.0) {
    t->guard_returned = 1;
    return 0.0;
  }

  /* :296 */
  const double ratio_sq = (Vmax / Vvir) * (Vmax / Vvir);
  t->ratio_sq_absent = 0;
  t->ratio_sq = ratio_sq;

  /* :301-318 */
  return tr_from_ratio_sq(ratio_sq, t);
}

/** One get_halo_concentration call's record from the transcription */
struct dispatch_trace {
  int selected_infall; /* -1 mode 0, 0 the current pair, 1 the infall pair (:342) */
  int pair_absent;     /* 1 when no pair is selected (mode 0) */
  double vmax, vvir;   /* the selected pair, each float widened (:343-347) */
  struct inversion_trace inv;
};

/**
 * @brief TRANSCRIPTION of get_halo_concentration for ConcentrationOn 0 and 3,
 *        R/src/model_halo_properties.c:327-350
 *
 * Modes 1 and 2 (:334-337, :352-362) are not transcribed; the reader refuses them.
 */
static double tr_get_halo_concentration(const int p, const double z, const struct GALAXY *g,
                                        const struct params *rp, struct dispatch_trace *t) {
  (void)z; /* read by neither mode */
  t->selected_infall = -1;
  t->pair_absent = 1;
  t->vmax = NAN;
  t->vvir = NAN;
  reset_inversion_trace(&t->inv);

  /* :330-332 */
  if (rp->ConcentrationOn == 0) {
    return 0.0;
  }

  /* :339 */
  if (rp->ConcentrationOn == 3) {
    /* :341 */
    double vmax, vvir;
    /* :342 */
    if (g[p].Type == 0) {
      /* :343-344 */
      vmax = g[p].Vmax;
      vvir = g[p].Vvir;
      t->selected_infall = 0;
    } else {
      /* :346-347 */
      vmax = g[p].infallVmax;
      vvir = g[p].infallVvir;
      t->selected_infall = 1;
    }
    t->pair_absent = 0;
    t->vmax = vmax;
    t->vvir = vvir;
    /* :349 */
    return tr_concentration_from_vmax_vvir(vmax, vvir, &t->inv);
  }

  fatal("ConcentrationOn %d is not transcribed", rp->ConcentrationOn);
}

// ============================================================================
// THE RECORD: CHECKED LOCALS AND TWO-SIDED VALIDATION
// ============================================================================

/** @brief Whether a candidate (return, store) differs from the recorded one */
static int outputs_differ(const double ret, const double ret_ref) {
  return !same_f64(ret, ret_ref) || !same_f32((float)ret, (float)ret_ref);
}

static int store_differs(const double ret, const double ret_ref) {
  return !same_f32((float)ret, (float)ret_ref);
}

/** The composition a recorded local is validated in */
enum local_kind { LOCAL_RATIO_SQ, LOCAL_C_LO, LOCAL_C_HI, LOCAL_VMAX, LOCAL_VVIR };

/** @brief Re-run a local's composition with the local replaced by `value` */
static double compose_with(const enum local_kind kind, const double value,
                           const struct inversion_trace *t, const double vmax, const double vvir) {
  struct inversion_trace scratch;
  reset_inversion_trace(&scratch);
  switch (kind) {
  case LOCAL_RATIO_SQ:
    return tr_from_ratio_sq(value, &scratch);
  case LOCAL_C_LO:
    return tr_return(value, t->c_hi);
  case LOCAL_C_HI:
    return tr_return(t->c_lo, value);
  case LOCAL_VMAX:
    /* the continuation is SAGE26's own routine (:349) */
    return concentration_from_vmax_vvir(value, vvir);
  case LOCAL_VVIR:
    return concentration_from_vmax_vvir(vmax, value);
  }
  fatal("unknown local kind (oracle bug)");
}

/**
 * @brief Write one recorded local with its markers (README.md, "The c12 Group")
 *
 * Refuses the fixture unless the local's composition, run from the recorded value, reproduces
 * SAGE26's return bit for bit.
 */
static void record_local(const char *prefix, const char *name, const enum local_kind kind,
                         const int check_kind, const int absent, const double value,
                         const double sage26_return, const struct inversion_trace *t,
                         const double vmax, const double vvir) {
  out_i32(field_name("%s.%s_absent", prefix, name), absent);
  if (absent) {
    return;
  }
  const double reproduced = compose_with(kind, value, t, vmax, vvir);
  if (!same_f64(reproduced, sage26_return)) {
    fatal("%s.%s: its composition gives %.17g, SAGE26 returned %.17g", prefix, name, reproduced,
          sage26_return);
  }
  const double up = compose_with(kind, nextafter(value, INFINITY), t, vmax, vvir);
  const double down = compose_with(kind, nextafter(value, -INFINITY), t, vmax, vvir);
  const int validated_up = outputs_differ(up, sage26_return);
  const int validated_down = outputs_differ(down, sage26_return);
  const int store_up = store_differs(up, sage26_return);
  const int store_down = store_differs(down, sage26_return);
  const int validated = validated_up && validated_down;

  out_f64(field_name("%s.%s", prefix, name), value);
  out_i32(field_name("%s.%s_checked", prefix, name), 1);
  out_i32(field_name("%s.%s_check_kind", prefix, name), check_kind);
  out_i32(field_name("%s.%s_validated_up", prefix, name), validated_up);
  out_i32(field_name("%s.%s_validated_down", prefix, name), validated_down);
  out_i32(field_name("%s.%s_validated", prefix, name), validated);
  out_i32(field_name("%s.%s_store_changes_up", prefix, name), store_up);
  out_i32(field_name("%s.%s_store_changes_down", prefix, name), store_down);
  out_i32(field_name("%s.%s_narrowing_limited", prefix, name),
          validated && !store_up && !store_down);
}

/** @brief Write an inversion's flags and locals under `prefix` */
static void record_inversion(const char *prefix, const struct inversion_trace *t,
                             const double sage26_return) {
  out_i32(field_name("%s.guard_returned", prefix), t->guard_returned);
  out_i32(field_name("%s.below_one", prefix), t->below_one);
  out_i32(field_name("%s.iterations", prefix), t->iterations);
  record_local(prefix, "ratio_sq", LOCAL_RATIO_SQ, CHECK_TAIL, t->ratio_sq_absent, t->ratio_sq,
               sage26_return, t, NAN, NAN);
  record_local(prefix, "c_lo", LOCAL_C_LO, CHECK_TAIL, t->bracket_absent, t->c_lo, sage26_return, t,
               NAN, NAN);
  record_local(prefix, "c_hi", LOCAL_C_HI, CHECK_TAIL, t->bracket_absent, t->c_hi, sage26_return, t,
               NAN, NAN);
  /* The bracket ends that never moved: the bound itself binds (c_lo 1.0, c_hi 200.0, :303) */
  out_i32(field_name("%s.c_lo_at_initial_bound", prefix),
          t->bracket_absent ? -1 : (t->c_lo == 1.0));
  out_i32(field_name("%s.c_hi_at_initial_bound", prefix),
          t->bracket_absent ? -1 : (t->c_hi == 200.0));
  out_i32(field_name("%s.ties", prefix), t->bracket_absent ? -1 : t->ties);
  out_i32(field_name("%s.first_tie_step", prefix), t->bracket_absent ? -1 : t->first_tie_step);
}

// ============================================================================
// THE ROUTINES
// ============================================================================

/** @brief Read call.ncase, 1..MAX_GALAXIES */
static int read_ncase(void) {
  const int ncase = in_i32("call.ncase");
  if (ncase < 1 || ncase > MAX_GALAXIES) {
    fatal("call.ncase = %d is outside 1..%d", ncase, MAX_GALAXIES);
  }
  return ncase;
}

static void run_concentration_from_vmax_vvir(void) {
  const int ncase = read_ncase();
  double vmax[MAX_GALAXIES], vvir[MAX_GALAXIES];
  for (int k = 0; k < ncase; k++) {
    vmax[k] = in_f64(field_name("case[%d].Vmax", k));
    vvir[k] = in_f64(field_name("case[%d].Vvir", k));
  }
  check_all_fields_used();

  for (int k = 0; k < ncase; k++) {
    /* 1. SAGE26's own call */
    const double sage26_return = concentration_from_vmax_vvir(vmax[k], vvir[k]);

    /* 2. The transcription must reproduce it */
    struct inversion_trace t;
    const double transcribed = tr_concentration_from_vmax_vvir(vmax[k], vvir[k], &t);
    if (!same_f64(transcribed, sage26_return)) {
      fatal("case %d: the transcription returns %.17g, SAGE26 %.17g", k, transcribed,
            sage26_return);
    }

    /* 3. The record */
    out_f64(field_name("return.case[%d]", k), sage26_return);
    out_f32(field_name("output.case[%d].Concentration", k), (float)sage26_return);
    char prefix[NAME_LEN];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    record_inversion(prefix, &t, sage26_return);
  }
}

static void run_get_halo_concentration(void) {
  run_params.ConcentrationOn = in_i32("params.ConcentrationOn");
  if (run_params.ConcentrationOn != 0 && run_params.ConcentrationOn != 3) {
    fatal("params.ConcentrationOn = %d: only modes 0 and 3 are transcribed",
          run_params.ConcentrationOn);
  }
  const double z = in_f64("call.z");
  const int ncase = read_ncase();
  for (int k = 0; k < ncase; k++) {
    galaxies[k].Type = in_i32(field_name("galaxy[%d].Type", k));
    galaxies[k].Vmax = in_f32(field_name("galaxy[%d].Vmax", k));
    galaxies[k].Vvir = in_f32(field_name("galaxy[%d].Vvir", k));
    galaxies[k].infallVmax = in_f32(field_name("galaxy[%d].infallVmax", k));
    galaxies[k].infallVvir = in_f32(field_name("galaxy[%d].infallVvir", k));
  }
  check_all_fields_used();

  static struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* SAGE26's joining makes the float store only under ConcentrationOn > 0 (B:291) */
  const int store_made = run_params.ConcentrationOn > 0;
  out_i32("intermediate.store_made", store_made);

  for (int k = 0; k < ncase; k++) {
    /* 1. SAGE26's own call, with the galaxy at index k (as joining passes ngal, B:292-294) */
    const double sage26_return = get_halo_concentration(k, z, galaxies, &run_params);
    if (memcmp(input, galaxies, sizeof(input)) != 0) {
      fatal("case %d: SAGE26's call changed the galaxy array", k);
    }

    /* 2. The transcription must reproduce it */
    struct dispatch_trace t;
    const double transcribed = tr_get_halo_concentration(k, z, input, &run_params, &t);
    if (!same_f64(transcribed, sage26_return)) {
      fatal("case %d: the transcription returns %.17g, SAGE26 %.17g", k, transcribed,
            sage26_return);
    }

    /* 3. The record */
    out_f64(field_name("return.case[%d]", k), sage26_return);
    if (store_made) {
      out_f32(field_name("output.case[%d].Concentration", k), (float)sage26_return);
    }
    char prefix[NAME_LEN];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    out_i32(field_name("%s.selected_infall", prefix), t.selected_infall);
    record_local(prefix, "vmax", LOCAL_VMAX, CHECK_CONTINUATION, t.pair_absent, t.vmax,
                 sage26_return, &t.inv, t.vmax, t.vvir);
    record_local(prefix, "vvir", LOCAL_VVIR, CHECK_CONTINUATION, t.pair_absent, t.vvir,
                 sage26_return, &t.inv, t.vmax, t.vvir);
    record_inversion(prefix, &t.inv, sage26_return);
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
    {"concentration_from_vmax_vvir", run_concentration_from_vmax_vvir},
    {"get_halo_concentration", run_get_halo_concentration},
};

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash != NULL ? slash + 1 : path;
}

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(&run_params, 0xff, sizeof(run_params));
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
          "# SAGE26 oracle output for %s, written by oracle_c12 (regenerate.sh); do not edit.\n",
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
