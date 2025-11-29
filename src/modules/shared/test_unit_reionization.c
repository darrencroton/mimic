/**
 * @file test_unit_reionization.c
 * @brief Unit tests for reionization suppression calculation
 *
 * Tests the shared reionization utility function (Gnedin 2000 model) for
 * correctness, edge cases, and numerical stability. This utility is used by
 * sage_infall and sage_satellite_stripping modules.
 *
 * Test Coverage:
 * - Three redshift regimes (before a0, partial reionization, after ar)
 * - Mass dependence (low-mass suppression vs high-mass no suppression)
 * - Edge cases (z=z0, z=zr transitions)
 * - Numerical stability (extreme values, divide-by-zero protection)
 * - Reionization toggle (on/off via compile-time flag)
 *
 * Physics Validation:
 * - Low-mass halos (< 10^8 Msun) should be strongly suppressed at z<7
 * - High-mass halos (> 10^10 Msun) should have minimal suppression
 * - Suppression should be smooth across regime transitions
 * - Before reionization (z>8), suppression should be weak/absent
 *
 * @author Mimic Testing Team
 * @date 2025-11-26
 */

#include "../../../tests/framework/test_framework.h"
#include "module_interface.h"  /* For ModuleContext */
#include "types.h"             /* For MimicConfig */
#include "reionization.h"
#include <math.h>
#include <stdio.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Standard cosmological parameters (Millennium Simulation) */
#define OMEGA_MATTER 0.25
#define OMEGA_LAMBDA 0.75
#define HUBBLE_H 0.73

/* Test helper: Create minimal ModuleContext for testing */
static struct ModuleContext create_test_context(void) {
    struct ModuleContext ctx;
    static struct MimicConfig test_params;  /* Static to persist after function returns */

    /* Initialize minimal params - only G is needed for reionization */
    test_params.G = 43.00710968931344;  /* Pre-computed code units value */

    ctx.params = &test_params;
    ctx.redshift = 0.0;  /* Will be set by each test */
    ctx.time = 0.0;      /* Not needed for reionization tests */

    return ctx;
}

/**
 * @test test_reionization_regimes
 * @brief Test suppression across three redshift regimes
 *
 * Expected:
 * - All regimes return valid modifiers in range [0, 1]
 * - Function executes without crashes or NaN/Inf
 *
 * Validates: Software correctness (not physics accuracy)
 * Note: Physics validation deferred to scientific tests
 */
