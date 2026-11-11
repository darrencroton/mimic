"""Provenance: the inputs a stage reads, hashed from the bytes it uses, and who ran it.

Every stage reads its spec and constraint table once, through :func:`read_spec_inputs`: the
SHA-256 it records is that of the bytes it parsed, so an edit made while a stage runs cannot
change what it reports having used. The same function computes the **request hash**
(:func:`request_sha256`), the SHA-256 of what defines an evaluation: the spec's
:data:`REQUEST_SECTIONS` as validated, with the constraint table's path replaced by the SHA-256 of
its bytes. ``capture`` records it; ``select-outputs`` and ``analyse`` recompute it from their
spec and refuse a capture whose request differs. The design, the noise model, the output lists,
the discrepancy budgets, the rejection rule, the criteria and the emulators are not part of it:
they choose what is run or how it is judged, not what one evaluation is, and the design is bound
by ``design.csv``'s own SHA-256 in the capture's manifest.

:func:`package_identity` and :func:`library_versions` describe the code that ran a stage; each
stage takes them once, when it starts.

Usage:
    inputs = read_spec_inputs(Path("specs/example.yaml"))
    print(inputs.request_sha256, package_identity()["commit"])
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import subprocess
from dataclasses import dataclass
from importlib import metadata
from pathlib import Path
from typing import Any, Mapping

from mimic_bench import __version__
from mimic_bench.constraints import ConstraintError, ConstraintTable, parse_constraint_table
from mimic_bench.spec import Spec, SpecError, parse_spec_text

REQUEST_SECTIONS = (
    "model",
    "simulation",
    "run_file",
    "parameters",
    "inert_control",
    "statistic",
    "constraint",
)
"""The spec sections that define an evaluation, and so the request hash."""

LIBRARIES = ("numpy", "scipy", "h5py", "PyYAML", "scikit-learn", "matplotlib")
"""The distributions whose versions a stage records."""

PACKAGE_DIRECTORY = Path(__file__).resolve().parents[1]
"""``bench/mimic-bench``: the directory whose git state is the package's identity."""


def sha256_bytes(data: bytes) -> str:
    """Return the SHA-256 of bytes, as hex."""
    return hashlib.sha256(data).hexdigest()


@dataclass(frozen=True, eq=False)
class SpecInputs:
    """A spec and its constraint table, parsed from the bytes that were hashed.

    Attributes:
        spec: The validated spec.
        spec_path: The spec file, as given.
        spec_sha256: The SHA-256 of the spec's bytes (provenance only).
        constraint_path: The constraint table, resolved against the spec's directory.
        constraint: The unconverted constraint table.
        constraint_sha256: The SHA-256 of the constraint table's bytes.
        request_sha256: The request hash (:func:`request_sha256`).
    """

    spec: Spec
    spec_path: Path
    spec_sha256: str
    constraint_path: Path
    constraint: ConstraintTable
    constraint_sha256: str
    request_sha256: str


def read_spec_inputs(spec_path: Path) -> SpecInputs:
    """Read the spec and its constraint table once each, hash those bytes and parse them.

    Raises:
        SpecError: If the spec cannot be read or is invalid.
        ConstraintError: If the constraint table cannot be read or is invalid.
    """
    spec_path = Path(spec_path)
    try:
        spec_bytes = spec_path.read_bytes()
    except OSError as exc:
        raise SpecError(f"{spec_path}: cannot read spec ({exc.strerror})") from exc
    spec = parse_spec_text(spec_bytes, spec_path)
    constraint_path = spec.constraint_path
    try:
        constraint_bytes = constraint_path.read_bytes()
    except OSError as exc:
        raise ConstraintError(
            f"{constraint_path}: cannot read constraint table ({exc.strerror})"
        ) from exc
    constraint_sha256 = sha256_bytes(constraint_bytes)
    return SpecInputs(
        spec=spec,
        spec_path=spec_path,
        spec_sha256=sha256_bytes(spec_bytes),
        constraint_path=constraint_path,
        constraint=parse_constraint_table(constraint_bytes, constraint_path),
        constraint_sha256=constraint_sha256,
        request_sha256=request_sha256(spec, constraint_sha256),
    )


