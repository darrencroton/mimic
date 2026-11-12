/**
 * @file    test_unit_sage26_apply_star_formation_supernova.c
 * @brief   Unit tests for sage26_apply_star_formation_supernova module
 *
 * Validates: the commit of one substep's star formation and supernova feedback to the forming
 * galaxy and its FoF central, the disk star-formation history, the CGMrecipeOn routing and the
 * feedback-free-burst bypass
 *
 * The module hands a substep's NewStellarMass, SupernovaReheatedMass and SupernovaEjectedMass to
 * SAGE26's two shared commits (shared/sage_sf_feedback_physics.h), so the tests pin the module's
 * own contract: what it caps, samples, routes and leaves for sage26_apply_metal_enrichment, and
 * the float rounding and hydrogen caps of the commits it calls.
 *
 * Test cases:
 *   - test_module_initialization: the module initialises and cleans up in its valid placement
 *   - test_commit_table: one table of substeps with exact literal expectations (RecycleFraction
 *     0.25, so a swapped R and 1 - R differ): star formation with recycling and the metal
 *     transfer at the cold-gas metallicity, the reheated gas and metals into the central's hot
 *     pair at the post-star-formation metallicity, the ejection capped after the reheating and
 *     its metals at the central's metallicity after the reheating, the reheated mass clamped to
 *     the remaining cold gas, the cold-gas cap without the supernova calculator (stars above and
 *     at zero cold gas), the StarFormationRate guard on a non-positive interval, and
 *     SupernovaOutflowRate accumulating the undivided mass. Every row also asserts that the
 *     forming galaxy's own hot and ejected pairs and the central's other reservoirs are
 *     untouched, that NewStellarMass is left for the enrichment module, that the consumed
 *     transport fields are zeroed, and that baryons and metals are conserved
 *   - test_zero_event_caps_negative_hot_metals: a zero event still runs the metal cap, and moves
 *     nothing else
 *   - test_sf_commit_fused_rounding: the shared SF commit rounds as SAGE26's fused build
 *   - test_commit_hydrogen_caps: SFprescription 1's H2gas and H1gas caps after the star-formation
 *     commit and after the feedback commit's reheating debit (each independently and strictly at
 *     max_h of the cold gas left, never their sum, the feedback's on the reheated galaxy only),
 *     and no cap at SFprescription 0
 *   - test_disk_history_sampled_every_call: SfrDisk[bin] += stars / dt and the cold-gas samples on
 *     every call over a 13-substep snapshot, zero-star calls included
 *   - test_disk_history_without_central: the Mimic-only no-central path still samples
 *   - test_disk_history_initial_boundary: no interval (snapshot-zero birth) samples nothing
 *   - test_cgm_feedback_destination: CGMrecipeOn 1 against 0 on the same input: the feedback into
 *     the FoF central's CGM pair (central Regime 0) or hot pair (Regime 1, or CGMrecipeOn 0), the
 *     ejection cap and the metal cap binding and not, the unselected pair and the satellite's own
 *     pairs unchanged; the satellite's Regime is always the other value (the commit routes by the
 *     FoF central's regime)
 *   - test_ffb_row_bypassed: a row whose FFBRegime is 1 leaves itself and its FoF central
 *     bytewise unchanged, its history included, also in an interval the time guard refuses (the
 *     bypass precedes the history sample and the guard); the same row with FFBRegime 0 is sampled
 *     and committed, its reheated gas to the central
 *
 * The CGMrecipeOn refusals of the module are asserted, with diagnostics, by
 * test_unit_sage26_parameter_refusals.c.
 *
 * @author  Mimic Development Team
 * @date    2025-12-18
 */

#include "../../../../tests/framework/test_framework.h"
#include "core/module_registry.h"
#include "../../../../tests/framework/test_phase_config.h"
#include "core/module_interface.h"
#include "include/types.h"
#include "include/proto.h"
#include "include/globals.h"
#include "util/error.h"
#include "util/memory.h"
#include "shared/metallicity.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/sage_sfr_history.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Shared sage26 test fixture boilerplate (counters, config reset, module registration) */
#include "modules/_tests/sage_test_fixtures.h"

/* Module functions (extern declarations for direct testing) */
extern int sage26_apply_star_formation_supernova_init(void);
extern int sage26_apply_star_formation_supernova_process(struct ModuleContext *ctx,
                                                         struct Halo *halos, int ngal);
extern int sage26_apply_star_formation_supernova_cleanup(void);

// ============================================================================
// TEST FIXTURES AND HELPERS
// ============================================================================

/**
 * @brief   Setup module parameters for testing
 *
 * Sets RecycleFraction plus the module's required SFprescription (0) and CGMrecipeOn (0).
 *
 * @param   recycle     RecycleFraction [0,1]
 */
static void setup_test_parameters(double recycle) {
  /* Set model parameters in MimicConfig */
  snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "RecycleFraction");
  snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "%.6f", recycle);
  MimicConfig.NumModelParams = 1;

  /* The module's required SFprescription and CGMrecipeOn */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_APPLY_MODULE);
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */
}

/**
 * @brief   Setup module context for testing
 *
 * @param   ctx             ModuleContext to initialize
 * @param   central         Pointer to central galaxy halo
 * @param   dt              Time step
 */
static void setup_module_context(struct ModuleContext *ctx, struct Halo *central, double dt) {
  memset(ctx, 0, sizeof(struct ModuleContext));
  ctx->central_galaxy = central;
  ctx->substep_dt = dt;
  ctx->params = &MimicConfig;
  ctx->redshift = 0.0;
  ctx->time = 13.8; /* Gyr */
  ctx->snapshot_number = 63;
  ctx->substep_number = 0;
  ctx->num_substeps = 1;
}

/**
 * @brief   Setup test halo and galaxy with specified properties
 *
 * @param   halo            Halo to initialize
 * @param   galaxy          Galaxy to initialize
 * @param   type            Halo type (0=central, 1=satellite, 2=orphan)
 * @param   mvir            Virial mass (1e10 Msun/h)
 * @param   dt              Time step (Gyr) for rate calculations
 * @param   cold_gas        Cold gas mass (1e10 Msun/h)
 * @param   hot_gas         Hot gas mass (1e10 Msun/h)
 * @param   ejected_gas     Ejected gas mass (1e10 Msun/h)
 * @param   stellar_mass    Stellar mass (1e10 Msun/h)
 * @param   metals_cold     Metals in cold gas (1e10 Msun/h)
 * @param   metals_hot      Metals in hot gas (1e10 Msun/h)
 * @param   metals_ejected  Metals in ejected gas (1e10 Msun/h)
 * @param   metals_stellar  Metals in stellar mass (1e10 Msun/h)
 * @param   new_stars       NewStellarMass (calculated by previous module)
 * @param   reheated_mass   SupernovaReheatedMass (calculated by previous module)
 * @param   ejected_mass    SupernovaEjectedMass (calculated by previous module)
 */
