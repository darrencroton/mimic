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
 *   starformation_ffb               S:1262-1368 (c13 Slice 2; S = R/src/
 *                                   model_starformation_and_feedback.c), called directly; the
 *                                   static compute_sn_feedback_ffb (S:901-972) and the inlined
 *                                   H I block are reached only through it
 *   starformation_and_feedback_ffb  the wrapper starformation_and_feedback (S:995-1110) on an
 *                                   FFBRegime 1 row at EnhancedStarFormationOn 1, whose early
 *                                   return (S:1004-1008) calls starformation_ffb
 *
 * The c13 Slice 2 routines' carried values come from the checked transcription
 * tr_starformation_ffb (README.md, "The c13 Group, Part 2: FFB Star Formation").
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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_allvars.h"
#include "macros.h"
#include "model_misc.h"
#include "model_regimes.h"
#include "model_starformation_and_feedback.h"

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
// c13 PART 2: FEEDBACK-FREE-BURST STAR FORMATION (starformation_ffb AND THE WRAPPER'S DISPATCH)
// ============================================================================
//
// S = R/src/model_starformation_and_feedback.c, X = R/src/model_misc.h (README.md, "The c13
// Group, Part 2: FFB Star Formation"). starformation_ffb (S:1262-1368) and the wrapper
// starformation_and_feedback (S:995-1110), whose FFB early return (S:1004-1008) calls it, are
// public and called directly for the end state. The static compute_sn_feedback_ffb (S:901-972)
// and the inlined H I block (S:1280-1292, with atomic_disk_radius and ionized_gas_fraction,
// S:130-152) are reached only through them, so the carried values come from the checked
// transcription tr_starformation_ffb below, each statement cited to its line, composed with
// SAGE26's own public get_metallicity, update_from_star_formation and update_from_feedback and its
// own static inline capped_eta_reheat, sn_energy_coupling and add_metals_to_hot_reservoir
// (X:67-74, X:118-149), compiled here from SAGE26's header.

/* SAGE26's file-scope constants of S, restated with their values and types (S:60, :75, :79, :84,
 * :97, :106) */
static const double SF_DISK_RADIUS_FRAC = 3.0;
static const double FIRE_V_CRIT_KMS = 60.0;
static const double FIRE_REDSHIFT_EXPONENT = 1.25;
static const double KD11_METAL_HALO_MASS = 30.0;
static const double SIGMA_HI_CRIT = 0.5;
static const double GAS_DISK_RADIUS_FACTOR = 1.0;
/* R/src/model_disk_instability.c:35, for the instability observation only */
static const double TOOMRE_DISK_FACTOR = 3.0;

#define FFB_MAX_GALAXIES 4

/** @brief A float member of SAGE26's struct GALAXY: a scalar, or STEPS elements */
struct ffb_member {
  const char *name;
  size_t offset;
  int elements;
};

#define FFB_SCALAR(m) {#m, offsetof(struct GALAXY, m), 1}
#define FFB_ARRAY(m) {#m, offsetof(struct GALAXY, m), STEPS}

/** The halo inputs: read, never written (float in SAGE26) */
static const struct ffb_member FFB_HALO[] = {FFB_SCALAR(Mvir), FFB_SCALAR(Vvir), FFB_SCALAR(Vmax),
                                             FFB_SCALAR(DiskScaleRadius)};

/**
 * The recorded float state of every galaxy: what starformation_ffb writes (the cold, stellar,
 * hot, CGM and ejected pairs, H1gas, H2gas, OutflowRate, MassLoading and the three disk histories)
 * and what it must leave as it was (the instability's bulge masses and radii, the depletion time,
 * the black hole and the three bulge histories)
 */
static const struct ffb_member FFB_STATE[] = {FFB_SCALAR(ColdGas),
                                              FFB_SCALAR(MetalsColdGas),
                                              FFB_SCALAR(StellarMass),
                                              FFB_SCALAR(MetalsStellarMass),
                                              FFB_SCALAR(BulgeMass),
                                              FFB_SCALAR(MetalsBulgeMass),
                                              FFB_SCALAR(InstabilityBulgeMass),
                                              FFB_SCALAR(MergerBulgeMass),
                                              FFB_SCALAR(BulgeRadius),
                                              FFB_SCALAR(MergerBulgeRadius),
                                              FFB_SCALAR(InstabilityBulgeRadius),
                                              FFB_SCALAR(HotGas),
                                              FFB_SCALAR(MetalsHotGas),
                                              FFB_SCALAR(CGMgas),
                                              FFB_SCALAR(MetalsCGMgas),
                                              FFB_SCALAR(EjectedMass),
                                              FFB_SCALAR(MetalsEjectedMass),
                                              FFB_SCALAR(H1gas),
                                              FFB_SCALAR(H2gas),
                                              FFB_SCALAR(H2DepletionTime_Gyr),
                                              FFB_SCALAR(OutflowRate),
                                              FFB_SCALAR(MassLoading),
                                              FFB_SCALAR(BlackHoleMass),
                                              FFB_SCALAR(QuasarModeBHaccretionMass),
                                              FFB_ARRAY(SfrDisk),
                                              FFB_ARRAY(SfrDiskColdGas),
                                              FFB_ARRAY(SfrDiskColdGasMetals),
                                              FFB_ARRAY(SfrBulge),
                                              FFB_ARRAY(SfrBulgeColdGas),
                                              FFB_ARRAY(SfrBulgeColdGasMetals)};

#define FFB_COUNT(a) (sizeof(a) / sizeof((a)[0]))

/** @brief Element k of a float member, read by its byte offset (memcpy, no pointer cast) */
static float ffb_get(const struct GALAXY *g, const struct ffb_member *m, const int k) {
  float value;
  memcpy(&value, (const char *)g + m->offset + (size_t)k * sizeof(float), sizeof(value));
  return value;
}

/** @brief Store element k of a float member by its byte offset */
static void ffb_set(struct GALAXY *g, const struct ffb_member *m, const int k, const float value) {
  memcpy((char *)g + m->offset + (size_t)k * sizeof(float), &value, sizeof(value));
}

static const char *ffb_member_field(const char *prefix, const int i, const struct ffb_member *m,
                                    const int k) {
  return m->elements == 1 ? field_name("%sgalaxy[%d].%s", prefix, i, m->name)
                          : field_name("%sgalaxy[%d].%s[%d]", prefix, i, m->name, k);
}

static void out_f32(const char *name, const float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  fprintf(output_file, "f32 %s 0x%08" PRIx32 "  # %.9g\n", name, bits, (double)value);
}

