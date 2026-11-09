/**
 * @file    test_progress.c
 * @brief   Unit tests for the terminal progress bar's pure helpers
 *
 * Covers progress_bar_format() (fill, percentage, counts, elapsed/ETA, colour),
 * progress_bar_format_fit() (terminal-width fitting and its degradation order)
 * and progress_bar_should_redraw(). The live drawing itself needs a real
 * terminal and is not exercised here.
 */

#include "../framework/test_framework.h"
#include "../../src/util/progress.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Count filled bar cells: tilde shaft ('~') plus arrowhead ('>'). */
static int count_filled(const char *s) {
  int n = 0;
  for (; *s; s++)
    if (*s == '~' || *s == '>')
      n++;
  return n;
}

/* Terminal columns a line occupies: ANSI CSI codes take none, each UTF-8 character one.
 * Deliberately independent of progress.c's own measure. */
static int columns_of(const char *s) {
  int n = 0;
  while (*s) {
    if (s[0] == '\x1b' && s[1] == '[') {
      s += 2;
      while (*s && !(*s >= 0x40 && *s <= 0x7e))
        s++;
      if (*s)
        s++;
      continue;
    }
    if (((unsigned char)*s & 0xC0) != 0x80)
      n++;
    s++;
  }
  return n;
}

/* Bar interior width of an uncoloured line, or -1 when it has no bar. */
static int bar_cells(const char *s) {
  const char *open = strchr(s, '[');
  const char *close = open ? strchr(open, ']') : NULL;
  return (open && close) ? (int)(close - open - 1) : -1;
}

/**
 * @test    test_bounds
 * @brief   0%, 50%, and 100% render the expected fill and percentage
 */
int test_bounds(void) {
  char buf[256];

  /* Empty */
  progress_bar_format(buf, sizeof(buf), 0, 100, 0.0, 30, 0, "file 1");
  TEST_ASSERT(strstr(buf, "  0%") != NULL, "0/100 should report 0%");
  TEST_ASSERT(strstr(buf, "(0/100)") != NULL, "count should be (0/100)");
  TEST_ASSERT(count_filled(buf) == 0, "0% should have no filled cells");

  /* Half */
  progress_bar_format(buf, sizeof(buf), 50, 100, 1.0, 30, 0, "file 1");
  TEST_ASSERT(strstr(buf, " 50%") != NULL, "50/100 should report 50%");
  TEST_ASSERT(count_filled(buf) == 15, "50% of width 30 should fill 15 cells");

  /* Full */
  progress_bar_format(buf, sizeof(buf), 100, 100, 2.0, 30, 0, "file 1");
  TEST_ASSERT(strstr(buf, "100%") != NULL, "100/100 should report 100%");
  TEST_ASSERT(count_filled(buf) == 30, "100% should fill the whole width");

  return TEST_PASS;
}

/**
 * @test    test_label_and_counts
 * @brief   Label prefix and item counts appear; NULL label is tolerated
 */
int test_label_and_counts(void) {
  char buf[256];

  progress_bar_format(buf, sizeof(buf), 42, 1000, 0.0, 30, 0, "file 7");
  TEST_ASSERT(strncmp(buf, "file 7 [", 8) == 0, "label should prefix the bar");
  TEST_ASSERT(strstr(buf, "(42/1000)") != NULL, "counts should be (42/1000)");

  /* NULL label must not crash and must not emit a leading space. */
  progress_bar_format(buf, sizeof(buf), 1, 2, 0.0, 30, 0, NULL);
  TEST_ASSERT(buf[0] == '[', "NULL label should start directly at the bar");

  return TEST_PASS;
}

/**
 * @test    test_eta_and_elapsed
 * @brief   ETA is zero at completion; elapsed/ETA formatting is correct
 */
int test_eta_and_elapsed(void) {
  char buf[256];

  /* Half done after 10s -> ~10s remaining; elapsed shown as 0:10. */
  progress_bar_format(buf, sizeof(buf), 50, 100, 10.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "elapsed 0:10") != NULL, "elapsed should format as 0:10");
  TEST_ASSERT(strstr(buf, "ETA 0:10") != NULL, "ETA at 50%% after 10s should be ~0:10");

  /* Complete -> ETA 0:00 regardless of elapsed. */
  progress_bar_format(buf, sizeof(buf), 100, 100, 30.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "ETA 0:00") != NULL, "ETA at completion should be 0:00");

  /* Durations of an hour or more switch to H:MM:SS. */
  progress_bar_format(buf, sizeof(buf), 50, 100, 3661.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "elapsed 1:01:01") != NULL, "3661s should format as 1:01:01");

  return TEST_PASS;
}

/**
 * @test    test_large_elapsed_clamps
 * @brief   Extremely large elapsed values saturate to a bounded display field
 */
