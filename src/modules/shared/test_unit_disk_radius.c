/**
 * @file test_unit_disk_radius.c
 * @brief Unit tests for disk radius calculation utilities
 *
 * Tests the shared disk_radius utility functions for correctness,
 * edge cases, and numerical stability.
 */

#include "../../../tests/framework/test_framework.h"
#include "disk_radius.h"
#include <math.h>
#include <stdio.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/**
 * @brief Test spin magnitude calculation
 */
int test_spin_magnitude(void) {
  float spin_mag;

  // Test case 1: Simple case with known result
  // |J| = sqrt(3^2 + 4^2 + 0^2) = 5.0
  spin_mag = mimic_get_spin_magnitude(3.0f, 4.0f, 0.0f);
  TEST_ASSERT(fabsf(spin_mag - 5.0f) < 1e-5f,
              "Spin magnitude (3,4,0) should be 5.0");

  // Test case 2: Equal components
  // |J| = sqrt(1^2 + 1^2 + 1^2) = sqrt(3) ≈ 1.732
  spin_mag = mimic_get_spin_magnitude(1.0f, 1.0f, 1.0f);
  TEST_ASSERT(fabsf(spin_mag - 1.732051f) < 1e-5f,
              "Spin magnitude (1,1,1) should be sqrt(3)");

  // Test case 3: Zero spin
  spin_mag = mimic_get_spin_magnitude(0.0f, 0.0f, 0.0f);
  TEST_ASSERT(fabsf(spin_mag) < 1e-10f, "Zero spin should give zero magnitude");

  // Test case 4: Negative components (still positive magnitude)
  spin_mag = mimic_get_spin_magnitude(-3.0f, -4.0f, 0.0f);
  TEST_ASSERT(fabsf(spin_mag - 5.0f) < 1e-5f,
              "Negative components should give positive magnitude");

  return TEST_PASS;
}

/**
 * @brief Test spin parameter calculation
 */
int test_spin_parameter(void) {
  float lambda;

  // Test case 1: Typical halo spin parameter
  // lambda = |J| / (sqrt(2) * Vvir * Rvir)
  // lambda = 10.0 / (1.414 * 100.0 * 0.2) = 10.0 / 28.28 ≈ 0.3536
  lambda = mimic_get_spin_parameter(10.0f, 100.0f, 0.2f);
  TEST_ASSERT(fabsf(lambda - 0.3536f) < 1e-3f,
              "Spin parameter calculation should be correct");

  // Test case 2: Zero virial velocity (should return 0)
  lambda = mimic_get_spin_parameter(10.0f, 0.0f, 0.2f);
  TEST_ASSERT(fabsf(lambda) < 1e-10f, "Zero Vvir should give lambda=0");

  // Test case 3: Zero virial radius (should return 0)
  lambda = mimic_get_spin_parameter(10.0f, 100.0f, 0.0f);
  TEST_ASSERT(fabsf(lambda) < 1e-10f, "Zero Rvir should give lambda=0");

  // Test case 4: Negative virial properties (should return 0)
  lambda = mimic_get_spin_parameter(10.0f, -100.0f, 0.2f);
  TEST_ASSERT(fabsf(lambda) < 1e-10f, "Negative Vvir should give lambda=0");

  // Test case 5: Very small values (numerical stability)
  lambda = mimic_get_spin_parameter(1e-15f, 100.0f, 0.2f);
  TEST_ASSERT(fabsf(lambda) < 1e-10f,
              "Very small spin should give small lambda");

  return TEST_PASS;
}

/**
 * @brief Test disk radius calculation with valid inputs
 */
int test_disk_radius_valid(void) {
  float disk_r;

  // Test case 1: Typical galaxy disk
  // Using spin (3, 4, 0), Vvir=100 km/s, Rvir=0.2 Mpc/h
  // |J| = 5.0
  // lambda = 5.0 / (1.414 * 100.0 * 0.2) = 5.0 / 28.28 ≈ 0.1768
  // Rd = lambda / sqrt(2) * Rvir = 0.1768 / 1.414 * 0.2 ≈ 0.025
  disk_r = mimic_get_disk_radius(3.0f, 4.0f, 0.0f, 100.0f, 0.2f);
  TEST_ASSERT(fabsf(disk_r - 0.025f) < 1e-3f,
              "Disk radius calculation should be correct");

  // Test case 2: High spin halo (larger disk)
  disk_r = mimic_get_disk_radius(10.0f, 10.0f, 10.0f, 100.0f, 0.2f);
  TEST_ASSERT(disk_r > 0.0f, "High spin should give positive disk radius");
  TEST_ASSERT(disk_r < 0.2f, "Disk radius should be less than virial radius");

  // Test case 3: Low spin halo (smaller disk)
  disk_r = mimic_get_disk_radius(0.1f, 0.1f, 0.1f, 100.0f, 0.2f);
  TEST_ASSERT(disk_r > 0.0f, "Low spin should still give positive disk radius");
  TEST_ASSERT(disk_r < 0.05f, "Low spin should give small disk radius");

  return TEST_PASS;
}

/**
 * @brief Test disk radius calculation with edge cases
 */
