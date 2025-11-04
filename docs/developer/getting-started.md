# Getting Started

## Building

To compile SAGE:

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

## Running

Basic execution:

```bash
./sage input/millennium.par
```

With command-line options:

```bash
./sage --verbose input/millennium.par
./sage --quiet input/millennium.par
./sage --skip input/millennium.par
```

## Code Style

See [coding-standards.md](coding-standards.md) for code style requirements.

## Directory Structure

- `src/core/` - Core execution (main, initialization, model building)
- `src/io/` - Input/output operations (tree readers, output writers)
- `src/util/` - Utility functions (memory, error handling, etc.)
- `src/modules/` - Physics modules (halo properties, etc.)
- `src/include/` - Public headers (types, globals, constants)
- `tests/` - Test framework (future)
- `docs/` - Documentation

## Testing

After making changes, always verify:

```bash
make clean && make
./sage input/millennium.par
```

Check the exit code to ensure success:

```bash
echo $?
# Should output 0 for success
```

## Additional Resources

- [Architecture Vision](../architecture/vision.md)
- [Coding Standards](coding-standards.md)
