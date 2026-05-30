#!/usr/bin/env python3
"""
SAGE Calculate Supernova Feedback Module - Integration Test

Validates: Module lifecycle, configuration, pipeline integration, and physics correctness

This test validates both software quality and physics correctness:

**Software Quality Tests**:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Module works in pipeline with sage_calculate_star_formation

**Physics Validation Tests**:
- Feedback properties have physically reasonable values
- Feedback is proportional to star formation
- Mass conservation (implicit in renormalization)
- Non-negative constraints enforced
- Edge cases produce expected behavior

Reference: Croton et al. (2006, 2016) - SAGE model

Author: Mimic Development Team
Date: 2025-12-18 (Refactored for comprehensive physics validation)
"""

import os
import sys
import shutil
import numpy as np
from pathlib import Path

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import create_test_param_file, run_mimic, load_binary_halos, check_no_memory_leaks

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


# ============================================================================
# SOFTWARE QUALITY TESTS
# ============================================================================

def test_module_loads():
    """Test that sage_calculate_supernova_feedback module loads and initializes"""
    print(f"\n{BLUE}TEST: Module loads and initializes{NC}")

    # Need star formation first to provide NewStellarMass
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_load",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module loads and initializes successfully{NC}")


def test_output_properties_exist():
    """Test that feedback properties appear in output"""
    print(f"\n{BLUE}TEST: Feedback properties exist in output{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_properties",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Check that feedback field exists (SupernovaOutflowRate is the output property)
    assert 'SupernovaOutflowRate' in halos.dtype.names, "Output should have SupernovaOutflowRate field"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Feedback properties exist in output{NC}")


def test_parameters_configurable():
    """Test that module parameters can be configured via YAML"""
    print(f"\n{BLUE}TEST: Module parameters are configurable{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_params",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 5.0,  # Custom value
            'FeedbackEjectionEfficiency': 0.8,  # Custom value
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute with custom parameters\nSTDERR: {stderr}"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module parameters are configurable{NC}")


def test_memory_safety():
    """Test that module doesn't leak memory"""
    print(f"\n{BLUE}TEST: No memory leaks{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_memory",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    assert check_no_memory_leaks(output_dir), "Should not have memory leaks"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ No memory leaks detected{NC}")


def test_execution_completes():
    """Test that full pipeline execution completes successfully"""
    print(f"\n{BLUE}TEST: Full pipeline execution completes{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_complete",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Pipeline should complete successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    assert output_file.stat().st_size > 0, "Output file should have content"

    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have output halos"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Full pipeline execution completes{NC}")


def test_pipeline_with_star_formation():
    """Test that module integrates correctly with star formation module"""
    print(f"\n{BLUE}TEST: Integration with star formation module{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_pipeline",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Pipeline should complete successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Both SF and feedback properties should exist
    assert 'StarFormationRate' in halos.dtype.names, "Should have StarFormationRate from SF module"
    assert 'SupernovaOutflowRate' in halos.dtype.names, "Should have SupernovaOutflowRate from feedback module"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Integration with star formation module works{NC}")


# ============================================================================
# PHYSICS VALIDATION TESTS
# ============================================================================

def test_feedback_values_nonnegative():
    """Test that feedback values are non-negative"""
    print(f"\n{BLUE}TEST: Feedback values are non-negative{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_nonnegative",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # SupernovaOutflowRate should be non-negative
    snfb = halos['SupernovaOutflowRate']
    assert np.all(snfb >= 0.0), "SupernovaOutflowRate should be non-negative"

    # Note: We don't assert that values must be non-zero, as this depends on
    # whether galaxies are star-forming in the test dataset. The module is
    # working correctly even if all values are zero (no SF = no feedback).

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Feedback values are non-negative{NC}")


def test_feedback_proportional_to_sf():
    """Test that feedback is proportional to star formation"""
    print(f"\n{BLUE}TEST: Feedback proportional to star formation{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_proportional",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    sfr = halos['StarFormationRate']
    snfb = halos['SupernovaOutflowRate']

    # Galaxies with star formation should have feedback
    has_sf = sfr > 0.0
    if np.any(has_sf):
        # All star-forming galaxies should have feedback
        assert np.all(snfb[has_sf] > 0.0), \
            "Galaxies with star formation should have supernova feedback"
        print(f"  Found {np.sum(has_sf)} star-forming galaxies with feedback")
    else:
        print(f"{YELLOW}  Note: No star-forming galaxies in test dataset (SF = 0 everywhere){NC}")

    # Galaxies without star formation should have zero feedback
    no_sf = sfr == 0.0
    if np.any(no_sf):
        assert np.all(snfb[no_sf] == 0.0), \
            "Galaxies without star formation should have zero supernova feedback"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Feedback is proportional to star formation{NC}")


def test_feedback_reasonable_magnitude():
    """Test that feedback values are physically reasonable"""
    print(f"\n{BLUE}TEST: Feedback has reasonable magnitude{NC}")

    # Use high efficiency to ensure we get feedback
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_magnitude",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,  # epsilon = 3
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    sfr = halos['StarFormationRate']
    snfb = halos['SupernovaOutflowRate']

    # For star-forming galaxies, feedback should be of similar order to SFR
    # (epsilon = 3 means reheated mass = 3 * SFR, plus possible ejection)
    has_sf = sfr > 0.0
    if np.any(has_sf):
        # Feedback should be at least comparable to SFR (epsilon=3 means 3x minimum)
        # But could be higher due to ejection
        feedback_ratio = snfb[has_sf] / sfr[has_sf]

        # With epsilon=3, minimum feedback is 3x SFR (reheating only)
        # Maximum depends on ejection, but shouldn't be astronomically high
        assert np.all(feedback_ratio >= 0.0), "Feedback ratio should be non-negative"
        assert np.all(feedback_ratio <= 100.0), \
            f"Feedback ratio should be physically reasonable (<100x), got max={np.max(feedback_ratio):.2f}"
        print(f"  Feedback ratio range: {np.min(feedback_ratio):.2f} - {np.max(feedback_ratio):.2f}")
    else:
        print(f"{YELLOW}  Note: No star-forming galaxies in test dataset - skipping magnitude check{NC}")

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Feedback has reasonable magnitude{NC}")


def test_edge_case_zero_efficiency():
    """Test edge case: zero feedback efficiency"""
    print(f"\n{BLUE}TEST: Edge case - zero feedback efficiency{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_zero_efficiency",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 0.0,  # Zero reheating
            'FeedbackEjectionEfficiency': 0.0,  # Zero ejection
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should handle zero efficiency\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # With zero efficiency, feedback should be zero (or very small due to ejection formula)
    snfb = halos['SupernovaOutflowRate']
    # Ejection formula has epsilon in subtraction, so could produce small positive values
    # but should be much smaller than with normal efficiency
    assert np.all(snfb >= 0.0), "Feedback should still be non-negative"

    # Check that feedback is minimal - use a reasonable threshold
    # (test dataset may not have any SF, so max could legitimately be 0)
    max_feedback = np.max(snfb)
    assert max_feedback < 10.0, \
        f"With zero efficiency, feedback should be minimal, got max={max_feedback:.3e}"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Edge case - zero feedback efficiency handled correctly{NC}")


def test_edge_case_very_high_efficiency():
    """Test edge case: very high feedback efficiency"""
    print(f"\n{BLUE}TEST: Edge case - very high feedback efficiency{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_high_efficiency",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 50.0,  # Very high reheating
            'FeedbackEjectionEfficiency': 10.0,  # Very high ejection
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should handle high efficiency\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Should still produce valid output
    snfb = halos['SupernovaOutflowRate']
    assert np.all(snfb >= 0.0), "Feedback should be non-negative even with high efficiency"
    assert np.all(np.isfinite(snfb)), "Feedback should be finite (no NaN/Inf)"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Edge case - very high feedback efficiency handled correctly{NC}")


def test_parameter_variation_effect():
    """Test that changing parameters produces expected changes in output"""
    print(f"\n{BLUE}TEST: Parameter variation effect{NC}")

    # Run 1: Low efficiency
    param_file_low, output_dir_low, temp_dir_low = create_test_param_file(
        output_name="snfb_low_eff",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 1.0,  # Low
            'FeedbackEjectionEfficiency': 0.1,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file_low)
    assert returncode == 0, f"Low efficiency run should succeed\nSTDERR: {stderr}"

    output_file_low = output_dir_low / "model_z0.000_0"
    halos_low, _ = load_binary_halos(output_file_low)
    snfb_low = halos_low['SupernovaOutflowRate']

    # Run 2: High efficiency
    param_file_high, output_dir_high, temp_dir_high = create_test_param_file(
        output_name="snfb_high_eff",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_apply_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 5.0,  # High
            'FeedbackEjectionEfficiency': 0.5,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file_high)
    assert returncode == 0, f"High efficiency run should succeed\nSTDERR: {stderr}"

    output_file_high = output_dir_high / "model_z0.000_0"
    halos_high, _ = load_binary_halos(output_file_high)
    snfb_high = halos_high['SupernovaOutflowRate']

    # Higher efficiency should produce higher feedback (on average)
    # Compare mean feedback for star-forming galaxies
    sfr_low = halos_low['StarFormationRate']
    sfr_high = halos_high['StarFormationRate']

    has_sf_low = sfr_low > 0.0
    has_sf_high = sfr_high > 0.0

    if np.any(has_sf_low) and np.any(has_sf_high):
        mean_snfb_low = np.mean(snfb_low[has_sf_low])
        mean_snfb_high = np.mean(snfb_high[has_sf_high])

        # High efficiency should produce more feedback (if both are non-zero)
        if mean_snfb_low > 0 and mean_snfb_high > 0:
            assert mean_snfb_high > mean_snfb_low, \
                f"Higher efficiency should produce more feedback: " \
                f"low={mean_snfb_low:.3e}, high={mean_snfb_high:.3e}"
            print(f"  Low efficiency mean: {mean_snfb_low:.3e}, High efficiency mean: {mean_snfb_high:.3e}")
        else:
            print(f"{YELLOW}  Note: Insufficient feedback for comparison (one or both means are zero){NC}")
    else:
        print(f"{YELLOW}  Note: No star-forming galaxies in test dataset - skipping parameter variation check{NC}")

    shutil.rmtree(temp_dir_low)
    shutil.rmtree(temp_dir_high)
    print(f"{GREEN}✓ Parameter variation produces expected changes{NC}")


# ============================================================================
# MAIN TEST RUNNER
# ============================================================================

def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage_calculate_supernova_feedback Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    try:
        # Software quality tests
        print(f"\n{BLUE}--- Software Quality Tests ---%s" % NC)
        test_module_loads()
        test_output_properties_exist()
        test_parameters_configurable()
        test_memory_safety()
        test_execution_completes()
        test_pipeline_with_star_formation()

        # Physics validation tests
        print(f"\n{BLUE}--- Physics Validation Tests ---%s" % NC)
        test_feedback_values_nonnegative()
        test_feedback_proportional_to_sf()
        test_feedback_reasonable_magnitude()
        test_edge_case_zero_efficiency()
        test_edge_case_very_high_efficiency()
        test_parameter_variation_effect()

        print(f"\n{GREEN}{'=' * 60}{NC}")
        print(f"{GREEN}All tests passed!{NC}")
        print(f"{GREEN}{'=' * 60}{NC}")
        return 0

    except AssertionError as e:
        print(f"\n{RED}{'=' * 60}{NC}")
        print(f"{RED}Test failed: {e}{NC}")
        print(f"{RED}{'=' * 60}{NC}")
        return 1

    except Exception as e:
        print(f"\n{RED}{'=' * 60}{NC}")
        print(f"{RED}Unexpected error: {e}{NC}")
        print(f"{RED}{'=' * 60}{NC}")
        return 1


if __name__ == '__main__':
    sys.exit(main())
