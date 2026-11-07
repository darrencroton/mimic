/**
 * @file    oracle_c08.c
 * @brief   SAGE26 function-level oracle for the molecular-gas routines (BR06 with radial
 *          integration, SFprescription 1)
 *
 * Reads one fixture input file, initialises the SAGE26 struct GALAXY and struct params
 * fields its reader for the selected routine consumes (a superset of the fields the routine
 * reads), calls the routine from the SAGE26 objects this executable is linked against, and
 * writes a fixture output file holding the routine's return value, every field it writes and
 * the routine's locals.
 *
 * Usage: oracle_c08 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one; R/ is the staged SAGE26 source root,
 * H = R/src/model_h2_chemistry.c):
 *   calculate_molecular_fraction_BR06                H:117-125, called directly
 *   calculate_molecular_fraction_radial_integration  H:134-290, called directly as sfr_br06
 *                                                    calls it (prescription 1, a NULL SFR-output
 *                                                    pointer, R/src/model_starformation_and_
 *                                                    feedback.c:222)
 *
 * Both are public in SAGE26; their file-static helpers, calculate_stellar_scale_height_BR06
 * (H:48-60) and calculate_molecular_fraction_BR06_from_hstar (H:68-101), cannot be linked, so
 * the routines' locals come from SOURCE-REVIEWED TRANSCRIPTIONS below (transcribe_*), each
 * statement cited to its line, composed into a transcription of the public routine. The
 * oracle refuses a fixture unless the transcribed public routine reproduces SAGE26's own
 * return value and every member of the galaxy bit for bit. SAGE26's own call gives the
 * recorded return and galaxy; the recorded locals come from the transcription, and the two
 * are kept apart. No routine passes a local to a SAGE26 function (the helpers are static and
 * transcribed; log10, pow, expf and sqrt are libm), so every local is TAIL-checked: the
 * transcription runs from the local to the routine's end. Each local is then replaced by its
 * upward and its downward neighbour of its own type (nextafterf for a float, nextafter for a
 * double) and the transcription rerun (two-sided validation).
 *
 * Every transcription restates SAGE26's statements with the same expression shapes, operand
 * order and types and is compiled with the same flags, so floating-point contraction (clang's
 * default -ffp-contract=on) and clang's library-call simplifications treat them as they treat
 * SAGE26's: the height's -0.23 + 0.8 * log10(R*) is fused in both (H:56), and the height's
 * pow(10.0, x) is a call to __exp10 in both (H:57). README.md ("The c08 Group") lists the
 * fused instructions, checked in the disassembly.
 *
 * The file format is defined in README.md ("Fixture File Format"). The parsing and output
 * helpers are copies of oracle_c05.c's, which stays unchanged.
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
#include "model_h2_chemistry.h"
#include "model_misc.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 1024
#define MAX_GALAXIES 1
#define NAME_LEN 96
#define LINE_LEN 512

/* The bin count the oracle supports, SAGE26's default H2RadialNBins (R/src/
 * core_read_parameter_file.c:100), and the factor, its default H2RadialRMaxFactor (:101) */
#define SUPPORTED_NBINS 25
#define SUPPORTED_RMAX_FACTOR 5.0

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
  fprintf(stderr, "oracle_c08: %s: ", input_path != NULL ? input_path : "(no input)");
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
// STATE INITIALISATION AND THE CONFIGURATION READER
// ============================================================================

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(&run_params, 0xff, sizeof(run_params));
}

/**
 * @brief SFprescription 1 and the H2 configuration the radial integration is supported with
 *
 * This oracle supports the radial integration only as sfr_br06 calls it: SFprescription 1 (BR06),
 * with H2RadialIntegrationOn 1, H2RadialNBins 25 and H2RadialRMaxFactor 5.0 (SAGE26's defaults,
 * R/src/core_read_parameter_file.c:99-101) and StarburstColdGasOn 1 (:109). Each is required
 * explicitly, and any other value is refused, naming the parameter. H2DiskAreaOption, read only by
 * the single-slab branch and the StarburstColdGasOn 0 burst fuel, stays poisoned: no supported path
 * reads it, and the poisoned state never supplies an H2 parameter.
 */
