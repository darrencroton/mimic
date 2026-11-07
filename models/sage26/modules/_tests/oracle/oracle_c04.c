/**
 * @file    oracle_c04.c
 * @brief   SAGE26 function-level oracle for the cooling and radio-heating routines
 *
 * Reads one fixture input file, initialises the SAGE26 struct GALAXY and struct params
 * fields its reader for the selected routine consumes (a superset of the fields the
 * routine reads; galaxy[i].Type, for example, is set for the Mimic side although no
 * routine here reads it), reproduces the arithmetic SAGE26's call site
 * (core_build_model.c) performs around that routine, calls the routine from the SAGE26
 * objects this executable is linked against, and writes a fixture output file holding
 * the routine's return value, every field it writes and every intermediate value the
 * Mimic package carries between modules.
 *
 * Usage: oracle_c04 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one; R/ is the staged SAGE26
 * source root, C = R/src/model_cooling_heating.c, B = R/src/core_build_model.c):
 *   get_metaldependent_cooling_rate  R/src/core_cool_func.c:108-158
 *   cooling_recipe_hot               C:135-315, with AGNrecipeOn 2 and CGMrecipeOn 0, so it
 *                                    reaches do_AGN_heating (C:587-627) and the private
 *                                    Bondi branch of agn_accretion_compute (C:526-576);
 *                                    called at B:508 and followed by
 *                                    cool_gas_onto_galaxy at B:509 (the sequence)
 *   do_AGN_heating                   C:587-627, called on its own
 *   cool_gas_onto_galaxy             C:689-707, called on its own
 *   normalise_cooling_heating        a TRANSCRIPTION of B:590-604 (Cooling and Heating only;
 *                                    evolve_galaxies is static and not in this link)
 *
 * The cooling tables are loaded once per process by SAGE26's own
 * read_cooling_functions() (R/src/core_cool_func.c:65-99), from ROOT_DIR as build.sh
 * sets it. That loader adds the solar offset to its static metallicity grid each time
 * it runs, so it must never be called twice in one process.
 *
 * The carried intermediates of cooling_recipe_hot (logTemp, logZ, x, rcool) are local
 * variables no SAGE26 entry point exposes. They come from a labelled TRANSCRIPTION of
 * C:141-173 below (transcribe_cooling_locals). Lambda comes from SAGE26's own
 * get_metaldependent_cooling_rate, the post-cap cooling mass from a second SAGE26
 * cooling_recipe_hot call with AGNrecipeOn 0, and the transcription is checked against
 * SAGE26's own do_AGN_heating bit for bit (check_against_do_AGN_heating); the oracle
 * refuses to write a fixture whose check fails. README.md documents every output.
 *
 * The file format is defined in README.md ("Fixture file format"). Every input field
 * must be consumed by the fixture reader for the selected routine, so a misspelt or
 * stray field is an error rather than a silently ignored value. All other struct
 * members are poisoned (all bits set: NaN for floating point, -1 for integers). The
 * parsing and output helpers are copies of oracle_c03.c's, which stays unchanged.
 *
 * This is SAGE26 reference tooling: it is compiled and linked only by build.sh, never
 * by Mimic's Makefile, and never against Mimic code.
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
#include "core_cool_func.h"
#include "model_cooling_heating.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 512
#define MAX_GALAXIES 8
#define NAME_LEN 96
#define LINE_LEN 512

/* The flags every cooling_recipe_hot and do_AGN_heating fixture runs with (R0) */
#define ORACLE_AGN_RECIPE 2
#define ORACLE_CGM_RECIPE 0

/* Values of intermediate.return_path (cooling_recipe_hot) */
enum return_path {
  PATH_NORMAL_TAIL = 0,   /* reached the tail at C:301-314 */
  PATH_OUTER_GUARD = 1,   /* HotGas > 0 && Vvir > 0 failed (C:141), tail with zero mass */
  PATH_LAMBDA_ZERO = 2,   /* lambda <= 0 early return (C:159-161) */
  PATH_RHO_RCOOL_ZERO = 3 /* rho_rcool <= 0 early return (C:167-169) */
};

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
  fprintf(stderr, "oracle_c04: %s: ", input_path != NULL ? input_path : "(no input)");
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

