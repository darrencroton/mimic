#ifndef IO_TREE_CTREES_PARSE_CTREES_H
#define IO_TREE_CTREES_PARSE_CTREES_H

/* File: parse_ctrees.h */
/*
  This file is a part of the ``parse_ctrees`` package
  Copyright (C) 2018-- Manodeep Sinha (manodeep@gmail.com)
  License: MIT LICENSE. See LICENSE file under the top-level
  directory at https://github.com/manodeep/parse_ctrees/
*/

/*
  Vendored into Mimic from sage-model (io/parse_ctrees.h) with minimal edits:
  the include paths point at Mimic's third_party/sglib.h and the ctrees_compat.h
  seam; heap scratch uses Mimic's categorised allocator (MEM_IO) and base-pointer
  growth uses myrealloc (which preserves the caller's category). The upstream
  allocation NULL-checks are dropped because Mimic's allocator aborts on failure
  rather than returning NULL. Header-only: all functions are `static inline`.
*/

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stddef.h> /* for offsetof macro*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "error.h"
#include "memory.h"
#include "third_party/sglib.h"
#include "tree/ctrees/ctrees_compat.h"

/* this is the maximum number of CTREES columns that can be requested
   (note: it is okay for the ctrees `tree_?_?_?.dat` files themselves to contain more columns) */
#define PARSE_CTREES_MAX_NCOLS 128

/* max. number of characters in a column name */
#define PARSE_CTREES_MAX_COLNAME_LEN 64

/* max. number of expected characters in one single-line */
#define PARSE_CTREES_MAXBUFSIZE 1240

#if PARSE_CTREES_MAX_COLNAME_LEN < 64
#error Some of the Consistent-Trees column names are long. Please increase PARSE_CTREES_MAX_COLNAME_LEN to be at least 64
#endif

#define LAST_NUMBERED_COLUMN_IN_CTREES 34

/* Function-like macros */
#ifdef NDEBUG
#define PARSE_CTREES_XASSERT(EXP, EXIT_STATUS, ...)                                                \
  do {                                                                                             \
  } while (0)
#else
#define PARSE_CTREES_XASSERT(EXP, EXIT_STATUS, ...)                                                \
  do {                                                                                             \
    if (!(EXP)) {                                                                                  \
      fprintf(stderr, "Error in file: %s\tfunc: %s\tline: %d with expression `" #EXP "'\n",        \
              __FILE__, __FUNCTION__, __LINE__);                                                   \
      fprintf(stderr, __VA_ARGS__);                                                                \
      return EXIT_STATUS;                                                                          \
    }                                                                                              \
  } while (0)
#endif

enum parse_numeric_types {
  I32 = 0, /* int32_t */
  I64 = 1, /* int64_t */
  U32 = 2, /* uint32_t */
  U64 = 3, /* uint64_t */
  F32 = 4, /* float */
  F64 = 5, /* double */
  num_numeric_types
};

struct base_ptr_info {
  int64_t num_base_ptrs;
  void **base_ptrs[PARSE_CTREES_MAX_NCOLS]; /* because the pointers may need to be re-allocated, I
                                               need 'void **' */
  size_t base_element_size[PARSE_CTREES_MAX_NCOLS]; /* sizeof(**(base_ptrs[i])) --> in bytes */

  union {
    int64_t N;      /* number of rows read */
    int64_t nhalos; /* for convenience */
    int64_t nhalos_read;
  };
  union {
    /* to remove any confusion about what nallocated refers to
       --> nallocated is the number of elements allocated for each
       element in base_ptrs (i.e., alloc'ed/re-alloc'ed via the void **base_ptrs)
     */
    int64_t nhalos_allocated; /* number of elements allocated in each `base_ptr` */
    int64_t nallocated;
  };
};

/* By storing the struct elements on the stack,
   I avoid pesky issues of malloc/free */
