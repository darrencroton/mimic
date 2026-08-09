#!/usr/bin/env python
"""Structural conformance checker for the committed snapshot-HDF5 fixtures.

Asserts everything ``scripts/convert/validate.py`` asserts about the structure
of a snapshot-HDF5 dataset **except chunk shape**, which is the only excluded
structural check: the exact object set, the exact header attribute names,
dtypes and values, the ``n_halos`` value bounds, the exact ``/halos`` dataset
set with contract dtypes and ranks/shapes, chunked (not contiguous) storage
with the absence of compression and other filters, and the ``/ForestID``
sidecar shape.

Chunk shape is deliberately excluded. The committed fixture is re-chunked small
so it costs kilobytes rather than the 6.25 MiB per populated snapshot the
production ``(65536,)`` layout would allocate, and the frozen specification
makes chunk layout a storage detail: "consumers must not depend on chunk
boundaries, only on dataset shape and type"
(docs/dev/SNAPSHOT-HDF5-FORMAT.md, Storage Layout).

The producer battery's data-level invariants (link ranges, FoF chain integrity,
progenitor closure, identity density, count conservation against the source)
are not repeated here: the generator runs the real battery over the
production-layout dataset these files are copied from, and asserts value
equality between the two.

Usage:
    mimic_venv/bin/python \\
        simulations/micro-uchuu-snapshot/_tests/input/check_fixture_conformance.py [DIR]

DIR defaults to the committed ``_tests/data/``. Exits 0 when the dataset
conforms, 1 with one line per defect otherwise.
"""

import argparse
import sys
from pathlib import Path

import h5py
import numpy as np
import yaml

PACKAGE_DIR = Path(__file__).resolve().parents[2]
DATA_DIR = PACKAGE_DIR / "_tests" / "data"
SIMULATION_INFO = PACKAGE_DIR / "simulation_info.yaml"

A_LIST_NAME = "micro-uchuu-fixture.a_list"

#: format_version this fixture set implements
FORMAT_VERSION = 1

#: header attributes: name -> numpy dtype (frozen spec, Header Attributes)
HEADER_ATTRS = {
    "format_version": np.int32,
    "links_adjacent": np.int32,
    "scale_factor": np.float64,
    "snapshot_number": np.int32,
    "n_halos": np.int64,
    "n_forests_total": np.int64,
    "max_halo_rank_in_forest": np.int64,
    "box_size_mpc_h": np.float64,
    "particle_mass_msun_h": np.float64,
    "omega_matter": np.float64,
    "omega_lambda": np.float64,
    "hubble_h": np.float64,
}

#: /halos datasets: name -> (numpy dtype, is_vec3) (frozen spec, Halo Datasets)
HALO_DATASETS = {
    "Descendant": (np.int32, False),
    "FirstProgenitor": (np.int32, False),
    "NextProgenitor": (np.int32, False),
    "FirstHaloInFOFgroup": (np.int32, False),
    "NextHaloInFOFgroup": (np.int32, False),
    "Len": (np.int32, False),
    "SnapNum": (np.int32, False),
    "M_Crit200": (np.float32, False),
    "Pos": (np.float32, True),
    "Vel": (np.float32, True),
    "Spin": (np.float32, True),
    "VelDisp": (np.float32, False),
    "Vmax": (np.float32, False),
    "MostBoundID": (np.int64, False),
    "ForestIndex": (np.int64, False),
    "HaloRankInForest": (np.int64, False),
}

#: 1e10 Msun/h (simulation_info) -> Msun/h (header attribute), as the writer
#: converts it (scripts/convert/hdf5_writer.py load_header_metadata)
REF_TO_NATIVE_MASS = 1.0e10


def load_a_list(path):
    values = []
    for line in Path(path).read_text().splitlines():
        text = line.strip()
        if text and not text.startswith("#"):
            values.append(float(text))
    return values


def load_physical_metadata(path):
    """The physical header attributes the package's own metadata implies."""
    data = yaml.safe_load(Path(path).read_text())
    sim = data["simulation"]
    return {
        "box_size_mpc_h": float(sim["box_size"]["value"]),
        "particle_mass_msun_h": float(sim["particle_mass"]["value"]) * REF_TO_NATIVE_MASS,
        "omega_matter": float(sim["cosmology"]["omega_matter"]),
        "omega_lambda": float(sim["cosmology"]["omega_lambda"]),
        "hubble_h": float(sim["cosmology"]["hubble_h"]),
    }


