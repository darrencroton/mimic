/**
 * @file    io_util.h
 * @brief   Utility functions for I/O operations
 *
 * This file provides utilities for handling input/output operations,
 * including endianness detection and conversion for cross-platform
 * compatibility. These utilities ensure consistent binary file format
 * handling across different architectures.
 */

#ifndef IO_UTIL_H
#define IO_UTIL_H

#include <stdint.h>
#include <stdio.h>

/* Endianness definitions */
#define MIMIC_LITTLE_ENDIAN 0
#define MIMIC_BIG_ENDIAN 1

/* Determine host endianness at compile time if possible */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define MIMIC_HOST_ENDIAN MIMIC_LITTLE_ENDIAN
#elif defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define MIMIC_HOST_ENDIAN MIMIC_BIG_ENDIAN
#elif defined(__LITTLE_ENDIAN__)
#define MIMIC_HOST_ENDIAN MIMIC_LITTLE_ENDIAN
#elif defined(__BIG_ENDIAN__)
#define MIMIC_HOST_ENDIAN MIMIC_BIG_ENDIAN
#else
/* Runtime detection as fallback - implemented in io_util.c */
int detect_host_endian(void);
#define MIMIC_HOST_ENDIAN detect_host_endian()
#endif

/* Endianness utilities */
int is_same_endian(int file_endian);
void *swap_bytes_if_needed(void *data, size_t size, size_t count,
                           int file_endian);

#endif /* IO_UTIL_H */