/** The carried values of the transcription, in statement order (README "The c13 Group, Part 2") */
enum ffb_local {
  FL_REFF,            /* S:1273 */
  FL_TDYN,            /* S:1274 */
  FL_ATOMICH,         /* S:1285-1286, after the floor */
  FL_F_ION,           /* S:1287-1289 */
  FL_STRDOT,          /* S:1309 */
  FL_STARS,           /* the stars formed, S:1306-1320 */
  FL_METALLICITY,     /* S:1329, the pre-commit metallicity, reused at S:1348 */
  FL_FIRE_SCALING,    /* S:912-924 */
  FL_ETA_RAW,         /* FeedbackReheatingEpsilon * fire_scaling, S:931 */
  FL_ETA,             /* capped_eta_reheat's return, S:930-931 */
  FL_REHEATED_PRE,    /* eta * stars, S:933 (0 with an invalid scaling) */
  FL_RENORM_FAC,      /* S:944 */
  FL_STARS_RENORM,    /* the renormalised stars, S:945 (the stars formed when inactive) */
  FL_REHEATED_RENORM, /* S:946 */
  FL_COUPLING,        /* sn_energy_coupling's return, S:954 */
  FL_E_FB,            /* S:954-956 */
  FL_E_LIFT,          /* S:957 */
  FL_EJECTED,         /* after the floor, S:958, :967 */
  FL_REHEATED_COMMIT, /* after the clamp at S:1346, passed to update_from_feedback */
  FL_UFF_REHEATED,    /* update_from_feedback's clamped reheated mass, S:1153-1156 */
  FL_UFF_EJECTED, /* its ejection capped to the selected gas, S:1184-1186, :1208-1210, :1232-1234 */
  FL_UFF_METALLICITY,   /* the selected pair's metallicity, S:1187, :1211, :1235 */
  FL_UFF_METALS,        /* metallicity * ejected, S:1189, :1213, :1237 */
  FL_UFF_METALS_CAPPED, /* after its cap, S:1190-1192, :1214-1216, :1238-1240 */
  FL_FRAC_Z,            /* FracZleaveDiskVal, S:1354-1355 */
  FL_YIELD_TO_HOT,      /* metals_leaving_disk (S:1358) or all_metals (S:1361) */
  FL_COUNT
};

/**
 * Each value's name, check kind (1 continuation: it is passed to a SAGE26 routine compiled from
 * SAGE26's source, which runs to the end state; 2 tail: the transcription runs from it), whether
 * it is a local of update_from_feedback (perturbed in the composition with that routine
 * transcribed) and whether it is on the loading chain (narrowing-limited when no stars form)
 */
static const struct {
  const char *name;
  int kind;
  int uff_local;
  int loading_chain;
} FFB_LOCALS[FL_COUNT] = {
    {"reff", CHECK_TAIL, 0, 0},
    {"tdyn", CHECK_TAIL, 0, 0},
    {"atomicH", CHECK_TAIL, 0, 0},
    {"f_ion", CHECK_TAIL, 0, 0},
    {"strdot", CHECK_TAIL, 0, 0},
    {"stars", CHECK_CONTINUATION, 0, 0},
    {"metallicity", CHECK_CONTINUATION, 0, 0},
    {"fire_scaling", CHECK_CONTINUATION, 0, 1},
    {"eta_raw", CHECK_CONTINUATION, 0, 1},
    {"eta_reheat", CHECK_TAIL, 0, 1},
    {"reheated_mass_pre_renorm", CHECK_TAIL, 0, 0},
    {"renorm_fac", CHECK_TAIL, 0, 0},
    {"stars_renorm", CHECK_TAIL, 0, 0},
    {"reheated_mass_renorm", CHECK_TAIL, 0, 0},
    {"coupling", CHECK_TAIL, 0, 0},
    {"E_FB", CHECK_TAIL, 0, 0},
    {"E_lift", CHECK_TAIL, 0, 0},
    {"ejected_mass", CHECK_CONTINUATION, 0, 0},
    {"reheated_mass_commit", CHECK_CONTINUATION, 0, 0},
    {"uff_reheated_mass_clamped", CHECK_TAIL, 1, 0},
    {"uff_ejected_mass_capped", CHECK_TAIL, 1, 0},
    {"uff_metallicity_selected", CHECK_TAIL, 1, 0},
    {"uff_metals_to_eject", CHECK_TAIL, 1, 0},
    {"uff_metals_to_eject_capped", CHECK_TAIL, 1, 0},
    {"FracZleaveDiskVal", CHECK_TAIL, 0, 0},
    {"yield_metals_to_hot", CHECK_CONTINUATION, 0, 0},
};

/** The branch flags (README "The c13 Group, Part 2"); -1 where the path does not evaluate one */
enum ffb_flag {
  FF_NONFINITE_GUARD,    /* S:1303-1306 */
  FF_TDYN_GAS_POSITIVE,  /* S:1307 */
  FF_STRDOT_REJECTED,    /* S:1311 */
  FF_STARS_CAP_BINDS,    /* S:1315 */
  FF_STARS_REJECTED,     /* S:1316 */
  FF_ATOMICH_FLOORED,    /* S:1286 */
  FF_F_ION_PATH,         /* S:138-151: 0 an input guard, 1 sigma0 <= crit, 2 the expression,
                            3 clamped to 0, 4 clamped to 1 */
  FF_SF_CAPS_TRACKED,    /* S:1131 */
  FF_SF_H2_CAP_BINDS,    /* S:1133 */
  FF_SF_H1_CAP_BINDS,    /* S:1134 */
  FF_FIRE_SCALING_VALID, /* S:917 */
  FF_FIRE_VC_FLOORED,    /* S:918 */
  FF_FIRE_HIGH_VELOCITY, /* S:920 */
  FF_ETA_CAP_BINDS,      /* X:149, observed: capped_eta_reheat returned other than its argument */
  FF_RENORM_ACTIVE,      /* S:943 */
  FF_EJECTION_EVALUATED, /* S:950, :952 */
  FF_COUPLING_CAPPED,    /* X:121, observed: FeedbackEjectionEfficiency * scaling > 2 */
  FF_ENERGY_EJECTS,      /* S:958, E_FB > E_lift */
  FF_ENERGY_EQUAL,       /* E_FB == E_lift (no ejection) */
  FF_EJECTED_FLOORED,    /* S:967 */
  FF_REHEAT_CLAMP_FFB_BINDS, /* S:1346 */
  FF_UFF_REHEAT_CLAMP_BINDS, /* S:1153 */
  FF_FEEDBACK_RESERVOIR_CGM, /* S:1175-1176 */
  FF_FB_CAPS_TRACKED,        /* S:1169 */
  FF_FB_H2_CAP_BINDS,        /* S:1171 */
  FF_FB_H1_CAP_BINDS,        /* S:1172 */
  FF_EJECTED_CAP_BINDS,      /* S:1184, :1208, :1232 */
  FF_METALS_CAP_BINDS,       /* S:1190, :1214, :1238 */
  FF_YIELD_ABOVE_THRESHOLD,  /* S:1353 */
  FF_YIELD_RESERVOIR_CGM,    /* X:69 */
  FF_COUNT
};