static void setup_test_halo(struct Halo *halo, struct GalaxyData *galaxy, int type, double mvir,
                            double dt, double cold_gas, double hot_gas, double ejected_gas,
                            double stellar_mass, double metals_cold, double metals_hot,
                            double metals_ejected, double metals_stellar, double new_stars,
                            double reheated_mass, double ejected_mass) {
  memset(halo, 0, sizeof(struct Halo));
  memset(galaxy, 0, sizeof(struct GalaxyData));

  halo->Type = type;
  halo->Mvir = mvir;
  halo->Vvir = 150.0; /* km/s */
  halo->SnapNum = 63;
  halo->dT = dt;
  halo->galaxy = galaxy;

  galaxy->ColdGas = cold_gas;
  galaxy->HotGas = hot_gas;
  galaxy->EjectedGas = ejected_gas;
  galaxy->StellarMass = stellar_mass;
  galaxy->MetalsColdGas = metals_cold;
  galaxy->MetalsHotGas = metals_hot;
  galaxy->MetalsEjectedGas = metals_ejected;
  galaxy->MetalsStellarMass = metals_stellar;
  galaxy->NewStellarMass = new_stars;
  galaxy->SupernovaReheatedMass = reheated_mass;
  galaxy->SupernovaEjectedMass = ejected_mass;
  galaxy->StarFormationRate = 0.0;
  galaxy->SupernovaOutflowRate = 0.0;
}

// ============================================================================
// SOFTWARE QUALITY TESTS
// ============================================================================

/**
 * @test    test_module_initialization
 * @brief   Test module initialization and cleanup lifecycle
 *
 * Expected: Module init and cleanup succeed without errors or leaks
 * Validates: Module lifecycle management
 */
int test_module_initialization(void) {
  /* ===== SETUP ===== */
  reset_config();
  init_memory_system(0);
  ensure_modules_registered();

  /* Set up minimal cosmology configuration */
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;

  /* Configure sage26_apply_star_formation_supernova module in galaxy_physics */
  test_phase_add("galaxy_physics", "sage26_apply_star_formation_supernova",
                 PROCESSING_MODE_BY_GALAXY);
  MimicConfig.SubSteps = 1;
  set_test_model_parameters();
  /* the module's required SFprescription */
  add_test_sf_prescription_parameters(SAGE26_TEST_KEYS_APPLY_MODULE);
  add_test_cgm_recipe_parameter(); /* CGMrecipeOn: 0, required */

  /* ===== EXECUTE ===== */
  int result = module_system_init();

  /* ===== VALIDATE ===== */
  TEST_ASSERT(result == 0, "Module system initialization should succeed");

  /* ===== CLEANUP ===== */
  module_system_cleanup();
  check_memory_leaks();

  return TEST_PASS;
}

