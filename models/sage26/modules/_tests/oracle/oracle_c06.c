/**
 * @file    oracle_c06.c
 * @brief   SAGE26 function-level oracle for the c06 merger, black-hole, burst and disruption
 *          routines, and the driver arithmetic around them
 *
 * Reads one fixture input file, initialises the SAGE26 struct GALAXY, struct halo_data and
 * struct params fields its reader for the selected routine consumes (a superset of the fields
 * the routine reads), calls the routine from the SAGE26 objects this executable is linked
 * against, and writes a fixture output file holding the routine's return value, every field
 * it writes on every galaxy it touches and every intermediate value the Mimic package carries
 * between modules.
 *
 * Usage: oracle_c06 <fixture.in> <fixture.out>
 *
 * Routines (the `routine` line of the input selects one; R/ is the staged SAGE26 source root,
 * M = R/src/model_mergers.c, B = R/src/core_build_model.c):
 *   estimate_merging_time         M:89-121, SAGE26's own routine, on a halo catalogue
 *   grow_black_hole               M:346-378 (which calls quasar_mode_wind, M:374)
 *   quasar_mode_wind              M:390-442 on its own, with the accreted mass as input
 *   collisional_starburst_recipe  M:584-832, both modes
 *   add_galaxies_together         M:456-543
 *   make_bulge_from_burst         M:553-570
 *   deal_with_galaxy_merger       M:228-333 end to end; with BulgeSizeOn 3 it also records the
 *                                 locals and branch flags of the remnant radius (M:135-212, a
 *                                 TRANSCRIPTION, the routine being static in SAGE26) and each
 *                                 value's narrowing limitation (README "The c07 Group")
 *   disrupt_satellite_to_ICS      M:845-903
 *   merger_pass                   a SOURCE-REVIEWED TRANSCRIPTION of the per-substep merger and
 *                                 disruption pass, B:519-587, calling SAGE26's own
 *                                 disrupt_satellite_to_ICS and deal_with_galaxy_merger
 *   join_progenitors              a SOURCE-REVIEWED TRANSCRIPTION of the clock, infall and
 *                                 history assignment of progenitor joining, B:234-345 and
 *                                 B:358-370, calling SAGE26's own halo helpers,
 *                                 estimate_merging_time, get_disk_radius and get_bulge_radius
 *   join_and_pass                 join_progenitors followed by merger_pass for one substep: the
 *                                 orphan history retention (B:252-282, B:322-337) carried into a
 *                                 merger in the same snapshot
 *   collisional_starburst_recipe_fire, deal_with_galaxy_merger_fire, merger_pass_fire
 *                                 (c09, R3) the same three calls with FIREmodeOn 0 or 1: each
 *                                 galaxy's stored-snapshot redshift read explicitly, MassLoading
 *                                 read and recorded as an invariant field, and the burst's FIRE
 *                                 budget (M:690-752) transcribed and recorded (README "The c09
 *                                 Group")
 *
 * Routine-internal intermediates. Every value of a SAGE26 routine that the package carries and
 * SAGE26 does not return is obtained from a TRANSCRIPTION of that routine below (each statement
 * cited to its line), and is CHECKED: the transcription, composed with SAGE26's own routines,
 * must reproduce SAGE26's own end state bit for bit, or the oracle refuses the fixture. A value
 * is continuation-checked when the routine passes the variable holding it to a SAGE26 function
 * (the composition then feeds the transcribed value into SAGE26's own function), and
 * tail-checked when it does not (the composition then runs the transcribed rest of the routine).
 * Each value is then replaced by its upward and its downward neighbouring double and the same
 * composition rerun (two-sided validation).
 *
 * SFprescription 1 (c08, R2). The merger-family routines read SFprescription 0 (R0, R1) or 1
 * (R2: BR06 with radial integration, with H2RadialIntegrationOn 1, H2RadialNBins 25 and
 * H2RadialRMaxFactor 5.0 required explicitly, as oracle_c05.c's reader requires them). For 1 the
 * transcriptions run SAGE26's three hydrogen sites these routines reach: the black-hole accretion
 * cap (M:366-372, guard SFprescription != 0 && != 2), the post-reheating cap of the burst's
 * update_from_feedback (S:1169-1173) and the burst's hydrogen refresh (M:798-803, guard
 * sf_prescription_tracks_h2); their flags and values are recorded after every earlier field,
 * so an SFprescription 0 output is unchanged (README "The c08 Group"). The black-hole cap of the
 * post-minor-merger recheck runs inside SAGE26's own check_disk_instability; the burst record
 * holds an OBSERVATION of it (SAGE26's own grow_black_hole on a copy of the recheck's state,
 * checked against SAGE26's recheck), struct recheck_bh_observation.
 *
 * FIREmodeOn 1 (c09, R3). The routines named *_fire read FIREmodeOn 0 or 1 (every other routine
 * still requires 0), each galaxy's stored SnapNum and, for each distinct one, params.ZZ[<s>],
 * and the float MassLoading of every galaxy, which no c06 routine writes. The burst's
 * transcription runs SAGE26's FIRE budget (M:690-752): the scaling from the TARGET's
 * (merger_centralgal's) float Vvir and ZZ[SnapNum], capped_eta_reheat's loading times the stars
 * with no MassLoading write, the burst's renormalisation, sn_energy_coupling and the energy-based
 * ejection; its values and flags are recorded after every earlier field. The post-minor-merger
 * recheck's own burst runs inside SAGE26's check_disk_instability; the record holds two
 * OBSERVATIONS of it (struct recheck_fire_observation): whether SAGE26's recheck changes the state
 * when its burst runs with FIREmodeOn 0, and when the target's stored-snapshot redshift is
 * replaced.
 *
 * Driver transcriptions. evolve_galaxies and join_galaxies_of_progenitors are static in
 * core_build_model.c, which this link excludes, so no SAGE26 code can anchor merger_pass,
 * join_progenitors or join_and_pass. They are SOURCE-REVIEWED TRANSCRIPTIONS: every statement
 * is cited to its line, and a mutation sweep covers them. Where they call a SAGE26 routine the
 * continuation is SAGE26's own, which shows nothing about the transcription itself.
 *
 * Every transcription restates SAGE26's statements with the same expression shapes, operand
 * types and operand order and is compiled with the same flags, so floating-point contraction
 * (clang's default -ffp-contract=on) treats them as it treats SAGE26's; README.md's c06
 * fused-instruction table lists the sites, checked in this file's disassembly. File-static
 * SAGE26 constants are restated with their values and source lines.
 *
 * The file format is defined in README.md ("Fixture File Format"; the c06 section adds indexed
 * halo-catalogue fields). The parsing and output helpers are copies of oracle_c05.c's, which
 * stays unchanged.
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
#include "model_disk_instability.h"
#include "model_halo_properties.h"
#include "model_mergers.h"
#include "model_misc.h"
#include "model_starformation_and_feedback.h"

#define ORACLE_FORMAT_LINE "format sage26-oracle-fixture 1"
#define MAX_FIELDS 1024
#define MAX_GALAXIES 4
#define MAX_HALOS 8
#define NAME_LEN 96
#define LINE_LEN 512

/* The halonr argument the merger routines pass on: on R0 it reaches only the instability's
 * mode-1 burst, which never reads it (M:807 requires mode 0). */
#define ORACLE_HALONR 0

/* SAGE26 file-static constants, restated (values and lines of the staged source) */
static const double DISK_HALF_MASS_FRAC = 1.68;    /* M:37 */
static const double COVINGTON11_C_RAD = 2.75;      /* M:41 */
static const double BH_GROWTH_V_KMS = 280.0;       /* M:45 */
static const double STARBURST_FRAC_COEFF = 0.56;   /* M:50 */
static const double STARBURST_MASS_POWER = 0.7;    /* M:51 */
static const double KD11_METAL_HALO_MASS = 30.0;   /* M:65 */
static const double FIRE_V_CRIT_KMS = 60.0;        /* M:56 (c09) */
static const double FIRE_REDSHIFT_EXPONENT = 1.25; /* M:60 (c09) */

/* c09: set by the *_fire routines: FIREmodeOn may be 1, each galaxy's stored-snapshot redshift is
 * read, and MassLoading (BLOCK_FIRE) is read and recorded */
static int fire_routine = 0;

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
  fprintf(stderr, "oracle_c06: %s: ", input_path != NULL ? input_path : "(no input)");
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

// ============================================================================
// GALAXY AND HALO FIELDS
// ============================================================================

enum member_kind { MK_F32, MK_F64, MK_I32 };

/** @brief One member of SAGE26's struct GALAXY: a scalar or a STEPS-element history */
struct member {
  const char *name;
  enum member_kind kind;
  size_t offset;
  int elements; /* 1 for a scalar, STEPS for a history */
};

#define F32_MEMBER(m) {#m, MK_F32, offsetof(struct GALAXY, m), 1}
#define F64_MEMBER(m) {#m, MK_F64, offsetof(struct GALAXY, m), 1}
#define I32_MEMBER(m) {#m, MK_I32, offsetof(struct GALAXY, m), 1}
#define HISTORY_MEMBER(m) {#m, MK_F32, offsetof(struct GALAXY, m), STEPS}
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

/** Identity and bookkeeping integers (R/src/core_allvars.h:108-128) */
static const struct member IDENT_MEMBERS[] = {I32_MEMBER(Type),       I32_MEMBER(SnapNum),
                                              I32_MEMBER(mergeType),  I32_MEMBER(mergeIntoID),
                                              I32_MEMBER(CentralGal), I32_MEMBER(Len)};

/** The halo-derived floats and the merger clock (core_allvars.h:129-133, :187) */
static const struct member HALO_MEMBERS[] = {F32_MEMBER(Mvir), F32_MEMBER(deltaMvir),
                                             F32_MEMBER(Rvir), F32_MEMBER(Vvir),
                                             F32_MEMBER(Vmax), F32_MEMBER(MergTime)};

/** Every float reservoir, channel, radius and timestamp a c06 routine reads or writes */
static const struct member RESERVOIR_MEMBERS[] = {F32_MEMBER(ColdGas),
                                                  F32_MEMBER(MetalsColdGas),
                                                  F32_MEMBER(StellarMass),
                                                  F32_MEMBER(MetalsStellarMass),
                                                  F32_MEMBER(BulgeMass),
                                                  F32_MEMBER(MetalsBulgeMass),
                                                  F32_MEMBER(MergerBulgeMass),
                                                  F32_MEMBER(InstabilityBulgeMass),
                                                  F32_MEMBER(HotGas),
                                                  F32_MEMBER(MetalsHotGas),
                                                  F32_MEMBER(EjectedMass),
                                                  F32_MEMBER(MetalsEjectedMass),
                                                  F32_MEMBER(BlackHoleMass),
                                                  F32_MEMBER(QuasarModeBHaccretionMass),
                                                  F32_MEMBER(ICS),
                                                  F32_MEMBER(MetalsICS),
                                                  F32_MEMBER(ICS_disrupt),
                                                  F32_MEMBER(ICS_accrete),
                                                  F32_MEMBER(ICS_sum_mt),
                                                  F32_MEMBER(CGMgas),
                                                  F32_MEMBER(MetalsCGMgas),
                                                  F32_MEMBER(H1gas),
                                                  F32_MEMBER(H2gas),
                                                  F32_MEMBER(OutflowRate),
                                                  F32_MEMBER(DiskScaleRadius),
                                                  F32_MEMBER(BulgeRadius),
                                                  F32_MEMBER(MergerBulgeRadius),
                                                  F32_MEMBER(InstabilityBulgeRadius),
                                                  F32_MEMBER(TimeOfLastMajorMerger),
                                                  F32_MEMBER(TimeOfLastMinorMerger)};

/** The infall record and the two double energy totals progenitor joining resets */
static const struct member INFALL_MEMBERS[] = {
    F32_MEMBER(infallMvir),   F32_MEMBER(infallVvir),
    F32_MEMBER(infallVmax),   F32_MEMBER(infallStellarMass),
    F32_MEMBER(TimeOfInfall), F64_MEMBER(Cooling),
    F64_MEMBER(Heating)};

/** Written by joining only (B:235, B:237); never read by a c06 routine */
static const struct member JOIN_WRITTEN_MEMBERS[] = {I32_MEMBER(HaloNr), F32_MEMBER(dT)};

/** The six per-substep histories, STEPS float elements each (core_allvars.h:158-163) */
static const struct member HISTORY_MEMBERS[] = {
    HISTORY_MEMBER(SfrDisk),         HISTORY_MEMBER(SfrBulge),
    HISTORY_MEMBER(SfrDiskColdGas),  HISTORY_MEMBER(SfrDiskColdGasMetals),
    HISTORY_MEMBER(SfrBulgeColdGas), HISTORY_MEMBER(SfrBulgeColdGasMetals)};

/** c09: the float MassLoading (core_allvars.h), which the disk FIRE path writes
 * (R/src/model_starformation_and_feedback.c:762-764) and no c06 routine writes: the burst's FIRE
 * budget leaves it as it was (M:707-710). Read and recorded by the *_fire routines only. */
static const struct member FIRE_MEMBERS[] = {F32_MEMBER(MassLoading)};

/** Which groups of members a galaxy reads or records */
enum block {
  BLOCK_IDENT = 1,
  BLOCK_HALO = 2,
  BLOCK_RESERVOIRS = 4,
  BLOCK_INFALL = 8,
  BLOCK_JOIN_WRITTEN = 16,
  BLOCK_HISTORIES = 32,
  BLOCK_FIRE = 64, /* c09: MassLoading, after every other block of the galaxy */
  BLOCK_MERGER_INPUT = BLOCK_IDENT | BLOCK_HALO | BLOCK_RESERVOIRS | BLOCK_HISTORIES,
  BLOCK_JOIN_INPUT = BLOCK_MERGER_INPUT | BLOCK_INFALL,
  BLOCK_ALL = BLOCK_JOIN_INPUT | BLOCK_JOIN_WRITTEN
};

static const struct {
  int block;
  const struct member *members;
  size_t count;
} BLOCKS[] = {
    {BLOCK_IDENT, IDENT_MEMBERS, COUNT(IDENT_MEMBERS)},
    {BLOCK_HALO, HALO_MEMBERS, COUNT(HALO_MEMBERS)},
    {BLOCK_RESERVOIRS, RESERVOIR_MEMBERS, COUNT(RESERVOIR_MEMBERS)},
    {BLOCK_INFALL, INFALL_MEMBERS, COUNT(INFALL_MEMBERS)},
    {BLOCK_JOIN_WRITTEN, JOIN_WRITTEN_MEMBERS, COUNT(JOIN_WRITTEN_MEMBERS)},
    {BLOCK_HISTORIES, HISTORY_MEMBERS, COUNT(HISTORY_MEMBERS)},
    {BLOCK_FIRE, FIRE_MEMBERS, COUNT(FIRE_MEMBERS)},
};

static void *member_address(const struct GALAXY *g, const struct member *m, const int element) {
  const size_t size = m->kind == MK_F64 ? sizeof(double) : sizeof(float);
  return (char *)g + m->offset + (size_t)element * size;
}

static const char *member_field(const char *prefix, const int index, const struct member *m,
                                const int element) {
  if (m->elements == 1) {
    return field_name("%sgalaxy[%d].%s", prefix, index, m->name);
  }
  return field_name("%sgalaxy[%d].%s[%d]", prefix, index, m->name, element);
}

static void read_galaxy(const int i, const int blocks) {
  for (size_t b = 0; b < COUNT(BLOCKS); b++) {
    if (!(blocks & BLOCKS[b].block)) {
      continue;
    }
    for (size_t k = 0; k < BLOCKS[b].count; k++) {
      const struct member *m = &BLOCKS[b].members[k];
      for (int e = 0; e < m->elements; e++) {
        void *address = member_address(&galaxies[i], m, e);
        const char *name = member_field("", i, m, e);
        if (m->kind == MK_F32) {
          const float v = in_f32(name);
          memcpy(address, &v, sizeof(v));
        } else if (m->kind == MK_F64) {
          const double v = in_f64(name);
          memcpy(address, &v, sizeof(v));
        } else {
          const int32_t v = in_i32(name);
          memcpy(address, &v, sizeof(v));
        }
      }
    }
  }
}

/** @brief Write every member of the given blocks of galaxy g under <prefix>galaxy[i] */
static void write_galaxy(const char *prefix, const int i, const struct GALAXY *g,
                         const int blocks) {
  for (size_t b = 0; b < COUNT(BLOCKS); b++) {
    if (!(blocks & BLOCKS[b].block)) {
      continue;
    }
    for (size_t k = 0; k < BLOCKS[b].count; k++) {
      const struct member *m = &BLOCKS[b].members[k];
      for (int e = 0; e < m->elements; e++) {
        const void *address = member_address(g, m, e);
        const char *name = member_field(prefix, i, m, e);
        if (m->kind == MK_F32) {
          float v;
          memcpy(&v, address, sizeof(v));
          out_f32(name, v);
        } else if (m->kind == MK_F64) {
          double v;
          memcpy(&v, address, sizeof(v));
          out_f64(name, v);
        } else {
          int32_t v;
          memcpy(&v, address, sizeof(v));
          out_i32(name, v);
        }
      }
    }
  }
}

/** @brief Which blocks of each galaxy a fixture records (0: the galaxy is not recorded) */
struct record_spec {
  int ngal;
  int blocks[MAX_GALAXIES];
};

/** @brief Whether every recorded member of two galaxy arrays has the same bit pattern */
static int same_recorded(const struct GALAXY *a, const struct GALAXY *b,
                         const struct record_spec *spec) {
  for (int i = 0; i < spec->ngal; i++) {
    for (size_t blk = 0; blk < COUNT(BLOCKS); blk++) {
      if (!(spec->blocks[i] & BLOCKS[blk].block)) {
        continue;
      }
      for (size_t k = 0; k < BLOCKS[blk].count; k++) {
        const struct member *m = &BLOCKS[blk].members[k];
        const size_t size = m->kind == MK_F64 ? sizeof(double) : sizeof(float);
        for (int e = 0; e < m->elements; e++) {
          if (memcmp(member_address(&a[i], m, e), member_address(&b[i], m, e), size) != 0) {
            return 0;
          }
        }
      }
    }
  }
  return 1;
}

static void write_recorded(const char *prefix, const struct GALAXY *g,
                           const struct record_spec *spec) {
  for (int i = 0; i < spec->ngal; i++) {
    if (spec->blocks[i] != 0) {
      write_galaxy(prefix, i, &g[i], spec->blocks[i]);
    }
  }
}

/** @brief The halo-catalogue members a c06 routine reads (R/src/core_simulation.h:17-46) */
enum halo_blocks {
  HALO_BASIC = 1, /* FirstHaloInFOFgroup, Len, Mvir, SnapNum: the virial helpers */
  HALO_JOIN = 2   /* Vmax and Spin: progenitor joining (B:262, get_disk_radius) */
};

static int read_halo_catalogue(const int blocks) {
  const int nhalos = in_i32("call.nhalos");
  if (nhalos < 1 || nhalos > MAX_HALOS) {
    fatal("call.nhalos = %d is outside [1, %d]", nhalos, MAX_HALOS);
  }
  for (int k = 0; k < nhalos; k++) {
    struct halo_data *h = &halos[k];
    h->FirstHaloInFOFgroup = in_i32(field_name("halo[%d].FirstHaloInFOFgroup", k));
    h->Len = in_i32(field_name("halo[%d].Len", k));
    h->Mvir = in_f32(field_name("halo[%d].Mvir", k));
    h->SnapNum = in_i32(field_name("halo[%d].SnapNum", k));
    if (h->FirstHaloInFOFgroup < 0 || h->FirstHaloInFOFgroup >= nhalos || h->SnapNum < 0 ||
        h->SnapNum >= ABSOLUTEMAXSNAPS) {
      fatal("halo[%d] has FirstHaloInFOFgroup %d or SnapNum %d outside the catalogue", k,
            h->FirstHaloInFOFgroup, h->SnapNum);
    }
    if (blocks & HALO_JOIN) {
      h->Vmax = in_f32(field_name("halo[%d].Vmax", k));
      for (int j = 0; j < 3; j++) {
        h->Spin[j] = in_f32(field_name("halo[%d].Spin[%d]", k, j));
      }
    }
  }
  /* The redshift of every distinct halo snapshot (get_virial_radius, model_halo_properties.c:65) */
  for (int k = 0; k < nhalos; k++) {
    int seen = 0;
    for (int j = 0; j < k; j++) {
      seen |= halos[j].SnapNum == halos[k].SnapNum;
    }
    if (!seen) {
      run_params.ZZ[halos[k].SnapNum] = in_f64(field_name("params.ZZ[%d]", halos[k].SnapNum));
    }
  }
  return nhalos;
}

// ============================================================================
// STATE INITIALISATION: FLAGS, PARAMETERS AND LAYOUT
// ============================================================================

/** @brief Fill every struct with all-bits-set so any field left unset reads as NaN or -1 */
static void poison_state(void) {
  memset(galaxies, 0xff, sizeof(galaxies));
  memset(halos, 0xff, sizeof(halos));
  memset(&run_params, 0xff, sizeof(run_params));
}

/** @brief Read an R0 flag the oracle requires at a fixed value */
static int32_t require_flag(const char *name, const int value) {
  const int read = in_i32(field_name("params.%s", name));
  if (read != value) {
    fatal("params.%s = %d, but the oracle requires R0's %d", name, read, value);
  }
  return read;
}

/**
 * @brief BulgeSizeOn as the fixture sets it, and Hubble_h when the sizes are on
 *
 * BulgeSizeOn is 0 (R0) or 3 (R1 onward, the Tonini sizes); modes 1 and 2 are not ported (the
 * campaign supports exactly 0 and 3) and any other value is refused, naming the parameter. Mode
 * 3's size fallbacks read Hubble_h (R/src/model_misc.c:249,398), so a mode-3 fixture must carry
 * params.Hubble_h (a missing field is refused, naming it); a mode-0 fixture does not, and one that
 * carries it is refused as an unread field, so every R0 input is read as before.
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

/* The H2 configuration the oracle supports with SFprescription 1: SAGE26's defaults
 * (R/src/core_read_parameter_file.c:99-101; R2.par sets none of them), as oracle_c05.c */
#define SUPPORTED_H2_NBINS 25
#define SUPPORTED_H2_RMAX_FACTOR 5.0

