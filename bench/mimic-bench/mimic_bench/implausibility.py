"""Implausibility, the rejection rule, the false-exclusion rate and the non-implausible fraction.

For output k at a point, with the constraint's converted value ``z_k`` and error ``sigma_k`` and
the discrepancy budget ``b``, all in dex:

* the emulator's implausibility is ``|E_k - z_k| / sqrt(Var_k + v(N_pred) + sigma_k^2 + b^2)``,
  with E and Var the emulator's mean and own variance and ``v(N_pred)`` the declared count noise
  at the predicted count (:mod:`mimic_bench.diagnostics`): the predictive variance of a new run
  enters whole, so it is passed here as one total;
* the simulator's own implausibility at a run replaces E by the run's value y and drops both Var
  and the count noise: ``|y_k - z_k| / sqrt(sigma_k^2 + b^2)``.

A point is rejected when the ``rank``-th largest implausibility over the outputs given exceeds
the spec's ``threshold`` (the second largest above 3 in the shipped spec). The caller passes
only the configuration's validated core outputs; with fewer than ``rank`` of them no rejection
is defined, and :func:`rejected` refuses, as it refuses a non-finite implausibility (sorting
would put a NaN last and shift the rank).

At each budget (:func:`evaluate_budget`):

* the **false-exclusion rate** is counted over runs: among the holdout runs the simulator keeps,
  the fraction the emulator rejects. Its numerator and denominator are kept with it;
* reported beside it and entering no criterion: the rate over run and output pairs (among the
  pairs whose simulator implausibility is within the threshold, the fraction whose emulator
  implausibility exceeds it), and the fraction of simulator-rejected holdout runs that the
  emulator also rejects;
* the **non-implausible fraction** is the fraction of the box sample the emulator keeps; beside
  it is the fraction of holdout runs the simulator keeps, with its binomial standard error
  ``sqrt(p (1 - p) / n)``.

Every function here is pure: arrays in, arrays or a result out.

Usage:
    sim = simulator_implausibility(y_holdout, z, sigma, budget)
    emu = emulator_implausibility(mean, total_variance, z, sigma, budget)
    kept = ~rejected(sim, rule.rank, rule.threshold)
"""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
from mimic_bench.spec import ImplausibilitySpec


def emulator_implausibility(
    mean: np.ndarray,
    total_variance: np.ndarray,
    z: np.ndarray,
    sigma: np.ndarray,
    budget: float,
) -> np.ndarray:
    """Return ``|E - z| / sqrt(Var + v(N_pred) + sigma^2 + b^2)`` per point and output.

    Args:
        mean: Predicted values E, shape ``(m, k)``.
        total_variance: ``Var + v(N_pred)`` at each point and output, shape ``(m, k)``, dex^2.
        z: The constraint's converted value per output, shape ``(k,)``.
        sigma: The constraint's error per output, in dex, shape ``(k,)``.
        budget: The discrepancy budget b, in dex.

    Returns:
        The implausibility, shape ``(m, k)``.
    """
    mean = np.asarray(mean, dtype=float)
    denominator = np.asarray(total_variance, dtype=float) + np.asarray(sigma) ** 2 + budget**2
    return np.abs(mean - np.asarray(z)) / np.sqrt(denominator)


def simulator_implausibility(
    observed: np.ndarray, z: np.ndarray, sigma: np.ndarray, budget: float
) -> np.ndarray:
    """Return ``|y - z| / sqrt(sigma^2 + b^2)`` per run and output: no emulator, no count noise.

    Args:
        observed: The runs' own values y, shape ``(m, k)``.
        z: The constraint's converted value per output, shape ``(k,)``.
        sigma: The constraint's error per output, in dex, shape ``(k,)``.
        budget: The discrepancy budget b, in dex.

    Returns:
        The implausibility, shape ``(m, k)``.
    """
    observed = np.asarray(observed, dtype=float)
    return np.abs(observed - np.asarray(z)) / np.sqrt(np.asarray(sigma) ** 2 + budget**2)


def rejected(implausibility: np.ndarray, rank: int, threshold: float) -> np.ndarray:
    """Return, per point, whether the ``rank``-th largest implausibility exceeds ``threshold``.

    Args:
        implausibility: Shape ``(m, k)``, over the outputs that enter the rule.
        rank: Which largest value is compared (2 for the second largest).
        threshold: The rejection threshold.

    Returns:
        A boolean array of shape ``(m,)``.

    Raises:
        ValueError: If there are fewer than ``rank`` outputs, so the rule is undefined, or an
            implausibility is not finite.
    """
    implausibility = np.asarray(implausibility, dtype=float)
    if implausibility.ndim != 2 or implausibility.shape[1] < rank:
        raise ValueError(
            f"rejected: the rule needs at least {rank} outputs; got shape {implausibility.shape}"
        )
    if not np.all(np.isfinite(implausibility)):
        bad = int(np.sum(~np.isfinite(implausibility)))
        raise ValueError(f"rejected: {bad} implausibility value(s) are not finite")
    ranked = np.sort(implausibility, axis=1)[:, -rank]
    return ranked > threshold


