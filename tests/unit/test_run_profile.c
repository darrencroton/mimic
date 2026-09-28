/**
 * @file    test_run_profile.c
 * @brief   Unit tests for the run memory profile's retention term.
 *
 * run_profile_note_retention() keeps two independent run-level maxima -- the
 * most generations the horizontal driver retained at once, and the most bytes
 * resident across its retention pool -- and print_run_memory_profile() reports
 * them, or reports the term as unmeasured when nothing was ever noted. The
 * profile's state is file-static and accumulates for the life of the process,
 * so each case runs in a forked child and the parent reads what it printed.
 */

#include "../../src/util/error.h"
#include "../../src/util/memory.h"
#include "../../src/util/run_profile.h"
#include "../framework/test_framework.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

#define MAX_NOTES 4

/* One retention note: the generations and resident bytes at one point. */
struct RetentionNote {
  int64_t generations;
  int64_t bytes;
};

/* Note `count` retention points in a child, print the profile, and capture it.
 * Returns 0 when the child ran and exited cleanly. */
static int profile_after_notes(const struct RetentionNote *notes, int count, char *output,
                               size_t output_size) {
  int pipefd[2];

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
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    /* WARNING, as under --quiet: the printer must still emit the block. */
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);
    for (int i = 0; i < count; i++) {
      run_profile_note_retention(notes[i].generations, notes[i].bytes);
    }
    print_run_memory_profile();
    fflush(NULL);
    _exit(0);
  }

  /* Read to EOF even once the buffer is full, discarding the excess, so the
   * child can never block on a full pipe while waitpid() waits for it. */
  close(pipefd[1]);
  size_t used = 0;
  char discard[1024] = {0};
  for (;;) {
    const int keeping = used < output_size - 1;
    char *into = keeping ? output + used : discard;
    const size_t room = keeping ? output_size - 1 - used : sizeof(discard);
    const ssize_t nread = read(pipefd[0], into, room);
    if (nread <= 0) {
      break;
    }
    if (keeping) {
      used += (size_t)nread;
    }
  }
  output[used] = '\0';
  close(pipefd[0]);

  int status;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/**
 * @test    test_retention_maxima_are_independent
 * @brief   The generations and bytes maxima come from different notes, and each
 *          is the largest noted, not the last or a sum.
 */
int test_retention_maxima_are_independent(void) {
  const struct RetentionNote notes[MAX_NOTES] = {{2, 1000}, {5, 400}, {3, 987654321012}, {1, 50}};
  char output[4096];

  TEST_ASSERT(profile_after_notes(notes, MAX_NOTES, output, sizeof(output)) == 0,
              "The profile child should run and exit cleanly");
  TEST_ASSERT(strstr(output, "Run memory profile (GB = 1e9 B):") != NULL,
              "The profile block should be printed even at WARNING level");
  TEST_ASSERT(strstr(output, "Retained generations R: at most 5 concurrently") != NULL,
              "The generations maximum should be the largest noted (5), from the second note");
  TEST_ASSERT(strstr(output, "Retention pool resident: at most 987654321012 B = 987.654 GB") !=
                  NULL,
              "The bytes maximum should be the largest noted, from the third note, in GB = 1e9 B");
  TEST_ASSERT(strstr(output, "unmeasured (no horizontal generation was retained)") == NULL,
              "A noted term should not be reported as unmeasured");
  return TEST_PASS;
}

/**
 * @test    test_retention_unnoted_is_unmeasured
 * @brief   A run that noted no retention reports the term as unmeasured, not as
 *          zero.
 */
int test_retention_unnoted_is_unmeasured(void) {
  char output[4096];

  TEST_ASSERT(profile_after_notes(NULL, 0, output, sizeof(output)) == 0,
              "The profile child should run and exit cleanly");
  TEST_ASSERT(strstr(output, "Retained generations R: unmeasured (no horizontal generation was "
                             "retained)") != NULL,
              "An unnoted retention term should be reported as unmeasured");
  TEST_ASSERT(strstr(output, "Retention pool resident:") == NULL,
              "An unnoted retention term should print no byte figure");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Run Memory Profile\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_retention_maxima_are_independent);
  TEST_RUN(test_retention_unnoted_is_unmeasured);

  TEST_SUMMARY();
  return TEST_RESULT();
}
