/**
 * @file    test_inheritance.c
 * @brief   Unit tests for the driver-neutral inheritance service
 *
 * Validates: deep-copy ownership, type transitions, orphan handling,
 * accumulator reset, new-object creation, and subhalo-local central links.
 */

#include "../framework/test_framework.h"
#include "../../src/core/inheritance.h"
#include "../../src/include/generated/property_test_helpers.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void fill_payload(struct HaloInitPayload *payload) {
    memset(payload, 0, sizeof(*payload));
    payload->SnapNum = 5;
    payload->Len = 1234;
    payload->Mvir = 150.0f;
    payload->Rvir = 1.5f;
    payload->Vvir = 250.0f;
    payload->MostBoundID = 987654321LL;
    for (int j = 0; j < 3; j++) {
        payload->Pos[j] = 10.0f + j;
        payload->Vel[j] = 20.0f + j;
        payload->Spin[j] = 0.1f + 0.01f * j;
    }
    payload->Vmax = 300.0f;
    payload->VelDisp = 120.0f;
}

static struct InheritanceDescendant make_descendant(int is_fof_central) {
    struct InheritanceDescendant descendant;
    memset(&descendant, 0, sizeof(descendant));
    descendant.halo_nr = 42;
    descendant.current_snap = 5;
    descendant.current_time = 10.0;
    descendant.new_halo_dt = 2.5;
    descendant.virial_mass = 150.0;
    descendant.virial_radius = 1.5;
    descendant.virial_velocity = 250.0;
    descendant.is_fof_central = is_fof_central;
    descendant.unique_galaxy_id = 111000222LL;
    fill_payload(&descendant.halo_payload);
    return descendant;
}

static void init_source_halo(struct Halo *halo, struct GalaxyData *galaxy,
                             int type) {
    memset(halo, 0, sizeof(*halo));
    memset(galaxy, 0, sizeof(*galaxy));

    halo->SnapNum = 4;
    halo->Type = type;
    halo->CentralHalo = -1;
    halo->HaloNr = 7;
    halo->UniqueGalaxyID = 4444;
    halo->UniqueCentralGalaxyID = 3333;
    halo->dT = 1.0f;
    halo->Len = 500;
    halo->Mvir = 100.0f;
    halo->deltaMvir = 4.0f;
    halo->Rvir = 1.0f;
    halo->Vvir = 200.0f;
    halo->infallMvir = -1.0f;
    halo->infallVvir = -1.0f;
    halo->infallVmax = -1.0f;
    halo->MostBoundID = 100;
    for (int j = 0; j < 3; j++) {
        halo->Pos[j] = 1.0f + j;
        halo->Vel[j] = 2.0f + j;
        halo->Spin[j] = 3.0f + j;
    }
    halo->Vmax = 210.0f;
    halo->VelDisp = 90.0f;
    halo->galaxy = galaxy;

    init_galaxy_defaults(galaxy);
}

static void cleanup_workspace(struct Halo *workspace, int start, int end) {
    for (int i = start; i < end; i++) {
        if (workspace[i].galaxy != NULL) {
            myfree(workspace[i].galaxy);
            workspace[i].galaxy = NULL;
        }
    }
}

int test_main_branch_deep_copy_and_reset(void) {
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    struct Halo source;
    struct GalaxyData source_galaxy;
    struct Halo workspace[2];
    struct InheritanceDescendant descendant = make_descendant(1);
    struct InheritanceProgenitorGalaxy progenitor;
    memset(workspace, 0, sizeof(workspace));
    init_source_halo(&source, &source_galaxy, 0);
    generated_test_seed_init_repeat_properties(source.galaxy);

    progenitor.source = &source;
    progenitor.source_time = 14.0;
    progenitor.is_main_branch = 1;

    int end = inherit_descendant_halos(workspace, 0, 2, &descendant,
                                      &progenitor, 1);

    TEST_ASSERT(end == 1, "Main branch progenitor should produce one workspace halo");
    TEST_ASSERT(workspace[0].galaxy != source.galaxy,
                "Inherited galaxy data must be deep-copied");
    TEST_ASSERT(generated_test_init_repeat_properties_equal_init(workspace[0].galaxy),
                "Snapshot accumulator properties should reset after deep copy");
    TEST_ASSERT(!generated_test_init_repeat_properties_equal_init(source.galaxy),
                "Source galaxy accumulators should remain untouched");
    TEST_ASSERT(workspace[0].Type == 0, "FOF-central main branch should remain Type 0");
    TEST_ASSERT(workspace[0].HaloNr == descendant.halo_nr,
                "Inherited halo should point to descendant halo number");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[0].dT, 4.0, 1e-6,
                             "dT should use source and descendant times");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[0].Mvir, 150.0, 1e-6,
                             "Main branch Mvir should update to descendant value");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[0].deltaMvir, 50.0, 1e-6,
                             "deltaMvir should compare descendant and previous Mvir");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[0].Rvir, 1.5, 1e-6,
                             "Rvir should update when descendant mass is larger");
    TEST_ASSERT(workspace[0].CentralHalo == 0,
                "Single inherited central should point CentralHalo to itself");

    cleanup_workspace(workspace, 0, end);
    check_memory_leaks();
    return TEST_PASS;
}

