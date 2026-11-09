/**
 * @file    progress.c
 * @brief   Terminal progress bar implementation (see progress.h).
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "error.h"
#include "progress.h"
#include "run_log.h" /* MimicLogUseColor */

#ifdef MPI
#include "globals.h" /* NTask */
#endif

#define PROGRESS_LOG_PCT_STEP 5 /* fallback logs a line every this many percent */
#define PROGRESS_TIME_MAX_HOURS 999999LL
#define PROGRESS_TIME_MAX_SECONDS ((PROGRESS_TIME_MAX_HOURS * 3600LL) + 3599LL)
#define PROGRESS_TIME_STR_LEN 32
#define PROGRESS_LINE_CAPACITY 512
/* Times a fitted bar is sized for (9:59:59): its width holds while both times stay below 10 h. */
#define PROGRESS_FIT_TIME_SECONDS 35999.0
#define PROGRESS_DONE_MARK "✓"

#define ANSI_CURSOR_UP "\x1b[1A"
#define ANSI_CLEAR_TO_EOL "\x1b[K"

/* Optional fields of a progress line; the percentage is always shown. */
enum { PART_BAR = 1, PART_COUNTS = 2, PART_TIMES = 4 };

/*
 * The live display. It keeps its own copy of the bar so the logger hooks can
 * redraw it, re-fitted to the current width, without pointing into a caller's
 * struct ProgressBar. At most one live bar exists: only a single-rank run draws one.
 */
static struct {
  int active;    /* between progress_bar_init() and progress_bar_finish() */
  int on_screen; /* the last frame is on the line directly above the cursor */
  int drawn;     /* `bar` has been drawn at least once */
  struct ProgressBar bar;
  char line[PROGRESS_LINE_CAPACITY];
} live_display;