/**
 * @brief SFprescription as the fixture sets it: 0 (R0, R1) or 1 (R2: BR06 with radial
 *        integration), and, for 1, the H2 configuration it is supported with (c08)
 *
 * oracle_c05.c's read_sf_prescription restated: SFprescription 1 requires, explicitly and at
 * exactly these values, H2RadialIntegrationOn 1, H2RadialNBins 25 and H2RadialRMaxFactor 5.0
 * (SAGE26's defaults, R/src/core_read_parameter_file.c:99-101). Any other value is refused, naming
 * the parameter. No c06 routine reads them on a supported path (only the StarburstColdGasOn 0
 * fuel, M:606-679, which read_merger_config refuses, calls the radial integration), so requiring
 * them only keeps the configuration R2's whole and the poisoned state from supplying one.
 * H2DiskAreaOption stays poisoned (read only by M:625-629 and S:225-236, neither supported).
 * SFprescription 0 reads nothing more, so every earlier fixture is read unchanged.
 */
static void read_sf_prescription(void) {
  run_params.SFprescription = in_i32("params.SFprescription");
  if (run_params.SFprescription != 0 && run_params.SFprescription != 1) {
    fatal("params.SFprescription = %d, but the oracle supports only 0 (R0, R1) and 1 (R2: BR06 "
          "with radial integration)",
          run_params.SFprescription);
  }
  if (run_params.SFprescription != 1) {
    return;
  }
  run_params.H2RadialIntegrationOn = in_i32("params.H2RadialIntegrationOn");
  if (run_params.H2RadialIntegrationOn != 1) {
    fatal("params.H2RadialIntegrationOn = %d, but the oracle supports only 1 (radial "
          "integration; the single-slab branch is not ported)",
          run_params.H2RadialIntegrationOn);
  }
  run_params.H2RadialNBins = in_i32("params.H2RadialNBins");
  if (run_params.H2RadialNBins != SUPPORTED_H2_NBINS) {
    fatal("params.H2RadialNBins = %d, but the oracle supports only %d", run_params.H2RadialNBins,
          SUPPORTED_H2_NBINS);
  }
  run_params.H2RadialRMaxFactor = in_f64("params.H2RadialRMaxFactor");
  if (!same_f64(run_params.H2RadialRMaxFactor, SUPPORTED_H2_RMAX_FACTOR)) {
    fatal("params.H2RadialRMaxFactor = %.17g, but the oracle supports only %.1f",
          run_params.H2RadialRMaxFactor, SUPPORTED_H2_RMAX_FACTOR);
  }
}

/**
 * @brief FIREmodeOn as a c09 fixture sets it: 0 (R0-R2, fixed-epsilon feedback) or 1 (R3, the
 *        FIRE budget of every burst, M:690-752); any other value is refused, naming the parameter
 *        (oracle_c05.c's read_fire_mode_on restated)
 */
static int32_t read_fire_mode_on(void) {
  const int read = in_i32("params.FIREmodeOn");
  if (read != 0 && read != 1) {
    fatal("params.FIREmodeOn = %d, but the oracle supports only 0 (R0-R2) and 1 (R3)", read);
  }
  return read;
}

/**
 * @brief c09: each galaxy's stored-snapshot redshift, read explicitly
 *
 * The burst's FIRE scaling reads ZZ[galaxies[merger_centralgal].SnapNum] (M:694), the snapshot the
 * target carries, not the current halo's. params.ZZ[<s>] is read once for each distinct stored
 * SnapNum of the fixture's galaxies; every other entry stays poisoned, so the poisoned state never
 * supplies a redshift. A SnapNum outside [0, ABSOLUTEMAXSNAPS), an index SAGE26 would read out of
 * bounds, is refused (the package refuses an index outside its redshift table instead, campaign
 * decision D35). A FIREmodeOn 0 fixture reads the same fields as its FIREmodeOn 1 twin.
 */
static void read_fire_snapshots(const int ngal) {
  for (int i = 0; i < ngal; i++) {
    const int snap = galaxies[i].SnapNum;
    if (snap < 0 || snap >= ABSOLUTEMAXSNAPS) {
      fatal("galaxy[%d].SnapNum = %d is outside [0, %d): SAGE26 would read ZZ out of bounds", i,
            snap, ABSOLUTEMAXSNAPS);
    }
    int seen = 0;
    for (int j = 0; j < i; j++) {
      seen |= galaxies[j].SnapNum == snap;
    }
    if (!seen) {
      run_params.ZZ[snap] = in_f64(field_name("params.ZZ[%d]", snap));
    }
  }
}

/**
 * @brief R0's flags and parameters for every routine that can reach a merger, a burst or the
 *        instability
 *
 * The pins are R0's run config, the campaign's par/R0.par (an edit of the upstream template
 * R/input/millennium.par, whose flag lines 48-57 differ): AGNrecipeOn 2 (R0.par:49),
 * SupernovaRecipeOn 1 (:50), DiskInstabilityOn 1 (:52), CGMrecipeOn 0 (:54), FIREmodeOn 0 (:55)
 * and SaveFullSFH 0 (:60); StarburstColdGasOn 1 is SAGE26's default
 * (R/src/core_read_parameter_file.c:109), which R0.par leaves alone. BulgeSizeOn is read, not
 * pinned: 0 (R0.par:109) or 3 (R1.par:109), with Hubble_h for 3 (read_bulge_size_config).
 * SFprescription is read, not pinned: 0 (R0.par:48, R1.par:48) or 1 (R2.par:48), with the H2
 * configuration for 1 (read_sf_prescription, c08). FIREmodeOn is 0, or 0 or 1 (R3.par:55, c09,
 * through read_fire_mode_on) for the *_fire routines.
 *
 * The transcriptions below implement R0's and R1's paths and, with SFprescription 1, R2's: the
 * SN recipe without FIRE, no CGM, the Bondi AGN recipe (AGNrecipeOn 2), cold-gas starbursts,
 * no full SFH and the disk instability on; with SFprescription 1 the hydrogen sites M:366-372,
 * S:1169-1173 and M:798-803 run. The code constants are SAGE26's (EtaSNcode, EnergySNcode,
 * R/src/core_init.c:102-103; G, :96).
 */
static void read_merger_config(void) {
  read_sf_prescription();
  run_params.SupernovaRecipeOn = require_flag("SupernovaRecipeOn", 1);
  run_params.CGMrecipeOn = require_flag("CGMrecipeOn", 0);
  run_params.FIREmodeOn = fire_routine ? read_fire_mode_on() : require_flag("FIREmodeOn", 0);
  read_bulge_size_config();
  run_params.SaveFullSFH = require_flag("SaveFullSFH", 0);
  run_params.AGNrecipeOn = require_flag("AGNrecipeOn", 2);
  run_params.StarburstColdGasOn = require_flag("StarburstColdGasOn", 1);
  run_params.DiskInstabilityOn = require_flag("DiskInstabilityOn", 1);
  run_params.FeedbackReheatingEpsilon = in_f64("params.FeedbackReheatingEpsilon");
  run_params.FeedbackEjectionEfficiency = in_f64("params.FeedbackEjectionEfficiency");
  run_params.EtaSNcode = in_f64("params.EtaSNcode");
  run_params.EnergySNcode = in_f64("params.EnergySNcode");
  run_params.RecycleFraction = in_f64("params.RecycleFraction");
  run_params.Yield = in_f64("params.Yield");
  run_params.FracZleaveDisk = in_f64("params.FracZleaveDisk");
  run_params.ThreshMajorMerger = in_f64("params.ThreshMajorMerger");
  run_params.BlackHoleGrowthRate = in_f64("params.BlackHoleGrowthRate");
  run_params.QuasarModeEfficiency = in_f64("params.QuasarModeEfficiency");
  run_params.UnitVelocity_in_cm_per_s = in_f64("params.UnitVelocity_in_cm_per_s");
  run_params.G = in_f64("params.G");
}

/**
 * @brief The clock's parameters (estimate_merging_time and the virial helpers it calls,
 *        R/src/model_halo_properties.c:31-85): PartMass, MergerTimeFactor and the cosmology,
 *        with G and Hubble at SAGE26's derived bit patterns (R/src/core_init.c:96, :106)
 */
static void read_clock_params(const int read_g) {
  run_params.PartMass = in_f64("params.PartMass");
  run_params.MergerTimeFactor = in_f64("params.MergerTimeFactor");
  if (read_g) {
    run_params.G = in_f64("params.G");
  }
  run_params.Hubble = in_f64("params.Hubble");
  run_params.Omega = in_f64("params.Omega");
  run_params.OmegaLambda = in_f64("params.OmegaLambda");
}

static int read_index(const char *name, const int limit) {
  const int value = in_i32(name);
  if (value < 0 || value >= limit) {
    fatal("%s = %d is outside [0, %d)", name, value, limit);
  }
  return value;
}

static int read_ngal(void) {
  const int ngal = in_i32("call.ngal");
  if (ngal < 1 || ngal > MAX_GALAXIES) {
    fatal("call.ngal = %d is outside [1, %d]", ngal, MAX_GALAXIES);
  }
  return ngal;
}

// ============================================================================
// CARRIED INTERMEDIATES
// ============================================================================

/** The SAGE26 routine a carried value is a local of */
enum stage {
  STAGE_CLOCK,      /* estimate_merging_time */
  STAGE_MERGER,     /* deal_with_galaxy_merger */
  STAGE_ADD,        /* add_galaxies_together */
  STAGE_BH,         /* grow_black_hole */
  STAGE_WIND,       /* quasar_mode_wind */
  STAGE_BURST,      /* collisional_starburst_recipe */
  STAGE_FEEDBACK,   /* update_from_feedback, as the burst calls it */
  STAGE_DISRUPT,    /* disrupt_satellite_to_ICS */
  STAGE_FIRE_HELPER /* c09: a local of capped_eta_reheat or sn_energy_coupling, in the burst */
};

/** How a carried value's transcription is checked (README "Carried Values") */
enum check_kind {
  CHECK_CONTINUATION = 1, /* the routine passes the variable to a SAGE26 function */
  CHECK_TAIL = 2          /* it does not: the transcribed rest of the routine runs */
};

enum im_id {
  IM_SAT_VIRIAL_MASS,
  IM_COULOMB,
  IM_SATELLITE_MASS,
  IM_SATELLITE_RADIUS,
  IM_MOTHER_VVIR,
  IM_MERGTIME_PRE_CAP,
  IM_MI,
  IM_MA,
  IM_MASS_RATIO,
  IM_CENTRAL_DISK_MASS,
  IM_OLD_DISK_RADIUS,
  IM_NEW_MERGER_RADIUS,
  IM_SATELLITE_DISK_MASS,
  IM_DISK_MASS_POST,
  IM_DISK_FRACTION_POST,
  IM_BH_ACCRETE_PRE_CAP,
  IM_BH_ACCRETE,
  IM_BH_METALLICITY,
  IM_QUASAR_ENERGY,
  IM_COLD_GAS_ENERGY,
  IM_HOT_GAS_ENERGY,
  IM_EBURST,
  IM_GAS_FOR_STARBURST,
  IM_STARS_PRE_RENORM,
  IM_REHEATED_PRE_RENORM,
  IM_RENORM_FAC,
  IM_STARS,
  IM_REHEATED,
  IM_EJECTED_PRE_FLOOR,
  IM_EJECTED,
  IM_METALLICITY_SF,
  IM_RECYCLED_STARS,
  IM_METALLICITY_FB,
  IM_REHEATED_FB_CLAMPED,
  IM_EJECTED_CAPPED,
  IM_METALLICITY_HOT,
  IM_METALS_TO_EJECT,
  IM_METALS_TO_EJECT_CAPPED,
  IM_FRAC_Z_LEAVE,
  IM_YIELD_TO_HOT,
  IM_TOTAL_GAS,
  IM_TOTAL_METALS_GAS,
  /* c07: the locals of calculate_merger_remnant_radius (M:138-199), recorded with
   * BulgeSizeOn 3 only; new_merger_radius, its return value, is IM_NEW_MERGER_RADIUS */
  IM_REM_M1,
  IM_REM_M2,
  IM_REM_M_TOT,
  IM_REM_R1_DISK_HALF,
  IM_REM_R1_BULGE_HALF,
  IM_REM_M1_DISK,
  IM_REM_M1_BULGE,
  IM_REM_R1_PROGENITOR,
  IM_REM_R2_DISK_HALF,
  IM_REM_R2_BULGE_HALF,
  IM_REM_M2_DISK,
  IM_REM_M2_BULGE,
  IM_REM_R2_PROGENITOR,
  IM_REM_R1,
  IM_REM_R2,
  IM_REM_E_INIT,
  IM_REM_E_ORB,
  IM_REM_F_GAS,
  IM_REM_E_RAD,
  IM_REM_E_FINAL,
  IM_COUNT
};

static const struct {
  const char *name;
  enum stage stage;
  enum check_kind check;
} IM_INFO[IM_COUNT] = {
    [IM_SAT_VIRIAL_MASS] = {"sat_virial_mass", STAGE_CLOCK, CHECK_TAIL},
    [IM_COULOMB] = {"coulomb", STAGE_CLOCK, CHECK_TAIL},
    [IM_SATELLITE_MASS] = {"SatelliteMass", STAGE_CLOCK, CHECK_TAIL},
    [IM_SATELLITE_RADIUS] = {"SatelliteRadius", STAGE_CLOCK, CHECK_TAIL},
    [IM_MOTHER_VVIR] = {"mother_virial_velocity", STAGE_CLOCK, CHECK_TAIL},
    [IM_MERGTIME_PRE_CAP] = {"mergtime_pre_cap", STAGE_CLOCK, CHECK_TAIL},
    [IM_MI] = {"mi", STAGE_MERGER, CHECK_TAIL},
    [IM_MA] = {"ma", STAGE_MERGER, CHECK_TAIL},
    [IM_MASS_RATIO] = {"mass_ratio", STAGE_MERGER, CHECK_CONTINUATION},
    [IM_CENTRAL_DISK_MASS] = {"central_disk_mass", STAGE_MERGER, CHECK_TAIL},
    [IM_OLD_DISK_RADIUS] = {"old_disk_radius", STAGE_MERGER, CHECK_CONTINUATION},
    [IM_NEW_MERGER_RADIUS] = {"new_merger_radius", STAGE_MERGER, CHECK_TAIL},
    [IM_SATELLITE_DISK_MASS] = {"satellite_disk_mass", STAGE_ADD, CHECK_CONTINUATION},
    [IM_DISK_MASS_POST] = {"disk_mass_post", STAGE_ADD, CHECK_TAIL},
    [IM_DISK_FRACTION_POST] = {"disk_fraction_post", STAGE_ADD, CHECK_TAIL},
    [IM_BH_ACCRETE_PRE_CAP] = {"BHaccrete_pre_cap", STAGE_BH, CHECK_CONTINUATION},
    [IM_BH_ACCRETE] = {"BHaccrete", STAGE_BH, CHECK_CONTINUATION},
    [IM_BH_METALLICITY] = {"bh_metallicity", STAGE_BH, CHECK_TAIL},
    [IM_QUASAR_ENERGY] = {"quasar_energy", STAGE_WIND, CHECK_TAIL},
    [IM_COLD_GAS_ENERGY] = {"cold_gas_energy", STAGE_WIND, CHECK_TAIL},
    [IM_HOT_GAS_ENERGY] = {"hot_gas_energy", STAGE_WIND, CHECK_TAIL},
    [IM_EBURST] = {"eburst", STAGE_BURST, CHECK_TAIL},
    [IM_GAS_FOR_STARBURST] = {"gas_for_starburst", STAGE_BURST, CHECK_TAIL},
    [IM_STARS_PRE_RENORM] = {"stars_pre_renorm", STAGE_BURST, CHECK_CONTINUATION},
    [IM_REHEATED_PRE_RENORM] = {"reheated_mass_pre_renorm", STAGE_BURST, CHECK_CONTINUATION},
    [IM_RENORM_FAC] = {"renorm_fac", STAGE_BURST, CHECK_TAIL},
    [IM_STARS] = {"stars", STAGE_BURST, CHECK_CONTINUATION},
    [IM_REHEATED] = {"reheated_mass", STAGE_BURST, CHECK_CONTINUATION},
    [IM_EJECTED_PRE_FLOOR] = {"ejected_mass_pre_floor", STAGE_BURST, CHECK_CONTINUATION},
    [IM_EJECTED] = {"ejected_mass", STAGE_BURST, CHECK_CONTINUATION},
    [IM_METALLICITY_SF] = {"metallicity_sf", STAGE_BURST, CHECK_CONTINUATION},
    [IM_RECYCLED_STARS] = {"recycled_stars", STAGE_BURST, CHECK_CONTINUATION},
    [IM_METALLICITY_FB] = {"metallicity_fb", STAGE_BURST, CHECK_CONTINUATION},
    [IM_REHEATED_FB_CLAMPED] = {"reheated_mass_fb_clamped", STAGE_FEEDBACK, CHECK_TAIL},
    [IM_EJECTED_CAPPED] = {"ejected_mass_capped", STAGE_FEEDBACK, CHECK_TAIL},
    [IM_METALLICITY_HOT] = {"metallicity_hot", STAGE_FEEDBACK, CHECK_TAIL},
    [IM_METALS_TO_EJECT] = {"metals_hot_to_eject", STAGE_FEEDBACK, CHECK_TAIL},
    [IM_METALS_TO_EJECT_CAPPED] = {"metals_hot_to_eject_capped", STAGE_FEEDBACK, CHECK_TAIL},
    [IM_FRAC_Z_LEAVE] = {"FracZleaveDiskVal", STAGE_BURST, CHECK_TAIL},
    [IM_YIELD_TO_HOT] = {"yield_metals_to_hot", STAGE_BURST, CHECK_CONTINUATION},
    [IM_TOTAL_GAS] = {"total_gas", STAGE_DISRUPT, CHECK_CONTINUATION},
    [IM_TOTAL_METALS_GAS] = {"total_metals_gas", STAGE_DISRUPT, CHECK_CONTINUATION},
    [IM_REM_M1] = {"remnant_M1", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_M2] = {"remnant_M2", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_M_TOT] = {"remnant_M_tot", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_R1_DISK_HALF] = {"remnant_R1_disk_half", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_R1_BULGE_HALF] = {"remnant_R1_bulge_half", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_M1_DISK] = {"remnant_M1_disk", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_M1_BULGE] = {"remnant_M1_bulge", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_R1_PROGENITOR] = {"remnant_R1_progenitor", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_R2_DISK_HALF] = {"remnant_R2_disk_half", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_R2_BULGE_HALF] = {"remnant_R2_bulge_half", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_M2_DISK] = {"remnant_M2_disk", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_M2_BULGE] = {"remnant_M2_bulge", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_R2_PROGENITOR] = {"remnant_R2_progenitor", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_R1] = {"remnant_R1", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_R2] = {"remnant_R2", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_E_INIT] = {"remnant_E_init", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_E_ORB] = {"remnant_E_orb", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_F_GAS] = {"remnant_f_gas", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_E_RAD] = {"remnant_E_rad", STAGE_MERGER, CHECK_TAIL},
    [IM_REM_E_FINAL] = {"remnant_E_final", STAGE_MERGER, CHECK_TAIL},
};

/** Carried integer values, validated by flipping them (README "Carried Values") */
enum carried_flag_id { CF_IS_DISK_DOMINATED, CF_BURST_TO_MERGER_BULGE, CF_COUNT };

static const struct {
  const char *name;
  enum check_kind check;
} CF_INFO[CF_COUNT] = {
    [CF_IS_DISK_DOMINATED] = {"is_disk_dominated", CHECK_TAIL},
    [CF_BURST_TO_MERGER_BULGE] = {"burst_to_merger_bulge", CHECK_CONTINUATION},
};

/** Branch flags: -1 not evaluated on this path, otherwise the branch taken */
enum flag_id {
  FL_CLOCK_SAME_HALO,
  FL_CLOCK_GUARD_PASSED,
  FL_CLOCK_CAPPED,
  FL_MASS_RATIO_PATH,
  FL_MAJOR_MERGER,
  FL_SATELLITE_DISK_POSITIVE,
  FL_DISK_FRACTION_ABOVE_HALF,
  FL_SATELLITE_ICS_POSITIVE,
  FL_BH_COLDGAS_POSITIVE,
  FL_BH_CAP_BINDS,
  FL_BH_METALS_FLOOR_BINDS,
  FL_WIND_EJECTS_COLD,
  FL_WIND_EJECTS_HOT,
  FL_BURST_GAS_FLOORED,
  FL_BURST_STARS_FLOORED,
  FL_BURST_RENORM_ACTIVE,
  FL_BURST_EJECTION_VVIR_NONPOSITIVE,
  FL_BURST_EJECTED_FLOORED,
  FL_REHEAT_CLAMP_FB_BINDS,
  FL_EJECTED_CAP_BINDS,
  FL_METALS_CAP_BINDS,
  FL_BURST_RECHECK_RAN,
  FL_BURST_RECHECK_CHANGED,
  FL_BURST_YIELD_STAYS_IN_DISK,
  FL_DISRUPT_STARS_POSITIVE,
  FL_DISRUPT_ICS_POSITIVE,
  /* c07: the branches of calculate_merger_remnant_radius (M:142-202), BulgeSizeOn 3 only */
  FL_REM_TOTAL_NONPOSITIVE,
  FL_REM_G1_MASS_POSITIVE,
  FL_REM_G2_MASS_POSITIVE,
  FL_REM_R1_FALLBACK,
  FL_REM_R2_FALLBACK,
  FL_REM_BOTH_RADII_ZERO,
  FL_REM_ENERGY_NONPOSITIVE,
  FL_COUNT
};

