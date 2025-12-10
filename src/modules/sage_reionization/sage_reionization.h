/**
 * @file    sage_reionization.h
 * @brief   SAGE reionization suppression module interface
 *
 * Implements reionization suppression of gas accretion onto low-mass halos using
 * the Gnedin (2000) model. After reionization, increased gas temperature and Jeans
 * mass suppress accretion onto halos below characteristic mass.
 *
 * Physics: HaloBaryonFraction = GlobalBaryonFraction × f_reion(Mvir, z)
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2016)
 */

#ifndef SAGE_REIONIZATION_H
#define SAGE_REIONIZATION_H

void sage_reionization_register(void);

#endif /* SAGE_REIONIZATION_H */
