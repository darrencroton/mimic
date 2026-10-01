#!/usr/bin/env python3
"""
Cross-format identity gate for the version 2 horizontal package.

The same simulation, read through two different drivers, must produce the same
galaxies. This gate runs micro-Uchuu through the vertical driver
(``micro-uchuu-ascii``, Consistent-Trees ASCII) and through the horizontal
driver (``micro-uchuu-ascii-horizontal``, version 2 snapshot HDF5) and requires, for
every output snapshot, identical ``UniqueGalaxyID`` sets and per-id
**byte-identical** fields, under ``halos-only`` and ``sage16``, each with fixed
and dynamic timesteps. There is no tolerance, no field exclusion and no
sampling anywhere in it.

It is a manual, dataset-present operation: both micro-Uchuu datasets are
machine-local and large, and the gate builds five executables and performs nine
full runs, so it takes hours. It is registered only when the selected package is
``micro-uchuu-ascii-horizontal``::

    make MODEL=halos-only SIMULATION=micro-uchuu-ascii-horizontal tests-scientific

A missing dataset **fails** the gate with the path it looked for. It never skips:
a gate that quietly reports success because it found nothing to compare is worse
than no gate, and this one is the whole evidentiary basis of the horizontal driver.

Stages 1-7 (preconditions, dataset provenance, run files, builds, the four
parity legs, leg verdicts) come from ``tests/framework/parity_gate.py``. Two
rules are specific to this package:

- ``sage16`` does not start until **both** halos-only legs have passed: a
  divergence in either is a driver bug and is reported before the sage16 cost
  is paid, and a ``halos-only`` pass alone is not the gate.
- the vertical run must write several partition files, or the comparator's
  partition aggregation is untested (the vertical side writes five; comparing
  one file would silently compare a fifth of the run).

Stage 8, vertical-path preservation, is this file's own: the same vertical run
built from the BASELINE_COMMIT reference commit, whose galaxy records must be
byte-identical to HEAD's and whose HDF5 metadata must differ in exactly the
permitted deltas (empty at the current anchor) beyond five provenance
attributes that carry no scientific content.

Worktrees and scratch outputs are removed on every exit path.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO_ROOT = next(
    p for p in Path(__file__).resolve().parents if (p / "tests" / "framework").is_dir()
)
sys.path.insert(0, str(REPO_ROOT / "tests"))

import h5py  # noqa: E402
import numpy  # noqa: E402
from framework.parity_gate import (  # noqa: E402
    SNAP_GROUP_RE,
    GatePackage,
    ParityGate,
    RunOutput,
    attr_value,
    banner,
    comparator,
    log,
)

PACKAGE = GatePackage(
    vertical="micro-uchuu-ascii",
    horizontal="micro-uchuu-ascii-horizontal",
    alist="micro-uchuu.a_list",
    evidence="complete real data (the whole micro-Uchuu Consistent-Trees ASCII catalogue)",
    # first_file/last_file are metadata only for the ASCII reader; both packages declare 0-0.
    file_range=(0, 0),
    override_vertical_range=False,
    format_version=2,
    source_format="consistent_trees_ascii",
    column_mapping_sha256=None,
    links_adjacent=1,
    halos=22_580_924,
    forests=440_651,
    gapped_descendants=0,
    max_descendant_span=1,
    required_free_bytes=20 * 1024**3,
    models=("halos-only", "sage16"),
    vertical_dataset_files=("forests.list", "locations.dat", "tree_0_0_0.dat"),
    require_halos_only_before_sage16=True,
)

#: The vertical-path-preservation reference commit: periodically advanced, not a
#: permanent invariant. Moved `ae22d278` -> `a654c228` (2026-09-10) after the
#: validated `fix_flybys` removal (it merged unrelated FoF groups at each forest's
#: final snapshot).
#: Moved `a654c228` -> `aedded2f` (2026-09-28) after the 94c4f22e Uchuu-family
#: particle-mass correction (0.0325 -> 0.0327), verified by a full-field,
#: all-record comparison of a654c228 against aedded2f over all 4,409,643
#: records: only Len and the fields it derives -- satellite Mvir, Rvir, Vvir,
#: deltaMvir, infallVvir -- changed. Len matched the rounding of
#: Len = round(Mvir*1e-10/PartMass) record for record, and Mvir, Rvir and Vvir
#: followed from it exactly; deltaMvir and infallVvir derive from those in code.
#: Every other field, dtype, shape and the dataset set were identical.
#: See PERMITTED_DELTAS below for what a re-anchor must reset.
BASELINE_COMMIT = "aedded2f"

#: HDF5 attributes that legitimately differ between two builds/runs of the same
#: code and carry no scientific content, mapped to the object paths where they
#: legitimately live. Reported separately, never counted as a delta, and never
#: silently dropped -- but excused only where they belong: the same name
#: appearing anywhere else is a real difference, and excusing it by name alone
#: would let a genuine metadata change hide behind a provenance label.
PROVENANCE_ATTR_PATHS = {
    "git_commit": ("/RunProperties/Version",),
    "git_branch": ("/RunProperties/Version",),
    "git_date": ("/RunProperties/Version",),
    "build_date": ("/RunProperties/Version",),
    "RunEndTime": ("/RunProperties",),
}

#: HDF5 metadata deltas permitted between BASELINE_COMMIT and HEAD, beyond the
#: provenance attributes in PROVENANCE_ATTR_PATHS. Empty at a fresh anchor --
#: every entry needs a matching classify() case pinning its exact object path
#: and before/after transition. Add an entry only when a new, deliberate schema
#: change needs pinning; when BASELINE_COMMIT next advances, reset this tuple,
#: classify()'s special cases and assert_output_schema_delta's `expected` set
#: back to empty -- a fresh anchor starts with nothing yet to permit.
PERMITTED_DELTAS = ()

#: output_schema.json top-level keys excluded from assert_output_schema_delta.
#: `source_md5` is a hash of the generator and its property/unit YAML inputs,
#: not of the schema itself, so it changes whenever either does, even when the
#: schema it describes -- the field list, types and units -- is bit-identical.
#: Excluded by exact key, the same way PROVENANCE_ATTR_PATHS excludes by exact
#: attribute path: only this key is exempt, nothing else silently rides along.
SCHEMA_PROVENANCE_KEYS = {".source_md5"}


# --------------------------------------------------------------------------
# Vertical-path preservation against the BASELINE_COMMIT reference
# --------------------------------------------------------------------------


def h5_structure(path: Path) -> dict:
    """Walk one HDF5 file: every object, attribute, link and non-record payload.

    Galaxies datasets are recorded by schema and shape only; their records are
    compared separately and byte-wise. Everything else -- including the payloads
    of Redshifts and FieldMetadata -- is captured as raw bytes, because this walk
    is the metadata delta check and a delta hidden in a payload is still a delta.
    """
    structure: dict = {}

    def attrs_of(obj):
        return {name: attr_value(value) for name, value in obj.attrs.items()}

    def walk(group, prefix):
        entry = {"kind": "group", "attrs": attrs_of(group), "links": {}}
        structure[prefix or "/"] = entry
        for key in sorted(group.keys()):
            link = group.get(key, getlink=True)
            child_path = f"{prefix}/{key}" if prefix else f"/{key}"
            if isinstance(link, h5py.ExternalLink):
                entry["links"][key] = ("external", link.filename, link.path)
                continue
            if isinstance(link, h5py.SoftLink):
                entry["links"][key] = ("soft", link.path)
                continue
            entry["links"][key] = ("hard",)
            child = group[key]
            if isinstance(child, h5py.Group):
                walk(child, child_path)
            else:
                record = {
                    "kind": "dataset",
                    "attrs": attrs_of(child),
                    "dtype": str(child.dtype),
                    "shape": child.shape,
                }
                if key != "Galaxies":
                    record["payload"] = numpy.array(child[()]).tobytes()
                structure[child_path] = record

    with h5py.File(path, "r") as handle:
        walk(handle, "")
    return structure


class Delta:
    """One metadata difference, with enough context to classify or report it."""

    def __init__(self, where, objpath, item, detail):
        self.where = where
        self.objpath = objpath
        self.item = item
        self.detail = detail

    def __str__(self):
        return f"{self.where}:{self.objpath} {self.item}: {self.detail}"


def diff_structures(where: str, left: dict, right: dict) -> list[Delta]:
    """Every difference between two walked files, unclassified."""
    deltas: list[Delta] = []
    for objpath in sorted(set(left) | set(right)):
        if objpath not in right:
            deltas.append(Delta(where, objpath, "object", "present only in the baseline"))
            continue
        if objpath not in left:
            deltas.append(Delta(where, objpath, "object", "present only at HEAD"))
            continue
        before, after = left[objpath], right[objpath]
        if before["kind"] != after["kind"]:
            deltas.append(Delta(where, objpath, "kind", f"{before['kind']} -> {after['kind']}"))
            continue

        for name in sorted(set(before["attrs"]) | set(after["attrs"])):
            if name not in after["attrs"]:
                deltas.append(Delta(where, objpath, f"attr {name}", "removed"))
                continue
            if name not in before["attrs"]:
                deltas.append(Delta(where, objpath, f"attr {name}", "added"))
                continue
            old_dtype, old_shape, old_bytes = before["attrs"][name]
            new_dtype, new_shape, new_bytes = after["attrs"][name]
            if (old_dtype, old_shape) != (new_dtype, new_shape):
                deltas.append(
                    Delta(
                        where,
                        objpath,
                        f"attr {name}",
                        f"dtype {old_dtype}{old_shape} -> {new_dtype}{new_shape}",
                    )
                )
                old_value = numpy.frombuffer(old_bytes, dtype=old_dtype)
                new_value = numpy.frombuffer(new_bytes, dtype=new_dtype)
                if old_value.tolist() != new_value.tolist():
                    deltas.append(
                        Delta(
                            where,
                            objpath,
                            f"attr {name}",
                            f"value {old_value.tolist()} -> {new_value.tolist()}",
                        )
                    )
            elif old_bytes != new_bytes:
                old_value = numpy.frombuffer(old_bytes, dtype=old_dtype)
                new_value = numpy.frombuffer(new_bytes, dtype=new_dtype)
                deltas.append(
                    Delta(
                        where,
                        objpath,
                        f"attr {name}",
                        f"value {old_value.tolist()} -> {new_value.tolist()}",
                    )
                )

        if before["kind"] == "group":
            if before["links"] != after["links"]:
                deltas.append(
                    Delta(where, objpath, "links", f"{before['links']} -> {after['links']}")
                )
            continue

        if before["dtype"] != after["dtype"]:
            deltas.append(
                Delta(where, objpath, "dataset dtype", f"{before['dtype']} -> {after['dtype']}")
            )
        if before["shape"] != after["shape"]:
            deltas.append(
                Delta(where, objpath, "dataset shape", f"{before['shape']} -> {after['shape']}")
            )
        if "payload" in before and before.get("payload") != after.get("payload"):
            deltas.append(Delta(where, objpath, "dataset payload", "differs"))
    return deltas


def field_metadata_delta(where, objpath, baseline: Path, head: Path) -> list[Delta]:
    """Explain a FieldMetadata payload difference row by row and column by column."""
    with h5py.File(baseline, "r") as handle:
        before = numpy.array(handle[objpath.lstrip("/")][()])
    with h5py.File(head, "r") as handle:
        after = numpy.array(handle[objpath.lstrip("/")][()])
    if before.shape != after.shape:
        return [Delta(where, objpath, "FieldMetadata", f"{before.shape} -> {after.shape} rows")]

    deltas = []
    columns = before.dtype.names
    for index in range(before.shape[0]):
        for column in columns:
            if before[index][column] == after[index][column]:
                continue
            name = before[index]["field_name"].decode()
            deltas.append(Delta(where, objpath, f"FieldMetadata {name}.{column}", "differs"))
    return deltas


def classify(delta: Delta) -> str | None:
    """Map one difference to provenance, to a permitted delta, or to None.

    Provenance attributes are anchor-independent and always excluded from the
    unclassified count. A permitted-delta case belongs here only while
    PERMITTED_DELTAS names it; each such case must bind the attribute (or
    metadata column), its exact object path, and its exact before/after
    transition -- matching on name alone would accept a version string moving
    to any value, or a counter widening to any width. PERMITTED_DELTAS is empty
    at the current anchor, so nothing below matches a permitted delta yet.
    """
    item = delta.item
    if item.startswith("attr "):
        name = item.split(" ", 1)[1]
        provenance_paths = PROVENANCE_ATTR_PATHS.get(name)
        if provenance_paths is not None:
            return "provenance" if delta.objpath in provenance_paths else None

    return None


def assert_records_byte_identical(baseline: RunOutput, head: RunOutput) -> int:
    """Every galaxy record of every snapshot of every partition, field by field.

    Compared per field rather than per whole record: the compound type carries
    padding between fields (160-byte record over 152 bytes of fields), and those
    padding bytes are not written by either run, so a whole-record comparison
    would test uninitialised memory.
    """
    baseline_files = baseline.partitions()
    head_files = head.partitions()
    if [path.name for path in baseline_files] != [path.name for path in head_files]:
        raise AssertionError(
            f"partition file sets differ: {[p.name for p in baseline_files]} vs "
            f"{[p.name for p in head_files]}"
        )

    total = 0
    for before_path, after_path in zip(baseline_files, head_files):
        with h5py.File(before_path, "r") as before, h5py.File(after_path, "r") as after:
            snaps = sorted(name for name in before if SNAP_GROUP_RE.match(name))
            after_snaps = sorted(name for name in after if SNAP_GROUP_RE.match(name))
            if snaps != after_snaps:
                raise AssertionError(
                    f"{before_path.name}: snapshot groups {snaps} vs {after_snaps}"
                )
            for name in snaps:
                left = before[f"{name}/Galaxies"]
                right = after[f"{name}/Galaxies"]
                left_signature = comparator.schema_signature(left.dtype)
                right_signature = comparator.schema_signature(right.dtype)
                if left_signature != right_signature:
                    raise AssertionError(
                        f"{before_path.name}/{name}: record schema changed:\n"
                        f"  baseline: {left_signature}\n  HEAD:     {right_signature}"
                    )
                if left.shape != right.shape:
                    raise AssertionError(
                        f"{before_path.name}/{name}: {left.shape[0]} records at the baseline, "
                        f"{right.shape[0]} at HEAD"
                    )
                left_records = left[()]
                right_records = right[()]
                for field in left_records.dtype.names:
                    left_bytes = comparator.field_bytes(left_records[field])
                    right_bytes = comparator.field_bytes(right_records[field])
                    if not numpy.array_equal(left_bytes, right_bytes):
                        differing = int((left_bytes != right_bytes).any(axis=1).sum())
                        raise AssertionError(
                            f"{before_path.name}/{name}: field {field} differs from the "
                            f"{BASELINE_COMMIT} baseline in {differing} of {left.shape[0]} records"
                        )
                total += int(left.shape[0])
                del left_records, right_records

    # A byte-identity claim over zero records is vacuously true, and every
    # structural check above passes on two runs that both produced nothing.
    # Nothing else in this stage would notice, so the count the comparison
    # actually made is asserted here rather than only logged by the caller.
    if total == 0:
        raise AssertionError(
            f"compared 0 galaxy records between {baseline.master.name} and "
            f"{head.master.name}; a byte-identity result over an empty comparison "
            f"certifies nothing"
        )
    return total


def assert_output_schema_delta(baseline: RunOutput, head: RunOutput) -> None:
    """The run-local output schema differs in exactly the permitted field changes.

    Kept in step with PERMITTED_DELTAS: a change that appears in one and not the
    other means the schema writer and the HDF5 writer have diverged. Empty at
    the current anchor; grows only alongside a new PERMITTED_DELTAS entry.

    SCHEMA_PROVENANCE_KEYS is excluded before that comparison: it names a hash
    of the generator and its YAML inputs, not of the schema, so it moves on its
    own schedule and carries no scientific content -- the field list, types and
    units are still compared exactly.
    """
    before = json.loads(baseline.schema_path().read_text())
    after = json.loads(head.schema_path().read_text())

    differences = []

    def compare(left, right, path):
        if isinstance(left, dict) and isinstance(right, dict):
            for key in sorted(set(left) | set(right)):
                if key not in left or key not in right:
                    differences.append(f"{path}.{key} (present on one side only)")
                    continue
                compare(left[key], right[key], f"{path}.{key}")
        elif isinstance(left, list) and isinstance(right, list):
            if len(left) != len(right):
                differences.append(f"{path} (list length {len(left)} vs {len(right)})")
                return
            for index, (a, b) in enumerate(zip(left, right)):
                label = a.get("name", index) if isinstance(a, dict) else index
                compare(a, b, f"{path}[{label}]")
        elif left != right:
            differences.append(path)

    compare(before, after, "")

    differences = [path for path in differences if path not in SCHEMA_PROVENANCE_KEYS]

    expected: set[str] = set()
    if set(differences) != expected:
        raise AssertionError(
            f"{head.schema_path()} differs from the {BASELINE_COMMIT} baseline in "
            f"{sorted(differences)}, expected exactly {sorted(expected)}"
        )
    log(f"  output_schema.json differs in exactly {sorted(expected)}")


# --------------------------------------------------------------------------
# The version 2 gate
# --------------------------------------------------------------------------


class VersionTwoGate(ParityGate):
    """The shared parity gate plus the multi-partition check and Stage 8."""

    def check_leg(
        self, model: str, scheme: str, vertical_run: RunOutput, horizontal_run: RunOutput
    ) -> None:
        partitions = len(vertical_run.partitions())
        if partitions < 2:
            raise AssertionError(
                f"{model}/{scheme}: the vertical run wrote {partitions} partition file(s); "
                f"{self.package.vertical} must write several, or partition aggregation is untested"
            )

    def stages(self):
        return [*super().stages(), self.stage(self.stage_tree_path_preservation)]

    def stage_tree_path_preservation(self) -> None:
        """The vertical path still produces the BASELINE_COMMIT baseline's galaxies."""
        vertical = self.package.vertical
        banner(f"Stage 8: vertical-path preservation against {BASELINE_COMMIT}")
        self.require("leg:halos-only:fixed", "no HEAD vertical run to compare against")

        head_run = self.runs[f"halos-only__{vertical}__fixed"]
        worktree = self.build_worktree("baseline", BASELINE_COMMIT, "halos-only", vertical)
        # Each side runs its own worktree's copy; they are required to be identical
        # bytes, so the two runs provably share one input without either of them
        # reaching into the working tree.
        baseline_committed = worktree / self.committed_run_file("halos-only", vertical).relative_to(
            REPO_ROOT
        )
        if baseline_committed.read_bytes() != head_run.run_file.read_bytes():
            raise AssertionError(
                f"the run file changed since {BASELINE_COMMIT}: {baseline_committed} differs "
                f"from {head_run.run_file}; the two runs would not share an input"
            )
        baseline_run = self.run_in_worktree(
            "baseline", worktree, baseline_committed, "halos-only", vertical
        )

        records = assert_records_byte_identical(baseline_run, head_run)
        log(f"  {records} galaxy records byte-identical to the {BASELINE_COMMIT} baseline")

        files = [(baseline_run.master, head_run.master)] + list(
            zip(baseline_run.partitions(), head_run.partitions())
        )
        deltas: list[Delta] = []
        for before_path, after_path in files:
            where = after_path.name
            for delta in diff_structures(
                where, h5_structure(before_path), h5_structure(after_path)
            ):
                if delta.item == "dataset payload" and delta.objpath.endswith("FieldMetadata"):
                    deltas.extend(
                        field_metadata_delta(where, delta.objpath, before_path, after_path)
                    )
                else:
                    deltas.append(delta)

        observed: dict[str, int] = {name: 0 for name in PERMITTED_DELTAS}
        provenance = 0
        unclassified = []
        for delta in deltas:
            kind = classify(delta)
            if kind == "provenance":
                provenance += 1
            elif kind in observed:
                observed[kind] += 1
            else:
                unclassified.append(delta)

        log(
            f"  {len(files)} HDF5 files walked; {provenance} excluded provenance attribute "
            f"difference(s)"
        )
        for name, count in observed.items():
            log(f"    delta observed: {name} ({count} occurrence(s))")
        if unclassified:
            detail = "\n".join(f"    {delta}" for delta in unclassified[:20])
            raise AssertionError(
                f"{len(unclassified)} HDF5 metadata difference(s) beyond the "
                f"{len(PERMITTED_DELTAS)} permitted deltas "
                f"and the {len(PROVENANCE_ATTR_PATHS)} excluded provenance attributes:\n{detail}"
            )
        missing = [name for name, count in observed.items() if count == 0]
        if missing:
            raise AssertionError(
                f"permitted delta(s) never observed: {missing}; the evidence does not show the "
                f"schema change actually reaching the output"
            )

        assert_output_schema_delta(baseline_run, head_run)
        self.done.add("preservation")


def main() -> int:
    return VersionTwoGate(PACKAGE).run(
        "micro-uchuu-ascii-horizontal version 2 cross-format identity gate "
        "(test_cross_format_identity.py)"
    )


if __name__ == "__main__":
    sys.exit(main())
