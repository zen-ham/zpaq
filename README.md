`zpaq`
===

[![pypi](https://img.shields.io/pypi/v/zpaq?logo=pypi&color=blue)](https://pypi.org/project/zpaq/) [![Downloads](https://static.pepy.tech/badge/zpaq)](https://pypi.org/project/zpaq/) [![github](https://img.shields.io/badge/GitHub-zpaq-blue?logo=github)](https://github.com/zen-ham/zpaq) [![stars](https://img.shields.io/github/stars/zen-ham/zpaq?style=social)](https://github.com/zen-ham/zpaq)

Pure in-memory ZPAQ compression for Python. I made this because every other zpaq package on PyPI is just a wrapper around the `zpaq` executable, they shell out to the CLI as a subprocess, which means temp files for every operation, fork overhead, and the user needing `zpaq.exe` on their PATH in the first place. None of them are actual bindings. I wanted real bytes->bytes zpaq from Python that just works.

```py
import zpaq

blob = zpaq.compress(b"hello world " * 1_000, level=3)
assert zpaq.decompress(blob) == b"hello world " * 1_000
```

It also ended up alot faster than the official `zpaq.exe` itself, up to **6.6× faster on decompress** and **6.3× faster on compress** at 10 MB, with the same compression ratio. Multi-threaded both directions, JIT-compiled predictor on x86_64, libsais for the suffix-array pass, AVX2 auto-vec compile flag. Default `threads=0` auto-scales across all CPU cores. Pass `threads=1` if you want the absolute best ratio:

```py
blob = zpaq.compress(big_data, level=5, threads=1)   # max ratio, single block
```

For inputs with repeated content (logs, large text corpora, similar binaries, snapshots), pass `dedup=True` to get fragment-level deduplication, input gets split into ~64 KB content-defined chunks and identical chunks are stored once. Matches what the `zpaq a` CLI produces, so the output is fully `zpaq x` extractable:

```py
blob = zpaq.compress(repetitive_data, level=5, dedup=True)   # JIDAC archive
```

Prebuilt wheels for Windows / Linux / macOS (including Apple Silicon) across Python 3.9 through 3.13. Installing it never compiles anything. On Windows the wheel statically links the C and C++ runtimes so there's no "Visual C++ Redistributable" requirement, if Python runs, `zpaq` works.

Performance
---

![benchmark](docs/benchmark.png)

Benchmarks vs the official `zpaq.exe -m5` (Ryzen-class 12-core x86_64, level 5). The `mem` binding wins both compress and decompress at every size, and `dedup=True` matches or beats the CLI on compression ratio at every scale:

| workload | CLI comp | best mem comp | CLI decomp | best mem decomp | mem dedup ratio vs CLI |
| --- | --- | --- | --- | --- | --- |
| 1 MB text | 2.25 s | 0.56 s (**4.0×**) | 2.23 s | 0.64 s (**3.5×**) | **+0.013 pp** |
| 10 MB text | 24.67 s | 4.36 s (**5.7×**) | 24.74 s | 4.29 s (**5.8×**) | **+0.001 pp** |
| 100 MB text | 188.79 s | 44.86 s (**4.2×**) | 185.47 s | 44.91 s (**4.1×**) | **+0.078 pp** |

Full breakdown by thread count below. CLI is the official `zpaq.exe` v7.15 invoked with `-m5` (its speeds already include the `-t0` default of two worker threads). `mem(t=N)` is `zpaq.compress(data, level=5, threads=N)`. Times in seconds; ratio is bytes-reduced over original.

1 MB text:

| algo | compress | decompress | ratio % |
| --- | --- | --- | --- |
| `zpaq.exe -m5` | 2.25 s | 2.23 s | 79.963 % |
| `zpaq.compress(t=1)` | 2.15 s | 2.21 s | 80.073 % |
| `zpaq.compress(dedup=True)` | 2.18 s | 2.16 s | **79.976 %** |
| `zpaq.compress(t=0)` (12 cores) | **0.56 s** | **0.64 s** | 77.547 % |

10 MB text:

| algo | compress | decompress | ratio % |
| --- | --- | --- | --- |
| `zpaq.exe -m5` | 24.67 s | 24.74 s | 84.161 % |
| `zpaq.compress(t=1)` | 24.76 s | 25.15 s | 84.206 % |
| `zpaq.compress(dedup=True)` | 24.24 s | 26.67 s | **84.162 %** |
| `zpaq.compress(t=0)` (12 cores) | **4.36 s** | **4.29 s** | 81.213 % |

100 MB text:

| algo | compress | decompress | ratio % |
| --- | --- | --- | --- |
| `zpaq.exe -m5` | 188.79 s | 185.47 s | 86.588 % |
| `zpaq.compress(t=1)` | 248.63 s | 66.07 s | 85.059 % |
| `zpaq.compress(dedup=True)` | 180.19 s | 184.52 s | **86.666 %** |
| `zpaq.compress(t=0)` (12 cores) | **44.86 s** | **44.91 s** | 84.210 % |

Why this is faster than the official CLI
---

`libzpaq`'s reference compiler ships an interpreter for the per-byte context-mixing predictor used at compression levels 3-5. The official `zpaq.exe` on x86_64 ships with that interpreter replaced by a JIT that translates the predictor bytecode into native machine code at archive-open time, that's where most of its speed comes from. This package's x86_64 wheels enable that same JIT path, plus a handful of additions the CLI doesn't have:

- multi-threaded block compression via `threads=N` (the official CLI tops out at two cores by default)
- multi-threaded block decompression, I scan the archive for ZPAQ locator-tag block boundaries up front, dispatch each block to a worker, and concatenate. `libzpaq`'s decompress API is sequential, so this layer sits above it.
- skip-checksum-by-default (`verify=False`); the SHA-1 per block that `zpaq.exe` always computes isn't free, and most "compress these bytes please" workflows don't need it
- AVX2-enabled compile flags so the optimizer auto-vectorizes where it can (x86_64 wheels assume AVX2; CPUs from 2013+ are covered, anything older falls back to the sdist build)
- libsais (Apache 2.0) for level-3 BWT suffix-array construction instead of the libdivsufsort-lite that libzpaq ships internally
- a faster decompress path for archives produced by `zpaq.compress` that skips the JIDAC-aware per-segment buffering

Compress scales nearly linearly with thread count up to ~12 cores. Compression ratio drops slightly as threads increase (more block boundaries = less context per block); the ratio at `t=1` matches or beats the CLI on every workload. For inputs with actual repetition `dedup=True` closes the remaining gap.

ARM / Apple Silicon wheels disable the x86-only JIT and AVX2 flags but still benefit from threading, libsais, and the fast decompress path.

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
    hints=False,             # If True, scan input for text/exe signatures + order-1
                             # redundancy, pass them to libzpaq via the method string.
                             # Slight overhead, can help ratio on mixed/binary data.
    verify=False,            # If True, compute & embed SHA-1 per segment.
                             # zpaq.exe also writes these by default; turning them off
                             # makes both us and zpaq.exe skip verification on extract.
    method=None,             # Optional raw libzpaq method-string override (e.g.
                             # "x4,4,1"). Overrides level/hints when set.
    dedup=False,             # If True, emit a JIDAC-format archive with fragment
                             # dedup. Output is `zpaq x`-extractable. Currently
                             # single-threaded encode; matches CLI ratio on repetitive
                             # inputs.
) -> bytes

zpaq.decompress(
    data,                    # bytes-like ZPAQ stream
    verify=False,            # If True, recompute SHA-1 of each segment and compare
                             # to the one in the archive. Raises zpaq.Error on
                             # mismatch.
    threads=0,               # 0 (default) = auto-detect, clamped by block count.
                             # 1 = single-thread.
) -> bytes

zpaq.Error                   # Raised on libzpaq failures (corrupt stream, bad header)
```

Both `compress` and `decompress` release the GIL while libzpaq runs, so this plays well with threaded workloads.

CLI interoperability
---

`zpaq.compress()` emits the same on-disk format `libzpaq` itself writes, and `zpaq.decompress()` understands archives produced by `zpaq a` (filters out the JIDAC index/hash/info segments, follows the file's fragment-ID list, etc.). Tested on ten varied real files (1 KB to 25 MB, text/image/csv/jar/png/jpg/svg/exe/binary, levels 1-5):

| Direction | Result |
| --- | --- |
| `zpaq.compress` → `zpaq.decompress` | 10 / 10 byte-exact |
| `zpaq.compress` → `zpaq x` (official CLI) | 10 / 10 byte-exact |
| `zpaq a` (official CLI) → `zpaq.decompress` | 10 / 10 byte-exact |

```py
import zpaq

# Pipe to the official CLI
with open("out.zpaq", "wb") as f:
    f.write(zpaq.compress(my_bytes, level=5))
# ... later, from any machine with the zpaq executable installed:
#   $ zpaq x out.zpaq

# Read an archive someone else produced with `zpaq a`
with open("their.zpaq", "rb") as f:
    file_bytes = zpaq.decompress(f.read())
```

For multi-file `zpaq a` archives, `zpaq.decompress` currently returns the concatenated bytes of every file in storage order. A per-segment iterator API for addressing files by name is on the v0.3 list.

Future work
---

Plenty of levers I haven't pulled yet, PRs welcome:

- **PGO** (profile-guided optimization). Adding `/GENPROFILE` + `/USEPROFILE` to the MSVC build (and the gcc/clang equivalent) usually adds another 5-15%. Skipped here because cibuildwheel doesn't expose a clean two-stage build hook yet.
- **AVX2 in the JIT predictor.** AVX2 is on at the C++ compile-flag level. The JIT-emitted predictor already uses SSE2 SIMD (`pmaddwd` / `paddd` for the MIX dot-product, lines 4126-4170 in vendored libzpaq.cpp). Upgrading the JIT codegen to AVX2 256-bit ymm registers would handle 16 mixer inputs per iteration instead of 8, but real gain depends heavily on the per-method MIX `m` parameter, and the work is byte-level instruction re-encoding which is fragile. Skipped for now.
- **Parallel JIDAC encode.** `dedup=True` is currently single-threaded; splitting the fragment-build pass across cores would speed up large dedup compresses.
- **Per-segment archive API.** As mentioned above, let callers address individual files inside multi-file `zpaq a` archives by name.

License
---

Released under the same terms as the underlying `libzpaq` sources: public domain. See `src/zpaq/vendor/COPYING`.

The vendored libsais suffix-array library is Apache 2.0 (Ilya Grebnov). See `src/zpaq/vendor/LICENSE-libsais`.

---

Not affiliated with Matt Mahoney. `libzpaq` was released into the public domain by its original author; this Python package wraps those sources and is an independent community project.
