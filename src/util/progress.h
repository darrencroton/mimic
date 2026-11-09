#ifndef PROGRESS_H
#define PROGRESS_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file    progress.h
 * @brief   Terminal progress bar for the tree-processing loop
 *
 * A single-rank run writing to a terminal gets a live bar; redirected output,
 * multi-rank MPI runs and quiet mode fall back to periodic log lines (or none).
 *
 * The live bar never hides the cursor, so no exit path can leave the terminal
 * without one. Each frame ends in a newline, parking the cursor below the bar;
 * the next frame moves up and redraws. Log lines erase the bar and redraw it
 * beneath them (set_log_display_hooks() in error.h). The bar redraws on a
 * percentage change or once per PROGRESS_HEARTBEAT_SECONDS, and is fitted to
 * the terminal width so it never wraps. A line written outside the logger, or
 * by the shell after Ctrl-Z and `fg`, can be overwritten by the next redraw.
 */

/** Widest bar interior in cells; narrow terminals shrink it. */
#define PROGRESS_BAR_WIDTH 50
/** Narrowest bar interior worth drawing; below it a fitted line drops the bar. */
#define PROGRESS_BAR_MIN_WIDTH 10
/** Columns a fitted line leaves free: the completion mark " ✓" plus a two-column buffer. */
#define PROGRESS_EDGE_MARGIN 4
/** Redraw interval when the percentage is unchanged, so elapsed time and ETA keep ticking. */
#define PROGRESS_HEARTBEAT_SECONDS 1.0

/** Progress state for one partition or a whole run. */
struct ProgressBar {
  int64_t total;      // total work items
  int64_t current;    // last reported position
  char label[128];    // prefix shown in the bar and fallback log lines
  double start_time;  // monotonic seconds at init
  double last_draw;   // monotonic seconds of the last live draw
  int last_drawn_pct; // percentage of the last live draw; -1 before the first
  int live;           // 1 = live bar, 0 = periodic log-line fallback
  int last_log_pct;   // last 5% boundary logged (fallback only)
};

/**
 * @brief Initialise @p pb for @p total items and choose live or fallback display.
 *
 * @param label Plain-ASCII prefix for the bar and fallback lines (may be empty, not NULL).
 */
void progress_bar_init(struct ProgressBar *pb, int64_t total, const char *label);

/** @brief Whether a live bar will be drawn (terminal stdout, single rank, not quiet). */
int progress_display_active(void);

/** @brief Report @p current items done; redraws per progress_bar_should_redraw() or logs at 5%. */
void progress_bar_update(struct ProgressBar *pb, int64_t current);

/**
 * @brief Draw the final 100% line with a completion mark and detach from the logger
 *        (live), or log a final 100% line if the last 5% boundary missed it (fallback).
 */
void progress_bar_finish(struct ProgressBar *pb);

/**
 * @brief Render the full progress line into @p buf (pure; exposed for unit tests).
 *
 * @param width     Bar interior in cells, clamped to [1, PROGRESS_BAR_WIDTH].
 * @param use_color Non-zero wraps the filled cells in ANSI green.
 * @param desc      Label prefix (may be NULL).
 * @return Bytes written, excluding the NUL.
 */
int progress_bar_format(char *buf, size_t n, int64_t cur, int64_t total, double elapsed, int width,
                        int use_color, const char *desc);

/**
 * @brief Render a progress line at most @p columns - PROGRESS_EDGE_MARGIN columns wide (pure).
 *
 * Degrades until it fits: shrink the bar to PROGRESS_BAR_MIN_WIDTH, drop the item
 * counts, drop the bar, drop the times, then truncate. Each layout is sized for full
 * counts and times up to 9:59:59, so the bar holds its width while both stay below 10 h.
 * @p columns <= 0 (unknown width) gives the full progress_bar_format() line.
 *
 * @return Bytes written, excluding the NUL.
 */
int progress_bar_format_fit(char *buf, size_t n, int64_t cur, int64_t total, double elapsed,
                            int columns, int use_color, const char *desc);

/**
 * @brief Whether a live bar should redraw (pure): on the final item, when the
 *        displayed percentage differs from @p last_drawn_pct (-1 before the first
 *        draw), or once PROGRESS_HEARTBEAT_SECONDS have passed.
 */
int progress_bar_should_redraw(int64_t current, int64_t total, int last_drawn_pct,
                               double seconds_since_draw);

#endif /* PROGRESS_H */
