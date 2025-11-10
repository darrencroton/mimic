/**
 * @file    test_property_metadata.c
 * @brief   Unit tests for property metadata system
 *
 * Validates: Generated property structures and metadata correctness
 * Phase: Phase 2 (Testing Framework)
 *
 * This test validates that the property metadata system correctly:
 * - Generates struct Halo with expected properties
 * - Generates struct GalaxyData with expected properties
 * - Generates struct HaloOutput with expected properties
 * - Maintains consistent sizes and layouts
 * - Properly separates halo tracking from galaxy physics
 *
 * Test cases:
 *   - test_halo_structure: Halo struct contains expected fields
 *   - test_galaxy_structure: GalaxyData struct contains expected fields
 *   - test_output_structure: HaloOutput struct contains expected fields
 *   - test_structure_sizes: Struct sizes are reasonable
 *   - test_galaxy_separation: Galaxy pointer properly separates physics
 *
 * @author  Mimic Testing Team
 * @date    2025-11-08
 */

#include "../framework/test_framework.h"
#include "../../src/include/generated/property_defs.h"
#include "../../src/util/memory.h"
#include "../../src/util/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/**
 * @test    test_halo_structure
 * @brief   Test that Halo structure contains expected properties
 *
 * Expected: Halo struct has core tracking properties
 * Validates: Property metadata generated Halo correctly
 */
