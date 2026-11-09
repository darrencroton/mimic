/**
 * @file    sage_exp10.h
 * @brief   10^x in the form SAGE26's compiled object computes pow(10.0, x)
 *
 * Apple clang compiles SAGE26's pow(10.0, x) to a call of __exp10 (clang's simplification of pow
 * with base 10; R = the staged SAGE26 source): at the cold streams' two sites
 * (R/src/model_cooling_heating.c:222, :242; the relocations of model_cooling_heating.o, oracle
 * README "The c11 Group") and at the feedback-free-burst threshold (R/src/model_regimes.c:312;
 * model_regimes.o, oracle README "The c13 Group"). macOS's __exp10 and pow need not agree in the
 * last bit, so on Apple platforms the package calls __exp10 there itself rather than relying on
 * the compiler's simplification, which Mimic's -O0 unit-test build does not make. Elsewhere it
 * writes pow.
 *
 * @note Used by shared/sage_cooling_physics.h (the streams) and shared/sage_ffb_regime.h (the
 *       threshold).
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
