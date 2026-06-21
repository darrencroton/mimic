#ifndef PROGRESS_H
#define PROGRESS_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file    progress.h
 * @brief   Lightweight terminal progress bar for the tree-processing loop
 *
 * Renders a single in-place updating bar (percentage, elapsed time, ETA) when
 * standard output is an interactive terminal and the run is effectively serial.
 * When output is redirected/non-interactive or running under MPI with more than
 * one rank, it degrades to the historical periodic log lines (no carriage
 * returns), keeping piped logs and multi-rank output clean. Quiet mode (log
 * level above INFO) suppresses both.
 *
 * Colour and TTY behaviour reuse the run-wide infrastructure in run_log.h.
 */

/**
 * @brief Progress bar state for one input file (partition).
 */
typedef struct ProgressBar {
  int64_t total;       // denominator (number of trees in the file)
  int64_t current;     // last reported position
  int context_id;      // output/file id, shown in both the bar and fallback logs
  double start_time;   // monotonic seconds captured at init
  double last_refresh; // monotonic seconds of the last live redraw
  double min_refresh;  // minimum seconds between live redraws
  int live;            // 1 = draw an in-place bar; 0 = periodic-line fallback
} ProgressBar;

/**
 * @brief Initialise a progress bar for a file with @p total trees.
 *
 * Captures the start time and decides whether to render a live bar (TTY,
 * single rank, not quiet) or fall back to periodic log lines.
 *
 * @param pb         Bar to initialise.
 * @param total      Total number of work items (trees) in this file.
 * @param context_id Output/file id used in the label and fallback messages.
 */
void progress_bar_init(ProgressBar *pb, int64_t total, int context_id);

/**
 * @brief Whether a live bar will be drawn for the current run (TTY, single
 *        rank, not quiet).
 *
 * Lets callers suppress redundant per-file completion messages when the live
 * bar already shows completion in place.
 */
int progress_display_active(void);

/**
 * @brief Report progress at @p current items completed.
 *
 * In live mode the bar is redrawn at most every @c min_refresh seconds (and
 * always on the final item). In fallback mode a periodic INFO_LOG line is
 * emitted every @c TREE_PROGRESS_INTERVAL items, matching legacy output.
 */
void progress_bar_update(ProgressBar *pb, int64_t current);

/**
 * @brief Finalise the bar. In live mode draws 100% and a trailing newline so
 *        the line is complete before any following message; no-op otherwise.
 */
void progress_bar_finish(ProgressBar *pb);

/**
 * @brief Render a progress line into @p buf (pure, terminal-independent).
 *
 * Exposed for unit testing the bar/percentage/ETA formatting without a TTY.
 *
 * @param buf       Destination buffer.
 * @param n         Size of @p buf.
 * @param cur       Items completed.
 * @param total     Total items (values <= 0 are treated as complete).
 * @param elapsed   Elapsed seconds since start (for elapsed/ETA fields).
 * @param width     Inner width of the bar in characters.
 * @param use_color Non-zero to wrap the filled portion in ANSI green.
 * @param desc      Short label prefix (may be NULL).
 * @return Number of characters written (excluding the terminating NUL).
 */
int progress_bar_format(char *buf, size_t n, int64_t cur, int64_t total, double elapsed, int width,
                        int use_color, const char *desc);

#endif /* PROGRESS_H */
