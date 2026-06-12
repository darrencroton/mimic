/**
 * @file    test_stubs.c
 * @brief   Stub implementations for unit tests
 *
 * Provides minimal implementations of functions from main.c that are
 * needed by unit tests but can't be linked from main.c (which has main()).
 *
 * Model knowledge does not belong here: model parameter fixtures live in the
 * model package (e.g. models/sage16/modules/_tests/sage_test_fixtures.h), so
 * each model brings its own alongside its tests.
 */

#include <stdio.h>
#include <stdlib.h>

/**
 * @brief   Test version of myexit - just call exit()
 *
 * This is a simplified version for tests. The real myexit() in main.c
 * includes MPI cleanup and other teardown logic.
 */
void myexit(int signum) {
  printf("Test exiting with code %d\n", signum);
  exit(signum);
}
