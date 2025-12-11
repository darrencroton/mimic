# Module Template

**Purpose**: Boilerplate code for creating new Mimic physics modules

**Status**: Template (do not compile directly)

---

## Quick Start

### 1. Copy Template

```bash
cp -r src/modules/_system/template src/modules/YOUR_MODULE_NAME
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
vim src/modules/model_properties.yaml

# Add your properties:
#   - name: YourProperty
#     type: float
#     units: "1e10 Msun/h"
#     description: "Your physics quantity"
#     output: true
#     init_source: default
#     init_value: 0.0

# Generate code
make generate
```

### 5. Implement Physics

Edit `your_module.c`:
- Update `MODEL PARAMETERS` section - declare variables for the model parameters your module uses
- Implement helper functions for physics calculations
- Update `your_module_init()` to read and validate model parameters (use parameter_helpers.h macros)
- Update `your_module_process()` with your physics logic
- Update `your_module_cleanup()` to free any resources

**Parameter Loading**: Modules read parameters from the input YAML file using `model_get_*()` functions or the convenient helper macros from `parameter_helpers.h`. All parameters must be declared in your `module_info.yaml` under `dependencies.parameters`.

### 6. Create Module Metadata

Create `module_info.yaml` in your module directory (see existing modules for examples):

```yaml
module:
  name: your_module
  display_name: "Your Module"
  description: "Brief description"
  version: "1.0.0"
  author: "Your Name"
  category: gas_physics

  sources:
    - your_module.c
  headers:
    - your_module.h
  register_function: your_module_register

  dependencies:
    # All halo and galaxy properties read or written by this module
    properties:
      - HotGas
      - ColdGas

    # All model parameters accessed via model_get_*() functions
    parameters:
      - BaryonFrac
      - SfrEfficiency

  tests:
    unit: test_unit_your_module.c
    integration: test_integration_your_module.py
    scientific: test_scientific_your_module_validation.py
```

Module registration is **auto-generated** from this metadata - no manual code needed!

**Important**: The `dependencies.parameters` list declares which parameters your module uses. All parameters must be specified in the input YAML file. The parameter linter (`make lint-parameters`) verifies this list matches actual usage.

### 7. Build and Test

```bash
make                  # Compile
make test-unit        # Run unit tests
make test-integration # Run integration tests
```

### 8. Configure and Run

Add your module to the enabled modules list in your YAML configuration:

```yaml
# Enable your module and configure parameters
modules:
  enabled:
  - sage_calculate_infall
  - sage_cooling
  - your_module  # Add your module here
  parameters:
    # All parameters REQUIRED - shown partially here
    BaryonFrac: 0.17
    SfrEfficiency: 0.02
    # ... (add all parameters used by enabled modules)
```

**Important**: All parameters used by your module must be specified in the `modules.parameters:` section. No defaults are provided. See `input/millennium.yaml` for a complete example.

Run:

```bash
./mimic input/millennium.yaml
```

---

## Template Structure

### Header File (`template_module.h`)

- Module interface declaration
- Documentation for users of the module
- Single public function: `template_module_register()`

### Implementation File (`template_module.c`)

Structured in sections:

1. **MODEL PARAMETERS**: Static variables for model parameters this module uses
2. **MODULE STATE**: Persistent data (lookup tables, caches, etc.)
3. **HELPER FUNCTIONS**: Pure physics calculations (testable independently)
4. **MODULE LIFECYCLE FUNCTIONS**:
   - `init()`: Initialize once at startup (read model parameters)
   - `process()`: Process each FOF group
   - `cleanup()`: Cleanup at shutdown
5. **MODULE REGISTRATION**: Register with module system

---

## What to Modify

### Essential Changes

These sections **must** be updated:

- [ ] File documentation (description, physics equations, references)
- [ ] Module name in `struct Module`
- [ ] `init()`: Model parameter reading using `model_get_*()` functions
- [ ] `process()`: Physics calculations
- [ ] Helper functions: Implement your physics logic
- [ ] Property reads/writes: Use actual properties from your physics
- [ ] MODEL PARAMETERS section: Declare variables for parameters your module needs

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
// MODEL PARAMETERS
static double example_param1;

static float compute_physics(float input1, double input2) {
    float result = example_param1 * input1 * input2;
    return result;
}

static int template_module_init(void) {
    if (model_get_double("ExampleParam1", &example_param1) != 0) {
        ERROR_LOG("Failed to read ExampleParam1");
        return -1;
    }
    // ...
}
```

### After (Custom Cooling Module)
```c
#include "../_system/parameter_helpers.h"

// MODEL PARAMETERS
static double baryon_frac;  // Read from input YAML via model_get_double()

static float compute_cooling_rate(float mvir, double redshift) {
    // Physics: accreted baryons cool from hot halo
    float cooling_rate = baryon_frac * mvir / (1.0 + redshift);
    return cooling_rate;
}

static int your_module_init(void) {
    // Load and validate parameter in one call
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BaryonFrac", baryon_frac, 0.0, 1.0,
                                      "cosmic baryon fraction must be physical");

    INFO_LOG("Cooling module initialized with BaryonFrac = %f", baryon_frac);
    // ...
}
```

**Note**: All parameters used by your module must be specified in the input YAML file (no defaults). Declare them in your `module_info.yaml` under `dependencies.parameters`.

---

## Next Steps

After customizing the template:

1. **Write Tests**: Add unit, integration, and scientific tests
2. **Document Physics**: Create comprehensive `README.md` in your module directory
3. **Update User Guide**: Module configuration is now in `docs/USER-GUIDE.md` (Configuration section)
4. **Update module_info.yaml**: Ensure `docs.physics` points to your module's `README.md`

See `docs/DEVELOPER-GUIDE.md` for comprehensive documentation.

---

## Examples

See working examples:
- `src/modules/simple_cooling/` - Simple gas cooling
- `src/modules/simple_sfr/` - Simple star formation

These follow the same structure as this template.
