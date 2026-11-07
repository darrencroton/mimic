"""Seeded synthetic catalogues for the sage26 plotting tests.

Builds, with no Mimic run, the galaxy record a snapshot figure reads and the per-snapshot input an
evolution figure reads. The record has one field for every output property declared by
``models/sage26/model_properties.yaml``, ``src/core/core_properties.yaml`` and
``simulations/mini-millennium/halo_properties.yaml`` (an entry with ``output: true``; a
``vec3_*`` type is a 3-element field), so a figure that reads any output property finds it.

The values are chosen to populate every registered figure's selections, not to be physical
truth: centrals, satellites and orphans; both ``Regime`` values (satellites keep -1, the
"unclassified" regime); haloes above 6e11 Msun; positive star formation, cold gas, H2, H I and
circumgalactic gas; bulges split into merger- and instability-driven parts; positive mass loading,
cooling rate, gas accretion and concentration; positions inside the 62.5 Mpc/h box.
Fields no figure selects on are left at zero.
"""

from pathlib import Path

import numpy as np
import yaml
from numpy.lib import recfunctions

REPO_ROOT = Path(__file__).resolve().parents[4]

CORE_PROPERTIES = REPO_ROOT / "src" / "core" / "core_properties.yaml"
MODEL_PROPERTIES = REPO_ROOT / "models" / "sage26" / "model_properties.yaml"
HALO_PROPERTIES = REPO_ROOT / "simulations" / "mini-millennium" / "halo_properties.yaml"
HORIZONTAL_HALO_PROPERTIES = (
    REPO_ROOT / "simulations" / "mini-millennium-horizontal" / "halo_properties.yaml"
)
A_LIST = REPO_ROOT / "simulations" / "mini-millennium" / "mini-millennium.a_list"

# (file, top-level key) of each property file, in the order the record is laid out
PROPERTY_FILES = (
    (CORE_PROPERTIES, "halo_properties"),
    (HALO_PROPERTIES, "halo_properties"),
    (MODEL_PROPERTIES, "galaxy_properties"),
)

# The eight snapshots sage26's run files write (`snapshot_list` in models/sage26/input), in the
# order the run file lists them.
RUN_SNAPSHOTS = (63, 37, 32, 27, 23, 20, 18, 16)

HUBBLE_H = 0.73  # simulations/mini-millennium/simulation_info.yaml
BOX_SIZE = 62.5  # Mpc/h
VOLUME = BOX_SIZE**3  # (Mpc/h)^3

# Mini-Millennium particle mass in 1e10 Msun/h (0.0860657), used for the Len column
PARTICLE_MASS = 0.0860657

_DTYPES = {
    "int": np.int32,
    "long long": np.int64,
    "float": np.float32,
    "double": np.float64,
    "vec3_float": (np.float32, (3,)),
    "vec3_double": (np.float64, (3,)),
}


def output_property_declarations(paths_and_keys=PROPERTY_FILES):
    """Return {name: type string} for every ``output: true`` property of the given files."""
    declared = {}
    for path, key in paths_and_keys:
        with open(path) as handle:
            entries = yaml.safe_load(handle)[key]
        for entry in entries:
            if entry.get("output") is True:
                declared[entry["name"]] = entry["type"]
    return declared


def record_dtype():
    """Return the numpy dtype of the full synthetic record."""
    declared = output_property_declarations()
    return np.dtype([(name, _DTYPES[ptype]) for name, ptype in declared.items()])


def run_redshifts():
    """Return {snapshot: redshift} for RUN_SNAPSHOTS, from the simulation's scale-factor list."""
    scale_factors = [float(line) for line in A_LIST.read_text().split()]
    return {snap: 1.0 / scale_factors[snap] - 1.0 for snap in RUN_SNAPSHOTS}


def _log10_to_code_mass(log_mass_msun):
    """Convert log10(M / Msun) to the output's 1e10 Msun/h unit at HUBBLE_H."""
    return 10.0**log_mass_msun * HUBBLE_H / 1.0e10