int test_large_elapsed_clamps(void) {
  char buf[256];

  progress_bar_format(buf, sizeof(buf), 100, 100, 1e300, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "elapsed 999999:59:59") != NULL,
              "huge elapsed time should clamp to the maximum display value");

  return TEST_PASS;
}

/**
 * @test    test_color_gating
 * @brief   ANSI escapes appear only when colour is requested
 */
int test_color_gating(void) {
  char buf[256];

  progress_bar_format(buf, sizeof(buf), 50, 100, 1.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "\x1b[") == NULL, "no ANSI escapes when colour is off");

  progress_bar_format(buf, sizeof(buf), 50, 100, 1.0, 30, 1, "f");
  TEST_ASSERT(strstr(buf, "\x1b[92m") != NULL, "green start code present when colour is on");
  TEST_ASSERT(strstr(buf, "\x1b[0m") != NULL, "reset code present when colour is on");

  return TEST_PASS;
}

/**
 * @test    test_zero_total
 * @brief   A zero/empty total is treated as complete without dividing by zero
 */
int test_zero_total(void) {
  char buf[256];

  progress_bar_format(buf, sizeof(buf), 0, 0, 0.0, 30, 0, "f");
  TEST_ASSERT(strstr(buf, "100%") != NULL, "empty file should render as complete");
  TEST_ASSERT(strstr(buf, "(0/0)") != NULL, "counts should be (0/0)");

  return TEST_PASS;
}

/**
 * @test    test_fit_unknown_width_is_full_line
 * @brief   An unknown terminal width renders the full-width line unchanged
 */
int test_fit_unknown_width_is_full_line(void) {
  char full[512], fit[512];

  progress_bar_format(full, sizeof(full), 42, 1000, 5.0, PROGRESS_BAR_WIDTH, 1, "file 7");
  progress_bar_format_fit(fit, sizeof(fit), 42, 1000, 5.0, 0, 1, "file 7");
  TEST_ASSERT_STRING_EQUAL(full, fit, "columns <= 0 should match progress_bar_format()");

  return TEST_PASS;
}

/**
 * @test    test_fit_never_exceeds_terminal
 * @brief   Every width from 1 to 200 columns leaves the edge margin free
 */
int test_fit_never_exceeds_terminal(void) {
  char buf[512];
  const char *labels[] = {"", "file 7", "a deliberately long label that will not fit anywhere"};

  for (size_t l = 0; l < sizeof(labels) / sizeof(labels[0]); l++) {
    for (int color = 0; color <= 1; color++) {
      for (int columns = 1; columns <= 200; columns++) {
        progress_bar_format_fit(buf, sizeof(buf), 12345, 29585, 3661.0, columns, color, labels[l]);
        int limit = columns - PROGRESS_EDGE_MARGIN;
        if (limit < 0)
          limit = 0;
        if (columns_of(buf) > limit) {
          printf("columns=%d color=%d label='%s' gave %d columns: '%s'\n", columns, color,
                 labels[l], columns_of(buf), buf);
          TEST_ASSERT(0, "fitted line must leave PROGRESS_EDGE_MARGIN columns free");
        }
      }
    }
  }

  return TEST_PASS;
}

/**
 * @test    test_fit_degradation_order
 * @brief   The bar shrinks, then counts drop, then the bar, then the times
 *
 * Layouts are sized at their widest: for total 100 the counts field is " (100/100)" (10)
 * and the times field " elapsed 9:59:59 ETA 9:59:59" (28); the percentage takes 4 and a
 * bar "[" + cells + "] " takes 3 + cells. With the 4-column margin each threshold below
 * follows by arithmetic.
 */
