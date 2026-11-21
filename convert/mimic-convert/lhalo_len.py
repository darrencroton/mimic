#!/usr/bin/env python3
"""Check and set the ``Len`` of L-Halo binary files.

In an L-Halo file written from Consistent-Trees catalogues, each record's ``Len`` is derived
from the record's own stored mass: ``Len = (int) roundf(M * (1.0 / PartMass))``, where ``M``
is the float32 mass in 1e10 Msun/h, ``1.0 / PartMass`` is a double, the product is narrowed
to float32 and rounded half away from zero (``ctrees_to_lhalo.py``, ``sage_len``). This tool
does two things with that relation, on files of the standard 104-byte record:

``check``
    Reports how many records' ``Len`` differs from the value a particle mass gives, and the
    interval of particle masses that reproduces every ``Len``. It writes nothing but an
    optional JSON result.

``rewrite``
    Writes a copy of each file, under the same name in another directory, in which only
    ``Len`` differs from the input (derived from the stored mass at the given particle
    mass), with a JSON sidecar recording the counts and hashes.

The file layout is ``int32 Ntrees``, ``int32 totNHalos``, ``int32 TreeNHalos[Ntrees]`` and
``totNHalos`` records of 104 bytes, little-endian. In the field order shared by the
``halo_properties.yaml`` of ``mini-uchuu``, ``micro-uchuu``, ``mini-millennium`` and
``millennium``, ``Len`` is the int32 at byte 20 and the float32 masses ``M_Mean200``,
``M_Crit200`` (``Mvir`` in ``ctrees_to_lhalo.py``) and ``M_TopHat`` sit at bytes 24, 28
and 32. Files are read and written in chunks, so memory stays bounded.

Exit statuses: 0 success; 1 ``check`` found a ``Len`` mismatch; 2 a read, layout or value
error (naming the file); 3 a usage error or a refused pre-flight check (nothing written).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import sys
from dataclasses import dataclass
from datetime import datetime, timezone

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import ctrees_to_lhalo as c2l  # noqa: E402
import yaml  # noqa: E402

EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_ERROR = 2
EXIT_USAGE = 3

RECORD_SIZE = 104
DEFAULT_CHUNK_RECORDS = 4_000_000
MASS_FIELDS = ("M_Crit200", "M_Mean200", "M_TopHat")
DEFAULT_MASS_FIELD = "M_Crit200"
PARTICLE_MASS_UNITS = "1e10 Msun/h"
MAX_EXAMPLES = 10
INT32_LIMIT = 2.0**31

# The fields this tool reads or writes, at their offsets in the 104-byte record.
RECORD_DTYPE = np.dtype(
    {
        "names": ["Len", "M_Mean200", "M_Crit200", "M_TopHat"],
        "formats": ["<i4", "<f4", "<f4", "<f4"],
        "offsets": [20, 24, 28, 32],
        "itemsize": RECORD_SIZE,
    }
)

SIDECAR_KEYS = (
    "input",
    "output",
    "particle_mass",
    "particle_mass_source",
    "mass_field",
    "n_records",
    "n_len_changed",
    "source_particle_mass",
    "n_len_mismatch_source",
    "sha256_input",
    "sha256_output",
    "tool_sha256",
    "started_utc",
    "finished_utc",
)


class LhaloLenError(Exception):
    """A read, layout or value error: exit status 2."""


class UsageError(Exception):
    """A usage error or a refused pre-flight check: exit status 3."""


# --------------------------------------------------------------------------------------
# The Len arithmetic
# --------------------------------------------------------------------------------------


def compute_len(
    masses,
    particle_mass: float,
    *,
    source: str = "<array>",
    first_index: int = 0,
    record_indices=None,
    allow_negative: bool = False,
) -> np.ndarray:
    """``Len`` of float32 ``masses`` at ``particle_mass``, as int32.

    ``int(roundf(float32(float64(M) * (1.0 / X))))``: the reciprocal is a double, the product
    is narrowed to float32 and rounded to the nearest integer, halves away from zero. A
    non-finite mass, a negative mass (unless ``allow_negative``) and a rounded value outside
    the int32 range raise ``LhaloLenError`` naming ``source`` and the record's index
    (``first_index`` plus its position, or ``record_indices[position]`` when given).
    """
    masses = np.asarray(masses)
    if masses.dtype != np.float32:
        raise TypeError("compute_len needs float32 masses")
    if not (math.isfinite(particle_mass) and particle_mass > 0):
        raise ValueError(f"particle mass must be positive and finite, not {particle_mass!r}")
    inverse = 1.0 / float(particle_mass)
    with np.errstate(over="ignore", invalid="ignore"):
        product = (masses.astype(np.float64) * np.float64(inverse)).astype(np.float32)
        rounded = c2l._roundf(product)
    nonfinite = ~np.isfinite(masses)
    negative = masses < 0
    out_of_range = ~((rounded >= -INT32_LIMIT) & (rounded < INT32_LIMIT))
    bad = nonfinite | out_of_range
    if not allow_negative:
        bad = bad | negative
    if bad.any():
        position = int(np.argmax(bad))
        index = (
            int(record_indices[position]) if record_indices is not None else first_index + position
        )
        mass = float(masses[position])
        if nonfinite[position]:
            reason = "non-finite mass"
        elif negative[position] and not allow_negative:
            reason = "negative mass (its Len is not mass-derived)"
        else:
            reason = f"rounded Len {float(rounded[position])!r} is outside the int32 range"
        raise LhaloLenError(f"{source}: record {index}: {reason}, mass {mass!r}")
    return rounded.astype(np.int32)


# --------------------------------------------------------------------------------------
# Files
# --------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Header:
    """The checked header of an L-Halo file."""

    n_trees: int
    n_halos: int
    offset: int  # bytes before the first record
    size: int  # file size in bytes


def read_header(path: str) -> Header:
    """Read and check an L-Halo file's header and size; ``LhaloLenError`` naming the file."""
    try:
        size = os.path.getsize(path)
        with open(path, "rb") as handle:
            head = handle.read(8)
            if len(head) < 8:
                raise LhaloLenError(f"{path}: {size} bytes, shorter than the 8-byte header")
            n_trees, n_halos = (int(v) for v in np.frombuffer(head, dtype="<i4"))
            if n_trees < 0 or n_halos < 0:
                raise LhaloLenError(
                    f"{path}: negative header count (Ntrees {n_trees}, totNHalos {n_halos})"
                )
            if size < 8 + 4 * n_trees:
                raise LhaloLenError(
                    f"{path}: {size} bytes, too short for the {n_trees} per-tree counts"
                )
            counts = np.frombuffer(handle.read(4 * n_trees), dtype="<i4")
    except OSError as err:
        raise LhaloLenError(f"{path}: cannot read: {err}") from err
    if len(counts) != n_trees:
        raise LhaloLenError(f"{path}: the per-tree counts end early")
    if (counts < 0).any():
        raise LhaloLenError(f"{path}: negative per-tree halo count")
    total = int(counts.astype(np.int64).sum())
    if total != n_halos:
        raise LhaloLenError(f"{path}: TreeNHalos sums to {total}, header totNHalos is {n_halos}")
    offset = 4 * (2 + n_trees)
    if size != offset + RECORD_SIZE * n_halos:
        raise LhaloLenError(
            f"{path}: {size} bytes, not 4 * (2 + {n_trees}) + {RECORD_SIZE} * {n_halos}"
            f" = {offset + RECORD_SIZE * n_halos}"
        )
    return Header(n_trees, n_halos, offset, size)


