#!/usr/bin/env python3
"""
Tree Preservation Integration Test

Validates: Simulation core properties are correctly preserved from input tree to output
Phase: Input-Output Validation
Scope: Physics-free mode (no modules), snapshot 63 only, Type 0+1 halos

This test validates the physics-agnostic data pipeline by comparing:
  - Input: Binary tree files (struct RawHalo) at snapshot 63
  - Output: Mimic output files (struct HaloOutput) at snapshot 63, Type 0+1 only

Test cases:
  - test_tree_preservation_coverage: Validates halo counts and matching coverage
  - test_tree_preservation_properties: Validates all directly-copied properties

Properties validated (directly copied from input):
  - Pos[3], Vel[3], Spin[3]: Vector properties (component-wise)
  - Len, SnapNum: Integer properties
  - Vmax, VelDisp: Float properties
  - MostBoundID: Original simulation halo ID (exact match)
  - Mvir: Conditionally validated for Type=0 centrals (when input Mvir >= 0.0)

Properties NOT validated:
  - Mvir: For satellites and centrals with Mvir < 0.0 (calculated from particles)
  - All other properties: Not directly copied from input

Matching strategy:
  - Filter input to SnapNum=63 only
  - Filter output to SnapNum=63 AND Type in [0,1] (skip orphans)
  - Match by MostBoundID (same field in input and output)
  - Categorize: perfect matches, imperfect matches, unmatched input, unmatched output

Tolerance: 1e-6 relative for floats, exact for integers

Author: Mimic Testing Team
Date: 2025-11-30
"""

import sys
from pathlib import Path
import numpy as np
from io import StringIO
from collections import defaultdict
from datetime import datetime

# Add framework to path
REPO_ROOT = Path(__file__).parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    load_binary_halos,
    TEST_DATA_DIR,
    MIMIC_EXE,
    ensure_output_dirs,
    model_input_file,
    run_mimic_fresh,
)

# Import tree loader
from framework.tree_loader import load_binary_tree, get_halos_by_snapshot

# Ensure output directories exist
ensure_output_dirs()

# ANSI color codes (module-level constants)
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def match_halos_snapshot63(input_halos, output_halos, snapshot=63):
    """
    Match snapshot 63 input halos to Type 0+1 output halos.

    Matching strategy:
    1. Filter input to SnapNum=63 only
    2. Filter output to SnapNum=63 AND Type in [0,1] (skip orphans)
    3. Match by MostBoundID (same field in input and output)
    4. Categorize ALL matches (no filtering to 1:1)

    Args:
        input_halos: Tree input halos (recarray from load_binary_tree)
        output_halos: Mimic output halos (recarray from load_binary_halos)
        snapshot: Snapshot to validate (default 63)

    Returns:
        dict: {
            'input_snap63': np.array of input indices at snapshot 63,
            'output_type01': np.array of output indices (Type 0+1 at snapshot),
            'matched_pairs': List of (input_idx, output_idx),
            'unmatched_input': List of input indices not matched,
            'unmatched_output': List of output indices not matched,
            'input_count': Total input halos at snapshot,
            'output_count': Total Type 0+1 output halos at snapshot,
        }
    """
    # Filter input to snapshot 63
    input_snap_mask = input_halos.SnapNum == snapshot
    input_snap63_indices = np.where(input_snap_mask)[0]

    # Filter output to snapshot 63 AND Type 0+1 (skip orphans)
    output_mask = (output_halos.SnapNum == snapshot) & (output_halos.Type != 2)
    output_type01_indices = np.where(output_mask)[0]

    # Build lookup: MostBoundID → input index (for snap 63 halos)
    input_lookup = {}
    for in_idx in input_snap63_indices:
        mbid = input_halos[in_idx].MostBoundID
        # Store ALL input halos with same MostBoundID (should be unique, but check)
        if mbid in input_lookup:
            input_lookup[mbid].append(in_idx)
        else:
            input_lookup[mbid] = [in_idx]

    # Match output Type 0+1 halos to input
    matched_pairs = []
    matched_input_set = set()
    matched_output_set = set()

    for out_idx in output_type01_indices:
        sim_halo_idx = output_halos[out_idx].MostBoundID

        # Find corresponding input halo(s)
        if sim_halo_idx in input_lookup:
            # Should be exactly one, but handle multiple if they exist
            input_matches = input_lookup[sim_halo_idx]
            for in_idx in input_matches:
                matched_pairs.append((in_idx, out_idx))
                matched_input_set.add(in_idx)
                matched_output_set.add(out_idx)

    # Find unmatched
    unmatched_input = [idx for idx in input_snap63_indices if idx not in matched_input_set]
    unmatched_output = [idx for idx in output_type01_indices if idx not in matched_output_set]

    return {
        'input_snap63': input_snap63_indices,
        'output_type01': output_type01_indices,
        'matched_pairs': matched_pairs,
        'unmatched_input': unmatched_input,
        'unmatched_output': unmatched_output,
        'input_count': len(input_snap63_indices),
        'output_count': len(output_type01_indices),
    }


