/**
 * @file    oracle_c11.c
 * @brief   SAGE26 function-level oracle for the c11 CGMrecipeOn 1 cooling and radio-heating
 * routines
 *
 * Reads one fixture input file, initialises the SAGE26 struct GALAXY and struct params fields its
 * reader for the selected routine consumes, calls the routine from the SAGE26 objects this
 * executable is linked against, and writes a fixture output file holding the routine's return
 * value, every field it writes and SAGE26's locals by name, each checked against SAGE26's own
 * result and tested two-sided (oracle README, "The c11 Group").
 *
 * Usage: oracle_c11 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one; R/ is the staged SAGE26 source root,
 * C = R/src/model_cooling_heating.c, H = R/src/model_halo_properties.c, B =
 * R/src/core_build_model.c). Every routine is a DIRECT call of a public SAGE26 routine:
 *   interpolate_clustering_mass  H:98-132
 *   cooling_recipe_hot_streams   cooling_recipe_hot (C:135-315) with CGMrecipeOn 1, so the cooling
 *                                mass is the cold streams (C:202-286); AGNrecipeOn 2 reaches
 *                                do_AGN_heating (C:587-627) and the static agn_accretion_compute
 *                                (C:526-576) through it; AGNrecipeOn 0 does not
 *   cooling_recipe_cgm           C:317-391 with CGMrecipeOn 1; AGNrecipeOn 2 reaches the static
 *                                do_AGN_heating_cgm (C:635-680) and agn_accretion_compute through
 *                                it, AGNrecipeOn 0 does not
 *   do_AGN_heating_cgm_on        do_AGN_heating (C:587-627) called on its own with CGMrecipeOn 1
 *                                and AGNrecipeOn 2
 * The static routines are never called directly: they run inside SAGE26's public routines, and
 * their locals come from the TRANSCRIPTIONS below (tr_*), each statement cited to its SAGE26 line.
 *
 * Checks (the oracle refuses to write a fixture otherwise):
 * - SAGE26's call changed no byte of the galaxy outside the recorded fields;
 * - the full transcription reproduces SAGE26's call in every recorded field and the return value
 *   bit for bit, so every recorded local is TAIL-CHECKED;
 * - cooling_recipe_hot_streams: the locals SAGE26 passes to its public do_AGN_heating (x, rcool,
 *   the post-cap mass) and to interpolate_clustering_mass (z) are CONTINUATION-CHECKED: SAGE26's
 *   own do_AGN_heating fed the transcribed x, rcool and post-cap mass reproduces the call's AGN
 *   outputs, and the transcription calls SAGE26's own interpolate_clustering_mass; the post-cap
 *   mass also equals the return value of SAGE26's call with AGNrecipeOn 0 (C:297, C:314);
 *   cooling_recipe_cgm: the post-cap mass equals SAGE26's call with AGNrecipeOn 0 likewise.
 * Two-sided validation (campaign decision D25): each recorded double local is replaced in the
 * transcription by its upward and, separately, its downward neighbouring double (nextafter), and
 * the output records whether each replacement changes at least one recorded field or the return.
 *
 * The cooling tables are loaded once per process by SAGE26's own read_cooling_functions()
 * (R/src/core_cool_func.c:65-99), from ROOT_DIR as build.sh sets it. The parsing and output
 * helpers are copies of oracle_c04.c's, which stays unchanged.
 *
 * This is SAGE26 reference tooling: it is compiled and linked only by build.sh, never by Mimic's
 * Makefile, and never against Mimic code.
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
#include "model_halo_properties.h"
#include "model_misc.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 512
#define MAX_GALAXIES 1
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
  fprintf(stderr, "oracle_c11: %s: ", input_path != NULL ? input_path : "(no input)");
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

static int same_f32(const float a, const float b) { return memcmp(&a, &b, sizeof(a)) == 0; }

static int same_f64(const double a, const double b) { return memcmp(&a, &b, sizeof(a)) == 0; }

// ============================================================================
// STATE, FLAGS AND RECORDED FIELDS
// ============================================================================

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(&run_params, 0xff, sizeof(run_params));
}

/** @brief CGMrecipeOn 1 (R4.par:54) and AGNrecipeOn 2 (R4.par:49) or a 0 probe */
static void read_flags(const int allow_agn_off) {
  run_params.AGNrecipeOn = in_i32("params.AGNrecipeOn");
  run_params.CGMrecipeOn = in_i32("params.CGMrecipeOn");
  if (run_params.CGMrecipeOn != 1) {
    fatal("params.CGMrecipeOn must be 1 (every c11 routine is a CGMrecipeOn 1 call)");
  }
  if (!(run_params.AGNrecipeOn == 2 || (allow_agn_off && run_params.AGNrecipeOn == 0))) {
    fatal("params.AGNrecipeOn must be 2 (R4's Bondi recipe)%s", allow_agn_off ? " or 0" : "");
  }
}

/** @brief The params the cooling and AGN arithmetic reads (C:145-148, :163-164, :352, :538-571) */
static void read_units(void) {
  run_params.G = in_f64("params.G");
  run_params.RadioModeEfficiency = in_f64("params.RadioModeEfficiency");
  run_params.Hubble_h = in_f64("params.Hubble_h");
  run_params.UnitEnergy_in_cgs = in_f64("params.UnitEnergy_in_cgs");
  run_params.UnitTime_in_s = in_f64("params.UnitTime_in_s");
  run_params.UnitDensity_in_cgs = in_f64("params.UnitDensity_in_cgs");
}

/**
 * @brief The streams' cosmology: Omega, MShockMsun and the stored and current snapshots' ZZ
 *
 * Every other ZZ entry stays poisoned (NaN), so a read of the wrong snapshot surfaces as NaN.
 * call.current_SnapNum names the halo's current snapshot, which SAGE26's streams must not read
 * (C:205 reads ZZ[galaxies[gal].SnapNum], the stored progenitor snapshot).
 */
static void read_stream_cosmology(void) {
  run_params.Omega = in_f64("params.Omega");
  run_params.MShockMsun = in_f64("params.MShockMsun");
  run_params.Snaplistlen = in_i32("params.Snaplistlen");
  const int current = in_i32("call.current_SnapNum");
  const double zz_stored = in_f64("params.ZZ_stored");
  const double zz_current = in_f64("params.ZZ_current");
  const int stored = galaxies[0].SnapNum;
  if (run_params.Snaplistlen < 1 || run_params.Snaplistlen > ABSOLUTEMAXSNAPS) {
    fatal("params.Snaplistlen = %d is outside [1, %d]", run_params.Snaplistlen, ABSOLUTEMAXSNAPS);
  }
  if (stored < 0 || stored >= run_params.Snaplistlen || current < 0 ||
      current >= run_params.Snaplistlen || current == stored) {
    fatal("galaxy[0].SnapNum = %d and call.current_SnapNum = %d must be distinct snapshots in "
          "[0, Snaplistlen)",
          stored, current);
  }
  run_params.ZZ[stored] = zz_stored;
  run_params.ZZ[current] = zz_current;
}