static const char *const FFB_FLAG_NAMES[FF_COUNT] = {"nonfinite_guard",
                                                     "tdyn_gas_positive",
                                                     "strdot_rejected",
                                                     "stars_cap_binds",
                                                     "stars_rejected",
                                                     "atomicH_floored",
                                                     "f_ion_path",
                                                     "sf_caps_tracked",
                                                     "sf_h2_cap_binds",
                                                     "sf_h1_cap_binds",
                                                     "fire_scaling_valid",
                                                     "fire_vc_floored",
                                                     "fire_high_velocity",
                                                     "eta_cap_binds",
                                                     "renorm_active",
                                                     "ejection_evaluated",
                                                     "coupling_capped",
                                                     "energy_ejects",
                                                     "energy_equal",
                                                     "ejected_floored",
                                                     "reheat_clamp_ffb_binds",
                                                     "uff_reheat_clamp_binds",
                                                     "feedback_reservoir_cgm",
                                                     "fb_caps_tracked",
                                                     "fb_h2_cap_binds",
                                                     "fb_h1_cap_binds",
                                                     "ejected_cap_binds",
                                                     "metals_cap_binds",
                                                     "yield_coldgas_above_threshold",
                                                     "yield_reservoir_cgm"};

/** One composition's record: every value formed and every flag */
struct ffb_trace {
  double value[FL_COUNT];
  int present[FL_COUNT];
  int flag[FF_COUNT];
  int out_of_domain;          /* a SAGE26 assertion would abort (S:940, S:1158-1163) */
  double metallicity_post;    /* observation: get_metallicity after the commit (not SAGE26 state) */
  float coldgas_after_commit; /* ColdGas as update_from_star_formation leaves it (S:1330), the
                                 cold gas compute_sn_feedback_ffb renormalises against */
};

/** A composition's variant: which value is replaced by its neighbour, and which routines run */
struct ffb_variant {
  int local;                   /* the perturbed value, or -1 */
  int direction;               /* +1 up, -1 down */
  int uff_transcribed;         /* 1: update_from_feedback transcribed; 0: SAGE26's own */
  int recompute_metallicity;   /* observation: the feedback with the post-commit metallicity */
  int yield_with_formed_stars; /* observation: the yield from the stars formed */
};

static const struct ffb_variant FFB_PLAIN = {-1, 0, 0, 0, 0};

/** The call's arguments */
struct ffb_call {
  int ngal;
  int p;
  int centralgal;
  int step; /* the substep index */
  int effective_steps;
  int bin; /* the history bin SAGE26 passes as step (B:514-515) */
  double deltaT;
  double dt;
  double time; /* the wrapper's time argument (not read on the FFB path) */
  int wrapper; /* 1: the routine is the wrapper's dispatch */
};

static void ffb_trace_init(struct ffb_trace *t) {
  for (int k = 0; k < FL_COUNT; k++) {
    t->value[k] = NAN;
    t->present[k] = 0;
  }
  for (int k = 0; k < FF_COUNT; k++) {
    t->flag[k] = -1;
  }
  t->out_of_domain = 0;
  t->metallicity_post = NAN;
  t->coldgas_after_commit = NAN;
}

/** @brief Record a value where it is formed, and hand on its neighbour if the variant says so */
static double ffb_tap(struct ffb_trace *t, const struct ffb_variant *v, const enum ffb_local id,
                      const double value) {
  t->value[id] = value;
  t->present[id] = 1;
  if (v->local == (int)id) {
    return nextafter(value, v->direction > 0 ? INFINITY : -INFINITY);
  }
  return value;
}

/** @brief SAGE26's atomic_disk_radius (S:130-133), transcribed */
static double tr_atomic_disk_radius(const double rs_code) {
  return GAS_DISK_RADIUS_FACTOR * rs_code;
}

/** @brief SAGE26's ionized_gas_fraction (S:135-152), transcribed; *path the branch taken */
static double tr_ionized_gas_fraction(const double coldgas_code, const double rs_code,
                                      const double h, const double sigma_hi_crit, int *path) {
  if (coldgas_code <= 0.0 || rs_code <= 0.0 || sigma_hi_crit <= 0.0 || h <= 0.0) { /* S:138 */
    *path = 0;
    return 0.0;
  }
  const double mgas = CODE_MASS_TO_MSUN(coldgas_code, h);    /* S:141 */
  const double rs_pc = CODE_LENGTH_TO_PC(rs_code, h);        /* S:142 */
  const double sigma0 = mgas / (2.0 * M_PI * rs_pc * rs_pc); /* S:143 */
  if (sigma0 <= sigma_hi_crit) {                             /* S:144-146 */
    *path = 1;
    return 1.0;
  }
  const double x = log(sigma0 / sigma_hi_crit);      /* S:147 */
  double f_ion = (1.0 + x) * sigma_hi_crit / sigma0; /* S:148 */
  *path = 2;
  if (f_ion < 0.0) { /* S:149 */
    f_ion = 0.0;
    *path = 3;
  }
  if (f_ion > 1.0) { /* S:150 */
    f_ion = 1.0;
    *path = 4;
  }
  return f_ion;
}

/**
 * @brief SAGE26's static compute_sn_feedback_ffb (S:901-972), transcribed, with
 *        SupernovaRecipeOn 1 and FIREmodeOn 1 (the reader requires both)
 */
