---
name: mimic-tests
description: Working with Mimic tests — writing, running, fixing, or investigating unit, integration, and scientific tests. Load when any test work is in scope.
---

# Mimic Tests

Full reference: `docs/DEVELOPER-GUIDE.md` (testing section). This skill covers the decision rules, locations, and critical patterns.

## Test Tiers

| Tier | Language | Scope | Speed |
|---|---|---|---|
| Unit | C | Physics/math via direct function calls — no full pipeline | Fast |
| Integration | Python | Full Mimic execution via run file | Up to 3 min |
| Scientific | Python | Physics output validation against reference data | ~30s |

## Location Decision

| Location | What it tests | Valid for |
|---|---|---|
| `tests/integration/test_*.py` | Core framework (execution order, substeps, processing modes) | Any MODEL/SIMULATION — use `test_fixture` |
| `models/<model>/modules/_tests/test_*.py` | Model-specific physics | Only the owning MODEL |
| `simulations/<sim>/_tests/integration/test_*.py` | Simulation reader behaviour | Only the owning SIMULATION |

## Starting a New Test

Copy the appropriate template — never start from scratch:
- C unit: `tests/framework/c_unit_test_template.c`
- Python integration: `tests/framework/python_integration_test_template.py`
- Python scientific: `tests/framework/python_scientific_test_template.py`

Register in `module_info.yaml`:
```yaml
tests:
  unit: _tests/test_unit_my_module.c
  integration: _tests/test_integration_my_module.py
  scientific: []
```

Test manifests are written to `build/generated/` (not `tests/`) by `make generate`.

Model-neutral C unit tests live directly in `tests/unit/test_*.c` and are auto-discovered (globbed) by the registry generator — no `module_info.yaml` entry. If such a test exercises a `src/` file that is not already linked, add that file to the shared-source lists in `tests/unit/run_tests.sh` (`UTIL_SRCS` / `CORE_SRCS` / `IO_SRCS`), including any transitive dependency it needs to resolve at link time.

Full selected-model test registration is enabled for `mini-millennium`, `micro-uchuu`, `micro-uchuu-hdf5`, and `micro-uchuu-ascii`. Larger simulations run core and selected-simulation tests only; they should use compact fixtures and rely on the default plus micro-Uchuu validation matrix for full model physics coverage.

Generated test inputs always embed `first_file: 0, last_file: 0` so test runs process a single tree partition regardless of how many files the production catalogue has. For the three micro-Uchuu packages the production `simulation_info.yaml` is used as the simulation config (so tests exercise the real L-Halo binary, Consistent-Trees HDF5, and Consistent-Trees ASCII reader paths), but the single-file cap still applies. Other simulations should provide `simulations/<sim>/_tests/input/test_simulation.yaml` fixtures when production data is too large or not reliably mounted.

## Model-Neutral Integration Tests

Use `test_fixture` when the test must pass for any MODEL/SIMULATION:

```python
from tests.framework import create_test_param_file, run_mimic

param_file = create_test_param_file(
    phase_config=[("test_fixture", "process_full_halo")],
    model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 1},
)
# TestFixtureDummyParameter and TestFixtureEnableLogging are always required
```

`create_test_param_file()` reads from `build/generated/test_inputs/<MODEL>/<SIMULATION>/core/test_binary.yaml`. `MODEL` and `SIMULATION` are exported by the Makefile test runner; set them explicitly when running outside make:

```bash
MODEL=sage16 SIMULATION=mini-millennium python3 tests/integration/test_foo.py
```

## Model-Specific Integration Tests

Use the module name in `phase_config` instead of `test_fixture`. The model name validation in `read_parameter_file.c` enforces the test only runs for the correct MODEL.

## C Test Pattern

```c
#include "tests/framework/test_framework.h"

// Test statistics (required by TEST_RUN)
static int passed = 0;
static int failed = 0;

static int test_my_function(void) {
  // SETUP
  // ...

  // EXECUTE
  // ...

  // VALIDATE
  TEST_ASSERT(result == expected, "description");

  // CLEANUP
  // ...
  return TEST_PASS;
}

static int test_requires_unavailable_feature(void) {
  return TEST_SKIP_WITH("requires unavailable feature");
}

int main(void) {
  TEST_RUN(test_my_function);
  TEST_RUN(test_requires_unavailable_feature);
  TEST_SUMMARY();
  return TEST_RESULT();
}
```

`TEST_RUN` emits `MIMIC_RESULT: PASS/FAIL/SKIP` automatically. A C test function returns `TEST_PASS`, `TEST_FAIL`, or `TEST_SKIP_WITH("reason")`; `main()` returns `TEST_RESULT()`.

## Python Test Pattern

```python
from tests.framework import result_pass, result_fail, result_skip, result_error, TestSkipped

def test_my_behaviour(param_file):
    try:
        # raise TestSkipped("reason") to skip cleanly
        output = run_mimic(param_file)
        assert some_condition(output)
        result_pass("test_my_behaviour")
    except TestSkipped as e:
        result_skip("test_my_behaviour", str(e))
    except Exception as e:
        result_fail("test_my_behaviour", str(e))
```

## Structured Marker Protocol

All tests emit `MIMIC_RESULT: PASS/FAIL/SKIP/WARN/ERROR <test_name>` to stdout. Summary mode suppresses PASS and shows everything else. New tests must emit these markers via the framework helpers — never write them manually.

## Running Tests

```bash
# Full suite with summary (suppresses passing tests)
make tests summary

# Single C unit test
tests/unit/run_tests.sh test_my_module

# Single Python test (outside make)
MODEL=sage16 SIMULATION=mini-millennium python3 models/sage16/modules/my_module/_tests/test_integration_my_module.py

# Long-running — delegate to a subagent and capture output
mkdir -p archive/test-logs
make tests-unit > archive/test-logs/tests-unit.log 2>&1
echo "exit_code=$?"
```
