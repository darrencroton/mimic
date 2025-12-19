/**
 * @file    sage_quasar_mode.c
 * @brief   SAGE quasar-mode AGN feedback - BH growth and energy-driven winds
 *
 * Implements Kauffmann & Haehnelt (2000) black hole growth model coupled with
 * energy-driven gas ejection. BH growth is triggered by disk instability OR
 * mergers, followed by powerful quasar winds that eject gas from the galaxy.
 *
 * Physics Summary:
 *   Step 1 - Black Hole Growth:
 *     Triggers: UnstableDiskGasFraction > 0 OR IsMerging
 *     BH_accrete = BlackHoleGrowthRate * efficiency_factor / (1 + (V_thresh/V_vir)^2) * ColdGas
 *     BlackHoleMass += BH_accrete
 *     QuasarModeBHaccretionMass += BH_accrete (tracker for wind)
 *
 *   Step 2 - Quasar Wind:
 *     If QuasarModeBHaccretionMass > 0:
 *       E_quasar = η * ε_rad * M_accrete * c²
 *       If E_quasar > E_bind(ColdGas): eject all cold gas
 *       If E_quasar > E_bind(ColdGas + HotGas): also eject all hot gas
 *
 * Module Communication:
 *   Reads:  UnstableDiskGasFraction, IsMerging, MergerMassRatio, ColdGas,
 *           MetalsColdGas, HotGas, MetalsHotGas, BlackHoleMass
 *   Writes: BlackHoleMass, ColdGas, MetalsColdGas, HotGas, MetalsHotGas,
 *           EjectedGas, MetalsEjectedGas, QuasarModeBHaccretionMass
 *
 * Trigger Sources:
 *   - UnstableDiskGasFraction: Set by sage_disk_instability
 *   - IsMerging + MergerMassRatio: Set by sage_update_merger_time / sage_merge_galaxies
 *
 * References:
 *   - SAGE: model_mergers.c lines 98-148 (grow_black_hole, quasar_mode_wind)
 *   - model_disk_instability.c lines 67-68
 *   - Kauffmann & Haehnelt (2000) - BH growth model
 *
 * Vision Principles:
 *   - Module Independence: Checks trigger properties, acts independently
 *   - Multi-Trigger Pattern: Same physics for disk instability and mergers
 *   - Sequential Physics: BH growth → wind (logical flow)
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"
#include "_system/physical_constants.h"
#include "module_interface.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static int AGN_RECIPE_ON;
static double BLACK_HOLE_GROWTH_RATE;
static double QUASAR_MODE_EFFICIENCY;

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

/* Black hole growth threshold velocity (km/s) */
static const double BH_GROWTH_VEL_THRESHOLD = 280.0;

/* Radiative efficiency for BH accretion (standard Shakura-Sunyaev) */
static const double RADIATIVE_EFFICIENCY = 0.1;

/* Kinetic energy factor for binding energy: E = 0.5 * m * v^2 */
static const double KINETIC_ENERGY_FACTOR = 0.5;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Grow black hole from disk instability or mergers
 *
 * Implements Kauffmann & Haehnelt (2000) BH growth model.
 * Efficiency factor comes from either UnstableDiskGasFraction (disk instability)
 * or MergerMassRatio (mergers).
 *
 * @param halo Galaxy halo pointer
 * @param efficiency_factor Efficiency (disk fraction or merger ratio)
 * @param G_code Gravitational constant in code units
 */
static void grow_black_hole(struct Halo *halo, double efficiency_factor, double G_code)
{
    (void)G_code;  /* Unused in current implementation */

    struct GalaxyData *gal = halo->galaxy;

    if (gal->ColdGas <= 0.0) {
        return;
    }

    /* Calculate BH accretion rate (Kauffmann & Haehnelt 2000) */
    double BHaccrete = BLACK_HOLE_GROWTH_RATE * efficiency_factor /
                       (1.0 + pow(BH_GROWTH_VEL_THRESHOLD / halo->Vvir, 2.0)) *
                       gal->ColdGas;

    /* Limit accretion to available cold gas */
    if (BHaccrete > gal->ColdGas) {
        BHaccrete = gal->ColdGas;
    }

    /* Calculate metallicity of accreted gas */
    float metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    /* Update galaxy properties */
    gal->BlackHoleMass += BHaccrete;
    gal->ColdGas -= BHaccrete;
    gal->MetalsColdGas -= metallicity * BHaccrete;
    gal->QuasarModeBHaccretionMass += BHaccrete;

    DEBUG_LOG("BH accreted %.3e Msun (eff=%.3f), new BH mass=%.3e",
              BHaccrete, efficiency_factor, gal->BlackHoleMass);
}

/**
 * @brief Execute quasar-mode AGN winds
 *
 * Energy-driven gas ejection triggered by BH accretion.
 * If quasar energy exceeds binding energy, eject gas to ejected reservoir.
 *
 * @param halo Galaxy halo pointer
 * @param ctx Module execution context
 */
