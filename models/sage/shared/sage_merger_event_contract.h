#ifndef MIMIC_SHARED_SAGE_EVENTS_H
#define MIMIC_SHARED_SAGE_EVENTS_H

/*
 * DEPRECATED: This header is superseded by the generated event contracts.
 *
 * Event identity is now producer-scoped and generated from module_info.yaml
 * declarations. Use:
 *
 *   #include "module_system/generated/event_contracts.h"
 *
 * and reference SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER instead of
 * SAGE_EVENT_MERGER. The hand-authored SageEventCode enum below is kept for
 * reference only and must not be used in new code.
 *
 * Payload contract (unchanged):
 *   value0 = baryonic mass ratio (mi/ma)
 *   value1 = source-object substep dt (deltaT_p / STEPS)
 */
enum SageEventCode {
  SAGE_EVENT_NONE = 0,
  SAGE_EVENT_MERGER = 1  /* Superseded by SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER */
};

#endif /* MIMIC_SHARED_SAGE_EVENTS_H */
