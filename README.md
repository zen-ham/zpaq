`zpaq`
===

Pure in-memory [ZPAQ](http://mattmahoney.net/dc/zpaq.html) compression for Python.

Every other `pip install`-able ZPAQ package on PyPI either shells out to the `zpaq` executable (which forces temp files) or ships only an sdist (which forces every user to have a working C++ toolchain). This package is both:

- A real pybind11 binding around `libzpaq` (the same library the official `zpaq` CLI uses), wrapping abstract `Reader`/`Writer` adapters that read from and write to `bytes` objects with no filesystem detour.
- Distributed as **prebuilt wheels** for Windows, Linux and macOS across modern Python versions. Installing it never compiles anything.

On Windows, the wheel statically links the C and C++ runtimes so users do not need to install any "Visual C++ Redistributable" package - if Python runs, `zpaq` works.

```py
import zpaq

raw = b"hello world " * 1_000
blob = zpaq.compress(raw, level=3)   # bytes -> bytes
assert zpaq.decompress(blob) == raw
```

Compatibility with the `zpaq` CLI
---

`zpaq.compress()` emits the same on-disk format `libzpaq` itself writes, and `zpaq.decompress()` understands archives produced by the `zpaq a` journaling archiver (it identifies the JIDAC index/hash/info segments, discards them, and strips each data segment's trailing fragment-size footer so the recovered bytes match the original file exactly).

Tested on ten varied real files (1 KB to 25 MB, text/image/csv/jar/png/jpg/svg/exe/binary, compression levels 1-5):

| Direction | Result |
| --- | --- |
| `zpaq.compress` → `zpaq.decompress` | 10 / 10 byte-exact |
| `zpaq.compress` → official `zpaq x` CLI | 10 / 10 byte-exact |
| official `zpaq a` CLI → `zpaq.decompress` | 10 / 10 byte-exact |

```py
import zpaq

# Pipe to the official CLI
with open("out.zpaq", "wb") as f:
    f.write(zpaq.compress(my_bytes, level=3))
# ...later, from any machine with the zpaq executable installed:
#   $ zpaq x out.zpaq

# Read an archive that someone else produced with `zpaq a`
with open("their.zpaq", "rb") as f:
    file_bytes = zpaq.decompress(f.read())
```

When `zpaq.decompress` is fed a multi-file archive it returns the concatenated bytes of every file in the order the CLI stored them. A future release will expose a per-segment iterator API so individual files can be addressed by name.

API
---

| Function | Description |
| --- | --- |
| `zpaq.compress(data, level=5)` | Compress a `bytes`-like object. `level` is 0..5 (0 = store, 5 = strongest). Returns `bytes`. |
| `zpaq.decompress(data)` | Decompress a `bytes`-like ZPAQ stream. Returns `bytes`. |
| `zpaq.Error` | Raised on any libzpaq failure (corrupt stream, bad header, ...). |

Both calls release the GIL while libzpaq runs, so `zpaq` is friendly to threaded workloads.

License
---

This package is released under the same terms as the underlying libzpaq sources: public domain. See `src/zpaq/vendor/COPYING`.
