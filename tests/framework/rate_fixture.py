"""
The rate fixtures' modules and law parameters, read from the one run file that sets them.

``simulations/mini-millennium-horizontal/_tests/input/forest_blocks_rate.yaml`` lists the four
``process_rates`` fixtures in its ``rates`` phase and gives every law parameter; the rate battery
and the rate integration tests all take them from it through ``load_rate_fixture()``, so the
fixture's laws are set in one place. A test that needs other values says so at the point of use,
for instance with ``scale_coefficients()``.
"""

from __future__ import annotations

from pathlib import Path

import h5py
import yaml

RATE_RUN_FILE = (
    Path(__file__).resolve().parents[2]
    / "simulations"
    / "mini-millennium-horizontal"
    / "_tests"
    / "input"
    / "forest_blocks_rate.yaml"
)
RATE_PHASE = "rates"

# The parameters that are rates (inverse internal time) or a rate's efficiency: the ones a stiff
# case scales. The tracer fraction, the star-formation threshold and the yield are not rates.
RATE_COEFFICIENTS = (
    "TestRateSupplyInfallRate",
    "TestRateSupplyCoolingRate",
    "TestRateFormationEfficiency",
    "TestRateFeedbackReheatRate",
    "TestRateStrippingRate",
    "TestRateStrippingEjectionRate",
)
STIFF_FACTOR = 1.0e3

# The explicit scheme's positivity cap, sigma * C = 0.9 * 2 (rate_integrator.h): a segment whose
# largest K_max exceeds it can only be integrated in more than one step.
POSITIVITY_CAP = 1.8

STATISTICS = "RunProperties/RateIntegration"


def load_rate_fixture() -> tuple[tuple[str, ...], dict[str, float]]:
    """The rate phase's module names, in run-file order, and the law parameters of those modules.

    A parameter belongs to a module when its name starts with the module's name in camel case
    (``test_rate_supply`` owns ``TestRateSupply...``); the parameters of the discrete
    ``test_rate_jump`` that the run file also sets are not part of the result.
    """
    config = yaml.safe_load(RATE_RUN_FILE.read_text())["modules"]
    modules = tuple(next(iter(entry)) for entry in config["phases"][RATE_PHASE])
    prefixes = tuple("".join(word.title() for word in name.split("_")) for name in modules)
    params = {
        name: value for name, value in config["parameters"].items() if name.startswith(prefixes)
    }
    return modules, params


def scale_coefficients(params: dict[str, float], factor: float) -> dict[str, float]:
    """A copy of ``params`` with every rate coefficient (RATE_COEFFICIENTS) multiplied by
    ``factor``; the other parameters are unchanged."""
    return {
        name: value * factor if name in RATE_COEFFICIENTS else value
        for name, value in params.items()
    }


def read_step_extremes(master: Path) -> tuple[int, float]:
    """(rejected steps, largest K_max of any segment) from a master file's
    ``RunProperties/RateIntegration``."""
    with h5py.File(master, "r") as handle:
        group = handle[STATISTICS]
        return int(group["StepTotals"][0]["rejected_steps"]), float(group["StepMaxima"][0]["k_max"])