static void tr_compute_sn_feedback_ffb(const int p, double *stars_inout, double *reheated_out,
                                       double *ejected_out, struct ffb_trace *t,
                                       const struct ffb_variant *v) {
  double stars = *stars_inout; /* S:905 */
  double reheated_mass = 0.0;  /* S:906 */
  double ejected_mass = 0.0;   /* S:907 */

  double fire_scaling = 0.0;                                             /* S:912 */
  int fire_scaling_valid = 0;                                            /* S:913 */
  if (run_params.SupernovaRecipeOn == 1 && run_params.FIREmodeOn == 1) { /* S:914 */
    const double z = run_params.ZZ[galaxies[p].SnapNum];                 /* S:915 */
    const double vc = galaxies[p].Vvir;                                  /* S:916 */
    if (vc > 0.0 && z >= 0.0) {                                          /* S:917 */
      const double vc_floored = (vc < 1.0) ? 1.0 : vc;                   /* S:918 */
      const double z_term = pow(1.0 + z, FIRE_REDSHIFT_EXPONENT);        /* S:919 */
      const double v_term = (vc_floored < FIRE_V_CRIT_KMS)               /* S:920-921 */
                                ? pow(vc_floored / FIRE_V_CRIT_KMS, -3.2)
                                : pow(vc_floored / FIRE_V_CRIT_KMS, -1.0);
      fire_scaling = z_term * v_term; /* S:922 */
      fire_scaling_valid = 1;         /* S:923 */
      t->flag[FF_FIRE_VC_FLOORED] = vc < 1.0;
      t->flag[FF_FIRE_HIGH_VELOCITY] = !(vc_floored < FIRE_V_CRIT_KMS);
    }
  }
  t->flag[FF_FIRE_SCALING_VALID] = fire_scaling_valid;
  if (fire_scaling_valid) {
    fire_scaling = ffb_tap(t, v, FL_FIRE_SCALING, fire_scaling);
  }

  if (run_params.SupernovaRecipeOn == 1) {                                   /* S:927 */
    if (run_params.FIREmodeOn == 1) {                                        /* S:928 */
      if (fire_scaling_valid) {                                              /* S:929 */
        double eta_raw = run_params.FeedbackReheatingEpsilon * fire_scaling; /* S:931 */
        eta_raw = ffb_tap(t, v, FL_ETA_RAW, eta_raw);
        double eta_reheat = capped_eta_reheat(eta_raw, galaxies[p].Vvir, &run_params); /* S:930 */
        t->flag[FF_ETA_CAP_BINDS] = !same_f64(eta_reheat, eta_raw);
        eta_reheat = ffb_tap(t, v, FL_ETA, eta_reheat);
        galaxies[p].MassLoading = (float)eta_reheat; /* S:932 */
        reheated_mass = eta_reheat * stars;          /* S:933 */
      }
    }
  }
  reheated_mass = ffb_tap(t, v, FL_REHEATED_PRE, reheated_mass);

  /* S:940-941: SAGE26 aborts on a negative reheated mass */
  if (!(reheated_mass >= 0.0)) {
    t->out_of_domain = 1;
    return;
  }

  t->flag[FF_RENORM_ACTIVE] = 0;
  if ((stars + reheated_mass) > galaxies[p].ColdGas && (stars + reheated_mass) > 0.0) { /* S:943 */
    double fac = galaxies[p].ColdGas / (stars + reheated_mass);                         /* S:944 */
    fac = ffb_tap(t, v, FL_RENORM_FAC, fac);
    stars *= fac;         /* S:945 */
    reheated_mass *= fac; /* S:946 */
    t->flag[FF_RENORM_ACTIVE] = 1;
  }
  stars = ffb_tap(t, v, FL_STARS_RENORM, stars);
  reheated_mass = ffb_tap(t, v, FL_REHEATED_RENORM, reheated_mass);

  if (run_params.SupernovaRecipeOn == 1) { /* S:949 */
    t->flag[FF_EJECTION_EVALUATED] = 0;
    if (galaxies[p].Vvir > 0.0) {       /* S:950 */
      if (run_params.FIREmodeOn == 1) { /* S:951 */
        if (fire_scaling_valid) {       /* S:952 */
          t->flag[FF_EJECTION_EVALUATED] = 1;
          const double vc = galaxies[p].Vvir; /* S:953 */
          t->flag[FF_COUPLING_CAPPED] =
              run_params.FeedbackEjectionEfficiency * fire_scaling > 2.0;  /* X:120-121 */
          double coupling = sn_energy_coupling(fire_scaling, &run_params); /* S:954 */
          coupling = ffb_tap(t, v, FL_COUPLING, coupling);
          double E_FB = coupling * 0.5 * stars *
                        (run_params.EtaSNcode * run_params.EnergySNcode); /* S:954-956 */
          E_FB = ffb_tap(t, v, FL_E_FB, E_FB);
          double E_lift = 0.5 * reheated_mass * vc * vc; /* S:957 */
          E_lift = ffb_tap(t, v, FL_E_LIFT, E_lift);
          t->flag[FF_ENERGY_EJECTS] = E_FB > E_lift;
          t->flag[FF_ENERGY_EQUAL] = E_FB == E_lift;
          ejected_mass = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0; /* S:958 */
        }
      }
    }
    t->flag[FF_EJECTED_FLOORED] = ejected_mass < 0.0;
    if (ejected_mass < 0.0) { /* S:967 */
      ejected_mass = 0.0;
    }
  }
  ejected_mass = ffb_tap(t, v, FL_EJECTED, ejected_mass);

  *stars_inout = stars;          /* S:970 */
  *reheated_out = reheated_mass; /* S:971 */
  *ejected_out = ejected_mass;   /* S:972 */
}

/**
 * @brief SAGE26's update_from_feedback (S:1146-1251), transcribed with SupernovaRecipeOn 1, its
 *        locals recorded (as oracle_c05.c's transcription, with c10's CGM branch)
 */
static void tr_update_from_feedback(const int p, const int centralgal, double reheated_mass,
                                    double ejected_mass, const double metallicity,
                                    struct ffb_trace *t, const struct ffb_variant *v) {
  t->flag[FF_UFF_REHEAT_CLAMP_BINDS] = reheated_mass > galaxies[p].ColdGas;
  if (reheated_mass > galaxies[p].ColdGas) { /* S:1153-1156 */
    reheated_mass = galaxies[p].ColdGas;
  }
  reheated_mass = ffb_tap(t, v, FL_UFF_REHEATED, reheated_mass);
  if (!(reheated_mass >= 0.0) || !(reheated_mass <= galaxies[p].ColdGas)) { /* S:1158-1163 */
    t->out_of_domain = 1;
    return;
  }

  galaxies[p].ColdGas -= reheated_mass;                     /* S:1167 */
  galaxies[p].MetalsColdGas -= metallicity * reheated_mass; /* S:1168 */
  t->flag[FF_FB_CAPS_TRACKED] = sf_prescription_tracks_h2(run_params.SFprescription);
  if (sf_prescription_tracks_h2(run_params.SFprescription)) { /* S:1169 */
    const float max_h_fb =
        (galaxies[p].ColdGas > 0.0f) ? galaxies[p].ColdGas * HYDROGEN_MASS_FRAC : 0.0f; /* S:1170 */
    t->flag[FF_FB_H2_CAP_BINDS] = galaxies[p].H2gas > max_h_fb;
    if (galaxies[p].H2gas > max_h_fb) /* S:1171 */
      galaxies[p].H2gas = max_h_fb;
    t->flag[FF_FB_H1_CAP_BINDS] = galaxies[p].H1gas > max_h_fb;
    if (galaxies[p].H1gas > max_h_fb) /* S:1172 */
      galaxies[p].H1gas = max_h_fb;
  }

  const int cgm = run_params.CGMrecipeOn == 1 && galaxies[centralgal].Regime == 0; /* S:1175-1176 */
  t->flag[FF_FEEDBACK_RESERVOIR_CGM] = cgm;
  if (cgm) {
    galaxies[centralgal].CGMgas += reheated_mass;                     /* S:1180 */
    galaxies[centralgal].MetalsCGMgas += metallicity * reheated_mass; /* S:1181 */
    t->flag[FF_EJECTED_CAP_BINDS] = ejected_mass > galaxies[centralgal].CGMgas;
    if (ejected_mass > galaxies[centralgal].CGMgas) { /* S:1184-1186 */
      ejected_mass = galaxies[centralgal].CGMgas;
    }
    ejected_mass = ffb_tap(t, v, FL_UFF_EJECTED, ejected_mass);
    double metallicityCGM = get_metallicity(galaxies[centralgal].CGMgas,
                                            galaxies[centralgal].MetalsCGMgas); /* S:1187 */
    metallicityCGM = ffb_tap(t, v, FL_UFF_METALLICITY, metallicityCGM);
    double metalsCGM_to_eject = metallicityCGM * ejected_mass; /* S:1189 */
    metalsCGM_to_eject = ffb_tap(t, v, FL_UFF_METALS, metalsCGM_to_eject);
    t->flag[FF_METALS_CAP_BINDS] = metalsCGM_to_eject > galaxies[centralgal].MetalsCGMgas;
    if (metalsCGM_to_eject > galaxies[centralgal].MetalsCGMgas) { /* S:1190-1192 */
      metalsCGM_to_eject = galaxies[centralgal].MetalsCGMgas;
    }
    metalsCGM_to_eject = ffb_tap(t, v, FL_UFF_METALS_CAPPED, metalsCGM_to_eject);
    galaxies[centralgal].CGMgas -= ejected_mass;                  /* S:1195 */
    galaxies[centralgal].MetalsCGMgas -= metalsCGM_to_eject;      /* S:1196 */
    galaxies[centralgal].EjectedMass += ejected_mass;             /* S:1197 */
    galaxies[centralgal].MetalsEjectedMass += metalsCGM_to_eject; /* S:1198 */
  } else {
    /* S:1204-1205 (CGMrecipeOn 1, Regime not 0) and S:1228-1229 (CGMrecipeOn 0): the same
     * statements on the hot pair */
    galaxies[centralgal].HotGas += reheated_mass;
    galaxies[centralgal].MetalsHotGas += metallicity * reheated_mass;
    t->flag[FF_EJECTED_CAP_BINDS] = ejected_mass > galaxies[centralgal].HotGas;
    if (ejected_mass > galaxies[centralgal].HotGas) { /* S:1208-1210, :1232-1234 */
      ejected_mass = galaxies[centralgal].HotGas;
    }
    ejected_mass = ffb_tap(t, v, FL_UFF_EJECTED, ejected_mass);
    double metallicityHot = get_metallicity(galaxies[centralgal].HotGas,
                                            galaxies[centralgal].MetalsHotGas); /* S:1211, :1235 */
    metallicityHot = ffb_tap(t, v, FL_UFF_METALLICITY, metallicityHot);
    double metalsHot_to_eject = metallicityHot * ejected_mass; /* S:1213, :1237 */
    metalsHot_to_eject = ffb_tap(t, v, FL_UFF_METALS, metalsHot_to_eject);
    t->flag[FF_METALS_CAP_BINDS] = metalsHot_to_eject > galaxies[centralgal].MetalsHotGas;
    if (metalsHot_to_eject > galaxies[centralgal].MetalsHotGas) { /* S:1214-1216, :1238-1240 */
      metalsHot_to_eject = galaxies[centralgal].MetalsHotGas;
    }
    metalsHot_to_eject = ffb_tap(t, v, FL_UFF_METALS_CAPPED, metalsHot_to_eject);
    galaxies[centralgal].HotGas -= ejected_mass;                  /* S:1219, :1243 */
    galaxies[centralgal].MetalsHotGas -= metalsHot_to_eject;      /* S:1220, :1244 */
    galaxies[centralgal].EjectedMass += ejected_mass;             /* S:1221, :1245 */
    galaxies[centralgal].MetalsEjectedMass += metalsHot_to_eject; /* S:1222, :1246 */
  }
  galaxies[p].OutflowRate += reheated_mass; /* S:1249 */
}

