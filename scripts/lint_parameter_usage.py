#!/usr/bin/env python3
"""
Parameter Usage Linter for Mimic

Verifies that parameter declarations in module_info.yaml match actual usage in module code.

Checks:
1. All parameters declared in dependencies.parameters are actually used in code
2. All parameters used in code are declared in dependencies.parameters
3. No orphaned or undeclared parameters

Usage:
    python3 scripts/lint_parameter_usage.py           # Check all modules
    python3 scripts/lint_parameter_usage.py --verbose # Verbose output
    python3 scripts/lint_parameter_usage.py --module sage_cooling # Check specific module

Exit codes:
    0 - All checks passed
    1 - Found undeclared parameters (ERROR)
    2 - Found unused parameters (WARNING)

Author: Mimic Development Team
Date: 2025-12-02
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, Set, Tuple

try:
    import yaml
except ImportError:
    print("ERROR: PyYAML not installed. Run: pip install PyYAML", file=sys.stderr)
    sys.exit(1)

# ANSI color codes
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
GREEN = '\033[0;32m'
BLUE = '\033[1;34m'
NC = '\033[0m'  # No Color

# Repository root
REPO_ROOT = Path(__file__).parent.parent
MODULES_DIR = REPO_ROOT / "src" / "modules"


def find_parameter_calls(c_file: Path) -> Dict[str, str]:
    """
    Find all model_get_* calls in a C file.

    Returns dict mapping parameter name to type (double, int, string).
    """
    if not c_file.exists():
        return {}

    with open(c_file) as f:
        content = f.read()

    # Pattern: model_get_TYPE("PARAM_NAME"
    pattern = r'model_get_(double|int|string)\s*\(\s*"(\w+)"'

    params = {}
    for match in re.finditer(pattern, content):
        param_type = match.group(1)
        param_name = match.group(2)
        params[param_name] = param_type

    return params


def load_module_info(module_dir: Path) -> Dict:
    """Load module_info.yaml for a module."""
    info_file = module_dir / "module_info.yaml"
    if not info_file.exists():
        return {}

    with open(info_file) as f:
        return yaml.safe_load(f)


def get_declared_parameters(module_info: Dict) -> Set[str]:
    """Get set of parameters declared in dependencies.parameters."""
    # Support both root-level and module.dependencies structure
    module_dict = module_info.get('module', module_info)
    deps = module_dict.get('dependencies', {})
    params = deps.get('parameters', [])

    # Handle both list format and dict format
    if params and isinstance(params[0], str):
        # Simple list format: ['ParamName1', 'ParamName2']
        return set(params)
    else:
        # Dict format: [{'name': 'ParamName1'}, ...]
        return {p.get('name', p) if isinstance(p, dict) else p for p in params}


def check_module(module_dir: Path, verbose: bool = False) -> Tuple[List[str], List[str]]:
    """
    Check parameter usage for one module.

    Returns (errors, warnings) where:
      errors = list of undeclared parameters (used but not declared)
      warnings = list of unused parameters (declared but not used)
    """
    module_name = module_dir.name
    errors = []
    warnings = []

    # Load module metadata
    module_info = load_module_info(module_dir)
    if not module_info:
        if verbose:
            print(f"  {BLUE}Skipping {module_name} (no module_info.yaml){NC}")
        return errors, warnings

    # Get declared parameters
    declared_params = get_declared_parameters(module_info)

    # Find all .c files in module
    used_params = {}
    for c_file in module_dir.glob("*.c"):
        if c_file.name.startswith("test_"):
            continue  # Skip test files
        used_params.update(find_parameter_calls(c_file))

    # Check for issues
    undeclared = set(used_params.keys()) - declared_params
    unused = declared_params - set(used_params.keys())

    # Report undeclared parameters (ERROR - breaks contract)
    if undeclared:
        for param in sorted(undeclared):
            errors.append(
                f"{RED}ERROR{NC}: {module_name} uses '{param}' "
                f"but doesn't declare it in dependencies.parameters"
            )

    # Report unused parameters (WARNING - documentation drift)
    if unused:
        for param in sorted(unused):
            warnings.append(
                f"{YELLOW}WARNING{NC}: {module_name} declares '{param}' "
                f"in dependencies.parameters but never uses it"
            )

    # Success message if all clean
    if not undeclared and not unused and verbose:
        print(f"  {GREEN}✓{NC} {module_name}: {len(declared_params)} parameters OK")

    return errors, warnings


def main():
    parser = argparse.ArgumentParser(
        description="Lint parameter usage across Mimic modules"
    )
    parser.add_argument(
        '--module',
        help='Check only this specific module'
    )
    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help='Verbose output'
    )
    args = parser.parse_args()

    print(f"{BLUE}{'='*60}{NC}")
    print(f"{BLUE}Mimic Parameter Usage Linter{NC}")
    print(f"{BLUE}{'='*60}{NC}")

    all_errors = []
    all_warnings = []

    # Determine which modules to check
    if args.module:
        module_dirs = [MODULES_DIR / args.module]
        if not module_dirs[0].exists():
            print(f"{RED}ERROR: Module directory not found: {module_dirs[0]}{NC}")
            return 1
    else:
        # Check all modules except _system and _archive
        module_dirs = [
            d for d in MODULES_DIR.iterdir()
            if d.is_dir() and not d.name.startswith('_')
        ]

    # Check each module
    print(f"Checking {len(module_dirs)} module(s) for parameter usage consistency...")
    for module_dir in sorted(module_dirs):
        module_name = module_dir.name
        if not args.verbose:
            print(f"  • {module_name}", end="", flush=True)
        errors, warnings = check_module(module_dir, args.verbose)
        all_errors.extend(errors)
        all_warnings.extend(warnings)
        if not args.verbose:
            if errors or warnings:
                print(f" {YELLOW}✗{NC}")
            else:
                print(f" {GREEN}✓{NC}")

    # Print results
    print()
    if all_errors:
        print(f"{RED}Errors found:{NC}")
        for error in all_errors:
            print(f"  {error}")
        print()

    if all_warnings:
        print(f"{YELLOW}Warnings found:{NC}")
        for warning in all_warnings:
            print(f"  {warning}")
        print()

    # Summary
    print(f"{BLUE}{'='*60}{NC}")
    print(f"Modules checked: {len(module_dirs)}")
    print(f"Errors:  {RED if all_errors else GREEN}{len(all_errors)}{NC}")
    print(f"Warnings: {YELLOW if all_warnings else GREEN}{len(all_warnings)}{NC}")
    print(f"{BLUE}{'='*60}{NC}")

    # Exit code
    if all_errors:
        print(f"\n{RED}✗ Parameter usage validation FAILED{NC}")
        return 1
    elif all_warnings:
        print(f"\n{YELLOW}⚠ Validation PASSED with warnings{NC}")
        return 2
    else:
        print(f"\n{GREEN}✓ Validation PASSED{NC}")
        return 0


if __name__ == '__main__':
    sys.exit(main())
