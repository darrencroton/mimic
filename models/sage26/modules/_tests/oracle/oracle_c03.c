/**
 * @file    oracle_c03.c
 * @brief   SAGE26 function-level oracle for the c03 baryon-supply routines
 *
 * Reads one fixture input file, initialises the SAGE26 struct GALAXY, struct
 * halo_data and struct params fields its reader for the selected routine consumes (a
 * superset of the fields the routine reads; galaxy[i].Type, for example, is set for
 * the Mimic side although most routines never read it), reproduces the arithmetic
 * SAGE26's call site (core_build_model.c) performs around that
 * routine, calls the routine from the SAGE26 objects this executable is linked
 * against, and writes a fixture output file holding the routine's return value,
 * every field it writes and every intermediate value the Mimic package carries
 * between modules.
 *
 * Usage: oracle_c03 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one; R/ is the staged SAGE26
 * source root, B = R/src/core_build_model.c, the call site):
 *   do_reionization       R/src/model_infall.c:286-337
 *   infall_recipe         R/src/model_infall.c:67-213, called at B:415
 *   add_infall_to_hot     R/src/model_infall.c:351-414, called at B:488
 *   reincorporate_gas     R/src/model_reincorporation.c:44-66, called at B:490-492
 *   strip_from_satellite  R/src/model_infall.c:225-273, called at B:493-498
 *   get_disk_radius       R/src/model_misc.c:203-229, called at B:297
 *
 * With params.CGMrecipeOn 1 (campaign node c10, "The c10 Group" in README.md) the
 * infall_recipe, add_infall_to_hot, reincorporate_gas and strip_from_satellite readers also
 * take each galaxy's Regime and the CGM pair, and the call is composed from a
 * source-reviewed transcription of SAGE26's CGMrecipeOn 1 path that must reproduce it; its
 * branch flags and locals, with their check kinds and two-sided validation, are recorded.
 * CGMrecipeOn 0 fixtures are read and written exactly as before.
 *
 * The file format is defined in README.md ("Fixture file format"). Every input
 * field must be consumed by the fixture reader for the selected routine, so a
 * misspelt or stray field is an error rather than a silently ignored value. All
 * other struct members are poisoned (all bits set: NaN for floating point, -1 for
 * integers), so a read of an uninitialised member would usually, though not
 * always (a NaN can be compared away or overwritten), surface as NaN output.
 *
 * This is SAGE26 reference tooling: it is compiled and linked only by build.sh,
 * never by Mimic's Makefile, and never against Mimic code.
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
#include "model_infall.h"
#include "model_misc.h"
#include "model_reincorporation.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 512
#define MAX_GALAXIES 8
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
static struct halo_data halos[1];
static struct params run_params;

// ============================================================================
// ERROR HANDLING AND NAMES
// ============================================================================

static void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static void fatal(const char *fmt, ...) {
  va_list args;
  fprintf(stderr, "oracle_c03: %s: ", input_path != NULL ? input_path : "(no input)");
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

/** @brief Name of a per-galaxy field, galaxy[<index>].<member> */
static const char *galaxy_field(const int index, const char *member) {
  return field_name("galaxy[%d].%s", index, member);
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
// OUTPUT
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

// ============================================================================
// STATE INITIALISATION
// ============================================================================

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(halos, 0xff, sizeof(halos));
  memset(&run_params, 0xff, sizeof(run_params));
}

static int read_galaxy_count(void) {
  const int ngal = in_i32("call.ngal");
  if (ngal < 1 || ngal > MAX_GALAXIES) {
    fatal("call.ngal = %d is outside [1, %d]", ngal, MAX_GALAXIES);
  }
  return ngal;
}

static int read_galaxy_index(const char *name, const int ngal) {
  const int index = in_i32(name);
  if (index < 0 || index >= ngal) {
    fatal("%s = %d is outside [0, %d)", name, index, ngal);
  }
  return index;
}

static int read_effective_steps(void) {
  const int steps = in_i32("call.effective_steps");
  if (steps < 1) {
    fatal("call.effective_steps = %d must be positive", steps);
  }
  return steps;
}

/** @brief The params do_reionization reads (R/src/model_infall.c:298-329) */
static void read_reionization_params(void) {
  run_params.a0 = in_f64("params.a0");
  run_params.ar = in_f64("params.ar");
  run_params.Omega = in_f64("params.Omega");
  run_params.OmegaLambda = in_f64("params.OmegaLambda");
  run_params.Hubble = in_f64("params.Hubble");
  run_params.G = in_f64("params.G");
}

/** @brief The reionization selection infall_recipe and strip_from_satellite both perform */
static double reionization_modifier_for(const int gal, const double Zcurr) {
  /* R/src/model_infall.c:122-126 and :230-234 */
  if (run_params.ReionizationOn) {
    return do_reionization(gal, Zcurr, galaxies, &run_params);
  }
  return 1.0;
}

// ============================================================================
// THE C10 GROUP: CGMrecipeOn 1 (REGIME-AWARE ROUTING)
// ============================================================================
//
// With params.CGMrecipeOn 1 a fixture's reader also takes each galaxy's Regime (and, for
// add_infall_to_hot and reincorporate_gas, the CGM pair), the output also records the CGM pair
// where the c03 output did not, and the oracle composes the routine as c06 does: a
// source-reviewed transcription of SAGE26's CGMrecipeOn 1 path, each statement cited (I =
// R/src/model_infall.c, G = R/src/model_reincorporation.c, X = R/src/model_misc.h, B =
// R/src/core_build_model.c), must reproduce SAGE26's own call in every recorded field bit for
// bit, and so must each composition that feeds a continuation-checked local into SAGE26's own
// function; otherwise the fixture is refused. The transcription's branch flags and locals are
// recorded, each local with its check kind and its two-sided validation (D25): whether
// replacing it by its upward, respectively downward, neighbour (nextafter for a double,
// nextafterf for a float local) in its composition changes at least one recorded field.
// Every byte of every galaxy outside the recorded fields must be unchanged by SAGE26's call.
// CGMrecipeOn 0 fixtures (every c03 fixture) are read, run and written exactly as before.

#define C10_CHECK_CONTINUATION 1
#define C10_CHECK_TAIL 2
#define C10_NO_OVERRIDE -1

/** @brief One local replaced in a composition: its index and the replacement value */
struct c10_override {
  int index; /* C10_NO_OVERRIDE for none */
  double value;
};

/** @brief A routine's recorded locals: value, presence, float-ness and check kind */
struct c10_locals {
  int count;
  const char *const *names;
  const int *kinds;
  const int *is_f32;
  double value[8];
  int present[8];
};

static double c10_ov_f64(const struct c10_override *ov, const int index, const double value) {
  return (ov->index == index) ? ov->value : value;
}

static float c10_ov_f32(const struct c10_override *ov, const int index, const float value) {
  return (ov->index == index) ? (float)ov->value : value;
}

static void c10_record(struct c10_locals *l, const int index, const double value) {
  l->value[index] = value;
  l->present[index] = 1;
}

/** @brief A float member of struct GALAXY by offset (every recorded c10 field is a float) */
struct c10_member {
  const char *name;
  size_t offset;
};

#define C10_MEMBER(m) {#m, offsetof(struct GALAXY, m)}

static uint32_t c10_member_bits(const struct GALAXY *g, const struct c10_member *m) {
  uint32_t bits;
  memcpy(&bits, (const char *)g + m->offset, sizeof(bits));
  return bits;
}