def validate_copied_properties(input_halo, output_halo, halonr, input_halos, rtol=1e-6):
    """
    Validate properties directly copied from input tree to output.

    Based on halo_properties.yaml, these properties have init_source or output_source
    with copy_from_tree or copy_from_tree_array:
        - SnapNum: int (exact)
        - Pos[3]: float (rtol)
        - Vel[3]: float (rtol)
        - Spin[3]: float (rtol)
        - Len: int (exact)
        - Vmax: float (rtol)
        - VelDisp: float (rtol)
        - MostBoundID: long long (exact)

    Conditional validation:
        - Mvir: For Type=0 centrals where halonr==FirstHaloInFOFgroup AND Mvir>=0.0,
                Mvir is copied from input (see get_virial_mass in virial.c)

    Properties NOT validated:
        - Mvir: For satellites and centrals with Mvir < 0.0 (calculated from particles)
        - All other properties: Not directly copied from input

    Args:
        input_halo: Input tree halo (recarray element)
        output_halo: Output halo (recarray element)
        halonr: HaloNr (index in InputTreeHalos)
        input_halos: Full input halos array (needed for FOF check)
        rtol: Relative tolerance for float comparison (default 1e-6)

    Returns:
        dict: {
            'passed': bool,
            'mismatches': List of (property, input_val, output_val, rel_diff)
        }
    """
    mismatches = []

    # Vector properties - check component-wise
    for prop in ['Pos', 'Vel', 'Spin']:
        for i, comp in enumerate(['x', 'y', 'z']):
            in_val = input_halo[prop][i]
            out_val = output_halo[prop][i]
            if not np.isclose(in_val, out_val, rtol=rtol, atol=0):
                rel_diff = abs(in_val - out_val) / (abs(in_val) + 1e-30)
                mismatches.append((f"{prop}[{comp}]", in_val, out_val, rel_diff))

    # Integer properties - exact match required
    for prop in ['Len', 'SnapNum']:
        if input_halo[prop] != output_halo[prop]:
            mismatches.append((prop, input_halo[prop], output_halo[prop], None))

    # Float properties - relative tolerance
    for prop in ['Vmax', 'VelDisp']:
        in_val = input_halo[prop]
        out_val = output_halo[prop]
        if not np.isclose(in_val, out_val, rtol=rtol, atol=0):
            rel_diff = abs(in_val - out_val) / (abs(in_val) + 1e-30)
            mismatches.append((prop, in_val, out_val, rel_diff))

    # MostBoundID
    if input_halo['MostBoundID'] != output_halo['MostBoundID']:
        mismatches.append((
            'MostBoundID',
            input_halo['MostBoundID'],
            output_halo['MostBoundID'],
            None
        ))

    # Mvir: Conditional validation for Type=0 centrals
    # From get_virial_mass() in src/core/virial.c:
    # If halonr == FirstHaloInFOFgroup AND Mvir >= 0.0, then Mvir is copied
    if output_halo.Type == 0:  # Central halo
        is_fof_central = (halonr == input_halos[halonr].FirstHaloInFOFgroup)
        has_valid_mvir = (input_halo['Mvir'] >= 0.0)

        if is_fof_central and has_valid_mvir:
            in_val = input_halo['Mvir']
            out_val = output_halo['Mvir']
            if not np.isclose(in_val, out_val, rtol=rtol, atol=0):
                rel_diff = abs(in_val - out_val) / (abs(in_val) + 1e-30)
                mismatches.append(('Mvir', in_val, out_val, rel_diff))

    return {
        'passed': len(mismatches) == 0,
        'mismatches': mismatches
    }