int test_halo_structure(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE ===== */
    struct Halo halo;

    /* Initialize to known values to test field access */
    halo.SnapNum = 63;
    halo.Type = 0;
    halo.Mvir = 1.0e12;
    halo.Rvir = 100.0;
    halo.Vvir = 200.0;
    halo.Len = 1000;
    halo.dT = 192.0;
    halo.galaxy = NULL;

    /* ===== VALIDATE ===== */
    /* Check that fields are accessible and values retained */
    TEST_ASSERT(halo.SnapNum == 63, "SnapNum field should be accessible");
    TEST_ASSERT(halo.Type == 0, "Type field should be accessible");
    TEST_ASSERT_DOUBLE_EQUAL(halo.Mvir, 1.0e12, 1.0e6, "Mvir field should be accessible");
    TEST_ASSERT_DOUBLE_EQUAL(halo.Rvir, 100.0, 0.1, "Rvir field should be accessible");
    TEST_ASSERT_DOUBLE_EQUAL(halo.Vvir, 200.0, 0.1, "Vvir field should be accessible");
    TEST_ASSERT(halo.Len == 1000, "Len field should be accessible");
    TEST_ASSERT_DOUBLE_EQUAL(halo.dT, 192.0, 0.1, "dT field should be accessible");
    TEST_ASSERT(halo.galaxy == NULL, "Galaxy pointer should be accessible");

    /* Check that arrays are accessible */
    halo.Pos[0] = 1.0;
    halo.Pos[1] = 2.0;
    halo.Pos[2] = 3.0;
    TEST_ASSERT_DOUBLE_EQUAL(halo.Pos[0], 1.0, 0.01, "Pos array should be accessible");

    halo.Vel[0] = 100.0;
    halo.Vel[1] = 200.0;
    halo.Vel[2] = 300.0;
    TEST_ASSERT_DOUBLE_EQUAL(halo.Vel[0], 100.0, 0.1, "Vel array should be accessible");

    printf("  struct Halo size: %zu bytes\n", sizeof(struct Halo));

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_galaxy_structure
 * @brief   Test that GalaxyData structure contains expected properties
 *
 * Expected: GalaxyData struct has physics properties
 * Validates: Property metadata generated GalaxyData correctly
 */
int test_galaxy_structure(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE ===== */
    struct GalaxyData galaxy;

    /* Initialize galaxy properties */
    galaxy.StellarMass = 1.0e10;
    galaxy.ColdGas = 5.0e9;

    /* ===== VALIDATE ===== */
    TEST_ASSERT_DOUBLE_EQUAL(galaxy.StellarMass, 1.0e10, 1.0,
                            "StellarMass field should be accessible");
    TEST_ASSERT_DOUBLE_EQUAL(galaxy.ColdGas, 5.0e9, 1.0,
                            "ColdGas field should be accessible");

    printf("  struct GalaxyData size: %zu bytes\n", sizeof(struct GalaxyData));

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_output_structure
 * @brief   Test that HaloOutput structure contains expected properties
 *
 * Expected: HaloOutput struct has all output fields
 * Validates: Property metadata generated HaloOutput correctly
 */
int test_output_structure(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE ===== */
    struct HaloOutput output;

    /* Initialize some key output fields */
    output.SnapNum = 63;
    output.Type = 0;
    output.Mvir = 1.0e12;
    output.Rvir = 100.0;
    output.Vvir = 200.0;

    /* ===== VALIDATE ===== */
    TEST_ASSERT(output.SnapNum == 63, "SnapNum in output should be accessible");
    TEST_ASSERT(output.Type == 0, "Type in output should be accessible");
    TEST_ASSERT_DOUBLE_EQUAL(output.Mvir, 1.0e12, 1.0e6,
                            "Mvir in output should be accessible");

    printf("  struct HaloOutput size: %zu bytes\n", sizeof(struct HaloOutput));

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_structure_sizes
 * @brief   Test that structure sizes are reasonable
 *
 * Expected: Structures are not excessively large
 * Validates: Memory efficiency of generated structures
 */
int test_structure_sizes(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */
    size_t halo_size = sizeof(struct Halo);
    size_t galaxy_size = sizeof(struct GalaxyData);
    size_t output_size = sizeof(struct HaloOutput);

    printf("  Structure sizes:\n");
    printf("    Halo:       %zu bytes\n", halo_size);
    printf("    GalaxyData: %zu bytes\n", galaxy_size);
    printf("    HaloOutput: %zu bytes\n", output_size);

    /* Sanity checks - structures shouldn't be tiny or enormous */
    TEST_ASSERT(halo_size > 50, "Halo structure should have reasonable minimum size");
    TEST_ASSERT(halo_size < 10000, "Halo structure should not be excessively large");

    TEST_ASSERT(galaxy_size > 0, "GalaxyData structure should have non-zero size");
    TEST_ASSERT(galaxy_size < 10000, "GalaxyData structure should not be excessively large");

    TEST_ASSERT(output_size > 50, "HaloOutput structure should have reasonable minimum size");
    TEST_ASSERT(output_size < 10000, "HaloOutput structure should not be excessively large");

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_galaxy_separation
 * @brief   Test that galaxy physics is properly separated via pointer
 *
 * Expected: Halo contains galaxy pointer, not embedded struct
 * Validates: Physics-agnostic core architecture principle
 */
int test_galaxy_separation(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE ===== */
    /* Allocate a halo */
    struct Halo *halo = (struct Halo *)mymalloc(sizeof(struct Halo));
    TEST_ASSERT(halo != NULL, "Halo allocation should succeed");

    /* Initially, galaxy pointer should be settable to NULL */
    halo->galaxy = NULL;
    TEST_ASSERT(halo->galaxy == NULL, "Galaxy pointer should be NULL initially");

    /* Allocate a galaxy */
    struct GalaxyData *galaxy = (struct GalaxyData *)mymalloc(sizeof(struct GalaxyData));
    TEST_ASSERT(galaxy != NULL, "Galaxy allocation should succeed");

    /* Initialize galaxy */
    galaxy->StellarMass = 1.0e10;
    galaxy->ColdGas = 5.0e9;

    /* Link galaxy to halo */
    halo->galaxy = galaxy;
    TEST_ASSERT(halo->galaxy != NULL, "Galaxy pointer should be set");

    /* ===== VALIDATE ===== */
    /* Access galaxy properties through halo */
    TEST_ASSERT_DOUBLE_EQUAL(halo->galaxy->StellarMass, 1.0e10, 1.0,
                            "Galaxy properties accessible through halo");
    TEST_ASSERT_DOUBLE_EQUAL(halo->galaxy->ColdGas, 5.0e9, 1.0,
                            "Galaxy properties accessible through halo");

    /* Verify separation: changing galaxy doesn't affect halo */
    halo->Mvir = 2.0e12;
    galaxy->StellarMass = 2.0e10;
    TEST_ASSERT_DOUBLE_EQUAL(halo->Mvir, 2.0e12, 1.0e6, "Halo properties independent");
    TEST_ASSERT_DOUBLE_EQUAL(halo->galaxy->StellarMass, 2.0e10, 1.0e4,
                            "Galaxy properties independent");

    printf("  ✓ Physics-agnostic separation validated\n");

    /* ===== CLEANUP ===== */
    myfree(galaxy);
    myfree(halo);
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @test    test_field_offsets
 * @brief   Test that common fields have consistent offsets
 *
 * Expected: SnapNum, Type, Mvir etc. at predictable offsets
 * Validates: Structure layout consistency
 */
int test_field_offsets(void) {
    /* ===== SETUP ===== */
    init_memory_system(0);

    /* ===== EXECUTE & VALIDATE ===== */
    printf("  Field offsets in struct Halo:\n");
    printf("    SnapNum offset: %zu\n", offsetof(struct Halo, SnapNum));
    printf("    Type offset:    %zu\n", offsetof(struct Halo, Type));
    printf("    Mvir offset:    %zu\n", offsetof(struct Halo, Mvir));
    printf("    Rvir offset:    %zu\n", offsetof(struct Halo, Rvir));
    printf("    galaxy offset:  %zu\n", offsetof(struct Halo, galaxy));

    /* Check that SnapNum is first (offset 0) */
    TEST_ASSERT(offsetof(struct Halo, SnapNum) == 0,
                "SnapNum should be first field");

    printf("  Field offsets in struct GalaxyData:\n");
    printf("    ColdGas offset:     %zu\n", offsetof(struct GalaxyData, ColdGas));
    printf("    StellarMass offset: %zu\n", offsetof(struct GalaxyData, StellarMass));

    /* ===== CLEANUP ===== */
    check_memory_leaks();

    return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all test cases and reports results.
 */
int main(void) {
    printf("========================================\n");
    printf("Test Suite: Property Metadata System\n");
    printf("========================================\n");

    /* Initialize error handling for tests */
    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    /* Run all test cases */
    TEST_RUN(test_halo_structure);
    TEST_RUN(test_galaxy_structure);
    TEST_RUN(test_output_structure);
    TEST_RUN(test_structure_sizes);
    TEST_RUN(test_galaxy_separation);
    TEST_RUN(test_field_offsets);

    /* Print summary and return result */
    TEST_SUMMARY();
    return TEST_RESULT();
}