/** @brief A double from its IEEE-754 bit pattern */
static double f64_from_bits(const uint64_t bits) {
  double value;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

/** @brief The IEEE-754 bit pattern of a float */
static uint32_t f32_bits(const float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/**
 * @test    test_sf_commit_fused_rounding
 * @brief   The shared SF commit rounds each update once, as SAGE26 does
 *
 * SAGE26's update_from_star_formation updates [ColdGas, StellarMass] += [-(1-R), (1-R)] * stars
 * and [MetalsColdGas, MetalsStellarMass] += [-(Z(1-R)), Z(1-R)] * stars with one rounding per
 * update. The inputs lie near a float rounding midpoint, where a fused and an unfused commit
 * round to different floats: case 0 differs in StellarMass, cases 1 and 2 in ColdGas and
 * StellarMass, case 3 in MetalsColdGas and MetalsStellarMass. R is 0.43. The expected bit
 * patterns were produced by executing SAGE26's own update_from_star_formation.
 */
int test_sf_commit_fused_rounding(void) {
  static const struct {
    uint32_t in[4]; /* ColdGas, StellarMass, MetalsColdGas, MetalsStellarMass */
    uint64_t stars;
    uint64_t metallicity;
    uint32_t sage26_out[4]; /* SAGE26's update_from_star_formation, executed */
  } cases[] = {
      {{0x3f800000, 0x3dcccccd, 0x00000000, 0x00000000},
       0x3fc1f707f9435e4fULL,
       0x0000000000000000ULL,
       {0x3f6b851b, 0x3e3851fb, 0x00000000, 0x00000000}},
      {{0x40400000, 0x40200000, 0x3d3851ec, 0x3d19999a},
       0x3fe0d79398b3a630ULL,
       0x3f8eb85200000000ULL,
       {0x402ccccd, 0x40333333, 0x3d25e355, 0x3d2c0831}},
      {{0x40400000, 0x40200000, 0x3d3851ec, 0x3d19999a},
       0x3fdc11f7e50d794aULL,
       0x3f8eb85200000000ULL,
       {0x402fffff, 0x40300001, 0x3d28f5c3, 0x3d28f5c3}},
      {{0x40400000, 0x40200000, 0x3d3851ec, 0x3d19999a},
       0x3fdc11f7ee68caf6ULL,
       0x3f8eb85200000000ULL,
       {0x402fffff, 0x40300001, 0x3d28f5c3, 0x3d28f5c3}},
  };
  const double recycle_fraction = f64_from_bits(0x3fdb851eb851eb85ULL); /* 0.43 */

  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    struct GalaxyData gal;
    memset(&gal, 0, sizeof(gal));
    gal.ColdGas = f32_from_bits(cases[c].in[0]);
    gal.StellarMass = f32_from_bits(cases[c].in[1]);
    gal.MetalsColdGas = f32_from_bits(cases[c].in[2]);
    gal.MetalsStellarMass = f32_from_bits(cases[c].in[3]);
    const double metallicity = mimic_get_metallicity(gal.ColdGas, gal.MetalsColdGas);
    TEST_ASSERT(metallicity == f64_from_bits(cases[c].metallicity),
                "the cold-gas metallicity must equal SAGE26's get_metallicity");

    /* SFprescription 0: SAGE26's commit, with no hydrogen cap */
    mimic_sage_update_from_star_formation(&gal, f64_from_bits(cases[c].stars), metallicity,
                                          recycle_fraction, MIMIC_SAGE_SF_PRESCRIPTION_CROTON,
                                          NULL);

    TEST_ASSERT(f32_bits(gal.ColdGas) == cases[c].sage26_out[0],
                "ColdGas must round as SAGE26's fused update");
    TEST_ASSERT(f32_bits(gal.StellarMass) == cases[c].sage26_out[1],
                "StellarMass must round as SAGE26's fused update");
    TEST_ASSERT(f32_bits(gal.MetalsColdGas) == cases[c].sage26_out[2],
                "MetalsColdGas must round as SAGE26's fused update");
    TEST_ASSERT(f32_bits(gal.MetalsStellarMass) == cases[c].sage26_out[3],
                "MetalsStellarMass must round as SAGE26's fused update");
  }
  return TEST_PASS;
}

// ============================================================================
// HYDROGEN CAPS OF THE SHARED COMMITS (SFPRESCRIPTION 1)
// ============================================================================
//
// Invariants of the two hydrogen caps SAGE26 runs for a prescription that tracks H2:
// update_from_star_formation's after its four reservoir stores and update_from_feedback's after
// the cold-gas and metal debits. Each bound is the float
// max_h = (ColdGas > 0.0f) ? ColdGas * 0.74 : 0.0f of the cold gas the commit leaves, a double
// product narrowed to float. The rows hold every float as a literal that round-trips (nine
// significant digits), the neighbours of a bound being its adjacent floats.

/** @brief A star-formation commit of a galaxy with the given cold gas, H2gas and H1gas */
static void commit_star_formation(struct GalaxyData *gal, const float cold_gas, const float h2,
                                  const float h1, const double stars, const int sf_prescription,
                                  struct MimicSageHydrogenCapLocals *caps) {
  memset(gal, 0, sizeof(*gal));
  gal->ColdGas = cold_gas;
  gal->StellarMass = 1.0f;
  gal->H2gas = h2;
  gal->H1gas = h1;
  mimic_sage_update_from_star_formation(gal, stars, 0.0, 0.43, sf_prescription, caps);
}

/** @brief A feedback commit of a galaxy (reheating into a separate central) */
static void commit_feedback(struct GalaxyData *gal, struct GalaxyData *central,
                            const float cold_gas, const float h2, const float h1,
                            const double reheated, const int sf_prescription,
                            struct MimicSageFeedbackLocals *locals) {
  memset(gal, 0, sizeof(*gal));
  memset(central, 0, sizeof(*central));
  gal->ColdGas = cold_gas;
  gal->H2gas = h2;
  gal->H1gas = h1;
  /* The central holds far more H2 and H1 than any bound here: a cap on it would show */
  central->HotGas = 10.0f;
  central->H2gas = 50.0f;
  central->H1gas = 60.0f;
  mimic_sage_update_from_feedback(gal, central, reheated, 0.0, 0.0, sf_prescription,
                                  MIMIC_SAGE_CGM_RECIPE_OFF, locals);
}

/** @brief Whether two floats have the same bit pattern (so -0.0 and +0.0 differ); reports if not */
static int expect_bits(const char *row, const char *field, const float actual,
                       const float expected) {
  if (f32_bits(actual) == f32_bits(expected)) {
    return 1;
  }
  printf("\n  row %s: %s = %.9g (0x%08x), expected %.9g (0x%08x)", row, field, (double)actual,
         f32_bits(actual), (double)expected, f32_bits(expected));
  return 0;
}

/**
 * @test    test_commit_hydrogen_caps
 * @brief   Both shared commits, at SFprescription 1, cap H2gas and H1gas independently and
 *          strictly at max_h of the cold gas they leave, never their sum
 *
 * Row kinds: a commit of stars (0) or of reheated mass (1); the bound is 0.74 * 1.0 after a
 * zero-star event, 0.74 * 0.75 after reheating 0.25 from 1.0, 0.74 * 1.715 after 0.5 stars from
 * 2.0 (R 0.43); H2gas and H1gas above, at and below the bound, together above it, with no cold
 * gas (the bound +0.0), at -0.0 (kept: the caps are strict); the feedback commit leaves the
 * central's H2gas and H1gas alone and credits its hot gas; SFprescription 0 writes neither.
 */
int test_commit_hydrogen_caps(void) {
  static const struct {
    const char *name;
    int feedback;        /* 0: update_from_star_formation, 1: update_from_feedback */
    int sf_prescription; /* SFprescription */
    float cold, h2, h1;  /* the forming galaxy before the commit */
    double amount;       /* stars formed (0) or reheated mass (1) */
    float x_cold;        /* the cold gas after the commit */
    float x_h2, x_h1;    /* H2gas and H1gas after the commit */
    float x_max_h;       /* the bound (read only when SFprescription 1) */
    int x_h2_binds, x_h1_binds;
  } rows[] = {
      /* star formation, stars 0: the bound is 0.74 * 1.0 = 0.74000001 */
      {"sf H2 above", 0, 1, 1.0f, 0.740000069f, 0.74000001f, 0.0, 1.0f, 0.74000001f, 0.74000001f,
       0.74000001f, 1, 0},
      {"sf H1 above", 0, 1, 1.0f, 0.74000001f, 0.740000069f, 0.0, 1.0f, 0.74000001f, 0.74000001f,
       0.74000001f, 0, 1},
      {"sf both below", 0, 1, 1.0f, 0.73999995f, 0.73999995f, 0.0, 1.0f, 0.73999995f, 0.73999995f,
       0.74000001f, 0, 0},
      {"sf sum above", 0, 1, 1.0f, 0.5f, 0.5f, 0.0, 1.0f, 0.5f, 0.5f, 0.74000001f, 0, 0},
      /* the bound uses the cold gas the stars leave (2.0 - 0.57 * 0.5 = 1.715, bound 1.2691):
       * H2gas 1.4 is within the hydrogen before the stars (1.48) and above it after */
      {"sf bound after stars", 0, 1, 2.0f, 1.39999998f, 0.100000001f, 0.5, 1.71500003f, 1.26910007f,
       0.100000001f, 1.26910007f, 1, 0},
      {"sf no cold gas", 0, 1, 0.0f, 0.100000001f, 0.200000003f, 0.0, 0.0f, 0.0f, 0.0f, 0.0f, 1, 1},
      {"sf negative zero kept", 0, 1, 0.0f, -0.0f, -0.0f, 0.0, 0.0f, -0.0f, -0.0f, 0.0f, 0, 0},
      {"sf prescription 0", 0, 0, 1.0f, 5.0f, 6.0f, 0.5, 0.714999974f, 5.0f, 6.0f, 0.0f, 0, 0},
      /* feedback, reheating 0.25 from 1.0: the bound is 0.74 * 0.75 = 0.555000007 */
      {"fb H2 above", 1, 1, 1.0f, 0.555000067f, 0.555000007f, 0.25, 0.75f, 0.555000007f,
       0.555000007f, 0.555000007f, 1, 0},
      {"fb H1 above", 1, 1, 1.0f, 0.555000007f, 0.555000067f, 0.25, 0.75f, 0.555000007f,
       0.555000007f, 0.555000007f, 0, 1},
      {"fb both below", 1, 1, 1.0f, 0.554999948f, 0.554999948f, 0.25, 0.75f, 0.554999948f,
       0.554999948f, 0.555000007f, 0, 0},
      {"fb sum above", 1, 1, 1.0f, 0.400000006f, 0.400000006f, 0.25, 0.75f, 0.400000006f,
       0.400000006f, 0.555000007f, 0, 0},
      /* the reheated mass clamps to the cold gas: all of it leaves, the bound is +0.0 */
      {"fb all reheated", 1, 1, 0.5f, 0.100000001f, 0.200000003f, 1.0, 0.0f, 0.0f, 0.0f, 0.0f, 1,
       1},
      {"fb negative zero kept", 1, 1, 0.5f, -0.0f, -0.0f, 1.0, 0.0f, -0.0f, -0.0f, 0.0f, 0, 0},
      {"fb prescription 0", 1, 0, 1.0f, 5.0f, 6.0f, 0.25, 0.75f, 5.0f, 6.0f, 0.0f, 0, 0},
  };

  int ok = 1;
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    const char *name = rows[r].name;
    struct GalaxyData gal;
    struct GalaxyData central;
    struct MimicSageHydrogenCapLocals caps;
    if (rows[r].feedback) {
      struct MimicSageFeedbackLocals locals;
      commit_feedback(&gal, &central, rows[r].cold, rows[r].h2, rows[r].h1, rows[r].amount,
                      rows[r].sf_prescription, &locals);
      caps = locals.hydrogen_caps;
      /* The reheated gas reaches the central's hot gas; the cap never acts on the central */
      ok &= expect_bits(name, "central HotGas", central.HotGas,
                        10.0f + (rows[r].cold - rows[r].x_cold));
      ok &= expect_bits(name, "central H2gas", central.H2gas, 50.0f);
      ok &= expect_bits(name, "central H1gas", central.H1gas, 60.0f);
    } else {
      commit_star_formation(&gal, rows[r].cold, rows[r].h2, rows[r].h1, rows[r].amount,
                            rows[r].sf_prescription, &caps);
    }
    ok &= expect_bits(name, "ColdGas", gal.ColdGas, rows[r].x_cold);
    ok &= expect_bits(name, "H2gas", gal.H2gas, rows[r].x_h2);
    ok &= expect_bits(name, "H1gas", gal.H1gas, rows[r].x_h1);

    const int tracks_h2 = rows[r].sf_prescription == MIMIC_SAGE_SF_PRESCRIPTION_BR06;
    if (caps.tracks_h2 != tracks_h2) {
      printf("\n  row %s: tracks_h2 = %d, expected %d", name, caps.tracks_h2, tracks_h2);
      ok = 0;
    }
    if (tracks_h2) {
      ok &= expect_bits(name, "max_h", caps.max_h, rows[r].x_max_h);
      if (caps.h2_cap_binds != rows[r].x_h2_binds || caps.h1_cap_binds != rows[r].x_h1_binds) {
        printf("\n  row %s: caps bind %d/%d (H2/H1), expected %d/%d", name, caps.h2_cap_binds,
               caps.h1_cap_binds, rows[r].x_h2_binds, rows[r].x_h1_binds);
        ok = 0;
      }
    }
  }
  TEST_ASSERT(ok, "every commit must cap H2gas and H1gas strictly, each at its own max_h");
  return TEST_PASS;
}

