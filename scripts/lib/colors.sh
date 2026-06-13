#!/bin/bash
# scripts/lib/colors.sh — Shared ANSI colour codes for Mimic shell scripts.
#
# Source this file from scripts that print coloured output; do not execute it.
# After sourcing, RED / GREEN / YELLOW / BLUE / NC are available for use with
# `echo -e`.
#
# Colours are emitted only when stdout is a TTY and NO_COLOR is unset, so piped
# or CI output stays free of escape codes. The single source of truth for the
# shell side, mirroring scripts/console.py for the Python side.

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[1;34m'
    NC='\033[0m' # No Color
else
    RED='' GREEN='' YELLOW='' BLUE='' NC=''
fi
