#ifndef UTIL_NUMERIC_H
#define UTIL_NUMERIC_H

#include "constants.h"
#include <math.h>
#include <stdbool.h>

/**
 * @file    util_numeric.h
 * @brief   Utility functions for numerical stability and safer floating-point
 * operations
 *
 * This file provides utility functions to improve numerical stability in
 * the Mimic codebase by offering safer floating-point comparison operations,
 * division checks, and value validation functions. Using these utilities
 * instead of direct floating-point operations helps avoid common numerical
 * issues.
 */

/**
 * @brief   Checks if a value is effectively zero (within EPSILON_SMALL)
 *
 * @param   x   The value to check
 * @return  true if |x| < EPSILON_SMALL, false otherwise
 */
bool is_zero(double x);

/**
 * @brief   Checks if two values are equal within EPSILON_MEDIUM
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if |x-y| < EPSILON_MEDIUM, false otherwise
 */
bool is_equal(double x, double y);

/**
 * @brief   Checks if x is definitely greater than y (accounting for
 * floating-point error)
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if x > y + EPSILON_SMALL, false otherwise
 */
bool is_greater(double x, double y);

/**
 * @brief   Checks if x is definitely less than y (accounting for floating-point
 * error)
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if x < y - EPSILON_SMALL, false otherwise
 */
bool is_less(double x, double y);

/**
 * @brief   Checks if x is greater than or equal to y (accounting for
 * floating-point error)
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if x >= y - EPSILON_SMALL, false otherwise
 */
bool is_greater_or_equal(double x, double y);

/**
 * @brief   Checks if x is less than or equal to y (accounting for
 * floating-point error)
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if x <= y + EPSILON_SMALL, false otherwise
 */
bool is_less_or_equal(double x, double y);

/**
 * @brief   Checks if a value is within a specified range (inclusive)
 *
 * @param   x     Value to check
 * @param   min   Minimum acceptable value
 * @param   max   Maximum acceptable value
 * @return  true if min <= x <= max (accounting for floating-point error), false
 * otherwise
 */
bool is_within(double x, double min, double max);

/**
 * @brief   Performs division with protection against division by zero
 *
 * @param   numerator      The numerator value
 * @param   denominator    The denominator value
 * @param   default_value  Value to return if denominator is effectively zero
 * @return  numerator/denominator if denominator is not zero (within
 * EPSILON_SMALL), default_value otherwise
 *
 * This function provides safe division that avoids division by zero errors.
 * If the denominator is effectively zero (within EPSILON_SMALL), the function
 * returns the specified default value instead of performing the division.
 *
 * Example usage:
 *   double ratio = safe_div(mass, volume, 0.0);  // Returns 0.0 if volume ~ 0
 *   double efficiency = safe_div(output, input, 1.0);  // Returns 1.0 if input
 * ~ 0
 */
double safe_div(double numerator, double denominator, double default_value);

#endif /* UTIL_NUMERIC_H */
