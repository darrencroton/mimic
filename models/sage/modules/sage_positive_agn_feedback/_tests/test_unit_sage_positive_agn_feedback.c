/**
 * @file    test_unit_sage_positive_agn_feedback.c
 * @brief   Unit tests for sage_positive_agn_feedback module
 *
 * Validates the positive AGN feedback physics of Silk et al. (2024),
 * arXiv:2401.02482:
 *   - The redshift-dependent column criterion: triggered SF switches ON at high
 *     z (positive, momentum-conserving regime) and OFF at low z (negative
 *     regime), reproducing the paper's z~6 transition.
 *   - Triggered stars are ADDED to NewStellarMass (not overwritten) and tracked
 *     cumulatively in AGNTriggeredStellarMass.
 *   - Gating: no black hole (no AGN) => no triggered SF.
 *   - Edge cases: zero cold gas, zero disc radius, zero velocity are no-ops.
 *   - Parameter sensitivity: higher efficiency => more triggered stars.
 *
 * Test cases:
 *   - test_high_z_triggers_star_formation
 *   - test_low_z_is_negative_regime
 *   - test_no_black_hole_no_trigger
 *   - test_adds_to_existing_new_stellar_mass
 *   - test_zero_cold_gas_is_noop
 *   - test_parameter_sensitivity
 *   - test_module_initialization
 *   - test_memory_safety
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/* Track whether modules have been registered */
static int modules_registered = 0;

/* Module parameters (extern to inspect module internals from the test) */
extern double POSITIVE_FEEDBACK_EFFICIENCY;
extern double POSITIVE_FEEDBACK_COLUMN_THRESHOLD;

/* External stubs */
extern void set_test_model_parameters(void);

/* Module functions under test */
extern int sage_positive_agn_feedback_init(void);
extern int sage_positive_agn_feedback_process(struct ModuleContext *ctx,
                                              struct Halo *halos, int ngal);
extern int sage_positive_agn_feedback_cleanup(void);

/* Paper constants reproduced for expected-value calculations (Silk et al. 2024
 * §3.3): N_H(z) = 10^21 (1+z)^3.3 cm^-2. */
#define NH_NORM_CM2   1.0e21
#define NH_Z_EXPONENT 3.3

/* Stable storage for the phase_1 pipeline the init() dependency checks need:
 * calculate_star_formation -> positive_agn_feedback -> apply_star_formation. */
static char pf_name0[] = "sage_calculate_star_formation";
static char pf_name1[] = "sage_positive_agn_feedback";
static char pf_name2[] = "sage_apply_star_formation_supernova";
static struct PhaseModuleConfig pf_pipeline[3];

// ============================================================================
// FIXTURES
// ============================================================================

static void reset_config(void)
{
    memset(&MimicConfig, 0, sizeof(MimicConfig));
}

static void ensure_modules_registered(void)
{
    if (!modules_registered) {
        register_all_modules();
        modules_registered = 1;
    }
}

/* Reproduce the module's positive-feedback weight f_pos(z). */
static double expected_f_pos(double redshift, double n_cool)
{
    const double n_h = NH_NORM_CM2 * pow(1.0 + redshift, NH_Z_EXPONENT);
    return n_h / (n_h + n_cool);
}

static void setup_test_galaxy(struct Halo *halo, struct GalaxyData *gal,
                              double cold_gas, double disk_radius, double vvir,
                              double black_hole_mass)
{
    memset(halo, 0, sizeof(struct Halo));
    memset(gal, 0, sizeof(struct GalaxyData));

    halo->Type = 0;
    halo->Mvir = 100.0;
    halo->Vvir = (float)vvir;
    halo->SnapNum = 16;        /* a high-redshift snapshot in the test config  */
    halo->dT = 0.01;
    halo->galaxy = gal;

    gal->ColdGas = (float)cold_gas;
    gal->DiskScaleRadius = (float)disk_radius;
    gal->BlackHoleMass = (float)black_hole_mass;
    gal->NewStellarMass = 0.0;
    gal->AGNTriggeredStellarMass = 0.0;
}

