"""The bench: evaluating a Mimic model over a parameter design, driven by a declared spec.

The evaluation boundary (spec, design, run files, capture with verification and provenance, one
statistic, one constraint) is shared by the methods built on it; history matching with emulators
is the first.

The package depends on Mimic's run-file interface only. Everything specific to a model (the
run-file template, the parameters and their scales, the measured property, the constraint and
the thresholds) is declared in a campaign spec and named nowhere in code.

Stages are run as ``mimic-bench.py <stage>``; see :mod:`mimic_bench.cli`.
"""

__version__ = "0.1.0"
