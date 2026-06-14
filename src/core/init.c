/**
 * @file    init.c
 * @brief   Initialization functions for the Mimic framework
 *
 * This file contains functions responsible for initializing the Mimic
 * framework. It handles defining physical units, calculating lookback times,
 * and initializing other components like cooling functions.
 *
 * Key functions:
 * - init(): Main initialization function that coordinates all setup tasks
 * - set_units(): Defines and converts physical units for the simulation
 * - time_to_present(): Calculates lookback time for a given redshift
 *
 * The cosmological calculations use numerical integration to compute
 * lookback times in a ΛCDM universe.
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "globals.h"
#include "error.h"
#include "integration.h"
#include "generated/reference_units.h"
#include "module_system/physical_constants.h"
#include "numeric.h"
#include "proto.h"

/**
 * @brief   Main initialization function for the Mimic framework
 *
 * Allocates the Age lookback-time table, sets up physical units and derived
 * constants, and computes the redshift and lookback time of every snapshot
 * from the validated scale-factor list. After this function completes, the
 * model is ready to begin processing merger trees.
 */
void init(void) {
  int i;

  /* Allocate the Age table; the +1 offset invariant is documented in globals.h */
  Age_base = mymalloc_cat(ABSOLUTEMAXSNAPS * sizeof(*Age_base), MEM_UTILITY);

  set_units();

  /* Leading slot holds the lookback time to recombination; Age[snap] starts
   * one element past it (Age[-1] addresses this value) */
  Age_base[0] = time_to_present(INITIAL_REDSHIFT);
  Age = Age_base + 1;

  for (i = 0; i < MimicConfig.Snaplistlen; i++) {
    MimicConfig.ZZ[i] = safe_div(1.0, MimicConfig.AA[i], 0.0) - 1;
    Age[i] = time_to_present(MimicConfig.ZZ[i]);
  }
}

/**
 * @brief   Sets up physical units and derived constants
 *
 * This function defines the unit system used throughout the Mimic framework
 * and calculates derived constants. It:
 *
 * 1. Computes derived units (time, density, pressure, energy)
 * 2. Converts physical constants to code units (G, Hubble constant)
 * 3. Computes the critical density of the universe
 *
 * Units are defined in terms of length (cm), mass (g), and velocity (cm/s),
 * with other units derived from these base units.
 */
void set_units(void) {
  MimicConfig.UnitLength_in_cm = MIMIC_REF_UNIT_LENGTH_IN_CM;
  MimicConfig.UnitMass_in_g = MIMIC_REF_UNIT_MASS_IN_G;
  MimicConfig.UnitVelocity_in_cm_per_s = MIMIC_REF_UNIT_VELOCITY_IN_CM_PER_S;

  // Calculate derived units and store in MimicConfig
  MimicConfig.UnitTime_in_s = MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitTime_in_Megayears = MimicConfig.UnitTime_in_s / SEC_PER_MEGAYEAR;
  MimicConfig.G = GRAVITY / pow(MimicConfig.UnitLength_in_cm, 3) * MimicConfig.UnitMass_in_g *
                  pow(MimicConfig.UnitTime_in_s, 2);
  MimicConfig.UnitDensity_in_cgs = MimicConfig.UnitMass_in_g / pow(MimicConfig.UnitLength_in_cm, 3);
  MimicConfig.UnitPressure_in_cgs =
      MimicConfig.UnitMass_in_g / MimicConfig.UnitLength_in_cm / pow(MimicConfig.UnitTime_in_s, 2);
  MimicConfig.UnitCoolingRate_in_cgs = MimicConfig.UnitPressure_in_cgs / MimicConfig.UnitTime_in_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g * pow(MimicConfig.UnitLength_in_cm, 2) /
                                  pow(MimicConfig.UnitTime_in_s, 2);

  // Convert some physical input parameters to internal units
  MimicConfig.Hubble = HUBBLE * MimicConfig.UnitTime_in_s;

  // Compute a few quantities
  MimicConfig.RhoCrit = 3 * MimicConfig.Hubble * MimicConfig.Hubble / (8 * M_PI * MimicConfig.G);
}

/**
 * @brief   Reads the list of snapshot scale factors from a file
 *
 * This function loads the list of snapshot scale factors (a) from the
 * file specified in the configuration. For each snapshot, it:
 *
 * 1. Reads the scale factor value (a = 1/(1+z))
 * 2. Stores it in the MimicConfig.AA array
 * 3. Counts the total number of snapshots
 *
 * If the file cannot be read, the function terminates with a fatal error.
 */
