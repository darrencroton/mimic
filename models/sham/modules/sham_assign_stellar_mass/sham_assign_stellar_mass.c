/**
 * @file    sham_assign_stellar_mass.c
 * @brief   Assign stellar masses from a monotonic SHAM-style halo proxy
 *
 * This module tracks each galaxy branch's peak halo mass and peak circular
 * velocity, then assigns stellar mass from a double-power-law stellar-to-halo
 * mass relation with deterministic log-normal scatter. It is designed as a
 * Mimic-native SHAM implementation that works inside the existing FoF-workspace
 * module contract.
 *
 * References: Conroy et al. (2006), Vale & Ostriker (2006), Reddick et al.
 * (2013), Moster et al. (2013), Guo & White (2014).
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

#include "module_system/parameter_helpers.h"
#include "module_system/physical_constants.h"

#define TWO_PI 6.28318530717958647692

static double sham_log_m1;
static double sham_n;
static double sham_beta;
static double sham_gamma;
static double sham_scatter_dex;
static double sham_min_mpeak;
static double sham_min_vpeak;
static double sham_max_stellar_baryon_fraction;
static double sham_orphan_max_age_myr;
static int sham_use_scatter;

static double max_double(double a, double b) { return (a > b) ? a : b; }

static uint64_t splitmix64(uint64_t x) {
  x += UINT64_C(0x9e3779b97f4a7c15);
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}

static double uniform_from_key(uint64_t key, uint64_t salt) {
  const uint64_t bits = splitmix64(key + salt);
  const double u = (double)(bits >> 11) * 0x1.0p-53;
  if (u <= 0.0) {
    return 0x1.0p-53;
  }
  if (u >= 1.0) {
    return 1.0 - 0x1.0p-53;
  }
  return u;
}

static double gaussian_from_key(uint64_t key) {
  const double u1 = uniform_from_key(key, UINT64_C(0x4d595df4d0f33173));
  const double u2 = uniform_from_key(key, UINT64_C(0x3793fdff2d7a2d43));
  return sqrt(-2.0 * log(u1)) * cos(TWO_PI * u2);
}

static uint64_t galaxy_key(const struct Halo *halo) {
  if (halo->UniqueGalaxyID != 0) {
    return (uint64_t)halo->UniqueGalaxyID;
  }
  return ((uint64_t)(uint32_t)FileNum << 48) ^
         ((uint64_t)(uint32_t)TreeID << 24) ^
         (uint64_t)(uint32_t)halo->HaloNr ^
         (uint64_t)halo->MostBoundID;
}

static double moster_stellar_mass_msun(double halo_mass_msun) {
  if (halo_mass_msun <= 0.0 || !isfinite(halo_mass_msun)) {
    return 0.0;
  }

  const double m1 = pow(10.0, sham_log_m1);
  const double ratio = halo_mass_msun / m1;
  const double denominator = pow(ratio, -sham_beta) + pow(ratio, sham_gamma);

  if (denominator <= 0.0 || !isfinite(denominator)) {
    return 0.0;
  }

  return 2.0 * sham_n * halo_mass_msun / denominator;
}

static void update_peak_proxies(struct Halo *halo) {
  struct GalaxyData *gal = halo->galaxy;

  if (halo->Type == 0 || halo->Type == 1) {
    gal->ShamMpeak = (float)max_double(gal->ShamMpeak, halo->Mvir);
    gal->ShamVpeak = (float)max_double(gal->ShamVpeak, halo->Vmax);
    gal->ShamOrphanAge = 0.0f;
  } else if (halo->Type == 2 && halo->dT > 0.0f) {
    const double dt_myr = halo->dT * UnitTime_in_s / SEC_PER_MEGAYEAR;
    if (isfinite(dt_myr) && dt_myr > 0.0) {
      gal->ShamOrphanAge += (float)dt_myr;
    }
  }
}

static int orphan_exceeds_lifetime(const struct Halo *halo) {
  if (halo->Type != 2 || sham_orphan_max_age_myr <= 0.0) {
    return 0;
  }
  return halo->galaxy->ShamOrphanAge > sham_orphan_max_age_myr;
}

static void clear_assigned_galaxy_properties(struct GalaxyData *gal) {
  gal->StellarMass = 0.0f;
  gal->BulgeMass = 0.0f;
  gal->MetalsStellarMass = 0.0f;
  gal->MetalsBulgeMass = 0.0f;
  gal->StarFormationRate = 0.0f;
  gal->ShamStellarMassNoScatter = 0.0f;
  gal->ShamScatterDex = 0.0f;
}

static void assign_stellar_mass(struct Halo *halo) {
  struct GalaxyData *gal = halo->galaxy;
  const double mpeak = gal->ShamMpeak;
  const double vpeak = gal->ShamVpeak;

  clear_assigned_galaxy_properties(gal);

  if (mpeak < sham_min_mpeak || vpeak < sham_min_vpeak) {
    return;
  }

  const double h = (MimicConfig.Hubble_h > 0.0) ? MimicConfig.Hubble_h : 1.0;
  const double halo_mass_msun = mpeak * 1.0e10 / h;
  const double unscattered_mstar_msun = moster_stellar_mass_msun(halo_mass_msun);
  double mstar_code = unscattered_mstar_msun * h / 1.0e10;
  double scatter = 0.0;

  gal->ShamStellarMassNoScatter = (float)mstar_code;

  if (sham_use_scatter && sham_scatter_dex > 0.0) {
    scatter = sham_scatter_dex * gaussian_from_key(galaxy_key(halo));
    mstar_code *= pow(10.0, scatter);
  }

  const double baryon_cap = sham_max_stellar_baryon_fraction * mpeak;
  if (baryon_cap > 0.0 && mstar_code > baryon_cap) {
    mstar_code = baryon_cap;
  }

  if (!isfinite(mstar_code) || mstar_code < 0.0) {
    mstar_code = 0.0;
  }

  gal->StellarMass = (float)mstar_code;
  gal->ShamScatterDex = (float)scatter;
}

int sham_assign_stellar_mass_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ShamLogM1", sham_log_m1, 8.0, 15.0,
                                    "log10 pivot halo mass in Msun");
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("ShamN", sham_n, 0.0, 1.0,
                                    "stellar-to-halo mass normalization");
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("ShamBeta", sham_beta, 0.0, 10.0,
                                    "low-mass slope");
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("ShamGamma", sham_gamma, 0.0, 10.0,
                                    "high-mass slope");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ShamScatterDex", sham_scatter_dex, 0.0,
                                    2.0, "stellar mass scatter");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ShamMinMpeak", sham_min_mpeak, 0.0,
                                    1.0e8, "minimum Mpeak in code units");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ShamMinVpeak", sham_min_vpeak, 0.0,
                                    5000.0, "minimum Vpeak in km/s");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ShamMaxStellarBaryonFraction",
                                    sham_max_stellar_baryon_fraction, 0.0, 1.0,
                                    "cap as fraction of Mpeak");
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ShamOrphanMaxAgeMyr",
                                    sham_orphan_max_age_myr, 0.0, 20000.0,
                                    "0 disables orphan age removal");
  LOAD_AND_VALIDATE_OPTION("ShamUseScatter", sham_use_scatter, 1,
                           "0=off, 1=deterministic log-normal scatter");

  INFO_LOG("SHAM stellar mass assignment initialized");
  VERBOSE_LOG("  SHMR: logM1=%.3f N=%.4f beta=%.3f gamma=%.3f scatter=%.3f dex",
              sham_log_m1, sham_n, sham_beta, sham_gamma, sham_scatter_dex);
  return 0;
}

int sham_assign_stellar_mass_process(struct ModuleContext *ctx,
                                     struct Halo *halos, int ngal) {
  (void)ctx;

  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL || halos[i].Type == 3) {
      continue;
    }

    update_peak_proxies(&halos[i]);
    if (orphan_exceeds_lifetime(&halos[i])) {
      DEBUG_LOG("SHAM removing orphan galaxy %lld after %.1f Myr",
                halos[i].UniqueGalaxyID, halos[i].galaxy->ShamOrphanAge);
      halos[i].Type = 3;
      continue;
    }

    assign_stellar_mass(&halos[i]);
  }

  return 0;
}

int sham_assign_stellar_mass_cleanup(void) { return 0; }