/** @brief Whether any recorded member of any of the ngal galaxies differs in its bits */
static int c10_members_differ(const struct GALAXY *a, const struct GALAXY *b, const int ngal,
                              const struct c10_member *members, const size_t count) {
  for (int i = 0; i < ngal; i++) {
    for (size_t k = 0; k < count; k++) {
      if (c10_member_bits(&a[i], &members[k]) != c10_member_bits(&b[i], &members[k])) {
        return 1;
      }
    }
  }
  return 0;
}

/** @brief Refuse the fixture unless SAGE26's call changed nothing but the recorded members */
static void c10_require_rest_unchanged(const struct GALAXY *before, const struct GALAXY *after,
                                       const int ngal, const struct c10_member *members,
                                       const size_t count, const char *routine) {
  for (int i = 0; i < ngal; i++) {
    struct GALAXY masked = after[i];
    for (size_t k = 0; k < count; k++) {
      memcpy((char *)&masked + members[k].offset, (const char *)&before[i] + members[k].offset,
             sizeof(float));
    }
    if (memcmp(&masked, &before[i], sizeof(masked)) != 0) {
      fatal("%s changed galaxy[%d] outside its recorded fields", routine, i);
    }
  }
}

/** @brief The neighbouring value of a local in one direction (+1 up, -1 down) */
static double c10_neighbour(const double value, const int is_f32, const int direction) {
  if (is_f32) {
    return (double)nextafterf((float)value, direction > 0 ? INFINITY : -INFINITY);
  }
  return nextafter(value, direction > 0 ? INFINITY : -INFINITY);
}

/**
 * @brief Write one local: _absent, and if present the value (unless already written), _checked,
 *        _check_kind and the two-sided validation markers
 */
static void c10_out_local(const char *name, const struct c10_locals *l, const int index,
                          const int up, const int down, const int value_written) {
  out_i32(field_name("intermediate.%s_absent", name), l->present[index] ? 0 : 1);
  if (!l->present[index]) {
    return;
  }
  if (!value_written) {
    if (l->is_f32[index]) {
      out_f32(field_name("intermediate.%s", name), (float)l->value[index]);
    } else {
      out_f64(field_name("intermediate.%s", name), l->value[index]);
    }
  }
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), l->kinds[index]);
  out_i32(field_name("intermediate.%s_validated_up", name), up);
  out_i32(field_name("intermediate.%s_validated_down", name), down);
  out_i32(field_name("intermediate.%s_validated", name), up && down);
}

static int read_cgm_recipe_on(void) {
  const int value = in_i32("params.CGMrecipeOn");
  if (value != 0 && value != 1) {
    fatal("params.CGMrecipeOn = %d: SAGE26 accepts only 0 and 1 "
          "(R/src/core_read_parameter_file.c:526)",
          value);
  }
  return value;
}

// ----------------------------------------------------------------------------
// infall_recipe, CGMrecipeOn 1 (I:67-213)
// ----------------------------------------------------------------------------

static const struct c10_member IR_MEMBERS[] = {
    C10_MEMBER(EjectedMass),  C10_MEMBER(MetalsEjectedMass),
    C10_MEMBER(ICS),          C10_MEMBER(MetalsICS),
    C10_MEMBER(ICS_disrupt),  C10_MEMBER(ICS_accrete),
    C10_MEMBER(ICS_sum_mt),   C10_MEMBER(CGMgas),
    C10_MEMBER(MetalsCGMgas), C10_MEMBER(HotGas),
    C10_MEMBER(MetalsHotGas),
};
#define IR_MEMBER_COUNT (sizeof(IR_MEMBERS) / sizeof(IR_MEMBERS[0]))

struct ir_flags {
  int central_regime_cgm;      /* I:164 taken: the central's hot pair folds into its CGM pair */
  int cgm_metals_cap_binds;    /* I:184 */
  int cgm_gas_negative_clears; /* I:188 */
  int cgm_metals_floor_binds;  /* I:192, -1 when I:188 cleared both */
};

/** @brief Transcription of infall_recipe's CGMrecipeOn 1 path; returns infallingMass */
static double transcribe_infall_recipe_cgm(const int centralgal, const int ngal, const double Zcurr,
                                           struct GALAXY *g, struct ir_flags *f) {
  double tot_stellarMass, tot_BHMass, tot_coldMass, tot_hotMass, tot_ejected, tot_ICS;
  double tot_ejectedMetals, tot_ICSMetals;
  double infallingMass, reionization_modifier;
  double tot_CGMgas, tot_MetalsCGMgas;

  /* I:75 */
  tot_stellarMass = tot_coldMass = tot_hotMass = tot_ejected = tot_BHMass = tot_ejectedMetals =
      tot_ICS = tot_ICSMetals = tot_CGMgas = tot_MetalsCGMgas = 0.0;

  for (int i = 0; i < ngal; i++) {               /* I:78 */
    tot_stellarMass += g[i].StellarMass;         /* I:79 */
    tot_BHMass += g[i].BlackHoleMass;            /* I:80 */
    tot_coldMass += g[i].ColdGas;                /* I:81 */
    tot_hotMass += g[i].HotGas;                  /* I:82 */
    tot_ejected += g[i].EjectedMass;             /* I:83 */
    tot_ejectedMetals += g[i].MetalsEjectedMass; /* I:84 */
    tot_ICS += g[i].ICS;                         /* I:85 */
    tot_ICSMetals += g[i].MetalsICS;             /* I:86 */
    tot_CGMgas += g[i].CGMgas;                   /* I:87 */
    tot_MetalsCGMgas += g[i].MetalsCGMgas;       /* I:88 */

    if (i != centralgal) {                             /* I:90 */
      g[i].EjectedMass = g[i].MetalsEjectedMass = 0.0; /* I:92 */
      if (g[i].ICS > 0.0) {                            /* I:96 */
        g[centralgal].ICS_accrete += g[i].ICS;         /* I:101 */
        g[centralgal].ICS_sum_mt += g[i].ICS_sum_mt;   /* I:103 */
        g[i].ICS_disrupt = 0.0;                        /* I:105 */
        g[i].ICS_accrete = 0.0;                        /* I:106 */
        g[i].ICS_sum_mt = 0.0;                         /* I:107 */
      }
      g[i].ICS = g[i].MetalsICS = 0.0; /* I:109 */
      /* I:115-117: CGMrecipeOn 1 keeps the satellite's CGM */
      if (run_params.CGMrecipeOn != 1) {
        g[i].CGMgas = g[i].MetalsCGMgas = 0.0;
      }
    }
  }

  /* I:122-126, SAGE26's own do_reionization */
  if (run_params.ReionizationOn) {
    reionization_modifier = do_reionization(centralgal, Zcurr, g, &run_params);
  } else {
    reionization_modifier = 1.0;
  }

  /* I:128-130 */
  infallingMass = reionization_modifier * run_params.BaryonFrac * g[centralgal].Mvir -
                  (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected + tot_BHMass +
                   tot_ICS + tot_CGMgas);

  g[centralgal].EjectedMass = tot_ejected;                           /* I:133 */
  g[centralgal].MetalsEjectedMass = tot_ejectedMetals;               /* I:134 */
  if (g[centralgal].MetalsEjectedMass > g[centralgal].EjectedMass) { /* I:136 */
    g[centralgal].MetalsEjectedMass = g[centralgal].EjectedMass;     /* I:137 */
  }
  if (g[centralgal].EjectedMass < 0.0) {                               /* I:140 */
    g[centralgal].EjectedMass = g[centralgal].MetalsEjectedMass = 0.0; /* I:141 */
  }
  if (g[centralgal].MetalsEjectedMass < 0.0) { /* I:144 */
    g[centralgal].MetalsEjectedMass = 0.0;     /* I:145 */
  }

  if (run_params.CGMrecipeOn == 1) { /* I:159 */
    if (g[centralgal].Regime == 0) { /* I:164 */
      f->central_regime_cgm = 1;
      g[centralgal].CGMgas += g[centralgal].HotGas;             /* I:166 */
      g[centralgal].MetalsCGMgas += g[centralgal].MetalsHotGas; /* I:167 */
      g[centralgal].HotGas = 0.0;                               /* I:168 */
      g[centralgal].MetalsHotGas = 0.0;                         /* I:169 */
    } else {
      f->central_regime_cgm = 0;
      g[centralgal].HotGas += g[centralgal].CGMgas;             /* I:172 */
      g[centralgal].MetalsHotGas += g[centralgal].MetalsCGMgas; /* I:173 */
      g[centralgal].CGMgas = 0.0;                               /* I:174 */
      g[centralgal].MetalsCGMgas = 0.0;                         /* I:175 */
    }
  } else {
    fatal("transcribe_infall_recipe_cgm: CGMrecipeOn %d is not transcribed",
          run_params.CGMrecipeOn);
  }

  f->cgm_metals_cap_binds = g[centralgal].MetalsCGMgas > g[centralgal].CGMgas; /* I:184 */
  if (g[centralgal].MetalsCGMgas > g[centralgal].CGMgas) {
    g[centralgal].MetalsCGMgas = g[centralgal].CGMgas; /* I:185 */
  }
  f->cgm_gas_negative_clears = g[centralgal].CGMgas < 0.0; /* I:188 */
  if (g[centralgal].CGMgas < 0.0) {
    g[centralgal].CGMgas = g[centralgal].MetalsCGMgas = 0.0; /* I:189 */
    f->cgm_metals_floor_binds = -1;
  } else {
    f->cgm_metals_floor_binds = g[centralgal].MetalsCGMgas < 0.0; /* I:192 */
  }
  if (g[centralgal].MetalsCGMgas < 0.0) {
    g[centralgal].MetalsCGMgas = 0.0; /* I:193 */
  }

  g[centralgal].ICS = tot_ICS;                       /* I:197 */
  g[centralgal].MetalsICS = tot_ICSMetals;           /* I:198 */
  if (g[centralgal].MetalsICS > g[centralgal].ICS) { /* I:200 */
    g[centralgal].MetalsICS = g[centralgal].ICS;     /* I:201 */
  }
  if (g[centralgal].ICS < 0.0) {                       /* I:204 */
    g[centralgal].ICS = g[centralgal].MetalsICS = 0.0; /* I:205 */
  }
  if (g[centralgal].MetalsICS < 0.0) { /* I:208 */
    g[centralgal].MetalsICS = 0.0;     /* I:209 */
  }
  return infallingMass; /* I:212 */
}