int test_disk_radius_edge_cases(void) {
  float disk_r;

  // Test case 1: Zero virial velocity (fallback to 0.1 * Rvir)
  disk_r = mimic_get_disk_radius(3.0f, 4.0f, 0.0f, 0.0f, 0.2f);
  TEST_ASSERT(fabsf(disk_r - 0.02f) < 1e-5f,
              "Zero Vvir should use fallback: 0.1*Rvir");

  // Test case 2: Zero virial radius (fallback)
  disk_r = mimic_get_disk_radius(3.0f, 4.0f, 0.0f, 100.0f, 0.0f);
  TEST_ASSERT(fabsf(disk_r) < 1e-10f, "Zero Rvir should give zero disk radius");

  // Test case 3: Negative virial properties (fallback)
  disk_r = mimic_get_disk_radius(3.0f, 4.0f, 0.0f, -100.0f, 0.2f);
  TEST_ASSERT(fabsf(disk_r - 0.02f) < 1e-5f,
              "Negative Vvir should use fallback");

  // Test case 4: Zero spin vector (valid calculation, gives small disk)
  disk_r = mimic_get_disk_radius(0.0f, 0.0f, 0.0f, 100.0f, 0.2f);
  TEST_ASSERT(disk_r >= 0.0f, "Zero spin should give non-negative disk radius");

  // Test case 5: Very large virial radius
  disk_r = mimic_get_disk_radius(3.0f, 4.0f, 0.0f, 100.0f, 10.0f);
  TEST_ASSERT(disk_r > 0.0f && disk_r < 10.0f,
              "Large Rvir should scale disk radius appropriately");

  return TEST_PASS;
}

/**
 * @brief Test physical plausibility of disk radius
 */
int test_disk_radius_physical_plausibility(void) {
  float disk_r;

  // Test case 1: Disk radius should be smaller than virial radius
  // for typical spin parameters (lambda ~ 0.03-0.05)
  disk_r = mimic_get_disk_radius(1.0f, 1.0f, 1.0f, 100.0f, 0.2f);
  TEST_ASSERT(disk_r < 0.2f, "Disk radius should be less than virial radius");

  // Test case 2: Disk radius should be independent of Rvir (for fixed |J| and
  // Vvir) In Mo98 model: Rd = |J| / (2 * Vvir), which doesn't depend on Rvir
  float disk_r1 = mimic_get_disk_radius(3.0f, 4.0f, 0.0f, 100.0f, 0.2f);
  float disk_r2 = mimic_get_disk_radius(3.0f, 4.0f, 0.0f, 100.0f, 0.4f);
  TEST_ASSERT(
      fabsf(disk_r2 - disk_r1) < 1e-5f,
      "Disk radius should be independent of Rvir for fixed |J| and Vvir");

  // Test case 3: Disk radius should scale with spin magnitude
  float disk_r_low = mimic_get_disk_radius(1.0f, 1.0f, 1.0f, 100.0f, 0.2f);
  float disk_r_high = mimic_get_disk_radius(10.0f, 10.0f, 10.0f, 100.0f, 0.2f);
  TEST_ASSERT(disk_r_high > disk_r_low,
              "Higher spin should give larger disk radius");

  return TEST_PASS;
}

/**
 * @brief Test consistency with Mo, Mao & White (1998) formulation
 */
int test_mo98_consistency(void) {
  // Reference case from Mo, Mao & White (1998)
  // For a typical halo with:
  // - lambda ~ 0.05 (typical spin parameter)
  // - Rvir = 0.2 Mpc/h
  // Expected: Rd ~ 0.007 Mpc/h (i.e., Rd ~ lambda * Rvir / sqrt(2) ~ 0.05 * 0.2
  // / 1.414)

  // Set up spin to give lambda ~ 0.05
  // lambda = |J| / (sqrt(2) * Vvir * Rvir)
  // 0.05 = |J| / (1.414 * 100 * 0.2)
  // |J| = 0.05 * 28.28 = 1.414
  // Use spin vector that gives |J| = 1.414: (1, 1, 0) gives |J| = 1.414

  float disk_r = mimic_get_disk_radius(1.0f, 1.0f, 0.0f, 100.0f, 0.2f);

  // Expected: Rd = (lambda / sqrt(2)) * Rvir = (0.05 / 1.414) * 0.2 = 0.00707
  TEST_ASSERT(fabsf(disk_r - 0.00707f) < 1e-3f,
              "Disk radius should match Mo98 prediction");

  return TEST_PASS;
}

/**
 * @brief Main test runner
 */
int main(void) {
  printf("\n============================================================\n");
  printf("Disk Radius Utilities - Unit Tests\n");
  printf("============================================================\n");

  TEST_RUN(test_spin_magnitude);
  TEST_RUN(test_spin_parameter);
  TEST_RUN(test_disk_radius_valid);
  TEST_RUN(test_disk_radius_edge_cases);
  TEST_RUN(test_disk_radius_physical_plausibility);
  TEST_RUN(test_mo98_consistency);

  TEST_SUMMARY();
  return TEST_RESULT();
}
