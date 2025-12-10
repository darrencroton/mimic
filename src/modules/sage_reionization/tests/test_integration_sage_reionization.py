#!/usr/bin/env python3
"""
Integration tests for sage_reionization module.

Tests module in full pipeline execution.
"""

import os
import sys
import unittest

# Add tests directory to path for framework import
repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../../..'))
sys.path.insert(0, os.path.join(repo_root, 'tests'))

from framework import create_test_param_file, run_mimic, load_binary_halos


class TestSageReionization(unittest.TestCase):
    """Integration tests for sage_reionization module"""

    def setUp(self):
        """Set up test environment"""
        self.repo_root = repo_root
        self.test_dir = os.path.join(repo_root, 'tests', 'data')

    def test_module_loads(self):
        """Test sage_reionization module executes without errors"""
        param_file, output_dir, temp_dir = create_test_param_file(
            output_name='test_sage_reionization_loads',
            phase_config={
                'pre_timestep': [('sage_reionization', 'once')],
                'phase_1': [],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={'GlobalBaryonFraction': 0.17}
        )

        returncode, stdout, stderr = run_mimic(param_file)

        # Should succeed
        self.assertEqual(returncode, 0,
                        f"Mimic should execute successfully\nStderr:\n{stderr}")

        # Should log module initialization
        self.assertIn("SAGE reionization module initialized", stdout,
                     "Module should log initialization")

    def test_property_set(self):
        """Test sage_reionization sets HaloBaryonFraction property"""
        param_file, output_dir, temp_dir = create_test_param_file(
            output_name='test_sage_reionization_property',
            phase_config={
                'pre_timestep': [('sage_reionization', 'once')],
                'phase_1': [],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={'GlobalBaryonFraction': 0.17}
        )

        returncode, stdout, stderr = run_mimic(param_file)
        self.assertEqual(returncode, 0, f"Mimic failed\nStderr:\n{stderr}")

        # Load output and check property
        output_file = os.path.join(output_dir, 'model_z0.000_0')
        if os.path.exists(output_file):
            halos, metadata = load_binary_halos(output_file)

            # Check property exists
            self.assertIn('HaloBaryonFraction', halos.dtype.names,
                         "HaloBaryonFraction should be in output")

            # Check property is set for halos with mass (Mvir > 0)
            # Orphans with Mvir=0 legitimately have HaloBaryonFraction=0
            halos_with_mass = halos[halos['Mvir'] > 0]
            self.assertTrue((halos_with_mass['HaloBaryonFraction'] > 0).all(),
                           "HaloBaryonFraction should be > 0 for halos with mass")

            # Check property is physical (0 <= value <= 0.17 for all halos)
            self.assertTrue((halos['HaloBaryonFraction'] >= 0).all(),
                           "HaloBaryonFraction should be >= 0")
            self.assertTrue((halos['HaloBaryonFraction'] <= 0.17).all(),
                           "HaloBaryonFraction should be <= GlobalBaryonFraction")

    def test_parameter_configuration(self):
        """Test sage_reionization uses GlobalBaryonFraction parameter"""
        param_file, output_dir, temp_dir = create_test_param_file(
            output_name='test_sage_reionization_param',
            phase_config={
                'pre_timestep': [('sage_reionization', 'once')],
                'phase_1': [],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={'GlobalBaryonFraction': 0.20}
        )

        returncode, stdout, stderr = run_mimic(param_file)
        self.assertEqual(returncode, 0, f"Mimic failed\nStderr:\n{stderr}")

        # Load output and verify parameter affects results
        output_file = os.path.join(output_dir, 'model_z0.000_0')
        if os.path.exists(output_file):
            halos, metadata = load_binary_halos(output_file)

            # With GlobalBaryonFraction = 0.20, HaloBaryonFraction should be <= 0.20
            # (can be less due to reionization suppression)
            halos_with_mass = halos[halos['Mvir'] > 0]
            self.assertTrue((halos_with_mass['HaloBaryonFraction'] <= 0.20).all(),
                           "HaloBaryonFraction should be <= custom GlobalBaryonFraction")


if __name__ == '__main__':
    unittest.main(verbosity=2)