int test_satellite_transition_captures_infall(void) {
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    struct Halo source;
    struct GalaxyData source_galaxy;
    struct Halo workspace[2];
    struct InheritanceDescendant descendant = make_descendant(0);
    struct InheritanceProgenitorGalaxy progenitor;
    memset(workspace, 0, sizeof(workspace));
    init_source_halo(&source, &source_galaxy, 0);

    progenitor.source = &source;
    progenitor.source_time = 14.0;
    progenitor.is_main_branch = 1;

    int end = inherit_descendant_halos(workspace, 0, 2, &descendant,
                                      &progenitor, 1);

    TEST_ASSERT(end == 1, "Satellite main branch should produce one halo");
    TEST_ASSERT(workspace[0].Type == 1,
                "Main branch in non-FOF subhalo should become Type 1");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[0].infallMvir, 100.0, 1e-6,
                             "Type 0 to Type 1 transition should capture infall Mvir");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[0].infallVvir, 200.0, 1e-6,
                             "Type 0 to Type 1 transition should capture infall Vvir");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[0].infallVmax, 210.0, 1e-6,
                             "Type 0 to Type 1 transition should capture infall Vmax");

    cleanup_workspace(workspace, 0, end);
    check_memory_leaks();
    return TEST_PASS;
}

int test_orphan_conversion_and_local_central(void) {
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    struct Halo sources[2];
    struct GalaxyData source_galaxies[2];
    struct Halo workspace[4];
    struct InheritanceDescendant descendant = make_descendant(0);
    struct InheritanceProgenitorGalaxy progenitors[2];
    memset(workspace, 0, sizeof(workspace));
    init_source_halo(&sources[0], &source_galaxies[0], 1);
    init_source_halo(&sources[1], &source_galaxies[1], 0);

    progenitors[0].source = &sources[0];
    progenitors[0].source_time = 14.0;
    progenitors[0].is_main_branch = 1;
    progenitors[1].source = &sources[1];
    progenitors[1].source_time = 14.0;
    progenitors[1].is_main_branch = 0;

    int end = inherit_descendant_halos(workspace, 0, 4, &descendant,
                                      progenitors, 2);

    TEST_ASSERT(end == 2, "Two non-merged progenitors should produce two halos");
    TEST_ASSERT(workspace[0].Type == 1, "Main Type 1 source should remain Type 1");
    TEST_ASSERT(workspace[1].Type == 2, "Non-main Type 0 source should become orphan");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[1].deltaMvir, -100.0, 1e-6,
                             "Orphan deltaMvir should be negative previous Mvir");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[1].Mvir, 0.0, 1e-6,
                             "Orphan Mvir should be zero");
    TEST_ASSERT(workspace[1].Len == 0, "Orphan Len should be zero");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[1].infallMvir, 100.0, 1e-6,
                             "Type 0 to Type 2 transition should capture infall Mvir");
    TEST_ASSERT(workspace[0].CentralHalo == 0 && workspace[1].CentralHalo == 0,
                "Type 2 orphan should point to the subhalo-local Type 1 central");

    cleanup_workspace(workspace, 0, end);
    check_memory_leaks();
    return TEST_PASS;
}

int test_type3_skip_and_new_halo_creation(void) {
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    struct Halo source;
    struct GalaxyData source_galaxy;
    struct Halo workspace[2];
    struct InheritanceDescendant descendant = make_descendant(1);
    struct InheritanceProgenitorGalaxy progenitor;
    memset(workspace, 0, sizeof(workspace));
    init_source_halo(&source, &source_galaxy, 3);

    progenitor.source = &source;
    progenitor.source_time = 14.0;
    progenitor.is_main_branch = 1;

    int end = inherit_descendant_halos(workspace, 0, 2, &descendant,
                                      &progenitor, 1);

    TEST_ASSERT(end == 1, "All-Type-3 central slice should create a new halo");
    TEST_ASSERT(workspace[0].Type == 0, "New central halo should be Type 0");
    TEST_ASSERT(workspace[0].SnapNum == descendant.current_snap - 1,
                "New halo SnapNum should preserve init_halo parity");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[0].dT, descendant.new_halo_dt, 1e-6,
                             "New halo dT should come from descendant payload");
    TEST_ASSERT(workspace[0].UniqueGalaxyID == descendant.unique_galaxy_id,
                "New halo UniqueGalaxyID should be driver supplied");
    TEST_ASSERT(workspace[0].galaxy != NULL, "New halo should allocate galaxy data");
    TEST_ASSERT(generated_test_default_galaxy_properties_equal_init(workspace[0].galaxy),
                "New halo galaxy properties should use generated defaults");

    cleanup_workspace(workspace, 0, end);
    check_memory_leaks();
    return TEST_PASS;
}

