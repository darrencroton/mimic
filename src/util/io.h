/**
 * @file    io.h
 * @brief   Utility functions for file I/O operations
 */

#ifndef UTIL_IO_H
#define UTIL_IO_H

/**
 * @brief   Copy a file from source to destination
 *
 * @param   source      Source file path
 * @param   dest        Destination file path
 * @return  0 on success, non-zero on error
 */
int copy_file(const char *source, const char *dest);

/**
 * @brief   Ensure a directory path exists, creating parent directories as needed
 *
 * @param   path        Directory path to create
 * @return  0 on success, non-zero on error
 */
int ensure_directory_exists(const char *path);

#endif // UTIL_IO_H