static const char *const FLAG_NAMES[FL_COUNT] = {
    [FL_CLOCK_SAME_HALO] = "clock_same_halo",
    [FL_CLOCK_GUARD_PASSED] = "clock_guard_passed",
    [FL_CLOCK_CAPPED] = "clock_capped",
    [FL_MASS_RATIO_PATH] = "mass_ratio_path",
    [FL_MAJOR_MERGER] = "major_merger",
    [FL_SATELLITE_DISK_POSITIVE] = "satellite_disk_positive",
    [FL_DISK_FRACTION_ABOVE_HALF] = "disk_fraction_above_half",
    [FL_SATELLITE_ICS_POSITIVE] = "satellite_ics_positive",
    [FL_BH_COLDGAS_POSITIVE] = "bh_coldgas_positive",
    [FL_BH_CAP_BINDS] = "bh_accretion_cap_binds",
    [FL_BH_METALS_FLOOR_BINDS] = "bh_metals_floor_binds",
    [FL_WIND_EJECTS_COLD] = "wind_ejects_cold",
    [FL_WIND_EJECTS_HOT] = "wind_ejects_hot",
    [FL_BURST_GAS_FLOORED] = "burst_gas_floored",
    [FL_BURST_STARS_FLOORED] = "burst_stars_floored",
    [FL_BURST_RENORM_ACTIVE] = "burst_renorm_active",
    [FL_BURST_EJECTION_VVIR_NONPOSITIVE] = "burst_ejection_vvir_nonpositive",
    [FL_BURST_EJECTED_FLOORED] = "burst_ejected_floored",
    [FL_REHEAT_CLAMP_FB_BINDS] = "reheat_clamp_fb_binds",
    [FL_EJECTED_CAP_BINDS] = "ejected_cap_binds",
    [FL_METALS_CAP_BINDS] = "metals_cap_binds",
    [FL_BURST_RECHECK_RAN] = "burst_instability_recheck_ran",
    [FL_BURST_RECHECK_CHANGED] = "burst_instability_recheck_changed_state",
    [FL_BURST_YIELD_STAYS_IN_DISK] = "burst_yield_minor_branch",
    [FL_DISRUPT_STARS_POSITIVE] = "disrupt_stars_positive",
    [FL_DISRUPT_ICS_POSITIVE] = "disrupt_ics_positive",
    [FL_REM_TOTAL_NONPOSITIVE] = "remnant_total_nonpositive",
    [FL_REM_G1_MASS_POSITIVE] = "remnant_g1_mass_positive",
    [FL_REM_G2_MASS_POSITIVE] = "remnant_g2_mass_positive",
    [FL_REM_R1_FALLBACK] = "remnant_r1_fallback",
    [FL_REM_R2_FALLBACK] = "remnant_r2_fallback",
    [FL_REM_BOTH_RADII_ZERO] = "remnant_both_radii_zero",
    [FL_REM_ENERGY_NONPOSITIVE] = "remnant_energy_nonpositive",
};

/** Values of intermediate.mass_ratio_path (M:243-249) */
enum mass_ratio_path { RATIO_DIVIDED = 0, RATIO_ONE = 1, RATIO_ZERO = 2 };

/**
 * The values SFprescription 1 adds (c08), recorded only for SFprescription 1 fixtures: the
 * black-hole accretion cap's bound (M:367-368), the burst feedback's post-reheating bound
 * (S:1170) and the burst's hydrogen refresh (M:799-802). Each is tail-checked (its routine
 * passes it to no SAGE26 function) and recorded with c06's markers and c07's narrowing
 * limitation, as the float or double it is.
 */
enum h_id {
  IMH_MAX_H_BH,         /* grow_black_hole's float max_h_bh (M:367-368) */
  IMH_MAX_H_FB,         /* the burst's update_from_feedback float max_h_fb (S:1170) */
  IMH_BURST_H2_BUDGET,  /* the double ColdGas * HYDROGEN_MASS_FRAC of the refresh (M:799-800) */
  IMH_BURST_H1_REFRESH, /* the fused double ColdGas * HYDROGEN_MASS_FRAC - H2gas (M:801-802) */
  IMH_COUNT
};

static const struct {
  const char *name;
  enum stage stage;
  int is_f32;            /* the local is a float */
  int narrowing_limited; /* 1: its only path to the state is one double-to-float narrowing */
} IMH_INFO[IMH_COUNT] = {
    [IMH_MAX_H_BH] = {"max_h_bh", STAGE_BH, 1, 0},
    [IMH_MAX_H_FB] = {"max_h_fb", STAGE_FEEDBACK, 1, 0},
    /* compared with H2gas and, when the cap binds, narrowed into it (M:799-800) */
    [IMH_BURST_H2_BUDGET] = {"burst_h2_budget", STAGE_BURST, 0, 1},
    /* narrowed into H1gas (M:801); the floor (M:803) reads the float */
    [IMH_BURST_H1_REFRESH] = {"burst_h1_refresh", STAGE_BURST, 0, 1},
};

/**
 * The values FIREmodeOn 1 adds (c09), recorded only by the *_fire routines that run the burst's
 * transcription: the locals of the burst's FIRE budget (M:690-752) and of the two header-inline
 * helpers it calls, capped_eta_reheat (R/src/model_misc.h:141-149) and sn_energy_coupling
 * (R/src/model_misc.h:118-124). Each is a double, recorded with c06's markers and c07's narrowing
 * limitation (0: a burst never writes MassLoading, so every value reaches the state only through
 * the reheated and ejected masses, or not at all on a zero-star burst).
 */
enum fire_id {
  IMF_REDSHIFT, /* z_fire = ZZ[galaxies[merger_centralgal].SnapNum] (M:694) */
  IMF_V_TERM,   /* the velocity term (M:698-700) */
  IMF_SCALING,  /* fire_scaling (M:692-703), passed to sn_energy_coupling (M:733) */
  IMF_ETA_RAW,  /* FeedbackReheatingEpsilon * fire_scaling, passed to capped_eta_reheat (M:709) */
  IMF_ETA_MAX,  /* capped_eta_reheat's eta_max (X:148) */
  IMF_ETA,      /* capped_eta_reheat's return value (M:708-710) */
  IMF_EPS_EFF,  /* sn_energy_coupling's eps_eff (X:120) */
  IMF_COUPLING, /* sn_energy_coupling's return value (M:733) */
  IMF_E_FB,     /* E_FB (M:733-734) */
  IMF_E_LIFT,   /* E_lift (M:735) */
  IMF_COUNT
};

static const struct {
  const char *name;
  enum stage stage;
  enum check_kind check;
} IMF_INFO[IMF_COUNT] = {
    [IMF_REDSHIFT] = {"fire_redshift", STAGE_BURST, CHECK_TAIL},
    [IMF_V_TERM] = {"fire_v_term", STAGE_BURST, CHECK_TAIL},
    [IMF_SCALING] = {"fire_scaling", STAGE_BURST, CHECK_CONTINUATION},
    [IMF_ETA_RAW] = {"eta_raw", STAGE_BURST, CHECK_CONTINUATION},
    [IMF_ETA_MAX] = {"eta_max", STAGE_FIRE_HELPER, CHECK_TAIL},
    [IMF_ETA] = {"eta", STAGE_BURST, CHECK_TAIL},
    [IMF_EPS_EFF] = {"eps_eff", STAGE_FIRE_HELPER, CHECK_TAIL},
    [IMF_COUPLING] = {"coupling", STAGE_BURST, CHECK_TAIL},
    [IMF_E_FB] = {"E_FB", STAGE_BURST, CHECK_TAIL},
    [IMF_E_LIFT] = {"E_lift", STAGE_BURST, CHECK_TAIL},
};

/** The branch flags FIREmodeOn 1 adds (c09): -1 not evaluated on this path, else 0 or 1 */
enum fire_flag_id {
  FLF_SCALING_VALID,            /* vc_fire > 0.0 && z_fire >= 0.0 (M:696) */
  FLF_VC_FLOORED,               /* vc_fire < 1.0 (M:697) */
  FLF_HIGH_VELOCITY,            /* vc_floored >= FIRE_V_CRIT_KMS: the slope -1.0 (M:698-700) */
  FLF_ETA_CAP_VVIR_NONPOSITIVE, /* vvir <= 0.0 in capped_eta_reheat (X:144) */
  FLF_ETA_CAP_BINDS,            /* eta_reheat > eta_max (X:149) */
  FLF_COUPLING_CAPPED,          /* eps_eff > MAX_SN_ENERGY_COUPLING (X:121) */
  FLF_ENERGY_EJECTS,            /* E_FB > E_lift (M:736) */
  FLF_COUNT
};

static const char *const FIRE_FLAG_NAMES[FLF_COUNT] = {
    [FLF_SCALING_VALID] = "fire_scaling_valid",
    [FLF_VC_FLOORED] = "fire_vc_floored",
    [FLF_HIGH_VELOCITY] = "fire_high_velocity",
    [FLF_ETA_CAP_VVIR_NONPOSITIVE] = "eta_cap_vvir_nonpositive",
    [FLF_ETA_CAP_BINDS] = "eta_cap_binds",
    [FLF_COUPLING_CAPPED] = "coupling_capped",
    [FLF_ENERGY_EJECTS] = "fire_energy_ejects",
};

/**
 * c09: OBSERVATIONS of the post-minor-merger recheck's own burst (M:807-811 into D:106-108), which
 * runs inside SAGE26's own check_disk_instability. On copies of the state the recheck receives,
 * SAGE26's own check_disk_instability is run again with one change to a copy of the run
 * parameters: FIREmodeOn 0 (mode_sensitive), or the redshift of the target's stored snapshot,
 * ZZ[galaxies[merger_centralgal].SnapNum], replaced by itself plus 1.0 (redshift_sensitive). Each
 * is 1 if the recheck's end state (the target's reservoirs and histories, the FoF central's
 * reservoirs) then differs from SAGE26's: within the merger family only the burst reads FIREmodeOn
 * and ZZ (M:693-694, :707, :730), so a difference shows that the recheck's burst took the FIRE
 * branch and read the target's stored-snapshot redshift. -1 where the recheck did not run or
 * FIREmodeOn is 0.
 */
struct recheck_fire_observation {
  int mode_sensitive;
  int redshift_sensitive;
};

/** The branch flags SFprescription 1 adds (c08): -1 not evaluated on this path, else 0 or 1 */
enum h_flag_id {
  FLH_BH_CAPS_TRACKED,      /* SFprescription != 0 && != 2 at M:366-367 */
  FLH_BH_H2_CAP_BINDS,      /* H2gas > max_h_bh (M:370) */
  FLH_BH_H1_CAP_BINDS,      /* H1gas > max_h_bh (M:371) */
  FLH_SF_COMMIT_H2_CAPPED,  /* OBSERVED: the burst's own update_from_star_formation (S:1133)
                               changed H2gas; SAGE26's routine, not a transcription */
  FLH_SF_COMMIT_H1_CAPPED,  /* OBSERVED: the same for H1gas (S:1134) */
  FLH_CAPS_FB_TRACKED,      /* sf_prescription_tracks_h2 at S:1169 */
  FLH_H2_CAP_FB_BINDS,      /* H2gas > max_h_fb (S:1171) */
  FLH_H1_CAP_FB_BINDS,      /* H1gas > max_h_fb (S:1172) */
  FLH_REFRESH_TRACKED,      /* sf_prescription_tracks_h2 at M:798 */
  FLH_REFRESH_H2_CAP_BINDS, /* H2gas > ColdGas * HYDROGEN_MASS_FRAC (M:799) */
  FLH_REFRESH_H1_FLOORED,   /* H1gas < 0.0 after the refresh (M:803) */
  FLH_COUNT
};

static const char *const H_FLAG_NAMES[FLH_COUNT] = {
    [FLH_BH_CAPS_TRACKED] = "bh_hydrogen_caps_tracked",
    [FLH_BH_H2_CAP_BINDS] = "bh_h2_cap_binds",
    [FLH_BH_H1_CAP_BINDS] = "bh_h1_cap_binds",
    [FLH_SF_COMMIT_H2_CAPPED] = "burst_sf_commit_h2_capped",
    [FLH_SF_COMMIT_H1_CAPPED] = "burst_sf_commit_h1_capped",
    [FLH_CAPS_FB_TRACKED] = "hydrogen_caps_fb_tracked",
    [FLH_H2_CAP_FB_BINDS] = "h2_cap_fb_binds",
    [FLH_H1_CAP_FB_BINDS] = "h1_cap_fb_binds",
    [FLH_REFRESH_TRACKED] = "burst_hydrogen_refresh_tracked",
    [FLH_REFRESH_H2_CAP_BINDS] = "burst_refresh_h2_cap_binds",
    [FLH_REFRESH_H1_FLOORED] = "burst_refresh_h1_floored",
};

/**
 * c08: an OBSERVATION of the post-minor-merger recheck's black-hole growth (M:807-811 into
 * D:102-104), which runs inside SAGE26's own check_disk_instability. The oracle restates the
 * recheck's unstable gas fraction (D:54-66, D:90, D:101, as oracle_c05.c's transcription does)
 * on a copy of the state the recheck receives and runs SAGE26's own grow_black_hole on that
 * copy; the copy's BlackHoleMass and QuasarModeBHaccretionMass must equal SAGE26's after its own
 * recheck (the recheck's burst writes neither), or the fixture is refused. The cap is
 * end-state invariant (the recheck's burst re-caps and refreshes the hydrogen), so this record
 * is its only evidence. grown is -1 where the recheck did not run, 0 where it ran without
 * growing the black hole.
 */
struct recheck_bh_observation {
  int grown;      /* the recheck reached its gas branch with AGNrecipeOn > 0 (D:90, D:102) */
  int h2_capped;  /* SAGE26's own growth changed H2gas (its strict cap, M:370) */
  int h1_capped;  /* the same for H1gas (M:371) */
  float h2_after; /* H2gas after that growth */
  float h1_after; /* H1gas after that growth */
};

/** The carried values, carried flags and branch flags of one composition */
struct locals {
  double value[IM_COUNT];
  int present[IM_COUNT];
  int cflag[CF_COUNT];
  int cflag_present[CF_COUNT];
  int flag[FL_COUNT];
  double h_value[IMH_COUNT]; /* c08: a float local widened exactly */
  int h_present[IMH_COUNT];
  int h_flag[FLH_COUNT];
  struct recheck_bh_observation recheck_bh; /* c08, SFprescription 1 only */
  double fire_value[IMF_COUNT];             /* c09 */
  int fire_present[IMF_COUNT];
  int fire_flag[FLF_COUNT];
  struct recheck_fire_observation recheck_fire; /* c09, FIREmodeOn 1 only */
  int out_of_domain; /* a SAGE26 assertion would abort on this path (perturbed runs only) */
};

static void init_locals(struct locals *l) {
  memset(l, 0, sizeof(*l));
  for (int f = 0; f < FL_COUNT; f++) {
    l->flag[f] = -1;
  }
  for (int f = 0; f < FLH_COUNT; f++) {
    l->h_flag[f] = -1;
  }
  l->recheck_bh.grown = -1;
  l->recheck_bh.h2_capped = -1;
  l->recheck_bh.h1_capped = -1;
  for (int f = 0; f < FLF_COUNT; f++) {
    l->fire_flag[f] = -1;
  }
  l->recheck_fire.mode_sensitive = -1;
  l->recheck_fire.redshift_sensitive = -1;
}

/** A replacement of one carried value by a neighbour, or of one carried flag by its negation */
struct perturbation {
  int im;   /* enum im_id to perturb, or -1 */
  int cf;   /* enum carried_flag_id to flip, or -1 */
  int up;   /* 1: toward +inf; 0: toward -inf */
  int h;    /* enum h_id to perturb (c08), or -1 */
  int fire; /* enum fire_id to perturb (c09), or -1 */
};

static const struct perturbation NO_PERTURBATION = {-1, -1, 0, -1, -1};

/** Which SAGE26 routines a composition replaces by their transcriptions */
struct composition {
  int add;          /* add_galaxies_together */
  int bh;           /* grow_black_hole */
  int wind;         /* quasar_mode_wind (inside grow_black_hole) */
  int burst;        /* collisional_starburst_recipe */
  int feedback;     /* update_from_feedback (inside the burst) */
  int fire_helpers; /* c09: capped_eta_reheat and sn_energy_coupling (inside the burst) */
};

static const struct composition OWN_ROUTINES = {0, 0, 0, 0, 0, 0};
static const struct composition ALL_TRANSCRIBED = {1, 1, 1, 1, 1, 1};

/** @brief The routines that must be transcribed for a value of the given stage to be reached */
static struct composition composition_for(const enum stage stage) {
  struct composition c = OWN_ROUTINES;
  c.add = stage == STAGE_ADD;
  c.bh = stage == STAGE_BH || stage == STAGE_WIND;
  c.wind = stage == STAGE_WIND;
  c.burst = stage == STAGE_BURST || stage == STAGE_FEEDBACK || stage == STAGE_FIRE_HELPER;
  c.feedback = stage == STAGE_FEEDBACK;
  c.fire_helpers = stage == STAGE_FIRE_HELPER;
  return c;
}

struct context {
  struct locals *l;
  const struct perturbation *pt;
  struct composition comp;
};

