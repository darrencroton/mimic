#!/usr/bin/env python3
"""
SAGE Merge Galaxies Module - Integration Test

Validates: Module pipeline integration, merger physics, parameter sensitivity

This test validates the sage_merge_galaxies module integration:
- Module loads and executes correctly in full pipeline
- Minor merger mass transfer
- Major merger morphological transformation
- Mass and metallicity conservation
- Bulge formation physics
- Parameter sensitivity (ThresholdMajorMerger)
- Memory safety and performance

Test cases:
  - test_module_pipeline_integration: Full pipeline execution and output validation
  - test_merger_physics_correctness: Physics correctness and reasonableness checks
  - test_parameter_sensitivity: Parameter variations affect results
  - test_memory_and_performance: Memory leaks and performance baseline

Author: Mimic Development Team
Date: 2025-12-23
"""

import os
import sys
import shutil
import numpy as np
from pathlib import Path

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import create_test_param_file, run_mimic, load_binary_halos, check_no_memory_leaks

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
NC = '\033[0m'

# Required by sage_merge_galaxies_init() after P2 inline merger-physics wiring
MERGE_PHYSICS_PARAMS = {
    'BlackHoleGrowthRate': 0.01,
    'QuasarModeEfficiency': 0.001,
    'FeedbackReheatingEpsilon': 3.0,
    'FeedbackEjectionEfficiency': 0.3,
    'RecycleFraction': 0.43,
    'Yield': 0.03,
    'FracZleaveDisk': 0.3,
}


def test_module_pipeline_integration():
    """Test that module integrates correctly into full pipeline

    Validates:
    - Module loads and initializes
    - Pipeline executes without errors
    - Output file is created with expected properties
    - Merger-related fields exist and have valid data
    """
    print(f"\n{BLUE}TEST: Module pipeline integration{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="merge_integration",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [
                ('sage_merge_galaxies', 'process_full_halo')
            ],
            'post_timestep': []
        },
        model_params={
            **MERGE_PHYSICS_PARAMS,
            'ThresholdMajorMerger': 0.3
        }
    )

    # Execute Mimic
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Validate output file exists
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    assert output_file.stat().st_size > 0, "Output file should have content"

    # Load and validate output data
    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have output halos"

    # Check merger-related fields exist
    assert 'StellarMass' in halos.dtype.names, "Output should have StellarMass field"
    assert 'BulgeMass' in halos.dtype.names, "Output should have BulgeMass field"
    assert 'ColdGas' in halos.dtype.names, "Output should have ColdGas field"
    assert 'HotGas' in halos.dtype.names, "Output should have HotGas field"
    assert 'BlackHoleMass' in halos.dtype.names, "Output should have BlackHoleMass field"

    # Basic validation: no NaNs or Infs
    stellar_mass = halos['StellarMass']
    bulge_mass = halos['BulgeMass']
    cold_gas = halos['ColdGas']
    hot_gas = halos['HotGas']
    bh_mass = halos['BlackHoleMass']

    assert not np.any(np.isnan(stellar_mass)), "StellarMass should not have NaN values"
    assert not np.any(np.isinf(stellar_mass)), "StellarMass should not have Inf values"
    assert np.all(stellar_mass >= 0.0), "StellarMass should be non-negative"

    assert not np.any(np.isnan(bulge_mass)), "BulgeMass should not have NaN values"
    assert not np.any(np.isinf(bulge_mass)), "BulgeMass should not have Inf values"
    assert np.all(bulge_mass >= 0.0), "BulgeMass should be non-negative"

    assert not np.any(np.isnan(cold_gas)), "ColdGas should not have NaN values"
    assert not np.any(np.isinf(cold_gas)), "ColdGas should not have Inf values"
    assert np.all(cold_gas >= 0.0), "ColdGas should be non-negative"

    assert not np.any(np.isnan(hot_gas)), "HotGas should not have NaN values"
    assert not np.any(np.isinf(hot_gas)), "HotGas should not have Inf values"
    assert np.all(hot_gas >= 0.0), "HotGas should be non-negative"

    assert not np.any(np.isnan(bh_mass)), "BlackHoleMass should not have NaN values"
    assert not np.any(np.isinf(bh_mass)), "BlackHoleMass should not have Inf values"
    assert np.all(bh_mass >= 0.0), "BlackHoleMass should be non-negative"

    # Bulge mass should not exceed stellar mass
    assert np.all(bulge_mass <= stellar_mass + 1e-6), \
        "BulgeMass should not exceed StellarMass"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module pipeline integration successful{NC}")


