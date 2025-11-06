/**
 * @file    core_init.c
 * @brief   Initialization functions for the Mimic framework
 *
 * This file contains functions responsible for initializing the Mimic framework.
 * It handles defining physical units, reading snapshot lists, calculating
 * lookback times, and initializing other components like cooling functions.
 *
 * Key functions:
 * - init(): Main initialization function that coordinates all setup tasks
 * - set_units(): Defines and converts physical units for the simulation
 * - read_snap_list(): Loads the list of snapshots from disk
 * - time_to_present(): Calculates lookback time for a given redshift
 *
 * The cosmological calculations use numerical integration to compute
 * lookback times in a ΛCDM universe.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "allvars.h"
#include "proto.h"
#include "error.h"
#include "integration.h"

/**
 * @brief   Main initialization function for the Mimic framework
 *
 * This function coordinates the initialization of all components required
 * by the Mimic framework. It performs the following tasks:
 *
 * 1. Allocates memory for the Age array
 * 2. Initializes the random number generator
 * 3. Sets up physical units and constants
 * 4. Reads the snapshot list and calculates redshifts
 * 5. Computes lookback times for each snapshot
 * 6. Initializes reionization parameters
 * 7. Reads cooling function tables
 *
 * After this function completes, the model is ready to begin processing
 * merger trees and evolving them.
 */