static void read_h2_config(void) {
  run_params.SFprescription = in_i32("params.SFprescription");
  if (run_params.SFprescription != 1) {
    fatal("params.SFprescription = %d, but the radial integration is supported only as "
          "sfr_br06 calls it (SFprescription 1)",
          run_params.SFprescription);
  }
  run_params.H2RadialIntegrationOn = in_i32("params.H2RadialIntegrationOn");
  if (run_params.H2RadialIntegrationOn != 1) {
    fatal("params.H2RadialIntegrationOn = %d, but the oracle supports only 1 (radial "
          "integration; the single-slab branch is not ported)",
          run_params.H2RadialIntegrationOn);
  }
  run_params.H2RadialNBins = in_i32("params.H2RadialNBins");
  if (run_params.H2RadialNBins != SUPPORTED_NBINS) {
    fatal("params.H2RadialNBins = %d, but the oracle supports only %d", run_params.H2RadialNBins,
          SUPPORTED_NBINS);
  }
  run_params.H2RadialRMaxFactor = in_f64("params.H2RadialRMaxFactor");
  if (!same_f64(run_params.H2RadialRMaxFactor, SUPPORTED_RMAX_FACTOR)) {
    fatal("params.H2RadialRMaxFactor = %.17g, but the oracle supports only %.1f",
          run_params.H2RadialRMaxFactor, SUPPORTED_RMAX_FACTOR);
  }
  run_params.StarburstColdGasOn = in_i32("params.StarburstColdGasOn");
  if (run_params.StarburstColdGasOn != 1) {
    fatal("params.StarburstColdGasOn = %d, but the oracle supports only 1 (cold-gas bursts)",
          run_params.StarburstColdGasOn);
  }
}

// ============================================================================
// LOCALS: TAPS, PERTURBATIONS AND THEIR RECORD
// ============================================================================

enum value_type { VALUE_F32, VALUE_F64 };

/** A local of a transcription: its name, type and whether one narrowing limits its validation */
struct local_info {
  const char *name;
  enum value_type type;
  int narrowing_limited; /* 1: its only path to the output is one double-to-float narrowing */
};

/** The locals of calculate_molecular_fraction_BR06 and its two static helpers (H:52-100) */
enum br06_local {
  B_LOG_H_STAR,
  B_H_STAR_PC,
  B_EFFECTIVE_SIGMA_STARS,
  B_PRESSURE,
  B_PRESSURE_RATIO,
  B_R_MOL,
  B_F_MOL,
  B_COUNT
};

static const struct local_info BR06_LOCALS[B_COUNT] = {
    [B_LOG_H_STAR] = {"log_h_star", VALUE_F32, 0},
    [B_H_STAR_PC] = {"h_star_pc", VALUE_F32, 0},
    [B_EFFECTIVE_SIGMA_STARS] = {"effective_sigma_stars", VALUE_F32, 0},
    [B_PRESSURE] = {"pressure", VALUE_F32, 0},
    [B_PRESSURE_RATIO] = {"pressure_ratio", VALUE_F32, 0},
    [B_R_MOL] = {"R_mol", VALUE_F32, 0},
    /* The double f_mol reaches the float return only through its narrowing (H:100) */
    [B_F_MOL] = {"f_mol", VALUE_F64, 1},
};

/** The scalar locals of the radial integration (H:138-178) */
enum radial_local {
  R_H,
  R_RS_PC,
  R_M_GAS_TOTAL,
  R_M_DISK_STAR,
  R_SIGMA_GAS_0,
  R_SIGMA_STAR_0,
  R_R_MAX,
  R_DR,
  R_LOG_H_STAR,
  R_H_STAR_BR06,
  R_COUNT
};