/** @brief Record a carried value, then return it, or its neighbour when it is perturbed */
static double tap(struct context *ctx, const enum im_id id, const double v) {
  ctx->l->value[id] = v;
  ctx->l->present[id] = 1;
  if (ctx->pt->im == (int)id) {
    return nextafter(v, ctx->pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

/** @brief Record a float c08 value, then return it, or its float neighbour when perturbed */
static float tap_h_f32(struct context *ctx, const enum h_id id, const float v) {
  ctx->l->h_value[id] = v;
  ctx->l->h_present[id] = 1;
  if (ctx->pt->h == (int)id) {
    return nextafterf(v, ctx->pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

/** @brief The double counterpart of tap_h_f32() */
static double tap_h_f64(struct context *ctx, const enum h_id id, const double v) {
  ctx->l->h_value[id] = v;
  ctx->l->h_present[id] = 1;
  if (ctx->pt->h == (int)id) {
    return nextafter(v, ctx->pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

/** @brief Record a c09 value, then return it, or its neighbour when it is perturbed */
static double tap_fire(struct context *ctx, const enum fire_id id, const double v) {
  ctx->l->fire_value[id] = v;
  ctx->l->fire_present[id] = 1;
  if (ctx->pt->fire == (int)id) {
    return nextafter(v, ctx->pt->up ? INFINITY : -INFINITY);
  }
  return v;
}

/** @brief Record a carried flag, then return it, or its negation when it is perturbed */
static int tap_flag(struct context *ctx, const enum carried_flag_id id, const int v) {
  ctx->l->cflag[id] = v;
  ctx->l->cflag_present[id] = 1;
  if (ctx->pt->cf == (int)id) {
    return !v;
  }
  return v;
}

// ============================================================================
// TRANSCRIPTIONS OF SAGE26 ROUTINES (checked against SAGE26's own end state)
// ============================================================================

static void call_quasar_mode_wind(struct context *ctx, const int gal, const double BHaccrete);
static void call_update_from_feedback(struct context *ctx, const int p, const int centralgal,
                                      const double reheated_mass, const double ejected_mass,
                                      const double metallicity);

/**
 * @brief TRANSCRIPTION of estimate_merging_time, M:89-121, returning the merger time
 *
 * SAGE26's own get_virial_mass, get_virial_radius and get_virial_velocity
 * (R/src/model_halo_properties.c:31-85) supply the halo quantities; the routine's own
 * statements are restated. The error message of the same-halo branch (M:95-96) prints only.
 */
static double transcribe_estimate_merging_time(struct context *ctx, const int sat_halo,
                                               const int mother_halo, const int ngal) {
  struct locals *l = ctx->l;
  double mergtime;
  const int MinNumPartSatHalo = 10; /* M:92 */

  /* M:94-98 */
  l->flag[FL_CLOCK_SAME_HALO] = 0;
  if (sat_halo == mother_halo) {
    l->flag[FL_CLOCK_SAME_HALO] = 1;
    return -1.0;
  }

  /* M:100 */
  double coulomb = log1p(halos[mother_halo].Len / ((double)halos[sat_halo].Len));
  coulomb = tap(ctx, IM_COULOMB, coulomb);

  /* M:102-103 */
  double sat_virial_mass = get_virial_mass(sat_halo, halos, &run_params);
  sat_virial_mass = tap(ctx, IM_SAT_VIRIAL_MASS, sat_virial_mass);
  double SatelliteMass = sat_virial_mass + galaxies[ngal].StellarMass + galaxies[ngal].ColdGas;
  SatelliteMass = tap(ctx, IM_SATELLITE_MASS, SatelliteMass);
  double SatelliteRadius = get_virial_radius(mother_halo, halos, &run_params);
  SatelliteRadius = tap(ctx, IM_SATELLITE_RADIUS, SatelliteRadius);

  /* M:105-110 */
  l->flag[FL_CLOCK_GUARD_PASSED] = 0;
  if (SatelliteMass > 0.0 && coulomb > 0.0 && halos[sat_halo].Len >= MinNumPartSatHalo) {
    l->flag[FL_CLOCK_GUARD_PASSED] = 1;
    double mother_virial_velocity = get_virial_velocity(mother_halo, halos, &run_params);
    mother_virial_velocity = tap(ctx, IM_MOTHER_VVIR, mother_virial_velocity);
    mergtime = run_params.MergerTimeFactor * 1.17 * SatelliteRadius * SatelliteRadius *
               mother_virial_velocity / (coulomb * run_params.G * SatelliteMass);
  } else {
    mergtime = -1.0;
  }
  mergtime = tap(ctx, IM_MERGTIME_PRE_CAP, mergtime);

  /* M:112-117 */
  l->flag[FL_CLOCK_CAPPED] = 0;
  if (mergtime >= 999.0) {
    mergtime = 998.0;
    l->flag[FL_CLOCK_CAPPED] = 1;
  }

  /* M:119 */
  return mergtime;
}

/**
 * @brief TRANSCRIPTION of quasar_mode_wind, M:390-442, on R0's path (CGMrecipeOn 0,
 *        M:430-441)
 */
static void transcribe_quasar_mode_wind(struct context *ctx, const int gal,
                                        const double BHaccrete) {
  struct locals *l = ctx->l;

  /* M:393-394 */
  double quasar_energy = run_params.QuasarModeEfficiency * 0.1 * BHaccrete *
                         (C / run_params.UnitVelocity_in_cm_per_s) *
                         (C / run_params.UnitVelocity_in_cm_per_s);
  quasar_energy = tap(ctx, IM_QUASAR_ENERGY, quasar_energy);
  double cold_gas_energy = 0.5 * galaxies[gal].ColdGas * galaxies[gal].Vvir * galaxies[gal].Vvir;
  cold_gas_energy = tap(ctx, IM_COLD_GAS_ENERGY, cold_gas_energy);

  /* M:397-403 */
  l->flag[FL_WIND_EJECTS_COLD] = 0;
  if (quasar_energy > cold_gas_energy) {
    l->flag[FL_WIND_EJECTS_COLD] = 1;
    galaxies[gal].EjectedMass += galaxies[gal].ColdGas;
    galaxies[gal].MetalsEjectedMass += galaxies[gal].MetalsColdGas;

    galaxies[gal].ColdGas = 0.0;
    galaxies[gal].MetalsColdGas = 0.0;
  }

  /* M:406 selects the non-CGM branch, M:430-441 */
  if (run_params.CGMrecipeOn == 1) {
    fatal("transcribe_quasar_mode_wind covers CGMrecipeOn 0 only");
  }
  double hot_gas_energy = 0.5 * galaxies[gal].HotGas * galaxies[gal].Vvir * galaxies[gal].Vvir;
  hot_gas_energy = tap(ctx, IM_HOT_GAS_ENERGY, hot_gas_energy);

  l->flag[FL_WIND_EJECTS_HOT] = 0;
  if (quasar_energy > cold_gas_energy + hot_gas_energy) {
    l->flag[FL_WIND_EJECTS_HOT] = 1;
    galaxies[gal].EjectedMass += galaxies[gal].HotGas;
    galaxies[gal].MetalsEjectedMass += galaxies[gal].MetalsHotGas;

    galaxies[gal].HotGas = 0.0;
    galaxies[gal].MetalsHotGas = 0.0;
  }
}

/**
 * @brief TRANSCRIPTION of grow_black_hole, M:346-378, on R0's and R1's path (SFprescription 0,
 *        so the hydrogen cap of M:366-372 does not run) and R2's (SFprescription 1, c08: the cap
 *        runs after the accretion and the negative-metal floor, before the wind)
 */
static void transcribe_grow_black_hole(struct context *ctx, const int merger_centralgal,
                                       const double mass_ratio) {
  struct locals *l = ctx->l;
  const int g = merger_centralgal;

  /* M:350 */
  l->flag[FL_BH_COLDGAS_POSITIVE] = 0;
  if (galaxies[g].ColdGas > 0.0) {
    l->flag[FL_BH_COLDGAS_POSITIVE] = 1;

    /* M:351-352 */
    double BHaccrete = run_params.BlackHoleGrowthRate * mass_ratio /
                       (1.0 + SQR(BH_GROWTH_V_KMS / galaxies[g].Vvir)) * galaxies[g].ColdGas;
    BHaccrete = tap(ctx, IM_BH_ACCRETE_PRE_CAP, BHaccrete);

    /* M:355-357 */
    l->flag[FL_BH_CAP_BINDS] = 0;
    if (BHaccrete > galaxies[g].ColdGas) {
      BHaccrete = galaxies[g].ColdGas;
      l->flag[FL_BH_CAP_BINDS] = 1;
    }
    BHaccrete = tap(ctx, IM_BH_ACCRETE, BHaccrete);

    /* M:359-362 */
    double metallicity = get_metallicity(galaxies[g].ColdGas, galaxies[g].MetalsColdGas);
    metallicity = tap(ctx, IM_BH_METALLICITY, metallicity);
    galaxies[g].BlackHoleMass += BHaccrete;
    galaxies[g].ColdGas -= BHaccrete;
    galaxies[g].MetalsColdGas -= metallicity * BHaccrete;

    /* M:363-365 */
    l->flag[FL_BH_METALS_FLOOR_BINDS] = 0;
    if (galaxies[g].MetalsColdGas < 0.0) {
      galaxies[g].MetalsColdGas = 0.0;
      l->flag[FL_BH_METALS_FLOOR_BINDS] = 1;
    }

    /* M:366-372: the guard as SAGE26 writes it at this site (not sf_prescription_tracks_h2) */
    const int sf_bh = run_params.SFprescription;
    l->h_flag[FLH_BH_CAPS_TRACKED] = sf_bh != 0 && sf_bh != 2;
    if (sf_bh != 0 && sf_bh != 2) {
      float max_h_bh =
          (galaxies[g].ColdGas > 0.0f) ? galaxies[g].ColdGas * HYDROGEN_MASS_FRAC : 0.0f;
      max_h_bh = tap_h_f32(ctx, IMH_MAX_H_BH, max_h_bh);
      l->h_flag[FLH_BH_H2_CAP_BINDS] = galaxies[g].H2gas > max_h_bh;
      if (galaxies[g].H2gas > max_h_bh)
        galaxies[g].H2gas = max_h_bh;
      l->h_flag[FLH_BH_H1_CAP_BINDS] = galaxies[g].H1gas > max_h_bh;
      if (galaxies[g].H1gas > max_h_bh)
        galaxies[g].H1gas = max_h_bh;
    }

    /* M:374 */
    call_quasar_mode_wind(ctx, g, BHaccrete);

    /* M:376 */
    galaxies[g].QuasarModeBHaccretionMass += BHaccrete;
  }
}

/**
 * @brief TRANSCRIPTION of update_from_feedback, R/src/model_starformation_and_feedback.c
 *        (S):1146-1250, on R0's path, as the burst calls it (M:791)
 *
 * oracle_c05.c's transcription, restated: SupernovaRecipeOn 1, CGMrecipeOn 0 ("Original SAGE
 * behavior", S:1224-1246) and SFprescription 0 or 1: for 1 (sf_prescription_tracks_h2), the
 * hydrogen cap of S:1169-1173 after the cold-gas and metal debits and before the hot-gas credit
 * (c08). Its XASSERTs (S:1158-1163) are checked: a violation marks the composition out of
 * domain, as SAGE26 would abort there.
 */
static void transcribe_update_from_feedback(struct context *ctx, const int p, const int centralgal,
                                            double reheated_mass, double ejected_mass,
                                            const double metallicity) {
  struct locals *l = ctx->l;

  /* S:1153-1156 */
  l->flag[FL_REHEAT_CLAMP_FB_BINDS] = 0;
  if (reheated_mass > galaxies[p].ColdGas) {
    reheated_mass = galaxies[p].ColdGas;
    l->flag[FL_REHEAT_CLAMP_FB_BINDS] = 1;
  }
  reheated_mass = tap(ctx, IM_REHEATED_FB_CLAMPED, reheated_mass);
  /* S:1158-1163 */
  if (!(reheated_mass >= 0.0) || !(reheated_mass <= galaxies[p].ColdGas)) {
    l->out_of_domain = 1;
    return;
  }

  /* S:1167-1168 */
  galaxies[p].ColdGas -= reheated_mass;
  galaxies[p].MetalsColdGas -= metallicity * reheated_mass;
  /* S:1169-1173 */
  l->h_flag[FLH_CAPS_FB_TRACKED] = sf_prescription_tracks_h2(run_params.SFprescription);
  if (sf_prescription_tracks_h2(run_params.SFprescription)) {
    float max_h_fb = (galaxies[p].ColdGas > 0.0f) ? galaxies[p].ColdGas * HYDROGEN_MASS_FRAC : 0.0f;
    max_h_fb = tap_h_f32(ctx, IMH_MAX_H_FB, max_h_fb);
    l->h_flag[FLH_H2_CAP_FB_BINDS] = galaxies[p].H2gas > max_h_fb;
    if (galaxies[p].H2gas > max_h_fb)
      galaxies[p].H2gas = max_h_fb;
    l->h_flag[FLH_H1_CAP_FB_BINDS] = galaxies[p].H1gas > max_h_fb;
    if (galaxies[p].H1gas > max_h_fb)
      galaxies[p].H1gas = max_h_fb;
  }

  /* S:1228-1229 */
  galaxies[centralgal].HotGas += reheated_mass;
  galaxies[centralgal].MetalsHotGas += metallicity * reheated_mass;

  /* S:1232-1234 */
  l->flag[FL_EJECTED_CAP_BINDS] = 0;
  if (ejected_mass > galaxies[centralgal].HotGas) {
    ejected_mass = galaxies[centralgal].HotGas;
    l->flag[FL_EJECTED_CAP_BINDS] = 1;
  }
  ejected_mass = tap(ctx, IM_EJECTED_CAPPED, ejected_mass);

  /* S:1235, S:1237-1240 */
  double metallicityHot =
      get_metallicity(galaxies[centralgal].HotGas, galaxies[centralgal].MetalsHotGas);
  metallicityHot = tap(ctx, IM_METALLICITY_HOT, metallicityHot);

  double metalsHot_to_eject = metallicityHot * ejected_mass;
  metalsHot_to_eject = tap(ctx, IM_METALS_TO_EJECT, metalsHot_to_eject);
  l->flag[FL_METALS_CAP_BINDS] = 0;
  if (metalsHot_to_eject > galaxies[centralgal].MetalsHotGas) {
    metalsHot_to_eject = galaxies[centralgal].MetalsHotGas;
    l->flag[FL_METALS_CAP_BINDS] = 1;
  }
  metalsHot_to_eject = tap(ctx, IM_METALS_TO_EJECT_CAPPED, metalsHot_to_eject);

  /* S:1243-1246 */
  galaxies[centralgal].HotGas -= ejected_mass;
  galaxies[centralgal].MetalsHotGas -= metalsHot_to_eject;
  galaxies[centralgal].EjectedMass += ejected_mass;
  galaxies[centralgal].MetalsEjectedMass += metalsHot_to_eject;

  /* S:1249 */
  galaxies[p].OutflowRate += reheated_mass;
}

/* SAGE26's TOOMRE_DISK_FACTOR (D:35), for the recheck observation */
#define C08_TOOMRE_DISK_FACTOR 3.0

/**
 * @brief c08: observe the recheck's black-hole growth on a copy of the state it receives
 *
 * The statements of D:54-66, D:90 and D:101 restated from SAGE26 (the stellar transfer of
 * D:73-84 writes no field grow_black_hole reads, so it is not repeated), then SAGE26's own
 * grow_black_hole (D:102-104) on the copy. Called before SAGE26's own recheck runs on the
 * oracle's galaxies; check_recheck_bh_observation() compares the copy with its result.
 */
static void observe_recheck_growth(struct locals *l, const int p, const struct GALAXY *state,
                                   struct GALAXY *observed) {
  memcpy(observed, state, MAX_GALAXIES * sizeof(observed[0]));
  l->recheck_bh.grown = 0;
  /* D:54 */
  const double diskmass = observed[p].ColdGas + (observed[p].StellarMass - observed[p].BulgeMass);
  if (!(diskmass > 0.0)) {
    return;
  }
  /* D:57-60 */
  double Mcrit = observed[p].Vmax * observed[p].Vmax *
                 (C08_TOOMRE_DISK_FACTOR * observed[p].DiskScaleRadius) / run_params.G;
  if (Mcrit > diskmass) {
    Mcrit = diskmass;
  }
  /* D:63-64 */
  const double gas_fraction = observed[p].ColdGas / diskmass;
  const double unstable_gas = gas_fraction * (diskmass - Mcrit);
  /* D:90, D:101-104 */
  if (unstable_gas > 0.0 && observed[p].ColdGas > 0.0 && run_params.AGNrecipeOn > 0) {
    const double unstable_gas_fraction = unstable_gas / observed[p].ColdGas;
    const float h2_before = observed[p].H2gas;
    const float h1_before = observed[p].H1gas;
    grow_black_hole(p, unstable_gas_fraction, observed, &run_params);
    l->recheck_bh.grown = 1;
    l->recheck_bh.h2_capped = memcmp(&h2_before, &observed[p].H2gas, sizeof(float)) != 0;
    l->recheck_bh.h1_capped = memcmp(&h1_before, &observed[p].H1gas, sizeof(float)) != 0;
    l->recheck_bh.h2_after = observed[p].H2gas;
    l->recheck_bh.h1_after = observed[p].H1gas;
  }
}

/**
 * @brief c08: refuse the fixture unless the observation's black hole is SAGE26's after its
 *        own recheck (BlackHoleMass and QuasarModeBHaccretionMass, bit for bit)
 */
static void check_recheck_bh_observation(const int p, const struct GALAXY *observed) {
  if (memcmp(&observed[p].BlackHoleMass, &galaxies[p].BlackHoleMass, sizeof(float)) != 0 ||
      memcmp(&observed[p].QuasarModeBHaccretionMass, &galaxies[p].QuasarModeBHaccretionMass,
             sizeof(float)) != 0) {
    fatal("recheck observation check failed: SAGE26's own grow_black_hole on the restated "
          "unstable gas fraction does not reproduce the black hole of SAGE26's recheck");
  }
}

/**
 * @brief c09: TRANSCRIPTION of capped_eta_reheat, R/src/model_misc.h:141-149 (header-inline in
 *        SAGE26; its own copy is what the burst calls unless the composition transcribes it)
 *
 *   if(vvir <= 0.0) return eta_reheat;
 *   const double esn_per_mass = run_params->EtaSNcode * run_params->EnergySNcode;
 *   const double eta_max = MAX_SN_ENERGY_COUPLING * esn_per_mass / (vvir * vvir);
 *   return (eta_reheat > eta_max) ? eta_max : eta_reheat;
 */
static double transcribe_capped_eta_reheat(struct context *ctx, const double eta_reheat,
                                           const double vvir) {
  struct locals *l = ctx->l;
  l->fire_flag[FLF_ETA_CAP_VVIR_NONPOSITIVE] = 0;
  if (vvir <= 0.0) { /* X:144-146 */
    l->fire_flag[FLF_ETA_CAP_VVIR_NONPOSITIVE] = 1;
    return eta_reheat;
  }
  const double esn_per_mass = run_params.EtaSNcode * run_params.EnergySNcode; /* X:147 */
  double eta_max = MAX_SN_ENERGY_COUPLING * esn_per_mass / (vvir * vvir);     /* X:148 */
  eta_max = tap_fire(ctx, IMF_ETA_MAX, eta_max);
  l->fire_flag[FLF_ETA_CAP_BINDS] = eta_reheat > eta_max;
  return (eta_reheat > eta_max) ? eta_max : eta_reheat; /* X:149 */
}

/**
 * @brief c09: TRANSCRIPTION of sn_energy_coupling, R/src/model_misc.h:118-124
 *
 *   const double eps_eff = run_params->FeedbackEjectionEfficiency * fire_scaling;
 *   if(eps_eff > MAX_SN_ENERGY_COUPLING) return MAX_SN_ENERGY_COUPLING;
 *   return eps_eff;
 */
static double transcribe_sn_energy_coupling(struct context *ctx, const double fire_scaling) {
  struct locals *l = ctx->l;
  double eps_eff = run_params.FeedbackEjectionEfficiency * fire_scaling; /* X:120 */
  eps_eff = tap_fire(ctx, IMF_EPS_EFF, eps_eff);
  l->fire_flag[FLF_COUPLING_CAPPED] = 0;
  if (eps_eff > MAX_SN_ENERGY_COUPLING) { /* X:121-123 */
    l->fire_flag[FLF_COUPLING_CAPPED] = 1;
    return MAX_SN_ENERGY_COUPLING;
  }
  return eps_eff;
}

/**
 * @brief c09: observe the recheck's own burst (struct recheck_fire_observation)
 *
 * before is the state the recheck received and after SAGE26's state after its own recheck; each
 * counterfactual runs SAGE26's own check_disk_instability on a copy of before, with a copy of the
 * run parameters changed in one member, and is compared with after on the watched blocks.
 */
static void observe_recheck_fire(struct locals *l, const int m, const int centralgal,
                                 const double time, const double dt, const int step,
                                 const struct GALAXY *before, const struct GALAXY *after,
                                 const struct record_spec *watched) {
  static struct params cf;
  struct GALAXY copy[MAX_GALAXIES];
  for (int which = 0; which < 2; which++) {
    memcpy(&cf, &run_params, sizeof(cf));
    if (which == 0) {
      cf.FIREmodeOn = 0;
    } else {
      cf.ZZ[before[m].SnapNum] = cf.ZZ[before[m].SnapNum] + 1.0;
    }
    memcpy(copy, before, sizeof(copy));
    check_disk_instability(m, centralgal, ORACLE_HALONR, time, dt, step, copy, &cf);
    const int changed = !same_recorded(copy, after, watched);
    if (which == 0) {
      l->recheck_fire.mode_sensitive = changed;
    } else {
      l->recheck_fire.redshift_sensitive = changed;
    }
  }
}

/**
 * @brief TRANSCRIPTION of collisional_starburst_recipe, M:584-832, on R0's to R3's paths
 *
 * StarburstColdGasOn 1 (M:680-682), SFprescription 0 or 1 (for 1, the hydrogen refresh of
 * M:798-803 after the feedback and before the instability recheck, c08; the star-formation
 * commit, M:760, is SAGE26's own update_from_star_formation, whose S:1131-1135 cap the oracle
 * observes), FIREmodeOn 0 (fire_scaling stays 0 and is never read, M:692-703) or, for the *_fire
 * routines, 1 (R3, c09: the FIRE budget M:690-752 with SAGE26's own or the transcribed
 * capped_eta_reheat and sn_energy_coupling, per the composition), SupernovaRecipeOn 1,
 * SaveFullSFH 0 (M:763-768 does not run),
 * BulgeSizeOn 0 or 3 (the instability channel's update_instability_bulge_radius, M:783-784, is
 * SAGE26's own: it returns at once in mode 0, R/src/model_misc.c:395) and
 * CGMrecipeOn 0 (add_metals_to_hot_reservoir adds to MetalsHotGas, model_misc.h:67-74).
 * SAGE26's assertions (M:589-592, M:718) are checked and mark the composition out of domain.
 * The instability recheck (M:807-811) is SAGE26's own check_disk_instability.
 */
static void transcribe_collisional_starburst_recipe(struct context *ctx, const double mass_ratio,
                                                    const int merger_centralgal,
                                                    const int centralgal, const double time,
                                                    const double dt, const int mode, const int step,
                                                    const int burst_to_merger_bulge,
                                                    const double old_disk_radius) {
  struct locals *l = ctx->l;
  const int m = merger_centralgal;

  /* M:589-592 */
  if (!(step >= 0 && step < STEPS) || !(dt > 0.0)) {
    l->out_of_domain = 1;
    return;
  }

  /* M:600-604 */
  double eburst;
  if (mode == 1) {
    eburst = mass_ratio;
  } else {
    eburst = STARBURST_FRAC_COEFF * pow(mass_ratio, STARBURST_MASS_POWER);
  }
  eburst = tap(ctx, IM_EBURST, eburst);

  /* M:606-683: StarburstColdGasOn 1, so M:681 */
  if (run_params.StarburstColdGasOn == 0) {
    fatal("transcribe_collisional_starburst_recipe covers StarburstColdGasOn 1 only");
  }
  double gas_for_starburst = galaxies[m].ColdGas;
  l->flag[FL_BURST_GAS_FLOORED] = 0;
  if (gas_for_starburst < 0.0) {
    gas_for_starburst = 0.0;
    l->flag[FL_BURST_GAS_FLOORED] = 1;
  }
  gas_for_starburst = tap(ctx, IM_GAS_FOR_STARBURST, gas_for_starburst);

  /* M:685-688 */
  double stars = eburst * gas_for_starburst;
  l->flag[FL_BURST_STARS_FLOORED] = 0;
  if (stars < 0.0) {
    stars = 0.0;
    l->flag[FL_BURST_STARS_FLOORED] = 1;
  }
  stars = tap(ctx, IM_STARS_PRE_RENORM, stars);

  /* M:690-703 (c09): the FIRE scaling from the TARGET's float Vvir widened to double and the
   * redshift of its stored snapshot, ZZ[SnapNum], read before the velocity test */
  double fire_scaling = 0.0;
  if (run_params.FIREmodeOn == 1 && run_params.SupernovaRecipeOn == 1) {
    double z_fire = run_params.ZZ[galaxies[m].SnapNum]; /* M:694 */
    z_fire = tap_fire(ctx, IMF_REDSHIFT, z_fire);
    const double vc_fire = galaxies[m].Vvir; /* M:695 */
    l->fire_flag[FLF_SCALING_VALID] = 0;
    if (vc_fire > 0.0 && z_fire >= 0.0) { /* M:696 */
      l->fire_flag[FLF_SCALING_VALID] = 1;
      const double vc_floored = (vc_fire < 1.0) ? 1.0 : vc_fire; /* M:697 */
      l->fire_flag[FLF_VC_FLOORED] = vc_fire < 1.0;
      l->fire_flag[FLF_HIGH_VELOCITY] = !(vc_floored < FIRE_V_CRIT_KMS);
      double v_term = (vc_floored < FIRE_V_CRIT_KMS) /* M:698-700 */
                          ? pow(vc_floored / FIRE_V_CRIT_KMS, -3.2)
                          : pow(vc_floored / FIRE_V_CRIT_KMS, -1.0);
      v_term = tap_fire(ctx, IMF_V_TERM, v_term);
      fire_scaling = pow(1.0 + z_fire, FIRE_REDSHIFT_EXPONENT) * v_term; /* M:701 */
    }
    fire_scaling = tap_fire(ctx, IMF_SCALING, fire_scaling);
  }

  /* M:706-716: SupernovaRecipeOn 1; FIREmodeOn 1 (c09) the capped loading times the stars, with
   * no MassLoading write (M:707-710), FIREmodeOn 0 the fixed epsilon (M:711-712) */
  double reheated_mass;
  if (run_params.FIREmodeOn == 1) {
    double eta_raw = run_params.FeedbackReheatingEpsilon * fire_scaling; /* M:709 */
    eta_raw = tap_fire(ctx, IMF_ETA_RAW, eta_raw);
    double eta = ctx->comp.fire_helpers
                     ? transcribe_capped_eta_reheat(ctx, eta_raw, galaxies[m].Vvir)
                     : capped_eta_reheat(eta_raw, galaxies[m].Vvir, &run_params); /* M:708-710 */
    eta = tap_fire(ctx, IMF_ETA, eta);
    reheated_mass = eta * stars;
  } else {
    reheated_mass = run_params.FeedbackReheatingEpsilon * stars;
  }
  reheated_mass = tap(ctx, IM_REHEATED_PRE_RENORM, reheated_mass);

  /* M:718 */
  if (!(reheated_mass >= 0.0)) {
    l->out_of_domain = 1;
    return;
  }

  /* M:721-725 */
  l->flag[FL_BURST_RENORM_ACTIVE] = 0;
  if ((stars + reheated_mass) > gas_for_starburst) {
    double fac = gas_for_starburst / (stars + reheated_mass);
    fac = tap(ctx, IM_RENORM_FAC, fac);
    stars *= fac;
    reheated_mass *= fac;
    l->flag[FL_BURST_RENORM_ACTIVE] = 1;
  }
  stars = tap(ctx, IM_STARS, stars);
  reheated_mass = tap(ctx, IM_REHEATED, reheated_mass);

  /* M:728-752: SupernovaRecipeOn 1; FIREmodeOn 1 (c09) the energy-based ejection (M:730-736),
   * FIREmodeOn 0 the fixed-epsilon formula (M:738-742) */
  double ejected_mass;
  l->flag[FL_BURST_EJECTION_VVIR_NONPOSITIVE] = 0;
  if (galaxies[m].Vvir > 0.0) {
    if (run_params.FIREmodeOn == 1) {
      const double vc = galaxies[m].Vvir; /* M:732 */
      double coupling = ctx->comp.fire_helpers
                            ? transcribe_sn_energy_coupling(ctx, fire_scaling)
                            : sn_energy_coupling(fire_scaling, &run_params); /* M:733 */
      coupling = tap_fire(ctx, IMF_COUPLING, coupling);
      double E_FB =
          coupling * 0.5 * stars * (run_params.EtaSNcode * run_params.EnergySNcode); /* M:733-734 */
      E_FB = tap_fire(ctx, IMF_E_FB, E_FB);
      double E_lift = 0.5 * reheated_mass * vc * vc; /* M:735 */
      E_lift = tap_fire(ctx, IMF_E_LIFT, E_lift);
      l->fire_flag[FLF_ENERGY_EJECTS] = E_FB > E_lift;
      ejected_mass = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0; /* M:736 */
    } else {
      ejected_mass = (run_params.FeedbackEjectionEfficiency *
                          (run_params.EtaSNcode * run_params.EnergySNcode) /
                          (galaxies[m].Vvir * galaxies[m].Vvir) -
                      run_params.FeedbackReheatingEpsilon) *
                     stars;
    }
    ejected_mass = tap(ctx, IM_EJECTED_PRE_FLOOR, ejected_mass);
  } else {
    ejected_mass = 0.0;
    l->flag[FL_BURST_EJECTION_VVIR_NONPOSITIVE] = 1;
  }
  l->flag[FL_BURST_EJECTED_FLOORED] = 0;
  if (ejected_mass < 0.0) {
    ejected_mass = 0.0;
    l->flag[FL_BURST_EJECTED_FLOORED] = 1;
  }
  ejected_mass = tap(ctx, IM_EJECTED, ejected_mass);

  /* M:755-757 */
  galaxies[m].SfrBulge[step] += stars / dt;
  galaxies[m].SfrBulgeColdGas[step] += galaxies[m].ColdGas;
  galaxies[m].SfrBulgeColdGasMetals[step] += galaxies[m].MetalsColdGas;

  /* M:759-760 */
  double metallicity = get_metallicity(galaxies[m].ColdGas, galaxies[m].MetalsColdGas);
  metallicity = tap(ctx, IM_METALLICITY_SF, metallicity);
  {
    /* c08: an OBSERVATION, not a transcription: whether SAGE26's own routine's hydrogen cap
     * (S:1131-1135) changed H2gas or H1gas. A binding strict cap always changes the field. */
    const float h2_before = galaxies[m].H2gas;
    const float h1_before = galaxies[m].H1gas;
    update_from_star_formation(m, stars, metallicity, galaxies, &run_params);
    if (sf_prescription_tracks_h2(run_params.SFprescription)) {
      l->h_flag[FLH_SF_COMMIT_H2_CAPPED] =
          memcmp(&h2_before, &galaxies[m].H2gas, sizeof(float)) != 0;
      l->h_flag[FLH_SF_COMMIT_H1_CAPPED] =
          memcmp(&h1_before, &galaxies[m].H1gas, sizeof(float)) != 0;
    }
  }

  /* M:763-768: SaveFullSFH 0 */
  if (run_params.SaveFullSFH) {
    fatal("transcribe_collisional_starburst_recipe covers SaveFullSFH 0 only");
  }

  /* M:770-773 */
  double recycled_stars = (1 - run_params.RecycleFraction) * stars;
  recycled_stars = tap(ctx, IM_RECYCLED_STARS, recycled_stars);

  galaxies[m].BulgeMass += recycled_stars;
  galaxies[m].MetalsBulgeMass += metallicity * recycled_stars;

  /* M:775-785 */
  if (burst_to_merger_bulge) {
    galaxies[m].MergerBulgeMass += recycled_stars;
  } else {
    galaxies[m].InstabilityBulgeMass += recycled_stars;
    update_instability_bulge_radius(m, recycled_stars, old_disk_radius, galaxies, &run_params);
  }

  /* M:788-791 */
  metallicity = get_metallicity(galaxies[m].ColdGas, galaxies[m].MetalsColdGas);
  metallicity = tap(ctx, IM_METALLICITY_FB, metallicity);
  call_update_from_feedback(ctx, m, centralgal, reheated_mass, ejected_mass, metallicity);
  if (l->out_of_domain) {
    return;
  }

  /* M:798-804 (c08): H2gas capped at the double hydrogen budget, then H1gas overwritten with
   * the fused remainder ColdGas * HYDROGEN_MASS_FRAC - H2gas (M:801-802, one fnmsub in SAGE26's
   * object; one C expression here), then a negative H1gas floored. The budget of M:799-800 is
   * formed once: the two identical expressions evaluate to the same double. */
  l->h_flag[FLH_REFRESH_TRACKED] = sf_prescription_tracks_h2(run_params.SFprescription);
  if (sf_prescription_tracks_h2(run_params.SFprescription)) {
    double h2_budget = galaxies[m].ColdGas * HYDROGEN_MASS_FRAC;
    h2_budget = tap_h_f64(ctx, IMH_BURST_H2_BUDGET, h2_budget);
    l->h_flag[FLH_REFRESH_H2_CAP_BINDS] = galaxies[m].H2gas > h2_budget;
    if (galaxies[m].H2gas > h2_budget)
      galaxies[m].H2gas = h2_budget;
    double h1_refresh = (galaxies[m].ColdGas * HYDROGEN_MASS_FRAC) - galaxies[m].H2gas;
    h1_refresh = tap_h_f64(ctx, IMH_BURST_H1_REFRESH, h1_refresh);
    galaxies[m].H1gas = h1_refresh;
    l->h_flag[FLH_REFRESH_H1_FLOORED] = galaxies[m].H1gas < 0.0;
    if (galaxies[m].H1gas < 0.0)
      galaxies[m].H1gas = 0.0;
  }

  /* M:807-811; the oracle also observes whether SAGE26's instability changed the host's or
   * the FoF central's reservoirs or histories (a gas- or star-unstable disk), not a transcription
   */
  l->flag[FL_BURST_RECHECK_RAN] = 0;
  if (run_params.DiskInstabilityOn && mode == 0) {
    if (mass_ratio < run_params.ThreshMajorMerger) {
      l->flag[FL_BURST_RECHECK_RAN] = 1;
      struct GALAXY before[MAX_GALAXIES];
      memcpy(before, galaxies, sizeof(before));
      /* c08: the recheck's black-hole growth observed on a copy (SFprescription 1 only) */
      struct GALAXY observed[MAX_GALAXIES];
      if (run_params.SFprescription == 1) {
        observe_recheck_growth(l, m, before, observed);
      }
      check_disk_instability(m, centralgal, ORACLE_HALONR, time, dt, step, galaxies, &run_params);
      if (run_params.SFprescription == 1) {
        check_recheck_bh_observation(m, observed);
      }
      struct record_spec watched = {MAX_GALAXIES, {0}};
      watched.blocks[m] = BLOCK_RESERVOIRS | BLOCK_HISTORIES;
      watched.blocks[centralgal] |= BLOCK_RESERVOIRS;
      l->flag[FL_BURST_RECHECK_CHANGED] = !same_recorded(before, galaxies, &watched);
      /* c09: the recheck's own burst observed under FIREmodeOn 0 and a replaced redshift */
      if (run_params.FIREmodeOn == 1) {
        observe_recheck_fire(l, m, centralgal, time, dt, step, before, galaxies, &watched);
      }
    }
  }

  /* M:814-831 */
  l->flag[FL_BURST_YIELD_STAYS_IN_DISK] = 0;
  if (galaxies[m].ColdGas > 1e-8 && mass_ratio < run_params.ThreshMajorMerger) {
    l->flag[FL_BURST_YIELD_STAYS_IN_DISK] = 1;
    double FracZleaveDiskVal =
        run_params.FracZleaveDisk * exp(-1.0 * galaxies[centralgal].Mvir / KD11_METAL_HALO_MASS);
    FracZleaveDiskVal = tap(ctx, IM_FRAC_Z_LEAVE, FracZleaveDiskVal);

    galaxies[m].MetalsColdGas += run_params.Yield * (1.0 - FracZleaveDiskVal) * stars;

    double metals_leaving_disk = run_params.Yield * FracZleaveDiskVal * stars;
    metals_leaving_disk = tap(ctx, IM_YIELD_TO_HOT, metals_leaving_disk);

    add_metals_to_hot_reservoir(&galaxies[centralgal], &run_params, metals_leaving_disk);
  } else {
    double all_metals = run_params.Yield * stars;
    all_metals = tap(ctx, IM_YIELD_TO_HOT, all_metals);

    add_metals_to_hot_reservoir(&galaxies[centralgal], &run_params, all_metals);
  }
}

/**
 * @brief TRANSCRIPTION of add_galaxies_together, M:456-543, on R0's and R1's paths (SaveFullSFH
 *        0, so M:537-542 does not run; BulgeSizeOn 0 or 3, the radius update of M:522 being
 *        SAGE26's own update_instability_bulge_radius, which returns at once in mode 0)
 */
static void transcribe_add_galaxies_together(struct context *ctx, const int t, const int p) {
  struct locals *l = ctx->l;

  /* M:458-468 */
  galaxies[t].ColdGas += galaxies[p].ColdGas;
  galaxies[t].MetalsColdGas += galaxies[p].MetalsColdGas;

  galaxies[t].StellarMass += galaxies[p].StellarMass;
  galaxies[t].MetalsStellarMass += galaxies[p].MetalsStellarMass;

  galaxies[t].HotGas += galaxies[p].HotGas;
  galaxies[t].MetalsHotGas += galaxies[p].MetalsHotGas;

  galaxies[t].EjectedMass += galaxies[p].EjectedMass;
  galaxies[t].MetalsEjectedMass += galaxies[p].MetalsEjectedMass;

  /* M:479-485 */
  l->flag[FL_SATELLITE_ICS_POSITIVE] = 0;
  if (galaxies[p].ICS > 0.0) {
    l->flag[FL_SATELLITE_ICS_POSITIVE] = 1;
    galaxies[t].ICS_accrete += galaxies[p].ICS;
    galaxies[t].ICS_sum_mt += galaxies[p].ICS_sum_mt;
  }

  /* M:487-488, M:492 */
  galaxies[t].ICS += galaxies[p].ICS;
  galaxies[t].MetalsICS += galaxies[p].MetalsICS;

  galaxies[p].ICS_disrupt = galaxies[p].ICS_accrete = galaxies[p].ICS_sum_mt = 0.0;

  /* M:494-505 */
  galaxies[t].BlackHoleMass += galaxies[p].BlackHoleMass;

  galaxies[t].CGMgas += galaxies[p].CGMgas;
  galaxies[t].MetalsCGMgas += galaxies[p].MetalsCGMgas;

  galaxies[t].BulgeMass += galaxies[p].StellarMass;
  galaxies[t].MetalsBulgeMass += galaxies[p].MetalsStellarMass;

  galaxies[t].InstabilityBulgeMass += galaxies[p].InstabilityBulgeMass;
  galaxies[t].MergerBulgeMass += galaxies[p].MergerBulgeMass;

  /* M:509-527 */
  double satellite_disk_mass = galaxies[p].StellarMass - galaxies[p].BulgeMass;
  satellite_disk_mass = tap(ctx, IM_SATELLITE_DISK_MASS, satellite_disk_mass);

  l->flag[FL_SATELLITE_DISK_POSITIVE] = 0;
  if (satellite_disk_mass > 0.0) {
    l->flag[FL_SATELLITE_DISK_POSITIVE] = 1;
    double disk_mass = galaxies[t].StellarMass - galaxies[t].BulgeMass;
    disk_mass = tap(ctx, IM_DISK_MASS_POST, disk_mass);
    double disk_fraction =
        (galaxies[t].StellarMass > 0.0) ? disk_mass / galaxies[t].StellarMass : 0.0;
    disk_fraction = tap(ctx, IM_DISK_FRACTION_POST, disk_fraction);

    l->flag[FL_DISK_FRACTION_ABOVE_HALF] = 0;
    if (disk_fraction > 0.5) {
      l->flag[FL_DISK_FRACTION_ABOVE_HALF] = 1;
      galaxies[t].InstabilityBulgeMass += satellite_disk_mass;
      const double old_disk_radius = galaxies[t].DiskScaleRadius;

      update_instability_bulge_radius(t, satellite_disk_mass, old_disk_radius, galaxies,
                                      &run_params);
    } else {
      galaxies[t].MergerBulgeMass += satellite_disk_mass;
    }
  }

  /* M:529-533 */
  for (int step = 0; step < STEPS; step++) {
    galaxies[t].SfrBulge[step] += galaxies[p].SfrDisk[step] + galaxies[p].SfrBulge[step];
    galaxies[t].SfrBulgeColdGas[step] +=
        galaxies[p].SfrDiskColdGas[step] + galaxies[p].SfrBulgeColdGas[step];
    galaxies[t].SfrBulgeColdGasMetals[step] +=
        galaxies[p].SfrDiskColdGasMetals[step] + galaxies[p].SfrBulgeColdGasMetals[step];
  }

  /* M:537-542: SaveFullSFH 0 */
  if (run_params.SaveFullSFH) {
    fatal("transcribe_add_galaxies_together covers SaveFullSFH 0 only");
  }
}

/**
 * @brief TRANSCRIPTION of calculate_merger_remnant_radius, M:135-212 (static in SAGE26)
 *
 * Every statement is restated with its line, every branch included: the M_tot <= 0 return
 * (M:142), each progenitor's positive and non-positive float stellar-plus-cold sum
 * (M:153-161, M:168-174), the zero-radius fallbacks in order (M:177-179) and the E_final <= 0
 * fallback (M:202-205). Each local is tapped (recorded, and replaced by a neighbour when it is
 * perturbed) right after the statement that forms it; R1 and R2 are tapped as M:153-174 form
 * them (remnant_R1_progenitor, remnant_R2_progenitor) and again after the fallbacks
 * (remnant_R1, remnant_R2). C_rad (M:193) is the constant COVINGTON11_C_RAD and is not a
 * carried value.
 *
 * The routine is static, so no SAGE26 call reaches it alone: the transcription is checked
 * through SAGE26's own deal_with_galaxy_merger, whose end state it must reproduce (tail check).
 * With BulgeSizeOn 0 every path of deal_with_galaxy_merger that stores the result in
 * MergerBulgeRadius then calls get_bulge_radius, which zeroes the radii
 * (R/src/model_misc.c:242-247), so the check covers nothing there. With BulgeSizeOn 3 the
 * result reaches the state on the major and the spheroid-dominated minor branch (M:307, :328),
 * by one narrowing into the float MergerBulgeRadius; the disk-dominated minor branch discards
 * it (M:318-325). The three fused multiply-adds of SAGE26's object (M:158, :171, :204,
 * README's c07 table) are the same single C expressions here, so clang contracts them alike.
 */
static double transcribe_merger_remnant_radius(struct context *ctx, const struct GALAXY *g1,
                                               const struct GALAXY *g2) {
  struct locals *l = ctx->l;

  /* M:138-140 */
  double M1 = g1->StellarMass + g1->ColdGas;
  M1 = tap(ctx, IM_REM_M1, M1);
  double M2 = g2->StellarMass + g2->ColdGas;
  M2 = tap(ctx, IM_REM_M2, M2);
  double M_tot = M1 + M2;
  M_tot = tap(ctx, IM_REM_M_TOT, M_tot);

  /* M:142 */
  l->flag[FL_REM_TOTAL_NONPOSITIVE] = 0;
  if (M_tot <= 0.0) {
    l->flag[FL_REM_TOTAL_NONPOSITIVE] = 1;
    return 0.0;
  }

  /* M:149-151 */
  double R1_disk_half = DISK_HALF_MASS_FRAC * g1->DiskScaleRadius;
  R1_disk_half = tap(ctx, IM_REM_R1_DISK_HALF, R1_disk_half);
  double R1_bulge_half = g1->BulgeRadius;
  R1_bulge_half = tap(ctx, IM_REM_R1_BULGE_HALF, R1_bulge_half);
  double R1;

  /* M:153-161 */
  l->flag[FL_REM_G1_MASS_POSITIVE] = 0;
  if (g1->StellarMass + g1->ColdGas > 0) {
    l->flag[FL_REM_G1_MASS_POSITIVE] = 1;
    double M1_disk = g1->ColdGas + (g1->StellarMass - g1->BulgeMass);
    M1_disk = tap(ctx, IM_REM_M1_DISK, M1_disk);
    double M1_bulge = g1->BulgeMass;
    M1_bulge = tap(ctx, IM_REM_M1_BULGE, M1_bulge);
    R1 = (M1_disk * R1_disk_half + M1_bulge * R1_bulge_half) / M1;
  } else {
    R1 = 0.0;
  }
  R1 = tap(ctx, IM_REM_R1_PROGENITOR, R1);

  /* M:164-174 */
  double R2_disk_half = DISK_HALF_MASS_FRAC * g2->DiskScaleRadius;
  R2_disk_half = tap(ctx, IM_REM_R2_DISK_HALF, R2_disk_half);
  double R2_bulge_half = g2->BulgeRadius;
  R2_bulge_half = tap(ctx, IM_REM_R2_BULGE_HALF, R2_bulge_half);
  double R2;

  l->flag[FL_REM_G2_MASS_POSITIVE] = 0;
  if (g2->StellarMass + g2->ColdGas > 0) {
    l->flag[FL_REM_G2_MASS_POSITIVE] = 1;
    double M2_disk = g2->ColdGas + (g2->StellarMass - g2->BulgeMass);
    M2_disk = tap(ctx, IM_REM_M2_DISK, M2_disk);
    double M2_bulge = g2->BulgeMass;
    M2_bulge = tap(ctx, IM_REM_M2_BULGE, M2_bulge);
    R2 = (M2_disk * R2_disk_half + M2_bulge * R2_bulge_half) / M2;
  } else {
    R2 = 0.0;
  }
  R2 = tap(ctx, IM_REM_R2_PROGENITOR, R2);

  /* M:177-179 */
  l->flag[FL_REM_R1_FALLBACK] = 0;
  if (R1 <= 0.0) {
    l->flag[FL_REM_R1_FALLBACK] = 1;
    R1 = R2;
  }
  l->flag[FL_REM_R2_FALLBACK] = 0;
  if (R2 <= 0.0) {
    l->flag[FL_REM_R2_FALLBACK] = 1;
    R2 = R1;
  }
  R1 = tap(ctx, IM_REM_R1, R1);
  R2 = tap(ctx, IM_REM_R2, R2);
  l->flag[FL_REM_BOTH_RADII_ZERO] = 0;
  if (R1 <= 0.0) {
    l->flag[FL_REM_BOTH_RADII_ZERO] = 1;
    return 0.0;
  }

  /* M:185 */
  double E_init = (M1 * M1) / R1 + (M2 * M2) / R2;
  E_init = tap(ctx, IM_REM_E_INIT, E_init);

  /* M:189 */
  double E_orb = (M1 * M2) / (R1 + R2);
  E_orb = tap(ctx, IM_REM_E_ORB, E_orb);

  /* M:193-195 */
  double C_rad = COVINGTON11_C_RAD;
  double f_gas = (g1->ColdGas + g2->ColdGas) / M_tot;
  f_gas = tap(ctx, IM_REM_F_GAS, f_gas);
  double E_rad = C_rad * E_init * f_gas;
  E_rad = tap(ctx, IM_REM_E_RAD, E_rad);

  /* M:199 */
  double E_final = E_init + E_orb + E_rad;
  E_final = tap(ctx, IM_REM_E_FINAL, E_final);

  /* M:202-205 */
  l->flag[FL_REM_ENERGY_NONPOSITIVE] = 0;
  if (E_final <= 0.0) {
    l->flag[FL_REM_ENERGY_NONPOSITIVE] = 1;
    return (M1 * R1 + M2 * R2) / M_tot;
  }

  /* M:209-211 */
  double R_final = (M_tot * M_tot) / E_final;

  return R_final;
}

/** @brief SAGE26's own or the transcribed add_galaxies_together, per the composition */
static void call_add_galaxies_together(struct context *ctx, const int t, const int p) {
  if (ctx->comp.add) {
    transcribe_add_galaxies_together(ctx, t, p);
  } else {
    add_galaxies_together(t, p, galaxies, &run_params);
  }
}

static void call_grow_black_hole(struct context *ctx, const int g, const double mass_ratio) {
  if (ctx->comp.bh) {
    transcribe_grow_black_hole(ctx, g, mass_ratio);
  } else {
    grow_black_hole(g, mass_ratio, galaxies, &run_params);
  }
}

static void call_quasar_mode_wind(struct context *ctx, const int gal, const double BHaccrete) {
  if (ctx->comp.wind) {
    transcribe_quasar_mode_wind(ctx, gal, BHaccrete);
  } else {
    quasar_mode_wind(gal, BHaccrete, galaxies, &run_params);
  }
}

/**
 * @brief Whether SAGE26's own collisional_starburst_recipe would pass its assertions
 *        (M:589-592, M:718) on these arguments: a perturbed mass ratio below zero makes the
 *        mode-0 pow() NaN, and a NaN reheated mass aborts SAGE26
 */
static int burst_in_domain(const double mass_ratio, const int m, const double dt, const int mode,
                           const int step) {
  if (!(step >= 0 && step < STEPS) || !(dt > 0.0)) {
    return 0;
  }
  const double eburst =
      mode == 1 ? mass_ratio : STARBURST_FRAC_COEFF * pow(mass_ratio, STARBURST_MASS_POWER);
  double gas = galaxies[m].ColdGas;
  if (gas < 0.0) {
    gas = 0.0;
  }
  double stars = eburst * gas;
  if (stars < 0.0) {
    stars = 0.0;
  }
  return run_params.FeedbackReheatingEpsilon * stars >= 0.0;
}

static void call_collisional_starburst_recipe(struct context *ctx, const double mass_ratio,
                                              const int merger_centralgal, const int centralgal,
                                              const double time, const double dt, const int mode,
                                              const int step, const int burst_to_merger_bulge,
                                              const double old_disk_radius) {
  if (ctx->comp.burst) {
    transcribe_collisional_starburst_recipe(ctx, mass_ratio, merger_centralgal, centralgal, time,
                                            dt, mode, step, burst_to_merger_bulge, old_disk_radius);
  } else if (!burst_in_domain(mass_ratio, merger_centralgal, dt, mode, step)) {
    ctx->l->out_of_domain = 1;
  } else {
    collisional_starburst_recipe(mass_ratio, merger_centralgal, centralgal, time, dt, ORACLE_HALONR,
                                 mode, step, burst_to_merger_bulge, old_disk_radius, galaxies,
                                 &run_params);
  }
}

/** @brief SAGE26's own update_from_feedback after checking its assertions (S:1158-1163) */
static void call_update_from_feedback(struct context *ctx, const int p, const int centralgal,
                                      const double reheated_mass, const double ejected_mass,
                                      const double metallicity) {
  if (ctx->comp.feedback) {
    transcribe_update_from_feedback(ctx, p, centralgal, reheated_mass, ejected_mass, metallicity);
    return;
  }
  double clamped = reheated_mass;
  if (clamped > galaxies[p].ColdGas) {
    clamped = galaxies[p].ColdGas;
  }
  if (!(clamped >= 0.0 && clamped <= galaxies[p].ColdGas)) {
    ctx->l->out_of_domain = 1;
    return;
  }
  update_from_feedback(p, centralgal, reheated_mass, ejected_mass, metallicity, galaxies,
                       &run_params);
}

/**
 * @brief TRANSCRIPTION of deal_with_galaxy_merger, M:228-333, on R0's and R1's paths
 *        (BulgeSizeOn 0 or 3)
 *
 * The sub-routines it calls are SAGE26's own or their transcriptions above, per the
 * composition; get_bulge_radius and make_bulge_from_burst are always SAGE26's own;
 * calculate_merger_remnant_radius is static in SAGE26 and always transcribed.
 */
static void transcribe_deal_with_galaxy_merger(struct context *ctx, const int p,
                                               const int merger_centralgal, const int centralgal,
                                               const double time, const double dt, const int step) {
  struct locals *l = ctx->l;
  const int t = merger_centralgal;
  double mi, ma, mass_ratio;

  /* M:235-241 */
  if (galaxies[p].StellarMass + galaxies[p].ColdGas <
      galaxies[t].StellarMass + galaxies[t].ColdGas) {
    mi = galaxies[p].StellarMass + galaxies[p].ColdGas;
    ma = galaxies[t].StellarMass + galaxies[t].ColdGas;
  } else {
    mi = galaxies[t].StellarMass + galaxies[t].ColdGas;
    ma = galaxies[p].StellarMass + galaxies[p].ColdGas;
  }
  mi = tap(ctx, IM_MI, mi);
  ma = tap(ctx, IM_MA, ma);

  /* M:243-249 */
  if (ma > 0) {
    mass_ratio = mi / ma;
    l->flag[FL_MASS_RATIO_PATH] = RATIO_DIVIDED;
  } else if (mi > 0) {
    mass_ratio = 1.0;
    l->flag[FL_MASS_RATIO_PATH] = RATIO_ONE;
  } else {
    mass_ratio = 0.0;
    l->flag[FL_MASS_RATIO_PATH] = RATIO_ZERO;
  }
  mass_ratio = tap(ctx, IM_MASS_RATIO, mass_ratio);

  /* M:253-257 */
  double central_disk_mass = galaxies[t].StellarMass - galaxies[t].BulgeMass;
  central_disk_mass = tap(ctx, IM_CENTRAL_DISK_MASS, central_disk_mass);
  int is_disk_dominated = (central_disk_mass > 0.5 * galaxies[t].StellarMass);
  is_disk_dominated = tap_flag(ctx, CF_IS_DISK_DOMINATED, is_disk_dominated);

  double old_disk_radius = galaxies[t].DiskScaleRadius;
  old_disk_radius = tap(ctx, IM_OLD_DISK_RADIUS, old_disk_radius);

  /* M:259 */
  call_add_galaxies_together(ctx, t, p);

  /* M:262-264 */
  if (run_params.AGNrecipeOn) {
    call_grow_black_hole(ctx, t, mass_ratio);
  }

  /* M:268-282 */
  int burst_to_merger_bulge = 0;
  if (mass_ratio > run_params.ThreshMajorMerger) {
    burst_to_merger_bulge = 1;
  } else {
    if (is_disk_dominated) {
      burst_to_merger_bulge = 0;
    } else {
      burst_to_merger_bulge = 1;
    }
  }
  burst_to_merger_bulge = tap_flag(ctx, CF_BURST_TO_MERGER_BULGE, burst_to_merger_bulge);

  /* M:285-287 */
  call_collisional_starburst_recipe(ctx, mass_ratio, t, centralgal, time, dt, 0, step,
                                    burst_to_merger_bulge, old_disk_radius);
  if (l->out_of_domain) {
    return;
  }

  /* M:293, M:297 */
  get_bulge_radius(t, galaxies, &run_params);
  double new_merger_radius = transcribe_merger_remnant_radius(ctx, &galaxies[t], &galaxies[p]);
  new_merger_radius = tap(ctx, IM_NEW_MERGER_RADIUS, new_merger_radius);

  /* M:299-332 */
  l->flag[FL_MAJOR_MERGER] = 0;
  if (mass_ratio > run_params.ThreshMajorMerger) {
    l->flag[FL_MAJOR_MERGER] = 1;
    make_bulge_from_burst(t, galaxies);

    galaxies[t].MergerBulgeRadius = new_merger_radius;
    get_bulge_radius(t, galaxies, &run_params);

    galaxies[t].TimeOfLastMajorMerger = time;
    galaxies[p].mergeType = 2;
  } else {
    galaxies[p].mergeType = 1;
    galaxies[t].TimeOfLastMinorMerger = time;

    if (is_disk_dominated) {
      get_bulge_radius(t, galaxies, &run_params);
    } else {
      galaxies[t].MergerBulgeRadius = new_merger_radius;
      get_bulge_radius(t, galaxies, &run_params);
    }
  }
}

/**
 * @brief TRANSCRIPTION of disrupt_satellite_to_ICS, M:845-903, on R0's path (CGMrecipeOn 0:
 *        add_gas_to_hot_reservoir adds to HotGas, R/src/model_misc.h:76-85)
 */
static void transcribe_disrupt_satellite_to_ICS(struct context *ctx, const int centralgal,
                                                const int gal, const double time) {
  struct locals *l = ctx->l;

  /* M:848-851 */
  double total_gas = galaxies[gal].ColdGas + galaxies[gal].HotGas + galaxies[gal].CGMgas;
  total_gas = tap(ctx, IM_TOTAL_GAS, total_gas);
  double total_metals_gas =
      galaxies[gal].MetalsColdGas + galaxies[gal].MetalsHotGas + galaxies[gal].MetalsCGMgas;
  total_metals_gas = tap(ctx, IM_TOTAL_METALS_GAS, total_metals_gas);

  add_gas_to_hot_reservoir(&galaxies[centralgal], &run_params, total_gas, total_metals_gas);

  /* M:854-863 */
  galaxies[centralgal].EjectedMass += galaxies[gal].EjectedMass;
  galaxies[centralgal].MetalsEjectedMass += galaxies[gal].MetalsEjectedMass;

  galaxies[centralgal].ICS += galaxies[gal].ICS;
  galaxies[centralgal].MetalsICS += galaxies[gal].MetalsICS;

  galaxies[centralgal].ICS += galaxies[gal].StellarMass;
  galaxies[centralgal].MetalsICS += galaxies[gal].MetalsStellarMass;

  /* M:869-872 */
  l->flag[FL_DISRUPT_STARS_POSITIVE] = 0;
  if (galaxies[gal].StellarMass > 0.0) {
    l->flag[FL_DISRUPT_STARS_POSITIVE] = 1;
    galaxies[centralgal].ICS_disrupt += galaxies[gal].StellarMass;
    galaxies[centralgal].ICS_sum_mt += galaxies[gal].StellarMass * time;
  }

  /* M:875 */
  galaxies[centralgal].BlackHoleMass += galaxies[gal].BlackHoleMass;

  /* M:881-887 */
  l->flag[FL_DISRUPT_ICS_POSITIVE] = 0;
  if (galaxies[gal].ICS > 0.0) {
    l->flag[FL_DISRUPT_ICS_POSITIVE] = 1;
    galaxies[centralgal].ICS_accrete += galaxies[gal].ICS;
    galaxies[centralgal].ICS_sum_mt += galaxies[gal].ICS_sum_mt;
  }

  /* M:891-902 */
  galaxies[gal].ColdGas = galaxies[gal].MetalsColdGas = 0.0f;
  galaxies[gal].HotGas = galaxies[gal].MetalsHotGas = 0.0f;
  galaxies[gal].CGMgas = galaxies[gal].MetalsCGMgas = 0.0f;
  galaxies[gal].EjectedMass = galaxies[gal].MetalsEjectedMass = 0.0f;
  galaxies[gal].ICS = galaxies[gal].MetalsICS = 0.0f;
  galaxies[gal].ICS_disrupt = galaxies[gal].ICS_accrete = 0.0f;
  galaxies[gal].ICS_sum_mt = 0.0f;
  galaxies[gal].StellarMass = galaxies[gal].MetalsStellarMass = 0.0f;
  galaxies[gal].BulgeMass = galaxies[gal].MetalsBulgeMass = 0.0f;
  galaxies[gal].BlackHoleMass = 0.0f;

  galaxies[gal].mergeType = 4;
}

// ============================================================================
// CHECKED COMPOSITIONS
// ============================================================================

/** The SAGE26 routines a fixture can select whose intermediates are checked */
enum checked_routine {
  CR_ESTIMATE_MERGING_TIME,
  CR_GROW_BLACK_HOLE,
  CR_QUASAR_MODE_WIND,
  CR_COLLISIONAL_STARBURST,
  CR_ADD_GALAXIES_TOGETHER,
  CR_DEAL_WITH_GALAXY_MERGER,
  CR_DISRUPT_SATELLITE_TO_ICS
};

/** The arguments of one call of a checked routine */
struct call_args {
  enum checked_routine routine;
  int ngal;
  int p;          /* satellite (merger, add, disrupt), or the galaxy (BH, wind) */
  int target;     /* merger_centralgal / t */
  int centralgal; /* FoF central */
  double time;
  double dt;
  int step;
  int mode;
  int burst_to_merger_bulge;
  double old_disk_radius;
  double mass_ratio; /* BH, burst */
  double bh_accrete; /* wind */
  int sat_halo;      /* clock */
  int mother_halo;   /* clock */
};

/** @brief SAGE26's own routine on the current galaxies; returns the clock's value */
static double run_sage26(const struct call_args *a) {
  switch (a->routine) {
  case CR_ESTIMATE_MERGING_TIME:
    return estimate_merging_time(a->sat_halo, a->mother_halo, a->p, halos, galaxies, &run_params);
  case CR_GROW_BLACK_HOLE:
    grow_black_hole(a->p, a->mass_ratio, galaxies, &run_params);
    break;
  case CR_QUASAR_MODE_WIND:
    quasar_mode_wind(a->p, a->bh_accrete, galaxies, &run_params);
    break;
  case CR_COLLISIONAL_STARBURST:
    collisional_starburst_recipe(a->mass_ratio, a->target, a->centralgal, a->time, a->dt,
                                 ORACLE_HALONR, a->mode, a->step, a->burst_to_merger_bulge,
                                 a->old_disk_radius, galaxies, &run_params);
    break;
  case CR_ADD_GALAXIES_TOGETHER:
    add_galaxies_together(a->target, a->p, galaxies, &run_params);
    break;
  case CR_DEAL_WITH_GALAXY_MERGER:
    deal_with_galaxy_merger(a->p, a->target, a->centralgal, a->time, a->dt, ORACLE_HALONR, a->step,
                            galaxies, &run_params);
    break;
  case CR_DISRUPT_SATELLITE_TO_ICS:
    disrupt_satellite_to_ICS(a->centralgal, a->p, a->time, galaxies, &run_params);
    break;
  }
  return 0.0;
}

/**
 * @brief The routine's transcription composed per comp, on a copy of the input; returns the
 *        clock's value (0 for the other routines) and leaves the end state in result
 */
static double compose(const struct GALAXY *input, const struct call_args *a,
                      const struct composition *comp, const struct perturbation *pt,
                      struct locals *l, struct GALAXY *result) {
  memcpy(galaxies, input, (size_t)a->ngal * sizeof(galaxies[0]));
  init_locals(l);
  struct context ctx = {l, pt, *comp};
  double value = 0.0;
  switch (a->routine) {
  case CR_ESTIMATE_MERGING_TIME:
    value = transcribe_estimate_merging_time(&ctx, a->sat_halo, a->mother_halo, a->p);
    break;
  case CR_GROW_BLACK_HOLE:
    ctx.comp.bh = 1;
    transcribe_grow_black_hole(&ctx, a->p, a->mass_ratio);
    break;
  case CR_QUASAR_MODE_WIND:
    ctx.comp.wind = 1;
    transcribe_quasar_mode_wind(&ctx, a->p, a->bh_accrete);
    break;
  case CR_COLLISIONAL_STARBURST:
    ctx.comp.burst = 1;
    transcribe_collisional_starburst_recipe(&ctx, a->mass_ratio, a->target, a->centralgal, a->time,
                                            a->dt, a->mode, a->step, a->burst_to_merger_bulge,
                                            a->old_disk_radius);
    break;
  case CR_ADD_GALAXIES_TOGETHER:
    ctx.comp.add = 1;
    transcribe_add_galaxies_together(&ctx, a->target, a->p);
    break;
  case CR_DEAL_WITH_GALAXY_MERGER:
    transcribe_deal_with_galaxy_merger(&ctx, a->p, a->target, a->centralgal, a->time, a->dt,
                                       a->step);
    break;
  case CR_DISRUPT_SATELLITE_TO_ICS:
    transcribe_disrupt_satellite_to_ICS(&ctx, a->centralgal, a->p, a->time);
    break;
  }
  memcpy(result, galaxies, (size_t)a->ngal * sizeof(galaxies[0]));
  return value;
}

/** @brief Whether a perturbed (or unperturbed) composition reproduces SAGE26's end state */
static int composition_matches(const struct GALAXY *result, const double value,
                               const struct locals *l, const struct GALAXY *end,
                               const double end_value, const struct record_spec *spec) {
  return !l->out_of_domain && same_recorded(result, end, spec) && same_f64(value, end_value);
}

/** @brief Whether a replacement changes the end state, in each direction */
struct sensitivity {
  int up;
  int down;
};

/** @brief One checked call: SAGE26's end state, the carried values and their sensitivity */
struct checked_record {
  struct GALAXY end[MAX_GALAXIES];
  double end_value;
  struct locals locals;
  struct sensitivity sens[IM_COUNT];
  int cflag_changes[CF_COUNT];
  struct sensitivity h_sens[IMH_COUNT];    /* c08 */
  struct sensitivity fire_sens[IMF_COUNT]; /* c09 */
};

/**
 * @brief Call SAGE26's routine, check every composition used, validate every carried value
 *
 * The oracle refuses the fixture unless the fully transcribed composition, the composition
 * with SAGE26's own sub-routines, and every composition a two-sided test uses (the value's
 * routine and the routines that call it transcribed, every other one SAGE26's own) each
 * reproduce SAGE26's end state in every recorded field bit for bit (and, for the clock, its
 * return value). The recorded carried values come from the fully transcribed composition.
 */
static void run_checked(const struct GALAXY *input, const struct call_args *a,
                        const struct record_spec *spec, struct checked_record *rec) {
  memcpy(galaxies, input, (size_t)a->ngal * sizeof(galaxies[0]));
  rec->end_value = run_sage26(a);
  memcpy(rec->end, galaxies, (size_t)a->ngal * sizeof(galaxies[0]));

  struct GALAXY result[MAX_GALAXIES];
  double value = compose(input, a, &ALL_TRANSCRIBED, &NO_PERTURBATION, &rec->locals, result);
  if (!composition_matches(result, value, &rec->locals, rec->end, rec->end_value, spec)) {
    fatal("transcription check failed: the fully transcribed composition does not reproduce "
          "SAGE26's %s bit for bit",
          routine_name);
  }
  struct locals own;
  value = compose(input, a, &OWN_ROUTINES, &NO_PERTURBATION, &own, result);
  if (!composition_matches(result, value, &own, rec->end, rec->end_value, spec)) {
    fatal("transcription check failed: the composition with SAGE26's own sub-routines does not "
          "reproduce SAGE26's %s bit for bit",
          routine_name);
  }

  for (int id = 0; id < IM_COUNT; id++) {
    rec->sens[id].up = 0;
    rec->sens[id].down = 0;
    if (!rec->locals.present[id]) {
      continue;
    }
    const struct composition comp = composition_for(IM_INFO[id].stage);
    struct locals lp;
    value = compose(input, a, &comp, &NO_PERTURBATION, &lp, result);
    if (!composition_matches(result, value, &lp, rec->end, rec->end_value, spec)) {
      fatal("transcription check failed: the validation composition for %s does not reproduce "
            "SAGE26's %s bit for bit",
            IM_INFO[id].name, routine_name);
    }
    for (int up = 0; up <= 1; up++) {
      const struct perturbation pt = {id, -1, up, -1, -1};
      value = compose(input, a, &comp, &pt, &lp, result);
      const int changed = !lp.out_of_domain && !(same_recorded(result, rec->end, spec) &&
                                                 same_f64(value, rec->end_value));
      if (up) {
        rec->sens[id].up = changed;
      } else {
        rec->sens[id].down = changed;
      }
    }
  }
  /* c08: the SFprescription 1 values, each in the composition of the routine it is a local of */
  for (int id = 0; id < IMH_COUNT; id++) {
    rec->h_sens[id].up = 0;
    rec->h_sens[id].down = 0;
    if (!rec->locals.h_present[id]) {
      continue;
    }
    const struct composition comp = composition_for(IMH_INFO[id].stage);
    struct locals lp;
    value = compose(input, a, &comp, &NO_PERTURBATION, &lp, result);
    if (!composition_matches(result, value, &lp, rec->end, rec->end_value, spec)) {
      fatal("transcription check failed: the validation composition for %s does not reproduce "
            "SAGE26's %s bit for bit",
            IMH_INFO[id].name, routine_name);
    }
    for (int up = 0; up <= 1; up++) {
      const struct perturbation pt = {-1, -1, up, id, -1};
      value = compose(input, a, &comp, &pt, &lp, result);
      const int changed = !lp.out_of_domain && !(same_recorded(result, rec->end, spec) &&
                                                 same_f64(value, rec->end_value));
      if (up) {
        rec->h_sens[id].up = changed;
      } else {
        rec->h_sens[id].down = changed;
      }
    }
  }
  /* c09: the FIREmodeOn 1 values, each in the composition of the routine it is a local of */
  for (int id = 0; id < IMF_COUNT; id++) {
    rec->fire_sens[id].up = 0;
    rec->fire_sens[id].down = 0;
    if (!rec->locals.fire_present[id]) {
      continue;
    }
    const struct composition comp = composition_for(IMF_INFO[id].stage);
    struct locals lp;
    value = compose(input, a, &comp, &NO_PERTURBATION, &lp, result);
    if (!composition_matches(result, value, &lp, rec->end, rec->end_value, spec)) {
      fatal("transcription check failed: the validation composition for %s does not reproduce "
            "SAGE26's %s bit for bit",
            IMF_INFO[id].name, routine_name);
    }
    for (int up = 0; up <= 1; up++) {
      const struct perturbation pt = {-1, -1, up, -1, id};
      value = compose(input, a, &comp, &pt, &lp, result);
      const int changed = !lp.out_of_domain && !(same_recorded(result, rec->end, spec) &&
                                                 same_f64(value, rec->end_value));
      if (up) {
        rec->fire_sens[id].up = changed;
      } else {
        rec->fire_sens[id].down = changed;
      }
    }
  }
  for (int id = 0; id < CF_COUNT; id++) {
    rec->cflag_changes[id] = 0;
    if (!rec->locals.cflag_present[id]) {
      continue;
    }
    const struct perturbation pt = {-1, id, 0, -1, -1};
    struct locals lp;
    value = compose(input, a, &OWN_ROUTINES, &pt, &lp, result);
    rec->cflag_changes[id] = !lp.out_of_domain && !(same_recorded(result, rec->end, spec) &&
                                                    same_f64(value, rec->end_value));
  }
}

/** @brief Write one carried value: absent marker, value, check and validation markers */
static void out_carried(const struct checked_record *rec, const int id) {
  const char *name = IM_INFO[id].name;
  const int present = rec->locals.present[id];
  out_i32(field_name("intermediate.%s_absent", name), present ? 0 : 1);
  if (!present) {
    return;
  }
  out_f64(field_name("intermediate.%s", name), rec->locals.value[id]);
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), (int)IM_INFO[id].check);
  out_i32(field_name("intermediate.%s_validated_up", name), rec->sens[id].up);
  out_i32(field_name("intermediate.%s_validated_down", name), rec->sens[id].down);
  out_i32(field_name("intermediate.%s_validated", name), rec->sens[id].up && rec->sens[id].down);
}

/** @brief Write one carried flag: absent marker, value, check and flip-validation markers */
static void out_carried_flag(const struct checked_record *rec, const int id) {
  const char *name = CF_INFO[id].name;
  const int present = rec->locals.cflag_present[id];
  out_i32(field_name("intermediate.%s_absent", name), present ? 0 : 1);
  if (!present) {
    return;
  }
  out_i32(field_name("intermediate.%s", name), rec->locals.cflag[id]);
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), (int)CF_INFO[id].check);
  out_i32(field_name("intermediate.%s_validated_flip", name), rec->cflag_changes[id]);
}

static void out_flags(const struct locals *l, const int first, const int last) {
  for (int f = first; f <= last; f++) {
    out_i32(field_name("intermediate.%s", FLAG_NAMES[f]), l->flag[f]);
  }
}

static void out_values(const struct checked_record *rec, const int first, const int last) {
  for (int id = first; id <= last; id++) {
    out_carried(rec, id);
  }
}

/**
 * @brief Write one c08 value: c06's markers (tail-checked, check kind 2) and the narrowing
 *        limitation, the value as the float or double it is
 */
static void out_carried_h(const struct checked_record *rec, const int id) {
  const char *name = IMH_INFO[id].name;
  const int present = rec->locals.h_present[id];
  out_i32(field_name("intermediate.%s_absent", name), present ? 0 : 1);
  if (!present) {
    return;
  }
  if (IMH_INFO[id].is_f32) {
    out_f32(field_name("intermediate.%s", name), (float)rec->locals.h_value[id]);
  } else {
    out_f64(field_name("intermediate.%s", name), rec->locals.h_value[id]);
  }
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), (int)CHECK_TAIL);
  out_i32(field_name("intermediate.%s_validated_up", name), rec->h_sens[id].up);
  out_i32(field_name("intermediate.%s_validated_down", name), rec->h_sens[id].down);
  out_i32(field_name("intermediate.%s_validated", name),
          rec->h_sens[id].up && rec->h_sens[id].down);
  out_i32(field_name("intermediate.%s_narrowing_limited", name), IMH_INFO[id].narrowing_limited);
}

static void out_h_flags(const struct locals *l, const int first, const int last) {
  for (int f = first; f <= last; f++) {
    out_i32(field_name("intermediate.%s", H_FLAG_NAMES[f]), l->h_flag[f]);
  }
}

/**
 * @brief The c08 record of an SFprescription 1 fixture (README "The c08 Group"), written after
 *        every earlier field, so an SFprescription 0 output is unchanged
 *
 * The black-hole cap (grow_black_hole and everything that calls it), and, where the burst runs,
 * the observed star-formation commit caps, the post-reheating cap and the burst refresh.
 */
static void write_h_record(const struct checked_record *rec, const int with_bh,
                           const int with_burst) {
  if (run_params.SFprescription != 1) {
    return;
  }
  if (with_bh) {
    out_h_flags(&rec->locals, FLH_BH_CAPS_TRACKED, FLH_BH_H1_CAP_BINDS);
    out_carried_h(rec, IMH_MAX_H_BH);
  }
  if (with_burst) {
    out_h_flags(&rec->locals, FLH_SF_COMMIT_H2_CAPPED, FLH_REFRESH_H1_FLOORED);
    out_carried_h(rec, IMH_MAX_H_FB);
    out_carried_h(rec, IMH_BURST_H2_BUDGET);
    out_carried_h(rec, IMH_BURST_H1_REFRESH);
    /* the recheck's black-hole growth, OBSERVED (struct recheck_bh_observation) */
    const struct recheck_bh_observation *o = &rec->locals.recheck_bh;
    out_i32("intermediate.burst_recheck_bh_grown", o->grown);
    if (o->grown == 1) {
      out_i32("intermediate.burst_recheck_bh_h2_capped", o->h2_capped);
      out_i32("intermediate.burst_recheck_bh_h1_capped", o->h1_capped);
      out_f32("intermediate.burst_recheck_after_bh.H2gas", o->h2_after);
      out_f32("intermediate.burst_recheck_after_bh.H1gas", o->h1_after);
    }
  }
}

/**
 * @brief c09: one FIREmodeOn 1 value: c06's markers with its check kind and c07's narrowing
 *        limitation (always 0 in a burst, which never writes MassLoading)
 */
static void out_carried_fire(const struct checked_record *rec, const int id) {
  const char *name = IMF_INFO[id].name;
  const int present = rec->locals.fire_present[id];
  out_i32(field_name("intermediate.%s_absent", name), present ? 0 : 1);
  if (!present) {
    return;
  }
  out_f64(field_name("intermediate.%s", name), rec->locals.fire_value[id]);
  out_i32(field_name("intermediate.%s_checked", name), 1);
  out_i32(field_name("intermediate.%s_check_kind", name), (int)IMF_INFO[id].check);
  out_i32(field_name("intermediate.%s_validated_up", name), rec->fire_sens[id].up);
  out_i32(field_name("intermediate.%s_validated_down", name), rec->fire_sens[id].down);
  out_i32(field_name("intermediate.%s_validated", name),
          rec->fire_sens[id].up && rec->fire_sens[id].down);
  out_i32(field_name("intermediate.%s_narrowing_limited", name), 0);
}

/**
 * @brief c09: the record of a *_fire routine that runs the burst's transcription, written after
 *        every earlier field: FIREmodeOn, the FIRE flags and values, and the observations of the
 *        recheck's own burst
 */
static void write_fire_record(const struct checked_record *rec) {
  out_i32("intermediate.fire_mode", run_params.FIREmodeOn);
  for (int f = 0; f < FLF_COUNT; f++) {
    out_i32(field_name("intermediate.%s", FIRE_FLAG_NAMES[f]), rec->locals.fire_flag[f]);
  }
  for (int id = 0; id < IMF_COUNT; id++) {
    out_carried_fire(rec, id);
  }
  out_i32("intermediate.burst_recheck_fire_mode_sensitive",
          rec->locals.recheck_fire.mode_sensitive);
  out_i32("intermediate.burst_recheck_fire_redshift_sensitive",
          rec->locals.recheck_fire.redshift_sensitive);
}

// ============================================================================
// ROUTINES: SAGE26'S OWN, CHECKED
// ============================================================================

/** @brief The blocks a merger-family galaxy reads: c06's, and MassLoading for a *_fire routine */
static int merger_input_blocks(void) {
  return BLOCK_MERGER_INPUT | (fire_routine ? BLOCK_FIRE : 0);
}

/** @brief The c09 block added to a recorded galaxy of a *_fire routine */
static int fire_block(void) { return fire_routine ? BLOCK_FIRE : 0; }

static void read_merger_galaxies(const int ngal) {
  for (int i = 0; i < ngal; i++) {
    read_galaxy(i, merger_input_blocks());
  }
  if (fire_routine) {
    read_fire_snapshots(ngal);
  }
}

/** @brief estimate_merging_time (M:89-121) on a halo catalogue */
static void run_estimate_merging_time(void) {
  read_clock_params(1);
  const int nhalos = read_halo_catalogue(HALO_BASIC);
  struct call_args a;
  memset(&a, 0, sizeof(a));
  a.routine = CR_ESTIMATE_MERGING_TIME;
  a.ngal = 1;
  /* SAGE26's argument ngal, the satellite galaxy's index: the fixture holds galaxy 0 only */
  a.p = read_index("call.ngal", 1);
  a.sat_halo = read_index("call.sat_halo", nhalos);
  a.mother_halo = read_index("call.mother_halo", nhalos);
  galaxies[0].Type = in_i32("galaxy[0].Type");
  galaxies[0].SnapNum = in_i32("galaxy[0].SnapNum");
  galaxies[0].StellarMass = in_f32("galaxy[0].StellarMass");
  galaxies[0].ColdGas = in_f32("galaxy[0].ColdGas");
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input[0]));
  const struct record_spec spec = {1, {0}};
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  out_f64("return.mergtime", rec.end_value);
  out_flags(&rec.locals, FL_CLOCK_SAME_HALO, FL_CLOCK_CAPPED);
  out_values(&rec, IM_SAT_VIRIAL_MASS, IM_MERGTIME_PRE_CAP);
}

/** @brief grow_black_hole (M:346-378) on one galaxy, with the wind it calls */
static void run_grow_black_hole(void) {
  read_merger_config();
  struct call_args a;
  memset(&a, 0, sizeof(a));
  a.routine = CR_GROW_BLACK_HOLE;
  a.ngal = 1;
  a.p = 0;
  a.mass_ratio = in_f64("call.mass_ratio");
  read_merger_galaxies(1);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input[0]));
  const struct record_spec spec = {1, {BLOCK_RESERVOIRS}};
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  write_recorded("", rec.end, &spec);
  out_flags(&rec.locals, FL_BH_COLDGAS_POSITIVE, FL_WIND_EJECTS_HOT);
  out_values(&rec, IM_BH_ACCRETE_PRE_CAP, IM_HOT_GAS_ENERGY);
  write_h_record(&rec, 1, 0);
}