// ============================================================================
// THE COMMIT TABLE
// ============================================================================
//
// One forming galaxy and one separate FoF central, one substep (SubSteps 1, so the substep's dt is
// the row's dT), no supernova calculator configured (so the cold-gas cap is live), CGMrecipeOn 0
// and SFprescription 0. Every value is a float: the inputs and expectations that are binary
// fractions are exact, the rest are the nine-digit literals of the floats they round to.
// RecycleFraction is 0.25, so the locked fraction 1 - R = 0.75 is not the recycled one.

/** @brief Whether two floats are equal; reports the row and field if not */
static int expect_float(const char *row, const char *field, const float actual,
                        const float expected) {
  if (actual == expected) {
    return 1;
  }
  printf("\n  row %s: %s = %.9g, expected %.9g", row, field, (double)actual, (double)expected);
  return 0;
}

/**
 * @test    test_commit_table
 * @brief   The apply step's commit of one substep, row by row, on exact literals
 *
 * Physics per row: stars lock (1 - R) * stars of cold gas, at the cold gas's metallicity, into
 * the galaxy's stars; the reheated mass (clamped to the cold gas left) moves from the galaxy's
 * cold gas to the central's hot gas at the post-star-formation metallicity; the ejected mass
 * (capped at the central's hot gas after the reheating) moves from the central's hot gas to its
 * ejected gas at the central's metallicity after the reheating; SupernovaOutflowRate gains the
 * undivided reheated mass; StarFormationRate gains stars / dT when dT is positive. Without the
 * supernova calculator the apply step caps stars at the cold gas (zero for none). NewStellarMass
 * is left, the reheated and ejected masses are zeroed.
 *
 * The "metals after star formation" row is the one a stale pre-star-formation metallicity
 * would get wrong: the cold gas 2.0 holds 0.1f metals, the stars take 0.5 * 0.75 of it, and the
 * metallicity the reheating uses is the ratio of the floats left, which differs from the
 * pre-star-formation ratio by one float rounding that shows in the credited metals.
 */