static const struct local_info RADIAL_LOCALS[R_COUNT] = {
    [R_H] = {"h", VALUE_F32, 0},
    [R_RS_PC] = {"rs_pc", VALUE_F32, 0},
    [R_M_GAS_TOTAL] = {"M_gas_total", VALUE_F32, 0},
    [R_M_DISK_STAR] = {"M_disk_star", VALUE_F32, 0},
    [R_SIGMA_GAS_0] = {"sigma_gas_0", VALUE_F32, 0},
    [R_SIGMA_STAR_0] = {"sigma_star_0", VALUE_F32, 0},
    [R_R_MAX] = {"R_MAX", VALUE_F32, 0},
    [R_DR] = {"dr", VALUE_F32, 0},
    [R_LOG_H_STAR] = {"log_h_star", VALUE_F32, 0},
    [R_H_STAR_BR06] = {"h_star_br06", VALUE_F32, 0},
};

/** The per-annulus locals of the radial integration (H:212-265) */
enum bin_local {
  N_R,
  N_EXP_FACTOR,
  N_SIGMA_GAS_R,
  N_SIGMA_STAR_R,
  N_F_MOL_R,
  N_DM_H2,
  N_M_H2_TOTAL,
  N_COUNT
};

static const struct local_info BIN_LOCALS[N_COUNT] = {
    [N_R] = {"r", VALUE_F32, 0},
    [N_EXP_FACTOR] = {"exp_factor", VALUE_F32, 0},
    [N_SIGMA_GAS_R] = {"sigma_gas_r", VALUE_F32, 0},
    [N_SIGMA_STAR_R] = {"sigma_star_r", VALUE_F32, 0},
    [N_F_MOL_R] = {"f_mol_r", VALUE_F32, 0},
    [N_DM_H2] = {"dM_H2", VALUE_F32, 0},
    [N_M_H2_TOTAL] = {"M_H2_total", VALUE_F32, 0},
};

/* One slot per local: the BR06 locals, the radial scalars, then N_COUNT per annulus */
#define SLOT_BR06(k) (k)
#define SLOT_RADIAL(k) (B_COUNT + (k))
#define SLOT_BIN(bin, k) (B_COUNT + R_COUNT + (bin) * N_COUNT + (k))
#define SLOT_COUNT (B_COUNT + R_COUNT + SUPPORTED_NBINS * N_COUNT)

/** The record of one transcribed call: its locals, branch flags and one optional perturbation */
struct trace {
  double value[SLOT_COUNT]; /* the local, a float widened exactly */
  int present[SLOT_COUNT];
  int perturb_slot; /* the slot to replace by its neighbour, or -1 */
  int perturb_up;   /* 1: toward +inf; 0: toward -inf */
  /* Branch flags, -1 where the path does not evaluate them */
  int scale_length_nonpositive;           /* the public BR06 routine's guard (H:120) */
  int pressure_branch;                    /* gas > 0.0 && h_star > 0.0 (H:72), public routine */
  int stellar_floor_binds;                /* stellar density < 0.1 (H:74), public routine */
  int pressure_nonpositive;               /* pressure <= 0.0 (H:83), public routine */
  int returned_early;                     /* rs_pc <= 0.0 || ColdGas <= 0.0 (H:141) */
  int disk_stars_positive;                /* M_disk_star > 0.0 (H:151) */
  int bin_skipped[SUPPORTED_NBINS];       /* sigma_gas_r < 1e-3f (H:220) */
  int bin_stellar_floor[SUPPORTED_NBINS]; /* the annulus's stellar floor (H:74) */
};

static void init_trace(struct trace *t, const int perturb_slot, const int perturb_up) {
  memset(t, 0, sizeof(*t));
  t->perturb_slot = perturb_slot;
  t->perturb_up = perturb_up;
  t->scale_length_nonpositive = -1;
  t->pressure_branch = -1;
  t->stellar_floor_binds = -1;
  t->pressure_nonpositive = -1;
  t->returned_early = -1;
  t->disk_stars_positive = -1;
  for (int i = 0; i < SUPPORTED_NBINS; i++) {
    t->bin_skipped[i] = -1;
    t->bin_stellar_floor[i] = -1;
  }
}

