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

## Developer Workflow: Adding a New Property

The Property Metadata System provides a streamlined workflow for adding new halo or galaxy properties. Instead of manually updating 8+ files, you edit a single YAML file and regenerate code.

### Workflow Steps

1. **Edit the metadata file:**
   - For halo properties: `metadata/properties/halo_properties.yaml`
   - For galaxy properties: `metadata/properties/galaxy_properties.yaml`

2. **Define your property** using the schema (see [property-metadata-schema.md](../architecture/property-metadata-schema.md)):
   ```yaml
   - name: MyNewProperty
     type: float
     output: true
     init_source: default
     init_value: 0.0f
     units: "dimensionless"
     description: "Brief description of what this property represents"
   ```

3. **Regenerate code:**
   ```bash
   make generate
   ```

4. **Compile and test:**
   ```bash
   make clean && make
   ./mimic input/millennium.par
   ```

5. **Verify:**
   ```bash
   make check-generated  # Ensures generated code is current
   ```

**Result**: Your new property is automatically added to:
- `struct Halo` / `struct GalaxyData` (processing)
- `struct HaloOutput` (file output)
- HDF5 field definitions
- Python dtype (plotting)
- Initialization code

**Time**: <2 minutes (vs 30 minutes manual implementation)

## Additional Resources

- [Architecture Vision](../architecture/vision.md)
- [Coding Standards](coding-standards.md)
- [Property Metadata Schema](../architecture/property-metadata-schema.md)
