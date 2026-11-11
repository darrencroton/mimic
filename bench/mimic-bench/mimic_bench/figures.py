"""Figures of the ``analyse`` stage, drawn from its supplementary tables and nothing else.

Every figure reads one or more of the CSV tables the stage wrote under ``<out>/tables/`` (never a
fitted emulator or an array in memory), so a figure is always the picture of a table that can be
read in a file (:func:`mimic_bench.tables.read_table`). A configuration's panel and colour come
from its table's ``family`` and ``noise`` columns (a candidate's name in ``budgets.csv`` is its
family), never from its name: a family's colour and markers are those of its position among the
families, in the order the table first lists them, which is the spec's candidate order. The
figures, as PNG files under ``<out>/figures/``:

* ``standardised_errors.png``: the holdout standardised errors of each configuration, pooled
  over the core outputs (``tables/holdout_points.csv``);
* ``predicted_vs_run.png``: each configuration's predicted mean against the run's value, with a
  panel of the residual, the run's value less the prediction (``tables/holdout_points.csv``);
* ``non_implausible_fraction.png``: the emulator's non-implausible fraction of the box against the
  discrepancy budget, with the simulator's own estimate, the fraction of holdout runs it keeps
  (``tables/budgets.csv``, ``no_noise_budgets.csv``, ``prediction_only_budgets.csv``);
* ``learning_curve.png``: the median root-mean-square error and the fraction of outputs validated
  against the training rows, with the spec's calibration threshold (``tables/learning_curve.csv``);
* ``profiles.png``: the non-implausible fraction in equal slices of each design dimension
  (``tables/profiles.csv``).

Rendering happens in a subprocess (:func:`render_figures`). matplotlib reads ``MPLCONFIGDIR`` once,
when it is first imported, so the parent sets the variable to a new temporary directory, runs
``python -m mimic_bench.figures`` with it, and removes the directory afterwards: no font cache is
left under ``--out``. The same directory holds the renderer's ``XDG_CACHE_HOME``, because
fontconfig's own cache would otherwise be written under the user's home. The renderer refuses to
run if ``MPLCONFIGDIR`` is unset or matplotlib was already imported, and uses the non-interactive
Agg backend. This module does not import matplotlib at import time.

Usage:
    figures = render_figures(Path("out/tables"), Path("out/figures"))
"""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Callable, Mapping, Sequence

from mimic_bench.tables import read_table

FIGURES_DIRECTORY = "figures"
"""The directory under ``--out`` that holds the PNG files."""

NOISE_TITLES = {
    "fit_and_prediction": "candidate: noise in the fit and at prediction",
    "prediction_only": "noise at prediction only",
    "none": "no noise",
}
"""How a panel names where the noise model enters (the tables' ``noise`` column)."""

RENDER_TIMEOUT_SECONDS = 600
"""How long the rendering subprocess may run."""

last_report: dict[str, Any] = {}
"""What the last rendering subprocess reported: its matplotlib config directory and backend, the
figures drawn and the ones skipped with the cause."""

# By a family's position among the table's families, repeating after the last. The simulator's
# curve is the same for two families that validate the same outputs, so each family draws it with
# its own marker and dash, and the second shows through the first.
_FAMILY_STYLES = (
    {
        "color": "#D55E00",
        "marker": "^",
        "markersize": 4,
        "simulator": {"marker": "x", "markersize": 6, "linestyle": ":"},
    },
    {
        "color": "#0072B2",
        "marker": "o",
        "markersize": 8,
        "simulator": {"marker": "s", "markersize": 8, "linestyle": "--"},
    },
)
_PHI_UNITS = r"$\log_{10}\phi$ [$\log_{10}(\mathrm{Mpc}^{-3}\,\mathrm{dex}^{-1})$]"


class FigureError(RuntimeError):
    """A figure that cannot be drawn; the message names the cause."""


def _family_style(position: int) -> Mapping[str, Any]:
    """The colour and markers of the family at ``position`` among a table's families."""
    return _FAMILY_STYLES[position % len(_FAMILY_STYLES)]


def _number(cell: str | None) -> float:
    """A table cell as a float; NaN when the cell is empty."""
    return float(cell) if cell not in (None, "") else math.nan


