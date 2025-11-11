# Mimic Documentation

This directory contains comprehensive documentation for Mimic development and architecture.

## Quick Navigation

### For New Developers
- **[Getting Started](developer/getting-started.md)** - Build, run, and test Mimic
- **[Coding Standards](developer/coding-standards.md)** - Documentation templates and code style

### Understanding the Architecture
- **[Vision](architecture/vision.md)** - 8 core architectural principles guiding Mimic's design
- **[Roadmap v4](architecture/roadmap_v4.md)** - Implementation roadmap (Phases 1-3 complete)
- **[Execution Flow](developer/execution-flow-reference.md)** - Complete function call trace from entry to exit

## Documentation Structure

```
docs/
├── README.md (this file)
├── architecture/
│   ├── vision.md            - Architectural principles and design philosophy
│   ├── roadmap_v4.md        - Implementation roadmap (Phases 1-3 complete)
│   └── property-metadata-schema.md - Property system specification
├── developer/
│   ├── getting-started.md   - Build, run, and test instructions
│   ├── coding-standards.md  - Documentation and code style standards
│   ├── testing.md           - Comprehensive testing guide
│   └── execution-flow-reference.md - Function call trace reference
└── user/
    ├── module-configuration.md - Configuring physics modules at runtime
    └── output-formats.md    - Binary and HDF5 output format guide
```

## Contributing

When adding new features or modules:
1. Follow the [coding standards](developer/coding-standards.md)
2. Align with the [architectural vision](architecture/vision.md)
3. Update relevant documentation
4. Ensure comprehensive function documentation

## Questions?

- Architecture questions → See [vision.md](architecture/vision.md)
- Implementation questions → See [roadmap_v4.md](architecture/roadmap_v4.md)
- Code flow questions → See [execution-flow-reference.md](developer/execution-flow-reference.md)
- Module configuration → See [module-configuration.md](user/module-configuration.md)
- Testing → See [testing.md](developer/testing.md)
