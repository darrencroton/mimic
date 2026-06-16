#ifndef IO_TREE_CTREES_COMPAT_H
#define IO_TREE_CTREES_COMPAT_H

/**
 * @file    tree/ctrees/ctrees_compat.h
 * @brief   Mimic-facing adapter for the vendored Consistent-Trees readers.
 *
 * The Consistent-Trees support code (ctrees_utils, forest_utils, parse_ctrees)
 * is ported from sage-model with minimal edits so it stays easy to re-sync with
 * upstream. This header is the single seam between that vendored code and Mimic:
 * it supplies the handful of types and macros the vendored code expects, mapped
 * onto Mimic conventions.
 *
 * Design notes:
 * - The topology code operates on a fixed `struct halo_data` (the L-Halo-tree
 *   in-memory record), NOT on the per-simulation generated `struct RawHalo`.
 *   Field names such as `Mvir` differ between simulation catalogs, and these
 *   files are compiled in every build, so they must not depend on a particular
 *   catalog's generated layout. The reader bridges `halo_data` -> `RawHalo` for
 *   the consistent-trees simulation package (see read_ctrees_*.c).
 * - XRETURN stays a return-code macro so the vendored error-propagation
 *   structure is preserved verbatim; the reader boundary turns a failed return
 *   into a Mimic FATAL_ERROR.
 */

#include <stdint.h>
#include <stdio.h>

#include "constants.h" /* MAX_STRING_LEN — reused so there is one source of truth */

/* The L-Halo-tree in-memory halo record (mirrors sage-model core_simulation.h,
   which is byte-compatible with Mimic's generated RawHalo field set). The
   vendored topology code reads and writes these fields by name. */
struct halo_data {
  /* merger tree pointers */
  int Descendant;
  int FirstProgenitor;
  int NextProgenitor;
  int FirstHaloInFOFgroup;
  int NextHaloInFOFgroup;

  /* properties of halo */
  int Len;
  float M_Mean200;
  union {
    float Mvir;
    float M200c; /* for Millennium, Mvir == M_Crit200 */
  };
  float M_TopHat;
  float Pos[3];
  float Vel[3];
  float VelDisp;
  float Vmax;
  float Spin[3];
  long long MostBoundID; /* most-bound particle ID, or a unique halo ID */

  /* original position in the simulation tree files */
  int SnapNum;
  int FileNr;
  int SubhaloIndex;
  float SubHalfMass;
};

/* Error codes returned by the vendored functions. Values are arbitrary
   non-zero (POSIX reserves 0 for success); they are only ever compared against
   EXIT_SUCCESS by callers, then surfaced as a FATAL_ERROR at the reader seam.
   Only the codes the topology helpers actually return are defined here;
   allocation-failure codes are unnecessary because Mimic's mymalloc/myrealloc
   abort internally rather than returning NULL. */
enum ctrees_error_types {
  CTREES_FILE_NOT_FOUND = 1 << 12,
  CTREES_INVALID_VALUE_READ_FROM_FILE,
};

/* Names used unprefixed by the vendored code (kept identical to upstream). */
#define FILE_NOT_FOUND CTREES_FILE_NOT_FOUND
#define INVALID_VALUE_READ_FROM_FILE CTREES_INVALID_VALUE_READ_FROM_FILE

/* Forest load-balancing cost schemes (mirrors sage-model core_allvars.h). */
enum Valid_Forest_Distribution_Schemes {
  uniform_in_forests = 0,      /* every forest has equal cost */
  linear_in_nhalos = 1,        /* cost = nhalos */
  quadratic_in_nhalos = 2,     /* cost = nhalos^2 */
  exponent_in_nhalos = 3,      /* cost = nhalos^exponent */
  generic_power_in_nhalos = 4, /* cost = pow(nhalos, exponent) */
  num_forest_weight_types
};

/* Guard-and-return on a failed expression, logging context to stderr. Mirrors
   sage-model macros.h XRETURN: the vendored functions return error codes that
   the reader boundary converts to FATAL_ERROR. */
#define XRETURN(EXP, VAL, ...)                                                                     \
  do {                                                                                             \
    if (!(EXP)) {                                                                                  \
      fprintf(stderr, "Error in file: %s\tfunc: %s\tline: %d with expression `" #EXP "'\n",        \
              __FILE__, __func__, __LINE__);                                                       \
      fprintf(stderr, __VA_ARGS__);                                                                \
      return VAL;                                                                                  \
    }                                                                                              \
  } while (0)

/* parse_ctrees.h uses the name XASSERT for the same guard-and-return contract
   (upstream sage distinguishes XASSERT=abort from XRETURN=return; the vendored
   parser only ever uses it to return an error code, so the two coincide here). */
#define XASSERT(EXP, VAL, ...) XRETURN(EXP, VAL, __VA_ARGS__)

#endif /* IO_TREE_CTREES_COMPAT_H */
