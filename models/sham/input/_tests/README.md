# SHAM Model Test Fixtures

This directory contains SHAM-owned developer test fixtures that are broader than any single runtime module.

General run YAMLs live in this directory:

- `test_binary.yaml`: physics-free binary-output fixture for core, integration, and scientific tests.
- `test_hdf5.yaml`: physics-free HDF5-output fixture for output-format and baseline regeneration tests.
- `test_uniquegalid.yaml`: two-snapshot fixture for UniqueGalaxyID persistence tests.

Keep user-facing SHAM run configurations in `models/sham/input/`.
