"""Test doubles and small cases for the diagnostics, implausibility and verdict tests.

:class:`TableEmulator` is an :class:`~mimic_bench.emulator.Emulator` whose predictions are
given, not fitted: at a point it was told about it returns that point's mean and variance, and
anywhere else a fixed mean and variance. It lets a test construct exact standardised errors and
implausibilities and run them through the package's own pure functions. :class:`ScaledEmulator`
wraps a fitted emulator and scales its variance, a planted defect.
"""

from __future__ import annotations

from typing import Callable

import numpy as np
from mimic_bench.analyse import AnalysisArrays
from mimic_bench.emulator import Emulator

# A set of ten standardised errors with 0.9 within 2, none beyond 3 and an RMS of about 1.2,
# so an output built from it is validated under the shipped spec's criteria.
CALIBRATED_Z = np.array([0.5, -0.5, 1.0, -1.0, 1.5, -1.5, 0.2, -0.2, 1.2, -2.5])


class TableEmulator(Emulator):
    """Returns given predictions at given points, and a fixed prediction elsewhere.

    Args:
        x_known: Points with a given prediction, shape ``(m, d)``.
        mean_known: The mean at each known point.
        variance_known: The emulator variance at each known point.
        mean_other: The mean anywhere else.
        variance_other: The emulator variance anywhere else.
    """

    def __init__(
        self,
        x_known: np.ndarray,
        mean_known: np.ndarray,
        variance_known: np.ndarray,
        mean_other: float = 0.0,
        variance_other: float = 1.0,
    ) -> None:
        super().__init__()
        self._index = {row.tobytes(): i for i, row in enumerate(np.asarray(x_known, dtype=float))}
        self._mean = np.asarray(mean_known, dtype=float)
        self._variance = np.asarray(variance_known, dtype=float)
        self._mean_other = mean_other
        self._variance_other = variance_other
        self.fitted_noise: np.ndarray | None = None

    def describe(self) -> dict:
        """The double reports only its family."""
        return {"family": "table"}

    def _fit(self, x: np.ndarray, y: np.ndarray, noise: np.ndarray | None) -> None:
        self.fitted_noise = noise

    def _leave_one_out(self) -> tuple[np.ndarray, np.ndarray]:
        raise NotImplementedError("the table double has no leave-one-out")

    def _predict(self, x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        rows = [self._index.get(row.tobytes()) for row in x]
        mean = np.array([self._mean_other if i is None else self._mean[i] for i in rows])
        variance = np.array(
            [self._variance_other if i is None else self._variance[i] for i in rows]
        )
        return mean, variance


class ScaledEmulator(Emulator):
    """A fitted emulator whose predictive variance is multiplied by ``factor``."""

    def __init__(self, inner: Emulator, factor: float) -> None:
        super().__init__()
        self._inner, self._factor = inner, factor
        self._n_inputs = inner._n_inputs

    def describe(self) -> dict:
        """The inner fit's description."""
        return self._inner.describe()

    def _fit(self, x: np.ndarray, y: np.ndarray, noise: np.ndarray | None) -> None:
        raise NotImplementedError("the scaled double wraps a fitted emulator")

    def _leave_one_out(self) -> tuple[np.ndarray, np.ndarray]:
        raise NotImplementedError("the scaled double has no leave-one-out")

    def _predict(self, x: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
        mean, variance = self._inner.predict(x)
        return mean, variance * self._factor


def table_factory(emulators: list[TableEmulator]) -> Callable[[], Emulator]:
    """A factory that hands out the given emulators in order, one per output."""
    queue = list(emulators)
    return lambda: queue.pop(0)


def small_arrays(
    k: int, m: int = 100, *, z: np.ndarray | None = None, sigma: float = 0.05
) -> AnalysisArrays:
    """Analysis arrays with k outputs, 20 training and m holdout points in one dimension.

    The holdout values are all zero, the constraint is ``z`` (zero by default) with error
    ``sigma`` in dex, and the box is the 50 points ``(k + 0.3) / 50``, none of which is a holdout
    point ``(j + 0.5) / m`` for the ``m`` the tests use (10 and 100), so a
    :class:`TableEmulator` gives the box its fixed prediction for points it was not told about.
    """
    x_train = np.linspace(0.0, 1.0, 20)[:, None]
    x_holdout = ((np.arange(m) + 0.5) / m)[:, None]
    x_box = ((np.arange(50) + 0.3) / 50)[:, None]
    counts_train = np.full((20, k), 100)
    y_train = np.tile(np.linspace(-1.0, 1.0, 20)[:, None], (1, k))
    return AnalysisArrays(
        labels=tuple(f"{9.05 + 0.1 * j:.2f}" for j in range(k)),
        x_train=x_train,
        y_train=y_train,
        counts_train=counts_train,
        x_holdout=x_holdout,
        y_holdout=np.zeros((m, k)),
        counts_holdout=np.full((m, k), 100),
        x_box=x_box,
        z=np.zeros(k) if z is None else np.asarray(z, dtype=float),
        sigma=np.full(k, sigma),
        holdout_ids=tuple(f"holdout-{i:04d}" for i in range(m)),
        exploratory_labels=(),
        exploratory_train=np.zeros((20, 0), dtype=np.int64),
        exploratory_holdout=np.zeros((m, 0), dtype=np.int64),
    )
