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

/* Define the magic number for MIMIC binary files */
#define MIMIC_MAGIC_NUMBER 0x4D494D49 /* "MIMI" in ASCII (4 bytes for uint32_t) */

/* Current binary file format version */
#define MIMIC_FILE_VERSION 1

/* File header structure for binary files */
struct MimicFileHeader {
  uint32_t magic;     /* Magic number for identification (MIMIC_MAGIC_NUMBER) */
  uint8_t version;    /* File format version */
  uint8_t endianness; /* File endianness (0=little, 1=big) */
  uint16_t reserved;  /* Reserved for future use */
};

/* Function prototypes for endianness conversion */
uint16_t swap_uint16(uint16_t value);
uint32_t swap_uint32(uint32_t value);
uint64_t swap_uint64(uint64_t value);
int16_t swap_int16(int16_t value);
int32_t swap_int32(int32_t value);
int64_t swap_int64(int64_t value);
float swap_float(float value);
double swap_double(double value);

/* Endianness utilities */
int is_same_endian(int file_endian);
void *swap_bytes_if_needed(void *data, size_t size, size_t count,
                           int file_endian);

/* File format utilities */
int write_mimic_header(FILE *file, int endianness);
int read_mimic_header(FILE *file, struct MimicFileHeader *header);
int check_file_compatibility(const struct MimicFileHeader *header);
int check_headerless_file(FILE *file);
long get_file_size(FILE *file);

#endif /* IO_UTIL_H */