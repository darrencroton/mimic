/**
 * @file    test_unit_sage_dependency_contracts.c
 * @brief   SAGE module dependency-contract unit tests
 *
 * These tests are SAGE-owned because they name SAGE production modules and
 * validate SAGE package dependency and warning contracts. Core module-system
 * tests use framework fixtures in tests/unit/ instead.
 */

#include "framework/test_framework.h"
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

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

static int modules_registered = 0;

static void reset_config(void) {
    memset(&MimicConfig, 0, sizeof(MimicConfig));
}

static void ensure_modules_registered(void) {
    if (!modules_registered) {
        register_all_modules();
        modules_registered = 1;
    }
}

/* ============================================================================
 * DEPENDENCY ENFORCEMENT TESTS (§7 of SAGE-MODULE-REVIEW.md)
 * ============================================================================
 *
 * Each test verifies one constraint from the §7 dependency table.
 *
 * Naming convention:
 *   test_dep_<module>_<condition>_<error|warn>
 *
 * ERROR tests assert module_system_init() returns non-zero.
 * WARNING tests assert module_system_init() returns zero AND verify the
 * warning text was actually emitted via set_log_output() capture.
 */

/**
 * @brief Read all content from a FILE* into a static buffer.
 *
 * Rewinds fp before reading.  The returned pointer is valid until the next
 * call to this function.  Truncates silently at 8191 bytes (ample for
 * warning log lines).
 */
static const char *read_captured_log(FILE *fp) {
    static char buf[8192];
    rewind(fp);
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    buf[n] = '\0';
    return buf;
}

extern void set_test_model_parameters(void);

/**
 * @test    test_dep_apply_infall_missing_prepare_error
 * @brief   sage_apply_infall requires sage_prepare_infall_budget in pre_timestep (ERROR)
 */
int test_dep_apply_infall_missing_prepare_error(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* Only sage_apply_infall in phase_1; pre_timestep is empty */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_apply_infall");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result != 0,
                "sage_apply_infall without sage_prepare_infall_budget must fail init");

    if (result == 0) { module_system_cleanup(); }
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_apply_cooling_wrong_order_error
 * @brief   sage_apply_cooling requires sage_calculate_cooling_budget to precede it (ERROR)
 */
