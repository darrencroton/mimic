#ifndef ERROR_HANDLING_H
#define ERROR_HANDLING_H

#include <stdio.h>

/**
 * Error severity levels
 */
typedef enum {
  LOG_LEVEL_DEBUG,   // Detailed debugging information (function tracing, variable
                     // values, etc.)
  LOG_LEVEL_INFO,    // General informational messages (processing milestones,
                     // configuration info)
  LOG_LEVEL_WARNING, // Warnings that don't stop execution (unusual but
                     // acceptable conditions)
  LOG_LEVEL_ERROR,   // Recoverable errors (operation failed but program can
                     // continue)
  LOG_LEVEL_FATAL    // Unrecoverable errors that terminate execution (critical
                     // failures)
} LogLevel;

/**
 * I/O-specific error codes
 */
typedef enum {
  IO_ERROR_NONE = 0,              // No error
  IO_ERROR_FILE_NOT_FOUND = 1,    // File not found or couldn't be opened
  IO_ERROR_PERMISSION_DENIED = 2, // Permission denied for file operation
  IO_ERROR_READ_FAILED = 3,       // Read operation failed
  IO_ERROR_WRITE_FAILED = 4,      // Write operation failed
  IO_ERROR_SEEK_FAILED = 5,       // Seek operation failed
  IO_ERROR_INVALID_HEADER = 6,    // Invalid or corrupted file header
  IO_ERROR_VERSION_MISMATCH = 7,  // File version incompatible
  IO_ERROR_ENDIANNESS = 8,        // Endianness-related error
  IO_ERROR_FORMAT = 9,            // General file format error
  IO_ERROR_BUFFER = 10,           // Buffer management error
  IO_ERROR_EOF = 11,              // Unexpected end of file
  IO_ERROR_CLOSE_FAILED = 12,     // Failed to close file
  IO_ERROR_HDF5 = 13              // HDF5-specific error
} IOErrorCode;

// General error handling function prototypes
void initialize_error_handling(LogLevel min_level, FILE *output_file);
void log_message(LogLevel level, const char *file, const char *func, int line, const char *format,
                 ...);
void set_log_level(LogLevel min_level);
LogLevel get_log_level(void);
void set_verbose_format(int enable);
int get_verbose_format(void);
void set_verbose_prefix(int enable);
int get_verbose_prefix(void);
void enable_debug_log_rate_limiting(void);
void disable_debug_log_rate_limiting(void);
int is_debug_log_rate_limiting_enabled(void);
FILE *set_log_output(FILE *output_file);

// Both GCC and Clang define __GNUC__; expands to nothing elsewhere.
#if defined(__GNUC__)
#define MIMIC_NORETURN __attribute__((noreturn))
#else
#define MIMIC_NORETURN
#endif

// Process-exit hook invoked by the FATAL_ERROR/IO_FATAL_ERROR macros below.
// Declared here so every translation unit that uses those macros has a
// prototype without depending on proto.h (defined in src/core/main.c).
// Marked noreturn (it always calls exit) so compilers and static analysers know
// FATAL_ERROR terminates; without it they report unreachable cleanup as a
// double free and treat post-FATAL_ERROR code as live.
MIMIC_NORETURN void myexit(int signum);

// I/O-specific error handling function prototypes
const char *get_io_error_name(IOErrorCode code);
void log_io_error(LogLevel level, IOErrorCode code, const char *file, const char *func, int line,
                  const char *operation, const char *filename, const char *format, ...);

// General logging convenience macros
#define DEBUG_LOG(...)                                                                             \
  do {                                                                                             \
    if (!is_debug_log_rate_limiting_enabled()) {                                                   \
      log_message(LOG_LEVEL_DEBUG, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);                 \
    } else {                                                                                       \
      static int _debug_call_count = 0;                                                            \
      static int _debug_suppressed = 0;                                                            \
      if (_debug_call_count < DEBUG_LOG_MAX_CALLS) {                                               \
        log_message(LOG_LEVEL_DEBUG, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);               \
        _debug_call_count++;                                                                       \
      } else if (!_debug_suppressed) {                                                             \
        log_message(LOG_LEVEL_DEBUG, __FILE__, __FUNCTION__, __LINE__,                             \
                    "(further debug output from this location suppressed)");                       \
        _debug_suppressed = 1;                                                                     \
      }                                                                                            \
    }                                                                                              \
  } while (0)
#define INFO_LOG(...) log_message(LOG_LEVEL_INFO, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)
#define WARNING_LOG(...)                                                                           \
  log_message(LOG_LEVEL_WARNING, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)
#define ERROR_LOG(...) log_message(LOG_LEVEL_ERROR, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__)
#define FATAL_ERROR(...)                                                                           \
  do {                                                                                             \
    log_message(LOG_LEVEL_FATAL, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);                   \
    myexit(1);                                                                                     \
  } while (0)

// Verbose logging macro - only logs when --verbose or --debug flag is set
// Use for configuration details and initialization messages that should only
// appear in verbose mode. Always logs at INFO level.
#define VERBOSE_LOG(...)                                                                           \
  do {                                                                                             \
    if (get_verbose_format()) {                                                                    \
      log_message(LOG_LEVEL_INFO, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);                  \
    }                                                                                              \
  } while (0)

// I/O-specific logging macros
#define IO_DEBUG_LOG(code, op, filename, ...)                                                      \
  log_io_error(LOG_LEVEL_DEBUG, code, __FILE__, __FUNCTION__, __LINE__, op, filename, __VA_ARGS__)
#define IO_INFO_LOG(code, op, filename, ...)                                                       \
  log_io_error(LOG_LEVEL_INFO, code, __FILE__, __FUNCTION__, __LINE__, op, filename, __VA_ARGS__)
#define IO_WARNING_LOG(code, op, filename, ...)                                                    \
  log_io_error(LOG_LEVEL_WARNING, code, __FILE__, __FUNCTION__, __LINE__, op, filename, __VA_ARGS__)
#define IO_ERROR_LOG(code, op, filename, ...)                                                      \
  log_io_error(LOG_LEVEL_ERROR, code, __FILE__, __FUNCTION__, __LINE__, op, filename, __VA_ARGS__)
#define IO_FATAL_ERROR(code, op, filename, ...)                                                    \
  do {                                                                                             \
    log_io_error(LOG_LEVEL_FATAL, code, __FILE__, __FUNCTION__, __LINE__, op, filename,            \
                 __VA_ARGS__);                                                                     \
    myexit(1);                                                                                     \
  } while (0)

// String representation functions
const char *get_log_level_name(LogLevel level);

// Maximum number of debug log messages per call site before suppression
#define DEBUG_LOG_MAX_CALLS 5

#endif /* ERROR_HANDLING_H */
