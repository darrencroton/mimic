"""Comprehensive halo-by-halo comparison between two Mimic outputs.

Used by the format-equivalence and committed-baseline tests; general enough
for any two structured halo arrays (binary vs HDF5, run vs baseline).
"""

from io import StringIO

import numpy as np

from .runner import GREEN, NC, YELLOW


def _write_ranked_mismatches(
    report, prop_name, ranked, summary, marker="❌", kind="mismatches", color=""
):
    """Write a property's diff block, worst (largest diff) first.

    Args:
        ranked: list of (sort_key, line) tuples covering EVERY diff in this band
            for this property. Sorted here by descending sort_key so the largest
            deviations are shown first, independent of halo ordering -- a small
            count of large diffs is never hidden behind low-id small diffs.
        summary: trailing summary line (already indented) describing the full set.
        marker/kind/color: presentation only. Defaults render a hard ❌ failure;
            callers pass a yellow ⚠️ variant for relaxed-tolerance warnings.
    """
    reset = NC if color else ""
    ranked.sort(key=lambda item: item[0], reverse=True)
    shown = min(10, len(ranked))
    report.write(
        f"\n{color}{marker} Property '{prop_name}' {kind} "
        f"(showing worst {shown} of {len(ranked)} by descending diff):{reset}\n"
    )
    for _key, line in ranked[:10]:
        report.write(f"  {color}{line}{reset}\n")
    if len(ranked) > 10:
        report.write(f"  ... and {len(ranked) - 10} more\n")
    report.write(summary + "\n")


def _classify_float_diffs(a, b, rtol, warn_rtol, atol):
    """Split 1-D float arrays into failing and warning index sets.

    Returns (fail_idx, warn_idx):
      fail_idx: not close at the effective (possibly relaxed) rtol -> these fail.
      warn_idx: close at rtol but NOT at the stricter warn_rtol -> tolerated by
                the relaxed gate yet still outside the strict baseline tolerance.
    When warn_rtol is None or not stricter than rtol there is no warning band.

    The absolute floor ``atol`` (numpy's |a-b| <= atol + rtol*|b|) keeps
    near-zero "dust" values -- where |b| is at the floating-point noise floor --
    from producing meaningless huge ratios in both the fail and warn bands.
    """
    close_eff = np.isclose(a, b, rtol=rtol, atol=atol, equal_nan=False)
    fail_idx = np.where(~close_eff)[0]
    if warn_rtol is None or warn_rtol >= rtol:
        warn_idx = np.empty(0, dtype=int)
    else:
        close_strict = np.isclose(a, b, rtol=warn_rtol, atol=atol, equal_nan=False)
        warn_idx = np.where(close_eff & ~close_strict)[0]
    return fail_idx, warn_idx


def _ranked_float_lines(a, b, indices, format_line):
    """Build a list of (rel_diff, line) for the given float diff indices."""
    ranked = []
    for i in indices:
        v1 = a[i]
        v2 = b[i]
        rel_diff = abs(v1) if v2 == 0.0 else abs(v1 - v2) / abs(v2)
        ranked.append((rel_diff, format_line(i, v1, v2, rel_diff)))
    return ranked


