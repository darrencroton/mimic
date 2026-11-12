/**
 * @file    test_unit_sage_bulge_size_physics.c
 * @brief   Unit tests for the BulgeSizeOn 3 bulge sizes and the merger remnant radius
 *
 * Checks shared/sage_bulge_size_physics.h's mode-3 routines, mimic_sage_get_bulge_radius() and
 * mimic_sage_update_instability_bulge_radius(), and shared/sage_merger_physics.h's
 * mimic_sage_merger_remnant_radius() against expressions written here from the documented
 * equations (h is the Hubble parameter, masses in 1e10 Msun/h, radii in Mpc/h):
 *
 *   - Shen et al. (2003) eq. 33 fallback: R = 10^(0.56 log10(M 1e10 / h) - 5.54) 1e-3 h
 *   - Tonini et al. (2016) eq. 15: R_inst = (R_old M_old + 0.2 R_disc dM) / M_new
 *   - Tonini et al. (2016) eq. 25: R_bulge = (M_merger R_merger + M_inst R_inst) / M_total
 *   - energy conservation with Covington et al. (2011) radiative losses:
 *     R = M_tot^2 / (E_init + E_orb + E_rad), E_init = M1^2 / R1 + M2^2 / R2,
 *     E_orb = M1 M2 / (R1 + R2), E_rad = 2.75 E_init f_gas
 *
 * Every float sum the routines form is exact for these inputs, so their double returns match these
 * expressions to 1e-12 relative (fma against separate rounding), and each stored float radius
 * matches the float of its expression to one float ulp. One row of each table is also checked
 * against a hand-computed literal.
 *
 * Test cases:
 *   - test_get_bulge_radius_mode3: the weighted average, the zeroing of a channel with no mass,
 *     the Shen merger fallback, the 0.2 DiskScaleRadius and Shen instability fallbacks, the
 *     zeroing for a non-positive total, and a stale radius kept beside a negative channel mass
 *   - test_update_instability_bulge_radius_mode3: the first transfer, the eq. 15 average, an
 *     old mass without a radius, and a non-positive transfer that changes nothing
 *   - test_merger_remnant_radius: gas-rich and gas-free pairs, each zero-radius fallback, the
 *     two zero returns and the mass-weighted fallback for a non-positive total energy
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../../../../tests/framework/test_framework.h"

#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_merger_physics.h"

static int passed = 0;
static int failed = 0;

/* h = 0.5 makes the Shen fallback's M 1e10 / h exactly 1e10 for M = 0.5 */
#define TEST_H 0.5
#define DOUBLE_RTOL 1e-12
#define FLOAT_RTOL 0x1p-23

static const struct MimicSageBulgeSize MODE3 = {MIMIC_SAGE_BULGE_SIZE_TONINI, TEST_H};

/** Shen et al. (2003) eq. 33, as the header documents it */
static double shen_radius(double mass) {
  return pow(10.0, 0.56 * log10(mass * 1.0e10 / TEST_H) - 5.54) * 1.0e-3 * TEST_H;
}

static int near(double actual, double expected, double rtol) {
  return fabs(actual - expected) <= rtol * fabs(expected);
}

/** The float field matches the float store of the expected double to one ulp */
static int near_float(float actual, double expected) {
  return near(actual, (float)expected, FLOAT_RTOL);
}

/**
 * @test    test_get_bulge_radius_mode3
 * @brief   get_bulge_radius with BulgeSizeOn 3, branch by branch
 *
 * Hand check of the Shen row: 0.5 * 1e10 / 0.5 = 1e10, so log R_kpc = 0.56 * 10 - 5.54 = 0.06
 * and R = 10^0.06 * 1e-3 * 0.5 = 5.7407681e-4.
 */