int test_commit_table(void) {
  static const struct {
    const char *name;
    double recycle, dt;
    int initial_interval; /* SnapNum -1: the substep has no positive dt */
    /* the forming galaxy */
    float cold, hot, ejected, stellar;
    float metals_cold, metals_hot, metals_ejected, metals_stellar;
    float outflow; /* SupernovaOutflowRate before the call */
    /* NewStellarMass, SupernovaReheatedMass and SupernovaEjectedMass */
    double stars, reheated, ejected_mass;
    /* the FoF central */
    float c_hot, c_ejected, c_metals_hot, c_metals_ejected;
    /* the forming galaxy after the call (hot, ejected and their metals unchanged) */
    float x_cold, x_stellar, x_metals_cold, x_metals_stellar;
    float x_sfr, x_outflow, x_new_stars;
    /* the central after the call */
    float x_c_hot, x_c_ejected, x_c_metals_hot, x_c_metals_ejected;
  } rows[] = {
      /* stars 1.0 lock 0.75 at Z 0.125 (1.25 / 10); SFR 1.0 / 0.5 */
      {.name = "star formation",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 10.0f,
       .hot = 50.0f,
       .stellar = 20.0f,
       .metals_cold = 1.25f,
       .metals_stellar = 0.25f,
       .stars = 1.0,
       .x_cold = 9.25f,
       .x_stellar = 20.75f,
       .x_metals_cold = 1.15625f,
       .x_metals_stellar = 0.34375f,
       .x_sfr = 2.0f,
       .x_new_stars = 1.0f},
      /* stars as above; reheat 1.0 at the post-star-formation Z 0.125 into the central's hot gas
       * (7 -> 8, metals 1.75 + 0.125 = 1.875, Z 0.234375, not the pre-reheating 0.25), then eject
       * 0.5 at that Z: 0.1171875 of metals */
      {.name = "reheat then eject",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 5.0f,
       .metals_cold = 0.625f,
       .stars = 1.0,
       .reheated = 1.0,
       .ejected_mass = 0.5,
       .c_hot = 7.0f,
       .c_ejected = 1.0f,
       .c_metals_hot = 1.75f,
       .c_metals_ejected = 0.125f,
       .x_cold = 3.25f,
       .x_stellar = 0.75f,
       .x_metals_cold = 0.40625f,
       .x_metals_stellar = 0.09375f,
       .x_sfr = 2.0f,
       .x_outflow = 1.0f,
       .x_new_stars = 1.0f,
       .x_c_hot = 7.5f,
       .x_c_ejected = 1.5f,
       .x_c_metals_hot = 1.7578125f,
       .x_c_metals_ejected = 0.2421875f},
      /* the metals credited at the metallicity of the floats left after the stars (see above),
       * and a seeded outflow rate that accumulates: 0.3f + 0.5 */
      {.name = "metals after star formation",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 2.0f,
       .metals_cold = 0.1f,
       .outflow = 0.3f,
       .stars = 0.5,
       .reheated = 0.5,
       .x_cold = 1.125f,
       .x_stellar = 0.375f,
       .x_metals_cold = 0.0562500022f,
       .x_metals_stellar = 0.0187500007f,
       .x_sfr = 1.0f,
       .x_outflow = 0.800000012f,
       .x_new_stars = 0.5f,
       .x_c_hot = 0.5f,
       .x_c_metals_hot = 0.0250000022f},
      /* no stars; the central's hot gas 0.5 plus the reheated 0.5 is 1.0, which caps the 5.0 of
       * ejection (capped before the reheating it would be 0.5); all its metals go with it */
      {.name = "ejection capped after reheating",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 4.0f,
       .metals_cold = 0.5f,
       .reheated = 0.5,
       .ejected_mass = 5.0,
       .c_hot = 0.5f,
       .c_metals_hot = 0.0625f,
       .x_cold = 3.5f,
       .x_metals_cold = 0.4375f,
       .x_outflow = 0.5f,
       .x_c_ejected = 1.0f,
       .x_c_metals_ejected = 0.125f},
      /* the cold gas left after the stars is 1.25: the 2.0 reheated is clamped to it, so the
       * galaxy is emptied and the central gains 1.25 and its metals, not 2.0 */
      {.name = "reheating clamped to the cold gas",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 2.0f,
       .metals_cold = 0.25f,
       .stars = 1.0,
       .reheated = 2.0,
       .c_hot = 10.0f,
       .x_stellar = 0.75f,
       .x_metals_stellar = 0.09375f,
       .x_sfr = 2.0f,
       .x_outflow = 1.25f,
       .x_new_stars = 1.0f,
       .x_c_hot = 11.25f,
       .x_c_metals_hot = 0.15625f},
      /* stars 2.0 exceed the cold gas 0.5: capped at it and written back */
      {.name = "stars capped at the cold gas",
       .recycle = 0.25,
       .dt = 0.5,
       .cold = 0.5f,
       .stellar = 20.0f,
       .metals_cold = 0.0625f,
       .stars = 2.0,
       .x_cold = 0.125f,
       .x_stellar = 20.375f,
       .x_metals_cold = 0.015625f,
       .x_metals_stellar = 0.046875f,
       .x_sfr = 1.0f,
       .x_new_stars = 0.5f},
      /* no cold gas: the cap is zero (not a negative or the stars), so nothing is formed */
      {.name = "stars capped at no cold gas", .recycle = 0.25, .dt = 0.5, .stars = 1.0},
      /* a substep with no positive dt (SnapNum -1) is not an error; the stars still form but no
       * rate is accumulated (a rate of 1.0 / -1.0 would be -1) */
      {.name = "no positive interval",
       .recycle = 0.25,
       .dt = -1.0,
       .initial_interval = 1,
       .cold = 10.0f,
       .stars = 1.0,
       .x_cold = 9.25f,
       .x_stellar = 0.75f,
       .x_new_stars = 1.0f},
  };

  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); r++) {
    const char *name = rows[r].name;
    reset_config();
    setup_test_parameters(rows[r].recycle);
    TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "init should succeed");

    struct Halo central_halo, halo;
    struct GalaxyData central, gal;
    setup_test_halo(&central_halo, &central, 0, 100.0, rows[r].dt, 0.0, rows[r].c_hot,
                    rows[r].c_ejected, 0.0, 0.0, rows[r].c_metals_hot, rows[r].c_metals_ejected,
                    0.0, 0.0, 0.0, 0.0);
    setup_test_halo(&halo, &gal, 0, 100.0, rows[r].dt, rows[r].cold, rows[r].hot, rows[r].ejected,
                    rows[r].stellar, rows[r].metals_cold, rows[r].metals_hot,
                    rows[r].metals_ejected, rows[r].metals_stellar, rows[r].stars, rows[r].reheated,
                    rows[r].ejected_mass);
    gal.SupernovaOutflowRate = rows[r].outflow;
    if (rows[r].initial_interval) {
      halo.SnapNum = -1;
    }
    struct ModuleContext ctx;
    setup_module_context(&ctx, &central_halo, rows[r].dt);

    const double baryons_before = (double)gal.ColdGas + gal.HotGas + gal.EjectedGas +
                                  gal.StellarMass + central.ColdGas + central.HotGas +
                                  central.EjectedGas + central.StellarMass;
    const double metals_before = (double)gal.MetalsColdGas + gal.MetalsHotGas +
                                 gal.MetalsEjectedGas + gal.MetalsStellarMass +
                                 central.MetalsColdGas + central.MetalsHotGas +
                                 central.MetalsEjectedGas + central.MetalsStellarMass;
    const float hot = gal.HotGas, ejected = gal.EjectedGas, metals_hot = gal.MetalsHotGas,
                metals_ejected = gal.MetalsEjectedGas;

    const int status = sage26_apply_star_formation_supernova_process(&ctx, &halo, 1);
    if (status != 0) {
      printf("\n  row %s: process returned %d", name, status);
      ok = 0;
    }

    /* the forming galaxy */
    ok &= expect_float(name, "ColdGas", gal.ColdGas, rows[r].x_cold);
    ok &= expect_float(name, "StellarMass", gal.StellarMass, rows[r].x_stellar);
    ok &= expect_float(name, "MetalsColdGas", gal.MetalsColdGas, rows[r].x_metals_cold);
    ok &= expect_float(name, "MetalsStellarMass", gal.MetalsStellarMass, rows[r].x_metals_stellar);
    ok &= expect_float(name, "StarFormationRate", gal.StarFormationRate, rows[r].x_sfr);
    ok &= expect_float(name, "SupernovaOutflowRate", gal.SupernovaOutflowRate, rows[r].x_outflow);
    ok &= expect_float(name, "HotGas (own pair untouched)", gal.HotGas, hot);
    ok &= expect_float(name, "EjectedGas (own pair untouched)", gal.EjectedGas, ejected);
    ok &= expect_float(name, "MetalsHotGas (own pair untouched)", gal.MetalsHotGas, metals_hot);
    ok &= expect_float(name, "MetalsEjectedGas (own pair untouched)", gal.MetalsEjectedGas,
                       metals_ejected);
    /* the transport fields: stars kept for the enrichment module, the feedback consumed */
    ok &= expect_float(name, "NewStellarMass (kept)", gal.NewStellarMass, rows[r].x_new_stars);
    ok &= expect_float(name, "SupernovaReheatedMass (zeroed)", gal.SupernovaReheatedMass, 0.0f);
    ok &= expect_float(name, "SupernovaEjectedMass (zeroed)", gal.SupernovaEjectedMass, 0.0f);

    /* the central, whose cold gas, stars and own transport fields are not the destination */
    ok &= expect_float(name, "central HotGas", central.HotGas, rows[r].x_c_hot);
    ok &= expect_float(name, "central EjectedGas", central.EjectedGas, rows[r].x_c_ejected);
    ok &= expect_float(name, "central MetalsHotGas", central.MetalsHotGas, rows[r].x_c_metals_hot);
    ok &= expect_float(name, "central MetalsEjectedGas", central.MetalsEjectedGas,
                       rows[r].x_c_metals_ejected);
    ok &= expect_float(name, "central ColdGas", central.ColdGas, 0.0f);
    ok &= expect_float(name, "central StellarMass", central.StellarMass, 0.0f);

    /* the baryons and the metals only move between reservoirs */
    const double baryons_after = (double)gal.ColdGas + gal.HotGas + gal.EjectedGas +
                                 gal.StellarMass + central.ColdGas + central.HotGas +
                                 central.EjectedGas + central.StellarMass;
    const double metals_after = (double)gal.MetalsColdGas + gal.MetalsHotGas +
                                gal.MetalsEjectedGas + gal.MetalsStellarMass +
                                central.MetalsColdGas + central.MetalsHotGas +
                                central.MetalsEjectedGas + central.MetalsStellarMass;
    if (fabs(baryons_after - baryons_before) > 1e-5 || fabs(metals_after - metals_before) > 1e-5) {
      printf("\n  row %s: baryons %.9g -> %.9g, metals %.9g -> %.9g", name, baryons_before,
             baryons_after, metals_before, metals_after);
      ok = 0;
    }
    sage26_apply_star_formation_supernova_cleanup();
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "every substep must commit exactly its row's literals");
  return TEST_PASS;
}

