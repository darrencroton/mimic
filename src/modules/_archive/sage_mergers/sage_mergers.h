/**
 * @file    sage_mergers.h
 * @brief   SAGE galaxy merger physics module interface
 *
 * Implements galaxy merger processes including dynamical friction timescales,
 * major/minor mergers, merger-induced starbursts, BH growth, quasar-mode AGN feedback,
 * morphological transformations, and satellite disruption to intracluster stars.
 *
 * Physics: Major merger (mass_ratio > ThreshMajorMerger) → spheroid transformation
 *          Minor merger (mass_ratio > 0.1) → bulge growth with disk preservation
 *          Starburst efficiency: ε_burst = 0.56 × mass_ratio^0.7
 *
 * Reference: Croton et al. (2016), based on SAGE model_mergers.c
 */

#ifndef SAGE_MERGERS_H
#define SAGE_MERGERS_H

/**
 * @brief   Register the sage_mergers module with the module registry
 */
void sage_mergers_register(void);

#endif /* SAGE_MERGERS_H */
