# SAGE Shared Utilities

This directory contains helper APIs that are local to the SAGE model package.
They are available when Mimic is built with `MODEL=sage` and should not be
treated as framework-wide conventions for other model sets.

If another model needs similar behavior, copy or reimplement the helper in that
model package and reconcile the property names, units, parameters, and tests
there.

Utility tests are registered in `shared/module_info.yaml`.