/**
 * @test    test_zero_event_caps_negative_hot_metals
 * @brief   A zero event still runs update_from_feedback's metal cap, and moves nothing else
 *
 * SAGE26 calls update_from_feedback on every substep, also with no stars. With zero ejected
 * mass the metal transfer metallicityHot * 0 = 0 exceeds a negative MetalsHotGas, so the cap
 * moves the negative hot metals into the ejected reservoir and leaves MetalsHotGas at zero.
 * A module that returned early on zero stars would not. Every other reservoir keeps its value and
 * the consumed transport fields stay zero.
 */
int test_zero_event_caps_negative_hot_metals(void) {
  setup_test_parameters(0.43);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0,
              "Module initialization should succeed");

  struct Halo test_halo;
  struct GalaxyData test_galaxy;
  const double negative_hot_metals = -1.0e-9;
  setup_test_halo(&test_halo, &test_galaxy, 0, 100.0, 0.1, 10.0, 50.0, 5.0, 20.0, 0.2,
                  negative_hot_metals, 0.1, 0.3, 0.0, 0.0, 0.0);
  const float ejected_metals_before = test_galaxy.MetalsEjectedGas;
  const float hot_metals_before = test_galaxy.MetalsHotGas;

  struct ModuleContext ctx;
  setup_module_context(&ctx, &test_halo, 0.1);

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &test_halo, 1) == 0,
              "Module process should succeed");
  TEST_ASSERT(test_galaxy.MetalsHotGas == 0.0f,
              "the zero event's capped transfer empties the negative hot metals");
  TEST_ASSERT(test_galaxy.MetalsEjectedGas == (float)(ejected_metals_before + hot_metals_before),
              "the negative hot metals move into the ejected reservoir");
  TEST_ASSERT(test_galaxy.HotGas == 50.0f && test_galaxy.ColdGas == 10.0f &&
                  test_galaxy.EjectedGas == 5.0f && test_galaxy.StellarMass == 20.0f,
              "no gas or stars move in a zero event");
  TEST_ASSERT(test_galaxy.SupernovaReheatedMass == 0.0f && test_galaxy.SupernovaEjectedMass == 0.0f,
              "the transport fields stay zero");
  return TEST_PASS;
}

// ============================================================================
// DISK STAR-FORMATION HISTORY SAMPLING
// ============================================================================

/** @brief Element (history, bin) of a galaxy */
static float history(struct GalaxyData *gal, const enum MimicSageSfrHistory h, const int bin) {
  return *mimic_sage_sfr_history(gal, h, bin);
}

/**
 * @test    test_disk_history_sampled_every_call
 * @brief   Every call samples the disk history at its substep's bin, zero stars included
 *
 * SAGE26 samples on every starformation_and_feedback call, after the feedback
 * renormalisation and before any reservoir update:
 *   SfrDisk[bin] += stars / dt; SfrDiskColdGas[bin] = ColdGas;
 *   SfrDiskColdGasMetals[bin] = MetalsColdGas.
 * With 13 substeps, bins 0, 3 and 6 are shared by two substeps (bin (step * 10) / 13).
 * Every third substep forms no stars. The expected rate is formed with SAGE26's
 * expression in float storage; the gas samples are the reservoirs before the call's
 * commit, the last substep of a bin winning.
 */
int test_disk_history_sampled_every_call(void) {
  reset_config();
  setup_test_parameters(0.43);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 0, 100.0, 0.13, 10.0, 50.0, 5.0, 20.0, 0.2, 0.5, 0.1, 0.3, 0.0, 0.0,
                  0.0);
  struct ModuleContext ctx;
  setup_module_context(&ctx, &halo, 0.01);
  ctx.num_substeps = 13;
  const double dt = halo.dT / 13.0;

  struct GalaxyData expected;
  memcpy(&expected, &gal, sizeof(expected));
  for (int step = 0; step < 13; step++) {
    ctx.substep_number = step;
    const double stars = (step % 3 == 2) ? 0.0 : 0.01 * (step + 1);
    gal.NewStellarMass = stars;
    gal.SupernovaReheatedMass = 0.0;
    gal.SupernovaEjectedMass = 0.0;
    const int bin = (step * 10) / 13;
    *mimic_sage_sfr_history(&expected, MIMIC_SFR_HISTORY_DISK, bin) += stars / dt;
    *mimic_sage_sfr_history(&expected, MIMIC_SFR_HISTORY_DISK_COLD_GAS, bin) = gal.ColdGas;
    *mimic_sage_sfr_history(&expected, MIMIC_SFR_HISTORY_DISK_COLD_GAS_METALS, bin) =
        gal.MetalsColdGas;

    TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) == 0,
                "process should succeed");
    for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
      for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
        TEST_ASSERT(history(&gal, (enum MimicSageSfrHistory)h, k) ==
                        history(&expected, (enum MimicSageSfrHistory)h, k),
                    "every history element equals SAGE26's sampling after every substep");
      }
    }
  }
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK, 6) > 0.0f, "a shared bin sums its rates");
  sage26_apply_star_formation_supernova_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_disk_history_without_central
 * @brief   With no FoF central (a Mimic-only path) the history is sampled, nothing committed
 */
