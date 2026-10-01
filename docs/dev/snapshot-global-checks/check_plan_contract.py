#!/usr/bin/env python3
"""Planning-only contract checker for MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md.

Runs the frozen plan through the *installed* Project Manager library
(``pm_lib.plan.parse_plan`` / ``plan_check_report`` and
``pm_lib.prompts.render_developer_prompt`` / ``render_reviewer_prompt``) and
checks that:

- all five slice receipts parse with every required section present;
- the risk flags parse to the values the plan prose promises;
- eligibility behaves as the plan's approval checkpoint describes;
- every authorized-surface entry normalizes cleanly, concrete witness paths are
  authorized and known-unauthorized paths are rejected by PM's segment-aware
  matcher;
- the binding contract text (critical phrases, recommended model/effort, every
  acceptance checkbox) survives prompt extraction into BOTH seats;
- the repository anchors cited by the plan still hold at the working tree;
- the repository has not drifted from the planning baseline outside the
  planning surface: HEAD must descend from the baseline commit, and every path
  changed since it (committed, staged, unstaged or untracked) must belong to the
  plan, the pathway, the two planning records or this checker directory. A
  planning-only commit is therefore allowed; any runtime, test, script, skill or
  other documentation change fails; a missing or failing Git is an explicit error;
- deliberately corrupted plan variants (written to temporary storage) produce
  the check-plan errors and warnings they should, and the variants PM cannot
  see are caught by the compensating guards implemented here.

Usage::

    python3 docs/dev/snapshot-global-checks/check_plan_contract.py [--pm-scripts DIR]
        [--skills-root DIR] [--plan FILE] [--json OUT]

Exit status is nonzero when any check fails or cannot run. This is a planning
experiment: passing it proves properties of the plan text and of the PM
tooling, not that any implementation exists or works. It is a pre-implementation
checker: once a slice lands, the drift check and the "new path absent" checks
are expected to fail, and that is the correct outcome, not a defect.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from checklib import (  # noqa: E402
    DEFAULT_PM_SCRIPTS,
    DEFAULT_SKILLS_ROOT,
    FIXTURES,
    PLAN_PATH,
    REPO_ROOT,
    Checker,
    import_pm_lib,
)
from plan_mutations import MUTATIONS  # noqa: E402

EXPECTED_SLICE_COUNT = 5
# From the plan's "Implementation Profiles" table; used to cross-check the
# per-slice "Recommended Developer" lines that actually reach the seats.
PROFILE_TABLE_RE = re.compile(r"^\|\s*(\d+)\s*\|\s*(Claude \w+)\s*\|\s*(\w+)\s*\|", re.MULTILINE)
RECOMMENDED_RE = re.compile(r"Recommended Developer:\s*(Claude \w+);\s*effort:\s*(\w+)")
ALLOWED_MODELS = {"Claude Opus", "Claude Sonnet"}
ALLOWED_EFFORTS = {"low", "medium", "high"}
CHECKBOX_RE = re.compile(r"^- \[ \] .+$", re.MULTILINE)
REVIEWER_SKILLS = ("drift-audit", "code-review")
# The exact planning surface that may differ from the planning baseline before PM
# initialization. A trailing slash authorizes a subtree, segment-aware like PM's matcher;
# anything else, including other files under docs/ or docs/dev/, is treated as drift.
PLANNING_SURFACE = (
    "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md",
    "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-CHECKS.md",
    "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-REVIEW.md",
    "docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md",
    "docs/dev/snapshot-global-checks/",
)


class GitUnavailable(Exception):
    """Git could not answer: missing binary, not a repository, or a failing command."""


def load_critical_phrases() -> dict[int, list[str]]:
    phrases: dict[int, list[str]] = {}
    for raw in (FIXTURES / "critical_phrases.txt").read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        number, phrase = line.split("|", 1)
        phrases.setdefault(int(number), []).append(phrase)
    return phrases


def git_output(repo: Path, *args: str, ok_codes: tuple[int, ...] = (0,)) -> tuple[int, str]:
    """Run one Git command and return ``(returncode, stdout)``; raise on any other failure."""
    try:
        result = subprocess.run(
            ["git", "-C", str(repo), *args],
            text=True,
            capture_output=True,
            check=False,
        )
    except OSError as exc:
        raise GitUnavailable(f"git {' '.join(args)}: {exc}") from exc
    if result.returncode not in ok_codes:
        raise GitUnavailable(
            f"git {' '.join(args)} exited {result.returncode}: {result.stderr.strip()}"
        )
    return result.returncode, result.stdout


def is_planning_path(path: str) -> bool:
    """Segment-aware membership in ``PLANNING_SURFACE``."""
    for entry in PLANNING_SURFACE:
        if entry.endswith("/"):
            if path.startswith(entry):
                return True
        elif path == entry:
            return True
    return False


def classify_drift(paths) -> tuple[list[str], list[str]]:
    """Split changed paths into ``(planning_surface, outside_surface)``, both sorted."""
    planning, outside = [], []
    for path in sorted(set(paths)):
        (planning if is_planning_path(path) else outside).append(path)
    return planning, outside


def parse_porcelain_z(status: str) -> dict[str, list[str]]:
    """Parse ``git status --porcelain=v1 -z`` into staged/unstaged/untracked path lists.

    Renamed and copied entries carry the original path as the next NUL-separated token;
    both names are recorded so a rename out of the planning surface is still visible.
    """
    staged: list[str] = []
    unstaged: list[str] = []
    untracked: list[str] = []
    tokens = status.split("\0")
    i = 0
    while i < len(tokens):
        token = tokens[i]
        i += 1
        if not token:
            continue
        xy, path = token[:2], token[3:]
        paths = [path]
        if xy[0] in "RC" or xy[1] in "RC":
            paths.append(tokens[i])
            i += 1
        if xy == "??":
            untracked.extend(paths)
            continue
        if xy == "!!":
            continue
        if xy[0] not in " ?":
            staged.extend(paths)
        if xy[1] not in " ?":
            unstaged.extend(paths)
    return {"staged": staged, "unstaged": unstaged, "untracked": untracked}


def collect_drift(repo: Path, baseline: str) -> dict:
    """Every path that differs from the planning baseline, by how it differs.

    Raises ``GitUnavailable`` when Git cannot answer, so a missing repository is never
    mistaken for a clean one.
    """
    _, head = git_output(repo, "rev-parse", "HEAD")
    head = head.strip()
    code, _ = git_output(repo, "merge-base", "--is-ancestor", baseline, head, ok_codes=(0, 1))
    descends = code == 0
    committed: list[str] = []
    if descends and head != baseline:
        _, names = git_output(repo, "diff", "--name-only", "-z", baseline, head)
        committed = [p for p in names.split("\0") if p]
    _, status = git_output(repo, "status", "--porcelain=v1", "-z", "--untracked-files=all")
    categories = parse_porcelain_z(status)
    categories["committed"] = committed
    return {
        "head": head,
        "baseline": baseline,
        "head_descends_from_baseline": descends,
        **categories,
    }


# Synthetic path sets exercising ``classify_drift`` without touching Git. Each entry is
# [name, paths, paths that must be reported as outside the planning surface].
DRIFT_CASES: list[list] = [
    [
        "planning_surface_only",
        [
            "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md",
            "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-CHECKS.md",
            "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-REVIEW.md",
            "docs/dev/MIMIC-DEVELOPMENT-PATHWAY.md",
            "docs/dev/snapshot-global-checks/fixtures/witness_paths.json",
            "docs/dev/snapshot-global-checks/results/.gitignore",
        ],
        [],
    ],
    [
        "runtime_source",
        ["src/core/module_registry.c"],
        ["src/core/module_registry.c"],
    ],
    [
        "test_and_script",
        ["tests/framework/harness.py", "scripts/module_modes.py"],
        ["scripts/module_modes.py", "tests/framework/harness.py"],
    ],
    [
        "skill_file",
        [".agents/skills/mimic-modules/SKILL.md"],
        [".agents/skills/mimic-modules/SKILL.md"],
    ],
    [
        "docs_outside_surface",
        ["docs/USER-GUIDE.md", "docs/VISION.md", "README.md"],
        ["README.md", "docs/USER-GUIDE.md", "docs/VISION.md"],
    ],
    [
        "other_dev_plan",
        ["docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md"],
        ["docs/dev/MIMIC-CHUNKED-SLAB-STREAMING-PLAN.md"],
    ],
    [
        "plan_name_prefix_lookalike",
        ["docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-ACCEPTANCE.md"],
        ["docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN-ACCEPTANCE.md"],
    ],
    [
        "checker_dir_lookalikes",
        ["docs/dev/snapshot-global-checks-old/x.py", "docs/dev/snapshot-global-checks.py"],
        ["docs/dev/snapshot-global-checks-old/x.py", "docs/dev/snapshot-global-checks.py"],
    ],
    [
        "checker_dir_bare_name_is_conservative",
        ["docs/dev/snapshot-global-checks"],
        ["docs/dev/snapshot-global-checks"],
    ],
    [
        "generated_and_build_outputs",
        ["src/include/generated/property_defs.h", "build/generated/x.h"],
        ["build/generated/x.h", "src/include/generated/property_defs.h"],
    ],
    [
        "pm_bookkeeping_is_conservative",
        [".pm/run.json"],
        [".pm/run.json"],
    ],
    [
        "mixed_reports_only_outside",
        [
            "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md",
            "Makefile",
            "docs/dev/snapshot-global-checks/README.md",
        ],
        ["Makefile"],
    ],
    ["empty_set", [], []],
]


# --- compensating guards (the plan-level checks PM does not perform) ---------


def guard_critical_phrases(
    chk: Checker,
    prefix: str,
    dev_prompts: dict[int, str],
    rev_prompts: dict[int, str],
    phrases: dict[int, list[str]],
) -> bool:
    all_ok = True
    for number, items in phrases.items():
        for phrase in items:
            in_dev = phrase in dev_prompts.get(number, "")
            in_rev = phrase in rev_prompts.get(number, "")
            ok = in_dev and in_rev
            all_ok &= ok
            if prefix == "plan":
                short = phrase if len(phrase) <= 48 else phrase[:45] + "..."
                chk.check(
                    f"critical_phrase.slice{number}.{short!r}",
                    ok,
                    f"developer={in_dev} reviewer={in_rev}",
                )
    return all_ok


def guard_model_effort(chk: Checker, prefix: str, text: str, slices) -> bool:
    table = {int(n): (m, e) for n, m, e in PROFILE_TABLE_RE.findall(text)}
    ok = True
    for plan_slice in slices:
        match = RECOMMENDED_RE.search(plan_slice.sections.get("Intended Change", ""))
        if not match:
            ok = False
            if prefix == "plan":
                chk.fail(
                    f"model_effort.slice{plan_slice.number}",
                    "no 'Recommended Developer: <model>; effort: <level>' line",
                )
            continue
        model, effort = match.group(1), match.group(2)
        consistent = (
            model in ALLOWED_MODELS
            and effort in ALLOWED_EFFORTS
            and table.get(plan_slice.number) == (model, effort)
        )
        ok &= consistent
        if prefix == "plan":
            chk.check(
                f"model_effort.slice{plan_slice.number}",
                consistent,
                f"receipt says {model}/{effort}, table says {table.get(plan_slice.number)}",
            )
    return ok


def guard_explicit_audit_flags(slices) -> bool:
    return all(s._risk_flag("Independent audit required") in {"yes", "no"} for s in slices)


def guard_all_slices_elevated(slices) -> bool:
    return all(s.plan_risk == "elevated" for s in slices)


# --- the real-plan checks ----------------------------------------------------


def render_all_prompts(
    prompts_mod, slices, plan_path: Path, skills_root: Path, tmp: Path, baseline: str
):
    dev: dict[int, str] = {}
    rev: dict[int, dict[str, str]] = {}
    for plan_slice in slices:
        artifact_dir = tmp / f"slice-{plan_slice.number}"
        dev[plan_slice.number] = prompts_mod.render_developer_prompt(
            plan_slice,
            plan_path=plan_path,
            artifact_dir=artifact_dir,
            notes_path=tmp / "notes.md",
            result_path=artifact_dir / "result.json",
            before_head=baseline,
            skills_root=skills_root,
        )
        rev[plan_slice.number] = {}
        for skill in REVIEWER_SKILLS:
            rev[plan_slice.number][skill] = prompts_mod.render_reviewer_prompt(
                skill_name=skill,
                repo=str(REPO_ROOT),
                slice_id=plan_slice.slice_id,
                slice_title=plan_slice.title,
                before_head=baseline,
                reviewed_head="0" * 40,
                diff_path=str(artifact_dir / "diff.patch"),
                changed_files=plan_slice.authorized_files[:2],
                intended_change=plan_slice.sections.get("Intended Change", "").rstrip(),
                acceptance_criteria=plan_slice.sections.get("Acceptance Criteria", "").rstrip(),
                authorized_surface=plan_slice.sections.get("Authorized Surface", "").rstrip(),
                explicit_non_goals=plan_slice.sections.get("Explicit Non-Goals", "").rstrip(),
                risk_flags=plan_slice.sections.get("Risk Flags", "").rstrip(),
                skills_root=skills_root,
            )
    return dev, rev


def check_real_plan(
    chk: Checker,
    plan_mod,
    prompts_mod,
    git_ops,
    PmError,
    plan_path: Path,
    skills_root: Path,
    report: dict,
) -> None:
    text = plan_path.read_text(encoding="utf-8")
    anchors = json.loads((FIXTURES / "anchors.json").read_text(encoding="utf-8"))
    baseline = anchors["planning_baseline"]
    report["planning_baseline"] = baseline

    # 0. Drift from the planning baseline. Planning-only commits are allowed; any
    #    committed, staged, unstaged or untracked path outside PLANNING_SURFACE fails.
    for name, paths, expected_outside in DRIFT_CASES:
        _, outside = classify_drift(paths)
        chk.check(
            f"drift.classify.{name}",
            outside == sorted(expected_outside),
            f"outside={outside} expected={sorted(expected_outside)}",
        )
    try:
        drift = collect_drift(REPO_ROOT, baseline)
    except GitUnavailable as exc:
        chk.error("baseline.git_available", str(exc))
        report["drift"] = {"error": str(exc)}
    else:
        chk.ok("baseline.git_available")
        report["head"] = drift["head"]
        chk.check(
            "baseline.head_descends_from_planning_baseline",
            drift["head_descends_from_baseline"],
            f"HEAD {drift['head']} does not descend from {baseline}",
        )
        summary = {}
        outside_any: dict[str, list[str]] = {}
        for category in ("committed", "staged", "unstaged", "untracked"):
            planning, outside = classify_drift(drift[category])
            summary[category] = {"planning_surface": planning, "outside_surface": outside}
            if outside:
                outside_any[category] = outside
        report["drift"] = {
            "head": drift["head"],
            "head_descends_from_baseline": drift["head_descends_from_baseline"],
            **summary,
        }
        chk.check(
            "baseline.no_drift_outside_planning_surface",
            not outside_any,
            "; ".join(f"{k}: {v}" for k, v in outside_any.items()),
        )
        planning_touched = sorted({p for c in summary.values() for p in c["planning_surface"]})
        counts = ", ".join(
            f"{k}={len(drift[k])}" for k in ("committed", "staged", "unstaged", "untracked")
        )
        chk.note(
            f"HEAD {drift['head'][:12]} vs baseline {baseline[:12]}: "
            f"{len(planning_touched)} planning-surface path(s) differ ({counts})"
        )

    # 1. Parse and check-plan on the frozen plan.
    slices = plan_mod.parse_plan(plan_path)
    chk.check(
        "parse.slice_count", len(slices) == EXPECTED_SLICE_COUNT, f"parsed {len(slices)} slices"
    )
    chk.check(
        "parse.slice_numbers_contiguous",
        [s.number for s in slices] == list(range(1, len(slices) + 1)),
        f"numbers {[s.number for s in slices]}",
    )
    for plan_slice in slices:
        chk.check(
            f"parse.slice{plan_slice.number}.sections_complete",
            not plan_slice.missing_sections,
            f"missing {plan_slice.missing_sections}",
        )
        chk.check(
            f"parse.slice{plan_slice.number}.no_foreign_sections",
            set(plan_slice.sections) == set(plan_mod.REQUIRED_SECTIONS),
            f"sections {sorted(plan_slice.sections)}",
        )
    chk.check(
        "parse.slice5_excludes_next_chat_prompts",
        "Next Chat Prompts" not in slices[-1].body,
        "slice 5 body absorbed a later heading",
    )

    pm_report = plan_mod.plan_check_report(plan_path, REPO_ROOT)
    report["check_plan"] = pm_report
    chk.check("check_plan.no_errors", not pm_report["errors"], "; ".join(pm_report["errors"]))
    chk.check(
        "check_plan.approval_gated_is_slices_1_to_4",
        pm_report["approval_gated"] == ["Slice 1", "Slice 2", "Slice 3", "Slice 4"],
        str(pm_report["approval_gated"]),
    )
    for warning in pm_report["warnings"]:
        chk.note(f"check-plan warning: {warning}")
    chk.check(
        "check_plan.no_warnings",
        not pm_report["warnings"],
        f"{len(pm_report['warnings'])} warning(s), see notes",
    )

    # 2. Risk fields.
    expected_approval = {1: True, 2: True, 3: True, 4: True, 5: False}
    for plan_slice in slices:
        n = plan_slice.number
        chk.check(
            f"risk.slice{n}.approval_needed",
            plan_slice.approval_needed is expected_approval[n],
            f"got {plan_slice.approval_needed}",
        )
        chk.check(
            f"risk.slice{n}.independent_audit_required",
            plan_slice.independent_audit_required is True,
            "audit flag not exactly 'yes'",
        )
        chk.check(
            f"risk.slice{n}.plan_risk_elevated",
            plan_slice.plan_risk == "elevated",
            f"got {plan_slice.plan_risk}",
        )
    risky5 = slices[4]._risk_flag("Risky surfaces touched")
    chk.check(
        "risk.slice5.risky_surfaces_not_literal_none",
        slices[4].risky_surfaces_clear is False,
        f"value {risky5!r}",
    )
    chk.note(
        "Slice 5 'Risky surfaces touched' is not the literal 'none' (it reads "
        f"{risky5!r}), so PM records it as a risky surface touched; plan_risk is elevated regardless because the audit flag is 'yes'."
    )
    chk.check(
        "guard.explicit_audit_flags",
        guard_explicit_audit_flags(slices),
        "an audit flag is not an explicit yes/no",
    )
    chk.check(
        "guard.all_slices_elevated",
        guard_all_slices_elevated(slices),
        "a slice derives standard risk",
    )

    # 3. Eligibility with and without recorded approvals.
    for plan_slice in slices:
        n = plan_slice.number
        eligible, reasons = plan_mod.eligibility(plan_slice)
        if expected_approval[n]:
            chk.check(
                f"eligibility.slice{n}.blocked_without_approval",
                not eligible and any("approval-needed" in r for r in reasons),
                f"eligible={eligible} reasons={reasons}",
            )
            eligible_after, reasons_after = plan_mod.eligibility(plan_slice, {plan_slice.slice_id})
            chk.check(
                f"eligibility.slice{n}.eligible_with_approval",
                eligible_after,
                f"reasons={reasons_after}",
            )
        else:
            chk.check(
                f"eligibility.slice{n}.eligible_without_approval", eligible, f"reasons={reasons}"
            )
    chk.check(
        "eligibility.next_slice_is_1",
        plan_mod.next_slice(slices, {"slices": []}).number == 1,
        "next_slice did not pick Slice 1",
    )
    state = {"slices": [{"id": f"Slice {k}", "status": "accepted"} for k in range(1, 5)]}
    chk.check(
        "eligibility.next_slice_after_1_to_4_is_5",
        plan_mod.next_slice(slices, state).number == 5,
        "next_slice after accepting 1-4 is not Slice 5",
    )

    # 4. Authorized surfaces and path matching.
    witness = json.loads((FIXTURES / "witness_paths.json").read_text(encoding="utf-8"))
    surfaces: dict[str, dict] = {}
    for plan_slice in slices:
        n = plan_slice.number
        entries = plan_slice.authorized_files
        normalized = [git_ops.normalize_authorized_entry(e) for e in entries]
        chk.check(f"surface.slice{n}.has_entries", bool(entries), "no entries captured")
        bad = [
            (e, plan_mod.authorized_entry_error(e))
            for e in entries
            if plan_mod.authorized_entry_error(e)
        ]
        chk.check(f"surface.slice{n}.entries_valid", not bad, str(bad))
        chk.check(
            f"surface.slice{n}.no_duplicate_entries",
            len(set(normalized)) == len(normalized),
            f"duplicates in {normalized}",
        )
        classified = {}
        for norm in normalized:
            target = REPO_ROOT / norm
            if norm.endswith("/"):
                kind = "dir-envelope-existing" if target.is_dir() else "dir-envelope-new"
            elif target.is_file():
                kind = "file-existing"
            elif target.is_dir():
                kind = "DIRECTORY-WITHOUT-SLASH"
            else:
                kind = "file-new"
            classified[norm] = kind
        chk.check(
            f"surface.slice{n}.no_plain_directory_entries",
            "DIRECTORY-WITHOUT-SLASH" not in classified.values(),
            str(classified),
        )
        surfaces[str(n)] = {"entries": entries, "normalized": normalized, "classified": classified}
        for path in witness["slices"][str(n)]["witness"]:
            chk.check(
                f"surface.slice{n}.witness.{path}",
                git_ops.is_authorized_path(path, entries),
                "witness path rejected",
            )
        for path in witness["slices"][str(n)]["unauthorized"] + witness["everywhere_unauthorized"]:
            chk.check(
                f"surface.slice{n}.reject.{path}",
                not git_ops.is_authorized_path(path, entries),
                "unauthorized path accepted",
            )
        chk.check(
            f"surface.slice{n}.unauthorized_files_helper",
            git_ops.unauthorized_files(
                set(witness["slices"][str(n)]["witness"])
                | {"docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md"},
                entries,
            )
            == ["docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md"],
            "unauthorized_files did not isolate exactly the plan file",
        )
    report["surfaces"] = surfaces
    # The plan's own text says the plan file may never be edited by an implementation session.
    for plan_slice in slices:
        chk.check(
            f"surface.slice{plan_slice.number}.plan_file_never_authorized",
            not git_ops.is_authorized_path(
                "docs/dev/MIMIC-SNAPSHOT-GLOBAL-MODULES-PLAN.md", plan_slice.authorized_files
            ),
            "plan authorizes editing itself",
        )
    # Makefile entries carry parenthetical annotations that must be stripped.
    for n in (1, 3, 4):
        chk.check(
            f"surface.slice{n}.makefile_annotation_stripped",
            "Makefile" in surfaces[str(n)]["normalized"],
            f"normalized {surfaces[str(n)]['normalized']}",
        )

    # 5. Prompt extraction for both seats.
    phrases = load_critical_phrases()
    with tempfile.TemporaryDirectory(prefix="mimic-plan-checks-") as tmpdir:
        tmp = Path(tmpdir)
        try:
            dev, rev = render_all_prompts(
                prompts_mod, slices, plan_path, skills_root, tmp, baseline
            )
        except PmError as exc:
            chk.error("prompts.render", f"PmError: {exc}")
            return
    lint_script = skills_root / "lint" / "scripts" / "lint.py"
    chk.check("prompts.lint_script_exists", lint_script.is_file(), f"{lint_script} missing")
    seat_sections = (
        "Intended Change",
        "Acceptance Criteria",
        "Authorized Surface",
        "Explicit Non-Goals",
        "Risk Flags",
    )
    dev_only_sections = ("Validation Plan", "Rollback Path")
    prompt_stats = {}
    for plan_slice in slices:
        n = plan_slice.number
        d = dev[n]
        chk.check(
            f"prompts.slice{n}.developer_no_unresolved_fields",
            not re.search(r"\{[a-z_]+\}", d.replace('{"slice"', "")),
            "unresolved {field} in developer prompt",
        )
        chk.check(
            f"prompts.slice{n}.developer_lint_command",
            f"python3 {lint_script} check --base {baseline}" in d,
            "lint command not rendered with baseline",
        )
        for section in seat_sections + dev_only_sections:
            body = plan_slice.sections[section].rstrip()
            chk.check(
                f"prompts.slice{n}.developer_has.{section}",
                body in d,
                "section text not verbatim in developer prompt",
            )
        for skill, r in rev[n].items():
            for section in seat_sections:
                body = plan_slice.sections[section].rstrip()
                chk.check(
                    f"prompts.slice{n}.reviewer[{skill}]_has.{section}",
                    body in r,
                    "section text not verbatim in reviewer prompt",
                )
            chk.check(
                f"prompts.slice{n}.reviewer[{skill}]_embeds_skill",
                f"BEGIN EMBEDDED SKILL FILE: {(skills_root / skill / 'SKILL.md').resolve()}" in r,
                "skill bundle missing",
            )
            chk.check(
                f"prompts.slice{n}.reviewer[{skill}]_no_unresolved_fields",
                not re.search(r"\{[a-z_]+\}", r),
                "unresolved {field} in reviewer prompt",
            )
        boxes = CHECKBOX_RE.findall(plan_slice.sections["Acceptance Criteria"])
        chk.check(
            f"prompts.slice{n}.acceptance_all_checkboxes",
            boxes
            and len(boxes)
            == sum(
                1
                for line in plan_slice.sections["Acceptance Criteria"].splitlines()
                if line.startswith("- ")
            ),
            "an acceptance bullet is not a '- [ ]' checkbox",
        )
        in_both = all(b in dev[n] and all(b in r for r in rev[n].values()) for b in boxes)
        chk.check(
            f"prompts.slice{n}.all_{len(boxes)}_checkboxes_reach_both_seats",
            in_both,
            "a checkbox line is missing from a seat",
        )
        chk.check(
            f"prompts.slice{n}.required_evidence_in_acceptance",
            any("Required evidence" in b for b in boxes),
            "no 'Required evidence' checkbox (Validation Plan does not reach the Reviewer)",
        )
        recommended = RECOMMENDED_RE.search(plan_slice.sections["Intended Change"])
        chk.check(
            f"prompts.slice{n}.model_effort_reaches_both_seats",
            bool(recommended)
            and recommended.group(0) in d
            and all(recommended.group(0) in r for r in rev[n].values()),
            "recommended model/effort not in both seats",
        )
        prompt_stats[n] = {
            "developer_chars": len(d),
            "reviewer_chars": {k: len(v) for k, v in rev[n].items()},
            "checkboxes": len(boxes),
        }
    report["prompts"] = prompt_stats
    chk.note(
        "Validation Plan and Rollback Path are rendered into the Developer prompt only; the Reviewer template has no field for them."
    )
    rev_first = {n: rev[n][REVIEWER_SKILLS[0]] for n in rev}
    guard_critical_phrases(chk, "plan", dev, rev_first, phrases)
    guard_model_effort(chk, "plan", text, slices)

    # 6. Repository anchors and name resolution.
    for anchor in anchors["anchors"]:
        path = REPO_ROOT / anchor["path"]
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        window = "\n".join(lines[anchor["start"] - 1 : anchor["end"]])
        missing = [needle for needle in anchor["must_contain"] if needle not in window]
        chk.check(
            f"anchor.{anchor['path']}:{anchor['start']}-{anchor['end']}",
            not missing,
            f"missing {missing}",
        )
    for ident in anchors["identifiers"]:
        path = REPO_ROOT / ident["path"]
        ok = path.is_file() and (
            ident["contains"] in path.read_text(encoding="utf-8", errors="replace")
        )
        chk.check(f"identifier.{ident['path']}:{ident['contains'][:40]}", ok, "not found")
    for rel in anchors["must_not_exist_yet"]:
        chk.check(
            f"new_path_absent.{rel}",
            not (REPO_ROOT / rel).exists(),
            "plan-named new path already exists at baseline",
        )
    for rel in anchors.get("must_not_exist_removed_envelopes", []):
        chk.check(
            f"removed_envelope_absent.{rel}",
            not (REPO_ROOT / rel).exists(),
            "a path the plan revision removed from its surface exists at baseline",
        )
    new_unannotated = []
    for plan_slice in slices:
        for entry in plan_slice.authorized_files:
            norm = git_ops.normalize_authorized_entry(entry)
            kind = surfaces[str(plan_slice.number)]["classified"][norm]
            if kind in {"file-new", "dir-envelope-new"} and "new" not in entry.lower():
                new_unannotated.append((plan_slice.number, norm))
    report["new_unannotated_entries"] = new_unannotated
    chk.note(
        f"Authorized entries that do not exist at baseline and are not annotated 'new': {new_unannotated}"
    )


# --- corrupted variants ------------------------------------------------------


def check_mutations(
    chk: Checker,
    plan_mod,
    prompts_mod,
    git_ops,
    PmError,
    plan_path: Path,
    skills_root: Path,
    report: dict,
) -> None:
    original = plan_path.read_text(encoding="utf-8")
    phrases = load_critical_phrases()
    outcomes = []
    with tempfile.TemporaryDirectory(prefix="mimic-plan-mutants-") as tmpdir:
        tmp = Path(tmpdir)
        for mutation in MUTATIONS:
            name = f"mutant.{mutation.name}"
            try:
                mutated = mutation.apply(original)
            except ValueError as exc:
                chk.error(name, f"mutation could not be applied: {exc}")
                continue
            if mutated == original:
                chk.error(name, "mutation left the plan unchanged")
                continue
            variant = tmp / f"{mutation.name}.md"
            variant.write_text(mutated, encoding="utf-8")
            pm_report = plan_mod.plan_check_report(variant, REPO_ROOT)
            errors, warnings = pm_report["errors"], pm_report["warnings"]
            problems: list[str] = []
            for needle in mutation.expect_errors:
                if not any(needle in e for e in errors):
                    problems.append(f"expected error containing {needle!r}; got {errors}")
            for needle in mutation.expect_warnings:
                if not any(needle in w for w in warnings):
                    problems.append(f"expected warning containing {needle!r}; got {warnings}")
            if mutation.expect_pm_silent and errors:
                problems.append(f"expected PM to be silent but it reported {errors}")
            slices = plan_mod.parse_plan(variant)
            for number, attr, expected in mutation.expect_attrs:
                target = next((s for s in slices if s.number == number), None)
                actual = getattr(target, attr) if target else "<missing slice>"
                if actual != expected:
                    problems.append(f"slice {number}.{attr} = {actual!r}, expected {expected!r}")
            guard_result = None
            if mutation.guard or mutation.name == "braces_in_acceptance_text":
                try:
                    dev, rev = render_all_prompts(
                        prompts_mod, slices, variant, skills_root, tmp / mutation.name, "0" * 40
                    )
                except PmError as exc:
                    problems.append(f"prompt rendering raised PmError: {exc}")
                    dev, rev = {}, {}
                rev_first = {n: rev[n][REVIEWER_SKILLS[0]] for n in rev}
                if mutation.name == "braces_in_acceptance_text":
                    rendered_ok = (
                        bool(dev)
                        and "{post_snapshot: [{name: x}]}" in dev[2]
                        and "{post_snapshot: [{name: x}]}" in rev_first[2]
                    )
                    if not rendered_ok:
                        problems.append("literal braces did not survive rendering into both seats")
                if mutation.guard == "critical_phrases":
                    guard_result = guard_critical_phrases(chk, "mutant", dev, rev_first, phrases)
                elif mutation.guard == "model_effort_consistency":
                    guard_result = guard_model_effort(chk, "mutant", mutated, slices)
                elif mutation.guard == "explicit_audit_flags":
                    guard_result = guard_explicit_audit_flags(slices)
                elif mutation.guard == "all_slices_elevated":
                    guard_result = guard_all_slices_elevated(slices)
                if mutation.guard and guard_result is not False:
                    problems.append(
                        f"compensating guard {mutation.guard!r} did not fail on this variant"
                    )
            outcomes.append(
                {
                    "mutation": mutation.name,
                    "description": mutation.description,
                    "pm_errors": errors,
                    "pm_warnings": warnings,
                    "pm_silent": not errors,
                    "guard": mutation.guard,
                    "guard_failed": (guard_result is False) if mutation.guard else None,
                    "verdict": "as-expected" if not problems else "UNEXPECTED",
                    "problems": problems,
                }
            )
            chk.check(name, not problems, "; ".join(problems))
    report["mutations"] = outcomes


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--pm-scripts", type=Path, default=DEFAULT_PM_SCRIPTS)
    parser.add_argument("--skills-root", type=Path, default=DEFAULT_SKILLS_ROOT)
    parser.add_argument("--plan", type=Path, default=PLAN_PATH)
    parser.add_argument(
        "--json", type=Path, default=None, help="write a machine-readable report here"
    )
    args = parser.parse_args()

    chk = Checker("plan_contract")
    report: dict = {
        "plan": str(args.plan),
        "pm_scripts": str(args.pm_scripts),
        "skills_root": str(args.skills_root),
        "python": sys.version.split()[0],
    }
    try:
        plan_mod, prompts_mod, git_ops, PmError = import_pm_lib(args.pm_scripts)
    except ImportError as exc:
        chk.error("setup.import_pm_lib", str(exc))
        return chk.finish()
    report["pm_lib"] = str(Path(plan_mod.__file__).resolve().parent)
    chk.ok("setup.import_pm_lib")
    if not args.plan.is_file():
        chk.error("setup.plan_exists", f"{args.plan} missing")
        return chk.finish()
    # PM freezes a run to this digest; record it so the evidence names the exact plan bytes checked.
    report["plan_sha256"] = plan_mod.plan_digest(args.plan.resolve())
    chk.note(f"plan sha256 {report['plan_sha256']}")
    check_real_plan(
        chk,
        plan_mod,
        prompts_mod,
        git_ops,
        PmError,
        args.plan.resolve(),
        args.skills_root.resolve(),
        report,
    )
    check_mutations(
        chk,
        plan_mod,
        prompts_mod,
        git_ops,
        PmError,
        args.plan.resolve(),
        args.skills_root.resolve(),
        report,
    )
    report["summary"] = {
        "passed": len(chk.passed),
        "failed": len(chk.failed),
        "skipped": len(chk.skipped),
        "errors": len(chk.errored),
        "notes": chk.notes,
    }
    report["failures"] = chk.failed
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(report, indent=2, default=str), encoding="utf-8")
    return chk.finish()


if __name__ == "__main__":
    sys.exit(main())