int test_fit_degradation_order(void) {
  char buf[512];

  /* Full bar from 99 columns: 3 + 50 + 4 + 10 + 28 = 95 = 99 - 4. */
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 200, 0, "");
  TEST_ASSERT(bar_cells(buf) == PROGRESS_BAR_WIDTH, "wide terminals keep the full bar");
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 99, 0, "");
  TEST_ASSERT(bar_cells(buf) == 50, "99 columns still fit the full bar");
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 98, 0, "");
  TEST_ASSERT(bar_cells(buf) == 49, "98 columns shrink the bar by one cell");

  /* Counts kept down to a 10-cell bar at 59 columns. */
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 59, 0, "");
  TEST_ASSERT(bar_cells(buf) == PROGRESS_BAR_MIN_WIDTH, "59 columns give the narrowest bar");
  TEST_ASSERT(strstr(buf, "(50/100)") != NULL, "counts kept while the bar fits beside them");

  /* At 58 columns the counts go and the bar regrows into their space. */
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 58, 0, "");
  TEST_ASSERT(strstr(buf, "(50/100)") == NULL, "counts dropped below 59 columns");
  TEST_ASSERT(bar_cells(buf) == 19, "the bar regrows into the counts' space");

  /* The bar goes below 49 columns; percentage and times remain down to 36. */
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 49, 0, "");
  TEST_ASSERT(bar_cells(buf) == PROGRESS_BAR_MIN_WIDTH, "49 columns still fit a bar");
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 48, 0, "");
  TEST_ASSERT(bar_cells(buf) == -1, "the bar is dropped below 49 columns");
  TEST_ASSERT_STRING_EQUAL(" 50% elapsed 0:10 ETA 0:10", buf, "text-only line at 48 columns");
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 36, 0, "");
  TEST_ASSERT(strstr(buf, "elapsed") != NULL, "times kept down to 36 columns");

  /* Then the bare percentage, then truncation. */
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 35, 0, "");
  TEST_ASSERT_STRING_EQUAL(" 50%", buf, "percentage alone below 36 columns");
  progress_bar_format_fit(buf, sizeof(buf), 50, 100, 10.0, 7, 0, "");
  TEST_ASSERT_STRING_EQUAL(" 50", buf, "truncated to the space left");

  return TEST_PASS;
}

/**
 * @test    test_fit_width_is_steady
 * @brief   The bar keeps its width as count digits grow and the ETA drops below an hour
 */
int test_fit_width_is_steady(void) {
  char buf[512];

  /* 80 columns, 29585 items: 9 items with an ETA over an hour, then 12345 with seconds left. */
  progress_bar_format_fit(buf, sizeof(buf), 9, 29585, 2.0, 80, 0, "");
  const int early = bar_cells(buf);
  progress_bar_format_fit(buf, sizeof(buf), 12345, 29585, 60.0, 80, 0, "");
  TEST_ASSERT(early > 0 && bar_cells(buf) == early, "bar width should not change during a run");

  return TEST_PASS;
}

/**
 * @test    test_fit_colour_takes_no_columns
 * @brief   Colour codes do not shrink the bar
 */
int test_fit_colour_takes_no_columns(void) {
  char plain[512], coloured[512];

  progress_bar_format_fit(plain, sizeof(plain), 50, 100, 10.0, 60, 0, "");
  progress_bar_format_fit(coloured, sizeof(coloured), 50, 100, 10.0, 60, 1, "");
  TEST_ASSERT(strstr(coloured, "\x1b[92m") != NULL, "colour requested and present");
  TEST_ASSERT(columns_of(coloured) == columns_of(plain), "colour should not change the layout");

  return TEST_PASS;
}

/**
 * @test    test_should_redraw
 * @brief   Redraw on the first draw, a percentage change, the heartbeat or the end
 */
int test_should_redraw(void) {
  TEST_ASSERT(progress_bar_should_redraw(0, 100, -1, 0.0) == 1, "first draw");
  TEST_ASSERT(progress_bar_should_redraw(5, 100, 5, 0.5) == 0, "same percentage, recent draw");
  TEST_ASSERT(progress_bar_should_redraw(5, 100, 5, PROGRESS_HEARTBEAT_SECONDS) == 1,
              "heartbeat elapsed");
  TEST_ASSERT(progress_bar_should_redraw(6, 100, 5, 0.0) == 1, "percentage changed");
  TEST_ASSERT(progress_bar_should_redraw(100, 100, 100, 0.0) == 1, "final item");
  TEST_ASSERT(progress_bar_should_redraw(0, 0, 100, 0.0) == 1, "empty total counts as final");

  /* Within the heartbeat, 100000 items redraw once per displayed percentage (0-100). */
  int redraws = 0, last_pct = -1;
  for (int64_t i = 0; i < 100000; i++) {
    if (progress_bar_should_redraw(i, 100000, last_pct, 0.0)) {
      redraws++;
      last_pct = (int)((double)i / 100000.0 * 100.0 + 0.5);
    }
  }
  TEST_ASSERT(redraws == 101, "one redraw per percentage, not per item");

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Progress Bar Formatter\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  TEST_RUN(test_bounds);
  TEST_RUN(test_label_and_counts);
  TEST_RUN(test_eta_and_elapsed);
  TEST_RUN(test_large_elapsed_clamps);
  TEST_RUN(test_color_gating);
  TEST_RUN(test_zero_total);
  TEST_RUN(test_fit_unknown_width_is_full_line);
  TEST_RUN(test_fit_never_exceeds_terminal);
  TEST_RUN(test_fit_degradation_order);
  TEST_RUN(test_fit_width_is_steady);
  TEST_RUN(test_fit_colour_takes_no_columns);
  TEST_RUN(test_should_redraw);

  TEST_SUMMARY();
  return TEST_RESULT();
}