/** @brief The galaxy fields every c11 cooling or heating fixture sets (a superset of any reader) */
static void read_galaxy_block(void) {
  struct GALAXY *g = &galaxies[0];
  g->Type = in_i32(galaxy_field(0, "Type"));
  g->SnapNum = in_i32(galaxy_field(0, "SnapNum"));
  g->Mvir = in_f32(galaxy_field(0, "Mvir"));
  g->Rvir = in_f32(galaxy_field(0, "Rvir"));
  g->Vvir = in_f32(galaxy_field(0, "Vvir"));
  g->HotGas = in_f32(galaxy_field(0, "HotGas"));
  g->MetalsHotGas = in_f32(galaxy_field(0, "MetalsHotGas"));
  g->CGMgas = in_f32(galaxy_field(0, "CGMgas"));
  g->MetalsCGMgas = in_f32(galaxy_field(0, "MetalsCGMgas"));
  g->ColdGas = in_f32(galaxy_field(0, "ColdGas"));
  g->MetalsColdGas = in_f32(galaxy_field(0, "MetalsColdGas"));
  g->BlackHoleMass = in_f32(galaxy_field(0, "BlackHoleMass"));
  g->r_heat = in_f32(galaxy_field(0, "r_heat"));
  g->Cooling = in_f64(galaxy_field(0, "Cooling"));
  g->Heating = in_f64(galaxy_field(0, "Heating"));
  g->tcool = in_f32(galaxy_field(0, "tcool"));
  g->tff = in_f32(galaxy_field(0, "tff"));
  g->RcoolToRvir = in_f32(galaxy_field(0, "RcoolToRvir"));
  g->CoolingRate = in_f32(galaxy_field(0, "CoolingRate"));
  g->mdot_cool = in_f32(galaxy_field(0, "mdot_cool"));
  g->mdot_stream = in_f32(galaxy_field(0, "mdot_stream"));
}

#define NUM_RECORDED_F32 14
static const char *const RECORDED_F32[NUM_RECORDED_F32] = {
    "HotGas",        "MetalsHotGas",  "CGMgas",    "MetalsCGMgas", "ColdGas",
    "MetalsColdGas", "BlackHoleMass", "r_heat",    "tcool",        "tff",
    "RcoolToRvir",   "CoolingRate",   "mdot_cool", "mdot_stream"};

static float *recorded_f32(struct GALAXY *g, const int k) {
  float *const fields[NUM_RECORDED_F32] = {
      &g->HotGas,        &g->MetalsHotGas,  &g->CGMgas,    &g->MetalsCGMgas, &g->ColdGas,
      &g->MetalsColdGas, &g->BlackHoleMass, &g->r_heat,    &g->tcool,        &g->tff,
      &g->RcoolToRvir,   &g->CoolingRate,   &g->mdot_cool, &g->mdot_stream};
  return fields[k];
}

/** @brief Write the sixteen fields these routines can write, under galaxy[0] */
static void write_galaxy_state(const struct GALAXY *g) {
  struct GALAXY copy = *g;
  for (int k = 0; k < NUM_RECORDED_F32; k++) {
    out_f32(galaxy_field(0, RECORDED_F32[k]), *recorded_f32(&copy, k));
  }
  out_f64(galaxy_field(0, "Cooling"), g->Cooling);
  out_f64(galaxy_field(0, "Heating"), g->Heating);
}

/** @brief Every recorded field bit for bit */
static int same_recorded_state(const struct GALAXY *a, const struct GALAXY *b) {
  struct GALAXY ca = *a, cb = *b;
  for (int k = 0; k < NUM_RECORDED_F32; k++) {
    if (!same_f32(*recorded_f32(&ca, k), *recorded_f32(&cb, k))) {
      return 0;
    }
  }
  return same_f64(a->Cooling, b->Cooling) && same_f64(a->Heating, b->Heating);
}

