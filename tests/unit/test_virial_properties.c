/**
 * @file    test_virial_properties.c
 * @brief   Unit tests for virial property calculations
 *
 * Validates: Halo virial mass, radius, and velocity calculations
 * Phase: Phase 4 (Test Coverage Enhancement)
 *
 * This test validates that Mimic's virial property calculations correctly:
 * - Calculate virial mass from Mvir or particle count
 * - Calculate virial radius from mass and cosmology
 * - Calculate virial velocity from mass and radius
 * - Maintain consistency relations (e.g., Vvir ∝ Mvir^(1/3))
 * - Handle edge cases (zero values, satellites)
 *
 * Test cases:
 *   - test_virial_mass_from_mvir: Virial mass when Mvir available
 *   - test_virial_mass_from_particles: Virial mass from particle count
 *   - test_virial_radius_calculation: Virial radius physics
 *   - test_virial_velocity_calculation: Virial velocity from mass/radius
 *   - test_virial_consistency_relations: Vvir^2 ∝ Mvir/Rvir
 *   - test_virial_edge_cases: Zero mass, satellites
 *
 * @author  Mimic Testing Team
 * @date    2025-11-23
 */

#include "../framework/test_framework.h"
#include "../../src/include/types.h"
#include "../../src/include/proto.h"
#include "../../src/util/memory.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* External globals */
extern struct MimicConfig MimicConfig;
extern struct RawHalo *InputTreeHalos;
extern double *Age_base;  /* For cleanup of init() allocation */

/**
 * @test    test_virial_mass_from_mvir
 * @brief   Test virial mass calculation when Mvir is available
 *
 * Expected: Returns Mvir value from InputTreeHalos for central halos
 * Validates: get_virial_mass() with Mvir >= 0
 */
int test_virial_mass_from_mvir(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Create test halo with known Mvir */
    InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo), MEM_HALOS);
    InputTreeHalos[0].Mvir = 100.0;  /* 10^12 Msun/h */
    InputTreeHalos[0].FirstHaloInFOFgroup = 0;  /* Central halo */
    InputTreeHalos[0].Len = 1000;

    MimicConfig.PartMass = 0.1;  /* Particle mass in 10^10 Msun/h */

    /* ===== EXECUTE ===== */
    double mvir = get_virial_mass(0);

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(mvir, 100.0, 1e-6,
                             "Virial mass should match Mvir for central halo");

    printf("  Mvir from catalog: %.2f (10^10 Msun/h)\n", mvir);

    /* ===== CLEANUP ===== */
    myfree(InputTreeHalos);
    InputTreeHalos = NULL;
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_virial_mass_from_particles
 * @brief   Test virial mass calculation from particle count
 *
 * Expected: Returns Len * PartMass when Mvir not available
 * Validates: get_virial_mass() fallback for satellites or Mvir < 0
 */
