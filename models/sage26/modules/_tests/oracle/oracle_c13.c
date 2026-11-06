/**
 * @file    oracle_c13.c
 * @brief   SAGE26 function-level oracle for the c13 feedback-free-burst classifier
 *          (calculate_ffb_threshold_mass, calculate_ffb_fraction and
 *          determine_and_store_ffb_regime, EnhancedStarFormationOn 1)
 *
 * Reads one fixture input file, calls one of SAGE26's public FFB routines
 * (R/src/model_regimes.c:188-341, declared at R/src/model_regimes.h:21-26; R/ is the staged
 * SAGE26 source root) from the SAGE26 objects this executable is linked against, and writes a
 * fixture output file holding SAGE26's return value or written FFBRegime and the routine's
 * carried intermediates.
 *
 * Usage: oracle_c13 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one):
 *   calculate_ffb_threshold_mass    :298-313, called directly once per case with the case's
 *                                   redshift and params.Hubble_h
 *   calculate_ffb_fraction          :321-341, called directly once per case with the case's
 *                                   double Mvir and redshift, params.EnhancedStarFormationOn 1
 *                                   and params.Hubble_h
 *   determine_and_store_ffb_regime  :188-290, called directly with the fixture's galaxies,
 *                                   call.Zcurr and call.snapnum, as SAGE26's driver calls it once
 *                                   per FoF with EnhancedStarFormationOn >= 1
 *                                   (R/src/core_build_model.c:425-427); mode 1 only
 *
 * THE TEST-ONLY rand() PROVIDER. SAGE26's classifier calls rand() once per unmerged row and
 * discards the value (:224), keeping its other draws in place; its useful draw is the static
 * SplitMix64 hash halo_uniform (:151-159) of the row's MostBoundID and the snapshot. This file
 * defines rand() itself, as oracle_c10.c does, so the linker resolves the SAGE26 objects' calls
 * to it instead of to libc's: it returns the fixture's integer queue (rand.queue[k]) in order,
 * refuses a call when the queue is exhausted or not armed, and counts the calls. SAGE26's source
 * is not edited. The provider is linked only into this oracle main (build.sh). Every classifier
 * fixture must consume its whole queue, so the discarded-draw count is exact, and the oracle
 * reruns SAGE26's call with every queue value q replaced by RAND_MAX - q and refuses the fixture
 * unless every written FFBRegime is the same (the discarded values do not reach the result).
 *
 * The routines' locals (z_norm, log_Mvir_ffb_code, the threshold, x, the fraction and the draw's
 * bits and deviate) cannot be read from SAGE26's calls, so they come from SOURCE-REVIEWED
 * TRANSCRIPTIONS below, each statement cited to its line. The oracle refuses a fixture unless
 * the transcription reproduces SAGE26's return (or every row's FFBRegime and draw consumption)
 * bit for bit and each recorded local reproduces it from where it is formed: z_norm,
 * log_Mvir_ffb_code, the threshold, x and the fraction through the transcribed remainder
 * (TAIL-checked, marker 2), and in the classifier the draw through SAGE26's own public
 * calculate_ffb_fraction (CONTINUATION-checked, marker 1: (draw < calculate_ffb_fraction(Mvir,
 * Zcurr)) must give SAGE26's FFBRegime). In the classifier the transcribed fraction and threshold
 * must also equal SAGE26's public calculate_ffb_fraction and calculate_ffb_threshold_mass of the
 * row bit for bit.
 *
 * Two-sided validation (campaign decision D25) replaces each recorded local, in its composition,
 * by its upward and, separately, its downward neighbouring double and records whether each
 * replacement changes the recorded output (the return, or the row's FFBRegime); `_validated` only
 * if both do. In the classifier a one-ulp change of a local changes the integer FFBRegime only
 * where the draw and the fraction are within an ulp, which no MostBoundID can be chosen to give,
 * so the classifier's locals are recorded with their directions but are not expected to be
 * validated; the threshold and fraction fixtures validate them.
 *
 * Two observations per threshold evaluation, which are not SAGE26 state: pow_equals_exp10
 * (whether libm's pow(10.0, v), called through a volatile pointer so clang cannot simplify it,
 * equals __exp10(v) bit for bit at the recorded log_Mvir_ffb_code: the package writes pow, which
 * its -O0 unit-test build calls, while SAGE26's object calls __exp10), and
 * unfused_threshold_differs (whether the log term rounded as a separate product and sum, rather
 * than fused as SAGE26's object computes it, gives a different threshold).
 *
 * The transcriptions restate SAGE26's statements with the same expression shapes, operand order
 * and types and are compiled with the same flags, so clang contracts and simplifies them as it
 * does SAGE26's object: the log term to one fmadd and pow(10.0, x) to a call of __exp10
 * (README.md, "The c13 Group").
 *
 * The file format is defined in README.md ("Fixture File Format"); this main reads and writes
 * the i64 type (c13) for MostBoundID and the draw's 64 bits. The parsing and output helpers are
 * copies of oracle_c12.c's with that type added; oracle_c12.c stays unchanged.
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
#include "model_regimes.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 1024
#define MAX_CASES 32
#define MAX_GALAXIES 32
#define MAX_QUEUE 32
#define NAME_LEN 96
#define LINE_LEN 512

enum field_type { FIELD_F32, FIELD_F64, FIELD_I32, FIELD_I64 };

struct input_field {
  char name[NAME_LEN];
  enum field_type type;
  uint64_t bits;  /* IEEE-754 bit pattern (f32, f64) */
  int64_t ivalue; /* decimal value (i32, i64) */
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
  fprintf(stderr, "oracle_c13: %s: ", input_path != NULL ? input_path : "(no input)");
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

