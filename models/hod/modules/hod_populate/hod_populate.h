/**
 * @file    hod_populate.h
 * @brief   HOD occupation, placement and draw helpers used by hod_populate
 *
 * Declares the pure functions behind hod_populate so its unit tests can check
 * the hand-checkable spot values and the drawn distributions directly. The
 * module's process() draws every host through hod_populate_draw_occupation()
 * and hod_populate_draw_satellite(), so a statistic measured on these helpers
 * is a statistic of the module's output.
 *
 * Units: masses passed here are in Msun/h (the module forms M = Mvir * 1e10);
 * Rvir is the host's physical virial radius and the comoving offset is in the
 * units of Pos (Mpc/h); velocities are km/s.
 *
 * Draw-index layout within one host's stream (hod_random.h):
 *   0                    central uniform u0
 *   1                    satellite-count uniform (Poisson inversion)
 *   2 + 16 s + 0         satellite s: radius uniform u
 *   2 + 16 s + 1         satellite s: cos(theta) uniform
 *   2 + 16 s + 2         satellite s: phi uniform
 *   2 + 16 s + 3, 5, 7   satellite s: first index of each Box-Muller pair (g1, g2, g3)
 * Indices 2 + 16 s + 9 to 15 are unused, reserved for future draws.
 */

#ifndef HOD_POPULATE_H
#define HOD_POPULATE_H

#include <stdbool.h>
#include <stdint.h>

/** Mass unit of Mvir in Msun/h (internal 1e10 Msun/h) */
#define HOD_MASS_UNIT_MSUN 1.0e10

/** Largest satellite count per host: the created-record identity radix, never a clip */
#define HOD_MAX_SATELLITES 1024

/** Absolute tolerance of the NFW inverse: |m(x)/m(c) - u| <= HOD_NFW_TOLERANCE */
#define HOD_NFW_TOLERANCE 1.0e-12

/** Draw indices within a host's stream (layout in the file header) */
#define HOD_DRAW_CENTRAL 0
#define HOD_DRAW_SATELLITE_COUNT 1
#define HOD_DRAW_FIRST_SATELLITE 2
#define HOD_DRAW_SATELLITE_STRIDE 16

/** The ten run-file parameters and the box size, validated by hod_populate_init() */
struct HodParameters {
  double log_mmin;        /**< HODLogMmin, log10(Msun/h) */
  double sigma_logm;      /**< HODSigmaLogM, dex, > 0 */
  double log_m0;          /**< HODLogM0, log10(Msun/h) */
  double log_m1;          /**< HODLogM1, log10(Msun/h) */
  double alpha;           /**< HODAlpha, dimensionless, >= 0 */
  int seed;               /**< HODSeed, >= 0 */
  double conc_a;          /**< HODConcA, dimensionless, > 0 */
  double conc_log_mpivot; /**< HODConcLogMpivot, log10(Msun/h) */
  double conc_b;          /**< HODConcB, mass slope */
  double conc_c;          /**< HODConcC, redshift slope */
  double box_size;        /**< BoxSize, Mpc/h, finite and > 0 */
};

/** One host's occupation draw */
struct HodOccupation {
  double mean_ncen;   /**< <Ncen>(M) */
  double lambda;      /**< Poisson mean of satellites given a central; 0 when M <= 10^HODLogM0 */
  bool central;       /**< u0 < <Ncen> */
  int num_satellites; /**< Nsat; 0 without a central; > HOD_MAX_SATELLITES signals overflow */
};

/** One satellite's placement relative to its host */
struct HodSatellite {
  double x;           /**< NFW scaled radius r / r_s, solving m(x)/m(c) = u */
  double r_phys;      /**< Physical radius Rvir x / c */
  double r_com;       /**< Comoving radius r_phys (1 + z) */
  double offset[3];   /**< Comoving offset r_com n, before wrapping */
  double velocity[3]; /**< Velocity offset (g1, g2, g3) Vvir / sqrt(2), km/s */
};