struct ctrees_column_to_ptr {
  int64_t ncols;                                                /* number of columns requested */
  int32_t column_number[PARSE_CTREES_MAX_NCOLS];                /* column number in CTREES data */
  enum parse_numeric_types field_types[PARSE_CTREES_MAX_NCOLS]; /* destination data-type, i.e, how
                                                                   to parse the string into a valid
                                                                   numeric value */

  /* For array-of-structure type base-ptrs, this is the offsetof(field);
     for structure of arrays, this should be 0

     this offset must be >= 0 and < size of each element of the base ptr
     For offset values that are not 0, absolutely use the OFFSETOF macro
     to derive the byte offset of each field */
  size_t dest_offset_to_element[PARSE_CTREES_MAX_NCOLS]; /* in bytes */
  int64_t base_ptr_idx[PARSE_CTREES_MAX_NCOLS];          /* index into the base_ptr array within
                                                            base_ptr_info          struct */
};

static inline int *match_column_name(const char (*wanted_columns)[PARSE_CTREES_MAX_COLNAME_LEN],
                                     const int nwanted,
                                     const char (*names)[PARSE_CTREES_MAX_COLNAME_LEN],
                                     const int totncols) {
  int *columns = mymalloc_cat(nwanted * sizeof(*columns), MEM_IO);
  for (int i = 0; i < nwanted; i++) {
    columns[i] = -1;
  }
  int nfound = 0;
  for (int i = 0; i < nwanted; i++) {
    const char *wanted_colname = wanted_columns[i];
    int found = 0;
    for (int j = 0; j < totncols; j++) {
      const char *file_colname = names[j];
      if (strcasecmp(wanted_colname, file_colname) == 0) {
        DEBUG_LOG("Found `%s` in column # %d as with name `%s`", wanted_colname, j, file_colname);
        columns[i] = j;
        nfound++;
        found = 1;
        break;
      }
    }
    if (found == 0) {
      DEBUG_LOG("Did not find requested column `%s'", wanted_colname);
    }
  }
  DEBUG_LOG("Found %d columns out of the requested %d", nfound, nwanted);
  return columns;
}

static inline int reallocate_base_ptrs(struct base_ptr_info *base_info, const int64_t new_N) {
  for (int64_t i = 0; i < base_info->num_base_ptrs; i++) {
    void **this_ptr = base_info->base_ptrs[i];
    const size_t size = base_info->base_element_size[i];
    /* myrealloc preserves the block's category and aborts on failure */
    *(base_info->base_ptrs[i]) = myrealloc(*this_ptr, size * new_N);
  }
  base_info->nallocated = new_N;
  return EXIT_SUCCESS;
}

static inline int parse_ctrees_token_to_dest(const char *token, const enum parse_numeric_types type,
                                             void *dest) {
  char *endptr = NULL;
  errno = 0;

  switch (type) {
  case F32: {
    const float tmp = strtof(token, &endptr);
    if (endptr == token || *endptr != '\0' || errno == ERANGE || !isfinite(tmp)) {
      fprintf(stderr, "Error: token `%s` is not a finite float32 value\n", token);
      return EXIT_FAILURE;
    }
    *((float *)dest) = tmp;
    return EXIT_SUCCESS;
  }
  case F64: {
    const double tmp = strtod(token, &endptr);
    if (endptr == token || *endptr != '\0' || errno == ERANGE || !isfinite(tmp)) {
      fprintf(stderr, "Error: token `%s` is not a finite float64 value\n", token);
      return EXIT_FAILURE;
    }
    *((double *)dest) = tmp;
    return EXIT_SUCCESS;
  }
  case I32: {
    const long long tmp = strtoll(token, &endptr, 10);
    if (endptr == token || *endptr != '\0' || errno == ERANGE || tmp < INT32_MIN ||
        tmp > INT32_MAX) {
      fprintf(stderr, "Error: token `%s` is not an int32 value\n", token);
      return EXIT_FAILURE;
    }
    *((int32_t *)dest) = (int32_t)tmp;
    return EXIT_SUCCESS;
  }
  case I64: {
    const long long tmp = strtoll(token, &endptr, 10);
    if (endptr == token || *endptr != '\0' || errno == ERANGE) {
      fprintf(stderr, "Error: token `%s` is not an int64 value\n", token);
      return EXIT_FAILURE;
    }
    *((int64_t *)dest) = (int64_t)tmp;
    return EXIT_SUCCESS;
  }
  default:
    fprintf(stderr, "Error: Unknown value for parse type = %d\n", type);
    fprintf(stderr, "Known values are in the range : [%d, %d)\n", I32, num_numeric_types);
    return EXIT_FAILURE;
  }
}

