/**
 * @file    integration.h
 * @brief   Numerical integration utilities for Mimic
 *
 * Provides adaptive Simpson integration for smooth one-dimensional integrands
 * such as the cosmological lookback-time integrand.
 */

#ifndef UTIL_INTEGRATION_H
#define UTIL_INTEGRATION_H

/**
 * @brief Function pointer type for the integrand function
 */
typedef double (*integrand_func_t)(double x, void *params);

/**
 * @brief   Integrate f over [a, b] with adaptive Simpson's rule
 *
 * Recursively bisects the interval until the local error estimate falls below
 * the (halved-per-level) tolerance or the maximum recursion depth is reached,
 * so a result is always returned — for smooth integrands the error estimate
 * is then well within @p tol.
 *
 * @param   f       Integrand function
 * @param   params  Opaque parameter pointer passed to @p f
 * @param   a       Lower integration limit
 * @param   b       Upper integration limit
 * @param   tol     Absolute error tolerance
 * @param   abserr  Optional output: accumulated error estimate (may be NULL)
 * @return  The integral estimate
 */
double integrate_adaptive_simpson(integrand_func_t f, void *params, double a, double b, double tol,
                                  double *abserr);

#endif /* UTIL_INTEGRATION_H */