int test_type2_preserved_without_orphan_retransition(void) {
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    struct Halo sources[2];
    struct GalaxyData source_galaxies[2];
    struct Halo workspace[3];
    struct InheritanceDescendant descendant = make_descendant(0);
    struct InheritanceProgenitorGalaxy progenitors[2];
    memset(workspace, 0, sizeof(workspace));
    init_source_halo(&sources[0], &source_galaxies[0], 1);
    init_source_halo(&sources[1], &source_galaxies[1], 2);
    sources[1].Mvir = 75.0f;
    sources[1].deltaMvir = -25.0f;
    sources[1].Len = 0;

    progenitors[0].source = &sources[0];
    progenitors[0].source_time = 14.0;
    progenitors[0].is_main_branch = 1;
    progenitors[1].source = &sources[1];
    progenitors[1].source_time = 14.0;
    progenitors[1].is_main_branch = 0;

    int end = inherit_descendant_halos(workspace, 0, 3, &descendant,
                                      progenitors, 2);

    TEST_ASSERT(end == 2, "Valid Type 1 + Type 2 slice should produce two halos");
    TEST_ASSERT(workspace[1].Type == 2,
                "Type 2 source should remain Type 2 without retransition");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[1].Mvir, 75.0, 1e-6,
                             "Type 2 source Mvir should be preserved");
    TEST_ASSERT_DOUBLE_EQUAL(workspace[1].deltaMvir, -25.0, 1e-6,
                             "Type 2 source deltaMvir should be preserved");
    TEST_ASSERT(workspace[1].Len == 0, "Type 2 source Len should be preserved");
    TEST_ASSERT(workspace[0].CentralHalo == 0 && workspace[1].CentralHalo == 0,
                "Preserved Type 2 should point to the local Type 1 central");

    cleanup_workspace(workspace, 0, end);
    check_memory_leaks();
    return TEST_PASS;
}

int test_no_progenitor_central_creates_new_halo(void) {
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    struct Halo workspace[1];
    struct InheritanceDescendant descendant = make_descendant(1);
    memset(workspace, 0, sizeof(workspace));

    int end = inherit_descendant_halos(workspace, 0, 1, &descendant, NULL, 0);

    TEST_ASSERT(end == 1, "No-progenitor central subhalo should create one halo");
    TEST_ASSERT(workspace[0].Type == 0, "New no-progenitor halo should be Type 0");
    TEST_ASSERT(workspace[0].HaloNr == descendant.halo_nr,
                "New no-progenitor halo should use descendant halo number");
    TEST_ASSERT(workspace[0].SnapNum == descendant.current_snap - 1,
                "New no-progenitor halo should preserve SnapNum parity");
    TEST_ASSERT(workspace[0].UniqueGalaxyID == descendant.unique_galaxy_id,
                "New no-progenitor halo should use driver-supplied identity");
    TEST_ASSERT(workspace[0].CentralHalo == 0,
                "New no-progenitor halo should be its subhalo-local central");

    cleanup_workspace(workspace, 0, end);
    check_memory_leaks();
    return TEST_PASS;
}

int test_no_progenitor_satellite_creates_none(void) {
    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_WARNING, NULL);

    struct Halo workspace[1];
    struct InheritanceDescendant descendant = make_descendant(0);
    memset(workspace, 0, sizeof(workspace));

    int end = inherit_descendant_halos(workspace, 0, 1, &descendant, NULL, 0);

    TEST_ASSERT(end == 0, "No-progenitor satellite subhalo should create no halo");

    check_memory_leaks();
    return TEST_PASS;
}

int main(void) {
    printf("%s", BLUE);
    printf("============================================================\n");
    printf("Test Suite: Inheritance Service\n");
    printf("============================================================\n");
    printf("%s\n", NC);

    initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

    TEST_RUN(test_main_branch_deep_copy_and_reset);
    TEST_RUN(test_satellite_transition_captures_infall);
    TEST_RUN(test_orphan_conversion_and_local_central);
    TEST_RUN(test_type3_skip_and_new_halo_creation);
    TEST_RUN(test_type2_preserved_without_orphan_retransition);
    TEST_RUN(test_no_progenitor_central_creates_new_halo);
    TEST_RUN(test_no_progenitor_satellite_creates_none);

    TEST_SUMMARY();
    return TEST_RESULT();
}
