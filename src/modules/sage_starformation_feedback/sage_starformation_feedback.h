/**
 * @file    sage_starformation_feedback.h
 * @brief   SAGE star formation and feedback module interface
 *
 * This module implements star formation in galaxy disks and supernova feedback
 * (reheating and ejection) from the SAGE model.
 *
 * Physics:
 *   - Star formation: Kennicutt-Schmidt law with critical gas density threshold
 *   - Reheating: Cold gas → Hot gas (proportional to stellar mass formed)
 *   - Ejection: Hot gas → Ejected reservoir (energy-driven outflow)
 *   - Metal enrichment: Instantaneous recycling approximation
 *
 * Reference:
 *   - Croton et al. (2016) - SAGE model description
 *   - Kennicutt (1998) - Star formation law
 *   - Kauffmann (1996) - Critical gas density threshold
 *   - SAGE source: sage-code/model_starformation_and_feedback.c
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Source of Truth: Updates GalaxyData properties only
 */

#ifndef SAGE_STARFORMATION_FEEDBACK_H
#define SAGE_STARFORMATION_FEEDBACK_H

/**
 * @brief Register the SAGE star formation and feedback module
 *
 * Called during module system initialization to register this module
 * with the core execution pipeline.
 */
void sage_starformation_feedback_register(void);

#endif /* SAGE_STARFORMATION_FEEDBACK_H */
