# Consistent-Trees Uchuu Validation

**Status:** Active validation record for the Uchuu-family Consistent-Trees readers.
**Scope:** Reader behaviour, package assumptions, and validation gates for `micro-uchuu`, `micro-uchuu-hdf5`, `mini-uchuu`, and `uchuu`, with emphasis on the forests-HDF5 path used by the Uchuu suite.

---

## 1. Reader and Package Boundary

The Consistent-Trees readers are format readers, not simulation-specific code. Simulation packages own cosmology, particle mass, snapshot scale factors, tree paths, and catalog-field metadata; the reader maps the on-disk format into Mimic's generated `RawHalo` structure and lets generated reference-unit accessors apply unit conversion at the reader boundary.

The Uchuu-family packages use two retained input formats: L-Halo binary for `micro-uchuu` and `mini-uchuu`, and forests-HDF5 for `micro-uchuu-hdf5` and `uchuu`. The forests-HDF5 reader is the production path for full Uchuu because the packaged input is a Consistent-Trees HDF5 catalogue.

## 2. Partition and Identity Contract

Both Consistent-Trees readers use `PARTITION_PER_TASK`: each MPI task owns one output partition and processes a contiguous assigned forest range. `UniqueGalaxyID` derives from the run-scoped global forest offset plus the unit-local halo index, so identity is independent of MPI task count and input file partitioning.

The forests-HDF5 reader can weight forest distribution by per-forest halo count because the HDF5 metadata exposes `ForestNhalos` before loading. This balancing choice changes task assignment but must not change output values for a fixed run configuration.

## 3. Value and Unit Conventions

Reader-local conversion is intentionally narrow. The shared Consistent-Trees value convention step (`apply_ctrees_value_conventions`) handles native-catalog conventions such as spin normalisation and particle-count estimates. Unit conversion, including mass and position conversion into Mimic's reference basis, is handled by generated metadata accessors from the selected simulation package.

Do not add simulation-specific unit policy to `read_ctrees_hdf5.c`. Add or correct units in the relevant `simulations/<simulation>/halo_properties.yaml` package and regenerate with the same `MODEL`/`SIMULATION` selectors used for build and tests.

## 4. Forests-HDF5 Read Path

The forests-HDF5 reader now uses a bounded, cache-backed read path:

- task-range `ForestInfo` rows are read once per relevant input file and indexed by forest row during `load_unit_ctrees_hdf5`;
- all 19 halo field datasets are opened once per task-range file, with dataset, filespace, datatype, extent, and element-size metadata cached until partition close;
- schema-level field checks happen when the field cache opens, so malformed extents or datatypes fail before the forest loop;
- normal forests are read from a fixed `CTREES_READ_WINDOW_BYTES` window (`128 MiB` per rank), refilled with one slab `H5Dread` per field when the requested forest leaves the current window;
- forests larger than the window capacity use the cached-handle direct read path, preserving the persistent window bound while still materialising the required forest data.

The read-window optimisation does not change input files, output schemas, run YAML, HDF5 chunking, compression, or physics execution. Link validation, snapshot validation, and Consistent-Trees value conventions still run per forest.

## 5. Validation Gates

For reader changes that affect the Uchuu forests-HDF5 path, use the same `MODEL`/`SIMULATION` selectors across generate, build, tests, and runs. The default focused pair is:

```bash
make MODEL=halos-only SIMULATION=micro-uchuu-hdf5
make MODEL=halos-only SIMULATION=micro-uchuu-hdf5 validate-modules
MODEL=halos-only SIMULATION=micro-uchuu-hdf5 tests/unit/run_tests.sh test_ctrees_hdf5_reader
MODEL=halos-only SIMULATION=micro-uchuu-hdf5 tests/unit/run_tests.sh test_ctrees_support
```

Code changes to the reader must also pass the byte-identical fixture gate for `simulations/micro-uchuu-hdf5/_tests/data/` and `simulations/uchuu/_tests/data/`, plus a `--debug` smoke run with no allocator leaks. For the windowed path, peak memory should include the fixed 128 MiB `MEM_IO` read window plus normal tree/output allocations.

## 6. Current Validation Evidence

The HDF5 read-path optimisation landed in three code slices: task-range `ForestInfo` caching, persistent per-file field handles, and the bounded read window. Final Slice 3 validation passed the focused build, generated-code check, module validation, HDF5 reader unit tests, ctrees support tests, byte-identical Snap payload comparisons for the micro-Uchuu-HDF5 and Uchuu fixtures, and a debug smoke run with no allocator leaks.

No NT production-scale timing was recorded during the implementation slices. The planning benchmark in `docs/dev/MIMIC-HDF5-IO-OPTIMISATION.md` remains the current speedup evidence until a production timing is added.
