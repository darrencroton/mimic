/**
 * @file    sage26_oracle_hydrogen.h
 * @brief   Oracle-test helpers for the c08 hydrogen sites of the merger-family routines
 *          (SFprescription 0 and 1)
 *
 * Adds, beside sage26_oracle_fixture.h, sage26_oracle_sizes.h and sage26_oracle_h2.h (which stay
 * as they are), what the oracle tests of the c08 black-hole, burst and merger fixtures need
 * (oracle README "The c08 Group", "The Hydrogen Sites of the Merger Family"):
 *
 * - oracle_c08_require_merger_flags(): c06's merger-family flags with SFprescription read (0 or
 *   1, with the H2 configuration oracle_c06 requires for 1) and BulgeSizeOn read (0 or 3), every
 *   other flag pinned at its R0-R2 value;
 * - oracle_c08_check_bh_hydrogen(): grow_black_hole's accretion cap record (M:366-372), present
 *   only on SFprescription 1 fixtures;
 * - oracle_c08_check_burst_hydrogen(): the burst's record (the observed star-formation commit
 *   caps, the post-reheating cap and the hydrogen refresh, S:1131-1135, S:1169-1173, M:798-803),
 *   present only on SFprescription 1 fixtures.
 *
 * Test-only: included by module oracle tests, never by package code.
 */

#ifndef SAGE26_ORACLE_HYDROGEN_H
#define SAGE26_ORACLE_HYDROGEN_H

#include <stdio.h>

#include "modules/_tests/sage26_oracle_fixture.h"
#include "modules/_tests/sage26_oracle_h2.h"
#include "modules/_tests/sage26_oracle_sizes.h"
#include "shared/sage_agn_physics.h"
#include "shared/sage_starburst_physics.h"

/**
 * @brief c06's merger-family flags with SFprescription and BulgeSizeOn read rather than pinned
 *
 * SFprescription 0 (R0, R1) or 1 (R2, with H2RadialIntegrationOn 1, H2RadialNBins 25 and
 * H2RadialRMaxFactor 5.0); BulgeSizeOn 0 or 3 (with Hubble_h for 3); the other flags are those
 * R0.par, R1.par and R2.par share.
 *
 * @param sf_prescription  Receives the fixture's SFprescription
 * @return Number of problems
 */
static inline int oracle_c08_require_merger_flags(struct oracle_fixture *in,
                                                  struct MimicSageBulgeSize *sizes,
                                                  int *sf_prescription) {
  struct oracle_c08_sf_config cfg;
  int problems = oracle_c08_read_sf_config(in, 0, &cfg);
  *sf_prescription = cfg.sf_prescription;
  problems += oracle_c05_require_flag(in, "params.SupernovaRecipeOn", 1);
  problems += oracle_c05_require_flag(in, "params.CGMrecipeOn", 0);
  problems += oracle_c05_require_flag(in, "params.FIREmodeOn", 0);
  problems += oracle_c07_read_sizes(in, sizes);
  problems += oracle_c05_require_flag(in, "params.SaveFullSFH", 0);
  problems += oracle_c05_require_flag(in, "params.AGNrecipeOn", 2);
  problems += oracle_c05_require_flag(in, "params.StarburstColdGasOn", 1);
  problems += oracle_c05_require_flag(in, "params.DiskInstabilityOn", 1);
  return problems;
}

/**
 * @brief Compare grow_black_hole's hydrogen-cap record with the port's cap locals
 *
 * Written by the oracle only for SFprescription 1: the guard flag (the site's own
 * SFprescription != 0 && != 2), the two independent strict caps and the float bound max_h_bh,
 * compared bit for bit with its markers (tail-checked, not narrowing-limited). An
 * SFprescription 0 fixture has no record, and nothing is read.
 *
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c08_check_bh_hydrogen(struct oracle_fixture *out,
                                               const int sf_prescription,
                                               const struct MimicSageHydrogenCapLocals *caps,
                                               struct oracle_c08_local_tally *tally) {
  if (sf_prescription != MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    return 0;
  }
  int problems = 0;
  problems += oracle_check_i32(out, "intermediate.bh_hydrogen_caps_tracked", caps->tracks_h2);
  problems += oracle_check_i32(out, "intermediate.bh_h2_cap_binds", caps->h2_cap_binds);
  problems += oracle_check_i32(out, "intermediate.bh_h1_cap_binds", caps->h1_cap_binds);
  problems += oracle_c08_check_local(out, "max_h_bh", caps->max_h, 1, 0, tally);
  return problems;
}

/**
 * @brief Compare the burst's hydrogen record with the port's burst locals
 *
 * Written by the oracle only for SFprescription 1. burst_sf_commit_h2_capped and
 * burst_sf_commit_h1_capped are OBSERVATIONS of SAGE26's own update_from_star_formation
 * (whether it changed H2gas or H1gas; a binding strict cap always changes the field), compared
 * with the port's commit cap flags. The post-reheating cap's flags and max_h_fb, and the
 * refresh's flags, its double budget and its fused H I remainder (both narrowing-limited: one
 * narrowing into a float field is the only path of each) are compared bit for bit.
 *
 * @return Number of mismatches and marker errors
 */
static inline int oracle_c08_check_burst_hydrogen(struct oracle_fixture *out,
                                                  const int sf_prescription,
                                                  const struct MimicSageStarburstLocals *burst,
                                                  struct oracle_c08_local_tally *tally) {
  if (sf_prescription != MIMIC_SAGE_SF_PRESCRIPTION_BR06) {
    return 0;
  }
  const struct MimicSageHydrogenCapLocals *fb = &burst->feedback.hydrogen_caps;
  const struct MimicSageBurstHydrogenRefreshLocals *r = &burst->hydrogen_refresh;
  int problems = 0;
  problems +=
      oracle_check_i32(out, "intermediate.burst_sf_commit_h2_capped", burst->sf_caps.h2_cap_binds);
  problems +=
      oracle_check_i32(out, "intermediate.burst_sf_commit_h1_capped", burst->sf_caps.h1_cap_binds);
  problems += oracle_check_i32(out, "intermediate.hydrogen_caps_fb_tracked", fb->tracks_h2);
  problems += oracle_check_i32(out, "intermediate.h2_cap_fb_binds", fb->h2_cap_binds);
  problems += oracle_check_i32(out, "intermediate.h1_cap_fb_binds", fb->h1_cap_binds);
  problems += oracle_check_i32(out, "intermediate.burst_hydrogen_refresh_tracked", r->tracks_h2);
  problems += oracle_check_i32(out, "intermediate.burst_refresh_h2_cap_binds", r->h2_cap_binds);
  problems += oracle_check_i32(out, "intermediate.burst_refresh_h1_floored", r->h1_floored);
  problems += oracle_c08_check_local(out, "max_h_fb", fb->max_h, 1, 0, tally);
  problems += oracle_c08_check_local(out, "burst_h2_budget", r->h2_budget, 0, 1, tally);
  problems += oracle_c08_check_local(out, "burst_h1_refresh", r->h1_refresh, 0, 1, tally);
  return problems;
}

#endif /* SAGE26_ORACLE_HYDROGEN_H */
