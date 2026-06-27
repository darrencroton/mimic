/**
 * @file    error.c
 * @brief   Implementation of the Mimic error handling and logging system
 *
 * This file implements a comprehensive error handling and logging system for
 * the Mimic framework. It provides functions for logging messages at different
 * severity levels (debug, info, warning, error, fatal), with configurable
 * verbosity and output destination.
 *
 * The system supports:
 * - Multiple log levels with filtering
 * - Context-rich messages (timestamp, file, function, line)
 * - Configurable output destination
 * - Automatic newline handling
 * - Immediate flushing for critical messages
 *
 * Key functions:
 * - initialize_error_handling(): Sets up the logging system
 * - set_log_level(): Controls verbosity
 * - set_log_output(): Redirects log output
 * - log_message(): Core logging function
 *
 * This system improves error reporting and debugging capabilities,
 * replacing the old error handling approach based on ABORT/myexit.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h> // for isatty()

#include "error.h"
#include "proto.h"

/* ============================================================================
 * Logging System Global State
 * Not thread-safe: MPI rank-per-process isolation is safe under the current
 * model. Add locking before any shared-memory migration.
 * ============================================================================ */

// Default log level: show everything except debug messages
static LogLevel current_log_level = LOG_LEVEL_INFO;

// Default output file pointer: stderr for warnings and errors, stdout for info
// and debug
static FILE *log_output = NULL;

// Verbose format flag: enables VERBOSE_LOG emission (--verbose and --debug)
static int verbose_format = 0;

// Verbose prefix flag: adds timestamp/file:line context to messages (--debug only)
static int verbose_prefix = 0;

// Debug log rate limiting flag: only enabled during intensive processing phases
static int debug_log_rate_limiting_enabled = 0;

// Level names for printing
static const char *level_names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};

// I/O error code names for printing
static const char *io_error_names[] = {"NONE",
                                       "FILE_NOT_FOUND",
                                       "PERMISSION_DENIED",
                                       "READ_FAILED",
                                       "WRITE_FAILED",
                                       "SEEK_FAILED",
                                       "INVALID_HEADER",
                                       "VERSION_MISMATCH",
                                       "ENDIANNESS",
                                       "FORMAT",
                                       "BUFFER",
                                       "EOF",
                                       "CLOSE_FAILED",
                                       "HDF5"};

/**
 * @brief   Initializes the error handling and logging system
 *
 * @param   min_level      Minimum log level to display
 * @param   output_file    File handle for log output (NULL for auto)
 *
 * This function sets up the error handling system with the specified
 * minimum log level and output destination. Messages below the minimum
 * level will be filtered out. If output_file is NULL, logs will be sent
 * to stdout for debug/info messages and stderr for warnings/errors.
 */
void initialize_error_handling(LogLevel min_level, FILE *output_file) {
  set_log_level(min_level);
  set_log_output(output_file);

  INFO_LOG("Error handling system initialized: log level %s", level_names[min_level]);
}

/**
 * @brief   Gets the string representation of a log level
 *
 * @param   level    The log level to convert to string
 * @return  String representation (e.g., "WARNING"), or "UNKNOWN" if out of range
 *
 * The returned string is statically allocated and must not be freed.
 */
const char *get_log_level_name(LogLevel level) {
  if (level < LOG_LEVEL_DEBUG || level > LOG_LEVEL_FATAL) {
    return "UNKNOWN";
  }
  return level_names[level];
}

/**
 * @brief   Gets the string representation of an I/O error code
 *
 * @param   code    The I/O error code to convert to string
 * @return  String representation (e.g., "READ_FAILED"), or "UNKNOWN" if out of range
 *
 * The returned string is statically allocated and must not be freed.
 */
const char *get_io_error_name(IOErrorCode code) {
  if (code < IO_ERROR_NONE || code > IO_ERROR_HDF5) {
    return "UNKNOWN";
  }
  return io_error_names[code];
}

/**
 * @brief   Sets the minimum log level to display
 *
 * @param   min_level    New minimum log level
 *
 * This function changes the filtering level for log messages.
 * Messages with a level lower than min_level will be suppressed.
 * This allows runtime control of log verbosity.
 *
 * Log levels in order of increasing severity:
 * - LOG_LEVEL_DEBUG   (most verbose)
 * - LOG_LEVEL_INFO
 * - LOG_LEVEL_WARNING
 * - LOG_LEVEL_ERROR
 * - LOG_LEVEL_FATAL   (least verbose)
 */
void set_log_level(LogLevel min_level) { current_log_level = min_level; }

/**
 * @brief   Gets the current log level
 *
 * @return  Current minimum log level
 */
LogLevel get_log_level(void) { return current_log_level; }

/**
 * @brief   Enables or disables verbose formatting
 *
 * @param   enable    1 to enable verbose format, 0 to disable
 *
 * When enabled, log messages include contextual information
 * (timestamp, level, file, line) for troubleshooting. When disabled,
 * only the message content is displayed for cleaner output.
 */
void set_verbose_format(int enable) { verbose_format = enable; }

/**
 * @brief   Gets the current verbose format setting
 *
 * @return  1 if verbose format is enabled, 0 otherwise
 */
int get_verbose_format(void) { return verbose_format; }

/**
 * @brief   Enables or disables the verbose log prefix
 *
 * @param   enable    1 to enable, 0 to disable
 *
 * When enabled, each log line gets a timestamp, level, and file:line prefix.
 * Activated by --debug; implies verbose_format is also set.
 */
void set_verbose_prefix(int enable) { verbose_prefix = enable; }

/**
 * @brief   Gets the current verbose prefix setting
 *
 * @return  1 if the verbose prefix is enabled, 0 otherwise
 */
