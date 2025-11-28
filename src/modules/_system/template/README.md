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
- Update `your_module_init()` to read model parameters using `model_get_*()`
- Update `your_module_process()` with your physics logic
- Update `your_module_cleanup()` to free any resources

**Note (Phase 4.4)**: All physics parameters are centralized in `src/modules/model_parameters.yaml`. Modules read parameters using `model_get_double()`, `model_get_int()`, etc. No module-specific parameters.

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
    requires: []  # Properties this module needs
    provides: []  # Properties this module creates

  model_parameters_used: []  # Which model parameters this module reads (documentation only)
  # Example: [BaryonFrac, SfrEfficiency]

  tests:
    unit: test_unit_your_module.c
    integration: test_integration_your_module.py
    scientific: test_scientific_your_module_validation.py
```

Module registration is **auto-generated** from this metadata - no manual code needed!

**Note (Phase 4.4)**: The `model_parameters_used` field is for documentation only. Actual parameter definitions are in `src/modules/model_parameters.yaml` (all 20 parameters shared across modules).

### 7. Build and Test

```bash
make                  # Compile
make test-unit        # Run unit tests
make test-integration # Run integration tests
```

### 8. Configure and Run

Add your module to the enabled modules list in your YAML configuration:

```yaml
# Model parameters (all 20 REQUIRED - shown partially here)
model_parameters:
  BaryonFrac: 0.17
  SfrEfficiency: 0.02
  # ... (all other parameters)

# Enable your module
modules:
  enabled:
  - sage_infall
  - sage_cooling
  - your_module  # Add your module here
```

**Note (Phase 4.4)**: Modules no longer have individual parameter sections. All physics parameters are in the centralized `model_parameters:` section. See `input/millennium.yaml` for a complete example.

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
// MODEL PARAMETERS (Phase 4.4: read from centralized model_parameters.yaml)
static double baryon_frac;  // Uses BaryonFrac from model_parameters

static float compute_cooling_rate(float mvir, double redshift) {
    // Physics: accreted baryons cool from hot halo
    float cooling_rate = baryon_frac * mvir / (1.0 + redshift);
    return cooling_rate;
}

static int your_module_init(void) {
    // Read centralized model parameter
    if (model_get_double("BaryonFrac", &baryon_frac) != 0) {
        ERROR_LOG("Failed to read BaryonFrac");
        return -1;
    }
    INFO_LOG("Cooling module initialized with BaryonFrac = %f", baryon_frac);
    // ...
}
```

**Note**: All 20 model parameters are defined in `src/modules/model_parameters.yaml` and REQUIRED in the input file. Your module reads only the parameters it needs.

---

## Next Steps

After customizing the template:

1. **Write Tests**: Add unit, integration, and scientific tests
2. **Document Physics**: Create comprehensive `README.md` in your module directory
3. **Update User Guide**: Add to `docs/user/module-configuration.md`
4. **Update module_info.yaml**: Ensure `docs.physics` points to your module's `README.md`

See `docs/developer/module-developer-guide.md` for comprehensive documentation.

---

## Examples

See working examples:
- `src/modules/simple_cooling/` - Simple gas cooling
- `src/modules/simple_sfr/` - Simple star formation

These follow the same structure as this template.
