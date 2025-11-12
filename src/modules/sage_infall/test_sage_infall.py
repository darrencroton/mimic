#!/usr/bin/env python3
"""
Integration tests for sage_infall module.

This test validates software quality aspects of the sage_infall module:
- Module loads and initializes correctly
- Parameters can be configured via .par files
- Module executes without errors or memory leaks
- Output properties appear in output files
- Module works in multi-module pipelines

NOTE: Physics validation (reionization correctness, infall amounts) deferred
      to Phase 4.3+ when downstream modules (cooling, star formation) are implemented.

Phase: Phase 4.2 (SAGE Physics Module Implementation)
Author: Mimic Development Team
Date: 2025-11-12
"""

import os
import sys
import shutil
import subprocess
import tempfile
import unittest

# Add tests directory to path to import framework
repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../..'))
sys.path.insert(0, os.path.join(repo_root, 'tests'))
from framework import load_binary_halos


class TestSageInfall(unittest.TestCase):
    """Integration tests for sage_infall module software quality."""

    @classmethod
    def setUpClass(cls):
        """Set up test environment once for all tests."""
        # Get repository root (three levels up from src/modules/sage_infall/)
        cls.repo_root = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "..", "..")
        )

        # Path to mimic executable
        cls.mimic_exe = os.path.join(cls.repo_root, "mimic")

        # Check mimic is compiled
        if not os.path.exists(cls.mimic_exe):
            raise RuntimeError(
                f"Mimic executable not found at {cls.mimic_exe}. "
                "Run 'make' to compile."
            )

        # Reference parameter file
        cls.ref_param_file = os.path.join(
            cls.repo_root, "input", "millennium.par"
        )

        # Create temporary directory for test outputs
        cls.temp_dir = tempfile.mkdtemp(prefix="mimic_sage_infall_test_")

    @classmethod
    def tearDownClass(cls):
        """Clean up test environment."""
        # Remove temporary directory
        if os.path.exists(cls.temp_dir):
            shutil.rmtree(cls.temp_dir)

    def _read_param_file(self, param_file):
        """Read parameter file and return as dictionary."""
        params = {}
        with open(param_file, 'r') as f:
            for line in f:
                line = line.strip()
                # Skip comments and empty lines
                if not line or line.startswith('%') or line.startswith('#'):
                    continue
                # Handle arrow notation (-> means skip)
                if '->' in line:
                    continue
                # Parse key-value pairs
                parts = line.split()
                if len(parts) >= 2:
                    params[parts[0]] = ' '.join(parts[1:])
        return params

    def _create_test_param_file(self, output_name, enabled_modules=None,
                                module_params=None, first_file=0, last_file=0):
        """
        Create a test parameter file with specified module configuration.

        Args:
            output_name: Name for output directory
            enabled_modules: List of module names to enable
            module_params: Dict of {ParamName: value} for module parameters
            first_file: First file to process (default: 0)
            last_file: Last file to process (default: 0)

        Returns:
            Path to created parameter file
        """
        # Read reference parameter file
        ref_params = self._read_param_file(self.ref_param_file)

        # Create output directory
        output_dir = os.path.join(self.temp_dir, output_name)
        os.makedirs(output_dir, exist_ok=True)

        # Update parameters
        ref_params['OutputDir'] = output_dir
        ref_params['FirstFile'] = str(first_file)
        ref_params['LastFile'] = str(last_file)
        ref_params['OutputFormat'] = 'binary'  # Use binary format for testing

        # Write test parameter file
        param_path = os.path.join(self.temp_dir, f"{output_name}.par")
        with open(param_path, 'w') as f:
            f.write("%------------------------------------------\n")
            f.write("%----- sage_infall Integration Test ------\n")
            f.write("%------------------------------------------\n\n")

            # Write basic parameters
            for key in ['OutputFileBaseName', 'OutputDir', 'FirstFile',
                       'LastFile', 'TreeName', 'TreeType', 'OutputFormat',
                       'SimulationDir', 'FileWithSnapList', 'LastSnapshotNr',
                       'BoxSize', 'Omega', 'OmegaLambda', 'Hubble_h',
                       'PartMass', 'UnitLength_in_cm', 'UnitMass_in_g',
                       'UnitVelocity_in_cm_per_s']:
                if key in ref_params:
                    f.write(f"{key:30s} {ref_params[key]}\n")

            # Write snapshot output list
            f.write("\nNumOutputs        8\n")
            f.write("-> 63 37 32 27 23 20 18 16\n\n")

            # Write module configuration
            f.write("%------------------------------------------\n")
            f.write("%----- Galaxy Physics Modules -------------\n")
            f.write("%------------------------------------------\n")

            if enabled_modules:
                f.write(f"EnabledModules          {','.join(enabled_modules)}\n\n")

                # Write module parameters
                if module_params:
                    for param_name, value in module_params.items():
                        f.write(f"{param_name:30s} {value}\n")

        return param_path

    def _run_mimic(self, param_file):
        """
        Run mimic with specified parameter file.

        Returns:
            (returncode, stdout, stderr)
        """
        result = subprocess.run(
            [self.mimic_exe, param_file],
            capture_output=True,
            text=True,
            timeout=60  # 60 second timeout for integration tests
        )
        return result.returncode, result.stdout, result.stderr

    def test_module_loads(self):
        """Test that sage_infall module loads and initializes successfully."""
        print("\n  Testing module load and initialization...")

        param_file = self._create_test_param_file(
            output_name="sage_infall_load",
            enabled_modules=["sage_infall"],
            module_params={
                "SageInfall_BaryonFrac": "0.17",
                "SageInfall_ReionizationOn": "1"
            }
        )

        returncode, stdout, stderr = self._run_mimic(param_file)

        # Check execution succeeded
        self.assertEqual(returncode, 0,
                        f"Mimic should execute successfully with sage_infall\nStderr: {stderr}")

        # Check initialization log message
        self.assertIn("SAGE infall module initialized", stdout,
                     "sage_infall should log initialization message")

    def test_output_properties_exist(self):
        """Test that HotGas and related properties appear in output."""
        print("\n  Testing output properties...")

        param_file = self._create_test_param_file(
            output_name="sage_infall_output",
            enabled_modules=["sage_infall"]
        )

        returncode, stdout, stderr = self._run_mimic(param_file)
        self.assertEqual(returncode, 0, "Mimic execution should succeed")

        # Load output file
        output_dir = os.path.join(self.temp_dir, "sage_infall_output")
        output_file = os.path.join(output_dir, "model_z0.000_0")
        self.assertTrue(os.path.exists(output_file), "Output file should exist")

        # Load and check halos
        halos, metadata = load_binary_halos(output_file)
        self.assertGreater(len(halos), 0, "Should have halos in output")

        # Check HotGas property exists
        self.assertIn('HotGas', halos.dtype.names,
                     "HotGas property should exist in output")
        self.assertIn('MetalsHotGas', halos.dtype.names,
                     "MetalsHotGas property should exist in output")

    def test_parameters_configurable(self):
        """Test that all sage_infall parameters can be configured."""
        print("\n  Testing parameter configuration...")

        # Test with non-default parameter values
        param_file = self._create_test_param_file(
            output_name="sage_infall_params",
            enabled_modules=["sage_infall"],
            module_params={
                "SageInfall_BaryonFrac": "0.20",
                "SageInfall_ReionizationOn": "0",
                "SageInfall_Reionization_z0": "9.0",
                "SageInfall_Reionization_zr": "6.0",
                "SageInfall_StrippingSteps": "5"
            }
        )

        returncode, stdout, stderr = self._run_mimic(param_file)
        self.assertEqual(returncode, 0, "Execution with custom parameters should succeed")

        # Verify parameters were read
        self.assertIn("BaryonFrac = 0.2000", stdout,
                     "Custom BaryonFrac should be logged")
        self.assertIn("ReionizationOn = 0", stdout,
                     "Custom ReionizationOn should be logged")

    def test_reionization_toggle(self):
        """Test that ReionizationOn parameter affects execution (no crash)."""
        print("\n  Testing reionization toggle...")

        # Test with reionization ON
        param_file_on = self._create_test_param_file(
            output_name="sage_infall_reion_on",
            enabled_modules=["sage_infall"],
            module_params={"SageInfall_ReionizationOn": "1"}
        )

        returncode_on, stdout_on, stderr_on = self._run_mimic(param_file_on)
        self.assertEqual(returncode_on, 0, "Should run with reionization ON")

        # Test with reionization OFF
        param_file_off = self._create_test_param_file(
            output_name="sage_infall_reion_off",
            enabled_modules=["sage_infall"],
            module_params={"SageInfall_ReionizationOn": "0"}
        )

        returncode_off, stdout_off, stderr_off = self._run_mimic(param_file_off)
        self.assertEqual(returncode_off, 0, "Should run with reionization OFF")

        # Both should succeed (physics differences tested later)
        self.assertIn("ReionizationOn = 1", stdout_on, "ON mode logged")
        self.assertIn("ReionizationOn = 0", stdout_off, "OFF mode logged")

    def test_memory_safety(self):
        """Test that sage_infall doesn't leak memory."""
        print("\n  Testing memory safety...")

        param_file = self._create_test_param_file(
            output_name="sage_infall_memory",
            enabled_modules=["sage_infall"]
        )

        returncode, stdout, stderr = self._run_mimic(param_file)
        self.assertEqual(returncode, 0, "Execution should succeed")

        # Check for memory leak messages
        self.assertNotIn("Memory leak detected", stdout,
                        "Should not have memory leaks")
        self.assertNotIn("Memory leak detected", stderr,
                        "Should not have memory leaks in stderr")

    def test_execution_completes(self):
        """Test that full pipeline execution completes without errors."""
        print("\n  Testing full pipeline completion...")

        param_file = self._create_test_param_file(
            output_name="sage_infall_complete",
            enabled_modules=["sage_infall"],
            first_file=0,
            last_file=0  # Process single file
        )

        returncode, stdout, stderr = self._run_mimic(param_file)
        self.assertEqual(returncode, 0, "Pipeline should complete successfully")

        # Check for success messages
        self.assertIn("SAGE infall module initialized", stdout,
                     "Module initialization message")
        self.assertIn("SAGE infall module cleaned up", stdout,
                     "Module cleanup message")

    def test_multiple_module_pipeline(self):
        """Test that sage_infall works with other modules in pipeline."""
        print("\n  Testing multi-module pipeline...")

        # Test sage_infall with simple_cooling and simple_sfr
        param_file = self._create_test_param_file(
            output_name="sage_infall_multi",
            enabled_modules=["sage_infall", "simple_cooling", "simple_sfr"],
            module_params={
                "SageInfall_BaryonFrac": "0.17",
                "SimpleCooling_BaryonFraction": "0.15",
                "SimpleSFR_Efficiency": "0.02"
            }
        )

        returncode, stdout, stderr = self._run_mimic(param_file)
        self.assertEqual(returncode, 0,
                        "Multi-module pipeline should execute successfully")

        # Verify all modules initialized
        self.assertIn("SAGE infall module initialized", stdout)
        self.assertIn("Simple cooling module initialized", stdout)
        self.assertIn("Simple star formation rate module initialized", stdout)


if __name__ == '__main__':
    # Run tests
    unittest.main(verbosity=2)
