---
name: mimic-plots
description: Working with Mimic plots — adding, running, debugging, or understanding the plotting system. Load when any plot work is in scope.
---

# Mimic Plots

Full reference: `plot/mimic-plot/README.md`. This skill covers the structure, key patterns, and diagnostic approach.

## Plot System Orientation

The plotting system is model-local: each model package owns its figures and profiles under `models/<model>/plots/`. The plotting engine lives in `plot/mimic-plot/`.

```
models/<model>/plots/
  figures/           # Python modules — one file per plot
    __init__.py      # registers figures by name
    halo_mass_function.py
    stellar_mass_function.py
    ...
  profiles/
    default.yaml     # canonical plot list (snapshot + evolution plot names)
    mini-millennium_plot_profile.yaml   # simulation-specific axis overrides
    millennium_plot_profile.yaml
```

**Figures are Python modules, not YAML files.** The profile YAMLs define which figures are active and their display settings; the figures themselves are Python.

## Running Plots

Always activate the virtual environment first:

```bash
source mimic_venv/bin/activate

# All plots for a run
python plot/mimic-plot/mimic-plot.py --param-file models/sage16/input/sage16_mini-millennium.yaml

# Specific plots
python plot/mimic-plot/mimic-plot.py \
  --param-file models/sage16/input/sage16_mini-millennium.yaml \
  --plots halo_mass_function,stellar_mass_function

# Snapshot-only or evolution-only
python plot/mimic-plot/mimic-plot.py --param-file ... --snapshot-plots
python plot/mimic-plot/mimic-plot.py --param-file ... --evolution-plots

deactivate
```

## Plot Profiles and Inheritance

`default.yaml` is the canonical registry — it lists which figure names are active:

```yaml
plots:
  snapshot:
    - halo_mass_function
    - stellar_mass_function
    ...
  evolution:
    - hmf_evolution
    ...
mode: exploration
```

Simulation-specific profiles inherit from `default.yaml` using relative-path `inherits`:

```yaml
inherits:
  - default.yaml
mode: validation
simulation: mini-millennium
axes:
  stellar_mass_function:
    xlim: [7.0, 12.5]
```

The plotting engine discovers the simulation-specific profile by matching `<simulation>_plot_profile.yaml` in the model's `plots/profiles/` directory.

## Adding a New Plot

1. Create `models/<model>/plots/figures/<plot_name>.py` — implement a `plot(data, profile, ax)` function (see any existing figure for the pattern).
2. Register it in `models/<model>/plots/figures/__init__.py` — add to the figure registry dict.
3. Add the name to `models/<model>/plots/profiles/default.yaml` under `snapshot:` or `evolution:`.
4. Test with `--plots <plot_name>`.

## Debugging Skipped Plots

```bash
python plot/mimic-plot/mimic-plot.py --param-file ... --verbose
```

`--verbose` logs why each figure was skipped. The most common cause is a required galaxy-physics field not present in the output (e.g. `StellarMass` is absent when running with `halos-only` MODEL). The `halos-only` package intentionally has a limited plot registry.

## Testing the Plotting System

```bash
source mimic_venv/bin/activate

# Plotting unit tests (validation helpers)
cd plot/mimic-plot/tests
python3 test_validation_helpers.py

# Plotting integration tests
./test_plotting.sh

deactivate
```
