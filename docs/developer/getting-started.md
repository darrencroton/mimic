# Getting Started

## Building

To compile Mimic:

```bash
make clean && make
```

To enable HDF5 support:

```bash
make clean && make USE-HDF5=yes
```

To enable MPI support:

```bash
make clean && make USE-MPI=yes
```

### Property Metadata System

After editing property metadata files (`metadata/properties/*.yaml`), regenerate C code:

```bash
make generate
```

To verify generated code is current (CI check):

```bash
make check-generated
```

## Running

Basic execution:

```bash
./mimic input/millennium.par
```

With command-line options:

```bash
./mimic --verbose input/millennium.par
./mimic --quiet input/millennium.par
./mimic --skip input/millennium.par
```

## Code Style

See [coding-standards.md](coding-standards.md) for code style requirements.

## Directory Structure

- `src/core/` - Core execution (main, initialization, model building)
  - `src/core/halo_properties/` - Halo virial calculations and properties
- `src/io/` - Input/output operations (tree readers, output writers)
- `src/util/` - Utility functions (memory, error handling, etc.)
- `src/modules/` - Physics modules (currently empty, will contain galaxy physics)
- `src/include/` - Public headers (types, globals, constants)
  - `src/include/generated/` - Auto-generated property code (from metadata)
- `metadata/properties/` - Property metadata YAML files (single source of truth)
- `scripts/` - Code generation and development tools
- `tests/` - Test framework (future)
- `docs/` - Documentation

## Testing

After making changes, always verify:

```bash
make clean && make
./mimic input/millennium.par
```

Check the exit code to ensure success:

```bash
echo $?
# Should output 0 for success
```

## Additional Resources

- [Architecture Vision](../architecture/vision.md)
- [Coding Standards](coding-standards.md)