/** @brief Record a float local, then return it, or its neighbour when it is perturbed */
static float tap_f32(struct trace *t, const int slot, const float v) {
  if (slot < 0) {
    return v;
  }
  t->value[slot] = v;
  t->present[slot] = 1;
  if (t->perturb_slot == slot) {
    return nextafterf(v, t->perturb_up ? INFINITY : -INFINITY);
  }
  return v;
}

/** @brief The double counterpart of tap_f32() */
static double tap_f64(struct trace *t, const int slot, const double v) {
  if (slot < 0) {
    return v;
  }
  t->value[slot] = v;
  t->present[slot] = 1;
  if (t->perturb_slot == slot) {
    return nextafter(v, t->perturb_up ? INFINITY : -INFINITY);
  }
  return v;
}

// ============================================================================
// SOURCE-REVIEWED TRANSCRIPTIONS OF SAGE26 SOURCE (model_h2_chemistry.c)
// ============================================================================

/** The slots the stellar scale height's two locals are recorded in (-1: not recorded) */
struct height_slots {
  int log_h_star;
  int h_star_pc;
};

/**
 * @brief TRANSCRIPTION of calculate_stellar_scale_height_BR06, H:48-60 (static in SAGE26)
 *
 * The comment at H:51 states -0.8 log R*; the code, transcribed here, has +0.8 (H:56).
 */
static float transcribe_stellar_scale_height_BR06(const float disk_scale_length_pc, struct trace *t,
                                                  const struct height_slots *s) {
  /* H:52-54 */
  if (disk_scale_length_pc <= 0.0) {
    return 0.0;
  }

  /* H:56 */
  float log_h_star = -0.23 + 0.8 * log10(disk_scale_length_pc);
  log_h_star = tap_f32(t, s->log_h_star, log_h_star);
  /* H:57 */
  float h_star_pc = pow(10.0, log_h_star);
  h_star_pc = tap_f32(t, s->h_star_pc, h_star_pc);

  /* H:59 */
  return h_star_pc;
}

/**
 * @brief TRANSCRIPTION of calculate_molecular_fraction_BR06_from_hstar, H:68-101 (static)
 *
 * @param bin  The annulus of the radial integration this call serves, or -1 for the public
 *             routine, whose locals are recorded (an annulus records only its floor flag)
 */
static float transcribe_molecular_fraction_BR06_from_hstar(
    const float gas_surface_density_msun_pc2, const float stellar_surface_density_msun_pc2,
    const float h_star_pc, struct trace *t, const int bin) {
  const int record = bin < 0;

  /* H:71 */
  float pressure = 0.0;
  /* H:72 */
  if (record) {
    t->pressure_branch = gas_surface_density_msun_pc2 > 0.0 && h_star_pc > 0.0;
  }
  if (gas_surface_density_msun_pc2 > 0.0 && h_star_pc > 0.0) {
    /* H:73 */
    float effective_sigma_stars = stellar_surface_density_msun_pc2;
    /* H:74-76 */
    const int floor_binds = stellar_surface_density_msun_pc2 < 0.1;
    if (stellar_surface_density_msun_pc2 < 0.1) {
      effective_sigma_stars = 0.1;
    }
    if (record) {
      t->stellar_floor_binds = floor_binds;
    } else {
      t->bin_stellar_floor[bin] = floor_binds;
    }
    effective_sigma_stars =
        tap_f32(t, record ? SLOT_BR06(B_EFFECTIVE_SIGMA_STARS) : -1, effective_sigma_stars);
    /* H:77 */
    const float v_g = 8.0;
    /* H:80 */
    pressure =
        272.0 * gas_surface_density_msun_pc2 * sqrt(effective_sigma_stars) * v_g / sqrt(h_star_pc);
  }
  pressure = tap_f32(t, record ? SLOT_BR06(B_PRESSURE) : -1, pressure);

  /* H:83-85 */
  if (record) {
    t->pressure_nonpositive = pressure <= 0.0;
  }
  if (pressure <= 0.0) {
    return 0.0;
  }

  /* H:89-90 */
  const float P0 = 4.3e4;
  const float alpha = 0.92;

  /* H:93 */
  float pressure_ratio = pressure / P0;
  pressure_ratio = tap_f32(t, record ? SLOT_BR06(B_PRESSURE_RATIO) : -1, pressure_ratio);
  /* H:94 */
  float R_mol = pow(pressure_ratio, alpha);
  R_mol = tap_f32(t, record ? SLOT_BR06(B_R_MOL) : -1, R_mol);

  /* H:98 */
  double f_mol = R_mol / (1.0 + R_mol);
  f_mol = tap_f64(t, record ? SLOT_BR06(B_F_MOL) : -1, f_mol);

  /* H:100 */
  return f_mol;
}