/** @brief Compose, check and record infall_recipe's CGMrecipeOn 1 call */
static void c10_infall_recipe(const struct GALAXY *input, const int ngal, const int centralgal,
                              const double Zcurr, const double sage_return) {
  c10_require_rest_unchanged(input, galaxies, ngal, IR_MEMBERS, IR_MEMBER_COUNT, "infall_recipe");

  struct GALAXY work[MAX_GALAXIES];
  memcpy(work, input, sizeof(work));
  struct ir_flags f = {-1, -1, -1, -1};
  const double transcribed = transcribe_infall_recipe_cgm(centralgal, ngal, Zcurr, work, &f);
  uint64_t a, b;
  memcpy(&a, &transcribed, sizeof(a));
  memcpy(&b, &sage_return, sizeof(b));
  if (a != b || c10_members_differ(work, galaxies, ngal, IR_MEMBERS, IR_MEMBER_COUNT)) {
    fatal("the infall_recipe transcription does not reproduce SAGE26's call");
  }

  out_i32("intermediate.central_regime_cgm", f.central_regime_cgm);
  out_i32("intermediate.cgm_metals_cap_binds", f.cgm_metals_cap_binds);
  out_i32("intermediate.cgm_gas_negative_clears", f.cgm_gas_negative_clears);
  out_i32("intermediate.cgm_metals_floor_binds", f.cgm_metals_floor_binds);
}

// ----------------------------------------------------------------------------
// add_infall_to_hot, CGMrecipeOn 1 (I:351-414), with the call-site share (B:488)
// ----------------------------------------------------------------------------

enum { AIH_PER_STEP, AIH_AFTER_EJECTED, AIH_Z_EJECTED, AIH_Z_SELECTED, AIH_COUNT };
static const char *const AIH_NAMES[] = {"infallingGas_per_step", "infallingGas_after_ejected",
                                        "metallicity_ejected", "metallicity_selected"};
static const int AIH_KINDS[] = {C10_CHECK_CONTINUATION, C10_CHECK_TAIL, C10_CHECK_TAIL,
                                C10_CHECK_TAIL};
static const int AIH_F32[] = {0, 0, 1, 1};

static const struct c10_member AIH_MEMBERS[] = {
    C10_MEMBER(EjectedMass), C10_MEMBER(MetalsEjectedMass),
    C10_MEMBER(HotGas),      C10_MEMBER(MetalsHotGas),
    C10_MEMBER(CGMgas),      C10_MEMBER(MetalsCGMgas),
};
#define AIH_MEMBER_COUNT (sizeof(AIH_MEMBERS) / sizeof(AIH_MEMBERS[0]))

struct aih_flags {
  int ejected_withdrawal_ran;      /* I:357 */
  int ejected_metals_floor_binds;  /* I:360 */
  int ejected_exhausted;           /* I:363 */
  int reservoir_cgm;               /* I:372 */
  int selected_metals_debit_ran;   /* I:376 / I:390 */
  int selected_metals_floor_binds; /* I:379 / I:393 */
  int selected_gas_cleared;        /* I:384 / I:398 */
};

/**
 * @brief Composition of add_infall_to_hot on galaxy 0: the transcription, or from the local
 *        `sage_from` (AIH_PER_STEP) SAGE26's own add_infall_to_hot
 */
