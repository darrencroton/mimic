#!/usr/bin/env python3
"""
Mimic pipeline fuzzer.

Randomly enables/disables and reorders physics modules to stress-test the
robustness of the core execution framework. Checks only for non-zero exit codes
and ERROR/FATAL lines in output; scientific correctness of output is not evaluated.

The empty pipeline (zero modules, halos-only) is always a valid configuration and
duplicate modules are allowed by design, so neither is treated as an error.

Two sampling modes are available:

  random (default): modules are independently selected, assigned to random
  phases, and shuffled. This is best for stressing Mimic's validation boundary.

  valid-subset: modules are selected from the canonical model/simulation run
  file, kept in their canonical phase and relative order, filtered to non-event
  processing modes, then closed over known hard model-specific dependency
  contracts. This produces random larger pipelines that should pass structural
  ordering validation. "Valid" here means structurally executable under the
  encoded ordering contracts, not scientifically complete or recommended.

Two failure modes (controlled by --strict):

  Default (non-strict): module-ordering validation errors are counted separately as
  VALIDATION results and do not surface as failures. These are deliberate rejections by
  Mimic's inter-module dependency contracts (e.g. "sage_X requires sage_Y to precede
  it"). Only unexpected errors — runtime data failures, IO errors, crashes — are
  treated as failures.

  Strict (--strict): every non-zero exit and every ERROR/FATAL line in output is a
  failure, including ordering validation rejections. Use this to audit the validation
  boundary itself.

Usage:
    python3 scripts/fuzz_pipeline.py                        # run until interrupted
    python3 scripts/fuzz_pipeline.py --runs 500             # stop after 500 runs
    python3 scripts/fuzz_pipeline.py --hours 24             # stop after 24 hours
    python3 scripts/fuzz_pipeline.py --seed 1234567890      # replay one specific seed
    python3 scripts/fuzz_pipeline.py --sampling valid-subset # random valid canonical subsets
    python3 scripts/fuzz_pipeline.py --strict               # treat all errors as failures
    python3 scripts/fuzz_pipeline.py --model sage16 --simulation mini-millennium
    python3 scripts/fuzz_pipeline.py --help

Failures are saved to archive/fuzz-logs/failures/<seed>/:
    config.yaml    the exact YAML config that produced the failure
    stdout.txt     captured stdout
    stderr.txt     captured stderr
    result.txt     one-line failure reason

The rolling summary is written to archive/fuzz-logs/summary.log, one line per run.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timedelta
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Set, Tuple

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML not installed. pip install pyyaml", file=sys.stderr)
    sys.exit(1)

# ---------------------------------------------------------------------------
# Repository layout
# ---------------------------------------------------------------------------

REPO_ROOT = Path(__file__).resolve().parent.parent
MIMIC_BIN = REPO_ROOT / "mimic"

# Use a single snapshot and one substep to keep each fuzz run fast.
FUZZ_SNAPSHOT_LIST = [63]
FUZZ_SUBSTEPS = 1

# Only fuzz non-event modes. process_per_event requires a matching producer in
# the same phase; excluding it keeps config generation simple and self-consistent.
NON_EVENT_MODES = frozenset({"process_full_halo", "process_by_galaxy"})

# Supported config generation modes.
SAMPLING_RANDOM = "random"
SAMPLING_VALID_SUBSET = "valid-subset"

# Pool of names for randomly generated substep phases.
_PHASE_NAME_POOL = [
    "physics",
    "feedback",
    "cooling",
    "mergers",
    "enrichment",
    "star_formation",
    "agn",
    "stripping",
    "reionization",
    "dynamics",
    "baryons",
    "evolution",
    "galaxy_physics",
    "satellite_physics",
]

# Regex pattern matched against stderr lines to catch error output.
# Mimic writes "ERROR: ..." and "FATAL: ..." to stderr (no ANSI codes when
# captured via pipe). These prefixes are always present for ERROR/FATAL levels.
_ERROR_PREFIXES = ("ERROR:", "FATAL:")

# Pattern that identifies deliberate inter-module ordering validation messages.
# These are intentional contract checks — "module A requires module B to precede it"
# — not runtime or infrastructure failures. Filtered in non-strict mode.
_VALIDATION_PATTERN = re.compile(
    r"ERROR: sage_\w.*(?:requires|must run after)\s+sage_",
    re.IGNORECASE,
)


# ---------------------------------------------------------------------------
# Module discovery
# ---------------------------------------------------------------------------


def discover_modules(model: str) -> List[Dict[str, Any]]:
    """Return a list of fuzzable modules for the given model.

    Each entry: {"name": str, "modes": List[str]} where modes contains only
    non-event processing modes the module supports.
    """
    modules_dir = REPO_ROOT / "models" / model / "modules"
    if not modules_dir.exists():
        sys.exit(f"ERROR: modules directory not found: {modules_dir}")

    result: List[Dict[str, Any]] = []
    skip_parts = {"_tests", "_archive", "archive", "template"}

    for yaml_path in sorted(modules_dir.glob("*/module_info.yaml")):
        parts = set(yaml_path.relative_to(REPO_ROOT).parts)
        if parts & skip_parts:
            continue
        with yaml_path.open() as fh:
            data = yaml.safe_load(fh)
        mod = data.get("module", {})
        name = mod.get("name")
        all_modes = mod.get("supported_processing_modes", [])
        if not name or not all_modes:
            continue
        eligible_modes = [m for m in all_modes if m in NON_EVENT_MODES]
        if not eligible_modes:
            continue
        result.append({"name": name, "modes": eligible_modes})

    if not result:
        sys.exit(f"ERROR: no fuzzable modules found for model '{model}'")
    return result


# ---------------------------------------------------------------------------
# Base config loading
# ---------------------------------------------------------------------------


def load_base_config(model: str, simulation: str) -> Dict[str, Any]:
    """Load the canonical run YAML for the given model/simulation pair."""
    yaml_path = REPO_ROOT / "models" / model / "input" / f"{model}_{simulation}.yaml"
    if not yaml_path.exists():
        sys.exit(f"ERROR: base config not found: {yaml_path}")
    with yaml_path.open() as fh:
        return yaml.safe_load(fh)


# ---------------------------------------------------------------------------
# Config generation
# ---------------------------------------------------------------------------


def _slot_entries(bucket: List[Tuple[str, str]]) -> Optional[List[Dict[str, str]]]:
    """Convert (name, mode) pairs to the YAML list format, or None if empty."""
    if not bucket:
        return None
    return [{name: mode} for name, mode in bucket]


def generate_config(
    seed: int,
    base_config: Dict[str, Any],
    all_modules: List[Dict[str, Any]],
    output_dir: Path,
) -> Dict[str, Any]:
    """Generate a deterministic fuzz config from seed.

    Inclusion density is itself random so the space covers:
    - empty pipeline (no modules at all)
    - sparse pipelines (a few modules)
    - dense pipelines (most or all modules, possibly with duplicates)

    Modules are randomly assigned to pre_timestep, one of 1-3 named phases,
    or post_timestep, in a random order within each slot.
    """
    rng = random.Random(seed)

    # Occasionally include duplicates (one extra copy of a randomly chosen module)
    # to exercise the "duplicate modules are valid" path.
    module_pool = list(all_modules)
    if module_pool and rng.random() < 0.15:
        extra = rng.choice(module_pool)
        module_pool.append(extra)

    # Inclusion density: bias toward a full scan of the space.
    #   10% of runs → empty pipeline
    #   15% of runs → all modules
    #   75% of runs → random density between 0.1 and 0.9
    density_roll = rng.random()
    if density_roll < 0.10:
        density = 0.0
    elif density_roll < 0.25:
        density = 1.0
    else:
        density = rng.uniform(0.1, 0.9)

    selected = [m for m in module_pool if rng.random() < density]

    # Named phase names (1-3 unique names from the pool)
    num_phases = rng.randint(1, 3)
    phase_names: List[str] = rng.sample(_PHASE_NAME_POOL, min(num_phases, len(_PHASE_NAME_POOL)))

    # All possible assignment slots
    slots = ["pre_timestep"] + phase_names + ["post_timestep"]

    # Assign each selected module to a random slot with a random eligible mode
    slot_buckets: Dict[str, List[Tuple[str, str]]] = {s: [] for s in slots}
    for mod in selected:
        slot = rng.choice(slots)
        mode = rng.choice(mod["modes"])
        slot_buckets[slot].append((mod["name"], mode))

    # Shuffle within each slot for ordering randomness
    for bucket in slot_buckets.values():
        rng.shuffle(bucket)

    # Build the phases sub-block (only include phases that have entries)
    phases_block: Optional[Dict[str, Any]] = {}
    for phase_name in phase_names:
        entries = _slot_entries(slot_buckets[phase_name])
        if entries is not None:
            phases_block[phase_name] = entries
    if not phases_block:
        phases_block = None

    # Omit lifecycle phase keys entirely when empty. The C YAML parser treats
    # absent keys as empty (valid), but rejects explicit 'null' scalars for phases
    # because yaml.dump renders None as the literal string "null" which the parser
    # sees as a non-empty scalar and rejects with "must be a sequence".
    modules_block: Dict[str, Any] = {}
    pre_entries = _slot_entries(slot_buckets["pre_timestep"])
    if pre_entries is not None:
        modules_block["pre_timestep"] = pre_entries
    if phases_block is not None:
        modules_block["phases"] = phases_block
    post_entries = _slot_entries(slot_buckets["post_timestep"])
    if post_entries is not None:
        modules_block["post_timestep"] = post_entries
    modules_block["parameters"] = (base_config.get("modules") or {}).get("parameters")

    return {
        "model": base_config["model"],
        "simulation": base_config["simulation"],
        "output": {
            "output_filename": "fuzz",
            "output_directory": str(output_dir),
            "output_format": "binary",
            "snapshot_list": FUZZ_SNAPSHOT_LIST,
        },
        "SubSteps": FUZZ_SUBSTEPS,
        "modules": modules_block,
    }


# ---------------------------------------------------------------------------
# Valid-subset config generation
# ---------------------------------------------------------------------------


CanonicalEntry = Tuple[str, str, str]


def _module_entry_items(entries: Iterable[Any]) -> Iterable[Tuple[str, str]]:
    """Yield (module_name, mode) from YAML module-entry dictionaries."""
    for entry in entries or []:
        if not isinstance(entry, dict) or len(entry) != 1:
            continue
        name, mode = next(iter(entry.items()))
        if isinstance(name, str) and isinstance(mode, str):
            yield name, mode


def _canonical_non_event_entries(
    base_config: Dict[str, Any], all_modules: List[Dict[str, Any]]
) -> List[CanonicalEntry]:
    """Flatten the canonical input pipeline into eligible non-event entries.

    A canonical entry is one concrete occurrence of a module in the base run
    file: (phase_name, module_name, processing_mode). An entry is eligible when
    the module is fuzz-discoverable and the occurrence uses one of the fuzzer's
    non-event modes. Event consumers are deliberately excluded because they
    require same-phase producer wiring and event-contract handling that the
    current fuzzer keeps out of scope.

    The valid-subset sampler intentionally draws from the model package's
    canonical run file and preserves its phase/order. This keeps scientific
    ordering explicit in user configuration, matching the architecture vision,
    while still producing randomized subsets for stress testing.
    """
    eligible_modes = {m["name"]: set(m["modes"]) for m in all_modules}
    modules = base_config.get("modules") or {}
    result: List[CanonicalEntry] = []

    for name, mode in _module_entry_items(modules.get("pre_timestep") or []):
        if mode in eligible_modes.get(name, set()):
            result.append(("pre_timestep", name, mode))

    for phase_name, entries in (modules.get("phases") or {}).items():
        for name, mode in _module_entry_items(entries or []):
            if mode in eligible_modes.get(name, set()):
                result.append((phase_name, name, mode))

    for name, mode in _module_entry_items(modules.get("post_timestep") or []):
        if mode in eligible_modes.get(name, set()):
            result.append(("post_timestep", name, mode))

    if not result:
        sys.exit(
            "ERROR: --sampling valid-subset requires the base config to contain "
            "at least one non-event fuzzable module"
        )

    return result


def _add_if_canonical(
    selected: Set[CanonicalEntry],
    canonical_set: Set[CanonicalEntry],
    phase: str,
    name: str,
    mode: str,
) -> bool:
    """Add a required module only when the canonical pipeline contains it."""
    entry = (phase, name, mode)
    if entry in canonical_set and entry not in selected:
        selected.add(entry)
        return True
    return False


def _close_sage16_dependencies(
    selected: Set[CanonicalEntry], canonical_entries: List[CanonicalEntry]
) -> None:
    """Mutate selected entries to satisfy known hard SAGE16 ordering contracts.

    These are the module-init contracts that otherwise dominate fully random
    dense fuzz runs. The order itself is not solved here; it remains the
    canonical order from the run file.
    """
    canonical_set = set(canonical_entries)

    while True:
        changed = False
        selected_names = {name for _, name, _ in selected}

        if "sage_apply_infall" in selected_names:
            changed |= _add_if_canonical(
                selected,
                canonical_set,
                "pre_timestep",
                "sage_prepare_infall_budget",
                "process_full_halo",
            )

        if "sage_apply_cooling" in selected_names:
            changed |= _add_if_canonical(
                selected,
                canonical_set,
                "galaxy_physics",
                "sage_calculate_cooling_budget",
                "process_by_galaxy",
            )

        if (
            "sage_calculate_star_formation" in selected_names
            or "sage_calculate_supernova_feedback" in selected_names
        ):
            changed |= _add_if_canonical(
                selected,
                canonical_set,
                "galaxy_physics",
                "sage_apply_star_formation_supernova",
                "process_by_galaxy",
            )

        if "sage_resolve_mergers_and_disruption" in selected_names:
            changed |= _add_if_canonical(
                selected,
                canonical_set,
                "pre_timestep",
                "sage_initialise_merger_clock",
                "process_full_halo",
            )

        if not changed:
            return


def _close_known_dependencies(
    model: str, selected: Set[CanonicalEntry], canonical_entries: List[CanonicalEntry]
) -> None:
    """Apply model-specific dependency closure for valid-subset sampling.

    Dependency closure adds required canonical entries when a selected module
    has a hard startup contract on another module being present. It does not
    invent phases or reorder modules; if a required entry is absent from the
    canonical pipeline, it is not added. That keeps this mode a subset sampler,
    not a generic dependency solver.
    """
    if model == "sage16":
        _close_sage16_dependencies(selected, canonical_entries)


def _entries_by_phase(
    selected: Set[CanonicalEntry], canonical_entries: List[CanonicalEntry]
) -> Dict[str, List[Tuple[str, str]]]:
    """Return selected entries grouped by phase in canonical order."""
    grouped: Dict[str, List[Tuple[str, str]]] = {}
    for phase, name, mode in canonical_entries:
        if (phase, name, mode) in selected:
            grouped.setdefault(phase, []).append((name, mode))
    return grouped


def _build_config_from_phase_entries(
    base_config: Dict[str, Any],
    output_dir: Path,
    grouped: Dict[str, List[Tuple[str, str]]],
) -> Dict[str, Any]:
    """Build a fuzz run config from grouped phase entries."""
    modules_block: Dict[str, Any] = {}

    pre_entries = _slot_entries(grouped.get("pre_timestep", []))
    if pre_entries is not None:
        modules_block["pre_timestep"] = pre_entries

    phases_block: Dict[str, Any] = {}
    for phase_name, entries in grouped.items():
        if phase_name in ("pre_timestep", "post_timestep"):
            continue
        phase_entries = _slot_entries(entries)
        if phase_entries is not None:
            phases_block[phase_name] = phase_entries
    if phases_block:
        modules_block["phases"] = phases_block

    post_entries = _slot_entries(grouped.get("post_timestep", []))
    if post_entries is not None:
        modules_block["post_timestep"] = post_entries

    modules_block["parameters"] = (base_config.get("modules") or {}).get("parameters")

    return {
        "model": base_config["model"],
        "simulation": base_config["simulation"],
        "output": {
            "output_filename": "fuzz",
            "output_directory": str(output_dir),
            "output_format": "binary",
            "snapshot_list": FUZZ_SNAPSHOT_LIST,
        },
        "SubSteps": FUZZ_SUBSTEPS,
        "modules": modules_block,
    }


def generate_valid_subset_config(
    seed: int,
    model: str,
    base_config: Dict[str, Any],
    all_modules: List[Dict[str, Any]],
    output_dir: Path,
) -> Dict[str, Any]:
    """Generate a random dependency-closed subset of the canonical pipeline.

    "Valid subset" means:
    - every module entry comes from the canonical base config;
    - selected entries keep their canonical lifecycle phase and relative order;
    - only non-event processing modes are included;
    - known hard dependency contracts for the model are closed by adding the
      canonical prerequisite entries.

    The result is intended to pass Mimic's structural ordering checks while
    still varying which chunks of the canonical physics pipeline execute. It may
    still produce scientifically incomplete pipelines or module warnings.
    """
    rng = random.Random(seed)
    canonical_entries = _canonical_non_event_entries(base_config, all_modules)

    # Bias toward larger valid pipelines while still sampling empty/sparse cases.
    density_roll = rng.random()
    if density_roll < 0.05:
        density = 0.0
    elif density_roll < 0.20:
        density = 1.0
    else:
        density = rng.uniform(0.45, 0.95)

    selected = {entry for entry in canonical_entries if rng.random() < density}
    _close_known_dependencies(model, selected, canonical_entries)
    grouped = _entries_by_phase(selected, canonical_entries)
    return _build_config_from_phase_entries(base_config, output_dir, grouped)


def generate_fuzz_config(
    seed: int,
    model: str,
    base_config: Dict[str, Any],
    all_modules: List[Dict[str, Any]],
    output_dir: Path,
    sampling: str,
) -> Dict[str, Any]:
    """Dispatch to the selected fuzz config generator."""
    if sampling == SAMPLING_VALID_SUBSET:
        return generate_valid_subset_config(seed, model, base_config, all_modules, output_dir)
    return generate_config(seed, base_config, all_modules, output_dir)


def _count_modules(config: Dict[str, Any]) -> int:
    """Count total module entries in a fuzz config."""
    modules = config.get("modules") or {}
    count = 0
    for key in ("pre_timestep", "post_timestep"):
        count += len(modules.get(key) or [])
    for entries in (modules.get("phases") or {}).values():
        count += len(entries or [])
    return count


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------


def run_mimic(config: Dict[str, Any], timeout: int) -> Tuple[int, str, str]:
    """Write config to a temp file, run mimic, return (exit_code, stdout, stderr).

    Returns exit_code=-1 on timeout.
    """
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".yaml", prefix="fuzz_config_", delete=False
    ) as fh:
        yaml.dump(config, fh, default_flow_style=False, sort_keys=False)
        config_path = fh.name

    try:
        proc = subprocess.run(
            [str(MIMIC_BIN), config_path],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        return proc.returncode, proc.stdout, proc.stderr
    except subprocess.TimeoutExpired:
        return -1, "", f"TIMEOUT after {timeout}s"
    finally:
        try:
            os.unlink(config_path)
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------


def classify(exit_code: int, stdout: str, stderr: str, strict: bool = False) -> Tuple[str, str]:
    """Return (status, reason).

    status is one of:
        'PASS'       — clean run, no errors
        'VALIDATION' — Mimic rejected the config via an inter-module ordering contract;
                       expected behaviour, not a framework bug (non-strict mode only)
        'FAIL'       — unexpected error: crash, runtime data failure, IO error, etc.

    In strict mode VALIDATION is promoted to FAIL so every rejection is surfaced.
    """
    if exit_code == -1:
        return "FAIL", "TIMEOUT"

    # Scan both streams; Mimic normally routes ERROR/FATAL to stderr but check
    # both to be safe.
    error_lines = [
        ln
        for ln in (stdout + stderr).splitlines()
        if any(ln.lstrip().startswith(p) for p in _ERROR_PREFIXES)
    ]

    if exit_code != 0:
        if error_lines:
            first = error_lines[0]
            reason = f"exit={exit_code} — {first[:200]}"
            if not strict and _VALIDATION_PATTERN.search(first):
                return "VALIDATION", reason
        elif stderr.strip():
            reason = f"exit={exit_code} — {stderr.strip().splitlines()[-1][:200]}"
        else:
            reason = f"exit={exit_code} (no error message captured)"
        return "FAIL", reason

    if error_lines:
        # Exit 0 but ERROR/FATAL in output — always a failure regardless of mode.
        return "FAIL", f"exit=0 but ERROR in output — {error_lines[0][:200]}"

    return "PASS", ""


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------


def save_failure(
    log_dir: Path,
    seed: int,
    config: Dict[str, Any],
    stdout: str,
    stderr: str,
    reason: str,
) -> Path:
    """Persist a failing run under archive/fuzz-logs/failures/<seed>/."""
    failure_dir = log_dir / "failures" / str(seed)
    failure_dir.mkdir(parents=True, exist_ok=True)
    (failure_dir / "config.yaml").write_text(
        yaml.dump(config, default_flow_style=False, sort_keys=False)
    )
    (failure_dir / "stdout.txt").write_text(stdout)
    (failure_dir / "stderr.txt").write_text(stderr)
    (failure_dir / "result.txt").write_text(f"FAIL: {reason}\n")
    return failure_dir


def _append_summary(summary_log: Path, line: str) -> None:
    with summary_log.open("a") as fh:
        fh.write(line + "\n")


# ---------------------------------------------------------------------------
# Single-run helper
# ---------------------------------------------------------------------------


def run_one(
    seed: int,
    model: str,
    base_config: Dict[str, Any],
    all_modules: List[Dict[str, Any]],
    log_dir: Path,
    timeout: int,
    summary_log: Optional[Path] = None,
    strict: bool = False,
    sampling: str = SAMPLING_RANDOM,
) -> str:
    """Run one fuzz iteration. Returns 'PASS', 'VALIDATION', or 'FAIL'."""
    with tempfile.TemporaryDirectory(prefix="mimic_fuzz_out_") as output_dir:
        config = generate_fuzz_config(
            seed, model, base_config, all_modules, Path(output_dir), sampling
        )
        t0 = time.monotonic()
        exit_code, stdout, stderr = run_mimic(config, timeout)
        elapsed = time.monotonic() - t0

    status, reason = classify(exit_code, stdout, stderr, strict=strict)
    module_count = _count_modules(config)
    timestamp = datetime.now().isoformat(timespec="seconds")

    summary_line = (
        f"{timestamp}  seed={seed:>12}  modules={module_count:>2}"
        f"  duration={elapsed:>6.2f}s  {status}" + (f"  -- {reason}" if reason else "")
    )

    if summary_log is not None:
        _append_summary(summary_log, summary_line)

    if status == "FAIL":
        failure_dir = save_failure(log_dir, seed, config, stdout, stderr, reason)
        print(f"FAIL  seed={seed}  {reason}")
        print(f"      saved → {failure_dir}")

    return status


# ---------------------------------------------------------------------------
# Replay helper (single seed, full output to console)
# ---------------------------------------------------------------------------


def replay_seed(
    seed: int,
    model: str,
    base_config: Dict[str, Any],
    all_modules: List[Dict[str, Any]],
    log_dir: Path,
    timeout: int,
    strict: bool = False,
    sampling: str = SAMPLING_RANDOM,
) -> str:
    """Replay a single seed and print the full config and output. Returns status string."""
    with tempfile.TemporaryDirectory(prefix="mimic_fuzz_out_") as output_dir:
        config = generate_fuzz_config(
            seed, model, base_config, all_modules, Path(output_dir), sampling
        )

        print(f"=== Config for seed={seed} ===")
        print(yaml.dump(config, default_flow_style=False, sort_keys=False))

        t0 = time.monotonic()
        exit_code, stdout, stderr = run_mimic(config, timeout)
        elapsed = time.monotonic() - t0

    status, reason = classify(exit_code, stdout, stderr, strict=strict)

    print("=== stdout ===")
    print(stdout or "(empty)")
    print("=== stderr ===")
    print(stderr or "(empty)")
    print("=== result ===")
    print(
        f"exit_code={exit_code}  duration={elapsed:.2f}s  {status}"
        + (f"  -- {reason}" if reason else "")
    )

    if status == "FAIL":
        failure_dir = save_failure(log_dir, seed, config, stdout, stderr, reason)
        print(f"saved → {failure_dir}")

    return status


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--runs",
        type=int,
        default=None,
        help="Stop after N completed runs (default: run until interrupted)",
    )
    parser.add_argument(
        "--hours",
        type=float,
        default=None,
        help="Stop after H hours (default: run until interrupted)",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=None,
        help="Replay one specific seed and exit (for debugging failures)",
    )
    parser.add_argument("--model", default="sage16", help="Model package name (default: sage16)")
    parser.add_argument(
        "--simulation",
        default="mini-millennium",
        help="Simulation package name (default: mini-millennium)",
    )
    parser.add_argument(
        "--timeout", type=int, default=120, help="Per-run timeout in seconds (default: 120)"
    )
    parser.add_argument(
        "--log-dir", default=None, help="Log directory (default: archive/fuzz-logs)"
    )
    parser.add_argument(
        "--progress-every",
        type=int,
        default=10,
        help="Print progress summary every N runs (default: 10)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Treat ordering validation rejections as failures (default: count them separately)",
    )
    parser.add_argument(
        "--sampling",
        choices=(SAMPLING_RANDOM, SAMPLING_VALID_SUBSET),
        default=SAMPLING_RANDOM,
        help=(
            "Config sampling strategy: 'random' scatters modules freely; "
            "'valid-subset' samples dependency-closed subsets of the canonical "
            "input pipeline (default: random)"
        ),
    )
    args = parser.parse_args()

    if not MIMIC_BIN.exists():
        sys.exit(f"ERROR: mimic binary not found at {MIMIC_BIN}. Run 'make' first.")

    all_modules = discover_modules(args.model)
    base_config = load_base_config(args.model, args.simulation)

    print(f"Mimic pipeline fuzzer")
    print(f"  model        : {args.model}")
    print(f"  simulation   : {args.simulation}")
    module_names = ", ".join(m["name"] for m in all_modules)
    print(f"  modules      : {len(all_modules)} fuzzable ({module_names})")
    print(f"  snapshot     : {FUZZ_SNAPSHOT_LIST}  substeps: {FUZZ_SUBSTEPS}")
    print(f"  timeout      : {args.timeout}s per run")
    print(f"  sampling     : {args.sampling}")

    log_dir = Path(args.log_dir) if args.log_dir else REPO_ROOT / "archive" / "fuzz-logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    summary_log = log_dir / "summary.log"

    print(f"  log dir      : {log_dir}")
    print(f"  summary log  : {summary_log}")

    if args.strict:
        print(f"  mode         : strict (ordering validation rejections count as failures)")
    else:
        print(f"  mode         : default (ordering validation rejections counted separately)")

    # Single-seed replay mode
    if args.seed is not None:
        print()
        status = replay_seed(
            args.seed,
            args.model,
            base_config,
            all_modules,
            log_dir,
            args.timeout,
            strict=args.strict,
            sampling=args.sampling,
        )
        sys.exit(1 if status == "FAIL" else 0)

    # Determine stop conditions
    deadline: Optional[datetime] = None
    if args.hours is not None:
        deadline = datetime.now() + timedelta(hours=args.hours)
        print(f"  stop after   : {args.hours}h  (at {deadline.strftime('%Y-%m-%d %H:%M:%S')})")
    if args.runs is not None:
        print(f"  stop after   : {args.runs} runs")
    if deadline is None and args.runs is None:
        print(f"  stop after   : Ctrl-C")

    print()

    runs_done = 0
    passes = 0
    validations = 0
    fails = 0

    try:
        while True:
            if args.runs is not None and runs_done >= args.runs:
                break
            if deadline is not None and datetime.now() >= deadline:
                break

            seed = random.randint(0, 2**32 - 1)
            status = run_one(
                seed,
                args.model,
                base_config,
                all_modules,
                log_dir,
                args.timeout,
                summary_log,
                strict=args.strict,
                sampling=args.sampling,
            )

            runs_done += 1
            if status == "PASS":
                passes += 1
            elif status == "VALIDATION":
                validations += 1
            else:
                fails += 1

            if runs_done % args.progress_every == 0:
                parts = [f"{passes} passed"]
                if validations:
                    parts.append(f"{validations} validation")
                if fails:
                    parts.append(f"{fails} FAILED")
                print(f"  [{runs_done:>6} runs]  {', '.join(parts)}")

    except KeyboardInterrupt:
        print()

    val_tag = f", {validations} validation (ordering contracts)" if validations else ""
    print(f"\nDone. {runs_done} runs: {passes} passed{val_tag}, {fails} failed.")
    if fails:
        print(f"Failures saved to: {log_dir / 'failures'}")
        print(f"Replay a failure:  python3 scripts/fuzz_pipeline.py --seed <seed>")
    sys.exit(1 if fails > 0 else 0)


if __name__ == "__main__":
    main()
