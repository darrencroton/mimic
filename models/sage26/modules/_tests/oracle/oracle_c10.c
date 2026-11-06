/**
 * @file    oracle_c10.c
 * @brief   SAGE26 function-level oracle for the c10 CGM regime classifier
 *          (determine_and_store_regime, CGMrecipeOn 1)
 *
 * Reads one fixture input file, initialises the SAGE26 struct GALAXY array and the struct
 * params fields its reader consumes, calls SAGE26's public determine_and_store_regime
 * (R/src/model_regimes.c:63-100, declared at R/src/model_regimes.h:19-20; R/ is the staged
 * SAGE26 source root) from the SAGE26 objects this executable is linked against, and writes a
 * fixture output file holding every row's Regime, which rows consumed a draw and in what order,
 * and the routine's locals.
 *
 * Usage: oracle_c10 <fixture.in> <fixture.out>
 *
 * Routine (the `routine` line of the input selects it):
 *   determine_and_store_regime   R/src/model_regimes.c:63-100, called directly with the
 *                                fixture's galaxies, as SAGE26's driver calls it once per FoF
 *                                with CGMrecipeOn 1 (R/src/core_build_model.c:410-411)
 *
 * THE TEST-ONLY rand() PROVIDER. SAGE26's draw is libc rand() (R/src/model_regimes.c:94). This
 * file defines rand() itself, so the linker resolves the SAGE26 objects' calls to it instead of
 * to libc's (a definition in a linked object takes precedence over the dynamic library): it
 * returns the fixture's explicit integer queue (rand.queue[k]) in order, refuses a call when the
 * queue is exhausted or not armed, and counts the calls. SAGE26's source is not edited. The
 * provider is linked only into this oracle main (build.sh), never into another oracle main or
 * any Mimic binary. Every fixture must consume its whole queue, so its draw count is exact.
 *
 * Which rows consume a draw is observed from SAGE26's own code: after the call over all rows,
 * the oracle calls determine_and_store_regime once per row (ngal 1, the row's own address) on a
 * fresh copy of the input, with the queue rewound, and records the queue entries each call
 * consumed. The loop body (:67-98) shares no state between rows other than the rand() stream, so
 * the per-row calls must reproduce the full call's Regime for every row and its queue order; the
 * oracle refuses a fixture otherwise.
 *
 * The routine's locals (Mshock, mass_ratio, x, hot_fraction and the deviate) cannot be read from
 * SAGE26's call, so they come from a SOURCE-REVIEWED TRANSCRIPTION below
 * (transcribe_determine_and_store_regime), each statement cited to its line, which takes its
 * deviates from the same queue with SAGE26's expression (double)q / (double)RAND_MAX. The oracle
 * refuses a fixture unless the transcription reproduces SAGE26's Regime for every row, the
 * same rows drawing in the same order, so every recorded local is TAIL-checked: the
 * transcription runs from it to the routine's end and reproduces SAGE26's result. The recorded
 * Regime and draw consumption come from SAGE26's own calls; the recorded locals from the
 * transcription. The routine writes only Regime: the oracle also refuses a fixture unless every
 * other byte of every galaxy is unchanged by SAGE26's call (classification moves no gas).
 *
 * The transcription restates SAGE26's statements with the same expression shapes, operand order
 * and types and is compiled with the same flags. None of them has a multiply feeding an add or
 * subtract, so contraction fuses none, here or in SAGE26's object (README.md, "The c10 Group").
 *
 * The file format is defined in README.md ("Fixture File Format"). The parsing and output
 * helpers are copies of oracle_c08.c's, which stays unchanged.
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
#define MAX_GALAXIES 32
#define MAX_QUEUE 32
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
  fprintf(stderr, "oracle_c10: %s: ", input_path != NULL ? input_path : "(no input)");
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

// ============================================================================
// THE TEST-ONLY rand() PROVIDER
// ============================================================================

static int rand_queue[MAX_QUEUE];
static int rand_queue_length = 0;
static int rand_next = 0;  /* index of the next queue entry rand() returns */
static int rand_armed = 0; /* 1 only around the SAGE26 calls */

/**
 * @brief The fixture's queue in place of libc rand(), for SAGE26's objects
 *
 * Linked only into oracle_c10. Refuses a call outside a SAGE26 call or past the queue's end, so
 * a fixture's queue states exactly the draws SAGE26 makes.
 */
int rand(void) {
  if (!rand_armed) {
    fatal("rand() called outside a determine_and_store_regime call");
  }
  if (rand_next >= rand_queue_length) {
    fatal("rand() called %d times, but the fixture's queue holds %d entries (rand.queue_length)",
          rand_next + 1, rand_queue_length);
  }
  return rand_queue[rand_next++];
}

// ============================================================================
// STATE INITIALISATION AND THE INPUT READER
// ============================================================================

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(&run_params, 0xff, sizeof(run_params));
}