/** @brief Refuse a SAGE26 call that changed a galaxy byte outside the recorded fields */
static void check_only_recorded_changed(const struct GALAXY *before, const struct GALAXY *after) {
  struct GALAXY masked = *after;
  struct GALAXY src = *before;
  for (int k = 0; k < NUM_RECORDED_F32; k++) {
    *recorded_f32(&masked, k) = *recorded_f32(&src, k);
  }
  masked.Cooling = before->Cooling;
  masked.Heating = before->Heating;
  if (memcmp(&masked, before, sizeof(masked)) != 0) {
    fatal("SAGE26's call changed a galaxy field outside the recorded fields");
  }
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
// RECORDED LOCALS
// ============================================================================

/** @brief Every SAGE26 local a c11 fixture can record, by its output name */
enum local_id {
  L_TCOOL_DYN, /* cooling_recipe_hot's tcool = Rvir / Vvir (C:142) */
  L_LAMBDA,    /* the cooling function (C:157, C:329) */
  L_X,         /* x (C:163-164, C:332-333) */
  L_RCOOL,     /* rcool (C:173, C:360) */
  L_Z,         /* ZZ[SnapNum] (C:205) */
  L_MSHOCK,
  L_MASS_RATIO,
  L_LOG_MSTAR,
  L_MSTAR,
  L_FMSTAR,
  L_RATIO,
  L_EXPONENT,
  L_FSTREAM_CRIT,
  L_FSTREAM,
  L_COLD,
  L_HOTHALO,
  L_RHO0,
  L_RHO_EFF,
  L_TCOOL_CGM,
  L_G_ACCEL,
  L_TFF_CGM,
  L_PRECAP,
  L_POSTCAP,
  L_SUPPRESSED,
  L_ACCRETED,
  L_HEATING,
  L_ZZ,
  L_FZ,
  L_COUNT
};

static const char *const LOCAL_NAME[L_COUNT] = {"tcool",
                                                "lambda",
                                                "x",
                                                "rcool",
                                                "z",
                                                "M_shock",
                                                "mass_ratio",
                                                "log_mstar",
                                                "Mstar",
                                                "fMstar",
                                                "ratio",
                                                "exponent",
                                                "f_stream_criterion",
                                                "f_stream",
                                                "cold_stream_cooling",
                                                "hot_halo_cooling",
                                                "rho0",
                                                "rho_eff",
                                                "tcool",
                                                "g_accel",
                                                "tff",
                                                "CoolingGas_precap",
                                                "CoolingGas_postcap",
                                                "CoolingGas_suppressed",
                                                "AGNaccreted",
                                                "AGNheating",
                                                "zz",
                                                "fz"};

/** @brief Integer flags a transcription records (-1 when the path does not evaluate them) */
enum flag_id {
  F_RETURN_PATH,          /* 0 tail, 1 outer guard, 2 lambda <= 0, 3 rho_rcool <= 0 (hot) */
  F_ENTERED,              /* CGM: CGMgas > 0 && Vvir > 0 (C:321) */
  F_LAMBDA_POSITIVE,      /* CGM: lambda > 0 (C:331) */
  F_RATIO_NONPOSITIVE,    /* C:229 */
  F_EXPONENT_CAPPED_HIGH, /* C:240 */
  F_EXPONENT_CAPPED_LOW,  /* C:241 */
  F_CUTOFF,               /* C:263 */
  F_FSTREAM_CAPPED_HIGH,  /* C:270 */
  F_FSTREAM_FLOORED,      /* C:271 */
  F_CAP,                  /* 0 none, 1 capped to the reservoir, 2 floored (C:288-292, C:366-370) */
  F_AGN_ENTERED,          /* C:297, C:372 */
  F_SUPPRESSED,           /* 1 suppression formula, 0 zeroed (C:590-595, C:638-643) */
  F_SATURATED,            /* the CGMrecipeOn 1 0.99 * rcool saturation zeroed it */
  F_RESERVOIR_POSITIVE,   /* C:599, C:648 */
  F_EDDINGTON_CAPPED,     /* C:557 */
  F_ACCRETION_CAPPED,     /* C:560 */
  F_HEATING_CAPPED,       /* C:568-571 */
  F_RATCHET_RAISED,       /* C:612-616, C:658-662 */
  F_RHEAT_CAPPED,         /* C:623-624, C:663-664 */
  F_RECAPPED,             /* C:675-677 */
  F_UCHUU,                /* H:118 */
  F_Z_CLAMPED_LOW,        /* H:123 */
  F_Z_CLAMPED_HIGH,       /* H:124 */
  F_IZ,                   /* H:126-128 */
  F_IZ_CLAMPED,           /* H:128 */
  F_COUNT
};

static const char *const FLAG_NAME[F_COUNT] = {"return_path",
                                               "cgm_entered",
                                               "lambda_positive",
                                               "ratio_nonpositive",
                                               "exponent_capped_high",
                                               "exponent_capped_low",
                                               "stream_cutoff",
                                               "f_stream_capped_high",
                                               "f_stream_floored",
                                               "reservoir_cap",
                                               "agn_entered",
                                               "agn_suppressed",
                                               "agn_saturated",
                                               "agn_reservoir_positive",
                                               "eddington_capped",
                                               "accretion_capped",
                                               "heating_capped",
                                               "ratchet_raised",
                                               "rheat_capped",
                                               "recapped",
                                               "uchuu",
                                               "z_clamped_low",
                                               "z_clamped_high",
                                               "iz",
                                               "iz_clamped"};

/** @brief One transcription run: the locals, flags, end state and return value */
struct tr_result {
  double local[L_COUNT];
  int present[L_COUNT];
  int flag[F_COUNT];
  struct GALAXY galaxy;
  double ret;
};

/** @brief The local a run replaces by a neighbour (L_COUNT for none) */
struct tr_override {
  enum local_id id;
  double value;
};

static const struct tr_override NO_OVERRIDE = {L_COUNT, 0.0};

static void tr_init(struct tr_result *r, const struct GALAXY *input) {
  for (int i = 0; i < L_COUNT; i++) {
    r->local[i] = 0.0;
    r->present[i] = 0;
  }
  for (int i = 0; i < F_COUNT; i++) {
    r->flag[i] = -1;
  }
  r->galaxy = *input;
  r->ret = 0.0;
}

/** @brief Record a local (or the override's neighbour in its place) and return what continues */
static double take(struct tr_result *r, const struct tr_override *ov, const enum local_id id,
                   const double value) {
  r->local[id] = (ov->id == id) ? ov->value : value;
  r->present[id] = 1;
  return r->local[id];
}

// ============================================================================
// TRANSCRIPTIONS
// ============================================================================

/**
 * @brief TRANSCRIPTION of the static agn_accretion_compute, Bondi branch (C:526-576)
 *
 * Only AGNrecipeOn 2 reaches here (read_flags). AGN_HEATING_COEFF_KMS (C:72, 1.34e5),
 * EDDINGTON_LUM_PER_MSUN_CGS (C:65, 1.3e38), AGN_RADIATIVE_EFFICIENCY (C:66, 0.1) and C_SQ_KMS2
 * (C:67, 9.0e10) are file-static in SAGE26 and restated as literals.
 */
static void tr_agn_accretion_compute(const struct GALAXY *g, const double dt, const double x,
                                     const double coolingGas, const double reservoir_mass,
                                     const struct params *p, struct tr_result *r,
                                     const struct tr_override *ov, double *accreted_out,
                                     double *heating_out) {
  const double Vvir = g->Vvir;            /* C:532 */
  const double BHmass = g->BlackHoleMass; /* C:534 */

  /* C:540 */
  double AGNrate = (2.5 * M_PI * p->G) * (0.375 * 0.6 * x) * BHmass * p->RadioModeEfficiency;

  /* C:554-557 */
  const double EDDrate = (1.3e38 * BHmass * 1e10 / p->Hubble_h) /
                         (p->UnitEnergy_in_cgs / p->UnitTime_in_s) / (0.1 * 9.0e10);
  r->flag[F_EDDINGTON_CAPPED] = (AGNrate > EDDrate) ? 1 : 0;
  if (AGNrate > EDDrate)
    AGNrate = EDDrate;

  /* C:559-560 */
  double AGNaccreted = AGNrate * dt;
  r->flag[F_ACCRETION_CAPPED] = (AGNaccreted > reservoir_mass) ? 1 : 0;
  if (AGNaccreted > reservoir_mass)
    AGNaccreted = reservoir_mass;

  /* C:563-572 */
  double AGNheating = 0.0;
  r->flag[F_HEATING_CAPPED] = 0;
  if (Vvir > 0.0) {
    const double AGNcoeff = (1.34e5 / Vvir) * (1.34e5 / Vvir);
    AGNheating = AGNcoeff * AGNaccreted;
    if (AGNheating > coolingGas && AGNcoeff > 0.0) {
      AGNaccreted = coolingGas / AGNcoeff;
      AGNheating = coolingGas;
      r->flag[F_HEATING_CAPPED] = 1;
    }
  }

  *accreted_out = take(r, ov, L_ACCRETED, AGNaccreted);
  *heating_out = take(r, ov, L_HEATING, AGNheating);
}

/** @brief TRANSCRIPTION of the suppression of C:590-595 and C:638-643 */
static double tr_suppress(const struct GALAXY *g, const double rcool, double coolingGas,
                          const struct params *p, struct tr_result *r) {
  if (g->r_heat < rcool && !(p->CGMrecipeOn == 1 && g->r_heat >= 0.99 * rcool)) {
    coolingGas = (1.0 - g->r_heat / rcool) * coolingGas;
    r->flag[F_SUPPRESSED] = 1;
    r->flag[F_SATURATED] = 0;
  } else {
    coolingGas = 0.0;
    r->flag[F_SUPPRESSED] = 0;
    r->flag[F_SATURATED] = (g->r_heat < rcool) ? 1 : 0;
  }
  return coolingGas;
}

/** @brief TRANSCRIPTION of do_AGN_heating (C:587-627), Bondi accretion */
static double tr_do_agn_heating(struct GALAXY *g, double coolingGas, const double dt,
                                const double x, const double rcool, const struct params *p,
                                struct tr_result *r, const struct tr_override *ov) {
  coolingGas = take(r, ov, L_SUPPRESSED, tr_suppress(g, rcool, coolingGas, p, r)); /* C:590-595 */

  r->flag[F_RESERVOIR_POSITIVE] = (g->HotGas > 0.0) ? 1 : 0;
  r->flag[F_RATCHET_RAISED] = 0;
  if (g->HotGas > 0.0) { /* C:599 */
    double AGNaccreted, AGNheating;
    tr_agn_accretion_compute(g, dt, x, coolingGas, g->HotGas, p, r, ov, &AGNaccreted,
                             &AGNheating); /* C:601-603 */

    const double metallicity = get_metallicity(g->HotGas, g->MetalsHotGas); /* C:605-606 */
    g->BlackHoleMass += AGNaccreted;                                        /* C:607 */
    g->HotGas -= AGNaccreted;                                               /* C:608 */
    g->MetalsHotGas -= metallicity * AGNaccreted;                           /* C:609 */

    if (g->r_heat < rcool && coolingGas > 0.0) { /* C:612-616 */
      const double r_heat_new = (AGNheating / coolingGas) * rcool;
      if (r_heat_new > g->r_heat) {
        g->r_heat = r_heat_new;
        r->flag[F_RATCHET_RAISED] = 1;
      }
    }

    if (AGNheating > 0.0) /* C:618-620 */
      g->Heating += 0.5 * AGNheating * g->Vvir * g->Vvir;
  }

  r->flag[F_RHEAT_CAPPED] = 0;
  if (p->CGMrecipeOn > 0 && g->r_heat > g->Rvir) { /* C:623-624 */
    g->r_heat = g->Rvir;
    r->flag[F_RHEAT_CAPPED] = 1;
  }
  return coolingGas; /* C:626 */
}

/** @brief TRANSCRIPTION of the static do_AGN_heating_cgm (C:635-680), Bondi accretion */
static double tr_do_agn_heating_cgm(struct GALAXY *g, double coolingGas, const double dt,
                                    const double x, const double rcool, const struct params *p,
                                    struct tr_result *r, const struct tr_override *ov) {
  coolingGas = take(r, ov, L_SUPPRESSED, tr_suppress(g, rcool, coolingGas, p, r)); /* C:638-643 */

  r->flag[F_RESERVOIR_POSITIVE] = (g->CGMgas > 0.0) ? 1 : 0;
  r->flag[F_RATCHET_RAISED] = 0;
  r->flag[F_RHEAT_CAPPED] = 0;
  r->flag[F_RECAPPED] = 0;
  if (g->CGMgas > 0.0) { /* C:648 */
    double AGNaccreted, AGNheating;
    tr_agn_accretion_compute(g, dt, x, coolingGas, g->CGMgas, p, r, ov, &AGNaccreted,
                             &AGNheating);                                  /* C:650-652 */
    const double metallicity = get_metallicity(g->CGMgas, g->MetalsCGMgas); /* C:653 */
    g->BlackHoleMass += AGNaccreted;                                        /* C:654 */
    g->CGMgas -= AGNaccreted;                                               /* C:655 */
    g->MetalsCGMgas -= metallicity * AGNaccreted;                           /* C:656 */

    if (g->r_heat < rcool && coolingGas > 0.0) { /* C:658-662 */
      const double r_heat_new = (AGNheating / coolingGas) * rcool;
      if (r_heat_new > g->r_heat) {
        g->r_heat = r_heat_new;
        r->flag[F_RATCHET_RAISED] = 1;
      }
    }
    if (g->r_heat > g->Rvir) { /* C:663-664 */
      g->r_heat = g->Rvir;
      r->flag[F_RHEAT_CAPPED] = 1;
    }

    if (AGNheating > 0.0) /* C:666-667 */
      g->Heating += 0.5 * AGNheating * g->Vvir * g->Vvir;

    if (coolingGas > g->CGMgas) { /* C:675-677 */
      coolingGas = g->CGMgas;
      r->flag[F_RECAPPED] = 1;
    }
  }
  return coolingGas; /* C:679 */
}

/**
 * @brief TRANSCRIPTION of cooling_recipe_hot with CGMrecipeOn 1 (C:135-315)
 *
 * The cold-stream branch (C:202-286). VIRIAL_TEMP_COEFF (C:77, 35.9), MU_IONISED (C:52, 0.59),
 * DB06_STREAM_MASS_FACTOR (C:104, 3.0) and STREAM_THRESHOLD_WIDTH_DEX (C:105, 0.15) are
 * file-static in SAGE26 and restated as literals; MSUN_TO_CODE_MASS (R/src/macros.h:47) is
 * expanded as ((x) * (h) / 1.0e10). The cooling function and the clustering mass are SAGE26's
 * own get_metaldependent_cooling_rate and interpolate_clustering_mass.
 */
static void tr_cooling_recipe_hot(const struct GALAXY *input, const double dt,
                                  const struct params *p, const struct tr_override *ov,
                                  struct tr_result *r) {
  tr_init(r, input);
  struct GALAXY *g = &r->galaxy;
  double coolingGas;

  g->tcool = 0.0f; /* C:139 */

  if (g->HotGas > 0.0 && g->Vvir > 0.0) {                             /* C:141 */
    const double tcool = take(r, ov, L_TCOOL_DYN, g->Rvir / g->Vvir); /* C:142 */
    const double tff = M_SQRT2 * tcool;                               /* C:143 */
    g->tcool = (float)((tcool * 1.0e10 / p->Hubble_h * SEC_PER_GIGAYEAR / p->UnitTime_in_s));
    g->tff = (float)((tff * 1.0e10 / p->Hubble_h * SEC_PER_GIGAYEAR / p->UnitTime_in_s));

    const double temp = 35.9 * g->Vvir * g->Vvir; /* C:150 */
    double logZ = -10.0;                          /* C:152-155 */
    if (g->MetalsHotGas > 0) {
      logZ = log10(g->MetalsHotGas / g->HotGas);
    }
    const double lambda = take(r, ov, L_LAMBDA, get_metaldependent_cooling_rate(log10(temp), logZ));

    if (lambda <= 0.0) { /* C:159-161 */
      r->flag[F_RETURN_PATH] = 2;
      r->ret = 0.0;
      return;
    }

    double x = PROTONMASS * BOLTZMANN * temp / lambda; /* C:163 */
    x /= (p->UnitDensity_in_cgs * p->UnitTime_in_s);   /* C:164 */
    x = take(r, ov, L_X, x);
    const double rho_rcool = x / tcool * (1.5 * 0.59); /* C:165 */

    if (rho_rcool <= 0.0) { /* C:167-169 */
      r->flag[F_RETURN_PATH] = 3;
      r->ret = 0.0;
      return;
    }

    const double rho0 = g->HotGas / (4 * M_PI * g->Rvir);              /* C:172 */
    const double rcool = take(r, ov, L_RCOOL, sqrt(rho0 / rho_rcool)); /* C:173 */
    g->RcoolToRvir = rcool / g->Rvir;                                  /* C:175 */

    coolingGas = 0.0; /* C:188 */

    /* C:202-286, CGMrecipeOn 1 */
    const double z = take(r, ov, L_Z, p->ZZ[g->SnapNum]); /* C:205 */
    const double M_shock = take(r, ov, L_MSHOCK, ((p->MShockMsun) * (p->Hubble_h) / 1.0e10));
    const double mass_ratio = take(r, ov, L_MASS_RATIO, g->Mvir / M_shock); /* C:211 */
    const double log_mstar = take(r, ov, L_LOG_MSTAR, interpolate_clustering_mass(z, p));
    const double Mstar =
        take(r, ov, L_MSTAR, ((pow(10.0, log_mstar)) * (p->Hubble_h) / 1.0e10)); /* C:222-223 */
    const double fMstar = take(r, ov, L_FMSTAR, 3.0 * Mstar);                    /* C:224 */
    const double ratio =
        take(r, ov, L_RATIO, pow(fMstar / g->Mvir, 2.0 / 3.0) * pow(mass_ratio, 4.0 / 3.0));

    double f_stream; /* C:228-243 */
    r->flag[F_EXPONENT_CAPPED_HIGH] = -1;
    r->flag[F_EXPONENT_CAPPED_LOW] = -1;
    if (ratio <= 0.0) {
      r->flag[F_RATIO_NONPOSITIVE] = 1;
      f_stream = 1.0;
    } else {
      r->flag[F_RATIO_NONPOSITIVE] = 0;
      double exponent = log10(ratio) / 0.15; /* C:239 */
      r->flag[F_EXPONENT_CAPPED_HIGH] = (exponent > 300.0) ? 1 : 0;
      if (exponent > 300.0) /* C:240 */
        exponent = 300.0;
      r->flag[F_EXPONENT_CAPPED_LOW] = (exponent < -300.0) ? 1 : 0;
      if (exponent < -300.0) /* C:241 */
        exponent = -300.0;
      exponent = take(r, ov, L_EXPONENT, exponent);
      f_stream = 1.0 / (1.0 + pow(10.0, exponent)); /* C:242 */
    }
    f_stream = take(r, ov, L_FSTREAM_CRIT, f_stream);

    r->flag[F_CUTOFF] = 0;
    if (fMstar >= M_shock && mass_ratio > 1.0) { /* C:263-265 */
      f_stream = 0.0;
      r->flag[F_CUTOFF] = 1;
    }
    r->flag[F_FSTREAM_CAPPED_HIGH] = (f_stream > 1.0) ? 1 : 0;
    if (f_stream > 1.0) /* C:270 */
      f_stream = 1.0;
    r->flag[F_FSTREAM_FLOORED] = (f_stream < 0.0) ? 1 : 0;
    if (f_stream < 0.0) /* C:271 */
      f_stream = 0.0;
    f_stream = take(r, ov, L_FSTREAM, f_stream);

    const double cold_stream_cooling =
        take(r, ov, L_COLD, f_stream * g->HotGas / tcool * dt); /* C:277 */
    const double hot_halo_cooling =
        take(r, ov, L_HOTHALO,
             (1.0 - f_stream) * (g->HotGas / g->Rvir) * (rcool / (2.0 * tcool)) * dt); /* C:279 */

    g->mdot_cool = hot_halo_cooling / dt;      /* C:282 */
    g->mdot_stream = cold_stream_cooling / dt; /* C:283 */

    coolingGas = take(r, ov, L_PRECAP, cold_stream_cooling + hot_halo_cooling); /* C:285 */

    if (coolingGas > g->HotGas) { /* C:288-292 */
      coolingGas = g->HotGas;
      r->flag[F_CAP] = 1;
    } else {
      r->flag[F_CAP] = 0;
      if (coolingGas < 0.0) {
        coolingGas = 0.0;
        r->flag[F_CAP] = 2;
      }
    }
    coolingGas = take(r, ov, L_POSTCAP, coolingGas);

    r->flag[F_AGN_ENTERED] = (p->AGNrecipeOn > 0 && coolingGas > 0.0) ? 1 : 0;
    if (p->AGNrecipeOn > 0 && coolingGas > 0.0) { /* C:297-299 */
      coolingGas = tr_do_agn_heating(g, coolingGas, dt, x, rcool, p, r, ov);
    }

    if (coolingGas > 0.0) { /* C:301-303 */
      g->Cooling += 0.5 * coolingGas * g->Vvir * g->Vvir;
    }
    r->flag[F_RETURN_PATH] = 0;
  } else {
    coolingGas = 0.0; /* C:305 */
    r->flag[F_RETURN_PATH] = 1;
  }

  g->CoolingRate =
      (dt > 0.0)
          ? (float)((coolingGas / dt) * 1.0e10 / p->Hubble_h * SEC_PER_GIGAYEAR / p->UnitTime_in_s)
          : 0.0f;      /* C:310-313 */
  r->ret = coolingGas; /* C:314 */
}

/**
 * @brief TRANSCRIPTION of cooling_recipe_cgm (C:317-391)
 *
 * The profile constants alpha 1.4, r0 = 0.1 Rvir and ratio 10 are SAGE26's locals (C:336-338),
 * kept as const locals so the compiler folds the integrals as it does in SAGE26's object.
 */
static void tr_cooling_recipe_cgm(const struct GALAXY *input, const double dt,
                                  const struct params *p, const struct tr_override *ov,
                                  struct tr_result *r) {
  tr_init(r, input);
  struct GALAXY *g = &r->galaxy;
  double coolingGas = 0.0; /* C:319 */

  r->flag[F_ENTERED] = (g->CGMgas > 0.0 && g->Vvir > 0.0) ? 1 : 0;
  if (g->CGMgas > 0.0 && g->Vvir > 0.0) {         /* C:321 */
    const double temp = 35.9 * g->Vvir * g->Vvir; /* C:322 */
    double logZ = -10.0;                          /* C:324-327 */
    if (g->MetalsCGMgas > 0) {
      logZ = log10(g->MetalsCGMgas / g->CGMgas);
    }
    const double lambda = take(r, ov, L_LAMBDA, get_metaldependent_cooling_rate(log10(temp), logZ));

    r->flag[F_LAMBDA_POSITIVE] = (lambda > 0.0) ? 1 : 0;
    if (lambda > 0.0) {                                  /* C:331 */
      double x = PROTONMASS * BOLTZMANN * temp / lambda; /* C:332 */
      x /= (p->UnitDensity_in_cgs * p->UnitTime_in_s);   /* C:333 */
      x = take(r, ov, L_X, x);

      const double alpha = 1.4;                                                          /* C:336 */
      const double r0 = 0.1 * g->Rvir;                                                   /* C:337 */
      const double ratio = 10.0;                                                         /* C:338 */
      const double I_M = (pow(ratio, 3.0 - alpha) - 1.0) / (3.0 - alpha);                /* C:341 */
      const double I_cool = (pow(ratio, 3.0 - 2.0 * alpha) - 1.0) / (3.0 - 2.0 * alpha); /* C:342 */

      const double rho0 =
          take(r, ov, L_RHO0, g->CGMgas / (4.0 * M_PI * r0 * r0 * r0 * I_M));      /* C:345 */
      const double rho_eff = take(r, ov, L_RHO_EFF, rho0 * (I_cool / I_M));        /* C:346 */
      const double tcool = take(r, ov, L_TCOOL_CGM, (x / rho_eff) * (1.5 * 0.59)); /* C:349 */

      const double g_accel =
          take(r, ov, L_G_ACCEL, p->G * g->Mvir / (g->Rvir * g->Rvir));         /* C:352 */
      const double tff = take(r, ov, L_TFF_CGM, sqrt(2.0 * g->Rvir / g_accel)); /* C:353 */

      g->tcool = (float)(tcool * p->UnitTime_in_s / SEC_PER_GIGAYEAR); /* C:356 */
      g->tff = (float)(tff * p->UnitTime_in_s / SEC_PER_GIGAYEAR);     /* C:357 */

      const double rcool = take(r, ov, L_RCOOL, g->Rvir); /* C:360 */

      coolingGas = take(r, ov, L_PRECAP, (g->CGMgas / (tcool + tff)) * dt); /* C:364 */

      if (coolingGas > g->CGMgas) { /* C:366-370 */
        coolingGas = g->CGMgas;
        r->flag[F_CAP] = 1;
      } else if (coolingGas < 0.0) {
        coolingGas = 0.0;
        r->flag[F_CAP] = 2;
      } else {
        r->flag[F_CAP] = 0;
      }
      coolingGas = take(r, ov, L_POSTCAP, coolingGas);

      r->flag[F_AGN_ENTERED] = (p->AGNrecipeOn > 0 && coolingGas > 0.0) ? 1 : 0;
      if (p->AGNrecipeOn > 0 && coolingGas > 0.0) { /* C:372-374 */
        coolingGas = tr_do_agn_heating_cgm(g, coolingGas, dt, x, rcool, p, r, ov);
      }

      if (coolingGas > 0.0) { /* C:376-378 */
        g->Cooling += 0.5 * coolingGas * g->Vvir * g->Vvir;
      }
    }
  }

  g->CoolingRate =
      (dt > 0.0)
          ? (float)((coolingGas / dt) * 1.0e10 / p->Hubble_h * SEC_PER_GIGAYEAR / p->UnitTime_in_s)
          : 0.0f;      /* C:382-385 */
  r->ret = coolingGas; /* C:390 */
}

/** @brief TRANSCRIPTION of do_AGN_heating called on its own (C:587-627) */
static void tr_do_agn_heating_alone(const struct GALAXY *input, const double coolingGas,
                                    const double dt, const double x, const double rcool,
                                    const struct params *p, const struct tr_override *ov,
                                    struct tr_result *r) {
  tr_init(r, input);
  r->ret = tr_do_agn_heating(&r->galaxy, coolingGas, dt, x, rcool, p, r, ov);
}

/* SAGE26's literal tables of interpolate_clustering_mass (H:101, H:104-108, H:111-115) */
static const double TR_MSTAR_Z[31] = {
    0.0, 0.5, 1.0, 1.5, 2.0,  2.5,  3.0,  3.5,  4.0,  4.5,  5.0,  5.5,  6.0,  6.5,  7.0, 7.5,
    8.0, 8.5, 9.0, 9.5, 10.0, 10.5, 11.0, 11.5, 12.0, 12.5, 13.0, 13.5, 14.0, 14.5, 15.0};
static const double TR_MSTAR_MILL[31] = {
    12.8964, 12.2501, 11.5685, 10.9046, 10.2717, 9.6699, 9.0969, 8.5486, 8.0214, 7.5126, 7.0206,
    6.5435,  6.0794,  5.6266,  5.1842,  4.7514,  4.3276, 3.9122, 3.5045, 3.1039, 2.7098, 2.3217,
    1.9393,  1.5626,  1.1912,  1.0000,  1.0000,  1.0000, 1.0000, 1.0000, 1.0000};
static const double TR_MSTAR_UCHUU[31] = {
    12.8028, 12.0672, 11.3101, 10.5795, 9.8831, 9.2194, 8.5834, 7.9701, 7.3764, 6.8002, 6.2389,
    5.6901,  5.1521,  4.6241,  4.1054,  3.5950, 3.0915, 2.5943, 2.1027, 1.6166, 1.1359, 1.0000,
    1.0000,  1.0000,  1.0000,  1.0000,  1.0000, 1.0000, 1.0000, 1.0000, 1.0000};

/** @brief TRANSCRIPTION of interpolate_clustering_mass (H:98-132) */
static double tr_interpolate_clustering_mass(const double z, const double omega,
                                             const struct tr_override *ov, struct tr_result *r) {
  const double *mstar_table_z = TR_MSTAR_Z;
  const double *mstar_mill = TR_MSTAR_MILL;
  const double *mstar_uchuu = TR_MSTAR_UCHUU;

  const double *table = mstar_mill; /* H:117-120 */
  r->flag[F_UCHUU] = 0;
  if (fabs(omega - 0.3089) < 0.01) {
    table = mstar_uchuu;
    r->flag[F_UCHUU] = 1;
  }

  double zz = z; /* H:122-124 */
  r->flag[F_Z_CLAMPED_LOW] = (zz < mstar_table_z[0]) ? 1 : 0;
  if (zz < mstar_table_z[0])
    zz = mstar_table_z[0];
  r->flag[F_Z_CLAMPED_HIGH] = (zz > mstar_table_z[31 - 1]) ? 1 : 0;
  if (zz > mstar_table_z[31 - 1])
    zz = mstar_table_z[31 - 1];
  zz = take(r, ov, L_ZZ, zz);

  int iz = (int)(zz / 0.5); /* H:126-128 */
  r->flag[F_IZ_CLAMPED] = 0;
  if (iz < 0) {
    iz = 0;
    r->flag[F_IZ_CLAMPED] = 1;
  }
  if (iz >= 31 - 1) {
    iz = 31 - 2;
    r->flag[F_IZ_CLAMPED] = 1;
  }
  r->flag[F_IZ] = iz;

  const double fz =
      take(r, ov, L_FZ, (zz - mstar_table_z[iz]) / (mstar_table_z[iz + 1] - mstar_table_z[iz]));
  return table[iz] * (1.0 - fz) + table[iz + 1] * fz; /* H:131 */
}

// ============================================================================
// CHECKS AND OUTPUT
// ============================================================================

/** @brief Whether a transcription result's recorded fields and return equal another's */
static int same_result(const struct tr_result *a, const struct tr_result *b) {
  return same_f64(a->ret, b->ret) && same_recorded_state(&a->galaxy, &b->galaxy);
}

typedef void (*tr_runner)(const struct tr_override *ov, struct tr_result *r);

/** @brief Write every present local with its check and two-sided validation markers */
static void write_locals(const struct tr_result *base, tr_runner run, const int *kind) {
  for (int id = 0; id < L_COUNT; id++) {
    if (!base->present[id]) {
      continue;
    }
    const double v = base->local[id];
    struct tr_override up = {(enum local_id)id, nextafter(v, INFINITY)};
    struct tr_override down = {(enum local_id)id, nextafter(v, -INFINITY)};
    struct tr_result ru, rd;
    run(&up, &ru);
    run(&down, &rd);
    const int vu = !same_result(&ru, base);
    const int vd = !same_result(&rd, base);
    const char *name = LOCAL_NAME[id];
    out_i32(field_name("intermediate.%s_absent", name), 0);
    out_f64(field_name("intermediate.%s", name), v);
    out_i32(field_name("intermediate.%s_checked", name), 1);
    out_i32(field_name("intermediate.%s_check_kind", name), kind[id]);
    out_i32(field_name("intermediate.%s_validated_up", name), vu);
    out_i32(field_name("intermediate.%s_validated_down", name), vd);
    out_i32(field_name("intermediate.%s_validated", name), vu && vd);
  }
}

/** @brief Write every listed flag (-1 where the path does not evaluate it) */
static void write_flags(const struct tr_result *r, const enum flag_id *flags, const int n) {
  for (int k = 0; k < n; k++) {
    out_i32(field_name("intermediate.%s", FLAG_NAME[flags[k]]), r->flag[flags[k]]);
  }
}

// ============================================================================
// ROUTINES
// ============================================================================

static struct GALAXY routine_input;
static struct call_time routine_time;
static double routine_cooling, routine_x, routine_rcool;
static double routine_z, routine_omega;

static void run_hot(const struct tr_override *ov, struct tr_result *r) {
  tr_cooling_recipe_hot(&routine_input, routine_time.dt, &run_params, ov, r);
}
static void run_cgm(const struct tr_override *ov, struct tr_result *r) {
  tr_cooling_recipe_cgm(&routine_input, routine_time.dt, &run_params, ov, r);
}
static void run_agn(const struct tr_override *ov, struct tr_result *r) {
  tr_do_agn_heating_alone(&routine_input, routine_cooling, routine_time.dt, routine_x,
                          routine_rcool, &run_params, ov, r);
}
static void run_interp(const struct tr_override *ov, struct tr_result *r) {
  tr_init(r, &galaxies[0]);
  r->ret = tr_interpolate_clustering_mass(routine_z, routine_omega, ov, r);
}

static void run_interpolate_clustering_mass(void) {
  routine_omega = in_f64("params.Omega");
  routine_z = in_f64("call.z");
  check_all_fields_used();
  run_params.Omega = routine_omega;

  const double log_mstar = interpolate_clustering_mass(routine_z, &run_params);

  struct tr_result base;
  run_interp(&NO_OVERRIDE, &base);
  if (!same_f64(base.ret, log_mstar)) {
    fatal("transcription check failed: interpolate_clustering_mass transcribed %.17g, SAGE26 %.17g",
          base.ret, log_mstar);
  }
  /* The unfused form, for the fused-instruction table: the two endpoint products rounded and
   * stored separately through volatiles, then added (H:131 without contraction). */
  const double fz = base.local[L_FZ];
  const int iz = base.flag[F_IZ];
  const double *t = base.flag[F_UCHUU] ? TR_MSTAR_UCHUU : TR_MSTAR_MILL;
  volatile double first = t[iz] * (1.0 - fz);
  volatile double second = t[iz + 1] * fz;
  const double unfused = first + second;

  static const int kind[L_COUNT] = {[L_ZZ] = 2, [L_FZ] = 2};
  out_f64("return.log_mstar", log_mstar);
  static const enum flag_id flags[] = {F_UCHUU, F_Z_CLAMPED_LOW, F_Z_CLAMPED_HIGH, F_IZ,
                                       F_IZ_CLAMPED};
  write_flags(&base, flags, (int)(sizeof(flags) / sizeof(flags[0])));
  write_locals(&base, run_interp, kind);
  out_f64("intermediate.unfused_log_mstar", unfused);
  out_i32("intermediate.fused_differs_from_unfused", !same_f64(unfused, log_mstar));
}

/** @brief The check kinds of cooling_recipe_hot_streams: continuation where SAGE26 passes it on */
static const int HOT_KIND[L_COUNT] = {[L_TCOOL_DYN] = 2,    [L_LAMBDA] = 2,    [L_X] = 1,
                                      [L_RCOOL] = 1,        [L_Z] = 1,         [L_MSHOCK] = 2,
                                      [L_MASS_RATIO] = 2,   [L_LOG_MSTAR] = 2, [L_MSTAR] = 2,
                                      [L_FMSTAR] = 2,       [L_RATIO] = 2,     [L_EXPONENT] = 2,
                                      [L_FSTREAM_CRIT] = 2, [L_FSTREAM] = 2,   [L_COLD] = 2,
                                      [L_HOTHALO] = 2,      [L_PRECAP] = 2,    [L_POSTCAP] = 1,
                                      [L_SUPPRESSED] = 2,   [L_ACCRETED] = 2,  [L_HEATING] = 2};

/** @brief Every other routine's locals are tail-checked: SAGE26 passes them to no public routine */
static const int TAIL_KIND[L_COUNT] = {
    [L_TCOOL_DYN] = 2, [L_LAMBDA] = 2,     [L_X] = 2,        [L_RCOOL] = 2,   [L_RHO0] = 2,
    [L_RHO_EFF] = 2,   [L_TCOOL_CGM] = 2,  [L_G_ACCEL] = 2,  [L_TFF_CGM] = 2, [L_PRECAP] = 2,
    [L_POSTCAP] = 2,   [L_SUPPRESSED] = 2, [L_ACCRETED] = 2, [L_HEATING] = 2};

static const enum flag_id AGN_FLAGS[] = {
    F_SUPPRESSED,       F_SATURATED,      F_RESERVOIR_POSITIVE, F_EDDINGTON_CAPPED,
    F_ACCRETION_CAPPED, F_HEATING_CAPPED, F_RATCHET_RAISED,     F_RHEAT_CAPPED};
#define NUM_AGN_FLAGS ((int)(sizeof(AGN_FLAGS) / sizeof(AGN_FLAGS[0])))

static void run_cooling_recipe_hot_streams(void) {
  read_flags(1);
  read_units();
  routine_time = read_call_time();
  read_galaxy_block();
  read_stream_cosmology();
  check_all_fields_used();

  routine_input = galaxies[0];

  /* B:508: the call itself, on the fixture input */
  const double coolingGas = cooling_recipe_hot(0, routine_time.dt, galaxies, &run_params);
  const struct GALAXY after = galaxies[0];
  check_only_recorded_changed(&routine_input, &after);

  struct tr_result base;
  run_hot(&NO_OVERRIDE, &base);
  if (!same_f64(base.ret, coolingGas) || !same_recorded_state(&base.galaxy, &after)) {
    fatal("transcription check failed: the cooling_recipe_hot transcription does not reproduce "
          "SAGE26's call bit for bit");
  }

  /* The post-cap mass is SAGE26's return with AGNrecipeOn 0 (C:297, C:314) */
  if (base.flag[F_RETURN_PATH] == 0) {
    struct params agn_off = run_params;
    agn_off.AGNrecipeOn = 0;
    galaxies[0] = routine_input;
    const double postcap = cooling_recipe_hot(0, routine_time.dt, galaxies, &agn_off);
    if (!same_f64(postcap, base.local[L_POSTCAP]) ||
        !same_f32(galaxies[0].mdot_cool, after.mdot_cool) ||
        !same_f32(galaxies[0].mdot_stream, after.mdot_stream)) {
      fatal("the AGNrecipeOn 0 call's return or mdot fields differ from the transcribed "
            "post-cap mass and mdot stores");
    }
    /* Continuation: SAGE26's own do_AGN_heating fed the transcribed x, rcool and post-cap mass */
    if (base.flag[F_AGN_ENTERED] == 1) {
      galaxies[0] = routine_input;
      const double ret = do_AGN_heating(base.local[L_POSTCAP], 0, routine_time.dt, base.local[L_X],
                                        base.local[L_RCOOL], galaxies, &run_params);
      const struct GALAXY *c = &galaxies[0];
      if (!same_f64(ret, coolingGas) || !same_f32(c->BlackHoleMass, after.BlackHoleMass) ||
          !same_f32(c->HotGas, after.HotGas) || !same_f32(c->MetalsHotGas, after.MetalsHotGas) ||
          !same_f32(c->r_heat, after.r_heat) || !same_f64(c->Heating, after.Heating)) {
        fatal("continuation check failed: SAGE26's do_AGN_heating with the transcribed x, rcool "
              "and post-cap mass does not reproduce the call");
      }
    }
  }

  write_call_time(&routine_time);
  out_f64("return.coolingGas", coolingGas);
  write_galaxy_state(&after);
  static const enum flag_id flags[] = {
      F_RETURN_PATH, F_RATIO_NONPOSITIVE,   F_EXPONENT_CAPPED_HIGH, F_EXPONENT_CAPPED_LOW,
      F_CUTOFF,      F_FSTREAM_CAPPED_HIGH, F_FSTREAM_FLOORED,      F_CAP,
      F_AGN_ENTERED};
  write_flags(&base, flags, (int)(sizeof(flags) / sizeof(flags[0])));
  write_flags(&base, AGN_FLAGS, NUM_AGN_FLAGS);
  write_locals(&base, run_hot, HOT_KIND);
}

static void run_cooling_recipe_cgm(void) {
  read_flags(1);
  read_units();
  routine_time = read_call_time();
  read_galaxy_block();
  check_all_fields_used();

  routine_input = galaxies[0];

  const double coolingGas = cooling_recipe_cgm(0, routine_time.dt, galaxies, &run_params);
  const struct GALAXY after = galaxies[0];
  check_only_recorded_changed(&routine_input, &after);

  struct tr_result base;
  run_cgm(&NO_OVERRIDE, &base);
  if (!same_f64(base.ret, coolingGas) || !same_recorded_state(&base.galaxy, &after)) {
    fatal("transcription check failed: the cooling_recipe_cgm transcription does not reproduce "
          "SAGE26's call bit for bit");
  }
  if (base.present[L_POSTCAP]) {
    struct params agn_off = run_params;
    agn_off.AGNrecipeOn = 0;
    galaxies[0] = routine_input;
    const double postcap = cooling_recipe_cgm(0, routine_time.dt, galaxies, &agn_off);
    if (!same_f64(postcap, base.local[L_POSTCAP])) {
      fatal("the AGNrecipeOn 0 call's return differs from the transcribed post-cap mass");
    }
  }

  write_call_time(&routine_time);
  out_f64("return.coolingGas", coolingGas);
  write_galaxy_state(&after);
  static const enum flag_id flags[] = {F_ENTERED, F_LAMBDA_POSITIVE, F_CAP, F_AGN_ENTERED,
                                       F_RECAPPED};
  write_flags(&base, flags, (int)(sizeof(flags) / sizeof(flags[0])));
  write_flags(&base, AGN_FLAGS, NUM_AGN_FLAGS);
  write_locals(&base, run_cgm, TAIL_KIND);
}

static void run_do_agn_heating_cgm_on(void) {
  read_flags(0);
  read_units();
  routine_time = read_call_time();
  routine_cooling = in_f64("call.coolingGas");
  routine_x = in_f64("call.x");
  routine_rcool = in_f64("call.rcool");
  read_galaxy_block();
  check_all_fields_used();
  if (!(routine_cooling > 0.0)) {
    fatal("call.coolingGas must be positive (do_AGN_heating is only entered then, C:297)");
  }

  routine_input = galaxies[0];
  const double suppressed = do_AGN_heating(routine_cooling, 0, routine_time.dt, routine_x,
                                           routine_rcool, galaxies, &run_params);
  const struct GALAXY after = galaxies[0];
  check_only_recorded_changed(&routine_input, &after);

  struct tr_result base;
  run_agn(&NO_OVERRIDE, &base);
  if (!same_f64(base.ret, suppressed) || !same_recorded_state(&base.galaxy, &after)) {
    fatal("transcription check failed: the do_AGN_heating transcription does not reproduce "
          "SAGE26's call bit for bit");
  }

  write_call_time(&routine_time);
  out_f64("return.coolingGas", suppressed);
  write_galaxy_state(&after);
  write_flags(&base, AGN_FLAGS, NUM_AGN_FLAGS);
  write_locals(&base, run_agn, TAIL_KIND);
}

// ============================================================================
// MAIN
// ============================================================================

struct routine_entry {
  const char *name;
  void (*run)(void);
};

static const struct routine_entry routines[] = {
    {"interpolate_clustering_mass", run_interpolate_clustering_mass},
    {"cooling_recipe_hot_streams", run_cooling_recipe_hot_streams},
    {"cooling_recipe_cgm", run_cooling_recipe_cgm},
    {"do_AGN_heating_cgm_on", run_do_agn_heating_cgm_on},
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
          "# SAGE26 oracle output for %s, written by oracle_c11 (regenerate.sh); do not edit.\n",
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