int test_get_bulge_radius_mode3(void) {
  const float rm = 0.004f, ri = 0.003f, rd = 0.01f;
  const struct {
    const char *name;
    float m_merger, m_inst, r_merger, r_inst, r_disc;
    double merger, inst, bulge;
  } rows[] = {
      {"both channels sized: the channel-mass-weighted average", 1.0f, 3.0f, rm, ri, rd, rm, ri,
       (1.0 * rm + 3.0 * ri) / 4.0},
      {"no merger mass: its stale radius zeroed", 0.0f, 2.0f, rm, ri, rd, 0.0, ri, ri},
      {"merger mass without a radius: the Shen fallback", 0.5f, 0.0f, 0.0f, ri, rd,
       shen_radius(0.5), 0.0, shen_radius(0.5)},
      {"instability mass without a radius: 0.2 DiskScaleRadius", 1.0f, 1.0f, rm, 0.0f, rd, rm,
       0.2 * rd, (rm + 0.2 * rd) / 2.0},
      {"instability mass without a radius or a disk: the Shen fallback", 1.0f, 0.5f, rm, 0.0f, 0.0f,
       rm, shen_radius(0.5), (rm + 0.5 * shen_radius(0.5)) / 1.5},
      {"no bulge mass: every radius zeroed", 0.0f, 0.0f, rm, ri, rd, 0.0, 0.0, 0.0},
      {"negative merger mass: its stale radius kept", -0.5f, 1.0f, rm, ri, rd, rm, ri,
       (-0.5 * rm + ri) / 0.5},
  };
  int ok = 1;
  TEST_ASSERT(near(shen_radius(0.5), 5.7407681e-4, 1e-8), "the Shen expression hand check");
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    struct GalaxyData gal;
    memset(&gal, 0, sizeof(gal));
    gal.MergerBulgeMass = rows[r].m_merger;
    gal.InstabilityBulgeMass = rows[r].m_inst;
    gal.MergerBulgeRadius = rows[r].r_merger;
    gal.InstabilityBulgeRadius = rows[r].r_inst;
    gal.DiskScaleRadius = rows[r].r_disc;
    gal.BulgeRadius = 9.0f;
    const double returned = mimic_sage_get_bulge_radius(&gal, &MODE3, NULL);
    const int row_ok = near(returned, rows[r].bulge, DOUBLE_RTOL) &&
                       near_float(gal.BulgeRadius, rows[r].bulge) &&
                       near_float(gal.MergerBulgeRadius, rows[r].merger) &&
                       near_float(gal.InstabilityBulgeRadius, rows[r].inst);
    if (!row_ok) {
      fprintf(stderr,
              "  row '%s': returned %.17g (expected %.17g); radii merger %.9g (%.9g) instability "
              "%.9g (%.9g) bulge %.9g\n",
              rows[r].name, returned, rows[r].bulge, (double)gal.MergerBulgeRadius, rows[r].merger,
              (double)gal.InstabilityBulgeRadius, rows[r].inst, (double)gal.BulgeRadius);
      ok = 0;
    }
  }
  TEST_ASSERT(ok, "each get_bulge_radius branch must give the tabulated radii");
  return TEST_PASS;
}

/**
 * @test    test_update_instability_bulge_radius_mode3
 * @brief   update_instability_bulge_radius with BulgeSizeOn 3, then the synchronisation
 *
 * The caller has already added dM to InstabilityBulgeMass, so M_old = InstabilityBulgeMass - dM.
 * The disk radius passed (0.01) is the caller's saved one, not the galaxy's DiskScaleRadius
 * (0.03). A merger channel of 1 at 0.004 is synchronised with the new radius into BulgeRadius;
 * the stale BulgeRadius 9 survives only where nothing runs. Hand check of the eq. 15 row:
 * (0.004 * 1 + 0.2 * 0.01 * 1) / 2 = 0.003.
 */