/**
 * @brief Parse a decimal signed 64-bit integer (the i64 type); refuses anything outside the full
 *        int64_t range (strtoll's ERANGE, never a wrap) or not wholly decimal
 */
static int64_t parse_i64(const char *text, const int line) {
  char *end = NULL;
  errno = 0;
  const long long value = strtoll(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    fatal("line %d: '%s' is not a decimal signed 64-bit integer", line, text);
  }
  return (int64_t)value;
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
  } else if (strcmp(type, "i64") == 0) {
    field->type = FIELD_I64;
    field->ivalue = parse_i64(value, line);
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

static long long in_i64(const char *name) { return (long long)take_field(name, FIELD_I64)->ivalue; }

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

/** @brief A 64-bit pattern written as the i64 of the same two's-complement bits */
static void out_bits_i64(const char *name, const uint64_t bits) {
  long long value;
  memcpy(&value, &bits, sizeof(value));
  fprintf(output_file, "i64 %s %lld  # 0x%016" PRIx64 "\n", name, value, bits);
}

// ============================================================================
// THE TEST-ONLY rand() PROVIDER
// ============================================================================

static int rand_queue[MAX_QUEUE];
static int rand_queue_length = 0;
static int rand_next = 0;   /* index of the next queue entry rand() returns */
static int rand_armed = 0;  /* 1 only around the SAGE26 calls */
static int rand_invert = 0; /* 1: return RAND_MAX - q (the queue-independence rerun) */

/**
 * @brief The fixture's queue in place of libc rand(), for SAGE26's objects
 *
 * Linked only into oracle_c13. Refuses a call outside a SAGE26 call or past the queue's end, so
 * a fixture's queue states exactly the draws SAGE26 discards.
 */
int rand(void) {
  if (!rand_armed) {
    fatal("rand() called outside a determine_and_store_ffb_regime call");
  }
  if (rand_next >= rand_queue_length) {
    fatal("rand() called %d times, but the fixture's queue holds %d entries (rand.queue_length)",
          rand_next + 1, rand_queue_length);
  }
  const int q = rand_queue[rand_next++];
  return rand_invert ? RAND_MAX - q : q;
}

// ============================================================================
// SOURCE-REVIEWED TRANSCRIPTION OF SAGE26 SOURCE (model_regimes.c)
// ============================================================================

/** The check kinds of a recorded local (README.md, "The c06 Group") */
#define CHECK_CONTINUATION 1
#define CHECK_TAIL 2

/* :50, the file-scope static of model_regimes.c */
static const double FFB_THRESHOLD_SLOPE = -6.2;

/** One calculate_ffb_threshold_mass evaluation's locals */
struct threshold_trace {
  double z_norm;            /* :309 */
  double log_Mvir_ffb_code; /* :310 */
  double threshold;         /* :312 */
};

/** One calculate_ffb_fraction evaluation's locals */
struct fraction_trace {
  struct threshold_trace thr; /* :330 */
  int guard_returned;         /* :332-334 returned 0 */
  double x;                   /* :338; NaN when the guard returned */
  double f_ffb;               /* :339, or the guard's 0 */
};

/** @brief TRANSCRIPTION of the threshold's return from its log, R/src/model_regimes.c:312 */
static double tr_threshold_from_log(const double log_Mvir_ffb_code) {
  /* :312 */
  return pow(10.0, log_Mvir_ffb_code);
}

/** @brief TRANSCRIPTION of calculate_ffb_threshold_mass from z_norm on, :308-312 */
static double tr_threshold_from_z_norm(const double z_norm, const double hubble_h) {
  /* :308 */
  const double h = hubble_h;
  /* :310 */
  const double log_Mvir_ffb_code = 0.8 + log10(h) + FFB_THRESHOLD_SLOPE * log10(z_norm);
  /* :312 */
  return tr_threshold_from_log(log_Mvir_ffb_code);
}

/** @brief TRANSCRIPTION of calculate_ffb_threshold_mass, R/src/model_regimes.c:298-313 */
static double tr_threshold(const double z, const struct params *rp, struct threshold_trace *t) {
  /* :308 */
  const double h = rp->Hubble_h;
  /* :309 */
  const double z_norm = (1.0 + z) / 10.0;
  /* :310 */
  const double log_Mvir_ffb_code = 0.8 + log10(h) + FFB_THRESHOLD_SLOPE * log10(z_norm);
  t->z_norm = z_norm;
  t->log_Mvir_ffb_code = log_Mvir_ffb_code;
  /* :312 */
  t->threshold = pow(10.0, log_Mvir_ffb_code);
  return t->threshold;
}

/** @brief TRANSCRIPTION of the fraction's return from x, R/src/model_regimes.c:339 */
static double tr_fraction_from_x(const double x) {
  /* :339 */
  const double f_ffb = 1.0 / (1.0 + exp(-x));
  return f_ffb;
}

/** @brief TRANSCRIPTION of calculate_ffb_fraction from the threshold on, :332-341 */
static double tr_fraction_from_threshold(const double Mvir, const double Mvir_ffb,
                                         struct fraction_trace *t) {
  t->guard_returned = 0;
  t->x = NAN;
  /* :332-334 */
  if (Mvir <= 0.0 || Mvir_ffb <= 0.0) {
    t->guard_returned = 1;
    t->f_ffb = 0.0;
    return 0.0;
  }
  /* :337 */
  const double delta_log_M = 0.15;
  /* :338 */
  const double log_ratio = log10(Mvir / Mvir_ffb);
  const double x = log_ratio / delta_log_M;
  t->x = x;
  /* :339, in the form SAGE26's object computes it: exp(-x) as exp(log10(Mvir / Mvir_ffb) /
   * -delta_log_M), the division by the negated width (model_regimes.o 0x4a8, 0x88c), which never
   * forms x. Equal to the source form for every non-NaN x; a negation would flip a NaN's sign, the
   * division carries it (README.md, "The c13 Group") */
  const double f_ffb = 1.0 / (1.0 + exp(log_ratio / -delta_log_M));
  t->f_ffb = f_ffb;
  return f_ffb;
}

/**
 * @brief TRANSCRIPTION of calculate_ffb_fraction, R/src/model_regimes.c:321-341
 *
 * The :326-328 return at EnhancedStarFormationOn 0 is not transcribed; the reader refuses that
 * setting.
 */
static double tr_fraction(const double Mvir, const double z, const struct params *rp,
                          struct fraction_trace *t) {
  if (rp->EnhancedStarFormationOn == 0) {
    fatal("EnhancedStarFormationOn 0 is not transcribed (oracle bug)");
  }
  /* :330 */
  const double Mvir_ffb = tr_threshold(z, rp, &t->thr);
  /* :332-341 */
  return tr_fraction_from_threshold(Mvir, Mvir_ffb, t);
}

/** @brief TRANSCRIPTION of halo_uniform's hash, R/src/model_regimes.c:153-157 */
static uint64_t tr_halo_uniform_bits(const long long most_bound_id, const int snapnum) {
  /* :153 */
  uint64_t x = (uint64_t)most_bound_id ^ ((uint64_t)(uint32_t)snapnum * 0x9E3779B97F4A7C15ULL);
  /* :154 */
  x += 0x9E3779B97F4A7C15ULL;
  /* :155 */
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  /* :156 */
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  /* :157 */
  x ^= x >> 31;
  return x;
}

/** @brief TRANSCRIPTION of halo_uniform's deviate, R/src/model_regimes.c:158 */
static double tr_uniform_from_bits(const uint64_t x) {
  /* :158 */
  return ((double)(x >> 11) + 0.5) * (1.0 / 9007199254740992.0);
}

/** One classifier row's record from the transcription */
struct row_trace {
  int skipped;        /* mergeType > 0 (:213) */
  int ffb_regime;     /* the FFBRegime it writes (:236-240); the input value for a skipped row */
  uint64_t draw_bits; /* :153-157 */
  double draw;        /* :225 */
  double Mvir;        /* :229, the float widened */
  struct fraction_trace frac;
};

/**
 * @brief TRANSCRIPTION of determine_and_store_ffb_regime at EnhancedStarFormationOn 1,
 *        R/src/model_regimes.c:188-240
 *
 * Mode 2 (:202-209, :241-288) is not transcribed; the reader refuses it. The discarded rand()
 * (:224) is not transcribed: its value reaches nothing.
 */
static void tr_determine_and_store_ffb_regime(const int ngal, const double Zcurr, const int snapnum,
                                              const struct GALAXY *g, const struct params *rp,
                                              struct row_trace *tr) {
  /* :212 */
  for (int p = 0; p < ngal; p++) {
    tr[p].skipped = 0;
    tr[p].ffb_regime = g[p].FFBRegime;
    tr[p].draw_bits = 0;
    tr[p].draw = NAN;
    tr[p].Mvir = NAN;
    tr[p].frac.guard_returned = -1;
    tr[p].frac.x = NAN;
    tr[p].frac.f_ffb = NAN;
    tr[p].frac.thr.z_norm = NAN;
    tr[p].frac.thr.log_Mvir_ffb_code = NAN;
    tr[p].frac.thr.threshold = NAN;

    /* :213 */
    if (g[p].mergeType > 0) {
      tr[p].skipped = 1;
      continue;
    }

    /* :225 */
    tr[p].draw_bits = tr_halo_uniform_bits(g[p].MostBoundID, snapnum);
    const double draw = tr_uniform_from_bits(tr[p].draw_bits);
    tr[p].draw = draw;

    /* :227 */
    if (rp->EnhancedStarFormationOn != 1) {
      fatal("EnhancedStarFormationOn %d is not transcribed", rp->EnhancedStarFormationOn);
    }
    /* :229 */
    const double Mvir = g[p].Mvir;
    tr[p].Mvir = Mvir;
    /* :232 */
    const double f_ffb = tr_fraction(Mvir, Zcurr, rp, &tr[p].frac);
    /* :234 */
    const double random_uniform = draw;
    /* :236-240 */
    if (random_uniform < f_ffb) {
      tr[p].ffb_regime = 1;
    } else {
      tr[p].ffb_regime = 0;
    }
  }
}

// ============================================================================
// OBSERVATIONS: pow AGAINST __exp10, AND THE UNFUSED LOG TERM
// ============================================================================

/* libm's pow, called through a volatile pointer so that clang cannot simplify pow(10.0, x) to
 * __exp10 as it does in a direct call */
static double (*volatile libm_pow)(double, double) = pow;

/** @brief Whether libm's pow(10.0, v) equals __exp10(v) bit for bit */
static int pow_equals_exp10(const double v) { return same_f64(libm_pow(10.0, v), __exp10(v)); }

/**
 * @brief Whether the threshold with the log term rounded as a separate product and sum differs
 *        from the fused one SAGE26's object computes
 */
static int unfused_threshold_differs(const double z_norm, const double hubble_h,
                                     const double fused_threshold) {
  /* Separate statements: contraction fuses only within one expression */
  const double product = FFB_THRESHOLD_SLOPE * log10(z_norm);
  const double unfused_log = 0.8 + log10(hubble_h) + product;
  return !same_f64(pow(10.0, unfused_log), fused_threshold);
}

// ============================================================================
// THE RECORD: CHECKED LOCALS AND TWO-SIDED VALIDATION
// ============================================================================

/** The composition a recorded local is checked and validated in */
enum local_kind {
  LOCAL_Z_NORM,    /* -> threshold (tr_threshold_from_z_norm) */
  LOCAL_LOG_CODE,  /* -> threshold (tr_threshold_from_log) */
  LOCAL_THRESHOLD, /* -> fraction (tr_fraction_from_threshold) */
  LOCAL_X,         /* -> fraction (tr_fraction_from_x) */
  LOCAL_FRACTION,  /* -> FFBRegime (draw < f_ffb) */
  LOCAL_DRAW       /* -> FFBRegime (draw < SAGE26's calculate_ffb_fraction) */
};

/** What a composition reads besides the local, and which output it is compared on */
struct composition {
  int output_is_regime; /* 1: the row's FFBRegime; 0: the routine's double return */
  int threshold_level;  /* 1: a threshold local whose output is the threshold itself */
  double hubble_h;
  double Mvir;
  double z;
  double draw;
};

/** @brief Re-run a local's composition with the local replaced by `value` */
static double compose_with(const enum local_kind kind, const double value,
                           const struct composition *c) {
  struct fraction_trace scratch;
  double threshold = NAN;
  switch (kind) {
  case LOCAL_Z_NORM:
    threshold = tr_threshold_from_z_norm(value, c->hubble_h);
    break;
  case LOCAL_LOG_CODE:
    threshold = tr_threshold_from_log(value);
    break;
  case LOCAL_THRESHOLD:
    return c->output_is_regime
               ? (double)(c->draw < tr_fraction_from_threshold(c->Mvir, value, &scratch))
               : tr_fraction_from_threshold(c->Mvir, value, &scratch);
  case LOCAL_X:
    return c->output_is_regime ? (double)(c->draw < tr_fraction_from_x(value))
                               : tr_fraction_from_x(value);
  case LOCAL_FRACTION:
    return (double)(c->draw < value);
  case LOCAL_DRAW: {
    /* the continuation is SAGE26's own public fraction (:232, :236) */
    const double f_ffb = calculate_ffb_fraction(c->Mvir, c->z, &run_params);
    return (double)(value < f_ffb);
  }
  }
  /* A threshold local: the threshold itself, or the fraction (or FFBRegime) it leads to */
  if (c->threshold_level) {
    return threshold;
  }
  const double f_ffb = tr_fraction_from_threshold(c->Mvir, threshold, &scratch);
  return c->output_is_regime ? (double)(c->draw < f_ffb) : f_ffb;
}

/** @brief Equal bit for bit, or, where `nan_any_sign`, both NaN */
static int reproduces(const double got, const double want, const int nan_any_sign) {
  return same_f64(got, want) || (nan_any_sign && isnan(got) && isnan(want));
}

/**
 * @brief Write one recorded local with its markers (README.md, "The c13 Group")
 *
 * Refuses the fixture unless the local's composition, run from the recorded value, reproduces
 * the recorded output bit for bit. x's composition, exp(-x), negates a NaN x where SAGE26's
 * object (which never forms x) divides by -0.15, so for a NaN x only the NaN is reproduced, not
 * its sign; nan_any_sign says so.
 */
static void record_local(const char *prefix, const char *name, const enum local_kind kind,
                         const int check_kind, const int absent, const double value,
                         const double output, const struct composition *c, const int nan_any_sign) {
  out_i32(field_name("%s.%s_absent", prefix, name), absent);
  if (absent) {
    return;
  }
  const double reproduced = compose_with(kind, value, c);
  if (!reproduces(reproduced, output, nan_any_sign)) {
    fatal("%s.%s: its composition gives %.17g, SAGE26 %.17g", prefix, name, reproduced, output);
  }
  const double up = compose_with(kind, nextafter(value, INFINITY), c);
  const double down = compose_with(kind, nextafter(value, -INFINITY), c);
  const int validated_up = !same_f64(up, output);
  const int validated_down = !same_f64(down, output);

  out_f64(field_name("%s.%s", prefix, name), value);
  out_i32(field_name("%s.%s_checked", prefix, name), 1);
  out_i32(field_name("%s.%s_check_kind", prefix, name), check_kind);
  out_i32(field_name("%s.%s_validated_up", prefix, name), validated_up && !isnan(value));
  out_i32(field_name("%s.%s_validated_down", prefix, name), validated_down && !isnan(value));
  out_i32(field_name("%s.%s_validated", prefix, name),
          validated_up && validated_down && !isnan(value));
}

/** @brief Write a threshold evaluation's locals and observations under `prefix` */
static void record_threshold(const char *prefix, const struct threshold_trace *t,
                             struct composition *c, const double output) {
  record_local(prefix, "z_norm", LOCAL_Z_NORM, CHECK_TAIL, 0, t->z_norm, output, c, 0);
  record_local(prefix, "log_Mvir_ffb_code", LOCAL_LOG_CODE, CHECK_TAIL, 0, t->log_Mvir_ffb_code,
               output, c, 0);
  out_i32(field_name("%s.pow_equals_exp10", prefix), pow_equals_exp10(t->log_Mvir_ffb_code));
  out_i32(field_name("%s.unfused_threshold_differs", prefix),
          unfused_threshold_differs(t->z_norm, c->hubble_h, t->threshold));
}

/** @brief Write a fraction evaluation's locals (the threshold's included) under `prefix` */
static void record_fraction(const char *prefix, const struct fraction_trace *t,
                            struct composition *c, const double output) {
  c->threshold_level = 0;
  record_threshold(prefix, &t->thr, c, output);
  record_local(prefix, "Mvir_ffb", LOCAL_THRESHOLD, CHECK_TAIL, 0, t->thr.threshold, output, c, 0);
  out_i32(field_name("%s.guard_returned", prefix), t->guard_returned);
  record_local(prefix, "x", LOCAL_X, CHECK_TAIL, t->guard_returned, t->x, output, c, isnan(t->x));
}

// ============================================================================
// THE ROUTINES
// ============================================================================

/** @brief Read call.ncase, 1..MAX_CASES */
static int read_ncase(void) {
  const int ncase = in_i32("call.ncase");
  if (ncase < 1 || ncase > MAX_CASES) {
    fatal("call.ncase = %d is outside 1..%d", ncase, MAX_CASES);
  }
  return ncase;
}

/** @brief Read params.EnhancedStarFormationOn, which must be 1 (the only transcribed mode) */
static void read_enhanced_star_formation_on(void) {
  run_params.EnhancedStarFormationOn = in_i32("params.EnhancedStarFormationOn");
  if (run_params.EnhancedStarFormationOn != 1) {
    fatal("params.EnhancedStarFormationOn = %d: only mode 1 is transcribed (0 is the caller's "
          "gate, 2 is not ported)",
          run_params.EnhancedStarFormationOn);
  }
}

static void run_calculate_ffb_threshold_mass(void) {
  run_params.Hubble_h = in_f64("params.Hubble_h");
  const int ncase = read_ncase();
  double z[MAX_CASES];
  for (int k = 0; k < ncase; k++) {
    z[k] = in_f64(field_name("case[%d].z", k));
  }
  check_all_fields_used();

  for (int k = 0; k < ncase; k++) {
    /* 1. SAGE26's own call */
    const double sage26_return = calculate_ffb_threshold_mass(z[k], &run_params);

    /* 2. The transcription must reproduce it */
    struct threshold_trace t;
    const double transcribed = tr_threshold(z[k], &run_params, &t);
    if (!same_f64(transcribed, sage26_return)) {
      fatal("case %d: the transcription returns %.17g, SAGE26 %.17g", k, transcribed,
            sage26_return);
    }

    /* 3. The record */
    out_f64(field_name("return.case[%d]", k), sage26_return);
    char prefix[NAME_LEN];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    struct composition c = {0, 1, run_params.Hubble_h, NAN, z[k], NAN};
    record_threshold(prefix, &t, &c, sage26_return);
  }
}

static void run_calculate_ffb_fraction(void) {
  read_enhanced_star_formation_on();
  run_params.Hubble_h = in_f64("params.Hubble_h");
  const int ncase = read_ncase();
  double mvir[MAX_CASES], z[MAX_CASES];
  for (int k = 0; k < ncase; k++) {
    mvir[k] = in_f64(field_name("case[%d].Mvir", k));
    z[k] = in_f64(field_name("case[%d].z", k));
  }
  check_all_fields_used();

  for (int k = 0; k < ncase; k++) {
    /* 1. SAGE26's own call */
    const double sage26_return = calculate_ffb_fraction(mvir[k], z[k], &run_params);

    /* 2. The transcription must reproduce it, and its threshold SAGE26's public one */
    struct fraction_trace t;
    const double transcribed = tr_fraction(mvir[k], z[k], &run_params, &t);
    if (!same_f64(transcribed, sage26_return)) {
      fatal("case %d: the transcription returns %.17g, SAGE26 %.17g", k, transcribed,
            sage26_return);
    }
    if (!same_f64(t.thr.threshold, calculate_ffb_threshold_mass(z[k], &run_params))) {
      fatal("case %d: the transcribed threshold differs from SAGE26's", k);
    }

    /* 3. The record */
    out_f64(field_name("return.case[%d]", k), sage26_return);
    char prefix[NAME_LEN];
    snprintf(prefix, sizeof(prefix), "intermediate.case[%d]", k);
    struct composition c = {0, 0, run_params.Hubble_h, mvir[k], z[k], NAN};
    record_fraction(prefix, &t, &c, sage26_return);
  }
}

/** @brief Whether SAGE26's call changed any byte of a galaxy other than its FFBRegime */
static int changed_other_than_ffb_regime(const struct GALAXY *before, const struct GALAXY *after) {
  /* Byte copies, so padding is compared as SAGE26's call left it */
  static struct GALAXY a, b;
  memcpy(&a, before, sizeof(a));
  memcpy(&b, after, sizeof(b));
  a.FFBRegime = 0;
  b.FFBRegime = 0;
  return memcmp(&a, &b, sizeof(a)) != 0;
}

/**
 * @brief Read the classifier call: the two params, the redshift and snapshot, the galaxies, the
 *        rand() queue and the optional reference values
 *
 * determine_and_store_ffb_regime reads params.EnhancedStarFormationOn and params.Hubble_h, and
 * per galaxy mergeType, MostBoundID and Mvir (R/src/model_regimes.c:194-232), and writes
 * FFBRegime (:236-240). Type is read for the Mimic side (the package skips a row by Type 3),
 * the input FFBRegime shows that a skipped row keeps it and a classified row's is overwritten,
 * and Regime (SAGE26's CGM regime, which the routine does not read) shows that it does not
 * restrict the classification. With call.has_reference 1 each galaxy's
 * reference.galaxy[i].FFBRegime is the value the R6 reference wrote for that row, and the oracle
 * refuses the fixture unless SAGE26's call reproduces it.
 *
 * @return The number of galaxies
 */
static int read_classifier_call(double *Zcurr, int *snapnum, int *has_reference,
                                int reference[MAX_GALAXIES]) {
  read_enhanced_star_formation_on();
  run_params.Hubble_h = in_f64("params.Hubble_h");
  *Zcurr = in_f64("call.Zcurr");
  *snapnum = in_i32("call.snapnum");
  const int ngal = in_i32("call.ngal");
  if (ngal < 1 || ngal > MAX_GALAXIES) {
    fatal("call.ngal = %d is outside 1..%d", ngal, MAX_GALAXIES);
  }
  for (int i = 0; i < ngal; i++) {
    galaxies[i].Type = in_i32(field_name("galaxy[%d].Type", i));
    galaxies[i].mergeType = in_i32(field_name("galaxy[%d].mergeType", i));
    galaxies[i].Mvir = in_f32(field_name("galaxy[%d].Mvir", i));
    galaxies[i].MostBoundID = in_i64(field_name("galaxy[%d].MostBoundID", i));
    galaxies[i].FFBRegime = in_i32(field_name("galaxy[%d].FFBRegime", i));
    galaxies[i].Regime = in_i32(field_name("galaxy[%d].Regime", i));
  }
  rand_queue_length = in_i32("rand.queue_length");
  if (rand_queue_length < 0 || rand_queue_length > MAX_QUEUE) {
    fatal("rand.queue_length = %d is outside 0..%d", rand_queue_length, MAX_QUEUE);
  }
  for (int k = 0; k < rand_queue_length; k++) {
    const int q = in_i32(field_name("rand.queue[%d]", k));
    if (q < 0 || q > RAND_MAX) {
      fatal("rand.queue[%d] = %d is not a rand() value (0..RAND_MAX = %d)", k, q, RAND_MAX);
    }
    rand_queue[k] = q;
  }
  *has_reference = in_i32("call.has_reference");
  if (*has_reference != 0 && *has_reference != 1) {
    fatal("call.has_reference = %d is not 0 or 1", *has_reference);
  }
  for (int i = 0; *has_reference && i < ngal; i++) {
    reference[i] = in_i32(field_name("reference.galaxy[%d].FFBRegime", i));
  }
  return ngal;
}

/** @brief SAGE26's call over every row from `input`, with the queue (inverted or not) */
static int sage26_classify(const int ngal, const double Zcurr, const int snapnum,
                           const struct GALAXY *input, const int invert) {
  memcpy(galaxies, input, sizeof(galaxies));
  rand_next = 0;
  rand_invert = invert;
  rand_armed = 1;
  determine_and_store_ffb_regime(ngal, Zcurr, snapnum, galaxies, &run_params);
  rand_armed = 0;
  rand_invert = 0;
  return rand_next;
}

static void run_determine_and_store_ffb_regime(void) {
  double Zcurr = NAN;
  int snapnum = 0;
  int has_reference = 0;
  int reference[MAX_GALAXIES];
  const int ngal = read_classifier_call(&Zcurr, &snapnum, &has_reference, reference);
  check_all_fields_used();

  static struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* 1. SAGE26's call over every row, as its driver makes it (R/src/core_build_model.c:426) */
  const int draws_discarded = sage26_classify(ngal, Zcurr, snapnum, input, 0);
  if (draws_discarded != rand_queue_length) {
    fatal("SAGE26's call discarded %d of the %d queue entries: a fixture's queue must state "
          "exactly its draws",
          draws_discarded, rand_queue_length);
  }
  static struct GALAXY full_call[MAX_GALAXIES];
  memcpy(full_call, galaxies, sizeof(full_call));
  for (int i = 0; i < ngal; i++) {
    if (changed_other_than_ffb_regime(&input[i], &full_call[i])) {
      fatal("SAGE26's call changed galaxy[%d] beyond its FFBRegime", i);
    }
  }

  /* 2. The same call with every queue value q replaced by RAND_MAX - q: the discarded draws must
   *    not reach the result */
  if (sage26_classify(ngal, Zcurr, snapnum, input, 1) != draws_discarded) {
    fatal("the inverted queue changed the number of discarded draws");
  }
  for (int i = 0; i < ngal; i++) {
    if (galaxies[i].FFBRegime != full_call[i].FFBRegime) {
      fatal("galaxy[%d]: the inverted rand() queue changed FFBRegime", i);
    }
  }

  /* 3. SAGE26's call on each row alone, from the input, with the queue rewound: which rows
   *    discard a draw */
  int row_discarded[MAX_GALAXIES];
  memcpy(galaxies, input, sizeof(input));
  rand_next = 0;
  for (int i = 0; i < ngal; i++) {
    const int before = rand_next;
    rand_armed = 1;
    determine_and_store_ffb_regime(1, Zcurr, snapnum, &galaxies[i], &run_params);
    rand_armed = 0;
    row_discarded[i] = rand_next - before;
    if (row_discarded[i] != 0 && row_discarded[i] != 1) {
      fatal("SAGE26's call on galaxy[%d] alone discarded %d draws", i, row_discarded[i]);
    }
    if (galaxies[i].FFBRegime != full_call[i].FFBRegime) {
      fatal("SAGE26's call on galaxy[%d] alone wrote FFBRegime %d, the full call %d", i,
            galaxies[i].FFBRegime, full_call[i].FFBRegime);
    }
  }

  /* 4. The transcription: the same FFBRegime, the same rows skipped; its threshold and fraction
   *    SAGE26's public ones; the draw continuation-checked through SAGE26's public fraction */
  struct row_trace tr[MAX_GALAXIES];
  tr_determine_and_store_ffb_regime(ngal, Zcurr, snapnum, input, &run_params, tr);
  for (int i = 0; i < ngal; i++) {
    if (tr[i].ffb_regime != full_call[i].FFBRegime) {
      fatal("the transcription disagrees with SAGE26 on galaxy[%d]: FFBRegime %d against %d", i,
            tr[i].ffb_regime, full_call[i].FFBRegime);
    }
    if (tr[i].skipped != (input[i].mergeType > 0) || tr[i].skipped != !row_discarded[i]) {
      fatal("galaxy[%d]: a row must discard a draw exactly when SAGE26 does not skip it", i);
    }
    if (tr[i].skipped) {
      continue;
    }
    if (!same_f64(tr[i].frac.f_ffb, calculate_ffb_fraction(tr[i].Mvir, Zcurr, &run_params)) ||
        !same_f64(tr[i].frac.thr.threshold, calculate_ffb_threshold_mass(Zcurr, &run_params))) {
      fatal("galaxy[%d]: the transcribed fraction or threshold differs from SAGE26's", i);
    }
  }
  if (has_reference) {
    for (int i = 0; i < ngal; i++) {
      if (full_call[i].FFBRegime != reference[i]) {
        fatal("galaxy[%d]: SAGE26's call wrote FFBRegime %d, the R6 reference %d", i,
              full_call[i].FFBRegime, reference[i]);
      }
    }
  }

  /* 5. The record */
  out_i32("intermediate.RAND_MAX", RAND_MAX);
  out_i32("intermediate.draws_discarded", draws_discarded);
  out_i32("intermediate.rand_queue_independent", 1);
  for (int i = 0; i < ngal; i++) {
    out_i32(field_name("galaxy[%d].FFBRegime", i), full_call[i].FFBRegime);
    if (has_reference) {
      out_i32(field_name("reference.galaxy[%d].FFBRegime", i), reference[i]);
    }
    char prefix[NAME_LEN];
    snprintf(prefix, sizeof(prefix), "intermediate.galaxy[%d]", i);
    out_i32(field_name("%s.skipped", prefix), tr[i].skipped);
    out_i32(field_name("%s.discarded_draw", prefix), row_discarded[i]);
    if (tr[i].skipped) {
      continue;
    }
    out_bits_i64(field_name("%s.draw_bits", prefix), tr[i].draw_bits);
    out_f64(field_name("%s.Mvir", prefix), tr[i].Mvir);
    const double regime = (double)full_call[i].FFBRegime;
    struct composition c = {1, 0, run_params.Hubble_h, tr[i].Mvir, Zcurr, tr[i].draw};
    record_local(prefix, "draw", LOCAL_DRAW, CHECK_CONTINUATION, 0, tr[i].draw, regime, &c, 0);
    record_fraction(prefix, &tr[i].frac, &c, regime);
    record_local(prefix, "f_ffb", LOCAL_FRACTION, CHECK_TAIL, 0, tr[i].frac.f_ffb, regime, &c, 0);
    out_i32(field_name("%s.draw_below_f_ffb", prefix), tr[i].draw < tr[i].frac.f_ffb);
    out_i32(field_name("%s.draw_equals_f_ffb", prefix), same_f64(tr[i].draw, tr[i].frac.f_ffb));
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
    {"calculate_ffb_threshold_mass", run_calculate_ffb_threshold_mass},
    {"calculate_ffb_fraction", run_calculate_ffb_fraction},
    {"determine_and_store_ffb_regime", run_determine_and_store_ffb_regime},
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
  poison_state();
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
          "# SAGE26 oracle output for %s, written by oracle_c13 (regenerate.sh); do not edit.\n",
          base_name(input_path));
  fprintf(output_file, "%s\nroutine %s\n", ORACLE_FORMAT_LINE, routine_name);

  entry->run();

  const int close_status = fclose(output_file);
  output_file = NULL;
  if (close_status != 0) {
    remove(output_path);
    fatal("cannot write output %s", output_path);
  }
  return EXIT_SUCCESS;
}