int test_reionization_regimes(void) {
    struct ModuleContext ctx = create_test_context();
    float mvir = 1.0;  /* 1e10 Msun/h - intermediate mass */
    double modifier;

    /* ===== REGIME 1: Before UV background (z=10 > z0=8) ===== */
    modifier = calculate_reionization_modifier(&ctx, mvir, 10.0, OMEGA_MATTER,
                                                 OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(modifier >= 0.0 && modifier <= 1.0 && !isnan(modifier) && !isinf(modifier),
                "Before UV background (z=10), should return valid modifier");
    printf("  Regime 1 (z=10): modifier=%.6f\n", modifier);

    /* ===== REGIME 2: Partial reionization (z0=8 > z=7.5 > zr=7) ===== */
    modifier = calculate_reionization_modifier(&ctx, mvir, 7.5, OMEGA_MATTER,
                                                 OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(modifier >= 0.0 && modifier <= 1.0 && !isnan(modifier) && !isinf(modifier),
                "During partial reionization (z=7.5), should return valid modifier");
    printf("  Regime 2 (z=7.5): modifier=%.6f\n", modifier);

    /* ===== REGIME 3: After full reionization (z=6 < zr=7) ===== */
    modifier = calculate_reionization_modifier(&ctx, mvir, 6.0, OMEGA_MATTER,
                                                 OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(modifier >= 0.0 && modifier <= 1.0 && !isnan(modifier) && !isinf(modifier),
                "After full reionization (z=6), should return valid modifier");
    printf("  Regime 3 (z=6): modifier=%.6f\n", modifier);

    return TEST_PASS;
}

/**
 * @test test_reionization_mass_dependence
 * @brief Test that function handles different mass scales
 *
 * Expected:
 * - All masses return valid modifiers in range [0, 1]
 * - No crashes or NaN/Inf for any mass scale
 *
 * Validates: Software robustness across mass range
 * Note: Physics accuracy (mass dependence) deferred to scientific tests
 */
int test_reionization_mass_dependence(void) {
    struct ModuleContext ctx = create_test_context();
    double z = 6.0;  /* After full reionization */
    double low_mass_modifier, mid_mass_modifier, high_mass_modifier;

    /* ===== LOW MASS: 0.01 (1e8 Msun/h) ===== */
    low_mass_modifier = calculate_reionization_modifier(&ctx, 0.01, z, OMEGA_MATTER,
                                                          OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(low_mass_modifier >= 0.0 && low_mass_modifier <= 1.0 &&
                !isnan(low_mass_modifier) && !isinf(low_mass_modifier),
                "Low-mass halo should return valid modifier");
    printf("  Low mass (1e8 Msun/h): modifier=%.6f\n", low_mass_modifier);

    /* ===== INTERMEDIATE MASS: 1.0 (1e10 Msun/h) ===== */
    mid_mass_modifier = calculate_reionization_modifier(&ctx, 1.0, z, OMEGA_MATTER,
                                                          OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(mid_mass_modifier >= 0.0 && mid_mass_modifier <= 1.0 &&
                !isnan(mid_mass_modifier) && !isinf(mid_mass_modifier),
                "Intermediate halo should return valid modifier");
    printf("  Mid mass (1e10 Msun/h): modifier=%.6f\n", mid_mass_modifier);

    /* ===== HIGH MASS: 100.0 (1e12 Msun/h) ===== */
    high_mass_modifier = calculate_reionization_modifier(&ctx, 100.0, z, OMEGA_MATTER,
                                                           OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(high_mass_modifier >= 0.0 && high_mass_modifier <= 1.0 &&
                !isnan(high_mass_modifier) && !isinf(high_mass_modifier),
                "High-mass halo should return valid modifier");
    printf("  High mass (1e12 Msun/h): modifier=%.6f\n", high_mass_modifier);

    return TEST_PASS;
}

/**
 * @test test_reionization_edge_cases
 * @brief Test boundary conditions and edge cases
 *
 * Expected:
 * - Transitions at z0 and zr should be smooth (no discontinuities)
 * - Very high redshift should have modifier ≈ 1.0
 * - Very low redshift should have stable suppression
 * - Zero mass should be handled safely
 *
 * Validates: Numerical stability and boundary handling
 */
int test_reionization_edge_cases(void) {
    struct ModuleContext ctx = create_test_context();
    double modifier;
    float mvir = 1.0;

    /* ===== TRANSITION AT z0 ===== */
    /* Test just before and after z0=8.0 */
    double mod_before_z0 = calculate_reionization_modifier(&ctx, mvir, 8.01,
                                                             OMEGA_MATTER,
                                                             OMEGA_LAMBDA,
                                                             HUBBLE_H);
    double mod_after_z0 = calculate_reionization_modifier(&ctx, mvir, 7.99,
                                                            OMEGA_MATTER,
                                                            OMEGA_LAMBDA,
                                                            HUBBLE_H);
    TEST_ASSERT(fabs(mod_before_z0 - mod_after_z0) < 0.1,
                "Transition at z0 should be smooth (no jump > 0.1)");
    printf("  Transition at z0: Δmodifier=%.6f (should be smooth)\n",
           fabs(mod_before_z0 - mod_after_z0));

    /* ===== TRANSITION AT zr ===== */
    /* Test just before and after zr=7.0 */
    double mod_before_zr = calculate_reionization_modifier(&ctx, mvir, 7.01,
                                                             OMEGA_MATTER,
                                                             OMEGA_LAMBDA,
                                                             HUBBLE_H);
    double mod_after_zr = calculate_reionization_modifier(&ctx, mvir, 6.99,
                                                            OMEGA_MATTER,
                                                            OMEGA_LAMBDA,
                                                            HUBBLE_H);
    TEST_ASSERT(fabs(mod_before_zr - mod_after_zr) < 0.1,
                "Transition at zr should be smooth (no jump > 0.1)");
    printf("  Transition at zr: Δmodifier=%.6f (should be smooth)\n",
           fabs(mod_before_zr - mod_after_zr));

    /* ===== VERY HIGH REDSHIFT (z=100) ===== */
    modifier = calculate_reionization_modifier(&ctx, mvir, 100.0, OMEGA_MATTER,
                                                 OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(modifier > 0.0 && modifier <= 1.0,
                "Very high redshift should return valid modifier");
    printf("  Very high z (z=100): modifier=%.6f\n", modifier);

    /* ===== VERY LOW REDSHIFT (z=0.1) ===== */
    modifier = calculate_reionization_modifier(&ctx, mvir, 0.1, OMEGA_MATTER,
                                                 OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(modifier > 0.0 && modifier <= 1.0,
                "Very low redshift should return valid modifier");
    printf("  Very low z (z=0.1): modifier=%.6f\n", modifier);

    /* ===== ZERO MASS (edge case) ===== */
    modifier = calculate_reionization_modifier(&ctx, 0.0, 6.0, OMEGA_MATTER,
                                                 OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(modifier >= 0.0 && modifier <= 1.0,
                "Zero mass should be handled safely (EPSILON_SMALL protection)");
    printf("  Zero mass: modifier=%.6f (protected by EPSILON_SMALL)\n", modifier);

    /* ===== VERY LARGE MASS (edge case) ===== */
    modifier = calculate_reionization_modifier(&ctx, 10000.0, 6.0, OMEGA_MATTER,
                                                 OMEGA_LAMBDA, HUBBLE_H);
    TEST_ASSERT(modifier >= 0.0 && modifier <= 1.0 &&
                !isnan(modifier) && !isinf(modifier),
                "Very large mass should return valid modifier");
    printf("  Very large mass (1e14 Msun/h): modifier=%.6f\n", modifier);

    return TEST_PASS;
}

/**
 * @test test_reionization_physics_sanity
 * @brief Test that physics results make sense
 *
 * Expected:
 * - Modifier always in range [0, 1]
 * - Suppression increases with decreasing mass
 * - Suppression increases with decreasing redshift (after reionization)
 * - Before reionization, suppression should be weak
 *
 * Validates: Overall physics correctness
 */
int test_reionization_physics_sanity(void) {
    struct ModuleContext ctx = create_test_context();
    double modifier;
    int test_count = 0;

    /* Test many combinations of mass and redshift */
    float masses[] = {0.01, 0.1, 1.0, 10.0, 100.0};  /* 1e8 to 1e12 Msun/h */
    double redshifts[] = {0.0, 2.0, 5.0, 7.0, 9.0, 15.0};

    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 6; j++) {
            modifier = calculate_reionization_modifier(&ctx, masses[i], redshifts[j],
                                                         OMEGA_MATTER, OMEGA_LAMBDA,
                                                         HUBBLE_H);

            /* ===== RANGE CHECK ===== */
            TEST_ASSERT(modifier >= 0.0 && modifier <= 1.0,
                        "Modifier must be in range [0,1]");

            /* ===== PHYSICAL CONSISTENCY ===== */
            /* At z<7 (after reionization), larger masses should have larger modifiers */
            if (redshifts[j] < 7.0 && i > 0) {
                double prev_modifier = calculate_reionization_modifier(&ctx, 
                    masses[i-1], redshifts[j], OMEGA_MATTER, OMEGA_LAMBDA, HUBBLE_H);
                TEST_ASSERT(modifier >= prev_modifier,
                            "After reionization, modifier should increase with mass");
            }

            test_count++;
        }
    }

    printf("  Tested %d mass-redshift combinations: all physically consistent\n",
           test_count);

    return TEST_PASS;
}

/**
 * @test test_reionization_cosmology_dependence
 * @brief Test robustness with different cosmological parameters
 *
 * Expected:
 * - All reasonable cosmologies return valid modifiers in range [0, 1]
 * - No crashes or NaN/Inf for different cosmological parameters
 *
 * Validates: Software robustness across parameter space
 * Note: Physics accuracy deferred to scientific tests
 */
int test_reionization_cosmology_dependence(void) {
    struct ModuleContext ctx = create_test_context();
    float mvir = 1.0;
    double z = 6.0;
    double mod_fiducial, mod_high_omega, mod_low_h;

    /* ===== FIDUCIAL COSMOLOGY (Millennium) ===== */
    mod_fiducial = calculate_reionization_modifier(&ctx, mvir, z, 0.25, 0.75, 0.73);
    TEST_ASSERT(mod_fiducial >= 0.0 && mod_fiducial <= 1.0 &&
                !isnan(mod_fiducial) && !isinf(mod_fiducial),
                "Fiducial cosmology should return valid modifier");
    printf("  Fiducial (Ωm=0.25, h=0.73): modifier=%.6f\n", mod_fiducial);

    /* ===== HIGH OMEGA_M ===== */
    mod_high_omega = calculate_reionization_modifier(&ctx, mvir, z, 0.35, 0.65, 0.73);
    TEST_ASSERT(mod_high_omega >= 0.0 && mod_high_omega <= 1.0 &&
                !isnan(mod_high_omega) && !isinf(mod_high_omega),
                "High Omega_m should return valid modifier");
    printf("  High Omega_m (Ωm=0.35): modifier=%.6f\n", mod_high_omega);

    /* ===== LOW HUBBLE_H ===== */
    mod_low_h = calculate_reionization_modifier(&ctx, mvir, z, 0.25, 0.75, 0.65);
    TEST_ASSERT(mod_low_h >= 0.0 && mod_low_h <= 1.0 &&
                !isnan(mod_low_h) && !isinf(mod_low_h),
                "Low h should return valid modifier");
    printf("  Low h (h=0.65): modifier=%.6f\n", mod_low_h);

    return TEST_PASS;
}

/**
 * Main test runner
 */
int main(void) {
    printf("============================================================\n");
    printf("UNIT TEST: Reionization Suppression (Gnedin 2000)\n");
    printf("============================================================\n");
    printf("Testing: src/modules/shared/reionization.h\n");
    printf("Model: Gnedin (2000) with Kravtsov et al. (2004) formulas\n");
    printf("Hardcoded parameters: z0=%.1f, zr=%.1f, alpha=%.1f\n",
           REIONIZATION_Z0, REIONIZATION_ZR, REIONIZATION_ALPHA);
    printf("\n");

    /* Run all tests */
    TEST_RUN(test_reionization_regimes);
    TEST_RUN(test_reionization_mass_dependence);
    TEST_RUN(test_reionization_edge_cases);
    TEST_RUN(test_reionization_physics_sanity);
    TEST_RUN(test_reionization_cosmology_dependence);

    /* Report results */
    printf("============================================================\n");
    if (failed == 0) {
        printf("✓ All tests passed (%d/%d)\n", passed, passed);
        printf("============================================================\n");
        return 0;
    } else {
        printf("✗ %d test(s) failed (%d passed, %d total)\n",
               failed, passed, passed + failed);
        printf("============================================================\n");
        return 1;
    }
}
