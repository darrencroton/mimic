# SAGE Model Test Fixtures

This directory contains SAGE-owned developer test fixtures that are broader than any single runtime module.

General run YAMLs live in this directory:

- `test_binary.yaml`: physics-free binary-output fixture for core, integration, and scientific tests.
- `test_hdf5.yaml`: physics-free HDF5-output fixture for output-format and baseline regeneration tests.
- `test_uniquegalid.yaml`: two-snapshot fixture for UniqueGalaxyID persistence tests.

Keep user-facing SAGE run configurations in `models/sage/input/`. Keep module-pipeline-specific fixtures, such as the full SAGE physics baseline input, under `models/sage/modules/_tests/input/`.
