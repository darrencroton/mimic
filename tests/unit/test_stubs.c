/**
 * @file    test_stubs.c
 * @brief   Stub implementations for unit tests
 *
 * Provides minimal implementations of functions from main.c that are
 * needed by unit tests but can't be linked from main.c (which has main()).
 *
 * Model knowledge does not belong here: model parameter fixtures live in the
 * model package (e.g. models/sage16/modules/_tests/sage_test_fixtures.h), so
 * each model brings its own alongside its tests.
 */

#include <stdio.h>
#include <stdlib.h>

#ifdef HDF5
#include <hdf5.h>
#endif

#include "globals.h"

/** @brief Stub: mark halo as processed without recursing into real tree-build logic.
 *
 * The unit harness links tree_driver.c (which references build_halo_tree) but
 * deliberately not build_model.c: driver plumbing tests (e.g.
 * test_enumerated_driver) exercise unit ordering over synthetic readers and
 * must not run the real recursive tree build. The shared FoF evolution
 * adapters the drivers call live in halo_evolution.c, which IS linked. */
void build_halo_tree(int halonr, int unit, int depth) {
  (void)unit;
  (void)depth;
  if (HaloAux != NULL) {
    HaloAux[halonr].DoneFlag = 1;
  }
}

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

#ifdef HDF5
void __attribute__((weak)) store_run_properties(hid_t master_file_id) { (void)master_file_id; }
#endif