/**
 * @brief SAGE26's starformation_ffb (S:1262-1368), transcribed, on a copy of `input`
 *
 * Leaves the end state in `galaxies`. update_from_star_formation, get_metallicity and
 * (unless the variant transcribes it) update_from_feedback are SAGE26's own; capped_eta_reheat,
 * sn_energy_coupling and add_metals_to_hot_reservoir are SAGE26's own static inline helpers
 * (X:67-74, :118-149).
 */
static void tr_starformation_ffb(const struct GALAXY *input, const struct ffb_call *c,
                                 struct ffb_trace *t, const struct ffb_variant *v) {
  memcpy(galaxies, input, sizeof(struct GALAXY) * (size_t)c->ngal);
  ffb_trace_init(t);
  const int p = c->p;
  const int centralgal = c->centralgal;
  const int step = c->bin;
  const double dt = c->dt;

  double reff = SF_DISK_RADIUS_FRAC * galaxies[p].DiskScaleRadius; /* S:1273 */
  reff = ffb_tap(t, v, FL_REFF, reff);
  double tdyn = (reff > 0.0 && galaxies[p].Vvir > 0.0) ? reff / galaxies[p].Vvir : 0.0; /* S:1274 */
  tdyn = ffb_tap(t, v, FL_TDYN, tdyn);

  galaxies[p].H2gas = 0.0; /* S:1280 */

  {                                                                                /* S:1284-1292 */
    double atomicH = galaxies[p].ColdGas * HYDROGEN_MASS_FRAC - galaxies[p].H2gas; /* S:1285 */
    t->flag[FF_ATOMICH_FLOORED] = atomicH < 0.0;
    if (atomicH < 0.0) { /* S:1286 */
      atomicH = 0.0;
    }
    atomicH = ffb_tap(t, v, FL_ATOMICH, atomicH);
    double f_ion = tr_ionized_gas_fraction(
        galaxies[p].ColdGas, tr_atomic_disk_radius(galaxies[p].DiskScaleRadius),
        run_params.Hubble_h, SIGMA_HI_CRIT, &t->flag[FF_F_ION_PATH]); /* S:1287-1289 */
    f_ion = ffb_tap(t, v, FL_F_ION, f_ion);
    atomicH *= (1.0 - f_ion);    /* S:1290 */
    galaxies[p].H1gas = atomicH; /* S:1291 */
  }

  const double gas_for_sf = galaxies[p].ColdGas; /* S:1297 */
  double stars;
  t->flag[FF_NONFINITE_GUARD] = isnan(galaxies[p].ColdGas) || isinf(galaxies[p].ColdGas) ||
                                isnan(galaxies[p].Vvir) || isinf(galaxies[p].Vvir) || isnan(reff) ||
                                isinf(reff) || isnan(tdyn) || isinf(tdyn);
  if (isnan(galaxies[p].ColdGas) || isinf(galaxies[p].ColdGas) || isnan(galaxies[p].Vvir) ||
      isinf(galaxies[p].Vvir) || isnan(reff) || isinf(reff) || isnan(tdyn) ||
      isinf(tdyn)) {                           /* S:1303-1305 */
    stars = 0.0;                               /* S:1306 */
  } else if (tdyn > 0.0 && gas_for_sf > 0.0) { /* S:1307 */
    t->flag[FF_TDYN_GAS_POSITIVE] = 1;
    const double epsilon_ffb = run_params.EnhancedSFEfficiency; /* S:1308 */
    double strdot = epsilon_ffb * gas_for_sf / tdyn;            /* S:1309 */
    strdot = ffb_tap(t, v, FL_STRDOT, strdot);
    t->flag[FF_STRDOT_REJECTED] = isnan(strdot) || isinf(strdot) || strdot < 0.0;
    if (isnan(strdot) || isinf(strdot) || strdot < 0.0) { /* S:1311 */
      stars = 0.0;
    } else {
      stars = strdot * dt; /* S:1314 */
      t->flag[FF_STARS_CAP_BINDS] = stars > galaxies[p].ColdGas;
      if (stars > galaxies[p].ColdGas) /* S:1315 */
        stars = galaxies[p].ColdGas;
      t->flag[FF_STARS_REJECTED] = isnan(stars) || isinf(stars) || stars < 0.0;
      if (isnan(stars) || isinf(stars) || stars < 0.0) /* S:1316 */
        stars = 0.0;
    }
  } else {
    t->flag[FF_TDYN_GAS_POSITIVE] = 0;
    stars = 0.0; /* S:1319 */
  }
  stars = ffb_tap(t, v, FL_STARS, stars);

  galaxies[p].SfrDisk[step] += stars / dt;                            /* S:1325 */
  galaxies[p].SfrDiskColdGas[step] = galaxies[p].ColdGas;             /* S:1326 */
  galaxies[p].SfrDiskColdGasMetals[step] = galaxies[p].MetalsColdGas; /* S:1327 */

  double metallicity = get_metallicity(galaxies[p].ColdGas, galaxies[p].MetalsColdGas); /* S:1329 */
  metallicity = ffb_tap(t, v, FL_METALLICITY, metallicity);
  const float h2_before = galaxies[p].H2gas;
  const float h1_before = galaxies[p].H1gas;
  update_from_star_formation(p, stars, metallicity, galaxies, &run_params); /* S:1330 */
  /* Its caps (S:1131-1135), observed and checked: each capped field is max_h, else as it was */
  t->flag[FF_SF_CAPS_TRACKED] = sf_prescription_tracks_h2(run_params.SFprescription);
  if (t->flag[FF_SF_CAPS_TRACKED]) {
    const float max_h =
        (galaxies[p].ColdGas > 0.0f) ? galaxies[p].ColdGas * HYDROGEN_MASS_FRAC : 0.0f;
    t->flag[FF_SF_H2_CAP_BINDS] = h2_before > max_h;
    t->flag[FF_SF_H1_CAP_BINDS] = h1_before > max_h;
    if (!same_f64(galaxies[p].H2gas, t->flag[FF_SF_H2_CAP_BINDS] ? max_h : h2_before) ||
        !same_f64(galaxies[p].H1gas, t->flag[FF_SF_H1_CAP_BINDS] ? max_h : h1_before)) {
      fatal("the observed star-formation hydrogen caps disagree with SAGE26's commit");
    }
  }
  /* S:1332-1337: SaveFullSFH is 0 (required by the reader) */
  t->metallicity_post = get_metallicity(galaxies[p].ColdGas, galaxies[p].MetalsColdGas);
  t->coldgas_after_commit = galaxies[p].ColdGas;

  if (v->recompute_metallicity) {
    /* Observation only: the ordinary path's recomputation (S:1076), not SAGE26's FFB path */
    metallicity = get_metallicity(galaxies[p].ColdGas, galaxies[p].MetalsColdGas);
  }

  double reheated_mass = 0.0; /* S:1342 */
  double ejected_mass = 0.0;  /* S:1343 */
  double stars_fb = stars;
  tr_compute_sn_feedback_ffb(p, &stars_fb, &reheated_mass, &ejected_mass, t, v); /* S:1344 */
  if (t->out_of_domain) {
    return;
  }

  t->flag[FF_REHEAT_CLAMP_FFB_BINDS] = reheated_mass > galaxies[p].ColdGas;
  if (reheated_mass > galaxies[p].ColdGas) { /* S:1346 */
    reheated_mass = galaxies[p].ColdGas;
  }
  reheated_mass = ffb_tap(t, v, FL_REHEATED_COMMIT, reheated_mass);

  if (v->uff_transcribed) { /* S:1348 */
    tr_update_from_feedback(p, centralgal, reheated_mass, ejected_mass, metallicity, t, v);
    if (t->out_of_domain) {
      return;
    }
  } else {
    double clamped = reheated_mass;
    if (clamped > galaxies[p].ColdGas) {
      clamped = galaxies[p].ColdGas;
    }
    if (!(clamped >= 0.0) || !(clamped <= galaxies[p].ColdGas)) { /* S:1158-1163 would abort */
      t->out_of_domain = 1;
      return;
    }
    update_from_feedback(p, centralgal, reheated_mass, ejected_mass, metallicity, galaxies,
                         &run_params);
  }

  const double yield_stars = v->yield_with_formed_stars ? stars : stars_fb;
  t->flag[FF_YIELD_ABOVE_THRESHOLD] = galaxies[p].ColdGas > 1.0e-8;
  t->flag[FF_YIELD_RESERVOIR_CGM] =
      run_params.CGMrecipeOn == 1 && galaxies[centralgal].Regime == 0; /* X:69 */
  if (galaxies[p].ColdGas > 1.0e-8) {                                  /* S:1353 */
    double FracZleaveDiskVal =
        run_params.FracZleaveDisk * exp(-1.0 * galaxies[centralgal].Mvir / KD11_METAL_HALO_MASS);
    FracZleaveDiskVal = ffb_tap(t, v, FL_FRAC_Z, FracZleaveDiskVal); /* S:1354-1355 */
    galaxies[p].MetalsColdGas +=
        run_params.Yield * (1.0 - FracZleaveDiskVal) * yield_stars;                  /* S:1356 */
    double metals_leaving_disk = run_params.Yield * FracZleaveDiskVal * yield_stars; /* S:1358 */
    metals_leaving_disk = ffb_tap(t, v, FL_YIELD_TO_HOT, metals_leaving_disk);
    add_metals_to_hot_reservoir(&galaxies[centralgal], &run_params,
                                metals_leaving_disk); /* S:1359 */
  } else {
    double all_metals = run_params.Yield * yield_stars; /* S:1361 */
    all_metals = ffb_tap(t, v, FL_YIELD_TO_HOT, all_metals);
    add_metals_to_hot_reservoir(&galaxies[centralgal], &run_params, all_metals); /* S:1362 */
  }
  /* S:1365-1367: no disk instability */
}