def _ratio(numerator: int, denominator: int) -> float:
    """Return ``numerator / denominator``, NaN when the denominator is zero."""
    return numerator / denominator if denominator else math.nan


@dataclass(frozen=True)
class BudgetResult:
    """The rejection rule's results at one discrepancy budget, for one configuration.

    Attributes:
        budget: The discrepancy budget, in dex.
        holdout_runs: The number of holdout runs.
        simulator_kept: Holdout runs the simulator keeps (the false-exclusion denominator).
        simulator_kept_fraction: ``simulator_kept / holdout_runs``.
        simulator_kept_se: Its binomial standard error, ``sqrt(p (1 - p) / n)``.
        false_exclusions: Simulator-kept holdout runs the emulator rejects (the numerator).
        false_exclusion_rate: ``false_exclusions / simulator_kept``; NaN when none is kept.
        pair_false_exclusions: Run and output pairs within the threshold for the simulator and
            beyond it for the emulator.
        pair_kept: Run and output pairs within the threshold for the simulator.
        pair_rate: ``pair_false_exclusions / pair_kept``; enters no criterion.
        simulator_rejected: Holdout runs the simulator rejects.
        also_rejected: Of those, the runs the emulator also rejects.
        also_rejected_fraction: ``also_rejected / simulator_rejected``; enters no criterion.
        box_points: The number of points in the box sample.
        non_implausible_fraction: The fraction of the box sample the emulator keeps.
    """

    budget: float
    holdout_runs: int
    simulator_kept: int
    simulator_kept_fraction: float
    simulator_kept_se: float
    false_exclusions: int
    false_exclusion_rate: float
    pair_false_exclusions: int
    pair_kept: int
    pair_rate: float
    simulator_rejected: int
    also_rejected: int
    also_rejected_fraction: float
    box_points: int
    non_implausible_fraction: float


def evaluate_budget(
    budget: float,
    simulator_holdout: np.ndarray,
    emulator_holdout: np.ndarray,
    emulator_box: np.ndarray,
    rule: ImplausibilitySpec,
) -> BudgetResult:
    """Apply the rejection rule at one budget to the holdout and the box sample.

    Args:
        budget: The discrepancy budget the implausibilities were computed at, in dex.
        simulator_holdout: The simulator's implausibility at each holdout run, shape ``(m, k)``,
            over the validated outputs.
        emulator_holdout: The emulator's implausibility at each holdout run, shape ``(m, k)``.
        emulator_box: The emulator's implausibility at each box point, shape ``(p, k)``.
        rule: The spec's rejection rule.

    Returns:
        The counts and rates of :class:`BudgetResult`.

    Raises:
        ValueError: If the shapes disagree, or there are fewer than ``rule.rank`` outputs.
    """
    simulator_holdout = np.asarray(simulator_holdout, dtype=float)
    emulator_holdout = np.asarray(emulator_holdout, dtype=float)
    emulator_box = np.asarray(emulator_box, dtype=float)
    if simulator_holdout.shape != emulator_holdout.shape:
        raise ValueError(
            f"evaluate_budget: simulator shape {simulator_holdout.shape} differs from the "
            f"emulator's {emulator_holdout.shape}"
        )
    if emulator_box.ndim != 2 or emulator_box.shape[1] != simulator_holdout.shape[1]:
        raise ValueError(
            f"evaluate_budget: box shape {emulator_box.shape} does not have "
            f"{simulator_holdout.shape[1]} outputs"
        )
    simulator_rejects = rejected(simulator_holdout, rule.rank, rule.threshold)
    emulator_rejects = rejected(emulator_holdout, rule.rank, rule.threshold)
    box_rejects = rejected(emulator_box, rule.rank, rule.threshold)
    runs = simulator_holdout.shape[0]
    kept = int(np.sum(~simulator_rejects))
    false = int(np.sum(~simulator_rejects & emulator_rejects))
    pair_within = simulator_holdout <= rule.threshold
    pair_false = int(np.sum(pair_within & (emulator_holdout > rule.threshold)))
    pair_kept = int(np.sum(pair_within))
    sim_rejected = int(np.sum(simulator_rejects))
    also = int(np.sum(simulator_rejects & emulator_rejects))
    kept_fraction = _ratio(kept, runs)
    kept_se = math.sqrt(kept_fraction * (1.0 - kept_fraction) / runs) if runs else math.nan
    return BudgetResult(
        budget=float(budget),
        holdout_runs=runs,
        simulator_kept=kept,
        simulator_kept_fraction=kept_fraction,
        simulator_kept_se=kept_se,
        false_exclusions=false,
        false_exclusion_rate=_ratio(false, kept),
        pair_false_exclusions=pair_false,
        pair_kept=pair_kept,
        pair_rate=_ratio(pair_false, pair_kept),
        simulator_rejected=sim_rejected,
        also_rejected=also,
        also_rejected_fraction=_ratio(also, sim_rejected),
        box_points=int(emulator_box.shape[0]),
        non_implausible_fraction=_ratio(int(np.sum(~box_rejects)), int(emulator_box.shape[0])),
    )