int test_update_instability_bulge_radius_mode3(void) {
  const float rm = 0.004f, r_old = 0.004f;
  const double r_disc = 0.01;
  const struct {
    const char *name;
    float m_inst_post, r_inst_old;
    double delta, inst, bulge;
  } rows[] = {
      {"first transfer: 0.2 times the saved disk radius", 1.0f, 0.0f, 1.0, 0.2 * r_disc,
       (rm + 0.2 * r_disc) / 2.0},
      {"eq. 15: the mass-weighted average with the old radius", 2.0f, r_old, 1.0,
       (r_old * 1.0 + 0.2 * r_disc * 1.0) / 2.0, (rm + 2.0 * (r_old + 0.2 * r_disc) / 2.0) / 3.0},
      {"old mass without a radius: restarts from the disk", 2.0f, 0.0f, 1.0, 0.2 * r_disc,
       (rm + 2.0 * 0.2 * r_disc) / 3.0},
      {"non-positive transfer: nothing changes", 2.0f, r_old, 0.0, r_old, 9.0},
  };
  int ok = 1;
  TEST_ASSERT(near(rows[1].inst, 0.003, 1e-7), "the eq. 15 expression hand check");
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    struct GalaxyData gal;
    memset(&gal, 0, sizeof(gal));
    gal.MergerBulgeMass = 1.0f;
    gal.MergerBulgeRadius = rm;
    gal.InstabilityBulgeMass = rows[r].m_inst_post;
    gal.InstabilityBulgeRadius = rows[r].r_inst_old;
    gal.DiskScaleRadius = 0.03f;
    gal.BulgeRadius = 9.0f;
    mimic_sage_update_instability_bulge_radius(&gal, rows[r].delta, r_disc, &MODE3, NULL);
    const int row_ok = near_float(gal.InstabilityBulgeRadius, rows[r].inst) &&
                       near_float(gal.BulgeRadius, rows[r].bulge) && gal.MergerBulgeRadius == rm;
    if (!row_ok) {
      fprintf(stderr,
              "  row '%s': instability radius %.9g (expected %.9g), bulge radius %.9g "
              "(expected %.9g), merger radius %.9g\n",
              rows[r].name, (double)gal.InstabilityBulgeRadius, rows[r].inst,
              (double)gal.BulgeRadius, rows[r].bulge, (double)gal.MergerBulgeRadius);
      ok = 0;
    }
  }
  TEST_ASSERT(ok, "each instability-radius update must give the tabulated radii");
  return TEST_PASS;
}

/** One progenitor of the remnant-radius calculation */
struct Progenitor {
  float stellar, cold, bulge, disk_radius, bulge_radius;
};

/** The disk-and-bulge mass-weighted half-mass radius, 0 without stars or cold gas */
static double half_mass_radius(const struct Progenitor *p) {
  const double mass = (double)p->stellar + p->cold;
  if (mass <= 0.0) {
    return 0.0;
  }
  const double disk = (double)p->cold + p->stellar - p->bulge;
  return (disk * 1.68 * p->disk_radius + (double)p->bulge * p->bulge_radius) / mass;
}

/**
 * The remnant radius by energy conservation: a non-positive radius takes the other progenitor's
 * (the first's first), two non-positive radii or no mass give 0, and a non-positive total
 * energy gives the mass-weighted average radius
 */
static double expected_remnant_radius(const struct Progenitor *g1, const struct Progenitor *g2) {
  const double m1 = (double)g1->stellar + g1->cold;
  const double m2 = (double)g2->stellar + g2->cold;
  const double m_tot = m1 + m2;
  if (m_tot <= 0.0) {
    return 0.0;
  }
  double r1 = half_mass_radius(g1);
  double r2 = half_mass_radius(g2);
  if (r1 <= 0.0) {
    r1 = r2;
  }
  if (r2 <= 0.0) {
    r2 = r1;
  }
  if (r1 <= 0.0) {
    return 0.0;
  }
  const double e_init = m1 * m1 / r1 + m2 * m2 / r2;
  const double e_orb = m1 * m2 / (r1 + r2);
  const double e_rad = 2.75 * e_init * ((double)g1->cold + g2->cold) / m_tot;
  const double e_final = e_init + e_orb + e_rad;
  return e_final > 0.0 ? m_tot * m_tot / e_final : (m1 * r1 + m2 * r2) / m_tot;
}

