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
 * issues including NaN propagation.
 *
 * All comparison functions return false for NaN inputs (IEEE 754 standard).
 */

/**
 * @brief   Checks if a value is effectively zero (within EPSILON_SMALL)
 *
 * @param   x   The value to check
 * @return  true if |x| < EPSILON_SMALL, false otherwise (including NaN)
 * @note    Returns false for NaN values (IEEE 754 standard)
 */
bool is_zero(double x);

/**
 * @brief   Checks if two values are equal within EPSILON_MEDIUM
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if |x-y| < EPSILON_MEDIUM, false otherwise (including NaN)
 * @note    Returns false if either value is NaN (IEEE 754 standard)
 */
bool is_equal(double x, double y);

/**
 * @brief   Checks if x is definitely greater than y (accounting for
 * floating-point error)
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if x > y + EPSILON_SMALL, false otherwise (including NaN)
 * @note    Returns false if either value is NaN (IEEE 754 standard)
 */
bool is_greater(double x, double y);

/**
 * @brief   Checks if x is definitely less than y (accounting for floating-point
 * error)
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if x < y - EPSILON_SMALL, false otherwise (including NaN)
 * @note    Returns false if either value is NaN (IEEE 754 standard)
 */
bool is_less(double x, double y);

/**
 * @brief   Checks if x is greater than or equal to y (accounting for
 * floating-point error)
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if x >= y - EPSILON_SMALL, false otherwise (including NaN)
 * @note    Returns false if either value is NaN (IEEE 754 standard)
 */
bool is_greater_or_equal(double x, double y);

/**
 * @brief   Checks if x is less than or equal to y (accounting for
 * floating-point error)
 *
 * @param   x   First value to compare
 * @param   y   Second value to compare
 * @return  true if x <= y + EPSILON_SMALL, false otherwise (including NaN)
 * @note    Returns false if either value is NaN (IEEE 754 standard)
 */
bool is_less_or_equal(double x, double y);

/**
 * @brief   Checks if a value is within a specified range (inclusive)
 *
 * @param   x     Value to check
 * @param   min   Minimum acceptable value
 * @param   max   Maximum acceptable value
 * @return  true if min <= x <= max (accounting for floating-point error), false
 * otherwise (including NaN)
 * @note    Returns false if any value is NaN (IEEE 754 standard)
 */
bool is_within(double x, double min, double max);

/**
 * @brief   Performs division with protection against division by zero and NaN
 *
 * @param   numerator      The numerator value
 * @param   denominator    The denominator value
 * @param   default_value  Value to return if denominator is effectively zero or
 * NaN
 * @return  numerator/denominator if denominator is not zero (within
 * EPSILON_SMALL) and both values are valid, default_value otherwise
 *
 * This function provides safe division that avoids division by zero errors and
 * NaN propagation. If the denominator is effectively zero (within
 * EPSILON_SMALL) or if either value is NaN, the function returns the specified
 * default value instead of performing the division.
 *
 * Example usage:
 *   double ratio = safe_div(mass, volume, 0.0);  // Returns 0.0 if volume ~ 0
 * or NaN
 *   double efficiency = safe_div(output, input, 1.0);  // Returns 1.0 if input
 * ~ 0 or NaN
 *
 * @note    Returns default_value if either argument is NaN (IEEE 754 standard)
 */
double safe_div(double numerator, double denominator, double default_value);

#endif /* UTIL_NUMERIC_H */
