# Plan: WARNING log capture for dependency enforcement tests

## Motivation

`tests/unit/test_module_configuration.c` contains 3 WARNING-class dependency tests:

- `test_dep_apply_sfn_warns_no_prescriptions`
- `test_dep_resolve_mergers_warns_no_clock`
- `test_dep_starburst_warns_no_disk_instability`

These tests assert `result == 0` — i.e., that `module_system_init()` succeeds when a
WARNING-only predecessor is absent. That is correct, but it is not sufficient: a future
change could silently remove the `log_warning()` call inside those `init()` functions and
the test would still pass. The warning would be gone with no signal.

The fix is to capture log output during the init call and assert that the expected warning
string appears in it. Without this, WARNING-class tests provide weaker guarantees than the
ERROR-class tests alongside them.

## Scope

Three tests in `tests/unit/test_module_configuration.c`. No other test files are affected.

The modules whose `init()` functions emit the warnings:
- `sage_apply_star_formation_supernova_init()` — warns if no star-formation prescription
  module is configured in phase_1 ahead of it
- `sage_resolve_mergers_and_disruption_init()` — warns if `sage_initialise_merger_clock`
  is not configured anywhere
- `sage_starburst_feedback_init()` — warns if neither `sage_disk_instability` nor
  `sage_resolve_mergers_and_disruption` is configured anywhere

## Implementation approach

Use **stderr redirect** (Option A). No changes to the logging infrastructure are required.

The project's `log_warning()` writes to stderr. Redirect stderr to a temp file before the
init call, restore it after, then read the temp file and assert the warning string.

### Helper pattern (add once at the top of the test file or in a local header)

```c
#include <unistd.h>   /* dup, dup2, close */
#include <fcntl.h>    /* open, O_RDWR, O_CREAT, O_TRUNC */

/*
 * Redirect stderr to a temp file. Returns the saved stderr fd.
 * Caller must pass saved_stderr to restore_stderr() after the call under test.
 */
static int capture_stderr_begin(const char *tmpfile) {
    int saved = dup(STDERR_FILENO);
    int fd = open(tmpfile, O_RDWR | O_CREAT | O_TRUNC, 0600);
    dup2(fd, STDERR_FILENO);
    close(fd);
    return saved;
}

/*
 * Restore stderr and return 1 if needle appears anywhere in the captured file.
 */
static int capture_stderr_end(int saved_stderr, const char *tmpfile,
                               const char *needle) {
    fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    close(saved_stderr);

    FILE *f = fopen(tmpfile, "r");
    if (!f) return 0;
    char buf[4096] = {0};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    remove(tmpfile);
    return strstr(buf, needle) != NULL;
}
```

### Test body pattern

```c
static int test_dep_apply_sfn_warns_no_prescriptions(void) {
    reset_config();
    set_test_model_parameters();
    /* ... configure phase_1 with sage_apply_star_formation_supernova but no
       star-formation prescription module ahead of it ... */

    const char *tmp = "/tmp/test_warn_sfn.txt";
    int saved = capture_stderr_begin(tmp);
    int result = module_system_init();
    int warned = capture_stderr_end(saved, tmp, "star formation prescription");

    TEST_ASSERT(result == 0,
        "apply_sfn init must succeed (WARNING only) with no prescription");
    TEST_ASSERT(warned,
        "apply_sfn init must emit a warning when no prescription is configured");
    return 1;
}
```

Apply the same pattern to the other two WARNING tests, substituting the appropriate
warning needle strings. Use the actual strings emitted by each module's `init()` function
(check the `log_warning()` call sites in each `.c` file).

## Warning needle strings

Read the `log_warning()` call in each module's `init()` and use a stable substring —
long enough to be unambiguous, short enough to survive minor message rewording:

| Test | Module source file | Suggested needle |
|------|--------------------|-----------------|
| `test_dep_apply_sfn_warns_no_prescriptions` | `sage_apply_star_formation_supernova.c` | extract from `log_warning(...)` call |
| `test_dep_resolve_mergers_warns_no_clock` | `sage_resolve_mergers_and_disruption.c` | extract from `log_warning(...)` call |
| `test_dep_starburst_warns_no_disk_instability` | `sage_starburst_feedback.c` | extract from `log_warning(...)` call |

Use a substring, not the full message. Full messages are fragile to rewording; a
distinctive 4–6 word phrase is enough.

## Validation

After implementation:

```bash
make test-unit > ignore/test-logs/test-unit.log 2>&1
test_rc=$?
tail -n 40 ignore/test-logs/test-unit.log
echo "exit_code=${test_rc}"
```

All 28 unit test suites must pass. Confirm the 3 WARNING tests now also assert `warned`
in addition to `result == 0`.

Additionally, manually verify the capture works: temporarily remove a `log_warning()` call
from one `init()`, rebuild, run the test, confirm it fails on the `warned` assertion, then
restore.

## Files to change

| File | Change |
|------|--------|
| `tests/unit/test_module_configuration.c` | Add `capture_stderr_begin/end` helpers; update the 3 WARNING test bodies to call them and assert the warned result |

No other files need changing. The logging system, module source files, and build system
are untouched.

## Effort

~2 hours. Self-contained to one test file.
