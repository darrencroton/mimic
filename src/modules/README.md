# Physics Modules

Runtime-configurable galaxy physics modules.

## Directory Structure

```
src/modules/
├── _archive/          # Archived modules (not compiled)
├── _system/           # Framework infrastructure (don't modify)
│   ├── template/      # Module template - start here for new modules
│   ├── test_fixture/  # Infrastructure testing only
│   ├── generated/     # Auto-generated registration code
│   ├── physical_constants.h   # Universal constants (G, c, Z_sun, etc.)
│   ├── parameter_helpers.h    # Parameter loading/validation macros
│   └── output_helpers.h       # Output formatting utilities
├── _shared/           # Shared physics utilities (see _shared/README.md)
└── [module_name]/     # Production physics modules
```

## What Goes Where

**Physics Modules** (e.g., `my_physics_module/`, `my_different_module_.c`):
- Module directories containing code, tests, and `module_info.yaml`
- Standalone module `.c` files
- Each module is self-contained with its own physics implementation

**Shared Utilities** (`_shared/`):
- Reusable physics calculations shared across multiple modules
- Swappable physics models (e.g., different cooling functions)
- See `_shared/README.md` for creating and using shared utilities

**Framework Infrastructure** (`_system/`):
- DO NOT MODIFY unless adding universal framework features
- Physical constants, helper macros, module template
- See `_system/README.md` for what's available

**Archive** (`_archive/`):
- Historical reference for modules not currently in use
- Not compiled or included in builds

## Creating a New Module

**Quick start**:
1. Copy template `cp -r _system/template/ my_module/` or create standalone `my_module.c`
2. Follow step-by-step instructions in `_system/template/README.md`
3. See comprehensive guide in `docs/DEVELOPER-GUIDE.md`

## Module Development Resources

- **Template**: `_system/template/` with step-by-step README
- **Shared Utilities**: `_shared/` for reusable physics code (see `_shared/README.md`)
- **Developer Guide**: `docs/DEVELOPER-GUIDE.md` - complete module development guide
- **Vision**: `docs/VISION.md` - architectural principles