static void compose_add_infall_to_hot(struct GALAXY *work, const double budget, const int steps,
                                      const struct c10_override *ov, const int sage_from,
                                      struct c10_locals *l, struct aih_flags *f) {
  struct GALAXY *g = &work[0];
  double infallingGas = budget / steps; /* B:488 */
  infallingGas = c10_ov_f64(ov, AIH_PER_STEP, infallingGas);
  c10_record(l, AIH_PER_STEP, infallingGas);
  if (sage_from == AIH_PER_STEP) {
    add_infall_to_hot(0, infallingGas, work, &run_params);
    return;
  }

  float metallicity; /* I:354 */
  f->ejected_withdrawal_ran = 0;
  if (infallingGas < 0.0 && g->EjectedMass > 0.0) { /* I:357 */
    f->ejected_withdrawal_ran = 1;
    metallicity = get_metallicity(g->EjectedMass, g->MetalsEjectedMass); /* I:358 */
    metallicity = c10_ov_f32(ov, AIH_Z_EJECTED, metallicity);
    c10_record(l, AIH_Z_EJECTED, metallicity);
    g->MetalsEjectedMass += infallingGas * metallicity; /* I:359 */
    f->ejected_metals_floor_binds = g->MetalsEjectedMass < 0.0;
    if (g->MetalsEjectedMass < 0.0) { /* I:360 */
      g->MetalsEjectedMass = 0.0;
    }

    g->EjectedMass += infallingGas; /* I:362 */
    if (g->EjectedMass < 0.0) {     /* I:363 */
      f->ejected_exhausted = 1;
      infallingGas = g->EjectedMass;               /* I:364 */
      g->EjectedMass = g->MetalsEjectedMass = 0.0; /* I:365 */
    } else {
      f->ejected_exhausted = 0;
      infallingGas = 0.0; /* I:367 */
    }
  }
  infallingGas = c10_ov_f64(ov, AIH_AFTER_EJECTED, infallingGas);
  c10_record(l, AIH_AFTER_EJECTED, infallingGas);

  if (run_params.CGMrecipeOn != 1) { /* I:371 */
    fatal("compose_add_infall_to_hot: CGMrecipeOn %d is not transcribed", run_params.CGMrecipeOn);
  }
  f->selected_metals_debit_ran = 0;
  if (g->Regime == 0) { /* I:372 */
    f->reservoir_cgm = 1;
    if (infallingGas < 0.0 && g->MetalsCGMgas > 0.0) { /* I:376 */
      f->selected_metals_debit_ran = 1;
      metallicity = get_metallicity(g->CGMgas, g->MetalsCGMgas); /* I:377 */
      metallicity = c10_ov_f32(ov, AIH_Z_SELECTED, metallicity);
      c10_record(l, AIH_Z_SELECTED, metallicity);
      g->MetalsCGMgas += infallingGas * metallicity; /* I:378 */
      f->selected_metals_floor_binds = g->MetalsCGMgas < 0.0;
      if (g->MetalsCGMgas < 0.0) { /* I:379 */
        g->MetalsCGMgas = 0.0;
      }
    }
    g->CGMgas += infallingGas; /* I:383 */
    f->selected_gas_cleared = g->CGMgas < 0.0;
    if (g->CGMgas < 0.0) { /* I:384 */
      g->CGMgas = g->MetalsCGMgas = 0.0;
    }
  } else {
    f->reservoir_cgm = 0;
    if (infallingGas < 0.0 && g->MetalsHotGas > 0.0) { /* I:390 */
      f->selected_metals_debit_ran = 1;
      metallicity = get_metallicity(g->HotGas, g->MetalsHotGas); /* I:391 */
      metallicity = c10_ov_f32(ov, AIH_Z_SELECTED, metallicity);
      c10_record(l, AIH_Z_SELECTED, metallicity);
      g->MetalsHotGas += infallingGas * metallicity; /* I:392 */
      f->selected_metals_floor_binds = g->MetalsHotGas < 0.0;
      if (g->MetalsHotGas < 0.0) { /* I:393 */
        g->MetalsHotGas = 0.0;
      }
    }
    g->HotGas += infallingGas; /* I:397 */
    f->selected_gas_cleared = g->HotGas < 0.0;
    if (g->HotGas < 0.0) { /* I:398 */
      g->HotGas = g->MetalsHotGas = 0.0;
    }
  }
}

static void c10_init_locals(struct c10_locals *l, const int count, const char *const *names,
                            const int *kinds, const int *is_f32) {
  memset(l, 0, sizeof(*l));
  l->count = count;
  l->names = names;
  l->kinds = kinds;
  l->is_f32 = is_f32;
}

/** @brief Compose, check, validate and record add_infall_to_hot's CGMrecipeOn 1 call */
static void c10_add_infall_to_hot(const struct GALAXY *input, const double budget,
                                  const int steps) {
  c10_require_rest_unchanged(input, galaxies, 1, AIH_MEMBERS, AIH_MEMBER_COUNT,
                             "add_infall_to_hot");
  const struct c10_override none = {C10_NO_OVERRIDE, 0.0};

  struct GALAXY work[MAX_GALAXIES];
  struct c10_locals l;
  struct aih_flags f = {-1, -1, -1, -1, -1, -1, -1};
  memcpy(work, input, sizeof(work));
  c10_init_locals(&l, AIH_COUNT, AIH_NAMES, AIH_KINDS, AIH_F32);
  compose_add_infall_to_hot(work, budget, steps, &none, C10_NO_OVERRIDE, &l, &f);
  if (c10_members_differ(work, galaxies, 1, AIH_MEMBERS, AIH_MEMBER_COUNT)) {
    fatal("the add_infall_to_hot transcription does not reproduce SAGE26's call");
  }
  /* The continuation composition from the per-substep share */
  {
    struct GALAXY cont[MAX_GALAXIES];
    struct c10_locals lc;
    struct aih_flags fc = f;
    memcpy(cont, input, sizeof(cont));
    c10_init_locals(&lc, AIH_COUNT, AIH_NAMES, AIH_KINDS, AIH_F32);
    compose_add_infall_to_hot(cont, budget, steps, &none, AIH_PER_STEP, &lc, &fc);
    if (c10_members_differ(cont, galaxies, 1, AIH_MEMBERS, AIH_MEMBER_COUNT)) {
      fatal("the add_infall_to_hot composition from infallingGas_per_step does not reproduce "
            "SAGE26's call");
    }
  }

  int up[AIH_COUNT] = {0}, down[AIH_COUNT] = {0};
  for (int k = 0; k < AIH_COUNT; k++) {
    if (!l.present[k]) {
      continue;
    }
    for (int direction = 1; direction >= -1; direction -= 2) {
      struct GALAXY perturbed[MAX_GALAXIES];
      struct c10_locals lp;
      struct aih_flags fp = {-1, -1, -1, -1, -1, -1, -1};
      const struct c10_override ov = {k, c10_neighbour(l.value[k], l.is_f32[k], direction)};
      memcpy(perturbed, input, sizeof(perturbed));
      c10_init_locals(&lp, AIH_COUNT, AIH_NAMES, AIH_KINDS, AIH_F32);
      compose_add_infall_to_hot(perturbed, budget, steps, &ov,
                                l.kinds[k] == C10_CHECK_CONTINUATION ? k : C10_NO_OVERRIDE, &lp,
                                &fp);
      const int changed = c10_members_differ(perturbed, galaxies, 1, AIH_MEMBERS, AIH_MEMBER_COUNT);
      if (direction > 0) {
        up[k] = changed;
      } else {
        down[k] = changed;
      }
    }
  }

  out_f32(galaxy_field(0, "CGMgas"), galaxies[0].CGMgas);
  out_f32(galaxy_field(0, "MetalsCGMgas"), galaxies[0].MetalsCGMgas);
  out_i32("intermediate.ejected_withdrawal_ran", f.ejected_withdrawal_ran);
  out_i32("intermediate.ejected_metals_floor_binds", f.ejected_metals_floor_binds);
  out_i32("intermediate.ejected_exhausted", f.ejected_exhausted);
  out_i32("intermediate.reservoir_cgm", f.reservoir_cgm);
  out_i32("intermediate.selected_metals_debit_ran", f.selected_metals_debit_ran);
  out_i32("intermediate.selected_metals_floor_binds", f.selected_metals_floor_binds);
  out_i32("intermediate.selected_gas_cleared", f.selected_gas_cleared);
  for (int k = 0; k < AIH_COUNT; k++) {
    /* infallingGas_per_step's value is the c03 line written above */
    c10_out_local(AIH_NAMES[k], &l, k, up[k], down[k], k == AIH_PER_STEP);
  }
}

// ----------------------------------------------------------------------------
// reincorporate_gas, CGMrecipeOn 1 (G:44-66, X:76-85)
// ----------------------------------------------------------------------------