/** @brief quasar_mode_wind (M:390-442) on its own, with the accreted mass as input */
static void run_quasar_mode_wind(void) {
  read_merger_config();
  struct call_args a;
  memset(&a, 0, sizeof(a));
  a.routine = CR_QUASAR_MODE_WIND;
  a.ngal = 1;
  a.p = 0;
  a.bh_accrete = in_f64("call.BHaccrete");
  read_merger_galaxies(1);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, sizeof(input[0]));
  const struct record_spec spec = {1, {BLOCK_RESERVOIRS}};
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  write_recorded("", rec.end, &spec);
  out_flags(&rec.locals, FL_WIND_EJECTS_COLD, FL_WIND_EJECTS_HOT);
  out_values(&rec, IM_QUASAR_ENERGY, IM_HOT_GAS_ENERGY);
}

static void read_step_and_dt(struct call_args *a) {
  a->time = in_f64("call.time");
  a->dt = in_f64("call.dt");
  a->step = in_i32("call.step");
  /* SAGE26's assertions (M:589-592) */
  if (!(a->dt > 0.0) || a->step < 0 || a->step >= STEPS) {
    fatal("call.dt must be positive and call.step in [0, %d)", STEPS);
  }
}

/** @brief collisional_starburst_recipe (M:584-832) in either mode */
static void run_collisional_starburst_recipe(void) {
  read_merger_config();
  struct call_args a;
  memset(&a, 0, sizeof(a));
  a.routine = CR_COLLISIONAL_STARBURST;
  a.ngal = read_ngal();
  a.target = read_index("call.merger_centralgal", a.ngal);
  a.centralgal = read_index("call.centralgal", a.ngal);
  a.mass_ratio = in_f64("call.mass_ratio");
  a.mode = in_i32("call.mode");
  a.burst_to_merger_bulge = in_i32("call.burst_to_merger_bulge");
  a.old_disk_radius = in_f64("call.old_disk_radius");
  read_step_and_dt(&a);
  if (a.mode != 0 && a.mode != 1) {
    fatal("call.mode = %d (SAGE26 calls the burst with 0 or 1, M:286, R/src/model_disk_"
          "instability.c:105)",
          a.mode);
  }
  read_merger_galaxies(a.ngal);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, (size_t)a.ngal * sizeof(input[0]));
  struct record_spec spec = {a.ngal, {0}};
  /* c09: a *_fire fixture also records any other galaxy (a merging satellite the burst is not
   * given), which must stay as it was */
  for (int i = 0; i < a.ngal; i++) {
    spec.blocks[i] = fire_routine ? BLOCK_RESERVOIRS | BLOCK_FIRE : 0;
  }
  spec.blocks[a.centralgal] = BLOCK_RESERVOIRS | fire_block();
  spec.blocks[a.target] = BLOCK_RESERVOIRS | BLOCK_HISTORIES | fire_block();
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  write_recorded("", rec.end, &spec);
  out_flags(&rec.locals, FL_BURST_GAS_FLOORED, FL_BURST_YIELD_STAYS_IN_DISK);
  out_values(&rec, IM_EBURST, IM_YIELD_TO_HOT);
  write_h_record(&rec, 0, 1);
  if (fire_routine) {
    write_fire_record(&rec);
  }
}

