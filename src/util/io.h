/**
 * @file    io.h
 * @brief   Utility functions for file I/O operations
 * @author  Mimic Development Team
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

#endif  // UTIL_IO_H