enum { RG_VCRIT, RG_PRE_CAP, RG_REINCORPORATED, RG_METALLICITY, RG_METALS, RG_COUNT };
static const char *const RG_NAMES[] = {"Vcrit", "reincorporated_pre_cap", "reincorporated",
                                       "metallicity", "metals_reincorporated"};
static const int RG_KINDS[] = {C10_CHECK_TAIL, C10_CHECK_TAIL, C10_CHECK_CONTINUATION,
                               C10_CHECK_TAIL, C10_CHECK_CONTINUATION};
static const int RG_F32[] = {0, 0, 0, 0, 0};

static const struct c10_member RG_MEMBERS[] = {
    C10_MEMBER(EjectedMass), C10_MEMBER(MetalsEjectedMass),
    C10_MEMBER(HotGas),      C10_MEMBER(MetalsHotGas),
    C10_MEMBER(CGMgas),      C10_MEMBER(MetalsCGMgas),
};
#define RG_MEMBER_COUNT (sizeof(RG_MEMBERS) / sizeof(RG_MEMBERS[0]))

struct rg_flags {
  int guards_passed; /* G:48 */
  int cap_binds;     /* G:54 */
  int reservoir_cgm; /* X:78 */
};

/**
 * @brief Composition of reincorporate_gas on galaxy 0: the transcription, or with SAGE26's own
 *        add_gas_to_hot_reservoir from a continuation-checked local (`sage_from`)
 */
static void compose_reincorporate_gas(struct GALAXY *work, const double dt,
                                      const struct c10_override *ov, const int sage_from,
                                      struct c10_locals *l, struct rg_flags *f) {
  struct GALAXY *g = &work[0];
  double Vcrit = 354.53 * run_params.ReIncorporationFactor; /* G:34, G:46 */
  Vcrit = c10_ov_f64(ov, RG_VCRIT, Vcrit);
  c10_record(l, RG_VCRIT, Vcrit);

  f->guards_passed = (Vcrit > 0.0 && g->Vvir > Vcrit && g->Rvir > 0.0);
  if (Vcrit > 0.0 && g->Vvir > Vcrit && g->Rvir > 0.0) { /* G:48 */
    double reincorporated =
        (g->Vvir / Vcrit - 1.0) * g->EjectedMass / (g->Rvir / g->Vvir) * dt; /* G:50-52 */
    reincorporated = c10_ov_f64(ov, RG_PRE_CAP, reincorporated);
    c10_record(l, RG_PRE_CAP, reincorporated);

    f->cap_binds = reincorporated > g->EjectedMass;
    if (reincorporated > g->EjectedMass) /* G:54 */
      reincorporated = g->EjectedMass;   /* G:55 */
    reincorporated = c10_ov_f64(ov, RG_REINCORPORATED, reincorporated);
    c10_record(l, RG_REINCORPORATED, reincorporated);

    double metallicity = get_metallicity(g->EjectedMass, g->MetalsEjectedMass); /* G:57 */
    metallicity = c10_ov_f64(ov, RG_METALLICITY, metallicity);
    c10_record(l, RG_METALLICITY, metallicity);

    g->EjectedMass -= reincorporated;                     /* G:60 */
    g->MetalsEjectedMass -= metallicity * reincorporated; /* G:61 */

    double metals = metallicity * reincorporated; /* G:64, the helper's metal argument */
    metals = c10_ov_f64(ov, RG_METALS, metals);
    c10_record(l, RG_METALS, metals);

    f->reservoir_cgm = (run_params.CGMrecipeOn == 1 && g->Regime == 0);
    if (sage_from == RG_REINCORPORATED || sage_from == RG_METALS) {
      add_gas_to_hot_reservoir(g, &run_params, reincorporated, metals); /* G:64, SAGE26's own */
      return;
    }
    if (run_params.CGMrecipeOn == 1 && g->Regime == 0) { /* X:78 */
      g->CGMgas += reincorporated;                       /* X:79 */
      g->MetalsCGMgas += metals;                         /* X:80 */
    } else {
      g->HotGas += reincorporated; /* X:82 */
      g->MetalsHotGas += metals;   /* X:83 */
    }
  }
}

/** @brief Compose, check, validate and record reincorporate_gas's CGMrecipeOn 1 call */
static void c10_reincorporate_gas(const struct GALAXY *input, const double dt, const int called) {
  c10_require_rest_unchanged(input, galaxies, 1, RG_MEMBERS, RG_MEMBER_COUNT, "reincorporate_gas");
  struct c10_locals l;
  struct rg_flags f = {-1, -1, -1};
  int up[RG_COUNT] = {0}, down[RG_COUNT] = {0};
  c10_init_locals(&l, RG_COUNT, RG_NAMES, RG_KINDS, RG_F32);

  if (called) {
    const struct c10_override none = {C10_NO_OVERRIDE, 0.0};
    struct GALAXY work[MAX_GALAXIES];
    memcpy(work, input, sizeof(work));
    compose_reincorporate_gas(work, dt, &none, C10_NO_OVERRIDE, &l, &f);
    if (c10_members_differ(work, galaxies, 1, RG_MEMBERS, RG_MEMBER_COUNT)) {
      fatal("the reincorporate_gas transcription does not reproduce SAGE26's call");
    }
    for (int k = 0; k < RG_COUNT; k++) {
      if (!l.present[k] || l.kinds[k] != C10_CHECK_CONTINUATION) {
        continue;
      }
      struct GALAXY cont[MAX_GALAXIES];
      struct c10_locals lc;
      struct rg_flags fc = {-1, -1, -1};
      memcpy(cont, input, sizeof(cont));
      c10_init_locals(&lc, RG_COUNT, RG_NAMES, RG_KINDS, RG_F32);
      compose_reincorporate_gas(cont, dt, &none, k, &lc, &fc);
      if (c10_members_differ(cont, galaxies, 1, RG_MEMBERS, RG_MEMBER_COUNT)) {
        fatal("the reincorporate_gas composition from %s does not reproduce SAGE26's call",
              RG_NAMES[k]);
      }
    }
    for (int k = 0; k < RG_COUNT; k++) {
      if (!l.present[k]) {
        continue;
      }
      for (int direction = 1; direction >= -1; direction -= 2) {
        struct GALAXY perturbed[MAX_GALAXIES];
        struct c10_locals lp;
        struct rg_flags fp = {-1, -1, -1};
        const struct c10_override ov = {k, c10_neighbour(l.value[k], l.is_f32[k], direction)};
        memcpy(perturbed, input, sizeof(perturbed));
        c10_init_locals(&lp, RG_COUNT, RG_NAMES, RG_KINDS, RG_F32);
        compose_reincorporate_gas(perturbed, dt, &ov,
                                  l.kinds[k] == C10_CHECK_CONTINUATION ? k : C10_NO_OVERRIDE, &lp,
                                  &fp);
        const int changed = c10_members_differ(perturbed, galaxies, 1, RG_MEMBERS, RG_MEMBER_COUNT);
        if (direction > 0) {
          up[k] = changed;
        } else {
          down[k] = changed;
        }
      }
    }
  } else if (c10_members_differ(input, galaxies, 1, RG_MEMBERS, RG_MEMBER_COUNT)) {
    fatal("reincorporate_gas was not called but the state changed");
  }

  out_f32(galaxy_field(0, "CGMgas"), galaxies[0].CGMgas);
  out_f32(galaxy_field(0, "MetalsCGMgas"), galaxies[0].MetalsCGMgas);
  out_i32("intermediate.guards_passed", f.guards_passed);
  out_i32("intermediate.reincorporation_cap_binds", f.cap_binds);
  out_i32("intermediate.reservoir_cgm", f.reservoir_cgm);
  for (int k = 0; k < RG_COUNT; k++) {
    c10_out_local(RG_NAMES[k], &l, k, up[k], down[k], 0);
  }
}

