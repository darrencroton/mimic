/**
 * @file    error_handling.c
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
 * ============================================================================
 *
 * THREAD SAFETY: These global variables are NOT thread-safe.
 *
 * The current Mimic architecture uses MPI process-based parallelism where each
 * MPI process has its own separate memory space. Global variables are safe in
 * this model because there is no shared memory between processes.
 *
 * IMPORTANT: If migrating to shared-memory threading (OpenMP, pthreads, etc.),
 * these variables MUST be protected with mutexes to prevent race conditions
 * during concurrent logging operations.
 *
 * Affected functions: set_log_level(), set_log_output(), log_message()
 * ============================================================================
 */

// Default log level: show everything except debug messages
static LogLevel current_log_level = LOG_LEVEL_INFO;

// Default output file pointer: stderr for warnings and errors, stdout for info
// and debug
static FILE *log_output = NULL;

// Verbose format flag: adds context (timestamp, file, line) to messages
static int verbose_format = 0;

// Debug log rate limiting flag: only enabled during intensive processing phases
static int debug_log_rate_limiting_enabled = 0;

// Level names for printing
static const char *level_names[] = {"DEBUG", "INFO", "WARNING", "ERROR",
                                    "FATAL"};

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
 *
 * The function also logs an initialization message at INFO level to
 * confirm the system is working correctly.
 */
void initialize_error_handling(LogLevel min_level, FILE *output_file) {
  set_log_level(min_level);
  set_log_output(output_file);

  // Log that the error handling system has been initialized
  INFO_LOG("Error handling system initialized. Log level set to %s",
           level_names[min_level]);
}

/**
 * @brief   Gets the string representation of a log level
 *
 * @param   level    The log level to convert to string
 * @return  String representation of the log level
 *
 * This function converts a LogLevel enum value to its corresponding
 * string representation (e.g., LOG_LEVEL_ERROR -> "ERROR"). If the
 * level is outside the valid range, it returns "UNKNOWN".
 *
 * The returned string is statically allocated and should not be freed.
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
 * @return  String representation of the error code
 *
 * This function converts an IOErrorCode enum value to its corresponding
 * string representation (e.g., IO_ERROR_READ_FAILED -> "READ_FAILED").
 * If the code is outside the valid range, it returns "UNKNOWN".
 *
 * The returned string is statically allocated and should not be freed.
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
 *
 * This function returns the current log level setting, allowing
 * other parts of the system to adjust their output accordingly.
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
 *
 * This function returns whether verbose formatting is currently enabled,
 * allowing other parts of the system to adjust their output accordingly.
 */
int get_verbose_format(void) { return verbose_format; }

/**
 * @brief   Enables rate limiting for DEBUG_LOG output
 *
 * When enabled, each DEBUG_LOG location will output at most DEBUG_LOG_MAX_CALLS
 * messages before suppressing further output. This prevents runaway logging
 * during intensive processing phases (e.g., tree processing loops).
 *
 * Typically enabled during TREE_PROCESSING phase and disabled for other phases.
 */
void enable_debug_log_rate_limiting(void) {
  debug_log_rate_limiting_enabled = 1;
}

/**
 * @brief   Disables rate limiting for DEBUG_LOG output
 *
 * When disabled, DEBUG_LOG messages are not rate-limited and will output
 * every time they are called. This is appropriate for configuration, module
 * initialization, and other non-loop contexts.
 */
void disable_debug_log_rate_limiting(void) {
  debug_log_rate_limiting_enabled = 0;
}

/**
 * @brief   Checks if DEBUG_LOG rate limiting is currently enabled
 *
 * @return  1 if rate limiting is enabled, 0 otherwise
 */
int is_debug_log_rate_limiting_enabled(void) {
  return debug_log_rate_limiting_enabled;
}

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
 * @brief   Central logging function
 *
 * @param   level     Severity level of the message
 * @param   file      Source file where logging occurs
 * @param   func      Function where logging occurs
 * @param   line      Line number where logging occurs
 * @param   format    Printf-style format string
 * @param   ...       Variable arguments for format string
 *
 * This function implements the core logging functionality. It formats
 * a message with context information (timestamp, level, file, function,
 * line) and writes it to the appropriate output. Messages below the
 * current minimum log level are silently ignored.
 *
 * The function automatically adds a newline if not present in the
 * format string, and immediately flushes the output for error and
 * fatal messages to ensure they are visible even if the program
 * terminates abnormally.
 *
 * Note: This function is not usually called directly; instead, use
 * the DEBUG_LOG, INFO_LOG, WARNING_LOG, ERROR_LOG, and FATAL_ERROR
 * macros defined in error_handling.h.
 */
