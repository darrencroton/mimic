/**
 * @file    progress.c
 * @brief   Terminal progress bar implementation (see progress.h).
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "constants.h" /* TREE_PROGRESS_INTERVAL */
#include "error.h"
#include "progress.h"
#include "run_log.h" /* MimicLogUseColor, ANSI helpers */

#ifdef MPI
#include "globals.h" /* ThisTask, NTask, ThisNode (multi-rank fallback) */
#endif

/* Default inner width of the bar and minimum seconds between live redraws. */
#define PROGRESS_BAR_WIDTH 30
#define PROGRESS_MIN_REFRESH 0.1

/* Completion marker appended to the finished live bar (in place of a separate
 * "Completed file" line). Modern terminals render the check-mark emoji. */
#define PROGRESS_DONE_MARK "- completed \xE2\x9C\x85"

/* Hide/show the terminal cursor while the live bar is active.
 * An atexit handler guarantees the cursor is restored on abnormal exit. */
static int cursor_hidden = 0;

static void restore_cursor_atexit(void) {
  if (cursor_hidden) {
    fputs("\x1b[?25h", stdout);
    fflush(stdout);
    cursor_hidden = 0;
  }
}

/* Signal handler: restore cursor then re-raise so the shell sees the signal. */
static void signal_restore_cursor(int sig) {
  restore_cursor_atexit();
  signal(sig, SIG_DFL);
  raise(sig);
}

static void hide_cursor(void) {
  if (cursor_hidden)
    return;
  static int atexit_registered = 0;
  if (!atexit_registered) {
    atexit(restore_cursor_atexit);
    signal(SIGINT, signal_restore_cursor);
    signal(SIGTERM, signal_restore_cursor);
    atexit_registered = 1;
  }
  fputs("\x1b[?25l", stdout);
  fflush(stdout);
  cursor_hidden = 1;
}

static void show_cursor(void) {
  if (!cursor_hidden)
    return;
  fputs("\x1b[?25h", stdout);
  fflush(stdout);
  cursor_hidden = 0;
}

/* Monotonic wall-clock seconds; falls back to 0 if the clock is unavailable. */
static double now_seconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Format a duration as M:SS, or H:MM:SS once it reaches an hour. */
static void format_hms(char *buf, size_t n, double seconds) {
  if (seconds < 0.0 || seconds != seconds) /* clamp negatives and NaN */
    seconds = 0.0;
  long total = (long)(seconds + 0.5);
  long h = total / 3600;
  long m = (total % 3600) / 60;
  long s = total % 60;
  if (h > 0)
    snprintf(buf, n, "%ld:%02ld:%02ld", h, m, s);
  else
    snprintf(buf, n, "%ld:%02ld", m, s);
}

int progress_bar_format(char *buf, size_t n, int64_t cur, int64_t total, double elapsed, int width,
                        int use_color, const char *desc) {
  if (width < 1)
    width = 1;

  /* Fraction complete; a non-positive total means "done". */
  double frac;
  if (total <= 0)
    frac = 1.0;
  else if (cur >= total)
    frac = 1.0;
  else if (cur <= 0)
    frac = 0.0;
  else
    frac = (double)cur / (double)total;

  int filled = (int)(frac * width + 0.5);
  if (filled > width)
    filled = width;

  /* ETA only while in-flight and making progress. */
  double eta = 0.0;
  if (total > 0 && cur > 0 && cur < total)
    eta = elapsed * (double)(total - cur) / (double)cur;

  char elapsed_str[16];
  char eta_str[16];
  format_hms(elapsed_str, sizeof(elapsed_str), elapsed);
  format_hms(eta_str, sizeof(eta_str), eta);

  const char *green = use_color ? "\x1b[92m" : "";
  const char *reset = use_color ? "\x1b[0m" : "";

  /* Build the bar interior: arrow shaft (coloured) then empty cells.
   * In-flight: dashes + arrowhead (e.g. "----->   ").
   * Complete:  all dashes (e.g. "----------"). */
  char bar[PROGRESS_BAR_WIDTH * 2 + 32];
  size_t bpos = 0;
  int safe_filled = filled <= PROGRESS_BAR_WIDTH ? filled : PROGRESS_BAR_WIDTH;
  int safe_width = width <= PROGRESS_BAR_WIDTH ? width : PROGRESS_BAR_WIDTH;
  int complete = (safe_filled >= safe_width);
  bpos += (size_t)snprintf(bar + bpos, sizeof(bar) - bpos, "%s", green);
  int shaft = complete ? safe_filled : (safe_filled > 0 ? safe_filled - 1 : 0);
  for (int i = 0; i < shaft && bpos < sizeof(bar) - 1; i++)
    bar[bpos++] = '-';
  if (!complete && safe_filled > 0 && bpos < sizeof(bar) - 1)
    bar[bpos++] = '>';
  bpos += (size_t)snprintf(bar + bpos, sizeof(bar) - bpos, "%s", reset);
  for (int i = safe_filled; i < safe_width && bpos < sizeof(bar) - 1; i++)
    bar[bpos++] = ' ';
  bar[bpos] = '\0';

  int pct = (int)(frac * 100.0 + 0.5);
  return snprintf(buf, n, "%s%s[%s] %3d%% (%lld/%lld) elapsed %s ETA %s", desc ? desc : "",
                  (desc && *desc) ? " " : "", bar, pct, (long long)cur, (long long)total,
                  elapsed_str, eta_str);
}

