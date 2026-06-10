/**
 * @file    integration.c
 * @brief   Adaptive Simpson integration for Mimic
 *
 * One public entry point (integrate_adaptive_simpson) over a recursive
 * adaptive Simpson kernel. Accurate for the smooth integrands Mimic needs
 * (cosmological lookback times).
 */

#include "integration.h"
#include <math.h>
#include <stddef.h>

/* Recursion limit: 2^20 subintervals is far beyond what smooth cosmological
 * integrands need to reach the requested tolerance. */
#define MAX_ADAPTIVE_DEPTH 20

/**
 * @brief Recursive adaptive Simpson kernel
 *
 * Compares the Simpson estimate over [a, b] against the sum of the two
 * half-interval estimates; recurses with half the tolerance per side until
 * the difference is within tolerance or the depth limit is reached.
 */
static void adaptive_simpson(integrand_func_t f, void *params, double a, double b, double tol,
                             int depth, int max_depth, double *result, double *error) {
  // Calculate midpoint and evaluate function at three points
  double c = (a + b) / 2.0;
  double fa = f(a, params);
  double fb = f(b, params);
  double fc = f(c, params);

  // Calculate Simpson's rule estimates for whole interval and halves
  double whole = (b - a) * (fa + 4.0 * fc + fb) / 6.0;
  double left = (c - a) * (fa + 4.0 * f((a + c) / 2.0, params) + fc) / 6.0;
  double right = (b - c) * (fc + 4.0 * f((c + b) / 2.0, params) + fb) / 6.0;

  // Calculate the error estimate
  double est_error = fabs(left + right - whole);

  // If error is small enough or at max depth, return result
  if (est_error <= tol || depth >= max_depth) {
    *result = left + right; // More accurate than 'whole'
    *error = est_error;
    return;
  }

  // Otherwise, recursively integrate each half with half the tolerance
  double left_result, left_error;
  double right_result, right_error;

  adaptive_simpson(f, params, a, c, tol / 2.0, depth + 1, max_depth, &left_result, &left_error);
  adaptive_simpson(f, params, c, b, tol / 2.0, depth + 1, max_depth, &right_result, &right_error);

  // Combine results
  *result = left_result + right_result;
  *error = left_error + right_error;
}

double integrate_adaptive_simpson(integrand_func_t f, void *params, double a, double b, double tol,
                                  double *abserr) {
  double result, error;

  adaptive_simpson(f, params, a, b, tol, 0, MAX_ADAPTIVE_DEPTH, &result, &error);

  if (abserr != NULL) {
    *abserr = error;
  }
  return result;
}