def check_filters(dataset, name, failures):
    """The storage contract is chunked and UNFILTERED (chunk shape itself is
    deliberately not checked — see the module docstring)."""
    if dataset.chunks is None:
        failures.append(
            "{} is stored contiguously; contiguous storage violates the contract's chunked "
            "layout requirement (chunk shape itself is unchecked)".format(name)
        )
    if dataset.compression is not None:
        failures.append(
            "{} is compressed ({}); the contract forbids compression".format(
                name, dataset.compression
            )
        )
    if dataset.scaleoffset is not None:
        failures.append(
            "{} uses the scale-offset filter; the contract forbids filters".format(name)
        )
    if dataset.shuffle:
        failures.append("{} uses the shuffle filter; the contract forbids filters".format(name))
    if dataset.fletcher32:
        failures.append("{} uses the fletcher32 filter; the contract forbids filters".format(name))


def check_snapshot_file(path, snap, scale_factor, physical, failures):
    """Object set, header attributes, and /halos datasets of one file."""
    prefix = path.name
    with h5py.File(path, "r") as handle:
        objects = set(handle.keys())
        if objects != {"header", "halos"}:
            failures.append(
                "{}: object set {} != {{'halos', 'header'}}".format(prefix, sorted(objects))
            )
            return None
        if not isinstance(handle["header"], h5py.Group) or not isinstance(
            handle["halos"], h5py.Group
        ):
            failures.append("{}: /header and /halos must both be groups".format(prefix))
            return None

        attrs = handle["header"].attrs
        attr_names = set(attrs.keys())
        expected_attrs = set(HEADER_ATTRS)
        if attr_names != expected_attrs:
            failures.append(
                "{}: header attribute set mismatch: missing {}, extra {}".format(
                    prefix,
                    sorted(expected_attrs - attr_names),
                    sorted(attr_names - expected_attrs),
                )
            )
        for name in sorted(attr_names & expected_attrs):
            actual = np.asarray(attrs[name])
            expected_dtype = np.dtype(HEADER_ATTRS[name])
            if actual.dtype != expected_dtype or actual.shape != ():
                failures.append(
                    "{}: attribute {} has dtype {} shape {}, contract requires scalar {}".format(
                        prefix, name, actual.dtype, actual.shape, expected_dtype
                    )
                )

        header = {name: np.asarray(attrs[name]).item() for name in attr_names & expected_attrs}
        expected_values = {
            "format_version": FORMAT_VERSION,
            "links_adjacent": 1,
            "snapshot_number": snap,
            "scale_factor": scale_factor,
        }
        expected_values.update(physical)
        for name, want in expected_values.items():
            if name in header and header[name] != want:
                failures.append(
                    "{}: attribute {} is {!r}, expected {!r}".format(
                        prefix, name, header[name], want
                    )
                )

        # Ordered comparisons only make sense on an integer; a wrong-dtype
        # n_halos is already reported by the dtype loop above, and comparing it
        # here would raise instead of adding to the collected report.
        n_halos = header.get("n_halos")
        if not isinstance(n_halos, (int, np.integer)):
            n_halos = None
        if n_halos is not None:
            if n_halos < 0:
                failures.append("{}: attribute n_halos is {}, must be >= 0".format(prefix, n_halos))
            if n_halos > np.iinfo(np.int32).max:
                failures.append(
                    "{}: attribute n_halos is {}, exceeds the int32 topology bound {}".format(
                        prefix, n_halos, np.iinfo(np.int32).max
                    )
                )

        halos = handle["halos"]
        dataset_names = set(halos.keys())
        expected_datasets = set(HALO_DATASETS)
        if dataset_names != expected_datasets:
            failures.append(
                "{}: /halos dataset set mismatch: missing {}, extra {}".format(
                    prefix,
                    sorted(expected_datasets - dataset_names),
                    sorted(dataset_names - expected_datasets),
                )
            )
        for name in sorted(dataset_names & expected_datasets):
            dtype, is_vec = HALO_DATASETS[name]
            dataset = halos[name]
            if not isinstance(dataset, h5py.Dataset):
                failures.append("{}: /halos/{} is not a dataset".format(prefix, name))
                continue
            if dataset.dtype != np.dtype(dtype):
                failures.append(
                    "{}: /halos/{} dtype {} != contract {}".format(
                        prefix, name, dataset.dtype, np.dtype(dtype)
                    )
                )
            if is_vec:
                if dataset.ndim != 2 or dataset.shape[1] != 3:
                    failures.append(
                        "{}: /halos/{} shape {} is not [n_halos, 3]".format(
                            prefix, name, dataset.shape
                        )
                    )
            elif dataset.ndim != 1:
                failures.append(
                    "{}: /halos/{} shape {} is not [n_halos]".format(prefix, name, dataset.shape)
                )
            if n_halos is not None and dataset.shape and dataset.shape[0] != n_halos:
                failures.append(
                    "{}: /halos/{} length {} != header n_halos {}".format(
                        prefix, name, dataset.shape[0], n_halos
                    )
                )
            check_filters(dataset, "{}: /halos/{}".format(prefix, name), failures)
        return header