/** @brief TRANSCRIPTION of calculate_molecular_fraction_BR06, H:117-125 */
static float transcribe_molecular_fraction_BR06(const float gas_surface_density_msun_pc2,
                                                const float stellar_surface_density_msun_pc2,
                                                const float disk_scale_length_pc, struct trace *t) {
  /* H:120-122 */
  t->scale_length_nonpositive = disk_scale_length_pc <= 0.0;
  if (disk_scale_length_pc <= 0.0) {
    return 0.0;
  }
  /* H:123 */
  const struct height_slots slots = {SLOT_BR06(B_LOG_H_STAR), SLOT_BR06(B_H_STAR_PC)};
  const float h_star_pc = transcribe_stellar_scale_height_BR06(disk_scale_length_pc, t, &slots);
  /* H:124 */
  return transcribe_molecular_fraction_BR06_from_hstar(
      gas_surface_density_msun_pc2, stellar_surface_density_msun_pc2, h_star_pc, t, -1);
}

/**
 * @brief TRANSCRIPTION of calculate_molecular_fraction_radial_integration, H:134-290, on the
 *        path sfr_br06 calls: SFprescription 1 (BR06) and strdot_code_out NULL
 *
 * Not transcribed, as they do not run or change nothing on this path: the GD14 constants
 * (H:161-173, prescription 7), the disk metallicity and the KMT09 and K13 constants (H:183-196,
 * which read MetalsColdGas and form locals the BR06 branch never uses), the K13 metallicity
 * (H:199-205), the other prescriptions' molecular fractions (H:229-259) and the K13 SFR and
 * depletion time (H:268-272, H:279-285), all behind prescription tests or the NULL pointer.
 */
