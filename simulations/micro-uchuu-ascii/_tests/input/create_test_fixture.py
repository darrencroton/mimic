#!/usr/bin/env python3
"""Create the tiny micro-Uchuu ASCII reader fixture used by integration tests.

Three forests, four halos total.  Forest 1001 has two halos (snap49 root plus
a snap48 progenitor) so the reader exercises a real multi-snapshot traversal.
Forests 1002 and 1003 each have a single snap49 halo.  With snapshot_list=[49]
the test expects 3 output halos at z=0.

The script writes three files to _tests/data/:
  tree_0_0_0.dat  — ASCII Consistent-Trees tree file
  forests.list    — forest/tree-root mapping
  locations.dat   — tree byte-offsets within the tree file
"""

from __future__ import annotations

import io
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DATA_DIR = ROOT / "data"
TREE_FILE = DATA_DIR / "tree_0_0_0.dat"
FORESTS_FILE = DATA_DIR / "forests.list"
LOCATIONS_FILE = DATA_DIR / "locations.dat"

# Scale factors from micro-uchuu.a_list: snap48 = second-to-last, snap49 = last
SCALE_48 = 0.97808
SCALE_49 = 0.99951

# Minimal column header that covers all fields the reader extracts.
# Positions 0-36 are explicitly numbered; 37-40 are sequential unnamed columns.
# The reader matches column names case-insensitively by position in this header.
HEADER = (
    "#scale(0) id(1) desc_scale(2) desc_id(3) num_prog(4) pid(5) upid(6) desc_pid(7) "
    "phantom(8) sam_Mvir(9) Mvir(10) Rvir(11) rs(12) vrms(13) mmp?(14) "
    "scale_of_last_MM(15) vmax(16) x(17) y(18) z(19) vx(20) vy(21) vz(22) "
    "Jx(23) Jy(24) Jz(25) Spin(26) Breadth_first_ID(27) Depth_first_ID(28) "
    "Tree_root_ID(29) Orig_halo_ID(30) Snap_num(31) "
    "Next_coprogenitor_depthfirst_ID(32) Last_progenitor_depthfirst_ID(33) "
    "Last_mainleaf_depthfirst_ID(34) Tidal_Force(35) Tidal_ID(36) "
    "Rs_Klypin Mvir_all M200b M200c\n"
)

# Number of tokens per data line — must match HEADER's column count (41).
NCOLS = 41