static struct GALAXY ffb_input[FFB_MAX_GALAXIES];
static struct GALAXY ffb_reference[FFB_MAX_GALAXIES]; /* SAGE26's own end state */

/** @brief Whether the composition's end state is SAGE26's, byte for byte */
static int ffb_reproduces(const struct ffb_call *c) {
  return memcmp(galaxies, ffb_reference, sizeof(struct GALAXY) * (size_t)c->ngal) == 0;
}

/** @brief Read the call, the flags, the parameters and the galaxies */
static void ffb_read_call(struct ffb_call *c) {
  /* The flags the FFB path reads (S:914, :927, :949, :1131, :1169, :1175, :1332, X:69) */
  run_params.SupernovaRecipeOn = in_i32("params.SupernovaRecipeOn");
  run_params.FIREmodeOn = in_i32("params.FIREmodeOn");
  if (run_params.SupernovaRecipeOn != 1 || run_params.FIREmodeOn != 1) {
    fatal("params.SupernovaRecipeOn and params.FIREmodeOn must be 1: only the FIRE branch of "
          "compute_sn_feedback_ffb is transcribed (its fixed-epsilon branch is on no rung)");
  }
  run_params.SFprescription = in_i32("params.SFprescription");
  if (run_params.SFprescription != 0 && run_params.SFprescription != 1) {
    fatal("params.SFprescription = %d: only 0 and 1 are ported", run_params.SFprescription);
  }
  run_params.CGMrecipeOn = in_i32("params.CGMrecipeOn");
  if (run_params.CGMrecipeOn != 0 && run_params.CGMrecipeOn != 1) {
    fatal("params.CGMrecipeOn = %d is not 0 or 1", run_params.CGMrecipeOn);
  }
  run_params.SaveFullSFH = in_i32("params.SaveFullSFH");
  if (run_params.SaveFullSFH != 0) {
    fatal("params.SaveFullSFH must be 0 (off on every rung, R6.par:60)");
  }
  if (c->wrapper) {
    run_params.EnhancedStarFormationOn = in_i32("params.EnhancedStarFormationOn");
    if (run_params.EnhancedStarFormationOn != 1) {
      fatal("params.EnhancedStarFormationOn must be 1 for the wrapper's FFB dispatch");
    }
    run_params.G = in_f64("params.G"); /* the instability observation only */
  }
  run_params.EnhancedSFEfficiency = in_f64("params.EnhancedSFEfficiency");
  run_params.RecycleFraction = in_f64("params.RecycleFraction");
  run_params.FeedbackReheatingEpsilon = in_f64("params.FeedbackReheatingEpsilon");
  run_params.FeedbackEjectionEfficiency = in_f64("params.FeedbackEjectionEfficiency");
  run_params.EtaSNcode = in_f64("params.EtaSNcode");
  run_params.EnergySNcode = in_f64("params.EnergySNcode");
  run_params.Yield = in_f64("params.Yield");
  run_params.FracZleaveDisk = in_f64("params.FracZleaveDisk");
  run_params.Hubble_h = in_f64("params.Hubble_h");

  c->ngal = in_i32("call.ngal");
  c->p = in_i32("call.p");
  c->centralgal = in_i32("call.centralgal");
  c->step = in_i32("call.step");
  c->effective_steps = in_i32("call.effective_steps");
  c->deltaT = in_f64("call.deltaT");
  c->dt = in_f64("call.dt");
  c->time = c->wrapper ? in_f64("call.time") : NAN;
  if (c->ngal < 1 || c->ngal > FFB_MAX_GALAXIES || c->p < 0 || c->p >= c->ngal ||
      c->centralgal < 0 || c->centralgal >= c->ngal || c->effective_steps < 1 || c->step < 0 ||
      c->step >= c->effective_steps) {
    fatal("call indices, effective_steps or step out of range");
  }
  /* The substep interval and its history bin as the driver forms them (B:513-516) */
  if (!same_f64(c->dt, c->deltaT / c->effective_steps) || !(c->dt > 0.0)) {
    fatal("call.dt must be call.deltaT / call.effective_steps and positive");
  }
  c->bin = (c->step * STEPS) / c->effective_steps;
  if (c->bin >= STEPS) {
    c->bin = STEPS - 1;
  }

  for (int i = 0; i < c->ngal; i++) {
    struct GALAXY *g = &galaxies[i];
    g->Type = in_i32(field_name("galaxy[%d].Type", i));
    g->SnapNum = in_i32(field_name("galaxy[%d].SnapNum", i));
    g->Regime = in_i32(field_name("galaxy[%d].Regime", i));
    g->FFBRegime = in_i32(field_name("galaxy[%d].FFBRegime", i));
    for (size_t m = 0; m < FFB_COUNT(FFB_HALO); m++) {
      ffb_set(g, &FFB_HALO[m], 0, in_f32(ffb_member_field("", i, &FFB_HALO[m], 0)));
    }
    for (size_t m = 0; m < FFB_COUNT(FFB_STATE); m++) {
      for (int k = 0; k < FFB_STATE[m].elements; k++) {
        ffb_set(g, &FFB_STATE[m], k, in_f32(ffb_member_field("", i, &FFB_STATE[m], k)));
      }
    }
  }
  if (c->wrapper && galaxies[c->p].FFBRegime != 1) {
    fatal("galaxy[call.p].FFBRegime must be 1: the routine is the wrapper's FFB dispatch");
  }
  /* The stored snapshot's redshift, ZZ[SnapNum] (S:915): only the forming galaxy's is read */
  const int snap = galaxies[c->p].SnapNum;
  if (snap < 0 || snap >= ABSOLUTEMAXSNAPS) {
    fatal("galaxy[call.p].SnapNum = %d is outside SAGE26's ZZ table", snap);
  }
  run_params.ZZ[snap] = in_f64(field_name("params.ZZ[%d]", snap));
}

