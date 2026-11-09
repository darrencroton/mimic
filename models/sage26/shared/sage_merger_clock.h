/**
 * @file sage_merger_clock.h
 * @brief SAGE26's dynamical-friction merger clock, estimate_merging_time
 *
 * Implements SAGE26's estimate_merging_time, called when progenitor joining makes a
 * Type 0 into a Type 1, or finds a Type 1 whose clock is above 999:
 *
 *   same halo -> -1
 *   coulomb   = log1p(mother Len / (double) satellite Len)
 *   M_sat     = get_virial_mass(sat_halo) + StellarMass + ColdGas
 *   R         = get_virial_radius(mother_halo)
 *   t         = MergerTimeFactor * 1.17 * R * R * get_virial_velocity(mother_halo)
 *               / (coulomb * G * M_sat)   if M_sat > 0, coulomb > 0 and satellite Len >= 10
 *             = -1                        otherwise
 *   t >= 999  -> 998
 *
 * get_virial_mass(sat_halo) is the satellite's struct Halo Mvir, which the core sets at
 * progenitor joining with SAGE26's rule (the catalogue mass for a FoF's first halo when
 * it is not negative, Len * PartMass otherwise). The mother's virial radius and velocity
 * are recomputed from its Mvir at the current redshift with the core's
 * virial_radius_for_mass and virial_velocity_for (virial.h); the stored Rvir and Vvir
 * are not used, since the core freezes them while a halo loses mass.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_MERGER_CLOCK_H
#define MIMIC_SHARED_SAGE_MERGER_CLOCK_H

#include <math.h>
#include <stddef.h>

#include "types.h"
#include "virial.h"
#include "shared/sage_constants.h"

/* SAGE parity: MinNumPartSatHalo, the particle floor of estimate_merging_time */
#define MIMIC_SAGE_MIN_NUM_PART_SAT_HALO 10

/* SAGE parity: clocks at or above 999 are capped to SAGE_MERGTIME_CEILING (998); a
 * same-halo call or a failed guard returns SAGE_MERGTIME_IMMEDIATE (-1) */
#define MIMIC_SAGE_MERGTIME_CAP_AT 999.0

/**
 * @brief SAGE26's locals of one estimate_merging_time call, reported for tests
 *
 * A flag is -1 where the call returns before evaluating it; a value's absent
 * marker is 1 where the call never forms it (everything after an early return),
 * and such a value holds NaN.
 */
struct MimicSageMergerClockTrace {
  int same_halo;           /* 1 when the satellite and mother halos coincide */
  int guard_passed;        /* the mass, Coulomb-logarithm and particle-count guard */
  int capped;              /* the >= 999 cap applied */
  int values_absent;       /* 1 after the same-halo return: none of the values below is formed */
  double sat_virial_mass;  /* get_virial_mass(sat_halo) */
  double coulomb;          /* the Coulomb logarithm */
  double satellite_mass;   /* SatelliteMass */
  double satellite_radius; /* SatelliteRadius (the mother's virial radius) */
  int mother_virial_velocity_absent; /* 1 unless the guard passed (only then is it formed) */
  double mother_virial_velocity;     /* get_virial_velocity(mother_halo) */
  double mergtime_pre_cap;           /* the clock before the cap */
};

/**
 * @brief SAGE26's estimate_merging_time for a satellite and its FoF's first halo
 *
 * @param satellite          The satellite's row: HaloNr, Len, Mvir (the core's virial mass
 *                           of its current halo) and its galaxy's StellarMass and ColdGas
 * @param mother             The FoF central's row: HaloNr, Len and Mvir (the core's
 *                           virial mass of the FoF's first halo)
 * @param redshift           Redshift of the current snapshot (SAGE26's ZZ[mother SnapNum])
 * @param merger_time_factor SAGE26's MergerTimeFactor
 * @param G                  Gravitational constant in code units
 * @param trace              Optional: receives SAGE26's locals (NULL to skip)
 * @return The clock in code time units, or -1 (same halo, failed guard)
 */
static inline double
mimic_sage_estimate_merging_time(const struct Halo *satellite, const struct Halo *mother,
                                 const double redshift, const double merger_time_factor,
                                 const double G, struct MimicSageMergerClockTrace *trace) {
  struct MimicSageMergerClockTrace local;
  struct MimicSageMergerClockTrace *t = (trace != NULL) ? trace : &local;
  t->same_halo = 0;
  t->guard_passed = -1;
  t->capped = -1;
  t->values_absent = 1;
  t->mother_virial_velocity_absent = 1;
  /* Values the call never forms stay NaN, so a trace is never read uninitialised */
  t->sat_virial_mass = NAN;
  t->coulomb = NAN;
  t->satellite_mass = NAN;
  t->satellite_radius = NAN;
  t->mother_virial_velocity = NAN;
  t->mergtime_pre_cap = NAN;

  // SAGE parity: SAGE26's same-halo error return, comparing the rows' halo numbers
  if (satellite->HaloNr == mother->HaloNr) {
    t->same_halo = 1;
    return SAGE_MERGTIME_IMMEDIATE;
  }

  // SAGE parity: the Coulomb logarithm is formed before the guards; a zero satellite Len
  // gives +inf or NaN without a trap, and the Len >= 10 guard then returns -1
  const double coulomb = log1p(mother->Len / ((double)satellite->Len));

  const double sat_virial_mass = satellite->Mvir;
  // SAGE parity: double + widened float + widened float, left to right
  const double SatelliteMass =
      sat_virial_mass + satellite->galaxy->StellarMass + satellite->galaxy->ColdGas;
  const double SatelliteRadius = virial_radius_for_mass(mother->Mvir, redshift);

  t->values_absent = 0;
  t->sat_virial_mass = sat_virial_mass;
  t->coulomb = coulomb;
  t->satellite_mass = SatelliteMass;
  t->satellite_radius = SatelliteRadius;

  double mergtime;
  if (SatelliteMass > 0.0 && coulomb > 0.0 && satellite->Len >= MIMIC_SAGE_MIN_NUM_PART_SAT_HALO) {
    // SAGE parity: the mother's virial velocity is formed inside the guarded branch, and
    // the clock is one expression in SAGE26's operand order
    const double mother_virial_velocity = virial_velocity_for(mother->Mvir, SatelliteRadius);
    mergtime = merger_time_factor * 1.17 * SatelliteRadius * SatelliteRadius *
               mother_virial_velocity / (coulomb * G * SatelliteMass);
    t->guard_passed = 1;
    t->mother_virial_velocity_absent = 0;
    t->mother_virial_velocity = mother_virial_velocity;
  } else {
    mergtime = SAGE_MERGTIME_IMMEDIATE;
    t->guard_passed = 0;
  }
  t->mergtime_pre_cap = mergtime;

  t->capped = 0;
  if (mergtime >= MIMIC_SAGE_MERGTIME_CAP_AT) {
    mergtime = SAGE_MERGTIME_CEILING;
    t->capped = 1;
  }

  return mergtime;
}

#endif /* MIMIC_SHARED_SAGE_MERGER_CLOCK_H */
