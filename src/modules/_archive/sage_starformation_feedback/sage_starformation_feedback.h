/**
 * @file    sage_starformation_feedback.h
 * @brief   SAGE star formation and feedback module interface
 *
 * Implements star formation in galaxy disks (Kennicutt-Schmidt law) and supernova
 * feedback: reheating (cold → hot gas) and ejection (hot → ejected reservoir).
 * Metal enrichment via instantaneous recycling approximation.
 *
 * Physics: Star formation with critical gas density threshold
 *          Reheating ∝ stellar mass formed
 *          Ejection via energy-driven outflows
 *
 * Reference: Kennicutt (1998), Kauffmann (1996), Croton et al. (2016),
 *            based on SAGE model_starformation_and_feedback.c
 */

#ifndef SAGE_STARFORMATION_FEEDBACK_H
#define SAGE_STARFORMATION_FEEDBACK_H

/**
 * @brief   Register the sage_starformation_feedback module with the module registry
 */
void sage_starformation_feedback_register(void);

#endif /* SAGE_STARFORMATION_FEEDBACK_H */