// ----------------------------------------------------------------------------
// strip_from_satellite, CGMrecipeOn 1 (I:225-273)
// ----------------------------------------------------------------------------

enum {
  ST_SAT_BARYONS,
  ST_PRE_CAP,
  ST_METALLICITY,
  ST_METALS_PRE_CAP,
  ST_STRIPPED,
  ST_STRIPPED_METALS,
  ST_COUNT
};
static const char *const ST_NAMES[] = {"satBaryons",  "strippedGas_pre_cap",
                                       "metallicity", "strippedGasMetals_pre_cap",
                                       "strippedGas", "strippedGasMetals"};
static const int ST_KINDS[] = {C10_CHECK_TAIL, C10_CHECK_TAIL, C10_CHECK_TAIL,
                               C10_CHECK_TAIL, C10_CHECK_TAIL, C10_CHECK_TAIL};
static const int ST_F32[] = {0, 0, 0, 0, 0, 0};

static const struct c10_member ST_MEMBERS[] = {
    C10_MEMBER(HotGas),
    C10_MEMBER(MetalsHotGas),
    C10_MEMBER(CGMgas),
    C10_MEMBER(MetalsCGMgas),
};
#define ST_MEMBER_COUNT (sizeof(ST_MEMBERS) / sizeof(ST_MEMBERS[0]))

struct st_flags {
  int stripped;         /* I:246 */
  int take_from_cgm;    /* I:250 */
  int give_to_cgm;      /* I:251 */
  int gas_cap_binds;    /* I:259 */
  int metals_cap_binds; /* I:260 */
};

/** @brief Transcription of strip_from_satellite's CGMrecipeOn 1 path */
static void compose_strip_from_satellite(struct GALAXY *g, const int centralgal, const int gal,
                                         const double Zcurr, const int nsteps,
                                         const struct c10_override *ov, struct c10_locals *l,
                                         struct st_flags *f) {
  double reionization_modifier;
  if (run_params.ReionizationOn) {                                       /* I:230 */
    reionization_modifier = do_reionization(gal, Zcurr, g, &run_params); /* I:231, SAGE26's own */
  } else {
    reionization_modifier = 1.0; /* I:233 */
  }

  double satBaryons = g[gal].StellarMass + g[gal].ColdGas + g[gal].HotGas + g[gal].CGMgas +
                      g[gal].EjectedMass + g[gal].BlackHoleMass + g[gal].ICS; /* I:238-241 */
  satBaryons = c10_ov_f64(ov, ST_SAT_BARYONS, satBaryons);
  c10_record(l, ST_SAT_BARYONS, satBaryons);

  double strippedGas = -1.0 *
                       (reionization_modifier * run_params.BaryonFrac * g[gal].Mvir - satBaryons) /
                       nsteps; /* I:243-244 */
  strippedGas = c10_ov_f64(ov, ST_PRE_CAP, strippedGas);
  c10_record(l, ST_PRE_CAP, strippedGas);

  f->stripped = strippedGas > 0.0;
  if (strippedGas > 0.0) { /* I:246 */
    if (run_params.CGMrecipeOn != 1) {
      fatal("compose_strip_from_satellite: CGMrecipeOn %d is not transcribed",
            run_params.CGMrecipeOn);
    }
    const int take_from_cgm = (run_params.CGMrecipeOn == 1 && g[gal].Regime == 0);      /* I:250 */
    const int give_to_cgm = (run_params.CGMrecipeOn == 1 && g[centralgal].Regime == 0); /* I:251 */
    f->take_from_cgm = take_from_cgm;
    f->give_to_cgm = give_to_cgm;

    float *sat_gas = take_from_cgm ? &g[gal].CGMgas : &g[gal].HotGas;                /* I:253 */
    float *sat_metals = take_from_cgm ? &g[gal].MetalsCGMgas : &g[gal].MetalsHotGas; /* I:254 */

    double metallicity = get_metallicity(*sat_gas, *sat_metals); /* I:256 */
    metallicity = c10_ov_f64(ov, ST_METALLICITY, metallicity);
    c10_record(l, ST_METALLICITY, metallicity);
    double strippedGasMetals = strippedGas * metallicity; /* I:257 */
    strippedGasMetals = c10_ov_f64(ov, ST_METALS_PRE_CAP, strippedGasMetals);
    c10_record(l, ST_METALS_PRE_CAP, strippedGasMetals);

    f->gas_cap_binds = strippedGas > *sat_gas;
    if (strippedGas > *sat_gas) { /* I:259 */
      strippedGas = *sat_gas;
    }
    strippedGas = c10_ov_f64(ov, ST_STRIPPED, strippedGas);
    c10_record(l, ST_STRIPPED, strippedGas);
    f->metals_cap_binds = strippedGasMetals > *sat_metals;
    if (strippedGasMetals > *sat_metals) { /* I:260 */
      strippedGasMetals = *sat_metals;
    }
    strippedGasMetals = c10_ov_f64(ov, ST_STRIPPED_METALS, strippedGasMetals);
    c10_record(l, ST_STRIPPED_METALS, strippedGasMetals);

    *sat_gas -= strippedGas;          /* I:262 */
    *sat_metals -= strippedGasMetals; /* I:263 */

    if (give_to_cgm) {                                         /* I:265 */
      g[centralgal].CGMgas += strippedGas;                     /* I:266 */
      g[centralgal].MetalsCGMgas += strippedGas * metallicity; /* I:267 */
    } else {
      g[centralgal].HotGas += strippedGas;                     /* I:269 */
      g[centralgal].MetalsHotGas += strippedGas * metallicity; /* I:270 */
    }
  }
}

/** @brief Compose, check, validate and record strip_from_satellite's CGMrecipeOn 1 call */
static void c10_strip_from_satellite(const struct GALAXY *input, const int ngal,
                                     const int centralgal, const int satellite, const double Zcurr,
                                     const int nsteps, const int called) {
  c10_require_rest_unchanged(input, galaxies, ngal, ST_MEMBERS, ST_MEMBER_COUNT,
                             "strip_from_satellite");
  struct c10_locals l;
  struct st_flags f = {-1, -1, -1, -1, -1};
  int up[ST_COUNT] = {0}, down[ST_COUNT] = {0};
  c10_init_locals(&l, ST_COUNT, ST_NAMES, ST_KINDS, ST_F32);

  if (called) {
    const struct c10_override none = {C10_NO_OVERRIDE, 0.0};
    struct GALAXY work[MAX_GALAXIES];
    memcpy(work, input, sizeof(work));
    compose_strip_from_satellite(work, centralgal, satellite, Zcurr, nsteps, &none, &l, &f);
    if (c10_members_differ(work, galaxies, ngal, ST_MEMBERS, ST_MEMBER_COUNT)) {
      fatal("the strip_from_satellite transcription does not reproduce SAGE26's call");
    }
    for (int k = 0; k < ST_COUNT; k++) {
      if (!l.present[k]) {
        continue;
      }
      for (int direction = 1; direction >= -1; direction -= 2) {
        struct GALAXY perturbed[MAX_GALAXIES];
        struct c10_locals lp;
        struct st_flags fp = {-1, -1, -1, -1, -1};
        const struct c10_override ov = {k, c10_neighbour(l.value[k], l.is_f32[k], direction)};
        memcpy(perturbed, input, sizeof(perturbed));
        c10_init_locals(&lp, ST_COUNT, ST_NAMES, ST_KINDS, ST_F32);
        compose_strip_from_satellite(perturbed, centralgal, satellite, Zcurr, nsteps, &ov, &lp,
                                     &fp);
        const int changed =
            c10_members_differ(perturbed, galaxies, ngal, ST_MEMBERS, ST_MEMBER_COUNT);
        if (direction > 0) {
          up[k] = changed;
        } else {
          down[k] = changed;
        }
      }
    }
  } else if (c10_members_differ(input, galaxies, ngal, ST_MEMBERS, ST_MEMBER_COUNT)) {
    fatal("strip_from_satellite was not called but the state changed");
  }

  out_i32("intermediate.stripped", f.stripped);
  out_i32("intermediate.take_from_cgm", f.take_from_cgm);
  out_i32("intermediate.give_to_cgm", f.give_to_cgm);
  out_i32("intermediate.gas_cap_binds", f.gas_cap_binds);
  out_i32("intermediate.metals_cap_binds", f.metals_cap_binds);
  for (int k = 0; k < ST_COUNT; k++) {
    c10_out_local(ST_NAMES[k], &l, k, up[k], down[k], 0);
  }
}