/**
 * @brief Read the call: the two params, the galaxies' four fields and the rand() queue
 *
 * determine_and_store_regime reads mergeType and Mvir of each row and params.MShockMsun and
 * params.Hubble_h (R/src/model_regimes.c:67-74), and writes Regime (:98). Type is read for the
 * Mimic side (the package skips a row by Type 3), and Regime's input value shows that a skipped
 * row keeps it.
 *
 * @return The number of galaxies
 */
static int read_call(void) {
  run_params.MShockMsun = in_f64("params.MShockMsun");
  run_params.Hubble_h = in_f64("params.Hubble_h");

  const int ngal = in_i32("call.ngal");
  if (ngal < 1 || ngal > MAX_GALAXIES) {
    fatal("call.ngal = %d is outside 1..%d", ngal, MAX_GALAXIES);
  }
  for (int i = 0; i < ngal; i++) {
    galaxies[i].Type = in_i32(field_name("galaxy[%d].Type", i));
    galaxies[i].mergeType = in_i32(field_name("galaxy[%d].mergeType", i));
    galaxies[i].Mvir = in_f32(field_name("galaxy[%d].Mvir", i));
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
  return ngal;
}

// ============================================================================
// SOURCE-REVIEWED TRANSCRIPTION OF SAGE26 SOURCE (model_regimes.c)
// ============================================================================

/** One row's record from the transcription */
struct row_trace {
  int skipped;         /* mergeType > 0 (:67) */
  int drew;            /* took the draw branch (:79-95) */
  int draw_index;      /* the queue entry it consumed, -1 without one */
  int regime;          /* the Regime it writes (:98); the input value for a skipped row */
  double mshock;       /* :71 */
  double mass_ratio;   /* :74 */
  double x;            /* :85 */
  double hot_fraction; /* :89 */
  double deviate;      /* :94 */
};

/**
 * @brief TRANSCRIPTION of determine_and_store_regime, R/src/model_regimes.c:63-100
 *
 * Takes its deviates from the fixture's queue in order, with SAGE26's own expression
 * (double)rand() / (double)RAND_MAX (:94) applied to the queue value, and never calls rand().
 *
 * @return The number of queue entries consumed
 */
static int transcribe_determine_and_store_regime(const int ngal, const struct GALAXY *g,
                                                 const struct params *rp, struct row_trace *tr) {
  int next = 0;
  /* :66 */
  for (int p = 0; p < ngal; p++) {
    tr[p].skipped = 0;
    tr[p].drew = 0;
    tr[p].draw_index = -1;
    tr[p].regime = g[p].Regime;
    tr[p].mshock = NAN;
    tr[p].mass_ratio = NAN;
    tr[p].x = NAN;
    tr[p].hot_fraction = NAN;
    tr[p].deviate = NAN;

    /* :67 */
    if (g[p].mergeType > 0) {
      tr[p].skipped = 1;
      continue;
    }

    /* :71 */
    const double Mshock = MSUN_TO_CODE_MASS(rp->MShockMsun, rp->Hubble_h);
    tr[p].mshock = Mshock;

    /* :74 */
    const double mass_ratio = g[p].Mvir / Mshock;
    tr[p].mass_ratio = mass_ratio;

    /* :76-78 */
    int32_t new_regime;
    if (mass_ratio <= 0.0) {
      new_regime = 0;
    } else {
      /* :82 */
      const double delta_log_M = 0.1;

      /* :85 */
      const double x = log10(mass_ratio) / delta_log_M;
      tr[p].x = x;

      /* :89 */
      const double hot_fraction = 1.0 / (1.0 + exp(-x));
      tr[p].hot_fraction = hot_fraction;

      /* :94, with the queue value in place of rand() */
      if (next >= rand_queue_length) {
        fatal("the transcription needs more than the %d queue entries", rand_queue_length);
      }
      tr[p].draw_index = next;
      const double random_uniform = (double)rand_queue[next++] / (double)RAND_MAX;
      tr[p].deviate = random_uniform;
      tr[p].drew = 1;

      /* :95 */
      new_regime = (random_uniform < hot_fraction) ? 1 : 0;
    }

    /* :98 */
    tr[p].regime = new_regime;
  }
  return next;
}

// ============================================================================
// THE ROUTINE
// ============================================================================

/** @brief Whether SAGE26's call changed any byte of a galaxy other than its Regime */
static int changed_other_than_regime(const struct GALAXY *before, const struct GALAXY *after) {
  /* Byte copies, so padding is compared as SAGE26's call left it */
  static struct GALAXY a, b;
  memcpy(&a, before, sizeof(a));
  memcpy(&b, after, sizeof(b));
  a.Regime = 0;
  b.Regime = 0;
  return memcmp(&a, &b, sizeof(a)) != 0;
}

static void run_determine_and_store_regime(void) {
  const int ngal = read_call();
  check_all_fields_used();

  static struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* 1. SAGE26's call over every row, as its driver makes it (R/src/core_build_model.c:411) */
  rand_next = 0;
  rand_armed = 1;
  determine_and_store_regime(ngal, galaxies, &run_params);
  rand_armed = 0;
  const int draws_consumed = rand_next;
  if (draws_consumed != rand_queue_length) {
    fatal("SAGE26's call consumed %d of the %d queue entries: a fixture's queue must state "
          "exactly its draws",
          draws_consumed, rand_queue_length);
  }
  static struct GALAXY full_call[MAX_GALAXIES];
  memcpy(full_call, galaxies, sizeof(full_call));
  for (int i = 0; i < ngal; i++) {
    if (changed_other_than_regime(&input[i], &full_call[i])) {
      fatal("SAGE26's call changed galaxy[%d] beyond its Regime", i);
    }
  }

  /* 2. SAGE26's call on each row alone, from the input, with the queue rewound: which rows
   *    consume a draw, and in what order */
  int row_draw_index[MAX_GALAXIES];
  memcpy(galaxies, input, sizeof(input));
  rand_next = 0;
  for (int i = 0; i < ngal; i++) {
    const int before = rand_next;
    rand_armed = 1;
    determine_and_store_regime(1, &galaxies[i], &run_params);
    rand_armed = 0;
    const int consumed = rand_next - before;
    if (consumed != 0 && consumed != 1) {
      fatal("SAGE26's call on galaxy[%d] alone consumed %d draws", i, consumed);
    }
    row_draw_index[i] = consumed ? before : -1;
    if (galaxies[i].Regime != full_call[i].Regime) {
      fatal("SAGE26's call on galaxy[%d] alone wrote Regime %d, the full call %d", i,
            galaxies[i].Regime, full_call[i].Regime);
    }
  }
  if (rand_next != draws_consumed) {
    fatal("the per-row calls consumed %d draws, the full call %d", rand_next, draws_consumed);
  }

  /* 3. The transcription: the same Regime, the same rows drawing the same queue entries */
  struct row_trace tr[MAX_GALAXIES];
  const int transcribed_draws = transcribe_determine_and_store_regime(ngal, input, &run_params, tr);
  if (transcribed_draws != draws_consumed) {
    fatal("the transcription consumed %d draws, SAGE26 %d", transcribed_draws, draws_consumed);
  }
  for (int i = 0; i < ngal; i++) {
    if (tr[i].regime != full_call[i].Regime || tr[i].draw_index != row_draw_index[i]) {
      fatal("the transcription disagrees with SAGE26 on galaxy[%d]: Regime %d against %d, "
            "queue entry %d against %d",
            i, tr[i].regime, full_call[i].Regime, tr[i].draw_index, row_draw_index[i]);
    }
    if (tr[i].skipped != (input[i].mergeType > 0) || tr[i].drew != (row_draw_index[i] >= 0)) {
      fatal("the transcription's branch flags disagree with SAGE26 on galaxy[%d]", i);
    }
  }

  /* 4. The record */
  out_i32("intermediate.RAND_MAX", RAND_MAX);
  out_i32("intermediate.draws_consumed", draws_consumed);
  for (int i = 0; i < ngal; i++) {
    out_i32(field_name("galaxy[%d].Regime", i), full_call[i].Regime);
    out_i32(field_name("intermediate.galaxy[%d].skipped", i), tr[i].skipped);
    out_i32(field_name("intermediate.galaxy[%d].drew", i), tr[i].drew);
    out_i32(field_name("intermediate.galaxy[%d].draw_index", i), row_draw_index[i]);
    if (tr[i].skipped) {
      continue;
    }
    out_f64(field_name("intermediate.galaxy[%d].Mshock", i), tr[i].mshock);
    out_f64(field_name("intermediate.galaxy[%d].mass_ratio", i), tr[i].mass_ratio);
    if (!tr[i].drew) {
      continue;
    }
    out_f64(field_name("intermediate.galaxy[%d].x", i), tr[i].x);
    out_f64(field_name("intermediate.galaxy[%d].hot_fraction", i), tr[i].hot_fraction);
    out_f64(field_name("intermediate.galaxy[%d].deviate", i), tr[i].deviate);
    out_i32(field_name("intermediate.galaxy[%d].deviate_below_hot_fraction", i),
            tr[i].deviate < tr[i].hot_fraction);
    out_i32(field_name("intermediate.galaxy[%d].deviate_equals_hot_fraction", i),
            same_f64(tr[i].deviate, tr[i].hot_fraction));
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
    {"determine_and_store_regime", run_determine_and_store_regime},
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
          "# SAGE26 oracle output for %s, written by oracle_c10 (regenerate.sh); do not edit.\n",
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
