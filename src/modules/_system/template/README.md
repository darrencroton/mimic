# Module Template

Quick start template for creating new Mimic physics modules.

**For complete documentation, see:**
- **[docs/DEVELOPER-GUIDE.md](../../../docs/DEVELOPER-GUIDE.md)** - Complete module development guide
- **[docs/REFERENCE.md](../../../docs/REFERENCE.md)** - Full metadata schema reference
- **[docs/VISION.md](../../../docs/VISION.md)** - Architecture principles

---

## Quick Start

### Option 1: Standalone Module (simplest)

For simple single-file modules:

```bash
# Just copy the .c file
cp src/modules/_system/template/template_module.c src/modules/my_module.c

# Edit my_module.c:
# 1. Change function names: template_module_* → my_module_*
# 2. Implement your physics
# 3. Build and run
make generate
make
```

**No `module_info.yaml` needed!** Standalone modules are auto-discovered.

### Option 2: Directory Module (recommended)

For multi-file modules with tests and documentation:

```bash
# Copy the entire template directory
cp -r src/modules/_system/template src/modules/my_module
cd src/modules/my_module

# Rename files
mv template_module.c my_module.c
mv template_module_info.yaml module_info.yaml
rm README.md  # Delete this template README

# Edit files:
# 1. Update module_info.yaml (change name, uncomment needed fields)
# 2. Edit my_module.c (change function names, implement physics)
# 3. Build and run
make generate
make
```

---

## What You Need to Implement

**Three functions** (naming convention strictly enforced):

```c
int my_module_init(void);           // Load parameters, initialize
int my_module_process(...);         // Implement physics
int my_module_cleanup(void);        // Free memory
```

**That's it.** Registration is automatic.

---

## Key Points

### Include Paths

**Directory modules** (`src/modules/my_module/my_module.c`):
```c
#include "_system/parameter_helpers.h"
#include "_shared/my_utility.h"
```

**Standalone modules** (`src/modules/my_module.c`):
```c
#include "../_system/parameter_helpers.h"
#include "_shared/my_utility.h"
```

### Loading Parameters

```c
// Load and validate in one call
LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("MyParam", my_param, 0.0, 1.0, "description");
LOAD_PARAM_INT("MyOption", my_option);
```

See `src/modules/_system/parameter_helpers.h` for all macros.

### Accessing Properties

```c
// Read input
float cold_gas = gal->ColdGas;

// Write output
gal->StellarMass += delta_mass;
```

Properties defined in `src/modules/model_properties.yaml`.

### Time Integration

```c
// Use ctx->substep_dt for time integration
double dt = ctx->substep_dt;
float delta = rate * dt;
```

---

## Next Steps

1. **Copy template** (standalone or directory)
2. **Rename files and functions**
3. **Implement your physics**
4. **Add to input YAML**:
   ```yaml
   modules:
     phase_1:
       - my_module: process_by_galaxy
   ```
5. **Build and test**:
   ```bash
   make generate
   make
   ./mimic input/your_config.yaml
   ```

**That's it!** For detailed examples and advanced topics, see the docs linked at the top.
