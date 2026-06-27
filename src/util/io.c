/**
 * @file    io.c
 * @brief   Utility functions for file I/O operations
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

/**
 * @brief   Create a single directory component if required
 *
 * @param   path        Directory path to check or create
 * @return  0 on success, non-zero on error
 */
static int ensure_directory_component(const char *path) {
  struct stat st;

  if (path == NULL || path[0] == '\0' || strcmp(path, ".") == 0) {
    return 0;
  }

  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode)) {
      return 0;
    }

    ERROR_LOG("Path exists but is not a directory: %s", path);
    return 1;
  }

  if (errno != ENOENT) {
    ERROR_LOG("Failed to inspect directory '%s': %s", path, strerror(errno));
    return 2;
  }

  if (mkdir(path, 0777) != 0 && errno != EEXIST) {
    ERROR_LOG("Failed to create directory '%s': %s", path, strerror(errno));
    return 3;
  }

  return 0;
}

/**
 * @brief   Ensure a directory path exists, creating parent directories as needed
 *
 * @param   path        Directory path to create
 * @return  0 on success, non-zero on error
 */
int ensure_directory_exists(const char *path) {
  char *path_copy;
  char *cursor;
  int status = 0;
  size_t len;

  if (path == NULL || path[0] == '\0') {
    ERROR_LOG("Cannot create an empty directory path");
    return 1;
  }

  len = strlen(path);
  path_copy = malloc(len + 1);
  if (path_copy == NULL) {
    ERROR_LOG("Failed to allocate memory for directory creation");
    return 2;
  }

  memcpy(path_copy, path, len + 1);

  while (len > 1 && path_copy[len - 1] == '/') {
    path_copy[len - 1] = '\0';
    len--;
  }

  for (cursor = path_copy + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') {
      continue;
    }

    *cursor = '\0';
    status = ensure_directory_component(path_copy);
    *cursor = '/';

    if (status != 0) {
      free(path_copy);
      return status;
    }
  }

  status = ensure_directory_component(path_copy);
  free(path_copy);
  return status;
}