// ============================================================================
// ROUTINES
// ============================================================================

static void run_do_reionization(void) {
  read_reionization_params();
  run_params.BaryonFrac = in_f64("params.BaryonFrac");
  const double Zcurr = in_f64("call.Zcurr");
  galaxies[0].Type = in_i32(galaxy_field(0, "Type"));
  galaxies[0].Mvir = in_f32(galaxy_field(0, "Mvir"));
  check_all_fields_used();

  const double modifier = do_reionization(0, Zcurr, galaxies, &run_params);

  out_f64("return.reionization_modifier", modifier);
  /* The package carries this once-rounded product as HaloBaryonFraction; SAGE26
   * forms it as the leading product of R/src/model_infall.c:129 and :244. */
  out_f64("intermediate.HaloBaryonFraction", modifier * run_params.BaryonFrac);
}

static void run_infall_recipe(void) {
  run_params.ReionizationOn = in_i32("params.ReionizationOn");
  run_params.CGMrecipeOn = read_cgm_recipe_on();
  run_params.BaryonFrac = in_f64("params.BaryonFrac");
  read_reionization_params();
  const int ngal = read_galaxy_count();
  const int centralgal = read_galaxy_index("call.centralgal", ngal);
  const double Zcurr = in_f64("call.Zcurr");
  for (int i = 0; i < ngal; i++) {
    struct GALAXY *g = &galaxies[i];
    g->Type = in_i32(galaxy_field(i, "Type"));
    g->Mvir = in_f32(galaxy_field(i, "Mvir"));
    g->StellarMass = in_f32(galaxy_field(i, "StellarMass"));
    g->BlackHoleMass = in_f32(galaxy_field(i, "BlackHoleMass"));
    g->ColdGas = in_f32(galaxy_field(i, "ColdGas"));
    g->HotGas = in_f32(galaxy_field(i, "HotGas"));
    g->MetalsHotGas = in_f32(galaxy_field(i, "MetalsHotGas"));
    g->EjectedMass = in_f32(galaxy_field(i, "EjectedMass"));
    g->MetalsEjectedMass = in_f32(galaxy_field(i, "MetalsEjectedMass"));
    g->ICS = in_f32(galaxy_field(i, "ICS"));
    g->MetalsICS = in_f32(galaxy_field(i, "MetalsICS"));
    g->CGMgas = in_f32(galaxy_field(i, "CGMgas"));
    g->MetalsCGMgas = in_f32(galaxy_field(i, "MetalsCGMgas"));
    g->ICS_disrupt = in_f32(galaxy_field(i, "ICS_disrupt"));
    g->ICS_accrete = in_f32(galaxy_field(i, "ICS_accrete"));
    g->ICS_sum_mt = in_f32(galaxy_field(i, "ICS_sum_mt"));
    if (run_params.CGMrecipeOn == 1) {
      g->Regime = in_i32(galaxy_field(i, "Regime")); /* the central's is read at I:164 */
    }
  }
  check_all_fields_used();
  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* do_reionization reads only the central's Mvir and params, which infall_recipe
   * never writes, so evaluating it here gives the value infall_recipe uses. */
  const double modifier = reionization_modifier_for(centralgal, Zcurr);

  /* R/src/core_build_model.c:415 */
  const double infallingGas = infall_recipe(centralgal, ngal, Zcurr, galaxies, &run_params);

  out_f64("return.InfallingGas", infallingGas);
  out_f64("intermediate.reionization_modifier", modifier);
  out_f64("intermediate.HaloBaryonFraction", modifier * run_params.BaryonFrac);
  for (int i = 0; i < ngal; i++) {
    const struct GALAXY *g = &galaxies[i];
    out_f32(galaxy_field(i, "EjectedMass"), g->EjectedMass);
    out_f32(galaxy_field(i, "MetalsEjectedMass"), g->MetalsEjectedMass);
    out_f32(galaxy_field(i, "ICS"), g->ICS);
    out_f32(galaxy_field(i, "MetalsICS"), g->MetalsICS);
    out_f32(galaxy_field(i, "ICS_disrupt"), g->ICS_disrupt);
    out_f32(galaxy_field(i, "ICS_accrete"), g->ICS_accrete);
    out_f32(galaxy_field(i, "ICS_sum_mt"), g->ICS_sum_mt);
    out_f32(galaxy_field(i, "CGMgas"), g->CGMgas);
    out_f32(galaxy_field(i, "MetalsCGMgas"), g->MetalsCGMgas);
    out_f32(galaxy_field(i, "HotGas"), g->HotGas);
    out_f32(galaxy_field(i, "MetalsHotGas"), g->MetalsHotGas);
  }
  if (run_params.CGMrecipeOn == 1) {
    c10_infall_recipe(input, ngal, centralgal, Zcurr, infallingGas);
  }
}

static void run_add_infall_to_hot(void) {
  run_params.CGMrecipeOn = read_cgm_recipe_on();
  const double infallingGas = in_f64("call.InfallingGas");
  const int effective_steps = read_effective_steps();
  struct GALAXY *g = &galaxies[0];
  g->Type = in_i32(galaxy_field(0, "Type"));
  g->EjectedMass = in_f32(galaxy_field(0, "EjectedMass"));
  g->MetalsEjectedMass = in_f32(galaxy_field(0, "MetalsEjectedMass"));
  g->HotGas = in_f32(galaxy_field(0, "HotGas"));
  g->MetalsHotGas = in_f32(galaxy_field(0, "MetalsHotGas"));
  if (run_params.CGMrecipeOn == 1) {
    g->Regime = in_i32(galaxy_field(0, "Regime")); /* I:372 */
    g->CGMgas = in_f32(galaxy_field(0, "CGMgas"));
    g->MetalsCGMgas = in_f32(galaxy_field(0, "MetalsCGMgas"));
  }
  check_all_fields_used();
  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* R/src/core_build_model.c:488: the per-substep share of the snapshot budget */
  const double infallingGas_per_step = infallingGas / effective_steps;
  add_infall_to_hot(0, infallingGas_per_step, galaxies, &run_params);

  out_f64("intermediate.infallingGas_per_step", infallingGas_per_step);
  out_f32(galaxy_field(0, "EjectedMass"), g->EjectedMass);
  out_f32(galaxy_field(0, "MetalsEjectedMass"), g->MetalsEjectedMass);
  out_f32(galaxy_field(0, "HotGas"), g->HotGas);
  out_f32(galaxy_field(0, "MetalsHotGas"), g->MetalsHotGas);
  if (run_params.CGMrecipeOn == 1) {
    c10_add_infall_to_hot(input, infallingGas, effective_steps);
  }
}