/**
 * @brief   Mean central occupation 0.5 [1 + erf((log10 M - HODLogMmin) / HODSigmaLogM)]
 *
 * Zheng et al. (2005) and Zheng, Coil & Zehavi (2007) equation 2. A mass at or
 * below zero has no central (returns 0).
 */
double hod_populate_mean_ncen(const struct HodParameters *p, double mass);

/**
 * @brief   Satellite Poisson mean ((M - 10^HODLogM0) / 10^HODLogM1)^HODAlpha for M > 10^HODLogM0
 *
 * Zheng, Coil & Zehavi (2007) equation 5 without the <Ncen> factor (the module
 * draws satellites only for a present central). Returns 0 for M <= 10^HODLogM0.
 */
double hod_populate_lambda(const struct HodParameters *p, double mass);

/**
 * @brief   Concentration HODConcA (M / 10^HODConcLogMpivot)^HODConcB (1 + z)^HODConcC
 *
 * Duffy et al. (2008), Table 1 form.
 */
double hod_populate_concentration(const struct HodParameters *p, double mass, double redshift);

/** @brief NFW enclosed-mass shape m(x) = ln(1 + x) - x / (1 + x) */
double hod_populate_nfw_mass(double x);

/**
 * @brief   Solve m(x) / m(c) = u for x by bisection on [0, c]
 *
 * Stops at the first midpoint with |m(x)/m(c) - u| <= HOD_NFW_TOLERANCE (or
 * once the bracket stops shrinking in double precision).
 *
 * @param   u  Target enclosed fraction in (0, 1)
 * @param   c  Concentration, finite and > 0
 * @return  x in [0, c]
 */
double hod_populate_nfw_inverse(double u, double c);

/**
 * @brief   Wrap a coordinate into [0, box): exactly box (or any multiple) maps to 0
 *
 * @param   coordinate  Coordinate in the units of box (finite)
 * @param   box         Box side, finite and > 0
 */
double hod_populate_wrap(double coordinate, double box);

/**
 * @brief   Store a wrapped coordinate as a float that is still inside [0, box)
 *
 * A double just below box can round up to box in float; the stored value is
 * then the largest float below box, within one float rounding of the double.
 */
float hod_populate_store_position(double wrapped, double box);

/**
 * @brief   Draw one host's occupation from its stream key
 *
 * Central present iff u0 < <Ncen>; given a central and lambda > 0, Nsat is
 * Poisson(lambda) by inversion from one uniform. Draws index 1 only when it is
 * used. The caller rejects lambda >= HOD_MAX_SATELLITES before drawing and a
 * returned num_satellites > HOD_MAX_SATELLITES after.
 */
void hod_populate_draw_occupation(const struct HodParameters *p, uint64_t key, double mass,
                                  struct HodOccupation *out);

/**
 * @brief   Draw satellite @p s of a host: NFW radius, isotropic direction, Gaussian velocity
 *
 * @param   key           Host stream key
 * @param   s             Satellite ordinal within the host, 0-based
 * @param   concentration Host concentration (hod_populate_concentration())
 * @param   rvir          Host physical virial radius
 * @param   vvir          Host virial velocity, km/s
 * @param   redshift      Snapshot redshift, for the comoving conversion
 * @param   out           Receives the placement
 */
void hod_populate_draw_satellite(uint64_t key, int s, double concentration, double rvir,
                                 double vvir, double redshift, struct HodSatellite *out);

/**
 * @brief   Whether @p snapshot is an output snapshot of the run
 *
 * True when it is in MimicConfig.ListOutputSnaps[0, NOUT), and for every
 * snapshot when the list is empty (NOUT == 0), matching an empty
 * output.snapshot_list.
 */
bool hod_populate_is_output_snapshot(int snapshot);

#endif /* HOD_POPULATE_H */
