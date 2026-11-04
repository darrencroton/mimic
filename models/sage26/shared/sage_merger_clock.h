/**
 * @file sage_merger_clock.h
 * @brief SAGE26's dynamical-friction merger clock, estimate_merging_time
 *
 * SAGE26 estimates a satellite's merger clock when progenitor joining makes it a
 * Type 1 from a Type 0, or finds a Type 1 whose clock is above 999
 * (R/src/core_build_model.c:315-318, R = the staged SAGE26 source), with
 * estimate_merging_time (R/src/model_mergers.c:89-121):
 *
 *   same halo                     -> -1                                     (:94-98)
 *   coulomb   = log1p(mother Len / (double) satellite Len)                  (:100)
 *   M_sat     = get_virial_mass(sat_halo) + StellarMass + ColdGas           (:102)
 *   R         = get_virial_radius(mother_halo)                              (:103)
 *   t         = MergerTimeFactor * 1.17 * R * R * get_virial_velocity(mother_halo)
 *               / (coulomb * G * M_sat)   if M_sat > 0, coulomb > 0 and satellite Len >= 10
 *             = -1                        otherwise                         (:105-110)
 *   t >= 999  -> 998                                                        (:112-117)
 *
 * The core does not keep SAGE26's raw halo catalogue, so the package computes the
 * halo helpers' values from what the core exposes about the current snapshot's
 * halos (a core-boundary item recorded by the node's validation):
 *
 * - get_virial_mass(sat_halo) is the satellite's struct Halo Mvir. At progenitor
 *   joining the core sets it to get_virial_mass of the descendant halo
 *   (src/core/inheritance.c:64, populate_halo_payload.inc), with SAGE26's rule
 *   (src/core/virial.c:47-54): the catalogue mass for a FoF's first halo when it is
 *   not negative, Len * PartMass otherwise.
 * - get_virial_radius and get_virial_velocity of the mother halo, the FoF's first
 *   halo, are the core's virial_radius_for_mass and virial_velocity_for (virial.h)
 *   of the mother's struct Halo Mvir (that of the FoF's Type 0 galaxy row, set the
 *   same way) at the current snapshot's redshift. They are SAGE26's expressions
 *   (R/src/model_halo_properties.c:40-50, :63-85) in the same operand order, so
 *   clang's contraction fuses the hubble_of_z_sq polynomial at the same site in
 *   both (the oracle README's fused-instruction table); the stored Rvir and Vvir
 *   of the galaxy rows are not used, since the core freezes them while a halo
 *   loses mass (src/core/inheritance.c:60-63).
 *
 * The final expression is one C expression with SAGE26's operand order and types:
 * no multiply-add is contractible in it (estimate_merging_time has no fused
 * instruction in the reference objects).
 */

#ifndef MIMIC_SHARED_SAGE_MERGER_CLOCK_H
#define MIMIC_SHARED_SAGE_MERGER_CLOCK_H

#include <math.h>
#include <stddef.h>

#include "types.h"
#include "virial.h"
#include "shared/sage_constants.h"

/* SAGE parity: MinNumPartSatHalo, the particle floor of estimate_merging_time
 * (R/src/model_mergers.c:92) */
#define MIMIC_SAGE_MIN_NUM_PART_SAT_HALO 10

/* SAGE parity: estimate_merging_time caps computed clocks at or above 999 to
 * SAGE_MERGTIME_CEILING, 998 (R/src/model_mergers.c:112-117); a same-halo call or a
 * failed guard returns SAGE_MERGTIME_IMMEDIATE, -1 (R/src/model_mergers.c:97, :109) */
#define MIMIC_SAGE_MERGTIME_CAP_AT 999.0

/**
 * @brief SAGE26's locals of one estimate_merging_time call, for the oracle test
 *
 * A flag is -1 where the call returns before evaluating it; a value's absent
 * marker is 1 where the call never forms it (everything after an early return),
 * and such a value holds NaN.
 */
struct MimicSageMergerClockTrace {
  int same_halo;           /* 1 when the satellite and mother halos coincide (:94) */
  int guard_passed;        /* the mass, Coulomb-logarithm and particle-count guard (:105) */
  int capped;              /* the >= 999 cap applied (:112) */
  int values_absent;       /* 1 after the same-halo return: none of the values below is formed */
  double sat_virial_mass;  /* get_virial_mass(sat_halo) */
  double coulomb;          /* :100 */
  double satellite_mass;   /* :102, SatelliteMass */
  double satellite_radius; /* :103, SatelliteRadius (the mother's virial radius) */
  int mother_virial_velocity_absent; /* 1 unless the guard passed (:106-107 forms it) */
  double mother_virial_velocity;     /* get_virial_velocity(mother_halo) */
  double mergtime_pre_cap;           /* :106-107 or :109, before the cap */
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

  // SAGE parity: the same-halo error return (R/src/model_mergers.c:94-98); the
  // package compares the halo numbers its rows carry for SAGE26's halo indices
  if (satellite->HaloNr == mother->HaloNr) {
    t->same_halo = 1;
    return SAGE_MERGTIME_IMMEDIATE;
  }

  // SAGE parity: the Coulomb logarithm is formed before the guards
  // (R/src/model_mergers.c:100). A zero satellite Len gives +inf (or NaN with a
  // zero mother Len) without a trap, and the Len >= 10 guard then returns -1, so
  // SAGE26 needs no zero-Len guard and the package has none
  const double coulomb = log1p(mother->Len / ((double)satellite->Len));

  const double sat_virial_mass = satellite->Mvir;
  // SAGE parity: double + widened float + widened float, left to right
  // (R/src/model_mergers.c:102)
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
    // the clock is one expression in SAGE26's operand order (R/src/model_mergers.c:106-107)
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
