/**
 * @file    sage_exp10.h
 * @brief   10^x in the form SAGE26's compiled object computes pow(10.0, x)
 *
 * SAGE parity: on Apple platforms SAGE26's pow(10.0, x) runs as __exp10, which need not agree
 * with pow in the last bit, so the helper calls __exp10 there and pow elsewhere.
 *
 * @note Used by shared/sage_cooling_physics.h (the cold streams) and shared/sage_ffb_regime.h
 *       (the feedback-free-burst threshold).
 */

#ifndef MIMIC_SHARED_SAGE_EXP10_H
#define MIMIC_SHARED_SAGE_EXP10_H

#include <math.h>

/** @brief 10^x as SAGE26's object computes pow(10.0, x): __exp10 on Apple platforms, else pow */
static inline double mimic_sage_exp10(const double x) {
#if defined(__APPLE__)
  return __exp10(x);
#else
  return pow(10.0, x);
#endif
}

#endif /* MIMIC_SHARED_SAGE_EXP10_H */
