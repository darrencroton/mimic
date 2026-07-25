#!/bin/bash
###############################################################################
# hdf5.sh - Shared HDF5 library detection for test/tool build scripts
#
# Sourced by tests/unit/run_tests.sh and tests/unit/tools/build_topology_dump.sh
# so both discover HDF5 identically. Keeping a single copy stops the two build
# scripts from drifting apart when a new platform layout (distro path, brew
# prefix, MPI-HDF5 variant) is added.
#
# Usage:
#   . "${REPO_ROOT}/scripts/lib/hdf5.sh"
#   detect_hdf5
#   # then read $HDF5_AVAILABLE (0/1), $HDF5_CFLAGS, $HDF5_LDFLAGS
#
# Note: the project Makefile performs the same probe sequence in GNU Make
# syntax (the HDF5 block in Makefile) — pkg-config, then a Homebrew prefix whose
# include/hdf5.h exists, then the three system paths. That copy cannot source
# this shell helper and is maintained in parallel by necessity; keep the probe
# order and the acceptance conditions in sync when either changes. The two
# differ only in what they emit: the Makefile appends straight to CFLAGS/
# LDFLAGS/LIBS and hard-errors when nothing is found, while this helper returns
# HDF5_AVAILABLE=0 so a caller can build a reduced source set instead.
###############################################################################

# Detect HDF5 and set HDF5_AVAILABLE (0/1), HDF5_CFLAGS, HDF5_LDFLAGS in the
# caller's shell scope.
detect_hdf5() {
    HDF5_AVAILABLE=0
    HDF5_CFLAGS=""
    HDF5_LDFLAGS=""
    if pkg-config --exists hdf5 2>/dev/null; then
        HDF5_AVAILABLE=1
        HDF5_CFLAGS="$(pkg-config --cflags hdf5 2>/dev/null)"
        HDF5_LDFLAGS="$(pkg-config --libs-only-L hdf5 2>/dev/null) -lhdf5_hl $(pkg-config --libs-only-l hdf5 2>/dev/null)"
    else
        BREW_HDF5="$(command -v brew >/dev/null 2>&1 && brew --prefix hdf5 2>/dev/null || true)"
        if [ -n "$BREW_HDF5" ] && [ -f "$BREW_HDF5/include/hdf5.h" ]; then
            HDF5_AVAILABLE=1
            HDF5_CFLAGS="-I${BREW_HDF5}/include"
            HDF5_LDFLAGS="-L${BREW_HDF5}/lib -lhdf5_hl -lhdf5"
        elif [ -f /usr/include/hdf5.h ]; then
            HDF5_AVAILABLE=1
            HDF5_LDFLAGS="-lhdf5_hl -lhdf5"
        elif [ -f /usr/include/hdf5/serial/hdf5.h ]; then
            HDF5_AVAILABLE=1
            HDF5_CFLAGS="-I/usr/include/hdf5/serial"
            HDF5_LDFLAGS="-L/usr/lib/x86_64-linux-gnu/hdf5/serial -lhdf5_hl -lhdf5"
        elif [ -f /usr/local/include/hdf5.h ]; then
            HDF5_AVAILABLE=1
            HDF5_CFLAGS="-I/usr/local/include"
            HDF5_LDFLAGS="-L/usr/local/lib -lhdf5_hl -lhdf5"
        fi
    fi
}