static void execute_quasar_wind(struct Halo *halo, struct ModuleContext *ctx)
{
    struct GalaxyData *gal = halo->galaxy;

    /* Check trigger: did BH accretion occur? */
    if (gal->QuasarModeBHaccretionMass <= 0.0) {
        return;
    }

    /* Calculate quasar wind energy: E = η * ε_rad * M_accrete * c² */
    double C_over_UnitVel = C_KM_S / ctx->params->UnitVelocity_in_cm_per_s;
    float quasar_energy = QUASAR_MODE_EFFICIENCY * RADIATIVE_EFFICIENCY *
                          gal->QuasarModeBHaccretionMass *
                          pow(C_over_UnitVel, 2.0);

    /* Calculate binding energies: E_bind = 0.5 * M * V_vir² */
    float cold_gas_energy = KINETIC_ENERGY_FACTOR * gal->ColdGas * halo->Vvir * halo->Vvir;
    float hot_gas_energy = KINETIC_ENERGY_FACTOR * gal->HotGas * halo->Vvir * halo->Vvir;

    /* If quasar energy exceeds cold gas binding energy, eject all cold gas */
    if (quasar_energy > cold_gas_energy) {
        gal->EjectedGas += gal->ColdGas;
        gal->MetalsEjectedGas += gal->MetalsColdGas;

        DEBUG_LOG("Quasar wind ejected all cold gas (%.3e Msun)", gal->ColdGas);

        gal->ColdGas = 0.0;
        gal->MetalsColdGas = 0.0;
    }

    /* If quasar energy exceeds combined binding energy, also eject hot gas */
    if (quasar_energy > cold_gas_energy + hot_gas_energy) {
        gal->EjectedGas += gal->HotGas;
        gal->MetalsEjectedGas += gal->MetalsHotGas;

        DEBUG_LOG("Quasar wind ejected all hot gas (%.3e Msun)", gal->HotGas);

        gal->HotGas = 0.0;
        gal->MetalsHotGas = 0.0;
    }
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_quasar_mode_init(void)
{
    LOAD_AND_VALIDATE_OPTION("AGNrecipeOn", AGN_RECIPE_ON, 1, "0=disabled, 1=enabled");

    if (AGN_RECIPE_ON) {
        LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BlackHoleGrowthRate", BLACK_HOLE_GROWTH_RATE,
                                           0.0, 1.0, "BH growth rate");
        LOAD_AND_VALIDATE_RANGE_INCLUSIVE("QuasarModeEfficiency", QUASAR_MODE_EFFICIENCY,
                                           0.0, 1.0, "quasar mode efficiency");

        INFO_LOG("SAGE quasar-mode AGN feedback module initialized");
        VERBOSE_LOG("  AGNrecipeOn = 1 (enabled)");
        VERBOSE_LOG("  BlackHoleGrowthRate = %.4f", BLACK_HOLE_GROWTH_RATE);
        VERBOSE_LOG("  QuasarModeEfficiency = %.3f", QUASAR_MODE_EFFICIENCY);
        VERBOSE_LOG("  RadiativeEfficiency = %.2f (fixed)", RADIATIVE_EFFICIENCY);
    } else {
        INFO_LOG("SAGE quasar-mode AGN feedback module initialized");
        VERBOSE_LOG("  AGNrecipeOn = 0 (disabled)");
    }

    return 0;
}

int sage_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    /* Skip if AGN is disabled */
    if (!AGN_RECIPE_ON) {
        return 0;
    }

    /* Verify process_by_galaxy mode */
    if (ngal != 1) {
        ERROR_LOG("sage_quasar_mode expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) {
        return 0;
    }

    /* Check trigger conditions - module is independent, just checks properties */
    double efficiency_factor = 0.0;

    if (gal->UnstableDiskGasFraction > 0.0) {
        /* Disk instability triggered BH growth */
        efficiency_factor = gal->UnstableDiskGasFraction;
        DEBUG_LOG("Quasar mode from disk instability (eff=%.3f)", efficiency_factor);
    }
    else if (gal->IsMerging && gal->MergerMassRatio > 0.0) {
        /* Merger triggered BH growth */
        efficiency_factor = gal->MergerMassRatio;
        DEBUG_LOG("Quasar mode from merger (ratio=%.3f)", efficiency_factor);
    }
    else {
        return 0;  /* No trigger */
    }

    /* Step 1: Grow black hole */
    grow_black_hole(halo, efficiency_factor, ctx->params->G);

    /* Step 2: Execute quasar wind (if BH accretion occurred) */
    execute_quasar_wind(halo, ctx);

    return 0;
}

int sage_quasar_mode_cleanup(void)
{
    VERBOSE_LOG("SAGE quasar-mode AGN feedback module cleaned up");
    return 0;
}
