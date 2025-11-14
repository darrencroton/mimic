/**
 * @file    test_fixture.h
 * @brief   Test fixture module interface
 *
 * ⚠️  WARNING: This module is for TESTING INFRASTRUCTURE ONLY ⚠️
 *
 * DO NOT USE IN PRODUCTION RUNS
 *
 * Purpose: Provides a minimal, stable module for testing core module system
 * functionality (configuration, registration, pipeline execution) without
 * coupling infrastructure tests to production physics modules.
 *
 * This maintains Vision Principle #1: Physics-Agnostic Core Infrastructure
 *
 * The module performs minimal operations:
 * - Reads test parameters from configuration
 * - Sets TestDummyProperty = 1.0 on all galaxies
 * - Logs initialization and processing (when EnableLogging=1)
 *
 * @author  Mimic Development Team
 * @date    2025-11-13
 */

#ifndef TEST_FIXTURE_H
#define TEST_FIXTURE_H

/**
 * @brief   Register the test fixture module
 *
 * Registers this module with the module registry. Should be called once
 * during program initialization before module_system_init().
 *
 * WARNING: This module is for testing infrastructure only.
 */
void test_fixture_register(void);

#endif // TEST_FIXTURE_H
