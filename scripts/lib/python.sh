#!/bin/bash
# Resolve the Python interpreter for repo scripts, mirroring the Makefile's
# PYTHON: activated virtualenv, then mimic_venv (pinned deps), then system.
# REPO_ROOT must be set before sourcing.

if [ -n "${VIRTUAL_ENV:-}" ] && [ -x "${VIRTUAL_ENV}/bin/python3" ]; then
    MIMIC_PYTHON="${VIRTUAL_ENV}/bin/python3"
elif [ -x "${REPO_ROOT}/mimic_venv/bin/python3" ]; then
    MIMIC_PYTHON="${REPO_ROOT}/mimic_venv/bin/python3"
else
    MIMIC_PYTHON="python3"
fi
export MIMIC_PYTHON