def format_validation_failures(failures, halo_type):
    """
    Format validation failures into readable report.

    Groups failures by property and shows example mismatches.

    Args:
        failures: List of (input_idx, output_idx, mismatches)
            where mismatches is list of (property, input_val, output_val, rel_diff)
        halo_type: String like "CENTRALS" or "SATELLITES" for report header

    Returns:
        str: Formatted report string
    """
    report = StringIO()

    report.write(f"\n{'='*60}\n")
    report.write(f"{halo_type} VALIDATION FAILURES\n")
    report.write(f"{'='*60}\n")
    report.write(f"Total failures: {len(failures)}\n\n")

    # Group by property
    property_failures = defaultdict(list)
    for in_idx, out_idx, mismatches in failures:
        for prop, expected, actual, diff in mismatches:
            property_failures[prop].append((in_idx, out_idx, expected, actual, diff))

    # Report by property
    for prop, prop_failures in sorted(property_failures.items()):
        report.write(f"\nProperty: {prop}\n")
        report.write(f"  Failures: {len(prop_failures)}\n")

        # Show first 5 examples
        for i, (in_idx, out_idx, exp, act, diff) in enumerate(prop_failures[:5]):
            if diff is not None:
                report.write(f"    Input[{in_idx}] → Output[{out_idx}]: "
                           f"expected={exp:.6e}, actual={act:.6e}, "
                           f"rel_diff={diff:.2e}\n")
            else:
                report.write(f"    Input[{in_idx}] → Output[{out_idx}]: "
                           f"expected={exp}, actual={act}\n")

        if len(prop_failures) > 5:
            report.write(f"    ... and {len(prop_failures) - 5} more\n")

    return report.getvalue()