def _row(
    scale: float,
    halo_id: int,
    desc_scale: float,
    desc_id: int,
    num_prog: int,
    pid: int,
    upid: int,
    mvir: float,
    vrms: float,
    vmax: float,
    x: float,
    y: float,
    z: float,
    vx: float,
    vy: float,
    vz: float,
    jx: float,
    jy: float,
    jz: float,
    snap_num: int,
    tree_root_id: int,
    last_prog_id: int,
) -> str:
    """Return one space-delimited data row (NCOLS tokens, newline-terminated)."""
    m200b = mvir * 1.1
    m200c = mvir * 0.9
    cols = [
        scale,  # 0  scale
        halo_id,  # 1  id
        desc_scale,  # 2  desc_scale
        desc_id,  # 3  desc_id
        num_prog,  # 4  num_prog
        pid,  # 5  pid
        upid,  # 6  upid
        -1,  # 7  desc_pid
        0,  # 8  phantom
        mvir,  # 9  sam_Mvir
        mvir,  # 10 Mvir
        38.0,  # 11 Rvir
        7.0,  # 12 rs
        vrms,  # 13 vrms (VelDisp)
        1,  # 14 mmp?
        0.8,  # 15 scale_of_last_MM
        vmax,  # 16 vmax
        x,  # 17 x
        y,  # 18 y
        z,  # 19 z
        vx,  # 20 vx
        vy,  # 21 vy
        vz,  # 22 vz
        jx,  # 23 Jx (→ spin[0]*mvir before norm; reader divides by mvir)
        jy,  # 24 Jy
        jz,  # 25 Jz
        0.03,  # 26 Spin
        halo_id,  # 27 Breadth_first_ID
        halo_id,  # 28 Depth_first_ID
        tree_root_id,  # 29 Tree_root_ID
        halo_id,  # 30 Orig_halo_ID
        snap_num,  # 31 Snap_num
        -1,  # 32 Next_coprogenitor_depthfirst_ID
        last_prog_id,  # 33 Last_progenitor_depthfirst_ID
        last_prog_id,  # 34 Last_mainleaf_depthfirst_ID
        0.02,  # 35 Tidal_Force
        -1,  # 36 Tidal_ID
        7.0,  # 37 Rs_Klypin
        mvir,  # 38 Mvir_all
        m200b,  # 39 M200b
        m200c,  # 40 M200c
    ]
    assert len(cols) == NCOLS, f"expected {NCOLS} cols, got {len(cols)}"
    return " ".join(str(c) for c in cols) + "\n"


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)

    buf = io.BytesIO()

    # Header and cosmology comment lines.
    buf.write(HEADER.encode())
    buf.write(b"#Omega_M = 0.308900; Omega_L = 0.691100; h0 = 0.677400\n")
    buf.write(b"#Full box size = 100.000000 Mpc/h\n")

    # tree_offsets[root_id] = byte offset of the FIRST DATA LINE after the
    # #tree marker.  The reader calls pread(fd, buf, len, offset) and checks
    # buf[0] == '#' to stop, so the offset must skip the #tree header line.
    tree_offsets: dict[int, int] = {}

    def write_tree(root_id: int, rows: list[str]) -> None:
        buf.write(f"#tree {root_id}\n".encode())
        tree_offsets[root_id] = buf.tell()
        for row in rows:
            buf.write(row.encode())

    # --- Forest 1001 (root 1000001): snap49 central + snap48 progenitor ---
    write_tree(
        1000001,
        [
            _row(
                SCALE_49,
                1000001,
                0.0,
                -1,
                1,
                -1,
                -1,
                5.0e10,
                80.0,
                120.0,
                5.0,
                6.0,
                7.0,
                10.0,
                20.0,
                30.0,
                0.0,
                0.0,
                0.0,
                49,
                1000001,
                1000011,
            ),
            _row(
                SCALE_48,
                1000011,
                SCALE_49,
                1000001,
                0,
                -1,
                -1,
                4.5e10,
                75.0,
                115.0,
                5.1,
                6.1,
                7.1,
                11.0,
                21.0,
                31.0,
                0.0,
                0.0,
                0.0,
                48,
                1000001,
                1000011,
            ),
        ],
    )

    # --- Forest 1002 (root 1000002): snap49 only ---
    write_tree(
        1000002,
        [
            _row(
                SCALE_49,
                1000002,
                0.0,
                -1,
                0,
                -1,
                -1,
                3.0e10,
                60.0,
                100.0,
                15.0,
                16.0,
                17.0,
                12.0,
                22.0,
                32.0,
                0.0,
                0.0,
                0.0,
                49,
                1000002,
                1000002,
            ),
        ],
    )

    # --- Forest 1003 (root 1000003): snap49 only ---
    write_tree(
        1000003,
        [
            _row(
                SCALE_49,
                1000003,
                0.0,
                -1,
                0,
                -1,
                -1,
                2.0e10,
                50.0,
                90.0,
                25.0,
                26.0,
                27.0,
                13.0,
                23.0,
                33.0,
                0.0,
                0.0,
                0.0,
                49,
                1000003,
                1000003,
            ),
        ],
    )

    TREE_FILE.write_bytes(buf.getvalue())

    # forests.list: TreeRootID ForestID (one tree per forest in this fixture)
    forests_lines = "#TreeRootID ForestID\n"
    for root_id, forest_id in [(1000001, 1001), (1000002, 1002), (1000003, 1003)]:
        forests_lines += f"{root_id} {forest_id}\n"
    FORESTS_FILE.write_text(forests_lines)

    # locations.dat: TreeRootID FileID Offset Filename
    locations_lines = "#TreeRootID FileID Offset Filename\n"
    for root_id in [1000001, 1000002, 1000003]:
        locations_lines += f"{root_id} 0 {tree_offsets[root_id]} tree_0_0_0.dat\n"
    LOCATIONS_FILE.write_text(locations_lines)

    print(f"Created {TREE_FILE}")
    print(f"Created {FORESTS_FILE}")
    print(f"Created {LOCATIONS_FILE}")


if __name__ == "__main__":
    main()
