`zpaq`
===

Pure in-memory [ZPAQ](http://mattmahoney.net/dc/zpaq.html) compression for Python — **up to 5.9× faster than the official `zpaq` CLI**, byte-exact CLI-interoperable, multi-threaded, prebuilt wheels for every modern Python on Windows / Linux / macOS, zero C++ toolchain or runtime dependencies to install.

```py
import zpaq

blob = zpaq.compress(b"hello world " * 1_000, level=3)   # bytes -> bytes
assert zpaq.decompress(blob) == b"hello world " * 1_000
```

By default `zpaq.compress(...)` auto-scales across all CPU cores (`threads=0`). If you want the absolute best compression ratio (around 0.5-3 percentage points better) at the cost of throughput, pass `threads=1` to keep the input as a single block:

```py
blob = zpaq.compress(big_data, level=5, threads=1)   # max ratio, single-thread
```

Why this exists
---

Every other ZPAQ binding on PyPI shells out to the `zpaq` executable, which forces temp files and a subprocess fork. This package is both:

- A real pybind11 binding around `libzpaq` (Matt Mahoney's underlying C++ library — the same one the official `zpaq` CLI is built on top of), wrapping abstract `Reader`/`Writer` adapters that read from and write to `bytes` objects with no filesystem detour.
- Distributed as **prebuilt wheels** for Windows, Linux, and macOS (including Apple Silicon) across Python 3.8 through 3.13. Installing it never compiles anything.

On Windows, the wheel statically links the C and C++ runtimes so users don't need any "Visual C++ Redistributable" installed — if Python runs, `zpaq` works.

Performance
---

Speedup vs the official `zpaq.exe -m5` (Ryzen-class 12-core x86_64, level 5):

| workload | `zpaq.exe -m5` | best `zpaq.compress` | speedup |
| --- | --- | --- | --- |
| 40 KB text | 0.16 s | 0.11 s | **1.5×** |
| 1 MB text | 2.13 s | 0.49 s (t=12) | **4.3×** |
| 10 MB text | 23.17 s | 3.91 s (t=12) | **5.9×** |
| 125 MB text | 183.77 s | 55.42 s (t=12) | **3.3×** |

Full benchmark by thread count below. CLI is the official `zpaq.exe` v7.15 invoked with `-m5` (the speeds shown include its `-t0` default of two worker threads). `mem(t=N)` is `zpaq.compress(data, level=5, threads=N)`. Times in seconds; ratio is bytes-reduced over original.

40 KB text:

| algo | compress | decompress | ratio % |
| --- | --- | --- | --- |
| `zpaq.exe -m5` | 0.16 s | 0.15 s | 71.5 % |
| `zpaq.compress(t=1)` | **0.11 s** | **0.11 s** | **73.4 %** |

1 MB text:

| algo | compress | decompress | ratio % |
| --- | --- | --- | --- |
| `zpaq.exe -m5` | 2.13 s | 2.16 s | 80.0 % |
| `zpaq.compress(t=1)` | 1.97 s | 2.03 s | 80.1 % |
| `zpaq.compress(t=4)` | 0.73 s | 2.27 s | 79.3 % |
| `zpaq.compress(t=12)` | **0.49 s** | 2.31 s | 77.6 % |

10 MB text:

| algo | compress | decompress | ratio % |
| --- | --- | --- | --- |
| `zpaq.exe -m5` | 23.17 s | 23.82 s | 84.2 % |
| `zpaq.compress(t=1)` | 21.07 s | 22.23 s | 84.2 % |
| `zpaq.compress(t=4)` | 6.83 s | 21.17 s | 82.8 % |
| `zpaq.compress(t=12)` | **3.91 s** | 21.37 s | 81.2 % |

125 MB text:

| algo | compress | decompress | ratio % |
| --- | --- | --- | --- |
| `zpaq.exe -m5` | 183.77 s | 187.99 s | 86.7 % |
| `zpaq.compress(t=4)` | 101.40 s | 296.47 s | 85.8 % |
| `zpaq.compress(t=8)` | 66.03 s | 297.45 s | 85.0 % |
| `zpaq.compress(t=12)` | **55.42 s** | 289.96 s | 84.5 % |

**How the speedup is achieved.** `libzpaq`'s reference compiler emits an interpreter for the per-byte context-mixing predictor at compression levels 3-5. The official `zpaq.exe` on x86_64 ships with that interpreter replaced by a JIT that translates the predictor bytecode into native machine code at archive-open time. This package's x86_64 wheels enable the same JIT path **plus**:

- multi-threaded block compression via `threads=N` (the official CLI tops out at 2 cores by default)
- skip-checksum-by-default (`verify=False`), since pure-data workflows rarely need the SHA-1 per block that `zpaq.exe` always computes
- a libsais-backed suffix array constructor for level-3 BWT mode (Apache 2.0, several times faster than `libzpaq`'s vendored libdivsufsort-lite)
- a faster decompress path for archives produced by `zpaq.compress` (avoids the JIDAC-aware per-segment buffering)

Compress scales nearly linearly with thread count up to ~12 cores. Compression ratio drops slightly as threads increase (more block boundaries reduce per-block context size); the ratio for `t=1` matches or beats the CLI on every workload.

Decompress is currently single-threaded for archives we produce — `libzpaq`'s decompress API doesn't expose a per-block worker model, and parallelizing it cleanly is on the v0.2 roadmap. On small/medium files decompress is competitive with or faster than the CLI; on the 125 MB sample the CLI's threaded extract pulls ahead.

ARM / Apple Silicon wheels disable the x86-only JIT but still benefit from threading, libsais, and the fast decompress path.

API
---

```py
zpaq.compress(
    data,                    # bytes-like
    level=5,                 # 0..5 (0=store, 5=strongest)
    threads=0,               # 0 (default) = auto-detect host CPU count, clamped
                             # by input size (64KB minimum chunk per worker).
                             # 1 = single-thread, deterministic, best ratio.
                             # N>1 = pin to exactly N workers.
    hints=False,             # If True, scan input for text/exe signatures and order-1
                             # redundancy, pass them to libzpaq via the method string.
                             # Slight overhead, helps ratio on some mixed/binary data.
    verify=False,            # If True, compute & embed SHA-1 per segment. zpaq.exe
                             # also writes these by default; turning them off makes
                             # both this package and zpaq.exe skip verification on
                             # extract, which is faster but won't catch corruption.
    method=None,             # Optional raw libzpaq method-string override (e.g. "x4,4,1"
                             # for custom predictor specs). Overrides level/hints when set.
) -> bytes

zpaq.decompress(
    data,                    # bytes-like ZPAQ stream
    verify=False,            # If True, recompute SHA-1 of each segment and compare to
                             # the one stored in the archive. Raises zpaq.Error on
                             # mismatch. Default off for speed.
) -> bytes

zpaq.Error                   # Raised on libzpaq failures (corrupt stream, bad header, etc.)
```

Both `compress` and `decompress` release the GIL while libzpaq runs, so `zpaq` plays well with threaded workloads.

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
    f.write(zpaq.compress(my_bytes, level=5))
# ...later, from any machine with the zpaq executable installed:
#   $ zpaq x out.zpaq

# Read an archive that someone else produced with `zpaq a`
with open("their.zpaq", "rb") as f:
    file_bytes = zpaq.decompress(f.read())
```

When `zpaq.decompress` is fed a multi-file archive it returns the concatenated bytes of every file in the order the CLI stored them. A future release will expose a per-segment iterator API so individual files can be addressed by name.

Future work
---

The current release leaves a few performance levers untouched; pull requests welcome:

- **Profile-guided optimization (PGO).** Adding `/GENPROFILE` + `/USEPROFILE` to the MSVC build (and equivalents on gcc/clang) typically gains another 5-15%. Skipped here because cibuildwheel doesn't expose a clean two-stage build hook yet.
- **AVX2 SIMD.** `libzpaq`'s predictor inner loop is small and serial; adding hand-written SIMD would require a deeper rewrite than a one-pass speedup.
- **Parallel decompress.** `libzpaq`'s decompress API is currently single-threaded; a block-parallel decompressor would close the remaining gap on large archives.
- **Per-segment archive API.** `zpaq.decompress` currently returns the concatenated bytes of every segment in a multi-file `zpaq a` archive. A future iterator API would let callers address individual files by name.

License
---

This package is released under the same terms as the underlying `libzpaq` sources: public domain. See `src/zpaq/vendor/COPYING`.

The vendored libsais suffix array library is Apache 2.0 (Ilya Grebnov). See `src/zpaq/vendor/LICENSE-libsais`.

---

Not affiliated with Matt Mahoney. `libzpaq` was released into the public domain by its original author; this Python package wraps those sources and is an independent community project.
