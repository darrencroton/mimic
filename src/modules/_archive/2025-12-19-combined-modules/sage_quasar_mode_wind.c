/**
 * @file    sage_quasar_mode_wind.c
 * @brief   Powerful AGN winds from quasar-mode BH accretion
 *
 * Energy-driven gas ejection triggered by BH accretion.
 * Checks QuasarModeBHaccretionMass (set by sage_grow_black_hole).
 * Doesn't care what triggered BH growth - just responds to accretion.
 *
 * References:
 *   - SAGE: model_mergers.c lines 124-148 (quasar_mode_wind)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "../modules/_system/parameter_helpers.h"
#include "../modules/_system/physical_constants.h"
#include "module_interface.h"
#include "types.h"

static double QUASAR_MODE_EFFICIENCY;
static double RADIATIVE_EFFICIENCY;  /* 0.1 in SAGE */
static double KINETIC_ENERGY_FACTOR;  /* 0.5 for E = 0.5 * m * v^2 */

/* Speed of light in km/s */
static const double SPEED_OF_LIGHT_KM_S = 2.99792458e5;

int sage_quasar_mode_wind_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("QuasarModeEfficiency", QUASAR_MODE_EFFICIENCY,
                                       0.0, 1.0, "quasar mode efficiency");
    RADIATIVE_EFFICIENCY = 0.1;  /* Standard radiative efficiency (SAGE line 127) */
    KINETIC_ENERGY_FACTOR = 0.5;  /* For kinetic energy calculation (SAGE line 128-129) */

    VERBOSE_LOG("SAGE Quasar Mode Wind initialized");
    VERBOSE_LOG("  QuasarModeEfficiency = %.3f", QUASAR_MODE_EFFICIENCY);
    VERBOSE_LOG("  RadiativeEfficiency = %.2f", RADIATIVE_EFFICIENCY);
    return 0;
}

int sage_quasar_mode_wind_cleanup(void)
{
    return 0;
}

/**
 * @brief   Quasar-mode AGN winds eject gas based on BH accretion energy
 *
 * SAGE reference: lines 126-148
 */
int sage_quasar_mode_wind_process(struct ModuleContext *ctx,
                                   struct Halo *halos,
                                   int ngal)
{
    if (ngal != 1) {
        ERROR_LOG("sage_quasar_mode_wind expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) return 0;

    /* Check trigger: did BH accretion occur? */
    if (gal->QuasarModeBHaccretionMass <= 0.0) {
        return 0;  /* No BH accretion, no wind */
    }

    /* Calculate quasar wind energy: E = η * ε_rad * M_accrete * c² (SAGE line 127)
     * Note: BH accretion is stored, not passed as parameter */
    double C_over_UnitVel = SPEED_OF_LIGHT_KM_S / ctx->params->UnitVelocity_in_cm_per_s;
    float quasar_energy = QUASAR_MODE_EFFICIENCY * RADIATIVE_EFFICIENCY *
                          gal->QuasarModeBHaccretionMass *
                          pow(C_over_UnitVel, 2.0);

    /* Calculate binding energies: E_bind = 0.5 * M * V_vir² (SAGE lines 128-129) */
    float cold_gas_energy = KINETIC_ENERGY_FACTOR * gal->ColdGas * halo->Vvir * halo->Vvir;
    float hot_gas_energy = KINETIC_ENERGY_FACTOR * gal->HotGas * halo->Vvir * halo->Vvir;

    /* If quasar energy exceeds cold gas binding energy, eject all cold gas (SAGE lines 132-138) */
    if (quasar_energy > cold_gas_energy) {
        gal->EjectedGas += gal->ColdGas;
        gal->MetalsEjectedGas += gal->MetalsColdGas;

        DEBUG_LOG("Quasar wind ejected all cold gas (%.3e Msun)", gal->ColdGas);

        gal->ColdGas = 0.0;
        gal->MetalsColdGas = 0.0;
    }

    /* If quasar energy exceeds combined binding energy, also eject hot gas (SAGE lines 141-147) */
    if (quasar_energy > cold_gas_energy + hot_gas_energy) {
        gal->EjectedGas += gal->HotGas;
        gal->MetalsEjectedGas += gal->MetalsHotGas;

        DEBUG_LOG("Quasar wind ejected all hot gas (%.3e Msun)", gal->HotGas);

        gal->HotGas = 0.0;
        gal->MetalsHotGas = 0.0;
    }

    return 0;
}
