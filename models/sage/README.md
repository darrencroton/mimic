# SAGE Model Package

This package contains the SAGE physics implementation for Mimic:

- `model_properties.yaml`: SAGE galaxy/model property metadata
- `modules/`: runtime physics modules and module-owned tests
- `shared/`: SAGE-specific helper APIs
- `plots/`: SAGE figure implementations and plot profiles
- `validation/`: reference outputs, tolerances, and validation assets

Build this model set with:

```bash
make MODEL=sage
```
