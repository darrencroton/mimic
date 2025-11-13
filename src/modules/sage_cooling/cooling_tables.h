/**
 * @file    cooling_tables.h
 * @brief   Interface for metallicity-dependent cooling function tables
 *
 * This header provides the interface for loading and interpolating cooling
 * function tables used in the SAGE cooling model. The cooling tables are
 * based on collisional ionization equilibrium models and cover a range of
 * metallicities from primordial to super-solar composition.
 *
 * References:
 * - Sutherland & Dopita (1993) - Cooling function tables
 * - SAGE core_cool_func.c - Original implementation
 */

#ifndef COOLING_TABLES_H
#define COOLING_TABLES_H

/**
 * @brief   Initialize and load cooling function tables from data files
 *
 * @param   cool_functions_dir  Path to directory containing cooling table files
 * @return  0 on success, -1 on failure
 *
 * This function loads eight cooling function tables covering metallicities
 * from primordial composition to super-solar. Each table contains 91
 * temperature points from log(T) = 4.0 to 8.5 (in steps of 0.05 dex).
 *
 * The files must be named: stripped_mzero.cie, stripped_m-30.cie, etc.
 */
int cooling_tables_init(const char *cool_functions_dir);

/**
 * @brief   Get metallicity-dependent cooling rate via 2D interpolation
 *
 * @param   logTemp  Log10 of temperature in Kelvin
 * @param   logZ     Log10 of absolute metallicity (not [Fe/H])
 * @return  Cooling rate in units of erg cm^3 s^-1
 *
 * This function performs 2D interpolation of the cooling tables to
 * determine the cooling rate for arbitrary temperature and metallicity.
 * It enforces limits on the metallicity range (primordial to super-solar).
 */
double get_metaldependent_cooling_rate(double logTemp, double logZ);

/**
 * @brief   Free resources allocated for cooling tables
 *
 * This function should be called during module cleanup to free any
 * resources allocated during cooling_tables_init().
 */
void cooling_tables_cleanup(void);

#endif /* COOLING_TABLES_H */