/** @brief c09: collisional_starburst_recipe with FIREmodeOn 0 or 1 (README "The c09 Group") */
static void run_collisional_starburst_recipe_fire(void) {
  fire_routine = 1;
  run_collisional_starburst_recipe();
}

/** @brief add_galaxies_together (M:456-543) */
static void run_add_galaxies_together(void) {
  read_merger_config();
  struct call_args a;
  memset(&a, 0, sizeof(a));
  a.routine = CR_ADD_GALAXIES_TOGETHER;
  a.ngal = read_ngal();
  a.target = read_index("call.t", a.ngal);
  a.p = read_index("call.p", a.ngal);
  if (a.target == a.p) {
    fatal("call.t and call.p must differ");
  }
  read_merger_galaxies(a.ngal);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, (size_t)a.ngal * sizeof(input[0]));
  struct record_spec spec = {a.ngal, {0}};
  spec.blocks[a.p] = BLOCK_RESERVOIRS;
  spec.blocks[a.target] = BLOCK_RESERVOIRS | BLOCK_HISTORIES;
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  write_recorded("", rec.end, &spec);
  out_flags(&rec.locals, FL_SATELLITE_DISK_POSITIVE, FL_SATELLITE_ICS_POSITIVE);
  out_values(&rec, IM_SATELLITE_DISK_MASS, IM_DISK_FRACTION_POST);
}

