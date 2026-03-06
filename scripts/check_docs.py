#!/usr/bin/env python3
"""
Documentation quality checks for Mimic.

Checks:
1. Internal Markdown links and anchors resolve.
2. SAGE module phase listings in USER-GUIDE are consistent with:
   - input/millennium.yaml (canonical default configuration)
   - generated module registry (module existence)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
DOC_FILES = [
    REPO_ROOT / "docs" / "VISION.md",
    REPO_ROOT / "docs" / "DEVELOPER-GUIDE.md",
    REPO_ROOT / "docs" / "USER-GUIDE.md",
]
USER_GUIDE = REPO_ROOT / "docs" / "USER-GUIDE.md"
INPUT_CONFIG = REPO_ROOT / "input" / "millennium.yaml"
MODULE_REGISTRY = REPO_ROOT / "src" / "modules" / "_system" / "generated" / "module_init.c"

VALID_PHASES = {"pre_timestep", "phase_1", "phase_2", "post_timestep"}


def slugify_heading(heading: str) -> str:
    """Approximate GitHub anchor generation for heading links."""
    heading = heading.strip().lower()
    heading = re.sub(r"`", "", heading)
    heading = re.sub(r"[^\w\s-]", "", heading)
    heading = re.sub(r"\s+", "-", heading).strip("-")
    return heading


def extract_anchors(markdown_text: str) -> Set[str]:
    anchors: Set[str] = set()
    seen: Dict[str, int] = {}

    for line in markdown_text.splitlines():
        match = re.match(r"^\s{0,3}#{1,6}\s+(.*?)\s*$", line)
        if not match:
            continue

        heading = re.sub(r"\s+#+\s*$", "", match.group(1)).strip()
        base = slugify_heading(heading)
        if not base:
            continue

        index = seen.get(base, 0)
        anchor = base if index == 0 else f"{base}-{index}"
        seen[base] = index + 1
        anchors.add(anchor)

    return anchors


def strip_fenced_code_blocks(markdown_text: str) -> str:
    return re.sub(r"```.*?```", "", markdown_text, flags=re.S)


def extract_links(markdown_text: str) -> List[str]:
    text = strip_fenced_code_blocks(markdown_text)
    # Ignore image links (`![](...)`) and extract regular markdown links.
    return re.findall(r"(?<!!)\[[^\]]+\]\(([^)]+)\)", text)


def parse_markdown_target(raw_target: str) -> Tuple[str, str]:
    target = raw_target.strip()
    if target.startswith("<") and target.endswith(">"):
        target = target[1:-1].strip()
    if " " in target:
        # Handle optional markdown title: path "title"
        target = target.split(" ", 1)[0].strip()

    if "#" in target:
        path_part, anchor_part = target.split("#", 1)
    else:
        path_part, anchor_part = target, ""
    return path_part.strip(), anchor_part.strip()


def validate_internal_links() -> List[str]:
    errors: List[str] = []
    markdown_cache: Dict[Path, str] = {}
    anchor_cache: Dict[Path, Set[str]] = {}

    def format_path(path: Path) -> str:
        try:
            return str(path.relative_to(REPO_ROOT))
        except ValueError:
            return str(path)

    def load_markdown(path: Path) -> str:
        if path not in markdown_cache:
            markdown_cache[path] = path.read_text(encoding="utf-8")
            anchor_cache[path] = extract_anchors(markdown_cache[path])
        return markdown_cache[path]

    for doc_file in DOC_FILES:
        content = load_markdown(doc_file)
        for raw_target in extract_links(content):
            path_part, anchor_part = parse_markdown_target(raw_target)
            if not path_part and not anchor_part:
                continue
            if path_part.startswith(("http://", "https://", "mailto:")):
                continue

            if path_part:
                target_path = (doc_file.parent / path_part).resolve()
            else:
                target_path = doc_file.resolve()

            if not target_path.exists():
                errors.append(
                    f"{doc_file.relative_to(REPO_ROOT)}: broken link target '{raw_target}' "
                    f"(missing file: {format_path(target_path)})"
                )
                continue

            if not anchor_part:
                continue

            if target_path.suffix.lower() != ".md":
                continue

            target_content = load_markdown(target_path)
            _ = target_content  # cached side effect
            anchor = slugify_heading(anchor_part)
            if anchor not in anchor_cache[target_path]:
                errors.append(
                    f"{doc_file.relative_to(REPO_ROOT)}: broken anchor '{raw_target}' "
                    f"(missing '#{anchor}' in {target_path.relative_to(REPO_ROOT)})"
                )

    return errors


def parse_module_phase_map(yaml_text: str) -> Dict[str, Set[str]]:
    """Extract `sage_*` module -> phase set from a YAML snippet."""
    phase_map: Dict[str, Set[str]] = {}
    in_modules = False
    current_phase = None

    for raw_line in yaml_text.splitlines():
        line = raw_line.split("#", 1)[0].rstrip()
        if not line:
            continue

        if not in_modules:
            if re.match(r"^\s*modules:\s*$", line):
                in_modules = True
            continue

        # End module section at next top-level key.
        if re.match(r"^[A-Za-z0-9_]+:\s*$", line):
            break

        phase_match = re.match(r"^\s*(pre_timestep|phase_1|phase_2|post_timestep):\s*$", line)
        if phase_match:
            current_phase = phase_match.group(1)
            continue

        module_match = re.match(r"^\s*-\s*(sage_[a-z0-9_]+)\s*:\s*process_[a-z_]+\s*$", line)
        if module_match and current_phase:
            phase_map.setdefault(module_match.group(1), set()).add(current_phase)

    return phase_map


def parse_user_guide_example_phase_map(user_guide_text: str) -> Dict[str, Set[str]]:
    match = re.search(
        r"\*\*Example configuration\*\*.*?```yaml\n(.*?)```",
        user_guide_text,
        flags=re.S,
    )
    if not match:
        return {}
    return parse_module_phase_map(match.group(1))


def parse_user_guide_table_phase_map(user_guide_text: str) -> Dict[str, Set[str]]:
    phase_map: Dict[str, Set[str]] = {}
    in_table = False

    for line in user_guide_text.splitlines():
        if "| Module | Phase | Description |" in line:
            in_table = True
            continue
        if not in_table:
            continue
        if not line.startswith("|"):
            if phase_map:
                break
            continue
        if line.startswith("|--------"):
            continue
        if not line.startswith("| `sage_"):
            continue

        cols = [col.strip() for col in line.split("|")[1:-1]]
        if len(cols) < 3:
            continue

        module = cols[0].strip("`")
        phase_tokens = [tok.strip() for tok in cols[1].split(",")]
        phases = {tok for tok in phase_tokens if tok}
        phase_map[module] = phases

    return phase_map


def parse_registry_modules(registry_text: str) -> Set[str]:
    return set(
        re.findall(
            r"^const enum ProcessingMode (sage_[a-z0-9_]+)_supported_modes\[\]\s*=",
            registry_text,
            flags=re.M,
        )
    )


def validate_module_phase_consistency() -> List[str]:
    errors: List[str] = []

    user_guide_text = USER_GUIDE.read_text(encoding="utf-8")
    input_text = INPUT_CONFIG.read_text(encoding="utf-8")
    registry_text = MODULE_REGISTRY.read_text(encoding="utf-8")

    table_map = parse_user_guide_table_phase_map(user_guide_text)
    example_map = parse_user_guide_example_phase_map(user_guide_text)
    input_map = parse_module_phase_map(input_text)
    registry_modules = parse_registry_modules(registry_text)

    if not table_map:
        errors.append("docs/USER-GUIDE.md: failed to parse 'Available SAGE modules' table")
    if not example_map:
        errors.append("docs/USER-GUIDE.md: failed to parse SAGE 'Example configuration' YAML block")
    if not input_map:
        errors.append("input/millennium.yaml: failed to parse modules section")
    if not registry_modules:
        errors.append("src/modules/_system/generated/module_init.c: failed to parse registered SAGE modules")

    if errors:
        return errors

    for phase_source, phase_map in (
        ("docs table", table_map),
        ("docs example", example_map),
        ("input config", input_map),
    ):
        invalid = {
            module: sorted(phases - VALID_PHASES)
            for module, phases in phase_map.items()
            if phases - VALID_PHASES
        }
        if invalid:
            errors.append(f"{phase_source}: invalid phase labels found: {invalid}")

    for module in sorted(table_map):
        if module not in registry_modules:
            errors.append(
                f"docs/USER-GUIDE.md: module '{module}' listed in table but not present in generated registry"
            )

    for module in sorted(example_map):
        if module not in registry_modules:
            errors.append(
                f"docs/USER-GUIDE.md: module '{module}' listed in example YAML but not present in generated registry"
            )

    for module in sorted(table_map):
        table_phases = table_map[module]
        input_phases = input_map.get(module)
        example_phases = example_map.get(module)

        if input_phases is None:
            errors.append(
                f"docs/USER-GUIDE.md: module '{module}' listed in table but absent from input/millennium.yaml"
            )
            continue
        if example_phases is None:
            errors.append(
                f"docs/USER-GUIDE.md: module '{module}' listed in table but absent from USER-GUIDE example YAML"
            )
            continue

        if table_phases != input_phases:
            errors.append(
                f"phase mismatch for '{module}' between table and input/millennium.yaml: "
                f"table={sorted(table_phases)} input={sorted(input_phases)}"
            )
        if table_phases != example_phases:
            errors.append(
                f"phase mismatch for '{module}' between table and USER-GUIDE example YAML: "
                f"table={sorted(table_phases)} example={sorted(example_phases)}"
            )

    # Ensure the docs example does not omit sage modules present in canonical input config.
    input_sage_map = {module: phases for module, phases in input_map.items() if module.startswith("sage_")}
    missing_from_table = sorted(set(input_sage_map) - set(table_map))
    missing_from_example = sorted(set(input_sage_map) - set(example_map))
    if missing_from_table:
        errors.append(
            "docs/USER-GUIDE.md: SAGE modules missing from table: " + ", ".join(missing_from_table)
        )
    if missing_from_example:
        errors.append(
            "docs/USER-GUIDE.md: SAGE modules missing from example YAML: " + ", ".join(missing_from_example)
        )

    return errors


def main() -> int:
    errors = []
    errors.extend(validate_internal_links())
    errors.extend(validate_module_phase_consistency())

    if errors:
        print("Documentation checks failed:")
        for err in errors:
            print(f"  - {err}")
        return 1

    print("Documentation checks passed:")
    print("  - Internal Markdown links/anchors resolve")
    print("  - USER-GUIDE SAGE module phases align with input config and generated registry")
    return 0


if __name__ == "__main__":
    sys.exit(main())
