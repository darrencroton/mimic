"""Seeded synthetic catalogue for the sage16 plotting tests.

Builds, with no Mimic run, the galaxy record a snapshot figure reads and the per-snapshot input an
evolution figure reads. The record has one field for every output property declared by
``models/sage16/model_properties.yaml``, ``src/core/core_properties.yaml`` and
``simulations/mini-millennium/halo_properties.yaml`` (an entry with ``output: true``; a ``vec3_*``
type is a 3-element field), so a figure that reads any output property finds it, and a renamed or
removed property fails here first.

The values are chosen to populate every registered figure's selections, not to be physical truth:
centrals, satellites and orphans; haloes of 10^10.6 to 10^14.6 Msun; stellar, cold, hot and ejected
gas, intracluster stars, a black hole and cold-gas metals; bulges of 0.1 to 0.5 of the stellar mass
for half the galaxies; specific star formation rates on both sides of the red/blue cut of 1e-11
per year; positions inside the 62.5 Mpc/h box. Fields no figure selects on are left at zero.
"""

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tests"))

from framework import plot_checks as checks  # noqa: E402

__all__ = [
    "HALO_PROPERTIES",
    "HORIZONTAL_HALO_PROPERTIES",
    "HUBBLE_H",
    "VOLUME",
    "build_catalogue",
    "build_evolution_input",
    "property_files",
    "record_dtype",
    "run_parameters",
    "snapshot_metadata",
]

# The simulation these tests run on: mini-Millennium
SIMULATION_DIR = checks.REPO_ROOT / "simulations" / "mini-millennium"
HALO_PROPERTIES = SIMULATION_DIR / "halo_properties.yaml"
HORIZONTAL_HALO_PROPERTIES = (
    checks.REPO_ROOT / "simulations" / "mini-millennium-horizontal" / "halo_properties.yaml"
)
A_LIST = SIMULATION_DIR / "mini-millennium.a_list"

# The eight snapshots the run files write (`snapshot_list` in models/sage16/input), in the order
# the run files list them
RUN_SNAPSHOTS = (63, 37, 32, 27, 23, 20, 18, 16)

HUBBLE_H = 0.73  # simulations/mini-millennium/simulation_info.yaml
OMEGA_MATTER = 0.25  # simulations/mini-millennium/simulation_info.yaml
OMEGA_LAMBDA = 0.75  # simulations/mini-millennium/simulation_info.yaml
BOX_SIZE = 62.5  # Mpc/h
VOLUME = BOX_SIZE**3  # (Mpc/h)^3

# Mini-Millennium particle mass in 1e10 Msun/h, used for the Len column
PARTICLE_MASS = 0.0860657


def property_files():
    """Return the (file, key) pairs declaring the output properties of the record."""
    return checks.package_property_files("sage16", HALO_PROPERTIES)


def record_dtype():
    """Return the numpy dtype of the full synthetic record."""
    return checks.record_dtype_of(property_files())


def snapshot_metadata(redshift=0.0, ngals=0):
    """Return the metadata dict the plotting engine passes to a figure for one snapshot."""
    return checks.snapshot_metadata(HUBBLE_H, BOX_SIZE, redshift, ngals)


def _log10_to_code_mass(log_mass_msun):
    """Convert log10(M / Msun) to the output's 1e10 Msun/h unit at HUBBLE_H."""
    return 10.0**log_mass_msun * HUBBLE_H / 1.0e10