/** @brief Name of a per-galaxy field, <prefix>galaxy[<index>].<member> */
static const char *galaxy_field_with_prefix(const char *prefix, const int index,
                                            const char *member) {
  return field_name("%sgalaxy[%d].%s", prefix, index, member);
}

static const char *galaxy_field(const int index, const char *member) {
  return galaxy_field_with_prefix("", index, member);
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

/** @brief Write intermediate.<name>_absent, then intermediate.<name> (f64) if present */
static void out_optional_f64(const char *name, const int present, const double value) {
  out_i32(field_name("intermediate.%s_absent", name), present ? 0 : 1);
  if (present) {
    out_f64(field_name("intermediate.%s", name), value);
  }
}

static int same_f32(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static int same_f64(const double a, const double b) { return memcmp(&a, &b, sizeof(a)) == 0; }

// ============================================================================
// STATE INITIALISATION
// ============================================================================

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(&run_params, 0xff, sizeof(run_params));
}

static int read_galaxy_count(void) {
  const int ngal = in_i32("call.ngal");
  if (ngal < 1 || ngal > MAX_GALAXIES) {
    fatal("call.ngal = %d is outside [1, %d]", ngal, MAX_GALAXIES);
  }
  return ngal;
}

/**
 * @brief The R0 flags every cooling_recipe_hot and do_AGN_heating fixture carries
 *
 * The oracle's flags are fixed: the transcription below assumes CGMrecipeOn 0, and the
 * post-cap mass is defined as the return value of the same call with AGNrecipeOn 0.
 */
static void read_fixed_flags(void) {
  run_params.AGNrecipeOn = in_i32("params.AGNrecipeOn");
  run_params.CGMrecipeOn = in_i32("params.CGMrecipeOn");
  if (run_params.AGNrecipeOn != ORACLE_AGN_RECIPE || run_params.CGMrecipeOn != ORACLE_CGM_RECIPE) {
    fatal("params.AGNrecipeOn must be %d and params.CGMrecipeOn %d (the oracle's fixed R0 flags)",
          ORACLE_AGN_RECIPE, ORACLE_CGM_RECIPE);
  }
}

/** @brief The params do_AGN_heating and agn_accretion_compute read on the Bondi path (C:538-571) */
static void read_agn_params(void) {
  run_params.G = in_f64("params.G");
  run_params.RadioModeEfficiency = in_f64("params.RadioModeEfficiency");
  run_params.Hubble_h = in_f64("params.Hubble_h");
  run_params.UnitEnergy_in_cgs = in_f64("params.UnitEnergy_in_cgs");
  run_params.UnitTime_in_s = in_f64("params.UnitTime_in_s");
}

/**
 * @brief The galaxy fields every cooling, heating and transfer fixture sets
 *
 * A superset of what any one routine reads. The diagnostics tcool, tff, RcoolToRvir,
 * CoolingRate, Cooling and Heating are set explicitly (SAGE26's init_galaxy never
 * initialises CoolingRate, R/src/model_misc.c:172-198) so the output shows exactly what
 * the routine leaves untouched. Mvir is read by agn_accretion_compute (C:533) although
 * the Bondi branch does not use it.
 */
static void read_galaxy_block(const int i) {
  struct GALAXY *g = &galaxies[i];
  g->Type = in_i32(galaxy_field(i, "Type"));
  g->Mvir = in_f32(galaxy_field(i, "Mvir"));
  g->Rvir = in_f32(galaxy_field(i, "Rvir"));
  g->Vvir = in_f32(galaxy_field(i, "Vvir"));
  g->HotGas = in_f32(galaxy_field(i, "HotGas"));
  g->MetalsHotGas = in_f32(galaxy_field(i, "MetalsHotGas"));
  g->ColdGas = in_f32(galaxy_field(i, "ColdGas"));
  g->MetalsColdGas = in_f32(galaxy_field(i, "MetalsColdGas"));
  g->BlackHoleMass = in_f32(galaxy_field(i, "BlackHoleMass"));
  g->r_heat = in_f32(galaxy_field(i, "r_heat"));
  g->Cooling = in_f64(galaxy_field(i, "Cooling"));
  g->Heating = in_f64(galaxy_field(i, "Heating"));
  g->tcool = in_f32(galaxy_field(i, "tcool"));
  g->tff = in_f32(galaxy_field(i, "tff"));
  g->RcoolToRvir = in_f32(galaxy_field(i, "RcoolToRvir"));
  g->CoolingRate = in_f32(galaxy_field(i, "CoolingRate"));
}

/** @brief Write the twelve fields these routines can write, under <prefix>galaxy[i]. */
static void write_galaxy_state(const char *prefix, const int i, const struct GALAXY *g) {
  out_f32(galaxy_field_with_prefix(prefix, i, "HotGas"), g->HotGas);
  out_f32(galaxy_field_with_prefix(prefix, i, "MetalsHotGas"), g->MetalsHotGas);
  out_f32(galaxy_field_with_prefix(prefix, i, "ColdGas"), g->ColdGas);
  out_f32(galaxy_field_with_prefix(prefix, i, "MetalsColdGas"), g->MetalsColdGas);
  out_f32(galaxy_field_with_prefix(prefix, i, "BlackHoleMass"), g->BlackHoleMass);
  out_f32(galaxy_field_with_prefix(prefix, i, "r_heat"), g->r_heat);
  out_f64(galaxy_field_with_prefix(prefix, i, "Cooling"), g->Cooling);
  out_f64(galaxy_field_with_prefix(prefix, i, "Heating"), g->Heating);
  out_f32(galaxy_field_with_prefix(prefix, i, "tcool"), g->tcool);
  out_f32(galaxy_field_with_prefix(prefix, i, "tff"), g->tff);
  out_f32(galaxy_field_with_prefix(prefix, i, "RcoolToRvir"), g->RcoolToRvir);
  out_f32(galaxy_field_with_prefix(prefix, i, "CoolingRate"), g->CoolingRate);
}

/** @brief The interval and substep length of B:479 and B:508 (deltaT / effective_steps) */
struct call_time {
  double deltaT;
  double dt;
};

static struct call_time read_call_time(void) {
  const double age_galaxy_snap = in_f64("call.Age_galaxy_snap");
  const double halo_age = in_f64("call.halo_age");
  const int effective_steps = in_i32("call.effective_steps");
  if (effective_steps < 1) {
    fatal("call.effective_steps = %d must be positive", effective_steps);
  }
  struct call_time t;
  t.deltaT = age_galaxy_snap - halo_age; /* B:479 */
  t.dt = t.deltaT / effective_steps;     /* B:508 */
  /* dt <= 0 is unreachable on R0 (README.md, "Routines"): not a fixture case */
  if (!(t.dt > 0.0)) {
    fatal("dt = deltaT / effective_steps = %.17g must be positive", t.dt);
  }
  return t;
}

static void write_call_time(const struct call_time *t) {
  out_f64("intermediate.deltaT", t->deltaT);
  out_f64("intermediate.dt", t->dt);
}

// ============================================================================
// ROUTINES
// ============================================================================

static void run_get_metaldependent_cooling_rate(void) {
  const double logTemp = in_f64("call.logTemp");
  const double logZ = in_f64("call.logZ");
  check_all_fields_used();

  const double lambda = get_metaldependent_cooling_rate(logTemp, logZ);

  out_f64("return.lambda", lambda);
}

/**
 * @brief The local variables of cooling_recipe_hot that the package carries
 *
 * Every member except lambda is filled by transcribe_cooling_locals, a TRANSCRIPTION of
 * SAGE26 source; lambda is SAGE26's own get_metaldependent_cooling_rate. A member is
 * meaningful only when its has_* flag is set.
 */
struct cooling_locals {
  enum return_path path;
  int has_lambda_args; /* logTemp, logZ and lambda */
  int has_x;
  int has_rcool;
  double logTemp;
  double logZ;
  double lambda;
  double x;
  double rcool;
};

/**
 * @brief TRANSCRIPTION of cooling_recipe_hot's locals, R/src/model_cooling_heating.c:141-173
 *
 * Reproduces the statements of C:141-173 that form tcool, temp, logZ, log10(temp), x,
 * rho_rcool, rho0 and rcool, in SAGE26's expression shapes and operand order (no
 * expression here contains an add or subtract, so floating-point contraction cannot
 * differ from SAGE26's compilation of them). VIRIAL_TEMP_COEFF (C:77, 35.9) and
 * MU_IONISED (C:52, 0.59) are file-static in SAGE26 and are restated as literals.
 * PROTONMASS and BOLTZMANN come from SAGE26's macros.h. Lambda is not transcribed: it is
 * SAGE26's own get_metaldependent_cooling_rate called with the transcribed arguments.
 * check_against_do_AGN_heating verifies the transcribed x and rcool.
 */
static struct cooling_locals transcribe_cooling_locals(const struct GALAXY *g,
                                                       const struct params *p) {
  struct cooling_locals l;
  memset(&l, 0, sizeof(l));

  /* C:141 */
  if (!(g->HotGas > 0.0 && g->Vvir > 0.0)) {
    l.path = PATH_OUTER_GUARD;
    return l;
  }

  /* C:142: a float division stored in a double */
  const double tcool = g->Rvir / g->Vvir;

  /* C:150-155 */
  const double temp = 35.9 * g->Vvir * g->Vvir;
  double logZ = -10.0;
  if (g->MetalsHotGas > 0) {
    logZ = log10(g->MetalsHotGas / g->HotGas);
  }
  l.logTemp = log10(temp);
  l.logZ = logZ;

  /* C:157: SAGE26's own interpolation, not a transcription */
  l.lambda = get_metaldependent_cooling_rate(l.logTemp, l.logZ);
  l.has_lambda_args = 1;

  /* C:159-161 */
  if (l.lambda <= 0.0) {
    l.path = PATH_LAMBDA_ZERO;
    return l;
  }

  /* C:163-165 */
  double x = PROTONMASS * BOLTZMANN * temp / l.lambda;
  x /= (p->UnitDensity_in_cgs * p->UnitTime_in_s);
  const double rho_rcool = x / tcool * (1.5 * 0.59);
  l.x = x;
  l.has_x = 1;

  /* C:167-169 */
  if (rho_rcool <= 0.0) {
    l.path = PATH_RHO_RCOOL_ZERO;
    return l;
  }

  /* C:172-173 */
  const double rho0 = g->HotGas / (4 * M_PI * g->Rvir);
  l.rcool = sqrt(rho0 / rho_rcool);
  l.has_rcool = 1;
  l.path = PATH_NORMAL_TAIL;
  return l;
}

/** @brief SAGE26's own do_AGN_heating on a copy of the input galaxy */
struct agn_result {
  double coolingGas;
  struct GALAXY galaxy;
};

static struct agn_result call_do_AGN_heating(const struct GALAXY *input, const double coolingGas,
                                             const double dt, const double x, const double rcool) {
  galaxies[0] = *input;
  struct agn_result r;
  r.coolingGas = do_AGN_heating(coolingGas, 0, dt, x, rcool, galaxies, &run_params);
  r.galaxy = galaxies[0];
  return r;
}

/** @brief The return value and the five fields do_AGN_heating writes (C:607-620), bit for bit */
static int same_agn_outputs(const double ret_a, const struct GALAXY *a, const double ret_b,
                            const struct GALAXY *b) {
  return same_f64(ret_a, ret_b) && same_f32(a->BlackHoleMass, b->BlackHoleMass) &&
         same_f32(a->HotGas, b->HotGas) && same_f32(a->MetalsHotGas, b->MetalsHotGas) &&
         same_f32(a->r_heat, b->r_heat) && same_f64(a->Heating, b->Heating);
}

/** @brief Whether replacing a value by its upward and its downward neighbour changes an output */
struct sensitivity {
  int up;
  int down;
};

/** @brief Write intermediate.<name>_validated_up, _down, and <name>_validated (both) */
static void out_sensitivity(const char *name, const struct sensitivity *s) {
  out_i32(field_name("intermediate.%s_validated_up", name), s->up);
  out_i32(field_name("intermediate.%s_validated_down", name), s->down);
  out_i32(field_name("intermediate.%s_validated", name), s->up && s->down);
}

/** @brief Whether a double output of do_AGN_heating (its return value or Heating) differs */
static int double_outputs_differ(const struct agn_result *a, const struct agn_result *b) {
  return !same_f64(a->coolingGas, b->coolingGas) || !same_f64(a->galaxy.Heating, b->galaxy.Heating);
}

/**
 * @brief Check the transcribed x and rcool against SAGE26's own do_AGN_heating
 *
 * Feeds the transcribed x and rcool, with the post-cap mass, into SAGE26's do_AGN_heating
 * on a copy of the inputs; the return value and the five fields it writes must equal, bit
 * for bit, what cooling_recipe_hot produced. Then reruns the check with x, and separately
 * rcool, replaced by its upward neighbouring double (nextafter toward +inf) and then by
 * its downward one (toward -inf). Each direction's result records whether that
 * replacement changes a double output (the return value or Heating). The value is
 * validated on this fixture only if both directions do (two-sided validation).
 */
static void check_against_do_AGN_heating(const struct GALAXY *input, const double postcap,
                                         const double dt, const struct cooling_locals *l,
                                         const double main_return, const struct GALAXY *main_state,
                                         struct sensitivity *x_sens,
                                         struct sensitivity *rcool_sens) {
  const struct agn_result check = call_do_AGN_heating(input, postcap, dt, l->x, l->rcool);
  if (!same_agn_outputs(check.coolingGas, &check.galaxy, main_return, main_state)) {
    fatal("transcription check failed: do_AGN_heating with the transcribed x = %.17g and "
          "rcool = %.17g does not reproduce cooling_recipe_hot bit for bit",
          l->x, l->rcool);
  }

  const struct agn_result x_up =
      call_do_AGN_heating(input, postcap, dt, nextafter(l->x, INFINITY), l->rcool);
  const struct agn_result x_down =
      call_do_AGN_heating(input, postcap, dt, nextafter(l->x, -INFINITY), l->rcool);
  const struct agn_result rcool_up =
      call_do_AGN_heating(input, postcap, dt, l->x, nextafter(l->rcool, INFINITY));
  const struct agn_result rcool_down =
      call_do_AGN_heating(input, postcap, dt, l->x, nextafter(l->rcool, -INFINITY));
  x_sens->up = double_outputs_differ(&x_up, &check);
  x_sens->down = double_outputs_differ(&x_down, &check);
  rcool_sens->up = double_outputs_differ(&rcool_up, &check);
  rcool_sens->down = double_outputs_differ(&rcool_down, &check);
}

/** @brief Every field write_galaxy_state records, bit for bit */
static int same_written_state(const struct GALAXY *a, const struct GALAXY *b) {
  return same_f32(a->HotGas, b->HotGas) && same_f32(a->MetalsHotGas, b->MetalsHotGas) &&
         same_f32(a->ColdGas, b->ColdGas) && same_f32(a->MetalsColdGas, b->MetalsColdGas) &&
         same_f32(a->BlackHoleMass, b->BlackHoleMass) && same_f32(a->r_heat, b->r_heat) &&
         same_f64(a->Cooling, b->Cooling) && same_f64(a->Heating, b->Heating) &&
         same_f32(a->tcool, b->tcool) && same_f32(a->tff, b->tff) &&
         same_f32(a->RcoolToRvir, b->RcoolToRvir) && same_f32(a->CoolingRate, b->CoolingRate);
}

/**
 * @brief Refuse a fixture whose transcribed return path disagrees with what SAGE26 wrote
 *
 * The outer guard writes tcool = 0, leaves tff, RcoolToRvir and Cooling untouched, and
 * its tail writes CoolingRate = 0 from the zero mass (dt being positive);
 * the two early returns leave RcoolToRvir, Cooling and CoolingRate untouched; the normal
 * tail writes RcoolToRvir = rcool / Rvir (C:175), which must equal the transcribed rcool
 * divided by Rvir and narrowed to float, and writes CoolingRate.
 */
static void check_return_path(const struct cooling_locals *l, const struct GALAXY *in,
                              const struct GALAXY *out, const double ret) {
  int consistent = 1;
  switch (l->path) {
  case PATH_OUTER_GUARD:
    consistent = same_f64(ret, 0.0) && same_f32(out->tcool, 0.0f) && same_f32(out->tff, in->tff) &&
                 same_f32(out->RcoolToRvir, in->RcoolToRvir) &&
                 same_f64(out->Cooling, in->Cooling) && same_f32(out->CoolingRate, 0.0f);
    break;
  case PATH_LAMBDA_ZERO:
  case PATH_RHO_RCOOL_ZERO:
    consistent = same_f64(ret, 0.0) && same_f32(out->RcoolToRvir, in->RcoolToRvir) &&
                 same_f64(out->Cooling, in->Cooling) && same_f32(out->CoolingRate, in->CoolingRate);
    break;
  case PATH_NORMAL_TAIL: {
    const float ratio = (float)(l->rcool / in->Rvir);
    consistent = same_f32(out->RcoolToRvir, ratio) && !same_f32(out->CoolingRate, in->CoolingRate);
    break;
  }
  }
  if (!consistent) {
    fatal("transcribed return path %d disagrees with the diagnostics cooling_recipe_hot wrote "
          "(the input's diagnostic bit patterns must differ from what the routine can write)",
          (int)l->path);
  }
}

static void run_cooling_recipe_hot(void) {
  read_fixed_flags();
  read_agn_params();
  run_params.UnitDensity_in_cgs = in_f64("params.UnitDensity_in_cgs");
  const struct call_time t = read_call_time();
  read_galaxy_block(0);
  check_all_fields_used();

  const struct GALAXY input = galaxies[0];
  const struct cooling_locals l = transcribe_cooling_locals(&input, &run_params);

  /* B:508: the call itself, on the fixture input */
  const double coolingGas = cooling_recipe_hot(0, t.dt, galaxies, &run_params);
  const struct GALAXY after_recipe = galaxies[0];
  check_return_path(&l, &input, &after_recipe, coolingGas);

  /* The post-cap, pre-suppression mass: the same call with AGNrecipeOn 0 returns the
   * capped mass of C:288-292 untouched by do_AGN_heating (C:297, C:314). */
  int has_postcap = 0;
  double postcap = 0.0;
  if (l.path == PATH_NORMAL_TAIL) {
    struct params agn_off = run_params;
    agn_off.AGNrecipeOn = 0;
    galaxies[0] = input;
    postcap = cooling_recipe_hot(0, t.dt, galaxies, &agn_off);
    has_postcap = 1;
  }

  /* C:297: do_AGN_heating runs only on a positive post-cap mass (AGNrecipeOn is 2) */
  const int agn_entered = has_postcap && run_params.AGNrecipeOn > 0 && postcap > 0.0;
  struct sensitivity x_sens = {0, 0};
  struct sensitivity rcool_sens = {0, 0};
  if (agn_entered) {
    check_against_do_AGN_heating(&input, postcap, t.dt, &l, coolingGas, &after_recipe, &x_sens,
                                 &rcool_sens);
  } else if (has_postcap) {
    /* Not entered: the AGNrecipeOn 0 call must have done exactly what the real one did */
    if (!same_f64(postcap, coolingGas) || !same_written_state(&galaxies[0], &after_recipe)) {
      fatal("post-cap mass is not positive, yet the AGNrecipeOn 0 call differs from the real one");
    }
  }

  /* B:509: the sequence, cool_gas_onto_galaxy on the returned (suppressed) mass */
  galaxies[0] = after_recipe;
  cool_gas_onto_galaxy(0, coolingGas, galaxies);
  const struct GALAXY after_sequence = galaxies[0];

  write_call_time(&t);
  out_f64("return.coolingGas", coolingGas);
  write_galaxy_state("", 0, &after_recipe);
  out_i32("intermediate.return_path", (int)l.path);
  out_optional_f64("logTemp", l.has_lambda_args, l.logTemp);
  out_optional_f64("logZ", l.has_lambda_args, l.logZ);
  out_optional_f64("lambda", l.has_lambda_args, l.lambda);
  out_optional_f64("x", l.has_x, l.x);
  out_optional_f64("rcool", l.has_rcool, l.rcool);
  out_optional_f64("CoolingGas_postcap", has_postcap, postcap);
  out_i32("intermediate.do_AGN_heating_entered", agn_entered);
  out_sensitivity("x", &x_sens);
  out_sensitivity("rcool", &rcool_sens);
  out_f64("intermediate.CoolingGas_suppressed", coolingGas);
  write_galaxy_state("intermediate.sequence.", 0, &after_sequence);
}

static void run_do_AGN_heating(void) {
  read_fixed_flags();
  read_agn_params();
  const struct call_time t = read_call_time();
  const double coolingGas = in_f64("call.coolingGas");
  const double x = in_f64("call.x");
  const double rcool = in_f64("call.rcool");
  read_galaxy_block(0);
  check_all_fields_used();
  /* The package radio module's gate is SAGE26's (C:297): a positive cooling mass */
  if (!(coolingGas > 0.0)) {
    fatal("call.coolingGas must be positive (do_AGN_heating is only entered then, C:297)");
  }

  const double suppressed = do_AGN_heating(coolingGas, 0, t.dt, x, rcool, galaxies, &run_params);

  write_call_time(&t);
  out_f64("return.coolingGas", suppressed);
  write_galaxy_state("", 0, &galaxies[0]);
}

static void run_cool_gas_onto_galaxy(void) {
  const double coolingGas = in_f64("call.coolingGas");
  read_galaxy_block(0);
  check_all_fields_used();
  /* cooling_recipe_hot never returns a negative mass (its XASSERT, C:308) */
  if (!(coolingGas >= 0.0)) {
    fatal("call.coolingGas must be non-negative");
  }

  cool_gas_onto_galaxy(0, coolingGas, galaxies);

  write_galaxy_state("", 0, &galaxies[0]);
}

/**
 * @brief TRANSCRIPTION of the end-of-halo normalisation, R/src/core_build_model.c:590-604
 *
 * evolve_galaxies is static in core_build_model.c, which is not in this link, so these lines are
 * restated here for Cooling and Heating only (OutflowRate's
 * is oracle_c05.c's normalise_outflow_rate):
 *
 *   const double deltaT = run_params->Age[galaxies[0].SnapNum] - halo_age;
 *   const double inv_deltaT = 1.0/deltaT;
 *   for each galaxy p: if mergeType > 0 skip; Cooling *= inv_deltaT; Heating *= inv_deltaT;
 *
 * call.Age[<snap>] supplies Age for every SnapNum a galaxy carries.
 */
static void run_normalise_cooling_heating(void) {
  const int ngal = read_galaxy_count();
  const double halo_age = in_f64("call.halo_age");
  double age[MAX_GALAXIES];
  for (int i = 0; i < ngal; i++) {
    struct GALAXY *g = &galaxies[i];
    g->Type = in_i32(galaxy_field(i, "Type"));
    g->SnapNum = in_i32(galaxy_field(i, "SnapNum"));
    g->mergeType = in_i32(galaxy_field(i, "mergeType"));
    g->Cooling = in_f64(galaxy_field(i, "Cooling"));
    g->Heating = in_f64(galaxy_field(i, "Heating"));
    g->tcool = in_f32(galaxy_field(i, "tcool"));
    g->tff = in_f32(galaxy_field(i, "tff"));
    g->RcoolToRvir = in_f32(galaxy_field(i, "RcoolToRvir"));
    g->CoolingRate = in_f32(galaxy_field(i, "CoolingRate"));
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
    normalised[p] = 1;
  }

  out_f64("intermediate.deltaT", deltaT);
  out_f64("intermediate.inv_deltaT", inv_deltaT);
  for (int i = 0; i < ngal; i++) {
    const struct GALAXY *g = &galaxies[i];
    /* The galaxy's own interval, Age[SnapNum] - halo_age (B:479), which B:591 does not use */
    out_f64(galaxy_field_with_prefix("intermediate.", i, "deltaT"), age[i] - halo_age);
    out_i32(galaxy_field_with_prefix("intermediate.", i, "normalised"), normalised[i]);
    out_f64(galaxy_field(i, "Cooling"), g->Cooling);
    out_f64(galaxy_field(i, "Heating"), g->Heating);
    out_f32(galaxy_field(i, "tcool"), g->tcool);
    out_f32(galaxy_field(i, "tff"), g->tff);
    out_f32(galaxy_field(i, "RcoolToRvir"), g->RcoolToRvir);
    out_f32(galaxy_field(i, "CoolingRate"), g->CoolingRate);
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
    {"get_metaldependent_cooling_rate", run_get_metaldependent_cooling_rate},
    {"cooling_recipe_hot", run_cooling_recipe_hot},
    {"do_AGN_heating", run_do_AGN_heating},
    {"cool_gas_onto_galaxy", run_cool_gas_onto_galaxy},
    {"normalise_cooling_heating", run_normalise_cooling_heating},
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

  /* Once per process: the loader shifts its static metallicity grid on every call
   * (R/src/core_cool_func.c:69-72). It reads ROOT_DIR/src/auxdata/CoolFunctions. */
  read_cooling_functions();

  output_path = argv[2];
  output_file = fopen(output_path, "w");
  if (output_file == NULL) {
    fatal("cannot open output %s: %s", output_path, strerror(errno));
  }
  fprintf(output_file,
          "# SAGE26 oracle output for %s, written by oracle_c04 (regenerate.sh); do not edit.\n",
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
