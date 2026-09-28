"""The converter's single fatal error type.

A leaf module that imports nothing from the converter, so every converter
module can raise and catch the same exception without importing a sibling that
may import it back.
"""

__all__ = ["ConverterError"]


class ConverterError(RuntimeError):
    """Fatal converter failure: the run must abort, never repair silently."""
