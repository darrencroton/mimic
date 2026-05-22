#!/usr/bin/env python3
"""
Documentation quality checks for Mimic.

Checks:
1. Internal Markdown links and anchors resolve.
2. Review-only PONDER markers are absent from committed documentation.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {
    ".git",
    ".github",
    ".claude",
    "archive",
    "build",
    "mimic_venv",
    "sage-code",
}

SKIP_PATH_PREFIXES = {
    ("tests", "data", "output"),
}


def discover_markdown_files() -> List[Path]:
    """Return repository Markdown files that should be checked."""
    files: List[Path] = []
    for path in REPO_ROOT.rglob("*.md"):
        rel_parts = path.relative_to(REPO_ROOT).parts
        if any(part in SKIP_DIRS for part in rel_parts):
            continue
        if any(rel_parts[: len(prefix)] == prefix for prefix in SKIP_PATH_PREFIXES):
            continue
        if not path.exists() or not path.is_file():
            continue
        files.append(path)
    return sorted(files)


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

    for doc_file in discover_markdown_files():
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


def validate_no_ponder_markers() -> List[str]:
    """Reject unresolved inline review markers in Markdown documentation."""
    errors: List[str] = []
    pattern = re.compile(r"\[ponder\s*:", re.IGNORECASE)
    for doc_file in discover_markdown_files():
        content = doc_file.read_text(encoding="utf-8")
        for lineno, line in enumerate(content.splitlines(), start=1):
            if pattern.search(line):
                errors.append(
                    f"{doc_file.relative_to(REPO_ROOT)}:{lineno}: unresolved PONDER marker"
                )
    return errors


def main() -> int:
    errors = []
    errors.extend(validate_internal_links())
    errors.extend(validate_no_ponder_markers())

    if errors:
        print("Documentation checks failed:")
        for err in errors:
            print(f"  - {err}")
        return 1

    print("Documentation checks passed:")
    print("  - Internal Markdown links/anchors resolve")
    print("  - No unresolved PONDER markers")
    return 0


if __name__ == "__main__":
    sys.exit(main())
