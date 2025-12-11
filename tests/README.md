# Mimic Test Suite

Quick reference for running tests. See [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md#testing) for complete testing documentation.

## Quick Start

```bash
# Run all tests (from mimic root directory)
make tests

# Run specific test tiers
make test-unit          # C unit tests (<10s)
make test-integration   # Python integration tests (<1min)
make test-scientific    # Physics validation (<5min)
```

## Test Tiers

**Unit Tests** (`tests/unit/`)
- C-based tests for individual functions and modules
- Fast (<10 seconds total)
- Test infrastructure: memory management, I/O, core functionality
- Test modules: individual module unit tests

**Integration Tests** (`tests/integration/`)
- Python-based end-to-end workflow tests
- Medium speed (<1 minute total)
- Test full pipeline execution, output format compatibility
- Use test data from `tests/data/`

**Scientific Tests** (`tests/scientific/`)
- Python-based physics validation
- Slower (<5 minutes total)
- Validate physics accuracy against published SAGE results
- Test mass/metal conservation, scientific correctness

## Running Individual Tests

**Unit tests**:
```bash
cd tests/unit
./test_memory_system.test
./test_unit_sage_cooling.test
```

**Integration tests**:
```bash
cd tests/integration
python test_full_pipeline.py
python test_output_formats.py
```

**Scientific tests**:
```bash
cd tests/scientific
python test_scientific.py
```

## Directory Structure

```
tests/
├── unit/               # C unit tests
├── integration/        # Python integration tests
├── scientific/         # Physics validation tests
├── framework/          # Shared test utilities
│   └── data_loader.py  # Binary/HDF5 data loading
├── data/               # Test input files
└── generated/          # Auto-generated test metadata
```

## Test Data

Test suite uses mini-Millennium simulation data, automatically downloaded by `./scripts/first_run.sh`.

Location: `input/data/millennium/`

## Writing Tests

See [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md#testing) for:
- Writing unit tests
- Writing integration tests
- Writing scientific tests
- Test framework utilities

## Troubleshooting

**Tests fail after code changes**: Run `make clean && make` before testing

**Missing test data**: Run `./scripts/first_run.sh` to download

**Integration tests fail**: Ensure Python environment activated (`source mimic_venv/bin/activate`)

**Need more detail**: See [docs/DEVELOPER-GUIDE.md](../docs/DEVELOPER-GUIDE.md#testing)