/* Monotonic wall-clock seconds; 0 if the clock is unavailable. */
static double now_seconds(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0.0;
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Terminal width of stdout from the tty, else $COLUMNS, else 0 (unknown). */
static int terminal_columns(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return (int)ws.ws_col;

  const char *env = getenv("COLUMNS");
  if (env != NULL) {
    char *end = NULL;
    long columns = strtol(env, &end, 10);
    if (end != env && *end == '\0' && columns > 0 && columns < 100000)
      return (int)columns;
  }
  return 0;
}

/* Format a duration as M:SS, or H:MM:SS from an hour. */
static void format_hms(char *buf, size_t n, double seconds) {
  /* Clamp so NaN, inf and huge values never reach an undefined cast. */
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

/* Fraction complete in [0, 1]; a non-positive total counts as done. */
static double fraction_done(int64_t cur, int64_t total) {
  if (total <= 0 || cur >= total)
    return 1.0;
  if (cur <= 0)
    return 0.0;
  return (double)cur / (double)total;
}

static int percent_done(int64_t cur, int64_t total) {
  return (int)(fraction_done(cur, total) * 100.0 + 0.5);
}

/* Remaining seconds, extrapolated from the rate so far; 0 unless in flight and progressing. */
static double eta_seconds(int64_t cur, int64_t total, double elapsed) {
  if (total > 0 && cur > 0 && cur < total)
    return elapsed * (double)(total - cur) / (double)cur;
  return 0.0;
}

/* Terminal columns @p s occupies: ANSI CSI sequences take none, each UTF-8 character one. */
static int visible_width(const char *s) {
  int width = 0;
  while (*s != '\0') {
    if (s[0] == '\x1b' && s[1] == '[') {
      s += 2;
      while (*s != '\0' && !(*s >= 0x40 && *s <= 0x7e))
        s++;
      if (*s != '\0')
        s++;
      continue;
    }
    if (((unsigned char)*s & 0xC0) != 0x80)
      width++;
    s++;
  }
  return width;
}

/* Cut ANSI-free @p s to @p max_columns on a UTF-8 boundary; returns the new length. */
static size_t truncate_visible(char *s, int max_columns) {
  int width = 0;
  size_t i = 0;
  for (; s[i] != '\0'; i++) {
    if (((unsigned char)s[i] & 0xC0) != 0x80) {
      if (width == max_columns)
        break;
      width++;
    }
  }
  s[i] = '\0';
  return i;
}

/* Bounded string assembly: appends never overflow and the buffer stays NUL-terminated. */
struct LineBuilder {
  char *buf;
  size_t cap;
  size_t used;
};

static void line_append(struct LineBuilder *b, const char *format, ...) {
  if (b->used + 1 >= b->cap)
    return;
  va_list args;
  va_start(args, format);
  int written = vsnprintf(b->buf + b->used, b->cap - b->used, format, args);
  va_end(args);
  if (written < 0)
    return;
  b->used += (size_t)written;
  if (b->used > b->cap - 1)
    b->used = b->cap - 1;
}

static const char *field_gap(const struct LineBuilder *b) { return b->used > 0 ? " " : ""; }

/*
 * Render the label, the percentage and the fields in @p parts. With every part this
 * is the full line, "<desc> [~~~>   ]  42% (42/100) elapsed 0:10 ETA 0:14".
 * @p width must lie in [1, PROGRESS_BAR_WIDTH] when PART_BAR is set.
 */
static int format_parts(char *buf, size_t n, int64_t cur, int64_t total, double elapsed, double eta,
                        int width, int parts, int use_color, const char *desc) {
  if (n == 0)
    return 0;
  struct LineBuilder b = {buf, n, 0};
  buf[0] = '\0';

  if (desc != NULL && *desc != '\0')
    line_append(&b, "%s", desc);

  if (parts & PART_BAR) {
    /* Tilde shaft led by an arrowhead, which stays the leading edge at 100%. */
    int filled = (int)(fraction_done(cur, total) * width + 0.5);
    if (filled > width)
      filled = width;
    line_append(&b, "%s[%s", field_gap(&b), use_color ? "\x1b[92m" : "");
    for (int i = 0; i < filled - 1; i++)
      line_append(&b, "~");
    if (filled > 0)
      line_append(&b, ">");
    line_append(&b, "%s", use_color ? "\x1b[0m" : "");
    for (int i = filled; i < width; i++)
      line_append(&b, " ");
    line_append(&b, "]");
  }

  line_append(&b, "%s%3d%%", field_gap(&b), percent_done(cur, total));

  if (parts & PART_COUNTS)
    line_append(&b, " (%lld/%lld)", (long long)cur, (long long)total);

  if (parts & PART_TIMES) {
    char elapsed_str[PROGRESS_TIME_STR_LEN];
    char eta_str[PROGRESS_TIME_STR_LEN];
    format_hms(elapsed_str, sizeof(elapsed_str), elapsed);
    format_hms(eta_str, sizeof(eta_str), eta);
    line_append(&b, " elapsed %s ETA %s", elapsed_str, eta_str);
  }

  return (int)b.used;
}

int progress_bar_format(char *buf, size_t n, int64_t cur, int64_t total, double elapsed, int width,
                        int use_color, const char *desc) {
  if (width < 1)
    width = 1;
  if (width > PROGRESS_BAR_WIDTH)
    width = PROGRESS_BAR_WIDTH;
  return format_parts(buf, n, cur, total, elapsed, eta_seconds(cur, total, elapsed), width,
                      PART_BAR | PART_COUNTS | PART_TIMES, use_color, desc);
}

/* Visible columns of a layout (one-cell bar, no colour) at its widest: the larger of its
 * width with full counts and 9:59:59 times and its width with the real values. */
static int layout_columns(char *buf, size_t n, int64_t cur, int64_t total, double elapsed,
                          int parts, const char *desc) {
  format_parts(buf, n, total, total, PROGRESS_FIT_TIME_SECONDS, PROGRESS_FIT_TIME_SECONDS, 1, parts,
               0, desc);
  int widest = visible_width(buf);
  format_parts(buf, n, cur, total, elapsed, eta_seconds(cur, total, elapsed), 1, parts, 0, desc);
  int actual = visible_width(buf);
  return actual > widest ? actual : widest;
}

int progress_bar_format_fit(char *buf, size_t n, int64_t cur, int64_t total, double elapsed,
                            int columns, int use_color, const char *desc) {
  if (n == 0)
    return 0;
  if (columns <= 0)
    return progress_bar_format(buf, n, cur, total, elapsed, PROGRESS_BAR_WIDTH, use_color, desc);

  const int avail = columns - PROGRESS_EDGE_MARGIN;
  const double eta = eta_seconds(cur, total, elapsed);

  /* Layouts are chosen and the bar sized at their widest, so neither shifts as a run's
   * count digits and times change; the bar takes whatever the rest leaves. */
  static const int bar_layouts[] = {PART_BAR | PART_COUNTS | PART_TIMES, PART_BAR | PART_TIMES};
  for (size_t i = 0; i < sizeof(bar_layouts) / sizeof(bar_layouts[0]); i++) {
    int width = 1 + avail - layout_columns(buf, n, cur, total, elapsed, bar_layouts[i], desc);
    if (width >= PROGRESS_BAR_MIN_WIDTH) {
      if (width > PROGRESS_BAR_WIDTH)
        width = PROGRESS_BAR_WIDTH;
      return format_parts(buf, n, cur, total, elapsed, eta, width, bar_layouts[i], use_color, desc);
    }
  }

  static const int text_layouts[] = {PART_TIMES, 0};
  for (size_t i = 0; i < sizeof(text_layouts) / sizeof(text_layouts[0]); i++) {
    if (layout_columns(buf, n, cur, total, elapsed, text_layouts[i], desc) <= avail)
      return format_parts(buf, n, cur, total, elapsed, eta, 1, text_layouts[i], 0, desc);
  }

  return (int)truncate_visible(buf, avail > 0 ? avail : 0);
}

int progress_bar_should_redraw(int64_t current, int64_t total, int last_drawn_pct,
                               double seconds_since_draw) {
  if (current >= total)
    return 1;
  if (percent_done(current, total) != last_drawn_pct)
    return 1;
  return seconds_since_draw >= PROGRESS_HEARTBEAT_SECONDS;
}

/* ------------------------------------------------------------------------- */
/* Live display                                                               */
/* ------------------------------------------------------------------------- */

/* Overwrite the previous frame (moving up if it is still there) with the cached line and
 * @p suffix, clear leftovers, and park the cursor at the start of the next line. */
static void live_write_frame(const char *suffix) {
  fprintf(stdout, "%s\r%s%s" ANSI_CLEAR_TO_EOL "\n", live_display.on_screen ? ANSI_CURSOR_UP : "",
          live_display.line, suffix);
  fflush(stdout);
  live_display.on_screen = 1;
}

static void live_stop(void);
static void live_render(double now);

/* Logger hook: erase the bar so the log line takes its place. The flush orders the erase
 * before a line written to stderr. A fatal line keeps the bar: the line above the cursor
 * may instead be the end of an HDF5 error stack, printed straight to stderr. */
static void live_before_log(LogLevel level) {
  if (live_display.on_screen && level < LOG_LEVEL_FATAL) {
    fputs(ANSI_CURSOR_UP "\r" ANSI_CLEAR_TO_EOL, stdout);
    fflush(stdout);
  }
  live_display.on_screen = 0;
}

/* Logger hook: redraw the bar below the log line. After a fatal line the run is exiting,
 * so the bar stays down rather than separate the error from what follows. */
static void live_after_log(LogLevel level) {
  if (level >= LOG_LEVEL_FATAL) {
    live_stop();
    return;
  }
  if (live_display.active && live_display.drawn) {
    live_render(now_seconds());
    live_write_frame("");
  }
}

static void live_start(void) {
  live_display.active = 1;
  live_display.on_screen = 0;
  live_display.drawn = 0;
  set_log_display_hooks(live_before_log, live_after_log);
}

static void live_stop(void) {
  if (!live_display.active)
    return;
  set_log_display_hooks(NULL, NULL);
  live_display.active = 0;
  live_display.on_screen = 0;
  live_display.drawn = 0;
}

/* Render the display's copy of the bar, fitted to the terminal's current width. */
static void live_render(double now) {
  const struct ProgressBar *pb = &live_display.bar;
  progress_bar_format_fit(live_display.line, sizeof(live_display.line), pb->current, pb->total,
                          now - pb->start_time, terminal_columns(), MimicLogUseColor, pb->label);
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                 */
/* ------------------------------------------------------------------------- */

void progress_bar_init(struct ProgressBar *pb, int64_t total, const char *label) {
  pb->total = total;
  pb->current = 0;
  strncpy(pb->label, label ? label : "", sizeof(pb->label) - 1);
  pb->label[sizeof(pb->label) - 1] = '\0';
  pb->start_time = now_seconds();
  pb->last_draw = pb->start_time;
  pb->last_drawn_pct = -1;
  pb->live = progress_display_active();
  pb->last_log_pct = -PROGRESS_LOG_PCT_STEP;
  if (pb->live)
    live_start();
}

int progress_display_active(void) {
  int multi_rank = 0;
#ifdef MPI
  multi_rank = (NTask > 1);
#endif
  return (!multi_rank && isatty(STDOUT_FILENO) && get_log_level() <= LOG_LEVEL_INFO);
}

void progress_bar_update(struct ProgressBar *pb, int64_t current) {
  pb->current = current;

  if (pb->live) {
    if (!live_display.active)
      return;
    double now = now_seconds();
    if (progress_bar_should_redraw(current, pb->total, pb->last_drawn_pct, now - pb->last_draw)) {
      live_display.bar = *pb;
      live_display.drawn = 1;
      live_render(now);
      live_write_frame("");
      pb->last_draw = now;
      pb->last_drawn_pct = percent_done(current, pb->total);
    }
    return;
  }

  int pct = (pb->total > 0) ? (int)(100LL * current / pb->total) : 100;
  if (pct > 100)
    pct = 100;
  if (pct >= pb->last_log_pct + PROGRESS_LOG_PCT_STEP) {
    pb->last_log_pct = pct - (pct % PROGRESS_LOG_PCT_STEP);
    char line[PROGRESS_LINE_CAPACITY];
    double elapsed = now_seconds() - pb->start_time;
    progress_bar_format(line, sizeof(line), current, pb->total, elapsed, PROGRESS_BAR_WIDTH,
                        MimicLogUseColor, pb->label);
    INFO_LOG("%s", line);
  }
}

void progress_bar_finish(struct ProgressBar *pb) {
  pb->current = pb->total;

  if (pb->live) {
    if (!live_display.active)
      return;
    live_display.bar = *pb;
    live_render(now_seconds());
    live_write_frame(" " PROGRESS_DONE_MARK);
    live_stop();
    return;
  }

  if (pb->last_log_pct < 100) {
    char line[PROGRESS_LINE_CAPACITY];
    double elapsed = now_seconds() - pb->start_time;
    progress_bar_format(line, sizeof(line), pb->current, pb->total, elapsed, PROGRESS_BAR_WIDTH,
                        MimicLogUseColor, pb->label);
    INFO_LOG("%s " PROGRESS_DONE_MARK, line);
  }
}
