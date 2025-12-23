/**
 * @file    sage_disk_instability.c
 * @brief   SAGE disk instability - stability check and stellar mass transfer to bulge
 *
 * Implements the Mo, Mao & White (1998) disk stability criterion. Unstable stellar
 * mass is transferred to the bulge. Unstable gas triggers downstream physics modules.
 *
 * Physics:
 *   Calculate disk mass and critical mass, then transfer unstable stars to bulge:
 *     diskmass = ColdGas + (StellarMass - BulgeMass)
 *     Mcrit = Vmax^2 * (3 * DiskScaleRadius) / G
 *     unstable_stars = star_fraction * (diskmass - Mcrit)
 *     BulgeMass += unstable_stars (with metallicity preservation)
 *
 * Trigger Flag:
 *   Sets UnstableDiskGasFraction for downstream modules:
 *     - sage_quasar_mode: BH growth + quasar winds from unstable gas
 *     - sage_collisional_starburst: Starburst + SN feedback from unstable gas
 *
 * References:
 *   - SAGE: model_disk_instability.c (lines 20-54)
 *   - Mo, Mao & White (1998) - Disk stability criterion
 */

#include "error.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"
#include "module_interface.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double STAR_FORMING_DISK_FACTOR;

// ============================================================================
// MODULE LIFECYCLE
// ============================================================================

int sage_disk_instability_init(void)
{
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR,
                                       0.0, 10.0, "star forming disk factor");

    INFO_LOG("SAGE disk instability module initialized");
    VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);
    return 0;
}

int sage_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    // Verify process_by_galaxy mode
    if (ngal != 1) {
        ERROR_LOG("sage_disk_instability expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) {
        return 0;
    }

    // Calculate total disk mass (Mo, Mao & White 1998)
    const double diskmass = gal->ColdGas + (gal->StellarMass - gal->BulgeMass);

    if (diskmass > 0.0) {
        // Critical disk mass for stability (Efstathiou et al. 1982)
        // STAR_FORMING_DISK_FACTOR typically 3.0 (stellar disk scale radius relation)
        double Mcrit = halo->Vmax * halo->Vmax * (STAR_FORMING_DISK_FACTOR * gal->DiskScaleRadius) / ctx->params->G;
        if (Mcrit > diskmass) {
            Mcrit = diskmass;
        }

        // Calculate mass fractions and unstable masses
        const double gas_fraction = gal->ColdGas / diskmass;
        const double unstable_gas = gas_fraction * (diskmass - Mcrit);
        const double star_fraction = 1.0 - gas_fraction;
        const double unstable_stars = star_fraction * (diskmass - Mcrit);
        
        // Transfer unstable stars to bulge
        if (unstable_stars > 0.0) {
            // Use disk metallicity (exclude existing bulge)
            const double disk_stellar_mass = gal->StellarMass - gal->BulgeMass;
            const double disk_metal_mass = gal->MetalsStellarMass - gal->MetalsBulgeMass;
            const double metallicity = mimic_get_metallicity(disk_stellar_mass, disk_metal_mass);

            gal->BulgeMass += unstable_stars;
            gal->MetalsBulgeMass += metallicity * unstable_stars;

            // Enforce physical constraints (Mimic safety check)
            if (gal->BulgeMass > gal->StellarMass) {
                WARNING_LOG("Disk instability: Bulge mass %.4e exceeds stellar mass %.4e in halo %d",
                           gal->BulgeMass, gal->StellarMass, halo->HaloNr);
                gal->BulgeMass = gal->StellarMass;
            }

            if (gal->MetalsBulgeMass > gal->MetalsStellarMass) {
                WARNING_LOG("Disk instability: Bulge metals %.4e exceed stellar metals %.4e in halo %d",
                           gal->MetalsBulgeMass, gal->MetalsStellarMass, halo->HaloNr);
                gal->MetalsBulgeMass = gal->MetalsStellarMass;
            }

            DEBUG_LOG("Halo %d: Disk unstable - transferred %.3e Msun to bulge",
                      halo->HaloNr, unstable_stars);
        }

        // Set trigger flag for downstream modules (sage_quasar_mode, sage_collisional_starburst)
        if (unstable_gas > 0.0) {
            const double unstable_gas_fraction = unstable_gas / gal->ColdGas;
            gal->UnstableDiskGasFraction = unstable_gas_fraction;

            DEBUG_LOG("Halo %d: Unstable gas fraction = %.4f",
                      halo->HaloNr, unstable_gas_fraction);
        } else {
            gal->UnstableDiskGasFraction = 0.0;
        }
    } else {
        // Stable disk (no mass or negative mass)
        gal->UnstableDiskGasFraction = 0.0;
    }

    return 0;
}

int sage_disk_instability_cleanup(void)
{
    VERBOSE_LOG("SAGE disk instability module cleaned up");
    return 0;
}
