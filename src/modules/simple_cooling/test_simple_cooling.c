/**
 * @file    test_simple_cooling.c
 * @brief   Placeholder unit test for simple_cooling module
 *
 * NOTE: simple_cooling is a Phase 3 infrastructure testing module,
 *       not production physics. Comprehensive module testing happens
 *       via test_module_pipeline.c (generic module system tests).
 *
 * Phase: Phase 3 (Runtime Module Configuration)
 */

#include "../../../tests/framework/test_framework.h"

static int passed = 0;
static int failed = 0;

int test_placeholder(void) {
    /* simple_cooling tested via generic module pipeline tests */
    return TEST_PASS;
}

int main(void) {
    TEST_RUN(test_placeholder);
    TEST_SUMMARY();
    return TEST_RESULT();
}