int test_virial_mass_from_particles(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Create satellite halo (not FirstHaloInFOFgroup) */
    InputTreeHalos = mymalloc_cat(2 * sizeof(struct RawHalo), MEM_HALOS);
    InputTreeHalos[0].Mvir = 100.0;
    InputTreeHalos[0].FirstHaloInFOFgroup = 0;

    InputTreeHalos[1].Mvir = -1.0;  /* Satellite - Mvir not available */
    InputTreeHalos[1].FirstHaloInFOFgroup = 0;  /* Points to central */
    InputTreeHalos[1].Len = 500;

    MimicConfig.PartMass = 0.1;  /* 10^9 Msun/h per particle */

    /* ===== EXECUTE ===== */
    double mvir = get_virial_mass(1);  /* Satellite halo */

    /* ===== VALIDATE ===== */
    double expected = 500 * 0.1;  /* Len * PartMass = 50.0 */
    TEST_ASSERT_DOUBLE_EQUAL(mvir, expected, 1e-6,
                             "Virial mass should be Len * PartMass for satellite");

    printf("  Mvir from particles: %.2f = %d * %.2f (10^10 Msun/h)\n",
           mvir, InputTreeHalos[1].Len, MimicConfig.PartMass);

    /* ===== CLEANUP ===== */
    myfree(InputTreeHalos);
    InputTreeHalos = NULL;
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_virial_radius_calculation
 * @brief   Test virial radius calculation from mass and cosmology
 *
 * Expected: Rvir = [3*Mvir / (4*π*200*ρcrit)]^(1/3)
 * Validates: get_virial_radius() physics calculation
 */
int test_virial_radius_calculation(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Load parameter file to get cosmology */
    read_parameter_file("tests/data/test_binary.yaml");
    init();  /* Initializes cosmology and ZZ[] array */

    /* Create test halo at z=0 (snapshot 63) */
    InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo), MEM_HALOS);
    InputTreeHalos[0].Mvir = 100.0;  /* 10^12 Msun/h */
    InputTreeHalos[0].FirstHaloInFOFgroup = 0;
    InputTreeHalos[0].SnapNum = 63;  /* z=0 */

    /* ===== EXECUTE ===== */
    double rvir = get_virial_radius(0);

    /* ===== VALIDATE ===== */
    /* At z=0 with standard cosmology (Omega=0.25, OmegaLambda=0.75, h=0.73):
     * Critical density ρcrit ≈ 277 Msun/Mpc^3 (in physical units)
     * For Mvir = 10^12 Msun/h, Rvir ≈ 0.2-0.3 Mpc/h
     */
    TEST_ASSERT(rvir > 0.0, "Virial radius should be positive");
    TEST_ASSERT(rvir < 10.0, "Virial radius should be reasonable (< 10 Mpc/h)");
    TEST_ASSERT(isfinite(rvir), "Virial radius should be finite");

    printf("  Mvir = %.2f (10^10 Msun/h) → Rvir = %.4f (Mpc/h) at z=0\n",
           InputTreeHalos[0].Mvir, rvir);

    /* Test scaling: Rvir ∝ Mvir^(1/3) */
    InputTreeHalos[0].Mvir = 800.0;  /* 8x mass */
    double rvir_8x = get_virial_radius(0);
    double ratio = rvir_8x / rvir;
    TEST_ASSERT_DOUBLE_EQUAL(ratio, 2.0, 0.01,
                             "Rvir should scale as Mvir^(1/3): 8x mass → 2x radius");

    printf("  Scaling test: 8x mass → %.2fx radius (expected 2.0)\n", ratio);

    /* ===== CLEANUP ===== */
    myfree(InputTreeHalos);
    InputTreeHalos = NULL;
    myfree(Age_base);  /* Free Age array allocated by init() */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_virial_velocity_calculation
 * @brief   Test virial velocity calculation from mass and radius
 *
 * Expected: Vvir = sqrt(G * Mvir / Rvir)
 * Validates: get_virial_velocity() physics calculation
 */
int test_virial_velocity_calculation(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    /* Load parameter file to get cosmology and G */
    read_parameter_file("tests/data/test_binary.yaml");
    init();

    /* Create test halo */
    InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo), MEM_HALOS);
    InputTreeHalos[0].Mvir = 100.0;  /* 10^12 Msun/h */
    InputTreeHalos[0].FirstHaloInFOFgroup = 0;
    InputTreeHalos[0].SnapNum = 63;  /* z=0 */

    /* ===== EXECUTE ===== */
    double vvir = get_virial_velocity(0);
    double rvir = get_virial_radius(0);
    double mvir = get_virial_mass(0);

    /* ===== VALIDATE ===== */
    /* For Mvir = 10^12 Msun/h, Vvir ~ 100-200 km/s is typical */
    TEST_ASSERT(vvir > 0.0, "Virial velocity should be positive");
    TEST_ASSERT(vvir < 1000.0, "Virial velocity should be reasonable (< 1000 km/s)");
    TEST_ASSERT(isfinite(vvir), "Virial velocity should be finite");

    /* Verify formula: Vvir = sqrt(G * Mvir / Rvir) */
    double vvir_expected = sqrt(MimicConfig.G * mvir / rvir);
    TEST_ASSERT_DOUBLE_EQUAL(vvir, vvir_expected, 1e-6,
                             "Vvir should match sqrt(G*Mvir/Rvir)");

    printf("  Mvir = %.2f, Rvir = %.4f → Vvir = %.2f km/s\n",
           mvir, rvir, vvir);
    printf("  Formula check: sqrt(%.2f * %.2f / %.4f) = %.2f km/s\n",
           MimicConfig.G, mvir, rvir, vvir_expected);

    /* ===== CLEANUP ===== */
    myfree(InputTreeHalos);
    InputTreeHalos = NULL;
    myfree(Age_base);  /* Free Age array allocated by init() */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_virial_consistency_relations
 * @brief   Test consistency relations between virial properties
 *
 * Expected: Vvir^2 = G * Mvir / Rvir, Vvir ∝ Mvir^(1/3)
 * Validates: Internal consistency of virial calculations
 */