int get_verbose_prefix(void) { return verbose_prefix; }

/**
 * @brief   Enables rate limiting for DEBUG_LOG output
 *
 * When enabled, each DEBUG_LOG location will output at most DEBUG_LOG_MAX_CALLS
 * messages before suppressing further output. This prevents runaway logging
 * during intensive processing phases (e.g., tree processing loops).
 *
 * Typically enabled during TREE_PROCESSING phase and disabled for other phases.
 */
void enable_debug_log_rate_limiting(void) { debug_log_rate_limiting_enabled = 1; }

/**
 * @brief   Disables rate limiting for DEBUG_LOG output
 *
 * When disabled, DEBUG_LOG messages are not rate-limited and will output
 * every time they are called. This is appropriate for configuration, module
 * initialization, and other non-loop contexts.
 */
void disable_debug_log_rate_limiting(void) { debug_log_rate_limiting_enabled = 0; }

/**
 * @brief   Checks if DEBUG_LOG rate limiting is currently enabled
 *
 * @return  1 if rate limiting is enabled, 0 otherwise
 */
int is_debug_log_rate_limiting_enabled(void) { return debug_log_rate_limiting_enabled; }

/**
 * @brief   Sets the output file for logging
 *
 * @param   output_file    New file handle for log output
 * @return  Previous output file handle
 *
 * This function changes where log messages are written. If output_file
 * is NULL, the system will automatically use stdout for debug/info
 * messages and stderr for warnings/errors.
 *
 * The function returns the previous output file handle, allowing it
 * to be restored later if needed.
 */
FILE *set_log_output(FILE *output_file) {
  FILE *old_output = log_output;
  log_output = output_file;
  return old_output;
}

/**
 * @brief   Shared log emitter used by log_message() and log_io_error()
 *
 * Handles stream selection, colour, the verbosity-dependent header, an
 * optional context prefix (used for I/O logs), the message body, trailing
 * newline, and flushing for errors. Level filtering is done by the callers.
 */
static void emit_log(LogLevel level, const char *file, int line, const char *prefix,
                     const char *format, va_list args) {
  /* Choose output stream based on message level */
  FILE *output = log_output;
  if (output == NULL) {
    output = (level >= LOG_LEVEL_WARNING) ? stderr : stdout;
  }

  /* Optional colour codes for terminal output:
   * WARNING -> yellow, ERROR -> red, FATAL -> bold red */
  const char *colour_start = "";
  const char *colour_end = "";
  if (isatty(fileno(output))) {
    if (level == LOG_LEVEL_WARNING) {
      colour_start = "\x1b[33m";
      colour_end = "\x1b[0m";
    } else if (level == LOG_LEVEL_ERROR) {
      colour_start = "\x1b[31m";
      colour_end = "\x1b[0m";
    } else if (level == LOG_LEVEL_FATAL) {
      colour_start = "\x1b[1;31m";
      colour_end = "\x1b[0m";
    }
  }

  /* Print header - format depends on verbosity setting */
  if (verbose_prefix) {
    /* Debug format: add context (timestamp, level, file:line) */
    time_t now;
    char time_str[9];
    time(&now);
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    const char *filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;
    fprintf(output, "%s[%s] %s - %s:%d - ", colour_start, time_str, level_names[level], filename,
            line);
  } else if (current_log_level >= LOG_LEVEL_WARNING || level >= LOG_LEVEL_WARNING) {
    /* Quiet mode or warnings/errors: show level prefix */
    fprintf(output, "%s%s: ", colour_start, level_names[level]);
  } else {
    /* Normal mode (default): clean output, just the message */
    fprintf(output, "%s", colour_start);
  }

  if (prefix != NULL) {
    fprintf(output, "%s", prefix);
  }

  vfprintf(output, format, args);

  if (colour_end[0] != '\0') {
    fprintf(output, "%s", colour_end);
  }

  /* Add newline if not already present */
  if (format[strlen(format) - 1] != '\n') {
    fprintf(output, "\n");
  }

  /* Flush output immediately for errors and fatal messages */
  if (level >= LOG_LEVEL_ERROR) {
    fflush(output);
  }
}

/**
 * @brief   Central logging function
 *
 * Formats a message with context information and writes it to the
 * appropriate output. Messages below the current minimum log level are
 * silently ignored. Use the DEBUG_LOG/INFO_LOG/WARNING_LOG/ERROR_LOG/
 * FATAL_ERROR macros from error.h rather than calling this directly.
 */
void log_message(LogLevel level, const char *file, const char *func, int line, const char *format,
                 ...) {
  (void)func;

  if (level < current_log_level) {
    return;
  }

  va_list args;
  va_start(args, format);
  emit_log(level, file, line, NULL, format, args);
  va_end(args);
}

/**
 * @brief   I/O-specific logging function
 *
 * Extends log_message() with I/O context (operation, filename, error code),
 * emitted as a bracketed prefix before the message. Follows the same
 * filtering, header, and output rules as log_message(). Use the IO_*_LOG
 * macros from error.h rather than calling this directly.
 */
void log_io_error(LogLevel level, IOErrorCode code, const char *file, const char *func, int line,
                  const char *operation, const char *filename, const char *format, ...) {
  (void)func;

  if (level < current_log_level) {
    return;
  }

  char prefix[512];
  snprintf(prefix, sizeof(prefix), "[I/O %s, file: '%s', error: %s] ", operation,
           filename ? filename : "?", get_io_error_name(code));

  va_list args;
  va_start(args, format);
  emit_log(level, file, line, prefix, format, args);
  va_end(args);
}