int test_disk_history_without_central(void) {
  reset_config();
  setup_test_parameters(0.43);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 1, 10.0, 0.1, 2.0, 0.0, 0.0, 1.0, 0.02, 0.0, 0.0, 0.01, 0.5, 0.1,
                  0.0);
  struct ModuleContext ctx;
  setup_module_context(&ctx, NULL, 0.01);
  ctx.num_substeps = 10;
  ctx.substep_number = 9;

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) == 0,
              "process should succeed");
  float rate = 0.0f;
  rate += 0.5 / (0.1 / 10.0);
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK, 9) == rate, "the rate is sampled");
  TEST_ASSERT(history(&gal, MIMIC_SFR_HISTORY_DISK_COLD_GAS, 9) == 2.0f, "the gas is sampled");
  TEST_ASSERT(gal.ColdGas == 2.0f, "nothing is committed without a central");
  sage26_apply_star_formation_supernova_cleanup();
  return TEST_PASS;
}

/**
 * @test    test_disk_history_initial_boundary
 * @brief   Without a positive interval (snapshot-zero birth) there is no dt: nothing sampled
 */
int test_disk_history_initial_boundary(void) {
  reset_config();
  setup_test_parameters(0.43);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "init should succeed");

  struct Halo halo;
  struct GalaxyData gal;
  setup_test_halo(&halo, &gal, 0, 10.0, -1.0, 2.0, 1.0, 0.0, 1.0, 0.02, 0.0, 0.0, 0.01, 0.0, 0.0,
                  0.0);
  halo.SnapNum = -1;
  struct ModuleContext ctx;
  setup_module_context(&ctx, &halo, 0.0);
  ctx.num_substeps = 10;

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) == 0,
              "the boundary is not an error");
  for (int h = 0; h < MIMIC_SFR_HISTORY_COUNT; h++) {
    for (int k = 0; k < MIMIC_SAGE_SFR_STEPS; k++) {
      TEST_ASSERT(history(&gal, (enum MimicSageSfrHistory)h, k) == 0.0f, "nothing is sampled");
    }
  }

  /* A non-positive interval off the boundary is an inconsistent state */
  halo.SnapNum = 40;
  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &halo, 1) != 0,
              "an invalid interval is an error");
  sage26_apply_star_formation_supernova_cleanup();
  return TEST_PASS;
}

// ============================================================================
// CGM ROUTING TESTS (CGMrecipeOn)
// ============================================================================
//
// SAGE26's update_from_feedback credits the reheated gas to, caps the ejection against and
// ejects from the FoF CENTRAL's CGM pair when CGMrecipeOn is 1 and the central's Regime is 0,
// and its hot pair otherwise. The forming galaxy's own Regime is never read. Each case below
// gives the central's two pairs the SAME input values, so the selected pair ends at the case's
// literal values and the unselected one keeps its input; the forming satellite always holds the
// other Regime. Every value is a short binary fraction, so every expectation is an exact literal:
//
//   satellite ColdGas 4, MetalsColdGas 0.5 (Z 0.125), no stars, reheated 1 -> ColdGas 3,
//   MetalsColdGas 0.375, outflow 1; the central's ejected pair 0.5 / 0.0625.
//   A (no cap binds): pairs 2 / 0.25, ejected 0.5 -> selected 3 / 0.375 after the reheating,
//     metallicity 0.125, -> 2.5 / 0.3125; ejected pair 1 / 0.125.
//   B (the gas cap binds): ejected 5 -> capped to the selected 3 after the reheating -> 0 / 0;
//     ejected pair 3.5 / 0.4375.
//   C (the metal cap binds): pairs 2 / -0.5 -> 3 / -0.375 after the reheating; metallicity 0
//     (get_metallicity of negative metals), the transfer 0 is above -0.375, so it is capped to
//     -0.375 -> 2.5 / 0; ejected pair 1 / -0.3125. A constructed
//     edge state (negative metals arise from float cancellation, as test_zero_event_caps_negative_
//     hot_metals uses for the hot pair).

/** @brief Initialise the module with CGMrecipeOn `value` */
static int cgm_init(const char *value) {
  setup_test_parameters(0.5);
  set_test_cgm_recipe_on(value);
  return sage26_apply_star_formation_supernova_init();
}

/**
 * @test    test_cgm_feedback_destination
 * @brief   The disk commit's feedback goes to the FoF central's pair its Regime selects
 */
int test_cgm_feedback_destination(void) {
  const struct {
    const char *cgm;
    int central_regime;
    int to_cgm;
  } runs[] = {{"1", 0, 1}, {"1", 1, 0}, {"0", 0, 0}, {"0", 1, 0}};
  const struct {
    const char *name;
    float pair_gas, pair_metals; /* both central pairs' input */
    double ejected;              /* SupernovaEjectedMass */
    float gas, metals;           /* the selected pair after the call */
    float ejected_gas, ejected_metals;
  } cases[] = {{"A no cap", 2.0f, 0.25f, 0.5, 2.5f, 0.3125f, 1.0f, 0.125f},
               {"B gas cap", 2.0f, 0.25f, 5.0, 0.0f, 0.0f, 3.5f, 0.4375f},
               {"C metal cap", 2.0f, -0.5f, 0.5, 2.5f, 0.0f, 1.0f, -0.3125f}};
  int ok = 1;
  init_memory_system(0);
  for (size_t r = 0; r < sizeof(runs) / sizeof(runs[0]); r++) {
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
      TEST_ASSERT(cgm_init(runs[r].cgm) == 0, "init must accept CGMrecipeOn 0 and 1");

      struct Halo central_halo, satellite_halo;
      struct GalaxyData central, satellite;
      setup_test_halo(&central_halo, &central, 0, 100.0, 0.1, 0.0, 0.0, 0.5, 0.0, 0.0, 0.0, 0.0625,
                      0.0, 0.0, 0.0, 0.0);
      central.Regime = runs[r].central_regime;
      central.HotGas = cases[c].pair_gas;
      central.MetalsHotGas = cases[c].pair_metals;
      central.CGMgas = cases[c].pair_gas;
      central.MetalsCGMgas = cases[c].pair_metals;
      setup_test_halo(&satellite_halo, &satellite, 1, 20.0, 0.1, 4.0, 7.0, 0.0, 1.0, 0.5, 0.75, 0.0,
                      0.0, 0.0, 1.0, cases[c].ejected);
      satellite.Regime = 1 - runs[r].central_regime;
      satellite.CGMgas = 6.0f;
      satellite.MetalsCGMgas = 0.5f;

      struct ModuleContext ctx;
      setup_module_context(&ctx, &central_halo, 0.1);
      const int status = sage26_apply_star_formation_supernova_process(&ctx, &satellite_halo, 1);

      const float *sel_gas = runs[r].to_cgm ? &central.CGMgas : &central.HotGas;
      const float *sel_metals = runs[r].to_cgm ? &central.MetalsCGMgas : &central.MetalsHotGas;
      const float *oth_gas = runs[r].to_cgm ? &central.HotGas : &central.CGMgas;
      const float *oth_metals = runs[r].to_cgm ? &central.MetalsHotGas : &central.MetalsCGMgas;
      const int selected_ok = *sel_gas == cases[c].gas && *sel_metals == cases[c].metals;
      const int unselected_ok =
          *oth_gas == cases[c].pair_gas && *oth_metals == cases[c].pair_metals;
      const int ejected_ok = central.EjectedGas == cases[c].ejected_gas &&
                             central.MetalsEjectedGas == cases[c].ejected_metals;
      const int satellite_ok = satellite.ColdGas == 3.0f && satellite.MetalsColdGas == 0.375f &&
                               satellite.SupernovaOutflowRate == 1.0f && satellite.HotGas == 7.0f &&
                               satellite.MetalsHotGas == 0.75f && satellite.CGMgas == 6.0f &&
                               satellite.MetalsCGMgas == 0.5f && satellite.EjectedGas == 0.0f;
      if (status != 0 || !selected_ok || !unselected_ok || !ejected_ok || !satellite_ok) {
        printf("\n  CGMrecipeOn %s, central Regime %d, case %s: status %d, hot %.9g/%.9g, CGM "
               "%.9g/%.9g, ejected %.9g/%.9g, satellite cold %.9g/%.9g",
               runs[r].cgm, runs[r].central_regime, cases[c].name, status, (double)central.HotGas,
               (double)central.MetalsHotGas, (double)central.CGMgas, (double)central.MetalsCGMgas,
               (double)central.EjectedGas, (double)central.MetalsEjectedGas,
               (double)satellite.ColdGas, (double)satellite.MetalsColdGas);
        ok = 0;
      }
      sage26_apply_star_formation_supernova_cleanup();
    }
  }
  check_memory_leaks();
  TEST_ASSERT(ok, "the feedback must reach exactly the FoF central's selected pair");
  return TEST_PASS;
}

