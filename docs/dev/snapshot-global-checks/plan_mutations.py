"""Deliberately corrupted variants of the frozen plan.

Each mutation is a pure function on the plan text plus an expectation about
what Project Manager's ``plan_check_report`` must say about the result. Some
mutations are expected to pass PM's mechanical check silently; those exist to
show where the parser is blind and where the compensating guards in
``check_plan_contract.py`` (critical-phrase survival, model/effort consistency,
explicit audit flags) have to carry the load.

The variants are written to temporary storage only, never next to the plan.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Callable


@dataclass(frozen=True)
class Mutation:
    name: str
    description: str
    apply: Callable[[str], str]
    # Substrings that must appear in check-plan errors (all of them).
    expect_errors: tuple[str, ...] = ()
    # Substrings that must appear in check-plan warnings (all of them).
    expect_warnings: tuple[str, ...] = ()
    # When True the mutation must produce NO check-plan error at all; it is a
    # demonstration of a parser blind spot and a compensating guard must catch it.
    expect_pm_silent: bool = False
    # Name of the compensating guard expected to fail on this variant, if any.
    guard: str | None = None
    # Extra assertions on the parsed slices: list of (slice_number, attribute, expected).
    expect_attrs: tuple[tuple[int, str, object], ...] = field(default_factory=tuple)


def _slice_span(text: str, number: int) -> tuple[int, int]:
    """Character span of ``## Slice <number>:`` up to the next ``## `` heading."""
    match = re.search(rf"^## Slice {number}:.*$", text, flags=re.MULTILINE)
    if not match:
        raise ValueError(f"Slice {number} heading not found")
    rest = re.search(r"^## .+$", text[match.end() :], flags=re.MULTILINE)
    end = match.end() + rest.start() if rest else len(text)
    return match.start(), end


def _edit_slice(text: str, number: int, func: Callable[[str], str]) -> str:
    start, end = _slice_span(text, number)
    body = func(text[start:end])
    return text[:start] + body + text[end:]


def _require(text: str, needle: str) -> None:
    if needle not in text:
        raise ValueError(f"expected plan text not found for mutation: {needle!r}")


def _drop_line_containing(text: str, needle: str) -> str:
    _require(text, needle)
    return "\n".join(line for line in text.split("\n") if needle not in line)


def _remove_section(body: str, heading: str) -> str:
    pattern = rf"^### {re.escape(heading)}\s*$\n(?:(?!^### ).*\n?)*"
    new_body, count = re.subn(pattern, "", body, flags=re.MULTILINE)
    if count != 1:
        raise ValueError(f"section {heading!r} not found exactly once")
    return new_body


def _blank_section(body: str, heading: str) -> str:
    pattern = rf"(^### {re.escape(heading)}\s*$\n)(?:(?!^### ).*\n?)*"
    new_body, count = re.subn(pattern, r"\1\n", body, flags=re.MULTILINE)
    if count != 1:
        raise ValueError(f"section {heading!r} not found exactly once")
    return new_body


def _add_surface_entry(body: str, entry: str) -> str:
    _require(body, "- Files allowed to change:\n")
    return body.replace(
        "- Files allowed to change:\n", f"- Files allowed to change:\n  - {entry}\n", 1
    )


def _set_risk_line(body: str, label: str, value: str | None) -> str:
    pattern = rf"^- {re.escape(label)}:.*$"
    if value is None:
        new_body, count = re.subn(pattern, "", body, flags=re.MULTILINE)
    else:
        new_body, count = re.subn(pattern, f"- {label}: {value}", body, flags=re.MULTILINE)
    if count != 1:
        raise ValueError(f"risk line {label!r} not found exactly once")
    return new_body


def _dedent_file_list(body: str) -> str:
    """Move the file entries under 'Files allowed to change:' to column 0.

    A plan author who writes the list flush-left produces sibling bullets that
    PM's capture stops at, so the slice silently authorizes nothing.
    """
    lines = body.split("\n")
    out: list[str] = []
    in_list = False
    for line in lines:
        if line.startswith("- Files allowed to change:"):
            in_list = True
            out.append(line)
            continue
        if in_list and line.startswith("  - "):
            out.append(line[2:])
            continue
        in_list = False
        out.append(line)
    return "\n".join(out)