def build_catalogue(seed=20261107, n_halos=600, epoch=0.0):
    """Return the synthetic record (a numpy recarray) for one snapshot.

    Args:
        seed: Seed of the generator; the same seed gives the same record.
        n_halos: Number of FoF haloes; each has one central and 0-3 satellites.
        epoch: Redshift-like scale (0 for z=0) that lowers the stellar mass and raises the
            specific star formation rate, so an evolution input differs between snapshots.
    """
    rng = np.random.default_rng(seed)

    halo_log_mass = rng.uniform(10.6, 14.6, n_halos)
    n_sat = np.minimum(rng.poisson(0.35 * np.maximum(halo_log_mass - 10.5, 0.0)), 3)
    host = np.concatenate([np.arange(n_halos), np.repeat(np.arange(n_halos), n_sat)])
    n = len(host)
    is_central = np.zeros(n, dtype=bool)
    is_central[:n_halos] = True

    galaxy_type = np.zeros(n, dtype=np.int32)
    orphan = rng.random(n) < 0.3  # 30% of satellites are orphans (Type 2)
    galaxy_type[~is_central] = np.where(orphan[~is_central], 2, 1)

    log_host = halo_log_mass[host]
    central_mvir = _log10_to_code_mass(log_host)
    mvir = np.where(is_central, central_mvir, central_mvir * rng.uniform(0.01, 0.3, n))

    # Stellar mass in Msun: a rising relation with halo mass, scattered; satellites lower
    log_stellar = 9.5 + 0.8 * (log_host - 11.5) + rng.normal(0.0, 0.25, n) - 0.15 * epoch
    log_stellar = np.where(is_central, log_stellar, rng.uniform(8.0, 10.5, n))
    stellar = _log10_to_code_mass(np.clip(log_stellar, 7.6, 11.8))

    # Bulge: half the galaxies with a bulge fraction in 0.1-0.5, the rest anywhere in 0-1
    bulge_fraction = np.where(
        rng.random(n) < 0.5, rng.uniform(0.1, 0.5, n), rng.uniform(0.0, 1.0, n)
    )
    bulge = stellar * bulge_fraction

    gas_fraction = rng.uniform(0.05, 0.8, n)
    cold = stellar * gas_fraction / (1.0 - gas_fraction)
    cold_metallicity = rng.uniform(0.005, 0.04, n)

    # Halo reservoirs live on centrals (satellites keep none)
    central_only = is_central.astype(np.float64)
    hot = 0.05 * central_mvir * rng.uniform(0.2, 1.0, n) * central_only
    ejected = 0.02 * central_mvir * rng.uniform(0.2, 1.0, n) * central_only
    ics = 0.01 * stellar * rng.uniform(0.2, 1.0, n) * central_only
    black_hole = 1.0e-3 * bulge * rng.lognormal(0.0, 0.3, n)

    # Star formation: log10 sSFR across the red/blue cut at -11, rising with `epoch`
    log_ssfr = rng.uniform(-13.0, -9.5, n) + 0.4 * epoch
    sfr = 10.0**log_ssfr * 10.0 ** np.clip(log_stellar, 7.6, 11.8)

    # Halo kinematics
    scale = (10.0**log_host / 1.0e12) ** (1.0 / 3.0)
    vvir = 150.0 * scale
    rvir = 0.2 * scale
    spin_direction = rng.normal(size=(n, 3))
    spin_direction /= np.linalg.norm(spin_direction, axis=1)[:, None]
    spin_lambda = 0.035 * rng.lognormal(0.0, 0.5, n)
    spin = spin_direction * (spin_lambda * np.sqrt(2.0) * vvir * rvir)[:, None]

    values = {
        "SnapNum": np.full(n, 63),
        "Type": galaxy_type,
        "UniqueGalaxyID": 1000 + np.arange(n),
        "UniqueCentralGalaxyID": 1000 + host,
        "dT": np.ones(n),
        "Len": np.maximum(np.round(mvir / PARTICLE_MASS), 1),
        "Mvir": mvir,
        "CentralMvir": central_mvir,
        "Rvir": rvir,
        "Vvir": vvir,
        "Vmax": 1.2 * vvir,
        "VelDisp": vvir / np.sqrt(2.0),
        "Pos": rng.uniform(0.0, BOX_SIZE, (n, 3)),
        "Vel": rng.normal(0.0, 150.0, (n, 3)),
        "Spin": spin,
        "ColdGas": cold,
        "HotGas": hot,
        "EjectedGas": ejected,
        "StellarMass": stellar,
        "BulgeMass": bulge,
        "ICS": ics,
        "MetalsColdGas": cold * cold_metallicity,
        "MetalsStellarMass": stellar * 0.02,
        "BlackHoleMass": black_hole,
        "StarFormationRate": sfr,
        "DiskScaleRadius": 0.003 * scale * rng.uniform(0.5, 1.5, n),
    }
    catalogue = np.zeros(n, dtype=record_dtype()).view(np.recarray)
    for name, array in values.items():
        catalogue[name] = array
    return catalogue


def run_parameters():
    """Return the cosmology entries of the engine's run parameters and the module parameter the
    baryon-fraction figure reads from the run file's modules.parameters (mimic-plot.py stores the
    modules section as EnabledModules), at the shipped run files' values
    (models/sage16/input/sage16_mini-millennium.yaml)."""
    return {
        "Hubble_h": HUBBLE_H,
        "Omega": OMEGA_MATTER,
        "OmegaLambda": OMEGA_LAMBDA,
        "EnabledModules": {"parameters": {"GlobalBaryonFraction": 0.17}},
    }


def build_evolution_input(fields, seed=20261107):
    """Return the ``snapshots`` argument of an evolution figure holding only ``fields``."""
    redshifts = checks.scale_factor_redshifts(A_LIST, RUN_SNAPSHOTS)
    return checks.evolution_input(
        build_catalogue, fields, redshifts, VOLUME, snapshot_metadata, seed
    )