def compare_halos_comprehensive(
    halos1,
    halos2,
    label1="dataset1",
    label2="dataset2",
    rtol=1e-6,
    properties_to_compare=None,
    warn_rtol=None,
    atol=0.0,
):
    """
    Comprehensive comparison of all properties for all halos between two datasets.

    Compares every property of every halo, using appropriate comparison methods:
    - Integers: Exact match required
    - Floats: Relative tolerance (default 1e-6)
    - Vectors: Component-wise relative tolerance
    - Sentinels: Must match exactly (e.g., -1 for unset values)

    Args:
        halos1: First halo array (numpy recarray)
        halos2: Second halo array (numpy recarray)
        label1: Descriptive label for first dataset (e.g., "test")
        label2: Descriptive label for second dataset (e.g., "baseline")
        rtol: Relative tolerance for floating-point comparisons (default 1e-6).
              This is the gate: diffs beyond it fail the comparison.
        properties_to_compare: Optional set of property names to compare.
                             If None, compares all common properties.
                             If provided, only compares properties in this set.
        warn_rtol: Optional stricter tolerance. When the gate (rtol) has been
              relaxed above this value, float diffs that pass rtol but exceed
              warn_rtol are surfaced as yellow warnings (worst-first, top 10)
              without failing the comparison. None disables the warning band.
        atol: Absolute floor for float comparisons (numpy semantics:
              |a-b| <= atol + rtol*|b|). Default 0.0 keeps same-run equivalence
              checks strict; committed-baseline comparisons pass a small floor so
              near-zero "dust" values are not compared by ratio.

    Returns:
        tuple: (passed, report_text) where passed is bool and report_text is str
    """
    report = StringIO()
    all_passed = True
    had_warnings = False

    # Check halo counts match
    if len(halos1) != len(halos2):
        report.write(f"\n❌ HALO COUNT MISMATCH:\n")
        report.write(f"  {label1}: {len(halos1)} halos\n")
        report.write(f"  {label2}: {len(halos2)} halos\n")
        return False, report.getvalue()

    n_halos = len(halos1)

    # Get all property names
    props1 = set(halos1.dtype.names)
    props2 = set(halos2.dtype.names)

    # Determine which properties to compare
    if properties_to_compare is not None:
        # Filter to requested properties that exist in both datasets
        common_props = sorted((props1 & props2) & properties_to_compare)

        # Report if some requested properties are missing
        missing_in_1 = properties_to_compare - props1
        missing_in_2 = properties_to_compare - props2

        report.write(f"\nComparing {n_halos} halos across {len(common_props)} core properties...\n")

        if missing_in_1:
            report.write(
                f"⚠️  Core properties missing in {label1}: {', '.join(sorted(missing_in_1))}\n"
            )
            all_passed = False
        if missing_in_2:
            report.write(
                f"⚠️  Core properties missing in {label2}: {', '.join(sorted(missing_in_2))}\n"
            )
            all_passed = False

        # Report non-core properties (informational only)
        extra_in_1 = props1 - properties_to_compare
        extra_in_2 = props2 - properties_to_compare
        if extra_in_1 or extra_in_2:
            report.write(f"ℹ️  Additional properties present but not compared:\n")
            if extra_in_1:
                report.write(f"  In {label1}: {', '.join(sorted(extra_in_1))}\n")
            if extra_in_2:
                report.write(f"  In {label2}: {', '.join(sorted(extra_in_2))}\n")
    else:
        # Compare all common properties
        common_props = sorted(props1 & props2)
        report.write(f"\nComparing {n_halos} halos across all {len(common_props)} properties...\n")

        if props1 != props2:
            report.write(f"\n⚠️  Property sets differ:\n")
            only_in_1 = props1 - props2
            only_in_2 = props2 - props1
            if only_in_1:
                report.write(f"  Only in {label1}: {', '.join(sorted(only_in_1))}\n")
            if only_in_2:
                report.write(f"  Only in {label2}: {', '.join(sorted(only_in_2))}\n")

    for prop_name in common_props:
        arr1 = halos1[prop_name]
        arr2 = halos2[prop_name]

        # Determine property type
        dtype = arr1.dtype

        # Handle vector properties (3-component arrays)
        if len(arr1.shape) > 1 and arr1.shape[1] == 3:
            # Vector property (Pos, Vel, Spin) -- classify each component, then
            # pool failures and warnings across components for this property.
            fail_ranked = []
            warn_ranked = []
            for component in range(3):
                comp_name = ["x", "y", "z"][component]
                c1 = arr1[:, component]
                c2 = arr2[:, component]
                fail_idx, warn_idx = _classify_float_diffs(c1, c2, rtol, warn_rtol, atol)
                fmt = lambda i, v1, v2, rel, _c=comp_name: (
                    f"Halo {i} [{_c}]: {label1}={v1:.6e}, "
                    f"{label2}={v2:.6e} (rel_diff={rel:.2e})"
                )
                fail_ranked += _ranked_float_lines(c1, c2, fail_idx, fmt)
                warn_ranked += _ranked_float_lines(c1, c2, warn_idx, fmt)

            if fail_ranked:
                all_passed = False
                summary = (
                    f"  Summary: {len(fail_ranked)} component mismatches across "
                    f"{n_halos * 3} total components "
                    f"({100.0 * len(fail_ranked) / (n_halos * 3):.2f}%)"
                )
                _write_ranked_mismatches(report, prop_name, fail_ranked, summary)
            if warn_ranked:
                had_warnings = True
                summary = (
                    f"  {YELLOW}Summary: {len(warn_ranked)} components within relaxed "
                    f"rtol={rtol:.1e} but exceed strict rtol={warn_rtol:.1e}{NC}"
                )
                _write_ranked_mismatches(
                    report,
                    prop_name,
                    warn_ranked,
                    summary,
                    marker="⚠️",
                    kind="relaxed-tolerance warnings",
                    color=YELLOW,
                )

        # Handle scalar integer properties
        elif np.issubdtype(dtype, np.integer):
            # Exact comparison for integers
            if not np.array_equal(arr1, arr2):
                diffs = arr1 != arr2
                diff_indices = np.where(diffs)[0]

                all_passed = False
                ranked = []
                for halo_idx in diff_indices:
                    val1 = arr1[halo_idx]
                    val2 = arr2[halo_idx]
                    abs_diff = abs(int(val1) - int(val2))  # rank by magnitude of the integer gap
                    ranked.append(
                        (
                            abs_diff,
                            f"Halo {halo_idx}: {label1}={val1}, {label2}={val2}",
                        )
                    )
                summary = (
                    f"  Summary: {len(diff_indices)} of {n_halos} halos differ "
                    f"({100.0 * len(diff_indices) / n_halos:.2f}%)"
                )
                _write_ranked_mismatches(report, prop_name, ranked, summary)

        # Handle scalar floating-point properties
        elif np.issubdtype(dtype, np.floating):
            fail_idx, warn_idx = _classify_float_diffs(arr1, arr2, rtol, warn_rtol, atol)
            fmt = lambda i, v1, v2, rel: (
                f"Halo {i}: {label1}={v1:.6e}, " f"{label2}={v2:.6e} (rel_diff={rel:.2e})"
            )
            if len(fail_idx):
                all_passed = False
                ranked = _ranked_float_lines(arr1, arr2, fail_idx, fmt)
                summary = (
                    f"  Summary: {len(fail_idx)} of {n_halos} halos differ "
                    f"({100.0 * len(fail_idx) / n_halos:.2f}%)"
                )
                _write_ranked_mismatches(report, prop_name, ranked, summary)
            if len(warn_idx):
                had_warnings = True
                ranked = _ranked_float_lines(arr1, arr2, warn_idx, fmt)
                summary = (
                    f"  {YELLOW}Summary: {len(warn_idx)} of {n_halos} halos within "
                    f"relaxed rtol={rtol:.1e} but exceed strict rtol={warn_rtol:.1e}{NC}"
                )
                _write_ranked_mismatches(
                    report,
                    prop_name,
                    ranked,
                    summary,
                    marker="⚠️",
                    kind="relaxed-tolerance warnings",
                    color=YELLOW,
                )

        else:
            # Unknown type - try exact comparison
            if not np.array_equal(arr1, arr2):
                report.write(
                    f"\n⚠️  Property '{prop_name}' (type {dtype}) differs but comparison method unknown\n"
                )
                all_passed = False

    if all_passed:
        report.write(
            f"\n{GREEN}✓ All {len(common_props)} properties match for all {n_halos} halos{NC}\n"
        )
        if had_warnings:
            report.write(
                f"{YELLOW}⚠ Passed at the relaxed tolerance, but some fields exceed the "
                f"strict baseline tolerance (warnings above).{NC}\n"
            )

    return all_passed, report.getvalue()
