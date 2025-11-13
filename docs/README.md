# Mimic Documentation

This directory contains comprehensive documentation for Mimic development and architecture.

## Start Here

**New to Mimic?** Start with the **[Getting Started Guide](getting-started.md)** - your entry point for users, developers, and AI coders.

---

## Quick Navigation by Role

### For Users
- **[Getting Started](getting-started.md)** - Setup, running simulations, configuration
- **[Module Configuration](user/module-configuration.md)** - Configure physics modules at runtime
- **[Output Formats](user/output-formats.md)** - Understanding Mimic's output files

### For Developers
- **[Getting Started](getting-started.md)** - Build, test, and development workflow
- **[Module Developer Guide](developer/module-developer-guide.md)** - Creating physics modules
- **[Testing Guide](developer/testing.md)** - Three-tier testing framework
- **[Coding Standards](developer/coding-standards.md)** - Code style and documentation
- **[Execution Flow Reference](developer/execution-flow-reference.md)** - Complete function call trace

### For AI Coders
- **[Getting Started](getting-started.md)** - Essential context and documentation map
- **[Architecture Vision](architecture/vision.md)** - 8 core architectural principles
- **[Roadmap](architecture/roadmap_v4.md)** - Current implementation status

---

## Documentation by Topic

### Architecture & Design
- **[Vision](architecture/vision.md)** - 8 core architectural principles
- **[Roadmap v4](architecture/roadmap_v4.md)** - Implementation status (Phases 1-3 complete)
- **[Property Metadata Schema](architecture/property-metadata-schema.md)** - Property system specification

### Development Guides
- **[Module Developer Guide](developer/module-developer-guide.md)** - Creating physics modules (1100+ lines)
- **[Module Metadata Schema](developer/module-metadata-schema.md)** - Module YAML specification (1200+ lines)
- **[Testing Guide](developer/testing.md)** - Comprehensive testing framework (2100+ lines)
- **[Execution Flow Reference](developer/execution-flow-reference.md)** - Function call trace (1000+ lines)
- **[Coding Standards](developer/coding-standards.md)** - Code style requirements

### User Guides
- **[Module Configuration](user/module-configuration.md)** - Runtime module configuration
- **[Output Formats](user/output-formats.md)** - Binary and HDF5 output specifications

### Physics Documentation
- **[SAGE Infall](physics/sage-infall.md)** - Cosmological infall module

---

## Documentation Structure

```
docs/
├── getting-started.md       ⭐ START HERE - Entry point for all users
├── README.md                This file - Documentation navigation
├── architecture/
│   ├── vision.md            - 8 core architectural principles
│   ├── roadmap_v4.md        - Implementation roadmap (Phases 1-3 complete)
│   └── property-metadata-schema.md - Property system specification
├── developer/
│   ├── module-developer-guide.md   - Creating physics modules
│   ├── module-metadata-schema.md   - Module YAML specification
│   ├── testing.md                  - Comprehensive testing guide
│   ├── execution-flow-reference.md - Function call trace reference
│   └── coding-standards.md         - Code style standards
├── user/
│   ├── module-configuration.md     - Configuring physics modules
│   └── output-formats.md           - Output file formats
└── physics/
    └── sage-infall.md              - SAGE infall module documentation
```

---

## Contributing

When adding new features or modules:
1. Follow the **[Coding Standards](developer/coding-standards.md)**
2. Align with the **[Architectural Vision](architecture/vision.md)**
3. Write comprehensive tests (see **[Testing Guide](developer/testing.md)**)
4. Update relevant documentation
5. Document as you go

---

## Common Questions

**Q: How do I build and run Mimic?**
→ See **[Getting Started](getting-started.md)**

**Q: How do I add a new physics module?**
→ See **[Module Developer Guide](developer/module-developer-guide.md)**

**Q: How do I add a new property?**
→ See **[Property Metadata Schema](architecture/property-metadata-schema.md)** quick start

**Q: How do I configure modules at runtime?**
→ See **[Module Configuration](user/module-configuration.md)**

**Q: Where is the detailed function call trace?**
→ See **[Execution Flow Reference](developer/execution-flow-reference.md)**

**Q: What are Mimic's architectural principles?**
→ See **[Vision](architecture/vision.md)**

**Q: What's the implementation status?**
→ See **[Roadmap](architecture/roadmap_v4.md)**
