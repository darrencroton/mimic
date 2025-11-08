/**
 * @file    test_stubs.c
 * @brief   Stub implementations for unit tests
 *
 * Provides minimal implementations of functions from main.c that are
 * needed by unit tests but can't be linked from main.c (which has main()).
 *
 * @author  Mimic Testing Team
 * @date    2025-11-08
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