// ============================================================================
// FEEDBACK-FREE-BURST BYPASS
// ============================================================================

/**
 * @test    test_ffb_row_bypassed
 * @brief   An FFBRegime 1 row is not processed; the same row with FFBRegime 0 is
 *
 * SAGE26's starformation_and_feedback hands an FFBRegime 1 row to starformation_ffb and returns
 * before its ordinary history sample and commits. The row is a satellite with seeded transport
 * fields and its own FoF central; with FFBRegime 1 both galaxy structs are compared byte for
 * byte with their inputs, in a valid interval and in one the time guard refuses on an ordinary
 * row (dT 0 with a stored snapshot).
 * With FFBRegime 0 the expectations are literals: SfrDisk[0] += 1.0 / 0.1, the locked
 * (1 - 0.43) * 1.0 leaves the cold gas, then the reheated 2.0, which the central's hot gas
 * gains before 0.5 is ejected from it (CGMrecipeOn 0).
 */
int test_ffb_row_bypassed(void) {
  reset_config();
  setup_test_parameters(0.43);
  TEST_ASSERT(sage26_apply_star_formation_supernova_init() == 0, "Module init should succeed");

  struct Halo central, row;
  struct GalaxyData central_gal, gal;
  setup_test_halo(&central, &central_gal, 0, 100.0, 0.1, 10.0, 50.0, 0.0, 20.0, 0.0, 0.0, 0.0, 0.0,
                  0.0, 0.0, 0.0);
  setup_test_halo(&row, &gal, 1, 10.0, 0.1, 10.0, 5.0, 0.0, 2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 2.0, 0.5);
  gal.FFBRegime = 1;
  struct ModuleContext ctx;
  setup_module_context(&ctx, &central, 0.1);

  struct GalaxyData row_in, central_in;
  memcpy(&row_in, &gal, sizeof(gal));
  memcpy(&central_in, &central_gal, sizeof(central_gal));

  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &row, 1) == 0,
              "process succeeds on an FFB row");
  TEST_ASSERT(memcmp(&gal, &row_in, sizeof(gal)) == 0,
              "FFBRegime 1: every field of the row is unchanged, its history included");
  TEST_ASSERT(memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
              "FFBRegime 1: every field of its central is unchanged");

  row.dT = 0.0;
  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &row, 1) == 0 &&
                  memcmp(&gal, &row_in, sizeof(gal)) == 0 &&
                  memcmp(&central_gal, &central_in, sizeof(central_gal)) == 0,
              "FFBRegime 1: returns before the history sample's time guard, writing nothing");
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &row, 1) != 0,
              "the time guard refuses that interval on an ordinary row");
  row.dT = 0.1;

  memcpy(&gal, &row_in, sizeof(gal));
  gal.FFBRegime = 0;
  TEST_ASSERT(sage26_apply_star_formation_supernova_process(&ctx, &row, 1) == 0,
              "process succeeds on an ordinary row");
  printf("\n  FFBRegime 0: SfrDisk[0] %.9g, ColdGas %.9g, StellarMass %.9g, central HotGas %.9g, "
         "central EjectedGas %.9g",
         (double)gal.SfrDiskBin0, (double)gal.ColdGas, (double)gal.StellarMass,
         (double)central_gal.HotGas, (double)central_gal.EjectedGas);
  TEST_ASSERT_DOUBLE_EQUAL(gal.SfrDiskBin0, 10.0, 1e-5, "FFBRegime 0: the history is sampled");
  TEST_ASSERT_DOUBLE_EQUAL(gal.StellarMass, 2.57, 1e-5, "FFBRegime 0: the locked stars");
  TEST_ASSERT_DOUBLE_EQUAL(gal.ColdGas, 10.0 - 0.57 - 2.0, 1e-5,
                           "FFBRegime 0: the locked stars and the reheated gas leave the cold gas");
  TEST_ASSERT_DOUBLE_EQUAL(
      central_gal.HotGas, 50.0 + 2.0 - 0.5, 1e-5,
      "FFBRegime 0: the central's hot gas gains the reheated, loses the ejected");
  TEST_ASSERT_DOUBLE_EQUAL(central_gal.EjectedGas, 0.5, 1e-7, "FFBRegime 0: the ejection");
  TEST_ASSERT(gal.HotGas == 5.0f, "FFBRegime 0: the row's own hot gas is not the destination");
  TEST_ASSERT(gal.SupernovaReheatedMass == 0.0 && gal.SupernovaEjectedMass == 0.0,
              "FFBRegime 0: the consumed transport fields are zeroed");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all sage26_apply_star_formation_supernova unit tests and reports results.
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: sage26_apply_star_formation_supernova Module\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  /* Module lifecycle */
  TEST_RUN(test_module_initialization);

  /* The commits: the apply step's table, the zero event, the shared commits' own contracts */
  TEST_RUN(test_commit_table);
  TEST_RUN(test_zero_event_caps_negative_hot_metals);
  TEST_RUN(test_sf_commit_fused_rounding);
  TEST_RUN(test_commit_hydrogen_caps);

  /* Run disk star-formation history sampling tests */
  TEST_RUN(test_disk_history_sampled_every_call);
  TEST_RUN(test_disk_history_without_central);
  TEST_RUN(test_disk_history_initial_boundary);

  /* CGMrecipeOn routing */
  TEST_RUN(test_cgm_feedback_destination);

  /* The feedback-free-burst bypass */
  TEST_RUN(test_ffb_row_bypassed);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