void read_snap_list(void) {
  FILE *fd;
  char fname[MAX_STRING_LEN + 1];
  char line[1024];
  int line_number = 0;

  snprintf(fname, MAX_STRING_LEN, "%s", MimicConfig.FileWithSnapList);

  if (!(fd = fopen(fname, "r"))) {
    FATAL_ERROR("Can't read output list in file '%s'", fname);
  }

  MimicConfig.Snaplistlen = 0;
  while (fgets(line, sizeof(line), fd) != NULL) {
    char *cursor = line;
    char *endptr;
    double scale_factor;

    line_number++;
    while (isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor == '\0' || *cursor == '#') {
      continue;
    }

    errno = 0;
    scale_factor = strtod(cursor, &endptr);
    if (cursor == endptr || errno != 0) {
      FATAL_ERROR("Invalid scale factor in '%s' at line %d", fname, line_number);
    }
    if (!isfinite(scale_factor)) {
      FATAL_ERROR("Scale factor in '%s' at line %d must be finite", fname, line_number);
    }
    if (scale_factor <= 0.0) {
      FATAL_ERROR("Scale factor in '%s' at line %d must be positive", fname, line_number);
    }
    if (MimicConfig.Snaplistlen > 0 &&
        scale_factor <= MimicConfig.AA[MimicConfig.Snaplistlen - 1]) {
      FATAL_ERROR("Scale factors in '%s' must be strictly increasing; line %d "
                  "is not greater than the previous snapshot",
                  fname, line_number);
    }

    while (isspace((unsigned char)*endptr)) {
      endptr++;
    }
    if (*endptr != '\0' && *endptr != '#') {
      FATAL_ERROR("Unexpected text after scale factor in '%s' at line %d", fname, line_number);
    }
    if (MimicConfig.Snaplistlen >= ABSOLUTEMAXSNAPS) {
      FATAL_ERROR("Snapshot scale-factor list '%s' has more than %d entries", fname,
                  ABSOLUTEMAXSNAPS);
    }

    MimicConfig.AA[MimicConfig.Snaplistlen] = scale_factor;
    MimicConfig.Snaplistlen++;
  }

  fclose(fd);

  if (MimicConfig.Snaplistlen == 0) {
    FATAL_ERROR("Snapshot scale-factor list '%s' is empty", fname);
  }

  MimicConfig.LastSnapshotNr = MimicConfig.Snaplistlen - 1;
  MimicConfig.MAXSNAPS = MimicConfig.Snaplistlen;

#ifdef MPI
  if (ThisTask == 0)
#endif
    INFO_LOG("Found %d defined times in snaplist (snapshots 0..%d)", MimicConfig.Snaplistlen,
             MimicConfig.LastSnapshotNr);
}

/**
 * @brief   Calculates the lookback time to a given redshift
 *
 * @param   z   Redshift to calculate lookback time for
 * @return  Lookback time in internal time units
 *
 * Computes the lookback time from the present to redshift z in a LCDM
 * universe:
 *
 * t(z) = 1/H0 * integral da / (a^2 sqrt(Om/a + (1 - Om - OL) + OL a^2))
 *
 * integrated from a = 1/(1+z) to a = 1, in internal time units.
 */
double time_to_present(double z) {
  /* Absolute tolerance 1e-9/H gives SAGE-parity precision for lookback times
   * of order 1/H (sage-code core_init.c used 1e-9 relative tolerance). */
  double result = integrate_adaptive_simpson(integrand_time_to_present, NULL,
                                             safe_div(1.0, z + 1, 1.0), 1.0, 1.0e-10, NULL);

  return safe_div(1.0, MimicConfig.Hubble, 0.0) * result;
}

/**
 * @brief   Integrand function for lookback time calculation
 *
 * @param   a      Scale factor (a = 1/(1+z))
 * @param   param  Unused parameter (required by integration interface)
 * @return  Value of the integrand at scale factor a
 *
 * This function provides the integrand for the lookback time calculation:
 *
 * 1/[a² √(Ω_m/a + (1-Ω_m-Ω_Λ) + Ω_Λ a²)]
 *
 * It represents the differential time element in the Friedmann equation
 * for a ΛCDM universe. The function is passed to the integration
 * routine to compute lookback times.
 */
double integrand_time_to_present(double a, void *param) {
  /* Parameter unused but required by integration function signature */
  (void)param;

  return safe_div(1.0,
                  sqrt(safe_div(MimicConfig.Omega, a, 0.0) +
                       (1 - MimicConfig.Omega - MimicConfig.OmegaLambda) +
                       MimicConfig.OmegaLambda * a * a),
                  0.0);
}