MUTATIONS: tuple[Mutation, ...] = (
    Mutation(
        "missing_rollback_section",
        "Slice 2 loses its Rollback Path section entirely",
        lambda t: _edit_slice(t, 2, lambda b: _remove_section(b, "Rollback Path")),
        expect_errors=("Slice 2 (", "missing required sections: Rollback Path"),
    ),
    Mutation(
        "empty_acceptance_section",
        "Slice 3 keeps the Acceptance Criteria heading but its body is blank",
        lambda t: _edit_slice(t, 3, lambda b: _blank_section(b, "Acceptance Criteria")),
        expect_errors=("Slice 3 (", "missing required sections: Acceptance Criteria"),
    ),
    Mutation(
        "absolute_surface_entry",
        "Slice 1 authorizes an absolute path",
        lambda t: _edit_slice(
            t, 1, lambda b: _add_surface_entry(b, "`/Users/someone/mimic/src/core/x.c`")
        ),
        expect_errors=("Slice 1 (", "is absolute"),
    ),
    Mutation(
        "dot_slash_surface_entry",
        "Slice 1 authorizes a './'-prefixed path that matches no git path",
        lambda t: _edit_slice(
            t, 1, lambda b: _add_surface_entry(b, "`./src/core/module_registry.c`")
        ),
        expect_errors=("Slice 1 (", "redundant './' prefix"),
    ),
    Mutation(
        "unwrapped_annotation_entry",
        "Slice 1 writes 'Makefile (generator dependencies only)' without backticks",
        lambda t: _edit_slice(
            t, 1, lambda b: _add_surface_entry(b, "Makefile (generator dependencies only)")
        ),
        expect_errors=("Slice 1 (", "unwrapped whitespace"),
    ),
    Mutation(
        "backslash_surface_entry",
        "Slice 1 authorizes a backslash-separated path",
        lambda t: _edit_slice(
            t, 1, lambda b: _add_surface_entry(b, "`src\\core\\module_registry.c`")
        ),
        expect_errors=("Slice 1 (", "uses a backslash"),
    ),
    Mutation(
        "parent_segment_entry",
        "Slice 1 authorizes a path containing '..'",
        lambda t: _edit_slice(t, 1, lambda b: _add_surface_entry(b, "`src/../Makefile`")),
        expect_errors=("Slice 1 (", "'.' or '..' path segment"),
    ),
    Mutation(
        "whole_repository_entry",
        "Slice 5 authorizes '**/*'",
        lambda t: _edit_slice(t, 5, lambda b: _add_surface_entry(b, "`**/*`")),
        expect_warnings=("Slice 5 (", "authorizes the entire repository"),
    ),
    Mutation(
        "dependency_manifest_entry",
        "Slice 3 authorizes requirements.txt",
        lambda t: _edit_slice(t, 3, lambda b: _add_surface_entry(b, "`requirements.txt`")),
        expect_warnings=("Slice 3 (", "looks dependency-shaped"),
    ),
    Mutation(
        "license_entry",
        "Slice 5 authorizes LICENSE",
        lambda t: _edit_slice(t, 5, lambda b: _add_surface_entry(b, "`LICENSE`")),
        expect_warnings=("Slice 5 (", "looks license-shaped"),
    ),
    Mutation(
        "directory_without_trailing_slash",
        "Slice 1 names an existing directory as a plain path (matches no git change)",
        lambda t: _edit_slice(
            t, 1, lambda b: _add_surface_entry(b, "`src/module_system/test_fixture`")
        ),
        expect_warnings=("Slice 1 (", "names an existing directory but is written as a plain path"),
    ),
    Mutation(
        "dedented_file_list",
        "Slice 1's file list is written flush-left, so PM captures no files",
        lambda t: _edit_slice(t, 1, _dedent_file_list),
        expect_errors=("Slice 1 (", "authorized surface has no files allowed to change"),
    ),
    Mutation(
        "approval_yes_after_review",
        "Slice 2 approval flag reads 'yes, after review'",
        lambda t: _edit_slice(
            t,
            2,
            lambda b: _set_risk_line(
                b, "Approval needed before implementation", "yes, after review"
            ),
        ),
        expect_errors=("Slice 2 (", "must be exactly 'yes' or 'no'"),
        expect_attrs=((2, "approval_needed", None),),
    ),
    Mutation(
        "approval_not_yet",
        "Slice 4 approval flag reads 'not yet' (prefix test would fail open)",
        lambda t: _edit_slice(
            t, 4, lambda b: _set_risk_line(b, "Approval needed before implementation", "not yet")
        ),
        expect_errors=("Slice 4 (", "must be exactly 'yes' or 'no'"),
        expect_attrs=((4, "approval_needed", None),),
    ),
    Mutation(
        "approval_line_missing",
        "Slice 1 approval line is deleted",
        lambda t: _edit_slice(
            t, 1, lambda b: _set_risk_line(b, "Approval needed before implementation", None)
        ),
        expect_errors=("Slice 1 (", "must be exactly 'yes' or 'no'"),
        expect_attrs=((1, "approval_needed", None),),
    ),
    Mutation(
        "approval_capitalized_with_period",
        "Slice 1 approval flag reads 'Yes.' (tolerated: case-insensitive, trailing period stripped)",
        lambda t: _edit_slice(
            t, 1, lambda b: _set_risk_line(b, "Approval needed before implementation", "Yes.")
        ),
        expect_pm_silent=True,
        expect_attrs=((1, "approval_needed", True),),
    ),
    Mutation(
        "audit_flag_blank",
        "Slice 5's audit flag is blank; PM fails closed to OFF without an error",
        lambda t: _edit_slice(t, 5, lambda b: _set_risk_line(b, "Independent audit required", "")),
        expect_pm_silent=True,
        guard="explicit_audit_flags",
        expect_attrs=((5, "independent_audit_required", False),),
    ),
    Mutation(
        "audit_and_risky_cleared_demotes_risk",
        "Slice 5 with audit 'no' and risky surfaces 'none' silently becomes standard risk",
        lambda t: _edit_slice(
            t,
            5,
            lambda b: _set_risk_line(
                _set_risk_line(b, "Independent audit required", "no"),
                "Risky surfaces touched",
                "none",
            ),
        ),
        expect_pm_silent=True,
        guard="all_slices_elevated",
        expect_attrs=((5, "plan_risk", "standard"),),
    ),
    Mutation(
        "duplicate_slice_number",
        "Slice 5 is renumbered to Slice 4",
        lambda t: t.replace("## Slice 5: Publish", "## Slice 4: Publish", 1),
        expect_errors=("duplicate slice numbers: 4",),
    ),
    Mutation(
        "fenced_slice_heading",
        "A '## Slice 6:' example sits inside a code fence in Slice 5",
        lambda t: _edit_slice(
            t,
            5,
            lambda b: b.replace(
                "### Rollback Path",
                "```text\n## Slice 6: Example inside a fence\n```\n\n### Rollback Path",
                1,
            ),
        ),
        expect_errors=("sits inside a fenced code block",),
    ),
    Mutation(
        "malformed_slice_heading",
        "A heading '## Slice 6 - Extra work' lacks the required colon form",
        lambda t: t.replace(
            "## Next Chat Prompts", "## Slice 6 - Extra work\n\ntext\n\n## Next Chat Prompts", 1
        ),
        expect_errors=("malformed slice heading",),
    ),
    Mutation(
        "nested_h3_slice_heading",
        "A '### Slice 6: Nested' heading appears inside Slice 5",
        lambda t: _edit_slice(
            t,
            5,
            lambda b: b.replace("### Rollback Path", "### Slice 6: Nested\n\n### Rollback Path", 1),
        ),
        expect_errors=("malformed slice heading",),
    ),
    Mutation(
        "unclosed_code_fence",
        "A code fence is opened at the end of the plan and never closed",
        lambda t: t + "\n```yaml\nmodules:\n  post_snapshot: []\n",
        expect_errors=("unclosed code fence",),
    ),
    Mutation(
        "slice_batches_section",
        "A 'Slice Batches' section groups slices (Mode A only)",
        lambda t: t.replace(
            "## Next Chat Prompts",
            "## Slice Batches\n\n- Batch A: Slices 1-2\n\n## Next Chat Prompts",
            1,
        ),
        expect_warnings=("plan defines slice batches",),
    ),
    Mutation(
        "lost_oracle_criterion",
        "Slice 3 loses the hand-solvable oracle acceptance line; PM cannot notice",
        lambda t: _edit_slice(
            t, 3, lambda b: _drop_line_containing(b, "winning the tie against ID 42")
        ),
        expect_pm_silent=True,
        guard="critical_phrases",
    ),
    Mutation(
        "lost_expanded_form_criterion",
        "Slice 3 loses the expanded log-form Outputs line (with its 'never materialise n_r' clause); PM cannot notice",
        lambda t: _edit_slice(t, 3, lambda b: _drop_line_containing(b, "Never materialise `n_r`")),
        expect_pm_silent=True,
        guard="critical_phrases",
    ),
    Mutation(
        "bound_moved_after_exp",
        "Slice 3's log-space upper bound becomes a check of the exponentiated double; PM cannot notice",
        lambda t: t.replace(
            "first fail the snapshot if `ln M_r > log(100000.0)`",
            "first fail the snapshot if `M_r > 100000`",
            1,
        ),
        expect_pm_silent=True,
        guard="critical_phrases",
    ),
    Mutation(
        "lost_mpeak_flt_max_clause",
        "Slice 3 drops the FLT_MAX bound on the updated ShamMpeak before the cast; PM cannot notice",
        lambda t: t.replace(
            "must be `<= FLT_MAX` before it is cast to float storage",
            "is cast to float storage",
            1,
        ),
        expect_pm_silent=True,
        guard="critical_phrases",
    ),
    Mutation(
        "lost_harness_key_criterion",
        "Slice 2 loses the harness post_snapshot lifecycle-key line; PM cannot notice",
        lambda t: _edit_slice(
            t, 2, lambda b: _drop_line_containing(b, "routes a `post_snapshot` key")
        ),
        expect_pm_silent=True,
        guard="critical_phrases",
    ),
    Mutation(
        "identity_test_moved_back_to_integration",
        "Slice 4's manual identity test is renamed into tests/integration/ (where make tests would auto-discover it); PM cannot notice",
        lambda t: t.replace(
            "tests/manual/test_snapshot_disabled_identity.py",
            "tests/integration/test_snapshot_disabled_identity.py",
        ),
        expect_pm_silent=True,
        guard="critical_phrases",
    ),
    Mutation(
        "lost_event_rejection_criterion",
        "Slice 2 loses the module_emit_event rejection line; PM cannot notice",
        lambda t: _edit_slice(t, 2, lambda b: _drop_line_containing(b, "module_emit_event")),
        expect_pm_silent=True,
        guard="critical_phrases",
    ),
    Mutation(
        "weakened_ulp_tolerance",
        "Slice 3's 'two float ULPs' becomes 'two percent'; PM cannot notice",
        lambda t: t.replace("at most two float ULPs", "at most two percent", 1),
        expect_pm_silent=True,
        guard="critical_phrases",
    ),
    Mutation(
        "swapped_model_effort",
        "Slice 4's receipt says Opus/low while the profile table says Sonnet/high; PM cannot notice",
        lambda t: _edit_slice(
            t,
            4,
            lambda b: b.replace(
                "Recommended Developer: Claude Sonnet; effort: high",
                "Recommended Developer: Claude Opus; effort: low",
                1,
            ),
        ),
        expect_pm_silent=True,
        guard="model_effort_consistency",
    ),
    Mutation(
        "braces_in_acceptance_text",
        "Slice 2 acceptance text contains literal braces; rendering must not treat them as fields",
        lambda t: _edit_slice(
            t,
            2,
            lambda b: b.replace(
                "- [ ] Inputs:",
                "- [ ] Inputs: `{post_snapshot: [{name: x}]}` is a JSON-shaped example.",
                1,
            ),
        ),
        expect_pm_silent=True,
    ),
)
