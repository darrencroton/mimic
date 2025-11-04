/**
 * @file    io.c
 * @brief   Utility functions for file I/O operations
 * @author  Mimic Development Team
 */

#include <stdio.h>
#include <stdlib.h>
#include "io.h"
#include "error.h"

/**
 * @brief   Copy a file from source to destination
 *
 * @param   source      Source file path
 * @param   dest        Destination file path
 * @return  0 on success, non-zero on error
 */
int copy_file(const char *source, const char *dest) {
  FILE *src, *dst;
  char buffer[8192];
  size_t bytes;

  src = fopen(source, "rb");
  if (!src) {
    ERROR_LOG("Failed to open source file: %s", source);
    return 1;
  }

  dst = fopen(dest, "wb");
  if (!dst) {
    ERROR_LOG("Failed to open destination file: %s", dest);
    fclose(src);
    return 2;
  }

  while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
    if (fwrite(buffer, 1, bytes, dst) != bytes) {
      ERROR_LOG("Error writing to destination file: %s", dest);
      fclose(src);
      fclose(dst);
      return 3;
    }
  }

  fclose(src);
  fclose(dst);
  return 0;
}
