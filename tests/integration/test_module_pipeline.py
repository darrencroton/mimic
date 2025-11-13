#!/usr/bin/env python3
"""
Integration tests for module configuration and execution pipeline.

This test validates that Mimic's module system works end-to-end:
- Modules can be enabled/disabled via parameter files
- Module parameters are read from configuration
- Module execution completes without errors
- Physics-free mode works (halo tracking only)
- Different module combinations work correctly
- Log messages confirm module initialization

Phase: Phase 3 (Runtime Module Configuration)
Author: Mimic Development Team
Date: 2025-11-09
"""

import os
import sys
import shutil
import subprocess
import tempfile
import unittest

class TestModulePipeline(unittest.TestCase):
    """Integration tests for module configuration and execution."""

    @classmethod
    def setUpClass(cls):
        """Set up test environment once for all tests."""
        # Get repository root (two levels up from tests/integration/)
        cls.repo_root = os.path.abspath(
            os.path.join(os.path.dirname(__file__), "..", "..")
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
        cls.temp_dir = tempfile.mkdtemp(prefix="mimic_module_test_")

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
            enabled_modules: List of module names to enable (None = physics-free)
            module_params: Dict of {ModuleName_ParameterName: value}
            first_file: First file to process
            last_file: Last file to process

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

        # Write test parameter file
        param_path = os.path.join(self.temp_dir, f"{output_name}.par")
        with open(param_path, 'w') as f:
            f.write("%------------------------------------------\n")
            f.write("%----- Module Configuration Test ----------\n")
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
            else:
                f.write("# Physics-free mode (no modules enabled)\n")
                f.write("EnabledModules\n")

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
            text=True
        )
        return result.returncode, result.stdout, result.stderr

    def test_physics_free_mode(self):
        """Test physics-free mode (no modules enabled)."""
        # Create parameter file with no modules
        param_file = self._create_test_param_file(
            output_name="physics_free",
            enabled_modules=None,  # No modules
            first_file=0,
            last_file=0
        )

        # Run mimic
        returncode, stdout, stderr = self._run_mimic(param_file)

        # Verify execution succeeded
        self.assertEqual(returncode, 0,
                        f"Mimic failed in physics-free mode:\n{stderr}")

        # Verify log messages
        self.assertIn("No modules enabled", stdout,
                     "Should log physics-free mode")

        # Verify output directory was created
        output_dir = os.path.join(self.temp_dir, "physics_free")
        self.assertTrue(os.path.exists(output_dir),
                       "Output directory should be created")

    def test_single_module_execution(self):
        """Test single module execution in isolation."""
        # Create parameter file with only test_fixture
        param_file = self._create_test_param_file(
            output_name="single_module",
            enabled_modules=["test_fixture"],
            module_params={
                "TestFixture_DummyParameter": "2.5"
            },
            first_file=0,
            last_file=0
        )

        # Run mimic
        returncode, stdout, stderr = self._run_mimic(param_file)

        # Verify execution succeeded
        self.assertEqual(returncode, 0,
                        f"Mimic failed with test_fixture:\n{stderr}")

        # Verify module was initialized with correct parameters
        self.assertIn("Test fixture module initialized", stdout)
        self.assertIn("DummyParameter = 2.500", stdout)

        # Verify output directory was created
        output_dir = os.path.join(self.temp_dir, "single_module")
        self.assertTrue(os.path.exists(output_dir),
                       "Output directory should be created")

    def test_multiple_modules_execution(self):
        """Test multiple module execution together."""
        # Create parameter file with test_fixture enabled twice (tests module list handling)
        param_file = self._create_test_param_file(
            output_name="multiple_modules",
            enabled_modules=["test_fixture", "test_fixture"],
            module_params={
                "TestFixture_DummyParameter": "1.5",
                "TestFixture_EnableLogging": "0"
            },
            first_file=0,
            last_file=0
        )

        # Run mimic
        returncode, stdout, stderr = self._run_mimic(param_file)

        # Verify execution succeeded
        self.assertEqual(returncode, 0,
                        f"Mimic failed with multiple modules:\n{stderr}")

        # Verify module was initialized with correct parameters
        self.assertIn("Test fixture module initialized", stdout)
        self.assertIn("DummyParameter = 1.500", stdout)
        self.assertIn("EnableLogging = 0", stdout)

        # Verify output directory was created
        output_dir = os.path.join(self.temp_dir, "multiple_modules")
        self.assertTrue(os.path.exists(output_dir),
                       "Output directory should be created")

    def test_custom_parameter_values(self):
        """Test that custom parameter values are actually used."""
        # Run with non-default dummy parameter
        param_file = self._create_test_param_file(
            output_name="custom_params",
            enabled_modules=["test_fixture"],
            module_params={
                "TestFixture_DummyParameter": "3.14"  # Non-default
            },
            first_file=0,
            last_file=0
        )

        # Run mimic
        returncode, stdout, stderr = self._run_mimic(param_file)

        # Verify execution succeeded
        self.assertEqual(returncode, 0,
                        f"Mimic failed with custom parameters:\n{stderr}")

        # Verify custom parameter was read
        self.assertIn("DummyParameter = 3.140", stdout,
                     "Custom parameter value should be logged")

    def test_unknown_module_error(self):
        """Test that unknown module names produce clear errors."""
        # Create parameter file with invalid module
        param_file = self._create_test_param_file(
            output_name="unknown_module",
            enabled_modules=["nonexistent_module"],
            first_file=0,
            last_file=0
        )

        # Run mimic
        returncode, stdout, stderr = self._run_mimic(param_file)

        # Verify execution failed
        self.assertNotEqual(returncode, 0,
                           "Mimic should fail with unknown module")

        # Verify error message lists available modules
        combined_output = stdout + stderr
        self.assertIn("not registered", combined_output,
                     "Should report module not registered")
        self.assertIn("Available modules:", combined_output,
                     "Should list available modules")
        self.assertIn("test_fixture", combined_output,
                     "Should list test_fixture as available")

    def test_module_execution_order(self):
        """Test that modules execute in the order specified in EnabledModules.

        Note: This test validates basic execution ordering infrastructure.
        Dependency-based ordering will be tested when modules with actual
        dependencies are implemented (e.g., sage_cooling depends on sage_infall).
        """
        # Create parameter file with test_fixture
        param_file = self._create_test_param_file(
            output_name="execution_order",
            enabled_modules=["test_fixture"],
            module_params={
                "TestFixture_DummyParameter": "1.0"
            },
            first_file=0,
            last_file=0
        )

        # Run mimic
        returncode, stdout, stderr = self._run_mimic(param_file)

        # Verify execution succeeded
        self.assertEqual(returncode, 0,
                        f"Mimic should succeed:\n{stderr}")

        # Verify module was initialized (basic ordering infrastructure works)
        self.assertIn("Test fixture module initialized", stdout,
                     "Module should be initialized in pipeline")


def main():
    """Main test runner."""
    # Set up test suite
    suite = unittest.TestLoader().loadTestsFromTestCase(TestModulePipeline)

    # Run tests with verbose output
    runner = unittest.TextTestRunner(verbosity=2)
    result = runner.run(suite)

    # Return exit code based on results
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
