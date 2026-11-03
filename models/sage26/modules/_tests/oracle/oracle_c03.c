/**
 * @file    oracle_c03.c
 * @brief   SAGE26 function-level oracle for the c03 baryon-supply routines
 *
 * Reads one fixture input file, initialises exactly the SAGE26 struct GALAXY,
 * struct halo_data and struct params fields the selected routine reads, reproduces
 * the arithmetic SAGE26's call site (core_build_model.c) performs around that
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
 * The file format is defined in README.md ("Fixture file format"). Every input
 * field must be consumed by the selected routine's reader, so a misspelt or
 * stray field is an error rather than a silently ignored value. Unread struct
 * members are poisoned (all bits set: NaN for floating point, -1 for integers)
 * so an accidental read of an uninitialised field shows up as NaN output.
 *
 * This is SAGE26 reference tooling: it is compiled and linked only by build.sh,
 * never by Mimic's Makefile, and never against Mimic code.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
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

/** @brief Parse an exact-width hexadecimal bit pattern such as 0x3ff0000000000000 */
static uint64_t parse_bits(const char *text, const int hex_digits, const int line) {
  if (text[0] != '0' || text[1] != 'x' || (int)strlen(text + 2) != hex_digits) {
    fatal("line %d: '%s' is not a %d-digit 0x bit pattern", line, text, hex_digits);
  }
  char *end = NULL;
  errno = 0;
  const unsigned long long value = strtoull(text + 2, &end, 16);
  if (errno != 0 || end == NULL || *end != '\0') {
    fatal("line %d: '%s' is not a hexadecimal bit pattern", line, text);
  }
  return (uint64_t)value;
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
  run_params.CGMrecipeOn = in_i32("params.CGMrecipeOn");
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
  }
  check_all_fields_used();

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
}

static void run_add_infall_to_hot(void) {
  run_params.CGMrecipeOn = in_i32("params.CGMrecipeOn");
  const double infallingGas = in_f64("call.InfallingGas");
  const int effective_steps = read_effective_steps();
  struct GALAXY *g = &galaxies[0];
  g->Type = in_i32(galaxy_field(0, "Type"));
  g->EjectedMass = in_f32(galaxy_field(0, "EjectedMass"));
  g->MetalsEjectedMass = in_f32(galaxy_field(0, "MetalsEjectedMass"));
  g->HotGas = in_f32(galaxy_field(0, "HotGas"));
  g->MetalsHotGas = in_f32(galaxy_field(0, "MetalsHotGas"));
  check_all_fields_used();

  /* R/src/core_build_model.c:488: the per-substep share of the snapshot budget */
  const double infallingGas_per_step = infallingGas / effective_steps;
  add_infall_to_hot(0, infallingGas_per_step, galaxies, &run_params);

  out_f64("intermediate.infallingGas_per_step", infallingGas_per_step);
  out_f32(galaxy_field(0, "EjectedMass"), g->EjectedMass);
  out_f32(galaxy_field(0, "MetalsEjectedMass"), g->MetalsEjectedMass);
  out_f32(galaxy_field(0, "HotGas"), g->HotGas);
  out_f32(galaxy_field(0, "MetalsHotGas"), g->MetalsHotGas);
}

static void run_reincorporate_gas(void) {
  run_params.ReIncorporationFactor = in_f64("params.ReIncorporationFactor");
  run_params.CGMrecipeOn = in_i32("params.CGMrecipeOn");
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
  check_all_fields_used();

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
}

static void run_strip_from_satellite(void) {
  run_params.ReionizationOn = in_i32("params.ReionizationOn");
  run_params.CGMrecipeOn = in_i32("params.CGMrecipeOn");
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
  }
  check_all_fields_used();
  if (satellite == centralgal) {
    fatal("call.satellite must differ from call.centralgal");
  }

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