/** @brief make_bulge_from_burst (M:553-570): SAGE26's own routine; it has no intermediates */
static void run_make_bulge_from_burst(void) {
  read_merger_galaxies(1);
  check_all_fields_used();
  make_bulge_from_burst(0, galaxies);
  write_galaxy("", 0, &galaxies[0], BLOCK_RESERVOIRS | BLOCK_HISTORIES);
}

/**
 * @brief The c07 record of a BulgeSizeOn 3 merger: the remnant radius's branch flags and
 *        locals, and each radius value's narrowing limitation (README "The c07 Group")
 *
 * Written only with BulgeSizeOn 3, after the c06 record, so a mode-0 output is unchanged. Each
 * remnant local is written with c06's markers and `_narrowing_limited` 1: its only path to the
 * state is new_merger_radius's float store into MergerBulgeRadius (M:307, :328), one
 * narrowing. new_merger_radius and old_disk_radius, already in the c06 record, get the same
 * marker: new_merger_radius reaches the state only through that store, old_disk_radius only
 * through the burst's update_instability_bulge_radius, whose result is one float store into
 * InstabilityBulgeRadius (R/src/model_misc.c:422; the closing get_bulge_radius reads that
 * float).
 */
static void write_remnant_record(const struct checked_record *rec) {
  out_flags(&rec->locals, FL_REM_TOTAL_NONPOSITIVE, FL_REM_ENERGY_NONPOSITIVE);
  out_i32("intermediate.new_merger_radius_narrowing_limited", 1);
  out_i32("intermediate.old_disk_radius_narrowing_limited", 1);
  for (int id = IM_REM_M1; id <= IM_REM_E_FINAL; id++) {
    out_carried(rec, id);
    if (rec->locals.present[id]) {
      out_i32(field_name("intermediate.%s_narrowing_limited", IM_INFO[id].name), 1);
    }
  }
}

/** @brief deal_with_galaxy_merger (M:228-333) end to end */
static void run_deal_with_galaxy_merger(void) {
  read_merger_config();
  struct call_args a;
  memset(&a, 0, sizeof(a));
  a.routine = CR_DEAL_WITH_GALAXY_MERGER;
  a.ngal = read_ngal();
  a.p = read_index("call.p", a.ngal);
  a.target = read_index("call.merger_centralgal", a.ngal);
  a.centralgal = read_index("call.centralgal", a.ngal);
  read_step_and_dt(&a);
  if (a.p == a.target || a.p == a.centralgal) {
    fatal("call.p must differ from call.merger_centralgal and call.centralgal");
  }
  read_merger_galaxies(a.ngal);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, (size_t)a.ngal * sizeof(input[0]));
  struct record_spec spec = {a.ngal, {0}};
  spec.blocks[a.centralgal] = BLOCK_IDENT | BLOCK_RESERVOIRS | fire_block();
  spec.blocks[a.p] = BLOCK_IDENT | BLOCK_RESERVOIRS | fire_block();
  spec.blocks[a.target] = BLOCK_IDENT | BLOCK_RESERVOIRS | BLOCK_HISTORIES | fire_block();
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  write_recorded("", rec.end, &spec);
  out_flags(&rec.locals, FL_MASS_RATIO_PATH, FL_BURST_YIELD_STAYS_IN_DISK);
  for (int id = 0; id < CF_COUNT; id++) {
    out_carried_flag(&rec, id);
  }
  out_values(&rec, IM_MI, IM_YIELD_TO_HOT);
  if (run_params.BulgeSizeOn == 3) {
    write_remnant_record(&rec);
  }
  write_h_record(&rec, 1, 1);
  if (fire_routine) {
    write_fire_record(&rec);
  }
}

/** @brief c09: deal_with_galaxy_merger with FIREmodeOn 0 or 1 (README "The c09 Group") */
static void run_deal_with_galaxy_merger_fire(void) {
  fire_routine = 1;
  run_deal_with_galaxy_merger();
}

/** @brief disrupt_satellite_to_ICS (M:845-903) */
static void run_disrupt_satellite_to_ICS(void) {
  read_merger_config();
  struct call_args a;
  memset(&a, 0, sizeof(a));
  a.routine = CR_DISRUPT_SATELLITE_TO_ICS;
  a.ngal = read_ngal();
  a.centralgal = read_index("call.centralgal", a.ngal);
  a.p = read_index("call.gal", a.ngal);
  a.time = in_f64("call.time");
  if (a.p == a.centralgal) {
    fatal("call.gal and call.centralgal must differ");
  }
  read_merger_galaxies(a.ngal);
  check_all_fields_used();

  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, (size_t)a.ngal * sizeof(input[0]));
  struct record_spec spec = {a.ngal, {0}};
  spec.blocks[a.centralgal] = BLOCK_RESERVOIRS;
  spec.blocks[a.p] = BLOCK_IDENT | BLOCK_RESERVOIRS | BLOCK_HISTORIES;
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  write_recorded("", rec.end, &spec);
  out_flags(&rec.locals, FL_DISRUPT_STARS_POSITIVE, FL_DISRUPT_ICS_POSITIVE);
  out_values(&rec, IM_TOTAL_GAS, IM_TOTAL_METALS_GAS);
}

// ============================================================================
// SOURCE-REVIEWED DRIVER TRANSCRIPTIONS (B = R/src/core_build_model.c)
// ============================================================================

/** What the pass did to one galaxy (intermediate.pass.galaxy[i].*) */
struct pass_galaxy {
  int considered;        /* B:523: a satellite not yet merged */
  double deltaT;         /* B:529 */
  double currentMvir;    /* B:534 */
  double galaxyBaryons;  /* B:535 */
  int gate_zero_baryons; /* B:536 */
  int gate_passed;       /* B:537 */
  int merger_centralgal; /* B:539-543, -1 if the gate failed */
  int redirected;        /* B:541-543 */
  int clock_finite;      /* B:547 */
  double event_time;     /* B:549 */
  int action;            /* 0 none, 1 deal_with_galaxy_merger, 4 disrupt_satellite_to_ICS */
  int step_bin;          /* B:579-580 */
  double dt;             /* B:581 */
};

/** The arguments of one substep of the pass */
struct pass_args {
  int ngal;
  int centralgal; /* B:393: galaxies[0].CentralGal */
  int step;
  int effective_steps;
  double halo_age;
  double age[MAX_GALAXIES]; /* Age[galaxies[p].SnapNum] for each galaxy */
  int numgals;
};

/**
 * @brief SOURCE-REVIEWED TRANSCRIPTION of the merger and disruption pass of one substep,
 *        B:519-587
 *
 * Statement for statement, each cited. The diagnostic log (B:550-572) writes only to a file
 * named by an environment variable and is not transcribed. The disruption and merger calls
 * are SAGE26's own routines. B:524-527 returns EXIT_FAILURE from evolve_galaxies; the oracle
 * refuses the fixture there instead.
 */
static void transcribe_merger_pass(const struct pass_args *a, struct pass_galaxy *rec) {
  const int centralgal = a->centralgal;
  const int step = a->step;
  const int effective_steps = a->effective_steps;

  /* B:520 */
  for (int p = 0; p < a->ngal; p++) {
    struct pass_galaxy *r = &rec[p];
    memset(r, 0, sizeof(*r));
    r->merger_centralgal = -1;
    r->event_time = NAN;

    /* B:523 */
    if ((galaxies[p].Type == 1 || galaxies[p].Type == 2) && galaxies[p].mergeType == 0) {
      r->considered = 1;
      /* B:524-527 */
      if (!(galaxies[p].MergTime < 999.0)) {
        fatal("galaxy[%d].MergTime = %g: SAGE26's evolve_galaxies returns EXIT_FAILURE "
              "(B:524-527)",
              p, (double)galaxies[p].MergTime);
      }

      /* B:529-530 */
      const double deltaT = a->age[p] - a->halo_age;
      galaxies[p].MergTime -= deltaT / effective_steps;
      r->deltaT = deltaT;

      /* B:534-536 */
      double currentMvir =
          galaxies[p].Mvir -
          galaxies[p].deltaMvir * (1.0 - ((double)step + 1.0) / (double)effective_steps);
      double galaxyBaryons = galaxies[p].StellarMass + galaxies[p].ColdGas;
      const int gate_zero_baryons = (galaxyBaryons == 0.0);
      r->currentMvir = currentMvir;
      r->galaxyBaryons = galaxyBaryons;
      r->gate_zero_baryons = gate_zero_baryons;

      /* B:537 */
      if ((galaxyBaryons == 0.0) || (galaxyBaryons > 0.0 && (currentMvir / galaxyBaryons <=
                                                             run_params.ThresholdSatDisruption))) {
        r->gate_passed = 1;

        /* B:539-543 */
        int merger_centralgal = galaxies[p].Type == 1 ? centralgal : galaxies[p].CentralGal;

        if (galaxies[merger_centralgal].mergeType > 0) {
          merger_centralgal = galaxies[merger_centralgal].CentralGal;
          r->redirected = 1;
        }
        r->merger_centralgal = merger_centralgal;

        /* B:545 */
        galaxies[p].mergeIntoID = a->numgals + merger_centralgal;

        /* B:547 */
        if (isfinite(galaxies[p].MergTime)) {
          r->clock_finite = 1;
          /* B:549 */
          const double event_time = a->age[p] - (step + 0.5) * (deltaT / effective_steps);
          r->event_time = event_time;

          /* B:574-582 */
          if (galaxies[p].MergTime > 0.0) {
            r->action = 4;
            disrupt_satellite_to_ICS(merger_centralgal, p, event_time, galaxies, &run_params);
          } else {
            r->action = 1;
            int step_bin = (step * STEPS) / effective_steps;
            if (step_bin >= STEPS) {
              step_bin = STEPS - 1;
            }
            r->step_bin = step_bin;
            r->dt = deltaT / effective_steps;
            deal_with_galaxy_merger(p, merger_centralgal, centralgal, event_time,
                                    deltaT / effective_steps, ORACLE_HALONR, step_bin, galaxies,
                                    &run_params);
          }
        }
      }
    }
  }
}

/** @brief An i32 record field with its _absent marker: the value only where the path sets it */
static void out_optional_i32(const char *base, const char *name, const int present,
                             const int value) {
  out_i32(field_name("%s%s_absent", base, name), present ? 0 : 1);
  if (present) {
    out_i32(field_name("%s%s", base, name), value);
  }
}

/** @brief An f64 record field with its _absent marker: the value only where the path sets it */
static void out_optional_f64(const char *base, const char *name, const int present,
                             const double value) {
  out_i32(field_name("%s%s_absent", base, name), present ? 0 : 1);
  if (present) {
    out_f64(field_name("%s%s", base, name), value);
  }
}

/**
 * @brief The pass record of every galaxy: considered and action on every path, and each other
 *        field with an _absent marker (1 where the path does not evaluate it)
 */
