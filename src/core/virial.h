#ifndef CORE_VIRIAL_H
#define CORE_VIRIAL_H

/**
 * @file    virial.h
 * @brief   Virial radius and velocity of a halo of given mass, for core and modules
 *
 * The one definition both the catalogue helpers in proto.h (get_virial_radius(),
 * get_virial_velocity(), which take an input view) and a module that reasons about
 * a mass it holds (hod_populate's satellite placement) apply: a mean density of
 * 200 times the critical density of the run's cosmology (MimicConfig) at the given
 * redshift. Modules include this header rather than the core-internal proto.h.
 */

/**
 * @brief   Virial radius of a halo of mass @p mvir at @p redshift
 *
 * @param   mvir      Virial mass in 1e10 Msun/h
 * @param   redshift  Redshift at which the critical density is taken
 * @return  Virial radius in Mpc/h (0 when the critical density is not positive)
 */
double virial_radius_for_mass(double mvir, double redshift);

/**
 * @brief   Circular velocity at the virial radius, sqrt(G Mvir / Rvir)
 *
 * @param   mvir  Virial mass in 1e10 Msun/h
 * @param   rvir  Virial radius in Mpc/h
 * @return  Virial velocity in km/s, or 0 when @p rvir is not positive
 */
double virial_velocity_for(double mvir, double rvir);

#endif /* CORE_VIRIAL_H */