static float transcribe_radial_integration(const int gal, struct trace *t) {
  /* H:138 */
  float h = run_params.Hubble_h;
  h = tap_f32(t, SLOT_RADIAL(R_H), h);
  /* H:139, CODE_LENGTH_TO_PC (R/src/macros.h:46) */
  float rs_pc = CODE_LENGTH_TO_PC(galaxies[gal].DiskScaleRadius, h);
  rs_pc = tap_f32(t, SLOT_RADIAL(R_RS_PC), rs_pc);

  /* H:141-143 */
  t->returned_early = rs_pc <= 0.0 || galaxies[gal].ColdGas <= 0.0;
  if (rs_pc <= 0.0 || galaxies[gal].ColdGas <= 0.0) {
    return 0.0;
  }

  /* H:146-147, CODE_MASS_TO_MSUN (R/src/macros.h:45) */
  float M_gas_total = CODE_MASS_TO_MSUN(galaxies[gal].ColdGas, h);
  M_gas_total = tap_f32(t, SLOT_RADIAL(R_M_GAS_TOTAL), M_gas_total);
  float M_disk_star = CODE_MASS_TO_MSUN(galaxies[gal].StellarMass - galaxies[gal].BulgeMass, h);
  M_disk_star = tap_f32(t, SLOT_RADIAL(R_M_DISK_STAR), M_disk_star);

  /* H:150-151 */
  float sigma_gas_0 = M_gas_total / (2.0 * M_PI * rs_pc * rs_pc);
  sigma_gas_0 = tap_f32(t, SLOT_RADIAL(R_SIGMA_GAS_0), sigma_gas_0);
  t->disk_stars_positive = M_disk_star > 0.0;
  float sigma_star_0 = (M_disk_star > 0.0) ? M_disk_star / (2.0 * M_PI * rs_pc * rs_pc) : 0.0;
  sigma_star_0 = tap_f32(t, SLOT_RADIAL(R_SIGMA_STAR_0), sigma_star_0);

  /* H:154-156 */
  const int N_BINS = run_params.H2RadialNBins;
  float R_MAX = (float)(run_params.H2RadialRMaxFactor) * rs_pc;
  R_MAX = tap_f32(t, SLOT_RADIAL(R_R_MAX), R_MAX);
  float dr = R_MAX / N_BINS;
  dr = tap_f32(t, SLOT_RADIAL(R_DR), dr);

  /* H:159 */
  const int sfpres = run_params.SFprescription;

  /* H:177-178 */
  const struct height_slots slots = {SLOT_RADIAL(R_LOG_H_STAR), -1};
  float h_star_br06 = sf_prescription_is_br06(sfpres)
                          ? transcribe_stellar_scale_height_BR06(rs_pc, t, &slots)
                          : 0.0f;
  h_star_br06 = tap_f32(t, SLOT_RADIAL(R_H_STAR_BR06), h_star_br06);

  /* H:208 */
  float M_H2_total = 0.0;

  /* H:210 */
  for (int i = 0; i < N_BINS; i++) {
    /* H:212 */
    float r = (i + 0.5f) * dr;
    r = tap_f32(t, SLOT_BIN(i, N_R), r);

    /* H:215-217 */
    float exp_factor = expf(-r / rs_pc);
    exp_factor = tap_f32(t, SLOT_BIN(i, N_EXP_FACTOR), exp_factor);
    float sigma_gas_r = sigma_gas_0 * exp_factor;
    sigma_gas_r = tap_f32(t, SLOT_BIN(i, N_SIGMA_GAS_R), sigma_gas_r);
    float sigma_star_r = sigma_star_0 * exp_factor;
    sigma_star_r = tap_f32(t, SLOT_BIN(i, N_SIGMA_STAR_R), sigma_star_r);

    /* H:220 */
    t->bin_skipped[i] = sigma_gas_r < 1e-3f;
    if (sigma_gas_r < 1e-3f)
      continue;

    /* H:223-227 */
    float f_mol_r = 0.0f;
    if (sf_prescription_is_br06(sfpres)) {
      f_mol_r = transcribe_molecular_fraction_BR06_from_hstar(sigma_gas_r, sigma_star_r,
                                                              h_star_br06, t, i);
    }
    f_mol_r = tap_f32(t, SLOT_BIN(i, N_F_MOL_R), f_mol_r);

    /* H:263 */
    float dM_H2 = 2.0f * (float)M_PI * r * sigma_gas_r * 0.74f * f_mol_r * dr;
    dM_H2 = tap_f32(t, SLOT_BIN(i, N_DM_H2), dM_H2);

    /* H:265 */
    M_H2_total += dM_H2;
    M_H2_total = tap_f32(t, SLOT_BIN(i, N_M_H2_TOTAL), M_H2_total);
  }

  /* H:276, MSUN_TO_CODE_MASS (R/src/macros.h:47) */
  const float H2_code_units = MSUN_TO_CODE_MASS(M_H2_total, h);

  /* H:288-289 */
  galaxies[gal].H2gas = H2_code_units;
  return H2_code_units;
}

// ============================================================================
// OUTPUT OF THE LOCALS
// ============================================================================

/** @brief Whether a replacement by a neighbour changes the recorded output */
struct sensitivity {
  int up;
  int down;
};

/** @brief Write one local: oracle_c06.c's markers (tail-checked) and the narrowing limitation */
static void out_local(const char *name, const struct local_info *info, const struct trace *t,
                      const int slot, const struct sensitivity *s) {
  out_i32(field_name("intermediate.%s_absent", name), t->present[slot] ? 0 : 1);
  if (!t->present[slot]) {
    return;
  }
  if (info->type == VALUE_F32) {
    out_f32(field_name("intermediate.%s", name), (float)t->value[slot]);
  } else {
    out_f64(field_name("intermediate.%s", name), t->value[slot]);
  }
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), 2);
  out_i32(field_name("intermediate.%s_validated_up", name), s->up);
  out_i32(field_name("intermediate.%s_validated_down", name), s->down);
  out_i32(field_name("intermediate.%s_validated", name), s->up && s->down);
  out_i32(field_name("intermediate.%s_narrowing_limited", name), info->narrowing_limited);
}