def build_catalogue(seed=20261107, n_halos=1500, epoch=0.0):
    """Return the synthetic record (a numpy recarray) for one snapshot.

    Args:
        seed: Seed of the generator; the same seed gives the same record.
        n_halos: Number of FoF haloes; each has one central and 0-3 satellites.
        epoch: Redshift-like scale (0 for z=0) that lowers the stellar mass and raises the
            specific star formation rate, so an evolution input differs between snapshots.
    """
    rng = np.random.default_rng(seed)

    # --- haloes: log10 M_halo from 10.6 to 14.6 (most above 6e11 Msun, i.e. log 11.78)
    halo_log_mass = rng.uniform(10.6, 14.6, n_halos)
    n_sat = np.minimum(rng.poisson(0.35 * np.maximum(halo_log_mass - 10.5, 0.0)), 3)
    host = np.concatenate([np.arange(n_halos), np.repeat(np.arange(n_halos), n_sat)])
    n = len(host)
    is_central = np.zeros(n, dtype=bool)
    is_central[:n_halos] = True

    galaxy_type = np.zeros(n, dtype=np.int32)
    satellite_kind = rng.random(n) < 0.3  # 30% of satellites are orphans (Type 2)
    galaxy_type[~is_central] = np.where(satellite_kind[~is_central], 2, 1)

    log_host = halo_log_mass[host]
    mvir = _log10_to_code_mass(log_host)
    mvir = np.where(is_central, mvir, mvir * rng.uniform(0.01, 0.3, n))
    central_mvir = _log10_to_code_mass(log_host)

    # --- stellar mass in Msun: a rising relation with halo mass, scattered; satellites lower
    log_stellar = 9.5 + 0.8 * (log_host - 11.5) + rng.normal(0.0, 0.25, n) - 0.15 * epoch
    log_stellar = np.where(is_central, log_stellar, rng.uniform(8.0, 10.5, n))
    log_stellar = np.clip(log_stellar, 7.6, 11.8)
    stellar = _log10_to_code_mass(log_stellar)

    # Bulge: half the galaxies with a bulge fraction in 0.1-0.5, the rest anywhere in 0-1
    bulge_fraction = np.where(
        rng.random(n) < 0.5, rng.uniform(0.1, 0.5, n), rng.uniform(0.0, 1.0, n)
    )
    bulge = stellar * bulge_fraction

    # Cold gas from a gas fraction; H2 and H I are parts of it, with some galaxies holding none
    gas_fraction = rng.uniform(0.05, 0.8, n)
    cold = stellar * gas_fraction / (1.0 - gas_fraction)
    h2 = np.where(rng.random(n) < 0.5, 0.3 * cold, 0.0)
    h1 = np.where(rng.random(n) < 0.8, 0.5 * cold, 0.0)
    cold_metallicity = rng.uniform(0.005, 0.04, n)

    # Halo reservoirs live on centrals (satellites keep none)
    central_only = is_central.astype(np.float64)
    hot = 0.05 * central_mvir * rng.uniform(0.2, 1.0, n) * central_only
    cgm = 0.03 * central_mvir * rng.uniform(0.2, 1.0, n) * central_only
    ejected = 0.02 * central_mvir * rng.uniform(0.2, 1.0, n) * central_only
    ics = 0.01 * stellar * rng.uniform(0.2, 1.0, n) * central_only
    black_hole = 1.0e-3 * bulge * rng.lognormal(0.0, 0.3, n)

    # Star formation: log10 sSFR across the red/blue cut at -11, rising with `epoch`
    log_ssfr = rng.uniform(-13.0, -9.5, n) + 0.4 * epoch
    stellar_msun = 10.0**log_stellar
    sfr = 10.0**log_ssfr * stellar_msun
    sfr_disk = 0.8 * sfr
    sfr_bulge = sfr - sfr_disk

    # Halo kinematics
    scale = (10.0**log_host / 1.0e12) ** (1.0 / 3.0)
    vvir = 150.0 * scale
    rvir = 0.2 * scale
    spin_lambda = 0.035 * rng.lognormal(0.0, 0.5, n)
    spin_direction = rng.normal(size=(n, 3))
    spin_direction /= np.linalg.norm(spin_direction, axis=1)[:, None]
    spin = spin_direction * (spin_lambda * np.sqrt(2.0) * vvir * rvir)[:, None]

    regime = np.where(is_central, rng.integers(0, 2, n), -1).astype(np.int32)
    regime[:2] = [0, 1]  # both Regime values are always present among the centrals

    # Fields of the SAGE26 z = 0 figures of c16 Slice 2
    merger_fraction = rng.uniform(0.0, 1.0, n)  # share of the bulge built by mergers
    merger_bulge = bulge * merger_fraction
    instability_bulge = bulge - merger_bulge
    mass_loading = rng.uniform(0.5, 10.0, n)  # dimensionless, positive for every galaxy
    # Cooling rate, Msun/Gyr: positive for about 80% of the galaxies, zero for the rest
    cooling_rate = np.where(rng.random(n) < 0.8, 10.0 ** rng.uniform(8.0, 10.5, n), 0.0)
    # Gas accretion, Msun/yr: cooling in 1-300 and cold streams in 0-5, each for a share of
    # the galaxies, so the logarithm of the sum falls inside the figure's 0 to 4 window
    mdot_cool = np.where(rng.random(n) < 0.6, rng.uniform(1.0, 300.0, n), 0.0)
    mdot_stream = np.where(rng.random(n) < 0.2, rng.uniform(0.0, 5.0, n), 0.0)
    concentration = rng.uniform(3.0, 20.0, n)  # every row, central or satellite, draws its own

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
        "infallMvir": mvir,
        "infallVvir": vvir,
        "infallVmax": 1.2 * vvir,
        "Pos": rng.uniform(0.0, BOX_SIZE, (n, 3)),
        "Vel": rng.normal(0.0, 150.0, (n, 3)),
        "VelDisp": vvir / np.sqrt(2.0),
        "Vmax": 1.2 * vvir,
        "Spin": spin,
        "MostBoundID": 5000 + np.arange(n),
        "ColdGas": cold,
        "HotGas": hot,
        "EjectedGas": ejected,
        "StellarMass": stellar,
        "BulgeMass": bulge,
        "ICS": ics,
        "CGMgas": cgm,
        "H2gas": h2,
        "H1gas": h1,
        "MetalsColdGas": cold * cold_metallicity,
        "MetalsStellarMass": stellar * 0.02,
        "BlackHoleMass": black_hole,
        "StarFormationRate": sfr,
        "SfrDisk": sfr_disk,
        "SfrBulge": sfr_bulge,
        "DiskScaleRadius": 0.003 * scale * rng.uniform(0.5, 1.5, n),
        "Regime": regime,
        "FFBRegime": np.where(is_central, rng.integers(0, 2, n), 0),
        "MergerBulgeMass": merger_bulge,
        "InstabilityBulgeMass": instability_bulge,
        "MassLoading": mass_loading,
        "CoolingRate": cooling_rate,
        "mdot_cool": mdot_cool,
        "mdot_stream": mdot_stream,
        "Concentration": concentration,
    }

    catalogue = np.zeros(n, dtype=record_dtype()).view(np.recarray)
    for name, array in values.items():
        catalogue[name] = array
    return catalogue