static void write_pass_record(const char *prefix, const int ngal, const struct pass_galaxy *rec) {
  for (int p = 0; p < ngal; p++) {
    const struct pass_galaxy *r = &rec[p];
    char base[NAME_LEN];
    snprintf(base, sizeof(base), "intermediate.%sgalaxy[%d].", prefix, p);
    const int considered = r->considered;
    const int gated = considered && r->gate_passed;
    out_i32(field_name("%sconsidered", base), considered);
    out_i32(field_name("%saction", base), r->action);
    out_optional_f64(base, "deltaT", considered, r->deltaT);
    out_optional_f64(base, "currentMvir", considered, r->currentMvir);
    out_optional_f64(base, "galaxyBaryons", considered, r->galaxyBaryons);
    out_optional_i32(base, "gate_zero_baryons", considered, r->gate_zero_baryons);
    out_optional_i32(base, "gate_passed", considered, r->gate_passed);
    out_optional_i32(base, "merger_centralgal", gated, r->merger_centralgal);
    out_optional_i32(base, "redirected", gated, r->redirected);
    out_optional_i32(base, "clock_finite", gated, r->clock_finite);
    out_optional_f64(base, "event_time", gated && r->clock_finite, r->event_time);
    out_optional_i32(base, "step_bin", r->action == 1, r->step_bin);
    out_optional_f64(base, "dt", r->action == 1, r->dt);
  }
}

/**
 * @brief c09: the record of a merger_pass_fire fixture, after every earlier field
 *
 * intermediate.fire_mode, and for FIREmodeOn 1 two OBSERVATIONS of the pass's bursts, which run
 * inside SAGE26's own deal_with_galaxy_merger: whether the pass's end state (every galaxy's
 * members) differs when the pass runs again on the input with FIREmodeOn 0
 * (pass_fire_mode_sensitive), and when the redshift of each merger target's stored snapshot,
 * ZZ[SnapNum], is replaced by itself plus 1.0 (pass_fire_redshift_sensitive); -1 for FIREmodeOn 0
 * or a pass that merges nothing. Only the burst reads FIREmodeOn and ZZ on these paths (M:693-694,
 * :707, :730).
 */
static void write_pass_fire_record(const struct pass_args *a, const struct GALAXY *input,
                                   const struct pass_galaxy *rec) {
  out_i32("intermediate.fire_mode", run_params.FIREmodeOn);
  int merged = 0;
  for (int i = 0; i < a->ngal; i++) {
    merged |= rec[i].action == 1;
  }
  int sensitive[2] = {-1, -1};
  if (run_params.FIREmodeOn == 1 && merged) {
    struct GALAXY end[MAX_GALAXIES];
    memcpy(end, galaxies, (size_t)a->ngal * sizeof(end[0]));
    static struct params saved;
    memcpy(&saved, &run_params, sizeof(saved));
    const struct record_spec spec = {a->ngal,
                                     {merger_input_blocks(), merger_input_blocks(),
                                      merger_input_blocks(), merger_input_blocks()}};
    for (int which = 0; which < 2; which++) {
      memcpy(&run_params, &saved, sizeof(run_params));
      if (which == 0) {
        run_params.FIREmodeOn = 0;
      } else {
        int done[ABSOLUTEMAXSNAPS] = {0};
        for (int i = 0; i < a->ngal; i++) {
          if (rec[i].action == 1) {
            const int snap = input[rec[i].merger_centralgal].SnapNum;
            if (!done[snap]) {
              run_params.ZZ[snap] = saved.ZZ[snap] + 1.0;
              done[snap] = 1;
            }
          }
        }
      }
      memcpy(galaxies, input, (size_t)a->ngal * sizeof(galaxies[0]));
      struct pass_galaxy cf[MAX_GALAXIES];
      transcribe_merger_pass(a, cf);
      sensitive[which] = !same_recorded(galaxies, end, &spec);
    }
    memcpy(&run_params, &saved, sizeof(run_params));
    memcpy(galaxies, end, (size_t)a->ngal * sizeof(galaxies[0]));
  }
  out_i32("intermediate.pass_fire_mode_sensitive", sensitive[0]);
  out_i32("intermediate.pass_fire_redshift_sensitive", sensitive[1]);
}

/** @brief Read the pass's call inputs: the substep, Age per galaxy snapshot, layout */
static void read_pass_args(struct pass_args *a, const int ngal) {
  a->ngal = ngal;
  a->centralgal = read_index("call.centralgal", ngal);
  a->step = in_i32("call.step");
  a->effective_steps = in_i32("call.effective_steps");
  if (a->effective_steps < 1 || a->effective_steps > MAX_STEPS || a->step < 0 ||
      a->step >= a->effective_steps) {
    fatal("call.step = %d and call.effective_steps = %d are inconsistent", a->step,
          a->effective_steps);
  }
  a->halo_age = in_f64("call.halo_age");
  a->numgals = in_i32("call.numgals");
}

/** @brief Age for every distinct galaxy SnapNum, read at its first use (B:529, B:549) */
static void read_pass_ages(struct pass_args *a) {
  for (int i = 0; i < a->ngal; i++) {
    int seen = -1;
    for (int j = 0; j < i; j++) {
      if (galaxies[j].SnapNum == galaxies[i].SnapNum) {
        seen = j;
      }
    }
    a->age[i] = seen >= 0 ? a->age[seen] : in_f64(field_name("call.Age[%d]", galaxies[i].SnapNum));
  }
}

static void read_pass_params(void) {
  run_params.ThresholdSatDisruption = in_f64("params.ThresholdSatDisruption");
}

/**
 * @brief The layout evolve_galaxies guarantees before the pass: the central is galaxies[0]'s
 *        CentralGal and is Type 0 (B:393-400), and every CentralGal indexes the FoF array
 *        (B:368-370)
 */
static void check_pass_layout(const struct pass_args *a) {
  if (a->centralgal != galaxies[0].CentralGal || galaxies[a->centralgal].Type != 0) {
    fatal("call.centralgal = %d must be galaxies[0].CentralGal (%d) and Type 0 (B:393-400)",
          a->centralgal, galaxies[0].CentralGal);
  }
  for (int i = 0; i < a->ngal; i++) {
    if (galaxies[i].CentralGal < 0 || galaxies[i].CentralGal >= a->ngal) {
      fatal("galaxy[%d].CentralGal = %d is outside the FoF array", i, galaxies[i].CentralGal);
    }
  }
}

/** @brief The merger and disruption pass of one substep (a source-reviewed transcription) */
static void run_merger_pass(void) {
  read_merger_config();
  read_pass_params();
  const int ngal = read_ngal();
  struct pass_args a;
  read_pass_args(&a, ngal);
  read_merger_galaxies(ngal);
  read_pass_ages(&a);
  check_all_fields_used();
  check_pass_layout(&a);

  /* c09: the input, for the FIREmodeOn 1 pass's observations */
  struct GALAXY input[MAX_GALAXIES];
  memcpy(input, galaxies, (size_t)ngal * sizeof(input[0]));

  struct pass_galaxy rec[MAX_GALAXIES];
  transcribe_merger_pass(&a, rec);

  for (int i = 0; i < ngal; i++) {
    write_galaxy("", i, &galaxies[i], merger_input_blocks());
  }
  write_pass_record("pass.", ngal, rec);
  if (fire_routine) {
    write_pass_fire_record(&a, input, rec);
  }
}

/** @brief c09: the merger pass with FIREmodeOn 0 or 1 (README "The c09 Group") */
static void run_merger_pass_fire(void) {
  fire_routine = 1;
  run_merger_pass();
}

/** What joining did to one progenitor galaxy (intermediate.join.progenitor[k].*) */
struct join_progenitor {
  int branch;   /* JOIN_* below */
  int slot;     /* the galaxies[] slot B:234 copied it into */
  int retained; /* 1 if B:341 counted the slot; 0 if the B:244 continue skipped it */
  double previousMvir, previousVvir, previousVmax; /* B:248-250 */
  double new_Mvir;                                 /* main branch only: B:264 */
  int halo_grew;                                   /* main branch only: B:267 */
  int infall_recorded;                             /* B:306 (satellite) or B:327 (orphan) */
  int clock_estimated;                             /* B:315, satellite only */
};

enum join_branch {
  JOIN_UNTOUCHED = 0,      /* Type 2 or 3 progenitor: copied, B:240 false */
  JOIN_ALREADY_MERGED = 1, /* mergeType != 0: Type 3 and dropped, B:242-245 */
  JOIN_CENTRAL = 2,        /* main branch, FoF central: B:284-300 */
  JOIN_SATELLITE = 3,      /* main branch, subhalo: B:301-321 */
  JOIN_ORPHAN = 4          /* not the main branch: B:322-338 */
};

/** The arguments of one join call */
struct join_args {
  int nprog; /* progenitor galaxies, in the order B:213-214 visits them */
  int halonr;
  int first_occupied;
  int prog[MAX_GALAXIES];
};

/** The progenitor galaxies as halogal holds them (B:234's source), call.galaxy[k] in the input */
static struct GALAXY progenitor_galaxies[MAX_GALAXIES];

/**
 * @brief SOURCE-REVIEWED TRANSCRIPTION of the per-galaxy block of join_galaxies_of_progenitors,
 *        B:210-345, and the central assignment, B:358-370, for one descendant halo
 *
 * The progenitor galaxies arrive as halogal holds them, in the order B:213-214 visits them, each
 * with the progenitor halo it came from (call.galaxy[k].prog) and the progenitor B:181-208
 * selects as first_occupied (call.first_occupied: tree bookkeeping, not c06 physics, taken as an
 * input). ngalstart is 0. As in SAGE26, the progenitor index and the galaxies[] slot are
 * separate: B:234 copies each progenitor into slot ngal, and the B:244 continue skips B:341's
 * ngal++, so an already-merged galaxy's slot is overwritten by the next progenitor (or left past
 * the returned count) and never enters B:358-370. Returns ngal (B:372). Not transcribed:
 * MostBoundID, Pos and Vel (B:254-259, identity data no c06 routine reads), the concentration
 * (B:291-295, ConcentrationOn 0 on R0, R0.par:57), the array growth (B:215-227) and the creation
 * of a new galaxy when no progenitor leaves one (B:347-351, c03's init_galaxy), which the oracle
 * refuses. The halo helpers, estimate_merging_time, get_disk_radius and get_bulge_radius are
 * SAGE26's own. B:361-362 returns -1 when two galaxies are Type 0 or 1; the oracle refuses the
 * fixture there instead.
 */
static int transcribe_join_progenitors(const struct join_args *a, struct join_progenitor *rec) {
  const int halonr = a->halonr;

  /* B:210, with ngalstart 0 */
  int ngal = 0;

  /* B:213-214 */
  for (int k = 0; k < a->nprog; k++) {
    struct join_progenitor *r = &rec[k];
    memset(r, 0, sizeof(*r));
    r->branch = JOIN_UNTOUCHED;
    r->slot = ngal;
    const int prog = a->prog[k];

    /* B:234-235 */
    galaxies[ngal] = progenitor_galaxies[k];
    galaxies[ngal].HaloNr = halonr;

    /* B:237 */
    galaxies[ngal].dT = -1.0;

    /* B:240 */
    if (galaxies[ngal].Type == 0 || galaxies[ngal].Type == 1) {
      /* B:242-245: the continue skips B:341 */
      if (galaxies[ngal].mergeType != 0) {
        galaxies[ngal].Type = 3;
        r->branch = JOIN_ALREADY_MERGED;
        continue;
      }

      /* B:248-250 */
      const double previousMvir = galaxies[ngal].Mvir;
      const double previousVvir = galaxies[ngal].Vvir;
      const double previousVmax = galaxies[ngal].Vmax;
      r->previousMvir = previousMvir;
      r->previousVvir = previousVvir;
      r->previousVmax = previousVmax;

      /* B:252 */
      if (prog == a->first_occupied) {
        /* B:261-262 */
        galaxies[ngal].Len = halos[halonr].Len;
        galaxies[ngal].Vmax = halos[halonr].Vmax;

        /* B:264-265 */
        const double new_Mvir = get_virial_mass(halonr, halos, &run_params);
        galaxies[ngal].deltaMvir = new_Mvir - galaxies[ngal].Mvir;
        r->new_Mvir = new_Mvir;

        /* B:267-271 */
        if (new_Mvir > galaxies[ngal].Mvir) {
          r->halo_grew = 1;
          galaxies[ngal].Rvir = get_virial_radius(halonr, halos, &run_params);
          galaxies[ngal].Vvir = get_virial_velocity(halonr, halos, &run_params);
        }
        galaxies[ngal].Mvir = new_Mvir;

        /* B:273-276 */
        galaxies[ngal].Cooling = 0.0;
        galaxies[ngal].Heating = 0.0;
        galaxies[ngal].QuasarModeBHaccretionMass = 0.0;
        galaxies[ngal].OutflowRate = 0.0;

        /* B:278-282 */
        for (int step = 0; step < STEPS; step++) {
          galaxies[ngal].SfrDisk[step] = galaxies[ngal].SfrBulge[step] = 0.0;
          galaxies[ngal].SfrDiskColdGas[step] = galaxies[ngal].SfrDiskColdGasMetals[step] = 0.0;
          galaxies[ngal].SfrBulgeColdGas[step] = galaxies[ngal].SfrBulgeColdGasMetals[step] = 0.0;
        }

        /* B:284 */
        if (halonr == halos[halonr].FirstHaloInFOFgroup) {
          r->branch = JOIN_CENTRAL;
          /* B:286-288 */
          galaxies[ngal].mergeType = 0;
          galaxies[ngal].mergeIntoID = -1;
          galaxies[ngal].MergTime = 999.9f;

          /* B:291-295: ConcentrationOn 0 on R0 */

          /* B:297-300 */
          galaxies[ngal].DiskScaleRadius = get_disk_radius(halonr, ngal, halos, galaxies);
          get_bulge_radius(ngal, galaxies, &run_params);

          galaxies[ngal].Type = 0;
        } else {
          r->branch = JOIN_SATELLITE;
          /* B:303-304 */
          galaxies[ngal].mergeType = 0;
          galaxies[ngal].mergeIntoID = -1;

          /* B:306-313 */
          if (galaxies[ngal].Type == 0) {
            r->infall_recorded = 1;
            galaxies[ngal].infallMvir = previousMvir;
            galaxies[ngal].infallVvir = previousVvir;
            galaxies[ngal].infallVmax = previousVmax;
            galaxies[ngal].infallStellarMass = galaxies[ngal].StellarMass;
            galaxies[ngal].TimeOfInfall = halos[halonr].SnapNum;
          }

          /* B:315-318 */
          if (galaxies[ngal].Type == 0 || galaxies[ngal].MergTime > 999.0f) {
            r->clock_estimated = 1;
            galaxies[ngal].MergTime = estimate_merging_time(
                halonr, halos[halonr].FirstHaloInFOFgroup, ngal, halos, galaxies, &run_params);
          }

          /* B:320 */
          galaxies[ngal].Type = 1;
        }
      } else {
        r->branch = JOIN_ORPHAN;
        /* B:324-325 */
        galaxies[ngal].deltaMvir = -1.0 * galaxies[ngal].Mvir;
        galaxies[ngal].Mvir = 0.0;

        /* B:327-335 */
        if (galaxies[ngal].MergTime > 999.0 || galaxies[ngal].Type == 0) {
          r->infall_recorded = 1;
          galaxies[ngal].MergTime = 0.0;

          galaxies[ngal].infallMvir = previousMvir;
          galaxies[ngal].infallVvir = previousVvir;
          galaxies[ngal].infallVmax = previousVmax;
          galaxies[ngal].infallStellarMass = galaxies[ngal].StellarMass;
        }

        /* B:337 */
        galaxies[ngal].Type = 2;
      }
    }

    /* B:341 */
    r->retained = 1;
    ngal++;
  }

  /* B:347-351 */
  if (ngal == 0) {
    fatal("no progenitor galaxy is retained: SAGE26 creates a new galaxy (B:347-351, c03's "
          "init_galaxy), which this transcription does not cover");
  }

  /* B:358-366 */
  int centralgal = -1;
  for (int i = 0; i < ngal; i++) {
    if (galaxies[i].Type == 0 || galaxies[i].Type == 1) {
      if (centralgal != -1) {
        fatal("two Type 0 or 1 galaxies (%d and %d) after joining: SAGE26 returns -1 (B:361-362)",
              centralgal, i);
      }
      centralgal = i;
    }
  }

  /* B:368-370 */
  for (int i = 0; i < ngal; i++) {
    galaxies[i].CentralGal = centralgal;
  }

  /* B:372 */
  return ngal;
}

static void write_join_record(const char *prefix, const int nprog,
                              const struct join_progenitor *rec) {
  for (int k = 0; k < nprog; k++) {
    const struct join_progenitor *r = &rec[k];
    char base[NAME_LEN];
    snprintf(base, sizeof(base), "intermediate.%sprogenitor[%d].", prefix, k);
    const int main_branch = r->branch == JOIN_CENTRAL || r->branch == JOIN_SATELLITE;
    const int previous = main_branch || r->branch == JOIN_ORPHAN;
    out_i32(field_name("%sbranch", base), r->branch);
    out_i32(field_name("%sslot", base), r->slot);
    out_i32(field_name("%sretained", base), r->retained);
    out_optional_f64(base, "previousMvir", previous, r->previousMvir);
    out_optional_f64(base, "previousVvir", previous, r->previousVvir);
    out_optional_f64(base, "previousVmax", previous, r->previousVmax);
    out_optional_f64(base, "new_Mvir", main_branch, r->new_Mvir);
    out_optional_i32(base, "halo_grew", main_branch, r->halo_grew);
    out_optional_i32(base, "infall_recorded",
                     r->branch == JOIN_SATELLITE || r->branch == JOIN_ORPHAN, r->infall_recorded);
    out_optional_i32(base, "clock_estimated", r->branch == JOIN_SATELLITE, r->clock_estimated);
  }
}

/** @brief Read the join's call inputs and parameters */
static void read_join_args(struct join_args *a, const int nprog, const int nhalos) {
  a->nprog = nprog;
  a->halonr = read_index("call.halonr", nhalos);
  a->first_occupied = read_index("call.first_occupied", nhalos);
  for (int k = 0; k < nprog; k++) {
    a->prog[k] = read_index(field_name("call.galaxy[%d].prog", k), nhalos);
  }
}

/**
 * @brief Read the progenitor galaxies (call.ngal of them, galaxy[k] in the input) into
 *        progenitor_galaxies[] and leave galaxies[], the slots joining fills, poisoned
 */
static void read_progenitor_galaxies(const int nprog) {
  for (int k = 0; k < nprog; k++) {
    read_galaxy(k, BLOCK_JOIN_INPUT);
    progenitor_galaxies[k] = galaxies[k];
  }
  memset(galaxies, 0xff, sizeof(galaxies));
}

/**
 * @brief The flag joining pins: ConcentrationOn 0 (B:291, R0.par:57). Its other flag,
 *        BulgeSizeOn (get_bulge_radius), is read by read_bulge_size_config: in
 *        run_join_progenitors directly, in run_join_and_pass through read_merger_config.
 */
static void read_join_flags(void) {
  run_params.ConcentrationOn = require_flag("ConcentrationOn", 0);
}

/**
 * @brief Clock, infall and history assignment of progenitor joining (a transcription)
 *
 * Records the returned count (return.ngal), every member of each retained galaxy
 * (galaxy[0 .. ngal-1]) and what joining did to each progenitor.
 */
static void run_join_progenitors(void) {
  read_join_flags();
  read_bulge_size_config();
  read_clock_params(1);
  const int nhalos = read_halo_catalogue(HALO_BASIC | HALO_JOIN);
  const int nprog = read_ngal();
  struct join_args a;
  read_join_args(&a, nprog, nhalos);
  read_progenitor_galaxies(nprog);
  check_all_fields_used();

  struct join_progenitor rec[MAX_GALAXIES];
  const int ngal = transcribe_join_progenitors(&a, rec);

  out_i32("return.ngal", ngal);
  for (int i = 0; i < ngal; i++) {
    write_galaxy("", i, &galaxies[i], BLOCK_ALL);
  }
  write_join_record("join.", nprog, rec);
}

/**
 * @brief Joining followed by one substep of the pass: the orphan history retention
 *        (B:252-282, B:322-337) carried into a merger in the same snapshot
 *
 * Both halves are the source-reviewed transcriptions above, and the pass sees the ngal galaxies
 * joining retained (B:152 passes the returned count to evolve_galaxies). The ordinary physics of
 * the substeps (B:466-517) between them is not composed: the pass sees the joined state. Every
 * member of each retained galaxy is recorded after joining (intermediate.join.galaxy[i]) and
 * after the pass (galaxy[i]).
 */
static void run_join_and_pass(void) {
  read_merger_config();
  read_pass_params();
  read_join_flags();
  read_clock_params(0);
  const int nhalos = read_halo_catalogue(HALO_BASIC | HALO_JOIN);
  const int nprog = read_ngal();
  struct join_args ja;
  read_join_args(&ja, nprog, nhalos);
  struct pass_args pa;
  read_pass_args(&pa, nprog);
  read_progenitor_galaxies(nprog);

  struct join_progenitor jrec[MAX_GALAXIES];
  const int ngal = transcribe_join_progenitors(&ja, jrec);

  /* The pass's Age per galaxy snapshot, for the retained galaxies (joining keeps SnapNum) */
  pa.ngal = ngal;
  read_pass_ages(&pa);
  check_all_fields_used();
  check_pass_layout(&pa);

  out_i32("intermediate.join.ngal", ngal);
  for (int i = 0; i < ngal; i++) {
    write_galaxy("intermediate.join.", i, &galaxies[i], BLOCK_ALL);
  }

  struct pass_galaxy prec[MAX_GALAXIES];
  transcribe_merger_pass(&pa, prec);

  for (int i = 0; i < ngal; i++) {
    write_galaxy("", i, &galaxies[i], BLOCK_ALL);
  }
  write_join_record("join.", nprog, jrec);
  write_pass_record("pass.", ngal, prec);
}

// ============================================================================
// MAIN
// ============================================================================

struct routine_entry {
  const char *name;
  void (*run)(void);
};

static const struct routine_entry routines[] = {
    {"estimate_merging_time", run_estimate_merging_time},
    {"grow_black_hole", run_grow_black_hole},
    {"quasar_mode_wind", run_quasar_mode_wind},
    {"collisional_starburst_recipe", run_collisional_starburst_recipe},
    {"add_galaxies_together", run_add_galaxies_together},
    {"make_bulge_from_burst", run_make_bulge_from_burst},
    {"deal_with_galaxy_merger", run_deal_with_galaxy_merger},
    {"disrupt_satellite_to_ICS", run_disrupt_satellite_to_ICS},
    {"merger_pass", run_merger_pass},
    {"join_progenitors", run_join_progenitors},
    {"join_and_pass", run_join_and_pass},
    {"collisional_starburst_recipe_fire", run_collisional_starburst_recipe_fire},
    {"deal_with_galaxy_merger_fire", run_deal_with_galaxy_merger_fire},
    {"merger_pass_fire", run_merger_pass_fire},
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
          "# SAGE26 oracle output for %s, written by oracle_c06 (regenerate.sh); do not edit.\n",
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
