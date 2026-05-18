"""
zpaq - pure in-memory ZPAQ compression for Python.

Wraps libzpaq (Matt Mahoney, public domain) via pybind11 with custom
byte-vector-backed Reader/Writer adapters. No temp files, no subprocess
calls. Ships prebuilt wheels for Windows, Linux and macOS so installation
never requires a C++ toolchain.

Public API:
    compress(data, level=5, threads=0, hints=False, verify=False, method=None,
             dedup=False)                                                       -> bytes
    decompress(data, verify=False, threads=0)                                   -> bytes
    Error                                                                       -> raised on any libzpaq failure

`level` is an integer in 0..5. 0 stores without compression; 5 is the
slowest / strongest setting. The default of 5 matches the `zpaq` CLI
default for `-method 5`.

Example:
    >>> import zpaq
    >>> blob = zpaq.compress(b"hello " * 1000, level=3)
    >>> zpaq.decompress(blob) == b"hello " * 1000
    True
"""
from ._zpaq import compress, decompress, Error

__all__ = ["compress", "decompress", "Error"]
__version__ = "0.0.1"
