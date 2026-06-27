/**
 * @file    numeric.c
 * @brief   Implementation of utility functions for numerical stability
 *
 * This file implements utility functions to improve numerical stability in
 * floating-point operations throughout the Mimic codebase. It provides
 * safer alternatives to direct comparison operations, division, and
 * value bounds checking.
 */

#include "numeric.h"
#include "constants.h"
#include "error.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

bool is_zero(double x) {
  if (isnan(x))
    return false;
  return fabs(x) < EPSILON_SMALL;
}

bool is_equal(double x, double y) {
  if (isnan(x) || isnan(y))
    return false;
  /* Handle exact equality (including infinities) */
  if (x == y)
    return true;
  return fabs(x - y) < EPSILON_MEDIUM;
}

bool is_greater(double x, double y) {
  if (isnan(x) || isnan(y))
    return false;
  return x > y + EPSILON_SMALL;
}

bool is_less(double x, double y) {
  if (isnan(x) || isnan(y))
    return false;
  return x < y - EPSILON_SMALL;
}

bool is_greater_or_equal(double x, double y) {
  if (isnan(x) || isnan(y))
    return false;
  return x >= y - EPSILON_SMALL;
}

bool is_less_or_equal(double x, double y) {
  if (isnan(x) || isnan(y))
    return false;
  return x <= y + EPSILON_SMALL;
}

bool is_within(double x, double min, double max) {
  if (isnan(x) || isnan(min) || isnan(max))
    return false;
  return is_greater_or_equal(x, min) && is_less_or_equal(x, max);
}

double safe_div(double numerator, double denominator, double default_value) {
  if (isnan(numerator) || isnan(denominator))
    return default_value;
  if (is_zero(denominator)) {
    return default_value;
  }
  return numerator / denominator;
}
