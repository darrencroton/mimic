# HOD Shared Utilities

Helpers local to the HOD model package. They are available when Mimic is built with `MODEL=hod` and are not framework-wide conventions; another model that needs similar behaviour should copy or reimplement them in its own package.

## Available Helpers

- **`hod_random.h`** — header-only counter-based random numbers. Every value is a pure function of a 64-bit stream key and a draw index, so a draw never depends on how many draws came before it:
  - `hod_random_key(seed, snapshot, host_id)`: one stream per host, each component salted and folded through the splitmix64 finaliser.
  - `hod_random_bits(key, index)` and `hod_random_uniform(key, index)`: 64 raw bits and a uniform strictly inside `(0, 1)` (52 bits centred in their cells, extremes `2^-53` and `1 - 2^-53`).
  - `hod_random_gaussian(key, index)`: a standard Gaussian by Box-Muller from the uniforms at `index` and `index + 1`.
  - `hod_random_poisson(lambda, u, max_count)`: a Poisson count by inversion from one uniform with a sequential cumulative search, terms formed in logarithms; returns `max_count + 1` when the count would exceed `max_count`.

`hod_populate` documents its draw-index layout in `modules/hod_populate/hod_populate.h`.

## Tests

The determinism and open-interval checks of `hod_random.h`, and its Poisson hand cases, live in `modules/hod_populate/_tests/test_unit_hod_populate.c` beside the module that uses it; its distributional checks are that file's statistical tests of the module's draws. There is no `shared/module_info.yaml` and no separately registered utility test.
