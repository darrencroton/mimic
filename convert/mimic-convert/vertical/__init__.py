"""The vertical converter: a forest-blocked version 3 horizontal-HDF5 dataset written as L-Halo
binary files in the record a simulation package declares.

``record`` derives that record from the package's ordered ``halo_properties.yaml`` exactly as the
C compiler lays out the generated ``struct RawHalo``; ``plan`` derives every forest's halo total
from the dataset's slabs, cuts the output files into contiguous forest ranges and reads and writes
the ``file_plan.json`` that binds a conversion to its dataset and record; ``assemble`` streams
a dataset's slabs in bounded windows and yields each forest of a ``ForestIndex`` range as one
record array in rank order, its links forest-local and verified.
"""