def check_sidecar(path, n_forests_total, failures):
    with h5py.File(path, "r") as handle:
        objects = set(handle.keys())
        if objects != {"ForestID"}:
            failures.append("forests.h5: object set {} != {{'ForestID'}}".format(sorted(objects)))
            return
        dataset = handle["ForestID"]
        if not isinstance(dataset, h5py.Dataset):
            failures.append("forests.h5: /ForestID is not a dataset")
            return
        if dataset.dtype != np.dtype(np.int64):
            failures.append("forests.h5: /ForestID dtype {} != int64".format(dataset.dtype))
        if dataset.ndim != 1:
            failures.append("forests.h5: /ForestID shape {} is not 1-D".format(dataset.shape))
        elif n_forests_total is not None and dataset.shape[0] != n_forests_total:
            failures.append(
                "forests.h5: /ForestID length {} != header n_forests_total {}".format(
                    dataset.shape[0], n_forests_total
                )
            )
        check_filters(dataset, "forests.h5: /ForestID", failures)


def check_dataset(directory):
    """Returns the list of conformance failures (empty when conforming)."""
    directory = Path(directory)
    failures = []
    if not directory.is_dir():
        return ["{}: not a directory".format(directory)]

    a_list_path = directory / A_LIST_NAME
    if not a_list_path.is_file():
        return ["{}: missing snapshot list {}".format(directory, A_LIST_NAME)]
    a_list = load_a_list(a_list_path)
    physical = load_physical_metadata(SIMULATION_INFO)

    expected_files = {"snapshot_{:03d}.h5".format(snap) for snap in range(len(a_list))}
    expected_files.add("forests.h5")
    present = {p.name for p in directory.glob("*.h5")}
    missing = sorted(expected_files - present)
    extra = sorted(present - expected_files)
    if missing:
        failures.append("{} missing file(s): {}".format(len(missing), ", ".join(missing)))
    if extra:
        failures.append("{} unexpected .h5 file(s): {}".format(len(extra), ", ".join(extra)))
    if missing:
        return failures

    headers = []
    for snap in range(len(a_list)):
        path = directory / "snapshot_{:03d}.h5".format(snap)
        try:
            header = check_snapshot_file(path, snap, a_list[snap], physical, failures)
        except OSError as exc:
            failures.append("{}: unreadable as HDF5 ({})".format(path.name, exc))
            header = None
        if header is not None:
            headers.append((path.name, header))

    for name in ("n_forests_total", "max_halo_rank_in_forest"):
        observed = {header[name] for _, header in headers if name in header}
        if len(observed) > 1:
            failures.append(
                "{} is not identical across files: observed {}".format(name, sorted(observed))
            )

    n_forests_total = None
    if headers and "n_forests_total" in headers[0][1]:
        n_forests_total = headers[0][1]["n_forests_total"]
    try:
        check_sidecar(directory / "forests.h5", n_forests_total, failures)
    except OSError as exc:
        failures.append("forests.h5: unreadable as HDF5 ({})".format(exc))
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="check_fixture_conformance",
        description="Structural conformance check for a committed snapshot-HDF5 fixture "
        "(chunk shape deliberately excluded)",
    )
    parser.add_argument(
        "directory",
        nargs="?",
        default=str(DATA_DIR),
        help="fixture directory to check (default: this package's committed _tests/data)",
    )
    args = parser.parse_args(argv)
    failures = check_dataset(args.directory)
    if failures:
        print("conformance: FAIL ({} defect(s))".format(len(failures)), file=sys.stderr)
        for failure in failures:
            print("  {}".format(failure), file=sys.stderr)
        return 1
    print("conformance: PASS ({})".format(args.directory))
    return 0


if __name__ == "__main__":
    sys.exit(main())
