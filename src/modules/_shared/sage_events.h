#ifndef MIMIC_SHARED_SAGE_EVENTS_H
#define MIMIC_SHARED_SAGE_EVENTS_H

/*
 * SAGE-owned event code definitions for process_per_event channeling.
 * Event code semantics are module-level physics decisions, not core concerns.
 *
 * Payload contract for SAGE_EVENT_MERGER:
 *   value0 = baryonic mass ratio (mi/ma)
 *   value1 = source-object substep dt (deltaT_p / STEPS)
 */
enum SageEventCode {
  SAGE_EVENT_NONE = 0,
  SAGE_EVENT_MERGER = 1
};

#endif /* MIMIC_SHARED_SAGE_EVENTS_H */