void log_message(LogLevel level, const char *file, const char *func, int line,
                 const char *format, ...) {
  // Suppress unused parameter warning (func not used in simplified format)
  (void)func;

  // Skip if below current log level
  if (level < current_log_level) {
    return;
  }

  // Get current time
  time_t now;
  time(&now);

  // Choose output stream based on message level
  FILE *output = log_output;
  if (output == NULL) {
    output = (level >= LOG_LEVEL_WARNING) ? stderr : stdout;
  }

  // Optional colour codes for terminal output
  const char *colour_start = "";
  const char *colour_end = "";

  // Apply colours only for warnings and above when running in a TTY
  // WARNING  -> yellow, ERROR -> red, FATAL -> bold red
  if (isatty(fileno(output))) {
    if (level == LOG_LEVEL_WARNING) {
      colour_start = "\x1b[33m";      // yellow
      colour_end = "\x1b[0m";
    } else if (level == LOG_LEVEL_ERROR) {
      colour_start = "\x1b[31m";      // red
      colour_end = "\x1b[0m";
    } else if (level == LOG_LEVEL_FATAL) {
      colour_start = "\x1b[1;31m";    // bold red
      colour_end = "\x1b[0m";
    }
  }

  // Print header - format depends on verbosity setting
  // Verbose format: Add context (timestamp, level, file:line)
  if (verbose_format) {
    char time_str[9];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    // Extract just filename from full path
    const char *filename = strrchr(file, '/');
    filename = filename ? filename + 1 : file;
    fprintf(output, "%s[%s] %s - %s:%d - ", colour_start, time_str,
            level_names[level], filename, line);
  }
  // Quiet mode or warnings/errors: Show level prefix
  else if (current_log_level >= LOG_LEVEL_WARNING || level >= LOG_LEVEL_WARNING) {
    fprintf(output, "%s%s: ", colour_start, level_names[level]);
  }
  // Normal mode (default): Clean output, just the message
  else {
    fprintf(output, "%s", colour_start);
  }

  // Print the actual message with variable arguments
  va_list args;
  va_start(args, format);
  vfprintf(output, format, args);
  va_end(args);

  // Reset colour if it was enabled
  if (colour_end[0] != '\0') {
    fprintf(output, "%s", colour_end);
  }

  // Add newline if not already present
  if (format[strlen(format) - 1] != '\n') {
    fprintf(output, "\n");
  }

  // Flush output immediately for errors and fatal messages
  if (level >= LOG_LEVEL_ERROR) {
    fflush(output);
  }
}

/**
 * @brief   I/O-specific logging function
 *
 * @param   level      Severity level of the message
 * @param   code       I/O error code
 * @param   file       Source file where logging occurs
 * @param   func       Function where logging occurs
 * @param   line       Line number where logging occurs
 * @param   operation  Type of I/O operation (e.g., "read", "write", "open")
 * @param   filename   Name of the file being operated on
 * @param   format     Printf-style format string for additional details
 * @param   ...        Variable arguments for format string
 *
 * This function extends the core logging functionality with I/O-specific
 * context information. It formats a message with operation type, filename,
 * and error code in addition to the standard context information.
 *
 * The function is especially useful for standardizing I/O error reporting
 * throughout the codebase. It follows the same filtering and output rules
 * as the core log_message() function.
 *
 * Note: This function is not usually called directly; instead, use
 * the IO_DEBUG_LOG, IO_INFO_LOG, IO_WARNING_LOG, IO_ERROR_LOG, and
 * IO_FATAL_ERROR macros defined in error_handling.h.
 */
void log_io_error(LogLevel level, IOErrorCode code, const char *file,
                  const char *func, int line, const char *operation,
                  const char *filename, const char *format, ...) {
  // Skip if below current log level
  if (level < current_log_level) {
    return;
  }

  // Get current time
  time_t now;
  time(&now);
  char time_str[20];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

  // Choose output stream based on message level
  FILE *output = log_output;
  if (output == NULL) {
    output = (level >= LOG_LEVEL_WARNING) ? stderr : stdout;
  }

  // Optional colour codes for terminal output
  const char *colour_start = "";
  const char *colour_end = "";

  if (isatty(fileno(output))) {
    if (level == LOG_LEVEL_WARNING) {
      colour_start = "\x1b[33m";      // yellow
      colour_end = "\x1b[0m";
    } else if (level == LOG_LEVEL_ERROR) {
      colour_start = "\x1b[31m";      // red
      colour_end = "\x1b[0m";
    } else if (level == LOG_LEVEL_FATAL) {
      colour_start = "\x1b[1;31m";    // bold red
      colour_end = "\x1b[0m";
    }
  }

  // Print header with time, level, file, function, and line
  fprintf(output, "%s[%s] %s - %s:%s:%d - ", colour_start, time_str,
          level_names[level], file, func, line);

  // Print I/O-specific information: operation, filename, and error code
  fprintf(output, "[I/O %s, file: '%s', error: %s] ", operation,
          filename ? filename : "?", get_io_error_name(code));

  // Print the additional details with variable arguments
  va_list args;
  va_start(args, format);
  vfprintf(output, format, args);
  va_end(args);

  // Reset colour if it was enabled
  if (colour_end[0] != '\0') {
    fprintf(output, "%s", colour_end);
  }

  // Add newline if not already present
  if (format[strlen(format) - 1] != '\n') {
    fprintf(output, "\n");
  }

  // Flush output immediately for errors and fatal messages
  if (level >= LOG_LEVEL_ERROR) {
    fflush(output);
  }
}