def render_figures(tables: Path, figures_directory: Path) -> list[str]:
    """Draw every figure from the tables, in a subprocess whose matplotlib config is temporary.

    The temporary ``MPLCONFIGDIR`` is new, passed to the subprocess before matplotlib is imported
    there, and removed here whether or not rendering succeeded. Every file matplotlib and
    fontconfig write lies in it, so no other directory needs guarding.

    Args:
        tables: The directory of the stage's tables.
        figures_directory: Where to write the PNG files; it is made.

    Returns:
        The names of the figures written.

    Raises:
        FigureError: If the subprocess fails, or any figure could not be drawn (the other
            figures are still written).
    """
    global last_report
    tables, figures_directory = Path(tables), Path(figures_directory)
    config_dir = Path(tempfile.mkdtemp(prefix="mimic-mplconfig-")).resolve()
    try:
        figures_directory.mkdir(exist_ok=True)
        package_root = str(Path(__file__).resolve().parents[1])
        environment = {
            **os.environ,
            "MPLCONFIGDIR": str(config_dir),
            # fontconfig, which matplotlib asks for the system fonts, keeps its own cache under
            # XDG_CACHE_HOME (else ~/.cache), outside MPLCONFIGDIR: keep it in the same directory.
            "XDG_CACHE_HOME": str(config_dir / "cache"),
            "MPLBACKEND": "Agg",
            "PYTHONPATH": os.pathsep.join(
                [package_root, *filter(None, [os.environ.get("PYTHONPATH")])]
            ),
        }
        completed = subprocess.run(
            [sys.executable, "-m", "mimic_bench.figures", str(tables), str(figures_directory)],
            env=environment,
            capture_output=True,
            text=True,
            timeout=RENDER_TIMEOUT_SECONDS,
            check=False,
        )
        try:
            last_report = json.loads(completed.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            last_report = {}
        if completed.returncode != 0 and not last_report:
            raise FigureError(
                f"rendering failed (exit {completed.returncode}): {completed.stderr.strip()[-500:]}"
            )
        skipped = last_report.get("skipped", {})
        if skipped:
            raise FigureError(f"not drawn: {skipped}")
        return list(last_report.get("figures", []))
    finally:
        shutil.rmtree(config_dir, ignore_errors=True)


# --- Drawing (runs in the subprocess, after MPLCONFIGDIR is set) -----------------------------


def _group_by(rows: Sequence[Mapping[str, str]], key: str) -> dict[str, list[Mapping[str, str]]]:
    """Group rows by a column's value, keeping first-seen order."""
    groups: dict[str, list[Mapping[str, str]]] = {}
    for row in rows:
        groups.setdefault(row[key], []).append(row)
    return groups


def _panels(rows: Sequence[Mapping[str, str]]) -> list[tuple[int, int, str, str, list]]:
    """Lay out a per-configuration table: one row of panels per family and one column per noise.

    Returns:
        ``(panel row, panel column, family, noise, rows)`` per configuration, in table order.
    """
    by_configuration = _group_by(rows, "configuration")
    families = list(dict.fromkeys(r[0]["family"] for r in by_configuration.values()))
    noises = list(dict.fromkeys(r[0]["noise"] for r in by_configuration.values()))
    return [
        (
            families.index(r[0]["family"]),
            noises.index(r[0]["noise"]),
            r[0]["family"],
            r[0]["noise"],
            r,
        )
        for r in by_configuration.values()
    ]


def _title(family: str, noise: str) -> str:
    """A panel title: the family, then where the noise model enters."""
    return f"{family.replace('_', ' ')}\n{NOISE_TITLES.get(noise, noise)}"


def _draw_standardised_errors(plt: Any, tables: Path, destination: Path) -> None:
    """Histograms of the pooled standardised errors, one panel per configuration."""
    panels = _panels(read_table(tables / "holdout_points.csv"))
    rows = 1 + max((p[0] for p in panels), default=0)
    columns = 1 + max((p[1] for p in panels), default=0)
    figure, axes = plt.subplots(rows, columns, figsize=(4 * columns, 3.5 * rows), squeeze=False)
    limit, bins = 5.0, 40
    grid = [i / 100 * limit * 2 - limit for i in range(101)]
    density = [math.exp(-0.5 * g * g) / math.sqrt(2 * math.pi) for g in grid]
    for row, column, family, noise, members in panels:
        axis = axes[row][column]
        values = [_number(r["standardised_error"]) for r in members]
        inside = [v for v in values if math.isfinite(v) and -limit <= v <= limit]
        colour = _family_style(row)["color"]
        axis.hist(inside, bins=bins, range=(-limit, limit), density=True, color=colour, alpha=0.7)
        axis.plot(grid, density, color="black", linewidth=1.0, label="standard normal")
        for edge in (-2.0, 2.0):
            axis.axvline(edge, color="grey", linestyle="--", linewidth=0.8)
        axis.set_title(_title(family, noise), fontsize=9)
        axis.set_xlabel("standardised error (dimensionless)")
        axis.set_ylabel("density (per unit standardised error)")
        outside = len(values) - len(inside)
        axis.text(
            0.98,
            0.95,
            f"{len(values)} pairs; {outside} outside +/-{limit:g}",
            transform=axis.transAxes,
            ha="right",
            va="top",
            fontsize=8,
        )
    axes[0][0].legend(fontsize=8, loc="upper left")
    figure.suptitle("Holdout standardised errors, core outputs pooled")
    figure.tight_layout()
    figure.savefig(destination, dpi=150)
    plt.close(figure)


def _draw_predicted_vs_run(plt: Any, tables: Path, destination: Path) -> None:
    """Predicted mean against the run's value, and the residual below, per configuration."""
    panels = _panels(read_table(tables / "holdout_points.csv"))
    rows = 1 + max((p[0] for p in panels), default=0)
    columns = 1 + max((p[1] for p in panels), default=0)
    figure, axes = plt.subplots(
        2 * rows,
        columns,
        figsize=(4 * columns, 4.6 * rows),
        squeeze=False,
        gridspec_kw={"height_ratios": [3, 1] * rows},
    )
    for row, column, family, noise, members in panels:
        axis, residual_axis = axes[2 * row][column], axes[2 * row + 1][column]
        observed = [_number(r["observed_log10_phi"]) for r in members]
        predicted = [_number(r["predicted_mean"]) for r in members]
        residual = [o - p for o, p in zip(observed, predicted, strict=True)]
        colour = _family_style(row)["color"]
        axis.scatter(predicted, observed, s=4, alpha=0.4, color=colour, linewidths=0)
        residual_axis.scatter(predicted, residual, s=4, alpha=0.4, color=colour, linewidths=0)
        residual_axis.axhline(0.0, color="black", linewidth=1.0)
        finite = [v for v in observed + predicted if math.isfinite(v)]
        if finite:
            low, high = min(finite), max(finite)
            axis.plot([low, high], [low, high], color="black", linewidth=1.0)
        residual_axis.sharex(axis)
        axis.tick_params(labelbottom=False)
        axis.set_title(_title(family, noise), fontsize=9)
        axis.set_ylabel("run's value " + _PHI_UNITS, fontsize=8)
        residual_axis.set_xlabel("predicted mean " + _PHI_UNITS, fontsize=8)
        residual_axis.set_ylabel("run - predicted (dex)", fontsize=8)
    figure.suptitle("Predicted mean against the run's value, holdout runs, core outputs pooled")
    figure.tight_layout()
    figure.savefig(destination, dpi=150)
    plt.close(figure)


def _draw_non_implausible_fraction(plt: Any, tables: Path, destination: Path) -> None:
    """The emulator's non-implausible fraction against the budget, with the simulator's own."""
    groups = (
        ("budgets.csv", NOISE_TITLES["fit_and_prediction"]),
        ("prediction_only_budgets.csv", NOISE_TITLES["prediction_only"]),
        ("no_noise_budgets.csv", NOISE_TITLES["none"]),
    )
    read = [_group_by(read_table(tables / file_name), "configuration") for file_name, _ in groups]
    # A candidate is named by its family.
    families = list(
        dict.fromkeys(
            rows[0].get("family") or name for group in read for name, rows in group.items()
        )
    )
    figure, axes = plt.subplots(1, 3, figsize=(13, 4.5), sharey=True, squeeze=False)
    for axis, (_, title), by_configuration in zip(axes[0], groups, read, strict=True):
        gate = math.nan
        for name, rows in by_configuration.items():
            family = rows[0].get("family") or name
            style = _family_style(families.index(family))
            budgets = [_number(r["budget_dex"]) for r in rows]
            fraction = [_number(r["non_implausible_fraction"]) for r in rows]
            kept = [_number(r["simulator_kept_fraction"]) for r in rows]
            error = [_number(r["simulator_kept_fraction_se"]) for r in rows]
            label = family.replace("_", " ")
            axis.plot(
                budgets,
                fraction,
                marker=style["marker"],
                markersize=style["markersize"],
                color=style["color"],
                label=f"{label}: emulator, box",
            )
            axis.errorbar(
                budgets,
                kept,
                yerr=error,
                mfc="none",
                color=style["color"],
                capsize=2,
                label=f"{label}: simulator, holdout runs",
                **style["simulator"],
            )
            gates = [b for b, r in zip(budgets, rows, strict=True) if r["gate"] == "true"]
            gate = gates[0] if gates else gate
        if math.isfinite(gate):
            axis.axvline(gate, color="grey", linestyle=":", linewidth=1.0)
        if not by_configuration:
            axis.text(
                0.5,
                0.5,
                "no implausibility computed",
                transform=axis.transAxes,
                ha="center",
                fontsize=9,
            )
        axis.set_title(title, fontsize=9)
        axis.set_xlabel("discrepancy budget b (dex)")
    axes[0][0].set_ylabel("non-implausible fraction (dimensionless)")
    axes[0][0].legend(fontsize=7, loc="best")
    figure.suptitle("Non-implausible fraction against the budget (dotted line: the gate budget)")
    figure.tight_layout()
    figure.savefig(destination, dpi=150)
    plt.close(figure)


def _draw_learning_curve(plt: Any, tables: Path, destination: Path) -> None:
    """Median root-mean-square error and fraction validated against the training rows."""
    rows_read = read_table(tables / "learning_curve.csv")
    by_configuration = _group_by(rows_read, "configuration")
    figure, axes = plt.subplots(1, 2, figsize=(10, 4.2), squeeze=False)
    sizes_shown = sorted({float(r["training_rows"]) for r in rows_read})
    for position, (name, rows) in enumerate(by_configuration.items()):
        sizes, median, calibrated, missing = [], [], [], []
        for size, members in _group_by(rows, "training_rows").items():
            if members[0]["fitted"] == "true":
                sizes.append(float(size))
                median.append(_number(members[0]["median_rmse_dex"]))
                calibrated.append(_number(members[0]["configuration_calibrated_fraction"]))
            else:
                missing.append(size)
        colour = _family_style(position)["color"]  # a candidate is named by its family
        label = name.replace("_", " ")
        axes[0][0].plot(sizes, median, marker="o", color=colour, label=label)
        axes[0][1].plot(sizes, calibrated, marker="o", color=colour, label=label)
        if missing:
            axes[0][1].text(
                0.02,
                0.05,
                f"{label}: not fitted at {', '.join(missing)} rows",
                transform=axes[0][1].transAxes,
                fontsize=7,
            )
    if rows_read:
        threshold = _number(rows_read[0]["calibrated_min_fraction"])
        axes[0][1].axhline(
            threshold, color="grey", linestyle="--", linewidth=1.0, label="calibrated at or above"
        )
    axes[0][0].set_ylabel("median rmse over core outputs (dex)")
    axes[0][1].set_ylabel("fraction of core outputs validated (dimensionless)")
    for axis in axes[0]:
        axis.set_xlabel("training rows (count)")
        axis.set_xscale("log")
        axis.set_xticks(sizes_shown, [f"{int(v)}" for v in sizes_shown])
        axis.minorticks_off()
    axes[0][0].legend(fontsize=8)
    axes[0][1].legend(fontsize=8)
    figure.suptitle("Learning curve: each candidate refitted on nested training subsets")
    figure.tight_layout()
    figure.savefig(destination, dpi=150)
    plt.close(figure)


def _draw_profiles(plt: Any, tables: Path, destination: Path) -> None:
    """The non-implausible fraction in equal slices of each design dimension."""
    rows = read_table(tables / "profiles.csv")
    by_parameter = _group_by(rows, "parameter")
    columns = 3
    panel_rows = math.ceil(max(len(by_parameter), 1) / columns)
    figure, axes = plt.subplots(panel_rows, columns, figsize=(12, 3.4 * panel_rows), squeeze=False)
    flat = [axis for row in axes for axis in row]
    configuration = rows[0]["configuration"] if rows else "no configuration"
    computed = bool(rows) and rows[0]["status"] == "computed"
    for axis, (parameter, members) in zip(flat, by_parameter.items(), strict=False):
        centres = [(_number(r["unit_low"]) + _number(r["unit_high"])) / 2 for r in members]
        fraction = [_number(r["non_implausible_fraction"]) for r in members]
        inert = members[0]["inert"] == "true"
        axis.plot(centres, fraction, marker="o", color="grey" if inert else "#0072B2")
        axis.set_ylim(0.0, 1.0)
        axis.set_title(f"{parameter}{' (inert control)' if inert else ''}", fontsize=9)
        axis.set_xlabel("position in range, own scale (unit cube)")
        axis.set_ylabel("non-implausible fraction (dimensionless)")
        if not computed:
            axis.text(0.5, 0.5, "not computed", transform=axis.transAxes, ha="center")
    for axis in flat[len(by_parameter) :]:
        axis.axis("off")
    figure.suptitle(f"Profiles at the gate budget: {configuration}")
    figure.tight_layout()
    figure.savefig(destination, dpi=150)
    plt.close(figure)


FIGURES: tuple[tuple[str, tuple[str, ...], str], ...] = (
    ("standardised_errors.png", ("holdout_points.csv",), "_draw_standardised_errors"),
    ("predicted_vs_run.png", ("holdout_points.csv",), "_draw_predicted_vs_run"),
    (
        "non_implausible_fraction.png",
        ("budgets.csv", "prediction_only_budgets.csv", "no_noise_budgets.csv"),
        "_draw_non_implausible_fraction",
    ),
    ("learning_curve.png", ("learning_curve.csv",), "_draw_learning_curve"),
    ("profiles.png", ("profiles.csv",), "_draw_profiles"),
)
"""Each figure: its file, the tables it is drawn from and the function that draws it."""


def draw_figures(tables: Path, figures_directory: Path) -> dict[str, Any]:
    """Draw every figure whose tables exist; run only in a process that set ``MPLCONFIGDIR``.

    Args:
        tables: The directory of the stage's tables.
        figures_directory: The directory to write the PNG files into; it must exist.

    Returns:
        A report: ``configdir`` (matplotlib's), ``backend``, ``figures`` (written) and
        ``skipped`` (file name to the cause).

    Raises:
        FigureError: If ``MPLCONFIGDIR`` is unset or matplotlib was imported before it was.
    """
    if not os.environ.get("MPLCONFIGDIR"):
        raise FigureError("MPLCONFIGDIR must be set before matplotlib is imported")
    if "matplotlib" in sys.modules:
        raise FigureError("matplotlib was imported before MPLCONFIGDIR was checked")
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    drawers: dict[str, Callable[..., None]] = {name: globals()[name] for _, _, name in FIGURES}
    report: dict[str, Any] = {
        "configdir": matplotlib.get_configdir(),
        "backend": matplotlib.get_backend(),
        "figures": [],
        "skipped": {},
    }
    for file_name, needed, drawer in FIGURES:
        absent = [t for t in needed if not (Path(tables) / t).is_file()]
        if absent:
            report["skipped"][file_name] = f"missing table(s) {absent}"
            continue
        try:
            drawers[drawer](plt, Path(tables), Path(figures_directory) / file_name)
        except Exception as exc:  # one figure that cannot be drawn does not stop the others
            report["skipped"][file_name] = repr(exc)
        else:
            report["figures"].append(file_name)
    return report


def main(argv: Sequence[str] | None = None) -> int:
    """Draw the figures: ``python -m mimic_bench.figures <tables> <figures>``.

    Prints the report as one JSON line and exits 0 when every figure was drawn, 1 otherwise.
    """
    arguments = list(sys.argv[1:] if argv is None else argv)
    if len(arguments) != 2:
        print(
            "usage: python -m mimic_bench.figures <tables directory> <figures directory>",
            file=sys.stderr,
        )
        return 2
    report = draw_figures(Path(arguments[0]), Path(arguments[1]))
    print(json.dumps(report))
    return 0 if not report["skipped"] else 1


if __name__ == "__main__":
    sys.exit(main())