def generate_markdown_report(match_result, input_halos, output_halos,
                            perfect_matches, imperfect_matches,
                            tree_file, output_file, snapshot=63):
    """
    Generate comprehensive markdown report of tree preservation validation.

    Args:
        match_result: Dictionary from match_halos_snapshot63()
        input_halos: Full input halos array
        output_halos: Full output halos array
        perfect_matches: List of (input_idx, output_idx) tuples
        imperfect_matches: List of (input_idx, output_idx, mismatches) tuples
        tree_file: Path to input tree file
        output_file: Path to output file
        snapshot: Snapshot number being validated

    Returns:
        str: Markdown formatted report
    """
    md = StringIO()

    # Header
    md.write("# Tree Preservation Validation Report\n\n")
    md.write(f"**Generated:** {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
    md.write(f"**Snapshot:** {snapshot}\n\n")
    md.write(f"**Input tree:** `{tree_file.relative_to(REPO_ROOT)}`\n\n")
    md.write(f"**Output file:** `{output_file.relative_to(REPO_ROOT)}`\n\n")
    md.write("---\n\n")

    # Summary Statistics
    md.write("## Summary\n\n")
    input_count = match_result['input_count']
    output_count = match_result['output_count']
    matched_count = len(match_result['matched_pairs'])
    unmatched_input_count = len(match_result['unmatched_input'])
    unmatched_output_count = len(match_result['unmatched_output'])
    perfect_count = len(perfect_matches)
    imperfect_count = len(imperfect_matches)

    md.write("| Metric | Count | Percentage |\n")
    md.write("|--------|------:|----------:|\n")
    md.write(f"| Input halos (snapshot {snapshot}) | {input_count} | 100.0% |\n")
    md.write(f"| Output halos (Type 0+1) | {output_count} | {output_count/input_count*100:.1f}% |\n")
    md.write(f"| Matched pairs | {matched_count} | {matched_count/input_count*100:.1f}% |\n")
    md.write(f"| Perfect matches | {perfect_count} | {perfect_count/input_count*100:.1f}% |\n")
    md.write(f"| Imperfect matches | {imperfect_count} | {imperfect_count/input_count*100:.1f}% |\n")
    md.write(f"| Unmatched input | {unmatched_input_count} | {unmatched_input_count/input_count*100:.1f}% |\n")
    md.write(f"| Unmatched output | {unmatched_output_count} | {unmatched_output_count/output_count*100 if output_count > 0 else 0:.1f}% |\n\n")

    # Breakdown by halo type
    if perfect_matches or imperfect_matches:
        central_perfect = sum(1 for in_idx, out_idx in perfect_matches if output_halos[out_idx].Type == 0)
        central_imperfect = sum(1 for in_idx, out_idx, _ in imperfect_matches if output_halos[out_idx].Type == 0)
        satellite_perfect = sum(1 for in_idx, out_idx in perfect_matches if output_halos[out_idx].Type == 1)
        satellite_imperfect = sum(1 for in_idx, out_idx, _ in imperfect_matches if output_halos[out_idx].Type == 1)

        md.write("### By Halo Type\n\n")
        md.write("| Type | Perfect | Imperfect | Total |\n")
        md.write("|------|--------:|----------:|------:|\n")
        md.write(f"| Centrals (Type=0) | {central_perfect} | {central_imperfect} | {central_perfect + central_imperfect} |\n")
        md.write(f"| Satellites (Type=1) | {satellite_perfect} | {satellite_imperfect} | {satellite_perfect + satellite_imperfect} |\n\n")

    # Properties validated
    md.write("### Properties Validated\n\n")
    md.write("Directly copied from input tree:\n")
    md.write("- **Vectors:** Pos[3], Vel[3], Spin[3] (component-wise, rtol=1e-6)\n")
    md.write("- **Integers:** Len, SnapNum (exact match)\n")
    md.write("- **Floats:** Vmax, VelDisp (rtol=1e-6)\n")
    md.write("- **ID:** MostBoundID (exact match)\n")
    md.write("- **Mvir:** Conditional for Type=0 centrals (when input Mvir >= 0.0)\n\n")
    md.write("---\n\n")

    # Imperfect matches section
    if imperfect_matches:
        md.write("## ⚠️ Imperfect Matches\n\n")
        md.write(f"**Total:** {len(imperfect_matches)} halos with property mismatches\n\n")

        # Truncate at 100
        display_count = min(len(imperfect_matches), 100)
        md.write(f"**Displaying:** First {display_count} of {len(imperfect_matches)}\n\n")

        for i, (in_idx, out_idx, mismatches) in enumerate(imperfect_matches[:100]):
            in_halo = input_halos[in_idx]
            out_halo = output_halos[out_idx]

            md.write(f"### Imperfect Match {i+1}\n\n")
            md.write("**Metadata:**\n")
            md.write(f"- Input index: {in_idx}\n")
            md.write(f"- Output index: {out_idx}\n")
            md.write(f"- Type: {out_halo.Type} ({'Central' if out_halo.Type == 0 else 'Satellite'})\n")
            md.write(f"- UniqueGalaxyID: {out_halo.UniqueGalaxyID}\n")
            md.write(f"- MostBoundID: {out_halo.MostBoundID}\n\n")

            md.write("**Property Comparison:**\n\n")
            md.write("| Property | Input | Output | Match | Rel. Diff |\n")
            md.write("|----------|------:|-------:|:-----:|----------:|\n")

            # Show ALL properties with status
            props_to_check = [
                ('SnapNum', False, None),
                ('Len', False, None),
                ('Pos[x]', True, 0), ('Pos[y]', True, 1), ('Pos[z]', True, 2),
                ('Vel[x]', True, 0), ('Vel[y]', True, 1), ('Vel[z]', True, 2),
                ('Spin[x]', True, 0), ('Spin[y]', True, 1), ('Spin[z]', True, 2),
                ('Vmax', False, None),
                ('VelDisp', False, None),
                ('MostBoundID', False, None),
            ]

            # Check if Mvir should be validated
            # in_idx IS the halonr (index in InputTreeHalos array)
            halonr = in_idx
            if out_halo.Type == 0 and halonr < len(input_halos):
                is_fof_central = (halonr == input_halos[halonr].FirstHaloInFOFgroup)
                has_valid_mvir = (in_halo['Mvir'] >= 0.0)
                if is_fof_central and has_valid_mvir:
                    props_to_check.append(('Mvir', False, None))

            # Build mismatch lookup
            mismatch_lookup = {prop: (expected, actual, diff) for prop, expected, actual, diff in mismatches}

            for prop_name, is_vector, comp_idx in props_to_check:
                if is_vector:
                    base_name = prop_name.split('[')[0]
                    in_val = in_halo[base_name][comp_idx]
                    out_val = out_halo[base_name][comp_idx]
                else:
                    if prop_name == 'MostBoundID':
                        in_val = in_halo['MostBoundID']
                        out_val = out_halo['MostBoundID']
                    else:
                        in_val = in_halo[prop_name]
                        out_val = out_halo[prop_name]

                # Check if this property failed
                if prop_name in mismatch_lookup:
                    expected, actual, diff = mismatch_lookup[prop_name]
                    match_status = "❌"
                    if diff is not None:
                        diff_str = f"{diff:.2e}"
                    else:
                        diff_str = "N/A"
                else:
                    match_status = "✓"
                    diff_str = "-"

                # Format values
                if isinstance(in_val, (int, np.integer)):
                    in_str = f"{in_val}"
                    out_str = f"{out_val}"
                elif isinstance(in_val, (float, np.floating)):
                    in_str = f"{in_val:.6e}"
                    out_str = f"{out_val:.6e}"
                else:
                    in_str = str(in_val)
                    out_str = str(out_val)

                md.write(f"| {prop_name} | {in_str} | {out_str} | {match_status} | {diff_str} |\n")

            md.write("\n")

        if len(imperfect_matches) > 100:
            md.write(f"*... and {len(imperfect_matches) - 100} more imperfect matches (truncated)*\n\n")

    # Unmatched input halos
    if match_result['unmatched_input']:
        md.write("## Unmatched Input Halos\n\n")
        unmatched_halos = input_halos[match_result['unmatched_input']]
        md.write(f"**Total:** {len(unmatched_halos)} input halos not found in output\n\n")

        # Statistics
        md.write("**Statistics:**\n")
        md.write(f"- Len range: {np.min(unmatched_halos.Len)} - {np.max(unmatched_halos.Len)}\n")
        md.write(f"- Mvir range: {np.min(unmatched_halos.Mvir):.3f} - {np.max(unmatched_halos.Mvir):.3f} (1e10 Msun/h)\n")
        md.write(f"- Vmax range: {np.min(unmatched_halos.Vmax):.2f} - {np.max(unmatched_halos.Vmax):.2f} (km/s)\n\n")

        # Check patterns
        mvir_zero_count = np.sum(unmatched_halos.Mvir == 0.0)
        if mvir_zero_count == len(unmatched_halos):
            md.write(f"**Pattern detected:** All {mvir_zero_count} unmatched halos have Mvir=0.0\n\n")

        # Show first 100
        display_count = min(len(unmatched_halos), 100)
        md.write(f"**Displaying:** First {display_count} of {len(unmatched_halos)}\n\n")
        md.write("| Index | MostBoundID | Len | Mvir | Vmax | VelDisp |\n")
        md.write("|------:|------------:|----:|-----:|-----:|--------:|\n")
        for i, idx in enumerate(match_result['unmatched_input'][:100]):
            halo = input_halos[idx]
            md.write(f"| {idx} | {halo.MostBoundID} | {halo.Len} | {halo.Mvir:.3f} | {halo.Vmax:.2f} | {halo.VelDisp:.2f} |\n")

        if len(unmatched_halos) > 100:
            md.write(f"\n*... and {len(unmatched_halos) - 100} more (truncated)*\n\n")

    # Unmatched output halos (should be empty if correct)
    if match_result['unmatched_output']:
        md.write("## ⚠️ Unmatched Output Halos\n\n")
        md.write(f"**Total:** {len(match_result['unmatched_output'])} output halos not found in input\n\n")
        md.write("**WARNING:** This suggests halos are being incorrectly created!\n\n")

        display_count = min(len(match_result['unmatched_output']), 100)
        md.write(f"**Displaying:** First {display_count} of {len(match_result['unmatched_output'])}\n\n")
        md.write("| Index | Type | MostBoundID | Len | Mvir | Vmax |\n")
        md.write("|------:|-----:|--------------------:|----:|-----:|-----:|\n")
        for idx in match_result['unmatched_output'][:100]:
            halo = output_halos[idx]
            md.write(f"| {idx} | {halo.Type} | {halo.MostBoundID} | {halo.Len} | {halo.Mvir:.3f} | {halo.Vmax:.2f} |\n")

        if len(match_result['unmatched_output']) > 100:
            md.write(f"\n*... and {len(match_result['unmatched_output']) - 100} more (truncated)*\n\n")

    # Success section
    if not imperfect_matches and not match_result['unmatched_output']:
        md.write("## ✅ Validation Success\n\n")
        md.write(f"All {perfect_count} matched halos have perfectly preserved properties!\n\n")
        md.write("**Properties validated:**\n")
        md.write("- Pos[3], Vel[3], Spin[3] (vectors)\n")
        md.write("- Len, SnapNum (integers)\n")
        md.write("- Vmax, VelDisp (floats)\n")
        md.write("- MostBoundID (original simulation halo ID)\n")
        md.write("- Mvir (conditional for Type=0 centrals)\n\n")
        md.write("**Tolerance:** 1e-6 relative for floats, exact for integers\n\n")

    md.write("---\n\n")
    md.write("*Report generated by test_tree_preservation.py*\n")

    return md.getvalue()


def test_tree_preservation_coverage():
    """
    Validate snapshot 63 halo counts and matching coverage.

    What: Check if Type 0+1 output count matches input count at snapshot 63
    Expected: Counts should match if no halos lost/added
    Validates: Halo processing doesn't incorrectly lose or add halos
    """
    print("Testing tree preservation coverage...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Load input tree
    tree_file = TEST_DATA_DIR / "input" / "trees_063.0"
    print(f"  Loading input: {tree_file.relative_to(REPO_ROOT)}")
    input_halos, tree_meta = load_binary_tree(tree_file)
    print(f"    → {tree_meta['totNHalos']} total halos, {tree_meta['Ntrees']} trees")

    # Load output
    output_dir = TEST_DATA_DIR / "output" / "binary"
    output_file = output_dir / "model_z0.000_0"

    # Always regenerate output for the selected model so a stale file cannot
    # satisfy this assertion.
    param_file = model_input_file("test_binary.yaml")
    run_mimic_fresh(param_file, output_file)

    print(f"  Loading output: {output_file.relative_to(REPO_ROOT)}")
    output_halos, output_meta = load_binary_halos(output_file)
    print(f"    → {output_meta['TotHalos']} total halos")

    # Match halos at snapshot 63
    print(f"\n  Matching snapshot 63 halos...")
    match_result = match_halos_snapshot63(input_halos, output_halos, snapshot=63)

    input_count = match_result['input_count']
    output_count = match_result['output_count']
    matched_count = len(match_result['matched_pairs'])
    unmatched_input_count = len(match_result['unmatched_input'])
    unmatched_output_count = len(match_result['unmatched_output'])

    # Report counts
    print(f"  Input snapshot 63 halos: {input_count}")
    print(f"  Output Type 0+1 halos: {output_count}")
    print(f"  Matched pairs: {matched_count}")
    print(f"  Unmatched input: {unmatched_input_count}")
    print(f"  Unmatched output: {unmatched_output_count}")

    # Check if counts match
    if input_count == output_count:
        print(f"  ✓ Counts match! No halos lost or added.")
    else:
        diff = output_count - input_count
        if diff > 0:
            print(f"{YELLOW}  ⚠ Output has {diff} MORE halos than input (investigate){NC}")
        else:
            print(f"{YELLOW}  ⚠ Output has {-diff} FEWER halos than input (investigate){NC}")

    # Validate matching coverage
    match_rate = matched_count / input_count * 100 if input_count > 0 else 0
    print(f"\n  Match rate: {match_rate:.1f}%")

    # Diagnostic information about unmatched input halos
    if unmatched_input_count > 0:
        unmatched_halos = input_halos[match_result['unmatched_input']]
        print(f"{YELLOW}\n  Diagnostic: Unmatched input halos ({unmatched_input_count}):{NC}")
        print(f"{YELLOW}    Len range: {np.min(unmatched_halos.Len)} - {np.max(unmatched_halos.Len)}{NC}")
        print(f"{YELLOW}    Mvir range: {np.min(unmatched_halos.Mvir):.3f} - {np.max(unmatched_halos.Mvir):.3f} (1e10 Msun/h){NC}")
        print(f"{YELLOW}    Vmax range: {np.min(unmatched_halos.Vmax):.2f} - {np.max(unmatched_halos.Vmax):.2f} (km/s){NC}")

        # Check if all have Mvir=0.0
        mvir_zero_count = np.sum(unmatched_halos.Mvir == 0.0)
        if mvir_zero_count == unmatched_input_count:
            print(f"{YELLOW}    Note: All {mvir_zero_count} unmatched halos have Mvir=0.0 (invalid mass){NC}")
            # Check if matched halos also have Mvir=0.0
            matched_indices = [idx for idx, _ in match_result['matched_pairs']]
            matched_halos = input_halos[matched_indices]
            matched_mvir_zero = np.sum(matched_halos.Mvir == 0.0)
            print(f"{YELLOW}    Note: {matched_mvir_zero} matched halos also have Mvir=0.0{NC}")
            print(f"{YELLOW}    → Mvir=0.0 alone is NOT the filtering criterion{NC}")

    # Expect high match rate (>95%)
    assert matched_count > 0, "No matches found! Matching algorithm broken."
    assert match_rate > 95.0, (
        f"Match rate too low: {match_rate:.1f}% (expected >95%). "
        f"This suggests halos are being lost in processing."
    )

    # Expect no unmatched output (all output should be in input)
    assert unmatched_output_count == 0, (
        f"Found {unmatched_output_count} output halos NOT in input! "
        f"This suggests halos are being incorrectly created."
    )

    print(f"\n  ✓ Coverage validation passed")
    if unmatched_input_count > 0:
        print(f"{YELLOW}  Note: {unmatched_input_count} input halos not in output (may be filtered by processing){NC}")


def test_tree_preservation_properties():
    """
    Validate copied properties for ALL matched snapshot 63 halos.

    What: Compares all directly-copied properties (Pos, Vel, Spin, Len, Vmax, VelDisp, etc.)
    Expected: Perfect matches within tolerance (1e-6)
    Validates: Physics-free data pipeline preserves input exactly
    Categorizes: Perfect matches, imperfect matches (with breakdown)
    """
    print("Testing tree preservation of copied properties...")

    if not MIMIC_EXE.exists():
        print("  Skipping (Mimic not built)")
        return

    # Load data
    tree_file = TEST_DATA_DIR / "input" / "trees_063.0"
    input_halos, _ = load_binary_tree(tree_file)

    output_file = TEST_DATA_DIR / "output" / "binary" / "model_z0.000_0"
    # Always regenerate output for the selected model so a stale file cannot
    # satisfy this assertion.
    param_file = model_input_file("test_binary.yaml")
    run_mimic_fresh(param_file, output_file)

    output_halos, _ = load_binary_halos(output_file)

    # Match halos at snapshot 63
    match_result = match_halos_snapshot63(input_halos, output_halos, snapshot=63)
    matched_pairs = match_result['matched_pairs']

    print(f"  Validating {len(matched_pairs)} matched halos...")

    # Validate each matched pair
    perfect_matches = []
    imperfect_matches = []

    for in_idx, out_idx in matched_pairs:
        # in_idx IS the halonr (index in InputTreeHalos array)
        result = validate_copied_properties(
            input_halos[in_idx],
            output_halos[out_idx],
            in_idx,  # halonr
            input_halos,
            rtol=1e-6
        )
        if result['passed']:
            perfect_matches.append((in_idx, out_idx))
        else:
            imperfect_matches.append((in_idx, out_idx, result['mismatches']))

    # Calculate statistics
    total = len(matched_pairs)
    perfect_count = len(perfect_matches)
    imperfect_count = len(imperfect_matches)
    perfect_pct = perfect_count / total * 100 if total > 0 else 0
    imperfect_pct = imperfect_count / total * 100 if total > 0 else 0

    # Report results
    print(f"\n  Validation Results:")
    print(f"    Total matched: {total}")
    print(f"    Perfect matches: {perfect_count} ({perfect_pct:.1f}%)")
    print(f"    Imperfect matches: {imperfect_count} ({imperfect_pct:.1f}%)")

    # Break down by halo type
    central_perfect = sum(1 for in_idx, out_idx in perfect_matches if output_halos[out_idx].Type == 0)
    central_imperfect = sum(1 for in_idx, out_idx, _ in imperfect_matches if output_halos[out_idx].Type == 0)
    satellite_perfect = sum(1 for in_idx, out_idx in perfect_matches if output_halos[out_idx].Type == 1)
    satellite_imperfect = sum(1 for in_idx, out_idx, _ in imperfect_matches if output_halos[out_idx].Type == 1)

    print(f"\n  By halo type:")
    print(f"    Centrals (Type=0): {central_perfect} perfect, {central_imperfect} imperfect")
    print(f"    Satellites (Type=1): {satellite_perfect} perfect, {satellite_imperfect} imperfect")

    # Generate detailed markdown report
    print(f"\n  Generating detailed markdown report...")
    report_content = generate_markdown_report(
        match_result, input_halos, output_halos,
        perfect_matches, imperfect_matches,
        tree_file, output_file, snapshot=63
    )

    # Save report to tests/data/output/
    report_dir = TEST_DATA_DIR / "output"
    report_dir.mkdir(parents=True, exist_ok=True)
    report_file = report_dir / "tree_preservation_report.md"

    with open(report_file, 'w') as f:
        f.write(report_content)

    print(f"    → Report saved: {report_file.relative_to(REPO_ROOT)}")

    # If there are imperfect matches, report details
    if imperfect_matches:
        report = format_validation_failures(imperfect_matches, "PROPERTY VALIDATION")
        print(f"\n{report}")
        print(f"\n  See detailed report at: {report_file.relative_to(REPO_ROOT)}")
        assert False, f"Found {imperfect_count} halos with property mismatches (see report for details)"

    print(f"  ✓ All {perfect_count} halos have perfectly preserved properties")
    print(f"    Properties validated: Pos[3], Vel[3], Spin[3], Len, Vmax, VelDisp, SnapNum, MostBoundID")
    print(f"    Mvir validated conditionally for Type=0 centrals (when input Mvir >= 0.0)")


def main():
    """Main test runner."""
    # Print test suite header
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: Tree Preservation (test_tree_preservation.py) {NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()
    print(f"Repository root: {REPO_ROOT}")
    print(f"Mimic executable: {MIMIC_EXE}")

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_tree_preservation_coverage,
        test_tree_preservation_properties,
    ]

    passed = 0
    failed = 0
    skipped = 0

    for test in tests:
        print()
        try:
            test()
            print(f"{GREEN}✓ PASS: {test.__name__}{NC}")
            passed += 1
        except AssertionError as e:
            print(f"{RED}✗ FAIL: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1
        except Exception as e:
            print(f"{RED}✗ ERROR: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1

    # Print summary
    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    print(f"Failed:  {failed}")
    print(f"Skipped: {skipped}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
