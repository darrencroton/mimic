/**
 * @file    parity_trace.h
 * @brief   Execution trace buffer for parity-comparison tests.
 *
 * Provides a fixed-capacity string trace that two runs can fill independently
 * and then compare with parity_trace_equal() to confirm identical execution
 * paths. Useful for side-by-side testing of refactored code against a known-
 * good baseline.
 */

#ifndef MIMIC_TESTS_PARITY_TRACE_H
#define MIMIC_TESTS_PARITY_TRACE_H

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define PARITY_TRACE_MAX_LINES 16
#define PARITY_TRACE_LINE_LEN 96

struct ParityTrace {
  int count;
  char lines[PARITY_TRACE_MAX_LINES][PARITY_TRACE_LINE_LEN];
};

static inline void parity_trace_reset(struct ParityTrace *trace) {
  if (trace == NULL) {
    return;
  }

  memset(trace, 0, sizeof(*trace));
}

static inline int parity_trace_append(struct ParityTrace *trace, const char *fmt, ...) {
  va_list args;
  int written;

  if (trace == NULL || fmt == NULL || trace->count >= PARITY_TRACE_MAX_LINES) {
    return -1;
  }

  va_start(args, fmt);
  written = vsnprintf(trace->lines[trace->count], PARITY_TRACE_LINE_LEN, fmt, args);
  va_end(args);

  if (written < 0 || written >= PARITY_TRACE_LINE_LEN) {
    return -1;
  }

  trace->count++;
  return 0;
}

static inline int parity_trace_equal(const struct ParityTrace *lhs, const struct ParityTrace *rhs) {
  if (lhs == NULL || rhs == NULL || lhs->count != rhs->count) {
    return 0;
  }

  for (int i = 0; i < lhs->count; i++) {
    if (strcmp(lhs->lines[i], rhs->lines[i]) != 0) {
      return 0;
    }
  }

  return 1;
}

static inline void parity_trace_render(const struct ParityTrace *trace, char *buffer,
                                       size_t buffer_size) {
  size_t used = 0;

  if (buffer == NULL || buffer_size == 0) {
    return;
  }

  buffer[0] = '\0';

  if (trace == NULL || trace->count == 0) {
    snprintf(buffer, buffer_size, "<empty>");
    return;
  }

  for (int i = 0; i < trace->count && used < buffer_size; i++) {
    int written = snprintf(buffer + used, buffer_size - used, "%s%s", trace->lines[i],
                           (i + 1 < trace->count) ? "\n" : "");
    if (written < 0) {
      buffer[0] = '\0';
      return;
    }
    if ((size_t)written >= buffer_size - used) {
      used = buffer_size - 1;
      break;
    }
    used += (size_t)written;
  }
}

#endif /* MIMIC_TESTS_PARITY_TRACE_H */