def snapshot_metadata(redshift=0.0, ngals=0):
    """Return the metadata dict the plotting engine passes to a figure for one snapshot."""
    return {
        "hubble_h": HUBBLE_H,
        "box_size": BOX_SIZE,
        "volume": VOLUME,
        "ntrees": 0,
        "ngals": ngals,
        "good_files": 1,
        "schema_units": {},
        "redshift": redshift,
    }


def build_evolution_input(fields, seed=20261107):
    """Return the ``snapshots`` argument of an evolution figure.

    The engine reads only the fields an evolution figure lists in ``EVOLUTION_PLOT_FIELDS``
    (mimic-plot.py:1554) for each written snapshot and passes ``{snapshot: (galaxies, volume,
    metadata)}`` with the snapshots in increasing redshift, as the redshift mapper orders them.
    This builds that input for the eight snapshots of RUN_SNAPSHOTS, each with only ``fields``.

    Args:
        fields: Field names the record holds (the figure's ``EVOLUTION_PLOT_FIELDS`` entry).
        seed: Seed of the generator; each snapshot derives its own from it.
    """
    redshifts = run_redshifts()
    snapshots = {}
    for index, snap in enumerate(sorted(RUN_SNAPSHOTS, key=lambda s: redshifts[s])):
        full = build_catalogue(seed + index, epoch=redshifts[snap])
        galaxies = recfunctions.repack_fields(full[list(fields)]).view(np.recarray)
        snapshots[snap] = (
            galaxies,
            VOLUME,
            snapshot_metadata(redshifts[snap], len(galaxies)),
        )
    return snapshots