void init(void) {
  int i;

  Age = mymalloc(ABSOLUTEMAXSNAPS * sizeof(*Age));

  // No need for random number generator as it's not actually used in the code

  set_units();
  srand((unsigned)time(NULL));

  read_snap_list();

  Age[0] = time_to_present(INITIAL_REDSHIFT); // lookback time from z=1000 (recombination era)
  Age++;

  for (i = 0; i < MimicConfig.Snaplistlen; i++) {
    MimicConfig.ZZ[i] = 1 / MimicConfig.AA[i] - 1;
    Age[i] = time_to_present(MimicConfig.ZZ[i]);
    ZZ[i] = MimicConfig.ZZ[i]; // Sync with global for backward compatibility
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
 * 3. Calculates supernova energy and feedback parameters in code units
 * 4. Computes the critical density of the universe
 *
 * The function also synchronizes the unit values with global variables
 * for backward compatibility with older code.
 *
 * Units are defined in terms of length (cm), mass (g), and velocity (cm/s),
 * with other units derived from these base units.
 */
void set_units(void) {
  // Calculate derived units and store in MimicConfig
  MimicConfig.UnitTime_in_s =
      MimicConfig.UnitLength_in_cm / MimicConfig.UnitVelocity_in_cm_per_s;
  MimicConfig.UnitTime_in_Megayears =
      MimicConfig.UnitTime_in_s / SEC_PER_MEGAYEAR;
  MimicConfig.G = GRAVITY / pow(MimicConfig.UnitLength_in_cm, 3) *
                 MimicConfig.UnitMass_in_g * pow(MimicConfig.UnitTime_in_s, 2);
  MimicConfig.UnitDensity_in_cgs =
      MimicConfig.UnitMass_in_g / pow(MimicConfig.UnitLength_in_cm, 3);
  MimicConfig.UnitPressure_in_cgs = MimicConfig.UnitMass_in_g /
                                   MimicConfig.UnitLength_in_cm /
                                   pow(MimicConfig.UnitTime_in_s, 2);
  MimicConfig.UnitCoolingRate_in_cgs =
      MimicConfig.UnitPressure_in_cgs / MimicConfig.UnitTime_in_s;
  MimicConfig.UnitEnergy_in_cgs = MimicConfig.UnitMass_in_g *
                                 pow(MimicConfig.UnitLength_in_cm, 2) /
                                 pow(MimicConfig.UnitTime_in_s, 2);

  // Convert some physical input parameters to internal units
  MimicConfig.Hubble = HUBBLE * MimicConfig.UnitTime_in_s;

  // Compute a few quantities
  MimicConfig.RhoCrit =
      3 * MimicConfig.Hubble * MimicConfig.Hubble / (8 * M_PI * MimicConfig.G);

  // Synchronize with global variables (for backward compatibility)
  UnitLength_in_cm = MimicConfig.UnitLength_in_cm;
  UnitMass_in_g = MimicConfig.UnitMass_in_g;
  UnitVelocity_in_cm_per_s = MimicConfig.UnitVelocity_in_cm_per_s;
  UnitTime_in_s = MimicConfig.UnitTime_in_s;
  UnitTime_in_Megayears = MimicConfig.UnitTime_in_Megayears;
  G = MimicConfig.G;
  UnitDensity_in_cgs = MimicConfig.UnitDensity_in_cgs;
  UnitPressure_in_cgs = MimicConfig.UnitPressure_in_cgs;
  UnitCoolingRate_in_cgs = MimicConfig.UnitCoolingRate_in_cgs;
  UnitEnergy_in_cgs = MimicConfig.UnitEnergy_in_cgs;
  Hubble = MimicConfig.Hubble;
  RhoCrit = MimicConfig.RhoCrit;
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
 * The function also synchronizes the snapshot data with global variables
 * for backward compatibility with older code.
 *
 * If the file cannot be read, the function terminates with a fatal error.
 */
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
 * The function also synchronizes the snapshot data with global variables
 * for backward compatibility with older code.
 *
 * If the file cannot be read, the function terminates with a fatal error.
 */
void read_snap_list(void) {
  FILE *fd;
  char fname[MAX_STRING_LEN + 1];

  snprintf(fname, MAX_STRING_LEN, "%s", MimicConfig.FileWithSnapList);

  if (!(fd = fopen(fname, "r"))) {
    FATAL_ERROR("Can't read output list in file '%s'", fname);
  }

  MimicConfig.Snaplistlen = 0;
  do {
    if (fscanf(fd, " %lg ", &MimicConfig.AA[MimicConfig.Snaplistlen]) == 1)
      MimicConfig.Snaplistlen++;
    else
      break;
  } while (MimicConfig.Snaplistlen < MimicConfig.MAXSNAPS);

  fclose(fd);

  // Synchronize with globals for backward compatibility
  Snaplistlen = MimicConfig.Snaplistlen;
  memcpy(AA, MimicConfig.AA, sizeof(double) * ABSOLUTEMAXSNAPS);

#ifdef MPI
  if (ThisTask == 0)
#endif
    INFO_LOG("Found %d defined times in snaplist", MimicConfig.Snaplistlen);
}

/**
 * @brief   Calculates the lookback time to a given redshift
 *
 * @param   z   Redshift to calculate lookback time for
 * @return  Lookback time in internal time units
 *
 * This function computes the lookback time from the present to a given
 * redshift in a ΛCDM universe. It uses numerical integration to calculate:
 *
 * t(z) = 1/H₀ ∫ da / (a² √(Ω_m/a + (1-Ω_m-Ω_Λ) + Ω_Λ a²))
 *
 * where the integration is performed from a=1/(1+z) to a=1.
 *
 * The result is returned in the internal time units of the simulation.
 */
double time_to_present(double z) {
#define WORKSIZE 1000
  integration_function_t F;
  integration_workspace_t *workspace;
  double time, result, abserr;

  workspace = integration_workspace_alloc(WORKSIZE);
  F.function = &integrand_time_to_present;
  F.params = NULL;

  // Use adaptive integration with GAUSS21 method
  integration_qag(&F, 1.0 / (z + 1), 1.0, 1.0 / MimicConfig.Hubble, 1.0e-8,
                  WORKSIZE, INTEG_GAUSS21, workspace, &result, &abserr);

  time = 1 / MimicConfig.Hubble * result;

  integration_workspace_free(workspace);

  // return time to present as a function of redshift
  return time;
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

  return 1 / sqrt(MimicConfig.Omega / a +
                  (1 - MimicConfig.Omega - MimicConfig.OmegaLambda) +
                  MimicConfig.OmegaLambda * a * a);
}