/** @brief Write one carried value with its markers, checked and two-sided tested */
static void ffb_record_local(const enum ffb_local id, const struct ffb_trace *t,
                             const struct ffb_call *c, const int zero_stars) {
  const char *name = FFB_LOCALS[id].name;
  out_i32(field_name("intermediate.%s_absent", name), !t->present[id]);
  if (!t->present[id]) {
    return;
  }
  const double value = t->value[id];
  /* Checked: the composition from the recorded value (the plain composition, in which the value
   * is the one recorded) reproduces SAGE26's end state; the caller has verified it */
  int changed[2];
  for (int d = 0; d < 2; d++) {
    struct ffb_variant v = {(int)id, d == 0 ? +1 : -1, FFB_LOCALS[id].uff_local, 0, 0};
    struct ffb_trace scratch;
    tr_starformation_ffb(ffb_input, c, &scratch, &v);
    changed[d] = !scratch.out_of_domain && !ffb_reproduces(c);
  }
  const int narrowing_limited = FFB_LOCALS[id].loading_chain && zero_stars;
  out_f64(field_name("intermediate.%s", name), value);
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), FFB_LOCALS[id].kind);
  out_i32(field_name("intermediate.%s_validated_up", name), changed[0] && !isnan(value));
  out_i32(field_name("intermediate.%s_validated_down", name), changed[1] && !isnan(value));
  out_i32(field_name("intermediate.%s_validated", name),
          changed[0] && changed[1] && !isnan(value) && !narrowing_limited);
  out_i32(field_name("intermediate.%s_narrowing_limited", name), narrowing_limited);
}

/** @brief Whether any byte of the galaxies other than the recorded state changed */
static int ffb_changed_unrecorded(const struct ffb_call *c) {
  static struct GALAXY a[FFB_MAX_GALAXIES];
  memcpy(a, ffb_input, sizeof(struct GALAXY) * (size_t)c->ngal);
  for (int i = 0; i < c->ngal; i++) {
    for (size_t m = 0; m < FFB_COUNT(FFB_STATE); m++) {
      for (int k = 0; k < FFB_STATE[m].elements; k++) {
        ffb_set(&a[i], &FFB_STATE[m], k, ffb_get(&ffb_reference[i], &FFB_STATE[m], k));
      }
    }
  }
  return memcmp(a, ffb_reference, sizeof(struct GALAXY) * (size_t)c->ngal) != 0;
}

