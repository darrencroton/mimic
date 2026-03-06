/**
 * @file sage_quasar_mode.c
 * @brief SAGE quasar-mode AGN feedback (BH growth + energy-driven winds)
 *
 * Implements Kauffmann & Haehnelt (2000) black hole growth coupled with
 * energy-driven gas ejection. Triggered by disk instability OR mergers.
 *
 * References:
 *   - SAGE: model_mergers.c (grow_black_hole, quasar_mode_wind)
 *   - SAGE: model_disk_instability.c (grow_black_hole call)
 *   - Kauffmann & Haehnelt (2000) - BH growth model
 *   - Croton et al. (2006, 2016) - SAGE model papers
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"
#include "_system/physical_constants.h"
#include "module_interface.h"
#include "types.h"

// Module parameters
static double BLACK_HOLE_GROWTH_RATE;
static double QUASAR_MODE_EFFICIENCY;

// Inline helper for squaring (avoids pow overhead)
#define SQR(x) ((x) * (x))

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * Grow black hole from disk instability or mergers (Kauffmann & Haehnelt 2000)
 *
 * @param halo Galaxy halo
 * @param efficiency_factor Disk gas fraction or merger mass ratio
 * @return Black hole accretion mass this event (1e10 Msun/h)
 */
static double grow_black_hole(struct Halo *halo, double efficiency_factor)
{
    struct GalaxyData *gal = halo->galaxy;

    if (gal->ColdGas <= 0.0) {
        return 0.0;
    }

    // BH accretion formula (Kauffmann & Haehnelt 2000)
    // Suppressed in low-mass halos (V_vir < 280 km/s)
    const double BHaccrete = BLACK_HOLE_GROWTH_RATE * efficiency_factor /
                             (1.0 + SQR(280.0 / halo->Vvir)) *  // 280 km/s threshold
                             gal->ColdGas;

    // Cannot accrete more gas than available
    const double accrete = (BHaccrete > gal->ColdGas) ? gal->ColdGas : BHaccrete;

    const float metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

    // Update galaxy properties
    gal->BlackHoleMass += accrete;
    gal->ColdGas -= accrete;
    gal->MetalsColdGas -= metallicity * accrete;
    gal->QuasarModeBHaccretionMass += accrete;  // Track total this snapshot

    DEBUG_LOG("BH accreted %.3e (eff=%.3f), new M_BH=%.3e",
              accrete, efficiency_factor, gal->BlackHoleMass);

    return accrete;
}

/**
 * Execute quasar-mode wind (energy-driven gas ejection)
 *
 * If quasar energy exceeds binding energy, eject gas to ejected reservoir.
 *
 * @param halo Galaxy halo
 * @param BHaccrete Black hole accretion mass this event (1e10 Msun/h)
 * @param ctx Module context (for unit conversions)
 */
static void quasar_mode_wind(struct Halo *halo, double BHaccrete, const struct ModuleContext *ctx)
{
    struct GalaxyData *gal = halo->galaxy;

    // Quasar wind energy: E = η * ε_rad * M_BH * c²
    // ε_rad = 0.1 (standard Shakura-Sunyaev radiative efficiency)
    const double c_over_unit_vel = C_KM_S / ctx->params->UnitVelocity_in_cm_per_s;
    const double quasar_energy = QUASAR_MODE_EFFICIENCY * 0.1 * BHaccrete *
                                 c_over_unit_vel * c_over_unit_vel;

    // Binding energies: E_bind = 0.5 * M * V_vir²
    const double cold_gas_energy = 0.5 * gal->ColdGas * SQR(halo->Vvir);
    const double hot_gas_energy = 0.5 * gal->HotGas * SQR(halo->Vvir);

    // Eject cold gas if quasar energy exceeds binding energy
    if (quasar_energy > cold_gas_energy) {
        gal->EjectedGas += gal->ColdGas;
        gal->MetalsEjectedGas += gal->MetalsColdGas;

        DEBUG_LOG("Quasar wind ejected all cold gas (%.3e)", gal->ColdGas);

        gal->ColdGas = 0.0;
        gal->MetalsColdGas = 0.0;
    }

    // Also eject hot gas if quasar energy exceeds combined binding energy
    if (quasar_energy > cold_gas_energy + hot_gas_energy) {
        gal->EjectedGas += gal->HotGas;
        gal->MetalsEjectedGas += gal->MetalsHotGas;

        DEBUG_LOG("Quasar wind ejected all hot gas (%.3e)", gal->HotGas);

        gal->HotGas = 0.0;
        gal->MetalsHotGas = 0.0;
    }
}

// ============================================================================
// MODULE LIFECYCLE
// ============================================================================

int sage_quasar_mode_init(void)
{
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BlackHoleGrowthRate", BLACK_HOLE_GROWTH_RATE,
                                        0.0, 1.0, "BH growth rate");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("QuasarModeEfficiency", QUASAR_MODE_EFFICIENCY,
                                        0.0, 1.0, "quasar mode efficiency");

    INFO_LOG("SAGE quasar-mode AGN feedback initialized");
    VERBOSE_LOG("  BlackHoleGrowthRate = %.4f", BLACK_HOLE_GROWTH_RATE);
    VERBOSE_LOG("  QuasarModeEfficiency = %.3f", QUASAR_MODE_EFFICIENCY);

    return 0;
}

int sage_quasar_mode_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    if (ngal != 1) {
        ERROR_LOG("sage_quasar_mode expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) {
        return 0;
    }

    // Process disk instability trigger (if present)
    if (gal->UnstableDiskGasFraction > 0.0) {
        DEBUG_LOG("Quasar mode from disk instability (eff=%.3f)", gal->UnstableDiskGasFraction);
        const double BHaccrete = grow_black_hole(halo, gal->UnstableDiskGasFraction);
        if (BHaccrete > 0.0) {
            quasar_mode_wind(halo, BHaccrete, ctx);
        }
    }

    // Process merger trigger (if present)
    if (gal->IsMerging && gal->MergerMassRatio > 0.0) {
        DEBUG_LOG("Quasar mode from merger (ratio=%.3f)", gal->MergerMassRatio);
        const double BHaccrete = grow_black_hole(halo, gal->MergerMassRatio);
        if (BHaccrete > 0.0) {
            quasar_mode_wind(halo, BHaccrete, ctx);
        }
    }

    /* Trigger lifecycle is managed by dedicated clear modules in pipeline config. */
    return 0;
}

int sage_quasar_mode_cleanup(void)
{
    VERBOSE_LOG("SAGE quasar-mode AGN feedback cleaned up");
    return 0;
}
