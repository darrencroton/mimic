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

#include "error.h"
#include "progress.h"
#include "run_log.h" /* MimicLogUseColor, ANSI helpers */

#ifdef MPI
#include "globals.h" /* ThisTask, NTask, ThisNode (multi-rank fallback) */
#endif

/* Default inner width of the bar and minimum seconds between live redraws. */
#define PROGRESS_BAR_WIDTH 50
#define PROGRESS_MIN_REFRESH 0.1
/* Fallback log: emit a bar line every this many percentage points. */
#define PROGRESS_LOG_PCT_STEP 5
#define PROGRESS_TIME_MAX_HOURS 999999LL
#define PROGRESS_TIME_MAX_SECONDS ((PROGRESS_TIME_MAX_HOURS * 3600LL) + 3599LL)
#define PROGRESS_TIME_STR_LEN 32

/* Completion marker appended to the finished live bar (in place of a separate
 * "Completed file" line). Modern terminals render the check-mark emoji. */
#define PROGRESS_DONE_MARK "- COMPLETED"

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
  /* Keep the field compact and avoid undefined casts for NaN/inf/huge values. */
  if (!(seconds >= 0.0))
    seconds = 0.0;
  if (seconds > (double)PROGRESS_TIME_MAX_SECONDS)
    seconds = (double)PROGRESS_TIME_MAX_SECONDS;

  long long total = (long long)(seconds + 0.5);
  if (total > PROGRESS_TIME_MAX_SECONDS)
    total = PROGRESS_TIME_MAX_SECONDS;

  long long h = total / 3600;
  long long m = (total % 3600) / 60;
  long long s = total % 60;
  if (h > 0)
    snprintf(buf, n, "%lld:%02lld:%02lld", h, m, s);
  else
    snprintf(buf, n, "%lld:%02lld", m, s);
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

  char elapsed_str[PROGRESS_TIME_STR_LEN];
  char eta_str[PROGRESS_TIME_STR_LEN];
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
    bar[bpos++] = '~';
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

void progress_bar_init(ProgressBar *pb, int64_t total, const char *label) {
  pb->total = total;
  pb->current = 0;
  strncpy(pb->label, label ? label : "", sizeof(pb->label) - 1);
  pb->label[sizeof(pb->label) - 1] = '\0';
  pb->start_time = now_seconds();
  pb->last_refresh = 0.0;
  pb->min_refresh = PROGRESS_MIN_REFRESH;
  pb->live = progress_display_active();
  pb->last_log_pct = -PROGRESS_LOG_PCT_STEP;
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
  char line[256];
  double elapsed = now_seconds() - pb->start_time;
  progress_bar_format(line, sizeof(line), pb->current, pb->total, elapsed, PROGRESS_BAR_WIDTH,
                      MimicLogUseColor, pb->label);
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

  /* Fallback: emit a bar-format log line at every PROGRESS_LOG_PCT_STEP % boundary. */
  int pct = (pb->total > 0) ? (int)(100LL * current / pb->total) : 100;
  if (pct > 100)
    pct = 100;
  if (pct >= pb->last_log_pct + PROGRESS_LOG_PCT_STEP) {
    pb->last_log_pct = pct - (pct % PROGRESS_LOG_PCT_STEP);
    char line[256];
    double elapsed = now_seconds() - pb->start_time;
    progress_bar_format(line, sizeof(line), current, pb->total, elapsed, PROGRESS_BAR_WIDTH,
                        MimicLogUseColor, pb->label);
    INFO_LOG("%s", line);
  }
}

void progress_bar_finish(ProgressBar *pb) {
  pb->current = pb->total;
  char line[256];
  double elapsed = now_seconds() - pb->start_time;
  progress_bar_format(line, sizeof(line), pb->current, pb->total, elapsed, PROGRESS_BAR_WIDTH,
                      MimicLogUseColor, pb->label);

  if (pb->live) {
    /* Overwrite the live bar in place with a completion marker. */
    fprintf(stdout, "\r%s %s  \n", line, PROGRESS_DONE_MARK);
    fflush(stdout);
    show_cursor();
  } else if (pb->last_log_pct < 100) {
    /* Fallback: emit a final 100% bar line if the last interval didn't reach it. */
    INFO_LOG("%s %s", line, PROGRESS_DONE_MARK);
  }
}