/** @brief The one routine body: starformation_ffb, or the wrapper's dispatch to it */
static void run_ffb(const int wrapper) {
  struct ffb_call c;
  memset(&c, 0, sizeof(c));
  c.wrapper = wrapper;
  ffb_read_call(&c);
  check_all_fields_used();
  memcpy(ffb_input, galaxies, sizeof(struct GALAXY) * (size_t)c.ngal);

  /* 1. SAGE26's own call */
  if (wrapper) {
    starformation_and_feedback(c.p, c.centralgal, c.time, c.dt, 0, c.bin, galaxies, &run_params);
  } else {
    starformation_ffb(c.p, c.centralgal, c.dt, c.bin, galaxies, &run_params);
  }
  memcpy(ffb_reference, galaxies, sizeof(struct GALAXY) * (size_t)c.ngal);
  if (ffb_changed_unrecorded(&c)) {
    fatal("SAGE26's call changed a galaxy member outside the recorded state");
  }
  if (wrapper) {
    /* The dispatch: the wrapper's end state is starformation_ffb's on the same input */
    memcpy(galaxies, ffb_input, sizeof(struct GALAXY) * (size_t)c.ngal);
    starformation_ffb(c.p, c.centralgal, c.dt, c.bin, galaxies, &run_params);
    if (!ffb_reproduces(&c)) {
      fatal("the wrapper's end state differs from starformation_ffb's on the same input");
    }
  }

  /* 2. The transcription must reproduce it, with SAGE26's own update_from_feedback and with its
   *    transcription */
  struct ffb_trace t, t_uff;
  tr_starformation_ffb(ffb_input, &c, &t, &FFB_PLAIN);
  if (t.out_of_domain || !ffb_reproduces(&c)) {
    fatal("the transcription does not reproduce SAGE26's end state");
  }
  const struct ffb_variant uff = {-1, 0, 1, 0, 0};
  tr_starformation_ffb(ffb_input, &c, &t_uff, &uff);
  if (t_uff.out_of_domain || !ffb_reproduces(&c)) {
    fatal("the transcription with update_from_feedback transcribed does not reproduce SAGE26");
  }
  for (int k = 0; k < FL_COUNT; k++) {
    if (!FFB_LOCALS[k].uff_local && (t.present[k] != t_uff.present[k] ||
                                     (t.present[k] && !same_f64(t.value[k], t_uff.value[k])))) {
      fatal("the two compositions disagree on %s", FFB_LOCALS[k].name);
    }
  }
  for (int k = 0; k < FL_COUNT; k++) {
    if (FFB_LOCALS[k].uff_local) {
      t.value[k] = t_uff.value[k];
      t.present[k] = t_uff.present[k];
    }
  }
  const enum ffb_flag uff_flags[] = {
      FF_UFF_REHEAT_CLAMP_BINDS, FF_FEEDBACK_RESERVOIR_CGM, FF_FB_CAPS_TRACKED, FF_FB_H2_CAP_BINDS,
      FF_FB_H1_CAP_BINDS,        FF_EJECTED_CAP_BINDS,      FF_METALS_CAP_BINDS};
  for (size_t k = 0; k < FFB_COUNT(uff_flags); k++) {
    t.flag[uff_flags[k]] = t_uff.flag[uff_flags[k]];
  }

  /* 3. Observations (not SAGE26 state): the feedback with the post-commit metallicity, and the
   *    yield from the stars formed, each changes the end state or not */
  struct ffb_trace scratch;
  const struct ffb_variant recompute = {-1, 0, 0, 1, 0};
  tr_starformation_ffb(ffb_input, &c, &scratch, &recompute);
  const int recompute_changes = scratch.out_of_domain || !ffb_reproduces(&c);
  const struct ffb_variant formed = {-1, 0, 0, 0, 1};
  tr_starformation_ffb(ffb_input, &c, &scratch, &formed);
  const int formed_changes = scratch.out_of_domain || !ffb_reproduces(&c);

  /* 4. The record: SAGE26's end state, every galaxy */
  memcpy(galaxies, ffb_reference, sizeof(struct GALAXY) * (size_t)c.ngal);
  out_i32("intermediate.step_bin", c.bin);
  for (int i = 0; i < c.ngal; i++) {
    for (size_t m = 0; m < FFB_COUNT(FFB_STATE); m++) {
      for (int k = 0; k < FFB_STATE[m].elements; k++) {
        out_f32(ffb_member_field("", i, &FFB_STATE[m], k),
                ffb_get(&ffb_reference[i], &FFB_STATE[m], k));
      }
    }
    out_i32(field_name("galaxy[%d].Regime", i), ffb_reference[i].Regime);
    out_i32(field_name("galaxy[%d].FFBRegime", i), ffb_reference[i].FFBRegime);
  }
  for (int k = 0; k < FF_COUNT; k++) {
    out_i32(field_name("intermediate.%s", FFB_FLAG_NAMES[k]), t.flag[k]);
  }
  const int zero_stars = t.present[FL_STARS] && t.value[FL_STARS] == 0.0;
  for (int k = 0; k < FL_COUNT; k++) {
    ffb_record_local((enum ffb_local)k, &t, &c, zero_stars);
  }
  /* Observation: the cold-gas metallicity after the commit, which the ordinary path would
   * recompute (S:1076) and the FFB path does not (S:1348) */
  out_f64("intermediate.metallicity_post_commit", t.metallicity_post);
  out_i32("intermediate.metallicity_post_commit_differs",
          !same_f64(t.metallicity_post, t.value[FL_METALLICITY]));
  /* SAGE26's own state at that point (its update_from_star_formation's result in the plain
   * composition, which reproduces SAGE26's end state) */
  out_f32("intermediate.coldgas_after_sf_commit", t.coldgas_after_commit);
  out_i32("intermediate.recomputed_metallicity_changes_state", recompute_changes);
  out_i32("intermediate.yield_with_formed_stars_changes_state", formed_changes);
  if (wrapper) {
    /* The forming galaxy's disk after the call, by the Mo, Mao & White (1998) criterion of
     * check_disk_instability (R/src/model_disk_instability.c:54-57), which the FFB return never
     * reaches (S:1007-1008) */
    const struct GALAXY *g = &ffb_reference[c.p];
    const double diskmass = g->ColdGas + (g->StellarMass - g->BulgeMass);
    const double Mcrit =
        g->Vmax * g->Vmax * (TOOMRE_DISK_FACTOR * g->DiskScaleRadius) / run_params.G;
    out_i32("intermediate.wrapper_equals_starformation_ffb", 1);
    out_i32("intermediate.disk_unstable_after_call", diskmass > 0.0 && Mcrit < diskmass);
  }
}

static void run_starformation_ffb(void) { run_ffb(0); }

static void run_starformation_and_feedback_ffb(void) { run_ffb(1); }

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
    {"starformation_ffb", run_starformation_ffb},
    {"starformation_and_feedback_ffb", run_starformation_and_feedback_ffb},
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