def _read_exact(handle, view: memoryview, path: str) -> None:
    """Fill ``view`` from ``handle`` or raise ``LhaloLenError`` (a file that ended early)."""
    filled = 0
    while filled < len(view):
        got = handle.readinto(view[filled:])
        if not got:
            raise LhaloLenError(f"{path}: the file ended early (changed while being read?)")
        filled += got


def _tool_sha256() -> str:
    with open(os.path.abspath(__file__), "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# --------------------------------------------------------------------------------------
# Particle mass
# --------------------------------------------------------------------------------------


def parse_particle_mass(text: str, option: str) -> float:
    """A positive finite number from command-line text; ``UsageError`` naming the option."""
    try:
        value = float(text)
    except (TypeError, ValueError):
        value = math.nan
    if not (math.isfinite(value) and value > 0):
        raise UsageError(f"{option} must be a positive finite number, not {text!r}")
    return value


def load_simulation_info_particle_mass(path: str) -> float:
    """``simulation.particle_mass.value`` of a package's ``simulation_info.yaml`` (1e10 Msun/h)."""
    try:
        with open(path, encoding="utf-8") as handle:
            data = yaml.safe_load(handle)
    except (OSError, yaml.YAMLError) as err:
        raise UsageError(f"{path}: cannot read the simulation info: {err}") from err
    try:
        entry = data["simulation"]["particle_mass"]
        value, units = entry["value"], entry["units"]
    except (KeyError, TypeError) as err:
        raise UsageError(f"{path}: no simulation.particle_mass value and units") from err
    if units != PARTICLE_MASS_UNITS:
        raise UsageError(
            f"{path}: simulation.particle_mass units are {units!r}, not {PARTICLE_MASS_UNITS!r}"
        )
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise UsageError(f"{path}: simulation.particle_mass.value {value!r} is not a number")
    value = float(value)
    if not (math.isfinite(value) and value > 0):
        raise UsageError(f"{path}: simulation.particle_mass.value {value!r} is not positive")
    return value


def resolve_particle_mass(particle_mass: str | None, simulation_info: str | None):
    """``(value, source text)`` from exactly one of the two options; ``UsageError`` otherwise."""
    if (particle_mass is None) == (simulation_info is None):
        raise UsageError("give exactly one of --particle-mass and --simulation-info")
    if particle_mass is not None:
        return parse_particle_mass(particle_mass, "--particle-mass"), particle_mass
    return (
        load_simulation_info_particle_mass(simulation_info),
        f"{simulation_info}:simulation.particle_mass",
    )


# --------------------------------------------------------------------------------------
# check
# --------------------------------------------------------------------------------------


def check_files(
    paths,
    particle_mass: float,
    mass_field: str = DEFAULT_MASS_FIELD,
    chunk_records: int = DEFAULT_CHUNK_RECORDS,
) -> dict:
    """Compare every record's ``Len`` with its value at ``particle_mass``; the result dict.

    Negative masses are counted and left out of the comparison and the interval. The interval
    is formed from the unique (mass bits, ``Len``) pairs accumulated chunk by chunk.
    """
    headers = [(path, read_header(path)) for path in paths]
    n_records = n_mismatch = n_negative = 0
    examples: list[dict] = []
    keys = np.empty(0, dtype=np.uint64)
    for path, header in headers:
        n_records += header.n_halos
        buffer = bytearray(min(chunk_records, max(header.n_halos, 1)) * RECORD_SIZE)
        try:
            with open(path, "rb") as handle:
                handle.seek(header.offset)
                done = 0
                while done < header.n_halos:
                    count = min(chunk_records, header.n_halos - done)
                    view = memoryview(buffer)[: count * RECORD_SIZE]
                    _read_exact(handle, view, path)
                    records = np.frombuffer(buffer, dtype=RECORD_DTYPE, count=count)
                    mass = np.ascontiguousarray(records[mass_field])
                    length = np.ascontiguousarray(records["Len"])
                    indices = done + np.arange(count)
                    finite = np.isfinite(mass)
                    if not finite.all():
                        position = int(np.argmin(finite))
                        raise LhaloLenError(
                            f"{path}: record {done + position}: non-finite mass "
                            f"{float(mass[position])!r}"
                        )
                    negative = mass < 0
                    n_negative += int(negative.sum())
                    keep = ~negative
                    mass, length, indices = mass[keep], length[keep], indices[keep]
                    expected = compute_len(mass, particle_mass, source=path, record_indices=indices)
                    wrong = expected != length
                    n_mismatch += int(wrong.sum())
                    for p in np.flatnonzero(wrong)[: MAX_EXAMPLES - len(examples)]:
                        examples.append(
                            {
                                "file": path,
                                "record": int(indices[p]),
                                "mass": float(mass[p]),
                                "len": int(length[p]),
                                "expected_len": int(expected[p]),
                            }
                        )
                    # -0.0 and 0.0 are the same mass: one key.
                    bits = (mass + np.float32(0)).view(np.uint32).astype(np.uint64)
                    chunk_keys = np.unique(
                        (bits << np.uint64(32))
                        | (length.astype(np.int64) & 0xFFFFFFFF).astype(np.uint64)
                    )
                    keys = np.unique(np.concatenate([keys, chunk_keys]))
                    done += count
        except OSError as err:
            raise LhaloLenError(f"{path}: cannot read: {err}") from err
    interval, status, statement = (
        None,
        "none_comparable",
        ("no interval: no record has a non-negative mass to compare"),
    )
    if len(keys):
        mvir = (keys >> np.uint64(32)).astype(np.uint32).view(np.float32)
        length = (keys & np.uint64(0xFFFFFFFF)).astype(np.uint32).view(np.int32)
        found = c2l.particle_mass_interval(mvir, length)
        if found.empty:
            status = "none_reproduces"
            statement = "no particle mass reproduces every Len of the comparable records"
        else:
            status = "found"
            statement = f"particle masses {found.low!r} to {found.high!r} reproduce every Len"
            interval = {
                "low": found.low,
                "high": found.high,
                "value": found.value,
                "n_unique_pairs": found.n_checked,
            }
    return {
        "n_files": len(headers),
        "particle_mass": particle_mass,
        "mass_field": mass_field,
        "n_records": n_records,
        "n_len_mismatch": n_mismatch,
        "examples": examples,
        "n_negative_mass": n_negative,
        "interval": interval,
        "interval_status": status,
        "interval_statement": statement,
    }


def _require_json_target(target: str, paths) -> None:
    """Refuse a ``--json`` that could damage data or fail late; ``UsageError`` naming it."""
    real = os.path.realpath(target)
    for path in paths:
        if os.path.realpath(path) == real:
            raise UsageError(f"--json {target} is the input file {path}")
    if os.path.lexists(target) and not os.path.isfile(target):
        raise UsageError(f"--json {target} exists and is not a regular file")
    parent = os.path.dirname(target) or "."
    if not os.path.isdir(parent):
        raise UsageError(f"--json {target}: the directory {parent} does not exist")
    if os.path.lexists(f"{target}.tmp"):
        raise UsageError(f"--json {target}: the temporary file {target}.tmp already exists")


def _write_json_atomic(path: str, data: dict) -> None:
    temporary = f"{path}.tmp"
    created = False
    try:
        with open(temporary, "x", encoding="utf-8") as handle:
            created = True
            json.dump(data, handle, indent=2)
            handle.write("\n")
        os.replace(temporary, path)
    except OSError as err:
        if created and os.path.lexists(temporary):
            os.remove(temporary)
        raise LhaloLenError(f"{path}: cannot write: {err}") from err


def _command_check(args) -> int:
    particle_mass, source = resolve_particle_mass(args.particle_mass, args.simulation_info)
    _require_expected_files(args.paths, args.expect_files)
    for path in args.paths:
        _require_readable(path)
    if args.json:
        _require_json_target(args.json, args.paths)
    result = check_files(args.paths, particle_mass, args.mass_field, args.chunk_records)
    result["particle_mass_source"] = source
    print(
        f"checked {result['n_records']} records in {result['n_files']} file(s) at particle mass "
        f"{particle_mass!r} ({args.mass_field})"
    )
    print(f"  Len mismatches: {result['n_len_mismatch']}")
    for example in result["examples"]:
        print(
            f"    {example['file']} record {example['record']}: mass {example['mass']!r}, "
            f"Len {example['len']}, expected {example['expected_len']}"
        )
    print(f"  negative masses (left out): {result['n_negative_mass']}")
    print(f"  interval: {result['interval_statement']}")
    if result["interval"]:
        print(f"  shortest value: {result['interval']['value']}")
    if args.json:
        _write_json_atomic(args.json, result)
    return EXIT_OK if result["n_len_mismatch"] == 0 else EXIT_MISMATCH


# --------------------------------------------------------------------------------------
# rewrite
# --------------------------------------------------------------------------------------


def _output_names(name: str) -> dict:
    return {
        "output": name,
        "sidecar": f"{name}.lhalo-len.json",
        "output_tmp": f".{name}.lhalo-len-tmp",
        "sidecar_tmp": f".{name}.lhalo-len.json.tmp",
    }


def _require_expected_files(paths, expected: int) -> None:
    if expected != len(paths):
        raise UsageError(f"--expect-files is {expected} but {len(paths)} PATH(s) were given")


def _require_readable(path: str) -> None:
    if not os.path.isfile(path) or not os.access(path, os.R_OK):
        raise UsageError(f"{path}: not an existing readable regular file")


def _rewrite_preflight(paths, out_dir: str) -> None:
    """The usage checks of ``rewrite`` that precede any header check or write."""
    for path in paths:
        _require_readable(path)
    real = {}
    names = {}
    for path in paths:
        resolved = os.path.realpath(path)
        if resolved in real:
            raise UsageError(f"{real[resolved]} and {path} are the same file")
        real[resolved] = path
        name = os.path.basename(path)
        if name in names:
            raise UsageError(f"{names[name]} and {path} give the same output name {name!r}")
        names[name] = path
    out_real = os.path.realpath(out_dir)
    for path in paths:
        for directory in (
            os.path.realpath(os.path.dirname(path) or "."),
            os.path.dirname(os.path.realpath(path)),
        ):
            if out_real == directory:
                raise UsageError(f"--out {out_dir} is the directory of {path}: {directory}")
    if os.path.lexists(out_dir) and not os.path.isdir(out_dir):
        raise UsageError(f"--out {out_dir} exists and is not a directory")
    if os.path.isdir(out_dir):
        for path in paths:
            for file_name in _output_names(os.path.basename(path)).values():
                existing = os.path.join(out_dir, file_name)
                if os.path.lexists(existing):
                    raise UsageError(f"{existing} already exists")


def rewrite_file(
    path: str,
    out_dir: str,
    particle_mass: float,
    particle_mass_source: str,
    mass_field: str = DEFAULT_MASS_FIELD,
    source_particle_mass: float | None = None,
    chunk_records: int = DEFAULT_CHUNK_RECORDS,
) -> dict:
    """Write ``path`` into ``out_dir`` with only ``Len`` changed; return the sidecar dict.

    The output is written to a temporary name in ``out_dir`` and renamed when complete, then
    the sidecar is written the same way. On an error the temporary files are removed and no
    sidecar is written.
    """
    started = _utc_now()
    header = read_header(path)
    names = _output_names(os.path.basename(path))
    output = os.path.join(out_dir, names["output"])
    sidecar_path = os.path.join(out_dir, names["sidecar"])
    output_tmp = os.path.join(out_dir, names["output_tmp"])
    sidecar_tmp = os.path.join(out_dir, names["sidecar_tmp"])
    for existing in (output, sidecar_path, output_tmp, sidecar_tmp):
        if os.path.lexists(existing):  # never overwritten, and never removed by the cleanup below
            raise LhaloLenError(f"{existing} already exists")
    hash_in, hash_out = hashlib.sha256(), hashlib.sha256()
    n_changed = n_source_mismatch = 0
    buffer = bytearray(min(chunk_records, max(header.n_halos, 1)) * RECORD_SIZE)
    try:
        with open(path, "rb") as source, open(output_tmp, "xb") as target:
            prefix = bytearray(header.offset)
            _read_exact(source, memoryview(prefix), path)
            hash_in.update(prefix)
            hash_out.update(prefix)
            target.write(prefix)
            done = 0
            while done < header.n_halos:
                count = min(chunk_records, header.n_halos - done)
                view = memoryview(buffer)[: count * RECORD_SIZE]
                _read_exact(source, view, path)
                hash_in.update(view)
                records = np.frombuffer(buffer, dtype=RECORD_DTYPE, count=count)
                mass = np.ascontiguousarray(records[mass_field])
                new_len = compute_len(mass, particle_mass, source=path, first_index=done)
                if source_particle_mass is not None:
                    at_source = compute_len(
                        mass,
                        source_particle_mass,
                        source=f"{path} (--source-particle-mass {source_particle_mass!r})",
                        first_index=done,
                    )
                    n_source_mismatch += int(np.count_nonzero(at_source != records["Len"]))
                n_changed += int(np.count_nonzero(new_len != records["Len"]))
                records["Len"] = new_len
                hash_out.update(view)
                target.write(view)
                done += count
            if source.read(1):
                raise LhaloLenError(f"{path}: the file grew while being read")
            target.flush()
            os.fsync(target.fileno())
        os.rename(output_tmp, output)
        sidecar = {
            "input": path,
            "output": output,
            "particle_mass": particle_mass,
            "particle_mass_source": particle_mass_source,
            "mass_field": mass_field,
            "n_records": header.n_halos,
            "n_len_changed": n_changed,
            "source_particle_mass": source_particle_mass,
            "n_len_mismatch_source": (
                n_source_mismatch if source_particle_mass is not None else None
            ),
            "sha256_input": hash_in.hexdigest(),
            "sha256_output": hash_out.hexdigest(),
            "tool_sha256": _tool_sha256(),
            "started_utc": started,
            "finished_utc": _utc_now(),
        }
        _write_sidecar(sidecar, sidecar_tmp, sidecar_path)
    except OSError as err:
        raise LhaloLenError(f"{path}: {err}") from err
    finally:
        for leftover in (output_tmp, sidecar_tmp):
            if os.path.lexists(leftover):
                os.remove(leftover)
    return sidecar


def _write_sidecar(sidecar: dict, temporary: str, final: str) -> None:
    with open(temporary, "x", encoding="utf-8") as handle:
        json.dump(sidecar, handle, indent=2)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.rename(temporary, final)


def _command_rewrite(args) -> int:
    particle_mass, source = resolve_particle_mass(args.particle_mass, args.simulation_info)
    source_mass = None
    if args.source_particle_mass is not None:
        source_mass = parse_particle_mass(args.source_particle_mass, "--source-particle-mass")
    _require_expected_files(args.paths, args.expect_files)
    _rewrite_preflight(args.paths, args.out)
    for path in args.paths:
        read_header(path)
    try:
        os.makedirs(args.out, exist_ok=True)
    except OSError as err:
        raise LhaloLenError(f"{args.out}: cannot create: {err}") from err
    for path in args.paths:
        sidecar = rewrite_file(
            path,
            args.out,
            particle_mass,
            source,
            args.mass_field,
            source_mass,
            args.chunk_records,
        )
        print(
            f"wrote {sidecar['output']}: {sidecar['n_records']} records, "
            f"{sidecar['n_len_changed']} Len changed"
        )
        if sidecar["n_len_mismatch_source"]:
            print(
                f"  note: {sidecar['n_len_mismatch_source']} input Len differ from the value at "
                f"source particle mass {source_mass!r}"
            )
    return EXIT_OK


# --------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------


class _ArgumentParser(argparse.ArgumentParser):
    """argparse with its usage exit (status 2) turned into a ``UsageError`` (status 3)."""

    def error(self, message):
        raise UsageError(message)


def _positive_int(text: str) -> int:
    try:
        value = int(text)
    except ValueError:
        value = 0
    if value < 1:
        raise argparse.ArgumentTypeError(f"must be a positive integer, not {text!r}")
    return value


def _add_common(parser) -> None:
    parser.add_argument("--particle-mass", metavar="X", help="particle mass, 1e10 Msun/h")
    parser.add_argument(
        "--simulation-info",
        metavar="PATH",
        help="a package's simulation_info.yaml (simulation.particle_mass, units 1e10 Msun/h)",
    )
    parser.add_argument(
        "--mass-field",
        choices=MASS_FIELDS,
        default=DEFAULT_MASS_FIELD,
        help=f"mass Len is derived from (default {DEFAULT_MASS_FIELD})",
    )
    parser.add_argument(
        "--expect-files", type=int, required=True, metavar="N", help="the number of PATHs"
    )
    parser.add_argument(
        "--chunk-records",
        type=_positive_int,
        default=DEFAULT_CHUNK_RECORDS,
        metavar="N",
        help=f"records read at a time (default {DEFAULT_CHUNK_RECORDS})",
    )
    parser.add_argument("paths", nargs="+", metavar="PATH", help="L-Halo files")


def build_parser() -> argparse.ArgumentParser:
    parser = _ArgumentParser(
        prog="lhalo_len.py",
        description="Check and set the Len of L-Halo binary files (104-byte records).",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="compare Len with a particle mass; write nothing")
    _add_common(check)
    check.add_argument("--json", metavar="OUT", help="write the full result as JSON")
    check.set_defaults(handler=_command_check)
    rewrite = commands.add_parser("rewrite", help="write copies with Len set at a particle mass")
    _add_common(rewrite)
    rewrite.add_argument("--out", required=True, metavar="DIR", help="output directory")
    rewrite.add_argument(
        "--source-particle-mass",
        metavar="Y",
        help="the particle mass the input is believed made at; its Len are checked against it",
    )
    rewrite.set_defaults(handler=_command_rewrite)
    return parser


def _join_mass_options(argv):
    """Join ``--particle-mass VALUE`` into ``--particle-mass=VALUE`` so a value such as
    ``-inf`` or ``-1`` reaches the check that names it, not argparse's own message."""
    joined, iterator = [], iter(argv)
    for token in iterator:
        if token in ("--particle-mass", "--source-particle-mass"):
            value = next(iterator, None)
            joined.append(token if value is None else f"{token}={value}")
        else:
            joined.append(token)
    return joined


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    try:
        args = build_parser().parse_args(_join_mass_options(argv))
        return args.handler(args)
    except UsageError as err:
        print(f"lhalo_len.py: usage error: {err}", file=sys.stderr)
        return EXIT_USAGE
    except LhaloLenError as err:
        print(f"lhalo_len.py: error: {err}", file=sys.stderr)
        return EXIT_ERROR


if __name__ == "__main__":
    sys.exit(main())
