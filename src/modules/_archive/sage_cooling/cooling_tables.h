/**
 * @file    cooling_tables.h
 * @brief   Metallicity-dependent cooling function tables interface
 *
 * Loads and interpolates Sutherland & Dopita (1993) cooling tables covering
 * metallicities from primordial to super-solar composition.
 *
 * Reference: Sutherland & Dopita (1993), based on SAGE core_cool_func.c
 */

#ifndef COOLING_TABLES_H
#define COOLING_TABLES_H

/**
 * @brief   Load cooling function tables from data files
 *
 * @param   cool_functions_dir  Path to directory containing cooling table files
 * @return  0 on success, -1 on failure
 */
int cooling_tables_init(const char *cool_functions_dir);

/**
 * @brief   Get cooling rate via 2D interpolation in temperature and metallicity
 *
 * @param   logTemp  Log10 of temperature (K)
 * @param   logZ     Log10 of absolute metallicity
 * @return  Cooling rate (erg cm^3 s^-1)
 */
double get_metaldependent_cooling_rate(double logTemp, double logZ);

/**
 * @brief   Free cooling table resources
 */
void cooling_tables_cleanup(void);

#endif /* COOLING_TABLES_H */