/** @brief The local recorded in a slot */
static const struct local_info *slot_info(const int slot) {
  if (slot < B_COUNT) {
    return &BR06_LOCALS[slot];
  }
  if (slot < B_COUNT + R_COUNT) {
    return &RADIAL_LOCALS[slot - B_COUNT];
  }
  return &BIN_LOCALS[(slot - B_COUNT - R_COUNT) % N_COUNT];
}

// ============================================================================
// ROUTINES
// ============================================================================

/**
 * @brief calculate_molecular_fraction_BR06 (H:117-125), called directly
 *
 * SAGE26's own call gives return.f_mol; the transcription must reproduce it bit for bit, and
 * gives the locals of the public routine and its two static helpers.
 */
static void run_molecular_fraction_BR06(void) {
  const float gas = in_f32("call.gas_surface_density_msun_pc2");
  const float stars = in_f32("call.stellar_surface_density_msun_pc2");
  const float rs = in_f32("call.disk_scale_length_pc");
  check_all_fields_used();

  const float f_mol = calculate_molecular_fraction_BR06(gas, stars, rs);

  static struct trace t;
  init_trace(&t, -1, 0);
  const float transcribed = transcribe_molecular_fraction_BR06(gas, stars, rs, &t);
  if (!same_f32(transcribed, f_mol)) {
    fatal("transcription check failed: the transcribed calculate_molecular_fraction_BR06 "
          "returns %.9g, SAGE26's own %.9g",
          (double)transcribed, (double)f_mol);
  }

  struct sensitivity sens[B_COUNT];
  memset(sens, 0, sizeof(sens));
  for (int k = 0; k < B_COUNT; k++) {
    if (!t.present[SLOT_BR06(k)]) {
      continue;
    }
    for (int up = 0; up <= 1; up++) {
      static struct trace tp;
      init_trace(&tp, SLOT_BR06(k), up);
      const float perturbed = transcribe_molecular_fraction_BR06(gas, stars, rs, &tp);
      const int changed = !same_f32(perturbed, f_mol);
      if (up) {
        sens[k].up = changed;
      } else {
        sens[k].down = changed;
      }
    }
  }

  out_f32("return.f_mol", f_mol);
  out_i32("intermediate.scale_length_nonpositive", t.scale_length_nonpositive);
  out_i32("intermediate.pressure_branch", t.pressure_branch);
  out_i32("intermediate.stellar_floor_binds", t.stellar_floor_binds);
  out_i32("intermediate.pressure_nonpositive", t.pressure_nonpositive);
  for (int k = 0; k < B_COUNT; k++) {
    out_local(BR06_LOCALS[k].name, &BR06_LOCALS[k], &t, SLOT_BR06(k), &sens[k]);
  }
}

/** The float members of galaxy 0 the radial integration reads, writes or must leave alone */
static void read_radial_galaxy(void) {
  struct GALAXY *g = &galaxies[0];
  g->DiskScaleRadius = in_f32("galaxy[0].DiskScaleRadius");
  g->ColdGas = in_f32("galaxy[0].ColdGas");
  /* Read by the disk-metallicity hoist (H:183-184), which the BR06 branch never uses */
  g->MetalsColdGas = in_f32("galaxy[0].MetalsColdGas");
  g->StellarMass = in_f32("galaxy[0].StellarMass");
  g->BulgeMass = in_f32("galaxy[0].BulgeMass");
  /* Written unless the routine returns early (H:288); written only with an SFR-output
   * pointer (H:283), which sfr_br06 does not pass */
  g->H2gas = in_f32("galaxy[0].H2gas");
  g->H2DepletionTime_Gyr = in_f32("galaxy[0].H2DepletionTime_Gyr");
}

