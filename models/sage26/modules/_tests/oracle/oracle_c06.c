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
 *   deal_with_galaxy_merger       M:228-333 end to end
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
static const double DISK_HALF_MASS_FRAC = 1.68;  /* M:37 */
static const double COVINGTON11_C_RAD = 2.75;    /* M:41 */
static const double BH_GROWTH_V_KMS = 280.0;     /* M:45 */
static const double STARBURST_FRAC_COEFF = 0.56; /* M:50 */
static const double STARBURST_MASS_POWER = 0.7;  /* M:51 */
static const double KD11_METAL_HALO_MASS = 30.0; /* M:65 */

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

/** Which groups of members a galaxy reads or records */
enum block {
  BLOCK_IDENT = 1,
  BLOCK_HALO = 2,
  BLOCK_RESERVOIRS = 4,
  BLOCK_INFALL = 8,
  BLOCK_JOIN_WRITTEN = 16,
  BLOCK_HISTORIES = 32,
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
 * @brief R0's flags and parameters for every routine that can reach a merger, a burst or the
 *        instability (R/input/millennium.par:48-89; StarburstColdGasOn and SaveFullSFH are
 *        SAGE26's defaults, R/src/core_read_parameter_file.c:93-130)
 *
 * The transcriptions below implement R0's paths only: SFprescription 0 (no H2 tracking), the
 * SN recipe without FIRE, no CGM, the Bondi AGN recipe (AGNrecipeOn 2), cold-gas starbursts,
 * no bulge sizes, no full SFH and the disk instability on. The code constants are SAGE26's
 * (EtaSNcode, EnergySNcode, R/src/core_init.c:102-103; G, :96).
 */
static void read_merger_config(void) {
  run_params.SFprescription = require_flag("SFprescription", 0);
  run_params.SupernovaRecipeOn = require_flag("SupernovaRecipeOn", 1);
  run_params.CGMrecipeOn = require_flag("CGMrecipeOn", 0);
  run_params.FIREmodeOn = require_flag("FIREmodeOn", 0);
  run_params.BulgeSizeOn = require_flag("BulgeSizeOn", 0);
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
 *        with G and Hubble at SAGE26's derived bit patterns (R/src/core_init.c:96, :109)
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
  STAGE_CLOCK,    /* estimate_merging_time */
  STAGE_MERGER,   /* deal_with_galaxy_merger */
  STAGE_ADD,      /* add_galaxies_together */
  STAGE_BH,       /* grow_black_hole */
  STAGE_WIND,     /* quasar_mode_wind */
  STAGE_BURST,    /* collisional_starburst_recipe */
  STAGE_FEEDBACK, /* update_from_feedback, as the burst calls it */
  STAGE_DISRUPT   /* disrupt_satellite_to_ICS */
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
};

/** Values of intermediate.mass_ratio_path (M:243-249) */
enum mass_ratio_path { RATIO_DIVIDED = 0, RATIO_ONE = 1, RATIO_ZERO = 2 };

/** The carried values, carried flags and branch flags of one composition */
struct locals {
  double value[IM_COUNT];
  int present[IM_COUNT];
  int cflag[CF_COUNT];
  int cflag_present[CF_COUNT];
  int flag[FL_COUNT];
  int out_of_domain; /* a SAGE26 assertion would abort on this path (perturbed runs only) */
};

static void init_locals(struct locals *l) {
  memset(l, 0, sizeof(*l));
  for (int f = 0; f < FL_COUNT; f++) {
    l->flag[f] = -1;
  }
}

/** A replacement of one carried value by a neighbour, or of one carried flag by its negation */
struct perturbation {
  int im; /* enum im_id to perturb, or -1 */
  int cf; /* enum carried_flag_id to flip, or -1 */
  int up; /* 1: toward +inf; 0: toward -inf */
};

static const struct perturbation NO_PERTURBATION = {-1, -1, 0};

/** Which SAGE26 routines a composition replaces by their transcriptions */
struct composition {
  int add;      /* add_galaxies_together */
  int bh;       /* grow_black_hole */
  int wind;     /* quasar_mode_wind (inside grow_black_hole) */
  int burst;    /* collisional_starburst_recipe */
  int feedback; /* update_from_feedback (inside the burst) */
};

static const struct composition OWN_ROUTINES = {0, 0, 0, 0, 0};
static const struct composition ALL_TRANSCRIBED = {1, 1, 1, 1, 1};

/** @brief The routines that must be transcribed for a value of the given stage to be reached */
static struct composition composition_for(const enum stage stage) {
  struct composition c = OWN_ROUTINES;
  c.add = stage == STAGE_ADD;
  c.bh = stage == STAGE_BH || stage == STAGE_WIND;
  c.wind = stage == STAGE_WIND;
  c.burst = stage == STAGE_BURST || stage == STAGE_FEEDBACK;
  c.feedback = stage == STAGE_FEEDBACK;
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
 * @brief TRANSCRIPTION of grow_black_hole, M:346-378, on R0's path (SFprescription 0, so the
 *        H2/H1 resynchronisation of M:366-372 does not run)
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

    /* M:366-372: sf_bh is 0 on R0 */
    if (run_params.SFprescription != 0 && run_params.SFprescription != 2) {
      fatal("transcribe_grow_black_hole covers SFprescription 0 only");
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
 * behavior", S:1224-1246) and SFprescription 0 (the H2/H1 clamp of S:1169-1173 does not run).
 * Its XASSERTs (S:1158-1163) are checked: a violation marks the composition out of domain, as
 * SAGE26 would abort there.
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
  if (sf_prescription_tracks_h2(run_params.SFprescription)) {
    fatal("transcribe_update_from_feedback covers SFprescription 0 only");
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

/**
 * @brief TRANSCRIPTION of collisional_starburst_recipe, M:584-832, on R0's path
 *
 * StarburstColdGasOn 1 (M:680-682), SFprescription 0, FIREmodeOn 0 (fire_scaling stays 0 and
 * is never read, M:692-703), SupernovaRecipeOn 1, SaveFullSFH 0 (M:763-768 does not run),
 * BulgeSizeOn 0 (update_instability_bulge_radius returns at once, R/src/model_misc.c:395) and
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

  /* M:706-716: SupernovaRecipeOn 1, FIREmodeOn 0 */
  double reheated_mass = run_params.FeedbackReheatingEpsilon * stars;
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

  /* M:728-752: SupernovaRecipeOn 1, FIREmodeOn 0 */
  double ejected_mass;
  l->flag[FL_BURST_EJECTION_VVIR_NONPOSITIVE] = 0;
  if (galaxies[m].Vvir > 0.0) {
    ejected_mass =
        (run_params.FeedbackEjectionEfficiency * (run_params.EtaSNcode * run_params.EnergySNcode) /
             (galaxies[m].Vvir * galaxies[m].Vvir) -
         run_params.FeedbackReheatingEpsilon) *
        stars;
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
  update_from_star_formation(m, stars, metallicity, galaxies, &run_params);

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

  /* M:798-804: sf_prescription_tracks_h2 is false on R0 */
  if (sf_prescription_tracks_h2(run_params.SFprescription)) {
    fatal("transcribe_collisional_starburst_recipe covers SFprescription 0 only");
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
      check_disk_instability(m, centralgal, ORACLE_HALONR, time, dt, step, galaxies, &run_params);
      struct record_spec watched = {MAX_GALAXIES, {0}};
      watched.blocks[m] = BLOCK_RESERVOIRS | BLOCK_HISTORIES;
      watched.blocks[centralgal] |= BLOCK_RESERVOIRS;
      l->flag[FL_BURST_RECHECK_CHANGED] = !same_recorded(before, galaxies, &watched);
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
 * @brief TRANSCRIPTION of add_galaxies_together, M:456-543, on R0's path (SaveFullSFH 0, so
 *        M:537-542 does not run; BulgeSizeOn 0, so update_instability_bulge_radius returns)
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
 * On R0 its result is output-inert: every path of deal_with_galaxy_merger that stores it in
 * MergerBulgeRadius then calls get_bulge_radius, which zeroes the radii (BulgeSizeOn 0,
 * R/src/model_misc.c:241-246). The value is recorded, never validated (README).
 */
static double transcribe_merger_remnant_radius(const struct GALAXY *g1, const struct GALAXY *g2) {
  /* M:138-142 */
  double M1 = g1->StellarMass + g1->ColdGas;
  double M2 = g2->StellarMass + g2->ColdGas;
  double M_tot = M1 + M2;

  if (M_tot <= 0.0) {
    return 0.0;
  }

  /* M:149-161 */
  double R1_disk_half = DISK_HALF_MASS_FRAC * g1->DiskScaleRadius;
  double R1_bulge_half = g1->BulgeRadius;
  double R1;

  if (g1->StellarMass + g1->ColdGas > 0) {
    double M1_disk = g1->ColdGas + (g1->StellarMass - g1->BulgeMass);
    double M1_bulge = g1->BulgeMass;
    R1 = (M1_disk * R1_disk_half + M1_bulge * R1_bulge_half) / M1;
  } else {
    R1 = 0.0;
  }

  /* M:164-174 */
  double R2_disk_half = DISK_HALF_MASS_FRAC * g2->DiskScaleRadius;
  double R2_bulge_half = g2->BulgeRadius;
  double R2;

  if (g2->StellarMass + g2->ColdGas > 0) {
    double M2_disk = g2->ColdGas + (g2->StellarMass - g2->BulgeMass);
    double M2_bulge = g2->BulgeMass;
    R2 = (M2_disk * R2_disk_half + M2_bulge * R2_bulge_half) / M2;
  } else {
    R2 = 0.0;
  }

  /* M:177-179 */
  if (R1 <= 0.0) {
    R1 = R2;
  }
  if (R2 <= 0.0) {
    R2 = R1;
  }
  if (R1 <= 0.0) {
    return 0.0;
  }

  /* M:185-199 */
  double E_init = (M1 * M1) / R1 + (M2 * M2) / R2;
  double E_orb = (M1 * M2) / (R1 + R2);
  double C_rad = COVINGTON11_C_RAD;
  double f_gas = (g1->ColdGas + g2->ColdGas) / M_tot;
  double E_rad = C_rad * E_init * f_gas;
  double E_final = E_init + E_orb + E_rad;

  /* M:202-205 */
  if (E_final <= 0.0) {
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
 * @brief TRANSCRIPTION of deal_with_galaxy_merger, M:228-333, on R0's path
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
  double new_merger_radius = transcribe_merger_remnant_radius(&galaxies[t], &galaxies[p]);
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
      const struct perturbation pt = {id, -1, up};
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
  for (int id = 0; id < CF_COUNT; id++) {
    rec->cflag_changes[id] = 0;
    if (!rec->locals.cflag_present[id]) {
      continue;
    }
    const struct perturbation pt = {-1, id, 0};
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

// ============================================================================
// ROUTINES: SAGE26'S OWN, CHECKED
// ============================================================================

static void read_merger_galaxies(const int ngal) {
  for (int i = 0; i < ngal; i++) {
    read_galaxy(i, BLOCK_MERGER_INPUT);
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
  spec.blocks[a.centralgal] = BLOCK_RESERVOIRS;
  spec.blocks[a.target] = BLOCK_RESERVOIRS | BLOCK_HISTORIES;
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  write_recorded("", rec.end, &spec);
  out_flags(&rec.locals, FL_BURST_GAS_FLOORED, FL_BURST_YIELD_STAYS_IN_DISK);
  out_values(&rec, IM_EBURST, IM_YIELD_TO_HOT);
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
  spec.blocks[a.centralgal] = BLOCK_IDENT | BLOCK_RESERVOIRS;
  spec.blocks[a.p] = BLOCK_IDENT | BLOCK_RESERVOIRS;
  spec.blocks[a.target] = BLOCK_IDENT | BLOCK_RESERVOIRS | BLOCK_HISTORIES;
  static struct checked_record rec;
  run_checked(input, &a, &spec, &rec);

  write_recorded("", rec.end, &spec);
  out_flags(&rec.locals, FL_MASS_RATIO_PATH, FL_BURST_YIELD_STAYS_IN_DISK);
  for (int id = 0; id < CF_COUNT; id++) {
    out_carried_flag(&rec, id);
  }
  out_values(&rec, IM_MI, IM_YIELD_TO_HOT);
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

static void write_pass_record(const char *prefix, const int ngal, const struct pass_galaxy *rec) {
  for (int p = 0; p < ngal; p++) {
    const struct pass_galaxy *r = &rec[p];
    char base[NAME_LEN];
    snprintf(base, sizeof(base), "intermediate.%sgalaxy[%d].", prefix, p);
    out_i32(field_name("%sconsidered", base), r->considered);
    if (!r->considered) {
      continue;
    }
    out_f64(field_name("%sdeltaT", base), r->deltaT);
    out_f64(field_name("%scurrentMvir", base), r->currentMvir);
    out_f64(field_name("%sgalaxyBaryons", base), r->galaxyBaryons);
    out_i32(field_name("%sgate_zero_baryons", base), r->gate_zero_baryons);
    out_i32(field_name("%sgate_passed", base), r->gate_passed);
    if (!r->gate_passed) {
      continue;
    }
    out_i32(field_name("%smerger_centralgal", base), r->merger_centralgal);
    out_i32(field_name("%sredirected", base), r->redirected);
    out_i32(field_name("%sclock_finite", base), r->clock_finite);
    out_i32(field_name("%saction", base), r->action);
    out_i32(field_name("%sevent_time_absent", base), r->clock_finite ? 0 : 1);
    if (r->clock_finite) {
      out_f64(field_name("%sevent_time", base), r->event_time);
    }
    out_i32(field_name("%sdt_absent", base), r->action == 1 ? 0 : 1);
    if (r->action == 1) {
      out_i32(field_name("%sstep_bin", base), r->step_bin);
      out_f64(field_name("%sdt", base), r->dt);
    }
  }
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

  struct pass_galaxy rec[MAX_GALAXIES];
  transcribe_merger_pass(&a, rec);

  for (int i = 0; i < ngal; i++) {
    write_galaxy("", i, &galaxies[i], BLOCK_MERGER_INPUT);
  }
  write_pass_record("pass.", ngal, rec);
}

/** What joining did to one galaxy (intermediate.join.galaxy[i].*) */
struct join_galaxy {
  int branch; /* JOIN_* below */
  double previousMvir, previousVvir, previousVmax;
  double new_Mvir; /* main branch only */
  int halo_grew;   /* main branch only: B:267 */
  int infall_recorded;
  int clock_estimated;
};

enum join_branch {
  JOIN_UNTOUCHED = 0,      /* Type 2 or 3 progenitor: copied, B:240 false */
  JOIN_ALREADY_MERGED = 1, /* mergeType != 0: Type 3, B:242-245 */
  JOIN_CENTRAL = 2,        /* main branch, FoF central: B:284-300 */
  JOIN_SATELLITE = 3,      /* main branch, subhalo: B:301-321 */
  JOIN_ORPHAN = 4          /* not the main branch: B:322-338 */
};

/** The arguments of one join call */
struct join_args {
  int ngal;
  int halonr;
  int first_occupied;
  int prog[MAX_GALAXIES];
};

/**
 * @brief SOURCE-REVIEWED TRANSCRIPTION of the per-galaxy block of join_galaxies_of_progenitors,
 *        B:234-345, and the central assignment, B:358-370, for one descendant halo
 *
 * The galaxies arrive as their progenitors left them (B:234 copies them in), in progenitor
 * order, each with the progenitor halo it came from (call.galaxy[i].prog) and the progenitor
 * B:181-208 selects as first_occupied (call.first_occupied: tree bookkeeping, not c06 physics,
 * taken as an input). Not transcribed: MostBoundID, Pos and Vel (B:254-259, identity data no
 * c06 routine reads), the concentration (B:291-295, ConcentrationOn 0 on R0), the array growth
 * (B:215-227) and the creation of a new galaxy when no progenitor has one (B:347-351, c03's
 * init_galaxy). The halo helpers, estimate_merging_time, get_disk_radius and get_bulge_radius
 * are SAGE26's own. B:361-362 returns -1 when two galaxies are Type 0 or 1; the oracle refuses
 * the fixture there instead.
 */
static void transcribe_join_progenitors(const struct join_args *a, struct join_galaxy *rec) {
  const int halonr = a->halonr;
  for (int ngal = 0; ngal < a->ngal; ngal++) {
    struct join_galaxy *r = &rec[ngal];
    memset(r, 0, sizeof(*r));
    r->branch = JOIN_UNTOUCHED;
    const int prog = a->prog[ngal];

    /* B:235, B:237 */
    galaxies[ngal].HaloNr = halonr;

    galaxies[ngal].dT = -1.0;

    /* B:240 */
    if (galaxies[ngal].Type == 0 || galaxies[ngal].Type == 1) {
      /* B:242-245 */
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
  }

  /* B:358-366 */
  int centralgal = -1;
  for (int i = 0; i < a->ngal; i++) {
    if (galaxies[i].Type == 0 || galaxies[i].Type == 1) {
      if (centralgal != -1) {
        fatal("two Type 0 or 1 galaxies (%d and %d) after joining: SAGE26 returns -1 (B:361-362)",
              centralgal, i);
      }
      centralgal = i;
    }
  }

  /* B:368-370 */
  for (int i = 0; i < a->ngal; i++) {
    galaxies[i].CentralGal = centralgal;
  }
}

static void write_join_record(const int ngal, const struct join_galaxy *rec) {
  for (int i = 0; i < ngal; i++) {
    const struct join_galaxy *r = &rec[i];
    char base[NAME_LEN];
    snprintf(base, sizeof(base), "intermediate.join.galaxy[%d].", i);
    out_i32(field_name("%sbranch", base), r->branch);
    if (r->branch == JOIN_UNTOUCHED || r->branch == JOIN_ALREADY_MERGED) {
      continue;
    }
    out_f64(field_name("%spreviousMvir", base), r->previousMvir);
    out_f64(field_name("%spreviousVvir", base), r->previousVvir);
    out_f64(field_name("%spreviousVmax", base), r->previousVmax);
    out_i32(field_name("%snew_Mvir_absent", base),
            r->branch == JOIN_CENTRAL || r->branch == JOIN_SATELLITE ? 0 : 1);
    if (r->branch == JOIN_CENTRAL || r->branch == JOIN_SATELLITE) {
      out_f64(field_name("%snew_Mvir", base), r->new_Mvir);
      out_i32(field_name("%shalo_grew", base), r->halo_grew);
    }
    out_i32(field_name("%sinfall_recorded", base), r->infall_recorded);
    out_i32(field_name("%sclock_estimated", base), r->clock_estimated);
  }
}

/** @brief Read the join's call inputs and parameters */
static void read_join_args(struct join_args *a, const int ngal, const int nhalos) {
  a->ngal = ngal;
  a->halonr = read_index("call.halonr", nhalos);
  a->first_occupied = read_index("call.first_occupied", nhalos);
  for (int i = 0; i < ngal; i++) {
    a->prog[i] = read_index(field_name("call.galaxy[%d].prog", i), nhalos);
  }
}

/** @brief The flags joining reads: ConcentrationOn 0 (B:291), BulgeSizeOn 0 (get_bulge_radius) */
static void read_join_flags(void) {
  run_params.ConcentrationOn = require_flag("ConcentrationOn", 0);
}

/** @brief Clock, infall and history assignment of progenitor joining (a transcription) */
static void run_join_progenitors(void) {
  read_join_flags();
  run_params.BulgeSizeOn = require_flag("BulgeSizeOn", 0);
  read_clock_params(1);
  const int nhalos = read_halo_catalogue(HALO_BASIC | HALO_JOIN);
  const int ngal = read_ngal();
  struct join_args a;
  read_join_args(&a, ngal, nhalos);
  for (int i = 0; i < ngal; i++) {
    read_galaxy(i, BLOCK_JOIN_INPUT);
  }
  check_all_fields_used();

  struct join_galaxy rec[MAX_GALAXIES];
  transcribe_join_progenitors(&a, rec);

  for (int i = 0; i < ngal; i++) {
    write_galaxy("", i, &galaxies[i], BLOCK_ALL);
  }
  write_join_record(ngal, rec);
}

/**
 * @brief Joining followed by one substep of the pass: the orphan history retention
 *        (B:252-282, B:322-337) carried into a merger in the same snapshot
 *
 * Both halves are the source-reviewed transcriptions above. The ordinary physics of the
 * substeps (B:466-517) between them is not composed: the pass sees the joined state.
 */
static void run_join_and_pass(void) {
  read_merger_config();
  read_pass_params();
  read_join_flags();
  read_clock_params(0);
  const int nhalos = read_halo_catalogue(HALO_BASIC | HALO_JOIN);
  const int ngal = read_ngal();
  struct join_args ja;
  read_join_args(&ja, ngal, nhalos);
  struct pass_args pa;
  read_pass_args(&pa, ngal);
  for (int i = 0; i < ngal; i++) {
    read_galaxy(i, BLOCK_JOIN_INPUT);
  }
  read_pass_ages(&pa);
  check_all_fields_used();

  struct join_galaxy jrec[MAX_GALAXIES];
  transcribe_join_progenitors(&ja, jrec);
  check_pass_layout(&pa);
  for (int i = 0; i < ngal; i++) {
    write_galaxy("intermediate.join.", i, &galaxies[i], BLOCK_IDENT | BLOCK_HALO | BLOCK_HISTORIES);
  }

  struct pass_galaxy prec[MAX_GALAXIES];
  transcribe_merger_pass(&pa, prec);

  for (int i = 0; i < ngal; i++) {
    write_galaxy("", i, &galaxies[i], BLOCK_MERGER_INPUT);
  }
  write_join_record(ngal, jrec);
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
