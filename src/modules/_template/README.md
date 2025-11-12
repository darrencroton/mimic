# Module Template

**Purpose**: Boilerplate code for creating new Mimic physics modules

**Status**: Template (do not compile directly)

---

## Quick Start

### 1. Copy Template

```bash
cp -r src/modules/_template src/modules/YOUR_MODULE_NAME
cd src/modules/YOUR_MODULE_NAME
```

### 2. Rename Files

```bash
mv template_module.h your_module.h
mv template_module.c your_module.c
rm README.md  # Remove this file
```

### 3. Find and Replace

In both `.h` and `.c` files:
- Replace `template_module` → `your_module` (all occurrences)
- Replace `TEMPLATE_MODULE` → `YOUR_MODULE`
- Update file documentation (author, description, physics equations)

### 4. Define Properties (if needed)

If your module creates new galaxy properties:

```bash
# Edit metadata
vim metadata/properties/galaxy_properties.yaml

# Add your properties:
#   - name: YourProperty
#     type: float
#     units: "1e10 Msun/h"
#     description: "Your physics quantity"
#     output: true
#     created_by: your_module
#     init_source: default
#     init_value: 0.0

# Generate code
make generate
```

### 5. Implement Physics

Edit `your_module.c`:
- Update `MODULE PARAMETERS` section with your parameters
- Implement helper functions for physics calculations
- Update `your_module_init()` to read and validate parameters
- Update `your_module_process()` with your physics logic
- Update `your_module_cleanup()` to free any resources

### 6. Register Module

Edit `src/modules/module_init.c`:

```c
#include "your_module/your_module.h"

void register_all_modules(void) {
    simple_cooling_register();
    simple_sfr_register();
    your_module_register();  // Add this line
}
```

### 7. Build and Test

```bash
make                  # Compile
make test-unit        # Run unit tests
make test-integration # Run integration tests
```

### 8. Configure and Run

Add to your `.par` file:

```
EnabledModules  simple_cooling,simple_sfr,your_module
YourModule_Parameter1  1.5
YourModule_Parameter2  0.8
```

Run:

```bash
./mimic input/millennium.par
```

---

## Template Structure

### Header File (`template_module.h`)

- Module interface declaration
- Documentation for users of the module
- Single public function: `template_module_register()`

### Implementation File (`template_module.c`)

Structured in sections:

1. **MODULE PARAMETERS**: Static variables for configurable parameters
2. **MODULE STATE**: Persistent data (lookup tables, caches, etc.)
3. **HELPER FUNCTIONS**: Pure physics calculations (testable independently)
4. **MODULE LIFECYCLE FUNCTIONS**:
   - `init()`: Initialize once at startup
   - `process()`: Process each FOF group
   - `cleanup()`: Cleanup at shutdown
5. **MODULE REGISTRATION**: Register with module system

---

## What to Modify

### Essential Changes

These sections **must** be updated:

- [ ] File documentation (description, physics equations, references)
- [ ] Module name in `struct Module`
- [ ] `init()`: Parameter reading and validation
- [ ] `process()`: Physics calculations
- [ ] Helper functions: Implement your physics logic
- [ ] Property reads/writes: Use actual properties from your physics

### Optional Changes

These sections are optional depending on your module:

- [ ] MODULE STATE: Add persistent data if needed
- [ ] `cleanup()`: Add resource cleanup if you allocated memory/files
- [ ] Additional helper functions for complex physics

### What NOT to Change

These sections should not need changes:

- Module interface structure (follows standard pattern)
- Function signatures (match `module_interface.h`)
- Error handling patterns (standard across all modules)
- Logging style (use INFO_LOG/DEBUG_LOG/ERROR_LOG)

---

## Example Customization

From template to a simple "gas cooling" module:

### Before (Template)
```c
static double PARAM1 = 1.0;
static float compute_physics(float input1, double input2) {
    float result = PARAM1 * input1 * input2;
    return result;
}
```

### After (Custom)
```c
static double COOLING_EFFICIENCY = 0.15;
static float compute_cooling_rate(float mvir, double redshift) {
    float cooling_rate = COOLING_EFFICIENCY * mvir / (1.0 + redshift);
    return cooling_rate;
}
```

---

## Next Steps

After customizing the template:

1. **Write Tests**: Add unit, integration, and scientific tests
2. **Document Physics**: Create `docs/physics/your-module.md`
3. **Update User Guide**: Add to `docs/user/module-configuration.md`
4. **Log Lessons**: Document in `docs/architecture/module-implementation-log.md`

See `docs/developer/module-developer-guide.md` for comprehensive documentation.

---

## Examples

See working examples:
- `src/modules/simple_cooling/` - Simple gas cooling
- `src/modules/simple_sfr/` - Simple star formation

These follow the same structure as this template.
