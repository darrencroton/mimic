"""The forest census: how a horizontal-HDF5 dataset's halos are distributed over forests and
effective descendant trees, the decided cut table that splits forests at their z = 0 FoF groups,
and the driver's task and chunk partition simulated on both. Each subcommand module
(``occupancy``, ``trees``, ``partition``, ``table``, ``cut``) reads bounded blocks of the dataset
and writes its results into a census aggregate directory (``aggregate``); ``cut_table`` owns the
cut table's naming, invariants and record, and ``common`` the small helpers they share. The
command line is ``forest_census.py``.
"""
