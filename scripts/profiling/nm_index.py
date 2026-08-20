#!/usr/bin/env python3
"""Build symbol -> defining source file index from the mimic object tree.

    nm_index.py > nm_index.json

build/obj mirrors the src/ and models/ layout, so the object path maps back to a
source path by swapping build/obj -> "" and .o -> .c.  Used to attribute frames
that carry no source annotation but live in ./mimic.
"""

import json
import os
import subprocess

REPO = os.environ.get(
    "MIMIC_REPO", os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)
idx = {}
for root, _dirs, files in os.walk(os.path.join(REPO, "build/obj")):
    for f in files:
        if not f.endswith(".o"):
            continue
        obj = os.path.join(root, f)
        src = os.path.relpath(obj, os.path.join(REPO, "build/obj"))[:-2] + ".c"
        out = subprocess.run(["nm", "-Uj", obj], capture_output=True, text=True).stdout
        for sym in out.split():
            idx.setdefault(sym.lstrip("_"), src)
print(json.dumps(idx, indent=0))