int test_virial_consistency_relations(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    read_parameter_file("tests/data/test_binary.yaml");
    init();

    InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo), MEM_HALOS);
    InputTreeHalos[0].Mvir = 100.0;
    InputTreeHalos[0].FirstHaloInFOFgroup = 0;
    InputTreeHalos[0].SnapNum = 63;

    /* ===== EXECUTE ===== */
    double mvir1 = get_virial_mass(0);
    double rvir1 = get_virial_radius(0);
    double vvir1 = get_virial_velocity(0);

    /* Test 8x mass */
    InputTreeHalos[0].Mvir = 800.0;
    double vvir2 = get_virial_velocity(0);

    /* ===== VALIDATE ===== */
    /* Consistency: Vvir^2 = G * Mvir / Rvir */
    double vvir_sq = vvir1 * vvir1;
    double expected_vvir_sq = MimicConfig.G * mvir1 / rvir1;
    TEST_ASSERT_DOUBLE_EQUAL(vvir_sq, expected_vvir_sq, 1e-3,
                             "Vvir^2 should equal G*Mvir/Rvir");

    /* Scaling: Vvir ∝ Mvir^(1/3) since Rvir ∝ Mvir^(1/3) */
    double vvir_ratio = vvir2 / vvir1;
    TEST_ASSERT_DOUBLE_EQUAL(vvir_ratio, 2.0, 0.01,
                             "Vvir should scale as Mvir^(1/3): 8x mass → 2x velocity");

    printf("  Consistency check: Vvir^2 = %.2f, G*Mvir/Rvir = %.2f\n",
           vvir_sq, expected_vvir_sq);
    printf("  Scaling: 8x mass → %.2fx velocity (expected 2.0)\n", vvir_ratio);

    /* ===== CLEANUP ===== */
    myfree(InputTreeHalos);
    InputTreeHalos = NULL;
    myfree(Age_base);  /* Free Age array allocated by init() */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_virial_edge_cases
 * @brief   Test edge cases and error handling
 *
 * Expected: Safe handling of zero mass, zero radius, negative values
 * Validates: Robustness of virial calculations
 */
int test_virial_edge_cases(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    read_parameter_file("tests/data/test_binary.yaml");
    init();

    InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo), MEM_HALOS);
    InputTreeHalos[0].FirstHaloInFOFgroup = 0;
    InputTreeHalos[0].SnapNum = 63;
    InputTreeHalos[0].Len = 0;  /* No particles */

    MimicConfig.PartMass = 0.1;

    /* ===== EXECUTE & VALIDATE ===== */

    /* Case 1: Zero mass halo (no particles, Mvir < 0) */
    InputTreeHalos[0].Mvir = -1.0;
    double mvir_zero = get_virial_mass(0);
    TEST_ASSERT_DOUBLE_EQUAL(mvir_zero, 0.0, 1e-6,
                             "Zero particles should give zero mass");

    /* Case 2: Zero mass should give zero velocity (safe_div protection) */
    double rvir_zero = get_virial_radius(0);
    double vvir_zero = get_virial_velocity(0);

    /* Rvir will be very small but not exactly zero due to cbrt(0) = 0 */
    /* Vvir should handle this safely */
    TEST_ASSERT(isfinite(vvir_zero), "Zero mass should produce finite velocity");
    TEST_ASSERT_DOUBLE_EQUAL(vvir_zero, 0.0, 1e-3,
                             "Zero mass should give ~zero velocity");

    printf("  Zero mass edge case: Mvir=%.6f → Rvir=%.6f → Vvir=%.6f\n",
           mvir_zero, rvir_zero, vvir_zero);

    /* Case 3: Very small but non-zero mass */
    InputTreeHalos[0].Mvir = 0.001;  /* 10^7 Msun/h */
    double rvir_small = get_virial_radius(0);
    double vvir_small = get_virial_velocity(0);

    TEST_ASSERT(rvir_small > 0.0, "Small mass should give positive radius");
    TEST_ASSERT(vvir_small > 0.0, "Small mass should give positive velocity");
    TEST_ASSERT(isfinite(rvir_small), "Small mass should give finite radius");
    TEST_ASSERT(isfinite(vvir_small), "Small mass should give finite velocity");

    printf("  Small mass case: Mvir=%.6f → Rvir=%.6f → Vvir=%.6f\n",
           0.001, rvir_small, vvir_small);

    /* ===== CLEANUP ===== */
    myfree(InputTreeHalos);
    InputTreeHalos = NULL;
    myfree(Age_base);  /* Free Age array allocated by init() */
    check_memory_leaks();

    return TEST_PASS;
}

int main(void) {
    printf("============================================================\n");
    printf("RUNNING VIRIAL PROPERTIES UNIT TESTS\n");
    printf("============================================================\n");

    printf("\n========================================\n");
    printf("Test Suite: Virial Property Calculations\n");
    printf("========================================\n");

    TEST_RUN(test_virial_mass_from_mvir);
    TEST_RUN(test_virial_mass_from_particles);
    TEST_RUN(test_virial_radius_calculation);
    TEST_RUN(test_virial_velocity_calculation);
    TEST_RUN(test_virial_consistency_relations);
    TEST_RUN(test_virial_edge_cases);

    printf("============================================================\n");
    TEST_SUMMARY();
    printf("============================================================\n");

    return TEST_RESULT();
}
