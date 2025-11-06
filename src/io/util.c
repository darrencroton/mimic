/**
 * @file    io_util.c
 * @brief   Implementation of I/O utility functions
 *
 * This file implements utilities for handling input/output operations,
 * including endianness detection and conversion. These utilities ensure
 * consistent cross-platform compatibility for binary file formats.
 */

#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "error.h"

/**
 * @brief   Detects the host system's endianness at runtime
 *
 * @return  MIMIC_BIG_ENDIAN or MIMIC_LITTLE_ENDIAN
 *
 * This function is used as a fallback when compile-time detection
 * is not possible. It determines the endianness by examining how a
 * multi-byte integer is stored in memory.
 */
int detect_host_endian(void) {
  union {
    uint32_t i;
    char c[4];
  } test = {0x01020304};

  return (test.c[0] == 1) ? MIMIC_BIG_ENDIAN : MIMIC_LITTLE_ENDIAN;
}

/**
 * @brief   Checks if the file endianness matches the host endianness
 *
 * @param   file_endian   The endianness of the file (MIMIC_LITTLE_ENDIAN or
 * MIMIC_BIG_ENDIAN)
 * @return  1 if same endianness, 0 if different
 */
int is_same_endian(int file_endian) { return file_endian == MIMIC_HOST_ENDIAN; }

/**
 * @brief   Swaps bytes in memory if host and file endianness differ
 *
 * @param   data         Pointer to the data to swap
 * @param   size         Size of each element in bytes
 * @param   count        Number of elements
 * @param   file_endian  Endianness of the file
 * @return  Pointer to the input data (modified in place)
 *
 * This function checks if byte swapping is needed and performs it if necessary.
 * It supports common data sizes (2, 4, and 8 bytes). The data is modified in
 * place.
 */
void *swap_bytes_if_needed(void *data, size_t size, size_t count,
                           int file_endian) {
  /* Return early if no conversion is needed */
  if (is_same_endian(file_endian) || data == NULL || count == 0) {
    return data;
  }

  /* Cast to byte array for easier manipulation */
  unsigned char *bytes = (unsigned char *)data;
  unsigned char tmp;
  size_t i, j;

  /* Swap bytes based on element size */
  switch (size) {
  case 2: /* 16-bit values */
    for (i = 0; i < count; i++) {
      tmp = bytes[i * 2];
      bytes[i * 2] = bytes[i * 2 + 1];
      bytes[i * 2 + 1] = tmp;
    }
    break;

  case 4: /* 32-bit values */
    for (i = 0; i < count; i++) {
      for (j = 0; j < 2; j++) {
        tmp = bytes[i * 4 + j];
        bytes[i * 4 + j] = bytes[i * 4 + 3 - j];
        bytes[i * 4 + 3 - j] = tmp;
      }
    }
    break;

  case 8: /* 64-bit values */
    for (i = 0; i < count; i++) {
      for (j = 0; j < 4; j++) {
        tmp = bytes[i * 8 + j];
        bytes[i * 8 + j] = bytes[i * 8 + 7 - j];
        bytes[i * 8 + 7 - j] = tmp;
      }
    }
    break;

  default:
    WARNING_LOG("Unsupported element size for byte swapping: %zu bytes", size);
    break;
  }

  return data;
}