void progress_bar_init(ProgressBar *pb, int64_t total, int context_id) {
  pb->total = total;
  pb->current = 0;
  pb->context_id = context_id;
  pb->start_time = now_seconds();
  pb->last_refresh = 0.0;
  pb->min_refresh = PROGRESS_MIN_REFRESH;
  pb->live = progress_display_active();
  if (pb->live)
    hide_cursor();
}

int progress_display_active(void) {
  int multi_rank = 0;
#ifdef MPI
  multi_rank = (NTask > 1);
#endif
  /* Live bar only when interactive, single-rank, and not in quiet mode. */
  return (!multi_rank && isatty(STDOUT_FILENO) && get_log_level() <= LOG_LEVEL_INFO);
}

/* Draw the in-place bar to stdout. A short trailing pad clears any leftover
 * characters from a previous, longer line (e.g. a shrinking ETA). */
static void draw_live(ProgressBar *pb) {
  char label[24];
  snprintf(label, sizeof(label), "file %d", pb->context_id);

  char line[256];
  double elapsed = now_seconds() - pb->start_time;
  progress_bar_format(line, sizeof(line), pb->current, pb->total, elapsed, PROGRESS_BAR_WIDTH,
                      MimicLogUseColor, label);
  fprintf(stdout, "\r%s   ", line);
  fflush(stdout);
}

void progress_bar_update(ProgressBar *pb, int64_t current) {
  pb->current = current;

  if (pb->live) {
    double now = now_seconds();
    if (current >= pb->total || (now - pb->last_refresh) >= pb->min_refresh) {
      pb->last_refresh = now;
      draw_live(pb);
    }
    return;
  }

  /* Fallback: periodic log lines, matching the historical message text. */
  if (current % TREE_PROGRESS_INTERVAL == 0) {
#ifdef MPI
    INFO_LOG("  Processing task %d | node %s | file %i | tree %lld of %lld", ThisTask, ThisNode,
             pb->context_id, (long long)current, (long long)pb->total);
#else
    INFO_LOG("  Processing file %i | tree %lld of %lld", pb->context_id, (long long)current,
             (long long)pb->total);
#endif
  }
}

void progress_bar_finish(ProgressBar *pb) {
  if (!pb->live)
    return;

  /* Finalise the bar in place with a completion marker, replacing the separate
   * "Completed file" line (which is kept only for the non-live fallback). */
  pb->current = pb->total;
  char label[24];
  snprintf(label, sizeof(label), "file %d", pb->context_id);
  char line[256];
  double elapsed = now_seconds() - pb->start_time;
  progress_bar_format(line, sizeof(line), pb->current, pb->total, elapsed, PROGRESS_BAR_WIDTH,
                      MimicLogUseColor, label);
  fprintf(stdout, "\r%s %s  \n", line, PROGRESS_DONE_MARK);
  fflush(stdout);
  show_cursor();
}