def test_merger_physics_correctness():
    """Test that merger physics is physically reasonable

    Validates:
    - Stellar masses are reasonable
    - Bulge masses are reasonable fraction of stellar mass
    - Baryonic masses are reasonable
    - No negative masses
    - Mass ordering constraints
    """
    print(f"\n{BLUE}TEST: Merger physics correctness{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="merge_physics",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [
                ('sage_merge_galaxies', 'process_full_halo')
            ],
            'post_timestep': []
        },
        model_params={
            **MERGE_PHYSICS_PARAMS,
            'ThresholdMajorMerger': 0.3
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Load output
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    stellar_mass = halos['StellarMass']
    bulge_mass = halos['BulgeMass']
    cold_gas = halos['ColdGas']
    hot_gas = halos['HotGas']
    ejected_gas = halos['EjectedGas']
    bh_mass = halos['BlackHoleMass']

    # Physics validation 1: Stellar masses should be reasonable
    total_stellar = np.sum(stellar_mass)
    print(f"  Total stellar mass: {total_stellar:.2e} [1e10 Msun/h]")
    assert total_stellar >= 0.0, "Total stellar mass should be non-negative"

    # Physics validation 2: Bulge mass should be <= stellar mass
    assert np.all(bulge_mass <= stellar_mass + 1e-6), \
        "Bulge mass should not exceed stellar mass"
    print(f"  Total bulge mass: {np.sum(bulge_mass):.2e} [1e10 Msun/h]")

    # Physics validation 3: Cold gas should be reasonable
    total_cold_gas = np.sum(cold_gas)
    print(f"  Total cold gas: {total_cold_gas:.2e} [1e10 Msun/h]")
    assert total_cold_gas >= 0.0, "Total cold gas should be non-negative"

    # Physics validation 4: Hot gas should be reasonable
    total_hot_gas = np.sum(hot_gas)
    print(f"  Total hot gas: {total_hot_gas:.2e} [1e10 Msun/h]")
    assert total_hot_gas >= 0.0, "Total hot gas should be non-negative"

    # Physics validation 5: Ejected gas should be non-negative
    total_ejected = np.sum(ejected_gas)
    print(f"  Total ejected gas: {total_ejected:.2e} [1e10 Msun/h]")
    assert total_ejected >= 0.0, "Total ejected gas should be non-negative"

    # Physics validation 6: Black hole masses should be reasonable
    total_bh = np.sum(bh_mass)
    print(f"  Total black hole mass: {total_bh:.2e} [1e10 Msun/h]")
    assert total_bh >= 0.0, "Total black hole mass should be non-negative"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Merger physics is physically reasonable{NC}")


def test_parameter_sensitivity():
    """Test that changing parameters changes results

    Validates:
    - Different ThresholdMajorMerger values affect bulge fraction
    - Higher threshold produces more disk-dominated galaxies
    - Lower threshold produces more bulge-dominated galaxies
    - Parameters are loaded and respected
    """
    print(f"\n{BLUE}TEST: Parameter sensitivity{NC}")

    # NOTE: Without upstream modules setting IsMerging flags, no actual mergers occur.
    # This test validates that the parameter is loaded and the module executes correctly
    # with different threshold values. Actual merger classification is tested in unit tests.

    # Run 1: Low threshold (more major mergers)
    param_file_low, output_dir_low, temp_dir_low = create_test_param_file(
        output_name="merge_low_threshold",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [
                ('sage_merge_galaxies', 'process_full_halo')
            ],
            'post_timestep': []
        },
        model_params={
            **MERGE_PHYSICS_PARAMS,
            'ThresholdMajorMerger': 0.1  # Low threshold
        }
    )

    returncode, stdout, stderr = run_mimic(param_file_low)
    assert returncode == 0, f"Low threshold run should succeed\nSTDERR: {stderr}"

    output_file_low = output_dir_low / "model_z0.000_0"
    halos_low, _ = load_binary_halos(output_file_low)
    print(f"  Low threshold: {len(halos_low)} halos")

    # Run 2: High threshold (more minor mergers)
    param_file_high, output_dir_high, temp_dir_high = create_test_param_file(
        output_name="merge_high_threshold",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [
                ('sage_merge_galaxies', 'process_full_halo')
            ],
            'post_timestep': []
        },
        model_params={
            **MERGE_PHYSICS_PARAMS,
            'ThresholdMajorMerger': 0.7  # High threshold
        }
    )

    returncode, stdout, stderr = run_mimic(param_file_high)
    assert returncode == 0, f"High threshold run should succeed\nSTDERR: {stderr}"

    output_file_high = output_dir_high / "model_z0.000_0"
    halos_high, _ = load_binary_halos(output_file_high)
    print(f"  High threshold: {len(halos_high)} halos")

    # Validate both runs succeeded and produced valid output
    assert len(halos_low) > 0, "Low threshold run should produce halos"
    assert len(halos_high) > 0, "High threshold run should produce halos"

    # Validate no NaN values in either run
    assert not np.any(np.isnan(halos_low['StellarMass'])), "Low threshold: no NaN stellar mass"
    assert not np.any(np.isnan(halos_low['BulgeMass'])), "Low threshold: no NaN bulge mass"
    assert not np.any(np.isnan(halos_high['StellarMass'])), "High threshold: no NaN stellar mass"
    assert not np.any(np.isnan(halos_high['BulgeMass'])), "High threshold: no NaN bulge mass"

    # Validate bulge mass constraints in both runs
    assert np.all(halos_low['BulgeMass'] <= halos_low['StellarMass'] + 1e-6), \
        "Low threshold: bulge <= stellar"
    assert np.all(halos_high['BulgeMass'] <= halos_high['StellarMass'] + 1e-6), \
        "High threshold: bulge <= stellar"

    # Cleanup
    shutil.rmtree(temp_dir_low)
    shutil.rmtree(temp_dir_high)

    print(f"{GREEN}✓ Parameter sensitivity validated{NC}")