def request_sha256(spec: Spec, constraint_sha256: str) -> str:
    """Return the request hash: the SHA-256 of the canonical JSON of the evaluation's definition.

    The document holds each of :data:`REQUEST_SECTIONS` as validated (so ``10`` and ``10.0`` give
    the same hash), with ``constraint.table`` replaced by ``constraint_sha256``, and floats
    written as ``repr`` writes them. Keys are not sorted: a section's fields are in their fixed
    order and a mapping in ``run_file.overrides`` keeps its YAML order, which Mimic can act on
    (it runs ``modules.phases`` in the order written), so a reordered mapping gives a new hash.

    Args:
        spec: The validated spec.
        constraint_sha256: The SHA-256 of the constraint table's bytes.

    Returns:
        The hash, as hex.
    """
    document: dict[str, Any] = {}
    for name in REQUEST_SECTIONS:
        value = getattr(spec, name)
        if isinstance(value, tuple):
            value = [dataclasses.asdict(item) for item in value]
        elif dataclasses.is_dataclass(value):
            value = dataclasses.asdict(value)
        document[name] = value
    document["constraint"]["table"] = {"sha256": constraint_sha256}
    text = json.dumps(document, separators=(",", ":"), allow_nan=False)
    return sha256_bytes(text.encode("utf-8"))


def request_mismatch(context: Mapping[str, Any], inputs: SpecInputs, path: Path) -> str | None:
    """Compare a capture's recorded request hash with the spec's; return the error, if any.

    Args:
        context: The capture's ``context.json``.
        inputs: The spec and constraint table the reading stage uses.
        path: The ``context.json`` file, named in the message.

    Returns:
        ``None`` when the hashes are equal, else a message naming both files.
    """
    recorded = context.get("request_sha256")
    if recorded == inputs.request_sha256:
        return None
    return (
        f"{path}: the capture's request {recorded} is not that of spec {inputs.spec_path} and "
        f"constraint table {inputs.constraint_path} ({inputs.request_sha256}); a section of "
        f"{', '.join(REQUEST_SECTIONS)} or the table differs from the capture's"
    )


def package_identity() -> dict[str, Any]:
    """Return this package's name, version, git commit and whether it has local changes.

    The commit is the repository's HEAD and ``dirty`` whether ``git status --porcelain`` reports
    anything under :data:`PACKAGE_DIRECTORY`; both are ``None`` when git is unavailable or the
    package's files are not tracked (no commit then describes them).
    """
    identity: dict[str, Any] = {"name": "mimic_bench", "version": __version__}

    def git(*arguments: str) -> str:
        return subprocess.run(
            ["git", "-C", str(PACKAGE_DIRECTORY), *arguments],
            capture_output=True,
            text=True,
            check=True,
        ).stdout

    try:
        if not git("ls-files", "--", "mimic_bench").strip():
            return {**identity, "commit": None, "dirty": None}
        commit = git("rev-parse", "HEAD").strip()
        dirty = bool(git("status", "--porcelain", "--", ".").strip())
    except (OSError, subprocess.CalledProcessError):
        return {**identity, "commit": None, "dirty": None}
    return {**identity, "commit": commit, "dirty": dirty}


def library_versions() -> dict[str, Any]:
    """Return the versions of :data:`LIBRARIES` and the BLAS and LAPACK libraries in use.

    ``blas_lapack`` holds what ``threadpoolctl.threadpool_info()`` finds loaded (it does not see
    every vendor library) and, for numpy and scipy, the BLAS and LAPACK they were built against.
    A distribution that is not installed, and any of these that cannot be read (a build
    configuration of another shape, an error from threadpoolctl), is recorded as ``None``: the
    record never stops a stage.
    """
    import numpy
    import scipy
    import scipy.linalg  # noqa: F401 (loads scipy's LAPACK, so threadpoolctl can see it)

    def built_against(module: Any) -> dict[str, str] | None:
        try:
            dependencies = module.show_config(mode="dicts")["Build Dependencies"]
            return {
                api: f"{dependencies[api]['name']} {dependencies[api]['version']}"
                for api in ("blas", "lapack")
            }
        except Exception:  # provenance only: an unreadable configuration is recorded as None
            return None

    def loaded() -> list[dict[str, Any]] | None:
        try:
            from threadpoolctl import threadpool_info

            return [
                {key: entry.get(key) for key in ("internal_api", "version", "filepath")}
                for entry in threadpool_info()
                if entry.get("user_api") == "blas"
            ]
        except Exception:  # provenance only: a threadpoolctl failure is recorded as None
            return None

    versions: dict[str, Any] = {}
    for name in LIBRARIES:
        try:
            versions[name] = metadata.version(name)
        except metadata.PackageNotFoundError:
            versions[name] = None
    versions["blas_lapack"] = {
        "loaded": loaded(),
        "built_against": {module.__name__: built_against(module) for module in (numpy, scipy)},
    }
    return versions