static inline int parse_header_ctrees(char (*column_names)[PARSE_CTREES_MAX_COLNAME_LEN],
                                      enum parse_numeric_types *field_types, int64_t *base_ptr_idx,
                                      size_t *dest_offset_to_element, const int64_t nfields,
                                      const char *filename,
                                      struct ctrees_column_to_ptr *column_info) {
  /* Because the struct elements (of column_info) are stored on the stack,
     need to check that nfields can fit */
  if (nfields > PARSE_CTREES_MAX_NCOLS) {
    fprintf(stderr,
            "Error: You have requested %" PRId64
            " columns but there is only space to store %" PRId64 "\n",
            nfields, (int64_t)PARSE_CTREES_MAX_NCOLS);
    fprintf(stderr,
            "Please define the macro variable `PARSE_CTREES_MAX_NCOLS' to be larger than %" PRId64
            " (before including the file `%s')\n",
            nfields, __FILE__);

    return EXIT_FAILURE;
  }

  FILE *fp = fopen(filename, "rt");
  if (fp == NULL) {
    fprintf(stderr, "Error: Could not open file `%s'\n", filename);
    perror(NULL);
    return EXIT_FAILURE;
  }

  char linebuf[PARSE_CTREES_MAXBUFSIZE];
  if (fgets(linebuf, PARSE_CTREES_MAXBUFSIZE, fp) != NULL) {

    /* only need the first line -> close the file */
    fclose(fp);

    /* first check that the first character is a '#' */
    if (linebuf[0] != '#') {
      fprintf(stderr,
              "Error: Consistent-Trees output always contain '#' as the comment character\n"
              "However, the first character in the buffer is '%c'\nEntire line is `%s'",
              linebuf[0], linebuf);
      return EXIT_FAILURE;
    }
    char *tofree, *string;

    tofree = string = strdup(linebuf);
    PARSE_CTREES_XASSERT(string != NULL, EXIT_FAILURE,
                         "Error: Could not duplicate the header line (header: = `%s`\n)", linebuf);

    int totncols = 0;
    char *token = NULL;

    /* consistent-trees currently uses white-space */
    while ((token = strsep(&string, " ,")) != NULL) {
      totncols++;
    }
    free(tofree);

    /* read succeeded -> now parse the column names */
    const char delimiters[] = " ,\n#"; /* space, comma, new-line, and #*/
    char (*names)[PARSE_CTREES_MAX_COLNAME_LEN] = mymalloc_cat(totncols * sizeof(names[0]), MEM_IO);
    memset(names, 0, totncols * sizeof(names[0]));

    tofree = string = strdup(linebuf);
    PARSE_CTREES_XASSERT(string != NULL, EXIT_FAILURE,
                         "Error: Could not duplicate the header line (header: = `%s`\n)", linebuf);

    int col = 0;
    while ((token = strsep(&string, delimiters)) != NULL) {
      size_t size = 0, totlen = strlen(token);
      if (totlen == 0)
        continue;
      PARSE_CTREES_XASSERT(totlen > 0 && totlen < PARSE_CTREES_MAX_COLNAME_LEN, EXIT_FAILURE,
                           "totlen = %zu should be between (0, %d)\n", totlen,
                           (int)PARSE_CTREES_MAX_COLNAME_LEN);
      char *colname = names[col];
      for (size_t i = 0; i < totlen; i++) {
        if (token[i] == '(') {
          break;
        }
        colname[size] = token[i];
        size++;
      }
      colname[size] = '\0';
      col++;
    }
    PARSE_CTREES_XASSERT(
        col == totncols, EXIT_FAILURE,
        "Error: Previous parsing indicated %d columns in the header but only found %d actual "
        "column "
        "names\n"
        "Please check that the delimiters spefied to `strsep` are the same in all calls\n",
        totncols, col);
    free(tofree);

    int *matched_columns =
        match_column_name((const char (*)[PARSE_CTREES_MAX_COLNAME_LEN])column_names, nfields,
                          (const char (*)[PARSE_CTREES_MAX_COLNAME_LEN])names, totncols);

    /* do not need the actual names of every column in the ctrees file any longer -> free that
     * memory */
    myfree(names);

    /* now sort the matched columns */
#define SGLIB_CHAR_ARRAY_ELEMENTS_EXCHANGER(maxlen, a, i, j)                                       \
  {                                                                                                \
    char _sgl_aee_tmp_[maxlen];                                                                    \
    memmove(_sgl_aee_tmp_, (a)[(i)], maxlen);                                                      \
    memmove((a)[(i)], (a)[(j)], maxlen);                                                           \
    memmove((a)[(j)], _sgl_aee_tmp_, maxlen);                                                      \
  }

    /* sort the matched_columns in ascending order */
#define MULTIPLE_ARRAY_EXCHANGER(type, a, i, j)                                                    \
  {                                                                                                \
    SGLIB_ARRAY_ELEMENTS_EXCHANGER(enum parse_numeric_types, field_types, i, j);                   \
    SGLIB_ARRAY_ELEMENTS_EXCHANGER(int, matched_columns, i, j);                                    \
    SGLIB_ARRAY_ELEMENTS_EXCHANGER(int64_t, base_ptr_idx, i, j);                                   \
    SGLIB_ARRAY_ELEMENTS_EXCHANGER(size_t, dest_offset_to_element, i, j);                          \
    SGLIB_CHAR_ARRAY_ELEMENTS_EXCHANGER(PARSE_CTREES_MAX_COLNAME_LEN, column_names, i, j);         \
  }

    SGLIB_ARRAY_QUICK_SORT(int, matched_columns, nfields, SGLIB_NUMERIC_COMPARATOR,
                           MULTIPLE_ARRAY_EXCHANGER);
#undef SGLIB_CHAR_ARRAY_ELEMENTS_EXCHANGER
#undef MULTIPLE_ARRAY_EXCHANGER

    /* now assign only the columns that were found
       into the column_info struct */
    column_info->ncols = 0;
    for (int i = 0; i < nfields; i++) {
      if (matched_columns[i] == -1)
        continue;

      const int icol = column_info->ncols;
      column_info->column_number[icol] = matched_columns[i];
      column_info->field_types[icol] = field_types[i];
      column_info->dest_offset_to_element[icol] = dest_offset_to_element[i];
      column_info->base_ptr_idx[icol] = base_ptr_idx[i];
      column_info->ncols++;
    }
    myfree(matched_columns);
  } else {
    fprintf(stderr, "Error: Could not read the first line (the header) in the file `%s'\n",
            filename);
    perror(NULL);
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

static inline int parse_line_ctrees(const char *linebuf,
                                    const struct ctrees_column_to_ptr *column_info,
                                    struct base_ptr_info *base_ptr_info) {
  if (base_ptr_info->nallocated == base_ptr_info->N) {
    const double large_N_memory_increase_fac = 1.2;
    const int64_t small_N_memory_increase_fac = 2;
    /* double (:=`small_N_memory_increase_fac`) the memory requested for small numbers, otherwise
     * increase by `large_N_memory_increase_fac` */
    const int64_t thresh_N_for_large_memory =
        1000000; /* small_N_memory_increase_fac * N, for N less than this threshold*/
    const int64_t new_N = (base_ptr_info->N < thresh_N_for_large_memory)
                              ? (base_ptr_info->N * small_N_memory_increase_fac)
                              : (base_ptr_info->N * large_N_memory_increase_fac);
    int status = reallocate_base_ptrs(base_ptr_info, new_N);
    if (status != EXIT_SUCCESS)
      return status;
    XASSERT(base_ptr_info->nallocated > base_ptr_info->N, EXIT_FAILURE,
            "Error: Something went wrong while memory reallocation "
            "nallocated = %" PRId64 " should have been larger than N = %" PRId64 "\n",
            base_ptr_info->nallocated, base_ptr_info->N);
  }

  int icol = -1;
  char *tofree, *string;
  tofree = string = strdup(linebuf);
  if (string == NULL) {
    fprintf(stderr, "Error: Could not duplicate ctrees data line `%s`\n", linebuf);
    return EXIT_FAILURE;
  }
  char *token = NULL;
  for (int i = 0; i < column_info->ncols; i++) {
    const int wanted_col = column_info->column_number[i];
    const int64_t base_ptr_idx = column_info->base_ptr_idx[i];
    PARSE_CTREES_XASSERT(base_ptr_idx < base_ptr_info->num_base_ptrs, EXIT_FAILURE,
                         "Error: Valid values for base pointer index must be in range [0, %" PRId64
                         "). Got %" PRId64 " instead\n",
                         base_ptr_info->num_base_ptrs, base_ptr_idx);
    char *dest = *((char **)(base_ptr_info->base_ptrs[base_ptr_idx]));
    const size_t base_ptr_stride = base_ptr_info->base_element_size[base_ptr_idx];
    const size_t dest_offset = column_info->dest_offset_to_element[i];
    PARSE_CTREES_XASSERT(base_ptr_stride >= 4, EXIT_FAILURE,
                         "Error: Stride=%zu is expected in bytes with a minimum of 4 bytes since "
                         "that's "
                         "the smallest data-type supported (corresponding to float or int32_t).\n"
                         "Perhaps you forgot to multiply by the sizeof(element)?\n",
                         base_ptr_stride);
    PARSE_CTREES_XASSERT(dest_offset <= base_ptr_stride, EXIT_FAILURE,
                         "Error: The offset from the starting address of an element can be at most "
                         "the total stride in bytes\n"
                         "In this case offset=%zu must in the closed range [0, %zu]. Perhaps you "
                         "mis-typed the offset?\n",
                         dest_offset, base_ptr_stride);

    /* get to the starting offset for this N'th element */
    dest += base_ptr_info->N * base_ptr_stride;

    /* now get to this particular field */
    dest += dest_offset;

    /* this is the type for the destination (hence called 'field_types' rather than 'column_types')
     */
    const enum parse_numeric_types wanted_type = column_info->field_types[i];

    /* there might be duplicate column numbers in matched_columns
       then the following while loop should immediately exit (without
       executing any lines within)
       and we will re-use the previous parsed value of token */
    while (icol < wanted_col || token == NULL) {
      token = strsep(&string, " ,");
      if (token != NULL && ((token[0] == '\0') || (token[0] == ' '))) {
        continue;
      }
      icol++;
    }
    if (token == NULL || token[0] == '\0' || icol != wanted_col) {
      fprintf(stderr,
              "Error: expected a numeric token for requested column %d but parsed column %d in "
              "line `%s`\n",
              wanted_col, icol, linebuf);
      free(tofree);
      return EXIT_FAILURE;
    }

    if (parse_ctrees_token_to_dest(token, wanted_type, dest) != EXIT_SUCCESS) {
      free(tofree);
      return EXIT_FAILURE;
    }
  }
  free(tofree);

  base_ptr_info->N++;

  return EXIT_SUCCESS;
}

static inline int read_single_tree_ctrees(int fd, off_t offset,
                                          const struct ctrees_column_to_ptr *column_info,
                                          struct base_ptr_info *base_ptr_info) {
  /* Because the struct elements (of column_info) are stored on the stack,
     need to check that nfields can fit */
  if (column_info->ncols > PARSE_CTREES_MAX_NCOLS) {
    fprintf(stderr,
            "Error: You have requested %" PRId64
            " columns but there is only space to store %" PRId64 "\n",
            column_info->ncols, (int64_t)PARSE_CTREES_MAX_NCOLS);
    fprintf(stderr,
            "Please define the macro variable `PARSE_CTREES_MAX_NCOLS' to be larger than %" PRId64
            " (before including the file `%s')\n",
            column_info->ncols, __FILE__);

    return EXIT_FAILURE;
  }

  char read_buffer[PARSE_CTREES_MAXBUFSIZE];
  const size_t to_read_bytes = PARSE_CTREES_MAXBUFSIZE - 1;
  read_buffer[PARSE_CTREES_MAXBUFSIZE - 1] = '\0';
  int done_reading_tree = 0;
  /* two things can happen while reading -> EOF or I reach the next tree */
  while (done_reading_tree == 0) {
    ssize_t nbytes_read = pread(fd, read_buffer, to_read_bytes, offset);
    if (nbytes_read == 0) {
      done_reading_tree = 1; /* we have reached end of file */
    } else if (nbytes_read < 0) {
      fprintf(
          stderr,
          "Error: trying to read %zu bytes from file failed. Encountered negative bytes read \n",
          to_read_bytes);
      perror(NULL);
      return EXIT_FAILURE;
    } else {
      const off_t start_offset = offset;
      if (read_buffer[0] == '#') {
        done_reading_tree = 1;
        break;
      }
      if (nbytes_read < (ssize_t)to_read_bytes) {
        done_reading_tree = 1; /* we have reached end of file but this read buffer needs to be
                                  processed*/
      }

      /* some bytes were read -> now let's parse one line at a time */
      char *start = &(read_buffer[0]);
      const char *end = read_buffer + nbytes_read;
      char *this = start;
      while (this < end) {
        if (*this == '\n') {
          *this = '\0';

          if (!(this >= start && (this - start) < PARSE_CTREES_MAXBUFSIZE)) {
            fprintf(stderr,
                    "Error: Expected this = %p to be >= start = %p "
                    "and (this - start) = %td to be less than PARSE_CTREES_MAXBUFSIZE = %d\n",
                    (void *)this, (void *)start, this - start, PARSE_CTREES_MAXBUFSIZE);
            return EXIT_FAILURE;
          }

          char linebuf[PARSE_CTREES_MAXBUFSIZE];
          memmove(linebuf, start, this - start + 1);

          offset += (this - start + 1);
          start = this + 1; /* might point beyond valid memory but should not get de-referenced */

          int status = parse_line_ctrees(linebuf, column_info, base_ptr_info);
          if (status != EXIT_SUCCESS) {
            return status;
          }
        }
        this ++;
        if (this < end && *this == '#') {
          /* we have encountered the beginning of a new tree (new line and begins with '#tree ')*/
          done_reading_tree = 1;
          this = (char *)end; /* casting, otherwise compiler complains */
          break;
        }
      }
      PARSE_CTREES_XASSERT(offset - start_offset <= nbytes_read, EXIT_FAILURE,
                           "Error: bytes processed = %" PRId64
                           " should be at most num bytes read = "
                           "%zd\n",
                           offset - start_offset, nbytes_read);
    }
  }

  return EXIT_SUCCESS;
}

/* these macros are for internal use only
   and can therefor be undefined */
#undef PARSE_CTREES_MAXBUFSIZE
#undef PARSE_CTREES_XASSERT
#undef LAST_NUMBERED_COLUMN_IN_CTREES

#endif /* IO_TREE_CTREES_PARSE_CTREES_H */