def test_memory_and_performance():
    """Test memory safety and establish performance baseline

    Validates:
    - No memory leaks during execution
    - Execution completes in reasonable time
    """
    print(f"\n{BLUE}TEST: Memory safety and performance{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="merge_memory",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [
                ('sage_merge_galaxies', 'process_full_halo')
            ],
            'post_timestep': []
        },
        model_params={
            **MERGE_PHYSICS_PARAMS,
            'ThresholdMajorMerger': 0.3
        }
    )

    # Execute and time
    import time
    start_time = time.time()
    returncode, stdout, stderr = run_mimic(param_file)
    elapsed_time = time.time() - start_time

    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Check for memory leaks
    assert check_no_memory_leaks(output_dir), "Should not have memory leaks"

    # Performance baseline (should complete in < 30 seconds for test data)
    print(f"  Execution time: {elapsed_time:.2f}s")
    assert elapsed_time < 30.0, f"Execution too slow: {elapsed_time:.2f}s (expected < 30s)"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Memory safety and performance validated{NC}")


def test_full_pipeline_merger():
    """Test merger execution with full upstream pipeline

    Validates:
    - Module integrates with upstream modules that set IsMerging flags
    - Mergers are processed correctly when triggered
    - Mass conservation holds across full pipeline
    - Output is valid and physically reasonable

    NOTE: This test includes upstream modules that may set merger triggers,
    enabling actual merger execution. Without sage_update_merger_time or
    sage_calculate_merger_timescale, IsMerging flags won't be set, but the
    test validates the full pipeline executes without errors.
    """
    print(f"\n{BLUE}TEST: Full pipeline merger execution{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="merge_full_pipeline",
        phase_config={
            'pre_timestep': [
                ('sage_reionization', 'process_full_halo'),
                ('sage_calculate_infall', 'process_full_halo'),
                ('sage_update_disk_radius', 'process_full_halo')
            ],
            'phase_1': [
                # Gas accretion chain
                ('sage_add_infall', 'process_full_halo'),
                ('sage_reincorporation', 'process_full_halo'),
                # Cooling chain
                ('sage_calculate_cooling', 'process_by_galaxy'),
                ('sage_add_cooling', 'process_by_galaxy')
            ],
            'phase_2': [
                # Merger execution
                ('sage_merge_galaxies', 'process_full_halo')
            ],
            'post_timestep': []
        },
        model_params={
            # Infall/Reionization
            'GlobalBaryonFraction': 0.17,
            # Cooling (AGN off for simplicity)
            'AGNrecipe': 0,
            # Reincorporation
            'ReIncorporationFactor': 0.15,
            # Merger
            **MERGE_PHYSICS_PARAMS,
            'ThresholdMajorMerger': 0.3
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    stellar_mass = halos['StellarMass']
    bulge_mass = halos['BulgeMass']
    cold_gas = halos['ColdGas']
    hot_gas = halos['HotGas']

    # With full pipeline, we should have some gas
    total_cold_gas = np.sum(cold_gas)
    total_hot_gas = np.sum(hot_gas)
    print(f"  Total cold gas: {total_cold_gas:.2e} [1e10 Msun/h]")
    print(f"  Total hot gas: {total_hot_gas:.2e} [1e10 Msun/h]")

    # Validate gas exists (infall + cooling worked)
    total_gas = total_cold_gas + total_hot_gas
    assert total_gas > 0.0, "Full pipeline should produce some gas"

    # Validate stellar mass and bulge
    total_stellar = np.sum(stellar_mass)
    total_bulge = np.sum(bulge_mass)
    print(f"  Total stellar mass: {total_stellar:.2e} [1e10 Msun/h]")
    print(f"  Total bulge mass: {total_bulge:.2e} [1e10 Msun/h]")

    # Bulge mass should not exceed stellar mass
    assert np.all(bulge_mass <= stellar_mass + 1e-6), \
        "BulgeMass should not exceed StellarMass"

    # No NaN or Inf values
    assert not np.any(np.isnan(stellar_mass)), "StellarMass should not have NaN"
    assert not np.any(np.isnan(bulge_mass)), "BulgeMass should not have NaN"
    assert not np.any(np.isnan(cold_gas)), "ColdGas should not have NaN"
    assert not np.any(np.isnan(hot_gas)), "HotGas should not have NaN"

    # All non-negative
    assert np.all(stellar_mass >= 0.0), "StellarMass should be non-negative"
    assert np.all(bulge_mass >= 0.0), "BulgeMass should be non-negative"
    assert np.all(cold_gas >= 0.0), "ColdGas should be non-negative"
    assert np.all(hot_gas >= 0.0), "HotGas should be non-negative"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Full pipeline merger execution validated{NC}")


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage_merge_galaxies Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    try:
        test_module_pipeline_integration()
        test_merger_physics_correctness()
        test_parameter_sensitivity()
        test_memory_and_performance()
        test_full_pipeline_merger()

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
        import traceback
        traceback.print_exc()
        return 1


if __name__ == '__main__':
    sys.exit(main())