/**
 * @test    test_merger_remnant_radius
 * @brief   calculate_merger_remnant_radius on each branch, g1 the target and g2 the satellite
 *
 * Hand check of the gas-rich row: R1 = ((1 + 1) * 1.68 * 0.01 + 1 * 0.002) / 3 = 0.0118667,
 * R2 = 1.68 * 0.005 = 0.0084, E_init = 9 / R1 + 1 / R2 = 877.48, E_orb = 3 / (R1 + R2) = 148.03,
 * f_gas = 1 / 4, E_rad = 2.75 * 877.48 / 4 = 603.27, so R = 16 / 1628.78 = 0.00982340. The
 * negative cold gas of the last row (a state the routine guards, not one a run produces) makes
 * f_gas = -1.5 and E_final negative.
 */
int test_merger_remnant_radius(void) {
  const struct Progenitor disky = {2.0f, 1.0f, 1.0f, 0.01f, 0.002f};
  const struct Progenitor small_disk = {1.0f, 0.0f, 0.0f, 0.005f, 0.0f};
  const struct Progenitor unsized = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  const struct Progenitor empty = {0.0f, 0.0f, 0.0f, 0.01f, 0.004f};
  const struct {
    const char *name;
    struct Progenitor g1, g2;
  } rows[] = {
      {"gas-rich pair: radiative losses included", disky, small_disk},
      {"gas-free pair", {6.0f, 0.0f, 3.0f, 0.01f, 0.004f}, {2.0f, 0.0f, 2.0f, 0.0f, 0.001f}},
      {"g1 without baryons takes g2's radius", empty, small_disk},
      {"g2 without a radius takes g1's", disky, unsized},
      {"neither has a radius: 0", unsized, unsized},
      {"no baryons at all: 0", empty, empty},
      {"non-positive total energy: the mass-weighted radius",
       {2.0f, -1.5f, 0.0f, 0.01f, 0.0f},
       {0.5f, 0.0f, 0.0f, 0.005f, 0.0f}},
  };
  int ok = 1;
  TEST_ASSERT(near(expected_remnant_radius(&disky, &small_disk), 9.8233955e-3, 1e-7),
              "the energy-conservation expression hand check");
  TEST_ASSERT(near(expected_remnant_radius(&empty, &small_disk), 0.0084, 1e-7) &&
                  near(expected_remnant_radius(&rows[6].g1, &rows[6].g2), 0.0126, 1e-7) &&
                  expected_remnant_radius(&unsized, &unsized) == 0.0,
              "the fallback expressions hand check");
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    struct GalaxyData g1, g2;
    memset(&g1, 0, sizeof(g1));
    memset(&g2, 0, sizeof(g2));
    const struct Progenitor *in[2] = {&rows[r].g1, &rows[r].g2};
    struct GalaxyData *gal[2] = {&g1, &g2};
    for (int k = 0; k < 2; k++) {
      gal[k]->StellarMass = in[k]->stellar;
      gal[k]->ColdGas = in[k]->cold;
      gal[k]->BulgeMass = in[k]->bulge;
      gal[k]->DiskScaleRadius = in[k]->disk_radius;
      gal[k]->BulgeRadius = in[k]->bulge_radius;
    }
    const double expected = expected_remnant_radius(&rows[r].g1, &rows[r].g2);
    const double actual = mimic_sage_merger_remnant_radius(&g1, &g2, NULL);
    const int row_ok = expected == 0.0 ? actual == 0.0 : near(actual, expected, DOUBLE_RTOL);
    if (!row_ok) {
      fprintf(stderr, "  row '%s': remnant radius %.17g, expected %.17g\n", rows[r].name, actual,
              expected);
      ok = 0;
    }
  }
  TEST_ASSERT(ok, "each remnant radius must match the energy-conservation expression");
  return TEST_PASS;
}

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Unit Test Suite: SAGE26 Bulge Sizes (BulgeSizeOn 3) and Remnant Radius\n");
  printf("============================================================\n");
  printf("%s", NC);

  TEST_RUN(test_get_bulge_radius_mode3);
  TEST_RUN(test_update_instability_bulge_radius_mode3);
  TEST_RUN(test_merger_remnant_radius);

  TEST_SUMMARY();
  return TEST_RESULT();
}
