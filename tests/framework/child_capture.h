/**
 * @file    child_capture.h
 * @brief   Run a test body in a forked child and assert on how it ended.
 *
 * FATAL_ERROR ends the process, so a test that pins an abort (or pins that a
 * near-limit input does not abort) runs the code under test in a child: the
 * child's stdout goes to /dev/null, its stderr is captured through a pipe, and
 * the parent inspects the wait status and the captured text.
 *
 * One implementation for every C test that needs it, so the harness guarantees
 * cannot drift between copies:
 *   - a harness failure (pipe, fork, waitpid) is reported as -1, never as a
 *     pass or an ordinary failure;
 *   - output that fills the capture buffer is reported as -1 (truncated), on
 *     both the abort and the success path, because a needle beyond the cut
 *     would otherwise read as "abort message missing" and a truncated success
 *     log would hide what the child printed;
 *   - on any mismatch the captured output is printed, so a wrong abort message
 *     is diagnosable rather than a bare failure.
 *
 * Callers assert on `== 1`. Include after test_framework.h; paths resolve via
 * the -I flags set by tests/unit/run_tests.sh, or relatively from a
 * package-local test (see simulations/<sim>/_tests/unit/).
 */

#ifndef CHILD_CAPTURE_H
#define CHILD_CAPTURE_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/** Capture buffer size, in bytes, including the terminating NUL. */
#define CHILD_CAPTURE_BYTES 65536

/** A test body run in the child. `arg` is passed through unchanged (usually a fixture path). */
typedef void (*child_body_fn)(const char *arg);

/**
 * @brief   Run `body(arg)` in a forked child and capture its stderr.
 * @param   output       Receives the NUL-terminated captured stderr.
 * @param   output_size  Size of `output`; must be at least 2.
 * @return  The child's wait status, or -1 on a harness failure or truncated capture.
 *
 * A body that returns normally makes the child exit 0.
 */
static inline int child_capture_run(const char *arg, child_body_fn body, char *output,
                                    size_t output_size) {
  int pipefd[2];
  size_t used = 0;
  ssize_t nread;
  int status;

  output[0] = '\0';
  fflush(NULL);
  if (pipe(pipefd) != 0) {
    return -1;
  }
  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    close(pipefd[0]);
    if (freopen("/dev/null", "w", stdout) == NULL) {
      _exit(127);
    }
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    body(arg);
    _exit(0);
  }

  close(pipefd[1]);
  while (used < output_size - 1 &&
         (nread = read(pipefd[0], output + used, output_size - 1 - used)) > 0) {
    used += (size_t)nread;
  }
  output[used] = '\0';
  close(pipefd[0]);
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (used == output_size - 1) {
    fprintf(stderr, "  child output truncated at %zu bytes; raise CHILD_CAPTURE_BYTES\n",
            output_size - 1);
    return -1;
  }
  return status;
}

/**
 * @brief   Require `body` to abort (exit non-zero) with both needles in its stderr.
 * @param   needle_a, needle_b  Required fragments; NULL means "not required".
 * @param   captured            Optional copy of the captured output (may be NULL).
 * @return  1 on a matching abort, 0 otherwise (output printed), -1 on a harness failure.
 */
static inline int expect_fatal_capture(const char *arg, child_body_fn body, const char *needle_a,
                                       const char *needle_b, char *captured, size_t captured_size) {
  static char output[CHILD_CAPTURE_BYTES];
  const int status = child_capture_run(arg, body, output, sizeof(output));
  if (captured != NULL && captured_size > 0) {
    snprintf(captured, captured_size, "%s", output);
  }
  if (status == -1) {
    return -1;
  }
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "  child died on signal %d; captured output:\n%s\n", WTERMSIG(status), output);
    return 0;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) == 0) {
    fprintf(stderr, "  child did not abort; captured output:\n%s\n", output);
    return 0;
  }
  if ((needle_a != NULL && strstr(output, needle_a) == NULL) ||
      (needle_b != NULL && strstr(output, needle_b) == NULL)) {
    fprintf(stderr, "  abort message missing an expected fragment\n");
    fprintf(stderr, "    wanted: '%s' and '%s'\n", needle_a != NULL ? needle_a : "(any)",
            needle_b != NULL ? needle_b : "(any)");
    fprintf(stderr, "    got:\n%s\n", output);
    return 0;
  }
  return 1;
}

/** @brief expect_fatal_capture() without access to the captured output. */
static inline int expect_fatal(const char *arg, child_body_fn body, const char *needle_a,
                               const char *needle_b) {
  return expect_fatal_capture(arg, body, needle_a, needle_b, NULL, 0);
}

/**
 * @brief   Require `body` to run to completion without aborting.
 * @return  1 when the child exited 0, 0 when it aborted or was signalled (output
 *          printed), -1 on a harness failure or truncated capture.
 *
 * The mirror image of expect_fatal_capture(): pins the accepted side of a limit.
 */
static inline int expect_success(const char *arg, child_body_fn body) {
  static char output[CHILD_CAPTURE_BYTES];
  const int status = child_capture_run(arg, body, output, sizeof(output));
  if (status == -1) {
    return -1;
  }
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "  child died on signal %d; captured output:\n%s\n", WTERMSIG(status), output);
    return 0;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "  child aborted; captured output:\n%s\n", output);
    return 0;
  }
  return 1;
}

#endif /* CHILD_CAPTURE_H */
