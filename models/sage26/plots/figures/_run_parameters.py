"""
Run parameters read by the SAGE26 figures.

The cosmology comes from the engine's parameters (plot/mimic-plot/mimic-plot.py:588-593). A
module parameter comes from the run file's modules.parameters (params["EnabledModules"]
["parameters"], read with output_utils.read_module_parameters, as the hod and sham figures read
theirs), so a figure drawn from a non-default run uses that run's value. SAGE26's plotting scripts
write those parameters as literals; a figure keeps the literal only as the fallback for a run that
does not give the parameter (for example native SAGE output read with --input-format=sage-hdf5),
and warns when it uses it.
"""

from output_utils import read_module_parameters, warn


def cosmology(params):
    """
    Return (Omega, OmegaLambda, Hubble_h) from the run's parameters, or None if one is missing
    or zero.
    """
    values = tuple(params.get(key) for key in ("Omega", "OmegaLambda", "Hubble_h"))
    if any(not value for value in values):
        return None
    return values


def module_parameter(params, name, fallback):
    """
    Return the run's value of module parameter ``name``, or ``fallback`` with a warning.

    Args:
        params: Mimic params dict (the run file's modules section is EnabledModules).
        name: Parameter name as the run file writes it.
        fallback: Value to use when the run does not give the parameter (SAGE26's plotting
            literal).

    Returns:
        The parameter as a float.
    """
    values, missing = read_module_parameters(params or {}, [name])
    if missing:
        warn(f"Run parameter {name} not found in modules.parameters; the figure uses {fallback:g}")
        return fallback
    return values[name]