/** @brief Whether the return and every member of galaxy 0 are the same bits as the reference */
static int same_radial_result(const float ret, const struct GALAXY *g, const float expected_ret,
                              const struct GALAXY *expected) {
  return same_f32(ret, expected_ret) && memcmp(g, expected, sizeof(*g)) == 0;
}

/**
 * @brief calculate_molecular_fraction_radial_integration (H:134-290), called directly as
 *        sfr_br06 calls it (R/src/model_starformation_and_feedback.c:222: a NULL SFR pointer)
 *
 * SAGE26's own call gives return.H2_code_units and galaxy 0; the transcription must reproduce
 * both, every member of the galaxy bit for bit, and gives the routine's locals.
 */
static void run_radial_integration(void) {
  read_h2_config();
  run_params.Hubble_h = in_f64("params.Hubble_h");
  read_radial_galaxy();
  check_all_fields_used();

  struct GALAXY input = galaxies[0];

  const float ret = calculate_molecular_fraction_radial_integration(0, galaxies, &run_params, NULL);
  const struct GALAXY end = galaxies[0];

  /* SAGE26 writes H2gas and nothing else of the galaxy on this path */
  struct GALAXY only_h2 = input;
  only_h2.H2gas = end.H2gas;
  if (memcmp(&only_h2, &end, sizeof(end)) != 0) {
    fatal("SAGE26's radial integration wrote a galaxy member other than H2gas");
  }

  static struct trace t;
  init_trace(&t, -1, 0);
  galaxies[0] = input;
  const float transcribed = transcribe_radial_integration(0, &t);
  if (!same_radial_result(transcribed, &galaxies[0], ret, &end)) {
    fatal("transcription check failed: the transcribed radial integration does not reproduce "
          "SAGE26's calculate_molecular_fraction_radial_integration bit for bit");
  }

  static struct sensitivity sens[SLOT_COUNT];
  memset(sens, 0, sizeof(sens));
  for (int slot = B_COUNT; slot < SLOT_COUNT; slot++) {
    if (!t.present[slot]) {
      continue;
    }
    for (int up = 0; up <= 1; up++) {
      static struct trace tp;
      init_trace(&tp, slot, up);
      galaxies[0] = input;
      const float perturbed = transcribe_radial_integration(0, &tp);
      const int changed = !same_radial_result(perturbed, &galaxies[0], ret, &end);
      if (up) {
        sens[slot].up = changed;
      } else {
        sens[slot].down = changed;
      }
    }
  }

  out_f32("return.H2_code_units", ret);
  out_f32("galaxy[0].H2gas", end.H2gas);
  out_f32("galaxy[0].H2DepletionTime_Gyr", end.H2DepletionTime_Gyr);
  out_i32("intermediate.returned_early", t.returned_early);
  out_i32("intermediate.disk_stars_positive", t.disk_stars_positive);
  for (int k = 0; k < R_COUNT; k++) {
    out_local(RADIAL_LOCALS[k].name, &RADIAL_LOCALS[k], &t, SLOT_RADIAL(k), &sens[SLOT_RADIAL(k)]);
  }
  /* The annuli: every bin of SUPPORTED_NBINS, absent where the routine returned early */
  for (int i = 0; i < SUPPORTED_NBINS; i++) {
    out_i32(field_name("intermediate.bin[%d].skipped", i), t.bin_skipped[i]);
    out_i32(field_name("intermediate.bin[%d].stellar_floor_binds", i), t.bin_stellar_floor[i]);
    for (int k = 0; k < N_COUNT; k++) {
      char name[NAME_LEN];
      snprintf(name, sizeof(name), "bin[%d].%s", i, BIN_LOCALS[k].name);
      const int slot = SLOT_BIN(i, k);
      out_local(name, slot_info(slot), &t, slot, &sens[slot]);
    }
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
    {"calculate_molecular_fraction_BR06", run_molecular_fraction_BR06},
    {"calculate_molecular_fraction_radial_integration", run_radial_integration},
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
          "# SAGE26 oracle output for %s, written by oracle_c08 (regenerate.sh); do not edit.\n",
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