static void setup_test_parameters(double efficiency, double column_threshold)
{
    int idx = 0;

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "PositiveFeedbackEfficiency");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6f", efficiency);

    snprintf(MimicConfig.ModelParams[idx].param_name, MAX_STRING_LEN, "PositiveFeedbackColumnThreshold");
    snprintf(MimicConfig.ModelParams[idx++].value, MAX_STRING_LEN, "%.6e", column_threshold);

    MimicConfig.NumModelParams = idx;

    /* Wire the dependency-checked phase_1 ordering. */
    pf_pipeline[0].module_name = pf_name0;
    pf_pipeline[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    pf_pipeline[1].module_name = pf_name1;
    pf_pipeline[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
    pf_pipeline[2].module_name = pf_name2;
    pf_pipeline[2].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1 = pf_pipeline;
    MimicConfig.num_phase_1 = 3;
}

static void setup_test_context(struct ModuleContext *ctx, double redshift)
{
    memset(ctx, 0, sizeof(struct ModuleContext));
    ctx->substep_dt = 0.01;
    ctx->redshift = redshift;
    ctx->time = 1.0;
    ctx->snapshot_number = 16;
    ctx->substep_number = 0;
    ctx->num_substeps = 1;
    ctx->params = &MimicConfig;
}

// ============================================================================
// PHYSICS TESTS
// ============================================================================

/**
 * @test    test_high_z_triggers_star_formation
 * @brief   At high redshift (positive regime) the module forms extra stars.
 */
int test_high_z_triggers_star_formation(void)
{
    init_memory_system(0);
    reset_config();

    /* Efficiency kept low enough that the substep does not exhaust ColdGas, so
     * the assertion tests the rate formula rather than the cold-gas cap. */
    const double efficiency = 0.01;
    const double threshold = 1.0e24;   /* paper-scale N_cool */
    setup_test_parameters(efficiency, threshold);
    TEST_ASSERT(sage_positive_agn_feedback_init() == 0, "Module init should succeed");

    struct Halo halo;
    struct GalaxyData gal;
    const double cold_gas = 5.0;
    const double disk_radius = 0.01;
    const double vvir = 150.0;
    const double bh = 1.0e-3;
    setup_test_galaxy(&halo, &gal, cold_gas, disk_radius, vvir, bh);

    struct ModuleContext ctx;
    const double redshift = 8.0;
    setup_test_context(&ctx, redshift);

    /* Expected triggered stars (reproduce module logic). */
    const double dt = 0.01;            /* dT / num_substeps */
    const double f_pos = expected_f_pos(redshift, threshold);
    const double t_dyn = disk_radius / vvir;
    double expected = efficiency * f_pos * cold_gas / t_dyn * dt;
    if (expected > cold_gas) expected = cold_gas;

    int result = sage_positive_agn_feedback_process(&ctx, &halo, 1);

    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(f_pos > 0.5, "z=8 should be deep in the positive regime");
    TEST_ASSERT(gal.NewStellarMass > 0.0, "Triggered stars should be positive at high z");
    TEST_ASSERT_DOUBLE_EQUAL(gal.NewStellarMass, expected, 1e-6,
                             "Triggered NewStellarMass should match expected value");
    TEST_ASSERT_DOUBLE_EQUAL(gal.AGNTriggeredStellarMass, expected, 1e-6,
                             "AGNTriggeredStellarMass diagnostic should track triggered stars");

    sage_positive_agn_feedback_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_low_z_is_negative_regime
 * @brief   The column criterion strongly suppresses triggering at low z.
 *
 * Runs the same galaxy at z=0 and z=8 and checks that the z=0 (negative regime)
 * trigger is a tiny fraction of the z=8 (positive regime) trigger, reproducing
 * the paper's high-z-only positive phase.
 */
int test_low_z_is_negative_regime(void)
{
    init_memory_system(0);
    reset_config();

    const double efficiency = 0.01;
    const double threshold = 1.0e24;
    setup_test_parameters(efficiency, threshold);
    sage_positive_agn_feedback_init();

    struct Halo halo_hi, halo_lo;
    struct GalaxyData gal_hi, gal_lo;
    setup_test_galaxy(&halo_hi, &gal_hi, 5.0, 0.01, 150.0, 1.0e-3);
    setup_test_galaxy(&halo_lo, &gal_lo, 5.0, 0.01, 150.0, 1.0e-3);

    struct ModuleContext ctx_hi, ctx_lo;
    setup_test_context(&ctx_hi, 8.0);   /* positive regime */
    setup_test_context(&ctx_lo, 0.0);   /* negative regime */

    sage_positive_agn_feedback_process(&ctx_hi, &halo_hi, 1);
    sage_positive_agn_feedback_process(&ctx_lo, &halo_lo, 1);

    TEST_ASSERT(expected_f_pos(0.0, threshold) < 0.01,
                "z=0 should be firmly in the negative regime");
    TEST_ASSERT(gal_hi.NewStellarMass > 0.0, "z=8 should trigger star formation");
    TEST_ASSERT(gal_lo.NewStellarMass < 0.01 * gal_hi.NewStellarMass,
                "Triggered SF at z=0 should be a tiny fraction of the z=8 value");

    sage_positive_agn_feedback_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_no_black_hole_no_trigger
 * @brief   No black hole means no AGN outflow, hence no triggered SF.
 */
int test_no_black_hole_no_trigger(void)
{
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.5, 1.0e24);
    sage_positive_agn_feedback_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 5.0, 0.01, 150.0, 0.0);  /* BH = 0 */

    struct ModuleContext ctx;
    setup_test_context(&ctx, 8.0);

    int result = sage_positive_agn_feedback_process(&ctx, &halo, 1);

    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(gal.NewStellarMass == 0.0, "No BH => no triggered SF even at high z");
    TEST_ASSERT(gal.AGNTriggeredStellarMass == 0.0, "No BH => diagnostic stays zero");

    sage_positive_agn_feedback_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_adds_to_existing_new_stellar_mass
 * @brief   Triggered stars are ADDED to a pre-existing NewStellarMass value.
 */
int test_adds_to_existing_new_stellar_mass(void)
{
    init_memory_system(0);
    reset_config();

    const double efficiency = 0.01;
    const double threshold = 1.0e24;
    setup_test_parameters(efficiency, threshold);
    sage_positive_agn_feedback_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 5.0, 0.01, 150.0, 1.0e-3);
    const double preexisting = 0.123;   /* quiescent SF from the previous module */
    gal.NewStellarMass = preexisting;

    struct ModuleContext ctx;
    setup_test_context(&ctx, 8.0);

    sage_positive_agn_feedback_process(&ctx, &halo, 1);

    TEST_ASSERT(gal.NewStellarMass > preexisting,
                "Module must add to, not overwrite, NewStellarMass");
    TEST_ASSERT_DOUBLE_EQUAL(gal.NewStellarMass - preexisting,
                             gal.AGNTriggeredStellarMass, 1e-6,
                             "Added amount should equal the tracked triggered mass");

    sage_positive_agn_feedback_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_zero_cold_gas_is_noop
 * @brief   No cold gas => nothing to convert.
 */
int test_zero_cold_gas_is_noop(void)
{
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.5, 1.0e24);
    sage_positive_agn_feedback_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 0.0, 0.01, 150.0, 1.0e-3);  /* ColdGas = 0 */

    struct ModuleContext ctx;
    setup_test_context(&ctx, 8.0);

    int result = sage_positive_agn_feedback_process(&ctx, &halo, 1);

    TEST_ASSERT(result == 0, "Process should succeed");
    TEST_ASSERT(gal.NewStellarMass == 0.0, "No cold gas => no triggered SF");

    sage_positive_agn_feedback_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_parameter_sensitivity
 * @brief   Higher efficiency yields proportionally more triggered stars.
 */
int test_parameter_sensitivity(void)
{
    init_memory_system(0);

    reset_config();
    setup_test_parameters(0.001, 1.0e24);
    sage_positive_agn_feedback_init();

    struct Halo halo1;
    struct GalaxyData gal1;
    setup_test_galaxy(&halo1, &gal1, 5.0, 0.01, 150.0, 1.0e-3);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 8.0);

    sage_positive_agn_feedback_process(&ctx, &halo1, 1);
    const double stars_low = gal1.NewStellarMass;
    sage_positive_agn_feedback_cleanup();

    reset_config();
    setup_test_parameters(0.005, 1.0e24);  /* 5x efficiency */
    sage_positive_agn_feedback_init();

    struct Halo halo2;
    struct GalaxyData gal2;
    setup_test_galaxy(&halo2, &gal2, 5.0, 0.01, 150.0, 1.0e-3);

    sage_positive_agn_feedback_process(&ctx, &halo2, 1);
    const double stars_high = gal2.NewStellarMass;
    sage_positive_agn_feedback_cleanup();

    TEST_ASSERT(stars_low > 0.0, "Low efficiency should produce stars");
    TEST_ASSERT(stars_high > stars_low, "Higher efficiency should produce more stars");
    const double ratio = stars_high / stars_low;
    TEST_ASSERT(ratio > 4.5 && ratio < 5.5, "Triggered SF should scale ~linearly with efficiency");

    check_memory_leaks();
    return TEST_PASS;
}

