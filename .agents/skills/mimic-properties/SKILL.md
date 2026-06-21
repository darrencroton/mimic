---
name: mimic-properties
description: Working with Mimic's property system — adding, modifying, or understanding galaxy, halo, catalog, and output properties. Load when property YAML files or generated property code is in scope.
---

# Mimic Properties

Full reference: `docs/DEVELOPER-GUIDE.md` (property system section) and `scripts/generate_properties.py`. This skill covers the key schema and workflow.

## Property Types and Locations

| Type | YAML file | Scope |
|---|---|---|
| Core halo | `src/core/core_properties.yaml` | All models and simulations |
| Catalog halo | `simulations/<SIM>/halo_properties.yaml` | Simulation-specific catalog fields |
| Galaxy/model | `models/<MODEL>/model_properties.yaml` | Model-specific galaxy fields |

## Property YAML Schema

```yaml
- name: StellarMass           # required: C identifier, PascalCase for galaxy properties
  type: float                 # required: float, double, int, long, char[N]
  units: 1e10 Msun/h          # required: reference unit string
  description: "..."          # required
  output: true                # required: whether written to output files

  # Common optional fields:
  init_source: zero           # how to initialise each timestep: zero, inherit, copy, none
  init_repeat: false          # true = carry value forward without reinitialising (transport property)
  output_source: galaxy_property  # see Output Source below
  output_convert: 1.0         # multiply by this scalar on output
  h_convention: carried       # carried | free | none — see Unit Conventions
  range: [0.0, 1e15]          # physical range for validation
  sentinels: [-1.0]           # special values that bypass range checks
```

## Output Source Values

`output_source` controls how the field value is written:

| Value | Meaning |
|---|---|
| `galaxy_property` | Read directly from the galaxy struct field |
| `copy_direct` | Copy a scalar from another location |
| `copy_direct_array` | Copy an array element |
| `recalculate` | Call a custom output helper function |
| `conditional` | Write one of two sources based on a condition |
| `custom` | Fully custom output logic |

Helper functions for recalculate/custom live in `src/module_system/output_helpers.h`.

## Transport Properties

A property that carries inter-module state without being output:

```yaml
- name: CoolingGas
  output: false
  init_repeat: true           # value persists across timesteps until explicitly overwritten
```

## Unit Conventions

Mimic uses fixed reference units: `Mpc/h`, `1e10 Msun/h`, `km/s`.

`h_convention` declares how h appears in the stored value:
- `carried` — value is in units that include h (e.g. `Mpc/h`) — most catalog properties
- `free` — value is in physical units (h already divided out)
- `none` — dimensionless or h not applicable

## Catalog Properties and Core Role Binding

Catalog properties that the core requires (e.g. `Mvir`, `Vvir`) must be bound via `provides_core_role`:

```yaml
- name: Mvir_200c
  provides_core_role: Mvir    # binds this catalog field to the Mvir core accessor
```

This allows simulation packages to rename columns without breaking core code.

## Dimensional Model Parameters

Model parameters with physical dimensions (e.g. a mass threshold) need a declaration in `models/<model>/parameter_units.yaml`:

```yaml
parameters:
  - name: ShamMinMpeak
    type: double
    units: 1e10 Msun/h
    h_convention: carried
```

Then use `LOAD_PARAM_DOUBLE_INTERNAL` in the module `init()` to get automatic unit conversion. Parameters not declared here are loaded as-is with `LOAD_PARAM_DOUBLE`.

## Adding a Property

1. Edit the appropriate YAML file.
2. Run `make generate` (or `make MODEL=X SIMULATION=Y generate` for non-default packages).
3. Run `make check-generated` to verify freshness (CI also checks this).
4. Build and use the generated struct field in module code.

The generated field is immediately accessible in the galaxy/halo struct — no manual struct editing needed.