int test_dep_apply_cooling_wrong_order_error(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* sage_apply_cooling before sage_calculate_cooling_budget — wrong order */
    MimicConfig.phase_1 = mymalloc_cat(2 * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_apply_cooling");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1[1].module_name = strdup("sage_calculate_cooling_budget");
    MimicConfig.phase_1[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 2;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result != 0,
                "sage_apply_cooling before sage_calculate_cooling_budget must fail init");

    if (result == 0) { module_system_cleanup(); }
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_supernova_wrong_order_error
 * @brief   sage_calculate_supernova_feedback requires sage_calculate_star_formation to precede it (ERROR)
 *
 * Triggers when both are configured but in wrong order (SN before SF).
 */
int test_dep_supernova_wrong_order_error(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* SN before SF — wrong order; apply step also present after both */
    MimicConfig.phase_1 = mymalloc_cat(3 * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_supernova_feedback");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1[1].module_name = strdup("sage_calculate_star_formation");
    MimicConfig.phase_1[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1[2].module_name = strdup("sage_apply_star_formation_supernova");
    MimicConfig.phase_1[2].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 3;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result != 0,
                "sage_calculate_supernova_feedback before sage_calculate_star_formation must fail init");

    if (result == 0) { module_system_cleanup(); }
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_apply_sfn_wrong_order_error
 * @brief   sage_apply_star_formation_supernova must follow SF/SN modules (ERROR)
 *
 * Triggers when apply step precedes SF in same phase.
 */
int test_dep_apply_sfn_wrong_order_error(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* apply step before SF — wrong order */
    MimicConfig.phase_1 = mymalloc_cat(2 * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_apply_star_formation_supernova");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.phase_1[1].module_name = strdup("sage_calculate_star_formation");
    MimicConfig.phase_1[1].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 2;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result != 0,
                "sage_apply_sfn before sage_calculate_star_formation must fail init");

    if (result == 0) { module_system_cleanup(); }
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_quasar_per_event_missing_producer_error
 * @brief   sage_quasar_mode process_per_event requires merger event producer (ERROR)
 */
int test_dep_quasar_per_event_missing_producer_error(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* quasar_mode as per_event with no merger event producer in phase_2 */
    MimicConfig.phase_2 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_2[0].module_name = strdup("sage_quasar_mode");
    MimicConfig.phase_2[0].processing_mode = PROCESSING_MODE_PER_EVENT;
    MimicConfig.num_phase_2 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result != 0,
                "sage_quasar_mode per_event without merger producer must fail init");

    if (result == 0) { module_system_cleanup(); }
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_starburst_per_event_missing_producer_error
 * @brief   sage_starburst_feedback process_per_event requires merger event producer (ERROR)
 */
int test_dep_starburst_per_event_missing_producer_error(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* starburst_feedback as per_event with no merger event producer in phase_2 */
    MimicConfig.phase_2 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_2[0].module_name = strdup("sage_starburst_feedback");
    MimicConfig.phase_2[0].processing_mode = PROCESSING_MODE_PER_EVENT;
    MimicConfig.num_phase_2 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result != 0,
                "sage_starburst_feedback per_event without merger producer must fail init");

    if (result == 0) { module_system_cleanup(); }
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_sf_missing_apply_error
 * @brief   sage_calculate_star_formation without sage_apply_star_formation_supernova must fail (ERROR)
 *
 * NewStellarMass would be computed each substep but never committed.
 */
int test_dep_sf_missing_apply_error(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* SF alone — no apply step anywhere */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_star_formation");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result != 0,
                "sage_calculate_star_formation without apply step must fail init");

    if (result == 0) { module_system_cleanup(); }
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_sn_missing_apply_error
 * @brief   sage_calculate_supernova_feedback without sage_apply_star_formation_supernova must fail (ERROR)
 *
 * SupernovaReheatedMass and SupernovaEjectedMass would be computed but never committed.
 */
int test_dep_sn_missing_apply_error(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* SN alone — no apply step anywhere */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_calculate_supernova_feedback");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    int result = module_system_init();
    TEST_ASSERT(result != 0,
                "sage_calculate_supernova_feedback without apply step must fail init");

    if (result == 0) { module_system_cleanup(); }
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_apply_sfn_warns_no_prescriptions
 * @brief   sage_apply_star_formation_supernova alone emits WARNING but succeeds (WARNING)
 */
int test_dep_apply_sfn_warns_no_prescriptions(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* apply step alone — no SF or SN configured anywhere */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_apply_star_formation_supernova");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    FILE *capture = tmpfile();
    FILE *old_out = set_log_output(capture);
    int result = module_system_init();
    set_log_output(old_out);
    const char *log = read_captured_log(capture);

    TEST_ASSERT(result == 0,
                "sage_apply_sfn alone should warn but not fail init");
    TEST_ASSERT(strstr(log, "sage_apply_star_formation_supernova") != NULL,
                "WARNING about missing SF/SN prescriptions must be logged");

    fclose(capture);
    module_system_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_resolve_mergers_warns_no_clock
 * @brief   sage_resolve_mergers_and_disruption without merger clock emits WARNING (WARNING)
 */
int test_dep_resolve_mergers_warns_no_clock(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* sage_resolve_mergers_and_disruption in phase_2 with no sage_initialise_merger_clock */
    MimicConfig.phase_2 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_2[0].module_name = strdup("sage_resolve_mergers_and_disruption");
    MimicConfig.phase_2[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.num_phase_2 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    FILE *capture = tmpfile();
    FILE *old_out = set_log_output(capture);
    int result = module_system_init();
    set_log_output(old_out);
    const char *log = read_captured_log(capture);

    TEST_ASSERT(result == 0,
                "sage_resolve_mergers without merger clock should warn but not fail");
    TEST_ASSERT(strstr(log, "sage_initialise_merger_clock") != NULL,
                "WARNING about missing merger clock must be logged");

    fclose(capture);
    module_system_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_starburst_warns_no_disk_instability
 * @brief   sage_starburst_feedback by_galaxy without disk_instability emits WARNING (WARNING)
 */
int test_dep_starburst_warns_no_disk_instability(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* starburst_feedback as by_galaxy without sage_disk_instability before it */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_starburst_feedback");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;
    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    FILE *capture = tmpfile();
    FILE *old_out = set_log_output(capture);
    int result = module_system_init();
    set_log_output(old_out);
    const char *log = read_captured_log(capture);

    TEST_ASSERT(result == 0,
                "sage_starburst_feedback by_galaxy without disk_instability should warn but not fail");
    TEST_ASSERT(strstr(log, "sage_disk_instability") != NULL,
                "WARNING about missing disk instability trigger must be logged");

    fclose(capture);
    module_system_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * @test    test_dep_starburst_warns_no_quasar_mode
 * @brief   sage_starburst_feedback: disk_instability in phase_1 but quasar_mode absent
 *          from phase_2 emits WARNING (WARNING) — §7 table, last row
 *
 * Scenario: starburst_feedback as process_per_event (valid merger consumer),
 * sage_disk_instability present in phase_1 (enables post-merger disk instability
 * recheck), but sage_quasar_mode absent from phase_2 (quasar wind silently skipped).
 */
int test_dep_starburst_warns_no_quasar_mode(void)
{
    reset_config();
    init_memory_system(0);
    ensure_modules_registered();

    /* phase_1: disk instability trigger writer */
    MimicConfig.phase_1 = mymalloc_cat(sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_1[0].module_name = strdup("sage_disk_instability");
    MimicConfig.phase_1[0].processing_mode = PROCESSING_MODE_BY_GALAXY;
    MimicConfig.num_phase_1 = 1;

    /* phase_2: merger producer + starburst consumer; no quasar_mode */
    MimicConfig.phase_2 = mymalloc_cat(2 * sizeof(struct PhaseModuleConfig), MEM_UTILITY);
    MimicConfig.phase_2[0].module_name = strdup("sage_resolve_mergers_and_disruption");
    MimicConfig.phase_2[0].processing_mode = PROCESSING_MODE_FULL_HALO;
    MimicConfig.phase_2[1].module_name = strdup("sage_starburst_feedback");
    MimicConfig.phase_2[1].processing_mode = PROCESSING_MODE_PER_EVENT;
    MimicConfig.num_phase_2 = 2;

    MimicConfig.SubSteps = 1;
    set_test_model_parameters();

    FILE *capture = tmpfile();
    FILE *old_out = set_log_output(capture);
    int result = module_system_init();
    set_log_output(old_out);
    const char *log = read_captured_log(capture);

    TEST_ASSERT(result == 0,
                "starburst with disk instability but no quasar_mode should warn but not fail");
    TEST_ASSERT(strstr(log, "sage_quasar_mode") != NULL,
                "WARNING about missing quasar_mode in phase_2 must be logged");

    fclose(capture);
    module_system_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

/**
 * Main test runner
 */
int main(void) {
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: SAGE Dependency Contracts\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    init_memory_system(0);

    TEST_RUN(test_dep_apply_infall_missing_prepare_error);
    TEST_RUN(test_dep_apply_cooling_wrong_order_error);
    TEST_RUN(test_dep_supernova_wrong_order_error);
    TEST_RUN(test_dep_apply_sfn_wrong_order_error);
    TEST_RUN(test_dep_quasar_per_event_missing_producer_error);
    TEST_RUN(test_dep_starburst_per_event_missing_producer_error);
    TEST_RUN(test_dep_sf_missing_apply_error);
    TEST_RUN(test_dep_sn_missing_apply_error);
    TEST_RUN(test_dep_apply_sfn_warns_no_prescriptions);
    TEST_RUN(test_dep_resolve_mergers_warns_no_clock);
    TEST_RUN(test_dep_starburst_warns_no_disk_instability);
    TEST_RUN(test_dep_starburst_warns_no_quasar_mode);

    TEST_SUMMARY();

    printf("\n");
    printf("Memory leak check:\n");
    print_allocated();

    return TEST_RESULT();
}