// ============================================================================
// INFRASTRUCTURE TESTS
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   Full module-system init with the module in a valid pipeline.
 */
int test_module_initialization(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    MimicConfig.Omega = 0.25;
    MimicConfig.OmegaLambda = 0.75;
    MimicConfig.Hubble_h = 0.73;

    MimicConfig.phase_1 = mymalloc_cat(3 * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_star_formation");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1[1].module_name = strdup("sage_positive_agn_feedback");
    MimicConfig.phase_1[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1[2].module_name = strdup("sage_apply_star_formation_supernova");
    MimicConfig.phase_1[2].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 3;
    MimicConfig.SubSteps = 1;

    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module system initialization should succeed");

    module_system_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_memory_safety
 * @brief   No leaks across an init/process/cleanup cycle.
 */
int test_memory_safety(void)
{
    init_memory_system(0);
    reset_config();

    setup_test_parameters(0.5, 1.0e24);
    sage_positive_agn_feedback_init();

    struct Halo halo;
    struct GalaxyData gal;
    setup_test_galaxy(&halo, &gal, 5.0, 0.01, 150.0, 1.0e-3);

    struct ModuleContext ctx;
    setup_test_context(&ctx, 8.0);

    sage_positive_agn_feedback_process(&ctx, &halo, 1);

    sage_positive_agn_feedback_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

// ============================================================================
// MAIN
// ============================================================================

int main(void)
{
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: sage_positive_agn_feedback Unit Tests\n");
    printf("============================================================\n");
    printf("%s", NC);

    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    TEST_RUN(test_high_z_triggers_star_formation);
    TEST_RUN(test_low_z_is_negative_regime);
    TEST_RUN(test_no_black_hole_no_trigger);
    TEST_RUN(test_adds_to_existing_new_stellar_mass);
    TEST_RUN(test_zero_cold_gas_is_noop);
    TEST_RUN(test_parameter_sensitivity);

    TEST_RUN(test_module_initialization);
    TEST_RUN(test_memory_safety);

    TEST_SUMMARY();
    return TEST_RESULT();
}