static void run_reincorporate_gas(void) {
  run_params.ReIncorporationFactor = in_f64("params.ReIncorporationFactor");
  run_params.CGMrecipeOn = read_cgm_recipe_on();
  const double age_galaxy_snap = in_f64("call.Age_galaxy_snap");
  const double halo_age = in_f64("call.halo_age");
  const int effective_steps = read_effective_steps();
  struct GALAXY *g = &galaxies[0];
  g->Type = in_i32(galaxy_field(0, "Type"));
  g->Vvir = in_f32(galaxy_field(0, "Vvir"));
  g->Rvir = in_f32(galaxy_field(0, "Rvir"));
  g->EjectedMass = in_f32(galaxy_field(0, "EjectedMass"));
  g->MetalsEjectedMass = in_f32(galaxy_field(0, "MetalsEjectedMass"));
  g->HotGas = in_f32(galaxy_field(0, "HotGas"));
  g->MetalsHotGas = in_f32(galaxy_field(0, "MetalsHotGas"));
  if (run_params.CGMrecipeOn == 1) {
    g->Regime = in_i32(galaxy_field(0, "Regime")); /* X:78 */
    g->CGMgas = in_f32(galaxy_field(0, "CGMgas"));
    g->MetalsCGMgas = in_f32(galaxy_field(0, "MetalsCGMgas"));
  }
  check_all_fields_used();
  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* R/src/core_build_model.c:479 and :490-492 */
  const double deltaT = age_galaxy_snap - halo_age;
  const double dt = deltaT / effective_steps;
  int called = 0;
  if (run_params.ReIncorporationFactor > 0.0) {
    reincorporate_gas(0, dt, galaxies, &run_params);
    called = 1;
  }

  out_f64("intermediate.deltaT", deltaT);
  out_f64("intermediate.dt", dt);
  out_i32("intermediate.reincorporate_gas_called", called);
  out_f32(galaxy_field(0, "EjectedMass"), g->EjectedMass);
  out_f32(galaxy_field(0, "MetalsEjectedMass"), g->MetalsEjectedMass);
  out_f32(galaxy_field(0, "HotGas"), g->HotGas);
  out_f32(galaxy_field(0, "MetalsHotGas"), g->MetalsHotGas);
  if (run_params.CGMrecipeOn == 1) {
    c10_reincorporate_gas(input, dt, called);
  }
}

static void run_strip_from_satellite(void) {
  run_params.ReionizationOn = in_i32("params.ReionizationOn");
  run_params.CGMrecipeOn = read_cgm_recipe_on();
  run_params.BaryonFrac = in_f64("params.BaryonFrac");
  read_reionization_params();
  const int ngal = read_galaxy_count();
  const int centralgal = read_galaxy_index("call.centralgal", ngal);
  const int satellite = read_galaxy_index("call.satellite", ngal);
  const double Zcurr = in_f64("call.Zcurr");
  const int effective_steps = read_effective_steps();
  for (int i = 0; i < ngal; i++) {
    struct GALAXY *g = &galaxies[i];
    g->Type = in_i32(galaxy_field(i, "Type"));
    g->Mvir = in_f32(galaxy_field(i, "Mvir"));
    g->StellarMass = in_f32(galaxy_field(i, "StellarMass"));
    g->ColdGas = in_f32(galaxy_field(i, "ColdGas"));
    g->HotGas = in_f32(galaxy_field(i, "HotGas"));
    g->MetalsHotGas = in_f32(galaxy_field(i, "MetalsHotGas"));
    g->CGMgas = in_f32(galaxy_field(i, "CGMgas"));
    g->MetalsCGMgas = in_f32(galaxy_field(i, "MetalsCGMgas"));
    g->EjectedMass = in_f32(galaxy_field(i, "EjectedMass"));
    g->BlackHoleMass = in_f32(galaxy_field(i, "BlackHoleMass"));
    g->ICS = in_f32(galaxy_field(i, "ICS"));
    if (run_params.CGMrecipeOn == 1) {
      g->Regime = in_i32(galaxy_field(i, "Regime")); /* the satellite's and central's, I:250-251 */
    }
  }
  check_all_fields_used();
  if (satellite == centralgal) {
    fatal("call.satellite must differ from call.centralgal");
  }
  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input));

  /* The satellite's own modifier: strip_from_satellite evaluates do_reionization on
   * the satellite (R/src/model_infall.c:230-234), which reads only its Mvir. */
  const double modifier = reionization_modifier_for(satellite, Zcurr);

  /* R/src/core_build_model.c:493-498: only Type 1 satellites are stripped */
  int called = 0;
  if (galaxies[satellite].Type == 1) {
    strip_from_satellite(centralgal, satellite, Zcurr, effective_steps, galaxies, &run_params);
    called = 1;
  }

  out_f64("intermediate.reionization_modifier", modifier);
  out_f64("intermediate.HaloBaryonFraction", modifier * run_params.BaryonFrac);
  out_i32("intermediate.strip_from_satellite_called", called);
  for (int i = 0; i < ngal; i++) {
    const struct GALAXY *g = &galaxies[i];
    out_f32(galaxy_field(i, "HotGas"), g->HotGas);
    out_f32(galaxy_field(i, "MetalsHotGas"), g->MetalsHotGas);
    out_f32(galaxy_field(i, "CGMgas"), g->CGMgas);
    out_f32(galaxy_field(i, "MetalsCGMgas"), g->MetalsCGMgas);
  }
  if (run_params.CGMrecipeOn == 1) {
    c10_strip_from_satellite(input, ngal, centralgal, satellite, Zcurr, effective_steps, called);
  }
}

static void run_get_disk_radius(void) {
  struct GALAXY *g = &galaxies[0];
  g->Type = in_i32(galaxy_field(0, "Type"));
  g->Vvir = in_f32(galaxy_field(0, "Vvir"));
  g->Rvir = in_f32(galaxy_field(0, "Rvir"));
  for (int j = 0; j < 3; j++) {
    halos[0].Spin[j] = in_f32(field_name("halo.Spin[%d]", j));
  }
  check_all_fields_used();

  /* R/src/core_build_model.c:297: the double result is stored in a float field */
  const double r_disk = get_disk_radius(0, 0, halos, galaxies);
  g->DiskScaleRadius = r_disk;

  out_f64("return.r_disk", r_disk);
  out_f32(galaxy_field(0, "DiskScaleRadius"), g->DiskScaleRadius);
}

// ============================================================================
// MAIN
// ============================================================================

struct routine_entry {
  const char *name;
  void (*run)(void);
};

static const struct routine_entry routines[] = {
    {"do_reionization", run_do_reionization},
    {"infall_recipe", run_infall_recipe},
    {"add_infall_to_hot", run_add_infall_to_hot},
    {"reincorporate_gas", run_reincorporate_gas},
    {"strip_from_satellite", run_strip_from_satellite},
    {"get_disk_radius", run_get_disk_radius},
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
          "# SAGE26 oracle output for %s, written by oracle_c03 (regenerate.sh); do not edit.\n",
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
